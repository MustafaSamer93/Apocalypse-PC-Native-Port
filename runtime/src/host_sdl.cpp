// Host platform layer: SDL3 window, audio, input and frame pacing.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "psx.h"

namespace {

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
int g_tex_w = 0, g_tex_h = 0;
SDL_AudioStream* g_audio = nullptr;

// Every connected controller drives player 1 (Apocalypse is single-player). Devices SDL
// has a mapping for are opened as gamepads; anything else (e.g. some 8BitDo modes) as a
// raw joystick with a generic layout.
struct InputDevice {
    SDL_JoystickID id;
    SDL_Gamepad* pad = nullptr;
    SDL_Joystick* joy = nullptr;
};
std::vector<InputDevice> g_devices;
bool g_quit = false;
bool g_turbo = false;
bool g_force_turbo = false;
bool g_vsync_pacing = false;   // display is ~60 Hz: RenderPresent paces the frames
bool g_vsync_active = false;   // current renderer vsync state (off while fast-forwarding)
uint64_t g_next_frame_ns = 0;
std::vector<uint32_t> g_pixels;

constexpr uint64_t FRAME_NS = 1000000000ull * 1001 / 60000;  // 59.94 Hz

// Scripted input for automated testing: button held on port 0 for a frame range.
struct ScriptedPress {
    uint16_t bit;
    uint64_t from, to;
};
std::vector<ScriptedPress> g_script;
uint64_t g_frame = 0;

// PS1 pad bits (active low in the wire format; here 1 = pressed)
enum : uint16_t {
    B_SELECT = 1 << 0, B_L3 = 1 << 1, B_R3 = 1 << 2, B_START = 1 << 3,
    B_UP = 1 << 4, B_RIGHT = 1 << 5, B_DOWN = 1 << 6, B_LEFT = 1 << 7,
    B_L2 = 1 << 8, B_R2 = 1 << 9, B_L1 = 1 << 10, B_R1 = 1 << 11,
    B_TRIANGLE = 1 << 12, B_CIRCLE = 1 << 13, B_CROSS = 1 << 14, B_SQUARE = 1 << 15,
};

void ensure_texture(int w, int h) {
    if (g_texture && w == g_tex_w && h == g_tex_h) return;
    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_NEAREST);
    g_tex_w = w;
    g_tex_h = h;
    g_pixels.assign((size_t)w * h, 0);
}

// Draws the most recent game frame (without presenting it).
void draw_game_frame() {
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    if (!g_texture) return;
    SDL_FRect dst{0, 0, 640, 480};  // stretch to 4:3 regardless of PS1 video mode
    SDL_RenderTexture(g_renderer, g_texture, nullptr, &dst);
}

// ---------------------------------------------------------------- custom button layouts
// Set up in the in-game settings screen (F1), one layout per controller model (SDL GUID),
// saved automatically to settings.ini.

struct Bind {
    enum Kind : uint8_t { None, Button, Hat, Axis } kind = None;
    uint8_t index = 0;
    int8_t value = 0;  // hat direction mask, or axis direction (+1/-1)
};
struct Layout {
    Bind bind[16];  // indexed by PS1 pad bit number
};
std::map<std::string, Layout> g_layouts;
const char* kSettingsFile = "settings.ini";
// Remapped keyboard: one key per PS1 button (bit number), used when g_key_custom.
SDL_Scancode g_keys[16] = {};
bool g_key_custom = false;

