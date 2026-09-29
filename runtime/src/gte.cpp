// Geometry Transformation Engine (COP2), bit-accurate per the psx-spx documentation.
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "psx.h"

namespace {

R3000* C;  // current context, set on every entry point (single-threaded)

}  // namespace

float g_gte_widescreen = 1.0f;

// Pose blending (sim60.cpp): one render records every transformed vertex and output colour,
// a second render of the same scene replaces each with the average of the two. The two
// renders differ only in animation state, but that changes how much work a model does (faces
// turned away get no lighting, near polygons are split, a model posed off screen is skipped),
// so the streams are not paired by one running index: they are cut into segments, one per
// load of the GTE translation (each model, each jointed part, each lighting pass), and each
// segment is paired with the next recorded one whose translation is the same or close (a
// part posed differently sits a little elsewhere), looking a few segments ahead; inside a
// segment the n-th vertex or colour goes with the n-th. A count that differs then only upsets
// that one segment instead of every vertex and colour after it (colours were a few apart on
// most frames, so the cave lighting and the models drawn after an animated one were averaged
// with the wrong entries). A pair that is not the same vertex is left alone.
GteBlendMode g_gte_blend = GteBlendMode::Off;
static std::vector<GteBlendVtx> g_blend_vtx;
static std::vector<uint32_t> g_blend_rgb;
uint32_t g_blend_stats[3];  // vertices blended, vertices skipped, colours blended

namespace {
struct BlendSeg {
    int32_t tr[3];
    uint32_t ra;  // the code that drew it (its return address)
    uint32_t v0, v1, c0, c1;  // recorded vertices [v0, v1), colours [c0, c1)
};
constexpr uint32_t kSegLookAhead = 24;
constexpr int64_t kSegNear = 512;  // largest distance (sum of |dx|, |dy|, |dz|) of paired translations
std::vector<BlendSeg> g_blend_segs;  // recorded
bool g_tr_loaded = true;             // the translation was loaded since the current segment began
const BlendSeg* g_seg_rec = nullptr;  // blend pass: the recorded segment paired with the current one
bool g_seg_near = false;               // blend pass: paired by a nearby translation, not the same one
uint32_t g_seg_next = 0;              // blend pass: the first recorded segment not paired yet
uint32_t g_seg_misses = 0;            // blend pass: segments in a row left unpaired
uint32_t g_seg_vi = 0, g_seg_ci = 0;  // blend pass: vertices and colours so far in the current segment
uint32_t g_seg_stats[3];  // APOC_BLEND_DEBUG: segments paired, paired with a different count, unpaired
bool g_seg_open = false;
void seg_close();
}  // namespace

void gte_blend_begin(GteBlendMode m) {
    static const bool dbg = std::getenv("APOC_BLEND_DEBUG") != nullptr;
    if (dbg && g_gte_blend == GteBlendMode::Blend) {
        seg_close();
        static int n = 0;
        if (++n % 60 == 0)
            std::fprintf(stderr, "BLENDDBG segments paired %u, count differs %u, unpaired %u\n", g_seg_stats[0],
                         g_seg_stats[1], g_seg_stats[2]);
    }
    g_gte_blend = m;
    if (m == GteBlendMode::Record) {
        g_blend_vtx.clear();
        g_blend_rgb.clear();
        g_blend_segs.clear();
    }
    g_tr_loaded = true;
    g_seg_open = false;
    g_seg_rec = nullptr;
    g_seg_next = 0;
    g_seg_misses = 0;
}

// Precise vertices (F1 -> Precise 3D). The GTE hands the game whole-pixel screen positions,
// so vertices jump from pixel to pixel as things move, and the GPU maps textures without
// perspective; both make textures wobble. Each projection's exact position and depth is kept
// here under the whole-pixel position it produced (as a GPU vertex word packs it: 11-bit x
// and y); the GPU looks its polygons' vertices up (gpu.cpp). Cleared after each frame's
// ordering table is drawn.
bool g_gte_precise = true;

namespace {

constexpr int kPreciseSlots = 1 << 15;  // power of two; a busy frame projects ~10,000 vertices
struct PreciseSlot {
    uint32_t key, gen;
    GtePrecise p;
};
PreciseSlot g_precise[kPreciseSlots];
}  // namespace
uint32_t g_precise_stats[8];  // polygons with every / some / no vertex precise, depth clashes; projections, too near, off; corners by shadow
namespace {
uint32_t g_precise_gen = 1, g_precise_seq = 0;
int g_precise_used = 0;

uint32_t precise_key(uint32_t xy) { return (xy & 0x7FF) | (xy & 0x7FF0000); }
uint32_t precise_hash(uint32_t k) { return (k * 0x9E3779B1u) >> 17; }

void precise_put(uint32_t sxy, float x, float y, float w) {
    if (g_precise_used >= kPreciseSlots * 3 / 4) return;
    uint32_t k = precise_key(sxy);
    const uint32_t seq = ++g_precise_seq;
    bool clash = false;
    for (uint32_t i = precise_hash(k);; i = (i + 1) & (kPreciseSlots - 1)) {
        PreciseSlot& s = g_precise[i];
        if (s.gen != g_precise_gen) {
            s = {k, g_precise_gen, {x, y, w, seq}};
            g_precise_used++;
            if (clash) g_precise_stats[3]++;
            return;
        }
        if (s.key != k) continue;
        // the same vertex projected again (a shared vertex, a model drawn twice): the latest
        if (std::abs(s.p.w - w) <= s.p.w * 0.002f && std::abs(s.p.x - x) < 0.05f && std::abs(s.p.y - y) < 0.05f) {
            s.p = {x, y, w, seq};
            return;
        }
        clash = true;  // a different vertex on the same pixel: kept as well
    }
}

}  // namespace

int gte_precise_find(uint32_t xy, GtePrecise* out, int max) {
    uint32_t k = precise_key(xy);
    int n = 0;
    for (uint32_t i = precise_hash(k); n < max; i = (i + 1) & (kPreciseSlots - 1)) {
        const PreciseSlot& s = g_precise[i];
        if (s.gen != g_precise_gen) break;
        if (s.key == k) out[n++] = s.p;
    }
    return n;
}

