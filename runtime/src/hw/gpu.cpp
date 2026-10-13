// PS1 GPU: command processing and a software rasterizer into 1024x512x16 VRAM.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/psx.h"
#include "core/state_io.h"
#include "game/game.h"
#include "host/host.h"
#include "render/gpu_hw.h"

bool g_smooth_font = true;  // psx.h

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
// An extension of ours (game_menu.cpp, the menu flames): GP0 E6 bit 2 draws only on pixels whose mask bit
// is set (the reverse of bit 1). Not in DrawState, so saved states keep their layout; the sequence that
// sets it clears it again.
bool g_mask_inside = false;

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
uint32_t g_fx_from = 0;  // packets: the effect renderer's (8003736C) from here to the HUD
                                                    // up to the tint's (phys)
uint32_t g_2d_bar = 0;                              // the HUD's boss bar from here (up to mid)
// Packets the text and icon routines made since the last frame was drawn (phys from, to).
std::vector<std::pair<uint32_t, uint32_t>> g_text_packets;
void draw_polygon(const uint32_t* w);
// HwPrim::box for texels u0..u1, v0..v1 at the vertices (the far ones are not drawn).
uint32_t smooth_box(int u0, int v0, int u1, int v1) {
    u1 = std::max(u0, u1 - 1), v1 = std::max(v0, v1 - 1);
    return (uint32_t)(u0 & 255) | (uint32_t)(v0 & 255) << 8 | (uint32_t)(u1 & 255) << 16 | (uint32_t)(v1 & 255) << 24;
}

// Text drawn tilted (subtitles, game_menu.cpp): the packets of one text routine call, turned by
// `angle` (radians, clockwise on screen) around its anchor (the routine's x, y).
struct Tilt {
    uint32_t from, to;
    float x, y, angle;
};
std::vector<Tilt> g_tilts;
uint32_t g_last_text_from = 0;  // packet the last text / icon routine call started at

const Tilt* tilt_of(uint32_t a) {
    for (const auto& t : g_tilts)
        if (a >= t.from && a < t.to) return &t;
    return nullptr;
}

bool text_packet(uint32_t a) {
    for (const auto& r : g_text_packets)
        if (a >= r.first && a < r.second) return true;
    return false;
}
// A picture was uploaded (movie frame strips, full screens) this host frame: frames then are not
// shown before the game flips to them. The first movie picture went into the other buffer while
// the display was still 15-bit, and shown early it read as green stripes.
uint64_t g_picture_frame = ~0ull;

uint64_t g_movie_vblank = 0;  // when the MDEC last decoded a movie frame (VBlank count)
bool g_movie_seen = false;

// The effect renderer's packets (smoke, fire, flashes, glows, sparks, shards): their corners are
// made on the screen from one projected point and a size in pixels, so in widescreen (the 3D
// squeezed by g_gte_widescreen) they were stretched across it, 2.7x at 32:9.
bool fx_packet(uint32_t a) { return g_fx_from && a >= g_fx_from && a < (g_2d_from ? g_2d_from : 0x200000); }

// Horizontal factor for a primitive spanning x0..x1 and the point that stays put (x' =
// fix + (x - fix) * factor); 1 = leave it.
float squeeze_2d(float x0, float x1, float* fix) {
    const uint32_t a = g_gpu_packet_addr;
    if (g_gte_widescreen == 1.0f) return 1.0f;
    if (!a) return 1.0f;
    if (replace_original_picture_shown(S.area_y1)) return 1.0f;  // shown at 4:3 (host.cpp present_options)
    if (fx_packet(a)) {  // a sprite (smoke, fire, a flash): its own shape round its middle
        *fix = (x0 + x1) * 0.5f;
        return g_gte_widescreen;
    }
    const bool text = text_packet(a);
    if (!text && (a < g_2d_from || a >= g_2d_to)) return 1.0f;
    const float left = (float)S.area_x1, right = (float)(S.area_x2 + 1), width = right - left;
    if (x1 - x0 >= 0.9f * width) return 1.0f;  // full-screen shapes
    const float mid = (x0 + x1) * 0.5f;
    // HUD text and icons too; the boss bar (from g_2d_bar) spans the middle: one piece, centred
    const bool hud = a >= g_2d_from && a < g_2d_mid && a < g_2d_bar;
    // Ultra-wide: the screen edge goes to the edge of a centred 16:9 area, so the HUD stays where
    // it is in 16:9 (the point that stays put is then where fix + (edge - fix) * k = that edge).
    const float k = g_gte_widescreen, centre = (left + right) * 0.5f;
    const float edge = width * 0.5f * std::min(1.0f, k / 0.75f);
    if (hud && mid < left + width / 3) *fix = (centre - edge - left * k) / (1 - k);          // HUD, left side
    else if (hud && mid > right - width / 3) *fix = (centre + edge - right * k) / (1 - k);  // HUD, right side
    else *fix = (left + right) * 0.5f;
    return g_gte_widescreen;
}