// Order the settings screen asks for buttons in.
const struct { uint16_t bit; const char* name; } kRemapOrder[] = {
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

void save_settings() {
    FILE* f = std::fopen(kSettingsFile, "w");
    if (!f) return;
    std::fprintf(f, "# Written by the game's settings screen (F1). Safe to delete to reset.\n");
    if (g_key_custom) {
        std::fprintf(f, "keys");
        for (int i = 0; i < 16; i++)
            if (g_keys[i] != SDL_SCANCODE_UNKNOWN) std::fprintf(f, " %d=%d", i, (int)g_keys[i]);
        std::fprintf(f, "\n");
    }
    for (auto& [guid, lay] : g_layouts) {
        std::fprintf(f, "pad %s", guid.c_str());
        for (int i = 0; i < 16; i++) {
            const Bind& b = lay.bind[i];
            if (b.kind == Bind::Button) std::fprintf(f, " %d=b%d", i, b.index);
            if (b.kind == Bind::Hat) std::fprintf(f, " %d=h%d:%d", i, b.index, b.value);
            if (b.kind == Bind::Axis) std::fprintf(f, " %d=a%d%c", i, b.index, b.value > 0 ? '+' : '-');
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

void load_settings() {
    FILE* f = std::fopen(kSettingsFile, "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "keys", 4) == 0) {
            g_key_custom = true;
            const char* p = line + 4;
            int bit, sc, used;
            while (std::sscanf(p, " %d=%d%n", &bit, &sc, &used) == 2) {
                if (bit >= 0 && bit < 16) g_keys[bit] = (SDL_Scancode)sc;
                p += used;
            }
            continue;
        }
        char guid[64];
        int n = 0;
        if (std::sscanf(line, "pad %63s%n", guid, &n) != 1) continue;
        Layout lay;
        const char* p = line + n;
        int bit, idx, val, used;
        char kind, sign;
        while (true) {
            while (*p == ' ') p++;
            if (std::sscanf(p, "%d=%c%n", &bit, &kind, &used) != 2 || bit < 0 || bit > 15) break;
            p += used;
            Bind b;
            if (kind == 'b' && std::sscanf(p, "%d%n", &idx, &used) == 1) b = {Bind::Button, (uint8_t)idx, 0};
            else if (kind == 'h' && std::sscanf(p, "%d:%d%n", &idx, &val, &used) == 2) b = {Bind::Hat, (uint8_t)idx, (int8_t)val};
            else if (kind == 'a' && std::sscanf(p, "%d%c%n", &idx, &sign, &used) == 2) b = {Bind::Axis, (uint8_t)idx, (int8_t)(sign == '+' ? 1 : -1)};
            else break;
            p += used;
            lay.bind[bit] = b;
        }
        g_layouts[guid] = lay;
    }
    std::fclose(f);
}

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

}  // namespace

bool host_init(const char* title, int scale) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        log_msg(LogCat::Host, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    // Community mappings (https://github.com/mdqinc/SDL_GameControllerDB), if the user adds the file.
    if (SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt") > 0)
        log_msg(LogCat::Host, "loaded extra controller mappings from gamecontrollerdb.txt");
    load_settings();
    g_window = SDL_CreateWindow(title, 320 * scale, 240 * scale, SDL_WINDOW_RESIZABLE);
    if (!g_window) return false;
    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (!g_renderer) return false;
    SDL_SetRenderLogicalPresentation(g_renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    // On a ~60 Hz display, pace the emulation by vsync: one emulated frame per refresh
    // gives perfectly even frame times (the game's 30 fps then shows each frame for
    // exactly two refreshes). Other refresh rates fall back to a timer.
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window))) {
        g_vsync_pacing = mode->refresh_rate > 58.5f && mode->refresh_rate < 61.5f;
        log_msg(LogCat::Host, "display refresh %.2f Hz: %s", mode->refresh_rate,
                g_vsync_pacing ? "pacing by vsync" : "pacing by timer");
    }
    SDL_SetRenderVSync(g_renderer, g_vsync_pacing ? 1 : 0);
    g_vsync_active = g_vsync_pacing;

    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    else log_msg(LogCat::Host, "no audio device: %s", SDL_GetError());
    g_next_frame_ns = SDL_GetTicksNS();
    return true;
}

void host_shutdown() {
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

static void run_settings_menu();

void host_poll() {
    bool open_menu = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) g_quit = true;
                if (e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat) open_menu = true;
                if (e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
                    bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(g_window, !fs);
                }
                break;
            // JOYSTICK_ADDED fires for every device (gamepads included); add_device
            // decides between the gamepad and raw-joystick paths and ignores repeats.
            case SDL_EVENT_JOYSTICK_ADDED: add_device(e.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: add_device(e.gdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: remove_device(e.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: remove_device(e.gdevice.which); break;
        }
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    g_turbo = g_force_turbo || keys[SDL_SCANCODE_TAB];

    // Holding SELECT + START for ~1.5 s opens the settings from a controller.
    static int combo_frames = 0;
    uint16_t held = (uint16_t)~host_pad(0).buttons;
    combo_frames = ((held & B_SELECT) && (held & B_START)) ? combo_frames + 1 : 0;
    if (combo_frames == 90) open_menu = true;
    if (open_menu && g_renderer) run_settings_menu();
}

void host_set_turbo(bool on) { g_force_turbo = on; }

bool host_quit_requested() { return g_quit; }

// "--press START@1200+10,CROSS@1500" : button@frame[+duration frames, default 6]
bool host_parse_press_script(const char* spec) {
    static const struct { const char* name; uint16_t bit; } names[] = {
        {"SELECT", B_SELECT}, {"START", B_START}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
        {"LEFT", B_LEFT}, {"L2", B_L2}, {"R2", B_R2}, {"L1", B_L1}, {"R1", B_R1},
        {"TRIANGLE", B_TRIANGLE}, {"CIRCLE", B_CIRCLE}, {"CROSS", B_CROSS}, {"SQUARE", B_SQUARE},
    };
    std::string s(spec);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        size_t at = item.find('@');
        if (at == std::string::npos) return false;
        std::string name = item.substr(0, at);
        uint64_t from = std::strtoull(item.c_str() + at + 1, nullptr, 10), len = 6;
        if (size_t plus = item.find('+'); plus != std::string::npos) len = std::strtoull(item.c_str() + plus + 1, nullptr, 10);
        uint16_t bit = 0;
        for (auto& n : names)
            if (name == n.name) bit = n.bit;
        if (!bit) return false;
        g_script.push_back({bit, from, from + len});
    }
    return true;
}

uint64_t host_frame_count() { return g_frame; }

namespace {

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
    if (!g_renderer || !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) return 0;
    float wx, wy;
    SDL_MouseButtonFlags b = SDL_GetMouseState(&wx, &wy);
    uint16_t pressed = 0;
    if (b & SDL_BUTTON_RMASK) pressed |= B_R1;
    if (b & SDL_BUTTON_LMASK) {
        float x, y;
        SDL_RenderCoordinatesFromWindow(g_renderer, wx, wy, &x, &y);  // 640x480 logical space
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

}  // namespace

PadState host_pad(int port, bool analog) {
    uint16_t pressed = 0;
    PadState s;
    if (port == 0) {
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
    for (auto& dev : g_devices) {
        if (port != 0) break;
        if (auto it = g_layouts.find(device_guid(dev)); it != g_layouts.end()) {
            SDL_Joystick* j = device_joystick(dev);
            for (int i = 0; i < 16; i++)
                if (bind_active(j, it->second.bind[i])) pressed |= (uint16_t)(1u << i);
            if (dev.pad) pressed |= gamepad_sticks(dev.pad, analog, s);
            continue;
        }
        if (SDL_Joystick* j = dev.joy) {
            // Generic DirectInput-style layout: hat = d-pad, left stick = d-pad,
            // buttons 0-3 = face buttons, 4-7 = shoulders, 8/9 = select/start.
            static const uint16_t btn_map[] = {B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_L1, B_R1,
                                               B_L2, B_R2, B_SELECT, B_START, B_L3, B_R3};
            int nb = SDL_GetNumJoystickButtons(j);
            for (int b = 0; b < nb && b < (int)(sizeof btn_map / sizeof *btn_map); b++)
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
            continue;
        }
        SDL_Gamepad* p = dev.pad;
        struct { SDL_GamepadButton b; uint16_t bit; } map[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT},
            {SDL_GAMEPAD_BUTTON_SOUTH, B_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, B_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST, B_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, B_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R1},
            {SDL_GAMEPAD_BUTTON_START, B_START}, {SDL_GAMEPAD_BUTTON_BACK, B_SELECT},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, B_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, B_R3},
        };
        for (auto& m : map)
            if (SDL_GetGamepadButton(p, m.b)) pressed |= m.bit;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) pressed |= B_L2;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) pressed |= B_R2;
        pressed |= gamepad_sticks(p, analog, s);
    }
    if (port == 0)
        for (auto& sp : g_script)
            if (g_frame >= sp.from && g_frame < sp.to) pressed |= sp.bit;
    s.buttons = (uint16_t)~pressed;
    s.connected = port == 0;
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

// --dump-vram FRAME PATH : writes all of VRAM (1024x512, 15-bit -> 24-bit) as a BMP.
uint64_t g_dump_vram_frame = 0;
std::string g_dump_vram_path;

static void write_vram_bmp(const uint16_t* vram, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const int w = 1024, h = 512, row = w * 3;
    uint32_t file_size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; i++) hdr[off + i] = (uint8_t)(v >> (8 * i)); };
    put32(2, file_size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    hdr[26] = 1; hdr[28] = 24; put32(34, row * h);
    std::fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t p = vram[y * 1024 + x];
            line[x * 3 + 0] = (uint8_t)(((p >> 10) & 31) << 3);
            line[x * 3 + 1] = (uint8_t)(((p >> 5) & 31) << 3);
            line[x * 3 + 2] = (uint8_t)((p & 31) << 3);
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}

