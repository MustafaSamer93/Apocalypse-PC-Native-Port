#include "render/present_common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/psx.h"
#include "render/ui_text.h"

namespace pres {

SDL_Window* g_win = nullptr;
LastImage g_last;
float g_cx = 0, g_cy = 0, g_cw = 1, g_ch = 1;
SDL_Surface* g_ov_surf = nullptr;

namespace {
SDL_Renderer* g_ov_ren = nullptr;
}

void fit_image(Aspect a, int ww, int wh) {
    const float r = aspect_ratio(a);
    const float ar = r > 0 ? r : (float)ww / (float)std::max(wh, 1);  // Stretch: the window's own
    float cw = (float)ww, ch = cw / ar;
    if (ch > (float)wh) { ch = (float)wh; cw = ch * ar; }
    g_cx = ((float)ww - cw) * 0.5f;
    g_cy = ((float)wh - ch) * 0.5f;
    g_cw = cw;
    g_ch = ch;
}

SDL_Renderer* overlay_begin(bool* remade) {
    int ww, wh, ow, oh;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    present_overlay_size(ww, wh, &ow, &oh);
    *remade = !g_ov_surf || g_ov_surf->w != ow || g_ov_surf->h != oh;
    if (*remade) {
        overlay_shutdown();
        g_ov_surf = SDL_CreateSurface(ow, oh, SDL_PIXELFORMAT_ARGB8888);
        g_ov_ren = SDL_CreateSoftwareRenderer(g_ov_surf);
    }
    SDL_SetRenderScale(g_ov_ren, (float)oh / 480.0f, (float)oh / 480.0f);
    SDL_SetRenderDrawColor(g_ov_ren, 0, 0, 0, 0);
    SDL_SetRenderDrawBlendMode(g_ov_ren, SDL_BLENDMODE_NONE);
    SDL_RenderClear(g_ov_ren);
    return g_ov_ren;
}

void overlay_flush() { SDL_RenderPresent(g_ov_ren); }  // (a software renderer draws into its surface here)

void overlay_shutdown() {
    ui_text_reset(g_ov_ren);
    if (g_ov_ren) SDL_DestroyRenderer(g_ov_ren), g_ov_ren = nullptr;
    if (g_ov_surf) SDL_DestroySurface(g_ov_surf), g_ov_surf = nullptr;
}

void overlay_area(float ww, float wh, float* x, float* y, float* w, float* h) {
    float ch = wh, cw = ch * 4.0f / 3.0f;
    if (cw > ww) { cw = ww; ch = cw * 0.75f; }
    *x = (ww - cw) * 0.5f;
    *y = (wh - ch) * 0.5f;
    *w = cw;
    *h = ch;
}

}  // namespace pres

using namespace pres;

bool present_window_to_game(float wx, float wy, float* x, float* y) {
    // SDL reports window coordinates; the image was placed in pixels
    int ww, wh, pw, ph;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    const float sx = ww ? (float)pw / (float)ww : 1.0f, sy = wh ? (float)ph / (float)wh : 1.0f;
    *x = (wx * sx - g_cx) / g_cw * 640.0f;
    *y = (wy * sy - g_cy) / g_ch * 480.0f;
    return *x >= 0 && *x < 640 && *y >= 0 && *y < 480;
}

// 24-bit, so the window's alpha (0 in the GL back buffer) cannot make viewers show it
// transparent or blank.
bool save_bmp_bgrx(const uint8_t* px, int w, int h, const char* path) {
    const uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; i++) hdr[off + i] = (uint8_t)(v >> (8 * i)); };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, (uint32_t)w);
    put32(22, (uint32_t)-h);  // top-down rows
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, row * (uint32_t)h);
    FILE* f = std::fopen(path, "wb");
    bool ok = f && std::fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    std::vector<uint8_t> line(row, 0);
    for (int y = 0; ok && y < h; y++) {
        const uint8_t* src = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) std::memcpy(&line[(size_t)x * 3], src + (size_t)x * 4, 3);
        ok = std::fwrite(line.data(), 1, row, f) == row;
    }
    if (f && std::fclose(f) != 0) ok = false;
    if (ok) log_msg(LogCat::Host, "screenshot saved to %s", path);
    else log_msg(LogCat::Host, "screenshot %s failed", path);
    return ok;
}
