// Window presentation through OpenGL: game image (software or hardware-rendered),
// aspect ratio, CRT filter, and the settings overlay.
#pragma once

#include <SDL3/SDL.h>

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

// 640x480 overlay drawn with an SDL software renderer, composited on top.
SDL_Renderer* present_overlay_begin();
void present_overlay_end();

void present_swap();
void present_save_bmp(const char* path);  // back buffer -> BMP (debug screenshots)

// Window pixel -> 640x480 position inside the game image (for mouse aiming).
bool present_window_to_game(float wx, float wy, float* x, float* y);
