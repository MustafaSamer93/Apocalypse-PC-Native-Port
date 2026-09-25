// 60 Hz simulation (level 1 by default; F1 -> Simulation; --sim60 for every level).
//
// The level loop (80030AD4) normally calls the tick (8002FF90) twice per rendered frame
// and waits for two VBlanks; the tick advances a 60 Hz counter (gp+0x438) and updates
// the whole world only when that counter is even, so the game simulates and renders at
// 30 Hz. In 60 Hz mode the loop does one tick, one render and one VBlank wait per
// iteration (OPTIONAL_CALLS in tools/recomp/emit.py), the emulated CPU runs 8x faster so
// that fits in a VBlank, and:
//   even ticks: the game's own full update (everything, as before);
//   odd ticks:  the player and the cameras (which did nothing before).
// So the player and cameras - what the controls act on - update 60 times a second.
//
// Their per-update amounts assume 30 Hz. Measured on the player (level 1): position +=
// velocity; forward/strafe speed ramps 25,600 per update to 204,800 through a step counter
// (+0x260); the animation frame (+0x18, copied to +0x134/+0x136) advances by one. Retiming,
// per 60 Hz update:
//   - continuous state (position, velocity vectors, camera vectors) moves by half of the
//     change the update made, so motion covers the same distance in the same time;
//   - whole-number counters (animation frame, speed step) keep their 30 Hz rate: an
//     odd-tick +1 is undone;
//   - damage only lands on world-update ticks (shots move at 30 Hz and would count twice).
// Enemies, items and shots keep their 30 Hz logic; so everything moves every frame, the render
// right after a world update draws the world half way between before and after it: object
// positions and rotations, the screen tint (damage flash, fades), and every model's pose.
// Poses are blended by rendering twice: once, rolled back, with each object's animation
// state from before the update, recording every vertex and colour the GTE produced, then for
// real with each vertex and colour replaced by the average of the two (gte.cpp). The next
// render draws everything where it is.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "psx.h"
#include "state_io.h"

void bios_ghost_state(StateIO& s);
void hw_state(StateIO& s);
void gpu_regs_state(StateIO& s);

int psx_opt_sim60 = 0;

// Setting: 0 = 30 Hz everywhere (original), 1 = 60 Hz in level 1 (the tested level),
// 2 = 60 Hz in every level (untested). Attract-mode demos always run at 30 Hz: they replay
// input recorded per 30 Hz update.
int g_sim60_setting = 1;
bool g_sim60_force = false;  // --sim60: on in any level, even when it is not known yet
std::string g_sim60_level;  // base name of the level's trigger file ("prsn" = level 1)
bool g_sim60_demo = false;

void sim60_apply() {
    bool on = !g_sim60_demo &&
              (g_sim60_force || (!g_sim60_level.empty() && (g_sim60_setting == 2 ||
                                                            (g_sim60_setting == 1 && g_sim60_level == "prsn"))));
    if ((on ? 1 : 0) != psx_opt_sim60)
        log_msg(LogCat::Host, "60 Hz simulation %s (level \"%s\"%s)", on ? "on" : "off", g_sim60_level.c_str(),
                g_sim60_demo ? ", demo" : "");
    psx_opt_sim60 = on ? 1 : 0;
    psx_cpi_q = on ? 1 : 4 * PSX_CYCLES_PER_INSN;  // 8x CPU: an update and a render per VBlank
}

// Every file the game loads (bios.cpp): a level starts with its "<name>_t.trg".
void sim60_note_file(const std::string& file) {
    // the game prints a newline, then the name: keep just the name, lower case
    std::string f;
    for (char ch : file)
        if (!std::isspace((unsigned char)ch)) f += (char)std::tolower((unsigned char)ch);
    size_t t = f.find("_t.trg");
    if (t != std::string::npos) {
        g_sim60_level = f.substr(0, t);
        g_sim60_demo = false;
    }
    if (f.find(".dem") != std::string::npos) g_sim60_demo = true;
    sim60_apply();
}

