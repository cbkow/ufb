//! GUI-owned plain-mount manager — plans/17 F1 (macOS) + slice B (Windows).
//!
//! The app mounts non-sync SMB shares itself — NetFS on macOS
//! (`ufb_core::macos_mounts`), persistent drive letters via WNet on
//! Windows (`ufb_core::windows_mounts`); the agent only hosts the sync
//! VFS. One tokio task per mount runs a deliberately tiny lifecycle:
//!
//!   Start(allow_ui) → mount → Mounted ── 30s heartbeat ──┐
//!        ▲                                               │ dead/foreign
//!        └── auto-remount (silent) ◄────────────────────┘
//!   Stop → guarded unmount → idle until next command
//!
//! State updates are emitted as `MountStateUpdateMsg` through the SAME
//! `MountEvents` forwarder the agent's IPC events use, so the states
//! map, live roots, tray, and sidebar don't know or care who owns a
//! mount. Auth failures surface as `auth_error` exactly like the
//! agent's; `allow_ui` rides user-initiated commands only, mirroring
//! the agent's one-shot UI permit.
//!
//! The task FSM below is OS-agnostic; everything platform-specific
//! lives in the small `sys` module (mount, unmount, ownership check,
//! drift notice, post-mount upkeep).

#![cfg(any(target_os = "macos", windows))]

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use tokio::sync::mpsc;
use ufb_core::events::MountEventsArc;
use ufb_core::mount_client::MountStateUpdateMsg;

use crate::runtime::shared_runtime;

#[derive(Debug, Clone, Copy)]
pub enum LocalCmd {
    Start { allow_ui: bool },
    Stop,
    Restart { allow_ui: bool },
}

/// Static per-mount facts the task needs.
#[derive(Debug, Clone)]
pub struct LocalMountSpec {
    pub id: String,
    pub nas_share_path: String,
    pub share_name: String,
    /// Windows: preferred drive letter from `mountDriveLetter` (first
    /// char), when configured. Ignored on macOS.
    pub drive_letter: Option<char>,
}

struct LocalMount {
    spec: LocalMountSpec,
    cmd_tx: mpsc::Sender<LocalCmd>,
    /// Flipped when the mount leaves config — the task exits at the
    /// next loop turn (its cmd channel also closes).
    retired: Arc<AtomicBool>,
}

fn registry() -> &'static Mutex<HashMap<String, LocalMount>> {
    static REG: OnceLock<Mutex<HashMap<String, LocalMount>>> = OnceLock::new();
    REG.get_or_init(|| Mutex::new(HashMap::new()))
}

/// True when `id` is currently GUI-owned (has a live local task).
pub fn is_local(id: &str) -> bool {
    registry().lock().map(|r| r.contains_key(id)).unwrap_or(false)
}

/// Record a first-assignment drive letter into the live registry spec
/// (Windows letter stability). Without this, the next `apply_config`
/// after the letter was persisted to mounts.json would see spec(None)
/// vs config(X) and needlessly retire + remount the task.
#[cfg(windows)]
pub fn note_assigned_letter(id: &str, letter: char) {
    if let Ok(mut reg) = registry().lock() {
        if let Some(m) = reg.get_mut(id) {
            if m.spec.drive_letter.is_none() {
                m.spec.drive_letter = Some(letter.to_ascii_uppercase());
            }
        }
    }
}

/// Route a lifecycle command to a GUI-owned mount. Returns false when
/// the id isn't locally managed (caller falls back to agent IPC).
pub fn send(id: &str, cmd: LocalCmd) -> bool {
    let tx = match registry().lock() {
        Ok(reg) => match reg.get(id) {
            Some(m) => m.cmd_tx.clone(),
            None => return false,
        },
        Err(_) => return false,
    };
    let _guard = shared_runtime().enter();
    tokio::spawn(async move {
        let _ = tx.send(cmd).await;
    });
    true
}

