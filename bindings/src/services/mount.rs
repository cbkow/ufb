//! `Mount` QObject — wraps `core::mount_client::MountClient`.
//!
//! Phase 3c: the events trait → Q_SIGNAL bridge is fully wired here.
//!
//! - `Mount.start()` (called once from QML) constructs a shared
//!   `MountClient` and spins its connection loop, passing in a
//!   `MountEventsForwarder` that satisfies `core::events::MountEvents`.
//! - Each MountEvents method runs on a tokio task. The forwarder
//!   holds a `cxx_qt::CxxQtThread<Mount>` and uses `.queue(...)` to
//!   marshal each event onto the Qt thread, where it mutates the
//!   QObject's Rust struct + the framework-generated `*Changed`
//!   signal fires automatically because of `#[qproperty]`.
//!
//! State storage: the QObject holds the live `HashMap<mount_id,
//! MountStateUpdateMsg>` in its Rust struct. Each event re-encodes
//! the map to a JSON `QString` for the `mount_states_json` property
//! — QML's `Repeater` consumes that. A real `QAbstractListModel` is
//! deferred to the DirectoryModel work in a later phase (cxx-qt 0.7
//! doesn't ship one).

use std::collections::HashMap;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};
use ufb_core::events::MountEvents;
use ufb_core::mount_client::{
    self, AckMsg, CacheStatsMsg, ConflictDetectedMsg, ErrorMsg, MountClient, MountIdMsg,
    MountStateUpdateMsg, MountsConfig, UfbToAgent,
};

use crate::runtime::shared_runtime;

/// Slim copy of the agent's `MountConfig` — only the fields QML needs.
/// Read directly from %LOCALAPPDATA%\ufb\mounts.json so we don't have to
/// duplicate the full schema or wait on a list_mounts IPC round-trip.
#[derive(Clone, serde::Deserialize, serde::Serialize)]
pub struct MountInfo {
    id: String,
    #[serde(default, rename = "displayName")]
    display_name: String,
    #[serde(default, rename = "nasSharePath")]
    nas_share_path: String,
    #[serde(default, rename = "credentialKey")]
    credential_key: String,
    #[serde(default)]
    enabled: bool,
    /// Mirrors a Bookmark's isProjectFolder flag - tells the
    /// FileBrowser to expose the New Job button + the Synced
    /// column when the user navigates into this mount. Without
    /// this field on the IPC payload the QML side couldn't tell
    /// a mount-rooted jobs folder from any other mount.
    #[serde(default, rename = "isJobsFolder")]
    is_jobs_folder: bool,
    /// "Fake" mount: a bookmark that lives in the Mounts UI. The
    /// agent never manages it; nas_share_path is a native path used
    /// verbatim as the row's volumePath.
    #[serde(default)]
    unmanaged: bool,
    /// On-demand sync. Sync mounts stay agent-owned (the VFS host);
    /// plain mounts are GUI-owned on macOS (plans/17 F1).
    #[serde(default, rename = "syncEnabled")]
    sync_enabled: bool,
    /// Windows (slice B): preferred drive letter for GUI-owned letter
    /// mounts. Read-only here — the editor preserves it on round-trip.
    #[serde(default, rename = "mountDriveLetter")]
    mount_drive_letter: String,
}

#[derive(serde::Deserialize)]
struct MountsFile {
    #[serde(default)]
    mounts: Vec<MountInfo>,
}

fn mounts_json_path() -> Option<PathBuf> {
    // Mirror `agent/src/config.rs::config_file_path` exactly. The
    // agent owns this file and `dirs::data_local_dir()` on macOS
    // resolves to `~/Library/Application Support`, while the agent
    // writes to `~/.local/share/ufb` (XDG-style) on every Unix.
    // A divergence here means UFB's bindings read 0 configs and
    // every mount row's volumePath comes out empty, which disables
    // the "Open"/"Reveal"/"Open in New Tab" right-click options.
    #[cfg(windows)]
    {
        return std::env::var_os("LOCALAPPDATA")
            .map(|d| PathBuf::from(d).join("ufb").join("mounts.json"));
    }
    #[cfg(not(windows))]
    {
        std::env::var_os("HOME")
            .map(|h| PathBuf::from(h).join(".local/share/ufb/mounts.json"))
    }
}

fn volumes_base() -> PathBuf {
    if cfg!(target_os = "windows") {
        PathBuf::from(r"C:\Volumes\ufb")
    } else if let Some(home) = dirs::home_dir() {
        home.join("ufb/mounts")
    } else {
        PathBuf::from("/tmp/ufb-mounts")
    }
}

fn share_name(nas_share_path: &str, fallback_id: &str) -> String {
    let trimmed = nas_share_path.trim_end_matches('\\');
    trimmed
        .rsplit('\\')
        .next()
        .filter(|s| !s.is_empty())
        .unwrap_or(fallback_id)
        .to_string()
}

/// Quietly persist a first-assignment drive letter into mounts.json
/// (slice B letter stability): once a mount lands on a letter, that
/// letter becomes its sticky preference so it never drifts on later
/// mounts/reboots. Raw-JSON edit — preserves every field the typed
/// save path doesn't know about, triggers no agent reload and no task
/// reconcile (nothing changed semantically: the letter written is the
/// letter currently mounted). Only fills an EMPTY mountDriveLetter —
/// a user-chosen letter is never overwritten.
#[cfg(windows)]
fn persist_drive_letter(mount_id: &str, letter: char) {
    let Some(path) = mounts_json_path() else { return };
    let Ok(text) = std::fs::read_to_string(&path) else { return };
    let Ok(mut v) = serde_json::from_str::<serde_json::Value>(&text) else { return };
    let Some(mounts) = v.get_mut("mounts").and_then(|m| m.as_array_mut()) else { return };
    let mut changed = false;
    for m in mounts.iter_mut() {
        if m.get("id").and_then(|i| i.as_str()) == Some(mount_id) {
            let cur = m
                .get("mountDriveLetter")
                .and_then(|l| l.as_str())
                .unwrap_or("");
            if cur.trim().is_empty() {
                m["mountDriveLetter"] =
                    serde_json::Value::String(letter.to_ascii_uppercase().to_string());
                changed = true;
            }
        }
    }
    if !changed {
        return;
    }
    match serde_json::to_string_pretty(&v) {
        Ok(s) => {
            if let Err(e) = std::fs::write(&path, s) {
                log::warn!("mount: persist_drive_letter write failed: {}", e);
            } else {
                log::info!(
                    "mount: persisted drive letter {}: for {} (first assignment)",
                    letter, mount_id
                );
            }
        }
        Err(e) => log::warn!("mount: persist_drive_letter serialize failed: {}", e),
    }
}

fn load_mount_configs() -> Vec<MountInfo> {
    let Some(path) = mounts_json_path() else {
        return Vec::new();
    };
    let Ok(data) = std::fs::read_to_string(&path) else {
        log::warn!("mount: could not read {:?}", path);
        return Vec::new();
    };
    match serde_json::from_str::<MountsFile>(&data) {
        Ok(f) => f.mounts,
        Err(e) => {
            log::warn!("mount: parse {:?} failed: {}", path, e);
            Vec::new()
        }
    }
}

/// Lazy single MountClient — the polling `is_agent_running()` and the
/// long-lived connection loop share it.
fn shared_client() -> Arc<MountClient> {
    static CLIENT: OnceLock<Arc<MountClient>> = OnceLock::new();
    CLIENT.get_or_init(|| Arc::new(MountClient::new())).clone()
}

