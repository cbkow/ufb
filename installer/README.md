# UFB Installer (Inno Setup)

Builds a Windows installer for the Qt-based UFB. Run from this directory after producing a release build.

## Prerequisites

- [Inno Setup 6](https://jrsoftware.org/isinfo.php) installed (`iscc.exe` on PATH or invoke from the IDE).
- A populated `build/release/` containing:
  - `ufb.exe`
  - All Qt DLLs (`Qt6*.dll`, `qml/`, `plugins/`, `translations/`) from `windeployqt`
  - ffmpeg DLLs (`avcodec-62.dll`, `avformat-62.dll`, `avutil-60.dll`, `swscale-9.dll`, `swresample-6.dll`) from `external/ffmpeg/bin/`
  - PDFium (`pdfium.dll`) from `external/pdfium/bin/`
  - vcpkg DLLs (`OpenEXR-3_4.dll`, `Imath-3_2.dll`, `Iex-3_4.dll`, `IlmThread-3_4.dll`, `OpenEXRCore-3_4.dll`, `OpenEXRUtil-3_4.dll`, plus their transitive deps like `zlib1.dll`, `deflate.dll`)
  - `templates/projectTemplate/` (synced by the CMake POST_BUILD step)

## Build sequence

```powershell
# From repo root
cmake --preset release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
cmake --build build/release

# Build the mount agent (separate cargo workspace - the installer
# pulls it from agent\target\release\ufb-agent.exe).
Push-Location agent; cargo build --release; Pop-Location

# Deploy Qt runtime
& 'C:\Qt\6.11.1\msvc2022_64\bin\windeployqt.exe' --qmldir app/qml --release build/release/ufb.exe

# Vendored ffmpeg + (one-time) pdfium download
.\scripts\setup-external.ps1

# Copy ffmpeg + vcpkg DLLs next to ufb.exe
Copy-Item external\ffmpeg\bin\*.dll build\release\ -Force
Copy-Item external\pdfium\bin\pdfium.dll build\release\ -Force
Copy-Item vcpkg_installed\x64-windows\bin\*.dll build\release\ -Force
# (templates/ and icons/ are auto-synced by CMake POST_BUILD)

# Build the installer
iscc installer\ufb_installer.iss
```

The output `UFB-X.Y.Z-x64.exe` lands next to the `.iss`.

## What's installed

| Component | Purpose |
|---|---|
| `core` (fixed) | `ufb.exe`, `ufb-agent.exe`, all DLLs, Qt plugin dirs (`platforms/`, `imageformats/`, …), `qml/`, `translations/`, `templates/`, `icons/`, `assets/scripts/` (`open_union_link.ps1` protocol handler), license |
| `uri_protocol` | Registers `ufb://` (opens in app) and `union://` (opens in Explorer via `open_union_link.ps1`, with cross-OS path-mapping resolution) |
| `firewall` | Allows mesh-sync TCP 49221/49222 + UDP 4265 inbound for this exe |
| `shortcuts/desktop` + `shortcuts/startmenu` | Self-explanatory |

> **Removed (1.0.5, plans/17 slice D):** the `shell_ext` component — Nilesoft
> Shell context-menu integration and its workflow `.ps1` scripts.
> **Removed (1.0.7):** the transitional upgrade scrubs (Nilesoft import
> reversal, `UfbAgent`/`MediaMountAgent` Run keys, Explorer nav-pins +
> `NoDrives` mask, legacy firewall rule names) — the 1.0.6 installer
> already cleaned that debris on every machine it upgraded.

## Bundled redistributables

- **WinFsp 2.x** — bundled as `external/winfsp/winfsp.msi` (downloaded by `setup-external.ps1`, version pinned in the script + `version.txt`). The installer detects `HKLM\SOFTWARE\WinFsp` and runs the MSI silently (`msiexec /qn /norestart`) only when missing. Existing WinFsp installs are left alone — other apps may share it. The MSI is extracted to `{tmp}` (`Flags: dontcopy`) so it doesn't sit in `{app}` after install. WinFsp is **not** uninstalled with UFB.

## Deferred — not yet wired into the new app

| Legacy feature | Status |
|---|---|
| Code-signing / SignTool step | Add once a signing cert is provisioned. Until then SmartScreen will warn on first run. |
| Real Windows shell extension (`IContextMenu` DLL) | Dropped — plans/17 removed shell-menu integration entirely (in-app context menus + `explorer_pins.rs` cover the workflow). |

## URI scheme registration (debug, no installer)

For dev work without running the installer, you can register `ufb://` against your dev exe by importing this `.reg` snippet (adjust the path):

```
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Classes\ufb]
@="URL:Union File Browser Protocol"
"URL Protocol"=""

[HKEY_CURRENT_USER\Software\Classes\ufb\shell\open\command]
@="\"C:\\path\\to\\ufb\\build\\release\\ufb.exe\" \"%1\""
```

The installer uses HKCR (machine-wide); the snippet above uses HKCU so it doesn't need admin and won't conflict with an installed copy.
