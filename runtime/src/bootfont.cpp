// The BIOS boot sequence's text at the window's resolution, in the BIOS's own letters. The BIOS
// draws its words ("SONY", "COMPUTER ENTERTAINMENT", "PlayStation", "Licensed by ...", "SCEA",
// "TM") from small 4-bit textures whose palettes shade the letters' edges. When it draws one, the
// polygon is not drawn; its texels are traced over the picture instead: each texel's ink coverage
// (how far its colour is from the background towards the ink's) is enlarged smoothly (bicubic) to
// window pixels and cut at half coverage, so the letters keep their exact shapes with sharp
// edges. The colours are read from the palette at every present, so the BIOS's fades still show.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "present.h"
#include "psx.h"

namespace {

struct Tex {
    int x, y, w, h;  // in VRAM words (4-bit: 4 texels each)
    uint32_t hash;
    std::vector<uint8_t> idx;  // palette index of each texel
    int bg;                    // the background's index (0: see-through)
    std::vector<int> inks;     // the ink's indices, most used first (5% or more of the ink)
};
std::vector<Tex> g_tex;

struct Item {
    size_t tex;            // into g_tex
    int u0, v0, u1, v1;    // texels it shows (of its texture), [u0, u1) x [v0, v1)
    float x0, y0, x1, y1;  // where on the screen (VRAM coordinates)
    int clut_x, clut_y;
    int r, g, b;  // the polygon's colour (128 = as the texture), -1: raw
};
std::vector<Item> g_items;

uint32_t fnv(const uint16_t* vram, int x, int y, int w, int h) {
    uint32_t hash = 0x811C9DC5;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const uint16_t p = vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
            hash = (hash ^ (p & 0xFF)) * 0x01000193;
            hash = (hash ^ (p >> 8)) * 0x01000193;
        }
    return hash;
}

int dist(uint16_t a, uint16_t b) {
    int d = 0;
    for (int s = 0; s < 15; s += 5) d += std::abs((int)((a >> s) & 31) - (int)((b >> s) & 31));
    return d;
}

// The item's ink colour now (0xRRGGBB) and each palette index's ink coverage (0..1).
uint32_t shade(const Item& it, const Tex& t, const uint16_t* vram, float cov[16]) {
    const uint16_t* row = vram + (it.clut_y & 511) * 1024;
    auto at = [&](int i) { return row[(it.clut_x + i) & 1023]; };
    // the ink: its most used colour, or on a solid background the one that stands out most
    uint16_t ink = at(t.inks.empty() ? 1 : t.inks[0]);
    if (t.bg != 0)
        for (int i : t.inks)
            if (dist(at(i), at(t.bg)) > dist(ink, at(t.bg))) ink = at(i);
    for (int i = 0; i < 16; i++) cov[i] = 0.0f;
    if (t.bg != 0) {  // coverage: the way from the background's colour to the ink's
        const float full = (float)std::max(1, dist(ink, at(t.bg)));
        for (int i = 0; i < 16; i++)
            if (i != t.bg) cov[i] = std::min(1.0f, (float)dist(at(i), at(t.bg)) / full);
    } else {  // see-through: the edge shades are the ones farthest from the ink
        int far = 1;
        for (int i : t.inks) far = std::max(far, dist(at(i), ink));
        for (int i = 1; i < 16; i++) cov[i] = std::clamp(1.0f - (float)dist(at(i), ink) / (1.4f * (float)far), 0.0f, 1.0f);
    }
    int r = (ink & 31) << 3, g = ((ink >> 5) & 31) << 3, b = ((ink >> 10) & 31) << 3;
    if (it.r >= 0) r = std::min(255, r * it.r / 128), g = std::min(255, g * it.g / 128), b = std::min(255, b * it.b / 128);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

float cubic(float a, float b, float c, float d, float t) {  // Catmull-Rom
    return b + 0.5f * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}

// The item traced into `out` (w x h, 0xAARRGGBB) over the window box (x0, y0)-(x1, y1).
void trace(const Item& it, const Tex& t, const float cov[16], uint32_t rgb, float x0, float y0, float x1, float y1,
           std::vector<uint32_t>& out, int w, int h) {
    const int tw = t.w * 4;
    auto c = [&](int u, int v) {
        u = std::clamp(u, it.u0, it.u1 - 1), v = std::clamp(v, it.v0, it.v1 - 1);
        return cov[t.idx[(size_t)v * tw + u]];
    };
    const float fu = (float)(it.u1 - it.u0) / (x1 - x0), fv = (float)(it.v1 - it.v0) / (y1 - y0);  // texels per pixel
    const float edge = 0.5f * std::max(fu, fv);  // the cut's soft width: about one window pixel
    for (int py = std::max(0, (int)std::floor(y0)); py < std::min(h, (int)std::ceil(y1)); py++)
        for (int px = std::max(0, (int)std::floor(x0)); px < std::min(w, (int)std::ceil(x1)); px++) {
            const float u = (float)it.u0 + ((float)px + 0.5f - x0) * fu - 0.5f;
            const float v = (float)it.v0 + ((float)py + 0.5f - y0) * fv - 0.5f;
            const int iu = (int)std::floor(u), iv = (int)std::floor(v);
            const float su = u - (float)iu, sv = v - (float)iv;
            float rows[4];
            for (int k = 0; k < 4; k++)
                rows[k] = cubic(c(iu - 1, iv - 1 + k), c(iu, iv - 1 + k), c(iu + 1, iv - 1 + k), c(iu + 2, iv - 1 + k), su);
            const float cv = cubic(rows[0], rows[1], rows[2], rows[3], sv);
            const float a = std::clamp((cv - 0.5f) / (2 * edge) + 0.5f, 0.0f, 1.0f);
            if (a <= 0.0f) continue;
            uint32_t& d = out[(size_t)py * w + px];
            d = std::max(d >> 24, (uint32_t)(a * 255.0f + 0.5f)) << 24 | rgb;
        }
}

}  // namespace

