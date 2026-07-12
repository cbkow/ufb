//! `Mesh` QObject — wraps `core::mesh::coordinator::MeshSyncManager`.
//!
//! Two responsibilities:
//!   1. **Outbound surface**: expose status / peers / enable+reinit /
//!      manual snapshot to QML.
//!   2. **Inbound surface**: receive `MeshEvents` callbacks from the
//!      core layer (fired by tokio tasks + the HTTP server) and bump
//!      Qt-thread-side rev counters that QML observers re-fetch on.
//!
//! ### Outbound surface
//!   * `status_json` qproperty — serialised MeshSyncStatus
//!   * `peers_json`  qproperty — serialised PeerInfo list
//!   * `start()`            — idempotent app-startup hook
//!   * `refresh_status()`   — re-fetch from manager + update properties
//!   * `set_enabled(bool)`  — persist + start/stop background tasks
//!   * `trigger_snapshot()` — leader-only manual snapshot
//!   * `reinit()`           — rebuild after farm_path / node_id change
//!
//! ### Inbound surface
//!   * `data_refresh_rev` qproperty — bumps after a snapshot restore
//!     applies remote rows. QML re-fetches subscriptions / columns / etc.
//!   * `metadata_change_rev` + `last_metadata_change_json` — bumps when
//!     a peer pushes a single-item metadata edit. Payload contains the
//!     `{jobPath, itemPath, folderName}` of the affected row so observers
//!     can do targeted refreshes instead of full reloads.
//!   * `table_change_rev` + `last_table_change_json` — bumps when a peer
//!     pushes a sub_add / sub_remove / col_* / preset_* table change.
//!
//! ### Module-level helpers (used by sibling QObjects)
//! `current_mgr`, `broadcast_table_change`, `broadcast_metadata_edit`
//! are `pub(crate)` so `subscription.rs` and `columns.rs` can plumb
//! local writes into the mesh outbox without re-implementing the
//! singleton lookup.

use crate::db::shared_db;
use crate::runtime::shared_runtime;
use std::pin::Pin;
use std::sync::{Arc, Mutex, OnceLock};
use ufb_core::columns::ColumnConfigManager;
use ufb_core::events::{MeshEvents, MeshEventsArc};
use ufb_core::mesh::coordinator::MeshSyncManager;
use ufb_core::settings::AppSettings;

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
        #[qproperty(QString, status_json)]
        #[qproperty(QString, peers_json)]
        // Bumps after the core layer fires `MeshEvents::data_refreshed`
        // (snapshot restore landed on the local DB). Coarse: QML
        // observers should treat this as "everything mesh-managed may
        // be stale, re-fetch open views".
        #[qproperty(i32, data_refresh_rev)]
        // Bumps after a peer pushes `/api/metadata/update` for a
        // single item. The accompanying `last_metadata_change_json`
        // carries `{jobPath, itemPath, folderName}` so observers can
        // re-fetch just the affected row.
        #[qproperty(i32, metadata_change_rev)]
        #[qproperty(QString, last_metadata_change_json)]
        // Bumps after a peer pushes `/api/table/update` (sub_add,
        // sub_remove, col_add, col_update, col_delete, preset_save,
        // preset_delete). `last_table_change_json` holds the raw
        // payload so QML can dispatch on the `action` field.
        #[qproperty(i32, table_change_rev)]
        #[qproperty(QString, last_table_change_json)]
        // Bumps when the core layer fires `MeshEvents::sync_warning` —
        // a degraded sync the user should see (edit dropped after retries,
        // outbox overflow, template fetch failure). `last_warning_json`
        // holds `{kind, detail, ts}`. MeshSettingsDialog surfaces it so
        // these failures aren't log-only. See the audit's "silent failure
        // everywhere" finding.
        #[qproperty(i32, warning_rev)]
        #[qproperty(QString, last_warning_json)]
        type Mesh = super::MeshRust;

        /// Idempotent app-startup hook. Builds the manager from
        /// settings and auto-enables it when settings.meshSync.enabled
        /// is true so the user doesn't have to open the dialog after
        /// every restart. Safe to call repeatedly from QML's
        /// Component.onCompleted.
        #[qinvokable]
        fn start(self: Pin<&mut Mesh>);

        /// Re-fetch MeshSyncStatus + peer list and update properties.
        #[qinvokable]
        fn refresh_status(self: Pin<&mut Mesh>);

        /// Toggle mesh on/off. Persists to settings.json and starts /
        /// stops the background tasks. Returns "" on success or an
        /// error message ("not configured", etc).
        #[qinvokable]
        fn set_enabled(self: Pin<&mut Mesh>, enabled: bool) -> QString;

        /// Manual snapshot (leader-only no-op for followers).
        #[qinvokable]
        fn trigger_snapshot(self: Pin<&mut Mesh>);

        /// Re-read farm_path / node_id from settings and rebuild the
        /// MeshSyncManager. Called after the user saves the Mesh
        /// Settings dialog.
        #[qinvokable]
        fn reinit(self: Pin<&mut Mesh>);
    }

    // Threading lets the core mesh tasks queue closures back onto the
    // Qt thread that mutate this QObject. Required by the events
    // forwarder pattern below.
    impl cxx_qt::Threading for Mesh {}
}

