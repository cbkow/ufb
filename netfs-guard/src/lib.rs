//! netfs-guard — the macOS NetFS mount-issue guard shared by the UFB GUI
//! (`ufb-core`) and the agent.
//!
//! Every NetFS mount on a Mac goes through one per-user daemon,
//! `NetAuthSysAgent`. Its SMB plugin does an LSARPC account lookup on
//! every *fresh* session, inside one shared DCE-RPC runtime, and two
//! fresh sessions to one server set up at the same moment park one of
//! them there forever (proven by experiment 2026-11-05: 3 parallel →
//! 2 parked; serial → none). A parked thread pins the kernel SMB
//! session, leaves a mode-0111 placeholder under `/Volumes`, and every
//! later NetFS mount on the machine — Finder included — queues behind
//! it. `NetFSMountURLCancel` returns 0 but never retires the request.
//! The only cure is restarting the daemon (plain user, launchd
//! respawns it), after which the NetFS client library in every process
//! REPLAYS the requests it still has pending.
//!
//! NetFS itself gives us exactly one signal: a callback on completion.
//! Nothing on a hang. The first version of this guard (2026-11-05)
//! therefore inferred everything from elapsed time: 60s deadline, 10s
//! cancel grace, then a pill. Live-tested 2026-11-06 it worked, but the
//! user waited 70s to learn anything and the restart replayed three
//! leaked requests at once, which re-parked the new daemon. This
//! version is built on the three real signals that exist:
//!
//! 1. **Prevention — the gate.** One NetFS issue at a time for this
//!    user, across the GUI and the agent (advisory `flock` on a file in
//!    the UFB data dir). Held from before `NetFSMountURLAsync` until the
//!    request reaches a *verdict* (callback, condemned as wedged, or the
//!    hold cap), not released on a timer. UFB can then never cause the
//!    collision itself and never has more than one request leaked.
//! 2. **Detection — observation.** A healthy mount completes in about a
//!    second. A request still pending after [`OBSERVE_AFTER`] gets the
//!    daemon sampled (`/usr/bin/sample`, ~0.2s, no privilege against a
//!    same-user process): a thread sitting in the LSARPC lookup across
//!    two samples [`CONFIRM_GAP`] apart is a certain diagnosis in about
//!    five seconds. The elapsed-time deadline stays only as the
//!    backstop for hangs with no known fingerprint.
//! 3. **Recovery — bounded restart.** A condemned request is cancelled
//!    (expected to go unanswered), the daemon is restarted automatically
//!    once per [`AUTO_RESTART_COOLDOWN`], the library's useless replay
//!    of the condemned request is given [`RESTART_SETTLE`] to pass, and
//!    a fresh request is issued with the gate still held — about nine
//!    seconds from issue to mounted, no user action. Only a wedge that
//!    recurs inside the cooldown is handed to the user as the
//!    "Restart…" pill. While the flag is set, new issues are refused up
//!    front with [`EWEDGED`] instead of being handed to a daemon that
//!    cannot answer them.
//! 4. **Pacing — one probe per server.** Every share is its own task
//!    with its own retry timer, in two processes. Against a server that
//!    accepts TCP but cannot complete a mount (daemon stuck, SMB
//!    service down, auth backend gone) that is three to five NetFS
//!    requests per cycle, each one slow and each one leakable. After a
//!    host-level failure the host is put on hold ([`host_hold`]) in a
//!    small file every process reads: the next share for that host
//!    returns the same error immediately without touching NetFS, and
//!    the hold doubles from [`HOST_HOLD_BASE`] up to [`HOST_HOLD_CAP`]
//!    until one request to the host succeeds. Explicit user actions
//!    (`allow_ui`) bypass the hold. Share-level failures (bad
//!    credentials, no such share) never hold the host.
//!
//! The twins (`core/src/macos_mounts.rs`, `agent/src/platform/macos/
//! netfs.rs`) keep the CF/FFI plumbing and call in here for the gate,
//! the wait, the verdict and the recovery policy.

use std::collections::BTreeSet;
use std::fs::File;
use std::sync::mpsc::{Receiver, RecvTimeoutError};
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

// ── Notices ──────────────────────────────────────────────────────────

/// Row-notice text for a wedged daemon that the policy would not
/// restart again (recurred within the cooldown).
pub const WEDGED_NOTICE: &str =
    "macOS's mount service (NetAuthSysAgent) is stuck again — mounts from every app hang until it is restarted";
/// `MountStateUpdateMsg.notice_fix` value for that notice.
pub const NOTICE_FIX_RESTART_NETAUTH: &str = "restart_netauth";
/// `notice_fix` value for the stale-placeholder drift notice.
pub const NOTICE_FIX_RMDIR_STALE_DIR: &str = "rmdir_stale_dir";

/// Errno a twin returns when it refuses to issue (or gives up on) a
/// mount because the daemon is flagged wedged and policy says the user
/// must restart it. `EDEADLK` is never produced by NetFS itself, so
/// callers can key on it. Retryable: the next attempt checks the flag
/// again and proceeds once the daemon's pid has changed.
pub const EWEDGED: i32 = libc::EDEADLK;

// ── Tunables ─────────────────────────────────────────────────────────

