use crate::config::MountConfig;
use crate::messages::{AgentToUfb, MountStateUpdateMsg};
use crate::state::{self, Effect, LogLevel, MountError, MountEvent, MountState, SyncPhase};
use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex, RwLock};
#[cfg(target_os = "macos")]
use std::sync::atomic::{AtomicU16, Ordering};
use tokio::sync::mpsc;

/// Per-orchestrator NFS port assignment. Bumped on first sync-spawn
/// inside each orchestrator and held for the lifetime of the
/// `MountInstance`. Starts at `BASE_PORT` and increments — wraps
/// only after ~50k restarts (`u16::MAX - BASE_PORT`), which is fine
/// for a long-lived agent.
#[cfg(target_os = "macos")]
static NEXT_NFS_PORT: AtomicU16 =
    AtomicU16::new(crate::sync::nfs_server::BASE_PORT);

/// Per-mount orchestrator. Receives events, runs transitions, dispatches effects.
/// Mount-drift notice (plans/17 slice C): Some(text) when NetFS parked
/// the share at a dedup-suffixed name because /Volumes/<leaf> was
/// taken by a live foreign occupant (a dead squatter would have been
/// force-unmounted pre-flight). Identity resolution follows the real
/// location either way — this is purely so the odd path isn't
/// mysterious in the UI.
#[cfg(target_os = "macos")]
fn drift_notice(config: &MountConfig, mounted_path: &str) -> Option<String> {
    let expected = config
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
    if expected.is_empty()
        || actual.is_empty()
        || actual.eq_ignore_ascii_case(expected)
    {
        return None;
    }
    Some(format!(
        "Mounted as \"{}\" — /Volumes/{} was taken by another volume",
        actual, expected
    ))
}

pub struct Orchestrator {
    pub mount_id: String,
    /// One-shot "the next mount attempt may show the OS auth dialog"
    /// permit. Set by MountService when a Start/Restart command arrives
    /// with `allow_ui: true` (the sidebar's Fix-credentials pill);
    /// consumed (swap-to-false) by the next macOS `mount_drive`. Auto
    /// attempts (boot, heartbeat reconnect, backoff retry) never set it,
    /// so they stay silent and fail to the `auth_error` pill instead.
    /// plans/17 slice C.
    ui_permit: Arc<std::sync::atomic::AtomicBool>,
    state: MountState,
    config: MountConfig,
    /// Global cache root for sync mounts.
    cache_root: std::path::PathBuf,
    event_tx: mpsc::Sender<MountEvent>,
    event_rx: mpsc::Receiver<MountEvent>,
    ipc_tx: mpsc::Sender<AgentToUfb>,
    /// Servers with active SMB sessions — shared across all orchestrators.
    /// If a server is already connected, skip credential lookup and reuse the session.
    connected_servers: Arc<Mutex<HashSet<String>>>,
    /// Per-domain VFS caches shared with MountService for UI drain/stats.
    /// `None` on Linux (no VFS server). Orchestrators populate this when
    /// they spawn their sync server and clear their entry on teardown.
    #[cfg(any(target_os = "macos", windows))]
    shared_caches: Option<crate::sync::SharedCaches>,
    /// Live VFS server handle for sync-enabled mounts. Populated by the
    /// `Effect::SpawnSyncServer` arm after a successful mount; consumed
    /// by `Effect::TeardownSyncServer`. `None` when the mount is not
    /// sync-enabled or between mount cycles.
    #[cfg(any(target_os = "macos", windows))]
    sync_handle: Option<crate::sync::SyncServerHandle>,
    /// Resolved backing filesystem path for the most recent successful
    /// MountDrive. macOS: `/Volumes/<share>` (or dedup variant) returned
    /// by `macos_smb_mount`. Windows: UNC path from config.
    /// Used by `SpawnSyncServer` to bind the VFS server to a known-good
    /// path — eliminates the 5s sleep + 60s poll dance that used to live
    /// in `main.rs::spawn_nfs_servers`.
    #[cfg(any(target_os = "macos", windows))]
    mounted_at: Option<std::path::PathBuf>,
    /// NFS port reserved for this orchestrator's sync server. Picked
    /// once on first `SpawnSyncServer` and held for the orchestrator's
    /// lifetime so Stop/Start cycles don't reshuffle ports between
    /// adjacent mounts.
    /// Human-readable mount-drift notice (plans/17 slice C): set when
    /// the share mounted at a dedup-suffixed name because the expected
    /// /Volumes/<share> was taken by a live foreign occupant. Rides on
    /// MountStateUpdateMsg.notice so mount rows can annotate the odd
    /// path instead of leaving it mysterious.
    #[cfg(target_os = "macos")]
    mount_notice: Option<String>,
    #[cfg(target_os = "macos")]
    nfs_port: Option<u16>,
    /// Shared NAS reachability atomic — written by the orchestrator's
    /// heartbeat handler (Slice D), read by PassthroughFs to short-
    /// circuit SMB-touching ops with JUKEBOX when offline. None when
    /// no sync server is currently live; populated by spawn_sync_server.
    #[cfg(target_os = "macos")]
    nas_health: Option<Arc<crate::sync::nas_health::NasHealth>>,
    /// ProjFS provider handle (Windows only). ProjFS is started from main.rs;
    /// the orchestrator only tracks sync state for status reporting.
    /// Sync sub-state lives in `state` as `MountState::Mounted(SyncPhase::*)`
    /// after Slice A — the separate `sync_state` field is gone.
    #[cfg(windows)]
    _projfs_active: bool,
    /// Windows twin of `nas_health`: written by the heartbeat handler,
    /// read by the WinFsp provider to short-circuit SMB ops offline.
    #[cfg(windows)]
    win_nas_health: Option<std::sync::Arc<crate::sync::nas_health::NasHealth>>,
    /// Consecutive Error-state auto-retry attempts since the last
    /// successful mount; drives the exponential backoff.
    error_retries: u32,
    /// When the next Error-state auto-retry is allowed to fire.
    next_error_retry_at: Option<std::time::Instant>,
    /// Last heartbeat-tick instant — a gap far beyond the 30s cadence
    /// means the machine slept; used to log the wake and reset backoff.
    last_heartbeat_at: Option<std::time::Instant>,
    /// User-facing drive-letter root (`X:\`) of the live WinFsp mount
    /// (slice B). `mounted_at` stays the UNC backing root the VFS
    /// reads through; serialize_state advertises THIS for sync mounts.
    #[cfg(windows)]
    sync_letter_root: Option<String>,
    /// Shared last-known-state cache. Written on every emit_state_update
    /// so MountService can build authoritative MountStateSnapshot
    /// messages without polling each orchestrator.
    state_cache: Arc<RwLock<HashMap<String, MountStateUpdateMsg>>>,
    /// Fires (with Err) when the paired sender held by MountService's
    /// `MountInstance` is dropped — i.e. the agent is shutting down or
    /// the mount was removed from config. We can't use `event_rx`'s
    /// None for this because the orchestrator keeps its own `event_tx`
    /// clone for self-emitted events, so the channel never closes
    /// from event_rx's perspective.
    shutdown_signal: Option<tokio::sync::oneshot::Receiver<()>>,
}

