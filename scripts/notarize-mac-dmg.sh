#!/bin/bash
# notarize-mac-dmg.sh — submit a built DMG to Apple's notary service,
# wait for the verdict, then staple the ticket so the DMG works
# offline without macOS phoning home on first launch.
#
# Usage:
#   scripts/notarize-mac-dmg.sh                        # auto-pick newest dist/*.dmg
#   scripts/notarize-mac-dmg.sh dist/UFB-0.9.8.dmg     # explicit path
#
# Prerequisites:
#   - DMG must already be Developer-ID signed (build-mac-dmg.sh
#     does this — it's not optional, notarytool will reject unsigned
#     submissions).
#   - Apple notarytool credentials saved as keychain profile
#     "AC_PASSWORD" (the same profile name the Tauri repo's
#     sign-and-notarize.sh script uses). Set up once via:
#       xcrun notarytool store-credentials AC_PASSWORD \
#           --apple-id <your@email>          \
#           --team-id 5Z4S9VHV56              \
#           --password <app-specific-password>
#
# Notes:
#   - `--wait` blocks ~5–15 minutes typically. Apple's service queue
#     varies; if you want async behaviour, run this script in the
#     background or split into submit + poll.
#   - On notarization failure, the script fetches the submission log
#     and surfaces the rejection reasons. Common causes: hardened
#     runtime missing, an unsigned dylib, an entitlement we can't
#     justify.
#   - Stapling embeds the ticket inside the DMG so subsequent
#     downloads launch fast (no CloudKit roundtrip), even offline.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROFILE="${UFB_NOTARY_PROFILE:-AC_PASSWORD}"

# ── Find the DMG ─────────────────────────────────────────────────────
if [ $# -gt 0 ]; then
    DMG_PATH="$1"
else
    # Newest dist/*.dmg by mtime.
    DMG_PATH="$(ls -t "$REPO_ROOT/dist"/*.dmg 2>/dev/null | head -1 || true)"
fi

if [ -z "$DMG_PATH" ] || [ ! -f "$DMG_PATH" ]; then
    echo "ERROR: no DMG found at $DMG_PATH" >&2
    echo "Run scripts/build-mac-dmg.sh first or pass an explicit path." >&2
    exit 1
fi

echo "=== UFB notarize ==="
echo "DMG:     $DMG_PATH"
echo "Profile: $PROFILE"
echo ""

# ── Sanity-check the keychain profile exists ─────────────────────────
# `notarytool history` is the cheapest call that uses the profile;
# a missing profile fails fast with "credentials not found".
if ! xcrun notarytool history --keychain-profile "$PROFILE" 2>&1 \
        | grep -q -E "id|No submissions"; then
    echo "ERROR: notarytool can't reach Apple with profile \"$PROFILE\"." >&2
    echo "Set up credentials with:" >&2
    echo "  xcrun notarytool store-credentials $PROFILE \\" >&2
    echo "    --apple-id <your@email> --team-id 5Z4S9VHV56 \\" >&2
    echo "    --password <app-specific-password>" >&2
    exit 1
fi

# ── Verify DMG is Developer-ID signed before submitting ──────────────
# notarytool will reject ad-hoc-signed payloads; failing here gives
# a clearer error than waiting 15 min for Apple to say no.
if ! codesign --verify --strict "$DMG_PATH" 2>/dev/null; then
    echo "ERROR: $DMG_PATH isn't validly Developer-ID signed." >&2
    echo "Re-run scripts/build-mac-dmg.sh which signs it." >&2
    exit 1
fi

# ── Submit + wait ────────────────────────────────────────────────────
# `--wait` polls until Apple returns the verdict (Accepted /
# Invalid). Submission ID is parsed out of the output for the log
# lookup on failure.
echo "[1/3] Submitting to Apple notary service..."
echo "       (Apple's service queue varies; expect 5-15 minutes.)"
echo ""

SUBMIT_OUT="$(mktemp -t ufb-notary.XXXXXX)"
trap 'rm -f "$SUBMIT_OUT"' EXIT

if ! xcrun notarytool submit "$DMG_PATH" \
        --keychain-profile "$PROFILE" \
        --wait \
        --output-format json \
        > "$SUBMIT_OUT" 2>&1; then
    echo "ERROR: notarytool submit failed:" >&2
    cat "$SUBMIT_OUT" >&2
    exit 1
fi

# Extract status + submission ID. notarytool's JSON keys are
# `status` and `id`. Accepted = clean ticket. Invalid = rejection
# (we'll fetch the log).
STATUS="$(/usr/bin/python3 -c \
    'import json,sys; d=json.load(open(sys.argv[1])); print(d.get("status",""))' \
    "$SUBMIT_OUT")"
ID="$(/usr/bin/python3 -c \
    'import json,sys; d=json.load(open(sys.argv[1])); print(d.get("id",""))' \
    "$SUBMIT_OUT")"

echo "       Submission $ID → $STATUS"

if [ "$STATUS" != "Accepted" ]; then
    echo ""
    echo "ERROR: notarization not accepted ($STATUS). Apple's log:"
    xcrun notarytool log "$ID" --keychain-profile "$PROFILE" 2>&1 \
        | sed 's/^/    /'
    exit 1
fi

# ── Staple ───────────────────────────────────────────────────────────
# Embeds the notarization ticket inside the DMG. Without this every
# downstream user's first launch still does a CloudKit lookup;
# stapling makes the verdict offline-checkable and shaves 4-ish
# minutes off the first-launch cost.
echo "[2/3] Stapling ticket"
xcrun stapler staple "$DMG_PATH" 2>&1 | sed 's/^/  /'

echo "[3/3] Verifying"
spctl --assess --type open --context context:primary-signature \
    --verbose "$DMG_PATH" 2>&1 | sed 's/^/  /' || {
    echo "WARN: spctl --assess didn't accept the DMG; manual review needed." >&2
}

echo ""
echo "=== Done ==="
echo "Notarized + stapled: $DMG_PATH"
echo "Size: $(du -sh "$DMG_PATH" | cut -f1)"