// gpu.cpp, during the boot sequence: an upload into VRAM (a texture: keep its texels).
void bootfont_upload(int x, int y, int w, int h, const uint16_t* vram) {
    if (h < 4 || x < 512) return;
    g_items.clear();  // (they index g_tex)
    g_tex.erase(std::remove_if(g_tex.begin(), g_tex.end(), [&](const Tex& t) {
        return t.x < x + w && x < t.x + t.w && t.y < y + h && y < t.y + t.h;
    }), g_tex.end());
    Tex t{x, y, w, h, fnv(vram, x, y, w, h), std::vector<uint8_t>((size_t)w * 4 * h), 0, {}};
    int count[16] = {};
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w * 4; i++) {
            const int v = (vram[((y + j) & 511) * 1024 + ((x + i / 4) & 1023)] >> (4 * (i % 4))) & 15;
            t.idx[(size_t)j * w * 4 + i] = (uint8_t)v;
            count[v]++;
        }
    t.bg = (int)(std::max_element(count, count + 16) - count);  // the background (0, or solid)
    count[t.bg] = 0;
    int total = 0;
    for (int n : count) total += n;
    for (int i = 0; i < 16; i++)
        if (count[i] && count[i] * 20 >= total) t.inks.push_back(i);
    std::sort(t.inks.begin(), t.inks.end(), [&](int a, int b) { return count[a] > count[b]; });
    if (!t.inks.empty()) g_tex.push_back(std::move(t));
}

// gpu.cpp: a fill (the BIOS clears the screen): the text drawn there is gone.
void bootfont_fill(int x, int y, int w, int h) {
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [&](const Item& it) {
        const float cx = (it.x0 + it.x1) * 0.5f, cy = (it.y0 + it.y1) * 0.5f;
        return cx >= x && cx < x + w && cy >= y && cy < y + h;
    }), g_items.end());
}

