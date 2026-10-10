// Smooth text for the settings overlay: a TrueType font (stb_truetype) rasterised at the
// overlay's real pixel size, so menus stay sharp at any window size. Falls back to SDL's
// 8x8 debug font when no font file is found.
#pragma once

#include <SDL3/SDL.h>

// Positions and sizes are in the overlay's 640x480 units (the renderer's scale maps them
// to pixels). `y` is the top of the line; `size` is the line height. Uses the draw color.
void ui_text(SDL_Renderer* r, float x, float y, const char* s, float size);
float ui_text_width(SDL_Renderer* r, const char* s, float size);

// Forgets the glyph textures made on `r`; call before destroying that renderer.
void ui_text_reset(SDL_Renderer* r);
