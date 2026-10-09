// High-level emulation of the PS1 kernel (A0/B0/C0 function tables and the exception
// handler). Only what Apocalypse and its PsyQ libraries call is implemented; anything
// else is reported loudly so it can be added.
#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "memcard.h"
#include "psx.h"
#include "savestate.h"

namespace {

// ---------------------------------------------------------------- helpers

uint32_t& A(R3000* c, int n) { return c->r[4 + n]; }  // a0..a3
void ret(R3000* c, uint32_t v) { c->r[2] = v; }

uint32_t stack_arg(R3000* c, int n) {  // n >= 4: arguments passed on the stack
    return MEM_R32(c, c->r[29] + 4u * (uint32_t)n);
}

// ---------------------------------------------------------------- events

constexpr uint32_t EV_DISABLED = 0x1000, EV_ENABLED = 0x2000, EV_READY = 0x4000;
constexpr uint32_t EVMODE_CALLBACK = 0x1000;  // 0x2000 = mark ready (the default path)

struct Event {
    bool used = false;
    uint32_t cls = 0, spec = 0, mode = 0, func = 0;
    uint32_t status = 0;
};
Event g_events[32];

uint32_t event_handle(int i) { return 0xF1000000u | (uint32_t)i; }
Event* event_from_handle(uint32_t h) {
    uint32_t i = h & 0xFFFF;
    if ((h & 0xFFFF0000u) != 0xF1000000u || i >= 32 || !g_events[i].used) return nullptr;
    return &g_events[i];
}

// ---------------------------------------------------------------- exceptions

uint32_t g_hook_buf = 0;        // HookEntryInt jmp_buf address in PSX RAM
uint32_t g_int_chains[4] = {};  // SysEnqIntRP priority chains (PSX addresses)
bool g_in_exception = false;
std::jmp_buf g_exc_jmp;

// ---------------------------------------------------------------- pads

uint32_t g_pad_buf[2] = {}, g_pad_len[2] = {};
bool g_pad_started = false;
uint32_t g_pad_legacy_dest = 0;  // OutdatedPadInitAndStart destination

// ---------------------------------------------------------------- memory card

constexpr uint32_t SWCARD = 0xF4000001u, HWCARD = 0xF0000011u;
// card result specs (EvSpNEW, 0x2000, is never reported: cards always look already seen)
constexpr uint32_t SPEC_IOE = 0x0004, SPEC_ERROR = 0x8000, SPEC_TIMEOUT = 0x0100;
struct PendingEvent { uint32_t cls, spec; };
std::deque<PendingEvent> g_card_events;  // delivered at the next VBlank, like real card I/O

struct OpenFile {
    bool used = false;
    int port = 0;
    MemoryCard::FileInfo info;
    uint32_t pos = 0;
    bool async = false;
};
OpenFile g_files[16];

// DIRENTRY search state for firstfile/nextfile
std::vector<MemoryCard::FileInfo> g_dir_list;
size_t g_dir_index = 0;
std::string g_dir_pattern;
int g_dir_port = 0;

uint32_t g_rcnt_clear[4] = {1, 1, 1, 1};
uint32_t g_rand_seed = 0x24040001;
uint32_t g_heap_base = 0, g_heap_size = 0;

void queue_card_result(uint32_t spec) {
    g_card_events.push_back({SWCARD, spec});
    g_card_events.push_back({HWCARD, spec});
}

bool parse_card_device(const std::string& name, int* port, std::string* file) {
    // "bu00:NAME" (port 0) / "bu10:NAME" (port 1)
    if (name.size() < 5 || name.compare(0, 2, "bu") != 0 || name[4] != ':') return false;
    *port = name[2] == '1' ? 1 : 0;
    if (file) *file = name.substr(5);
    return true;
}

bool wildcard_match(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') return true;  // PS1 BIOS: '*' matches the rest of the name
    if (!*s) return false;
    if (*pat == '?' || *pat == *s) return wildcard_match(pat + 1, s + 1);
    return false;
}

// DIRENTRY { char name[20]; long attr; long size; DIRENTRY* next; long head; char system[4]; }
void write_direntry(R3000* c, uint32_t addr, const MemoryCard::FileInfo& f) {
    for (int i = 0; i < 20; i++) MEM_W8(c, addr + i, i < (int)f.name.size() ? (uint8_t)f.name[i] : 0);
    MEM_W32(c, addr + 20, 0x50);  // attr: first block
    MEM_W32(c, addr + 24, f.size);
    MEM_W32(c, addr + 28, 0);
    MEM_W32(c, addr + 32, (uint32_t)f.first_block);
    MEM_W32(c, addr + 36, 0);
}

uint32_t dir_next(R3000* c, uint32_t dest) {
    while (g_dir_index < g_dir_list.size()) {
        const auto& f = g_dir_list[g_dir_index++];
        if (wildcard_match(g_dir_pattern.c_str(), f.name.c_str())) {
            write_direntry(c, dest, f);
            return dest;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- printf

std::string format_printf(R3000* c, const std::string& fmt, int first_arg) {
    int argn = first_arg;
    auto next = [&]() -> uint32_t {
        uint32_t v = argn < 4 ? c->r[4 + argn] : stack_arg(c, argn);
        argn++;
        return v;
    };
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && std::strchr("-+ #0123456789.lh", fmt[i])) {
            if (fmt[i] != 'l' && fmt[i] != 'h') spec += fmt[i];
            i++;
        }
        if (i >= fmt.size()) break;
        char conv = fmt[i];
        char buf[512];
        switch (conv) {
            case 'd': case 'i': spec += 'd'; std::snprintf(buf, sizeof buf, spec.c_str(), (int32_t)next()); out += buf; break;
            case 'u': case 'x': case 'X': case 'o': case 'c':
                spec += conv; std::snprintf(buf, sizeof buf, spec.c_str(), next()); out += buf; break;
            case 'p': std::snprintf(buf, sizeof buf, "%08X", next()); out += buf; break;
            case 's': spec += 's'; std::snprintf(buf, sizeof buf, spec.c_str(), ram_cstr(next(), 400).c_str()); out += buf; break;
            case '%': out += '%'; break;
            default: out += spec; out += conv; break;
        }
    }
    return out;
}

void bios_puts(const std::string& s) {
    std::fputs(s.c_str(), stdout);
    std::fflush(stdout);
    state_note_file_loaded(s);  // the game prints each file name as it loads it
    LOG(Host, "[frame %llu] game says: %s", (unsigned long long)host_frame_count(), s.c_str());
}

[[noreturn]] void unimplemented(char table, uint32_t fn, R3000* c) {
    fatal("BIOS %c0:%02X not implemented (ra=%08X a0=%08X a1=%08X)", table, fn, c->r[31], c->r[4], c->r[5]);
}

}  // namespace

// ================================================================ init

void bios_init(R3000* c) {
    std::memset(g_events, 0, sizeof g_events);
    g_hook_buf = 0;
    for (auto& ch : g_int_chains) ch = 0;
    // CU2 (GTE usable) + IEc + IM2; I_MASK is 0 so nothing fires yet.
    c->cop0[12] = 0x40000401;
    c->cop0[15] = 0x00000002;  // PRId: R3000A
    // Kernel vectors: jr to the HLE entry points so any code that jumps to them works.
    auto stub = [&](uint32_t addr) {
        MEM_W32(c, addr, 0x03E00008);  // jr ra (never executed natively)
        MEM_W32(c, addr + 4, 0);
    };
    stub(0x800000A0);
    stub(0x800000B0);
    stub(0x800000C0);
}

// ================================================================ A0

void bios_call_a0(R3000* c) {
    uint32_t fn = c->r[9];
    LOG(Bios, "A0:%02X a0=%08X a1=%08X a2=%08X ra=%08X", fn, A(c, 0), A(c, 1), A(c, 2), c->r[31]);
    trace("A0:%02X a0=%08X a1=%08X ra=%08X sp=%08X", fn, A(c, 0), A(c, 1), c->r[31], c->r[29]);
    switch (fn) {
        case 0x13: {  // setjmp(buf): ra, sp, fp, s0-s7, gp
            uint32_t b = A(c, 0);
            MEM_W32(c, b + 0, c->r[31]);
            MEM_W32(c, b + 4, c->r[29]);
            MEM_W32(c, b + 8, c->r[30]);
            for (int i = 0; i < 8; i++) MEM_W32(c, b + 12 + 4 * i, c->r[16 + i]);
            MEM_W32(c, b + 44, c->r[28]);
            ret(c, 0);
            return;
        }
        case 0x17: ret(c, (uint32_t)std::strcmp(ram_cstr(A(c, 0), 4096).c_str(), ram_cstr(A(c, 1), 4096).c_str())); return;
        case 0x18: ret(c, (uint32_t)std::strncmp(ram_cstr(A(c, 0), 4096).c_str(), ram_cstr(A(c, 1), 4096).c_str(), A(c, 2))); return;
        case 0x19: {  // strcpy
            uint32_t d = A(c, 0), s = A(c, 1);
            if (!d || !s) { ret(c, 0); return; }
            uint8_t ch;
            do { ch = (uint8_t)MEM_R8(c, s++); MEM_W8(c, d++, ch); } while (ch);
            ret(c, A(c, 0));
            return;
        }
        case 0x1B: ret(c, A(c, 0) ? (uint32_t)ram_cstr(A(c, 0), 1 << 20).size() : 0); return;
        case 0x28: {  // bzero
            for (uint32_t i = 0; i < A(c, 1); i++) MEM_W8(c, A(c, 0) + i, 0);
            ret(c, A(c, 0));
            return;
        }
        case 0x2A: {  // memcpy
            uint32_t d = A(c, 0), s = A(c, 1), n = A(c, 2);
            for (uint32_t i = 0; i < n; i++) MEM_W8(c, d + i, MEM_R8(c, s + i));
            ret(c, d);
            return;
        }
        case 0x2B: {  // memset
            for (uint32_t i = 0; i < A(c, 2); i++) MEM_W8(c, A(c, 0) + i, A(c, 1));
            ret(c, A(c, 0));
            return;
        }
        case 0x2E: {  // memchr
            for (uint32_t i = 0; i < A(c, 2); i++)
                if ((uint8_t)MEM_R8(c, A(c, 0) + i) == (uint8_t)A(c, 1)) { ret(c, A(c, 0) + i); return; }
            ret(c, 0);
            return;
        }
        case 0x2F:  // rand: the BIOS LCG
            g_rand_seed = g_rand_seed * 1103515245u + 12345u;
            ret(c, (g_rand_seed >> 16) & 0x7FFF);
            return;
        case 0x30: g_rand_seed = A(c, 0); return;  // srand
        case 0x39: g_heap_base = A(c, 0); g_heap_size = A(c, 1); return;  // InitHeap
        case 0x3F: bios_puts(format_printf(c, ram_cstr(A(c, 0), 1024), 1)); return;
        case 0x44: return;  // FlushCache: no instruction cache to flush
        case 0x49: gpu_write_gp1(c, 0x01000000); gpu_write_gp0(c, A(c, 0)); return;  // GPU_cw
        case 0x70: return;  // _bu_init
        case 0x72: return;  // CdRemove
        case 0xAB:          // _card_info(port)
        case 0xAC: {        // _card_load(port)
            int port = (A(c, 0) >> 4) & 1;
            queue_card_result(memcard(port).present() ? SPEC_IOE : SPEC_TIMEOUT);
            ret(c, 1);
            return;
        }
        default: unimplemented('A', fn, c);
    }
}

// ================================================================ B0

void bios_call_b0(R3000* c) {
    uint32_t fn = c->r[9];
    if (fn != 0x0B && fn != 0x17)  // TestEvent is polled constantly
        LOG(Bios, "B0:%02X a0=%08X a1=%08X a2=%08X ra=%08X", fn, A(c, 0), A(c, 1), A(c, 2), c->r[31]);
    if (fn != 0x0B) trace("B0:%02X a0=%08X a1=%08X ra=%08X sp=%08X", fn, A(c, 0), A(c, 1), c->r[31], c->r[29]);
    switch (fn) {
        case 0x07: bios_deliver_event(c, A(c, 0), A(c, 1)); ret(c, 0); return;
        case 0x08: {  // OpenEvent(class, spec, mode, func)
            for (int i = 0; i < 32; i++) {
                if (!g_events[i].used) {
                    g_events[i] = {true, A(c, 0), A(c, 1), A(c, 2), A(c, 3), EV_DISABLED};
                    ret(c, event_handle(i));
                    return;
                }
            }
            ret(c, 0xFFFFFFFF);
            return;
        }
        case 0x09: if (Event* e = event_from_handle(A(c, 0))) e->used = false; ret(c, 1); return;
        case 0x0A: {  // WaitEvent: block until ready, running emulated time forward
            Event* e = event_from_handle(A(c, 0));
            if (!e || e->status == EV_DISABLED) { ret(c, 0); return; }
            while (e->status != EV_READY) {
                c->cycles = c->next_event;
                psx_run_events(c);
            }
            e->status = EV_ENABLED;
            ret(c, 1);
            return;
        }
        case 0x0B: {  // TestEvent
            Event* e = event_from_handle(A(c, 0));
            if (e && e->status == EV_READY) { e->status = EV_ENABLED; ret(c, 1); }
            else ret(c, 0);
            return;
        }
        case 0x0C: if (Event* e = event_from_handle(A(c, 0))) e->status = EV_ENABLED; ret(c, 1); return;
        case 0x0D: if (Event* e = event_from_handle(A(c, 0))) e->status = EV_DISABLED; ret(c, 1); return;
        case 0x12:  // InitPad(buf1, siz1, buf2, siz2)
            g_pad_buf[0] = A(c, 0); g_pad_len[0] = A(c, 1);
            g_pad_buf[1] = A(c, 2); g_pad_len[1] = A(c, 3);
            ret(c, 2);
            return;
        case 0x13: g_pad_started = true; ret(c, 1); return;   // StartPad
        case 0x14: g_pad_started = false; ret(c, 1); return;  // StopPad
        case 0x15:  // OutdatedPadInitAndStart(type, dest, ...)
            g_pad_legacy_dest = A(c, 1);
            g_pad_started = true;
            ret(c, 2);
            return;
        case 0x17: {  // ReturnFromException
            if (!g_in_exception) fatal("ReturnFromException outside an exception (ra=%08X)", c->r[31]);
            std::longjmp(g_exc_jmp, 1);
        }
        case 0x18: g_hook_buf = 0; return;             // ResetEntryInt
        case 0x19: g_hook_buf = A(c, 0); return;       // HookEntryInt
        case 0x32: {  // open(name, mode)
            std::string path = ram_cstr(A(c, 0));
            uint32_t mode = A(c, 1);
            int port;
            std::string name;
            if (!parse_card_device(path, &port, &name)) {
                LOG(Bios, "open(%s): only memory card files are supported", path.c_str());
                ret(c, 0xFFFFFFFF);
                return;
            }
            MemoryCard& mc = memcard(port);
            MemoryCard::FileInfo info;
            bool ok = mc.present() && ((mode & 0x200) ? mc.create(name, (int)(mode >> 16), &info) : mc.find(name, &info));
            if (!ok) { ret(c, 0xFFFFFFFF); return; }
            if (mode & 0x200) mc.flush();
            for (int fd = 2; fd < 16; fd++) {
                if (!g_files[fd].used) {
                    g_files[fd] = {true, port, info, 0, (mode & 0x8000) != 0};
                    ret(c, (uint32_t)fd);
                    return;
                }
            }
            ret(c, 0xFFFFFFFF);
            return;
        }
        case 0x33: {  // lseek(fd, offset, whence)
            uint32_t fd = A(c, 0);
            if (fd >= 16 || !g_files[fd].used) { ret(c, 0xFFFFFFFF); return; }
            OpenFile& f = g_files[fd];
            f.pos = A(c, 2) == 1 ? f.pos + A(c, 1) : A(c, 1);
            ret(c, f.pos);
            return;
        }
        case 0x34:    // read(fd, dst, len)
        case 0x35: {  // write(fd, src, len)
            uint32_t fd = A(c, 0), buf = A(c, 1), len = A(c, 2);
            if (fd == 1 && fn == 0x35) {  // stdout
                std::string s;
                for (uint32_t i = 0; i < len; i++) s += (char)MEM_R8(c, buf + i);
                bios_puts(s);
                ret(c, len);
                return;
            }
            if (fd >= 16 || !g_files[fd].used) { ret(c, 0xFFFFFFFF); return; }
            OpenFile& f = g_files[fd];
            MemoryCard& mc = memcard(f.port);
            uint32_t n = 0;
            for (; n < len && f.pos < f.info.size; n++, f.pos++) {
                int off = mc.file_offset(f.info, f.pos);
                if (off < 0) break;
                if (fn == 0x34) MEM_W8(c, buf + n, mc.data()[off]);
                else mc.data()[off] = (uint8_t)MEM_R8(c, buf + n);
            }
            if (fn == 0x35) { mc.mark_dirty(); mc.flush(); }
            if (f.async) queue_card_result(SPEC_IOE);
            ret(c, n);
            return;
        }
        case 0x36: {  // close(fd)
            uint32_t fd = A(c, 0);
            if (fd < 16 && g_files[fd].used) { g_files[fd].used = false; ret(c, fd); }
            else ret(c, 0xFFFFFFFF);
            return;
        }
        case 0x3F: bios_puts(ram_cstr(A(c, 0), 1024) + "\n"); return;  // puts
        case 0x41: {  // format(dev)
            int port;
            if (parse_card_device(ram_cstr(A(c, 0)), &port, nullptr) && memcard(port).present()) {
                memcard(port).format();
                memcard(port).flush();
                ret(c, 1);
            } else {
                ret(c, 0);
            }
            return;
        }
        case 0x42: {  // firstfile(pattern, direntry)
            std::string pat = ram_cstr(A(c, 0));
            std::string name;
            if (!parse_card_device(pat, &g_dir_port, &name) || !memcard(g_dir_port).present()) { ret(c, 0); return; }
            g_dir_pattern = name.empty() ? "*" : name;
            g_dir_list = memcard(g_dir_port).list();
            g_dir_index = 0;
            ret(c, dir_next(c, A(c, 1)));
            return;
        }
        case 0x43: ret(c, dir_next(c, A(c, 0))); return;  // nextfile
        case 0x45: {  // delete(name)
            int port;
            std::string name;
            bool ok = parse_card_device(ram_cstr(A(c, 0)), &port, &name) && memcard(port).remove(name);
            if (ok) memcard(port).flush();
            ret(c, ok ? 1 : 0);
            return;
        }
        case 0x4A: ret(c, 1); return;  // InitCard(pad_enable)
        case 0x4B: ret(c, 1); return;  // StartCard
        case 0x4C: ret(c, 1); return;  // StopCard
        case 0x4E: {  // _card_write(port, sector, src)
            int port = (A(c, 0) >> 4) & 1;
            uint8_t frame[128];
            for (int i = 0; i < 128; i++) frame[i] = (uint8_t)MEM_R8(c, A(c, 2) + i);
            bool ok = memcard(port).present() && memcard(port).write_frame((int)A(c, 1), frame);
            if (ok) memcard(port).flush();
            queue_card_result(ok ? SPEC_IOE : SPEC_ERROR);
            ret(c, 1);
            return;
        }
        case 0x4F: {  // _card_read(port, sector, dst)
            int port = (A(c, 0) >> 4) & 1;
            uint8_t frame[128];
            bool ok = memcard(port).present() && memcard(port).read_frame((int)A(c, 1), frame);
            if (ok) for (int i = 0; i < 128; i++) MEM_W8(c, A(c, 2) + i, frame[i]);
            queue_card_result(ok ? SPEC_IOE : SPEC_ERROR);
            ret(c, 1);
            return;
        }
        case 0x50: return;  // _new_card
        // GetC0Table / GetB0Table: libcard's _patch_card follows these to rewrite a few
        // instructions inside the real kernel. With the kernel emulated natively there
        // is nothing to patch, so hand back the real BIOS table addresses in unused low
        // RAM (all zero); the patch writes land there harmlessly.
        case 0x56: ret(c, 0x00000674); return;
        case 0x57: ret(c, 0x00000874); return;
        case 0x5B: ret(c, 0); return;  // ChangeClearPad
        default: unimplemented('B', fn, c);
    }
}

// ================================================================ C0

void bios_call_c0(R3000* c) {
    uint32_t fn = c->r[9];
    LOG(Bios, "C0:%02X a0=%08X a1=%08X ra=%08X", fn, A(c, 0), A(c, 1), c->r[31]);
    trace("C0:%02X a0=%08X a1=%08X ra=%08X sp=%08X", fn, A(c, 0), A(c, 1), c->r[31], c->r[29]);
    switch (fn) {
        case 0x02: {  // SysEnqIntRP(priority, elem): push to the chain head
            uint32_t pri = A(c, 0) & 3, elem = A(c, 1);
            MEM_W32(c, elem, g_int_chains[pri]);
            g_int_chains[pri] = elem;
            ret(c, 0);
            return;
        }
        case 0x03: {  // SysDeqIntRP(priority, elem)
            uint32_t pri = A(c, 0) & 3, elem = A(c, 1);
            uint32_t* link = &g_int_chains[pri];
            uint32_t prev = 0;
            for (uint32_t cur = *link; cur; prev = cur, cur = MEM_R32(c, cur)) {
                if (cur != elem) continue;
                uint32_t next = MEM_R32(c, cur);
                if (prev) MEM_W32(c, prev, next);
                else *link = next;
                break;
            }
            ret(c, 0);
            return;
        }
        case 0x0A: {  // ChangeClearRCnt(t, flag) -> old
            uint32_t t = A(c, 0) & 3, old = g_rcnt_clear[t];
            g_rcnt_clear[t] = A(c, 1);
            ret(c, old);
            return;
        }
        default: unimplemented('C', fn, c);
    }
}

// ================================================================ events / IRQ

void bios_deliver_event(R3000* c, uint32_t cls, uint32_t spec) {
    for (auto& e : g_events) {
        if (!e.used || e.cls != cls || e.spec != spec || e.status != EV_ENABLED) continue;
        if (e.mode == EVMODE_CALLBACK) {
            if (e.func) {
                R3000 saved = *c;
                c->r[31] = 0;
                call_indirect(c, e.func);
                std::memcpy(c->r, saved.r, sizeof c->r);
                c->hi = saved.hi;
                c->lo = saved.lo;
            }
        } else {
            e.status = EV_READY;
        }
    }
}

static void call_psx(R3000* c, uint32_t addr, uint32_t a0) {
    c->r[4] = a0;
    c->r[31] = 0;
    call_indirect(c, addr);
}

bool bios_in_exception() { return g_in_exception; }

void bios_take_interrupt(R3000* c) {
    if (g_in_exception) return;
    R3000 saved = *c;
    g_in_exception = true;
    trace("IRQ enter stat=%03X mask=%03X sp=%08X ra=%08X", irq_read(0), irq_read(1), c->r[29], c->r[31]);
    uint32_t sr = c->cop0[12];
    c->cop0[12] = (sr & ~0x3Fu) | ((sr << 2) & 0x3Fu);  // push KU/IE: interrupts off
    c->cop0[13] = (c->cop0[13] & ~0x7Cu);              // ExcCode = 0 (interrupt)
    c->cop0[14] = 0;                                   // EPC: not meaningful for HLE

    if (setjmp(g_exc_jmp) == 0) {
        // Kernel chains: for each element {next, func2, func1, pad}: v = func1(); if (v) func2(v)
        for (int pri = 0; pri < 4; pri++) {
            for (uint32_t el = g_int_chains[pri]; el; el = MEM_R32(c, el)) {
                uint32_t f2 = MEM_R32(c, el + 4), f1 = MEM_R32(c, el + 8);
                if (!f1) continue;
                c->r[29] = saved.r[29] - 0x100;  // run on the interrupted stack, below its frame
                call_psx(c, f1, 0);
                uint32_t v = c->r[2];
                if (v && f2) call_psx(c, f2, v);
            }
        }
        if (g_hook_buf) {
            // longjmp into the hook: restore callee-saved registers and "return" 1
            uint32_t b = g_hook_buf;
            c->r[29] = MEM_R32(c, b + 4);
            c->r[30] = MEM_R32(c, b + 8);
            for (int i = 0; i < 8; i++) c->r[16 + i] = MEM_R32(c, b + 12 + 4 * i);
            c->r[28] = MEM_R32(c, b + 44);
            c->r[2] = 1;
            uint32_t target = MEM_R32(c, b + 0);
            c->r[31] = 0;
            call_indirect(c, target);
        }
        // handlers normally end in ReturnFromException; returning normally is also fine
    }
    std::memcpy(c->r, saved.r, sizeof c->r);
    c->hi = saved.hi;
    c->lo = saved.lo;
    sr = c->cop0[12];
    c->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);  // pop KU/IE
    g_in_exception = false;
    trace("IRQ exit sp=%08X", c->r[29]);
    if (irq_should_take(c)) sched_request_check(c);
}

