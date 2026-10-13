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

// Player 1 is played with the keyboard and mouse or with a controller (Apocalypse is single-player):
// the settings screen picks which, and each game action can be put on up to two inputs of each.
// Devices SDL has a mapping for are opened as gamepads; anything else (e.g. some 8BitDo modes) as
// a raw joystick read through a generic layout, so the same controller bindings work for it.
struct InputDevice {
    SDL_JoystickID id;
    SDL_Gamepad* pad = nullptr;
    SDL_Joystick* joy = nullptr;
};
extern std::vector<InputDevice> g_devices;

enum InputMode { M_KEYBOARD, M_CONTROLLER, M_COUNT };
// The game's actions. Shoulder actions go to whichever shoulder button the game's own controller
// config gives them; the rest to a fixed PS1 button.
enum Action : uint8_t {
    A_UP, A_DOWN, A_LEFT, A_RIGHT, A_AIM, A_FIRE_UP, A_FIRE_DOWN, A_FIRE_LEFT, A_FIRE_RIGHT,
    A_JUMP, A_CROUCH, A_WEAPON, A_BOMB, A_PAUSE, A_SELECT, A_COUNT
};
// One input: a key (SDL scancode), mouse button (1-5), the mouse wheel (either way), a gamepad
// button (SDL_GamepadButton), a gamepad axis pushed one way (SDL_GamepadAxis, dir +1/-1), or a stick
// (0 left, 1 right: aiming).
struct Input {
    enum Kind : uint8_t { None, Key, Mouse, Wheel, Button, Axis, Stick } kind = None;
    int16_t code = 0;
    int8_t dir = 0;
    bool operator==(const Input& o) const { return kind == o.kind && code == o.code && dir == o.dir; }
};
constexpr int kSlots = 2;
extern Input g_binds[M_COUNT][A_COUNT][kSlots];
extern int g_input_mode;  // the chosen InputMode, -1 = not chosen (a controller when one is connected)

int input_mode();                       // the mode in use (a chosen controller that is unplugged: keyboard)
std::string input_name(const Input& i); // "Space", "Mouse right", "RB", "Left stick up"...
const char* action_label(int a);       // "Jump"
const char* action_shoulder(int a);    // "R1" for a shoulder action (where the game's config puts it), else ""
void input_mute();                     // the game sees no input until everything is let go
void binds_default(int mode);
std::string input_token(const Input& i);              // settings.ini form
bool input_from_token(const std::string& t, Input* out);
const char* action_key(int a);                        // settings.ini name of an action
void input_event(const SDL_Event& e);  // every event (the mouse wheel)
// The input of `mode` this event presses (true = *out filled).
bool input_capture(int mode, const SDL_Event& e, Input* out);

bool device_event(const SDL_Event& e);  // a controller connected or disconnected (true: handled)
const char* device_name(const InputDevice& d);

// ---------------------------------------------------------------- settings_menu.cpp

// The settings screen (Select + Start held). Modal: the game is paused while it is open.
void run_settings_menu();
// The controls screen (game's Options -> Controls). Modal too.
void run_controls_menu();
bool controls_requested();  // asked for (controls_open) since the last call

}  // namespace host
