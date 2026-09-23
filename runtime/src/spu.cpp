// SPU: 24 ADPCM voices with ADSR, noise, CD audio input. Reverb is not emulated yet.
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

#include "psx.h"

namespace {

constexpr uint32_t SPU_RAM = 512 * 1024;
uint8_t g_ram[SPU_RAM];
uint16_t g_regs[0x200];  // raw register file, 0x1F801C00..0x1F801FFF / 2

uint16_t& reg(uint32_t off) { return g_regs[(off >> 1) & 0x1FF]; }

enum Phase { Off, Attack, Decay, Sustain, Release };

struct Voice {
    uint32_t addr = 0;         // current ADPCM block (bytes)
    uint32_t repeat = 0;
    uint32_t counter = 0;      // 12.12 fixed-point sample position within the decoded block
    int16_t decoded[28] = {};
    int16_t prev[2] = {};      // ADPCM filter history
    int16_t last = 0, last2 = 0;  // for interpolation across blocks
    uint8_t flags = 0;
    Phase phase = Off;
    int32_t level = 0;         // ADSR 0..0x7FFF
    int32_t env_counter = 0;
    bool repeat_set_by_block = false;
};
Voice g_voice[24];
uint32_t g_endx = 0;
uint32_t g_transfer_addr = 0;
bool g_irq_flag = false;
uint32_t g_noise = 1;
int32_t g_noise_counter = 0;
int16_t g_noise_level = 0;

// CD audio input: resampled to 44.1 kHz stereo
std::deque<int16_t> g_cd_fifo;
double g_cd_phase = 0;
int16_t g_cd_prev[2] = {};

uint64_t g_sample_accum_cycles = 0;
uint64_t g_last_cycles = 0;

const int kPos[5] = {0, 60, 115, 98, 122};
const int kNeg[5] = {0, 0, -52, -55, -60};

uint16_t SPUCNT() { return reg(0x1AA); }

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
        v.decoded[i] = (int16_t)s;
    }
}

// ---------------------------------------------------------------- ADSR

struct EnvParams {
    bool exp, decrease;
    int shift, step;
};

void env_step(Voice& v, const EnvParams& p) {
    int cycles = 1 << std::max(0, p.shift - 11);
    int step = p.decrease ? (-8 + p.step) : (7 - p.step);
    step <<= std::max(0, 11 - p.shift);
    if (p.exp && !p.decrease && v.level > 0x6000) cycles *= 4;
    if (p.exp && p.decrease) step = (step * v.level) >> 15;
    if (++v.env_counter < cycles) return;
    v.env_counter = 0;
    v.level = std::clamp(v.level + step, 0, 0x7FFF);
}

void run_adsr(int n) {
    Voice& v = g_voice[n];
    uint16_t a1 = reg(n * 16 + 8), a2 = reg(n * 16 + 10);
    switch (v.phase) {
        case Attack:
            env_step(v, {(a1 & 0x8000) != 0, false, (a1 >> 10) & 0x1F, (a1 >> 8) & 3});
            if (v.level >= 0x7FFF) v.phase = Decay;
            break;
        case Decay: {
            env_step(v, {true, true, ((a1 >> 4) & 0xF) << 2, 0});
            int32_t sustain = std::min(0x7FFF, ((a1 & 0xF) + 1) * 0x800);
            if (v.level <= sustain) v.phase = Sustain;
            break;
        }
        case Sustain:
            env_step(v, {(a2 & 0x8000) != 0, (a2 & 0x4000) != 0, (a2 >> 8) & 0x1F, (a2 >> 6) & 3});
            break;
        case Release:
            env_step(v, {(a2 & 0x20) != 0, true, (a2 & 0x1F) << 2, 0});
            if (v.level <= 0) v.phase = Off;
            break;
        case Off: break;
    }
    reg(n * 16 + 12) = (uint16_t)v.level;
}

void key_on(int n) {
    Voice& v = g_voice[n];
    v.addr = (uint32_t)reg(n * 16 + 6) * 8;
    v.counter = 0;
    v.prev[0] = v.prev[1] = 0;
    v.last = v.last2 = 0;
    v.phase = Attack;
    v.level = 0;
    v.env_counter = 0;
    g_endx &= ~(1u << n);
    decode_block(v);
}

void key_off(int n) {
    if (g_voice[n].phase != Off) g_voice[n].phase = Release;
}

int16_t vol_fixed(uint16_t v) {
    if (v & 0x8000) return 0x3FFF;  // sweep mode: approximated as a steady mid-high volume
    return (int16_t)(v << 1);
}

