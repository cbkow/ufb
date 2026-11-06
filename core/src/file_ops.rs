//! File operations.
//!
//! Phase 1d: full surface ported from `src-tauri/src/file_ops.rs`. The
//! original `commands.rs` wrapped this module's free functions with
//! `#[tauri::command]` attributes and emitted `fileop:*` Tauri events
//! around copy/move (which used `fs_extra` directly inside the command
//! body). Both go away in Phase 1e — copy/move move into here taking a
//! `FileOpsEvents` trait object.

use serde::{Deserialize, Serialize};
use std::path::Path;

/// A single filesystem entry (file or directory) returned by listings,
/// search, drag/drop, and clipboard operations.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FileEntry {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
    pub size: u64,
    pub modified: Option<i64>,
    pub extension: String,
}

/// Windows `ERROR_UNTRUSTED_MOUNT_POINT`. Raised by the NT FS filter when
/// a process tries to traverse a reparse point (symlink/junction/WinFsp
/// mount) and SmartScreen's first-run reputation check is still running
/// on the calling EXE. The restriction lifts within a few seconds once
/// the scan completes — the only fix without a code-signing cert is to
/// retry. We retry only on this exact errno to avoid masking real
/// permission or not-found errors.
#[cfg(windows)]
const ERROR_UNTRUSTED_MOUNT_POINT: i32 = 448;

/// Wrapper around `read_dir` that transparently retries on the Windows
/// SmartScreen first-launch 448 error. Up to 4 attempts over ~1.5s total.
fn read_dir_with_448_retry(path: &Path) -> std::io::Result<std::fs::ReadDir> {
    #[cfg(windows)]
    {
        let mut delay_ms = 200;
        for attempt in 0..4 {
            match std::fs::read_dir(path) {
                Ok(rd) => return Ok(rd),
                Err(e) => {
                    if e.raw_os_error() == Some(ERROR_UNTRUSTED_MOUNT_POINT) && attempt < 3 {
                        log::warn!(
                            "[file_ops] 448 UNTRUSTED_MOUNT_POINT on {:?}, retry {} in {}ms",
                            path,
                            attempt + 1,
                            delay_ms
                        );
                        std::thread::sleep(std::time::Duration::from_millis(delay_ms));
                        delay_ms *= 2;
                        continue;
                    }
                    return Err(e);
                }
            }
        }
        unreachable!()
    }
    #[cfg(not(windows))]
    {
        std::fs::read_dir(path)
    }
}

/// List directory contents, returning file entries sorted (dirs first, then by name).
pub fn list_directory(path: &str) -> Result<Vec<FileEntry>, String> {
    let dir_path = Path::new(path);

    let mut entries = Vec::new();
    let read_dir = match read_dir_with_448_retry(dir_path) {
        Ok(rd) => rd,
        Err(e) => return Err(format!("Not a directory or cannot read: {}", e)),
    };

    for entry in read_dir {
        let entry = match entry {
            Ok(e) => e,
            Err(_) => continue,
        };

        let is_symlink = entry.file_type().map(|ft| ft.is_symlink()).unwrap_or(false);

        // Use std::fs::metadata (follows symlinks) to get the target's metadata.
        // Fall back to symlink_metadata so symlinks still appear even if target is slow.
        let metadata =
            std::fs::metadata(entry.path()).or_else(|_| std::fs::symlink_metadata(entry.path()));
        let metadata = match metadata {
            Ok(m) => m,
            Err(_) => continue,
        };
        let name = entry.file_name().to_string_lossy().to_string();
        // Use the path as-is (preserving symlink paths). Do NOT canonicalize —
        // that resolves symlinks to their target, breaking path prefix matching
        // for subscriptions and mount detection.
        let path_str = entry.path().to_string_lossy().to_string();
        let extension = entry
            .path()
            .extension()
            .map(|e| e.to_string_lossy().to_string())
            .unwrap_or_default();
        let modified = metadata
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_millis() as i64);

        // Directory symlinks: metadata.is_dir() is true when std::fs::metadata
        // followed the symlink. If we fell back to symlink_metadata, is_dir is
        // false — so for symlinks, assume directory if we can't tell.
        let is_dir = metadata.is_dir() || (is_symlink && !metadata.is_file());

        entries.push(FileEntry {
            name,
            path: path_str,
            is_dir,
            size: metadata.len(),
            modified,
            extension,
        });
    }

    entries.sort_by(|a, b| {
        b.is_dir
            .cmp(&a.is_dir)
            .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
    });

    Ok(entries)
}

/// Create a new directory.
pub fn create_directory(path: &str) -> Result<(), String> {
    std::fs::create_dir_all(path).map_err(|e| format!("Failed to create directory: {}", e))
}

/// Rename a file or directory.
pub fn rename_path(old_path: &str, new_path: &str) -> Result<(), String> {
    std::fs::rename(old_path, new_path).map_err(|e| format!("Failed to rename: {}", e))
}

/// Try to trash a single path. Returns Ok if trashed, Err if it needs fallback.
pub fn try_trash_one(path: &str) -> Result<(), ()> {
    trash::delete(path).map_err(|_| ())
}

/// Fallback deletion for paths that can't be recycled (network/junction paths).
pub fn fallback_delete(paths: &[String]) -> Result<(), String> {
    if paths.is_empty() {
        return Ok(());
    }

    #[cfg(windows)]
    {
        shell_delete_files(paths)?;
    }

    #[cfg(not(windows))]
    {
        for path in paths {
            let p = Path::new(path);
            if p.is_dir() {
                std::fs::remove_dir_all(p)
                    .map_err(|e| format!("Failed to delete '{}': {}", path, e))?;
            } else {
                std::fs::remove_file(p)
                    .map_err(|e| format!("Failed to delete '{}': {}", path, e))?;
            }
        }
    }

    Ok(())
}

/// Delete files/directories to the OS trash/recycle bin.
pub fn delete_to_trash(paths: &[String]) -> Result<(), String> {
    let mut failed: Vec<String> = Vec::new();

    for path in paths {
        if try_trash_one(path).is_err() {
            failed.push(path.clone());
        }
    }

    fallback_delete(&failed)
}

/// Windows: delete files using SHFileOperationW which shows the native
/// "Are you sure you want to permanently delete?" dialog for network paths.
#[cfg(windows)]
fn shell_delete_files(paths: &[String]) -> Result<(), String> {
    use windows::Win32::UI::Shell::FOF_ALLOWUNDO;
    use windows::Win32::UI::Shell::{SHFileOperationW, FO_DELETE, SHFILEOPSTRUCTW};

    let mut wide: Vec<u16> = Vec::new();
    for path in paths {
        wide.extend(path.encode_utf16());
        wide.push(0);
    }
    wide.push(0);

    let mut op = SHFILEOPSTRUCTW {
        wFunc: FO_DELETE,
        pFrom: windows::core::PCWSTR(wide.as_ptr()),
        fFlags: FOF_ALLOWUNDO.0 as u16,
        ..Default::default()
    };

    let result = unsafe { SHFileOperationW(&mut op) };

    if result != 0 {
        if op.fAnyOperationsAborted.as_bool() {
            return Ok(());
        }
        return Err(format!("Shell delete failed (error {})", result));
    }

    Ok(())
}

/// Whether a paste should move or copy the underlying files.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PasteEffect {
    Copy,
    Move,
}

/// Result of a clipboard paste read: the file paths plus the effect
/// (move vs copy). Effect is determined per-platform — see
/// [`clipboard_paste_intent`].
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PasteIntent {
    pub paths: Vec<String>,
    pub effect: PasteEffect,
}

