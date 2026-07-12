use std::path::PathBuf;

/// Get the app data directory (platform-specific).
/// Windows: %LOCALAPPDATA%/ufb/
/// macOS: ~/Library/Application Support/ufb/
pub fn get_app_data_dir() -> PathBuf {
    let base = dirs::config_local_dir().unwrap_or_else(|| PathBuf::from("."));
    let dir = base.join("ufb");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

/// Get or create a persistent device ID.
pub fn get_device_id() -> String {
    let id_path = get_app_data_dir().join("device_id.txt");
    if let Ok(id) = std::fs::read_to_string(&id_path) {
        let id = id.trim().to_string();
        if !id.is_empty() {
            return id;
        }
    }
    let id = uuid::Uuid::new_v4().to_string();
    let _ = std::fs::write(&id_path, &id);
    id
}

/// Get the database file path.
///
/// `ufb_v5.db` is the clean-DB epoch (mesh epoch 6): a deliberate CLEAN
/// BREAK from `ufb_v4.db`, which accumulated duplicate `column_definitions`
/// via the NULL-uuid snapshot-restore leak (now fixed in snapshot_worker).
/// There is no migration — a v5 node starts with an empty DB and converges
/// with peers over the v6 mesh, re-deriving columns from the clean shared
/// templates. The old `ufb_v4.db` / `ufb_v3.db` are left untouched for
/// old-build rollback.
pub fn get_database_path() -> PathBuf {
    get_app_data_dir().join("ufb_v5.db")
}

/// Get the settings file path.
pub fn get_settings_path() -> PathBuf {
    get_app_data_dir().join("settings.json")
}

/// Get current time in milliseconds since epoch.
pub fn current_time_ms() -> i64 {
    chrono::Utc::now().timestamp_millis()
}

/// Derive a stable project-slug for `job_path` relative to
/// `farm_root`. Used by the auto-promote-templates flow to scope
/// templates to a project namespace shared across peers (two peers
/// looking at the same job on the same share produce the same
/// slug, regardless of how they have the share locally mounted).
///
/// Strips `farm_root` from `job_path` (case-insensitive prefix
/// match), normalises path separators to `/`, and slugs the result
/// (lowercase alnum + underscore, max 64 chars).
///
/// When `job_path` doesn't start with `farm_root` (job is somewhere
/// else on the user's local disk), falls back to slugging the
/// final two path components as a best-effort namespace. Local-
/// only columns shouldn't reach this code path in production —
/// the caller filters via `is_shared_folder` first — but the
/// fallback keeps tests and edge cases sane.
pub fn project_slug_for(job_path: &str, farm_root: &str) -> String {
    let trimmed = job_path
        .trim_end_matches(['/', '\\']);
    let farm_lower = farm_root.trim_end_matches(['/', '\\']).to_lowercase();
    let path_lower = trimmed.to_lowercase();
    let relative: String = if !farm_lower.is_empty()
        && (path_lower.starts_with(&format!("{}/", farm_lower))
            || path_lower.starts_with(&format!("{}\\", farm_lower)))
    {
        // Prefix match — slice off the farm root + the separator.
        // Use byte offsets from the lowercased version (path
        // separators are single ASCII bytes), but slice the
        // original-case path so the result preserves any case the
        // user had in their job names.
        trimmed[farm_lower.len() + 1..].to_string()
    } else if !farm_lower.is_empty() && path_lower == farm_lower {
        // job_path IS the farm root (degenerate but possible) —
        // treat as the unscoped namespace.
        return "_root".to_string();
    } else {
        // No farm prefix match — fall back to the last two path
        // components so we still produce a recognisable slug.
        let parts: Vec<&str> = trimmed.rsplit(['/', '\\']).take(2).collect();
        parts.into_iter().rev().collect::<Vec<_>>().join("_")
    };
    slug_path_segment(&relative)
}

/// Lowercase + alnum/underscore + max 64 chars. Mirrors the
/// `slug_for` helper in `core::templates`. Defined here so callers
/// don't have to import templates just for the slug.
fn slug_path_segment(s: &str) -> String {
    let raw: String = s
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() {
                c.to_ascii_lowercase()
            } else {
                '_'
            }
        })
        .collect();
    let mut out = String::with_capacity(raw.len());
    let mut prev_under = false;
    for c in raw.chars() {
        if c == '_' {
            if !prev_under {
                out.push(c);
            }
            prev_under = true;
        } else {
            out.push(c);
            prev_under = false;
        }
    }
    let trimmed = out.trim_matches('_').to_string();
    if trimmed.is_empty() {
        "_unscoped".to_string()
    } else if trimmed.len() > 64 {
        trimmed.chars().take(64).collect()
    } else {
        trimmed
    }
}

