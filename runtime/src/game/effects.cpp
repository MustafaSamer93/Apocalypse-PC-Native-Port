// Native 3D for the effects and the other routines that make 3D shapes on the CPU:
// glows, sprites (smoke, fire, flashes), shards, world quads, lines, trails, beams, ribbons,
// tubes, text hung on 3D points, and the blob shadows and halos of jointed models.
//
// Each still runs the original code, which takes every decision (culling, sizes, colours, texture
// words, ordering-table slots, packet order, the RAM it writes): only the vertex positions of the
// packets it made are written again, from float geometry - the world position with its fraction,
// the exact projection instead of whole pixels, screen sizes without integer steps - with the
// exact position in each word's shadow, so gpu.cpp draws them where they are, perspective-correct.
// The original made these shapes on the screen around one projected point: in widescreen their
// widths are squeezed here (gpu.cpp's fx_squeeze does that for shapes without exact corners).
// Shapes the original drew with corners behind the camera (no near test: clamped whole pixels,
// spikes across the screen) are not drawn.
// Specs of the routines: the scratchpad notes of the native-gte-all work (spec_fx / spec_beams /
// spec_objects).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/psx.h"
#include "game/game.h"
#include "game/native3d.h"
#include "host/host.h"
#include "render/gpu_hw.h"

using namespace n3d;

namespace {

constexpr uint32_t kScratch = 0x1F800000;
constexpr uint32_t kSinCos = 0x800F863C;  // 4096 x {int16 cos, int16 sin} (4.12)

bool on() { return hw_active(); }
uint32_t ptr(R3000* c) { return rd(c, kPacketPtr) & 0xFFFFFF; }
int16_t h16(R3000* c, uint32_t a) { return (int16_t)MEM_R16(c, a); }
int32_t cos_t(R3000* c, int a) { return h16(c, kSinCos + 4 * (uint32_t)(a & 0xFFF)); }
int32_t sin_t(R3000* c, int a) { return h16(c, kSinCos + 4 * (uint32_t)(a & 0xFFF) + 2); }
float znear(R3000* c) { return std::max(1.0f, (float)(int32_t)rd(c, kNear)); }

// The original routine (a hooked entry) run from its hook: its decisions, then the positions fixed.
// Inside is noted with the frame: a ghost render aborted inside (longjmp) leaves it set only for
// that frame.
struct Inside {
    uint64_t frame = 0;
    bool now() const { return frame == host_frame_count() + 1; }
};
struct Through {
    uint32_t ra, sp;
    Through(R3000* c, uint32_t fn, Inside& in) : ra(c->r[31]), sp(c->r[29]) {
        in.frame = host_frame_count() + 1;
        call_indirect(c, fn);
        in.frame = 0;
        c->r[31] = ra;
        c->r[29] = sp;
    }
};

// A unit screen-space perpendicular of a -> b, the way the effects make their widths: measured in
// square pixels (a 512-wide screen's pixel is 5/8 as wide), x back in screen pixels (and squeezed
// in widescreen). None when the two are closer than minlen pixels.
struct Perp {
    float x = 0, y = 0;
    bool ok = false;
};
Perp perp(const View& v, const Pt& a, const Pt& b, float minlen) {
    const float dx = (b.sx - a.sx) * 0.625f / v.ws, dy = b.sy - a.sy;
    const float len = std::sqrt(dx * dx + dy * dy);
    Perp p;
    if (len < minlen || !(a.z > 0) || !(b.z > 0)) return p;
    p.x = dy / len * 1.6f * v.ws, p.y = -dx / len, p.ok = true;
    return p;
}
Pt offset(const Pt& c, const Perp& n, float w, float sign = 1.0f) {
    Pt p = c;
    p.sx += sign * n.x * w, p.sy += sign * n.y * w;
    return p;
}

// The original's perpendicular (80026370 / 8002626C) on whole-pixel words: zero = no width.
bool perp_int(uint32_t a, uint32_t b, int minlen) {
    const int dx = ((int16_t)(b & 0xFFFF) - (int16_t)(a & 0xFFFF)) * 320 / 512, dy = (int16_t)(b >> 16) - (int16_t)(a >> 16);
    const int ax = std::abs(dx), ay = std::abs(dy);
    return std::max(ax, ay) + std::min(ax, ay) / 2 >= minlen;
}

}  // namespace

