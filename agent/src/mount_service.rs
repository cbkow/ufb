use crate::config::{self, MountConfig, MountsConfig};
use crate::messages::{
    AckMsg, AgentToUfb, CacheStatsMsg, ErrorMsg, FreshnessSweepMsg, MountIdMsg,
    MountStateSnapshotMsg, MountStateUpdateMsg, TestCredentialsResultMsg, UfbToAgent,
};
use crate::orchestrator::Orchestrator;
use crate::state::MountEvent;
use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};
use tokio::sync::mpsc;

/// Top-level mount manager. Manages N mount instances.
pub struct MountService {
    mounts: HashMap<String, MountInstance>,
    ipc_tx: mpsc::Sender<AgentToUfb>,
    /// Servers with active SMB sessions — shared across all orchestrators.
    connected_servers: Arc<Mutex<HashSet<String>>>,
    /// Global cache root for sync mounts.
    cache_root: std::path::PathBuf,
    /// Per-domain caches shared with the VFS server (NFS on macOS, WinFsp
    /// on Windows) so UI-triggered drain + stats commands hit the same
    /// instance. Each orchestrator opens its own cache during
    /// SpawnSyncServer and registers it here; clears on teardown.
    /// MountService passes a clone into every orchestrator at construction.
    #[cfg(any(target_os = "macos", windows))]
    shared_caches: Option<crate::sync::SharedCaches>,
    /// Last-known `MountStateUpdateMsg` per mount, written by each
    /// orchestrator's `emit_state_update` and read by
    /// `emit_state_snapshot` on GetStates (i.e. every fresh IPC client
    /// connection). Eliminates the pre-Slice C pattern of
    /// fan-out-RequestStateUpdate where every reconnect triggered a
    /// burst of per-mount events and clients couldn't tell mounts
    /// "have been removed" from "no update yet."
    state_cache: Arc<std::sync::RwLock<HashMap<String, MountStateUpdateMsg>>>,
}

struct MountInstance {
    config: MountConfig,
    event_tx: mpsc::Sender<MountEvent>,
    /// One-shot "next mount attempt may show the OS auth dialog"
    /// permit, shared with the orchestrator (see Orchestrator docs).
    /// Set here when a Start/Restart command carries `allow_ui: true`.
    ui_permit: std::sync::Arc<std::sync::atomic::AtomicBool>,
    task_handle: tokio::task::JoinHandle<()>,
    /// Held but never `.send()`'d. Its sole purpose is to be dropped
    /// (when this MountInstance is removed from `mounts`) — the
    /// matching `oneshot::Receiver` lives in the orchestrator's
    /// select! loop, which exits cleanly the moment this sender drops.
    /// This is the "external owner gone, please exit" signal.
    _shutdown_signal: tokio::sync::oneshot::Sender<()>,
}

impl MountService {
    pub fn new(ipc_tx: mpsc::Sender<AgentToUfb>) -> Self {
        Self {
            mounts: HashMap::new(),
            ipc_tx,
            connected_servers: Arc::new(Mutex::new(HashSet::new())),
            cache_root: crate::config::MountConfig::default_cache_root(),
            #[cfg(any(target_os = "macos", windows))]
            shared_caches: None,
            state_cache: Arc::new(std::sync::RwLock::new(HashMap::new())),
        }
    }

    /// Snapshot every known mount's last broadcast state and emit it
    /// as a single MountStateSnapshotMsg. Sent in response to
    /// GetStates and after config-removals so reconnecting clients
    /// can replace their entire local mounts map authoritatively.
    async fn emit_state_snapshot(&self) {
        let mounts: Vec<MountStateUpdateMsg> = match self.state_cache.read() {
            Ok(g) => g.values().cloned().collect(),
            Err(_) => Vec::new(),
        };
        let _ = self
            .ipc_tx
            .send(AgentToUfb::MountStateSnapshot(MountStateSnapshotMsg {
                mounts,
            }))
            .await;
    }

    /// Inject the per-domain VFS cache map. Call after `new` but before
    /// `start_from_config` so UI drain/stats commands have access. The
    /// orchestrators register/clear their entries here as they spawn /
    /// tear down their VFS servers.
    #[cfg(any(target_os = "macos", windows))]
    pub fn set_shared_caches(&mut self, caches: crate::sync::SharedCaches) {
        self.shared_caches = Some(caches);
    }

    /// Load config and start all enabled mounts.
    pub async fn start_from_config(&mut self) {
        let config = config::load_config();
        self.apply_config(config).await;
    }

