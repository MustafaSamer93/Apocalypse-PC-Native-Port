// Settings: settings.ini (loaded at start, saved at every change) and the enhancement options
// shared by the settings screen and the game's own Recomp Enhancements page.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include "core/psx.h"
#include "core/savestate.h"
#include "game/game.h"
#include "host/host.h"
#include "host/host_internal.h"
#include "render/gpu_hw.h"
#include "render/render_backend.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace host {

GfxSettings g_gfx;
int g_active_scale = -1;
bool g_fullscreen = true;  // start in fullscreen
bool g_fps_counter = false;
const char* const kAaNames[4] = {"Off", "FXAA", "SSAA", "SSAA + FXAA"};

namespace {

int g_active_msaa = 1;  // MSAA samples the renderer was created with

// Choices offered in the menus.
// percent of the game's own (drawdist.cpp): Original, Medium (2x), High (the whole depth range)
constexpr int kDrawDistChoices[] = {100, 200, 400};
constexpr int kFontScaleChoices[] = {30, 40, 50, 60, 70, 80};  // the game's menus (game_menu.cpp)
constexpr int kMenuFpsChoices[] = {30, 60, 75, 90, 120};        // 30 = the original; above 60: in-between images (interp.cpp)
// Display choices: resolution and aspect ratio together (the game's Enhancements page).
const struct { const char* name; int scale; Aspect aspect; } kDisplays[] = {
    {"320x240 (Original)", 1, Aspect::A4_3},     {"960x720", 3, Aspect::A4_3},
    {"1280x720 (Widescreen)", 3, Aspect::A16_9}, {"1280x800 (Steam Deck)", 4, Aspect::A16_10},
    {"1440x1080", 5, Aspect::A4_3},              {"1920x1080 (Widescreen)", 5, Aspect::A16_9},
    {"2560x1080 (Ultra-wide)", 5, Aspect::A21_9}, {"3840x1080 (Super ultra-wide)", 5, Aspect::A32_9},
    {"2560x1440 (Widescreen)", 6, Aspect::A16_9}, {"3440x1440 (Ultra-wide)", 6, Aspect::A21_9},
    {"5120x1440 (Super ultra-wide)", 6, Aspect::A32_9}, {"2880x2160", 9, Aspect::A4_3},
    {"3840x2160 (4K)", 9, Aspect::A16_9},
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

std::string drawdist_label() {
    return g_draw_distance <= 100 ? "Original" : g_draw_distance < 400 ? "Medium" : "High";
}

// Internal resolution actually rendered: the Resolution setting, raised by supersampling to
// about twice the lines the window shows (at most 8x = 1920 lines).
int render_scale() {
    if (g_gfx.scale == 0 || !(g_gfx.aa & 2) || !g_window) return g_gfx.scale;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    return std::clamp((2 * h + 239) / 240, g_gfx.scale, std::max(g_gfx.scale, 8));
}

}  // namespace

const char* settings_file() {
    static const char* f = std::getenv("APOC_SETTINGS") ? std::getenv("APOC_SETTINGS") : "settings.ini";
    return f;
}

void apply_graphics() {
    // "30 (original)": the game as it was, so no 60 Hz simulation either (sim60.cpp)
    if (g_sim60_fps30 != (g_gfx.fps == 30)) {
        g_sim60_fps30 = g_gfx.fps == 30;
        sim60_apply();
    }
    const int want = render_scale();
    if (g_video_ok && (want != g_active_scale || g_gfx.msaa != g_active_msaa)) {
        g_active_scale = gpu_set_renderer(want, g_gfx.msaa);
        g_active_msaa = g_gfx.msaa;
    }
    if (!g_video_ok) g_active_scale = 0;
    hw_set_texture_filter(g_gfx.texture_filter);
    psx_opt_menu60 = g_gfx.fps != 30;  // the menus at 60 Hz too (game_menu.cpp slows what they animate)
    const float ratio = aspect_ratio(g_gfx.aspect);  // (4/3) / ratio: 16:9 = 0.75, 32:9 = 0.375
    g_gte_widescreen = ratio > 0 ? (4.0f / 3.0f) / ratio : 1.0f;
}

// The multisampling rate for the settings screen (with what the GPU could give).
std::string msaa_label() {
    std::string s = std::to_string(g_gfx.msaa) + "x" + (g_gfx.msaa == 1 ? " (off)" : "");
    int got = hw_samples();
    if (g_gfx.msaa > 1 && got > 0 && got < g_gfx.msaa) s += ", " + std::to_string(got) + "x fits";
    return s;
}

// ================================================================ settings.ini

// Written to a new file, flushed to the disk and then put in place of the old one: a crash or
// power loss while saving leaves the old settings, never a file of zeros.
void save_settings() {
    const std::string tmp = std::string(settings_file()) + ".new";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# Written by the game's settings screen. Safe to delete to reset.\n");
    std::fprintf(f, "gfx scale=%d aspect=%d crt=%d filter=%d fps=%d\n", g_gfx.scale, (int)g_gfx.aspect,
                 g_gfx.crt ? 1 : 0, g_gfx.texture_filter ? 1 : 0, g_gfx.fps);
    std::fprintf(f, "drawdist %d\n", g_draw_distance);
    std::fprintf(f, "gamefont %d\n", g_game_font ? 1 : 0);
    std::fprintf(f, "font %d\n", g_smooth_font ? 1 : 0);
    std::fprintf(f, "menuzoom %d\n", g_menu_zoom);
    std::fprintf(f, "autosave %d\n", g_autosave ? 1 : 0);
    std::fprintf(f, "subtitles %d\n", g_subtitles ? 1 : 0);
    std::fprintf(f, "fpscounter %d\n", g_fps_counter ? 1 : 0);
    std::fprintf(f, "texreplace %d\n", g_tex_replace ? 1 : 0);
    std::fprintf(f, "fullscreen %d\n", g_fullscreen ? 1 : 0);
    std::fprintf(f, "aa %d\n", g_gfx.aa);
    std::fprintf(f, "msaa %d\n", g_gfx.msaa);
    std::fprintf(f, "renderer %s\n", g_gfx.vulkan ? "vulkan" : "opengl");
    if (!g_input.empty()) std::fprintf(f, "input %s\n", g_input.c_str());
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
#else
    if (ok) fsync(fileno(f));
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
    // "name N" lines for on/off and number settings
    const struct { const char* name; bool* flag; int* value; int lo, hi; } kSimple[] = {
        {"fullscreen", &g_fullscreen}, {"font", &g_smooth_font}, {"texreplace", &g_tex_replace},
        {"fpscounter", &g_fps_counter}, {"subtitles", &g_subtitles}, {"autosave", &g_autosave},
        {"gamefont", &g_game_font},
        {"menuzoom", nullptr, &g_menu_zoom, 30, 80}, {"drawdist", nullptr, &g_draw_distance, 100, 400},
        {"aa", nullptr, &g_gfx.aa, 0, 3},
    };
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        int sc, as, crt, filt, fps;
        if (int got = std::sscanf(line, "gfx scale=%d aspect=%d crt=%d filter=%d fps=%d", &sc, &as, &crt, &filt, &fps);
            got >= 4) {
            g_gfx.scale = std::clamp(sc, 0, 9);
            g_gfx.aspect = (Aspect)std::clamp(as, 0, 5);
            g_gfx.crt = crt != 0;
            g_gfx.texture_filter = filt != 0;
            if (got == 5) g_gfx.fps = fps == 30 || fps == 75 || fps == 90 || fps == 120 ? fps : 60;
            continue;
        }
        char name[16];
        int v, used = 0;
        if (std::sscanf(line, "%15s %d%n", name, &v, &used) == 2) {
            bool found = false;
            for (const auto& s : kSimple) {
                if (std::strcmp(name, s.name) != 0) continue;
                if (s.flag) *s.flag = v != 0;
                else *s.value = std::clamp(v, s.lo, s.hi);
                found = true;
            }
            if (!std::strcmp(name, "msaa")) g_gfx.msaa = v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 1, found = true;
            if (found) continue;
        }
        if (char rn[16]; std::sscanf(line, "renderer %15s", rn) == 1) {
            g_gfx.vulkan = std::strcmp(rn, "vulkan") == 0;
            continue;
        }
        if (char in[64]; std::sscanf(line, "input %63s", in) == 1) {
            g_input = in;
            continue;
        }
        if (std::strncmp(line, "keys", 4) == 0) {
            g_key_custom = true;
            const char* p = line + 4;
            int bit, key;
            while (std::sscanf(p, " %d=%d%n", &bit, &key, &used) == 2) {
                if (bit >= 0 && bit < 16) g_keys[bit] = (SDL_Scancode)key;
                p += used;
            }
            continue;
        }
        char guid[64];
        int n = 0;
        if (std::sscanf(line, "pad %63s%n", guid, &n) != 1) continue;
        Layout lay;
        const char* p = line + n;
        int bit, idx, val;
        char kind, sign;
        for (;;) {
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
    // older settings files had 150 / 300 too: the nearest of the three
    g_draw_distance = g_draw_distance < 150 ? 100 : g_draw_distance < 300 ? 200 : 400;
}

}  // namespace host

// ================================================================ enhancement settings

using namespace host;

const char* enh_name(int id) {
    static const char* names[] = {"Frame rate", "Draw distance", "Smooth font",
                                  "Font upscaling", "Renderer", "Screen mode", "Display", "Autosave", "One-liners",
                                  "Anti-aliasing", "Infinite health", "Infinite lives", "Unlock all weapons", "Infinite ammo",
                                  "Infinite smart bombs", "One hit kill", "FPS counter", "CRT filter", "Font type", "Smooth textures", "HD replacements"};
    static_assert(std::size(names) == E_REPLACE + 1, "one name per EnhSetting");
    return id >= 0 && id < (int)std::size(names) ? names[id] : "";
}

std::string enh_value(int id) {
    switch (id) {
        case E_FPS: return g_gfx.fps == 30 ? "30 original" : std::to_string(g_gfx.fps);
        case E_DRAWDIST: return drawdist_label();
        case E_SMOOTH_FONT: return g_smooth_font ? "On" : "Off (original)";
        case E_FONT_SCALE: return std::to_string(g_menu_zoom) + "%";
        case E_RENDERER:
            return std::string(g_gfx.vulkan ? "Vulkan" : "OpenGL") +
                   ((g_gfx.vulkan == (render_backend() == Backend::Vulkan)) ? "" : " (restart)");
        case E_SCREEN: return g_fullscreen ? "Full" : "Window";
        case E_DISPLAY: return kDisplays[display_choice()].name;
        case E_AUTOSAVE: return g_autosave ? "On" : "Off";
        case E_SUBTITLES: return g_subtitles ? "On" : "Off";
        case E_AA: return kAaNames[std::clamp(g_gfx.aa, 0, 3)];
        case E_FPS_COUNTER: return g_fps_counter ? "On" : "Off";
        case E_CRT: return g_gfx.crt ? "On" : "Off";
        case E_TEX_SMOOTH: return g_gfx.texture_filter ? "On" : "Off (original)";
        case E_REPLACE: return g_tex_replace ? "On" : "Off (original)";
        case E_GAME_FONT: {
            if (texreplace_font_name().empty()) return "Original (no font in assets/fonts)";
            return g_game_font ? "Remastered" : "Original";
        }
        case E_CHEAT_HEALTH:
        case E_CHEAT_LIVES:
        case E_CHEAT_WEAPONS:
        case E_CHEAT_AMMO:
        case E_CHEAT_BOMBS:
        case E_CHEAT_ONEHIT: return cheat_on(id - E_CHEAT_HEALTH) ? "On" : "Off";
    }
    return "";
}

void enh_change(int id, int dir) {
    auto cycle = [dir](const int* list, int n, int cur) {
        int k = 0;
        while (k < n && list[k] != cur) k++;
        if (k == n) return list[dir > 0 ? 0 : n - 1];  // not one of the choices: start at an end
        return list[(k + dir + n) % n];
    };
    switch (id) {
        case E_FPS: g_gfx.fps = cycle(kMenuFpsChoices, (int)std::size(kMenuFpsChoices), g_gfx.fps); break;
        case E_DRAWDIST: g_draw_distance = cycle(kDrawDistChoices, (int)std::size(kDrawDistChoices), g_draw_distance); break;
        case E_SMOOTH_FONT: g_smooth_font = !g_smooth_font; break;
        case E_FONT_SCALE: g_menu_zoom = cycle(kFontScaleChoices, (int)std::size(kFontScaleChoices), g_menu_zoom); break;
        case E_RENDERER: g_gfx.vulkan = !g_gfx.vulkan; break;  // takes effect when the game is started again
        case E_SCREEN:
            g_fullscreen = !g_fullscreen;
            if (g_window) SDL_SetWindowFullscreen(g_window, g_fullscreen);
            break;
        case E_DISPLAY: {  // (supersampling, which renders more, is the Anti-aliasing row's)
            const int n = (int)std::size(kDisplays), k = (display_choice() + dir + n) % n;
            g_gfx.scale = kDisplays[k].scale;
            g_gfx.aspect = kDisplays[k].aspect;
            break;
        }
        case E_AUTOSAVE: g_autosave = !g_autosave; break;
        case E_SUBTITLES: g_subtitles = !g_subtitles; break;
        case E_AA: g_gfx.aa = (g_gfx.aa + dir + 4) % 4; break;
        case E_FPS_COUNTER: g_fps_counter = !g_fps_counter; break;
        case E_CRT: g_gfx.crt = !g_gfx.crt; break;
        case E_TEX_SMOOTH: g_gfx.texture_filter = !g_gfx.texture_filter; break;  // (apply_graphics)
        case E_REPLACE:  // textures and pictures (texreplace.cpp, replace.cpp) and movies (movie.cpp)
            g_tex_replace = !g_tex_replace;
            texreplace_vram_changed();
            break;
        case E_GAME_FONT:
            g_game_font = !g_game_font;
            texreplace_font_changed();
            break;
        case E_CHEAT_HEALTH:
        case E_CHEAT_LIVES:
        case E_CHEAT_WEAPONS:
        case E_CHEAT_AMMO:
        case E_CHEAT_BOMBS:
        case E_CHEAT_ONEHIT:  // never saved
            cheat_set(id - E_CHEAT_HEALTH, !cheat_on(id - E_CHEAT_HEALTH));
            return;
        default: return;
    }
    log_msg(LogCat::Host, "setting changed: %s = %s", enh_name(id), enh_value(id).c_str());
    apply_graphics();
    save_settings();
}
