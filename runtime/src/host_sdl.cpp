// Host platform layer: SDL3 window, audio, input and frame pacing.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gpu_hw.h"
#include "present.h"
#include "render_backend.h"
#include "psx.h"
#include "savestate.h"
#include "smooth.h"

constexpr const char* kVersion = "v0.1 beta";
int g_force_smooth = 0;  // --smooth K: images per game step regardless of the display
void host_toast(const std::string& text);
int g_renderer_override = -1;  // --renderer opengl|vulkan: 0 / 1, -1 = the saved setting
float g_refresh_override = 0;  // --refresh HZ: assume this display refresh rate

namespace {

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;  // the settings overlay's software renderer while it draws
bool g_gl_ok = false;
SDL_AudioStream* g_audio = nullptr;

// Graphics settings (settings screen, saved in settings.ini).
struct GfxSettings {
    int scale = 5;          // 0 = software renderer, 1..8 = OpenGL internal resolution (5 = 1200 lines)
    Aspect aspect = Aspect::A16_9;
    bool crt = false;
    bool texture_filter = false;
    int fps = 0;            // images per second: 0 = match the display, 30 = original, 60..150 (smooth.cpp)
    int aa = 3;             // anti-aliasing: bit 0 = FXAA pass, bit 1 = supersampling (SSAA)
    int msaa = 1;           // multisample anti-aliasing: samples per pixel (1, 2, 4, 8)
    bool vulkan = false;    // renderer: Vulkan instead of OpenGL (applies at the next start)
};
int g_active_msaa = 1;      // MSAA samples the renderer was created with
const char* const kAaNames[] = {"Off", "FXAA", "SSAA", "SSAA + FXAA"};
GfxSettings g_gfx;
float g_refresh_hz = 60.0f;  // display refresh rate
bool g_multi_present = false;  // not one image per emulated vblank: host_throttle_frame presents
bool g_present_vsync = true;   // ...paced by the display (else by a timer, for rates above its refresh)
uint64_t g_next_present_ns = 0;
constexpr int kFpsChoices[] = {0, 30, 60, 90, 120, 150};
int g_active_scale = -1;    // renderer actually in use

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

// Short on-screen messages ("State 1 saved").
std::string g_toast;
uint64_t g_toast_until = 0;

void draw_toast() {
    if (g_toast.empty() || SDL_GetTicksNS() >= g_toast_until) return;
    SDL_Renderer* r = present_overlay_begin();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(r, 2.0f, 2.0f);
    float w = 8.0f * (float)g_toast.size() + 12.0f;
    SDL_FRect box{8, 8, w, 16};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 255, 230, 180, 255);
    SDL_RenderDebugText(r, 14, 12, g_toast.c_str());
    SDL_SetRenderScale(r, 1.0f, 1.0f);
    present_overlay_end();
}

PresentOptions present_options() {
    PresentOptions o;
    o.aspect = g_gfx.aspect;
    o.crt = g_gfx.crt;
    o.fxaa = g_gfx.aa & 1;
    return o;
}

// Settings screen text for the multisampling rate (with what the GPU could give).
std::string msaa_label() {
    std::string s = std::to_string(g_gfx.msaa) + "x" + (g_gfx.msaa == 1 ? " (off)" : "");
    int got = hw_samples();
    if (g_gfx.msaa > 1 && got > 0 && got < g_gfx.msaa) s += ", " + std::to_string(got) + "x fits";
    return s;
}

// Settings screen text for the simulation rate choice.
std::string sim60_label() {
    const char* names[] = {"30 Hz original", "60 Hz level 1", "60 Hz all (beta)"};
    std::string s = names[std::clamp(g_sim60_setting, 0, 2)];
    if (psx_opt_sim60) s += " ON";  // active in the current level
    return s;
}

// Settings screen text for the frame rate choice.
std::string fps_label() {
    if (g_gfx.fps == 30) return "30 (original)";
    if (g_active_scale <= 0) return "30 (others need 1x+)";
    int rate = g_gfx.fps ? g_gfx.fps : 30 * smooth_factor_for_refresh(g_refresh_hz);
    std::string s = g_gfx.fps ? std::to_string(rate) : "Auto: " + std::to_string(rate);
    if ((float)rate > g_refresh_hz + 2.0f) s += " (no vsync)";
    return s;
}