impl Orchestrator {
    pub fn new(
        config: MountConfig,
        cache_root: std::path::PathBuf,
        ipc_tx: mpsc::Sender<AgentToUfb>,
        connected_servers: Arc<Mutex<HashSet<String>>>,
        #[cfg(any(target_os = "macos", windows))]
        shared_caches: Option<crate::sync::SharedCaches>,
        state_cache: Arc<RwLock<HashMap<String, MountStateUpdateMsg>>>,
        shutdown_signal: tokio::sync::oneshot::Receiver<()>,
    ) -> Self {
        let (event_tx, event_rx) = mpsc::channel(64);
        let mount_id = config.id.clone();

        Self {
            mount_id,
            ui_permit: Arc::new(std::sync::atomic::AtomicBool::new(false)),
            state: MountState::Initializing,
            config,
            cache_root,
            event_tx,
            event_rx,
            ipc_tx,
            connected_servers,
            #[cfg(any(target_os = "macos", windows))]
            shared_caches,
            #[cfg(any(target_os = "macos", windows))]
            sync_handle: None,
            #[cfg(any(target_os = "macos", windows))]
            mounted_at: None,
            #[cfg(target_os = "macos")]
            mount_notice: None,
            #[cfg(target_os = "macos")]
            nfs_port: None,
            #[cfg(target_os = "macos")]
            nas_health: None,
            #[cfg(windows)]
            _projfs_active: false,
            #[cfg(windows)]
            win_nas_health: None,
            #[cfg(windows)]
            sync_letter_root: None,
            error_retries: 0,
            next_error_retry_at: None,
            last_heartbeat_at: None,
            state_cache,
            shutdown_signal: Some(shutdown_signal),
        }
    }

    /// Get a sender for sending events to this orchestrator.
    pub fn event_sender(&self) -> mpsc::Sender<MountEvent> {
        self.event_tx.clone()
    }

    /// Shared handle to the one-shot UI permit (see field docs).
    /// MountService stores this on the MountInstance and sets it when a
    /// Start/Restart command arrives with `allow_ui: true`.
    pub fn ui_permit_handle(&self) -> Arc<std::sync::atomic::AtomicBool> {
        Arc::clone(&self.ui_permit)
    }

    /// Run the orchestrator event loop. Blocks until stopped.
    pub async fn run(&mut self) {
        log::info!(
            "[{}] Orchestrator started (state: {})",
            self.mount_id,
            self.state
        );

        // Auto-start
        self.handle_event(MountEvent::Start).await;

        // If mounting succeeded (no error), transition to Mounted
        if matches!(self.state, MountState::Mounting) {
            self.handle_event(MountEvent::RequestStateUpdate).await;
        }

        // Periodic sync activity update (2s) and NAS heartbeat (30s)
        let mut sync_tick = tokio::time::interval(std::time::Duration::from_secs(2));
        let mut heartbeat_tick = tokio::time::interval(std::time::Duration::from_secs(30));
        // Non-blocking heartbeat: result arrives via channel so select! stays responsive
        let (hb_tx, mut hb_rx) = mpsc::channel::<bool>(1);

        // Held by the matching MountInstance over in MountService —
        // dropped when the agent shuts down (mount_service.shutdown)
        // or when the mount is removed from config. Polling it in the
        // select! is the only reliable signal that "no external party
        // still wants this orchestrator," since event_tx is kept alive
        // by the orchestrator itself for self-emitted events.
        let mut shutdown_signal = self
            .shutdown_signal
            .take()
            .expect("orchestrator run() called twice");

        loop {
            tokio::select! {
                // Biased so a pending Stop / Restart / etc. is always
                // processed BEFORE we exit on shutdown_signal. Without
                // this, MountService::shutdown's "send Stop, then drop
                // the MountInstance" sequence races: half the time
                // shutdown_signal wins and TeardownSyncServer is
                // skipped.
                biased;

                event = self.event_rx.recv() => {
                    match event {
                        Some(MountEvent::ClearSyncCache) => {
                            // ProjFS / NFS: cache drain is handled inline by
                            // mount_service::try_drain_cache via SharedCaches.
                            // Nothing to do here.
                        }
                        Some(event) => {
                            self.handle_event(event).await;

                            // After restart or start, if we're in
                            // Mounting state and no error, transition
                            // to Mounted by self-emitting a synthetic
                            // RequestStateUpdate.
                            //
                            // Pre-refactor we broke out of the loop on
                            // MountState::Stopped, which left the
                            // event channel dead — subsequent
                            // Start/Restart silently no-op'd because
                            // the receiver was dropped. The orchestrator
                            // now stays alive for the lifetime of its
                            // MountInstance; tear-down happens only on
                            // (a) agent shutdown or (b) config removal
                            // (channel close via the `None` arm below).
                            if matches!(self.state, MountState::Mounting) {
                                self.handle_event(MountEvent::RequestStateUpdate).await;
                            }
                        }
                        None => {
                            // Channel closed — mount removed from
                            // config or agent shutting down. This is
                            // the ONLY exit path for the orchestrator
                            // task now.
                            log::info!(
                                "[{}] Event channel closed, orchestrator exiting",
                                self.mount_id
                            );
                            break;
                        }
                    }
                }
                // Periodic sync activity update
                _ = sync_tick.tick() => {
                    #[cfg(any(target_os = "macos", windows))]
                    if self.is_sync_active() {
                        self.emit_state_update().await;
                    }
                }
                // NAS heartbeat — fire-and-forget with 10s timeout, result comes via hb_rx.
                // Slice D: cross-platform now (was Windows-only). macOS uses
                // mounted_at (resolved /Volumes/<share>) so the stat hits the
                // actual SMB mount; falling back to nas_share_path keeps
                // Windows working (its mounted_at is the UNC).
                _ = heartbeat_tick.tick() => {
                    // Sleep/wake detection: interval ticks can't fire
                    // while the machine sleeps, so a gap far beyond the
                    // 30s cadence means we just woke. The SMB session is
                    // almost certainly dead; the probe below detects it
                    // this same tick. Reset the error backoff so a wake
                    // into a healthy network retries immediately.
                    let now = std::time::Instant::now();
                    if let Some(last) = self.last_heartbeat_at {
                        if now.duration_since(last).as_secs() > 90 {
                            log::info!(
                                "[{}] heartbeat gap {}s — wake from sleep, resetting retry backoff",
                                self.mount_id,
                                now.duration_since(last).as_secs()
                            );
                            self.error_retries = 0;
                            self.next_error_retry_at = None;
                        }
                    }
                    self.last_heartbeat_at = Some(now);

                    // Error auto-retry with exponential backoff. Without
                    // this, any mount that failed at startup (agent up
                    // before the network / NAS booting / sync spawn
                    // failure) stays in Error until the user clicks
                    // Restart.
                    if matches!(self.state, MountState::Error(_)) {
                        let due = self
                            .next_error_retry_at
                            .map(|t| now >= t)
                            .unwrap_or(true);
                        if due {
                            let backoff_secs = 30u64
                                .saturating_mul(1 << self.error_retries.min(4)); // 30s..480s
                            self.error_retries = self.error_retries.saturating_add(1);
                            self.next_error_retry_at =
                                Some(now + std::time::Duration::from_secs(backoff_secs));
                            log::info!(
                                "[{}] Error state — auto-retry #{} (next in {}s)",
                                self.mount_id, self.error_retries, backoff_secs
                            );
                            self.handle_event(MountEvent::Restart).await;
                            if matches!(self.state, MountState::Mounting) {
                                self.handle_event(MountEvent::RequestStateUpdate).await;
                            }
                        }
                    } else if matches!(self.state, MountState::Mounted(_)) {
                        // Healthy (or at least mounted) — clear backoff.
                        self.error_retries = 0;
                        self.next_error_retry_at = None;
                    }

                    // Probe gate widened from is_sync_active() to "any
                    // mounted state": a mount whose sync phase is dead
                    // still needs zombie detection.
                    #[cfg(any(target_os = "macos", windows))]
                    if matches!(self.state, MountState::Mounted(_)) {
                        let probe_path = self
                            .mounted_at
                            .clone()
                            .unwrap_or_else(|| {
                                std::path::PathBuf::from(&self.config.nas_share_path)
                            });
                        let tx = hb_tx.clone();
                        #[cfg(target_os = "macos")]
                        let nas_path = self.config.nas_share_path.clone();
                        tokio::spawn(async move {
                            // Timeout the SMB call — stale connections can block 60s+.
                            let result = tokio::time::timeout(
                                std::time::Duration::from_secs(10),
                                tokio::task::spawn_blocking(move || {
                                    // macOS ownership pre-check: a foreign
                                    // volume can occupy our mountpoint
                                    // (our mount died, a disk image / USB
                                    // with the same name took the path) —
                                    // read_dir would answer happily against
                                    // it and mask the dead mount forever.
                                    #[cfg(target_os = "macos")]
                                    {
                                        let p = probe_path.to_string_lossy();
                                        if crate::platform::macos::mount_at_path_is_ours(
                                            &p, &nas_path,
                                        ) != Some(true)
                                        {
                                            return false;
                                        }
                                    }
                                    // read_dir + first entry, NOT metadata():
                                    // the kernel serves the mount root's
                                    // attributes from its attribute cache
                                    // even when the SMB session is dead, so
                                    // a stat probe reports zombie mounts as
                                    // healthy forever. A directory read
                                    // forces real wire traffic.
                                    match std::fs::read_dir(&probe_path) {
                                        Ok(mut rd) => !matches!(rd.next(), Some(Err(_))),
                                        Err(_) => false,
                                    }
                                }),
                            ).await;
                            let reachable = match result {
                                Ok(Ok(r)) => r,
                                _ => false, // Timeout or panic = unreachable
                            };
                            let _ = tx.send(reachable).await;
                        });
                    }
                }
                // Heartbeat result — handle disconnect/reconnect.
                // Gated on Mounted(_) (not is_sync_active) to match the
                // probe: zombie detection must run even when the sync
                // phase is dead.
                Some(reachable) = hb_rx.recv() => {
                    #[cfg(any(target_os = "macos", windows))]
                    if matches!(self.state, MountState::Mounted(_)) {
                        self.handle_heartbeat_result(reachable).await;
                    }
                }

                // Fires (Err) when MountService's MountInstance drops
                // its paired oneshot::Sender — i.e. mount removed from
                // config OR agent shutting down. Last arm so any
                // pending Stop / Restart / heartbeat is drained first.
                _ = &mut shutdown_signal => {
                    log::info!(
                        "[{}] shutdown signal fired, orchestrator exiting",
                        self.mount_id
                    );
                    break;
                }
            }
        }
    }