namespace {

constexpr uint32_t kGp = 0x800FEEBC;
constexpr uint32_t kHalfTicks = kGp + 0x438;   // 60 Hz counter; odd = no world update
constexpr uint32_t kHalfTicks2 = kGp + 0x4D0;  // second counter the tick advances with it
constexpr uint32_t kPlayerList = 0x800FF5A0;
constexpr uint32_t kCameraList = 0x800FF444;
// The view camera: the object at the head of this list (the render draws from it). It moves by
// its own velocity (+0x68) and runs scripted moves with a countdown (+0x14C, e.g. the rise that
// frames a ledge climb) and a counter (+0x240), all per update: retimed like the player (a
// climb's camera moved twice as fast, stopped half way and jumped when the climb ended).
constexpr uint32_t kViewCamList = 0x800FF904;
constexpr int kViewCamVecs[] = {0x04, 0x68, 0x134};  // position, velocity, the point it moves towards
constexpr int kViewCamCounters[] = {0x14C, 0x240};  // an odd tick's step of 1 either way is undone
// Every other object list the world update runs (8002FF90; objects linked at +0x1C): enemies,
// items, pickups, gibs... (level 1 uses the first two).
constexpr uint32_t kOtherLists[] = {0x800FF4E8, 0x800FF5DC, 0x800FF204, 0x800FF220, 0x800FF8A0};
constexpr uint32_t kRender = 0x800307F0;

constexpr uint32_t kTick = 0x8002FF90;
constexpr uint32_t kUpdateList = 0x80063770;     // update every object of a list
constexpr uint32_t kContactsBegin = 0x80063CC4;  // per-update contact flags around the player
constexpr uint32_t kContactsEnd = 0x80063CF0;
constexpr uint32_t kUpdateCameras = 0x80037258;  // update one small-object list (a0 = head)

// Player object fields (vectors are x, y, z).
// position, world and local velocity. The velocities are mostly recomputed each update
// (from the speed step and facing); halving them matters where they are integrated (falls).
constexpr int kPlayerVecs[] = {0x04, 0x68, 0x98};
constexpr int kPlayerCounters[] = {0x260, 0x23A};  // speed step, time in the air (long-fall animation from 49)
// Animation clock: each update adds the rate (+0x130, 16.16 frames) to the fraction (+0x12C)
// and moves the frame (+0x134, copied to +0x136 and +0x18) on by the carry, 1 or 2 frames.
// While the animation (+0x1A) and its start frame (+0x128) stay the same, the clock (frame and
// fraction as one 16.16 number) moves by half of each update's advance, like a position: the
// animation keeps its speed (a ledge climb played twice as fast) and, at more than one frame
// per update, shows a new pose on every 60 Hz frame (undoing the extra tick's advance instead
// held each pose for two frames). A new animation or a wrap back to the start is kept.
constexpr int kAnimClock[] = {0x018, 0x12C, 0x134, 0x136};
constexpr int kAnimId = 0x1A, kAnimStart = 0x128;
// Player state (+0x1CC). In the ledge states (grab 0x1000/0x4000/0x8000, climb up 0x2000) the
// game places the player itself, so their position is not retimed: halving the step onto the
// ledge at the end of a climb left the player below it, falling, and climbing a second time.
constexpr int kPlayerState = 0x1CC;
bool ledge_state(uint32_t st) { return st & (0x1000 | 0x2000 | 0x4000 | 0x8000); }
constexpr int kCameraVecs[] = {0x18, 0x48, 0x54, 0x60};
// Health (16-bit). Enemy shots and hazards move only on the 30 Hz world update, so one
// still touching the player during the extra update would count twice: the player only
// takes damage on world-update ticks.
constexpr int kPlayerHealth = 0xDA;
constexpr int kJumpRise = 0x238;  // updates left in which holding jump keeps rising (8 at take-off)

bool valid_ptr(uint32_t a) { return a && (a & 0x1FFFFFFF) < 0x200000 && !(a & 3); }

// The world update skips the objects while either of these is set (8003001C: level start,
// cutscenes) and runs only the view camera's list: the player-only tick must do nothing then.
// Updating the small-object cameras there called an empty handler (address 0) and crashed at
// the start of level 1.
constexpr uint32_t kFreeze1 = 0x800FF008, kFreeze2 = kGp + 0x444;
bool world_frozen(R3000* c) {
    const bool f = MEM_R32(c, kFreeze1) || MEM_R32(c, kFreeze2);
    static bool was = false;
    if (f != was && std::getenv("APOC_SIM60_TRACE"))
        log_msg(LogCat::Host, "sim60 world %s f=%llu (%08X %08X)", f ? "frozen" : "running",
                (unsigned long long)host_frame_count(), MEM_R32(c, kFreeze1), MEM_R32(c, kFreeze2));
    was = f;
    return f;
}

void call(R3000* c, uint32_t fn, uint32_t a0 = 0) {
    c->r[4] = a0;
    call_indirect(c, fn);
}

struct Vec3 {
    int32_t v[3];
};

bool g_in_tick = false;
bool g_in_render = false;

// Positions of the other objects before the last world update (for the render after it).
struct ObjPos {
    uint32_t obj;
    int32_t before[3], after[3];      // position (+0x04)
    int16_t rot_before[3], rot_after[3];  // rotation (+0x10), 12-bit angles
};

constexpr int kObjRot = 0x10;

// Half way between two 12-bit angles, the short way round; large turns snap.
int16_t mid_angle(int16_t a, int16_t b, bool* ok) {
    int d = ((b - a) & 0xFFF);
    if (d >= 0x800) d -= 0x1000;
    if (d > 0x400 || d < -0x400) *ok = false;
    return (int16_t)(a + d / 2);
}
std::vector<ObjPos> g_moved;
bool g_mid_pending = false;

// Effects: gun flashes and their light, shot lights, beams, trails, sparks, smoke, shards...
// They are "small objects" in 13 lists (gp+0x578..0x5A8, linked at +0x04, class table at
// +0x44, age in the low half of +0x08), run by the world update (800372B8), so they moved at
// 30 Hz. The frame after a world update draws them half way, like the big objects: the fields
// the effect renderer (8003736C, one section per list) reads, found there and measured in
// level 1. Every effect keeps its position at +0x18 (20.12 fixed point).
// The cameras (gp+0x588) are updated at 60 Hz already and are left out.
constexpr uint32_t kEffectLists = kGp + 0x578, kEffectListsEnd = kGp + 0x5A8;
constexpr uint32_t kSpriteList = kGp + 0x57C, kGlowList = kGp + 0x594, kShardList = kGp + 0x598,
                   kTrailList = kGp + 0x5A0, kBeamList = kGp + 0x5A8;
constexpr int kEffectClass = 0x44, kEffectAge = 0x08, kEffectPos = 0x18;
constexpr int32_t kFar = 0x400000;  // a longer step (20.12) is a jump: a reused object, a teleport

enum class Kind : uint8_t {
    Point,   // x, y, z words
    Colour,  // 0xCCBBGGRR: each of the low three bytes
    Halves,  // two 16-bit numbers
    Number,
};
struct EffectField {
    uint32_t list, cls;  // 0 = any
    Kind kind;
    int off;
    int32_t limit;  // largest step drawn half way (Point, Halves, Number)
};
constexpr EffectField kEffectFields[] = {
    {0, 0, Kind::Point, kEffectPos, kFar},
    // sprites: sparks (A1C30), flashes (A1C60), smoke puffs (A1C78: size at +0x5E, spin angle at
    // +0x60 and its speed at +0x62)
    {kSpriteList, 0, Kind::Colour, 0x4C, 0},
    {kSpriteList, 0x800A1C78, Kind::Halves, 0x5C, 0x800},
    {kSpriteList, 0x800A1C78, Kind::Halves, 0x60, 0x800},
    // a chain's segments (A1BE0), each from one point to the next
    {0, 0x800A1BE0, Kind::Point, 0x60, kFar},
    {0, 0x800A1BE0, Kind::Point, 0x6C, kFar},
    // smoke (A1C90): a second point and a fading colour
    {0, 0x800A1C90, Kind::Point, 0x78, kFar},
    {0, 0x800A1C90, Kind::Colour, 0x4C, 0},
    // quads (A12C8): four corners in whole units
    {0, 0x800A12C8, Kind::Point, 0x68, 0x400},
    {0, 0x800A12C8, Kind::Point, 0x74, 0x400},
    {0, 0x800A12C8, Kind::Point, 0x80, 0x400},
    {0, 0x800A12C8, Kind::Point, 0x8C, 0x400},
    // lights: a fan of vertices round the position, turned by +0x60, in the colour +0x58
    {kGlowList, 0, Kind::Halves, 0x60, 0x400},
    {kGlowList, 0, Kind::Colour, 0x58, 0},
    // shards: three corners
    {kShardList, 0, Kind::Point, 0x48, kFar},
    {kShardList, 0, Kind::Point, 0x54, kFar},
    {kShardList, 0, Kind::Point, 0x60, kFar},
    // trails: the first point (the others are in a table, below)
    {kTrailList, 0, Kind::Point, 0x54, kFar},
    // beams: head, tail and a fading colour
    {kBeamList, 0, Kind::Point, 0x50, kFar},
    {kBeamList, 0, Kind::Point, 0x5C, kFar},
    {kBeamList, 0, Kind::Colour, 0x4C, 0},
};
// Tables an effect points to: +ptr, with +count entries of `stride` bytes.
struct EffectTable {
    uint32_t list;
    int ptr, count, stride;
    int point, colour;  // offsets in an entry, -1 = none
};
constexpr EffectTable kEffectTables[] = {
    {kGlowList, 0x48, 0x50, 8, -1, 4},    // lights: each vertex's distance (random), colour
    {kTrailList, 0x50, 0x4C, 16, 0, 12},  // trails: each further point and its colour
};
constexpr uint32_t kMaxTable = 64;

struct EffectObj {
    uint32_t obj, list, cls, age;
};
std::vector<EffectObj> g_effects_before;
std::vector<std::pair<uint32_t, uint32_t>> g_effect_words_before;  // address, value (sorted)

// One word drawn half way.
struct MidWord {
    uint32_t addr, before, after;
    Kind kind;
    uint32_t owner, list, cls;  // the effect it belongs to (for APOC_SIM60_CHECK)
    uint32_t serial, size_word;  // the owner's heap block header (see heap_block)
};
std::vector<MidWord> g_mid_words;
uint64_t g_effect_frames = 0, g_effect_words = 0;

// APOC_SIM60_CHECK=1 (debugging): after each step, every small object the world update will
// run (800372B8: not skipped by +0x3F) must have a class (+0x44) with an update (+0x14).
void check_small(R3000* c, const char* where, bool force = false) {
    static const bool on = std::getenv("APOC_SIM60_CHECK") != nullptr;
    static int reports = 0;
    if ((!on && !force) || reports > 20) return;
    for (uint32_t head = kGp + 0x578; head <= kGp + 0x5A8; head += 4) {
        uint32_t o = MEM_R32(c, head), prev = head - 4;
        for (int n = 0; o && n < 2048; n++, prev = o, o = MEM_R32(c, o + 4)) {
            if (!valid_ptr(o)) {
                log_msg(LogCat::Host, "CHECK %s f=%llu list %08X: bad link %08X", where, (unsigned long long)host_frame_count(), head, o);
                reports++;
                break;
            }
            if (MEM_R8(c, o + 0x3F)) continue;
            const uint32_t cls = MEM_R32(c, o + 0x44);
            if (valid_ptr(cls) && MEM_R32(c, cls + 0x14)) continue;
            log_msg(LogCat::Host, "CHECK %s f=%llu list %08X obj %08X cls %08X fn %08X", where,
                    (unsigned long long)host_frame_count(), head, o, cls, valid_ptr(cls) ? MEM_R32(c, cls + 0x14) : 0);
            log_msg(LogCat::Host, "    reached from %08X (cls %08X)", prev, MEM_R32(c, prev + 0x44));
            for (const MidWord& m : g_mid_words)
                if (m.addr >= prev && m.addr < prev + 0x100)
                    log_msg(LogCat::Host, "    prev+%02X by effect %08X list %08X cls %08X kind %d: %08X -> %08X", m.addr - prev,
                            m.owner, m.list, m.cls, (int)m.kind, m.before, m.after);
            reports++;
        }
    }
}

template <class F>
void for_each_effect(R3000* c, F f) {
    for (uint32_t head = kEffectLists; head <= kEffectListsEnd; head += 4) {
        if (head == kCameraList) continue;
        uint32_t o = MEM_R32(c, head);
        for (int n = 0; valid_ptr(o) && n < 1024; n++, o = MEM_R32(c, o + 4)) f(head, o);
    }
}

// Every field of an effect drawn half way: f(address, kind, limit); a Point is its x address.
template <class F>
void for_each_field(R3000* c, uint32_t list, uint32_t o, uint32_t cls, F f) {
    for (const EffectField& e : kEffectFields)
        if ((!e.list || e.list == list) && (!e.cls || e.cls == cls)) f(o + e.off, e.kind, e.limit);
    for (const EffectTable& t : kEffectTables) {
        if (t.list != list) continue;
        const uint32_t p = MEM_R32(c, o + t.ptr), n = MEM_R32(c, o + t.count);
        if (!valid_ptr(p) || n > kMaxTable) continue;
        for (uint32_t i = 0; i < n; i++) {
            if (t.point >= 0) f(p + i * t.stride + t.point, Kind::Point, kFar);
            if (t.colour >= 0) f(p + i * t.stride + t.colour, Kind::Colour, 0);
        }
    }
}

void effects_before(R3000* c) {
    g_effects_before.clear();
    g_effect_words_before.clear();
    for_each_effect(c, [&](uint32_t list, uint32_t o) {
        const uint32_t cls = MEM_R32(c, o + kEffectClass);
        g_effects_before.push_back({o, list, cls, MEM_R32(c, o + kEffectAge)});
        for_each_field(c, list, o, cls, [&](uint32_t a, Kind k, int32_t) {
            for (uint32_t i = 0; i < (k == Kind::Point ? 3u : 1u); i++)
                g_effect_words_before.push_back({a + 4 * i, MEM_R32(c, a + 4 * i)});
        });
        // the tables' addresses: a table moved elsewhere is not the same table
        for (const EffectTable& t : kEffectTables)
            if (t.list == list) g_effect_words_before.push_back({o + t.ptr, MEM_R32(c, o + t.ptr)});
    });
    std::sort(g_effects_before.begin(), g_effects_before.end(),
              [](const EffectObj& a, const EffectObj& b) { return a.obj < b.obj; });
    std::sort(g_effect_words_before.begin(), g_effect_words_before.end());
}

bool word_before(uint32_t a, uint32_t* v) {
    auto it = std::lower_bound(g_effect_words_before.begin(), g_effect_words_before.end(), std::make_pair(a, 0u));
    if (it == g_effect_words_before.end() || it->first != a) return false;
    *v = it->second;
    return true;
}

bool step_ok(int32_t was, int32_t now, int32_t limit) {
    const int64_t d = (int64_t)now - was;
    return d >= -limit && d <= limit;
}

// Where every small object starts (all lists, sorted). Objects are packed with different sizes,
// so a field is only drawn half way when it ends before the next object starts: smoke puffs
// (A1C90) can be 0x74 bytes, and their second point (+0x78) was then the next object's list
// link and age, half-way values that sent the lists into garbage (a crash in level 1).
std::vector<uint32_t> small_object_starts(R3000* c) {
    std::vector<uint32_t> v;
    for (uint32_t head = kEffectLists; head <= kEffectListsEnd; head += 4) {
        uint32_t o = MEM_R32(c, head);
        for (int n = 0; valid_ptr(o) && n < 1024; n++, o = MEM_R32(c, o + 4)) v.push_back(o & 0x1FFFFF);
    }
    std::sort(v.begin(), v.end());
    return v;
}

// The object a word belongs to: the nearest start at or below it, within 0x100 bytes (no
// small object is bigger); 0 = none (free memory, a table elsewhere).
uint32_t owner_of(const std::vector<uint32_t>& starts, uint32_t a) {
    auto it = std::upper_bound(starts.begin(), starts.end(), a);
    if (it == starts.begin()) return 0;
    --it;
    return a - *it < 0x100 ? *it : 0;
}

// Object o may draw [a, a + len) half way only if no word of it lies in another object: its own
// fields must end before the next object starts, and its tables must not overlap live objects.
bool span_free(const std::vector<uint32_t>& starts, uint32_t o, uint32_t a, uint32_t len) {
    a &= 0x1FFFFF, o &= 0x1FFFFF;
    for (uint32_t x : {a, a + len - 1}) {
        const uint32_t s = owner_of(starts, x);
        if (s && s != o) return false;
    }
    auto it = std::upper_bound(starts.begin(), starts.end(), a);  // an object starting inside
    return it == starts.end() || *it >= a + len;
}

// Every small object list intact: links point into RAM, and every object the world update will
// run (800372B8: +0x3F clear) has a class (+0x44) with an update function (+0x14).
bool small_lists_ok(R3000* c) {
    for (uint32_t head = kGp + 0x578; head <= kGp + 0x5A8; head += 4) {
        uint32_t o = MEM_R32(c, head);
        for (int n = 0; o; n++, o = MEM_R32(c, o + 4)) {
            if (!valid_ptr(o) || n >= 2048) return false;
            if (MEM_R8(c, o + 0x3F)) continue;
            const uint32_t cls = MEM_R32(c, o + 0x44);
            if (!valid_ptr(cls) || !MEM_R32(c, cls + 0x14)) return false;
        }
    }
    return true;
}

// Effects live in the game's heap (8006B864): 8 bytes before each block, a serial number that
// is new for every allocation (-8) and the size << 4 (-4). The size bounds the fields an effect
// may draw half way (the field table was measured on the biggest variant: level 1's smoke
// blocks are 0x58 bytes, but its second point is at +0x78, in the next block), and a serial
// that changed means the block was freed and allocated again.
bool heap_block(R3000* c, uint32_t o, uint32_t* serial, uint32_t* size_word) {
    *serial = MEM_R32(c, o - 8);
    *size_word = MEM_R32(c, o - 4);
    const uint32_t size = *size_word >> 4;
    return size >= 0x40 && size <= 0x400;
}

void effects_after(R3000* c) {
    g_mid_words.clear();
    const std::vector<uint32_t> starts = small_object_starts(c);
    for_each_effect(c, [&](uint32_t list, uint32_t o) {
        auto it = std::lower_bound(g_effects_before.begin(), g_effects_before.end(), o,
                                   [](const EffectObj& a, uint32_t v) { return a.obj < v; });
        if (it == g_effects_before.end() || it->obj != o) return;  // new: drawn where it starts
        const uint32_t cls = MEM_R32(c, o + kEffectClass);
        if (cls != it->cls || list != it->list) return;
        // an age that went back, or a position that jumped: freed and reused for a new effect
        if ((uint16_t)MEM_R32(c, o + kEffectAge) < (uint16_t)it->age) return;
        for (int k = 0; k < 3; k++) {
            uint32_t was;
            if (!word_before(o + kEffectPos + 4 * k, &was) ||
                !step_ok((int32_t)was, (int32_t)MEM_R32(c, o + kEffectPos + 4 * k), kFar))
                return;
        }
        for (const EffectTable& t : kEffectTables) {
            uint32_t was;
            if (t.list == list && (!word_before(o + t.ptr, &was) || was != MEM_R32(c, o + t.ptr))) return;
        }
        uint32_t serial, size_word;
        if (!heap_block(c, o, &serial, &size_word)) return;
        const uint32_t size = size_word >> 4;
        for_each_field(c, list, o, cls, [&](uint32_t a, Kind k, int32_t limit) {
            const int n = k == Kind::Point ? 3 : 1;
            // inside its own block (tables it points to are elsewhere: the span check below)
            if (a >= o && a < o + 0x100 && a + 4 * n > o + size) return;
            if (!span_free(starts, o, a, 4 * n)) return;
            uint32_t was[3], now[3];
            bool moved = false;
            for (int i = 0; i < n; i++) {
                if (!word_before(a + 4 * i, &was[i])) return;
                now[i] = MEM_R32(c, a + 4 * i);
                if (now[i] != was[i]) moved = true;
                if (k == Kind::Point || k == Kind::Number) {
                    if (!step_ok((int32_t)was[i], (int32_t)now[i], limit)) return;
                } else if (k == Kind::Halves) {
                    for (int s = 0; s < 32; s += 16)
                        if (!step_ok(0, (int16_t)((now[i] >> s) - (was[i] >> s)), limit)) return;
                }
            }
            if (moved)
                for (int i = 0; i < n; i++)
                    g_mid_words.push_back({a + 4 * (uint32_t)i, was[i], now[i], k == Kind::Point ? Kind::Number : k, o, list, cls,
                                           serial, size_word});
        });
    });
    g_effects_before.clear();
    g_effect_words_before.clear();
}

uint32_t mid_word(const MidWord& m) {
    switch (m.kind) {
        case Kind::Colour: {
            uint32_t r = m.after & 0xFF000000u;
            for (int s = 0; s < 24; s += 8) r |= ((((m.before >> s) & 0xFF) + ((m.after >> s) & 0xFF) + 1) / 2) << s;
            return r;
        }
        case Kind::Halves: {
            uint32_t r = 0;
            for (int s = 0; s < 32; s += 16) {
                const int16_t d = (int16_t)((m.after >> s) - (m.before >> s));
                r |= (uint32_t)(uint16_t)((m.before >> s) + d / 2) << s;
            }
            return r;
        }
        default: return (uint32_t)((int32_t)m.before + ((int32_t)m.after - (int32_t)m.before) / 2);
    }
}

// Flickering lights (class 800A1154: a fan of coloured vertices on the walls and floor, e.g. the
// flash of a gun being fired): 8001D114 gives every vertex a random distance and the fan a random
// angle. The game does that when a light is made (a gun's flash lives for one update and is made
// again every update) and, for lights flagged at +0x78, every update: they flickered at 30 Hz. On
// the player-only ticks they flicker once more, with the game's random numbers (gp+0xD58..D60)
// put back afterwards and without the fade the flicker also steps (+0x6E..+0x74 by +0x76), so the
// game plays exactly as before.
constexpr uint32_t kFlickerClass = 0x800A1154, kFlicker = 0x8001D114, kRandState = kGp + 0xD58;
void flicker_lights(R3000* c) {
    static const bool off = std::getenv("APOC_NO_EFFECT_BLEND") != nullptr;
    if (off) return;
    uint32_t lights[64];
    int n = 0;
    for_each_effect(c, [&](uint32_t list, uint32_t o) {
        if (n < 64 && list == kGlowList && MEM_R32(c, o + kEffectClass) == kFlickerClass && !MEM_R8(c, o + 0x3F) &&
            (MEM_R8(c, o + 0x78) || MEM_R16(c, o + 0x0A) == 1) && MEM_R32(c, o + 0x50) <= kMaxTable)
            lights[n++] = o;
    });
    if (!n) return;
    uint32_t rnd[3];
    for (int i = 0; i < 3; i++) rnd[i] = MEM_R32(c, kRandState + 4 * i);
    for (int g = 0; g < n; g++) {
        uint16_t fade[4];
        for (int i = 0; i < 4; i++) fade[i] = (uint16_t)MEM_R16(c, lights[g] + 0x6E + 2 * i);
        call(c, kFlicker, lights[g]);
        for (int i = 0; i < 4; i++) MEM_W16(c, lights[g] + 0x6E + 2 * i, fade[i]);
    }
    for (int i = 0; i < 3; i++) MEM_W32(c, kRandState + 4 * i, rnd[i]);
}

// An object's animation state (everything that picks its pose) and damage flash, from
// before the last world update: the frame after it is drawn half way between the pose
// before and the pose after (sim60_render).
constexpr int kAnimA = 0x16, kAnimALen = 5;     // mesh frame index, frame counter, animation
constexpr int kAnimB = 0x128, kAnimBLen = 0x14;  // animation playback state
constexpr int kFlash = 0x1D0;                    // hit timer / damage flash (16-bit), -256 per update
constexpr int kType = 0x1B;
struct AnimState {
    uint32_t obj;
    uint8_t type;
    uint8_t a[kAnimALen], b[kAnimBLen];
    uint16_t flash;
};
std::vector<AnimState> g_prev_anim;
uint64_t g_blend_frames = 0;

// Screen tint (the red damage flash and the game's other fades): red, green and blue as
// 16.16 values that the world update steps (8001BF34); the render draws a full-screen
// translucent quad in that colour (8001C004). Drawn half way after a world update.
constexpr uint32_t kTint = kGp + 0xCA4;
int32_t g_tint_before[3], g_tint_after[3];

Vec3 rd(R3000* c, uint32_t a) {
    return {{(int32_t)MEM_R32(c, a), (int32_t)MEM_R32(c, a + 4), (int32_t)MEM_R32(c, a + 8)}};
}

// Everything the retiming needs from before an update.
struct Before {
    uint32_t player = 0;
    Vec3 pv[std::size(kPlayerVecs)];
    uint16_t counters[std::size(kPlayerCounters)];
    uint16_t clock[std::size(kAnimClock)];
    uint8_t anim_id = 0;
    uint32_t state = 0;
    uint16_t anim_start = 0;
    int16_t health = 0;
    int16_t flash = 0;
    uint8_t rise = 0;
    uint32_t cams[8] = {};
    Vec3 cv[8][std::size(kCameraVecs)];
    int ncams = 0;
};

Before capture(R3000* c) {
    Before b;
    b.player = MEM_R32(c, kPlayerList);
    if (valid_ptr(b.player)) {
        for (size_t i = 0; i < std::size(kPlayerVecs); i++) b.pv[i] = rd(c, b.player + kPlayerVecs[i]);
        for (size_t i = 0; i < std::size(kPlayerCounters); i++)
            b.counters[i] = (uint16_t)MEM_R16(c, b.player + kPlayerCounters[i]);
        for (size_t i = 0; i < std::size(kAnimClock); i++) b.clock[i] = (uint16_t)MEM_R16(c, b.player + kAnimClock[i]);
        b.anim_id = (uint8_t)MEM_R8(c, b.player + kAnimId);
        b.state = MEM_R32(c, b.player + kPlayerState);
        b.anim_start = (uint16_t)MEM_R16(c, b.player + kAnimStart);
        b.health = (int16_t)MEM_R16(c, b.player + kPlayerHealth);
        b.flash = (int16_t)MEM_R16(c, b.player + kFlash);
        b.rise = (uint8_t)MEM_R8(c, b.player + kJumpRise);
    }
    uint32_t cam = MEM_R32(c, kCameraList);
    for (; valid_ptr(cam) && b.ncams < 8; cam = MEM_R32(c, cam + 4)) {
        b.cams[b.ncams] = cam;
        for (size_t i = 0; i < std::size(kCameraVecs); i++) b.cv[b.ncams][i] = rd(c, cam + kCameraVecs[i]);
        b.ncams++;
    }
    return b;
}

// What the last update itself left in each retimed vector, before halving (see halve).
struct Raw {
    uint32_t addr = 0;
    Vec3 v{};
};
Raw g_raw[1 + 8 + 1][4];  // player, up to 8 cameras, the view camera

// Moves a vector back to halfway between its old and new value. Jumps larger than any
// real motion (teleports, respawns, a reused object) are left alone. A component the update
// set to the same value as the update before it is a value the game assigns (the rising
// speed of a jump, 0 on landing, a position held during a climb) rather than one it
// integrates, and is kept: halving it again every update would only ease towards it (the
// jump started slowly and peaked 12% lower).
void halve(R3000* c, uint32_t addr, const Vec3& old, Raw& raw) {
    Vec3 now = rd(c, addr);
    const bool same_obj = raw.addr == addr;
    raw.addr = addr;
    const Vec3 prev = raw.v;
    raw.v = now;
    for (int k = 0; k < 3; k++) {
        int64_t d = (int64_t)now.v[k] - old.v[k];
        if (d > 0x400000 || d < -0x400000) return;
    }
    // rounded away from `old`, so the value always reaches what the game set (truncated, a
    // speed the game set to 0 stayed at 1 or 2)
    for (int k = 0; k < 3; k++) {
        if (same_obj && now.v[k] == prev.v[k]) continue;
        int32_t d = now.v[k] - old.v[k];
        MEM_W32(c, addr + 4 * k, (uint32_t)(old.v[k] + (d + (d > 0) - (d < 0)) / 2));
    }
}

void retime(R3000* c, const Before& b, bool odd) {
    uint32_t p = MEM_R32(c, kPlayerList);
    if (valid_ptr(p) && p == b.player) {
        const bool placed = ledge_state(b.state) || ledge_state(MEM_R32(c, p + kPlayerState));
        for (size_t i = 0; i < std::size(kPlayerVecs); i++)
            if (!placed) halve(c, p + kPlayerVecs[i], b.pv[i], g_raw[0][i]);
        if (MEM_R8(c, p + kAnimId) == b.anim_id && MEM_R16(c, p + kAnimStart) == b.anim_start) {
            const int32_t was = (int32_t)b.clock[2] << 16 | b.clock[1];
            const int32_t now = (int32_t)MEM_R16(c, p + 0x134) << 16 | MEM_R16(c, p + 0x12C);
            if (now > was && now - was <= (8 << 16)) {
                const int32_t mid = was + (now - was + 1) / 2;
                MEM_W16(c, p + 0x12C, (uint16_t)mid);
                for (int off : {0x018, 0x134, 0x136}) MEM_W16(c, p + off, (uint16_t)(mid >> 16));
            }
        }
        if (odd) {
            for (size_t i = 0; i < std::size(kPlayerCounters); i++) {
                uint32_t a = p + kPlayerCounters[i];
                if ((uint16_t)(MEM_R16(c, a) - b.counters[i]) == 1) MEM_W16(c, a, b.counters[i]);
            }
            if ((int16_t)MEM_R16(c, p + kPlayerHealth) < b.health) MEM_W16(c, p + kPlayerHealth, (uint16_t)b.health);
            // jump rise counter: holding jump keeps rising while it is above 0 (80061584); it
            // counts down 1 per update, so at 60 Hz a long press rose only half as high
            if ((uint8_t)MEM_R8(c, p + kJumpRise) == (uint8_t)(b.rise - 1)) MEM_W8(c, p + kJumpRise, b.rise);
        }
        // The hit timer (it also sets the damage flash) counts down 256 per update and the player
        // stays in the hit state until it is exactly 0. It keeps its 30 Hz rate: the odd-tick
        // step is undone. (Halving the step left 128, never 0: the player froze after a hit, on
        // the last frame of the hit animation.) The flash is still drawn fading every frame by
        // the pose blend (sim60_render).
        if (odd) {
            int16_t now = (int16_t)MEM_R16(c, p + kFlash);
            if (now < b.flash && now >= 0) MEM_W16(c, p + kFlash, (uint16_t)b.flash);
        }
    }
    uint32_t cam = MEM_R32(c, kCameraList);
    for (int n = 0; valid_ptr(cam) && n < 8; n++, cam = MEM_R32(c, cam + 4)) {
        for (int j = 0; j < b.ncams; j++) {
            if (b.cams[j] != cam) continue;
            for (size_t i = 0; i < std::size(kCameraVecs); i++) halve(c, cam + kCameraVecs[i], b.cv[j][i], g_raw[1 + n][i]);
        }
    }
}

// The view camera is updated by the render (80078760), which runs every 60 Hz tick: it is
// retimed around each render, its counters keeping their 30 Hz rate on the renders that follow
// a player-only tick.
struct ViewCam {
    uint32_t obj = 0;
    Vec3 v[std::size(kViewCamVecs)];
    uint16_t n[std::size(kViewCamCounters)];
};
ViewCam view_before(R3000* c) {
    ViewCam b;
    b.obj = MEM_R32(c, kViewCamList);
    if (!valid_ptr(b.obj)) return b;
    for (size_t i = 0; i < std::size(kViewCamVecs); i++) b.v[i] = rd(c, b.obj + kViewCamVecs[i]);
    for (size_t i = 0; i < std::size(kViewCamCounters); i++) b.n[i] = (uint16_t)MEM_R16(c, b.obj + kViewCamCounters[i]);
    return b;
}
void view_retime(R3000* c, const ViewCam& b) {
    const uint32_t v = MEM_R32(c, kViewCamList);
    if (!valid_ptr(v) || v != b.obj) return;
    for (size_t i = 0; i < std::size(kViewCamVecs); i++) halve(c, v + kViewCamVecs[i], b.v[i], g_raw[9][i]);
    if (MEM_R32(c, kHalfTicks) & 1) return;  // the last tick was a world update
    for (size_t i = 0; i < std::size(kViewCamCounters); i++) {
        const uint16_t now = (uint16_t)MEM_R16(c, v + kViewCamCounters[i]);
        if (now != b.n[i] && (uint16_t)(now - b.n[i] + 1) <= 2) MEM_W16(c, v + kViewCamCounters[i], b.n[i]);
    }
}

// APOC_SIM60_TRACE=1: log the player's position at every tick (for measuring speeds).
void trace_player(R3000* c, uint32_t t) {
    static const bool on = std::getenv("APOC_SIM60_TRACE") != nullptr;
    uint32_t p = on ? MEM_R32(c, kPlayerList) : 0;
    if (p)
        log_msg(LogCat::Host, "sim60 f=%llu tick=%u player %d %d %d health %d flash %d anim %d", (unsigned long long)host_frame_count(), t,
                (int32_t)MEM_R32(c, p + 4), (int32_t)MEM_R32(c, p + 8), (int32_t)MEM_R32(c, p + 12),
                (int16_t)MEM_R16(c, p + kPlayerHealth), (int16_t)MEM_R16(c, p + kFlash), (int16_t)MEM_R16(c, p + 0x18));
    // APOC_SIM60_DUMP=FROM,TO: the player object's first 0x300 bytes as words, each tick
    static const char* dump = std::getenv("APOC_SIM60_DUMP");
    if (p && dump) {
        unsigned long long f0 = 0, f1 = 0;
        std::sscanf(dump, "%llu,%llu", &f0, &f1);
        uint64_t f = host_frame_count();
        if (f >= f0 && f <= f1) {
            std::string line;
            char b[16];
            for (int i = 0; i < 0x300; i += 4) {
                std::snprintf(b, sizeof b, " %d", (int32_t)MEM_R32(c, p + i));
                line += b;
            }
            log_msg(LogCat::Host, "sim60dump f=%llu tick=%u%s", (unsigned long long)f, t, line.c_str());
            uint32_t cam = MEM_R32(c, kCameraList);
            if (valid_ptr(cam)) {
                line.clear();
                for (int i = 0; i < 0xA0; i += 4) {
                    std::snprintf(b, sizeof b, " %d", (int32_t)MEM_R32(c, cam + i));
                    line += b;
                }
                log_msg(LogCat::Host, "sim60cam f=%llu tick=%u%s", (unsigned long long)f, t, line.c_str());
            }
        }
    }
}

}  // namespace

