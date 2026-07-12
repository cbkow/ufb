#!/bin/bash
# build-mac-dmg.sh — produce a drag-to-install DMG containing all
# three components in the slice 08 layout:
#
#   UFB.dmg
#   ├── UFB/
#   │   ├── UFB.app
#   │   └── ufb-agent.app
#   └── Applications/   (symlink to /Applications)
#
# User drags the UFB/ folder onto Applications. End state:
# /Applications/UFB/{UFB.app, ufb-agent.app}. (UFBTray deleted 1.0.7;
# the drag-replace of the UFB folder removes it from installs.)
#
# Inputs (must already exist + be Developer-ID signed; run
# scripts/sign-mac-dev.sh first):
#   - build/mac-debug/app/ufb.app
#   - agent/target/debug/ufb-agent.app
#
# Output:
#   dist/UFB-<version>-<arch>.dmg
#
# Set UFB_BUILD_PRESET=mac-release to lift artifacts from the
# release dirs instead.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIGNING_IDENTITY="${UFB_SIGNING_IDENTITY:-Developer ID Application: Christopher Bialkowski (5Z4S9VHV56)}"
PRESET="${UFB_BUILD_PRESET:-mac-debug}"

case "$PRESET" in
    mac-debug)   CMAKE_DIR="build/mac-debug"; CARGO_PROFILE="debug"; XCODE_CFG="Debug" ;;
    mac-release) CMAKE_DIR="build/mac-release"; CARGO_PROFILE="release"; XCODE_CFG="Release" ;;
    *) echo "ERROR: unknown UFB_BUILD_PRESET=$PRESET (use mac-debug or mac-release)" >&2; exit 2 ;;
esac

UFB_APP="$REPO_ROOT/$CMAKE_DIR/app/ufb.app"
AGENT_APP="$REPO_ROOT/agent/target/$CARGO_PROFILE/ufb-agent.app"

# ── Validate inputs ──────────────────────────────────────────────────
for d in "$UFB_APP" "$AGENT_APP"; do
    if [ ! -d "$d" ]; then
        echo "ERROR: $d not found." >&2
        echo "Build all three artifacts first (cmake build + xcodebuild + cargo build)" >&2
        exit 1
    fi
    if ! codesign --verify --strict "$d" 2>/dev/null; then
        echo "ERROR: $d is not signed (or signature is invalid)." >&2
        echo "Run scripts/sign-mac-dev.sh first." >&2
        exit 1
    fi
done

# Pull version from UFB.app's Info.plist so the DMG filename tracks
# the in-app version with no separate source of truth.
VERSION="$(/usr/libexec/PlistBuddy \
    -c 'Print :CFBundleShortVersionString' \
    "$UFB_APP/Contents/Info.plist")"
ARCH="$(uname -m)"

DIST_DIR="$REPO_ROOT/dist"
DMG_PATH="$DIST_DIR/UFB-${VERSION}-${ARCH}.dmg"
STAGE="$(mktemp -d -t ufb-dmg.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

echo "=== UFB DMG build ==="
echo "Preset:   $PRESET"
echo "Version:  $VERSION"
echo "Arch:     $ARCH"
echo "Output:   $DMG_PATH"
echo ""

# ── Stage ────────────────────────────────────────────────────────────
# `cp -R` preserves codesignatures + extended attributes. Putting all
# three .apps inside a single UFB/ folder lets users drag the whole
# bundle into /Applications in one shot.
echo "[1/3] Staging $STAGE"
mkdir -p "$STAGE/UFB"
# CMake's `qt_add_executable(ufb)` produces `ufb.app` (lowercase
# target name → lowercase bundle leaf). The DMG ships the cased
# form `UFB.app` so end users see it consistent with the brand. `cp -R` of a .app dir to a renamed
# destination preserves the codesignature; the inner Mach-O
# (Contents/MacOS/ufb) keeps its lowercase name to match
# CFBundleExecutable in Info.plist.in.
cp -R "$UFB_APP"   "$STAGE/UFB/UFB.app"
cp -R "$AGENT_APP" "$STAGE/UFB/"
ln -s /Applications "$STAGE/Applications"

# ── Create the DMG ───────────────────────────────────────────────────
# UDZO = compressed read-only. -ov overwrites any prior DMG at the
# same path.
echo "[2/3] hdiutil create"
mkdir -p "$DIST_DIR"
rm -f "$DMG_PATH"
hdiutil create \
    -volname "UFB" \
    -srcfolder "$STAGE" \
    -ov \
    -format UDZO \
    "$DMG_PATH" >/dev/null

# ── Sign the DMG ─────────────────────────────────────────────────────
# Notarization requires the DMG itself to carry a Developer ID
# signature. `--timestamp` adds a trusted timestamp so the signature
# stays valid past Apple cert renewals.
echo "[3/3] codesign DMG"
codesign --force --sign "$SIGNING_IDENTITY" --timestamp "$DMG_PATH"
codesign --verify --verbose=1 "$DMG_PATH" 2>&1 | sed 's/^/  /'

echo ""
echo "=== Done ==="
echo "DMG:  $DMG_PATH"
echo "Size: $(du -sh "$DMG_PATH" | cut -f1)"
echo ""
echo "Next: notarize via xcrun notarytool (scripts/release-mac.sh, when wired)."