// PGXP shadows (recomp.h): the exact vertex behind each RAM/scratchpad word, CPU register and
// SXY FIFO entry, carried by the generated code from the projection to the GPU packet. Unlike
// the table above, this knows which vertex a word came from even when several share a pixel.
extern "C" {
PgxpVal pgxp_gpr[32];
PgxpVal pgxp_ram[PSX_RAM_SIZE / 4];
PgxpVal pgxp_scratch[PSX_SCRATCH_SIZE / 4];
}
namespace {
PgxpVal g_pgxp_sxy[3];  // SXY0..2

PgxpVal& pgxp_sxy(int reg) { return g_pgxp_sxy[reg == 15 ? 2 : reg - 12]; }
void pgxp_push(const PgxpVal& v) {
    g_pgxp_sxy[0] = g_pgxp_sxy[1];
    g_pgxp_sxy[1] = g_pgxp_sxy[2];
    g_pgxp_sxy[2] = v;
}
}  // namespace

extern "C" void pgxp_gte_to_gpr(int gpr, int reg) { pgxp_gpr[gpr] = pgxp_sxy(reg); }
extern "C" void pgxp_gpr_to_gte(int reg, int gpr) {
    if (reg == 15) pgxp_push(pgxp_gpr[gpr]);
    else pgxp_sxy(reg) = pgxp_gpr[gpr];
}
extern "C" void pgxp_gte_to_mem(int reg, uint32_t a) {
    if (PgxpVal* m = PGXP_MEM(a)) *m = pgxp_sxy(reg);
}
extern "C" void pgxp_mem_to_gte(int reg, uint32_t a) {
    const PgxpVal* m = PGXP_MEM(a);
    const PgxpVal v = m ? *m : PgxpVal{};
    if (reg == 15) pgxp_push(v);
    else pgxp_sxy(reg) = v;
}

// Exact view-space corners (PGXP "CPU mode" for this game's level renderer). The level pass
// (80080A20...) projects each floor/wall corner, then keeps its view position as the GTE's
// whole units (IR1..3, the fraction cut off) and, for near polygons, loads three such corners
// as the columns of the rotation matrix and projects weighted mixes of them (the subdivision
// grid, V0 = weights). Built from whole-unit corners, every grid vertex was off by up to a
// unit in view space, ~2 px near the camera and different for each corner: the textures of
// near floors and walls swam even with exact projection. Every projection's exact view
// position is kept here under the IR triple the game gets, and rtp() rebuilds a grid vertex
// from its exact corners.
namespace {
constexpr int kCornerSlots = 1 << 14;
struct CornerSlot {
    uint64_t key;  // IR1 | IR2 << 16 | IR3 << 32 (16 bits each)
    uint32_t gen, seq;
    double v[3];   // exact view position, whole units
};
CornerSlot g_corner[kCornerSlots];
uint32_t g_corner_gen = 1, g_corner_seq = 0;
int g_corner_used = 0;
uint32_t corner_hash(uint64_t k) { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 50); }
uint64_t corner_key(int32_t a, int32_t b, int32_t c) {
    return (uint64_t)(uint16_t)a | (uint64_t)(uint16_t)b << 16 | (uint64_t)(uint16_t)c << 32;
}
void corner_put(uint64_t k, const double* v) {
    if (g_corner_used >= kCornerSlots * 3 / 4) return;
    for (uint32_t i = corner_hash(k);; i = (i + 1) & (kCornerSlots - 1)) {
        CornerSlot& s = g_corner[i];
        if (s.gen != g_corner_gen) {
            g_corner_used++;
        } else if (s.key != k) {
            continue;
        }
        s = {k, g_corner_gen, ++g_corner_seq, {v[0], v[1], v[2]}};  // the latest of one IR triple
        return;
    }
}
const CornerSlot* corner_find(uint64_t k) {
    for (uint32_t i = corner_hash(k);; i = (i + 1) & (kCornerSlots - 1)) {
        const CornerSlot& s = g_corner[i];
        if (s.gen != g_corner_gen) return nullptr;
        if (s.key == k) return &s;
    }
}
}  // namespace
uint32_t g_corner_stats[2];  // grid vertices rebuilt, corner sets rejected

// The exact vertex for a GPU vertex word read from RAM address `a` (0: not from RAM). The
// shadow counts when its 11-bit x and y are those of the word or one pixel off (code that
// packs flags into SXY words, or nudges grid vertices, keeps the shadow of the original).
bool pgxp_find(uint32_t a, uint32_t word, GtePrecise* out) {
    if (!a) return false;
    const PgxpVal& s = pgxp_ram[(a & (PSX_RAM_SIZE - 1)) >> 2];
    if (s.w <= 0) return false;
    auto sx = [](uint32_t v) { return (int32_t)(v << 21) >> 21; };
    auto sy = [](uint32_t v) { return (int32_t)((v >> 16) << 21) >> 21; };
    const int dx = sx(word) - sx(s.v), dy = sy(word) - sy(s.v);
    if (dx < -1 || dx > 1 || dy < -1 || dy > 1) return false;
    *out = {s.x + (float)dx, s.y + (float)dy, s.w, 0};
    return true;
}

void gte_precise_clear() {
    if (g_corner_used) {
        g_corner_used = 0;
        if (++g_corner_gen == 0) {
            std::memset(g_corner, 0, sizeof g_corner);
            g_corner_gen = 1;
        }
    }
    if (g_precise_used == 0) return;
    g_precise_used = 0;
    if (++g_precise_gen == 0) {  // wrapped: stamps from 2^32 frames ago would look current
        std::memset(g_precise, 0, sizeof g_precise);
        g_precise_gen = 1;
    }
}

