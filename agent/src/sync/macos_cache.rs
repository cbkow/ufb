/// SQLite cache for macOS NFS-loopback VFS backend.
///
/// Tracks known files, visited folders, hydration state, and chunk-level
/// content bitmaps. The NFS server reads from this cache for metadata
/// and content, falling back to live SMB on cold paths.

use crate::sync::cache_core::{self, SqliteConn, SqlitePool};
pub use crate::sync::cache_core::{
    bit_is_set, bitmap_is_complete, num_chunks, parent_of, set_bit, CachedAttr, CHUNK_SIZE,
    EVICTION_TARGET_PERCENT,
};
use rusqlite::{params, Connection};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

/// Per-domain cache database.
pub struct MacosCache {
    pool: SqlitePool,
    nas_root: PathBuf,
    cache_limit: u64,
    /// Root directory for this cache instance — DB files live directly
    /// under this path, block-level content blobs under `{cache_dir}/by_handle/`.
    /// User-configurable via `syncCacheRoot` in `mounts.json`; defaults to
    /// `MountConfig::default_cache_root()`.
    cache_dir: PathBuf,
    /// Domain/share name this cache belongs to — used when emitting
    /// BadgeUpdate messages so the FinderSync extension can scope badges
    /// to a specific share.
    domain: String,
    /// Per-file-handle read/write lock registry. Readers (NFS `read()`) take a
    /// read guard for the duration of the call; the eviction worker takes a
    /// non-blocking write guard, skipping any fh whose read is in flight.
    /// Sparse — entries are created lazily on first access.
    per_fh_locks: Mutex<HashMap<u64, Arc<tokio::sync::RwLock<()>>>>,
    /// Optional agent→UFB channel used to broadcast BadgeUpdate messages
    /// on hydration state changes. Set via `set_badge_tx` after
    /// construction (during agent wiring). None on Windows / tests.
    badge_tx: Mutex<Option<tokio::sync::mpsc::Sender<crate::messages::AgentToUfb>>>,
}

impl MacosCache {
    /// Open or create the cache DB for a domain.
    ///
    /// `cache_dir` is the effective cache root (`MountsConfig::cache_root()`) —
    /// the DB lives at `{cache_dir}/{domain}.db`, and block-level content
    /// blobs under `{cache_dir}/by_handle/`.
    pub fn open(
        domain: &str,
        nas_root: PathBuf,
        cache_limit: u64,
        cache_dir: &Path,
    ) -> Result<Self, String> {
        std::fs::create_dir_all(cache_dir)
            .map_err(|e| format!("Failed to create cache dir: {}", e))?;

        let db_path = cache_dir.join(format!("{}.db", domain));
        log::info!("[macos-cache] Opening DB at {}", db_path.display());

        // Defensive pre-touch — see `cache_core::ensure_db_file`. The
        // 8.3-short-path bug it works around is Windows-specific, but
        // running this on macOS is a harmless no-op (the file usually
        // already exists from a prior run, and creating an empty file
        // is fine on every supported FS).
        cache_core::ensure_db_file(&db_path);

        // One-time setup: enable WAL, create tables, apply migrations, create indexes.
        // Must happen on a single serial connection before the pool opens, because
        // ALTER TABLE concurrency is not safe to race.
        {
            let mut conn = Connection::open(&db_path)
                .map_err(|e| format!("Failed to open cache DB: {}", e))?;

            conn.execute_batch(cache_core::INIT_PRAGMAS)
                .map_err(|e| format!("Failed to set pragmas: {}", e))?;

            conn.execute_batch(
                "CREATE TABLE IF NOT EXISTS known_files (
                    fh INTEGER PRIMARY KEY AUTOINCREMENT,
                    path TEXT NOT NULL UNIQUE COLLATE NOCASE,
                    name TEXT NOT NULL,
                    is_dir INTEGER NOT NULL DEFAULT 0,
                    nas_size INTEGER NOT NULL,
                    nas_mtime REAL NOT NULL,
                    nas_created REAL NOT NULL DEFAULT 0,
                    is_hydrated INTEGER NOT NULL DEFAULT 0,
                    hydrated_size INTEGER DEFAULT 0,
                    last_accessed REAL DEFAULT 0,
                    last_verified_at REAL DEFAULT 0,
                    parent_path TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
                    chunk_bitmap BLOB DEFAULT NULL
                );

                CREATE TABLE IF NOT EXISTS visited_folders (
                    path TEXT PRIMARY KEY COLLATE NOCASE,
                    folder_mtime REAL NOT NULL DEFAULT 0
                );

                CREATE TABLE IF NOT EXISTS metadata (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );",
            )
            .map_err(|e| format!("Failed to create schema: {}", e))?;

            // Migrate: add hydration columns if missing
            let has_hydrated: bool = conn
                .prepare("SELECT is_hydrated FROM known_files LIMIT 0")
                .is_ok();
            if !has_hydrated {
                log::info!("[macos-cache] Migrating: adding hydration columns");
                let _ = conn.execute_batch(
                    "ALTER TABLE known_files ADD COLUMN is_hydrated INTEGER NOT NULL DEFAULT 0;
                     ALTER TABLE known_files ADD COLUMN hydrated_size INTEGER DEFAULT 0;
                     ALTER TABLE known_files ADD COLUMN last_accessed REAL DEFAULT 0;",
                );
            }

            let has_verified: bool = conn
                .prepare("SELECT last_verified_at FROM known_files LIMIT 0")
                .is_ok();
            if !has_verified {
                log::info!("[macos-cache] Migrating: adding last_verified_at column");
                let _ = conn.execute_batch(
                    "ALTER TABLE known_files ADD COLUMN last_verified_at REAL DEFAULT 0;",
                );
            }

            // Wave 3.2: add parent_path column (the directory containing the entry)
            // so orphan / enumeration queries can use an indexed equality lookup
            // instead of a full-table LIKE scan. Backfill existing rows from path.
            let has_parent: bool = conn
                .prepare("SELECT parent_path FROM known_files LIMIT 0")
                .is_ok();
            if !has_parent {
                log::info!("[macos-cache] Migrating: adding parent_path column + backfilling");
                let _ = conn.execute_batch(
                    "ALTER TABLE known_files ADD COLUMN parent_path TEXT NOT NULL DEFAULT '';",
                );
                // Backfill parent_path from path in application code — simpler + safer
                // than nested substr/instr SQL.
                let rows: Vec<(i64, String)> = conn
                    .prepare("SELECT rowid, path FROM known_files")
                    .and_then(|mut stmt| {
                        stmt.query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
                            .collect::<Result<Vec<_>, _>>()
                    })
                    .unwrap_or_default();

                if let Ok(tx) = conn.transaction() {
                    if let Ok(mut upd) = tx
                        .prepare("UPDATE known_files SET parent_path = ?1 WHERE rowid = ?2")
                    {
                        for (rowid, path) in rows {
                            let _ = upd.execute(params![parent_of(&path), rowid]);
                        }
                    }
                    let _ = tx.commit();
                }
            }

            let _ = conn.execute_batch(
                "CREATE INDEX IF NOT EXISTS idx_hydrated ON known_files(is_hydrated);
                 CREATE INDEX IF NOT EXISTS idx_accessed ON known_files(last_accessed);
                 CREATE INDEX IF NOT EXISTS idx_parent_path ON known_files(parent_path);",
            );

            // Phase 3 slice 2.5: fh lives directly on known_files instead of a
            // separate nfs_handles table. Reasons:
            //   - Two tables meant two places where rows could go out of sync;
            //     repeatedly observed nfs_handles rows vanishing despite no
            //     explicit DELETE in the code (fireworks from INSERT OR
            //     REPLACE + trigger interactions).
            //   - Switching the main upserts from INSERT OR REPLACE (delete +
            //     re-insert) to INSERT ... ON CONFLICT(path) DO UPDATE (update
            //     in place) keeps the rowid/fh stable under re-enumeration.
            //   - One source of truth, simpler to reason about.
            //
            // Migration detects the old schema (path TEXT PRIMARY KEY with no
            // fh column) and rewrites the table in one transaction. Fh values
            // are re-assigned by AUTOINCREMENT — client-cached handles from
            // before the migration become STALE (one final umount/remount).
            let has_fh: bool = conn
                .prepare("SELECT fh FROM known_files LIMIT 0")
                .is_ok();
            if !has_fh {
                log::info!("[macos-cache] Migrating: known_files fh INTEGER PRIMARY KEY + drop nfs_handles");
                let tx = conn
                    .transaction()
                    .map_err(|e| format!("migration tx failed: {}", e))?;
                tx.execute_batch(
                    "DROP TRIGGER IF EXISTS nfs_handles_insert;
                     ALTER TABLE known_files RENAME TO known_files_old;
                     CREATE TABLE known_files (
                         fh INTEGER PRIMARY KEY AUTOINCREMENT,
                         path TEXT NOT NULL UNIQUE COLLATE NOCASE,
                         name TEXT NOT NULL,
                         is_dir INTEGER NOT NULL DEFAULT 0,
                         nas_size INTEGER NOT NULL,
                         nas_mtime REAL NOT NULL,
                         nas_created REAL NOT NULL DEFAULT 0,
                         is_hydrated INTEGER NOT NULL DEFAULT 0,
                         hydrated_size INTEGER DEFAULT 0,
                         last_accessed REAL DEFAULT 0,
                         last_verified_at REAL DEFAULT 0,
                         parent_path TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
                         chunk_bitmap BLOB DEFAULT NULL
                     );
                     -- Reserve fh=1 for the share root.
                     INSERT INTO known_files (fh, path, name, is_dir, nas_size, nas_mtime, parent_path)
                     VALUES (1, '', '', 1, 0, 0, '');
                     INSERT INTO known_files
                         (path, name, is_dir, nas_size, nas_mtime, nas_created,
                          is_hydrated, hydrated_size, last_accessed, last_verified_at,
                          parent_path)
                     SELECT path, name, is_dir, nas_size, nas_mtime, nas_created,
                            COALESCE(is_hydrated, 0),
                            COALESCE(hydrated_size, 0),
                            COALESCE(last_accessed, 0),
                            COALESCE(last_verified_at, 0),
                            COALESCE(parent_path, '')
                     FROM known_files_old
                     WHERE path != '';
                     DROP TABLE known_files_old;
                     DROP TABLE IF EXISTS nfs_handles;
                     CREATE INDEX IF NOT EXISTS idx_hydrated ON known_files(is_hydrated);
                     CREATE INDEX IF NOT EXISTS idx_accessed ON known_files(last_accessed);
                     CREATE INDEX IF NOT EXISTS idx_parent_path ON known_files(parent_path);",
                )
                .map_err(|e| format!("migration schema rewrite failed: {}", e))?;
                tx.commit()
                    .map_err(|e| format!("migration commit failed: {}", e))?;
            } else {
                // Already-new schema on a fresh DB: make sure root row exists.
                let _ = conn.execute(
                    "INSERT OR IGNORE INTO known_files (fh, path, name, is_dir, nas_size, nas_mtime, parent_path)
                     VALUES (1, '', '', 1, 0, 0, '')",
                    [],
                );
                // And tidy up any leftover nfs_handles from a partial upgrade.
                let _ = conn.execute_batch(
                    "DROP TRIGGER IF EXISTS nfs_handles_insert;
                     DROP TABLE IF EXISTS nfs_handles;",
                );
            }

            // Block-level content cache bitmap (Phase 2).
            // One bit per 1 MiB chunk, NULL until any chunk is cached.
            // Fully-hydrated files skip the bitmap via is_hydrated=1.
            let has_bitmap: bool = conn
                .prepare("SELECT chunk_bitmap FROM known_files LIMIT 0")
                .is_ok();
            if !has_bitmap {
                log::info!("[macos-cache] Migrating: adding chunk_bitmap column");
                let _ = conn.execute_batch(
                    "ALTER TABLE known_files ADD COLUMN chunk_bitmap BLOB DEFAULT NULL;",
                );
            }

            // Migration: case-insensitive + Unicode-normalized path keys.
            // SMB is case-insensitive/case-preserving and macOS NFS
            // clients send NFD-decomposed names, but the old schema
            // compared raw bytes — so `File.MOV` vs `file.mov` or
            // NFD-`café` vs NFC-`café` produced ghost rows, duplicate
            // fhs for one physical file, and NOENT on files plainly
            // visible in listings. Rebuild both path-keyed tables with
            // COLLATE NOCASE (every `WHERE path = ?` then matches
            // case-insensitively via the column collation) and rewrite
            // stored paths to NFC. fh values are preserved; rows that
            // collide under the new key (true duplicates of one file)
            // keep the first fh and drop the rest.
            let is_nocase: bool = conn
                .query_row(
                    "SELECT sql FROM sqlite_master WHERE type='table' AND name='known_files'",
                    [],
                    |r| r.get::<_, String>(0),
                )
                .map(|sql| sql.to_uppercase().contains("COLLATE NOCASE"))
                .unwrap_or(true);
            if !is_nocase {
                log::info!("[macos-cache] Migrating: COLLATE NOCASE + NFC path keys");
                migrate_ci_paths(&mut conn)?;
            }
        }