// ------------------------------------------------------------------ the effects list (8003736C)

// 80037F88: a glow (s4) was drawn: its mode packet (sp+0xF0) and after it, per drawn segment, a
// triangle to the centre (unless hollow) and a quad per band. Ring points: radius R (world) at a
// table angle, an ellipse on the screen round the centre: x 625 R cos / (4096 z), y -400 R sin / (4096 z).
extern "C" void psx_fx_glow(R3000* c) {
    if (!on()) return;
    const uint32_t g = c->r[20];
    const uint32_t from = (rd(c, c->r[29] + 0xF0) & 0xFFFFFF) + 8, to = ptr(c);
    const View v = view_now(c);
    const Pt C = see_at(c, v, g + 0x18);
    if (!(C.z >= 1)) return;
    const uint32_t segs = rd(c, g + 0x48), bands = rd(c, g + 0x4C);
    const int n = (int)rd(c, g + 0x50), m = (int)rd(c, g + 0x54);
    if (n <= 0 || m < 0 || m > 30) return;
    const int step = h16(c, g + 0x5C), start = h16(c, g + 0x60);
    const bool hollow = MEM_R8(c, g + 0x5E) != 0;
    const uint32_t mask = rd(c, g + 0x64);
    auto point = [&](int32_t R, int a) {
        Pt p = C;
        p.sx = std::clamp(C.sx + v.ws * 625.0f * R * cos_t(c, a) / (4096.0f * C.z), -100.0f, 612.0f);
        p.sy = std::clamp(C.sy - 400.0f * R * sin_t(c, a) / (4096.0f * C.z), -100.0f, 340.0f);
        return p;
    };
    auto dr = [&](int k, int s) { return (int32_t)rd(c, bands + (uint32_t)(k * n + s) * 8); };
    Pt A[32], B[32];
    int32_t R = (int32_t)rd(c, segs + (uint32_t)(n - 1) * 8);
    A[0] = point(R, start);
    for (int k = 0; k < m; k++) R += dr(k, n - 1), A[k + 1] = point(R, start);
    struct W {
        uint32_t a;
        Pt p;
    };
    static std::vector<W> out;
    out.clear();
    uint32_t p = from;
    int angle = start + step;
    for (int s = 0; s < n; s++) {
        const bool draw = (mask >> (s & 31)) & 1;
        if (s == n - 1) angle = start;
        R = (int32_t)rd(c, segs + (uint32_t)s * 8);
        B[0] = point(R, angle);
        if (!hollow && draw) {
            out.push_back({p + 8, C}), out.push_back({p + 16, A[0]}), out.push_back({p + 24, B[0]});
            p += 28;
        }
        for (int k = 0; k < m; k++) {
            R += dr(k, s);
            B[k + 1] = point(R, angle);
            if (draw) {
                out.push_back({p + 8, A[k + 1]}), out.push_back({p + 16, B[k + 1]});
                out.push_back({p + 24, A[k]}), out.push_back({p + 32, B[k]});
                p += 36;
            }
        }
        std::swap(A, B);
        angle += step;
    }
    if (p != to) return;  // (not the packets this glow made)
    for (const W& w : out) put_xy(c, w.a, w.p);
}

