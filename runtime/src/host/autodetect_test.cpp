// Self-check for autodetect_choose. Build and run (SDL headers only for present.h's Aspect):
//   clang-cl /std:c++20 /EHsc /Iruntime\src /I<SDL3 include dir> runtime\src\host\autodetect_test.cpp runtime\src\host\autodetect.cpp && autodetect_test.exe
#include <cassert>
#include <cstdio>

#include "host/autodetect.h"

int main() {
    HwProfile deck{.linux = true, .deck_like = true, .vulkan = true, .vram_mb = 1024, .desktop_w = 1280, .desktop_h = 800, .refresh_hz = 90};
    AutoSettings s = autodetect_choose(deck);
    assert(s.scale == 4 && s.aa == 1 && s.msaa == 1 && s.fps == 60 && s.aspect == Aspect::A16_10);

    HwProfile igpu{.vulkan = true, .vram_mb = 2048, .desktop_w = 2560, .desktop_h = 1440, .refresh_hz = 144};
    s = autodetect_choose(igpu);
    assert(s.scale == 5 && s.aa == 1 && s.msaa == 1 && s.fps == 60 && s.aspect == Aspect::A16_9);

    HwProfile mid{.vulkan = true, .discrete = true, .vram_mb = 6144, .desktop_w = 2560, .desktop_h = 1440, .refresh_hz = 144};
    s = autodetect_choose(mid);
    assert(s.scale == 6 && s.aa == 1 && s.msaa == 4 && s.fps == 120 && s.aspect == Aspect::A16_9);

    HwProfile big{.vulkan = true, .discrete = true, .vram_mb = 16384, .desktop_w = 3840, .desktop_h = 2160, .refresh_hz = 60};
    s = autodetect_choose(big);
    assert(s.scale == 9 && s.aa == 3 && s.msaa == 8 && s.fps == 60 && s.aspect == Aspect::A16_9);

    HwProfile mine{.vulkan = true, .discrete = true, .vram_mb = 16112, .desktop_w = 1920, .desktop_h = 1080, .refresh_hz = 75};
    s = autodetect_choose(mine);
    assert(s.scale == 5 && s.aa == 3 && s.msaa == 8 && s.fps == 75 && s.aspect == Aspect::A16_9);

    std::puts("autodetect ok");
}