namespace {

// ---------------------------------------------------------------- register access

int32_t& D(int r) { return C->gte_data[r]; }
int32_t& K(int r) { return C->gte_ctrl[r]; }

int16_t lo16(int32_t v) { return (int16_t)(v & 0xFFFF); }
int16_t hi16(int32_t v) { return (int16_t)((uint32_t)v >> 16); }

// vectors
int16_t VX(int n) { return lo16(D(n * 2)); }
int16_t VY(int n) { return hi16(D(n * 2)); }
int16_t VZ(int n) { return lo16(D(n * 2 + 1)); }

int32_t& IR(int n) { return D(8 + n); }  // IR0..IR3 (stored sign-extended)
int32_t& MAC(int n) { return D(24 + n); }
uint32_t& FLAG() { return (uint32_t&)K(31); }

// matrices: 0 = rotation, 1 = light, 2 = light color
int16_t M(int m, int row, int col) {
    int base = m * 8;
    int idx = row * 3 + col;  // 0..8
    int32_t w = K(base + idx / 2);
    return (idx & 1) ? hi16(w) : lo16(w);
}

uint8_t rgbc(int n) { return (uint8_t)((uint32_t)D(6) >> (8 * n)); }  // R,G,B,CODE

// ---------------------------------------------------------------- flags / saturation

void set_flag(int bit) { FLAG() |= 1u << bit; }

int64_t chk_mac(int i, int64_t v) {
    if (v > 0x7FFFFFFFFFFLL) set_flag(31 - i);
    else if (v < -0x80000000000LL) set_flag(28 - i);
    return (v << 20) >> 20;  // wrap to 44 bits
}

int32_t chk_mac0(int64_t v) {
    if (v > 0x7FFFFFFFLL) set_flag(16);
    else if (v < -0x80000000LL) set_flag(15);
    return (int32_t)v;
}

int32_t lim_ir(int i, int32_t v, bool lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { set_flag(25 - i); return lo; }
    if (v > 0x7FFF) { set_flag(25 - i); return 0x7FFF; }
    return v;
}

void set_mac_ir(int i, int64_t v, int shift, bool lm) {
    MAC(i) = (int32_t)(chk_mac(i, v) >> shift);
    IR(i) = lim_ir(i, MAC(i), lm);
}

// APOC_BLEND_DEBUG: how the segment just finished was paired.
void seg_close() {
    if (g_gte_blend != GteBlendMode::Blend || !g_seg_open) return;
    if (!g_seg_rec) g_seg_stats[2]++;
    else if (g_seg_vi != g_seg_rec->v1 - g_seg_rec->v0 || g_seg_ci != g_seg_rec->c1 - g_seg_rec->c0) g_seg_stats[1]++;
    else g_seg_stats[0]++;
}
// The segment the next vertex or colour belongs to (see pose blending, above).
void blend_segment() {
    if (!g_tr_loaded) return;
    seg_close();
    g_tr_loaded = false;
    g_seg_open = true;
    const int32_t tr[3] = {K(5), K(6), K(7)};
    if (g_gte_blend == GteBlendMode::Record) {
        g_blend_segs.push_back({{tr[0], tr[1], tr[2]}, C->r[31], (uint32_t)g_blend_vtx.size(), (uint32_t)g_blend_vtx.size(),
                                (uint32_t)g_blend_rgb.size(), (uint32_t)g_blend_rgb.size()});
        return;
    }
    // Drawn by the same code (return address) and, looking ahead from the first segment not
    // paired yet: (1) a part at exactly the same place (a translation other than 0, which
    // lighting and level passes share, identifies it) up to kSegLookAhead ahead; (2) else one of
    // the next few at the same or a nearby place (a part posed a little elsewhere: level 2's
    // zombies compute their joints while drawing); (3) after a run of misses, the same
    // translation up to kSegLookAhead ahead, to find the thread again. A segment only one render
    // has (a lighting pass for a part turned away, a model split differently) stays unpaired.
    g_seg_rec = nullptr;
    g_seg_vi = g_seg_ci = 0;
    const uint32_t ra = C->r[31];
    const bool zero = !tr[0] && !tr[1] && !tr[2];
    auto dist = [&](const BlendSeg& r) {
        return std::abs((int64_t)r.tr[0] - tr[0]) + std::abs((int64_t)r.tr[1] - tr[1]) + std::abs((int64_t)r.tr[2] - tr[2]);
    };
    auto find = [&](uint32_t n, int64_t near) -> const BlendSeg* {
        const uint32_t end = std::min<uint32_t>((uint32_t)g_blend_segs.size(), g_seg_next + n);
        for (uint32_t j = g_seg_next; j < end; j++)
            if (g_blend_segs[j].ra == ra && dist(g_blend_segs[j]) <= near) return &g_blend_segs[j];
        return nullptr;
    };
    g_seg_near = false;
    if (!zero) g_seg_rec = find(kSegLookAhead, 0);
    if (!g_seg_rec && !zero && (g_seg_rec = find(4, kSegNear))) g_seg_near = true;
    if (!g_seg_rec) g_seg_rec = find(4, 0);
    if (!g_seg_rec && g_seg_misses >= 3) g_seg_rec = find(kSegLookAhead, 0);
    g_seg_misses = g_seg_rec ? 0 : g_seg_misses + 1;
    if (g_seg_rec) g_seg_next = (uint32_t)(g_seg_rec - g_blend_segs.data()) + 1;
}

uint8_t lim_color(int i, int32_t v) {
    if (v < 0) { set_flag(22 - i); return 0; }
    if (v > 0xFF) { set_flag(22 - i); return 0xFF; }
    return (uint8_t)v;
}

void push_color() {
    D(20) = D(21);
    D(21) = D(22);
    uint32_t r = lim_color(1, MAC(1) >> 4), g = lim_color(2, MAC(2) >> 4), b = lim_color(3, MAC(3) >> 4);
    D(22) = (int32_t)(r | g << 8 | b << 16 | (uint32_t)rgbc(3) << 24);
    if (g_gte_blend == GteBlendMode::Record) {
        blend_segment();
        g_blend_rgb.push_back((uint32_t)D(22));
        g_blend_segs.back().c1++;
    } else if (g_gte_blend == GteBlendMode::Blend) {
        blend_segment();
        const uint32_t i = g_seg_rec ? g_seg_rec->c0 + g_seg_ci++ : 0;
        if (g_seg_rec && i < g_seg_rec->c1) {
            uint32_t o = g_blend_rgb[i], n = (uint32_t)D(22), m = n & 0xFF000000u;
            bool near = true;  // a colour a pose apart; a very different one is another vertex's
            for (int s = 0; s < 24; s += 8) near &= std::abs((int)((o >> s) & 0xFF) - (int)((n >> s) & 0xFF)) <= 64;
            if (near) {
                for (int s = 0; s < 24; s += 8) m |= ((((o >> s) & 0xFF) + ((n >> s) & 0xFF) + 1) / 2) << s;
                D(22) = (int32_t)m;
                g_blend_stats[2]++;
            }
        }
    }
}

void push_sz(int32_t v) {
    if (v < 0) { set_flag(18); v = 0; }
    else if (v > 0xFFFF) { set_flag(18); v = 0xFFFF; }
    D(16) = D(17);
    D(17) = D(18);
    D(18) = D(19);
    D(19) = v;
}

void push_sxy(int32_t x, int32_t y) {
    if (x < -0x400) { set_flag(14); x = -0x400; } else if (x > 0x3FF) { set_flag(14); x = 0x3FF; }
    if (y < -0x400) { set_flag(13); y = -0x400; } else if (y > 0x3FF) { set_flag(13); y = 0x3FF; }
    D(12) = D(13);
    D(13) = D(14);
    D(14) = (int32_t)((uint32_t)(x & 0xFFFF) | ((uint32_t)y << 16));
}

// ---------------------------------------------------------------- UNR division

uint8_t g_unr[0x101];
bool g_unr_init = false;

void init_unr() {
    for (int i = 0; i < 0x101; i++) g_unr[i] = (uint8_t)std::max(0, (0x40000 / (i + 0x100) + 1) / 2 - 0x101);
    g_unr_init = true;
}

uint32_t gte_divide(uint32_t h, uint32_t sz3) {
    if (h >= sz3 * 2) {
        set_flag(17);
        return 0x1FFFF;
    }
    int z = 0;
    while (z < 16 && !(sz3 & (0x8000u >> z))) z++;
    uint64_t n = (uint64_t)h << z;
    uint32_t d = sz3 << z;
    uint32_t u = g_unr[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - (d * u)) >> 8;
    d = (0x0000080 + (d * u)) >> 8;
    uint64_t q = ((n * d) + 0x8000) >> 16;
    return (uint32_t)std::min<uint64_t>(q, 0x1FFFF);
}

// ---------------------------------------------------------------- operations

struct Cmd {
    int shift;  // 0 or 12
    bool lm;
    int mx, v, cv;
};

int64_t mul_vec_row(int i, int64_t t, int16_t m0, int16_t m1, int16_t m2, int32_t x, int32_t y, int32_t z) {
    int64_t s = chk_mac(i, t + (int64_t)m0 * x);
    s = chk_mac(i, s + (int64_t)m1 * y);
    return chk_mac(i, s + (int64_t)m2 * z);
}

void rtp(int n, const Cmd& c, bool last) {
    int32_t x = VX(n), y = VY(n), z = VZ(n);
    int64_t tr[3] = {K(5), K(6), K(7)};
    int64_t mac3_full = 0, view[3];
    for (int i = 1; i <= 3; i++) {
        int64_t v = mul_vec_row(i, tr[i - 1] << 12, M(0, i - 1, 0), M(0, i - 1, 1), M(0, i - 1, 2), x, y, z);
        view[i - 1] = v;
        if (i == 3) mac3_full = v;
        MAC(i) = (int32_t)(v >> c.shift);
    }
    IR(1) = lim_ir(1, MAC(1), c.lm);
    IR(2) = lim_ir(2, MAC(2), c.lm);
    // IR3 saturation flag comes from the unshifted MAC3 >> 12, independent of sf
    {
        int32_t m3 = MAC(3);
        int32_t lo = c.lm ? 0 : -0x8000;
        IR(3) = std::clamp(m3, lo, 0x7FFF);
        int64_t f = mac3_full >> 12;
        if (f < -0x8000 || f > 0x7FFF) set_flag(22);
    }
    push_sz((int32_t)(mac3_full >> 12));
    uint32_t h = (uint16_t)K(26);
    int64_t q = gte_divide(h, (uint32_t)D(19));
    int64_t sx_num = q * IR(1);
    if (g_gte_widescreen != 1.0f) sx_num = (int64_t)((double)sx_num * g_gte_widescreen);  // widescreen hack
    MAC(0) = chk_mac0(sx_num + K(24));
    MAC(0) = chk_mac0(q * IR(2) + K(25));
    push_sxy((int32_t)((sx_num + K(24)) >> 16), (int32_t)((q * IR(2) + K(25)) >> 16));
    pgxp_push({});  // filled in below
    // The exact projection: h * view x / view z, without the divider's rounding or the
    // whole-pixel result, kept when it agrees with the result the game got. Vertices close to the camera (the divider overflows) or off the screen (the result is
    // clamped to +-1024) still get their exact position when in front of the camera: big floor
    // and wall polygons reaching past the screen edge are the ones whose textures swam most.
    // The renderer draws them with w, so even huge positions come out right.
    // exact view position (x4096): a subdivision grid vertex is rebuilt from its exact corners
    double ev[3] = {(double)view[0], (double)view[1], (double)view[2]};
    bool rebuilt = false;
    if (g_gte_precise && c.shift == 12 && K(5) == 0 && K(6) == 0 && K(7) == 0) {
        const CornerSlot* cs[3];
        bool all = true;
        for (int j = 0; j < 3 && all; j++)
            all = (cs[j] = corner_find(corner_key(M(0, 0, j), M(0, 1, j), M(0, 2, j)))) != nullptr;
        // the three corners come from one polygon, projected together shortly before
        for (int j = 1; j < 3 && all; j++)
            all = (cs[j]->seq > cs[0]->seq ? cs[j]->seq - cs[0]->seq : cs[0]->seq - cs[j]->seq) < 4096;
        if (all) {
            const double w[3] = {(double)x, (double)y, (double)z};
            double r[3], wsum = 0;
            for (int j = 0; j < 3; j++) wsum += std::abs(w[j]);
            bool ok = wsum <= 3 * 4096.0;
            for (int i = 0; i < 3 && ok; i++) {
                r[i] = cs[0]->v[i] * w[0] + cs[1]->v[i] * w[1] + cs[2]->v[i] * w[2];
                // whole-unit corners are at most a unit off each: the rebuilt one stays close
                ok = std::abs(r[i] - (double)view[i]) <= wsum + 4096.0;
            }
            if (ok) {
                for (int i = 0; i < 3; i++) ev[i] = r[i];
                rebuilt = true;
                g_corner_stats[0]++;
            } else {
                g_corner_stats[1]++;
            }
        }
    }
    if (g_gte_precise && c.shift == 12) {
        const double u[3] = {ev[0] / 4096.0, ev[1] / 4096.0, ev[2] / 4096.0};
        corner_put(corner_key(IR(1), IR(2), IR(3)), u);
    }
    float px = 0, py = 0, pw = 0;
    if (g_gte_precise && ev[2] >= 4096) {
        const double ws = g_gte_widescreen;
        const double fx = (double)K(24) / 65536.0 + (double)h * ev[0] / ev[2] * ws;
        const double fy = (double)K(25) / 65536.0 + (double)h * ev[1] / ev[2];
        const int sx = lo16(D(14)), sy = hi16(D(14));
        const bool overflow = (int64_t)h * 4096 >= view[2] * 2;
        const bool clamp_x = (sx == 0x3FF && fx > 0x3FE) || (sx == -0x400 && fx < -0x3FF);
        const bool clamp_y = (sy == 0x3FF && fy > 0x3FE) || (sy == -0x400 && fy < -0x3FF);
        // The GTE divides whole units (IR = view / 4096, SZ = view z / 4096), and this game's views
        // are small (SZ 150..600 near the camera): its result is off the exact one by up to
        // h / SZ * (1 + |IR| / SZ) pixels, often 5 to 10. A tighter test turned exactly the near
        // floor and wall vertices away, which kept their polygons affine and snapped to pixels.
        const double sz = std::max<int32_t>(D(19), 1);
        const double err_x = (double)h * ws / sz * (1.0 + std::abs(IR(1)) / sz) + 1.5;
        const double err_y = (double)h / sz * (1.0 + std::abs(IR(2)) / sz) + 1.5;
        const bool near_x = std::abs(fx - sx) < err_x || clamp_x, near_y = std::abs(fy - sy) < err_y || clamp_y;
        if ((near_x && near_y) || overflow || rebuilt)
            px = (float)fx, py = (float)fy, pw = (float)(ev[2] / 4096.0);
        else
            g_precise_stats[6]++;
        if (overflow) g_precise_stats[5]++;
    } else {
        g_precise_stats[5]++;
    }
    g_precise_stats[4]++;
    if (g_gte_blend == GteBlendMode::Record) {
        blend_segment();
        g_blend_vtx.push_back({(int16_t)x, (int16_t)y, (int16_t)z, D(14), D(19), px, py, pw});
        g_blend_segs.back().v1++;
    } else if (g_gte_blend == GteBlendMode::Blend) {
        // Pair up with the recorded vertex: same model vertex (segmented models) or a nearby
        // one (keyframed meshes), and not far away on screen.
        blend_segment();
        const uint32_t i = g_seg_rec ? g_seg_rec->v0 + g_seg_vi++ : 0;
        const GteBlendVtx* r = g_seg_rec && i < g_seg_rec->v1 ? &g_blend_vtx[i] : nullptr;
        int ox = r ? lo16(r->sxy) : 0, oy = r ? hi16(r->sxy) : 0, nx = lo16(D(14)), ny = hi16(D(14));
        // A part paired only by a nearby translation (level 2's zombies pose their joints while
        // drawing) may be the neighbouring joint: it must be the very same model vertex, or a
        // hand averaged with an elbow stretches into a spike.
        const int dv = r ? std::abs(r->vx - x) + std::abs(r->vy - y) + std::abs(r->vz - z) : 0;
        if (r && (g_seg_near ? dv == 0 : dv < 512) &&
            std::abs(ox - nx) < 64 && std::abs(oy - ny) < 64) {
            D(14) = (int32_t)((uint32_t)(((ox + nx) / 2) & 0xFFFF) | ((uint32_t)((oy + ny) / 2) << 16));
            D(19) = (r->sz + D(19)) / 2;
            if (pw > 0 && r->pw > 0) px = (px + r->px) / 2, py = (py + r->py) / 2, pw = (pw + r->pw) / 2;
            else pw = 0;
            g_blend_stats[0]++;
        } else {
            g_blend_stats[1]++;
        }
    }
    g_pgxp_sxy[2] = {(uint32_t)D(14), px, py, pw};
    if (pw > 0 && g_gte_blend != GteBlendMode::Record) precise_put((uint32_t)D(14), px, py, pw);
    if (last) {
        int64_t dq = q * (int16_t)K(27) + K(28);
        MAC(0) = chk_mac0(dq);
        int32_t ir0 = (int32_t)(dq >> 12);
        if (ir0 < 0) { set_flag(12); ir0 = 0; } else if (ir0 > 0x1000) { set_flag(12); ir0 = 0x1000; }
        IR(0) = ir0;
    }
}

void mvmva_generic(int mx, int32_t x, int32_t y, int32_t z, int cv, const Cmd& c) {
    int64_t t[3] = {0, 0, 0};
    if (cv == 0) { t[0] = K(5); t[1] = K(6); t[2] = K(7); }
    else if (cv == 1) { t[0] = K(13); t[1] = K(14); t[2] = K(15); }
    else if (cv == 2) { t[0] = K(21); t[1] = K(22); t[2] = K(23); }
    for (int i = 1; i <= 3; i++) {
        int16_t m0, m1, m2;
        if (mx == 3) {  // "garbage" matrix
            int r = i - 1;
            if (r == 0) { m0 = (int16_t)(-(int)rgbc(0) << 4); m1 = (int16_t)(rgbc(0) << 4); m2 = (int16_t)IR(0); }
            else if (r == 1) { m0 = m1 = m2 = M(0, 0, 2); }
            else { m0 = m1 = m2 = M(0, 1, 1); }
        } else {
            m0 = M(mx, i - 1, 0); m1 = M(mx, i - 1, 1); m2 = M(mx, i - 1, 2);
        }
        int64_t v;
        if (cv == 2) {
            // hardware bug: the FC column only affects flags, result uses columns 2..3
            (void)chk_mac(i, (t[i - 1] << 12) + (int64_t)m0 * x);
            int32_t tmp = lim_ir(i, (int32_t)(chk_mac(i, (t[i - 1] << 12) + (int64_t)m0 * x) >> c.shift), false);
            (void)tmp;
            v = chk_mac(i, chk_mac(i, (int64_t)m1 * y) + (int64_t)m2 * z);
        } else {
            v = mul_vec_row(i, t[i - 1] << 12, m0, m1, m2, x, y, z);
        }
        MAC(i) = (int32_t)(v >> c.shift);
        IR(i) = lim_ir(i, MAC(i), c.lm);
    }
}

// MAC/IR = [BK] + LCM * (LLM * V)
void light_stage(int n, const Cmd& c) {
    int32_t x = n < 3 ? VX(n) : IR(1), y = n < 3 ? VY(n) : IR(2), z = n < 3 ? VZ(n) : IR(3);
    Cmd l = c;
    l.cv = 3;
    mvmva_generic(1, x, y, z, 3, l);
    mvmva_generic(2, IR(1), IR(2), IR(3), 1, c);
}

// in_mac: unshifted per-channel input; interpolates towards far color by IR0
void interpolate(int64_t m1, int64_t m2, int64_t m3, const Cmd& c) {
    int64_t in[3] = {m1, m2, m3};
    int64_t fc[3] = {K(21), K(22), K(23)};
    for (int i = 1; i <= 3; i++) {
        MAC(i) = (int32_t)(chk_mac(i, (fc[i - 1] << 12) - in[i - 1]) >> c.shift);
        IR(i) = lim_ir(i, MAC(i), false);
    }
    for (int i = 1; i <= 3; i++) {
        set_mac_ir(i, (int64_t)IR(i) * IR(0) + in[i - 1], c.shift, c.lm);
    }
}

void color_times_ir(int64_t* out) {  // (RGB * IR) << 4
    out[0] = ((int64_t)rgbc(0) * IR(1)) << 4;
    out[1] = ((int64_t)rgbc(1) * IR(2)) << 4;
    out[2] = ((int64_t)rgbc(2) * IR(3)) << 4;
}

void op_nc(int n, const Cmd& c, int kind) {
    // kind: 0 = NCS, 1 = NCCS, 2 = NCDS
    light_stage(n, c);
    if (kind == 0) {
        push_color();
        return;
    }
    int64_t m[3];
    color_times_ir(m);
    if (kind == 1) {
        for (int i = 1; i <= 3; i++) set_mac_ir(i, m[i - 1], c.shift, c.lm);
    } else {
        interpolate(m[0], m[1], m[2], c);
    }
    push_color();
}

}  // namespace

