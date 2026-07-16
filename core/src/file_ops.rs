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
use std::sync::{Mutex, OnceLock};

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

/// Process-global registry of "paths that UFB itself most recently
/// cut" plus the OS clipboard sequence number captured at write time.
/// Used so paste, in any pane / tab, can ask "did *we* cut this?"
/// without per-pane state. The sequence number lets us detect when
/// another app has overwritten the clipboard since our cut, in which
/// case the record is stale and paste falls back to copy.
struct LastCutRecord {
    paths: Vec<String>, // sorted, for set-equality compare
    sequence: i64,
}

fn last_cut_registry() -> &'static Mutex<Option<LastCutRecord>> {
    static REG: OnceLock<Mutex<Option<LastCutRecord>>> = OnceLock::new();
    REG.get_or_init(|| Mutex::new(None))
}

/// Current OS clipboard generation. Windows: GetClipboardSequenceNumber.
/// macOS: NSPasteboard.changeCount. Both increment monotonically every
/// time anything writes to the system clipboard.
fn current_clipboard_sequence() -> i64 {
    #[cfg(target_os = "windows")]
    {
        unsafe {
            windows::Win32::System::DataExchange::GetClipboardSequenceNumber() as i64
        }
    }
    #[cfg(target_os = "macos")]
    {
        // objc2-app-kit ≥ 0.3 demarcates these as safe and returns
        // `isize` from changeCount. Cast widens to our cross-platform
        // i64 sequence type.
        use objc2_app_kit::NSPasteboard;
        let pb = NSPasteboard::generalPasteboard();
        pb.changeCount() as i64
    }
    #[cfg(not(any(target_os = "windows", target_os = "macos")))]
    {
        0
    }
}

fn record_cut(paths: &[String]) {
    let mut sorted: Vec<String> = paths.to_vec();
    sorted.sort();
    let seq = current_clipboard_sequence();
    *last_cut_registry().lock().unwrap() = Some(LastCutRecord { paths: sorted, sequence: seq });
}

fn clear_cut_record() {
    *last_cut_registry().lock().unwrap() = None;
}

/// True if the registry's most recent cut still owns the clipboard
/// (sequence matches) AND the path set equals `paths`.
fn registry_matches(paths: &[String]) -> bool {
    let reg = last_cut_registry().lock().unwrap();
    let Some(rec) = reg.as_ref() else { return false };
    if rec.sequence != current_clipboard_sequence() {
        return false;
    }
    let mut sorted: Vec<String> = paths.to_vec();
    sorted.sort();
    sorted == rec.paths
}

/// Copy file paths to clipboard.
pub fn clipboard_copy_paths(paths: &[String]) -> Result<(), String> {
    #[cfg(target_os = "windows")]
    let r = clipboard_set_paths_windows(paths, 0x01); // DROPEFFECT_COPY
    #[cfg(target_os = "macos")]
    let r = clipboard_copy_paths_macos(paths);
    if r.is_ok() {
        clear_cut_record();
    }
    r
}

/// Cut file paths to clipboard. On Windows, sets the
/// "Preferred DropEffect" clipboard format to DROPEFFECT_MOVE so that
/// Explorer interprets a paste as a move (with the visual cut-pending
/// fade on the source items). On macOS, the pasteboard has no native
/// move/copy distinction at the file-URL layer; this is the same as
/// `clipboard_copy_paths`. The "cut-pending" visual treatment lives
/// in the UI side regardless.
pub fn clipboard_cut_paths(paths: &[String]) -> Result<(), String> {
    #[cfg(target_os = "windows")]
    let r = clipboard_set_paths_windows(paths, 0x02); // DROPEFFECT_MOVE
    #[cfg(target_os = "macos")]
    let r = clipboard_copy_paths_macos(paths);
    if r.is_ok() {
        record_cut(paths);
    }
    r
}

#[cfg(target_os = "windows")]
fn clipboard_set_paths_windows(paths: &[String], drop_effect: u32) -> Result<(), String> {
    use clipboard_win::{raw, Clipboard};

    let _clip = Clipboard::new_attempts(10)
        .map_err(|e| format!("Failed to open clipboard: {}", e))?;

    raw::empty().map_err(|e| format!("Failed to empty clipboard: {}", e))?;

    raw::set_file_list(paths)
        .map_err(|e| format!("Failed to set clipboard CF_HDROP: {}", e))?;

    set_preferred_drop_effect(drop_effect);

    Ok(())
}

