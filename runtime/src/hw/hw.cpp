// DMA controller, root counters and SIO0 (controller port).
#include <cstring>

#include "core/psx.h"
#include "core/state_io.h"
#include "host/host.h"

// ================================================================ DMA

namespace {

struct DmaChan {
    uint32_t madr = 0, bcr = 0, chcr = 0;
};
DmaChan g_dma[7];
uint32_t g_dpcr = 0x07654321, g_dicr = 0;
bool g_mdec_out_waiting = false;

constexpr uint32_t CHCR_START = 1u << 24, CHCR_TRIGGER = 1u << 28;

void dicr_update(R3000* c) {
    bool force = g_dicr & (1u << 15);
    bool master_en = g_dicr & (1u << 23);
    uint32_t en = (g_dicr >> 16) & 0x7F, flags = (g_dicr >> 24) & 0x7F;
    bool prev = g_dicr & 0x80000000u;
    bool now = force || (master_en && (en & flags));
    if (now) g_dicr |= 0x80000000u;
    else g_dicr &= ~0x80000000u;
    if (now && !prev) irq_raise(c, IRQ_DMA);
}

void dma_finish(R3000* c, int ch) {
    g_dma[ch].chcr &= ~(CHCR_START | CHCR_TRIGGER);
    if (g_dicr & (1u << (16 + ch))) g_dicr |= 1u << (24 + ch);
    dicr_update(c);
}

uint32_t word_count(const DmaChan& d) {
    uint32_t sync = (d.chcr >> 9) & 3;
    if (sync == 0) {
        uint32_t n = d.bcr & 0xFFFF;
        return n ? n : 0x10000;
    }
    return (d.bcr & 0xFFFF) * (d.bcr >> 16);
}

}  // namespace

uint32_t g_gpu_packet_addr = 0;  // RAM address of the packet being sent (0 = not from a list)
uint32_t g_gpu_word_addr = 0;
uint32_t g_gpu_ot_z = 0;

// The game's two ordering tables (frame buffers 800A72A0 / +1F070, table at +0x70, 4096 slots):
// a slot's byte offset is the depth the game sorted the packets after it at.
uint32_t ot_depth(uint32_t addr) {
    for (uint32_t base : {0x0A7310u, 0x0C6380u})
        if (addr >= base && addr < base + 0x4000) return addr - base;
    return 0;
}

// Sends a linked list of GPU packets (an ordering table) to GP0; returns the words read.
uint32_t gpu_run_list(R3000* c, uint32_t addr) {
    addr &= 0x1FFFFC;
    uint32_t guard = 0, words = 0;
    for (;;) {
        uint32_t header;
        std::memcpy(&header, psx_ram + addr, 4);
        uint32_t n = header >> 24;
        if (n == 0) g_gpu_ot_z = ot_depth(addr);
        g_gpu_packet_addr = addr;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w;
            g_gpu_word_addr = (addr + 4 + i * 4) & 0x1FFFFC;
            std::memcpy(&w, psx_ram + g_gpu_word_addr, 4);
            gpu_write_gp0(c, w);
        }
        words += n + 1;
        g_gpu_packet_addr = g_gpu_word_addr = 0;
        if (header & 0x800000) break;
        addr = header & 0x1FFFFC;
        if (++guard > 0x100000) {
            log_msg(LogCat::Dma, "GPU linked list does not terminate");
            break;
        }
    }
    g_gpu_ot_z = 0;
    if (words > 100) {  // a frame's ordering table (not the small setup lists)
        gpu_list_done();
        if (!core_in_ghost()) core_frame_drawn(c);
    }
    return words;
}

