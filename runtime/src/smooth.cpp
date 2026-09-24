// Smooth frame rate for a game whose simulation runs at a fixed 30 Hz.
//
// Apocalypse's level loop (80030AD4) advances the world once per rendered frame with
// fixed-size steps; there is no time delta to scale. The engine does, however, know how
// to draw a moment *between* two steps: when the PS1 fell behind, its render (800307F0)
// drew every object at 1/4 previous + 3/4 current position (80030168, and 800303F4 for
// the cameras) and then put the real positions back (80030390 / 80030764).
//
// This module generalises that: at the entry of each real render, with the positions of
// the last step still remembered, it runs the render again for j/k of the way from the
// last step to this one (j = 1..k-1), draws the resulting ordering table, and keeps the
// image. Each extra render runs in "ghost mode" (core.cpp): outside emulated time, with
// no interrupts, and with memory, scheduler, kernel and GPU state rolled back afterwards,
// so the game never sees it. The presenter then shows the real image followed by the k-1
// extra images, spread over the time until the game's next frame.
//
// Because the PS1 already shows frame N-1 while N is being drawn, the extra images (from
// N-1 towards N) add no latency: they show part of state N earlier than the original.
#include "smooth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <unordered_map>
#include <vector>

#include "gpu_hw.h"
#include "psx.h"
#include "state_io.h"

void bios_ghost_state(StateIO& s);
void hw_state(StateIO& s);
void gpu_regs_state(StateIO& s);
bool gpu_idle();

