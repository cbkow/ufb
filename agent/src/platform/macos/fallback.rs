use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Mutex, OnceLock};

/// Global mutex to serialize macOS mount operations.
/// Prevents concurrent snapshot-open-poll cycles from misidentifying each other's volumes.
static MOUNT_LOCK: OnceLock<Mutex<()>> = OnceLock::new();
fn mount_mutex() -> &'static Mutex<()> {
    MOUNT_LOCK.get_or_init(|| Mutex::new(()))
}

/// Extract the SMB share name from a UNC-style `nas_share_path`.
///
/// `\\server\share\sub\dir` → `share`. macOS's `mount_smbfs` mounts at
/// `/Volumes/<share>/`, so existing-mount detection and post-mount
/// volume polling have to match against the *share* component, not
/// the trailing path segment. The previous `rsplit('\\').next()` form
/// returned `dir` and silently missed real mounts whenever the user's
/// `nas_share_path` walked into a sub-directory of the share (e.g.
/// `\\server\Tank\Deep\DEEP_JOBS`) — agent then either tried
/// to re-mount and produced `/Volumes/Tank-1/` dedup suffixes,
/// or appeared stuck "Mounting" forever. Surfaced 2026-05-09.
///
/// Inputs without a `\\` prefix (already-extracted share names, mostly
/// from older mounts.json shapes) pass through unchanged for back-compat.
fn extract_share_name(nas_share_path: &str) -> String {
    let normalized = nas_share_path.replace('/', "\\");
    let stripped = match normalized.strip_prefix("\\\\") {
        Some(rest) => rest,
        None => return nas_share_path.trim_matches('\\').to_string(),
    };
    let mut parts = stripped.split('\\').filter(|p| !p.is_empty());
    let _server = parts.next();
    parts.next().unwrap_or("").to_string()
}

/// Mount an SMB share on macOS.
///
/// Strategy (OS-native credentials — plans/17 slice C):
/// 1. Reuse an existing mount if the share is already up under our
///    user-owned location or `/Volumes/`.
/// 2. Mount via `NetFSMountURLSync` with NULL credentials — NetFS
///    consults the login Keychain's internet-password entries for
///    this server, exactly like Finder's silent Cmd-K path. UFB never
///    touches the secret.
/// 3. `allow_ui=false` (auto attempts: boot, reconnect, heartbeat):
///    missing/stale Keychain entry returns EAUTH, which routes to the
///    sidebar's `auth_error` state and its "Fix credentials" pill.
/// 4. `allow_ui=true` (explicit user action from the pill / mount
///    editor): NetAuthAgent presents Apple's auth dialog with
///    "Remember this password in my keychain" — the user signs in
///    once and every future silent mount succeeds off the Keychain.
///    Cancelling the dialog returns ECANCELED, reported like an auth
///    failure (state stays actionable, no scary generic error).
///
/// `nas_share_path` is UNC format: `\\server\share`.
/// Typed mount failure so the orchestrator can route auth-class
/// errors to `Error(AuthFailed)` (the sidebar's "Fix credentials"
/// pill) and everything else to the generic mount error, mirroring
/// Windows' `SmbSessionError::{Auth, Other}` split.
#[derive(Debug)]
pub enum MacosMountError {
    /// EAUTH (no/stale Keychain sign-in) or ECANCELED (user dismissed
    /// the NetAuthAgent dialog) — actionable via the credentials pill.
    Auth(String),
    Other(String),
}

impl std::fmt::Display for MacosMountError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            MacosMountError::Auth(m) | MacosMountError::Other(m) => write!(f, "{}", m),
        }
    }
}

