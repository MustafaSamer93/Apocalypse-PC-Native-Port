// Host platform layer: SDL3 window, audio, input and frame pacing.
#include <SDL3/SDL.h>

#include <algorithm>
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
    g_window = SDL_CreateWindow(title, 320 * scale, 240 * scale, SDL_WINDOW_RESIZABLE);
    if (!g_window) return false;
    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (!g_renderer) return false;
    SDL_SetRenderVSync(g_renderer, 0);
    SDL_SetRenderLogicalPresentation(g_renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);

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

void host_poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) g_quit = true;
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

PadState host_pad(int port) {
    uint16_t pressed = 0;
    if (port == 0) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        struct { SDL_Scancode sc; uint16_t bit; } map[] = {
            {SDL_SCANCODE_UP, B_UP}, {SDL_SCANCODE_DOWN, B_DOWN}, {SDL_SCANCODE_LEFT, B_LEFT},
            {SDL_SCANCODE_RIGHT, B_RIGHT}, {SDL_SCANCODE_Z, B_CROSS}, {SDL_SCANCODE_X, B_CIRCLE},
            {SDL_SCANCODE_A, B_SQUARE}, {SDL_SCANCODE_S, B_TRIANGLE}, {SDL_SCANCODE_Q, B_L1},
            {SDL_SCANCODE_W, B_R1}, {SDL_SCANCODE_1, B_L2}, {SDL_SCANCODE_2, B_R2},
            {SDL_SCANCODE_RETURN, B_START}, {SDL_SCANCODE_BACKSPACE, B_SELECT},
            {SDL_SCANCODE_RSHIFT, B_SELECT},
        };
        for (auto& m : map)
            if (k[m.sc]) pressed |= m.bit;
    }
    for (auto& dev : g_devices) {
        if (port != 0) break;
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
        // left stick doubles as the d-pad (the game predates analog pads)
        int lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        if (lx < -16000) pressed |= B_LEFT;
        if (lx > 16000) pressed |= B_RIGHT;
        if (ly < -16000) pressed |= B_UP;
        if (ly > 16000) pressed |= B_DOWN;
    }
    if (port == 0)
        for (auto& sp : g_script)
            if (g_frame >= sp.from && g_frame < sp.to) pressed |= sp.bit;
    PadState s;
    s.buttons = (uint16_t)~pressed;
    s.connected = port == 0;
    return s;
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
    static uint64_t fps_t0 = SDL_GetTicksNS(), fps_frames = 0;
    if (++fps_frames == 60) {
        uint64_t now = SDL_GetTicksNS();
        double fps = 60.0 * 1e9 / (double)(now - fps_t0);
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
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_FRect dst{0, 0, 640, 480};  // stretch to 4:3 regardless of PS1 video mode
    SDL_RenderTexture(g_renderer, g_texture, nullptr, &dst);
    SDL_RenderPresent(g_renderer);
}

void host_throttle_frame() {
    if (g_turbo) {
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
    // drop audio rather than build up latency if the queue grows beyond ~100 ms
    if (SDL_GetAudioStreamQueued(g_audio) > 44100 * 4 / 10) return;
    SDL_PutAudioStreamData(g_audio, stereo, frames * 4);
}

int host_audio_queued_frames() { return g_audio ? SDL_GetAudioStreamQueued(g_audio) / 4 : 0; }
