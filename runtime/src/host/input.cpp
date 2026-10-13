// Player 1's pad: the keyboard and mouse or a controller, each input bound to one of the game's
// actions (settings screen -> Set up buttons); free aim; scripted presses (--press).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>

#include "core/psx.h"
#include "core/savestate.h"
#include "game/game.h"
#include "host/host.h"
#include "host/host_internal.h"
#include "render/present_common.h"

namespace host {

std::vector<InputDevice> g_devices;
Input g_binds[M_COUNT][A_COUNT][kSlots];
int g_input_mode = -1;

const char* device_name(const InputDevice& d) {
    const char* n = d.pad ? SDL_GetGamepadName(d.pad) : SDL_GetJoystickName(d.joy);
    return n ? n : "Controller";
}

namespace {

void add_device(SDL_JoystickID id) {
    for (auto& d : g_devices)
        if (d.id == id) return;
    InputDevice d{id};
    if (SDL_IsGamepad(id)) {
        d.pad = SDL_OpenGamepad(id);
        if (!d.pad) return;
        log_msg(LogCat::Host, "controller connected: %s", SDL_GetGamepadName(d.pad));
    } else {
        d.joy = SDL_OpenJoystick(id);
        if (!d.joy) return;
        log_msg(LogCat::Host,
                "controller connected: %s (no SDL mapping, using a generic layout; a line for it in "
                "gamecontrollerdb.txt gives exact buttons)",
                SDL_GetJoystickName(d.joy));
    }
    g_devices.push_back(d);
}

void remove_device(SDL_JoystickID id) {
    for (auto it = g_devices.begin(); it != g_devices.end(); ++it) {
        if (it->id != id) continue;
        if (it->pad) SDL_CloseGamepad(it->pad);
        if (it->joy) SDL_CloseJoystick(it->joy);
        g_devices.erase(it);
        return;
    }
}

// ================================================================ actions

struct ActionInfo {
    const char* key;   // settings.ini name
    const char* name;
    uint16_t bit;      // the PS1 button (0: a shoulder action, or aiming)
    int shoulder;      // the game's controller-config action (0 weapon, 1 crouch, 2 jump, 3 bomb), -1 none
};
const ActionInfo kActions[A_COUNT] = {
    {"up", "Move up", B_UP, -1},
    {"down", "Move down", B_DOWN, -1},
    {"left", "Move left", B_LEFT, -1},
    {"right", "Move right", B_RIGHT, -1},
    {"aim", "Aim and fire", 0, -1},
    {"fireup", "Fire up", B_TRIANGLE, -1},
    {"firedown", "Fire down", B_CROSS, -1},
    {"fireleft", "Fire left", B_SQUARE, -1},
    {"fireright", "Fire right", B_CIRCLE, -1},
    {"jump", "Jump", 0, 2},
    {"crouch", "Crouch", 0, 1},
    {"weapon", "Weapon select", 0, 0},
    {"bomb", "Smart bomb", 0, 3},
    {"pause", "Pause", B_START, -1},
    {"select", "Select", B_SELECT, -1},
};

// The shoulder button (game config value: 1 L2, 2 R2, 4 L1, 8 R1) of a shoulder action; before the
// game has set its config up, its defaults.
int shoulder_value(int k) {
    static const int kGameDefaults[4] = {1, 4, 8, 2};  // weapon L2, crouch L1, jump R1, smart bomb R2
    const int v = g_cpu ? game_pad_action(g_cpu, k) : 0;
    return v ? v : kGameDefaults[k];
}
uint16_t action_bit(int a) {
    if (kActions[a].shoulder < 0) return kActions[a].bit;
    switch (shoulder_value(kActions[a].shoulder)) {
        case 1: return B_L2;
        case 2: return B_R2;
        case 4: return B_L1;
        default: return B_R1;
    }
}

// ================================================================ reading inputs

// Raw joysticks (no SDL mapping), a generic DirectInput-style layout: buttons 0-3 face, 4-7
// shoulders (6, 7 = the triggers), 8/9 select/start, 10/11 stick presses; hat 0 = d-pad; axes
// 0-3 = left and right stick.
constexpr int kRawButtons[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                               SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                               -SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_BUTTON_BACK,
                               SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};

bool dev_button(const InputDevice& d, int b) {
    if (d.pad) return SDL_GetGamepadButton(d.pad, (SDL_GamepadButton)b);
    if (b >= SDL_GAMEPAD_BUTTON_DPAD_UP && b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
        static const Uint8 hat[] = {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT};
        return SDL_GetNumJoystickHats(d.joy) > 0 && (SDL_GetJoystickHat(d.joy, 0) & hat[b - SDL_GAMEPAD_BUTTON_DPAD_UP]);
    }
    for (int i = 0; i < (int)std::size(kRawButtons); i++)
        if (kRawButtons[i] == b) return SDL_GetJoystickButton(d.joy, i);
    return false;
}
int dev_axis(const InputDevice& d, int axis) {
    if (d.pad) return SDL_GetGamepadAxis(d.pad, (SDL_GamepadAxis)axis);
    for (int i = 0; i < (int)std::size(kRawButtons); i++)
        if (kRawButtons[i] == -axis && axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return SDL_GetJoystickButton(d.joy, i) ? 32767 : 0;
    return axis < SDL_GetNumJoystickAxes(d.joy) ? SDL_GetJoystickAxis(d.joy, axis) : 0;
}

constexpr int kAxisPress = 16000;     // an axis bound as a button: pressed past half way
constexpr int kAimPress = 20000;      // a stick aims (and fires) past this
bool focused() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS); }

// The mouse wheel: each notch is one press, long enough for the game's 30 Hz update to see it, with a
// gap before the next (a fast spin queues up to three).
constexpr uint64_t kWheelMs = 100, kWheelGapMs = 70;
int g_wheel_queued = 0;
uint64_t g_wheel_until = 0, g_wheel_next = 0;
bool g_wheel_now = false;  // this read's state
bool wheel_step() {
    const uint64_t now = SDL_GetTicks();
    if (now < g_wheel_until) return true;
    if (!g_wheel_queued || now < g_wheel_next) return false;
    g_wheel_queued--;
    g_wheel_until = now + kWheelMs;
    g_wheel_next = g_wheel_until + kWheelGapMs;
    return true;
}

bool stick(int which, float& x, float& y) {
    for (auto& d : g_devices) {
        const int sx = dev_axis(d, which ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX);
        const int sy = dev_axis(d, which ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY);
        if ((int64_t)sx * sx + (int64_t)sy * sy > (int64_t)kAimPress * kAimPress) {
            x = (float)sx, y = (float)sy;
            return true;
        }
    }
    return false;
}

bool input_down(const Input& in) {
    switch (in.kind) {
        case Input::Key: return SDL_GetKeyboardState(nullptr)[in.code];
        case Input::Mouse: return focused() && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(in.code));
        case Input::Wheel: return g_wheel_now;
        case Input::Button:
            for (auto& d : g_devices)
                if (dev_button(d, in.code)) return true;
            return false;
        case Input::Axis:
            for (auto& d : g_devices)
                if (dev_axis(d, in.code) * in.dir > kAxisPress) return true;
            return false;
        case Input::Stick: {
            float x, y;
            return stick(in.code, x, y);
        }
        default: return false;
    }
}

