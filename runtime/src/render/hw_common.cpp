#include "render/hw_common.h"

#include <algorithm>
#include <cmath>

#include "core/psx.h"

namespace hwc {

int S = 0;
int g_samples = 1;
int g_samples_wanted = 1;
bool g_filter = false;
std::vector<Vtx> g_batch;
BatchKey g_key;
bool g_dirty = false;
int g_dx0, g_dy0, g_dx1, g_dy1;
void (*flush)() = [] {};
void (*sync_read)() = [] {};

uint32_t rgba_of(uint16_t p) {
    uint32_t r = (p & 31) * 255 / 31, g = ((p >> 5) & 31) * 255 / 31, b = ((p >> 10) & 31) * 255 / 31;
    uint32_t a = (p & 0x8000) ? 255 : 0;
    return r | g << 8 | b << 16 | a << 24;
}

void mark_dirty(int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, 1024); y1 = std::min(y1, 512);
    if (x0 >= x1 || y0 >= y1) return;
    if (!g_dirty) {
        g_dirty = true;
        g_dx0 = x0; g_dy0 = y0; g_dx1 = x1; g_dy1 = y1;
    } else {
        g_dx0 = std::min(g_dx0, x0); g_dy0 = std::min(g_dy0, y0);
        g_dx1 = std::max(g_dx1, x1); g_dy1 = std::max(g_dy1, y1);
    }
}

bool overlaps_dirty(int x0, int y0, int x1, int y1) {
    return g_dirty && x0 < g_dx1 && x1 > g_dx0 && y0 < g_dy1 && y1 > g_dy0;
}

// Frame interpolation: the frame being recorded, and the last finished ones.
bool g_rec_on = false;
RecFrame g_rec;
constexpr int kRecFrames = 4;
RecFrame g_rec_done[kRecFrames];
int g_rec_next = 0;

const RecFrame* rec_frame(int id) { return id >= 0 && id < kRecFrames ? &g_rec_done[id] : nullptr; }

namespace {

void rec_touch(int x0, int y0, int x1, int y1) {
    if (g_rec_on) g_rec.touched.push_back({x0, y0, x1, y1});
}

// A primitive starts: if it samples VRAM drawn since the last sync, sync first; a new drawing
// state ends the batch.
void begin_prim(const HwPrim& p, const HwState& s) {
    static bool warned_mask = false;
    if (s.check_mask && !warned_mask) {
        warned_mask = true;
        log_msg(LogCat::Gpu, "hardware renderer: mask-bit test is not emulated");
    }
    if (p.textured && g_dirty) {
        const int pw = p.depth == 0 ? 64 : p.depth == 1 ? 128 : 256;
        bool hit = overlaps_dirty(p.page_x, p.page_y, p.page_x + pw, p.page_y + 256);
        if (p.depth < 2) hit = hit || overlaps_dirty(p.clut_x, p.clut_y, p.clut_x + (p.depth == 0 ? 16 : 256), p.clut_y + 1);
        if (hit) {
            sync_read();
            // (drawn and then used as a texture: a picture of the frame itself cannot be redrawn)
            rec_touch(p.page_x, p.page_y, p.page_x + pw, p.page_y + 256);
        }
    }
    BatchKey k;
    k.semi = p.semi;
    k.semi_mode = p.semi_mode;
    k.set_mask = s.set_mask;
    k.ztest = p.ztest;
    k.tw[0] = s.tw_mask_x; k.tw[1] = s.tw_mask_y; k.tw[2] = s.tw_off_x; k.tw[3] = s.tw_off_y;
    k.sc[0] = s.area_x1; k.sc[1] = s.area_y1; k.sc[2] = s.area_x2; k.sc[3] = s.area_y2;
    if (!(k == g_key)) {
        flush();
        g_key = k;
    }
}

void push(float x, float y, int r, int g, int b, float u, float v, const HwPrim& p, float w = 1.0f, float z = 0.0f,
          Mot m = {0, 0, 1}) {
    const uint32_t info0 = (uint32_t)(p.page_x & 1023) | (uint32_t)(p.page_y & 511) << 10 | (uint32_t)p.depth << 20 |
                           (uint32_t)p.textured << 22 | (uint32_t)p.raw << 23 | (uint32_t)p.smooth << 24 |
                           (uint32_t)p.hd << 27 | (uint32_t)p.glyph << 28;
    const uint32_t info1 = (uint32_t)(p.clut_x & 1023) | (uint32_t)(p.clut_y & 511) << 10;
    g_batch.push_back(Vtx{x, y, (uint8_t)r, (uint8_t)g, (uint8_t)b, 255, u, v, info0, info1, w, p.box, z});
    if (!g_rec_on) return;
    if (g_rec.ops.empty() || g_rec.ops.back().kind != RecOp::Batch || !(g_rec.ops.back().key == g_key) ||
        g_rec.ops.back().filter != g_filter) {
        RecOp op{RecOp::Batch, g_key};
        op.filter = g_filter, op.first = (uint32_t)g_rec.v.size();
        g_rec.ops.push_back(op);
    }
    g_rec.ops.back().count++;
    g_rec.v.push_back(g_batch.back());
    g_rec.m.push_back(m);
}

// The drawn area, clipped to the drawing area, becomes dirty.
void draw_area_dirty(int x0, int y0, int x1, int y1, const HwState& s) {
    mark_dirty(std::max(x0, s.area_x1), std::max(y0, s.area_y1), std::min(x1, s.area_x2 + 1), std::min(y1, s.area_y2 + 1));
}

}  // namespace
}  // namespace hwc