// ================================================================ register interface

extern uint64_t g_gte_dump_frame;
static int g_gte_dumped;

extern "C" uint32_t gte_read_data(R3000* c, int r) {
    C = c;
    switch (r) {
        case 1: case 3: case 5: case 8: case 9: case 10: case 11:
            return (uint32_t)(int32_t)(int16_t)c->gte_data[r];
        case 7: case 16: case 17: case 18: case 19:
            return (uint16_t)c->gte_data[r];
        case 15: return (uint32_t)c->gte_data[14];
        case 28: case 29: {
            auto col = [&](int i) { return (uint32_t)std::clamp(c->gte_data[8 + i] >> 7, 0, 0x1F); };
            return col(1) | col(2) << 5 | col(3) << 10;
        }
        default: return (uint32_t)c->gte_data[r];
    }
}

extern "C" void gte_write_data(R3000* c, int r, uint32_t v) {
    C = c;
    switch (r) {
        case 1: case 3: case 5: case 8: case 9: case 10: case 11:
            c->gte_data[r] = (int16_t)v; break;
        case 7: case 16: case 17: case 18: case 19:
            c->gte_data[r] = (uint16_t)v; break;
        case 15:
            c->gte_data[12] = c->gte_data[13];
            c->gte_data[13] = c->gte_data[14];
            c->gte_data[14] = (int32_t)v;
            break;
        case 28:
            c->gte_data[28] = (int32_t)(v & 0x7FFF);
            c->gte_data[9] = (int32_t)((v & 0x1F) << 7);
            c->gte_data[10] = (int32_t)(((v >> 5) & 0x1F) << 7);
            c->gte_data[11] = (int32_t)(((v >> 10) & 0x1F) << 7);
            break;
        case 29: break;  // ORGB is read-only
        case 30: {
            c->gte_data[30] = (int32_t)v;
            uint32_t x = (v & 0x80000000u) ? ~v : v;
            int n = 0;
            while (n < 32 && !(x & (0x80000000u >> n))) n++;
            c->gte_data[31] = n;
            break;
        }
        case 31: break;  // LZCR is read-only
        default: c->gte_data[r] = (int32_t)v; break;
    }
}