// The clipboard file-list code (one NSURL per item via writeObjects:, CF_HDROP + Preferred DropEffect, the cut registry
// keyed on the pasteboard sequence, the non-blocking has_paths probe) moved to min-paths `clipboard` (canon §22 row 125);
// these keep ufb's API and types.
fn from_min_intent(i: min_paths::clipboard::PasteIntent) -> PasteIntent {
    PasteIntent { paths: i.paths, effect: match i.effect { min_paths::clipboard::PasteEffect::Move => PasteEffect::Move, min_paths::clipboard::PasteEffect::Copy => PasteEffect::Copy } }
}

/// Copy file paths to clipboard.
pub fn clipboard_copy_paths(paths: &[String]) -> Result<(), String> { min_paths::clipboard::copy_paths(paths) }

/// Cut file paths to clipboard: Windows sets "Preferred DropEffect" = DROPEFFECT_MOVE (Explorer pastes as a move);
/// macOS records the cut in the registry (the pasteboard has no move flavour).
pub fn clipboard_cut_paths(paths: &[String]) -> Result<(), String> { min_paths::clipboard::cut_paths(paths) }

/// Paste file paths from clipboard (native file list first, then path-looking lines of text).
pub fn clipboard_paste_paths() -> Result<Vec<String>, String> { min_paths::clipboard::paste_paths() }

/// Read paths and resolve move-vs-copy intent in one pass.
pub fn clipboard_paste_intent() -> Result<PasteIntent, String> { min_paths::clipboard::paste_intent().map(from_min_intent) }

/// Cheap, non-blocking "could Paste do anything?" probe for menu enablement (never renders delayed clipboard data).
pub fn clipboard_has_paths() -> bool { min_paths::clipboard::has_paths() }

/// Reveal a path in the native file manager. Selects the item in
/// its parent folder.
pub fn reveal_in_file_manager(path: &str) -> Result<(), String> {
    reveal_paths_in_file_manager(&[path.to_string()])
}

/// Reveal `path` when it exists; otherwise open its parent folder (a
/// `union://` link may name a file that hasn't synced here yet). Neither
/// existing is an error, never a shell hand-off: ShellExecute on an
/// unresolved string like `Union:\\win\x` reads it as a protocol and
/// relaunches ufb.exe with the same link — an endless loop.
/// Blocking file-system calls (SMB timeouts): call off the GUI thread.
pub fn reveal_or_open_parent(path: &str) -> Result<(), String> {
    let p = std::path::Path::new(path);
    if p.exists() {
        return reveal_in_file_manager(path);
    }
    match p.parent() {
        Some(parent) if parent.is_dir() => opener::open(parent)
            .map_err(|e| format!("Failed to open {}: {}", parent.display(), e)),
        _ => Err(format!("{} not found (nor its parent folder)", path)),
    }
}

/// Reveal one or more paths in the native file manager. On macOS uses
/// `NSWorkspace.activateFileViewerSelectingURLs:` so multiple
/// selections all show selected in their (single) parent folder. On
/// Windows, falls back to `opener::reveal` for the first path
/// (Windows shell doesn't expose an obvious multi-select reveal).
///
/// Per macOSplans/07.
pub fn reveal_paths_in_file_manager(paths: &[String]) -> Result<(), String> {
    if paths.is_empty() {
        return Ok(());
    }

    #[cfg(target_os = "macos")]
    {
        return reveal_paths_macos(paths);
    }

    #[cfg(not(target_os = "macos"))]
    {
        // Windows / other unix: opener::reveal handles a single
        // path; we reveal the first and ignore the rest.
        opener::reveal(&paths[0]).map_err(|e| format!("Failed to reveal: {}", e))
    }
}

#[cfg(target_os = "macos")]
fn reveal_paths_macos(paths: &[String]) -> Result<(), String> {
    use objc2_app_kit::NSWorkspace;
    use objc2_foundation::{NSArray, NSString, NSURL};

    let workspace = NSWorkspace::sharedWorkspace();

    // Build NSURLs from each path. We hold the NSURL retainable
    // wrappers in a Vec so they live until activateFileViewerSelectingURLs
    // returns; the NSArray below borrows them.
    let urls: Vec<objc2::rc::Retained<NSURL>> = paths
        .iter()
        .map(|p| {
            let s = NSString::from_str(p);
            NSURL::fileURLWithPath(&s)
        })
        .collect();
    let url_refs: Vec<&NSURL> = urls.iter().map(|u| &**u).collect();
    let array = NSArray::from_slice(&url_refs);

    workspace.activateFileViewerSelectingURLs(&array);
    Ok(())
}

/// Resolve a non-colliding path inside `parent` for `base`. Tries
/// `parent/base` first, then `parent/base{a..z}`. Returns the
/// candidate as a string. Falls back to `parent/base` (which will
/// collide) if all 26 letter-suffixed slots are taken too — caller
/// should treat the unchanged base as failure if that matters.
pub fn find_available_path(parent: &str, base: &str) -> String {
    let parent_path = Path::new(parent);
    let bare = parent_path.join(base);
    if !bare.exists() {
        return bare.to_string_lossy().into_owned();
    }
    for c in 'a'..='z' {
        let candidate = parent_path.join(format!("{}{}", base, c));
        if !candidate.exists() {
            return candidate.to_string_lossy().into_owned();
        }
    }
    bare.to_string_lossy().into_owned()
}

/// Create a UFB-folder directory using the `{date_prefix}{letter}_{label}`
/// naming pattern. The letter is shared across ALL existing items with
/// the same date prefix, not just items with the same label — so if
/// `260518a_alpha` exists, the next UFB folder is `260518b_<label>`
/// regardless of label. Matches the slot semantics used by
/// [`create_date_prefixed_item`].
///
/// Returns the absolute path of the new directory on success.
pub fn create_ufb_folder(
    parent: &str,
    date_prefix: &str,
    label: &str,
) -> Result<String, String> {
    let parent_path = Path::new(parent);
    if !parent_path.is_dir() {
        return Err(format!("Folder does not exist: {}", parent));
    }

    let existing: Vec<String> = std::fs::read_dir(parent_path)
        .map_err(|e| format!("read {:?}: {}", parent_path, e))?
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .map(|e| e.file_name().to_string_lossy().to_lowercase())
        .filter(|n| n.starts_with(&date_prefix.to_lowercase()))
        .collect();

    for c in 'a'..='z' {
        let slot = format!("{}{}", date_prefix.to_lowercase(), c);
        if !existing.iter().any(|n| n.starts_with(&slot)) {
            let dir_name = format!("{}{}_{}", date_prefix, c, label);
            let full_path = parent_path.join(&dir_name);
            std::fs::create_dir_all(&full_path)
                .map_err(|e| format!("create {:?}: {}", full_path, e))?;
            return Ok(full_path.to_string_lossy().into_owned());
        }
    }

    Err(format!(
        "All UFB folder slots for {} are taken (a–z exhausted)",
        date_prefix
    ))
}

