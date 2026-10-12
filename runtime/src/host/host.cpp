// Host platform layer: the SDL3 window and renderer start-up, presenting frames, frame pacing,
// audio and on-screen messages.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "core/psx.h"
#include "core/savestate.h"
#include "game/game.h"
#include "host/host.h"
#include "host/host_internal.h"
#include "render/gpu_hw.h"
#include "render/present.h"
#include "render/render_backend.h"
#include "render/ui_text.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

constexpr const char* kVersion = "v1.0";
int g_renderer_override = -1;
bool g_low_lag = true;
int g_start_state = -1;
std::vector<uint64_t> g_shot_frames;
std::string g_shot_prefix;

namespace host {

SDL_Window* g_window = nullptr;
SDL_AudioStream* g_audio = nullptr;
bool g_video_ok = false;
bool g_quit = false;
uint64_t g_frame = 0;
uint64_t g_next_frame_ns = 0;

namespace {

constexpr uint64_t FRAME_NS = 1000000000ull * 1001 / 60000;  // 59.94 Hz
float g_refresh_hz = 60.0f;    // display refresh rate
bool g_turbo = false;          // fast-forward (Tab held, or --turbo)
bool g_force_turbo = false;
bool g_vsync_pacing = false;   // display is ~60 Hz: the swap paces the frames
bool g_vsync_active = false;   // current vsync state (off while fast-forwarding)
std::vector<uint32_t> g_pixels;

// Audio playback health, reported with the frame times (--log host).
int g_audio_underruns = 0, g_audio_drops = 0, g_audio_min_queued = 1 << 30;

// Short on-screen messages ("State 1 saved").
std::string g_toast;
uint64_t g_toast_until = 0;

double g_fps_shown = 0;        // images per second over the last second (host_present)
uint64_t g_images = 0;         // distinct images shown
uint64_t g_last_image = ~0ull;

// ================================================================ overlays

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

// Over the game image: the screenshot first (the game alone), then the toast, the version (on
// the title screen and while the game's menus are up) and the FPS counter; then the swap.
void finish_frame(bool version) {
    for (uint64_t f : g_shot_frames)
        if (f == g_frame) present_save_bmp((g_shot_prefix + "_" + std::to_string(f) + ".bmp").c_str());
    draw_toast();
    if (version && (gpu_title_screen() || replace_title_shown(gpu_display().y)))
        draw_badge(1, kVersion, (int)std::strlen(kVersion) + 1, true);
    if (g_fps_counter) draw_badge(0, std::to_string((int)std::lround(g_fps_shown)) + " FPS", 7, false);
    present_swap();
}

// ================================================================ the game image

// Lines to cut off at the top and at the bottom of an image `h` lines high: in widescreen a
// movie (made for 4:3, with black bands in the picture) is enlarged to the full width, keeping
// its proportions, and loses what no longer fits (1/8 at each edge for 16:9).
int movie_crop(int h) {
    if (g_gfx.aspect == Aspect::A4_3 || g_gfx.aspect == Aspect::Stretch || !gpu_movie_playing()) return 0;
    return (int)((float)h * (1.0f - std::max(g_gte_widescreen, 0.75f)) * 0.5f);  // (ultra-wide: as 16:9)
}

// Draws the game image, then the overlays, and swaps.
void draw_game(const GpuDisplay& d) {
    const int w = std::clamp(d.w, 1, 1024), h = std::clamp(d.h, 1, 512);
    const PresentOptions opt = present_options();
    const int crop = movie_crop(h);
    // A replacement movie (movie.cpp) in place of the game's own movie picture, cropped like it.
    if (int mw = 0, mh = 0; d.enabled && d.rgb24 && gpu_movie_playing()) {
        if (const uint32_t* px = movie_picture(&mw, &mh)) {
            const int mc = (int)((int64_t)crop * mh / h);
            present_game_pixels(px + (size_t)mc * mw, mw, mh - 2 * mc, opt);
            if (g_last_image != (uint64_t)(uintptr_t)px) g_images++;  // (each new movie frame)
            g_last_image = (uint64_t)(uintptr_t)px;
            finish_frame(false);
            return;
        }
    }
    if (g_display_flips != g_last_image) g_images++;
    g_last_image = g_display_flips;
    // 15-bit images drawn by the hardware renderer are shown straight from its VRAM; 24-bit
    // movie frames (and the software renderer) go through the CPU copy.
    if (d.enabled && !d.rgb24 && hw_active()) {
        int px, py, pw, ph;
        const unsigned vram = hw_display_texture(d.x, d.y, w, h, &px, &py, &pw, &ph);
        const int cs = crop * hw_scale();
        present_game_texture(vram, 1024 * hw_scale(), 512 * hw_scale(), px, py + cs, pw, ph - 2 * cs, h - 2 * crop, opt);
        finish_frame(true);
        return;
    }
    g_pixels.resize((size_t)w * h);
    if (d.enabled) {
        for (int y = 0; y < h; y++) {
            uint32_t* row = &g_pixels[(size_t)y * w];
            const int vy = (d.y + y) & 511;
            if (d.rgb24) {
                const uint8_t* src = (const uint8_t*)&d.vram[vy * 1024];
                for (int x = 0; x < w; x++) {
                    const int bx = (d.x * 2 + x * 3) % 2048;
                    const uint8_t r = src[bx], g = src[(bx + 1) % 2048], b = src[(bx + 2) % 2048];
                    row[x] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
                }
            } else {
                for (int x = 0; x < w; x++) {
                    const uint16_t p = d.vram[vy * 1024 + ((d.x + x) & 1023)];
                    const uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                    row[x] = (r | r >> 5) << 16 | (g | g >> 5) << 8 | (b | b >> 5);
                }
            }
        }
    } else {
        std::fill(g_pixels.begin(), g_pixels.end(), 0);
    }
    present_game_pixels(g_pixels.data() + (size_t)crop * w, w, h - 2 * crop, opt);
    finish_frame(true);  // (the title screen's moving background is a 24-bit movie too)
}

// ================================================================ in-between images (above 60 fps)

// With a frame rate above 60 chosen (and a display at least that fast), the screen gets images at
// that rate, each the latest frame drawn again (hw_replay) with every vertex at the point of its
// motion since the frame before (motion.cpp) that matches the moment the image will be on the
// screen. The images run one emulated frame behind: frame k, due at N_k, is shown whole at N_(k+1),
// and on its way there it moves in from where frame k-1 had everything.
//
// Each image is aimed at a refresh of the display: started just before it (interp_service is polled
// while emulating and while waiting for the next frame), so the swap, which waits for the refresh
// (vsync), holds the game up only briefly. When the swap returns, that refresh has just happened:
// the next ones follow every 1/refresh seconds.
struct Recorded {
    int id;        // hw_replay's frame
    uint64_t due;  // N_k
    int nh;        // its PS1 line count (CRT filter)
};
std::vector<Recorded> g_ip_frames;  // the last few, oldest first
int g_ip_rate = 0;                  // images per second (0: off)
uint64_t g_ip_target = 0;           // the refresh the next image is for
uint64_t g_ip_ideal = 0;            // ... and when it would be at exactly the chosen rate
uint64_t g_ip_lead = 3000000;       // started this long before its refresh (ns; adapts)

int interp_rate() {
    if (g_gfx.fps <= 60 || g_turbo || g_vsync_pacing || !hw_active() || !psx_opt_sim60) return 0;
    const int rate = std::min(g_gfx.fps, (int)std::lround(g_refresh_hz));
    return rate > 60 ? rate : 0;
}

uint64_t refresh_ns() { return (uint64_t)(1e9 / std::max(g_refresh_hz, 1.0f)); }

// When the next image is to be started.
uint64_t interp_due() { return g_ip_target - g_ip_lead; }

// After an image's swap returned (at `back`): just after the refresh it waited for. Missed it
// (returned a refresh later): start earlier; on time: a little later each time, so the game is held
// up as little as can be. The next image is for the first refresh at or after the chosen rate's
// next moment.
void interp_aim(uint64_t back) {
    const uint64_t vb = refresh_ns(), period = 1000000000ull / (uint64_t)g_ip_rate;
    const bool missed = back > g_ip_target + vb / 2;
    g_ip_lead = missed ? std::min(g_ip_lead + 1000000, vb * 3 / 4) : std::max<uint64_t>(g_ip_lead - 20000, 1500000);
    const uint64_t refresh = missed ? g_ip_target + vb : back;  // the one just shown
    g_ip_ideal += period;
    if (g_ip_ideal + vb < refresh || g_ip_ideal > refresh + 2 * vb) g_ip_ideal = refresh + period;
    g_ip_target = refresh + vb;
    while (g_ip_target + vb / 2 < g_ip_ideal) g_ip_target += vb;
}

void interp_service() {
    if (!g_ip_rate || g_ip_frames.empty()) return;
    const uint64_t start = SDL_GetTicksNS();
    if (start < interp_due()) return;
    const uint64_t vb = refresh_ns();
    if (start > g_ip_target) g_ip_target += (start - g_ip_target + vb - 1) / vb * vb;  // (late: the next refresh)
    // the moment shown: a frame period before the refresh, between the frame due last before it
    // and the one after
    const uint64_t at = g_ip_target - FRAME_NS;
    size_t k = 0;
    while (k + 1 < g_ip_frames.size() && g_ip_frames[k].due < at) k++;
    const Recorded& f = g_ip_frames[k];
    const uint64_t span = k ? f.due - g_ip_frames[k - 1].due : FRAME_NS;
    const float t = at >= f.due ? 1.0f : std::clamp(1.0f - (float)(f.due - at) / (float)span, 0.0f, 1.0f);
    int tw = 0, th = 0;
    const unsigned tex = hw_replay(f.id, t, &tw, &th);
    const int nh = f.nh;
    if (k > 1) g_ip_frames.erase(g_ip_frames.begin(), g_ip_frames.begin() + (k - 1));  // (older ones are done)
    if (!tex) return;
    present_game_texture(tex, tw, th, 0, 0, tw, th, nh, present_options());
    g_images++;
    finish_frame(true);
    interp_aim(SDL_GetTicksNS());
}

// Once a second: the frame rate in the title (and FPS counter) and, with --log host, frame
// times and audio health; once a minute in every log: the frame rate and slow moments.
void frame_stats() {
    static uint64_t fps_t0 = SDL_GetTicksNS(), fps_frames = 0, fps_images0 = 0, last_present = 0, dt_min = ~0ull,
                    dt_max = 0, dt_max_seen = 0;
    static int slow_seen = 0;
    const uint64_t t_now = SDL_GetTicksNS();
    if (last_present) {
        dt_min = std::min(dt_min, t_now - last_present);
        dt_max = std::max(dt_max, t_now - last_present);
        dt_max_seen = std::max(dt_max_seen, t_now - last_present);
        if (t_now - last_present > 50'000'000ull) slow_seen++;
    }
    last_present = t_now;
    if (++fps_frames < 60) return;
    const uint64_t now = SDL_GetTicksNS();
    const double secs = (double)(now - fps_t0) / 1e9;
    const double fps = 60.0 / secs, images = (double)(g_images - fps_images0) / secs;
    g_fps_shown = images;
    LOG(Host, "frame times: min %.2f ms, max %.2f ms; audio: queue min %d, underruns %d, drops %d",
        dt_min / 1e6, dt_max / 1e6, g_audio_min_queued, g_audio_underruns, g_audio_drops);
    g_audio_min_queued = 1 << 30;
    dt_min = ~0ull;
    dt_max = 0;
    char title[96];
    std::snprintf(title, sizeof title, "Repocalypse %s - %.0f fps%s", kVersion, images, g_turbo ? " (fast-forward)" : "");
    // only when it changes: renaming the window goes through Windows' window manager and
    // could hold the frame up by several ms (a hiccup every second)
    static std::string shown_title;
    if (shown_title != title) {
        shown_title = title;
        SDL_SetWindowTitle(g_window, title);
    }
    LOG(Host, "frame %llu: %.1f Hz emulated, %.1f images/s", (unsigned long long)g_frame, fps, images);
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

// ================================================================ start-up

// Some OpenGL drivers crash while SDL sets up an OpenGL window (seen on a PC with DisplayLink USB
// display adapters next to its AMD GPU; every build closed there without a word). On Windows the
// crash is caught, but SDL is left broken by it (the game then hung on a black screen), so the
// renderer setting is switched to Vulkan and the game starts again as a new process.
[[maybe_unused]] bool g_gl_crashed = false;
SDL_Window* create_window(const char* title, int w, int h, SDL_WindowFlags flags) {
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
[[noreturn]] void restart_self() {
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

// The top of the log (debug/repocalypse.log): what a tester's report needs to know about the
// game and the PC. The GPU, display and renderer follow when the window opens.
void report_header() {
    char when[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    log_msg(LogCat::Host, "===== Repocalypse %s report log, started %s =====", kVersion, when);
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW os{sizeof os};
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&os);
    log_msg(LogCat::Host, "system: Windows %lu.%lu build %lu", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    MEMORYSTATUSEX mem{sizeof mem};
    GlobalMemoryStatusEx(&mem);
    log_msg(LogCat::Host, "memory: %llu MB, %llu MB free", (unsigned long long)(mem.ullTotalPhys >> 20),
            (unsigned long long)(mem.ullAvailPhys >> 20));
#else
    log_msg(LogCat::Host, "system: %s", SDL_GetPlatform());
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

}  // namespace

// The mouse pointer hides after 2 s without mouse use and shows again when it is used.
void mouse_autohide(const SDL_Event* e) {
    static uint64_t used = SDL_GetTicks();
    if (e && (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_WHEEL)) {
        used = SDL_GetTicks();
        if (!SDL_CursorVisible()) SDL_ShowCursor();
    }
    if (!e && SDL_CursorVisible() && SDL_GetTicks() - used > 2000) SDL_HideCursor();
}

PresentOptions present_options() {
    PresentOptions o;
    o.aspect = g_gfx.aspect;
    // the disc's own full-screen pictures keep their 4:3 (replacements are fitted to the screen)
    if (o.aspect != Aspect::A4_3 && !gpu_movie_playing() && replace_original_picture_shown(gpu_display().y))
        o.aspect = Aspect::A4_3;
    // movies on ultra-wide screens: shaped as in 16:9, with bars at the sides
    if ((o.aspect == Aspect::A21_9 || o.aspect == Aspect::A32_9) && gpu_movie_playing()) o.aspect = Aspect::A16_9;
    o.crt = g_gfx.crt;
    o.fxaa = g_gfx.aa & 1;
    return o;
}

}  // namespace host

using namespace host;

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
    // Fullscreen (on the desktop's own mode) unless the player chose a window. The window, for
    // then: shaped like the picture; `scale` N asks for 240*N lines; 0 (the default) asks for
    // 1080: a 1080p window if the desktop has room, else the largest that fits.
    const float ratio = aspect_ratio(g_gfx.aspect) > 0 ? aspect_ratio(g_gfx.aspect) : 4.0f / 3;
    int win_h = scale > 0 ? 240 * scale : 1080;
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect usable{};
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (SDL_GetDisplayUsableBounds(display, &usable)) {
        const int frame = 40;  // title bar and borders
        const int fit_h = std::min(usable.h - frame, (int)((float)(usable.w - 16) / ratio));
        if (win_h > fit_h) win_h = std::max(240, fit_h);
    }
    const int win_w = (int)((float)win_h * ratio + 0.5f);
    if (desktop)
        log_msg(LogCat::Host, "desktop %dx%d; window %dx%d%s", desktop->w, desktop->h, win_w, win_h,
                g_fullscreen ? " (fullscreen)" : "");
    // APOC_BACKGROUND (test runs): the window opens behind the others, without focus.
    const bool background = std::getenv("APOC_BACKGROUND") != nullptr;
    // The renderer: Vulkan if chosen (and it starts), otherwise OpenGL.
    auto open_window = [&](Backend b) {
        render_select(b);
        if (b == Backend::OpenGL) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        }
        if (background) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        g_window = create_window(title, win_w, win_h,
                                 SDL_WINDOW_RESIZABLE | (b == Backend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) |
                                     (g_fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!g_window) return false;
#ifdef _WIN32
        if (background)
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
    g_video_ok = want_vulkan && open_window(Backend::Vulkan);
    if (want_vulkan && !g_video_ok) {
        log_msg(LogCat::Host, "Vulkan did not start; using OpenGL");
        host_toast("Vulkan unavailable - using OpenGL");
    }
    if (!g_video_ok) g_video_ok = open_window(Backend::OpenGL);
#ifdef _WIN32
    if (g_gl_crashed && !want_vulkan && g_renderer_override != 0) {  // (not when --renderer opengl asks for it)
        log_msg(LogCat::Host, "the OpenGL driver crashed while opening the window: switching to Vulkan and restarting");
        g_gfx.vulkan = true;
        g_renderer_override = -1;
        save_settings();
        restart_self();
    }
#endif
    if (!g_video_ok && !want_vulkan) {  // OpenGL failed: Vulkan instead
        log_msg(LogCat::Host, "OpenGL did not start; using Vulkan");
        g_video_ok = open_window(Backend::Vulkan);
        if (g_video_ok) host_toast("OpenGL unavailable - using Vulkan");
    }
    if (!g_video_ok) fatal("Could not start the graphics: this needs OpenGL 3.3 or Vulkan 1.3.");
    log_msg(LogCat::Host, "renderer: %s", render_backend_name(render_backend()));
    // On a ~60 Hz display, pace the emulation by vsync: one emulated frame per refresh gives
    // perfectly even frame times (the game's 30 fps then shows each frame for exactly two
    // refreshes). Other refresh rates fall back to a timer.
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window))) {
        if (mode->refresh_rate > 0) g_refresh_hz = mode->refresh_rate;
        g_vsync_pacing = g_refresh_hz > 58.5f && g_refresh_hz < 61.5f;
        log_msg(LogCat::Host, "display refresh %.2f Hz: %s", g_refresh_hz, g_vsync_pacing ? "pacing by vsync" : "pacing by timer");
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

void host_poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        mouse_autohide(&e);
        if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE)) g_quit = true;
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) apply_graphics();
        device_event(e);
    }
    g_turbo = g_force_turbo || SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB];
    // Holding SELECT + START for ~1.5 s opens the settings from a controller.
    static int combo_frames = 0;
    const uint16_t held = (uint16_t)~host_pad(0).buttons;
    combo_frames = ((held & B_SELECT) && (held & B_START)) ? combo_frames + 1 : 0;
    mouse_autohide(nullptr);
    if (combo_frames == 90 && g_video_ok) run_settings_menu();
}

