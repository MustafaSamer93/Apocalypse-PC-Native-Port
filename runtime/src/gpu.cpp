// PS1 GPU: command processing and a software rasterizer into 1024x512x16 VRAM.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gpu_hw.h"
#include "psx.h"

namespace {

constexpr int VRAM_W = 1024, VRAM_H = 512;
uint16_t g_vram[VRAM_W * VRAM_H];

inline uint16_t& vram(int x, int y) { return g_vram[(y & (VRAM_H - 1)) * VRAM_W + (x & (VRAM_W - 1))]; }

// ---------------------------------------------------------------- state

struct DrawState {
    int tpage_x = 0, tpage_y = 0;   // texture page base in VRAM pixels
    int semi_mode = 0;              // 0..3
    int tex_depth = 0;              // 0 = 4bpp, 1 = 8bpp, 2 = 15bpp
    bool dither = false;
    bool draw_to_display = false;
    bool tex_disable = false;
    bool rect_flip_x = false, rect_flip_y = false;
    uint32_t tex_window = 0;
    int tw_mask_x = 0, tw_mask_y = 0, tw_off_x = 0, tw_off_y = 0;
    int area_x1 = 0, area_y1 = 0, area_x2 = 0, area_y2 = 0;
    int off_x = 0, off_y = 0;
    bool set_mask = false, check_mask = false;
    uint32_t e1 = 0;
};
DrawState S;

struct DisplayState {
    bool enabled = false;
    int start_x = 0, start_y = 0;
    int h1 = 0x200, h2 = 0xC00, v1 = 0x10, v2 = 0x100;
    uint32_t mode = 0;
    int dma_dir = 0;
    bool irq = false;
};
DisplayState DS;

bool g_odd_line = false;
uint64_t g_vblanks = 0;

// ---------------------------------------------------------------- widescreen 2D
// With a widescreen aspect the GTE narrows the 3D view (gte.cpp) and the whole picture is
// stretched to the wider screen. The level render's 2D (HUD, messages) would be stretched
// with it, so it is narrowed by the same factor: it keeps its proportions. HUD pieces stay at
// the screen edge of their side (the panel in the top right corner); the pause menu and
// messages stay centred. Text and icons (the game's routines for them, also used by the
// menus) are narrowed around the centre wherever they are drawn. The 2D is what the render's last
// routines add; their packets come from one buffer in order (next free packet: 800FF668),
// so the packet address tells what made a primitive. Near full-width shapes (the red damage
// flash, fades) still cover the screen.
constexpr uint32_t kNextPacket = 0x800FF668;
uint32_t g_2d_from = 0, g_2d_mid = 0, g_2d_to = 0;  // packets: HUD from..mid, then menus/messages
                                                    // up to the tint's (phys)
// Packets the text and icon routines made since the last frame was drawn (phys from, to).
std::vector<std::pair<uint32_t, uint32_t>> g_text_packets;
bool text_packet(uint32_t a) {
    for (const auto& r : g_text_packets)
        if (a >= r.first && a < r.second) return true;
    return false;
}
uint64_t g_movie_vblank = 0;  // when the MDEC last decoded a movie frame (VBlank count)
bool g_movie_seen = false;

// Horizontal factor for a primitive spanning x0..x1 and the point that stays put (x' =
// fix + (x - fix) * factor); 1 = leave it.
float squeeze_2d(float x0, float x1, float* fix) {
    const uint32_t a = g_gpu_packet_addr;
    if (g_gte_widescreen == 1.0f || !a) return 1.0f;
    const bool text = text_packet(a);
    if (!text && (a < g_2d_from || a >= g_2d_to)) return 1.0f;
    const float left = (float)S.area_x1, right = (float)(S.area_x2 + 1), width = right - left;
    if (x1 - x0 >= 0.9f * width) return 1.0f;  // full-screen shapes
    const float mid = (x0 + x1) * 0.5f;
    const bool hud = a >= g_2d_from && a < g_2d_mid;  // HUD text and icons too
    if (hud && mid < left + width / 3) *fix = left;         // HUD, left side
    else if (hud && mid > right - width / 3) *fix = right;  // HUD, right side
    else *fix = (left + right) * 0.5f;
    return g_gte_widescreen;
}

// command FIFO accumulation
std::vector<uint32_t> g_cmd;
size_t g_cmd_need = 0;

// CPU<->VRAM transfer state
struct Transfer {
    bool active = false;
    int x = 0, y = 0, w = 0, h = 0;
    int cx = 0, cy = 0;
} g_upload, g_download;

uint32_t g_gpuread = 0;

// ---------------------------------------------------------------- pixel helpers

inline uint16_t blend(uint16_t back, uint16_t front, int mode) {
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, g, b;
    switch (mode) {
        case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
        case 1: r = std::min(31, br + fr); g = std::min(31, bg + fg); b = std::min(31, bb + fb); break;
        case 2: r = std::max(0, br - fr); g = std::max(0, bg - fg); b = std::max(0, bb - fb); break;
        default: r = std::min(31, br + (fr >> 2)); g = std::min(31, bg + (fg >> 2)); b = std::min(31, bb + (fb >> 2)); break;
    }
    return (uint16_t)(r | g << 5 | b << 10 | (front & 0x8000));
}

const int8_t kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

inline void plot(int x, int y, uint16_t color, bool semi) {
    if (x < S.area_x1 || x > S.area_x2 || y < S.area_y1 || y > S.area_y2) return;
    uint16_t& dst = vram(x, y);
    if (S.check_mask && (dst & 0x8000)) return;
    if (semi) color = blend(dst, color, S.semi_mode);
    dst = color | (S.set_mask ? 0x8000 : 0);
}

inline uint16_t rgb24_to_15(int r, int g, int b) {
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

inline uint16_t shade(int r, int g, int b, int x, int y, bool dither) {
    if (dither) {
        int d = kDither[y & 3][x & 3];
        r = std::clamp(r + d, 0, 255);
        g = std::clamp(g + d, 0, 255);
        b = std::clamp(b + d, 0, 255);
    }
    return rgb24_to_15(r, g, b);
}

struct TexCtx {
    int page_x, page_y, depth;
    int clut_x, clut_y;
};

inline uint16_t texel(const TexCtx& t, int u, int v) {
    u &= 0xFF;
    v &= 0xFF;
    u = (u & ~(S.tw_mask_x * 8)) | ((S.tw_off_x & S.tw_mask_x) * 8);
    v = (v & ~(S.tw_mask_y * 8)) | ((S.tw_off_y & S.tw_mask_y) * 8);
    switch (t.depth) {
        case 0: {
            uint16_t w = vram(t.page_x + (u >> 2), t.page_y + v);
            int idx = (w >> ((u & 3) * 4)) & 0xF;
            return vram(t.clut_x + idx, t.clut_y);
        }
        case 1: {
            uint16_t w = vram(t.page_x + (u >> 1), t.page_y + v);
            int idx = (w >> ((u & 1) * 8)) & 0xFF;
            return vram(t.clut_x + idx, t.clut_y);
        }
        default: return vram(t.page_x + u, t.page_y + v);
    }
}

// Texel * vertex colour / 128 (0x80 = unmodulated)
inline uint16_t modulate(uint16_t tx, int r, int g, int b, int x, int y, bool dither) {
    int tr = (tx & 31) << 3, tg = ((tx >> 5) & 31) << 3, tb = ((tx >> 10) & 31) << 3;
    int orr = (tr * r) >> 7, og = (tg * g) >> 7, ob = (tb * b) >> 7;
    uint16_t c = shade(std::min(orr, 255), std::min(og, 255), std::min(ob, 255), x, y, dither);
    return c | (tx & 0x8000);
}

// ---------------------------------------------------------------- triangles

struct Vtx {
    int x, y;
    int r, g, b;
    int u, v;
};

void set_tpage(uint32_t attr) {
    // tpage attribute from a textured primitive (lower 9 bits + tex disable)
    S.tpage_x = (attr & 0xF) * 64;
    S.tpage_y = ((attr >> 4) & 1) * 256;
    S.semi_mode = (attr >> 5) & 3;
    S.tex_depth = std::min<int>((attr >> 7) & 3, 2);
}

void draw_triangle(const Vtx* v0, const Vtx* v1, const Vtx* v2, bool gouraud, bool textured, bool raw,
                   bool semi, const TexCtx& tex) {
    // orient counter-clockwise for a positive area
    int64_t area = (int64_t)(v1->x - v0->x) * (v2->y - v0->y) - (int64_t)(v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0) return;
    if (area < 0) {
        std::swap(v1, v2);
        area = -area;
    }
    int minx = std::min({v0->x, v1->x, v2->x}), maxx = std::max({v0->x, v1->x, v2->x});
    int miny = std::min({v0->y, v1->y, v2->y}), maxy = std::max({v0->y, v1->y, v2->y});
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;  // hardware drops oversized polys
    minx = std::max(minx, S.area_x1);
    miny = std::max(miny, S.area_y1);
    maxx = std::min(maxx, S.area_x2 + 1);
    maxy = std::min(maxy, S.area_y2 + 1);
    if (minx >= maxx || miny >= maxy) return;

    // Edge function E(a,b,p) = cross(b-a, p-a). With area > 0, E(v1,v2,p), E(v2,v0,p) and
    // E(v0,v1,p) are all >= 0 inside the triangle and sum to `area` (barycentric weights).
    // Pixels exactly on an edge are drawn only for top/left edges, like the hardware,
    // which never draws the right and bottom edges.
    auto edge = [](const Vtx* a, const Vtx* b, int px, int py) -> int64_t {
        return (int64_t)(b->x - a->x) * (py - a->y) - (int64_t)(b->y - a->y) * (px - a->x);
    };
    auto bias = [](const Vtx* a, const Vtx* b) -> int {
        bool top_left = (a->y == b->y && b->x > a->x) || (b->y < a->y);
        return top_left ? 0 : -1;
    };
    const Vtx *A0 = v1, *B0 = v2, *A1 = v2, *B1 = v0, *A2 = v0, *B2 = v1;
    int bias0 = bias(A0, B0), bias1 = bias(A1, B1), bias2 = bias(A2, B2);

    // Attributes are interpolated exactly with the integer edge weights (w0+w1+w2 == area).
    // Floating point here produced values like 31.99999 for texel 32, sampling the
    // neighbouring texel/texture along polygon edges.
    auto interp = [&](int64_t w0, int64_t w1, int64_t w2, int a0, int a1, int a2) {
        return (int)((w0 * a0 + w1 * a1 + w2 * a2) / area);
    };
    auto interp_round = [&](int64_t w0, int64_t w1, int64_t w2, int a0, int a1, int a2) {
        return (int)((w0 * a0 + w1 * a1 + w2 * a2 + area / 2) / area);
    };
    bool dither = S.dither && (gouraud || (textured && !raw));
    int fr = v0->r, fg = v0->g, fb = v0->b;
    for (int y = miny; y < maxy; y++) {
        for (int x = minx; x < maxx; x++) {
            int64_t w0 = edge(A0, B0, x, y), w1 = edge(A1, B1, x, y), w2 = edge(A2, B2, x, y);
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
            int r = fr, g = fg, b = fb;
            if (gouraud) {
                r = interp_round(w0, w1, w2, v0->r, v1->r, v2->r);
                g = interp_round(w0, w1, w2, v0->g, v1->g, v2->g);
                b = interp_round(w0, w1, w2, v0->b, v1->b, v2->b);
            }
            uint16_t out;
            bool px_semi = semi;
            if (textured) {
                int u = interp(w0, w1, w2, v0->u, v1->u, v2->u);
                int vv = interp(w0, w1, w2, v0->v, v1->v, v2->v);
                uint16_t t = texel(tex, u, vv);
                if (t == 0) continue;
                px_semi = semi && (t & 0x8000);
                out = raw ? t : modulate(t, r, g, b, x, y, dither);
            } else {
                out = shade(r, g, b, x, y, dither);
            }
            plot(x, y, out, px_semi);
        }
    }
}

HwState hw_state() {
    HwState s;
    s.area_x1 = S.area_x1; s.area_y1 = S.area_y1; s.area_x2 = S.area_x2; s.area_y2 = S.area_y2;
    s.tw_mask_x = S.tw_mask_x; s.tw_mask_y = S.tw_mask_y; s.tw_off_x = S.tw_off_x; s.tw_off_y = S.tw_off_y;
    s.set_mask = S.set_mask; s.check_mask = S.check_mask;
    return s;
}

HwVtx hw_vtx(const Vtx& v) { return HwVtx{(float)v.x, (float)v.y, v.r, v.g, v.b, v.u, v.v}; }

// Hardware drops triangles whose bounding box is 1024x512 or larger.
bool oversized(const Vtx& a, const Vtx& b, const Vtx& c) {
    return std::max({a.x, b.x, c.x}) - std::min({a.x, b.x, c.x}) >= 1024 ||
           std::max({a.y, b.y, c.y}) - std::min({a.y, b.y, c.y}) >= 512;
}

// Scanline-accurate rasterization would need the exact hardware edge walker; the
// edge-function rasterizer above follows the same fill convention (top/left inclusive).
void draw_polygon(const uint32_t* w) {
    uint32_t cmd = w[0] >> 24;
    bool gouraud = cmd & 0x10, quad = cmd & 0x08, textured = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
    int nv = quad ? 4 : 3;
    Vtx v[4];
    TexCtx tex{};
    int idx = 0;
    uint32_t color = w[idx++] & 0xFFFFFF;
    for (int i = 0; i < nv; i++) {
        if (i > 0 && gouraud) color = w[idx++] & 0xFFFFFF;
        uint32_t xy = w[idx++];
        v[i].x = ((int32_t)(xy << 21) >> 21) + S.off_x;
        v[i].y = ((int32_t)((xy >> 16) << 21) >> 21) + S.off_y;
        v[i].r = color & 0xFF;
        v[i].g = (color >> 8) & 0xFF;
        v[i].b = (color >> 16) & 0xFF;
        if (textured) {
            uint32_t uv = w[idx++];
            v[i].u = uv & 0xFF;
            v[i].v = (uv >> 8) & 0xFF;
            if (i == 0) {
                tex.clut_x = ((uv >> 16) & 0x3F) * 16;
                tex.clut_y = (uv >> 22) & 0x1FF;
            } else if (i == 1) {
                set_tpage(uv >> 16);
                S.e1 = (S.e1 & ~0x9FFu) | ((uv >> 16) & 0x9FF);
            }
        }
    }
    tex.page_x = S.tpage_x;
    tex.page_y = S.tpage_y;
    tex.depth = S.tex_depth;
    if (textured && S.tex_disable) textured = false;
    int bx0 = v[0].x, bx1 = v[0].x;
    for (int i = 1; i < nv; i++) bx0 = std::min(bx0, v[i].x), bx1 = std::max(bx1, v[i].x);
    float cx = 0, k = squeeze_2d((float)bx0, (float)bx1, &cx);
    if (hw_active()) {
        HwPrim p;
        p.textured = textured; p.raw = raw; p.semi = semi; p.gouraud = gouraud;
        p.semi_mode = S.semi_mode;
        p.page_x = tex.page_x; p.page_y = tex.page_y; p.depth = tex.depth;
        p.clut_x = tex.clut_x; p.clut_y = tex.clut_y;
        HwState st = hw_state();
        HwVtx h[4];
        for (int i = 0; i < nv; i++) {
            h[i] = hw_vtx(v[i]);
            if (k != 1.0f) h[i].x = cx + (h[i].x - cx) * k;
        }
        if (!oversized(v[0], v[1], v[2])) hw_triangle(&h[0], &h[1], &h[2], p, st);
        if (quad && !oversized(v[1], v[2], v[3])) hw_triangle(&h[1], &h[2], &h[3], p, st);
        return;
    }
    if (k != 1.0f)
        for (int i = 0; i < nv; i++) v[i].x = (int)std::lround(cx + ((float)v[i].x - cx) * k);
    draw_triangle(&v[0], &v[1], &v[2], gouraud, textured, raw, semi, tex);
    if (quad) draw_triangle(&v[1], &v[2], &v[3], gouraud, textured, raw, semi, tex);
}

// ---------------------------------------------------------------- lines

void draw_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool gouraud, bool semi) {
    int dx = x1 - x0, dy = y1 - y0;
    if (std::abs(dx) >= 1024 || std::abs(dy) >= 512) return;
    float cx = 0, k = squeeze_2d((float)std::min(x0, x1), (float)std::max(x0, x1), &cx);
    if (k != 1.0f) {
        x0 = (int)std::lround(cx + ((float)x0 - cx) * k);
        x1 = (int)std::lround(cx + ((float)x1 - cx) * k);
        dx = x1 - x0;
    }
    if (hw_active()) {
        HwPrim p;
        p.semi = semi;
        p.semi_mode = S.semi_mode;
        hw_line(x0, y0, c0, x1, y1, c1, gouraud, p, hw_state());
        return;
    }
    int steps = std::max(std::abs(dx), std::abs(dy));
    int r0 = c0 & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = (c0 >> 16) & 0xFF;
    int r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
    for (int i = 0; i <= steps; i++) {
        int x = steps ? x0 + (dx * i + (dx >= 0 ? steps / 2 : -steps / 2)) / steps : x0;
        int y = steps ? y0 + (dy * i + (dy >= 0 ? steps / 2 : -steps / 2)) / steps : y0;
        int r = r0, g = g0, b = b0;
        if (gouraud && steps) {
            r = r0 + (r1 - r0) * i / steps;
            g = g0 + (g1 - g0) * i / steps;
            b = b0 + (b1 - b0) * i / steps;
        }
        plot(x, y, shade(r, g, b, x, y, S.dither && gouraud), semi);
    }
}

// ---------------------------------------------------------------- rectangles

void draw_rect(const uint32_t* w) {
    uint32_t cmd = w[0] >> 24;
    bool textured = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
    int size = (cmd >> 3) & 3;
    int idx = 0;
    uint32_t color = w[idx++] & 0xFFFFFF;
    uint32_t xy = w[idx++];
    int x = ((int32_t)(xy << 21) >> 21) + S.off_x;
    int y = ((int32_t)((xy >> 16) << 21) >> 21) + S.off_y;
    TexCtx tex{S.tpage_x, S.tpage_y, S.tex_depth, 0, 0};
    int u0 = 0, v0 = 0;
    if (textured) {
        uint32_t uv = w[idx++];
        u0 = uv & 0xFF;
        v0 = (uv >> 8) & 0xFF;
        tex.clut_x = ((uv >> 16) & 0x3F) * 16;
        tex.clut_y = (uv >> 22) & 0x1FF;
    }
    int wdt, hgt;
    switch (size) {
        case 0: { uint32_t wh = w[idx++]; wdt = wh & 0x3FF; hgt = (wh >> 16) & 0x1FF; break; }
        case 1: wdt = hgt = 1; break;
        case 2: wdt = hgt = 8; break;
        default: wdt = hgt = 16; break;
    }
    if (textured && S.tex_disable) textured = false;
    if (hw_active()) {
        HwPrim p;
        p.textured = textured; p.raw = raw; p.semi = semi;
        p.semi_mode = S.semi_mode;
        p.page_x = tex.page_x; p.page_y = tex.page_y; p.depth = tex.depth;
        p.clut_x = tex.clut_x; p.clut_y = tex.clut_y;
        float cx = 0, k = squeeze_2d((float)x, (float)(x + wdt), &cx);
        if (k != 1.0f && wdt > 0 && hgt > 0) {
            int r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
            float xa = cx + ((float)x - cx) * k, xb = cx + ((float)(x + wdt) - cx) * k;
            int ub = S.rect_flip_x ? u0 - wdt : u0 + wdt, vb = S.rect_flip_y ? v0 - hgt : v0 + hgt;
            HwVtx q[4] = {{xa, (float)y, r, g, b, u0, v0}, {xb, (float)y, r, g, b, ub, v0},
                          {xa, (float)(y + hgt), r, g, b, u0, vb}, {xb, (float)(y + hgt), r, g, b, ub, vb}};
            HwState st = hw_state();
            hw_triangle(&q[0], &q[1], &q[2], p, st);
            hw_triangle(&q[1], &q[2], &q[3], p, st);
            return;
        }
        hw_rect(x, y, wdt, hgt, u0, v0, S.rect_flip_x, S.rect_flip_y, color, p, hw_state());
        return;
    }
    int r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
    uint16_t flat = rgb24_to_15(r, g, b);
    for (int j = 0; j < hgt; j++) {
        int py = y + j;
        if (py < S.area_y1 || py > S.area_y2) continue;
        for (int i = 0; i < wdt; i++) {
            int px = x + i;
            if (px < S.area_x1 || px > S.area_x2) continue;
            if (textured) {
                int u = S.rect_flip_x ? u0 - i : u0 + i;
                int v = S.rect_flip_y ? v0 - j : v0 + j;
                uint16_t t = texel(tex, u, v);
                if (t == 0) continue;
                uint16_t out = raw ? t : modulate(t, r, g, b, px, py, false);
                plot(px, py, out, semi && (t & 0x8000));
            } else {
                plot(px, py, flat, semi);
            }
        }
    }
}

// ---------------------------------------------------------------- command lengths

size_t command_length(uint32_t first) {
    uint32_t cmd = first >> 24;
    if (cmd >= 0x20 && cmd < 0x40) {
        bool gouraud = cmd & 0x10, quad = cmd & 0x08, textured = cmd & 0x04;
        int nv = quad ? 4 : 3;
        return 1 + nv * (1 + (textured ? 1 : 0)) + (gouraud ? nv - 1 : 0);
    }
    if (cmd >= 0x40 && cmd < 0x60) {
        bool gouraud = cmd & 0x10, poly = cmd & 0x08;
        if (poly) return SIZE_MAX;  // terminated by 0x5xxx5xxx
        return gouraud ? 4 : 3;
    }
    if (cmd >= 0x60 && cmd < 0x80) {
        bool textured = cmd & 0x04;
        int size = (cmd >> 3) & 3;
        return 2 + (textured ? 1 : 0) + (size == 0 ? 1 : 0);
    }
    if (cmd >= 0x80 && cmd < 0xA0) return 4;
    if (cmd >= 0xA0 && cmd < 0xC0) return 3;
    if (cmd >= 0xC0 && cmd < 0xE0) return 3;
    if (cmd == 0x02) return 3;
    return 1;
}

void polyline(const std::vector<uint32_t>& w) {
    uint32_t cmd = w[0] >> 24;
    bool gouraud = cmd & 0x10, semi = cmd & 0x02;
    size_t idx = 0;
    uint32_t c0 = w[idx++] & 0xFFFFFF;
    uint32_t xy = w[idx++];
    int x0 = ((int32_t)(xy << 21) >> 21) + S.off_x, y0 = ((int32_t)((xy >> 16) << 21) >> 21) + S.off_y;
    while (idx < w.size()) {
        uint32_t c1 = c0;
        if (gouraud) {
            if ((w[idx] & 0xF000F000) == 0x50005000) break;
            c1 = w[idx++] & 0xFFFFFF;
        }
        if (idx >= w.size() || (w[idx] & 0xF000F000) == 0x50005000) break;
        xy = w[idx++];
        int x1 = ((int32_t)(xy << 21) >> 21) + S.off_x, y1 = ((int32_t)((xy >> 16) << 21) >> 21) + S.off_y;
        draw_line(x0, y0, c0, x1, y1, c1, gouraud, semi);
        x0 = x1; y0 = y1; c0 = c1;
    }
}

// per-frame primitive counters for --log gpu: polys, lines, rects, fills, uploads
uint32_t g_stat_counts[5];

void execute(const std::vector<uint32_t>& w) {
    uint32_t cmd = w[0] >> 24;
    if (cmd >= 0x20 && cmd < 0x40) g_stat_counts[0]++;
    else if (cmd >= 0x40 && cmd < 0x60) g_stat_counts[1]++;
    else if (cmd >= 0x60 && cmd < 0x80) g_stat_counts[2]++;
    else if (cmd == 0x02) g_stat_counts[3]++;
    else if (cmd >= 0xA0 && cmd < 0xC0) g_stat_counts[4]++;
    if (cmd >= 0x20 && cmd < 0x40) { draw_polygon(w.data()); return; }
    if (cmd >= 0x40 && cmd < 0x60) {
        if (cmd & 0x08) { polyline(w); return; }
        bool gouraud = cmd & 0x10;
        uint32_t c0 = w[0] & 0xFFFFFF, xy0 = w[1];
        uint32_t c1 = gouraud ? w[2] & 0xFFFFFF : c0, xy1 = gouraud ? w[3] : w[2];
        draw_line(((int32_t)(xy0 << 21) >> 21) + S.off_x, ((int32_t)((xy0 >> 16) << 21) >> 21) + S.off_y, c0,
                  ((int32_t)(xy1 << 21) >> 21) + S.off_x, ((int32_t)((xy1 >> 16) << 21) >> 21) + S.off_y, c1,
                  gouraud, cmd & 0x02);
        return;
    }
    if (cmd >= 0x60 && cmd < 0x80) { draw_rect(w.data()); return; }
    if (cmd >= 0x80 && cmd < 0xA0) {  // VRAM -> VRAM
        int sx = w[1] & 0x3FF, sy = (w[1] >> 16) & 0x1FF, dx = w[2] & 0x3FF, dy = (w[2] >> 16) & 0x1FF;
        int ww = ((w[3] - 1) & 0x3FF) + 1, hh = (((w[3] >> 16) - 1) & 0x1FF) + 1;
        std::vector<uint16_t> tmp((size_t)ww * hh);
        for (int j = 0; j < hh; j++)
            for (int i = 0; i < ww; i++) tmp[(size_t)j * ww + i] = vram(sx + i, sy + j);
        for (int j = 0; j < hh; j++)
            for (int i = 0; i < ww; i++) {
                uint16_t& d = vram(dx + i, dy + j);
                if (S.check_mask && (d & 0x8000)) continue;
                d = tmp[(size_t)j * ww + i] | (S.set_mask ? 0x8000 : 0);
            }
        if (hw_active()) hw_copy(sx, sy, dx, dy, ww, hh);
        replace_note_copy(sx, sy, dx, dy, ww, hh);
        return;
    }
    if (cmd >= 0xA0 && cmd < 0xC0) {  // CPU -> VRAM: data words follow
        g_upload.x = w[1] & 0x3FF;
        g_upload.y = (w[1] >> 16) & 0x1FF;
        g_upload.w = ((w[2] - 1) & 0x3FF) + 1;
        g_upload.h = (((w[2] >> 16) - 1) & 0x1FF) + 1;
        g_upload.cx = g_upload.cy = 0;
        g_upload.active = true;
        return;
    }
    if (cmd >= 0xC0 && cmd < 0xE0) {  // VRAM -> CPU
        g_download.x = w[1] & 0x3FF;
        g_download.y = (w[1] >> 16) & 0x1FF;
        g_download.w = ((w[2] - 1) & 0x3FF) + 1;
        g_download.h = (((w[2] >> 16) - 1) & 0x1FF) + 1;
        g_download.cx = g_download.cy = 0;
        g_download.active = true;
        if (hw_active()) hw_readback(g_download.x, g_download.y, g_download.w, g_download.h, g_vram);
        return;
    }
    switch (cmd) {
        case 0x00: case 0x01: return;
        case 0x02: {  // fill rectangle (no mask/area checks, x in 16-pixel units)
            uint32_t col = w[0];
            uint16_t c = rgb24_to_15(col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF);
            int x = w[1] & 0x3F0, y = (w[1] >> 16) & 0x1FF;
            int ww = ((w[2] & 0x3FF) + 15) & ~15, hh = (w[2] >> 16) & 0x1FF;
            for (int j = 0; j < hh; j++)
                for (int i = 0; i < ww; i++) vram(x + i, y + j) = c;
            if (hw_active()) hw_fill(x, y, ww, hh, c);
            return;
        }
        case 0x1F: DS.irq = true; return;
        case 0xE1: {
            uint32_t v = w[0];
            S.e1 = v & 0x3FFF;
            set_tpage(v);
            S.dither = (v >> 9) & 1;
            S.draw_to_display = (v >> 10) & 1;
            S.tex_disable = (v >> 11) & 1;
            S.rect_flip_x = (v >> 12) & 1;
            S.rect_flip_y = (v >> 13) & 1;
            return;
        }
        case 0xE2: {
            uint32_t v = w[0];
            S.tex_window = v & 0xFFFFF;
            S.tw_mask_x = v & 0x1F;
            S.tw_mask_y = (v >> 5) & 0x1F;
            S.tw_off_x = (v >> 10) & 0x1F;
            S.tw_off_y = (v >> 15) & 0x1F;
            return;
        }
        case 0xE3: S.area_x1 = w[0] & 0x3FF; S.area_y1 = (w[0] >> 10) & 0x1FF; return;
        case 0xE4: S.area_x2 = w[0] & 0x3FF; S.area_y2 = (w[0] >> 10) & 0x1FF; return;
        case 0xE5:
            S.off_x = (int32_t)(w[0] << 21) >> 21;
            S.off_y = (int32_t)((w[0] >> 11) << 21) >> 21;
            return;
        case 0xE6: S.set_mask = w[0] & 1; S.check_mask = (w[0] >> 1) & 1; return;
        default:
            LOG(Gpu, "unhandled GP0 %08X", w[0]);
            return;
    }
}

void upload_word(uint32_t v) {
    for (int k = 0; k < 2 && g_upload.active; k++) {
        uint16_t px = (uint16_t)(k ? v >> 16 : v);
        uint16_t& d = vram(g_upload.x + g_upload.cx, g_upload.y + g_upload.cy);
        if (!(S.check_mask && (d & 0x8000))) d = px | (S.set_mask ? 0x8000 : 0);
        if (++g_upload.cx >= g_upload.w) {
            g_upload.cx = 0;
            if (++g_upload.cy >= g_upload.h) {
                g_upload.active = false;
                if (hw_active()) hw_upload(g_upload.x, g_upload.y, g_upload.w, g_upload.h, g_vram);
                replace_note_upload(g_upload.x, g_upload.y, g_upload.w, g_upload.h);
            }
        }
    }
}

}  // namespace