/// Create a date-prefixed item directory. Format:
/// `{YYMMDD}{letter}_{base_name}/` where letter is a–z. The letter
/// is shared across ALL existing items with today's date prefix, not
/// just items with the same base name — so if `271015a_foo` exists,
/// the next item is `271015b_bar` regardless of base. Used by
/// "postings" / asset-style folders (detect_add_mode → "date_prefixed").
///
/// Returns the absolute path of the new directory on success.
pub fn create_date_prefixed_item(parent: &str, base_name: &str) -> Result<String, String> {
    let today = chrono::Local::now().format("%y%m%d").to_string();
    let parent_path = Path::new(parent);
    if !parent_path.is_dir() {
        return Err(format!("Folder does not exist: {}", parent));
    }

    let existing: Vec<String> = std::fs::read_dir(parent_path)
        .map_err(|e| format!("read {:?}: {}", parent_path, e))?
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .map(|e| e.file_name().to_string_lossy().to_lowercase())
        .filter(|n| n.starts_with(&today))
        .collect();

    for c in 'a'..='z' {
        let prefix = format!("{}{}", today, c);
        if !existing.iter().any(|n| n.starts_with(&prefix)) {
            let dir_name = format!("{}_{}", prefix, base_name);
            let full_path = parent_path.join(&dir_name);
            std::fs::create_dir_all(&full_path)
                .map_err(|e| format!("create {:?}: {}", full_path, e))?;
            return Ok(full_path.to_string_lossy().into_owned());
        }
    }

    Err(format!(
        "All date-prefixed slots for {} are taken (a–z exhausted)",
        today
    ))
}

/// Create a date-prefixed minNotes note. Format:
/// `{YYMMDD}{letter}_{base_name}.mnd` — the same shared a–z slot
/// walk as [`create_date_prefixed_item`], with two differences:
/// the result is a file rather than a directory, and the slot scan
/// counts every entry (files AND folders) with today's prefix so a
/// note and a folder can't end up sharing a letter.
///
/// The file is a real minNotes document, not an empty placeholder:
/// minNotes only seeds a first block for documents it creates itself
/// (`BlockModel::newDocument` → `seedEmptyDoc`), so an empty file gave
/// it nothing to put a cursor in and the note never opened. This writes
/// what minNotes' own new-document path writes — the `.mnd` schema, a
/// stamped `doc_meta` row (format marker included — minNotes 1.0+
/// refuses a file without it), and one empty paragraph block — as a plain
/// rollback-journal SQLite file (minNotes normalises saves to that
/// form; it never runs SQLite on the share itself, it stages a copy).
///
/// Returns the absolute path of the new file on success.
pub fn create_date_prefixed_note(parent: &str, base_name: &str) -> Result<String, String> {
    let today = chrono::Local::now().format("%y%m%d").to_string();
    let parent_path = Path::new(parent);
    if !parent_path.is_dir() {
        return Err(format!("Folder does not exist: {}", parent));
    }

    let existing: Vec<String> = std::fs::read_dir(parent_path)
        .map_err(|e| format!("read {:?}: {}", parent_path, e))?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().to_lowercase())
        .filter(|n| n.starts_with(&today))
        .collect();

    for c in 'a'..='z' {
        let prefix = format!("{}{}", today, c);
        if !existing.iter().any(|n| n.starts_with(&prefix)) {
            let file_name = format!("{}_{}.mnd", prefix, base_name);
            let full_path = parent_path.join(&file_name);
            if full_path.exists() {
                continue;
            }
            if let Err(e) = write_minnotes_document(&full_path) {
                // Never leave a torn file behind: it would hold the
                // date-letter slot and minNotes refuses it ("isn't a
                // minNotes document").
                let _ = std::fs::remove_file(&full_path);
                let mut journal = full_path.clone().into_os_string();
                journal.push("-journal");
                let _ = std::fs::remove_file(journal);
                return Err(e);
            }
            return Ok(full_path.to_string_lossy().into_owned());
        }
    }

    Err(format!(
        "All date-prefixed slots for {} are taken (a–z exhausted)",
        today
    ))
}

/// minNotes document format version this writer produces
/// (`Document::kSchemaVersion` in the minNotes repo). The numbering
/// restarted at 1 with the 1.0 `.mnd` clean break.
const MINNOTES_SCHEMA_VERSION: i64 = 1;

/// `doc_meta.format` marker (`Document::kFormat`). minNotes 1.0+ opens
/// only files stamped with it (`BlockModel::acceptOpenedFormat`); a
/// file without it is refused as "made with an earlier minNotes".
const MINNOTES_FORMAT: &str = "mnd";

