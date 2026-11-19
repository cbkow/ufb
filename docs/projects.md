---
title: Jobs and tracking
permalink: /projects/
nav_order: 4
---

# Jobs and tracking

![A job](/images/ufb2-job.png)

## Jobs

A **job** is a folder made from a [project type](types.md): its sections (3d, ae, postings…) and what each holds.

- **Project → New Job…** asks for a number and a name and makes the job in the jobs folder, with its sections.
- A jobs folder is a saved share (or a folder) marked **Is a Jobs Folder**. Any folder can also be subscribed as a job from its menu (**Subscribe as Job**).
- Jobs are listed in the sidebar. A click on one shows its folder in the pane in front, like any other place. To open it **as a job** — its sections along the top, the section's items on the left, the chosen item's folders on the right — choose **Open as Job** in its menu, or click it with Alt held (Option on macOS). A click with the middle button, or with Ctrl held (Cmd on macOS), opens the job in a new tab.
- **Add** at the foot of the list adds an item the way the section's kind says: a *shot* is copied from the section's skeleton with its project file named after it; a *dated* item is named `YYMMDD<letter>_<name>`; a *folder* is a plain folder.

## Columns

Each section has its own columns: **Columns** at the foot of the list adds them; a column's head has its menu (edit, **Hide Column**).

| Type | Holds |
|---|---|
| Text, Number | a line of text, a number |
| Dropdown, Priority | one of the column's options, each with its colour |
| Date | a day |
| Checkbox | a tick |
| Notes, Links | several lines, in a small editor by the cell |

Press a cell to edit it. Columns sort the list from their heads. A link in a Links cell opens when it is pressed — a web page in the browser, an application's own address in that application; press beside it to edit the cell.

## The star, the tracker

The star on an item **tracks** it. A job's **Tracker** (after its sections) lists the job's tracked items across all sections; **All Tracked** in the sidebar lists those of every job. Cells are edited there as in a job, and the **Due** chips (Overdue, Today, This Week, This Month) filter by a date column. With several rows chosen (Ctrl or Cmd, Shift), the menu sets a value on all of them or untracks them.

![All Tracked](/images/ufb2-tracker.png)

## The board

**Board** at the foot of a section's list shows its items as cards in lanes — by a dropdown, a priority, a checkbox, or the star. Drag a card to another lane to give it that value; several chosen cards go together.

![The board](/images/ufb2-board.png)

## Renames and moves

When UFB renames or moves an item, a section or a job, its metadata follows — and comes back with Undo. A tracked item whose folder was renamed outside UFB is marked as missing, with **Relink** and **Remove** in its menu.
