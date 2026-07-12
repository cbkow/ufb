// dead_code allowed file-wide (2026-07-11): chunks of the cache API
// (hydration bookkeeping, drain/rebuild, connectivity stamps) are
// currently uncalled from the WinFsp path but are the surface the sync
// dogfood round is expected to wire or delete. Revisit with slice G —
// drop this allow and let the compiler name what dogfood didn't claim.
#![allow(dead_code)]

/// Per-mount cache index backed by SQLite (Windows).
///
/// Tracks hydrated files, NAS metadata, and chunk-level content bitmaps
/// for LRU eviction and block-level partial hydration.
///
/// DB location: `{cache_dir}/{mount_id}.db`, where `cache_dir` comes from
/// `MountsConfig::cache_root()` (user-configurable via `syncCacheRoot`).
///
/// Eviction: after each hydration, check budget → evict LRU to 80% of limit.
use crate::sync::cache_core::{self, SqliteConn, SqlitePool};
pub use crate::sync::cache_core::{parent_of, CachedAttr, EVICTION_TARGET_PERCENT};
use rusqlite::{params, Connection};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Instant;

/// Maximum age of a folder's last-enumeration timestamp before we
/// force a re-enum on the next read_directory, regardless of mtime
/// equality. Bounds the worst-case lag when the Windows SMB
/// redirector's `DirectoryCacheLifetime` (10s default) hides peer
/// changes from `fs::metadata(folder).modified()`. Per-folder timer
/// state is in-memory only — agent restarts naturally re-enumerate.
const FOLDER_REENUM_FALLBACK_SECS: u64 = 5;

pub struct CacheIndex {
    pool: SqlitePool,
    cache_limit: u64,
    client_root: PathBuf,
    /// Cache root (`MountsConfig::cache_root()`). DB lives at
    /// `{cache_dir}/{domain}.db`; block blobs under `{cache_dir}/by_key/`.
    cache_dir: PathBuf,
    /// Shared with NasSyncFilter — files with refcount > 0 can't be evicted.
    open_handles: Arc<Mutex<HashMap<PathBuf, u32>>>,
    /// Per-file read/write lock registry for ProjFS content cache. Readers
    /// (stream_file_content) take a read guard; eviction takes a non-blocking
    /// write guard, skipping files with active reads. Sparse — lazily created.
    per_key_locks: Mutex<HashMap<i64, Arc<std::sync::RwLock<()>>>>,
    /// Wall-clock timestamp of the last successful enumeration per
    /// relative folder path. Drives the timer-fallback path in
    /// `folder_needs_reenum` — see `FOLDER_REENUM_FALLBACK_SECS`.
    /// In-memory only by design; agent restart implicitly invalidates.
    last_enumerated_at: Mutex<HashMap<String, Instant>>,
    /// Domain/share name for log scoping.
    domain: String,
}

impl CacheIndex {
    /// Open or create the cache index DB. If corrupt, delete and recreate.
    pub fn open(
        client_root: &Path,
        mount_id: &str,
        cache_limit: u64,
        open_handles: Arc<Mutex<HashMap<PathBuf, u32>>>,
        cache_dir: &Path,
    ) -> (Self, bool) {
        // DB + block blobs live under the user-configurable cache root.
        let _ = std::fs::create_dir_all(cache_dir);
        let db_path = cache_dir.join(format!("{}.db", mount_id));

        // Run schema init / repair on a single serial connection before the pool opens.
        let needs_repair = Self::prepare_db(&db_path);

        let pool = cache_core::build_pool(&db_path)
            .expect("Failed to build SQLite pool");

        let index = Self {
            pool,
            cache_limit,
            client_root: client_root.to_path_buf(),
            cache_dir: cache_dir.to_path_buf(),
            open_handles,
            per_key_locks: Mutex::new(HashMap::new()),
            last_enumerated_at: Mutex::new(HashMap::new()),
            domain: mount_id.to_string(),
        };
        index.migrate_blob_layout();
        (index, needs_repair)
    }

    /// One-time blob-layout migration (v2 = per-domain `by_key/{domain}/`).
    /// The flat layout collided rowids across domains — see
    /// `cache_file_dir`. On first run after the change, dehydrate every
    /// row (old blobs are unreachable and may belong to another domain)
    /// and purge legacy flat files.
    fn migrate_blob_layout(&self) {
        let db = self.db();
        let layout: Option<String> = db
            .prepare_cached("SELECT value FROM metadata WHERE key='blob_layout'")
            .ok()
            .and_then(|mut s| s.query_row([], |r| r.get(0)).ok());
        if layout.as_deref() == Some("2") {
            return;
        }
        log::info!(
            "[cache] {}: migrating blob store to per-domain layout (full dehydrate)",
            self.domain
        );
        let _ = db.execute_batch(
            "UPDATE known_files SET is_hydrated=0, hydrated_size=0, chunk_bitmap=NULL;",
        );
        let flat = self.cache_dir.join("by_key");
        if let Ok(rd) = std::fs::read_dir(&flat) {
            for e in rd.flatten() {
                if e.file_type().map(|t| t.is_file()).unwrap_or(false) {
                    let _ = std::fs::remove_file(e.path());
                }
            }
        }
        let _ = db.execute(
            "INSERT OR REPLACE INTO metadata (key, value) VALUES ('blob_layout', '2')",
            [],
        );
    }

    /// Initialize (or repair) the DB on a single serial connection. Returns true
    /// if the DB was recreated from scratch (caller should rebuild cache index).
    fn prepare_db(db_path: &Path) -> bool {
        // Pre-touch so the OPEN call below takes the OPEN-EXISTING
        // branch through SQLite's Win32 VFS — its CREATE branch
        // fails on paths that resolve through 8.3 short names
        // (`C:\Users\UNIONG~1\…`). See `cache_core::ensure_db_file`.
        cache_core::ensure_db_file(db_path);
        match Connection::open(db_path) {
            Ok(conn) => {
                if Self::init_schema(&conn).is_ok() && Self::check_integrity(&conn) {
                    return false;
                }
                drop(conn);
                log::warn!("[cache] DB corrupt, recreating: {:?}", db_path);
                let _ = std::fs::remove_file(db_path);
            }
            Err(e) => {
                log::warn!("[cache] DB open failed ({}), recreating: {:?}", e, db_path);
                let _ = std::fs::remove_file(db_path);
            }
        }

        // After the remove above, the file is gone. Pre-touch again
        // so the recreate-from-scratch path also takes OPEN-EXISTING.
        cache_core::ensure_db_file(db_path);
        let conn = Connection::open(db_path).expect("Failed to create cache DB");
        Self::init_schema(&conn).expect("Failed to init cache schema");
        true
    }

    /// Get a pooled connection. Short-lived; returned to pool on drop.
    #[inline]
    fn db(&self) -> SqliteConn {
        self.pool.get().expect("SQLite pool exhausted")
    }

