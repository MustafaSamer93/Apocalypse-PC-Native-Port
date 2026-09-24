// SPU: 24 ADPCM voices with ADSR envelopes, volume sweeps, pitch modulation, noise,
// reverb and CD audio input. Samples are produced in emulated time (caught up on every
// register access and every scanline) and handed to the host once per frame.
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

#include "psx.h"

namespace {

constexpr uint32_t SPU_RAM = 512 * 1024;
constexpr uint64_t CYCLES_PER_SAMPLE = PSX_CPU_HZ / 44100;  // 768

uint8_t g_ram[SPU_RAM];
uint16_t g_regs[0x200];  // raw register file, 0x1F801C00..0x1F801FFF / 2

uint16_t& reg(uint32_t off) { return g_regs[(off >> 1) & 0x1FF]; }
uint16_t SPUCNT() { return reg(0x1AA); }

int16_t sat16(int32_t v) { return (int16_t)std::clamp(v, -32768, 32767); }
int32_t mul15(int32_t a, int32_t b) { return (a * b) >> 15; }

// ---------------------------------------------------------------- envelopes

// Shared by ADSR and volume sweeps: one step of the PS1 envelope generator.
//   shift/step come straight from the register fields (step is the 2-bit "rate" part).
struct Envelope {
    int32_t counter = 0;

    void tick(int32_t& level, bool exponential, bool decrease, int shift, int step_bits, int32_t max) {
        int32_t cycles = 1 << std::max(0, shift - 11);
        int32_t step = decrease ? (-8 + step_bits) : (7 - step_bits);
        step = step * (1 << std::max(0, 11 - shift));
        if (exponential && !decrease && level > 0x6000) cycles *= 4;
        if (exponential && decrease) step = (step * level) >> 15;
        if (++counter < cycles) return;
        counter = 0;
        level = std::clamp(level + step, 0, max);
    }
};

// A volume register pair value: either fixed, or a sweep driven by an envelope.
struct SweepVolume {
    int32_t level = 0;  // 0..0x7FFF magnitude (sweep mode)
    Envelope env;

    int32_t tick(uint16_t r) {
        if (!(r & 0x8000)) {  // fixed volume: 15-bit signed, *2
            level = std::abs((int16_t)(r << 1));
            return (int16_t)(r << 1);
        }
        bool exponential = r & 0x4000, decrease = r & 0x2000, negative = r & 0x1000;
        env.tick(level, exponential, decrease, (r >> 2) & 0x1F, r & 3, 0x7FFF);
        return negative ? -level : level;
    }
};

// ---------------------------------------------------------------- voices

enum Phase { Off, Attack, Decay, Sustain, Release };

struct Voice {
    uint32_t addr = 0;     // current ADPCM block (bytes)
    uint32_t repeat = 0;
    uint32_t counter = 0;  // 12.12 fixed-point position inside the block
    int16_t buf[31] = {};  // [0..2] = last samples of the previous block, [3..30] = current block
    int16_t prev[2] = {};  // ADPCM filter history
    uint8_t flags = 0;
    Phase phase = Off;
    int32_t level = 0;     // ADSR 0..0x7FFF
    Envelope adsr;
    SweepVolume vol[2];
    int32_t last_out = 0;  // ADSR-scaled output, feeds the next voice's pitch modulation
};
Voice g_voice[24];
uint32_t g_endx = 0;
uint32_t g_transfer_addr = 0;
bool g_irq_flag = false;
SweepVolume g_main_vol[2];

// noise generator (psx-spx algorithm)
int32_t g_noise_timer = 0;
uint16_t g_noise_level = 1;

// reverb
uint32_t g_rev_addr = 0;
int32_t g_rev_out[2] = {};
bool g_rev_odd = false;

// CD audio input, resampled to 44.1 kHz stereo
std::deque<int16_t> g_cd_fifo;
double g_cd_phase = 0;
int16_t g_cd_prev[2] = {};

// sample production in emulated time
uint64_t g_last_cycles = 0;
uint64_t g_cycle_accum = 0;
std::vector<int16_t> g_out;  // produced since the last frame hand-off

const int kPos[5] = {0, 60, 115, 98, 122};
const int kNeg[5] = {0, 0, -52, -55, -60};

void check_irq(uint32_t addr) {
    if (!(SPUCNT() & 0x40)) return;
    uint32_t irq_addr = (uint32_t)reg(0x1A4) * 8;
    if ((addr & ~0xFu) == (irq_addr & ~0xFu) && !g_irq_flag) {
        g_irq_flag = true;
        if (g_cpu) irq_raise(g_cpu, IRQ_SPU);
    }
}

void decode_block(Voice& v) {
    const uint8_t* b = &g_ram[v.addr & (SPU_RAM - 1)];
    check_irq(v.addr);
    int shift = b[0] & 0xF;
    if (shift > 12) shift = 9;
    int filter = std::min((b[0] >> 4) & 7, 4);
    v.flags = b[1];
    if (v.flags & 4) v.repeat = v.addr;  // loop start
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t t = (int32_t)((uint32_t)nib << 28) >> 28;
        int32_t s = ((t << 12) >> shift) + ((v.prev[0] * kPos[filter] + v.prev[1] * kNeg[filter] + 32) >> 6);
        s = std::clamp(s, -32768, 32767);
        v.prev[1] = v.prev[0];
        v.prev[0] = (int16_t)s;
        v.buf[3 + i] = (int16_t)s;
    }
}

