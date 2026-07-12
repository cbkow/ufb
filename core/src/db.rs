use r2d2::Pool;
use r2d2_sqlite::SqliteConnectionManager;
use rusqlite::{Connection, Result as SqlResult};

/// Per-column counts returned by `Database::normalize_job_paths`.
/// Each field is the number of rows whose stored path string was
/// rewritten to the local-OS native form.
#[derive(Debug, Default, Clone, Copy)]
pub struct NormalizeStats {
    pub column_defs: usize,
    pub meta_job: usize,
    pub meta_item: usize,
    pub subs: usize,
    pub bookmarks: usize,
}

/// Per-action counts from `Database::classify_all_path_columns`
/// (Tier 2 of the path-identity migration).
#[derive(Debug, Default, Clone, Copy)]
pub struct ClassifyStats {
    /// Rows whose path column(s) were rewritten to tagged-identity form.
    pub converted: usize,
    /// Rows moved to `migration_orphans` — a synced-table row whose
    /// path is not under any registered volume.
    pub orphaned: usize,
    /// Rows dropped as the losing side of a UNIQUE collision once two
    /// legacy path forms classified to the same identity.
    pub deduped: usize,
}
use std::path::PathBuf;

const POOL_SIZE: u32 = 6;

/// Manages the shared SQLite database via an r2d2 connection pool (WAL mode).
/// All managers access the DB through `with_conn`, which hands out a pooled
/// connection for the duration of the closure. Readers don't block each other;
/// writers still serialize at the SQLite level (WAL one-writer).
pub struct Database {
    pool: Pool<SqliteConnectionManager>,
}

impl Database {
    /// Open (or create) the UFB database at the given path.
    pub fn open(path: &PathBuf) -> SqlResult<Self> {
        // Defensive pre-create: on Windows, SQLite's CREATE codepath
        // through `winFullPathname` can fail with "unable to open
        // database" when the path contains an 8.3 short-name component
        // (e.g. profiles whose %LOCALAPPDATA% resolves to
        // C:\Users\UNIONG~1\... rather than the long form). The
        // OPEN-EXISTING branch handles such paths correctly, so we
        // touch the file with std::fs first and let SQLite open it
        // existing. Idempotent — no-op when the file already exists,
        // which is the common case after first launch. Same pattern
        // mesh's `take_snapshot` uses for staged snapshot files.
        if let Some(parent) = path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        if !path.exists() {
            let _ = std::fs::OpenOptions::new()
                .create(true)
                .write(true)
                .truncate(false)
                .open(path);
        }

        // One-time setup on a serial connection: enable WAL (persistent on disk)
        // + set pragmas the pool's init closure can't set (journal_mode).
        {
            let conn = Connection::open(path)?;
            conn.execute_batch(
                "PRAGMA journal_mode=WAL;
                 PRAGMA foreign_keys=ON;
                 PRAGMA busy_timeout=5000;",
            )?;
        }

        let manager = SqliteConnectionManager::file(path).with_init(|c| {
            c.execute_batch(
                "PRAGMA foreign_keys=ON;
                 PRAGMA busy_timeout=5000;
                 PRAGMA synchronous=NORMAL;",
            )
        });
        let pool = Pool::builder()
            .max_size(POOL_SIZE)
            .build(manager)
            .map_err(|e| rusqlite::Error::ToSqlConversionFailure(Box::new(e)))?;

        Ok(Self { pool })
    }

    /// Open an in-memory database (for testing).
    /// Note: in-memory DBs are per-connection, so the pool shares via a named
    /// shared-cache URI so all pooled connections see the same data.
    #[allow(dead_code)]
    pub fn open_in_memory() -> SqlResult<Self> {
        let manager = SqliteConnectionManager::memory()
            .with_init(|c| c.execute_batch("PRAGMA foreign_keys=ON;"));
        let pool = Pool::builder()
            .max_size(1) // single-conn for in-memory to keep schema shared
            .build(manager)
            .map_err(|e| rusqlite::Error::ToSqlConversionFailure(Box::new(e)))?;
        Ok(Self { pool })
    }

    /// Execute a closure with a reference to a pooled connection.
    ///
    /// The connection is returned to the pool automatically when the closure
    /// returns. Keep the closure body short; long-running work should not hold
    /// a connection longer than necessary.
    pub fn with_conn<F, T>(&self, f: F) -> SqlResult<T>
    where
        F: FnOnce(&Connection) -> SqlResult<T>,
    {
        let conn = self
            .pool
            .get()
            .map_err(|e| rusqlite::Error::ToSqlConversionFailure(Box::new(e)))?;
        f(&conn)
    }

    /// Execute a closure with a MUTABLE reference to a pooled connection.
    /// Required for `conn.transaction()` which needs `&mut Connection`.
    #[allow(dead_code)]
    pub fn with_conn_mut<F, T>(&self, f: F) -> SqlResult<T>
    where
        F: FnOnce(&mut Connection) -> SqlResult<T>,
    {
        let mut conn = self
            .pool
            .get()
            .map_err(|e| rusqlite::Error::ToSqlConversionFailure(Box::new(e)))?;
        f(&mut conn)
    }

    /// 0.9.97 — normalize all path-bearing columns to the local-OS
    /// native form so cross-OS mesh syncs don't leave foreign-form
    /// strings (`/Volumes/...` on a Windows DB or `C:\Volumes\...` on
    /// a Mac DB) that silently miss local-form WHERE queries.
    /// Called from boot migration (user_version=2 for the original
    /// pair, user_version=3 added subscriptions/item_path/bookmarks)
    /// AND from the snapshot-restore tail. Idempotent.
    ///
    /// Returns NormalizeStats { column_defs, meta_job, meta_item, subs, bookmarks }.
    pub fn normalize_job_paths(conn: &Connection) -> NormalizeStats {
        let settings = crate::settings::AppSettings::load();
        let mappings = &settings.path_mappings;

        let canonicalize = |p: &str| -> Option<String> {
            let c = crate::utils::auto_canonicalize_path(p, mappings);
            if c != p { Some(c) } else { None }
        };

        // ── column_definitions.job_path (PK = id, no UNIQUE on path) ──
        let mut col_updates: Vec<(i64, String)> = Vec::new();
        if let Ok(mut stmt) = conn.prepare("SELECT id, job_path FROM column_definitions") {
            if let Ok(rows) = stmt.query_map([], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
            }) {
                for r in rows.flatten() {
                    if let Some(c) = canonicalize(&r.1) {
                        col_updates.push((r.0, c));
                    }
                }
            }
        }
        for (id, c) in &col_updates {
            let _ = conn.execute(
                "UPDATE column_definitions SET job_path = ?1 WHERE id = ?2",
                rusqlite::params![c, id],
            );
        }

