#include "host/autodetect.h"

#include <algorithm>
#include <cmath>

namespace {

// First row that matches wins. The scale is 240 lines per step up to the screen's height, at most
// `max_scale`; `hi_fps` lets the frame rate follow the monitor above 60, with `aa_hi` there (every
// in-between image is a full render: supersampling only on cards with room for it).
struct Rule {
    bool (*when)(const HwProfile&);
    int max_scale, aa, msaa;
    bool hi_fps;
    int aa_hi;
};
const Rule kRules[] = {
    {[](const HwProfile& h) { return h.deck_like; }, 4, 1, 1, false, 1},
    {[](const HwProfile& h) { return !h.vulkan || !h.discrete; }, 5, 1, 1, false, 1},
    {[](const HwProfile& h) { return h.vram_mb < 4096; }, 6, 1, 2, true, 1},
    {[](const HwProfile& h) { return h.vram_mb < 8192; }, 9, 3, 4, true, 1},
    {[](const HwProfile&) { return true; }, 9, 3, 8, true, 3},  // the MSAA budget lowers it if it does not fit
};

const struct { Aspect aspect; float ratio; } kAspects[] = {
    {Aspect::A4_3, 4.0f / 3}, {Aspect::A16_10, 1.6f}, {Aspect::A16_9, 16.0f / 9},
    {Aspect::A21_9, 21.0f / 9}, {Aspect::A32_9, 32.0f / 9},
};

}  // namespace

AutoSettings autodetect_choose(const HwProfile& hw) {
    const Rule* r = kRules;
    while (!r->when(hw)) r++;
    AutoSettings s{};
    s.scale = std::min((hw.desktop_h + 239) / 240, r->max_scale);
    s.msaa = r->msaa;
    s.fps = 60;
    if (r->hi_fps)
        for (int f : {75, 90, 120})
            if (hw.refresh_hz >= f) s.fps = f;
    s.aa = s.fps > 60 ? r->aa_hi : r->aa;
    const float want = (float)hw.desktop_w / (float)hw.desktop_h;
    s.aspect = Aspect::A16_9;
    float best = 1e9f;
    for (const auto& a : kAspects)
        if (std::fabs(a.ratio - want) < best) best = std::fabs(a.ratio - want), s.aspect = a.aspect;
    return s;
}
