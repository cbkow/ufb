#!/usr/bin/env bash
# fetch-sparkle-mac.sh — put the pinned Sparkle release in external/Sparkle.
#
# Sparkle is not a submodule: the binary release carries the framework, the
# XPC services and the tools (generate_keys, sign_update, generate_appcast)
# that release-mac.sh / make-appcast.sh use. The same pin and SHA-256 as the
# sister repos (QCView-Player scripts/fetch_sparkle.sh); the hash is checked
# before anything is unpacked — an update framework fetched over the network
# without one is a supply-chain hole.
#
# A sibling checkout that already holds this exact version is copied instead
# of downloaded (../QCView-Player/external/Sparkle), which is how the family
# has always shared it.
#
# Usage: scripts/fetch-sparkle-mac.sh [--force]
set -euo pipefail
VERSION="2.9.2"
SHA256="1cb340cbbef04c6c0d162078610c25e2221031d794a3449d89f2f56f4df77c95"
URL="https://github.com/sparkle-project/Sparkle/releases/download/$VERSION/Sparkle-$VERSION.tar.xz"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO/external/Sparkle"
SIBLING="$REPO/../QCView-Player/external/Sparkle"

have_version() {
    local plist="$1/Sparkle.framework/Resources/Info.plist"
    [ -f "$plist" ] && [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$plist" 2>/dev/null)" = "$VERSION" ]
}

if [ "${1:-}" != "--force" ] && have_version "$DEST"; then
    echo "[sparkle] $VERSION already in $DEST (use --force to refetch)"
    exit 0
fi
if [ "${1:-}" != "--force" ] && have_version "$SIBLING"; then
    echo "[sparkle] copying $VERSION from $SIBLING"
    rm -rf "$DEST"; mkdir -p "$DEST"
    ditto "$SIBLING" "$DEST"
    exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "[sparkle] downloading Sparkle $VERSION"
curl -fsSL -o "$TMP/sparkle.tar.xz" "$URL"
GOT="$(shasum -a 256 "$TMP/sparkle.tar.xz" | awk '{print $1}')"
if [ "$GOT" != "$SHA256" ]; then
    echo "[sparkle] SHA-256 mismatch for Sparkle $VERSION — not using it" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi
rm -rf "$DEST"; mkdir -p "$DEST"
tar xJf "$TMP/sparkle.tar.xz" -C "$DEST"
echo "[sparkle] Sparkle $VERSION in $DEST"
