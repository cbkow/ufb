//! Mesh sync coordinator (was `mesh_sync.rs` in the Tauri build).
//!
//! Carries over the entire MeshSyncManager + sync_worker_loop +
//! broadcast/flush helpers verbatim. The only changes:
//!   * `tauri::AppHandle` storage replaced by a `MeshEventsArc` set at
//!     construction time.
//!   * `handle.emit("mesh:data-refreshed", ())` calls replaced by
//!     `events.data_refreshed()`.
//!   * Path imports updated for the new module layout
//!     (`crate::mesh::*` instead of `crate::*`).
//!   * `apply_table_change` and `build_mesh_http_client` now live in
//!     `super::commands` rather than this file.

use super::commands::{
    build_mesh_http_client, EditQueueEntry, MeshSyncStatus, SyncCommand, TableChangeQueueEntry,
};
use super::http_server::{HttpState, MeshHttpServer};
use super::peer_manager::{get_local_ip_for_farm, PeerManager};
use super::peer_outbox::{OutboundMsg, PeerOutboxes};
use super::snapshot_worker::{SnapshotRequest, SnapshotWorker};
use super::udp_notify::UdpNotify;
use super::{
    DEFAULT_MULTICAST_GROUP, DEFAULT_UDP_PORT, EDIT_QUEUE_FLUSH_INTERVAL_MS, MAX_EDIT_RETRY_COUNT,
    PEER_LOOP_INTERVAL_MS, SNAPSHOT_HEARTBEAT_INTERVAL_MS, SNAPSHOT_INTERVAL_MS,
    SNAPSHOT_MTIME_POLL_INTERVAL_MS, UDP_HEARTBEAT_INTERVAL_MS,
};
use crate::columns::ColumnConfigManager;
use crate::db::Database;
use crate::events::MeshEventsArc;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use tokio::sync::{mpsc, oneshot};

// ----------------------------------------------------------------------------
// Manager
// ----------------------------------------------------------------------------

pub struct MeshSyncManager {
    configured: bool,
    peer_manager: Arc<PeerManager>,
    udp_notify: Arc<UdpNotify>,
    is_leader: Arc<AtomicBool>,
    snapshot_needed: Arc<AtomicBool>,
    /// Latches true the first time any peer is observed alive. No longer
    /// gates snapshot writes (sidecar fencing handles the dual-leader
    /// race as of 0.7.3); still maintained for diagnostics / future use.
    has_ever_seen_peer: Arc<AtomicBool>,
    /// Latches true after the first successful `restore_snapshot` call
    /// (or immediately if no NAS snap exists at enable-time).
    has_restored_once: Arc<AtomicBool>,
    /// Mtime (ms since epoch) of the NAS snapshot file at the moment
    /// of our most recent successful restore. The periodic mtime poll
    /// uses this to detect leader writes we missed via `SnapshotNotify`.
    last_restored_snapshot_mtime: Arc<AtomicI64>,
    last_snapshot_time: Arc<AtomicU64>,
    enabled: Arc<AtomicBool>,
    command_tx: Arc<tokio::sync::Mutex<mpsc::UnboundedSender<SyncCommand>>>,
    edit_queue: Arc<tokio::sync::Mutex<Vec<EditQueueEntry>>>,
    table_change_queue: Arc<tokio::sync::Mutex<Vec<TableChangeQueueEntry>>>,
    db: Arc<Database>,
    farm_path: String,
    node_id: String,
    /// Configured control-plane HTTP port (from `settings.meshSync.httpPort`).
    http_port: u16,
    /// Configured data-plane HTTP port.
    data_port: u16,
    tags: Vec<String>,
    column_config_manager: Arc<ColumnConfigManager>,
    /// Replaces `Mutex<Option<tauri::AppHandle>>`. The trait object is
    /// supplied at construction time and lives for the manager's lifetime.
    events: MeshEventsArc,
    /// Human-readable message surfaced via `MeshSyncStatus.statusMessage`.
    status_message: Arc<StdMutex<String>>,
    task_handles: tokio::sync::Mutex<Vec<tokio::task::JoinHandle<()>>>,
    http_control_server: tokio::sync::Mutex<Option<MeshHttpServer>>,
    http_data_server: tokio::sync::Mutex<Option<MeshHttpServer>>,
    snapshot_worker: tokio::sync::Mutex<Option<SnapshotWorker>>,
    peer_outboxes: Arc<tokio::sync::Mutex<PeerOutboxes>>,
}

impl MeshSyncManager {
    pub fn new(
        farm_path: String,
        node_id: String,
        http_port: u16,
        data_port: u16,
        tags: Vec<String>,
        db: Arc<Database>,
        column_config_manager: Arc<ColumnConfigManager>,
        events: MeshEventsArc,
    ) -> Self {
        let configured = !farm_path.is_empty() && !node_id.is_empty();

        let peer_manager = Arc::new(PeerManager::new(
            farm_path.clone(),
            node_id.clone(),
            http_port,
            data_port,
            tags.clone(),
        ));

        let udp_notify = Arc::new(UdpNotify::new(
            node_id.clone(),
            DEFAULT_UDP_PORT,
            DEFAULT_MULTICAST_GROUP.to_string(),
        ));

        let (command_tx, _command_rx) = mpsc::unbounded_channel();

        let peer_outboxes_init = PeerOutboxes::new(
            build_mesh_http_client(),
            peer_manager.clone(),
        );

        Self {
            configured,
            peer_manager,
            udp_notify,
            is_leader: Arc::new(AtomicBool::new(false)),
            snapshot_needed: Arc::new(AtomicBool::new(true)),
            has_ever_seen_peer: Arc::new(AtomicBool::new(false)),
            has_restored_once: Arc::new(AtomicBool::new(false)),
            last_restored_snapshot_mtime: Arc::new(AtomicI64::new(0)),
            last_snapshot_time: Arc::new(AtomicU64::new(0)),
            enabled: Arc::new(AtomicBool::new(false)),
            command_tx: Arc::new(tokio::sync::Mutex::new(command_tx)),
            edit_queue: Arc::new(tokio::sync::Mutex::new(Vec::new())),
            table_change_queue: Arc::new(tokio::sync::Mutex::new(Vec::new())),
            db,
            farm_path,
            node_id,
            http_port,
            data_port,
            tags,
            column_config_manager,
            events,
            status_message: Arc::new(StdMutex::new(String::new())),
            task_handles: tokio::sync::Mutex::new(Vec::new()),
            http_control_server: tokio::sync::Mutex::new(None),
            http_data_server: tokio::sync::Mutex::new(None),
            snapshot_worker: tokio::sync::Mutex::new(None),
            peer_outboxes: Arc::new(tokio::sync::Mutex::new(peer_outboxes_init)),
        }
    }