    async fn handle_event(&mut self, event: MountEvent) {
        log::debug!(
            "[{}] Event {:?} in state {}",
            self.mount_id,
            event,
            self.state
        );

        // Update stored config if this is a ConfigChanged event
        if let MountEvent::ConfigChanged { ref new_config } = event {
            self.config = new_config.clone();
        }

        let old_state = self.state.clone();
        let (new_state, effects) = state::transition(self.state.clone(), event);

        self.state = new_state;

        if self.state != old_state {
            log::info!(
                "[{}] {} → {}",
                self.mount_id,
                old_state,
                self.state
            );
        }

        for effect in effects {
            self.dispatch_effect(effect).await;
        }
    }

    async fn dispatch_effect(&mut self, effect: Effect) {
        match effect {
            Effect::MountDrive => {
                #[cfg(windows)]
                if self.config.is_sync_mode() {
                    self.start_sync().await;
                    return;
                }
                self.mount_drive().await;
            }
            Effect::DisconnectDrive => {
                #[cfg(windows)]
                if self._projfs_active {
                    self.stop_sync().await;
                    return;
                }
                self.disconnect_drive().await;
            }
            Effect::SpawnSyncServer => {
                #[cfg(any(target_os = "macos", windows))]
                self.spawn_sync_server().await;
            }
            Effect::TeardownSyncServer => {
                #[cfg(any(target_os = "macos", windows))]
                self.teardown_sync_server().await;
            }
            Effect::UpdateTray => {
                // Tray updates are handled by the mount_service via state updates
            }
            Effect::LogEvent { level, message } => match level {
                LogLevel::Info => log::info!("[{}] {}", self.mount_id, message),
                LogLevel::Error => log::error!("[{}] {}", self.mount_id, message),
            },
            Effect::EmitStateUpdate => {
                self.emit_state_update().await;
            }
        }
    }