// ================================================================ ports

void gpu_init() {
    std::memset(g_vram, 0, sizeof g_vram);
    S = DrawState{};
    DS = DisplayState{};
    g_cmd.clear();
    g_upload.active = g_download.active = false;
}

void gpu_write_gp0(R3000* c, uint32_t v) {
    (void)c;
    if (g_upload.active) {
        upload_word(v);
        return;
    }
    if (g_cmd.empty()) {
        g_cmd_need = command_length(v);
    }
    g_cmd.push_back(v);
    if (g_cmd_need == SIZE_MAX) {  // polyline: ends with terminator (after at least 2 vertices)
        if (g_cmd.size() >= 3 && (v & 0xF000F000) == 0x50005000) {
            execute(g_cmd);
            g_cmd.clear();
        }
        return;
    }
    if (g_cmd.size() >= g_cmd_need) {
        execute(g_cmd);
        g_cmd.clear();
    }
}

void gpu_write_gp1(R3000* c, uint32_t v) {
    (void)c;
    uint32_t cmd = (v >> 24) & 0x3F;
    switch (cmd) {
        case 0x00:
            S = DrawState{};
            DS = DisplayState{};
            g_cmd.clear();
            g_upload.active = g_download.active = false;
            break;
        case 0x01: g_cmd.clear(); g_upload.active = false; break;
        case 0x02: DS.irq = false; break;
        case 0x03: DS.enabled = !(v & 1); break;
        case 0x04: DS.dma_dir = v & 3; break;
        case 0x05:
            DS.start_x = v & 0x3FE;
            DS.start_y = (v >> 10) & 0x1FF;
            LOG(Gpu, "display start %d,%d", DS.start_x, DS.start_y);
            if (!core_in_ghost()) smooth_on_flip();
            break;
        case 0x06: DS.h1 = v & 0xFFF; DS.h2 = (v >> 12) & 0xFFF; break;
        case 0x07: DS.v1 = v & 0x3FF; DS.v2 = (v >> 10) & 0x3FF; break;
        case 0x08: DS.mode = v & 0xFF; break;
        case 0x10: {  // GPU info
            switch (v & 7) {
                case 2: g_gpuread = S.tex_window; break;
                case 3: g_gpuread = (uint32_t)(S.area_x1 | S.area_y1 << 10); break;
                case 4: g_gpuread = (uint32_t)(S.area_x2 | S.area_y2 << 10); break;
                case 5: g_gpuread = (uint32_t)((S.off_x & 0x7FF) | (S.off_y & 0x7FF) << 11); break;
                case 7: g_gpuread = 2; break;  // GPU version
                default: break;
            }
            break;
        }
        default: LOG(Gpu, "unhandled GP1 %08X", v); break;
    }
}

