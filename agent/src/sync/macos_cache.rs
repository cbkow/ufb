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
use rusqlite::{params, Connection, TransactionBehavior};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

/// Escape a literal string for use as a SQLite `LIKE` pattern with
/// `ESCAPE '\'`. `%` and `_` are LIKE wildcards; unescaped, a folder
/// named `shot_010` also matched its sibling `shot-010`, so a rename or
/// prune of one re-pathed / deleted the other's rows (audit 2026-09-11
/// C-2). Every prefix match in this module MUST go through
/// `like_prefix` + `ESCAPE '\'`.
///
/// LIKE is ASCII-case-insensitive, which matches the NOCASE collation on
/// the path columns — that's intended (paths are case-insensitive keys).
pub fn like_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 4);
    for c in s.chars() {
        if matches!(c, '\\' | '%' | '_') {
            out.push('\\');
        }
        out.push(c);
    }
    out
}

/// `LIKE` pattern matching every string that starts with `prefix`
/// (literally — wildcards in `prefix` are escaped). Pair with
/// `LIKE ?n ESCAPE '\'` in the SQL.
pub fn like_prefix(prefix: &str) -> String {
    let mut out = like_escape(prefix);
    out.push('%');
    out
}

/// Bytes a chunk bitmap accounts for: set-bit count × chunk size, clamped
/// to the file size (the last chunk is usually short). Feeds the
/// `cached_bytes` column so the evictor can see partially-hydrated blobs
/// (audit 2026-09-11 C-6b).
pub fn bitmap_cached_bytes(bitmap: &[u8], file_size: u64) -> u64 {
    let set: u64 = bitmap.iter().map(|b| b.count_ones() as u64).sum();
    set.saturating_mul(CHUNK_SIZE).min(file_size)
}