pub fn macos_smb_mount(
    nas_share_path: &str,
    allow_ui: bool,
) -> Result<String, MacosMountError> {
    // Serialize mount operations so concurrent mounts don't misidentify each other's volumes
    let _guard = mount_mutex().lock().unwrap();

    // Extract expected share name for matching against /Volumes/ entries.
    let expected_name = extract_share_name(nas_share_path);

    // Check if already mounted (user-owned location first, then /Volumes/ fallback
    // for shares a user may have mounted manually via Finder).
    if let Some(existing) = find_existing_user_mount(&expected_name) {
        log::info!("macOS: share already mounted at {}", existing);
        return Ok(existing);
    }
    if let Some(existing) = find_existing_volume(&expected_name, nas_share_path) {
        log::info!("macOS: share already mounted at {}", existing);
        return Ok(existing);
    }
    // Source-of-truth fallback for deep-path SMB URLs. extract_share_name
    // returns the top-level share component (e.g. "Tank" from
    // \\srv\Tank\Deep\DEEP_JOBS), but NetFSMountURLSync names
    // the volume by the leaf path segment (/Volumes/DEEP_JOBS). The two
    // checks above search by share name and miss any deep-path mount
    // inherited from a previous UFB session — without this fallback we
    // re-enter NetFS, get EEXIST, and fail to recover the mountpoint.
    if let Some(existing) = find_mount_by_smb_url(nas_share_path) {
        log::info!("macOS: share already mounted at {} (matched by SMB URL)", existing);
        return Ok(existing);
    }

    // ── Collision pre-flight (plans/17 slice C) ───────────────────────────
    // The reuse checks above didn't match, so if /Volumes/<leaf> (the
    // name NetFS will want) is occupied, the occupant is NOT our share.
    // Probe it: a DEAD mount squatting the name is force-unmounted —
    // it serves nobody, hangs Finder/Spotlight on touch, and would
    // push us to a dedup-suffixed name forever. A LIVE occupant is
    // left alone; NetFS dedups to <leaf>-1 and the orchestrator
    // reports the drift as a row notice.
    {
        let leaf = nas_share_path
            .trim_end_matches('\\')
            .rsplit('\\')
            .next()
            .unwrap_or("");
        if !leaf.is_empty() {
            let expected_path = format!("/Volumes/{}", leaf);
            if path_is_mount_point(&expected_path)
                && !probe_path_alive(&expected_path, std::time::Duration::from_secs(3))
            {
                log::warn!(
                    "macOS: dead mount squatting {} — force-unmounting before mount",
                    expected_path
                );
                if let Err(e) = macos_smb_unmount(&expected_path) {
                    log::warn!("macOS: squatter unmount failed ({}); proceeding — NetFS will dedup", e);
                }
            }
        }
    }

    // ── NetFS with Keychain credentials (NULL user/pass) ──────────────────
    match try_mount_netfs(&expected_name, nas_share_path, allow_ui) {
        Ok(path) => {
            log::info!("macOS: mounted at {} via NetFS (keychain)", path);
            Ok(path)
        }
        Err(status) if status == crate::platform::macos::netfs::EAUTH => {
            Err(MacosMountError::Auth(format!(
                "Authentication failed for {} — no valid saved sign-in for this server. \
                 Use Fix credentials to sign in once (check \"Remember this password in my keychain\")",
                nas_share_path
            )))
        }
        Err(status) if status == libc::ECANCELED => {
            Err(MacosMountError::Auth(format!(
                "Sign-in cancelled for {} — use Fix credentials to try again",
                nas_share_path
            )))
        }
        // NetFS returns EEXIST (17) when the share/URL is already mounted.
        // Two ways to land here: (1) a redundant Start while the share is
        // up — the pre-check at the top of this fn races with the
        // orchestrator's second IPC call, since a freshly-mounted SMB
        // volume's /Volumes/<share> may briefly read_dir empty before the
        // server completes enumeration; (2) a manual Finder mount that
        // landed at a path our scan didn't recognize. Either way, the
        // share IS mounted — re-scan and return the existing path so the
        // state machine stays in Mounted instead of flipping to Error.
        Err(status) if status == libc::EEXIST => {
            if let Some(existing) = find_existing_user_mount(&expected_name)
                .or_else(|| find_existing_volume(&expected_name, nas_share_path))
                .or_else(|| find_mount_by_smb_url(nas_share_path))
            {
                log::info!(
                    "macOS: NetFS reported already-mounted (EEXIST); using existing mount at {}",
                    existing
                );
                Ok(existing)
            } else {
                Err(MacosMountError::Other(format!(
                    "NetFS reported {} already mounted (EEXIST) but no matching volume found in scan",
                    nas_share_path
                )))
            }
        }
        Err(status) => Err(MacosMountError::Other(format!(
            "NetFS mount of {} failed: {}",
            nas_share_path,
            crate::platform::macos::netfs::status_message(status)
        ))),
    }
}