namespace {

// Game addresses (NTSC-U SLUS-00373; gp = 0x800FEEBC).
constexpr uint32_t kRenderFunc = 0x800307F0;
constexpr uint32_t kObjectLists[] = {0x800FF5A0, 0x800FF4E8, 0x800FF5DC};  // heads; next at +0x1C
constexpr uint32_t kPlayerList = 0x800FF5A0;   // its first object is the player
constexpr uint32_t kCameraList = 0x800FF444;   // head; next at +0x04
constexpr uint32_t kCurBuffer = 0x800FF660;    // gp+0x7A4: frame buffer struct being built
constexpr uint32_t kUpdateCount = 0x800FF2F0;  // gp+0x434: world updates so far
constexpr uint32_t kRenderCount = 0x800FFB8C;  // gp+0xCD0: updates at the last render
constexpr int kObjPos = 0x04;
constexpr int kPlayerExtra = 0xB8;
constexpr int kCameraVecs[] = {0x18, 0x48, 0x54, 0x60};
// Frame buffer struct: DRAWENV at +0, ordering table (4096 entries, cleared reversed) at
// +0x70, so DrawOTag starts at its last entry.
constexpr uint32_t kOtStart = 0x406C;

constexpr int kMaxFactor = 5;
constexpr int kMaxExtra = kMaxFactor - 1;
constexpr uint64_t kCycleBudget = 40'000'000;  // a real render takes ~1M cycles
// Positions are 20.12 fixed point; measured steps span 2^10..2^21 units with nothing at
// 2^22, while teleports and reused objects jump 2^23 and more: those are not blended.
constexpr int32_t kMaxStep = 0x400000;

struct Vec3 {
    int32_t x, y, z;
};

int g_factor = 1;
bool g_failed = false;

// Positions at the last real render.
bool g_have_prev = false;
std::unordered_map<uint32_t, Vec3> g_prev_obj;
uint32_t g_prev_player = 0;
Vec3 g_prev_player_extra{};
std::unordered_map<uint32_t, std::vector<Vec3>> g_prev_cam;

// Capture sets: the back set is filled at the render hook, becomes the front set (shown)
// when the game flips to the frame those images lead up to.
struct CaptureSet {
    int count = 0;   // extra images captured
    int images = 1;  // images for the whole step (the game's own + extras)
    int period = 2;  // emulated vblanks the step is expected to be shown for
};
int g_back = 0, g_front = 1;
CaptureSet g_sets[2];
uint64_t g_flips = 0;
uint64_t g_last_flip = 0;  // host frame of the first present after the last flip
int g_period = 2;          // emulated vblanks between the game's last two frames (2 = 30 fps)

bool valid_ptr(uint32_t a) { return a && (a & 0x1FFFFFFF) < 0x200000 && !(a & 3); }

Vec3 rd(R3000* c, uint32_t a) { return {(int32_t)MEM_R32(c, a), (int32_t)MEM_R32(c, a + 4), (int32_t)MEM_R32(c, a + 8)}; }
void wr(R3000* c, uint32_t a, const Vec3& v) {
    MEM_W32(c, a, (uint32_t)v.x);
    MEM_W32(c, a + 4, (uint32_t)v.y);
    MEM_W32(c, a + 8, (uint32_t)v.z);
}

bool near(const Vec3& a, const Vec3& b) {
    return std::abs(a.x - b.x) <= kMaxStep && std::abs(a.y - b.y) <= kMaxStep && std::abs(a.z - b.z) <= kMaxStep;
}

Vec3 blend(const Vec3& p, const Vec3& q, int j, int k) {
    auto f = [&](int32_t a, int32_t b) { return (int32_t)(a + (int64_t)(b - a) * j / k); };
    return {f(p.x, q.x), f(p.y, q.y), f(p.z, q.z)};
}

// Visits every object of the interpolated lists (guarding against broken links).
template <class F>
void for_each_object(R3000* c, F f) {
    for (uint32_t head : kObjectLists) {
        uint32_t o = MEM_R32(c, head);
        for (int n = 0; valid_ptr(o) && n < 4096; n++, o = MEM_R32(c, o + 0x1C)) f(o, head);
    }
}

template <class F>
void for_each_camera(R3000* c, F f) {
    uint32_t cam = MEM_R32(c, kCameraList);
    for (int n = 0; valid_ptr(cam) && n < 64; n++, cam = MEM_R32(c, cam + 0x04)) f(cam);
}

void record_prev(R3000* c) {
    g_prev_obj.clear();
    for_each_object(c, [&](uint32_t o, uint32_t) { g_prev_obj[o] = rd(c, o + kObjPos); });
    g_prev_player = MEM_R32(c, kPlayerList);
    if (valid_ptr(g_prev_player)) g_prev_player_extra = rd(c, g_prev_player + kPlayerExtra);
    g_prev_cam.clear();
    for_each_camera(c, [&](uint32_t cam) {
        auto& v = g_prev_cam[cam];
        for (int off : kCameraVecs) v.push_back(rd(c, cam + off));
    });
    g_have_prev = true;
}

// Moves everything j/k of the way from the last render's positions to the current ones.
void apply_blend(R3000* c, int j, int k) {
    for_each_object(c, [&](uint32_t o, uint32_t) {
        auto it = g_prev_obj.find(o);
        if (it == g_prev_obj.end()) return;  // new this step: draw where it is
        Vec3 cur = rd(c, o + kObjPos);
        if (near(it->second, cur)) wr(c, o + kObjPos, blend(it->second, cur, j, k));
    });
    uint32_t player = MEM_R32(c, kPlayerList);
    if (valid_ptr(player) && player == g_prev_player) {
        Vec3 cur = rd(c, player + kPlayerExtra);
        if (near(g_prev_player_extra, cur)) wr(c, player + kPlayerExtra, blend(g_prev_player_extra, cur, j, k));
    }
    for_each_camera(c, [&](uint32_t cam) {
        auto it = g_prev_cam.find(cam);
        if (it == g_prev_cam.end()) return;
        for (size_t i = 0; i < std::size(kCameraVecs); i++) {
            Vec3 cur = rd(c, cam + kCameraVecs[i]);
            if (near(it->second[i], cur)) wr(c, cam + kCameraVecs[i], blend(it->second[i], cur, j, k));
        }
    });
}

// Does what the game's PutDrawEnv + DrawOTag would do for the frame buffer being built,
// straight into the GPU, and keeps the drawn image in capture `slot`.
bool draw_and_capture(R3000* c, int slot) {
    uint32_t buf = MEM_R32(c, kCurBuffer);
    if (!valid_ptr(buf)) {
        log_msg(LogCat::Host, "smooth frames: frame buffer pointer %08X invalid (gp=%08X, [gp+7A4]=%08X)", buf,
                c->r[28], MEM_R32(c, c->r[28] + 0x7A4));
        return false;
    }
    // DRAWENV { RECT clip; short ofs[2]; RECT tw; u_short tpage; u_char dtd, dfe, isbg, r0, g0, b0; }
    int cx = (int16_t)MEM_R16(c, buf + 0), cy = (int16_t)MEM_R16(c, buf + 2);
    int cw = (int16_t)MEM_R16(c, buf + 4), ch = (int16_t)MEM_R16(c, buf + 6);
    int ox = (int16_t)MEM_R16(c, buf + 8), oy = (int16_t)MEM_R16(c, buf + 10);
    int twx = MEM_R16(c, buf + 12), twy = MEM_R16(c, buf + 14), tww = MEM_R16(c, buf + 16), twh = MEM_R16(c, buf + 18);
    uint32_t tpage = MEM_R16(c, buf + 20);
    uint32_t dtd = MEM_R8(c, buf + 22), dfe = MEM_R8(c, buf + 23), isbg = MEM_R8(c, buf + 24);
    uint32_t r0 = MEM_R8(c, buf + 25), g0 = MEM_R8(c, buf + 26), b0 = MEM_R8(c, buf + 27);
    if (cw < 64 || cw > 1024 || ch < 64 || ch > 512 || cx < 0 || cy < 0) {
        log_msg(LogCat::Host, "smooth frames: unexpected draw environment at %08X: %d,%d %dx%d", buf, cx, cy, cw, ch);
        return false;
    }

    hw_stash(cx, cy, cw, ch);
    std::vector<uint16_t> cpu_copy;
    gpu_vram_save(cx, cy, cw, ch, cpu_copy);

    gpu_write_gp0(c, 0xE1000000u | (tpage & 0x7FF) | (dtd ? 1u << 9 : 0) | (dfe ? 1u << 10 : 0));
    uint32_t tw = 0xE2000000u;
    if (tww && twh)  // libgpu get_tw()
        tw |= ((uint32_t)(twy >> 3) << 15) | ((uint32_t)(twx >> 3) << 10) |
              ((((uint32_t)~(twh - 1)) & 0xFF) >> 3) << 5 | ((((uint32_t)~(tww - 1)) & 0xFF) >> 3);
    gpu_write_gp0(c, tw);
    gpu_write_gp0(c, 0xE3000000u | (uint32_t)(cy & 0x3FF) << 10 | (uint32_t)(cx & 0x3FF));
    gpu_write_gp0(c, 0xE4000000u | (uint32_t)((cy + ch - 1) & 0x3FF) << 10 | (uint32_t)((cx + cw - 1) & 0x3FF));
    gpu_write_gp0(c, 0xE5000000u | (uint32_t)(oy & 0x7FF) << 11 | (uint32_t)(ox & 0x7FF));
    if (isbg) {
        gpu_write_gp0(c, 0x02000000u | b0 << 16 | g0 << 8 | r0);
        gpu_write_gp0(c, (uint32_t)cy << 16 | (uint32_t)cx);
        gpu_write_gp0(c, (uint32_t)ch << 16 | (uint32_t)cw);
    }
    gpu_run_list(c, buf + kOtStart);

    hw_capture(slot, cx, cy, cw, ch);
    hw_unstash();
    gpu_vram_restore(cx, cy, cw, ch, cpu_copy);
    return true;
}

// Renders the game j/k of the way between the last step and this one into `slot`.
bool render_extra(R3000* c, int j, int k, int slot) {
    static std::jmp_buf abort;
    const R3000 saved_cpu = *c;
    StateIO snap(false);
    bios_ghost_state(snap);
    hw_state(snap);
    gpu_regs_state(snap);

    volatile bool done = false;
    const uint64_t cycles0 = c->cycles;
    core_ghost_begin(c, kCycleBudget, &abort);
    if (setjmp(abort) != 0) {
        log_msg(LogCat::Host, "smooth frames: render aborted after %llu cycles at pc~ra=%08X",
                (unsigned long long)(c->cycles - cycles0), c->r[31]);
    } else {
        apply_blend(c, j, k);
        MEM_W32(c, kRenderCount, MEM_R32(c, kUpdateCount));  // the render's own blend stays off
        c->r[31] = 0;
        call_indirect(c, kRenderFunc);
        done = draw_and_capture(c, slot);
    }
    int blocked = core_ghost_end(c);
    *c = saved_cpu;
    StateIO back(snap.data());
    bios_ghost_state(back);
    hw_state(back);
    gpu_regs_state(back);

    static bool warned_io = false;
    if (blocked && !warned_io) {
        warned_io = true;
        log_msg(LogCat::Host, "smooth frames: the render touched sound/CD hardware (%d writes ignored)", blocked);
    }
    if (!done) {
        g_failed = true;  // don't keep paying for renders that cannot finish
        log_msg(LogCat::Host, "smooth frames: an extra render did not complete; smooth frames turned off");
    }
    return done;
}

}  // namespace