/// ULID (48-bit ms timestamp + 80 random bits, Crockford base32, 26
/// chars) — the block-id shape minNotes' `makeUlid()` produces, so ids
/// created here sort alongside ids minNotes adds later.
fn make_ulid() -> String {
    const ENC: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    let mut ts = chrono::Utc::now().timestamp_millis().max(0) as u64;
    let mut out = [b'0'; 26];
    for i in (0..10).rev() {
        out[i] = ENC[(ts & 0x1F) as usize];
        ts >>= 5;
    }
    let rnd = uuid::Uuid::new_v4();
    let bytes = rnd.as_bytes();
    for (k, slot) in out.iter_mut().enumerate().skip(10) {
        // One 5-bit draw per char from the 128 random bits (16 draws = 80 bits).
        let bit = (k - 10) * 5;
        let byte = bit / 8;
        let shift = bit % 8;
        let v = ((bytes[byte] as u16) | ((bytes[(byte + 1) % 16] as u16) << 8)) >> shift;
        *slot = ENC[(v & 0x1F) as usize];
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Write a fresh minNotes document at `path`: schema, `doc_meta` stamp,
/// and one empty paragraph block. Mirrors `Document::open` (DDL) +
/// `stampMeta` + `BlockModel::seedEmptyDoc` in the minNotes repo.
fn write_minnotes_document(path: &Path) -> Result<(), String> {
    use rusqlite::{params, Connection, OpenFlags};
    let conn = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE,
    )
    .map_err(|e| format!("create {:?}: {}", path, e))?;
    conn.execute_batch(
        r#"
        PRAGMA foreign_keys=ON;
        BEGIN;
        CREATE TABLE IF NOT EXISTS blocks (
            id       TEXT PRIMARY KEY,
            rank     TEXT NOT NULL,
            depth    INTEGER NOT NULL DEFAULT 0,
            type     TEXT NOT NULL,
            attrs    TEXT,
            content  TEXT NOT NULL DEFAULT '',
            created  INTEGER,
            modified INTEGER
        );
        CREATE INDEX IF NOT EXISTS blocks_order ON blocks(rank, depth, type);
        CREATE TABLE IF NOT EXISTS doc_meta (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            title TEXT, schema_version INTEGER, app_version TEXT,
            created INTEGER, modified INTEGER, last_cursor TEXT,
            page_width INTEGER, format TEXT
        );
        CREATE TABLE IF NOT EXISTS block_ink (
            block_id TEXT PRIMARY KEY REFERENCES blocks(id) ON DELETE CASCADE,
            ink      TEXT NOT NULL,
            modified INTEGER
        );
        CREATE TABLE IF NOT EXISTS comment_threads (
            id       TEXT PRIMARY KEY,
            created  INTEGER,
            resolved INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS comment_messages (
            id        TEXT PRIMARY KEY,
            thread_id TEXT NOT NULL REFERENCES comment_threads(id) ON DELETE CASCADE,
            body      TEXT NOT NULL,
            created   INTEGER,
            modified  INTEGER
        );
        "#,
    )
    .map_err(|e| format!("schema {:?}: {}", path, e))?;

    let now = chrono::Utc::now().timestamp_millis();
    let app_version = format!("ufb {}", crate::version());
    conn.execute(
        "INSERT INTO doc_meta (id, schema_version, app_version, created, modified, format)          VALUES (1, ?1, ?2, ?3, ?3, ?4)",
        params![MINNOTES_SCHEMA_VERSION, app_version, now, MINNOTES_FORMAT],
    )
    .map_err(|e| format!("doc_meta {:?}: {}", path, e))?;

    // First block: rank "V" is what minNotes' rankBetween("", "") yields —
    // the midpoint of the 62-digit alphabet, leaving room to insert
    // before it. attrs NULL, content "" (NOT NULL — an empty string, not
    // a null, or the row is refused and the doc opens empty).
    conn.execute(
        "INSERT INTO blocks (id, rank, depth, type, attrs, content, created, modified)          VALUES (?1, 'V', 0, 'paragraph', NULL, '', ?2, ?2)",
        params![make_ulid(), now],
    )
    .map_err(|e| format!("seed block {:?}: {}", path, e))?;

    // One transaction for schema + stamp + seed: one journal round trip
    // on a share instead of one per statement, and all-or-nothing.
    conn.execute_batch("COMMIT")
        .map_err(|e| format!("commit {:?}: {}", path, e))?;
    conn.close().map_err(|(_, e)| format!("close {:?}: {}", path, e))?;
    Ok(())
}

// ── Folder layout detection (FolderTabView Mode B / Mode C) ─────────
//
// Lifted from the legacy ufb-tauri implementation (commands.rs:1553–
// 1617 + FolderTabView.tsx:82–90). Used by the QML FolderTabView to
// decide whether each inner job tab renders as ItemListPanel + 1
// browser ("B") or ItemListPanel + 2 stacked browsers for project /
// renders ("C").

/// Subdirectory name patterns that indicate a "project" / working-
/// file location inside a shot folder. Case-insensitive comparison.
const PROJECT_SUBDIR_NAMES: &[&str] = &[
    "project", "projects", "scenes", "scene", "work", "source", "src",
    "maya", "houdini", "nuke", "ae", "c4d", "blender", "flame",
    "fusion", "resolve", "premiere", "aftereffects", "pfx",
    "matchmove", "roto",
];

/// Subdirectory name patterns that indicate a "renders" / output
/// location inside a shot folder. Case-insensitive.
const RENDER_SUBDIR_NAMES: &[&str] = &[
    "renders", "render", "output", "outputs", "comp", "comps",
    "export", "exports", "deliverables", "plates", "precomp",
];

/// Subdirectory name patterns for the "proxies" alternate output
/// location. Case-insensitive. Only the AE folder type surfaces
/// this today (its bottom pane gets a renders/proxies subtab strip),
/// but detection is item-shape based like the other two lists.
const PROXY_SUBDIR_NAMES: &[&str] = &["proxies", "proxy"];

fn matches_any_ci(name: &str, list: &[&str]) -> bool {
    list.iter().any(|n| name.eq_ignore_ascii_case(n))
}

/// Look at up to 10 child directories of `folder_path` and decide
/// whether the folder hosts shot-style items (each with their own
/// project + renders subfolders → "C") or flat items ("B").
///
/// Rules (matches the legacy detector):
///   1. If any one child contains BOTH a PROJECT-named and a
///      RENDER-named subfolder → "C".
///   2. Else if 2+ children each contain at least one PROJECT or
///      RENDER subfolder → "C".
///   3. Else → "B".
pub fn detect_folder_layout_mode(folder_path: &str) -> &'static str {
    let dir = match read_dir_with_448_retry(Path::new(folder_path)) {
        Ok(d) => d,
        Err(_) => return "B",
    };

    let mut sampled = 0;
    let mut children_with_known = 0;
    for entry in dir {
        let Ok(entry) = entry else { continue };
        let Ok(ft) = entry.file_type() else { continue };
        if !ft.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        if name.starts_with('.') {
            continue;
        }

        let (has_project, has_renders) = scan_known_children(&entry.path());
        if has_project && has_renders {
            return "C";
        }
        if has_project || has_renders {
            children_with_known += 1;
            if children_with_known >= 2 {
                return "C";
            }
        }

        sampled += 1;
        if sampled >= 10 {
            break;
        }
    }
    "B"
}

/// Scan the immediate children of `path`, returning whether any name
/// matches PROJECT or RENDER lists.
fn scan_known_children(path: &Path) -> (bool, bool) {
    let dir = match read_dir_with_448_retry(path) {
        Ok(d) => d,
        Err(_) => return (false, false),
    };
    let mut has_project = false;
    let mut has_renders = false;
    for entry in dir {
        let Ok(entry) = entry else { continue };
        let Ok(ft) = entry.file_type() else { continue };
        if !ft.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        if matches_any_ci(&name, PROJECT_SUBDIR_NAMES) {
            has_project = true;
        }
        if matches_any_ci(&name, RENDER_SUBDIR_NAMES) {
            has_renders = true;
        }
        if has_project && has_renders {
            break;
        }
    }
    (has_project, has_renders)
}

/// Look at the immediate children of `item_path` and return the path
/// of the first one whose name matches PROJECT_SUBDIR_NAMES, or None.
pub fn find_project_subdir(item_path: &str) -> Option<String> {
    find_named_subdir(item_path, PROJECT_SUBDIR_NAMES)
}

/// Look at the immediate children of `item_path` and return the path
/// of the first one whose name matches RENDER_SUBDIR_NAMES, or None.
pub fn find_render_subdir(item_path: &str) -> Option<String> {
    find_named_subdir(item_path, RENDER_SUBDIR_NAMES)
}

/// Look at the immediate children of `item_path` and return the path
/// of the first one whose name matches PROXY_SUBDIR_NAMES, or None.
/// Drives the AE tab's bottom-pane "proxies" subtab.
pub fn find_proxies_subdir(item_path: &str) -> Option<String> {
    find_named_subdir(item_path, PROXY_SUBDIR_NAMES)
}

fn find_named_subdir(item_path: &str, list: &[&str]) -> Option<String> {
    let dir = read_dir_with_448_retry(Path::new(item_path)).ok()?;
    for entry in dir {
        let Ok(entry) = entry else { continue };
        let Ok(ft) = entry.file_type() else { continue };
        if !ft.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        if matches_any_ci(&name, list) {
            return Some(entry.path().to_string_lossy().into_owned());
        }
    }
    None
}

// ── Add-Item mode detection ─────────────────────────────────────────
//
// Mirrors the legacy ufb-tauri behaviour (commands.rs:1499-1546):
// look up the folder name (case-insensitive) inside the bundled
// `assets/projectTemplate/` tree, then inspect its children:
//   - `_t_*` directory present  → "shot"     (template tree to copy)
//   - `000000*` directory present → "date_prefixed"
//   - matching folder exists but no marker children → "folder"
//   - matching folder absent entirely → "folder"
//   - `audition` is special-cased to "none" (no Add button)
//
// This means the same project-template directory drives both detection
// and the actual copy in `copy_template_to`, so adding a new folder
// type is just adding a directory under `assets/projectTemplate/`.

/// Returns the Add-Item dialog mode for a folder of the given name.
/// One of `"shot"`, `"date_prefixed"`, `"folder"`, `"none"`.
pub fn detect_add_mode(folder_name: &str) -> &'static str {
    if folder_name.eq_ignore_ascii_case("audition") {
        return "none";
    }
    let Some(template_root) = find_bundled_template_dir() else {
        return "folder";
    };

    // Case-insensitive match against template subdirs.
    let lower = folder_name.to_lowercase();
    let folder_dir = std::fs::read_dir(&template_root)
        .ok()
        .and_then(|read| {
            read.flatten()
                .find(|e| e.file_name().to_string_lossy().to_lowercase() == lower)
                .map(|e| e.path())
        });
    let Some(folder_dir) = folder_dir else {
        return "folder";
    };
    if !folder_dir.is_dir() {
        return "folder";
    }

    if let Ok(children) = std::fs::read_dir(&folder_dir) {
        for child in children.flatten() {
            let name = child.file_name().to_string_lossy().into_owned();
            if !child.path().is_dir() {
                continue;
            }
            if name.starts_with("_t_") {
                return "shot";
            }
            if name.starts_with("000000") {
                return "date_prefixed";
            }
        }
    }
    "folder"
}