/// The events forwarder created in Mount::start(), shared so config
/// saves can reconcile local mount tasks with the same emitter.
#[cfg(any(target_os = "macos", windows))]
fn shared_forwarder() -> &'static OnceLock<ufb_core::events::MountEventsArc> {
    static FWD: OnceLock<ufb_core::events::MountEventsArc> = OnceLock::new();
    &FWD
}

/// Mirror of the `connected` / `agent_required` properties readable
/// off the GUI thread — feeds the agent keep-alive retry loop. The
/// one-shot spawn at startup is raceable (a dying agent's socket can
/// accept one last connection and fake "running", then exit), so a
/// 15s background loop re-spawns whenever the agent is needed but
/// unreachable. spawn_agent_if_needed is idempotent.
static AGENT_CONNECTED: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);
static AGENT_REQUIRED: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(true);

/// True when any enabled mount needs the agent process (= has sync
/// on). Plain mounts are GUI-owned on macOS (plans/17 F1) and Windows
/// (slice B); the agent exists purely as the sync VFS host.
fn agent_required() -> bool {
    #[cfg(any(target_os = "macos", windows))]
    {
        load_mount_configs()
            .iter()
            .any(|c| c.enabled && !c.unmanaged && c.sync_enabled)
    }
    #[cfg(not(any(target_os = "macos", windows)))]
    {
        true
    }
}

/// The set of mounts the GUI owns locally: enabled, real (not
/// bookmark-only), not sync.
#[cfg(any(target_os = "macos", windows))]
fn local_specs_from_configs() -> Vec<crate::local_mounts::LocalMountSpec> {
    load_mount_configs()
        .into_iter()
        .filter(|c| c.enabled && !c.unmanaged && !c.sync_enabled)
        .map(|c| crate::local_mounts::LocalMountSpec {
            share_name: share_name(&c.nas_share_path, &c.id),
            // Windows: preferred letter from mountDriveLetter ("M" /
            // "M:" spellings both fine — first alphabetic char wins).
            drive_letter: c
                .mount_drive_letter
                .trim()
                .chars()
                .next()
                .filter(|ch| ch.is_ascii_alphabetic()),
            nas_share_path: c.nas_share_path,
            id: c.id,
        })
        .collect()
}

/// Fire-and-forget command send. Logs failures but doesn't surface
/// them — the agent's reply path (via the events forwarder) updates
/// state so the user sees the result without us having to await.
fn send_mount_id(cmd: UfbToAgent) {
    let client = shared_client();
    let _guard = shared_runtime().enter();
    if let Err(e) = shared_runtime().block_on(client.send_command(cmd)) {
        log::warn!("mount: send_command failed: {}", e);
    }
}

/// Called from C++ on `QGuiApplication::aboutToQuit` (via the
/// `ufb_shutdown_agent_blocking` extern). Sends `UfbToAgent::Quit` to
/// the agent and waits briefly for the pipe to drain so the message
/// actually reaches the agent before this process tears the tokio
/// runtime down.
///
/// The agent's Quit handler (see `agent/src/mount_service.rs`)
/// unmounts every share and calls `process::exit(0)`, so by the time
/// the wait completes the agent is already gone.
pub fn shutdown_agent_for_quit() {
    log::info!("mount: shutdown_agent_for_quit() entered");
    let client = shared_client();
    let connected = shared_runtime().block_on(client.is_connected());
    log::info!("mount: shutdown_agent_for_quit — IPC connected={}", connected);
    let _guard = shared_runtime().enter();
    shared_runtime().block_on(async move {
        match client.send_command(UfbToAgent::Quit).await {
            Ok(()) => {
                log::info!("mount: Quit queued onto mount-client cmd channel");
            }
            Err(e) => {
                log::warn!("mount: shutdown_agent Quit send failed: {}", e);
                return;
            }
        }
        // Give the connection_loop time to forward the cmd to the
        // blocking I/O thread and for the I/O thread to write it to
        // the pipe before this process exits and tears the tokio
        // runtime down. 300ms is plenty for a local named pipe.
        tokio::time::sleep(Duration::from_millis(300)).await;
        log::info!("mount: shutdown_agent_for_quit — drain wait finished");
    });
}

/// Generate a UUID, stash it in `pending_commands`, re-encode the
/// JSON property, then fire the command via `send_mount_id`. Returns
/// the UUID so QML callers can correlate explicitly if they want.
/// The 10s timeout sweep (spawned from `Mount::start`) catches the
/// case where the agent never Acks.
fn track_and_send(
    mut mount: core::pin::Pin<&mut qobject::Mount>,
    mount_id: &str,
    verb: &'static str,
    build_cmd: impl FnOnce(String) -> UfbToAgent,
) -> cxx_qt_lib::QString {
    use cxx_qt::CxxQtType;

    let command_id = uuid::Uuid::new_v4().to_string();
    let started_at_ms = ufb_core::utils::current_time_ms();

    // Single borrow window: insert pending entry, clear prior error
    // for this mount (fresh command supersedes stale error pill),
    // re-encode both JSON properties for emit below.
    let (pending_json, err_json) = {
        let rust_mut = mount.as_mut().rust_mut();
        let rust = unsafe { rust_mut.get_unchecked_mut() };
        rust.pending_commands.insert(
            command_id.clone(),
            PendingCommand {
                mount_id: mount_id.to_string(),
                verb,
                started_at: Instant::now(),
                started_at_ms,
            },
        );
        rust.last_command_errors.remove(mount_id);
        (
            serialize_pending_commands(&rust.pending_commands),
            serialize_command_errors(&rust.last_command_errors),
        )
    };
    mount
        .as_mut()
        .set_pending_commands_json(cxx_qt_lib::QString::from(&pending_json));
    mount
        .as_mut()
        .set_last_command_error_json(cxx_qt_lib::QString::from(&err_json));

    send_mount_id(build_cmd(command_id.clone()));
    cxx_qt_lib::QString::from(&command_id)
}

/// Serialize `pending_commands` to the JSON shape QML expects.
fn serialize_pending_commands(
    pending: &HashMap<String, PendingCommand>,
) -> String {
    #[derive(serde::Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Entry<'a> {
        command_id: &'a str,
        mount_id: &'a str,
        verb: &'a str,
        started_at_ms: i64,
    }
    let entries: Vec<Entry<'_>> = pending
        .iter()
        .map(|(cid, p)| Entry {
            command_id: cid,
            mount_id: &p.mount_id,
            verb: p.verb,
            started_at_ms: p.started_at_ms,
        })
        .collect();
    serde_json::to_string(&entries).unwrap_or_else(|_| "[]".to_string())
}

/// Serialize `last_command_errors` to a JSON map keyed by mount_id.
fn serialize_command_errors(errs: &HashMap<String, CommandError>) -> String {
    let map: serde_json::Map<String, serde_json::Value> = errs
        .iter()
        .map(|(mid, e)| {
            (
                mid.clone(),
                serde_json::json!({
                    "message": e.message,
                    "verb":    e.verb,
                    "atMs":    e.at_ms,
                }),
            )
        })
        .collect();
    serde_json::to_string(&serde_json::Value::Object(map)).unwrap_or_else(|_| "{}".into())
}

/// How long a pending command can sit before we declare it failed.
const COMMAND_TIMEOUT: Duration = Duration::from_secs(10);

