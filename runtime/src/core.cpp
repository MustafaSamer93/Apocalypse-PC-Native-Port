// Memory map, scheduler, interrupt controller, COP0 and indirect dispatch.
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "psx.h"

uint8_t psx_ram[PSX_RAM_SIZE];
uint8_t psx_scratch[PSX_SCRATCH_SIZE];
int psx_cache_isolated = 0;
R3000* g_cpu = nullptr;

// ================================================================ logging

static uint32_t g_log_mask = 0;
static const char* kCatNames[] = {"core", "bios", "mem", "gpu", "cd", "spu", "dma", "irq", "pad", "mdec", "host"};

bool log_enabled(LogCat cat) { return (g_log_mask >> (int)cat) & 1; }

void log_enable(const char* list) {
    std::string s(list);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string name = s.substr(pos, end - pos);
        if (name == "all") g_log_mask = ~0u;
        for (int i = 0; i < (int)(sizeof kCatNames / sizeof *kCatNames); i++)
            if (name == kCatNames[i]) g_log_mask |= 1u << i;
        pos = end + 1;
    }
}

void log_msg(LogCat cat, const char* fmt, ...) {
    std::fprintf(stderr, "[%s] ", kCatNames[(int)cat]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

static char g_trace[128][160];
static unsigned g_trace_pos = 0;

void trace(const char* fmt, ...) {
    char* slot = g_trace[g_trace_pos++ & 127];
    int n = std::snprintf(slot, 32, "%llu ", g_cpu ? (unsigned long long)g_cpu->cycles : 0ull);
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(slot + n, 160 - n, fmt, ap);
    va_end(ap);
}

void trace_dump() {
    std::fprintf(stderr, "--- recent events ---\n");
    for (unsigned i = 0; i < 128; i++) {
        const char* s = g_trace[(g_trace_pos + i) & 127];
        if (*s) std::fprintf(stderr, "  %s\n", s);
    }
}

void fatal(const char* fmt, ...) {
    std::fprintf(stderr, "FATAL: ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    if (g_cpu) {
        std::fprintf(stderr, "  cycles=%llu ra=%08X sp=%08X\n", (unsigned long long)g_cpu->cycles,
                     g_cpu->r[31], g_cpu->r[29]);
    }
    trace_dump();
    std::fflush(stderr);
    std::abort();
}

// ================================================================ scheduler

static uint64_t g_ev_time[(int)Ev::Count];
static bool g_ev_active[(int)Ev::Count];
static bool g_force_check = false;

static void sched_update(R3000* c) {
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < (int)Ev::Count; i++)
        if (g_ev_active[i]) next = std::min(next, g_ev_time[i]);
    c->next_event = g_force_check ? 0 : next;
}

void sched_init(R3000* c) {
    for (auto& a : g_ev_active) a = false;
    sched_update(c);
}

void sched_add(R3000* c, Ev ev, uint64_t cycles_from_now) {
    g_ev_time[(int)ev] = c->cycles + cycles_from_now;
    g_ev_active[(int)ev] = true;
    sched_update(c);
}

// For periodic events: the next occurrence counted from when this one was due, so the
// lateness of each check point (events are only noticed at branches and calls) does not
// accumulate into a slower clock.
static void sched_next_period(R3000* c, Ev ev, uint64_t period) {
    g_ev_time[(int)ev] += period;
    g_ev_active[(int)ev] = true;
    sched_update(c);
}

void sched_cancel(R3000* c, Ev ev) {
    g_ev_active[(int)ev] = false;
    sched_update(c);
}

bool sched_pending(Ev ev) { return g_ev_active[(int)ev]; }

void sched_request_check(R3000* c) {
    g_force_check = true;
    c->next_event = 0;
}

static void run_event(R3000* c, Ev ev);

static int g_events_depth = 0;

void psx_run_events(R3000* c) {
    // Debug aid: remember recent (cycles, sp, ra) and dump them if sp leaves RAM.
    struct SpSample { uint64_t cyc; uint32_t sp, ra; };
    static SpSample hist[64];
    static unsigned hist_pos = 0;
    static bool sp_reported = false;
    hist[hist_pos++ & 63] = {c->cycles, c->r[29], c->r[31]};
    if (!sp_reported && ((c->r[29] & 0x1FFFFFFF) >= 0x800000 || c->r[29] < 0x80000000u)) {
        sp_reported = true;
        log_msg(LogCat::Core, "stack pointer out of range: sp=%08X ra=%08X cycles=%llu", c->r[29], c->r[31],
                (unsigned long long)c->cycles);
        for (unsigned i = 48; i < 64; i++) {
            const SpSample& s = hist[(hist_pos + i) & 63];
            log_msg(LogCat::Core, "  history: cycles=%llu sp=%08X ra=%08X", (unsigned long long)s.cyc, s.sp, s.ra);
        }
        trace_dump();
    }
    g_force_check = false;
    g_events_depth++;
    for (;;) {
        int due = -1;
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < (int)Ev::Count; i++) {
            if (g_ev_active[i] && g_ev_time[i] <= c->cycles && g_ev_time[i] < best) {
                best = g_ev_time[i];
                due = i;
            }
        }
        if (due < 0) break;
        g_ev_active[due] = false;
        run_event(c, (Ev)due);
    }
    sched_update(c);
    // Interrupts are only delivered from the outermost check point, never while the
    // runtime is itself inside event processing.
    if (g_events_depth == 1 && irq_should_take(c)) bios_take_interrupt(c);
    g_events_depth--;
    sched_update(c);
}

