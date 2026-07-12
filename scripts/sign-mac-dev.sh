#!/bin/bash
# sign-mac-dev.sh — Developer ID sign UFB.app (+ embedded FinderSync
# appex) and ufb-agent.app
# for local dogfood. No notarization (that's the release script).
#
# Why this exists: ad-hoc (`codesign --sign -`) signing changes the
# cdhash on every build, so macOS TCC re-prompts for Downloads /
# Desktop / Files / Folders access on every launch. A stable Developer
# ID identity + the entitlements files in this repo fix that.
#
# Usage:
#   scripts/sign-mac-dev.sh                    # signs all three
#   scripts/sign-mac-dev.sh --only-app         # only UFB.app
#   scripts/sign-mac-dev.sh --only-agent       # only ufb-agent
#
# Prerequisites:
#   1. CMake build of UFB has run:
#        build/mac-debug/app/ufb.app/Contents/MacOS/ufb
#   2. Cargo release build of agent has run:
#        agent/target/release/ufb-agent  (or .../debug/ufb-agent)
#   4. Developer ID Application cert is in the user's keychain:
#        security find-identity -v -p codesigning
#

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIGNING_IDENTITY="${UFB_SIGNING_IDENTITY:-Developer ID Application: Christopher Bialkowski (5Z4S9VHV56)}"

# Pick the build flavour to sign. Mirrors build-mac-dmg.sh and the
# release-mac.sh wrapper — set UFB_BUILD_PRESET=mac-release for a
# notarisable Release build, default mac-debug for dogfood.
PRESET="${UFB_BUILD_PRESET:-mac-debug}"
case "$PRESET" in
    mac-debug)   CMAKE_DIR="build/mac-debug";   XCODE_CFG="Debug"   ;;
    mac-release) CMAKE_DIR="build/mac-release"; XCODE_CFG="Release" ;;
    *) echo "ERROR: unknown UFB_BUILD_PRESET=$PRESET (use mac-debug or mac-release)" >&2; exit 2 ;;
esac

UFB_APP="$REPO_ROOT/$CMAKE_DIR/app/ufb.app"

# Agent binary is selected per-preset too — release pipeline must
# sign the optimized binary, dogfood signs whatever cargo produced.
AGENT_BIN_RELEASE="$REPO_ROOT/agent/target/release/ufb-agent"
AGENT_BIN_DEBUG="$REPO_ROOT/agent/target/debug/ufb-agent"

FINDER_SYNC_ENTITLEMENTS="$REPO_ROOT/macos-helpers/UFBFinderSync/UFBFinderSync.entitlements"

# Bundle icon. All three apps reference `CFBundleIconFile = UFB`, so
# each `.app/Contents/Resources/UFB.icns` is the canonical location.
# Sourced from `app/icons/icon.icns`.
ICON_ICNS="$REPO_ROOT/app/icons/icon.icns"

# Copy the bundle icon into a target's Resources/ as UFB.icns so
# Finder / Activity Monitor / spctl all show the same icon. No-op
# when the source file is missing (logs a warning so the developer
# notices).
embed_icon() {
    local bundle="$1"
    local resources="$bundle/Contents/Resources"
    if [ ! -f "$ICON_ICNS" ]; then
        echo "  WARN: $ICON_ICNS missing — bundle icon not embedded" >&2
        return
    fi
    mkdir -p "$resources"
    cp "$ICON_ICNS" "$resources/UFB.icns"
}

UFB_ENTITLEMENTS="$REPO_ROOT/app/UFB.entitlements"
AGENT_ENTITLEMENTS="$REPO_ROOT/agent/ufb-agent.entitlements"

ONLY=""
case "${1:-}" in
    --only-app)   ONLY="app" ;;
    --only-agent) ONLY="agent" ;;
    "")           ONLY="" ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
esac

# Verify cert is reachable before we strip any signatures.
if ! security find-identity -v -p codesigning | grep -q "$SIGNING_IDENTITY"; then
    echo "ERROR: signing identity not found in keychain:" >&2
    echo "  $SIGNING_IDENTITY" >&2
    echo "Available identities:" >&2
    security find-identity -v -p codesigning >&2
    exit 1
fi

echo "=== UFB Developer ID dev sign ==="
echo "Identity: $SIGNING_IDENTITY"
echo ""

sign() {
    local target="$1"
    local entitlements="${2:-}"
    if [ -n "$entitlements" ]; then
        codesign --force --sign "$SIGNING_IDENTITY" \
            --timestamp --options runtime \
            --entitlements "$entitlements" \
            "$target"
    else
        codesign --force --sign "$SIGNING_IDENTITY" \
            --timestamp --options runtime \
            "$target"
    fi
}

