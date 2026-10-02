#!/bin/sh
# Builds the native Linux version of Apocalypse from your own disc image.
#   1. put your disc image (.cue and .bin files, any names, or just the .bin files) in "assets/image"
#   2. ./build.sh
#   3. ./repocalypse (it is made in this folder)
# Needs: cmake, ninja, clang (or gcc), python3, git, and SDL3's build dependencies, e.g. on
# Debian / Ubuntu:
#   sudo apt install cmake ninja-build clang python3 git libgl-dev libx11-dev libxext-dev \
#     libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libwayland-dev \
#     libxkbcommon-dev wayland-protocols libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
#     pkg-config libavformat-dev libavcodec-dev libswscale-dev libswresample-dev  # (replacement movies)
set -e
cd "$(dirname "$0")"
BUILD=${BUILD_DIR:-build-linux}

if ! ls assets/image/*.cue assets/image/*.bin >/dev/null 2>&1; then
    echo 'Put your disc image (.cue/.bin) in "assets/image" first'; exit 1
fi

echo "[1/4] Extracting the executable from the disc..."
python3 tools/discfs.py extract >/dev/null

echo "[2/4] Recompiling MIPS code to C..."
python3 tools/recomp/main.py --no-ra-check

echo "[3/4] Configuring..."
CC=${CC:-$(command -v clang || command -v gcc)}
CXX=${CXX:-$(command -v clang++ || command -v g++)}
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRECOMP_RA_CHECK=OFF \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" >/dev/null

echo "[4/4] Building..."
cmake --build "$BUILD"

echo
echo "Done: ./repocalypse"