        let pool = cache_core::build_pool(&db_path)?;

        let cache = Self {
            pool,
            nas_root,
            cache_limit,
            cache_dir: cache_dir.to_path_buf(),
            domain: domain.to_string(),
            per_fh_locks: Mutex::new(HashMap::new()),
            badge_tx: Mutex::new(None),
        };
        cache.reconcile_blob_store();
        Ok(cache)
    }

    /// One-time blob-layout migration + startup blob/DB reconcile.
    ///
    /// Layout v2 moved blobs from the flat `by_handle/{fh}` (SHARED
    /// across domains — fh collisions between two mounts served one
    /// share's bytes for another's files) to `by_handle/{domain}/{fh}`.
    /// On first run after the change: dehydrate every row (the old
    /// blobs are unreachable and may belong to another domain) and
    /// purge legacy flat files.
    ///
    /// Then reconcile: any blob in this domain's dir with no row
    /// claiming cached content (hydrated or partial bitmap) is
    /// unlinked. This is a correctness invariant, not just hygiene —
    /// the sparse zero-skip in hydration assumes a fresh cache file
    /// never contains stale non-zero bytes, and fh reassignment (e.g.
    /// the nfs_handles→known_files migration) could otherwise alias an
    /// old blob to a new file.
    fn reconcile_blob_store(&self) {
        let conn = self.conn();

        // ── Layout migration ──
        let layout: Option<String> = conn
            .prepare_cached("SELECT value FROM metadata WHERE key='blob_layout'")
            .ok()
            .and_then(|mut s| s.query_row([], |r| r.get(0)).ok());
        if layout.as_deref() != Some("2") {
            log::info!(
                "[macos-cache] {}: migrating blob store to per-domain layout (full dehydrate)",
                self.domain
            );
            let _ = conn.execute_batch(
                "UPDATE known_files SET is_hydrated=0, hydrated_size=0, chunk_bitmap=NULL;",
            );
            // Purge legacy flat blobs (files directly under by_handle/;
            // per-domain subdirs are left alone).
            let flat = self.cache_dir.join("by_handle");
            if let Ok(rd) = std::fs::read_dir(&flat) {
                for e in rd.flatten() {
                    if e.file_type().map(|t| t.is_file()).unwrap_or(false) {
                        let _ = std::fs::remove_file(e.path());
                    }
                }
            }
            let _ = conn.execute(
                "INSERT OR REPLACE INTO metadata (key, value) VALUES ('blob_layout', '2')",
                [],
            );
        }

        // ── Blob/DB reconcile ──
        let claimed: std::collections::HashSet<u64> = conn
            .prepare_cached(
                "SELECT fh FROM known_files
                 WHERE is_hydrated=1 OR chunk_bitmap IS NOT NULL",
            )
            .ok()
            .and_then(|mut s| {
                s.query_map([], |r| r.get::<_, i64>(0))
                    .ok()
                    .map(|rows| rows.filter_map(|r| r.ok()).map(|v| v as u64).collect())
            })
            .unwrap_or_default();
        let dir = self.cache_file_dir();
        let mut swept = 0usize;
        if let Ok(rd) = std::fs::read_dir(&dir) {
            for e in rd.flatten() {
                let name = e.file_name();
                let Some(name) = name.to_str() else { continue };
                let Ok(fh) = u64::from_str_radix(name, 16) else { continue };
                if !claimed.contains(&fh) {
                    if std::fs::remove_file(e.path()).is_ok() {
                        swept += 1;
                    }
                }
            }
        }
        if swept > 0 {
            log::info!(
                "[macos-cache] {}: swept {} orphaned cache blobs",
                self.domain, swept
            );
        }
    }

    /// Wire a broadcast channel for BadgeUpdate messages. Called once
    /// during agent startup after MacosCache::open. Before this setter
    /// runs or when `None`, badge emissions are silently dropped.
    pub fn set_badge_tx(
        &self,
        tx: tokio::sync::mpsc::Sender<crate::messages::AgentToUfb>,
    ) {
        *self.badge_tx.lock().unwrap() = Some(tx);
    }

    /// Send a BadgeUpdate to any connected subscribers. Non-blocking:
    /// the channel has a bounded queue; if the consumer is slow the
    /// update is silently dropped (FinderSync re-derives state from the
    /// next update or a full-state refresh).
    fn emit_badge(&self, relpath: &str, badge: crate::messages::BadgeKind) {
        let Some(tx) = self.badge_tx.lock().unwrap().clone() else {
            return;
        };
        let msg = crate::messages::AgentToUfb::BadgeUpdate(crate::messages::BadgeUpdateMsg {
            domain: self.domain.clone(),
            relpath: relpath.to_string(),
            badge,
        });
        // try_send drops rather than blocking; acceptable for a coalescing
        // UI cue. Using spawn to avoid awaiting inside sync callers would
        // require a runtime handle — simpler to fire-and-forget.
        let _ = tx.try_send(msg);
    }

    /// Get-or-create the per-fh RwLock used to serialize readers/evictor on a
    /// single cache file. Call before touching `cache_file_path(fh)` from a
    /// request path; hold the read guard for the duration of the I/O.
    pub fn fh_lock(&self, fh: u64) -> Arc<tokio::sync::RwLock<()>> {
        let mut guard = self.per_fh_locks.lock().unwrap();
        guard
            .entry(fh)
            .or_insert_with(|| Arc::new(tokio::sync::RwLock::new(())))
            .clone()
    }

    /// Cache limit in bytes (0 = unlimited).
    pub fn cache_limit(&self) -> u64 {
        self.cache_limit
    }

    /// Get a pooled connection. Short-lived; returned to pool on drop.
    #[inline]
    fn conn(&self) -> SqliteConn {
        self.pool.get().expect("SQLite pool exhausted")
    }

    /// Record a directory listing from an enumeration.
    /// Updates known_files for all entries and marks the folder as visited.
    ///
    /// Drift detection: any entry whose cached (nas_size, nas_mtime) differs
    /// from the enumerated values is queued for eviction. The extension drains
    /// this queue via `getChanges` and calls `evictItem`, dropping cached bytes
    /// so the next open triggers a fresh `fetchContents`.
    ///
    /// Performance: all DB work for a single enumeration happens in ONE
    /// transaction (not N autocommits) using prepared-cached statements.
    /// NAS I/O (folder mtime stat) happens BEFORE acquiring the connection
    /// mutex so other cache operations aren't blocked on SMB latency.
    /// pending_evictions is populated AFTER releasing the connection mutex.
    pub fn record_enumeration(
        &self,
        relative_path: &str,
        entries: &[crate::messages::DirEntry],
        is_partial: bool,
    ) {
        // Stat the NAS folder BEFORE taking the DB mutex — this is SMB I/O.
        let folder_mtime = self.get_folder_mtime(relative_path);

        // (path, fh) of hydrated rows whose NAS size/mtime no longer match
        // the cache — their cached bytes are stale and must be dropped.
        let mut drifted: Vec<(String, u64)> = Vec::new();
        // (fh, path) of rows the orphan prune deleted — their on-disk
        // cache files must go too, or the blobs leak until (never) and a
        // future fh could alias them.
        let mut pruned_files: Vec<(u64, String)> = Vec::new();
        let mut committed = false;

        {
            let mut conn_guard = self.conn();
            let tx = match conn_guard.transaction() {
                Ok(t) => t,
                Err(e) => {
                    log::warn!("[macos-cache] record_enumeration tx begin failed: {}", e);
                    return;
                }
            };

            // Upsert visited folder.
            if let Ok(mut stmt) = tx.prepare_cached(
                "INSERT OR REPLACE INTO visited_folders (path, folder_mtime) VALUES (?1, ?2)",
            ) {
                let _ = stmt.execute(params![relative_path, folder_mtime]);
            }

            // Build set of current entry paths for deletion detection.
            let mut current_paths: HashSet<String> = HashSet::new();

            // Iterate entries: drift-check existing rows, upsert metadata.
            {
                let mut stmt_select = tx
                    .prepare_cached(
                        "SELECT nas_size, nas_mtime, is_hydrated, fh FROM known_files WHERE path = ?1",
                    )
                    .expect("prepare stmt_select");
                // `path = excluded.path` on conflict: with NOCASE keys a
                // conflict can be a case/form variant of the stored path —
                // adopt the enumerated (on-disk) form so the DB tracks
                // reality. fh is untouched, so client handles survive.
                let mut stmt_upsert = tx
                    .prepare_cached(
                        "INSERT INTO known_files
                             (path, name, is_dir, nas_size, nas_mtime, nas_created, parent_path)
                         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                         ON CONFLICT(path) DO UPDATE SET
                             path = excluded.path,
                             name = excluded.name,
                             is_dir = excluded.is_dir,
                             nas_size = excluded.nas_size,
                             nas_mtime = excluded.nas_mtime,
                             nas_created = excluded.nas_created,
                             parent_path = excluded.parent_path",
                    )
                    .expect("prepare stmt_upsert");

                for entry in entries {
                    let entry_path = if relative_path.is_empty() {
                        entry.name.clone()
                    } else {
                        format!("{}/{}", relative_path, entry.name)
                    };
                    // Folded key — SQL matching is NOCASE but this Rust
                    // set isn't; without folding, a row stored under a
                    // different case reads as an orphan and gets pruned.
                    current_paths.insert(entry_path.to_lowercase());

                    if !entry.is_dir {
                        let existing: Option<(i64, f64, i64, i64)> = stmt_select
                            .query_row(params![entry_path], |row| {
                                Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?))
                            })
                            .ok();
                        if let Some((cached_size, cached_mtime, is_hydrated, fh)) = existing {
                            let drift = entry.size != cached_size as u64
                                || (entry.modified - cached_mtime).abs() > 0.001;
                            if drift && is_hydrated != 0 {
                                drifted.push((entry_path.clone(), fh as u64));
                            }
                        }
                    }

                    let _ = stmt_upsert.execute(params![
                        entry_path,
                        entry.name,
                        entry.is_dir as i32,
                        entry.size,
                        entry.modified,
                        entry.created,
                        relative_path, // parent_path
                    ]);
                }
            }

            // Orphan detection + deletion — indexed equality lookup on
            // parent_path. The `path != parent_path` filter excludes the
            // self-reference case: the share root has path='' AND
            // parent_path='' (it's its own parent semantically), which
            // would otherwise flag root as an orphan and delete it
            // whenever we enumerate the share root.
            let orphans: Vec<(String, u64, String, f64, f64)> = {
                let mut stmt_scan = tx
                    .prepare_cached(
                        "SELECT path, fh, name, nas_mtime, last_accessed FROM known_files
                         WHERE parent_path = ?1 AND path != parent_path",
                    )
                    .expect("prepare stmt_scan");
                stmt_scan
                    .query_map(params![relative_path], |row| {
                        Ok((
                            row.get::<_, String>(0)?,
                            row.get::<_, i64>(1)? as u64,
                            row.get::<_, String>(2)?,
                            row.get::<_, f64>(3)?,
                            row.get::<_, f64>(4).unwrap_or(0.0),
                        ))
                    })
                    .map(|rows| rows.filter_map(|r| r.ok()).collect())
                    .unwrap_or_default()
            };

            // Slice E: guard against orphan-deleting the entire
            // folder's cache when SMB returns a transient empty
            // listing. If we cached N children last time and SMB
            // now reports 0, that's almost certainly a flaky
            // readdir (mid-cycle network blip) — not a real folder
            // emptying. Skip the prune; next enumeration retries.
            // Real "user deleted everything" cases still resolve
            // on the second enumeration that confirms empty.
            let suspicious_empty = entries.is_empty() && !orphans.is_empty();
            if is_partial {
                // A listing that dropped entries (per-entry SMB errors)
                // cannot distinguish "deleted" from "unlisted" — pruning
                // on it destroys live fhs. Upserts above still applied.
                log::warn!(
                    "[macos-cache] partial listing for {:?} — skipping orphan prune",
                    relative_path
                );
            } else if suspicious_empty {
                log::warn!(
                    "[macos-cache] {} appears suddenly empty (had {} cached children) \
                     — skipping orphan prune; will retry on next enumeration",
                    if relative_path.is_empty() { "(root)" } else { relative_path },
                    orphans.len(),
                );
            } else if !orphans.is_empty() {
                // Grace window: a row whose NAS mtime or last access is
                // recent belongs to in-flight I/O (every write bumps
                // nas_mtime to "now"). Deleting it kills the fh the NFS
                // client is actively using — the mid-copy STALE bug. A
                // genuinely deleted file older than the window still
                // prunes on the next enumeration.
                const PRUNE_GRACE_SECS: f64 = 60.0;
                let now = crate::sync::cache_core::unix_now_f64();
                let mut stmt_delete = tx
                    .prepare_cached("DELETE FROM known_files WHERE path = ?1")
                    .expect("prepare stmt_delete");
                for (path, fh, name, nas_mtime, last_accessed) in &orphans {
                    if current_paths.contains(&path.to_lowercase()) {
                        continue;
                    }
                    // Hidden-class names (.DS_Store, ._AppleDouble, NAS
                    // sentinels) never appear in enumerations but are
                    // legitimately created through the mount — they are
                    // permanent "orphans" and must never be pruned.
                    if crate::sync::cache_core::is_ignored_name(name) {
                        continue;
                    }
                    if now - nas_mtime < PRUNE_GRACE_SECS
                        || now - last_accessed < PRUNE_GRACE_SECS
                    {
                        continue;
                    }
                    if stmt_delete.execute(params![path]).is_ok() {
                        pruned_files.push((*fh, path.clone()));
                    }
                }
            }

            match tx.commit() {
                Ok(()) => committed = true,
                Err(e) => {
                    log::warn!("[macos-cache] record_enumeration tx commit failed: {}", e)
                }
            }
        }
        // Conn lock released here.

        log::debug!(
            "[macos-cache] record_enumeration parent={:?} entries={} → nfs_handles rows={}",
            relative_path,
            entries.len(),
            self.nfs_handles_count()
        );

        if committed {
            // Remove pruned rows' cache blobs (rows are already gone, so
            // no reader can newly reach them; an in-flight read holding
            // an open fd survives the unlink harmlessly).
            for (fh, _path) in &pruned_files {
                let _ = std::fs::remove_file(self.cache_file_path(*fh));
            }

            // Drift: the NAS copy changed out-of-band (edited by another
            // machine) while we hold hydrated bytes — the cache is now
            // WRONG, not just cold. Drop it so the next read re-hydrates
            // from SMB. try_write: if a reader/writer is mid-flight on
            // the fh, skip this round — drift persists, so the next
            // enumeration retries.
            for (path, fh) in &drifted {
                let lock = self.fh_lock(*fh);
                let Ok(_guard) = lock.try_write() else {
                    log::debug!(
                        "[macos-cache] drift invalidation of {} deferred (fh busy)",
                        path
                    );
                    continue;
                };
                self.invalidate_cache(path, *fh);
            }
            if !drifted.is_empty() {
                log::info!(
                    "[macos-cache] Enumeration drift: invalidated {} stale entries under {:?}",
                    drifted.len(),
                    relative_path
                );
            }
        }
    }

    /// Get changes since a given anchor (timestamp).
    /// Walks all visited folders and diffs NAS state against DB.
    ///
    /// Two-phase: snapshot visited folders under a short-held lock, release it,
    /// then do all NAS read_dirs outside the mutex so other cache writers don't block.
    /// Reacquire the lock for the diff + write-back.
    pub fn get_changes_since(&self, _since_anchor: f64) -> ChangesResult {
        // ── Phase A: snapshot visited folders (short-held lock) ──
        let folders: Vec<String> = {
            let conn = self.conn();
            let mut list: Vec<String> = conn
                .prepare_cached("SELECT path FROM visited_folders")
                .ok()
                .and_then(|mut stmt| {
                    stmt.query_map([], |row| row.get::<_, String>(0))
                        .ok()
                        .map(|rows| rows.filter_map(|r| r.ok()).collect())
                })
                .unwrap_or_default();
            if !list.iter().any(|p| p.is_empty()) {
                list.push(String::new());
            }
            list
        };

        // ── Phase B: do all NAS read_dirs WITHOUT the conn lock ──
        type NasEntries = HashMap<String, (bool, u64, f64, f64)>;
        let mut snapshot: Vec<(String, NasEntries)> = Vec::with_capacity(folders.len());

        for folder_path in &folders {
            let nas_folder = if folder_path.is_empty() {
                self.nas_root.clone()
            } else {
                self.nas_root.join(folder_path)
            };

            if !nas_folder.is_dir() {
                continue;
            }

            let nas_entries: NasEntries = match std::fs::read_dir(&nas_folder) {
                Ok(rd) => rd.flatten()
                    .filter_map(|entry| {
                        let name = entry.file_name().to_string_lossy().to_string();
                        if name.starts_with('.') || name.starts_with('@') || name.starts_with('#') {
                            return None;
                        }
                        let meta = entry.metadata().ok()?;
                        let mtime = meta.modified().ok()
                            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                            .map(|d| d.as_secs_f64()).unwrap_or(0.0);
                        let ctime = meta.created().ok()
                            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                            .map(|d| d.as_secs_f64()).unwrap_or(0.0);
                        Some((name, (meta.is_dir(), meta.len(), mtime, ctime)))
                    })
                    .collect(),
                Err(_) => continue,
            };

            snapshot.push((folder_path.clone(), nas_entries));
        }

        // ── Phase C: reacquire lock, diff DB against snapshot, write updates in one tx ──
        let mut updated: Vec<ChangedEntry> = Vec::new();
        let mut deleted: Vec<String> = Vec::new();

        {
            let mut conn = self.conn();
            let tx = match conn.transaction() {
                Ok(t) => t,
                Err(e) => {
                    log::warn!("[macos-cache] get_changes_since: failed to begin tx: {}", e);
                    return ChangesResult {
                        updated,
                        deleted,
                        new_anchor: std::time::SystemTime::now()
                            .duration_since(std::time::UNIX_EPOCH)
                            .unwrap_or_default()
                            .as_secs_f64(),
                    };
                }
            };

            for (folder_path, nas_entries) in &snapshot {
                let db_files: HashMap<String, (String, bool, u64, f64, f64)> = tx
                    .prepare_cached(
                        "SELECT path, name, is_dir, nas_size, nas_mtime, nas_created
                         FROM known_files WHERE parent_path = ?1",
                    )
                    .ok()
                    .and_then(|mut stmt| {
                        stmt.query_map(params![folder_path], |row| {
                            Ok((
                                row.get::<_, String>(0)?,
                                (
                                    row.get::<_, String>(1)?,
                                    row.get::<_, i32>(2)? != 0,
                                    row.get::<_, u64>(3)?,
                                    row.get::<_, f64>(4)?,
                                    row.get::<_, f64>(5)?,
                                ),
                            ))
                        })
                        .ok()
                        .map(|rows| rows.filter_map(|r| r.ok()).collect())
                    })
                    .unwrap_or_default();

                let db_names: HashSet<String> = db_files.values().map(|(name, _, _, _, _)| name.clone()).collect();
                let nas_names: HashSet<String> = nas_entries.keys().cloned().collect();

                for name in nas_names.difference(&db_names) {
                    if let Some((is_dir, size, mtime, ctime)) = nas_entries.get(name) {
                        let rel_path = if folder_path.is_empty() {
                            name.clone()
                        } else {
                            format!("{}/{}", folder_path, name)
                        };
                        updated.push(ChangedEntry {
                            relative_path: rel_path,
                            name: name.clone(),
                            is_dir: *is_dir,
                            size: *size,
                            modified: *mtime,
                            created: *ctime,
                        });
                    }
                }

                for name in db_names.difference(&nas_names) {
                    let rel_path = if folder_path.is_empty() {
                        name.clone()
                    } else {
                        format!("{}/{}", folder_path, name)
                    };
                    deleted.push(rel_path);
                }

                for name in nas_names.intersection(&db_names) {
                    if let Some((is_dir, nas_size, nas_mtime, nas_ctime)) = nas_entries.get(name) {
                        let rel_path = if folder_path.is_empty() {
                            name.clone()
                        } else {
                            format!("{}/{}", folder_path, name)
                        };
                        if let Some((_, _, db_size, db_mtime, _)) = db_files.get(&rel_path) {
                            if *nas_size != *db_size || (*nas_mtime - *db_mtime).abs() > 1.0 {
                                updated.push(ChangedEntry {
                                    relative_path: rel_path,
                                    name: name.clone(),
                                    is_dir: *is_dir,
                                    size: *nas_size,
                                    modified: *nas_mtime,
                                    created: *nas_ctime,
                                });
                            }
                        }
                    }
                }
            }

            // Apply updates + deletes inside the same transaction.
            {
                let mut upsert = tx
                    .prepare_cached(
                        "INSERT INTO known_files
                             (path, name, is_dir, nas_size, nas_mtime, nas_created, parent_path)
                         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                         ON CONFLICT(path) DO UPDATE SET
                             name = excluded.name,
                             is_dir = excluded.is_dir,
                             nas_size = excluded.nas_size,
                             nas_mtime = excluded.nas_mtime,
                             nas_created = excluded.nas_created,
                             parent_path = excluded.parent_path",
                    )
                    .ok();
                if let Some(stmt) = upsert.as_mut() {
                    for entry in &updated {
                        let _ = stmt.execute(params![
                            entry.relative_path,
                            entry.name,
                            entry.is_dir as i32,
                            entry.size,
                            entry.modified,
                            entry.created,
                            parent_of(&entry.relative_path),
                        ]);
                    }
                }
            }
            {
                let mut del = tx.prepare_cached("DELETE FROM known_files WHERE path = ?1").ok();
                if let Some(stmt) = del.as_mut() {
                    for path in &deleted {
                        let _ = stmt.execute(params![path]);
                    }
                }
            }

            if let Err(e) = tx.commit() {
                log::warn!("[macos-cache] get_changes_since: commit failed: {}", e);
            }
        }

        let new_anchor = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs_f64();

        log::info!("[macos-cache] Changes: {} updated, {} deleted", updated.len(), deleted.len());

        ChangesResult {
            updated,
            deleted,
            new_anchor,
        }
    }

    /// Remove a known file from the DB (after deletion).
    pub fn remove_known_file(&self, relative_path: &str) {
        let conn = self.conn();
        let _ = conn
            .prepare_cached("DELETE FROM known_files WHERE path = ?1")
            .map(|mut stmt| stmt.execute(params![relative_path]));
    }

    /// Add or update a known file in the DB (after write).
    pub fn record_known_file(&self, relative_path: &str, entry: &crate::messages::DirEntry) {
        let conn = self.conn();
        let parent = parent_of(relative_path).to_string();
        let _ = conn
            .prepare_cached(
                "INSERT INTO known_files
                     (path, name, is_dir, nas_size, nas_mtime, nas_created, parent_path)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                 ON CONFLICT(path) DO UPDATE SET
                     name = excluded.name,
                     is_dir = excluded.is_dir,
                     nas_size = excluded.nas_size,
                     nas_mtime = excluded.nas_mtime,
                     nas_created = excluded.nas_created,
                     parent_path = excluded.parent_path"
            )
            .map(|mut stmt| {
                stmt.execute(params![
                    relative_path, entry.name, entry.is_dir as i32,
                    entry.size, entry.modified, entry.created, parent,
                ])
            });
    }

    // ── Hydration tracking + LRU eviction ──

    /// Record that a file was downloaded (materialized). Triggers eviction check.
    pub fn record_hydration(&self, relative_path: &str, size: u64) {
        if size == 0 {
            return;
        }
        let now = unix_now_f64();

        {
            let conn = self.conn();
            let _ = conn
                .prepare_cached(
                    "UPDATE known_files SET is_hydrated=1, hydrated_size=?1, last_accessed=?2 WHERE path=?3",
                )
                .map(|mut stmt| stmt.execute(params![size as i64, now, relative_path]));
        }

        self.emit_badge(relative_path, crate::messages::BadgeKind::Hydrated);
        // The NFS evictor task (spawned from nfs_server::start) enforces the
        // budget on its own 30s tick. No synchronous trigger here.
    }

    /// Update last_accessed time (called on each file read).
    pub fn touch(&self, relative_path: &str) {
        let now = unix_now_f64();
        let conn = self.conn();
        let _ = conn
            .prepare_cached("UPDATE known_files SET last_accessed=?1 WHERE path=?2")
            .map(|mut stmt| stmt.execute(params![now, relative_path]));
    }

    /// Stamp last_verified_at = now. Called after a successful NAS stat confirms
    /// cached metadata matches reality.
    pub fn record_verification(&self, relative_path: &str) {
        let now = unix_now_f64();
        let conn = self.conn();
        let _ = conn
            .prepare_cached("UPDATE known_files SET last_verified_at=?1 WHERE path=?2")
            .map(|mut stmt| stmt.execute(params![now, relative_path]));
    }

    /// Update NAS metadata to fresh stat values and stamp last_verified_at.
    /// Preserves hydration state and last_accessed via targeted UPDATE.
    pub fn update_nas_metadata(&self, relative_path: &str, nas_size: u64, nas_mtime: f64) {
        let now = unix_now_f64();
        let conn = self.conn();
        let _ = conn
            .prepare_cached(
                "UPDATE known_files SET nas_size=?1, nas_mtime=?2, last_verified_at=?3 WHERE path=?4",
            )
            .map(|mut stmt| stmt.execute(params![nas_size as i64, nas_mtime, now, relative_path]));
    }

    /// Returns cached `(nas_size, nas_mtime)` if the entry warrants re-verification,
    /// or `None` if we should skip the NAS stat.
    pub fn needs_verification(&self, relative_path: &str, ttl_secs: f64) -> Option<(u64, f64)> {
        let now = unix_now_f64();
        let conn = self.conn();
        let row: Option<(i64, f64, f64)> = conn
            .prepare_cached(
                "SELECT nas_size, nas_mtime, last_verified_at FROM known_files WHERE path=?1",
            )
            .ok()
            .and_then(|mut stmt| {
                stmt.query_row(params![relative_path], |row| {
                    Ok((row.get::<_, i64>(0)?, row.get::<_, f64>(1)?, row.get::<_, f64>(2)?))
                })
                .ok()
            });
        row.and_then(|(size, mtime, verified)| {
            if now - verified < ttl_secs {
                None
            } else {
                Some((size as u64, mtime))
            }
        })
    }

    /// Compare provided NAS metadata against the cached row.
    pub fn compare_nas_metadata(
        &self,
        relative_path: &str,
        nas_size: u64,
        nas_mtime: f64,
    ) -> Option<bool> {
        let conn = self.conn();
        let cached: Option<(i64, f64)> = conn
            .prepare_cached("SELECT nas_size, nas_mtime FROM known_files WHERE path=?1")
            .ok()
            .and_then(|mut stmt| {
                stmt.query_row(params![relative_path], |row| {
                    Ok((row.get::<_, i64>(0)?, row.get::<_, f64>(1)?))
                })
                .ok()
            });
        cached.map(|(cached_size, cached_mtime)| {
            nas_size != cached_size as u64 || (nas_mtime - cached_mtime).abs() > 0.001
        })
    }

    /// Check if a path is tracked in known_files (has any row).
    pub fn is_known(&self, relative_path: &str) -> bool {
        let conn = self.conn();
        conn.prepare_cached("SELECT 1 FROM known_files WHERE path=?1")
            .ok()
            .and_then(|mut stmt| stmt.query_row(params![relative_path], |_| Ok(true)).ok())
            .unwrap_or(false)
    }

    /// Total bytes of hydrated (locally cached) files.
    pub fn total_cached_bytes(&self) -> u64 {
        let conn = self.conn();
        conn.prepare_cached(
            "SELECT COALESCE(SUM(hydrated_size), 0) FROM known_files WHERE is_hydrated=1",
        )
        .ok()
        .and_then(|mut stmt| stmt.query_row([], |row| row.get::<_, i64>(0)).ok())
        .unwrap_or(0) as u64
    }

    /// Eviction: if hydrated bytes exceed `cache_limit`, delete
    /// oldest-accessed cache files until back under `cache_limit * 0.8`.
    /// Performs the actual `fs::remove_file` + row update inline.
    ///
    /// Uses `try_write` on the per-fh RwLock so in-flight NFS reads are never
    /// blocked; contending files are skipped this tick and revisited next
    /// time. Returns `(files_evicted, bytes_freed)`.
    pub async fn evict_over_budget_now(&self) -> (usize, u64) {
        if self.cache_limit == 0 {
            return (0, 0);
        }
        let total = self.total_cached_bytes();
        if total <= self.cache_limit {
            return (0, 0);
        }
        let target = (self.cache_limit as f64 * EVICTION_TARGET_PERCENT) as u64;

        // Collect candidates in LRU order. We include `fh` so we can scope
        // the per-fh lock and name the cache file without another lookup,
        // and `path` so we can emit a BadgeUpdate after eviction.
        let candidates: Vec<(u64, String, u64)> = {
            let conn = self.conn();
            let mut stmt = match conn.prepare(
                "SELECT fh, path, hydrated_size FROM known_files
                 WHERE is_hydrated=1 AND is_dir=0
                 ORDER BY last_accessed ASC",
            ) {
                Ok(s) => s,
                Err(e) => {
                    log::error!("[macos-cache] eviction prepare failed: {}", e);
                    return (0, 0);
                }
            };
            stmt.query_map([], |row| {
                Ok((
                    row.get::<_, i64>(0)? as u64,
                    row.get::<_, String>(1)?,
                    row.get::<_, i64>(2)? as u64,
                ))
            })
            .ok()
            .map(|it| it.filter_map(|r| r.ok()).collect())
            .unwrap_or_default()
        };

        let mut remaining = total;
        let mut files_evicted = 0usize;

        for (fh, path, size) in candidates {
            if remaining <= target {
                break;
            }
            // Non-blocking try: if a read is in flight, skip and revisit later.
            let lock = self.fh_lock(fh);
            let Ok(_write_guard) = lock.try_write() else {
                continue;
            };

            // Flip the row FIRST, then remove the bytes. The reverse
            // order has a window where is_hydrated=1 but the file is
            // gone — a reader hitting the hydrated fast path then gets
            // zeros/ENOENT. Row-first is safe: a reader arriving after
            // the flip sees is_hydrated=0 and refetches from SMB; if
            // the remove_file below fails, the stale blob is orphaned
            // (harmless — nothing references it, startup reconcile
            // sweeps it) rather than lied about.
            {
                let conn = self.conn();
                let _ = conn
                    .prepare_cached(
                        "UPDATE known_files
                         SET is_hydrated=0, hydrated_size=0, chunk_bitmap=NULL
                         WHERE fh=?1",
                    )
                    .map(|mut stmt| stmt.execute(params![fh as i64]));
            }
            let cache_path = self.cache_file_path(fh);
            let _ = std::fs::remove_file(&cache_path);

            self.emit_badge(&path, crate::messages::BadgeKind::Uncached);

            remaining = remaining.saturating_sub(size);
            files_evicted += 1;

            // Drop the map entry so we don't accumulate dead Arcs. Next
            // access will re-create lazily.
            self.per_fh_locks.lock().unwrap().remove(&fh);
        }

        let bytes_freed = total - remaining;
        if files_evicted > 0 {
            log::info!(
                "[macos-cache] NFS eviction: {} files ({:.1} MB) — cache {:.1}/{:.1} MB",
                files_evicted,
                bytes_freed as f64 / 1_048_576.0,
                remaining as f64 / 1_048_576.0,
                self.cache_limit as f64 / 1_048_576.0,
            );
        }
        (files_evicted, bytes_freed)
    }

    // ── Content cache (Phase 2) ──

    /// Read the current chunk bitmap for a file. Returns an empty Vec for
    /// uncached files (no chunks yet). Cheap — single indexed read.
    pub fn get_chunk_bitmap(&self, path: &str) -> Vec<u8> {
        let conn = self.conn();
        let Ok(mut stmt) =
            conn.prepare_cached("SELECT chunk_bitmap FROM known_files WHERE path = ?1")
        else {
            return Vec::new();
        };
        stmt.query_row(params![path], |row| {
            Ok(row.get::<_, Option<Vec<u8>>>(0)?.unwrap_or_default())
        })
        .unwrap_or_default()
    }

    /// Persist an updated chunk bitmap for a file. Also advances
    /// `last_accessed` (the read path is the only caller right now, so this
    /// keeps eviction LRU honest).
    pub fn update_chunk_bitmap(&self, path: &str, bitmap: &[u8]) {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs_f64();
        let conn = self.conn();
        let _ = conn
            .prepare_cached(
                "UPDATE known_files
                 SET chunk_bitmap = ?1, last_accessed = ?2
                 WHERE path = ?3",
            )
            .map(|mut stmt| stmt.execute(params![bitmap, now, path]));
    }

    /// Insert (or update) a single entry in `known_files`. On first insert
    /// AUTOINCREMENT assigns an `fh`; on conflict we UPDATE in place so the
    /// `fh` stays stable (critical for NFS — client-cached handles for this
    /// path keep working). Used by NFS CREATE / MKDIR to register a
    /// freshly-created file or folder without running `record_enumeration`'s
    /// orphan-scan against the parent (which would erase sibling rows).
    pub fn record_new_entry(
        &self,
        relative_path: &str,
        name: &str,
        is_dir: bool,
        size: u64,
        mtime: f64,
        created: f64,
    ) {
        let parent = parent_of(relative_path).to_string();
        let conn = self.conn();
        let _ = conn
            .prepare_cached(
                "INSERT INTO known_files
                     (path, name, is_dir, nas_size, nas_mtime, nas_created, parent_path)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                 ON CONFLICT(path) DO UPDATE SET
                     name = excluded.name,
                     is_dir = excluded.is_dir,
                     nas_size = excluded.nas_size,
                     nas_mtime = excluded.nas_mtime,
                     nas_created = excluded.nas_created,
                     parent_path = excluded.parent_path",
            )
            .map(|mut stmt| {
                stmt.execute(params![
                    relative_path,
                    name,
                    is_dir as i32,
                    size as i64,
                    mtime,
                    created,
                    parent,
                ])
            });
    }

    /// Reflect an NFS RENAME in the cache. Preserves the source row's `fh`
    /// so NFS clients' cached handles keep resolving after the rename —
    /// critical for editor "save as" patterns that rename over the file.
    ///
    /// Caller must have performed `std::fs::rename(from_abs, to_abs)` first.
    /// This function does the DB-side bookkeeping:
    ///   1. Delete any stale rows at the target (the disk rename already
    ///      clobbered them).
    ///   2. UPDATE the source row's path/parent_path/name in place so `fh`
    ///      survives.
    ///   3. Fix up every descendant row (when renaming a directory) —
    ///      their path prefix changes, their `fh` stays.
    pub fn rename_path(&self, from: &str, to: &str) -> Result<(), String> {
        let mut conn = self.conn();
        let tx = conn
            .transaction()
            .map_err(|e| format!("rename tx begin: {}", e))?;

        // Clear anything under the target path. The disk rename moved the
        // actual files to `to`, so whatever was previously there is gone.
        // Collect the doomed rows' fhs first — their on-disk cache blobs
        // must be removed too (rename-over-existing is the editor
        // atomic-save pattern; leaking a blob per save adds up).
        let clobbered_fhs: Vec<i64> = {
            let mut stmt = tx
                .prepare(
                    "SELECT fh FROM known_files WHERE path = ?1 OR path LIKE ?1 || '/%'",
                )
                .map_err(|e| format!("rename scan target: {}", e))?;
            let rows = stmt
                .query_map(params![to], |row| row.get::<_, i64>(0))
                .map_err(|e| format!("rename query target: {}", e))?;
            rows.filter_map(|r| r.ok()).collect()
        };
        tx.execute(
            "DELETE FROM known_files WHERE path = ?1 OR path LIKE ?1 || '/%'",
            params![to],
        )
        .map_err(|e| format!("rename clear target: {}", e))?;

        let new_parent = parent_of(to).to_string();
        let new_name = to.rsplit('/').next().unwrap_or(to).to_string();
        tx.execute(
            "UPDATE known_files SET path = ?1, parent_path = ?2, name = ?3
             WHERE path = ?4",
            params![to, new_parent, new_name, from],
        )
        .map_err(|e| format!("rename update source: {}", e))?;

        // Descendant fixup for directory renames. Collect first (borrow
        // ends with the prepare), then apply UPDATEs.
        let from_prefix = format!("{}/", from);
        let to_prefix = format!("{}/", to);
        let descendants: Vec<(i64, String)> = {
            let mut stmt = tx
                .prepare("SELECT fh, path FROM known_files WHERE path LIKE ?1 || '%'")
                .map_err(|e| format!("rename prep desc: {}", e))?;
            let rows = stmt
                .query_map(params![from_prefix], |row| {
                    Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
                })
                .map_err(|e| format!("rename query desc: {}", e))?;
            let mut out = Vec::new();
            for r in rows {
                out.push(r.map_err(|e| format!("rename row desc: {}", e))?);
            }
            out
        };

        for (fh, old_desc_path) in descendants {
            let new_desc_path =
                format!("{}{}", to_prefix, &old_desc_path[from_prefix.len()..]);
            let new_desc_parent = parent_of(&new_desc_path).to_string();
            tx.execute(
                "UPDATE known_files SET path = ?1, parent_path = ?2 WHERE fh = ?3",
                params![new_desc_path, new_desc_parent, fh],
            )
            .map_err(|e| format!("rename update desc: {}", e))?;
        }

        tx.commit()
            .map_err(|e| format!("rename commit: {}", e))?;

        // Rows are gone; now the blobs. Post-commit so a failed rename
        // never deletes bytes for rows that still exist.
        for fh in clobbered_fhs {
            let _ = std::fs::remove_file(self.cache_file_path(fh as u64));
        }
        Ok(())
    }

    /// Remove a path from the cache. Deletes its row from `known_files`
    /// (its fh becomes permanently `NFS3ERR_STALE` — AUTOINCREMENT never
    /// reuses it) and deletes the on-disk cache file if present. Used by
    /// NFS REMOVE / RMDIR after the NAS-side delete succeeds.
    pub fn forget_path(&self, path: &str, fh: u64) {
        {
            let conn = self.conn();
            let _ = conn
                .prepare_cached("DELETE FROM known_files WHERE path = ?1")
                .map(|mut stmt| stmt.execute(params![path]));
        }
        let cache_path = self.cache_file_path(fh);
        let _ = std::fs::remove_file(&cache_path);
    }

    /// Invalidate the content cache for a file — clears `is_hydrated`, drops
    /// the chunk bitmap, and deletes the on-disk cache file. Metadata rows
    /// (size, mtime, fh) are left untouched; callers should follow up with
    /// `update_nas_metadata` after the authoritative NAS state is known.
    /// Called on every write path so subsequent reads re-hydrate from SMB.
    pub fn invalidate_cache(&self, path: &str, fh: u64) {
        {
            let conn = self.conn();
            let _ = conn
                .prepare_cached(
                    "UPDATE known_files
                     SET is_hydrated = 0, hydrated_size = 0, chunk_bitmap = NULL
                     WHERE path = ?1",
                )
                .map(|mut stmt| stmt.execute(params![path]));
        }
        let cache_path = self.cache_file_path(fh);
        let _ = std::fs::remove_file(&cache_path);
        self.emit_badge(path, crate::messages::BadgeKind::Uncached);
    }

    /// Mark a file as fully hydrated (all chunks cached). Nulls the bitmap
    /// since `is_hydrated=1` is the fast-path shortcut.
    pub fn mark_fully_hydrated(&self, path: &str, size: u64) {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs_f64();
        {
            let conn = self.conn();
            let _ = conn
                .prepare_cached(
                    "UPDATE known_files
                     SET is_hydrated = 1,
                         hydrated_size = ?1,
                         chunk_bitmap = NULL,
                         last_accessed = ?2
                     WHERE path = ?3",
                )
                .map(|mut stmt| stmt.execute(params![size as i64, now, path]));
        }
        self.emit_badge(path, crate::messages::BadgeKind::Hydrated);
    }

    /// Cache-file path for a given NFS handle. Directory is created on first
    /// call; callers can assume the parent exists.
    pub fn cache_file_path(&self, fh: u64) -> PathBuf {
        let dir = self.cache_file_dir();
        let _ = std::fs::create_dir_all(&dir);
        dir.join(format!("{:016x}", fh))
    }

    /// Root of the content-cache filesystem layout for this domain. Rooted
    /// under the user-configurable `cache_dir` passed into `open()`.
    ///
    /// MUST be namespaced by domain: `cache_dir` is shared across all
    /// mounts while each domain has its own DB, so fh values (per-DB
    /// AUTOINCREMENT) collide across domains. A flat `by_handle/` had
    /// two shares' fh=N resolving to the SAME blob — reads served
    /// another share's file content.
    fn cache_file_dir(&self) -> PathBuf {
        self.cache_dir.join("by_handle").join(&self.domain)
    }

    // ── NFS handle / metadata serving (Phase 1) ──

    /// Look up the NFS file handle for a relative path. `""` is the root.
    /// Returns `None` if the path has never been indexed.
    pub fn fh_for_path(&self, path: &str) -> Option<u64> {
        let conn = self.conn();
        let mut stmt = conn.prepare_cached("SELECT fh FROM known_files WHERE path = ?1").ok()?;
        stmt.query_row(params![path], |row| row.get::<_, i64>(0))
            .ok()
            .map(|v| v as u64)
    }

    /// Diagnostic: total number of known_files rows (one per cached path).
    pub fn nfs_handles_count(&self) -> i64 {
        let conn = self.conn();
        conn.prepare_cached("SELECT COUNT(*) FROM known_files")
            .ok()
            .and_then(|mut stmt| stmt.query_row([], |row| row.get(0)).ok())
            .unwrap_or(-1)
    }

    /// Reverse lookup: path for an NFS file handle.
    pub fn path_for_fh(&self, fh: u64) -> Option<String> {
        let conn = self.conn();
        let mut stmt = conn.prepare_cached("SELECT path FROM known_files WHERE fh = ?1").ok()?;
        stmt.query_row(params![fh as i64], |row| row.get::<_, String>(0)).ok()
    }

    /// Ensure a handle exists for `path`, returning its fh. On first insert
    /// AUTOINCREMENT assigns an `fh`; on conflict the row is left untouched.
    /// Used for the share root at server startup (so fh=1 is always reserved).
    pub fn ensure_fh(&self, path: &str) -> Option<u64> {
        {
            let conn = self.conn();
            let parent = parent_of(path).to_string();
            let mut stmt = conn
                .prepare_cached(
                    "INSERT INTO known_files
                         (path, name, is_dir, nas_size, nas_mtime, parent_path)
                     VALUES (?1, ?2, 1, 0, 0, ?3)
                     ON CONFLICT(path) DO NOTHING",
                )
                .ok()?;
            let _ = stmt.execute(params![path, path, parent]);
        }
        self.fh_for_path(path)
    }

    /// Cached metadata for a given path — fields an NFS GETATTR needs. Returns
    /// `None` if the path isn't in `known_files` (cold — caller must populate).
    pub fn cached_attr(&self, path: &str) -> Option<CachedAttr> {
        let conn = self.conn();
        let mut stmt = conn
            .prepare_cached(
                "SELECT is_dir, nas_size, nas_mtime, nas_created, is_hydrated, hydrated_size
                 FROM known_files WHERE path = ?1",
            )
            .ok()?;
        stmt.query_row(params![path], |row| {
            Ok(CachedAttr {
                is_dir: row.get::<_, i32>(0)? != 0,
                size: row.get::<_, i64>(1)? as u64,
                mtime: row.get::<_, f64>(2)?,
                created: row.get::<_, f64>(3)?,
                is_hydrated: row.get::<_, i32>(4)? != 0,
                hydrated_size: row.get::<_, i64>(5)? as u64,
            })
        })
        .ok()
    }

    /// Direct children of a folder — (fh, name, CachedAttr) tuples, joined
    /// across `known_files` and `nfs_handles`. Returns an empty Vec for cold
    /// folders (caller falls back to live enumeration then recalls us).
    pub fn cached_children(&self, parent_path: &str) -> Vec<(u64, String, CachedAttr)> {
        let conn = self.conn();
        conn.prepare_cached(
            "SELECT fh, name, is_dir, nas_size, nas_mtime, nas_created,
                    is_hydrated, hydrated_size
             FROM known_files
             WHERE parent_path = ?1 AND path != ''",
        )
        .ok()
        .and_then(|mut stmt| {
            stmt.query_map(params![parent_path], |row| {
                Ok((
                    row.get::<_, i64>(0)? as u64,
                    row.get::<_, String>(1)?,
                    CachedAttr {
                        is_dir: row.get::<_, i32>(2)? != 0,
                        size: row.get::<_, i64>(3)? as u64,
                        mtime: row.get::<_, f64>(4)?,
                        created: row.get::<_, f64>(5)?,
                        is_hydrated: row.get::<_, i32>(6)? != 0,
                        hydrated_size: row.get::<_, i64>(7)? as u64,
                    },
                ))
            })
            .ok()
            .map(|rows| rows.filter_map(|r| r.ok()).collect())
        })
        .unwrap_or_default()
    }

    /// Has this folder already been enumerated? Used to distinguish
    /// cold-path (fall through to NAS) vs empty-but-warm folders.
    pub fn folder_is_enumerated(&self, path: &str) -> bool {
        let conn = self.conn();
        conn.prepare_cached("SELECT 1 FROM visited_folders WHERE path = ?1")
            .ok()
            .and_then(|mut stmt| stmt.query_row(params![path], |_| Ok(true)).ok())
            .unwrap_or(false)
    }

    /// The SMB folder mtime we stored last time we enumerated this
    /// folder. `None` if the folder has never been visited. Paired with
    /// `folder_needs_reenum` for logging the cached-vs-live comparison.
    pub fn cached_folder_mtime(&self, path: &str) -> Option<f64> {
        let conn = self.conn();
        conn.prepare_cached("SELECT folder_mtime FROM visited_folders WHERE path = ?1")
            .ok()
            .and_then(|mut stmt| stmt.query_row(params![path], |row| row.get(0)).ok())
    }

    /// Should this folder be re-enumerated from SMB?
    ///
    /// Returns true if either:
    /// - The folder has never been visited (cold), OR
    /// - The folder's cached `folder_mtime` is older than `current_nas_mtime`
    ///   (out-of-band changes on SMB — another machine added/removed files
    ///   inside this folder, which bumps its mtime).
    ///
    /// `record_enumeration` stores the SMB folder mtime (not the visit
    /// time) so subsequent comparisons here are meaningful. Mirror of
    /// `windows_cache::folder_needs_reenum`.
    ///
    /// SMB mtime granularity is ~1s; the 1.0 slop avoids spurious
    /// re-enum churn from sub-second clock skew.
    pub fn folder_needs_reenum(&self, path: &str, current_nas_mtime: f64) -> bool {
        match self.cached_folder_mtime(path) {
            None => true,
            Some(cached_mtime) => current_nas_mtime > cached_mtime + 1.0,
        }
    }

    /// NFS-native "drain all": delete every hydrated cache file for this
    /// share, flip their DB rows to uncached, and return (count, bytes).
    /// Uses non-blocking `try_write` on each fh — files with active readers
    /// are skipped (user retries; rare). Intended to back the UI "Drain
    /// Cache" button under NFS mode.
    pub async fn drain_all(&self) -> (u64, u64) {
        let candidates: Vec<(u64, String, u64)> = {
            let conn = self.conn();
            let mut stmt = match conn.prepare(
                "SELECT fh, path, hydrated_size FROM known_files
                 WHERE is_hydrated=1 AND is_dir=0",
            ) {
                Ok(s) => s,
                Err(e) => {
                    log::error!("[macos-cache] drain_all prepare failed: {}", e);
                    return (0, 0);
                }
            };
            stmt.query_map([], |row| {
                Ok((
                    row.get::<_, i64>(0)? as u64,
                    row.get::<_, String>(1)?,
                    row.get::<_, i64>(2)? as u64,
                ))
            })
            .ok()
            .map(|it| it.filter_map(|r| r.ok()).collect())
            .unwrap_or_default()
        };

        let mut count = 0u64;
        let mut bytes = 0u64;
        for (fh, path, size) in candidates {
            let lock = self.fh_lock(fh);
            let Ok(_wg) = lock.try_write() else {
                continue;
            };
            // Row first, then file — same ordering rationale as
            // evict_over_budget_now: never leave is_hydrated=1 pointing
            // at a deleted blob.
            {
                let conn = self.conn();
                let _ = conn
                    .prepare_cached(
                        "UPDATE known_files
                         SET is_hydrated=0, hydrated_size=0, chunk_bitmap=NULL
                         WHERE fh=?1",
                    )
                    .map(|mut stmt| stmt.execute(params![fh as i64]));
            }
            let cache_path = self.cache_file_path(fh);
            let _ = std::fs::remove_file(&cache_path);
            self.emit_badge(&path, crate::messages::BadgeKind::Uncached);
            self.per_fh_locks.lock().unwrap().remove(&fh);
            count += 1;
            bytes += size;
        }

        if count > 0 {
            log::info!(
                "[macos-cache] drain_all: {} files, {:.1} MB",
                count,
                bytes as f64 / 1_048_576.0,
            );
        }
        (count, bytes)
    }

    /// Cheap single-query stats for the UI: (hydrated_bytes, hydrated_count).
    pub fn cache_stats(&self) -> (u64, u64) {
        let conn = self.conn();
        let row: Option<(i64, i64)> = conn
            .prepare_cached(
                "SELECT
                    COALESCE(SUM(hydrated_size), 0),
                    COUNT(*)
                 FROM known_files
                 WHERE is_hydrated=1 AND is_dir=0",
            )
            .ok()
            .and_then(|mut stmt| {
                stmt.query_row([], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?)))
                    .ok()
            });
        row.map(|(b, c)| (b as u64, c as u64)).unwrap_or((0, 0))
    }

    /// Get the NAS folder mtime for a relative path.
    fn get_folder_mtime(&self, relative_path: &str) -> f64 {
        let nas_folder = if relative_path.is_empty() {
            self.nas_root.clone()
        } else {
            self.nas_root.join(relative_path)
        };
        std::fs::metadata(&nas_folder)
            .ok()
            .and_then(|m| m.modified().ok())
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs_f64())
            .unwrap_or(0.0)
    }
}