# ── Agent ────────────────────────────────────────────────────────────
# Wraps the cargo-built bare binary in a real `ufb-agent.app` bundle
# (Info.plist + PkgInfo per macOSplans/08), then signs the bundle.
# Production layout puts ufb-agent.app as a sibling of UFB.app under
# /Applications/UFB/; staging the .app at build time lets the dev
# locators (mount.rs, AgentProcess.swift, CredentialPrompt_mac.mm)
# find the same path shape they'll see post-install, so we test what
# we ship.
if [ -z "$ONLY" ] || [ "$ONLY" = "agent" ]; then
    echo "[agent]"
    if [ -x "$AGENT_BIN_RELEASE" ] && [ "$AGENT_BIN_RELEASE" -nt "$AGENT_BIN_DEBUG" ]; then
        AGENT_BIN="$AGENT_BIN_RELEASE"
    elif [ -x "$AGENT_BIN_DEBUG" ]; then
        AGENT_BIN="$AGENT_BIN_DEBUG"
    elif [ -x "$AGENT_BIN_RELEASE" ]; then
        AGENT_BIN="$AGENT_BIN_RELEASE"
    else
        echo "  ERROR: ufb-agent binary not found (run cargo build)" >&2
        exit 1
    fi

    AGENT_APP="$(dirname "$AGENT_BIN")/ufb-agent.app"
    AGENT_INFO="$REPO_ROOT/agent/macos/Info.plist"
    AGENT_PKG="$REPO_ROOT/agent/macos/PkgInfo"

    echo "  staging $AGENT_BIN → $AGENT_APP"
    rm -rf "$AGENT_APP"
    mkdir -p "$AGENT_APP/Contents/MacOS"
    mkdir -p "$AGENT_APP/Contents/Resources"
    cp "$AGENT_INFO" "$AGENT_APP/Contents/Info.plist"
    cp "$AGENT_PKG"  "$AGENT_APP/Contents/PkgInfo"
    cp "$AGENT_BIN"  "$AGENT_APP/Contents/MacOS/ufb-agent"

    # Re-sign the inner binary first, then the bundle.
    codesign --remove-signature "$AGENT_APP/Contents/MacOS/ufb-agent" 2>/dev/null || true
    codesign --force --sign "$SIGNING_IDENTITY" \
        --timestamp --options runtime \
        "$AGENT_APP/Contents/MacOS/ufb-agent"
    codesign --remove-signature "$AGENT_APP" 2>/dev/null || true
    embed_icon "$AGENT_APP"
    sign "$AGENT_APP" "$AGENT_ENTITLEMENTS"

    # The cargo-built bare binary at $AGENT_BIN stays in place
    # untouched — locators that prefer the bundle will find the .app
    # sibling, and the bare binary remains as a fallback path for
    # any flow that hasn't migrated yet.
fi

# (UFBTray deleted 1.0.7 — the FinderSync appex now lives inside
#  UFB.app/Contents/PlugIns and is signed in the [app] section below.)

