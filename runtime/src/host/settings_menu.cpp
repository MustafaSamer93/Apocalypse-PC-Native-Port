// The settings screen (Select + Start held): states, chapters, graphics and the Recomp
// Enhancements page, drawn over the paused game.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/psx.h"
#include "core/savestate.h"
#include "game/game.h"
#include "host/host.h"
#include "host/host_internal.h"
#include "render/render_backend.h"
#include "render/ui_text.h"

namespace host {
namespace {

// ================================================================ drawing and keys

// A centred menu box over the game. lines[0] is the title; a '\t' splits a line into a label
// and a value column. Sizes are in the overlay's 640x480 units.
void draw_panel(const std::vector<std::string>& lines, int highlight) {
    constexpr float kLine = 15.0f, kTitle = 21.0f, kPad = 12.0f, kGap = 26.0f;
    SDL_Renderer* r = present_overlay_begin();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    const int n = (int)lines.size();
    float label_w = 0, value_w = 0, whole_w = n ? ui_text_width(r, lines[0].c_str(), kTitle) : 0;
    for (int i = 1; i < n; i++) {
        const size_t tab = lines[i].find('\t');
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
        const size_t tab = lines[i].find('\t');
        ui_text(r, x0 + kPad, y, lines[i].substr(0, tab).c_str(), kLine);
        if (tab != std::string::npos) ui_text(r, x0 + kPad + col, y, lines[i].c_str() + tab + 1, kLine);
    }
    present_redraw(present_options());
    present_overlay_end();
    present_swap();
}

// Pumps events for the menus; key presses come back as SDL scancodes. False: the window closed.
bool pump_menu_events(std::vector<SDL_Scancode>& keys) {
    SDL_Event e;
    mouse_autohide(nullptr);
    while (SDL_PollEvent(&e)) {
        mouse_autohide(&e);
        if (e.type == SDL_EVENT_QUIT) return false;
        if (e.type == SDL_EVENT_KEY_DOWN) keys.push_back(e.key.scancode);
        device_event(e);
    }
    return true;
}

// Pad presses since the last call as menu keys (edge-triggered). The default keyboard keys also
// drive the pad (arrows = d-pad), so a key already seen as a key event is not added again.
void pad_keys(uint16_t& prev, std::vector<SDL_Scancode>& keys, bool shoulders) {
    const uint16_t pad = host_pad(0).buttons, pressed = (uint16_t)(prev & ~pad);
    prev = pad;
    auto push = [&](uint16_t bits, SDL_Scancode k) {
        if ((pressed & bits) && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    };
    push(B_UP, SDL_SCANCODE_UP);
    push(B_DOWN, SDL_SCANCODE_DOWN);
    push(B_LEFT | (shoulders ? B_L1 : 0), SDL_SCANCODE_LEFT);
    push(B_RIGHT | (shoulders ? B_R1 : 0), SDL_SCANCODE_RIGHT);
    push(B_CROSS | B_START, SDL_SCANCODE_RETURN);
    push(B_CIRCLE, SDL_SCANCODE_ESCAPE);
}

// ================================================================ the controls screen

// Its look: the game's colours (dark panel, orange), in the overlay's 640x480 units.
struct Rgb {
    Uint8 r, g, b, a = 255;
};
constexpr Rgb kInk{243, 227, 204}, kDim{168, 134, 106}, kAccent{200, 80, 30}, kGlow{255, 138, 60}, kTitle{255, 170, 90},
              kPanel{18, 13, 10, 240}, kRowSel{48, 28, 16}, kCap{30, 21, 16}, kCapEdge{107, 74, 48}, kNone{122, 96, 80};
void set(SDL_Renderer* r, Rgb c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); }
void fill(SDL_Renderer* r, float x, float y, float w, float h, Rgb c) {
    set(r, c);
    SDL_FRect f{x, y, w, h};
    SDL_RenderFillRect(r, &f);
}
void frame(SDL_Renderer* r, float x, float y, float w, float h, Rgb c) {
    set(r, c);
    SDL_FRect f{x, y, w, h};
    SDL_RenderRect(r, &f);
}
void text(SDL_Renderer* r, float x, float y, const std::string& s, float size, Rgb c) {
    set(r, c);
    ui_text(r, x, y, s.c_str(), size);
}

// The actions in groups, as the screen lists them.
struct Group {
    const char* name;
    int first, count;
};
constexpr Group kActionGroups[] = {{"MOVEMENT", A_UP, 4}, {"COMBAT", A_AIM, 9}, {"MENU", A_PAUSE, 2}};

void draw_controls(int mode, int sel, int slot, bool waiting, const std::string& status) {
    SDL_Renderer* r = present_overlay_begin();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    fill(r, 0, 0, 640, 480, {0, 0, 0, 110});
    constexpr float x0 = 70, y0 = 8, w = 500, h = 464, pad = 18;
    fill(r, x0, y0, w, h, kPanel);
    frame(r, x0, y0, w, h, kAccent);
    text(r, x0 + pad, y0 + 10, "CONTROLS", 22, kTitle);

    // the two devices as tabs: the one picked is the one played with
    float tx = x0 + pad;
    const float ty = y0 + 42;
    for (int m = 0; m < M_COUNT; m++) {
        const char* label = m == M_KEYBOARD ? "Keyboard and mouse" : "Controller";
        const float tw = ui_text_width(r, label, 14) + 24;
        if (m == mode) fill(r, tx, ty, tw, 22, kAccent);
        else frame(r, tx, ty, tw, 22, kCapEdge);
        text(r, tx + 12, ty + 4, label, 14, m == mode ? Rgb{255, 255, 255} : kDim);
        tx += tw + 8;
    }
    if (mode == M_CONTROLLER && g_devices.empty()) text(r, tx + 6, ty + 5, "none connected", 12, kNone);
    else if (mode == M_CONTROLLER) text(r, tx + 6, ty + 5, std::string(device_name(g_devices[0])).substr(0, 24), 12, kDim);

    const float cx[3] = {x0 + pad, x0 + 220, x0 + 360};
    float y = ty + 34;
    text(r, cx[0] + 8, y, "Action", 12, kDim);
    text(r, cx[1], y, "Input 1", 12, kDim);
    text(r, cx[2], y, "Input 2", 12, kDim);
    y += 16;
    for (const Group& g : kActionGroups) {
        y += 2;
        text(r, cx[0], y, g.name, 11, kDim);
        y += 13;
        for (int a = g.first; a < g.first + g.count; a++) {
            const bool on = a == sel;
            if (on) fill(r, x0 + pad - 6, y - 1, w - 2 * pad + 12, 17, kRowSel);
            text(r, cx[0] + 8, y, action_label(a), 13, kInk);
            if (*action_shoulder(a))
                text(r, cx[0] + 14 + ui_text_width(r, action_label(a), 13), y + 2, action_shoulder(a), 10, kDim);
            for (int s = 0; s < kSlots; s++) {
                const Input& in = g_binds[mode][a][s];
                const bool hl = on && s == slot;
                std::string label = in.kind == Input::None ? "none" : input_name(in);
                if (hl && waiting) label = mode == M_KEYBOARD ? "Press a key..." : a == A_AIM ? "Push a stick..." : "Press a button...";
                const float bw = std::max(30.0f, ui_text_width(r, label.c_str(), 12) + 16);
                fill(r, cx[1 + s], y - 1, bw, 15, hl ? Rgb{58, 32, 16} : kCap);
                frame(r, cx[1 + s], y - 1, bw, 15, hl ? kGlow : in.kind == Input::None ? Rgb{70, 50, 36} : kCapEdge);
                text(r, cx[1 + s] + 8, y + 0.5f, label, 12, hl && waiting ? kTitle : in.kind == Input::None ? kNone : kInk);
            }
            y += 17;
        }
    }
    const float fy = y0 + h - 40;
    fill(r, x0 + pad, fy, w - 2 * pad, 1, {58, 42, 30});
    if (!status.empty()) text(r, x0 + pad, fy - 18, status, 12, kTitle);
    const bool pad_nav = input_mode() == M_CONTROLLER;
    text(r, x0 + pad, fy + 6, pad_nav ? "D-pad: move      Cross / A: change      Square / X: clear" : "Arrows: move      Enter: change      Delete: clear", 12, kDim);
    text(r, x0 + pad, fy + 20, pad_nav ? "L1 / R1: device      Triangle / Y: defaults      Circle / B: back" : "Tab: device      R: defaults      Esc: back", 12, kDim);
    present_redraw(present_options());
    present_overlay_end();
    present_swap();
}

// ================================================================ chapters

// The chapter list: every chapter kept so far, in the order reached. Up/down move, left/right
// jump to the previous/next level, Enter loads, Esc goes back. Returns true (with the chosen
// index in *chapter) to load it.
bool pick_chapter(const std::vector<std::string>& chapters, int* chapter, bool* quit) {
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
        if (!pump_menu_events(keys)) {
            *quit = true;
            return false;
        }
        pad_keys(prev_pad, keys, true);
        for (auto k : keys) {
            if (k == SDL_SCANCODE_ESCAPE) return false;
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

// ================================================================ the settings screen

enum Row {
    R_SAVE, R_LOAD, R_LOAD_AUTO, R_CHAPTER, R_RENDERER, R_RES, R_AA, R_MSAA, R_ASPECT, R_CRT, R_FILTER,
    R_DUMP_TEX, R_TEX_REPLACE, R_ENHANCE, R_BACK,
    // the Enhancements page: rows of enhancement settings (enh_of)
    R_FPS, R_FPS_COUNTER, R_DRAWDIST, R_GAME_FONT, R_FONT, R_MENU_SIZE, R_ENH_BACK,
};
const std::vector<Row> kMainRows = {R_SAVE, R_LOAD, R_LOAD_AUTO, R_CHAPTER, R_RENDERER, R_RES, R_AA, R_MSAA, R_ASPECT,
                                    R_CRT, R_FILTER, R_DUMP_TEX, R_TEX_REPLACE, R_ENHANCE, R_BACK};
const std::vector<Row> kEnhRows = {R_FPS, R_FPS_COUNTER, R_DRAWDIST, R_GAME_FONT, R_FONT, R_MENU_SIZE, R_ENH_BACK};

// The enhancement setting a row shows (-1: none).
int enh_of(Row r) {
    switch (r) {
        case R_FPS: return E_FPS;
        case R_FPS_COUNTER: return E_FPS_COUNTER;
        case R_DRAWDIST: return E_DRAWDIST;
        case R_GAME_FONT: return E_GAME_FONT;
        case R_FONT: return E_SMOOTH_FONT;
        case R_MENU_SIZE: return E_FONT_SCALE;
        case R_CRT: return E_CRT;
        default: return -1;
    }
}

int g_chapter_sel = 0;  // the chapter last chosen
int g_slot = 1;         // the state slot last chosen

struct MenuOpen {  // (core.cpp watchdog: the game stops on purpose while a menu is open)
    MenuOpen() { g_host_menu_open = true; }
    ~MenuOpen() { g_host_menu_open = false; }
};

}  // namespace

// The controls screen: up/down an action, left/right its first or second input, Enter (Cross) sets
// it from the next input pressed (Esc cancels, Delete clears), Tab (L1/R1) switches between keyboard
// and mouse and controller (the one played with), R (Triangle) restores the defaults.
void run_controls_menu() {
    MenuOpen open_flag;
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    int sel = A_UP, slot = 0;
    bool waiting = false, quit = false;
    std::string status;
    uint16_t prev = host_pad(0).buttons;
    for (bool open = true; open && !quit;) {
        const int mode = g_input_mode >= 0 ? g_input_mode : input_mode();
        std::vector<SDL_Scancode> keys;
        mouse_autohide(nullptr);
        for (SDL_Event e; SDL_PollEvent(&e);) {
            mouse_autohide(&e);
            device_event(e);
            if (e.type == SDL_EVENT_QUIT) { quit = true; break; }
            if (!waiting) {
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) keys.push_back(e.key.scancode);
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && (e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_DELETE)) {
                if (e.key.scancode == SDL_SCANCODE_DELETE) g_binds[mode][sel][slot] = {}, save_settings();
                waiting = false;
                status = "";
                continue;
            }
            Input in;
            if (!input_capture(mode, e, &in)) continue;
            if (sel == A_AIM && mode == M_CONTROLLER) {  // aiming on a controller is a stick
                if (in.kind != Input::Axis || in.code > SDL_GAMEPAD_AXIS_RIGHTY) continue;
                in = {Input::Stick, (int16_t)(in.code >= SDL_GAMEPAD_AXIS_RIGHTX ? 1 : 0), 0};
            }
            status = std::string(action_label(sel)) + ": " + input_name(in);
            for (int a = 0; a < A_COUNT; a++)  // an input does one thing: taken from any other action
                for (int s = 0; s < kSlots; s++)
                    if (g_binds[mode][a][s] == in && !(a == sel && s == slot)) {
                        g_binds[mode][a][s] = {};
                        status += std::string("  (taken from ") + action_label(a) + ")";
                    }
            g_binds[mode][sel][slot] = in;
            save_settings();
            waiting = false;
        }
        // a controller drives the screen too (its buttons through the bindings: d-pad, Cross, Circle...)
        const uint16_t pad = host_pad(0).buttons, pressed = (uint16_t)(prev & ~pad);
        prev = pad;
        if (!waiting && input_mode() == M_CONTROLLER) {
            auto push = [&](uint16_t bit, SDL_Scancode k) { if (pressed & bit) keys.push_back(k); };
            push(B_UP, SDL_SCANCODE_UP), push(B_DOWN, SDL_SCANCODE_DOWN), push(B_LEFT, SDL_SCANCODE_LEFT),
                push(B_RIGHT, SDL_SCANCODE_RIGHT), push(B_CROSS, SDL_SCANCODE_RETURN), push(B_CIRCLE, SDL_SCANCODE_ESCAPE),
                push(B_SQUARE, SDL_SCANCODE_DELETE), push(B_L1 | B_R1, SDL_SCANCODE_TAB), push(B_TRIANGLE, SDL_SCANCODE_R);
        }
        for (SDL_Scancode k : keys) {
            switch (k) {
                case SDL_SCANCODE_ESCAPE: open = false; break;
                case SDL_SCANCODE_UP: sel = (sel + A_COUNT - 1) % A_COUNT; break;
                case SDL_SCANCODE_DOWN: sel = (sel + 1) % A_COUNT; break;
                case SDL_SCANCODE_LEFT:
                case SDL_SCANCODE_RIGHT: slot ^= 1; break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER: waiting = true, status = ""; break;
                case SDL_SCANCODE_DELETE:
                case SDL_SCANCODE_BACKSPACE:
                    g_binds[mode][sel][slot] = {};
                    save_settings();
                    status = "Cleared.";
                    break;
                case SDL_SCANCODE_TAB:
                    g_input_mode = mode == M_KEYBOARD ? M_CONTROLLER : M_KEYBOARD;
                    save_settings();
                    status = "";
                    break;
                case SDL_SCANCODE_R:
                    binds_default(mode);
                    save_settings();
                    status = "Defaults restored.";
                    break;
                default: break;
            }
            if (!open || waiting) break;
        }
        draw_controls(g_input_mode >= 0 ? g_input_mode : input_mode(), sel, slot, waiting, status);
        SDL_Delay(15);
    }
    if (quit) g_quit = true;
    input_mute();  // (the key or button that closed it is still down)
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    g_next_frame_ns = SDL_GetTicksNS();
}

void run_settings_menu() {
    MenuOpen open_flag;
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    bool enh_page = false;
    int& chapter = g_chapter_sel;
    const std::vector<std::string> chapters = state_chapter_names();
    if (chapter >= (int)chapters.size()) chapter = 0;
    int sel = 0;
    std::string status;
    uint16_t prev_pad = host_pad(0).buttons;  // buttons held as the menu opens (Select + Start) are no presses
    bool quit = false;
    static const char* kAspect[] = {"4:3", "16:9", "16:10", "Stretch", "21:9", "32:9"};
    for (bool open = true; open && !quit;) {
        std::vector<SDL_Scancode> keys;
        if (!pump_menu_events(keys)) { quit = true; break; }
        pad_keys(prev_pad, keys, false);

        bool gfx_changed = false;
        auto open_page = [&](bool enh) {  // back to the main page lands on "Enhancements"
            sel = enh ? 0 : (int)(std::find(kMainRows.begin(), kMainRows.end(), R_ENHANCE) - kMainRows.begin());
            enh_page = enh;
            status = "";
        };
        for (auto k : keys) {
            const std::vector<Row>& rows = enh_page ? kEnhRows : kMainRows;
            const int count = (int)rows.size();
            const int dir = k == SDL_SCANCODE_LEFT ? -1 : k == SDL_SCANCODE_RIGHT ? 1 : 0;
            if (k == SDL_SCANCODE_UP) sel = (sel + count - 1) % count;
            if (k == SDL_SCANCODE_DOWN) sel = (sel + 1) % count;
            if (k == SDL_SCANCODE_ESCAPE) {
                if (enh_page) open_page(false);
                else open = false;
                continue;
            }
            const bool enter = k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER;
            const Row row = rows[sel];
            if (dir || enter) {
                const int step = dir ? dir : 1;
                if (const int e = enh_of(row); e >= 0) {
                    enh_change(e, step);
                    gfx_changed = e != E_GAME_FONT;
                }
                switch (row) {
                    case R_RES: g_gfx.scale = (g_gfx.scale + step + 10) % 10; gfx_changed = true; break;
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
                    case R_ASPECT: {  // 4:3, 16:9, 16:10, 21:9, 32:9, Stretch
                        static const int order[] = {0, 1, 2, 4, 5, 3};
                        int i = 0;
                        while (order[i] != (int)g_gfx.aspect) i++;
                        g_gfx.aspect = (Aspect)order[(i + step + 6) % 6];
                        gfx_changed = true;
                        break;
                    }
                    case R_FILTER: g_gfx.texture_filter = !g_gfx.texture_filter; gfx_changed = true; break;
                    case R_DUMP_TEX: g_tex_dump = !g_tex_dump; break;  // texdump.cpp (not saved)
                    case R_TEX_REPLACE:  // assets/replacements: textures (texreplace.cpp), movies (movie.cpp)
                        g_tex_replace = !g_tex_replace;
                        texreplace_vram_changed();
                        save_settings();
                        break;
                    case R_CHAPTER:
                        if (dir && !chapters.empty()) chapter = (chapter + step + (int)chapters.size()) % (int)chapters.size();
                        break;
                    case R_SAVE:
                    case R_LOAD:
                        if (dir) g_slot = (g_slot - 1 + step + kStateSlots) % kStateSlots + 1;
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
                state_request_save(g_slot);  // taken at the next frame, once the menu closes
                open = false;
            } else if (row == R_LOAD || row == R_LOAD_AUTO) {
                const int s = row == R_LOAD ? g_slot : kAutoSlot;
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
            }
        }
        if (gfx_changed) {
            apply_graphics();
            save_settings();
            status = g_gfx.scale > 0 && g_active_scale == 0 ? "Hardware renderer unavailable." : "";
        }
        if (!open || quit) break;

        std::string res = g_gfx.scale == 0 ? std::string("Original (software)")
                                           : std::to_string(g_gfx.scale) + "x  (" + std::to_string(240 * g_gfx.scale) + " lines)";
        if (g_gfx.scale > 0 && g_active_scale > g_gfx.scale)
            res = std::to_string(g_gfx.scale) + "x, SSAA: " + std::to_string(g_active_scale) + "x";
        auto choice = [](const char* label, const std::string& value) { return std::string(label) + "\t< " + value + " >"; };
        const std::string slot_line = std::to_string(g_slot) + ": " + state_slot_label(g_slot);
        auto line = [&](Row r) -> std::string {
            if (const int e = enh_of(r); e >= 0) return choice(enh_name(e), enh_value(e));
            switch (r) {
                case R_SAVE: return choice("Save state", slot_line);
                case R_LOAD: return choice("Load state", slot_line);
                case R_LOAD_AUTO: return choice("Load auto-save", state_slot_label(kAutoSlot));
                case R_CHAPTER: return choice("Chapter", chapters.empty() ? std::string("none yet") : chapters[chapter].substr(0, 26));
                case R_RENDERER: return choice("Renderer", enh_value(E_RENDERER));
                case R_RES: return choice("Resolution", res);
                case R_AA: return choice("Anti-aliasing", kAaNames[g_gfx.aa]);
                case R_MSAA: return choice("AA rate (MSAA)", msaa_label());
                case R_ASPECT: return choice("Aspect ratio", kAspect[(int)g_gfx.aspect]);
                case R_FILTER: return choice("Smooth textures", g_gfx.texture_filter ? "On" : "Off");
                case R_DUMP_TEX: return choice("Dump textures", g_tex_dump ? "On (assets/dumps/textures)" : "Off");
                case R_TEX_REPLACE: return choice("HD replacements (textures, movies)", g_tex_replace ? "[x] On" : "[ ] Off (original)");
                case R_ENHANCE: return "Recomp Enhancements\t>";
                case R_BACK: return "Back to game";
                case R_ENH_BACK: return "Back";
                default: return "";
            }
        };
        std::vector<std::string> lines = {enh_page ? "RECOMP ENHANCEMENTS" : "SETTINGS"};
        for (Row r : enh_page ? kEnhRows : kMainRows) lines.push_back(line(r));
        lines.push_back(status);
        if (enh_page) {
            lines.push_back("Beyond the original game. Also in the game's");
            lines.push_back("own main menu: Recomp Enhancements.");
        }
        lines.push_back("Arrows: change   Enter: pick   Esc: back");
        draw_panel(lines, 1 + sel);
        SDL_Delay(15);
    }
    if (quit) g_quit = true;
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    g_next_frame_ns = SDL_GetTicksNS();  // don't try to "catch up" the paused time
}

}  // namespace host