        // ── item_metadata.job_path (no UNIQUE) ──
        let mut meta_job_updates: Vec<(String, String)> = Vec::new();
        if let Ok(mut stmt) = conn.prepare("SELECT item_path, job_path FROM item_metadata") {
            if let Ok(rows) = stmt.query_map([], |row| {
                Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
            }) {
                for r in rows.flatten() {
                    if let Some(c) = canonicalize(&r.1) {
                        meta_job_updates.push((r.0, c));
                    }
                }
            }
        }
        for (item_path, c) in &meta_job_updates {
            let _ = conn.execute(
                "UPDATE item_metadata SET job_path = ?1 WHERE item_path = ?2",
                rusqlite::params![c, item_path],
            );
        }

        // ── item_metadata.item_path (PRIMARY KEY) ──
        // PK conflict means the local-form row already exists; in that
        // case keep the row with the newer modified_time and delete
        // the loser.
        let mut meta_item_updates: Vec<(String, String)> = Vec::new();
        if let Ok(mut stmt) = conn.prepare("SELECT item_path FROM item_metadata") {
            if let Ok(rows) = stmt.query_map([], |row| row.get::<_, String>(0)) {
                for r in rows.flatten() {
                    if let Some(c) = canonicalize(&r) {
                        meta_item_updates.push((r, c));
                    }
                }
            }
        }
        let mut meta_item_done = 0usize;
        for (old, new) in &meta_item_updates {
            let collision: Option<(i64, i64)> = conn
                .query_row(
                    "SELECT a.rowid, b.rowid FROM item_metadata a, item_metadata b
                     WHERE a.item_path = ?1 AND b.item_path = ?2",
                    rusqlite::params![old, new],
                    |row| Ok((row.get::<_, i64>(0)?, row.get::<_, i64>(1)?)),
                )
                .ok();
            if collision.is_some() {
                // Both rows exist — keep the newer modified_time, drop the older.
                let _ = conn.execute(
                    "DELETE FROM item_metadata WHERE item_path = ?1
                     AND COALESCE(modified_time, 0) <=
                         (SELECT COALESCE(modified_time, 0) FROM item_metadata WHERE item_path = ?2)",
                    rusqlite::params![old, new],
                );
                // If the old row survived (it was newer), repoint to canonical
                let still_old: bool = conn
                    .query_row(
                        "SELECT 1 FROM item_metadata WHERE item_path = ?1",
                        rusqlite::params![old],
                        |_| Ok(true),
                    )
                    .unwrap_or(false);
                if still_old {
                    let _ = conn.execute(
                        "DELETE FROM item_metadata WHERE item_path = ?1",
                        rusqlite::params![new],
                    );
                    if conn
                        .execute(
                            "UPDATE item_metadata SET item_path = ?1 WHERE item_path = ?2",
                            rusqlite::params![new, old],
                        )
                        .unwrap_or(0)
                        > 0
                    {
                        meta_item_done += 1;
                    }
                }
            } else if conn
                .execute(
                    "UPDATE item_metadata SET item_path = ?1 WHERE item_path = ?2",
                    rusqlite::params![new, old],
                )
                .unwrap_or(0)
                > 0
            {
                meta_item_done += 1;
            }
        }

