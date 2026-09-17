#!/usr/bin/env bash
# build-mac-pkg.sh — produce the signed macOS installer package.
#
#   dist/UFB-<version>-<arch>.pkg
#     └─ UFB-core.pkg (component, non-relocatable, preinstall script)
#          └─ /Applications/UFB/
#               ├── UFB.app
#               ├── ufb-agent.app
#               └── Uninstall UFB.command
#
# Replaces the drag-install DMG (retired 1.2.0): a pkg lays the
# two-bundle layout down deterministically, can stop the running app
# first (packaging/macos/scripts/preinstall), and is what lets Sparkle
# install macOS updates itself instead of just linking to a download.
# Modelled on minColor's packaging/macos/build-pkg.sh.
#
# Inputs (must already exist + be Developer-ID signed; run
# scripts/sign-mac-dev.sh first — and ideally notarize-mac.sh on the
# bundles so the installed apps carry stapled tickets):
#   - build/<preset>/app/ufb.app
#   - agent/target/<profile>/ufb-agent.app
#
# Version: read from the built UFB.app's CFBundleShortVersionString —
# the pkg is not a separate version pin.
#
# Signing: "Developer ID Installer" (NOT "Developer ID Application" —
# a pkg signed with the app identity notarizes but Installer refuses
# it). Falls back to an unsigned pkg with a loud warning when the
# identity is missing, for local testing only.
#
# Set UFB_BUILD_PRESET=mac-release to lift artifacts from the release
# dirs (release-mac.sh does).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESET="${UFB_BUILD_PRESET:-mac-debug}"
PKG_ID="dev.ufb.UFB"
INSTALLER_IDENTITY="${UFB_INSTALLER_IDENTITY:-Developer ID Installer: Christopher Bialkowski (5Z4S9VHV56)}"

case "$PRESET" in
    mac-debug)   CMAKE_DIR="build/mac-debug";   CARGO_PROFILE="debug" ;;
    mac-release) CMAKE_DIR="build/mac-release"; CARGO_PROFILE="release" ;;
    *) echo "ERROR: unknown UFB_BUILD_PRESET=$PRESET (use mac-debug or mac-release)" >&2; exit 2 ;;
esac

UFB_APP="$REPO_ROOT/$CMAKE_DIR/app/ufb.app"
AGENT_APP="$REPO_ROOT/agent/target/$CARGO_PROFILE/ufb-agent.app"
PKG_SRC="$REPO_ROOT/packaging/macos"

for b in "$UFB_APP" "$AGENT_APP"; do
    [ -d "$b" ] || { echo "ERROR: missing $b — build + scripts/sign-mac-dev.sh first" >&2; exit 1; }
    codesign --verify --strict "$b" 2>/dev/null \
        || { echo "ERROR: $b is not validly signed — run scripts/sign-mac-dev.sh" >&2; exit 1; }
done

VERSION="$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$UFB_APP/Contents/Info.plist")"
ARCH="$(uname -m)"
OUT_DIR="$REPO_ROOT/dist"
PKG="$OUT_DIR/UFB-$VERSION-$ARCH.pkg"
mkdir -p "$OUT_DIR"

STAGE="$(mktemp -d -t ufb-pkg)"
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/root/Applications/UFB"
mkdir -p "$ROOT" "$STAGE/pkgs" "$STAGE/scripts"

echo "[pkg] staging payload (v$VERSION, $ARCH)"
ditto "$UFB_APP"   "$ROOT/UFB.app"
ditto "$AGENT_APP" "$ROOT/ufb-agent.app"
cp "$PKG_SRC/uninstall.command" "$ROOT/Uninstall UFB.command"
chmod 755 "$ROOT/Uninstall UFB.command"
cp "$PKG_SRC/scripts/preinstall" "$STAGE/scripts/preinstall"
chmod 755 "$STAGE/scripts/preinstall"

# Component plist: every bundle pkgbuild finds (UFB.app, ufb-agent.app,
# and the nested frameworks / appex / XPC services) gets
# BundleIsRelocatable=false, so Installer can't "helpfully" redirect
# the payload onto a same-identifier bundle elsewhere on the volume
# (e.g. a dev copy under build/).
echo "[pkg] pkgbuild --analyze"
pkgbuild --analyze --root "$STAGE/root" "$STAGE/components.plist" >/dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$STAGE/components.plist" >/dev/null 2>&1; do
    # Set, else Add: pkgbuild on macOS 27 stopped emitting the key in its
    # analysis (older releases wrote it as true), and Set fails on a
    # missing key — which under set -e killed the 1.2.3 release run.
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$STAGE/components.plist" 2>/dev/null \
        || /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$STAGE/components.plist"
    i=$((i + 1))
done
echo "[pkg] $i bundle entries pinned non-relocatable"

echo "[pkg] pkgbuild (component)"
pkgbuild --root "$STAGE/root" \
         --component-plist "$STAGE/components.plist" \
         --identifier "$PKG_ID" \
         --version "$VERSION" \
         --install-location / \
         --scripts "$STAGE/scripts" \
         "$STAGE/pkgs/UFB-core.pkg" >/dev/null

sed "s/@VERSION@/$VERSION/g" "$PKG_SRC/distribution.xml" > "$STAGE/distribution.xml"

rm -f "$PKG"
if security find-identity -v 2>/dev/null | grep -q "$INSTALLER_IDENTITY"; then
    echo "[pkg] productbuild --sign \"$INSTALLER_IDENTITY\""
    productbuild --distribution "$STAGE/distribution.xml" \
                 --package-path "$STAGE/pkgs" \
                 --sign "$INSTALLER_IDENTITY" --timestamp \
                 "$PKG" >/dev/null
    pkgutil --check-signature "$PKG" | sed 's/^/  /' | head -4
else
    echo "WARN: installer identity not found — building UNSIGNED pkg (local testing only)" >&2
    productbuild --distribution "$STAGE/distribution.xml" \
                 --package-path "$STAGE/pkgs" \
                 "$PKG" >/dev/null
fi

echo "[pkg] done: $PKG ($(du -sh "$PKG" | cut -f1))"
