// The settings screen (Select + Start held): states, chapters, graphics, controls and the Recomp
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

// ================================================================ button setup

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

std::string step_line(int step) {
    return "Step " + std::to_string(step + 1) + " of " + std::to_string((int)std::size(kRemapOrder));
}

// Walks the user through pressing each PS1 button on `dev`. Returns false if cancelled.
bool remap_device(const InputDevice& dev, Layout* out, bool* quit) {
    SDL_Joystick* j = device_joystick(dev);
    Layout lay;
    for (int step = 0; step < (int)std::size(kRemapOrder); step++) {
        // wait until everything is released so one press is not taken twice
        while (any_input_held(j)) {
            std::vector<SDL_Scancode> keys;
            if (!pump_menu_events(keys)) { *quit = true; return false; }
            SDL_Delay(10);
        }
        const JoySnapshot base = snapshot(j);
        for (;;) {
            std::vector<SDL_Scancode> keys;
            if (!pump_menu_events(keys)) { *quit = true; return false; }
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
            draw_panel({"CONTROLLER SETUP", std::string(device_name(dev)).substr(0, 36), "", step_line(step), "",
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
    for (int step = 0; step < (int)std::size(kRemapOrder);) {
        std::vector<SDL_Scancode> keys;
        if (!pump_menu_events(keys)) { *quit = true; return false; }
        if (!keys.empty()) {
            if (keys[0] == SDL_SCANCODE_ESCAPE) return false;
            if (keys[0] != SDL_SCANCODE_DELETE) keys_out[bit_index(kRemapOrder[step].bit)] = keys[0];
            step++;
            continue;
        }
        draw_panel({"KEYBOARD SETUP", "", step_line(step), "", "Press the key for:", "",
                    std::string("   ") + kRemapOrder[step].name, "", "", "Delete: leave unbound", "Esc: cancel"},
                   -1);
        SDL_Delay(10);
    }
    std::memcpy(out, keys_out, sizeof keys_out);
    return true;
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
    R_DUMP_TEX, R_TEX_REPLACE, R_ENHANCE, R_DEVICE, R_REMAP, R_RESET, R_BACK,
    // the Enhancements page: rows of enhancement settings (enh_of)
    R_FPS, R_FPS_COUNTER, R_SIM, R_DRAWDIST, R_PRECISE, R_NATIVE_3D, R_GAME_FONT, R_FONT, R_MENU_SIZE, R_ENH_BACK,
};
const std::vector<Row> kMainRows = {R_SAVE, R_LOAD, R_LOAD_AUTO, R_CHAPTER, R_RENDERER, R_RES, R_AA, R_MSAA, R_ASPECT,
                                    R_CRT, R_FILTER, R_DUMP_TEX, R_TEX_REPLACE, R_ENHANCE, R_DEVICE, R_REMAP, R_RESET, R_BACK};
const std::vector<Row> kEnhRows = {R_FPS, R_FPS_COUNTER, R_SIM, R_DRAWDIST, R_PRECISE, R_NATIVE_3D, R_GAME_FONT, R_FONT, R_MENU_SIZE, R_ENH_BACK};

// The enhancement setting a row shows (-1: none).
int enh_of(Row r) {
    switch (r) {
        case R_FPS: return E_FPS;
        case R_FPS_COUNTER: return E_FPS_COUNTER;
        case R_SIM: return E_SIM;
        case R_DRAWDIST: return E_DRAWDIST;
        case R_PRECISE: return E_PRECISE;
        case R_NATIVE_3D: return E_NATIVE_3D;
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

void run_settings_menu() {
    MenuOpen open_flag;
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    bool enh_page = false;
    int& chapter = g_chapter_sel;
    const std::vector<std::string> chapters = state_chapter_names();
    if (chapter >= (int)chapters.size()) chapter = 0;
    size_t dev_index = 0;  // the Controls row: 0 = all, 1 = keyboard & mouse, 2.. = controllers
    if (g_input == "keyboard") dev_index = 1;
    for (size_t i = 0; i < g_devices.size(); i++)
        if (!g_input.empty() && device_guid(g_devices[i]) == g_input) dev_index = 2 + i;
    int sel = 0;
    std::string status;
    uint16_t prev_pad = host_pad(0).buttons;  // buttons held as the menu opens (Select + Start) are no presses
    bool quit = false;
    static const char* kAspect[] = {"4:3", "16:9", "16:10", "Stretch", "21:9", "32:9"};
    for (bool open = true; open && !quit;) {
        std::vector<SDL_Scancode> keys;
        if (!pump_menu_events(keys)) { quit = true; break; }
        pad_keys(prev_pad, keys, false);

        const size_t entries = 2 + g_devices.size();
        if (dev_index >= entries) dev_index = 0;
        const bool is_all = dev_index == 0, is_keyboard = dev_index == 1;
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
                    case R_TEX_REPLACE:  // texreplace.cpp: assets/replacements/textures
                        g_tex_replace = !g_tex_replace;
                        texreplace_vram_changed();
                        save_settings();
                        break;
                    case R_CHAPTER:
                        if (dir && !chapters.empty()) chapter = (chapter + step + (int)chapters.size()) % (int)chapters.size();
                        break;
                    case R_DEVICE:
                        dev_index = (dev_index + entries + step) % entries;
                        g_input = dev_index == 0 ? "" : dev_index == 1 ? "keyboard" : device_guid(g_devices[dev_index - 2]);
                        save_settings();
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
            } else if ((row == R_REMAP || row == R_RESET) && is_all) {
                status = "Pick the keyboard or a controller first.";
            } else if (row == R_REMAP) {
                bool done;
                if (is_keyboard) {
                    SDL_Scancode keys_new[16];
                    if ((done = remap_keyboard(keys_new, &quit))) {
                        std::memcpy(g_keys, keys_new, sizeof g_keys);
                        g_key_custom = true;
                    }
                } else {
                    const InputDevice dev = g_devices[dev_index - 2];
                    Layout lay;
                    if ((done = remap_device(dev, &lay, &quit))) g_layouts[device_guid(dev)] = lay;
                }
                if (done) save_settings(), status = "Saved. This layout is used every time.";
                else if (!quit) status = "Cancelled, layout unchanged.";
                prev_pad = host_pad(0).buttons;
            } else if (row == R_RESET) {
                if (is_keyboard) g_key_custom = false;
                else g_layouts.erase(device_guid(g_devices[dev_index - 2]));
                save_settings();
                status = "Restored the default layout.";
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
        const std::string dev_line = is_all        ? std::string("All (keyboard + controllers)")
                                     : is_keyboard ? std::string("Keyboard & mouse")
                                                   : std::string(device_name(g_devices[dev_index - 2])).substr(0, 22);
        const bool custom = is_all ? false : is_keyboard ? g_key_custom : g_layouts.count(device_guid(g_devices[dev_index - 2])) > 0;
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
                case R_TEX_REPLACE: return choice("Texture replacements", g_tex_replace ? "[x] On" : "[ ] Off (original)");
                case R_ENHANCE: return "Recomp Enhancements\t>";
                case R_DEVICE: return choice("Controls", dev_line);
                case R_REMAP: return std::string("Set up buttons\t") + (is_all ? "" : custom ? "(custom)" : "(default)");
                case R_RESET: return "Reset buttons to default";
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
