#!/usr/bin/env bash
# build-external-ffmpeg-mac.sh
#
# Build a minimal ffmpeg suitable for thumbnail extraction (decode
# common video formats + the codecs UFB's VideoBackend touches).
#
# One-time setup: ~15 min on Apple Silicon, ~30 min on Intel.
# Re-runs are no-ops; delete external/ffmpeg/ to force rebuild.
#
# Output (per cmake/external.cmake's expectations):
#   external/ffmpeg/include/<libavcodec/...>   headers
#   external/ffmpeg/lib/lib<name>.<MAJOR>.dylib   versioned dylibs
#   external/ffmpeg/lib/lib<name>.dylib            unversioned symlinks
#   external/ffmpeg/bin/{ffmpeg,ffprobe}           transcode CLI tools
#
# The CLI tools are what bindings/src/services/transcode.rs spawns
# as subprocesses. They sit alongside the dylibs they link against
# and, post-build, get copied into UFB.app/Contents/MacOS/ next to
# the main `ufb` Mach-O — so an LC_RPATH of @executable_path/../
# Frameworks resolves the bundled libav* dylibs at runtime.
#
# Re-run as `just mac-setup-external` (slice 09 wires the recipe);
# called transitively from scripts/setup-external-mac.sh.
#
# Borrows the configure flag set from the Tauri repo's
# scripts/build-ffmpeg-macos.sh, which has been hardened against
# Apple Clang's pickier mode and post-Sonoma SDKs.

set -euo pipefail

FFMPEG_VERSION="8.1.2"
DEPLOYMENT_TARGET="13.0"
SOURCE_DIR="/tmp/ufb-ffmpeg-build/ffmpeg-${FFMPEG_VERSION}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="$REPO_ROOT/external/ffmpeg"
X264_PREFIX="/tmp/ufb-ffmpeg-build/x264-prefix"

if [[ -f "$OUTPUT_DIR/include/libavcodec/avcodec.h" \
   && -x "$OUTPUT_DIR/bin/ffmpeg" \
   && -x "$OUTPUT_DIR/bin/ffprobe" ]]; then
    echo "[ffmpeg] already built at $OUTPUT_DIR — delete to rebuild"
    exit 0
fi

if ! command -v nasm >/dev/null 2>&1; then
    echo "[ffmpeg] error: nasm not found. Install via: brew install nasm"
    exit 1
fi
if ! command -v pkg-config >/dev/null 2>&1; then
    echo "[ffmpeg] error: pkg-config not found. Install via: brew install pkg-config"
    exit 1
fi

mkdir -p /tmp/ufb-ffmpeg-build
cd /tmp/ufb-ffmpeg-build

# x264: static-only PIC build linked into libavcodec. Static keeps the
# bundle free of an extra dylib to patch + sign; PIC is required since
# libavcodec itself is shared. The Windows side already ships BtbN's
# GPL build with libx264 — this brings the mac transcode path to
# parity (transcode.rs hardcodes -c:v libx264).
if [[ ! -f "$X264_PREFIX/lib/libx264.a" ]]; then
    if [[ ! -d x264-stable ]]; then
        echo "[x264] downloading stable branch..."
        curl -L "https://code.videolan.org/videolan/x264/-/archive/stable/x264-stable.tar.bz2" \
            -o x264.tar.bz2
        tar xf x264.tar.bz2
    fi
    cd x264-stable
    echo "[x264] configuring..."
    ./configure \
        --prefix="$X264_PREFIX" \
        --enable-static \
        --disable-shared \
        --disable-cli \
        --enable-pic
    echo "[x264] building..."
    make -j"$(sysctl -n hw.ncpu)"
    make install
    cd /tmp/ufb-ffmpeg-build
fi
export PKG_CONFIG_PATH="$X264_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

if [[ ! -d "$SOURCE_DIR" ]]; then
    echo "[ffmpeg] downloading ffmpeg ${FFMPEG_VERSION}..."
    curl -L "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
        -o ffmpeg.tar.xz
    tar xf ffmpeg.tar.xz
fi

cd "$SOURCE_DIR"
export MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"

