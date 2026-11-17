---
title: Upgrading from 1.4
permalink: /upgrade/
nav_order: 7
---

# Upgrading from UFB 1.4

UFB 2 is the same tool rebuilt: a file browser first, with the job tracking you know. It installs over 1.4 and takes over what 1.4 kept.

## What happens when you update

- **macOS**: UFB 1.4 offers the update and installs it. The installer stops UFB and its mount agent, removes the old `/Applications/UFB` folder (the application, the agent and its Finder extension) and installs `/Applications/UFB.app`.
- **Windows**: the Microsoft Store updates ufBrowser.
- **At the first launch**, UFB 2 opens your database where it is, after putting one copy beside it (`ufb_v5.db.before-2.0`), and reads 1.4's settings once: bookmarks become favourites, project folders become jobs folders, path mappings and the sync settings are taken as they are. 1.4's own settings file is not changed.

## What is different

- **No sync mounts.** UFB 1.4's mount agent, its local cache of a share and the drivers under it (WinFsp on Windows, the loopback mount on macOS) are gone. A share is the system's own mount, shown in the sidebar and connected on a press. A sync mount saved in 1.4 becomes a plain saved share. WinFsp can be uninstalled if nothing else needs it.
- **No Finder extension** on macOS.
- **Tabs are optional**: a window shows one view until you open a tab.
- **New**: Undo and Redo for file operations, a tasks panel, archives without a bundled 7-Zip, a Settings window, project types a studio can write itself, a board that takes several cards, notes on several lines.

## Mixed studios

UFB 2 and UFB 1.4 machines share metadata on the same network: cells, stars and columns made in 1.4 travel both ways. **Columns made on a UFB 2 machine do not appear on machines still on 1.4**, so move a studio's machines together.

## Going back

Quit UFB 2, install 1.4.2 again from its [release](https://github.com/cbkow/ufb/releases/tag/v1.4.2) (macOS), and, to drop what was done since, put `ufb_v5.db.before-2.0` back as `ufb_v5.db`. UFB 1.4 can also open the database as UFB 2 left it.