// ================================================================ video timing

static uint32_t g_scanline = 0;
constexpr uint32_t VBLANK_START_LINE = 240;

uint64_t g_dump_ram_at = 0;
std::string g_dump_ram_path;

static void scanline_event(R3000* c) {
    if (g_dump_ram_at && c->cycles >= g_dump_ram_at) {
        g_dump_ram_at = 0;
        if (FILE* f = std::fopen(g_dump_ram_path.c_str(), "wb")) {
            std::fwrite(psx_ram, 1, PSX_RAM_SIZE, f);
            std::fclose(f);
            log_msg(LogCat::Core, "RAM dumped to %s at cycle %llu", g_dump_ram_path.c_str(),
                    (unsigned long long)c->cycles);
        }
    }
    g_scanline++;
    if (g_scanline >= SCANLINES_NTSC) g_scanline = 0;
    bool vbl_start = g_scanline == VBLANK_START_LINE;
    bool vbl_end = g_scanline == 0;
    timers_scanline(c, vbl_start, vbl_end);
    spu_scanline();
    if (vbl_start) {
        irq_raise(c, IRQ_VBLANK);
        gpu_vblank();
        bios_vblank(c);
        spu_end_frame(c);
        host_poll();
        host_present(gpu_display());
        host_throttle_frame();
        if (host_quit_requested()) std::exit(0);
    }
    sched_next_period(c, Ev::Scanline, CYCLES_PER_SCANLINE_NTSC);
}

uint32_t psx_current_scanline() { return g_scanline; }

static void run_event(R3000* c, Ev ev) {
    switch (ev) {
        case Ev::Scanline: scanline_event(c); break;
        case Ev::Cdrom: cdrom_event(c); break;
        case Ev::CdromRead: cdrom_read_event(c); break;
        case Ev::DmaGpu: dma_event(c, 2); break;
        case Ev::DmaCd: dma_event(c, 3); break;
        case Ev::DmaSpu: dma_event(c, 4); break;
        case Ev::DmaMdec: dma_event(c, 1); break;
        case Ev::Sio: sio_event(c); break;
        default: break;
    }
}

// ================================================================ interrupt controller

static uint32_t g_istat = 0, g_imask = 0;

void irq_raise(R3000* c, uint32_t bits) {
    g_istat |= bits;
    LOG(Irq, "raise %03X stat=%03X mask=%03X", bits, g_istat, g_imask);
    if (irq_should_take(c)) sched_request_check(c);
}

uint32_t irq_read(int reg) { return reg == 0 ? g_istat : g_imask; }

void irq_write(R3000* c, int reg, uint32_t v) {
    if (reg == 0) g_istat &= v & 0x7FF;  // write 0 to acknowledge
    else g_imask = v & 0x7FF;
    if (irq_should_take(c)) sched_request_check(c);
}

bool irq_should_take(R3000* c) {
    uint32_t sr = c->cop0[12];
    // IEc (bit 0) and IM2 (hardware interrupt line, bit 10)
    return (g_istat & g_imask) && (sr & 1) && (sr & 0x400);
}

// ================================================================ COP0

