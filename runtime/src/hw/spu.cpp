// SPU: 24 ADPCM voices with ADSR envelopes, volume sweeps, pitch modulation, noise,
// reverb and CD audio input. Samples are produced in emulated time (caught up on every
// register access and every scanline) and handed to the host once per frame.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "core/psx.h"
#include "core/state_io.h"
#include "game/game.h"
#include "host/host.h"

int g_sound_filter = 1;  // host.h

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
// A voice's samples with more history and a look ahead, for Sharp (not saved in states: rebuilt
// as the voice plays): ext[e] = buf[e - 3] for buf indices -3..34 (31..34: the next block's first).
int16_t g_ext[24][38];
bool g_ext_ok[24];
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
int32_t g_rev_hist[4][2] = {};  // the last four reverb outputs (not saved in states)

// CD audio input, resampled to 44.1 kHz stereo
std::deque<int16_t> g_cd_fifo;
double g_cd_phase = 0;
int16_t g_cd_prev[2] = {};
int16_t g_cd_hist[16][2] = {};  // the last 16 input frames (resample_into; not saved in states)
// A voice line played beside the drive (cdrom.cpp g_side), mixed over the CD input
std::deque<int16_t> g_side_fifo;
double g_side_phase = 0;
int16_t g_side_prev[2] = {};
int16_t g_side_hist[16][2] = {};

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

// The first n samples of the ADPCM block at b, with its filter history prev (updated).
void decode_samples(const uint8_t* b, int16_t prev[2], int16_t* out, int n) {
    int shift = b[0] & 0xF;
    if (shift > 12) shift = 9;
    int filter = std::min((b[0] >> 4) & 7, 4);
    for (int i = 0; i < n; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t t = (int32_t)((uint32_t)nib << 28) >> 28;
        int32_t s = ((t << 12) >> shift) + ((prev[0] * kPos[filter] + prev[1] * kNeg[filter] + 32) >> 6);
        s = std::clamp(s, -32768, 32767);
        prev[1] = prev[0];
        prev[0] = (int16_t)s;
        out[i] = (int16_t)s;
    }
}

void decode_block(Voice& v) {
    const uint8_t* b = &g_ram[v.addr & (SPU_RAM - 1)];
    check_irq(v.addr);
    v.flags = b[1];
    if (v.flags & 4) v.repeat = v.addr;  // loop start
    decode_samples(b, v.prev, v.buf + 3, 28);
}

// Sharp: the voice's longer history after a block was decoded (fresh: a key on, no history), and
// the first samples of the block that comes next (where the loop flags say; none after the end).
void ext_update(int n, bool fresh) {
    const Voice& v = g_voice[n];
    int16_t* e = g_ext[n];
    if (fresh) std::memset(e, 0, 6 * sizeof(int16_t));
    else std::memmove(e, e + 28, 6 * sizeof(int16_t));  // the last block's buf[25..30] -> buf[-3..2]
    std::memcpy(e + 6, v.buf + 3, 28 * sizeof(int16_t));
    int16_t prev[2] = {v.prev[0], v.prev[1]};
    if (!(v.flags & 1)) decode_samples(&g_ram[(v.addr + 16) & (SPU_RAM - 1)], prev, e + 34, 4);
    else if (v.flags & 2) decode_samples(&g_ram[v.repeat & (SPU_RAM - 1)], prev, e + 34, 4);
    else std::memset(e + 34, 0, 4 * sizeof(int16_t));
    g_ext_ok[n] = true;
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
    ext_update(n, true);
}

void key_off(int n) {
    if (g_voice[n].phase != Off) g_voice[n].phase = Release;
}

// The voices' interpolation (Recomp Enhancements -> Sound filter), between p1 and p2 of the
// samples around the pitch counter (frac: 0..4095 of the way):
//   0 Original: the hardware's 4-point Gaussian (its table, psx-spx), a little muffled;
//   1 Smooth: 4-point cubic (Catmull-Rom);
//   2 Sharp: 8-point windowed sinc (Lanczos, a = 4), from a longer history of the voice (g_ext).
const int16_t kGauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963
};

int32_t gauss(int32_t p0, int32_t p1, int32_t p2, int32_t p3, int32_t frac) {
    const int i = frac >> 4;
    return ((kGauss[0xFF - i] * p0) >> 15) + ((kGauss[0x1FF - i] * p1) >> 15) + ((kGauss[0x100 + i] * p2) >> 15) +
           ((kGauss[i] * p3) >> 15);
}