/// Detect current OS tag for URI construction.
pub fn current_os_tag() -> &'static str {
    #[cfg(target_os = "windows")]
    {
        "win"
    }
    #[cfg(target_os = "macos")]
    {
        "mac"
    }
}

/// Build a UFB URI with OS prefix: ufb:///{os}/{path}
/// Example: ufb:///win/C:/Users/Alice/Desktop
pub fn build_path_uri(path: &str) -> String {
    let normalized = path.replace('\\', "/");
    let encoded = urlencoding::encode(&normalized);
    format!("ufb:///{}/{}", current_os_tag(), encoded)
}

/// Build a Union URI with OS prefix: union:///{os}/{path}
pub fn build_union_uri(path: &str) -> String {
    let normalized = path.replace('\\', "/");
    let encoded = urlencoding::encode(&normalized);
    format!("union:///{}/{}", current_os_tag(), encoded)
}

/// Convert a local filesystem path to a `file://` URL for a
/// `text/uri-list` drag payload. Returns `None` for any path that
/// cannot be expressed as a safe `file://` URL.
///
/// The critical case: a drive-less Windows path (`\Volumes\X`). Emitted
/// naively it becomes `file:////Volumes/X` (four slashes), which Windows
/// reads as the UNC `\\Volumes\X` — a non-existent host. The failed host
/// resolution wedges system-wide OLE drag-and-drop until a reboot.
/// Dropping the entry from the payload is strictly safer than poisoning
/// the whole drag. A genuine UNC path is emitted in proper
/// `file://host/share` form instead of the old `file://///…`.
pub fn path_to_file_url(path: &str) -> Option<String> {
    let normalised = path.replace('\\', "/");
    if cfg!(target_os = "windows") {
        if let Some(rest) = normalised.strip_prefix("//") {
            // UNC: \\server\share\… -> file://server/share/…
            if rest.is_empty() {
                return None;
            }
            return Some(format!("file://{}", percent_encode_path(rest)));
        }
        let b = normalised.as_bytes();
        let has_drive =
            b.len() >= 2 && b[1] == b':' && b[0].is_ascii_alphabetic();
        if has_drive {
            return Some(format!(
                "file:///{}",
                percent_encode_path(&normalised)
            ));
        }
        // Drive-less or relative — unrepresentable without inventing a
        // drive letter; dropping the entry beats wedging the OS.
        log::warn!(
            "path_to_file_url: dropping drive-less Windows path from drag payload: {:?}",
            path
        );
        None
    } else if normalised.starts_with('/') {
        Some(format!("file://{}", percent_encode_path(&normalised)))
    } else {
        Some(format!("file:///{}", percent_encode_path(&normalised)))
    }
}

/// Percent-encode the bytes of a path that aren't already URL-safe.
/// Preserves `/`, `:`, `-`, `_`, `.`, `~`, alphanumerics. Encodes the
/// rest. Path separators stay literal.
fn percent_encode_path(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for byte in s.bytes() {
        let safe = byte.is_ascii_alphanumeric()
            || matches!(byte, b'/' | b':' | b'-' | b'_' | b'.' | b'~');
        if safe {
            out.push(byte as char);
        } else {
            out.push_str(&format!("%{:02X}", byte));
        }
    }
    out
}

/// Parsed result from a ufb:// or union:// URI.
pub struct ParsedUri {
    pub source_os: String,
    pub path: String,
}