uint32_t cop0_read(R3000* c, int reg) {
    if (reg == 13) {  // CAUSE: reflect the hardware interrupt line in IP2
        uint32_t cause = c->cop0[13] & ~0x400u;
        if (g_istat & g_imask) cause |= 0x400;
        return cause;
    }
    return c->cop0[reg];
}

void cop0_write(R3000* c, int reg, uint32_t v) {
    c->cop0[reg] = v;
    if (reg == 12) {
        psx_cache_isolated = (v >> 16) & 1;
        if (irq_should_take(c)) sched_request_check(c);
    }
}

void cop0_rfe(R3000* c) {
    uint32_t sr = c->cop0[12];
    c->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xF);
    if (irq_should_take(c)) sched_request_check(c);
}

void psx_syscall(R3000* c, uint32_t pc) {
    // The only syscalls PsyQ uses: 1 = EnterCriticalSection, 2 = ExitCriticalSection.
    uint32_t fn = c->r[4];
    uint32_t& sr = c->cop0[12];
    switch (fn) {
        case 1: {
            bool was = (sr & 0x401) == 0x401;
            sr &= ~0x401u;
            c->r[2] = was ? 1 : 0;
            break;
        }
        case 2:
            sr |= 0x401;
            if (irq_should_take(c)) sched_request_check(c);
            break;
        default:
            LOG(Core, "syscall(%u) at %08X ignored", fn, pc);
            break;
    }
}

void psx_break(R3000* c, uint32_t pc, uint32_t code) {
    // GCC emits break 7 after a divide-by-zero check and break 6 on overflow; the
    // R3000A result is already defined, so just note it.
    LOG(Core, "break %u at %08X (ra=%08X)", code, pc, c->r[31]);
}

// ================================================================ memory slow path

static uint32_t g_memctrl[16];
static uint32_t g_ramsize = 0x00000B88;

// 32-bit registers (IRQ, DMA, timers) may be accessed with lb/lh/sb/sh at any byte
// offset; libcd for example toggles DMA interrupt enables with `sb` to DICR+2.
static uint32_t read_reg32(R3000* c, uint32_t aligned) {
    if (aligned == 0x1F801070) return irq_read(0);
    if (aligned == 0x1F801074) return irq_read(1);
    if (aligned >= 0x1F801080 && aligned < 0x1F801100) return dma_read(aligned);
    return timers_read(c, aligned);
}

static void write_reg32(R3000* c, uint32_t aligned, uint32_t v) {
    if (aligned == 0x1F801070) irq_write(c, 0, v);
    else if (aligned == 0x1F801074) irq_write(c, 1, v);
    else if (aligned >= 0x1F801080 && aligned < 0x1F801100) dma_write(c, aligned, v);
    else timers_write(c, aligned, v);
}

static bool is_reg32(uint32_t p) {
    return (p >= 0x1F801070 && p < 0x1F801078) || (p >= 0x1F801080 && p < 0x1F801130);
}

uint32_t mem_read_slow(R3000* c, uint32_t addr, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (addr >= 0xFFFE0000u) return 0;  // cache control
    if (p >= 0x1F801000 && p < 0x1F801024) return g_memctrl[(p - 0x1F801000) >> 2];
    if (p >= 0x1F801040 && p < 0x1F801060) return sio_read(c, p, size);
    if (p == 0x1F801060) return g_ramsize;
    if (is_reg32(p)) {
        uint32_t v = read_reg32(c, p & ~3u) >> ((p & 3) * 8);
        return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
    }
    if (p >= 0x1F801800 && p < 0x1F801804) {
        uint32_t v = cdrom_read(c, p & 3);
        if (size >= 2) v |= (uint32_t)cdrom_read(c, p & 3) << 8;  // rare; matches FIFO reads
        return v;
    }
    if (p == 0x1F801810) return gpu_read_data(c);
    if (p == 0x1F801814) return gpu_read_stat(c);
    if (p == 0x1F801820 || p == 0x1F801824) return mdec_read(p & 4);
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        uint32_t v = spu_read(p);
        if (size == 4) v |= (uint32_t)spu_read(p + 2) << 16;
        return v;
    }
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFFFFFFu;  // no expansion ROM
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;            // no BIOS ROM
    LOG(Mem, "unhandled read%d %08X", size * 8, addr);
    return 0;
}

