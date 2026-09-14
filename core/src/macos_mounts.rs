//! Plain (non-sync) SMB mounting for the GUI process — plans/17 F1.
//!
//! Ported from `agent/src/platform/macos/{netfs,fallback}.rs` so the
//! app can own plain mounts directly. The agent keeps its own copy
//! for sync-BACKING mounts until F2 dedupes (the agent crate does not
//! depend on ufb-core; a shared home needs a new tiny crate —
//! deferred). Behavior is identical to the agent's mounting as of
//! 2026-07-10: Keychain credentials (NULL user/pass into NetFS), the
//! one-shot allow_ui for NetAuthAgent's dialog, mount-reuse scans,
//! dead-squatter pre-flight, and mount-table ownership guards. See
//! the agent originals for the full design commentary.

use std::path::Path;
use std::process::Command;
use std::sync::{Mutex, MutexGuard, OnceLock};

/// Global mutex serializing the NetFS call (and the squatter
/// pre-flight right before it) so two concurrent mounts can't both
/// decide `/Volumes/<leaf>` is free and race NetFS for it. Held ONLY
/// around that window (audit 2026-09-11 P2 dead-mount path): the
/// mount-table scans before it are table-only and the liveness probe
/// is bounded, but a hung read_dir under this lock used to queue every
/// other GUI-owned mount behind one dead server.
static MOUNT_LOCK: OnceLock<Mutex<()>> = OnceLock::new();
fn mount_mutex() -> MutexGuard<'static, ()> {
    // A poisoned lock (a mount thread panicked while holding it) only
    // guards a serialization window — the data behind it is `()`, so
    // recovering the guard is always safe. Refusing would strand every
    // later mount in the process.
    MOUNT_LOCK
        .get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|e| e.into_inner())
}

// ── mount(8) line parsing ───────────────────────────────────────────
//
// audit 2026-09-11 M-1/M-2: every mount-table check used to be a
// `contains(fragment)` substring test, so a configured `\\nas\Tank`
// adopted a Finder-mounted `//user@nas/tank_archive` line, and a
// share sitting at `/Volumes/Projects-1` claimed a foreign volume at
// `/Volumes/Projects` because the -1 line "contains" the shorter
// path. A Stop / Restart / dead-heartbeat then ran `diskutil unmount`
// on the user's OTHER volume. Everything now goes through one parser
// and exact (case-insensitive) comparison of the decoded source
// against `host/share[/sub]` built from the UNC. The agent's twin in
// agent/src/platform/macos/fallback.rs mirrors this design.

/// One parsed `mount(8)` line.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MountLine {
    /// SMB source with the leading `//` and any `user@` authority
    /// prefix stripped and percent-escapes decoded — `host/share[/sub]`
    /// with no trailing slash. For non-URL sources (`/dev/disk3s1`,
    /// `map auto_home`) it is the raw text before " on ".
    pub source: String,
    /// The mountpoint between " on " and the trailing " (" — may
    /// contain spaces.
    pub mount_point: String,
    /// First token inside the trailing parens (`smbfs`, `apfs`, `nfs`).
    pub fstype: String,
}

/// Parse one line of `mount` output, e.g.
/// `//chris%20bialkowski@192.168.40.100/GFX_Dropbox on /Volumes/GFX_Dropbox (smbfs, nodev, nosuid, mounted by chris)`.
/// Returns None for anything that doesn't have both the " on " and
/// the trailing " (" markers. The source is split at the FIRST " on "
/// (a mountpoint may itself contain " on "; an SMB source can't — the
/// URL is percent-encoded) and the mountpoint at the LAST " (" (the
/// options block is always the tail).
pub fn parse_mount_line(line: &str) -> Option<MountLine> {
    let line = line.trim_end();
    let (source_raw, rest) = line.split_once(" on ")?;
    let (mount_point, opts) = rest.rsplit_once(" (")?;
    let fstype = opts
        .trim_end_matches(')')
        .split(',')
        .next()
        .unwrap_or("")
        .trim()
        .to_string();
    let mount_point = mount_point.trim();
    if source_raw.is_empty() || mount_point.is_empty() {
        return None;
    }
    let source = normalize_mount_source(source_raw);
    Some(MountLine {
        source,
        mount_point: mount_point.to_string(),
        fstype,
    })
}

/// `//[user@]host/share/sub/` → `host/share/sub` (decoded). The
/// authority is everything up to the first `/` after the `//`; the
/// user part is cut at the LAST `@` inside it — a literal `@` in a
/// username is percent-encoded by mount_smbfs, so the last one is
/// always the separator.
fn normalize_mount_source(raw: &str) -> String {
    let Some(rest) = raw.strip_prefix("//") else {
        return raw.trim().to_string();
    };
    let (authority, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, ""),
    };
    let host = authority.rsplit('@').next().unwrap_or(authority);
    let joined = format!("{}{}", strip_port(host), path);
    percent_decode(joined.trim_end_matches('/'))
}

/// `host:445` → `host`; `[::1]:445` → `[::1]`. The UNC side never
/// carries a port, and the agent's twin (`smb_source_host_path`)
/// strips it too — `//guest:@127.0.0.1:22000/union-jobs` must key as
/// `127.0.0.1/union-jobs` in both processes (review 2026-09-11).
fn strip_port(host: &str) -> &str {
    if host.starts_with('[') {
        return match host.find(']') {
            Some(i) => &host[..=i],
            None => host,
        };
    }
    match host.rsplit_once(':') {
        Some((h, port)) if !port.is_empty() && port.bytes().all(|b| b.is_ascii_digit()) => h,
        _ => host,
    }
}