void run_adsr(Voice& v, int n) {
    uint16_t a1 = reg(n * 16 + 8), a2 = reg(n * 16 + 10);
    switch (v.phase) {
        case Attack:
            v.adsr.tick(v.level, a1 & 0x8000, false, (a1 >> 10) & 0x1F, (a1 >> 8) & 3, 0x7FFF);
            if (v.level >= 0x7FFF) v.phase = Decay;
            break;
        case Decay: {
            // decay: exponential decrease, shift = 4-bit field, step = -8
            v.adsr.tick(v.level, true, true, (a1 >> 4) & 0xF, 0, 0x7FFF);
            int32_t sustain = std::min(0x7FFF, ((a1 & 0xF) + 1) * 0x800);
            if (v.level <= sustain) v.phase = Sustain;
            break;
        }
        case Sustain:
            v.adsr.tick(v.level, a2 & 0x8000, a2 & 0x4000, (a2 >> 8) & 0x1F, (a2 >> 6) & 3, 0x7FFF);
            break;
        case Release:
            // release: linear or exponential decrease, shift = 5-bit field, step = -8
            v.adsr.tick(v.level, a2 & 0x20, true, a2 & 0x1F, 0, 0x7FFF);
            if (v.level <= 0) v.phase = Off;
            break;
        case Off: break;
    }
}

void key_on(int n) {
    Voice& v = g_voice[n];
    v.addr = (uint32_t)reg(n * 16 + 6) * 8;
    v.counter = 0;
    v.prev[0] = v.prev[1] = 0;
    std::memset(v.buf, 0, sizeof v.buf);
    v.phase = Attack;
    v.level = 0;
    v.adsr.counter = 0;
    g_endx &= ~(1u << n);
    decode_block(v);
}

void key_off(int n) {
    if (g_voice[n].phase != Off) g_voice[n].phase = Release;
}

// 4-point cubic (Catmull-Rom) through p0..p3, evaluated between p1 and p2.
int32_t cubic(int32_t p0, int32_t p1, int32_t p2, int32_t p3, int32_t frac /* 0..4095 */) {
    int64_t t = frac;
    int64_t a = -p0 + 3 * p1 - 3 * p2 + p3;
    int64_t b = 2 * p0 - 5 * p1 + 4 * p2 - p3;
    int64_t c = -p0 + p2;
    int64_t v = (((a * t >> 12) + b) * t >> 12) + c;
    return (int32_t)(p1 + ((v * t) >> 13));
}

// ---------------------------------------------------------------- reverb

uint32_t rev_base() { return (uint32_t)reg(0x1A2) * 8; }