/// Locate the bundled `assets/projectTemplate/` directory. Same
/// search strategy as `resolve_shot_template`: check next to the
/// executable first (production install), walk up the tree to find
/// dev layouts. Cached per-call — caller is expected to invoke this
/// at human pace (Add dialog open / mode probe), not in a hot loop.
fn find_bundled_template_dir() -> Option<std::path::PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let exe_dir = exe.parent()?.to_path_buf();
    // Qt port deploys to `<exe_dir>/templates/projectTemplate/` via the
    // app/CMakeLists.txt sync step + install rule. Legacy/dev layouts
    // under `assets/projectTemplate/` are kept as fallbacks so running
    // from the source tree still works.
    let bases: [std::path::PathBuf; 8] = [
        exe_dir.join("templates/projectTemplate"),
        exe_dir.join("../templates/projectTemplate"),
        exe_dir.join("../Resources/templates/projectTemplate"),
        exe_dir.join("../share/ufb/templates/projectTemplate"),
        exe_dir.join("assets/projectTemplate"),
        exe_dir.join("../assets/projectTemplate"),
        exe_dir.join("../../assets/projectTemplate"),
        exe_dir.join("../../../assets/projectTemplate"),
    ];
    for base in bases {
        if base.is_dir() {
            return Some(base);
        }
    }
    None
}

// ── Project-template copy ──────────────────────────────────────────
//
// Bundled templates live under `assets/projectTemplate/{folder_type}/`
// and contain a `_t_project_name/` directory whose tree gets copied
// into the user's job folder when they "Add Shot". Files named
// `template.<ext>` or `_template.<ext>` get renamed to
// `{item_name}_v001.<ext>` on copy. `.gitkeep` markers are skipped —
// they only exist to let git track empty directories in the legacy
// repo and aren't useful at runtime.

/// Find the bundled `_t_*` template directory for `folder_type`
/// (case-insensitive). Returns `None` if the bundled template tree
/// isn't reachable or the requested folder type doesn't ship one.
pub fn resolve_shot_template(folder_type: &str) -> Option<String> {
    let template_root = find_bundled_template_dir()?;
    let lower = folder_type.to_lowercase();

    // Case-insensitive folder match (mirrors detect_add_mode).
    let folder_dir = std::fs::read_dir(&template_root)
        .ok()?
        .flatten()
        .find(|e| e.file_name().to_string_lossy().to_lowercase() == lower)
        .map(|e| e.path())?;
    if !folder_dir.is_dir() {
        return None;
    }

    let read = std::fs::read_dir(&folder_dir).ok()?;
    for entry in read.flatten() {
        let Ok(ft) = entry.file_type() else { continue };
        if !ft.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with("_t_") {
            if let Ok(canon) = std::fs::canonicalize(entry.path()) {
                return Some(canon.to_string_lossy().into_owned());
            }
            return Some(entry.path().to_string_lossy().into_owned());
        }
    }
    None
}

/// Copy a template directory (e.g. the `_t_project_name` returned by
/// `resolve_shot_template`) into `dest_parent/{item_name}`, recursively.
/// Files named `template.<ext>` / `_template.<ext>` are renamed to
/// `{item_name}_v001.<ext>`. `.gitkeep` files are skipped. Returns
/// the absolute path of the new item directory on success.
pub fn copy_template_to(
    template_dir: &str,
    dest_parent: &str,
    item_name: &str,
) -> Result<String, String> {
    let src = Path::new(template_dir);
    if !src.is_dir() {
        return Err(format!("Template not found: {}", template_dir));
    }
    let dest = Path::new(dest_parent).join(item_name);
    if dest.exists() {
        return Err(format!(
            "Destination already exists: {}",
            dest.display()
        ));
    }
    std::fs::create_dir_all(&dest)
        .map_err(|e| format!("create dir {:?}: {}", dest, e))?;
    copy_template_recursive(src, &dest, item_name)?;
    Ok(dest.to_string_lossy().into_owned())
}

fn copy_template_recursive(src: &Path, dest: &Path, item_name: &str) -> Result<(), String> {
    let read = std::fs::read_dir(src)
        .map_err(|e| format!("read {:?}: {}", src, e))?;
    for entry in read {
        let entry = entry.map_err(|e| format!("entry: {}", e))?;
        let from = entry.path();
        let original_name = entry.file_name().to_string_lossy().into_owned();

        if original_name == ".gitkeep" {
            continue;
        }

        let new_name = rename_template_file(&original_name, item_name);
        let to = dest.join(&new_name);

        let ft = entry
            .file_type()
            .map_err(|e| format!("file_type {:?}: {}", from, e))?;
        if ft.is_dir() {
            std::fs::create_dir_all(&to).map_err(|e| format!("mkdir {:?}: {}", to, e))?;
            copy_template_recursive(&from, &to, item_name)?;
        } else {
            std::fs::copy(&from, &to).map_err(|e| format!("copy {:?} → {:?}: {}", from, to, e))?;
        }
    }
    Ok(())
}

/// `template.foo` / `_template.foo` → `{item_name}_v001.foo`. Other
/// names pass through unchanged.
fn rename_template_file(name: &str, item_name: &str) -> String {
    let trimmed = name.strip_prefix('_').unwrap_or(name);
    if let Some(stripped) = trimmed.strip_prefix("template.") {
        return format!("{}_v001.{}", item_name, stripped);
    }
    name.to_string()
}

/// Put a plain-text string on the system clipboard. Used for the
/// "Copy Path" / "Copy Filename" menu actions where we're not
/// transferring file objects — just human-readable text.
pub fn clipboard_copy_text(text: &str) -> Result<(), String> { min_paths::clipboard::copy_text(text) }

/// Plain text currently on the clipboard, or "" (no text / error).
/// Used to prefill the New Web Link dialog when the clipboard holds
/// a URL.
pub fn clipboard_text() -> String { min_paths::clipboard::clipboard_text() }

// ── Web links (.url / .webloc) ──────────────────────────────────────
//
// UFB creates Windows-style `.url` files (`[InternetShortcut]` INI):
// Explorer AND Finder both open them natively, whereas the macOS
// `.webloc` plist is Mac-only. UFB opens both kinds itself (see
// `open_file`) so a double-click behaves identically on both OSes
// and on shares populated from either side.

/// Extensions `open_file` treats as web links.
pub const WEB_LINK_EXTENSIONS: &[&str] = &["url", "webloc"];

pub fn is_web_link_path(path: &str) -> bool {
    Path::new(path)
        .extension()
        .and_then(|e| e.to_str())
        .map(|e| WEB_LINK_EXTENSIONS.contains(&e.to_ascii_lowercase().as_str()))
        .unwrap_or(false)
}

