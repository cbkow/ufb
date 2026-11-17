---
title: Privacy
permalink: /privacy/
nav_order: 15
---

# Privacy

UFB runs on your machine and talks only to the places you point it at.

- **No accounts, no telemetry, no analytics.** UFB never phones home about
  you, your machine or your files, and there is no UFB server it could report
  to.
- **Your files stay where they are.** UFB browses, previews and organises
  files on your disks and on the network shares you connect. It uploads
  nothing anywhere.
- **Network access** is limited to things you set up:
  - **Your shares.** Mounting a share connects to that server with the
    credentials Windows or macOS already stores (Credential Manager /
    Keychain). UFB never sees or keeps the password.
  - **Your teammates on the local network.** Copies of UFB on the same LAN
    find each other with a small heartbeat that never leaves the local
    network (multicast, TTL 1). They exchange the job metadata you work with
    together: tracker columns and cell values. The heartbeat carries a
    random node ID, the machine's LAN address and port numbers. All of it
    stays between your own machines and your own shares.
  - **On macOS, the update check:** Sparkle fetches the release feed from
    `ufbrowser.com`.
  - **On Windows, the Microsoft Store** installs and updates UFB. The app
    makes no update requests itself, and the Store's own handling is covered
    by Microsoft's privacy statement.
  - **Links you open.** A link in a previewed page is handed to your browser. A web page you preview loads what it
    embeds, as a browser would.
- **Previews are local.** Thumbnails, the spacebar preview, and video, PDF and
  document rendering all run on your machine. The thumbnail cache stays on
  your machine.
- **What stays on your machine:** your preferences, saved shares, path mappings, favourites, the metadata
  database, the thumbnail cache and logs, all in UFB's own data folder. A Windows Store install keeps them in the
  app's private storage, which uninstalling removes. A folder left by the old Windows installer
  (`%LOCALAPPDATA%\ufb`) stays until you delete it. On macOS they live in `~/Library/Application Support/ufb`.

UFB is free software. The complete source is at
[github.com/cbkow/minplatform](https://github.com/cbkow/minplatform) under GPL-3.0-or-later,
so you can check all of the above. Questions:
[open an issue](https://github.com/cbkow/ufb/issues).
