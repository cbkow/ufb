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
//!
//! ## In-flight registry (audit 2026-09-11 M-3)
//!
//! The deadline covers a *hung* request, but not a *dying client*: every
//! SIGTERM in the field log showed "shutdown timed out after 15s" — the
//! orchestrator was parked inside the mount call and couldn't see Stop,
//! then `process::exit` fired with the request still pending, which is
//! exactly the wedge-NetAuthSysAgent case above. So every issued request
//! is recorded in [`INFLIGHT`] until its `mount_report` block fires, and
//! [`cancel_inflight_mounts`] lets the exit path cancel-and-wait for all
//! of them before the process goes away. The orchestrator uses the same
//! registry (per-URL) to abort a mount when Stop arrives mid-flight.

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
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
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

/// One issued-but-unretired `NetFSMountURLAsync` request. `request_id`
/// is the opaque pointer NetFS handed back, stored as usize so the
/// registry is `Send`; `done` flips when the `mount_report` block
/// fires, after which the id must not be passed to
/// `NetFSMountURLCancel` again.
struct InflightMount {
    request_id: usize,
    smb_url: String,
    done: Arc<AtomicBool>,
    /// `NetFSMountURLCancel` has already been issued for this request.
    /// Never cancel twice and never WAIT on such an entry again: if the
    /// daemon didn't acknowledge the first cancel it won't acknowledge
    /// a second, and every later caller (Stop, graceful_exit, the 15s
    /// shutdown-timeout path) would otherwise burn the full
    /// CANCEL_GRACE on a flag that can't flip (review 2026-09-11 #6).
    cancel_issued: Arc<AtomicBool>,
}

/// Registry of in-flight NetFS requests (audit 2026-09-11 M-3). Entries
/// are added right after a successful `NetFSMountURLAsync` and removed
/// by the issuing thread once the callback has been received. The lock
/// is held while calling `NetFSMountURLCancel` so a concurrently
/// retiring request can't have its id reused underneath us.
static INFLIGHT: Mutex<Vec<InflightMount>> = Mutex::new(Vec::new());

fn inflight() -> std::sync::MutexGuard<'static, Vec<InflightMount>> {
    // A panic while holding this lock (none expected — the critical
    // sections are a Vec push/retain and an FFI call) must not make
    // shutdown-time cancellation impossible: recover the guard.
    INFLIGHT.lock().unwrap_or_else(|e| e.into_inner())
}

/// Number of NetFS mount requests currently awaiting their callback.
pub fn inflight_count() -> usize {
    inflight().len()
}

/// Cancel every in-flight NetFS mount request and wait (up to
/// `CANCEL_GRACE`) for NetAuthSysAgent to acknowledge each one. Returns
/// how many requests were cancelled. MUST run before the process exits
/// (audit 2026-09-11 M-3): dying with a request pending wedges the
/// per-user NetAuthSysAgent for the whole login session (see module
/// docs). Safe to call with nothing in flight (returns 0 immediately).
pub fn cancel_inflight_mounts() -> usize {
    cancel_inflight_matching(|_| true)
}

/// Cancel only the in-flight request(s) for one SMB URL — the
/// orchestrator's Stop-while-Mounting path. Same acknowledgement wait
/// as `cancel_inflight_mounts`.
pub fn cancel_inflight_mount(smb_url: &str) -> usize {
    cancel_inflight_matching(|u| u == smb_url)
}