/// Attempt a NetFS mount with Keychain credentials (NULL user/pass).
/// Returns the resolved mount path on success or the raw NetFS errno
/// on failure. Caller maps EAUTH (80) / ECANCELED (89) to actionable
/// auth states and other errnos to a generic error.
fn try_mount_netfs(
    _share_name: &str,
    nas_share_path: &str,
    allow_ui: bool,
) -> Result<String, i32> {
    // NetFS wants a creds-free URL (smb://host/share); credentials come
    // from the Keychain (or the NetAuthAgent dialog when allow_ui).
    let smb_url = unc_to_smb_url(nas_share_path, "");

    // Pass `None` for mountpath: NetFS picks `/Volumes/<share>` (its
    // standard location). Mounting elsewhere triggers macOS Sequoia's
    // "wants to mount to this folder, which is unusual" approval
    // dialog every single time. The orchestrator's symlink layer
    // re-points `~/ufb/mounts/<share>` at whatever NetFS returns, so
    // letting Apple pick costs us nothing user-visible and dodges the
    // prompt entirely.
    crate::platform::macos::netfs::netfs_smb_mount(
        &smb_url, None, None, allow_ui,
    )
}

/// Check if a path is a mountpoint by comparing device IDs of path and parent.
fn is_mountpoint(path: &Path) -> bool {
    use std::os::unix::fs::MetadataExt;
    let path_meta = match std::fs::metadata(path) {
        Ok(m) => m,
        Err(_) => return false,
    };
    let parent = match path.parent() {
        Some(p) => p,
        None => return false,
    };
    let parent_meta = match std::fs::metadata(parent) {
        Ok(m) => m,
        Err(_) => return false,
    };
    path_meta.dev() != parent_meta.dev()
}

/// Check if a matching share is already mounted at our user-owned base.
/// True when `path` appears as a mount point in the mount table.
/// Table-based (no stat) so a dead mount can't hang us here.
fn path_is_mount_point(path: &str) -> bool {
    let Some(output) = Command::new("mount").output().ok() else {
        return false;
    };
    let table = String::from_utf8_lossy(&output.stdout);
    let needle = format!(" on {} (", path);
    table.lines().any(|l| l.contains(&needle))
}

/// Bounded liveness probe: read_dir on a worker thread with a
/// timeout. A dead SMB mount blocks the read_dir indefinitely — the
/// worker is abandoned (detached) and we report dead. read_dir, not
/// metadata: the kernel serves stale attrs from cache long after the
/// server is gone (same reasoning as the orchestrator heartbeat).
fn probe_path_alive(path: &str, timeout: std::time::Duration) -> bool {
    let (tx, rx) = std::sync::mpsc::channel();
    let p = path.to_string();
    std::thread::spawn(move || {
        let alive = std::fs::read_dir(&p).is_ok();
        let _ = tx.send(alive);
    });
    matches!(rx.recv_timeout(timeout), Ok(true))
}

fn find_existing_user_mount(share_name: &str) -> Option<String> {
    let smb_base = crate::config::MountConfig::smb_mount_base();
    let candidate = smb_base.join(share_name);
    if is_mountpoint(&candidate) {
        Some(candidate.to_string_lossy().to_string())
    } else {
        None
    }
}