// Runs the render once outside emulated time (everything rolled back afterwards) with every
// object's animation state from before the world update, recording what the GTE produced.
bool record_previous_pose(R3000* c) {
    static const bool off = std::getenv("APOC_NO_POSE_BLEND") != nullptr;  // for A/B comparisons
    if (off) return false;
    bool changed = false;
    for (const AnimState& a : g_prev_anim) {
        if (MEM_R8(c, a.obj + kType) != a.type) continue;
        for (int i = 0; i < kAnimALen && !changed; i++) changed = MEM_R8(c, a.obj + kAnimA + i) != a.a[i];
        for (int i = 0; i < kAnimBLen && !changed; i++) changed = MEM_R8(c, a.obj + kAnimB + i) != a.b[i];
        if (!changed) changed = MEM_R16(c, a.obj + kFlash) != a.flash;
        if (changed) break;
    }
    if (!changed) return false;  // nothing animated: the plain render is exact

    static std::jmp_buf abort;
    const R3000 saved = *c;
    StateIO snap(false);
    bios_ghost_state(snap);
    hw_state(snap);
    gpu_regs_state(snap);
    volatile bool done = false;
    core_ghost_begin(c, 40'000'000, &abort);
    if (setjmp(abort) == 0) {
        for (const AnimState& a : g_prev_anim) {
            if (MEM_R8(c, a.obj + kType) != a.type) continue;  // object replaced since
            for (int i = 0; i < kAnimALen; i++) MEM_W8(c, a.obj + kAnimA + i, a.a[i]);
            for (int i = 0; i < kAnimBLen; i++) MEM_W8(c, a.obj + kAnimB + i, a.b[i]);
            MEM_W16(c, a.obj + kFlash, a.flash);
        }
        gte_blend_begin(GteBlendMode::Record);
        c->r[31] = 0;
        call(c, kRender);
        done = true;
    }
    gte_blend_begin(GteBlendMode::Off);
    core_ghost_end(c);
    *c = saved;
    StateIO back(snap.data());
    bios_ghost_state(back);
    hw_state(back);
    gpu_regs_state(back);
    if (done && (++g_blend_frames % 600) == 0)
        log_msg(LogCat::Host, "sim60 pose blend: %u vertices blended, %u kept, %u colours", g_blend_stats[0],
                g_blend_stats[1], g_blend_stats[2]);
    return done;
}