uint32_t gpu_read_data(R3000* c) {
    (void)c;
    if (!g_download.active) return g_gpuread;
    uint32_t out = 0;
    for (int k = 0; k < 2; k++) {
        uint16_t px = vram(g_download.x + g_download.cx, g_download.y + g_download.cy);
        out |= (uint32_t)px << (16 * k);
        if (++g_download.cx >= g_download.w) {
            g_download.cx = 0;
            if (++g_download.cy >= g_download.h) {
                g_download.active = false;
                break;
            }
        }
    }
    g_gpuread = out;
    return out;
}

extern uint32_t psx_current_scanline();

uint32_t gpu_read_stat(R3000* c) {
    (void)c;
    uint32_t s = S.e1 & 0x7FF;
    s |= (S.tex_disable ? 1u : 0u) << 15;
    s |= (uint32_t)S.set_mask << 11 | (uint32_t)S.check_mask << 12;
    uint32_t m = DS.mode;
    s |= ((m >> 6) & 1) << 16;         // hres2
    s |= (m & 3) << 17;                // hres1
    s |= ((m >> 2) & 1) << 19;         // vres
    s |= ((m >> 3) & 1) << 20;         // PAL
    s |= ((m >> 4) & 1) << 21;         // 24-bit
    s |= ((m >> 5) & 1) << 22;         // interlace
    s |= (DS.enabled ? 0u : 1u) << 23;
    s |= (DS.irq ? 1u : 0u) << 24;
    s |= 0x1C000000;                   // ready for cmd / VRAM->CPU / DMA block
    s |= (uint32_t)DS.dma_dir << 29;
    switch (DS.dma_dir) {              // DMA request bit
        case 1: s |= 1u << 25; break;
        case 2: s |= 1u << 25; break;
        case 3: s |= (g_download.active ? 1u : 0u) << 25; break;
    }
    // odd/even line: toggles every scanline outside vblank in non-interlaced mode
    uint32_t line = psx_current_scanline();
    bool odd = (m & 0x24) == 0x24 ? g_odd_line : (line & 1);
    if (line >= 240) odd = false;
    s |= (odd ? 1u : 0u) << 31;
    s |= (g_odd_line ? 1u : 0u) << 13;
    return s;
}