// The pads into their BIOS buffers (at VBlank, and again after the host waited for the display).
void bios_pad_refresh(R3000* c) {
    if (g_pad_started) {
        for (int port = 0; port < 2; port++) {
            uint32_t buf = g_pad_buf[port];
            PadState p = host_pad(port);
            if (buf && g_pad_len[port] >= 4) {
                MEM_W8(c, buf + 0, p.connected ? 0x00 : 0xFF);
                MEM_W8(c, buf + 1, 0x41);  // digital pad, 1 halfword of data
                MEM_W8(c, buf + 2, (uint8_t)p.buttons);
                MEM_W8(c, buf + 3, (uint8_t)(p.buttons >> 8));
            }
        }
        if (g_pad_legacy_dest) {
            PadState p = host_pad(0), q = host_pad(1);
            MEM_W32(c, g_pad_legacy_dest, ~(uint32_t)(p.buttons | (uint32_t)q.buttons << 16));
        }
    }
}

void bios_vblank(R3000* c) {
    bios_pad_refresh(c);
    // Card I/O completes one frame after it was requested.
    size_t n = g_card_events.size();
    for (size_t i = 0; i < n; i++) {
        PendingEvent ev = g_card_events.front();
        g_card_events.pop_front();
        bios_deliver_event(c, ev.cls, ev.spec);
    }
}