// command FIFO accumulation
std::vector<uint32_t> g_cmd;
uint32_t g_cmd_addr[16];  // RAM address of each word of g_cmd (0: not from a list)
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

inline void plot(int x, int y, uint16_t color, bool semi) {
    if (x < S.area_x1 || x > S.area_x2 || y < S.area_y1 || y > S.area_y2) return;
    uint16_t& dst = vram(x, y);
    if (S.check_mask && (dst & 0x8000)) return;
    if (g_mask_inside && !(dst & 0x8000)) return;
    if (semi) color = blend(dst, color, S.semi_mode);
    dst = color | (S.set_mask ? 0x8000 : 0);
}

inline uint16_t rgb24_to_15(int r, int g, int b) {
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// No dithering (the game's dither bit, S.dither, is only reported back in GPUSTAT): shading is
// plain truncation to 15 bits here; the hardware renderers keep full 24-bit colour.
inline uint16_t shade(int r, int g, int b) { return rgb24_to_15(r, g, b); }

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
inline uint16_t modulate(uint16_t tx, int r, int g, int b) {
    int tr = (tx & 31) << 3, tg = ((tx >> 5) & 31) << 3, tb = ((tx >> 10) & 31) << 3;
    int orr = (tr * r) >> 7, og = (tg * g) >> 7, ob = (tb * b) >> 7;
    uint16_t c = shade(std::min(orr, 255), std::min(og, 255), std::min(ob, 255));
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
                out = raw ? t : modulate(t, r, g, b);
            } else {
                out = shade(r, g, b);
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
    s.mask_inside = g_mask_inside;
    return s;
}

HwVtx hw_vtx(const Vtx& v) { return HwVtx{(float)v.x, (float)v.y, v.r, v.g, v.b, (float)v.u, (float)v.v}; }

// Hardware drops triangles whose bounding box is 1024x512 or larger.
bool oversized(const Vtx& a, const Vtx& b, const Vtx& c) {
    return std::max({a.x, b.x, c.x}) - std::min({a.x, b.x, c.x}) >= 1024 ||
           std::max({a.y, b.y, c.y}) - std::min({a.y, b.y, c.y}) >= 512;
}

// Precise 3D: the exact position and depth of each corner of a polygon (gte.cpp), found by the
// whole-pixel vertex word the GPU received; ok[i] false where none was found.
// - Several vertices can land on one pixel: each corner takes the candidate projected nearest
//   in time to the others (a polygon's corners come from one model's batch of projections).
// - The game nudges some vertices by a pixel after projecting them (level 1's subdivided floors
//   and walls: every other grid vertex). A corner with no exact match takes a candidate from
//   the pixels around it, if that was projected in the same batch, keeping the nudge.
void precise_corners(const uint32_t* xyw, int nv, GtePrecise* pr, bool* ok) {
    constexpr int kMax = 8;
    constexpr uint32_t kBatch = 64;  // projections apart that still count as the same batch
    struct Cand {
        GtePrecise p;
        int dx, dy;
    };
    Cand cand[4][kMax];
    int nc[4];
    bool any_exact = false;
    for (int i = 0; i < nv; i++) {
        GtePrecise found[kMax];
        nc[i] = gte_precise_find(xyw[i], found, kMax);
        for (int m = 0; m < nc[i]; m++) cand[i][m] = {found[m], 0, 0};
        if (nc[i]) {
            any_exact = true;
            continue;
        }
        const int x = (int32_t)(xyw[i] << 21) >> 21, y = (int32_t)((xyw[i] >> 16) << 21) >> 21;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                const uint32_t k = ((uint32_t)(x + dx) & 0x7FF) | ((uint32_t)(y + dy) & 0x7FF) << 16;
                const int n = gte_precise_find(k, found, kMax - nc[i]);
                for (int m = 0; m < n; m++) cand[i][nc[i]++] = {found[m], dx, dy};
            }
    }
    uint64_t best_cost = UINT64_MAX;
    uint32_t best_anchor = 0;
    for (int a = 0; a < nv; a++)
        for (int j = 0; j < nc[a]; j++) {
            const Cand& an = cand[a][j];
            if (any_exact && (an.dx || an.dy)) continue;  // anchored by an exact corner
            uint64_t cost = 0;
            for (int i = 0; i < nv; i++) {
                uint32_t d_best = UINT32_MAX;
                for (int m = 0; m < nc[i]; m++) {
                    const uint32_t s = cand[i][m].p.seq;
                    d_best = std::min(d_best, s > an.p.seq ? s - an.p.seq : an.p.seq - s);
                }
                if (nc[i]) cost += d_best;
            }
            if (cost < best_cost) best_cost = cost, best_anchor = an.p.seq;
        }
    for (int i = 0; i < nv; i++) {
        ok[i] = false;
        uint32_t d_best = UINT32_MAX;
        for (int m = 0; m < nc[i]; m++) {
            const Cand& cd = cand[i][m];
            const uint32_t d = cd.p.seq > best_anchor ? cd.p.seq - best_anchor : best_anchor - cd.p.seq;
            if (d >= d_best || ((cd.dx || cd.dy) && d > kBatch)) continue;
            d_best = d;
            pr[i] = cd.p;
            pr[i].x -= (float)cd.dx;  // the game's nudge
            pr[i].y -= (float)cd.dy;
            ok[i] = true;
        }
    }
}