// Windowed-sinc weights for `taps` points around the position (fraction 0..1 of the way from point
// taps/2 - 1 to point taps/2), 256 phases, each set summing to 1.
template <int taps>
struct SincTable {
    float w[256][taps];
    SincTable() {
        constexpr double a = taps / 2, pi = 3.14159265358979323846;
        for (int i = 0; i < 256; i++) {
            double sum = 0;
            for (int k = 0; k < taps; k++) {
                const double x = (k - (taps / 2 - 1)) - i / 256.0;
                const double s = x == 0 ? 1 : std::sin(pi * x) / (pi * x) * std::sin(pi * x / a) / (pi * x / a);
                w[i][k] = (float)s;
                sum += s;
            }
            for (int k = 0; k < taps; k++) w[i][k] = (float)(w[i][k] / sum);
        }
    }
};
const SincTable<8> kSinc8;    // voices (Sharp)
const SincTable<16> kSinc16;  // CD audio, speech and streamed music (resample_into)

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
        int32_t sample;
        if (g_sound_filter == 0) {
            sample = gauss(v.buf[idx], v.buf[idx + 1], v.buf[idx + 2], v.buf[idx + 3], frac);
        } else if (g_sound_filter == 2 && g_ext_ok[n]) {  // ext[idx + 1 .. idx + 8] = buf[idx - 2 .. idx + 5]
            const float* w = kSinc8.w[frac >> 4];
            float s = 0;
            for (int k = 0; k < 8; k++) s += w[k] * g_ext[n][idx + 1 + k];
            sample = (int32_t)std::lround(s);
        } else {
            sample = cubic(v.buf[idx], v.buf[idx + 1], v.buf[idx + 2], v.buf[idx + 3], frac);
        }
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
            ext_update(n, false);
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
    if (movie_replacing()) {  // a replacement movie's sound instead of the disc movie's (movie.cpp)
        int16_t ml = 0, mr = 0;
        if (movie_audio(&ml, &mr)) cdl = ml, cdr = mr;
        else cdl = cdr = 0;
    }
    if (g_side_fifo.size() >= 2) {
        cdl = std::clamp(cdl + g_side_fifo[0], -32768, 32767);
        cdr = std::clamp(cdr + g_side_fifo[1], -32768, 32767);
        g_side_fifo.pop_front();
        g_side_fifo.pop_front();
    }
    if (cnt & 1) {
        int32_t cl = mul15(cdl, (int16_t)reg(0x1B0)), cr = mul15(cdr, (int16_t)reg(0x1B2));
        l += cl;
        r += cr;
        if (cnt & 4) { rev_l += cl; rev_r += cr; }
    }

    // Reverb runs at 22.05 kHz: between its steps the output is interpolated (cubic, a step and a
    // half behind) instead of each one held for two samples, which left a grainy edge on the tails.
    if (!g_rev_odd) {
        reverb_step(sat16(rev_l), sat16(rev_r));
        std::memmove(g_rev_hist[0], g_rev_hist[1], 3 * sizeof g_rev_hist[0]);
        g_rev_hist[3][0] = g_rev_out[0], g_rev_hist[3][1] = g_rev_out[1];
    }
    if (cnt & 0x80) {
        for (int ch = 0; ch < 2; ch++) {
            const int32_t v = g_rev_odd ? g_rev_hist[2][ch]
                                        : cubic(g_rev_hist[0][ch], g_rev_hist[1][ch], g_rev_hist[2][ch], g_rev_hist[3][ch], 2048);
            (ch ? r : l) += v;
        }
    }
    g_rev_odd = !g_rev_odd;

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
    g_side_fifo.clear();
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

namespace {
// To 44.1 kHz with a 16-point windowed sinc (speech and music at 37.8 / 18.9 kHz come out clean,
// without the harsh top a straight line between samples gave), 8 input frames behind.
void resample_into(std::deque<int16_t>& fifo, double& phase, int16_t prev[2], int16_t hist[16][2], const int16_t* stereo,
                   int frames, int rate) {
    catch_up();
    double step = (double)rate / 44100.0;
    for (int i = 0; i < frames; i++) {
        std::memmove(hist[0], hist[1], 15 * sizeof hist[0]);
        hist[15][0] = stereo[i * 2], hist[15][1] = stereo[i * 2 + 1];
        while (phase < 1.0) {  // between hist[7] and hist[8]
            const float* w = kSinc16.w[std::min(255, (int)(phase * 256.0))];
            for (int ch = 0; ch < 2; ch++) {
                float s = 0;
                for (int k = 0; k < 16; k++) s += w[k] * hist[k][ch];
                fifo.push_back(sat16((int32_t)std::lround(s)));
            }
            phase += step;
        }
        phase -= 1.0;
        prev[0] = hist[15][0];
        prev[1] = hist[15][1];
    }
    // keep latency bounded if the game streams faster than it plays
    const size_t max_samples = 44100 * 2;
    while (fifo.size() > max_samples) fifo.pop_front();
}
}  // namespace

void spu_push_cd_audio(const int16_t* stereo, int frames, int rate) {
    resample_into(g_cd_fifo, g_cd_phase, g_cd_prev, g_cd_hist, stereo, frames, rate);
}
void spu_push_side_audio(const int16_t* stereo, int frames, int rate) {
    resample_into(g_side_fifo, g_side_phase, g_side_prev, g_side_hist, stereo, frames, rate);
}

size_t spu_cd_queued() { return g_cd_fifo.size() / 2; }
size_t spu_side_queued() { return g_side_fifo.size() / 2; }

void spu_scanline() { catch_up(); }

void spu_end_frame(R3000* c) {
    (void)c;
    cdrom_voice_poll();
    catch_up();
    if (!g_out.empty()) host_audio_push(g_out.data(), (int)(g_out.size() / 2));
    g_out.clear();
}

// ================================================================ save states

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
    if (s.loading()) {
        g_out.clear(), g_side_fifo.clear();
        std::fill(std::begin(g_ext_ok), std::end(g_ext_ok), false);  // (Sharp: cubic until each voice's next key on)
    }
}
