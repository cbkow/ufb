//! `AppCore` — the shared Rust-side container that the binding layer wraps
//! into cxx-qt QObjects. Replaces the Tauri-era `AppState` (which was a
//! `tauri::State<AppState>` injected into commands).
//!
//! Holds:
//!   * The DB pool
//!   * Each manager (subscription, metadata, columns, bookmarks, backup,
//!     transcode, mount_client, mesh)
//!   * In-memory settings cache (RwLock<AppSettings>)
//!   * Resolved tool paths (ffmpeg, ffprobe, exiftool)
//!   * Event-trait handles for each emitting subsystem
//!
//! Phase 14: thumbnails + system_icons moved to C++ (app/thumbnails/),
//! so the corresponding Rust managers and FFI providers were removed.
//!
//! The `AppCoreEvents` struct bundles the per-domain trait objects so
//! callers can construct AppCore in one shot. The bindings layer
//! implements each trait as a thin shim that forwards into the matching
//! Qt signal on the corresponding QObject; tests use the `Noop*` impls
//! from `crate::events`.

use crate::backup::BackupManager;
use crate::bookmarks::BookmarkManager;
use crate::columns::ColumnConfigManager;
use crate::db::Database;
use crate::events::{FileOpsEventsArc, MeshEventsArc, MountEventsArc, TranscodeEventsArc};
use crate::mesh::coordinator::MeshSyncManager;
use crate::metadata::MetadataManager;
use crate::mount_client::MountClient;
use crate::settings::AppSettings;
use crate::subscription::SubscriptionManager;
use crate::transcode::TranscodeManager;
use std::path::PathBuf;
use std::sync::{Arc, RwLock};

/// Bundle of event-trait handles handed to `AppCore::new`. Each is a
/// trait object that forwards to the binding layer (cxx-qt → Qt signals).
#[derive(Clone)]
pub struct AppCoreEvents {
    pub mesh: MeshEventsArc,
    pub transcode: TranscodeEventsArc,
    pub mount: MountEventsArc,
    pub file_ops: FileOpsEventsArc,
}

/// Tool paths resolved at startup. The binding layer is responsible for
/// finding these (next to the executable on Windows; inside the .app
/// bundle on macOS) and passing them in.
#[derive(Debug, Clone)]
pub struct ToolPaths {
    pub ffmpeg: PathBuf,
    pub ffprobe: PathBuf,
    pub exiftool: PathBuf,
}

/// The shared Rust core container. Held behind an `Arc` and cloned into
/// each cxx-qt QObject + every long-running task.
pub struct AppCore {
    pub db: Arc<Database>,
    pub settings: Arc<RwLock<AppSettings>>,
    pub device_id: String,

    pub subscription: Arc<SubscriptionManager>,
    pub metadata: Arc<MetadataManager>,
    pub columns: Arc<ColumnConfigManager>,
    pub bookmarks: Arc<BookmarkManager>,
    pub backup: Arc<BackupManager>,
    pub transcode: Arc<TranscodeManager>,
    pub mount_client: Arc<MountClient>,

    /// Mesh manager — held in a Mutex<Option> because construction is
    /// settings-dependent (farm path) and may be re-created on settings
    /// change. `None` until `init_mesh` is called or the user enables it
    /// for the first time.
    pub mesh: tokio::sync::Mutex<Option<Arc<MeshSyncManager>>>,

    pub events: AppCoreEvents,
    pub tool_paths: ToolPaths,
}

impl AppCore {
    /// Construct the shared core. Opens the DB, runs migrations,
    /// initialises every manager, but does NOT start the mesh subsystem
    /// or the agent IPC connection — those start on demand via
    /// `start_agent_client` / `init_mesh` so the binding layer can wire
    /// signals first.
    pub fn new(events: AppCoreEvents, tool_paths: ToolPaths) -> anyhow::Result<Arc<Self>> {
        let db_path = crate::utils::get_database_path();
        let db = Arc::new(
            Database::open(&db_path)
                .map_err(|e| anyhow::anyhow!("failed to open DB at {:?}: {}", db_path, e))?,
        );
        db.run_migrations()
            .map_err(|e| anyhow::anyhow!("DB migrations failed: {}", e))?;

        let settings = Arc::new(RwLock::new(AppSettings::load()));
        let device_id = crate::utils::get_device_id();

        let subscription = Arc::new(SubscriptionManager::new(db.clone()));
        let metadata = Arc::new(MetadataManager::new(db.clone()));
        let columns = Arc::new(ColumnConfigManager::new(db.clone()));
        let bookmarks = Arc::new(BookmarkManager::new(db.clone()));
        let backup = Arc::new(BackupManager::new(device_id.clone()));
        let transcode = Arc::new(TranscodeManager::new(
            tool_paths.ffmpeg.clone(),
            tool_paths.ffprobe.clone(),
            tool_paths.exiftool.clone(),
            events.transcode.clone(),
        ));
        let mount_client = Arc::new(MountClient::new());

        Ok(Arc::new(Self {
            db,
            settings,
            device_id,
            subscription,
            metadata,
            columns,
            bookmarks,
            backup,
            transcode,
            mount_client,
            mesh: tokio::sync::Mutex::new(None),
            events,
            tool_paths,
        }))
    }

    /// Spawn the transcode worker loop. Idempotent — call once after
    /// construction.
    pub fn start_transcode_worker(self: &Arc<Self>) {
        self.transcode.start_worker();
    }

    /// Spawn the agent IPC client connection loop. Idempotent — call
    /// once at startup.
    pub fn start_agent_client(self: &Arc<Self>) {
        self.mount_client.start(self.events.mount.clone());
    }

    /// Build (or rebuild) the mesh subsystem from current settings and
    /// stash it in `self.mesh`. Caller invokes `set_enabled(true)` on
    /// the returned manager when ready to start sync.
    ///
    /// Returns `None` if mesh isn't configured (empty farm_path or node_id).
    pub async fn init_mesh(self: &Arc<Self>) -> Option<Arc<MeshSyncManager>> {
        let (farm_path, node_id, http_port, data_port, tags_str) = {
            let s = self.settings.read().unwrap();
            (
                s.mesh_sync.farm_path.clone(),
                s.mesh_sync.node_id.clone(),
                s.mesh_sync.http_port,
                s.mesh_sync.data_port,
                s.mesh_sync.tags.clone(),
            )
        };

        if farm_path.is_empty() || node_id.is_empty() {
            log::info!("Mesh sync not configured (farm_path or node_id empty)");
            return None;
        }

        let tags: Vec<String> = tags_str
            .split(',')
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .collect();

        let mgr = Arc::new(MeshSyncManager::new(
            farm_path,
            node_id,
            http_port,
            data_port,
            tags,
            self.db.clone(),
            self.columns.clone(),
            self.events.mesh.clone(),
        ));

        *self.mesh.lock().await = Some(mgr.clone());
        Some(mgr)
    }
}
