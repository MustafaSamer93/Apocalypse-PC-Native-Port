// Cheats (Options -> Cheats; off at every start, never saved):
//   infinite health  - the game's own invincibility switch (0x800FF31C: its damage routine,
//                      8005DDF0, then ignores hits); falling into a hole still kills (the level
//                      scripts and the player code call the death routine 8005E708 directly)
//   infinite lives   - a life lost is given back
//   all weapons      - every weapon the player does not have is given (quietly), and taken back
//                      when the cheat is switched off unless it was picked up meanwhile
//   infinite ammo    - ammo spent is given back
//   infinite bombs   - a smart bomb used is given back
// Lives and bombs are never raised above what the player really has: pickups made meanwhile
// count, and switching the cheat off leaves exactly that. (With none left, one is shown so one
// can be used; it goes again when the cheat is switched off.)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "psx.h"

namespace {

constexpr uint32_t kPlayer = 0x800FF5A0;       // the player object
constexpr uint32_t kInvincible = 0x800FF31C;   // the game's cheat switch
constexpr uint32_t kLives = 0x800FF874;
constexpr uint32_t kBombs = 532;               // player: smart bombs
constexpr uint32_t kWeaponSlots = 616;         // player: weapon objects of types 1-7 (+4 each)
constexpr uint32_t kCurrent = 644, kPrevious = 648;  // player: weapon types in use and before
constexpr uint32_t kAmmo = 56;                 // weapon object: ammo (1000 when picked up, 20 a shot)
constexpr uint32_t kGiveWeapon = 0x8005E260;   // (player, type): the pickup routine
constexpr int kTypes = 7;

bool g_on[5];
// what the player really has while the cheat runs (-1 = not tracking)
int32_t g_real_lives = -1, g_seen_lives = 0;
int32_t g_real_bombs = -1, g_seen_bombs = 0;
uint32_t g_bombs_player = 0;
// weapons: the ones the cheat gave (bit per type), on which player
uint32_t g_given = 0, g_given_player = 0;
// ammo per weapon slot, and the object it was for
uint32_t g_ammo_obj[kTypes];
int32_t g_ammo[kTypes];

bool g_quiet = false;  // our own weapon pickups: no voice line (psx_hook_voice)

bool valid(uint32_t p) { return (p & 0xFF000000u) == 0x80000000u && (p & 0x1FFFFF) != 0; }

uint32_t player(R3000* c) {
    const uint32_t p = MEM_R32(c, kPlayer);
    return valid(p) ? p : 0;
}

// Keeps a count from going down while `on`: `real` follows pickups, uses are given back.
void keep(R3000* c, uint32_t addr, bool word, bool on, int32_t& real, int32_t& seen) {
    const int32_t now = word ? (int32_t)MEM_R32(c, addr) : (int16_t)MEM_R16(c, addr);
    auto put = [&](int32_t v) { word ? MEM_W32(c, addr, (uint32_t)v) : MEM_W16(c, addr, (uint16_t)v); };
    if (!on) {
        if (real >= 0) put(real);  // switched off: what the player really has
        real = -1;
        return;
    }
    if (real < 0) real = now, seen = now;
    if (now > seen) real += now - seen;  // a pickup
    const int32_t show = real > 0 ? real : 1;
    if (now != show) put(show);
    seen = show;
}

void remove_weapon(R3000* c, uint32_t p, int type) {
    const uint32_t slot = p + kWeaponSlots + 4 * (uint32_t)(type - 1), obj = MEM_R32(c, slot);
    if (valid(obj)) {  // its destructor, as the pickup routine calls it (a1 = 3: delete)
        const uint32_t vt = MEM_R32(c, obj), fn = MEM_R32(c, vt + 12);
        const int16_t adj = (int16_t)MEM_R16(c, vt + 8);
        uint32_t r[32];
        for (int i = 0; i < 32; i++) r[i] = c->r[i];
        c->r[29] -= 64;
        c->r[4] = obj + (uint32_t)(int32_t)adj, c->r[5] = 3;
        call_indirect(c, fn);
        for (int i = 0; i < 32; i++) c->r[i] = r[i];
    }
    MEM_W32(c, slot, 0);
    if ((int)MEM_R32(c, p + kCurrent) == type) MEM_W32(c, p + kCurrent, 0);  // back to the gun
    if ((int)MEM_R32(c, p + kPrevious) == type) MEM_W32(c, p + kPrevious, 0);
}

void give_weapon(R3000* c, uint32_t p, int type) {
    const uint32_t cur = MEM_R32(c, p + kCurrent), prev = MEM_R32(c, p + kPrevious);
    uint32_t r[32];
    for (int i = 0; i < 32; i++) r[i] = c->r[i];
    const uint32_t hi = c->hi, lo = c->lo;
    c->r[29] -= 64;
    c->r[4] = p, c->r[5] = (uint32_t)type;
    g_quiet = true;
    call_indirect(c, kGiveWeapon);
    g_quiet = false;
    for (int i = 0; i < 32; i++) c->r[i] = r[i];
    c->hi = hi, c->lo = lo;
    MEM_W32(c, p + kCurrent, cur);  // the weapon in hand stays
    MEM_W32(c, p + kPrevious, prev);
}

// A weapon the player holds must be a live object: its heap header's first word is then the
// block's allocation serial (a free block's is a free-list link), and its first word its class.
bool weapon_live(R3000* c, uint32_t obj) {
    if (!valid(obj) || (obj & 3)) return false;
    const uint32_t cls = MEM_R32(c, obj);
    return MEM_R32(c, obj - 8) < 0x80000000u && cls >= 0x80010000u && cls < 0x80200000u && !(cls & 3);
}

}  // namespace