/// A request still pending this long after issue gets the daemon
/// sampled. Healthy mounts finish in ~1s on a LAN, a few seconds over
/// VPN.
pub const OBSERVE_AFTER: Duration = Duration::from_secs(3);
/// Interval between samples while a request stays pending and nothing
/// suspicious has been seen.
pub const OBSERVE_EVERY: Duration = Duration::from_secs(5);
/// Gap before the confirming sample once a parked thread is seen. A
/// healthy lookup takes milliseconds on a LAN and well under a second
/// over a VPN, so a thread seen in it twice this far apart is parked.
/// (The fingerprint cannot be tied to one request: a parked thread
/// blocks every NetFS mount on the machine, so whichever request is
/// pending pays for the restart — the right remedy regardless.)
pub const CONFIRM_GAP: Duration = Duration::from_secs(3);
/// Longest a request holds the gate without a verdict. Session setup
/// (where the collision lives) either completes or parks well inside
/// this; past it the request is in something else — the sign-in
/// dialog, a slow server — and other shares may proceed.
pub const GATE_HOLD_CAP: Duration = Duration::from_secs(20);
/// Longest a waiter sits behind the gate before proceeding ungated.
/// Longer than the hold cap so a waiter normally gets its turn.
pub const GATE_WAIT: Duration = Duration::from_secs(30);
/// `flock(LOCK_NB)` retry interval while waiting for the gate.
const GATE_POLL: Duration = Duration::from_millis(50);
/// One automatic daemon restart per this interval; a wedge inside it
/// goes to the user instead.
pub const AUTO_RESTART_COOLDOWN: Duration = Duration::from_secs(10 * 60);
/// After a restart, how long to let the client library's replay of the
/// condemned request pass through the new daemon before issuing a
/// fresh one. Seen live 2026-11-06: the library replays the request
/// AND its cancel 1ms apart; the new daemon opens a session, closes it
/// (~250ms) and replies "cancelled" — a reply that never reaches the
/// original callback. The replay is therefore useless but its
/// short-lived session would collide with a fresh request issued at
/// the same instant, so the fresh one waits this long (gate still
/// held).
pub const RESTART_SETTLE: Duration = Duration::from_millis(1500);
/// How long `restart_netauth_agent` waits for SIGTERM to land before
/// escalating to SIGKILL.
const TERM_GRACE: Duration = Duration::from_secs(3);
/// `daemon_wedged` re-checks the daemon pid at most this often.
const WEDGE_RECHECK: Duration = Duration::from_secs(2);
/// First per-host hold after a host-level NetFS failure; doubles per
/// consecutive failure. Matches the GUI's retry heartbeat so the first
/// retry is not delayed by the hold at all.
pub const HOST_HOLD_BASE: Duration = Duration::from_secs(30);
/// Longest per-host hold: one NetFS probe per server per this interval
/// during a long outage, from whichever process gets there first.
pub const HOST_HOLD_CAP: Duration = Duration::from_secs(5 * 60);
/// A hold that expired longer ago than this starts the doubling over.
const HOST_HOLD_DECAY: Duration = Duration::from_secs(10 * 60);

/// Call-stack frames that identify a thread parked in the SMB plugin's
/// account lookup. Any one of them in a thread's stack counts.
pub const PARKED_FRAMES: &[&str] = &["LsarGetUserName", "GetNetworkAccountSID", "setNetworkAccountSID"];

// ── Daemon observation ───────────────────────────────────────────────

/// Pid of this user's NetAuthSysAgent, if running.
pub fn netauth_agent_pid() -> Option<u32> {
    let uid = unsafe { libc::getuid() };
    let out = std::process::Command::new("/usr/bin/pgrep")
        .args(["-x", "-u", &uid.to_string(), "NetAuthSysAgent"])
        .output()
        .ok()?;
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .find_map(|l| l.trim().parse::<u32>().ok())
}

/// Thread ids (`Thread_NNN`) whose sampled stack contains one of
/// [`PARKED_FRAMES`]. Parses `sample(1)` call-graph output.
pub fn parked_threads_in(sample_text: &str) -> BTreeSet<String> {
    let mut parked = BTreeSet::new();
    let mut current: Option<String> = None;
    for line in sample_text.lines() {
        let t = line.trim_start();
        // Thread headers look like "1 Thread_1227358" or
        // "1 Thread_1227273   DispatchQueue_1: ...". Frame lines start
        // with "+".
        if let Some(rest) = t.split_whitespace().nth(1) {
            if rest.starts_with("Thread_") && !t.starts_with('+') {
                current = Some(rest.to_string());
                continue;
            }
        }
        if t.starts_with("Binary Images") || t.starts_with("Total number in stack") {
            break;
        }
        if let Some(tid) = &current {
            if PARKED_FRAMES.iter().any(|f| t.contains(f)) {
                parked.insert(tid.clone());
            }
        }
    }
    parked
}

/// One look at the daemon.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Observation {
    /// No NetAuthSysAgent for this user — nothing can be parked.
    NotRunning,
    /// Sampled; these threads sit in the account lookup (may be empty).
    Sampled { pid: u32, parked: BTreeSet<String> },
    /// `sample` could not run or returned nothing usable.
    Unavailable { pid: u32, reason: String },
}

