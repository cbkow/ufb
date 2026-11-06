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
use std::sync::{mpsc, Arc, Mutex, MutexGuard};
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

// ── Wedged-daemon detection (2026-11-05) ─────────────────────────────
//
// Second wedge mode seen live: NetAuthSysAgent's SMB plugin parked in
// the post-session LSARPC account lookup (`smb_mount →
// setNetworkAccountSID → LsarGetUserName`) after several sessions to
// one server were set up at once. `NetFSMountURLCancel` returned 0 but
// the daemon never retired the request — the thread it would have to
// unwind was waiting on an RPC reply that never came — and every NetFS
// mount on the machine (Finder included) queued behind it. The only
// remedy is restarting the per-user daemon (launchd respawns it on
// demand; the blocked callers return at once; no admin rights needed
// since it runs as the logged-in user). An unacknowledged cancel is the
// one signal we have: a healthy daemon acknowledges a cancel within
// milliseconds even when the server is dead. The flag below records it
// so the orchestrator can surface a fixable row notice; the GUI
// performs the restart (`ufb_core::macos_mounts::restart_netauth_agent`).
// Cleared when a request to the same host completes (the daemon is
// answering again) or when the daemon's pid changes (someone restarted
// it). Twinned in `core/src/macos_mounts.rs`.

/// Row-notice text for a wedged daemon. Kept identical in both twins.
pub const NETAUTH_WEDGED_NOTICE: &str =
    "macOS's mount service (NetAuthSysAgent) is stuck — mounts from every app hang until it is restarted";
/// `MountStateUpdateMsg.notice_fix` value for that notice: the GUI's
/// Fix pill restarts the daemon and retries the mount.
pub const NOTICE_FIX_RESTART_NETAUTH: &str = "restart_netauth";
/// `notice_fix` value for the stale-placeholder drift notice: the
/// GUI's Fix pill removes the directory with admin rights.
pub const NOTICE_FIX_RMDIR_STALE_DIR: &str = "rmdir_stale_dir";

struct WedgeInfo {
    /// NetAuthSysAgent's pid when the wedge was detected (None when
    /// pgrep couldn't tell). A different pid later means the parked
    /// threads died with the old process.
    pid: Option<u32>,
    /// Host of the request whose cancel went unanswered. A completed
    /// request to this host clears the flag.
    host: String,
    /// Last pid re-check, so `daemon_wedged` doesn't fork pgrep on
    /// every state-update tick.
    checked_at: std::time::Instant,
}

static WEDGE: Mutex<Option<WedgeInfo>> = Mutex::new(None);

fn wedge() -> MutexGuard<'static, Option<WedgeInfo>> {
    WEDGE.lock().unwrap_or_else(|e| e.into_inner())
}

/// Minimum interval between pid re-checks in `daemon_wedged`.
const WEDGE_RECHECK: Duration = Duration::from_secs(2);

/// Pid of the current user's NetAuthSysAgent, if it is running. It is
/// launched on demand by the NetFS framework, so None is normal when
/// nothing has mounted anything yet this login session.
pub fn netauth_agent_pid() -> Option<u32> {
    let uid = unsafe { libc::getuid() };
    let out = std::process::Command::new("/usr/bin/pgrep")
        .args(["-x", "-u", &uid.to_string(), "NetAuthSysAgent"])
        .output()
        .ok()?;
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .next()?
        .trim()
        .parse()
        .ok()
}

/// Record that `NetFSMountURLCancel` on a request for `smb_url` was
/// accepted but never acknowledged within `CANCEL_GRACE`.
fn note_cancel_unacknowledged(smb_url: &str) {
    let pid = netauth_agent_pid();
    let host = smb_url_host(smb_url).unwrap_or("").to_string();
    log::warn!(
        "[netfs] NetAuthSysAgent{} is wedged: the cancel of {} went unanswered — \
         every NetFS mount on this Mac (Finder included) will hang until the daemon \
         is restarted (the GUI offers this as a Fix pill; by hand: killall NetAuthSysAgent)",
        pid.map(|p| format!(" (pid {})", p)).unwrap_or_default(),
        smb_url
    );
    *wedge() = Some(WedgeInfo { pid, host, checked_at: std::time::Instant::now() });
}

/// A request for `smb_url` reached its terminal callback: if the wedge
/// was recorded against the same host, the daemon is answering again.
fn note_daemon_responsive(smb_url: &str) {
    let host = smb_url_host(smb_url).unwrap_or("");
    let mut w = wedge();
    let same_host = w.as_ref().map(|i| i.host == host).unwrap_or(false);
    if same_host {
        log::info!(
            "[netfs] NetAuthSysAgent answered a request for {} — clearing the wedged flag",
            host
        );
        *w = None;
    }
}