/// Parse the target URL out of a `.url` (INI) or `.webloc` (XML or
/// binary plist) file. Returns `None` for anything that isn't a
/// recognisable link.
pub fn read_web_link(path: &str) -> Option<String> {
    let bytes = std::fs::read(path).ok()?;
    let ext = Path::new(path)
        .extension()
        .and_then(|e| e.to_str())
        .map(|e| e.to_ascii_lowercase())
        .unwrap_or_default();
    match ext.as_str() {
        "url" => parse_url_ini(&String::from_utf8_lossy(&bytes)),
        "webloc" => {
            if bytes.starts_with(b"bplist") {
                // Binary plist (what recent Safari drag-outs write).
                // plutil is always present on macOS; elsewhere a
                // binary .webloc simply isn't openable by UFB.
                #[cfg(target_os = "macos")]
                {
                    let out = std::process::Command::new("plutil")
                        .args(["-convert", "xml1", "-o", "-", path])
                        .output()
                        .ok()?;
                    if !out.status.success() {
                        return None;
                    }
                    return parse_webloc_xml(&String::from_utf8_lossy(&out.stdout));
                }
                #[cfg(not(target_os = "macos"))]
                {
                    None
                }
            } else {
                parse_webloc_xml(&String::from_utf8_lossy(&bytes))
            }
        }
        _ => None,
    }
}

/// `[InternetShortcut]` / `URL=…` — case-insensitive key, tolerant of
/// CRLF, BOM, and keys before the section header.
fn parse_url_ini(text: &str) -> Option<String> {
    let text = text.trim_start_matches('\u{feff}');
    for line in text.lines() {
        let line = line.trim();
        if let Some((k, v)) = line.split_once('=') {
            if k.trim().eq_ignore_ascii_case("URL") {
                let v = v.trim();
                if !v.is_empty() {
                    return Some(v.to_string());
                }
            }
        }
    }
    None
}

/// `<key>URL</key>` followed by `<string>…</string>` in an XML plist.
fn parse_webloc_xml(text: &str) -> Option<String> {
    let key_at = text.find("<key>URL</key>")?;
    let rest = &text[key_at..];
    let start = rest.find("<string>")? + "<string>".len();
    let end = rest[start..].find("</string>")? + start;
    let raw = rest[start..end].trim();
    if raw.is_empty() {
        return None;
    }
    Some(
        raw.replace("&amp;", "&")
            .replace("&lt;", "<")
            .replace("&gt;", ">")
            .replace("&quot;", "\"")
            .replace("&apos;", "'"),
    )
}

/// Turn a user-typed link name into a safe `.url` file name (min-paths `names`: separators and control characters
/// become "-", a trailing ".url" is not doubled, a Windows reserved name gets a "_", an empty name falls back to the URL's
/// host).
pub fn web_link_file_name(name: &str, url: &str) -> String { min_paths::names::web_link_file_name(name, url) }

/// Create `<dir>/<name>.url` pointing at `url`. Never overwrites: a
/// collision gets a " 2", " 3", … suffix. Returns the created path.
pub fn create_web_link(dir: &str, name: &str, url: &str) -> Result<String, String> {
    let url = url.trim();
    if url.is_empty() {
        return Err("URL is empty".into());
    }
    // Prepend a scheme for bare hosts so both OSes hand it to a
    // browser rather than a file handler.
    let url = if url.contains("://") || url.starts_with("mailto:") {
        url.to_string()
    } else {
        format!("https://{}", url)
    };
    let parent = Path::new(dir);
    if !parent.is_dir() {
        return Err(format!("Not a directory: {}", dir));
    }
    let file_name = web_link_file_name(name, &url);
    let stem = &file_name[..file_name.len() - 4];
    let mut target = parent.join(&file_name);
    let mut n = 2;
    while target.exists() {
        target = parent.join(format!("{} {}.url", stem, n));
        n += 1;
        if n > 10_000 {
            return Err("Could not find a free file name".into());
        }
    }
    // CRLF: what Windows writes, and what every reader accepts.
    let body = format!("[InternetShortcut]\r\nURL={}\r\n", url);
    std::fs::write(&target, body).map_err(|e| format!("Failed to write link: {}", e))?;
    Ok(target.to_string_lossy().into_owned())
}

/// Video extensions that, on macOS, are handed off to QCView.app
/// instead of the OS default handler. Lowercase, no leading dot.
#[cfg(target_os = "macos")]
const QCVIEW_EXTS: &[&str] = &[
    "mp4", "mov", "m4v", "mkv", "webm", "avi", "mxf", "mpg", "mpeg", "m2v", "exr",
];

/// True if `path` has a video extension we route to QCView.
#[cfg(target_os = "macos")]
fn is_qcview_video(path: &str) -> bool {
    Path::new(path)
        .extension()
        .and_then(|e| e.to_str())
        .map(|e| {
            let e = e.to_ascii_lowercase();
            QCVIEW_EXTS.contains(&e.as_str())
        })
        .unwrap_or(false)
}

/// Open a file with the default application.
///
/// On macOS, video files (see [`QCVIEW_EXTS`]) are handed off to
/// QCView.app via `open -a QCView`. If QCView can't be launched
/// (not installed, etc.) we fall back to the OS default handler so
/// the user still gets *something*.
pub fn open_file(path: &str) -> Result<(), String> {
    // Web links: open the TARGET in the default browser ourselves so
    // .url and .webloc behave the same on both OSes (Finder ignores
    // .webloc files created elsewhere on some versions, Explorer
    // never understood .webloc at all).
    if is_web_link_path(path) {
        return match read_web_link(path) {
            Some(url) => opener::open(&url)
                .map_err(|e| format!("Failed to open link {}: {}", url, e)),
            None => Err(format!("{} is not a readable web link", path)),
        };
    }
    #[cfg(target_os = "macos")]
    {
        if is_qcview_video(path) {
            match std::process::Command::new("open")
                .arg("-a")
                .arg("QCView")
                .arg(path)
                .status()
            {
                Ok(s) if s.success() => return Ok(()),
                Ok(s) => log::warn!(
                    "open -a QCView {} exited {}; falling back to default app",
                    path,
                    s
                ),
                Err(e) => log::warn!(
                    "open -a QCView {} failed: {}; falling back to default app",
                    path,
                    e
                ),
            }
        }
    }
    opener::open(path).map_err(|e| format!("Failed to open: {}", e))
}

// ──────────────────────────────────────────────────────────────────────
// Copy / Move / Delete with progress events
// ──────────────────────────────────────────────────────────────────────

use crate::events::{
    FileOpCompleted, FileOpError, FileOpProgress, FileOpStarted, FileOpsEventsArc,
};
use std::sync::atomic::{AtomicU64, Ordering};

/// Deduped set of parent directories of `paths`, in first-seen order.
/// Native path strings (same form the caller passed in) — the UI
/// normalizes for comparison against a view's current path.
fn parent_dirs(paths: &[String]) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for p in paths {
        if let Some(parent) = Path::new(p).parent() {
            let s = parent.to_string_lossy().to_string();
            if !s.is_empty() && !out.contains(&s) {
                out.push(s);
            }
        }
    }
    out
}

#[cfg(test)]
mod changed_dirs_tests {
    use super::parent_dirs;

    #[cfg(windows)]
    const A: &str = r"C:\jobs\shotA\file1.mov";
    #[cfg(windows)]
    const B: &str = r"C:\jobs\shotA\file2.mov";
    #[cfg(windows)]
    const C: &str = r"C:\jobs\shotB\file3.mov";

    #[cfg(not(windows))]
    const A: &str = "/jobs/shotA/file1.mov";
    #[cfg(not(windows))]
    const B: &str = "/jobs/shotA/file2.mov";
    #[cfg(not(windows))]
    const C: &str = "/jobs/shotB/file3.mov";

