//! Plain (non-sync) SMB mounting for the GUI process — plans/17 slice B.
//!
//! The Windows twin of `macos_mounts.rs`: the app maps non-sync SMB
//! shares to persistent drive letters itself; the agent only hosts the
//! sync VFS. OS-native credentials throughout (plans/17 decision 4):
//!
//! 1. Mount = `WNetAddConnection2W` with a local device name and NULL
//!    credentials — the SMB redirector consults Credential Manager for
//!    the server, exactly like `net use` / Explorer. UFB never touches
//!    the secret. `CONNECT_UPDATE_PROFILE` makes the mapping persistent
//!    so the OS restores it at logon with no UFB process running.
//! 2. `allow_ui=false` (auto attempts: boot, heartbeat, backoff):
//!    missing/stale stored credentials fail with an auth-class code,
//!    which routes to the sidebar's `auth_error` state and its pill.
//! 3. `allow_ui=true` (explicit user action): `WNetUseConnectionW` with
//!    `CONNECT_INTERACTIVE | CONNECT_PROMPT` presents the standard
//!    Windows credential dialog ("Remember my credentials" stores to
//!    Credential Manager); every future silent mount succeeds off it.
//! 4. Letter selection honors `mountDriveLetter` when configured and
//!    free (or already ours); otherwise scans Z:→D: for a free letter.
//!    The caller compares the returned letter against the preference
//!    to surface a drift notice — same UX as macOS `Share-1` drift.
//!
//! Known constraints honored, not fought (plans/17 slice B):
//! - One credential per server per session (`ERROR_SESSION_CREDENTIAL_
//!   CONFLICT`, 1219) — surfaced with a self-explanatory message.
//! - Logon-restored letters can be disconnected ("red X") — a mount
//!   attempt on one gets `ERROR_DEVICE_ALREADY_REMEMBERED` (1202); we
//!   forget the remembered mapping and retry once, which doubles as
//!   the reconnect nudge.
//! - Elevated processes don't see per-user letters; UNC spelling
//!   always works as the fallback.

#![cfg(windows)]

use windows::core::{PCWSTR, PWSTR};
use windows::Win32::Foundation::WIN32_ERROR;
use windows::Win32::NetworkManagement::WNet::{
    WNetAddConnection2W, WNetCancelConnection2W, WNetGetConnectionW, WNetUseConnectionW,
    CONNECT_INTERACTIVE, CONNECT_PROMPT, CONNECT_UPDATE_PROFILE, NETRESOURCEW,
    NET_CONNECT_FLAGS, RESOURCETYPE_DISK,
};
use windows::Win32::Storage::FileSystem::GetLogicalDrives;

/// Typed mount failure so the local-mount task can route auth-class
/// errors to `auth_error` (the sidebar pill) and everything else to
/// the generic error state — mirroring `MacosMountError`.
#[derive(Debug)]
pub enum WindowsMountError {
    /// Credentials missing/rejected (5 / 86 / 1326) or the user
    /// cancelled the interactive dialog — actionable via the pill.
    Auth(String),
    Other(String),
}

impl std::fmt::Display for WindowsMountError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WindowsMountError::Auth(m) | WindowsMountError::Other(m) => write!(f, "{}", m),
        }
    }
}

/// Serialize mount operations. Concurrent tasks snapshot the free-
/// letter set (`GetLogicalDrives` + provider scan); without the lock
/// all of them pick the same letter, one wins, and the rest error into
/// a 30s heartbeat retry. Same pattern as macOS's `MOUNT_LOCK`.
static MOUNT_LOCK: std::sync::OnceLock<std::sync::Mutex<()>> = std::sync::OnceLock::new();
fn mount_mutex() -> &'static std::sync::Mutex<()> {
    MOUNT_LOCK.get_or_init(|| std::sync::Mutex::new(()))
}

