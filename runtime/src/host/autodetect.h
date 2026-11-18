// First-boot hardware auto-detect: the settings a new install starts with.
#pragma once

#include <cstdint>

#include "render/present.h"

// What the machine is, filled in once at start-up (host.cpp). The rules below only read this.
struct HwProfile {
    bool linux = false;
    bool deck_like = false;   // Steam Deck: Linux on a 1280x800 screen, or its AMD APU by name
    bool vulkan = false;      // the Vulkan renderer started (else OpenGL: nothing is known about the GPU)
    bool discrete = false;    // a discrete GPU (not integrated)
    uint64_t vram_mb = 0;
    int desktop_w = 1920, desktop_h = 1080;
    int refresh_hz = 60;
};

struct AutoSettings {
    int scale, aa, msaa, fps;
    Aspect aspect;
};

AutoSettings autodetect_choose(const HwProfile& hw);  // pure: the same machine gets the same settings