    async fn mount_drive(&mut self) {
        // Retrieve credentials. macOS is credential-free here: NetFS
        // consults the login Keychain directly (plans/17 slice C).
        #[cfg(not(target_os = "macos"))]
        let (username, password) = self.retrieve_credentials().await;

        // Every successful mount writes the resolved backing path to
        // `self.mounted_at`. SpawnSyncServer reads it directly — no
        // more 5s sleep + 60s `/Volumes/<share>` poll dance like the
        // pre-refactor `main.rs::spawn_nfs_servers` closure did.
        // On Windows the resolved path is just the UNC; on macOS it's
        // whatever NetFSMountURLSync parked at (which may be a dedup
        // variant like `/Volumes/share-1`).
        #[cfg(any(target_os = "macos", windows))]
        {
            self.mounted_at = None;
        }

        #[cfg(windows)]
        {
            // Slice B: only sync mounts reach the agent on Windows —
            // plain mounts are GUI-owned drive letters, and the
            // C:\Volumes\ufb symlink layer is gone. The agent's job
            // here is the backing SMB session the WinFsp server reads
            // through.
            //
            // Establish SMB session before transitioning to Mounted.
            // Previously this was fire-and-forget under the assumption Windows
            // would have a cached session — true on warm relaunches, false on
            // cold first launch. The UI reported "Connected" while the symlink
            // it pointed at had no live SMB session behind it, so the first
            // click-through silently failed in listDirectory. Blocking here
            // keeps the Mounting → Mounted transition honest.
            {
                let share = self.config.nas_share_path.clone();
                let u = username.clone();
                let p = password.clone();
                let share2 = share.clone();
                let result = tokio::task::spawn_blocking(move || {
                    crate::platform::windows::fallback::establish_smb_session(&share2, &u, &p)
                })
                .await
                .unwrap_or_else(|e| {
                    Err(crate::platform::windows::fallback::SmbSessionError::Other(
                        format!("SMB session task panicked: {}", e),
                    ))
                });

                match result {
                    Ok(()) => {
                        let host = share
                            .trim_start_matches('\\')
                            .split('\\')
                            .next()
                            .unwrap_or("")
                            .to_lowercase();
                        if !host.is_empty() {
                            self.connected_servers.lock().unwrap().insert(host);
                        }
                    }
                    Err(crate::platform::windows::fallback::SmbSessionError::Auth(e)) => {
                        // Server is reachable but rejected our credentials.
                        // Drive the mount to Error(AuthFailed) so the
                        // sidebar can render the "Fix credentials" pill.
                        log::warn!("[{}] SMB session auth failed: {}", self.mount_id, e);
                        let _ = self
                            .event_tx
                            .send(MountEvent::AuthFailed { reason: e })
                            .await;
                        return;
                    }
                    Err(crate::platform::windows::fallback::SmbSessionError::Other(e)) => {
                        // Network / unknown — keep current behavior of
                        // logging and proceeding. Mount may still work
                        // via a previously cached session, or fail later
                        // at first listDirectory.
                        log::warn!("[{}] SMB session failed: {}", self.mount_id, e);
                    }
                }
            }

            // Windows: VFS server reads the UNC directly. No path
            // resolution; just stash the UNC for SpawnSyncServer.
            self.mounted_at =
                Some(std::path::PathBuf::from(&self.config.nas_share_path));
        }

        #[cfg(target_os = "linux")]
        {
            // Linux: gio mount + symlink (two-step, same as before)
            use crate::platform::SmbSession;
            let smb = crate::platform::linux::LinuxSmbSession::new();
            let smb_result = smb.ensure_session(
                &self.config.nas_share_path,
                &self.config.smb_target_path(),
                &username,
                &password,
            );

            if let Err(e) = smb_result {
                log::error!("[{}] SMB session failed: {}", self.mount_id, e);
                let _ = self
                    .event_tx
                    .send(MountEvent::MountFailed { reason: e })
                    .await;
                return;
            }

            // Create symlink from user-facing path to SMB mount
            use crate::platform::DriveMapping;
            let dm = crate::platform::linux::LinuxMountMapping::new();
            let mount_point = self.config.mount_path();
            let target = self.config.smb_target_path();
            if let Err(e) = dm.switch(&mount_point, &target) {
                log::error!("[{}] Mount mapping failed: {}", self.mount_id, e);
                let _ = self
                    .event_tx
                    .send(MountEvent::MountFailed { reason: e })
                    .await;
            }
        }

        #[cfg(target_os = "macos")]
        {
            use crate::platform::macos::MacosMountError;
            self.mount_notice = None;
            // Consume the one-shot UI permit: set only when this mount
            // attempt came from an explicit user action (Fix-credentials
            // pill), so only that attempt may pop the OS auth dialog.
            let allow_ui = self
                .ui_permit
                .swap(false, std::sync::atomic::Ordering::SeqCst);

            if self.config.is_sync_mode() {
                // Sync mode: headless SMB mount. The user-facing path
                // (~/ufb/mounts/<share>) is owned by the NFS loopback
                // server which Slice B spawns via SpawnSyncServer
                // immediately after this returns; no FileProvider
                // symlink (the abandoned scaffolding was removed in
                // Slice G along with MacosNasWatcher).
                let smb_result = crate::platform::macos::macos_smb_mount(
                    &self.config.nas_share_path,
                    allow_ui,
                );
                match smb_result {
                    Ok(volumes_path) => {
                        log::info!("[{}] Headless SMB mount at {}", self.mount_id, volumes_path);
                        self.mount_notice = drift_notice(&self.config, &volumes_path);
                        self.mounted_at =
                            Some(std::path::PathBuf::from(&volumes_path));
                    }
                    Err(MacosMountError::Auth(e)) => {
                        log::warn!("[{}] SMB auth failed: {}", self.mount_id, e);
                        let _ = self
                            .event_tx
                            .send(MountEvent::AuthFailed { reason: e })
                            .await;
                        return;
                    }
                    Err(MacosMountError::Other(e)) => {
                        log::error!("[{}] Headless SMB mount failed: {}", self.mount_id, e);
                        let _ = self
                            .event_tx
                            .send(MountEvent::MountFailed { reason: e })
                            .await;
                        return;
                    }
                }
                return;
            }

            // Non-sync mounts are GUI-owned on macOS (plans/17 F1) and
            // never reach this orchestrator — apply_config skips them.
            // The old mount+symlink arm was deleted in 1.0.7.
            let _ = allow_ui;
            log::error!(
                "[{}] BUG: agent asked to mount a non-sync macOS share — \
                 these are GUI-owned since 1.0.5 (F1)",
                self.mount_id
            );
            let _ = self
                .event_tx
                .send(MountEvent::MountFailed {
                    reason: "non-sync mounts are app-owned on macOS".into(),
                })
                .await;
        }
    }

    async fn disconnect_drive(&mut self) {
        #[cfg(windows)]
        {
            // Disconnect the deviceless SMB session
            let share_path = self.config.nas_share_path.clone();
            let mount_id = self.mount_id.clone();
            let result = tokio::task::spawn_blocking(move || {
                crate::platform::windows::fallback::disconnect_smb_session(&share_path)
            })
            .await
            .unwrap_or_else(|e| Err(format!("disconnect_smb task panicked: {}", e)));
            if let Err(e) = result {
                log::warn!("[{}] SMB disconnect failed (non-fatal): {}", mount_id, e);
            }
        }

        #[cfg(target_os = "linux")]
        {
            use crate::platform::DriveMapping;
            let dm = crate::platform::linux::LinuxMountMapping::new();
            let mount_point = self.config.mount_path();
            if let Err(e) = dm.remove(&mount_point) {
                log::warn!("[{}] Remove symlink failed (non-fatal): {}", self.mount_id, e);
            }
        }

        #[cfg(target_os = "macos")]
        {
            // Only sync mounts reach this orchestrator on macOS (F1);
            // the mount point is the NFS loopback dir, never a plain
            // symlink (MacosMountMapping deleted 1.0.7).
            let mount_point = self.config.mount_path();

            // If the path is still an empty directory (post-NFS-
            // unmount artifact), remove it so the next mount_drive
            // can put a fresh NFS mount there. Only remove if empty —
            // never touch user data.
            let mp_path = std::path::Path::new(&mount_point);
            if mp_path.is_dir() && !mp_path.is_symlink() {
                let empty = std::fs::read_dir(mp_path)
                    .map(|mut d| d.next().is_none())
                    .unwrap_or(false);
                if empty {
                    if let Err(e) = std::fs::remove_dir(mp_path) {
                        log::warn!(
                            "[{}] removing stale empty mount dir {}: {}",
                            self.mount_id, mount_point, e
                        );
                    }
                }
            }

            // Unmount the SMB backing volume via the path mount_drive
            // captured (the dm.read_target symlink fallback died with
            // MacosMountMapping in 1.0.7).
            let volumes_path = self
                .mounted_at
                .take()
                .map(|p| p.to_string_lossy().to_string());
            if let Some(p) = volumes_path {
                // Ownership guard (plans/17 slice C): only unmount if
                // the mount table says the path holds OUR smb share.
                // A foreign volume can land at this path (disk image /
                // USB named like the share, or a squatter that
                // displaced us to a dedup name) — unmounting by path
                // would eject the user's volume.
                match crate::platform::macos::mount_at_path_is_ours(
                    &p,
                    &self.config.nas_share_path,
                ) {
                    Some(true) => {
                        if let Err(e) = crate::platform::macos::macos_smb_unmount(&p) {
                            log::warn!(
                                "[{}] SMB unmount of {} failed (non-fatal): {}",
                                self.mount_id, p, e
                            );
                        }
                    }
                    Some(false) => log::warn!(
                        "[{}] NOT unmounting {} — occupied by a foreign volume, not our share",
                        self.mount_id, p
                    ),
                    None => {} // nothing mounted there
                }
            }
        }
    }