/// Locate `ufb-agent[.exe]` on disk. Search order:
///   1. <exe dir>/ufb-agent[.exe]                     — bundled installer
///   2. macOS .app sibling: ../../ufb-agent.app/Contents/MacOS/ufb-agent
///   3. <repo root>/agent/target/{release,debug}/     — dev build
///   4. PATH                                           — fallback
fn locate_agent_binary() -> Result<PathBuf, String> {
    let agent_name = if cfg!(target_os = "windows") {
        "ufb-agent.exe"
    } else {
        "ufb-agent"
    };

    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            // Bundled: alongside ufb.exe (Windows) or inside UFB.app
            // /Contents/MacOS (macOS — but we want the agent's own
            // .app, sibling of UFB.app per macOSplans/08).
            candidates.push(dir.join(agent_name));

            #[cfg(target_os = "macos")]
            {
                // Production layout: /Applications/UFB/{UFB.app,
                // ufb-agent.app}/. From UFB.app/Contents/MacOS/ufb,
                // three .parent() calls land at /Applications/UFB/,
                // and the agent bundle is a sibling of UFB.app there.
                //   dir            = UFB.app/Contents/MacOS
                //   .parent()      = UFB.app/Contents
                //   .parent()      = UFB.app
                //   .parent()      = /Applications/UFB   ← bundle root
                // Earlier this had a fourth .parent() that landed at
                // /Applications and silently looked for
                // /Applications/ufb-agent.app — which doesn't exist,
                // so heal_macos's posix_spawn fell through to the
                // bare "ufb-agent" name and failed with ENOENT after
                // SIGKILL'ing the agent the tray had already spawned.
                if let Some(contents_dir) = dir.parent() {
                    if let Some(app_bundle) = contents_dir.parent() {
                        if let Some(bundle_root) = app_bundle.parent() {
                            candidates.push(
                                bundle_root
                                    .join("ufb-agent.app/Contents/MacOS")
                                    .join(agent_name),
                            );
                        }
                    }
                }
                // Pre-08 fallback: agent next to UFB.app's bundle
                // resources (slice 02-and-earlier dev installs).
                if let Some(macos_dir) = dir.parent() {
                    candidates.push(
                        macos_dir.join("Resources").join(agent_name),
                    );
                }
            }

            // Dev: walk up to the repo root, then dive into
            // agent/target/{debug,release}. Prefer the .app bundle
            // (staged by scripts/sign-mac-dev.sh) over the bare
            // binary so dev runs match the install layout post slice
            // 08; the bare-binary path stays as a fallback for
            // builds where the staging step hasn't run.
            for ancestor in dir.ancestors().take(8) {
                let agent_root = ancestor.join("agent").join("target");
                if agent_root.exists() {
                    #[cfg(target_os = "macos")]
                    {
                        candidates.push(
                            agent_root
                                .join("release")
                                .join("ufb-agent.app/Contents/MacOS")
                                .join(agent_name),
                        );
                        candidates.push(
                            agent_root
                                .join("debug")
                                .join("ufb-agent.app/Contents/MacOS")
                                .join(agent_name),
                        );
                    }
                    candidates.push(agent_root.join("release").join(agent_name));
                    candidates.push(agent_root.join("debug").join(agent_name));
                    candidates.push(
                        agent_root
                            .join("x86_64-pc-windows-msvc")
                            .join("release")
                            .join(agent_name),
                    );
                    candidates.push(
                        agent_root
                            .join("x86_64-pc-windows-msvc")
                            .join("debug")
                            .join(agent_name),
                    );
                    candidates.push(
                        agent_root
                            .join("aarch64-apple-darwin")
                            .join("release")
                            .join(agent_name),
                    );
                    candidates.push(
                        agent_root
                            .join("aarch64-apple-darwin")
                            .join("debug")
                            .join(agent_name),
                    );
                    break;
                }
            }
        }
    }
    // PATH fallback — std::process::Command handles this if we pass
    // the bare name.
    candidates.push(PathBuf::from(agent_name));

    candidates
        .into_iter()
        .find(|p| p == &PathBuf::from(agent_name) || p.exists())
        .ok_or_else(|| format!("{} not found", agent_name))
}

/// Read the agent's PID file. None if no file or unreadable.
/// Mirrors `agent/src/main.rs::pid_file_path`.
#[allow(dead_code)]
fn read_agent_pid() -> Option<i32> {
    #[cfg(target_os = "macos")]
    let pid_path = std::env::var_os("HOME").map(|home| {
        PathBuf::from(home).join("Library/Application Support/ufb/agent.pid")
    })?;
    #[cfg(windows)]
    let pid_path = std::env::var_os("LOCALAPPDATA").map(|local| {
        PathBuf::from(local).join("ufb").join("agent.pid")
    })?;
    #[cfg(all(unix, not(target_os = "macos")))]
    let pid_path = std::env::var_os("HOME").map(|home| {
        PathBuf::from(home).join(".local/share/ufb/agent.pid")
    })?;

    std::fs::read_to_string(&pid_path)
        .ok()
        .and_then(|s| s.trim().parse::<i32>().ok())
}