// The level's render (called from smooth.cpp's render hook when psx_opt_sim60 is on).
int sim60_render(R3000* c) {
    if (g_in_render) return 0;
    const ViewCam cam = view_before(c);
    if (!g_mid_pending) {
        const uint32_t ra = c->r[31], sp = c->r[29];
        g_in_render = true;
        call(c, kRender);
        g_in_render = false;
        c->r[31] = ra;
        c->r[29] = sp;
        view_retime(c, cam);
        return 1;
    }
    g_mid_pending = false;
    // Only objects still in their lists now: the odd tick may have freed some since the world
    // update, and freed memory keeps its old values, so a value check alone lets the half-way
    // values land in the game's heap (a later allocation then overlapped a live object).
    std::vector<uint32_t> live_big;
    for (uint32_t head : kOtherLists) {
        uint32_t o = MEM_R32(c, head);
        for (int n = 0; valid_ptr(o) && n < 4096; n++, o = MEM_R32(c, o + 0x1C)) live_big.push_back(o);
    }
    std::sort(live_big.begin(), live_big.end());
    std::vector<std::pair<uint32_t, uint32_t>> live_small;  // object, list
    for_each_effect(c, [&](uint32_t list, uint32_t o) { live_small.push_back({o, list}); });
    std::sort(live_small.begin(), live_small.end());
    std::vector<const ObjPos*> set;
    for (const ObjPos& op : g_moved) {
        if (rd(c, op.obj + 4).v[0] != op.after[0]) continue;  // moved again or gone
        if (!std::binary_search(live_big.begin(), live_big.end(), op.obj)) continue;
        for (int k = 0; k < 3; k++) MEM_W32(c, op.obj + 4 + 4 * k, (uint32_t)(op.before[k] + (op.after[k] - op.before[k]) / 2));
        bool turn = true;
        int16_t mid[3];
        for (int k = 0; k < 3; k++) mid[k] = mid_angle(op.rot_before[k], op.rot_after[k], &turn);
        if (turn)
            for (int k = 0; k < 3; k++) MEM_W16(c, op.obj + kObjRot + 2 * k, (uint16_t)mid[k]);
        set.push_back(&op);
    }
    std::vector<const MidWord*> words;
    static const bool no_effects = std::getenv("APOC_NO_EFFECT_BLEND") != nullptr;  // for A/B comparisons
    const bool lists_were_ok = small_lists_ok(c);
    for (const MidWord& m : g_mid_words) {
        if (no_effects || MEM_R32(c, m.addr) != m.after) continue;  // changed since (freed, reused)
        if (!std::binary_search(live_small.begin(), live_small.end(), std::make_pair(m.owner, m.list)) ||
            MEM_R32(c, m.owner + kEffectClass) != m.cls || MEM_R32(c, m.owner - 8) != m.serial ||
            MEM_R32(c, m.owner - 4) != m.size_word)
            continue;  // freed since (and maybe allocated again)
        MEM_W32(c, m.addr, mid_word(m));
        words.push_back(&m);
    }
    // Safety net: if the half-way words broke a list (a field that was not what it seemed), put
    // them all back before the game walks the lists, and say which effect did it (once).
    if (lists_were_ok && !words.empty() && !small_lists_ok(c)) {
        static int told = 0;
        if (told++ < 3) {
            log_msg(LogCat::Host, "sim60: half-way effect words broke an object list; undone (frame %llu)",
                    (unsigned long long)host_frame_count());
            check_small(c, "mid words", true);
        }
        for (const MidWord* m : words) MEM_W32(c, m->addr, m->after);
        words.clear();
    }
    g_effect_words += words.size();
    if ((++g_effect_frames % 600) == 0) {
        log_msg(LogCat::Host, "sim60 effects: %.1f words drawn half way per frame", g_effect_words / 600.0);
        g_effect_words = 0;
    }
    // the tint half way, unless it jumped (a new flash starts at full strength)
    bool tint = true;
    for (int k = 0; k < 3; k++) {
        if ((int32_t)MEM_R32(c, kTint + 4 * k) != g_tint_after[k]) tint = false;
        if (std::abs((int64_t)g_tint_after[k] - g_tint_before[k]) > (0x40 << 16)) tint = false;
    }
    if (tint)
        for (int k = 0; k < 3; k++)
            MEM_W32(c, kTint + 4 * k, (uint32_t)(g_tint_before[k] + (g_tint_after[k] - g_tint_before[k]) / 2));
    const uint32_t ra = c->r[31], sp = c->r[29];
    g_in_render = true;
    check_small(c, "mid words");
    const bool blend = record_previous_pose(c);
    check_small(c, "ghost render");
    gte_blend_begin(blend ? GteBlendMode::Blend : GteBlendMode::Off);
    call(c, kRender);
    check_small(c, "mid render");
    gte_blend_begin(GteBlendMode::Off);
    g_in_render = false;
    c->r[31] = ra;
    c->r[29] = sp;
    view_retime(c, cam);
    if (tint)
        for (int k = 0; k < 3; k++) MEM_W32(c, kTint + 4 * k, (uint32_t)g_tint_after[k]);
    // back to the real values, but only words still holding what was written here (the render
    // may have changed or freed something since)
    for (const MidWord* m : words)
        if (MEM_R32(c, m->addr) == mid_word(*m)) MEM_W32(c, m->addr, m->after);
    for (const ObjPos* op : set) {
        for (int k = 0; k < 3; k++) {
            const uint32_t a = op->obj + 4 + 4 * k;
            if (MEM_R32(c, a) == (uint32_t)(op->before[k] + (op->after[k] - op->before[k]) / 2))
                MEM_W32(c, a, (uint32_t)op->after[k]);
        }
        bool turn = true;
        int16_t mid[3];
        for (int k = 0; k < 3; k++) mid[k] = mid_angle(op->rot_before[k], op->rot_after[k], &turn);
        for (int k = 0; k < 3; k++) {
            const uint32_t a = op->obj + kObjRot + 2 * k;
            if (turn && (int16_t)MEM_R16(c, a) == mid[k]) MEM_W16(c, a, (uint16_t)op->rot_after[k]);
        }
    }
    check_small(c, "restore");
    return 1;
}

