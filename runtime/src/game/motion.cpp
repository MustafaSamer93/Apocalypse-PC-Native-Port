// Frame interpolation, the game's half (above 60 fps; the in-between images are drawn by the
// renderer, hw_replay, and shown by host.cpp): where each exact vertex of this frame was in the
// previous one.
//
// - A model part the GTE projected with its own translation (characters, enemies, props: one
//   translation and rotation per jointed part) is paired with the same part last frame: drawn by
//   the same code (return address), in about the same order, at about the same place. Its vertex
//   then goes through last frame's rotation and translation: where that very vertex was.
// - Everything else that has an exact position (the level, drawn camera-relative with no
//   translation; Native 3D's effects; polygons cut at the near plane) is taken as standing still
//   in the world: only the camera moved, so it is projected again with last frame's camera.
// A camera cut (a jump or a turn too large for one frame) has no motion at all.
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/psx.h"
#include "game/native3d.h"

bool g_motion_on = false;

namespace {

struct Seg {
    int16_t rt[9];  // rotation (4.12)
    int32_t tr[3];  // translation (view units)
    uint32_t ra;    // the code that drew it
};
constexpr uint32_t kLookAhead = 48;
std::vector<Seg> g_prev, g_cur;  // last frame's parts, this frame's
uint32_t g_next = 0;             // where the next part is expected in last frame's order
const Seg* g_pair = nullptr;     // the current part's pair (nullptr: none, or not a part)

struct Key {
    uint32_t x, y, w;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && w == o.w; }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        return (size_t)(((uint64_t)k.x * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)k.y * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t)k.w);
    }
};
std::unordered_map<Key, MotionPrev, KeyHash> g_map;  // exact position this frame -> last frame's
Key key_of(float x, float y, float w) {
    Key k;
    std::memcpy(&k.x, &x, 4), std::memcpy(&k.y, &y, 4), std::memcpy(&k.w, &w, 4);
    return k;
}

// The camera: this frame's (taken at the level renderer's first call) and last frame's.
n3d::View g_view, g_view_prev;
bool g_have_view = false, g_have_prev = false;
bool g_reproject = false;  // both known and no cut: d = D p + e takes a view point to last frame's
bool g_cut = false;        // the camera cut: nothing moves
double g_D[9], g_e[3];

void inverse3(const double* m, double* r) {
    const double a = m[4] * m[8] - m[5] * m[7], b = m[5] * m[6] - m[3] * m[8], c = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * a + m[1] * b + m[2] * c;
    const double k = std::abs(det) > 1e-9 ? 1.0 / det : 0.0;
    r[0] = a * k, r[1] = (m[2] * m[7] - m[1] * m[8]) * k, r[2] = (m[1] * m[5] - m[2] * m[4]) * k;
    r[3] = b * k, r[4] = (m[0] * m[8] - m[2] * m[6]) * k, r[5] = (m[2] * m[3] - m[0] * m[5]) * k;
    r[6] = c * k, r[7] = (m[1] * m[6] - m[0] * m[7]) * k, r[8] = (m[0] * m[4] - m[1] * m[3]) * k;
}

// The reprojection for the world standing still: view (this frame) -> world -> view (last frame).
// p = M (world - cam) / 4096, so p_prev = M' M^-1 p + M' (cam - cam') / 4096.
void reprojection() {
    g_reproject = false;
    if (!g_have_view || !g_have_prev) return;
    double inv[9];
    inverse3(g_view.m, inv);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += g_view_prev.m[3 * i + k] * inv[3 * k + j];
            g_D[3 * i + j] = s;
        }
    double dc[3];
    for (int i = 0; i < 3; i++) dc[i] = g_view.cam[i] - g_view_prev.cam[i];
    for (int i = 0; i < 3; i++) g_e[i] = (g_view_prev.m[3 * i] * dc[0] + g_view_prev.m[3 * i + 1] * dc[1] + g_view_prev.m[3 * i + 2] * dc[2]) / 4096.0;
    // a cut: the camera jumped (more than a few steps' run) or turned more than ~25 degrees
    const double jump = std::abs(dc[0]) + std::abs(dc[1]) + std::abs(dc[2]);
    bool cut = jump > 3000;
    for (int i = 0; i < 9 && !cut; i++) cut = std::abs(g_D[i] - (i % 4 == 0 ? 1.0 : 0.0)) > 0.45;
    g_reproject = !cut;
    g_cut = cut;
}

// The player's spot on the screen, for mouse aim (host/input.cpp): taken with the frame's camera.
constexpr uint32_t kPlayerList = 0x800FF5A0;
constexpr double kChest = 48;  // whole units above the player's origin (its waist): about where it fires from
bool g_player_seen = false;      // this frame's is taken
bool g_player_ok = false;
float g_player_sx = 0, g_player_sy = 0;

}  // namespace

bool player_screen(float* sx, float* sy) {
    *sx = g_player_sx, *sy = g_player_sy;
    return g_player_ok;
}

void motion_frame_begin() {
    g_player_seen = false;
    g_prev.swap(g_cur);
    g_cur.clear();
    g_next = 0;
    g_pair = nullptr;
    g_map.clear();
    g_view_prev = g_view;
    g_have_prev = g_have_view;
    g_have_view = false;
    g_reproject = g_cut = false;
}