uint32_t rev_wrap(int64_t addr) {
    uint32_t base = rev_base();
    uint32_t size = SPU_RAM - base;
    if (size == 0) return 0;
    int64_t rel = (addr - base) % size;
    if (rel < 0) rel += size;
    return base + (uint32_t)rel;
}

// Reverb buffer access relative to the current buffer address. `off` is a register value
// (8-byte units); `extra` is in bytes (the "-2" in the psx-spx formulas).
int32_t rev_read(uint16_t off, int extra = 0) {
    uint32_t a = rev_wrap((int64_t)g_rev_addr + (int64_t)off * 8 + extra) & ~1u;
    int16_t v;
    std::memcpy(&v, &g_ram[a], 2);
    return v;
}

void rev_write(uint16_t off, int32_t value) {
    if (!(SPUCNT() & 0x80)) return;  // master reverb enable gates writes
    uint32_t a = rev_wrap((int64_t)g_rev_addr + (int64_t)off * 8) & ~1u;
    int16_t v = sat16(value);
    std::memcpy(&g_ram[a], &v, 2);
}

// Processes one reverb step (22.05 kHz) and updates g_rev_out.
void reverb_step(int32_t in_l, int32_t in_r) {
    auto R = [](uint32_t off) { return (int32_t)(int16_t)reg(off); };
    auto U = [](uint32_t off) { return reg(off); };
    const int32_t vIIR = R(0x1C4), vWALL = R(0x1CE), vAPF1 = R(0x1D0), vAPF2 = R(0x1D2);
    const int32_t vCOMB1 = R(0x1C6), vCOMB2 = R(0x1C8), vCOMB3 = R(0x1CA), vCOMB4 = R(0x1CC);
    const uint16_t dAPF1 = U(0x1C0), dAPF2 = U(0x1C2);
    const uint16_t mLSAME = U(0x1D4), mRSAME = U(0x1D6), mLCOMB1 = U(0x1D8), mRCOMB1 = U(0x1DA);
    const uint16_t mLCOMB2 = U(0x1DC), mRCOMB2 = U(0x1DE), dLSAME = U(0x1E0), dRSAME = U(0x1E2);
    const uint16_t mLDIFF = U(0x1E4), mRDIFF = U(0x1E6), mLCOMB3 = U(0x1E8), mRCOMB3 = U(0x1EA);
    const uint16_t mLCOMB4 = U(0x1EC), mRCOMB4 = U(0x1EE), dLDIFF = U(0x1F0), dRDIFF = U(0x1F2);
    const uint16_t mLAPF1 = U(0x1F4), mRAPF1 = U(0x1F6), mLAPF2 = U(0x1F8), mRAPF2 = U(0x1FA);

    int32_t Lin = mul15(R(0x1FC), in_l), Rin = mul15(R(0x1FE), in_r);

    // same-side and different-side reflections
    rev_write(mLSAME, mul15(Lin + mul15(rev_read(dLSAME), vWALL) - rev_read(mLSAME, -2), vIIR) + rev_read(mLSAME, -2));
    rev_write(mRSAME, mul15(Rin + mul15(rev_read(dRSAME), vWALL) - rev_read(mRSAME, -2), vIIR) + rev_read(mRSAME, -2));
    rev_write(mLDIFF, mul15(Lin + mul15(rev_read(dRDIFF), vWALL) - rev_read(mLDIFF, -2), vIIR) + rev_read(mLDIFF, -2));
    rev_write(mRDIFF, mul15(Rin + mul15(rev_read(dLDIFF), vWALL) - rev_read(mRDIFF, -2), vIIR) + rev_read(mRDIFF, -2));

    // early echo (comb filter)
    int32_t Lout = mul15(vCOMB1, rev_read(mLCOMB1)) + mul15(vCOMB2, rev_read(mLCOMB2)) +
                   mul15(vCOMB3, rev_read(mLCOMB3)) + mul15(vCOMB4, rev_read(mLCOMB4));
    int32_t Rout = mul15(vCOMB1, rev_read(mRCOMB1)) + mul15(vCOMB2, rev_read(mRCOMB2)) +
                   mul15(vCOMB3, rev_read(mRCOMB3)) + mul15(vCOMB4, rev_read(mRCOMB4));

    // late reverb: two all-pass filters
    auto apf = [&](int32_t x, uint16_t m, uint16_t d, int32_t vol) {
        int32_t delayed = rev_read((uint16_t)(m - d));
        x = sat16(x - mul15(vol, delayed));
        rev_write(m, x);
        return mul15(x, vol) + delayed;
    };
    Lout = apf(Lout, mLAPF1, dAPF1, vAPF1);
    Rout = apf(Rout, mRAPF1, dAPF1, vAPF1);
    Lout = apf(Lout, mLAPF2, dAPF2, vAPF2);
    Rout = apf(Rout, mRAPF2, dAPF2, vAPF2);

    g_rev_out[0] = mul15(sat16(Lout), R(0x184));
    g_rev_out[1] = mul15(sat16(Rout), R(0x186));

    uint32_t base = rev_base();
    g_rev_addr = std::max(base, (g_rev_addr + 2) & 0x7FFFE);
    if (g_rev_addr < base) g_rev_addr = base;
}