fn cancel_inflight_matching(pred: impl Fn(&str) -> bool) -> usize {
    // Snapshot the done-flags of what we cancel so the wait below can
    // observe retirement without re-taking the registry lock in a loop.
    let cancelled: Vec<(String, Arc<AtomicBool>)> = {
        let guard = inflight();
        guard
            .iter()
            .filter(|m| {
                pred(&m.smb_url)
                    && !m.done.load(Ordering::SeqCst)
                    && !m.cancel_issued.swap(true, Ordering::SeqCst)
            })
            .map(|m| {
                let status = unsafe { NetFSMountURLCancel(m.request_id as *mut c_void) };
                log::warn!(
                    "[netfs] cancelling in-flight mount {} (cancel_status={})",
                    m.smb_url,
                    status
                );
                (m.smb_url.clone(), Arc::clone(&m.done))
            })
            .collect()
    };
    if cancelled.is_empty() {
        return 0;
    }
    let deadline = std::time::Instant::now() + CANCEL_GRACE;
    loop {
        if cancelled.iter().all(|(_, d)| d.load(Ordering::SeqCst)) {
            log::info!("[netfs] {} in-flight mount(s) retired after cancel", cancelled.len());
            break;
        }
        if std::time::Instant::now() >= deadline {
            let stuck: Vec<&str> = cancelled
                .iter()
                .filter(|(_, d)| !d.load(Ordering::SeqCst))
                .map(|(u, _)| u.as_str())
                .collect();
            log::warn!(
                "[netfs] cancel not acknowledged within {}s for {:?} — NetAuthSysAgent may be wedged",
                CANCEL_GRACE.as_secs(),
                stuck
            );
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    cancelled.len()
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
/// Convenience wrapper: [`NetfsRequest::issue`] followed by
/// [`NetfsRequest::wait`]. Callers that must not hold a lock across the
/// wait (fallback.rs's `MOUNT_LOCK`, audit 2026-09-11 P2) use the two
/// halves directly.
///
/// Blocks the calling thread up to the mount deadline (see module
/// docs); a request that outlives it is cancelled and reported as
/// ETIMEDOUT, which callers treat as retryable.
#[allow(dead_code)]
pub fn netfs_smb_mount(
    smb_url: &str,
    mountpoint: Option<&Path>,
    credentials: Option<(&str, &str)>,
    allow_ui: bool,
) -> Result<String, i32> {
    NetfsRequest::issue(smb_url, mountpoint, credentials, allow_ui)?.wait()
}

/// An issued `NetFSMountURLAsync` request whose terminal status hasn't
/// been collected yet. Holds every CF object the request references so
/// they outlive the daemon's use of them; `wait` consumes it.
pub struct NetfsRequest {
    smb_url: String,
    mountpoint: Option<std::path::PathBuf>,
    allow_ui: bool,
    request_id: *mut c_void,
    rx: mpsc::Receiver<(i32, Option<String>)>,
    done: Arc<AtomicBool>,
    cancel_issued: Arc<AtomicBool>,
    // Kept alive until the callback has fired (or leaked on a wedged
    // cancel) — NetAuthSysAgent may still reference them.
    _cf_url: CFURL,
    _cf_mountpath: Option<CFURL>,
    _cf_creds: Option<(CFString, CFString)>,
    _open_options: CFMutableDictionary<CFString, CFNumber>,
    _mount_options: CFMutableDictionary<CFString, CFNumber>,
    _report: block2::RcBlock<dyn Fn(i32, *mut c_void, *const c_void)>,
}

impl NetfsRequest {
    /// Issue the mount request. Returns as soon as NetAuthSysAgent has
    /// accepted it (or the TCP pre-flight / CF setup rejected it).
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
    pub fn issue(
        smb_url: &str,
        mountpoint: Option<&Path>,
        credentials: Option<(&str, &str)>,
        allow_ui: bool,
    ) -> Result<Self, i32> {
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
        let done = Arc::new(AtomicBool::new(false));
        let cancel_issued = Arc::new(AtomicBool::new(false));
        let done_cb = Arc::clone(&done);
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
                // Order matters: the registry's cancel path reads `done`
                // to decide whether the request id is still cancellable,
                // so flip it before the waiter can wake and retire the
                // entry.
                done_cb.store(true, Ordering::SeqCst);
                let _ = tx.send((status, resolved));
            },
        );

        let mut request_id: *mut c_void = std::ptr::null_mut();
        let start_status = {
            // Hold the registry lock across the issue so a concurrent
            // `cancel_inflight_mounts` (shutdown) either sees this
            // request registered or runs before it exists — never in
            // between, where it would be issued but uncancellable.
            let mut reg = inflight();
            let st = unsafe {
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
            if st == 0 {
                reg.push(InflightMount {
                    request_id: request_id as usize,
                    smb_url: smb_url.to_string(),
                    done: Arc::clone(&done),
                    cancel_issued: Arc::clone(&cancel_issued),
                });
            }
            st
        };
        if start_status != 0 {
            log::warn!(
                "[netfs] NetFSMountURLAsync({}) failed to start: {}",
                smb_url,
                status_message(start_status)
            );
            return Err(start_status);
        }

        Ok(Self {
            smb_url: smb_url.to_string(),
            mountpoint: mountpoint.map(Path::to_path_buf),
            allow_ui,
            request_id,
            rx,
            done,
            cancel_issued,
            _cf_url: cf_url,
            _cf_mountpath: cf_mountpath,
            _cf_creds: cf_creds,
            _open_options: open_options,
            _mount_options: mount_options,
            _report: report,
        })
    }

    /// Drop this request's registry entry. Called once the callback has
    /// been observed (or we've given up on it).
    fn unregister(&self) {
        let id = self.request_id as usize;
        inflight().retain(|m| m.request_id != id);
    }

    /// Block until the terminal status arrives or the deadline passes.
    /// On expiry the request is cancelled through the API (so the
    /// daemon retires it) and ETIMEDOUT is returned — NOT the ECANCELED
    /// the callback reports, because a deadline expiry is a retryable
    /// network condition, whereas ECANCELED means "the user dismissed
    /// the sign-in dialog" to the caller's error mapping. An external
    /// cancel via the registry (Stop / shutdown) surfaces as ECANCELED;
    /// the orchestrator drops that result by generation.
    pub fn wait(self) -> Result<String, i32> {
        let timeout = if self.allow_ui { MOUNT_TIMEOUT_UI } else { MOUNT_TIMEOUT_SILENT };
        let (status, resolved) = match self.rx.recv_timeout(timeout) {
            Ok(done) => done,
            Err(_) => {
                log::warn!(
                    "[netfs] mount {} still pending after {}s — cancelling request",
                    self.smb_url,
                    timeout.as_secs()
                );
                // Under the registry lock so it can't race the exit-path
                // cancel on the same id.
                let cancel_status = {
                    let _reg = inflight();
                    if self.done.load(Ordering::SeqCst) {
                        0 // callback landed between the timeout and here
                    } else if self.cancel_issued.swap(true, Ordering::SeqCst) {
                        0 // a Stop / exit path already cancelled it
                    } else {
                        unsafe { NetFSMountURLCancel(self.request_id) }
                    }
                };
                match self.rx.recv_timeout(CANCEL_GRACE) {
                    Ok((st, _)) => {
                        log::warn!(
                            "[netfs] cancelled mount {} retired with status={}",
                            self.smb_url,
                            st
                        );
                        self.unregister();
                    }
                    Err(_) => {
                        log::warn!(
                            "[netfs] cancel of {} not acknowledged (cancel_status={}) — \
                             leaking request arguments",
                            self.smb_url,
                            cancel_status
                        );
                        // The unretired request may still reference these CF
                        // objects from NetAuthSysAgent's side; leak them
                        // rather than risk a use-after-free. Rare (requires a
                        // wedged daemon) and small. The block itself is
                        // refcounted by NetFS's own copy, so leaking our
                        // RcBlock handle is harmless either way. Unregister
                        // first: a cancel was issued and not acknowledged, so
                        // there is nothing a later exit-time pass could do
                        // except wait another CANCEL_GRACE for it.
                        self.unregister();
                        std::mem::forget(self);
                    }
                }
                return Err(libc::ETIMEDOUT);
            }
        };
        self.unregister();

        log::info!(
            "[netfs] NetFSMountURLAsync({}) completed status={}",
            self.smb_url,
            status,
        );

        if status != 0 {
            return Err(status);
        }

        Ok(resolved.unwrap_or_else(|| {
            // Should not happen on success per Apple's docs — fall back
            // to the requested path (or a placeholder if we asked NetFS to
            // pick).
            self.mountpoint
                .as_ref()
                .map(|p| p.to_string_lossy().into_owned())
                .unwrap_or_default()
        }))
    }
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