void host_present(const GpuDisplay& d) {
    g_frame++;
    if (g_dump_vram_frame && g_frame == g_dump_vram_frame) {
        write_vram_bmp(d.vram, g_dump_vram_path);
        log_msg(LogCat::Host, "VRAM dumped to %s (display at %d,%d %dx%d%s)", g_dump_vram_path.c_str(), d.x, d.y,
                d.w, d.h, d.rgb24 ? " 24-bit" : "");
    }
    if (!g_renderer) return;
    static uint64_t fps_t0 = SDL_GetTicksNS(), fps_frames = 0, last_present = 0, dt_min = ~0ull, dt_max = 0;
    uint64_t t_now = SDL_GetTicksNS();
    if (last_present) {
        dt_min = std::min(dt_min, t_now - last_present);
        dt_max = std::max(dt_max, t_now - last_present);
    }
    last_present = t_now;
    if (++fps_frames == 60) {
        uint64_t now = SDL_GetTicksNS();
        double fps = 60.0 * 1e9 / (double)(now - fps_t0);
        LOG(Host, "frame times: min %.2f ms, max %.2f ms", dt_min / 1e6, dt_max / 1e6);
        dt_min = ~0ull;
        dt_max = 0;
        char title[96];
        std::snprintf(title, sizeof title, "Apocalypse - %.0f fps%s", fps, g_turbo ? " (fast-forward)" : "");
        SDL_SetWindowTitle(g_window, title);
        LOG(Host, "frame %llu: %.1f fps", (unsigned long long)g_frame, fps);
        fps_t0 = now;
        fps_frames = 0;
    }
    int w = std::clamp(d.w, 1, 1024), h = std::clamp(d.h, 1, 512);
    ensure_texture(w, h);
    if (d.enabled) {
        for (int y = 0; y < h; y++) {
            uint32_t* row = &g_pixels[(size_t)y * w];
            int vy = (d.y + y) & 511;
            if (d.rgb24) {
                const uint8_t* src = (const uint8_t*)&d.vram[vy * 1024];
                for (int x = 0; x < w; x++) {
                    int bx = (d.x * 2 + x * 3) % 2048;
                    uint8_t r = src[bx], g = src[(bx + 1) % 2048], b = src[(bx + 2) % 2048];
                    row[x] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
                }
            } else {
                for (int x = 0; x < w; x++) {
                    uint16_t p = d.vram[vy * 1024 + ((d.x + x) & 1023)];
                    uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                    row[x] = (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5);
                }
            }
        }
    } else {
        std::fill(g_pixels.begin(), g_pixels.end(), 0);
    }
    SDL_UpdateTexture(g_texture, nullptr, g_pixels.data(), w * 4);
    draw_game_frame();
    SDL_RenderPresent(g_renderer);
}

