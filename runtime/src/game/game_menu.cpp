// The game's own menus (front end and pause):
//  - Font upscaling: each menu is drawn at g_menu_zoom percent around its middle (text size, line
//    spacing and gaps), without changing the menu itself.
//  - Recomp Enhancements (main menu and pause menu, before "options"): a list of pages of our
//    settings - performance, cosmetics, cheats (host/settings.cpp enh_*) - drawn and driven as
//    menus of the game's own.
//  - Pause menu: "Options" (a list of pages of the game's own options: sound, game, controller
//    config, and "controls": our controls screen, host/settings_menu.cpp) and "Exit to desktop";
//    main menu: "exit to desktop", and "controls" in Recomp Enhancements.
//  - Main Menu -> Load Game: "Auto Save" at the top of the memory card saves (savestate.cpp).
//  - game_message: text shown the way the game shows a pickup ("Auto Saved").
//
// A menu (built with alloc(444), 80015228 init, 80015474 add item) is:
//   +4 u8 alignment, +6 u8 selected item, +10 u8 item count, +12 x, +16 y (first item),
//   +20 line spacing, then 15 items of 28 bytes from +24:
//   +0 text, +4 s16 scale now, +6 s16 scale, +8 s16 scale when selected, +10 s16 scale when not,
//   +12 u8 gap above, +13 u8 enabled.
// Scales are 8.8 fixed point (256 = the font's size).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <ctime>
#include <string>
#include <vector>

#include "core/psx.h"
#include "core/savestate.h"
#include "game/game.h"
#include "host/host.h"

int g_menu_zoom = 60;
bool g_menu_flames = true;  // setting (host.h): Remastered
int psx_opt_menu60 = 0;
int psx_opt_menu_odd = 0;

namespace {

constexpr uint32_t kStrings = 0x800A54E4;  // the game's text table
constexpr int kStrSelectLevel = 38, kStrLoadGame = 60;
constexpr uint32_t kMenuInit = 0x80015228, kMenuAdd = 0x80015474, kMenuLayout = 0x800152F8,
                   kMenuSelect = 0x80015374, kMenuDraw = 0x800156E0, kMenuKeys = 0x80017364,
                   kAlloc = 0x8002FED8, kFree = 0x8002FF34;
constexpr uint32_t kItems = 24, kItemSize = 28, kMaxItems = 15;
constexpr uint32_t kTextSize = 40;  // bytes per item text in our block
constexpr uint16_t kPadLeft = 1 << 7, kPadRight = 1 << 5;
// the front end (80011BA0, its menus' loop) and what its 60 Hz mode keeps at the game's pace
constexpr uint32_t kFrontEnd = 0x80011BA0, kFrontEndEnd = 0x80014700;
constexpr uint32_t kButtons = 0x800EC0F8;  // 16 bytes a button: +0 held, +1 pressed
constexpr uint32_t kGpRepeat = 0x800FEEBC + 344, kUpdateCount = 0x800FF2F0;

// Our pages: their settings (host/settings.cpp enh_*), in order.
constexpr int kPerformanceSettings[] = {E_RENDERER, E_SCREEN, E_DISPLAY, E_AA, E_TEX_SMOOTH, E_REPLACE, E_DRAWDIST, E_FPS, E_AUTOSAVE};
constexpr int kCosmeticSettings[] = {E_SUBTITLES, E_CRT, E_GAME_FONT, E_FONT_SCALE, E_MENU_FLAMES, E_FPS_COUNTER, E_SOUND_FILTER};
constexpr int kCheatSettings[] = {E_CHEAT_HEALTH, E_CHEAT_LIVES, E_CHEAT_WEAPONS, E_CHEAT_AMMO, E_CHEAT_BOMBS, E_CHEAT_ONEHIT};
// The game's own options, for the pause menu (the front end has its own pages for them); ids
// after the enhancement settings. Found in the front end (80011BA0) and 80016014 (options from
// a save): the volumes are s16 at 800ECC78.., difficulty and vibration words.
enum GameOpt { G_FX = 1000, G_MUSIC, G_VOICE, G_DIFFICULTY, G_VIBRATION, G_PAD_WEAPON, G_PAD_CROUCH, G_PAD_JUMP, G_PAD_BOMB };
constexpr int kSoundSettings[] = {G_FX, G_MUSIC, G_VOICE};
constexpr int kGameSettings[] = {G_DIFFICULTY, G_VIBRATION};
constexpr int kPadSettings[] = {G_PAD_WEAPON, G_PAD_CROUCH, G_PAD_JUMP, G_PAD_BOMB};
// The pages come in groups, each an item of a game menu that opens the list of its pages:
// Recomp Enhancements (main menu and pause menu) and Options (pause menu: the front end has its own).
enum Group { kGroupEnhancements, kGroupOptions, kGroups };
constexpr const char* kGroupLabels[kGroups] = {"Recomp Enhancements", "Options"};
struct Page {
    const char* label;
    const int* settings;
    int count;
    Group group;
    bool main_menu = false;  // also in the main menu's Recomp Enhancements (it has no Options of ours)
};
// (no settings: the item opens the controls screen, host/settings_menu.cpp)
constexpr Page g_pages[] = {
    {"performance", kPerformanceSettings, (int)std::size(kPerformanceSettings), kGroupEnhancements},
    {"cosmetics", kCosmeticSettings, (int)std::size(kCosmeticSettings), kGroupEnhancements},
    {"cheats", kCheatSettings, (int)std::size(kCheatSettings), kGroupEnhancements},
    {"sound", kSoundSettings, (int)std::size(kSoundSettings), kGroupOptions},
    {"game", kGameSettings, (int)std::size(kGameSettings), kGroupOptions},
    {"controller config", kPadSettings, (int)std::size(kPadSettings), kGroupOptions},
    {"controls", nullptr, 0, kGroupOptions, true},
};
constexpr int kPages = (int)std::size(g_pages);

// A game menu our menus are shown in place of: the main menu or the pause menu. A group's list
// and a page are made in the game's heap while they are open (the heap in a level is small).
// Our item texts: the groups' labels, the pages' labels, "back" (a page), the lists' way back,
// "Exit to desktop" (pause menu).
enum Text { kTextGroup = 0, kTextPage = kTextGroup + kGroups, kTextBack = kTextPage + kPages, kTextListBack, kTextExit, kTexts };
struct Host {
    uint32_t menu = 0;
    uint32_t texts = 0;           // our item texts (Text)
    bool groups[kGroups] = {};    // the groups it offers
    int group = -1;               // the group whose list is shown, -1 = none
    uint32_t list = 0;            // its list
    int page = -1;                // the page shown, -1 = none
    uint32_t pages[kPages] = {};  // (only the one shown is made)
    bool drawn = false;           // the menu was drawn since the last key read
};
Host g_pause, g_mainh;
uint32_t host_text(const Host& h, int t) { return h.texts + kTextSize * (uint32_t)t; }

constexpr uint32_t kPauseMade = 0x80017B40;  // return address of the pause menu's "Quit" menu_add

// Main menu: "exit to desktop" at the end, added at its first draw.
constexpr uint32_t kMainMade = 0x80011F38;  // return address of the main menu's "new game" menu_add
uint32_t g_main = 0, g_main_text = 0;
bool g_main_drawn = false;

// A theme song in the main menu (the original's is silent): level 3's (plague; the table at
// 800A5254, 24 bytes an entry, +8 its name: prison sewer plague death rooftops cemetary plague
// warfact warboss backwall beast), loaded and played the way Options -> Sound's music test does
// (80015F48 stops the song and loads NAME.VAB/.SEQ, 8006A428 plays it at the music level).
// A level loads its own.
constexpr uint32_t kSongs = 0x800A5254, kSongLoad = 0x80015F48, kSongPlay = 0x8006A428,
                   kSongStop = 0x8006A3F0;
bool g_song_started = false;  // for this main menu

// Load Game: the game lists the memory card saves in a menu and loads the one whose number is
// selected, so the list is shown through a stand-in menu with "Auto Save" at the top; choosing a
// card save selects it in the game's menu, choosing Auto Save loads the auto-save's record as a
// card save (autoload_start).
constexpr uint32_t kLoadMenuMade = 0x80014388;  // return address of the front end's init of that menu
constexpr int kStrEmpty = 95;
uint32_t g_load = 0, g_proxy = 0;  // the game's Load Game menu, and ours (with its texts after it)
bool g_load_drawn = false;         // Load Game was drawn since the last key read

uint32_t text_of(R3000* c, int id) { return MEM_R32(c, kStrings + 4 * (uint32_t)id); }

std::string read_text(R3000* c, uint32_t at, uint32_t max = 64) {
    std::string s;
    for (uint32_t i = 0; i < max; i++) {
        const char ch = (char)MEM_R8(c, at + i);
        if (!ch) break;
        s += ch;
    }
    return s;
}
uint32_t item(uint32_t menu, int i) { return menu + kItems + kItemSize * (uint32_t)i; }

void write_text(R3000* c, uint32_t at, const std::string& s, uint32_t size) {
    uint32_t i = 0;
    for (; i < s.size() && i + 1 < size; i++) MEM_W8(c, at + i, (uint8_t)s[i]);
    MEM_W8(c, at + i, 0);
}

// Calls a game function with arguments (stack arguments from sp+16), keeping every register.
uint32_t game_call(R3000* c, uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0,
                   std::initializer_list<uint32_t> stack = {}) {
    uint32_t r[32];
    std::memcpy(r, c->r, sizeof r);
    const uint32_t hi = c->hi, lo = c->lo;
    c->r[29] -= 64;
    uint32_t at = c->r[29] + 16;
    for (uint32_t v : stack) MEM_W32(c, at, v), at += 4;
    c->r[4] = a0, c->r[5] = a1, c->r[6] = a2, c->r[7] = a3;
    call_indirect(c, fn);
    const uint32_t v0 = c->r[2];
    std::memcpy(c->r, r, sizeof r);
    c->hi = hi, c->lo = lo;
    return v0;
}

// The game's options (GameOpt): address, step, largest value (the front end's own steps).
struct GameOptInfo { const char* name; uint32_t addr; int step, max; bool word; };
constexpr GameOptInfo kGameOpts[] = {
    {"fx level", 0x800ECC7A, 1024, 16383, false},
    {"music level", 0x800ECC78, 8, 127, false},
    {"voice level", 0x800ECC7C, 16, 255, false},
    {"difficulty", 0x800FF384, 1, 2, true},  // easy, normal, hard
    {"vibration", 0x800FF2FC, 1, 2, true},   // none, some, full
    // controller config (front end 800139B8): the shoulder button of each action, one bit each
    // (1 l2, 2 r2, 4 l1, 8 r1), halfwords in the options block (pad_offset)
    {"weapon select", 0, 0, 0, false},
    {"crouch", 0, 0, 0, false},
    {"jump", 0, 0, 0, false},
    {"smart bomb", 0, 0, 0, false},
};
// The options block's halfword of controller-config action k: the front end edits the digital set
// for a digital pad (id 0x41 at button table +364), the analog set otherwise.
constexpr uint32_t kPadOptions = 0x800EC474, kButtonTable = 0x800EC0F8;
uint32_t pad_offset(R3000* c, int k) {
    static const uint32_t digital[4] = {6, 0, 2, 4}, analog[4] = {38, 32, 34, 36};
    return kPadOptions + (MEM_R32(c, kButtonTable + 364) == 0x41 ? digital : analog)[k];
}
}  // namespace

