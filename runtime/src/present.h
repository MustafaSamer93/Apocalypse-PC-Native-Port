// Window presentation through OpenGL: game image (software or hardware-rendered),
// aspect ratio, CRT filter, and the settings overlay.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>

enum class Aspect { A4_3, A16_9, A16_10, Stretch };

struct PresentOptions {
    Aspect aspect = Aspect::A4_3;
    bool crt = false;
    bool fxaa = false;  // edge-smoothing pass (not with the CRT filter)
};

bool present_init(SDL_Window* window);
void present_shutdown();
void present_set_vsync(bool on);

// Draws the game image into the back buffer. Either `pixels` (w*h XRGB8888, row 0 = top)
// or a GL texture region in texel units (row y = top; see gpu_hw.cpp) is shown.
// `native_h` is the PS1 line count (for CRT scanlines).
void present_game_pixels(const uint32_t* pixels, int w, int h, const PresentOptions& o);
void present_game_texture(unsigned tex, int tex_w, int tex_h, int x, int y, int w, int h, int native_h,
                          const PresentOptions& o);
// Draws the last game image again (menus redraw it under the overlay).
void present_redraw(const PresentOptions& o);

// Overlay drawn with an SDL software renderer, composited on top. It covers the window's
// largest 4:3 area at its real pixel size (so text stays sharp); the renderer's scale is set
// so drawing uses 640x480 units.
SDL_Renderer* present_overlay_begin();
void present_overlay_end();
// A small w x h image (ARGB, row 0 = top) in the window's lower-left corner, `margin` pixels in
// (the FPS counter). The pixels are uploaded only when `changed`; otherwise the last are shown.
void present_badge(int slot, const uint32_t* px, int w, int h, int x, int margin, bool changed);  // slot 0 FPS, 1 version, 2 boot text

void present_swap();
void present_save_bmp(const char* path);  // back buffer -> BMP (debug screenshots)
// Top-down 32-bit pixels, blue first (alpha ignored) -> 24-bit BMP; logs the result.
bool save_bmp_bgrx(const uint8_t* px, int w, int h, const char* path);

// Window pixel -> 640x480 position inside the game image (for mouse aiming).
bool present_window_to_game(float wx, float wy, float* x, float* y);

// Overlay pixel size for a window: its largest 4:3 area, 480 to 2160 lines.
inline void present_overlay_size(int win_w, int win_h, int* w, int* h) {
    *h = std::clamp(std::min(win_h, win_w * 3 / 4), 480, 2160);
    *w = *h * 4 / 3;
}