    /// Reload config from disk and apply changes.
    pub async fn reload_config(&mut self) {
        log::info!("Reloading config...");
        let config = config::load_config();
        self.apply_config(config).await;
    }

    /// Apply a config, starting new mounts and stopping removed ones.
    async fn apply_config(&mut self, config: MountsConfig) {
        // macOS: clean up stale entries in ~/ufb/mounts/ from removed mounts.
        // Entries may be symlinks (plain-SMB mounts point at /Volumes/<share>)
        // or real directories (NFS mount points for sync mounts). Both live in
        // the same namespace now so toggling sync on a mount doesn't move the
        // user-facing path — orphan classification has to handle both shapes.
        #[cfg(target_os = "macos")]
        {
            // Sync mounts only (1.0.7): the plain-mount symlink farm is
            // retired — the GUI stopped maintaining those symlinks, so
            // this cleanup now REMOVES them (they're no longer in the
            // active set). Sync NFS mountpoint dirs stay protected.
            let active_names: std::collections::HashSet<String> = config
                .mounts
                .iter()
                .filter(|m| m.enabled && m.sync_enabled)
                .map(|m| m.share_name())
                .collect();
            let base = config::MountConfig::volumes_base();
            if let Ok(entries) = std::fs::read_dir(&base) {
                for entry in entries.flatten() {
                    let path = entry.path();
                    let name = match path.file_name().and_then(|n| n.to_str()) {
                        Some(n) => n.to_string(),
                        None => continue,
                    };
                    if active_names.contains(&name) {
                        continue;
                    }
                    let md = match std::fs::symlink_metadata(&path) {
                        Ok(m) => m,
                        Err(_) => continue,
                    };
                    if md.file_type().is_symlink() {
                        let _ = std::fs::remove_file(&path);
                        log::info!("Removed stale symlink: {}", path.display());
                    } else if md.is_dir() {
                        // Directory entry: could be a live NFS mount point left
                        // over from a now-disabled mount, or just an empty dir.
                        if crate::sync::nfs_server::is_mounted(&path) {
                            log::info!("Umounting stale NFS mount: {}", path.display());
                            let _ = std::process::Command::new("umount")
                                .arg(&path)
                                .status();
                            // Try forced umount if the gentle one left it mounted
                            if crate::sync::nfs_server::is_mounted(&path) {
                                let _ = std::process::Command::new("umount")
                                    .arg("-f")
                                    .arg(&path)
                                    .status();
                            }
                        }
                        match std::fs::remove_dir(&path) {
                            Ok(()) => log::info!("Removed stale dir: {}", path.display()),
                            Err(e) => log::warn!(
                                "Could not remove stale dir {} (may still be mounted or non-empty): {}",
                                path.display(),
                                e
                            ),
                        }
                    }
                }
            }

            // Warn about share name collisions
            for (name, ids) in config.share_name_collisions() {
                log::warn!(
                    "Share name collision: '{}' used by mounts [{}]. Set mount_path_macos to resolve.",
                    name,
                    ids.join(", ")
                );
            }
        }

        // Detect cache root change. The orchestrators captured the old
        // cache_root at construction; the cleanest way to give them a
        // fresh one is to drop the MountInstance (drops the sender, the
        // orchestrator exits via its `None` arm) and re-create it via
        // start_mount(). Slice A made the orchestrator stay alive
        // forever otherwise, so this is the explicit recreate path.
        //
        // The orchestrator's own teardown effects (TeardownSyncServer,
        // DisconnectDrive) run as part of the Stop dispatched before
        // we drop — the channel-close on `None` only fires *after*
        // pending events drain.
        let new_cache_root = config.cache_root();
        if new_cache_root != self.cache_root && !self.mounts.is_empty() {
            log::info!(
                "Sync cache root changed: {:?} → {:?}. Recreating sync mounts.",
                self.cache_root, new_cache_root
            );
            let sync_ids: Vec<String> = self.mounts.iter()
                .filter(|(_, inst)| inst.config.sync_enabled)
                .map(|(id, _)| id.clone())
                .collect();
            for id in &sync_ids {
                if let Some(instance) = self.mounts.remove(id) {
                    // Tell the orchestrator to clean up (teardown sync
                    // server + disconnect SMB) before exiting.
                    let _ = instance.event_tx.send(MountEvent::Stop).await;
                    // Dropping `instance` drops `event_tx`; orchestrator
                    // sees the channel close after draining Stop and
                    // exits via its None arm. The new instance gets
                    // built below with the updated cache_root.
                    drop(instance);
                }
            }
            self.cache_root = new_cache_root.clone();
            // The recreated sync mounts get rebuilt by the loop below
            // (they'll hit the !mounts.contains_key branch and trigger
            // start_mount with the new cache_root).
        } else {
            self.cache_root = new_cache_root;
        }

        let new_ids: std::collections::HashSet<String> =
            config.mounts.iter().map(|m| m.id.clone()).collect();

        // Stop mounts that are no longer in config
        let to_remove: Vec<String> = self
            .mounts
            .keys()
            .filter(|id| !new_ids.contains(*id))
            .cloned()
            .collect();

        let mut pruned_any = !to_remove.is_empty();
        for id in to_remove {
            log::info!("Removing mount: {}", id);
            if let Some(instance) = self.mounts.remove(&id) {
                let _ = instance.event_tx.send(MountEvent::Stop).await;
            }
            // Slice C: drop the cached state so the snapshot we emit
            // below doesn't keep advertising the deleted mount.
            if let Ok(mut g) = self.state_cache.write() {
                g.remove(&id);
            }
        }

        // Start/update mounts from config. Unmanaged ("fake") mounts
        // are UI-only bookmarks — the agent treats them exactly like
        // disabled entries: never orchestrated, stopped if a previous
        // config had the agent managing them.
        //
        // plans/17 F1 (macOS) + slice B (Windows): plain (non-sync)
        // mounts are GUI-owned — the app mounts them directly (NetFS /
        // WNet drive letters). The agent exists purely as the sync VFS
        // host, so non-sync entries are skipped exactly like disabled
        // ones.
        for mount_config in config.mounts {
            let gui_owned =
                cfg!(any(target_os = "macos", windows)) && !mount_config.sync_enabled;
            if !mount_config.enabled || mount_config.unmanaged || gui_owned {
                // If mount exists but is now disabled, stop it
                if let Some(instance) = self.mounts.remove(&mount_config.id) {
                    log::info!("Disabling mount: {}", mount_config.id);
                    let _ = instance.event_tx.send(MountEvent::Stop).await;
                    if let Ok(mut g) = self.state_cache.write() {
                        g.remove(&mount_config.id);
                    }
                    pruned_any = true;
                }
                continue;
            }

            if let Some(instance) = self.mounts.get(&mount_config.id) {
                // Mount exists — check if config changed
                if instance.config != mount_config {
                    log::info!("Config changed for mount: {}", mount_config.id);
                    let _ = instance
                        .event_tx
                        .send(MountEvent::ConfigChanged {
                            new_config: mount_config.clone(),
                        })
                        .await;
                    // Update stored config
                    if let Some(instance) = self.mounts.get_mut(&mount_config.id) {
                        instance.config = mount_config;
                    }
                }
                continue;
            }

            self.start_mount(mount_config).await;
        }

        // Slice C: if anything was pruned (mount removed or disabled),
        // emit a fresh snapshot so reconnect-free clients drop the
        // stale entry from their local mounts map.
        if pruned_any {
            self.emit_state_snapshot().await;
        }
    }