// 8003891C: the next sprite (t6) - this one (packet t2) was drawn when the packet pointer moved
// on 40 bytes. Corners: frame F's rectangle round the projected centre, size / z pixels per frame
// unit (rotated and scaled when it has an angle; mirrored by flag 1).
extern "C" void psx_fx_sprite(R3000* c) {
    const uint32_t t = c->r[14], P = c->r[10] & 0xFFFFFF;
    if (!on() || ptr(c) != P + 40) return;
    const View v = view_now(c);
    const Pt C = see_at(c, v, t + 0x18);
    if (!(C.z >= 1)) return;
    const int32_t size = h16(c, t + 0x5E);
    const uint32_t F = rd(c, t + 0x54);
    const float x0 = (int8_t)MEM_R8(c, F), y0 = (int8_t)MEM_R8(c, F + 1);
    const float w = MEM_R8(c, F + 2), h = MEM_R8(c, F + 3);
    const float k = size / C.z;
    const int angle = MEM_R16(c, t + 0x60);
    const bool mirror = MEM_R8(c, t + 0x58) & 1;
    Pt q[4];
    auto put = [&](int i, float ox, float oy) {
        q[i] = C;
        q[i].sx = C.sx + v.ws * ox, q[i].sy = C.sy + oy;
    };
    if (angle == 0) {
        // (far sprites keep the original's least size: 2 x 1 pixels)
        const int32_t z = (int32_t)rd(c, c->r[29] + 0x68);
        const uint32_t s = z > 0 ? ((uint32_t)size << 7) / (uint32_t)z : 0;
        const float W = ((s * (uint32_t)w) >> 7) < 5 ? 2.0f : k * w, H = ((s * (uint32_t)h) >> 7) < 5 ? 1.0f : k * h;
        const float ax = k * x0, ay = k * y0;
        put(0, ax, ay), put(1, ax + W, ay), put(2, ax, ay + H), put(3, ax + W, ay + H);
    } else {
        const float cs = cos_t(c, angle) / 4096.0f, sn = sin_t(c, angle) / 4096.0f;
        const uint32_t sc = rd(c, t + 0x68);
        const float kx = k * (sc & 0xFFFF) / 4096.0f, ky = k * (sc >> 16) / 4096.0f;
        const float X[4] = {x0, x0 + w, x0, x0 + w}, Y[4] = {y0, y0, y0 + h, y0 + h};
        for (int i = 0; i < 4; i++) put(i, kx * (X[i] * sn - Y[i] * cs), ky * (X[i] * cs + Y[i] * sn));
    }
    static const int kOrder[2][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}};
    for (int i = 0; i < 4; i++) put_xy(c, P + 8 + 8 * i, q[kOrder[mirror][i]]);
}

// 80038B10: a flat semi-transparent triangle (a2; packet a1, always drawn): three world points.
extern "C" void psx_fx_tri(R3000* c) {
    if (!on()) return;
    const uint32_t o = c->r[6], P = c->r[5] & 0xFFFFFF;
    const View v = view_now(c);
    Pt q[3];
    for (int i = 0; i < 3; i++) q[i] = see_at(c, v, o + 0x48 + 12 * i);
    // (no culling in the original: a shard reaching behind the camera was a spike of clamped
    // whole pixels across the screen; not drawn)
    const float zn = znear(c);
    bool front = true;
    for (int i = 0; i < 3; i++) front &= q[i].z >= zn;
    if (!front) {
        for (int i = 0; i < 3; i++) put_xy(c, P + 8 + 4 * i, 0, 0, 0);
        return;
    }
    for (int i = 0; i < 3; i++) put_xy(c, P + 8 + 4 * i, q[i]);
}

// 80039130: the next world quad (s1) - this one (packet s0) was drawn when the pointer moved on
// 40 bytes (and it has a texture: without one s0 is the last quad's).
extern "C" void psx_fx_quad(R3000* c) {
    const uint32_t o = c->r[17], P = c->r[16] & 0xFFFFFF;
    if (!on() || !rd(c, o + 0x94) || ptr(c) != P + 40) return;
    const View v = view_now(c);
    static const uint32_t kAt[4] = {0x18, 0x48, 0x54, 0x60};
    Pt q[4];
    for (int i = 0; i < 4; i++)
        if (!((q[i] = see_at(c, v, o + kAt[i])).z >= 1)) return;
    for (int i = 0; i < 4; i++) put_xy(c, P + 8 + 8 * i, q[i]);
}

