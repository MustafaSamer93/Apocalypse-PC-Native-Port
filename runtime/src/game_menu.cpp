// The game's own menus (front end and pause):
//  - Font scaling: each menu is drawn at g_menu_zoom percent around its middle (text size, line
//    spacing and gaps), without changing the menu itself.
//  - Options -> Enhancements: an extra item in the Options menu opening a page of the
//    menu settings (renderer, screen mode, display, frame rate, font scaling, autosave:
//    host_sdl.cpp enh_*), drawn and driven as a menu of the game's own.
//  - Main Menu -> Load Game: "Auto Save" at the top of the memory card saves (savestate.cpp).
//  - Pause menu: "Options" (a list of the pages above) and "Exit to desktop"; main menu: "exit
//    to desktop".
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

#include "psx.h"
#include "savestate.h"

int g_menu_zoom = 60;
int psx_opt_menu60 = 0;
int psx_opt_menu_odd = 0;

namespace {

constexpr uint32_t kStrings = 0x800A54E4;  // the game's text table
constexpr int kStrMiscOptions = 65, kStrReturnToMain = 66, kStrReturnToOptions = 48;
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

// Our pages: their settings (host_sdl.cpp enh_*), in order.
constexpr int kEnhSettings[] = {E_RENDERER, E_SCREEN, E_DISPLAY, E_AA, E_FPS, E_FPS_COUNTER, E_FONT_SCALE,
                                E_AUTOSAVE, E_SUBTITLES};
constexpr int kCheatSettings[] = {E_CHEAT_HEALTH, E_CHEAT_LIVES, E_CHEAT_WEAPONS, E_CHEAT_AMMO, E_CHEAT_BOMBS};
struct Page {
    const char* label;
    const int* settings;
    int count;
};
constexpr Page g_pages[] = {
    {"enhancements", kEnhSettings, (int)std::size(kEnhSettings)},
    {"cheats", kCheatSettings, (int)std::size(kCheatSettings)},
};
constexpr int kPages = (int)std::size(g_pages);

// A page's block in the game's heap: its menu, its item texts, then its label.
uint32_t page_label(uint32_t block, const Page& p) { return block + 444 + kTextSize * (uint32_t)p.count; }

// A game menu our menus are shown in place of: Options (the pages' items are in it, after "misc
// options") or the pause menu ("Options" opens a list of the pages).
struct Host {
    uint32_t menu = 0;
    uint32_t pages[kPages] = {};  // their blocks (in the pause menu: only while one is open)
    int page = -1;                // the page shown, -1 = none
    bool in_list = false;         // pause: the list of the pages is shown
    bool drawn = false;           // the menu was drawn since the last key read
};
Host g_opt, g_pause;

// The pause menu's block: the list of the pages (a menu), then texts: "Options", "Exit to
// desktop", the pages' labels, "back".
constexpr uint32_t kPauseMade = 0x80017B40;  // return address of the pause menu's "Quit" menu_add
uint32_t g_pause_block = 0;
uint32_t pause_text(int i) { return g_pause_block + 444 + kTextSize * (uint32_t)i; }
constexpr int kPauseOptions = 0, kPauseExit = 1, kPauseLabels = 2, kPauseBack = 2 + kPages;

// Main menu: "exit to desktop" at the end, added at its first draw.
constexpr uint32_t kMainMade = 0x80011F38;  // return address of the main menu's "new game" menu_add
uint32_t g_main = 0, g_main_text = 0;
bool g_main_drawn = false;

// Load Game: the game lists the memory card saves in a menu and loads the one whose number is
// selected, so the list is shown through a stand-in menu with "Auto Save" at the top; choosing a
// card save selects it in the game's menu, choosing Auto Save loads saves/autosave.state.
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

// A page's item texts: "setting  value", in characters the game's font has.
void refresh_texts(R3000* c, uint32_t block, const Page& p) {
    for (int i = 0; i < p.count; i++) {
        std::string s = std::string(enh_name(p.settings[i])) + "  " + enh_value(p.settings[i]);
        s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return ch == '(' || ch == ')'; }), s.end());
        std::replace(s.begin(), s.end(), ':', 'x');  // the font has no ':' ("16:9" -> "16x9")
        for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
        write_text(c, block + 444 + kTextSize * (uint32_t)i, s, kTextSize);
    }
}