// The shoulder button the game's controller config gives action k (0 weapon select, 1 crouch, 2 jump,
// 3 smart bomb): 1 L2, 2 R2, 4 L1, 8 R1; 0 = not set up yet.
int game_pad_action(R3000* c, int k) {
    const uint16_t v = (uint16_t)MEM_R16(c, pad_offset(c, k));
    return v == 1 || v == 2 || v == 4 || v == 8 ? v : 0;
}

namespace {
constexpr uint32_t kSeqSetVol = 0x8008F56C, kGpSeq = 0x800FEEBC + 2032;  // music: (seq, left, right)

int game_opt_get(R3000* c, const GameOptInfo& o) { return o.word ? (int32_t)MEM_R32(c, o.addr) : (int16_t)MEM_R16(c, o.addr); }

std::string opt_name(int id) { return id >= G_FX ? kGameOpts[id - G_FX].name : enh_name(id); }

std::string opt_value(R3000* c, int id) {
    if (id < G_FX) return enh_value(id);
    const GameOptInfo& o = kGameOpts[id - G_FX];
    const int v = game_opt_get(c, o);
    if (id >= G_PAD_WEAPON) {
        switch (MEM_R16(c, pad_offset(c, id - G_PAD_WEAPON))) {
            case 1: return "l2";
            case 2: return "r2";
            case 4: return "l1";
            case 8: return "r1";
        }
        return "";
    }
    if (id == G_DIFFICULTY || id == G_VIBRATION) {
        static const char* names[2][3] = {{"easy", "normal", "hard"}, {"none", "some", "full"}};
        return names[id == G_VIBRATION][std::clamp(v, 0, 2)];
    }
    return std::to_string((v * 100 + o.max / 2) / o.max) + "%";
}

void opt_change(R3000* c, int id, int dir) {
    if (id < G_FX) return enh_change(id, dir);
    if (id >= G_PAD_WEAPON) {
        // next button (l1 r1 l2 r2); the action that had it takes this one's (as the front end's
        // 80011860), then the button table is rebuilt as the game does after loading options (800710B4)
        static const uint16_t order[4] = {4, 8, 1, 2};
        const uint32_t at = pad_offset(c, id - G_PAD_WEAPON);
        const uint16_t cur = MEM_R16(c, at);
        int k = 0;
        while (k < 4 && order[k] != cur) k++;
        const uint16_t next = order[(k + (dir > 0 ? 1 : 3)) % 4];
        for (int j = 0; j < 4; j++)
            if (pad_offset(c, j) != at && MEM_R16(c, pad_offset(c, j)) == next) MEM_W16(c, pad_offset(c, j), cur);
        MEM_W16(c, at, next);
        auto h = [c](uint32_t off) { return (uint32_t)(int32_t)(int16_t)MEM_R16(c, kPadOptions + off); };
        game_call(c, 0x8006FDFC, kButtonTable, h(0), h(2), h(4), {h(6)});
        game_call(c, 0x8006FE14, kButtonTable, 3, 2, 1, {0, h(32), h(34), h(36), h(38)});
        return;
    }
    const GameOptInfo& o = kGameOpts[id - G_FX];
    int v = game_opt_get(c, o) + dir * o.step;
    if (o.word) v = (v + o.max + 1) % (o.max + 1);  // choices go round
    else v = std::clamp(v, 0, o.max);
    if (o.word) MEM_W32(c, o.addr, (uint32_t)v);
    else MEM_W16(c, o.addr, (uint16_t)v);
    // the music's volume is set on the playing sequence (the front end's 8006A428 would also
    // restart a paused one); the other volumes are read when a sound starts
    const int seq = (int16_t)MEM_R16(c, kGpSeq);
    if (id == G_MUSIC && seq >= 0 && seq < 32) game_call(c, kSeqSetVol, (uint32_t)seq, (uint32_t)v, (uint32_t)v);
}

// A page's item texts: "setting  value", in characters the game's font has.
void refresh_texts(R3000* c, uint32_t block, const Page& p) {
    for (int i = 0; i < p.count; i++) {
        std::string s = opt_name(p.settings[i]) + "  " + opt_value(c, p.settings[i]);
        s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return ch == '(' || ch == ')'; }), s.end());
        std::replace(s.begin(), s.end(), ':', 'x');  // the font has no ':' ("16:9" -> "16x9")
        for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
        write_text(c, block + 444 + kTextSize * (uint32_t)i, s, kTextSize);
    }
}

// Starts `menu` in the same place, alignment, scales and spacing as the game's menu `like`.
void init_like(R3000* c, uint32_t menu, uint32_t like) {
    const uint32_t it = item(like, 0);
    game_call(c, kMenuInit, menu, MEM_R32(c, like + 12), MEM_R32(c, like + 16), MEM_R8(c, like + 4),
              {(uint32_t)(int16_t)MEM_R16(c, it + 8), (uint32_t)(int16_t)MEM_R16(c, it + 10), MEM_R32(c, like + 20)});
}

// Our texts for a host: lower case in the main menu (as its own items), as written in the pause menu.
uint32_t make_texts(R3000* c, Host& h, bool lower_case) {
    h.texts = game_call(c, kAlloc, kTextSize * kTexts);
    if (!h.texts) return 0;
    auto put = [&](int t, std::string s) {
        if (lower_case) for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
        write_text(c, host_text(h, t), s, kTextSize);
    };
    for (int g = 0; g < kGroups; g++) put(kTextGroup + g, kGroupLabels[g]);
    for (int i = 0; i < kPages; i++) put(kTextPage + i, g_pages[i].label);
    put(kTextBack, "back");
    put(kTextListBack, lower_case ? "return to main menu" : "return to pause menu");
    put(kTextExit, "Exit to desktop");
    return h.texts;
}