/// OR `incoming` into `current` (growing `current` as needed). Used by
/// the bitmap persist so two concurrent readers that each fetched
/// different chunks don't overwrite each other's bits (audit 2026-09-11
/// C-10).
pub fn bitmap_or_merge(current: &mut Vec<u8>, incoming: &[u8]) {
    if current.len() < incoming.len() {
        current.resize(incoming.len(), 0);
    }
    for (dst, src) in current.iter_mut().zip(incoming.iter()) {
        *dst |= *src;
    }
}

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
                    chunk_bitmap BLOB DEFAULT NULL,
                    cached_bytes INTEGER NOT NULL DEFAULT 0
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

            // Bytes actually held on disk for this row — hydrated OR
            // partial. The evictor used to budget on `hydrated_size`
            // of `is_hydrated=1` rows only, so sparse partially-
            // hydrated blobs were invisible and unbounded (audit
            // 2026-09-11 C-6b). Additive migration; backfill from
            // hydrated_size / the bitmap popcount. Runs LAST so the
            // table-rewriting migrations above never have to carry it.
            let has_cached_bytes: bool = conn
                .prepare("SELECT cached_bytes FROM known_files LIMIT 0")
                .is_ok();
            if !has_cached_bytes {
                log::info!("[macos-cache] Migrating: adding cached_bytes column + backfilling");
                conn.execute_batch(
                    "ALTER TABLE known_files ADD COLUMN cached_bytes INTEGER NOT NULL DEFAULT 0;
                     UPDATE known_files SET cached_bytes = COALESCE(hydrated_size, 0)
                         WHERE is_hydrated = 1;",
                )
                .map_err(|e| format!("cached_bytes migration failed: {}", e))?;
                let partial: Vec<(i64, Vec<u8>, i64)> = conn
                    .prepare(
                        "SELECT fh, chunk_bitmap, nas_size FROM known_files
                         WHERE is_hydrated = 0 AND chunk_bitmap IS NOT NULL",
                    )
                    .and_then(|mut stmt| {
                        stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?
                            .collect::<Result<Vec<_>, _>>()
                    })
                    .unwrap_or_default();
                if let Ok(tx) = conn.transaction() {
                    if let Ok(mut upd) =
                        tx.prepare("UPDATE known_files SET cached_bytes = ?1 WHERE fh = ?2")
                    {
                        for (fh, bm, size) in partial {
                            let bytes = bitmap_cached_bytes(&bm, size.max(0) as u64);
                            let _ = upd.execute(params![bytes as i64, fh]);
                        }
                    }
                    let _ = tx.commit();
                }
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
        let Some(conn) = self.conn_or_warn("reconcile_blob_store") else {
            return;
        };

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
                "UPDATE known_files SET is_hydrated=0, hydrated_size=0, chunk_bitmap=NULL, cached_bytes=0;",
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
    ///
    /// Fallible on purpose: the old `expect("SQLite pool exhausted")`
    /// panicked inside an NFS handler when r2d2's 30s checkout timeout
    /// elapsed under burst I/O. A panicked handler never answers its
    /// RPC, and the hard loopback mount retransmits that request
    /// forever — a wedged Finder until reboot. Callers on the request
    /// path map `Err` to `NFS3ERR_JUKEBOX` (retry later); everything
    /// else degrades to "cold / unknown" and logs (audit 2026-09-11
    /// C-12).
    #[inline]
    fn conn(&self) -> Result<SqliteConn, String> {
        self.pool
            .get()
            .map_err(|e| format!("SQLite pool checkout failed: {}", e))
    }

    /// `conn()` for callers whose safe fallback is a default value —
    /// logs the failure with context so a dropped mutation is never
    /// silent (audit 2026-09-11 C-12).
    fn conn_or_warn(&self, what: &str) -> Option<SqliteConn> {
        match self.conn() {
            Ok(c) => Some(c),
            Err(e) => {
                log::warn!("[macos-cache] {}: {} skipped — {}", self.domain, what, e);
                None
            }
        }
    }

    /// Log a failed mutation instead of dropping it on the floor.
    /// SQLITE_BUSY past `busy_timeout` used to vanish into `let _ =`,
    /// leaving the index silently out of step with disk (audit
    /// 2026-09-11 C-12).
    fn log_exec(&self, what: &str, res: rusqlite::Result<usize>) {
        if let Err(e) = res {
            log::warn!("[macos-cache] {}: {} failed — {}", self.domain, what, e);
        }
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
    ///
    /// `folder_mtime` is the SMB folder's mtime, stat'd by the caller
    /// inside the same `spawn_blocking` that produced `entries`. This
    /// function does no NAS I/O of its own and the caller runs it on a
    /// blocking thread too — the SQLite transaction used to run inline
    /// on a tokio worker after the enumeration's spawn_blocking had
    /// returned (audit 2026-09-11 C-5b).
    pub fn record_enumeration(
        &self,
        relative_path: &str,
        entries: &[crate::messages::DirEntry],
        is_partial: bool,
        folder_mtime: f64,
    ) {
        // (path, fh, live_size, live_mtime) of rows holding cached bytes
        // (fully hydrated OR a partial bitmap) whose NAS size/mtime no
        // longer match the cache — their bytes are stale and must be
        // dropped before the new metadata is adopted.
        let mut drifted: Vec<(String, u64, u64, f64)> = Vec::new();
        // (fh, path) of rows the orphan prune deleted — their on-disk
        // cache files must go too, or the blobs leak until (never) and a
        // future fh could alias them.
        let mut pruned_files: Vec<(u64, String)> = Vec::new();
        let mut committed = false;

        {
            let Some(mut conn_guard) = self.conn_or_warn("record_enumeration") else {
                return;
            };
            let tx = match conn_guard.transaction() {
                Ok(t) => t,
                Err(e) => {
                    log::warn!("[macos-cache] record_enumeration tx begin failed: {}", e);
                    return;
                }
            };

            // Build set of current entry paths for deletion detection.
            let mut current_paths: HashSet<String> = HashSet::new();

            // Iterate entries: drift-check existing rows, upsert metadata.
            {
                let mut stmt_select = match tx.prepare_cached(
                    "SELECT nas_size, nas_mtime,
                            (is_hydrated != 0 OR chunk_bitmap IS NOT NULL), fh
                     FROM known_files WHERE path = ?1",
                ) {
                    Ok(s) => s,
                    Err(e) => {
                        log::warn!("[macos-cache] record_enumeration prepare select: {}", e);
                        return;
                    }
                };
                // `path = excluded.path` on conflict: with NOCASE keys a
                // conflict can be a case/form variant of the stored path —
                // adopt the enumerated (on-disk) form so the DB tracks
                // reality. fh is untouched, so client handles survive.
                let mut stmt_upsert = match tx.prepare_cached(
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
                ) {
                    Ok(s) => s,
                    Err(e) => {
                        log::warn!("[macos-cache] record_enumeration prepare upsert: {}", e);
                        return;
                    }
                };

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

                    let mut defer_metadata = false;
                    if !entry.is_dir {
                        let existing: Option<(i64, f64, i64, i64)> = stmt_select
                            .query_row(params![entry_path], |row| {
                                Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?))
                            })
                            .ok();
                        if let Some((cached_size, cached_mtime, has_bytes, fh)) = existing {
                            let drift = entry.size != cached_size as u64
                                || (entry.modified - cached_mtime).abs() > 0.001;
                            if drift && has_bytes != 0 {
                                drifted.push((
                                    entry_path.clone(),
                                    fh as u64,
                                    entry.size,
                                    entry.modified,
                                ));
                                // Do NOT adopt the new size/mtime yet. If
                                // the row already matched the NAS when the
                                // invalidation below got deferred (fh
                                // busy), every later stat said "Fresh" and
                                // the stale hydrated bytes were served
                                // until eviction. Keeping the OLD metadata
                                // until the blob is actually dropped means
                                // the next enumeration / TTL stat re-detects
                                // the drift (audit 2026-09-11 C-1).
                                defer_metadata = true;
                            }
                        }
                    }

                    if !defer_metadata {
                        self.log_exec(
                            "record_enumeration upsert",
                            stmt_upsert.execute(params![
                                entry_path,
                                entry.name,
                                entry.is_dir as i32,
                                entry.size,
                                entry.modified,
                                entry.created,
                                relative_path, // parent_path
                            ]),
                        );
                    }
                }
            }

            // Orphan detection + deletion — indexed equality lookup on
            // parent_path. The `path != parent_path` filter excludes the
            // self-reference case: the share root has path='' AND
            // parent_path='' (it's its own parent semantically), which
            // would otherwise flag root as an orphan and delete it
            // whenever we enumerate the share root.
            let orphans: Vec<(String, u64, String, f64, f64, bool)> = {
                let mut stmt_scan = match tx.prepare_cached(
                    "SELECT path, fh, name, nas_mtime, last_accessed, is_dir FROM known_files
                     WHERE parent_path = ?1 AND path != parent_path",
                ) {
                    Ok(s) => s,
                    Err(e) => {
                        log::warn!("[macos-cache] record_enumeration prepare scan: {}", e);
                        return;
                    }
                };
                stmt_scan
                    .query_map(params![relative_path], |row| {
                        Ok((
                            row.get::<_, String>(0)?,
                            row.get::<_, i64>(1)? as u64,
                            row.get::<_, String>(2)?,
                            row.get::<_, f64>(3)?,
                            row.get::<_, f64>(4).unwrap_or(0.0),
                            row.get::<_, i32>(5)? != 0,
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

            // Upsert visited folder. A partial or suspicious-empty
            // listing must NOT stamp the real folder mtime: readdir only
            // re-enumerates when the live mtime moves past the stored
            // one, so a listing that dropped entries would freeze the
            // folder at that incomplete view until something else
            // touched it. Stamp 0 instead — the next first-page READDIR
            // re-enumerates (audit 2026-09-11 C-6a).
            let stamp_mtime = if is_partial || suspicious_empty { 0.0 } else { folder_mtime };
            match tx.prepare_cached(
                "INSERT OR REPLACE INTO visited_folders (path, folder_mtime) VALUES (?1, ?2)",
            ) {
                Ok(mut stmt) => self.log_exec(
                    "record_enumeration visited_folders",
                    stmt.execute(params![relative_path, stamp_mtime]),
                ),
                Err(e) => log::warn!("[macos-cache] record_enumeration prepare vf: {}", e),
            }

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
                let (mut stmt_delete, mut stmt_desc_scan, mut stmt_desc_delete) = match (
                    tx.prepare_cached("DELETE FROM known_files WHERE path = ?1"),
                    // Descendant cascade for pruned directories: a folder
                    // deleted on the NAS took its subtree with it, but the
                    // subtree's rows have a different parent_path and never
                    // showed up as orphans of anything — the index grew
                    // ghost rows (and orphaned blobs) forever (audit
                    // 2026-09-11 C-13). Escaped prefix match (C-2).
                    tx.prepare_cached(
                        "SELECT fh, path FROM known_files WHERE path LIKE ?1 ESCAPE '\\'",
                    ),
                    tx.prepare_cached(
                        "DELETE FROM known_files WHERE path LIKE ?1 ESCAPE '\\'",
                    ),
                ) {
                    (Ok(a), Ok(b), Ok(c)) => (a, b, c),
                    _ => {
                        log::warn!("[macos-cache] record_enumeration prepare prune failed");
                        return;
                    }
                };
                for (path, fh, name, nas_mtime, last_accessed, is_dir) in &orphans {
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
                    match stmt_delete.execute(params![path]) {
                        Ok(_) => pruned_files.push((*fh, path.clone())),
                        Err(e) => {
                            log::warn!("[macos-cache] prune of {} failed — {}", path, e);
                            continue;
                        }
                    }
                    if *is_dir {
                        let pattern = like_prefix(&format!("{}/", path));
                        let desc: Vec<(u64, String)> = stmt_desc_scan
                            .query_map(params![pattern], |r| {
                                Ok((r.get::<_, i64>(0)? as u64, r.get::<_, String>(1)?))
                            })
                            .map(|rows| rows.filter_map(|r| r.ok()).collect())
                            .unwrap_or_default();
                        if !desc.is_empty() {
                            match stmt_desc_delete.execute(params![pattern]) {
                                Ok(_) => pruned_files.extend(desc),
                                Err(e) => log::warn!(
                                    "[macos-cache] cascade prune under {} failed — {}",
                                    path, e
                                ),
                            }
                        }
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
            // machine) while we hold cached bytes — the cache is now
            // WRONG, not just cold. Drop it, THEN adopt the new metadata
            // (order matters: metadata-first made the row look Fresh
            // while the blob was still stale). try_write: if a reader/
            // writer is mid-flight on the fh, skip this round — the row
            // keeps its old metadata, so the drift is re-detected by the
            // next enumeration or the read path's TTL stat, which
            // invalidates under a blocking write lock (audit 2026-09-11
            // C-1).
            let mut invalidated = 0usize;
            for (path, fh, live_size, live_mtime) in &drifted {
                let lock = self.fh_lock(*fh);
                let Ok(_guard) = lock.try_write() else {
                    log::debug!(
                        "[macos-cache] drift invalidation of {} deferred (fh busy) — \
                         metadata left stale so it is re-detected",
                        path
                    );
                    continue;
                };
                self.invalidate_cache(path, *fh);
                self.update_nas_metadata(path, *live_size, *live_mtime);
                invalidated += 1;
            }
            if !drifted.is_empty() {
                log::info!(
                    "[macos-cache] Enumeration drift: invalidated {}/{} stale entries under {:?}",
                    invalidated,
                    drifted.len(),
                    relative_path
                );
            }
        }
    }

    // Dead API removed (audit 2026-09-11 C-13): `get_changes_since`,
    // `remove_known_file`, `record_known_file`, `record_hydration`,
    // `compare_nas_metadata`, `folder_is_enumerated`, `is_known` — all
    // FileProvider-era entry points with zero callers since the NFS
    // backend replaced the extension. `ChangesResult` / `ChangedEntry`
    // went with them.

    // ── Hydration tracking + LRU eviction ──

    /// Update last_accessed time (called on each file read).
    pub fn touch(&self, relative_path: &str) {
        let now = unix_now_f64();
        let Some(conn) = self.conn_or_warn("touch") else { return };
        match conn.prepare_cached("UPDATE known_files SET last_accessed=?1 WHERE path=?2") {
            Ok(mut stmt) => self.log_exec("touch", stmt.execute(params![now, relative_path])),
            Err(e) => log::warn!("[macos-cache] touch prepare: {}", e),
        };
    }

    /// Stamp `last_verified_at = at`. `stat_and_refresh` uses `now` after
    /// a successful NAS stat confirms cached metadata matches reality,
    /// and a value slightly in the past after a FAILED stat so the next
    /// few reads don't each re-pay the SMB timeout (audit 2026-09-11
    /// C-15).
    fn stamp_verified_at(&self, relative_path: &str, at: f64) {
        let Some(conn) = self.conn_or_warn("stamp_verified_at") else { return };
        match conn.prepare_cached("UPDATE known_files SET last_verified_at=?1 WHERE path=?2") {
            Ok(mut stmt) => self.log_exec(
                "stamp_verified_at",
                stmt.execute(params![at, relative_path]),
            ),
            Err(e) => log::warn!("[macos-cache] stamp_verified_at prepare: {}", e),
        };
    }

    /// Update NAS metadata to fresh stat values and stamp last_verified_at.
    /// Preserves hydration state and last_accessed via targeted UPDATE.
    ///
    /// Ordering contract (audit 2026-09-11 C-1): when the row holds
    /// cached bytes and the new values differ, the caller MUST call
    /// `invalidate_cache` under the per-fh write lock BEFORE this —
    /// adopting the metadata first makes the row look Fresh over a
    /// stale blob.
    pub fn update_nas_metadata(&self, relative_path: &str, nas_size: u64, nas_mtime: f64) {
        let now = unix_now_f64();
        let Some(conn) = self.conn_or_warn("update_nas_metadata") else { return };
        match conn.prepare_cached(
            "UPDATE known_files SET nas_size=?1, nas_mtime=?2, last_verified_at=?3 WHERE path=?4",
        ) {
            Ok(mut stmt) => self.log_exec(
                "update_nas_metadata",
                stmt.execute(params![nas_size as i64, nas_mtime, now, relative_path]),
            ),
            Err(e) => log::warn!("[macos-cache] update_nas_metadata prepare: {}", e),
        };
    }

    /// Extend `hydrated_size` / `cached_bytes` of a fully-hydrated row
    /// after a write-through past its previous EOF. The blob really does
    /// hold every byte (the mirror landed and the file was complete
    /// before), so the accounting must follow — the evictor budgeted a
    /// grown file at its pre-write size forever (audit 2026-09-11 C-6b).
    /// No badge emission: the file was already Hydrated.
    pub fn extend_hydrated_size(&self, relative_path: &str, size: u64) {
        let Some(conn) = self.conn_or_warn("extend_hydrated_size") else { return };
        match conn.prepare_cached(
            "UPDATE known_files SET hydrated_size=?1, cached_bytes=?1
             WHERE path=?2 AND is_hydrated=1 AND hydrated_size < ?1",
        ) {
            Ok(mut stmt) => self.log_exec(
                "extend_hydrated_size",
                stmt.execute(params![size as i64, relative_path]),
            ),
            Err(e) => log::warn!("[macos-cache] extend_hydrated_size prepare: {}", e),
        };
    }

    /// Cached `(nas_size, nas_mtime, last_verified_at)` for a path, or
    /// `None` when the path is unknown. Single indexed read; the
    /// freshness decision is made by `stat_and_refresh`.
    fn verification_row(&self, relative_path: &str) -> Option<(u64, f64, f64)> {
        let conn = self.conn_or_warn("verification_row")?;
        let mut stmt = conn
            .prepare_cached(
                "SELECT nas_size, nas_mtime, last_verified_at FROM known_files WHERE path=?1",
            )
            .ok()?;
        stmt.query_row(params![relative_path], |row| {
            Ok((
                row.get::<_, i64>(0)? as u64,
                row.get::<_, f64>(1)?,
                row.get::<_, f64>(2)?,
            ))
        })
        .ok()
    }

    /// Total bytes held on disk by this domain's blobs — fully hydrated
    /// AND partially hydrated rows. Pre-C-6b this summed `hydrated_size`
    /// of `is_hydrated=1` rows only, so sparse partial blobs never
    /// counted against the budget.
    pub fn total_cached_bytes(&self) -> u64 {
        let Some(conn) = self.conn_or_warn("total_cached_bytes") else { return 0 };
        conn.prepare_cached(
            "SELECT COALESCE(SUM(cached_bytes), 0) FROM known_files
             WHERE is_hydrated=1 OR chunk_bitmap IS NOT NULL",
        )
        .ok()
        .and_then(|mut stmt| stmt.query_row([], |row| row.get::<_, i64>(0)).ok())
        .unwrap_or(0)
        .max(0) as u64
    }

    /// Flip a row to "nothing cached" (row-first, see callers) and
    /// return the on-disk blob path for the caller to unlink.
    fn clear_cached_row_by_fh(&self, fh: u64, what: &str) {
        let Some(conn) = self.conn_or_warn(what) else { return };
        match conn.prepare_cached(
            "UPDATE known_files
             SET is_hydrated=0, hydrated_size=0, chunk_bitmap=NULL, cached_bytes=0
             WHERE fh=?1",
        ) {
            Ok(mut stmt) => self.log_exec(what, stmt.execute(params![fh as i64])),
            Err(e) => log::warn!("[macos-cache] {} prepare: {}", what, e),
        };
    }

    /// LRU candidates for eviction / drain: `(fh, path, cached_bytes)` of
    /// every row holding bytes on disk — fully hydrated OR a partial
    /// bitmap (audit 2026-09-11 C-6b: partial blobs were unevictable).
    fn cached_candidates(&self, what: &str) -> Vec<(u64, String, u64)> {
        let Some(conn) = self.conn_or_warn(what) else { return Vec::new() };
        let mut stmt = match conn.prepare(
            "SELECT fh, path, cached_bytes FROM known_files
             WHERE (is_hydrated=1 OR chunk_bitmap IS NOT NULL) AND is_dir=0
             ORDER BY last_accessed ASC",
        ) {
            Ok(s) => s,
            Err(e) => {
                log::error!("[macos-cache] {} prepare failed: {}", what, e);
                return Vec::new();
            }
        };
        stmt.query_map([], |row| {
            Ok((
                row.get::<_, i64>(0)? as u64,
                row.get::<_, String>(1)?,
                row.get::<_, i64>(2)?.max(0) as u64,
            ))
        })
        .ok()
        .map(|it| it.filter_map(|r| r.ok()).collect())
        .unwrap_or_default()
    }

    /// Eviction: if cached bytes exceed `cache_limit`, delete
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
        let candidates = self.cached_candidates("eviction");

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
            self.clear_cached_row_by_fh(fh, "eviction row flip");
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
        let Some(conn) = self.conn_or_warn("get_chunk_bitmap") else {
            return Vec::new();
        };
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

    /// Persist chunk-bitmap bits a reader/writer just filled. Also advances
    /// `last_accessed` and recomputes `cached_bytes` (eviction accounting,
    /// C-6b).
    ///
    /// OR-merge, not replace: concurrent READs on one fh each hold the
    /// per-fh READ guard, snapshot the bitmap, fetch different chunks and
    /// used to write their whole snapshot back — the last writer erased
    /// every bit the others had set, so those chunks were re-fetched from
    /// SMB on the next read (and worse, their bytes sat unreferenced in
    /// the blob). The merge runs inside one `BEGIN IMMEDIATE` transaction,
    /// which serialises writers at the DB level across the whole pool —
    /// an in-process mutex could not cover a second connection (audit
    /// 2026-09-11 C-10).
    ///
    /// A row that became fully hydrated meanwhile is left alone: its
    /// bits are implied and re-adding a bitmap would invent a partial
    /// state. (Pre-existing, unchanged: a persist racing an
    /// `invalidate_cache` can resurrect bits over a deleted blob — the
    /// short-read demotion in `read_with_bitmap` self-heals that.)
    pub fn update_chunk_bitmap(&self, path: &str, bitmap: &[u8]) {
        let now = unix_now_f64();
        let Some(mut conn) = self.conn_or_warn("update_chunk_bitmap") else { return };
        let tx = match conn.transaction_with_behavior(TransactionBehavior::Immediate) {
            Ok(t) => t,
            Err(e) => {
                log::warn!("[macos-cache] update_chunk_bitmap tx begin failed: {}", e);
                return;
            }
        };
        let current: Option<(Option<Vec<u8>>, i64, i64)> = tx
            .query_row(
                "SELECT chunk_bitmap, nas_size, is_hydrated FROM known_files WHERE path = ?1",
                params![path],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .ok();
        let Some((existing, size, is_hydrated)) = current else { return };
        if is_hydrated != 0 {
            return;
        }
        let mut merged = existing.unwrap_or_default();
        bitmap_or_merge(&mut merged, bitmap);
        let cached_bytes = bitmap_cached_bytes(&merged, size.max(0) as u64);
        self.log_exec(
            "update_chunk_bitmap",
            tx.execute(
                "UPDATE known_files
                 SET chunk_bitmap = ?1, cached_bytes = ?2, last_accessed = ?3
                 WHERE path = ?4",
                params![merged, cached_bytes as i64, now, path],
            ),
        );
        if let Err(e) = tx.commit() {
            log::warn!("[macos-cache] update_chunk_bitmap commit failed: {}", e);
        }
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
        let Some(conn) = self.conn_or_warn("record_new_entry") else { return };
        match conn.prepare_cached(
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
        ) {
            Ok(mut stmt) => self.log_exec(
                "record_new_entry",
                stmt.execute(params![
                    relative_path,
                    name,
                    is_dir as i32,
                    size as i64,
                    mtime,
                    created,
                    parent,
                ]),
            ),
            Err(e) => log::warn!("[macos-cache] record_new_entry prepare: {}", e),
        };
    }

    /// Reflect an NFS RENAME in the cache. Preserves the source row's `fh`
    /// so NFS clients' cached handles keep resolving after the rename —
    /// critical for editor "save as" patterns that rename over the file.
    ///
    /// Caller must have performed `std::fs::rename(from_abs, to_abs)` first.
    /// This function does the DB-side bookkeeping:
    ///   1. Delete any stale rows at the target (the disk rename already
    ///      clobbered them) — EXCLUDING the source row and its descendants.
    ///      Under NOCASE keys a case-only rename (`Foo.mov` → `foo.mov`)
    ///      makes the target query match the SOURCE row: the old code
    ///      deleted the source row + blob before the UPDATE, so the client's
    ///      fh went STALE and a case-only folder rename wiped its whole
    ///      subtree (audit 2026-09-11 C-4b).
    ///   2. UPDATE the source row's path/parent_path/name in place so `fh`
    ///      survives.
    ///   3. Fix up every descendant row (when renaming a directory) —
    ///      their path prefix changes, their `fh` stays.
    ///
    /// All prefix matches use `like_prefix` + `ESCAPE '\'` — unescaped,
    /// `shot_010` matched sibling `shot-010` (audit 2026-09-11 C-2).
    pub fn rename_path(&self, from: &str, to: &str) -> Result<(), String> {
        let mut conn = self.conn()?;
        let tx = conn
            .transaction()
            .map_err(|e| format!("rename tx begin: {}", e))?;

        // The source may be absent from the index (pruned, or renamed
        // before it was ever listed). The disk rename already happened,
        // so that is NOT an error — there is simply no row to carry
        // over; `-1` never matches a real fh, so the exclusions below
        // become no-ops and the target rows are cleared as usual.
        let src_fh: i64 = match tx.query_row(
            "SELECT fh FROM known_files WHERE path = ?1",
            params![from],
            |r| r.get(0),
        ) {
            Ok(fh) => fh,
            Err(rusqlite::Error::QueryReturnedNoRows) => -1,
            Err(e) => return Err(format!("rename source lookup {:?}: {}", from, e)),
        };
        let from_subtree = like_prefix(&format!("{}/", from));
        let to_subtree = like_prefix(&format!("{}/", to));

        // Clear anything under the target path. The disk rename moved the
        // actual files to `to`, so whatever was previously there is gone.
        // Collect the doomed rows' fhs first — their on-disk cache blobs
        // must be removed too (rename-over-existing is the editor
        // atomic-save pattern; leaking a blob per save adds up).
        let clobbered_fhs: Vec<i64> = {
            let mut stmt = tx
                .prepare(
                    "SELECT fh FROM known_files
                     WHERE (path = ?1 OR path LIKE ?2 ESCAPE '\\')
                       AND fh != ?3
                       AND NOT (path LIKE ?4 ESCAPE '\\')",
                )
                .map_err(|e| format!("rename scan target: {}", e))?;
            let rows = stmt
                .query_map(params![to, to_subtree, src_fh, from_subtree], |row| {
                    row.get::<_, i64>(0)
                })
                .map_err(|e| format!("rename query target: {}", e))?;
            rows.filter_map(|r| r.ok()).collect()
        };
        if !clobbered_fhs.is_empty() {
            // Delete by fh (the exact set we just scanned) rather than
            // re-running the path predicate — keeps the "never the
            // source subtree" guarantee in one place.
            let mut del = tx
                .prepare("DELETE FROM known_files WHERE fh = ?1")
                .map_err(|e| format!("rename clear target prep: {}", e))?;
            for fh in &clobbered_fhs {
                del.execute(params![fh])
                    .map_err(|e| format!("rename clear target: {}", e))?;
            }
        }

        let new_parent = parent_of(to).to_string();
        let new_name = to.rsplit('/').next().unwrap_or(to).to_string();
        tx.execute(
            "UPDATE known_files SET path = ?1, parent_path = ?2, name = ?3
             WHERE fh = ?4",
            params![to, new_parent, new_name, src_fh],
        )
        .map_err(|e| format!("rename update source: {}", e))?;

        // Descendant fixup for directory renames. Collect first (borrow
        // ends with the prepare), then apply UPDATEs. The stored prefix
        // may differ from `from` in ASCII case (NOCASE key) but never in
        // byte length, so slicing by `from_prefix.len()` is safe.
        let from_prefix = format!("{}/", from);
        let to_prefix = format!("{}/", to);
        let descendants: Vec<(i64, String)> = {
            let mut stmt = tx
                .prepare("SELECT fh, path FROM known_files WHERE path LIKE ?1 ESCAPE '\\'")
                .map_err(|e| format!("rename prep desc: {}", e))?;
            let rows = stmt
                .query_map(params![from_subtree], |row| {
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
            if old_desc_path.len() < from_prefix.len() {
                continue;
            }
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
        if let Some(conn) = self.conn_or_warn("forget_path") {
            match conn.prepare_cached("DELETE FROM known_files WHERE path = ?1") {
                Ok(mut stmt) => self.log_exec("forget_path", stmt.execute(params![path])),
                Err(e) => log::warn!("[macos-cache] forget_path prepare: {}", e),
            }
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
        if let Some(conn) = self.conn_or_warn("invalidate_cache") {
            match conn.prepare_cached(
                "UPDATE known_files
                 SET is_hydrated = 0, hydrated_size = 0, chunk_bitmap = NULL, cached_bytes = 0
                 WHERE path = ?1",
            ) {
                Ok(mut stmt) => {
                    self.log_exec("invalidate_cache", stmt.execute(params![path]))
                }
                Err(e) => log::warn!("[macos-cache] invalidate_cache prepare: {}", e),
            }
        }
        let cache_path = self.cache_file_path(fh);
        let _ = std::fs::remove_file(&cache_path);
        self.emit_badge(path, crate::messages::BadgeKind::Uncached);
    }

    /// Mark a file as fully hydrated (all chunks cached). Nulls the bitmap
    /// since `is_hydrated=1` is the fast-path shortcut.
    pub fn mark_fully_hydrated(&self, path: &str, size: u64) {
        let now = unix_now_f64();
        if let Some(conn) = self.conn_or_warn("mark_fully_hydrated") {
            match conn.prepare_cached(
                "UPDATE known_files
                 SET is_hydrated = 1,
                     hydrated_size = ?1,
                     cached_bytes = ?1,
                     chunk_bitmap = NULL,
                     last_accessed = ?2
                 WHERE path = ?3",
            ) {
                Ok(mut stmt) => self.log_exec(
                    "mark_fully_hydrated",
                    stmt.execute(params![size as i64, now, path]),
                ),
                Err(e) => log::warn!("[macos-cache] mark_fully_hydrated prepare: {}", e),
            }
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
        let conn = self.conn_or_warn("fh_for_path")?;
        let mut stmt = conn.prepare_cached("SELECT fh FROM known_files WHERE path = ?1").ok()?;
        stmt.query_row(params![path], |row| row.get::<_, i64>(0))
            .ok()
            .map(|v| v as u64)
    }

    /// Diagnostic: total number of known_files rows (one per cached path).
    pub fn nfs_handles_count(&self) -> i64 {
        let Some(conn) = self.conn_or_warn("nfs_handles_count") else { return -1 };
        conn.prepare_cached("SELECT COUNT(*) FROM known_files")
            .ok()
            .and_then(|mut stmt| stmt.query_row([], |row| row.get(0)).ok())
            .unwrap_or(-1)
    }

    /// Reverse lookup: path for an NFS file handle.
    ///
    /// `Err` is a DB-layer failure (pool exhausted, SQLITE_BUSY past the
    /// timeout) and must NOT be conflated with "no such row": the NFS
    /// handler answers `Ok(None)` with STALE (the client discards its
    /// handle for good) but `Err` with JUKEBOX (retry shortly) — audit
    /// 2026-09-11 C-12.
    pub fn path_for_fh(&self, fh: u64) -> Result<Option<String>, String> {
        let conn = self.conn()?;
        let mut stmt = conn
            .prepare_cached("SELECT path FROM known_files WHERE fh = ?1")
            .map_err(|e| format!("path_for_fh prepare: {}", e))?;
        match stmt.query_row(params![fh as i64], |row| row.get::<_, String>(0)) {
            Ok(p) => Ok(Some(p)),
            Err(rusqlite::Error::QueryReturnedNoRows) => Ok(None),
            Err(e) => Err(format!("path_for_fh query: {}", e)),
        }
    }

    /// Ensure a handle exists for `path`, returning its fh. On first insert
    /// AUTOINCREMENT assigns an `fh`; on conflict the row is left untouched.
    /// Used for the share root at server startup (so fh=1 is always reserved).
    pub fn ensure_fh(&self, path: &str) -> Option<u64> {
        {
            let conn = self.conn_or_warn("ensure_fh")?;
            let parent = parent_of(path).to_string();
            let mut stmt = conn
                .prepare_cached(
                    "INSERT INTO known_files
                         (path, name, is_dir, nas_size, nas_mtime, parent_path)
                     VALUES (?1, ?2, 1, 0, 0, ?3)
                     ON CONFLICT(path) DO NOTHING",
                )
                .ok()?;
            self.log_exec("ensure_fh", stmt.execute(params![path, path, parent]));
        }
        self.fh_for_path(path)
    }

    /// Cached metadata for a given path — fields an NFS GETATTR needs. Returns
    /// `None` if the path isn't in `known_files` (cold — caller must populate).
    pub fn cached_attr(&self, path: &str) -> Option<CachedAttr> {
        let conn = self.conn_or_warn("cached_attr")?;
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
        let Some(conn) = self.conn_or_warn("cached_children") else { return Vec::new() };
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

    /// The SMB folder mtime we stored last time we enumerated this
    /// folder. `None` if the folder has never been visited. Paired with
    /// `folder_needs_reenum` for logging the cached-vs-live comparison.
    pub fn cached_folder_mtime(&self, path: &str) -> Option<f64> {
        let conn = self.conn_or_warn("cached_folder_mtime")?;
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
    /// time) so subsequent comparisons here are meaningful — and stores
    /// 0 for a partial / suspicious-empty listing, which any real live
    /// mtime beats, forcing a retry (C-6a). Mirror of
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

    /// NFS-native "drain all": delete every cache blob for this share
    /// (fully AND partially hydrated), flip their DB rows to uncached, and
    /// return (count, bytes). Uses non-blocking `try_write` on each fh —
    /// files with active readers are skipped (user retries; rare).
    /// Intended to back the UI "Drain Cache" button under NFS mode.
    pub async fn drain_all(&self) -> (u64, u64) {
        let candidates = self.cached_candidates("drain_all");

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
            self.clear_cached_row_by_fh(fh, "drain_all row flip");
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

    /// Cheap single-query stats for the UI: (cached_bytes, hydrated_count).
    /// Bytes cover partial blobs too — that's the real disk footprint the
    /// budget is enforced against (C-6b); the count stays "fully
    /// hydrated files" since that's what the badge/UI vocabulary means.
    pub fn cache_stats(&self) -> (u64, u64) {
        let Some(conn) = self.conn_or_warn("cache_stats") else { return (0, 0) };
        let row: Option<(i64, i64)> = conn
            .prepare_cached(
                "SELECT
                    COALESCE(SUM(cached_bytes), 0),
                    COALESCE(SUM(is_hydrated = 1), 0)
                 FROM known_files
                 WHERE (is_hydrated=1 OR chunk_bitmap IS NOT NULL) AND is_dir=0",
            )
            .ok()
            .and_then(|mut stmt| {
                stmt.query_row([], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?)))
                    .ok()
            });
        row.map(|(b, c)| (b.max(0) as u64, c.max(0) as u64)).unwrap_or((0, 0))
    }

    /// Test/diagnostic accessor: `(is_hydrated, has_bitmap, cached_bytes,
    /// nas_size, nas_mtime)` for a path.
    #[cfg(test)]
    fn row_state(&self, path: &str) -> Option<(bool, bool, u64, u64, f64)> {
        let conn = self.conn().ok()?;
        conn.query_row(
            "SELECT is_hydrated, chunk_bitmap IS NOT NULL, cached_bytes, nas_size, nas_mtime
             FROM known_files WHERE path = ?1",
            params![path],
            |r| {
                Ok((
                    r.get::<_, i32>(0)? != 0,
                    r.get::<_, i32>(1)? != 0,
                    r.get::<_, i64>(2)? as u64,
                    r.get::<_, i64>(3)? as u64,
                    r.get::<_, f64>(4)?,
                ))
            },
        )
        .ok()
    }
}

/// Result of a stat-and-refresh call against the NAS.
#[derive(Debug)]
pub enum StatResult {
    /// Within TTL — no stat was performed. Caller can serve cached data.
    Skipped,
    /// NAS was stat'd; matched cached metadata. `last_verified_at` was refreshed.
    Fresh { size: u64, mtime: f64 },
    /// NAS was stat'd; drifted from cache. The DB row is NOT updated —
    /// the caller must invalidate cached bytes under the per-fh write
    /// lock and only then adopt `size`/`mtime` via `update_nas_metadata`
    /// (audit 2026-09-11 C-1). Until it does, the row keeps its old
    /// metadata so the drift is re-detected on the next stat.
    Drifted { size: u64, mtime: f64 },
    /// Path unknown to cache — caller should let the normal read path populate
    /// a baseline. Fresh stat values provided for convenience.
    Unknown { size: u64, mtime: f64 },
    /// NAS stat failed. Caller should log + fall through — freshness is
    /// an optimization hint, never a blocker.
    Error(std::io::Error),
}

/// After a failed NAS stat, how long the next reads skip re-stat'ing.
/// Short: the error is usually a transient SMB hiccup, but paying the
/// full SMB timeout on EVERY read while the heartbeat still says online
/// froze the reader for the whole outage (audit 2026-09-11 C-15).
const STAT_ERROR_NEGATIVE_TTL_SECS: f64 = 2.0;

/// Lazy freshness primitive: TTL-gated NAS stat against the cache.
///
/// If the entry was verified within `ttl_secs`, returns `Skipped` with no NAS
/// traffic. Otherwise stats `nas_path` and either stamps verification (match)
/// or reports drift WITHOUT touching the row (see `StatResult::Drifted`).
/// On stat failure returns `Error` after stamping a short negative TTL;
/// callers should log and fall through to their normal path.
pub fn stat_and_refresh(
    cache: &MacosCache,
    relative_path: &str,
    nas_path: &Path,
    ttl_secs: f64,
) -> StatResult {
    let now = unix_now_f64();
    let Some((cached_size, cached_mtime, verified_at)) = cache.verification_row(relative_path)
    else {
        return match std::fs::metadata(nas_path) {
            Ok(m) => StatResult::Unknown {
                size: m.len(),
                mtime: mtime_secs_f64(&m),
            },
            Err(e) => StatResult::Error(e),
        };
    };
    if now - verified_at < ttl_secs {
        return StatResult::Skipped;
    }

    let meta = match std::fs::metadata(nas_path) {
        Ok(m) => m,
        Err(e) => {
            // Negative TTL: pretend we verified (ttl - NEG) seconds ago
            // so only NEG seconds of reads are spared, not a full TTL.
            let at = (now - ttl_secs + STAT_ERROR_NEGATIVE_TTL_SECS).min(now);
            cache.stamp_verified_at(relative_path, at);
            return StatResult::Error(e);
        }
    };

    let nas_size = meta.len();
    let nas_mtime = mtime_secs_f64(&meta);

    // f64 equality is fine here — both values come from the same SystemTime
    // → Duration → f64 path. If a future platform introduces sub-nanosecond
    // jitter we'll want a tolerance, but mtime resolution on SMB is second-grained.
    if nas_size == cached_size && (nas_mtime - cached_mtime).abs() < 0.001 {
        cache.stamp_verified_at(relative_path, now);
        StatResult::Fresh {
            size: nas_size,
            mtime: nas_mtime,
        }
    } else {
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

        cache.record_enumeration("", &[entry("Foo.MOV", 10)], false, 1.0);
        // Same file, different case → same row, same fh.
        let fh_exact = cache.fh_for_path("Foo.MOV").expect("exact case");
        let fh_lower = cache.fh_for_path("foo.mov").expect("lower case");
        let fh_upper = cache.fh_for_path("FOO.mov").expect("upper case");
        assert_eq!(fh_exact, fh_lower);
        assert_eq!(fh_exact, fh_upper);

        // NFC-normalized accented name: lookup via the NFC form of an
        // NFD input must hit the row stored from an NFC enumeration.
        cache.record_enumeration("", &[entry("caf\u{e9}.mov", 5)], false, 1.0);
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

    fn dir_entry(name: &str) -> crate::messages::DirEntry {
        crate::messages::DirEntry {
            name: name.to_string(),
            is_dir: true,
            size: 0,
            modified: 1000.0,
            created: 1000.0,
        }
    }

    fn open_cache(tag: &str, limit: u64) -> (MacosCache, PathBuf) {
        let dir = temp_cache_dir(tag);
        let cache = MacosCache::open("testshare", dir.clone(), limit, &dir).unwrap();
        (cache, dir)
    }

    // ── C-2: LIKE escaping ──

    #[test]
    fn like_helpers_escape_wildcards() {
        assert_eq!(like_escape("shot_010/50%"), "shot\\_010/50\\%");
        assert_eq!(like_escape("a\\b"), "a\\\\b");
        assert_eq!(like_prefix("shot_010/"), "shot\\_010/%");
    }

    #[test]
    fn rename_dir_does_not_touch_wildcard_siblings() {
        let (cache, dir) = open_cache("likesib", 0);
        cache.record_enumeration(
            "",
            &[dir_entry("shot_010"), dir_entry("shot-010"), dir_entry("shot_010_old")],
            false,
            1.0,
        );
        cache.record_enumeration("shot_010", &[entry("a.mov", 1)], false, 1.0);
        cache.record_enumeration("shot-010", &[entry("b.mov", 1)], false, 1.0);
        cache.record_enumeration("shot_010_old", &[entry("c.mov", 1)], false, 1.0);
        let fh_b = cache.fh_for_path("shot-010/b.mov").unwrap();
        let fh_c = cache.fh_for_path("shot_010_old/c.mov").unwrap();
        let fh_a = cache.fh_for_path("shot_010/a.mov").unwrap();
        let fh_dir = cache.fh_for_path("shot_010").unwrap();

        // Rename shot_010 → shot_020: with an unescaped LIKE, `shot_010/%`
        // also matched `shot-010/…` (and `shot_010_old/…` under the
        // clobber query for a target prefix), re-pathing sibling rows.
        cache.rename_path("shot_010", "shot_020").unwrap();

        assert_eq!(cache.fh_for_path("shot_020"), Some(fh_dir));
        assert_eq!(cache.fh_for_path("shot_020/a.mov"), Some(fh_a));
        assert_eq!(cache.fh_for_path("shot-010/b.mov"), Some(fh_b), "sibling untouched");
        assert_eq!(cache.fh_for_path("shot_010_old/c.mov"), Some(fh_c), "sibling untouched");
        assert!(cache.fh_for_path("shot_010/a.mov").is_none());

        // Rename INTO the now-free `shot_010`: the target clobber scan
        // `shot_010/%` unescaped matches `shot-010/b.mov` and would delete
        // the sibling's row + blob.
        cache.record_enumeration(
            "",
            &[dir_entry("shot_020"), dir_entry("shot-010"), dir_entry("shot_010_old"), dir_entry("tmp")],
            false,
            2.0,
        );
        cache.rename_path("tmp", "shot_010").unwrap();
        assert_eq!(cache.fh_for_path("shot_020/a.mov"), Some(fh_a));
        assert_eq!(cache.fh_for_path("shot-010/b.mov"), Some(fh_b), "sibling not clobbered");
        assert_eq!(cache.fh_for_path("shot_010_old/c.mov"), Some(fh_c));

        // Source missing from the index (pruned): disk rename already
        // happened, so this must be Ok and still clear the target rows.
        cache.rename_path("never-indexed", "shot-010").unwrap();
        assert!(cache.fh_for_path("shot-010/b.mov").is_none(), "target subtree cleared");

        let _ = std::fs::remove_dir_all(&dir);
    }

    // ── C-4b: case-only rename ──

    #[test]
    fn case_only_rename_keeps_fh_and_descendants() {
        let (cache, dir) = open_cache("caserename", 0);
        cache.record_enumeration("", &[dir_entry("Shots"), entry("Foo.mov", 3)], false, 1.0);
        cache.record_enumeration("Shots", &[entry("x.mov", 1)], false, 1.0);
        let fh_file = cache.fh_for_path("Foo.mov").unwrap();
        let fh_dir = cache.fh_for_path("Shots").unwrap();
        let fh_child = cache.fh_for_path("Shots/x.mov").unwrap();
        // Pretend the file is hydrated with a blob on disk.
        cache.mark_fully_hydrated("Foo.mov", 3);
        std::fs::write(cache.cache_file_path(fh_file), b"abc").unwrap();

        cache.rename_path("Foo.mov", "foo.mov").unwrap();
        assert_eq!(cache.fh_for_path("foo.mov"), Some(fh_file), "fh survives");
        let (hyd, _, cached, _, _) = cache.row_state("foo.mov").unwrap();
        assert!(hyd && cached == 3, "hydration state survives");
        assert!(cache.cache_file_path(fh_file).exists(), "blob not clobbered");
        assert_eq!(
            cache.cached_attr("foo.mov").map(|a| a.size),
            Some(3)
        );
        // Stored case follows the rename.
        let children = cache.cached_children("");
        assert!(children.iter().any(|(_, n, _)| n == "foo.mov"));

        cache.rename_path("Shots", "shots").unwrap();
        assert_eq!(cache.fh_for_path("shots"), Some(fh_dir));
        assert_eq!(cache.fh_for_path("shots/x.mov"), Some(fh_child), "descendants survive");
        assert_eq!(cache.cached_children("shots").len(), 1);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn rename_over_existing_clobbers_only_the_target() {
        let (cache, dir) = open_cache("renameover", 0);
        cache.record_enumeration("", &[entry("a.tmp", 5), entry("a.txt", 9)], false, 1.0);
        let fh_src = cache.fh_for_path("a.tmp").unwrap();
        let fh_old = cache.fh_for_path("a.txt").unwrap();
        cache.mark_fully_hydrated("a.txt", 9);
        std::fs::write(cache.cache_file_path(fh_old), b"oldoldold").unwrap();

        cache.rename_path("a.tmp", "a.txt").unwrap();
        assert_eq!(cache.fh_for_path("a.txt"), Some(fh_src), "source fh wins");
        assert!(cache.path_for_fh(fh_old).unwrap().is_none(), "old target row gone");
        assert!(!cache.cache_file_path(fh_old).exists(), "old target blob gone");

        let _ = std::fs::remove_dir_all(&dir);
    }

    // ── C-1: drift never adopts metadata before the blob is dropped ──

    #[test]
    fn enumeration_drift_defers_metadata_while_fh_busy() {
        let (cache, dir) = open_cache("driftdefer", 0);
        cache.record_enumeration("", &[entry("r.mov", 10)], false, 1.0);
        let fh = cache.fh_for_path("r.mov").unwrap();
        cache.mark_fully_hydrated("r.mov", 10);
        std::fs::write(cache.cache_file_path(fh), vec![1u8; 10]).unwrap();

        let mut changed = entry("r.mov", 20);
        changed.modified = 2000.0;

        // A reader holds the per-fh guard → try_write fails → the row
        // must keep its OLD metadata so the drift is re-detected.
        let lock = cache.fh_lock(fh);
        let guard = lock.try_read().unwrap();
        cache.record_enumeration("", std::slice::from_ref(&changed), false, 2.0);
        let (hyd, _, cached, size, mtime) = cache.row_state("r.mov").unwrap();
        assert!(hyd, "still hydrated (invalidation deferred)");
        assert_eq!((cached, size, mtime), (10, 10, 1000.0), "old metadata kept");
        assert!(cache.cache_file_path(fh).exists());
        drop(guard);

        // Guard released → next enumeration invalidates THEN adopts.
        cache.record_enumeration("", std::slice::from_ref(&changed), false, 3.0);
        let (hyd, bm, cached, size, mtime) = cache.row_state("r.mov").unwrap();
        assert!(!hyd && !bm && cached == 0, "blob dropped");
        assert_eq!((size, mtime), (20, 2000.0), "metadata adopted after invalidate");
        assert!(!cache.cache_file_path(fh).exists());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn enumeration_drift_covers_partially_hydrated_rows() {
        let (cache, dir) = open_cache("driftpartial", 0);
        cache.record_enumeration("", &[entry("p.mov", 3 * CHUNK_SIZE)], false, 1.0);
        let fh = cache.fh_for_path("p.mov").unwrap();
        let mut bm = Vec::new();
        set_bit(&mut bm, 1);
        cache.update_chunk_bitmap("p.mov", &bm);
        std::fs::write(cache.cache_file_path(fh), b"x").unwrap();
        let (_, has_bm, cached, _, _) = cache.row_state("p.mov").unwrap();
        assert!(has_bm && cached == CHUNK_SIZE);

        let mut changed = entry("p.mov", 3 * CHUNK_SIZE);
        changed.modified = 5000.0;
        cache.record_enumeration("", std::slice::from_ref(&changed), false, 2.0);
        let (_, has_bm, cached, _, mtime) = cache.row_state("p.mov").unwrap();
        assert!(!has_bm && cached == 0, "partial bitmap dropped on drift");
        assert_eq!(mtime, 5000.0);
        assert!(!cache.cache_file_path(fh).exists());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn stat_and_refresh_drifted_leaves_row_untouched() {
        let (cache, dir) = open_cache("statdrift", 0);
        let nas = dir.join("nas");
        std::fs::create_dir_all(&nas).unwrap();
        let file = nas.join("s.bin");
        std::fs::write(&file, vec![0u8; 20]).unwrap();
        cache.record_enumeration("", &[entry("s.bin", 10)], false, 1.0);

        match stat_and_refresh(&cache, "s.bin", &file, 5.0) {
            StatResult::Drifted { size, .. } => assert_eq!(size, 20),
            other => panic!("expected Drifted, got {:?}", other),
        }
        let (_, _, _, size, _) = cache.row_state("s.bin").unwrap();
        assert_eq!(size, 10, "Drifted must not adopt metadata (C-1)");
        // And it keeps reporting drift until the caller adopts.
        assert!(matches!(
            stat_and_refresh(&cache, "s.bin", &file, 5.0),
            StatResult::Drifted { .. }
        ));
        let live_mtime = mtime_secs_f64(&std::fs::metadata(&file).unwrap());
        cache.update_nas_metadata("s.bin", 20, live_mtime);
        assert!(matches!(
            stat_and_refresh(&cache, "s.bin", &file, 0.0),
            StatResult::Fresh { .. }
        ));

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn stat_and_refresh_error_stamps_negative_ttl() {
        let (cache, dir) = open_cache("statneg", 0);
        cache.record_enumeration("", &[entry("gone.bin", 10)], false, 1.0);
        let missing = dir.join("nope").join("gone.bin");
        assert!(matches!(
            stat_and_refresh(&cache, "gone.bin", &missing, 5.0),
            StatResult::Error(_)
        ));
        // Within the negative TTL the stat is skipped (C-15) …
        assert!(matches!(
            stat_and_refresh(&cache, "gone.bin", &missing, 5.0),
            StatResult::Skipped
        ));
        // … but not for a whole TTL: with ttl=0 the window is closed.
        assert!(matches!(
            stat_and_refresh(&cache, "gone.bin", &missing, 0.0),
            StatResult::Error(_)
        ));
        let _ = std::fs::remove_dir_all(&dir);
    }

    // ── C-6a: partial / suspicious-empty listings force a re-enum ──

    #[test]
    fn partial_listing_does_not_freeze_folder_mtime() {
        let (cache, dir) = open_cache("partialmtime", 0);
        cache.record_enumeration("", &[entry("a", 1), entry("b", 1)], false, 500.0);
        assert!(!cache.folder_needs_reenum("", 500.0));

        cache.record_enumeration("", &[entry("a", 1)], true, 600.0);
        assert_eq!(cache.cached_folder_mtime(""), Some(0.0));
        assert!(cache.folder_needs_reenum("", 600.0), "partial → retry");
        assert!(cache.fh_for_path("b").is_some(), "partial never prunes");

        cache.record_enumeration("", &[entry("a", 1), entry("b", 1)], false, 700.0);
        assert!(!cache.folder_needs_reenum("", 700.0));
        cache.record_enumeration("", &[], false, 800.0);
        assert_eq!(cache.cached_folder_mtime(""), Some(0.0), "suspicious empty → retry");
        assert!(cache.fh_for_path("a").is_some());

        let _ = std::fs::remove_dir_all(&dir);
    }

    // ── C-13: pruning a directory cascades to its subtree ──

    #[test]
    fn prune_of_directory_cascades_to_descendants() {
        let (cache, dir) = open_cache("cascade", 0);
        cache.record_enumeration("", &[dir_entry("gone"), entry("keep", 1)], false, 1.0);
        cache.record_enumeration("gone", &[dir_entry("deep"), entry("f.mov", 4)], false, 1.0);
        cache.record_enumeration("gone/deep", &[entry("g.mov", 4)], false, 1.0);
        let fh_f = cache.fh_for_path("gone/f.mov").unwrap();
        let fh_g = cache.fh_for_path("gone/deep/g.mov").unwrap();
        cache.mark_fully_hydrated("gone/deep/g.mov", 4);
        std::fs::write(cache.cache_file_path(fh_g), b"gggg").unwrap();

        // `gone` vanished from the NAS listing (old mtime, never
        // accessed → outside the prune grace window).
        cache.record_enumeration("", &[entry("keep", 1)], false, 2.0);
        assert!(cache.fh_for_path("gone").is_none());
        assert!(cache.path_for_fh(fh_f).unwrap().is_none(), "child row cascaded");
        assert!(cache.path_for_fh(fh_g).unwrap().is_none(), "grandchild row cascaded");
        assert!(!cache.cache_file_path(fh_g).exists(), "descendant blob removed");
        assert!(cache.fh_for_path("keep").is_some());

        let _ = std::fs::remove_dir_all(&dir);
    }

    // ── C-6b / C-10: cached_bytes accounting + OR-merged bitmaps ──

    #[test]
    fn bitmap_helpers() {
        assert_eq!(bitmap_cached_bytes(&[], 5), 0);
        assert_eq!(bitmap_cached_bytes(&[0b101], 10 * CHUNK_SIZE), 2 * CHUNK_SIZE);
        // Clamped to the file size (short last chunk).
        assert_eq!(bitmap_cached_bytes(&[0b11], CHUNK_SIZE + 7), CHUNK_SIZE + 7);

        let mut cur = vec![0b0001];
        bitmap_or_merge(&mut cur, &[0b0100, 0b1000]);
        assert_eq!(cur, vec![0b0101, 0b1000]);
        bitmap_or_merge(&mut cur, &[]);
        assert_eq!(cur, vec![0b0101, 0b1000]);
    }

    #[test]
    fn update_chunk_bitmap_merges_and_accounts() {
        let (cache, dir) = open_cache("ormerge", 0);
        cache.record_enumeration("", &[entry("m.mov", 4 * CHUNK_SIZE)], false, 1.0);

        // Two readers each persist their own snapshot; neither may
        // erase the other's bit.
        let mut a = Vec::new();
        set_bit(&mut a, 0);
        let mut b = Vec::new();
        set_bit(&mut b, 3);
        cache.update_chunk_bitmap("m.mov", &a);
        cache.update_chunk_bitmap("m.mov", &b);
        let bm = cache.get_chunk_bitmap("m.mov");
        assert!(bit_is_set(&bm, 0) && bit_is_set(&bm, 3));
        let (_, has_bm, cached, _, _) = cache.row_state("m.mov").unwrap();
        assert!(has_bm);
        assert_eq!(cached, 2 * CHUNK_SIZE);
        assert_eq!(cache.total_cached_bytes(), 2 * CHUNK_SIZE);
        let (bytes, hydrated_count) = cache.cache_stats();
        assert_eq!((bytes, hydrated_count), (2 * CHUNK_SIZE, 0));

        // Fully hydrated row ignores late partial persists.
        cache.mark_fully_hydrated("m.mov", 4 * CHUNK_SIZE);
        cache.update_chunk_bitmap("m.mov", &a);
        let (hyd, has_bm, cached, _, _) = cache.row_state("m.mov").unwrap();
        assert!(hyd && !has_bm && cached == 4 * CHUNK_SIZE);
        assert_eq!(cache.cache_stats(), (4 * CHUNK_SIZE, 1));

        // Write-through past EOF on a hydrated file grows the accounting.
        cache.extend_hydrated_size("m.mov", 5 * CHUNK_SIZE);
        let (_, _, cached, _, _) = cache.row_state("m.mov").unwrap();
        assert_eq!(cached, 5 * CHUNK_SIZE);
        assert_eq!(cache.cached_attr("m.mov").unwrap().hydrated_size, 5 * CHUNK_SIZE);
        // Never shrinks through this path.
        cache.extend_hydrated_size("m.mov", CHUNK_SIZE);
        assert_eq!(cache.row_state("m.mov").unwrap().2, 5 * CHUNK_SIZE);

        cache.invalidate_cache("m.mov", cache.fh_for_path("m.mov").unwrap());
        assert_eq!(cache.row_state("m.mov").unwrap().2, 0);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[tokio::test]
    async fn evictor_reclaims_partially_hydrated_blobs() {
        // Budget: 2 chunks. One hydrated 1-chunk file (hot) + one
        // partial 2-chunk blob (cold) = 3 chunks → over budget. The
        // old evictor only saw is_hydrated=1 rows and could never get
        // under budget.
        let (cache, dir) = open_cache("evictpartial", 2 * CHUNK_SIZE);
        cache.record_enumeration(
            "",
            &[entry("hot.mov", CHUNK_SIZE), entry("cold.mov", 4 * CHUNK_SIZE)],
            false,
            1.0,
        );
        let fh_cold = cache.fh_for_path("cold.mov").unwrap();
        let mut bm = Vec::new();
        set_bit(&mut bm, 0);
        set_bit(&mut bm, 2);
        cache.update_chunk_bitmap("cold.mov", &bm);
        std::fs::write(cache.cache_file_path(fh_cold), b"cold").unwrap();
        // Touch order: cold first, hot last → cold is the LRU victim.
        std::thread::sleep(std::time::Duration::from_millis(5));
        cache.mark_fully_hydrated("hot.mov", CHUNK_SIZE);
        assert_eq!(cache.total_cached_bytes(), 3 * CHUNK_SIZE);

        let (files, bytes) = cache.evict_over_budget_now().await;
        assert_eq!((files, bytes), (1, 2 * CHUNK_SIZE));
        let (hyd, has_bm, cached, _, _) = cache.row_state("cold.mov").unwrap();
        assert!(!hyd && !has_bm && cached == 0);
        assert!(!cache.cache_file_path(fh_cold).exists());
        assert!(cache.row_state("hot.mov").unwrap().0, "hot file kept");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[tokio::test]
    async fn drain_all_covers_partial_blobs() {
        let (cache, dir) = open_cache("drainpartial", 0);
        cache.record_enumeration("", &[entry("p.mov", 2 * CHUNK_SIZE)], false, 1.0);
        let mut bm = Vec::new();
        set_bit(&mut bm, 1);
        cache.update_chunk_bitmap("p.mov", &bm);
        let (count, bytes) = cache.drain_all().await;
        assert_eq!((count, bytes), (1, CHUNK_SIZE));
        assert!(!cache.row_state("p.mov").unwrap().1);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn cached_bytes_migration_backfills() {
        let dir = temp_cache_dir("cbmigrate");
        let db_path = dir.join("testshare.db");
        {
            let conn = Connection::open(&db_path).unwrap();
            conn.execute_batch(
                "CREATE TABLE known_files (
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
                 );
                 CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
                 INSERT INTO metadata VALUES ('blob_layout', '2');
                 INSERT INTO known_files (fh, path, name, is_dir, nas_size, nas_mtime, parent_path)
                 VALUES (1, '', '', 1, 0, 0, '');
                 INSERT INTO known_files (fh, path, name, nas_size, nas_mtime, is_hydrated, hydrated_size)
                 VALUES (2, 'full.mov', 'full.mov', 4096, 1, 1, 4096);
                 INSERT INTO known_files (fh, path, name, nas_size, nas_mtime, chunk_bitmap)
                 VALUES (3, 'part.mov', 'part.mov', 3145728, 1, X'05');",
            )
            .unwrap();
        }
        let cache = MacosCache::open("testshare", dir.clone(), 0, &dir).unwrap();
        assert_eq!(cache.row_state("full.mov").unwrap().2, 4096);
        assert_eq!(cache.row_state("part.mov").unwrap().2, 2 * CHUNK_SIZE);
        assert_eq!(cache.total_cached_bytes(), 4096 + 2 * CHUNK_SIZE);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn path_for_fh_distinguishes_missing_from_error() {
        let (cache, dir) = open_cache("pathfh", 0);
        assert_eq!(cache.path_for_fh(1).unwrap().as_deref(), Some(""));
        assert!(cache.path_for_fh(999_999).unwrap().is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
