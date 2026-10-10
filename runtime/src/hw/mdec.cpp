// MDEC: macroblock decoder used by the STR cutscenes.
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

#include "core/psx.h"
#include "core/state_io.h"

namespace {

const uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

uint8_t g_qt_y[64], g_qt_c[64];
int16_t g_scale[64];

uint32_t g_cmd = 0;
std::vector<uint16_t> g_in;     // halfwords of the current command's payload
uint32_t g_in_words_left = 0;
enum class Mode { Idle, Decode, Quant, Scale } g_mode = Mode::Idle;
bool g_quant_color = false;

std::deque<uint32_t> g_out;
bool g_en_in = false, g_en_out = false;

int signed10(uint16_t v) { return (int)((uint32_t)v << 22) >> 22; }

// Returns false when the input ran out before the block finished.
bool rl_decode(size_t& pos, int16_t* blk, const uint8_t* qt) {
    std::memset(blk, 0, 64 * sizeof(int16_t));
    while (pos < g_in.size() && g_in[pos] == 0xFE00) pos++;
    if (pos >= g_in.size()) return false;
    uint16_t n = g_in[pos++];
    int q_scale = (n >> 10) & 0x3F;
    int k = 0;
    int val = signed10(n) * qt[0];
    for (;;) {
        if (q_scale == 0) val = signed10(n) * 2;
        val = std::clamp(val, -0x400, 0x3FF);
        if (q_scale > 0) blk[kZigzag[k]] = (int16_t)val;
        else blk[k] = (int16_t)val;
        if (pos >= g_in.size()) return true;
        n = g_in[pos++];
        k += (n >> 10) + 1;
        if (k > 63) break;  // 0xFE00 (end of block) lands here too
        val = (signed10(n) * qt[k] * q_scale + 4) / 8;
    }
    return true;
}

void idct(int16_t* blk) {
    int32_t tmp[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += (int64_t)blk[y + z * 8] * (g_scale[x + z * 8] >> 3);
            tmp[x + y * 8] = (int32_t)((sum + 0xFFF) >> 13);
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += (int64_t)tmp[y + z * 8] * (g_scale[x + z * 8] >> 3);
            blk[x + y * 8] = (int16_t)std::clamp<int64_t>((sum + 0xFFF) >> 13, -128, 127);
        }
}

void emit_pixels_color(const int16_t* cr, const int16_t* cb, const int16_t* y4[4]) {
    int depth = (g_cmd >> 27) & 3;  // 2 = 24-bit, 3 = 15-bit
    bool is_signed = (g_cmd >> 26) & 1;
    bool bit15 = (g_cmd >> 25) & 1;
    uint8_t rgb[16 * 16 * 3];
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            const int16_t* yb = y4[(py >> 3) * 2 + (px >> 3)];
            int Y = yb[(py & 7) * 8 + (px & 7)];
            int ci = (py >> 1) * 8 + (px >> 1);
            int Cr = cr[ci], Cb = cb[ci];
            int R = Y + ((1435 * Cr) >> 10);
            int G = Y - ((352 * Cb + 731 * Cr) >> 10);
            int B = Y + ((1814 * Cb) >> 10);
            R = std::clamp(R, -128, 127);
            G = std::clamp(G, -128, 127);
            B = std::clamp(B, -128, 127);
            if (!is_signed) { R ^= 0x80; G ^= 0x80; B ^= 0x80; }
            uint8_t* p = &rgb[(py * 16 + px) * 3];
            p[0] = (uint8_t)R; p[1] = (uint8_t)G; p[2] = (uint8_t)B;
        }
    }
    if (depth == 2) {
        const uint8_t* p = rgb;
        for (int i = 0; i < 16 * 16 * 3; i += 4)
            g_out.push_back(p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24);
    } else {
        for (int i = 0; i < 256; i += 2) {
            uint32_t w = 0;
            for (int k = 0; k < 2; k++) {
                const uint8_t* p = &rgb[(i + k) * 3];
                uint16_t c = (uint16_t)((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | (bit15 ? 0x8000 : 0));
                w |= (uint32_t)c << (16 * k);
            }
            g_out.push_back(w);
        }
    }
}

void emit_pixels_mono(const int16_t* y) {
    int depth = (g_cmd >> 27) & 3;  // 0 = 4-bit, 1 = 8-bit
    bool is_signed = (g_cmd >> 26) & 1;
    uint8_t px[64];
    for (int i = 0; i < 64; i++) px[i] = (uint8_t)(is_signed ? y[i] : (y[i] ^ 0x80));
    if (depth == 1) {
        for (int i = 0; i < 64; i += 4) g_out.push_back(px[i] | px[i + 1] << 8 | px[i + 2] << 16 | (uint32_t)px[i + 3] << 24);
    } else {
        for (int i = 0; i < 64; i += 8) {
            uint32_t w = 0;
            for (int k = 0; k < 8; k++) w |= (uint32_t)(px[i + k] >> 4) << (4 * k);
            g_out.push_back(w);
        }
    }
}

