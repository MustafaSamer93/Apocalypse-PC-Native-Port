// Host platform layer: SDL3 window, audio, input and frame pacing.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "gpu_hw.h"
#include "present.h"
#include "render_backend.h"
#include "psx.h"
#include "savestate.h"
#include "smooth.h"
#include "ui_text.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#endif

constexpr const char* kVersion = "v1.0";
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
    bool crt = false;       // CRT filter (present.cpp / present_vk.cpp)
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
bool g_present_timed = false;  // ...on our own clock with vsync on (a display not a multiple of 60 Hz: 75)
uint64_t g_next_present_ns = 0;
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
// APOC_BOT=DIR (bot_step): buttons the AI tester holds until frame g_bot_until.
uint16_t g_bot_buttons = 0;
uint64_t g_bot_until = 0;

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

// Badges: a line of white text in a dark box at the bottom of the window, drawn into its own
// small surface only when the text or the window size changes (present_badge keeps it).
// Slot 0: the FPS counter, lower left; slot 1: the version, lower right.
void draw_badge(int slot, const std::string& text, int chars, bool right) {
    struct Badge { SDL_Surface* surf = nullptr; SDL_Renderer* ren = nullptr; std::string text; int h = 0; };
    static Badge badges[2];
    Badge& b = badges[slot];
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
    const int size = std::max(12, wh * 26 / 1080), pad = size / 3;  // 26 px at 1080 lines
    bool changed = false;
    if (!b.surf || b.h != wh) {  // made for the window's size, `chars` characters wide
        if (b.ren) ui_text_reset(b.ren), SDL_DestroyRenderer(b.ren);
        if (b.surf) SDL_DestroySurface(b.surf);
        b.surf = SDL_CreateSurface(size * chars * 3 / 5 + 2 * pad, size + pad, SDL_PIXELFORMAT_ARGB8888);
        b.ren = b.surf ? SDL_CreateSoftwareRenderer(b.surf) : nullptr;
        b.h = wh;
        b.text.clear();
    }
    if (!b.ren) return;
    if (text != b.text) {
        b.text = text;
        changed = true;
        SDL_SetRenderDrawBlendMode(b.ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(b.ren, 0, 0, 0, 150);
        SDL_RenderClear(b.ren);
        SDL_SetRenderDrawBlendMode(b.ren, SDL_BLENDMODE_BLEND);
        const float tw = ui_text_width(b.ren, text.c_str(), (float)size);
        SDL_SetRenderDrawColor(b.ren, 255, 255, 255, 255);
        ui_text(b.ren, ((float)b.surf->w - tw) * 0.5f, (float)pad * 0.5f, text.c_str(), (float)size);
        SDL_RenderPresent(b.ren);
    }
    const int margin = size / 2;
    present_badge(slot, (const uint32_t*)b.surf->pixels, b.surf->w, b.surf->h, right ? ww - margin - b.surf->w : margin,
                  margin, changed);
}

// FPS counter (Enhancements): the images shown per second.
bool g_fps_counter = false;
double g_fps_shown = 0;  // images per second over the last second (host_present)

void draw_fps_counter() {
    if (g_fps_counter) draw_badge(0, std::to_string((int)std::lround(g_fps_shown)) + " FPS", 7, false);
}

void draw_toast() {
    if (g_toast.empty() || SDL_GetTicksNS() >= g_toast_until) return;
    SDL_Renderer* r = present_overlay_begin();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    constexpr float kSize = 18.0f;
    SDL_FRect box{8, 8, ui_text_width(r, g_toast.c_str(), kSize) + 16.0f, kSize + 6.0f};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 255, 230, 180, 255);
    ui_text(r, 16, 11, g_toast.c_str(), kSize);
    present_overlay_end();
}

// The version, on the title screen and while the game's menus are up.
void draw_version() {
    if (gpu_title_screen() || replace_title_shown(gpu_display().y)) draw_badge(1, kVersion, (int)std::strlen(kVersion) + 1, true);
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
    const char* names[] = {"30 Hz original", "60 Hz all levels", "60 Hz all (beta)"};
    std::string s = names[std::clamp(g_sim60_setting, 0, 2)];
    if (g_sim60_fps30) s += " (frame rate 30)";  // the frame rate setting keeps it off
    else if (psx_opt_sim60) s += " ON";          // active in the current level
    return s;
}

// Draw distance choices, percent of the game's own (drawdist.cpp).
constexpr int kDrawDistChoices[] = {100, 150, 200, 300, 400};

std::string drawdist_label() {
    if (g_draw_distance <= 100) return "Original";
    if (g_draw_distance >= 400) return "Max";  // the depth range (16380) in every level (drawdist.cpp)
    std::string s = std::to_string(g_draw_distance / 100);
    if (g_draw_distance % 100) s += "." + std::to_string(g_draw_distance % 100 / 10);
    return s + "x";
}

// Most images per game step the display can show (a frame rate above its refresh rate was
// presented on a timer without vsync: no more images reached the screen, only uneven ones).
double max_factor_for_display() { return std::max(1.0, (g_refresh_hz + 2.0) / 30.0); }

// Settings screen text for the frame rate choice.
std::string fps_label() {
    if (g_gfx.fps == 30) return "30 (original)";
    if (g_active_scale <= 0) return "30 (others need 1x+)";
    const int rate = g_gfx.fps ? g_gfx.fps : (int)std::lround(30 * smooth_factor_for_refresh(g_refresh_hz));
    const int screen = (int)std::lround(g_refresh_hz);
    std::string s = g_gfx.fps ? std::to_string(rate) : "Auto: " + std::to_string(rate);
    if (g_gfx.fps && rate > screen) s += " (screen: " + std::to_string(screen) + ")";
    return s;
}