pub struct MeshRust {
    pub status_json: cxx_qt_lib::QString,
    pub peers_json: cxx_qt_lib::QString,
    pub data_refresh_rev: i32,
    pub metadata_change_rev: i32,
    pub last_metadata_change_json: cxx_qt_lib::QString,
    pub table_change_rev: i32,
    pub last_table_change_json: cxx_qt_lib::QString,
    pub warning_rev: i32,
    pub last_warning_json: cxx_qt_lib::QString,
}

impl Default for MeshRust {
    fn default() -> Self {
        Self {
            status_json: cxx_qt_lib::QString::from("{}"),
            peers_json: cxx_qt_lib::QString::from("[]"),
            data_refresh_rev: 0,
            metadata_change_rev: 0,
            last_metadata_change_json: cxx_qt_lib::QString::from("{}"),
            table_change_rev: 0,
            last_table_change_json: cxx_qt_lib::QString::from("{}"),
            warning_rev: 0,
            last_warning_json: cxx_qt_lib::QString::from("{}"),
        }
    }
}

/// Lazily-built MeshSyncManager. Held in a Mutex<Option<Arc<...>>>
/// so reinit() can swap in a fresh instance when the user changes
/// farm_path or node_id from the Settings dialog.
fn shared_mgr_slot() -> &'static Mutex<Option<Arc<MeshSyncManager>>> {
    static SLOT: OnceLock<Mutex<Option<Arc<MeshSyncManager>>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

/// MeshEventsArc captured from the QObject's qt_thread() during start().
/// Set exactly once, lives for the app's lifetime. Any call to
/// `ensure_manager()` before this is set will fail (returning None) —
/// QML's `Mesh.start()` runs from `Component.onCompleted` so by the time
/// any other binding looks for the manager this slot is populated.
fn shared_events_slot() -> &'static OnceLock<MeshEventsArc> {
    static EVENTS: OnceLock<MeshEventsArc> = OnceLock::new();
    &EVENTS
}

/// Build a manager from current settings. Returns None when farm_path
/// / node_id are empty (mesh not configured — UI shows "Not
/// configured" and the gear opens the dialog).
fn build_manager(events: MeshEventsArc) -> Option<Arc<MeshSyncManager>> {
    let s = AppSettings::load();
    let farm = s.mesh_sync.farm_path.clone();
    let node = s.mesh_sync.node_id.clone();
    if farm.is_empty() || node.is_empty() {
        return None;
    }
    let db = shared_db()?;
    let columns = Arc::new(ColumnConfigManager::new(db.clone()));
    let tags: Vec<String> = s
        .mesh_sync
        .tags
        .split(',')
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .collect();
    Some(Arc::new(MeshSyncManager::new(
        farm,
        node,
        s.mesh_sync.http_port,
        s.mesh_sync.data_port,
        tags,
        db,
        columns,
        events,
    )))
}

