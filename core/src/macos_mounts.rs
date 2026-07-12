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
    netfs_smb_mount(
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
    // Mirrors agent config::MountConfig::smb_mount_base() — the legacy
    // private base where mount_smbfs used to park shares. Kept in the
    // reuse scan so pre-NetFS-era mounts are still recognized.
    let smb_base = match std::env::var_os("HOME") {
        Some(home) => std::path::PathBuf::from(home).join(".local/share/ufb/smb-mounts"),
        None => std::path::PathBuf::from("/tmp/ufb-smb-mounts"),
    };
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


// ── NetFS FFI (from agent netfs.rs) ──────────────────────────────

use core_foundation::base::TCFType;
use core_foundation::dictionary::CFMutableDictionary;
use core_foundation::number::CFNumber;
use core_foundation::string::{CFString, CFStringRef};
use core_foundation::url::{CFURL, CFURLRef};
use core_foundation_sys::array::{CFArrayGetCount, CFArrayGetValueAtIndex, CFArrayRef};
use core_foundation_sys::base::{kCFAllocatorDefault, CFRelease};
use core_foundation_sys::dictionary::CFMutableDictionaryRef;
use core_foundation_sys::url::CFURLCreateWithString;

#[link(name = "NetFS", kind = "framework")]
extern "C" {
    /// `NetFSMountURLSync(url, mountpath, user, passwd, open_options,
    ///                    mount_options, mountpoints)`
    /// Returns 0 on success, errno-style on failure (see `<sys/errno.h>`).
    /// Notable codes: `EAUTH` (80) — bad credentials. `ECANCELED` (89)
    /// — user cancelled (only with UI allowed). `ETIMEDOUT` (60).
    /// `mountpoints` (out) is a CFArray of CFString; first entry is the
    /// path the share was actually mounted at (NetFS may pick a fallback
    /// like `/Volumes/Share-1` if the requested path was busy).
    fn NetFSMountURLSync(
        url: CFURLRef,
        mountpath: CFURLRef,
        user: CFStringRef,
        passwd: CFStringRef,
        open_options: CFMutableDictionaryRef,
        mount_options: CFMutableDictionaryRef,
        mountpoints: *mut CFArrayRef,
    ) -> i32;
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
pub fn netfs_smb_mount(
    smb_url: &str,
    mountpoint: Option<&Path>,
    credentials: Option<(&str, &str)>,
    allow_ui: bool,
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

    let mut mountpoints: CFArrayRef = std::ptr::null();
    let mountpath_ref = cf_mountpath
        .as_ref()
        .map(|u| u.as_concrete_TypeRef())
        .unwrap_or(std::ptr::null_mut());
    let (user_ref, pass_ref): (CFStringRef, CFStringRef) = match &cf_creds {
        Some((u, p)) => (u.as_concrete_TypeRef(), p.as_concrete_TypeRef()),
        None => (std::ptr::null(), std::ptr::null()),
    };
    let status = unsafe {
        NetFSMountURLSync(
            cf_url.as_concrete_TypeRef(),
            mountpath_ref,
            user_ref,
            pass_ref,
            open_options.as_concrete_TypeRef() as CFMutableDictionaryRef,
            mount_options.as_concrete_TypeRef() as CFMutableDictionaryRef,
            &mut mountpoints,
        )
    };

    log::info!(
        "[netfs] NetFSMountURLSync({}) returned status={}",
        smb_url,
        status,
    );

    if status != 0 {
        if !mountpoints.is_null() {
            unsafe { CFRelease(mountpoints as _) };
        }
        return Err(status);
    }

    // Out array: CFArray of CFString (per Apple's NetFS.h). Use the sys
    // API to read index 0 by raw pointer, retain it as a CFString, then
    // release the array. Skipping the typed `CFArray<CFString>` wrapper
    // because mismatched-type assumptions there can throw a CF
    // exception across the FFI boundary, which Rust treats as a
    // foreign exception and aborts the process.
    let resolved: Option<String> = if !mountpoints.is_null() {
        let count = unsafe { CFArrayGetCount(mountpoints) };
        let first = if count > 0 {
            let raw_item = unsafe { CFArrayGetValueAtIndex(mountpoints, 0) };
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
        };
        unsafe { CFRelease(mountpoints as _) };
        first
    } else {
        None
    };

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