    fn init_schema(conn: &Connection) -> rusqlite::Result<()> {
        conn.execute_batch(cache_core::INIT_PRAGMAS)?;

        // Windows cache schema. Columns mirror `sync/macos_cache.rs` so
        // `sync/cache_core.rs` helpers work on both platforms. This is the
        // base schema — v0.5.0 is a hard cutover from the pre-WinFsp/CF era,
        // so no migration conditionals.
        // COLLATE NOCASE on the path keys: NTFS/SMB are case-insensitive
        // and case-preserving, but apps can open `REPORT.PDF` for a row
        // stored as `report.pdf` — byte-compared keys then miss (lost
        // caching) or worse, an invalidate targets the wrong-case key
        // and the other-case row keeps serving stale hydrated bytes.
        // With NOCASE every existing `WHERE path = ?` compares
        // case-insensitively via the column collation.
        conn.execute_batch(
            "CREATE TABLE IF NOT EXISTS known_files (
                 path TEXT PRIMARY KEY COLLATE NOCASE,
                 name TEXT NOT NULL DEFAULT '',
                 parent_path TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
                 is_dir INTEGER NOT NULL DEFAULT 0,
                 nas_size INTEGER NOT NULL,
                 nas_mtime INTEGER NOT NULL,
                 nas_created INTEGER NOT NULL DEFAULT 0,
                 is_hydrated INTEGER NOT NULL DEFAULT 0,
                 hydrated_size INTEGER DEFAULT 0,
                 chunk_bitmap BLOB DEFAULT NULL,
                 last_accessed INTEGER DEFAULT 0,
                 last_verified_at INTEGER DEFAULT 0
             );
             CREATE INDEX IF NOT EXISTS idx_hydrated ON known_files(is_hydrated);
             CREATE INDEX IF NOT EXISTS idx_accessed ON known_files(last_accessed);
             CREATE INDEX IF NOT EXISTS idx_parent_path ON known_files(parent_path);

             CREATE TABLE IF NOT EXISTS visited_folders (
                 nas_path TEXT PRIMARY KEY COLLATE NOCASE,
                 client_path TEXT NOT NULL,
                 folder_mtime INTEGER NOT NULL DEFAULT 0
             );",
        )?;

        conn.execute_batch(cache_core::METADATA_DDL)?;

        // Migration for pre-NOCASE DBs: rebuild both tables with the
        // new collation. Cache blobs are keyed by ROWID, so the copy
        // preserves rowids explicitly — otherwise every hydrated file's
        // blob would be orphaned (or worse, aliased to another row).
        let is_nocase: bool = conn
            .query_row(
                "SELECT sql FROM sqlite_master WHERE type='table' AND name='known_files'",
                [],
                |r| r.get::<_, String>(0),
            )
            .map(|sql| sql.to_uppercase().contains("COLLATE NOCASE"))
            .unwrap_or(true);
        if !is_nocase {
            log::info!("[cache] Migrating: COLLATE NOCASE path keys");
            conn.execute_batch(
                "BEGIN;
                 ALTER TABLE known_files RENAME TO kf_ci_old;
                 ALTER TABLE visited_folders RENAME TO vf_ci_old;
                 CREATE TABLE known_files (
                     path TEXT PRIMARY KEY COLLATE NOCASE,
                     name TEXT NOT NULL DEFAULT '',
                     parent_path TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
                     is_dir INTEGER NOT NULL DEFAULT 0,
                     nas_size INTEGER NOT NULL,
                     nas_mtime INTEGER NOT NULL,
                     nas_created INTEGER NOT NULL DEFAULT 0,
                     is_hydrated INTEGER NOT NULL DEFAULT 0,
                     hydrated_size INTEGER DEFAULT 0,
                     chunk_bitmap BLOB DEFAULT NULL,
                     last_accessed INTEGER DEFAULT 0,
                     last_verified_at INTEGER DEFAULT 0
                 );
                 CREATE TABLE visited_folders (
                     nas_path TEXT PRIMARY KEY COLLATE NOCASE,
                     client_path TEXT NOT NULL,
                     folder_mtime INTEGER NOT NULL DEFAULT 0
                 );
                 INSERT OR IGNORE INTO known_files
                     (rowid, path, name, parent_path, is_dir, nas_size, nas_mtime,
                      nas_created, is_hydrated, hydrated_size, chunk_bitmap,
                      last_accessed, last_verified_at)
                 SELECT rowid, path, name, parent_path, is_dir, nas_size, nas_mtime,
                        nas_created, is_hydrated, hydrated_size, chunk_bitmap,
                        last_accessed, last_verified_at
                 FROM kf_ci_old ORDER BY rowid ASC;
                 INSERT OR IGNORE INTO visited_folders (nas_path, client_path, folder_mtime)
                 SELECT nas_path, client_path, folder_mtime FROM vf_ci_old;
                 DROP TABLE kf_ci_old;
                 DROP TABLE vf_ci_old;
                 CREATE INDEX IF NOT EXISTS idx_hydrated ON known_files(is_hydrated);
                 CREATE INDEX IF NOT EXISTS idx_accessed ON known_files(last_accessed);
                 CREATE INDEX IF NOT EXISTS idx_parent_path ON known_files(parent_path);
                 COMMIT;",
            )?;
        }