// ================================================================ settings screen (F1)

namespace {

// Snapshot of a joystick's inputs, used to detect "the next thing the user pressed".
struct JoySnapshot {
    std::vector<bool> buttons;
    std::vector<Uint8> hats;
    std::vector<Sint16> axes;
};

JoySnapshot snapshot(SDL_Joystick* j) {
    JoySnapshot s;
    for (int i = 0; i < SDL_GetNumJoystickButtons(j); i++) s.buttons.push_back(SDL_GetJoystickButton(j, i));
    for (int i = 0; i < SDL_GetNumJoystickHats(j); i++) s.hats.push_back(SDL_GetJoystickHat(j, i));
    for (int i = 0; i < SDL_GetNumJoystickAxes(j); i++) s.axes.push_back(SDL_GetJoystickAxis(j, i));
    return s;
}

// Returns true and fills `out` when something changed from `base` to a new press.
bool detect_press(SDL_Joystick* j, const JoySnapshot& base, Bind* out) {
    for (int i = 0; i < (int)base.buttons.size(); i++)
        if (!base.buttons[i] && SDL_GetJoystickButton(j, i)) { *out = {Bind::Button, (uint8_t)i, 0}; return true; }
    for (int i = 0; i < (int)base.hats.size(); i++) {
        Uint8 h = SDL_GetJoystickHat(j, i);
        for (Uint8 dir : {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT})
            if ((h & dir) && !(base.hats[i] & dir)) { *out = {Bind::Hat, (uint8_t)i, (int8_t)dir}; return true; }
    }
    for (int i = 0; i < (int)base.axes.size(); i++) {
        int v = SDL_GetJoystickAxis(j, i);
        if (std::abs(v - base.axes[i]) > 20000 && std::abs(v) > 16000) {
            *out = {Bind::Axis, (uint8_t)i, (int8_t)(v > 0 ? 1 : -1)};
            return true;
        }
    }
    return false;
}

bool any_input_held(SDL_Joystick* j) {
    for (int i = 0; i < SDL_GetNumJoystickButtons(j); i++)
        if (SDL_GetJoystickButton(j, i)) return true;
    for (int i = 0; i < SDL_GetNumJoystickHats(j); i++)
        if (SDL_GetJoystickHat(j, i) != SDL_HAT_CENTERED) return true;
    return false;
}

void draw_panel(const std::vector<std::string>& lines, int highlight) {
    draw_game_frame();
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 200);
    SDL_FRect bg{0, 0, 640, 480};
    SDL_RenderFillRect(g_renderer, &bg);
    SDL_SetRenderScale(g_renderer, 2.0f, 2.0f);  // 8x8 debug font -> 16x16; 40x30 character grid
    for (int i = 0; i < (int)lines.size(); i++) {
        float y = 24.0f + 12.0f * (float)i;
        if (i == highlight) {
            SDL_SetRenderDrawColor(g_renderer, 200, 60, 20, 255);
            SDL_FRect r{12, y - 2, 296, 12};
            SDL_RenderFillRect(g_renderer, &r);
        }
        SDL_SetRenderDrawColor(g_renderer, 255, 230, 180, 255);
        SDL_RenderDebugText(g_renderer, 16, y, lines[i].c_str());
    }
    SDL_SetRenderScale(g_renderer, 1.0f, 1.0f);
    SDL_RenderPresent(g_renderer);
}