const ERROR_SUCCESS: WIN32_ERROR = WIN32_ERROR(0);
const ERROR_ACCESS_DENIED: WIN32_ERROR = WIN32_ERROR(5);
const ERROR_ALREADY_ASSIGNED: WIN32_ERROR = WIN32_ERROR(85);
const ERROR_INVALID_PASSWORD: WIN32_ERROR = WIN32_ERROR(86);
const ERROR_DEVICE_ALREADY_REMEMBERED: WIN32_ERROR = WIN32_ERROR(1202);
const ERROR_SESSION_CREDENTIAL_CONFLICT: WIN32_ERROR = WIN32_ERROR(1219);
const ERROR_LOGON_FAILURE: WIN32_ERROR = WIN32_ERROR(1326);
const ERROR_CANCELLED: WIN32_ERROR = WIN32_ERROR(1223);
const ERROR_NOT_CONNECTED: WIN32_ERROR = WIN32_ERROR(2250);

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

/// Trim a UNC path down to its share root (`\\server\share`).
/// `WNetAddConnection2W` wants a share, not a sub-directory walk —
/// deeper paths return ERROR_BAD_NET_NAME on some builds. Same rule
/// as the agent's `fallback::share_root`.
pub fn share_root(unc: &str) -> String {
    let s = unc.replace('/', "\\");
    let stripped = match s.strip_prefix("\\\\") {
        Some(rest) => rest,
        None => return unc.to_string(),
    };
    let mut parts = stripped.splitn(3, '\\');
    let server = parts.next().unwrap_or("");
    let share = parts.next().unwrap_or("");
    if server.is_empty() || share.is_empty() {
        return unc.to_string();
    }
    format!("\\\\{}\\{}", server, share)
}

/// The path components beyond the share root: `\\srv\share\a\b` → `a\b`,
/// `\\srv\share` → "". The drive letter maps to the share root (WNet
/// requirement), so a deep `nasSharePath` surfaces as `X:\a\b`.
fn deep_remainder(nas_share_path: &str) -> String {
    let s = nas_share_path.replace('/', "\\");
    let root = share_root(&s);
    s.get(root.len()..)
        .unwrap_or("")
        .trim_matches('\\')
        .to_string()
}

/// The user-facing mounted root for `letter` + this share config:
/// `Z:\` for a share-root path, `Z:\Deep\DEEP_JOBS` for a deep one.
fn mounted_root(letter: char, nas_share_path: &str) -> String {
    let rest = deep_remainder(nas_share_path);
    if rest.is_empty() {
        format!("{}:\\", letter)
    } else {
        format!("{}:\\{}", letter, rest)
    }
}

/// The server component of a UNC path (`\\server\share\…` → `server`).
/// Used as the Credential Manager target key for migration.
pub fn unc_server(unc: &str) -> Option<String> {
    let s = unc.replace('/', "\\");
    let stripped = s.strip_prefix("\\\\")?;
    let server = stripped.split('\\').next()?.trim();
    if server.is_empty() {
        None
    } else {
        Some(server.to_string())
    }
}

/// What a mapped drive letter currently points at, per the network
/// provider (`WNetGetConnectionW`). None when the letter has no
/// remembered/active network mapping (local disks included).
pub fn letter_target(letter: char) -> Option<String> {
    let local = wide(&format!("{}:", letter));
    let mut buf = vec![0u16; 1024];
    let mut len = buf.len() as u32;
    let res = unsafe {
        WNetGetConnectionW(
            PCWSTR(local.as_ptr()),
            PWSTR(buf.as_mut_ptr()),
            &mut len,
        )
    };
    if res != ERROR_SUCCESS {
        return None;
    }
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    Some(String::from_utf16_lossy(&buf[..end]))
}

/// Case-insensitive share-root comparison of two UNC spellings.
fn same_share(a: &str, b: &str) -> bool {
    share_root(a).eq_ignore_ascii_case(&share_root(b))
}

