#!/bin/bash
# release-mac.sh — one-command UFB macOS release.
#
#   1. cargo build --release         (Rust agent + bindings)
#   2. cmake --build release          (UFB.app)
#   3. xcodebuild Release             (UFBTray.app + FinderSync.appex)
#   4. scripts/sign-mac-dev.sh        (Developer ID + hardened runtime,
#                                      stages ufb-agent.app from binary)
#   5. scripts/build-mac-dmg.sh       (UDZO DMG, signed)
#   6. scripts/notarize-mac-dmg.sh    (Apple notary, --wait, staple)
#
# Output: dist/UFB-<version>-<arch>.dmg
#
# Time: typically 12–20 min — most of it is Apple's notary queue
# in step (6). Steps (1)–(5) take 2–4 min on an M1.
#
# Env:
#   UFB_BUILD_PRESET=mac-release   (default, set explicitly to override)
#   UFB_SIGNING_IDENTITY=...
#   UFB_NOTARY_PROFILE=AC_PASSWORD
#
# Use this for actual releases; for dev iteration on signing or
# DMG layout, run the individual scripts.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESET="${UFB_BUILD_PRESET:-mac-release}"

case "$PRESET" in
    mac-release) CMAKE_DIR="build/mac-release"; CARGO_PROFILE="release"; XCODE_CFG="Release" ;;
    mac-debug)
        echo "WARN: UFB_BUILD_PRESET=mac-debug for a release build." >&2
        echo "      Releases should be Release-mode for size + speed." >&2
        CMAKE_DIR="build/mac-debug"; CARGO_PROFILE="debug"; XCODE_CFG="Debug"
        ;;
    *) echo "ERROR: unknown UFB_BUILD_PRESET=$PRESET" >&2; exit 2 ;;
esac

echo "=== UFB macOS release ==="
echo "Preset:    $PRESET"
echo "Repo:      $REPO_ROOT"
echo ""

# ── 1. Cargo (agent + core + bindings) ───────────────────────────────
# Building the agent here — the bindings are also built but folded
# into UFB.app's libbindings.a by Corrosion during the cmake step.
echo "[1/6] cargo build --$CARGO_PROFILE (agent)"
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
echo "[2/6] cmake (UFB.app) — preset=$PRESET"
QMAKE="${QMAKE:-$HOME/Qt/6.11.1/macos/bin/qmake}"
QMAKE="$QMAKE" cmake "$REPO_ROOT" -B "$REPO_ROOT/$CMAKE_DIR" 2>&1 | tail -3
QMAKE="$QMAKE" cmake --build "$REPO_ROOT/$CMAKE_DIR" --target ufb 2>&1 | tail -3

# ── 3. Xcode (UFBFinderSync.appex) ───────────────────────────────────
# UFBTray was deleted in 1.0.7 (plans/17 slice E close-out) — the appex
# is the only Swift artifact left and it now rides inside UFB.app's
# own PlugIns/ (sync verdict kept FinderSync badges).
echo ""
echo "[3/6] xcodebuild (UFBFinderSync.appex) — config=$XCODE_CFG"
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

# ── 4. Sign all three ────────────────────────────────────────────────
# sign-mac-dev.sh stages ufb-agent.app from the cargo binary, signs
# all three components Developer-ID, attaches entitlements + the
# hardened runtime flag.
echo ""
echo "[4/6] sign-mac-dev.sh"
UFB_BUILD_PRESET="$PRESET" "$REPO_ROOT/scripts/sign-mac-dev.sh" 2>&1 \
    | sed 's/^/  /'

# ── 5. DMG ───────────────────────────────────────────────────────────
echo ""
echo "[5/6] build-mac-dmg.sh"
UFB_BUILD_PRESET="$PRESET" "$REPO_ROOT/scripts/build-mac-dmg.sh" 2>&1 \
    | sed 's/^/  /'

# Find the freshly-built DMG so we hand notarytool an explicit path
# (the dist/ dir may have older artifacts hanging around).
DMG_PATH="$(ls -t "$REPO_ROOT/dist"/*.dmg 2>/dev/null | head -1)"
if [ -z "$DMG_PATH" ]; then
    echo "ERROR: build-mac-dmg.sh didn't produce a DMG." >&2
    exit 1
fi

# ── 6. Notarize + staple ─────────────────────────────────────────────
# This is the long one (5–15 min). Apple notary service queue is
# the dominant cost. The staple at the end embeds the ticket so
# downstream first-launches are fast.
echo ""
echo "[6/6] notarize-mac-dmg.sh"
"$REPO_ROOT/scripts/notarize-mac-dmg.sh" "$DMG_PATH" 2>&1 \
    | sed 's/^/  /'

# ── 7. Appcast (Sparkle, informational) ──────────────────────────────
# Insert this release into docs/appcast-mac.xml — the committed feed
# GitHub Pages serves at ufbrowser.com. Informational item — no
# signing needed (notarization is the integrity guarantee). To also
# update the Windows appcast, run make-appcast.sh again with the
# Windows-built UFB-<ver>-x64.exe as a 3rd arg (needs sign_update +
# the private key).
echo ""
echo "[7/7] make-appcast.sh (docs/appcast-mac.xml)"
DMG_VERSION="$(basename "$DMG_PATH" | sed -E 's/^UFB-(.+)-(arm64|x86_64|x64)\.dmg$/\1/')"
"$REPO_ROOT/scripts/make-appcast.sh" "$DMG_VERSION" "$DMG_PATH" 2>&1 \
    | sed 's/^/  /' || echo "  (appcast update skipped/failed — non-fatal)"

echo ""
echo "=== Release ready ==="
echo "DMG:     $DMG_PATH"
echo "Size:    $(du -sh "$DMG_PATH" | cut -f1)"
echo "Appcast: $REPO_ROOT/docs/appcast-mac.xml (edited, not yet committed)"
echo "Publish: gh release create v$DMG_VERSION \"$DMG_PATH\" --title \"UFB $DMG_VERSION\""
echo "         then commit + push docs/appcast-mac.xml (Pages -> ufbrowser.com)"
echo ""
echo "Next: drag-test on a fresh machine (or rm + reinstall locally"
echo "      after \`xattr -d com.apple.quarantine\` to simulate a"
echo "      Safari download). First launch should now be seconds,"
echo "      not minutes."
