# Windows session primer — branch `qcview-sync-2026-10` (2026-10-23)

Written on the Mac at the end of the 2026-10-23 session. Everything below
is committed on the branch `qcview-sync-2026-10` (16 commits off `main`'s
1.2.3 appcast commit `170a714`), built and verified on macOS, **none of it
compiled on Windows yet.** The branch is not merged. This is the map for
the Windows session that has to happen before any of it ships.

## 0. What the branch is

Three things stacked: the player caught up with QCView-Player (copies, never
shared — `devdocs/lightbox-preview-plan.md`, changelog entry 2026-10-23), the
family's humanist UI pass with Geist fonts, and the chromeless window with
the tabs as the title bar (the minNotes model). Then two macOS-only
features and one cross-platform one. In order of risk to the Windows build:

| Area | Commits | Windows status |
|---|---|---|
| Chromeless window: `WindowChrome` + Main.qml title row | `97d3ab7` `c498e41` `09e4d60` | **the whole Windows half is unverified (§2)** |
| Audio drain stage / post-seek hold / sample-accurate seeks / servo retune | `d90adb7` `0114d71` | WASAPI device untouched; the deferred stop is period-aware (§3.2) |
| Read-ahead + swscale padded pitch + BT.709 default | `eef6960` | `read_ahead.cpp` has QCView's `windows.h` min/max guard; the padded pitch flows through `VideoSurfaceItem`'s QImage upload |
| LucidLink rows (pin / unpin / copy link / guest link) | `5ff91a3` `7b5f15a` (+ the drive-letter fix in this commit) | CLI discovery under Program Files and `L:\`-style mount points unverified (§3.3) |
| Geist Sans + Geist Mono app-wide | `d9af3e4` | QFontDatabase registration + 9 pt app font; `"Consolas"` literals gone |
| Humanist pass, hover ramps, FlatSlider accent | `72c18a6` `d3c33ac` `9f8bf21` | QML only |
| Native "Show Finder Menu" (`MacShellMenu`) | `e9432f1` | **macOS-only**; Windows keeps the IContextMenu host; `MacAccessibility` deleted |
| exiftool from SourceForge (mac setup script) | `88508c5` | mac script only; Windows copies exiftool from the local tree |

## 1. Setup — nothing new to fetch

No new externals. The Geist `.ttf`s are in the repo (`app/qml/Ufb/App/fonts/`,
QML resources), vcpkg.json is unchanged, FFmpeg is still the 9.0.1 the box
already has. A fresh configure is needed because `app/CMakeLists.txt` lists
new sources (`WindowChrome.{h,cpp}` on both OSes, `WindowChrome_win.cpp` in
the Windows block) and new resources:

```powershell
git fetch; git checkout qcview-sync-2026-10
cmake --preset release; cmake --build build\release
```

`WindowChrome_win.cpp` needs `user32` (already linked). Rust:
`core/src/lucidlink.rs` uses `std::os::windows::process::CommandExt` under
`cfg(windows)` and compiles only there — the first thing to confirm is
`cargo check -p ufb-core` (or just the cmake build, Corrosion runs it).

## 2. The chromeless window on Windows — read before launching

`Main.qml` sets the flags before the window exists:
`Qt.Window | CustomizeWindowHint | WindowSystemMenuHint |
WindowMinMaxButtonsHint | WindowCloseButtonHint | ExpandedClientAreaHint |
NoTitleBarBackgroundHint` (no `WindowTitleHint`). This is exactly minNotes
2.0.2's working recipe (its `app/qml/MinNotes/App/Main.qml` + `WindowChrome*`),
which went through three failed variants first — do not reinvent; diff
against minNotes if something is off. What Qt then does:

- draws and hit-tests the Windows 11 caption buttons at the row's right end
  (Segoe Fluent glyphs, snap-layouts flyout on maximize);
- treats any press on the row that no item accepts as a native caption drag
  (double-click zooms, edge snap works);
- removes the painted top border; the DWM frame keeps shadow + corners.

Our side: the title row (`titleBar` in Main.qml) is `WindowChrome.captionHeight`
tall on Windows (Qt's title-bar height at the window's dpi — 32 at 100 %,
31.2 → 32 at 125 %; `SafeArea.margins.top` is wrong at 125 %, see
`WindowChrome.h`), and reserves `captionWidth` (three buttons of
height × 1.5) at the right through `captionSpacer`, plus a 16 px gap and a
120 px drag reserve the tab strip can never grow into.

**Interplay to check first:** `main.cpp`'s Windows startup leaves the window
hidden, cloaks it (DWMWA_CLOAK), then `showMaximized()` / `show()` after
reading `startMaximized`. The flags are set in QML before that, so creation
should carry them. If the window comes up with a native caption, the flags
did not reach creation — check nothing in main.cpp touches `setFlags`.

**Tabs as drag handles (`DragTabMouseArea`):** the four tab areas arm on
press and call `WindowChrome.startMove(window)` after 6 px of movement
(Qt's `startSystemMove` → `SC_MOVE` on Windows). minNotes reverted its own
*row* handle on Windows because Qt's title-zone hit test synthesized presses
into QML; the tabs are items that accept the press, so this should be a
different case — but it is the one piece with no precedent. If tab clicks
misbehave (double fire, no select, no drag), make the arming
`WindowChrome.nativeLights`-only (macOS) in `DragTabMouseArea.onPressed` and
leave Windows to the native caption on the bare row.

**Lightbox:** `PreviewLightbox` insets its close button by `captionWidth`
so it is not under Qt's caption buttons (which hit-test first).

## 3. What else is new on Windows

### 3.1 Audio (shared code, WASAPI device untouched)
`AudioDrainStage` owns the resampler and every fade; `AudioPlayer::pause()`
fades and stops the device after **three device periods** (`bufferFrameCount
/ sampleRate`, min 30 ms) — WASAPI's first callback can request the whole
buffer, so check the fade plays out and the device really stops (`Audio
Player: hold released …` lines, no `re-seeking` lines, no click on pause).
`close()` stops the device synchronously before swapping decoders.

### 3.2 Read-ahead
`ReadAhead` thread uses `THREAD_MODE_BACKGROUND_BEGIN` on Windows; env
`UFB_READAHEAD_SECONDS=0` turns it off if it misbehaves on a share. Log:
`ReadAhead: … window warm, N MB in …` and `VideoDecoder: playing N fps …
read waits …` every 2 s.

### 3.3 LucidLink
`core/src/lucidlink.rs` finds `lucid3.exe` / `lucid.exe` on PATH, then
`%ProgramFiles%`, `%ProgramFiles(x86)%`, `%LOCALAPPDATA%` under any `Lucid*`
folder (two levels). Mount points come from `lucid3 --id N status` ("Mount
point: L:\" expected — `normalize_mount` trims the trailing separator, and
`locate()` drops the RootDir component so the relative path has no leading
`\`). The REST calls go to `127.0.0.1:<PORT>` from `lucid3 list`. Run
`lucid3 list` and `lucid3 status` in a shell first and compare with the
parser's expectations (tests in the file). Rows: Pin Locally / Unpin / Copy
LucidLink Link / Create LucidLink Guest Link… (the last opens
`lucidlink://create-guest-link?…` through `opener`).

