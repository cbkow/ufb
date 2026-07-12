//! NFS3 loopback server — presents an SMB mount to Finder via macOS's native
//! kernel NFS client, sidestepping FileProvider's per-op framework overhead.
//!
//! Phase 1 (this file, current): read-only passthrough to the backing SMB
//! mount path. Identical behaviour to the standalone `mediamount-nfs-spike`
//! crate — this module exists to prove the NFS server integrates cleanly into
//! the agent process.
//!
//! Phase 1 (next iteration): swap the live `fs::read_dir` / `stat` calls for
//! SQLite-backed lookups against `MacosCache`. The request path becomes a
//! single indexed SELECT; maintenance (NAS polling, drift detection, cache
//! refresh) runs in decoupled worker tasks.
//!
//! One NFS server per sync-enabled mount. Each binds a distinct loopback port
//! (base + offset) so multiple mounts don't collide. The client mounts at
//! `~/ufb/mounts/<share>` — the same user-facing path as non-sync plain-SMB
//! mounts, so toggling sync on a mount does not invalidate bookmarks. A stale
//! symlink at that path (from a prior non-sync run) is unlinked before the
//! NFS mount.

use crate::messages::{AgentToUfb, ConflictDetectedMsg};
use crate::sync::conflict;
use crate::sync::macos_cache::{self, CachedAttr, MacosCache, CHUNK_SIZE};
use crate::sync::nas_health::NasHealth;
use async_trait::async_trait;
use nfsserve::{
    nfs::{
        fattr3, fileid3, filename3, ftype3, mode3, nfspath3, nfsstat3, nfstime3, sattr3,
        set_size3, specdata3,
    },
    tcp::{NFSTcp, NFSTcpListener},
    vfs::{DirEntry, NFSFileSystem, ReadDirResult, VFSCapabilities},
};
use std::{
    os::unix::fs::FileExt,
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration,
};
use tokio::sync::mpsc;

/// Base loopback port. Each enabled mount is assigned `BASE_PORT + offset`.
pub const BASE_PORT: u16 = 12345;

// Slice G: the `MOUNTS` global static was deleted. Each orchestrator
// now owns its mount via `SyncServerHandle::shutdown_and_wait`, which
// invokes `umount` as part of the task's cleanup. The standalone
// `unmount_all()` helper was redundant — it would double-unmount the
// same paths the handle's drain already covered.

/// Force-unmount every `localhost:` NFS share under `~/ufb/mounts/`.
/// Called once at agent startup before anything else touches the
/// filesystem.
///
/// When the agent crashes or is `kill -9`'d, the kernel keeps its NFS
/// mount entries pointing at the now-dead loopback NFS server (port
/// 12345). Any process that subsequently `stat`s the mount point —
/// Finder, Spotlight, mds, even `ls` — blocks indefinitely waiting
/// for the dead server to respond, and the hang propagates through
/// the LaunchServices / FSEvents / Spotlight ecosystems until the
/// machine is rebooted.
///
/// We scan the live `mount(8)` table for entries of the shape
/// `localhost:/<domain> on <our-user-mounts-dir>/<domain>` and run
/// `umount -f` against each. `umount -f` does NOT block on the dead
/// server (unlike a path-based stat), so this is safe to run at
/// startup. Any of our own live mounts from a still-running sibling
/// agent would also be force-unmounted, but the orchestrator
/// re-mounts immediately afterwards — and the multi-agent scenario
/// is already broken in other ways (PID file lock).
pub fn cleanup_stale_mounts_on_startup() {
    let Ok(out) = std::process::Command::new("mount").output() else {
        return;
    };
    let text = String::from_utf8_lossy(&out.stdout);
    let mounts_root = {
        let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
        PathBuf::from(home).join("ufb/mounts")
    };
    let mounts_root_str = format!("{}/", mounts_root.display());
    let mut victims: Vec<String> = Vec::new();
    for line in text.lines() {
        // mount line: "localhost:/<domain> on /Users/.../ufb/mounts/<domain> (nfs, ...)"
        let Some((source, rest)) = line.split_once(" on ") else { continue };
        if !source.starts_with("localhost:/") { continue; }
        let Some((path, _)) = rest.split_once(' ') else { continue };
        if !path.starts_with(&mounts_root_str) { continue; }
        victims.push(path.to_string());
    }
    if victims.is_empty() { return; }
    log::warn!(
        "[nfs-server] startup: {} stale localhost NFS mount(s) detected — force-unmounting",
        victims.len(),
    );
    for path in victims {
        log::warn!("[nfs-server]   umount -f {}", path);
        let _ = std::process::Command::new("umount")
            .arg("-f")
            .arg(&path)
            .status();
    }
}

/// Local user-facing mount point for a domain. Unified with the non-sync
/// SMB symlink path so toggling sync preserves bookmarks.
pub fn mount_point_for(domain: &str) -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
    PathBuf::from(home).join("ufb/mounts").join(domain)
}

/// True if `path` currently appears as a mount point in `mount(8)` output.
pub fn is_mounted(path: &Path) -> bool {
    let Ok(out) = std::process::Command::new("mount").output() else {
        return false;
    };
    let text = String::from_utf8_lossy(&out.stdout);
    let needle = format!(" on {} ", path.display());
    text.contains(&needle)
}

/// Prepare `path` for use as an NFS mount point. Existing installs may have a
/// plain-SMB symlink at this location (pointing to `/Volumes/<share>`); we
/// unlink it first so `mkdir` succeeds and the NFS mount can take over.
/// Idempotent — returns Ok(()) if the directory is already in place.
fn prepare_nfs_mount_point(path: &Path) -> Result<(), String> {
    match std::fs::symlink_metadata(path) {
        Ok(md) if md.file_type().is_symlink() => {
            std::fs::remove_file(path).map_err(|e| {
                format!("remove stale symlink {}: {}", path.display(), e)
            })?;
        }
        Ok(_) => { /* already a dir (possibly a live mount, is_mounted handles that) */ }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => { /* fresh install */ }
        Err(e) => return Err(format!("lstat {}: {}", path.display(), e)),
    }
    Ok(())
}

/// Mount the loopback NFS export at `mount_point`. Pre-cleans a stale mount
/// if the process was previously killed uncleanly. Returns the path that
/// was mounted (same as `mount_point`) on success.
fn mount_nfs_share(domain: &str, port: u16, mount_point: &Path) -> Result<(), String> {
    prepare_nfs_mount_point(mount_point)?;
    std::fs::create_dir_all(mount_point)
        .map_err(|e| format!("mkdir {}: {}", mount_point.display(), e))?;

    if is_mounted(mount_point) {
        log::warn!(
            "[nfs-server] {}: stale mount at {} — force-unmounting",
            domain,
            mount_point.display()
        );
        let _ = std::process::Command::new("umount")
            .arg("-f")
            .arg(mount_point)
            .status();
    }

    // Stays a HARD mount (soft mounts can surface spurious write errors
    // to apps), but with two escape hatches the default lacks:
    //  * intr — in-flight ops are interruptible, so a stuck Finder can
    //    be force-quit instead of beachballing until reboot;
    //  * deadtimeout=60 — if the loopback server stops answering for
    //    60s (agent killed -9, wedged on a dead SMB session), the
    //    kernel force-unmounts instead of hanging every process that
    //    touches the tree. The offline JUKEBOX gates keep the server
    //    answering during mere NAS outages, so this only fires when the
    //    agent itself is gone — exactly when the mount is unrecoverable
    //    anyway.
    let opts = format!(
        "port={0},mountport={0},nolocks,vers=3,tcp,nobrowse,intr,deadtimeout=60,actimeo=1,rsize=1048576,wsize=1048576",
        port
    );
    let out = std::process::Command::new("mount")
        .args([
            "-t", "nfs",
            "-o", &opts,
            &format!("localhost:/{}", domain),
            &mount_point.to_string_lossy(),
        ])
        .output()
        .map_err(|e| format!("spawn mount: {}", e))?;

    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        return Err(format!("mount_nfs failed: {}", stderr.trim()));
    }

    // Slice G: per-orchestrator SyncServerHandle owns the mount
    // lifetime now; no global registry needed.
    Ok(())
}

/// Read-only cache-backed filesystem.
///
/// Metadata is served from `MacosCache` when warm. Cold folders trigger a live
/// `fs::read_dir` that populates the cache, then serves from it — subsequent
/// visits are all SQLite.
///
/// Paths in the cache are relative to the share root (`""` = root, no leading
/// slash). We translate between relative cache paths and absolute
/// filesystem paths (rooted at `nas_root`) at the I/O boundary.
pub struct PassthroughFs {
    domain: String,
    nas_root: PathBuf,
    cache: Arc<MacosCache>,
    /// Agent → UFB channel for out-of-band events (e.g. ConflictDetected).
    ipc_tx: mpsc::Sender<AgentToUfb>,
    /// Rolling NAS reachability state. Ops that would touch SMB consult
    /// this first and short-circuit with JUKEBOX when offline.
    health: Arc<NasHealth>,
}

impl PassthroughFs {
    pub fn new(
        domain: String,
        nas_root: PathBuf,
        cache: Arc<MacosCache>,
        ipc_tx: mpsc::Sender<AgentToUfb>,
        health: Arc<NasHealth>,
    ) -> Result<Self, String> {
        let canon = nas_root
            .canonicalize()
            .map_err(|e| format!("Failed to canonicalize {}: {}", nas_root.display(), e))?;
        // Root always has an fh (seeded at schema init; ensure idempotently).
        cache.ensure_fh("");
        Ok(Self { domain, nas_root: canon, cache, ipc_tx, health })
    }