// One jump height: the game keeps a jump rising while the jump button is held and the rise
// counter (+0x238, 8 updates) lasts (80061584); a short press gave a low jump that could not
// reach ledges such as level 1's towers. While a jump rises, the button (the player's button
// table +0x1BC, jump held at +0x110) counts as held, so every jump rises the full height.
extern "C" void psx_hook_player(R3000* c) {
    const uint32_t p = c->r[4];
    if (!valid_ptr(p) || p != MEM_R32(c, kPlayerList)) return;
    if (MEM_R32(c, p + kPlayerState) != 2 || MEM_R8(c, p + kJumpRise) == 0) return;
    const uint32_t buttons = MEM_R32(c, p + 0x1BC);
    if (valid_ptr(buttons)) MEM_W8(c, buttons + 0x110, 1);
}

extern "C" int psx_hook_tick(R3000* c) {
    if (g_in_tick) return 0;  // the game's own tick, run from below
    uint32_t t = MEM_R32(c, kHalfTicks);
    trace_player(c, t);
    if (!psx_opt_sim60) return 0;
    check_small(c, t & 1 ? "before odd tick" : "before world tick");

    const uint32_t ra = c->r[31], sp = c->r[29];
    const Before before = capture(c);

    if (!(t & 1)) {
        // even: the game's full update (it advances the counters itself)
        g_prev_anim.clear();
        for (uint32_t head : {kPlayerList, kOtherLists[0], kOtherLists[1], kOtherLists[2], kOtherLists[3], kOtherLists[4]}) {
            uint32_t o = MEM_R32(c, head);
            for (int n = 0; valid_ptr(o) && n < 4096; n++, o = MEM_R32(c, o + 0x1C)) {
                AnimState a{o, (uint8_t)MEM_R8(c, o + kType), {}, {}, (uint16_t)MEM_R16(c, o + kFlash)};
                for (int i = 0; i < kAnimALen; i++) a.a[i] = (uint8_t)MEM_R8(c, o + kAnimA + i);
                for (int i = 0; i < kAnimBLen; i++) a.b[i] = (uint8_t)MEM_R8(c, o + kAnimB + i);
                g_prev_anim.push_back(a);
            }
        }
        std::vector<ObjPos> objs;
        for (uint32_t head : kOtherLists) {
            uint32_t o = MEM_R32(c, head);
            for (int n = 0; valid_ptr(o) && n < 4096; n++, o = MEM_R32(c, o + 0x1C)) {
                Vec3 p = rd(c, o + 4);
                ObjPos op{o, {p.v[0], p.v[1], p.v[2]}, {}, {}, {}};
                for (int k = 0; k < 3; k++) op.rot_before[k] = (int16_t)MEM_R16(c, o + kObjRot + 2 * k);
                objs.push_back(op);
            }
        }
        for (int k = 0; k < 3; k++) g_tint_before[k] = (int32_t)MEM_R32(c, kTint + 4 * k);
        effects_before(c);
        g_in_tick = true;
        call(c, kTick);
        g_in_tick = false;
        effects_after(c);
        for (int k = 0; k < 3; k++) g_tint_after[k] = (int32_t)MEM_R32(c, kTint + 4 * k);
        g_moved.clear();
        for (ObjPos& op : objs) {
            Vec3 p = rd(c, op.obj + 4);
            bool moved = false, near = true;
            for (int k = 0; k < 3; k++) {
                op.after[k] = p.v[k];
                int64_t d = (int64_t)p.v[k] - op.before[k];
                if (d) moved = true;
                if (d > 0x400000 || d < -0x400000) near = false;
                op.rot_after[k] = (int16_t)MEM_R16(c, op.obj + kObjRot + 2 * k);
                if (op.rot_after[k] != op.rot_before[k]) moved = true;
            }
            if (moved && near) g_moved.push_back(op);
        }
        g_mid_pending = true;
    } else {
        // odd: the player and the cameras
        MEM_W32(c, kHalfTicks, t + 1);
        MEM_W32(c, kHalfTicks2, MEM_R32(c, kHalfTicks2) + 1);
        if (world_frozen(c)) {
            c->r[31] = ra;
            c->r[29] = sp;
            return 1;
        }
        call(c, kContactsBegin);
        call(c, kUpdateList, kPlayerList);
        call(c, kContactsEnd);
        call(c, kUpdateCameras, MEM_R32(c, kCameraList));
        check_small(c, "odd tick");
        flicker_lights(c);
        check_small(c, "flicker");
    }
    retime(c, before, t & 1);
    c->r[31] = ra;
    c->r[29] = sp;
    return 1;
}