// Starts `menu` in the same place, alignment, scales and spacing as the game's menu `like`
// (text size and spacing times `size`).
void init_like(R3000* c, uint32_t menu, uint32_t like, float size = 1.0f) {
    const uint32_t it = item(like, 0);
    auto sized = [size](int v) { return (uint32_t)std::lround((float)v * size); };
    game_call(c, kMenuInit, menu, MEM_R32(c, like + 12), MEM_R32(c, like + 16), MEM_R8(c, like + 4),
              {sized((int16_t)MEM_R16(c, it + 8)), sized((int16_t)MEM_R16(c, it + 10)),
               sized((int32_t)MEM_R32(c, like + 20))});
}

// Builds a page's menu like the game's menu `like`; 0 if the heap is full.
uint32_t build_page(R3000* c, uint32_t like, const Page& p, float size = 1.0f) {
    const uint32_t block = game_call(c, kAlloc, 444 + kTextSize * (uint32_t)p.count + 16);
    if (!block) return 0;
    write_text(c, page_label(block, p), p.label, 16);
    init_like(c, block, like, size);
    refresh_texts(c, block, p);
    for (int i = 0; i < p.count; i++) game_call(c, kMenuAdd, block, block + 444 + kTextSize * (uint32_t)i);
    game_call(c, kMenuAdd, block, text_of(c, kStrReturnToOptions));
    game_call(c, kMenuLayout, block);  // centred up and down
    return block;
}

// Adds an item to the end of a laid out menu, keeping it centred (half a line up), unless it is
// there. (Done at each draw: a draw may be one the 60 Hz renderer rolls back, sim60.cpp.)
void append_item(R3000* c, uint32_t menu, uint32_t text) {
    const int n = MEM_R8(c, menu + 10);
    if (n >= (int)kMaxItems || (n > 0 && MEM_R32(c, item(menu, n - 1)) == text)) return;
    game_call(c, kMenuAdd, menu, text);
    MEM_W32(c, menu + 16, (uint32_t)((int32_t)MEM_R32(c, menu + 16) - (int32_t)MEM_R32(c, menu + 20) / 2));
}

// The menu shown in place of the host's.
uint32_t shown_menu(const Host& h) {
    if (h.page >= 0) return h.pages[h.page];
    return h.in_list ? g_pause_block : h.menu;
}

