// Memory map, scheduler, interrupt controller, COP0 and indirect dispatch.
#include <algorithm>
#include <csetjmp>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "core/psx.h"
#include "core/state_io.h"
#include "host/host.h"

uint8_t psx_ram[PSX_RAM_SIZE];
uint8_t psx_scratch[PSX_SCRATCH_SIZE];
int psx_cache_isolated = 0;
uint32_t psx_cpi_q = 4 * PSX_CYCLES_PER_INSN;
uint32_t psx_cycle_frac = 0;
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
    // time since the start (minutes:seconds), so a tester's report lines up with what they saw
    const uint64_t ms = SDL_GetTicks();
    std::fprintf(stderr, "%3llu:%02llu.%01llu [%s] ", (unsigned long long)(ms / 60000), (unsigned long long)(ms / 1000 % 60),
                 (unsigned long long)(ms / 100 % 10), kCatNames[(int)cat]);
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

// Freeze report: a thread that notices when no frame has come for 8 s (outside the settings and chapter
// menus, which stop the game on purpose) and writes, once per freeze, what the CPU was doing - its
// registers and the return addresses on its stack - plus the RAM to debug/, like a crash.
// (Read from another thread while the game runs: a snapshot, enough to see where it is stuck.)
bool g_host_menu_open = false;
void watchdog_start() {
    std::thread([] {
        uint64_t last = ~0ull;
        int still = 0;
        for (;;) {
            SDL_Delay(1000);
            const uint64_t f = host_frame_count();
            still = (f == last && !g_host_menu_open) ? still + 1 : 0;
            last = f;
            if (still != 8 || !g_cpu) continue;
            const R3000 c = *g_cpu;
            std::fprintf(stderr, "FREEZE: no frame for 8 s at frame %llu, cycles=%llu ra=%08X sp=%08X\n",
                         (unsigned long long)f, (unsigned long long)c.cycles, c.r[31], c.r[29]);
            for (int i = 0; i < 32; i += 8) {
                std::fprintf(stderr, "  r%-2d", i);
                for (int k = i; k < i + 8; k++) std::fprintf(stderr, " %08X", c.r[k]);
                std::fprintf(stderr, "\n");
            }
            std::fprintf(stderr, "  stack:");
            for (uint32_t s = c.r[29] & 0x1FFFFC, n = 0; s < PSX_RAM_SIZE && n < 24; s += 4) {
                uint32_t w;
                std::memcpy(&w, psx_ram + s, 4);
                if (w >= 0x80010000u && w < 0x800A0000u && !(w & 3)) std::fprintf(stderr, " %08X", w), n++;
            }
            std::fprintf(stderr, "\n");
            std::error_code ec;
            std::filesystem::create_directories("debug", ec);
            if (FILE* out = std::fopen("debug/freeze_ram.bin", "wb")) {
                std::fwrite(psx_ram, 1, PSX_RAM_SIZE, out);
                std::fclose(out);
                std::fprintf(stderr, "  RAM saved to debug/freeze_ram.bin\n");
            }
        }
    }).detach();
}

void fatal(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "FATAL: %s\n", msg);
    // The exe has no console window: show the error, so a double-click never fails
    // silently (scripts set APOC_NO_DIALOGS to get only the log).
    if (!std::getenv("APOC_NO_DIALOGS")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Repocalypse", msg, nullptr);
    if (g_cpu) {
        std::fprintf(stderr, "  cycles=%llu ra=%08X sp=%08X\n", (unsigned long long)g_cpu->cycles,
                     g_cpu->r[31], g_cpu->r[29]);
        for (int i = 0; i < 32; i += 8) {
            std::fprintf(stderr, "  r%-2d", i);
            for (int k = i; k < i + 8; k++) std::fprintf(stderr, " %08X", g_cpu->r[k]);
            std::fprintf(stderr, "\n");
        }
        // the whole RAM, for finding what led here (next to the log)
        std::error_code ec;
        std::filesystem::create_directories("debug", ec);
        char name[64];
        const std::time_t now = std::time(nullptr);
        std::strftime(name, sizeof name, "debug/crash_%Y%m%d_%H%M%S_ram.bin", std::localtime(&now));
        if (FILE* f = std::fopen(name, "wb")) {
            std::fwrite(psx_ram, 1, PSX_RAM_SIZE, f);
            std::fclose(f);
            std::fprintf(stderr, "  RAM saved to %s\n", name);
        }
    }
    trace_dump();
    std::fflush(stderr);
    std::abort();
}

// ================================================================ scheduler

static uint64_t g_ev_time[(int)Ev::Count];
static bool g_ev_active[(int)Ev::Count];
static bool g_force_check = false;

