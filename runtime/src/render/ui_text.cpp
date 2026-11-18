// Menu text with stb_truetype: one glyph atlas per pixel size, drawn 1:1 in pixels.
#include "render/ui_text.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
// (also replace.cpp's image loader on Linux: PNG, JPEG, BMP)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "prompt_icons.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int kFirst = 32, kCount = 95;  // printable ASCII

struct Atlas {
    SDL_Texture* tex = nullptr;
    stbtt_packedchar chars[kCount];
    float ascent = 0;  // pixels
};

std::vector<unsigned char> g_font_data;
stbtt_fontinfo g_font;
bool g_font_tried = false, g_font_ok = false;
std::map<std::pair<SDL_Renderer*, int>, Atlas> g_atlases;  // by renderer and pixel size

bool load_font() {
    if (g_font_tried) return g_font_ok;
    g_font_tried = true;
    std::string win = std::getenv("WINDIR") ? std::getenv("WINDIR") : "C:\\Windows";
    const std::string paths[] = {
        win + "\\Fonts\\seguisb.ttf",  // Segoe UI Semibold
        win + "\\Fonts\\segoeui.ttf",
        win + "\\Fonts\\arial.ttf",
        "assets/font.ttf",  // shipped with the Linux build (tools/package_linux.sh)
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",     // Debian / Ubuntu
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",  // Fedora
        "/usr/share/fonts/TTF/DejaVuSans.ttf",                // Arch
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    for (const std::string& p : paths) {
        size_t size = 0;
        void* data = SDL_LoadFile(p.c_str(), &size);
        if (!data) continue;
        g_font_data.assign((unsigned char*)data, (unsigned char*)data + size);
        SDL_free(data);
        if (stbtt_InitFont(&g_font, g_font_data.data(), stbtt_GetFontOffsetForIndex(g_font_data.data(), 0))) {
            g_font_ok = true;
            break;
        }
    }
    return g_font_ok;
}

// The atlas for this renderer's scale, or null (then the debug font is used).
Atlas* atlas_for(SDL_Renderer* r, float size, float* scale) {
    float sx = 1, sy = 1;
    SDL_GetRenderScale(r, &sx, &sy);
    *scale = sy;
    if (!load_font()) return nullptr;
    const int px = (int)std::lround(size * sy);
    auto it = g_atlases.find({r, px});
    if (it != g_atlases.end()) return it->second.tex ? &it->second : nullptr;
    Atlas& a = g_atlases[{r, px}];
    // text height = line height minus a little room above and below
    const float font_px = (float)px * 0.8f;
    int w = 256, h = 64;
    std::vector<unsigned char> bitmap;
    for (;;) {
        bitmap.assign((size_t)w * h, 0);
        stbtt_pack_context pc;
        stbtt_PackBegin(&pc, bitmap.data(), w, h, 0, 1, nullptr);
        const bool ok = stbtt_PackFontRange(&pc, g_font_data.data(), 0, font_px, kFirst, kCount, a.chars) != 0;
        stbtt_PackEnd(&pc);
        if (ok) break;
        if (h >= 4096) return nullptr;
        (w < 2048 ? w : h) *= 2;
    }
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    const float fs = stbtt_ScaleForPixelHeight(&g_font, font_px);
    // centre the font's ascent..descent in the line
    a.ascent = std::round(((float)px - (float)(asc - desc) * fs) * 0.5f + (float)asc * fs);
    std::vector<Uint32> argb((size_t)w * h);
    for (size_t i = 0; i < argb.size(); i++) argb[i] = ((Uint32)bitmap[i] << 24) | 0xFFFFFF;
    a.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!a.tex) return nullptr;
    SDL_UpdateTexture(a.tex, nullptr, argb.data(), w * 4);
    SDL_SetTextureBlendMode(a.tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(a.tex, SDL_SCALEMODE_NEAREST);
    return &a;
}

// The icons: decoded once (straight RGBA), then made at each pixel size they are drawn at, so a
// 128-pixel icon shrunk to 30 stays smooth (the renderer's own scaling would skip pixels).
struct Decoded {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};
std::map<std::string, Decoded> g_decoded;
std::map<std::tuple<SDL_Renderer*, std::string, int>, SDL_Texture*> g_icons;

const Decoded* decoded(const char* name) {
    auto it = g_decoded.find(name);
    if (it != g_decoded.end()) return it->second.rgba.empty() ? nullptr : &it->second;
    Decoded& d = g_decoded[name];
    for (const EmbeddedPng& p : kPromptPngs) {
        if (std::strcmp(p.name, name) != 0) continue;
        int n = 0;
        if (unsigned char* px = stbi_load_from_memory(p.data, (int)p.size, &d.w, &d.h, &n, 4)) {
            d.rgba.assign(px, px + (size_t)d.w * d.h * 4);
            stbi_image_free(px);
        }
    }
    return d.rgba.empty() ? nullptr : &d;
}

}  // namespace