/// Heal-on-open (macOS), trimmed in 1.0.7: the launchctl kickstart /
/// bootstrap steps died with the LaunchAgent-plist concept (nothing
/// installs one; F2's SMAppService + socket activation replaces it).
///   1. Read PID. If alive → SIGKILL it (it's stuck — IPC failed).
///   2. posix_spawn with setsid() so the child detaches from our
///      process group and survives Qt-app exit.
/// Then waits up to 5s for the agent socket to come back.
#[cfg(target_os = "macos")]
fn heal_macos() -> Result<(), String> {
    use std::os::unix::process::CommandExt;
    use std::time::Duration;

    // Step 1: kill stale agent if its PID is still alive.
    if let Some(pid) = read_agent_pid() {
        let alive = unsafe { libc::kill(pid, 0) == 0 };
        if alive {
            log::info!("[heal] stale agent pid {} still alive — sending SIGKILL", pid);
            unsafe {
                libc::kill(pid, libc::SIGKILL);
            }
            std::thread::sleep(Duration::from_millis(500));
        }
    }

    // Step 2: bare posix_spawn with setsid() so the agent detaches.
    let agent_path = locate_agent_binary()?;
    log::info!(
        "[heal] posix_spawn fallback: {}",
        agent_path.display()
    );
    let mut cmd = std::process::Command::new(&agent_path);
    unsafe {
        cmd.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    cmd.spawn().map_err(|e| {
        format!("Failed to spawn {}: {}", agent_path.display(), e)
    })?;

    wait_for_agent()
}

/// Wait up to 5s for `is_agent_running()` to flip true. The
/// MountClient's IPC reconnection loop handles the actual connect; we
/// just wait long enough for the socket/pipe to come up.
#[cfg(any(target_os = "macos", windows))]
fn wait_for_agent() -> Result<(), String> {
    use std::time::{Duration, Instant};
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        if shared_client().is_agent_running() {
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    Err("Agent did not come up within 5s".into())
}

/// Spawn or revive the agent if it isn't accepting IPC. macOS uses
/// the heal chain above; Windows + Linux use a plain detached spawn
/// (the heal-on-open story for those targets stays simple — they
/// don't have launchctl).
fn spawn_agent_if_needed() -> Result<(), String> {
    if shared_client().is_agent_running() {
        return Ok(());
    }

    #[cfg(target_os = "macos")]
    {
        return heal_macos();
    }

    #[cfg(not(target_os = "macos"))]
    {
        let agent_path = locate_agent_binary()?;
        log::info!("mount: launching agent: {}", agent_path.display());

        #[cfg(target_os = "windows")]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x08000000;
            std::process::Command::new(&agent_path)
                .creation_flags(CREATE_NO_WINDOW)
                .spawn()
                .map_err(|e| format!("Failed to launch {}: {}", agent_path.display(), e))?;
            // Block briefly until the agent's named pipe is up. On a
            // cold first launch the agent takes a second or so to
            // initialize logging + IPC pipe + mount orchestrators;
            // without this wait, MountClient.start (which fires
            // immediately after) would enter its 500ms-then-3s retry
            // loop and the user sees "agent waiting" until they close
            // and reopen UFB. Mirror of the macOS wait_for_agent path.
            return wait_for_agent();
        }
        #[cfg(all(unix, not(target_os = "macos")))]
        {
            std::process::Command::new(&agent_path)
                .spawn()
                .map_err(|e| format!("Failed to launch {}: {}", agent_path.display(), e))?;
        }
        // Windows returned via wait_for_agent above; only the
        // non-macOS unix path falls through to here.
        #[cfg(not(target_os = "windows"))]
        Ok(())
    }
}

#[cxx_qt::bridge]
pub mod qobject {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qml_singleton]
        #[qproperty(bool, connected)]
        #[qproperty(QString, mount_states_json)]
        /// JSON map of mount_id -> {hydratedBytes, hydratedCount,
        /// updatedAtMs}. Populated lazily as QML calls
        /// `request_cache_stats(mount_id)`; agent responses arrive
        /// via the events forwarder. `{}` until the first response.
        #[qproperty(QString, cache_stats_json)]
        /// JSON array of `{commandId, mountId, verb, startedAtMs}` for
        /// every lifecycle command (start/stop/restart/clear_sync_cache)
        /// that the agent hasn't yet Ack'd. QML
        /// derives per-mount busy state from this — sidebar rows and
        /// MountManagerDialog buttons can show spinners while an entry
        /// for their mountId is present. Slice C addition; cleared by
        /// Ack/Error from the agent or by the 10-second timeout sweep.
        #[qproperty(QString, pending_commands_json)]
        /// JSON map of `mountId -> {message, atMs, verb}` for the most
        /// recent command failure (timeout or agent-side Error). QML
        /// renders an inline error pill from this. Cleared by the next
        /// successful command on the same mount. Slice C addition.
        #[qproperty(QString, last_command_error_json)]
        /// False when no configured mount needs the agent process (on
        /// macOS the GUI owns plain mounts — F1 — so the agent exists
        /// only as the sync VFS host). QML gates the "agent waiting"
        /// footer + Launch affordance on this so a sync-free setup
        /// never nags about a process it doesn't need.
        #[qproperty(bool, agent_required)]
        type Mount = super::MountRust;

        /// Probe the agent's pipe / socket synchronously. QML timers
        /// can use this as a backstop in case the long-lived IPC
        /// connection races startup ordering.
        #[qinvokable]
        fn is_agent_running(self: &Mount) -> bool;

        /// Start the long-lived agent IPC connection loop. Idempotent —
        /// safe to call from QML's `Component.onCompleted` more than once.
        /// Auto-spawns the ufb-agent subprocess if it isn't already
        /// running (mirrors the legacy mount_launch_agent flow).
        #[qinvokable]
        fn start(self: Pin<&mut Mount>);

        /// Spawn `ufb-agent.exe` (Windows) / `ufb-agent` (mac) as a
        /// detached child process if it isn't already running.
        /// Returns "" on success or an error message. Search order:
        ///   1. `<exe dir>/ufb-agent[.exe]`           — bundled
        ///   2. `<repo root>/agent/target/release/`    — dev build
        ///   3. `<repo root>/agent/target/debug/`      — dev build
        ///   4. PATH lookup
        #[qinvokable]
        fn launch_agent(self: &Mount) -> QString;

        // ── Mount config CRUD ────────────────────────────────────────
        // Read-back of the full mounts.json (NOT the slim merged-state
        // mount_states_json). Used by MountManagerDialog so the user
        // can edit every field on each mount config.
        #[qinvokable]
        fn read_mount_configs(self: &Mount) -> QString;

        /// Overwrite mounts.json with `configs_json` (a serialised
        /// MountsConfig). Sends ReloadConfig to the agent on success.
        /// Returns "" on success or an error message.
        #[qinvokable]
        fn write_mount_configs(self: Pin<&mut Mount>, configs_json: QString) -> QString;

        // ── Per-mount agent commands ─────────────────────────────────
        // After Slice C, each command returns the UUID command_id it
        // sent to the agent. QML can ignore the return value and just
        // observe `pending_commands_json` for per-mount busy state, or
        // capture the UUID to correlate explicit Ack/Error toasts.
        // Pending entries auto-expire after 10s.
        #[qinvokable]
        fn start_mount(self: Pin<&mut Mount>, mount_id: QString) -> QString;
        #[qinvokable]
        fn stop_mount(self: Pin<&mut Mount>, mount_id: QString) -> QString;
        #[qinvokable]
        fn restart_mount(self: Pin<&mut Mount>, mount_id: QString) -> QString;
        #[qinvokable]
        fn clear_sync_cache(self: Pin<&mut Mount>, mount_id: QString) -> QString;
        #[qinvokable]
        fn reload_config(self: &Mount);

        /// Request fresh cache statistics for `mount_id`. Fire-and-
        /// forget - the agent replies asynchronously and the result
        /// lands in `cache_stats_json` via the events forwarder.
        /// QML rebinds whatever Label is reading the property.
        #[qinvokable]
        fn request_cache_stats(self: &Mount, mount_id: QString);

        /// JSON array of drive letters currently occupied on this
        /// machine (local volumes + any network mapping, live or
        /// remembered) — e.g. ["C","D","U","V"]. Feeds the mount
        /// editor's letter picker; "[]" on non-Windows.
        #[qinvokable]
        fn used_drive_letters(self: &Mount) -> QString;

        /// Open the agent log in the system default text editor.
        /// `mount_id` is currently ignored (the agent writes a single
        /// shared log file, not per-mount); the parameter is reserved
        /// for when per-mount log splitting lands. Returns "" on
        /// success or an error message.
        #[qinvokable]
        fn open_logs(self: &Mount, mount_id: QString) -> QString;
    }

    // Threading allows background tasks to queue closures onto the
    // Qt thread that update this QObject. Required by the events
    // forwarder pattern below.
    impl cxx_qt::Threading for Mount {}
}

/// Backing data for the Mount QObject. Exposed properties are public
/// fields with the same names as their `#[qproperty(...)]` declarations.
pub struct MountRust {
    pub connected: bool,
    pub mount_states_json: cxx_qt_lib::QString,
    /// JSON map of mount_id -> {hydratedBytes, hydratedCount,
    /// updatedAtMs}. Re-encoded whenever a CacheStats event lands.
    pub cache_stats_json: cxx_qt_lib::QString,
    /// JSON array of `{commandId, mountId, verb, startedAtMs}`.
    /// Re-encoded whenever a command is issued, acked, or expires.
    pub pending_commands_json: cxx_qt_lib::QString,
    /// JSON map of `mountId -> {message, atMs, verb}` for most recent
    /// command failure. Slice C addition.
    pub last_command_error_json: cxx_qt_lib::QString,
    /// Live cache of state updates keyed by mount_id. Re-encoded to
    /// JSON whenever an event lands.
    pub states: HashMap<String, MountStateUpdateMsg>,
    /// Live cache of CacheStats responses keyed by mount_id.
    pub cache_stats: HashMap<String, CacheStatsMsg>,
    /// In-flight commands keyed by command_id. Holds enough metadata to
    /// reconstruct the JSON property without re-walking other state.
    pub pending_commands: HashMap<String, PendingCommand>,
    /// Most recent command-failure per mount (timeout or agent Error).
    /// Cleared by the next successful command on the same mount.
    pub last_command_errors: HashMap<String, CommandError>,
    /// Static config snapshot loaded from mounts.json on start().
    /// Used to enrich state updates with display_name + volume_path.
    pub configs: HashMap<String, MountInfo>,
    /// See the qproperty doc — false when no mount needs the agent.
    pub agent_required: bool,
    /// Guards against double-start from QML.
    pub started: bool,
}