// A list or page menu for `h`: in its menu's place, text size and spacing (Font upscaling then
// sizes it as every menu); 0 if the heap is full.
uint32_t start_menu(R3000* c, const Host& h, uint32_t bytes) {
    const uint32_t block = game_call(c, kAlloc, bytes);
    if (block) init_like(c, block, h.menu);
    return block;
}
void finish_menu(R3000* c, uint32_t block) {
    game_call(c, kMenuLayout, block);  // centred up and down
    game_call(c, kMenuSelect, block, 0);
}

// The pages of group `g`, in order.
int group_pages(int g, int* out, bool main_menu) {
    int n = 0;
    for (int i = 0; i < kPages; i++)
        if (g_pages[i].group == g || (main_menu && g == kGroupEnhancements && g_pages[i].main_menu)) out[n++] = i;
    return n;
}

// Adds an item to the end of a laid out menu, keeping it centred (half a line up), unless it is
// in it. (Done at each draw: a draw may be one the 60 Hz renderer rolls back, sim60.cpp.)
void append_item(R3000* c, uint32_t menu, uint32_t text) {
    const int n = MEM_R8(c, menu + 10);
    if (n >= (int)kMaxItems) return;
    for (int i = 0; i < n; i++)
        if (MEM_R32(c, item(menu, i)) == text) return;
    game_call(c, kMenuAdd, menu, text);
    MEM_W32(c, menu + 16, (uint32_t)((int32_t)MEM_R32(c, menu + 16) - (int32_t)MEM_R32(c, menu + 20) / 2));
}

// Moves the item with `text` to just after (or before) the one with `anchor` (no change once it
// is there).
void move_to(R3000* c, uint32_t menu, uint32_t text, uint32_t anchor, bool after = true) {
    const int n = std::min<int>(MEM_R8(c, menu + 10), kMaxItems);
    int from = -1, to = -1;
    for (int i = 0; i < n; i++) {
        if (MEM_R32(c, item(menu, i)) == text) from = i;
        if (MEM_R32(c, item(menu, i)) == anchor) to = after ? i + 1 : i;
    }
    if (from < 0 || to < 0 || from == to) return;
    uint8_t moved[kItemSize];
    for (uint32_t b = 0; b < kItemSize; b++) moved[b] = (uint8_t)MEM_R8(c, item(menu, from) + b);
    const int step = from < to ? 1 : -1;
    if (from < to) to--;  // (the items between shift up)
    for (int i = from; i != to; i += step)
        for (uint32_t b = 0; b < kItemSize; b++) MEM_W8(c, item(menu, i) + b, MEM_R8(c, item(menu, i + step) + b));
    for (uint32_t b = 0; b < kItemSize; b++) MEM_W8(c, item(menu, to) + b, moved[b]);
}

// The menu shown in place of the host's.
uint32_t shown_menu(const Host& h) {
    if (h.page >= 0) return h.pages[h.page];
    return h.group >= 0 ? h.list : h.menu;
}

void open_page(R3000* c, Host& h, int i) {
    const Page& p = g_pages[i];
    const uint32_t block = start_menu(c, h, 444 + kTextSize * (uint32_t)p.count);
    if (!block) return;
    refresh_texts(c, block, p);
    for (int k = 0; k < p.count; k++) game_call(c, kMenuAdd, block, block + 444 + kTextSize * (uint32_t)k);
    game_call(c, kMenuAdd, block, host_text(h, kTextBack));
    finish_menu(c, block);
    h.pages[i] = block;
    h.page = i;
}
void close_page(R3000* c, Host& h) {
    if (h.page < 0) return;
    if (h.pages[h.page]) game_call(c, kFree, h.pages[h.page]), h.pages[h.page] = 0;
    h.page = -1;
}

void open_list(R3000* c, Host& h, int g) {
    const uint32_t block = start_menu(c, h, 444);
    if (!block) return;
    int pages[kPages];
    const int n = group_pages(g, pages, &h == &g_mainh);
    for (int k = 0; k < n; k++) game_call(c, kMenuAdd, block, host_text(h, kTextPage + pages[k]));
    game_call(c, kMenuAdd, block, host_text(h, kTextListBack));
    finish_menu(c, block);
    h.list = block;
    h.group = g;
}
void close_list(R3000* c, Host& h) {
    close_page(c, h);
    if (h.list) game_call(c, kFree, h.list), h.list = 0;
    h.group = -1;
}

// Makes our Load Game menu show "Auto Save" and then the game's list, and keeps the game's
// selection on the card save ours has selected.
void sync_load_menu(R3000* c) {
    const int n = std::min<int>(MEM_R8(c, g_load + 10), kMaxItems - 1);
    const int32_t spacing = (int32_t)MEM_R32(c, g_load + 20);
    if (!g_proxy) {
        g_proxy = game_call(c, kAlloc, 444 + kTextSize * kMaxItems);
        if (!g_proxy) return;
        const uint32_t it = item(g_load, 0);
        game_call(c, kMenuInit, g_proxy, MEM_R32(c, g_load + 12), MEM_R32(c, g_load + 16), MEM_R8(c, g_load + 4),
                  {(uint32_t)(int16_t)MEM_R16(c, it + 8), (uint32_t)(int16_t)MEM_R16(c, it + 10), (uint32_t)spacing});
    }
    const uint32_t texts = g_proxy + 444;
    const std::string level = state_autosave_level();
    write_text(c, texts, !state_slot_exists(kAutoSlot) ? "auto save  empty" : "auto save  " + level, kTextSize);
    MEM_W32(c, item(g_proxy, 0), texts);
    MEM_W8(c, item(g_proxy, 0) + 12, 0);
    MEM_W8(c, item(g_proxy, 0) + 13, 1);
    const bool numbered = MEM_R8(c, g_load + 9) != 0;  // "Sewers 1": the game adds the slot number
    for (int i = 0; i < n; i++) {
        const uint32_t from = item(g_load, i), to = item(g_proxy, i + 1), text = texts + kTextSize * (uint32_t)(i + 1);
        const uint32_t src = MEM_R32(c, from);
        std::string s = src ? read_text(c, src) : std::string();
        if (numbered && s != read_text(c, text_of(c, kStrEmpty))) s += std::string(" ") + (char)('1' + i);
        write_text(c, text, s, kTextSize);
        MEM_W32(c, to, text);
        MEM_W8(c, to + 12, MEM_R8(c, from + 12));
        MEM_W8(c, to + 13, MEM_R8(c, from + 13));
    }
    MEM_W8(c, g_proxy + 10, (uint8_t)(n + 1));
    MEM_W32(c, g_proxy + 12, MEM_R32(c, g_load + 12));
    MEM_W32(c, g_proxy + 16, (uint32_t)((int32_t)MEM_R32(c, g_load + 16) - spacing / 2));  // one more line: half up
    const int sel = std::min<int>(MEM_R8(c, g_proxy + 6), n);
    MEM_W8(c, g_proxy + 6, (uint8_t)sel);
    MEM_W8(c, g_load + 6, (uint8_t)std::max(sel - 1, 0));
}

bool selected_is(R3000* c, uint32_t menu, uint32_t text) {
    return MEM_R32(c, item(menu, MEM_R8(c, menu + 6))) == text;
}