// Where the player is on screen, in the 640x480 units over the game image: the last level frame's
// (motion.cpp), else about where the camera usually keeps it.
void player_origin(float& px, float& py) {
    using pres::g_last;
    px = 320.0f, py = 270.0f;
    float sx, sy;
    if (!g_cpu || host_frame_count() > g_level_render_frame + 2 || !player_screen(&sx, &sy) || !g_last.valid || g_last.h <= 0)
        return;
    const float lines = (float)g_last.native_h, width = (float)g_last.w * lines / (float)g_last.h;
    px = sx / width * 640.0f;
    py = sy / lines * 480.0f;
}

// Aim and fire: the direction to fire in (x right, y down) while its input is held; a stick aims
// where it points, anything else at the mouse pointer, from the player.
bool aim_dir(int mode, float& x, float& y) {
    for (const Input& in : g_binds[mode][A_AIM]) {
        if (in.kind == Input::Stick) {
            if (stick(in.code, x, y)) return true;
            continue;
        }
        if (!input_down(in) || !focused()) continue;
        float wx, wy, cx, cy, px, py;
        SDL_GetMouseState(&wx, &wy);
        present_window_to_game(wx, wy, &cx, &cy);
        player_origin(px, py);
        x = cx - px, y = cy - py;
        if (x * x + y * y > 64.0f) return true;
    }
    return false;
}