// 0 = software renderer, 1..8 = OpenGL renderer at that resolution scale. Needs the
// window's GL context to be current. Returns the scale actually in use.
int gpu_set_renderer(int scale, int samples) {
    if (hw_active()) {
        hw_readback(0, 0, 1024, 512, g_vram);  // keep what was drawn on the GPU
        if (scale != hw_scale() || samples != hw_samples_wanted()) hw_shutdown();
    }
    hw_set_samples(samples);
    if (scale > 0 && !hw_active()) {
        if (!hw_init(scale)) {
            log_msg(LogCat::Gpu, "OpenGL renderer unavailable, using the software renderer");
            return 0;
        }
        hw_upload_all(g_vram);
        replace_note_vram_reset();
    }
    return hw_active() ? hw_scale() : 0;
}

void gpu_vblank() {
    g_odd_line = !g_odd_line;
    g_vblanks++;
    replace_check(g_vram);
    LOG(Gpu, "frame: polys=%u lines=%u rects=%u fills=%u uploads=%u area=(%d,%d)-(%d,%d) offset=(%d,%d)",
        g_stat_counts[0], g_stat_counts[1], g_stat_counts[2], g_stat_counts[3], g_stat_counts[4], S.area_x1,
        S.area_y1, S.area_x2, S.area_y2, S.off_x, S.off_y);
    std::memset(g_stat_counts, 0, sizeof g_stat_counts);
    extern void gte_frame_stats();
    if (log_enabled(LogCat::Gpu)) gte_frame_stats();
}
bool gpu_is_interlaced_odd_field() { return g_odd_line; }

