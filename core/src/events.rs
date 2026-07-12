//! Event traits — how Rust core notifies the binding layer (and through it,
//! QML) of state changes.
//!
//! In the Tauri build these were `app_handle.emit("event:name", payload)`
//! calls scattered throughout the code. The binding layer (cxx-qt) implements
//! these traits and forwards each method call to the appropriate Qt signal on
//! the corresponding QObject. Tests can implement them as no-ops or recorders.
//!
//! All traits are `Send + Sync + 'static` so they can be cloned into tokio
//! tasks and held across await points.

use serde_json::Value as JsonValue;
use std::sync::Arc;

// ----------------------------------------------------------------------------
// MeshEvents — emitted by core/src/mesh/*
// ----------------------------------------------------------------------------

pub trait MeshEvents: Send + Sync + 'static {
    /// Remote metadata applied locally. UI re-fetches affected views.
    fn data_refreshed(&self);

    /// Specific item metadata updated via HTTP push from a peer.
    /// Payload is in native (already-translated-from-canonical) form.
    fn metadata_changed(&self, job_path: &str, item_path: &str, folder_name: &str);

    /// Table-level change (subscription add/remove, column add/remove, etc.)
    /// applied via HTTP push from a peer. The JSON body shape mirrors what
    /// the original Tauri event carried so QML stores can dispatch on the
    /// `action` field.
    fn table_changed(&self, payload: JsonValue);

    /// A sync operation degraded in a way the user should be able to see —
    /// e.g. an edit dropped after exhausting retries (silent data loss),
    /// an outbox overflow, or a template fetch failure. `kind` is a short
    /// machine tag (`edit_dropped`, `outbox_full`, `template_fetch`);
    /// `detail` is a human sentence. The binding layer stashes the latest
    /// into a qproperty so MeshSettingsDialog can surface it instead of
    /// these failures only living in the log. Default no-op so existing
    /// impls (tests) need no change.
    fn sync_warning(&self, _kind: &str, _detail: &str) {}
}

// ----------------------------------------------------------------------------
// Convenience: a no-op implementation handy for tests.
// ----------------------------------------------------------------------------

/// `MeshEvents` impl that drops every event. Useful as a placeholder before
/// the binding layer is wired and in unit tests.
pub struct NoopMeshEvents;

impl MeshEvents for NoopMeshEvents {
    fn data_refreshed(&self) {}
    fn metadata_changed(&self, _: &str, _: &str, _: &str) {}
    fn table_changed(&self, _: JsonValue) {}
}

/// Convenience type alias for the trait object the rest of core/ holds.
pub type MeshEventsArc = Arc<dyn MeshEvents>;

// ----------------------------------------------------------------------------
// TranscodeEvents — emitted by core/src/transcode.rs
// ----------------------------------------------------------------------------

use crate::transcode::TranscodeJob;

pub trait TranscodeEvents: Send + Sync + 'static {
    /// Job state changed (Queued → Processing → CopyingMetadata → Completed,
    /// or any → Cancelled / Failed).
    fn job_updated(&self, job: &TranscodeJob);

    /// Progress tick — fired ~once per second per active job during ffmpeg.
    fn progress(&self, job: &TranscodeJob);
}

pub struct NoopTranscodeEvents;

impl TranscodeEvents for NoopTranscodeEvents {
    fn job_updated(&self, _: &TranscodeJob) {}
    fn progress(&self, _: &TranscodeJob) {}
}

pub type TranscodeEventsArc = Arc<dyn TranscodeEvents>;

// ----------------------------------------------------------------------------
// MountEvents — emitted by core/src/mount_client.rs
// ----------------------------------------------------------------------------

use crate::mount_client::{
    AckMsg, CacheStatsMsg, ConflictDetectedMsg, ErrorMsg, MountStateSnapshotMsg,
    MountStateUpdateMsg, TestCredentialsResultMsg,
};