#[cfg(target_os = "windows")]
fn set_preferred_drop_effect(effect: u32) {
    use windows::core::w;
    use windows::Win32::System::DataExchange::{RegisterClipboardFormatW, SetClipboardData};
    use windows::Win32::System::Memory::{GlobalAlloc, GlobalLock, GlobalUnlock, GMEM_MOVEABLE};

    unsafe {
        let fmt = RegisterClipboardFormatW(w!("Preferred DropEffect"));
        if fmt == 0 {
            return;
        }
        let hmem = GlobalAlloc(GMEM_MOVEABLE, 4);
        if let Ok(hmem) = hmem {
            let lock: *mut std::ffi::c_void = GlobalLock(hmem);
            if !lock.is_null() {
                std::ptr::write(lock as *mut u32, effect);
                let _ = GlobalUnlock(hmem);
            }
            let _ = SetClipboardData(fmt, windows::Win32::Foundation::HANDLE(hmem.0));
        }
    }
}

/// Write file URLs the way Finder does: one NSURL object per file via
/// `writeObjects:`. AppKit fans each URL out into a full pasteboard item
/// (`public.file-url` + auto-translated legacy flavors), so Finder and
/// every modern app can paste all of them. The previous implementation
/// wrote a single legacy `NSFilenamesPboardType` plist string, which the
/// UTI bridge collapsed to ONE item — multi-file copies pasted only the
/// first file, and some readers saw nothing at all.
#[cfg(target_os = "macos")]
fn clipboard_copy_paths_macos(paths: &[String]) -> Result<(), String> {
    use objc2::runtime::ProtocolObject;
    use objc2_app_kit::{NSPasteboard, NSPasteboardWriting};
    use objc2_foundation::{NSArray, NSString, NSURL};

    if paths.is_empty() {
        return Err("No valid file paths to copy".into());
    }

    let urls: Vec<objc2::rc::Retained<NSURL>> = paths
        .iter()
        .map(|p| {
            let resolved =
                std::fs::canonicalize(p).unwrap_or_else(|_| std::path::PathBuf::from(p));
            NSURL::fileURLWithPath(&NSString::from_str(&resolved.to_string_lossy()))
        })
        .collect();
    let objects: Vec<&ProtocolObject<dyn NSPasteboardWriting>> =
        urls.iter().map(|u| ProtocolObject::from_ref(&**u)).collect();

    let pasteboard = NSPasteboard::generalPasteboard();
    pasteboard.clearContents();
    if pasteboard.writeObjects(&NSArray::from_slice(&objects)) {
        Ok(())
    } else {
        Err("Failed to write file paths to pasteboard".into())
    }
}

/// Read file URLs via `readObjectsForClasses:` — the canonical AppKit
/// paste path. Unlike a manual `public.file-url` string scan, this
/// resolves every URL flavor a writer may have used (per-item file-url
/// strings, legacy `NSFilenamesPboardType` plists, AppleScript `furl`
/// data, `file://localhost/` forms) and hands back decoded POSIX paths.
#[cfg(target_os = "macos")]
fn clipboard_paste_paths_macos() -> Result<Vec<String>, String> {
    use objc2_app_kit::NSPasteboard;
    use objc2_foundation::{NSArray, NSURL};

    let pasteboard = NSPasteboard::generalPasteboard();
    let classes = NSArray::from_slice(&[objc2::class!(NSURL)]);
    let objects = unsafe { pasteboard.readObjectsForClasses_options(&classes, None) };

    let mut paths = Vec::new();
    if let Some(objects) = objects {
        for obj in objects.iter() {
            if let Some(url) = obj.downcast_ref::<NSURL>() {
                if url.isFileURL() {
                    if let Some(p) = url.path() {
                        paths.push(p.to_string());
                    }
                }
            }
        }
    }

    Ok(paths)
}

