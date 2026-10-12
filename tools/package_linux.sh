#!/bin/sh
# A Linux build that runs on other PCs: dist/linux/repocalypse. Run it on an
# old distro (Ubuntu 22.04: glibc 2.35, so most distros since 2022 can run it).
# FFmpeg (movie formats only) and the C++ runtime are built in; the rest (OpenGL / Vulkan,
# X11 / Wayland, sound) comes from the system as with any game.
#   needs, besides build.sh's packages: nasm make curl
# Then copy assets/ (image, replacements) next to dist/linux/repocalypse.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
FF=$ROOT/build-linux-dist/ffmpeg

if [ ! -f "$FF/lib/pkgconfig/libavformat.pc" ]; then
    echo "[ffmpeg] building a static FFmpeg (replacement movie formats only)..."
    mkdir -p "$FF/src"
    curl -sL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz | tar xJ -C "$FF/src" --strip-components=1
    cd "$FF/src"
    ./configure --prefix="$FF" --cc="${CC:-cc}" --enable-static --disable-shared --enable-pic \
        --disable-programs --disable-doc --disable-network --disable-autodetect --disable-avdevice \
        --disable-avfilter --disable-everything --enable-protocol=file \
        --enable-demuxer=matroska,mov,avi,asf \
        --enable-decoder=h264,hevc,vp8,vp9,mpeg4,mpeg2video,msmpeg4v3,wmv1,wmv2,wmv3,vc1,aac,mp3,opus,vorbis,flac,ac3,eac3,wmav2,pcm_s16le,pcm_s24le,pcm_f32le \
        --enable-parser=h264,hevc,vp8,vp9,mpeg4video,mpegvideo,vc1,aac,mpegaudio,opus,vorbis,flac,ac3 >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
    cd "$ROOT"
fi

PKG_CONFIG_PATH=$FF/lib/pkgconfig LDFLAGS="-static-libstdc++ -static-libgcc" BUILD_DIR=build-linux-dist ./build.sh

mkdir -p dist/linux
cp repocalypse dist/linux/
strip dist/linux/repocalypse
# the menu font, for systems without DejaVu at the usual places
for f in /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /usr/share/fonts/TTF/DejaVuSans.ttf; do
    [ -f "$f" ] && mkdir -p dist/linux/assets && cp "$f" dist/linux/assets/font.ttf && break
done
echo "glibc needed: $(objdump -T dist/linux/repocalypse 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
echo "Done: dist/linux (add assets/image, assets/replacements)"