/// Sample the daemon once (about 0.2s wall clock).
pub fn observe_daemon() -> Observation {
    let Some(pid) = netauth_agent_pid() else {
        return Observation::NotRunning;
    };
    let out = std::process::Command::new("/usr/bin/sample")
        // `-file /dev/stdout`: without it sample(1) ALSO writes a copy
        // of every report to /tmp/<name>_<date>.sample.txt.
        .args([&pid.to_string(), "0.2", "-mayDie", "-file", "/dev/stdout"])
        .stdin(std::process::Stdio::null())
        .output();
    match out {
        Ok(o) if o.status.success() && !o.stdout.is_empty() => {
            let text = String::from_utf8_lossy(&o.stdout);
            if !text.contains("Thread_") {
                return Observation::Unavailable {
                    pid,
                    reason: format!("sample produced no call graph ({} bytes)", o.stdout.len()),
                };
            }
            Observation::Sampled { pid, parked: parked_threads_in(&text) }
        }
        Ok(o) => Observation::Unavailable {
            pid,
            reason: format!(
                "sample exited {} ({})",
                o.status,
                String::from_utf8_lossy(&o.stderr).trim()
            ),
        },
        Err(e) => Observation::Unavailable { pid, reason: format!("cannot run /usr/bin/sample: {}", e) },
    }
}

// ── Wedge flag ───────────────────────────────────────────────────────

struct WedgeInfo {
    /// Daemon pid when the wedge was condemned (None when pgrep couldn't
    /// tell). A different pid later means the parked threads died with
    /// the old process.
    pid: Option<u32>,
    /// Host of the condemned request. A completed request to this host
    /// clears the flag.
    host: String,
    checked_at: Instant,
}

static WEDGE: Mutex<Option<WedgeInfo>> = Mutex::new(None);

fn wedge() -> MutexGuard<'static, Option<WedgeInfo>> {
    WEDGE.lock().unwrap_or_else(|e| e.into_inner())
}

/// Host component of an `smb://[user@]host[:port]/share` URL.
pub fn smb_url_host(smb_url: &str) -> Option<&str> {
    let authority = smb_url.strip_prefix("smb://")?.split('/').next()?;
    let hostport = authority.rsplit('@').next()?;
    let host = hostport.split(':').next()?;
    (!host.is_empty()).then_some(host)
}

/// Flag the daemon as wedged for `smb_url`'s host.
pub fn note_wedged(smb_url: &str, pid: Option<u32>) {
    let host = smb_url_host(smb_url).unwrap_or(smb_url).to_string();
    log::warn!(
        "[netfs] NetAuthSysAgent ({}) is wedged on {} — every NetFS mount on this Mac (Finder included) hangs until it is restarted",
        pid.map(|p| format!("pid {}", p)).unwrap_or_else(|| "pid unknown".into()),
        host
    );
    *wedge() = Some(WedgeInfo { pid, host, checked_at: Instant::now() });
}

/// A terminal callback for `smb_url` arrived: the daemon answers for
/// that host again. Clears the flag if it was for the same host.
pub fn note_daemon_responsive(smb_url: &str) {
    let host = smb_url_host(smb_url).unwrap_or(smb_url);
    let mut w = wedge();
    if w.as_ref().is_some_and(|i| i.host == host) {
        log::info!("[netfs] NetAuthSysAgent answered for {} — clearing the wedged flag", host);
        *w = None;
    }
}