namespace {

// Moves the data immediately; completion (busy bit, IRQ) is signalled after `delay`.
void dma_run(R3000* c, int ch) {
    DmaChan& d = g_dma[ch];
    uint32_t sync = (d.chcr >> 9) & 3;
    bool from_ram = d.chcr & 1;
    int32_t step = (d.chcr & 2) ? -4 : 4;
    uint32_t addr = d.madr & 0x1FFFFC;
    uint64_t delay = 0;

    if (ch == 6) {  // OTC: reverse linked list clear
        uint32_t n = word_count(d);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = i == n - 1 ? 0x00FFFFFF : ((addr - 4) & 0x1FFFFC);
            std::memcpy(psx_ram + addr, &v, 4);
            addr = (addr - 4) & 0x1FFFFC;
        }
        LOG(Dma, "OTC %08X x%u", d.madr, n);
        dma_finish(c, ch);
        return;
    }

    if (ch == 2 && sync == 2) {  // GPU linked list
        uint32_t words = gpu_run_list(c, addr);
        d.madr = 0x00FFFFFF;
        delay = words;  // roughly 1 cycle per word of bus time
        LOG(Dma, "GPU list %u words", words);
        sched_add(c, Ev::DmaGpu, delay + 64);
        return;
    }

    if (core_in_ghost() && ch != 2) return;  // sound, CD and movie DMA stay in real time
    uint32_t n = word_count(d);
    if (ch == 1 && !mdec_out_ready()) {
        // Output DMA is usually armed before the input arrives; run it once decoded.
        g_mdec_out_waiting = true;
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t* p = (uint32_t*)(psx_ram + addr);
        uint32_t w;
        switch (ch) {
            case 0: std::memcpy(&w, p, 4); mdec_dma_in(w); break;
            case 1: w = mdec_dma_out(); std::memcpy(p, &w, 4); break;
            case 2:
                if (from_ram) { std::memcpy(&w, p, 4); gpu_write_gp0(c, w); }
                else { w = gpu_read_data(c); std::memcpy(p, &w, 4); }
                break;
            case 3: w = cdrom_dma_read_word(); std::memcpy(p, &w, 4); break;
            case 4:
                if (from_ram) { std::memcpy(&w, p, 4); spu_dma_write(w); }
                else { w = spu_dma_read(); std::memcpy(p, &w, 4); }
                break;
            default: break;
        }
        addr = (addr + (uint32_t)step) & 0x1FFFFC;
    }
    if (sync == 1) {
        d.madr = addr;
        d.bcr &= 0xFFFF;  // block count reaches zero
    }
    LOG(Dma, "ch%d %s %u words", ch, from_ram ? "from RAM" : "to RAM", n);
    delay = n;
    Ev ev = ch == 3 ? Ev::DmaCd : ch == 4 ? Ev::DmaSpu : (ch == 0 || ch == 1) ? Ev::DmaMdec : Ev::DmaGpu;
    if (ch == 0) {
        dma_finish(c, 0);
        if (g_mdec_out_waiting && mdec_out_ready()) {
            g_mdec_out_waiting = false;
            dma_run(c, 1);
        }
        return;
    }
    sched_add(c, ev, delay + 16);
}

}  // namespace

void dma_init() {
    for (auto& d : g_dma) d = {};
    g_dpcr = 0x07654321;
    g_dicr = 0;
}

void dma_event(R3000* c, int ch) {
    if (g_dma[ch].chcr & CHCR_START) dma_finish(c, ch);
}

uint32_t dma_read(uint32_t addr) {
    if (addr == 0x1F8010F0) return g_dpcr;
    if (addr == 0x1F8010F4) return g_dicr;
    int ch = (int)((addr - 0x1F801080) >> 4);
    if (ch > 6) return 0;
    switch (addr & 0xF) {
        case 0x0: return g_dma[ch].madr;
        case 0x4: return g_dma[ch].bcr;
        case 0x8: return g_dma[ch].chcr;
    }
    return 0;
}