/// True/false when the drive letter under `path` (e.g. `Z:\`) is a
/// network mapping we can attribute; `Some(true)` only when it maps to
/// `nas_share_path`'s share root. None when the letter has no network
/// mapping at all (foreign local volume or nothing).
pub fn mount_at_letter_is_ours(path: &str, nas_share_path: &str) -> Option<bool> {
    let letter = path.chars().next()?;
    if !letter.is_ascii_alphabetic() {
        return None;
    }
    let target = letter_target(letter.to_ascii_uppercase())?;
    Some(same_share(&target, nas_share_path))
}

/// Every letter currently occupied — by a local/removable volume or by
/// any network mapping (live or remembered). Feeds the mount editor's
/// letter picker so taken letters render disabled.
pub fn letters_in_use() -> Vec<char> {
    let mask = unsafe { GetLogicalDrives() };
    ('A'..='Z')
        .filter(|&c| {
            let bit = 1u32 << (c as u8 - b'A');
            mask & bit != 0 || letter_target(c).is_some()
        })
        .collect()
}

/// Scan mapped letters for an existing mapping of this share (mount
/// reuse — e.g. the OS restored the persistent letter at logon, or the
/// user mapped it via Explorer). Returns `X:\` on hit.
pub fn find_existing_letter(nas_share_path: &str) -> Option<String> {
    let mask = unsafe { GetLogicalDrives() };
    for i in 0..26u32 {
        if mask & (1 << i) == 0 {
            // No volume present — but a remembered (red-X) mapping can
            // still own the letter; WNetGetConnection reports those too.
        }
        let letter = (b'A' + i as u8) as char;
        if let Some(target) = letter_target(letter) {
            if same_share(&target, nas_share_path) {
                return Some(format!("{}:\\", letter));
            }
        }
    }
    None
}

/// Pick the letter to mount at: the preference when it's free or
/// already ours, else the first free letter scanning Z:→D:. Letters
/// with local volumes or foreign network mappings are skipped. None
/// when everything D:–Z: is taken.
fn choose_letter(preferred: Option<char>, nas_share_path: &str) -> Option<char> {
    let mask = unsafe { GetLogicalDrives() };
    let is_free = |c: char| {
        let bit = 1u32 << (c.to_ascii_uppercase() as u8 - b'A');
        mask & bit == 0 && letter_target(c).is_none()
    };
    if let Some(p) = preferred {
        let p = p.to_ascii_uppercase();
        if p.is_ascii_uppercase() {
            if is_free(p) {
                return Some(p);
            }
            if letter_target(p)
                .map(|t| same_share(&t, nas_share_path))
                .unwrap_or(false)
            {
                return Some(p); // remembered/live mapping of our own share
            }
        }
    }
    ('D'..='Z').rev().find(|&c| is_free(c))
}