    async fn start_mount(&mut self, config: MountConfig) {
        let mount_id = config.id.clone();

        log::info!("Starting mount: {}", mount_id);

        let (shutdown_tx, shutdown_rx) = tokio::sync::oneshot::channel::<()>();
        let mut orchestrator = Orchestrator::new(
            config.clone(),
            self.cache_root.clone(),
            self.ipc_tx.clone(),
            self.connected_servers.clone(),
            #[cfg(any(target_os = "macos", windows))]
            self.shared_caches.clone(),
            Arc::clone(&self.state_cache),
            shutdown_rx,
        );
        let event_tx = orchestrator.event_sender();
        let ui_permit = orchestrator.ui_permit_handle();

        // Run orchestrator in background task
        let id_clone = mount_id.clone();
        let task_handle = tokio::spawn(async move {
            orchestrator.run().await;
            log::info!("[{}] Orchestrator exited", id_clone);
        });

        // Store the mount instance
        self.mounts.insert(
            mount_id,
            MountInstance {
                config,
                event_tx: event_tx.clone(),
                ui_permit,
                task_handle,
                _shutdown_signal: shutdown_tx,
            },
        );
    }

    /// Arm the one-shot UI permit on a mount before routing a
    /// Start/Restart that arrived with `allow_ui: true` — the next
    /// mount attempt may then present the OS auth dialog (macOS
    /// NetAuthAgent). No-op for unknown mounts or allow_ui=false.
    fn grant_ui_permit(&self, msg: &crate::messages::MountIdMsg) {
        if !msg.allow_ui {
            return;
        }
        if let Some(instance) = self.mounts.get(&msg.mount_id) {
            log::info!("[{}] UI permit granted for next mount attempt", msg.mount_id);
            instance
                .ui_permit
                .store(true, std::sync::atomic::Ordering::SeqCst);
        }
    }

