---
title: Install
permalink: /install/
nav_order: 2
---

# Install

## Windows

Install **ufBrowser** from the [Microsoft Store](https://apps.microsoft.com/detail/9N71KC2RQ65R). The Store is the only Windows channel and delivers the updates:

<a href="https://apps.microsoft.com/detail/9N71KC2RQ65R?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200" style="border: 1px solid #707070; border-radius: 4.5px;"/>
</a>

Or from a terminal:

```
winget install --id 9N71KC2RQ65R --source msstore --accept-package-agreements
```

### Requirements

- **Windows 10 version 1809 or later, or Windows 11**, x64.
- Nothing else. UFB 2 needs no driver: the WinFsp driver UFB 1.4 asked for is no longer used and can be uninstalled if nothing else on the machine needs it.

The package declares firewall rules for the ports UFB uses to share metadata on the local network (TCP **49221–49222**, UDP **4265**), registers the `ufb:` and `union:` link schemes, and has a startup task, so UFB can start in the tray when you sign in (Settings → General, or Task Manager → Startup).

### Where UFB keeps its data

A Store install keeps everything in the application's private storage:
`%LOCALAPPDATA%\Packages\cbkow.ufBrowser_k3nw8mfd599r4\LocalCache\Local\ufb`.

| Item | What it is |
|---|---|
| `ufb2-settings.json` | UFB 2's settings: favourites, saved shares, path mappings, sync, the window |
| `ufb_v5.db` | the metadata database: jobs, columns, cells, tracked items |
| `ufb_v5.db.before-2.0` | a copy UFB 2.0.1 - 2.0.3 made once, before first opening a 1.4 database |
| `settings.json` | UFB 1.4's settings, never changed |

### Uninstall

Uninstall from **Settings → Apps** like any Store app. That removes the program, its startup task, its firewall rules and the data above.

---

## macOS

Download the latest release from [GitHub Releases](https://github.com/cbkow/ufb/releases/latest). Requires **macOS 13 or later** on Apple Silicon.

- **`UFB-<version>-arm64.pkg`** — the installer. Use it when UFB 1.4 is installed on the Mac: it stops the old application and its mount agent, removes the old `/Applications/UFB` folder and installs `/Applications/UFB.app`.
- **`UFB-<version>-macOS.dmg`** — for a Mac that never had UFB: drag UFB to Applications.

An installed UFB 1.4 is offered UFB 2 by its own updater and installs the package itself.

UFB checks for updates once a day and from Settings → About → Check for Updates.

### Where UFB keeps its data

`~/Library/Application Support/ufb` — the same files as on Windows.

### Uninstall

Quit UFB from its menu-bar icon and move `/Applications/UFB.app` to the Trash. To remove the data too, delete `~/Library/Application Support/ufb`.

---

## The tray

Closing the last window leaves UFB running in the tray (the menu bar on macOS), so sync goes on. The tray's menu says what sync is doing, opens a window and quits. On macOS UFB leaves the Dock while no window is open.

## Links

`ufb:` links open a file or folder in UFB; `union:` links show it in Explorer or Finder. Copy either from any file's menu; both are written so that a colleague on the other system lands on the same file.