/// Minimal RFC 3986 percent-decoding (`%20` → space). Malformed
/// escapes are kept verbatim so a stray `%` can't erase characters;
/// invalid UTF-8 is replaced lossily — this only feeds comparisons.
fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            // Byte-sliced (not str-sliced) so a multibyte char right
            // after a stray `%` can't land us on a non-char boundary.
            // Both chars must be hex digits: from_str_radix alone
            // accepts a sign, so "%+1" would decode to 0x01.
            let hex = &bytes[i + 1..i + 3];
            if let Some(v) = hex
                .iter()
                .all(|b| b.is_ascii_hexdigit())
                .then(|| std::str::from_utf8(hex).ok())
                .flatten()
                .and_then(|h| u8::from_str_radix(h, 16).ok())
            {
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

/// `\\host\share\sub\` → `host/share/sub`: the comparison key a UNC
/// yields for `MountLine::source`. Empty segments (doubled
/// separators, trailing slash) are dropped so spelling noise in
/// mounts.json can't break a match.
pub fn unc_source_key(nas_share_path: &str) -> String {
    nas_share_path
        .replace('\\', "/")
        .split('/')
        .filter(|seg| !seg.is_empty())
        .collect::<Vec<_>>()
        .join("/")
}

/// True when a parsed smbfs line is backed by exactly the share the
/// UNC names — whole-string, case-insensitive (SMB hosts and share
/// names are case-insensitive; a Finder mount may spell either
/// differently than mounts.json). `Tank` vs `Tank_Archive` and
/// `Projects` vs `Projects-1` are different sources.
pub fn mount_line_matches_unc(line: &MountLine, nas_share_path: &str) -> bool {
    if !line.fstype.eq_ignore_ascii_case("smbfs") {
        return false;
    }
    let key = unc_source_key(nas_share_path);
    !key.is_empty() && line.source.to_lowercase() == key.to_lowercase()
}

/// Snapshot of the mount table, parsed. `mount(8)` reads the kernel's
/// table (getmntinfo) and never touches the mounted filesystems, so
/// this can't hang on a dead server.
fn mount_table() -> Vec<MountLine> {
    let Some(output) = Command::new("mount").output().ok() else {
        return Vec::new();
    };
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter_map(parse_mount_line)
        .collect()
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
    // Extract expected share name for matching against /Volumes/ entries.
    let expected_name = extract_share_name(nas_share_path);

    // Check if already mounted (user-owned location first, then /Volumes/ fallback
    // for shares a user may have mounted manually via Finder). No lock
    // held: these are mount-table reads plus a bounded probe.
    if let Some(existing) = find_existing_user_mount(&expected_name) {
        log::info!("macOS: share already mounted at {}", existing);
        return Ok(existing);
    }
    // Source-of-truth fallback for deep-path SMB URLs. extract_share_name
    // returns the top-level share component (e.g. "Tank" from
    // \\srv\Tank\Deep\DEEP_JOBS), but NetFSMountURLSync names
    // the volume by the leaf path segment (/Volumes/DEEP_JOBS). The
    // name-scoped check searches by share name and misses any deep-path
    // mount inherited from a previous UFB session — without this
    // fallback we re-enter NetFS, get EEXIST, and fail to recover the
    // mountpoint.
    if let Some(existing) = find_existing_volume(&expected_name, nas_share_path)
        .or_else(|| find_mount_by_smb_url(nas_share_path))
    {
        // Both finders match the source EXACTLY (audit M-1/M-2), so
        // this mount is ours to keep or to tear down. Adopting a DEAD
        // one used to hand the task a path that fails its next
        // heartbeat and loops back here forever; unmount it (ours by
        // construction) and fall through to a fresh NetFS mount.
        if probe_path_alive(&existing, PROBE_TIMEOUT) {
            log::info!("macOS: share already mounted at {}", existing);
            return Ok(existing);
        }
        log::warn!(
            "macOS: existing mount of {} at {} is dead — unmounting before remount",
            nas_share_path,
            existing
        );
        if let Err(e) = macos_smb_unmount(&existing) {
            log::warn!("macOS: dead-mount unmount of {} failed ({}); NetFS will dedup", existing, e);
        }
    }

    // Serialize the squatter pre-flight + the ISSUING of the NetFS
    // request so two mounts can't both find /Volumes/<leaf> free and
    // race NetFS for it. The guard travels into netfs_smb_mount and is
    // released the moment NetFSMountURLAsync returns — never across
    // the 60s/600s wait (review 2026-09-11: holding it there queued
    // every other GUI-owned mount behind one slow server).
    let issue_guard = mount_mutex();

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
                && !probe_path_alive(&expected_path, PROBE_TIMEOUT)
            {
                log::warn!(
                    "macOS: dead mount squatting {} — force-unmounting before mount",
                    expected_path
                );
                if let Err(e) = macos_smb_unmount(&expected_path) {
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
    match try_mount_netfs(&expected_name, nas_share_path, allow_ui, issue_guard) {
        Ok(path) => {
            log::info!("macOS: mounted at {} via NetFS (keychain)", path);
            Ok(path)
        }
        Err(status) if status == EAUTH => {
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
        // audit 2026-09-11 P2 (failure visibility): a UI-permitted
        // attempt that outlives MOUNT_TIMEOUT_UI almost always means
        // the NetAuthAgent dialog sat unanswered — the request was
        // cancelled (dismissing the dialog), which is a sign-in
        // outcome, not a network one. Mapping it to Other lost the
        // Sign-in pill and left a generic "error" the user couldn't
        // act on.
        Err(status) if status == libc::ETIMEDOUT && allow_ui => {
            Err(MacosMountError::Auth(format!(
                "Sign-in for {} timed out waiting for the password dialog — use Fix credentials to try again",
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
            status_message(status)
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
    issue_guard: MutexGuard<'static, ()>,
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
    netfs_smb_mount_serialized(&smb_url, None, None, allow_ui, Some(issue_guard))
}

/// True when `path` appears as a mount point in the mount table.
/// Table-based (no stat) so a dead mount can't hang us here. Exact
/// mountpoint comparison via the parser — `/Volumes/Projects` must
/// not match the `/Volumes/Projects-1` line (audit M-2).
fn path_is_mount_point(path: &str) -> bool {
    let path = path.trim_end_matches('/');
    mount_table()
        .iter()
        .any(|l| l.mount_point.trim_end_matches('/') == path)
}

/// Bounded read_dir budget for liveness probes of candidate mounts.
const PROBE_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(3);

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

/// Bounded liveness probe: read_dir on a worker thread with a
/// timeout. A dead SMB mount blocks the read_dir indefinitely — the
/// worker is abandoned (detached) and we report dead. read_dir, not
/// metadata: the kernel serves stale attrs from cache long after the
/// server is gone (same reasoning as the orchestrator heartbeat).
fn probe_path_alive(path: &str, timeout: std::time::Duration) -> bool {
    let (tx, rx) = std::sync::mpsc::channel();
    let p = path.to_string();
    std::thread::spawn(move || {
        // Read the FIRST ENTRY, not just opendir: opendir succeeds off
        // the kernel attr cache on a dead smbfs mount, so `.is_ok()`
        // adopted zombies at Start that the stricter heartbeat (same
        // body as here and as the agent) declared dead 30s later —
        // an adopt/unmount loop (review 2026-09-11).
        let alive = match std::fs::read_dir(&p) {
            Ok(mut rd) => !matches!(rd.next(), Some(Err(_))),
            Err(_) => false,
        };
        let _ = tx.send(alive);
    });
    matches!(rx.recv_timeout(timeout), Ok(true))
}

fn find_existing_user_mount(share_name: &str) -> Option<String> {
    // Mirrors agent config::MountConfig::smb_mount_base() — the legacy
    // private base where mount_smbfs used to park shares. Kept in the
    // reuse scan so pre-NetFS-era mounts are still recognized.
    let smb_base = match std::env::var_os("HOME") {
        Some(home) => std::path::PathBuf::from(home).join(".local/share/ufb/smb-mounts"),
        None => std::path::PathBuf::from("/tmp/ufb-smb-mounts"),
    };
    let candidate = smb_base.join(share_name).to_string_lossy().to_string();
    // Table check rather than the stat-based `is_mountpoint`: a dead
    // legacy mount would otherwise block here before the bounded
    // probes even get a look in.
    if path_is_mount_point(&candidate) {
        Some(candidate)
    } else {
        None
    }
}

/// Unmount an SMB share on macOS.
/// `volumes_path` is the actual /Volumes/... path (not the symlink).
pub fn macos_smb_unmount(volumes_path: &str) -> Result<(), String> {
    // Mount-table check, NOT `Path::exists()`: stat on a dead smbfs
    // mount blocks until the server answers, which is exactly the
    // case this fn is called for (audit 2026-09-11 P2 dead-mount path).
    if !path_is_mount_point(volumes_path) {
        log::info!("macOS: {} is not in the mount table, nothing to unmount", volumes_path);
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

/// Find a `/Volumes/<name>` mount for this share, where `<name>` is the
/// expected share name or a macOS dedup of it (`MyShare-1` when another
/// SMB mount already held `MyShare`) AND the mount table attributes
/// the mountpoint to exactly this UNC's source. Pure table logic —
/// `find_existing_volume_in` is the testable core (audit M-2: the old
/// `line.contains(candidate)` adopted a live foreign `/Volumes/Projects`
/// when our share sat at `/Volumes/Projects-1`). No filesystem touch:
/// the caller probes liveness with a bounded read_dir.
pub fn find_existing_volume(expected_name: &str, nas_share_path: &str) -> Option<String> {
    find_existing_volume_in(&mount_table(), expected_name, nas_share_path)
}

fn find_existing_volume_in(
    table: &[MountLine],
    expected_name: &str,
    nas_share_path: &str,
) -> Option<String> {
    if expected_name.is_empty() {
        return None;
    }
    table
        .iter()
        .filter(|l| mount_line_matches_unc(l, nas_share_path))
        .find(|l| {
            let name = match l.mount_point.strip_prefix("/Volumes/") {
                Some(n) => n.trim_end_matches('/'),
                None => return false,
            };
            !name.contains('/')
                && (name.eq_ignore_ascii_case(expected_name)
                    || strip_macos_dedup_suffix(name)
                        .map(|base| base.eq_ignore_ascii_case(expected_name))
                        .unwrap_or(false))
        })
        .map(|l| l.mount_point.clone())
}

/// Look up an SMB mount's mountpoint by matching `mount(8)` output
/// against the SMB source derived from a UNC path. This is the source
/// of truth for SMB mounts and is independent of volume-name
/// heuristics (which break for deep-path mounts where NetFS names the
/// volume by the leaf path segment, not the SMB share component).
/// Exact source match — see the parser header (audit M-1).
fn find_mount_by_smb_url(nas_share_path: &str) -> Option<String> {
    find_mount_by_smb_url_in(&mount_table(), nas_share_path)
}

fn find_mount_by_smb_url_in(table: &[MountLine], nas_share_path: &str) -> Option<String> {
    table
        .iter()
        .find(|l| mount_line_matches_unc(l, nas_share_path))
        .map(|l| l.mount_point.clone())
}

/// Ownership check for a mountpoint (plans/17 slice C): what the mount
/// table says occupies `path`.
///   None        — nothing mounted there (plain dir or absent)
///   Some(true)  — an smbfs mount whose source is exactly `nas_share_path`
///   Some(false) — someone else's volume (foreign SMB, disk image,
///                 USB, …). Never unmount these: a mount Restart must
///                 not eject the user's identically-named disk — nor,
///                 since audit M-1, their `Tank_Archive` when we own
///                 `Tank`.
pub fn mount_at_path_is_ours(path: &str, nas_share_path: &str) -> Option<bool> {
    mount_at_path_is_ours_in(&mount_table(), path, nas_share_path)
}

fn mount_at_path_is_ours_in(
    table: &[MountLine],
    path: &str,
    nas_share_path: &str,
) -> Option<bool> {
    let path = path.trim_end_matches('/');
    table
        .iter()
        .find(|l| l.mount_point.trim_end_matches('/') == path)
        .map(|l| mount_line_matches_unc(l, nas_share_path))
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


// ── NetFS FFI (from agent netfs.rs) ──────────────────────────────

use core_foundation::base::TCFType;
use core_foundation::dictionary::CFMutableDictionary;
use core_foundation::number::CFNumber;
use core_foundation::string::{CFString, CFStringRef};
use core_foundation::url::{CFURL, CFURLRef};
use core_foundation_sys::array::{CFArrayGetCount, CFArrayGetValueAtIndex, CFArrayRef};
use core_foundation_sys::base::kCFAllocatorDefault;
use core_foundation_sys::dictionary::CFMutableDictionaryRef;
use core_foundation_sys::url::CFURLCreateWithString;
use std::ffi::c_void;
use std::sync::mpsc;
use std::time::Duration;

// Async NetFS + deadline + cancel, twinned from the agent's netfs.rs
// (2026-07-22): every NetFS mount is brokered through the per-user
// NetAuthSysAgent daemon with no timeout of its own — a login-time
// mount aimed at a host behind a not-yet-connected VPN parked inside
// `NetFSMountURLSync` for hours, and a client dying with a request in
// flight wedges the daemon for every process on the machine. See the
// agent's netfs.rs module docs for the full incident write-up. The
// GUI's aboutToQuit calls `cancel_inflight_mounts` (below) so a Cmd-Q
// mid-attempt retires the request through the API instead of
// abandoning it (audit 2026-09-11 M-3).

#[link(name = "NetFS", kind = "framework")]
extern "C" {
    /// `NetFSMountURLAsync(url, mountpath, user, passwd, open_options,
    ///                     mount_options, requestID, queue, mount_report)`
    /// Returns 0 when the request was accepted; the terminal status
    /// arrives via `mount_report` (an ObjC block) on `queue`. Statuses
    /// are errno-style (see `<sys/errno.h>`). Notable codes: `EAUTH`
    /// (80) — bad credentials. `ECANCELED` (89) — user cancelled the
    /// auth dialog, or the request was cancelled via
    /// `NetFSMountURLCancel`. `ETIMEDOUT` (60). The block's
    /// `mountpoints` CFArray of CFString (first entry = actual mount
    /// path; NetFS may pick a fallback like `/Volumes/Share-1` if the
    /// requested path was busy) is owned by NetFS and only valid for
    /// the duration of the callback.
    fn NetFSMountURLAsync(
        url: CFURLRef,
        mountpath: CFURLRef,
        user: CFStringRef,
        passwd: CFStringRef,
        open_options: CFMutableDictionaryRef,
        mount_options: CFMutableDictionaryRef,
        request_id: *mut *mut c_void,
        change_notification_queue: *mut c_void,
        mount_report: *mut c_void,
    ) -> i32;

    /// Cancel an in-flight `NetFSMountURLAsync` request. The
    /// `mount_report` block still fires (with ECANCELED) — cancelling
    /// through the API lets NetAuthSysAgent retire the request; simply
    /// abandoning it (or dying with it in flight) wedges the daemon
    /// for the whole login session.
    fn NetFSMountURLCancel(request_id: *mut c_void) -> i32;
}

extern "C" {
    /// libdispatch — global concurrent queue for the mount_report block.
    fn dispatch_get_global_queue(identifier: isize, flags: usize) -> *mut c_void;
}

/// `kNAUIOptionKey` from `<NetFS/NetFS.h>` — the UI-policy switch in the
/// mount-options dictionary.
const NA_UI_OPTION_KEY: &str = "UIOption";
/// `kNAUIOptionNoUI` — suppress all NetFS-side prompts. With UI allowed,
/// `NetFSMountURLSync` would block on auth failure showing a system
/// dialog; we want failures to return errnos so the GUI can render a
/// "credentials incorrect" pill instead.
const NA_UI_OPTION_NO_UI: i32 = 0;

/// `EAUTH` from `<sys/errno.h>` — surfaced by NetFS when credentials
/// are wrong or missing. Callers map this to an auth-failed mount
/// state so the sidebar can prompt the user to fix credentials.
pub const EAUTH: i32 = 80;

/// Deadline for a silent (no-UI) mount attempt. NetFS's own SMB
/// negotiation finishes in seconds when the host is reachable; anything
/// past this is a hung NetAuthSysAgent request, not a slow server.
const MOUNT_TIMEOUT_SILENT: Duration = Duration::from_secs(60);
/// Deadline when `allow_ui` — the NetAuthAgent credentials dialog may
/// legitimately sit open while the user types, so give it minutes, not
/// seconds. Expiry cancels the request (and dismisses the dialog).
const MOUNT_TIMEOUT_UI: Duration = Duration::from_secs(600);
/// TCP connect budget for the port-445 pre-flight probe.
const PREFLIGHT_TIMEOUT: Duration = Duration::from_secs(3);
/// How long to wait for the ECANCELED callback after
/// `NetFSMountURLCancel` before giving up on a clean retirement.
const CANCEL_GRACE: Duration = Duration::from_secs(10);

// ── In-flight request registry (audit 2026-09-11 M-3) ────────────────
//
// A process that dies (or simply exits) with a NetFSMountURLAsync
// request pending wedges NetAuthSysAgent for the whole login session
// — every later mount from ANY app parks forever until the daemon is
// killed (see the incident notes above). The deadline path below
// cancels its own request, but nothing covered app quit: a user who
// hit Cmd-Q while a VPN-less mount attempt sat in its 60s window left
// the request behind. Every accepted request is registered here so
// `cancel_inflight_mounts` (called from the GUI's aboutToQuit) can
// retire them through the API before the process goes away.

/// One NetFS request. Registered BEFORE `NetFSMountURLAsync` is
/// issued (review 2026-09-11: registering after left a window the
/// quit sweep couldn't see); `request_id` — the opaque pointer NetFS
/// hands back, stored as usize so the entry is Send+Sync — is 0 until
/// the call returns.
struct Inflight {
    url: String,
    request_id: std::sync::atomic::AtomicUsize,
    /// Set by the mount_report block before it sends the result —
    /// the callback has fired, the daemon has retired the request.
    done: std::sync::atomic::AtomicBool,
    /// CAS-guarded so the deadline path and the quit path can't both
    /// call NetFSMountURLCancel on the same id. Once set, a request
    /// that never acknowledges stays registered (see `InflightGuard::
    /// keep_registered`) but the sweep never waits on it again.
    cancelled: std::sync::atomic::AtomicBool,
}

/// How long the quit sweep waits for `NetFSMountURLAsync` to hand back
/// an id for a request that was registered but not yet issued.
const ISSUE_ID_WAIT: Duration = Duration::from_secs(2);

impl Inflight {
    /// Cancel through the API exactly once. Returns the cancel status
    /// (0 = accepted) or None when already cancelled / never started.
    fn cancel(&self) -> Option<i32> {
        use std::sync::atomic::Ordering;
        let id = self.request_id.load(Ordering::SeqCst);
        if id == 0 || self.done.load(Ordering::SeqCst) {
            return None;
        }
        if self
            .cancelled
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return None;
        }
        Some(unsafe { NetFSMountURLCancel(id as *mut c_void) })
    }
}

/// Keyed by a process-local sequence number, not the request id —
/// the entry exists before the id does.
fn inflight_registry() -> &'static Mutex<std::collections::HashMap<u64, std::sync::Arc<Inflight>>> {
    static REG: OnceLock<Mutex<std::collections::HashMap<u64, std::sync::Arc<Inflight>>>> =
        OnceLock::new();
    REG.get_or_init(|| Mutex::new(std::collections::HashMap::new()))
}

/// Insert a fresh entry under the registry lock; returns its key.
fn register_inflight(entry: std::sync::Arc<Inflight>) -> u64 {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    let seq = NEXT.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    if let Ok(mut reg) = inflight_registry().lock() {
        reg.insert(seq, entry);
    }
    seq
}

/// Removes the entry when the mounting thread leaves `netfs_smb_mount`
/// by any path — unless `keep_registered` was called: a request whose
/// cancel was never acknowledged is still live inside NetAuthSysAgent,
/// and the quit sweep must still see it (review 2026-09-11).
struct InflightGuard {
    seq: u64,
    keep: bool,
}

impl InflightGuard {
    fn keep_registered(&mut self) {
        self.keep = true;
    }
}

impl Drop for InflightGuard {
    fn drop(&mut self) {
        if self.keep {
            return;
        }
        if let Ok(mut reg) = inflight_registry().lock() {
            reg.remove(&self.seq);
        }
    }
}

/// Number of NetFS requests currently pending in this process.
pub fn inflight_mount_count() -> usize {
    inflight_registry().lock().map(|r| r.len()).unwrap_or(0)
}

/// Cancel every pending `NetFSMountURLAsync` request and wait (up to
/// `CANCEL_GRACE`) for the daemon to acknowledge each with its
/// ECANCELED callback. Returns how many requests were cancelled.
/// Called from the GUI's aboutToQuit hook — the mounting threads
/// themselves see ECANCELED through their own channels and return;
/// an unacknowledged request is logged and abandoned (the process is
/// exiting either way; the API call is what lets NetAuthSysAgent
/// retire it).
pub fn cancel_inflight_mounts() -> usize {
    use std::sync::atomic::Ordering;
    let pending: Vec<std::sync::Arc<Inflight>> = inflight_registry()
        .lock()
        .map(|r| r.values().cloned().collect())
        .unwrap_or_default();
    if pending.is_empty() {
        return 0;
    }
    let mut cancelled = 0usize;
    // Only the requests THIS sweep cancels are waited on: one already
    // flagged by the deadline path (and never acknowledged) is a
    // wedged daemon — waiting again would just stall quit.
    let mut wait_on: Vec<std::sync::Arc<Inflight>> = Vec::new();
    let id_deadline = std::time::Instant::now() + ISSUE_ID_WAIT;
    for req in &pending {
        if req.done.load(Ordering::SeqCst) {
            continue;
        }
        if req.cancelled.load(Ordering::SeqCst) {
            log::warn!(
                "[netfs] quit: {} was already cancelled and never acknowledged — not waiting",
                req.url
            );
            continue;
        }
        // Registered but NetFSMountURLAsync hasn't returned an id yet:
        // give the issuing thread a moment, then cancel.
        while req.request_id.load(Ordering::SeqCst) == 0
            && !req.done.load(Ordering::SeqCst)
            && std::time::Instant::now() < id_deadline
        {
            std::thread::sleep(Duration::from_millis(20));
        }
        match req.cancel() {
            Some(st) => {
                cancelled += 1;
                wait_on.push(req.clone());
                log::warn!(
                    "[netfs] quit: cancelling in-flight mount {} (cancel_status={})",
                    req.url,
                    st
                );
            }
            None if req.done.load(Ordering::SeqCst) => {
                log::info!("[netfs] quit: in-flight mount {} completed before cancel", req.url)
            }
            None => log::warn!(
                "[netfs] quit: {} has no request id after {}s (NetFSMountURLAsync still blocked) — cannot cancel",
                req.url,
                ISSUE_ID_WAIT.as_secs()
            ),
        }
    }
    let deadline = std::time::Instant::now() + CANCEL_GRACE;
    loop {
        let outstanding: Vec<&std::sync::Arc<Inflight>> = wait_on
            .iter()
            .filter(|r| !r.done.load(Ordering::SeqCst))
            .collect();
        if outstanding.is_empty() {
            log::info!("[netfs] quit: all {} cancelled mount(s) retired", wait_on.len());
            break;
        }
        if std::time::Instant::now() >= deadline {
            for r in outstanding {
                log::warn!(
                    "[netfs] quit: cancel of {} not acknowledged within {}s — abandoning",
                    r.url,
                    CANCEL_GRACE.as_secs()
                );
            }
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    cancelled
}

/// Host component of an `smb://[user@]host[:port]/share` URL.
fn smb_url_host(smb_url: &str) -> Option<&str> {
    let authority = smb_url.strip_prefix("smb://")?.split('/').next()?;
    let host = authority.rsplit('@').next()?.split(':').next()?;
    if host.is_empty() {
        None
    } else {
        Some(host)
    }
}

/// Fast reachability probe of the SMB port. Failing here (VPN not up
/// yet, server down) returns EHOSTUNREACH without ever handing
/// NetAuthSysAgent a request — callers' retry paths re-attempt once
/// the route exists. An unparseable URL passes; NetFS gets to reject it.
fn preflight_smb_reachable(smb_url: &str) -> Result<(), i32> {
    use std::net::{TcpStream, ToSocketAddrs};
    let Some(host) = smb_url_host(smb_url) else {
        return Ok(());
    };
    let addr = match (host, 445u16).to_socket_addrs().ok().and_then(|mut a| a.next()) {
        Some(a) => a,
        None => {
            log::warn!("[netfs] preflight: cannot resolve {} — skipping NetFS call", host);
            return Err(libc::EHOSTUNREACH);
        }
    };
    match TcpStream::connect_timeout(&addr, PREFLIGHT_TIMEOUT) {
        Ok(_) => Ok(()),
        Err(e) => {
            log::warn!(
                "[netfs] preflight: {}:445 unreachable ({}) — skipping NetFS call",
                host,
                e
            );
            Err(libc::EHOSTUNREACH)
        }
    }
}

/// Mount an SMB share via NetFS. Returns the actual mount path on
/// success (which may differ from the requested `mountpoint` if NetFS
/// rerouted) or the raw errno on failure.
///
/// `credentials`: `Some((user, pass))` passes explicit in-memory
/// CFStrings (legacy path). `None` passes NULL for both — NetFS then
/// consults the login Keychain's `kSecClassInternetPassword` entries
/// for this server, exactly like Finder's Cmd-K silent path. This is
/// the OS-native credential flow (plans/17 slice C): UFB never reads
/// or stores the secret itself.
///
/// `allow_ui`: when true the `kNAUIOptionNoUI` suppression is omitted,
/// so on missing/stale credentials NetAuthAgent presents Apple's own
/// Connect-to-Server auth dialog (with "Remember this password in my
/// keychain"). NetAuthAgent hosts that dialog in its own process, so
/// this works from the headless agent too. When false, failures come
/// back as errnos (EAUTH etc.) for the sidebar pill to render.
///
/// When `mountpoint` is `None`, NetFS picks its default location
/// (`/Volumes/<share>`) — that's also Apple's "standard mount path" so
/// macOS Sequoia's "wants to mount to this folder, which is unusual"
/// confirmation never fires. With `Some(path)` we land at exactly that
/// path but eat the prompt every time, so the orchestrator passes
/// `None` and re-points its user-facing symlinks at whatever NetFS
/// returns.
///
/// Blocks the calling thread up to the mount deadline; a request that
/// outlives it is cancelled and reported as ETIMEDOUT, which callers
/// treat as retryable.
pub fn netfs_smb_mount(
    smb_url: &str,
    mountpoint: Option<&Path>,
    credentials: Option<(&str, &str)>,
    allow_ui: bool,
) -> Result<String, i32> {
    netfs_smb_mount_serialized(smb_url, mountpoint, credentials, allow_ui, None)
}

/// `netfs_smb_mount` with an optional serialization guard that is
/// released as soon as `NetFSMountURLAsync` has returned — the wait for
/// the mount_report block runs unlocked.
fn netfs_smb_mount_serialized(
    smb_url: &str,
    mountpoint: Option<&Path>,
    credentials: Option<(&str, &str)>,
    allow_ui: bool,
    issue_guard: Option<MutexGuard<'static, ()>>,
) -> Result<String, i32> {
    log::info!(
        "[netfs] mount {} → {} (creds={}, ui={})",
        smb_url,
        mountpoint
            .map(|p| p.display().to_string())
            .unwrap_or_else(|| "/Volumes/<auto>".into()),
        match &credentials {
            Some((u, _)) => format!("explicit user={}", u),
            None => "keychain".into(),
        },
        allow_ui,
    );

    preflight_smb_reachable(smb_url)?;

    let url_str = CFString::new(smb_url);
    let cf_url = unsafe {
        let raw = CFURLCreateWithString(
            kCFAllocatorDefault,
            url_str.as_concrete_TypeRef(),
            std::ptr::null(),
        );
        if raw.is_null() {
            log::warn!("netfs: CFURLCreateWithString returned null for {}", smb_url);
            return Err(libc::EINVAL);
        }
        CFURL::wrap_under_create_rule(raw)
    };

    // file:// URL of the local mountpoint we want NetFS to mount onto.
    // Optional — Apple's docs explicitly say NULL means "let NetFS pick
    // a default location under /Volumes". Wrapping in Option keeps the
    // pointer story explicit: Some → real CFURL, None → null pointer.
    let cf_mountpath = match mountpoint {
        Some(path) => match CFURL::from_path(path, true) {
            Some(u) => Some(u),
            None => {
                log::warn!("netfs: CFURL::from_path failed for {}", path.display());
                return Err(libc::EINVAL);
            }
        },
        None => None,
    };

    // None → NULL CFStringRefs → NetFS consults the login Keychain
    // (and prompts via NetAuthAgent when allow_ui).
    let cf_creds = credentials
        .map(|(u, p)| (CFString::new(u), CFString::new(p)));

    // Mount options: `{ "UIOption": 0 }` (no UI) unless the caller
    // explicitly allows the system auth dialog. NetFS wants
    // CFMutableDictionaryRef for both options arguments — internally it
    // populates default values into them, and pushing into an immutable
    // dictionary throws a CFException across the FFI boundary, which
    // Rust treats as a foreign exception and aborts the process.
    let mut mount_options: CFMutableDictionary<CFString, CFNumber> =
        CFMutableDictionary::new();
    if !allow_ui {
        mount_options.add(
            &CFString::new(NA_UI_OPTION_KEY),
            &CFNumber::from(NA_UI_OPTION_NO_UI),
        );
    }

    // Empty mutable open_options so NetFS can write its session-level
    // defaults too. Passing null was tempting, but Apple's docs are
    // ambiguous and at least one undocumented codepath dereferences
    // the pointer unconditionally — better to give it a real bag.
    let open_options: CFMutableDictionary<CFString, CFNumber> =
        CFMutableDictionary::new();

    let mountpath_ref = cf_mountpath
        .as_ref()
        .map(|u| u.as_concrete_TypeRef())
        .unwrap_or(std::ptr::null_mut());
    let (user_ref, pass_ref): (CFStringRef, CFStringRef) = match &cf_creds {
        Some((u, p)) => (u.as_concrete_TypeRef(), p.as_concrete_TypeRef()),
        None => (std::ptr::null(), std::ptr::null()),
    };

    // Terminal status arrives via this block on a GCD queue thread.
    // Args are raw-pointer-typed for block2's encoding; `mountpoints`
    // is really a CFArrayRef, owned by NetFS and valid only inside the
    // callback — extract the resolved path here, send it over.
    let (tx, rx) = mpsc::channel::<(i32, Option<String>)>();
    let inflight = std::sync::Arc::new(Inflight {
        url: smb_url.to_string(),
        request_id: std::sync::atomic::AtomicUsize::new(0),
        done: std::sync::atomic::AtomicBool::new(false),
        cancelled: std::sync::atomic::AtomicBool::new(false),
    });
    let inflight_for_block = inflight.clone();
    let report = block2::RcBlock::new(
        move |status: i32, _request_id: *mut c_void, mountpoints: *const c_void| {
            // Mark retired BEFORE handing the result over: the quit
            // path polls `done` and must not outrun the send.
            inflight_for_block
                .done
                .store(true, std::sync::atomic::Ordering::SeqCst);
            let resolved: Option<String> = if status == 0 && !mountpoints.is_null() {
                let arr = mountpoints as CFArrayRef;
                let count = unsafe { CFArrayGetCount(arr) };
                if count > 0 {
                    let raw_item = unsafe { CFArrayGetValueAtIndex(arr, 0) };
                    if raw_item.is_null() {
                        None
                    } else {
                        let s = unsafe {
                            CFString::wrap_under_get_rule(raw_item as CFStringRef)
                        };
                        Some(s.to_string())
                    }
                } else {
                    None
                }
            } else {
                None
            };
            let _ = tx.send((status, resolved));
        },
    );

    // Registered BEFORE issuing so a quit sweep racing this call sees
    // the request (it waits briefly for the id if needed).
    let seq = register_inflight(inflight.clone());
    let mut inflight_guard = InflightGuard { seq, keep: false };

    let mut request_id: *mut c_void = std::ptr::null_mut();
    let start_status = unsafe {
        NetFSMountURLAsync(
            cf_url.as_concrete_TypeRef(),
            mountpath_ref,
            user_ref,
            pass_ref,
            open_options.as_concrete_TypeRef() as CFMutableDictionaryRef,
            mount_options.as_concrete_TypeRef() as CFMutableDictionaryRef,
            &mut request_id,
            dispatch_get_global_queue(0, 0),
            &*report as *const _ as *mut c_void,
        )
    };
    // The request is issued (or refused) — other mounts may proceed
    // to their own pre-flight now; nothing below touches /Volumes.
    drop(issue_guard);
    if start_status != 0 {
        log::warn!(
            "[netfs] NetFSMountURLAsync({}) failed to start: {}",
            smb_url,
            status_message(start_status)
        );
        return Err(start_status);
    }
    inflight
        .request_id
        .store(request_id as usize, std::sync::atomic::Ordering::SeqCst);

    let timeout = if allow_ui { MOUNT_TIMEOUT_UI } else { MOUNT_TIMEOUT_SILENT };
    let (status, resolved) = match rx.recv_timeout(timeout) {
        Ok(done) => done,
        Err(_) => {
            log::warn!(
                "[netfs] mount {} still pending after {}s — cancelling request",
                smb_url,
                timeout.as_secs()
            );
            // CAS-guarded: if the quit sweep already cancelled this
            // id we just wait for the callback it triggered.
            let cancel_status = inflight.cancel().unwrap_or(0);
            match rx.recv_timeout(CANCEL_GRACE) {
                Ok((st, _)) => log::warn!(
                    "[netfs] cancelled mount {} retired with status={}",
                    smb_url,
                    st
                ),
                Err(_) => {
                    log::warn!(
                        "[netfs] cancel of {} not acknowledged (cancel_status={}) — \
                         leaking request arguments",
                        smb_url,
                        cancel_status
                    );
                    // Still live inside the daemon: leave it in the
                    // registry (flagged cancelled) so the quit sweep
                    // knows about it — it will log, not wait.
                    inflight_guard.keep_registered();
                    // The unretired request may still reference these CF
                    // objects from NetAuthSysAgent's side; leak them
                    // rather than risk a use-after-free. Rare (requires a
                    // wedged daemon) and small. The block itself is
                    // refcounted by NetFS's own copy, so dropping our
                    // RcBlock handle is safe either way.
                    std::mem::forget(cf_url);
                    std::mem::forget(cf_mountpath);
                    std::mem::forget(cf_creds);
                    std::mem::forget(open_options);
                    std::mem::forget(mount_options);
                }
            }
            return Err(libc::ETIMEDOUT);
        }
    };

    log::info!(
        "[netfs] NetFSMountURLAsync({}) completed status={}",
        smb_url,
        status,
    );

    if status != 0 {
        return Err(status);
    }

    Ok(resolved.unwrap_or_else(|| {
        // Should not happen on success per Apple's docs — fall back
        // to the requested path (or a placeholder if we asked NetFS to
        // pick).
        mountpoint
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default()
    }))
}

/// Map an NetFS errno to a short human message for log output. The raw
/// number is always preserved so error pills can key off it.
pub fn status_message(status: i32) -> String {
    let label = match status {
        EAUTH => "authentication failed",
        libc::ECANCELED => "sign-in cancelled",
        libc::EACCES => "permission denied",
        libc::EBUSY => "mountpoint busy",
        libc::ENETUNREACH => "network unreachable",
        libc::ETIMEDOUT => "operation timed out",
        libc::ECONNREFUSED => "connection refused",
        libc::EHOSTUNREACH => "no route to host",
        libc::ENOENT => "share not found",
        _ => "mount failed",
    };
    format!("{} (errno {})", label, status)
}

#[cfg(test)]
mod tests {
    use super::*;

    const GFX: &str = "//chris%20bialkowski@192.168.40.100/GFX_Dropbox on /Volumes/GFX_Dropbox (smbfs, nodev, nosuid, mounted by chris)";
    const TANK: &str = "//chris@nas/Tank on /Volumes/Tank (smbfs, nodev, nosuid, mounted by chris)";
    const TANK_ARCHIVE: &str = "//chris@nas/tank_archive on /Volumes/Tank_Archive (smbfs, nodev, nosuid, mounted by chris)";
    const PROJECTS_FOREIGN: &str = "//alice@otherbox/Projects on /Volumes/Projects (smbfs, nodev, nosuid, mounted by chris)";
    const PROJECTS_OURS_DEDUP: &str = "//chris@nas/Projects on /Volumes/Projects-1 (smbfs, nodev, nosuid, mounted by chris)";
    const SPACED: &str = "//chris@nas/My%20Share on /Volumes/My Share (smbfs, nodev, nosuid, mounted by chris)";
    const PORTED: &str = "//guest:@127.0.0.1:22000/union-jobs on /Volumes/union-jobs (smbfs, nodev, nosuid, mounted by chris)";
    const APFS: &str = "/dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)";
    const AUTOFS: &str = "map auto_home on /System/Volumes/Data/home (autofs, automounted, nobrowse)";

    fn table() -> Vec<MountLine> {
        [GFX, TANK, TANK_ARCHIVE, PROJECTS_FOREIGN, PROJECTS_OURS_DEDUP, SPACED, PORTED, APFS, AUTOFS]
            .iter()
            .map(|l| parse_mount_line(l).expect("fixture parses"))
            .collect()
    }

    #[test]
    fn parses_user_prefixed_percent_encoded_line() {
        let l = parse_mount_line(GFX).unwrap();
        assert_eq!(l.source, "192.168.40.100/GFX_Dropbox");
        assert_eq!(l.mount_point, "/Volumes/GFX_Dropbox");
        assert_eq!(l.fstype, "smbfs");
    }

    #[test]
    fn parses_mount_point_with_space_and_decodes_source() {
        let l = parse_mount_line(SPACED).unwrap();
        assert_eq!(l.source, "nas/My Share");
        assert_eq!(l.mount_point, "/Volumes/My Share");
        assert!(mount_line_matches_unc(&l, r"\\nas\My Share"));
    }

    #[test]
    fn parses_non_smb_lines_and_rejects_garbage() {
        let l = parse_mount_line(APFS).unwrap();
        assert_eq!(l.source, "/dev/disk3s1s1");
        assert_eq!(l.mount_point, "/");
        assert_eq!(l.fstype, "apfs");
        assert!(!mount_line_matches_unc(&l, r"\\nas\Tank"));
        let a = parse_mount_line(AUTOFS).unwrap();
        assert_eq!(a.fstype, "autofs");
        assert!(parse_mount_line("").is_none());
        assert!(parse_mount_line("no markers here").is_none());
        assert!(parse_mount_line("//x@h/s on /Volumes/s").is_none());
    }

    #[test]
    fn unc_source_key_normalises_separators() {
        assert_eq!(unc_source_key(r"\\nas\Tank"), "nas/Tank");
        assert_eq!(unc_source_key(r"\\nas\Tank\Deep\"), "nas/Tank/Deep");
        assert_eq!(unc_source_key("//nas/Tank/"), "nas/Tank");
        assert_eq!(unc_source_key(""), "");
    }

    #[test]
    fn percent_decode_keeps_malformed_escapes() {
        assert_eq!(percent_decode("a%20b"), "a b");
        assert_eq!(percent_decode("100%"), "100%");
        assert_eq!(percent_decode("%zz"), "%zz");
        assert_eq!(percent_decode("%+1"), "%+1", "sign is not a hex digit");
        assert_eq!(percent_decode("%C3%A9"), "é");
    }

    // Twin alignment: the agent strips ":port" from the source host.
    #[test]
    fn source_port_is_stripped_like_the_agent_twin() {
        let l = parse_mount_line(PORTED).unwrap();
        assert_eq!(l.source, "127.0.0.1/union-jobs");
        assert!(mount_line_matches_unc(&l, r"\\127.0.0.1\union-jobs"));
        assert_eq!(strip_port("nas"), "nas");
        assert_eq!(strip_port("nas:445"), "nas");
        assert_eq!(strip_port("[::1]:445"), "[::1]");
        assert_eq!(strip_port("nas:notaport"), "nas:notaport");
    }

    // audit M-1: `Tank` must not adopt `tank_archive` (substring) and
    // vice versa; matching is case-insensitive on the whole source.
    #[test]
    fn tank_does_not_match_tank_archive() {
        let t = table();
        assert_eq!(
            find_mount_by_smb_url_in(&t, r"\\nas\Tank").as_deref(),
            Some("/Volumes/Tank")
        );
        assert_eq!(
            find_mount_by_smb_url_in(&t, r"\\nas\Tank_Archive").as_deref(),
            Some("/Volumes/Tank_Archive")
        );
        assert_eq!(
            find_mount_by_smb_url_in(&t, r"\\NAS\tank").as_deref(),
            Some("/Volumes/Tank")
        );
        assert_eq!(find_mount_by_smb_url_in(&t, r"\\nas\Tan"), None);
        assert_eq!(find_mount_by_smb_url_in(&t, r"\\nas\Tank\Sub"), None);
        // Ownership: our Tank task must never call Tank_Archive ours.
        assert_eq!(
            mount_at_path_is_ours_in(&t, "/Volumes/Tank_Archive", r"\\nas\Tank"),
            Some(false)
        );
        assert_eq!(
            mount_at_path_is_ours_in(&t, "/Volumes/Tank", r"\\nas\Tank"),
            Some(true)
        );
        assert_eq!(
            mount_at_path_is_ours_in(&t, "/Volumes/Nope", r"\\nas\Tank"),
            None
        );
    }

    // audit M-2: our share at /Volumes/Projects-1 must not adopt the
    // foreign live volume at /Volumes/Projects.
    #[test]
    fn projects_dedup_does_not_adopt_foreign_projects() {
        let t = table();
        assert_eq!(
            find_existing_volume_in(&t, "Projects", r"\\nas\Projects").as_deref(),
            Some("/Volumes/Projects-1")
        );
        assert_eq!(
            find_existing_volume_in(&t, "Projects", r"\\otherbox\Projects").as_deref(),
            Some("/Volumes/Projects")
        );
        assert_eq!(
            mount_at_path_is_ours_in(&t, "/Volumes/Projects", r"\\nas\Projects"),
            Some(false)
        );
        assert_eq!(
            mount_at_path_is_ours_in(&t, "/Volumes/Projects-1", r"\\nas\Projects"),
            Some(true)
        );
        // Name scoping still applies: a matching source mounted under
        // an unrelated name is the URL finder's job, not this one's.
        assert_eq!(find_existing_volume_in(&t, "Elsewhere", r"\\nas\Projects"), None);
        assert_eq!(
            find_existing_volume_in(&t, "GFX_Dropbox", r"\\192.168.40.100\GFX_Dropbox").as_deref(),
            Some("/Volumes/GFX_Dropbox")
        );
    }

    #[test]
    fn extract_share_name_handles_deep_and_bare() {
        assert_eq!(extract_share_name(r"\\srv\Tank\Deep\DEEP_JOBS"), "Tank");
        assert_eq!(extract_share_name(r"\\srv\Tank"), "Tank");
        assert_eq!(extract_share_name("Tank"), "Tank");
    }

    // One test (the registry is process-global): empty sweep is a
    // no-op; an entry already flagged cancelled-but-unacknowledged is
    // reported, not waited on, and never re-cancelled (no FFI call —
    // the id below is fake, a real NetFSMountURLCancel would crash).
    #[test]
    fn cancel_sweep_skips_flagged_entries_and_does_not_stall() {
        assert_eq!(inflight_mount_count(), 0);
        assert_eq!(cancel_inflight_mounts(), 0);
        let stuck = std::sync::Arc::new(Inflight {
            url: "smb://test/stuck".into(),
            request_id: std::sync::atomic::AtomicUsize::new(0xdead_beef),
            done: std::sync::atomic::AtomicBool::new(false),
            cancelled: std::sync::atomic::AtomicBool::new(true),
        });
        let seq = register_inflight(stuck);
        assert_eq!(inflight_mount_count(), 1);
        let t0 = std::time::Instant::now();
        assert_eq!(cancel_inflight_mounts(), 0);
        assert!(t0.elapsed() < CANCEL_GRACE, "sweep must not wait on a flagged entry");
        inflight_registry().lock().unwrap().remove(&seq);
        assert_eq!(inflight_mount_count(), 0);
    }
}
