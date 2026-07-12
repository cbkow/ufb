/// On-demand NAS sync via native OS user-mode filesystem drivers.
///
/// Windows: WinFsp via the `winfsp` crate.
/// macOS: NFS3 loopback server via the `nfsserve` crate.
///
/// The sync module presents NAS files as virtual entries in the local filesystem.
/// Each user-space callback (open, read, readdir, write) is answered on the fly —
/// serve from local cache when warm, pass through to SMB otherwise. No persistent
/// placeholder state; no reconciliation database after offline periods.

pub mod cache_core;

#[cfg(any(windows, target_os = "macos"))]
pub mod nas_health;

#[cfg(windows)]
pub mod windows_cache;
#[cfg(windows)]
pub mod winfsp_server;

#[cfg(any(windows, target_os = "macos"))]
pub mod conflict;
// Slice G: `macos_watcher` retained only for `post_darwin_notification`
// — used by mount_service::handle_freshness_sweep to nudge the
// FinderSync extension. The FSEvents watcher, poll loop, dirty_folders
// set, and EchoSuppressor were all unused after Slice B (NFS server
// is the macOS VFS surface; no FileProvider subscriber for them).
#[cfg(target_os = "macos")]
pub mod macos_watcher;
#[cfg(target_os = "macos")]
pub mod macos_cache;
#[cfg(target_os = "macos")]
pub mod nfs_server;
#[cfg(target_os = "macos")]
pub use macos_cache::MacosCache;
#[cfg(windows)]
pub use windows_cache::CacheIndex;
// `connectivity` (NasConnectivity) deleted 2026-07-11 — superseded by
// the orchestrator-owned NasHealth (Slice D) which the WinFsp provider
// reads; nothing fed the old atomic since then.

/// Per-domain cache map shared between main and the NFS server. Keyed by
/// share name. Readers: NFS server startup, mount_service drain/stats.
/// Writers: main, on first hydration of a new mount.
#[cfg(target_os = "macos")]
pub type SharedCaches = std::sync::Arc<
    std::sync::RwLock<std::collections::HashMap<String, std::sync::Arc<MacosCache>>>,
>;

/// Per-domain cache map for Windows WinFsp mounts. Same pattern as macOS.
#[cfg(windows)]
pub type SharedCaches = std::sync::Arc<
    std::sync::RwLock<std::collections::HashMap<String, std::sync::Arc<CacheIndex>>>,
>;

/// Handle returned from `nfs_server::start` / `winfsp_server::start` for a
/// single per-domain sync server. Carries the shutdown primitive and the
/// task/thread join handle so callers can tear down the server cleanly
/// (e.g. on cache-root change or agent quit).
///
/// Dropping a `SyncServerHandle` without calling `shutdown_and_wait` does
/// NOT shut down the server — the shutdown signal must be sent explicitly.
/// This is intentional: the default process lifetime behavior (park
/// forever) should not regress just because someone forgot to hold the
/// handle somewhere.
#[cfg(any(target_os = "macos", windows))]
pub struct SyncServerHandle {
    /// Share name — kept for teardown log context (read on macOS;
    /// Windows logs via the dispatcher thread instead).
    #[cfg_attr(windows, allow(dead_code))]
    pub domain: String,
    #[cfg(target_os = "macos")]
    shutdown_tx: tokio::sync::oneshot::Sender<()>,
    #[cfg(target_os = "macos")]
    task_handle: tokio::task::JoinHandle<()>,
    #[cfg(windows)]
    shutdown_tx: std::sync::mpsc::Sender<()>,
    #[cfg(windows)]
    thread_handle: Option<std::thread::JoinHandle<()>>,
}

// `SyncServersRegistry` was deleted in Slice B. Each orchestrator
// owns its own `SyncServerHandle` directly and drives lifecycle via
// FSM effects.

#[cfg(any(target_os = "macos", windows))]
impl SyncServerHandle {
    #[cfg(target_os = "macos")]
    pub fn new_macos(
        domain: String,
        shutdown_tx: tokio::sync::oneshot::Sender<()>,
        task_handle: tokio::task::JoinHandle<()>,
    ) -> Self {
        Self { domain, shutdown_tx, task_handle }
    }

    #[cfg(windows)]
    pub fn new_windows(
        domain: String,
        shutdown_tx: std::sync::mpsc::Sender<()>,
        thread_handle: std::thread::JoinHandle<()>,
    ) -> Self {
        Self { domain, shutdown_tx, thread_handle: Some(thread_handle) }
    }

    /// Signal shutdown and wait for the server to exit. Safe to call
    /// exactly once.
    pub async fn shutdown_and_wait(mut self) {
        #[cfg(target_os = "macos")]
        {
            let _ = self.shutdown_tx.send(());
            let _ = self.task_handle.await;
        }
        #[cfg(windows)]
        {
            let _ = self.shutdown_tx.send(());
            if let Some(h) = self.thread_handle.take() {
                // Join the OS thread off the tokio runtime so we don't
                // block an executor worker while WinFsp unmounts.
                let _ = tokio::task::spawn_blocking(move || h.join()).await;
            }
        }
    }
}
