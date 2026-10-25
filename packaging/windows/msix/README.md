# ufb on Windows — the Microsoft Store package ("ufBrowser")

Since 1.4.0, Windows ships only through the **Microsoft Store**. The Store signs the MSIX on ingestion (no
Authenticode certificate, no SmartScreen prompt) and delivers updates, so Windows has no self-updater: WinSparkle
is retired (Windows builds take `Updater_stub.cpp`), `docs/appcast-win.xml` stays frozen at 1.3.3 so old Inno
installs see "no update" rather than an error, and `installer/ufb_installer.iss` is an unpublished fallback only.
macOS is unchanged: signed `.pkg` + Sparkle (`scripts/release-mac.sh`).

Ported from minNotes (`packaging/windows/msix/`); the certification lessons come from minRender
(`installer/STORE.md`).

## Layout

```
packaging/windows/msix/
├── CMakeLists.txt        the ufb_msix / ufb_msix_clean targets + the Partner Center identity
├── AppxManifest.xml.in   the manifest (configure_file @ONLY) — its header comment explains each choice
├── build_msix.ps1        stage → C++ runtime → MakePri → MakeAppx → optional signtool
├── images/               tiles, generated from assets/icons/ufbwindows.svg (below)
└── dist/                 the packed .msix (gitignored)
```

Identity (Partner Center → Product management → Product identity), plain variables in `CMakeLists.txt`:
`cbkow.ufBrowser` / `CN=C64A1D41-EFAA-4EED-AE81-0C074867D4E9` / `cbkow` — the same publisher as minNotes and
QCView (ingestion rejects a mismatch). The Store name is **ufBrowser** ("ufb" was taken); the manifest's
DisplayName must match it. Package family name `cbkow.ufBrowser_k3nw8mfd599r4`, Store ID **9N71KC2RQ65R**
(<https://apps.microsoft.com/detail/9N71KC2RQ65R> once published; headless install:
`winget install --id 9N71KC2RQ65R --source msstore --accept-package-agreements`).
The MSIX version is `PROJECT_VERSION` + `.0`.

## Build

From a VS x64 developer shell (the script copies the MSVC C++ runtime out of `VCToolsRedistDir`), after the usual
release build + deploy (`C:\qcb-lab\ufb-win-release-*.ps1` on the build box: app preset, agent cargo, windeployqt,
DLL copies):

```powershell
cargo build --manifest-path agent\Cargo.toml --release --target x86_64-pc-windows-msvc
cmake --build build\release --target ufb_msix
```

Output: `packaging\windows\msix\dist\ufb-<version>-x64.msix`, **unsigned** — the file Partner Center takes.
The agent comes from `agent\target\x86_64-pc-windows-msvc\release` (the bare `agent\target\release` on the box is
a stale 1.2.3 build the 1.3.x Inno installers picked up by mistake).

## Signed sideload twin (testing before certification)

Sign a COPY with the family's dev certificate (same publisher CN, so it installs as the same package):

```powershell
Copy-Item dist\ufb-<v>-x64.msix dist\ufb-<v>-x64-sideload.msix
signtool sign /fd SHA256 /f <ABSOLUTE path to the dev .pfx> /p <password> /tr http://timestamp.digicert.com /td SHA256 dist\ufb-<v>-x64-sideload.msix
```

(`build_msix.ps1` also signs in place when `UFB_SIGNING_PFX` / `UFB_SIGNING_PASSWORD` / `UFB_TIMESTAMP_URL` are
set.) signtool's PFX import (`0x80090010`) and `Add-AppxPackage` / `Remove-AppxPackage` fail in a plain SSH
session — run them in the logged-on desktop session; from the Mac that means launching a script through the box's
`C:\qcb-lab\run.ps1` desktop runner (the password stays on the box). `C:\qcb-lab\ufb-sideload.ps1` does sign +
install interactively at the box. Uninstall the Inno build first: both register `ufb://` / `union://` and a login entry.

## What the package does and doesn't need

- **Capabilities: `runFullTrust` + `privateNetworkClientServer`** (declarative for a full-trust app; it labels
  the LAN mesh / SMB traffic on the install page — `internetClientServer` was dropped, nothing serves the internet). No
  `unvirtualizedResources` — minRender 0.5.6.0 was denied for it. Everything under `%LOCALAPPDATA%\ufb` (settings,
  `ufb_v5.db`, `mounts.json`, thumbnails, the sync cache) is read only by `ufb.exe` and the `ufb-agent.exe` it
  spawns — both inside the package — so default virtualization is fine. Sync files reach Explorer and other apps
  through the WinFsp drive letter, never the cache path.
- **Virtualization side effect, handled:** on a machine that never had the Inno build, those files physically live
  in `%LOCALAPPDATA%\Packages\<pfn>\LocalCache\Local\ufb`, and uninstall removes them (cache included). The Mount
  Manager's cache **Reveal** button is hidden in the package (`LoginItem.packaged`) for a cache under AppData — Explorer would show an empty folder;
  per-mount **Clear Cache** covers the need. A machine that had the Inno build keeps using the real folder.