    /// Short-circuit guard for SMB-touching ops. Returns `NFS3ERR_JUKEBOX`
    /// when the heartbeat probe has marked the NAS offline, so clients
    /// get a fast retry signal instead of waiting for a 60-second SMB
    /// timeout on every op.
    #[inline]
    fn require_online(&self) -> Result<(), nfsstat3> {
        if self.health.is_online() {
            Ok(())
        } else {
            Err(nfsstat3::NFS3ERR_JUKEBOX)
        }
    }

    fn absolute(&self, rel: &str) -> PathBuf {
        if rel.is_empty() {
            self.nas_root.clone()
        } else {
            self.nas_root.join(rel)
        }
    }

    fn rel_path(&self, fh: fileid3) -> Result<String, nfsstat3> {
        match self.cache.path_for_fh(fh) {
            Some(p) => Ok(p),
            None => {
                let total = self.cache.nfs_handles_count();
                log::warn!(
                    "[nfs-server] {}: STALE — no nfs_handles row for fh={} (table has {} rows total)",
                    self.domain,
                    fh,
                    total
                );
                Err(nfsstat3::NFS3ERR_STALE)
            }
        }
    }

    /// Cold-path populate: do a single `fs::read_dir` on `parent_rel` and
    /// push every entry into `known_files` via `record_enumeration`. Cheap to
    /// call repeatedly (idempotent upsert).
    ///
    /// Slice E: async + spawn_blocking. The SMB read_dir + per-entry
    /// metadata stat can take 100ms+ per folder; running on the tokio
    /// worker stalled every concurrent NFS request when ~8 cold
    /// folders enumerated at once.
    async fn populate_folder(&self, parent_rel: &str) -> Result<(), nfsstat3> {
        self.require_online()?;
        let abs = self.absolute(parent_rel);
        let (entries, is_partial): (Vec<crate::messages::DirEntry>, bool) =
            tokio::task::spawn_blocking(move || -> Result<(Vec<_>, bool), nfsstat3> {
                let rd = std::fs::read_dir(&abs).map_err(io_to_nfsstat)?;
                let mut entries: Vec<crate::messages::DirEntry> = Vec::new();
                // Any per-entry error (flaky SMB readdir under load, stat
                // timeout) means the listing may be missing real files. A
                // partial listing must NOT drive orphan pruning — a file
                // absent from a partial list is indistinguishable from a
                // deleted one, and pruning it destroys its fh (the client's
                // open handle goes STALE mid-copy).
                let mut is_partial = false;
                for e in rd {
                    let e = match e {
                        Ok(e) => e,
                        Err(_) => {
                            is_partial = true;
                            continue;
                        }
                    };
                    // NFC-normalize: SMB may hand back NFD (files written
                    // by old macs) — the cache key space is all-NFC.
                    let name =
                        macos_cache::nfc(&e.file_name().to_string_lossy());
                    // Exact NAS sentinels + dotfiles only — a broad @/#
                    // prefix filter hid legitimate user content like
                    // `@Media` or `#2 take.mov` from the mount entirely.
                    if crate::sync::cache_core::is_ignored_name(&name) {
                        continue;
                    }
                    let meta = match e.metadata() {
                        Ok(m) => m,
                        Err(_) => {
                            is_partial = true;
                            continue;
                        }
                    };
                    let mtime = meta
                        .modified()
                        .ok()
                        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                        .map(|d| d.as_secs_f64())
                        .unwrap_or(0.0);
                    let created = meta
                        .created()
                        .ok()
                        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                        .map(|d| d.as_secs_f64())
                        .unwrap_or(0.0);
                    entries.push(crate::messages::DirEntry {
                        name,
                        is_dir: meta.is_dir(),
                        size: meta.len(),
                        modified: mtime,
                        created,
                    });
                }
                Ok((entries, is_partial))
            })
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)??;
        self.cache.record_enumeration(parent_rel, &entries, is_partial);
        Ok(())
    }

    /// Build an `fattr3` for the share root (no known_files row for "").
    /// Slice E: async + spawn_blocking — stat'ing the SMB root can
    /// hang briefly on a slow link.
    async fn root_attr(&self) -> Result<fattr3, nfsstat3> {
        let nas_root = self.nas_root.clone();
        let meta = tokio::task::spawn_blocking(move || std::fs::metadata(&nas_root))
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)?
            .map_err(io_to_nfsstat)?;
        Ok(nfsserve::fs_util::metadata_to_fattr3(1, &meta))
    }

    /// Conflict-detection pre-flight for a truncate-style write. If the NAS
    /// file has changed since we last cached it (concurrent writer), COPY
    /// the current NAS content to a sidecar path before letting the truncate
    /// proceed. Both versions survive: the user's write lands on the original
    /// path, the other writer's version is preserved as `.conflict-*`.
    /// Emits `ConflictDetected` to UFB when this fires.
    ///
    /// Slice G: bail loudly when the sidecar copy fails. Pre-refactor
    /// the function logged + returned, allowing the truncate to proceed
    /// and silently lose the conflicting version's data. Now it returns
    /// JUKEBOX up to setattr, which propagates to the NFS client — the
    /// user sees an error pill instead of silent data loss. Drift
    /// detection or copy preflight failures (where there's nothing
    /// useful to preserve) still return Ok so we don't block legitimate
    /// truncates.
    async fn preserve_conflict_sidecar_if_drifted(
        &self,
        rel: &str,
        abs: &Path,
    ) -> Result<(), nfsstat3> {
        let abs_for_stat = abs.to_path_buf();
        // Live NAS stat — wrapped in spawn_blocking.
        let live_meta = match tokio::task::spawn_blocking(move || {
            std::fs::metadata(&abs_for_stat)
        })
        .await
        {
            Ok(Ok(m)) => m,
            // Can't stat: either the file is gone (no conflict
            // possible) or SMB hiccup. Either way, no data to
            // preserve — let the truncate proceed.
            _ => return Ok(()),
        };
        let live_size = live_meta.len();
        let live_mtime = live_meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs_f64())
            .unwrap_or(0.0);

        // What we last saw.
        let cached = match self.cache.cached_attr(rel) {
            Some(c) => c,
            None => return Ok(()), // never seen — nothing to compare
        };

        // mtime granularity on Synology/SMB is ~1s; allow 2s slop.
        let drifted =
            live_size != cached.size || (live_mtime - cached.mtime).abs() > 2.0;
        if !drifted {
            return Ok(());
        }

        let conflict_abs = conflict::make_conflict_path(abs);
        let copy_src = abs.to_path_buf();
        let copy_dst = conflict_abs.clone();
        let copy_result =
            tokio::task::spawn_blocking(move || std::fs::copy(&copy_src, &copy_dst))
                .await;
        match copy_result {
            Ok(Ok(_)) => {}
            Ok(Err(e)) => {
                log::error!(
                    "[nfs-server] {}: conflict sidecar copy FAILED for {} → {}: {} \
                     — refusing to truncate (would lose remote version)",
                    self.domain,
                    rel,
                    conflict_abs.display(),
                    e
                );
                return Err(nfsstat3::NFS3ERR_JUKEBOX);
            }
            Err(e) => {
                log::error!(
                    "[nfs-server] {}: conflict sidecar task panicked: {} \
                     — refusing to truncate",
                    self.domain, e
                );
                return Err(nfsstat3::NFS3ERR_IO);
            }
        }

        let sidecar_name = conflict_abs
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default();
        log::warn!(
            "[nfs-server] {}: conflict on write to {} — preserved remote version as {}",
            self.domain,
            rel,
            sidecar_name
        );

        let detected_at = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let msg = AgentToUfb::ConflictDetected(ConflictDetectedMsg {
            domain: self.domain.clone(),
            original_path: rel.to_string(),
            conflict_path: sidecar_name,
            host: conflict::hostname_short(),
            detected_at,
        });
        if let Err(e) = self.ipc_tx.try_send(msg) {
            log::warn!("[nfs-server] {}: conflict event dropped: {}", self.domain, e);
        }
        Ok(())
    }

    /// Common tail for CREATE / MKDIR: stat the freshly-made entry, register
    /// it in `known_files` (trigger assigns an `fh`), return `(fh, attr)`.
    /// Slice E: async + spawn_blocking for the SMB stat.
    async fn register_new_entry(
        &self,
        child_rel: &str,
        name: &str,
        abs: &std::path::Path,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        let abs_for_io = abs.to_path_buf();
        let meta = tokio::task::spawn_blocking(move || std::fs::metadata(&abs_for_io))
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)?
            .map_err(io_to_nfsstat)?;
        let mtime = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs_f64())
            .unwrap_or(0.0);
        let created = meta
            .created()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs_f64())
            .unwrap_or(mtime);

        self.cache.record_new_entry(
            child_rel,
            name,
            meta.is_dir(),
            meta.len(),
            mtime,
            created,
        );

        let fh = self
            .cache
            .fh_for_path(child_rel)
            .ok_or(nfsstat3::NFS3ERR_IO)?;

        log::debug!(
            "[nfs-server] {}: registered new entry {} → fh={}",
            self.domain,
            child_rel,
            fh
        );

        let attr = CachedAttr {
            is_dir: meta.is_dir(),
            size: meta.len(),
            mtime,
            created,
            is_hydrated: false,
            hydrated_size: 0,
        };
        Ok((fh, attr_from_cache(fh, &attr)))
    }

    /// Serve a read from the local cache file (fully-hydrated fast path).
    fn read_from_cache(
        &self,
        fh: fileid3,
        offset: u64,
        len: usize,
        size: u64,
    ) -> Result<(Vec<u8>, bool), nfsstat3> {
        let cache_path = self.cache.cache_file_path(fh);
        let f = std::fs::File::open(&cache_path).map_err(io_to_nfsstat)?;
        let mut buf = vec![0u8; len];
        let n = f.read_at(&mut buf, offset).map_err(io_to_nfsstat)?;
        buf.truncate(n);
        let eof = offset + n as u64 >= size;
        Ok((buf, eof))
    }

    /// Chunk-aware read: for each chunk covered by the request, serve from
    /// the cache if the bitmap bit is set, else pull bytes from SMB and
    /// write them to the cache file (persisting the bitmap as we go).
    ///
    /// The cache file is sparse — we only write chunks we actually fetch —
    /// and the bitmap is authoritative for "is this chunk valid" because
    /// sparse holes would otherwise read as zeros.
    async fn read_with_bitmap(
        &self,
        fh: fileid3,
        rel: &str,
        attr: &CachedAttr,
        offset: u64,
        len: usize,
        bitmap: Vec<u8>,
    ) -> Result<(Vec<u8>, bool), nfsstat3> {
        let size = attr.size;
        let total_chunks = macos_cache::num_chunks(size);

        let cache_path = self.cache.cache_file_path(fh);
        let abs = self.absolute(rel);
        let domain = self.domain.clone();
        let rel_owned = rel.to_string();
        let health = self.health.clone();

        // Slice E: the entire SMB-read + cache-write loop runs in
        // one spawn_blocking task. Pre-refactor each std::fs op
        // executed inline on the async runtime; a 10MB cold read
        // could pin a tokio worker for hundreds of ms over SMB.
        let outcome: Result<(Vec<u8>, Vec<u8>, bool), nfsstat3> =
            tokio::task::spawn_blocking(move || -> Result<_, nfsstat3> {
                let mut bitmap = bitmap;
                let cache_file = std::fs::OpenOptions::new()
                    .read(true)
                    .write(true)
                    .create(true)
                    .truncate(false)
                    .open(&cache_path)
                    .map_err(io_to_nfsstat)?;
                if cache_file.metadata().map(|m| m.len()).unwrap_or(0) < size {
                    cache_file.set_len(size).map_err(io_to_nfsstat)?;
                }

                let mut smb_file: Option<std::fs::File> = None;
                let mut result = vec![0u8; len];
                let mut bitmap_dirty = false;

                let first_chunk = offset / CHUNK_SIZE;
                let last_chunk = (offset + len as u64 - 1) / CHUNK_SIZE;
                let mut chunk = first_chunk;
                while chunk <= last_chunk {
                    let cached = macos_cache::bit_is_set(&bitmap, chunk);
                    let mut run_end = chunk;
                    while run_end < last_chunk
                        && macos_cache::bit_is_set(&bitmap, run_end + 1) == cached
                    {
                        run_end += 1;
                    }

                    let run_start_byte = (chunk * CHUNK_SIZE).max(offset);
                    let run_end_byte =
                        ((run_end + 1) * CHUNK_SIZE).min(offset + len as u64);
                    let run_len = (run_end_byte - run_start_byte) as usize;
                    let result_offset = (run_start_byte - offset) as usize;

                    if cached {
                        let n = cache_file
                            .read_at(
                                &mut result
                                    [result_offset..result_offset + run_len],
                                run_start_byte,
                            )
                            .map_err(io_to_nfsstat)?;
                        if n < run_len {
                            for c in chunk..=run_end {
                                bit_unset(&mut bitmap, c);
                            }
                            bitmap_dirty = true;
                            log::warn!(
                                "[nfs-server] {}: short read from cache for {} run {}..{}, invalidating",
                                domain, rel_owned, chunk, run_end
                            );
                            continue;
                        }
                    } else {
                        // Health gate inside the blocking task —
                        // saves a 60s SMB timeout when offline.
                        if !health.is_online() {
                            return Err(nfsstat3::NFS3ERR_JUKEBOX);
                        }
                        let f = match smb_file {
                            Some(ref f) => f,
                            None => {
                                smb_file = Some(
                                    std::fs::File::open(&abs)
                                        .map_err(io_to_nfsstat)?,
                                );
                                smb_file.as_ref().unwrap()
                            }
                        };

                        let fetch_start = chunk * CHUNK_SIZE;
                        let fetch_end =
                            ((run_end + 1) * CHUNK_SIZE).min(size);
                        let fetch_len = (fetch_end - fetch_start) as usize;
                        let mut buf = vec![0u8; fetch_len];
                        // pread over SMB can legitimately return fewer
                        // bytes than requested without being at EOF —
                        // loop until the buffer fills or a zero read
                        // signals true EOF (the cached size can exceed
                        // the live SMB size after an out-of-band change).
                        let mut filled = 0usize;
                        while filled < fetch_len {
                            let n = f
                                .read_at(
                                    &mut buf[filled..],
                                    fetch_start + filled as u64,
                                )
                                .map_err(io_to_nfsstat)?;
                            if n == 0 {
                                break;
                            }
                            filled += n;
                        }
                        buf.truncate(filled);
                        if !is_all_zeros(&buf) {
                            cache_file
                                .write_all_at(&buf, fetch_start)
                                .map_err(io_to_nfsstat)?;
                        }
                        // Only chunks whose FULL logical range was
                        // actually fetched may set their bitmap bit.
                        // Marking unfetched chunks would permanently
                        // alias sparse zeros as file content.
                        let fetched_end = fetch_start + filled as u64;
                        for c in chunk..=run_end {
                            let chunk_logical_end =
                                ((c + 1) * CHUNK_SIZE).min(size);
                            if fetched_end >= chunk_logical_end {
                                macos_cache::set_bit(&mut bitmap, c);
                                bitmap_dirty = true;
                            }
                        }

                        let user_start = (run_start_byte - fetch_start) as usize;
                        let avail = buf.len().saturating_sub(user_start);
                        let copy_len = run_len.min(avail);
                        result[result_offset..result_offset + copy_len]
                            .copy_from_slice(
                                &buf[user_start..user_start + copy_len],
                            );
                        if copy_len < run_len {
                            // True EOF inside this run — everything past
                            // it is beyond the live file. Return a short
                            // read rather than zero-padding to the stale
                            // cached size.
                            result.truncate(result_offset + copy_len);
                            return Ok((result, bitmap, bitmap_dirty));
                        }
                    }
                    chunk = run_end + 1;
                }

                if bitmap_dirty {
                    let _ = cache_file.sync_all();
                }

                Ok((result, bitmap, bitmap_dirty))
            })
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let (result, bitmap, bitmap_dirty) = outcome?;

        if bitmap_dirty {
            if macos_cache::bitmap_is_complete(&bitmap, total_chunks) {
                self.cache.mark_fully_hydrated(rel, size);
                log::info!("[nfs-server] {}: fully hydrated {}", self.domain, rel);
            } else {
                self.cache.update_chunk_bitmap(rel, &bitmap);
            }
        }

        // A result shorter than requested means we hit true EOF on SMB
        // (live file smaller than the cached size) — report eof so the
        // client doesn't spin re-requesting the missing tail.
        let eof = result.len() < len || offset + len as u64 >= size;
        Ok((result, eof))
    }
}