    /// Start all mesh sync background tasks.
    pub async fn set_enabled(&self, enabled: bool) {
        if enabled && !self.configured {
            log::warn!("Cannot enable mesh sync: not configured");
            return;
        }

        if enabled == self.enabled.load(Ordering::Relaxed) {
            return;
        }

        if enabled {
            self.enabled.store(true, Ordering::Relaxed);
            log::info!(
                "Starting mesh sync (node: {}, farm: {})",
                self.node_id,
                self.farm_path
            );

            // Create fresh channel pair on each enable
            let (new_tx, rx) = mpsc::unbounded_channel();
            *self.command_tx.lock().await = new_tx;

            // 0. Refresh the phonebook entry FIRST — before UDP, HTTP,
            // restore, anything. The peer-discovery task that calls
            // `register_endpoint` on its own cadence (every 3s) lives
            // a few startup steps below, and the snapshot restore in
            // step 5 can stall startup on a slow SMB share. Until the
            // first peer-discovery tick fires, the phonebook still
            // carries the LAST session's IP — wrong for a mobile
            // laptop that hopped networks since last launch. Peers
            // polling the phonebook would dial the stale IP and fail
            // until we're ~3s into the loop. Eagerly publishing a
            // fresh endpoint at t=0 closes that window. spawn_blocking
            // because the write goes to SMB, which can block.
            {
                let pm = self.peer_manager.clone();
                let _ = tokio::task::spawn_blocking(move || {
                    match pm.register_endpoint() {
                        Ok(()) => log::info!("Mesh: phonebook registered with current NIC at startup"),
                        Err(e) => log::warn!("Mesh: startup phonebook register failed: {}", e),
                    }
                })
                .await;
            }

            // 1. Start UDP socket
            if let Err(e) = self.udp_notify.start().await {
                log::error!("Failed to start UDP: {}", e);
            }

            // 2. Send immediate heartbeat
            let ip = get_local_ip_for_farm(&self.farm_path);
            if let Err(e) = self
                .udp_notify
                .send_heartbeat(&ip, self.http_port, self.data_port, &self.tags)
                .await
            {
                log::warn!("Failed to send initial heartbeat: {}", e);
            }

            // 3. Start HTTP servers — separate listeners for control + data.
            let http_state = Arc::new(HttpState {
                db: self.db.clone(),
                node_id: self.node_id.clone(),
                tags: self.tags.clone(),
                peer_manager: self.peer_manager.clone(),
                is_leader: self.is_leader.clone(),
                last_snapshot_time: self.last_snapshot_time.clone(),
                enabled: self.enabled.clone(),
                command_tx: self.command_tx.clone(),
                column_config_manager: self.column_config_manager.clone(),
                events: self.events.clone(),
            });

            // Clear any stale status message from a prior attempt.
            if let Ok(mut msg) = self.status_message.lock() {
                msg.clear();
            }

            match MeshHttpServer::start_control(self.http_port, http_state.clone()).await {
                Ok(server) => {
                    *self.http_control_server.lock().await = Some(server);
                }
                Err(e) => {
                    let message = format!(
                        "HTTP control bind failed on port {}: {}",
                        self.http_port, e
                    );
                    log::error!("{}", message);
                    if let Ok(mut msg) = self.status_message.lock() {
                        *msg = message;
                    }
                }
            }
            match MeshHttpServer::start_data(self.data_port, http_state).await {
                Ok(server) => {
                    *self.http_data_server.lock().await = Some(server);
                }
                Err(e) => {
                    let message = format!(
                        "HTTP data bind failed on port {}: {}",
                        self.data_port, e
                    );
                    log::error!("{}", message);
                    if let Ok(mut msg) = self.status_message.lock() {
                        if msg.is_empty() {
                            *msg = message;
                        }
                    }
                }
            }

            // 4. Load edit queue + table change queue from disk
            self.load_edit_queue().await;
            self.load_table_change_queue().await;

            // 4-pre. Heal: stamp every legacy-NULL `modified_time` on
            // every LWW-tracked row. Idempotent — subsequent boots find
            // no NULLs and skip. Without this, the merge predicates in
            // `restore_snapshot` silently drop UPDATEs for rows that
            // pre-date the LWW columns (NULL > anything is NULL → false
            // in three-valued SQL). See `backfill_null_modified_times`
            // for the rationale.
            {
                let db = self.db.clone();
                let now = crate::utils::current_time_ms();
                let _ = tokio::task::spawn_blocking(move || {
                    let _ = db.with_conn(|c| {
                        if let Err(e) = super::snapshot_worker::backfill_null_modified_times(c, now) {
                            log::warn!("Mesh heal: NULL backfill failed: {}", e);
                        }
                        Ok(())
                    });
                })
                .await;
            }

            // 4a. Start the snapshot worker.
            match SnapshotWorker::start(
                crate::utils::get_database_path(),
                self.farm_path.clone(),
                self.node_id.clone(),
            ) {
                Ok(worker) => {
                    *self.snapshot_worker.lock().await = Some(worker);
                }
                Err(e) => {
                    log::error!("Failed to start snapshot worker: {}", e);
                }
            }

            // 4b. Reset per-peer outboxes for this enable cycle.
            *self.peer_outboxes.lock().await = PeerOutboxes::new(
                build_mesh_http_client(),
                self.peer_manager.clone(),
            );

            // 5. If snapshot exists, queue restore. Otherwise latch
            // `has_restored_once=true` immediately so the snapshot-write
            // gate doesn't block indefinitely.
            let snapshot_path = self.snapshot_path();
            let exists = tokio::task::spawn_blocking(move || snapshot_path.exists())
                .await
                .unwrap_or(false);
            if exists {
                let _ = self
                    .command_tx
                    .lock()
                    .await
                    .send(SyncCommand::RestoreSnapshot);
            } else {
                self.has_restored_once.store(true, Ordering::Relaxed);
            }

            // 6. Spawn sync worker task
            {
                let db = self.db.clone();
                let farm_path = self.farm_path.clone();
                let is_leader = self.is_leader.clone();
                let snapshot_needed = self.snapshot_needed.clone();
                let has_ever_seen_peer = self.has_ever_seen_peer.clone();
                let has_restored_once = self.has_restored_once.clone();
                let last_restored_snapshot_mtime = self.last_restored_snapshot_mtime.clone();
                let last_snapshot_time = self.last_snapshot_time.clone();
                let edit_queue = self.edit_queue.clone();
                let table_change_queue = self.table_change_queue.clone();
                let peer_manager = self.peer_manager.clone();
                let enabled = self.enabled.clone();
                let ccm = self.column_config_manager.clone();
                let events = self.events.clone();
                let snapshot_tx = self
                    .snapshot_worker
                    .lock()
                    .await
                    .as_ref()
                    .map(|w| w.sender());
                let peer_outboxes = self.peer_outboxes.clone();

                let handle = tokio::spawn(async move {
                    sync_worker_loop(
                        rx,
                        db,
                        farm_path,
                        is_leader,
                        snapshot_needed,
                        has_ever_seen_peer,
                        has_restored_once,
                        last_restored_snapshot_mtime,
                        last_snapshot_time,
                        edit_queue,
                        table_change_queue,
                        peer_manager,
                        enabled,
                        ccm,
                        events,
                        snapshot_tx,
                        peer_outboxes,
                    )
                    .await;
                });
                self.task_handles.lock().await.push(handle);
            }

            // 7. Spawn peer discovery task
            {
                let pm = self.peer_manager.clone();
                let enabled = self.enabled.clone();
                let is_leader = self.is_leader.clone();
                let cmd_tx = self.command_tx.clone();
                let peer_outboxes = self.peer_outboxes.clone();
                let has_ever_seen_peer = self.has_ever_seen_peer.clone();
                let snapshot_needed = self.snapshot_needed.clone();
                let udp = self.udp_notify.clone();
                let farm_for_loop = self.farm_path.clone();
                let http_port_for_loop = self.http_port;
                let data_port_for_loop = self.data_port;
                let tags_for_loop = self.tags.clone();
                let handle = tokio::spawn(async move {
                    let client = build_mesh_http_client();
                    let mut had_alive_peers = false;
                    // Tracks the local NIC IP we last published to the
                    // phonebook. When it changes between ticks (mobile
                    // laptop hopped networks), we log it and broadcast
                    // a fresh UDP heartbeat immediately so peers
                    // re-poll us at the new IP without waiting for
                    // the heartbeat task's cadence.
                    let mut last_known_ip: Option<String> = None;
                    // Consecutive endpoint-registration failures. The
                    // farm lives on an SMB mount the agent tears down and
                    // re-establishes (restart, reconnect), so one-off
                    // EACCES/ENOENT during that window is expected and
                    // self-heals on the next 3s tick — only a sustained
                    // streak is worth a user-visible warning.
                    let mut endpoint_fail_streak: u32 = 0;
                    loop {
                        if !enabled.load(Ordering::Relaxed) {
                            break;
                        }
                        // ── Proactive: detect NIC change ─────────────
                        // Cheap UDP-route probe; no SMB I/O. If our
                        // best route to the farm host now lands on a
                        // different local IP than last tick, the user
                        // probably just moved networks.
                        let current_ip = {
                            let farm = farm_for_loop.clone();
                            tokio::task::spawn_blocking(move || {
                                super::peer_manager::get_local_ip_for_farm(&farm)
                            })
                            .await
                            .unwrap_or_else(|_| "127.0.0.1".to_string())
                        };
                        let ip_changed = match &last_known_ip {
                            Some(prev) => prev != &current_ip,
                            None => false, // first iteration, nothing to compare to
                        };
                        if ip_changed {
                            log::warn!(
                                "Mesh: local NIC IP changed {} → {} (mobile network hop) — broadcasting fresh heartbeat",
                                last_known_ip.as_deref().unwrap_or("?"),
                                current_ip
                            );
                            if let Err(e) = udp
                                .send_heartbeat(
                                    &current_ip,
                                    http_port_for_loop,
                                    data_port_for_loop,
                                    &tags_for_loop,
                                )
                                .await
                            {
                                log::warn!(
                                    "Mesh: NIC-change heartbeat send failed: {}",
                                    e
                                );
                            }
                        }
                        last_known_ip = Some(current_ip.clone());

                        // ── Reactive: ≥2 peers failed delivery ───────
                        // Likely *our* side, not just one peer being
                        // down. Broadcast a fresh heartbeat so peers
                        // re-pull our endpoint, then clear the log so
                        // the same burst doesn't keep re-triggering.
                        // The register_endpoint call below already
                        // refreshes the phonebook every iteration, so
                        // no separate force-register is needed.
                        let recent_failures =
                            pm.distinct_recent_failures(30_000);
                        if recent_failures >= 2 {
                            log::warn!(
                                "Mesh: {} distinct peer deliveries failed in last 30s — broadcasting fresh heartbeat (likely local network shift)",
                                recent_failures
                            );
                            if let Err(e) = udp
                                .send_heartbeat(
                                    &current_ip,
                                    http_port_for_loop,
                                    data_port_for_loop,
                                    &tags_for_loop,
                                )
                                .await
                            {
                                log::warn!(
                                    "Mesh: failure-burst heartbeat send failed: {}",
                                    e
                                );
                            }
                            pm.clear_delivery_failures();
                        }

                        // Offload synchronous std::fs I/O against the farm
                        // path (SMB) to the blocking pool.
                        {
                            let pm_c = pm.clone();
                            match tokio::task::spawn_blocking(move || pm_c.register_endpoint()).await {
                                Ok(Ok(())) => {
                                    if endpoint_fail_streak >= 5 {
                                        log::info!(
                                            "Mesh: endpoint registration recovered after {} failed attempts",
                                            endpoint_fail_streak
                                        );
                                    }
                                    endpoint_fail_streak = 0;
                                }
                                Ok(Err(e)) => {
                                    endpoint_fail_streak += 1;
                                    // ~15s of continuous failure before the
                                    // first warning; then every ~90s so a
                                    // genuinely unreachable farm stays
                                    // visible without spamming.
                                    if endpoint_fail_streak == 5
                                        || endpoint_fail_streak % 30 == 0
                                    {
                                        log::warn!(
                                            "Failed to register endpoint ({} consecutive): {}",
                                            endpoint_fail_streak, e
                                        );
                                    } else {
                                        log::debug!("Failed to register endpoint (streak {}): {}", endpoint_fail_streak, e);
                                    }
                                }
                                Err(e) => log::warn!("register_endpoint join error: {}", e),
                            }
                        }
                        {
                            let pm_c = pm.clone();
                            match tokio::task::spawn_blocking(move || pm_c.discover_peers()).await {
                                Ok(Ok(_)) => {}
                                Ok(Err(e)) => log::warn!("Failed to discover peers: {}", e),
                                Err(e) => log::warn!("discover_peers join error: {}", e),
                            }
                        }
                        pm.poll_peers(&client).await;
                        let was_leader = is_leader.load(Ordering::Relaxed);
                        pm.run_election();
                        let now_leader = pm.is_leader();
                        is_leader.store(now_leader, Ordering::Relaxed);
                        if was_leader != now_leader {
                            log::info!(
                                "Leadership changed: now {} (peers: {})",
                                if now_leader { "LEADER" } else { "FOLLOWER" },
                                pm.get_peer_count()
                            );
                            // Edge trigger on the false→true transition:
                            // arm the snapshot writer immediately so a
                            // freshly-elected leader publishes within
                            // one tick instead of waiting up to a full
                            // SNAPSHOT_HEARTBEAT_INTERVAL_MS for the
                            // existing re-arm. Defends against the
                            // "leader rebooted into a disarmed writer"
                            // failure mode.
                            if !was_leader && now_leader {
                                snapshot_needed.store(true, Ordering::Relaxed);
                            }
                        }

                        // Sync the per-peer outboxes with the current peer list.
                        {
                            let mut outboxes = peer_outboxes.lock().await;
                            let peers = pm.get_peers();
                            let live_ids: std::collections::HashSet<String> =
                                peers.iter().map(|p| p.node_id.clone()).collect();
                            for p in &peers {
                                if p.endpoint.data_port == 0 {
                                    continue;
                                }
                                if !outboxes.has(&p.node_id) {
                                    outboxes.add(p.endpoint.clone()).await;
                                }
                            }
                            let managed: Vec<String> =
                                peers.iter().map(|p| p.node_id.clone()).collect();
                            let to_remove: Vec<String> = managed
                                .iter()
                                .filter(|id| !live_ids.contains(*id))
                                .cloned()
                                .collect();
                            for id in to_remove {
                                outboxes.remove(&id).await;
                            }
                        }

                        let alive_count = pm.get_alive_peers().len();
                        let has_alive_peers = alive_count > 0;

                        if has_alive_peers {
                            has_ever_seen_peer.store(true, Ordering::Relaxed);
                        }

                        if has_alive_peers && !had_alive_peers {
                            if now_leader {
                                log::info!("Sync: Peer(s) reappeared, triggering snapshot for catch-up");
                                let _ = cmd_tx.lock().await.send(SyncCommand::TakeSnapshot);
                            } else {
                                log::info!("Sync: Peers reappeared after isolation, re-restoring snapshot");
                                let _ = cmd_tx.lock().await.send(SyncCommand::RestoreSnapshot);
                            }
                        }
                        had_alive_peers = has_alive_peers;

                        {
                            let pm_c = pm.clone();
                            if let Err(e) =
                                tokio::task::spawn_blocking(move || pm_c.cleanup_stale_peers()).await
                            {
                                log::warn!("cleanup_stale_peers join error: {}", e);
                            }
                        }
                        tokio::time::sleep(std::time::Duration::from_millis(
                            PEER_LOOP_INTERVAL_MS,
                        ))
                        .await;
                    }
                });
                self.task_handles.lock().await.push(handle);
            }

            // 8. Spawn UDP heartbeat/listener task
            {
                let udp = self.udp_notify.clone();
                let pm = self.peer_manager.clone();
                let enabled = self.enabled.clone();
                let http_port = self.http_port;
                let data_port = self.data_port;
                let tags = self.tags.clone();
                let farm_path_for_udp = self.farm_path.clone();
                let handle = tokio::spawn(async move {
                    let mut heartbeat_interval = tokio::time::interval(
                        std::time::Duration::from_millis(UDP_HEARTBEAT_INTERVAL_MS),
                    );
                    let mut poll_interval =
                        tokio::time::interval(std::time::Duration::from_millis(200));

                    loop {
                        if !enabled.load(Ordering::Relaxed) {
                            break;
                        }

                        tokio::select! {
                            _ = heartbeat_interval.tick() => {
                                let ip = get_local_ip_for_farm(&farm_path_for_udp);
                                if let Err(e) = udp.send_heartbeat(&ip, http_port, data_port, &tags).await {
                                    log::warn!("Heartbeat send failed: {}", e);
                                }
                            }
                            _ = poll_interval.tick() => {
                                let messages = udp.poll().await;
                                for msg in messages {
                                    let msg_type = msg.get("t").and_then(|v| v.as_str()).unwrap_or("");
                                    match msg_type {
                                        "hb" => {
                                            let node_id = msg.get("n").or(msg.get("from"))
                                                .and_then(|v| v.as_str()).unwrap_or("");
                                            let ip = msg.get("ip").and_then(|v| v.as_str()).unwrap_or("");
                                            let port = msg.get("port").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                                            let data_port = msg.get("data_port").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                                            let mesh_epoch = msg.get("epoch").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
                                            let tags: Vec<String> = msg.get("tags")
                                                .and_then(|v| v.as_array())
                                                .map(|arr| arr.iter().filter_map(|v| v.as_str().map(String::from)).collect())
                                                .unwrap_or_default();
                                            if !node_id.is_empty() {
                                                pm.process_heartbeat(node_id, ip, port, data_port, mesh_epoch, &tags);
                                            }
                                        }
                                        "bye" => {
                                            let node_id = msg.get("n").or(msg.get("from"))
                                                .and_then(|v| v.as_str()).unwrap_or("");
                                            if !node_id.is_empty() {
                                                pm.process_goodbye(node_id);
                                            }
                                        }
                                        _ => {}
                                    }
                                }
                            }
                        }
                    }
                });
                self.task_handles.lock().await.push(handle);
            }

            log::info!("Mesh sync started");
        } else {
            self.shutdown().await;
        }
    }