    /// Spawn the VFS server (NFS loopback on macOS, WinFsp on Windows)
    /// for a sync-enabled mount. No-op for non-sync mounts. Idempotent:
    /// if a handle already exists from a prior cycle, leaves it alone.
    ///
    /// Reads `self.mounted_at` (populated by a preceding MountDrive) for
    /// the resolved backing filesystem path. Populates
    /// `self.shared_caches` so MountService's UI drain/stats path can
    /// see the per-domain cache.
    #[cfg(any(target_os = "macos", windows))]
    async fn spawn_sync_server(&mut self) {
        if !self.config.is_sync_mode() {
            return;
        }
        if self.sync_handle.is_some() {
            log::debug!(
                "[{}] spawn_sync_server: handle already live, skipping",
                self.mount_id
            );
            return;
        }
        let Some(nas_root) = self.mounted_at.clone() else {
            self.fail_sync_spawn(
                "no backing path resolved for sync server (MountDrive \
                 did not set mounted_at)",
            )
            .await;
            return;
        };
        let share = self.config.share_name();

        #[cfg(target_os = "macos")]
        {
            // Reserve our NFS port once. Stays sticky across Stop/Start
            // cycles within the same orchestrator instance so the macOS
            // NFS client's persistent fh cache keeps working.
            let port = match self.nfs_port {
                Some(p) => p,
                None => {
                    let p = NEXT_NFS_PORT.fetch_add(1, Ordering::Relaxed);
                    self.nfs_port = Some(p);
                    p
                }
            };

            // Open (or reuse) the per-domain SQLite cache.
            let cache = {
                let Some(ref caches) = self.shared_caches else {
                    self.fail_sync_spawn("shared_caches not wired").await;
                    return;
                };
                let existing = caches.read().ok().and_then(|g| g.get(&share).cloned());
                if let Some(arc) = existing {
                    arc
                } else {
                    // "0 = unlimited" is a footgun as a DEFAULT: with no
                    // limit the evictor never even spawns and the read
                    // cache grows until the boot disk fills. Treat 0 as
                    // "unset" and apply a sane ceiling; users who truly
                    // want more can set an explicit large value.
                    const DEFAULT_CACHE_LIMIT: u64 = 50 * 1024 * 1024 * 1024; // 50 GiB
                    let cache_limit = if self.config.sync_cache_limit_bytes == 0 {
                        log::info!(
                            "[{}] sync_cache_limit_bytes unset — defaulting to 50 GiB",
                            self.mount_id
                        );
                        DEFAULT_CACHE_LIMIT
                    } else {
                        self.config.sync_cache_limit_bytes
                    };
                    let opened = crate::sync::MacosCache::open(
                        &share,
                        nas_root.clone(),
                        cache_limit,
                        &self.cache_root,
                    );
                    match opened {
                        Ok(c) => {
                            let arc = std::sync::Arc::new(c);
                            arc.set_badge_tx(self.ipc_tx.clone());
                            if let Ok(mut g) = caches.write() {
                                g.insert(share.clone(), std::sync::Arc::clone(&arc));
                            }
                            log::info!(
                                "[{}] cache opened for sync domain {}",
                                self.mount_id, share
                            );
                            arc
                        }
                        Err(e) => {
                            self.fail_sync_spawn(&format!(
                                "cache open failed for {}: {}",
                                share, e
                            ))
                            .await;
                            return;
                        }
                    }
                }
            };

            // Slice D: orchestrator owns the NasHealth atomic and
            // drives it from the heartbeat. NFS server gets a clone
            // to read for JUKEBOX short-circuit.
            let health = crate::sync::nas_health::NasHealth::new(
                share.clone(),
                nas_root.clone(),
            );
            self.nas_health = Some(Arc::clone(&health));

            let handle = crate::sync::nfs_server::start(
                share.clone(),
                nas_root,
                port,
                cache,
                self.ipc_tx.clone(),
                health,
            );
            self.sync_handle = Some(handle);
            // Slice H fix: go straight to Active. The Spawning state
            // had no transition out of it (heartbeat only flips
            // Offline/Reconnecting → Active), so mounts were stuck
            // visible as "Registering" in the UI forever. The NFS
            // server is up and bound by the time start() returns,
            // and the loopback auto-mount fires in a spawn_blocking
            // task that's already going — there's no useful "still
            // spawning" window for the user to see.
            self.state = MountState::Mounted(SyncPhase::Active);
            log::info!(
                "[{}] NFS sync server spawned on port {}",
                self.mount_id, port
            );
        }

        #[cfg(windows)]
        {
            // WinFsp variant — same shape. The Windows cache uses a
            // different open() signature than macOS (CacheIndex needs an
            // open_handles Arc + returns a needs_repair flag); fold into
            // a helper to keep this method readable.
            let cache = {
                let Some(ref caches) = self.shared_caches else {
                    self.fail_sync_spawn("shared_caches not wired").await;
                    return;
                };
                let existing = caches.read().ok().and_then(|g| g.get(&share).cloned());
                if let Some(arc) = existing {
                    arc
                } else {
                    let open_handles = std::sync::Arc::new(std::sync::Mutex::new(
                        std::collections::HashMap::new(),
                    ));
                    // Same 0-means-unset clamp as the macOS branch: an
                    // unlimited default disables eviction entirely and
                    // fills the system disk.
                    const DEFAULT_CACHE_LIMIT: u64 = 50 * 1024 * 1024 * 1024; // 50 GiB
                    let cache_limit = if self.config.sync_cache_limit_bytes == 0 {
                        log::info!(
                            "[{}] sync_cache_limit_bytes unset — defaulting to 50 GiB",
                            self.mount_id
                        );
                        DEFAULT_CACHE_LIMIT
                    } else {
                        self.config.sync_cache_limit_bytes
                    };
                    let (ci, _needs_repair) = crate::sync::CacheIndex::open(
                        &nas_root,
                        &share,
                        cache_limit,
                        open_handles,
                        &self.cache_root,
                    );
                    let arc = std::sync::Arc::new(ci);
                    if let Ok(mut g) = caches.write() {
                        g.insert(share.clone(), std::sync::Arc::clone(&arc));
                    }
                    log::info!(
                        "[{}] cache opened for sync domain {}",
                        self.mount_id, share
                    );
                    arc
                }
            };

            // Windows parity with the macOS Slice D wiring: the
            // orchestrator owns the NasHealth atomic, feeds it from the
            // heartbeat, and the WinFsp provider reads it to short-
            // circuit SMB-touching ops while offline. Pre-fix the
            // provider constructed its own NasHealth that nothing fed —
            // a NAS drop parked every WinFsp dispatcher thread on the
            // SMB redirector timeout and hung Explorer.
            let health = crate::sync::nas_health::NasHealth::new(
                share.clone(),
                nas_root.clone(),
            );
            self.win_nas_health = Some(std::sync::Arc::clone(&health));

            // Slice B: WinFsp mounts straight to a drive letter (its
            // default happy path). Configured mountDriveLetter is the
            // preference; otherwise scan Z:→D: for a free letter.
            let preferred = self
                .config
                .mount_drive_letter
                .trim()
                .chars()
                .next()
                .filter(|c| c.is_ascii_alphabetic());
            let nas_unc = self.config.nas_share_path.clone();
            let letter = tokio::task::spawn_blocking(move || {
                crate::platform::windows::fallback::choose_free_letter(preferred, &nas_unc)
            })
            .await
            .unwrap_or(None);
            let Some(letter) = letter else {
                self.fail_sync_spawn("no free drive letter (D:–Z: all in use)")
                    .await;
                return;
            };

            let (handle, ready_rx) = crate::sync::winfsp_server::start(
                share.clone(),
                nas_root,
                cache,
                self.ipc_tx.clone(),
                self.cache_root.clone(),
                letter,
                health,
            );
            self.sync_handle = Some(handle);

            // Wait for the dispatcher thread to actually claim the
            // mount point before reporting Active. Pre-fix this went
            // straight to Active while init/mount ran fire-and-forget
            // on the thread — a squatted mount point failed silently
            // and the UI showed a green mount over a dead path with no
            // retry. Mounting is local and sub-second; 15s covers cold
            // WinFsp service starts.
            let ready = tokio::task::spawn_blocking(move || {
                ready_rx.recv_timeout(std::time::Duration::from_secs(15))
            })
            .await;
            match ready {
                Ok(Ok(Ok(()))) => {
                    // The letter is the user-facing mount location;
                    // serialize_state advertises it for sync mounts
                    // (self.mounted_at stays the UNC backing root).
                    self.sync_letter_root = Some(format!("{}:\\", letter));
                    // See macOS branch above — same reasoning for going
                    // straight to Active.
                    self.state = MountState::Mounted(SyncPhase::Active);
                    log::info!(
                        "[{}] WinFsp sync server spawned at {}:\\",
                        self.mount_id, letter
                    );
                }
                outcome => {
                    let reason = match outcome {
                        Ok(Ok(Err(msg))) => msg,
                        Ok(Err(_)) => "WinFsp start timed out after 15s".to_string(),
                        Err(e) => format!("readiness wait task failed: {}", e),
                        Ok(Ok(Ok(()))) => unreachable!(),
                    };
                    // Tear down the handle (thread has already exited on
                    // failure; this joins it and drops the cache entry)
                    // and route through the FSM: state becomes Error and
                    // the auto-retry backoff re-drives Start.
                    self.teardown_sync_server().await;
                    self.fail_sync_spawn(&reason).await;
                }
            }
        }
    }