/// Clear a bit in the chunk bitmap (used when a cached chunk turns out to
/// be invalid and we need to refetch).
#[inline]
fn bit_unset(bitmap: &mut [u8], chunk: u64) {
    let byte = (chunk / 8) as usize;
    let mask = !(1u8 << ((chunk % 8) as u8));
    if let Some(b) = bitmap.get_mut(byte) {
        *b &= mask;
    }
}

/// True if every byte in `buf` is 0. Fast path for keeping the cache
/// file sparse when the fetched chunk is all zeros (common in padded
/// media containers).
#[inline]
fn is_all_zeros(buf: &[u8]) -> bool {
    // Chunk-wise compare is faster than byte-wise in release builds —
    // the compiler autovectorizes the `iter().all` too, but explicit
    // 8-byte windows matches what a manual SIMD version would do and
    // is unambiguous.
    const STRIDE: usize = 8;
    let chunks = buf.chunks_exact(STRIDE);
    let remainder = chunks.remainder();
    chunks.map(|c| u64::from_ne_bytes(c.try_into().unwrap())).all(|w| w == 0)
        && remainder.iter().all(|&b| b == 0)
}

/// Convert a `CachedAttr` + fh into an `fattr3`. Used for all non-root entries.
///
/// Slice G: Y2038 saturation. `nfstime3.seconds` is u32 (NFS3 protocol
/// limit). Naive `as u32` casts wrap silently for mtimes past
/// 2038-01-19 03:14:07 UTC — handing back a tiny seconds value to the
/// kernel mid-2038. Saturate to `u32::MAX` instead so the rollover
/// surfaces as "extremely future" rather than "extremely past."
fn attr_from_cache(fh: fileid3, a: &CachedAttr) -> fattr3 {
    let ftype = if a.is_dir { ftype3::NF3DIR } else { ftype3::NF3REG };
    let mode: mode3 = if a.is_dir { 0o755 } else { 0o644 };
    let size = if a.is_dir { 4096 } else { a.size };
    let atime = nfstime3 {
        seconds: saturate_secs_u32(a.mtime),
        nseconds: ((a.mtime.fract()) * 1e9) as u32,
    };
    let mtime = atime;
    let ctime = nfstime3 {
        seconds: saturate_secs_u32(a.created),
        nseconds: ((a.created.fract()) * 1e9) as u32,
    };
    fattr3 {
        ftype,
        mode,
        nlink: if a.is_dir { 2 } else { 1 },
        uid: 501,
        gid: 20,
        size,
        used: if a.is_hydrated { a.hydrated_size } else { size },
        rdev: specdata3::default(),
        fsid: 0,
        fileid: fh,
        atime,
        mtime,
        ctime,
    }
}

