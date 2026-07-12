# UFB — Union File Browser

A native Qt + Rust file browser and project-management tool built for
visual-effects and post-production workflows. UFB mounts NAS shares
through a user-mode VFS (WinFsp on Windows, FileProvider / NFS loopback
on macOS), renders fast thumbnails for production media (PSD / EXR /
HDR / PDF / AI / video) directly from native C++ libraries, and tracks
job + shot metadata in a local SQLite store synced across nodes via a
mesh data plane.

Ships on **macOS** (Apple Silicon, notarized DMG) and **Windows x64**
(Inno Setup installer), with in-app auto-update on both. Website +
downloads: [ufbrowser.com](https://ufbrowser.com) · releases:
[GitHub Releases](https://github.com/cbkow/ufb/releases).

This is the Qt port of the original Tauri build. The numbered
port phases (and the macOS port that followed) are complete; current
work is incremental polish under the **1.0.x** series.

## What's in the box

| Surface | Stack |
|---|---|
| App shell, file browser, sidebar, dialogs | Qt 6.11 / QML / Qt Quick Controls |
| Cross-OS path handling, settings, mesh sync, project DB | Rust core (`core/`) |
| Rust↔Qt bridge (cxx-qt QObjects exposed to QML) | `bindings/` |
| NAS mount + VFS + credential store + SMB session | Standalone Rust agent (`agent/`) launched as a child process |
| Native thumbnails (PSD/EXR/PDF/video/system icons) | C++ in `app/thumbnails/` (psd_sdk, OpenEXR, PDFium, FFmpeg) |
| Native credential prompt + store | Win32 `CredUIPromptForWindowsCredentialsW` (`app/CredentialPrompt.cpp`) / macOS Keychain + NetFS auth |

## Requirements

- **Qt 6.11.1** — default install path on Windows is
  `C:\Qt\6.11.1\msvc2022_64`. `qmake` must be findable by CMake (and
  exposed as `$env:QMAKE` for cxx-qt-build).
- **Rust stable** (edition 2021, MSRV pinned in
  `Cargo.toml::workspace.package.rust-version`).
- **CMake 3.24+** with **Ninja**.
- **MSVC 2022** (Visual Studio 17.x or 18.x) on Windows / **Xcode
  Command Line Tools** on macOS.
- **WinFsp 2.x Developer SDK** on Windows for the agent build (headers
  + import lib at `C:\Program Files (x86)\WinFsp\{inc,lib}\`). The
  installer bundles the WinFsp redistributable for end users — see
  [`installer/README.md`](installer/README.md).
- **LLVM / `libclang`** on Windows for `winfsp-sys`'s bindgen step.
  Set `LIBCLANG_PATH` or put `clang.exe` on `PATH`.
- **vcpkg** for OpenEXR (manifest mode — `vcpkg.json` drives it).

## Quick start

```bash
just build       # dev build (RelWithDebInfo) into build/debug
just run         # build + launch
just release     # release build into build/release
just agent       # build the agent crate (separate workspace)
just test        # run all Rust tests
just clean-all   # wipe build artifacts
```

Install `just` with `cargo install just` if you don't have it.

### Why `RelWithDebInfo`, not `Debug`

The Rust MSVC stdlib always links against the release CRT (`/MD`).
Building C++ in `Debug` (`/MDd`) produces unresolved-CRT-symbol link
errors when linking against the Rust-produced static library. Use
`RelWithDebInfo` for dev — it keeps debug symbols and matches the CRT
flag Rust enforces. Both presets in `CMakePresets.json` honor this.

### Building outside `just`

```powershell
$env:QMAKE = 'C:/Qt/6.11.1/msvc2022_64/bin/qmake.exe'
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake --preset release && cmake --build build\release'

# Agent is a separate cargo workspace
cd agent && cargo build --release
```

To launch the result, prepend Qt's `bin/` to `PATH` so the runtime
DLLs resolve:

```bash
PATH="/c/Qt/6.11.1/msvc2022_64/bin:$PATH" build/release/ufb.exe
```

## Repo layout

```
agent/         ufb-agent — standalone Rust binary; sync VFS host + mesh
app/           Qt executable (C++): main, image providers, thumbnail backends, updater, lightbox
app/qml/       QML UI tree (Main, Sidebar, FileBrowser, MountManagerDialog, …)
bindings/      Rust↔Qt bridge (cxx-qt QObjects, IPC client, services exposed to QML)
core/          Pure Rust core (Qt-unaware): settings, mesh, mount client, file ops, DB
devdocs/       Engineering docs (auto-update setup, subsystem audits, feature plans)
docs/          ufbrowser.com — GitHub Pages site + the live Sparkle/WinSparkle appcasts
external/      Vendored prebuilts (ffmpeg, pdfium, winfsp MSI) + the nfsserve source fork
installer/     Inno Setup script + redistributable bundling (WinFsp, LICENSES/)
macos-helpers/ Swift FinderSync extension (xcodegen project)
scripts/       Build/sign/notarize/release + appcast helpers
```

## Building a release installer

End-to-end Windows release build:

```powershell
# 1. Vendored prebuilts (one-time per fresh clone or version bump)
scripts/setup-external.ps1

# 2. App + agent
just release
just agent-release

# 3. Qt runtime deploy + DLL copy
& 'C:\Qt\6.11.1\msvc2022_64\bin\windeployqt.exe' --qmldir app/qml --release build/release/ufb.exe
Copy-Item external\ffmpeg\bin\*.dll build\release\ -Force
Copy-Item external\pdfium\bin\pdfium.dll build\release\ -Force
Copy-Item vcpkg_installed\x64-windows\bin\*.dll build\release\ -Force

# 4. Compile the installer
& 'C:\Program Files (x86)\Inno Setup 6\iscc.exe' installer\ufb_installer.iss
```

Output: `installer/UFB-{version}-x64.exe` (e.g.
`UFB-0.9.9909-x64.exe`). See [`installer/README.md`](installer/README.md)
for what's bundled + end-user prerequisites.

## Project status

Both platforms ship from this repo. The numbered Windows-port phases
and the macOS port are complete, including the OS-native
simplification pass: GUI-owned SMB mounts, OS-keychain credentials,
and path-identity live mode. The **1.0.x** series is incremental polish —
the spacebar preview lightbox, persistent thumbnail cache, in-app
auto-update (Sparkle / WinSparkle via [ufbrowser.com](https://ufbrowser.com)),
and startup/DB hardening.

macOS releases are built by `scripts/release-mac.sh` (sign → notarize
→ DMG → appcast); Windows releases by the Inno Setup flow below.

## License

UFB is licensed **GPL-3.0-or-later**. See [`LICENSE`](LICENSE) for
the GPL text and [`LICENSES/`](LICENSES/) for the third-party
license texts UFB redistributes (Qt LGPL-3.0, FFmpeg LGPL-2.1+,
OpenEXR / PDFium / psd_sdk / Phosphor / cxx-qt / SQLite / WinFsp).
[`LICENSES/THIRD_PARTY_NOTICES.txt`](LICENSES/THIRD_PARTY_NOTICES.txt)
is the index plus a categorised summary of the Rust / Qt-bundled
support libraries.
