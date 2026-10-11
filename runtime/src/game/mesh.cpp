// Native level renderer (Native 3D, always on): the game's level mesh renderer 800817FC,
// written again in C++ with float geometry.
//
// The original (hand-written MIPS + GTE) draws each polygon record of a mesh from the vertices
// 800809B0 & co. projected to whole pixels, and cuts polygons near the camera (and large ones)
// into a grid of up to 7x7 pieces, each projected to whole pixels again: the source of most of
// the level's remaining wobble and texture swim. Here every vertex keeps its exact position (the
// shadow gte.cpp left on its screen-buffer word), polygons reaching inside the near plane are
// clipped in view space instead of cut into a grid, and every packet vertex carries its exact
// position (and texture coordinates) to the renderer through the same shadows (pgxp_ram), so it
// is drawn perspective-correct as one piece, whatever its size. Culling, fog, lighting, colours, texture words, draw
// order and packets are the original's (spec: the 800817FC notes; colours use the GTE's own
// integer colour commands, so they come out the same to the bit).
//
// Sections with the game's clip plane (gp[3200]: geometry below a level's floor height is cut away)
// are cut against that plane in view space as well, where the original split them on whole units
// (800837AC) and drew the pieces in a second pass.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/psx.h"
#include "game/game.h"
#include "render/gpu_hw.h"