// ---------------------------------------------------------------- mixer

void noise_step() {
    uint16_t cnt = SPUCNT();
    int shift = (cnt >> 10) & 0xF, step = ((cnt >> 8) & 3) + 4;
    g_noise_timer -= step;
    uint16_t n = g_noise_level;
    int parity = ((n >> 15) ^ (n >> 12) ^ (n >> 11) ^ (n >> 10) ^ 1) & 1;
    if (g_noise_timer < 0) g_noise_level = (uint16_t)(n * 2 + parity);
    if (g_noise_timer < 0) g_noise_timer += 0x20000 >> shift;
    if (g_noise_timer < 0) g_noise_timer += 0x20000 >> shift;
}

// One 44.1 kHz output sample.
void mix_sample(int16_t& out_l, int16_t& out_r) {
    uint16_t cnt = SPUCNT();
    uint32_t pmon = reg(0x190) | (uint32_t)reg(0x192) << 16;
    uint32_t non = reg(0x194) | (uint32_t)reg(0x196) << 16;
    uint32_t eon = reg(0x198) | (uint32_t)reg(0x19A) << 16;
    noise_step();

    int32_t l = 0, r = 0, rev_l = 0, rev_r = 0;
    for (int n = 0; n < 24; n++) {
        Voice& v = g_voice[n];
        int32_t vol_l = v.vol[0].tick(reg(n * 16 + 0)), vol_r = v.vol[1].tick(reg(n * 16 + 2));
        if (v.phase == Off) {
            v.last_out = 0;
            continue;
        }
        int32_t idx = (int32_t)(v.counter >> 12), frac = (int32_t)(v.counter & 0xFFF);
        int32_t sample = cubic(v.buf[idx], v.buf[idx + 1], v.buf[idx + 2], v.buf[idx + 3], frac);
        sample = std::clamp(sample, -32768, 32767);
        if (non & (1u << n)) sample = (int16_t)g_noise_level;
        run_adsr(v, n);
        sample = (sample * v.level) >> 15;
        v.last_out = sample;
        int32_t sl = mul15(sample, vol_l), sr = mul15(sample, vol_r);
        l += sl;
        r += sr;
        if (eon & (1u << n)) { rev_l += sl; rev_r += sr; }

        int32_t step = reg(n * 16 + 4);
        if (n > 0 && (pmon & (1u << n))) {
            int32_t factor = g_voice[n - 1].last_out + 0x8000;
            step = (int32_t)(((int64_t)(int16_t)step * factor) >> 15) & 0xFFFF;
        }
        v.counter += (uint32_t)std::min(step, 0x4000);
        while ((v.counter >> 12) >= 28) {
            v.counter -= 28 << 12;
            std::memcpy(v.buf, v.buf + 28, 3 * sizeof(int16_t));  // keep history for interpolation
            if (v.flags & 1) {  // loop end
                g_endx |= 1u << n;
                if (v.flags & 2) {
                    v.addr = v.repeat;
                } else {
                    v.phase = Off;
                    v.level = 0;
                    v.addr = (v.addr + 16) & (SPU_RAM - 1);
                }
            } else {
                v.addr = (v.addr + 16) & (SPU_RAM - 1);
            }
            decode_block(v);
        }
    }

    // CD audio
    int32_t cdl = 0, cdr = 0;
    if (g_cd_fifo.size() >= 2) {
        cdl = g_cd_fifo[0];
        cdr = g_cd_fifo[1];
        g_cd_fifo.pop_front();
        g_cd_fifo.pop_front();
    }
    if (cnt & 1) {
        int32_t cl = mul15(cdl, (int16_t)reg(0x1B0)), cr = mul15(cdr, (int16_t)reg(0x1B2));
        l += cl;
        r += cr;
        if (cnt & 4) { rev_l += cl; rev_r += cr; }
    }

    // reverb runs at 22.05 kHz; its output is held for two samples
    if (!g_rev_odd) reverb_step(sat16(rev_l), sat16(rev_r));
    g_rev_odd = !g_rev_odd;
    if (cnt & 0x80) {
        l += g_rev_out[0];
        r += g_rev_out[1];
    }

    int32_t ml = g_main_vol[0].tick(reg(0x180)), mr = g_main_vol[1].tick(reg(0x182));
    if (!(cnt & 0x4000)) { l = 0; r = 0; }  // muted
    out_l = sat16(mul15(sat16(l), ml));
    out_r = sat16(mul15(sat16(r), mr));
}