enum class MenuResult { Continue, Close, Quit };

// Pumps events for the settings screen; returns key presses as SDL scancodes.
MenuResult pump_menu_events(std::vector<SDL_Scancode>& keys) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: return MenuResult::Quit;
            case SDL_EVENT_KEY_DOWN: keys.push_back(e.key.scancode); break;
            case SDL_EVENT_JOYSTICK_ADDED: add_device(e.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: add_device(e.gdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: remove_device(e.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: remove_device(e.gdevice.which); break;
        }
    }
    return MenuResult::Continue;
}

// Walks the user through pressing each PS1 button on `dev`. Returns false if cancelled.
bool remap_device(const InputDevice& dev, Layout* out, bool* quit) {
    SDL_Joystick* j = device_joystick(dev);
    Layout lay;
    const int total = (int)(sizeof kRemapOrder / sizeof *kRemapOrder);
    for (int step = 0; step < total; step++) {
        // wait until everything is released so one press is not taken twice
        while (any_input_held(j)) {
            std::vector<SDL_Scancode> keys;
            if (pump_menu_events(keys) == MenuResult::Quit) { *quit = true; return false; }
            SDL_Delay(10);
        }
        JoySnapshot base = snapshot(j);
        for (;;) {
            std::vector<SDL_Scancode> keys;
            if (pump_menu_events(keys) == MenuResult::Quit) { *quit = true; return false; }
            bool skip = false;
            for (auto k : keys) {
                if (k == SDL_SCANCODE_ESCAPE) return false;
                if (k == SDL_SCANCODE_BACKSPACE || k == SDL_SCANCODE_DELETE) skip = true;
            }
            if (skip) break;
            Bind b;
            if (detect_press(j, base, &b)) {
                lay.bind[bit_index(kRemapOrder[step].bit)] = b;
                break;
            }
            char progress[48];
            std::snprintf(progress, sizeof progress, "Step %d of %d", step + 1, total);
            draw_panel({"CONTROLLER SETUP", std::string(device_name(dev)).substr(0, 36), "", progress, "",
                        "Press the button for:", "", std::string("   ") + kRemapOrder[step].name, "", "",
                        "Backspace: skip this button", "Esc: cancel"},
                       -1);
            SDL_Delay(10);
        }
    }
    *out = lay;
    return true;
}