    /// Graceful shutdown of all mesh sync tasks.
    pub async fn shutdown(&self) {
        if !self.enabled.load(Ordering::Relaxed) {
            return;
        }

        log::info!("Shutting down mesh sync...");

        let _ = self.command_tx.lock().await.send(SyncCommand::Shutdown);
        self.enabled.store(false, Ordering::Relaxed);

        self.udp_notify.stop(&self.tags).await;

        if let Some(mut server) = self.http_control_server.lock().await.take() {
            server.stop().await;
        }
        if let Some(mut server) = self.http_data_server.lock().await.take() {
            server.stop().await;
        }

        if let Some(mut worker) = self.snapshot_worker.lock().await.take() {
            worker.shutdown().await;
        }
        self.peer_outboxes.lock().await.shutdown_all().await;

        self.save_edit_queue().await;
        self.save_table_change_queue().await;

        {
            let pm_c = self.peer_manager.clone();
            if let Err(e) = tokio::task::spawn_blocking(move || pm_c.unregister_endpoint()).await {
                log::warn!("unregister_endpoint join error: {}", e);
            }
        }

        let mut handles = self.task_handles.lock().await;
        for handle in handles.drain(..) {
            handle.abort();
        }

        log::info!("Mesh sync shutdown complete");
    }

    /// Queue a metadata edit for sync.
    pub async fn on_metadata_edited(
        &self,
        job_path: &str,
        item_path: &str,
        metadata_json: &str,
        folder_name: &str,
        is_tracked: bool,
        modified_time: i64,
    ) {
        if !self.enabled.load(Ordering::Relaxed) {
            log::debug!("Sync: on_metadata_edited called but sync is disabled");
            return;
        }
        log::info!("Sync: on_metadata_edited queuing for {}", item_path);
        let _ = self
            .command_tx
            .lock()
            .await
            .send(SyncCommand::EditMetadata {
                job_path: job_path.to_string(),
                item_path: item_path.to_string(),
                metadata_json: metadata_json.to_string(),
                folder_name: folder_name.to_string(),
                is_tracked,
                modified_time,
            });
    }