// Draws `menu` at g_menu_zoom percent around its middle: its fields are scaled for the draw and
// put back after. window: a menu of more items shows only that many, the selected one among them
// (they scroll with the selection), in the middle of where the whole menu would be.
void draw_zoomed(R3000* c, uint32_t menu, int window = 0) {
    const int n = std::min<int>(MEM_R8(c, menu + 10), kMaxItems);
    const bool scroll = window > 0 && n > window;
    if ((g_menu_zoom >= 100 && !scroll) || n <= 0) {
        call_indirect(c, kMenuDraw);
        return;
    }
    const float f = (float)std::min(g_menu_zoom, 100) / 100.0f;
    auto scaled = [f](int v) { return (int)std::lround((float)v * f); };
    const int32_t y = (int32_t)MEM_R32(c, menu + 16), spacing = (int32_t)MEM_R32(c, menu + 20);
    const uint8_t sel = MEM_R8(c, menu + 6);
    uint8_t saved[kMaxItems][kItemSize];
    for (int i = 0; i < n; i++)
        for (uint32_t k = 0; k < kItemSize; k++) saved[i][k] = (uint8_t)MEM_R8(c, item(menu, i) + k);
    // the height of items [from, to) as laid out (gaps above and the spacing between shown ones)
    auto height = [&](int from, int to, int line, bool scale) {
        int h = 0, shown = 0;
        for (int i = from; i < to; i++)
            if (saved[i][13]) h += scale ? scaled(saved[i][12]) : saved[i][12], shown++;
        return h + std::max(0, shown - 1) * line;
    };
    const int middle = y + height(0, n, spacing, false) / 2;
    int top = 0, count = n;
    if (scroll) {
        top = std::clamp((int)sel - window / 2, 0, n - window), count = window;
        for (int i = 0; i < count; i++)
            for (uint32_t k = 0; k < kItemSize; k++) MEM_W8(c, item(menu, i) + k, saved[top + i][k]);
        MEM_W8(c, menu + 10, (uint8_t)count);
        MEM_W8(c, menu + 6, (uint8_t)(sel - top));
    }
    MEM_W32(c, menu + 16, (uint32_t)(middle - height(top, top + count, scaled(spacing), true) / 2));
    MEM_W32(c, menu + 20, (uint32_t)scaled(spacing));
    for (int i = 0; i < count; i++) {
        const uint32_t it = item(menu, i);
        for (int k = 0; k < 4; k++) MEM_W16(c, it + 4 + 2 * k, (uint16_t)scaled((int16_t)MEM_R16(c, it + 4 + 2 * k)));
        MEM_W8(c, it + 12, (uint8_t)scaled(MEM_R8(c, it + 12)));
    }
    call_indirect(c, kMenuDraw);
    MEM_W32(c, menu + 16, (uint32_t)y);
    MEM_W32(c, menu + 20, (uint32_t)spacing);
    MEM_W8(c, menu + 10, (uint8_t)n);
    MEM_W8(c, menu + 6, sel);
    // what the draw changed in an item (the selected one's flames) stays; only the scales and the
    // gap we set go back, and the items the window moved go back to their places
    uint8_t drawn_items[kMaxItems][kItemSize];
    for (int i = 0; i < count; i++)
        for (uint32_t k = 0; k < kItemSize; k++) drawn_items[i][k] = (uint8_t)MEM_R8(c, item(menu, i) + k);
    for (int i = 0; i < n; i++) {
        const bool was_drawn = i >= top && i < top + count;
        for (uint32_t k = 0; k < kItemSize; k++) {
            const bool ours = (k >= 4 && k < 12) || k == 12;  // scales and gap (scaled for the draw)
            MEM_W8(c, item(menu, i) + k, was_drawn && !ours ? drawn_items[i - top][k] : saved[i][k]);
        }
    }
}

// The selected item's flames, in its letters only. The game draws each letter three times (opaque
// textured quads, GP0 2C: three packets in a row, the face drawn last) and its flame: a translucent
// textured quad (2E, the next packet) and a gouraud one (3E, after it) fading up, added onto the screen
// (texture page 0xA8: back + front), bigger than the letter, so the fire spread round the text. Every
// colour of the font's palette has the PS1's mask bit, so a letter marks its own pixels, and the flames
// are drawn only on marked pixels (GP0 E6 bit 2, our extension: gpu.cpp g_mask_inside). When the GPU is
// about to draw either flame quad (hw.cpp sends the list), it gets instead: the marks in the flames'
// area cleared (a black quad added on, unmarked: nothing changes but the mark), the letter's edge and
// shadow, the marks cleared again, its face (marking its pixels), then the flames at kFlamePercent on
// the face only; the other flame quad and the letter's own packets then draw nothing (length 0).
constexpr int kFlamePercent = 70;
uint32_t dim_colour(uint32_t w, int percent) {
    uint32_t out = w & 0xFF000000u;
    for (int s = 0; s < 24; s += 8) out |= ((((w >> s) & 0xFF) * (uint32_t)percent / 100) & 0xFF) << s;
    return out;
}

}  // namespace

// 80015228 menu_init(menu, ...): notes the front end's Load Game menu.
extern "C" void psx_hook_menu_init(R3000* c) {
    if (c->r[31] != kLoadMenuMade) return;
    g_load = c->r[4];
    g_proxy = 0;  // (made at its first draw)
}

// 800119C4: the front end's background camera, once per loop: its angle (+24) goes on by 4 and
// 800119F0 places the camera. With the menus at 60 Hz it goes on by 2.
extern "C" int psx_hook_menu_camera(R3000* c) {
    // the next loop is an in-between one; this one's step of the update counter is undone
    // (the game's animations (button prompts, ...) count it)
    static uint32_t counted = 0;
    if (psx_opt_menu_odd && MEM_R32(c, kUpdateCount) == counted + 1) MEM_W32(c, kUpdateCount, counted);
    counted = MEM_R32(c, kUpdateCount);
    psx_opt_menu_odd = psx_opt_menu60 && !psx_opt_menu_odd;
    if (!psx_opt_menu60) return 0;
    const uint32_t cam = c->r[4];
    MEM_W32(c, cam + 24, MEM_R32(c, cam + 24) + 2);
    game_call(c, 0x800119F0, cam);
    return 1;
}

// 80015474 menu_add(menu, text): before the pause menu's "Quit", add "Recomp Enhancements" and
// "Options"; the main menu is noted at its "new game".
extern "C" void psx_hook_menu_add(R3000* c) {
    static bool inside = false;  // (our own menu_add calls keep the caller's ra)
    if (inside || core_in_ghost()) return;
    const uint32_t menu = c->r[4];
    struct Inside {
        Inside() { inside = true; }
        ~Inside() { inside = false; }
    } guard;
    if (c->r[31] == kMainMade) {
        g_main = menu;
        g_song_started = false;
        if (!g_main_text) g_main_text = game_call(c, kAlloc, 32);
        if (g_main_text) write_text(c, g_main_text, "exit to desktop", 16);
        // The game's own "select level" item (its level-select cheat flag, read only here), named
        // "level selection": the menu is built from the text table after this and the game knows
        // the item by that table entry, so the entry points at our text.
        MEM_W32(c, 0x800FF330, 1);
        if (g_main_text) {
            write_text(c, g_main_text + 16, "level selection", 16);
            MEM_W32(c, kStrings + 4 * kStrSelectLevel, g_main_text + 16);
        }
        state_note_main_menu();
        // Recomp Enhancements (its item goes before "options" at the menu's draws)
        g_mainh = Host{};
        g_mainh.menu = menu;
        g_mainh.groups[kGroupEnhancements] = true;
        make_texts(c, g_mainh, true);
        return;
    }
    if (c->r[31] == kPauseMade) {
        g_pause = Host{};  // (the old one's blocks went with its menu)
        g_pause.menu = menu;
        g_pause.groups[kGroupEnhancements] = g_pause.groups[kGroupOptions] = true;
        if (!make_texts(c, g_pause, false)) return;
        game_call(c, kMenuAdd, menu, host_text(g_pause, kTextGroup + kGroupEnhancements));
        game_call(c, kMenuAdd, menu, host_text(g_pause, kTextGroup + kGroupOptions));
    }
}

// 800152C4 menu destructor: our blocks go with the Options, pause, main and Load Game menus.
void autoload_end(R3000* c);  // (below)

extern "C" void psx_hook_menu_free(R3000* c) {
    if (core_in_ghost()) return;
    const uint32_t menu = c->r[4];
    if (g_load && menu == g_load) {
        autoload_end(c);
        if (g_proxy) game_call(c, kFree, g_proxy);
        g_load = g_proxy = 0;
        return;
    }
    if (g_main && menu == g_main) {
        if (g_song_started) game_call(c, kSongStop);  // (else it plays on over the intro movie)
        if (g_main_text) game_call(c, kFree, g_main_text);
        g_main = g_main_text = 0;
    }
    for (Host* h : {&g_pause, &g_mainh}) {
        if (!h->menu || menu != h->menu) continue;
        close_list(c, *h);
        if (h->texts) game_call(c, kFree, h->texts);
        *h = Host{};
    }
}