// Keyboard version of remap_device: the next key pressed is taken for each button.
// Delete skips a button (keeps it unbound), Esc cancels.
bool remap_keyboard(SDL_Scancode* out, bool* quit) {
    SDL_Scancode keys_out[16] = {};
    const int total = (int)(sizeof kRemapOrder / sizeof *kRemapOrder);
    for (int step = 0; step < total; step++) {
        for (bool got = false; !got;) {
            std::vector<SDL_Scancode> keys;
            if (pump_menu_events(keys) == MenuResult::Quit) { *quit = true; return false; }
            for (auto k : keys) {
                if (k == SDL_SCANCODE_ESCAPE) return false;
                if (k == SDL_SCANCODE_DELETE) { got = true; break; }
                if (k == SDL_SCANCODE_F1) continue;
                keys_out[bit_index(kRemapOrder[step].bit)] = k;
                got = true;
                break;
            }
            if (got) break;
            char progress[48];
            std::snprintf(progress, sizeof progress, "Step %d of %d", step + 1, total);
            draw_panel({"KEYBOARD SETUP", "", progress, "", "Press the key for:", "",
                        std::string("   ") + kRemapOrder[step].name, "", "", "Delete: leave unbound",
                        "Esc: cancel"},
                       -1);
            SDL_Delay(10);
        }
    }
    std::memcpy(out, keys_out, sizeof keys_out);
    return true;
}

}  // namespace

// Modal settings screen. The game is paused while it is open (this runs inside the
// emulated VBlank), and audio is paused with it.
static void run_settings_menu() {
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    size_t dev_index = 0;
    int sel = 0;
    std::string status;
    // buttons held when the menu opens (e.g. SELECT+START) must not count as presses
    uint16_t prev_pad = host_pad(0).buttons;
    bool quit = false;
    for (bool open = true; open && !quit;) {
        std::vector<SDL_Scancode> keys;
        if (pump_menu_events(keys) == MenuResult::Quit) { quit = true; break; }
        // controller navigation (edge-triggered) using the current layout
        uint16_t pad = host_pad(0).buttons, pressed = (uint16_t)(prev_pad & ~pad);
        prev_pad = pad;
        if (pressed & B_UP) keys.push_back(SDL_SCANCODE_UP);
        if (pressed & B_DOWN) keys.push_back(SDL_SCANCODE_DOWN);
        if (pressed & B_LEFT) keys.push_back(SDL_SCANCODE_LEFT);
        if (pressed & B_RIGHT) keys.push_back(SDL_SCANCODE_RIGHT);
        if (pressed & (B_CROSS | B_START)) keys.push_back(SDL_SCANCODE_RETURN);
        if (pressed & B_CIRCLE) keys.push_back(SDL_SCANCODE_ESCAPE);

        // entry 0 is the keyboard, entries 1.. are the connected controllers
        const size_t entries = 1 + g_devices.size();
        if (dev_index >= entries) dev_index = 0;
        const bool is_keyboard = dev_index == 0;
        const int items = 3;  // remap, reset, back
        for (auto k : keys) {
            switch (k) {
                case SDL_SCANCODE_UP: sel = (sel + items - 1) % items; break;
                case SDL_SCANCODE_DOWN: sel = (sel + 1) % items; break;
                case SDL_SCANCODE_LEFT: dev_index = (dev_index + entries - 1) % entries; break;
                case SDL_SCANCODE_RIGHT: dev_index = (dev_index + 1) % entries; break;
                case SDL_SCANCODE_ESCAPE:
                case SDL_SCANCODE_F1: open = false; break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                    if (sel == 2) { open = false; break; }
                    if (sel == 0 && is_keyboard) {
                        SDL_Scancode keys_new[16];
                        if (remap_keyboard(keys_new, &quit)) {
                            std::memcpy(g_keys, keys_new, sizeof g_keys);
                            g_key_custom = true;
                            save_settings();
                            status = "Saved. This layout is used every time.";
                        } else if (!quit) {
                            status = "Cancelled, layout unchanged.";
                        }
                    } else if (sel == 0) {
                        InputDevice dev = g_devices[dev_index - 1];
                        Layout lay;
                        if (remap_device(dev, &lay, &quit)) {
                            g_layouts[device_guid(dev)] = lay;
                            save_settings();
                            status = "Saved. This layout is used every time.";
                        } else if (!quit) {
                            status = "Cancelled, layout unchanged.";
                        }
                    } else if (sel == 1) {
                        if (is_keyboard) g_key_custom = false;
                        else g_layouts.erase(device_guid(g_devices[dev_index - 1]));
                        save_settings();
                        status = "Restored the default layout.";
                    }
                    prev_pad = host_pad(0).buttons;
                    break;
                default: break;
            }
        }
        if (!open || quit) break;
        std::string dev_line = is_keyboard ? std::string("Keyboard & mouse")
                                           : std::string(device_name(g_devices[dev_index - 1])).substr(0, 30);
        dev_line = "< " + dev_line + " >";
        bool custom = is_keyboard ? g_key_custom : g_layouts.count(device_guid(g_devices[dev_index - 1])) > 0;
        draw_panel({"SETTINGS", "", "Device (left/right to change):", dev_line,
                    custom ? "Layout: custom" : "Layout: default", "",
                    "Set up buttons", "Reset to default layout", "Back to game", "", status, "", "",
                    "Keys: arrows, Enter, Esc", "Pad:  d-pad, X select, O back", "Open anytime: F1, or hold",
                    "      SELECT + START"},
                   6 + sel);
        SDL_Delay(15);
    }
    if (quit) g_quit = true;
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    g_next_frame_ns = SDL_GetTicksNS();  // don't try to "catch up" the paused time
}

