#!/bin/bash
# Uninstall UFB — ships inside /Applications/UFB as "Uninstall UFB.command".
# Double-click in Finder (or run from Terminal). Removes the app bundles
# and the installer receipt. Leaves user data in place unless asked.

set -u
echo "UFB uninstaller"
echo
echo "This removes /Applications/UFB (UFB.app, ufb-agent.app) and the"
echo "installer receipt. It will ask for your password."
echo
read -r -p "Continue? [y/N] " yn
case "$yn" in y|Y|yes|YES) ;; *) echo "Cancelled."; exit 0 ;; esac

pkill -x ufb       2>/dev/null || true
pkill -x ufb-agent 2>/dev/null || true
sleep 1

sudo rm -rf "/Applications/UFB"
sudo pkgutil --forget dev.ufb.UFB >/dev/null 2>&1 || true
echo "Removed /Applications/UFB."
echo

echo "Your settings, databases and thumbnail cache are still at:"
echo "  ~/Library/Application Support/ufb"
echo "  ~/Library/Caches/UFB"
echo "  ~/Library/Preferences/dev.ufb.app.plist"
read -r -p "Delete those too? [y/N] " yn2
case "$yn2" in
    y|Y|yes|YES)
        rm -rf "$HOME/Library/Application Support/ufb" \
               "$HOME/Library/Caches/UFB" \
               "$HOME/Library/Caches/dev.ufb.app" \
               "$HOME/Library/Logs/ufb"
        defaults delete dev.ufb.app >/dev/null 2>&1 || true
        echo "User data removed."
        ;;
    *) echo "User data kept." ;;
esac
echo "Done."
