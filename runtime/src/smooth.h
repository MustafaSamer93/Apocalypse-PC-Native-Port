// Smooth frame rate: the game simulates at a fixed 30 Hz (see smooth.cpp), so for a
// higher display rate the engine's own render is run again for moments between two
// simulation steps, with object and camera positions blended the way the engine already
// does when it drops a frame.
#pragma once

#include <cstdint>

// 1 = off (the original 30 fps); 2..5 = images per simulation step (60..150 fps).
void smooth_set_factor(double k);
double smooth_factor();  // images per game step of 2 vblanks (fps / 30; 2.5 = 75 fps)
// Images per step that suit a display refresh rate (e.g. 90 Hz -> 3).
double smooth_factor_for_refresh(float hz);

// Forget the previous step (level start, loaded state): the next frame is not blended.
void smooth_reset();

// Frames the game has shown so far (display flips).
uint64_t smooth_flips();

// Presenter: the image to show `t` emulated vblanks (fractional) after host frame 0.
// Returns false to show the game's own display (VRAM), otherwise an extra-frame texture.
bool smooth_pick(double t, unsigned* tex, int* tex_w, int* tex_h);
