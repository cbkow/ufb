//! Mesh HTTP server (control + data planes).
//!
//! Replaces `tauri::AppHandle::emit(...)` calls with `MeshEvents` trait
//! method calls. Path-translation (canonical → native) still happens
//! here so the trait receives already-native payloads — that translation
//! depends on `AppSettings::load().path_mappings`, which the binding
//! layer doesn't need to redo.

use super::commands::{apply_table_change, SyncCommand};
use super::peer_manager::PeerManager;
use crate::columns::ColumnConfigManager;
use crate::db::Database;
use crate::events::MeshEventsArc;
use crate::settings::AppSettings;
use crate::utils::from_canonical_path;
use axum::extract::{Path as AxumPath, Query as AxumQuery, State as AxumState};
use axum::http::StatusCode;
use axum::routing::{get, post};
use axum::{Json, Router};
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use tokio::sync::{mpsc, oneshot};

/// Shared state for the HTTP server.
///
/// Both the control listener (`/api/status`) and the data listener
/// (`/api/metadata/*`, `/api/table/*`, `/api/snapshot/notify`) share
/// this state. `db` is still present for handlers that dispatch DB
/// work via `spawn_blocking`; rusqlite is synchronous and must never
/// run on an axum handler future's tokio worker thread.
pub struct HttpState {
    pub db: Arc<Database>,
    pub node_id: String,
    pub tags: Vec<String>,
    pub peer_manager: Arc<PeerManager>,
    pub is_leader: Arc<AtomicBool>,
    pub last_snapshot_time: Arc<AtomicU64>,
    pub enabled: Arc<AtomicBool>,
    pub command_tx: Arc<tokio::sync::Mutex<mpsc::UnboundedSender<SyncCommand>>>,
    pub column_config_manager: Arc<ColumnConfigManager>,
    /// Trait object that forwards events to the binding layer
    /// (cxx-qt → Qt signals on the `Mesh` QObject). May be a `NoopMeshEvents`
    /// in tests.
    pub events: MeshEventsArc,
}

pub struct MeshHttpServer {
    shutdown_tx: Option<oneshot::Sender<()>>,
    task_handle: Option<tokio::task::JoinHandle<()>>,
}

impl MeshHttpServer {
    /// Start the CONTROL-plane listener.
    pub async fn start_control(
        port: u16,
        state: Arc<HttpState>,
    ) -> Result<Self, std::io::Error> {
        let app = Router::new()
            .route("/api/status", get(handle_status))
            .with_state(state);
        Self::start_router(port, app, "control").await
    }

    /// Start the DATA-plane listener.
    pub async fn start_data(
        port: u16,
        state: Arc<HttpState>,
    ) -> Result<Self, std::io::Error> {
        let app = Router::new()
            .route("/api/metadata/update", post(handle_metadata_update))
            .route("/api/metadata/{job_path}", get(handle_metadata_get))
            .route("/api/metadata/batch", post(handle_metadata_batch))
            .route("/api/table/update", post(handle_table_update))
            .route("/api/snapshot/notify", post(handle_snapshot_notify))
            .with_state(state);
        Self::start_router(port, app, "data").await
    }

    /// Common bind + serve. Binds synchronously so a collision surfaces
    /// as `Err` at `set_enabled` time.
    async fn start_router(
        port: u16,
        app: Router,
        label: &'static str,
    ) -> Result<Self, std::io::Error> {
        let addr = std::net::SocketAddr::from(([0, 0, 0, 0], port));
        // SO_REUSEADDR: on Windows, when the previous app process exits,
        // its 49201/49202 sockets sit in TIME_WAIT for up to 4 min.
        // Without this flag a relaunch within that window fails with
        // WSAEADDRINUSE.
        let socket = tokio::net::TcpSocket::new_v4()?;
        socket.set_reuseaddr(true)?;
        socket.bind(addr)?;
        let listener = socket.listen(1024)?;
        log::info!(
            "Mesh HTTP {} server listening on port {}",
            label,
            port
        );

        let (shutdown_tx, shutdown_rx) = oneshot::channel::<()>();
        let task_handle = tokio::spawn(async move {
            axum::serve(listener, app)
                .with_graceful_shutdown(async {
                    let _ = shutdown_rx.await;
                })
                .await
                .unwrap_or_else(|e| log::error!("HTTP {} server error: {}", label, e));
            log::info!("Mesh HTTP {} server stopped", label);
        });

        Ok(Self {
            shutdown_tx: Some(shutdown_tx),
            task_handle: Some(task_handle),
        })
    }

