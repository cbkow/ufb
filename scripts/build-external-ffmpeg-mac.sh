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

FFMPEG_VERSION="9.0.1"
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
    # Local patches (copies of QCView-Player's external/patches/ffmpeg,
    # kept in-tree so the two apps decode the same way): DNxHR 444
    # adaptive colour transform + Avid legal-range tag, MXF RGBA range
    # tag from ComponentMin/MaxRef, ProRes RAW non-zero Bayer pattern
    # headers. Applied once, right after extraction; a fresh SOURCE_DIR
    # re-applies them.
    for p in "$REPO_ROOT"/scripts/ffmpeg-patches/*.patch; do
        echo "[ffmpeg] applying $(basename "$p")"
        patch -d "$SOURCE_DIR" -p1 --forward < "$p"
    done
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

# Make the Mach-Os relocatable: dylib ids, cross-deps and the CLI
# binaries' load commands all become @rpath/<real versioned file name>,
# and ffmpeg/ffprobe get LC_RPATH @executable_path/../Frameworks.
# Lives in its own idempotent script so it can be re-run on an existing
# external/ffmpeg (see its header for the macOS 27.2 leaf-name rule).
"$SCRIPT_DIR/fix-ffmpeg-install-names-mac.sh" "$OUTPUT_DIR"

echo "[ffmpeg] done. Output: $OUTPUT_DIR"