void host_set_turbo(bool on) { g_force_turbo = on; }
bool host_quit_requested() { return g_quit; }
void host_request_quit() { g_quit = true; }
uint64_t host_frame_count() { return g_frame; }

void host_present(const GpuDisplay& d) {
    g_frame++;
    movie_vblank(d.enabled && d.rgb24 && gpu_movie_playing());
    if (g_start_state >= 0 && g_frame == 30) state_request_load(g_start_state);
    state_poll_load(g_cpu);
    if (!g_video_ok) return;
    frame_stats();
    // above 60 fps: the frame is recorded to be shown by interp_service
    const int rate = interp_rate(), w = std::clamp(d.w, 1, 1024), h = std::clamp(d.h, 1, 512);
    const int id = hw_rec_finish(d.x, d.y, w, h);
    hw_rec_enable(rate > 0);
    g_motion_on = rate > 0;
    if (rate && id == -2 && !g_ip_frames.empty()) return;  // nothing drawn since: no new frame yet
    if (rate && id >= 0 && d.enabled && !d.rgb24) {
        if (g_ip_rate != rate || g_ip_frames.empty())
            g_ip_target = g_ip_ideal = SDL_GetTicksNS() + g_ip_lead + refresh_ns() / 2;
        g_ip_rate = rate;
        g_ip_frames.push_back({id, g_next_frame_ns + FRAME_NS, h});
        if (g_ip_frames.size() > 3) g_ip_frames.erase(g_ip_frames.begin());
        interp_service();
        return;
    }
    g_ip_frames.clear();
    g_ip_rate = 0;
    draw_game(d);
}

