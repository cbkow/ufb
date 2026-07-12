#!/usr/bin/env bash
# setup-external-mac.sh
#
# Fetch + build the external dependencies UFB's Phase 14 thumbnail
# pipeline + transcode subprocess needs on macOS:
#   external/pdfium/    PDFium darwin (bblanchon prebuilt)
#   external/ffmpeg/    ffmpeg darwin libs + ffmpeg/ffprobe binaries
#                       (built from source — see
#                       scripts/build-external-ffmpeg-mac.sh)
#   external/exiftool/  exiftool standalone Perl distribution from
#                       exiftool.org (mirrors the Windows pattern in
#                       scripts/setup-external.ps1; on macOS the
#                       script runs against /usr/bin/perl which
#                       ships with the OS)
#
# Also applies the Qt 6.11.0 qyieldcpu.h fix per macOSplans/01 +
# the QTBUG-145239 patch script.
#
# Idempotent — every step is gated on output existence. Re-running
# this script is the safe way to refresh the env after a Qt update,
# OS update, or external/ purge.
#
# Wired into `just mac-setup-external` (slice 09).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXTERNAL="$REPO_ROOT/external"

ARCH=$(uname -m)
case "$ARCH" in
    arm64)  PDFIUM_ARCH="mac-arm64" ;;
    x86_64) PDFIUM_ARCH="mac-x64"   ;;
    *) echo "[setup] unsupported arch: $ARCH"; exit 1 ;;
esac

# ---- PDFium (bblanchon prebuilts) --------------------------
# Bump deliberately when upgrading. Each release is tagged
# `chromium/<chromium-version>` upstream.
PDFIUM_VERSION="6996"
PDFIUM_URL="https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F${PDFIUM_VERSION}/pdfium-${PDFIUM_ARCH}.tgz"
PDFIUM_DIR="$EXTERNAL/pdfium"

if [[ ! -f "$PDFIUM_DIR/include/fpdfview.h" ]]; then
    echo "[setup] fetching PDFium $PDFIUM_VERSION ($PDFIUM_ARCH)..."
    mkdir -p "$PDFIUM_DIR"
    tmp=$(mktemp -d)
    curl -L "$PDFIUM_URL" -o "$tmp/pdfium.tgz"
    tar xf "$tmp/pdfium.tgz" -C "$PDFIUM_DIR"
    rm -rf "$tmp"
    echo "[setup] PDFium installed at $PDFIUM_DIR"

    # Make sure libpdfium.dylib's install_name is @rpath-relative so
    # the bundle copy in app/CMakeLists.txt's POST_BUILD step resolves
    # cleanly. bblanchon ships dylibs with the bare basename, which
    # becomes a self-relative load on link; rewriting to @rpath
    # mirrors what ffmpeg gets in build-external-ffmpeg-mac.sh.
    if [[ -f "$PDFIUM_DIR/lib/libpdfium.dylib" ]]; then
        install_name_tool -id "@rpath/libpdfium.dylib" \
            "$PDFIUM_DIR/lib/libpdfium.dylib" 2>/dev/null || true
    fi
else
    echo "[setup] PDFium already present"
fi

# ---- ffmpeg (built from source) ----------------------------
# Gated on both headers AND CLI tools — adding the ffmpeg/ffprobe
# requirement after a libs-only build is what triggers a rebuild
# on machines that ran the older script.
if [[ ! -f "$EXTERNAL/ffmpeg/include/libavcodec/avcodec.h" \
   || ! -x "$EXTERNAL/ffmpeg/bin/ffmpeg" \
   || ! -x "$EXTERNAL/ffmpeg/bin/ffprobe" ]]; then
    echo "[setup] building ffmpeg from source (one-time, ~15-30 min)..."
    if [[ -d "$EXTERNAL/ffmpeg" \
       && ! -x "$EXTERNAL/ffmpeg/bin/ffmpeg" ]]; then
        echo "[setup] removing libs-only ffmpeg build to pick up programs"
        rm -rf "$EXTERNAL/ffmpeg"
        rm -rf /tmp/ufb-ffmpeg-build
    fi
    bash "$REPO_ROOT/scripts/build-external-ffmpeg-mac.sh"
else
    echo "[setup] ffmpeg already built"
fi

# ---- exiftool (standalone Perl distribution) ----------------
# Pulls the same vendor-bundled tarball Windows uses, sourced
# directly from exiftool.org (no sister-repo dependency since
# this is just a tarball). Runs on system /usr/bin/perl —
# verified at the top of bindings/src/services/transcode.rs's
# bundled_tool fallback.
EXIFTOOL_VERSION="13.58"
EXIFTOOL_DIR="$EXTERNAL/exiftool"
if [[ ! -f "$EXIFTOOL_DIR/exiftool" ]]; then
    echo "[setup] fetching exiftool $EXIFTOOL_VERSION..."
    tmp=$(mktemp -d)
    tarball_url="https://exiftool.org/Image-ExifTool-${EXIFTOOL_VERSION}.tar.gz"
    if curl -fL "$tarball_url" -o "$tmp/exiftool.tar.gz"; then
        mkdir -p "$EXIFTOOL_DIR"
        # The tarball extracts to Image-ExifTool-<version>/; strip
        # that wrapper so the script + lib/ tree sit directly under
        # external/exiftool/ (mirrors the Windows external/exiftool/
        # layout the CMake POST_BUILD step copies from).
        tar xf "$tmp/exiftool.tar.gz" -C "$tmp"
        cp -R "$tmp/Image-ExifTool-${EXIFTOOL_VERSION}/." "$EXIFTOOL_DIR/"
        rm -rf "$tmp"
        # Make sure the script is executable; tar should preserve
        # this, but a hostile umask can clear it.
        chmod +x "$EXIFTOOL_DIR/exiftool"
        echo "[setup] exiftool installed at $EXIFTOOL_DIR"
    else
        rm -rf "$tmp"
        echo "[setup] WARN: failed to fetch exiftool from $tarball_url"
        echo "[setup]       transcode pipeline will skip metadata copy"
    fi
else
    echo "[setup] exiftool already present"
fi

# ---- Qt 6.11.0 header patch (QTBUG-145239) -----------------
# Skips silently if Qt 6.11.1+ is installed (unbroken upstream)
# or if already patched. See script header for context.
bash "$REPO_ROOT/scripts/patch-qt-mac-headers.sh" || \
    echo "[setup] WARN: Qt header patch failed; cxx-qt builds may fail until Qt 6.11.1 lands"

echo
echo "[setup] done. Next: cmake --preset=mac-release && cmake --build --preset=mac-release"