pub trait MountEvents: Send + Sync + 'static {
    /// Pipe/socket connection state to the agent flipped.
    fn connection_changed(&self, connected: bool);

    /// Agent reported mount state changed (Mounting → Mounted, etc.).
    fn state_update(&self, update: &MountStateUpdateMsg);

    /// Agent sent an authoritative snapshot of every known mount. UI
    /// implementations should replace their entire local mounts map on
    /// receipt — any mount missing from the snapshot has been removed
    /// and should be pruned. Sent on (a) new client connection, and
    /// (b) orchestrator removal/death events. Default impl re-emits as
    /// individual state_updates so UIs that don't care about pruning
    /// keep working.
    fn state_snapshot(&self, snapshot: &MountStateSnapshotMsg) {
        for entry in &snapshot.mounts {
            self.state_update(entry);
        }
    }

    /// Agent acknowledged a previously-issued async command.
    fn ack(&self, ack: &AckMsg);

    /// Agent reported an error executing a command.
    fn error(&self, err: &ErrorMsg);

    /// Agent detected a write conflict on a sync-mounted file.
    fn conflict_detected(&self, conflict: &ConflictDetectedMsg);

    /// Periodic / on-demand cache stats reply.
    fn cache_stats(&self, stats: &CacheStatsMsg);

    /// Reply to a TestCredentials probe. Tombstone (plans/17 slice B):
    /// the probe UI is gone — credentials are OS-owned — so this
    /// defaults to a no-op. The wire message stays parseable for one
    /// release of old-agent compatibility; slice G removes the verb.
    fn test_credentials_result(&self, _result: &TestCredentialsResultMsg) {}
}

pub struct NoopMountEvents;

impl MountEvents for NoopMountEvents {
    fn connection_changed(&self, _: bool) {}
    fn state_update(&self, _: &MountStateUpdateMsg) {}
    fn ack(&self, _: &AckMsg) {}
    fn error(&self, _: &ErrorMsg) {}
    fn conflict_detected(&self, _: &ConflictDetectedMsg) {}
    fn cache_stats(&self, _: &CacheStatsMsg) {}
}

pub type MountEventsArc = Arc<dyn MountEvents>;

// ----------------------------------------------------------------------------
// FileOpsEvents — emitted by core/src/file_ops.rs for copy/move/delete
// ----------------------------------------------------------------------------

use serde::Serialize;

/// One file operation has begun; UI can show a progress entry.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileOpStarted {
    pub id: String,
    pub operation: String, // "copy" | "move" | "delete"
    pub items_total: usize,
}

/// Periodic progress tick during a file operation.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileOpProgress {
    pub id: String,
    /// Bytes total for the *current item*. 0 for delete operations.
    pub total_bytes: u64,
    /// Bytes copied for the current item. 0 for delete operations.
    pub copied_bytes: u64,
    pub current_file: String,
    pub items_total: usize,
    pub items_done: usize,
}

/// Operation finished (some items may have failed but ≥1 succeeded).
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileOpCompleted {
    pub id: String,
    pub succeeded: usize,
    pub failed: usize,
    pub errors: Vec<String>,
    /// Canonicalized directories whose immediate listing changed as a
    /// result of this op (dest for copy; dest + source parents for move;
    /// source parents for delete). Lets the UI refresh exactly the
    /// affected views. Deduped.
    pub changed_dirs: Vec<String>,
}

/// Operation failed entirely (no items succeeded).
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileOpError {
    pub id: String,
    pub error: String,
}

pub trait FileOpsEvents: Send + Sync + 'static {
    fn op_started(&self, started: FileOpStarted);
    fn op_progress(&self, progress: FileOpProgress);
    fn op_completed(&self, completed: FileOpCompleted);
    fn op_error(&self, error: FileOpError);
}

pub struct NoopFileOpsEvents;

impl FileOpsEvents for NoopFileOpsEvents {
    fn op_started(&self, _: FileOpStarted) {}
    fn op_progress(&self, _: FileOpProgress) {}
    fn op_completed(&self, _: FileOpCompleted) {}
    fn op_error(&self, _: FileOpError) {}
}

pub type FileOpsEventsArc = Arc<dyn FileOpsEvents>;