### 3.4 Fonts
`main.cpp` registers the thirteen Geist cuts from the resource path
`:/qt/qml/Ufb/App/qml/Ufb/App/fonts` and sets the app font to Geist 9 pt
(was Segoe UI Variable Display 9). If the window shows a fallback font, the
log says `ufb: no Geist fonts registered`.

## 4. Verification checklist (in the order most likely to break)

1. **Window chrome at 100 % and 125 % scaling:** caption buttons inside the
   row, no painted top border, drag on the bare row, double-click zooms,
   snap layouts on the maximize button, minimize / restore / maximize round
   trip, the persisted maximized state on relaunch (the cloak path).
2. **Tab drag vs click:** click selects, 6 px drag moves the window, the
   close boxes still close, right-click still opens the tab menu.
3. **Lightbox:** close button clear of the caption buttons; Esc / Space.
4. **Audio:** H.264 + AAC and ProRes + PCM: play, pause (no click), resume,
   16 quick Q steps then resume (one `hold released, skipping` line, zero
   `re-seeking`), loop wrap.
5. **Portrait 1080×1920 H.264 with hardware decode off** (the swscale
   overshoot case QCView 2.3.3 fixed): no crash, no garbage pixels at slice
   boundaries.
6. **LucidLink rows** on a filespace path and absent off it; pin → `lucid3
   pin --list` shows it; Copy Link puts an `app.lucidlink.com/l/…` URL on
   the clipboard.
7. **Fonts:** Geist everywhere, Geist Mono in the path bar and date column.
8. **Sidebar bottom rows** (Path swaps, Mesh sync) visible at a short
   window height; the grid-size slider; hover ramps on rows and tabs.

## 5. Mac-only, do not touch

`app/MacShellMenu.{h,mm}`, `app/WindowChrome_mac.mm`, `app/MacAccent.*`,
`app/MacOSBootstrap.*`, the Metal / CoreAudio player files, and the
`Qt.platform.os === "osx"` branch of `showShellMenuAction` (Windows takes
the `FileOps.show_shell_menu` branch to the IContextMenu host; the Rust
macOS arm is a stub).

## 6. Open after Windows

- Listen test on both OSes (the audio changes were verified by log only).
- Merge `qcview-sync-2026-10` → `main`, then the usual bump / release.
- Decision pending: flatten the 31 TextField / 12 CheckBox / 2 ComboBox
  sites still on FluentWinUI3 defaults (QCView made Flat* primitives).
- Pre-existing, both OSes: `QML Dialog: Binding loop detected for property
  "implicitWidth"` from three dialogs (Main.qml close-all confirm, two in
  Sidebar).
