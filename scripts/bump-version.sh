#!/usr/bin/env bash
# bump-version.sh — set the UFB version across every place it's hardcoded.
#
# The version lives in several files that must move together; if they
# desync, the auto-updater's appcast version comparison misfires (the
# appcast sparkle:version must match what the installed app reports).
# macOS UFB.app derives its CFBundleVersion from CMAKE_PROJECT_VERSION,
# but Cargo, the Inno installer, and the macOS helper bundles are
# independent hardcodes. This script keeps them in lockstep.
#
# Usage:  scripts/bump-version.sh <new-version>      e.g. 0.9.9910
#
# Files updated:
#   CMakeLists.txt                          project(VERSION ...)  [source of truth]
#   Cargo.toml                              [workspace.package] version
#   agent/Cargo.toml                        [package] version
#   installer/ufb_installer.iss             #define MyAppVersion
#   agent/macos/Info.plist                  CFBundleShortVersionString
#   macos-helpers/UFBTray/Info.plist        CFBundleShortVersionString
#   macos-helpers/UFBFinderSync/Info.plist  CFBundleShortVersionString

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

NEW="${1:?usage: bump-version.sh <new-version>  e.g. 0.9.9910}"
if ! printf '%s' "$NEW" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "ERROR: version must be X.Y.Z (got '$NEW')" >&2
    exit 2
fi

# Source of truth: the project() VERSION in the root CMakeLists.
OLD="$(grep -E '^[[:space:]]*VERSION [0-9]' CMakeLists.txt | head -1 | awk '{print $2}')"
if [ -z "$OLD" ]; then
    echo "ERROR: could not read current VERSION from CMakeLists.txt" >&2
    exit 1
fi
if [ "$OLD" = "$NEW" ]; then
    echo "Version is already $NEW — nothing to do."
    exit 0
fi

echo "Bumping $OLD -> $NEW"

# Each of these files carries the OLD version only as the app version,
# so an exact-string replace is safe (the string is distinctive).
FILES=(
    CMakeLists.txt
    Cargo.toml
    agent/Cargo.toml
    installer/ufb_installer.iss
    agent/macos/Info.plist
    macos-helpers/UFBTray/Info.plist
    macos-helpers/UFBFinderSync/Info.plist
    vcpkg.json
)

for f in "${FILES[@]}"; do
    if [ ! -f "$f" ]; then
        echo "  WARN: $f missing, skipping" >&2
        continue
    fi
    if grep -q "$OLD" "$f"; then
        # BSD/macOS sed in-place needs the '' arg; this script runs on macOS.
        sed -i '' "s/$OLD/$NEW/g" "$f"
        echo "  updated $f"
    else
        echo "  NOTE: $OLD not found in $f (already bumped?)"
    fi
done

echo ""
echo "Done. Verify, then build:"
echo "  git diff -- ${FILES[*]}"
echo "  scripts/release-mac.sh        # macOS release at $NEW"
