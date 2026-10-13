// OpenGL renderer for the PS1 GPU: draws into a VRAM render target scaled by 1..8x.
//
// gpu.cpp keeps parsing commands and maintains an exact native-resolution copy of VRAM
// for CPU uploads, fills and copies; polygons, lines and rectangles go here instead of
// the software rasterizer while the hardware renderer is active.
#pragma once

#include <cstdint>

struct HwVtx {
    float x, y;        // native VRAM coordinates (draw offset applied; whole numbers except
                       // for the widescreen HUD, gpu.cpp)
    int r, g, b;
    float u, v;        // texels; fractional with exact texture coordinates (gte.cpp pgxp_uv_find)
    float w = 1.0f;    // view depth for perspective-correct texturing (precise 3D); 1 = affine
    float z = 0.0f;    // view depth (Z-buffer, HwPrim::ztest)
    // Frame interpolation (motion.cpp, hw_replay): where the vertex was last frame, as the
    // position's change (native pixels) and the view depth's ratio (w and z)
    float dx = 0.0f, dy = 0.0f, dr = 1.0f;
};

// Per-primitive drawing state (texture page, palette, blending) as the GPU had it.
struct HwPrim {
    bool textured = false, raw = false, semi = false, gouraud = false;
    bool smooth = false;           // smooth the texture (game text and icons, psx.h g_smooth_font)
    uint32_t box = 0;              // smooth: the texels it may use, u0 | v0 << 8 | u1 << 16 | v1 << 24
    int semi_mode = 0;             // 0..3
    int page_x = 0, page_y = 0;    // texture page base (VRAM pixels)
    int depth = 0;                 // 0 = 4bpp, 1 = 8bpp, 2 = 15bpp
    int clut_x = 0, clut_y = 0;
    bool hd = false;               // a replacement texture (texreplace.cpp): u, v are atlas pixels
    bool glyph = false;            // hd with letters drawn in the game font: the game's own texels never show
    int ztest = 0;                 // Z-buffer: 0 off (2D, glows), 1 test and write (opaque 3D), 2 test only
                                   // (semi-transparent 3D); depth = HwVtx::z (view units, hw_depth_reset clears)
};

struct HwState {
    int area_x1, area_y1, area_x2, area_y2;  // drawing area (inclusive)
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    bool set_mask, check_mask;
    bool mask_inside;  // (gpu.cpp g_mask_inside)
};

bool hw_init(int scale);            // needs a current GL context
void hw_shutdown();
bool hw_active();
int hw_scale();
// Multisample anti-aliasing: samples asked for (1, 2, 4, 8; applies at the next hw_init) and
// in use (lower if the GPU or a 512 MB budget does not allow it).
void hw_set_samples(int n);
int hw_samples_wanted();
int hw_samples();

// Primitives (native coordinates).
void hw_triangle(const HwVtx* v0, const HwVtx* v1, const HwVtx* v2, const HwPrim& p, const HwState& s);
void hw_rect(int x, int y, int w, int h, int u0, int v0, bool flip_x, bool flip_y, uint32_t color,
             const HwPrim& p, const HwState& s);
void hw_line(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, bool gouraud, const HwPrim& p,
             const HwState& s);

// VRAM operations mirrored from the CPU copy (native coordinates).
void hw_fill(int x, int y, int w, int h, uint16_t color);
void hw_upload(int x, int y, int w, int h, const uint16_t* vram_1024x512);
void hw_copy(int sx, int sy, int dx, int dy, int w, int h);
// Reads rendered pixels back into the CPU VRAM copy (for VRAM->CPU transfers).
void hw_readback(int x, int y, int w, int h, uint16_t* vram_1024x512);
// Writes the whole CPU VRAM copy to the GPU (when switching renderers).
void hw_upload_all(const uint16_t* vram_1024x512);
// Draws a picture over a native VRAM rectangle at full detail: `rgba` is (w * scale) x (h *
// scale) pixels, R G B A bytes, top row first (replacement images, replace.cpp).
void hw_draw_picture(int x, int y, int w, int h, const uint32_t* rgba);
bool hw_hd_upload(int level, int x, int y, int w, int h, const uint32_t* rgba);  // replacements' HD copy of VRAM, mip levels 0-2 (texreplace.cpp)

// Finishes pending draws; returns the VRAM texture and the display rectangle in its pixels.
unsigned hw_display_texture(int x, int y, int w, int h, int* px, int* py, int* pw, int* ph);

void hw_depth_reset();  // a new frame: the Z-buffer is cleared
bool hw_zbuffer_ready();  // the VRAM render target has a depth buffer (HwPrim::ztest works)

// Frame interpolation (host.cpp): while on, each frame's primitives are recorded (gpu.cpp notes
// the fills, depth clears and other VRAM writes). A finished frame, shown at x, y, w, h, gets an
// id (-2: nothing drawn since the last; -1: none, or it cannot be drawn again: a picture or copy went into it, or its own pixels were
// used as a texture); hw_replay (render_backend.h) draws it again with every vertex at phase t of
// its motion (0: where it was last frame, 1: where it is) into an image of its own.
void hw_rec_enable(bool on);
void hw_rec_note_fill(int x, int y, int w, int h, uint16_t color);
void hw_rec_note_depth();
void hw_rec_note_write(int x, int y, int w, int h);
int hw_rec_finish(int x, int y, int w, int h);
// The image (a texture of *tw x *th pixels, the frame's top row first; 0: none).
unsigned hw_replay(int id, float t, int* tw, int* th);

// Texture smoothing on/off (bilinear filtering of game textures).
void hw_set_texture_filter(bool on);