/// Reconcile the set of GUI-owned mounts with config: start tasks for
/// enabled non-sync non-unmanaged mounts, retire tasks whose mount
/// left that set. Idempotent; call at startup and after every config
/// save.
pub fn apply_config(specs: Vec<LocalMountSpec>, events: MountEventsArc) {
    let mut reg = match registry().lock() {
        Ok(r) => r,
        Err(_) => return,
    };

    let wanted: HashMap<String, LocalMountSpec> =
        specs.into_iter().map(|s| (s.id.clone(), s)).collect();

    // Retire removed/changed mounts. A changed nas_share_path (or
    // Windows drive-letter preference) retires + restarts the task so
    // it can't keep heartbeating the old share/letter.
    let stale: Vec<String> = reg
        .iter()
        .filter(|(id, m)| {
            wanted
                .get(*id)
                .map(|w| {
                    w.nas_share_path != m.spec.nas_share_path
                        || w.drive_letter != m.spec.drive_letter
                })
                .unwrap_or(true)
        })
        .map(|(id, _)| id.clone())
        .collect();
    for id in stale {
        if let Some(m) = reg.remove(&id) {
            log::info!("[local-mounts] retiring {}", id);
            m.retired.store(true, Ordering::SeqCst);
            // Nudge the task so it notices retirement promptly; a
            // closed channel (drop below) also wakes it.
            let tx = m.cmd_tx.clone();
            let _guard = shared_runtime().enter();
            tokio::spawn(async move {
                let _ = tx.send(LocalCmd::Stop).await;
            });
        }
    }

    // Start new ones.
    for (id, spec) in wanted {
        if reg.contains_key(&id) {
            continue;
        }
        let (cmd_tx, cmd_rx) = mpsc::channel::<LocalCmd>(8);
        let retired = Arc::new(AtomicBool::new(false));
        log::info!("[local-mounts] starting {}", id);
        {
            let spec = spec.clone();
            let retired = retired.clone();
            let events = events.clone();
            let _guard = shared_runtime().enter();
            tokio::spawn(run_mount(spec, cmd_rx, retired, events));
        }
        reg.insert(
            id,
            LocalMount {
                spec,
                cmd_tx,
                retired,
            },
        );
    }
}

fn emit(events: &MountEventsArc, spec: &LocalMountSpec, state: &str, detail: String,
        mounted_at: Option<String>, notice: Option<(String, bool)>) {
    let (notice, notice_fixable) = match notice {
        Some((text, fixable)) => (Some(text), if fixable { Some(true) } else { None }),
        None => (None, None),
    };
    events.state_update(&MountStateUpdateMsg {
        mount_id: spec.id.clone(),
        state: state.to_string(),
        state_detail: detail,
        sync_state: None,
        sync_state_detail: None,
        mounted_at,
        notice,
        notice_fixable,
    });
}

/// The platform seam. Everything below `sys` is OS-agnostic.
mod sys {
    use super::LocalMountSpec;

    /// Auth-vs-other split shared by both backends — routes to the
    /// `auth_error` pill vs the generic error state.
    pub enum MountError {
        Auth(String),
        Other(String),
    }

    #[cfg(target_os = "macos")]
    mod imp {
        use super::*;
        use ufb_core::macos_mounts::{self, MacosMountError};

        pub fn mount(spec: &LocalMountSpec, allow_ui: bool) -> Result<String, MountError> {
            macos_mounts::macos_smb_mount(&spec.nas_share_path, allow_ui).map_err(|e| match e {
                MacosMountError::Auth(m) => MountError::Auth(m),
                MacosMountError::Other(m) => MountError::Other(m),
            })
        }

        /// `_forget` is a Windows concept (persistent profile entry);
        /// macOS unmount has nothing to forget.
        pub fn unmount(path: &str, _forget: bool) {
            let _ = macos_mounts::macos_smb_unmount(path);
        }

        pub fn is_ours(path: &str, nas_share_path: &str) -> Option<bool> {
            macos_mounts::mount_at_path_is_ours(path, nas_share_path)
        }

        /// Drift notice — same semantics as the agent's `drift_notice`:
        /// Some((text, fixable)); fixable=true when a stale leftover
        /// directory (empty, unmounted) squats the expected name, so
        /// the row can offer the admin-privileged removal.
        pub fn drift_notice(spec: &LocalMountSpec, mounted_path: &str) -> Option<(String, bool)> {
            let expected = spec
                .nas_share_path
                .trim_end_matches('\\')
                .rsplit('\\')
                .next()
                .unwrap_or("");
            let actual = mounted_path
                .trim_end_matches('/')
                .rsplit('/')
                .next()
                .unwrap_or("");
            if expected.is_empty() || actual.is_empty() || actual.eq_ignore_ascii_case(expected)
            {
                return None;
            }
            let expected_path = format!("/Volumes/{}", expected);
            if macos_mounts::stale_dir_blocking(&expected_path) {
                return Some((
                    format!(
                        "Mounted as \"{}\" — a stale leftover folder is blocking /Volumes/{}",
                        actual, expected
                    ),
                    true,
                ));
            }
            Some((
                format!(
                    "Mounted as \"{}\" — /Volumes/{} was taken by another volume",
                    actual, expected
                ),
                false,
            ))
        }