        Ok(())
    }

    fn check_integrity(conn: &Connection) -> bool {
        cache_core::check_integrity(conn)
    }

    // ── Hydration tracking ──

    /// Record a successful hydration. Triggers eviction check.
    pub fn record_hydration(&self, path: &Path, size: u64) {
        if size == 0 {
            return;
        }
        let path_str = path.to_string_lossy();
        let now = Self::unix_now();

        let db = self.db();
        let _ = db
            .prepare_cached(
                "INSERT INTO known_files (path, nas_size, nas_mtime, is_hydrated, hydrated_size, last_accessed)
                 VALUES (?1, ?2, 0, 1, ?2, ?3)
                 ON CONFLICT(path) DO UPDATE SET is_hydrated=1, hydrated_size=?2, last_accessed=?3",
            )
            .map(|mut stmt| stmt.execute(params![path_str.as_ref(), size as i64, now]));
        drop(db);

        self.evict_if_over_budget();
    }

    /// Record a dehydration (OS-initiated or programmatic).
    pub fn record_dehydration(&self, path: &Path) {
        let path_str = path.to_string_lossy();
        let db = self.db();
        let _ = db
            .prepare_cached("UPDATE known_files SET is_hydrated=0, hydrated_size=0 WHERE path = ?1")
            .map(|mut stmt| stmt.execute(params![path_str.as_ref()]));
    }

    /// Update last-access timestamp (LRU refresh on re-access).
    pub fn touch(&self, path: &Path) {
        let path_str = path.to_string_lossy();
        let now = Self::unix_now();
        let db = self.db();
        let _ = db
            .prepare_cached("UPDATE known_files SET last_accessed = ?1 WHERE path = ?2")
            .map(|mut stmt| stmt.execute(params![now, path_str.as_ref()]));
    }

    /// Stamp last_verified_at = now. Called after a successful NAS stat confirms
    /// cached metadata matches reality.
    pub fn record_verification(&self, path: &Path) {
        let path_str = path.to_string_lossy();
        let now = Self::unix_now();
        let db = self.db();
        let _ = db
            .prepare_cached("UPDATE known_files SET last_verified_at = ?1 WHERE path = ?2")
            .map(|mut stmt| stmt.execute(params![now, path_str.as_ref()]));
    }

    /// Update NAS metadata to fresh stat values and stamp last_verified_at.
    /// Preserves hydration state and last_accessed via targeted UPDATE.
    pub fn update_nas_metadata(&self, path: &Path, nas_size: u64, nas_mtime: i64) {
        let path_str = path.to_string_lossy();
        let now = Self::unix_now();
        let db = self.db();
        let _ = db
            .prepare_cached(
                "UPDATE known_files SET nas_size = ?1, nas_mtime = ?2, last_verified_at = ?3 WHERE path = ?4",
            )
            .map(|mut stmt| stmt.execute(params![nas_size as i64, nas_mtime, now, path_str.as_ref()]));
    }

    /// Compare provided NAS metadata against the cached row.
    ///
    /// Returns `Some(true)` if drift (cached differs), `Some(false)` if match,
    /// `None` if the path is not tracked. Used by hooks that already have
    /// fresh stat values (e.g. directory enumeration) and want to avoid a
    /// second NAS round-trip via `stat_and_refresh`.
    pub fn compare_nas_metadata(&self, path: &Path, nas_size: u64, nas_mtime: i64) -> Option<bool> {
        let path_str = path.to_string_lossy();
        let db = self.db();
        db.prepare_cached("SELECT nas_size, nas_mtime FROM known_files WHERE path = ?1")
            .ok()
            .and_then(|mut stmt| {
                stmt.query_row(params![path_str.as_ref()], |row| {
                    let size: i64 = row.get(0)?;
                    let mtime: i64 = row.get(1)?;
                    Ok((size, mtime))
                })
                .ok()
            })
            .map(|(cached_size, cached_mtime)| {
                nas_size != cached_size as u64 || nas_mtime != cached_mtime
            })
    }

    /// Check if a path has any row in known_files.
    /// Used by `stat_and_refresh` to disambiguate "within TTL" from "unknown".
    pub fn is_known(&self, path: &Path) -> bool {
        let path_str = path.to_string_lossy();
        let db = self.db();
        db.prepare_cached("SELECT 1 FROM known_files WHERE path = ?1")
            .ok()
            .and_then(|mut stmt| {
                stmt.query_row(params![path_str.as_ref()], |_| Ok(true)).ok()
            })
            .unwrap_or(false)
    }

    /// Returns cached `(nas_size, nas_mtime)` if the entry warrants re-verification,
    /// or `None` if we should skip the NAS stat.
    ///
    /// Skips when: entry was verified within `ttl_secs`, OR the path is unknown
    /// (no baseline to compare against — let the normal hydration path populate it).
    pub fn needs_verification(&self, path: &Path, ttl_secs: i64) -> Option<(u64, i64)> {
        let path_str = path.to_string_lossy();
        let now = Self::unix_now();
        let db = self.db();
        db.prepare_cached(
            "SELECT nas_size, nas_mtime, last_verified_at FROM known_files WHERE path = ?1",
        )
        .ok()
        .and_then(|mut stmt| {
            stmt.query_row(params![path_str.as_ref()], |row| {
                let size: i64 = row.get(0)?;
                let mtime: i64 = row.get(1)?;
                let verified: i64 = row.get(2)?;
                Ok((size, mtime, verified))
            })
            .ok()
        })
        .and_then(|(size, mtime, verified)| {
            if now - verified < ttl_secs {
                None
            } else {
                Some((size as u64, mtime))
            }
        })
    }

    /// Update path after a rename.
    pub fn rename_entry(&self, old_path: &Path, new_path: &Path) {
        let old_str = old_path.to_string_lossy();
        let new_str = new_path.to_string_lossy();
        let db = self.db();
        let _ = db.execute(
            "UPDATE known_files SET path = ?1 WHERE path = ?2",
            params![new_str.as_ref(), old_str.as_ref()],
        );
    }

    /// Total bytes of cached (hydrated) files.
    pub fn total_cached_bytes(&self) -> u64 {
        let db = self.db();
        db.prepare_cached(
            "SELECT COALESCE(SUM(hydrated_size), 0) FROM known_files WHERE is_hydrated = 1",
        )
        .ok()
        .and_then(|mut stmt| stmt.query_row([], |row| row.get::<_, i64>(0)).ok())
        .unwrap_or(0) as u64
    }

    // ── Known files (placeholder tracking for reconciliation) ──

    /// Record a known file (placeholder created during FETCH_PLACEHOLDERS or watcher).
    pub fn record_known_file(&self, path: &Path, nas_size: u64, nas_mtime: i64) {
        let path_str = path.to_string_lossy();
        let db = self.db();
        let _ = db
            .prepare_cached(
                "INSERT INTO known_files (path, nas_size, nas_mtime) VALUES (?1, ?2, ?3)
                 ON CONFLICT(path) DO UPDATE SET nas_size=?2, nas_mtime=?3",
            )
            .map(|mut stmt| stmt.execute(params![path_str.as_ref(), nas_size as i64, nas_mtime]));
    }

    /// Remove a known file (placeholder removed).
    pub fn remove_known_file(&self, path: &Path) {
        let path_str = path.to_string_lossy();
        let db = self.db();
        let _ = db
            .prepare_cached("DELETE FROM known_files WHERE path = ?1")
            .map(|mut stmt| stmt.execute(params![path_str.as_ref()]));
    }

    /// Get all known files for a folder (for reconciliation diff).
    pub fn known_files_in_folder(&self, folder_path: &Path) -> Vec<(String, i64, i64)> {
        let prefix = format!("{}\\", folder_path.to_string_lossy());
        let db = self.db();
        let mut stmt = db
            .prepare("SELECT path, nas_size, nas_mtime FROM known_files WHERE path LIKE ?1 AND path NOT LIKE ?2")
            .unwrap();
        // Match direct children only (path starts with prefix, no additional backslash)
        stmt.query_map(params![format!("{}%", prefix), format!("{}%\\\\%", prefix)], |row| {
            Ok((row.get(0)?, row.get(1)?, row.get(2)?))
        })
        .unwrap()
        .filter_map(|r| r.ok())
        .collect()
    }

    // ── Visited folders ──

    /// Record a visited folder (called during FETCH_PLACEHOLDERS / watcher.register).
    pub fn record_visited_folder(&self, nas_path: &Path, client_path: &Path, folder_mtime: i64) {
        let nas_str = nas_path.to_string_lossy();
        let client_str = client_path.to_string_lossy();
        let db = self.db();
        let _ = db.execute(
            "INSERT OR REPLACE INTO visited_folders (nas_path, client_path, folder_mtime) VALUES (?1, ?2, ?3)",
            params![nas_str.as_ref(), client_str.as_ref(), folder_mtime],
        );
    }

    /// Ensure a visited folder exists in the DB. If already present, keeps existing mtime.
    /// If new, inserts with mtime=0 (forces reconciliation on next startup).
    pub fn ensure_visited_folder(&self, nas_path: &Path, client_path: &Path) {
        let nas_str = nas_path.to_string_lossy();
        let client_str = client_path.to_string_lossy();
        let db = self.db();
        let _ = db.execute(
            "INSERT OR IGNORE INTO visited_folders (nas_path, client_path, folder_mtime) VALUES (?1, ?2, 0)",
            params![nas_str.as_ref(), client_str.as_ref()],
        );
    }

    /// Get all visited folders for startup reconciliation.
    pub fn visited_folders(&self) -> Vec<(PathBuf, PathBuf, i64)> {
        let db = self.db();
        let mut stmt = db
            .prepare("SELECT nas_path, client_path, folder_mtime FROM visited_folders")
            .unwrap();
        stmt.query_map([], |row| {
            let nas: String = row.get(0)?;
            let client: String = row.get(1)?;
            let mtime: i64 = row.get(2)?;
            Ok((PathBuf::from(nas), PathBuf::from(client), mtime))
        })
        .unwrap()
        .filter_map(|r| r.ok())
        .collect()
    }

    /// Update folder mtime after reconciliation.
    pub fn update_folder_mtime(&self, nas_path: &Path, folder_mtime: i64) {
        let nas_str = nas_path.to_string_lossy();
        let db = self.db();
        let _ = db.execute(
            "UPDATE visited_folders SET folder_mtime = ?1 WHERE nas_path = ?2",
            params![folder_mtime, nas_str.as_ref()],
        );
    }

    // ── ProjFS cache-serving (Slice 2+) ──
    //
    // These methods serve the ProjFS provider callbacks. Paths are relative
    // to the share root, forward-slash-separated (normalized at the provider
    // boundary). The `parent_path` column is indexed for fast READDIR.

    /// Cached metadata for a single relative path.
    pub fn cached_attr_by_path(&self, rel: &str) -> Option<CachedAttr> {
        let db = self.db();
        db.prepare_cached(
            "SELECT is_dir, nas_size, nas_mtime, nas_created, is_hydrated, hydrated_size
             FROM known_files WHERE path = ?1",
        )
        .ok()
        .and_then(|mut stmt| {
            stmt.query_row(params![rel], |row| {
                Ok(CachedAttr {
                    is_dir: row.get::<_, i32>(0)? != 0,
                    size: row.get::<_, i64>(1)? as u64,
                    mtime: row.get::<_, i64>(2)? as f64,
                    created: row.get::<_, i64>(3)? as f64,
                    is_hydrated: row.get::<_, i32>(4)? != 0,
                    hydrated_size: row.get::<_, i64>(5)? as u64,
                })
            })
            .ok()
        })
    }

    /// Direct children of a folder — (name, CachedAttr) tuples.
    /// Returns empty Vec for cold (unenumerated) folders.
    pub fn cached_children_by_parent(&self, parent_rel: &str) -> Vec<(String, CachedAttr)> {
        let db = self.db();
        db.prepare_cached(
            "SELECT name, is_dir, nas_size, nas_mtime, nas_created, is_hydrated, hydrated_size
             FROM known_files
             WHERE parent_path = ?1 AND path != ''",
        )
        .ok()
        .and_then(|mut stmt| {
            stmt.query_map(params![parent_rel], |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    CachedAttr {
                        is_dir: row.get::<_, i32>(1)? != 0,
                        size: row.get::<_, i64>(2)? as u64,
                        mtime: row.get::<_, i64>(3)? as f64,
                        created: row.get::<_, i64>(4)? as f64,
                        is_hydrated: row.get::<_, i32>(5)? != 0,
                        hydrated_size: row.get::<_, i64>(6)? as u64,
                    },
                ))
            })
            .ok()
            .map(|rows| rows.filter_map(|r| r.ok()).collect())
        })
        .unwrap_or_default()
    }

    /// Has this folder been enumerated since the agent started (or at all)?
    pub fn folder_is_enumerated_by_rel(&self, rel: &str) -> bool {
        let db = self.db();
        db.prepare_cached(
            "SELECT 1 FROM visited_folders WHERE nas_path = ?1",
        )
        .ok()
        .and_then(|mut stmt| stmt.query_row(params![rel], |_| Ok(true)).ok())
        .unwrap_or(false)
    }

    /// Should this folder be re-enumerated from SMB?
    ///
    /// Returns true if either:
    /// - The folder has never been visited (cold), OR
    /// - The folder's cached `folder_mtime` is older than `current_nas_mtime`
    ///   (out-of-band changes on SMB — another machine added/removed files
    ///   inside this folder, which bumps its mtime).
    ///
    /// `populate_folder` stores the SMB folder mtime (not the visit time)
    /// so subsequent comparisons here are meaningful. Phase 1+ pattern.
    pub fn folder_needs_reenum(&self, rel: &str, current_nas_mtime: i64) -> bool {
        // Cold path: never seen this folder → must enumerate.
        let Some(cached_mtime) = self.cached_folder_mtime(rel) else {
            return true;
        };
        // Drift path: SMB folder mtime advanced since our last enum.
        // Reliable when peer writes are old enough that the local SMB
        // redirector cache has rolled over (~10s).
        if current_nas_mtime > cached_mtime {
            return true;
        }
        // Timer fallback path: even if mtimes appear equal, force a
        // re-enum if our last successful enumeration was more than
        // FOLDER_REENUM_FALLBACK_SECS ago. Bounds the lag created by
        // the SMB redirector returning a stale folder mtime for peer
        // writes (it only invalidates on local writes). In-memory map
        // means an agent restart implicitly triggers this path on
        // first access — desirable on restart.
        let elapsed = self
            .last_enumerated_at
            .lock()
            .ok()
            .and_then(|m| m.get(rel).map(|t| t.elapsed()));
        match elapsed {
            None => true,
            Some(d) => d.as_secs() >= FOLDER_REENUM_FALLBACK_SECS,
        }
    }

    /// Diagnostic accessor: the SMB folder mtime we stored last time we
    /// enumerated this folder. `None` if the folder has never been
    /// visited. Paired with `folder_needs_reenum` for logging the
    /// cached-vs-live comparison.
    pub fn cached_folder_mtime(&self, rel: &str) -> Option<i64> {
        let db = self.db();
        db.prepare_cached("SELECT folder_mtime FROM visited_folders WHERE nas_path = ?1")
            .ok()
            .and_then(|mut stmt| stmt.query_row(params![rel], |row| row.get(0)).ok())
    }

    /// Populate the cache for a cold folder from a live SMB enumeration.
    /// Upserts each entry into `known_files` with `parent_path` set, then
    /// marks the folder as visited. Orphans (DB rows not in `entries`)
    /// are deleted.
    /// Populate cache from a folder enumeration.
    ///
    /// `folder_nas_mtime` is the SMB mtime of the folder itself at the
    /// moment we enumerated it; gets written into `visited_folders.folder_mtime`
    /// so `folder_needs_reenum` can detect out-of-band changes (another
    /// machine creates/deletes inside this folder → SMB bumps the folder
    /// mtime → we re-enumerate on next read). Pre-Phase-1+ this column
    /// stored `unix_now()` instead, which was useless for drift detection.
    ///
    /// `is_partial` signals that the caller hit per-entry stat errors
    /// during enumeration: in that case we still upsert what we got
    /// (better than no data) but skip the orphan-delete pass (some
    /// "missing" entries are really just entries we failed to stat) AND
    /// skip marking the folder visited (so next read retries). Phase 3.
    pub fn populate_folder(
        &self,
        folder_rel: &str,
        entries: &[(String, bool, u64, i64, i64)], // (name, is_dir, size, mtime, created)
        folder_nas_mtime: i64,
        is_partial: bool,
    ) {
        let db = self.db();

        // Phase 1: snapshot existing children BEFORE the upsert so we
        // can detect per-entry drift (cached size/mtime != live SMB).
        // For drifted entries that were is_hydrated=1, queue them for
        // cache-file invalidation after the upsert. Mirror of
        // macos_cache.rs::record_enumeration's drift check.
        let existing_attrs = self.cached_children_by_parent(folder_rel);
        let mut drifted: Vec<String> = Vec::new();
        let existing_map: std::collections::HashMap<&str, &CachedAttr> = existing_attrs
            .iter()
            .map(|(name, attr)| (name.as_str(), attr))
            .collect();

        // Upsert entries.
        if let Ok(mut stmt) = db.prepare_cached(
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
            for (name, is_dir, size, mtime, created) in entries {
                let child_rel = if folder_rel.is_empty() {
                    name.clone()
                } else {
                    format!("{}/{}", folder_rel, name)
                };

                // Phase 1: drift check against the pre-upsert snapshot.
                // Only flag content-bearing files (is_dir == false) since
                // dir metadata changes don't need cache invalidation.
                if !*is_dir {
                    if let Some(prev) = existing_map.get(name.as_str()) {
                        if prev.is_hydrated {
                            // SMB mtime is stored as i64 secs; cached
                            // is f64 secs (cache_core::CachedAttr).
                            // 1 sec slop for SMB granularity.
                            let mtime_drift = (*mtime as f64 - prev.mtime).abs() > 1.0;
                            let size_drift = *size != prev.size;
                            if mtime_drift || size_drift {
                                log::info!(
                                    "[windows-cache] {} drift detected for {} \
                                     (cached size={} mtime={:.0}, NAS size={} mtime={}) \
                                     — invalidating cache",
                                    self.domain, child_rel, prev.size, prev.mtime, size, mtime
                                );
                                drifted.push(child_rel.clone());
                            }
                        }
                    }
                }

                let _ = stmt.execute(params![
                    child_rel,
                    name,
                    *is_dir as i32,
                    *size as i64,
                    mtime,
                    created,
                    folder_rel,
                ]);
            }
        }

        // Phase 1: invalidate cache content for drifted files. Clears
        // is_hydrated/hydrated_size/chunk_bitmap and removes the cache
        // file so next read refetches from SMB.
        for path in &drifted {
            self.invalidate_cache_by_path(path);
        }

        // Phase 3: if the caller hit per-entry stat errors mid-enumeration,
        // the `entries` slice is an under-count of what's actually on
        // SMB. Orphan-delete and visited-mark both rely on a COMPLETE
        // enumeration, so skip both — the upsert above still gives the
        // user partial data immediately, and the next read_directory
        // call will retry the whole enumeration.
        if is_partial {
            log::debug!(
                "[windows-cache] {} {} partial enumeration ({} entries upserted) — \
                 skipping orphan prune and visited mark; will retry on next read",
                self.domain,
                if folder_rel.is_empty() { "(root)" } else { folder_rel },
                entries.len(),
            );
            return;
        }

        // Compute orphans: DB children not in the live listing.
        // Case-folded — the SQL layer is NOCASE but this Rust set
        // isn't; without folding, a row stored under a different case
        // reads as an orphan and gets pruned mid-use.
        let live_names: std::collections::HashSet<String> =
            entries.iter().map(|(n, ..)| n.to_lowercase()).collect();

        // Phase 2: suspicious_empty guard — if SMB returned zero entries
        // but the cache had children, this is almost certainly a
        // transient SMB blip (network glitch, server overload). Don't
        // delete every cached child. Defer the prune to the next
        // enumeration. Mirror of macos_cache.rs::record_enumeration.
        let suspicious_empty = entries.is_empty() && !existing_attrs.is_empty();
        if suspicious_empty {
            log::warn!(
                "[windows-cache] {} {} appears suddenly empty (had {} cached children) — \
                 skipping orphan prune; will retry on next enumeration",
                self.domain,
                if folder_rel.is_empty() { "(root)" } else { folder_rel },
                existing_attrs.len(),
            );
        } else {
            // Grace window (macOS parity): rows touched in the last 60s
            // belong to in-flight I/O — a lagging redirector listing
            // that misses a just-created file must not prune the row a
            // copy is actively writing through. Genuinely deleted files
            // still prune on the next enumeration past the window.
            const PRUNE_GRACE_SECS: i64 = 60;
            let now = Self::unix_now();
            let mut doomed: Vec<(String, Option<i64>)> = Vec::new();
            for (existing_name, attr) in &existing_attrs {
                if live_names.contains(&existing_name.to_lowercase()) {
                    continue;
                }
                if now - (attr.mtime as i64) < PRUNE_GRACE_SECS {
                    continue;
                }
                let orphan_path = if folder_rel.is_empty() {
                    existing_name.clone()
                } else {
                    format!("{}/{}", folder_rel, existing_name)
                };
                let rowid = self.rowid_for_path(&orphan_path);
                doomed.push((orphan_path, rowid));
            }
            if let Ok(mut del) = db.prepare_cached("DELETE FROM known_files WHERE path = ?1") {
                for (orphan_path, rowid) in &doomed {
                    if del.execute(params![orphan_path]).is_ok() {
                        // Remove the blob too — a bare row DELETE leaked
                        // it until the next startup scan_and_index.
                        if let Some(rowid) = rowid {
                            let _ = std::fs::remove_file(self.cache_file_path(*rowid));
                        }
                    }
                }
            }
        }

        // Mark folder as visited. Store the SMB folder mtime (not
        // unix_now) so folder_needs_reenum can detect out-of-band
        // changes by comparing against a fresh stat next time.
        let _ = db.execute(
            "INSERT OR REPLACE INTO visited_folders (nas_path, client_path, folder_mtime) VALUES (?1, '', ?2)",
            params![folder_rel, folder_nas_mtime],
        );

        // Stamp the wall-clock time for the timer-fallback branch in
        // folder_needs_reenum. The DB folder_mtime alone is not enough
        // because the Windows SMB redirector returns a stale folder
        // mtime for peer writes until its own cache TTL expires.
        if let Ok(mut map) = self.last_enumerated_at.lock() {
            map.insert(folder_rel.to_string(), Instant::now());
        }
    }

    // ── Block-level content cache (Slice 3) ──

    /// Accessor for `cache_limit` — used by the ProjFS server to decide
    /// whether to spawn the eviction task.
    pub fn cache_limit(&self) -> u64 {
        self.cache_limit
    }

    /// Get or create a per-key RwLock for the given rowid. Readers
    /// (stream_file_content) take a read guard; eviction takes a
    /// non-blocking write guard.
    pub fn key_lock(&self, rowid: i64) -> Arc<std::sync::RwLock<()>> {
        let mut map = self.per_key_locks.lock().unwrap();
        map.entry(rowid)
            .or_insert_with(|| Arc::new(std::sync::RwLock::new(())))
            .clone()
    }

    /// SQLite rowid for a relative path. Used as the cache-file key.
    pub fn rowid_for_path(&self, rel: &str) -> Option<i64> {
        let db = self.db();
        db.prepare_cached("SELECT rowid FROM known_files WHERE path = ?1")
            .ok()
            .and_then(|mut stmt| stmt.query_row(params![rel], |row| row.get(0)).ok())
    }

    /// Read the current chunk bitmap for a file.
    pub fn get_chunk_bitmap(&self, rel: &str) -> Vec<u8> {
        let db = self.db();
        let Ok(mut stmt) =
            db.prepare_cached("SELECT chunk_bitmap FROM known_files WHERE path = ?1")
        else {
            return Vec::new();
        };
        stmt.query_row(params![rel], |row| {
            Ok(row.get::<_, Option<Vec<u8>>>(0)?.unwrap_or_default())
        })
        .unwrap_or_default()
    }

    /// Update last_accessed for a relative path (LRU refresh on re-access).
    pub fn touch_by_rel(&self, rel: &str) {
        let now = Self::unix_now();
        let db = self.db();
        let _ = db
            .prepare_cached("UPDATE known_files SET last_accessed = ?1 WHERE path = ?2")
            .map(|mut stmt| stmt.execute(params![now, rel]));
    }

    /// Persist an updated chunk bitmap + bump `last_accessed`.
    pub fn update_chunk_bitmap(&self, rel: &str, bitmap: &[u8]) {
        let now = Self::unix_now();
        let db = self.db();
        let _ = db
            .prepare_cached(
                "UPDATE known_files SET chunk_bitmap = ?1, last_accessed = ?2 WHERE path = ?3",
            )
            .map(|mut stmt| stmt.execute(params![bitmap, now, rel]));
    }

    /// Mark a file as fully hydrated — null the bitmap, flip `is_hydrated`.
    pub fn mark_fully_hydrated(&self, rel: &str, size: u64) {
        let now = Self::unix_now();
        let db = self.db();
        let _ = db
            .prepare_cached(
                "UPDATE known_files
                 SET is_hydrated = 1, hydrated_size = ?1, chunk_bitmap = NULL, last_accessed = ?2
                 WHERE path = ?3",
            )
            .map(|mut stmt| stmt.execute(params![size as i64, now, rel]));
    }

    /// Remove a known file by relative path (after deletion).
    pub fn remove_known_file_by_rel(&self, rel: &str) {
        if let Some(rowid) = self.rowid_for_path(rel) {
            let cache_path = self.cache_file_path(rowid);
            let _ = std::fs::remove_file(&cache_path);
            self.per_key_locks.lock().unwrap().remove(&rowid);
        }
        let db = self.db();
        let _ = db
            .prepare_cached("DELETE FROM known_files WHERE path = ?1")
            .map(|mut stmt| stmt.execute(params![rel]));
    }

    /// Rename a known file's path in the cache (after NAS rename).
    pub fn rename_entry_by_rel(&self, old_rel: &str, new_rel: &str) {
        let new_parent = parent_of(new_rel).to_string();
        let new_name = new_rel.rsplit('/').next().unwrap_or(new_rel).to_string();

        // Rename-over-existing (the editor atomic-save pattern: write
        // `~tmp`, rename over the final name). `path` is the PRIMARY
        // KEY, so if a row already exists at the destination the UPDATE
        // below would collide and silently fail — leaving the OLD
        // destination row alive with is_hydrated=1 over its pre-save
        // cache blob. Every read of the saved file then returns the
        // pre-save bytes. Clear the destination rows (and their blobs)
        // first; do the same for descendants on a directory rename.
        let clobbered: Vec<i64> = {
            let db = self.db();
            db.prepare_cached(
                "SELECT rowid FROM known_files WHERE path = ?1 OR path LIKE ?1 || '/%'",
            )
            .ok()
            .and_then(|mut s| {
                s.query_map(params![new_rel], |r| r.get::<_, i64>(0))
                    .ok()
                    .map(|rows| rows.filter_map(|r| r.ok()).collect())
            })
            .unwrap_or_default()
        };
        if !clobbered.is_empty() {
            let db = self.db();
            let _ = db
                .prepare_cached(
                    "DELETE FROM known_files WHERE path = ?1 OR path LIKE ?1 || '/%'",
                )
                .map(|mut stmt| stmt.execute(params![new_rel]));
        }
        for rowid in clobbered {
            let _ = std::fs::remove_file(self.cache_file_path(rowid));
        }

        {
            let db = self.db();
            let _ = db
                .prepare_cached(
                    "UPDATE known_files SET path = ?1, name = ?2, parent_path = ?3 WHERE path = ?4",
                )
                .map(|mut stmt| stmt.execute(params![new_rel, new_name, new_parent, old_rel]));
        }

        // Directory rename: cascade the prefix rewrite to descendants,
        // or they linger as ghosts under the old path while the new
        // subtree cold-re-enumerates with fresh rowids.
        let old_prefix = format!("{}/", old_rel);
        let new_prefix = format!("{}/", new_rel);
        let descendants: Vec<(i64, String)> = {
            let db = self.db();
            db.prepare_cached("SELECT rowid, path FROM known_files WHERE path LIKE ?1 || '%'")
                .ok()
                .and_then(|mut s| {
                    s.query_map(params![old_prefix], |r| {
                        Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?))
                    })
                    .ok()
                    .map(|rows| rows.filter_map(|r| r.ok()).collect())
                })
                .unwrap_or_default()
        };
        for (rowid, old_path) in descendants {
            let new_path = format!("{}{}", new_prefix, &old_path[old_prefix.len()..]);
            let new_parent = parent_of(&new_path).to_string();
            let db = self.db();
            let _ = db
                .prepare_cached(
                    "UPDATE known_files SET path = ?1, parent_path = ?2 WHERE rowid = ?3",
                )
                .map(|mut stmt| stmt.execute(params![new_path, new_parent, rowid]));
        }
    }

    /// Insert a single new entry after a `create` / `mkdir` on the NAS.
    /// Upserts so callers can retry idempotently. `relative_path` /
    /// `parent_path` use forward-slash form (no leading slash). Mirrors
    /// `macos_cache::record_new_entry`.
    pub fn record_new_entry(
        &self,
        relative_path: &str,
        name: &str,
        is_dir: bool,
        size: u64,
        mtime: i64,
        created: i64,
    ) {
        let parent = cache_core::parent_of(relative_path).to_string();
        let db = self.db();
        let _ = db
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

    /// Update post-write NAS metadata for a single file. Called by the
    /// write / overwrite / set_file_size callbacks to keep the cache
    /// coherent with SMB after authoritative writes.
    pub fn update_metadata_by_rel(&self, rel: &str, size: u64, mtime: i64) {
        let now = Self::unix_now();
        let db = self.db();
        let _ = db
            .prepare_cached(
                "UPDATE known_files
                 SET nas_size = ?1, nas_mtime = ?2, last_verified_at = ?3
                 WHERE path = ?4",
            )
            .map(|mut stmt| stmt.execute(params![size as i64, mtime, now, rel]));
    }

    /// Invalidate cache for a file (write-through path).
    pub fn invalidate_cache_by_path(&self, rel: &str) {
        // Resolve the rowid FIRST so the per-key lock covers both the
        // flag clear and the file removal — otherwise a concurrent
        // reader holding the cache file open interleaves with a
        // half-applied invalidation.
        let rowid = self.rowid_for_path(rel);
        let _guard = rowid.map(|r| self.key_lock(r));
        let _guard = _guard.as_ref().map(|l| l.write().unwrap());
        self.invalidate_cache_locked(rel, rowid);
    }

    /// Invalidation core — caller MUST already hold the key_lock write
    /// guard for `rowid` (or know no lock exists yet). Split out so the
    /// write path, which already holds the guard, doesn't deadlock
    /// re-acquiring it.
    pub fn invalidate_cache_locked(&self, rel: &str, rowid: Option<i64>) {
        {
            let db = self.db();
            let _ = db
                .prepare_cached(
                    "UPDATE known_files
                     SET is_hydrated = 0, hydrated_size = 0, chunk_bitmap = NULL
                     WHERE path = ?1",
                )
                .map(|mut stmt| stmt.execute(params![rel]));
        }

        if let Some(rowid) = rowid {
            let cache_path = self.cache_file_path(rowid);
            if std::fs::remove_file(&cache_path).is_err() {
                // Windows can't delete a file another handle holds open.
                // Leaving the old bytes on disk is NOT safe: the next
                // hydration opens with truncate(false) and the zero-skip
                // sparse optimization skips writing zero runs, so stale
                // non-zero bytes from the old version would bleed
                // through. Truncation succeeds on open handles.
                if let Ok(f) = std::fs::OpenOptions::new().write(true).open(&cache_path) {
                    let _ = f.set_len(0);
                }
            }
        }
    }

    /// Cache-file path for a given rowid. Directory is created on first call.
    pub fn cache_file_path(&self, rowid: i64) -> PathBuf {
        let dir = self.cache_file_dir();
        let _ = std::fs::create_dir_all(&dir);
        dir.join(format!("{:016x}", rowid))
    }

    /// MUST be namespaced by domain: `cache_dir` is shared across all
    /// mounts while each domain has its own DB, so rowids collide across
    /// domains. A flat `by_key/` had two shares' rowid=N resolving to
    /// the SAME blob — reads served another share's file content.
    fn cache_file_dir(&self) -> PathBuf {
        self.cache_dir.join("by_key").join(&self.domain)
    }

    /// LRU eviction for the block-level content cache. Deletes oldest-accessed
    /// cache files until total hydrated bytes ≤ 80% of cache_limit. Skips
    /// files with active readers via `per_key_locks.try_write()`.
    /// Returns `(files_evicted, bytes_freed)`.
    pub fn evict_over_budget_now(&self) -> (usize, u64) {
        if self.cache_limit == 0 {
            return (0, 0);
        }
        let total = self.total_cached_bytes();
        if total <= self.cache_limit {
            return (0, 0);
        }
        let target = (self.cache_limit as f64 * EVICTION_TARGET_PERCENT) as u64;

        let candidates: Vec<(i64, String, u64)> = {
            let db = self.db();
            let mut stmt = match db.prepare(
                "SELECT rowid, path, hydrated_size FROM known_files
                 WHERE is_hydrated=1 AND is_dir=0
                 ORDER BY last_accessed ASC",
            ) {
                Ok(s) => s,
                Err(e) => {
                    log::error!("[cache] eviction prepare failed: {}", e);
                    return (0, 0);
                }
            };
            stmt.query_map([], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?, row.get::<_, i64>(2)? as u64))
            })
            .ok()
            .map(|it| it.filter_map(|r| r.ok()).collect())
            .unwrap_or_default()
        };

        let mut remaining = total;
        let mut files_evicted = 0usize;

        for (rowid, _path, size) in &candidates {
            if remaining <= target {
                break;
            }
            let lock = self.key_lock(*rowid);
            let Ok(_write_guard) = lock.try_write() else {
                continue;
            };

            let cache_path = self.cache_file_path(*rowid);
            let _ = std::fs::remove_file(&cache_path);

            let db = self.db();
            let _ = db
                .prepare_cached(
                    "UPDATE known_files
                     SET is_hydrated=0, hydrated_size=0, chunk_bitmap=NULL
                     WHERE rowid=?1",
                )
                .map(|mut stmt| stmt.execute(params![rowid]));

            remaining = remaining.saturating_sub(*size);
            files_evicted += 1;
            self.per_key_locks.lock().unwrap().remove(rowid);
        }

        let bytes_freed = total - remaining;
        if files_evicted > 0 {
            log::info!(
                "[cache] eviction: {} files ({:.1} MB) — cache {:.1}/{:.1} MB",
                files_evicted,
                bytes_freed as f64 / 1_048_576.0,
                remaining as f64 / 1_048_576.0,
                self.cache_limit as f64 / 1_048_576.0,
            );
        }
        (files_evicted, bytes_freed)
    }

    /// Cheap single-query stats for the UI: (hydrated_bytes, hydrated_count).
    pub fn cache_stats(&self) -> (u64, u64) {
        let db = self.db();
        let row: Option<(i64, i64)> = db
            .prepare_cached(
                "SELECT COALESCE(SUM(hydrated_size), 0), COUNT(*)
                 FROM known_files WHERE is_hydrated=1 AND is_dir=0",
            )
            .ok()
            .and_then(|mut stmt| {
                stmt.query_row([], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?)))
                    .ok()
            });
        row.map(|(b, c)| (b as u64, c as u64)).unwrap_or((0, 0))
    }

    /// Drain all cached content for this mount. Deletes cache files, resets
    /// DB rows to uncached. Returns (files_drained, bytes_freed).
    pub fn drain_all(&self) -> (u64, u64) {
        let candidates: Vec<(i64, String, u64)> = {
            let db = self.db();
            let mut stmt = match db.prepare(
                "SELECT rowid, path, hydrated_size FROM known_files
                 WHERE is_hydrated=1 AND is_dir=0",
            ) {
                Ok(s) => s,
                Err(e) => {
                    log::error!("[cache] drain_all prepare failed: {}", e);
                    return (0, 0);
                }
            };
            stmt.query_map([], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?, row.get::<_, i64>(2)? as u64))
            })
            .ok()
            .map(|it| it.filter_map(|r| r.ok()).collect())
            .unwrap_or_default()
        };

        let mut count = 0u64;
        let mut bytes = 0u64;

        for (rowid, _path, size) in &candidates {
            let lock = self.key_lock(*rowid);
            let Ok(_guard) = lock.try_write() else {
                continue;
            };
            let cache_path = self.cache_file_path(*rowid);
            let _ = std::fs::remove_file(&cache_path);
            count += 1;
            bytes += size;
        }

        let db = self.db();
        let _ = db.execute(
            "UPDATE known_files SET is_hydrated=0, hydrated_size=0, chunk_bitmap=NULL
             WHERE is_hydrated=1",
            [],
        );

        log::info!(
            "[cache] drain_all: {} files, {:.1} MB",
            count,
            bytes as f64 / 1_048_576.0,
        );
        (count, bytes)
    }

    // ── Metadata ──

    /// Get last_connected_at timestamp.
    pub fn last_connected_at(&self) -> Option<i64> {
        let db = self.db();
        db.query_row(
            "SELECT value FROM metadata WHERE key = 'last_connected_at'",
            [],
            |row| row.get::<_, String>(0),
        )
        .ok()
        .and_then(|v| v.parse::<i64>().ok())
    }

    /// Update last_connected_at to now.
    pub fn update_last_connected(&self) {
        let now = Self::unix_now();
        let db = self.db();
        let _ = db.execute(
            "INSERT OR REPLACE INTO metadata (key, value) VALUES ('last_connected_at', ?1)",
            params![now.to_string()],
        );
    }

    fn unix_now() -> i64 {
        cache_core::unix_now_secs()
    }

    /// Inline eviction after hydration: delegates to evict_over_budget_now.
    fn evict_if_over_budget(&self) {
        self.evict_over_budget_now();
    }

    /// Clear ALL cached data for this mount. Returns (files_cleared, bytes_cleared).
    /// Delegates to `drain_all` which handles cache-file cleanup.
    pub fn clear_all(&self) -> (u32, u64) {
        let (count, bytes) = self.drain_all();
        (count as u32, bytes)
    }

    /// Rebuild the cache index. Clears all tracking on `reset_all=true`.
    pub fn rebuild(&self, reset_all: bool) {
        log::info!(
            "[cache] Rebuilding cache index from {:?} (reset_all={})",
            self.client_root,
            reset_all
        );

        if reset_all {
            // Delete all cache files.
            let dir = self.cache_file_dir();
            if let Ok(rd) = std::fs::read_dir(&dir) {
                for entry in rd.flatten() {
                    let _ = std::fs::remove_file(entry.path());
                }
            }
            let db = self.db();
            let _ = db.execute("DELETE FROM known_files", []);
            let _ = db.execute("DELETE FROM visited_folders", []);
            let _ = db.execute("DELETE FROM metadata", []);
            return;
        }

        self.scan_and_index(&self.client_root);
    }

    fn scan_and_index(&self, _dir: &Path) {
        // ProjFS: cache files live in by_key/ keyed by rowid. Scan the cache
        // directory and reconcile against known_files. Any cache file without
        // a matching known_files row is orphaned and deleted.
        let cache_dir = self.cache_file_dir();
        if let Ok(rd) = std::fs::read_dir(&cache_dir) {
            for entry in rd.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if let Ok(rowid) = i64::from_str_radix(&name, 16) {
                    let db = self.db();
                    let exists: bool = db
                        .prepare_cached("SELECT 1 FROM known_files WHERE rowid = ?1")
                        .ok()
                        .and_then(|mut stmt| stmt.query_row(params![rowid], |_| Ok(true)).ok())
                        .unwrap_or(false);
                    if !exists {
                        let _ = std::fs::remove_file(entry.path());
                    }
                }
            }
        }
    }
}