// Applies graphics settings that affect emulation (renderer, widescreen projection).
// Images per game step for the current settings and display (1 = the original 30 fps).
int smooth_target() {
    if (psx_opt_sim60) return 1;  // the game itself renders every VBlank
    if (g_force_smooth) return g_force_smooth;
    if (g_active_scale <= 0) return 1;  // extra images need the OpenGL renderer
    if (g_gfx.fps == 0) return smooth_factor_for_refresh(g_refresh_hz);
    return std::clamp(g_gfx.fps / 30, 1, 5);
}

// Internal resolution actually rendered: the Resolution setting, raised by supersampling to
// about twice the lines the window shows (at most 8x = 1920 lines).
int render_scale() {
    if (g_gfx.scale == 0 || !(g_gfx.aa & 2) || !g_window) return g_gfx.scale;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    return std::clamp((2 * h + 239) / 240, g_gfx.scale, 8);
}

void apply_graphics() {
    const int want = render_scale();
    if (g_gl_ok && (want != g_active_scale || g_gfx.msaa != g_active_msaa)) {
        g_active_scale = gpu_set_renderer(want, g_gfx.msaa);
        g_active_msaa = g_gfx.msaa;
    }
    if (!g_gl_ok) g_active_scale = 0;
    hw_set_texture_filter(g_gfx.texture_filter);
    smooth_set_factor(smooth_target());
    // A 60 Hz display at 60 images/s shows one image per emulated vblank (vsync pacing);
    // other combinations present from host_throttle_frame: in step with the display when
    // it is at least as fast as the image rate, otherwise on a timer without vsync.
    const float rate = 30.0f * (float)smooth_factor();
    g_multi_present = smooth_factor() >= 2 && !(g_vsync_pacing && smooth_factor() == 2);
    g_present_vsync = rate <= g_refresh_hz + 2.0f;
    switch (g_gfx.aspect) {
        case Aspect::A16_9: g_gte_widescreen = 0.75f; break;       // (4/3) / (16/9)
        case Aspect::A16_10: g_gte_widescreen = 5.0f / 6.0f; break; // (4/3) / (16/10)
        default: g_gte_widescreen = 1.0f; break;
    }
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
    std::fprintf(f, "gfx scale=%d aspect=%d crt=%d filter=%d fps=%d\n", g_gfx.scale, (int)g_gfx.aspect,
                 g_gfx.crt ? 1 : 0, g_gfx.texture_filter ? 1 : 0, g_gfx.fps);
    std::fprintf(f, "sim %d\n", g_sim60_setting);
    std::fprintf(f, "aa %d\n", g_gfx.aa);
    std::fprintf(f, "msaa %d\n", g_gfx.msaa);
    std::fprintf(f, "renderer %s\n", g_gfx.vulkan ? "vulkan" : "opengl");
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
        int sc, as, crt, filt, fps, smooth;
        int got = std::sscanf(line, "gfx scale=%d aspect=%d crt=%d filter=%d fps=%d", &sc, &as, &crt, &filt, &fps);
        if (got >= 4) {
            g_gfx.scale = std::clamp(sc, 0, 8);
            g_gfx.aspect = (Aspect)std::clamp(as, 0, 3);
            g_gfx.crt = crt != 0;
            g_gfx.texture_filter = filt != 0;
            if (got == 5) g_gfx.fps = std::clamp(fps / 30 * 30, 0, 150);
            else if (std::sscanf(line, "gfx scale=%*d aspect=%*d crt=%*d filter=%*d smooth=%d", &smooth) == 1)
                g_gfx.fps = smooth ? 0 : 30;  // older settings file
            continue;
        }
        if (char rn[16]; std::sscanf(line, "renderer %15s", rn) == 1) {
            g_gfx.vulkan = std::strcmp(rn, "vulkan") == 0;
            continue;
        }
        if (int ms; std::sscanf(line, "msaa %d", &ms) == 1) {
            g_gfx.msaa = ms >= 8 ? 8 : ms >= 4 ? 4 : ms >= 2 ? 2 : 1;
            continue;
        }
        if (int aa; std::sscanf(line, "aa %d", &aa) == 1) {
            g_gfx.aa = std::clamp(aa, 0, 3);
            continue;
        }
        if (int sim; std::sscanf(line, "sim %d", &sim) == 1) {
            g_sim60_setting = std::clamp(sim, 0, 2);
            continue;
        }
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
    // Window shaped like the picture. `scale` N asks for 240*N lines; 0 (the default) asks for
    // 1080: a 1080p window if the desktop has room, else fullscreen on a 1080-line display,
    // else the largest window that fits.
    const float ratio = g_gfx.aspect == Aspect::A16_9 ? 16.0f / 9 : g_gfx.aspect == Aspect::A16_10 ? 16.0f / 10 : 4.0f / 3;
    int win_h = scale > 0 ? 240 * scale : 1080;
    bool fullscreen = false;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect usable{};
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (SDL_GetDisplayUsableBounds(display, &usable)) {
        const int frame = 40;  // title bar and borders
        int fit_h = std::min(usable.h - frame, (int)((float)(usable.w - 16) / ratio));
        if (win_h > fit_h) {
            if (scale == 0 && desktop && desktop->h >= 1080 && (float)desktop->w >= 1080 * ratio - 1)
                fullscreen = true;  // a 1080-line screen: use all of it
            else
                win_h = std::max(240, fit_h);
        }
    }
    int win_w = (int)((float)win_h * ratio + 0.5f);
    if (desktop)
        log_msg(LogCat::Host, "desktop %dx%d; window %dx%d%s", desktop->w, desktop->h, win_w, win_h,
                fullscreen ? " (fullscreen)" : "");
    // The renderer: Vulkan if chosen (and it starts), otherwise OpenGL.
    auto open_window = [&](Backend b) {
        render_select(b);
        if (b == Backend::OpenGL) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        }
        g_window = SDL_CreateWindow(title, win_w, win_h,
                                    SDL_WINDOW_RESIZABLE | (b == Backend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) |
                                        (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!g_window) return false;
        if (present_init(g_window)) return true;
        present_shutdown();
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return false;
    };
    const bool want_vulkan = g_renderer_override >= 0 ? g_renderer_override == 1 : g_gfx.vulkan;
    g_gl_ok = want_vulkan && open_window(Backend::Vulkan);
    if (want_vulkan && !g_gl_ok) {
        log_msg(LogCat::Host, "Vulkan did not start; using OpenGL");
        host_toast("Vulkan unavailable - using OpenGL");
    }
    if (!g_gl_ok) g_gl_ok = open_window(Backend::OpenGL);
    if (!g_gl_ok) fatal("Could not start the graphics: this needs OpenGL 3.3 or Vulkan 1.3.");
    log_msg(LogCat::Host, "renderer: %s", render_backend_name(render_backend()));
    // On a ~60 Hz display, pace the emulation by vsync: one emulated frame per refresh
    // gives perfectly even frame times (the game's 30 fps then shows each frame for
    // exactly two refreshes). Other refresh rates fall back to a timer.
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window))) {
        if (mode->refresh_rate > 0) g_refresh_hz = mode->refresh_rate;
        if (g_refresh_override > 0) g_refresh_hz = g_refresh_override;
        g_vsync_pacing = g_refresh_hz > 58.5f && g_refresh_hz < 61.5f;
        log_msg(LogCat::Host, "display refresh %.2f Hz: %s", g_refresh_hz,
                g_vsync_pacing ? "pacing by vsync" : "pacing by timer");
    }
    present_set_vsync(g_vsync_pacing);
    g_vsync_active = g_vsync_pacing;
    apply_graphics();

    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    else log_msg(LogCat::Host, "no audio device: %s", SDL_GetError());
    g_next_frame_ns = SDL_GetTicksNS();
    return true;
}

