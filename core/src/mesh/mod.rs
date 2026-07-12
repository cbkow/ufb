//! Mesh sync subsystem.
//!
//! Carried over verbatim from `src-tauri/src/mesh_sync.rs` and friends
//! (per `plans/11-mesh-sync.md`). The mesh logic is the most battle-tested
//! subsystem in the repo; the only changes during the Qt port are:
//!
//! * Files relocated from `src-tauri/src/{mesh_sync, peer_*, ...}.rs`
//!   into this module.
//! * `tauri::AppHandle::emit(...)` calls in `coordinator.rs` and
//!   `http_server.rs` replaced with `MeshEvents` trait calls.
//!
//! Submodule layout matches `plans/02-rust-core-services.md`.

pub mod commands;
pub mod coordinator;
pub mod http_server;
pub mod peer_manager;
pub mod peer_outbox;
pub mod snapshot_worker;
pub mod udp_notify;

// ----------------------------------------------------------------------------
// Constants — wire-protocol + timing knobs.
//
// Mirrored in `crate::settings` (DEFAULT_HTTP_CONTROL_PORT / DEFAULT_HTTP_DATA_PORT)
// because settings was ported before mesh; once `coordinator` lands the
// settings module's duplicates can be deleted in favor of these.
// ----------------------------------------------------------------------------

// v6 epoch (clean-DB break) network parameters — bumped wholesale from
// the v5 epoch (49211/49212/4255/239.42.5.3) so a v6 node and a v5 node
// can run side-by-side on one machine without port/group contention. The
// MESH_EPOCH check below already isolates the two at the app layer; the
// distinct ports + multicast group make the isolation total at the
// network layer too.
pub const DEFAULT_HTTP_CONTROL_PORT: u16 = 49221;
pub const DEFAULT_HTTP_DATA_PORT: u16 = 49222;
pub const DEFAULT_HTTP_PORT: u16 = DEFAULT_HTTP_CONTROL_PORT;
pub const DEFAULT_UDP_PORT: u16 = 4265;
pub const DEFAULT_MULTICAST_GROUP: &str = "239.42.6.3";
pub const MULTICAST_TTL: u32 = 1;

pub const PEER_LOOP_INTERVAL_MS: u64 = 3_000;
pub const UDP_HEARTBEAT_INTERVAL_MS: u64 = 3_000;
pub const UDP_SILENCE_THRESHOLD_MS: u64 = 15_000;
pub const PEER_DEAD_POLL_COUNT: i32 = 3;
pub const SNAPSHOT_INTERVAL_MS: u64 = 30_000;
/// Periodic re-arm of `snapshot_needed` on the leader. Guarantees a
/// fresh NAS snapshot even on completely quiet days where no edits
/// flowed through `on_metadata_edited` / `on_table_changed`. The
/// existing `SNAPSHOT_INTERVAL_MS` gate still applies; this just
/// flips the flag so the gate fires.
pub const SNAPSHOT_HEARTBEAT_INTERVAL_MS: u64 = 10 * 60 * 1_000;
pub const EDIT_QUEUE_FLUSH_INTERVAL_MS: u64 = 5_000;

pub const SNAPSHOT_MTIME_POLL_INTERVAL_MS: u64 = 10_000;
pub const STALE_FILE_CLEANUP_MS: u64 = 7 * 24 * 3600 * 1_000;
pub const MAX_EDIT_RETRY_COUNT: i32 = 10;

// ----------------------------------------------------------------------------
// Path-identity migration — mesh epoch.
//
// New-build nodes form a SEPARATE mesh from old-build nodes: they advertise
// `MESH_EPOCH` on every discovery channel + HTTP request and ignore peers /
// reject requests that don't match. The NAS snapshot also moves to a new
// filename so the two epochs never share a snapshot. Old builds have no epoch
// field — it deserializes to 0 and is skipped. See
// plans/path-identity-migration.md.
// ----------------------------------------------------------------------------

/// Current mesh epoch. Bumped on an incompatible wire/snapshot change;
/// nodes only peer with / accept data from same-epoch nodes. Epoch 4 is
/// the first tagged-path-identity epoch (old builds advertise no field →
/// deserialize to 0). Epoch 6 is the clean-DB break: a fresh empty
/// `ufb_v5.db` + a fresh `<farm>/v6` snapshot tree, abandoning the v5
/// data polluted by the NULL-uuid snapshot-restore leak.
pub const MESH_EPOCH: u32 = 6;

/// NAS snapshot filenames for this epoch. Old-epoch nodes use
/// `ufb_snapshot_v5.*` and never touch these.
pub const SNAPSHOT_FILENAME: &str = "ufb_snapshot_v6.db";
pub const SNAPSHOT_META_FILENAME: &str = "ufb_snapshot_v6.meta.json";

/// Umbrella subfolder under the user's farm/share path that contains
/// EVERYTHING this version writes (snapshots, phonebook nodes). A clean
/// break from the v5 layout — old nodes wrote to `<farm>/v5/...` and
/// never look here, so the two versions can share one farm path without
/// contention. NOTE: shared templates are intentionally reused from the
/// clean `v5/templates` tree (see templates.rs) — they were never
/// polluted, so the bump preserves every user's column configs.
pub const FARM_VERSION_SUBDIR: &str = "v6";

/// `<farm>/v5` — the root for all v5 shared-folder writes.
pub fn farm_version_root(farm_path: &str) -> std::path::PathBuf {
    std::path::Path::new(farm_path).join(FARM_VERSION_SUBDIR)
}

/// Full path to the shared NAS snapshot DB under `farm_path`.
pub fn snapshot_file_path(farm_path: &str) -> std::path::PathBuf {
    farm_version_root(farm_path)
        .join("snapshots")
        .join(SNAPSHOT_FILENAME)
}