// Neighbouring polygons share corners, but the lookups above can answer differently for each
// of them (a light's fan: a ring vertex is found for the outer quad but not for the inner triangle,
// or two candidates a fraction of a pixel apart are picked), and the shared edge then splits into
// a thin gap or overlap line. Untextured polygons (the lights, flat shading) take the answer the
// first polygon this frame got for the same vertex word, found or not.
struct SharedCorner {
    GtePrecise p;
    bool ok;
};
std::unordered_map<uint64_t, SharedCorner> g_shared_corners;  // cleared by gpu_list_done
// a corner this frame: its vertex word and the draw offset it is drawn at
uint64_t corner_key(uint32_t xy) {
    return (uint64_t)xy << 32 | (uint32_t)(uint16_t)S.off_x << 16 | (uint16_t)S.off_y;
}
void shared_corners(const uint32_t* xyw, int nv, GtePrecise* pr, bool* ok) {
    for (int i = 0; i < nv; i++) {
        auto [it, fresh] = g_shared_corners.emplace(corner_key(xyw[i]), SharedCorner{pr[i], ok[i]});
        if (!fresh) pr[i] = it->second.p, ok[i] = it->second.ok;
    }
}

// Effects in widescreen (fx_packet): the corners made on the screen are narrowed round a point
// that stays put. A light is a fan - triangles from its projected middle out to a ring, quads from
// the ring outwards - whose polygons share corners: narrowed each round its own middle, the shared
// corners split apart and the fan showed dark gaps. A corner placed once this frame keeps its place,
// and a polygon with one of those corners is narrowed round the same point as the polygon before
// (found from that corner); otherwise round its exact corners (a fan's middle). A fan's quad comes
// before its triangle: its sides lie on the light's rays, and where they meet is the middle. Else
// (a sprite: parallel sides) round its own middle.
std::unordered_map<uint64_t, float> g_fx_corners;  // vertex word + draw offset -> its x; cleared by gpu_list_done
void fx_squeeze(HwVtx* h, const uint32_t* xyw, const bool* exact, int nv) {
    const float k = g_gte_widescreen;
    float fix = 0;
    bool have = false;
    for (int i = 0; i < nv && !have; i++) {
        if (exact[i]) continue;
        const auto it = g_fx_corners.find(corner_key(xyw[i]));
        if (it != g_fx_corners.end()) fix = (it->second - k * h[i].x) / (1 - k), have = true;
    }
    if (!have) {
        float sx = 0, ax = 0;
        int n = 0;
        for (int i = 0; i < nv; i++) sx += h[i].x, ax += exact[i] ? h[i].x : 0, n += exact[i];
        fix = n ? ax / (float)n : sx / (float)nv;
        if (!n && nv == 4) {  // sides 0 -> 2 and 1 -> 3 (outer corners, then inner ones)
            const float dx0 = h[2].x - h[0].x, dy0 = h[2].y - h[0].y, dx1 = h[3].x - h[1].x, dy1 = h[3].y - h[1].y;
            const float det = dx0 * dy1 - dy0 * dx1;
            if (std::fabs(det) > 1e-3f) {
                const float t = ((h[1].x - h[0].x) * dy1 - (h[1].y - h[0].y) * dx1) / det;
                if (t > 1.0f && t < 16.0f) fix = h[0].x + dx0 * t;  // past the inner corners, not far
            }
        }
    }
    for (int i = 0; i < nv; i++)
        if (!exact[i]) h[i].x = g_fx_corners.emplace(corner_key(xyw[i]), fix + (h[i].x - fix) * k).first->second;
}