/// Unmount an SMB share on macOS.
/// `volumes_path` is the actual /Volumes/... path (not the symlink).
pub fn macos_smb_unmount(volumes_path: &str) -> Result<(), String> {
    let path = Path::new(volumes_path);
    if !path.exists() {
        log::info!("macOS: mount point {} doesn't exist, nothing to unmount", volumes_path);
        return Ok(());
    }

    log::info!("macOS: unmounting {}", volumes_path);

    // Try diskutil first (clean unmount)
    let output = Command::new("diskutil")
        .args(["unmount", volumes_path])
        .output()
        .map_err(|e| format!("Failed to run diskutil unmount: {}", e))?;

    if output.status.success() {
        return Ok(());
    }

    // Fallback to umount
    let output = Command::new("umount")
        .arg(volumes_path)
        .output()
        .map_err(|e| format!("Failed to run umount: {}", e))?;

    if output.status.success() {
        Ok(())
    } else {
        let stderr = String::from_utf8_lossy(&output.stderr);
        Err(format!("Unmount failed: {}", stderr.trim()))
    }
}

/// Convert a UNC path to an smb:// URL.
/// `\\server\share` → `smb://user@server/share` (or `smb://server/share` if no user)
///
/// Usernames are percent-encoded per RFC 3986 userinfo rules so that names
/// containing spaces or other reserved characters produce a URL that
/// `mount_smbfs` accepts. Finder's `open smb://` tolerates unencoded
/// usernames; percent-encoded ones work in both.
fn unc_to_smb_url(unc_path: &str, username: &str) -> String {
    let stripped = unc_path.trim_start_matches('\\').replace('\\', "/");
    if username.is_empty() {
        format!("smb://{}", stripped)
    } else {
        format!("smb://{}@{}", percent_encode_userinfo(username), stripped)
    }
}

/// Percent-encode a string for use in the userinfo component of a URL.
/// Preserves unreserved characters and userinfo-safe sub-delims (RFC 3986 §3.2.1).
/// Encodes everything else — critically including ` `, `@`, `:`, `/`.
fn percent_encode_userinfo(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9'
            | b'-' | b'.' | b'_' | b'~'
            | b'!' | b'$' | b'&' | b'\'' | b'(' | b')'
            | b'*' | b'+' | b',' | b';' | b'=' => out.push(b as char),
            _ => out.push_str(&format!("%{:02X}", b)),
        }
    }
    out
}

/// Check if a volume matching the expected name is already mounted from the correct server.
/// Accepts macOS dedup suffixes (e.g. `MyShare-1` when another SMB mount already holds
/// `MyShare`) and verifies via `mount` output that the backing SMB source matches.
pub fn find_existing_volume(expected_name: &str, nas_share_path: &str) -> Option<String> {
    let candidates: Vec<String> = if let Ok(entries) = std::fs::read_dir("/Volumes") {
        entries
            .flatten()
            .filter_map(|entry| {
                let name = entry.file_name().to_str()?.to_string();
                if name.eq_ignore_ascii_case(expected_name)
                    || strip_macos_dedup_suffix(&name)
                        .map(|base| base.eq_ignore_ascii_case(expected_name))
                        .unwrap_or(false)
                {
                    let path = format!("/Volumes/{}", name);
                    // Verify it's actually a mount point (not just an empty dir)
                    if std::fs::read_dir(&path).map(|mut d| d.next().is_some()).unwrap_or(false) {
                        return Some(path);
                    }
                }
                None
            })
            .collect()
    } else {
        return None;
    };

    if candidates.is_empty() {
        return None;
    }

    // Verify the candidate is actually mounted from the expected server/share
    let smb_fragment = nas_share_path
        .trim_start_matches('\\')
        .replace('\\', "/")
        .to_lowercase();

    let mount_output = Command::new("mount").output().ok()?;
    let mount_text = String::from_utf8_lossy(&mount_output.stdout);

    for candidate in &candidates {
        for line in mount_text.lines() {
            if line.contains(candidate) && line.to_lowercase().contains(&smb_fragment) {
                return Some(candidate.clone());
            }
        }
    }

    None
}

