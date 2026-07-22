//! NetFS framework wrapper for credentialed SMB mounts.
//!
//! `mount_smbfs -N` queries the user's Keychain for `kSecClassInternetPassword`
//! keyed on host + protocol — but UFB stores its credentials as
//! `kSecClassGenericPassword` under `ufb_<key>` service names. The two
//! never meet, so the silent path falls through and Finder pops a Connect
//! to Server dialog every mount.
//!
//! NetFS takes user + password as in-memory CFStrings, so we hand the
//! agent's resolved credentials directly to the mount call: no Keychain
//! class collision, no plaintext password in argv (visible to `ps`), no
//! UI prompting (`kNAUIOptionNoUI`).
//!
//! This is the C API `mount_smbfs` itself wraps internally and what
//! Finder uses for `Cmd-K Connect to Server`. It's the Apple-blessed
//! programmatic mount path.
//!
//! ## Why async + deadline, not `NetFSMountURLSync` (2026-07-22)
//!
//! Every NetFS mount is brokered through the per-user NetAuthSysAgent
//! daemon, and the request has **no timeout of its own**. At login the
//! agent races the network: a mount aimed at a host behind a
//! not-yet-connected VPN parked inside `NetFSMountURLSync` (blocked in
//! `mach_msg` under `NAAA_MountURL`) for hours, freezing that mount's
//! orchestrator in `Mounting` so the retry/backoff machinery never ran.
//! Worse, a client that dies with a request in flight wedges
//! NetAuthSysAgent itself — after that, every NetFS mount from every
//! process on the machine queues forever until the daemon is killed.
//!
//! So: (1) a 3s TCP pre-flight of the SMB port fails fast (EHOSTUNREACH
//! → orchestrator backoff) without ever handing NetAuthSysAgent a
//! request it can't finish, and (2) the mount runs via
//! `NetFSMountURLAsync` under a deadline, with `NetFSMountURLCancel` on
//! expiry so the daemon retires the request cleanly instead of being
//! abandoned mid-flight.

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
use std::path::Path;
use std::sync::mpsc;
use std::time::Duration;

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
/// NetFS would park the request on a system auth dialog; we want
/// failures to return errnos so the GUI can render a "credentials
/// incorrect" pill instead.
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
/// NetAuthSysAgent a request — the orchestrator's backoff retries once
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
/// Blocks the calling thread up to the mount deadline (see module
/// docs); a request that outlives it is cancelled and reported as
/// ETIMEDOUT, which callers treat as retryable.
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
    let report = block2::RcBlock::new(
        move |status: i32, _request_id: *mut c_void, mountpoints: *const c_void| {
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
    if start_status != 0 {
        log::warn!(
            "[netfs] NetFSMountURLAsync({}) failed to start: {}",
            smb_url,
            status_message(start_status)
        );
        return Err(start_status);
    }

    let timeout = if allow_ui { MOUNT_TIMEOUT_UI } else { MOUNT_TIMEOUT_SILENT };
    let (status, resolved) = match rx.recv_timeout(timeout) {
        Ok(done) => done,
        Err(_) => {
            log::warn!(
                "[netfs] mount {} still pending after {}s — cancelling request",
                smb_url,
                timeout.as_secs()
            );
            let cancel_status = unsafe { NetFSMountURLCancel(request_id) };
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

    #[test]
    fn test_smb_url_host_plain() {
        assert_eq!(smb_url_host("smb://192.168.40.100/Jobs_Live"), Some("192.168.40.100"));
    }

    #[test]
    fn test_smb_url_host_userinfo() {
        assert_eq!(
            smb_url_host("smb://first%20last@nas.local/share/deep"),
            Some("nas.local")
        );
    }

    #[test]
    fn test_smb_url_host_port() {
        assert_eq!(smb_url_host("smb://nas:139/share"), Some("nas"));
    }

    #[test]
    fn test_smb_url_host_invalid() {
        assert_eq!(smb_url_host("nfs://nas/share"), None);
        assert_eq!(smb_url_host("smb:///share"), None);
    }
}