// 80015BA4 menu_update(menu) (moves the selection with up/down): an open page (or the pause
// menu's list of them) takes the menu's turn.
// With the front end at 60 Hz: the selected item grows and the others shrink by 40 an update,
// so here by at most 20; and up / down (held: move, wait 10 updates, then move every second
// one, counted in gp+344) are only seen every other loop.
extern "C" int psx_hook_menu_update(R3000* c) {
    static bool inside = false;
    if (inside) return 0;
    for (Host* h : {&g_pause, &g_mainh})
        if (h->menu && c->r[4] == h->menu) c->r[4] = shown_menu(*h);
    if (g_proxy && g_load && c->r[4] == g_load) c->r[4] = g_proxy;  // Load Game: ours moves
    const uint32_t ra = c->r[31], sp = c->r[29];
    if (!psx_opt_menu60 || ra < kFrontEnd || ra >= kFrontEndEnd || core_in_ghost()) return 0;
    const uint32_t menu = c->r[4];
    const int n = std::min<int>(MEM_R8(c, menu + 10), kMaxItems);
    int16_t before[kMaxItems];
    for (int i = 0; i < n; i++) before[i] = (int16_t)MEM_R16(c, item(menu, i) + 4);
    const bool odd = psx_opt_menu_odd != 0;
    const uint8_t up = MEM_R8(c, kButtons + 160), down = MEM_R8(c, kButtons + 176);
    const uint32_t repeat = MEM_R32(c, kGpRepeat);
    if (odd) MEM_W8(c, kButtons + 160, 0), MEM_W8(c, kButtons + 176, 0);
    inside = true;
    call_indirect(c, 0x80015BA4);
    inside = false;
    c->r[31] = ra, c->r[29] = sp;
    if (odd) {
        MEM_W8(c, kButtons + 160, up), MEM_W8(c, kButtons + 176, down);
        MEM_W32(c, kGpRepeat, repeat);
    }
    for (int i = 0; i < n; i++) {
        const int d = (int16_t)MEM_R16(c, item(menu, i) + 4) - before[i];
        if (d) MEM_W16(c, item(menu, i) + 4, (uint16_t)(before[i] + std::clamp(d, -20, 20)));
    }
    return 1;
}

// The mouse in a menu (host_menu_mouse): the item under the pointer is selected, a left click picks it
// (a press of Cross), a right click goes back (Triangle) and the wheel moves up and down. Where each item
// is, from the letters its text call made (texts: packets of three opaque textured quads a letter, then
// the selected item's flame quads; the front end fills in packet lengths later, so they are walked by
// their commands): from its top letter to its bottom one, any x (the items are lines across).
void menu_mouse(R3000* c, uint32_t menu, const std::vector<TextCall>& texts) {
    const MenuMouse m = host_menu_mouse();
    if (m.wheel) host_menu_press(m.wheel < 0 ? 1u << 4 : 1u << 6);  // up / down
    if (m.back) host_menu_press(1u << 12);                           // triangle
    if (!m.on || !(m.moved || m.click)) return;
    const int n = std::min<int>(MEM_R8(c, menu + 10), kMaxItems);
    const float y = m.y * (float)gpu_display().h;
    int hit = -1;
    for (const TextCall& t : texts) {
        int item_at = -1;
        for (int i = 0; i < n; i++)
            if (MEM_R32(c, item(menu, i)) == t.text && MEM_R8(c, item(menu, i) + 13)) item_at = i;
        if (item_at < 0) continue;
        int y0 = 4096, y1 = -4096;
        for (uint32_t p = t.from; p + 4 < t.to;) {
            const uint32_t cmd = MEM_R32(c, 0x80000000u | (p + 4)) >> 24;
            const uint32_t words = cmd == 0x2C || cmd == 0x2D || cmd == 0x2E || cmd == 0x2F ? 9 : cmd == 0x3E ? 12 : 0;
            if (!words) break;
            if (cmd <= 0x2D)  // a letter (not its flames): its corners
                for (uint32_t k : {2u, 4u, 6u, 8u}) {
                    const int v = (int16_t)(MEM_R32(c, 0x80000000u | (p + 4 * k)) >> 16);
                    y0 = std::min(y0, v), y1 = std::max(y1, v);
                }
            p += 4 * (words + 1);
        }
        if (y0 <= y1 && y >= (float)y0 - 1 && y <= (float)y1 + 1) hit = item_at;
    }
    if (hit < 0) return;
    if (hit != MEM_R8(c, menu + 6)) game_call(c, kMenuSelect, menu, (uint32_t)hit);
    if (m.click) host_menu_press(1u << 14);  // cross
}

// 800156E0 menu_draw(menu).
extern "C" int psx_hook_menu_draw(R3000* c) {
    static bool inside = false;
    if (inside) return 0;
    const bool ghost = core_in_ghost();  // a render that is rolled back: our state stays
    if (!ghost) {
        g_menu_drawn = host_frame_count();  // gpu.cpp: the version number shows while a menu is up
    }
    uint32_t menu = c->r[4];
    if (g_load && menu == g_load) {
        sync_load_menu(c);
        if (!ghost) g_load_drawn = true;
        if (g_proxy) menu = c->r[4] = g_proxy;
    }
    if (g_main && menu == g_main) {
        if (!ghost) g_main_drawn = true;
        if (!ghost && !g_song_started) {
            g_song_started = true;
            constexpr uint32_t song = 2;  // plague (level 3; entry 6 is the same song)
            game_call(c, kSongLoad, MEM_R32(c, kSongs + 24 * song + 8));
            game_call(c, kSongPlay);
        }
        if (g_main_text) append_item(c, menu, g_main_text);
        move_to(c, menu, text_of(c, kStrSelectLevel), text_of(c, kStrLoadGame));
        // Recomp Enhancements, before "options"
        uint32_t options = 0;
        for (int i = 0; i < std::min<int>(MEM_R8(c, menu + 10), kMaxItems); i++) {
            std::string s = read_text(c, MEM_R32(c, item(menu, i)));
            for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
            if (s == "options") options = MEM_R32(c, item(menu, i));
        }
        if (g_mainh.texts) {
            const uint32_t label = host_text(g_mainh, kTextGroup + kGroupEnhancements);
            append_item(c, menu, label);
            if (options) move_to(c, menu, label, options, false);
        }
    }
    if (g_pause.menu && menu == g_pause.menu && g_pause.texts) append_item(c, menu, host_text(g_pause, kTextExit));
    int window = 0;
    for (Host* h : {&g_pause, &g_mainh}) {
        if (!h->menu || menu != h->menu) continue;
        if (!ghost) h->drawn = true;
        if (h->page >= 0) refresh_texts(c, h->pages[h->page], g_pages[h->page]);
        menu = c->r[4] = shown_menu(*h);
        if (h->page >= 0) window = 5;  // a long page scrolls, five lines at a time
    }
    const uint32_t ra = c->r[31], sp = c->r[29];
    inside = true;
    std::vector<TextCall> texts;
    if (!ghost) g_text_calls = &texts;
    draw_zoomed(c, menu, window);
    g_text_calls = nullptr;
    if (!ghost) menu_mouse(c, menu, texts);
    inside = false;
    c->r[31] = ra, c->r[29] = sp;
    return 1;
}

namespace {

// Keys for the main menu or the pause menu. While one of ours is shown the game's screen sees no
// back (the main menu would go to the title, the pause menu back to the game); select stays set
// (the screen plays its sound, and no item of its own is selected).
void host_keys(R3000* c, Host& h, uint32_t p_select, uint32_t p_back, uint16_t pressed) {
    const bool select = MEM_R32(c, p_select) != 0, back = MEM_R32(c, p_back) != 0;
    if (h.page >= 0) {
        MEM_W32(c, p_back, 0);
        const Page& p = g_pages[h.page];
        const int sel = MEM_R8(c, h.pages[h.page] + 6);
        if (back || (select && sel >= p.count)) {
            int pages[kPages];
            const int n = group_pages(h.group, pages, &h == &g_mainh);
            const int was = (int)(std::find(pages, pages + n, h.page) - pages);
            close_page(c, h);
            game_call(c, kMenuSelect, h.list, was);
            return;
        }
        int dir = select ? 1 : 0;
        if (pressed & kPadLeft) dir = -1;
        if (pressed & kPadRight) dir = 1;
        if (dir) opt_change(c, p.settings[sel], dir);
        return;
    }
    if (h.group >= 0) {
        MEM_W32(c, p_back, 0);
        int pages[kPages];
        const int n = group_pages(h.group, pages, &h == &g_mainh);
        const int sel = MEM_R8(c, h.list + 6);
        if (back || (select && sel >= n)) close_list(c, h);
        else if (select && !g_pages[pages[sel]].settings) controls_open();  // (a screen of its own)
        else if (select) open_page(c, h, pages[sel]);
        return;
    }
    if (!select || !h.texts) return;
    if (&h == &g_pause && selected_is(c, h.menu, host_text(h, kTextExit))) return host_request_quit();
    for (int g = 0; g < kGroups; g++)
        if (h.groups[g] && selected_is(c, h.menu, host_text(h, kTextGroup + g))) open_list(c, h, g);
}

}  // namespace

