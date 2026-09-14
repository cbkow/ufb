#!/bin/bash
# release-mac.sh — one-command UFB macOS release.
#
#   1. cargo build --release         (Rust agent + bindings)
#   2. cmake --build release          (UFB.app)
#   3. xcodebuild Release             (UFBFinderSync.appex, embedded)
#   4. scripts/sign-mac-dev.sh        (Developer ID + hardened runtime,
#                                      stages ufb-agent.app from binary)
#   5. scripts/notarize-mac.sh        (both bundles: notarize + staple)
#   6. scripts/build-mac-pkg.sh       (installer pkg, Developer ID Installer)
#   7. scripts/notarize-mac.sh        (the pkg: notarize + staple)
#   8. scripts/make-appcast.sh        (signed enclosure -> docs/appcast-mac.xml)
#
# Output: dist/UFB-<version>-<arch>.pkg   (DMG retired in 1.2.0)
#
# Time: typically 15–30 min — most of it is Apple's notary queue,
# twice (bundles, then pkg). Steps (1)–(4) take 2–4 min on an M1.
#
# Env:
#   UFB_BUILD_PRESET=mac-release   (default, set explicitly to override)
#   UFB_SIGNING_IDENTITY=...
#   UFB_NOTARY_PROFILE=AC_PASSWORD
#
# Use this for actual releases; for dev iteration on signing or
# pkg layout, run the individual scripts.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESET="${UFB_BUILD_PRESET:-mac-release}"

case "$PRESET" in
    mac-release) CMAKE_DIR="build/mac-release"; CARGO_PROFILE="release"; XCODE_CFG="Release"; CMAKE_BUILD_TYPE="Release" ;;
    mac-debug)
        echo "WARN: UFB_BUILD_PRESET=mac-debug for a release build." >&2
        echo "      Releases should be Release-mode for size + speed." >&2
        CMAKE_DIR="build/mac-debug"; CARGO_PROFILE="debug"; XCODE_CFG="Debug"; CMAKE_BUILD_TYPE="Debug"
        ;;
    *) echo "ERROR: unknown UFB_BUILD_PRESET=$PRESET" >&2; exit 2 ;;
esac

echo "=== UFB macOS release ==="
echo "Preset:    $PRESET"
echo "Repo:      $REPO_ROOT"
echo ""

# Minimum supported macOS. Mirrors CMAKE_OSX_DEPLOYMENT_TARGET in
# CMakeLists.txt (which covers the cmake step); exporting it here also
# pins the agent's cargo build and anything else cc-compiled outside
# CMake. The [gate] step below rejects any bundled Mach-O stamped newer.
UFB_MACOS_MIN="14.0"
export MACOSX_DEPLOYMENT_TARGET="$UFB_MACOS_MIN"

# ── 0. vcpkg (libheif + OpenEXR, static, deployment-target-pinned) ───
# find_package in cmake/external.cmake looks in vcpkg_installed/ at the
# repo root. If this step hasn't run, those find_packages used to fall
# through to Homebrew — whose bottles target the *host* macOS and made
# the shipped app host-OS-only. The overlay triplet pins
# VCPKG_OSX_DEPLOYMENT_TARGET; no-op when already installed.
echo "[0/8] vcpkg install (libheif + OpenEXR)"
VCPKG_BIN="$(command -v vcpkg || true)"
[ -z "$VCPKG_BIN" ] && [ -x "${VCPKG_ROOT:-}/vcpkg" ] && VCPKG_BIN="$VCPKG_ROOT/vcpkg"
[ -z "$VCPKG_BIN" ] && [ -x "$HOME/vcpkg/vcpkg" ] && VCPKG_BIN="$HOME/vcpkg/vcpkg"
if [ -z "$VCPKG_BIN" ]; then
    echo "ERROR: vcpkg not found (PATH, \$VCPKG_ROOT, ~/vcpkg) — needed for libheif/OpenEXR." >&2
    exit 2
fi
(
    cd "$REPO_ROOT"
    "$VCPKG_BIN" install \
        --triplet "arm64-osx" \
        --overlay-triplets "$REPO_ROOT/cmake/vcpkg-triplets" 2>&1 | tail -3
)

# ── 1. Cargo (agent + core + bindings) ───────────────────────────────
# Building the agent here — the bindings are also built but folded
# into UFB.app's libbindings.a by Corrosion during the cmake step.
echo "[1/8] cargo build --$CARGO_PROFILE (agent)"
(
    cd "$REPO_ROOT/agent"
    if [ "$CARGO_PROFILE" = "release" ]; then
        cargo build --release 2>&1 | tail -3
    else
        cargo build 2>&1 | tail -3
    fi
)