        /// 1.0.7: the legacy `~/ufb/mounts/<share>` symlink farm is
        /// retired (identity resolution has been live since 1.0.5 —
        /// nothing reads the alias). Instead of maintaining the
        /// symlink, remove a stale one if present so upgraded
        /// machines converge on clean state. SYMLINKS ONLY — a real
        /// directory here is a sync-mount NFS mountpoint, never ours
        /// to touch.
        pub fn post_mount_upkeep(spec: &LocalMountSpec, _target: &str) {
            let Some(home) = std::env::var_os("HOME") else { return };
            let link = std::path::PathBuf::from(home)
                .join("ufb/mounts")
                .join(&spec.share_name);
            if link.is_symlink() {
                if std::fs::remove_file(&link).is_ok() {
                    log::info!(
                        "[local-mounts] retired legacy symlink {}",
                        link.display()
                    );
                }
            }
        }
    }

    #[cfg(windows)]
    mod imp {
        use super::*;
        use ufb_core::windows_mounts::{self, WindowsMountError};

        pub fn mount(spec: &LocalMountSpec, allow_ui: bool) -> Result<String, MountError> {
            windows_mounts::windows_smb_mount(&spec.nas_share_path, spec.drive_letter, allow_ui)
                .map_err(|e| match e {
                    WindowsMountError::Auth(m) => MountError::Auth(m),
                    WindowsMountError::Other(m) => MountError::Other(m),
                })
        }

        /// `forget` drops the persistent profile entry too — explicit
        /// user Stop must not resurrect at next logon; the transient
        /// unmount inside Restart/heartbeat keeps it.
        pub fn unmount(path: &str, forget: bool) {
            if let Err(e) = windows_mounts::windows_smb_unmount(path, forget) {
                log::debug!("[local-mounts] unmount {} failed: {}", path, e);
            }
        }

        pub fn is_ours(path: &str, nas_share_path: &str) -> Option<bool> {
            windows_mounts::mount_at_letter_is_ours(path, nas_share_path)
        }

        /// Drift = mounted at a different letter than configured
        /// (configured letter taken by a foreign mapping/volume).
        /// Never fixable — reclaiming a drive letter is a different
        /// (and riskier) operation than removing a stale directory.
        pub fn drift_notice(spec: &LocalMountSpec, mounted_path: &str) -> Option<(String, bool)> {
            let preferred = spec.drive_letter?.to_ascii_uppercase();
            let actual = mounted_path.chars().next()?.to_ascii_uppercase();
            if actual == preferred {
                return None;
            }
            Some((
                format!(
                    "Mounted at {}: — {}: was taken by another drive",
                    actual, preferred
                ),
                false,
            ))
        }

        /// No legacy path spelling to maintain on Windows — the
        /// `C:\Volumes\ufb` symlink farm deletes with slice B.
        pub fn post_mount_upkeep(_spec: &LocalMountSpec, _target: &str) {}
    }

    pub use imp::*;
}

/// Bounded liveness probe against the live mount path: mount-table
/// ownership first (a foreign volume at our path must read as dead),
/// then a read_dir on a worker thread with a timeout (dead SMB mounts
/// block read_dir indefinitely; the kernel's attr cache makes stat
/// lie). Mirrors the agent's heartbeat probe.
fn probe_alive(path: &str, nas_share_path: &str) -> bool {
    if sys::is_ours(path, nas_share_path) != Some(true) {
        return false;
    }
    let (tx, rx) = std::sync::mpsc::channel();
    let p = path.to_string();
    std::thread::spawn(move || {
        let ok = match std::fs::read_dir(&p) {
            Ok(mut rd) => !matches!(rd.next(), Some(Err(_))),
            Err(_) => false,
        };
        let _ = tx.send(ok);
    });
    matches!(rx.recv_timeout(std::time::Duration::from_secs(10)), Ok(true))
}

const HEARTBEAT_SECS: u64 = 30;