// Load Game -> Auto Save: the game loads it as it loads a card save. The game's menu has card
// save 1 selected, and for this one load the game's copy of the card file (0x800A493C: 512 bytes
// of header, then one 136-byte record per save) holds the auto-save's record there, and that item
// reads as not empty. Both are put back once the game has copied the record (autoload_end: when
// the menu goes, else at the next frame); the memory card itself is never written.
constexpr uint32_t kCardCopy = 0x800A493C + 512;
struct Autoload {
    bool active = false;
    uint32_t menu = 0, text = 0;
    uint8_t record[kSaveRecordSize];
} g_autoload;

void autoload_end(R3000* c) {
    if (!g_autoload.active) return;
    g_autoload.active = false;
    for (uint32_t i = 0; i < kSaveRecordSize; i++) MEM_W8(c, kCardCopy + i, g_autoload.record[i]);
    if (g_load && g_load == g_autoload.menu) MEM_W32(c, item(g_load, 0), g_autoload.text);
}

bool autoload_start(R3000* c) {
    uint8_t r[kSaveRecordSize];
    if (!g_load || MEM_R8(c, g_load + 10) == 0 || !state_read_autosave(r)) return false;
    autoload_end(c);
    g_autoload.active = true;
    g_autoload.menu = g_load;
    g_autoload.text = MEM_R32(c, item(g_load, 0));
    for (uint32_t i = 0; i < kSaveRecordSize; i++) {
        g_autoload.record[i] = (uint8_t)MEM_R8(c, kCardCopy + i);
        MEM_W8(c, kCardCopy + i, r[i]);
    }
    MEM_W8(c, g_load + 6, 0);
    MEM_W32(c, item(g_load, 0), g_proxy + 444);  // "auto save  Sewers"
    host_toast("Auto-save loaded");
    return true;
}

// 80017364 menu_keys(&select, &back, &any, &x): the game's menus read their keys; ours are
// handled here after them.
extern "C" int psx_hook_menu_keys(R3000* c) {
    static bool inside = false;
    static uint16_t prev_pad = 0xFFFF;
    if (inside || core_in_ghost()) return 0;
    const uint32_t p_select = c->r[4], p_back = c->r[5], ra = c->r[31], sp = c->r[29];
    inside = true;
    call_indirect(c, kMenuKeys);
    inside = false;
    c->r[31] = ra, c->r[29] = sp;

    const uint16_t pad = host_pad(0).buttons, pressed = (uint16_t)(prev_pad & ~pad);  // active low
    prev_pad = pad;
    // Load Game: Auto Save is ours to load; a card save is already selected in the game's menu
    const bool load_shown = g_load_drawn;
    g_load_drawn = false;
    if (load_shown && g_proxy && MEM_R32(c, p_select) && MEM_R8(c, g_proxy + 6) == 0 && !autoload_start(c))
        MEM_W32(c, p_select, 0);
    const bool main_shown = g_main_drawn;
    g_main_drawn = false;
    if (main_shown && g_main && g_main_text && MEM_R32(c, p_select) && selected_is(c, g_main, g_main_text))
        host_request_quit();
    for (Host* h : {&g_pause, &g_mainh}) {
        const bool shown = h->drawn;
        h->drawn = false;
        if (!h->menu) continue;
        if (!shown) {  // another screen: ours close
            close_list(c, *h);
            continue;
        }
        host_keys(c, *h, p_select, p_back, pressed);
    }
    return 1;
}

// The game shows a pickup ("20% Health", 8001B788(text, animation)) as text animated by
// keyframes of 28 bytes: s16 frames, 1, scale from, scale to, colour (-1 = the text's own) x3,
// two more (the text routine's a3; the weapon pickups animate them), x, y from, x, y to (screen centre at 256, 90 for pickups), u8 0, u8 next
// keyframe (+1; 0 = the end). Ours: the same pop in and fade, in the top-left corner.
uint32_t g_message = 0;  // our text and keyframes in the game's heap (made again after level starts)

void write_keyframe(R3000* c, uint32_t at, int frames, int scale0, int scale1, int x, int y, int next) {
    const int16_t v[13] = {(int16_t)frames, 1, (int16_t)scale0, (int16_t)scale1, -1, -1, -1, 0, 0,
                           (int16_t)x, (int16_t)y, (int16_t)x, (int16_t)y};
    for (int i = 0; i < 13; i++) MEM_W16(c, at + 2 * (uint32_t)i, (uint16_t)v[i]);
    MEM_W8(c, at + 26, 0);
    MEM_W8(c, at + 27, (uint8_t)next);
}

void game_message(R3000* c, const char* text) {
    constexpr uint32_t kClear = 0x8001B708, kShow = 0x8001B788, kFrames = 16;
    constexpr int kScale = 176, kX = 96, kY = 22;  // centre of the text: about 12 letters from the left edge
    if (!g_message) g_message = game_call(c, kAlloc, kFrames + 4 * 28);
    if (!g_message) return;
    write_text(c, g_message, text, kFrames);
    const uint32_t k = g_message + kFrames;
    write_keyframe(c, k, 8, 0, kScale, kX, kY, 1);            // pop in
    write_keyframe(c, k + 28, 60, kScale, kScale, kX, kY, 1);  // stay two seconds
    write_keyframe(c, k + 56, 12, kScale, 0, kX, kY, 0);      // and go
    game_call(c, kClear);
    game_call(c, kShow, g_message, k);
}

// Subtitles (subtitles.cpp): a message of our own on a random side of the screen, tilted, for the
// clip's length. Only the previous subtitle is replaced; pickups and "Auto Saved" stay. A long
// line is split in two (two messages, one under the other along the tilt) so it stays readable.
uint32_t g_subtitle[2] = {};  // their text and keyframes in the game's heap (made again after level starts)
float g_subtitle_tilt = 0;  // radians, clockwise (gpu.cpp turns the text's packets)
// The subtitle on screen: where its lines are and when its clip ends (host frames), to show it
// again when the game clears the messages (a pickup clears them all before showing its own).
struct {
    int n = 0, scale = 0, x[2] = {}, y[2] = {};
    uint64_t end = 0;
} g_sub;
constexpr uint32_t kMsgList = 0x800FEEBC + 752, kMsgShow = 0x8001B788, kSubText = 96;
// Messages move on once per world update, 30 times a second at either simulation rate.
constexpr int kMsgRate = 30, kSubIn = 6, kSubOut = 8;

bool subtitle_listed(R3000* c) {
    for (uint32_t n = MEM_R32(c, kMsgList), i = 0; n && i < 64; n = MEM_R32(c, n + 12), i++)
        if (g_subtitle[0] && MEM_R32(c, n) == g_subtitle[0]) return true;
    return false;
}

// Shows the subtitle's lines for `frames` message frames in all: popping in (unless it is shown
// again), staying, and shrinking away as the clip ends.
void subtitle_show(R3000* c, int frames, bool pop_in) {
    for (int i = 0; i < g_sub.n; i++) {
        const uint32_t k = g_subtitle[i] + kSubText;
        const int in = pop_in ? kSubIn : 0;
        const int hold = std::max(1, frames - in - kSubOut);
        if (in) write_keyframe(c, k, in, 0, g_sub.scale, g_sub.x[i], g_sub.y[i], 1);
        write_keyframe(c, k + 28, hold, g_sub.scale, g_sub.scale, g_sub.x[i], g_sub.y[i], 1);
        write_keyframe(c, k + 56, kSubOut, g_sub.scale, 0, g_sub.x[i], g_sub.y[i], 0);
        game_call(c, kMsgShow, g_subtitle[i], in ? k : k + 28);
    }
}