/// Result of a stat-and-refresh call against the NAS.
#[derive(Debug)]
pub enum StatResult {
    /// Within TTL — no stat was performed. Caller can serve cached data.
    Skipped,
    /// NAS was stat'd; matched cached metadata. `last_verified_at` was refreshed.
    Fresh { size: u64, mtime: i64 },
    /// NAS was stat'd; drifted from cache. DB updated with new values.
    /// Caller may want to invalidate OS-side cached content.
    Drifted { size: u64, mtime: i64 },
    /// Path unknown to cache — caller should let the normal hydration path
    /// populate a baseline. Fresh stat values provided for convenience.
    Unknown { size: u64, mtime: i64 },
    /// NAS stat failed (network error, not found, permission). Caller should
    /// log + continue; freshness check is optional, never block the open.
    Error(std::io::Error),
}

/// Lazy freshness primitive: TTL-gated NAS stat against the cache.
///
/// If the entry was verified within `ttl_secs`, returns `Skipped` without any
/// NAS traffic. Otherwise stats `nas_path` and either stamps verification
/// (match) or updates cached metadata (drift). On stat failure, returns
/// `Error` — callers should log and fall through to their normal path.
pub fn stat_and_refresh(
    cache: &CacheIndex,
    client_path: &Path,
    nas_path: &Path,
    ttl_secs: i64,
) -> StatResult {
    let (cached_size, cached_mtime) = match cache.needs_verification(client_path, ttl_secs) {
        Some(v) => v,
        None => {
            // Either within TTL or unknown path. Disambiguate.
            if cache.is_known(client_path) {
                return StatResult::Skipped;
            }
            // Unknown — stat NAS anyway so caller has fresh values to work with,
            // but don't write to the DB (no row to update).
            return match std::fs::metadata(nas_path) {
                Ok(m) => StatResult::Unknown {
                    size: m.len(),
                    mtime: mtime_secs(&m),
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
    let nas_mtime = mtime_secs(&meta);

    if nas_size == cached_size && nas_mtime == cached_mtime {
        cache.record_verification(client_path);
        StatResult::Fresh {
            size: nas_size,
            mtime: nas_mtime,
        }
    } else {
        cache.update_nas_metadata(client_path, nas_size, nas_mtime);
        StatResult::Drifted {
            size: nas_size,
            mtime: nas_mtime,
        }
    }
}

fn mtime_secs(meta: &std::fs::Metadata) -> i64 {
    meta.modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}