// The fire button(s) of a direction (diagonals press two).
uint16_t fire_buttons(float x, float y) {
    const float len = std::sqrt(x * x + y * y);
    uint16_t b = 0;
    if (y < -0.38f * len) b |= B_TRIANGLE;
    if (y > 0.38f * len) b |= B_CROSS;
    if (x < -0.38f * len) b |= B_SQUARE;
    if (x > 0.38f * len) b |= B_CIRCLE;
    return b;
}

// Free aim (psx_hook_aim): the exact direction of the aim input. False when it is not aiming.
bool free_aim(float& x, float& y) { return aim_dir(input_mode(), x, y); }

uint8_t to8(int v) { return (uint8_t)std::clamp((v + 32768) >> 8, 0, 255); }

bool g_mute = false;  // no input until all is let go (a menu just closed: the key that closed it would act at once)

// The game's menus with the mouse (host_menu_mouse): what happened since game_menu.cpp last looked, and
// buttons it presses for a moment (host_menu_press).
bool g_menu_moved = false, g_menu_click = false, g_menu_back = false;
int g_menu_wheel = 0;
uint16_t g_menu_pulse = 0;
uint64_t g_menu_pulse_until = 0;
constexpr uint64_t kPulseMs = 100;
bool game_menu_up() { return g_menu_drawn && host_frame_count() - g_menu_drawn < 4; }

// The buttons named as the controller in use names them.
bool playstation_pad() {
    for (auto& d : g_devices) {
        if (!d.pad) continue;
        const SDL_GamepadType t = SDL_GetGamepadType(d.pad);
        return t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
    }
    return false;
}

// Scripted input (--press): a button held on port 0 for a frame range.
struct ScriptedPress {
    uint16_t bit;
    uint64_t from, to;
};
std::vector<ScriptedPress> g_script;

}  // namespace

// ================================================================ bindings

int input_mode() {
    if (g_input_mode == M_KEYBOARD || g_devices.empty()) return M_KEYBOARD;
    return M_CONTROLLER;
}

const char* action_key(int a) { return kActions[a].key; }

std::string input_name(const Input& in) {
    static const char* kXbox[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB", "D-pad up",
                                  "D-pad down", "D-pad left", "D-pad right", "Misc", "Paddle R1", "Paddle L1",
                                  "Paddle R2", "Paddle L2", "Touchpad"};
    static const char* kPlayStation[] = {"Cross", "Circle", "Square", "Triangle", "Select", "PS", "Start", "L3", "R3",
                                         "L1", "R1", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Mute",
                                         "Paddle R1", "Paddle L1", "Paddle R2", "Paddle L2", "Touchpad"};
    static const char* kMouse[] = {"Mouse", "Mouse left", "Mouse middle", "Mouse right", "Mouse 4", "Mouse 5"};
    const bool ps = playstation_pad();
    switch (in.kind) {
        case Input::Key: {
            const char* n = SDL_GetScancodeName((SDL_Scancode)in.code);
            return n && *n ? n : "Key " + std::to_string(in.code);
        }
        case Input::Mouse: return in.code >= 1 && in.code <= 5 ? kMouse[in.code] : "Mouse " + std::to_string(in.code);
        case Input::Wheel: return "Mouse wheel";
        case Input::Button:
            if (in.code < 0 || in.code >= (int)std::size(kXbox)) return "Button " + std::to_string(in.code);
            return ps ? kPlayStation[in.code] : kXbox[in.code];
        case Input::Axis:
            switch (in.code) {
                case SDL_GAMEPAD_AXIS_LEFTX: return in.dir < 0 ? "L stick left" : "L stick right";
                case SDL_GAMEPAD_AXIS_LEFTY: return in.dir < 0 ? "L stick up" : "L stick down";
                case SDL_GAMEPAD_AXIS_RIGHTX: return in.dir < 0 ? "R stick left" : "R stick right";
                case SDL_GAMEPAD_AXIS_RIGHTY: return in.dir < 0 ? "R stick up" : "R stick down";
                case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return ps ? "L2" : "LT";
                case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return ps ? "R2" : "RT";
                default: return "Axis " + std::to_string(in.code) + (in.dir < 0 ? "-" : "+");
            }
        case Input::Stick: return in.code ? "Right stick" : "Left stick";
        default: return "";
    }
}