// Safety net (the player update, 8005F688, and once a frame): a weapon slot left holding an
// object that was freed made the next shot call through the freed object (FATAL call to
// 80000000 from 8005D050, level 3). Such a slot is emptied and the player takes the gun.
void weapons_check(R3000* c, uint32_t p) {
    if (!valid(p) || core_in_ghost()) return;
    for (int t = 1; t <= kTypes; t++) {
        const uint32_t slot = p + kWeaponSlots + 4 * (uint32_t)(t - 1), obj = MEM_R32(c, slot);
        if (!obj || weapon_live(c, obj)) continue;
        static int told = 0;
        if (told++ < 5)
            log_msg(LogCat::Host, "weapon %d slot held a freed object %08X (current %d); emptied (frame %llu)", t, obj,
                    (int)MEM_R32(c, p + kCurrent), (unsigned long long)host_frame_count());
        MEM_W32(c, slot, 0);
        if ((int)MEM_R32(c, p + kCurrent) == t) MEM_W32(c, p + kCurrent, 0);
        if ((int)MEM_R32(c, p + kPrevious) == t) MEM_W32(c, p + kPrevious, 0);
    }
}

// Debug (APOC_WEAPON_TRACE=1): 8002FF34 free(block): a free of an object a weapon slot still
// holds, with the return addresses found on the stack.
extern "C" void psx_hook_free(R3000* c) {
    static const bool on = std::getenv("APOC_WEAPON_TRACE") != nullptr;
    if (!on || core_in_ghost()) return;
    const uint32_t p = player(c), a = c->r[4];
    if (!p) return;
    for (int t = 0; t <= kTypes; t++) {
        if (MEM_R32(c, p + 612 + 4 * (uint32_t)t) != a) continue;
        std::string chain;
        char b[16];
        for (uint32_t s = c->r[29]; s < c->r[29] + 256; s += 4) {
            const uint32_t w = MEM_R32(c, s);
            if (w >= 0x80010000u && w < 0x800A0000u && !(w & 3)) std::snprintf(b, sizeof b, " %08X", w), chain += b;
        }
        log_msg(LogCat::Host, "weapon trace: free of weapon %d object %08X (current %d) ra=%08X frame %llu stack:%s", t, a,
                (int)MEM_R32(c, p + kCurrent), c->r[31], (unsigned long long)host_frame_count(), chain.c_str());
    }
}

bool cheat_quiet() { return g_quiet; }

// A weapon pickup the player made (not ours): it is theirs to keep.
void cheat_note_pickup(uint32_t p, int type) {
    if (!g_quiet && p == g_given_player && type >= 1 && type <= kTypes) g_given &= ~(1u << type);
}

bool cheat_on(int which) { return which >= 0 && which < 5 && g_on[which]; }
void cheat_set(int which, bool on) {
    if (which >= 0 && which < 5) g_on[which] = on;
}

// Once per frame (savestate.cpp psx_safepoint).
void cheats_frame(R3000* c) {
    static bool invincible_set = false;
    if (static const char* t = std::getenv("APOC_TEST_CHEATS"); t) {  // debug: "10110" = cheats 0, 2, 3 on
        for (int i = 0; i < 5 && t[i]; i++) g_on[i] = t[i] == '1';
        t = nullptr;
    }
    if (static const char* t = std::getenv("APOC_TEST_CHEATS_OFF"); t && host_frame_count() >= std::strtoull(t, nullptr, 10)) {
        for (bool& on : g_on) on = false;  // debug: all off at that frame
        t = nullptr;
    }
    const uint32_t p = player(c);
    if (p) weapons_check(c, p);
    // health: the game's own switch, and only while ours is on (its cheat code can use it too)
    if (g_on[0]) MEM_W32(c, kInvincible, 1), invincible_set = true;
    else if (invincible_set) MEM_W32(c, kInvincible, 0), invincible_set = false;
    if (!p) {  // no level: nothing to keep, start again in the next one
        g_real_bombs = g_real_lives = -1;
        return;
    }
    keep(c, kLives, true, g_on[1], g_real_lives, g_seen_lives);
    if (p != g_bombs_player) g_real_bombs = -1, g_bombs_player = p;
    keep(c, p + kBombs, true, g_on[4], g_real_bombs, g_seen_bombs);
    // weapons
    if (p != g_given_player) g_given = 0, g_given_player = p;
    for (int t = 1; t <= kTypes; t++) {
        const bool have = valid(MEM_R32(c, p + kWeaponSlots + 4 * (uint32_t)(t - 1)));
        if (g_on[2] && !have) {
            give_weapon(c, p, t);
            g_given |= 1u << t;
        } else if (!g_on[2] && (g_given & (1u << t))) {
            remove_weapon(c, p, t);
            g_given &= ~(1u << t);
        }
    }
    // ammo
    for (int t = 0; t < kTypes; t++) {
        const uint32_t obj = MEM_R32(c, p + kWeaponSlots + 4 * (uint32_t)t);
        if (!valid(obj) || !g_on[3]) {
            g_ammo_obj[t] = 0;
            continue;
        }
        const int32_t now = (int32_t)MEM_R32(c, obj + kAmmo);
        if (obj != g_ammo_obj[t] || now > g_ammo[t]) g_ammo_obj[t] = obj, g_ammo[t] = now;
        else if (now < g_ammo[t]) MEM_W32(c, obj + kAmmo, (uint32_t)g_ammo[t]);
    }
}