/// True while an unacknowledged cancel stands against the daemon that
/// is still running. Re-checks the pid at most every `WEDGE_RECHECK`:
/// a new pid (someone ran `killall NetAuthSysAgent`, or the GUI's Fix
/// pill did) clears the flag — the parked threads died with the old
/// process. "Not running" also clears it: launchd will start a fresh
/// daemon for the next request.
pub fn daemon_wedged() -> bool {
    let mut w = wedge();
    let Some(info) = w.as_mut() else {
        return false;
    };
    if info.checked_at.elapsed() < WEDGE_RECHECK {
        return true;
    }
    info.checked_at = std::time::Instant::now();
    match (info.pid, netauth_agent_pid()) {
        (Some(old), Some(now)) if old == now => true,
        (None, _) => true, // couldn't identify the wedged daemon; wait for a callback
        (Some(old), now) => {
            log::info!(
                "[netfs] NetAuthSysAgent pid changed ({} → {:?}) — the wedged daemon is gone, clearing the flag",
                old, now
            );
            *w = None;
            false
        }
    }
}

// ── Cross-process issue gate (2026-11-05) ────────────────────────────
//
// Proven live (UFB closed, no session to the server): three fresh-
// session mounts handed to NetAuthSysAgent in the same millisecond →
// one mounts in under a second, the other two park forever in the
// LSARPC account lookup (`smb_mount → setNetworkAccountSID →
// LsarGetUserName`) inside the daemon's shared DCE-RPC runtime. The
// same three issued one after another → 0.8s / 0.2s / 0.1s, nothing
// parked. Three separate `mount_smbfs` processes in parallel all
// succeed too — the collision is inside the ONE daemon every NetFS
// client shares. UFB produced exactly that collision at every login:
// the agent's sync mount and the GUI's plain mounts fail the pre-flight
// together (VPN not up yet) and retry on the same 30s cadence, so three
// fresh sessions to one host reached the daemon within a millisecond.
//
// So: one NetFS issue at a time, machine-wide, for this user. An
// advisory `flock` on a file in the user's UFB data dir — the GUI and
// the agent are separate processes, so an in-process mutex can't do it
// (the other twin takes the same file; flock also excludes a second
// open of the file within one process). Held from just before
// `NetFSMountURLAsync` until the mount callback arrives or
// `GATE_HOLD_CAP` passes, whichever is first: a healthy mount releases
// it in well under a second; a hung one stops blocking the others after
// the cap (they then meet the same hung daemon — no worse than before).
// A waiter that can't get the lock within `GATE_WAIT` proceeds anyway:
// the gate must never turn a slow mount into a lost one. Lock-file
// trouble (no HOME, unwritable dir) disables the gate for that attempt
// with a warning; the mount still runs, just ungated. Twinned in the
// agent's netfs.rs and core's macos_mounts.rs.

/// Longest a mount holds the gate while waiting for its callback.
const GATE_HOLD_CAP: Duration = Duration::from_secs(15);
/// Longest a mount waits for the gate before proceeding ungated.
/// Deliberately longer than `GATE_HOLD_CAP` so a waiter normally gets
/// its turn.
const GATE_WAIT: Duration = Duration::from_secs(20);
/// `flock(LOCK_NB)` retry interval while waiting for the gate.
const GATE_POLL: Duration = Duration::from_millis(50);

fn gate_path() -> Option<std::path::PathBuf> {
    gate_path_in(std::path::PathBuf::from(std::env::var_os("HOME")?))
}

fn gate_path_in(home: std::path::PathBuf) -> Option<std::path::PathBuf> {
    Some(home.join(".local/share/ufb/netfs-issue.lock"))
}

/// The gate as held by one mount attempt. `file` is `Some` only while
/// the flock is held; dropping (or `release`) unlocks. Never blocks
/// anything on error — see the module notes above.
struct IssueGate {
    file: Option<std::fs::File>,
    smb_url: String,
}

impl IssueGate {
    fn acquire(smb_url: &str) -> IssueGate {
        Self::acquire_at(smb_url, gate_path())
    }

    fn ungated(smb_url: &str) -> IssueGate {
        IssueGate { file: None, smb_url: smb_url.to_string() }
    }