# Two-purpose build:
#   1. The C++ thumbnail backend (app/thumbnails/VideoBackend.cpp)
#      links libavcodec/format/util/swscale/swresample directly, so we
#      need those libs present with a minimal decoder/demuxer set.
#   2. bindings/src/services/transcode.rs spawns ffmpeg + ffprobe as
#      subprocesses to do the actual h264 transcode the user asked
#      for, so the CLI tools need a much broader codec/encoder/filter
#      set than the C++ backend's read-only path.
#
# Rather than maintain two separate configure invocations, ship the
# fuller "default ffmpeg" build (videotoolbox HW accel for free on
# darwin, plus libx264 statically linked in for the transcode path).
#
# Notes:
#   --disable-autodetect prevents configure from picking up Homebrew
#     libs (libxcb / libx11 / sdl2) that would baked-in absolute
#     paths into bin/ffmpeg and break on user machines without
#     /opt/homebrew. Apple's bundled frameworks (Foundation, AppKit,
#     AudioToolbox, AVFoundation, VideoToolbox, CoreFoundation) come
#     in via --enable-videotoolbox + framework auto-link and aren't
#     subject to autodetect — those are part of macOS itself.
#   --disable-ffplay drops the SDL2-using preview binary; transcode
#     only spawns ffmpeg + ffprobe.
#   --enable-videotoolbox uses macOS's hardware decoder/encoder for
#     h264/hevc when present.
#   --enable-gpl --enable-libx264 makes the build GPL, matching the
#     BtbN GPL build Windows ships. UFB itself is GPL-3.0-or-later,
#     so nothing changes for the app; transcode.rs needs libx264.
#     x264 is found via PKG_CONFIG_PATH (static prefix above) —
#     explicit --enable-* still probes despite --disable-autodetect.
echo "[ffmpeg] configuring..."
./configure \
    --prefix="$OUTPUT_DIR" \
    --disable-static \
    --enable-shared \
    --disable-autodetect \
    --disable-ffplay \
    --enable-videotoolbox \
    --enable-gpl \
    --enable-libx264 \
    --disable-doc \
    --disable-debug

echo "[ffmpeg] building..."
make -j"$(sysctl -n hw.ncpu)"
echo "[ffmpeg] installing to $OUTPUT_DIR..."
make install

# Confirm Mach-Os are relocatable. ffmpeg's default install_name uses
# the build prefix; we rewrite to @rpath/<name> so the bundle's
# Frameworks/ layout works without LD_LIBRARY_PATH gymnastics.
#
# Drives the rewrite off `otool -L <macho>` rather than file globs.
# ffmpeg's binaries link against the symlink-named dylibs (e.g.
# `libavformat.61.dylib`, not the realpath `libavformat.61.7.100.
# dylib`), so a glob loop that filters out symlinks misses the names
# the linker actually wrote into the load commands. Walking otool's
# output catches every such reference exactly as it appears.
patch_macho() {
    local target="$1"
    [[ -f "$target" ]] || return 0
    # Iterate every dep that points back into the build prefix and
    # rewrite to @rpath/<basename>.
    while IFS= read -r dep; do
        local depbase
        depbase=$(basename "$dep")
        install_name_tool -change "$dep" "@rpath/$depbase" "$target" \
            2>/dev/null || true
    done < <(otool -L "$target" 2>/dev/null \
                | awk -v prefix="$OUTPUT_DIR/lib/" \
                      'NR>1 && $1 ~ "^"prefix {print $1}')
}

echo "[ffmpeg] rewriting dylib install_names + cross-deps to @rpath..."
for lib in "$OUTPUT_DIR/lib"/*.dylib; do
    # Skip symlinks for the -id rewrite (only patch the real versioned
    # files), but DO patch all real dylibs' cross-deps.
    if [[ -L "$lib" ]]; then continue; fi
    base=$(basename "$lib")
    install_name_tool -id "@rpath/$base" "$lib" 2>/dev/null || true
    patch_macho "$lib"
done

# ffmpeg/ffprobe binaries: same dependency rewrite, plus an LC_RPATH
# of @executable_path/../Frameworks. The CMake POST_BUILD step in
# app/CMakeLists.txt drops these binaries into UFB.app/Contents/
# MacOS/ next to the main `ufb` Mach-O, where the parent path
# resolves to UFB.app/Contents/Frameworks/ — exactly where the libav*
# dylibs are placed by the same step that handles ufb's own links.
echo "[ffmpeg] rewriting binary load paths..."
for bin in "$OUTPUT_DIR/bin/ffmpeg" "$OUTPUT_DIR/bin/ffprobe"; do
    [[ -x "$bin" ]] || continue
    patch_macho "$bin"
    # Drop any pre-existing rpath that points at the build prefix
    # (configure leaves one), then add the bundle-relative one. The
    # check-and-delete loop is bounded by the rpath count to avoid
    # an infinite spin if delete_rpath ever no-ops silently.
    while IFS= read -r old; do
        [[ -z "$old" ]] && continue
        install_name_tool -delete_rpath "$old" "$bin" 2>/dev/null || true
    done < <(otool -l "$bin" 2>/dev/null \
                | awk '/cmd LC_RPATH/{f=1; next} f && /path /{print $2; f=0}')
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$bin"
done

echo "[ffmpeg] done. Output: $OUTPUT_DIR"