/// Map `nas_share_path` to a drive letter with OS-stored credentials.
///
/// Returns the mounted root (`Z:\`). The caller owns drift messaging:
/// compare the returned letter with the preference.
pub fn windows_smb_mount(
    nas_share_path: &str,
    preferred_letter: Option<char>,
    allow_ui: bool,
) -> Result<String, WindowsMountError> {
    let _guard = mount_mutex().lock().unwrap();

    // Reuse an existing mapping of this share wherever it lives —
    // matches the macOS "already mounted" scan. EXCEPT when the mount
    // has an explicit letter preference and the existing mapping sits
    // elsewhere: the configured letter is authoritative (facilities
    // standardize letters across seats so external apps' saved paths
    // match), so the stray mapping is migrated — forgotten here,
    // remounted at the preferred letter below.
    if let Some(existing) = find_existing_letter(nas_share_path) {
        if let Some(letter) = existing.chars().next() {
            let matches_pref = preferred_letter
                .map(|p| p.to_ascii_uppercase() == letter.to_ascii_uppercase())
                .unwrap_or(true); // no preference — anywhere is fine
            if matches_pref {
                // A remembered-but-disconnected (red X) letter still
                // reports a target; a directory probe distinguishes
                // live from dead. Dead falls through to a fresh
                // connect on the same letter.
                if std::fs::read_dir(&existing).is_ok() {
                    let root = mounted_root(letter, nas_share_path);
                    log::info!(
                        "[win-mounts] {} already mapped at {} — reusing",
                        nas_share_path, root
                    );
                    return finish_mount(letter, nas_share_path);
                }
            } else {
                log::info!(
                    "[win-mounts] {} is mapped at {}: but {}: is configured — migrating",
                    nas_share_path,
                    letter,
                    preferred_letter.unwrap_or('?')
                );
                let _ = windows_smb_unmount(&existing, true);
            }
        }
    }

    let Some(letter) = choose_letter(preferred_letter, nas_share_path) else {
        return Err(WindowsMountError::Other(
            "No free drive letter (D:–Z: all in use)".to_string(),
        ));
    };

    let share = share_root(nas_share_path);
    let local = wide(&format!("{}:", letter));
    let remote = wide(&share);

    let mut attempt = 0;
    loop {
        attempt += 1;
        let nr = NETRESOURCEW {
            dwType: RESOURCETYPE_DISK,
            lpLocalName: PWSTR(local.as_ptr() as *mut _),
            lpRemoteName: PWSTR(remote.as_ptr() as *mut _),
            ..Default::default()
        };
        // NULL credentials — Credential Manager supplies them.
        let result = unsafe {
            WNetAddConnection2W(&nr, PCWSTR::null(), PCWSTR::null(), CONNECT_UPDATE_PROFILE)
        };

        match result {
            ERROR_SUCCESS => {
                log::info!("[win-mounts] mounted {} at {}:\\", share, letter);
                return finish_mount(letter, nas_share_path);
            }
            ERROR_ALREADY_ASSIGNED => {
                // Letter grabbed between choose_letter and the call, or
                // an existing mapping of ours the free-scan missed.
                if letter_target(letter)
                    .map(|t| same_share(&t, nas_share_path))
                    .unwrap_or(false)
                {
                    return finish_mount(letter, nas_share_path);
                }
                return Err(WindowsMountError::Other(format!(
                    "Drive {}: is already assigned to another resource",
                    letter
                )));
            }
            ERROR_DEVICE_ALREADY_REMEMBERED if attempt == 1 => {
                // A stale persistent mapping (red-X or old share) owns
                // the letter in the user profile. Forget it and retry
                // once — this is the logon reconnect nudge.
                log::info!(
                    "[win-mounts] {}: has a remembered mapping — forgetting and retrying",
                    letter
                );
                let _ = unsafe {
                    WNetCancelConnection2W(PCWSTR(local.as_ptr()), CONNECT_UPDATE_PROFILE, false)
                };
                continue;
            }
            ERROR_SESSION_CREDENTIAL_CONFLICT => {
                return Err(WindowsMountError::Other(format!(
                    "Windows allows one set of credentials per server per \
                     session and another connection to this server already \
                     uses different ones (error 1219). Disconnect the other \
                     mapping or align its credentials. Share: {}",
                    share
                )));
            }
            ERROR_ACCESS_DENIED | ERROR_INVALID_PASSWORD | ERROR_LOGON_FAILURE => {
                if allow_ui {
                    return interactive_mount(nas_share_path, letter);
                }
                return Err(WindowsMountError::Auth(format!(
                    "Stored credentials for {} were rejected (error {})",
                    share, result.0
                )));
            }
            other => {
                return Err(WindowsMountError::Other(format!(
                    "Failed to map {} to {}:\\ — error {}",
                    share, letter, other.0
                )));
            }
        }
    }
}