    /// Handle a command from UFB.
    pub async fn handle_command(&mut self, cmd: UfbToAgent) {
        match cmd {
            UfbToAgent::Ping => {
                let _ = self.ipc_tx.send(AgentToUfb::Pong).await;
            }
            UfbToAgent::ReloadConfig => {
                self.reload_config().await;
            }
            UfbToAgent::GetStates => {
                // Emit a single authoritative snapshot rather than
                // fanning out per-mount RequestStateUpdate events.
                // Clients use this to replace their entire mounts map
                // on (re)connection — any mount missing from the
                // snapshot has been removed and should be pruned.
                self.emit_state_snapshot().await;
            }
            UfbToAgent::StartMount(msg) => {
                self.grant_ui_permit(&msg);
                self.route_event(&msg.mount_id, MountEvent::Start, &msg.command_id)
                    .await;
            }
            UfbToAgent::StopMount(msg) => {
                self.route_event(&msg.mount_id, MountEvent::Stop, &msg.command_id)
                    .await;
            }
            UfbToAgent::RestartMount(msg) => {
                self.grant_ui_permit(&msg);
                self.route_event(&msg.mount_id, MountEvent::Restart, &msg.command_id)
                    .await;
            }
            UfbToAgent::ClearSyncCache(msg) => {
                if self.try_drain_cache(&msg).await {
                    return;
                }
                // Fall-through: orchestrator's ClearSyncCache handler does
                // per-platform teardown for legacy paths.
                self.route_event(&msg.mount_id, MountEvent::ClearSyncCache, &msg.command_id)
                    .await;
            }
            UfbToAgent::GetCacheStats(msg) => {
                self.handle_get_cache_stats(msg).await;
            }
            UfbToAgent::TestCredentials(msg) => {
                self.handle_test_credentials(msg).await;
            }
            UfbToAgent::CreateSymlinks => {
                // Slice B deleted the C:\Volumes\ufb symlink layer and
                // the elevation worker. The verb stays parseable for
                // one release (an old GUI may still send it) and no-ops.
                log::debug!("CreateSymlinks ignored (symlink layer removed in slice B)");
            }
            UfbToAgent::Quit => {
                log::info!("Quit command received via IPC, shutting down...");
                self.shutdown().await;
                std::process::exit(0);
            }
            UfbToAgent::FreshnessSweep(msg) => {
                self.handle_freshness_sweep(msg).await;
            }
        }
    }

    /// Trigger a cross-platform freshness sweep — user-driven hint that "now
    /// would be a good time to invalidate stale caches." Runs platform-native
    /// signaling rather than crawling.
    ///
    /// macOS: post the same Darwin notification the watcher uses; the
    /// extension responds with `signalEnumerator(.workingSet)` → `getChanges`
    /// drains any drift the agent has already detected.
    ///
    /// Windows: log only for now. The CF filter's `opened` and
    /// `fetch_placeholders` hooks do per-access freshness; a broader sweep
    /// would walk known-hydrated paths and stat them, which we'll add when
    /// it earns its keep against real workloads.
    async fn handle_freshness_sweep(&self, msg: FreshnessSweepMsg) {
        let domains: Vec<String> = if let Some(d) = msg.domain.clone() {
            vec![d]
        } else {
            // All currently-enabled mount share names.
            self.mounts
                .values()
                .map(|m| m.config.share_name())
                .collect()
        };

        log::info!(
            "[freshness-sweep] domains={:?} (from cmd {})",
            domains,
            msg.command_id
        );

        #[cfg(target_os = "macos")]
        {
            for d in &domains {
                crate::sync::macos_watcher::post_darwin_notification(d);
            }
        }
        #[cfg(not(target_os = "macos"))]
        {
            // Windows / Linux: no per-platform sweep wired yet. The opportunistic
            // hooks (CF `opened`, `fetch_placeholders`) cover most cases.
            let _ = &domains;
        }

        if !msg.command_id.is_empty() {
            let _ = self
                .ipc_tx
                .send(AgentToUfb::Ack(AckMsg {
                    command_id: msg.command_id,
                }))
                .await;
        }
    }