/// Parse a UFB or Union URI back to its source OS + path.
/// Input: "ufb:///win/C%3A/Users/Alice" → ParsedUri { source_os: "win", path: "C:/Users/Alice" }
/// Also handles legacy URIs without OS prefix for backwards compat.
pub fn parse_path_uri(uri: &str) -> Option<ParsedUri> {
    let stripped = uri
        .strip_prefix("ufb:///")
        .or_else(|| uri.strip_prefix("union:///"))?;

    // Check for OS prefix: win/, mac/
    let (source_os, encoded_path) = if stripped.starts_with("win/") {
        ("win".to_string(), &stripped[4..])
    } else if stripped.starts_with("mac/") {
        ("mac".to_string(), &stripped[4..])
    } else {
        // Legacy URI without OS prefix — assume current OS
        (current_os_tag().to_string(), stripped)
    };

    let decoded = urlencoding::decode(encoded_path).ok()?;
    Some(ParsedUri {
        source_os,
        path: decoded.to_string(),
    })
}

/// Expand a leading `~` or `~/` against the current machine's $HOME. Returns
/// the input unchanged if it doesn't start with `~`. Only meaningful when the
/// path is intended for the current OS — we never expand against some other
/// machine's home. Callers gate by comparing the mapping's OS to
/// `current_os_tag()`.
fn expand_home(path: &str) -> String {
    let rest = if let Some(r) = path.strip_prefix("~/") {
        r
    } else if path == "~" {
        ""
    } else {
        return path.to_string();
    };
    let Some(home) = dirs::home_dir() else {
        return path.to_string();
    };
    let home_str = home.to_string_lossy();
    if rest.is_empty() {
        home_str.into_owned()
    } else {
        format!("{}/{}", home_str.trim_end_matches('/'), rest)
    }
}

/// Expand `~` in a mapping prefix iff the mapping's OS is also the current
/// machine's OS — otherwise we'd be expanding against the wrong home dir.
fn expand_mapping_prefix(prefix: &str, os: &str) -> String {
    if os == current_os_tag() {
        expand_home(prefix)
    } else {
        prefix.to_string()
    }
}

/// Translate a path from source_os format to target_os format using mapping rules.
pub fn translate_path_to(
    source_os: &str,
    target_os: &str,
    path: &str,
    mappings: &[crate::settings::PathMapping],
) -> String {
    // NOTE: deliberately NO `source_os == target_os` short-circuit. A
    // same-OS call must still run the mapping loop so a foreign-form
    // string that drifted into a local-OS field — a drive-less
    // `\Volumes\…` or a forward-slash `/Volumes/…` row in a Windows DB —
    // gets repaired to the proper native form. For a same-OS call the
    // loop matches a mapping prefix against itself, normalising it;
    // paths that match nothing fall through to the `to_native_path` tail
    // unchanged, exactly as the old early-return did.

    // Try each mapping rule (skip disabled mappings)
    for mapping in mappings {
        if !mapping.enabled {
            continue;
        }
        let source_prefix_raw = match source_os {
            "win" => &mapping.win,
            "mac" => &mapping.mac,
            _ => continue,
        };
        let target_prefix_raw = match target_os {
            "win" => &mapping.win,
            "mac" => &mapping.mac,
            _ => continue,
        };

        if source_prefix_raw.is_empty() || target_prefix_raw.is_empty() {
            continue;
        }

        // Expand ~/ in prefixes when they refer to the current machine. This
        // lets a stored mapping of `~/ufb/mounts/X` work on any Mac regardless
        // of the user's login name.
        let source_prefix = expand_mapping_prefix(source_prefix_raw, source_os);
        let target_prefix = expand_mapping_prefix(target_prefix_raw, target_os);

        // Normalize for comparison (forward slashes, case-insensitive on Windows).
        // For win-source paths, also strip the leading drive letter (`C:`) and
        // any leading slash so legacy DB rows that lost their drive prefix
        // (`Volumes\X`, `\Volumes\X`) still match a mapping written as
        // `C:\Volumes\X`. Drive letters in path mappings are conventional —
        // the share suffix is what actually identifies the location.
        let norm_path = path.replace('\\', "/");
        let norm_source = source_prefix.replace('\\', "/");

        fn strip_for_win(s: &str) -> &str {
            let bytes = s.as_bytes();
            let s = if bytes.len() >= 2
                && bytes[1] == b':'
                && bytes[0].is_ascii_alphabetic()
            {
                &s[2..]
            } else {
                s
            };
            s.trim_start_matches('/')
        }

        // Keep a case-preserved version for slicing the remainder; the
        // lowercased copies are only for the win-source equality check.
        let (case_path, cmp_path, cmp_source) = if source_os == "win" {
            let cp = strip_for_win(&norm_path);
            let cs = strip_for_win(&norm_source);
            (cp.to_string(), cp.to_lowercase(), cs.to_lowercase())
        } else {
            (norm_path.clone(), norm_path.clone(), norm_source.clone())
        };

        if cmp_path.starts_with(&cmp_source) {
            // Slice from the case-preserved path using the source's
            // byte length — both went through identical
            // strip_for_win/replace passes so the lengths line up.
            let remainder = &case_path[cmp_source.len()..];
            let target_norm = target_prefix
                .trim_end_matches('/')
                .trim_end_matches('\\');
            let remainder_trimmed = remainder.trim_start_matches('/').trim_start_matches('\\');
            let translated = if remainder_trimmed.is_empty() {
                target_norm.to_string()
            } else {
                format!("{}/{}", target_norm, remainder_trimmed)
            };
            return to_native_path(&translated, target_os);
        }
    }

    // No mapping found — just convert to native path separators
    to_native_path(path, target_os)
}

