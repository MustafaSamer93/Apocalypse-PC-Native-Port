// What the host layer's files share (host.cpp, input.cpp, settings.cpp, settings_menu.cpp).
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "render/present.h"

namespace host {

// PS1 pad bits (active low in the wire format; here 1 = pressed)
enum : uint16_t {
    B_SELECT = 1 << 0, B_L3 = 1 << 1, B_R3 = 1 << 2, B_START = 1 << 3,
    B_UP = 1 << 4, B_RIGHT = 1 << 5, B_DOWN = 1 << 6, B_LEFT = 1 << 7,
    B_L2 = 1 << 8, B_R2 = 1 << 9, B_L1 = 1 << 10, B_R1 = 1 << 11,
    B_TRIANGLE = 1 << 12, B_CIRCLE = 1 << 13, B_CROSS = 1 << 14, B_SQUARE = 1 << 15,
};

// ---------------------------------------------------------------- host.cpp

extern SDL_Window* g_window;
extern SDL_AudioStream* g_audio;
extern bool g_video_ok;          // a renderer runs (the settings screen needs one)
extern bool g_quit;
extern uint64_t g_frame;         // frames presented so far
extern uint64_t g_next_frame_ns; // when the timer-paced next frame is due
void mouse_autohide(const SDL_Event* e);  // with every event (null: just the timer)
PresentOptions present_options();         // how the game image is shown now

// ---------------------------------------------------------------- settings.cpp

struct GfxSettings {
#ifdef _WIN32
    int scale = 5;          // 0 = software renderer, 1..9 = internal resolution 240*scale lines
#else
    int scale = 4;          // (Linux: what holds 60 fps on the Steam Deck, with FXAA and Vulkan)
#endif
    Aspect aspect = Aspect::A16_9;
    bool crt = false;       // CRT filter (present_gl.cpp / present_vk.cpp)
    bool texture_filter = false;
    int fps = 60;           // 30 = the original (no 60 Hz simulation), 60
#ifdef _WIN32
    int aa = 3;             // anti-aliasing: bit 0 = FXAA pass, bit 1 = supersampling (SSAA)
#else
    int aa = 1;             // (Linux: FXAA only; SSAA rendered the Steam Deck at 7x, 20-30 fps)
#endif
    int msaa = 1;           // multisample anti-aliasing: samples per pixel (1, 2, 4, 8)
#ifdef _WIN32
    bool vulkan = false;    // renderer: Vulkan instead of OpenGL (applies at the next start)
#else
    bool vulkan = true;     // (Linux: Mesa's OpenGL ran the Steam Deck at 20-50 fps where Vulkan holds 60)
#endif
};
extern GfxSettings g_gfx;
extern int g_active_scale;   // renderer scale in use (render_scale(); 0 = software)
extern bool g_fullscreen;
extern bool g_fps_counter;
extern const char* const kAaNames[4];
const char* settings_file();  // settings.ini (APOC_SETTINGS=PATH: another file, for test runs)
void load_settings();
void save_settings();          // written safely: a crash while saving keeps the old file
void apply_graphics();         // the renderer, filters and aspect for the current settings
std::string msaa_label();

// ---------------------------------------------------------------- input.cpp

// Every connected controller can drive player 1 (Apocalypse is single-player). Devices SDL
// has a mapping for are opened as gamepads; anything else (e.g. some 8BitDo modes) as a raw
// joystick with a generic layout.
struct InputDevice {
    SDL_JoystickID id;
    SDL_Gamepad* pad = nullptr;
    SDL_Joystick* joy = nullptr;
};
// A custom button layout (settings screen -> Set up buttons), one per controller model (GUID).
struct Bind {
    enum Kind : uint8_t { None, Button, Hat, Axis } kind = None;
    uint8_t index = 0;
    int8_t value = 0;  // hat direction mask, or axis direction (+1/-1)
};
struct Layout {
    Bind bind[16];  // indexed by PS1 pad bit number
};
extern std::vector<InputDevice> g_devices;
extern std::map<std::string, Layout> g_layouts;
extern SDL_Scancode g_keys[16];  // remapped keyboard: one key per PS1 button (bit number)
extern bool g_key_custom;        // ... in use
// Player 1's input: "" = keyboard and every controller, "keyboard" = keyboard & mouse only, else
// one controller's GUID (unplugged: all, so a missing pad never leaves the game without input).
extern std::string g_input;

struct RemapStep {
    uint16_t bit;
    const char* name;
};
extern const RemapStep kRemapOrder[14];  // the order the settings screen asks for buttons in

bool device_event(const SDL_Event& e);  // a controller connected or disconnected (true: handled)
SDL_Joystick* device_joystick(const InputDevice& d);
std::string device_guid(const InputDevice& d);
const char* device_name(const InputDevice& d);
int bit_index(uint16_t bit);

// ---------------------------------------------------------------- settings_menu.cpp

// The settings screen (Select + Start held). Modal: the game is paused while it is open.
void run_settings_menu();

}  // namespace host