// gpu.cpp, during the boot sequence: a textured polygon with screen corners (sx, sy) (after the
// widescreen squeeze) and texels (u, v) of the page at (page_x, page_y): true if it shows one of
// the textures kept above (then bootfont_present draws it instead).
bool bootfont_draw(int nv, const float* sx, const float* sy, const int* u, const int* v, int page_x, int page_y,
                   int depth, int clut_x, int clut_y, bool raw, int r, int g, int b) {
    if (depth != 0 || g_tex.empty()) return false;
    float x0 = sx[0], x1 = sx[0], y0 = sy[0], y1 = sy[0];
    int u0 = u[0], u1 = u[0], v0 = v[0], v1 = v[0];
    for (int i = 1; i < nv; i++) {
        x0 = std::min(x0, sx[i]), x1 = std::max(x1, sx[i]), y0 = std::min(y0, sy[i]), y1 = std::max(y1, sy[i]);
        u0 = std::min(u0, u[i]), u1 = std::max(u1, u[i]), v0 = std::min(v0, v[i]), v1 = std::max(v1, v[i]);
    }
    if (u1 <= u0 || v1 <= v0 || x1 <= x0 || y1 <= y0) return false;
    const int tx = page_x + u0 / 4, ty = page_y + v0;
    for (size_t n = 0; n < g_tex.size(); n++) {
        const Tex& t = g_tex[n];
        if (tx < t.x || tx >= t.x + t.w || ty < t.y || ty >= t.y + t.h) continue;
        const Item it{n, page_x * 4 + u0 - t.x * 4, page_y + v0 - t.y, page_x * 4 + u1 - t.x * 4, page_y + v1 - t.y,
                      x0, y0, x1, y1, clut_x, clut_y, raw ? -1 : r, g, b};
        if (it.u1 > t.w * 4 || it.v1 > t.h) return false;
        // drawn again at the same place (the BIOS redraws without clearing, from either of its two
        // copies of each texture): replaces it
        g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [&](const Item& o) {
            return g_tex[o.tex].hash == t.hash && o.u0 == it.u0 && o.v0 == it.v0 && std::abs(o.x0 - it.x0) < 2 &&
                   std::abs(o.y0 - it.y0) < 2;
        }), g_items.end());
        g_items.push_back(it);
        if (g_items.size() > 64) g_items.erase(g_items.begin(), g_items.begin() + 32);
        return true;
    }
    return false;
}

// host_sdl.cpp, with the boot sequence's picture just presented: its text over it (window pixels).
void bootfont_present(const GpuDisplay& d, SDL_Window* window) {
    if (g_items.empty() || d.w <= 0 || d.h <= 0) return;
    int ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    float ax, ay, bx, by;  // the game's 640x480 space at the window's corners
    present_window_to_game(0, 0, &ax, &ay);
    present_window_to_game((float)ww, (float)wh, &bx, &by);
    if (bx <= ax || by <= ay) return;
    auto wx = [&](float vx) { return ((vx - (float)d.x) / (float)d.w * 640.0f - ax) / (bx - ax) * (float)pw; };
    auto wy = [&](float vy) { return ((vy - (float)d.y) / (float)d.h * 480.0f - ay) / (by - ay) * (float)ph; };
    // what is shown now (items of this frame buffer), and its bounds
    std::vector<const Item*> shown;
    float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;
    for (const Item& it : g_items) {
        const float cy = (it.y0 + it.y1) * 0.5f;
        if (cy < (float)d.y || cy >= (float)(d.y + d.h)) continue;
        shown.push_back(&it);
        l = std::min(l, wx(it.x0)), r = std::max(r, wx(it.x1)), t = std::min(t, wy(it.y0)), b = std::max(b, wy(it.y1));
    }
    if (shown.empty()) return;
    const int bl = std::max(0, (int)std::floor(l) - 2), bt = std::max(0, (int)std::floor(t) - 2);
    const int bw = std::min(pw, (int)std::ceil(r) + 2) - bl, bh = std::min(ph, (int)std::ceil(b) + 2) - bt;
    if (bw <= 0 || bh <= 0) return;
    // traced again only when something changed (the palettes fade, the window is resized)
    static std::string last;
    static std::vector<uint32_t> px;
    std::string key = std::to_string(bl) + "," + std::to_string(bt) + "," + std::to_string(bw) + "," + std::to_string(bh);
    std::vector<uint32_t> rgbs;
    std::vector<std::vector<float>> covs;
    for (const Item* it : shown) {
        float cov[16];
        rgbs.push_back(shade(*it, g_tex[it->tex], d.vram, cov));
        covs.emplace_back(cov, cov + 16);
        char k[128];
        std::snprintf(k, sizeof k, ";%zu,%d,%d,%.1f,%.1f,%.1f,%.1f,%06X", it->tex, it->u0, it->v0, it->x0, it->y0, it->x1,
                      it->y1, rgbs.back());
        key += k;
        for (float c : covs.back()) key += std::to_string((int)(c * 255));
    }
    const bool changed = key != last;
    if (changed) {
        last = key;
        px.assign((size_t)bw * bh, 0);
        for (size_t i = 0; i < shown.size(); i++) {
            const Item& it = *shown[i];
            trace(it, g_tex[it.tex], covs[i].data(), rgbs[i], wx(it.x0) - (float)bl, wy(it.y0) - (float)bt,
                  wx(it.x1) - (float)bl, wy(it.y1) - (float)bt, px, bw, bh);
        }
    }
    present_badge(2, px.data(), bw, bh, bl, ph - (bt + bh), changed);
}
