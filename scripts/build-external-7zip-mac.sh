#!/usr/bin/env bash
# build-external-7zip-mac.sh
#
# Build the official 7-Zip command-line tool (`7zz`) for macOS from
# Igor Pavlov's source tarball, as a universal (arm64 + x86_64) Mach-O
# pinned to the same deployment target as the rest of UFB.
#
# Why from source and not the prebuilt 7zNNNN-mac.tar.xz from
# 7-zip.org: the prebuilt is stamped with the build machine's SDK
# (minos 26.0 as of 26.03), so dyld refuses to load it on macOS 14/15
# and scripts/release-mac.sh's minos gate rejects the bundle. Building
# ourselves takes ~1 minute per arch on Apple Silicon.
#
# Output (consumed by app/CMakeLists.txt's APPLE POST_BUILD block):
#   external/7zip/bin/7zz        universal CLI, copied into
#                                UFB.app/Contents/MacOS/ beside ffmpeg
#   external/7zip/License.txt    7-Zip license (LGPL + unRAR restriction)
#
# Re-runs are no-ops; delete external/7zip/ to force a rebuild. Called
# from scripts/setup-external-mac.sh.

set -euo pipefail

SEVENZIP_VERSION="26.03"          # bump deliberately (CVE fixes land here)
SEVENZIP_TAG="${SEVENZIP_VERSION//./}"
DEPLOYMENT_TARGET="${UFB_MACOS_MIN:-14.0}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="$REPO_ROOT/external/7zip"
BUILD_ROOT="/tmp/ufb-7zip-build"
SRC_DIR="$BUILD_ROOT/7z${SEVENZIP_TAG}-src"
TARBALL_URL="https://www.7-zip.org/a/7z${SEVENZIP_TAG}-src.tar.xz"

if [[ -x "$OUTPUT_DIR/bin/7zz" ]]; then
    echo "[7zip] already built at $OUTPUT_DIR — delete to rebuild"
    exit 0
fi

if ! xcode-select -p >/dev/null 2>&1; then
    echo "[7zip] Xcode command line tools required (xcode-select --install)" >&2
    exit 1
fi

mkdir -p "$BUILD_ROOT"
if [[ ! -f "$SRC_DIR/CPP/7zip/Bundles/Alone2/makefile.gcc" ]]; then
    echo "[7zip] fetching 7-Zip $SEVENZIP_VERSION source..."
    rm -rf "$SRC_DIR"
    mkdir -p "$SRC_DIR"
    curl -fL "$TARBALL_URL" -o "$BUILD_ROOT/7z-src.tar.xz"
    tar xf "$BUILD_ROOT/7z-src.tar.xz" -C "$SRC_DIR"
fi

# The gcc makefile only threads LOCAL_FLAGS into compile steps, not the
# link, so the deployment target goes through the env var clang honours
# for both. Alone2 is the "7zz" bundle (all formats, no plugins).
export MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"
cd "$SRC_DIR/CPP/7zip/Bundles/Alone2"
rm -rf b
for arch in arm64 x64; do
    echo "[7zip] building $arch (minos $DEPLOYMENT_TARGET)..."
    make -j"$(sysctl -n hw.ncpu)" -f "../../cmpl_mac_${arch}.mak" >/dev/null
done

mkdir -p "$OUTPUT_DIR/bin"
lipo -create b/m_arm64/7zz b/m_x64/7zz -output "$OUTPUT_DIR/bin/7zz"
chmod +x "$OUTPUT_DIR/bin/7zz"
cp "$SRC_DIR/DOC/License.txt" "$OUTPUT_DIR/License.txt"

# Sanity: universal + pinned minos + actually runs.
file "$OUTPUT_DIR/bin/7zz" | grep -q "universal" || { echo "[7zip] not universal?" >&2; exit 1; }
minos="$(vtool -show-build "$OUTPUT_DIR/bin/7zz" | awk '/minos/{print $2; exit}')"
if [[ "$(printf '%s\n' "$DEPLOYMENT_TARGET" "$minos" | sort -V | tail -1)" != "$DEPLOYMENT_TARGET" ]]; then
    echo "[7zip] minos $minos exceeds $DEPLOYMENT_TARGET" >&2
    exit 1
fi
"$OUTPUT_DIR/bin/7zz" i | head -1
echo "[7zip] done: $OUTPUT_DIR/bin/7zz (minos $minos)"
