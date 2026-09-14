#!/usr/bin/env bash
# fetch-third-party-sources.sh
#
# Download the exact source tarballs of the copyleft (GPL / LGPL)
# components UFB redistributes as binaries, so they can be attached
# to the matching GitHub release next to the installers. The LGPL and
# GPL ask the *distributor* to provide corresponding source; a link
# to upstream is not enough once upstream moves or vanishes.
#
# Versions here MUST match what the build scripts actually bundle —
# cross-check LICENSES/THIRD_PARTY_NOTICES.txt when bumping any of:
#   scripts/build-external-7zip-mac.sh   (SEVENZIP_VERSION)
#   scripts/build-external-ffmpeg-mac.sh (FFMPEG_VERSION, x264 stable)
#   vcpkg.json / vcpkg_installed/vcpkg/status (libheif, libde265)
#
# Usage:
#   scripts/fetch-third-party-sources.sh [version]     # default: app version
#   gh release upload v<version> dist/third-party-sources-<version>/*
#
# Idempotent: existing files are kept. Run from any cwd.

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-$(sed -n 's/^version = "\(.*\)"/\1/p' "$REPO_ROOT/Cargo.toml" | head -1)}"
OUT="$REPO_ROOT/dist/third-party-sources-$VERSION"
mkdir -p "$OUT"

SEVENZIP_VERSION="26.03"
FFMPEG_VERSION="9.0.1"
LIBHEIF_VERSION="1.21.2"
LIBDE265_VERSION="1.0.18"

fetch() {  # fetch <url> <dest-name>
    local url="$1" name="$2"
    if [[ -s "$OUT/$name" ]]; then
        echo "[src] have  $name"
    else
        echo "[src] fetch $name"
        curl -fL --retry 3 "$url" -o "$OUT/$name.part"
        mv "$OUT/$name.part" "$OUT/$name"
    fi
}

fetch "https://www.7-zip.org/a/7z${SEVENZIP_VERSION//./}-src.tar.xz" \
      "7z${SEVENZIP_VERSION//./}-src.tar.xz"
fetch "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
      "ffmpeg-${FFMPEG_VERSION}.tar.xz"
fetch "https://github.com/strukturag/libheif/archive/refs/tags/v${LIBHEIF_VERSION}.tar.gz" \
      "libheif-${LIBHEIF_VERSION}.tar.gz"
fetch "https://github.com/strukturag/libde265/archive/refs/tags/v${LIBDE265_VERSION}.tar.gz" \
      "libde265-${LIBDE265_VERSION}.tar.gz"

# x264 is built from the moving "stable" branch. Prefer the tarball the
# mac FFmpeg build actually consumed (kept in its build dir) so the
# attached source matches the shipped binary; fall back to fetching
# stable now and say so.
X264_LOCAL="/tmp/ufb-ffmpeg-build/x264.tar.bz2"
if [[ -s "$X264_LOCAL" ]]; then
    cp -n "$X264_LOCAL" "$OUT/x264-stable.tar.bz2" 2>/dev/null || true
    echo "[src] x264  copied from the local FFmpeg build ($(date -r "$X264_LOCAL" +%Y-%m-%d))"
else
    echo "[src] WARN: no local x264 tarball; fetching current stable (may be newer than the shipped build)"
    fetch "https://code.videolan.org/videolan/x264/-/archive/stable/x264-stable.tar.bz2" \
          "x264-stable.tar.bz2"
fi

# Windows FFmpeg is BtbN's build of the release/9.0 branch; the exact
# git revision + build scripts are published per BtbN release. Record
# the pointer rather than mirroring their multi-GB tree.
cat > "$OUT/README.txt" <<TXT
Corresponding source for the copyleft components bundled in UFB $VERSION.
See LICENSES/THIRD_PARTY_NOTICES.txt inside the app for how each is used.

  7-Zip $SEVENZIP_VERSION        LGPL-2.1 + unRAR restriction   (7zz / 7z.exe + 7z.dll)
  FFmpeg $FFMPEG_VERSION         GPL-2.0-or-later (libx264 on)  (macOS build)
  x264 stable             GPL-2.0-or-later               (macOS, static in libavcodec)
  libheif $LIBHEIF_VERSION        LGPL-3.0                       (static on macOS, DLL on Windows)
  libde265 $LIBDE265_VERSION      LGPL-3.0                       (static on macOS, DLL on Windows)

Windows FFmpeg: BtbN ffmpeg-n9.0-latest-win64-gpl-shared —
https://github.com/BtbN/FFmpeg-Builds (source revision + scripts per release).
Qt 6.11.1 (LGPL-3): https://download.qt.io/official_releases/qt/6.11/6.11.1/single/
UFB itself (GPL-3.0-or-later): https://github.com/cbkow/ufb, tag v$VERSION
TXT

shasum -a 256 "$OUT"/* > "$OUT/SHA256SUMS" 2>/dev/null || true
echo
echo "[src] done → $OUT"
echo "Attach: gh release upload v$VERSION \"$OUT\"/*"
