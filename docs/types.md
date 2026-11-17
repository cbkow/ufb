---
title: Project types
nav_order: 6
---

# Project types

A project type says what a job looks like: the pattern of its folder's name, its sections, and what **Add** makes in each. UFB has one built in (*Studio*); a studio can write its own.

## Where a type lives

A type is a folder with a `type.toml` and, for its shot sections, a `skeletons/` folder:

- `<the studio's shared sync folder>/types/<name>/` — every machine of the studio reads the same ones; or
- `types/<name>/` in this machine's UFB data folder.

A job's type is that of its **jobs folder**: choose it in the menu of the saved share marked *Is a Jobs Folder*. Nothing is written into a job. A type that cannot be read is never taken — choosing it says why.

## `type.toml`

```toml
name = "Studio"
job_name = "{number}_{name}"

[[section]]
folder = "3d"
kind = "shot"          # Add copies skeletons/3d to 3d/<name>; the project file becomes <name>_v001.<ext>
skeleton = "3d"
folders = ["assets"]   # made inside the section when the job is made
columns = [
  { name = "Status", type = "dropdown", options = ["Todo", "In progress", { name = "Done", color = "#5fbf3f" }] },
  { name = "Artist" },
  { name = "Due", type = "date" },
]

[[section]]
folder = "postings"
kind = "dated"         # Add makes <YYMMDD><letter>_<name>

[[section]]
folder = "docs"
kind = "folder"        # Add makes a plain folder

[[section]]
folder = "audition"
kind = "none"          # nothing is added by UFB
folders = ["outputs", "projects"]
```

- **kind**: `shot`, `dated`, `folder` or `none`.
- **columns**: what the section starts with in a *new* job. `type` is `text` (when none is written), `dropdown`, `date`, `priority`, `number`, `checkbox`, `links` or `notes`; `options` are for a dropdown or a priority — a name, or a name with its colour.

The built-in type, with its skeleton files, is in the source:
[`crates/ufb-projtype/types/studio`](https://github.com/cbkow/minplatform/tree/main/crates/ufb-projtype/types/studio).