/// Tell the shell a drive letter appeared/vanished. Without this,
/// Explorer's This PC keeps showing the pre-change drive list until a
/// manual refresh — WNet/WinFsp mounts made by another process don't
/// broadcast on their own.
pub fn notify_shell_drive(letter: char, added: bool) {
    use windows::Win32::UI::Shell::{
        SHChangeNotify, SHCNE_DRIVEADD, SHCNE_DRIVEREMOVED, SHCNF_PATHW,
    };
    let root = wide(&format!("{}:\\", letter.to_ascii_uppercase()));
    unsafe {
        SHChangeNotify(
            if added { SHCNE_DRIVEADD } else { SHCNE_DRIVEREMOVED },
            SHCNF_PATHW,
            Some(root.as_ptr() as *const core::ffi::c_void),
            None,
        );
    }
}

/// Post-mount finish: validate a deep `nasSharePath`'s sub-directory
/// actually exists on the share and return the user-facing root.
fn finish_mount(letter: char, nas_share_path: &str) -> Result<String, WindowsMountError> {
    let root = mounted_root(letter, nas_share_path);
    if !deep_remainder(nas_share_path).is_empty() && std::fs::metadata(&root).is_err() {
        return Err(WindowsMountError::Other(format!(
            "Mapped {}: but {} does not exist on the share",
            letter, root
        )));
    }
    notify_shell_drive(letter, true);
    Ok(root)
}

/// The `allow_ui` path: standard Windows credential dialog via
/// `WNetUseConnectionW(CONNECT_INTERACTIVE | CONNECT_PROMPT)`. The
/// dialog's "Remember my credentials" writes to Credential Manager, so
/// subsequent silent mounts succeed.
fn interactive_mount(nas_share_path: &str, letter: char) -> Result<String, WindowsMountError> {
    let share = share_root(nas_share_path);
    let local = wide(&format!("{}:", letter));
    let remote = wide(&share);
    let nr = NETRESOURCEW {
        dwType: RESOURCETYPE_DISK,
        lpLocalName: PWSTR(local.as_ptr() as *mut _),
        lpRemoteName: PWSTR(remote.as_ptr() as *mut _),
        ..Default::default()
    };
    let flags = NET_CONNECT_FLAGS(
        CONNECT_INTERACTIVE.0 | CONNECT_PROMPT.0 | CONNECT_UPDATE_PROFILE.0,
    );
    let result = unsafe {
        WNetUseConnectionW(
            None, // no owner window — the dialog centers on screen
            &nr,
            PCWSTR::null(),
            PCWSTR::null(),
            flags,
            PWSTR::null(),
            None,
            None,
        )
    };
    match result {
        ERROR_SUCCESS => {
            log::info!("[win-mounts] interactive mount {} at {}:\\", share, letter);
            finish_mount(letter, nas_share_path)
        }
        ERROR_CANCELLED => Err(WindowsMountError::Auth(format!(
            "Sign-in cancelled for {}",
            share
        ))),
        ERROR_ACCESS_DENIED | ERROR_INVALID_PASSWORD | ERROR_LOGON_FAILURE => {
            Err(WindowsMountError::Auth(format!(
                "Credentials for {} were rejected (error {})",
                share, result.0
            )))
        }
        other => Err(WindowsMountError::Other(format!(
            "Interactive mount of {} at {}:\\ failed — error {}",
            share, letter, other.0
        ))),
    }
}