# ── 2. CMake / Qt (UFB.app) ──────────────────────────────────────────
# Qt's qmake is in the standard install path. The build invokes
# Corrosion which builds the bindings crate via cargo, then links
# everything into UFB.app/Contents/MacOS/ufb. Reconfigure first so
# any change in CMakeLists.txt picks up.
echo ""
echo "[2/8] cmake (UFB.app) — preset=$PRESET"
QMAKE="${QMAKE:-$HOME/Qt/6.11.1/macos/bin/qmake}"
# Explicit build type: on a fresh build dir a bare configure would
# otherwise land on an empty CMAKE_BUILD_TYPE (unoptimized).
QMAKE="$QMAKE" cmake "$REPO_ROOT" -B "$REPO_ROOT/$CMAKE_DIR" \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" 2>&1 | tail -3
QMAKE="$QMAKE" cmake --build "$REPO_ROOT/$CMAKE_DIR" --target ufb 2>&1 | tail -3

# ── 3. Xcode (UFBFinderSync.appex) ───────────────────────────────────
# UFBTray was deleted in 1.0.7 (plans/17 slice E close-out) — the appex
# is the only Swift artifact left and it now rides inside UFB.app's
# own PlugIns/ (sync verdict kept FinderSync badges).
echo ""
echo "[3/8] xcodebuild (UFBFinderSync.appex) — config=$XCODE_CFG"
(
    cd "$REPO_ROOT/macos-helpers"
    xcodegen generate 2>&1 | tail -3
    xcodebuild \
        -project UFBHelpers.xcodeproj \
        -scheme UFBFinderSync \
        -configuration "$XCODE_CFG" \
        BUILD_DIR="$(pwd)/build" 2>&1 | tail -3
)

# Embed the appex into UFB.app before signing — the sign step seals
# the outer bundle over it. ditto preserves bundle structure.
UFB_APP_BUNDLE="$REPO_ROOT/$CMAKE_DIR/app/ufb.app"
FS_APPEX_SRC="$REPO_ROOT/macos-helpers/build/$XCODE_CFG/UFBFinderSync.appex"
if [ -d "$FS_APPEX_SRC" ]; then
    mkdir -p "$UFB_APP_BUNDLE/Contents/PlugIns"
    rm -rf "$UFB_APP_BUNDLE/Contents/PlugIns/UFBFinderSync.appex"
    ditto "$FS_APPEX_SRC" "$UFB_APP_BUNDLE/Contents/PlugIns/UFBFinderSync.appex"
    echo "  embedded UFBFinderSync.appex into UFB.app/Contents/PlugIns/"
else
    echo "  WARN: $FS_APPEX_SRC missing — shipping without FinderSync badges" >&2
fi

# ── Gate: deployment-target + dylib-provenance sweep ─────────────────
# Every Mach-O in the bundle must be loadable on macOS $UFB_MACOS_MIN:
# dyld hard-refuses any binary whose minos is newer than the running
# OS. This is the regression guard for the 1.0.x era where the main
# binary + Homebrew dylibs were stamped with the build machine's OS
# and the app only launched on macOS 26.
echo ""
echo "[gate] minos <= $UFB_MACOS_MIN sweep"
GATE_FAIL=0
while IFS= read -r -d '' f; do
    file -b "$f" | grep -q "Mach-O" || continue
    minos="$(vtool -show-build "$f" 2>/dev/null | awk '/minos/{print $2; exit}')"
    [ -n "$minos" ] || continue
    if [ "$(printf '%s\n' "$UFB_MACOS_MIN" "$minos" | sort -V | tail -1)" != "$UFB_MACOS_MIN" ]; then
        echo "  FAIL minos=$minos  ${f#"$UFB_APP_BUNDLE"/}" >&2
        GATE_FAIL=1
    fi
done < <(find "$UFB_APP_BUNDLE" "$REPO_ROOT/agent/target/$CARGO_PROFILE/ufb-agent" -type f -print0)

# Provenance: libheif/OpenEXR must come from vcpkg_installed, never
# Homebrew (host-targeted bottles), and must not be silently absent
# (HeifBackend degrades to a stub and HEIC thumbnails vanish).
for var in libheif_DIR OpenEXR_DIR Imath_DIR; do
    val="$(grep "^${var}:PATH=" "$REPO_ROOT/$CMAKE_DIR/CMakeCache.txt" | cut -d= -f2-)"
    case "$val" in
        *vcpkg_installed*) ;;
        *homebrew*|*-NOTFOUND|"")
            echo "  FAIL $var=$val (want vcpkg_installed path)" >&2
            GATE_FAIL=1 ;;
        *) echo "  WARN $var=$val (not vcpkg_installed — check provenance)" >&2 ;;
    esac