void host_interp_poll() {
    if (g_ip_rate) interp_service();
}

void host_throttle_frame() {
    // vsync on (no tearing); a ~60 Hz display paces the frames itself (the swap waits), others
    // by our clock at the PS1's 59.94 Hz
    const bool want_vsync = !g_turbo;
    if (want_vsync != g_vsync_active) {
        present_set_vsync(want_vsync);
        g_vsync_active = want_vsync;
    }
    if (g_turbo || g_vsync_pacing) {
        g_next_frame_ns = SDL_GetTicksNS();
        return;
    }
    g_next_frame_ns += FRAME_NS;
    // the in-between images due before the next frame (above 60 fps)
    for (uint64_t t = SDL_GetTicksNS(); g_ip_rate && !g_ip_frames.empty() && interp_due() < g_next_frame_ns && t < g_next_frame_ns;
         t = SDL_GetTicksNS()) {
        if (interp_due() > t) SDL_DelayPrecise(interp_due() - t);
        interp_service();
    }
    const uint64_t now = SDL_GetTicksNS();
    if (g_next_frame_ns > now) {
        SDL_DelayPrecise(g_next_frame_ns - now);
    } else if (now - g_next_frame_ns > FRAME_NS * 4) {
        g_next_frame_ns = now;  // fell far behind (debugger, window drag): resync
    }
}