    /// Signal shutdown and wait for the server to stop.
    pub async fn stop(&mut self) {
        if let Some(tx) = self.shutdown_tx.take() {
            let _ = tx.send(());
        }
        if let Some(handle) = self.task_handle.take() {
            let _ = handle.await;
        }
    }
}

// ── Helper: run a synchronous DB closure off the async runtime ──
async fn db_blocking<F, T>(db: Arc<Database>, f: F) -> Result<T, StatusCode>
where
    F: FnOnce(&rusqlite::Connection) -> rusqlite::Result<T> + Send + 'static,
    T: Send + 'static,
{
    tokio::task::spawn_blocking(move || db.with_conn(f))
        .await
        .map_err(|e| {
            log::error!("DB task join error: {}", e);
            StatusCode::INTERNAL_SERVER_ERROR
        })?
        .map_err(|e| {
            log::error!("DB error: {}", e);
            StatusCode::INTERNAL_SERVER_ERROR
        })
}

// ── Route handlers ──

#[derive(Serialize)]
struct StatusResponse {
    node_id: String,
    is_leader: bool,
    peer_count: usize,
    last_snapshot_time: u64,
    enabled: bool,
    tags: Vec<String>,
    /// This node's mesh epoch — peers drop us if it doesn't match
    /// theirs. `/api/status` itself is never epoch-gated; it's the
    /// channel epoch is discovered through.
    mesh_epoch: u32,
}

async fn handle_status(AxumState(state): AxumState<Arc<HttpState>>) -> Json<StatusResponse> {
    Json(StatusResponse {
        node_id: state.node_id.clone(),
        is_leader: state.is_leader.load(Ordering::Relaxed),
        peer_count: state.peer_manager.get_peer_count(),
        last_snapshot_time: state.last_snapshot_time.load(Ordering::Relaxed),
        enabled: state.enabled.load(Ordering::Relaxed),
        tags: state.tags.clone(),
        mesh_epoch: super::MESH_EPOCH,
    })
}

/// Query string for the epoch-gated GET endpoints. A caller on a
/// different epoch (or an old build that sends nothing → 0) is rejected
/// with `409 CONFLICT` so it can never pull tagged-identity data.
#[derive(Deserialize)]
struct EpochQuery {
    #[serde(default)]
    mesh_epoch: u32,
}

#[derive(Deserialize)]
struct MetadataUpdateRequest {
    job_path: String,
    item_path: String,
    metadata: String,
    folder_name: String,
    #[serde(default)]
    is_tracked: bool,
    /// Sender's edit timestamp (ms), coupled to `metadata`. Used for the
    /// LWW gate so a replayed/older edit can't clobber a newer local
    /// value. Absent (0) from a peer that predates this field → we fall
    /// back to receive-time `now()` and apply (no regression).
    #[serde(default)]
    modified_time: i64,
    #[serde(default)]
    mesh_epoch: u32,
}