/// Translate a path from one OS to the local OS using path mapping rules.
pub fn translate_path(
    source_os: &str,
    path: &str,
    mappings: &[crate::settings::PathMapping],
) -> String {
    translate_path_to(source_os, current_os_tag(), path, mappings)
}

/// Convert a native OS path to Windows-canonical format for DB storage.
/// On Windows this is a no-op.
pub fn to_canonical_path(native_path: &str, mappings: &[crate::settings::PathMapping]) -> String {
    translate_path_to(current_os_tag(), "win", native_path, mappings)
}

/// Auto-detect a path's source OS by format and translate to the
/// local OS canonical form. Used by mesh receive to normalise
/// incoming job_paths regardless of the sender's OS — without this,
/// a mac peer's `/Volumes/...` path would store verbatim on a
/// Windows DB and become invisible to the local Windows-form
/// queries (silent drift / "stranded columns").
///
/// Detection: paths containing `/` are treated as mac-form first;
/// paths starting with a drive letter (`C:\`) are win-form.
/// Falls through to the input unchanged when no mapping matches.
pub fn auto_canonicalize_path(
    path: &str,
    mappings: &[crate::settings::PathMapping],
) -> String {
    let local = current_os_tag();
    if path.contains('/') {
        let from_mac = translate_path_to("mac", local, path, mappings);
        if from_mac != path {
            return from_mac;
        }
    }
    if path.len() >= 2
        && path.as_bytes()[1] == b':'
        && path.as_bytes()[0].is_ascii_alphabetic()
    {
        let from_win = translate_path_to("win", local, path, mappings);
        if from_win != path {
            return from_win;
        }
    }
    // Drive-letter-less Windows form (`\Volumes\…`) — produced by some
    // earlier mesh-sync paths that stripped the leading `C:`. The
    // win-source mapping branch in `translate_path_to` already strips
    // a leading drive prefix for comparison, so this falls out
    // correctly when we route the input through the win→local
    // translator. Only attempt this if the path contains backslashes
    // and does NOT start with a forward slash (those are mac-form,
    // already handled above).
    if path.contains('\\') && !path.starts_with('/') {
        let from_win = translate_path_to("win", local, path, mappings);
        if from_win != path {
            return from_win;
        }
    }
    path.to_string()
}