fn ensure_manager() -> Option<Arc<MeshSyncManager>> {
    let events = shared_events_slot().get()?.clone();
    let mut slot = shared_mgr_slot().lock().unwrap();
    if let Some(m) = slot.as_ref() {
        return Some(Arc::clone(m));
    }
    let mgr = build_manager(events)?;
    *slot = Some(Arc::clone(&mgr));
    Some(mgr)
}

fn current_manager() -> Option<Arc<MeshSyncManager>> {
    shared_mgr_slot().lock().unwrap().as_ref().cloned()
}

fn placeholder_status_json() -> String {
    let s = AppSettings::load();
    let configured =
        !s.mesh_sync.farm_path.is_empty() && !s.mesh_sync.node_id.is_empty();
    serde_json::json!({
        "isLeader": false,
        "leaderId": "",
        "peerCount": 0,
        "lastSnapshotTime": serde_json::Value::Null,
        "pendingEditsCount": 0,
        "statusMessage": if configured { "Disabled" } else { "Not configured" },
        "isEnabled": false,
        "isConfigured": configured,
        "nodeId": s.mesh_sync.node_id,
        "farmPath": s.mesh_sync.farm_path,
    })
    .to_string()
}

/// Guards against double-start from QML. Mesh.start() may be called
/// from Component.onCompleted on every QML root reload during dev.
static STARTED: OnceLock<()> = OnceLock::new();

impl qobject::Mesh {
    fn start(mut self: Pin<&mut qobject::Mesh>) {
        use cxx_qt::Threading;

        if STARTED.set(()).is_err() {
            // Already started — just refresh state.
            self.as_mut().refresh_status();
            return;
        }

        // Capture a Qt-thread handle and stash a forwarder so any
        // mesh-side event (snapshot restored, peer push) can bump our
        // rev-counter qproperties from a tokio task.
        let qt_handle: cxx_qt::CxxQtThread<qobject::Mesh> = self.as_ref().qt_thread();
        let forwarder: MeshEventsArc = Arc::new(MeshEventsForwarder { qt_handle });
        let _ = shared_events_slot().set(forwarder);

        let s = AppSettings::load();
        // Build the manager from current settings (no-op when farm/
        // node aren't configured — placeholder_status_json publishes
        // "Not configured" via refresh_status below).
        let _ = ensure_manager();
        if s.mesh_sync.enabled {
            if let Some(mgr) = current_manager() {
                let _guard = shared_runtime().enter();
                shared_runtime().block_on(mgr.set_enabled(true));
                log::info!("mesh: auto-enabled at app start");
            } else {
                log::info!(
                    "mesh: settings.meshSync.enabled=true but mesh isn't configured (farm/node empty) — skipping auto-start"
                );
            }
        }
        self.as_mut().refresh_status();
    }

    fn refresh_status(mut self: Pin<&mut qobject::Mesh>) {
        let Some(mgr) = ensure_manager() else {
            self.as_mut()
                .set_status_json(cxx_qt_lib::QString::from(&placeholder_status_json()));
            self.as_mut()
                .set_peers_json(cxx_qt_lib::QString::from("[]"));
            return;
        };
        // get_status is non-async — short DB-free walk over the
        // peer manager + atomics.
        let mut status = serde_json::to_value(mgr.get_status())
            .unwrap_or_else(|_| serde_json::json!({}));
        // Decorate with nodeId / farmPath so QML doesn't need a
        // second roundtrip.
        if let Some(obj) = status.as_object_mut() {
            obj.insert("nodeId".into(), serde_json::json!(mgr.node_id()));
            obj.insert("farmPath".into(), serde_json::json!(mgr.farm_path()));
        }
        self.as_mut()
            .set_status_json(cxx_qt_lib::QString::from(&status.to_string()));

        let peers = mgr.peer_manager().get_peers();
        let peers_json = serde_json::to_string(&peers).unwrap_or_else(|_| "[]".into());
        self.as_mut()
            .set_peers_json(cxx_qt_lib::QString::from(&peers_json));
    }