async fn handle_metadata_update(
    AxumState(state): AxumState<Arc<HttpState>>,
    Json(body): Json<MetadataUpdateRequest>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    if body.mesh_epoch != super::MESH_EPOCH {
        log::warn!(
            "HTTP: rejecting metadata update — peer mesh epoch {} != {}",
            body.mesh_epoch,
            super::MESH_EPOCH
        );
        return Err(StatusCode::CONFLICT);
    }
    log::info!(
        "HTTP: Received metadata update for {} (leader={})",
        body.item_path,
        state.is_leader.load(Ordering::Relaxed)
    );

    let mappings = AppSettings::load().path_mappings;
    let job_path = crate::utils::to_identity_storage(&body.job_path, &mappings);
    let item_path = crate::utils::to_identity_storage(&body.item_path, &mappings);
    // Resolve to native here, before `job_path`/`item_path` are moved
    // into the DB closure below — the emit at the end needs them.
    let native_job_path = from_canonical_path(&job_path, &mappings);
    let native_item_path = from_canonical_path(&item_path, &mappings);
    let folder_name = body.folder_name.clone();
    let metadata = body.metadata.clone();
    let is_tracked = body.is_tracked;
    let now = crate::utils::current_time_ms();
    // LWW gate: stamp the row with the SENDER's edit time (so the
    // timestamp is consistent mesh-wide), and on a conflict only apply
    // when the incoming edit is at-or-newer than the local one — a
    // replayed/older edit (e.g. a delayed retry) must not clobber a newer
    // value. A peer that sent no timestamp (0) falls back to receive-time
    // `now`, which is always >= local, preserving prior apply-always
    // behaviour.
    let effective_mtime = if body.modified_time > 0 { body.modified_time } else { now };
    db_blocking(state.db.clone(), move |conn| {
        conn.execute(
            "INSERT INTO item_metadata (item_path, job_path, folder_name, metadata_json, is_tracked, modified_time, deleted_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, NULL)
             ON CONFLICT(item_path) DO UPDATE SET
                 metadata_json = excluded.metadata_json,
                 is_tracked = excluded.is_tracked,
                 modified_time = excluded.modified_time,
                 deleted_at = NULL
             WHERE excluded.modified_time >= item_metadata.modified_time",
            rusqlite::params![
                item_path,
                job_path,
                folder_name,
                metadata,
                is_tracked as i64,
                effective_mtime,
            ],
        )?;
        Ok(())
    })
    .await?;

    // Notify the binding layer (was Tauri emit) with the native paths
    // resolved above (before the DB closure moved the identities).
    state
        .events
        .metadata_changed(&native_job_path, &native_item_path, &body.folder_name);

    // If leader, broadcast to other peers
    if state.is_leader.load(Ordering::Relaxed) {
        let _ = state
            .command_tx
            .lock()
            .await
            .send(SyncCommand::BroadcastEdit {
                job_path: body.job_path,
                item_path: body.item_path,
                metadata_json: body.metadata,
                folder_name: body.folder_name,
                is_tracked: body.is_tracked,
                // Re-broadcast with the SAME edit timestamp we applied, so
                // downstream peers gate against the original edit time.
                modified_time: effective_mtime,
            });
    }

    Ok(Json(serde_json::json!({"status": "ok"})))
}

async fn handle_metadata_get(
    AxumState(state): AxumState<Arc<HttpState>>,
    AxumPath(job_path): AxumPath<String>,
    AxumQuery(epoch): AxumQuery<EpochQuery>,
) -> Result<Json<Vec<serde_json::Value>>, StatusCode> {
    if epoch.mesh_epoch != super::MESH_EPOCH {
        log::warn!(
            "HTTP: rejecting metadata GET — peer mesh epoch {} != {}",
            epoch.mesh_epoch,
            super::MESH_EPOCH
        );
        return Err(StatusCode::CONFLICT);
    }
    let decoded = urlencoding::decode(&job_path)
        .map(|s| s.into_owned())
        .unwrap_or(job_path);
    let decoded = crate::utils::to_identity_storage(
        &decoded,
        &AppSettings::load().path_mappings,
    );

    let records = db_blocking(state.db.clone(), move |conn| {
        let mut stmt = conn.prepare(
            "SELECT item_path, job_path, folder_name, metadata_json, is_tracked, modified_time
             FROM item_metadata WHERE job_path = ?1 AND deleted_at IS NULL",
        )?;
        let rows = stmt.query_map([decoded.as_str()], |row| {
            Ok(serde_json::json!({
                "item_path": row.get::<_, String>(0)?,
                "job_path": row.get::<_, String>(1)?,
                "folder_name": row.get::<_, String>(2)?,
                "metadata": row.get::<_, String>(3)?,
                "is_tracked": row.get::<_, i64>(4)? != 0,
                "modified_time": row.get::<_, Option<i64>>(5)?,
            }))
        })?;
        let mut records = Vec::new();
        for row in rows {
            if let Ok(val) = row {
                records.push(val);
            }
        }
        Ok(records)
    })
    .await?;

    Ok(Json(records))
}

#[derive(Deserialize)]
struct BatchRequest {
    job_path: String,
    #[serde(default)]
    _since: Option<i64>,
    #[serde(default)]
    mesh_epoch: u32,
}