const char* action_label(int a) { return a >= 0 && a < A_COUNT ? kActions[a].name : ""; }
const char* action_shoulder(int a) {
    if (a < 0 || a >= A_COUNT || kActions[a].shoulder < 0) return "";
    switch (shoulder_value(kActions[a].shoulder)) {
        case 1: return "L2";
        case 2: return "R2";
        case 4: return "L1";
        default: return "R1";
    }
}
void input_mute() { g_mute = true; }

void binds_default(int mode) {
    for (auto& action : g_binds[mode])
        for (Input& in : action) in = {};
    auto set = [&](int a, Input x, Input y = {}) { g_binds[mode][a][0] = x, g_binds[mode][a][1] = y; };
    auto key = [](SDL_Scancode k) { return Input{Input::Key, (int16_t)k, 0}; };
    auto mouse = [](int b) { return Input{Input::Mouse, (int16_t)b, 0}; };
    auto button = [](SDL_GamepadButton b) { return Input{Input::Button, (int16_t)b, 0}; };
    auto axis = [](SDL_GamepadAxis a, int dir) { return Input{Input::Axis, (int16_t)a, (int8_t)dir}; };
    if (mode == M_KEYBOARD) {
        set(A_UP, key(SDL_SCANCODE_W), key(SDL_SCANCODE_UP));
        set(A_DOWN, key(SDL_SCANCODE_S), key(SDL_SCANCODE_DOWN));
        set(A_LEFT, key(SDL_SCANCODE_A), key(SDL_SCANCODE_LEFT));
        set(A_RIGHT, key(SDL_SCANCODE_D), key(SDL_SCANCODE_RIGHT));
        set(A_AIM, mouse(SDL_BUTTON_LEFT));
        set(A_FIRE_UP, key(SDL_SCANCODE_I));
        set(A_FIRE_DOWN, key(SDL_SCANCODE_K));
        set(A_FIRE_LEFT, key(SDL_SCANCODE_J));
        set(A_FIRE_RIGHT, key(SDL_SCANCODE_L));
        set(A_JUMP, key(SDL_SCANCODE_SPACE), mouse(SDL_BUTTON_RIGHT));
        set(A_CROUCH, key(SDL_SCANCODE_LSHIFT));
        set(A_WEAPON, Input{Input::Wheel, 0, 0}, key(SDL_SCANCODE_Q));
        set(A_BOMB, key(SDL_SCANCODE_F), mouse(SDL_BUTTON_MIDDLE));
        set(A_PAUSE, key(SDL_SCANCODE_RETURN));
        set(A_SELECT, key(SDL_SCANCODE_BACKSPACE));
    } else {  // the PS1 layout, shoulders as the game has them by default
        set(A_UP, button(SDL_GAMEPAD_BUTTON_DPAD_UP), axis(SDL_GAMEPAD_AXIS_LEFTY, -1));
        set(A_DOWN, button(SDL_GAMEPAD_BUTTON_DPAD_DOWN), axis(SDL_GAMEPAD_AXIS_LEFTY, 1));
        set(A_LEFT, button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), axis(SDL_GAMEPAD_AXIS_LEFTX, -1));
        set(A_RIGHT, button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), axis(SDL_GAMEPAD_AXIS_LEFTX, 1));
        set(A_AIM, Input{Input::Stick, 1, 0});
        set(A_FIRE_UP, button(SDL_GAMEPAD_BUTTON_NORTH));
        set(A_FIRE_DOWN, button(SDL_GAMEPAD_BUTTON_SOUTH));
        set(A_FIRE_LEFT, button(SDL_GAMEPAD_BUTTON_WEST));
        set(A_FIRE_RIGHT, button(SDL_GAMEPAD_BUTTON_EAST));
        set(A_JUMP, button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
        set(A_CROUCH, button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
        set(A_WEAPON, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1));
        set(A_BOMB, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1));
        set(A_PAUSE, button(SDL_GAMEPAD_BUTTON_START));
        set(A_SELECT, button(SDL_GAMEPAD_BUTTON_BACK));
    }
}