/// Is the daemon currently flagged wedged? Re-checks the pid (cheap
/// pgrep, rate-limited) so a restart by anyone clears the flag.
pub fn daemon_wedged() -> bool {
    let mut w = wedge();
    let Some(info) = w.as_mut() else {
        return false;
    };
    if info.checked_at.elapsed() < WEDGE_RECHECK {
        return true;
    }
    info.checked_at = Instant::now();
    match (info.pid, netauth_agent_pid()) {
        (Some(old), Some(now)) if old == now => true,
        (None, _) => true,
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

// ── Restart policy ───────────────────────────────────────────────────

static LAST_AUTO_RESTART: Mutex<Option<Instant>> = Mutex::new(None);

fn last_auto_restart() -> MutexGuard<'static, Option<Instant>> {
    LAST_AUTO_RESTART.lock().unwrap_or_else(|e| e.into_inner())
}

/// May the guard restart the daemon on its own right now? True when no
/// automatic restart happened within [`AUTO_RESTART_COOLDOWN`] — by
/// this process (in-memory) or by any UFB process (the mtime of a
/// stamp file in the state dir, so the GUI and the agent share one
/// cooldown). Claims the slot when it returns true.
pub fn claim_auto_restart() -> bool {
    let stamp = ufb_state_dir().map(|d| d.join("netfs-restart.stamp"));
    claim_auto_restart_at(Instant::now(), stamp.as_deref())
}

fn claim_auto_restart_at(now: Instant, stamp: Option<&std::path::Path>) -> bool {
    let mut last = last_auto_restart();
    if let Some(t) = *last {
        if now.duration_since(t) < AUTO_RESTART_COOLDOWN {
            return false;
        }
    }
    if let Some(stamp) = stamp {
        if let Ok(mtime) = std::fs::metadata(stamp).and_then(|m| m.modified()) {
            if let Ok(age) = std::time::SystemTime::now().duration_since(mtime) {
                if age < AUTO_RESTART_COOLDOWN {
                    log::info!(
                        "[netfs] another UFB process restarted NetAuthSysAgent {}s ago — not restarting again",
                        age.as_secs()
                    );
                    *last = Some(now);
                    return false;
                }
            }
        }
        if let Some(dir) = stamp.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        if let Err(e) = std::fs::write(stamp, b"") {
            log::warn!("[netfs] cannot write the restart stamp {}: {}", stamp.display(), e);
        }
    }
    *last = Some(now);
    true
}

/// Give the auto-restart slot back: the claim was made but no restart
/// happened (the kill failed), so the cooldown must not count it.
fn unclaim_auto_restart() {
    *last_auto_restart() = None;
    if let Some(stamp) = ufb_state_dir().map(|d| d.join("netfs-restart.stamp")) {
        let _ = std::fs::remove_file(stamp);
    }
}

/// Restart the current user's NetAuthSysAgent: SIGTERM, then SIGKILL
/// after a grace period. Returns the pid that was stopped (None when
/// the daemon wasn't running, which already means "not wedged").
/// launchd respawns it on the next NetFS request. Every process with a
/// request parked inside the old daemon has that request REPLAYED by
/// its NetFS client library against the new one.
///
/// Blocking (up to a few seconds) — run on a worker thread.
pub fn restart_netauth_agent(reason: &str) -> Result<Option<u32>, String> {
    let Some(pid) = netauth_agent_pid() else {
        log::info!("[netfs] restart_netauth_agent: NetAuthSysAgent is not running — nothing to restart");
        *wedge() = None;
        return Ok(None);
    };
    log::warn!("[netfs] restarting NetAuthSysAgent (pid {}) — {}", pid, reason);
    if unsafe { libc::kill(pid as libc::pid_t, libc::SIGTERM) } != 0 {
        return Err(format!("SIGTERM to NetAuthSysAgent (pid {}) failed: {}", pid, std::io::Error::last_os_error()));
    }
    let deadline = Instant::now() + TERM_GRACE;
    while Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(100));
        if netauth_agent_pid() != Some(pid) {
            *wedge() = None;
            clear_host_holds();
            return Ok(Some(pid));
        }
    }
    log::warn!("[netfs] NetAuthSysAgent (pid {}) ignored SIGTERM for {}s — sending SIGKILL", pid, TERM_GRACE.as_secs());
    if unsafe { libc::kill(pid as libc::pid_t, libc::SIGKILL) } != 0 {
        return Err(format!("SIGKILL to NetAuthSysAgent (pid {}) failed: {}", pid, std::io::Error::last_os_error()));
    }
    std::thread::sleep(Duration::from_millis(200));
    *wedge() = None;
    clear_host_holds();
    Ok(Some(pid))
}

// ── UFB state dir ────────────────────────────────────────────────────

/// `~/.local/share/ufb` — the directory the gate, the per-host holds
/// and the restart stamp live in. Shared by the GUI and the agent, which
/// is what makes each of them cross-process.
fn ufb_state_dir() -> Option<std::path::PathBuf> {
    Some(std::path::PathBuf::from(std::env::var_os("HOME")?).join(".local/share/ufb"))
}

fn unix_ms(t: std::time::SystemTime) -> u64 {
    t.duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

// ── Per-host hold ────────────────────────────────────────────────────

/// A host the guard is not issuing NetFS requests to right now.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HostHold {
    pub host: String,
    /// Errno of the failure that set the hold; the twin returns it so
    /// the row shows the real reason, not a synthetic one.
    pub status: i32,
    /// Consecutive host-level failures so far.
    pub failures: u32,
    pub remaining: Duration,
}

/// Statuses that describe the server, not one share: these put the
/// host on hold. Everything else (EAUTH, ENOENT, EACCES, EBUSY,
/// ECANCELED, …) is about the share or the user and must not stop the
/// other shares from trying. [`EWEDGED`] is deliberately NOT here: it
/// describes the daemon, which the wedge flag already stops issuing
/// to, and a hold fed by every refused issue would outlive the
/// daemon's restart by minutes.
pub fn is_host_failure(status: i32) -> bool {
    matches!(
        status,
        libc::ETIMEDOUT
            | libc::EHOSTUNREACH
            | libc::ENETUNREACH
            | libc::EHOSTDOWN
            | libc::ENETDOWN
            | libc::ECONNREFUSED
            | libc::ECONNRESET
    )
}

fn host_file_name(host: &str) -> String {
    host.chars().map(|c| if c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_') { c } else { '_' }).collect()
}

fn host_hold_path_in(dir: &std::path::Path, host: &str) -> std::path::PathBuf {
    dir.join("netfs-hosts").join(host_file_name(host))
}

/// `(until_unix_ms, failures, status)` as stored in the hold file.
fn read_host_record(path: &std::path::Path) -> Option<(u64, u32, i32)> {
    let text = std::fs::read_to_string(path).ok()?;
    let mut it = text.split_whitespace();
    let until = it.next()?.parse().ok()?;
    let failures = it.next()?.parse().ok()?;
    let status = it.next()?.parse().ok()?;
    Some((until, failures, status))
}

fn write_host_record(path: &std::path::Path, until: u64, failures: u32, status: i32) -> std::io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    // with_file_name, not with_extension: "nas.local" and "nas.lan"
    // must not share a temp file.
    let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("host");
    let tmp = path.with_file_name(format!("{}.tmp{}", name, std::process::id()));
    std::fs::write(&tmp, format!("{} {} {}\n", until, failures, status))?;
    std::fs::rename(&tmp, path)
}