/// Saturate fractional seconds-since-epoch into the u32 NFS3 wire field.
/// Negative values clamp to 0; values past `u32::MAX` (~year 2106) clamp
/// to `u32::MAX`. Per-protocol Y2106 limit; the 2038 wrap was a Rust
/// `as` cast quirk that this fixes.
#[inline]
fn saturate_secs_u32(secs: f64) -> u32 {
    if !secs.is_finite() || secs < 0.0 {
        0
    } else if secs >= u32::MAX as f64 {
        u32::MAX
    } else {
        secs as u32
    }
}

/// Map a Rust `io::Error` onto the closest NFS3 status code.
///
/// Transient NAS errors (timeouts, refused connections, unexpected EOF)
/// map to `NFS3ERR_JUKEBOX` — the NFS "please retry shortly" signal —
/// rather than opaque `NFS3ERR_IO`. Clients back off and try again
/// instead of failing the op outright, which is the right behaviour
/// when the NAS link is flaky but probably-coming-back.
/// True iff `e` is the portable spelling of ENOTEMPTY. `DirectoryNotEmpty`
/// is unstable on older Rust stdlibs and surfaces as `Other` there, so we
/// also squint at the raw OS errno — 66 on macOS, 39 on Linux.
fn is_dir_not_empty(e: &std::io::Error) -> bool {
    if matches!(e.kind(), std::io::ErrorKind::DirectoryNotEmpty) {
        return true;
    }
    matches!(e.raw_os_error(), Some(66) | Some(39))
}

/// Recovery path for the `remove_dir → ENOTEMPTY` case. Enumerates the
/// directory, deletes every entry whose name starts with `.`, `@`, or `#`
/// (the same filter `populate_folder` applies when building the NFS view
/// of the folder) recursively, then retries `remove_dir`.
///
/// Returns the original `NOTEMPTY` error untouched if any user-visible
/// entry remains — that's a real "not empty" and NFS3 should report it.
fn sweep_hidden_children_then_remove(abs: &std::path::Path) -> Result<(), nfsstat3> {
    let rd = std::fs::read_dir(abs).map_err(io_to_nfsstat)?;
    let mut hidden: Vec<std::path::PathBuf> = Vec::new();
    let mut has_visible = false;
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        // Must match populate_folder's filter: only names invisible to
        // enumeration may be swept. A broad @/# prefix here would
        // silently delete real user content (`@Media`, `#DELIVERED.mov`)
        // during an RMDIR.
        if crate::sync::cache_core::is_ignored_name(&name) {
            hidden.push(e.path());
        } else {
            has_visible = true;
        }
    }
    if has_visible {
        return Err(nfsstat3::NFS3ERR_NOTEMPTY);
    }
    for p in hidden {
        let meta = match std::fs::symlink_metadata(&p) {
            Ok(m) => m,
            Err(_) => continue,
        };
        let res = if meta.is_dir() {
            std::fs::remove_dir_all(&p)
        } else {
            std::fs::remove_file(&p)
        };
        if let Err(e) = res {
            log::warn!(
                "[nfs-server] sweep hidden child {} failed: {} — RMDIR will fail",
                p.display(), e
            );
        }
    }
    std::fs::remove_dir(abs).map_err(io_to_nfsstat)
}

fn io_to_nfsstat(e: std::io::Error) -> nfsstat3 {
    use std::io::ErrorKind::*;
    match e.kind() {
        NotFound => nfsstat3::NFS3ERR_NOENT,
        PermissionDenied => nfsstat3::NFS3ERR_ACCES,
        AlreadyExists => nfsstat3::NFS3ERR_EXIST,
        DirectoryNotEmpty => nfsstat3::NFS3ERR_NOTEMPTY,
        InvalidInput | InvalidData => nfsstat3::NFS3ERR_INVAL,
        // Anything that looks like "NAS is flaky, try me again later."
        // Clients interpret JUKEBOX as a transient error and retry after
        // a short backoff — way better UX than a hard IO failure.
        //
        // TimedOut is deliberately NOT here: it means the NAS was
        // reachable (require_online passed) yet the op still timed out —
        // a mid-transfer stall on a big file. JUKEBOX would make the
        // client re-issue the same doomed op forever (Finder beachball);
        // IO surfaces a real error the app can show.
        TimedOut => nfsstat3::NFS3ERR_IO,
        WouldBlock
        | ConnectionRefused
        | ConnectionReset
        | ConnectionAborted
        | NotConnected
        | BrokenPipe
        | UnexpectedEof => nfsstat3::NFS3ERR_JUKEBOX,
        // Disk full — NFS3 has a specific code for this.
        Unsupported => nfsstat3::NFS3ERR_NOTSUPP,
        _ => {
            // Special-case ENOSPC — its ErrorKind is `Other` in stable
            // stdlib (the stable `StorageFull` variant isn't exposed yet),
            // so we squint at the raw_os_error.
            if let Some(code) = e.raw_os_error() {
                match code {
                    28 /* ENOSPC */ => return nfsstat3::NFS3ERR_NOSPC,
                    30 /* EROFS */  => return nfsstat3::NFS3ERR_ROFS,
                    63 /* ENAMETOOLONG */ => return nfsstat3::NFS3ERR_NAMETOOLONG,
                    69 /* EDQUOT */ => return nfsstat3::NFS3ERR_DQUOT,
                    _ => {}
                }
            }
            nfsstat3::NFS3ERR_IO
        }
    }
}

#[async_trait]
impl NFSFileSystem for PassthroughFs {
    fn capabilities(&self) -> VFSCapabilities {
        VFSCapabilities::ReadWrite
    }

    fn root_dir(&self) -> fileid3 {
        // Root is seeded at schema init as path="". Its fh is always 1 on a
        // fresh DB; on an upgraded DB it's whatever AUTOINCREMENT assigned
        // the first time we inserted — so we look it up, not hardcode.
        self.cache.fh_for_path("").unwrap_or(1)
    }

    async fn lookup(&self, dirid: fileid3, filename: &filename3) -> Result<fileid3, nfsstat3> {
        let parent_rel = self.rel_path(dirid)?;
        let name = filename_to_string(filename)?;

        // Warm-path: straight SQLite lookup. The trigger on known_files has
        // already mirrored the path into nfs_handles, so fh_for_path hits.
        let child_rel = join_rel(&parent_rel, &name);
        if let Some(fh) = self.cache.fh_for_path(&child_rel) {
            // Still verify it's a known child of the parent (defence against
            // a stale handle lingering for a different folder).
            if self.cache.cached_attr(&child_rel).is_some() {
                log::debug!(
                    "[nfs-server] {}: lookup(warm) {} → fh={}",
                    self.domain, child_rel, fh
                );
                return Ok(fh);
            }
        }

        // Cold-path: populate the parent folder, then retry. Both the
        // incoming name and enumerated names are NFC + NOCASE-keyed, so
        // a single lookup covers all case/normalization variants.
        self.populate_folder(&parent_rel).await?;
        let result = self.cache.fh_for_path(&child_rel);
        match result {
            Some(fh) => {
                log::debug!(
                    "[nfs-server] {}: lookup(cold) {} → fh={}",
                    self.domain, child_rel, fh
                );
                Ok(fh)
            }
            None => {
                log::debug!(
                    "[nfs-server] {}: lookup {} → NOENT",
                    self.domain, child_rel
                );
                Err(nfsstat3::NFS3ERR_NOENT)
            }
        }
    }

    async fn getattr(&self, id: fileid3) -> Result<fattr3, nfsstat3> {
        let rel = self.rel_path(id)?;
        if rel.is_empty() {
            return self.root_attr().await;
        }
        if let Some(attr) = self.cache.cached_attr(&rel) {
            return Ok(attr_from_cache(id, &attr));
        }
        // Not in known_files — try populating the parent folder.
        let parent = crate::sync::macos_cache::parent_of(&rel).to_string();
        self.populate_folder(&parent).await?;
        let attr = self
            .cache
            .cached_attr(&rel)
            .ok_or(nfsstat3::NFS3ERR_NOENT)?;
        Ok(attr_from_cache(id, &attr))
    }