// settings.ini: k<scancode>, m<mouse button>, w, b<gamepad button>, a<axis>+ / a<axis>-, s<stick>
std::string input_token(const Input& in) {
    switch (in.kind) {
        case Input::Key: return "k" + std::to_string(in.code);
        case Input::Mouse: return "m" + std::to_string(in.code);
        case Input::Wheel: return "w";
        case Input::Button: return "b" + std::to_string(in.code);
        case Input::Axis: return "a" + std::to_string(in.code) + (in.dir < 0 ? "-" : "+");
        case Input::Stick: return "s" + std::to_string(in.code);
        default: return "";
    }
}
bool input_from_token(const std::string& t, Input* out) {
    if (t.empty()) return false;
    const int n = std::atoi(t.c_str() + 1);
    switch (t[0]) {
        case 'k': *out = {Input::Key, (int16_t)n, 0}; return n > 0 && n < SDL_SCANCODE_COUNT;
        case 'm': *out = {Input::Mouse, (int16_t)n, 0}; return n >= 1 && n <= 5;
        case 'w': *out = {Input::Wheel, 0, 0}; return true;
        case 'b': *out = {Input::Button, (int16_t)n, 0}; return n >= 0 && n < SDL_GAMEPAD_BUTTON_COUNT;
        case 'a': *out = {Input::Axis, (int16_t)n, (int8_t)(t.back() == '-' ? -1 : 1)}; return n >= 0 && n < SDL_GAMEPAD_AXIS_COUNT;
        case 's': *out = {Input::Stick, (int16_t)(n ? 1 : 0), 0}; return true;
        default: return false;
    }
}

void input_event(const SDL_Event& e) {
    if (!focused()) return;
    const bool menu = game_menu_up();
    switch (e.type) {
        case SDL_EVENT_MOUSE_WHEEL:
            if (e.wheel.y == 0) break;
            if (menu) g_menu_wheel += e.wheel.y > 0 ? -1 : 1;
            else g_wheel_queued = std::min(g_wheel_queued + 1, 3);
            break;
        case SDL_EVENT_MOUSE_MOTION: g_menu_moved = true; break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (menu && e.button.button == SDL_BUTTON_LEFT) g_menu_click = true;
            if (menu && e.button.button == SDL_BUTTON_RIGHT) g_menu_back = true;
            break;
        default: break;
    }
}

bool input_capture(int mode, const SDL_Event& e, Input* out) {
    if (mode == M_KEYBOARD) {
        switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat || e.key.scancode == SDL_SCANCODE_ESCAPE) return false;
                *out = {Input::Key, (int16_t)e.key.scancode, 0};
                return true;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: *out = {Input::Mouse, (int16_t)e.button.button, 0}; return e.button.button <= 5;
            case SDL_EVENT_MOUSE_WHEEL: *out = {Input::Wheel, 0, 0}; return e.wheel.y != 0;
            default: return false;
        }
    }
    switch (e.type) {
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: *out = {Input::Button, (int16_t)e.gbutton.button, 0}; return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (std::abs((int)e.gaxis.value) < 24000) return false;
            *out = {Input::Axis, (int16_t)e.gaxis.axis, (int8_t)(e.gaxis.value < 0 ? -1 : 1)};
            return true;
        // raw joysticks (their gamepad events above when SDL knows them), through the generic layout
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN: {
            if (SDL_IsGamepad(e.jbutton.which) || e.jbutton.button >= std::size(kRawButtons)) return false;
            const int b = kRawButtons[e.jbutton.button];
            *out = b >= 0 ? Input{Input::Button, (int16_t)b, 0} : Input{Input::Axis, (int16_t)-b, 1};
            return true;
        }
        case SDL_EVENT_JOYSTICK_HAT_MOTION: {
            if (SDL_IsGamepad(e.jhat.which) || e.jhat.hat != 0) return false;
            const int v = e.jhat.value;
            const int b = v & SDL_HAT_UP ? SDL_GAMEPAD_BUTTON_DPAD_UP : v & SDL_HAT_DOWN ? SDL_GAMEPAD_BUTTON_DPAD_DOWN
                        : v & SDL_HAT_LEFT ? SDL_GAMEPAD_BUTTON_DPAD_LEFT : v & SDL_HAT_RIGHT ? SDL_GAMEPAD_BUTTON_DPAD_RIGHT : -1;
            *out = {Input::Button, (int16_t)b, 0};
            return b >= 0;
        }
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            if (SDL_IsGamepad(e.jaxis.which) || e.jaxis.axis > 3 || std::abs((int)e.jaxis.value) < 24000) return false;
            *out = {Input::Axis, (int16_t)e.jaxis.axis, (int8_t)(e.jaxis.value < 0 ? -1 : 1)};
            return true;
        default: return false;
    }
}