/// One entry in the `pending_commands` tracking map.
#[derive(Clone)]
pub struct PendingCommand {
    pub mount_id: String,
    /// "start" | "stop" | "restart" | "clear_sync_cache"
    pub verb: &'static str,
    /// Monotonic clock the command was issued on. Used for the 10s
    /// timeout sweep. We don't expose this directly; QML reads
    /// `startedAtMs` which is wall-clock millis at the same moment.
    pub started_at: Instant,
    /// Wall-clock millis at issue time (rendered into JSON for QML).
    pub started_at_ms: i64,
}

/// One entry in `last_command_errors`.
#[derive(Clone)]
pub struct CommandError {
    pub message: String,
    pub verb: &'static str,
    pub at_ms: i64,
}

impl Default for MountRust {
    fn default() -> Self {
        Self {
            connected: false,
            mount_states_json: cxx_qt_lib::QString::from("[]"),
            cache_stats_json: cxx_qt_lib::QString::from("{}"),
            pending_commands_json: cxx_qt_lib::QString::from("[]"),
            last_command_error_json: cxx_qt_lib::QString::from("{}"),
            states: HashMap::new(),
            cache_stats: HashMap::new(),
            pending_commands: HashMap::new(),
            last_command_errors: HashMap::new(),
            configs: HashMap::new(),
            // Assume required until start() computes the truth — the
            // brief "agent waiting" flash is better than hiding a real
            // dependency.
            agent_required: true,
            started: false,
        }
    }
}

impl qobject::Mount {
    fn is_agent_running(self: &qobject::Mount) -> bool {
        shared_client().is_agent_running()
    }