extern "C" uint32_t gte_read_ctrl(R3000* c, int r) {
    C = c;
    switch (r) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            return (uint32_t)(int32_t)(int16_t)c->gte_ctrl[r];
        default: return (uint32_t)c->gte_ctrl[r];
    }
}

extern "C" void gte_write_ctrl(R3000* c, int r, uint32_t v) {
    C = c;
    if (g_gte_dump_frame && host_frame_count() == g_gte_dump_frame && r <= 7 && g_gte_dumped < 12)
        log_msg(LogCat::Gpu, "ctc2 %d = %08X (ra=%08X a0=%08X)", r, v, c->r[31], c->r[4]);
    if (r >= 5 && r <= 7) g_tr_loaded = true;  // a new pose-blend segment
    switch (r) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            c->gte_ctrl[r] = (int16_t)v; break;
        case 31: {
            uint32_t f = v & 0x7FFFF000u;
            if (f & 0x7F87E000u) f |= 0x80000000u;
            c->gte_ctrl[31] = (int32_t)f;
            break;
        }
        default: c->gte_ctrl[r] = (int32_t)v; break;
    }
}

// ================================================================ commands

uint32_t g_gte_counts[64];  // per-command usage, reported by gte_frame_stats()
uint32_t g_nclip_pos, g_nclip_neg, g_nclip_zero, g_rtp_err, g_rtp_divov;