GpuDisplay gpu_display() {
    GpuDisplay d;
    d.vram = g_vram;
    d.x = DS.start_x;
    d.y = DS.start_y;
    static const int hres[] = {256, 320, 512, 640};
    d.w = (DS.mode & 0x40) ? 368 : hres[DS.mode & 3];
    bool interlace = (DS.mode & 0x24) == 0x24;
    int lines = DS.v2 - DS.v1;
    if (lines <= 0 || lines > 256) lines = 240;
    d.h = interlace ? lines * 2 : lines;
    d.rgb24 = DS.mode & 0x10;
    d.enabled = DS.enabled;
    return d;
}

// ================================================================ save states

// Entry hooks (tools/recomp/emit.py) on the level render's last routines, called from the
// render (80030A30..80030A48): 0 = the HUD, 1 and 2 = more 2D, 3 = the full-screen tint.
extern "C" void psx_hook_2d(R3000* c, int stage) {
    static const uint32_t kFrom[4] = {0x80030A38, 0x80030A40, 0x80030A48, 0x80030A50};
    if (c->r[31] != kFrom[stage]) return;
    const uint32_t next = MEM_R32(c, kNextPacket) & 0x1FFFFC;
    if (stage == 0) {
        g_2d_from = next;
        g_2d_mid = g_2d_to = 0x200000;
    } else if (stage == 1) {
        g_2d_mid = next;
    } else if (stage == 3) {
        g_2d_to = next;
    }
}