void mem_write_slow(R3000* c, uint32_t addr, uint32_t v, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (psx_cache_isolated) return;  // BIOS cache flush pattern; never reached with HLE
    if (addr >= 0xFFFE0000u) return;
    if (PSX_IS_RAM(addr)) {  // only reached when cache isolation toggled mid-path
        std::memcpy(psx_ram + (p & (PSX_RAM_SIZE - 1)), &v, size);
        return;
    }
    if (p >= 0x1F801000 && p < 0x1F801024) { g_memctrl[(p - 0x1F801000) >> 2] = v; return; }
    if (p >= 0x1F801040 && p < 0x1F801060) { sio_write(c, p, v, size); return; }
    if (p == 0x1F801060) { g_ramsize = v; return; }
    if (is_reg32(p)) {
        uint32_t aligned = p & ~3u;
        if (size == 4) {
            write_reg32(c, aligned, v);
            return;
        }
        int sh = (int)(p & 3) * 8;
        uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
        uint32_t cur = read_reg32(c, aligned);
        if (aligned == 0x1F8010F4) cur &= 0x00FFFFFFu;  // don't re-acknowledge DICR flags
        if (aligned == 0x1F801070) cur = 0xFFFFFFFFu;   // I_STAT: untouched bits stay pending
        write_reg32(c, aligned, (cur & ~mask) | ((v << sh) & mask));
        return;
    }
    if (p >= 0x1F801800 && p < 0x1F801804) { cdrom_write(c, p & 3, (uint8_t)v); return; }
    if (p == 0x1F801810) { gpu_write_gp0(c, v); return; }
    if (p == 0x1F801814) { gpu_write_gp1(c, v); return; }
    if (p == 0x1F801820 || p == 0x1F801824) { mdec_write(p & 4, v); return; }
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        spu_write(c, p, (uint16_t)v);
        if (size == 4) spu_write(c, p + 2, (uint16_t)(v >> 16));
        return;
    }
    if (p >= 0x1F802000 && p < 0x1F803000) return;  // expansion 2 (POST / debug UART)
    LOG(Mem, "unhandled write%d %08X = %08X", size * 8, addr, v);
}

uint32_t ram_r32(uint32_t addr) { return MEM_R32(g_cpu, addr); }
void ram_w32(uint32_t addr, uint32_t v) { MEM_W32(g_cpu, addr, v); }

std::string ram_cstr(uint32_t addr, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)MEM_R8(g_cpu, addr + (uint32_t)i);
        if (!ch) break;
        s += ch;
    }
    return s;
}

// ================================================================ dispatch

static std::unordered_map<uint32_t, RecompFuncPtr> g_funcs;

void dispatch_register(const RecompFunc* table, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) g_funcs[table[i].addr] = table[i].fn;
}

void dispatch_unregister_range(uint32_t start, uint32_t end) {
    for (auto it = g_funcs.begin(); it != g_funcs.end();) {
        if (it->first >= start && it->first < end) it = g_funcs.erase(it);
        else ++it;
    }
}

static uint32_t canonical_code_addr(uint32_t addr) {
    // KUSEG/KSEG1 mirrors of RAM execute the same code as KSEG0.
    if (PSX_IS_RAM(addr)) return 0x80000000u | (addr & (PSX_RAM_SIZE - 1));
    return addr;
}

RecompFuncPtr dispatch_lookup(uint32_t addr) {
    auto it = g_funcs.find(canonical_code_addr(addr));
    return it == g_funcs.end() ? nullptr : it->second;
}

int psx_opt_skip_frame_wait = 0;

uint32_t g_trace_call_addr = 0;  // --trace-call: log calls to this function (interp mode)
static int g_trace_call_count = 0;

