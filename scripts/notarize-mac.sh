#!/usr/bin/env bash
# notarize-mac.sh — submit to Apple's notary service, wait, staple.
#
#   scripts/notarize-mac.sh dist/UFB-1.2.0-arm64.pkg
#   scripts/notarize-mac.sh build/mac-release/app/ufb.app agent/target/release/ufb-agent.app
#
# Accepts ONE .pkg / .dmg, or ONE OR MORE .app bundles. Bundles are
# zipped together (ditto, one submission) and each is stapled in
# place afterwards — so the apps inside the pkg validate offline at
# first launch instead of Gatekeeper fetching tickets. The pkg itself
# is then notarized + stapled separately (its own ticket covers the
# installer flat file).
#
# Requirements:
#   - Everything already Developer-ID signed with the hardened runtime
#     (scripts/sign-mac-dev.sh / build-mac-pkg.sh).
#   - notarytool keychain profile (default AC_PASSWORD, override with
#     UFB_NOTARY_PROFILE):
#       xcrun notarytool store-credentials AC_PASSWORD \
#         --apple-id <you@email> --team-id 5Z4S9VHV56 --password <app-pw>
#
# Apple's queue is the dominant cost: 2-15 minutes per submission.
# On "Invalid" the full log is printed (unsigned nested binary, missing
# timestamp, wrong entitlement) so the fix is obvious.

set -euo pipefail

PROFILE="${UFB_NOTARY_PROFILE:-AC_PASSWORD}"
[ $# -ge 1 ] || { echo "usage: notarize-mac.sh <file.pkg|file.dmg|app... >" >&2; exit 2; }

if ! xcrun notarytool history --keychain-profile "$PROFILE" >/dev/null 2>&1; then
    echo "ERROR: notarytool can't reach Apple with profile \"$PROFILE\" (see header)." >&2
    exit 1
fi

TMP="$(mktemp -d -t ufb-notary)"
trap 'rm -rf "$TMP"' EXIT

MODE=""
SUBMIT=""
for p in "$@"; do
    [ -e "$p" ] || { echo "ERROR: not found: $p" >&2; exit 1; }
    case "$p" in
        *.app)          [ -z "$MODE" ] || [ "$MODE" = app ] || { echo "ERROR: mix of bundles and flat files" >&2; exit 1; }; MODE=app ;;
        *.pkg|*.dmg)    [ -z "$MODE" ] || { echo "ERROR: one flat file per run" >&2; exit 1; }; MODE=flat; SUBMIT="$p" ;;
        *) echo "ERROR: unsupported: $p" >&2; exit 1 ;;
    esac
    codesign --verify --strict "$p" >/dev/null 2>&1 \
        || pkgutil --check-signature "$p" >/dev/null 2>&1 \
        || { echo "ERROR: $p is not validly signed" >&2; exit 1; }
done

if [ "$MODE" = app ]; then
    SUBMIT="$TMP/bundles.zip"
    echo "[notary] zipping $# bundle(s) for one submission"
    # --keepParent keeps each bundle as a top-level dir in the zip;
    # multiple -c invocations can't append, so build a staging dir.
    mkdir -p "$TMP/stage"
    for p in "$@"; do ditto "$p" "$TMP/stage/$(basename "$p")"; done
    ditto -c -k --keepParent "$TMP/stage" "$SUBMIT" 2>/dev/null \
        || ditto -c -k "$TMP/stage" "$SUBMIT"
fi

echo "[notary] submitting $(basename "$SUBMIT") (profile $PROFILE) — Apple's queue: 2-15 min"
OUT="$TMP/submit.json"
if ! xcrun notarytool submit "$SUBMIT" --keychain-profile "$PROFILE" --wait \
        --output-format json > "$OUT" 2>&1; then
    echo "ERROR: notarytool submit failed:" >&2; cat "$OUT" >&2; exit 1
fi
STATUS="$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("status",""))' "$OUT")"
ID="$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("id",""))' "$OUT")"
echo "[notary] submission $ID → $STATUS"
if [ "$STATUS" != "Accepted" ]; then
    echo "ERROR: notarization not accepted ($STATUS). Apple's log:" >&2
    xcrun notarytool log "$ID" --keychain-profile "$PROFILE" 2>&1 | sed 's/^/    /' >&2
    exit 1
fi

echo "[notary] stapling"
for p in "$@"; do
    xcrun stapler staple -q "$p" && xcrun stapler validate -q "$p" \
        && echo "  stapled: $p" \
        || { echo "ERROR: staple/validate failed for $p" >&2; exit 1; }
done

case "$SUBMIT" in
    *.pkg) spctl --assess --type install --verbose=2 "$SUBMIT" 2>&1 | sed 's/^/  /' ;;
    *.dmg) spctl --assess --type open --context context:primary-signature --verbose "$SUBMIT" 2>&1 | sed 's/^/  /' ;;
    *)     for p in "$@"; do spctl --assess --type execute --verbose "$p" 2>&1 | sed 's/^/  /'; done ;;
esac
echo "[notary] done"