// Entry hooks on the text routine (8001AA28, a2 = the text) and the icon routines: run the
// routine and note the packets it made.
extern "C" int psx_hook_2d_call(R3000* c, int which) {
    static const uint32_t kFn[3] = {0x8001AA28, 0x8006D028, 0x8006D0D4};
    static bool inside[3];
    if (inside[which]) return 0;
    const uint32_t ra = c->r[31], sp = c->r[29];
    const uint32_t from = MEM_R32(c, kNextPacket) & 0x1FFFFC;
    inside[which] = true;
    call_indirect(c, kFn[which]);
    inside[which] = false;
    const uint32_t to = MEM_R32(c, kNextPacket) & 0x1FFFFC;
    if (to > from && g_text_packets.size() < 256) g_text_packets.push_back({from, to});
    c->r[31] = ra;
    c->r[29] = sp;
    return 1;
}

// A frame's ordering table was drawn: the packets noted so far are done with (each frame's
// render notes its own; after a level, its HUD range must not apply to menu packets).
void gpu_list_done() {
    g_text_packets.clear();
    g_2d_from = g_2d_mid = g_2d_to = 0;
}

void gpu_note_movie_frame() {
    g_movie_vblank = g_vblanks;
    g_movie_seen = true;
}
bool gpu_movie_playing() { return g_movie_seen && g_vblanks - g_movie_vblank < 30; }

