// Controllers, keyboard and mouse as player 1's pad; free aim; scripted presses (--press).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "core/psx.h"
#include "host/host.h"
#include "host/host_internal.h"

namespace host {

std::vector<InputDevice> g_devices;
std::map<std::string, Layout> g_layouts;
SDL_Scancode g_keys[16] = {};
bool g_key_custom = false;
std::string g_input;

const RemapStep kRemapOrder[14] = {
    {B_UP, "D-PAD UP"}, {B_DOWN, "D-PAD DOWN"}, {B_LEFT, "D-PAD LEFT"}, {B_RIGHT, "D-PAD RIGHT"},
    {B_CROSS, "CROSS (X)"}, {B_CIRCLE, "CIRCLE (O)"}, {B_SQUARE, "SQUARE"}, {B_TRIANGLE, "TRIANGLE"},
    {B_L1, "L1"}, {B_R1, "R1"}, {B_L2, "L2"}, {B_R2, "R2"}, {B_START, "START"}, {B_SELECT, "SELECT"},
};

int bit_index(uint16_t bit) {
    int i = 0;
    while (!(bit & (1u << i))) i++;
    return i;
}

SDL_Joystick* device_joystick(const InputDevice& d) { return d.pad ? SDL_GetGamepadJoystick(d.pad) : d.joy; }

std::string device_guid(const InputDevice& d) {
    char buf[64];
    SDL_GUIDToString(SDL_GetJoystickGUID(device_joystick(d)), buf, sizeof buf);
    return buf;
}

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

bool bind_active(SDL_Joystick* j, const Bind& b) {
    switch (b.kind) {
        case Bind::Button: return SDL_GetJoystickButton(j, b.index);
        case Bind::Hat: return (SDL_GetJoystickHat(j, b.index) & b.value) != 0;
        case Bind::Axis: {
            int v = SDL_GetJoystickAxis(j, b.index);
            return b.value > 0 ? v > 16000 : v < -16000;
        }
        default: return false;
    }
}

// Default keyboard layout (used until the keyboard is remapped in the settings screen).
// WASD/arrows move; I/J/K/L are the shape buttons laid out like the pad (aim/fire).
const struct { SDL_Scancode sc; uint16_t bit; } kDefaultKeys[] = {
    {SDL_SCANCODE_UP, B_UP}, {SDL_SCANCODE_DOWN, B_DOWN}, {SDL_SCANCODE_LEFT, B_LEFT}, {SDL_SCANCODE_RIGHT, B_RIGHT},
    {SDL_SCANCODE_W, B_UP}, {SDL_SCANCODE_S, B_DOWN}, {SDL_SCANCODE_A, B_LEFT}, {SDL_SCANCODE_D, B_RIGHT},
    {SDL_SCANCODE_I, B_TRIANGLE}, {SDL_SCANCODE_L, B_CIRCLE}, {SDL_SCANCODE_K, B_CROSS}, {SDL_SCANCODE_J, B_SQUARE},
    {SDL_SCANCODE_Q, B_L1}, {SDL_SCANCODE_E, B_R1}, {SDL_SCANCODE_Z, B_L2}, {SDL_SCANCODE_C, B_R2},
    {SDL_SCANCODE_RETURN, B_START}, {SDL_SCANCODE_BACKSPACE, B_SELECT},
};

uint8_t to8(int v) { return (uint8_t)std::clamp((v + 32768) >> 8, 0, 255); }

// Sticks of an SDL gamepad. In digital mode the left stick doubles as the d-pad and the
// right stick aims: it presses the shape button(s) in its direction (twin-stick style).
uint16_t gamepad_sticks(SDL_Gamepad* p, bool analog, PadState& s) {
    uint16_t pressed = 0;
    int lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
    int rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY);
    if (!s.has_sticks) {  // first gamepad provides the analog values
        s.has_sticks = true;
        s.lx = to8(lx); s.ly = to8(ly); s.rx = to8(rx); s.ry = to8(ry);
    }
    if (analog) return 0;
    if (lx < -16000) pressed |= B_LEFT;
    if (lx > 16000) pressed |= B_RIGHT;
    if (ly < -16000) pressed |= B_UP;
    if (ly > 16000) pressed |= B_DOWN;
    if ((int64_t)rx * rx + (int64_t)ry * ry > 20000LL * 20000) {
        if (ry < -12000) pressed |= B_TRIANGLE;
        if (ry > 12000) pressed |= B_CROSS;
        if (rx < -12000) pressed |= B_SQUARE;
        if (rx > 12000) pressed |= B_CIRCLE;
    }
    return pressed;
}

// Mouse: holding the left button fires toward the cursor (the shape button(s) in the
// direction from the screen centre, where the player usually is); right button = R1.
uint16_t mouse_buttons() {
    if (!g_window || !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) return 0;
    float wx, wy;
    SDL_MouseButtonFlags b = SDL_GetMouseState(&wx, &wy);
    uint16_t pressed = 0;
    if (b & SDL_BUTTON_RMASK) pressed |= B_R1;
    if (b & SDL_BUTTON_LMASK) {
        float x, y;
        present_window_to_game(wx, wy, &x, &y);  // 640x480 space over the game image
        float dx = x - 320.0f, dy = y - 270.0f;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 8.0f) {
            if (dy < -0.38f * len) pressed |= B_TRIANGLE;
            if (dy > 0.38f * len) pressed |= B_CROSS;
            if (dx < -0.38f * len) pressed |= B_SQUARE;
            if (dx > 0.38f * len) pressed |= B_CIRCLE;
        }
    }
    return pressed;
}