void call_indirect(R3000* c, uint32_t addr) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (g_trace_call_addr && canonical_code_addr(addr) == g_trace_call_addr && g_trace_call_count < 400) {
        g_trace_call_count++;
        log_msg(LogCat::Core, "call %08X a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X frame=%llu", addr, c->r[4], c->r[5],
                c->r[6], c->r[7], c->r[31], (unsigned long long)host_frame_count());
        for (int a = 0; a < 2; a++) {
            uint32_t p = c->r[4 + a];
            if (!PSX_IS_RAM(p) || p < 0x80000000u) continue;
            log_msg(LogCat::Core, "    [a%d] %08X %08X %08X %08X %08X %08X", a, MEM_R32(c, p), MEM_R32(c, p + 4),
                    MEM_R32(c, p + 8), MEM_R32(c, p + 12), MEM_R32(c, p + 16), MEM_R32(c, p + 20));
        }
    }
    if (p == 0xA0) { bios_call_a0(c); return; }
    if (p == 0xB0) { bios_call_b0(c); return; }
    if (p == 0xC0) { bios_call_c0(c); return; }
    if (RecompFuncPtr fn = dispatch_lookup(addr)) {
        fn(c);
        return;
    }
    interp_run(c, canonical_code_addr(addr));
}

void jump_indirect(R3000* c, uint32_t addr, uint32_t func_ra) {
    // Hand-written library code (e.g. libgte helpers) returns with `jr t6` etc.: a jump
    // to the address this function was called from is a return, whatever the register.
    if (addr == func_ra) return;
    uint32_t p = addr & 0x1FFFFFFF;
    if (p == 0xA0 || p == 0xB0 || p == 0xC0 || dispatch_lookup(addr)) {
        call_indirect(c, addr);
        return;
    }
    interp_run_until(c, canonical_code_addr(addr), func_ra);
}

void psx_bad_jump(R3000* c, uint32_t pc, uint32_t target) {
    fatal("jump table at %08X: target %08X not in table", pc, target);
}

void psx_fell_off(R3000* c, uint32_t pc) {
    fatal("recompiled code fell through past the end of a function at %08X", pc);
}

void psx_ra_mismatch(R3000* c, uint32_t pc, uint32_t expected, uint32_t actual) {
    static std::vector<uint32_t> reported;
    if (std::find(reported.begin(), reported.end(), pc) != reported.end()) return;
    reported.push_back(pc);
    log_msg(LogCat::Core, "jr ra at %08X returns to %08X, but the function was entered with ra=%08X (sp=%08X)",
            pc, actual, expected, c->r[29]);
}

void psx_sp_mismatch(R3000* c, uint32_t pc, uint32_t expected, uint32_t actual) {
    static std::vector<uint32_t> reported;
    if (std::find(reported.begin(), reported.end(), pc) != reported.end()) return;
    reported.push_back(pc);
    log_msg(LogCat::Core, "jr ra at %08X: sp=%08X but the function was entered with sp=%08X (ra=%08X cycles=%llu)",
            pc, actual, expected, c->r[31], (unsigned long long)c->cycles);
}

uint32_t psx_watch_addr = 0;

void psx_watch_hit(R3000* c, uint32_t addr, uint32_t value, int size) {
    log_msg(LogCat::Core, "watch: write%d %08X = %08X  (ra=%08X sp=%08X a0=%08X a1=%08X a2=%08X s0=%08X s1=%08X cycles=%llu)",
            size * 8, addr, value, c->r[31], c->r[29], c->r[4], c->r[5], c->r[6], c->r[16], c->r[17],
            (unsigned long long)c->cycles);
}

// DMA and other runtime-side writes to RAM report through here too.
void psx_watch_dma(uint32_t phys, uint32_t len, const char* who) {
    if (!psx_watch_addr) return;
    if (psx_watch_addr >= (phys & 0x1FFFFC) && psx_watch_addr < (phys & 0x1FFFFC) + len)
        log_msg(LogCat::Core, "watch: %s wrote %08X..%08X (cycles=%llu)", who, phys, phys + len,
                (unsigned long long)g_cpu->cycles);
}

// ================================================================ save states

#include "state_io.h"

void core_state(StateIO& s) {
    s.marker(0x434F5245);  // "CORE"
    s.raw(psx_ram, sizeof psx_ram);
    s.raw(psx_scratch, sizeof psx_scratch);
    s(psx_cache_isolated);
    s.raw(g_ev_time, sizeof g_ev_time);
    s.raw(g_ev_active, sizeof g_ev_active);
    s(g_force_check);
    s(g_scanline);
    s(g_istat);
    s(g_imask);
    s.raw(g_memctrl, sizeof g_memctrl);
    s(g_ramsize);
    // a load longjmps out of whatever native frames were active, including any
    // psx_run_events that was mid-flight: its depth counter must start over
    if (s.loading()) g_events_depth = 0;
}

void sched_refresh(R3000* c) { sched_update(c); }