        // ── subscriptions.job_path (UNIQUE) ──
        // Conflict resolution: keep the row with the newer modified_time.
        let mut sub_updates: Vec<(i64, String, String)> = Vec::new();
        if let Ok(mut stmt) = conn.prepare("SELECT id, job_path FROM subscriptions") {
            if let Ok(rows) = stmt.query_map([], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
            }) {
                for r in rows.flatten() {
                    if let Some(c) = canonicalize(&r.1) {
                        sub_updates.push((r.0, r.1, c));
                    }
                }
            }
        }
        let mut subs_done = 0usize;
        for (id, _old, new) in &sub_updates {
            let conflict_id: Option<i64> = conn
                .query_row(
                    "SELECT id FROM subscriptions WHERE job_path = ?1 AND id != ?2",
                    rusqlite::params![new, id],
                    |row| row.get(0),
                )
                .ok();
            if let Some(other) = conflict_id {
                // Two rows with foreign + local form. Keep the one with
                // newer modified_time; delete the other; repoint if needed.
                let mine_mt: i64 = conn
                    .query_row(
                        "SELECT COALESCE(modified_time, 0) FROM subscriptions WHERE id = ?1",
                        rusqlite::params![id],
                        |row| row.get(0),
                    )
                    .unwrap_or(0);
                let other_mt: i64 = conn
                    .query_row(
                        "SELECT COALESCE(modified_time, 0) FROM subscriptions WHERE id = ?1",
                        rusqlite::params![other],
                        |row| row.get(0),
                    )
                    .unwrap_or(0);
                if mine_mt >= other_mt {
                    let _ = conn.execute(
                        "DELETE FROM subscriptions WHERE id = ?1",
                        rusqlite::params![other],
                    );
                    if conn
                        .execute(
                            "UPDATE subscriptions SET job_path = ?1 WHERE id = ?2",
                            rusqlite::params![new, id],
                        )
                        .unwrap_or(0)
                        > 0
                    {
                        subs_done += 1;
                    }
                } else {
                    let _ = conn.execute(
                        "DELETE FROM subscriptions WHERE id = ?1",
                        rusqlite::params![id],
                    );
                    subs_done += 1;
                }
            } else if conn
                .execute(
                    "UPDATE subscriptions SET job_path = ?1 WHERE id = ?2",
                    rusqlite::params![new, id],
                )
                .unwrap_or(0)
                > 0
            {
                subs_done += 1;
            }
        }

        // ── bookmarks.path (UNIQUE; no modified_time on this table) ──
        // Conflict: prefer the older (lower id) row, drop the newer dup.
        let mut bookmark_updates: Vec<(i64, String, String)> = Vec::new();
        if let Ok(mut stmt) = conn.prepare("SELECT id, path FROM bookmarks") {
            if let Ok(rows) = stmt.query_map([], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
            }) {
                for r in rows.flatten() {
                    if let Some(c) = canonicalize(&r.1) {
                        bookmark_updates.push((r.0, r.1, c));
                    }
                }
            }
        }
        let mut bookmarks_done = 0usize;
        for (id, _old, new) in &bookmark_updates {
            let conflict_id: Option<i64> = conn
                .query_row(
                    "SELECT id FROM bookmarks WHERE path = ?1 AND id != ?2",
                    rusqlite::params![new, id],
                    |row| row.get(0),
                )
                .ok();
            if conflict_id.is_some() {
                // Drop our (foreign-form) row; the local-form one stays.
                let _ = conn.execute(
                    "DELETE FROM bookmarks WHERE id = ?1",
                    rusqlite::params![id],
                );
                bookmarks_done += 1;
            } else if conn
                .execute(
                    "UPDATE bookmarks SET path = ?1 WHERE id = ?2",
                    rusqlite::params![new, id],
                )
                .unwrap_or(0)
                > 0
            {
                bookmarks_done += 1;
            }
        }

        NormalizeStats {
            column_defs: col_updates.len(),
            meta_job: meta_job_updates.len(),
            meta_item: meta_item_done,
            subs: subs_done,
            bookmarks: bookmarks_done,
        }
    }

    /// Tier 2 of the path-identity migration: rewrite every
    /// path-bearing column to tagged-identity storage form
    /// (`vol:{uuid}/{rel}` or `native:{os}/{path}`).
    ///
    /// * A path under a registered volume becomes `vol:…`.
    /// * A `bookmarks` row may legitimately be `native:…` (a personal
    ///   bookmark) — those convert in place, never quarantined.
    /// * A row in a SYNCED table (`subscriptions`, `item_metadata`,
    ///   `column_definitions`, `column_layout_personal`) whose path is
    ///   NOT under a volume cannot be a shared identity — it is moved
    ///   to `migration_orphans` (the pre-migration DB backup is the
    ///   full recovery path).
    /// * When two legacy forms of one path classify to the same
    ///   identity, the resulting UNIQUE collision is resolved by newer
    ///   `modified_time` (bookmarks, which have none, keep the lower
    ///   `id`).
    ///
    /// Idempotent — a row already in tagged form classifies back to
    /// itself. `view` is the volume snapshot from `volumes::load_view`.
    pub fn classify_all_path_columns(
        conn: &Connection,
        view: &crate::identity::VolumeView,
    ) -> ClassifyStats {
        use crate::identity::{classify, Identity};
        let now = crate::utils::current_time_ms();
        let mut stats = ClassifyStats::default();

        // A legacy path string OR an already-tagged string → Identity.
        let tag = |p: &str| -> Identity {
            Identity::from_storage(p).unwrap_or_else(|| classify(p, view))
        };
        let quarantine =
            |table: &str, original: &str, detail: serde_json::Value| {
                let _ = conn.execute(
                    "INSERT INTO migration_orphans
                         (source_table, original_path, detail, quarantined_at)
                     VALUES (?1, ?2, ?3, ?4)",
                    rusqlite::params![table, original, detail.to_string(), now],
                );
            };

        // ── subscriptions.job_path (UNIQUE) ──
        let subs: Vec<(i64, String, Option<i64>)> = {
            let mut v = Vec::new();
            if let Ok(mut stmt) = conn
                .prepare("SELECT id, job_path, modified_time FROM subscriptions")
            {
                if let Ok(it) = stmt.query_map([], |r| {
                    Ok((
                        r.get::<_, i64>(0)?,
                        r.get::<_, String>(1)?,
                        r.get::<_, Option<i64>>(2)?,
                    ))
                }) {
                    v.extend(it.flatten());
                }
            }
            v
        };
        for (id, old, mtime) in subs {
            let ident = tag(&old);
            let new = ident.to_storage();
            if new == old {
                continue;
            }
            if !ident.is_volume() {
                quarantine("subscriptions", &old, serde_json::json!({ "id": id }));
                let _ = conn.execute(
                    "DELETE FROM subscriptions WHERE id = ?1",
                    rusqlite::params![id],
                );
                stats.orphaned += 1;
                continue;
            }
            let conflict: Option<(i64, Option<i64>)> = conn
                .query_row(
                    "SELECT id, modified_time FROM subscriptions
                     WHERE job_path = ?1 AND id != ?2",
                    rusqlite::params![new, id],
                    |r| Ok((r.get(0)?, r.get(1)?)),
                )
                .ok();
            if let Some((other_id, other_mt)) = conflict {
                let loser = if mtime.unwrap_or(0) >= other_mt.unwrap_or(0) {
                    other_id
                } else {
                    id
                };
                let _ = conn.execute(
                    "DELETE FROM subscriptions WHERE id = ?1",
                    rusqlite::params![loser],
                );
                if loser != id {
                    let _ = conn.execute(
                        "UPDATE subscriptions SET job_path = ?1 WHERE id = ?2",
                        rusqlite::params![new, id],
                    );
                }
                stats.deduped += 1;
            } else if conn
                .execute(
                    "UPDATE subscriptions SET job_path = ?1 WHERE id = ?2",
                    rusqlite::params![new, id],
                )
                .unwrap_or(0)
                > 0
            {
                stats.converted += 1;
            }
        }

        // ── item_metadata (item_path PK + job_path) ──
        let items: Vec<(String, String, Option<i64>)> = {
            let mut v = Vec::new();
            if let Ok(mut stmt) = conn.prepare(
                "SELECT item_path, job_path, modified_time FROM item_metadata",
            ) {
                if let Ok(it) = stmt.query_map([], |r| {
                    Ok((
                        r.get::<_, String>(0)?,
                        r.get::<_, String>(1)?,
                        r.get::<_, Option<i64>>(2)?,
                    ))
                }) {
                    v.extend(it.flatten());
                }
            }
            v
        };
        for (old_item, old_job, mtime) in items {
            let item_ident = tag(&old_item);
            let job_ident = tag(&old_job);
            if !item_ident.is_volume() || !job_ident.is_volume() {
                quarantine(
                    "item_metadata",
                    &old_item,
                    serde_json::json!({ "item_path": old_item, "job_path": old_job }),
                );
                let _ = conn.execute(
                    "DELETE FROM item_metadata WHERE item_path = ?1",
                    rusqlite::params![old_item],
                );
                stats.orphaned += 1;
                continue;
            }
            let new_item = item_ident.to_storage();
            let new_job = job_ident.to_storage();
            if new_item == old_item && new_job == old_job {
                continue;
            }
            if new_item != old_item {
                let other_mt: Option<Option<i64>> = conn
                    .query_row(
                        "SELECT modified_time FROM item_metadata WHERE item_path = ?1",
                        rusqlite::params![new_item],
                        |r| r.get(0),
                    )
                    .ok();
                if let Some(other_mt) = other_mt {
                    if mtime.unwrap_or(0) >= other_mt.unwrap_or(0) {
                        let _ = conn.execute(
                            "DELETE FROM item_metadata WHERE item_path = ?1",
                            rusqlite::params![new_item],
                        );
                        let _ = conn.execute(
                            "UPDATE item_metadata SET item_path = ?1, job_path = ?2
                             WHERE item_path = ?3",
                            rusqlite::params![new_item, new_job, old_item],
                        );
                    } else {
                        let _ = conn.execute(
                            "DELETE FROM item_metadata WHERE item_path = ?1",
                            rusqlite::params![old_item],
                        );
                    }
                    stats.deduped += 1;
                    continue;
                }
            }
            if conn
                .execute(
                    "UPDATE item_metadata SET item_path = ?1, job_path = ?2
                     WHERE item_path = ?3",
                    rusqlite::params![new_item, new_job, old_item],
                )
                .unwrap_or(0)
                > 0
            {
                stats.converted += 1;
            }
        }

        // ── column_definitions.job_path (id PK; the natural-key dedup
        //    block later in run_migrations collapses any logical
        //    duplicates this rewrite produces) ──
        let cols: Vec<(i64, String)> = {
            let mut v = Vec::new();
            if let Ok(mut stmt) =
                conn.prepare("SELECT id, job_path FROM column_definitions")
            {
                if let Ok(it) = stmt.query_map([], |r| {
                    Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?))
                }) {
                    v.extend(it.flatten());
                }
            }
            v
        };
        for (id, old) in cols {
            let ident = tag(&old);
            let new = ident.to_storage();
            if new == old {
                continue;
            }
            if !ident.is_volume() {
                quarantine(
                    "column_definitions",
                    &old,
                    serde_json::json!({ "id": id }),
                );
                let _ = conn.execute(
                    "DELETE FROM column_definitions WHERE id = ?1",
                    rusqlite::params![id],
                );
                stats.orphaned += 1;
            } else if conn
                .execute(
                    "UPDATE column_definitions SET job_path = ?1 WHERE id = ?2",
                    rusqlite::params![new, id],
                )
                .unwrap_or(0)
                > 0
            {
                stats.converted += 1;
            }
        }

        // ── column_layout_personal.job_path
        //    (UNIQUE(job_path, folder_name, column_name)) ──
        let layouts: Vec<(i64, String, String, String, Option<i64>)> = {
            let mut v = Vec::new();
            if let Ok(mut stmt) = conn.prepare(
                "SELECT id, job_path, folder_name, column_name, modified_time
                 FROM column_layout_personal",
            ) {
                if let Ok(it) = stmt.query_map([], |r| {
                    Ok((
                        r.get::<_, i64>(0)?,
                        r.get::<_, String>(1)?,
                        r.get::<_, String>(2)?,
                        r.get::<_, String>(3)?,
                        r.get::<_, Option<i64>>(4)?,
                    ))
                }) {
                    v.extend(it.flatten());
                }
            }
            v
        };
        for (id, old, folder, col, mtime) in layouts {
            let ident = tag(&old);
            let new = ident.to_storage();
            if new == old {
                continue;
            }
            if !ident.is_volume() {
                quarantine(
                    "column_layout_personal",
                    &old,
                    serde_json::json!({ "id": id }),
                );
                let _ = conn.execute(
                    "DELETE FROM column_layout_personal WHERE id = ?1",
                    rusqlite::params![id],
                );
                stats.orphaned += 1;
                continue;
            }
            let conflict: Option<(i64, Option<i64>)> = conn
                .query_row(
                    "SELECT id, modified_time FROM column_layout_personal
                     WHERE job_path = ?1 AND folder_name = ?2
                       AND column_name = ?3 AND id != ?4",
                    rusqlite::params![new, folder, col, id],
                    |r| Ok((r.get(0)?, r.get(1)?)),
                )
                .ok();
            if let Some((other_id, other_mt)) = conflict {
                let loser = if mtime.unwrap_or(0) >= other_mt.unwrap_or(0) {
                    other_id
                } else {
                    id
                };
                let _ = conn.execute(
                    "DELETE FROM column_layout_personal WHERE id = ?1",
                    rusqlite::params![loser],
                );
                if loser != id {
                    let _ = conn.execute(
                        "UPDATE column_layout_personal SET job_path = ?1 WHERE id = ?2",
                        rusqlite::params![new, id],
                    );
                }
                stats.deduped += 1;
            } else if conn
                .execute(
                    "UPDATE column_layout_personal SET job_path = ?1 WHERE id = ?2",
                    rusqlite::params![new, id],
                )
                .unwrap_or(0)
                > 0
            {
                stats.converted += 1;
            }
        }

        // ── bookmarks.path (UNIQUE; may be vol: OR native: — never
        //    quarantined. No modified_time — keep the lower id.) ──
        let bms: Vec<(i64, String)> = {
            let mut v = Vec::new();
            if let Ok(mut stmt) = conn.prepare("SELECT id, path FROM bookmarks") {
                if let Ok(it) = stmt.query_map([], |r| {
                    Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?))
                }) {
                    v.extend(it.flatten());
                }
            }
            v
        };
        for (id, old) in bms {
            let new = tag(&old).to_storage();
            if new == old {
                continue;
            }
            let conflict: Option<i64> = conn
                .query_row(
                    "SELECT id FROM bookmarks WHERE path = ?1 AND id != ?2",
                    rusqlite::params![new, id],
                    |r| r.get(0),
                )
                .ok();
            if let Some(other_id) = conflict {
                let loser = id.max(other_id);
                let _ = conn.execute(
                    "DELETE FROM bookmarks WHERE id = ?1",
                    rusqlite::params![loser],
                );
                if loser != id {
                    let _ = conn.execute(
                        "UPDATE bookmarks SET path = ?1 WHERE id = ?2",
                        rusqlite::params![new, id],
                    );
                }
                stats.deduped += 1;
            } else if conn
                .execute(
                    "UPDATE bookmarks SET path = ?1 WHERE id = ?2",
                    rusqlite::params![new, id],
                )
                .unwrap_or(0)
                > 0
            {
                stats.converted += 1;
            }
        }

        stats
    }

    /// Safely add a column if it doesn't already exist.
    fn add_column_if_missing(conn: &Connection, table: &str, column: &str, col_type: &str) {
        let sql = format!("ALTER TABLE {} ADD COLUMN {} {}", table, column, col_type);
        match conn.execute_batch(&sql) {
            Ok(()) => log::info!("Added column {}.{}", table, column),
            Err(_) => {} // Column already exists — expected
        }
    }

    /// Run all table-creation migrations.
    pub fn run_migrations(&self) -> SqlResult<()> {
        self.with_conn(|conn| {
            conn.execute_batch(
                "
                CREATE TABLE IF NOT EXISTS subscriptions (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    job_path TEXT NOT NULL UNIQUE,
                    job_name TEXT NOT NULL,
                    is_active INTEGER NOT NULL DEFAULT 1,
                    subscribed_time INTEGER NOT NULL,
                    last_sync_time INTEGER,
                    sync_status TEXT NOT NULL DEFAULT 'Pending',
                    shot_count INTEGER NOT NULL DEFAULT 0
                );
                CREATE INDEX IF NOT EXISTS idx_subs_active
                    ON subscriptions(is_active);

                CREATE TABLE IF NOT EXISTS item_metadata (
                    item_path TEXT NOT NULL,
                    job_path TEXT NOT NULL,
                    folder_name TEXT NOT NULL,
                    metadata_json TEXT NOT NULL DEFAULT '{}',
                    is_tracked INTEGER NOT NULL DEFAULT 0,
                    created_time INTEGER,
                    modified_time INTEGER,
                    device_id TEXT,
                    PRIMARY KEY (item_path)
                );
                CREATE INDEX IF NOT EXISTS idx_item_metadata_job
                    ON item_metadata(job_path);
                CREATE INDEX IF NOT EXISTS idx_item_metadata_tracked
                    ON item_metadata(is_tracked);
                CREATE INDEX IF NOT EXISTS idx_item_metadata_job_tracked
                    ON item_metadata(job_path, is_tracked);

                CREATE TABLE IF NOT EXISTS column_definitions (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    job_path TEXT NOT NULL,
                    folder_name TEXT NOT NULL,
                    column_name TEXT NOT NULL,
                    column_type TEXT NOT NULL DEFAULT 'text',
                    column_order INTEGER NOT NULL DEFAULT 0,
                    column_width REAL NOT NULL DEFAULT 120.0,
                    is_visible INTEGER NOT NULL DEFAULT 1,
                    default_value TEXT,
                    -- v5 epoch: stable cross-peer column identity
                    -- (crate::columns::derive_column_uuid). The cell-value
                    -- metadata blobs key on this, and the mesh merges on it.
                    column_uuid TEXT
                );
                CREATE INDEX IF NOT EXISTS idx_column_defs_job_folder
                    ON column_definitions(job_path, folder_name);

                CREATE TABLE IF NOT EXISTS column_options (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    column_id INTEGER NOT NULL,
                    option_name TEXT NOT NULL,
                    option_color TEXT,
                    FOREIGN KEY (column_id) REFERENCES column_definitions(id) ON DELETE CASCADE
                );

                -- Per-user view of (job, folder, column) layout —
                -- column_order, column_width, is_visible. Never
                -- mesh-broadcast. Falls back to column_definitions'
                -- defaults when no row is present. Keyed on
                -- column_name (not id) so it survives the
                -- mesh-driven re-create cycle that gives the same
                -- logical column a different local id on each peer.
                CREATE TABLE IF NOT EXISTS column_layout_personal (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    job_path TEXT NOT NULL,
                    folder_name TEXT NOT NULL,
                    column_name TEXT NOT NULL,
                    column_order INTEGER,
                    column_width REAL,
                    is_visible INTEGER,
                    modified_time INTEGER NOT NULL DEFAULT 0,
                    UNIQUE(job_path, folder_name, column_name)
                );
                CREATE INDEX IF NOT EXISTS idx_column_layout_personal_lookup
                    ON column_layout_personal(job_path, folder_name);

                CREATE TABLE IF NOT EXISTS bookmarks (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    path TEXT NOT NULL UNIQUE,
                    display_name TEXT NOT NULL,
                    created_time INTEGER NOT NULL,
                    is_project_folder INTEGER NOT NULL DEFAULT 0
                );

                CREATE TABLE IF NOT EXISTS column_presets (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    preset_name TEXT NOT NULL UNIQUE,
                    columns_json TEXT NOT NULL,
                    created_time INTEGER NOT NULL,
                    modified_time INTEGER NOT NULL
                );

                -- Path-identity migration (Tier 1): the volume registry.
                -- `volumes` is mesh-synced — the shared truth of what
                -- storage exists. `volume_bindings` is per-machine and
                -- NEVER synced — where this machine has each volume
                -- mounted. See plans/path-identity-migration.md.
                CREATE TABLE IF NOT EXISTS volumes (
                    uuid TEXT PRIMARY KEY,
                    display_name TEXT NOT NULL,
                    kind TEXT NOT NULL DEFAULT 'smb',
                    connection_hints TEXT NOT NULL DEFAULT '{}',
                    modified_time INTEGER NOT NULL DEFAULT 0,
                    deleted_at INTEGER
                );
                CREATE TABLE IF NOT EXISTS volume_bindings (
                    uuid TEXT PRIMARY KEY,
                    local_mount_root TEXT NOT NULL,
                    bound_time INTEGER NOT NULL,
                    auto_discovered INTEGER NOT NULL DEFAULT 0
                );

                -- Path-identity migration (Tier 2): rows from synced
                -- tables whose path is not under any registered volume,
                -- moved here by classify_all_path_columns so the
                -- migration never silently drops a subscription.
                CREATE TABLE IF NOT EXISTS migration_orphans (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    source_table TEXT NOT NULL,
                    original_path TEXT NOT NULL,
                    detail TEXT NOT NULL DEFAULT '{}',
                    quarantined_at INTEGER NOT NULL
                );
                ",
            )?;
            // NOTE: the thumbnail cache is NOT here. It lives in a
            // dedicated SQLite file owned by the C++ thumbnail
            // subsystem (app/thumbnails/ThumbCache.cpp) under the OS
            // cache dir — keeping encoded image blobs out of the
            // metadata DB avoids bloat + WAL write-contention and lets
            // the cache be reclaimed/wiped independently.

            // ── Schema migrations for C++ app DB compatibility ──
            // The C++ app's item_metadata has no device_id column
            Self::add_column_if_missing(conn, "item_metadata", "device_id", "TEXT");

            // ── v0.7.0 granular mesh sync migrations ──
            //
            // Every syncable row needs a logical timestamp (`modified_time`)
            // and a soft-delete flag (`deleted_at`). Together they let the
            // snapshot-restore path do three-case per-row merges (insert
            // missing, update-if-newer, keep local) and propagate deletions
            // through the snapshot channel — instead of today's additive-
            // only restore that can't resolve divergence or deletion.
            //
            // Existing rows get modified_time=0 (loses to any non-zero) and
            // deleted_at=NULL (active). First write bumps correctly.
            Self::add_column_if_missing(
                conn,
                "column_definitions",
                "modified_time",
                "INTEGER NOT NULL DEFAULT 0",
            );
            Self::add_column_if_missing(
                conn,
                "column_options",
                "modified_time",
                "INTEGER NOT NULL DEFAULT 0",
            );
            Self::add_column_if_missing(
                conn,
                "subscriptions",
                "modified_time",
                "INTEGER NOT NULL DEFAULT 0",
            );
            Self::add_column_if_missing(conn, "item_metadata", "deleted_at", "INTEGER");
            Self::add_column_if_missing(conn, "column_definitions", "deleted_at", "INTEGER");
            Self::add_column_if_missing(conn, "column_options", "deleted_at", "INTEGER");
            Self::add_column_if_missing(conn, "column_presets", "deleted_at", "INTEGER");
            Self::add_column_if_missing(conn, "subscriptions", "deleted_at", "INTEGER");
            // 2026-05-09 — auto-promote-templates work. Each
            // column_definitions row gains an optional pointer to a
            // shared template at <farm>/ufb/templates/<uuid>.json.
            // NULL = legacy row that hasn't been migrated yet
            // (handled by Phase 7 migration). Mesh col_add /
            // col_update payloads include this field; old peers that
            // don't know about it ignore the unknown key on
            // deserialise — no protocol bump needed.
            Self::add_column_if_missing(conn, "column_definitions", "template_hash", "TEXT");

            // ── 0.9.97 — column trashcan + rename-on-untrash ──
            //
            // `trashed_at` (broadcast via mesh) marks a column as soft-trashed
            // — hidden from grids on every peer but recoverable via the Trash
            // dialog. `deleted_at` semantics narrow to "permanently deleted"
            // (post-empty-trash, future janitor candidate). The two are
            // independent dimensions: a row may be active (both NULL),
            // trashed (trashed_at set, deleted_at NULL), or permanently
            // deleted (deleted_at set; trashed_at typically also set).
            //
            // `name_conflict_marker` records the rejected display-name during
            // an untrash that hit a `RenameRequired` conflict, so the QML
            // dialog can surface "this name was taken — pick another."
            // Cleared once a successful rename completes the untrash.
            Self::add_column_if_missing(conn, "column_definitions", "trashed_at", "INTEGER");
            // v5 epoch — stable column identity. add_column_if_missing
            // handles a v4 DB that was created by an intermediate build
            // before the CREATE TABLE carried the column. Fresh v4 DBs
            // already have it from the CREATE above. NULLs are backfilled
            // deterministically by the every-boot pass below.
            Self::add_column_if_missing(conn, "column_definitions", "column_uuid", "TEXT");
            Self::add_column_if_missing(
                conn,
                "column_definitions",
                "name_conflict_marker",
                "TEXT",
            );

            // One-shot data migration via PRAGMA user_version. Existing
            // rows soft-deleted under the old "deleted_at = trash" meaning
            // get reclassified into the trashcan (recoverable) instead of
            // jumping straight to permanently-deleted under the new
            // semantics. Idempotent: user_version=1 sentinel ensures we
            // don't re-migrate freshly permanent-deleted rows on later
            // launches.
            let user_version: i64 = conn
                .query_row("PRAGMA user_version", [], |row| row.get(0))
                .unwrap_or(0);
            if user_version < 1 {
                conn.execute_batch(
                    "UPDATE column_definitions
                        SET trashed_at = deleted_at,
                            deleted_at = NULL
                      WHERE deleted_at IS NOT NULL
                        AND trashed_at IS NULL;
                     PRAGMA user_version = 1;",
                )
                .ok();
                log::info!("db: migrated legacy column_definitions.deleted_at to trashed_at");
            }

            // user_version=2 (0.9.97): canonicalize job_path on
            // column_definitions + item_metadata to Windows form.
            // Cross-OS mesh sync was inserting macOS-form job_paths
            // (`/Volumes/...`) verbatim, so a Windows peer's queries
            // (which normalise to `C:\Volumes\...`) silently missed
            // those rows. The grid + Column Manager would then surface
            // the columns as italic "discovered" entries via the
            // metadata-key fallback walk, with no Trash button or
            // Promote action — stranded rows the user couldn't act on.
            //
            // Strategy: load the user's path_mappings, walk every
            // job_path, and for any that translate from mac form to a
            // different (Windows) form via translate_path_to("mac",
            // "win", ...), update the row in place. After this the
            // existing dedup migration below collapses any duplicates
            // (same natural key after normalisation).
            if user_version < 2 {
                let s = Self::normalize_job_paths(conn);
                conn.execute_batch("PRAGMA user_version = 2;").ok();
                log::info!(
                    "db: canonicalized {} column_definitions + {} item_metadata job_paths to local form",
                    s.column_defs, s.meta_job
                );
            }

            // user_version=3 (0.9.99): extend the canonicalisation pass
            // to columns the v2 migration missed — `subscriptions.job_path`,
            // `item_metadata.item_path`, `bookmarks.path`. These were
            // arriving from cross-OS mesh in the sender's form and
            // never normalised, so a Mac DB ended up with canonical
            // `C:\…` strings in subscriptions while the rest of the
            // tables held `/Volumes/…`. Folder-scoped metadata queries
            // (`folder_item_metadata`) silently returned 0 rows for
            // jobs whose item_paths sat in foreign form.
            //
            // Idempotent: a second run is a no-op once paths are local.
            if user_version < 3 {
                let s = Self::normalize_job_paths(conn);
                conn.execute_batch("PRAGMA user_version = 3;").ok();
                log::info!(
                    "db v3: normalised paths — column_defs={} meta_job={} meta_item={} subs={} bookmarks={}",
                    s.column_defs, s.meta_job, s.meta_item, s.subs, s.bookmarks
                );
            }

            // Path-identity migration: seed the volume registry + this
            // machine's bindings from path mappings. Idempotent
            // (deterministic UUIDs + INSERT OR IGNORE). MUST run before
            // the user_version=4 block below so that migration has a
            // populated registry to classify paths against.
            match crate::volumes::seed_from_path_mappings(conn) {
                Ok(n) if n > 0 => {
                    log::info!("db: seeded {} volume(s) from path mappings", n)
                }
                Ok(_) => {}
                Err(e) => log::warn!("db: volume seed failed: {}", e),
            }

            // user_version=4 — Tier 2 of the path-identity migration:
            // rewrite every path column to tagged-identity form
            // (`vol:…` / `native:…`). Destructive + irreversible, so:
            //   1. back the DB up first via SQLite `VACUUM INTO` — a
            //      clean, WAL-safe, complete copy;
            //   2. run `classify_all_path_columns` inside a transaction
            //      (crash-atomic — a power loss rolls the whole thing
            //      back);
            //   3. `VACUUM` at the end to defragment.
            // A backup failure aborts the migration for this boot
            // (user_version stays 3, retried next boot) — we never
            // migrate without a backup. user_version is bumped only
            // after the migration transaction durably commits.
            if user_version < 4 {
                let db_file = conn.path().unwrap_or("").to_string();
                let is_real_file =
                    !db_file.is_empty() && !db_file.contains(":memory:");
                let backup_ok = if is_real_file {
                    let backup = format!(
                        "{}.pre-identity-{}",
                        db_file,
                        crate::utils::current_time_ms()
                    );
                    let escaped = backup.replace('\'', "''");
                    match conn.execute_batch(&format!(
                        "VACUUM INTO '{}';",
                        escaped
                    )) {
                        Ok(()) => {
                            log::info!(
                                "db: pre-migration backup written to {}",
                                backup
                            );
                            true
                        }
                        Err(e) => {
                            log::error!(
                                "db: pre-identity-migration backup failed ({}); skipping migration this boot",
                                e
                            );
                            false
                        }
                    }
                } else {
                    true // in-memory / test DB — no backup needed
                };
                if backup_ok {
                    let view = crate::volumes::load_view(conn);
                    match conn.unchecked_transaction() {
                        Ok(tx) => {
                            let stats =
                                Self::classify_all_path_columns(&tx, &view);
                            match tx.commit() {
                                Ok(()) => {
                                    conn.execute_batch(
                                        "PRAGMA user_version = 4;",
                                    )
                                    .ok();
                                    log::info!(
                                        "db v4: path-identity migration — {} converted, {} deduped, {} orphaned",
                                        stats.converted,
                                        stats.deduped,
                                        stats.orphaned
                                    );
                                    let _ = conn.execute_batch("VACUUM;");
                                }
                                Err(e) => log::error!(
                                    "db v4: migration commit failed ({}); will retry next boot",
                                    e
                                ),
                            }
                        }
                        Err(e) => log::error!(
                            "db v4: could not begin migration transaction: {}",
                            e
                        ),
                    }
                }
            }

            // ── Smart dedup column_definitions on natural key ──
            //
            // Earlier "keep highest modified_time" was naive — when
            // cross-OS snapshot drift created N copies of the same
            // natural key with the foreign-form path winning the
            // mtime comparison, the LOCAL promoted row got nuked,
            // leaving the surviving row unpromoted (template_hash
            // wiped). 0.9.97 repair pass:
            //
            //   1. For each duplicate group, pick the survivor by
            //      priority: has template_hash > active (not trashed,
            //      not deleted) > newest modified_time > highest id.
            //   2. BEFORE deleting siblings, copy template_hash from
            //      any sibling that has one to the survivor — so
            //      promotion state is preserved across the dedup.
            //   3. Delete the rest. column_options cascades via FK
            //      ON DELETE CASCADE.
            //
            // Idempotent: groups with only one row produce no work.
            // Runs every boot so future drift recoveries are
            // automatic.
            #[derive(Debug)]
            struct DupGroup {
                job_path: String,
                folder_name: String,
                column_name: String,
                survivor_id: i64,
                recovered_hash: Option<String>,
            }
            // Only dedup ACTIVE rows. Trashed and permanently-deleted
            // rows are independent state the user explicitly created
            // (Trash button → trashed_at; Empty Trash → deleted_at)
            // and must survive dedup so they remain reachable from
            // the Trash dialog / janitor. The earlier version
            // collapsed them too — silently nuking the user's just-
            // trashed row before they could empty-trash it.
            let groups: Vec<DupGroup> = {
                let mut stmt = conn.prepare(
                    "SELECT cd.job_path, cd.folder_name, cd.column_name,
                            (SELECT id FROM column_definitions cd2
                             WHERE cd2.job_path = cd.job_path
                               AND cd2.folder_name = cd.folder_name
                               AND cd2.column_name = cd.column_name
                               AND cd2.trashed_at IS NULL
                               AND cd2.deleted_at IS NULL
                             ORDER BY (template_hash IS NOT NULL) DESC,
                                      modified_time DESC,
                                      id DESC
                             LIMIT 1) AS survivor_id,
                            (SELECT template_hash FROM column_definitions cd3
                             WHERE cd3.job_path = cd.job_path
                               AND cd3.folder_name = cd.folder_name
                               AND cd3.column_name = cd.column_name
                               AND cd3.template_hash IS NOT NULL
                               AND cd3.trashed_at IS NULL
                               AND cd3.deleted_at IS NULL
                             ORDER BY modified_time DESC, id DESC
                             LIMIT 1) AS recovered_hash
                     FROM column_definitions cd
                     WHERE cd.trashed_at IS NULL AND cd.deleted_at IS NULL
                     GROUP BY cd.job_path, cd.folder_name, cd.column_name
                     HAVING COUNT(*) > 1",
                )?;
                let rows = stmt.query_map([], |row| {
                    Ok(DupGroup {
                        job_path: row.get(0)?,
                        folder_name: row.get(1)?,
                        column_name: row.get(2)?,
                        survivor_id: row.get(3)?,
                        recovered_hash: row.get::<_, Option<String>>(4)?,
                    })
                })?;
                rows.collect::<Result<Vec<_>, _>>().unwrap_or_default()
            };
            for g in &groups {
                if let Some(ref h) = g.recovered_hash {
                    let _ = conn.execute(
                        "UPDATE column_definitions
                            SET template_hash = COALESCE(template_hash, ?1)
                          WHERE id = ?2",
                        rusqlite::params![h, g.survivor_id],
                    );
                }
                let _ = conn.execute(
                    "DELETE FROM column_definitions
                      WHERE job_path = ?1 AND folder_name = ?2 AND column_name = ?3
                        AND id != ?4
                        AND trashed_at IS NULL
                        AND deleted_at IS NULL",
                    rusqlite::params![g.job_path, g.folder_name, g.column_name, g.survivor_id],
                );
            }
            if !groups.is_empty() {
                log::info!(
                    "db: smart-deduped {} column_definitions natural-key groups (recovered template_hash on {})",
                    groups.len(),
                    groups.iter().filter(|g| g.recovered_hash.is_some()).count()
                );
            }

            // ── column_uuid backfill (additive, idempotent, every boot) ──
            // Assign a stable cross-peer identity to any row missing one.
            // Fresh v5 rows get theirs at INSERT; this catches rows created
            // by an intermediate build before column_uuid existed. Additive
            // only (writes a NULL→value), so no backup / user_version gate —
            // mirrors the dedup pass above. Derivation is deterministic so
            // two peers backfilling the same row converge.
            {
                let pending: Vec<(i64, String, String, String, String)> = {
                    match conn.prepare(
                        "SELECT id, job_path, folder_name, column_name, column_type
                           FROM column_definitions
                          WHERE column_uuid IS NULL OR column_uuid = ''",
                    ) {
                        Ok(mut stmt) => stmt
                            .query_map([], |row| {
                                Ok((
                                    row.get::<_, i64>(0)?,
                                    row.get::<_, String>(1)?,
                                    row.get::<_, String>(2)?,
                                    row.get::<_, String>(3)?,
                                    row.get::<_, String>(4)?,
                                ))
                            })
                            .and_then(|r| r.collect::<Result<Vec<_>, _>>())
                            .unwrap_or_default(),
                        Err(_) => Vec::new(),
                    }
                };
                let mut backfilled = 0usize;
                for (id, job, folder, name, ctype) in &pending {
                    let u = crate::columns::derive_column_uuid(job, folder, name, ctype);
                    if conn
                        .execute(
                            "UPDATE column_definitions SET column_uuid = ?1 WHERE id = ?2",
                            rusqlite::params![u, id],
                        )
                        .is_ok()
                    {
                        backfilled += 1;
                    }
                }
                if backfilled > 0 {
                    log::info!("db: backfilled column_uuid on {} legacy row(s)", backfilled);
                }
            }

            Ok(())
        })
    }
}