    /// Try to drain the VFS cache for a mount inline. Returns `true` when
    /// handled (NFS on macOS, ProjFS on Windows); `false` means the caller
    /// should fall back to the orchestrator path.
    #[cfg(any(target_os = "macos", windows))]
    async fn try_drain_cache(&self, msg: &MountIdMsg) -> bool {
        let Some(ref caches) = self.shared_caches else {
            return false;
        };
        let share_name = self
            .mounts
            .get(&msg.mount_id)
            .map(|instance| instance.config.share_name())
            .unwrap_or_else(|| msg.mount_id.clone());
        let cache = {
            let guard = caches.read().unwrap();
            guard.get(&share_name).cloned()
        };
        let Some(cache) = cache else {
            return false;
        };

        #[cfg(target_os = "macos")]
        let (count, bytes) = cache.drain_all().await;
        #[cfg(windows)]
        let (count, bytes) = cache.drain_all();

        log::info!(
            "[{}] Drain cache: {} files, {:.1} MB",
            msg.mount_id,
            count,
            bytes as f64 / 1_048_576.0,
        );
        if !msg.command_id.is_empty() {
            let _ = self
                .ipc_tx
                .send(AgentToUfb::Ack(AckMsg {
                    command_id: msg.command_id.clone(),
                }))
                .await;
        }
        let (hb, hc) = cache.cache_stats();
        let _ = self
            .ipc_tx
            .send(AgentToUfb::CacheStats(CacheStatsMsg {
                mount_id: msg.mount_id.clone(),
                hydrated_bytes: hb,
                hydrated_count: hc,
                command_id: String::new(),
            }))
            .await;
        true
    }

    #[cfg(not(any(target_os = "macos", windows)))]
    async fn try_drain_cache(&self, _msg: &MountIdMsg) -> bool {
        false
    }

    /// TestCredentials is a tombstone (plans/17 slice B/C): credentials
    /// are OS-owned on macOS (Keychain via NetFS) and Windows
    /// (Credential Manager via the redirector) — UFB has no store to
    /// probe. The verb stays parseable for one release; an old GUI
    /// gets a terminal "unsupported" reply so its spinner resolves.
    async fn handle_test_credentials(&self, msg: MountIdMsg) {
        let _ = self
            .ipc_tx
            .send(AgentToUfb::TestCredentialsResult(TestCredentialsResultMsg {
                mount_id: msg.mount_id,
                kind: "unsupported".into(),
                message: "Credentials are OS-owned — use the system credential store".into(),
                command_id: msg.command_id,
            }))
            .await;
    }

    async fn handle_get_cache_stats(&self, msg: MountIdMsg) {
        let (hydrated_bytes, hydrated_count) = {
            #[cfg(any(target_os = "macos", windows))]
            {
                let zeros = (0u64, 0u64);
                let share_name = self
                    .mounts
                    .get(&msg.mount_id)
                    .map(|m| m.config.share_name());
                match (share_name, self.shared_caches.as_ref()) {
                    (Some(share), Some(caches)) => {
                        let cache = caches.read().unwrap().get(&share).cloned();
                        cache.map(|c| c.cache_stats()).unwrap_or(zeros)
                    }
                    _ => zeros,
                }
            }
            #[cfg(not(any(target_os = "macos", windows)))]
            {
                let _ = &msg;
                (0u64, 0u64)
            }
        };
        let _ = self
            .ipc_tx
            .send(AgentToUfb::CacheStats(CacheStatsMsg {
                mount_id: msg.mount_id,
                hydrated_bytes,
                hydrated_count,
                command_id: msg.command_id,
            }))
            .await;
    }