void motion_camera(R3000* c) {
    if (!g_player_seen && !core_in_ghost() && g_gte_blend != GteBlendMode::Record) {
        g_player_seen = true;
        const uint32_t p = n3d::rd(c, kPlayerList);
        g_player_ok = false;
        if (p && (p & 0x1FFFFF) < 0x200000) {
            const n3d::View v = n3d::view_now(c);
            const n3d::Pt pt = v.see((int32_t)n3d::rd(c, p + 4) / 4096.0, (int32_t)n3d::rd(c, p + 8) / 4096.0 - kChest,
                                     (int32_t)n3d::rd(c, p + 12) / 4096.0);
            g_player_ok = pt.z > 0;
            g_player_sx = pt.sx, g_player_sy = pt.sy;
        }
    }
    if (!g_motion_on || g_have_view || core_in_ghost() || g_gte_blend == GteBlendMode::Record) return;
    g_view = n3d::view_now(c);
    g_have_view = true;
    reprojection();
}

void motion_segment(const int16_t* rt, const int32_t* tr, uint32_t ra) {
    Seg s;
    std::memcpy(s.rt, rt, sizeof s.rt);
    std::memcpy(s.tr, tr, sizeof s.tr);
    s.ra = ra;
    g_cur.push_back(s);
    g_pair = nullptr;
    const uint32_t e = g_next++;  // where it would be in last frame's order (the next part comes after)
    if (!tr[0] && !tr[1] && !tr[2]) return;  // the level (camera-relative): the world standing still
    // Last frame's part in the same place in the drawing order (a model's parts come in a fixed
    // order, close together: the nearest one could be the neighbouring joint), drawn by the same
    // code and within a step's move plus what a turning camera adds far away; else, when parts came
    // or went before it, the nearest such one around there.
    const int64_t mag = std::abs((int64_t)tr[0]) + std::abs((int64_t)tr[1]) + std::abs((int64_t)tr[2]);
    const int64_t limit = 256 + mag / 6;
    auto dist = [&](const Seg& p) {
        return std::abs((int64_t)p.tr[0] - tr[0]) + std::abs((int64_t)p.tr[1] - tr[1]) + std::abs((int64_t)p.tr[2] - tr[2]);
    };
    if (e < g_prev.size() && g_prev[e].ra == ra && dist(g_prev[e]) <= limit) {
        g_pair = &g_prev[e];
    } else {
        int64_t best = limit + 1;
        const uint32_t begin = e > kLookAhead / 2 ? e - kLookAhead / 2 : 0;
        const uint32_t end = std::min<uint32_t>((uint32_t)g_prev.size(), e + kLookAhead);
        for (uint32_t j = begin; j < end; j++)
            if (g_prev[j].ra == ra && dist(g_prev[j]) < best) best = dist(g_prev[j]), g_pair = &g_prev[j];
        if (g_pair) g_next = (uint32_t)(g_pair - g_prev.data()) + 1;
    }
}

void motion_vertex(int16_t vx, int16_t vy, int16_t vz, float px, float py, float pw, const int16_t* rt, const int32_t* tr,
                   float h) {
    if (!g_pair) return;
    auto view = [&](const int16_t* m, const int32_t* t, double* o) {
        for (int i = 0; i < 3; i++) o[i] = ((double)m[3 * i] * vx + (double)m[3 * i + 1] * vy + (double)m[3 * i + 2] * vz) / 4096.0 + t[i];
    };
    double now[3], was[3];
    view(rt, tr, now);
    view(g_pair->rt, g_pair->tr, was);
    if (now[2] < 1 || was[2] < 1) return;
    const double ws = g_gte_widescreen;
    // moved by the difference of the two projections (pw, px, py may be a blend of poses)
    const double dx = h * ws * (was[0] / was[2] - now[0] / now[2]), dy = h * (was[1] / was[2] - now[1] / now[2]);
    if (std::abs(dx) > 200 || std::abs(dy) > 200) return;  // not the same part after all
    g_map[key_of(px, py, pw)] = MotionPrev{(float)(px + dx), (float)(py + dy), (float)(pw * was[2] / now[2])};
}

bool motion_prev(float x, float y, float w, MotionPrev* out) {
    if (!g_motion_on || w <= 0 || g_cut) return false;
    if (auto it = g_map.find(key_of(x, y, w)); it != g_map.end()) {
        *out = it->second;
        return true;
    }
    if (!g_reproject) return false;
    // the world standing still: back to view space, into last frame's view, projected again
    const n3d::View& v = g_view;
    const double p[3] = {(x - v.ofx) * (double)w / (v.h * v.ws), (y - v.ofy) * (double)w / v.h, (double)w};
    double q[3];
    for (int i = 0; i < 3; i++) q[i] = g_D[3 * i] * p[0] + g_D[3 * i + 1] * p[1] + g_D[3 * i + 2] * p[2] + g_e[i];
    if (q[2] < 1) return false;
    const n3d::View& o = g_view_prev;
    const double sx = o.ofx + o.h * o.ws * q[0] / q[2], sy = o.ofy + o.h * q[1] / q[2];
    if (std::abs(sx - x) > 200 || std::abs(sy - y) > 200) return false;
    *out = MotionPrev{(float)sx, (float)sy, (float)q[2]};
    return true;
}