    #[test]
    fn parent_dirs_dedupes_same_parent() {
        // file1 and file2 share a parent; it appears once.
        let dirs = parent_dirs(&[A.to_string(), B.to_string()]);
        assert_eq!(dirs.len(), 1);
    }

    #[test]
    fn parent_dirs_keeps_distinct_parents_in_order() {
        let dirs = parent_dirs(&[A.to_string(), C.to_string()]);
        assert_eq!(dirs.len(), 2);
        assert!(dirs[0].ends_with("shotA"));
        assert!(dirs[1].ends_with("shotB"));
    }

    #[test]
    fn parent_dirs_empty_for_no_paths() {
        assert!(parent_dirs(&[]).is_empty());
    }
}

/// Generate a unique operation id for progress tracking.
fn new_op_id(prefix: &str) -> String {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    format!("{}-{}-{}", prefix, std::process::id(), n)
}

/// Drop guards (min-paths `volume::drop_guard`): a file dropped back where it came from (fs_extra would truncate the
/// source), or a folder dropped into itself / one of its own subfolders. Skip these items.
fn is_drop_onto_self(src: &Path, target: &Path) -> bool {
    match min_paths::volume::drop_guard(src, target) {
        Some(reason) => { log::info!("[drop guard] {}: src={} target={}", reason, src.display(), target.display()); true }
        None => false,
    }
}

/// Copy files / directories with progress events.
pub async fn copy_files(
    sources: Vec<String>,
    dest: String,
    events: FileOpsEventsArc,
) -> Result<(), String> {
    let op_id = new_op_id("copy");
    log::info!(
        "[copy] op={} sources={} dest={} (paths: {:?})",
        op_id,
        sources.len(),
        dest,
        sources
    );
    events.op_started(FileOpStarted {
        id: op_id.clone(),
        operation: "copy".to_string(),
        items_total: sources.len(),
    });

    // Copy adds entries to the destination directory only.
    let changed_dirs = vec![dest.clone()];

    let events_inner = events.clone();
    let op_id_inner = op_id.clone();

    let result = tokio::task::spawn_blocking(move || {
        let dest_path = Path::new(&dest);
        let total = sources.len();
        let mut last_emit = std::time::Instant::now();
        let mut errors: Vec<String> = Vec::new();
        let mut succeeded: usize = 0;

        for (i, src) in sources.iter().enumerate() {
            let src_path = Path::new(src);
            let file_name = match src_path.file_name() {
                Some(f) => f,
                None => {
                    errors.push(format!("Invalid source path: {}", src));
                    continue;
                }
            };
            let target = dest_path.join(file_name);

            if is_drop_onto_self(src_path, &target) {
                log::info!(
                    "[copy] skipping drop-onto-self: src={} target={}",
                    src_path.display(),
                    target.display()
                );
                continue;
            }

            // Sync-aware: copy within sync root creates a dehydrated placeholder
            match crate::sync_aware::sync_copy(src_path, dest_path) {
                Ok(true) => {
                    succeeded += 1;
                    continue;
                }
                Ok(false) => {}
                Err(e) => {
                    errors.push(e);
                    continue;
                }
            }

            let item_result = if src_path.is_dir() {
                let mut options = fs_extra::dir::CopyOptions::new();
                options.overwrite = true;
                options.copy_inside = true;
                let events_cb = events_inner.clone();
                let op_id_cb = op_id_inner.clone();
                fs_extra::dir::copy_with_progress(src_path, &target, &options, |info| {
                    if last_emit.elapsed().as_millis() >= 100 {
                        events_cb.op_progress(FileOpProgress {
                            id: op_id_cb.clone(),
                            total_bytes: info.total_bytes,
                            copied_bytes: info.copied_bytes,
                            current_file: info.file_name.clone(),
                            items_total: total,
                            items_done: i,
                        });
                        last_emit = std::time::Instant::now();
                    }
                    fs_extra::dir::TransitProcessResult::ContinueOrAbort
                })
                .map_err(|e| format!("Failed to copy dir '{}': {}", src, e))
                .map(|_| ())
            } else {
                let mut options = fs_extra::file::CopyOptions::new();
                options.overwrite = true;
                let events_cb = events_inner.clone();
                let op_id_cb = op_id_inner.clone();
                let fname = file_name.to_string_lossy().to_string();
                fs_extra::file::copy_with_progress(src_path, &target, &options, |info| {
                    if last_emit.elapsed().as_millis() >= 100 {
                        events_cb.op_progress(FileOpProgress {
                            id: op_id_cb.clone(),
                            total_bytes: info.total_bytes,
                            copied_bytes: info.copied_bytes,
                            current_file: fname.clone(),
                            items_total: total,
                            items_done: i,
                        });
                        last_emit = std::time::Instant::now();
                    }
                })
                .map_err(|e| format!("Failed to copy file '{}': {}", src, e))
                .map(|_| ())
            };

            match item_result {
                Ok(()) => succeeded += 1,
                Err(e) => errors.push(e),
            }
        }
        (succeeded, errors)
    })
    .await
    .map_err(|e| format!("Copy task failed: {}", e))?;

    let (succeeded, errors) = result;
    let failed = errors.len();

    if failed > 0 && succeeded == 0 {
        events.op_error(FileOpError {
            id: op_id.clone(),
            error: errors.join("; "),
        });
        return Err(errors.join("; "));
    }

    events.op_completed(FileOpCompleted {
        id: op_id,
        succeeded,
        failed,
        errors,
        changed_dirs,
    });

    Ok(())
}

