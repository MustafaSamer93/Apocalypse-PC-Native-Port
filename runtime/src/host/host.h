// The host platform (SDL3): window and presentation, frame pacing, audio, controllers, the
// settings (settings.ini) and the settings screen.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GpuDisplay;

// ---------------------------------------------------------------- start-up (host.cpp)

bool host_init(const char* title, int scale);  // scale: window height 240*scale (0 = 1080 lines)
void host_shutdown();
extern int g_renderer_override;  // --renderer opengl|vulkan: 0 / 1, -1 = the saved setting

// ---------------------------------------------------------------- frames, audio, messages (host.cpp)

void host_poll();                    // pumps OS events (and opens the settings screen)
uint64_t host_frame_count();
bool host_quit_requested();
void host_request_quit();            // close the game (as closing the window does)
void host_present(const GpuDisplay& d);
void host_throttle_frame();          // holds the display rate (vsync or a 59.94 Hz timer)
void host_interp_poll();             // above 60 fps: an in-between image due now (from the emulation)
void host_set_turbo(bool on);        // run as fast as possible
void host_audio_push(const int16_t* stereo, int frames);
void host_audio_clear();
void host_toast(const std::string& text);  // a short message in the corner ("State 1 saved")

// Scripted runs: --chapter, --screenshot
extern int g_start_state;                // state slot loaded at frame 30 (-1: none)
extern std::vector<uint64_t> g_shot_frames;
extern std::string g_shot_prefix;

// ---------------------------------------------------------------- controllers (input.cpp)

struct PadState {
    uint16_t buttons = 0xFFFF;  // active low, PS1 digital pad bit order
    bool connected = true;
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;  // analog sticks, 0x80 = centre
    bool has_sticks = false;    // a real gamepad with sticks is connected
};
// `analog`: the game switched the pad to analog mode, so the left stick stops doubling as the d-pad.
PadState host_pad(int port, bool analog = false);
void host_rumble(int port, uint8_t small_motor, uint8_t large_motor);
bool host_parse_press_script(const char* spec);  // --press BUTTON@FRAME[+FRAMES],...

// The game's Options -> Controls (game_menu.cpp) opens the controls screen (settings_menu.cpp) at the
// next host_poll: play with the keyboard and mouse (mode 0) or a controller (1), and each action's two
// inputs.
void controls_open();

// The game's menus with the mouse (game_menu.cpp): the pointer's height over the game image (0..1, `on`:
// over it, in the focused window) and what happened since the last look (a move, left / right click, wheel
// notches: + = down). While a menu is up the mouse is the menu's only (no aiming or firing with it).
struct MenuMouse {
    bool on = false;
    float y = 0;
    bool moved = false, click = false, back = false;
    int wheel = 0;
};
MenuMouse host_menu_mouse();
void host_menu_press(uint16_t buttons);  // PS1 pad bits (1 = pressed), held a moment for the game to see

// ---------------------------------------------------------------- enhancement settings (settings.cpp)
// Shared by the settings screen and the game's own Options -> Recomp Enhancements page
// (game_menu.cpp). A change applies and is saved at once.

enum EnhSetting {
    E_FPS, E_DRAWDIST, E_SMOOTH_FONT, E_FONT_SCALE, E_RENDERER, E_SCREEN, E_DISPLAY,
    E_AUTOSAVE, E_SUBTITLES, E_AA, E_CHEAT_HEALTH, E_CHEAT_LIVES, E_CHEAT_WEAPONS, E_CHEAT_AMMO,
    E_CHEAT_BOMBS, E_CHEAT_ONEHIT, E_FPS_COUNTER, E_CRT, E_GAME_FONT, E_TEX_SMOOTH, E_REPLACE, E_SOUND_FILTER,
    E_MENU_FLAMES,
};
extern bool g_menu_flames;  // the selected menu item's flames: in its letters (Remastered) or round them (Original)
extern int g_sound_filter;  // the voices' interpolation: 0 Original (Gaussian), 1 Smooth (cubic), 2 Sharp (spu.cpp)
const char* enh_name(int id);
std::string enh_value(int id);
void enh_change(int id, int dir);  // the next (1) or previous (-1) choice