#include "state_io.h"

// No command half-sent and no VRAM transfer running (safe to feed GP0 from elsewhere).
bool gpu_idle() { return g_cmd.empty() && !g_upload.active && !g_download.active; }

// Everything but VRAM (smooth.cpp rolls this back after drawing an extra frame).
void gpu_regs_state(StateIO& s) {
    s(S);
    s(DS);
    s(g_odd_line);
    s.vec(g_cmd);
    s(g_cmd_need);
    s(g_upload);
    s(g_download);
    s(g_gpuread);
}

void gpu_state(StateIO& s) {
    s.marker(0x47505520);  // "GPU "
    if (!s.loading() && hw_active()) hw_readback(0, 0, 1024, 512, g_vram);  // GPU-side VRAM is authoritative
    s.raw(g_vram, sizeof g_vram);
    gpu_regs_state(s);
    if (s.loading() && hw_active()) hw_upload_all(g_vram);
    if (s.loading()) replace_note_vram_reset();
}

// The CPU-side VRAM copy of a rectangle (fills and uploads land there even while the
// OpenGL renderer draws the polygons).
void gpu_vram_save(int x, int y, int w, int h, std::vector<uint16_t>& out) {
    out.resize((size_t)w * h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) out[(size_t)j * w + i] = g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
}

void gpu_vram_restore(int x, int y, int w, int h, const std::vector<uint16_t>& in) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = in[(size_t)j * w + i];
}