    /// Queue a table change (subscription or column) for sync.
    pub async fn on_table_changed(&self, change_json: &str) {
        if !self.enabled.load(Ordering::Relaxed) {
            return;
        }
        log::info!("Sync: on_table_changed: {}", change_json);
        let _ = self
            .command_tx
            .lock()
            .await
            .send(SyncCommand::TableChange {
                change_json: change_json.to_string(),
            });
    }

    pub async fn trigger_flush_edits(&self) {
        let _ = self
            .command_tx
            .lock()
            .await
            .send(SyncCommand::FlushEditQueue);
    }

    pub async fn trigger_snapshot(&self) {
        let _ = self
            .command_tx
            .lock()
            .await
            .send(SyncCommand::TakeSnapshot);
    }

    /// Queue a fresh restore of the leader's shared snapshot into the local
    /// DB. Used by the reset flow so a node can re-sync cleanly after its
    /// local column/preset tables are wiped.
    pub async fn trigger_restore_snapshot(&self) {
        let _ = self
            .command_tx
            .lock()
            .await
            .send(SyncCommand::RestoreSnapshot);
    }

    /// Discard all pending table changes (follower-only state).
    pub async fn clear_table_change_queue(&self) {
        self.table_change_queue.lock().await.clear();
        self.save_table_change_queue().await;
    }

