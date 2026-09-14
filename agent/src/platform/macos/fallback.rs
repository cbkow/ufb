use std::path::Path;
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

/// Global mutex to serialize macOS mount operations.
/// Prevents concurrent snapshot-open-poll cycles from misidentifying each other's volumes.
///
/// Audit 2026-09-11 P2: held only across the mount-table scan, the
/// squatter pre-flight and the NetFS *issue* — never across the wait.
/// Pre-fix a single share parked on the 600s Sign-in dialog blocked
/// every other mount in the fleet behind this lock, and a panic under
/// it poisoned it for the rest of the process lifetime (every later
/// mount unwrapped the poison and panicked in turn).
static MOUNT_LOCK: OnceLock<Mutex<()>> = OnceLock::new();
fn mount_mutex() -> std::sync::MutexGuard<'static, ()> {
    MOUNT_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|e| e.into_inner())
}

/// Liveness budget for "is this mount-table hit actually serving?"
/// probes. Matches the orchestrator heartbeat's read_dir budget.
const LIVENESS_PROBE_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(3);

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

/// Result of a successful `macos_smb_mount`.
#[derive(Debug, Clone, PartialEq)]
pub struct MacosMount {
    /// Where the share is mounted (`/Volumes/<share>` or a dedup variant).
    pub path: String,
    /// True when the share was already mounted (by Finder, a previous
    /// agent session, or another UFB process) and we adopted it rather
    /// than mounting it ourselves. Audit 2026-09-11 P2: an adopted
    /// mount is not ours to eject on Stop/Quit — the user (or the GUI)
    /// put it there.
    pub adopted: bool,
}