    fn launch_agent(self: &qobject::Mount) -> cxx_qt_lib::QString {
        match spawn_agent_if_needed() {
            Ok(()) => cxx_qt_lib::QString::from(""),
            Err(e) => {
                log::warn!("mount: launch_agent failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn read_mount_configs(self: &qobject::Mount) -> cxx_qt_lib::QString {
        let cfg = mount_client::load_mount_config();
        let json = serde_json::to_string(&cfg).unwrap_or_else(|_| "{\"version\":1,\"mounts\":[]}".into());
        cxx_qt_lib::QString::from(&json)
    }

    fn write_mount_configs(
        mut self: core::pin::Pin<&mut qobject::Mount>,
        configs_json: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let cfg: MountsConfig = match serde_json::from_str(&configs_json.to_string()) {
            Ok(c) => c,
            Err(e) => return cxx_qt_lib::QString::from(&format!("Parse failed: {}", e)),
        };
        if let Err(e) = mount_client::save_mount_config(&cfg) {
            return cxx_qt_lib::QString::from(&e);
        }
        log::info!("mount: wrote {} configs to mounts.json", cfg.mounts.len());
        // Tell the agent to re-read config so any new mounts come up
        // and removed ones come down.
        let client = shared_client();
        {
            let _guard = shared_runtime().enter();
            let _ = shared_runtime().block_on(client.send_command(UfbToAgent::ReloadConfig));
        }
        // Refresh OUR config cache too — it was loaded once in start()
        // and serialize_merged reads it forever after. Managed mounts
        // used to paper over the staleness because agent state events
        // re-serialized anyway (config-less, but visible); unmanaged
        // bookmark entries have NO agent state, so without this reload
        // they only appeared after an app restart.
        let new_json = {
            use cxx_qt::CxxQtType;
            let configs = load_mount_configs();
            let rust_mut = self.as_mut().rust_mut();
            let rust = unsafe { rust_mut.get_unchecked_mut() };
            rust.configs = configs.into_iter().map(|c| (c.id.clone(), c)).collect();
            publish_live_roots(&rust.states, &rust.configs);
            serialize_merged(&rust.states, &rust.configs)
        };
        self.as_mut()
            .set_mount_states_json(cxx_qt_lib::QString::from(&new_json));

        // Reconcile GUI-owned mount tasks with the new config (F1),
        // and bring the agent up if this save introduced the first
        // sync mount (it wasn't spawned at startup without one).
        let required = agent_required();
        self.as_mut().set_agent_required(required);
        AGENT_REQUIRED.store(required, std::sync::atomic::Ordering::SeqCst);
        #[cfg(any(target_os = "macos", windows))]
        {
            if let Some(fwd) = shared_forwarder().get() {
                crate::local_mounts::apply_config(
                    local_specs_from_configs(),
                    fwd.clone(),
                );
            }
            if required && !shared_client().is_agent_running() {
                if let Err(e) = spawn_agent_if_needed() {
                    log::warn!("mount: agent spawn after config save failed: {}", e);
                }
            }
        }
        cxx_qt_lib::QString::from("")
    }

    fn start_mount(
        self: core::pin::Pin<&mut qobject::Mount>,
        mount_id: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        // These invokables only fire from explicit user actions (tray,
        // sidebar menu, mount manager), so the OS auth dialog is always
        // permitted: it self-limits to the case where the server
        // actually rejects the stored credentials. Background attempts
        // (boot, heartbeat, backoff) never come through here and stay
        // silent. "Login" is not a UFB concept — plans/17 slice C.
        //
        // GUI-owned plain mounts route to the local manager; sync
        // mounts route to the agent.
        #[cfg(any(target_os = "macos", windows))]
        if crate::local_mounts::send(
            &mount_id.to_string(),
            crate::local_mounts::LocalCmd::Start { allow_ui: true },
        ) {
            return cxx_qt_lib::QString::from("");
        }
        track_and_send(self, &mount_id.to_string(), "start", |cid| {
            UfbToAgent::StartMount(MountIdMsg {
                mount_id: mount_id.to_string(),
                command_id: cid,
                allow_ui: true,
            })
        })
    }
    fn stop_mount(
        self: core::pin::Pin<&mut qobject::Mount>,
        mount_id: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        #[cfg(any(target_os = "macos", windows))]
        if crate::local_mounts::send(
            &mount_id.to_string(),
            crate::local_mounts::LocalCmd::Stop,
        ) {
            return cxx_qt_lib::QString::from("");
        }
        track_and_send(self, &mount_id.to_string(), "stop", |cid| {
            UfbToAgent::StopMount(MountIdMsg {
                mount_id: mount_id.to_string(),
                command_id: cid,
                allow_ui: false,
            })
        })
    }
    fn restart_mount(
        self: core::pin::Pin<&mut qobject::Mount>,
        mount_id: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        // User-initiated — UI permitted (see start_mount).
        #[cfg(any(target_os = "macos", windows))]
        if crate::local_mounts::send(
            &mount_id.to_string(),
            crate::local_mounts::LocalCmd::Restart { allow_ui: true },
        ) {
            return cxx_qt_lib::QString::from("");
        }
        track_and_send(self, &mount_id.to_string(), "restart", |cid| {
            UfbToAgent::RestartMount(MountIdMsg {
                mount_id: mount_id.to_string(),
                command_id: cid,
                allow_ui: true,
            })
        })
    }
    fn clear_sync_cache(
        self: core::pin::Pin<&mut qobject::Mount>,
        mount_id: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        track_and_send(self, &mount_id.to_string(), "clear_sync_cache", |cid| {
            UfbToAgent::ClearSyncCache(MountIdMsg {
                mount_id: mount_id.to_string(),
                command_id: cid,
                allow_ui: false,
            })
        })
    }

    fn request_cache_stats(
        self: &qobject::Mount,
        mount_id: cxx_qt_lib::QString,
    ) {
        // GetCacheStats is a poll, not a lifecycle command — no need
        // for UUID round-trip + spinner; the response lands directly
        // in cache_stats_json. Keep the old fire-and-forget shape.
        send_mount_id(UfbToAgent::GetCacheStats(MountIdMsg {
            mount_id: mount_id.to_string(),
            command_id: String::new(),
            allow_ui: false,
        }));
    }

    fn reload_config(self: &qobject::Mount) {
        let client = shared_client();
        let _guard = shared_runtime().enter();
        let _ = shared_runtime().block_on(client.send_command(UfbToAgent::ReloadConfig));
    }

    fn used_drive_letters(self: &qobject::Mount) -> cxx_qt_lib::QString {
        #[cfg(windows)]
        {
            let letters: Vec<String> = ufb_core::windows_mounts::letters_in_use()
                .into_iter()
                .map(|c| c.to_string())
                .collect();
            cxx_qt_lib::QString::from(
                &serde_json::to_string(&letters).unwrap_or_else(|_| "[]".into()),
            )
        }
        #[cfg(not(windows))]
        {
            cxx_qt_lib::QString::from("[]")
        }
    }

    fn open_logs(
        self: &qobject::Mount,
        _mount_id: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let log_path = ufb_core::utils::get_app_data_dir().join("ufb-agent.log");
        if !log_path.exists() {
            let msg = format!("agent log not yet written ({})", log_path.display());
            log::warn!("mount: open_logs: {}", msg);
            return cxx_qt_lib::QString::from(&msg);
        }
        // Hand off to the OS so the file opens in whatever the user
        // has associated with `.log` (Notepad, VS Code, etc.). Don't
        // wait for the editor to exit - this is fire-and-forget.
        let path_str = log_path.to_string_lossy().into_owned();
        let result = {
            #[cfg(target_os = "windows")]
            {
                // Empty title arg ("") so `start` doesn't treat the
                // path as the window title when it has spaces.
                std::process::Command::new("cmd")
                    .args(["/C", "start", "", path_str.as_str()])
                    .spawn()
                    .map(|_| ())
            }
            #[cfg(target_os = "macos")]
            {
                std::process::Command::new("open")
                    .arg(&path_str)
                    .spawn()
                    .map(|_| ())
            }
            #[cfg(not(any(target_os = "windows", target_os = "macos")))]
            {
                std::process::Command::new("xdg-open")
                    .arg(&path_str)
                    .spawn()
                    .map(|_| ())
            }
        };
        match result {
            Ok(()) => cxx_qt_lib::QString::from(""),
            Err(e) => {
                let msg = format!("could not open log: {}", e);
                log::warn!("mount: open_logs failed: {}", msg);
                cxx_qt_lib::QString::from(&msg)
            }
        }
    }

    fn start(mut self: core::pin::Pin<&mut qobject::Mount>) {
        use cxx_qt::CxxQtType;
        use cxx_qt::Threading;

        // Idempotent — QML may call start() on every reload.
        if self.as_ref().rust().started {
            return;
        }
        self.as_mut().rust_mut().started = true;

        // Load static mount configs from mounts.json. Synchronous —
        // tiny file, runs once at startup. Re-emit the (currently
        // empty) merged JSON so QML sees the config side even before
        // the agent connects.
        let configs = load_mount_configs();
        let new_json = {
            let rust_mut = self.as_mut().rust_mut();
            let rust = unsafe { rust_mut.get_unchecked_mut() };
            rust.configs = configs.into_iter().map(|c| (c.id.clone(), c)).collect();
            log::info!("mount: loaded {} configs from mounts.json", rust.configs.len());
            publish_live_roots(&rust.states, &rust.configs);
            serialize_merged(&rust.states, &rust.configs)
        };
        self.as_mut()
            .set_mount_states_json(cxx_qt_lib::QString::from(&new_json));

        // One-time credential migration (slice B): move legacy ufb_*
        // Credential Manager entries to server-keyed domain entries the
        // SMB redirector reads on its own. Must run before the local
        // mount tasks start so silent NULL-credential mounts find the
        // migrated secrets. No-op once the legacy set is gone.
        #[cfg(windows)]
        {
            let entries: Vec<(String, String)> = load_mount_configs()
                .into_iter()
                .filter(|c| !c.unmanaged && !c.credential_key.is_empty())
                .map(|c| (c.credential_key.clone(), c.nas_share_path.clone()))
                .collect();
            if !entries.is_empty() {
                let n = ufb_core::windows_mounts::migrate_legacy_credentials(&entries);
                if n > 0 {
                    log::info!("mount: migrated {} legacy credential entr(ies)", n);
                }
            }
        }

        // Auto-spawn the agent ONLY when something needs it: plain
        // mounts are GUI-owned (macOS F1 / Windows slice B), so the
        // agent exists purely as the sync VFS host. No sync mounts →
        // no agent process at all.
        let required = agent_required();
        self.as_mut().set_agent_required(required);
        AGENT_REQUIRED.store(required, std::sync::atomic::Ordering::SeqCst);
        if required {
            if let Err(e) = spawn_agent_if_needed() {
                log::warn!("mount: auto-launch agent failed: {}", e);
            }
        } else {
            log::info!("mount: no sync mounts configured — agent not spawned (GUI owns plain mounts)");
        }

        // Get a thread-safe handle for queueing events back onto Qt.
        let qt_handle: cxx_qt::CxxQtThread<qobject::Mount> = self.as_ref().qt_thread();

        let forwarder: ufb_core::events::MountEventsArc =
            Arc::new(MountEventsForwarder { qt_handle });

        // GUI-owned plain mounts (macOS F1 / Windows slice B):
        // reconcile local mount tasks with config. States flow through
        // the same forwarder the agent uses, so every downstream
        // surface is owner-agnostic.
        #[cfg(any(target_os = "macos", windows))]
        {
            let _ = shared_forwarder().set(forwarder.clone());
            crate::local_mounts::apply_config(
                local_specs_from_configs(),
                forwarder.clone(),
            );
        }

        let client = shared_client();

        // MountClient::start calls tokio::spawn — needs an active
        // runtime in scope. enter() returns a guard that activates
        // the runtime for the current thread; the spawn inside
        // captures the runtime handle and lives on it after we drop
        // the guard.
        let _guard = shared_runtime().enter();
        client.start(forwarder);

        // Agent keep-alive: the one-shot spawn above is raceable (a
        // dying agent's socket can accept one final connection and
        // fake "running"). Every 15s, if the agent is needed but the
        // IPC is down, re-run the idempotent spawn. Blocking work
        // (wait_for_agent polls up to 5s) stays on the blocking pool.
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(Duration::from_secs(15));
            loop {
                tick.tick().await;
                use std::sync::atomic::Ordering;
                if AGENT_REQUIRED.load(Ordering::SeqCst)
                    && !AGENT_CONNECTED.load(Ordering::SeqCst)
                {
                    log::info!("mount: agent required but unreachable — respawning");
                    let _ = tokio::task::spawn_blocking(|| {
                        if let Err(e) = spawn_agent_if_needed() {
                            log::warn!("mount: agent respawn failed: {}", e);
                        }
                    })
                    .await;
                }
            }
        });

        // Slice C: timeout sweep for the pending_commands map. Every
        // second, scan for entries older than COMMAND_TIMEOUT (10s)
        // and synthesize a timeout failure for each. Without this an
        // agent crash mid-command would leave a pending spinner
        // forever; this guarantees the UI heals after at most ~11s.
        let timeout_handle: cxx_qt::CxxQtThread<qobject::Mount> =
            self.as_ref().qt_thread();
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(Duration::from_secs(1));
            loop {
                tick.tick().await;
                let _ = timeout_handle.queue(|mut mount| {
                    use cxx_qt::CxxQtType;
                    let (pending_json, err_json, changed) = {
                        let rust_mut = mount.as_mut().rust_mut();
                        let rust = unsafe { rust_mut.get_unchecked_mut() };
                        let now = Instant::now();
                        let expired: Vec<String> = rust
                            .pending_commands
                            .iter()
                            .filter(|(_, p)| now.duration_since(p.started_at) > COMMAND_TIMEOUT)
                            .map(|(cid, _)| cid.clone())
                            .collect();
                        if expired.is_empty() {
                            return;
                        }
                        for cid in &expired {
                            if let Some(p) = rust.pending_commands.remove(cid) {
                                log::warn!(
                                    "[mount] command {} ({}) on {} timed out after 10s",
                                    cid, p.verb, p.mount_id
                                );
                                rust.last_command_errors.insert(
                                    p.mount_id.clone(),
                                    CommandError {
                                        message: "command timed out".to_string(),
                                        verb: p.verb,
                                        at_ms: ufb_core::utils::current_time_ms(),
                                    },
                                );
                            }
                        }
                        (
                            serialize_pending_commands(&rust.pending_commands),
                            serialize_command_errors(&rust.last_command_errors),
                            true,
                        )
                    };
                    if changed {
                        mount
                            .as_mut()
                            .set_pending_commands_json(cxx_qt_lib::QString::from(&pending_json));
                        mount
                            .as_mut()
                            .set_last_command_error_json(cxx_qt_lib::QString::from(&err_json));
                    }
                });
            }
        });