// ================================================================ save states

#include "state_io.h"

// Kernel state game code can change in passing (sim60.cpp rolls it back after its ghost
// render); open files and directory listings are left alone.
void bios_ghost_state(StateIO& s) {
    s.raw(g_events, sizeof g_events);
    s(g_hook_buf);
    s.raw(g_int_chains, sizeof g_int_chains);
    s.raw(g_pad_buf, sizeof g_pad_buf);
    s.raw(g_pad_len, sizeof g_pad_len);
    s(g_pad_started);
    s(g_pad_legacy_dest);
    s.deq(g_card_events);
    s.raw(g_rcnt_clear, sizeof g_rcnt_clear);
    s(g_rand_seed);
    s(g_heap_base);
    s(g_heap_size);
}

void bios_state(StateIO& s) {
    s.marker(0x42494F53);  // "BIOS"
    s.raw(g_events, sizeof g_events);
    s(g_hook_buf);
    s.raw(g_int_chains, sizeof g_int_chains);
    s.raw(g_pad_buf, sizeof g_pad_buf);
    s.raw(g_pad_len, sizeof g_pad_len);
    s(g_pad_started);
    s(g_pad_legacy_dest);
    s.deq(g_card_events);
    for (auto& f : g_files) {
        s(f.used);
        s(f.port);
        s.str(f.info.name);
        s(f.info.first_block);
        s(f.info.size);
        s(f.pos);
        s(f.async);
    }
    s.raw(g_rcnt_clear, sizeof g_rcnt_clear);
    s(g_rand_seed);
    s(g_heap_base);
    s(g_heap_size);
    if (s.loading()) {
        g_in_exception = false;
        g_dir_list.clear();
        g_dir_index = 0;
    }
}