async fn handle_metadata_batch(
    AxumState(state): AxumState<Arc<HttpState>>,
    Json(body): Json<BatchRequest>,
) -> Result<Json<Vec<serde_json::Value>>, StatusCode> {
    if body.mesh_epoch != super::MESH_EPOCH {
        log::warn!(
            "HTTP: rejecting metadata batch — peer mesh epoch {} != {}",
            body.mesh_epoch,
            super::MESH_EPOCH
        );
        return Err(StatusCode::CONFLICT);
    }
    let job_path = crate::utils::to_identity_storage(
        &body.job_path,
        &AppSettings::load().path_mappings,
    );
    let records = db_blocking(state.db.clone(), move |conn| {
        let mut stmt = conn.prepare(
            "SELECT item_path, job_path, folder_name, metadata_json, is_tracked, modified_time
             FROM item_metadata WHERE job_path = ?1 AND deleted_at IS NULL",
        )?;
        let rows = stmt.query_map([&job_path], |row| {
            Ok(serde_json::json!({
                "item_path": row.get::<_, String>(0)?,
                "job_path": row.get::<_, String>(1)?,
                "folder_name": row.get::<_, String>(2)?,
                "metadata": row.get::<_, String>(3)?,
                "is_tracked": row.get::<_, i64>(4)? != 0,
                "modified_time": row.get::<_, Option<i64>>(5)?,
            }))
        })?;
        let mut records = Vec::new();
        for row in rows {
            if let Ok(val) = row {
                records.push(val);
            }
        }
        Ok(records)
    })
    .await?;

    Ok(Json(records))
}

async fn handle_table_update(
    AxumState(state): AxumState<Arc<HttpState>>,
    Json(body): Json<serde_json::Value>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let peer_epoch = body
        .get("mesh_epoch")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    if peer_epoch != super::MESH_EPOCH as u64 {
        log::warn!(
            "HTTP: rejecting table update — peer mesh epoch {} != {}",
            peer_epoch,
            super::MESH_EPOCH
        );
        return Err(StatusCode::CONFLICT);
    }
    let change_json = body.to_string();
    let action = body
        .get("action")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    log::info!(
        "HTTP: Received table update action={} (leader={})",
        action,
        state.is_leader.load(Ordering::Relaxed)
    );

    // Apply to local DB off-thread.
    let db = state.db.clone();
    let change_json_for_apply = change_json.clone();
    let apply_result =
        tokio::task::spawn_blocking(move || apply_table_change(&db, &change_json_for_apply)).await;
    match apply_result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => {
            log::error!("Failed to apply table change from peer: {}", e);
            return Err(StatusCode::INTERNAL_SERVER_ERROR);
        }
        Err(e) => {
            log::error!("apply_table_change join error: {}", e);
            return Err(StatusCode::INTERNAL_SERVER_ERROR);
        }
    }

    // Invalidate column caches if column operation (in-memory, non-blocking).
    if action.starts_with("col_") {
        state.column_config_manager.invalidate_all_caches();
    }

    // If leader, broadcast to other peers
    if state.is_leader.load(Ordering::Relaxed) {
        let _ = state
            .command_tx
            .lock()
            .await
            .send(SyncCommand::BroadcastTableChange {
                change_json,
            });
    }

    // Notify the binding layer (was Tauri emit). Translate any job_path
    // in the payload from canonical to native OS format.
    let mut emit_body = body.clone();
    if let Some(jp) = emit_body
        .get("job_path")
        .and_then(|v| v.as_str())
        .map(|s| s.to_string())
    {
        let mappings = AppSettings::load().path_mappings;
        emit_body["job_path"] =
            serde_json::Value::String(from_canonical_path(&jp, &mappings));
    }
    state.events.table_changed(emit_body);

    Ok(Json(serde_json::json!({"status": "ok"})))
}

async fn handle_snapshot_notify(
    AxumState(state): AxumState<Arc<HttpState>>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    if !state.is_leader.load(Ordering::Relaxed) {
        log::info!("HTTP: Received snapshot notify, queuing restore");
        let _ = state
            .command_tx
            .lock()
            .await
            .send(SyncCommand::RestoreSnapshot);
    } else {
        log::info!("HTTP: Ignoring snapshot notify (I am leader)");
    }

    Ok(Json(serde_json::json!({"status": "ok"})))
}