    async fn setattr(&self, id: fileid3, setattr: sattr3) -> Result<fattr3, nfsstat3> {
        let rel = self.rel_path(id)?;
        if rel.is_empty() {
            return Err(nfsstat3::NFS3ERR_ROFS);
        }
        let abs = self.absolute(&rel);

        // We honour truncate (editors do save-as-truncate-then-write). Mode,
        // uid, gid, atime, mtime are passthrough no-ops — SMB doesn't expose
        // fine-grained control we could proxy here, and NFS clients treat
        // them as advisory.
        if let set_size3::size(new_size) = setattr.size {
            self.require_online()?;

            // Slice E: per-fh write lock + spawn_blocking, matching
            // the write() path. Conflict-sidecar runs inside the lock
            // window so a concurrent read doesn't see torn state.
            let fh_lock = self.cache.fh_lock(id);
            let _write_guard = fh_lock.write().await;

            if new_size == 0 {
                self.preserve_conflict_sidecar_if_drifted(&rel, &abs).await?;
            }

            let abs_for_io = abs.clone();
            let truncate_result: Result<(), nfsstat3> =
                tokio::task::spawn_blocking(move || {
                    let f = std::fs::OpenOptions::new()
                        .write(true)
                        .open(&abs_for_io)
                        .map_err(io_to_nfsstat)?;
                    f.set_len(new_size).map_err(io_to_nfsstat)?;
                    f.sync_all().map_err(io_to_nfsstat)?;
                    Ok(())
                })
                .await
                .map_err(|_| nfsstat3::NFS3ERR_IO)?;
            truncate_result?;

            // Cached content is now stale — nuke it.
            self.cache.invalidate_cache(&rel, id);
            // Slice E: skip the post-truncate SMB stat; we know
            // size = new_size, mtime = now. Avoids the SMB attribute
            // cache staleness window (see write() for the same reasoning).
            let mtime = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs_f64())
                .unwrap_or(0.0);
            self.cache.update_nas_metadata(&rel, new_size, mtime);
        }

