// Button prompts: the controller button and the keyboard key bound to the same thing, side by side
// ("[pad] / [key]"), as Kenney's Input Prompts icons (runtime/assets/prompts, built into the exe) in the
// style of the controller: PlayStation, Xbox or Steam Deck. Used by the settings and controls screens,
// the game's own legends (gpu.cpp gpu_prompts: the front end's button icons and PRESS START) and
// host_prompt_hint.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "core/psx.h"
#include "game/game.h"
#include "host/host.h"
#include "host/host_internal.h"
#include "render/present.h"
#include "render/present_common.h"
#include "render/ui_text.h"

namespace host {

PromptStyle g_prompt_style = PromptStyle::Auto;

namespace {

// Icon file names by input, one column per style (PlayStation, Xbox, Steam Deck); null: no icon (its name
// is shown instead).
constexpr const char* kPadButtons[][3] = {  // by SDL_GamepadButton
    {"playstation_button_color_cross", "xbox_button_color_a", "steamdeck_button_a"},         // south
    {"playstation_button_color_circle", "xbox_button_color_b", "steamdeck_button_b"},        // east
    {"playstation_button_color_square", "xbox_button_color_x", "steamdeck_button_x"},        // west
    {"playstation_button_color_triangle", "xbox_button_color_y", "steamdeck_button_y"},      // north
    {"playstation4_button_share", "xbox_button_view", "steamdeck_button_view"},              // back
    {nullptr, nullptr, nullptr},                                                             // guide
    {"playstation4_button_options", "xbox_button_menu", "steamdeck_button_options"},         // start
    {"playstation_button_l3", "xbox_ls", "steamdeck_stick_l_press"},                         // left stick
    {"playstation_button_r3", "xbox_rs", "steamdeck_stick_r_press"},                         // right stick
    {"playstation_trigger_l1", "xbox_lb", "steamdeck_button_l1"},                            // left shoulder
    {"playstation_trigger_r1", "xbox_rb", "steamdeck_button_r1"},                            // right shoulder
    {"playstation_dpad_up", "xbox_dpad_up", "steamdeck_dpad_up"},
    {"playstation_dpad_down", "xbox_dpad_down", "steamdeck_dpad_down"},
    {"playstation_dpad_left", "xbox_dpad_left", "steamdeck_dpad_left"},
    {"playstation_dpad_right", "xbox_dpad_right", "steamdeck_dpad_right"},
    {nullptr, nullptr, nullptr},                                                             // misc 1
    {nullptr, nullptr, "steamdeck_button_r4"},                                               // right paddle 1
    {nullptr, nullptr, "steamdeck_button_l4"},                                               // left paddle 1
    {nullptr, nullptr, "steamdeck_button_r5"},                                               // right paddle 2
    {nullptr, nullptr, "steamdeck_button_l5"},                                               // left paddle 2
    {"playstation5_touchpad", nullptr, nullptr},                                             // touchpad
};
constexpr const char* kPadAxes[][2][3] = {  // by SDL_GamepadAxis, then pushed - / +
    {{"playstation_stick_l_left", "xbox_stick_l_left", "steamdeck_stick_l_left"},
     {"playstation_stick_l_right", "xbox_stick_l_right", "steamdeck_stick_l_right"}},
    {{"playstation_stick_l_up", "xbox_stick_l_up", "steamdeck_stick_l_up"},
     {"playstation_stick_l_down", "xbox_stick_l_down", "steamdeck_stick_l_down"}},
    {{"playstation_stick_r_left", "xbox_stick_r_left", "steamdeck_stick_r_left"},
     {"playstation_stick_r_right", "xbox_stick_r_right", "steamdeck_stick_r_right"}},
    {{"playstation_stick_r_up", "xbox_stick_r_up", "steamdeck_stick_r_up"},
     {"playstation_stick_r_down", "xbox_stick_r_down", "steamdeck_stick_r_down"}},
    {{"playstation_trigger_l2", "xbox_lt", "steamdeck_button_l2"}, {"playstation_trigger_l2", "xbox_lt", "steamdeck_button_l2"}},
    {{"playstation_trigger_r2", "xbox_rt", "steamdeck_button_r2"}, {"playstation_trigger_r2", "xbox_rt", "steamdeck_button_r2"}},
};
constexpr const char* kPadSticks[][3] = {  // Input::Stick (aiming): left, right
    {"playstation_stick_l", "xbox_stick_l", "steamdeck_stick_l"},
    {"playstation_stick_r", "xbox_stick_r", "steamdeck_stick_r"},
};
constexpr const char* kStylePrefix[3] = {"playstation", "xbox", "steamdeck"};  // (pad_icon_named)

// Keys: "keyboard_" and the key's SDL name in lower case ("A", "F1", "Space", "Return"), except these.
constexpr struct { SDL_Scancode key; const char* icon; } kKeyIcons[] = {
    {SDL_SCANCODE_UP, "keyboard_arrow_up"}, {SDL_SCANCODE_DOWN, "keyboard_arrow_down"},
    {SDL_SCANCODE_LEFT, "keyboard_arrow_left"}, {SDL_SCANCODE_RIGHT, "keyboard_arrow_right"},
    {SDL_SCANCODE_LSHIFT, "keyboard_shift"}, {SDL_SCANCODE_RSHIFT, "keyboard_shift"},
    {SDL_SCANCODE_LCTRL, "keyboard_ctrl"}, {SDL_SCANCODE_RCTRL, "keyboard_ctrl"},
    {SDL_SCANCODE_LALT, "keyboard_alt"}, {SDL_SCANCODE_RALT, "keyboard_alt"},
    {SDL_SCANCODE_LGUI, "keyboard_win"}, {SDL_SCANCODE_RGUI, "keyboard_win"},
    {SDL_SCANCODE_PAGEUP, "keyboard_page_up"}, {SDL_SCANCODE_PAGEDOWN, "keyboard_page_down"},
    {SDL_SCANCODE_MINUS, "keyboard_minus"}, {SDL_SCANCODE_EQUALS, "keyboard_equals"},
    {SDL_SCANCODE_LEFTBRACKET, "keyboard_bracket_open"}, {SDL_SCANCODE_RIGHTBRACKET, "keyboard_bracket_close"},
    {SDL_SCANCODE_SEMICOLON, "keyboard_semicolon"}, {SDL_SCANCODE_APOSTROPHE, "keyboard_apostrophe"},
    {SDL_SCANCODE_COMMA, "keyboard_comma"}, {SDL_SCANCODE_PERIOD, "keyboard_period"},
    {SDL_SCANCODE_SLASH, "keyboard_slash_forward"}, {SDL_SCANCODE_BACKSLASH, "keyboard_slash_back"},
    {SDL_SCANCODE_GRAVE, "keyboard_tilde"}, {SDL_SCANCODE_KP_ENTER, "keyboard_numpad_enter"},
    {SDL_SCANCODE_KP_PLUS, "keyboard_numpad_plus"}, {SDL_SCANCODE_KP_MULTIPLY, "keyboard_asterisk"},
};
constexpr const char* kMouseIcons[] = {nullptr, "mouse_left", "mouse_scroll", "mouse_right", "mouse_side_back",
                                       "mouse_side_forward"};  // by mouse button (1-5)

// Auto: the connected controller's style. Valve's (the Steam Deck, and Steam's virtual pads on it) by
// its vendor; the rest by the type SDL gives it; anything else looks like an Xbox pad.
constexpr struct { SDL_GamepadType type; PromptStyle style; } kPadTypes[] = {
    {SDL_GAMEPAD_TYPE_PS3, PromptStyle::PlayStation}, {SDL_GAMEPAD_TYPE_PS4, PromptStyle::PlayStation},
    {SDL_GAMEPAD_TYPE_PS5, PromptStyle::PlayStation}, {SDL_GAMEPAD_TYPE_XBOX360, PromptStyle::Xbox},
    {SDL_GAMEPAD_TYPE_XBOXONE, PromptStyle::Xbox},
};
constexpr Uint16 kValveVendor = 0x28DE;

int style_column() { return (int)prompt_style() - 1; }

PromptIcon icon_or_name(const char* icon, const Input& in) {
    return icon && ui_icon_exists(icon) ? PromptIcon{icon, ""} : PromptIcon{"", input_name(in)};
}

// One icon (or a key cap with the input's name) `h` high at x, y; returns its width.
float draw_icon(SDL_Renderer* r, const PromptIcon& i, float x, float y, float h, bool draw) {
    if (!i.icon.empty()) {
        if (draw) ui_icon(r, i.icon.c_str(), x, y, h);
        return h;
    }
    const float size = h * 0.6f, w = ui_text_width(r, i.text.c_str(), size) + h * 0.5f;
    if (draw) {
        Uint8 cr, cg, cb, ca;
        SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
        SDL_SetRenderDrawColor(r, 255, 255, 255, ca);
        const SDL_FRect cap{x + 1, y + h * 0.12f, w - 2, h * 0.76f};
        SDL_RenderRect(r, &cap);
        ui_text(r, x + h * 0.25f, y + (h - size) * 0.5f, i.text.c_str(), size);
        SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    }
    return w;
}

// "[pad] / [key]" (one of them when the other is empty); returns its width.
float draw_prompt(SDL_Renderer* r, const Prompt& p, float x, float y, float h, bool draw) {
    float w = 0;
    if (!p.pad.empty()) w += draw_icon(r, p.pad, x, y, h, draw);
    if (!p.pad.empty() && !p.key.empty()) {
        const float size = h * 0.7f, sw = ui_text_width(r, "/", size);
        if (draw) ui_text(r, x + w + h * 0.1f, y + (h - size) * 0.5f, "/", size);
        w += sw + h * 0.2f;
    }
    if (!p.key.empty()) w += draw_icon(r, p.key, x + w, y, h, draw);
    return w;
}

// host_prompt_hint's line
struct PromptHint {
    std::string before, after;
    int action = -1;
    uint64_t until_ns = 0;
};
PromptHint g_hint;

// A game screen position (pixels of the display area) -> the overlay's 640x480 units: as gpu.cpp
// squeeze_2d narrows the game's 2D in widescreen, then where the image is in the window.
void game_to_overlay(const GpuDisplay& d, float gx, float gy, float* ox, float* oy) {
    int ww, wh;
    SDL_GetWindowSizeInPixels(pres::g_win, &ww, &wh);
    float ax, ay, aw, ah;
    pres::overlay_area((float)ww, (float)wh, &ax, &ay, &aw, &ah);
    const float k = replace_original_picture_shown(d.y) ? 1.0f : g_gte_widescreen;  // (shown at 4:3)
    const float mid = (float)d.w * 0.5f, sx = mid + (gx - mid) * k;
    *ox = (pres::g_cx + sx / (float)d.w * pres::g_cw - ax) / aw * 640.0f;
    *oy = (pres::g_cy + gy / (float)d.h * pres::g_ch - ay) / ah * 480.0f;
}

}  // namespace

PromptStyle prompt_style() {
    if (g_prompt_style != PromptStyle::Auto) return g_prompt_style;
    for (const InputDevice& d : g_devices) {
        if (!d.pad) break;
        if (SDL_GetGamepadVendor(d.pad) == kValveVendor) return PromptStyle::SteamDeck;
        const SDL_GamepadType t = SDL_GetGamepadType(d.pad);
        for (const auto& k : kPadTypes)
            if (k.type == t) return k.style;
        break;
    }
    return PromptStyle::Xbox;
}

PromptIcon pad_icon(const Input& in) {
    if (g_devices.empty()) return {};
    const int s = style_column();
    switch (in.kind) {
        case Input::Button:
            return icon_or_name(in.code >= 0 && in.code < (int)std::size(kPadButtons) ? kPadButtons[in.code][s] : nullptr, in);
        case Input::Axis:
            return icon_or_name(in.code >= 0 && in.code < (int)std::size(kPadAxes) ? kPadAxes[in.code][in.dir > 0][s] : nullptr, in);
        case Input::Stick: return icon_or_name(kPadSticks[in.code ? 1 : 0][s], in);
        case Input::None: return {};
        default: return key_icon(in);
    }
}

PromptIcon key_icon(const Input& in) {
    switch (in.kind) {
        case Input::Key: {
            for (const auto& k : kKeyIcons)
                if (k.key == in.code) return icon_or_name(k.icon, in);
            std::string name = "keyboard_";
            for (const char* p = SDL_GetScancodeName((SDL_Scancode)in.code); p && *p; p++)
                name += *p == ' ' ? '_' : (char)std::tolower((unsigned char)*p);
            return icon_or_name(name.c_str(), in);
        }
        case Input::Mouse: return icon_or_name(in.code >= 1 && in.code <= 5 ? kMouseIcons[in.code] : nullptr, in);
        case Input::Wheel: return icon_or_name("mouse_scroll_vertical", in);
        case Input::None: return {};
        default: return pad_icon(in);
    }
}

PromptIcon pad_icon_named(const char* what) {
    if (g_devices.empty()) return {};
    return {std::string(kStylePrefix[style_column()]) + "_" + what, ""};
}

Prompt prompt_for_action(int a) {
    Prompt p;
    if (a < 0 || a >= A_COUNT) return p;
    for (const Input& in : g_binds[M_CONTROLLER][a])
        if (p.pad.empty()) p.pad = pad_icon(in);
    for (const Input& in : g_binds[M_KEYBOARD][a])
        if (p.key.empty()) p.key = key_icon(in);
    return p;
}

Prompt prompt_for_button(uint16_t bit) {
    for (int a = 0; a < A_COUNT; a++)
        if (action_bit(a) == bit) return prompt_for_action(a);
    return {};
}

float draw_hints(SDL_Renderer* r, float x, float y, float h, const std::vector<Hint>& hints, bool draw) {
    const float size = h * 0.72f;
    float w = 0;
    for (const Hint& hint : hints) {
        if (w > 0) w += h * 0.9f;
        const float pw = draw_prompt(r, hint.prompt, x + w, y, h, draw);
        if (pw > 0) w += pw + h * 0.25f;
        if (draw) ui_text(r, x + w, y + (h - size) * 0.5f, hint.label.c_str(), size);
        w += ui_text_width(r, hint.label.c_str(), size);
    }
    return w;
}

void draw_game_prompts() {
    const std::vector<ScreenPrompt> game = gpu_prompts();
    const bool hint = g_hint.action >= 0 && SDL_GetTicksNS() < g_hint.until_ns;
    if (game.empty() && !hint) return;
    SDL_Renderer* r = present_overlay_begin();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    const GpuDisplay d = gpu_display();
    for (const ScreenPrompt& p : game) {
        if (d.w <= 0 || d.h <= 0) break;
        float x0, y0, x1, y1;
        game_to_overlay(d, (float)p.x0, (float)p.y0, &x0, &y0);
        game_to_overlay(d, (float)p.x1, (float)p.y1, &x1, &y1);
        const Prompt prompt = prompt_for_button(p.button);
        // a legend's pair ends where the game's icon did (its text follows); PRESS START's follows the text
        float h = (y1 - y0) * (p.after ? 1.15f : 1.05f);
        if (!p.after) h *= std::min(1.0f, (x1 - 2) / std::max(1.0f, draw_prompt(r, prompt, 0, 0, h, false)));  // (on screen)
        const float x = p.after ? x1 + h * 0.25f : x1 - draw_prompt(r, prompt, 0, 0, h, false);
        draw_prompt(r, prompt, x, (y0 + y1 - h) * 0.5f, h, true);
    }
    if (hint) {  // a line in a dark box at the bottom
        constexpr float kH = 22.0f, kText = kH * 0.72f, kGap = kH * 0.3f;
        const Prompt prompt = prompt_for_action(g_hint.action);
        const float bw = ui_text_width(r, g_hint.before.c_str(), kText), pw = draw_prompt(r, prompt, 0, 0, kH, false);
        const float w = bw + kGap + pw + kGap + ui_text_width(r, g_hint.after.c_str(), kText), x = (640 - w) * 0.5f, y = 440;
        const SDL_FRect box{x - 10, y - 4, w + 20, kH + 8};
        SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
        SDL_RenderFillRect(r, &box);
        SDL_SetRenderDrawColor(r, 255, 230, 180, 255);
        ui_text(r, x, y + (kH - kText) * 0.5f, g_hint.before.c_str(), kText);
        draw_prompt(r, prompt, x + bw + kGap, y, kH, true);
        ui_text(r, x + bw + kGap + pw + kGap, y + (kH - kText) * 0.5f, g_hint.after.c_str(), kText);
    }
    present_overlay_end();
}

}  // namespace host

void host_prompt_hint(const std::string& before, int action, const std::string& after) {
    host::g_hint = {before, after, action, SDL_GetTicksNS() + 4000000000ull};
}

void host_rewind_hint() { host_prompt_hint("Hold", host::A_REWIND, "to rewind"); }