void game_subtitle(R3000* c, const char* text, float seconds) {
    constexpr uint32_t kList = 0x800FEEBC + 752, kRemove = 0x8001B694;
    constexpr uint32_t kSetScale = 0x8001A7BC, kWidth = 0x8001A97C, kText = 96;
    constexpr float kTiltDegrees = 30.0f;
    constexpr int kMaxWidth = 190, kBig = 224, kOneLine = 200;  // one line only if it fits at 200
    // lines apart at scale 256, in message units (16 left the two lines overlapping: a letter is
    // about as tall as 24 units)
    constexpr float kLineHeight = 36.0f;
    for (uint32_t& m : g_subtitle) {
        if (!m) m = game_call(c, kAlloc, kText + 4 * 28);
        if (!m) return;
    }
    for (uint32_t n = MEM_R32(c, kList); n;) {  // the last subtitle goes
        const uint32_t next = MEM_R32(c, n + 12);
        if (MEM_R32(c, n) == g_subtitle[0] || MEM_R32(c, n) == g_subtitle[1]) game_call(c, kRemove, n);
        n = next;
    }
    std::string s;  // in characters the game's font has
    for (const char* p = text; *p; p++)
        if (std::isalnum((unsigned char)*p) || std::strchr(" !%+-.'", *p)) s += *p;
    game_call(c, kSetScale, 256);
    auto width_of = [&](const std::string& t) {
        write_text(c, g_subtitle[0], t, kText);
        return std::max(1, (int)game_call(c, kWidth, g_subtitle[0]));
    };
    // as big as a pickup, smaller when the line is long: at most 190 wide
    std::string lines[2] = {s, ""};
    int width = width_of(s);
    if (256 * kMaxWidth / width < kOneLine) {  // too long: split at the space nearest the middle
        size_t best = std::string::npos;
        for (size_t i = s.find(' '); i != std::string::npos; i = s.find(' ', i + 1))
            if (best == std::string::npos || std::abs((int)i * 2 - (int)s.size()) < std::abs((int)best * 2 - (int)s.size()))
                best = i;
        if (best != std::string::npos) {
            lines[0] = s.substr(0, best);
            lines[1] = s.substr(best + 1);
            width = std::max(width_of(lines[0]), width_of(lines[1]));
        }
    }
    const int scale = std::min(kBig, 256 * kMaxWidth / width);
    static const bool seeded = (std::srand((unsigned)std::time(nullptr)), true);
    (void)seeded;
    const bool left = std::rand() & 1;
    const int x = left ? 124 : 388, y = 150;
    // leaning up towards the middle of the screen
    g_subtitle_tilt = (left ? -kTiltDegrees : kTiltDegrees) * 3.14159265f / 180.0f;
    const int n = lines[1].empty() ? 1 : 2;
    g_sub.n = n;
    g_sub.scale = scale;
    for (int i = 0; i < n; i++) {
        // each line is turned around its own centre: the centres sit on the turned vertical
        // (a screen pixel is 0.625 lines wide, gpu.cpp)
        const float d = ((float)i - (float)(n - 1) / 2.0f) * kLineHeight * (float)scale / 256.0f;
        g_sub.x[i] = x - (int)std::lround(d * std::sin(g_subtitle_tilt) / 0.625f);
        g_sub.y[i] = y + (int)std::lround(d * std::cos(g_subtitle_tilt));
        write_text(c, g_subtitle[i], lines[i], kText);
    }
    // on screen exactly as long as the clip plays, popping in and shrinking away included
    const int frames = std::max(kSubIn + kSubOut + 1, (int)std::lround(seconds * (float)kMsgRate));
    g_sub.end = host_frame_count() + (uint64_t)frames * 60 / kMsgRate;
    subtitle_show(c, frames, true);
}

// Once per frame (subtitles.cpp): a subtitle the game cleared while its clip still plays (a
// pickup clears every message first) is shown again for the rest of the clip.
void game_subtitle_keep(R3000* c) {
    const uint64_t now = host_frame_count();
    if (!g_sub.n || !g_subtitle[0] || now + 60 / kMsgRate * (kSubOut + 2) >= g_sub.end) return;
    if (subtitle_listed(c)) return;
    subtitle_show(c, (int)((g_sub.end - now) * kMsgRate / 60), false);
}

// Pickup voice lines (8002FC64(block, channel, where, 1)): the game's own choice for health and
// weapons. The Smart Bomb and Extra Life pickups say nothing in the game: they get a health
// pickup's line (block 1, one of two), started where the game shows their message.
namespace {
constexpr uint32_t kPlayVoice = 0x8002FC64, kRandom = 0x80066570, kPlayer = 0x800FF5A0;
constexpr int kStrSmartBomb = 147, kStrExtraLife = 148;
}  // namespace

// A pickup line asked for while another line is playing is dropped by the game (one voice at a
// time, 8002FC64 checks gp+1024 "ready" and gp+1012). Pickup lines are kept instead, in order,
// and each started as soon as the voice is free, if it has not waited at the front of the queue
// for more than kQueueFrames (a pickup line plays about 3 s, so several pickups in a row each get
// theirs).
// The game also refuses a voice while the drive is busy: a data read (section loads), reads it
// has queued, or a level's own XA stream (pest, the whole level). A pickup line is then played
// beside the drive, straight from the disc image (cdrom_side_voice).
constexpr uint32_t kGp = 0x800FEEBC, kVoiceReady = kGp + 1024, kVoiceBusy = kGp + 1012;
constexpr uint32_t kCdReading = 0x800FF700, kCdQueue = 0x800EAD38, kCdQueueAt = 0x800FF79C, kXaStream = 0x800FF250;
constexpr uint64_t kQueueFrames = 360;  // 6 s (a level line can play 4 s and more)
constexpr size_t kQueueMax = 3;
// return addresses of the pickups' calls: health (8005CE70), weapons (8005E260), and 0 for ours
constexpr uint32_t kPickupCalls[] = {0x8005CECC, 0x8005E2B4, 0x8005E350, 0x8005E3EC, 0x8005E498,
                                     0x8005E534, 0x8005E5D0, 0x8005E66C};
struct QueuedVoice {
    uint32_t args[4];  // block, channel, (where), priority
    uint64_t frame;  // when it was queued, or reached the front of the queue
};
std::vector<QueuedVoice> g_queued;
// Queues a line at the back (dropped when the queue is full) or the front (the last one goes).
void queue_line(QueuedVoice line, bool front) {
    line.frame = host_frame_count();
    g_queued.insert(front ? g_queued.begin() : g_queued.end(), line);
    if (g_queued.size() > kQueueMax) g_queued.pop_back();
}
void dequeue_line() {  // the next one's wait starts now
    g_queued.erase(g_queued.begin());
    if (!g_queued.empty()) g_queued[0].frame = host_frame_count();
}
bool g_voice_ours = false;  // the call comes from us (a queued line, or Extra Life / Smart Bomb)
// The last pickup line the game started: a level line that cuts it within kRecutFrames sends it
// back to the front of the queue (the level's lines start the clip themselves, 8002FD2C).
QueuedVoice g_last_line{};
constexpr uint64_t kRecutFrames = 60;

// The game's voice is in use: a line playing, the rest of a cut one or ours beside the drive, or
// a pickup line started in the last kStartFrames (the game marks the voice busy only once the
// clip's CD seek is under way, and a second line started before that cuts the first one off).
constexpr uint64_t kStartFrames = 30;
bool voice_talking(R3000* c) {
    return host_frame_count() + 1 < g_last_line.frame + kStartFrames || MEM_R32(c, kVoiceReady) == 0 ||
           MEM_R32(c, kVoiceBusy) != 0 || cdrom_side_busy();
}
bool cd_busy(R3000* c) {
    return MEM_R32(c, kCdReading) != 0 || MEM_R8(c, kCdQueue + 16 * MEM_R32(c, kCdQueueAt) + 13) != 0 ||
           MEM_R32(c, kXaStream) != 0;
}

// A pickup line beside the drive; false = it must wait.
bool play_beside(R3000* c, uint32_t block, uint32_t chan) {
    uint32_t lba, end;
    if (voice_talking(c) || !voice_clip_lba((int)block, (int)chan, &lba, &end)) return false;
    LOG(Host, "voice b%02u_c%02u beside the drive", block, chan);
    return cdrom_side_voice(lba, end, (int)chan);
}

extern "C" int psx_hook_voice(R3000* c) {
    if (cheat_quiet()) return 1;  // a weapon the cheat gives says nothing
    bool pickup = g_voice_ours;
    for (uint32_t a : kPickupCalls) pickup |= c->r[31] == a;
    if (!pickup) return 0;
    const bool talking = voice_talking(c), busy = cd_busy(c);
    LOG(Host, "voice b%02u_c%02u: pickup line%s%s", c->r[4], c->r[5], talking ? ", voice in use" : "",
        busy ? ", CD busy" : "");
    const QueuedVoice line{{c->r[4], c->r[5], c->r[6], c->r[7]}, host_frame_count() + 1};
    if (!talking && !busy) {  // the game plays it
        g_last_line = line;
        return 0;
    }
    if (!busy || !play_beside(c, line.args[0], line.args[1])) queue_line(line, false);
    return 1;
}

