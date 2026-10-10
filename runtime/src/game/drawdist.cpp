// Draw distance (Recomp Enhancements -> Draw distance).
//
// Each level sets one view distance with 8007D76C(near, fog start, fog length) (from its
// script; the boot default is 10, 6000, 2048). From that the function derives, in RAM:
//   800FFAE8 near plane            800FFAEC far plane = start + length
//   800FFACC fog start             800FFAD4 fog shift (length << shift ~ 4096)
//   800FFAC8 start + length        800FFAD8 the same along the screen corner rays
// The far plane culls whole objects (level pieces included) against their bounding boxes;
// the fog darkens each polygon from the fog start to full strength at the far plane, so
// objects vanish just as they turn fully fogged. Sprites and particles are drawn up to
// 800FFAD8. Scaling start and length together pushes all of that further out.
//
// Kept at the original distance: 800FFAC8, which shots compare their distance to the
// camera with to remove themselves (gameplay). That also keeps the game's own values
// recoverable from RAM (start = fog start * 800FFAC8 / far plane), so each render re-derives
// them and re-runs 8007D76C when the wanted distance differs: level loads, save states and
// setting changes all take effect on the next frame.
//
// Limit: both ordering tables index by z & 0x3FFC (4096 slots, z up to 16383). Meshes farther
// than that all share the farthest slot (the game clamps them there) and are drawn in the order
// submitted, so a far building covered nearer ones (rooftops: the game's own far plane is
// 16024, and at 400% big slabs and streaks showed over the skyline). The far plane stops at
// 16380, as the sprites' limit does.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/psx.h"
#include "game/game.h"

namespace {

constexpr uint32_t kSetViewFunc = 0x8007D76C;
constexpr uint32_t kViewDist = 0x800FFAC8;   // start + length, as the game set it (shots use it)
constexpr uint32_t kFogStart = 0x800FFACC;
constexpr uint32_t kSpriteFar = 0x800FFAD8;  // sprite/particle far limit
constexpr uint32_t kNear = 0x800FFAE8;
constexpr uint32_t kFar = 0x800FFAEC;
constexpr int32_t kMaxFar = 16380;  // the ordering tables' depth range (see above)

int32_t rd32(uint32_t a) { int32_t v; std::memcpy(&v, ram_ptr(a), 4); return v; }
void wr32(uint32_t a, int32_t v) { std::memcpy(ram_ptr(a), &v, 4); }

}  // namespace

// Setting: percent of the game's own view distance (100 = original).
int g_draw_distance = 400;

void drawdist_apply(R3000* c) {
    const int32_t orig_far = rd32(kViewDist), far_z = rd32(kFar), start = rd32(kFogStart);
    if (orig_far <= 0 || far_z <= 0 || start < 0 || start > far_z) return;  // not set up yet
    // Max (400 and up): the whole depth range in every level, however short the level's own is
    const int64_t scaled = g_draw_distance >= 400 ? kMaxFar : (int64_t)orig_far * g_draw_distance / 100;
    const int32_t want = std::max(orig_far, (int32_t)std::min<int64_t>(scaled, kMaxFar));
    if (want == far_z) return;
    const int32_t orig_start = (int32_t)((int64_t)start * orig_far / far_z);
    const int32_t new_start = (int32_t)((int64_t)orig_start * want / orig_far);
    LOG(Host, "draw distance: game %d (fog from %d), drawing to %d", orig_far, orig_start, want);

    R3000 saved = *c;
    c->r[4] = (uint32_t)rd32(kNear);
    c->r[5] = (uint32_t)new_start;
    c->r[6] = (uint32_t)(want - new_start);
    call_indirect(c, kSetViewFunc);
    const uint64_t cycles = c->cycles, next = c->next_event;
    *c = saved;
    c->cycles = cycles;
    c->next_event = next;

    wr32(kViewDist, orig_far);
    wr32(kSpriteFar, std::min(rd32(kSpriteFar), 16380));
}

// Widescreen: 8007E63C(camera, view 800A67B8, ...) builds the culling planes 800F25D0 (8
// bytes each: normal x, y, z, distance) from the view: +0 right edge, +4 left edge, +C centre
// x, +10 projection distance. In 16:9 whole objects at the sides were culled as if the
// screen were still 4:3. The view itself cannot be widened (the same call sets up drawing
// from it), so after it runs the four side planes are rebuilt with its own formulas from
// edges moved out by the widescreen factor, and its rotation of planes 0-5 into camera
// space (800F3E30 / 800F3E50, 8008784C per plane) is redone.
namespace {

int32_t isqrt(int64_t v) {
    int64_t r = (int64_t)std::sqrt((double)std::max<int64_t>(v, 0));
    while (r * r > v) r--;
    while ((r + 1) * (r + 1) <= v) r++;
    return (int32_t)r;
}

void put_plane(int i, int32_t x, int32_t z) {
    const int16_t p[4] = {(int16_t)x, 0, (int16_t)z, 0};
    std::memcpy(ram_ptr(0x800F25D0 + 8 * i), p, 8);
}

}  // namespace

extern "C" int psx_hook_frustum(R3000* c) {
    static bool inside = false;
    if (inside || g_gte_widescreen >= 1.0f) return 0;
    const uint32_t camera = c->r[4], view = c->r[5];
    const uint32_t ra = c->r[31], sp = c->r[29];
    inside = true;
    call_indirect(c, 0x8007E63Cu);
    inside = false;
    c->r[31] = ra;
    c->r[29] = sp;

    uint16_t right, left, cx, h;
    std::memcpy(&right, ram_ptr(view + 0x0), 2);
    std::memcpy(&left, ram_ptr(view + 0x4), 2);
    std::memcpy(&cx, ram_ptr(view + 0xC), 2);
    std::memcpy(&h, ram_ptr(view + 0x10), 2);
    if (left > cx || right < cx || !h) return 1;
    const int64_t dl = std::lround((cx - left) / g_gte_widescreen), dr = std::lround((right - cx) / g_gte_widescreen);
    const int64_t H = h;
    int32_t n = isqrt(25 * dl * dl + 64 * H * H);
    put_plane(4, (int32_t)((H << 15) / n), (int32_t)((dl * 5 << 12) / n));
    n = isqrt(25 * dr * dr + 64 * H * H);
    put_plane(5, (int32_t)(-(H << 15) / n), (int32_t)((dr * 5 << 12) / n));
    n = isqrt(dl * dl + H * H);
    put_plane(6, (int32_t)((H << 12) / n), (int32_t)((dl << 12) / n));
    n = isqrt(dr * dr + H * H);
    put_plane(7, (int32_t)(-(H << 12) / n), (int32_t)((dr << 12) / n));

    R3000 saved = *c;  // 8008784C loads the camera matrix into the GTE
    for (int i = 0; i < 3; i++) {
        for (int set = 0; set < 2; set++) {
            c->r[4] = camera + 0x34;
            c->r[5] = 0x800F25D0 + 8 * (i + 3 * set);
            c->r[6] = (set ? 0x800F3E50 : 0x800F3E30) + 6 * i;
            call_indirect(c, 0x8008784Cu);
        }
    }
    const uint64_t cycles = c->cycles, next = c->next_event;
    *c = saved;
    c->cycles = cycles;
    c->next_event = next;
    return 1;
}