void smooth_set_factor(int k) {
    k = std::clamp(k, 1, kMaxFactor);
    if (k != g_factor) smooth_reset();
    g_factor = k;
    g_failed = false;
}

int smooth_factor() { return g_failed ? 1 : g_factor; }

int smooth_factor_for_refresh(float hz) { return std::clamp((int)std::lround(hz / 30.0f), 2, kMaxFactor); }

void smooth_reset() {
    g_have_prev = false;
    g_prev_obj.clear();
    g_prev_cam.clear();
    g_prev_player = 0;
    g_sets[0] = g_sets[1] = CaptureSet{};
}

struct Stats { int extras; double ms; } g_st{};

int sim60_render(R3000* c);

extern "C" int psx_hook_render(R3000* c) {
    if (core_in_ghost()) return 0;
    if (psx_opt_sim60) return sim60_render(c);
    const int k = smooth_factor();
    CaptureSet& back = g_sets[g_back];
    // k images per 2 vblanks; a step the game takes longer over gets proportionally more.
    back = CaptureSet{0, std::clamp((int)std::lround(k * g_period / 2.0), 1, kMaxFactor), g_period};
    if (k >= 2 && g_have_prev && hw_active() && gpu_idle()) {
        for (int j = 1; j < back.images; j++) {
            auto t0 = std::chrono::steady_clock::now();
            if (!render_extra(c, j, back.images, g_back * kMaxExtra + j - 1)) break;
            g_st.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            back.count = j;
            if (++g_st.extras % 1800 == 0)
                LOG(Host, "smooth frames: %d extra images, %.2f ms each", g_st.extras, g_st.ms / g_st.extras);
        }
    }
    record_prev(c);
    return 0;
}

uint64_t smooth_flips() { return g_flips; }

void smooth_on_flip() {
    g_flips++;
    uint64_t f = host_frame_count() + 1;
    if (g_last_flip && f > g_last_flip) g_period = (int)std::clamp<uint64_t>(f - g_last_flip, 2, 4);
    g_last_flip = f;
    g_front = g_back;
    g_back ^= 1;
    g_sets[g_back] = CaptureSet{};
}

bool smooth_pick(double t, unsigned* tex, int* tex_w, int* tex_h) {
    const CaptureSet& front = g_sets[g_front];
    if (smooth_factor() < 2 || front.count == 0) return false;
    double since = t - (double)g_last_flip;
    if (since < 0 || since > 30) return false;  // the game stopped drawing frames here
    int i = (int)std::floor(since * front.images / front.period + 1e-6);
    if (i <= 0) return false;       // the game's own image
    i = std::min(i, front.count);   // the game is late: hold the newest extra image
    *tex = hw_capture_texture(g_front * kMaxExtra + i - 1, tex_w, tex_h);
    return *tex != 0;
}