/// Result of a change detection query.
pub struct ChangesResult {
    pub updated: Vec<ChangedEntry>,
    pub deleted: Vec<String>,
    pub new_anchor: f64,
}

/// A changed file/folder entry with full metadata.
#[derive(Clone)]
pub struct ChangedEntry {
    pub relative_path: String,
    pub name: String,
    pub is_dir: bool,
    pub size: u64,
    pub modified: f64,
    pub created: f64,
}

/// Result of a stat-and-refresh call against the NAS.
#[derive(Debug)]
pub enum StatResult {
    /// Within TTL — no stat was performed. Caller can serve cached data.
    Skipped,
    /// NAS was stat'd; matched cached metadata. `last_verified_at` was refreshed.
    Fresh { size: u64, mtime: f64 },
    /// NAS was stat'd; drifted from cache. DB updated with new values.
    /// Caller may want to invalidate OS-side cached content.
    Drifted { size: u64, mtime: f64 },
    /// Path unknown to cache — caller should let the normal read path populate
    /// a baseline. Fresh stat values provided for convenience.
    Unknown { size: u64, mtime: f64 },
    /// NAS stat failed. Caller should log + fall through — freshness is
    /// an optimization hint, never a blocker.
    Error(std::io::Error),
}

/// Lazy freshness primitive: TTL-gated NAS stat against the cache.
///
/// If the entry was verified within `ttl_secs`, returns `Skipped` with no NAS
/// traffic. Otherwise stats `nas_path` and either stamps verification (match)
/// or updates cached metadata (drift). On stat failure returns `Error`; callers
/// should log and fall through to their normal path.
pub fn stat_and_refresh(
    cache: &MacosCache,
    relative_path: &str,
    nas_path: &Path,
    ttl_secs: f64,
) -> StatResult {
    let (cached_size, cached_mtime) = match cache.needs_verification(relative_path, ttl_secs) {
        Some(v) => v,
        None => {
            // Either within TTL or unknown. Disambiguate.
            if cache.is_known(relative_path) {
                return StatResult::Skipped;
            }
            return match std::fs::metadata(nas_path) {
                Ok(m) => StatResult::Unknown {
                    size: m.len(),
                    mtime: mtime_secs_f64(&m),
                },
                Err(e) => StatResult::Error(e),
            };
        }
    };

    let meta = match std::fs::metadata(nas_path) {
        Ok(m) => m,
        Err(e) => return StatResult::Error(e),
    };

    let nas_size = meta.len();
    let nas_mtime = mtime_secs_f64(&meta);

    // f64 equality is fine here — both values come from the same SystemTime
    // → Duration → f64 path. If a future platform introduces sub-nanosecond
    // jitter we'll want a tolerance, but mtime resolution on SMB is second-grained.
    if nas_size == cached_size && (nas_mtime - cached_mtime).abs() < 0.001 {
        cache.record_verification(relative_path);
        StatResult::Fresh {
            size: nas_size,
            mtime: nas_mtime,
        }
    } else {
        cache.update_nas_metadata(relative_path, nas_size, nas_mtime);
        StatResult::Drifted {
            size: nas_size,
            mtime: nas_mtime,
        }
    }
}

