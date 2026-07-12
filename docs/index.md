---
title: Home
permalink: /
nav_order: 1
---

# UFB — Union File Browser

A native Qt + Rust file browser and project-management tool built for
visual-effects and post-production workflows — on macOS and Windows.

## What it does

- **Browse production media fast** — native thumbnails for PSD, EXR,
  HDR, PDF, AI, and video, rendered directly from C++ decoders, with a
  persistent thumbnail cache.
- **Spacebar preview** — a QuickLook-style lightbox for images, video,
  PDFs, and text, including an EXR layer grid, without leaving the app.
- **Mount NAS shares** — SMB mounts with credentials in the OS
  keychain/credential store, plus a user-mode VFS for synced project
  folders (WinFsp on Windows, NFS loopback on macOS).
- **Track jobs and shots** — project metadata lives in a local SQLite
  store and syncs between workstations over a LAN mesh; columns and
  cells merge conflict-free across machines.
- **Cross-OS paths** — the same project tree resolves on macOS and
  Windows; drag-and-drop, copy/paste, and links work between both.

## Download

Grab the latest release from
[GitHub Releases](https://github.com/cbkow/ufb/releases): the macOS
DMG (Apple Silicon, notarized) or the Windows x64 installer. Both
platforms update themselves in-app after that.

## Source

UFB is developed in the open at
[github.com/cbkow/ufb](https://github.com/cbkow/ufb).