void host_shutdown() {
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    present_shutdown();
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
                if (e.key.scancode == SDL_SCANCODE_F5 && !e.key.repeat) state_request_save(1);
                if (e.key.scancode == SDL_SCANCODE_F9 && !e.key.repeat) state_request_load(1);
                if (e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
                    bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(g_window, !fs);
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: apply_graphics(); break;
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
    if (open_menu && g_gl_ok) run_settings_menu();
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
std::vector<uint64_t> g_shot_frames;  // --screenshot F1,F2,... PREFIX
std::string g_shot_prefix;

// --states save1@4000,load1@4600 : scripted save/load requests (testing)
std::vector<std::pair<uint64_t, int>> g_state_script;  // frame -> +slot (save) / -slot-1 (load)

static void run_state_script() {
    for (auto& [f, op] : g_state_script) {
        if (f != g_frame) continue;
        if (op >= 0) state_request_save(op);
        else state_request_load(-op - 1);
    }
}

bool host_parse_state_script(const char* spec) {
    std::string s(spec);
    for (size_t p = 0; p < s.size();) {
        size_t e = s.find(',', p);
        if (e == std::string::npos) e = s.size();
        std::string item = s.substr(p, e - p);
        p = e + 1;
        int slot;
        unsigned long long frame;
        if (std::sscanf(item.c_str(), "save%d@%llu", &slot, &frame) == 2) g_state_script.push_back({frame, slot});
        else if (std::sscanf(item.c_str(), "load%d@%llu", &slot, &frame) == 2) g_state_script.push_back({frame, -slot - 1});
        else return false;
    }
    return true;
}

static void maybe_screenshot() {
    for (uint64_t f : g_shot_frames)
        if (f == g_frame) present_save_bmp((g_shot_prefix + "_" + std::to_string(f) + ".bmp").c_str());
}

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

GpuDisplay g_last_display{};
uint64_t g_images = 0;      // distinct images shown (for the fps in the title)
uint64_t g_last_image = ~0ull;

// Draws the game image for `frac` of the way into the current emulated vblank, then swaps.
static void draw_game(const GpuDisplay& d, double frac) {
    int w = std::clamp(d.w, 1, 1024), h = std::clamp(d.h, 1, 512);
    PresentOptions opt = present_options();
    // 15-bit images drawn by the OpenGL renderer are shown straight from its VRAM (or an
    // extra frame in between the game's own, see smooth.cpp); 24-bit movie frames (and
    // the software renderer) go through the CPU copy.
    if (d.enabled && !d.rgb24 && hw_active()) {
        unsigned tex;
        int tw, th;
        uint64_t image;
        if (smooth_pick((double)g_frame + frac, &tex, &tw, &th)) {
            present_game_texture(tex, tw, th, 0, 0, tw, th, h, opt);
            image = smooth_flips() << 8 | (tex & 0xFF);
        } else {
            int px, py, pw, ph;
            unsigned vram = hw_display_texture(d.x, d.y, w, h, &px, &py, &pw, &ph);
            present_game_texture(vram, 1024 * hw_scale(), 512 * hw_scale(), px, py, pw, ph, h, opt);
            image = smooth_flips() << 8;
        }
        if (image != g_last_image) g_images++;
        g_last_image = image;
        draw_toast();
        maybe_screenshot();
        present_swap();
        return;
    }
    if (smooth_flips() << 8 != g_last_image) g_images++;
    g_last_image = smooth_flips() << 8;
    g_pixels.resize((size_t)w * h);
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
    present_game_pixels(g_pixels.data(), w, h, opt);
    draw_toast();
    maybe_screenshot();
    present_swap();
}

void host_present(const GpuDisplay& d) {
    g_frame++;
    run_state_script();
    state_poll_load(g_cpu);
    if (g_dump_vram_frame && g_frame == g_dump_vram_frame) {
        write_vram_bmp(d.vram, g_dump_vram_path);
        log_msg(LogCat::Host, "VRAM dumped to %s (display at %d,%d %dx%d%s)", g_dump_vram_path.c_str(), d.x, d.y,
                d.w, d.h, d.rgb24 ? " 24-bit" : "");
    }
    if (!g_gl_ok) return;
    static uint64_t fps_t0 = SDL_GetTicksNS(), fps_frames = 0, fps_images0 = 0, last_present = 0, dt_min = ~0ull,
                    dt_max = 0;
    uint64_t t_now = SDL_GetTicksNS();
    if (last_present) {
        dt_min = std::min(dt_min, t_now - last_present);
        dt_max = std::max(dt_max, t_now - last_present);
    }
    last_present = t_now;
    if (++fps_frames == 60) {
        uint64_t now = SDL_GetTicksNS();
        double secs = (double)(now - fps_t0) / 1e9;
        double fps = 60.0 / secs, images = (double)(g_images - fps_images0) / secs;
        extern int g_audio_underruns, g_audio_drops, g_audio_min_queued;
        LOG(Host, "frame times: min %.2f ms, max %.2f ms; audio: queue min %d, underruns %d, drops %d",
            dt_min / 1e6, dt_max / 1e6, g_audio_min_queued, g_audio_underruns, g_audio_drops);
        g_audio_min_queued = 1 << 30;
        dt_min = ~0ull;
        dt_max = 0;
        char title[96];
        std::snprintf(title, sizeof title, "Apocalypse %s - %.0f fps%s", kVersion, images,
                      g_turbo ? " (fast-forward)" : "");
        SDL_SetWindowTitle(g_window, title);
        LOG(Host, "frame %llu: %.1f Hz emulated, %.1f images/s", (unsigned long long)g_frame, fps, images);
        fps_t0 = now;
        fps_frames = 0;
        fps_images0 = g_images;
    }
    g_last_display = d;
    if (g_multi_present && !g_turbo) return;  // host_throttle_frame shows it over several refreshes
    draw_game(d, 0.0);
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
    g_renderer = present_overlay_begin();
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 200);
    SDL_FRect bg{0, 0, 640, 480};
    SDL_RenderFillRect(g_renderer, &bg);
    SDL_SetRenderScale(g_renderer, 2.0f, 2.0f);  // 8x8 debug font -> 16x16; 40x30 character grid
    for (int i = 0; i < (int)lines.size(); i++) {
        float y = 10.0f + 11.0f * (float)i;
        if (i == highlight) {
            SDL_SetRenderDrawColor(g_renderer, 200, 60, 20, 255);
            SDL_FRect r{12, y - 2, 296, 11};
            SDL_RenderFillRect(g_renderer, &r);
        }
        SDL_SetRenderDrawColor(g_renderer, 255, 230, 180, 255);
        SDL_RenderDebugText(g_renderer, 16, y, lines[i].c_str());
    }
    SDL_SetRenderScale(g_renderer, 1.0f, 1.0f);
    present_redraw(present_options());
    present_overlay_end();
    present_swap();
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
    enum Row { R_SAVE, R_LOAD, R_LOAD_AUTO, R_RENDERER, R_RES, R_AA, R_MSAA, R_ASPECT, R_CRT, R_FILTER, R_FPS, R_SIM, R_DEVICE, R_REMAP,
               R_RESET, R_BACK, R_COUNT };
    static int slot = 1;
    size_t dev_index = 0;
    int sel = 0;
    std::string status;
    // buttons held when the menu opens (e.g. SELECT+START) must not count as presses
    uint16_t prev_pad = host_pad(0).buttons;
    bool quit = false;
    static const char* kAspect[] = {"4:3", "16:9", "16:10", "Stretch"};
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

        // controls device: entry 0 is the keyboard, entries 1.. are the connected controllers
        const size_t entries = 1 + g_devices.size();
        if (dev_index >= entries) dev_index = 0;
        const bool is_keyboard = dev_index == 0;
        bool gfx_changed = false;
        for (auto k : keys) {
            int dir = k == SDL_SCANCODE_LEFT ? -1 : k == SDL_SCANCODE_RIGHT ? 1 : 0;
            if (k == SDL_SCANCODE_UP) sel = (sel + R_COUNT - 1) % R_COUNT;
            if (k == SDL_SCANCODE_DOWN) sel = (sel + 1) % R_COUNT;
            if (k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_F1) open = false;
            bool enter = k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER;
            if (dir || enter) {
                int step = dir ? dir : 1;
                switch (sel) {
                    case R_RES: g_gfx.scale = (g_gfx.scale + step + 9) % 9; gfx_changed = true; break;
                    case R_AA: g_gfx.aa = (g_gfx.aa + step + 4) % 4; gfx_changed = true; break;
                    case R_RENDERER:  // saved; takes effect when the game is started again
                        g_gfx.vulkan = !g_gfx.vulkan;
                        save_settings();
                        status = "Renderer changes on the next start.";
                        break;
                    case R_MSAA: {
                        static const int rates[] = {1, 2, 4, 8};
                        int i = 0;
                        while (i < 3 && rates[i] != g_gfx.msaa) i++;
                        g_gfx.msaa = rates[(i + step + 4) % 4];
                        gfx_changed = true;
                        break;
                    }
                    case R_ASPECT: g_gfx.aspect = (Aspect)(((int)g_gfx.aspect + step + 4) % 4); gfx_changed = true; break;
                    case R_CRT: g_gfx.crt = !g_gfx.crt; gfx_changed = true; break;
                    case R_FILTER: g_gfx.texture_filter = !g_gfx.texture_filter; gfx_changed = true; break;
                    case R_FPS: {
                        int n = (int)std::size(kFpsChoices), i = 0;
                        while (i < n - 1 && kFpsChoices[i] != g_gfx.fps) i++;
                        g_gfx.fps = kFpsChoices[(i + step + n) % n];
                        gfx_changed = true;
                        break;
                    }
                    case R_SIM: {  // 60 Hz simulation (sim60.cpp)
                        static const int order[] = {1, 0, 2};  // level 1, off, all levels
                        int i = 0;
                        while (i < 2 && order[i] != g_sim60_setting) i++;
                        g_sim60_setting = order[(i + step + 3) % 3];
                        sim60_apply();
                        gfx_changed = true;
                        break;
                    }
                    case R_DEVICE: dev_index = (dev_index + entries + step) % entries; break;
                    case R_SAVE:
                    case R_LOAD:
                        if (dir) slot = (slot - 1 + step + kStateSlots) % kStateSlots + 1;
                        break;
                    default: break;
                }
            }
            if (!enter) continue;
            if (sel == R_BACK) {
                open = false;
            } else if (sel == R_SAVE) {
                state_request_save(slot);  // taken at the next frame, once the menu closes
                open = false;
            } else if (sel == R_LOAD || sel == R_LOAD_AUTO) {
                int s = sel == R_LOAD ? slot : kAutoSlot;
                if (state_slot_exists(s)) {
                    state_request_load(s);
                    open = false;
                } else {
                    status = "That slot is empty.";
                }
            } else if (sel == R_REMAP && is_keyboard) {
                SDL_Scancode keys_new[16];
                if (remap_keyboard(keys_new, &quit)) {
                    std::memcpy(g_keys, keys_new, sizeof g_keys);
                    g_key_custom = true;
                    save_settings();
                    status = "Saved. This layout is used every time.";
                } else if (!quit) {
                    status = "Cancelled, layout unchanged.";
                }
                prev_pad = host_pad(0).buttons;
            } else if (sel == R_REMAP) {
                InputDevice dev = g_devices[dev_index - 1];
                Layout lay;
                if (remap_device(dev, &lay, &quit)) {
                    g_layouts[device_guid(dev)] = lay;
                    save_settings();
                    status = "Saved. This layout is used every time.";
                } else if (!quit) {
                    status = "Cancelled, layout unchanged.";
                }
                prev_pad = host_pad(0).buttons;
            } else if (sel == R_RESET) {
                if (is_keyboard) g_key_custom = false;
                else g_layouts.erase(device_guid(g_devices[dev_index - 1]));
                save_settings();
                status = "Restored the default layout.";
            }
        }
        if (gfx_changed) {
            apply_graphics();
            save_settings();
            if (g_gfx.scale > 0 && g_active_scale == 0) status = "Hardware renderer unavailable.";
            else status = "";
        }
        if (!open || quit) break;

        std::string res = g_gfx.scale == 0 ? std::string("Original (software)")
                                           : std::to_string(g_gfx.scale) + "x  (" + std::to_string(240 * g_gfx.scale) +
                                                 " lines)";
        if (g_gfx.scale > 0 && g_active_scale > g_gfx.scale)
            res = std::to_string(g_gfx.scale) + "x, SSAA: " + std::to_string(g_active_scale) + "x";
        std::string dev_line = is_keyboard ? std::string("Keyboard & mouse")
                                           : std::string(device_name(g_devices[dev_index - 1])).substr(0, 22);
        bool custom = is_keyboard ? g_key_custom : g_layouts.count(device_guid(g_devices[dev_index - 1])) > 0;
        auto row = [](const char* label, const std::string& value) {
            std::string s = label;
            s.resize(16, ' ');
            return s + "< " + value + " >";
        };
        std::string slot_line = std::to_string(slot) + ": " + state_slot_label(slot);
        draw_panel({"SETTINGS",
                    row("Save state", slot_line),
                    row("Load state", slot_line),
                    row("Load auto-save", state_slot_label(kAutoSlot)),
                    row("Renderer", std::string(g_gfx.vulkan ? "Vulkan" : "OpenGL") +
                                        ((g_gfx.vulkan == (render_backend() == Backend::Vulkan)) ? "" : " (restart)")),
                    row("Resolution", res),
                    row("Anti-aliasing", kAaNames[g_gfx.aa]),
                    row("AA rate (MSAA)", msaa_label()),
                    row("Aspect ratio", kAspect[(int)g_gfx.aspect]),
                    row("CRT filter", g_gfx.crt ? "On" : "Off"),
                    row("Smooth textures", g_gfx.texture_filter ? "On" : "Off"),
                    row("Frame rate", fps_label()),
                    row("Simulation", sim60_label()),
                    row("Controls", dev_line),
                    std::string("Set up buttons") + (custom ? "  (custom)" : "  (default)"),
                    "Reset buttons to default",
                    "Back to game",
                    status,
                    "Arrows: change  Enter: select  Esc: back",
                    "F5/F9: quick save/load  Pad: SELECT+START"},
                   1 + sel);
        SDL_Delay(15);
    }
    if (quit) g_quit = true;
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    g_next_frame_ns = SDL_GetTicksNS();  // don't try to "catch up" the paused time
}