/// NFC-normalize a path or name for storage/lookup. All strings entering
/// the cache key space (from the NFS client AND from SMB enumerations)
/// funnel through this so one file has exactly one key regardless of
/// which side produced the name. Case folding is handled by the columns'
/// COLLATE NOCASE, not here — stored paths keep their on-disk case.
#[inline]
pub fn nfc(s: &str) -> String {
    use unicode_normalization::UnicodeNormalization;
    if s.is_ascii() {
        s.to_string()
    } else {
        s.nfc().collect()
    }
}

/// One-time rebuild of `known_files` + `visited_folders` with
/// COLLATE NOCASE path columns and NFC-normalized stored paths.
/// Preserves fh values (client NFS handles stay valid). Rows colliding
/// under the new key are duplicates of one physical file — the first
/// (lowest-fh) row wins, and the losers' hydration state is dropped
/// with them (their blobs are swept by the startup blob reconcile).
fn migrate_ci_paths(conn: &mut Connection) -> Result<(), String> {
    let tx = conn
        .transaction()
        .map_err(|e| format!("ci migration tx: {}", e))?;
    tx.execute_batch(
        "ALTER TABLE known_files RENAME TO kf_ci_old;
         ALTER TABLE visited_folders RENAME TO vf_ci_old;
         CREATE TABLE known_files (
             fh INTEGER PRIMARY KEY AUTOINCREMENT,
             path TEXT NOT NULL UNIQUE COLLATE NOCASE,
             name TEXT NOT NULL,
             is_dir INTEGER NOT NULL DEFAULT 0,
             nas_size INTEGER NOT NULL,
             nas_mtime REAL NOT NULL,
             nas_created REAL NOT NULL DEFAULT 0,
             is_hydrated INTEGER NOT NULL DEFAULT 0,
             hydrated_size INTEGER DEFAULT 0,
             last_accessed REAL DEFAULT 0,
             last_verified_at REAL DEFAULT 0,
             parent_path TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
             chunk_bitmap BLOB DEFAULT NULL
         );
         CREATE TABLE visited_folders (
             path TEXT PRIMARY KEY COLLATE NOCASE,
             folder_mtime REAL NOT NULL DEFAULT 0
         );",
    )
    .map_err(|e| format!("ci migration ddl: {}", e))?;

    {
        let rows: Vec<(i64, String, String, i64, i64, f64, f64, i64, i64, f64, f64, String, Option<Vec<u8>>)> = {
            let mut stmt = tx
                .prepare(
                    "SELECT fh, path, name, is_dir, nas_size, nas_mtime, nas_created,
                            is_hydrated, hydrated_size, last_accessed, last_verified_at,
                            parent_path, chunk_bitmap
                     FROM kf_ci_old ORDER BY fh ASC",
                )
                .map_err(|e| format!("ci migration select: {}", e))?;
            let mapped = stmt
                .query_map([], |r| {
                    Ok((
                        r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?,
                        r.get(5)?, r.get(6)?, r.get(7)?, r.get(8)?, r.get(9)?,
                        r.get(10)?, r.get(11)?, r.get(12)?,
                    ))
                })
                .map_err(|e| format!("ci migration query: {}", e))?;
            mapped.filter_map(|r| r.ok()).collect()
        };
        let mut ins = tx
            .prepare(
                "INSERT OR IGNORE INTO known_files
                     (fh, path, name, is_dir, nas_size, nas_mtime, nas_created,
                      is_hydrated, hydrated_size, last_accessed, last_verified_at,
                      parent_path, chunk_bitmap)
                 VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
            )
            .map_err(|e| format!("ci migration prep ins: {}", e))?;
        for (fh, path, name, is_dir, size, mtime, created, hyd, hsize, acc, ver, parent, bm) in rows {
            let _ = ins.execute(params![
                fh, nfc(&path), nfc(&name), is_dir, size, mtime, created,
                hyd, hsize, acc, ver, nfc(&parent), bm
            ]);
        }

        let folders: Vec<(String, f64)> = {
            let mut stmt = tx
                .prepare("SELECT path, folder_mtime FROM vf_ci_old")
                .map_err(|e| format!("ci migration vf select: {}", e))?;
            let mapped = stmt
                .query_map([], |r| Ok((r.get(0)?, r.get(1)?)))
                .map_err(|e| format!("ci migration vf query: {}", e))?;
            mapped.filter_map(|r| r.ok()).collect()
        };
        let mut vins = tx
            .prepare("INSERT OR IGNORE INTO visited_folders (path, folder_mtime) VALUES (?1, ?2)")
            .map_err(|e| format!("ci migration vf ins: {}", e))?;
        for (path, mtime) in folders {
            let _ = vins.execute(params![nfc(&path), mtime]);
        }
    }

    tx.execute_batch(
        "DROP TABLE kf_ci_old;
         DROP TABLE vf_ci_old;
         CREATE INDEX IF NOT EXISTS idx_hydrated ON known_files(is_hydrated);
         CREATE INDEX IF NOT EXISTS idx_accessed ON known_files(last_accessed);
         CREATE INDEX IF NOT EXISTS idx_parent_path ON known_files(parent_path);",
    )
    .map_err(|e| format!("ci migration cleanup: {}", e))?;
    tx.commit().map_err(|e| format!("ci migration commit: {}", e))
}