        self.getattr(id).await
    }

    async fn read(
        &self,
        id: fileid3,
        offset: u64,
        count: u32,
    ) -> Result<(Vec<u8>, bool), nfsstat3> {
        let rel = self.rel_path(id)?;
        if rel.is_empty() {
            return Err(nfsstat3::NFS3ERR_ISDIR);
        }
        let mut attr = self.cache.cached_attr(&rel).ok_or(nfsstat3::NFS3ERR_NOENT)?;
        if attr.is_dir {
            return Err(nfsstat3::NFS3ERR_ISDIR);
        }

        // Per-file freshness: folder-mtime re-enumeration only fires when
        // the parent is re-listed, so a file opened directly (recent
        // files, an app reopening its document) could serve stale cached
        // bytes forever after another machine edits it. TTL-gated NAS
        // stat closes that hole.
        //
        // Two guards keep this from fighting our own writes:
        //  * Recently-written files are skipped outright — during an
        //    active copy the SMB attribute cache lags our own writes and
        //    a stat would read as spurious drift, nuking the bitmap of a
        //    half-copied file.
        //  * Same-size drift with a small mtime delta is a self-write
        //    echo (we stamp nas_mtime with LOCAL time on write; the NAS
        //    stamps its own clock) — adopt the server values instead of
        //    invalidating, or every copied file would fully re-hydrate
        //    on its first read a minute later.
        const FRESHNESS_TTL_SECS: f64 = 5.0;
        const RECENT_WRITE_SKIP_SECS: f64 = 60.0;
        const SELF_WRITE_ECHO_SECS: f64 = 30.0;
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs_f64())
            .unwrap_or(0.0);
        if self.health.is_online() && now - attr.mtime > RECENT_WRITE_SKIP_SECS {
            let cache = self.cache.clone();
            let rel_owned = rel.clone();
            let abs = self.absolute(&rel);
            let stat = tokio::task::spawn_blocking(move || {
                macos_cache::stat_and_refresh(
                    &cache,
                    &rel_owned,
                    &abs,
                    FRESHNESS_TTL_SECS,
                )
            })
            .await
            .unwrap_or(macos_cache::StatResult::Skipped);
            if let macos_cache::StatResult::Drifted { size, mtime } = stat {
                let self_echo = size == attr.size
                    && (mtime - attr.mtime).abs() < SELF_WRITE_ECHO_SECS;
                if !self_echo {
                    log::info!(
                        "[nfs-server] {}: {} changed on NAS (size {}→{}) — \
                         dropping cached bytes",
                        self.domain, rel, attr.size, size
                    );
                    let lock = self.cache.fh_lock(id);
                    if let Ok(_g) = lock.try_write() {
                        self.cache.invalidate_cache(&rel, id);
                    }
                    // Re-read: size/mtime were refreshed by the stat and
                    // hydration state by the invalidate.
                    attr = self
                        .cache
                        .cached_attr(&rel)
                        .ok_or(nfsstat3::NFS3ERR_NOENT)?;
                }
            }
        }
        let size = attr.size;

        // Clamp request to file size — NFS convention.
        let read_end = (offset + count as u64).min(size);
        if offset >= size {
            return Ok((Vec::new(), true));
        }
        let read_len = (read_end - offset) as usize;

        // Hold a read guard on this fh for the duration of the cache touch.
        // The eviction worker takes a non-blocking `try_write`, so it skips
        // any fh with an active reader rather than tearing the file out from
        // under us.
        let fh_lock = self.cache.fh_lock(id);
        let _read_guard = fh_lock.read().await;

        // Fast path: fully hydrated. Slice E: bump last_accessed so
        // hot files stay hot in the eviction LRU. Pre-refactor only
        // the chunk-bitmap path touched it, so frequently-read fully-
        // hydrated files looked coldest and got evicted first.
        //
        // Trust-but-verify: is_hydrated is a DB claim about a separate
        // file on disk. If the blob is missing or short (evicted in the
        // attr-snapshot window, mirror gap, layout migration), erroring
        // or serving a short read here surfaces phantom I/O errors /
        // zeros for a file that is perfectly fine on SMB. Demote to the
        // bitmap path instead — it re-hydrates from SMB.
        if attr.is_hydrated {
            self.cache.touch(&rel);
            match self.read_from_cache(id, offset, read_len, size) {
                Ok((buf, eof)) if buf.len() == read_len => {
                    return Ok((buf, eof));
                }
                Ok((buf, _)) => {
                    log::warn!(
                        "[nfs-server] {}: hydrated blob for {} short ({} < {}) — \
                         demoting to SMB re-hydrate",
                        self.domain, rel, buf.len(), read_len
                    );
                }
                Err(e) => {
                    log::warn!(
                        "[nfs-server] {}: hydrated blob for {} unreadable ({:?}) — \
                         demoting to SMB re-hydrate",
                        self.domain, rel, e
                    );
                }
            }
            // The hydrated flag lied. Invalidate (also deletes the blob,
            // keeping the sparse zero-skip invariant: cache files never
            // hold stale non-zero bytes) and fall through to the bitmap
            // path with a fresh empty bitmap.
            self.cache.invalidate_cache(&rel, id);
        }

        // Chunk-level path: walk the requested range chunk-by-chunk, serving
        // each from cache if the bit is set, otherwise from SMB (and writing
        // to cache as we go).
        let bitmap = self.cache.get_chunk_bitmap(&rel);
        self.read_with_bitmap(id, &rel, &attr, offset, read_len, bitmap).await
    }

    async fn write(&self, id: fileid3, offset: u64, data: &[u8]) -> Result<fattr3, nfsstat3> {
        // Legacy entry point — Slice F dispatches via write_with_stable
        // now, but the trait still requires write(). Delegate to the
        // FILE_SYNC variant so anyone calling write() directly gets
        // the same sync-on-every-write semantics.
        let (attr, _) = self
            .write_with_stable(
                id,
                offset,
                data,
                nfsserve::nfs_handlers::stable_how::FILE_SYNC,
            )
            .await?;
        Ok(attr)
    }

    /// Slice F: honor `stable_how`. UNSTABLE writes skip the per-WRITE
    /// `sync_all()` over SMB — the macOS NFS client will follow up
    /// with COMMIT before declaring data safe. Eliminates the per-
    /// chunk fsync that dominated `write` latency in upstream nfsserve.
    async fn write_with_stable(
        &self,
        id: fileid3,
        offset: u64,
        data: &[u8],
        stable: nfsserve::nfs_handlers::stable_how,
    ) -> Result<(fattr3, nfsserve::nfs_handlers::stable_how), nfsstat3> {
        self.require_online()?;
        let rel = self.rel_path(id)?;
        if rel.is_empty() {
            return Err(nfsstat3::NFS3ERR_ISDIR);
        }
        let abs = self.absolute(&rel);

        // Slice E: take the per-fh write lock for the duration of the
        // SMB write + cache invalidation. Pre-refactor the write path
        // skipped the lock entirely, so a concurrent read could hold
        // the read guard, see is_hydrated=true, then get ENOENT mid-
        // stream when invalidate_cache deleted the cache file under it.
        let fh_lock = self.cache.fh_lock(id);
        let _write_guard = fh_lock.write().await;

        let want_sync = !matches!(
            stable,
            nfsserve::nfs_handlers::stable_how::UNSTABLE
        );

        // Snapshot pre-write attrs ahead of any I/O — we need them to
        // know whether the file was already hydrated, what its prior
        // size was, and the original creation timestamp.
        let prior_attr = self.cache.cached_attr(&rel);
        let prev_size = prior_attr.as_ref().map(|a| a.size).unwrap_or(0);
        let was_hydrated = prior_attr.as_ref().map(|a| a.is_hydrated).unwrap_or(false);
        let new_size = prev_size.max(offset + data.len() as u64);

        // Slice H: write-through to the local cache file at the same
        // offset. Pre-Slice-H the write path called invalidate_cache
        // which deleted the cache file outright, forcing every read-
        // after-write to re-fetch the same bytes from SMB. For a media
        // workflow (save big render → scrub it back) that doubled SMB
        // traffic. Now SMB stays authoritative, but the local cache
        // gets the bytes too and the bitmap learns about fully-
        // covered chunks. Partial leading/trailing slivers leave their
        // bitmap bit clear — next read picks up the whole chunk from
        // SMB to keep the chunk-aligned invariant honest.
        let cache_path = self.cache.cache_file_path(id);

        let abs_for_io = abs.clone();
        let cache_path_for_io = cache_path.clone();
        let data_owned = data.to_vec();
        let data_for_cache = data_owned.clone();
        // Returns whether the cache mirror actually landed. A failed
        // mirror is NOT ignorable state: for a hydrated file the read
        // fast path never consults SMB again, so silently dropping the
        // bytes would serve the pre-write content forever.
        let write_result: Result<bool, nfsstat3> = tokio::task::spawn_blocking(move || {
            // ── SMB write (authoritative) ─────────────────────────
            let f = std::fs::OpenOptions::new()
                .write(true)
                .create(false)
                .truncate(false)
                .open(&abs_for_io)
                .map_err(io_to_nfsstat)?;
            // write_all_at, not write_at: pwrite over SMB can return
            // short, and ignoring the count silently drops the tail of
            // the client's WRITE while reporting success.
            f.write_all_at(&data_owned, offset).map_err(io_to_nfsstat)?;
            if want_sync {
                f.sync_all().map_err(io_to_nfsstat)?;
            }

            // ── Cache mirror ──────────────────────────────────────
            // Failures never propagate to the NFS client (SMB is the
            // source of truth) but MUST be reported to the caller so
            // it can invalidate rather than let the cache lie.
            let cache_file = match std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .truncate(false)
                .open(&cache_path_for_io)
            {
                Ok(f) => f,
                Err(e) => {
                    log::warn!(
                        "[nfs-server] write-through cache open failed for fh \
                         (path {:?}): {} — SMB write ok, cache will be invalidated",
                        cache_path_for_io, e
                    );
                    return Ok(false);
                }
            };
            // Extend sparse to at least new_size so writes past prior
            // EOF land at the right offset.
            if cache_file.metadata().map(|m| m.len()).unwrap_or(0) < new_size {
                let _ = cache_file.set_len(new_size);
            }
            if let Err(e) = cache_file.write_all_at(&data_for_cache, offset) {
                log::warn!(
                    "[nfs-server] write-through cache pwrite failed at offset \
                     {}: {} — SMB write ok, cache will be invalidated",
                    offset, e
                );
                return Ok(false);
            }
            Ok(true)
        })
        .await
        .map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let mirror_ok = write_result?;

        if !mirror_ok && was_hydrated {
            // The cache file now diverges from SMB at this offset and
            // the hydrated fast path would serve the stale bytes
            // forever. Drop the cache entry; the next read re-hydrates
            // from SMB. (We hold the per-fh write guard, so no reader
            // is mid-flight on this fh.)
            self.cache.invalidate_cache(&rel, id);
        }

        // Update the chunk bitmap for chunks the write fully covered.
        // If the file was already fully hydrated, leave the bitmap
        // alone (mark_fully_hydrated nulls it; flipping bits would
        // invent a partial state where there isn't one — the cache
        // file just got the new bytes overlaid and is still complete).
        // Only when the mirror landed — setting bits for bytes the
        // cache file doesn't hold would serve zeros in their place.
        if mirror_ok && !was_hydrated {
            let mut bitmap = self.cache.get_chunk_bitmap(&rel);
            let write_end = offset + data.len() as u64;
            let first_chunk = offset / CHUNK_SIZE;
            let last_chunk_inclusive =
                (offset + data.len() as u64 - 1) / CHUNK_SIZE;
            for chunk in first_chunk..=last_chunk_inclusive {
                let chunk_start = chunk * CHUNK_SIZE;
                let chunk_end = ((chunk + 1) * CHUNK_SIZE).min(new_size);
                // Fully-covered iff the write spans the chunk's whole
                // range. Partial leading/trailing slivers stay 0; the
                // next read fetches the whole chunk from SMB.
                if offset <= chunk_start && write_end >= chunk_end {
                    macos_cache::set_bit(&mut bitmap, chunk);
                }
            }
            let total_chunks = macos_cache::num_chunks(new_size);
            if total_chunks > 0
                && macos_cache::bitmap_is_complete(&bitmap, total_chunks)
            {
                // Bitmap just completed via the write — promote to
                // fully hydrated. Common path for full-file overwrites
                // of small-to-medium files.
                self.cache.mark_fully_hydrated(&rel, new_size);
                log::info!(
                    "[nfs-server] {}: fully hydrated via write-through {}",
                    self.domain, rel
                );
            } else {
                self.cache.update_chunk_bitmap(&rel, &bitmap);
            }
        }

        // Slice E: compute new size/mtime locally from the write we
        // just did, rather than re-stat'ing SMB. macOS's SMB client
        // caches positive attribute lookups for ~10s; the post-write
        // stat could return PRE-write metadata, leaving the cache's
        // nas_size stale until drift detection kicks in.
        let mtime = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs_f64())
            .unwrap_or(0.0);
        self.cache.update_nas_metadata(&rel, new_size, mtime);

        let created = prior_attr.as_ref().map(|a| a.created).unwrap_or(mtime);
        // Re-read is_hydrated since mark_fully_hydrated may have
        // flipped it (or not). cached_attr is a single indexed SELECT,
        // cheap.
        let post_hydrated = self
            .cache
            .cached_attr(&rel)
            .map(|a| a.is_hydrated)
            .unwrap_or(false);
        let post_hydrated_size = self
            .cache
            .cached_attr(&rel)
            .map(|a| a.hydrated_size)
            .unwrap_or(0);
        let attr = CachedAttr {
            is_dir: false,
            size: new_size,
            mtime,
            created,
            is_hydrated: post_hydrated,
            hydrated_size: post_hydrated_size,
        };
        // Echo back the stable_how we actually achieved. If the
        // client asked UNSTABLE and we skipped the fsync, return
        // UNSTABLE so they know to follow with COMMIT. Anything
        // else (DATA_SYNC, FILE_SYNC) was fsync'd — say FILE_SYNC.
        let achieved = if !want_sync {
            nfsserve::nfs_handlers::stable_how::UNSTABLE
        } else {
            nfsserve::nfs_handlers::stable_how::FILE_SYNC
        };
        Ok((attr_from_cache(id, &attr), achieved))
    }

    /// Slice F: NFSPROC3_COMMIT — fsync the requested byte range on the
    /// SMB-backed file. The macOS NFS client sends this after a batch
    /// of UNSTABLE writes; returning Ok(verf) tells the client its
    /// data is on stable storage and the writes don't need to be
    /// re-sent. count=0 means "whole file from offset".
    async fn commit(
        &self,
        id: fileid3,
        _offset: u64,
        _count: u32,
    ) -> Result<nfsserve::nfs::writeverf3, nfsstat3> {
        self.require_online()?;
        let rel = self.rel_path(id)?;
        if rel.is_empty() {
            return Err(nfsstat3::NFS3ERR_ISDIR);
        }
        let abs = self.absolute(&rel);

        // Per-fh write lock so we don't race a concurrent
        // write_with_stable that's also opening + writing the file.
        let fh_lock = self.cache.fh_lock(id);
        let _guard = fh_lock.write().await;

        // SMB doesn't expose a range-fsync primitive — sync_all is the
        // hammer. Offset/count are advisory; flushing the whole file
        // satisfies the protocol contract.
        let sync_result: Result<(), nfsstat3> = tokio::task::spawn_blocking(move || {
            let f = std::fs::OpenOptions::new()
                .write(true)
                .open(&abs)
                .map_err(io_to_nfsstat)?;
            f.sync_all().map_err(io_to_nfsstat)?;
            Ok(())
        })
        .await
        .map_err(|_| nfsstat3::NFS3ERR_IO)?;
        sync_result?;

        Ok(self.serverid())
    }

    async fn create(
        &self,
        dirid: fileid3,
        filename: &filename3,
        attr: sattr3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        self.require_online()?;
        let parent_rel = self.rel_path(dirid)?;
        let name = filename_to_string(filename)?;
        let child_rel = join_rel(&parent_rel, &name);
        let abs = self.absolute(&child_rel);

        // Slice E: SMB create wrapped in spawn_blocking. The SMB result
        // MUST gate registration — registering a file whose SMB create
        // failed leaves a phantom row, and every subsequent WRITE to it
        // ENOENTs (the write path opens with create(false)).
        let abs_for_io = abs.clone();
        let attr_size = attr.size;
        let create_result: Result<(), nfsstat3> = tokio::task::spawn_blocking(move || {
            // CREATE (UNCHECKED) — overwrite is fine.
            let f = std::fs::OpenOptions::new()
                .write(true)
                .create(true)
                .truncate(true)
                .open(&abs_for_io)
                .map_err(io_to_nfsstat)?;
            if let set_size3::size(new_size) = attr_size {
                if new_size > 0 {
                    f.set_len(new_size).map_err(io_to_nfsstat)?;
                }
            }
            Ok(())
        })
        .await
        .map_err(|_| nfsstat3::NFS3ERR_IO)?;
        create_result?;

        self.register_new_entry(&child_rel, &name, &abs).await
    }

    async fn create_exclusive(
        &self,
        dirid: fileid3,
        filename: &filename3,
    ) -> Result<fileid3, nfsstat3> {
        self.require_online()?;
        let parent_rel = self.rel_path(dirid)?;
        let name = filename_to_string(filename)?;
        let child_rel = join_rel(&parent_rel, &name);
        let abs = self.absolute(&child_rel);

        // Slice E: SMB create_new wrapped in spawn_blocking.
        let abs_for_io = abs.clone();
        let create_result: Result<(), nfsstat3> =
            tokio::task::spawn_blocking(move || {
                std::fs::OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .open(&abs_for_io)
                    .map(|_| ())
                    .map_err(io_to_nfsstat)
            })
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)?;
        create_result?;

        let (fh, _attr) = self.register_new_entry(&child_rel, &name, &abs).await?;
        Ok(fh)
    }

    async fn mkdir(
        &self,
        dirid: fileid3,
        dirname: &filename3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        self.require_online()?;
        let name_preview = filename_to_string(dirname).unwrap_or_default();
        log::debug!(
            "[nfs-server] {}: mkdir(dirid={}, name={:?})",
            self.domain, dirid, name_preview
        );
        let parent_rel = self.rel_path(dirid)?;
        let name = filename_to_string(dirname)?;
        let child_rel = join_rel(&parent_rel, &name);
        let abs = self.absolute(&child_rel);

        // Slice E: SMB mkdir wrapped in spawn_blocking.
        let abs_for_io = abs.clone();
        let mkdir_result: Result<(), nfsstat3> =
            tokio::task::spawn_blocking(move || {
                std::fs::create_dir(&abs_for_io).map_err(io_to_nfsstat)
            })
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)?;
        mkdir_result?;

        self.register_new_entry(&child_rel, &name, &abs).await
    }

    async fn remove(&self, dirid: fileid3, filename: &filename3) -> Result<(), nfsstat3> {
        // nfsserve routes both NFSPROC3_REMOVE and NFSPROC3_RMDIR to this
        // single trait method. We dispatch to remove_file vs remove_dir
        // based on the target's actual type on disk.
        self.require_online()?;
        let parent_rel = self.rel_path(dirid)?;
        let name = filename_to_string(filename)?;
        let child_rel = join_rel(&parent_rel, &name);
        let abs = self.absolute(&child_rel);

        // Snapshot the fh before deletion so we can scrub the cache file.
        let fh = self.cache.fh_for_path(&child_rel).unwrap_or(0);

        // Slice E: stat + remove wrapped in spawn_blocking. The whole
        // delete block runs in one blocking task — the
        // sweep_hidden_children_then_remove helper does its own SMB
        // I/O so it stays inside the same spawn.
        let abs_for_io = abs.clone();
        let removed_kind: Result<bool, nfsstat3> =
            tokio::task::spawn_blocking(move || {
                let meta =
                    std::fs::symlink_metadata(&abs_for_io).map_err(io_to_nfsstat)?;
                let is_dir = meta.is_dir();
                if is_dir {
                    // NFS3 RMDIR fails if the directory isn't empty.
                    // First try the strict path. If it fails because
                    // hidden junk (`.DS_Store`, `@eaDir`, `#recycle`)
                    // is still present, sweep + retry. User-visible
                    // entries still present → honest NOTEMPTY.
                    match std::fs::remove_dir(&abs_for_io) {
                        Ok(()) => Ok(true),
                        Err(e) if is_dir_not_empty(&e) => {
                            sweep_hidden_children_then_remove(&abs_for_io)
                                .map(|_| true)
                        }
                        Err(e) => Err(io_to_nfsstat(e)),
                    }
                } else {
                    std::fs::remove_file(&abs_for_io)
                        .map(|_| false)
                        .map_err(io_to_nfsstat)
                }
            })
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let is_dir = removed_kind?;

        if fh != 0 {
            self.cache.forget_path(&child_rel, fh);
        }

        log::debug!(
            "[nfs-server] {}: removed {} {} (fh={})",
            self.domain,
            if is_dir { "dir" } else { "file" },
            child_rel,
            fh
        );
        Ok(())
    }

    async fn rename(
        &self,
        from_dirid: fileid3,
        from_filename: &filename3,
        to_dirid: fileid3,
        to_filename: &filename3,
    ) -> Result<(), nfsstat3> {
        self.require_online()?;
        let from_parent = self.rel_path(from_dirid)?;
        let to_parent = self.rel_path(to_dirid)?;
        let from_name = filename_to_string(from_filename)?;
        let to_name = filename_to_string(to_filename)?;
        let from_rel = join_rel(&from_parent, &from_name);
        let to_rel = join_rel(&to_parent, &to_name);
        let from_abs = self.absolute(&from_rel);
        let to_abs = self.absolute(&to_rel);

        // Same-path rename is a no-op (some clients do this while editors
        // re-save files); short-circuit before hitting the NAS.
        if from_rel == to_rel {
            return Ok(());
        }

        // Slice E: SMB rename wrapped in spawn_blocking. Atomic on
        // same filesystem; SMB handles cross-dir moves. Fails if
        // renaming over a non-empty directory (→ ENOTEMPTY), etc.
        let from_for_io = from_abs.clone();
        let to_for_io = to_abs.clone();
        let rename_result: Result<(), nfsstat3> =
            tokio::task::spawn_blocking(move || {
                std::fs::rename(&from_for_io, &to_for_io).map_err(io_to_nfsstat)
            })
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)?;
        rename_result?;

        // Preserve the source fh so cached client handles keep resolving.
        if let Err(e) = self.cache.rename_path(&from_rel, &to_rel) {
            log::error!(
                "[nfs-server] {}: cache rename_path failed: {} (disk rename succeeded)",
                self.domain, e
            );
            return Err(nfsstat3::NFS3ERR_IO);
        }

        log::debug!(
            "[nfs-server] {}: renamed {} → {}",
            self.domain, from_rel, to_rel
        );
        Ok(())
    }

    async fn readdir(
        &self,
        dirid: fileid3,
        start_after: fileid3,
        max_entries: usize,
    ) -> Result<ReadDirResult, nfsstat3> {
        let parent_rel = self.rel_path(dirid)?;

        // Only check for drift on the first page of an enumeration.
        // Mid-pagination re-enumeration would change which entries
        // appear past `start_after` and confuse clients.
        //
        // Offline gate: every other SMB-touching op fast-fails via
        // require_online(); without it here, browsing any folder while
        // the NAS is unreachable blocks Finder on a ~60s SMB timeout
        // per folder. Offline we serve whatever is cached instead.
        if start_after == 0 && self.health.is_online() {
            // Live SMB folder mtime — bumped by the NAS when another
            // machine adds/removes a child. Compared against
            // visited_folders.folder_mtime to detect out-of-band changes.
            // First read of an unvisited folder hits the cold path
            // (cached_folder_mtime returns None → needs_reenum=true).
            let abs = self.absolute(&parent_rel);
            let live_mtime: f64 = tokio::task::spawn_blocking(move || {
                std::fs::metadata(&abs)
                    .ok()
                    .and_then(|m| m.modified().ok())
                    .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                    .map(|d| d.as_secs_f64())
                    .unwrap_or(0.0)
            })
            .await
            .unwrap_or(0.0);
            let cached = self.cache.cached_folder_mtime(&parent_rel);
            let needs = self.cache.folder_needs_reenum(&parent_rel, live_mtime);
            log::info!(
                "[nfs-server] {}: readdir({}) cached_mtime={:?} live_mtime={:.0} \
                 needs_reenum={}",
                self.domain, parent_rel, cached, live_mtime, needs
            );
            if needs {
                self.populate_folder(&parent_rel).await?;
            }
        }

        // Sort by fh — the SAME key the resume cookie uses. The previous
        // name-sort + exact-fh-match resume had two pagination bugs on
        // big folders: (a) if the cursor entry was deleted between pages
        // the match never fired and the rest of the listing silently
        // vanished; (b) a rename could move the cursor's sort position
        // and re-emit or skip entries. With fh-ordered entries and a
        // strict `fh > cursor` resume, both degrade gracefully. Clients
        // do their own display sorting; NFS imposes no server order.
        let mut children = self.cache.cached_children(&parent_rel);
        children.sort_by_key(|(fh, _, _)| *fh);

        let mut result = ReadDirResult::default();

        for (fh, name, attr) in children {
            if fh <= start_after {
                continue;
            }
            if result.entries.len() >= max_entries {
                return Ok(result);
            }
            let attr3 = attr_from_cache(fh, &attr);
            result.entries.push(DirEntry {
                fileid: fh,
                name: name.into_bytes().into(),
                attr: attr3,
            });
        }
        result.end = true;
        Ok(result)
    }

    async fn symlink(
        &self,
        _dirid: fileid3,
        _linkname: &filename3,
        _symlink: &nfspath3,
        _attr: &sattr3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        Err(nfsstat3::NFS3ERR_ROFS)
    }

    async fn readlink(&self, id: fileid3) -> Result<nfspath3, nfsstat3> {
        self.require_online()?;
        let rel = self.rel_path(id)?;
        let abs = self.absolute(&rel);
        // Slice E: SMB readlink wrapped in spawn_blocking.
        let target = tokio::task::spawn_blocking(move || std::fs::read_link(&abs))
            .await
            .map_err(|_| nfsstat3::NFS3ERR_IO)?
            .map_err(io_to_nfsstat)?;
        use std::os::unix::ffi::OsStrExt;
        let bytes: Vec<u8> = target.as_os_str().as_bytes().to_vec();
        Ok(bytes.into())
    }

    /// Single-export server — the client's mount path is ignored; we always
    /// hand back root. Without this override the default impl walks path
    /// components via lookup(), so `localhost:/anything` would ENOENT.
    async fn path_to_id(&self, _path: &[u8]) -> Result<fileid3, nfsstat3> {
        Ok(self.root_dir())
    }

    /// Persistent NFS file handle encoding. The default `nfsserve` impl
    /// prefixes with a generation number derived from server startup time,
    /// which invalidates every client-cached handle on agent restart. Our
    /// `fh` comes from `nfs_handles.fh` (SQLite AUTOINCREMENT — never reused)
    /// and persists across restarts, so we use a fixed zero generation:
    /// handles survive agent bounces without `umount`/`mount`.
    fn id_to_fh(&self, id: fileid3) -> nfsserve::nfs::nfs_fh3 {
        let mut data = Vec::with_capacity(16);
        data.extend_from_slice(&[0u8; 8]);
        data.extend_from_slice(&id.to_le_bytes());
        nfsserve::nfs::nfs_fh3 { data }
    }

    fn fh_to_id(&self, fh: &nfsserve::nfs::nfs_fh3) -> Result<fileid3, nfsstat3> {
        if fh.data.len() != 16 {
            return Err(nfsstat3::NFS3ERR_BADHANDLE);
        }
        let bytes: [u8; 8] = fh.data[8..16]
            .try_into()
            .map_err(|_| nfsstat3::NFS3ERR_BADHANDLE)?;
        Ok(u64::from_le_bytes(bytes))
    }
}