    fn acquire_at(smb_url: &str, path: Option<std::path::PathBuf>) -> IssueGate {
        use std::os::fd::AsRawFd;
        let Some(path) = path else {
            log::warn!("[netfs] issue gate: no HOME — mounting {} ungated", smb_url);
            return Self::ungated(smb_url);
        };
        if let Some(dir) = path.parent() {
            if let Err(e) = std::fs::create_dir_all(dir) {
                log::warn!(
                    "[netfs] issue gate: cannot create {} ({}) — mounting {} ungated",
                    dir.display(),
                    e,
                    smb_url
                );
                return Self::ungated(smb_url);
            }
        }
        let file = match std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&path)
        {
            Ok(f) => f,
            Err(e) => {
                log::warn!(
                    "[netfs] issue gate: cannot open {} ({}) — mounting {} ungated",
                    path.display(),
                    e,
                    smb_url
                );
                return Self::ungated(smb_url);
            }
        };
        let started = std::time::Instant::now();
        loop {
            let rc = unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
            if rc == 0 {
                let waited = started.elapsed();
                if waited >= Duration::from_secs(1) {
                    log::info!(
                        "[netfs] issue gate: waited {:.1}s behind another mount before issuing {}",
                        waited.as_secs_f64(),
                        smb_url
                    );
                }
                return IssueGate { file: Some(file), smb_url: smb_url.to_string() };
            }
            let err = std::io::Error::last_os_error();
            let busy = matches!(err.raw_os_error(), Some(libc::EWOULDBLOCK) | Some(libc::EINTR));
            if !busy {
                log::warn!(
                    "[netfs] issue gate: flock on {} failed ({}) — mounting {} ungated",
                    path.display(),
                    err,
                    smb_url
                );
                return Self::ungated(smb_url);
            }
            if started.elapsed() >= GATE_WAIT {
                log::warn!(
                    "[netfs] issue gate: still held by another mount after {}s — issuing {} ungated",
                    GATE_WAIT.as_secs(),
                    smb_url
                );
                return Self::ungated(smb_url);
            }
            std::thread::sleep(GATE_POLL);
        }
    }

    /// Let the next mount issue. Idempotent; also runs on drop.
    fn release(&mut self) {
        use std::os::fd::AsRawFd;
        if let Some(file) = self.file.take() {
            unsafe {
                libc::flock(file.as_raw_fd(), libc::LOCK_UN);
            }
            // close → any lock the kernel still associates with this
            // open file description is dropped too.
            drop(file);
        }
    }

    fn is_held(&self) -> bool {
        self.file.is_some()
    }
}

impl Drop for IssueGate {
    fn drop(&mut self) {
        self.release();
    }
}

/// Wait for the mount callback with the gate held for at most
/// `GATE_HOLD_CAP`, then release it and keep waiting for the rest of
/// `timeout`. The gate is released on every exit path.
fn recv_gated(
    rx: &mpsc::Receiver<(i32, Option<String>)>,
    gate: &mut IssueGate,
    timeout: Duration,
) -> Result<(i32, Option<String>), mpsc::RecvTimeoutError> {
    let gated = timeout.min(GATE_HOLD_CAP);
    match rx.recv_timeout(gated) {
        Ok(done) => {
            gate.release();
            Ok(done)
        }
        Err(mpsc::RecvTimeoutError::Timeout) if timeout > gated => {
            if gate.is_held() {
                log::info!(
                    "[netfs] issue gate: {} still pending after {}s — releasing the gate so other mounts can proceed",
                    gate.smb_url,
                    gated.as_secs()
                );
            }
            gate.release();
            rx.recv_timeout(timeout - gated)
        }
        Err(e) => {
            gate.release();
            Err(e)
        }
    }
}

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
            for url in stuck {
                note_cancel_unacknowledged(url);
            }
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
    /// Machine-wide issue gate, held until the callback or
    /// `GATE_HOLD_CAP` (see `recv_gated`).
    gate: IssueGate,
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
        let url_cb = smb_url.to_string();
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
                // Any terminal callback — success, error, or the
                // ECANCELED of a replayed request after a daemon
                // restart — proves the daemon is answering for this
                // host again.
                note_daemon_responsive(&url_cb);
                let _ = tx.send((status, resolved));
            },
        );

        // One NetFS issue at a time across the GUI and the agent (see
        // the gate notes above). Taken before the registry lock so a
        // long gate wait never blocks the shutdown sweep.
        let gate = IssueGate::acquire(smb_url);

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
            gate,
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
    pub fn wait(mut self) -> Result<String, i32> {
        let timeout = if self.allow_ui { MOUNT_TIMEOUT_UI } else { MOUNT_TIMEOUT_SILENT };
        // Gate released inside recv_gated on every path — including
        // before the `mem::forget(self)` below, which must never leak
        // a held flock.
        let (status, resolved) = match recv_gated(&self.rx, &mut self.gate, timeout) {
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
                        note_cancel_unacknowledged(&self.smb_url);
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
    fn test_issue_gate_excludes_second_opener_and_releases() {
        use std::os::fd::AsRawFd;
        let home = std::env::temp_dir().join(format!("ufb-gate-test-{}", std::process::id()));
        let path = gate_path_in(home.clone());
        let mut g = IssueGate::acquire_at("smb://h/s", path.clone());
        assert!(g.is_held(), "first acquire must hold the gate");
        let p = path.clone().unwrap();
        let second = std::fs::OpenOptions::new().read(true).write(true).open(&p).unwrap();
        let rc = unsafe { libc::flock(second.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
        assert_eq!(rc, -1, "a second opener must be excluded while the gate is held");
        g.release();
        assert!(!g.is_held());
        let rc = unsafe { libc::flock(second.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
        assert_eq!(rc, 0, "release must let the next opener in");
        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn test_issue_gate_without_home_is_ungated() {
        let g = IssueGate::acquire_at("smb://h/s", None);
        assert!(!g.is_held());
    }

    #[test]
    fn test_daemon_wedged_default_false() {
        assert!(!daemon_wedged());
    }

    #[test]
    fn test_smb_url_host_invalid() {
        assert_eq!(smb_url_host("nfs://nas/share"), None);
        assert_eq!(smb_url_host("smb:///share"), None);
    }
}