    fn set_enabled(
        mut self: Pin<&mut qobject::Mesh>,
        enabled: bool,
    ) -> cxx_qt_lib::QString {
        // Mirror to settings so the toggle persists across restarts.
        {
            let mut s = AppSettings::load();
            s.mesh_sync.enabled = enabled;
            if let Err(e) = s.save() {
                log::warn!("mesh: settings save failed: {}", e);
            }
        }
        let Some(mgr) = ensure_manager() else {
            return cxx_qt_lib::QString::from(
                "Mesh sync isn't configured — set a farm path and node ID first.",
            );
        };
        let _guard = shared_runtime().enter();
        // set_enabled is async because it spawns the background tasks.
        // block_on is fine — UI thread waits while subprocess wires up.
        shared_runtime().block_on(mgr.set_enabled(enabled));
        log::info!("mesh: set_enabled({})", enabled);
        self.as_mut().refresh_status();
        cxx_qt_lib::QString::from("")
    }

    fn trigger_snapshot(mut self: Pin<&mut qobject::Mesh>) {
        let Some(mgr) = current_manager() else { return };
        let _guard = shared_runtime().enter();
        shared_runtime().block_on(mgr.trigger_snapshot());
        self.as_mut().refresh_status();
    }

    fn reinit(mut self: Pin<&mut qobject::Mesh>) {
        // Rebuild the manager from current settings. If the previous
        // instance was running, shut it down first to free the ports.
        if let Some(prev) = current_manager() {
            let _guard = shared_runtime().enter();
            shared_runtime().block_on(prev.shutdown());
        }
        *shared_mgr_slot().lock().unwrap() = None;
        let _ = ensure_manager();
        // If the user wants mesh enabled, kick the new manager.
        let s = AppSettings::load();
        if s.mesh_sync.enabled {
            if let Some(mgr) = current_manager() {
                let _guard = shared_runtime().enter();
                shared_runtime().block_on(mgr.set_enabled(true));
            }
        }
        self.as_mut().refresh_status();
    }
}

// ─────────────────────────────────────────────────────────────────────
// MeshEventsForwarder — turns trait calls (on tokio threads / from
// the axum HTTP server) into queued qproperty mutations on the Qt
// thread. Mirrors the Mount/Transcode/FileOps forwarder pattern.
// ─────────────────────────────────────────────────────────────────────

struct MeshEventsForwarder {
    qt_handle: cxx_qt::CxxQtThread<qobject::Mesh>,
}

impl MeshEvents for MeshEventsForwarder {
    fn data_refreshed(&self) {
        let _ = self.qt_handle.queue(move |mut mesh| {
            let cur = *mesh.as_ref().data_refresh_rev();
            mesh.as_mut().set_data_refresh_rev(cur.wrapping_add(1));
        });
    }

    fn metadata_changed(&self, job_path: &str, item_path: &str, folder_name: &str) {
        let payload = serde_json::json!({
            "jobPath": job_path,
            "itemPath": item_path,
            "folderName": folder_name,
        })
        .to_string();
        let _ = self.qt_handle.queue(move |mut mesh| {
            mesh.as_mut()
                .set_last_metadata_change_json(cxx_qt_lib::QString::from(&payload));
            let cur = *mesh.as_ref().metadata_change_rev();
            mesh.as_mut().set_metadata_change_rev(cur.wrapping_add(1));
        });
    }

    fn table_changed(&self, payload: serde_json::Value) {
        let payload_s = payload.to_string();
        let _ = self.qt_handle.queue(move |mut mesh| {
            mesh.as_mut()
                .set_last_table_change_json(cxx_qt_lib::QString::from(&payload_s));
            let cur = *mesh.as_ref().table_change_rev();
            mesh.as_mut().set_table_change_rev(cur.wrapping_add(1));
        });
    }

    fn sync_warning(&self, kind: &str, detail: &str) {
        // Always log (existing behaviour) AND surface to the UI.
        log::warn!("mesh sync_warning [{}]: {}", kind, detail);
        let payload = serde_json::json!({
            "kind": kind,
            "detail": detail,
            "ts": ufb_core::utils::current_time_ms(),
        })
        .to_string();
        let _ = self.qt_handle.queue(move |mut mesh| {
            mesh.as_mut()
                .set_last_warning_json(cxx_qt_lib::QString::from(&payload));
            let cur = *mesh.as_ref().warning_rev();
            mesh.as_mut().set_warning_rev(cur.wrapping_add(1));
        });
    }
}

