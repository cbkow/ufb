---
title: Browse
permalink: /browse/
nav_order: 3
---

# Browse

![The browser](/images/ufb2-browser.png)

## The window

- **The sidebar**: Favorites, Shares, Jobs, All Tracked, Places (the system's folders and drives) and Sync. Add a favourite from a folder's menu (**Add to Favorites**) or by dropping a folder on the Favorites heading; dropping on a favourite itself copies or moves into it.
- **List or grid**, with a size slider for the tiles. A folder remembers how it was shown once you change its view.
- **Two panes** side by side, each with its own history. A browser view opens with both; **View → Show Second Pane** (or the toolbar's right-hand button) closes and opens the second, and **Settings → General → Show a second pane** says how a view starts.
- **Tabs** are there when you ask for them: Ctrl+T (Cmd+T on macOS) opens one, Ctrl+W closes it, and a window with one tab shows no tab strip. Any folder's menu has **Open in New Tab** and **Open in New Window**.
- **The path bar** takes a typed or pasted path, a `ufb:` link, or a path written for the other system.

## Preview

Press **Space** on a file. Stills (EXR with its layers, PSD, TIFF, HEIC and the common formats), video with a playbar, sound, image sequences, text, PDF and web pages. The arrow keys step through the folder; Space or Escape closes.

![Preview](/images/ufb2-preview.png)

## Image sequences

Numbered frames are shown as one entry (`shot_010.[1001-1048].exr`). It previews as a clip, and copies, moves, renames and transcodes as a whole. **View → Group Sequences** turns that off; chosen frames can also be grouped by hand (**Group as Sequence**).

## File operations

New folder, rename, duplicate, copy, move, Trash — by menu, by key and by drag and drop, between panes, tabs, windows and other applications. A name that is taken is asked about (**Keep Both** among the answers).

- **Undo and Redo** (Edit menu; Redo is Shift+Cmd+Z on macOS, Ctrl+Y on Windows) cover what UFB did: a rename is renamed back, a copy goes to the Trash, a trashed file comes back.
- **Rename with several chosen** asks for *Find* and *Replace with*, shows what it would make, and renames all of them or none — in one step of Undo. A sequence is renamed as a whole this way.
- **Copy Path / Copy Name / Copy Link** in every menu. A path under a share is copied as every machine reads it.

## Tasks

Everything that takes time — copies, moves, archives, transcodes — is listed in the tasks panel, opened from the status bar. A row shows how far it is and can be stopped. File operations run one at a time; archives and transcodes have a lane of their own.

![Tasks](/images/ufb2-tasks.png)

## Archives

- **Extract Here**: zip, 7z, tar (plain, gzip, bzip2, xz, zstd) by UFB itself; RAR, cab and iso through the system's own archive tool. An archive with a password asks for it. Nothing is ever overwritten: the result is a new folder beside the archive.
- **Compress to ZIP**: whatever is chosen.
- Undo sends the result to the Trash.

## Transcode to MP4

**Transcode to MP4** in the menu of clips and image sequences makes `MP4/<name>.mp4` beside the source — H.264 with AAC, at the picture's full size. HDR and linear sources are brought to SDR on the way.

## Search

Typing in the search field filters the folder in view. **Return** searches the folders below as well; what is found is a listing like any other (open, preview, copy, drag, transcode…), and **Show in Enclosing Folder** goes to a find's folder. Escape goes back.

## Settings

**Settings…** is in the application menu on macOS (Cmd+,) and in Edit on Windows (Ctrl+,): what a new pane shows (hidden files, folders first, sequences as one entry, grid), whether a view starts with its second pane, start at sign-in, sync, path mappings, and About with the licence notices.

![Settings](/images/ufb2-settings.png)