void host_throttle_frame() {
    bool want_vsync = g_vsync_pacing && !g_turbo;
    if (want_vsync != g_vsync_active) {
        SDL_SetRenderVSync(g_renderer, want_vsync ? 1 : 0);
        g_vsync_active = want_vsync;
    }
    if (g_turbo || g_vsync_active) {  // vsync: SDL_RenderPresent already waited
        g_next_frame_ns = SDL_GetTicksNS();
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    g_next_frame_ns += FRAME_NS;
    if (g_next_frame_ns > now) {
        SDL_DelayPrecise(g_next_frame_ns - now);
    } else if (now - g_next_frame_ns > FRAME_NS * 4) {
        g_next_frame_ns = now;  // fell far behind (debugger, window drag): resync
    }
}

// --wav PATH: also write everything the SPU produces to a 44.1 kHz stereo WAV file.
FILE* g_wav = nullptr;
uint32_t g_wav_bytes = 0;

static void wav_header(FILE* f, uint32_t data_bytes) {
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f); w32(36 + data_bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(2); w32(44100); w32(44100 * 4); w16(4); w16(16);
    std::fwrite("data", 1, 4, f); w32(data_bytes);
}

bool host_open_wav(const char* path) {
    g_wav = std::fopen(path, "wb");
    if (!g_wav) return false;
    wav_header(g_wav, 0);
    return true;
}

void host_close_wav() {
    if (!g_wav) return;
    wav_header(g_wav, g_wav_bytes);
    std::fclose(g_wav);
    g_wav = nullptr;
}

void host_audio_push(const int16_t* stereo, int frames) {
    if (g_wav) {
        std::fwrite(stereo, 4, (size_t)frames, g_wav);
        g_wav_bytes += (uint32_t)frames * 4;
        wav_header(g_wav, g_wav_bytes);  // keep the file valid if the process is killed
        std::fseek(g_wav, 0, SEEK_END);
    }
    if (!g_audio || g_turbo) return;
    // Emulated time now follows the display (vsync), which is never exactly the PS1's
    // 59.94 Hz, so audio is produced slightly faster or slower than it plays. Nudge the
    // playback rate (<=0.5%, inaudible) to hold the queue near 60 ms instead of letting
    // it underrun (clicks) or overflow (dropped chunks).
    const int target = 44100 * 60 / 1000;
    int queued = SDL_GetAudioStreamQueued(g_audio) / 4;
    if (queued > target * 4) return;  // way behind (after a stall): drop rather than lag
    float ratio = 1.0f + 0.005f * std::clamp((float)(queued - target) / (float)target, -1.0f, 1.0f);
    SDL_SetAudioStreamFrequencyRatio(g_audio, ratio);
    SDL_PutAudioStreamData(g_audio, stereo, frames * 4);
}

int host_audio_queued_frames() { return g_audio ? SDL_GetAudioStreamQueued(g_audio) / 4 : 0; }