    /// A sync-mode mount whose VFS server failed to spawn is NOT
    /// mounted — reporting Mounted(NotApplicable) leaves the UI green
    /// while sync is silently dead and, because `is_sync_active()` is
    /// false, the heartbeat never runs so nothing ever retries. Route
    /// the failure through the FSM instead: state becomes Error, the
    /// user sees it, and the Error auto-retry backoff re-drives Start.
    #[cfg(any(target_os = "macos", windows))]
    async fn fail_sync_spawn(&mut self, reason: &str) {
        log::error!("[{}] sync server spawn failed: {}", self.mount_id, reason);
        let _ = self
            .event_tx
            .send(MountEvent::MountFailed {
                reason: format!("sync server spawn failed: {}", reason),
            })
            .await;
    }

    /// Tear down the VFS server. Always emitted before DisconnectDrive
    /// so the loopback NFS / FileSystemHost unmounts before the SMB
    /// session it depends on goes away. Idempotent — no-op if no handle.
    /// Drops the per-domain entry from `shared_caches` so the SQLite
    /// pool's last Arc release lands cleanly (closes the DB file).
    /// Also drops the NasHealth Arc — without this, the orchestrator's
    /// heartbeat would keep updating a health atomic no one reads.
    #[cfg(any(target_os = "macos", windows))]
    async fn teardown_sync_server(&mut self) {
        let Some(handle) = self.sync_handle.take() else {
            return;
        };
        let share = self.config.share_name();
        log::info!("[{}] tearing down sync server for {}", self.mount_id, share);
        handle.shutdown_and_wait().await;
        if let Some(ref caches) = self.shared_caches {
            if let Ok(mut g) = caches.write() {
                g.remove(&share);
            }
        }
        #[cfg(target_os = "macos")]
        {
            self.nas_health = None;
        }
        #[cfg(windows)]
        {
            self.win_nas_health = None;
            self.sync_letter_root = None;
        }
    }

    /// Start on-demand sync: ensure the SMB share is reachable, then let
    /// main.rs start the WinFsp backend. The orchestrator tracks the
    /// sync sub-state by mutating `self.state` to
    /// `MountState::Mounted(SyncPhase::*)` — this replaces the pre-
    /// refactor `self.sync_state: SyncState` field. Slice B will move
    /// the actual VFS server handle ownership in here too.
    ///
    /// Prefers Windows' existing cached SMB session when available. We
    /// only call `WNetAddConnection2W` if the share isn't already
    /// accessible — otherwise our explicit auth races with any cached
    /// session (Windows returns `ERROR_LOGON_FAILURE` even with correct
    /// creds if a different-credentialed session is already live),
    /// leaving syncState stuck in Error while WinFsp works fine.
    #[cfg(windows)]
    async fn start_sync(&mut self) {
        // Phase 0: do NOT mutate self.state. Pre-fix this function set
        // state to Mounted(Spawning) on entry and Mounted(Active) on
        // success, bypassing the FSM's Mounting → Mounted(NotApplicable)
        // transition — which is what emits the SpawnSyncServer effect.
        // Net result was WinFsp never starting: log said "Sync active"
        // but no `[winfsp]` line ever appeared and no reparse point
        // existed at the mount path. By leaving self.state at Mounting,
        // the run loop's synthetic RequestStateUpdate fires after this
        // function returns, the FSM transitions correctly, and
        // SpawnSyncServer dispatches to spawn_sync_server (which calls
        // winfsp_server::start and sets state to Mounted(Active) itself).
        let share_path = self.config.nas_share_path.clone();
        let mount_id = self.mount_id.clone();

        // Slice B: no user-facing junction anymore — WinFsp mounts
        // straight to a drive letter in spawn_sync_server, and the
        // letter (not a C:\Volumes\ufb path) is the advertised mount
        // location. Junction-era debris is cleaned up by
        // winfsp_server::cleanup_junction_era_debris on spawn.

        // Probe: can we already list the share? If so, an existing session
        // (ours from a previous attempt, or Windows' cached one) works.
        let probe_path = share_path.clone();
        let already_reachable = tokio::task::spawn_blocking(move || {
            std::fs::metadata(&probe_path).is_ok()
        })
        .await
        .unwrap_or(false);

        if !already_reachable {
            let (username, password) = self.retrieve_credentials().await;
            let u = username.clone();
            let p = password.clone();
            let sp = share_path.clone();
            let result = tokio::task::spawn_blocking(move || {
                crate::platform::windows::fallback::establish_smb_session(&sp, &u, &p)
            })
            .await
            .unwrap_or_else(|e| {
                Err(crate::platform::windows::fallback::SmbSessionError::Other(
                    format!("SMB session task panicked: {}", e),
                ))
            });

            match result {
                Ok(()) => {}
                Err(crate::platform::windows::fallback::SmbSessionError::Auth(e)) => {
                    log::error!("[{}] SMB session auth failed (sync): {}", mount_id, e);
                    let _ = self
                        .event_tx
                        .send(MountEvent::AuthFailed { reason: e })
                        .await;
                    return;
                }
                Err(crate::platform::windows::fallback::SmbSessionError::Other(e)) => {
                    log::error!("[{}] SMB session failed (sync): {}", mount_id, e);
                    let _ = self
                        .event_tx
                        .send(MountEvent::MountFailed { reason: e })
                        .await;
                    return;
                }
            }
        } else {
            log::info!(
                "[{}] SMB session already established (using Windows cached creds)",
                mount_id
            );
        }

        // Mark server as connected so other mounts skip credential lookup
        let host = Self::server_host(&self.config.nas_share_path);
        if !host.is_empty() {
            self.connected_servers.lock().unwrap().insert(host);
        }

        // _projfs_active flags "this orchestrator owns a sync mount" so
        // DisconnectDrive routes through stop_sync rather than
        // disconnect_drive. Keep it set.
        self._projfs_active = true;

        // Stash the NAS root for spawn_sync_server to bind WinFsp to.
        // Without this, the SpawnSyncServer effect (now actually
        // reachable since we no longer pre-empt the FSM transition)
        // bails with "no mounted_at set" and WinFsp silently never starts.
        self.mounted_at = Some(std::path::PathBuf::from(&self.config.nas_share_path));

        log::info!(
            "[{}] SMB session ready for sync — awaiting SpawnSyncServer effect",
            self.mount_id
        );
    }