/// Heuristic check: does this string look like a filesystem path?
fn looks_like_path(s: &str) -> bool {
    if s.len() >= 3
        && s.as_bytes()[1] == b':'
        && (s.as_bytes()[2] == b'\\' || s.as_bytes()[2] == b'/')
    {
        return s.as_bytes()[0].is_ascii_alphabetic();
    }
    if s.starts_with("\\\\") || s.starts_with("//") {
        return true;
    }
    if s.starts_with('/') {
        return true;
    }
    false
}

/// Paste file paths from clipboard.
pub fn clipboard_paste_paths() -> Result<Vec<String>, String> {
    #[cfg(target_os = "windows")]
    {
        if let Ok(paths) = clipboard_paste_paths_hdrop() {
            if !paths.is_empty() {
                return Ok(paths);
            }
        }
    }

    #[cfg(target_os = "macos")]
    {
        if let Ok(paths) = clipboard_paste_paths_macos() {
            if !paths.is_empty() {
                return Ok(paths);
            }
        }
    }

    // Fallback: read plain text
    let mut clipboard =
        arboard::Clipboard::new().map_err(|e| format!("Failed to open clipboard: {}", e))?;
    let text = clipboard
        .get_text()
        .map_err(|e| format!("Failed to read clipboard: {}", e))?;
    let paths: Vec<String> = text
        .lines()
        .map(|l| l.trim().to_string())
        .filter(|l| !l.is_empty() && looks_like_path(l))
        .collect();
    Ok(paths)
}

#[cfg(target_os = "windows")]
fn clipboard_paste_paths_hdrop() -> Result<Vec<String>, String> {
    use clipboard_win::{raw, Clipboard};

    let _clip = Clipboard::new_attempts(10)
        .map_err(|e| format!("Failed to open clipboard: {}", e))?;

    let mut file_list: Vec<String> = Vec::new();
    raw::get_file_list(&mut file_list)
        .map_err(|e| format!("Failed to read CF_HDROP: {}", e))?;

    let paths: Vec<String> = file_list.into_iter().filter(|s| !s.is_empty()).collect();

    Ok(paths)
}

/// Read paths and resolve move-vs-copy intent in one pass.
///
/// - **Windows**: opens the clipboard once, reads `CF_HDROP` for the
///   paths and `"Preferred DropEffect"` for the effect. Explorer's
///   own Cut sets DROPEFFECT_MOVE, so this transparently supports
///   Explorer→UFB cut→paste = move.
/// - **macOS**: there is no native move/copy distinction at the
///   pasteboard layer. The effect is `Move` only when our internal
///   registry shows that *we* cut these exact paths and the clipboard
///   hasn't been touched since. Anything else is `Copy`.
/// - **Plain text fallback**: always `Copy` — text has no move semantics.
pub fn clipboard_paste_intent() -> Result<PasteIntent, String> {
    #[cfg(target_os = "windows")]
    {
        match clipboard_paste_intent_windows() {
            Ok(intent) if !intent.paths.is_empty() => return Ok(intent),
            _ => {}
        }
    }

    #[cfg(target_os = "macos")]
    {
        if let Ok(paths) = clipboard_paste_paths_macos() {
            if !paths.is_empty() {
                let effect = if registry_matches(&paths) {
                    PasteEffect::Move
                } else {
                    PasteEffect::Copy
                };
                return Ok(PasteIntent { paths, effect });
            }
        }
    }

    // Plain-text fallback (URLs/paths in clipboard text).
    let paths = clipboard_paste_paths()?;
    Ok(PasteIntent {
        paths,
        effect: PasteEffect::Copy,
    })
}

#[cfg(target_os = "windows")]
fn clipboard_paste_intent_windows() -> Result<PasteIntent, String> {
    use clipboard_win::{raw, Clipboard};

    let _clip = Clipboard::new_attempts(10)
        .map_err(|e| format!("Failed to open clipboard: {}", e))?;

    let mut file_list: Vec<String> = Vec::new();
    raw::get_file_list(&mut file_list)
        .map_err(|e| format!("Failed to read CF_HDROP: {}", e))?;
    let paths: Vec<String> = file_list.into_iter().filter(|s| !s.is_empty()).collect();

    if paths.is_empty() {
        return Ok(PasteIntent {
            paths,
            effect: PasteEffect::Copy,
        });
    }

    // Authoritative on Windows: Explorer/UFB Cut both write
    // DROPEFFECT_MOVE here. Fall back to the registry only if the
    // format isn't present at all (some apps publish CF_HDROP without
    // a Preferred DropEffect).
    let effect = match read_preferred_drop_effect_holding_clipboard() {
        Some(true) => PasteEffect::Move,
        Some(false) => PasteEffect::Copy,
        None => {
            if registry_matches(&paths) {
                PasteEffect::Move
            } else {
                PasteEffect::Copy
            }
        }
    };

    Ok(PasteIntent { paths, effect })
}