using namespace hwc;

bool hw_active() { return S != 0; }
int hw_scale() { return S; }
int hw_samples() { return S ? g_samples : 0; }
void hw_set_samples(int n) { g_samples_wanted = std::clamp(n, 1, 8); }
int hw_samples_wanted() { return g_samples_wanted; }

void hw_set_texture_filter(bool on) {
    if (on != g_filter) flush();
    g_filter = on;
}

void hw_triangle(const HwVtx* a, const HwVtx* b, const HwVtx* c, const HwPrim& p, const HwState& s) {
    begin_prim(p, s);
    for (const HwVtx* v : {a, b, c}) {
        const HwVtx* col = p.gouraud ? v : a;
        push(v->x, v->y, col->r, col->g, col->b, v->u, v->v, p, v->w, v->z, {v->dx, v->dy, v->dr});
    }
    draw_area_dirty((int)std::floor(std::min({a->x, b->x, c->x})), (int)std::floor(std::min({a->y, b->y, c->y})),
                    (int)std::ceil(std::max({a->x, b->x, c->x})) + 1, (int)std::ceil(std::max({a->y, b->y, c->y})) + 1, s);
}

void hw_rect(int x, int y, int w, int h, int u0, int v0, bool flip_x, bool flip_y, uint32_t color, const HwPrim& p,
             const HwState& s) {
    if (w <= 0 || h <= 0) return;
    begin_prim(p, s);
    const int r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
    const float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    const float ua = (float)u0, va = (float)v0;
    const float ub = flip_x ? ua - (float)w : ua + (float)w, vb = flip_y ? va - (float)h : va + (float)h;
    push(x0, y0, r, g, b, ua, va, p);
    push(x1, y0, r, g, b, ub, va, p);
    push(x0, y1, r, g, b, ua, vb, p);
    push(x1, y0, r, g, b, ub, va, p);
    push(x1, y1, r, g, b, ub, vb, p);
    push(x0, y1, r, g, b, ua, vb, p);
    draw_area_dirty(x, y, x + w, y + h, s);
}

void hw_line(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, bool gouraud, const HwPrim& p, const HwState& s) {
    begin_prim(p, s);
    if (!gouraud) c1 = c0;
    const int r0 = c0 & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = (c0 >> 16) & 0xFF;
    const int r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
    // a one-pixel-wide quad along the major axis, covering both end pixels
    float ax = x0, ay = y0, bx = x1, by = y1;
    const bool xmajor = std::abs(x1 - x0) >= std::abs(y1 - y0);
    const float px = xmajor ? 0.0f : 1.0f, py = xmajor ? 1.0f : 0.0f;  // the quad's width
    if (xmajor) {
        if (bx >= ax) bx += 1; else ax += 1;
    } else {
        if (by >= ay) by += 1; else ay += 1;
    }
    push(ax, ay, r0, g0, b0, 0, 0, p); push(bx, by, r1, g1, b1, 0, 0, p); push(ax + px, ay + py, r0, g0, b0, 0, 0, p);
    push(bx, by, r1, g1, b1, 0, 0, p); push(bx + px, by + py, r1, g1, b1, 0, 0, p); push(ax + px, ay + py, r0, g0, b0, 0, 0, p);
    draw_area_dirty((int)std::floor(std::min(x0, x1)), (int)std::floor(std::min(y0, y1)), (int)std::ceil(std::max(x0, x1)) + 1,
                    (int)std::ceil(std::max(y0, y1)) + 1, s);
}

// ================================================================ frame interpolation

void hw_rec_enable(bool on) {
    if (on == g_rec_on) return;
    g_rec_on = on;
    g_rec = RecFrame{};
}

void hw_rec_note_fill(int x, int y, int w, int h, uint16_t color) {
    if (!g_rec_on) return;
    RecOp op{RecOp::Fill};
    op.x = x, op.y = y, op.w = w, op.h = h, op.color = color;
    g_rec.ops.push_back(op);
}

void hw_rec_note_depth() {
    if (g_rec_on) g_rec.ops.push_back(RecOp{RecOp::Depth});
}

void hw_rec_note_write(int x, int y, int w, int h) { rec_touch(x, y, x + w, y + h); }

int hw_rec_finish(int x, int y, int w, int h) {
    if (!g_rec_on) return -1;
    if (g_rec.ops.empty()) return -2;
    bool ok = w > 0 && h > 0 && x + w <= 1024 && y + h <= 512;
    for (const auto& r : g_rec.touched)  // (a few pixels do not count: the game puts 2 at 0,0 every other frame)
        ok &= !(r[0] < x + w && r[2] > x && r[1] < y + h && r[3] > y && (r[2] - r[0]) * (r[3] - r[1]) >= 64);
    int id = -1;
    if (ok) {
        id = g_rec_next;
        g_rec_next = (g_rec_next + 1) % kRecFrames;
        g_rec.x = x, g_rec.y = y, g_rec.w = w, g_rec.h = h;
        std::swap(g_rec_done[id], g_rec);
    }
    // (the vectors keep their memory for the next frame)
    g_rec.v.clear(), g_rec.m.clear(), g_rec.ops.clear(), g_rec.touched.clear();
    return id;
}