namespace {

constexpr uint32_t kPacketPtr = 0x800FF668;  // next free packet (24-bit address)
constexpr uint32_t kPacketEnd = 0x800FF374;
constexpr uint32_t kScreenBuf = 0x800FFAC0;  // per vertex (8 bytes): SXY, view z | clip flags << 16
constexpr uint32_t kPalette = 0x800FFB04;    // colour table (gouraud polygons)
constexpr uint32_t kClipConsts = 0x800FFB08; // 800809B0's screen/near/far bounds: [8] = near | far << 16
constexpr uint32_t kOtBase = 0x800FFB30;
constexpr uint32_t kClipPlane = 0x800FFB10;   // the clip plane: normal x | y << 16, z | h << 16 (8007EB74)
constexpr uint32_t kOutOfPlane = 0x00800000;  // vertex word +4: on the plane's cut side (8008128C)
constexpr uint32_t kLights = 0x800F3E70;       // per-vertex light / env-map UVs, same offsets as vertices
constexpr uint32_t kView = 8000;               // view x | y << 16 of each vertex, after the screen buffer
constexpr uint32_t S = 0x1F800000;             // scratchpad parameters (see the notes)

uint32_t rd(R3000* c, uint32_t a) { return MEM_R32(c, a); }
void wr(R3000* c, uint32_t a, uint32_t v) { MEM_W32(c, a, v); }
uint32_t phys(uint32_t a) { return a & 0x1FFFFF; }

// The GTE's colour commands on the emulated GTE's registers (bit-exact with the original).
enum : uint32_t { kDPCS = 0x0780010, kDPCL = 0x0680029, kDPCT = 0x0F8002A };
enum { kIR0 = 8, kIR1 = 9, kRGBC = 6, kRGB0 = 20, kRGB2 = 22 };

struct Vert {
    float x, y, z;    // view space (z = depth)
    float sx, sy;     // screen
    bool exact;       // sx, sy, z exact (else from whole units)
    float u, v;       // texel
    uint32_t col;     // colour word (with the GTE's code byte)
};

struct Poly {          // what every piece of one polygon shares
    uint32_t flags;    // masked record flags
    uint32_t rec;      // the record
    bool textured, gouraud;
    uint32_t cmd;      // GP0 command byte (0x20..0x3C)
    uint32_t uvhi[3];  // the texture words' high halves: clut, tpage (+ semi mode), 0
    uint32_t slot;     // ordering-table entry
};

struct Ctx {
    R3000* c;
    uint32_t p;        // packet pointer (24-bit)
    uint32_t at;       // the end the original checks against (80081960: a 24-bit pointer below the
                       // raw word at 800FF374 - 100, which holds a 0x80.. address: never reached)
    float ofx, ofy, h, ws;
    float znear;
    bool full = false;  // out of packet space
};

void project(const Ctx& k, Vert& v) {
    v.sx = k.ofx + k.h * k.ws * v.x / v.z;
    v.sy = k.ofy + k.h * v.y / v.z;
    v.exact = true;
}

uint32_t sxy_word(const Vert& v) {
    const int x = (int)std::clamp(std::lround(v.sx), -1024L, 1023L), y = (int)std::clamp(std::lround(v.sy), -1024L, 1023L);
    return (uint32_t)(x & 0xFFFF) | (uint32_t)y << 16;
}

// One packet of nv (3 or 4) vertices, linked into its ordering-table entry as the original links
// (texture window words around it, semi-transparency mode packet before untextured ones).
void emit(Ctx& k, const Poly& p, const Vert* v, int nv) {
    R3000* c = k.c;
    const int words = 1 + nv * (1 + (p.textured ? 1 : 0)) + (p.gouraud ? nv - 1 : 0);
    if (k.p >= k.at) {  // (as the original compares: see Ctx::at)
        k.full = true;
        return;
    }
    const uint32_t P = k.p & 0xFFFFFF;
    const uint32_t cmd = (nv == 4 ? 0x08u : 0u) | p.cmd;
    const uint32_t semi = (p.flags & 0x40) << 19;
    uint32_t a = P + 4;
    wr(c, a, v[0].col | cmd << 24 | semi), a += 4;
    for (int i = 0; i < nv; i++) {
        if (p.gouraud && i > 0) wr(c, a, v[i].col), a += 4;
        const uint32_t xy = sxy_word(v[i]);
        wr(c, a, xy);
        PgxpVal& s = pgxp_ram[phys(a) >> 2];
        s = v[i].exact && v[i].z > 0 ? PgxpVal{xy, v[i].sx, v[i].sy, v[i].z} : PgxpVal{};
        a += 4;
        if (p.textured) {
            const int u = std::clamp((int)std::lround(v[i].u), 0, 255), t = std::clamp((int)std::lround(v[i].v), 0, 255);
            const uint32_t uv = (uint32_t)u | (uint32_t)t << 8 | p.uvhi[std::min(i, 2)];
            wr(c, a, uv);
            pgxp_ram[phys(a) >> 2] = PgxpVal{uv, v[i].u, v[i].v, -1.0f};  // exact texture coordinates
            a += 4;
        }
    }
    const uint32_t e = rd(c, kOtBase) + p.slot;
    uint32_t size = (uint32_t)(words - 0) << 24;  // words after the tag
    if (p.flags & 0x20) {  // texture window: set before, reset after (80082104)
        size += 0x03000000;
        wr(c, P, rd(c, e) | size);
        uint32_t q = P + 4 + (size >> 22);
        wr(c, q - 12, 0), wr(c, q - 8, 0xE2000000), wr(c, q - 4, 0);
        wr(c, q, P | 0x02000000), wr(c, q + 4, rd(c, p.rec + 32)), wr(c, q + 8, 0);
        wr(c, e, q);
        k.p = q + 12;
        return;
    }
    wr(c, P, rd(c, e) | size);
    wr(c, e, P);
    uint32_t q = P + 4 + (size >> 22);
    if ((p.flags & 0x41) == 0x40) {  // semi-transparent, untextured: its mode first (80082154)
        wr(c, q + 4, 0xE1000200 | ((p.flags & 0x180) >> 2));
        wr(c, q, 0x01000000 | P);
        wr(c, e, q);
        q += 8;
    }
    k.p = q;
}

constexpr int kMaxCorners = 8;  // a quad cut by two planes has at most 6

// A polygon's corners in outline order (PS1 quads are 0-1-2 and 1-2-3: the outline is 0-1-3-2).
int outline(const Vert* v, int nv, Vert* out) {
    static const int kOrder[4] = {0, 1, 3, 2};
    for (int i = 0; i < nv; i++) out[i] = v[nv == 4 ? kOrder[i] : i];
    return nv;
}

// The part of outline `in` where dist >= 0 (dist: per corner), corners on cut edges in view space.
int clip(const Ctx& k, const Poly& p, const Vert* in, const float* dist, int n, Vert* out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const Vert& a = in[i];
        const Vert& b = in[j];
        const bool ai = dist[i] >= 0, bi = dist[j] >= 0;
        if (ai) out[m++] = a;
        if (ai == bi) continue;
        const float t = dist[i] / (dist[i] - dist[j]);
        Vert& q = out[m++];
        q.x = a.x + (b.x - a.x) * t, q.y = a.y + (b.y - a.y) * t, q.z = a.z + (b.z - a.z) * t;
        q.u = a.u + (b.u - a.u) * t, q.v = a.v + (b.v - a.v) * t;
        q.col = a.col;
        if (p.gouraud) {
            q.col = a.col & 0xFF000000;
            for (int s = 0; s < 24; s += 8)
                q.col |= (uint32_t)std::lround((a.col >> s & 0xFF) + ((int)(b.col >> s & 0xFF) - (int)(a.col >> s & 0xFF)) * t) << s;
        }
        q.exact = false;  // projected by the caller (a cut at the near plane is not projected yet)
    }
    return m;
}