// Produces all samples due up to the current emulated time.
void catch_up() {
    if (!g_cpu) return;
    uint64_t now = g_cpu->cycles;
    if (now <= g_last_cycles) return;
    g_cycle_accum += now - g_last_cycles;
    g_last_cycles = now;
    while (g_cycle_accum >= CYCLES_PER_SAMPLE) {
        g_cycle_accum -= CYCLES_PER_SAMPLE;
        int16_t l, r;
        mix_sample(l, r);
        g_out.push_back(l);
        g_out.push_back(r);
    }
    if (g_out.size() > 44100 * 2) g_out.erase(g_out.begin(), g_out.end() - 44100 * 2);  // after a long stall
}

}  // namespace

void spu_init() {
    std::memset(g_ram, 0, sizeof g_ram);
    std::memset(g_regs, 0, sizeof g_regs);
    for (auto& v : g_voice) v = Voice{};
    g_cd_fifo.clear();
    g_out.clear();
    g_rev_addr = 0;
}

uint16_t spu_read(uint32_t addr) {
    catch_up();
    uint32_t off = addr - 0x1F801C00;
    if (off < 0x180 && (off & 0xF) == 0xC) return (uint16_t)g_voice[off >> 4].level;
    if (off >= 0x200 && off < 0x260) {  // current voice volumes (L/R per voice)
        int n = (int)((off - 0x200) >> 2);
        return (uint16_t)g_voice[n].vol[(off >> 1) & 1].level;
    }
    switch (off) {
        case 0x19C: return (uint16_t)g_endx;
        case 0x19E: return (uint16_t)(g_endx >> 16);
        case 0x1B8: return (uint16_t)g_main_vol[0].level;
        case 0x1BA: return (uint16_t)g_main_vol[1].level;
        case 0x1AE: {  // SPUSTAT
            uint16_t cnt = SPUCNT();
            uint16_t s = cnt & 0x3F;
            if (g_irq_flag) s |= 0x40;
            uint32_t mode = (cnt >> 4) & 3;
            if (mode == 2 || mode == 3) s |= 0x80;  // DMA request
            return s;
        }
        case 0x1A8: return 0;
    }
    return reg(off);
}