async fn run_mount(
    spec: LocalMountSpec,
    mut cmd_rx: mpsc::Receiver<LocalCmd>,
    retired: Arc<AtomicBool>,
    events: MountEventsArc,
) {
    let mut mounted_at: Option<String> = None;
    // Start immediately (silent) — mirrors the agent's boot behavior.
    let mut pending: Option<LocalCmd> = Some(LocalCmd::Start { allow_ui: false });

    loop {
        if retired.load(Ordering::SeqCst) {
            return;
        }

        match pending.take() {
            Some(LocalCmd::Stop) => {
                if let Some(p) = mounted_at.take() {
                    // Ownership guard — never eject a foreign volume.
                    // Explicit Stop forgets the persistent mapping
                    // (Windows) so the OS doesn't resurrect it at logon.
                    guarded_unmount(&spec, p, true).await;
                }
                emit(&events, &spec, "stopped", "Stopped".into(), None, None);
                // Idle until the next command.
                match cmd_rx.recv().await {
                    Some(cmd) => pending = Some(cmd),
                    None => return,
                }
                continue;
            }
            Some(LocalCmd::Restart { allow_ui }) => {
                if let Some(p) = mounted_at.take() {
                    // Transient unmount — keep the persistent profile.
                    guarded_unmount(&spec, p, false).await;
                }
                pending = Some(LocalCmd::Start { allow_ui });
                continue;
            }
            Some(LocalCmd::Start { allow_ui }) => {
                emit(&events, &spec, "mounting", "Mounting".into(), None, None);
                let s = spec.clone();
                let result = tokio::task::spawn_blocking(move || {
                    sys::mount(&s, allow_ui)
                })
                .await
                .unwrap_or_else(|e| {
                    Err(sys::MountError::Other(format!("mount task panicked: {}", e)))
                });
                match result {
                    Ok(path) => {
                        let notice = sys::drift_notice(&spec, &path);
                        sys::post_mount_upkeep(&spec, &path);
                        mounted_at = Some(path.clone());
                        log::info!("[local-mounts] {} mounted at {}", spec.id, path);
                        emit(&events, &spec, "mounted", "Mounted".into(),
                             Some(path), notice);
                    }
                    Err(sys::MountError::Auth(e)) => {
                        log::warn!("[local-mounts] {} auth failed: {}", spec.id, e);
                        emit(&events, &spec, "auth_error", e, None, None);
                        // Wait for the user (pill → Restart w/ UI). No
                        // auto-retry: it would fail identically.
                        match cmd_rx.recv().await {
                            Some(cmd) => pending = Some(cmd),
                            None => return,
                        }
                        continue;
                    }
                    Err(sys::MountError::Other(e)) => {
                        log::warn!("[local-mounts] {} mount failed: {}", spec.id, e);
                        emit(&events, &spec, "error", e, None, None);
                        // Fall through to the wait: heartbeat tick
                        // doubles as retry-with-backoff (30s).
                    }
                }
            }
            None => {}
        }

        // Mounted (or errored) steady state: wait for a command or the
        // heartbeat tick.
        tokio::select! {
            cmd = cmd_rx.recv() => {
                match cmd {
                    Some(c) => pending = Some(c),
                    None => return,
                }
            }
            _ = tokio::time::sleep(std::time::Duration::from_secs(HEARTBEAT_SECS)) => {
                match &mounted_at {
                    Some(p) => {
                        let pp = p.clone();
                        let np = spec.nas_share_path.clone();
                        let alive = tokio::task::spawn_blocking(move || {
                            probe_alive(&pp, &np)
                        })
                        .await
                        .unwrap_or(false);
                        if !alive {
                            log::warn!(
                                "[local-mounts] {} unreachable at {:?} — remounting",
                                spec.id, mounted_at
                            );
                            // Guarded cleanup of our own dead mount;
                            // foreign occupants are left alone and the
                            // remount lands wherever the OS picks.
                            if let Some(dead) = mounted_at.take() {
                                guarded_unmount(&spec, dead, false).await;
                            }
                            pending = Some(LocalCmd::Start { allow_ui: false });
                        }
                    }
                    None => {
                        // Errored earlier — heartbeat doubles as retry.
                        pending = Some(LocalCmd::Start { allow_ui: false });
                    }
                }
            }
        }
    }
}

/// Unmount `path` only when the mount table attributes it to our
/// share — never eject a foreign volume/mapping.
async fn guarded_unmount(spec: &LocalMountSpec, path: String, forget: bool) {
    let np = spec.nas_share_path.clone();
    let pp = path.clone();
    let ours = tokio::task::spawn_blocking(move || sys::is_ours(&pp, &np))
        .await
        .unwrap_or(None);
    if ours == Some(true) {
        let _ = tokio::task::spawn_blocking(move || sys::unmount(&path, forget)).await;
    }
}