/// Move files / directories with progress events.
pub async fn move_files(
    sources: Vec<String>,
    dest: String,
    events: FileOpsEventsArc,
) -> Result<(), String> {
    let op_id = new_op_id("move");
    log::info!(
        "[move] op={} sources={} dest={} (paths: {:?})",
        op_id,
        sources.len(),
        dest,
        sources
    );
    events.op_started(FileOpStarted {
        id: op_id.clone(),
        operation: "move".to_string(),
        items_total: sources.len(),
    });

    // Move removes from each source's parent and adds to the dest dir.
    let changed_dirs = {
        let mut d = parent_dirs(&sources);
        if !d.contains(&dest) {
            d.push(dest.clone());
        }
        d
    };

    let events_inner = events.clone();
    let op_id_inner = op_id.clone();

    let result = tokio::task::spawn_blocking(move || {
        let dest_path = Path::new(&dest);
        let total = sources.len();
        let mut last_emit = std::time::Instant::now();
        let mut errors: Vec<String> = Vec::new();
        let mut succeeded: usize = 0;

        for (i, src) in sources.iter().enumerate() {
            let src_path = Path::new(src);
            log::info!(
                "[move] op={} item {}/{}: src={}",
                op_id_inner,
                i + 1,
                total,
                src_path.display()
            );
            let file_name = match src_path.file_name() {
                Some(f) => f,
                None => {
                    log::warn!("[move] op={} invalid source path: {}", op_id_inner, src);
                    errors.push(format!("Invalid source path: {}", src));
                    continue;
                }
            };
            let target = dest_path.join(file_name);
            log::info!(
                "[move] op={} item {}/{}: target={}",
                op_id_inner,
                i + 1,
                total,
                target.display()
            );

            if is_drop_onto_self(src_path, &target) {
                log::info!(
                    "[move] skipping drop-onto-self: src={} target={}",
                    src_path.display(),
                    target.display()
                );
                continue;
            }

            // Sync-aware: move within sync root uses fs::rename
            match crate::sync_aware::sync_move(src_path, dest_path) {
                Ok(true) => {
                    log::info!("[move] op={} item {}/{} via sync_move (rename) OK", op_id_inner, i + 1, total);
                    succeeded += 1;
                    continue;
                }
                Ok(false) => {
                    log::debug!("[move] op={} item {}/{}: sync_move declined, falling back to fs_extra", op_id_inner, i + 1, total);
                }
                Err(e) => {
                    log::warn!("[move] op={} item {}/{} sync_move ERR: {}", op_id_inner, i + 1, total, e);
                    errors.push(e);
                    continue;
                }
            }

            let item_result = if src_path.is_dir() {
                let mut options = fs_extra::dir::CopyOptions::new();
                options.overwrite = true;
                options.copy_inside = true;
                let events_cb = events_inner.clone();
                let op_id_cb = op_id_inner.clone();
                fs_extra::dir::move_dir_with_progress(src_path, &target, &options, |info| {
                    if last_emit.elapsed().as_millis() >= 100 {
                        events_cb.op_progress(FileOpProgress {
                            id: op_id_cb.clone(),
                            total_bytes: info.total_bytes,
                            copied_bytes: info.copied_bytes,
                            current_file: info.file_name.clone(),
                            items_total: total,
                            items_done: i,
                        });
                        last_emit = std::time::Instant::now();
                    }
                    fs_extra::dir::TransitProcessResult::ContinueOrAbort
                })
                .map_err(|e| format!("Failed to move dir '{}': {}", src, e))
                .map(|_| ())
            } else {
                let mut options = fs_extra::file::CopyOptions::new();
                options.overwrite = true;
                let events_cb = events_inner.clone();
                let op_id_cb = op_id_inner.clone();
                let fname = file_name.to_string_lossy().to_string();
                fs_extra::file::move_file_with_progress(src_path, &target, &options, |info| {
                    if last_emit.elapsed().as_millis() >= 100 {
                        events_cb.op_progress(FileOpProgress {
                            id: op_id_cb.clone(),
                            total_bytes: info.total_bytes,
                            copied_bytes: info.copied_bytes,
                            current_file: fname.clone(),
                            items_total: total,
                            items_done: i,
                        });
                        last_emit = std::time::Instant::now();
                    }
                })
                .map_err(|e| format!("Failed to move file '{}': {}", src, e))
                .map(|_| ())
            };

            match item_result {
                Ok(()) => {
                    log::info!("[move] op={} item {}/{} via fs_extra OK: {} → {}", op_id_inner, i + 1, total, src_path.display(), target.display());
                    succeeded += 1;
                }
                Err(e) => {
                    log::warn!("[move] op={} item {}/{} fs_extra ERR: {}", op_id_inner, i + 1, total, e);
                    errors.push(e);
                }
            }
        }
        log::info!("[move] op={} done: succeeded={} errors={}", op_id_inner, succeeded, errors.len());
        (succeeded, errors)
    })
    .await
    .map_err(|e| format!("Move task failed: {}", e))?;

    let (succeeded, errors) = result;
    let failed = errors.len();

    if failed > 0 && succeeded == 0 {
        events.op_error(FileOpError {
            id: op_id.clone(),
            error: errors.join("; "),
        });
        return Err(errors.join("; "));
    }

    events.op_completed(FileOpCompleted {
        id: op_id,
        succeeded,
        failed,
        errors,
        changed_dirs,
    });

    Ok(())
}

/// Delete files to trash with progress events.
/// Falls back to `fallback_delete` (SHFileOperationW on Windows) for paths
/// that can't be recycled (network/junction).
pub async fn delete_to_trash_with_progress(
    paths: Vec<String>,
    events: FileOpsEventsArc,
) -> Result<(), String> {
    let op_id = new_op_id("delete");
    events.op_started(FileOpStarted {
        id: op_id.clone(),
        operation: "delete".to_string(),
        items_total: paths.len(),
    });

    // Delete removes entries from each source's parent directory.
    let changed_dirs = parent_dirs(&paths);

    let events_inner = events.clone();
    let op_id_inner = op_id.clone();

    let result = tokio::task::spawn_blocking(move || {
        let total = paths.len();
        let mut errors: Vec<String> = Vec::new();
        let mut succeeded: usize = 0;
        let mut trash_failed: Vec<String> = Vec::new();

        for (i, path) in paths.iter().enumerate() {
            events_inner.op_progress(FileOpProgress {
                id: op_id_inner.clone(),
                total_bytes: 0,
                copied_bytes: 0,
                current_file: path.clone(),
                items_total: total,
                items_done: i,
            });

            if try_trash_one(path).is_ok() {
                succeeded += 1;
            } else {
                trash_failed.push(path.clone());
            }
        }

        if !trash_failed.is_empty() {
            match fallback_delete(&trash_failed) {
                Ok(()) => succeeded += trash_failed.len(),
                Err(e) => {
                    for path in &trash_failed {
                        errors.push(format!("{}: {}", path, e));
                    }
                }
            }
        }

        (succeeded, errors)
    })
    .await
    .map_err(|e| format!("Delete task failed: {}", e))?;

    let (succeeded, errors) = result;
    let failed = errors.len();

    if failed > 0 && succeeded == 0 {
        events.op_error(FileOpError {
            id: op_id.clone(),
            error: errors.join("; "),
        });
        return Err(errors.join("; "));
    }

    events.op_completed(FileOpCompleted {
        id: op_id,
        succeeded,
        failed,
        errors,
        changed_dirs,
    });

    Ok(())
}

#[cfg(test)]
mod web_link_tests {
    use super::*;

    #[test]
    fn parses_url_ini_and_webloc() {
        assert_eq!(
            parse_url_ini("\u{feff}[InternetShortcut]\r\nURL=https://a.b/c?d=1\r\nIconIndex=0\r\n").as_deref(),
            Some("https://a.b/c?d=1")
        );
        assert_eq!(parse_url_ini("[InternetShortcut]\nurl = https://x\n").as_deref(), Some("https://x"));
        assert_eq!(parse_url_ini("[InternetShortcut]\n"), None);
        let plist = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>URL</key><string>https://e.f/?a=1&amp;b=2</string></dict></plist>";
        assert_eq!(parse_webloc_xml(plist).as_deref(), Some("https://e.f/?a=1&b=2"));
        assert_eq!(parse_webloc_xml("<plist/>"), None);
        assert!(is_web_link_path("/x/Site.URL"));
        assert!(is_web_link_path("/x/site.webloc"));
        assert!(!is_web_link_path("/x/site.html"));
    }

    #[test]
    fn link_file_names_are_safe() {
        assert_eq!(web_link_file_name("Client / Brief: v2", "https://x"), "Client - Brief- v2.url");
        assert_eq!(web_link_file_name("notes.url", "https://x"), "notes.url");
        assert_eq!(web_link_file_name("", "https://www.frame.io/proj?x"), "frame.io.url");
        assert_eq!(web_link_file_name("  ", ""), "Link.url");
    }

    #[test]
    fn create_and_read_round_trip() {
        let dir = std::env::temp_dir().join(format!("ufb-weblink-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let d = dir.to_string_lossy().into_owned();
        let p1 = create_web_link(&d, "Brief", "frame.io/x").unwrap();
        assert!(p1.ends_with("Brief.url"));
        assert_eq!(read_web_link(&p1).as_deref(), Some("https://frame.io/x"));
        let p2 = create_web_link(&d, "Brief", "https://other").unwrap();
        assert!(p2.ends_with("Brief 2.url"), "{}", p2);
        assert!(create_web_link(&d, "x", "   ").is_err());
        assert!(create_web_link(&format!("{}/missing", d), "x", "https://y").is_err());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