        log::info!("Mount QObject started — agent IPC connection loop spawned");
    }
}

/// Load mount configs and return the local volume_path for every
/// mount flagged `is_jobs_folder = true` AND `enabled = true`. Used
/// by `services::columns::is_shared_folder` to detect "are we
/// inside a Jobs folder mount?" without round-tripping through QML.
pub(crate) fn jobs_folder_volume_paths() -> Vec<String> {
    let cfg = mount_client::load_mount_config();
    let live = ufb_core::volumes::live_mount_roots();
    cfg.mounts
        .into_iter()
        .filter(|c| c.enabled && c.is_jobs_folder)
        .flat_map(|c| {
            // Unmanaged jobs folders: the configured path verbatim.
            if c.unmanaged {
                return vec![c.nas_share_path.clone()];
            }
            let name = share_name(&c.nas_share_path, &c.id);
            let derived = volumes_base()
                .join(&name)
                .to_string_lossy()
                .to_string();
            // Callers prefix-match browsing paths against this list, and
            // browsing paths follow the live mount location in
            // identityLiveRoots=live mode — so return BOTH spellings
            // (legacy ~/ufb/mounts alias + live /Volumes root) while the
            // transition is underway. plans/17 slice A→C bridge.
            let mut roots = vec![derived];
            if let Some(r) = ufb_core::volumes::share_leaf(&name)
                .and_then(|leaf| live.get(&leaf).cloned())
            {
                if !roots.contains(&r) {
                    roots.push(r);
                }
            }
            roots
        })
        .collect()
}

// ─────────────────────────────────────────────────────────────────────
// MountEventsForwarder — turns trait calls (on tokio threads) into
// queued mutations of the Mount QObject (on the Qt thread).
// ─────────────────────────────────────────────────────────────────────

struct MountEventsForwarder {
    qt_handle: cxx_qt::CxxQtThread<qobject::Mount>,
}

impl MountEvents for MountEventsForwarder {
    fn connection_changed(&self, connected: bool) {
        AGENT_CONNECTED.store(connected, std::sync::atomic::Ordering::SeqCst);
        let _ = self.qt_handle.queue(move |mut mount| {
            mount.as_mut().set_connected(connected);
        });
    }

    fn state_update(&self, update: &MountStateUpdateMsg) {
        let update = update.clone();
        let _ = self.qt_handle.queue(move |mut mount| {
            use cxx_qt::CxxQtType;
            // Update map + re-encode JSON merged with static configs.
            let new_json = {
                let rust_mut = mount.as_mut().rust_mut();
                let rust = unsafe { rust_mut.get_unchecked_mut() };

                // Letter stability (slice B): the first time a mount
                // lands on a letter with no configured preference,
                // that letter becomes its sticky mountDriveLetter so
                // it never drifts on later mounts/reboots. Applies to
                // both GUI-owned plain mounts and agent-reported sync
                // letters (same state stream). User-chosen letters are
                // never overwritten (only empty fields are filled).
                #[cfg(windows)]
                if update.state == "mounted" {
                    if let Some(letter) = update
                        .mounted_at
                        .as_deref()
                        .filter(|p| p.get(1..2) == Some(":"))
                        .and_then(|p| p.chars().next())
                        .filter(|c| c.is_ascii_alphabetic())
                    {
                        if let Some(cfg) = rust.configs.get_mut(&update.mount_id) {
                            if cfg.mount_drive_letter.trim().is_empty() {
                                persist_drive_letter(&update.mount_id, letter);
                                cfg.mount_drive_letter =
                                    letter.to_ascii_uppercase().to_string();
                                crate::local_mounts::note_assigned_letter(
                                    &update.mount_id,
                                    letter,
                                );
                            }
                        }
                    }
                }

                rust.states.insert(update.mount_id.clone(), update);
                publish_live_roots(&rust.states, &rust.configs);
                serialize_merged(&rust.states, &rust.configs)
            };
            mount.as_mut().set_mount_states_json(cxx_qt_lib::QString::from(&new_json));
        });
    }

    fn ack(&self, ack: &AckMsg) {
        // Slice C: Acks now surface to QML by clearing the matching
        // entry from pending_commands_json. A QML row whose mountId
        // was busy goes back to idle on the next binding rebind.
        let command_id = ack.command_id.clone();
        if command_id.is_empty() {
            return;
        }
        let _ = self.qt_handle.queue(move |mut mount| {
            use cxx_qt::CxxQtType;
            let new_json = {
                let rust_mut = mount.as_mut().rust_mut();
                let rust = unsafe { rust_mut.get_unchecked_mut() };
                rust.pending_commands.remove(&command_id);
                serialize_pending_commands(&rust.pending_commands)
            };
            mount
                .as_mut()
                .set_pending_commands_json(cxx_qt_lib::QString::from(&new_json));
        });
    }

    fn error(&self, err: &ErrorMsg) {
        log::warn!("[mount] error for {}: {}", err.command_id, err.message);
        // Slice C: surface the failure into last_command_error_json
        // and clear the pending entry, so QML can render an error
        // pill on the affected mount row.
        let command_id = err.command_id.clone();
        let message = err.message.clone();
        if command_id.is_empty() {
            return;
        }
        let _ = self.qt_handle.queue(move |mut mount| {
            use cxx_qt::CxxQtType;
            let (pending_json, err_json) = {
                let rust_mut = mount.as_mut().rust_mut();
                let rust = unsafe { rust_mut.get_unchecked_mut() };
                let entry = rust.pending_commands.remove(&command_id);
                if let Some(p) = entry {
                    rust.last_command_errors.insert(
                        p.mount_id.clone(),
                        CommandError {
                            message: message.clone(),
                            verb: p.verb,
                            at_ms: ufb_core::utils::current_time_ms(),
                        },
                    );
                }
                (
                    serialize_pending_commands(&rust.pending_commands),
                    serialize_command_errors(&rust.last_command_errors),
                )
            };
            mount
                .as_mut()
                .set_pending_commands_json(cxx_qt_lib::QString::from(&pending_json));
            mount
                .as_mut()
                .set_last_command_error_json(cxx_qt_lib::QString::from(&err_json));
        });
    }

    fn conflict_detected(&self, conflict: &ConflictDetectedMsg) {
        log::warn!(
            "[mount] conflict on {}/{} → {}",
            conflict.domain,
            conflict.original_path,
            conflict.conflict_path
        );
    }