/// Drop every per-host hold. Called after a daemon restart: each hold
/// was a verdict on the old daemon's behaviour and is stale now.
pub fn clear_host_holds() {
    let Some(dir) = ufb_state_dir() else {
        return;
    };
    let Ok(entries) = std::fs::read_dir(dir.join("netfs-hosts")) else {
        return;
    };
    let mut n = 0;
    for e in entries.flatten() {
        if std::fs::remove_file(e.path()).is_ok() {
            n += 1;
        }
    }
    if n > 0 {
        log::info!("[netfs] cleared {} per-host hold(s) after the NetAuthSysAgent restart", n);
    }
}

/// Is `smb_url`'s host on hold? Cheap (one small file read); the twin
/// calls it before taking the gate and returns `status` without
/// issuing when it is `Some`. Never holds when the state dir is
/// unavailable.
pub fn host_hold(smb_url: &str) -> Option<HostHold> {
    host_hold_in(smb_url, ufb_state_dir()?.as_path(), std::time::SystemTime::now())
}

fn host_hold_in(smb_url: &str, dir: &std::path::Path, now: std::time::SystemTime) -> Option<HostHold> {
    let host = smb_url_host(smb_url)?;
    let (until, failures, status) = read_host_record(&host_hold_path_in(dir, host))?;
    let remaining = until.checked_sub(unix_ms(now)).filter(|ms| *ms > 0)?;
    Some(HostHold { host: host.to_string(), status, failures, remaining: Duration::from_millis(remaining) })
}

/// Record how a NetFS attempt for `smb_url` ended. Success (or any
/// share-level error, which proves the host answered) clears the hold;
/// a host-level failure sets or extends it.
pub fn note_host_outcome<T>(smb_url: &str, result: &Result<T, i32>) {
    let Some(dir) = ufb_state_dir() else {
        return;
    };
    note_host_outcome_in(smb_url, result.as_ref().err().copied(), &dir, std::time::SystemTime::now());
}

fn note_host_outcome_in(smb_url: &str, err: Option<i32>, dir: &std::path::Path, now: std::time::SystemTime) {
    let Some(host) = smb_url_host(smb_url) else {
        return;
    };
    let path = host_hold_path_in(dir, host);
    if err == Some(EWEDGED) {
        // A refused issue says nothing about the host either way.
        return;
    }
    let status = match err {
        Some(st) if is_host_failure(st) => st,
        _ => {
            if path.exists() {
                if let Some((_, failures, _)) = read_host_record(&path) {
                    log::info!("[netfs] {} answers again after {} host-level failure(s) — hold cleared", host, failures);
                }
                let _ = std::fs::remove_file(&path);
            }
            return;
        }
    };
    let now_ms = unix_ms(now);
    let failures = match read_host_record(&path) {
        Some((until, n, _)) if now_ms.saturating_sub(until) < HOST_HOLD_DECAY.as_millis() as u64 => n.saturating_add(1),
        _ => 1,
    };
    let hold = HOST_HOLD_BASE
        .checked_mul(1u32 << (failures - 1).min(16))
        .unwrap_or(HOST_HOLD_CAP)
        .min(HOST_HOLD_CAP);
    match write_host_record(&path, now_ms + hold.as_millis() as u64, failures, status) {
        Ok(()) => log::warn!(
            "[netfs] {} failed at the host level ({}; failure #{}) — no NetFS request to this server for {}s, from any UFB process",
            host,
            status_label(status).unwrap_or("see status"),
            failures,
            hold.as_secs()
        ),
        Err(e) => log::warn!("[netfs] cannot record the hold for {} ({}): {}", host, path.display(), e),
    }
}

// ── Issue gate ───────────────────────────────────────────────────────

fn gate_path() -> Option<std::path::PathBuf> {
    gate_path_in(ufb_state_dir()?)
}

fn gate_path_in(state_dir: std::path::PathBuf) -> Option<std::path::PathBuf> {
    Some(state_dir.join("netfs-issue.lock"))
}

/// The machine-wide issue gate as held by one mount attempt. `file` is
/// `Some` only while the flock is held; dropping (or `release`) unlocks.
/// Never blocks anything on error: lock-file trouble (no HOME,
/// unwritable dir) and a wait past [`GATE_WAIT`] both proceed ungated
/// with a warning — the gate must never turn a slow mount into a lost
/// one.
pub struct IssueGate {
    file: Option<File>,
    smb_url: String,
    acquired_at: Instant,
}

impl IssueGate {
    /// Wait for the gate (up to [`GATE_WAIT`]) and take it.
    pub fn acquire(smb_url: &str) -> IssueGate {
        Self::acquire_at(smb_url, gate_path())
    }

    pub fn ungated(smb_url: &str) -> IssueGate {
        IssueGate { file: None, smb_url: smb_url.to_string(), acquired_at: Instant::now() }
    }