void gte_frame_stats() {
    static const char* names[64] = {};
    names[0x01] = "RTPS"; names[0x06] = "NCLIP"; names[0x0C] = "OP"; names[0x10] = "DPCS";
    names[0x11] = "INTPL"; names[0x12] = "MVMVA"; names[0x13] = "NCDS"; names[0x14] = "CDP";
    names[0x16] = "NCDT"; names[0x1B] = "NCCS"; names[0x1C] = "CC"; names[0x1E] = "NCS";
    names[0x20] = "NCT"; names[0x28] = "SQR"; names[0x29] = "DCPL"; names[0x2A] = "DPCT";
    names[0x2D] = "AVSZ3"; names[0x2E] = "AVSZ4"; names[0x30] = "RTPT"; names[0x3D] = "GPF";
    names[0x3E] = "GPL"; names[0x3F] = "NCCT";
    std::string s;
    for (int i = 0; i < 64; i++) {
        if (!g_gte_counts[i]) continue;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%s=%u ", names[i] ? names[i] : "?", g_gte_counts[i]);
        s += buf;
    }
    LOG(Gpu, "gte: %s| nclip +%u -%u 0:%u | rtp err=%u divov=%u", s.empty() ? "(none)" : s.c_str(), g_nclip_pos,
        g_nclip_neg, g_nclip_zero, g_rtp_err, g_rtp_divov);
    std::memset(g_gte_counts, 0, sizeof g_gte_counts);
    g_nclip_pos = g_nclip_neg = g_nclip_zero = g_rtp_err = g_rtp_divov = 0;
}