/// Mount an SMB share on macOS.
///
/// Strategy (OS-native credentials — plans/17 slice C):
/// 1. Reuse an existing mount if the share is already up under our
///    user-owned location or `/Volumes/` — after a bounded liveness
///    probe (audit 2026-09-11 L-3b): a mount-table hit whose SMB
///    session is dead is force-unmounted (when it's ours) and we fall
///    through to a fresh mount instead of handing the orchestrator a
///    zombie that hangs every read_dir.
/// 2. Mount via NetFS (async, deadline-bounded) with NULL credentials — NetFS
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
///
/// `abandoned`: set by the orchestrator when the attempt was superseded
/// (Stop/Restart/ConfigChanged) while this call was still queued on
/// MOUNT_LOCK or busy in the pre-flight probes — i.e. BEFORE anything
/// was handed to NetFS, when there is no request to cancel. Checked
/// right before the issue so an abandoned attempt never creates a
/// volume nobody owns (review 2026-09-11 #1).
///
/// Blocks for the whole mount (up to the NetFS deadline) — callers run
/// it under `spawn_blocking` (audit 2026-09-11 M-3).
pub fn macos_smb_mount(
    nas_share_path: &str,
    allow_ui: bool,
    abandoned: &AtomicBool,
) -> Result<MacosMount, MacosMountError> {
    // Serialize the scan + issue so concurrent mounts don't misidentify
    // each other's volumes. Released before the wait (see MOUNT_LOCK).
    let guard = mount_mutex();
    if abandoned.load(Ordering::SeqCst) {
        return Err(MacosMountError::Other(abandoned_msg(nas_share_path)));
    }

    // Extract expected share name for matching against /Volumes/ entries.
    let expected_name = extract_share_name(nas_share_path);

    // Check if already mounted (user-owned location first, then /Volumes/
    // for shares a user may have mounted manually via Finder, then the
    // SMB-URL source-of-truth scan for deep-path mounts NetFS named by
    // the leaf segment). Any hit is liveness-probed before adoption.
    if let Some(existing) = find_live_existing_mount(&expected_name, nas_share_path) {
        log::info!("macOS: share already mounted at {} — adopting", existing);
        return Ok(MacosMount { path: existing, adopted: true });
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
                && !probe_path_alive(&expected_path, LIVENESS_PROBE_TIMEOUT)
            {
                log::warn!(
                    "macOS: dead mount squatting {} — force-unmounting before mount",
                    expected_path
                );
                if let Err(e) = macos_smb_unmount_force(&expected_path) {
                    log::warn!("macOS: squatter unmount failed ({}); proceeding — NetFS will dedup", e);
                }
            } else if stale_dir_blocking(&expected_path) {
                // Orphaned NetFS placeholder dir. A plain rmdir only
                // works where /Volumes permissions allow it (they
                // usually don't — root-owned parent); on failure NetFS
                // dedups to <leaf>-1 and the drift notice marks the
                // squatter as fixable so the GUI can offer the
                // admin-privileged removal.
                match std::fs::remove_dir(&expected_path) {
                    Ok(()) => log::info!(
                        "macOS: removed stale mountpoint dir {}",
                        expected_path
                    ),
                    Err(e) => log::warn!(
                        "macOS: stale dir blocking {} ({}); NetFS will dedup",
                        expected_path,
                        e
                    ),
                }
            }
        }
    }

    // ── NetFS with Keychain credentials (NULL user/pass) ──────────────────
    // Last chance to bail before NetFS owns a request: the scans and
    // probes above can take seconds, during which the orchestrator may
    // have moved on.
    if abandoned.load(Ordering::SeqCst) {
        return Err(MacosMountError::Other(abandoned_msg(nas_share_path)));
    }
    // Issue under the lock, wait outside it: the wait can legitimately
    // last minutes (Sign-in dialog) and must not stall other shares.
    let issued = issue_mount_netfs(nas_share_path, allow_ui);
    drop(guard);
    let outcome = issued.and_then(|req| req.wait());

    match outcome {
        Ok(path) => {
            log::info!("macOS: mounted at {} via NetFS (keychain)", path);
            Ok(MacosMount { path, adopted: false })
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
            if let Some(existing) = find_live_existing_mount(&expected_name, nas_share_path) {
                log::info!(
                    "macOS: NetFS reported already-mounted (EEXIST); using existing mount at {}",
                    existing
                );
                Ok(MacosMount { path: existing, adopted: true })
            } else {
                Err(MacosMountError::Other(format!(
                    "NetFS reported {} already mounted (EEXIST) but no matching live volume found in scan",
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

/// Heartbeat-reconnect twin of `macos_smb_mount` (audit 2026-09-11
/// L-3b): when the orchestrator already holds a backing path, probe it
/// with a bounded read_dir (NOT `metadata` — the kernel answers that
/// from its attribute cache long after the SMB session died), force-
/// unmount it when it's dead and ours, then run the normal mount path
/// which either adopts the live mount or mounts afresh. Blocking; run
/// under `spawn_blocking`.
pub fn macos_reconnect_blocking(
    mounted_at: Option<&str>,
    nas_share_path: &str,
    abandoned: &AtomicBool,
) -> Result<MacosMount, MacosMountError> {
    if let Some(stale_path) = mounted_at {
        if !probe_path_alive(stale_path, LIVENESS_PROBE_TIMEOUT) {
            // Ownership guard (plans/17 slice C): unmount only OUR dead
            // share. A foreign volume at this path (squatter that
            // displaced us / renamed disk) must not be ejected — the
            // remount lands wherever NetFS picks.
            match mount_at_path_is_ours(stale_path, nas_share_path) {
                Some(true) => {
                    log::warn!(
                        "macOS: stale SMB mount at {} — force-unmounting before reconnect",
                        stale_path
                    );
                    if let Err(e) = macos_smb_unmount_force(stale_path) {
                        log::warn!("macOS: force-unmount of {} failed: {}", stale_path, e);
                    }
                }
                Some(false) => log::warn!(
                    "macOS: {} is occupied by a foreign volume — leaving it, remounting elsewhere",
                    stale_path
                ),
                None => {}
            }
        }
    }
    // Background reconnect: never allowed to pop the auth dialog — a
    // stale Keychain entry surfaces via the pill on the next explicit
    // Start instead.
    macos_smb_mount(nas_share_path, false, abandoned)
}

fn abandoned_msg(nas_share_path: &str) -> String {
    format!("mount of {} abandoned before it was issued (superseded)", nas_share_path)
}

/// Cancel an in-flight NetFS request for this share, if any (audit
/// 2026-09-11 M-3): the orchestrator's Stop/Restart-while-Mounting
/// path. Returns the number of requests cancelled.
pub fn cancel_inflight_mount(nas_share_path: &str) -> usize {
    let smb_url = unc_to_smb_url(nas_share_path, "");
    crate::platform::macos::netfs::cancel_inflight_mount(&smb_url)
}

/// Issue a NetFS mount with Keychain credentials (NULL user/pass).
/// Returns the pending request (the caller waits on it outside
/// MOUNT_LOCK) or the raw NetFS errno on failure. Caller maps EAUTH
/// (80) / ECANCELED (89) to actionable auth states and other errnos to
/// a generic error.
fn issue_mount_netfs(
    nas_share_path: &str,
    allow_ui: bool,
) -> Result<crate::platform::macos::netfs::NetfsRequest, i32> {
    // NetFS wants a creds-free URL (smb://host/share); credentials come
    // from the Keychain (or the NetAuthAgent dialog when allow_ui).
    let smb_url = unc_to_smb_url(nas_share_path, "");

    // Pass `None` for mountpath: NetFS picks `/Volumes/<share>` (its
    // standard location). Mounting elsewhere triggers macOS Sequoia's
    // "wants to mount to this folder, which is unusual" approval
    // dialog every single time. The NFS loopback server is bound to
    // whatever NetFS returns, so letting Apple pick costs us nothing
    // user-visible and dodges the prompt entirely.
    crate::platform::macos::netfs::NetfsRequest::issue(&smb_url, None, None, allow_ui)
}

/// The three reuse scans, each followed by a liveness probe (audit
/// 2026-09-11 L-3b). A dead hit that the mount table attributes to our
/// share is force-unmounted so the caller falls through to a fresh
/// mount; a dead hit that isn't ours (the name-only legacy location)
/// is just skipped.
fn find_live_existing_mount(expected_name: &str, nas_share_path: &str) -> Option<String> {
    let mut table = read_mount_table()?;
    // The three scans usually return the SAME path; dedupe so a dead
    // hit isn't probed (3s each, under MOUNT_LOCK) three times over
    // (review 2026-09-11 #4).
    let mut candidates: Vec<String> = Vec::new();
    for c in [
        find_existing_user_mount(expected_name),
        find_volume_in(&table, expected_name, nas_share_path),
        find_smb_mount_point_in(&table, nas_share_path),
    ]
    .into_iter()
    .flatten()
    {
        if !candidates.contains(&c) {
            candidates.push(c);
        }
    }
    for hit in candidates {
        // Re-checked against a table re-read after any force-unmount
        // below: an emptied ex-mountpoint directory answers read_dir
        // happily and must not be adopted as a live mount.
        if !is_mount_point_in(&table, &hit) {
            log::info!("macOS: {} is no longer a mount point — skipping", hit);
            continue;
        }
        if probe_path_alive(&hit, LIVENESS_PROBE_TIMEOUT) {
            return Some(hit);
        }
        match owner_at_in(&table, &hit, nas_share_path) {
            Some(true) => {
                log::warn!(
                    "macOS: existing mount at {} is dead — force-unmounting before remount",
                    hit
                );
                if let Err(e) = macos_smb_unmount_force(&hit) {
                    log::warn!("macOS: force-unmount of dead {} failed: {}", hit, e);
                }
                if let Some(fresh) = read_mount_table() {
                    table = fresh;
                }
            }
            _ => log::warn!(
                "macOS: existing mount at {} is dead and not ours — ignoring it",
                hit
            ),
        }
    }
    None
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

/// `mount(8)` output, or None if the command couldn't run.
fn read_mount_table() -> Option<String> {
    let output = Command::new("mount").output().ok()?;
    Some(String::from_utf8_lossy(&output.stdout).into_owned())
}

/// True when `path` appears as a mount point in the mount table.
/// Table-based (no stat) so a dead mount can't hang us here.
fn path_is_mount_point(path: &str) -> bool {
    read_mount_table()
        .map(|t| is_mount_point_in(&t, path))
        .unwrap_or(false)
}

/// True when `path` is an orphaned mountpoint placeholder: a directory
/// that exists on disk, is NOT in the mount table, and is empty. NetFS
/// creates `/Volumes/<share>` before mounting; an attempt that dies
/// uncleanly (crash, power loss, the pre-1.0.11 NetAuthSysAgent hang)
/// leaves the dir behind forever — `/Volumes` is root-owned, so no
/// user-space cleanup ever runs and every later mount dedups to
/// `<share>-1`. Distinguishes that removable case from a live foreign
/// occupant, which must never be touched. read_dir here is safe: the
/// path is confirmed NOT a mount, so it can't hang on a dead server.
pub fn stale_dir_blocking(path: &str) -> bool {
    if path_is_mount_point(path) {
        return false;
    }
    let Ok(md) = std::fs::symlink_metadata(path) else {
        return false;
    };
    if !md.is_dir() {
        return false;
    }
    match std::fs::read_dir(path) {
        Ok(mut d) => d.next().is_none(),
        // NetFS creates its placeholder dirs mode 0111 (no read) — we
        // can't prove emptiness, but "exists, unmounted, unreadable"
        // has no other legitimate shape, and rmdir itself refuses
        // non-empty dirs, so misclassification can't delete data.
        Err(e) if e.kind() == std::io::ErrorKind::PermissionDenied => true,
        Err(_) => false,
    }
}

/// Bounded liveness probe: read_dir + first entry on a worker thread
/// with a timeout. A dead SMB mount blocks the read_dir indefinitely —
/// the worker is abandoned (detached) and we report dead. read_dir,
/// not metadata: the kernel serves stale attrs from cache long after
/// the server is gone (same reasoning as the orchestrator heartbeat).
pub fn probe_path_alive(path: &str, timeout: std::time::Duration) -> bool {
    let (tx, rx) = std::sync::mpsc::channel();
    let p = path.to_string();
    std::thread::spawn(move || {
        let alive = match std::fs::read_dir(&p) {
            Ok(mut rd) => !matches!(rd.next(), Some(Err(_))),
            Err(_) => false,
        };
        let _ = tx.send(alive);
    });
    matches!(rx.recv_timeout(timeout), Ok(true))
}

/// Legacy user-owned mount location (`~/.local/share/ufb/smb-mounts/<share>`)
/// from the mount_smbfs era. Name-only — the liveness/ownership checks
/// in `find_live_existing_mount` gate adoption.
fn find_existing_user_mount(share_name: &str) -> Option<String> {
    let smb_base = crate::config::MountConfig::smb_mount_base();
    let candidate = smb_base.join(share_name);
    if is_mountpoint(&candidate) {
        Some(candidate.to_string_lossy().to_string())
    } else {
        None
    }
}

/// Unmount an SMB share on macOS (clean unmount, then plain umount).
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

/// Force-unmount a DEAD SMB mount. `umount -f` does not touch the
/// server so it can't hang on one that's gone; `diskutil unmount
/// force` is the fallback. Never use on a live mount you don't own.
/// Note: `path.exists()` is deliberately NOT checked first — stat on a
/// dead mount is exactly the thing that hangs.
pub fn macos_smb_unmount_force(volumes_path: &str) -> Result<(), String> {
    log::info!("macOS: force-unmounting {}", volumes_path);
    let output = Command::new("umount")
        .arg("-f")
        .arg(volumes_path)
        .output()
        .map_err(|e| format!("Failed to run umount -f: {}", e))?;
    if output.status.success() {
        return Ok(());
    }
    let output = Command::new("diskutil")
        .args(["unmount", "force", volumes_path])
        .output()
        .map_err(|e| format!("Failed to run diskutil unmount force: {}", e))?;
    if output.status.success() {
        Ok(())
    } else {
        let stderr = String::from_utf8_lossy(&output.stderr);
        Err(format!("Force unmount failed: {}", stderr.trim()))
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

/// Decode `%XX` escapes (mount(8) prints smbfs sources with RFC 3986
/// encoding — `first%20last@host/My%20Share`). Malformed escapes pass
/// through untouched; invalid UTF-8 is replaced lossily.
fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            // Slice bytes, not the &str: a non-ASCII byte after '%'
            // would make a str slice panic on the char boundary.
            let hex = std::str::from_utf8(&bytes[i + 1..i + 3]).ok();
            if let Some(v) = hex.and_then(|h| u8::from_str_radix(h, 16).ok()) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

// ── Mount-table parsing (pure; audit 2026-09-11 M-1/M-2) ───────────────
//
// The GUI's twin lives in core/src/macos_mounts.rs and must mirror these
// rules exactly:
//   * a line is `<source> on <mount point> (<fstype>, <opts…>)`; split the
//     trailing ` (` FIRST (rsplit) so a mount point containing spaces
//     survives, then the first ` on ` (smbfs sources never contain a
//     space — they're percent-encoded);
//   * an smbfs source is `//[user@]host[:port]/share[/sub…]`; strip the
//     `//`, the userinfo up to the last `@` of the authority, the `:port`,
//     percent-decode, and compare the WHOLE `host/share[/sub]` string
//     case-insensitively against the UNC's `host\share[\sub]` — never a
//     substring test (`nas/tank` used to match `//nas/tank_archive`);
//   * a mount point matches by exact string equality, never `contains`
//     (`/Volumes/Projects` used to match `/Volumes/Projects-1`).

/// One parsed line of `mount(8)` output.
#[derive(Debug, PartialEq)]
struct MountEntry<'a> {
    source: &'a str,
    mount_point: &'a str,
    fstype: &'a str,
}

fn parse_mount_line(line: &str) -> Option<MountEntry<'_>> {
    let (head, opts) = line.rsplit_once(" (")?;
    let (source, mount_point) = head.split_once(" on ")?;
    let fstype = opts
        .trim_end_matches(')')
        .split(',')
        .next()
        .unwrap_or("")
        .trim();
    let mount_point = mount_point.trim();
    if source.is_empty() || mount_point.is_empty() {
        return None;
    }
    Some(MountEntry { source, mount_point, fstype })
}

/// `\\host\share\sub` → `host/share/sub` (lowercased, no trailing slash).
/// Forward slashes are tolerated for older config shapes.
fn unc_to_host_path(nas_share_path: &str) -> String {
    nas_share_path
        .replace('\\', "/")
        .trim_matches('/')
        .to_lowercase()
}

/// Canonical `host/share[/sub]` of an smbfs mount source, or None when
/// the source isn't an SMB URL shape (`localhost:/x`, `/dev/disk…`).
fn smb_source_host_path(source: &str) -> Option<String> {
    let rest = source.strip_prefix("//")?;
    let (authority, path) = match rest.split_once('/') {
        Some((a, p)) => (a, p),
        None => (rest, ""),
    };
    let host = authority.rsplit('@').next().unwrap_or(authority);
    let host = host.split(':').next().unwrap_or(host);
    if host.is_empty() {
        return None;
    }
    let path = percent_decode(path);
    let path = path.trim_matches('/');
    let mut out = percent_decode(host).to_lowercase();
    if !path.is_empty() {
        out.push('/');
        out.push_str(&path.to_lowercase());
    }
    Some(out)
}

/// Exact (case-insensitive) match of an smbfs source against a UNC path.
fn smb_source_matches(source: &str, nas_share_path: &str) -> bool {
    let want = unc_to_host_path(nas_share_path);
    if want.is_empty() {
        return false;
    }
    smb_source_host_path(source).map(|got| got == want).unwrap_or(false)
}

fn is_mount_point_in(table: &str, path: &str) -> bool {
    table
        .lines()
        .filter_map(parse_mount_line)
        .any(|e| e.mount_point == path)
}

/// Mount point of the smbfs entry whose source is exactly our share
/// (deep paths included), regardless of where NetFS parked it.
fn find_smb_mount_point_in(table: &str, nas_share_path: &str) -> Option<String> {
    table
        .lines()
        .filter_map(parse_mount_line)
        .find(|e| e.fstype == "smbfs" && smb_source_matches(e.source, nas_share_path))
        .map(|e| e.mount_point.to_string())
}

/// `/Volumes/<expected>` (or a `<expected>-N` dedup) whose smbfs source
/// is exactly our share. Name-gated so the caller's log line reflects
/// the "share by name" intent; the source check is what makes it safe
/// (a live foreign `/Volumes/Projects` next to our `/Volumes/Projects-1`
/// used to be adopted by the old `line.contains(candidate)` test).
fn find_volume_in(table: &str, expected_name: &str, nas_share_path: &str) -> Option<String> {
    table
        .lines()
        .filter_map(parse_mount_line)
        .filter(|e| e.fstype == "smbfs")
        .filter(|e| {
            let Some(name) = e.mount_point.strip_prefix("/Volumes/") else {
                return false;
            };
            name.eq_ignore_ascii_case(expected_name)
                || strip_macos_dedup_suffix(name)
                    .map(|base| base.eq_ignore_ascii_case(expected_name))
                    .unwrap_or(false)
        })
        .find(|e| smb_source_matches(e.source, nas_share_path))
        .map(|e| e.mount_point.to_string())
}

/// Ownership of the entry mounted exactly at `path`:
///   None        — nothing mounted there
///   Some(true)  — an smbfs mount whose source is exactly our share
///   Some(false) — someone else's volume
fn owner_at_in(table: &str, path: &str, nas_share_path: &str) -> Option<bool> {
    table
        .lines()
        .filter_map(parse_mount_line)
        .find(|e| e.mount_point == path)
        .map(|e| e.fstype == "smbfs" && smb_source_matches(e.source, nas_share_path))
}

/// Check if a volume matching the expected name is already mounted from
/// the correct server. Accepts macOS dedup suffixes (e.g. `MyShare-1`
/// when another SMB mount already holds `MyShare`) and verifies via the
/// mount table that the backing SMB source is exactly ours. Table-only:
/// no read_dir on the candidate (that hangs on a dead mount).
#[allow(dead_code)]
pub fn find_existing_volume(expected_name: &str, nas_share_path: &str) -> Option<String> {
    let table = read_mount_table()?;
    find_volume_in(&table, expected_name, nas_share_path)
}

/// Ownership check for a mountpoint (plans/17 slice C): what the mount
/// table says occupies `path`.
///   None        — nothing mounted there (plain dir or absent)
///   Some(true)  — an smbfs mount whose URL matches `nas_share_path`
///   Some(false) — someone else's volume (foreign SMB, disk image,
///                 USB, …). Never unmount these: a mount Restart must
///                 not eject the user's identically-named disk.
pub fn mount_at_path_is_ours(path: &str, nas_share_path: &str) -> Option<bool> {
    let table = read_mount_table()?;
    owner_at_in(&table, path, nas_share_path)
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
    fn test_percent_decode() {
        assert_eq!(percent_decode("My%20Share"), "My Share");
        assert_eq!(percent_decode("plain"), "plain");
        assert_eq!(percent_decode("bad%zz"), "bad%zz");
        assert_eq!(percent_decode("trail%2"), "trail%2");
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

    // ── Mount-table parsing (audit 2026-09-11 M-1/M-2) ──

    const TABLE: &str = "\
/dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)
devfs on /dev (devfs, local, nobrowse)
//chris@nas/Tank on /Volumes/Tank (smbfs, nodev, nosuid, mounted by chris)
//chris@nas/Tank_Archive on /Volumes/Tank_Archive (smbfs, nodev, nosuid, mounted by chris)
//alice@nas/Projects on /Volumes/Projects (smbfs, nodev, nosuid, mounted by alice)
//chris@nas/Projects on /Volumes/Projects-1 (smbfs, nodev, nosuid, mounted by chris)
//first%20last@nas.local/My%20Share/Deep%20Sub on /Volumes/Deep Sub (smbfs, nodev, nosuid, mounted by chris)
/dev/disk5s1 on /Volumes/Jobs_Live (apfs, local, nodev, nosuid, journaled, noowners)
localhost:/Jobs_Live on /Users/chris/ufb/mounts/Jobs_Live (nfs, nodev, nosuid, mounted by chris)
";

    #[test]
    fn test_parse_mount_line_shapes() {
        let e = parse_mount_line("//chris@nas/Tank on /Volumes/Tank (smbfs, nodev, nosuid, mounted by chris)").unwrap();
        assert_eq!(e, MountEntry { source: "//chris@nas/Tank", mount_point: "/Volumes/Tank", fstype: "smbfs" });
        // Mount point with a space survives (trailing " (" split first).
        let e = parse_mount_line("//a@nas/My%20Share on /Volumes/My Share (smbfs, nodev)").unwrap();
        assert_eq!(e.mount_point, "/Volumes/My Share");
        assert_eq!(e.fstype, "smbfs");
        let e = parse_mount_line("localhost:/Jobs on /Users/c/ufb/mounts/Jobs (nfs, nodev)").unwrap();
        assert_eq!(e.fstype, "nfs");
        assert!(parse_mount_line("garbage").is_none());
    }

    #[test]
    fn test_smb_source_host_path() {
        assert_eq!(smb_source_host_path("//chris@nas/Tank").as_deref(), Some("nas/tank"));
        assert_eq!(
            smb_source_host_path("//first%20last@nas.local/My%20Share/Deep%20Sub").as_deref(),
            Some("nas.local/my share/deep sub")
        );
        assert_eq!(smb_source_host_path("//nas:139/Share").as_deref(), Some("nas/share"));
        assert_eq!(smb_source_host_path("//a%40b@nas/x").as_deref(), Some("nas/x"));
        assert_eq!(smb_source_host_path("localhost:/Jobs"), None);
        assert_eq!(smb_source_host_path("/dev/disk3s1"), None);
    }

    #[test]
    fn test_tank_does_not_match_tank_archive() {
        // M-1: `nas/tank` used to substring-match `//chris@nas/Tank_Archive`.
        assert_eq!(
            find_smb_mount_point_in(TABLE, r"\\nas\Tank").as_deref(),
            Some("/Volumes/Tank")
        );
        assert_eq!(
            find_smb_mount_point_in(TABLE, r"\\nas\Tank_Archive").as_deref(),
            Some("/Volumes/Tank_Archive")
        );
        assert_eq!(owner_at_in(TABLE, "/Volumes/Tank_Archive", r"\\nas\Tank"), Some(false));
        assert_eq!(owner_at_in(TABLE, "/Volumes/Tank", r"\\nas\Tank"), Some(true));
        assert_eq!(owner_at_in(TABLE, "/Volumes/Tank", r"\\NAS\tank"), Some(true));
        // A share that's a prefix of a mounted one is NOT mounted.
        assert_eq!(find_smb_mount_point_in(TABLE, r"\\nas\Tan"), None);
    }

    #[test]
    fn test_projects_dedup_does_not_adopt_foreign_volume() {
        // M-2: alice's /Volumes/Projects must not be adopted by chris's
        // \\nas\Projects config just because the name matches — the
        // exact source check picks /Volumes/Projects-1 (mounted by us).
        // Both sources are `nas/projects`, so ownership is by the
        // *share*, which is the best the mount table can tell us; the
        // point is that `/Volumes/Projects` (contains) no longer beats
        // `/Volumes/Projects-1` (exact) — the first exact-source hit
        // in table order wins and both are legitimately our share.
        let table_foreign = "\
/dev/disk5s1 on /Volumes/Projects (apfs, local, nodev, nosuid, journaled, noowners)
//chris@nas/Projects on /Volumes/Projects-1 (smbfs, nodev, nosuid, mounted by chris)
";
        assert_eq!(
            find_volume_in(table_foreign, "Projects", r"\\nas\Projects").as_deref(),
            Some("/Volumes/Projects-1")
        );
        assert_eq!(owner_at_in(table_foreign, "/Volumes/Projects", r"\\nas\Projects"), Some(false));
        assert_eq!(owner_at_in(table_foreign, "/Volumes/Projects-1", r"\\nas\Projects"), Some(true));
        // Mount point is exact: "/Volumes/Projects" must not hit "-1".
        assert!(is_mount_point_in(table_foreign, "/Volumes/Projects-1"));
        assert!(!is_mount_point_in(table_foreign, "/Volumes/Project"));
    }

    #[test]
    fn test_foreign_share_with_same_leaf_is_not_ours() {
        // Same leaf name, different share → Some(false), never adopted.
        let table = "//bob@othernas/Projects on /Volumes/Projects (smbfs, nodev, nosuid, mounted by bob)\n";
        assert_eq!(owner_at_in(table, "/Volumes/Projects", r"\\nas\Projects"), Some(false));
        assert_eq!(find_volume_in(table, "Projects", r"\\nas\Projects"), None);
        assert_eq!(find_smb_mount_point_in(table, r"\\nas\Projects"), None);
    }

    #[test]
    fn test_deep_path_and_percent_decoding() {
        assert_eq!(
            find_smb_mount_point_in(TABLE, r"\\nas.local\My Share\Deep Sub").as_deref(),
            Some("/Volumes/Deep Sub")
        );
        assert_eq!(
            owner_at_in(TABLE, "/Volumes/Deep Sub", r"\\nas.local\My Share\Deep Sub"),
            Some(true)
        );
        // The parent share alone is not the deep mount.
        assert_eq!(find_smb_mount_point_in(TABLE, r"\\nas.local\My Share"), None);
    }

    #[test]
    fn test_non_smb_entry_at_path_is_foreign() {
        // A local APFS volume squatting /Volumes/Jobs_Live is never ours.
        assert_eq!(owner_at_in(TABLE, "/Volumes/Jobs_Live", r"\\nas\Jobs_Live"), Some(false));
        assert_eq!(owner_at_in(TABLE, "/Volumes/Nope", r"\\nas\Nope"), None);
        // NFS loopback lines are ignored by the SMB scans.
        assert_eq!(find_smb_mount_point_in(TABLE, r"\\localhost\Jobs_Live"), None);
    }

    #[test]
    fn test_find_volume_in_accepts_dedup_of_our_share() {
        assert_eq!(
            find_volume_in(TABLE, "Projects", r"\\nas\Projects").as_deref(),
            // alice's /Volumes/Projects has the same source (nas/projects) —
            // table order wins; both are the same share.
            Some("/Volumes/Projects")
        );
        assert_eq!(find_volume_in(TABLE, "Tank", r"\\nas\Tank").as_deref(), Some("/Volumes/Tank"));
        assert_eq!(find_volume_in(TABLE, "Tank", r"\\nas\Tank_Archive"), None);
    }
}