void run_decode() {
    gpu_note_movie_frame();
    int depth = (g_cmd >> 27) & 3;
    size_t pos = 0;
    int16_t cr[64], cb[64], y[4][64];
    for (;;) {
        if (depth >= 2) {
            if (!rl_decode(pos, cr, g_qt_c)) break;
            if (!rl_decode(pos, cb, g_qt_c)) break;
            bool ok = true;
            for (int i = 0; i < 4 && ok; i++) ok = rl_decode(pos, y[i], g_qt_y);
            if (!ok) break;
            idct(cr); idct(cb);
            for (auto& b : y) idct(b);
            const int16_t* yp[4] = {y[0], y[1], y[2], y[3]};
            emit_pixels_color(cr, cb, yp);
        } else {
            if (!rl_decode(pos, y[0], g_qt_y)) break;
            idct(y[0]);
            emit_pixels_mono(y[0]);
        }
    }
}

void finish_command() {
    switch (g_mode) {
        case Mode::Decode: run_decode(); break;
        case Mode::Quant: {
            const uint8_t* b = (const uint8_t*)g_in.data();
            std::memcpy(g_qt_y, b, 64);
            if (g_quant_color) std::memcpy(g_qt_c, b + 64, 64);
            break;
        }
        case Mode::Scale: std::memcpy(g_scale, g_in.data(), 128); break;
        default: break;
    }
    g_mode = Mode::Idle;
    g_in.clear();
}

void write_command_word(uint32_t v) {
    if (g_mode != Mode::Idle) {
        g_in.push_back((uint16_t)v);
        g_in.push_back((uint16_t)(v >> 16));
        if (--g_in_words_left == 0) finish_command();
        return;
    }
    g_cmd = v;
    g_in.clear();
    switch (v >> 29) {
        case 1:
            g_mode = Mode::Decode;
            g_in_words_left = v & 0xFFFF;
            break;
        case 2:
            g_mode = Mode::Quant;
            g_quant_color = v & 1;
            g_in_words_left = g_quant_color ? 32 : 16;
            break;
        case 3:
            g_mode = Mode::Scale;
            g_in_words_left = 32;
            break;
        default:
            g_in_words_left = 0;
            break;
    }
    if (g_mode != Mode::Idle && g_in_words_left == 0) finish_command();
}

}  // namespace

void mdec_init() {
    g_mode = Mode::Idle;
    g_in.clear();
    g_out.clear();
}

uint32_t mdec_read(uint32_t reg) {
    if (reg == 0) return mdec_dma_out();
    uint32_t s = 0;
    if (g_out.empty()) s |= 1u << 31;
    if (g_mode != Mode::Idle) s |= 1u << 29;
    if (g_en_in && g_mode != Mode::Idle) s |= 1u << 28;
    if (g_en_out && !g_out.empty()) s |= 1u << 27;
    s |= ((g_cmd >> 25) & 0xF) << 23;
    s |= (g_mode != Mode::Idle ? (g_in_words_left - 1) : 0xFFFF) & 0xFFFF;
    return s;
}

void mdec_write(uint32_t reg, uint32_t v) {
    if (reg == 0) {
        write_command_word(v);
        return;
    }
    if (v & 0x80000000u) {
        g_mode = Mode::Idle;
        g_in.clear();
        g_out.clear();
    }
    g_en_in = (v >> 30) & 1;
    g_en_out = (v >> 29) & 1;
}

void mdec_dma_in(uint32_t w) { write_command_word(w); }

uint32_t mdec_dma_out() {
    if (g_out.empty()) return 0;
    uint32_t w = g_out.front();
    g_out.pop_front();
    return w;
}

bool mdec_out_ready() { return !g_out.empty(); }

// ================================================================ save states

void mdec_state(StateIO& s) {
    s.marker(0x4D444543);  // "MDEC"
    s.raw(g_qt_y, sizeof g_qt_y);
    s.raw(g_qt_c, sizeof g_qt_c);
    s.raw(g_scale, sizeof g_scale);
    s(g_cmd);
    s.vec(g_in);
    s(g_in_words_left);
    s(g_mode);
    s(g_quant_color);
    s.deq(g_out);
    s(g_en_in);
    s(g_en_out);
}
