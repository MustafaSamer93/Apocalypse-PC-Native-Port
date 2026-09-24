// OpenGL renderer for the PS1 GPU: draws into a VRAM render target scaled by 1..8x.
//
// gpu.cpp keeps parsing commands and maintains an exact native-resolution copy of VRAM
// for CPU uploads, fills and copies; polygons, lines and rectangles go here instead of
// the software rasterizer while the hardware renderer is active.
#pragma once

#include <cstdint>

struct HwVtx {
    int x, y;          // native VRAM coordinates (draw offset applied)
    int r, g, b;
    int u, v;
};

// Per-primitive drawing state (texture page, palette, blending) as the GPU had it.
struct HwPrim {
    bool textured = false, raw = false, semi = false, gouraud = false;
    int semi_mode = 0;             // 0..3
    int page_x = 0, page_y = 0;    // texture page base (VRAM pixels)
    int depth = 0;                 // 0 = 4bpp, 1 = 8bpp, 2 = 15bpp
    int clut_x = 0, clut_y = 0;
};

struct HwState {
    int area_x1, area_y1, area_x2, area_y2;  // drawing area (inclusive)
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    bool set_mask, check_mask;
};

bool hw_init(int scale);            // needs a current GL context
void hw_shutdown();
bool hw_active();
int hw_scale();

// Primitives (native coordinates).
void hw_triangle(const HwVtx* v0, const HwVtx* v1, const HwVtx* v2, const HwPrim& p, const HwState& s);
void hw_rect(int x, int y, int w, int h, int u0, int v0, bool flip_x, bool flip_y, uint32_t color,
             const HwPrim& p, const HwState& s);
void hw_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool gouraud, const HwPrim& p,
             const HwState& s);

// VRAM operations mirrored from the CPU copy (native coordinates).
void hw_fill(int x, int y, int w, int h, uint16_t color);
void hw_upload(int x, int y, int w, int h, const uint16_t* vram_1024x512);
void hw_copy(int sx, int sy, int dx, int dy, int w, int h);
// Reads rendered pixels back into the CPU VRAM copy (for VRAM->CPU transfers).
void hw_readback(int x, int y, int w, int h, uint16_t* vram_1024x512);
// Writes the whole CPU VRAM copy to the GPU (when switching renderers).
void hw_upload_all(const uint16_t* vram_1024x512);

// Finishes pending draws; returns the VRAM texture and the display rectangle in its pixels.
unsigned hw_display_texture(int x, int y, int w, int h, int* px, int* py, int* pw, int* ph);
// Extra frames (smooth.cpp). A capture copies a rendered VRAM rectangle (native
// coordinates) into its own texture; slots survive until the renderer is restarted.
void hw_capture(int slot, int x, int y, int w, int h);
unsigned hw_capture_texture(int slot, int* tex_w, int* tex_h);  // 0 if the slot is empty
// Keeps a copy of a VRAM rectangle so it can be put back after drawing over it.
void hw_stash(int x, int y, int w, int h);
void hw_unstash();

// Texture smoothing on/off (bilinear filtering of game textures).
void hw_set_texture_filter(bool on);