// One output sample (44.1 kHz) for all voices.
void mix_sample(int32_t& out_l, int32_t& out_r) {
    uint16_t cnt = SPUCNT();
    uint32_t pmon = reg(0x190) | (uint32_t)reg(0x192) << 16;
    uint32_t non = reg(0x194) | (uint32_t)reg(0x196) << 16;

    // noise generator
    int nshift = (cnt >> 10) & 0xF, nstep = ((cnt >> 8) & 3) + 4;
    g_noise_counter += nstep << (15 - std::min(nshift, 15));
    while (g_noise_counter >= 0x20000) {
        g_noise_counter -= 0x20000;
        uint32_t fb = ((g_noise >> 15) ^ (g_noise >> 12) ^ (g_noise >> 11) ^ (g_noise >> 10) ^ 1) & 1;
        g_noise = (g_noise << 1) | fb;
        g_noise_level = (int16_t)g_noise;
    }

    int32_t l = 0, r = 0;
    int16_t prev_voice_out = 0;
    for (int n = 0; n < 24; n++) {
        Voice& v = g_voice[n];
        if (v.phase == Off) {
            prev_voice_out = 0;
            continue;
        }
        int32_t idx = (int32_t)(v.counter >> 12);
        int32_t frac = (int32_t)(v.counter & 0xFFF);
        int16_t s0 = idx == 0 ? v.last : v.decoded[idx - 1];
        int16_t s1 = v.decoded[idx];
        int32_t sample = s0 + (((s1 - s0) * frac) >> 12);
        if (non & (1u << n)) sample = g_noise_level;
        run_adsr(n);
        sample = (sample * v.level) >> 15;
        prev_voice_out = (int16_t)sample;
        l += (sample * vol_fixed(reg(n * 16 + 0))) >> 15;
        r += (sample * vol_fixed(reg(n * 16 + 2))) >> 15;

        uint32_t step = reg(n * 16 + 4);
        if (n > 0 && (pmon & (1u << n))) {
            int32_t factor = g_voice[n - 1].phase == Off ? 0 : prev_voice_out + 0x8000;
            step = (uint32_t)(((int32_t)(int16_t)step * factor) >> 15) & 0xFFFF;
        }
        v.counter += std::min<uint32_t>(step, 0x3FFF);
        while ((v.counter >> 12) >= 28) {
            v.counter -= 28 << 12;
            v.last = v.decoded[27];
            // end of block: follow loop flags
            if (v.flags & 1) {
                g_endx |= 1u << n;
                if (v.flags & 2) {
                    v.addr = v.repeat;
                } else {
                    v.phase = Off;
                    v.level = 0;
                    break;
                }
            } else {
                v.addr = (v.addr + 16) & (SPU_RAM - 1);
            }
            decode_block(v);
        }
        (void)prev_voice_out;
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
        l += (cdl * (int16_t)reg(0x1B0)) >> 15;
        r += (cdr * (int16_t)reg(0x1B2)) >> 15;
    }

    if (!(cnt & 0x4000)) { l = 0; r = 0; }  // muted
    l = (l * vol_fixed(reg(0x180))) >> 15;
    r = (r * vol_fixed(reg(0x182))) >> 15;
    out_l = std::clamp(l, -32768, 32767);
    out_r = std::clamp(r, -32768, 32767);
}

}  // namespace

void spu_init() {
    std::memset(g_ram, 0, sizeof g_ram);
    std::memset(g_regs, 0, sizeof g_regs);
    for (auto& v : g_voice) v = Voice{};
    g_cd_fifo.clear();
}

uint16_t spu_read(uint32_t addr) {
    uint32_t off = addr - 0x1F801C00;
    if (off < 0x180 && (off & 0xF) == 0xC) return (uint16_t)g_voice[off >> 4].level;
    switch (off) {
        case 0x19C: return (uint16_t)g_endx;
        case 0x19E: return (uint16_t)(g_endx >> 16);
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
    uint32_t off = addr - 0x1F801C00;
    reg(off) = v;
    switch (off) {
        case 0x188: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_on(i); break;
        case 0x18A: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_on(16 + i); break;
        case 0x18C: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_off(i); break;
        case 0x18E: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_off(16 + i); break;
        case 0x1A6: g_transfer_addr = (uint32_t)v * 8; break;
        case 0x1A8:  // manual transfer FIFO
            check_irq(g_transfer_addr);
            g_ram[g_transfer_addr & (SPU_RAM - 1)] = (uint8_t)v;
            g_ram[(g_transfer_addr + 1) & (SPU_RAM - 1)] = (uint8_t)(v >> 8);
            g_transfer_addr = (g_transfer_addr + 2) & (SPU_RAM - 1);
            break;
        case 0x1AA:
            if (!(v & 0x40)) g_irq_flag = false;  // disabling IRQ acknowledges it
            break;
        default:
            if (off < 0x180 && (off & 0xF) == 0xE) {  // repeat address written by software
                g_voice[off >> 4].repeat = (uint32_t)v * 8;
            }
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

void spu_end_frame(R3000* c) {
    uint64_t now = c->cycles;
    g_sample_accum_cycles += now - g_last_cycles;
    g_last_cycles = now;
    int frames = (int)(g_sample_accum_cycles * 44100 / PSX_CPU_HZ);
    g_sample_accum_cycles -= (uint64_t)frames * PSX_CPU_HZ / 44100;
    if (frames <= 0) return;
    frames = std::min(frames, 4096);
    std::vector<int16_t> buf((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        int32_t l, r;
        mix_sample(l, r);
        buf[(size_t)i * 2] = (int16_t)l;
        buf[(size_t)i * 2 + 1] = (int16_t)r;
    }
    host_audio_push(buf.data(), frames);
}