// An outline clipped at the near plane in view space and drawn as triangles.
void emit_outline(Ctx& k, const Poly& p, const Vert* in, int n) {
    float d[kMaxCorners];
    for (int i = 0; i < n; i++) d[i] = in[i].z - k.znear;
    Vert out[kMaxCorners + 2];
    const int m = clip(k, p, in, d, n, out);
    for (int i = 0; i < m; i++) {
        if (out[i].exact) continue;
        out[i].z = std::max(out[i].z, k.znear);
        project(k, out[i]);
    }
    for (int i = 1; i + 1 < m && !k.full; i++) {
        const Vert t[3] = {out[0], out[i], out[i + 1]};
        emit(k, p, t, 3);
    }
}

int32_t nclip(const Vert& a, const Vert& b, const Vert& d, uint32_t wa, uint32_t wb, uint32_t wd) {
    auto sx = [](uint32_t w) { return (int64_t)(int16_t)(w & 0xFFFF); };
    auto sy = [](uint32_t w) { return (int64_t)(int16_t)(w >> 16); };
    int64_t m = sx(wa) * sy(wb) + sx(wb) * sy(wd) + sx(wd) * sy(wa) - sx(wa) * sy(wd) - sx(wb) * sy(wa) - sx(wd) * sy(wb);
    // as gte.cpp's NCLIP: the exact area's sign where the whole pixels got it wrong
    if (a.exact && b.exact && d.exact) {
        const double e = ((double)b.sx - a.sx) * ((double)d.sy - a.sy) - ((double)d.sx - a.sx) * ((double)b.sy - a.sy);
        if ((e > 0.5 && m <= 0) || (e < -0.5 && m >= 0)) m = e > 0 ? 1 : -1;
    }
    return (int32_t)m;
}

}  // namespace