    /// Stop on-demand sync: disconnect SMB session.
    /// Slice B will add `FileSystemHost::unmount()` here via the
    /// orchestrator-owned `sync_handle`.
    #[cfg(windows)]
    async fn stop_sync(&mut self) {
        // Do NOT touch `self.state` here — this runs as an effect and
        // the FSM already transitioned before effects dispatch. The
        // old `state = Mounted(Spawning)` line clobbered that:
        // - Stop: final state stayed Mounted(Registering) instead of
        //   Stopped (the "outer machine sets Stopped after" claim was
        //   stale — nothing runs after the effects).
        // - ConfigChanged/Restart: the run loop's synthetic
        //   RequestStateUpdate only fires when state is Mounting after
        //   handle_event returns; the clobber made it Mounted, so
        //   Mounting → Mounted never happened and SpawnSyncServer never
        //   dispatched — a config change or user Restart on a sync
        //   mount tore the VFS down and never brought it back.
        self._projfs_active = false;

        // Disconnect the deviceless SMB session
        let share_path = self.config.nas_share_path.clone();
        let _ = tokio::task::spawn_blocking(move || {
            crate::platform::windows::fallback::disconnect_smb_session(&share_path)
        })
        .await;

        log::info!("[{}] Sync stopped", self.mount_id);
    }

    /// "Is the sync server live right now and worth heartbeating?"
    /// Cross-platform check used by the heartbeat arm. Replaces the
    /// pre-Slice-D `_projfs_active` flag (which was Windows-only).
    /// Slice B made `sync_handle` the universal "VFS server up" signal.
    #[cfg(any(target_os = "macos", windows))]
    fn is_sync_active(&self) -> bool {
        self.config.is_sync_mode() && self.sync_handle.is_some()
    }

    /// Handle heartbeat result — trigger disconnect/reconnect as needed.
    /// Reads the sub-state from `MountState::Mounted(SyncPhase)` now
    /// that the parallel `sync_state` field is gone.
    /// Slice D: cross-platform now (was Windows-only). macOS routes
    /// reconnect through `macos_smb_mount` which the existing-mount
    /// short-circuit makes idempotent.
    #[cfg(any(target_os = "macos", windows))]
    async fn handle_heartbeat_result(&mut self, reachable: bool) {
        let phase = match &self.state {
            MountState::Mounted(p) => p.clone(),
            // Not mounted (mid-transition, stopped, errored) — nothing
            // for the heartbeat to act on.
            _ => return,
        };

        // Slice D: feed NasHealth so the VFS provider short-circuits
        // SMB-touching ops (JUKEBOX on macOS, fast error / cache-serve
        // on Windows) while we're offline.
        #[cfg(target_os = "macos")]
        if let Some(ref h) = self.nas_health {
            h.set_online(reachable);
        }
        #[cfg(windows)]
        if let Some(ref h) = self.win_nas_health {
            h.set_online(reachable);
        }

        match (reachable, phase) {
            (false, SyncPhase::Active) => {
                // NAS just went down. win_nas_health (set above) is
                // what the WinFsp provider reads to short-circuit SMB
                // ops; the old NasConnectivity atomic is deleted.
                log::warn!("[{}] NAS heartbeat failed — going offline", self.mount_id);
                self.state = MountState::Mounted(SyncPhase::Offline);
                self.emit_state_update().await;
                // Immediately transition to reconnecting
                self.state = MountState::Mounted(SyncPhase::Reconnecting);
                self.emit_state_update().await;
            }
            (true, SyncPhase::Offline) | (true, SyncPhase::Reconnecting) => {
                // NAS is back
                log::info!("[{}] NAS heartbeat OK — reconnecting", self.mount_id);
                self.complete_reconnect().await;
            }
            _ => {} // No change
        }
    }

    /// Re-establish the SMB session after a disconnect. Cross-platform
    /// after Slice D — Windows reuses `establish_smb_session`; macOS
    /// reuses `macos_smb_mount` (idempotent: short-circuits when the
    /// mount is already up). Stale-mount detection (kernel says mounted
    /// but SMB rejects ops) is handled inside `macos_smb_mount`'s
    /// new force-remount path.
    #[cfg(any(target_os = "macos", windows))]
    async fn complete_reconnect(&mut self) {
        // macOS reconnects credential-free — NetFS reads the Keychain.
        #[cfg(not(target_os = "macos"))]
        let (username, password) = self.retrieve_credentials().await;

        #[cfg(windows)]
        {
            let share_path = self.config.nas_share_path.clone();
            let _ = tokio::task::spawn_blocking(move || {
                crate::platform::windows::fallback::establish_smb_session(
                    &share_path, &username, &password,
                )
            })
            .await;
        }

        #[cfg(target_os = "macos")]
        {
            // Slice D: stale-mount recovery. If the kernel still thinks
            // /Volumes/<share> is mounted but a quick stat probe times
            // out, the SMB session has gone dead beneath us — Apple's
            // NetFS won't notice unless we force-unmount first. Without
            // this, macos_smb_mount's existing-mount short-circuit
            // hands back the stale path and the heartbeat stays stuck
            // in Reconnecting forever.
            if let Some(stale_path) = self.mounted_at.clone() {
                let probe = tokio::time::timeout(
                    std::time::Duration::from_secs(3),
                    tokio::task::spawn_blocking(move || {
                        std::fs::metadata(&stale_path).is_ok()
                    }),
                )
                .await;
                let reachable = matches!(probe, Ok(Ok(true)));
                if !reachable {
                    let unmount_path =
                        self.mounted_at.clone().unwrap().to_string_lossy().to_string();
                    // Ownership guard (plans/17 slice C): unmount only
                    // OUR dead share. A foreign volume at this path
                    // (squatter that displaced us / renamed disk) must
                    // not be ejected — clear mounted_at and let the
                    // remount land wherever NetFS picks.
                    let nas = self.config.nas_share_path.clone();
                    let up = unmount_path.clone();
                    let ours = tokio::task::spawn_blocking(move || {
                        crate::platform::macos::mount_at_path_is_ours(&up, &nas)
                    })
                    .await
                    .unwrap_or(None);
                    if ours == Some(true) {
                        log::warn!(
                            "[{}] stale SMB mount at {} — force-unmounting before reconnect",
                            self.mount_id, unmount_path
                        );
                        let _ = tokio::task::spawn_blocking(move || {
                            crate::platform::macos::macos_smb_unmount(&unmount_path)
                        })
                        .await;
                    } else if ours == Some(false) {
                        log::warn!(
                            "[{}] {} is occupied by a foreign volume — leaving it, remounting elsewhere",
                            self.mount_id, unmount_path
                        );
                    }
                    self.mounted_at = None;
                }
            }

            let share_path = self.config.nas_share_path.clone();
            // Background reconnect: never allowed to pop the auth
            // dialog — a stale Keychain entry surfaces via the pill on
            // the next explicit Start instead.
            let result = tokio::task::spawn_blocking(move || {
                crate::platform::macos::macos_smb_mount(&share_path, false)
                    .map_err(|e| e.to_string())
            })
            .await;
            match result {
                Ok(Ok(volumes_path)) => {
                    log::info!(
                        "[{}] macOS SMB session reattached at {}",
                        self.mount_id, volumes_path
                    );
                    self.mount_notice = drift_notice(&self.config, &volumes_path);
                    self.mounted_at =
                        Some(std::path::PathBuf::from(&volumes_path));
                }
                Ok(Err(e)) => {
                    log::warn!(
                        "[{}] macOS SMB reconnect failed: {} — staying Reconnecting",
                        self.mount_id, e
                    );
                    // Don't flip to Active; next heartbeat will retry.
                    return;
                }
                Err(e) => {
                    log::warn!(
                        "[{}] macOS SMB reconnect task panicked: {}",
                        self.mount_id, e
                    );
                    return;
                }
            }
        }

        // Mark server as connected
        let host = Self::server_host(&self.config.nas_share_path);
        if !host.is_empty() {
            self.connected_servers.lock().unwrap().insert(host);
        }

        // Set online
        self.state = MountState::Mounted(SyncPhase::Active);
        #[cfg(target_os = "macos")]
        if let Some(ref h) = self.nas_health {
            h.set_online(true);
        }
        #[cfg(windows)]
        if let Some(ref h) = self.win_nas_health {
            h.set_online(true);
        }
        self.emit_state_update().await;
        log::info!("[{}] NAS reconnected", self.mount_id);
    }