    /// Mark that a snapshot is needed (e.g. after subscription/column changes).
    pub fn mark_snapshot_needed(&self) {
        self.snapshot_needed.store(true, Ordering::Relaxed);
    }

    pub fn get_status(&self) -> MeshSyncStatus {
        let pm = &self.peer_manager;
        let pending = self.edit_queue.try_lock().map(|q| q.len()).unwrap_or(0);
        let snap_time = self.last_snapshot_time.load(Ordering::Relaxed);
        let sticky = self
            .status_message
            .lock()
            .ok()
            .map(|g| g.clone())
            .unwrap_or_default();
        MeshSyncStatus {
            is_leader: self.is_leader.load(Ordering::Relaxed),
            leader_id: pm.get_current_leader_id().unwrap_or_default(),
            peer_count: pm.get_peer_count(),
            last_snapshot_time: if snap_time > 0 {
                Some(snap_time as i64)
            } else {
                None
            },
            pending_edits_count: pending,
            status_message: if !sticky.is_empty() {
                sticky
            } else if self.enabled.load(Ordering::Relaxed) {
                if self.is_leader.load(Ordering::Relaxed) {
                    "Leader".to_string()
                } else {
                    "Follower".to_string()
                }
            } else {
                "Disabled".to_string()
            },
            is_enabled: self.enabled.load(Ordering::Relaxed),
            is_configured: self.configured,
        }
    }

    pub fn peer_manager(&self) -> &Arc<PeerManager> {
        &self.peer_manager
    }

    pub fn farm_path(&self) -> &str {
        &self.farm_path
    }

    pub fn node_id(&self) -> &str {
        &self.node_id
    }

    pub fn http_port(&self) -> u16 {
        self.http_port
    }

    pub fn is_enabled(&self) -> bool {
        self.enabled.load(Ordering::Relaxed)
    }

    // ── Snapshot path ──

    fn snapshot_path(&self) -> PathBuf {
        super::snapshot_file_path(&self.farm_path)
    }

    // ── Edit queue persistence ──

    fn edit_queue_path(&self) -> PathBuf {
        crate::utils::get_app_data_dir().join("edit_queue.json")
    }

    async fn save_edit_queue(&self) {
        let queue = self.edit_queue.lock().await;
        if queue.is_empty() {
            let _ = std::fs::remove_file(self.edit_queue_path());
            return;
        }
        match serde_json::to_string_pretty(&*queue) {
            Ok(json) => {
                if let Err(e) = std::fs::write(self.edit_queue_path(), json) {
                    log::error!("Failed to save edit queue: {}", e);
                }
            }
            Err(e) => log::error!("Failed to serialize edit queue: {}", e),
        }
    }

    async fn load_edit_queue(&self) {
        let path = self.edit_queue_path();
        if !path.exists() {
            return;
        }
        match std::fs::read_to_string(&path) {
            Ok(json) => match serde_json::from_str::<Vec<EditQueueEntry>>(&json) {
                Ok(entries) => {
                    let count = entries.len();
                    *self.edit_queue.lock().await = entries;
                    if count > 0 {
                        log::info!("Loaded {} entries from edit queue", count);
                    }
                }
                Err(e) => log::warn!("Failed to parse edit queue: {}", e),
            },
            Err(e) => log::warn!("Failed to read edit queue: {}", e),
        }
    }

    // ── Table change queue persistence ──

    fn table_change_queue_path(&self) -> PathBuf {
        crate::utils::get_app_data_dir().join("table_change_queue.json")
    }

    async fn save_table_change_queue(&self) {
        let queue = self.table_change_queue.lock().await;
        if queue.is_empty() {
            let _ = std::fs::remove_file(self.table_change_queue_path());
            return;
        }
        match serde_json::to_string_pretty(&*queue) {
            Ok(json) => {
                if let Err(e) = std::fs::write(self.table_change_queue_path(), json) {
                    log::error!("Failed to save table change queue: {}", e);
                }
            }
            Err(e) => log::error!("Failed to serialize table change queue: {}", e),
        }
    }

    async fn load_table_change_queue(&self) {
        let path = self.table_change_queue_path();
        if !path.exists() {
            return;
        }
        match std::fs::read_to_string(&path) {
            Ok(json) => match serde_json::from_str::<Vec<TableChangeQueueEntry>>(&json) {
                Ok(entries) => {
                    let count = entries.len();
                    *self.table_change_queue.lock().await = entries;
                    if count > 0 {
                        log::info!("Loaded {} entries from table change queue", count);
                    }
                }
                Err(e) => log::warn!("Failed to parse table change queue: {}", e),
            },
            Err(e) => log::warn!("Failed to read table change queue: {}", e),
        }
    }
}

// ----------------------------------------------------------------------------
// Sync worker loop
// ----------------------------------------------------------------------------

