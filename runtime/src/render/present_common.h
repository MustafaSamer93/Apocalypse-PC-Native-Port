// What the two presenters (present_gl.cpp, present_vk.cpp) share: the window, the last game image
// (menus draw it again under the overlay), where it went in the window, and the overlay's surface.
#pragma once

#include <SDL3/SDL.h>

#include "render/present.h"

namespace pres {

extern SDL_Window* g_win;

// The last game image: a renderer texture (or the software picture's) and its region.
struct LastImage {
    bool valid = false;
    unsigned tex = 0;
    int tex_w = 0, tex_h = 0, x = 0, y = 0, w = 0, h = 0, native_h = 240;
};
extern LastImage g_last;

// Where the game image goes in a ww x wh window (pixels, y down): as large as fits in its aspect
// ratio, centred. Kept for present_window_to_game.
extern float g_cx, g_cy, g_cw, g_ch;
void fit_image(Aspect a, int ww, int wh);

// The overlay's software surface and renderer, (re)made at the window's overlay size
// (present_overlay_size); *remade tells the presenter to remake its texture too.
extern SDL_Surface* g_ov_surf;
SDL_Renderer* overlay_begin(bool* remade);
void overlay_flush();  // what was drawn on the renderer into g_ov_surf
void overlay_shutdown();
// The overlay's place in a ww x wh window: as much of it as fits at 4:3, centred.
void overlay_area(float ww, float wh, float* x, float* y, float* w, float* h);

}  // namespace pres