void spu_write(R3000* c, uint32_t addr, uint16_t v) {
    (void)c;
    catch_up();  // everything before this write plays with the old register values
    uint32_t off = addr - 0x1F801C00;
    if (off >= 0x200) return;
    reg(off) = v;
    switch (off) {
        case 0x188: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_on(i); break;
        case 0x18A: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_on(16 + i); break;
        case 0x18C: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_off(i); break;
        case 0x18E: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_off(16 + i); break;
        case 0x1A2: g_rev_addr = (uint32_t)v * 8; break;  // new reverb work area
        case 0x1A6: g_transfer_addr = (uint32_t)v * 8; break;
        case 0x1A8:  // manual transfer FIFO
            check_irq(g_transfer_addr);
            g_ram[g_transfer_addr & (SPU_RAM - 1)] = (uint8_t)v;
            g_ram[(g_transfer_addr + 1) & (SPU_RAM - 1)] = (uint8_t)(v >> 8);
            g_transfer_addr = (g_transfer_addr + 2) & (SPU_RAM - 1);
            break;
        case 0x1AA: {
            if (!(v & 0x40)) g_irq_flag = false;  // disabling IRQ acknowledges it
            static bool rev_on = false;
            if (((v & 0x80) != 0) != rev_on) {
                rev_on = v & 0x80;
                LOG(Spu, "reverb %s (work area %05X, EON %06X)", rev_on ? "on" : "off", rev_base(),
                    reg(0x198) | (uint32_t)reg(0x19A) << 16);
            }
            break;
        }
        default:
            if (off < 0x180 && (off & 0xF) == 0xE) g_voice[off >> 4].repeat = (uint32_t)v * 8;
            break;
    }
}

void spu_dma_write(uint32_t w) {
    for (int i = 0; i < 4; i++) {
        if ((i & 1) == 0) check_irq(g_transfer_addr);
        g_ram[g_transfer_addr] = (uint8_t)(w >> (8 * i));
        g_transfer_addr = (g_transfer_addr + 1) & (SPU_RAM - 1);
    }
}

uint32_t spu_dma_read() {
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) {
        w |= (uint32_t)g_ram[g_transfer_addr] << (8 * i);
        g_transfer_addr = (g_transfer_addr + 1) & (SPU_RAM - 1);
    }
    return w;
}

void spu_push_cd_audio(const int16_t* stereo, int frames, int rate) {
    catch_up();
    // linear resample to 44.1 kHz
    double step = (double)rate / 44100.0;
    for (int i = 0; i < frames; i++) {
        int16_t cur[2] = {stereo[i * 2], stereo[i * 2 + 1]};
        while (g_cd_phase < 1.0) {
            for (int ch = 0; ch < 2; ch++)
                g_cd_fifo.push_back((int16_t)(g_cd_prev[ch] + (cur[ch] - g_cd_prev[ch]) * g_cd_phase));
            g_cd_phase += step;
        }
        g_cd_phase -= 1.0;
        g_cd_prev[0] = cur[0];
        g_cd_prev[1] = cur[1];
    }
    // keep latency bounded if the game streams faster than it plays
    const size_t max_samples = 44100 * 2;
    while (g_cd_fifo.size() > max_samples) g_cd_fifo.pop_front();
}

void spu_scanline() { catch_up(); }

void spu_end_frame(R3000* c) {
    (void)c;
    catch_up();
    if (!g_out.empty()) host_audio_push(g_out.data(), (int)(g_out.size() / 2));
    g_out.clear();
}

// ================================================================ save states

#include "state_io.h"

void spu_state(StateIO& s) {
    s.marker(0x53505520);  // "SPU "
    s.raw(g_ram, sizeof g_ram);
    s.raw(g_regs, sizeof g_regs);
    s.raw(g_voice, sizeof g_voice);
    s(g_endx); s(g_transfer_addr); s(g_irq_flag);
    s.raw(g_main_vol, sizeof g_main_vol);
    s(g_noise_timer); s(g_noise_level);
    s(g_rev_addr); s.raw(g_rev_out, sizeof g_rev_out); s(g_rev_odd);
    s.deq(g_cd_fifo);
    s(g_cd_phase);
    s.raw(g_cd_prev, sizeof g_cd_prev);
    s(g_last_cycles);
    s(g_cycle_accum);
    if (s.loading()) g_out.clear();
}