// Ghost mode (sim60.cpp): game code runs outside emulated time while the previous pose is
// rendered. No events or interrupts are delivered; reaching the cycle budget aborts it.
static bool g_ghost = false;
static uint64_t g_ghost_deadline = 0;
static std::jmp_buf* g_ghost_abort = nullptr;
static int g_ghost_blocked_io = 0;

static void sched_update(R3000* c) {
    if (g_ghost) {
        c->next_event = g_ghost_deadline;
        return;
    }
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

void sched_request_check(R3000* c) {
    if (g_ghost) return;
    g_force_check = true;
    c->next_event = 0;
}

static void run_event(R3000* c, Ev ev);

static int g_events_depth = 0;

void psx_run_events(R3000* c) {
    if (g_ghost) std::longjmp(*g_ghost_abort, 1);  // over budget: something is waiting on hardware
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

// Presenting (scanline_event): whether this refresh still waits for its picture, and the frame
// shown early (core_frame_drawn) until the game flips to it.
static bool g_pending = false, g_flip_seen = false, g_early_valid = false;
static uint32_t g_lines = 0;
static uint8_t g_flips = 0;  // one bit a frame: the game showed a new frame
static GpuDisplay g_early{};

static void present_frame(R3000* c, const GpuDisplay& d) {
    g_pending = false;
    host_present(d);
    host_throttle_frame();
    host_poll();
    bios_pad_refresh(c);
    if (host_quit_requested()) std::exit(0);
}

// What the screen shows now: the frame shown early while the game has not flipped to it yet.
// gpu.cpp: a picture (a movie frame) was uploaded: a frame shown early is no longer what the
// game will show (the first movie picture went into it while the display was still 15-bit).
void core_early_cancel() { g_early_valid = false; }

static GpuDisplay shown_display() {
    GpuDisplay d = gpu_display();
    if (g_early_valid) d.x = g_early.x, d.y = g_early.y;
    return d;
}

// gpu.cpp: the game drew a frame's ordering table (not in a ghost render). It goes to the screen
// now, instead of a refresh later when the game flips to it: one frame less input lag. Only for
// double-buffered frames the size of the display, one per refresh.
void core_frame_drawn(R3000* c) {
    GpuDisplay d;
    if (!g_pending || !gpu_drawn_display(&d)) return;
    const GpuDisplay now = gpu_display();
    if (d.x == now.x && d.y == now.y) return;  // drawn where the screen is: shown as usual
    g_early = d;
    g_early_valid = true;
    present_frame(c, d);
}

static void scanline_event(R3000* c) {
    g_scanline++;
    if (g_scanline >= SCANLINES_NTSC) g_scanline = 0;
    bool vbl_start = g_scanline == VBLANK_START_LINE;
    bool vbl_end = g_scanline == 0;
    timers_scanline(c, vbl_start, vbl_end);
    spu_scanline();
    // One picture goes to the screen per refresh; the host waits for the display there, and the
    // controller is read after that wait, for the game's next frame. Each frame is shown as soon
    // as the game has drawn it (core_frame_drawn), a refresh before the game itself flips to it.
    // Without that (movies, other screens), the picture goes at the game's flip (it waits for
    // VBlank and then sets the display start, gpu.cpp g_display_flipped). Without a new frame by
    // the end of VBlank: a game drawing every frame (7 of the last 8) is late with this one, which
    // is shown when it comes (or at the next VBlank); otherwise (30 fps, loading) the picture is
    // presented as it is.
    if (vbl_start) {
        irq_raise(c, IRQ_VBLANK);
        gpu_vblank();
        bios_vblank(c);
        spu_end_frame(c);
        if (g_pending) present_frame(c, shown_display());  // no new frame all frame long
        g_flips = (uint8_t)(g_flips << 1 | (g_display_flipped ? 1 : 0));
        g_pending = true, g_lines = 0, g_display_flipped = false, g_flip_seen = false;
    } else if (g_pending) {
        if (g_display_flipped && !g_flip_seen) {
            g_flip_seen = true;
            const GpuDisplay d = gpu_display();
            if (g_early_valid && d.x == g_early.x && d.y == g_early.y) g_early_valid = false;  // shown already
            else present_frame(c, d);
        } else if (!g_display_flipped && ++g_lines == SCANLINES_NTSC - VBLANK_START_LINE - 1 &&
                   __builtin_popcount(g_flips) < 7) {
            present_frame(c, shown_display());
        }
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
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;  // the BIOS ROM (high-level BIOS: none)
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
    if (g_ghost && ((p >= 0x1F801800 && p < 0x1F801804) || p == 0x1F801820 || p == 0x1F801824 ||
                    (p >= 0x1F801C00 && p < 0x1F802000))) {
        g_ghost_blocked_io++;  // sound, CD and movie hardware keep running in real time
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

static uint32_t canonical_code_addr(uint32_t addr) {
    // KUSEG/KSEG1 mirrors of RAM execute the same code as KSEG0.
    if (PSX_IS_RAM(addr)) return 0x80000000u | (addr & (PSX_RAM_SIZE - 1));
    return addr;
}

RecompFuncPtr dispatch_lookup(uint32_t addr) {
    auto it = g_funcs.find(canonical_code_addr(addr));
    return it == g_funcs.end() ? nullptr : it->second;
}

void call_indirect(R3000* c, uint32_t addr) {
    uint32_t p = addr & 0x1FFFFFFF;
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

// ================================================================ save states

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

// ================================================================ ghost mode (sim60.cpp)

struct CoreGhostSnap {
    std::vector<uint8_t> ram;
    uint8_t scratch[PSX_SCRATCH_SIZE];
    uint64_t ev_time[(int)Ev::Count];
    bool ev_active[(int)Ev::Count];
    bool force_check;
    uint32_t istat, imask;
    int cache_isolated;
    // precise 3D's shadows (gte.cpp): a ghost render's stores would otherwise leave its in-between
    // positions behind, taken later wherever the real word has the same whole-pixel value
    std::vector<PgxpVal> pgxp_ram;
    PgxpVal pgxp_scratch[PSX_SCRATCH_SIZE / 4], pgxp_gpr[32], rgb2;
    double mac[4];
    bool mac_ok;
};
static CoreGhostSnap g_snap;

void core_ghost_begin(R3000* c, uint64_t cycle_budget, std::jmp_buf* abort) {
    g_snap.ram.assign(psx_ram, psx_ram + PSX_RAM_SIZE);
    std::memcpy(g_snap.scratch, psx_scratch, sizeof psx_scratch);
    std::memcpy(g_snap.ev_time, g_ev_time, sizeof g_ev_time);
    std::memcpy(g_snap.ev_active, g_ev_active, sizeof g_ev_active);
    g_snap.force_check = g_force_check;
    g_snap.istat = g_istat;
    g_snap.imask = g_imask;
    g_snap.cache_isolated = psx_cache_isolated;
    // ponytail: copies all 8 MB of shadows (~1 ms per ghost render); journal the ghost's shadow stores if that shows up in profiles
    if (g_gte_precise) {
        g_snap.pgxp_ram.assign(pgxp_ram, pgxp_ram + PSX_RAM_SIZE / 4);
        std::memcpy(g_snap.pgxp_scratch, pgxp_scratch, sizeof pgxp_scratch);
        std::memcpy(g_snap.pgxp_gpr, pgxp_gpr, sizeof pgxp_gpr);
        g_snap.rgb2 = g_rgb2_exact;
        std::memcpy(g_snap.mac, g_mac_exact, sizeof g_mac_exact);
        g_snap.mac_ok = g_mac_exact_ok;
    } else {
        g_snap.pgxp_ram.clear();
    }
    g_ghost = true;
    g_ghost_deadline = c->cycles + cycle_budget;
    g_ghost_abort = abort;
    g_ghost_blocked_io = 0;
    sched_update(c);
}

// Puts memory, scheduler and interrupt state back as they were at core_ghost_begin.
int core_ghost_end(R3000* c) {
    std::memcpy(psx_ram, g_snap.ram.data(), PSX_RAM_SIZE);
    std::memcpy(psx_scratch, g_snap.scratch, sizeof psx_scratch);
    std::memcpy(g_ev_time, g_snap.ev_time, sizeof g_ev_time);
    std::memcpy(g_ev_active, g_snap.ev_active, sizeof g_ev_active);
    g_force_check = g_snap.force_check;
    g_istat = g_snap.istat;
    g_imask = g_snap.imask;
    psx_cache_isolated = g_snap.cache_isolated;
    if (!g_snap.pgxp_ram.empty()) {
        std::memcpy(pgxp_ram, g_snap.pgxp_ram.data(), sizeof pgxp_ram);
        std::memcpy(pgxp_scratch, g_snap.pgxp_scratch, sizeof pgxp_scratch);
        std::memcpy(pgxp_gpr, g_snap.pgxp_gpr, sizeof pgxp_gpr);
        g_rgb2_exact = g_snap.rgb2;
        std::memcpy(g_mac_exact, g_snap.mac, sizeof g_mac_exact);
        g_mac_exact_ok = g_snap.mac_ok;
    }
    g_ghost = false;
    g_ghost_abort = nullptr;
    sched_update(c);
    return g_ghost_blocked_io;
}

bool core_in_ghost() { return g_ghost; }
