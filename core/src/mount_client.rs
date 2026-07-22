//! IPC client to `ufb-agent` (was `mediamount-agent`).
//!
//! Phase 1d port: pipe/socket renamed (`MediaMountAgent` → `UfbAgent`,
//! `mediamount-agent.sock` → `ufb-agent.sock`), `tauri::Emitter` calls
//! replaced with the `MountEvents` trait.

use crate::events::MountEventsArc;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::sync::Arc;
use tokio::sync::Mutex;

#[cfg(windows)]
use std::os::windows::io::FromRawHandle;

const PIPE_NAME: &str = r"\\.\pipe\UfbAgent";

// ── Wire protocol types (must match agent/src/messages.rs) ──

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum AgentToUfb {
    MountStateUpdate(MountStateUpdateMsg),
    /// Full per-mount state snapshot. See agent/src/messages.rs for the
    /// canonical docstring. Clients replace their entire local mounts
    /// map on receipt — any mount missing from the snapshot has been
    /// removed and should be pruned.
    MountStateSnapshot(MountStateSnapshotMsg),
    Ack(AckMsg),
    Error(ErrorMsg),
    Pong,
    ConflictDetected(ConflictDetectedMsg),
    CacheStats(CacheStatsMsg),
    /// BadgeUpdate messages are consumed by the FinderSync extension
    /// (separate process); UFB ignores them.
    BadgeUpdate(serde_json::Value),
    TestCredentialsResult(TestCredentialsResultMsg),
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountStateSnapshotMsg {
    pub mounts: Vec<MountStateUpdateMsg>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TestCredentialsResultMsg {
    pub mount_id: String,
    /// One of: "ok", "auth_failed", "network_error", "no_credentials",
    /// "no_mount", "unsupported", "internal_error". See messages.rs
    /// in the agent crate for the canonical comment.
    pub kind: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub message: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub command_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CacheStatsMsg {
    pub mount_id: String,
    pub hydrated_bytes: u64,
    pub hydrated_count: u64,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub command_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ConflictDetectedMsg {
    pub domain: String,
    pub original_path: String,
    pub conflict_path: String,
    pub host: String,
    pub detected_at: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountStateUpdateMsg {
    pub mount_id: String,
    pub state: String,
    pub state_detail: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sync_state: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sync_state_detail: Option<String>,
    /// Where the mount actually landed on this machine (agent-side
    /// `orchestrator.mounted_at`): resolved `/Volumes/<share>` on macOS
    /// (including any `-1` dedup suffix), the UNC on Windows, the NFS
    /// mountpoint for sync mounts. Present only while state=="mounted".
    /// Feeds `volumes::set_live_mount_roots` so identity resolution
    /// follows the live mount instead of a static mapping.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mounted_at: Option<String>,
    /// Informational mount-drift notice (e.g. "Mounted as Share-1 —
    /// /Volumes/Share was taken"). UI renders it as a low-key ⚠ on
    /// the mount row; absence means the mount landed where expected.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub notice: Option<String>,
    /// True when the drift is caused by a stale leftover directory
    /// squatting the expected mountpoint (empty, not in the mount
    /// table) — i.e. UFB can offer a "Fix…" action that removes it
    /// with admin rights and remounts. False/absent for a live
    /// foreign occupant, which must never be touched.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub notice_fixable: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AckMsg {
    pub command_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ErrorMsg {
    pub command_id: String,
    pub message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum UfbToAgent {
    StartMount(MountIdMsg),
    StopMount(MountIdMsg),
    RestartMount(MountIdMsg),
    ClearSyncCache(MountIdMsg),
    GetCacheStats(MountIdMsg),
    CreateSymlinks,
    ReloadConfig,
    GetStates,
    Ping,
    /// Tell the agent to drain mounts and exit. Sent from UFB's
    /// `aboutToQuit` so closing UFB also shuts down the agent.
    Quit,
    /// User-driven freshness signal (window focus, refresh button).
    /// On macOS, agent posts a Darwin notification so FileProvider drains
    /// any pending evictions for opportunistic-hook-detected drift.
    FreshnessSweep(FreshnessSweepMsg),
    TestCredentials(MountIdMsg),
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountIdMsg {
    pub mount_id: String,
    #[serde(default)]
    pub command_id: String,
    /// Start/Restart only: this attempt came from an explicit user
    /// action (Fix-credentials pill), so the mount may present the OS
    /// auth dialog (macOS NetAuthAgent). Background attempts leave it
    /// false and fail silently to the auth_error pill. plans/17 slice C.
    #[serde(default)]
    pub allow_ui: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct FreshnessSweepMsg {
    /// Optional domain / share name. `None` = sweep all enabled mounts.
    #[serde(default)]
    pub domain: Option<String>,
    #[serde(default)]
    pub command_id: String,
}

// ── Config types (must match agent/src/config.rs) ──

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountsConfig {
    pub version: u32,
    pub mounts: Vec<MountConfig>,
    #[serde(default)]
    pub sync_cache_root: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountConfig {
    pub id: String,
    #[serde(default = "default_true")]
    pub enabled: bool,
    pub display_name: String,
    pub nas_share_path: String,
    /// "Fake" mount: a bookmark that lives in the Mounts UI. The agent
    /// never manages it; nas_share_path is a native path used verbatim.
    /// Must exist here (not just agent-side) or the GUI's typed
    /// save path would silently drop it from mounts.json.
    #[serde(default)]
    pub unmanaged: bool,
    /// Legacy credential-store key. New mounts stopped writing it in
    /// slice B (credentials are OS-owned, keyed by server) so it must
    /// default — a required field here rejects every Add-dialog save.
    #[serde(default)]
    pub credential_key: String,
    #[serde(default)]
    pub mount_drive_letter: String,
    #[serde(default)]
    pub smb_mount_path: Option<String>,
    #[serde(default)]
    pub mount_path_linux: Option<String>,
    #[serde(default = "default_true")]
    pub is_jobs_folder: bool,

    // On-demand sync (Windows Cloud Files API / WinFsp)
    #[serde(default)]
    pub sync_enabled: bool,
    #[serde(default)]
    pub sync_root_path: Option<String>,
    #[serde(default)]
    pub sync_cache_limit_bytes: u64,

    // Legacy fields — kept for backwards compat with existing config files.
    #[serde(default)]
    pub rclone_drive_letter: String,
    #[serde(default)]
    pub smb_drive_letter: String,
    #[serde(default)]
    pub junction_path: String,
    #[serde(default)]
    pub rclone_mount_path: Option<String>,
    #[serde(default)]
    pub rclone_remote: Option<String>,
    #[serde(default)]
    pub cache_dir_path: String,
    #[serde(default)]
    pub cache_max_size: String,
    #[serde(default)]
    pub cache_max_age: String,
    #[serde(default)]
    pub vfs_write_back: String,
    #[serde(default)]
    pub vfs_read_chunk_size: String,
    #[serde(default)]
    pub vfs_read_chunk_streams: u32,
    #[serde(default)]
    pub vfs_read_ahead: String,
    #[serde(default)]
    pub buffer_size: String,
    #[serde(default)]
    pub probe_interval_secs: u64,
    #[serde(default)]
    pub probe_timeout_ms: u64,
    #[serde(default)]
    pub fallback_threshold: u32,
    #[serde(default)]
    pub recovery_threshold: u32,
    #[serde(default)]
    pub max_rclone_start_attempts: u32,
    #[serde(default)]
    pub healthcheck_file_name: String,
    #[serde(default)]
    pub extra_rclone_flags: Vec<String>,
}

fn default_true() -> bool {
    true
}

// ── IPC framing ──

fn write_message<W: Write>(writer: &mut W, payload: &[u8]) -> io::Result<()> {
    let len = payload.len() as u32;
    writer.write_all(&len.to_le_bytes())?;
    writer.write_all(payload)?;
    Ok(())
}

fn read_message<R: Read>(reader: &mut R) -> io::Result<Vec<u8>> {
    let mut len_buf = [0u8; 4];
    reader.read_exact(&mut len_buf)?;
    let len = u32::from_le_bytes(len_buf) as usize;
    if len > 16 * 1024 * 1024 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("message too large: {} bytes", len),
        ));
    }
    let mut buf = vec![0u8; len];
    reader.read_exact(&mut buf)?;
    Ok(buf)
}

fn send_msg<W: Write>(writer: &mut W, msg: &UfbToAgent) -> io::Result<()> {
    let payload =
        serde_json::to_vec(msg).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
    write_message(writer, &payload)
}

fn recv_msg<R: Read>(reader: &mut R) -> io::Result<AgentToUfb> {
    let payload = read_message(reader)?;
    serde_json::from_slice(&payload).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}

// ── Mount Client ──

pub struct MountClient {
    state: Mutex<MountClientState>,
    /// Channel for sending commands to the connection loop, which writes them to the pipe.
    cmd_tx: tokio::sync::mpsc::Sender<UfbToAgent>,
    cmd_rx: Mutex<Option<tokio::sync::mpsc::Receiver<UfbToAgent>>>,
}

struct MountClientState {
    /// Last known mount states, keyed by mount_id
    mount_states: HashMap<String, MountStateUpdateMsg>,
    /// Whether we're connected to the agent
    connected: bool,
}

impl Default for MountClient {
    fn default() -> Self {
        Self::new()
    }
}

impl MountClient {
    pub fn new() -> Self {
        let (cmd_tx, cmd_rx) = tokio::sync::mpsc::channel(64);
        Self {
            state: Mutex::new(MountClientState {
                mount_states: HashMap::new(),
                connected: false,
            }),
            cmd_tx,
            cmd_rx: Mutex::new(Some(cmd_rx)),
        }
    }

    /// Start the background connection task.
    pub fn start(self: &Arc<Self>, events: MountEventsArc) {
        let client = Arc::clone(self);
        tokio::spawn(async move {
            client.connection_loop(events).await;
        });
    }

    /// Get current mount states.
    pub async fn get_states(&self) -> HashMap<String, MountStateUpdateMsg> {
        self.state.lock().await.mount_states.clone()
    }

    /// Check if connected to agent.
    pub async fn is_connected(&self) -> bool {
        self.state.lock().await.connected
    }

    /// Quick synchronous check if the agent is reachable.
    pub fn is_agent_running(&self) -> bool {
        #[cfg(windows)]
        {
            use windows::core::HSTRING;
            use windows::Win32::System::Pipes::WaitNamedPipeW;
            let pipe_name = HSTRING::from(PIPE_NAME);
            unsafe { WaitNamedPipeW(&pipe_name, 100) }.as_bool()
        }
        #[cfg(unix)]
        {
            // Stat alone is unsafe: a Unix domain socket file persists
            // after the agent dies, so `exists()` would falsely report
            // a dead agent as live and short-circuit the heal-on-open
            // chain. Actually attempt a connect — a stale socket gives
            // ECONNREFUSED immediately.
            connect_to_agent_unix().is_ok()
        }
    }

    /// Send a command to the agent via the persistent connection.
    pub async fn send_command(&self, cmd: UfbToAgent) -> Result<(), String> {
        self.cmd_tx
            .send(cmd)
            .await
            .map_err(|e| format!("Failed to queue command: {}", e))
    }

    async fn connection_loop(&self, events: MountEventsArc) {
        let mut cmd_rx = self
            .cmd_rx
            .lock()
            .await
            .take()
            .expect("connection_loop called twice");

        loop {
            log::info!("MountClient: connecting to agent...");

            // Offload the (potentially-blocking) connect to the blocking pool.
            #[cfg(windows)]
            let connect_result = tokio::task::spawn_blocking(connect_to_agent)
                .await
                .map_err(|e| {
                    io::Error::other(format!("connect_to_agent join error: {}", e))
                })
                .and_then(|r| r);
            #[cfg(windows)]
            match connect_result {
                Ok(pipe) => {
                    log::info!("MountClient: connected to agent");
                    {
                        let mut state = self.state.lock().await;
                        state.connected = true;
                    }
                    events.connection_changed(true);

                    // Single blocking I/O thread handles both read and write —
                    // concurrent ReadFile/WriteFile on a synchronous Windows
                    // named-pipe handle deadlocks.
                    let (msg_tx, mut msg_rx) =
                        tokio::sync::mpsc::channel::<AgentToUfb>(64);
                    let (write_tx, write_rx) = std::sync::mpsc::channel::<UfbToAgent>();

                    let _ = write_tx.send(UfbToAgent::GetStates);

                    tokio::task::spawn_blocking(move || {
                        log::info!("MountClient: I/O thread started");
                        let mut pipe_rw = pipe;
                        loop {
                            // 1. Write any pending outgoing commands
                            while let Ok(cmd) = write_rx.try_recv() {
                                if let Err(e) = send_msg(&mut pipe_rw, &cmd) {
                                    log::error!("MountClient: write failed: {}", e);
                                    return;
                                }
                            }

                            // 2. Peek for incoming data
                            let mut available = 0u32;
                            let peek_ok = unsafe {
                                windows::Win32::System::Pipes::PeekNamedPipe(
                                    windows::Win32::Foundation::HANDLE(
                                        std::os::windows::io::AsRawHandle::as_raw_handle(
                                            &pipe_rw,
                                        ),
                                    ),
                                    None,
                                    0,
                                    None,
                                    Some(&mut available),
                                    None,
                                )
                            };
                            if peek_ok.is_err() {
                                log::info!("MountClient: pipe closed (peek failed)");
                                return;
                            }

                            if available > 0 {
                                match recv_msg(&mut pipe_rw) {
                                    Ok(msg) => {
                                        if msg_tx.blocking_send(msg).is_err() {
                                            return;
                                        }
                                    }
                                    Err(_) => {
                                        log::info!("MountClient: pipe closed (read failed)");
                                        return;
                                    }
                                }
                            } else {
                                std::thread::sleep(std::time::Duration::from_millis(10));
                            }
                        }
                    });

                    let write_tx_clone = write_tx.clone();

                    loop {
                        tokio::select! {
                            msg = msg_rx.recv() => {
                                match msg {
                                    Some(AgentToUfb::MountStateUpdate(update)) => {
                                        log::info!("MountClient: state update {} ({})", update.mount_id, update.state);
                                        let mount_id = update.mount_id.clone();
                                        {
                                            let mut state = self.state.lock().await;
                                            state.mount_states.insert(mount_id, update.clone());
                                        }
                                        events.state_update(&update);
                                    }
                                    Some(AgentToUfb::MountStateSnapshot(snapshot)) => {
                                        log::info!(
                                            "MountClient: state snapshot — {} mount(s)",
                                            snapshot.mounts.len()
                                        );
                                        // Authoritative replacement: drop
                                        // everything not in the snapshot.
                                        {
                                            let mut state = self.state.lock().await;
                                            state.mount_states.clear();
                                            for entry in &snapshot.mounts {
                                                state.mount_states.insert(
                                                    entry.mount_id.clone(),
                                                    entry.clone(),
                                                );
                                            }
                                        }
                                        events.state_snapshot(&snapshot);
                                    }
                                    Some(AgentToUfb::Pong) => {}
                                    Some(AgentToUfb::Ack(ack)) => {
                                        events.ack(&ack);
                                    }
                                    Some(AgentToUfb::Error(err)) => {
                                        events.error(&err);
                                    }
                                    Some(AgentToUfb::ConflictDetected(conflict)) => {
                                        log::warn!(
                                            "[mount] Conflict on {}/{} → {}",
                                            conflict.domain, conflict.original_path, conflict.conflict_path
                                        );
                                        events.conflict_detected(&conflict);
                                    }
                                    Some(AgentToUfb::CacheStats(stats)) => {
                                        events.cache_stats(&stats);
                                    }
                                    Some(AgentToUfb::TestCredentialsResult(r)) => {
                                        events.test_credentials_result(&r);
                                    }
                                    Some(AgentToUfb::BadgeUpdate(_)) => {
                                        // Consumed by FinderSync extension; UFB ignores.
                                    }
                                    None => {
                                        break;
                                    }
                                }
                            }
                            Some(cmd) = cmd_rx.recv() => {
                                if write_tx_clone.send(cmd).is_err() {
                                    log::error!("MountClient: I/O thread gone, can't send command");
                                    break;
                                }
                            }
                        }
                    }

                    log::info!("MountClient: disconnected from agent");
                    {
                        let mut state = self.state.lock().await;
                        state.connected = false;
                    }
                    events.connection_changed(false);
                }
                Err(e) => {
                    log::debug!("MountClient: agent not available: {}", e);
                }
            }

            #[cfg(unix)]
            match connect_to_agent_unix() {
                Ok(stream) => {
                    log::info!("MountClient: connected to agent (unix socket)");
                    {
                        let mut state = self.state.lock().await;
                        state.connected = true;
                    }
                    events.connection_changed(true);

                    let mut write_stream = match stream.try_clone() {
                        Ok(s) => s,
                        Err(e) => {
                            log::error!("MountClient: failed to clone unix stream: {}", e);
                            continue;
                        }
                    };

                    let _ = send_msg(&mut write_stream, &UfbToAgent::GetStates);

                    let (msg_tx, mut msg_rx) = tokio::sync::mpsc::channel(64);
                    tokio::task::spawn_blocking(move || {
                        let mut reader = stream;
                        loop {
                            match recv_msg(&mut reader) {
                                Ok(msg) => {
                                    if msg_tx.blocking_send(msg).is_err() {
                                        break;
                                    }
                                }
                                Err(_) => break,
                            }
                        }
                    });

                    loop {
                        tokio::select! {
                            msg = msg_rx.recv() => {
                                match msg {
                                    Some(AgentToUfb::MountStateUpdate(update)) => {
                                        log::info!("MountClient: received state update for {} ({})", update.mount_id, update.state);
                                        let mount_id = update.mount_id.clone();
                                        {
                                            let mut state = self.state.lock().await;
                                            state.mount_states.insert(mount_id, update.clone());
                                        }
                                        events.state_update(&update);
                                    }
                                    Some(AgentToUfb::MountStateSnapshot(snapshot)) => {
                                        log::info!(
                                            "MountClient: state snapshot — {} mount(s)",
                                            snapshot.mounts.len()
                                        );
                                        {
                                            let mut state = self.state.lock().await;
                                            state.mount_states.clear();
                                            for entry in &snapshot.mounts {
                                                state.mount_states.insert(
                                                    entry.mount_id.clone(),
                                                    entry.clone(),
                                                );
                                            }
                                        }
                                        events.state_snapshot(&snapshot);
                                    }
                                    Some(AgentToUfb::Pong) => {}
                                    Some(AgentToUfb::Ack(ack)) => {
                                        events.ack(&ack);
                                    }
                                    Some(AgentToUfb::Error(err)) => {
                                        events.error(&err);
                                    }
                                    Some(AgentToUfb::ConflictDetected(conflict)) => {
                                        log::warn!(
                                            "[mount] Conflict on {}/{} → {}",
                                            conflict.domain, conflict.original_path, conflict.conflict_path
                                        );
                                        events.conflict_detected(&conflict);
                                    }
                                    Some(AgentToUfb::CacheStats(stats)) => {
                                        events.cache_stats(&stats);
                                    }
                                    Some(AgentToUfb::TestCredentialsResult(r)) => {
                                        events.test_credentials_result(&r);
                                    }
                                    Some(AgentToUfb::BadgeUpdate(_)) => {
                                        // Consumed by FinderSync extension; UFB ignores.
                                    }
                                    None => break,
                                }
                            }
                            Some(cmd) = cmd_rx.recv() => {
                                if let Err(e) = send_msg(&mut write_stream, &cmd) {
                                    log::error!("MountClient: failed to send command: {}", e);
                                    break;
                                }
                            }
                        }
                    }

                    log::info!("MountClient: disconnected from agent");
                    {
                        let mut state = self.state.lock().await;
                        state.connected = false;
                    }
                    events.connection_changed(false);
                }
                Err(e) => {
                    log::debug!("MountClient: agent not available: {}", e);
                }
            }

            // Drain any commands that queued while disconnected
            while cmd_rx.try_recv().is_ok() {}

            static RETRY_COUNT: std::sync::atomic::AtomicU32 =
                std::sync::atomic::AtomicU32::new(0);
            let count = RETRY_COUNT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let delay = if count < 5 { 500 } else { 3000 };
            tokio::time::sleep(std::time::Duration::from_millis(delay)).await;
        }
    }
}

/// Config file path: %LOCALAPPDATA%/ufb/mounts.json
pub fn config_file_path() -> Option<std::path::PathBuf> {
    #[cfg(windows)]
    {
        if let Ok(local) = std::env::var("LOCALAPPDATA") {
            let dir = std::path::PathBuf::from(local).join("ufb");
            let _ = std::fs::create_dir_all(&dir);
            return Some(dir.join("mounts.json"));
        }
    }
    #[cfg(not(windows))]
    {
        if let Some(home) = std::env::var_os("HOME") {
            let dir = std::path::PathBuf::from(home).join(".local/share/ufb");
            let _ = std::fs::create_dir_all(&dir);
            return Some(dir.join("mounts.json"));
        }
    }
    None
}

pub fn load_mount_config() -> MountsConfig {
    let path = match config_file_path() {
        Some(p) => p,
        None => {
            return MountsConfig {
                version: 1,
                mounts: vec![],
                sync_cache_root: None,
            }
        }
    };
    if !path.exists() {
        return MountsConfig {
            version: 1,
            mounts: vec![],
            sync_cache_root: None,
        };
    }
    match std::fs::read_to_string(&path) {
        Ok(contents) => serde_json::from_str(&contents).unwrap_or(MountsConfig {
            version: 1,
            mounts: vec![],
            sync_cache_root: None,
        }),
        Err(_) => MountsConfig {
            version: 1,
            mounts: vec![],
            sync_cache_root: None,
        },
    }
}

pub fn save_mount_config(config: &MountsConfig) -> Result<(), String> {
    let path = config_file_path().ok_or_else(|| "Could not determine config file path".to_string())?;
    let json = serde_json::to_string_pretty(config)
        .map_err(|e| format!("Failed to serialize config: {}", e))?;
    std::fs::write(&path, json).map_err(|e| format!("Failed to write config: {}", e))?;
    Ok(())
}

/// Shared socket-path resolution — mirrors `unix_server::socket_path()`
/// in the agent. macOS puts it inside the App Group container so the
/// sandboxed FinderSync extension can reach it.
///
/// MUST stay in lock-step with `agent/src/platform/macos/mod.rs`'s
/// `APP_GROUP_ID` constant. We duplicate the string here because the
/// `agent` crate is excluded from the workspace and `core` can't depend
/// on it.
#[cfg(unix)]
fn agent_socket_path() -> std::path::PathBuf {
    #[cfg(target_os = "macos")]
    {
        if let Some(home) = std::env::var_os("HOME") {
            // Short filename `a.sock` keeps the full path under
            // macOS's 104-byte sun_path limit.
            return std::path::PathBuf::from(home).join(
                "Library/Group Containers/5Z4S9VHV56.group.com.unionfiles.ufb/a.sock",
            );
        }
    }
    if let Ok(runtime_dir) = std::env::var("XDG_RUNTIME_DIR") {
        std::path::PathBuf::from(runtime_dir).join("ufb/ufb-agent.sock")
    } else {
        std::path::PathBuf::from("/tmp/ufb-agent.sock")
    }
}

#[cfg(unix)]
fn connect_to_agent_unix() -> io::Result<std::os::unix::net::UnixStream> {
    std::os::unix::net::UnixStream::connect(agent_socket_path())
}

#[cfg(windows)]
fn connect_to_agent() -> io::Result<std::fs::File> {
    use windows::core::HSTRING;
    use windows::Win32::Foundation::INVALID_HANDLE_VALUE;
    use windows::Win32::Storage::FileSystem::{
        CreateFileW, FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_SHARE_NONE, OPEN_EXISTING,
    };
    use windows::Win32::System::Pipes::WaitNamedPipeW;

    let pipe_name = HSTRING::from(PIPE_NAME);

    let wait_ok = unsafe { WaitNamedPipeW(&pipe_name, 2000) }.as_bool();
    if !wait_ok {
        return Err(io::Error::new(
            io::ErrorKind::ConnectionRefused,
            "agent pipe not available",
        ));
    }

    let handle = unsafe {
        CreateFileW(
            &pipe_name,
            (FILE_GENERIC_READ | FILE_GENERIC_WRITE).0,
            FILE_SHARE_NONE,
            None,
            OPEN_EXISTING,
            Default::default(),
            None,
        )
    };

    match handle {
        Ok(h) if h != INVALID_HANDLE_VALUE => {
            let file = unsafe { std::fs::File::from_raw_handle(h.0) };
            Ok(file)
        }
        Ok(_) => Err(io::Error::last_os_error()),
        Err(e) => Err(io::Error::new(
            io::ErrorKind::ConnectionRefused,
            format!("CreateFileW failed: {}", e),
        )),
    }
}