// ─────────────────────────────────────────────────────────────────────
// Crate-visible helpers used by sibling QObjects (subscription, columns)
// to broadcast local writes into the mesh outbox.
//
// Both helpers fire-and-forget onto the shared tokio runtime — the QML
// invokable that triggered the local DB write returns immediately;
// mesh delivery (HTTP POST to leader / broadcast to peers) happens
// async. Failures only log; persistence already happened in the local
// DB before these were called.
//
// `MeshSyncManager::on_*` already enabled-check internally, so passing
// edits while disabled is harmless — we just avoid the spawn.
// ─────────────────────────────────────────────────────────────────────

/// Normalise the path(s) in a table-change payload to tagged-identity
/// form for the wire, and return `None` (skip the broadcast) when the
/// path is a per-machine `native:` location — only volume-backed
/// identities are shared on the mesh. `sub_*` / `col_delete` hold a
/// top-level `job_path`; `col_*` hold it inside the serialized `def`
/// (camelCase `jobPath`).
fn tagify_table_change(change_json: &str) -> Option<String> {
    let mut change: serde_json::Value = serde_json::from_str(change_json).ok()?;
    let mappings = AppSettings::load().path_mappings;
    let to_shared = |p: &str| -> Option<String> {
        let t = ufb_core::utils::to_identity_storage(p, &mappings);
        if t.starts_with("native:") {
            None
        } else {
            Some(t)
        }
    };
    let action = change
        .get("action")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string();
    if action == "col_add" || action == "col_update" {
        let def_str = change.get("def").and_then(|v| v.as_str())?.to_string();
        let mut def: serde_json::Value = serde_json::from_str(&def_str).ok()?;
        if let Some(jp) = def.get("jobPath").and_then(|v| v.as_str()) {
            def["jobPath"] = serde_json::Value::String(to_shared(jp)?);
        }
        change["def"] = serde_json::Value::String(def.to_string());
    } else if let Some(jp) = change.get("job_path").and_then(|v| v.as_str()) {
        change["job_path"] = serde_json::Value::String(to_shared(jp)?);
    }
    Some(change.to_string())
}

/// Push a `TableChange` (sub_add / sub_remove / col_*) to the mesh
/// manager's outbox. Paths are normalised to tagged-identity form for
/// the wire; a change for a per-machine (`native:`) path is dropped —
/// only volume-backed identities are shared. JSON shape must match what
/// `core::mesh::commands::apply_table_change` expects.
pub(crate) fn broadcast_table_change(change_json: String) {
    let Some(mgr) = current_manager() else { return };
    if !mgr.is_enabled() {
        return;
    }
    let Some(change_json) = tagify_table_change(&change_json) else {
        log::debug!("mesh: skipping broadcast of a non-volume table change");
        return;
    };
    shared_runtime().spawn(async move {
        mgr.on_table_changed(&change_json).await;
    });
}

/// Push a per-item metadata edit to the mesh manager's outbox.
/// `metadata_json` should be the post-merge full row JSON (i.e. the
/// blob that was just persisted to the local DB).
pub(crate) fn broadcast_metadata_edit(
    job_path: String,
    item_path: String,
    metadata_json: String,
    folder_name: String,
    is_tracked: bool,
    modified_time: i64,
) {
    let Some(mgr) = current_manager() else { return };
    if !mgr.is_enabled() {
        return;
    }
    // Normalise to tagged identity for the wire; skip the broadcast
    // when the item is a per-machine (`native:`) path — only
    // volume-backed identities are shared.
    let mappings = AppSettings::load().path_mappings;
    let job_path = ufb_core::utils::to_identity_storage(&job_path, &mappings);
    let item_path = ufb_core::utils::to_identity_storage(&item_path, &mappings);
    if job_path.starts_with("native:") || item_path.starts_with("native:") {
        log::debug!("mesh: skipping metadata broadcast for a non-volume path");
        return;
    }
    shared_runtime().spawn(async move {
        mgr.on_metadata_edited(
            &job_path, &item_path, &metadata_json, &folder_name, is_tracked, modified_time,
        )
        .await;
    });
}
