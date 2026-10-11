// Shared pieces of the native 3D renderers (mesh.cpp, effects.cpp): the camera's view in float,
// world points projected exactly, and packet vertex words with their exact shadows (gpu.cpp
// draws a polygon whose every vertex word has a shadow at that exact position, perspective-correct).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/psx.h"

namespace n3d {

inline uint32_t rd(R3000* c, uint32_t a) { return MEM_R32(c, a); }
inline void wr(R3000* c, uint32_t a, uint32_t v) { MEM_W32(c, a, v); }
inline uint32_t phys(uint32_t a) { return a & 0x1FFFFF; }

constexpr uint32_t kPacketPtr = 0x800FF668;  // next free packet (24-bit address)
constexpr uint32_t kCamera = 0x800ED51C;     // the render camera: +4..+0xC position (whole units), +0x74 rotation
constexpr uint32_t kNear = 0x800FFAE8;       // near distance (view z)

// A point in view space (z = depth) and on the screen (valid when z > 0).
struct Pt {
    float x, y, z;
    float sx, sy;
};

// The camera's view as the effects see it: rotation (the GTE's RT, 4.12, aspect in row 1),
// whole-unit position, and the frame's projection (GTE OFX, OFY, H, widescreen factor).
struct View {
    double m[9];
    double cam[3];
    float ofx, ofy, h, ws;

    // a point in world units (20.12 / 4096, or whole units) to view space and the screen
    Pt see(double x, double y, double z) const { return rel(x - cam[0], y - cam[1], z - cam[2]); }
    // a point relative to the camera position
    Pt rel(double x, double y, double z) const {
        const double v[3] = {x, y, z};
        Pt p{};
        p.x = (float)((m[0] * v[0] + m[1] * v[1] + m[2] * v[2]) / 4096.0);
        p.y = (float)((m[3] * v[0] + m[4] * v[1] + m[5] * v[2]) / 4096.0);
        p.z = (float)((m[6] * v[0] + m[7] * v[1] + m[8] * v[2]) / 4096.0);
        project(p);
        return p;
    }
    void project(Pt& p) const {
        if (p.z <= 0) return;
        p.sx = ofx + h * ws * p.x / p.z;
        p.sy = ofy + h * p.y / p.z;
    }
};
inline View view_now(R3000* c) {
    View v;
    for (int i = 0; i < 5; i++) {
        const uint32_t r = rd(c, kCamera + 0x74 + 4 * i);
        v.m[2 * i] = (int16_t)(r & 0xFFFF);
        if (i < 4) v.m[2 * i + 1] = (int16_t)(r >> 16);
    }
    for (int i = 0; i < 3; i++) v.cam[i] = (int32_t)rd(c, kCamera + 4 + 4 * i);
    v.ofx = (float)(int32_t)gte_read_ctrl(c, 24) / 65536.0f;
    v.ofy = (float)(int32_t)gte_read_ctrl(c, 25) / 65536.0f;
    v.h = (float)(uint16_t)gte_read_ctrl(c, 26);
    v.ws = g_gte_widescreen;
    return v;
}
// a world point stored as three 20.12 words at a
inline Pt see_at(R3000* c, const View& v, uint32_t a) {
    return v.see((int32_t)rd(c, a) / 4096.0, (int32_t)rd(c, a + 4) / 4096.0, (int32_t)rd(c, a + 8) / 4096.0);
}

inline uint32_t sxy_word(float sx, float sy) {
    const int x = (int)std::clamp(std::lround(sx), -1024L, 1023L), y = (int)std::clamp(std::lround(sy), -1024L, 1023L);
    return (uint32_t)(x & 0xFFFF) | (uint32_t)y << 16;
}

// A packet's vertex word at a: the exact position (sx, sy) at view depth z (z <= 0: none).
inline void put_xy(R3000* c, uint32_t a, float sx, float sy, float z) {
    const uint32_t xy = sxy_word(sx, sy);
    wr(c, a, xy);
    pgxp_ram[phys(a) >> 2] = z > 0 ? PgxpVal{xy, sx, sy, z} : PgxpVal{};
}
inline void put_xy(R3000* c, uint32_t a, const Pt& p) { put_xy(c, a, p.sx, p.sy, p.z); }

}  // namespace n3d