bool g_controls_request = false;  // (controls_open)
bool controls_requested() {
    const bool r = g_controls_request;
    g_controls_request = false;
    return r;
}

bool device_event(const SDL_Event& e) {
    switch (e.type) {
        // JOYSTICK_ADDED fires for every device (gamepads included); add_device decides
        // between the gamepad and raw-joystick paths and ignores repeats.
        case SDL_EVENT_JOYSTICK_ADDED: add_device(e.jdevice.which); return true;
        case SDL_EVENT_GAMEPAD_ADDED: add_device(e.gdevice.which); return true;
        case SDL_EVENT_JOYSTICK_REMOVED: remove_device(e.jdevice.which); return true;
        case SDL_EVENT_GAMEPAD_REMOVED: remove_device(e.gdevice.which); return true;
        default: return false;
    }
}

}  // namespace host

using namespace host;

PadState host_pad(int port, bool analog) {
    PadState s;
    s.connected = port == 0;
    if (port != 0) return s;
    const int mode = input_mode();
    const bool menu_up = game_menu_up();
    g_wheel_now = mode == M_KEYBOARD && wheel_step();
    uint16_t pressed = 0;
    for (int a = 0; a < A_COUNT; a++) {
        if (a == A_AIM) continue;
        for (const Input& in : g_binds[mode][a]) {
            // in analog mode the left stick is the stick, not the d-pad
            if (analog && in.kind == Input::Axis && in.code <= SDL_GAMEPAD_AXIS_LEFTY) continue;
            if (menu_up && (in.kind == Input::Mouse || in.kind == Input::Wheel)) continue;  // (the menu's)
            if (input_down(in)) pressed |= action_bit(a);
        }
    }
    float x, y;
    if (!menu_up && aim_dir(mode, x, y)) pressed |= fire_buttons(x, y);
    if (SDL_GetTicks() < g_menu_pulse_until) pressed |= g_menu_pulse;
    if (g_mute) {  // (input_mute)
        if (!pressed) g_mute = false;
        pressed = 0;
    }
    if (mode == M_CONTROLLER && !g_devices.empty()) {  // the first controller's sticks (analog mode)
        const InputDevice& d = g_devices[0];
        s.has_sticks = d.pad != nullptr;
        s.lx = to8(dev_axis(d, SDL_GAMEPAD_AXIS_LEFTX)), s.ly = to8(dev_axis(d, SDL_GAMEPAD_AXIS_LEFTY));
        s.rx = to8(dev_axis(d, SDL_GAMEPAD_AXIS_RIGHTX)), s.ry = to8(dev_axis(d, SDL_GAMEPAD_AXIS_RIGHTY));
    }
    for (auto& sp : g_script)
        if (g_frame >= sp.from && g_frame < sp.to) pressed |= sp.bit;
    s.buttons = (uint16_t)~pressed;
    return s;
}

void host_rumble(int port, uint8_t small_motor, uint8_t large_motor) {
    if (port != 0 || input_mode() != M_CONTROLLER) return;
    static uint8_t last_small = 0, last_large = 0;
    static uint64_t last_ns = 0;
    uint64_t now = SDL_GetTicksNS();
    // refresh at least every 50 ms so a held rumble keeps going (each call lasts 100 ms)
    if (small_motor == last_small && large_motor == last_large && now - last_ns < 50000000ull) return;
    last_small = small_motor;
    last_large = large_motor;
    last_ns = now;
    Uint16 low = (Uint16)(large_motor * 257), high = (Uint16)(small_motor ? 0xFFFF : 0);
    for (auto& d : g_devices)
        if (d.pad) SDL_RumbleGamepad(d.pad, low, high, (low || high) ? 100 : 0);
}