/// Convert a stored DB path to the local-OS native form.
///
/// Post-migration the DB holds tagged identities (`vol:…` / `native:…`):
/// those are resolved through the volume registry derived from
/// `mappings`. A legacy raw path (pre-migration, or from an old peer)
/// falls back to mapping-based translation. A `vol:` identity for a
/// volume not mounted on this machine returns the stored string
/// unchanged — the UI shows it as unavailable rather than navigating
/// somewhere wrong.
pub fn from_canonical_path(db_path: &str, mappings: &[crate::settings::PathMapping]) -> String {
    use crate::volumes::LiveRootMode;
    if let Some(id) = crate::identity::Identity::from_storage(db_path) {
        let mapped = {
            let view = crate::volumes::view_from_path_mappings(mappings);
            crate::identity::resolve(&id, &view)
        };
        match crate::volumes::live_root_mode() {
            LiveRootMode::Off => {}
            LiveRootMode::Shadow => {
                let live_view =
                    crate::volumes::view_from_path_mappings_live(mappings);
                let live = crate::identity::resolve(&id, &live_view);
                if live != mapped {
                    log_live_root_mismatch("resolve", db_path, &mapped, &live);
                }
            }
            LiveRootMode::Live => {
                let live_view =
                    crate::volumes::view_from_path_mappings_live(mappings);
                if let Some(p) = crate::identity::resolve(&id, &live_view) {
                    return p;
                }
            }
        }
        return mapped.unwrap_or_else(|| db_path.to_string());
    }
    translate_path_to("win", current_os_tag(), db_path, mappings)
}

/// Shadow-mode disagreement log for the live-root overlay: each
/// distinct (op, input, old, new) combination logs once per process so
/// the dogfood log stays readable. See plans/17 slice A.
fn log_live_root_mismatch(
    op: &str,
    input: &str,
    old: &Option<String>,
    new: &Option<String>,
) {
    use std::collections::HashSet;
    use std::sync::Mutex;
    static SEEN: Mutex<Option<HashSet<String>>> = Mutex::new(None);
    let key = format!("{op}|{input}|{old:?}|{new:?}");
    if let Ok(mut guard) = SEEN.lock() {
        let seen = guard.get_or_insert_with(HashSet::new);
        if seen.insert(key) {
            log::info!(
                "[identity-shadow] {op} '{input}': mapped={old:?} live={new:?}"
            );
        }
    }
}

/// Convert a native filesystem path to its tagged-identity storage
/// string (`vol:{uuid}/{rel}` or `native:{os}/{path}`) — the write-side
/// counterpart of `from_canonical_path`. Idempotent: a string that is
/// already a tagged identity is returned unchanged.
pub fn to_identity_storage(
    native_path: &str,
    mappings: &[crate::settings::PathMapping],
) -> String {
    use crate::volumes::LiveRootMode;
    if crate::identity::Identity::from_storage(native_path).is_some() {
        return native_path.to_string();
    }
    let mapped = {
        let view = crate::volumes::view_from_path_mappings(mappings);
        crate::identity::classify(native_path, &view).to_storage()
    };
    match crate::volumes::live_root_mode() {
        LiveRootMode::Off => mapped,
        LiveRootMode::Shadow => {
            let live_view =
                crate::volumes::view_from_path_mappings_live(mappings);
            let live =
                crate::identity::classify(native_path, &live_view).to_storage();
            if live != mapped {
                log_live_root_mismatch(
                    "classify",
                    native_path,
                    &Some(mapped.clone()),
                    &Some(live),
                );
            }
            mapped
        }
        LiveRootMode::Live => {
            let live_view =
                crate::volumes::view_from_path_mappings_live(mappings);
            crate::identity::classify(native_path, &live_view).to_storage()
        }
    }
}

