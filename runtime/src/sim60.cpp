// 60 Hz simulation prototype (--sim60).
//
// The level loop (80030AD4) normally calls the tick (8002FF90) twice per rendered frame
// and waits for two VBlanks; the tick advances a 60 Hz counter (gp+0x438) and updates
// the whole world only when that counter is even, so the game simulates and renders at
// 30 Hz. With psx_opt_sim60 the loop does one tick, one render and one VBlank wait per
// iteration (OPTIONAL_CALLS in tools/recomp/emit.py), the emulated CPU runs 4x faster so
// that fits in a VBlank (main.cpp), and:
//   even ticks: the game's own full update (everything, as before);
//   odd ticks:  the player and the cameras only (they did nothing before).
// So the player and cameras update 60 times a second and everything else 30.
//
// Their per-update amounts still assume 30 Hz. Measured on the player (level 1): position
// += velocity; forward/strafe speed ramps 25,600 per update to 204,800 through a step
// counter (+0x260); sideways drift halves every update; animation and age counters +1.
// Retiming, per 60 Hz update:
//   - continuous state (position, velocity vectors, camera vectors) moves by half of
//     the change the update made; integrated motion (speed from acceleration, position
//     from speed) then covers the same distance in the same time as at 30 Hz;
//   - whole-number counters (animation frame, speed-ramp step, age) keep their 30 Hz
//     rate: an odd-tick +1 is undone.
// Everything else still moves at 30 Hz; so that it moves evenly on a 60 fps display, the
// render right after a world update draws those objects half way from where they were
// before it (half a step behind), and the next render draws them where they are.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "psx.h"

int psx_opt_sim60 = 0;

namespace {

constexpr uint32_t kGp = 0x800FEEBC;
constexpr uint32_t kHalfTicks = kGp + 0x438;   // 60 Hz counter; odd = no world update
constexpr uint32_t kHalfTicks2 = kGp + 0x4D0;  // second counter the tick advances with it
constexpr uint32_t kPlayerList = 0x800FF5A0;
constexpr uint32_t kCameraList = 0x800FF444;
constexpr uint32_t kOtherLists[] = {0x800FF4E8, 0x800FF5DC};  // enemies, items, projectiles...
constexpr uint32_t kRender = 0x800307F0;

constexpr uint32_t kTick = 0x8002FF90;
constexpr uint32_t kUpdateList = 0x80063770;     // update every object of a list
constexpr uint32_t kContactsBegin = 0x80063CC4;  // per-update contact flags around the player
constexpr uint32_t kContactsEnd = 0x80063CF0;
constexpr uint32_t kUpdateCameras = 0x80037258;  // update one small-object list (a0 = head)

// Player object fields (vectors are x, y, z).
constexpr int kPlayerVecs[] = {0x04, 0x68, 0x98};  // position, world velocity, local velocity
constexpr int kPlayerCounters[] = {0x018, 0x134, 0x136, 0x260};  // age, animation (2 halves), speed step
constexpr int kCameraVecs[] = {0x18, 0x48, 0x54, 0x60};

struct Vec3 {
    int32_t v[3];
};

bool g_in_tick = false;
bool g_in_render = false;

// Positions of the other objects before the last world update (for the render after it).
struct ObjPos {
    uint32_t obj;
    int32_t before[3], after[3];
};
std::vector<ObjPos> g_moved;
bool g_mid_pending = false;

void call(R3000* c, uint32_t fn, uint32_t a0 = 0) {
    c->r[4] = a0;
    call_indirect(c, fn);
}

bool valid_ptr(uint32_t a) { return a && (a & 0x1FFFFFFF) < 0x200000 && !(a & 3); }

Vec3 rd(R3000* c, uint32_t a) {
    return {{(int32_t)MEM_R32(c, a), (int32_t)MEM_R32(c, a + 4), (int32_t)MEM_R32(c, a + 8)}};
}

// Everything the retiming needs from before an update.
struct Before {
    uint32_t player = 0;
    Vec3 pv[std::size(kPlayerVecs)];
    uint16_t counters[std::size(kPlayerCounters)];
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
    }
    uint32_t cam = MEM_R32(c, kCameraList);
    for (; valid_ptr(cam) && b.ncams < 8; cam = MEM_R32(c, cam + 4)) {
        b.cams[b.ncams] = cam;
        for (size_t i = 0; i < std::size(kCameraVecs); i++) b.cv[b.ncams][i] = rd(c, cam + kCameraVecs[i]);
        b.ncams++;
    }
    return b;
}

// Moves a vector back to halfway between its old and new value. Jumps larger than any
// real motion (teleports, respawns, a reused object) are left alone.
void halve(R3000* c, uint32_t addr, const Vec3& old) {
    Vec3 now = rd(c, addr);
    for (int k = 0; k < 3; k++) {
        int64_t d = (int64_t)now.v[k] - old.v[k];
        if (d > 0x400000 || d < -0x400000) return;
    }
    for (int k = 0; k < 3; k++) MEM_W32(c, addr + 4 * k, (uint32_t)(old.v[k] + (now.v[k] - old.v[k]) / 2));
}