void host_audio_push(const int16_t* stereo, int frames) {
    if (!g_audio || g_turbo) return;
    // Emulated time follows the display (vsync), which is never exactly the PS1's 59.94 Hz, so
    // audio is produced slightly faster or slower than it plays. Nudge the playback rate (<=0.5%,
    // inaudible) to hold the queue near 60 ms instead of letting it underrun (clicks) or
    // overflow (dropped chunks).
    const int target = 44100 * 60 / 1000;
    const int queued = SDL_GetAudioStreamQueued(g_audio) / 4;
    g_audio_min_queued = std::min(g_audio_min_queued, queued);
    if (queued == 0) g_audio_underruns++;  // the device ran dry since the last push: a gap
    if (queued > target * 4) {  // way behind (after a stall): drop rather than lag
        g_audio_drops++;
        return;
    }
    const float ratio = 1.0f + 0.005f * std::clamp((float)(queued - target) / (float)target, -1.0f, 1.0f);
    SDL_SetAudioStreamFrequencyRatio(g_audio, ratio);
    SDL_PutAudioStreamData(g_audio, stereo, frames * 4);
}

void host_audio_clear() {
    if (g_audio) SDL_ClearAudioStream(g_audio);
}

void host_toast(const std::string& text) {
    g_toast = text;
    g_toast_until = SDL_GetTicksNS() + 2500000000ull;
    log_msg(LogCat::Host, "%s", text.c_str());
}