/// Decode an NFS filename and NFC-normalize it. The macOS NFS client
/// sends NFD-decomposed names while SMB enumerations return the NAS's
/// stored form; the cache key space is all-NFC (see
/// `macos_cache::nfc`), so both sides funnel through the same form and
/// one file has exactly one key. Opening the NFC form over SMB works
/// regardless of the on-disk form — SMB servers match names
/// case/normalization-insensitively.
fn filename_to_string(f: &filename3) -> Result<String, nfsstat3> {
    std::str::from_utf8(f.0.as_ref())
        .map(macos_cache::nfc)
        .map_err(|_| nfsstat3::NFS3ERR_IO)
}

fn join_rel(parent: &str, child: &str) -> String {
    if parent.is_empty() {
        child.to_string()
    } else {
        format!("{}/{}", parent, child)
    }
}

/// Start one NFS server bound to `127.0.0.1:<port>`, serving `nas_root` as
/// the export root and using `cache` as the metadata authority. Spawns a
/// background tokio task; returns immediately. If bind or handshake fails,
/// logs the error and the task exits.
///
/// Returns a `SyncServerHandle`. Calling `shutdown_and_wait()` on it fires
/// the shutdown signal; the task unmounts the NFS loopback, drops the
/// listener, and exits. Dropping the handle alone does NOT shut the server
/// down — the signal must be sent explicitly (matches Windows behavior).
pub fn start(
    domain: String,
    nas_root: PathBuf,
    port: u16,
    cache: Arc<MacosCache>,
    ipc_tx: mpsc::Sender<AgentToUfb>,
    health: Arc<NasHealth>,
) -> crate::sync::SyncServerHandle {
    let (shutdown_tx, mut shutdown_rx) = tokio::sync::oneshot::channel::<()>();
    // Child shutdown for the evictor — separate oneshot so we can signal
    // it from inside the server task's cleanup path.
    let (evict_shutdown_tx, mut evict_shutdown_rx) = tokio::sync::oneshot::channel::<()>();

    let domain_for_task = domain.clone();
    let task_handle = tokio::spawn(async move {
        let domain = domain_for_task;
        // Slice D: NasHealth is now passed in by the orchestrator;
        // there's no internal probe loop spawn (eliminates the
        // orphan-probe-loop leak across respawns).

        // Cache eviction tick runs independently of the NFS listener. One
        // task per cache instance; 30s cadence trades bounded-over-budget
        // latency for low idle wakeups. Clone `cache` here before it's moved
        // into `PassthroughFs::new` below.
        if cache.cache_limit() > 0 {
            let cache_for_evict = Arc::clone(&cache);
            let domain_for_evict = domain.clone();
            tokio::spawn(async move {
                let mut tick = tokio::time::interval(Duration::from_secs(30));
                // First tick fires immediately — skip it so we don't race
                // against the initial mount-up window.
                tick.tick().await;
                loop {
                    tokio::select! {
                        _ = tick.tick() => {
                            let (files, bytes) = cache_for_evict.evict_over_budget_now().await;
                            if files > 0 {
                                log::debug!(
                                    "[nfs-server] {} evictor freed {} files / {} bytes",
                                    domain_for_evict, files, bytes,
                                );
                            }
                        }
                        _ = &mut evict_shutdown_rx => {
                            log::debug!("[nfs-server] {} evictor exiting", domain_for_evict);
                            break;
                        }
                    }
                }
            });
        }

        let fs = match PassthroughFs::new(
            domain.clone(),
            nas_root.clone(),
            cache,
            ipc_tx,
            health,
        ) {
            Ok(fs) => fs,
            Err(e) => {
                log::error!(
                    "[nfs-server] {} failed to create passthrough fs for {}: {}",
                    domain,
                    nas_root.display(),
                    e
                );
                let _ = evict_shutdown_tx.send(());
                return;
            }
        };

        let bind = format!("127.0.0.1:{}", port);
        let listener = match NFSTcpListener::bind(&bind, fs).await {
            Ok(l) => l,
            Err(e) => {
                log::error!("[nfs-server] {} failed to bind {}: {}", domain, bind, e);
                let _ = evict_shutdown_tx.send(());
                return;
            }
        };

        log::info!(
            "[nfs-server] {} listening on {} (nas_root={})",
            domain,
            bind,
            nas_root.display(),
        );

        // Auto-mount runs in a separate blocking task. It MUST run
        // concurrently with `handle_forever` below — `mount_nfs` performs
        // an NFS handshake over localhost, so it blocks until our server
        // accepts the connection. Running it inline would deadlock
        // (handle_forever can't start until mount_nfs returns; mount_nfs
        // can't return until handle_forever accepts it).
        let domain_for_mount = domain.clone();
        tokio::task::spawn_blocking(move || {
            let mount_point = mount_point_for(&domain_for_mount);
            match mount_nfs_share(&domain_for_mount, port, &mount_point) {
                Ok(()) => log::info!(
                    "[nfs-server] {} auto-mounted at {}",
                    domain_for_mount,
                    mount_point.display()
                ),
                Err(e) => log::warn!(
                    "[nfs-server] {} auto-mount failed ({}) — mount manually with: \
                     mount -t nfs -o \"port={p},mountport={p},nolocks,vers=3,tcp,nobrowse,actimeo=1,rsize=1048576,wsize=1048576\" \
                     localhost:/{} {}",
                    domain_for_mount,
                    e,
                    domain_for_mount,
                    mount_point.display(),
                    p = port,
                ),
            }
        });

        // Run the NFS listener under a shutdown watch. `handle_forever`
        // returns only on fatal error, so the select lets us cleanly
        // unwind on cache-root change / agent quit.
        tokio::select! {
            res = listener.handle_forever() => {
                if let Err(e) = res {
                    log::error!("[nfs-server] {} handle_forever exited: {}", domain, e);
                }
            }
            _ = &mut shutdown_rx => {
                log::info!("[nfs-server] {} shutdown requested — unmounting", domain);
            }
        }

        // Unmount the NFS loopback point. Blocking call — use spawn_blocking
        // so we don't stall the tokio worker while umount(8) waits for the
        // kernel to release any in-flight references.
        let mount_point = mount_point_for(&domain);
        let domain_for_umount = domain.clone();
        let _ = tokio::task::spawn_blocking(move || {
            if is_mounted(&mount_point) {
                let _ = std::process::Command::new("umount").arg(&mount_point).status();
                if is_mounted(&mount_point) {
                    let _ = std::process::Command::new("umount")
                        .arg("-f")
                        .arg(&mount_point)
                        .status();
                }
                log::info!(
                    "[nfs-server] {} unmounted {}",
                    domain_for_umount,
                    mount_point.display()
                );
            }
        })
        .await;

        // Signal the evictor so it drops its cache Arc clone. If the evictor
        // never started (cache_limit == 0) this is a no-op.
        let _ = evict_shutdown_tx.send(());

        log::info!("[nfs-server] {} stopped", domain);
    });

    crate::sync::SyncServerHandle::new_macos(domain, shutdown_tx, task_handle)
}