    /// Route a MountEvent directly to a mount's orchestrator. The
    /// single command surface — both IPC `handle_command` and the
    /// (currently no-op on macOS) `tray_cmd_rx` arm in `main.rs` go
    /// through here. Replaces the pre-refactor `send_to_mount` +
    /// `route_event` split where IPC commands skipped the sync-server
    /// lifecycle path (making Stop/Start/Restart from the QML mount
    /// settings ineffective for sync mounts on macOS, where the Rust
    /// tray is a no-op).
    ///
    /// After Slice B the sync-server lifecycle lives inside the
    /// orchestrator's FSM (Effect::SpawnSyncServer /
    /// TeardownSyncServer). MountService just forwards the event;
    /// the orchestrator handles tear-down and respawn deterministically
    /// against its own state — no separate registry or respawn channel.
    ///
    /// `command_id` is plumbed through for the IPC Ack/Error round-trip.
    /// Empty string means "no caller wants an Ack" (today's UI behavior;
    /// Slice C will switch both QML + Swift tray to non-empty UUIDs).
    pub async fn route_event(
        &self,
        mount_id: &str,
        event: MountEvent,
        command_id: &str,
    ) {
        // Pre-flight: caller routed to an unknown mount id. Surface as
        // Error so the UI doesn't sit on a never-resolving spinner.
        if !self.mounts.contains_key(mount_id) {
            log::warn!("route_event: unknown mount {}", mount_id);
            if !command_id.is_empty() {
                let _ = self
                    .ipc_tx
                    .send(AgentToUfb::Error(ErrorMsg {
                        command_id: command_id.into(),
                        message: format!("unknown mount: {}", mount_id),
                    }))
                    .await;
            }
            return;
        }

        // After Slice B, the sync-server lifecycle lives inside the
        // orchestrator (via Effect::SpawnSyncServer /
        // Effect::TeardownSyncServer in the FSM's transition table).
        // route_event just forwards the event — no special-casing for
        // sync vs non-sync, no separate respawn channel, no registry.
        let send_ok = self.forward_to_mount(mount_id, event).await;

        // Ack on successful enqueue. The Slice C UI work will populate
        // command_id with real UUIDs; until then the empty-string guard
        // keeps wire traffic unchanged for callers that don't track
        // pending commands.
        if send_ok && !command_id.is_empty() {
            let _ = self
                .ipc_tx
                .send(AgentToUfb::Ack(AckMsg {
                    command_id: command_id.into(),
                }))
                .await;
        } else if !send_ok && !command_id.is_empty() {
            // Orchestrator's event channel returned Err — most likely
            // the run loop exited and the receiver was dropped. With
            // Slice A's "orchestrator stays alive" change this should
            // be unreachable in practice, but the Error keeps the UI
            // honest if the invariant ever breaks.
            let _ = self
                .ipc_tx
                .send(AgentToUfb::Error(ErrorMsg {
                    command_id: command_id.into(),
                    message: format!(
                        "mount orchestrator unreachable: {}",
                        mount_id
                    ),
                }))
                .await;
        }
    }

    /// Lightweight helper: forward an event to a mount's orchestrator,
    /// returning whether the enqueue succeeded. Wraps the
    /// `event_tx.send(...).await.is_ok()` pattern used in every arm of
    /// `route_event`.
    async fn forward_to_mount(&self, mount_id: &str, event: MountEvent) -> bool {
        match self.mounts.get(mount_id) {
            Some(instance) => instance.event_tx.send(event).await.is_ok(),
            None => false,
        }
    }

    /// Stop all mounts gracefully and wait for orchestrators to finish.
    ///
    /// Stop signals are sent in parallel, then each per-mount timeout
    /// is awaited concurrently via tokio::spawn so the total wait is
    /// bounded by MAX(per-mount) instead of SUM. Pre-fix Windows shut
    /// down took ~60s on a 4-mount config because the 15s timeouts
    /// ran sequentially.
    pub async fn shutdown(&mut self) {
        log::info!("Shutting down all mounts...");

        // Send Stop to all mounts.
        let mut handles = Vec::new();
        for (id, instance) in self.mounts.drain() {
            log::info!("Stopping mount: {}", id);
            let _ = instance.event_tx.send(MountEvent::Stop).await;
            handles.push((id, instance.task_handle));
        }

        // Spawn each timeout-wait as its own task so they run in
        // parallel. join_next then drains them in completion order.
        let mut set = tokio::task::JoinSet::new();
        for (id, handle) in handles {
            set.spawn(async move {
                let result =
                    tokio::time::timeout(std::time::Duration::from_secs(15), handle).await;
                (id, result)
            });
        }
        while let Some(res) = set.join_next().await {
            match res {
                Ok((id, Ok(Ok(())))) => log::info!("Mount {} shut down cleanly", id),
                Ok((id, Ok(Err(e)))) => log::error!("Mount {} task panicked: {}", id, e),
                Ok((id, Err(_))) => {
                    log::warn!("Mount {} shutdown timed out after 15s", id)
                }
                Err(e) => log::error!("Shutdown supervisor task error: {}", e),
            }
        }

        log::info!("All mounts shut down");
    }
}