bool ui_icon_exists(const char* name) { return decoded(name) != nullptr; }

bool ui_icon(SDL_Renderer* r, const char* name, float x, float y, float size) {
    const Decoded* d = decoded(name);
    if (!d) return false;
    float sx, sy;
    SDL_GetRenderScale(r, &sx, &sy);
    const int px = std::max(1, (int)std::lround(size * sy));
    SDL_Texture*& tex = g_icons[{r, name, px}];
    if (!tex) {
        std::vector<unsigned char> out((size_t)px * px * 4);
        stbir_resize_uint8_srgb(d->rgba.data(), d->w, d->h, 0, out.data(), px, px, 0, STBIR_RGBA);
        SDL_Surface* s = SDL_CreateSurfaceFrom(px, px, SDL_PIXELFORMAT_ABGR8888, out.data(), px * 4);
        tex = s ? SDL_CreateTextureFromSurface(r, s) : nullptr;
        SDL_DestroySurface(s);
        if (!tex) return false;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    }
    Uint8 cr, cg, cb, ca;
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    SDL_SetTextureAlphaMod(tex, ca);
    SDL_SetRenderScale(r, 1, 1);  // drawn 1:1 in pixels
    SDL_FRect dst{std::round(x * sx), std::round(y * sy), (float)px, (float)px};
    SDL_RenderTexture(r, tex, nullptr, &dst);
    SDL_SetRenderScale(r, sx, sy);
    return true;
}

void ui_text_reset(SDL_Renderer* r) {  // the textures go with their renderer
    for (auto it = g_atlases.begin(); it != g_atlases.end();)
        it = it->first.first == r ? g_atlases.erase(it) : std::next(it);
    for (auto it = g_icons.begin(); it != g_icons.end();) {
        if (std::get<0>(it->first) != r) { ++it; continue; }
        if (it->second) SDL_DestroyTexture(it->second);
        it = g_icons.erase(it);
    }
}

float ui_text_width(SDL_Renderer* r, const char* s, float size) {
    float scale;
    Atlas* a = atlas_for(r, size, &scale);
    if (!a) return (float)SDL_strlen(s) * size * 0.5f;
    float x = 0, y = 0;
    for (; *s; s++) {
        const int c = (unsigned char)*s;
        if (c < kFirst || c >= kFirst + kCount) continue;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(a->chars, 1, 1, c - kFirst, &x, &y, &q, 1);
    }
    return x / scale;
}

void ui_text(SDL_Renderer* r, float x, float y, const char* s, float size) {
    float scale;
    Atlas* a = atlas_for(r, size, &scale);
    if (!a) {  // 8x8 debug font, scaled to half the line height
        float sx, sy;
        SDL_GetRenderScale(r, &sx, &sy);
        const float k = size * 0.5f / 8.0f;
        SDL_SetRenderScale(r, sx * k, sy * k);
        SDL_RenderDebugText(r, x / k, (y + size * 0.25f) / k, s);
        SDL_SetRenderScale(r, sx, sy);
        return;
    }
    Uint8 cr, cg, cb, ca;
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    SDL_SetTextureColorMod(a->tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(a->tex, ca);
    float sx, sy;
    SDL_GetRenderScale(r, &sx, &sy);
    SDL_SetRenderScale(r, 1, 1);  // glyphs are drawn in pixels, 1:1
    float px = std::round(x * sx), py = std::round(y * sy) + a->ascent;
    for (; *s; s++) {
        const int c = (unsigned char)*s;
        if (c < kFirst || c >= kFirst + kCount) continue;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(a->chars, 1, 1, c - kFirst, &px, &py, &q, 1);
        // (a 1x1 "atlas size" makes s/t texel positions)
        SDL_FRect src{q.s0, q.t0, q.s1 - q.s0, q.t1 - q.t0};
        SDL_FRect dst{q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
        SDL_RenderTexture(r, a->tex, &src, &dst);
    }
    SDL_SetRenderScale(r, sx, sy);
}
