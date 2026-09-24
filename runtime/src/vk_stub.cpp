// Vulkan backend: temporary stubs (start-up reports it unavailable).
#include "render_backend.h"

bool vk_hw_init(int scale) { return false; }
void vk_hw_shutdown() { }
bool vk_hw_active() { return false; }
int vk_hw_scale() { return 0; }
void vk_hw_set_samples(int n) { }
int vk_hw_samples_wanted() { return 0; }
int vk_hw_samples() { return 0; }
void vk_hw_set_texture_filter(bool on) { }
void vk_hw_fill(int x, int y, int w, int h, uint16_t color) { }
void vk_hw_upload(int x, int y, int w, int h, const uint16_t* vram) { }
void vk_hw_copy(int sx, int sy, int dx, int dy, int w, int h) { }
void vk_hw_readback(int x, int y, int w, int h, uint16_t* vram) { }
void vk_hw_upload_all(const uint16_t* vram) { }
void vk_hw_capture(int slot, int x, int y, int w, int h) { }
unsigned vk_hw_capture_texture(int slot, int* tw, int* th) { return 0; }
void vk_hw_stash(int x, int y, int w, int h) { }
void vk_hw_unstash() { }
bool vk_present_init(SDL_Window * window) { return false; }
void vk_present_shutdown() { }
void vk_present_set_vsync(bool on) { }
void vk_present_game_pixels(const uint32_t* px, int w, int h, const PresentOptions& o) { }
void vk_present_redraw(const PresentOptions& o) { }
SDL_Renderer* vk_present_overlay_begin() { return nullptr; }
void vk_present_overlay_end() { }
void vk_present_swap() { }
void vk_present_save_bmp(const char* path) { }
bool vk_present_window_to_game(float wx, float wy, float* x, float* y) { return false; }
void vk_hw_triangle(const HwVtx*, const HwVtx*, const HwVtx*, const HwPrim&, const HwState&) {}
void vk_hw_rect(int, int, int, int, int, int, bool, bool, uint32_t, const HwPrim&, const HwState&) {}
void vk_hw_line(int, int, uint32_t, int, int, uint32_t, bool, const HwPrim&, const HwState&) {}
unsigned vk_hw_display_texture(int, int, int, int, int*, int*, int*, int*) { return 0; }
void vk_present_game_texture(unsigned, int, int, int, int, int, int, int, const PresentOptions&) {}