void retime(R3000* c, const Before& b, bool odd) {
    uint32_t p = MEM_R32(c, kPlayerList);
    if (valid_ptr(p) && p == b.player) {
        for (size_t i = 0; i < std::size(kPlayerVecs); i++) halve(c, p + kPlayerVecs[i], b.pv[i]);
        if (odd) {
            for (size_t i = 0; i < std::size(kPlayerCounters); i++) {
                uint32_t a = p + kPlayerCounters[i];
                if ((uint16_t)(MEM_R16(c, a) - b.counters[i]) == 1) MEM_W16(c, a, b.counters[i]);
            }
        }
    }
    uint32_t cam = MEM_R32(c, kCameraList);
    for (int n = 0; valid_ptr(cam) && n < 8; n++, cam = MEM_R32(c, cam + 4)) {
        for (int j = 0; j < b.ncams; j++) {
            if (b.cams[j] != cam) continue;
            for (size_t i = 0; i < std::size(kCameraVecs); i++) halve(c, cam + kCameraVecs[i], b.cv[j][i]);
        }
    }
}

// APOC_SIM60_TRACE=1: log the player's position at every tick (for measuring speeds).
void trace_player(R3000* c, uint32_t t) {
    static const bool on = std::getenv("APOC_SIM60_TRACE") != nullptr;
    uint32_t p = on ? MEM_R32(c, kPlayerList) : 0;
    if (p)
        log_msg(LogCat::Host, "sim60 f=%llu tick=%u player %d %d %d", (unsigned long long)host_frame_count(), t,
                (int32_t)MEM_R32(c, p + 4), (int32_t)MEM_R32(c, p + 8), (int32_t)MEM_R32(c, p + 12));
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
        }
    }
}

}  // namespace

// The level's render (called from smooth.cpp's render hook when psx_opt_sim60 is on).
int sim60_render(R3000* c) {
    if (g_in_render || !g_mid_pending) return 0;
    g_mid_pending = false;
    std::vector<const ObjPos*> set;
    for (const ObjPos& op : g_moved) {
        if (rd(c, op.obj + 4).v[0] != op.after[0]) continue;  // moved again or gone
        for (int k = 0; k < 3; k++) MEM_W32(c, op.obj + 4 + 4 * k, (uint32_t)(op.before[k] + (op.after[k] - op.before[k]) / 2));
        set.push_back(&op);
    }
    const uint32_t ra = c->r[31], sp = c->r[29];
    g_in_render = true;
    call(c, kRender);
    g_in_render = false;
    c->r[31] = ra;
    c->r[29] = sp;
    for (const ObjPos* op : set)
        for (int k = 0; k < 3; k++) MEM_W32(c, op->obj + 4 + 4 * k, (uint32_t)op->after[k]);
    return 1;
}

extern "C" int psx_hook_tick(R3000* c) {
    if (g_in_tick) return 0;  // the game's own tick, run from below
    uint32_t t = MEM_R32(c, kHalfTicks);
    trace_player(c, t);
    if (!psx_opt_sim60) return 0;

    const uint32_t ra = c->r[31], sp = c->r[29];
    const Before before = capture(c);
    if (!(t & 1)) {
        // even: the game's full update (it advances the counters itself)
        std::vector<ObjPos> objs;
        for (uint32_t head : kOtherLists) {
            uint32_t o = MEM_R32(c, head);
            for (int n = 0; valid_ptr(o) && n < 4096; n++, o = MEM_R32(c, o + 0x1C)) {
                Vec3 p = rd(c, o + 4);
                objs.push_back({o, {p.v[0], p.v[1], p.v[2]}, {}});
            }
        }
        g_in_tick = true;
        call(c, kTick);
        g_in_tick = false;
        g_moved.clear();
        for (ObjPos& op : objs) {
            Vec3 p = rd(c, op.obj + 4);
            bool moved = false, near = true;
            for (int k = 0; k < 3; k++) {
                op.after[k] = p.v[k];
                int64_t d = (int64_t)p.v[k] - op.before[k];
                if (d) moved = true;
                if (d > 0x400000 || d < -0x400000) near = false;
            }
            if (moved && near) g_moved.push_back(op);
        }
        g_mid_pending = true;
    } else {
        // odd: the player and the cameras
        MEM_W32(c, kHalfTicks, t + 1);
        MEM_W32(c, kHalfTicks2, MEM_R32(c, kHalfTicks2) + 1);
        call(c, kContactsBegin);
        call(c, kUpdateList, kPlayerList);
        call(c, kContactsEnd);
        call(c, kUpdateCameras, MEM_R32(c, kCameraList));
    }
    retime(c, before, t & 1);
    c->r[31] = ra;
    c->r[29] = sp;
    return 1;
}