// Scanline-accurate rasterization would need the exact hardware edge walker; the
// edge-function rasterizer above follows the same fill convention (top/left inclusive).
void draw_polygon(const uint32_t* w) {
    replace_note_poly(S.area_y1);
    uint32_t cmd = w[0] >> 24;
    bool gouraud = cmd & 0x10, quad = cmd & 0x08, textured = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
    int nv = quad ? 4 : 3;
    Vtx v[4];
    uint32_t xyw[4], xya[4], uvw[4] = {}, uva[4] = {};
    TexCtx tex{};
    int idx = 0;
    uint32_t color = w[idx++] & 0xFFFFFF;
    for (int i = 0; i < nv; i++) {
        if (i > 0 && gouraud) color = w[idx++] & 0xFFFFFF;
        xya[i] = w == g_cmd.data() ? g_cmd_addr[idx] : 0;
        uint32_t xy = xyw[i] = w[idx++];
        v[i].x = ((int32_t)(xy << 21) >> 21) + S.off_x;
        v[i].y = ((int32_t)((xy >> 16) << 21) >> 21) + S.off_y;
        v[i].r = color & 0xFF;
        v[i].g = (color >> 8) & 0xFF;
        v[i].b = (color >> 16) & 0xFF;
        if (textured) {
            uva[i] = w == g_cmd.data() ? g_cmd_addr[idx] : 0;
            uint32_t uv = uvw[i] = w[idx++];
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
    const bool fx = g_gte_widescreen < 1.0f && g_gpu_packet_addr && fx_packet(g_gpu_packet_addr);
    float cx = 0, k = fx ? 1.0f : squeeze_2d((float)bx0, (float)bx1, &cx);
    if (hw_active()) {
        HwPrim p;
        p.textured = textured; p.raw = raw; p.semi = semi; p.gouraud = gouraud;
        // the game's text (2D, also when tilted on the screen: subtitles, "Auto Saved"): never
        // precise 3D - its corners would be taken for 3D points projected to the same pixels
        const bool text = g_gpu_packet_addr && text_packet(g_gpu_packet_addr);
        p.smooth = textured && g_smooth_font && text;
        if (textured) {  // the texels it may use (the shader clamps to them)
            int u0 = v[0].u, u1 = v[0].u, v0 = v[0].v, v1 = v[0].v;
            for (int i = 1; i < nv; i++)
                u0 = std::min(u0, v[i].u), u1 = std::max(u1, v[i].u), v0 = std::min(v0, v[i].v), v1 = std::max(v1, v[i].v);
            // a glyph ends before its last coordinate (its neighbours in the font are close);
            // a polygon may use the texel at its largest coordinate
            p.box = p.smooth ? smooth_box(u0, v0, u1, v1) : smooth_box(u0, v0, u1 + 1, v1 + 1);
        }
        p.semi_mode = S.semi_mode;
        p.page_x = tex.page_x; p.page_y = tex.page_y; p.depth = tex.depth;
        p.clut_x = tex.clut_x; p.clut_y = tex.clut_y;
        if (textured && g_tex_dump) texdump_note(tex.page_x, tex.page_y, tex.depth, tex.clut_x, tex.clut_y, g_vram);
        HwState st = hw_state();
        HwVtx h[4];
        for (int i = 0; i < nv; i++) {
            h[i] = hw_vtx(v[i]);
            if (k != 1.0f) h[i].x = cx + (h[i].x - cx) * k;
        }
        const Tilt* tilt = g_gpu_packet_addr && !g_tilts.empty() ? tilt_of(g_gpu_packet_addr) : nullptr;
        if (const Tilt* t = tilt) {
            // turned on the screen, where a VRAM pixel is 0.625 lines wide (320 x 240 shown as 4:3),
            // wider on a wider screen (the text above is narrowed by g_gte_widescreen to keep its shape)
            const float kPixel = 320.0f / 512.0f / g_gte_widescreen;
            float px = t->x + (float)S.off_x, py = t->y + (float)S.off_y;
            if (k != 1.0f) px = cx + (px - cx) * k;
            const float cs = std::cos(t->angle), sn = std::sin(t->angle);
            for (int i = 0; i < nv; i++) {
                const float dx = (h[i].x - px) * kPixel, dy = h[i].y - py;
                h[i].x = px + (dx * cs - dy * sn) / kPixel;
                h[i].y = py + dx * sn + dy * cs;
            }
        }
        // Precise 3D (gte.cpp): each vertex the GTE projected goes to its exact position (all of
        // them, so neighbouring polygons still share their edges); when every vertex was, the
        // texture is perspective-correct too (w relative to the farthest vertex).
        float zv[4] = {};  // exact view depth of each vertex (0: none)
        bool exact_all = false;  // every vertex exact: drawn whatever its size (the PS1's limit is for whole pixels)
        // exact texture coordinates (gte.cpp): the subdivision's whole-texel UVs get their fraction back
        if (textured)
            for (int i = 0; i < nv; i++) pgxp_uv_find(uva[i], uvw[i], &h[i].u, &h[i].v);
        if (k == 1.0f && !text && !tilt) {
            // the vertex the word itself came from (PGXP), else one projected to that pixel
            GtePrecise pr[4];
            bool nc[4], shadow_all = true;
            for (int i = 0; i < nv; i++) shadow_all &= nc[i] = pgxp_find(xya[i], xyw[i], &pr[i]);
            // (an effect's corners made on the screen may land on some 3D point's pixel: only the
            // corners the GTE made are exact, the rest come in with fx_squeeze)
            if (!shadow_all && !fx) {
                GtePrecise pc[4];
                bool cc[4];
                precise_corners(xyw, nv, pc, cc);
                for (int i = 0; i < nv; i++)
                    if (!nc[i]) pr[i] = pc[i], nc[i] = cc[i];
            }
            if (!textured && !fx) shared_corners(xyw, nv, pr, nc);
            bool all = true;
            float wmax = 0;
            for (int i = 0; i < nv; i++) {
                if (!nc[i]) {
                    all = false;
                    continue;
                }
                zv[i] = pr[i].w;
                h[i].x = pr[i].x + (float)S.off_x;
                h[i].y = pr[i].y + (float)S.off_y;
                wmax = std::max(wmax, pr[i].w);
            }
            if (all)
                for (int i = 0; i < nv; i++) h[i].w = pr[i].w / wmax;
            exact_all = all;
            // frame interpolation (motion.cpp): where each exact corner was last frame
            if (g_motion_on)
                for (int i = 0; i < nv; i++) {
                    MotionPrev m;
                    if (!nc[i] || !motion_prev(pr[i].x, pr[i].y, pr[i].w, &m)) continue;
                    h[i].dx = m.x - pr[i].x, h[i].dy = m.y - pr[i].y;
                    if (all) h[i].dr = m.w / pr[i].w;
                }
            // an effect: the corners made on the screen come in (fx_squeeze)
            if (fx && !all) fx_squeeze(h, xyw, nc, nv);
        } else if (fx) {  // (no exact corners known)
            const bool none[4] = {};
            fx_squeeze(h, xyw, none, nv);
        }
        // a replacement texture (texreplace.cpp): drawn from its HD copy of VRAM
        if (textured && g_tex_replace) {
            float u0 = h[0].u, u1 = h[0].u, v0 = h[0].v, v1 = h[0].v;
            for (int i = 1; i < nv; i++)
                u0 = std::min(u0, h[i].u), u1 = std::max(u1, h[i].u), v0 = std::min(v0, h[i].v), v1 = std::max(v1, h[i].v);
            const int tw[4] = {S.tw_mask_x, S.tw_mask_y, S.tw_off_x, S.tw_off_y};
            const int found = texreplace_find(tex.page_x, tex.page_y, tex.depth, tex.clut_x, tex.clut_y, u0, v0, u1, v1, tw, g_vram);
            p.hd = found != 0, p.glyph = found == 2;
        }
        // Z-buffer: 3D polygons test (and opaque ones write) their exact view depth, so the
        // ordering table's one depth per polygon no longer lets near polygons show through far
        // ones where they cross. Only polygons with every vertex exact take part; the rest,
        // glows, 2D and the table's last slot (everything the game clamped there: the sky, far
        // skylines - the sky is nearer than the skyline it must stay behind) draw in list order
        // as before. Semi-transparent polygons (decals, shadows on the floor) test a little
        // nearer than they are, not to fight the surface they lie on.
        // Effects (glows, sprites, beams: exact with Native 3D) stay in list order: a glow or a
        // puff of smoke cut off by the wall behind it was never the game's look.
        if (k == 1.0f && !(g_gpu_packet_addr && fx_packet(g_gpu_packet_addr)) && g_gpu_ot_z && g_gpu_ot_z < 0x3FF0 && hw_zbuffer_ready()) {
            bool exact = true;
            for (int i = 0; i < nv; i++) exact &= zv[i] > 0;
            if (exact) {
                p.ztest = semi ? 2 : 1;
                for (int i = 0; i < nv; i++) h[i].z = semi ? zv[i] * 0.99f : zv[i];
            }
        }
        if (exact_all || !oversized(v[0], v[1], v[2])) hw_triangle(&h[0], &h[1], &h[2], p, st);
        if (quad && (exact_all || !oversized(v[1], v[2], v[3]))) hw_triangle(&h[1], &h[2], &h[3], p, st);
        return;
    }
    if (k != 1.0f)
        for (int i = 0; i < nv; i++) v[i].x = (int)std::lround(cx + ((float)v[i].x - cx) * k);
    draw_triangle(&v[0], &v[1], &v[2], gouraud, textured, raw, semi, tex);
    if (quad) draw_triangle(&v[1], &v[2], &v[3], gouraud, textured, raw, semi, tex);
}

// ---------------------------------------------------------------- lines

// exact: the ends' exact positions (Native 3D's effect lines), else whole pixels
void draw_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool gouraud, bool semi, const float* exact = nullptr) {
    int dx = x1 - x0, dy = y1 - y0;
    if (std::abs(dx) >= 1024 || std::abs(dy) >= 512) return;
    float cx = 0, k = exact ? 1.0f : squeeze_2d((float)std::min(x0, x1), (float)std::max(x0, x1), &cx);
    if (exact && hw_active()) {
        HwPrim p;
        p.semi = semi;
        p.semi_mode = S.semi_mode;
        hw_line(exact[0], exact[1], c0, exact[2], exact[3], c1, gouraud, p, hw_state());
        return;
    }
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
        plot(x, y, shade(r, g, b), semi);
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
        p.smooth = textured && g_smooth_font && g_gpu_packet_addr && text_packet(g_gpu_packet_addr);
        if (textured) {  // the texels it may use: [lo, hi) each way (a flipped sprite runs back from
                         // u0); one that wraps past 255 keeps the whole page
            const int ulo = S.rect_flip_x ? u0 - wdt + 1 : u0, vlo = S.rect_flip_y ? v0 - hgt + 1 : v0;
            const int uhi = ulo + wdt, vhi = vlo + hgt;
            const bool wraps = ulo < 0 || vlo < 0 || uhi > 256 || vhi > 256;
            p.box = wraps ? smooth_box(0, 0, 256, 256) : smooth_box(ulo, vlo, uhi, vhi);
        }
        p.semi_mode = S.semi_mode;
        p.page_x = tex.page_x; p.page_y = tex.page_y; p.depth = tex.depth;
        p.clut_x = tex.clut_x; p.clut_y = tex.clut_y;
        if (textured && g_tex_dump) texdump_note(tex.page_x, tex.page_y, tex.depth, tex.clut_x, tex.clut_y, g_vram);
        float cx = 0, k = squeeze_2d((float)x, (float)(x + wdt), &cx);
        // a replacement texture (texreplace.cpp): drawn from its HD copy of VRAM
        if (textured && g_tex_replace && wdt > 0 && hgt > 0) {
            const int ua = S.rect_flip_x ? u0 - wdt + 1 : u0, va = S.rect_flip_y ? v0 - hgt + 1 : v0;
            const int tw[4] = {S.tw_mask_x, S.tw_mask_y, S.tw_off_x, S.tw_off_y};
            const int found = texreplace_find(tex.page_x, tex.page_y, tex.depth, tex.clut_x, tex.clut_y, (float)ua, (float)va,
                                   (float)(ua + wdt), (float)(va + hgt), tw, g_vram);
            p.hd = found != 0, p.glyph = found == 2;
        }
        if (k != 1.0f && wdt > 0 && hgt > 0) {
            int r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
            float xa = cx + ((float)x - cx) * k, xb = cx + ((float)(x + wdt) - cx) * k;
            int ub = S.rect_flip_x ? u0 - wdt : u0 + wdt, vb = S.rect_flip_y ? v0 - hgt : v0 + hgt;
            const float fu0 = (float)u0, fv0 = (float)v0, fub = (float)ub, fvb = (float)vb;
            HwVtx q[4] = {{xa, (float)y, r, g, b, fu0, fv0}, {xb, (float)y, r, g, b, fub, fv0},
                          {xa, (float)(y + hgt), r, g, b, fu0, fvb}, {xb, (float)(y + hgt), r, g, b, fub, fvb}};
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
                uint16_t out = raw ? t : modulate(t, r, g, b);
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
        GtePrecise a, b;
        float exact[4];
        const bool ex = w.data() == g_cmd.data() && pgxp_find(g_cmd_addr[1], xy0, &a) &&
                        pgxp_find(g_cmd_addr[gouraud ? 3 : 2], xy1, &b);
        if (ex) exact[0] = a.x + S.off_x, exact[1] = a.y + S.off_y, exact[2] = b.x + S.off_x, exact[3] = b.y + S.off_y;
        draw_line(((int32_t)(xy0 << 21) >> 21) + S.off_x, ((int32_t)((xy0 >> 16) << 21) >> 21) + S.off_y, c0,
                  ((int32_t)(xy1 << 21) >> 21) + S.off_x, ((int32_t)((xy1 >> 16) << 21) >> 21) + S.off_y, c1,
                  gouraud, cmd & 0x02, ex ? exact : nullptr);
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
        if (hw_active()) hw_copy(sx, sy, dx, dy, ww, hh), hw_rec_note_write(dx, dy, ww, hh);
        replace_note_copy(sx, sy, dx, dy, ww, hh);
        texdump_vram_changed(dx, dy, ww, hh);
        if (dx + ww > 512) texreplace_vram_changed(dx, dy, ww, hh);
        texreplace_vram_written(dx, dy, ww, hh);
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
            if (hw_active()) hw_fill(x, y, ww, hh, c), hw_rec_note_fill(x, y, ww, hh, c);
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
        case 0xE6: S.set_mask = w[0] & 1; S.check_mask = (w[0] >> 1) & 1; g_mask_inside = (w[0] >> 2) & 1; return;
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
                LOG(Gpu, "upload %d,%d %dx%d", g_upload.x, g_upload.y, g_upload.w, g_upload.h);
                if (hw_active()) {
                    hw_upload(g_upload.x, g_upload.y, g_upload.w, g_upload.h, g_vram);
                    hw_rec_note_write(g_upload.x, g_upload.y, g_upload.w, g_upload.h);
                }
                texdump_vram_changed(g_upload.x, g_upload.y, g_upload.w, g_upload.h);
                if (g_upload.x + g_upload.w > 512 && g_upload.h > 1) texreplace_vram_changed(g_upload.x, g_upload.y, g_upload.w, g_upload.h);
                texreplace_vram_written(g_upload.x, g_upload.y, g_upload.w, g_upload.h);
                if (g_upload.h >= 200) g_picture_frame = host_frame_count(), core_early_cancel();  // (gpu_drawn_display)
                replace_note_upload(g_upload.x, g_upload.y, g_upload.w, g_upload.h);
                // a whole picture: replace it now, before the game draws on it in the same
                // frame (the title's PRESS START); pictures sent in strips wait for the VBlank
                if (g_upload.w >= 512 && g_upload.h >= 240) replace_check(g_vram);
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
    if (g_cmd.size() < 16) g_cmd_addr[g_cmd.size()] = g_gpu_word_addr;
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
            if (!core_in_ghost()) g_display_flipped = true, g_display_flips++;
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
    // interlace field: the opposite of the field bit 31 shows; always 1 when not interlaced
    s |= ((m & 0x20) ? (g_odd_line ? 0u : 1u) : 1u) << 13;
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
    if (log_enabled(LogCat::Gpu)) gte_frame_stats();
}

bool g_display_flipped = false;
uint64_t g_display_flips = 0;  // display starts the game has set (new frames shown)

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

// The frame just drawn, as the screen will show it once the game flips to it: the display at
// the drawing area's corner. False when the drawing area is not a whole display-sized buffer.
bool gpu_drawn_display(GpuDisplay* out) {
    GpuDisplay d = gpu_display();
    if (d.rgb24 || !d.enabled || S.area_x2 - S.area_x1 + 1 != d.w || S.area_y2 - S.area_y1 + 1 < d.h) return false;
    if (g_picture_frame != ~0ull && host_frame_count() - g_picture_frame <= 1) return false;
    d.x = S.area_x1;
    d.y = S.area_y1;
    *out = d;
    return true;
}

// ================================================================ save states

// Entry hooks (tools/recomp/emit.py) on the level render's last routines, called from the
// render (80030A30..80030A48): 0 = the HUD, 1 and 2 = more 2D, 3 = the full-screen tint;
// 4 = the boss health bar, the HUD's last routine (8006D3C0, called from 8006DF78); 5 = the
// effects (8003736C, called from 80030A20).
extern "C" void psx_hook_2d(R3000* c, int stage) {
    static const uint32_t kFrom[6] = {0x80030A38, 0x80030A40, 0x80030A48, 0x80030A50, 0x8006DF80, 0x80030A28};
    if (c->r[31] != kFrom[stage]) return;
    const uint32_t next = MEM_R32(c, kNextPacket) & 0x1FFFFC;
    if (stage == 0) {
        g_2d_from = next;
        g_2d_mid = g_2d_to = g_2d_bar = 0x200000;
    } else if (stage == 4) {
        g_2d_bar = next;
    } else if (stage == 1) {
        g_2d_mid = next;
    } else if (stage == 3) {
        g_2d_to = next;
    } else if (stage == 5) {
        g_fx_from = next;
    }
}

// The version number is shown in the corner (host.cpp finish_frame) on the title screen
// (its blinking "Press start", str#2 at 800A0818, was drawn this recently) and while one of the
// game's menus is drawn (game_menu.cpp: front end, Start's pause menu); not in play or movies.
uint64_t g_press_start_seen = 0, g_menu_drawn = 0;
std::vector<TextCall>* g_text_calls = nullptr;
bool gpu_title_screen() {
    const uint64_t f = host_frame_count();
    // (both are drawn every frame - "Press start" blinks by colour - so a few frames of grace suffice)
    return (g_press_start_seen && f - g_press_start_seen < 4) || (g_menu_drawn && f - g_menu_drawn < 4);
}

// Flaming text (the selected menu item): after each letter the text routine links up to three
// flame sprites into the letters' ordering-table entry (INSN hook 8001B588, s1 = the flame), so
// each letter's flames covered the letters after it. Its flames now go behind all its letters.
std::vector<uint32_t> g_flames;
extern "C" void psx_hook_text_flame(R3000* c) {
    if (g_flames.size() < 1024) g_flames.push_back(c->r[17] & 0xFFFFFF);
}
void flames_behind(R3000* c, uint32_t from, uint32_t to) {
    if (g_flames.empty()) return;
    const uint32_t e = MEM_R32(c, 0x800FF660) + 0x70 + 4u * MEM_R16(c, 0x800FEEBC + 0x2FE);  // (8001B578)
    std::vector<uint32_t> flames, letters;
    uint32_t p = MEM_R32(c, e) & 0xFFFFFF;
    while (p >= from && p < to && flames.size() + letters.size() < 4096) {  // this call's, at the head
        (std::find(g_flames.begin(), g_flames.end(), p) != g_flames.end() ? flames : letters).push_back(p);
        p = MEM_R32(c, p) & 0xFFFFFF;
    }
    g_flames.clear();
    if (flames.empty() || letters.empty()) return;
    for (uint32_t l : letters) MEM_W8(c, l + 7, MEM_R8(c, l + 7) & ~2u);  // the letters solid (not semi-transparent)
    flames.insert(flames.end(), letters.begin(), letters.end());  // drawn in list order: flames first
    for (size_t i = 0; i < flames.size(); i++) {
        const uint32_t next = i + 1 < flames.size() ? flames[i + 1] : p;
        MEM_W32(c, flames[i], (MEM_R32(c, flames[i]) & 0xFF000000) | next);
    }
    MEM_W32(c, e, (MEM_R32(c, e) & 0xFF000000) | flames[0]);
}

// Entry hooks on the text routine (8001AA28, a2 = the text) and the icon routines: run the
// routine and note the packets it made.
extern "C" int psx_hook_2d_call(R3000* c, int which) {
    static const uint32_t kFn[3] = {0x8001AA28, 0x8006D028, 0x8006D0D4};
    static bool inside[3];
    if (inside[which]) return 0;
    const uint32_t ra = c->r[31], sp = c->r[29];
    const uint32_t from = MEM_R32(c, kNextPacket) & 0x1FFFFC;
    // Packets come from one buffer in order. When the next free packet is back below where the
    // last call started, the frame is being built again (the 60 Hz and smooth-frame renders build
    // one, drop it and build it again before it is drawn): the ranges noted for the dropped build
    // would now cover other packets (the HUD, other text), so they go.
    if (from < g_last_text_from) {
        g_text_packets.clear();
        g_tilts.clear();
    }
    g_last_text_from = from;
    if (which == 0 && c->r[6] == 0x800A0818 && !core_in_ghost()) {
        if (!g_press_start_seen) log_msg(LogCat::Host, "title screen (version shown)");
        g_press_start_seen = host_frame_count();
    }
    const float tilt = which == 0 ? subtitle_tilt(c->r[6]) : 0.0f;  // (a2 = the text)
    const float tx = (float)(int32_t)c->r[4], ty = (float)(int32_t)c->r[5];
    const uint32_t text = c->r[6];
    inside[which] = true;
    g_flames.clear();
    call_indirect(c, kFn[which]);
    inside[which] = false;
    const uint32_t to = MEM_R32(c, kNextPacket) & 0x1FFFFC;
    if (which == 0) flames_behind(c, from, to);
    if (to > from && g_text_packets.size() < 256) g_text_packets.push_back({from, to});
    if (which == 0 && g_text_calls && g_text_calls->size() < 32) g_text_calls->push_back({text, from, to});
    if (to > from && tilt != 0.0f && g_tilts.size() < 16) g_tilts.push_back({from, to, tx, ty, tilt});
    c->r[31] = ra;
    c->r[29] = sp;
    return 1;
}

// A frame's ordering table was drawn: the packets noted so far are done with (each frame's
// render notes its own; after a level, its HUD range must not apply to menu packets).
void gpu_list_done() {
    g_shared_corners.clear();
    g_fx_corners.clear();
    gte_precise_clear();
    g_text_packets.clear();
    g_tilts.clear();
    g_last_text_from = 0;
    g_2d_from = g_2d_mid = g_2d_to = g_2d_bar = g_fx_from = 0;
    if (hw_active()) hw_depth_reset(), hw_rec_note_depth();
}

void gpu_note_movie_frame() {
    g_movie_vblank = g_vblanks;
    g_movie_seen = true;
}
// Full-screen movies show a 24-bit display; levels also decode video for in-game screens
// (15-bit display), which must not count.
bool gpu_movie_playing() { return g_movie_seen && g_vblanks - g_movie_vblank < 30 && (DS.mode & 0x10); }

// No command half-sent and no VRAM transfer running (safe to feed GP0 from elsewhere).

// Everything but VRAM (sim60.cpp rolls this back after its ghost render).
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