/// Look up an SMB mount's mountpoint by matching `mount(8)` output
/// against the SMB URL derived from a UNC path. This is the source of
/// truth for SMB mounts and is independent of volume-name heuristics
/// (which break for deep-path mounts where NetFS names the volume by
/// the leaf path segment, not the SMB share component).
///
/// `mount(8)` SMB lines look like:
///   //user%20name@host/share/sub/leaf on /Volumes/leaf (smbfs, ...)
/// We build `host/share/sub/leaf` from the UNC, match it case-insensitively
/// against the source side, and return the mountpoint following ` on `.
fn find_mount_by_smb_url(nas_share_path: &str) -> Option<String> {
    let fragment = nas_share_path
        .trim_start_matches('\\')
        .replace('\\', "/")
        .to_lowercase();
    if fragment.is_empty() {
        return None;
    }
    let output = Command::new("mount").output().ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    for line in text.lines() {
        let lower = line.to_lowercase();
        if !lower.contains("smbfs") || !lower.contains(&fragment) {
            continue;
        }
        if let Some(rest) = line.split(" on ").nth(1) {
            if let Some(mp) = rest.split(" (").next() {
                let mp = mp.trim();
                if !mp.is_empty() {
                    return Some(mp.to_string());
                }
            }
        }
    }
    None
}

/// Ownership check for a mountpoint (plans/17 slice C): what the mount
/// table says occupies `path`.
///   None        — nothing mounted there (plain dir or absent)
///   Some(true)  — an smbfs mount whose URL matches `nas_share_path`
///   Some(false) — someone else's volume (foreign SMB, disk image,
///                 USB, …). Never unmount these: a mount Restart must
///                 not eject the user's identically-named disk.
pub fn mount_at_path_is_ours(path: &str, nas_share_path: &str) -> Option<bool> {
    let fragment = nas_share_path
        .trim_start_matches('\\')
        .replace('\\', "/")
        .to_lowercase();
    let output = Command::new("mount").output().ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    let needle = format!(" on {} (", path);
    for line in text.lines() {
        if !line.contains(&needle) {
            continue;
        }
        let lower = line.to_lowercase();
        return Some(lower.contains("smbfs") && lower.contains(&fragment));
    }
    None
}

/// Strip a macOS dedup suffix like "-1", "-2" from a volume name.
/// Returns the base name if a suffix was stripped, or None if no suffix present.
/// Correctly handles names that already contain hyphens (e.g. "my-share-1" → "my-share").
fn strip_macos_dedup_suffix(name: &str) -> Option<&str> {
    if let Some(pos) = name.rfind('-') {
        let after = &name[pos + 1..];
        if !after.is_empty() && after.chars().all(|c| c.is_ascii_digit()) {
            return Some(&name[..pos]);
        }
    }
    None
}

/// List current entries in /Volumes/.
fn list_volumes() -> HashSet<String> {
    let mut volumes = HashSet::new();
    if let Ok(entries) = std::fs::read_dir("/Volumes") {
        for entry in entries.flatten() {
            if let Some(name) = entry.file_name().to_str() {
                volumes.insert(name.to_string());
            }
        }
    }
    volumes
}