    fn acquire_at(smb_url: &str, path: Option<std::path::PathBuf>) -> IssueGate {
        use std::os::fd::AsRawFd;
        let Some(path) = path else {
            log::warn!("[netfs] issue gate: no HOME — mounting {} ungated", smb_url);
            return Self::ungated(smb_url);
        };
        if let Some(dir) = path.parent() {
            if let Err(e) = std::fs::create_dir_all(dir) {
                log::warn!("[netfs] issue gate: cannot create {} ({}) — mounting {} ungated", dir.display(), e, smb_url);
                return Self::ungated(smb_url);
            }
        }
        let file = match std::fs::OpenOptions::new().read(true).write(true).create(true).truncate(false).open(&path) {
            Ok(f) => f,
            Err(e) => {
                log::warn!("[netfs] issue gate: cannot open {} ({}) — mounting {} ungated", path.display(), e, smb_url);
                return Self::ungated(smb_url);
            }
        };
        let started = Instant::now();
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
                return IssueGate { file: Some(file), smb_url: smb_url.to_string(), acquired_at: Instant::now() };
            }
            let err = std::io::Error::last_os_error();
            let busy = matches!(err.raw_os_error(), Some(libc::EWOULDBLOCK) | Some(libc::EINTR));
            if !busy {
                log::warn!("[netfs] issue gate: flock on {} failed ({}) — mounting {} ungated", path.display(), err, smb_url);
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
    pub fn release(&mut self) {
        use std::os::fd::AsRawFd;
        if let Some(file) = self.file.take() {
            unsafe {
                libc::flock(file.as_raw_fd(), libc::LOCK_UN);
            }
            drop(file);
        }
    }

    pub fn is_held(&self) -> bool {
        self.file.is_some()
    }

    pub fn smb_url(&self) -> &str {
        &self.smb_url
    }
}

impl Drop for IssueGate {
    fn drop(&mut self) {
        self.release();
    }
}

// ── The observed wait ────────────────────────────────────────────────

/// Outcome of [`wait_observed`].
#[derive(Debug)]
pub enum Verdict<T> {
    /// The callback arrived.
    Done(T),
    /// The daemon was seen parked in the account lookup across two
    /// samples while this request sat pending. `pid` is the daemon to
    /// restart. The gate is STILL HELD on this verdict (re-acquired if
    /// the hold cap had released it): nothing else
    /// may be issued until the twin has cancelled, applied
    /// [`recover`] and let the replay settle — a request issued into
    /// the wedged daemon in that window would only be replayed
    /// alongside this one and re-park the new daemon (seen live
    /// 2026-11-06). The twin releases it (or drops the gate) when done.
    Wedged { pid: u32, parked: BTreeSet<String> },
    /// `deadline` passed with no callback and no fingerprint.
    Deadline,
}

/// Wait for the mount callback on `rx`, sampling the daemon while the
/// request stays pending (see the module notes). The gate is held until
/// a verdict or [`GATE_HOLD_CAP`], whichever is first, and released on
/// every exit path except [`Verdict::Wedged`], where the twin keeps it
/// through recovery.
pub fn wait_observed<T>(rx: &Receiver<T>, gate: &mut IssueGate, deadline: Duration) -> Verdict<T> {
    let started = Instant::now();
    let smb_url = gate.smb_url().to_string();
    let mut next_sample = started + OBSERVE_AFTER;
    let mut suspect: Option<(Instant, BTreeSet<String>)> = None;
    let mut sampling_unavailable = false;
    loop {
        let now = Instant::now();
        let hard = started + deadline;
        if now >= hard {
            gate.release();
            return Verdict::Deadline;
        }
        let mut wake = hard;
        if !sampling_unavailable {
            wake = wake.min(next_sample);
        }
        if gate.is_held() {
            wake = wake.min(gate.acquired_at + GATE_HOLD_CAP);
        }
        let slice = wake.saturating_duration_since(now);
        match rx.recv_timeout(slice) {
            Ok(done) => {
                gate.release();
                return Verdict::Done(done);
            }
            Err(RecvTimeoutError::Disconnected) => {
                // The sender is gone without a value: treat as the
                // deadline — nothing more can arrive.
                gate.release();
                return Verdict::Deadline;
            }
            Err(RecvTimeoutError::Timeout) => {}
        }
        let now = Instant::now();
        if gate.is_held() && now >= gate.acquired_at + GATE_HOLD_CAP {
            log::info!(
                "[netfs] issue gate: {} still pending after {}s with no wedge fingerprint — releasing the gate",
                smb_url,
                GATE_HOLD_CAP.as_secs()
            );
            gate.release();
        }
        if sampling_unavailable || now < next_sample {
            continue;
        }
        match observe_daemon() {
            Observation::NotRunning => {
                // Nothing to be parked in; keep waiting for the
                // callback (launchd may be respawning it).
                next_sample = now + OBSERVE_EVERY;
            }
            Observation::Unavailable { pid, reason } => {
                log::warn!(
                    "[netfs] cannot observe NetAuthSysAgent (pid {}): {} — falling back to the {}s deadline for {}",
                    pid,
                    reason,
                    deadline.as_secs(),
                    smb_url
                );
                sampling_unavailable = true;
            }
            Observation::Sampled { pid, parked } if parked.is_empty() => {
                if suspect.is_some() {
                    log::info!("[netfs] NetAuthSysAgent (pid {}) no longer parked — {} still pending", pid, smb_url);
                }
                suspect = None;
                next_sample = now + OBSERVE_EVERY;
            }
            Observation::Sampled { pid, parked } => match &suspect {
                Some((seen_at, earlier)) if earlier.intersection(&parked).next().is_some() => {
                    let still: BTreeSet<String> = earlier.intersection(&parked).cloned().collect();
                    log::warn!(
                        "[netfs] NetAuthSysAgent (pid {}) parked in the account lookup for {:.1}s ({:?}) while {} has waited {:.1}s — condemning the request",
                        pid,
                        seen_at.elapsed().as_secs_f64(),
                        still,
                        smb_url,
                        started.elapsed().as_secs_f64()
                    );
                    // Gate deliberately kept: see `Verdict::Wedged`. If
                    // the hold cap already released it, take it back so
                    // recovery still runs with nothing else issuing.
                    if !gate.is_held() {
                        log::info!("[netfs] issue gate: re-acquiring for the recovery of {}", smb_url);
                        *gate = IssueGate::acquire(&smb_url);
                    }
                    return Verdict::Wedged { pid, parked: still };
                }
                _ => {
                    log::info!(
                        "[netfs] NetAuthSysAgent (pid {}) seen in the account lookup ({:?}) while {} is pending — confirming in {}s",
                        pid,
                        parked,
                        smb_url,
                        CONFIRM_GAP.as_secs()
                    );
                    suspect = Some((now, parked));
                    next_sample = now + CONFIRM_GAP;
                }
            },
        }
    }
}

/// What a twin should do with a request that was condemned and whose
/// cancel went unanswered.
#[derive(Debug, PartialEq, Eq)]
pub enum Recovery {
    /// The daemon was restarted: leak the condemned request, sleep
    /// [`RESTART_SETTLE`] with the gate still held, then issue a fresh
    /// request once (a second condemnation goes to the deadline path).
    Restarted { old_pid: Option<u32> },
    /// Policy declined (restart within the cooldown, or the kill
    /// failed): leave the flag set, leak the request, return
    /// [`EWEDGED`] so the row shows the pill.
    HandToUser,
}

/// Apply the restart policy to a condemned request whose cancel the
/// daemon did not acknowledge. Flags the wedge first so concurrent
/// attempts in this process refuse to issue; on a restart the flag
/// clears as soon as the pid changes.
pub fn recover(smb_url: &str, pid: Option<u32>) -> Recovery {
    note_wedged(smb_url, pid);
    if !claim_auto_restart() {
        log::warn!(
            "[netfs] NetAuthSysAgent wedged again within {} min of the last automatic restart — handing {} to the user",
            AUTO_RESTART_COOLDOWN.as_secs() / 60,
            smb_url
        );
        return Recovery::HandToUser;
    }
    match restart_netauth_agent("automatic: a mount request was condemned as parked") {
        Ok(old_pid) => Recovery::Restarted { old_pid },
        Err(e) => {
            log::warn!("[netfs] automatic NetAuthSysAgent restart failed: {} — handing {} to the user", e, smb_url);
            unclaim_auto_restart();
            Recovery::HandToUser
        }
    }
}

/// Label for [`EWEDGED`] and friends in log/error text; twins fold it
/// into their `status_message`.
pub fn status_label(status: i32) -> Option<&'static str> {
    (status == EWEDGED).then_some("mount service (NetAuthSysAgent) wedged — restart pending")
}