// Free aim: the exact direction of the right stick, or of the cursor while the left mouse
// button is held (x right, y down). False when neither is aiming.
bool free_aim(float& x, float& y) {
    for (auto& dev : g_devices) {
        if (!dev.pad) continue;
        int rx = SDL_GetGamepadAxis(dev.pad, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(dev.pad, SDL_GAMEPAD_AXIS_RIGHTY);
        if ((int64_t)rx * rx + (int64_t)ry * ry > 20000LL * 20000) {  // same threshold as gamepad_sticks
            x = (float)rx; y = (float)ry;
            return true;
        }
    }
    if (!g_window || !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) return false;
    float wx, wy;
    if (!(SDL_GetMouseState(&wx, &wy) & SDL_BUTTON_LMASK)) return false;
    present_window_to_game(wx, wy, &x, &y);
    x -= 320.0f; y -= 270.0f;  // ponytail: assumes the player is near screen centre, like mouse_buttons()
    return x * x + y * y > 64.0f;
}

// Whether every device drives player 1 (the "All" choice, or the chosen controller is unplugged).
bool input_all() {
    if (g_input.empty()) return true;
    if (g_input == "keyboard") return false;
    for (auto& d : g_devices)
        if (device_guid(d) == g_input) return false;
    return true;
}

// The pad bits of one device.
uint16_t device_buttons(const InputDevice& dev, bool analog, PadState& s) {
    uint16_t pressed = 0;
    if (auto it = g_layouts.find(device_guid(dev)); it != g_layouts.end()) {  // a custom layout
        SDL_Joystick* j = device_joystick(dev);
        for (int i = 0; i < 16; i++)
            if (bind_active(j, it->second.bind[i])) pressed |= (uint16_t)(1u << i);
        if (dev.pad) pressed |= gamepad_sticks(dev.pad, analog, s);
        return pressed;
    }
    if (SDL_Joystick* j = dev.joy) {
        // Generic DirectInput-style layout: hat = d-pad, left stick = d-pad,
        // buttons 0-3 = face buttons, 4-7 = shoulders, 8/9 = select/start.
        static const uint16_t btn_map[] = {B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_L1, B_R1,
                                           B_L2, B_R2, B_SELECT, B_START, B_L3, B_R3};
        int nb = SDL_GetNumJoystickButtons(j);
        for (int b = 0; b < nb && b < (int)std::size(btn_map); b++)
            if (SDL_GetJoystickButton(j, b)) pressed |= btn_map[b];
        if (SDL_GetNumJoystickHats(j) > 0) {
            Uint8 h = SDL_GetJoystickHat(j, 0);
            if (h & SDL_HAT_UP) pressed |= B_UP;
            if (h & SDL_HAT_DOWN) pressed |= B_DOWN;
            if (h & SDL_HAT_LEFT) pressed |= B_LEFT;
            if (h & SDL_HAT_RIGHT) pressed |= B_RIGHT;
        }
        if (SDL_GetNumJoystickAxes(j) >= 2) {
            int x = SDL_GetJoystickAxis(j, 0), y = SDL_GetJoystickAxis(j, 1);
            if (x < -16000) pressed |= B_LEFT;
            if (x > 16000) pressed |= B_RIGHT;
            if (y < -16000) pressed |= B_UP;
            if (y > 16000) pressed |= B_DOWN;
        }
        return pressed;
    }
    static const struct { SDL_GamepadButton b; uint16_t bit; } kMap[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT},
        {SDL_GAMEPAD_BUTTON_SOUTH, B_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, B_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, B_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, B_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R1},
        {SDL_GAMEPAD_BUTTON_START, B_START}, {SDL_GAMEPAD_BUTTON_BACK, B_SELECT},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, B_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, B_R3},
    };
    for (auto& m : kMap)
        if (SDL_GetGamepadButton(dev.pad, m.b)) pressed |= m.bit;
    if (SDL_GetGamepadAxis(dev.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) pressed |= B_L2;
    if (SDL_GetGamepadAxis(dev.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) pressed |= B_R2;
    return pressed | gamepad_sticks(dev.pad, analog, s);
}

// Scripted input (--press): a button held on port 0 for a frame range.
struct ScriptedPress {
    uint16_t bit;
    uint64_t from, to;
};
std::vector<ScriptedPress> g_script;

}  // namespace

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
    uint16_t pressed = 0;
    const bool all = input_all();
    if (all || g_input == "keyboard") {
        const bool* k = SDL_GetKeyboardState(nullptr);
        if (g_key_custom) {
            for (int i = 0; i < 16; i++)
                if (g_keys[i] != SDL_SCANCODE_UNKNOWN && k[g_keys[i]]) pressed |= (uint16_t)(1u << i);
        } else {
            for (auto& m : kDefaultKeys)
                if (k[m.sc]) pressed |= m.bit;
        }
        pressed |= mouse_buttons();
    }
    for (auto& dev : g_devices)
        if (all || device_guid(dev) == g_input) pressed |= device_buttons(dev, analog, s);
    for (auto& sp : g_script)
        if (g_frame >= sp.from && g_frame < sp.to) pressed |= sp.bit;
    s.buttons = (uint16_t)~pressed;
    return s;
}

void host_rumble(int port, uint8_t small_motor, uint8_t large_motor) {
    if (port != 0) return;
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