void dma_write(R3000* c, uint32_t addr, uint32_t v) {
    if (addr == 0x1F8010F0) { g_dpcr = v; return; }
    if (addr == 0x1F8010F4) {
        uint32_t ack = v & 0x7F000000u;
        g_dicr = (g_dicr & 0xFF000000u & ~ack) | (v & 0x00FF803Fu);
        dicr_update(c);
        return;
    }
    int ch = (int)((addr - 0x1F801080) >> 4);
    if (ch > 6) return;
    DmaChan& d = g_dma[ch];
    switch (addr & 0xF) {
        case 0x0: d.madr = v & 0xFFFFFF; return;
        case 0x4: d.bcr = v; return;
        case 0x8:
            d.chcr = ch == 6 ? ((v & 0x51000000u) | 2) : v;
            break;
        default: return;
    }
    bool enabled = (g_dpcr >> (ch * 4 + 3)) & 1;
    uint32_t sync = (d.chcr >> 9) & 3;
    bool go = (d.chcr & CHCR_START) && (sync != 0 || (d.chcr & CHCR_TRIGGER) || ch == 6);
    if (go && enabled) dma_run(c, ch);
    else if (go) LOG(Dma, "ch%d started while disabled in DPCR", ch);
}

// ================================================================ root counters

namespace {

struct Timer {
    uint32_t mode = 0, target = 0;
    uint32_t value = 0;
    uint64_t last_sync = 0;   // cycle count at last lazy update
    uint64_t frac = 0;        // sub-tick remainder for divided clocks
    bool irq_armed = true;
};
Timer g_tmr[3];

// Converts elapsed CPU cycles into timer ticks for sources driven by the CPU clock.
uint64_t ticks_for(int n, uint64_t cycles, Timer& t) {
    uint32_t src = (t.mode >> 8) & 3;
    if (n == 0 && (src & 1)) {  // dot clock, 320-wide mode: ~CPU*11/7/8
        t.frac += cycles * 11;
        uint64_t k = t.frac / 56;
        t.frac %= 56;
        return k;
    }
    if (n == 1 && (src & 1)) return 0;  // hblank clock, advanced per scanline
    if (n == 2 && (src & 2)) {
        t.frac += cycles;
        uint64_t k = t.frac / 8;
        t.frac %= 8;
        return k;
    }
    return cycles;
}

void advance(R3000* c, int n, uint64_t ticks) {
    Timer& t = g_tmr[n];
    if (!ticks) return;
    uint64_t v = t.value + ticks;
    bool reset_at_target = t.mode & (1 << 3);
    uint32_t tgt = t.target ? t.target : 0x10000;
    bool hit_target = false, hit_ffff = false;
    if (reset_at_target) {
        if (v >= tgt) {
            hit_target = true;
            v %= tgt;
        }
    } else {
        if (t.value < tgt && v >= tgt) hit_target = true;
        if (v > 0xFFFF) {
            hit_ffff = true;
            v &= 0xFFFF;
        }
    }
    t.value = (uint32_t)v;
    if (hit_target) t.mode |= 1u << 11;
    if (hit_ffff) t.mode |= 1u << 12;
    bool irq = (hit_target && (t.mode & (1 << 4))) || (hit_ffff && (t.mode & (1 << 5)));
    if (irq && t.irq_armed) {
        irq_raise(c, IRQ_TMR0 << n);
        if (!(t.mode & (1 << 6))) t.irq_armed = false;  // one-shot
        t.mode &= ~(1u << 10);                          // IRQ flag (active low)
    }
}

void sync_timer(R3000* c, int n) {
    Timer& t = g_tmr[n];
    uint64_t dt = c->cycles - t.last_sync;
    t.last_sync = c->cycles;
    advance(c, n, ticks_for(n, dt, t));
}

}  // namespace

void timers_init() {
    for (auto& t : g_tmr) t = {};
    for (auto& t : g_tmr) t.mode = 1u << 10;
}

uint32_t timers_read(R3000* c, uint32_t addr) {
    int n = (int)((addr - 0x1F801100) >> 4);
    if (n > 2) return 0;
    sync_timer(c, n);
    Timer& t = g_tmr[n];
    switch (addr & 0xF) {
        case 0x0: return t.value & 0xFFFF;
        case 0x4: {
            uint32_t m = t.mode;
            t.mode &= ~((1u << 11) | (1u << 12));  // reached-flags clear on read
            return m;
        }
        case 0x8: return t.target;
    }
    return 0;
}