#[cfg(test)]
mod tests {
    use super::*;

    const PARKED: &str = include_str!("../tests/fixtures/netauth_parked.txt");

    #[test]
    fn parser_finds_the_parked_thread_in_the_fixture() {
        let parked = parked_threads_in(PARKED);
        assert_eq!(parked.len(), 1, "{:?}", parked);
        assert!(parked.contains("Thread_1227358"));
    }

    #[test]
    fn parser_ignores_a_healthy_sample() {
        let healthy: String = PARKED
            .lines()
            .filter(|l| !PARKED_FRAMES.iter().any(|f| l.contains(f)))
            .map(|l| format!("{}\n", l))
            .collect();
        assert!(parked_threads_in(&healthy).is_empty());
        assert!(parked_threads_in("").is_empty());
    }

    #[test]
    fn smb_url_host_parses() {
        assert_eq!(smb_url_host("smb://192.168.40.100/Jobs_Live"), Some("192.168.40.100"));
        assert_eq!(smb_url_host("smb://me%20x@nas.local:445/share/deep"), Some("nas.local"));
        assert_eq!(smb_url_host("nfs://x/y"), None);
    }

    #[test]
    fn wedge_flag_round_trip() {
        *wedge() = None;
        assert!(!daemon_wedged());
        note_wedged("smb://h1/s", Some(1));
        assert!(daemon_wedged());
        note_daemon_responsive("smb://h2/s");
        assert!(daemon_wedged(), "other host must not clear it");
        note_daemon_responsive("smb://h1/t");
        assert!(!daemon_wedged());
    }