uint64_t g_gte_dump_frame = 0;  // --gte-dump FRAME: detailed log of RTPS/RTPT on that frame

static void dump_rtp_state(const char* when, R3000* c) {
    auto d = [&](int r) { return c->gte_data[r]; };
    auto k = [&](int r) { return c->gte_ctrl[r]; };
    if (when[0] == 'i') {
        log_msg(LogCat::Gpu,
                "  in : V0=(%d,%d,%d) V1=(%d,%d,%d) V2=(%d,%d,%d) RT=[%d %d %d|%d %d %d|%d %d %d] TR=(%d,%d,%d) "
                "OF=(%d,%d) H=%u",
                (int16_t)d(0), (int16_t)(d(0) >> 16), (int16_t)d(1), (int16_t)d(2), (int16_t)(d(2) >> 16),
                (int16_t)d(3), (int16_t)d(4), (int16_t)(d(4) >> 16), (int16_t)d(5), (int16_t)k(0),
                (int16_t)(k(0) >> 16), (int16_t)k(1), (int16_t)(k(1) >> 16), (int16_t)k(2), (int16_t)(k(2) >> 16),
                (int16_t)k(3), (int16_t)(k(3) >> 16), (int16_t)k(4), k(5), k(6), k(7), k(24) >> 16, k(25) >> 16,
                (uint16_t)k(26));
    } else {
        log_msg(LogCat::Gpu, "  out: MAC=(%d,%d,%d) SZ=(%d,%d,%d) SXY0=(%d,%d) SXY1=(%d,%d) SXY2=(%d,%d) FLAG=%08X",
                d(25), d(26), d(27), d(17), d(18), d(19), (int16_t)d(12), (int16_t)(d(12) >> 16), (int16_t)d(13),
                (int16_t)(d(13) >> 16), (int16_t)d(14), (int16_t)(d(14) >> 16), (uint32_t)k(31));
    }
}

