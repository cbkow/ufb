---
title: Shares, paths and sync
nav_order: 5
---

# Shares, paths and sync

## Saved shares

**Shares** in the sidebar holds the network shares you work on. Add one with **+**: `smb://server/share` or `\\server\share` (either spelling on either system), or a folder of this machine that holds the same storage.

- UFB asks the system where a share is mounted; it never guesses from the name. A share that macOS mounted as `Name-2` is found all the same.
- A share that is already there is used as it is. One that is not is connected on a press, with the credentials the system holds (Keychain, Credential Manager) — UFB never sees or keeps a password.
- A share can be marked **Is a Jobs Folder**: its folders are jobs, of the [project type](types.md) chosen in its menu.

UFB 2 has no mount agent, no cache layer and no driver: a share is the system's own mount.

## Path mappings

A mapping says what a share is called on each system — `\\server\jobs` or `P:\Jobs` on Windows, `/Volumes/jobs` on macOS. With it, a path or link copied on one system opens on the other, and both systems agree which file a piece of metadata belongs to.

**Settings → Path mappings** lists every saved share with its pair and the mappings written by hand. The same pair on every machine means the same metadata, so set them once and alike.

## Sharing metadata with your team

Columns, cells and stars are shared between the machines of a studio on the local network:

- **Settings → Sync**: switch it on, choose the studio's shared folder (the same on every machine) and give this machine a name.
- Machines find each other on the LAN and exchange changes directly; the shared folder carries a snapshot for machines that were off. Edits merge cell by cell, so two people editing different cells of one shot both keep their work.
- The **Sync** entry at the foot of the sidebar and the tray's menu say what sync is doing.
- Nothing leaves your network. See [Privacy](privacy.md).

UFB 2 and UFB 1.4 machines share metadata with each other; see [Upgrading from 1.4](upgrade.md) for what does not cross.