- **WinFsp is a user-installed prerequisite.** The Store can't install drivers. Plain SMB mounts and browsing work
  without it; a sync mount that fails `winfsp_init` shows an **Install WinFsp…** pill in the sidebar
  (→ <https://winfsp.dev/rel/>) and retries on its own once it's installed.
- **Protocols:** `ufb://` opens in UFB; `union://` shows the target in Explorer — handled by `ufb.exe`
  (`FileOps.reveal_ufb_uri`) now that a package protocol can't launch PowerShell. A cold `union://` launch starts
  UFB tray-only.
- **Startup task** (`ufbStartup`, on by default after the first launch): starts UFB tray-only at sign-in. It can't
  pass `--background`, so `main.cpp` detects the StartupTask activation. The tray menu's **Start at Login** toggles
  it (`LoginItem`); if the user switched it off in Task Manager, the item opens Settings → Startup instead.
- **Firewall:** mesh control TCP 49221, data TCP 49222, heartbeat UDP 4265, inbound on `ufb.exe`, all profiles.
- **Minimum OS: Windows 10 1809 (10.0.17763)**, like the Inno installer.
- No hidden Application entries and no execution alias (minRender's HeadlessAppBypass refusal).

## Verify on the box (sideload, desktop session)

After any manifest or packaging change:

1. Launches from Start; no `com.unionfiles.ufb` AUMID override (taskbar button groups with the Start tile).
2. Mesh comes up with peers and **no firewall prompt**.
3. A plain SMB mount maps a letter that Explorer sees, and it **comes back after sign-out/in** (persistent WNet
   mappings live in `HKCU\Network` — confirm MSIX doesn't virtualize it).
4. With WinFsp installed, a sync mount's letter is visible in Explorer and other apps; without WinFsp the
   sidebar shows **Install WinFsp…**.
5. No ERROR_UNTRUSTED_MOUNT_POINT (448) on the first traversal of a sync mount (the Inno installer's SmartScreen
   prime is gone).
6. Task Manager → Startup lists "ufb"; after sign-out/in UFB is in the tray with no window. The tray's
   **Start at Login** reflects and changes the task.
7. A `ufb://` link navigates in UFB; a `union://` link reveals in Explorer, both with UFB running and closed
   (closed → it stays in the tray).
8. No "Check for Updates" in the status bar.
9. Uninstall from Start removes the app, startup task, firewall rules and (fresh machine) the LocalCache data.

## Submission

1. Build the unsigned MSIX from the release commit; Defender-scan it on the box
   (`MpCmdRun.exe -Scan -ScanType 3 -File <msix> -DisableRemediation`).
2. Partner Center → ufBrowser → new submission: upload the `.msix`, choose **Publish this submission manually**.
   Only one submission can be in flight per app.
3. **License terms:** supply the GPL-3.0 as the app's own license terms (ufb is GPL-3.0-or-later and bundles GPL
   ffmpeg); minNotes is GPL-3.0 too and certified.
4. Certification typically takes 1–3 business days. A denied package gets a version bump before resubmitting.
5. Listing basics: website <https://ufbrowser.com>, support <https://github.com/cbkow/ufb/issues>, privacy policy
   (needed — the app uses the network: a page on ufbrowser.com stating ufb collects nothing and talks only to the
   user's own LAN peers and shares).

### Listing text (draft)

Policy 10.2.4 wants a dependency on non-integrated software disclosed at the *start* of the description:

> **Sync mounts require the free WinFsp driver (winfsp.dev), installed separately. Browsing and network-share
> mounts work without it.** ufb is a file browser and project manager for visual-effects and post-production
> teams: dual-pane browsing of local and network shares, job and shot templates, per-file tracking columns shared
> live with teammates on the LAN, fast previews of images, EXR, video, PDF and documents, and offline-capable sync
> mounts for remote work.

### Notes for certification (draft)

```
ufb is a desktop file browser (Qt/C++ and Rust). runFullTrust: it is a Win32 application that browses and opens
user files anywhere on disk or network shares, maps network drives, and starts its own helper (ufb-agent.exe, in
the package) for sync mounts.

To test without a studio network: launch ufb, browse any local folder (dual-pane browser, previews with Space).
Sync mounts need the free WinFsp driver (winfsp.dev), disclosed at the start of the description; without it the
sidebar offers an "Install WinFsp" link and the rest of the app works normally.

Expected behavior: closing the window keeps ufb in the system tray (quit via tray > Quit UFB). A startup task
starts it in the tray at sign-in; users turn it off in the tray menu (Start at Login), Settings > Apps > Startup
or Task Manager. Declared firewall rules (TCP 49221-49222, UDP 4265) let ufb exchange metadata with teammates'
copies on the local network without a prompt.

Uninstall removes the app, its data, the startup task and the firewall rules.
```

## Per-release routine

1. Bump with `scripts/bump-version.sh` (the MSIX version follows `project(UFB VERSION …)`).
2. macOS: the usual `scripts/release-mac.sh` + Sparkle appcast (`scripts/make-appcast.sh` without the win exe).
3. Windows: release build + `ufb_msix`, sideload-smoke-test if the release touched packaging, Defender-scan,
   submit, publish when certified. No `appcast-win.xml` update.

## Regenerating the tiles

```sh
magick -background none -density 1200 assets/icons/ufbwindows.svg -resize 1024x1024 /tmp/ufb1024.png
D=packaging/windows/msix/images
for sz in 16 24 32 44 48 256; do
  magick /tmp/ufb1024.png -resize ${sz}x${sz} $D/Square44x44Logo.targetsize-${sz}.png
  cp $D/Square44x44Logo.targetsize-${sz}.png $D/Square44x44Logo.targetsize-${sz}_altform-unplated.png
done
magick /tmp/ufb1024.png -resize 50x50   $D/StoreLogo.png
magick /tmp/ufb1024.png -resize 150x150 $D/Square150x150Logo.png
magick /tmp/ufb1024.png -resize 44x44   $D/Square44x44Logo.png
magick /tmp/ufb1024.png -resize 150x150 -background none -gravity center -extent 310x150 $D/Wide310x150Logo.png
```