#[allow(clippy::too_many_arguments)]
async fn sync_worker_loop(
    mut rx: mpsc::UnboundedReceiver<SyncCommand>,
    _db: Arc<Database>,
    farm_path: String,
    is_leader: Arc<AtomicBool>,
    snapshot_needed: Arc<AtomicBool>,
    _has_ever_seen_peer: Arc<AtomicBool>,
    has_restored_once: Arc<AtomicBool>,
    last_restored_snapshot_mtime: Arc<AtomicI64>,
    last_snapshot_time: Arc<AtomicU64>,
    edit_queue: Arc<tokio::sync::Mutex<Vec<EditQueueEntry>>>,
    table_change_queue: Arc<tokio::sync::Mutex<Vec<TableChangeQueueEntry>>>,
    peer_manager: Arc<PeerManager>,
    enabled: Arc<AtomicBool>,
    _column_config_manager: Arc<ColumnConfigManager>,
    events: MeshEventsArc,
    snapshot_tx: Option<mpsc::Sender<SnapshotRequest>>,
    peer_outboxes: Arc<tokio::sync::Mutex<PeerOutboxes>>,
) {
    let client = build_mesh_http_client();
    let mut last_snapshot_check = tokio::time::Instant::now();
    let mut last_flush_check = tokio::time::Instant::now();
    let mut last_mtime_check = tokio::time::Instant::now();
    let mut last_heartbeat_check = tokio::time::Instant::now();

    log::info!("Sync worker loop started");

    loop {
        tokio::select! {
            Some(cmd) = rx.recv() => {
                match cmd {
                    SyncCommand::EditMetadata { job_path, item_path, metadata_json, folder_name, is_tracked, modified_time } => {
                        log::info!("Sync: EditMetadata for {} (leader={})", item_path, is_leader.load(Ordering::Relaxed));
                        if is_leader.load(Ordering::Relaxed) {
                            peer_outboxes.lock().await.broadcast(OutboundMsg::MetadataEdit {
                                job_path: job_path.clone(),
                                item_path: item_path.clone(),
                                metadata_json: metadata_json.clone(),
                                folder_name: folder_name.clone(),
                                is_tracked,
                                modified_time,
                            });
                            snapshot_needed.store(true, Ordering::Relaxed);
                        } else {
                            // Follower: POST to leader, on failure queue
                            if let Some(leader_ep) = peer_manager.get_leader_endpoint() {
                                let url = format!(
                                    "http://{}:{}/api/metadata/update",
                                    leader_ep.ip, leader_ep.data_port
                                );
                                log::info!("Sync: Follower posting edit to leader at {}", url);
                                let body = serde_json::json!({
                                    "job_path": job_path,
                                    "item_path": item_path,
                                    "metadata": metadata_json,
                                    "folder_name": folder_name,
                                    "is_tracked": is_tracked,
                                    "modified_time": modified_time,
                                    "mesh_epoch": super::MESH_EPOCH,
                                });
                                let req = client.post(&url).json(&body);
                                match req.timeout(std::time::Duration::from_secs(5)).send().await {
                                    Ok(resp) if resp.status().is_success() => {
                                        log::info!("Sync: Edit sent to leader OK");
                                    }
                                    Ok(resp) => {
                                        log::warn!("Sync: Leader returned {}, queuing edit", resp.status());
                                        let mut queue = edit_queue.lock().await;
                                        queue.push(EditQueueEntry {
                                            job_path, item_path, metadata_json, folder_name, is_tracked, modified_time,
                                            queued_time: crate::utils::current_time_ms(), retry_count: 0,
                                        });
                                    }
                                    Err(e) => {
                                        log::warn!(
                                            "Sync: Failed to reach leader: {} (is_connect={}, is_timeout={}, is_request={}), queuing edit",
                                            super::peer_outbox::reqwest_error_chain(&e),
                                            e.is_connect(),
                                            e.is_timeout(),
                                            e.is_request(),
                                        );
                                        let mut queue = edit_queue.lock().await;
                                        queue.push(EditQueueEntry {
                                            job_path, item_path, metadata_json, folder_name, is_tracked, modified_time,
                                            queued_time: crate::utils::current_time_ms(), retry_count: 0,
                                        });
                                    }
                                }
                            } else {
                                log::warn!("Sync: No leader known, queuing edit for {}", item_path);
                                let mut queue = edit_queue.lock().await;
                                queue.push(EditQueueEntry {
                                    job_path, item_path, metadata_json, folder_name, is_tracked, modified_time,
                                    queued_time: crate::utils::current_time_ms(), retry_count: 0,
                                });
                            }
                        }
                    }
                    SyncCommand::BroadcastEdit { job_path, item_path, metadata_json, folder_name, is_tracked, modified_time } => {
                        peer_outboxes.lock().await.broadcast(OutboundMsg::MetadataEdit {
                            job_path, item_path, metadata_json, folder_name, is_tracked, modified_time,
                        });
                        snapshot_needed.store(true, Ordering::Relaxed);
                    }
                    SyncCommand::TableChange { change_json } => {
                        handle_table_change(&client, &peer_manager, &is_leader, &snapshot_needed, &table_change_queue, &peer_outboxes, &change_json).await;
                    }
                    SyncCommand::BroadcastTableChange { change_json } => {
                        peer_outboxes.lock().await.broadcast(OutboundMsg::TableChange { change_json });
                        snapshot_needed.store(true, Ordering::Relaxed);
                    }
                    SyncCommand::TakeSnapshot => {
                        if !is_leader.load(Ordering::Relaxed) {
                            log::info!("Sync: Ignoring TakeSnapshot — not the leader");
                            continue;
                        }
                        if !has_restored_once.load(Ordering::Relaxed) {
                            log::info!("Sync: Deferring TakeSnapshot until startup restore completes");
                            continue;
                        }
                        if let Some(ref tx) = snapshot_tx {
                            let (reply, _) = oneshot::channel();
                            match tx.try_send(SnapshotRequest::Take { reply }) {
                                Ok(()) => {
                                    log::info!("Sync: TakeSnapshot dispatched to worker (catch-up / on-demand path)");
                                    snapshot_needed.store(false, Ordering::Relaxed);
                                    last_snapshot_time
                                        .store(crate::utils::current_time_ms() as u64, Ordering::Relaxed);
                                    peer_outboxes.lock().await.broadcast(OutboundMsg::SnapshotNotify);
                                }
                                Err(e) => {
                                    log::warn!("Sync: SnapshotWorker take send failed: {:?}", e);
                                }
                            }
                        } else {
                            log::warn!("Sync: TakeSnapshot received but snapshot_tx is None (worker never started)");
                        }
                    }
                    SyncCommand::RestoreSnapshot => {
                        let is_startup = !has_restored_once.load(Ordering::Relaxed);
                        let allow = !is_leader.load(Ordering::Relaxed) || is_startup;
                        if !allow {
                            log::debug!("Sync: RestoreSnapshot skipped (established leader)");
                        } else {
                            if is_startup && is_leader.load(Ordering::Relaxed) {
                                log::info!("Sync: running startup restore despite leader=true (has_restored_once=false) — merging NAS snap before asserting leadership");
                            }
                            flush_table_change_queue(
                                &client, &table_change_queue, &peer_manager, &is_leader,
                            ).await;
                            if let Some(ref tx) = snapshot_tx {
                                let (reply, reply_rx) = oneshot::channel();
                                if tx
                                    .send(SnapshotRequest::Restore {
                                        include_column_tables: true,
                                        reply,
                                    })
                                    .await
                                    .is_ok()
                                {
                                    match reply_rx.await {
                                        Ok(Ok(())) => {
                                            snapshot_needed.store(false, Ordering::Relaxed);
                                            has_restored_once.store(true, Ordering::Relaxed);
                                            let snap_path = super::snapshot_file_path(&farm_path);
                                            let mtime = tokio::task::spawn_blocking(move || {
                                                std::fs::metadata(&snap_path)
                                                    .ok()
                                                    .and_then(|m| m.modified().ok())
                                                    .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                                                    .map(|d| d.as_millis() as i64)
                                                    .unwrap_or(0)
                                            })
                                            .await
                                            .unwrap_or(0);
                                            last_restored_snapshot_mtime.store(mtime, Ordering::Relaxed);
                                            events.data_refreshed();
                                        }
                                        Ok(Err(e)) => {
                                            // Restore failed (transient SMB hiccup, missing
                                            // file, schema mismatch, …). We DON'T want this
                                            // to permanently disarm the leader's snapshot
                                            // writer — `has_restored_once` is gating that
                                            // path and a single bad startup would otherwise
                                            // leave the leader silent forever (observed: a
                                            // 10-day snapshot gap with the leader still
                                            // heartbeating). Treat "we tried" as enough to
                                            // arm the writer; the next snapshot cycle will
                                            // republish whatever local state we have.
                                            has_restored_once.store(true, Ordering::Relaxed);
                                            log::warn!("Snapshot restore failed: {} (writer armed anyway)", e);
                                        }
                                        Err(_) => {
                                            // Worker channel dropped — same reasoning.
                                            has_restored_once.store(true, Ordering::Relaxed);
                                            log::warn!("Snapshot worker dropped reply (writer armed anyway)");
                                        }
                                    }
                                }
                            }
                        }
                    }
                    SyncCommand::FlushEditQueue => {
                        flush_edit_queue(&client, &edit_queue, &peer_manager, &is_leader, &events).await;
                    }
                    SyncCommand::Shutdown => {
                        log::info!("Sync worker received shutdown");
                        break;
                    }
                }
            }
            _ = tokio::time::sleep(std::time::Duration::from_millis(500)) => {
                if !enabled.load(Ordering::Relaxed) {
                    break;
                }
                let now = tokio::time::Instant::now();

                // Heartbeat: every SNAPSHOT_HEARTBEAT_INTERVAL_MS the
                // leader force-flips snapshot_needed=true so the gate
                // below fires. Guarantees a fresh NAS snapshot even on
                // days with zero edits — the take_snapshot worker will
                // skip cleanly if all syncable tables are empty, and
                // the meta.json yield_window prevents thrash on dual
                // -leader setups.
                if is_leader.load(Ordering::Relaxed)
                    && has_restored_once.load(Ordering::Relaxed)
                    && now.duration_since(last_heartbeat_check).as_millis()
                        >= SNAPSHOT_HEARTBEAT_INTERVAL_MS as u128
                {
                    last_heartbeat_check = now;
                    if !snapshot_needed.load(Ordering::Relaxed) {
                        log::info!(
                            "Snapshot: heartbeat tick — re-arming snapshot_needed (no edits in {}ms)",
                            SNAPSHOT_HEARTBEAT_INTERVAL_MS
                        );
                        snapshot_needed.store(true, Ordering::Relaxed);
                    }
                }

                if is_leader.load(Ordering::Relaxed)
                    && snapshot_needed.load(Ordering::Relaxed)
                    && has_restored_once.load(Ordering::Relaxed)
                    && now.duration_since(last_snapshot_check).as_millis() >= SNAPSHOT_INTERVAL_MS as u128
                {
                    if let Some(ref tx) = snapshot_tx {
                        let (reply, _) = oneshot::channel();
                        if tx.try_send(SnapshotRequest::Take { reply }).is_ok() {
                            snapshot_needed.store(false, Ordering::Relaxed);
                            last_snapshot_time
                                .store(crate::utils::current_time_ms() as u64, Ordering::Relaxed);
                            last_snapshot_check = now;
                            peer_outboxes.lock().await.broadcast(OutboundMsg::SnapshotNotify);
                            log::info!("Snapshot: leader dispatched periodic snapshot request");
                        } else {
                            log::warn!("Snapshot: worker channel full, retrying next tick");
                        }
                    } else {
                        log::warn!("Snapshot: leader has snapshot_needed=true but worker isn't started");
                    }
                } else if is_leader.load(Ordering::Relaxed)
                    && snapshot_needed.load(Ordering::Relaxed)
                    && !has_restored_once.load(Ordering::Relaxed)
                    && now.duration_since(last_snapshot_check).as_millis() >= SNAPSHOT_INTERVAL_MS as u128
                {
                    last_snapshot_check = now;
                    log::info!("Snapshot: leader deferring snapshot write until startup restore completes (has_restored_once=false)");
                }

                if !is_leader.load(Ordering::Relaxed)
                    && now.duration_since(last_flush_check).as_millis() >= EDIT_QUEUE_FLUSH_INTERVAL_MS as u128
                {
                    flush_edit_queue(&client, &edit_queue, &peer_manager, &is_leader, &events).await;
                    flush_table_change_queue(&client, &table_change_queue, &peer_manager, &is_leader).await;
                    last_flush_check = now;
                }

                if !is_leader.load(Ordering::Relaxed)
                    && has_restored_once.load(Ordering::Relaxed)
                    && now.duration_since(last_mtime_check).as_millis() >= SNAPSHOT_MTIME_POLL_INTERVAL_MS as u128
                {
                    last_mtime_check = now;
                    let snap_path = super::snapshot_file_path(&farm_path);
                    let mtime = tokio::task::spawn_blocking(move || {
                        std::fs::metadata(&snap_path)
                            .ok()
                            .and_then(|m| m.modified().ok())
                            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                            .map(|d| d.as_millis() as i64)
                            .unwrap_or(0)
                    })
                    .await
                    .unwrap_or(0);
                    let last = last_restored_snapshot_mtime.load(Ordering::Relaxed);
                    if mtime > 0 && mtime > last {
                        log::info!(
                            "Snapshot: mtime poll detected fresh NAS snap (mtime={} > last_restored={}), restoring",
                            mtime, last
                        );
                        if let Some(ref tx) = snapshot_tx {
                            let (reply, reply_rx) = oneshot::channel();
                            if tx
                                .send(SnapshotRequest::Restore {
                                    include_column_tables: true,
                                    reply,
                                })
                                .await
                                .is_ok()
                            {
                                match reply_rx.await {
                                    Ok(Ok(())) => {
                                        snapshot_needed.store(false, Ordering::Relaxed);
                                        last_restored_snapshot_mtime.store(mtime, Ordering::Relaxed);
                                        events.data_refreshed();
                                    }
                                    Ok(Err(e)) => log::warn!("Snapshot mtime-poll restore failed: {}", e),
                                    Err(_) => log::warn!("Snapshot worker dropped reply (mtime poll)"),
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    log::info!("Sync worker loop exited");
}

// ----------------------------------------------------------------------------
// Broadcast / flush helpers
// ----------------------------------------------------------------------------

#[allow(dead_code)]
async fn broadcast_edit(
    client: &reqwest::Client,
    peer_manager: &Arc<PeerManager>,
    job_path: &str,
    item_path: &str,
    metadata_json: &str,
    folder_name: &str,
    is_tracked: bool,
    modified_time: i64,
) {
    let peers = peer_manager.get_alive_peers();
    log::info!(
        "Sync: Broadcasting edit for {} to {} alive peers",
        item_path,
        peers.len()
    );
    let body = serde_json::json!({
        "job_path": job_path,
        "item_path": item_path,
        "metadata": metadata_json,
        "folder_name": folder_name,
        "is_tracked": is_tracked,
        "modified_time": modified_time,
        "mesh_epoch": super::MESH_EPOCH,
    });

    for peer in &peers {
        let url = format!(
            "http://{}:{}/api/metadata/update",
            peer.endpoint.ip, peer.endpoint.data_port
        );
        let req = client.post(&url).json(&body);
        match req.timeout(std::time::Duration::from_secs(5)).send().await {
            Ok(resp) if resp.status().is_success() => {
                log::info!("Sync: Broadcast to {} OK", peer.node_id);
            }
            Ok(resp) => {
                log::warn!(
                    "Sync: Broadcast to {} returned {}",
                    peer.node_id,
                    resp.status()
                );
            }
            Err(e) => {
                log::warn!("Sync: Broadcast to {} failed: {}", peer.node_id, e);
            }
        }
    }
}

async fn flush_edit_queue(
    client: &reqwest::Client,
    edit_queue: &Arc<tokio::sync::Mutex<Vec<EditQueueEntry>>>,
    peer_manager: &Arc<PeerManager>,
    is_leader: &Arc<AtomicBool>,
    events: &MeshEventsArc,
) {
    let mut queue = edit_queue.lock().await;
    if queue.is_empty() {
        return;
    }

    let leader_ep = if is_leader.load(Ordering::Relaxed) {
        None
    } else {
        peer_manager.get_leader_endpoint()
    };

    let mut remaining = Vec::new();

    for mut entry in queue.drain(..) {
        if entry.retry_count >= MAX_EDIT_RETRY_COUNT {
            // Silent data loss otherwise — this edit never reached its
            // peer. Surface it so the user knows their change didn't
            // propagate (audit: HIGH-severity "stale follower queue
            // dropped without error").
            events.sync_warning(
                "edit_dropped",
                &format!(
                    "An edit to “{}” could not be delivered after {} retries and was dropped — it did not reach other machines.",
                    entry.item_path, entry.retry_count
                ),
            );
            continue;
        }

        let success = if is_leader.load(Ordering::Relaxed) {
            broadcast_edit(
                client,
                peer_manager,
                &entry.job_path,
                &entry.item_path,
                &entry.metadata_json,
                &entry.folder_name,
                entry.is_tracked,
                entry.modified_time,
            )
            .await;
            true
        } else if let Some(ref ep) = leader_ep {
            let url = format!(
                "http://{}:{}/api/metadata/update",
                ep.ip, ep.data_port
            );
            let body = serde_json::json!({
                "job_path": entry.job_path,
                "item_path": entry.item_path,
                "metadata": entry.metadata_json,
                "folder_name": entry.folder_name,
                "is_tracked": entry.is_tracked,
                "modified_time": entry.modified_time,
                "mesh_epoch": super::MESH_EPOCH,
            });
            let req = client.post(&url).json(&body);
            matches!(
                req.timeout(std::time::Duration::from_secs(5)).send().await,
                Ok(resp) if resp.status().is_success()
            )
        } else {
            false
        };

        if !success {
            entry.retry_count += 1;
            remaining.push(entry);
        }
    }

    *queue = remaining;
}

async fn try_post_table_change_to_leader(
    client: &reqwest::Client,
    peer_manager: &Arc<PeerManager>,
    change_json: &str,
) -> bool {
    let leader_ep = match peer_manager.get_leader_endpoint() {
        Some(ep) => ep,
        None => return false,
    };
    let mut body: serde_json::Value = match serde_json::from_str(change_json) {
        Ok(v) => v,
        Err(e) => {
            log::warn!("Sync: Invalid table change JSON, dropping: {}", e);
            return true;
        }
    };
    if let Some(obj) = body.as_object_mut() {
        obj.insert(
            "mesh_epoch".to_string(),
            serde_json::Value::from(super::MESH_EPOCH),
        );
    }
    let url = format!(
        "http://{}:{}/api/table/update",
        leader_ep.ip, leader_ep.data_port
    );
    let req = client.post(&url).json(&body);
    match req.timeout(std::time::Duration::from_secs(5)).send().await {
        Ok(resp) if resp.status().is_success() => true,
        Ok(resp) => {
            log::warn!("Sync: Leader returned {} for table change", resp.status());
            false
        }
        Err(e) => {
            log::warn!(
                "Sync: Failed to reach leader for table change: {} (is_connect={}, is_timeout={}, is_request={})",
                super::peer_outbox::reqwest_error_chain(&e),
                e.is_connect(),
                e.is_timeout(),
                e.is_request(),
            );
            false
        }
    }
}

#[allow(clippy::too_many_arguments)]
async fn handle_table_change(
    client: &reqwest::Client,
    peer_manager: &Arc<PeerManager>,
    is_leader: &Arc<AtomicBool>,
    snapshot_needed: &Arc<AtomicBool>,
    table_change_queue: &Arc<tokio::sync::Mutex<Vec<TableChangeQueueEntry>>>,
    peer_outboxes: &Arc<tokio::sync::Mutex<PeerOutboxes>>,
    change_json: &str,
) {
    if is_leader.load(Ordering::Relaxed) {
        peer_outboxes.lock().await.broadcast(OutboundMsg::TableChange {
            change_json: change_json.to_string(),
        });
        snapshot_needed.store(true, Ordering::Relaxed);
    } else {
        log::info!("Sync: Follower posting table change to leader");
        let delivered = try_post_table_change_to_leader(client, peer_manager, change_json).await;
        if delivered {
            log::info!("Sync: Table change sent to leader OK");
        } else {
            let mut queue = table_change_queue.lock().await;
            queue.push(TableChangeQueueEntry {
                change_json: change_json.to_string(),
                queued_time: crate::utils::current_time_ms(),
                retry_count: 0,
            });
            log::info!(
                "Sync: Queued table change for retry ({} pending)",
                queue.len()
            );
        }
    }
}

async fn flush_table_change_queue(
    client: &reqwest::Client,
    queue: &Arc<tokio::sync::Mutex<Vec<TableChangeQueueEntry>>>,
    peer_manager: &Arc<PeerManager>,
    is_leader: &Arc<AtomicBool>,
) {
    if is_leader.load(Ordering::Relaxed) {
        return;
    }
    let to_retry: Vec<TableChangeQueueEntry> = {
        let mut q = queue.lock().await;
        if q.is_empty() {
            return;
        }
        std::mem::take(&mut *q)
    };
    let initial = to_retry.len();
    let mut still_pending = Vec::new();
    for mut entry in to_retry {
        let delivered =
            try_post_table_change_to_leader(client, peer_manager, &entry.change_json).await;
        if delivered {
            continue;
        }
        entry.retry_count += 1;
        if entry.retry_count < MAX_EDIT_RETRY_COUNT {
            still_pending.push(entry);
        } else {
            log::warn!(
                "Sync: Dropping table change after {} retries: {}",
                entry.retry_count,
                entry.change_json
            );
        }
    }
    let flushed = initial - still_pending.len();
    if flushed > 0 {
        log::info!("Sync: Flushed {} table change(s) to leader", flushed);
    }
    let mut q = queue.lock().await;
    let new_arrivals = std::mem::take(&mut *q);
    still_pending.extend(new_arrivals);
    *q = still_pending;
}
