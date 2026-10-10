// Two rendering backends, OpenGL (gpu_gl.cpp + present_gl.cpp) and Vulkan (gpu_vk.cpp +
// present_vk.cpp), behind the same functions (gpu_hw.h, present.h). Each backend defines
// them with its prefix (gl_hw_fill, vk_present_swap, ...); render_backend.cpp forwards
// the plain names to the backend chosen at start-up. The primitives and settings both share
// are defined once (hw_common.cpp).
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

#include "render/gpu_hw.h"
#include "render/present.h"

// X(return type, name, (parameters), (arguments))
#define RENDER_FUNCS(X)                                                                                        \
    X(bool, hw_init, (int scale), (scale))                                                                     \
    X(void, hw_shutdown, (), ())                                                                               \
    X(void, hw_fill, (int x, int y, int w, int h, uint16_t color), (x, y, w, h, color))                        \
    X(void, hw_upload, (int x, int y, int w, int h, const uint16_t* vram), (x, y, w, h, vram))                 \
    X(void, hw_copy, (int sx, int sy, int dx, int dy, int w, int h), (sx, sy, dx, dy, w, h))                   \
    X(void, hw_readback, (int x, int y, int w, int h, uint16_t* vram), (x, y, w, h, vram))                     \
    X(void, hw_upload_all, (const uint16_t* vram), (vram))                                                         \
    X(void, hw_draw_picture, (int x, int y, int w, int h, const uint32_t* rgba), (x, y, w, h, rgba))           \
    X(unsigned, hw_display_texture, (int x, int y, int w, int h, int* px, int* py, int* pw, int* ph),          \
      (x, y, w, h, px, py, pw, ph))                                                                            \
    X(void, hw_depth_reset, (), ())                                                                            \
    X(bool, hw_zbuffer_ready, (), ())                                                                          \
    X(bool, hw_hd_upload, (int level, int x, int y, int w, int h, const uint32_t* rgba), (level, x, y, w, h, rgba))              \
    X(bool, present_init, (SDL_Window * window), (window))                                                     \
    X(void, present_shutdown, (), ())                                                                          \
    X(void, present_set_vsync, (bool on), (on))                                                                \
    X(void, present_game_pixels, (const uint32_t* px, int w, int h, const PresentOptions& o), (px, w, h, o))   \
    X(void, present_game_texture,                                                                              \
      (unsigned tex, int tw, int th, int x, int y, int w, int h, int nh, const PresentOptions& o),             \
      (tex, tw, th, x, y, w, h, nh, o))                                                                        \
    X(void, present_redraw, (const PresentOptions& o), (o))                                                    \
    X(SDL_Renderer*, present_overlay_begin, (), ())                                                            \
    X(void, present_overlay_end, (), ())                                                                       \
    X(void, present_badge, (int slot, const uint32_t* px, int w, int h, int x, int margin, bool changed),     \
      (slot, px, w, h, x, margin, changed))                                                                    \
    X(void, present_swap, (), ())                                                                              \
    X(void, present_save_bmp, (const char* path), (path))                                                      \
    X(bool, present_window_to_game, (float wx, float wy, float* x, float* y), (wx, wy, x, y))

#define RENDER_DECLARE(ret, name, params, args) ret gl_##name params; ret vk_##name params;
RENDER_FUNCS(RENDER_DECLARE)
#undef RENDER_DECLARE

// Vulkan only: the renderer's shaders, pipelines and images, before the device goes (present_vk.cpp).
void vk_renderer_destroy();

enum class Backend { OpenGL, Vulkan };

// Chooses the backend the plain functions forward to (before present_init).
void render_select(Backend b);
Backend render_backend();
const char* render_backend_name(Backend b);