// 8003982C: a tetrahedron (s1, shards) made its faces, up to four FT3 of 32 bytes ending at a0
// (the ones facing the camera): corners +0x68 + 12k in whole world units; the original's corner
// words are at sp+0xB0 + 8k. The faces are found from the last one back by their corner words.
extern "C" void psx_fx_tetra(R3000* c) {
    if (!on()) return;
    const uint32_t o = c->r[17], end = c->r[4] & 0xFFFFFF, sp = c->r[29];
    static const int kFace[4][3] = {{0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {2, 3, 0}};
    uint32_t w[4];
    for (int k = 0; k < 4; k++) w[k] = rd(c, sp + 0xB0 + 8 * k);
    const View v = view_now(c);
    const float zn = znear(c);
    Pt q[4];
    for (int k = 0; k < 4; k++) {
        const uint32_t a = o + 0x68 + 12 * k;
        q[k] = v.see((int32_t)rd(c, a), (int32_t)rd(c, a + 4), (int32_t)rd(c, a + 8));
        if (!(q[k].z >= zn)) return;
    }
    uint32_t p = end;
    for (int f = 3; f >= 0 && p >= 32; f--) {
        const int* F = kFace[f];
        const uint32_t P = p - 32;
        if (rd(c, P + 8) != w[F[0]] || rd(c, P + 0x10) != w[F[1]] || rd(c, P + 0x18) != w[F[2]]) continue;
        for (int i = 0; i < 3; i++) put_xy(c, P + 8 + 8 * i, q[F[i]]);
        p = P;
    }
}

// 8006AE04: the text list (8006AAB8) made a glyph (packet a1) of entry s3: an entry anchored at
// a 3D point (+0x14, kept in whole units like the object it hangs on) has its glyphs placed in
// whole pixels round that point's projection (sp+0x58): the same offsets round its exact one.
extern "C" void psx_fx_glyph(R3000* c) {
    const uint32_t E = c->r[19], P = c->r[5] & 0xFFFFFF, sp = c->r[29];
    if (!on() || !MEM_R8(c, E + 0x10)) return;
    const View v = view_now(c);
    const Pt a = v.see(std::floor((int32_t)rd(c, E + 0x14) / 4096.0), std::floor((int32_t)rd(c, E + 0x18) / 4096.0),
                       std::floor((int32_t)rd(c, E + 0x1C) / 4096.0));
    if (!(a.z >= 1)) return;
    const uint32_t at = rd(c, sp + 0x58);
    const int sx = (int16_t)(at & 0xFFFF), sy = (int16_t)(at >> 16);
    for (int i = 0; i < 4; i++) {
        const uint32_t w = rd(c, P + 8 + 8 * i);
        put_xy(c, P + 8 + 8 * i, a.sx + ((int16_t)(w & 0xFFFF) - sx) * v.ws, a.sy + ((int16_t)(w >> 16) - sy), a.z);
    }
}

// Lines: two world points (gpu.cpp draws a line between exact ends too).
static void line_ends(R3000* c, uint32_t xy0, const Pt& a, uint32_t xy1, const Pt& b) {
    const float zn = znear(c);
    if (!(a.z >= zn) || !(b.z >= zn)) return;
    put_xy(c, xy0, a), put_xy(c, xy1, b);
}
// a segment 800850A4 cut (scratchpad: A +0, B +0xC, t +0x20 / +0x24 of 4096)
static void cut_line(R3000* c, uint32_t xy0, uint32_t xy1) {
    const View v = view_now(c);
    const Pt A = see_at(c, v, kScratch), B = see_at(c, v, kScratch + 12);
    auto cut = [&](float t) {
        Pt p{A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t, A.z + (B.z - A.z) * t, 0, 0};
        v.project(p);
        return p;
    };
    line_ends(c, xy0, cut((int32_t)rd(c, kScratch + 0x20) / 4096.0f), xy1, cut((int32_t)rd(c, kScratch + 0x24) / 4096.0f));
}
// 800380F8 / 800382A0: a gouraud (a0) / flat (a1) polyline's segment was linked
extern "C" void psx_fx_polyline(R3000* c, int flat) {
    if (!on()) return;
    const uint32_t L = c->r[flat ? 5 : 4] & 0xFFFFFF;
    if (flat) cut_line(c, L + 8, L + 0xC);
    else cut_line(c, L + 8, L + 0x10);
}
// 80038D20: a single line (s0; packet a1) is drawn: points +0x50 and +0x5C
extern "C" void psx_fx_line(R3000* c) {
    if (!on()) return;
    const uint32_t o = c->r[16], P = c->r[5] & 0xFFFFFF;
    const View v = view_now(c);
    line_ends(c, P + 8, see_at(c, v, o + 0x50), P + 0x10, see_at(c, v, o + 0x5C));
}

// 80039FB4: a trail piece (s6; packet s0) is drawn: a quad from end A (+0x60) to end B (+0x6C),
// half-width size * frame width / (2 z), min 2 pixels. Pieces of one list chain on (A = the last
// piece's end) unless flag 16 starts one or the last was not drawn; flag 16 also saves its start
// corners, which a piece with flag 32 ends on (a loop).
extern "C" void psx_fx_trail(R3000* c) {
    static uint32_t last_obj, last_end;
    static Pt last_b, last_bp, last_bm, loop_p, loop_m;
    if (!on()) return;
    const uint32_t o = c->r[22], P = c->r[16] & 0xFFFFFF;
    const uint32_t flags = MEM_R8(c, o + 0x58);
    const bool start = (flags & 16) || !last_obj || rd(c, last_obj + 4) != o || last_end != P;
    last_obj = 0;
    const View v = view_now(c);
    const Pt A = start ? see_at(c, v, o + 0x60) : last_b, B = see_at(c, v, o + 0x6C);
    if (!(A.z >= 1) || !(B.z >= 1)) return;
    const float wide = (float)h16(c, o + 0x5E) * MEM_R8(c, rd(c, o + 0x54) + 2);
    const float ha = std::max(2.0f, wide / A.z / 2), hb = std::max(2.0f, wide / B.z / 2);
    const float dx = (B.sx - A.sx) / v.ws, dy = B.sy - A.sy, len = std::sqrt(dx * dx + dy * dy);
    if (!(len > 0)) return;
    auto side = [&](const Pt& p, float h, float s) {
        Pt q = p;
        q.sx += s * v.ws * dy * h / len, q.sy -= s * dx * h / len;
        return q;
    };
    const Pt ap = start ? side(A, ha, 1) : last_bp, am = start ? side(A, ha, -1) : last_bm;
    if (flags & 16) loop_p = ap, loop_m = am;
    const Pt bp = (flags & 32) ? loop_p : side(B, hb, 1), bm = (flags & 32) ? loop_m : side(B, hb, -1);
    const Pt* order[4][4] = {{&ap, &bp, &am, &bm}, {&am, &bm, &ap, &bp}, {&bm, &bp, &am, &ap}, {&bp, &bm, &ap, &am}};
    for (int i = 0; i < 4; i++) put_xy(c, P + 8 + 8 * i, *order[flags & 3][i]);
    last_obj = o, last_end = P + 40, last_b = B, last_bp = bp, last_bm = bm;
}

// ------------------------------------------------------------------ self-drawing effects

// 80025BEC: a gouraud line from point +0x6C (bright) to +0x18 (dim), 28 bytes when drawn.
extern "C" int psx_hook_ray(R3000* c) {
    static Inside in;
    if (in.now() || !on()) return 0;
    const uint32_t o = c->r[4], from = ptr(c);
    Through run(c, 0x80025BEC, in);
    if (ptr(c) != from + 28) return 1;
    const View v = view_now(c);
    line_ends(c, from + 8, see_at(c, v, o + 0x6C), from + 0x10, see_at(c, v, o + 0x18));
    return 1;
}

// 80026488: a beam (additive quads fading out to +-width round a line of points), with a smear
// to where the points were two draws ago. Point i: world position, colour, width W (W * 400 / z
// pixels); the original's per-point results stay in the scratchpad (E_i: SXY, normal, z, width,
// dropped), and its history words (+0x18 / +0x1C, read back as the smear's corners) are the game's:
// their shadows carry the exact positions along.
extern "C" int psx_hook_beam(R3000* c) {
    static Inside in;
    if (in.now() || !on()) return 0;
    const uint32_t o = c->r[4];
    const int n = (int)rd(c, o + 0x4C);
    const uint32_t pts = rd(c, o + 0x50);
    const uint32_t h0 = rd(c, o + 0x48);
    if (n < 2 || n > 64) return 0;
    PgxpVal old[64];
    uint32_t oldw[64];
    for (int i = 0; i < n; i++) oldw[i] = rd(c, pts + 0x24 * i + 0x1C), old[i] = pgxp_ram[phys(pts + 0x24 * i + 0x1C) >> 2];
    const uint32_t from = ptr(c);
    Through run(c, 0x80026488, in);
    const uint32_t to = ptr(c);
    const View v = view_now(c);
    Pt pt[64];
    float w[64];
    bool drop[64], nz[64];
    uint32_t sxy[64];
    for (int i = 0; i < n; i++) {
        const uint32_t E = kScratch + 0x1C * i;
        sxy[i] = rd(c, E);
        drop[i] = rd(c, E + 0x18) != 0;
        nz[i] = (rd(c, E + 4) | rd(c, E + 8)) != 0;
        pt[i] = see_at(c, v, pts + 0x24 * i);
        drop[i] |= !(pt[i].z >= 1);
        w[i] = drop[i] ? 0 : MEM_R16(c, pts + 0x24 * i + 0x10) * 400.0f / pt[i].z;
    }
    Perp seg[64], nrm[64];
    for (int i = 0; i + 1 < n; i++)
        if (!drop[i] && !drop[i + 1]) seg[i] = perp(v, pt[i], pt[i + 1], 2);
    for (int i = 0; i < n; i++) {
        const Perp& a = seg[i > 0 ? i - 1 : 0];
        const Perp& b = seg[i < n - 1 ? i : n - 2];
        nrm[i].x = (a.x + b.x) * 0.5f, nrm[i].y = (a.y + b.y) * 0.5f;
        if (i == 0 || i == n - 1) nrm[i] = i == 0 ? b : a;
    }
    const uint32_t size = h0 >= 3 ? 0x74 : 0x50;
    uint32_t q = from;
    for (int k = 1; k < n && q + size <= to; k++) {
        if (!nz[k] || !nz[k - 1]) continue;
        if (rd(c, q + 0x18) != sxy[k] || rd(c, q + 0x20) != sxy[k - 1]) break;  // (not this beam's packets)
        if (!drop[k] && !drop[k - 1]) {
            for (int side = 0; side < 2; side++) {
                const uint32_t Q = q + 0x24 * side;
                const float sg = side ? -1.0f : 1.0f;
                put_xy(c, Q + 0x08, offset(pt[k], nrm[k], w[k], sg));
                put_xy(c, Q + 0x10, offset(pt[k - 1], nrm[k - 1], w[k - 1], sg));
                put_xy(c, Q + 0x18, pt[k]);
                put_xy(c, Q + 0x20, pt[k - 1]);
            }
            if (h0 >= 3) {
                const uint32_t Q = q + 0x50;
                for (int j = 0; j < 2; j++) {  // the smear: where the points were two draws ago
                    const int i = k - j;
                    const PgxpVal& s = old[i];
                    if (s.w > 0 && s.v == oldw[i] && rd(c, Q + 0x08 + 8 * j) == oldw[i]) put_xy(c, Q + 0x08 + 8 * j, s.x, s.y, s.w);
                }
                put_xy(c, Q + 0x18, pt[k]);
                put_xy(c, Q + 0x20, pt[k - 1]);
            }
        }
        q += size;
    }
    if (h0)  // the history word just written: this draw's exact position
        for (int i = 0; i < n; i++) {
            const uint32_t a = pts + 0x24 * i + 0x18;
            if (!drop[i] && rd(c, a) == sxy[i]) pgxp_ram[phys(a) >> 2] = PgxpVal{sxy[i], pt[i].sx, pt[i].sy, pt[i].z};
            if (h0 == 1) pgxp_ram[phys(a + 4) >> 2] = pgxp_ram[phys(a) >> 2];
        }
    return 1;
}

// 800276A4: a textured ribbon (80026C68, object s7) drew segment i (sp+0x44; block a3: four
// quads, inner width W1 and outer W2 on both sides of the line A'-B'). A' and B' are A and B
// (scratchpad +0 / +0xC) cut where 800850A4 cut them (t = +0x20 / +0x24 of 4096); a segment drawn
// right after the last one starts from that one's end corners.
extern "C" void psx_fx_ribbon(R3000* c) {
    static uint32_t last_obj, last_end;
    static Pt last_b[4];
    static Perp last_prev;
    if (!on()) return;
    const uint32_t o = c->r[23], P = c->r[7] & 0xFFFFFF, sp = c->r[29];
    const int i = (int)rd(c, sp + 0x44), n = (int)rd(c, o + 0x64);
    const uint32_t pa = rd(c, o + 0x68) + 0x24 * (uint32_t)i, pb = pa + 0x24;
    const View v = view_now(c);
    const Pt A = see_at(c, v, kScratch), B = see_at(c, v, kScratch + 12);
    auto cut = [&](float t) {
        Pt p;
        p.x = A.x + (B.x - A.x) * t, p.y = A.y + (B.y - A.y) * t, p.z = A.z + (B.z - A.z) * t;
        v.project(p);
        return p;
    };
    const Pt a = cut((int32_t)rd(c, kScratch + 0x20) / 4096.0f), b = cut((int32_t)rd(c, kScratch + 0x24) / 4096.0f);
    const bool cont = last_obj == o && last_end == P;
    last_obj = 0;
    if (!(a.z >= 1) || !(b.z >= 1)) return;
    // normals as the original chooses them (zero = shorter than 3 pixels, from its whole pixels)
    Perp cur = perp(v, a, b, 0.0f);
    cur.ok = perp_int(rd(c, kScratch + 0x18), rd(c, kScratch + 0x1C), 3);
    const Perp prev = cont ? last_prev : Perp{};
    const Perp nA = prev.ok ? prev : cur, nB = cur.ok ? cur : prev;
    if (!nA.ok || !nB.ok) return;
    const float w1a = MEM_R16(c, pa + 0x10) * 400.0f / a.z, w2a = MEM_R16(c, pa + 0x12) * 400.0f / a.z;
    const float w1b = MEM_R16(c, pb + 0x10) * 400.0f / b.z, w2b = MEM_R16(c, pb + 0x12) * 400.0f / b.z;
    Pt ca[4], cb[4];
    if (cont) std::copy(last_b, last_b + 4, ca);
    else ca[0] = offset(a, nA, w1a), ca[1] = offset(a, nA, w1a, -1), ca[2] = offset(a, nA, w2a), ca[3] = offset(a, nA, w2a, -1);
    cb[0] = offset(b, nB, w1b), cb[1] = offset(b, nB, w1b, -1), cb[2] = offset(b, nB, w2b), cb[3] = offset(b, nB, w2b, -1);
    if (rd(c, o + 0x60) && i >= n / 2) {  // the second half wobbles (whole pixels, as the original)
        const int f = (int)rd(c, 0x800FF2F0);
        cb[0].sy += (float)(cos_t(c, 200 * f + 130) / 2048);
        cb[1].sy += (float)(3 * h16(c, kSinCos + ((uint32_t)(1000 * f) & 0x3FF8)) / 4096 + cos_t(c, 150 * f + 100) / 2048);
    }
    // Q0..Q3 at +0, +0x34, +0x68, +0x9C: (b1 a1 B' A') (b2 a2 B' A') (b3 a3 b1 a1) (b4 a4 b2 a2)
    const Pt* kQ[4][4] = {{&cb[0], &ca[0], &b, &a}, {&cb[1], &ca[1], &b, &a}, {&cb[2], &ca[2], &cb[0], &ca[0]}, {&cb[3], &ca[3], &cb[1], &ca[1]}};
    for (int q = 0; q < 4; q++)
        for (int j = 0; j < 4; j++) put_xy(c, P + 0x34 * q + 8 + 12 * j, *kQ[q][j]);
    last_obj = o, last_end = P + 0xF0;
    std::copy(cb, cb + 4, last_b);
    last_prev = cur.ok ? cur : prev;
}

// 8002948C: a tube of three-point rings (two quads between each pair of rings), points computed
// on the CPU; the original's per-ring results stay in the scratchpad (G_k: SXY, valid flag per
// point, least z). Quads reaching behind the near plane (the original had no near test) are not drawn.
extern "C" int psx_hook_tube(R3000* c) {
    static Inside in;
    if (in.now() || !on()) return 0;
    const uint32_t o = c->r[4];
    const uint32_t from = ptr(c);
    Through run(c, 0x8002948C, in);
    const uint32_t to = ptr(c);
    const int n = (int)rd(c, o + 0x4C);
    if (n < 1 || n > 40) return 1;
    const uint32_t rings = rd(c, o + 0x50), mask = rd(c, o + 0x6C);
    const uint32_t fixed = MEM_R16(c, o + 0x72);
    const View v = view_now(c);
    const float zn = znear(c);
    static const uint32_t kPoint[3] = {0x80, 0x74, 0x68};
    auto G = [&](int k) { return kScratch + 0x18 * (uint32_t)k; };
    auto valid = [&](int k, int j) { return h16(c, G(k) + 8 * j + 4) != 0; };
    auto quad = [&](uint32_t Q, int p, int q, int j0, int j1) {
        const uint32_t w[4] = {rd(c, G(p) + 8 * j0), rd(c, G(q) + 8 * j0), rd(c, G(p) + 8 * j1), rd(c, G(q) + 8 * j1)};
        const int ring[4] = {p, q, p, q}, pnt[4] = {j0, j0, j1, j1};
        Pt e[4];
        for (int i = 0; i < 4; i++) {
            if (rd(c, Q + 8 + 12 * i) != w[i]) return false;  // (not this tube's packets)
            e[i] = see_at(c, v, rings + 0x8C * (uint32_t)ring[i] + kPoint[pnt[i]]);
            if (!(e[i].z >= zn)) {  // (the original drew clamped whole pixels: not drawn)
                for (int j = 0; j < 4; j++) put_xy(c, Q + 8 + 12 * j, 0, 0, 0);
                return true;
            }
        }
        for (int i = 0; i < 4; i++) put_xy(c, Q + 8 + 12 * i, e[i]);
        return true;
    };
    uint32_t q = from;
    for (int k = 1; k <= n; k++) {
        const int p = k - 1, cc = k == n ? 0 : k;
        const int16_t mz = std::min(h16(c, G(p) + 0x16), h16(c, G(cc) + 0x16));
        if ((!fixed && mz == 0) || !((mask >> (k & 31)) & 1)) continue;
        q += 0x20;
        if (valid(p, 0) && valid(p, 1) && valid(cc, 0) && valid(cc, 1)) {
            if (q + 0x34 > to || !quad(q, p, cc, 0, 1)) return 1;
            q += 0x34;
        }
        if (valid(p, 2) && valid(p, 1) && valid(cc, 2) && valid(cc, 1)) {
            if (q + 0x34 > to || !quad(q, p, cc, 1, 2)) return 1;
            q += 0x34;
        }
    }
    return 1;
}

// ------------------------------------------------------------------ jointed models (8007F340)

// 8007A8F8 (blob shadow of a part, slid along a light onto the ground) and 8007B2E4 (a halo
// facing the camera): one FT4 from four corners K0..K3 in whole units round the camera, which
// the original projected to whole pixels without any near test (a corner behind the camera made
// a streak across the screen). Its RTPT / RTPS leave K1, K2 (V1, V2) and K3 (V0) in the GTE;
// K0 = K1 + K2 - K3. A quad reaching behind the near plane is not drawn.
static int part_quad(R3000* c, uint32_t fn, Inside& in) {
    if (in.now() || !on()) return 0;
    const uint32_t from = ptr(c);
    Through run(c, fn, in);
    if (ptr(c) != from + 40) return 1;
    auto vec = [&](int r) {
        const uint32_t xy = gte_read_data(c, 2 * r);
        return std::array<int32_t, 3>{(int16_t)(xy & 0xFFFF), (int16_t)(xy >> 16), (int16_t)gte_read_data(c, 2 * r + 1)};
    };
    const auto k1 = vec(1), k2 = vec(2), k3 = vec(0);
    const View v = view_now(c);
    const float zn = znear(c);
    Pt q[4];
    q[0] = v.rel(k1[0] + k2[0] - k3[0], k1[1] + k2[1] - k3[1], k1[2] + k2[2] - k3[2]);
    q[1] = v.rel(k1[0], k1[1], k1[2]), q[2] = v.rel(k2[0], k2[1], k2[2]), q[3] = v.rel(k3[0], k3[1], k3[2]);
    bool front = true;
    for (const Pt& p : q) front &= p.z >= zn;
    for (int i = 0; i < 4; i++) {
        if (front) put_xy(c, from + 8 + 8 * i, q[i]);
        else put_xy(c, from + 8 + 8 * i, 0, 0, 0);  // ponytail: dropped, not cut at the near plane
    }
    return 1;
}
extern "C" int psx_hook_part_shadow(R3000* c) {
    static Inside in;
    return part_quad(c, 0x8007A8F8, in);
}
extern "C" int psx_hook_part_halo(R3000* c) {
    static Inside in;
    return part_quad(c, 0x8007B2E4, in);
}