void host_throttle_frame() {
    bool multi = g_multi_present && !g_turbo && g_gl_ok;
    bool want_vsync = (multi ? g_present_vsync : g_vsync_pacing) && !g_turbo;
    if (want_vsync != g_vsync_active) {
        present_set_vsync(want_vsync);
        g_vsync_active = want_vsync;
    }
    if (g_turbo || (g_vsync_active && !multi)) {  // vsync: the swap already waited
        g_next_frame_ns = SDL_GetTicksNS();
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    g_next_frame_ns += FRAME_NS;
    if (multi) {
        // Several images per emulated vblank, each picking the image for its moment in time.
        const uint64_t start = g_next_frame_ns - FRAME_NS;
        auto frac_at = [&](uint64_t t) {
            return std::clamp((double)(int64_t)(t - start) / (double)FRAME_NS, 0.0, 0.999);
        };
        if (g_present_vsync) {
            // As many display refreshes as fit before the next vblank is due (each swap
            // waits for the display).
            const uint64_t refresh = (uint64_t)(1e9 / g_refresh_hz);
            for (bool drawn = false;; drawn = true) {
                now = SDL_GetTicksNS();
                if (drawn && now + refresh / 2 > g_next_frame_ns) break;
                draw_game(g_last_display, frac_at(now));
            }
        } else {
            // The image rate is above the display refresh: present on our own clock.
            const uint64_t period = (uint64_t)(1e9 / (30.0 * smooth_factor()));
            if (g_next_present_ns + 4 * period < start || g_next_present_ns > g_next_frame_ns + period)
                g_next_present_ns = start;  // (re)start in step with emulated time
            while (g_next_present_ns < g_next_frame_ns) {
                now = SDL_GetTicksNS();
                if (g_next_present_ns > now) SDL_DelayPrecise(g_next_present_ns - now);
                draw_game(g_last_display, frac_at(g_next_present_ns));
                g_next_present_ns += period;
            }
        }
        now = SDL_GetTicksNS();
    }
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

// playback health, reported with the frame times (--log host)
int g_audio_underruns = 0, g_audio_drops = 0, g_audio_min_queued = 1 << 30;

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
    g_audio_min_queued = std::min(g_audio_min_queued, queued);
    if (queued == 0) g_audio_underruns++;  // the device ran dry since the last push: a gap
    if (queued > target * 4) {  // way behind (after a stall): drop rather than lag
        g_audio_drops++;
        return;
    }
    float ratio = 1.0f + 0.005f * std::clamp((float)(queued - target) / (float)target, -1.0f, 1.0f);
    SDL_SetAudioStreamFrequencyRatio(g_audio, ratio);
    SDL_PutAudioStreamData(g_audio, stereo, frames * 4);
}

int host_audio_queued_frames() { return g_audio ? SDL_GetAudioStreamQueued(g_audio) / 4 : 0; }

void host_toast(const std::string& text) {
    g_toast = text;
    g_toast_until = SDL_GetTicksNS() + 2500000000ull;
    log_msg(LogCat::Host, "%s", text.c_str());
}

void host_audio_clear() {
    if (g_audio) SDL_ClearAudioStream(g_audio);
}