// Applies graphics settings that affect emulation (renderer, widescreen projection).
// Images per game step for the current settings and display (1 = the original 30 fps).
double smooth_target() {
    if (g_force_smooth) return g_force_smooth;
    if (g_active_scale <= 0) return 1;  // extra images need the OpenGL renderer
    if (g_gfx.fps == 0) return smooth_factor_for_refresh(g_refresh_hz);
    return std::clamp(g_gfx.fps / 30.0, 1.0, std::min(5.0, max_factor_for_display()));
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
    // "30 (original)": the game as it was, so no 60 Hz simulation either (sim60.cpp)
    if (g_sim60_fps30 != (g_gfx.fps == 30)) {
        g_sim60_fps30 = g_gfx.fps == 30;
        sim60_apply();
    }
    const int want = render_scale();
    if (g_gl_ok && (want != g_active_scale || g_gfx.msaa != g_active_msaa)) {
        g_active_scale = gpu_set_renderer(want, g_gfx.msaa);
        g_active_msaa = g_gfx.msaa;
    }
    if (!g_gl_ok) g_active_scale = 0;
    hw_set_texture_filter(g_gfx.texture_filter);
    smooth_set_factor(smooth_target());
    // The menus run at 60 Hz with a frame rate of 60 or more (game_menu.cpp slows what they animate).
    psx_opt_menu60 = (g_gfx.fps ? g_gfx.fps : 30 * smooth_factor_for_refresh(g_refresh_hz)) >= 60;
    // A 60 Hz display at 60 images/s shows one image per emulated vblank (vsync pacing);
    // other combinations present from host_throttle_frame: in step with the display when
    // it is at least as fast as the image rate, otherwise on a timer without vsync.
    const float rate = 30.0f * (float)smooth_factor();
    g_multi_present = smooth_factor() >= 2 && !(g_vsync_pacing && smooth_factor() == 2);
    g_present_vsync = rate <= g_refresh_hz + 2.0f;
    // A display whose refreshes do not line up with the 60 Hz vblanks (75 Hz) shows at most one
    // image per vblank when paced by it: present on our own clock there too (each swap still waits
    // for the display, so no tearing).
    const float beat = g_refresh_hz / 60.0f;
    g_present_timed = g_present_vsync && std::abs(beat - std::round(beat)) * 60.0f > 2.0f;
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
// (debug: APOC_SETTINGS=PATH uses another file, so test runs leave the player's alone)
const char* settings_file() {
    static const char* f = std::getenv("APOC_SETTINGS") ? std::getenv("APOC_SETTINGS") : "settings.ini";
    return f;
}
// Remapped keyboard: one key per PS1 button (bit number), used when g_key_custom.
SDL_Scancode g_keys[16] = {};
bool g_key_custom = false;
bool g_fullscreen = true;  // start in fullscreen (F11 switches; saved as "fullscreen N")

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

// Written to a new file, flushed to the disk and then put in place of the old one: a crash or
// power loss while saving leaves the old settings, never a file of zeros.
void save_settings() {
    const std::string tmp = std::string(settings_file()) + ".new";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# Written by the game's settings screen (F1). Safe to delete to reset.\n");
    std::fprintf(f, "gfx scale=%d aspect=%d crt=%d filter=%d fps=%d\n", g_gfx.scale, (int)g_gfx.aspect,
                 g_gfx.crt ? 1 : 0, g_gfx.texture_filter ? 1 : 0, g_gfx.fps);
    std::fprintf(f, "sim %d\n", g_sim60_setting);
    std::fprintf(f, "drawdist %d\n", g_draw_distance);
    std::fprintf(f, "precise %d\n", g_gte_precise ? 1 : 0);
    std::fprintf(f, "font %d\n", g_smooth_font ? 1 : 0);
    std::fprintf(f, "menuzoom %d\n", g_menu_zoom);
    std::fprintf(f, "autosave %d\n", g_autosave ? 1 : 0);
    std::fprintf(f, "subtitles %d\n", g_subtitles ? 1 : 0);
    std::fprintf(f, "fpscounter %d\n", g_fps_counter ? 1 : 0);
    std::fprintf(f, "bootseq %d\n", g_boot_sequence ? 1 : 0);
    std::fprintf(f, "texreplace %d\n", g_tex_replace ? 1 : 0);
    std::fprintf(f, "fullscreen %d\n", g_fullscreen ? 1 : 0);
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
    const bool ok = std::fflush(f) == 0 && !std::ferror(f);
#ifdef _WIN32
    if (ok) _commit(_fileno(f));
#endif
    std::fclose(f);
#ifdef _WIN32
    if (ok) MoveFileExA(tmp.c_str(), settings_file(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    if (ok) std::rename(tmp.c_str(), settings_file());
#endif
}

void load_settings() {
    FILE* f = std::fopen(settings_file(), "r");
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
            if (got == 5) g_gfx.fps = fps == 0 ? 0 : std::clamp(fps / 30 * 30, 30, 60);  // (75-150 were offered once)
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
        if (int fs; std::sscanf(line, "fullscreen %d", &fs) == 1) {
            g_fullscreen = fs != 0;
            continue;
        }
        if (int sf; std::sscanf(line, "font %d", &sf) == 1) {
            g_smooth_font = sf != 0;
            continue;
        }
        if (int tr; std::sscanf(line, "texreplace %d", &tr) == 1) g_tex_replace = tr != 0;
        if (int bs; std::sscanf(line, "bootseq %d", &bs) == 1) {
            g_boot_sequence = bs != 0;
            continue;
        }
        if (int fc; std::sscanf(line, "fpscounter %d", &fc) == 1) {
            g_fps_counter = fc != 0;
            continue;
        }
        if (int st; std::sscanf(line, "subtitles %d", &st) == 1) {
            g_subtitles = st != 0;
            continue;
        }
        if (int as; std::sscanf(line, "autosave %d", &as) == 1) {
            g_autosave = as != 0;
            continue;
        }
        if (int mz; std::sscanf(line, "menuzoom %d", &mz) == 1) {
            g_menu_zoom = std::clamp(mz, 30, 80);
            continue;
        }
        if (int pr; std::sscanf(line, "precise %d", &pr) == 1) {
            g_gte_precise = pr != 0;
            continue;
        }
        if (int dd; std::sscanf(line, "drawdist %d", &dd) == 1) {
            g_draw_distance = std::clamp(dd, 100, 400);
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

// Some OpenGL drivers crash while SDL sets up an OpenGL window (seen on a PC with DisplayLink USB
// display adapters next to its AMD GPU; every build closed there without a word). On Windows the
// crash is caught, but SDL is left broken by it (the game then hung on a black screen), so the
// renderer setting is switched to Vulkan and the game starts again as a new process.
static bool g_gl_crashed = false;
static SDL_Window* create_window(const char* title, int w, int h, SDL_WindowFlags flags) {
#ifdef _WIN32
    __try {
        return SDL_CreateWindow(title, w, h, flags);
    } __except (1) {
        g_gl_crashed = true;
        return nullptr;
    }
#else
    return SDL_CreateWindow(title, w, h, flags);
#endif
}

#ifdef _WIN32
// Starts this exe again with the same command line (and standard handles), then ends this one.
[[noreturn]] static void restart_self() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    std::_Exit(0);
}
#endif

// The top of the log (debug/apocalypse.log): what a tester's report needs to know about the
// game and the PC. The GPU, display and renderer follow when the window opens.
void report_header() {
    char when[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    log_msg(LogCat::Host, "===== Apocalypse %s report log, started %s =====", kVersion, when);
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW os{sizeof os};
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&os);
    log_msg(LogCat::Host, "system: Windows %lu.%lu build %lu", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    MEMORYSTATUSEX mem{sizeof mem};
    GlobalMemoryStatusEx(&mem);
    log_msg(LogCat::Host, "memory: %llu MB, %llu MB free", (unsigned long long)(mem.ullTotalPhys >> 20),
            (unsigned long long)(mem.ullAvailPhys >> 20));
#endif
    log_msg(LogCat::Host, "processor: %d logical cores, %d MB RAM (SDL)", SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM());
    if (FILE* f = std::fopen(settings_file(), "r")) {  // the settings, as saved
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            if (line[0] && line[0] != '#' && std::strncmp(line, "keys", 4) && std::strncmp(line, "pad ", 4))
                log_msg(LogCat::Host, "setting: %s", line);
        }
        std::fclose(f);
    }
}

bool host_init(const char* title, int scale) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        log_msg(LogCat::Host, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    // Community mappings (https://github.com/mdqinc/SDL_GameControllerDB), if the user adds the file.
    if (SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt") > 0)
        log_msg(LogCat::Host, "loaded extra controller mappings from gamecontrollerdb.txt");
    load_settings();
    report_header();
    // Fullscreen (on the desktop's own mode) unless the player chose a window with F11. The
    // window, for then: shaped like the picture; `scale` N asks for 240*N lines; 0 (the
    // default) asks for 1080: a 1080p window if the desktop has room, else the largest that fits.
    const float ratio = g_gfx.aspect == Aspect::A16_9 ? 16.0f / 9 : g_gfx.aspect == Aspect::A16_10 ? 16.0f / 10 : 4.0f / 3;
    int win_h = scale > 0 ? 240 * scale : 1080;
    const bool fullscreen = g_fullscreen;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect usable{};
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (SDL_GetDisplayUsableBounds(display, &usable)) {
        const int frame = 40;  // title bar and borders
        int fit_h = std::min(usable.h - frame, (int)((float)(usable.w - 16) / ratio));
        if (win_h > fit_h) win_h = std::max(240, fit_h);
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
        if (std::getenv("APOC_BACKGROUND")) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        g_window = create_window(title, win_w, win_h,
                                 SDL_WINDOW_RESIZABLE | (b == Backend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) |
                                     (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!g_window) return false;
#ifdef _WIN32
        if (std::getenv("APOC_BACKGROUND"))  // test runs: open behind the other windows, without focus
            SetWindowPos((HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr),
                         HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
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
#ifdef _WIN32
    if (g_gl_crashed && !want_vulkan && g_renderer_override != 0) {  // (not when --renderer opengl asks for it)
        log_msg(LogCat::Host, "the OpenGL driver crashed while opening the window: switching to Vulkan and restarting");
        g_gfx.vulkan = true;
        g_renderer_override = -1;
        save_settings();
        restart_self();
    }
#endif
    if (!g_gl_ok && !want_vulkan) {  // OpenGL failed: Vulkan instead
        log_msg(LogCat::Host, "OpenGL did not start; using Vulkan");
        g_gl_ok = open_window(Backend::Vulkan);
        if (g_gl_ok) host_toast("OpenGL unavailable - using Vulkan");
    }
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
static void run_chapter_menu();
static bool g_shot_key = false;  // F12: save the next frame to debug/ (maybe_screenshot)

// The mouse pointer hides after 2 s without mouse use and shows again when it is used.
// Called with every event (null = no event, just the timer) by both event loops.
static void mouse_autohide(const SDL_Event* e) {
    static uint64_t used = SDL_GetTicks();
    if (e && (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_WHEEL)) {
        used = SDL_GetTicks();
        if (!SDL_CursorVisible()) SDL_ShowCursor();
    }
    if (!e && SDL_CursorVisible() && SDL_GetTicks() - used > 2000) SDL_HideCursor();
}

void host_poll() {
    bool open_menu = false, open_chapters = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        mouse_autohide(&e);
        switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) g_quit = true;
                // F keys (settings, chapters, states, issue marker, screenshot, fullscreen) are off
                // in the game; APOC_DEV_KEYS=1 turns them on for development
                static const bool dev_keys = std::getenv("APOC_DEV_KEYS") != nullptr;
                if (!dev_keys) break;
                if (e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat) open_menu = true;
                if (e.key.scancode == SDL_SCANCODE_F2 && !e.key.repeat) open_chapters = true;
                if (e.key.scancode == SDL_SCANCODE_F5 && !e.key.repeat) state_request_save(1);
                if (e.key.scancode == SDL_SCANCODE_F9 && !e.key.repeat) state_request_load(1);
                if (e.key.scancode == SDL_SCANCODE_F8 && !e.key.repeat) {  // issue marker + state saves/slot1NN.state
                    // ponytail: numbering restarts each launch, so older issue states get overwritten
                    static int issue = 0;
                    extern std::string g_sim60_level;
                    extern int psx_opt_sim60;
                    issue++;
                    log_msg(LogCat::Host, "===== F8 issue #%d: frame %llu, level \"%s\", sim60 %d, state saves/slot%d.state =====",
                            issue, (unsigned long long)g_frame, g_sim60_level.c_str(), psx_opt_sim60, 100 + issue);
                    std::fflush(stderr);
                    state_request_save(100 + issue);
                }
                if (e.key.scancode == SDL_SCANCODE_F12 && !e.key.repeat) g_shot_key = true;
                if (e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
                    g_fullscreen = !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN);
                    SDL_SetWindowFullscreen(g_window, g_fullscreen);
                    save_settings();  // the next start opens the same way
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
    if (static const char* t = std::getenv("APOC_MENU_AT"); t && g_frame == std::strtoull(t, nullptr, 10)) open_menu = true;  // tests
    mouse_autohide(nullptr);
    if (open_menu && g_gl_ok) run_settings_menu();
    else if (open_chapters && g_gl_ok) run_chapter_menu();
}

void host_set_turbo(bool on) { g_force_turbo = on; }

bool host_quit_requested() { return g_quit; }
bool host_multi_present() { return g_multi_present && !g_turbo; }
void host_request_quit() { g_quit = true; }

// Button name as in --press ("SQUARE") -> pad bit, 0 if unknown.
static uint16_t button_bit(const std::string& name) {
    static const struct { const char* name; uint16_t bit; } names[] = {
        {"SELECT", B_SELECT}, {"START", B_START}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
        {"LEFT", B_LEFT}, {"L2", B_L2}, {"R2", B_R2}, {"L1", B_L1}, {"R1", B_R1},
        {"TRIANGLE", B_TRIANGLE}, {"CIRCLE", B_CIRCLE}, {"CROSS", B_CROSS}, {"SQUARE", B_SQUARE},
    };
    for (auto& n : names)
        if (name == n.name) return n.bit;
    return 0;
}

// "--press START@1200+10,CROSS@1500" : button@frame[+duration frames, default 6]
bool host_parse_press_script(const char* spec) {
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
        const uint16_t bit = button_bit(name);
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

}  // namespace

constexpr int kAimDeadband = 16;        // 4096ths of a turn (1.4 degrees): smaller stick changes are noise
constexpr int kStanceHysteresis = 96;   // 8.4 degrees past a stance border before the stance changes

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
        // Debug: APOC_PAD_SCRIPT=FRAME:BUTTON[:FRAMES],... presses a button (start, select, x, o,
        // t = triangle, sq = square, up, down, left, right, lone/rone/ltwo/rtwo = L1/R1/L2/R2) from
        // each FRAME, for FRAMES frames (6 if not given).
        static const char* script = std::getenv("APOC_PAD_SCRIPT");
        for (const char* p = script; p && *p;) {
            unsigned long long f = 0, len = 6;
            char name[16] = "";
            if (std::sscanf(p, "%llu:%15[a-z]:%llu", &f, name, &len) < 2) break;
            static const std::pair<const char*, uint16_t> kNames[] = {
                {"start", B_START}, {"x", B_CROSS}, {"o", B_CIRCLE}, {"t", B_TRIANGLE}, {"sq", B_SQUARE},
                {"up", B_UP}, {"down", B_DOWN}, {"left", B_LEFT}, {"right", B_RIGHT}, {"select", B_SELECT},
                {"lone", B_L1}, {"rone", B_R1}, {"ltwo", B_L2}, {"rtwo", B_R2}};
            if (g_frame >= f && g_frame < f + len)
                for (auto& [n, bit] : kNames)
                    if (!std::strcmp(n, name)) pressed |= bit;
            p = std::strchr(p, ',');
            if (p) p++;
        }
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
    if (port == 0 && g_frame < g_bot_until) pressed |= g_bot_buttons;
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
    if (!g_shot_key) return;
    g_shot_key = false;
    // debug/shot_<date>_<time>.bmp (a second one within the same second gets _2, _3...)
    std::error_code ec;
    std::filesystem::create_directories("debug", ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
    std::string path = std::string("debug/shot_") + stamp + ".bmp";
    for (int n = 2; std::filesystem::exists(path, ec); n++)
        path = std::string("debug/shot_") + stamp + "_" + std::to_string(n) + ".bmp";
    present_save_bmp(path.c_str());
    log_msg(LogCat::Host, "===== F12 screenshot %s: frame %llu =====", path.c_str(), (unsigned long long)g_frame);
    host_toast("Screenshot saved: " + path);
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

// Lines to cut off at the top and at the bottom of an image `h` lines high: in widescreen a
// movie (made for 4:3, with black bands in the picture) is enlarged to the full width, keeping
// its proportions, and loses what no longer fits (1/8 at each edge for 16:9).
static int movie_crop(int h) {
    if ((g_gfx.aspect != Aspect::A16_9 && g_gfx.aspect != Aspect::A16_10) || !gpu_movie_playing()) return 0;
    return (int)((float)h * (1.0f - g_gte_widescreen) * 0.5f);
}

// Draws the game image for `frac` of the way into the current emulated vblank, then swaps.
static void draw_game(const GpuDisplay& d, double frac) {
    int w = std::clamp(d.w, 1, 1024), h = std::clamp(d.h, 1, 512);
    PresentOptions opt = present_options();
    const int crop = movie_crop(h);
    // A replacement movie (movie.cpp) in place of the game's own movie picture, cropped like it.
    if (int mw = 0, mh = 0; d.enabled && d.rgb24 && gpu_movie_playing()) {
        if (const uint32_t* px = movie_picture(&mw, &mh)) {
            const int mc = (int)((int64_t)crop * mh / h);
            present_game_pixels(px + (size_t)mc * mw, mw, mh - 2 * mc, opt);
            if (g_last_image != (uint64_t)(uintptr_t)px) g_images++;  // (each new movie frame)
            g_last_image = (uint64_t)(uintptr_t)px;
            maybe_screenshot();
            draw_toast();
            draw_fps_counter();
            present_swap();
            return;
        }
    }
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
            const int cs = crop * hw_scale();
            present_game_texture(vram, 1024 * hw_scale(), 512 * hw_scale(), px, py + cs, pw, ph - 2 * cs, h - 2 * crop, opt);
            image = smooth_flips() << 8;
        }
        if (image != g_last_image) g_images++;
        g_last_image = image;
        if (g_bios_rom) {
            void bootfont_present(const GpuDisplay& d, SDL_Window* window);
            bootfont_present(d, g_window);  // the boot sequence's text (bootfont.cpp)
        }
        maybe_screenshot();  // before the toasts: the picture is the game alone
        draw_toast();
        draw_version();
        draw_fps_counter();
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
    present_game_pixels(g_pixels.data() + (size_t)crop * w, w, h - 2 * crop, opt);
    maybe_screenshot();
    draw_toast();
    draw_version();  // (the title screen's moving background is a 24-bit movie too)
    draw_fps_counter();
    present_swap();
}

// Test (APOC_BOT=DIR, tools/bot.py): an AI tester plays in steps. At the end of each step the
// game stops, DIR/state.txt says where the player is (the step's picture is DIR/shot_FRAME.bmp)
// and it waits for DIR/cmd.txt: "UP+SQUARE 30" = hold those buttons for 30 frames ("- 30" = none).
static void bot_step() {
    static const char* dir = std::getenv("APOC_BOT");
    if (!dir || g_frame < g_bot_until || g_frame < 60) return;
    const std::string d = dir;
    if (g_bot_until) {  // report the step that just ended
        auto ram_str = [](uint32_t a, size_t max) {  // progress record, see savestate.cpp
            std::string s;
            for (size_t i = 0; i < max && psx_ram[(a + i) & (PSX_RAM_SIZE - 1)]; i++) s += (char)psx_ram[(a + i) & (PSX_RAM_SIZE - 1)];
            return s;
        };
        const std::string level = ram_str(0x800A53D8, 9), restart = ram_str(0x800A53E1, 24);
        const uint32_t p = MEM_R32(g_cpu, 0x800FF5A0);  // the player object
        if (FILE* f = std::fopen((d + "/state.tmp").c_str(), "w")) {
            std::fprintf(f, "frame %llu\nshot %s/shot_%llu.bmp\nlevel %s\ncheckpoint %s\nlives %d\n",
                         (unsigned long long)g_frame, dir, (unsigned long long)g_frame, level.c_str(), restart.c_str(),
                         (int)MEM_R32(g_cpu, 0x800FF874));
            if ((p & 0xFF000000u) == 0x80000000u)
                std::fprintf(f, "health %d\npos %d %d %d\n", (int16_t)MEM_R16(g_cpu, p + 0xDA), (int32_t)MEM_R32(g_cpu, p + 4),
                             (int32_t)MEM_R32(g_cpu, p + 8), (int32_t)MEM_R32(g_cpu, p + 12));
            std::fclose(f);
            MoveFileExA((d + "/state.tmp").c_str(), (d + "/state.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
    const std::string cmd = d + "/cmd.txt";
    for (;;) {
        SDL_PumpEvents();  // stay responsive while the tester thinks
        if (FILE* f = std::fopen(cmd.c_str(), "r")) {
            char buttons[128] = "";
            int frames = 0;
            const int n = std::fscanf(f, "%127s %d", buttons, &frames);
            std::fclose(f);
            std::remove(cmd.c_str());
            if (n >= 1 && !std::strcmp(buttons, "quit")) std::exit(0);
            if (n < 2 || frames < 1) continue;
            g_bot_buttons = 0;
            for (char* t = std::strtok(buttons, "+"); t; t = std::strtok(nullptr, "+")) g_bot_buttons |= button_bit(t);
            g_bot_until = g_frame + (uint64_t)frames;
            g_shot_frames.push_back(g_bot_until);
            g_shot_prefix = d + "/shot";
            return;
        }
        SDL_Delay(10);
    }
}

void host_present(const GpuDisplay& d) {
    bot_step();
    g_frame++;
    movie_vblank(d.enabled && d.rgb24 && gpu_movie_playing());
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
    static uint64_t dt_max_seen = 0;
    static int slow_seen = 0;
    if (last_present) {
        dt_min = std::min(dt_min, t_now - last_present);
        dt_max = std::max(dt_max, t_now - last_present);
        dt_max_seen = std::max(dt_max_seen, t_now - last_present);
        if (t_now - last_present > 50'000'000ull) slow_seen++;
    }
    last_present = t_now;
    if (++fps_frames == 60) {
        uint64_t now = SDL_GetTicksNS();
        double secs = (double)(now - fps_t0) / 1e9;
        double fps = 60.0 / secs, images = (double)(g_images - fps_images0) / secs;
        g_fps_shown = images;
        extern int g_audio_underruns, g_audio_drops, g_audio_min_queued;
        LOG(Host, "frame times: min %.2f ms, max %.2f ms; audio: queue min %d, underruns %d, drops %d",
            dt_min / 1e6, dt_max / 1e6, g_audio_min_queued, g_audio_underruns, g_audio_drops);
        g_audio_min_queued = 1 << 30;
        dt_min = ~0ull;
        dt_max = 0;
        char title[96];
        std::snprintf(title, sizeof title, "Apocalypse %s - %.0f fps%s", kVersion, images,
                      g_turbo ? " (fast-forward)" : "");
        // only when it changes: renaming the window goes through Windows' window manager and
        // could hold the frame up by several ms (a hiccup every second)
        static std::string shown_title;
        if (shown_title != title) {
            shown_title = title;
            SDL_SetWindowTitle(g_window, title);
        }
        LOG(Host, "frame %llu: %.1f Hz emulated, %.1f images/s", (unsigned long long)g_frame, fps, images);
        // once a minute in every log: the frame rate and slow moments of that minute
        static uint64_t minute_t0 = now, minute_images = g_images, minute_worst = 0;
        static int minute_slow = 0;
        minute_worst = std::max(minute_worst, dt_max_seen);
        minute_slow += slow_seen;
        dt_max_seen = 0;
        slow_seen = 0;
        if (now - minute_t0 >= 60'000'000'000ull) {
            const double m = (double)(now - minute_t0) / 1e9;
            log_msg(LogCat::Host, "performance: %.1f fps, slowest frame %.0f ms, %d frames over 50 ms (frame %llu)",
                    (double)(g_images - minute_images) / m, minute_worst / 1e6, minute_slow, (unsigned long long)g_frame);
            minute_t0 = now;
            minute_images = g_images;
            minute_worst = 0;
            minute_slow = 0;
        }
        fps_t0 = now;
        fps_frames = 0;
        fps_images0 = g_images;
    }
    g_last_display = d;
    if (render_backend() == Backend::Vulkan) {
        void vk_profile_frame(uint64_t frame);
        vk_profile_frame(g_frame);
    }
    if (g_multi_present && !g_turbo) return;  // host_throttle_frame shows it over several refreshes
    draw_game(d, 0.0);
}

// ================================================================ enhancement settings
// Settings changed from the menus: the settings screen (F1 -> Enhancements, ids 0..5) and the
// game's own Options -> Enhancements page (game_menu.cpp). Changes apply and save at once.

// Font scaling of the game's menus (percent, game_menu.cpp).
constexpr int kFontScaleChoices[] = {30, 40, 50, 60, 70, 80};
// Frame rates offered in the menus (30 = the original).
constexpr int kMenuFpsChoices[] = {30, 60};
// Resolution choices: render scale (lines = 240 x scale, the first at or above the named lines;
// the renderer works in whole multiples of the PS1's 240) and name.
constexpr int kResChoices[] = {1, 3, 4, 5};
const char* const kResNames[] = {"Original", "720", "800", "1080"};

// The Resolution choice for the current setting: the highest at or below it (a scale set in F1
// between choices, e.g. 8x = 1920 lines on a 1080p screen, shows as 1080).
// Display choices: resolution and aspect ratio together (the game's Enhancements page).
const struct { const char* name; int scale; Aspect aspect; } kDisplays[] = {
    {"320x240 (Original)", 1, Aspect::A4_3},     {"960x720", 3, Aspect::A4_3},
    {"1280x720 (Widescreen)", 3, Aspect::A16_9}, {"1280x800 (Steam Deck)", 4, Aspect::A16_10},
    {"1440x1080", 5, Aspect::A4_3},              {"1920x1080 (Widescreen)", 5, Aspect::A16_9},
};

// The Display choice for the current settings: same aspect ratio (16:10 for Stretch), the highest
// resolution at or below the setting (8x on a 1080p screen shows as 1080).
int display_choice() {
    const Aspect a = g_gfx.aspect == Aspect::Stretch ? Aspect::A16_10 : g_gfx.aspect;
    int k = -1;
    for (int i = 0; i < (int)std::size(kDisplays); i++)
        if (kDisplays[i].aspect == a && (k < 0 || kDisplays[i].scale <= g_gfx.scale)) k = i;
    return std::max(k, 0);
}

int res_choice() {
    int k = 0;
    for (int i = 0; i < (int)std::size(kResChoices); i++)
        if (kResChoices[i] <= g_gfx.scale) k = i;
    return k;
}

const char* enh_name(int id) {
    static const char* names[] = {"Frame rate", "Simulation", "Draw distance", "Precise 3D", "Smooth font",
                                  "Font scaling", "Renderer", "Screen mode", "Resolution", "Aspect ratio", "Display",
                                  "Autosave", "One-liners", "Anti-aliasing", "Infinite health", "Infinite lives",
                                  "All weapons", "Infinite ammo", "Infinite smart bombs", "One hit kill", "FPS counter", "Boot sequence", "CRT filter"};
    return id >= 0 && id < (int)std::size(names) ? names[id] : "";
}

std::string enh_value(int id) {
    switch (id) {
        case E_FPS: return g_gfx.fps == 30 ? "30 (original)" : fps_label();
        case E_SIM: return sim60_label();
        case E_DRAWDIST: return drawdist_label();
        case E_PRECISE: return g_gte_precise ? "On (no wobble)" : "Off (original)";
        case E_SMOOTH_FONT: return g_smooth_font ? "On" : "Off (original)";
        case E_FONT_SCALE: return std::to_string(g_menu_zoom) + "%";
        case E_RENDERER:
            return std::string(g_gfx.vulkan ? "Vulkan" : "OpenGL") +
                   ((g_gfx.vulkan == (render_backend() == Backend::Vulkan)) ? "" : " (restart)");
        case E_SCREEN: return g_fullscreen ? "Full" : "Window";
        case E_ASPECT: {
            static const char* names[] = {"Original", "Widescreen", "Widescreen (Steam Deck)", "Stretch"};  // 4:3, 16:9, 16:10
            return names[std::clamp((int)g_gfx.aspect, 0, 3)];
        }
        case E_RES: return kResNames[res_choice()];
        case E_DISPLAY: return kDisplays[display_choice()].name;
        case E_AUTOSAVE: return g_autosave ? "On" : "Off";
        case E_SUBTITLES: return g_subtitles ? "On" : "Off";
        case E_AA: return kAaNames[std::clamp(g_gfx.aa, 0, 3)];
        case E_FPS_COUNTER: return g_fps_counter ? "On" : "Off";
        case E_BOOT: return g_boot_sequence ? "On" : "Off";
        case E_CRT: return g_gfx.crt ? "On" : "Off";
        case E_CHEAT_HEALTH:
        case E_CHEAT_LIVES:
        case E_CHEAT_WEAPONS:
        case E_CHEAT_AMMO:
        case E_CHEAT_BOMBS:
        case E_CHEAT_ONEHIT: return cheat_on(id - E_CHEAT_HEALTH) ? "On" : "Off";
    }
    return "";
}

// The next (dir 1) or previous (dir -1) choice of setting `id`.
void enh_change(int id, int dir) {
    auto cycle = [dir](const int* list, int n, int cur) {
        int k = 0;
        while (k < n && list[k] != cur) k++;
        if (k == n) return list[dir > 0 ? 0 : n - 1];  // not one of the choices: start at an end
        return list[(k + dir + n) % n];
    };
    switch (id) {
        case E_FPS: g_gfx.fps = cycle(kMenuFpsChoices, (int)std::size(kMenuFpsChoices), g_gfx.fps); break;
        case E_SIM: {  // 60 Hz simulation (sim60.cpp)
            static const int order[] = {1, 0, 2};  // level 1, off, all levels
            g_sim60_setting = cycle(order, 3, g_sim60_setting);
            sim60_apply();
            break;
        }
        case E_DRAWDIST: g_draw_distance = cycle(kDrawDistChoices, (int)std::size(kDrawDistChoices), g_draw_distance); break;
        case E_PRECISE: g_gte_precise = !g_gte_precise; break;  // gte.cpp
        case E_SMOOTH_FONT: g_smooth_font = !g_smooth_font; break;  // gpu.cpp
        case E_FONT_SCALE: g_menu_zoom = cycle(kFontScaleChoices, (int)std::size(kFontScaleChoices), g_menu_zoom); break;  // game_menu.cpp
        case E_RENDERER: g_gfx.vulkan = !g_gfx.vulkan; break;  // takes effect when the game is started again
        case E_SCREEN:
            g_fullscreen = !g_fullscreen;
            if (g_window) SDL_SetWindowFullscreen(g_window, g_fullscreen);
            break;
        case E_ASPECT: {  // 4:3, 16:9, 16:10 (Stretch, from F1, goes to the next end)
            static const int order[] = {(int)Aspect::A4_3, (int)Aspect::A16_9, (int)Aspect::A16_10};
            g_gfx.aspect = (Aspect)cycle(order, 3, (int)g_gfx.aspect);
            break;
        }
        case E_AUTOSAVE: g_autosave = !g_autosave; break;  // savestate.cpp
        case E_SUBTITLES: g_subtitles = !g_subtitles; break;  // subtitles.cpp
        case E_AA: g_gfx.aa = (g_gfx.aa + dir + 4) % 4; break;
        case E_FPS_COUNTER: g_fps_counter = !g_fps_counter; break;
        case E_CRT: g_gfx.crt = !g_gfx.crt; break;  // present.cpp / present_vk.cpp
        case E_BOOT: g_boot_sequence = !g_boot_sequence; break;  // bios.cpp: from the next start
        case E_CHEAT_HEALTH:
        case E_CHEAT_LIVES:
        case E_CHEAT_WEAPONS:
        case E_CHEAT_AMMO:
        case E_CHEAT_BOMBS:
        case E_CHEAT_ONEHIT:  // cheats.cpp; never saved
            cheat_set(id - E_CHEAT_HEALTH, !cheat_on(id - E_CHEAT_HEALTH));
            return;
        case E_DISPLAY: {  // (supersampling, which renders more, is the Anti-aliasing row's)
            const int n = (int)std::size(kDisplays), k = (display_choice() + dir + n) % n;
            g_gfx.scale = kDisplays[k].scale;
            g_gfx.aspect = kDisplays[k].aspect;
            break;
        }
        case E_RES: {
            // the chosen lines exactly: supersampling (which renders more) is switched off
            const int n = (int)std::size(kResChoices);
            g_gfx.scale = kResChoices[(res_choice() + dir + n) % n];
            g_gfx.aa &= ~2;
            break;
        }
        default: return;
    }
    log_msg(LogCat::Host, "setting changed: %s = %s", enh_name(id), enh_value(id).c_str());
    apply_graphics();
    save_settings();
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

// A centred menu box over the game. lines[0] is the title; a '	' splits a line into
// a label and a value column. Sizes are in the overlay's 640x480 units.
void draw_panel(const std::vector<std::string>& lines, int highlight) {
    constexpr float kLine = 15.0f, kTitle = 21.0f, kPad = 12.0f, kGap = 26.0f;
    SDL_Renderer* r = g_renderer = present_overlay_begin();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    const int n = (int)lines.size();
    float label_w = 0, value_w = 0, whole_w = n ? ui_text_width(r, lines[0].c_str(), kTitle) : 0;
    for (int i = 1; i < n; i++) {
        const size_t tab = lines[i].find('	');
        if (tab == std::string::npos) {
            whole_w = std::max(whole_w, ui_text_width(r, lines[i].c_str(), kLine));
        } else {
            label_w = std::max(label_w, ui_text_width(r, lines[i].substr(0, tab).c_str(), kLine));
            value_w = std::max(value_w, ui_text_width(r, lines[i].c_str() + tab + 1, kLine));
        }
    }
    const float col = label_w > 0 ? label_w + kGap : 0;
    const float w = std::min(620.0f, std::max({whole_w, col + value_w, 300.0f}) + 2 * kPad);
    const float h = kPad + kTitle + 8 + kLine * (float)std::max(0, n - 1) + kPad;
    const float x0 = std::round((640 - w) * 0.5f), y0 = std::round(std::max(4.0f, (480 - h) * 0.5f));

    SDL_FRect box{x0, y0, w, h};
    SDL_SetRenderDrawColor(r, 14, 10, 8, 235);
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 200, 60, 20, 255);
    SDL_RenderRect(r, &box);
    if (n) {
        SDL_SetRenderDrawColor(r, 255, 170, 90, 255);
        ui_text(r, x0 + (w - ui_text_width(r, lines[0].c_str(), kTitle)) * 0.5f, y0 + kPad - 4, lines[0].c_str(), kTitle);
        SDL_SetRenderDrawColor(r, 200, 60, 20, 160);
        SDL_FRect rule{x0 + kPad, y0 + kPad + kTitle, w - 2 * kPad, 1};
        SDL_RenderFillRect(r, &rule);
    }
    for (int i = 1; i < n; i++) {
        const float y = y0 + kPad + kTitle + 8 + kLine * (float)(i - 1);
        if (i == highlight) {
            SDL_SetRenderDrawColor(r, 200, 60, 20, 255);
            SDL_FRect bar{x0 + kPad - 6, y, w - 2 * kPad + 12, kLine};
            SDL_RenderFillRect(r, &bar);
        }
        SDL_SetRenderDrawColor(r, 255, 230, 180, 255);
        const size_t tab = lines[i].find('	');
        ui_text(r, x0 + kPad, y, lines[i].substr(0, tab).c_str(), kLine);
        if (tab != std::string::npos) ui_text(r, x0 + kPad + col, y, lines[i].c_str() + tab + 1, kLine);
    }
    present_redraw(present_options());
    present_overlay_end();
    // tests: APOC_MENU_SHOT=path.bmp saves menu image number APOC_MENU_SHOT_N (default the first)
    if (static const char* shot = std::getenv("APOC_MENU_SHOT"); shot && *shot) {
        static int n = 0, want = std::getenv("APOC_MENU_SHOT_N") ? std::atoi(std::getenv("APOC_MENU_SHOT_N")) : 0;
        if (n++ == want) {
            present_save_bmp(shot);
            shot = "";
        }
    }
    present_swap();
}

enum class MenuResult { Continue, Close, Quit };

// Pumps events for the settings screen; returns key presses as SDL scancodes.
MenuResult pump_menu_events(std::vector<SDL_Scancode>& keys) {
    // tests: APOC_MENU_KEYS="d,d,d,e,r,e" (up/down/left/right/enter/x = escape), one key per pass
    if (static const char* mk = std::getenv("APOC_MENU_KEYS"); mk && *mk) {
        const char k = *mk;
        mk += mk[1] == ',' ? 2 : 1;
        keys.push_back(k == 'u' ? SDL_SCANCODE_UP : k == 'd' ? SDL_SCANCODE_DOWN : k == 'l' ? SDL_SCANCODE_LEFT
                       : k == 'r' ? SDL_SCANCODE_RIGHT : k == 'x' ? SDL_SCANCODE_ESCAPE : SDL_SCANCODE_RETURN);
    }
    SDL_Event e;
    mouse_autohide(nullptr);
    while (SDL_PollEvent(&e)) {
        mouse_autohide(&e);
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

// A pad press as a menu key. The default keyboard keys also drive the pad (arrows = d-pad),
// so a key already seen as a key event this pass is not added again (it would move twice).
static void push_pad_key(std::vector<SDL_Scancode>& keys, SDL_Scancode k) {
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
}

// The chapter list (F1 -> Chapter, Enter): every chapter kept so far, in the order reached.
// Up/down move, left/right jump to the previous/next level, Enter loads, Esc goes back.
// Returns true (with the chosen index in *chapter) to load it.
static bool pick_chapter(const std::vector<std::string>& chapters, int* chapter, bool* quit) {
    const int n = (int)chapters.size();
    auto level_of = [&](int i) {  // "12: Rat (sewr)" -> "(sewr)"
        const size_t o = chapters[i].rfind('(');
        return o == std::string::npos ? std::string() : chapters[i].substr(o);
    };
    auto level_start = [&](int i) {
        while (i > 0 && level_of(i - 1) == level_of(i)) i--;
        return i;
    };
    int cur = std::clamp(*chapter, 0, n - 1);
    uint16_t prev_pad = host_pad(0).buttons;
    for (;;) {
        std::vector<SDL_Scancode> keys;
        if (pump_menu_events(keys) == MenuResult::Quit) {
            *quit = true;
            return false;
        }
        const uint16_t pad = host_pad(0).buttons, pressed = (uint16_t)(prev_pad & ~pad);
        prev_pad = pad;
        if (pressed & B_UP) push_pad_key(keys, SDL_SCANCODE_UP);
        if (pressed & B_DOWN) push_pad_key(keys, SDL_SCANCODE_DOWN);
        if (pressed & (B_LEFT | B_L1)) push_pad_key(keys, SDL_SCANCODE_LEFT);
        if (pressed & (B_RIGHT | B_R1)) push_pad_key(keys, SDL_SCANCODE_RIGHT);
        if (pressed & (B_CROSS | B_START)) push_pad_key(keys, SDL_SCANCODE_RETURN);
        if (pressed & B_CIRCLE) push_pad_key(keys, SDL_SCANCODE_ESCAPE);
        for (auto k : keys) {
            if (k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_F1 || k == SDL_SCANCODE_F2) return false;
            if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) {
                *chapter = cur;
                return true;
            }
            if (k == SDL_SCANCODE_UP) cur = (cur + n - 1) % n;
            if (k == SDL_SCANCODE_DOWN) cur = (cur + 1) % n;
            if (k == SDL_SCANCODE_RIGHT) {  // the first chapter of the next level (after the last: the first)
                int i = cur;
                while (i < n && level_of(i) == level_of(cur)) i++;
                cur = i < n ? i : 0;
            }
            if (k == SDL_SCANCODE_LEFT) {  // the start of this level, or of the one before
                const int s0 = level_start(cur);
                cur = cur != s0 ? s0 : level_start(s0 > 0 ? s0 - 1 : n - 1);
            }
        }
        constexpr int kShown = 16;
        const int first = std::clamp(cur - kShown / 2, 0, std::max(0, n - kShown));
        std::vector<std::string> lines = {"CHAPTERS", ""};
        for (int i = first; i < std::min(n, first + kShown); i++) lines.push_back(" " + chapters[i].substr(0, 35));
        lines.push_back("");
        lines.push_back("Up/Down: choose  Left/Right: level");
        lines.push_back("Enter/X: load    Esc/O: back");
        draw_panel(lines, 2 + cur - first);
        SDL_Delay(10);
    }
}

static int g_chapter_sel = 0;  // the chapter last chosen (shared by F1 -> Chapter and F2)

// F2: the chapter list on its own, straight from the game. Paused like the settings screen.
struct MenuOpen {  // (core.cpp watchdog: the game stops on purpose while a menu is open)
    MenuOpen() { g_host_menu_open = true; }
    ~MenuOpen() { g_host_menu_open = false; }
};

static void run_chapter_menu() {
    MenuOpen open_flag;
    const std::vector<std::string> chapters = state_chapter_names();
    if (chapters.empty()) return;
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    if (g_chapter_sel >= (int)chapters.size()) g_chapter_sel = 0;
    bool quit = false;
    if (pick_chapter(chapters, &g_chapter_sel, &quit)) state_request_load(kChapterBase + g_chapter_sel);
    if (quit) g_quit = true;
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    g_next_frame_ns = SDL_GetTicksNS();  // don't try to "catch up" the paused time
}

// Modal settings screen. The game is paused while it is open (this runs inside the
// emulated VBlank), and audio is paused with it.
static void run_settings_menu() {
    MenuOpen open_flag;
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    enum Row { R_SAVE, R_LOAD, R_LOAD_AUTO, R_CHAPTER, R_RENDERER, R_RES, R_AA, R_MSAA, R_ASPECT, R_CRT, R_FILTER,
               R_DUMP_TEX, R_TEX_REPLACE, R_ENHANCE, R_DEVICE, R_REMAP, R_RESET, R_BACK,
               R_FPS, R_SIM, R_DRAWDIST, R_PRECISE, R_FONT, R_MENU_SIZE, R_FPS_COUNTER, R_BOOT, R_ENH_BACK };  // R_FPS.. = EnhSetting E_FPS..
    // the main page, and the Enhancements page (what goes beyond the original game)
    static const std::vector<Row> kMainRows = {R_SAVE, R_LOAD, R_LOAD_AUTO, R_CHAPTER, R_RENDERER, R_RES, R_AA, R_MSAA,
                                               R_ASPECT, R_CRT, R_FILTER, R_DUMP_TEX, R_TEX_REPLACE, R_ENHANCE, R_DEVICE, R_REMAP, R_RESET, R_BACK};
    static const std::vector<Row> kEnhRows = {R_FPS, R_FPS_COUNTER, R_SIM, R_DRAWDIST, R_PRECISE, R_FONT, R_MENU_SIZE, R_BOOT, R_ENH_BACK};
    bool enh_page = false;
    static int slot = 1;
    int& chapter = g_chapter_sel;
    const std::vector<std::string> chapters = state_chapter_names();
    if (chapter >= (int)chapters.size()) chapter = 0;
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
        if (pressed & B_UP) push_pad_key(keys, SDL_SCANCODE_UP);
        if (pressed & B_DOWN) push_pad_key(keys, SDL_SCANCODE_DOWN);
        if (pressed & B_LEFT) push_pad_key(keys, SDL_SCANCODE_LEFT);
        if (pressed & B_RIGHT) push_pad_key(keys, SDL_SCANCODE_RIGHT);
        if (pressed & (B_CROSS | B_START)) push_pad_key(keys, SDL_SCANCODE_RETURN);
        if (pressed & B_CIRCLE) push_pad_key(keys, SDL_SCANCODE_ESCAPE);

        // controls device: entry 0 is the keyboard, entries 1.. are the connected controllers
        const size_t entries = 1 + g_devices.size();
        if (dev_index >= entries) dev_index = 0;
        const bool is_keyboard = dev_index == 0;
        bool gfx_changed = false;
        auto open_page = [&](bool enh) {  // back to the main page lands on "Enhancements"
            sel = enh ? 0 : (int)(std::find(kMainRows.begin(), kMainRows.end(), R_ENHANCE) - kMainRows.begin());
            enh_page = enh;
            status = "";
        };
        for (auto k : keys) {
            const std::vector<Row>& rows = enh_page ? kEnhRows : kMainRows;
            const int count = (int)rows.size();
            int dir = k == SDL_SCANCODE_LEFT ? -1 : k == SDL_SCANCODE_RIGHT ? 1 : 0;
            if (k == SDL_SCANCODE_UP) sel = (sel + count - 1) % count;
            if (k == SDL_SCANCODE_DOWN) sel = (sel + 1) % count;
            if (k == SDL_SCANCODE_F1) open = false;
            if (k == SDL_SCANCODE_ESCAPE) {
                if (enh_page) open_page(false);
                else open = false;
                continue;
            }
            bool enter = k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER;
            const Row row = rows[sel];
            if (dir || enter) {
                int step = dir ? dir : 1;
                switch (row) {
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
                    case R_DUMP_TEX: g_tex_dump = !g_tex_dump; break;  // texdump.cpp (not saved)
                    case R_TEX_REPLACE:  // texreplace.cpp: assets/replacements/textures
                        g_tex_replace = !g_tex_replace;
                        texreplace_vram_changed();
                        save_settings();
                        break;
                    case R_FPS:
                    case R_SIM:
                    case R_DRAWDIST:
                    case R_PRECISE:
                    case R_FONT:
                    case R_MENU_SIZE:
                        enh_change(row - R_FPS, step);
                        gfx_changed = true;
                        break;
                    case R_BOOT:
                        enh_change(E_BOOT, step);
                        gfx_changed = true;
                        break;
                    case R_FPS_COUNTER:
                        enh_change(E_FPS_COUNTER, step);
                        gfx_changed = true;
                        break;
                    case R_CHAPTER:
                        if (dir && !chapters.empty()) chapter = (chapter + step + (int)chapters.size()) % (int)chapters.size();
                        break;
                    case R_DEVICE: dev_index = (dev_index + entries + step) % entries; break;
                    case R_SAVE:
                    case R_LOAD:
                        if (dir) slot = (slot - 1 + step + kStateSlots) % kStateSlots + 1;
                        break;
                    default: break;
                }
            }
            if (!enter) continue;
            if (row == R_BACK) {
                open = false;
            } else if (row == R_ENHANCE || row == R_ENH_BACK) {
                open_page(row == R_ENHANCE);
            } else if (row == R_SAVE) {
                state_request_save(slot);  // taken at the next frame, once the menu closes
                open = false;
            } else if (row == R_LOAD || row == R_LOAD_AUTO) {
                int s = row == R_LOAD ? slot : kAutoSlot;
                if (state_slot_exists(s)) {
                    state_request_load(s);
                    open = false;
                } else {
                    status = "That slot is empty.";
                }
            } else if (row == R_CHAPTER) {
                if (chapters.empty()) {
                    status = "Chapters appear as you reach checkpoints.";
                } else if (pick_chapter(chapters, &chapter, &quit)) {
                    state_request_load(kChapterBase + chapter);
                    open = false;
                }
                prev_pad = host_pad(0).buttons;
            } else if (row == R_REMAP && is_keyboard) {
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
            } else if (row == R_REMAP) {
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
            } else if (row == R_RESET) {
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
        auto choice = [](const char* label, const std::string& value) { return std::string(label) + "\t< " + value + " >"; };
        std::string slot_line = std::to_string(slot) + ": " + state_slot_label(slot);
        auto line = [&](Row r) -> std::string {
            switch (r) {
                case R_SAVE: return choice("Save state", slot_line);
                case R_LOAD: return choice("Load state", slot_line);
                case R_LOAD_AUTO: return choice("Load auto-save", state_slot_label(kAutoSlot));
                case R_CHAPTER: return choice("Chapter", chapters.empty() ? std::string("none yet") : chapters[chapter].substr(0, 26));
                case R_RENDERER:
                    return choice("Renderer", std::string(g_gfx.vulkan ? "Vulkan" : "OpenGL") +
                                                  ((g_gfx.vulkan == (render_backend() == Backend::Vulkan)) ? "" : " (restart)"));
                case R_RES: return choice("Resolution", res);
                case R_AA: return choice("Anti-aliasing", kAaNames[g_gfx.aa]);
                case R_MSAA: return choice("AA rate (MSAA)", msaa_label());
                case R_ASPECT: return choice("Aspect ratio", kAspect[(int)g_gfx.aspect]);
                case R_CRT: return choice(enh_name(E_CRT), enh_value(E_CRT));
                case R_FILTER: return choice("Smooth textures", g_gfx.texture_filter ? "On" : "Off");
                case R_DUMP_TEX: return choice("Dump textures", g_tex_dump ? "On (assets/dumps/textures)" : "Off");
                case R_TEX_REPLACE: return choice("Texture replacements", g_tex_replace ? "[x] On" : "[ ] Off (original)");
                case R_ENHANCE: return "Enhancements\t>";
                case R_DEVICE: return choice("Controls", dev_line);
                case R_REMAP: return std::string("Set up buttons\t") + (custom ? "(custom)" : "(default)");
                case R_RESET: return "Reset buttons to default";
                case R_BACK: return "Back to game";
                case R_FPS:
                case R_SIM:
                case R_DRAWDIST:
                case R_PRECISE:
                case R_FONT:
                case R_MENU_SIZE: return choice(enh_name(r - R_FPS), enh_value(r - R_FPS));
                case R_FPS_COUNTER: return choice(enh_name(E_FPS_COUNTER), enh_value(E_FPS_COUNTER));
                case R_BOOT: return choice(enh_name(E_BOOT), enh_value(E_BOOT));
                case R_ENH_BACK: return "Back";
            }
            return "";
        };
        std::vector<std::string> lines = {enh_page ? "ENHANCEMENTS" : "SETTINGS"};
        for (Row r : enh_page ? kEnhRows : kMainRows) lines.push_back(line(r));
        lines.push_back(status);
        if (enh_page) {
            lines.push_back("Beyond the original game. Also in the game's");
            lines.push_back("own menu: Options -> Enhancements.");
        }
        lines.push_back("Arrows: change   Enter: pick   Esc: back");
        if (!enh_page) lines.push_back("F2: chapters   F5/F9: save/load");
        draw_panel(lines, 1 + sel);
        SDL_Delay(15);
    }
    if (quit) g_quit = true;
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    g_next_frame_ns = SDL_GetTicksNS();  // don't try to "catch up" the paused time
}

void host_throttle_frame() {
    if (static const bool on = std::getenv("APOC_FRAME_TIMES") != nullptr; on) {  // debug: frames over N ms (default 20)
        // with where the time went: waiting for the display (acquire/present), waiting for the
        // GPU (fences), GPU work (Vulkan timestamps); the rest is the game's code and ours
        extern uint64_t g_prof_fence_ns, g_prof_present_ns, g_prof_gpu_ns;
        static uint64_t last = 0, p0 = 0, f0 = 0, g0 = 0;
        const uint64_t now = SDL_GetTicksNS();
        auto d = [](uint64_t v, uint64_t v0) { return v >= v0 ? (double)(v - v0) / 1e6 : 0.0; };  // (reset by APOC_PROFILE)
        static const uint64_t limit = (uint64_t)(std::max(0.0, std::atof(std::getenv("APOC_FRAME_TIMES"))) * 1e6) ?: 20000000ull;
        if (last && now - last > limit) {
            const double total = (double)(now - last) / 1e6, pr = d(g_prof_present_ns, p0), fe = d(g_prof_fence_ns, f0);
            std::fprintf(stderr, "SLOW frame %llu: %.1f ms (display %.1f, GPU wait %.1f, GPU work %.1f, CPU %.1f)\n",
                         (unsigned long long)g_frame, total, pr, fe, d(g_prof_gpu_ns, g0), total - pr - fe);
        }
        last = now, p0 = g_prof_present_ns, f0 = g_prof_fence_ns, g0 = g_prof_gpu_ns;
    }
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
        if (g_present_vsync && !g_present_timed) {
            // As many display refreshes as fit before the next vblank is due (each swap
            // waits for the display).
            const uint64_t refresh = (uint64_t)(1e9 / g_refresh_hz);
            for (bool drawn = false;; drawn = true) {
                now = SDL_GetTicksNS();
                if (drawn && now + refresh / 2 > g_next_frame_ns) break;
                draw_game(g_last_display, frac_at(now));
            }
        } else {
            // The image rate is above the display refresh, or does not line up with it: present
            // on our own clock.
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


void host_toast(const std::string& text) {
    g_toast = text;
    g_toast_until = SDL_GetTicksNS() + 2500000000ull;
    log_msg(LogCat::Host, "%s", text.c_str());
}

void host_audio_clear() {
    if (g_audio) SDL_ClearAudioStream(g_audio);
}