// 800817FC(a0 = polygon records, a2 = count): nonzero = drawn here.
extern "C" int psx_hook_mesh(R3000* c) {
    if (!hw_active()) return 0;
    motion_camera(c);  // (frame interpolation: the frame's camera)
    uint32_t rec = c->r[4], count = c->r[6];
    if (!count) return 1;
    Ctx k;
    k.c = c;
    k.p = rd(c, kPacketPtr) & 0xFFFFFF;
    k.at = rd(c, kPacketEnd) - 100;
    k.ofx = (float)(int32_t)gte_read_ctrl(c, 24) / 65536.0f;
    k.ofy = (float)(int32_t)gte_read_ctrl(c, 25) / 65536.0f;
    k.h = (float)(uint16_t)gte_read_ctrl(c, 26);
    k.ws = g_gte_widescreen;
    k.znear = std::max(1.0f, (float)(int16_t)(rd(c, rd(c, kClipConsts) + 8) & 0xFFFF));
    // the GTE state the original sets up (colours: fog from DQ, no translation)
    for (int r = 5; r <= 7; r++) gte_write_ctrl(c, r, 0);
    gte_write_ctrl(c, 28, 0x10000000), gte_write_ctrl(c, 27, 0);
    wr(c, S + 484, 0x80082FAC), wr(c, S + 500, 0);
    const uint32_t screen = rd(c, kScreenBuf), palette = rd(c, kPalette);
    const uint32_t mask = rd(c, S + 324);
    // the clip plane (only vertices 8008128C marked are cut; the rest stay whole)
    const uint32_t pl0 = rd(c, kClipPlane), pl1 = rd(c, kClipPlane + 4);
    const float pnx = (float)(int16_t)(pl0 & 0xFFFF), pny = (float)(int16_t)(pl0 >> 16);
    const float pnz = (float)(int16_t)(pl1 & 0xFFFF), ph = (float)(int16_t)(pl1 >> 16);
    struct Piece {  // a polygon the plane cut: drawn after the rest, as the original's second pass
        Poly p;
        Vert v[kMaxCorners];
        int n;
    };
    std::vector<Piece> pieces;
    for (; count-- && !k.full; ) {
        const uint32_t w0 = rd(c, rec);
        const uint32_t flags = (w0 & (uint32_t)((int32_t)mask >> 16)) | (mask & 0xFFFF);
        const uint32_t this_rec = rec;
        rec += flags >> 16;
        if (!(flags & 0xC0)) continue;
        uint32_t idx[4] = {rd(c, this_rec + 4) & 0xFFFF, rd(c, this_rec + 4) >> 16, rd(c, this_rec + 8) & 0xFFFF, rd(c, this_rec + 8) >> 16};
        uint32_t sxy[4], zf[4];
        Vert v[4];
        for (int i = 0; i < 4; i++) {
            sxy[i] = rd(c, screen + idx[i]);
            zf[i] = rd(c, screen + idx[i] + 4);
            const uint32_t vxy = rd(c, screen + idx[i] + kView);
            Vert& q = v[i];
            q.x = (float)(int16_t)(vxy & 0xFFFF), q.y = (float)(int16_t)(vxy >> 16), q.z = (float)(int16_t)(zf[i] & 0xFFFF);
            q.sx = (float)(int16_t)(sxy[i] & 0xFFFF), q.sy = (float)(int16_t)(sxy[i] >> 16);
            q.exact = false;
            const PgxpVal& s = pgxp_ram[phys(screen + idx[i]) >> 2];
            if (s.w > 0 && s.v == sxy[i]) {  // the exact projection (gte.cpp): view position back from it
                q.sx = s.x, q.sy = s.y, q.z = s.w, q.exact = true;
                q.x = (s.x - k.ofx) * s.w / (k.h * k.ws), q.y = (s.y - k.ofy) * s.w / k.h;
            }
        }
        // culling (80081934..80081958)
        const uint32_t cw = ((zf[0] & zf[1] & zf[2] & zf[3]) >> 16) ^ 0xFF00;
        if (cw & 0x20FF) continue;
        const bool near = cw & 0x5000;
        if (near) {
            if (!(flags & 0x1000)) continue;
            // facing, in view space (80082274: (A x B) . C, whole units)
            auto vi = [&](int i) {
                const uint32_t xy = rd(c, screen + idx[i] + kView);
                return std::array<int64_t, 3>{(int16_t)(xy & 0xFFFF), (int16_t)(xy >> 16), (int16_t)(zf[i] & 0xFFFF)};
            };
            const auto A = vi(0), B = vi(1), C = vi(2);
            const int64_t n0 = A[1] * B[2] - A[2] * B[1], n1 = A[2] * B[0] - A[0] * B[2], n2 = A[0] * B[1] - A[1] * B[0];
            if (C[0] * n0 + C[1] * n1 + C[2] * n2 < 0) continue;
        } else if (nclip(v[0], v[1], v[2], sxy[0], sxy[1], sxy[2]) <= 0 &&
                   nclip(v[1], v[2], v[3], sxy[1], sxy[2], sxy[3]) >= 0) {
            continue;
        }
        if (k.p >= k.at) break;
        const bool tri = flags & 0x10;
        const int nv = tri ? 3 : 4;
        // the clip plane: the polygon's part on the kept side, its corners cut in view space
        bool cut = false;
        float pd[4];
        for (int i = 0; i < nv; i++) {
            const bool out = zf[i] & kOutOfPlane;
            cut |= out;
            const float e = (pnx * v[i].x + pny * v[i].y + pnz * v[i].z) / 4096.0f + ph;  // > 0: kept
            pd[i] = out ? std::min(e, -1e-3f) : std::max(e, 1e-3f);  // (the marks decide the side)
        }
        // sort depth: the largest view z (80081970); of a cut polygon, of its kept part
        int32_t t6 = (int32_t)(zf[0] << 16);
        for (int i = 1; i < 4; i++)
            if ((int32_t)(t6 - (int32_t)(zf[i] << 16)) < 0) t6 = (int32_t)(zf[i] << 16);
        t6 >>= 16;
        Vert ring[4], kept[kMaxCorners];
        float od[4];
        int nkept = 0;
        if (cut) {
            static const int kOrder[4] = {0, 1, 3, 2};
            for (int i = 0; i < nv; i++) od[i] = pd[tri ? i : kOrder[i]];
            Poly none{};
            nkept = clip(k, none, ring, od, outline(v, nv, ring), kept);
            if (nkept < 3) continue;
            float zmax = 0;
            for (int i = 0; i < nkept; i++) zmax = std::max(zmax, kept[i].z);
            t6 = (int32_t)zmax;
        }
        // fog (800819A8)
        const int32_t d = t6 - (int32_t)rd(c, S + 356), sh = (int32_t)rd(c, S + 372);
        const int32_t f = d <= 0 ? 0 : sh >= 0 ? (int32_t)((uint32_t)d << (sh & 31)) : d >> ((-sh) & 31);
        gte_write_data(c, kIR0, (uint32_t)(int32_t)(int16_t)f);
        // colours (section 4 of the notes)
        const bool lit4 = flags & 0x04, lit8 = flags & 0x08;
        Poly p;
        p.flags = flags, p.rec = this_rec;
        p.gouraud = flags & 0x0800;
        auto light_ir = [&](uint32_t entry) {
            gte_write_data(c, kIR1, rd(c, entry)), gte_write_data(c, kIR1 + 1, (uint32_t)((int32_t)rd(c, entry) >> 16));
            gte_write_data(c, kIR1 + 2, rd(c, entry + 4));
        };
        auto global_ir = [&] {
            for (int i = 0; i < 3; i++) gte_write_data(c, kIR1 + i, rd(c, S + 420 + 16 * i));
        };
        auto pal = [&](int i) { return rd(c, palette + 4 * (rd(c, this_rec + 12) >> (8 * i) & 0xFF)); };
        if (!p.gouraud) {
            gte_write_data(c, kRGBC, rd(c, this_rec + 12));
            if (lit4 && lit8) {  // per-vertex light: drawn gouraud
                for (int i = 0; i < nv; i++) {
                    light_ir(kLights + idx[i]);
                    gte_command(c, kDPCL);
                    v[i].col = gte_read_data(c, kRGB2);
                }
                p.gouraud = true;
            } else {
                if (!lit4 && !lit8) gte_command(c, kDPCS);
                else {
                    if (lit8) global_ir();
                    else light_ir(kLights + rd(c, this_rec + 16));
                    gte_command(c, kDPCL);
                }
                for (int i = 0; i < nv; i++) v[i].col = gte_read_data(c, kRGB2);
            }
        } else if (!lit4 && !lit8) {
            if (tri) {
                for (int i = 0; i < 3; i++) gte_write_data(c, kRGB0 + i, pal(i));
                gte_command(c, kDPCT);
                for (int i = 0; i < 3; i++) v[i].col = gte_read_data(c, kRGB0 + i);
            } else {
                gte_write_data(c, kRGBC, pal(0));
                gte_command(c, kDPCS);
                v[0].col = gte_read_data(c, kRGB2);
                for (int i = 0; i < 3; i++) gte_write_data(c, kRGB0 + i, pal(i + 1));
                gte_command(c, kDPCT);
                for (int i = 0; i < 3; i++) v[i + 1].col = gte_read_data(c, kRGB0 + i);
            }
        } else {
            for (int i = 0; i < nv; i++) {
                gte_write_data(c, kRGBC, pal(i));
                if (lit4 && lit8) light_ir(kLights + idx[i]);
                else if (lit8) global_ir();
                else light_ir(kLights + rd(c, this_rec + 16));
                gte_command(c, kDPCL);
                v[i].col = gte_read_data(c, kRGB2);
            }
        }
        // texture words: the record's, or the environment map's (flat, untextured, 0x0002)
        uint32_t t1 = 0, t2 = 0, t3 = 0;
        p.textured = flags & 0x01;
        if (p.textured) {
            t1 = rd(c, this_rec + 20), t2 = rd(c, this_rec + 24), t3 = rd(c, this_rec + 28);
        } else if ((flags & 0x02) && !(flags & 0x0800) && !(lit4 && lit8)) {
            t1 = rd(c, kLights + idx[0] + 4) | rd(c, S + 388);
            t2 = rd(c, kLights + idx[1] + 4) | rd(c, S + 404);
            t3 = rd(c, kLights + idx[2] + 4) | (tri ? 0 : rd(c, kLights + idx[3] + 4) << 16);
            p.textured = true;
        }
        if (p.textured) {
            t2 |= (flags & 0x180) << 14;
            const uint32_t uv[4] = {t1 & 0xFFFF, t2 & 0xFFFF, t3 & 0xFFFF, t3 >> 16};
            for (int i = 0; i < nv; i++) v[i].u = (float)(uv[i] & 0xFF), v[i].v = (float)(uv[i] >> 8 & 0xFF);
            p.uvhi[0] = t1 & 0xFFFF0000, p.uvhi[1] = t2 & 0xFFFF0000, p.uvhi[2] = 0;
        }
        p.cmd = 0x20 | (p.gouraud ? 0x10 : 0) | (p.textured ? 0x04 : 0);
        // ordering-table entry (80082030)
        int32_t z = t6;
        if (flags & 0x6000) z += (int32_t)rd(c, S + ((flags & 0x2000) ? 516 : 532));
        p.slot = (uint32_t)z < 16384 ? (uint32_t)z & 0xFFFC : 16380;
        if (cut) {  // now with colours and texture coordinates
            Piece& pc = pieces.emplace_back();
            pc.p = p;
            pc.n = clip(k, p, ring, od, outline(v, nv, ring), pc.v);
            for (int i = 0; i < pc.n; i++)
                if (!pc.v[i].exact && pc.v[i].z >= k.znear) project(k, pc.v[i]);
            continue;
        }
        if (near) {
            Vert ol[4];
            emit_outline(k, p, ol, outline(v, nv, ol));
            continue;
        }
        emit(k, p, v, nv);  // (any size: every vertex exact, gpu.cpp draws it as it is)
    }
    for (const Piece& pc : pieces) {
        if (k.full) break;
        emit_outline(k, pc.p, pc.v, pc.n);
    }
    wr(c, kPacketPtr, k.p & 0xFFFFFF);
    c->r[2] = k.p & 0xFFFFFF;
    return 1;
}