/// Poll /Volumes/ for a new entry that appeared after the mount command.
/// Retries for up to 60 seconds (user may need to enter credentials in a dialog).
fn poll_for_new_volume(before: &HashSet<String>, nas_share_path: &str) -> Result<String, String> {
    // Match /Volumes/<share>/, not /Volumes/<trailing-subdir>/.
    let expected_name = extract_share_name(nas_share_path);

    log::info!("macOS: polling /Volumes/ for '{}' (timeout 60s)", expected_name);

    for attempt in 0..120 {
        std::thread::sleep(std::time::Duration::from_millis(500));

        let after = list_volumes();
        let new_entries: Vec<&String> = after.difference(before).collect();

        if !new_entries.is_empty() {
            // Prefer exact match on share name
            for entry in &new_entries {
                if entry.eq_ignore_ascii_case(&expected_name) {
                    return Ok(format!("/Volumes/{}", entry));
                }
            }
            // Accept name with macOS dedup suffix (-1, -2, etc.)
            for entry in &new_entries {
                if let Some(base) = strip_macos_dedup_suffix(entry) {
                    if base.eq_ignore_ascii_case(&expected_name) {
                        return Ok(format!("/Volumes/{}", entry));
                    }
                }
            }
            // No match — keep polling. Do NOT grab an arbitrary volume.
        }

        if attempt % 10 == 9 {
            log::info!("macOS: still waiting for mount in /Volumes/ ({}s)...", (attempt + 1) / 2);
        }
    }

    Err(format!(
        "Timed out after 60s waiting for SMB mount to appear in /Volumes/ (expected '{}')",
        expected_name
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_unc_to_smb_url_with_user() {
        assert_eq!(
            unc_to_smb_url(r"\\nas\media", "alice"),
            "smb://alice@nas/media"
        );
    }

    #[test]
    fn test_unc_to_smb_url_no_user() {
        assert_eq!(
            unc_to_smb_url(r"\\nas\media", ""),
            "smb://nas/media"
        );
    }

    #[test]
    fn test_unc_to_smb_url_deep_path() {
        assert_eq!(
            unc_to_smb_url(r"\\server.local\share\subfolder", "admin"),
            "smb://admin@server.local/share/subfolder"
        );
    }

    #[test]
    fn test_unc_to_smb_url_username_with_space() {
        // Regression: mount_smbfs rejects unencoded spaces in usernames.
        assert_eq!(
            unc_to_smb_url(r"\\nas\share", "first last"),
            "smb://first%20last@nas/share"
        );
    }

    #[test]
    fn test_percent_encode_userinfo_common_chars() {
        assert_eq!(percent_encode_userinfo("alice"), "alice");
        assert_eq!(percent_encode_userinfo("first last"), "first%20last");
        assert_eq!(percent_encode_userinfo("user:pass"), "user%3Apass");
        assert_eq!(percent_encode_userinfo("a@b"), "a%40b");
        assert_eq!(percent_encode_userinfo("a.b-c_d"), "a.b-c_d");
    }

    #[test]
    fn test_strip_macos_dedup_suffix_simple() {
        assert_eq!(strip_macos_dedup_suffix("media-1"), Some("media"));
        assert_eq!(strip_macos_dedup_suffix("media-2"), Some("media"));
        assert_eq!(strip_macos_dedup_suffix("media-12"), Some("media"));
    }

    #[test]
    fn test_strip_macos_dedup_suffix_hyphenated_name() {
        assert_eq!(strip_macos_dedup_suffix("my-share-1"), Some("my-share"));
        assert_eq!(strip_macos_dedup_suffix("my-share-2"), Some("my-share"));
    }

    #[test]
    fn test_strip_macos_dedup_suffix_no_suffix() {
        assert_eq!(strip_macos_dedup_suffix("media"), None);
        assert_eq!(strip_macos_dedup_suffix("my-share"), None);
        assert_eq!(strip_macos_dedup_suffix("archive-2023data"), None);
    }

    #[test]
    fn test_strip_macos_dedup_suffix_edge_cases() {
        // Name ending in hyphen-digits that is part of the real name
        // The function strips it, but callers only use the result when it matches expected_name
        assert_eq!(strip_macos_dedup_suffix("archive-2023"), Some("archive"));
        // Trailing hyphen with no digits
        assert_eq!(strip_macos_dedup_suffix("media-"), None);
    }
}