// Closes a page (the pause menu's pages are made only while open: the heap in a level is small).
void close_page(R3000* c, Host& h) {
    if (h.page < 0) return;
    if (&h == &g_pause && h.pages[h.page]) game_call(c, kFree, h.pages[h.page]), h.pages[h.page] = 0;
    h.page = -1;
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
// put back after.
void draw_zoomed(R3000* c, uint32_t menu) {
    const int n = std::min<int>(MEM_R8(c, menu + 10), kMaxItems);
    if (g_menu_zoom >= 100 || n <= 0) {
        call_indirect(c, kMenuDraw);
        return;
    }
    const float f = (float)g_menu_zoom / 100.0f;
    const int32_t y = (int32_t)MEM_R32(c, menu + 16), spacing = (int32_t)MEM_R32(c, menu + 20);
    uint16_t saved[kMaxItems][5];
    int height = 0, shown = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t it = item(menu, i);
        for (int k = 0; k < 4; k++) saved[i][k] = MEM_R16(c, it + 4 + 2 * k);
        saved[i][4] = MEM_R8(c, it + 12);
        if (MEM_R8(c, it + 13)) height += MEM_R8(c, it + 12), shown++;
    }
    height += std::max(0, shown - 1) * spacing;
    auto scaled = [f](int v) { return (int)std::lround((float)v * f); };
    MEM_W32(c, menu + 16, (uint32_t)(y + height / 2 - scaled(height) / 2));
    MEM_W32(c, menu + 20, (uint32_t)scaled(spacing));
    for (int i = 0; i < n; i++) {
        const uint32_t it = item(menu, i);
        for (int k = 0; k < 4; k++) MEM_W16(c, it + 4 + 2 * k, (uint16_t)scaled((int16_t)saved[i][k]));
        MEM_W8(c, it + 12, (uint8_t)scaled(saved[i][4]));
    }
    call_indirect(c, kMenuDraw);
    MEM_W32(c, menu + 16, (uint32_t)y);
    MEM_W32(c, menu + 20, (uint32_t)spacing);
    for (int i = 0; i < n; i++) {
        const uint32_t it = item(menu, i);
        for (int k = 0; k < 4; k++) MEM_W16(c, it + 4 + 2 * k, saved[i][k]);
        MEM_W8(c, it + 12, (uint8_t)saved[i][4]);
    }
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
    if (static const char* dump = std::getenv("APOC_LOOP_DUMP"); dump) {  // debug: START:COUNT:PREFIX
        static unsigned loop = 0;
        unsigned start = 0, count = 0;
        char prefix[260] = "";
        if (std::sscanf(dump, "%u:%u:%259s", &start, &count, prefix) == 3 && loop >= start && loop < start + count)
            if (FILE* f = std::fopen((std::string(prefix) + std::to_string(loop - start) + ".bin").c_str(), "wb"))
                std::fwrite(psx_ram, 1, PSX_RAM_SIZE, f), std::fclose(f);
        if (loop % 60 == 0) std::fprintf(stderr, "front end loop %u at frame %llu\n", loop, (unsigned long long)host_frame_count());
        loop++;
    }
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

// 80015474 menu_add(menu, text): before Options' "return to main menu" (right after
// "misc options"), add our pages; before the pause menu's "Quit", add "Options"; the main menu
// is noted at its "new game".
extern "C" void psx_hook_menu_add(R3000* c) {
    static bool inside = false;  // (our own menu_add calls keep the caller's ra)
    if (inside || core_in_ghost()) return;
    const uint32_t menu = c->r[4], text = c->r[5];
    struct Inside {
        Inside() { inside = true; }
        ~Inside() { inside = false; }
    } guard;
    if (c->r[31] == kMainMade) {
        g_main = menu;
        if (!g_main_text) g_main_text = game_call(c, kAlloc, 16);
        if (g_main_text) write_text(c, g_main_text, "exit to desktop", 16);
        return;
    }
    if (c->r[31] == kPauseMade) {
        g_pause = Host{};  // (the old one's blocks went with its menu)
        g_pause.menu = menu;
        g_pause_block = game_call(c, kAlloc, 444 + kTextSize * (kPauseBack + 1));
        if (!g_pause_block) return;
        write_text(c, pause_text(kPauseOptions), "Options", kTextSize);
        write_text(c, pause_text(kPauseExit), "Exit to desktop", kTextSize);
        for (int i = 0; i < kPages; i++) write_text(c, pause_text(kPauseLabels + i), g_pages[i].label, kTextSize);
        write_text(c, pause_text(kPauseBack), "return to pause menu", kTextSize);
        game_call(c, kMenuAdd, menu, pause_text(kPauseOptions));
        return;
    }
    if (text != text_of(c, kStrReturnToMain)) return;
    const int n = MEM_R8(c, menu + 10);
    if (n <= 0 || n >= (int)kMaxItems - kPages || MEM_R32(c, item(menu, n - 1)) != text_of(c, kStrMiscOptions)) return;
    g_opt = Host{};
    g_opt.menu = menu;
    for (int i = 0; i < kPages; i++) {
        g_opt.pages[i] = build_page(c, menu, g_pages[i]);
        if (g_opt.pages[i]) game_call(c, kMenuAdd, menu, page_label(g_opt.pages[i], g_pages[i]));
    }
}

// 800152C4 menu destructor: our blocks go with the Options, pause, main and Load Game menus.
extern "C" void psx_hook_menu_free(R3000* c) {
    if (core_in_ghost()) return;
    const uint32_t menu = c->r[4];
    if (g_load && menu == g_load) {
        if (g_proxy) game_call(c, kFree, g_proxy);
        g_load = g_proxy = 0;
        return;
    }
    if (g_main && menu == g_main) {
        if (g_main_text) game_call(c, kFree, g_main_text);
        g_main = g_main_text = 0;
        return;
    }
    for (Host* h : {&g_opt, &g_pause}) {
        if (!h->menu || menu != h->menu) continue;
        for (uint32_t& b : h->pages)
            if (b) game_call(c, kFree, b), b = 0;
        if (h == &g_pause && g_pause_block) game_call(c, kFree, g_pause_block), g_pause_block = 0;
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
    for (Host* h : {&g_opt, &g_pause})
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

// 800156E0 menu_draw(menu).
extern "C" int psx_hook_menu_draw(R3000* c) {
    static bool inside = false;
    if (inside) return 0;
    const bool ghost = core_in_ghost();  // a render that is rolled back: our state stays
    uint32_t menu = c->r[4];
    if (g_load && menu == g_load) {
        sync_load_menu(c);
        if (!ghost) g_load_drawn = true;
        if (g_proxy) menu = c->r[4] = g_proxy;
    }
    if (g_main && menu == g_main) {
        if (!ghost) g_main_drawn = true;
        if (g_main_text) append_item(c, menu, g_main_text);
    }
    if (g_pause.menu && menu == g_pause.menu && g_pause_block) append_item(c, menu, pause_text(kPauseExit));
    for (Host* h : {&g_opt, &g_pause}) {
        if (!h->menu || menu != h->menu) continue;
        if (!ghost) h->drawn = true;
        if (h->page >= 0) refresh_texts(c, h->pages[h->page], g_pages[h->page]);
        menu = c->r[4] = shown_menu(*h);
    }
    const uint32_t ra = c->r[31], sp = c->r[29];
    inside = true;
    draw_zoomed(c, menu);
    inside = false;
    c->r[31] = ra, c->r[29] = sp;
    return 1;
}

namespace {

// Keys for Options or the pause menu. While one of ours is shown the game's screen sees no back
// (Options would go to the main menu, the pause menu back to the game); select stays set (the
// screen plays its sound, and no item of its own is selected).
void host_keys(R3000* c, Host& h, uint32_t p_select, uint32_t p_back, uint16_t pressed) {
    const bool select = MEM_R32(c, p_select) != 0, back = MEM_R32(c, p_back) != 0;
    const bool pause = &h == &g_pause;
    auto open_page = [&](int i) {
        // (smaller between the pause screen's title and button prompts)
        if (pause && !h.pages[i]) h.pages[i] = build_page(c, h.menu, g_pages[i], 0.7f);
        if (!h.pages[i]) return;
        h.page = i;
        game_call(c, kMenuSelect, h.pages[i], 0);
    };
    if (h.page >= 0) {
        MEM_W32(c, p_back, 0);
        const Page& p = g_pages[h.page];
        const int sel = MEM_R8(c, h.pages[h.page] + 6);
        if (back || (select && sel >= p.count)) {
            const int was = h.page;
            close_page(c, h);
            if (h.in_list) game_call(c, kMenuSelect, g_pause_block, was);
            return;
        }
        int dir = select ? 1 : 0;
        if (pressed & kPadLeft) dir = -1;
        if (pressed & kPadRight) dir = 1;
        if (dir) enh_change(p.settings[sel], dir);
        return;
    }
    if (h.in_list) {
        MEM_W32(c, p_back, 0);
        const int sel = MEM_R8(c, g_pause_block + 6);
        if (back || (select && sel >= kPages)) h.in_list = false;
        else if (select) open_page(sel);
        return;
    }
    if (!select) return;
    if (!pause) {
        for (int i = 0; i < kPages; i++)
            if (h.pages[i] && selected_is(c, h.menu, page_label(h.pages[i], g_pages[i]))) open_page(i);
        return;
    }
    if (!g_pause_block) return;
    if (selected_is(c, h.menu, pause_text(kPauseExit))) {
        host_request_quit();
    } else if (selected_is(c, h.menu, pause_text(kPauseOptions))) {
        // the list of the pages, where the pause menu is
        init_like(c, g_pause_block, h.menu);
        for (int i = 0; i <= kPages; i++) game_call(c, kMenuAdd, g_pause_block, pause_text(kPauseLabels + i));
        game_call(c, kMenuLayout, g_pause_block);
        game_call(c, kMenuSelect, g_pause_block, 0);
        h.in_list = true;
    }
}

}  // namespace

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
    if (load_shown && g_proxy && MEM_R32(c, p_select) && MEM_R8(c, g_proxy + 6) == 0) {
        MEM_W32(c, p_select, 0);
        if (state_slot_exists(kAutoSlot)) state_request_load(kAutoSlot);
    }
    const bool main_shown = g_main_drawn;
    g_main_drawn = false;
    if (main_shown && g_main && g_main_text && MEM_R32(c, p_select) && selected_is(c, g_main, g_main_text))
        host_request_quit();
    for (Host* h : {&g_opt, &g_pause}) {
        const bool shown = h->drawn;
        h->drawn = false;
        if (!h->menu) continue;
        if (!shown) {  // another screen: ours close
            close_page(c, *h);
            h->in_list = false;
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
    constexpr int kScale = 176, kX = 76, kY = 22;  // centre of the text: about 10 letters from the left edge
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
// clip's length. Only the previous subtitle is replaced; pickups and "Auto Saved" stay.
uint32_t g_subtitle = 0;  // its text and keyframes in the game's heap (made again after level starts)
float g_subtitle_tilt = 0;  // radians, clockwise (gpu.cpp turns the text's packets)

void game_subtitle(R3000* c, const char* text, float seconds) {
    constexpr uint32_t kList = 0x800FEEBC + 752, kRemove = 0x8001B694, kShow = 0x8001B788;
    constexpr uint32_t kSetScale = 0x8001A7BC, kWidth = 0x8001A97C, kText = 96;
    constexpr float kTiltDegrees = 30.0f;
    if (!g_subtitle) g_subtitle = game_call(c, kAlloc, kText + 4 * 28);
    if (!g_subtitle) return;
    for (uint32_t n = MEM_R32(c, kList); n; n = MEM_R32(c, n + 12))  // the last subtitle goes
        if (MEM_R32(c, n) == g_subtitle) {
            game_call(c, kRemove, n);
            break;
        }
    std::string s;  // in characters the game's font has
    for (const char* p = text; *p; p++)
        if (std::isalnum((unsigned char)*p) || std::strchr(" !%+-.'", *p)) s += *p;
    write_text(c, g_subtitle, s, kText);
    // as big as a pickup, smaller when the line is long: at most 190 wide
    game_call(c, kSetScale, 256);
    const int width = std::max(1, (int)game_call(c, kWidth, g_subtitle));
    const int scale = std::min(224, 256 * 190 / width);
    static const bool seeded = (std::srand((unsigned)std::time(nullptr)), true);
    (void)seeded;
    const bool left = std::rand() & 1;
    const int x = left ? 124 : 388, y = 150;
    // leaning up towards the middle of the screen
    g_subtitle_tilt = (left ? -kTiltDegrees : kTiltDegrees) * 3.14159265f / 180.0f;
    const int fps = psx_opt_sim60 ? 60 : 30;  // the messages move on with the game's loop
    const uint32_t k = g_subtitle + kText;
    write_keyframe(c, k, 6, 0, scale, x, y, 1);
    write_keyframe(c, k + 28, std::max(1, (int)(seconds * (float)fps)), scale, scale, x, y, 1);
    write_keyframe(c, k + 56, 8, scale, 0, x, y, 0);
    game_call(c, kShow, g_subtitle, k);
}

// Pickup voice lines (8002FC64(block, channel, where, 1)): the game's own choice for health and
// weapons. The Smart Bomb and Extra Life pickups say nothing in the game: they get a health
// pickup's line (block 1, one of two), started where the game shows their message.
namespace {
constexpr uint32_t kPlayVoice = 0x8002FC64, kRandom = 0x80066570, kPlayer = 0x800FF5A0;
constexpr int kStrSmartBomb = 147, kStrExtraLife = 148;
}  // namespace

// A pickup line asked for while another line is playing is dropped by the game (one voice at a
// time, 8002FC64 checks gp+1024 "ready" and gp+1012). Pickup lines are kept instead and started
// as soon as the voice is free, if that is within kQueueFrames.
constexpr uint32_t kGp = 0x800FEEBC, kVoiceReady = kGp + 1024, kVoiceBusy = kGp + 1012;
constexpr uint64_t kQueueFrames = 180;  // 3 s
// return addresses of the pickups' calls: health (8005CE70), weapons (8005E260), and 0 for ours
constexpr uint32_t kPickupCalls[] = {0x8005CECC, 0x8005E2B4, 0x8005E350, 0x8005E3EC, 0x8005E498,
                                     0x8005E534, 0x8005E5D0, 0x8005E66C};
struct QueuedVoice {
    uint32_t args[4];
    uint64_t frame;
    bool pending = false;
} g_queued;
bool g_voice_ours = false;  // the call comes from us (a queued line, or Extra Life / Smart Bomb)

bool voice_free(R3000* c) { return MEM_R32(c, kVoiceReady) != 0 && MEM_R32(c, kVoiceBusy) == 0; }

extern "C" int psx_hook_voice(R3000* c) {
    if (cheat_quiet()) return 1;  // a weapon the cheat gives says nothing
    bool pickup = g_voice_ours;
    for (uint32_t a : kPickupCalls) pickup |= c->r[31] == a;
    if (pickup && !voice_free(c)) {
        g_queued = {{c->r[4], c->r[5], c->r[6], c->r[7]}, host_frame_count(), true};
        return 1;
    }
    return 0;
}

// Once per frame (savestate.cpp): a queued pickup line starts when the voice is free.
void game_voice_poll(R3000* c) {
    if (!g_queued.pending) return;
    if (host_frame_count() - g_queued.frame > kQueueFrames || !MEM_R32(c, kPlayer)) {
        g_queued.pending = false;
        return;
    }
    if (!voice_free(c)) return;
    g_queued.pending = false;
    g_voice_ours = true;
    game_call(c, kPlayVoice, g_queued.args[0], g_queued.args[1], g_queued.args[2], g_queued.args[3]);
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

// Debug (APOC_TEST_PICKUP, savestate.cpp): a weapon pickup of type 1-7 (8005E260), 0 = the Extra
// Life pickup's message, 8 = the Smart Bomb pickup's (their voice lines come from psx_hook_message).
void game_test_pickup(R3000* c, int what) {
    const uint32_t player = MEM_R32(c, kPlayer);
    if (!player) return;
    if (what == 0) game_call(c, 0x8001B788, text_of(c, kStrExtraLife), 0x800A59DC);
    else if (what == 8) game_call(c, 0x8001B788, text_of(c, kStrSmartBomb), 0x800A57A8);
    else game_call(c, 0x8005E260, player, (uint32_t)what);
}

float subtitle_tilt(uint32_t text) { return g_subtitle && text == g_subtitle ? g_subtitle_tilt : 0.0f; }

// A saved state was loaded or a level starts: our menus close (the game's memory may hold
// another screen), and the message block is made again when next needed. A loaded state has
// another heap: the menus and our blocks in it are forgotten (not freed).
void game_menu_reset(bool state_loaded) {
    g_message = g_subtitle = 0;  // (the heap they were in is gone)
    g_queued.pending = false;
    if (state_loaded) {
        g_opt = g_pause = Host{};
        g_pause_block = g_main = g_main_text = g_load = g_proxy = 0;
    }
    g_opt.page = g_pause.page = -1;  // (the pause menu's page blocks go with its menu)
    g_opt.in_list = g_pause.in_list = false;
    g_opt.drawn = g_pause.drawn = g_main_drawn = false;
}
