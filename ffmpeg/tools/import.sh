#!/bin/sh
# Regenerates gesso's FFmpeg build files from an FFmpeg release tarball:
#   ffmpeg/tools/import.sh ffmpeg-9.0.2.tar.xz
# For each target it runs FFmpeg's configure with zig cc, builds once with make to learn which
# sources the trimmed configuration compiles, and copies the generated files and that source list
# into ffmpeg/<target>/. build.zig then compiles the release's sources with those files and no
# configure step. Needs zig, make, nasm and llvm-nm on a Linux host, and libva for configure's
# link checks.
set -eu
tarball=$(realpath "$1")
gesso=$(cd "$(dirname "$0")/../.." && pwd)
tools=$gesso/ffmpeg/tools
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

common="--enable-cross-compile --pkg-config=$tools/pkg-config --ar=zig\ ar --ranlib=zig\ ranlib
 --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug
 --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-network
 --enable-static --disable-shared --enable-decoder=h264,aac --enable-parser=h264,aac"

configure_target() {  # name, then configure arguments
    name=$1; shift
    mkdir "$work/$name"
    tar -xf "$tarball" -C "$work/$name" --strip-components=1
    (cd "$work/$name" && eval ./configure $common '"$@"' > configure.out 2>&1) || { tail -5 "$work/$name/configure.out"; exit 1; }
    # zig's glibc headers lack sys/sysctl.h, though configure's link check finds sysctl().
    case $name in linux-*) sed -i 's/#define HAVE_SYSCTL 1/#define HAVE_SYSCTL 0/' "$work/$name/config.h";; esac
}

build_and_copy() {  # name
    name=$1
    src=$work/$name
    (cd "$src" && make -j"$(nproc)" libavcodec/libavcodec.a libavutil/libavutil.a > make.out 2>&1) || { grep -m5 error "$src/make.out"; exit 1; }
    out=$gesso/ffmpeg/$name
    rm -rf "$out" && mkdir -p "$out/libavcodec" "$out/libavutil"
    # The configuration string records the build host's paths; it is only informational.
    sed 's/^#define FFMPEG_CONFIGURATION .*/#define FFMPEG_CONFIGURATION "gesso"/; s/^#define CC_IDENT .*/#define CC_IDENT "zig cc"/' "$src/config.h" > "$out/config.h"
    cp "$src/config_components.h" "$out/"
    for f in config.asm config_components.asm; do if [ -f "$src/$f" ]; then cp "$src/$f" "$out/"; fi; done
    cp "$src/libavcodec/codec_list.c" "$src/libavcodec/parser_list.c" "$src/libavcodec/bsf_list.c" "$out/libavcodec/"
    cp "$src/libavutil/avconfig.h" "$src/libavutil/ffversion.h" "$out/libavutil/"
    # Each object built maps back to the source beside it: C, NASM or GNU assembler.
    (cd "$src" && find libavcodec libavutil -name '*.o' | sort | while read -r o; do
        for ext in c asm S; do [ -f "${o%.o}.$ext" ] && { echo "${o%.o}.$ext"; break; }; done
    done) > "$out/sources.txt"
}

configure_target linux-x86_64 --arch=x86_64 --target-os=linux --cc="zig cc -target x86_64-linux-gnu.2.27" \
    --nm=nm --x86asmexe=nasm --enable-pic --enable-pthreads --enable-vaapi --enable-hwaccel=h264_vaapi
build_and_copy linux-x86_64

configure_target windows-x86_64 --arch=x86_64 --target-os=mingw32 --cc="$tools/zigcc-windows" \
    --nm=nm --x86asmexe=nasm --enable-w32threads --enable-d3d11va --enable-hwaccel=h264_d3d11va,h264_d3d11va2
build_and_copy windows-x86_64

# VideoToolbox's configure checks need the macOS SDK, which only a Mac has, so the macOS
# configuration is generated without it and VideoToolbox is switched on afterwards, as
# --enable-videotoolbox would with a current SDK.
configure_target macos-aarch64 --arch=aarch64 --target-os=darwin --cc="$tools/zigcc-macos" \
    --nm=llvm-nm --enable-pic --enable-pthreads
build_and_copy macos-aarch64
out=$gesso/ffmpeg/macos-aarch64
sed -i -E 's/#define (CONFIG_VIDEOTOOLBOX|HAVE_KCMVIDEOCODECTYPE_[A-Z0-9_]+|HAVE_KCVPIXELFORMATTYPE_[A-Z0-9_]+|HAVE_KCVIMAGEBUFFER[A-Z0-9_]+) 0/#define \1 1/' "$out/config.h"
sed -i 's/#define CONFIG_H264_VIDEOTOOLBOX_HWACCEL 0/#define CONFIG_H264_VIDEOTOOLBOX_HWACCEL 1/' "$out/config_components.h"
printf '%s\n' libavcodec/videotoolbox.c libavutil/hwcontext_videotoolbox.c >> "$out/sources.txt"

echo "$(basename "$tarball") imported into $gesso/ffmpeg"