done
if [ "$GATE_FAIL" -ne 0 ]; then
    echo "ERROR: deployment-target gate failed — app would not load on macOS $UFB_MACOS_MIN." >&2
    exit 1
fi
echo "  OK — all Mach-Os load on macOS $UFB_MACOS_MIN+"

# ── 4. Sign all three ────────────────────────────────────────────────
# sign-mac-dev.sh stages ufb-agent.app from the cargo binary, signs
# all three components Developer-ID, attaches entitlements + the
# hardened runtime flag.
echo ""
echo "[4/8] sign-mac-dev.sh"
UFB_BUILD_PRESET="$PRESET" "$REPO_ROOT/scripts/sign-mac-dev.sh" 2>&1 \
    | sed 's/^/  /'

# ── 5. Notarize + staple the bundles ─────────────────────────────────
# One submission for both apps, stapled in place, so the installed
# apps validate offline at first launch. The pkg gets its own ticket
# in step 7. (Apple's queue: 2-15 min per submission.)
echo ""
echo "[5/8] notarize-mac.sh (UFB.app + ufb-agent.app)"
"$REPO_ROOT/scripts/notarize-mac.sh" \
    "$UFB_APP_BUNDLE" \
    "$REPO_ROOT/agent/target/$CARGO_PROFILE/ufb-agent.app" 2>&1 \
    | sed 's/^/  /'

# ── 6. Installer package ─────────────────────────────────────────────
# Signed with the Developer ID *Installer* identity. Replaced the
# drag-install DMG in 1.2.0 — see scripts/build-mac-pkg.sh.
echo ""
echo "[6/8] build-mac-pkg.sh"
UFB_BUILD_PRESET="$PRESET" "$REPO_ROOT/scripts/build-mac-pkg.sh" 2>&1 \
    | sed 's/^/  /'

PKG_PATH="$(ls -t "$REPO_ROOT/dist"/*.pkg 2>/dev/null | head -1)"
if [ -z "$PKG_PATH" ]; then
    echo "ERROR: build-mac-pkg.sh didn't produce a pkg." >&2
    exit 1
fi

# ── 7. Notarize + staple the pkg ─────────────────────────────────────
echo ""
echo "[7/8] notarize-mac.sh ($(basename "$PKG_PATH"))"
"$REPO_ROOT/scripts/notarize-mac.sh" "$PKG_PATH" 2>&1 \
    | sed 's/^/  /'

# ── 8. Appcast (Sparkle, signed pkg enclosure) ───────────────────────
# Insert this release into docs/appcast-mac.xml — the committed feed
# GitHub Pages serves at ufbrowser.com. Since 1.2.0 the mac item is a
# real download: Sparkle fetches the pkg from the GitHub release,
# verifies the ed25519 sparkle:edSignature, and runs a guided package
# install. Needs sign_update (Sparkle's bin/) + the private key in the
# login Keychain — same as the Windows item. To also update the
# Windows appcast, run make-appcast.sh again with the Windows-built
# UFB-<ver>-x64.exe as a 3rd arg.
echo ""
echo "[8/8] make-appcast.sh (docs/appcast-mac.xml)"
PKG_VERSION="$(basename "$PKG_PATH" | sed -E 's/^UFB-(.+)-(arm64|x86_64|x64)\.pkg$/\1/')"
"$REPO_ROOT/scripts/make-appcast.sh" "$PKG_VERSION" "$PKG_PATH" 2>&1 \
    | sed 's/^/  /' || echo "  (appcast update skipped/failed — non-fatal)"

echo ""
echo "=== Release ready ==="
echo "PKG:     $PKG_PATH"
echo "Size:    $(du -sh "$PKG_PATH" | cut -f1)"
echo "Appcast: $REPO_ROOT/docs/appcast-mac.xml (edited, not yet committed)"
echo "Publish: gh release create v$PKG_VERSION \"$PKG_PATH\" --title \"UFB $PKG_VERSION\""
echo "Source:  scripts/fetch-third-party-sources.sh $PKG_VERSION"
echo "         gh release upload v$PKG_VERSION dist/third-party-sources-$PKG_VERSION/*   (GPL/LGPL corresponding source)"
echo "         then commit + push docs/appcast-mac.xml (Pages -> ufbrowser.com)"
echo ""
echo "Next: install-test the pkg here (double-click, or"
echo "      sudo installer -pkg \"$PKG_PATH\" -target /), then launch"
echo "      /Applications/UFB/UFB.app. For the Sparkle path, point the"
echo "      installed app at a local feed: defaults write dev.ufb.app"
echo "      SUFeedURL file:///path/to/appcast-mac.xml (delete the key after)."