/// Disconnect the drive letter under `path` (`Z:\`). `forget` removes
/// the persistent profile entry too (explicit user Stop); without it
/// the OS still restores the mapping at next logon (transient unmount
/// during Restart).
pub fn windows_smb_unmount(path: &str, forget: bool) -> Result<(), String> {
    let Some(letter) = path.chars().next().filter(|c| c.is_ascii_alphabetic()) else {
        return Err(format!("not a drive-letter path: {}", path));
    };
    let local = wide(&format!("{}:", letter.to_ascii_uppercase()));
    let flags = if forget {
        CONNECT_UPDATE_PROFILE
    } else {
        NET_CONNECT_FLAGS(0)
    };
    let result = unsafe { WNetCancelConnection2W(PCWSTR(local.as_ptr()), flags, true) };
    if result == ERROR_SUCCESS || result == ERROR_NOT_CONNECTED {
        log::info!("[win-mounts] disconnected {}:\\ (forget={})", letter, forget);
        if result == ERROR_SUCCESS {
            notify_shell_drive(letter.to_ascii_uppercase(), false);
        }
        // A remembered-but-disconnected mapping isn't "connected", so
        // the cancel above reports 2250 without touching the profile.
        // Forgetting must still clear the profile entry or the OS
        // resurrects the letter at next logon.
        if forget && result == ERROR_NOT_CONNECTED {
            let _ = unsafe {
                WNetCancelConnection2W(PCWSTR(local.as_ptr()), CONNECT_UPDATE_PROFILE, false)
            };
        }
        Ok(())
    } else {
        Err(format!(
            "WNetCancelConnection2W failed for {}:\\ — error {}",
            letter, result.0
        ))
    }
}