    #[test]
    fn auto_restart_is_rate_limited() {
        *last_auto_restart() = None;
        let t0 = Instant::now();
        assert!(claim_auto_restart_at(t0, None));
        assert!(!claim_auto_restart_at(t0 + Duration::from_secs(60), None));
        assert!(claim_auto_restart_at(t0 + AUTO_RESTART_COOLDOWN + Duration::from_secs(1), None));
        *last_auto_restart() = None;
    }

    #[test]
    fn auto_restart_cooldown_is_shared_through_the_stamp() {
        let dir = std::env::temp_dir().join(format!("netfs-guard-stamp-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let stamp = dir.join("netfs-restart.stamp");
        *last_auto_restart() = None;
        assert!(claim_auto_restart_at(Instant::now(), Some(&stamp)));
        assert!(stamp.exists());
        // A second process (fresh in-memory state) finds the stamp.
        *last_auto_restart() = None;
        assert!(!claim_auto_restart_at(Instant::now(), Some(&stamp)));
        *last_auto_restart() = None;
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn host_hold_doubles_caps_and_clears() {
        use std::time::SystemTime;
        let dir = std::env::temp_dir().join(format!("netfs-guard-hosts-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let t0 = SystemTime::UNIX_EPOCH + Duration::from_secs(1_800_000_000);
        let a = "smb://nas.local/A";
        let b = "smb://nas.local/B";
        assert!(host_hold_in(a, &dir, t0).is_none());

        // Share-level errors never hold the host.
        note_host_outcome_in(a, Some(libc::EACCES), &dir, t0);
        assert!(host_hold_in(b, &dir, t0).is_none());

        note_host_outcome_in(a, Some(libc::ETIMEDOUT), &dir, t0);
        let h = host_hold_in(b, &dir, t0 + Duration::from_secs(1)).expect("sibling share is held");
        assert_eq!((h.failures, h.status), (1, libc::ETIMEDOUT));
        assert!(h.remaining <= HOST_HOLD_BASE && h.remaining > HOST_HOLD_BASE - Duration::from_secs(2));
        assert!(host_hold_in(b, &dir, t0 + HOST_HOLD_BASE + Duration::from_secs(1)).is_none(), "hold expires");

        // Second failure right after expiry doubles; keeps doubling to the cap.
        let mut t = t0 + HOST_HOLD_BASE + Duration::from_secs(1);
        note_host_outcome_in(b, Some(libc::EHOSTUNREACH), &dir, t);
        note_host_outcome_in(b, Some(EWEDGED), &dir, t);
        assert_eq!(host_hold_in(a, &dir, t).unwrap().failures, 2, "EWEDGED must not feed the hold");
        let h = host_hold_in(a, &dir, t).unwrap();
        assert_eq!(h.failures, 2);
        assert!(h.remaining > HOST_HOLD_BASE);
        for _ in 0..6 {
            t += HOST_HOLD_CAP + Duration::from_secs(1);
            note_host_outcome_in(a, Some(libc::ETIMEDOUT), &dir, t);
        }
        let h = host_hold_in(a, &dir, t).unwrap();
        assert!(h.remaining <= HOST_HOLD_CAP);
        assert_eq!(h.failures, 8);

        // Other hosts are independent; success clears.
        assert!(host_hold_in("smb://other/A", &dir, t).is_none());
        note_host_outcome_in(a, None, &dir, t);
        assert!(host_hold_in(b, &dir, t).is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn host_file_names_are_safe() {
        assert_eq!(host_file_name("nas.local"), "nas.local");
        assert_eq!(host_file_name("192.168.40.100"), "192.168.40.100");
        assert_eq!(host_file_name("../x/y"), ".._x_y");
    }

    #[test]
    fn gate_excludes_second_opener_and_releases() {
        use std::os::fd::AsRawFd;
        let dir = std::env::temp_dir().join(format!("netfs-guard-gate-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let path = gate_path_in(dir.clone()).unwrap();
        let mut g = IssueGate::acquire_at("smb://h/a", Some(path.clone()));
        assert!(g.is_held());
        let probe = std::fs::OpenOptions::new().read(true).write(true).open(&path).unwrap();
        let rc = unsafe { libc::flock(probe.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
        assert_eq!(rc, -1, "second opener must be excluded while held");
        g.release();
        assert!(!g.is_held());
        let rc = unsafe { libc::flock(probe.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
        assert_eq!(rc, 0, "released gate must be takeable");
        drop(probe);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn gate_without_home_is_ungated() {
        let g = IssueGate::acquire_at("smb://h/a", None);
        assert!(!g.is_held());
    }

    #[test]
    fn wait_observed_returns_done_and_releases() {
        let (tx, rx) = std::sync::mpsc::channel::<i32>();
        let mut g = IssueGate::acquire_at("smb://h/a", None);
        tx.send(7).unwrap();
        match wait_observed(&rx, &mut g, Duration::from_secs(5)) {
            Verdict::Done(7) => {}
            other => panic!("{:?}", other),
        }
        assert!(!g.is_held());
    }

    #[test]
    fn wait_observed_hits_deadline_when_sender_vanishes() {
        let (tx, rx) = std::sync::mpsc::channel::<i32>();
        drop(tx);
        let mut g = IssueGate::acquire_at("smb://h/a", None);
        assert!(matches!(wait_observed(&rx, &mut g, Duration::from_secs(5)), Verdict::Deadline));
    }
}
