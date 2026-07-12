use crate::file_ops::FileEntry;
use std::path::Path;
use std::process::Command;

#[cfg(target_os = "windows")]
use std::os::windows::process::CommandExt;
#[cfg(target_os = "windows")]
const CREATE_NO_WINDOW: u32 = 0x08000000;

/// Hard cap on results from any one search pass (matches es.exe's
/// `-max-results`).
const RESULT_CAP: usize = 200;
/// How many new walkdir hits accumulate before `on_batch` fires.
const BATCH_SIZE: usize = 25;

/// Search for files using platform-specific search tools.
/// Windows: Everything (es.exe), macOS: mdfind. Both fall back to a
/// recursive walk of the scope when the OS tool yields nothing — on
/// macOS this is the common case, because Spotlight has no usable
/// index for SMB/NFS mounts (all NAS and agent-mount paths).
pub fn search_files(query: &str, scope_path: Option<&str>) -> Result<Vec<FileEntry>, String> {
    search_files_cancellable(query, scope_path, &|| false, &mut |_| {})
}

/// Like [`search_files`], for callers that need to abandon or stream a
/// slow search (the recursive-walk fallback can run for a while on big
/// network trees):
/// - `should_stop` is polled throughout the walk; returning `true`
///   aborts it and yields whatever was found so far.
/// - `on_batch` receives *increments* — each call is only the entries
///   found since the previous call. The final return value is always
///   the complete result set.
pub fn search_files_cancellable(
    query: &str,
    scope_path: Option<&str>,
    should_stop: &dyn Fn() -> bool,
    on_batch: &mut dyn FnMut(Vec<FileEntry>),
) -> Result<Vec<FileEntry>, String> {
    #[cfg(target_os = "windows")]
    {
        search_everything(query, scope_path, should_stop, on_batch)
    }
    #[cfg(target_os = "macos")]
    {
        let found = search_mdfind(query, scope_path).unwrap_or_default();
        if !found.is_empty() {
            return Ok(found);
        }
        search_walkdir(query, scope_path, should_stop, on_batch)
    }
}

/// Windows: Use Everything's command-line interface (es.exe).
#[cfg(target_os = "windows")]
fn search_everything(
    query: &str,
    scope_path: Option<&str>,
    should_stop: &dyn Fn() -> bool,
    on_batch: &mut dyn FnMut(Vec<FileEntry>),
) -> Result<Vec<FileEntry>, String> {
    let full_query = if let Some(scope) = scope_path {
        format!("{} {}", scope, query)
    } else {
        query.to_string()
    };

    let es_paths = [
        r"C:\Program Files\Everything\es.exe",
        r"C:\Program Files (x86)\Everything\es.exe",
    ];

    let es_path = es_paths.iter().find(|p| Path::new(p).exists());

    if let Some(es) = es_path {
        let mut cmd = Command::new(es);
        cmd.args(["-max-results", "200", &full_query]);
        cmd.creation_flags(CREATE_NO_WINDOW);
        let output = cmd
            .output()
            .map_err(|e| format!("Failed to run es.exe: {}", e))?;

        let stdout = String::from_utf8_lossy(&output.stdout);
        parse_search_results(&stdout)
    } else {
        // Fallback: recursive walk of the scope
        search_walkdir(query, scope_path, should_stop, on_batch)
    }
}

/// macOS: Use Spotlight's mdfind. Only useful on locally indexed
/// volumes — network mounts return nothing (see `search_files`).
#[cfg(target_os = "macos")]
fn search_mdfind(query: &str, scope_path: Option<&str>) -> Result<Vec<FileEntry>, String> {
    let mut cmd = Command::new("mdfind");
    if let Some(scope) = scope_path {
        cmd.args(["-onlyin", scope]);
    }
    // Strip quotes so user input can't break out of the query literal.
    let sanitized: String = query.chars().filter(|c| *c != '\'' && *c != '"').collect();
    cmd.arg(format!("kMDItemDisplayName == '*{}*'c", sanitized));

    let output = cmd
        .output()
        .map_err(|e| format!("Failed to run mdfind: {}", e))?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    parse_search_results(&stdout)
}

/// Fallback: breadth-first directory walk with case-insensitive
/// substring name matching. Nearer (shallower) hits surface first.
/// Does not follow directory symlinks (network trees love cycles) and
/// does not descend into dot-directories.
fn search_walkdir(
    query: &str,
    scope_path: Option<&str>,
    should_stop: &dyn Fn() -> bool,
    on_batch: &mut dyn FnMut(Vec<FileEntry>),
) -> Result<Vec<FileEntry>, String> {
    use std::collections::VecDeque;

    let root = match scope_path {
        Some(r) if !r.is_empty() => r,
        // No scope — refuse to walk the whole filesystem.
        _ => return Ok(Vec::new()),
    };
    let query_lower = query.to_lowercase();

    let mut results: Vec<FileEntry> = Vec::new();
    let mut emitted = 0usize;
    let mut queue: VecDeque<std::path::PathBuf> = VecDeque::new();
    queue.push_back(root.into());

    'walk: while let Some(dir) = queue.pop_front() {
        if should_stop() {
            break;
        }
        let Ok(rd) = std::fs::read_dir(&dir) else {
            continue;
        };
        for entry in rd.flatten() {
            if should_stop() {
                break 'walk;
            }
            let name = entry.file_name().to_string_lossy().to_string();
            // file_type() doesn't follow symlinks, so a symlinked dir is
            // neither descended into nor misreported.
            let is_dir = entry.file_type().map(|t| t.is_dir()).unwrap_or(false);
            if name.to_lowercase().contains(&query_lower) {
                if let Some(fe) = file_entry_from_dir_entry(&entry, &name) {
                    results.push(fe);
                }
                if results.len() - emitted >= BATCH_SIZE {
                    on_batch(results[emitted..].to_vec());
                    emitted = results.len();
                }
                if results.len() >= RESULT_CAP {
                    break 'walk;
                }
            }
            if is_dir && !name.starts_with('.') {
                queue.push_back(entry.path());
            }
        }
    }

    Ok(results)
}

fn file_entry_from_dir_entry(entry: &std::fs::DirEntry, name: &str) -> Option<FileEntry> {
    let metadata = entry.metadata().ok()?;
    let path = entry.path();
    let extension = path
        .extension()
        .map(|e| e.to_string_lossy().to_string())
        .unwrap_or_default();
    let modified = metadata
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as i64);
    Some(FileEntry {
        name: name.to_string(),
        path: path.to_string_lossy().to_string(),
        is_dir: metadata.is_dir(),
        size: metadata.len(),
        modified,
        extension,
    })
}

/// Parse newline-separated file paths into FileEntry list.
fn parse_search_results(output: &str) -> Result<Vec<FileEntry>, String> {
    let entries: Vec<FileEntry> = output
        .lines()
        .filter(|line| !line.is_empty())
        .take(RESULT_CAP)
        .filter_map(|line| {
            let path = Path::new(line.trim());
            let metadata = std::fs::metadata(path).ok()?;
            let name = path.file_name()?.to_string_lossy().to_string();
            let extension = path
                .extension()
                .map(|e| e.to_string_lossy().to_string())
                .unwrap_or_default();
            let modified = metadata
                .modified()
                .ok()
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map(|d| d.as_millis() as i64);
            Some(FileEntry {
                name,
                path: line.trim().to_string(),
                is_dir: metadata.is_dir(),
                size: metadata.len(),
                modified,
                extension,
            })
        })
        .collect();
    Ok(entries)
}