    /// Credential keys in the config are bare names (e.g., "gfx-nas").
    /// We store them with a "ufb_" prefix in the OS credential store.
    /// Linux-only since slice B — Windows credentials are OS-owned
    /// (Credential Manager server-keyed entries) and macOS uses the
    /// login Keychain via NetFS.
    #[cfg(target_os = "linux")]
    const CRED_PREFIX: &'static str = "ufb_";

    /// Extract the server hostname from a UNC path (e.g., \\192.168.1.50\share → 192.168.1.50).
    fn server_host(nas_path: &str) -> String {
        nas_path
            .trim_start_matches('\\')
            .split('\\')
            .next()
            .unwrap_or("")
            .to_lowercase()
    }

    /// Windows/Linux only — macOS mounts are credential-free (NetFS
    /// consults the login Keychain directly; plans/17 slice C).
    ///
    /// Windows (slice B): credentials are OS-owned. NULL user/pass into
    /// `WNetAddConnection2W` makes the redirector consult Credential
    /// Manager's server-keyed entries (populated by the GUI's one-time
    /// migration or the native auth dialog). UFB never reads the
    /// secret itself; the `ufb_*` store is gone.
    #[cfg(not(target_os = "macos"))]
    async fn retrieve_credentials(&self) -> (String, String) {
        #[cfg(windows)]
        {
            return (String::new(), String::new());
        }
        #[cfg(target_os = "linux")]
        {
            // If another mount already connected to this server, skip
            // the lookup and let the OS reuse the session.
            let host = Self::server_host(&self.config.nas_share_path);
            if !host.is_empty() && self.connected_servers.lock().unwrap().contains(&host) {
                log::debug!(
                    "[{}] Server {} already connected, reusing session",
                    self.mount_id, host
                );
                return (String::new(), String::new());
            }
            let configured = self.config.credential_key.trim();
            let key: String = if configured.is_empty() {
                self.mount_id.trim().to_string()
            } else {
                configured.to_string()
            };
            if key.is_empty() {
                return (String::new(), String::new());
            }
            let prefixed = if key.starts_with(Self::CRED_PREFIX) {
                key.clone()
            } else {
                format!("{}{}", Self::CRED_PREFIX, key)
            };
            use crate::platform::CredentialStore;
            let cred_store = crate::platform::linux::LinuxCredentialStore::new();
            match cred_store.retrieve(&prefixed) {
                Ok(creds) => creds,
                Err(e) => {
                    log::warn!(
                        "[{}] No credentials found for {}: {}, trying without",
                        self.mount_id, key, e
                    );
                    (String::new(), String::new())
                }
            }
        }
        #[cfg(not(any(windows, target_os = "linux")))]
        {
            (String::new(), String::new())
        }
    }

    async fn emit_state_update(&self) {
        let state_name = match &self.state {
            MountState::Initializing => "initializing",
            MountState::Mounting => "mounting",
            MountState::Mounted(_) => "mounted",
            // Differentiate auth-class errors so the sidebar can render
            // a "Fix credentials" pill that pops the credential prompt
            // directly, instead of the generic error toast.
            MountState::Error(MountError::AuthFailed { .. }) => "auth_error",
            MountState::Error(_) => "error",
            MountState::Stopped => "stopped",
        };

        // Sync sub-state is now embedded in MountState::Mounted; pull
        // it out only when we're actually Mounted on a sync-enabled
        // mount. `wire_name` returns None for SyncPhase::NotApplicable
        // so non-sync mounts emit None as before.
        let (sync_state, sync_state_detail) = match &self.state {
            MountState::Mounted(phase) if self.config.is_sync_mode() => (
                phase.wire_name().map(str::to_string),
                Some(phase.to_string()),
            ),
            _ => (None, None),
        };

        // The elevation subsystem is gone (slice B) — the wire field
        // stays for one release of back-compat, always None.
        let needs_elevation = None;

        // Only advertise a location while actually Mounted — a stale
        // mounted_at from a previous session must not feed resolution.
        // Windows sync mounts advertise the WinFsp drive letter, not
        // the UNC backing root the VFS reads through.
        let mounted_at = match &self.state {
            MountState::Mounted(_) => {
                #[cfg(windows)]
                if self.config.is_sync_mode() {
                    self.sync_letter_root.clone()
                } else {
                    self.mounted_at
                        .as_ref()
                        .map(|p| p.to_string_lossy().to_string())
                }
                #[cfg(not(windows))]
                self.mounted_at
                    .as_ref()
                    .map(|p| p.to_string_lossy().to_string())
            }
            _ => None,
        };

        let notice = {
            #[cfg(target_os = "macos")]
            {
                match &self.state {
                    MountState::Mounted(_) => self.mount_notice.clone(),
                    _ => None,
                }
            }
            #[cfg(not(target_os = "macos"))]
            {
                None
            }
        };

        let payload = MountStateUpdateMsg {
            mount_id: self.mount_id.clone(),
            state: state_name.into(),
            state_detail: self.state.to_string(),
            sync_state,
            sync_state_detail,
            needs_elevation,
            mounted_at,
            notice,
        };

        // Stash into the shared state_cache before broadcasting so
        // MountStateSnapshot built from the cache always sees the
        // freshest per-mount entry. Slice C addition.
        if let Ok(mut g) = self.state_cache.write() {
            g.insert(self.mount_id.clone(), payload.clone());
        }

        if let Err(e) = self.ipc_tx.send(AgentToUfb::MountStateUpdate(payload)).await {
            log::debug!("[{}] Failed to send state update: {}", self.mount_id, e);
        }
    }
}