extern "C" void gte_command(R3000* c, uint32_t cmd) {
    C = c;
    g_gte_counts[cmd & 0x3F]++;
    bool dump = g_gte_dump_frame && host_frame_count() == g_gte_dump_frame && g_gte_dumped < 12 &&
                ((cmd & 0x3F) == 0x01 || (cmd & 0x3F) == 0x30);
    if (dump) {
        g_gte_dumped++;
        log_msg(LogCat::Gpu, "%s cmd=%07X", (cmd & 0x3F) == 1 ? "RTPS" : "RTPT", cmd);
        dump_rtp_state("in", c);
    }
    struct Post { R3000* c; bool on; ~Post() { if (on) dump_rtp_state("out", c); } } post{c, dump};
    if (g_gte_dump_frame && host_frame_count() == g_gte_dump_frame && (cmd & 0x3F) == 0x0C) {
        log_msg(LogCat::Gpu, "OP cmd=%07X D=(%d,%d,%d) IR=(%d,%d,%d) ctrl0=%08X ctrl2=%08X ctrl4=%08X ra=%08X", cmd,
                M(0, 0, 0), M(0, 1, 1), M(0, 2, 2), IR(1), IR(2), IR(3), K(0), K(2), K(4), c->r[31]);
    }
    if (!g_unr_init) init_unr();
    FLAG() = 0;
    Cmd k;
    k.shift = (cmd & (1 << 19)) ? 12 : 0;
    k.lm = (cmd >> 10) & 1;
    k.mx = (cmd >> 17) & 3;
    k.v = (cmd >> 15) & 3;
    k.cv = (cmd >> 13) & 3;

    switch (cmd & 0x3F) {
        case 0x01: rtp(0, k, true); break;  // RTPS
        case 0x06: {  // NCLIP
            int32_t sx0 = lo16(D(12)), sy0 = hi16(D(12)), sx1 = lo16(D(13)), sy1 = hi16(D(13));
            int32_t sx2 = lo16(D(14)), sy2 = hi16(D(14));
            MAC(0) = chk_mac0((int64_t)sx0 * sy1 + (int64_t)sx1 * sy2 + (int64_t)sx2 * sy0 -
                              (int64_t)sx0 * sy2 - (int64_t)sx1 * sy0 - (int64_t)sx2 * sy1);
            // PGXP culling correction: facing from the exact positions, when all three are known
            // (thin or tiny polygons whose whole-pixel corners flip the sign, and flicker)
            if (g_gte_precise) {
                GtePrecise e[3];
                bool all = true;
                for (int i = 0; i < 3 && all; i++) {
                    const PgxpVal& s = g_pgxp_sxy[i];
                    const int32_t w = D(12 + i);
                    all = s.w > 0 && std::abs(lo16(w) - lo16((int32_t)s.v)) <= 1 && std::abs(hi16(w) - hi16((int32_t)s.v)) <= 1;
                    if (all) e[i] = {s.x + (float)(lo16(w) - lo16((int32_t)s.v)), s.y + (float)(hi16(w) - hi16((int32_t)s.v)), s.w, 0};
                }
                if (all) {
                    const double a = ((double)e[1].x - e[0].x) * ((double)e[2].y - e[0].y) -
                                     ((double)e[2].x - e[0].x) * ((double)e[1].y - e[0].y);
                    const int32_t m = MAC(0);
                    if ((a > 0.5 && m <= 0) || (a < -0.5 && m >= 0)) MAC(0) = a > 0 ? 1 : -1;
                }
            }
            break;
        }
        case 0x0C: {  // OP: all three components use the IR values from before the command
            int64_t d1 = M(0, 0, 0), d2 = M(0, 1, 1), d3 = M(0, 2, 2);
            int64_t i1 = IR(1), i2 = IR(2), i3 = IR(3);
            set_mac_ir(1, i3 * d2 - i2 * d3, k.shift, k.lm);
            set_mac_ir(2, i1 * d3 - i3 * d1, k.shift, k.lm);
            set_mac_ir(3, i2 * d1 - i1 * d2, k.shift, k.lm);
            break;
        }
        case 0x10: {  // DPCS
            interpolate((int64_t)rgbc(0) << 16, (int64_t)rgbc(1) << 16, (int64_t)rgbc(2) << 16, k);
            push_color();
            break;
        }
        case 0x11: {  // INTPL
            interpolate((int64_t)IR(1) << 12, (int64_t)IR(2) << 12, (int64_t)IR(3) << 12, k);
            push_color();
            break;
        }
        case 0x12: {  // MVMVA
            int32_t x, y, z;
            if (k.v < 3) { x = VX(k.v); y = VY(k.v); z = VZ(k.v); }
            else { x = IR(1); y = IR(2); z = IR(3); }
            mvmva_generic(k.mx, x, y, z, k.cv, k);
            break;
        }
        case 0x13: op_nc(0, k, 2); break;  // NCDS
        case 0x14: {  // CDP
            mvmva_generic(2, IR(1), IR(2), IR(3), 1, k);
            int64_t m[3];
            color_times_ir(m);
            interpolate(m[0], m[1], m[2], k);
            push_color();
            break;
        }
        case 0x16: for (int n = 0; n < 3; n++) op_nc(n, k, 2); break;  // NCDT
        case 0x1B: op_nc(0, k, 1); break;  // NCCS
        case 0x1C: {  // CC
            mvmva_generic(2, IR(1), IR(2), IR(3), 1, k);
            int64_t m[3];
            color_times_ir(m);
            for (int i = 1; i <= 3; i++) set_mac_ir(i, m[i - 1], k.shift, k.lm);
            push_color();
            break;
        }
        case 0x1E: op_nc(0, k, 0); break;  // NCS
        case 0x20: for (int n = 0; n < 3; n++) op_nc(n, k, 0); break;  // NCT
        case 0x28: {  // SQR
            for (int i = 1; i <= 3; i++) set_mac_ir(i, (int64_t)IR(i) * IR(i), k.shift, k.lm);
            break;
        }
        case 0x29: {  // DCPL
            int64_t m[3];
            color_times_ir(m);
            interpolate(m[0], m[1], m[2], k);
            push_color();
            break;
        }
        case 0x2A: {  // DPCT: three times on the colour FIFO head
            for (int n = 0; n < 3; n++) {
                uint32_t col = (uint32_t)D(20);
                interpolate((int64_t)(col & 0xFF) << 16, (int64_t)((col >> 8) & 0xFF) << 16,
                            (int64_t)((col >> 16) & 0xFF) << 16, k);
                push_color();
            }
            break;
        }
        case 0x2D: {  // AVSZ3
            int64_t s = (int64_t)(int16_t)K(29) * ((uint16_t)D(17) + (uint16_t)D(18) + (uint16_t)D(19));
            MAC(0) = chk_mac0(s);
            int64_t o = s >> 12;
            if (o < 0) { set_flag(18); o = 0; } else if (o > 0xFFFF) { set_flag(18); o = 0xFFFF; }
            D(7) = (int32_t)o;
            break;
        }
        case 0x2E: {  // AVSZ4
            int64_t s = (int64_t)(int16_t)K(30) *
                        ((uint16_t)D(16) + (uint16_t)D(17) + (uint16_t)D(18) + (uint16_t)D(19));
            MAC(0) = chk_mac0(s);
            int64_t o = s >> 12;
            if (o < 0) { set_flag(18); o = 0; } else if (o > 0xFFFF) { set_flag(18); o = 0xFFFF; }
            D(7) = (int32_t)o;
            break;
        }
        case 0x30: rtp(0, k, false); rtp(1, k, false); rtp(2, k, true); break;  // RTPT
        case 0x3D: {  // GPF
            for (int i = 1; i <= 3; i++) set_mac_ir(i, (int64_t)IR(0) * IR(i), k.shift, k.lm);
            push_color();
            break;
        }
        case 0x3E: {  // GPL
            for (int i = 1; i <= 3; i++)
                set_mac_ir(i, ((int64_t)MAC(i) << k.shift) + (int64_t)IR(0) * IR(i), k.shift, k.lm);
            push_color();
            break;
        }
        case 0x3F: {  // NCCT
            for (int n = 0; n < 3; n++) op_nc(n, k, 1);
            break;
        }
        default:
            LOG(Core, "unknown GTE command %07X", cmd);
            break;
    }
    uint32_t f = FLAG();
    if (f & 0x7F87E000u) f |= 0x80000000u;
    FLAG() = f;
    uint32_t op = cmd & 0x3F;
    if (op == 0x06) {
        if (MAC(0) > 0) g_nclip_pos++;
        else if (MAC(0) < 0) g_nclip_neg++;
        else g_nclip_zero++;
    } else if (op == 0x01 || op == 0x30) {
        if (f & 0x80000000u) g_rtp_err++;
        if (f & (1u << 17)) g_rtp_divov++;
    }
}