// 8002FD2C (block, channel) starts a clip: from 8002FC64 (pickups, ...) or the level's own lines.
extern "C" void psx_hook_voice_clip(R3000* c) {
    if (c->r[31] == 0x8002FCF8 || !g_last_line.frame) return;
    if (host_frame_count() > g_last_line.frame + kRecutFrames) return;
    if (cd_busy(c) || MEM_R32(c, kVoiceReady) == 0 || MEM_R32(c, kVoiceBusy) != 0) return;  // it will not play
    LOG(Host, "voice b%02u_c%02u cuts the pickup line: it plays again after", c->r[4], c->r[5]);
    queue_line(g_last_line, true);
    g_last_line.frame = 0;
}

// Once per frame (savestate.cpp): a queued pickup line starts when the voice is free.
void game_voice_poll(R3000* c) {
    autoload_end(c);  // (the game copied the auto-save's record in the frame it was chosen)
    if (g_queued.empty()) return;
    if (!MEM_R32(c, kPlayer)) return g_queued.clear();
    if (host_frame_count() - g_queued[0].frame > kQueueFrames) return dequeue_line();
    if (voice_talking(c)) return;
    const QueuedVoice q = g_queued[0];
    const bool busy = cd_busy(c);
    if (busy && !play_beside(c, q.args[0], q.args[1])) return;
    dequeue_line();
    if (busy) return;  // (playing beside the drive)
    g_voice_ours = true;
    // (every pickup line is at the player)
    game_call(c, kPlayVoice, q.args[0], q.args[1], MEM_R32(c, kPlayer) + 4, q.args[3]);
    g_voice_ours = false;
}

// 8005E260 give weapon (player, type): the cheat keeps weapons the player picks up.
extern "C" void psx_hook_weapon_pickup(R3000* c) { cheat_note_pickup(c->r[4], (int)(c->r[5] & 0xFFFF)); }

extern "C" void psx_hook_message(R3000* c) {
    if (c->r[4] != text_of(c, kStrExtraLife) && c->r[4] != text_of(c, kStrSmartBomb)) return;
    const uint32_t player = MEM_R32(c, kPlayer);
    if (!player) return;
    // as a health pickup does (8005CE70): block 1, one of two lines
    g_voice_ours = true;
    game_call(c, kPlayVoice, 1, game_call(c, kRandom, 2), player + 4, 1);
    g_voice_ours = false;
}

float subtitle_tilt(uint32_t text) {
    return text && (text == g_subtitle[0] || text == g_subtitle[1]) ? g_subtitle_tilt : 0.0f;
}

// A saved state was loaded or a level starts: our menus close (the game's memory may hold
// another screen), and the message block is made again when next needed. A loaded state has
// another heap: the menus and our blocks in it are forgotten (not freed).
void game_menu_reset(bool state_loaded) {
    g_message = g_subtitle[0] = g_subtitle[1] = 0;  // (the heap they were in is gone)
    g_sub.n = 0;
    if (state_loaded) {
        g_queued.clear();  // (a section load also comes here: queued lines stay)
        g_pause = g_mainh = Host{};
        g_main = g_main_text = g_load = g_proxy = 0;
    }
    for (Host* h : {&g_pause, &g_mainh}) {  // (an open list and page go with their menu)
        h->group = h->page = -1;
        h->list = 0;
        for (uint32_t& b : h->pages) b = 0;
        h->drawn = false;
    }
    g_main_drawn = false;
}

// hw.cpp: the packet at `addr` (24-bit) is about to go to the GPU. A selected menu item's flame quad: the
// GP0 words to draw in its place go in `out` (see kFlamePercent), and its letter's and flames' packets are
// made empty.
bool menu_flame_words(R3000* c, uint32_t addr, std::vector<GpuWord>& out) {
    if (!g_menu_flames || core_in_ghost()) return false;  // (Original: as the game draws them)
    auto rd = [c](uint32_t p) { return MEM_R32(c, 0x80000000u | (p & 0x1FFFFC)); };
    auto wr = [c](uint32_t p, uint32_t v) { MEM_W32(c, 0x80000000u | (p & 0x1FFFFC), v); };
    auto packet = [&](uint32_t p, uint32_t len, uint32_t cmd) { return rd(p) >> 24 == len && rd(p + 4) >> 24 == cmd; };
    auto tpage_word = [](uint32_t cmd) { return cmd == 0x2E ? 5u : 6u; };  // (with the second vertex's uv)
    auto added = [&](uint32_t p, uint32_t cmd) { return ((rd(p + 4 * tpage_word(cmd)) >> 21) & 3) == 1; };
    const uint32_t f = (rd(addr + 4) >> 24) == 0x3E ? addr - 40 : addr, g = f + 40;
    if (!packet(f, 9, 0x2E) || !packet(g, 12, 0x3E) || !added(f, 0x2E) || !added(g, 0x3E)) return false;
    uint32_t letters[3] = {f - 120, f - 80, f - 40};
    for (uint32_t l : letters)
        if (!packet(l, 9, 0x2C)) return false;
    // in the order the game draws them (their links; the pause menu's and the front end's differ): the
    // last one is the face
    auto next = [&](uint32_t p) { return rd(p) & 0x1FFFFC; };
    for (int k = 0; k < 3; k++) {
        bool pointed = false;
        for (uint32_t l : letters) pointed |= next(l) == letters[k];
        if (pointed) continue;
        const uint32_t first = letters[k];
        uint32_t order[3] = {first, 0, 0};
        int n = 1;
        for (uint32_t at = next(first); n < 3; at = next(at)) {
            bool ours = false;
            for (uint32_t l : letters) ours |= l == at;
            if (!ours) break;
            order[n++] = at;
        }
        if (n == 3) std::copy(order, order + 3, letters);
        break;
    }
    auto copy = [&](uint32_t src, uint32_t n, int percent, int semi_mode) {  // colours scaled, mode set
        const uint32_t cmd = rd(src + 4) >> 24;
        for (uint32_t i = 1; i <= n; i++) {
            uint32_t v = rd(src + 4 * i);
            const bool colour = i == 1 || (n == 12 && (i == 4 || i == 7 || i == 10));
            if (colour && percent != 100) v = dim_colour(v, percent);
            if (semi_mode >= 0 && i == tpage_word(cmd)) v = (v & ~(3u << 21)) | (uint32_t)semi_mode << 21;
            out.push_back({v, src & 0x1FFFFC, (src + 4 * i) & 0x1FFFFC});
        }
    };
    // the flames' area: their corners' bounds
    int x0 = 4096, y0 = 4096, x1 = -4096, y1 = -4096;
    auto corner = [&](uint32_t v) {
        const int x = (int16_t)(v & 0xFFFF), y = (int16_t)(v >> 16);
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    };
    for (uint32_t i : {2u, 4u, 6u, 8u}) corner(rd(f + 4 * i));
    for (uint32_t i : {2u, 5u, 8u, 11u}) corner(rd(g + 4 * i));
    auto xy = [](int x, int y) { return (uint32_t)(x & 0xFFFF) | (uint32_t)(y & 0xFFFF) << 16; };
    const uint32_t mode = 0xE1000000u | (gpu_read_stat(c) & 0x600) | ((rd(f + 4 * 5) >> 16) & 0x1FF);  // the flame's page
    auto put = [&](std::initializer_list<uint32_t> words) {  // made-up words, placed as the flame
        for (uint32_t v : words) out.push_back({v, f & 0x1FFFFC, 0});
    };
    const std::initializer_list<uint32_t> clear = {0x2A000000u, xy(x0, y0), xy(x1, y0), xy(x0, y1), xy(x1, y1)};  // black, added
    put({mode, 0xE6000000u});
    put(clear);
    copy(letters[0], 9, 100, -1);  // edge and shadow
    copy(letters[1], 9, 100, -1);
    put(clear);  // (their marks too: the flames go on the face only)
    copy(letters[2], 9, 100, -1);
    put({0xE6000004u});  // (ours: only on marked pixels, see gpu.cpp g_mask_inside)
    copy(f, 9, kFlamePercent, 1);
    copy(g, 12, kFlamePercent, 1);
    put({0xE6000000u, mode});  // the draw mode as the flame left it
    for (uint32_t p : {letters[0], letters[1], letters[2], f, g}) wr(p, rd(p) & 0xFFFFFF);  // empty now
    return true;
}
