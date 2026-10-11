// The part of the hardware renderers that OpenGL (gpu_gl.cpp) and Vulkan (gpu_vk.cpp) share:
// the scale and sample settings, the vertex format, primitives batched while their drawing state
// stays the same, and the VRAM region drawn since `read` (the copy textures are sampled from)
// was last synchronised with `draw` (the render target). The primitive functions of gpu_hw.h
// are defined here once; each backend sets the two functions below in its hw_init.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "render/gpu_hw.h"

namespace hwc {

extern int S;                    // VRAM scale; 0 = no hardware renderer
extern int g_samples;            // MSAA samples in use
extern int g_samples_wanted;     // ... asked for (applies at the next hw_init)
extern bool g_filter;            // texture smoothing
constexpr int kHdW = 8192, kHdH = 4096;  // replacements' atlas: VRAM's texture area x 4 (4-bit above, 8-bit below)

struct Vtx {
    float x, y;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t info0, info1;  // texture page, depth and flags; palette (the shaders' v_info)
    float w;                // 1, or view depth for perspective-correct texturing (HwVtx::w)
    uint32_t box;           // HwPrim::box
    float z;                // Z-buffer depth (HwVtx::z)
};
static_assert(sizeof(Vtx) == 40, "vertex layout (gpu_gl.cpp attributes, vk_context.cpp kVtxStride)");

// The drawing state a batch shares.
struct BatchKey {
    bool semi = false;
    int semi_mode = 0;
    bool set_mask = false;
    int ztest = 0;    // HwPrim::ztest
    int tw[4] = {};   // texture window mask x, y, offset x, y
    int sc[4] = {};   // scissor x1, y1, x2, y2 (native, inclusive)
    bool operator==(const BatchKey& o) const {
        return semi == o.semi && (!semi || semi_mode == o.semi_mode) && set_mask == o.set_mask && ztest == o.ztest &&
               std::memcmp(tw, o.tw, sizeof tw) == 0 && std::memcmp(sc, o.sc, sizeof sc) == 0;
    }
};
extern std::vector<Vtx> g_batch;
extern BatchKey g_key;

// Region drawn since `read` was last synchronised (native pixels, exclusive max).
extern bool g_dirty;
extern int g_dx0, g_dy0, g_dx1, g_dy1;
void mark_dirty(int x0, int y0, int x1, int y1);
bool overlaps_dirty(int x0, int y0, int x1, int y1);

// The backend: draws and empties g_batch; makes `read` match `draw` (flushing first) and clears g_dirty.
extern void (*flush)();
extern void (*sync_read)();

uint32_t rgba_of(uint16_t p);  // a VRAM pixel as R G B A bytes (alpha = bit 15)

// Frame interpolation (gpu_hw.h hw_rec_*): a frame's primitives, fills and depth clears in order,
// each vertex with its motion (HwVtx::dx, dy, dr), to be drawn again by the backend's hw_replay.
struct Mot {
    float dx, dy, dr;
};
struct RecOp {
    enum Kind : uint8_t { Batch, Fill, Depth } kind;
    BatchKey key;            // Batch: its drawing state
    bool filter = false;     // Batch: texture smoothing
    uint32_t first = 0, count = 0;  // Batch: its vertices
    int x = 0, y = 0, w = 0, h = 0;  // Fill
    uint16_t color = 0;
};
struct RecFrame {
    std::vector<Vtx> v;
    std::vector<Mot> m;
    std::vector<RecOp> ops;
    std::vector<std::array<int, 4>> touched;  // VRAM written other than by drawing, or drawn and then sampled
    bool moving = false;    // some vertex moved
    int x = 0, y = 0, w = 0, h = 0;  // where it is shown (finished frames)
};
const RecFrame* rec_frame(int id);  // a finished frame (nullptr: gone)
// A recorded vertex at phase t of its motion (1: where it is now).
inline Vtx rec_vertex(const Vtx& v, const Mot& m, float t) {
    const float back = 1.0f - t, f = 1.0f + back * (m.dr - 1.0f);
    Vtx o = v;
    o.x += back * m.dx, o.y += back * m.dy, o.w *= f, o.z *= f;
    return o;
}

}  // namespace hwc