fn unix_now_f64() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs_f64()
}

fn mtime_secs_f64(meta: &std::fs::Metadata) -> f64 {
    meta.modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_cache_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "ufb-cache-test-{}-{}",
            tag,
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn entry(name: &str, size: u64) -> crate::messages::DirEntry {
        crate::messages::DirEntry {
            name: name.to_string(),
            is_dir: false,
            size,
            modified: 1000.0,
            created: 1000.0,
        }
    }

    #[test]
    fn nocase_lookup_hits_across_case_and_form() {
        let dir = temp_cache_dir("nocase");
        let cache =
            MacosCache::open("testshare", dir.clone(), 0, &dir).unwrap();

        cache.record_enumeration("", &[entry("Foo.MOV", 10)], false);
        // Same file, different case → same row, same fh.
        let fh_exact = cache.fh_for_path("Foo.MOV").expect("exact case");
        let fh_lower = cache.fh_for_path("foo.mov").expect("lower case");
        let fh_upper = cache.fh_for_path("FOO.mov").expect("upper case");
        assert_eq!(fh_exact, fh_lower);
        assert_eq!(fh_exact, fh_upper);

        // NFC-normalized accented name: lookup via the NFC form of an
        // NFD input must hit the row stored from an NFC enumeration.
        cache.record_enumeration("", &[entry("caf\u{e9}.mov", 5)], false);
        let nfd_input = "cafe\u{301}.mov"; // e + combining acute
        let looked_up = cache.fh_for_path(&nfc(nfd_input));
        assert!(looked_up.is_some(), "NFC-of-NFD lookup must hit");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn ci_migration_rebuilds_old_schema_and_dedupes() {
        let dir = temp_cache_dir("cimigrate");
        let db_path = dir.join("testshare.db");

        // Fabricate a PRE-NOCASE DB shaped like the previous schema,
        // with case-duplicate rows for one physical file.
        {
            let conn = Connection::open(&db_path).unwrap();
            conn.execute_batch(
                "CREATE TABLE known_files (
                     fh INTEGER PRIMARY KEY AUTOINCREMENT,
                     path TEXT NOT NULL UNIQUE,
                     name TEXT NOT NULL,
                     is_dir INTEGER NOT NULL DEFAULT 0,
                     nas_size INTEGER NOT NULL,
                     nas_mtime REAL NOT NULL,
                     nas_created REAL NOT NULL DEFAULT 0,
                     is_hydrated INTEGER NOT NULL DEFAULT 0,
                     hydrated_size INTEGER DEFAULT 0,
                     last_accessed REAL DEFAULT 0,
                     last_verified_at REAL DEFAULT 0,
                     parent_path TEXT NOT NULL DEFAULT '',
                     chunk_bitmap BLOB DEFAULT NULL
                 );
                 CREATE TABLE visited_folders (
                     path TEXT PRIMARY KEY,
                     folder_mtime REAL NOT NULL DEFAULT 0
                 );
                 INSERT INTO known_files (fh, path, name, is_dir, nas_size, nas_mtime, parent_path)
                 VALUES (1, '', '', 1, 0, 0, '');
                 INSERT INTO known_files (fh, path, name, is_dir, nas_size, nas_mtime, parent_path)
                 VALUES (7, 'jobs/Render.MOV', 'Render.MOV', 0, 100, 500, 'jobs');
                 INSERT INTO known_files (fh, path, name, is_dir, nas_size, nas_mtime, parent_path)
                 VALUES (9, 'jobs/render.mov', 'render.mov', 0, 100, 600, 'jobs');",
            )
            .unwrap();
        }

        let cache =
            MacosCache::open("testshare", dir.clone(), 0, &dir).unwrap();

        // Duplicates collapsed to the first (lowest-fh) row; both case
        // variants resolve to it.
        let fh = cache.fh_for_path("jobs/Render.MOV").expect("row survives");
        assert_eq!(fh, 7, "lowest-fh duplicate wins");
        assert_eq!(cache.fh_for_path("JOBS/RENDER.mov"), Some(7));

        // Root row intact.
        assert_eq!(cache.fh_for_path(""), Some(1));

        let _ = std::fs::remove_dir_all(&dir);
    }
}