/// Convert forward-slash path to native OS path separators.
pub fn to_native_path(path: &str, os: &str) -> String {
    if os == "win" {
        path.replace('/', "\\")
    } else {
        path.replace('\\', "/")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::PathMapping;

    fn mapping(win: &str, mac: &str) -> PathMapping {
        PathMapping {
            win: win.to_string(),
            mac: mac.to_string(),
            enabled: true,
            label: String::new(),
        }
    }

    #[test]
    fn project_slug_strips_farm_prefix() {
        assert_eq!(
            project_slug_for(
                "/Volumes/share/Jobs/2025/MyShow/EP01",
                "/Volumes/share"
            ),
            "jobs_2025_myshow_ep01"
        );
    }

    #[test]
    fn project_slug_handles_trailing_slashes() {
        assert_eq!(
            project_slug_for(
                "/Volumes/share/Jobs/EP01/",
                "/Volumes/share/"
            ),
            "jobs_ep01"
        );
    }

    #[test]
    fn project_slug_falls_back_when_no_prefix_match() {
        // Job not under the farm root — fall back to last two
        // components. Important for local-only (non-shared) folders
        // that still need a stable slug for any save-as-preset path
        // that elevates them.
        let s = project_slug_for("/Users/alice/Desktop/random/folder", "/Volumes/share");
        // Last two components: "random/folder"
        assert_eq!(s, "random_folder");
    }

    #[test]
    fn project_slug_handles_root_match() {
        let s = project_slug_for("/Volumes/share", "/Volumes/share");
        assert_eq!(s, "_root");
    }

    #[test]
    fn project_slug_unicode_is_underscored() {
        let s = project_slug_for("/share/Jöbs/Ép01", "/share");
        // Non-ASCII gets underscored; collapses runs.
        assert!(s.starts_with("j_bs_"));
    }

    #[test]
    fn expand_home_basic() {
        let out = expand_home("~/ufb/mounts/X");
        assert!(!out.starts_with('~'), "~/... should be expanded: {}", out);
        assert!(out.ends_with("/ufb/mounts/X"), "suffix preserved: {}", out);
        assert_eq!(expand_home("/abs/path"), "/abs/path");
        assert_eq!(expand_home("relative"), "relative");
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn translate_tilde_mac_to_win() {
        let maps = [mapping("R:\\Projects", "~/ufb/mounts/Projects")];
        let home = dirs::home_dir().unwrap().to_string_lossy().into_owned();
        let input = format!("{}/ufb/mounts/Projects/Flame/reel.mov", home);
        let out = translate_path_to("mac", "win", &input, &maps);
        assert_eq!(out, "R:\\Projects\\Flame\\reel.mov");
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn translate_win_to_tilde_mac() {
        let maps = [mapping("R:\\Projects", "~/ufb/mounts/Projects")];
        let home = dirs::home_dir().unwrap().to_string_lossy().into_owned();
        let out = translate_path_to("win", "mac", "R:\\Projects\\Flame\\reel.mov", &maps);
        assert_eq!(out, format!("{}/ufb/mounts/Projects/Flame/reel.mov", home));
    }

    #[test]
    fn translate_no_mapping_preserves_path() {
        let maps: [PathMapping; 0] = [];
        assert_eq!(
            translate_path_to("mac", "win", "/Volumes/X/file", &maps),
            "\\Volumes\\X\\file"
        );
    }

    // Legacy DB rows can lack the drive letter
    // (`Volumes\studio-nas\jobs\` vs the mapping's
    // `C:\Volumes\studio-nas\jobs\`). The drive prefix is
    // conventional — the share suffix identifies the location — so the
    // matcher tolerates either form on the win side.
    #[cfg(target_os = "macos")]
    #[test]
    fn translate_drive_letterless_win_path_to_mac() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let out = translate_path_to(
            "win",
            "mac",
            "Volumes\\studio-nas\\jobs\\250099_x",
            &maps,
        );
        assert_eq!(out, "/Volumes/studio-nas/jobs/250099_x");
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn translate_leading_slash_no_drive_win_path_to_mac() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let out = translate_path_to(
            "win",
            "mac",
            "\\Volumes\\studio-nas\\jobs\\",
            &maps,
        );
        assert_eq!(out, "/Volumes/studio-nas/jobs");
    }

    // ── `translate_path_to` no longer early-returns when
    // source_os == target_os: a same-OS call must repair a foreign-form
    // string that drifted into a local-OS field. ──

    #[test]
    fn translate_win_to_win_repairs_driveless() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let out = translate_path_to(
            "win",
            "win",
            "\\Volumes\\studio-nas\\jobs\\250101_demo",
            &maps,
        );
        assert_eq!(out, "C:\\Volumes\\studio-nas\\jobs\\250101_demo");
    }

    #[test]
    fn translate_win_to_win_repairs_forward_slash() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let out = translate_path_to(
            "win",
            "win",
            "/Volumes/studio-nas/jobs/250101_demo",
            &maps,
        );
        assert_eq!(out, "C:\\Volumes\\studio-nas\\jobs\\250101_demo");
    }

    #[test]
    fn translate_win_to_win_idempotent_on_canonical() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let canon = "C:\\Volumes\\studio-nas\\jobs\\250101_demo";
        assert_eq!(translate_path_to("win", "win", canon, &maps), canon);
    }

    #[test]
    fn translate_win_to_win_no_mapping_preserves_path() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        // A genuine local path under no mapping must pass through untouched.
        let p = "C:\\Users\\alice\\Desktop\\notes.txt";
        assert_eq!(translate_path_to("win", "win", p, &maps), p);
    }

    #[test]
    fn translate_mac_to_mac_idempotent() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let p = "/Volumes/studio-nas/jobs/250101_demo";
        assert_eq!(translate_path_to("mac", "mac", p, &maps), p);
    }

    #[test]
    fn translate_mac_to_mac_no_mapping_preserves_path() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let p = "/Users/alice/Desktop/notes.txt";
        assert_eq!(translate_path_to("mac", "mac", p, &maps), p);
    }

    // ── path_to_file_url — drag-payload URL building. The drive-less
    // Windows case is the one that wedges OLE drag-drop until reboot. ──

    #[cfg(target_os = "windows")]
    #[test]
    fn file_url_drops_driveless_windows_path() {
        assert_eq!(path_to_file_url("\\Volumes\\studio-nas\\x"), None);
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn file_url_keeps_drive_letter_path() {
        assert_eq!(
            path_to_file_url("C:\\Users\\alice\\file.txt").as_deref(),
            Some("file:///C:/Users/alice/file.txt"),
        );
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn file_url_unc_path_uses_host_form() {
        let out = path_to_file_url("\\\\server\\share\\f").unwrap();
        assert_eq!(out, "file://server/share/f");
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn file_url_never_emits_four_slashes() {
        for p in ["\\Volumes\\x", "C:\\ok", "\\\\srv\\sh\\f", "rel\\x"] {
            if let Some(u) = path_to_file_url(p) {
                assert!(
                    !u.starts_with("file:////"),
                    "4-slash leaked for {:?}: {}",
                    p,
                    u
                );
            }
        }
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn file_url_mac_absolute_path() {
        assert_eq!(
            path_to_file_url("/Volumes/share/file.txt").as_deref(),
            Some("file:///Volumes/share/file.txt"),
        );
    }

    // ── Tier 2.2: from_canonical_path / to_identity_storage are
    // identity-aware — a tagged identity round-trips through the volume
    // registry; a legacy raw path still translates via mappings. ──

    #[test]
    fn to_identity_storage_classifies_native_path() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let s = to_identity_storage(
            "C:\\Volumes\\studio-nas\\jobs\\250101_demo",
            &maps,
        );
        assert!(s.starts_with("vol:"), "got {s}");
        assert!(s.ends_with("/250101_demo"), "got {s}");
    }

    #[test]
    fn to_identity_storage_is_idempotent() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let once = to_identity_storage(
            "C:\\Volumes\\studio-nas\\jobs\\250101_demo",
            &maps,
        );
        assert_eq!(once, to_identity_storage(&once, &maps));
    }

    #[test]
    fn from_canonical_path_resolves_tagged_identity() {
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let native = if cfg!(target_os = "windows") {
            "C:\\Volumes\\studio-nas\\jobs\\250101_demo"
        } else {
            "/Volumes/studio-nas/jobs/250101_demo"
        };
        let stored = to_identity_storage(native, &maps);
        assert_eq!(from_canonical_path(&stored, &maps), native);
    }

    #[test]
    fn from_canonical_path_still_handles_legacy_path() {
        // A non-tagged (legacy) string keeps mapping-based translation.
        let maps = [mapping(
            "C:\\Volumes\\studio-nas\\jobs\\",
            "/Volumes/studio-nas/jobs",
        )];
        let legacy = "C:\\Volumes\\studio-nas\\jobs\\250101_demo";
        let out = from_canonical_path(legacy, &maps);
        // On Windows this is a same-OS repair → unchanged native form.
        #[cfg(target_os = "windows")]
        assert_eq!(out, legacy);
        #[cfg(not(target_os = "windows"))]
        let _ = out;
    }
}