    fn cache_stats(&self, stats: &CacheStatsMsg) {
        let stats = stats.clone();
        let _ = self.qt_handle.queue(move |mut mount| {
            use cxx_qt::CxxQtType;
            let new_json = {
                let rust_mut = mount.as_mut().rust_mut();
                let rust = unsafe { rust_mut.get_unchecked_mut() };
                rust.cache_stats.insert(stats.mount_id.clone(), stats);
                serialize_cache_stats(&rust.cache_stats)
            };
            mount.as_mut().set_cache_stats_json(cxx_qt_lib::QString::from(&new_json));
        });
    }

    // test_credentials_result: default no-op — the TestCredentials
    // probe UI was deleted in slice B (credentials are OS-owned). The
    // wire message stays parseable in core for one release.
}

/// JSON map of `mount_id -> {hydratedBytes, hydratedCount, updatedAtMs}`.
/// QML reads this off the `cache_stats_json` property and renders the
/// row for whichever mount it cares about.
fn serialize_cache_stats(stats: &HashMap<String, CacheStatsMsg>) -> String {
    let now = ufb_core::utils::current_time_ms();
    let map: serde_json::Map<String, serde_json::Value> = stats
        .iter()
        .map(|(id, s)| {
            (
                id.clone(),
                serde_json::json!({
                    "hydratedBytes": s.hydrated_bytes,
                    "hydratedCount": s.hydrated_count,
                    "updatedAtMs": now,
                }),
            )
        })
        .collect();
    serde_json::to_string(&serde_json::Value::Object(map)).unwrap_or_else(|_| "{}".into())
}

/// Push the current mounted locations into the core volume registry
/// so path-identity resolution can follow the live mounts
/// (`volumes::view_from_path_mappings_live`). Keyed by lowercased
/// share leaf; only mounts the agent reports as "mounted" with a
/// known location participate. Called after every states/configs
/// mutation — cheap (N ≤ dozen string ops) and idempotent.
fn publish_live_roots(
    states: &HashMap<String, MountStateUpdateMsg>,
    configs: &HashMap<String, MountInfo>,
) {
    let mut roots = std::collections::HashMap::new();
    for (id, st) in states {
        if st.state != "mounted" {
            continue;
        }
        let Some(mounted_at) = st.mounted_at.as_deref().filter(|s| !s.is_empty()) else {
            continue;
        };
        // Key by the configured share name (leaf of the UNC), NOT by
        // the leaf of mounted_at — a dedup-suffixed mount
        // (/Volumes/Share-1) must still register under "share".
        let Some(cfg) = configs.get(id) else { continue };
        if let Some(leaf) =
            ufb_core::volumes::share_leaf(&share_name(&cfg.nas_share_path, id))
        {
            roots.insert(leaf, mounted_at.to_string());
        }
    }
    ufb_core::volumes::set_live_mount_roots(roots);
}

/// Merge static configs (display name, derived volume path) with live
/// state into a JSON array QML consumes. Includes configured mounts
/// even before their first state update arrives — they show as
/// "(unknown)" / disabled-clickable until the agent reports.
fn serialize_merged(
    states: &HashMap<String, MountStateUpdateMsg>,
    configs: &HashMap<String, MountInfo>,
) -> String {
    // Field names rendered as JSON match what the QML readers expect
    // (mountId, displayName, volumePath, nasSharePath, stateDetail).
    // Without the rename, the lookup `s.mountId` in MountManagerDialog
    // stays undefined and the state pill renders as "unknown" / grey.
    //
    // nasSharePath is included so the sidebar can fall back to it
    // when the volumePath isn't computed yet (common right after
    // startup, before the agent has connected).
    #[derive(serde::Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Entry<'a> {
        mount_id: &'a str,
        display_name: String,
        volume_path: String,
        nas_share_path: String,
        credential_key: String,
        is_jobs_folder: bool,
        state: &'a str,
        state_detail: &'a str,
        /// Mount-drift notice ("" when the mount landed at its
        /// expected name) — rendered as a low-key ⚠ on mount rows.
        notice: &'a str,
        /// "Fake" mount: UI-only bookmark, no agent lifecycle. Rows
        /// render without state pill / lifecycle buttons and are
        /// always navigable.
        unmanaged: bool,
    }

    // Union of ids from states + configs so we render config-only and
    // state-only entries. Drop ids whose config is `enabled=false` so
    // the sidebar only shows mounts the user has actually turned on
    // (the MountManagerDialog reads mounts.json directly via
    // read_mount_configs and stays comprehensive). State-only ids are
    // kept — those exist briefly during transitions and are safer to
    // surface than to hide.
    let mut ids: std::collections::BTreeSet<&str> = std::collections::BTreeSet::new();
    for k in states.keys() {
        ids.insert(k.as_str());
    }
    for (k, cfg) in configs.iter() {
        if cfg.enabled {
            ids.insert(k.as_str());
        }
    }

    let entries: Vec<Entry<'_>> = ids
        .into_iter()
        .map(|id| {
            let cfg = configs.get(id);
            let st = states.get(id);
            let display_name = cfg
                .map(|c| {
                    if c.display_name.is_empty() {
                        c.id.clone()
                    } else {
                        c.display_name.clone()
                    }
                })
                .unwrap_or_else(|| id.to_string());
            // Unmanaged ("fake") mounts: the configured path IS the
            // destination — no agent, no derivation.
            //
            // Managed mounts: prefer the live location the agent
            // reported (where the mount actually landed —
            // /Volumes/<share> incl. dedup suffixes) over the
            // config-derived ~/ufb/mounts symlink path, so
            // Open/Reveal and navigation from a mount row use the
            // real mount, not the legacy alias. Falls back to the
            // derived path while unmounted / agent-disconnected.
            // plans/17 slice A→C bridge.
            let unmanaged = cfg.map(|c| c.unmanaged).unwrap_or(false);
            let volume_path = if unmanaged {
                cfg.map(|c| c.nas_share_path.clone()).unwrap_or_default()
            } else {
                st.filter(|s| s.state == "mounted")
                    .and_then(|s| s.mounted_at.clone())
                    .filter(|p| !p.is_empty())
                    .unwrap_or_else(|| {
                        // Not mounted yet / owner disconnected. Windows
                        // (slice B): the UNC is the only stable spelling
                        // — letters are assigned at mount time and the
                        // C:\Volumes\ufb symlink farm is gone. macOS
                        // keeps the legacy ~/ufb/mounts alias while the
                        // slice A→C bridge is underway.
                        cfg.map(|c| {
                            if cfg!(windows) {
                                c.nas_share_path.clone()
                            } else {
                                let name = share_name(&c.nas_share_path, &c.id);
                                volumes_base()
                                    .join(name)
                                    .to_string_lossy()
                                    .to_string()
                            }
                        })
                        .unwrap_or_default()
                    })
            };
            let nas_share_path = cfg
                .map(|c| c.nas_share_path.clone())
                .unwrap_or_default();
            let credential_key = cfg
                .map(|c| c.credential_key.clone())
                .unwrap_or_default();
            let is_jobs_folder = cfg.map(|c| c.is_jobs_folder).unwrap_or(false);
            Entry {
                mount_id: id,
                display_name,
                volume_path,
                nas_share_path,
                credential_key,
                is_jobs_folder,
                state: st.map(|s| s.state.as_str()).unwrap_or(""),
                state_detail: st.map(|s| s.state_detail.as_str()).unwrap_or(""),
                notice: st
                    .and_then(|s| s.notice.as_deref())
                    .unwrap_or(""),
                unmanaged,
            }
        })
        .collect();

    serde_json::to_string(&entries).unwrap_or_else(|_| "[]".to_string())
}