void timers_write(R3000* c, uint32_t addr, uint32_t v) {
    int n = (int)((addr - 0x1F801100) >> 4);
    if (n > 2) return;
    sync_timer(c, n);
    Timer& t = g_tmr[n];
    switch (addr & 0xF) {
        case 0x0: t.value = v & 0xFFFF; break;
        case 0x4:
            t.mode = (v & 0x3FF) | (1u << 10);
            t.value = 0;
            t.irq_armed = true;
            break;
        case 0x8: t.target = v & 0xFFFF; break;
    }
}

void timers_scanline(R3000* c, bool vblank_start, bool vblank_end) {
    (void)vblank_end;
    for (int n = 0; n < 3; n++) sync_timer(c, n);
    if (g_tmr[1].mode & (1 << 8)) advance(c, 1, 1);  // hblank-clocked
    (void)vblank_start;
}

// ================================================================ SIO0: digital pads

namespace {

// A byte exchange takes time: the reply is readable after the 8 bits are clocked out,
// and a device that wants more bytes pulses /ACK shortly after. Drivers that clear
// the IRQ flags between "reply ready" and "wait for /ACK" depend on that ordering.
constexpr uint64_t SIO_BYTE_CYCLES = 1088;  // 8 bits at the usual JOY_BAUD of 0x88
constexpr uint64_t SIO_ACK_DELAY = 170;

enum : uint32_t { STAT_TX_READY = 1, STAT_RX_READY = 2, STAT_TX_DONE = 4, STAT_ACK_LOW = 1 << 7, STAT_IRQ = 1 << 9 };

uint16_t g_sio_ctrl = 0, g_sio_mode = 0, g_sio_baud = 0;
uint32_t g_sio_stat = STAT_TX_READY | STAT_TX_DONE;
int g_sio_step = 0;
uint8_t g_sio_rx = 0xFF;
bool g_sio_rx_full = false;
bool g_sio_device_pad = false;

// transfer in flight
enum class SioPhase { Idle, Transferring, AckPending } g_sio_phase = SioPhase::Idle;
uint8_t g_sio_next_rx = 0xFF;
bool g_sio_next_ack = false;

// DualShock (SCPH-1200) emulation on port 1: digital mode (ID 41), analog mode (ID 73),
// config mode (ID F3) and the rumble motors.
struct DualShock {
    bool analog = false, config = false;
    uint8_t motor_map[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 0x4D mapping
    uint8_t small = 0, large = 0;
    // current transaction
    uint8_t cmd = 0;
    uint8_t resp[16] = {};
    uint8_t txb[16] = {};
    int len = 0;  // bytes after the 0x01 address byte
} g_ds;

// Builds the reply for a command; called when the command byte arrives.
void ds_begin(uint8_t cmd, const PadState& p) {
    DualShock& d = g_ds;
    d.cmd = cmd;
    std::memset(d.resp, 0, sizeof d.resp);
    std::memset(d.txb, 0, sizeof d.txb);
    uint8_t id = d.config ? 0xF3 : d.analog ? 0x73 : 0x41;
    d.resp[0] = id;
    d.resp[1] = 0x5A;
    auto buttons = [&] {
        d.resp[2] = (uint8_t)p.buttons;
        d.resp[3] = (uint8_t)(p.buttons >> 8);
        d.resp[4] = p.rx; d.resp[5] = p.ry; d.resp[6] = p.lx; d.resp[7] = p.ly;
    };
    int payload = (id & 0xF) * 2;  // 41 -> 2 bytes, 73/F3 -> 6 bytes
    if (cmd == 0x42 || (cmd == 0x43 && !d.config)) {
        buttons();
    } else if (d.config) {
        switch (cmd) {
            case 0x45: {  // status: controller type, current mode
                const uint8_t r[6] = {0x01, 0x02, (uint8_t)(d.analog ? 1 : 0), 0x02, 0x01, 0x00};
                std::memcpy(d.resp + 2, r, 6);
                break;
            }
            case 0x47: { const uint8_t r[6] = {0, 0, 2, 0, 1, 0}; std::memcpy(d.resp + 2, r, 6); break; }
            case 0x4D: std::memcpy(d.resp + 2, d.motor_map, 6); break;
            default: break;  // 0x43 (exit), 0x44 (set mode), 0x46/0x4C (answered on the param byte)
        }
    } else {
        payload = 0;  // unknown command outside config mode
    }
    d.len = 2 + payload;
}

// Called for every payload byte; some replies depend on the parameter just sent.
void ds_param(int idx, uint8_t tx) {
    DualShock& d = g_ds;
    d.txb[idx] = tx;
    if (d.config && idx == 2) {
        if (d.cmd == 0x46) {
            const uint8_t r0[6] = {0, 0, 1, 2, 0, 0x0A}, r1[6] = {0, 0, 1, 1, 1, 0x14};
            std::memcpy(d.resp + 3, (tx ? r1 : r0) + 1, 5);
        } else if (d.cmd == 0x4C) {
            d.resp[5] = tx ? 0x07 : 0x04;
        }
    }
}

// The last completed reading (command 42): the pad's ID and its data bytes (pad_last_reading).
uint8_t g_read_id = 0, g_read_data[6];
int g_read_len = 0;

// Applies the effects of a completed transaction.
void ds_end() {
    DualShock& d = g_ds;
    if (d.cmd == 0x42 && !d.config) {
        g_read_id = d.resp[0];
        g_read_len = std::min(d.len - 2, 6);
        std::memcpy(g_read_data, d.resp + 2, (size_t)g_read_len);
    }
    const uint8_t* a = d.txb + 2;  // parameter bytes
    if (d.cmd == 0x42 && !d.config) {
        // motor bytes routed by the 0x4D mapping: 00 = small motor (on/off), 01 = large motor
        uint8_t small = 0, large = 0;
        for (int i = 0; i < 6; i++) {
            if (d.motor_map[i] == 0x00) small = a[i] == 0xFF || a[i] == 1 ? 1 : 0;
            if (d.motor_map[i] == 0x01) large = a[i];
        }
        if (small != d.small || large != d.large) LOG(Pad, "rumble small=%u large=%u", small, large);
        d.small = small;
        d.large = large;
        host_rumble(0, small ? 255 : 0, large);
    } else if (d.cmd == 0x43) {
        bool was = d.config;
        d.config = a[0] == 1;
        if (was != d.config) LOG(Pad, "DualShock %s config mode", d.config ? "entered" : "left");
    } else if (d.config && d.cmd == 0x44) {
        d.analog = a[0] == 1;
        LOG(Pad, "DualShock set to %s mode", d.analog ? "analog" : "digital");
    } else if (d.config && d.cmd == 0x4D) {
        std::memcpy(d.motor_map, a, 6);
        LOG(Pad, "rumble mapping %02X %02X %02X %02X %02X %02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    }
}

void sio_transfer(R3000* c, uint8_t tx) {
    int port = (g_sio_ctrl >> 13) & 1;
    uint8_t rx = 0xFF;
    bool ack = false;
    if (g_sio_step == 0) {
        g_sio_device_pad = tx == 0x01 && port == 0 && host_pad(0).connected;
        ack = g_sio_device_pad;
    } else if (g_sio_device_pad) {
        int idx = g_sio_step - 1;  // 0 = command byte
        if (idx == 0) ds_begin(tx, host_pad(0, g_ds.analog));
        else ds_param(idx, tx);
        rx = g_ds.resp[idx];
        ack = idx + 1 < g_ds.len;
        if (!ack) ds_end();
    }
    LOG(Pad, "sio port%d step%d tx=%02X rx=%02X ack=%d ctrl=%04X", port, g_sio_step, tx, rx, ack, g_sio_ctrl);
    g_sio_step = ack ? g_sio_step + 1 : 0;
    g_sio_next_rx = rx;
    g_sio_next_ack = ack;
    g_sio_stat &= ~(STAT_TX_DONE | STAT_ACK_LOW);
    g_sio_phase = SioPhase::Transferring;
    sched_add(c, Ev::Sio, SIO_BYTE_CYCLES);
}

}  // namespace

bool pad_last_reading(uint8_t* id, uint8_t* data, int* n) {
    if (!g_read_len) return false;
    *id = g_read_id;
    *n = g_read_len;
    std::memcpy(data, g_read_data, (size_t)g_read_len);
    return true;
}

void sio_event(R3000* c) {
    switch (g_sio_phase) {
        case SioPhase::Transferring:
            g_sio_rx = g_sio_next_rx;
            g_sio_rx_full = true;
            g_sio_stat |= STAT_TX_DONE;
            if (g_sio_next_ack) {
                g_sio_phase = SioPhase::AckPending;
                sched_add(c, Ev::Sio, SIO_ACK_DELAY);
            } else {
                g_sio_phase = SioPhase::Idle;
            }
            break;
        case SioPhase::AckPending:
            g_sio_phase = SioPhase::Idle;
            g_sio_stat |= STAT_ACK_LOW;
            if (g_sio_ctrl & (1 << 12)) {
                g_sio_stat |= STAT_IRQ;
                irq_raise(c, IRQ_SIO0);
            }
            break;
        case SioPhase::Idle: break;
    }
}

void sio_init() {
    g_sio_ctrl = g_sio_mode = g_sio_baud = 0;
    g_sio_step = 0;
    g_sio_rx_full = false;
    g_sio_phase = SioPhase::Idle;
}

uint32_t sio_read(R3000* c, uint32_t addr, int size) {
    (void)c; (void)size;
    switch (addr) {
        case 0x1F801040: {
            uint8_t v = g_sio_rx_full ? g_sio_rx : 0xFF;
            g_sio_rx_full = false;
            return v;
        }
        case 0x1F801044: return g_sio_stat | (g_sio_rx_full ? 2u : 0u);
        case 0x1F801048: return g_sio_mode;
        case 0x1F80104A: return g_sio_ctrl;
        case 0x1F80104E: return g_sio_baud;
    }
    return 0;  // SIO1 (serial port) is not connected
}

void sio_write(R3000* c, uint32_t addr, uint32_t v, int size) {
    (void)size;
    switch (addr) {
        case 0x1F801040:
            if (g_sio_ctrl & 2) sio_transfer(c, (uint8_t)v);
            break;
        case 0x1F801048: g_sio_mode = (uint16_t)v; break;
        case 0x1F80104A:
            if (v & 0x40) {  // reset
                g_sio_ctrl = 0;
                g_sio_stat = STAT_TX_READY | STAT_TX_DONE;
                g_sio_step = 0;
                g_sio_phase = SioPhase::Idle;
                sched_cancel(c, Ev::Sio);
                break;
            }
            if (v & 0x10) g_sio_stat &= ~(STAT_IRQ | STAT_ACK_LOW);  // acknowledge
            if (!(v & 2)) {                                           // /JOY deselected: end of transfer
                g_sio_step = 0;
                g_sio_stat &= ~STAT_ACK_LOW;
            }
            g_sio_ctrl = (uint16_t)(v & ~0x50u);
            break;
        case 0x1F80104E: g_sio_baud = (uint16_t)v; break;
    }
}

// ================================================================ save states

void hw_state(StateIO& s) {
    s.marker(0x44544D53);  // "DTMS"
    s.raw(g_dma, sizeof g_dma);
    s(g_dpcr);
    s(g_dicr);
    s(g_mdec_out_waiting);
    s.raw(g_tmr, sizeof g_tmr);
    s(g_sio_ctrl); s(g_sio_mode); s(g_sio_baud); s(g_sio_stat); s(g_sio_step);
    s(g_sio_rx); s(g_sio_rx_full); s(g_sio_device_pad); s(g_sio_phase);
    s(g_sio_next_rx); s(g_sio_next_ack);
    s(g_ds);
}