/// One-time migration (plans/17 slice B): move UFB's private `ufb_*`
/// generic Credential Manager entries to server-keyed
/// `CRED_TYPE_DOMAIN_PASSWORD` entries — the kind `cmdkey /add:server`
/// creates and the SMB redirector consults on its own. After this,
/// UFB never reads or stores an SMB secret again; NULL-credential
/// mounts authenticate via these entries.
///
/// `entries` is (credentialKey, nasSharePath) per configured mount.
/// Rules: an existing domain entry for the server wins (never
/// overwritten); the legacy `ufb_*` entry is deleted once the server
/// entry exists either way. Idempotent — once the legacy set is gone,
/// this is a no-op. Returns how many server entries were written.
pub fn migrate_legacy_credentials(entries: &[(String, String)]) -> u32 {
    use windows::Win32::Security::Credentials::{
        CredDeleteW, CredFree, CredReadW, CredWriteW, CREDENTIALW,
        CRED_PERSIST_LOCAL_MACHINE, CRED_TYPE_DOMAIN_PASSWORD, CRED_TYPE_GENERIC,
    };

    fn read_generic(target: &str) -> Option<(String, String)> {
        use windows::Win32::Security::Credentials::{
            CredFree, CredReadW, CREDENTIALW, CRED_TYPE_GENERIC,
        };
        let t = wide(target);
        let mut ptr: *mut CREDENTIALW = std::ptr::null_mut();
        unsafe {
            CredReadW(PCWSTR(t.as_ptr()), CRED_TYPE_GENERIC, 0, &mut ptr).ok()?;
            let cred = &*ptr;
            let username = if cred.UserName.is_null() {
                String::new()
            } else {
                cred.UserName.to_string().unwrap_or_default()
            };
            // Legacy writers (agent store + CredUI prompt path) wrote
            // the password blob as UTF-8.
            let password = if cred.CredentialBlob.is_null() || cred.CredentialBlobSize == 0 {
                String::new()
            } else {
                let s = std::slice::from_raw_parts(
                    cred.CredentialBlob,
                    cred.CredentialBlobSize as usize,
                );
                String::from_utf8_lossy(s).to_string()
            };
            CredFree(ptr as *const _ as *const core::ffi::c_void);
            Some((username, password))
        }
    }

    fn domain_entry_exists(server: &str) -> bool {
        let t = wide(server);
        let mut ptr: *mut CREDENTIALW = std::ptr::null_mut();
        unsafe {
            if CredReadW(PCWSTR(t.as_ptr()), CRED_TYPE_DOMAIN_PASSWORD, 0, &mut ptr).is_ok() {
                CredFree(ptr as *const _ as *const core::ffi::c_void);
                true
            } else {
                false
            }
        }
    }

    let mut written = 0u32;
    let mut done_servers: std::collections::HashSet<String> = Default::default();

    for (key, nas) in entries {
        if key.is_empty() {
            continue;
        }
        let Some(server) = unc_server(nas) else { continue };
        let legacy_target = format!("ufb_{}", key);
        let Some((username, password)) = read_generic(&legacy_target) else {
            continue; // no legacy entry — nothing to migrate for this key
        };

        let have_domain = done_servers.contains(&server) || domain_entry_exists(&server);
        if !have_domain {
            if username.is_empty() || password.is_empty() {
                log::warn!(
                    "[cred-migrate] {} has an empty username/password — skipping",
                    legacy_target
                );
                continue;
            }
            // Domain credentials carry the secret as UTF-16LE bytes
            // (what cmdkey writes and the redirector expects).
            let target_w = wide(&server);
            let user_w = wide(&username);
            let blob: Vec<u8> = password
                .encode_utf16()
                .flat_map(|u| u.to_le_bytes())
                .collect();
            let cred = CREDENTIALW {
                Type: CRED_TYPE_DOMAIN_PASSWORD,
                TargetName: PWSTR(target_w.as_ptr() as *mut _),
                UserName: PWSTR(user_w.as_ptr() as *mut _),
                CredentialBlobSize: blob.len() as u32,
                CredentialBlob: blob.as_ptr() as *mut _,
                Persist: CRED_PERSIST_LOCAL_MACHINE,
                ..Default::default()
            };
            match unsafe { CredWriteW(&cred, 0) } {
                Ok(()) => {
                    written += 1;
                    log::info!(
                        "[cred-migrate] {} → server-keyed entry for {} (user {})",
                        legacy_target, server, username
                    );
                }
                Err(e) => {
                    log::warn!(
                        "[cred-migrate] CredWriteW for {} failed: {} — keeping {}",
                        server, e, legacy_target
                    );
                    continue; // keep the legacy entry for a retry next launch
                }
            }
        } else {
            log::info!(
                "[cred-migrate] server entry for {} already exists — dropping {}",
                server, legacy_target
            );
        }
        done_servers.insert(server);

        let lt = wide(&legacy_target);
        if let Err(e) = unsafe { CredDeleteW(PCWSTR(lt.as_ptr()), CRED_TYPE_GENERIC, 0) } {
            log::warn!("[cred-migrate] delete {} failed: {}", legacy_target, e);
        }
    }
    written
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn share_root_trims_deep_paths() {
        assert_eq!(
            share_root(r"\\192.168.1.60\Tank\Deep\DEEP_JOBS"),
            r"\\192.168.1.60\Tank"
        );
        assert_eq!(share_root(r"\\nas\share"), r"\\nas\share");
        assert_eq!(share_root(r"//nas/share/sub"), r"\\nas\share");
        assert_eq!(share_root(r"not-unc"), "not-unc");
    }

    #[test]
    fn deep_remainder_and_mounted_root() {
        assert_eq!(
            deep_remainder(r"\\192.168.1.60\Tank\Deep\DEEP_JOBS"),
            r"Deep\DEEP_JOBS"
        );
        assert_eq!(deep_remainder(r"\\nas\share"), "");
        assert_eq!(
            mounted_root('W', r"\\192.168.1.60\Tank\Deep\DEEP_JOBS"),
            r"W:\Deep\DEEP_JOBS"
        );
        assert_eq!(mounted_root('Z', r"\\nas\share"), r"Z:\");
    }

    #[test]
    fn unc_server_extracts_host() {
        assert_eq!(
            unc_server(r"\\192.168.1.50\Projects").as_deref(),
            Some("192.168.1.50")
        );
        assert_eq!(unc_server(r"//nas/share").as_deref(), Some("nas"));
        assert_eq!(unc_server("C:\\local"), None);
    }

    #[test]
    fn same_share_is_case_insensitive_and_root_based() {
        assert!(same_share(r"\\NAS\Share\deep\er", r"\\nas\share"));
        assert!(!same_share(r"\\nas\share", r"\\nas\other"));
    }

    #[test]
    fn unmount_rejects_non_letter_paths() {
        assert!(windows_smb_unmount(r"\\nas\share", false).is_err());
    }
}
