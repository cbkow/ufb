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
  PDFs, text, HTML, Word documents, and minNotes notes, including an EXR
  layer grid, without leaving the app.
- **Mount NAS shares** — SMB mounts with credentials in the OS
  keychain/credential store, plus a user-mode VFS for synced project
  folders (WinFsp on Windows, NFS loopback on macOS).
- **Track jobs and shots** — project metadata lives in a local SQLite
  store and syncs between workstations over a LAN mesh; columns and
  cells merge conflict-free across machines.
- **Cross-OS paths** — the same project tree resolves on macOS and
  Windows; drag-and-drop, copy/paste, and links work between both.
- **Archives and links** — extract zip, 7z, rar and tar archives or
  zip up any selection from the context menu (bundled 7-Zip), and
  create web-link files that open in the browser on either OS.
- **Transcode in the background** — MP4 proxies, extractions and zips
  run in a task queue with progress in the status bar, never taking
  you away from the folder you are in.

## Download

Grab the latest release from
[GitHub Releases](https://github.com/cbkow/ufb/releases): the macOS
installer package (Apple Silicon, notarized) or the Windows x64
installer. Both platforms update themselves in-app after that.

UFB is GPL-3.0-or-later. Third-party components and their licenses
are listed in the app's `LICENSES/` folder and in
[THIRD_PARTY_NOTICES](https://github.com/cbkow/ufb/blob/main/LICENSES/THIRD_PARTY_NOTICES.txt);
each release also carries the source tarballs for the copyleft
components it bundles.

## Source

UFB is developed in the open at
[github.com/cbkow/ufb](https://github.com/cbkow/ufb).