# ── Main app ─────────────────────────────────────────────────────────
if [ -z "$ONLY" ] || [ "$ONLY" = "app" ]; then
    echo "[app]"
    if [ ! -d "$UFB_APP" ]; then
        echo "  ERROR: $UFB_APP not found (run cmake build)" >&2
        exit 1
    fi
    echo "  $UFB_APP"

    # Inside-out: every dylib + framework + plugin first, bundle last.
    # macdeployqt left ad-hoc signatures all over Contents/Frameworks/
    # and Contents/PlugIns/; replace each with our Developer ID before
    # the outer bundle sign so codesign --verify --deep doesn't reject
    # the result.
    while IFS= read -r -d '' f; do
        codesign --remove-signature "$f" 2>/dev/null || true
        codesign --force --sign "$SIGNING_IDENTITY" \
            --timestamp --options runtime "$f"
    done < <(find "$UFB_APP/Contents/Frameworks" "$UFB_APP/Contents/PlugIns" \
                  -type f \( -name "*.dylib" -o -name "*.framework" \) \
                  -print0 2>/dev/null)

    # FinderSync appex (embedded by release-mac.sh since 1.0.7): its
    # inner dylibs were covered by the PlugIns find above; sign the
    # appex bundle itself (seals its main executable) with the
    # FinderSync entitlements BEFORE the outer bundle sign seals over
    # PlugIns/. Absent in plain dev builds — skip silently.
    FS_APPEX="$UFB_APP/Contents/PlugIns/UFBFinderSync.appex"
    if [ -d "$FS_APPEX" ]; then
        codesign --remove-signature "$FS_APPEX" 2>/dev/null || true
        sign "$FS_APPEX" "$FINDER_SYNC_ENTITLEMENTS"
        echo "    UFBFinderSync.appex (in UFB.app)"
    fi

    # Sparkle.framework (Phase 2 auto-update) ships nested helper code
    # the generic *.dylib pass above doesn't reach: the main binary
    # `Sparkle` has no extension, and Autoupdate / Updater.app /
    # XPCServices/*.xpc are executables + bundles. Notarization rejects
    # any nested Mach-O without Developer ID + hardened runtime + secure
    # timestamp, so sign them inside-out BEFORE the framework-unit pass
    # below seals Sparkle.framework. (Untested in this dev env — built
    # without the framework; verify against the vendored Sparkle's exact
    # Versions/ layout on the first real release build.)
    SPARKLE_FW="$UFB_APP/Contents/Frameworks/Sparkle.framework"
    if [ -d "$SPARKLE_FW" ]; then
        echo "  Sparkle.framework nested code (inside-out)"
        # Sign specific nested targets DEEPEST-FIRST: XPC services, then
        # Updater.app, then the Autoupdate CLI. The framework-unit pass
        # below signs the `Sparkle` binary + the framework itself. Do NOT
        # broadly find-and-sign every executable — that would descend
        # into the just-signed .xpc/.app and break their seals. This
        # mirrors QCView-Player's proven sign order for Sparkle 2.9.2.
        # --force replaces Sparkle's upstream signatures (no separate
        # --remove-signature needed).
        while IFS= read -r -d '' x; do
            codesign --force --sign "$SIGNING_IDENTITY" \
                --timestamp --options runtime "$x"
        done < <(find "$SPARKLE_FW"/Versions/*/XPCServices -maxdepth 1 \
                      -name "*.xpc" -type d -print0 2>/dev/null)
        while IFS= read -r -d '' a; do
            codesign --force --sign "$SIGNING_IDENTITY" \
                --timestamp --options runtime "$a"
        done < <(find "$SPARKLE_FW"/Versions/* -maxdepth 1 \
                      -name "Updater.app" -type d -print0 2>/dev/null)
        while IFS= read -r -d '' u; do
            codesign --force --sign "$SIGNING_IDENTITY" \
                --timestamp --options runtime "$u"
        done < <(find "$SPARKLE_FW"/Versions/* -maxdepth 1 \
                      -name "Autoupdate" -type f -print0 2>/dev/null)
    fi

    # Each *.framework directory gets its own pass — codesign
    # treats it as a unit, not via dylib symlinks inside.
    while IFS= read -r -d '' fw; do
        codesign --remove-signature "$fw" 2>/dev/null || true
        codesign --force --sign "$SIGNING_IDENTITY" \
            --timestamp --options runtime "$fw"
    done < <(find "$UFB_APP/Contents/Frameworks" -maxdepth 1 \
                  -type d -name "*.framework" -print0 2>/dev/null)

    # Transcode subprocess Mach-Os (ffmpeg, ffprobe) sit in
    # Contents/MacOS/ next to the main `ufb` binary. Notarisation
    # rejects any Mach-O without a Developer ID + secure timestamp +
    # hardened runtime, so sign them individually before the outer
    # bundle seal. Skip the main executable (the outer-bundle sign
    # carries its entitlements). exiftool ships under Resources/
    # as a Perl script — text, not Mach-O, no signing needed.
    while IFS= read -r -d '' bin; do
        if [ "$(basename "$bin")" = "ufb" ]; then continue; fi
        # Filter to actual Mach-O files (skip text shims if any).
        if file "$bin" 2>/dev/null | grep -q "Mach-O"; then
            codesign --remove-signature "$bin" 2>/dev/null || true
            codesign --force --sign "$SIGNING_IDENTITY" \
                --timestamp --options runtime "$bin"
        fi
    done < <(find "$UFB_APP/Contents/MacOS" -maxdepth 1 -type f -print0 2>/dev/null)

    # Outer bundle last — entitlements + hardened runtime in one shot.
    codesign --remove-signature "$UFB_APP" 2>/dev/null || true
    embed_icon "$UFB_APP"
    sign "$UFB_APP" "$UFB_ENTITLEMENTS"
fi

# ── Verify ───────────────────────────────────────────────────────────
echo ""
echo "=== verify ==="
if [ -z "$ONLY" ] || [ "$ONLY" = "app" ]; then
    codesign --verify --deep --strict --verbose=1 "$UFB_APP" 2>&1 \
        | sed 's/^/  app: /'
fi
if [ -z "$ONLY" ] || [ "$ONLY" = "agent" ]; then
    codesign --verify --deep --strict --verbose=1 "$AGENT_APP" 2>&1 \
        | sed 's/^/  agent: /'
fi
echo ""
echo "Done."