// "--press START@1200+10,CROSS@1500" : button@frame[+duration frames, default 6]
bool host_parse_press_script(const char* spec) {
    static const struct { const char* name; uint16_t bit; } kNames[] = {
        {"SELECT", B_SELECT}, {"START", B_START}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
        {"LEFT", B_LEFT}, {"L2", B_L2}, {"R2", B_R2}, {"L1", B_L1}, {"R1", B_R1},
        {"TRIANGLE", B_TRIANGLE}, {"CIRCLE", B_CIRCLE}, {"CROSS", B_CROSS}, {"SQUARE", B_SQUARE},
    };
    const std::string s(spec);
    for (size_t pos = 0; pos < s.size();) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t at = item.find('@');
        if (at == std::string::npos) return false;
        uint16_t bit = 0;
        for (auto& n : kNames)
            if (item.compare(0, at, n.name) == 0) bit = n.bit;
        if (!bit) return false;
        uint64_t from = std::strtoull(item.c_str() + at + 1, nullptr, 10), len = 6;
        if (size_t plus = item.find('+'); plus != std::string::npos) len = std::strtoull(item.c_str() + plus + 1, nullptr, 10);
        g_script.push_back({bit, from, from + len});
    }
    return true;
}

// ================================================================ free aim

namespace {
constexpr int kAimDeadband = 16;       // 4096ths of a turn (1.4 degrees): smaller stick changes are noise
constexpr int kStanceHysteresis = 96;  // 8.4 degrees past a stance border before the stance changes
}  // namespace

// The game turns the fire buttons into a vector (+471 y, +472 x, signed bytes, +-127 per
// button) and aims at its atan2 (+476), so the buttons give only 8 directions. When the
// game registered a fire direction, replace it by the exact stick / cursor direction; the
// game's own code then does the rest (upper-body pose within each 45-degree sector, auto
// aim, fire rate).
extern "C" void psx_hook_aim(R3000* c) {
    static int last = -1;  // game angle last written (-1: not firing)
    const uint32_t p = c->r[16];
    float x, y;
    if (!(MEM_R8(c, p + 471) | MEM_R8(c, p + 472)) || !free_aim(x, y)) {
        last = -1;
        return;
    }
    // the game's angle: (1024 - atan2(-y, x)) in 4096ths of a turn, 0 = up the screen
    constexpr double kUnit = 6.283185307179586 / 4096;
    int a = (1024 - (int)std::lround(std::atan2(-y, x) / kUnit)) & 4095;
    if (last >= 0) {
        auto diff = [](int u, int v) { return ((u - v + 2048) & 4095) - 2048; };
        if (std::abs(diff(a, last)) < kAimDeadband) a = last;  // stick noise: keep the aim still
        // The player's stance is one of 8 (45 degrees each, centred on 0, 512, ...: see
        // 80060834); near a border noise flipped it every tick, so stay until clearly past it.
        const int centre = ((last + 256) & 4095) & ~511, d = diff(a, centre);
        if (std::abs(d) > 255 && std::abs(d) <= 255 + kStanceHysteresis) a = (centre + (d > 0 ? 255 : -255)) & 4095;
    }
    last = a;
    const double t = (1024 - a) * kUnit;
    MEM_W8(c, p + 471, (uint8_t)(int8_t)std::lround(-std::sin(t) * 127));
    MEM_W8(c, p + 472, (uint8_t)(int8_t)std::lround(std::cos(t) * 127));
}

// The game's Options -> Controls: the screen opens at the next host_poll (host.cpp).
void controls_open() { g_controls_request = true; }

MenuMouse host_menu_mouse() {
    MenuMouse m;
    m.moved = g_menu_moved, m.click = g_menu_click, m.back = g_menu_back, m.wheel = g_menu_wheel;
    g_menu_moved = g_menu_click = g_menu_back = false, g_menu_wheel = 0;
    float wx, wy, x = 0, y = 0;
    SDL_GetMouseState(&wx, &wy);
    m.on = focused() && present_window_to_game(wx, wy, &x, &y);
    m.y = y / 480.0f;
    return m;
}

void host_menu_press(uint16_t buttons) {
    g_menu_pulse = buttons;
    g_menu_pulse_until = SDL_GetTicks() + kPulseMs;
}
