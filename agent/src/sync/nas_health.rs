//! NAS reachability tracking — shared between macOS NFS and Windows WinFsp.
//!
//! Background probe loop `stat()`s the share root every `PROBE_INTERVAL`.
//! After `FAILURE_THRESHOLD` consecutive failures, flips to "offline" so
//! handlers can short-circuit SMB ops instead of waiting 60 seconds for the
//! kernel timeout. Platform-neutral: no macOS or Windows-specific code here.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

/// Shared NAS reachability atomic.
///
/// Slice D refactor: NasHealth no longer runs its own probe loop. The
/// orchestrator's 30s heartbeat (`Orchestrator::handle_heartbeat_result`)
/// is the single source of truth for "is the NAS reachable right now";
/// it calls `set_online` after each probe. PassthroughFs reads
/// `is_online` to short-circuit SMB-touching ops with JUKEBOX when
/// offline. Eliminates the pre-Slice-D leak where every NFS server
/// respawn spawned an orphan probe loop holding `Arc<NasHealth>`
/// forever.
pub struct NasHealth {
    domain: String,
    #[allow(dead_code)]
    nas_root: PathBuf,
    connected: AtomicBool,
}

impl NasHealth {
    pub fn new(domain: String, nas_root: PathBuf) -> Arc<Self> {
        Arc::new(Self {
            domain,
            nas_root,
            // Start optimistic — the orchestrator's first heartbeat
            // (30s after boot) corrects to false if the NAS is gone.
            connected: AtomicBool::new(true),
        })
    }

    pub fn is_online(&self) -> bool {
        self.connected.load(Ordering::Relaxed)
    }

    /// External driver: orchestrator calls this from its heartbeat /
    /// reconnect paths. Logs only on edge transitions so a healthy
    /// link doesn't spam the log every 30s.
    pub fn set_online(&self, online: bool) {
        let prev = self.connected.swap(online, Ordering::Relaxed);
        if prev != online {
            if online {
                log::info!(
                    "[nas-health] {} NAS reachable again — resuming writes",
                    self.domain
                );
            } else {
                log::warn!(
                    "[nas-health] {} NAS unreachable — serving from cache only",
                    self.domain
                );
            }
        }
    }
}