/// Read the `"Preferred DropEffect"` clipboard format. Caller must
/// already hold the clipboard open. Returns `Some(true)` for MOVE,
/// `Some(false)` for any other effect, `None` if the format isn't
/// present.
#[cfg(target_os = "windows")]
fn read_preferred_drop_effect_holding_clipboard() -> Option<bool> {
    use windows::core::w;
    use windows::Win32::Foundation::{HANDLE, HGLOBAL};
    use windows::Win32::System::DataExchange::{GetClipboardData, RegisterClipboardFormatW};
    use windows::Win32::System::Memory::{GlobalLock, GlobalUnlock};

    unsafe {
        let fmt = RegisterClipboardFormatW(w!("Preferred DropEffect"));
        if fmt == 0 {
            return None;
        }
        let handle: HANDLE = GetClipboardData(fmt).ok()?;
        let hmem = HGLOBAL(handle.0);
        let lock: *mut std::ffi::c_void = GlobalLock(hmem);
        if lock.is_null() {
            return None;
        }
        let effect = std::ptr::read(lock as *const u32);
        let _ = GlobalUnlock(hmem);
        // DROPEFFECT_MOVE = 0x02
        Some((effect & 0x02) != 0)
    }
}

/// Reveal a path in the native file manager. Selects the item in
/// its parent folder.
pub fn reveal_in_file_manager(path: &str) -> Result<(), String> {
    reveal_paths_in_file_manager(&[path.to_string()])
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

/// Create a date-prefixed note FILE. Format:
/// `{YYMMDD}{letter}_{base_name}.mndb` — the same shared a–z slot
/// walk as [`create_date_prefixed_item`], with two differences:
/// the result is an empty file rather than a directory, and the
/// slot scan counts every entry (files AND folders) with today's
/// prefix so a note and a folder can't end up sharing a letter.
/// `.mndb` is the WIP UFB notes format; the file is created empty
/// and the (future) notes app owns its contents.
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
            let file_name = format!("{}_{}.mndb", prefix, base_name);
            let full_path = parent_path.join(&file_name);
            std::fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&full_path)
                .map_err(|e| format!("create {:?}: {}", full_path, e))?;
            return Ok(full_path.to_string_lossy().into_owned());
        }
    }

    Err(format!(
        "All date-prefixed slots for {} are taken (a–z exhausted)",
        today
    ))
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
pub fn clipboard_copy_text(text: &str) -> Result<(), String> {
    arboard::Clipboard::new()
        .and_then(|mut cb| cb.set_text(text.to_string()))
        .map_err(|e| format!("Failed to write text to clipboard: {}", e))
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

/// Drop-onto-self guard. If user drops files back onto the folder they
/// came from (e.g. drag out → cursor flicks back into app → drop on
/// source panel), `target == src_path`. fs_extra opens the destination
/// with `File::create` BEFORE reading the source, which truncates the
/// source to zero bytes. Skip these items.
fn is_drop_onto_self(src: &Path, target: &Path) -> bool {
    if src == target {
        return true;
    }
    if let (Ok(s), Ok(t)) = (
        std::fs::canonicalize(src),
        std::fs::canonicalize(target.parent().unwrap_or(target)).map(|p| {
            target
                .file_name()
                .map(|fname| p.join(fname))
                .unwrap_or(p)
        }),
    ) {
        let norm = |p: &Path| {
            p.to_string_lossy()
                .replace('\\', "/")
                .trim_end_matches('/')
                .to_lowercase()
        };
        if norm(&s) == norm(&t) {
            return true;
        }
    }
    false
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
