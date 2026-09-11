# Auto-update setup (Sparkle + WinSparkle)

UFB's in-app updater checks an appcast served by GitHub Pages at
`https://ufbrowser.com/` (the `docs/` folder of this repo, same setup
as the minNotes sister site). macOS uses **Sparkle**; Windows uses
**WinSparkle**. Both verify an **ed25519** signature (same keypair).
Release binaries live on **GitHub Releases** — the appcast items point
at `github.com/cbkow/ufb/releases/download/v<ver>/...`.

> **Both OSes are full signed downloads since 1.2.0.** macOS ships a
> signed + notarized installer **pkg** (`scripts/build-mac-pkg.sh`,
> Developer ID *Installer* identity); the appcast item carries the pkg
> as its enclosure with an ed25519 `sparkle:edSignature`, and Sparkle
> runs a guided package install (admin password prompt per update —
> the price of a root-owned /Applications/UFB layout) then relaunches.
> Through 1.1.6 the macOS item was *informational* (no enclosure, a
> button opening the multi-bundle DMG) because Sparkle can't swap a
> drag-install; those items stay informational in the feed history.
> 1.1.6 clients already carry `SUPublicEDKey` + Sparkle 2.9.2, so the
> first pkg release upgrades them automatically. **Windows**:
> WinSparkle downloads `UFB-<ver>-x64.exe`, verifies the ed25519
> signature, runs it, relaunches.

## The signing key (already provisioned)

The ed25519 **private key lives in the login Keychain only** — it is
never in this repo. The public half is the build default in
`CMakeLists.txt`:

```
+Xlrad3WswxIRTyzBTp8KaTEjyfPO5Ap4llmPdZf3RQ=
```

To confirm/recover the public key any time:
`external/Sparkle/bin/generate_keys -p`.

## One-time: vendor the frameworks

**macOS** — vendor Sparkle 2.9.x at `external/Sparkle/`:

```sh
# -> external/Sparkle/Sparkle.framework  (+ bin/sign_update, bin/generate_keys)
```

**Windows** — run the setup script (downloads WinSparkle into place):

```powershell
scripts/setup-external.ps1
# -> external/winsparkle/{include/winsparkle.h, lib/WinSparkle.lib, bin/WinSparkle.dll}
```

If a framework is absent the app still builds — the updater compiles to
a no-op and the "Check for Updates" button hides (`Updater.available`
is false). So you can build without it; the updater activates once
vendored.

## Build

The public key is already the default, so no extra flags are needed:

```sh
# macOS
QMAKE=~/Qt/6.11.1/macos/bin/qmake cmake . -B build/mac-release
```
```powershell
# Windows
cmake --preset release
```

(Override with `-DUFB_UPDATE_PUBLIC_KEY=<base64>` only if you ever rotate
the key.) The key is substituted into `app/Info.plist.in` as
`SUPublicEDKey` (macOS) and compiled into `Updater_win.cpp` (Windows).
The feed URLs (`https://ufbrowser.com/appcast-{mac,win}.xml`) are baked
in; being HTTPS, no ATS exception is needed.

## Release + publish

**macOS** — `scripts/release-mac.sh` builds, signs (incl. Sparkle's
nested helpers — see `sign-mac-dev.sh`), notarizes + staples the two
bundles, builds the pkg, notarizes + staples that, and inserts the
signed enclosure into `docs/appcast-mac.xml`. Then:

```sh
gh release create v<ver> dist/UFB-<ver>-arm64.pkg --title "UFB <ver>"
git add docs/appcast-mac.xml && git commit -m "appcast: <ver> (mac)" && git push
```

**Windows** — build + run the Inno installer (`installer/ufb_installer.iss`)
to produce `UFB-<ver>-x64.exe`, upload it to the same `v<ver>` GitHub
release, then sign + insert the Windows item (run on the Mac, where the
private key lives — copy the exe over or use a shared path):

```sh
scripts/make-appcast.sh <ver> dist/UFB-<ver>-arm64.pkg /path/to/UFB-<ver>-x64.exe
# (sign_update is found on PATH or in ../QCView-Player/external/Sparkle/bin;
#  SPARKLE_BIN=... overrides. Re-running re-signs the mac pkg too — the
#  sentinel insert is idempotent per version.)
# -> inserts into docs/appcast-win.xml (enclosure + sparkle:edSignature)
git add docs/appcast-win.xml && git commit -m "appcast: <ver> (win)" && git push
```

The push is the publish — GitHub Pages redeploys ufbrowser.com with the
updated feeds. Asset basenames in the appcast must match the names
uploaded to the release exactly.

## Hosting layout

```
https://ufbrowser.com/               (GitHub Pages <- docs/)
├── appcast-mac.xml                  (enclosure + edSignature -> pkg on Releases)
└── appcast-win.xml                  (enclosure + edSignature -> Releases)

https://github.com/cbkow/ufb/releases/download/v<ver>/
├── UFB-<ver>-arm64.pkg              (Developer ID Installer, notarized, ed25519-signed)
└── UFB-<ver>-x64.exe                (ed25519-signed)
```

## Verify

- macOS: launch UFB, click **Check for Updates** (bottom status bar).
  With a higher version in `appcast-mac.xml`, Sparkle shows the notice +
  release notes, downloads the pkg, verifies the signature, asks for an
  admin password, installs, relaunches. To rehearse before pushing the
  feed: `defaults write dev.ufb.app SUFeedURL file:///abs/path/docs/appcast-mac.xml`
  on a machine with the previous version installed, check, then
  `defaults delete dev.ufb.app SUFeedURL`.
- Windows: same button → WinSparkle downloads the exe, **verifies the
  signature**, runs the installer, relaunches. Tamper the exe (or use a
  wrong key) → WinSparkle refuses to install.

## Migration note (pre-1.0.8 installs)

Builds ≤1.0.7 poll the old internal web-root URL, not ufbrowser.com.
To move existing machines onto the new feed, publish one final appcast
at the old location whose newest item points at the first
ufbrowser.com-aware build; after they take that update, the old web
root can be retired.
