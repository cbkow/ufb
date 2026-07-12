//! Dedicated snapshot worker.
//!
//! Runs `snapshot_to_db` / `restore_from_snapshot` on its own blocking
//! thread with its own `rusqlite::Connection` opened directly at the DB
//! file — bypassing the r2d2 pool. The pool has 6 slots; before this
//! module existed, every 30-second snapshot took one slot for its full
//! duration (multi-second NAS write) while also holding SQLite's WAL
//! writer lock. Under fan-in load, handler reads and writes piled up
//! behind it, which in turn kept axum handler futures alive while their
//! sockets sat in `CLOSE_WAIT`.
//!
//! Isolating the snapshot:
//! - Takes nothing from the handler pool.
//! - Writer-lock contention still exists at the SQLite level (WAL is
//!   single-writer), but that's bounded by `busy_timeout=5000` on both
//!   sides rather than blocking an entire pool connection.
//! - Runs `spawn_blocking`'d, so it's on tokio's blocking thread pool
//!   (default 512) which never starves handler futures.
//! - Snapshot requests arrive over a bounded (4) mpsc so duplicates
//!   coalesce into a small queue.

use rusqlite::{Connection, OpenFlags};
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use tokio::sync::{mpsc, oneshot};

#[derive(Serialize, Deserialize)]
struct SnapshotMeta {
    node_id: String,
    /// Wall-clock ms since epoch of the last successful write.
    written_at: i64,
}

const SNAPSHOT_YIELD_WINDOW_MS: i64 = 60_000;
const SNAPSHOT_CHANNEL_DEPTH: usize = 4;

/// Idempotent boot-time heal: stamp `modified_time` on every LWW-tracked
/// row that was created before the LWW columns existed (`NULL`). Without
/// this, the merge predicates in `restore_snapshot` (which require
/// `snap.modified_time > main.modified_time`) silently drop UPDATEs for
/// any row whose `modified_time` was never written — three-valued SQL
/// makes `NULL > anything` and `anything > NULL` both false.
///
/// Each node runs this on startup with `current_time_ms()`; nodes
/// converge to different timestamps for the same legacy row, but any
/// real edit afterwards stamps a fresh time and LWW resolves correctly.
/// Subsequent boots find no NULLs and the UPDATEs are no-ops.
///
/// Returns the total number of rows stamped across all tables.
pub fn backfill_null_modified_times(
    conn: &Connection,
    timestamp_ms: i64,
) -> Result<u64, String> {
    let tables = [
        "subscriptions",
        "column_definitions",
        "column_options",
        "item_metadata",
        // column_presets removed from snapshot sync — presets moved
        // to filesystem (<farm>/ufb/presets/) in commit b0a6c5f.
        // The local table still exists for any data the user
        // imported pre-migration, but it's no longer synced.
    ];
    let mut total: u64 = 0;
    for t in tables {
        // CHECK constraint on column_options' option_color etc. don't
        // care about modified_time — straight UPDATE is safe.
        let sql = format!(
            "UPDATE {} SET modified_time = ?1 WHERE modified_time IS NULL",
            t
        );
        match conn.execute(&sql, [timestamp_ms]) {
            Ok(n) => {
                if n > 0 {
                    log::info!(
                        "Mesh heal: stamped {} legacy NULL modified_time row(s) in {}",
                        n,
                        t
                    );
                }
                total += n as u64;
            }
            Err(e) => {
                // Don't fail startup on a single table — log and keep going
                // (e.g. a table missing on a fresh DB before migrations land).
                log::warn!(
                    "Mesh heal: backfill on {} failed (continuing): {}",
                    t,
                    e
                );
            }
        }
    }
    Ok(total)
}

pub enum SnapshotRequest {
    Take {
        reply: oneshot::Sender<Result<(), String>>,
    },
    Restore {
        include_column_tables: bool,
        reply: oneshot::Sender<Result<(), String>>,
    },
    Shutdown,
}

pub struct SnapshotWorker {
    tx: mpsc::Sender<SnapshotRequest>,
    task: Option<tokio::task::JoinHandle<()>>,
}

impl SnapshotWorker {
    pub fn start(
        db_path: PathBuf,
        farm_path: String,
        node_id: String,
    ) -> Result<Self, String> {
        let (tx, mut rx) = mpsc::channel::<SnapshotRequest>(SNAPSHOT_CHANNEL_DEPTH);
        let task = tokio::task::spawn_blocking(move || {
            let mut conn = match Connection::open_with_flags(
                &db_path,
                OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_URI,
            ) {
                Ok(c) => c,
                Err(e) => {
                    log::error!(
                        "SnapshotWorker: failed to open DB {:?}: {}",
                        db_path,
                        e
                    );
                    return;
                }
            };
            if let Err(e) = conn.execute_batch(
                "PRAGMA foreign_keys=ON;
                 PRAGMA busy_timeout=5000;
                 PRAGMA synchronous=NORMAL;",
            ) {
                log::warn!("SnapshotWorker: failed to set pragmas: {}", e);
            }

            log::info!(
                "SnapshotWorker started (db={:?}, farm={})",
                db_path,
                farm_path
            );

            while let Some(req) = rx.blocking_recv() {
                match req {
                    SnapshotRequest::Take { reply } => {
                        let result = take_snapshot(&mut conn, &farm_path, &node_id);
                        // Log the outcome here regardless of whether the
                        // reply channel still has a live receiver. The
                        // periodic / catch-up dispatchers in coordinator
                        // intentionally drop the receiver (fire-and-forget),
                        // so any Err returned by `take_snapshot` would
                        // otherwise vanish silently — which is exactly
                        // how a leader can spend hours "writing" snapshots
                        // that never land on the NAS.
                        match &result {
                            Ok(()) => log::debug!("SnapshotWorker: Take completed Ok"),
                            Err(e) => log::warn!("SnapshotWorker: Take failed: {}", e),
                        }
                        let _ = reply.send(result);
                    }
                    SnapshotRequest::Restore {
                        include_column_tables,
                        reply,
                    } => {
                        let result = restore_snapshot(&mut conn, &farm_path, include_column_tables);
                        match &result {
                            Ok(()) => log::debug!("SnapshotWorker: Restore completed Ok"),
                            Err(e) => log::warn!("SnapshotWorker: Restore failed: {}", e),
                        }
                        let _ = reply.send(result);
                    }
                    SnapshotRequest::Shutdown => {
                        log::info!("SnapshotWorker received shutdown");
                        break;
                    }
                }
            }

            log::info!("SnapshotWorker stopped");
        });

        Ok(Self {
            tx,
            task: Some(task),
        })
    }

    pub fn sender(&self) -> mpsc::Sender<SnapshotRequest> {
        self.tx.clone()
    }

    pub fn request_take(&self) {
        let (reply, _rx) = oneshot::channel();
        match self.tx.try_send(SnapshotRequest::Take { reply }) {
            Ok(()) => {}
            Err(mpsc::error::TrySendError::Full(_)) => {
                log::debug!("SnapshotWorker: channel full, skipping this take");
            }
            Err(mpsc::error::TrySendError::Closed(_)) => {
                log::warn!("SnapshotWorker: channel closed, can't take snapshot");
            }
        }
    }

    pub async fn take(&self) -> Result<(), String> {
        let (reply_tx, reply_rx) = oneshot::channel();
        self.tx
            .send(SnapshotRequest::Take { reply: reply_tx })
            .await
            .map_err(|_| "SnapshotWorker channel closed".to_string())?;
        reply_rx
            .await
            .map_err(|_| "SnapshotWorker dropped reply".to_string())?
    }

    pub async fn restore(&self, include_column_tables: bool) -> Result<(), String> {
        let (reply_tx, reply_rx) = oneshot::channel();
        self.tx
            .send(SnapshotRequest::Restore {
                include_column_tables,
                reply: reply_tx,
            })
            .await
            .map_err(|_| "SnapshotWorker channel closed".to_string())?;
        reply_rx
            .await
            .map_err(|_| "SnapshotWorker dropped reply".to_string())?
    }

    pub async fn shutdown(&mut self) {
        let _ = self.tx.send(SnapshotRequest::Shutdown).await;
        if let Some(task) = self.task.take() {
            let _ = task.await;
        }
    }
}

fn take_snapshot(conn: &mut Connection, farm_path: &str, node_id: &str) -> Result<(), String> {
    let total: i64 = conn
        .query_row(
            "SELECT
               (SELECT COUNT(*) FROM main.item_metadata)
             + (SELECT COUNT(*) FROM main.column_definitions)
             + (SELECT COUNT(*) FROM main.subscriptions);",
            [],
            |row| row.get(0),
        )
        .unwrap_or(0);
    if total == 0 {
        log::warn!("Snapshot: all syncable tables empty, skipping");
        return Ok(());
    }

    let snapshot_dir = super::farm_version_root(farm_path).join("snapshots");
    if let Err(e) = std::fs::create_dir_all(&snapshot_dir) {
        return Err(format!("Failed to create snapshot dir: {}", e));
    }

    let meta_path = snapshot_dir.join(super::SNAPSHOT_META_FILENAME);
    let now_ms = crate::utils::current_time_ms();

    if let Ok(existing) = std::fs::read_to_string(&meta_path) {
        if let Ok(meta) = serde_json::from_str::<SnapshotMeta>(&existing) {
            let age = now_ms.saturating_sub(meta.written_at);
            if meta.node_id != node_id && age < SNAPSHOT_YIELD_WINDOW_MS {
                log::info!(
                    "Snapshot: yielding to {} (wrote {}ms ago) — skipping our write",
                    meta.node_id,
                    age
                );
                return Ok(());
            }
        }
    }

    // Stage the SQLite work on local disk, NEVER on the SMB share
    // directly. SQLite ATTACH against UNC paths is unreliable: byte-
    // range locking semantics differ across SMB versions, file create
    // can fail with ambiguous "unable to open database" errors when
    // the calling user has read-but-not-write ACLs (observed in 0.8.8
    // on the leader where `uniongraphics` could read the existing
    // snapshot but not create new files in `\\nas\snapshots\`).
    //
    // Build the snapshot DB locally → fs::copy bytes to the SMB tmp →
    // fs::rename SMB tmp to final. The SMB layer never sees SQLite,
    // just plain bytes + an atomic rename.
    //
    // Use %TEMP% (matches `restore_snapshot`'s proven pattern). Earlier
    // attempt at %LOCALAPPDATA%\ufb\snapshot-stage\ ran into "unable to
    // open database" on at least one machine — likely an ACL / folder-
    // redirection quirk on that profile. %TEMP% is the OS's blessed
    // user-writable scratch dir and works everywhere.
    let local_tmp_path = std::env::temp_dir().join(format!(
        "ufb_snapshot_v4_take_{}.db",
        crate::utils::current_time_ms()
    ));
    let smb_tmp_path =
        snapshot_dir.join(format!("{}.tmp", super::SNAPSHOT_FILENAME));
    let final_path = snapshot_dir.join(super::SNAPSHOT_FILENAME);
    // Clean any leftover from a prior crashed run; the unique
    // timestamp-suffixed name above means there's almost certainly
    // nothing to remove, but don't pile up cruft.
    let _ = std::fs::remove_file(&local_tmp_path);
    let _ = std::fs::remove_file(&smb_tmp_path);

    let snap_path_str = local_tmp_path.to_string_lossy().to_string();
    log::info!("Snapshot: staging build at {}", snap_path_str);

    // Pre-create an empty file at the staging path with std::fs.
    // 0.8.12 confirmed Rust can write the file but SQLite ATTACH
    // could not (re)create it at the same path moments later — the
    // path 8.3-resolved to UNIONG~1\... on this user's profile and
    // SQLite's Windows VFS apparently handles its CREATE codepath
    // differently than its OPEN-EXISTING codepath through
    // winFullPathname. Leaving the file in place lets SQLite take
    // the OPEN-EXISTING branch, which works on the same path. SQLite
    // treats an empty file as "new database" and writes the header
    // on first write, so functionally it's still a fresh DB.
    if let Err(e) = std::fs::write(&local_tmp_path, b"") {
        return Err(format!(
            "Snapshot: pre-create FAILED at {} — Windows OS error: {} (raw_os_error={:?})",
            snap_path_str,
            e,
            e.raw_os_error()
        ));
    }
    log::debug!("Snapshot: empty staging file pre-created at {}", snap_path_str);

    conn.execute_batch(&format!(
        "ATTACH DATABASE '{}' AS snap;",
        snap_path_str.replace('\'', "''")
    ))
    .map_err(|e| format!("ATTACH failed for {} (rusqlite reports: {})", snap_path_str, e))?;

    let create_tables = &[
        "CREATE TABLE IF NOT EXISTS snap.subscriptions (
             id INTEGER PRIMARY KEY AUTOINCREMENT,
             job_path TEXT NOT NULL UNIQUE,
             job_name TEXT NOT NULL,
             is_active INTEGER NOT NULL DEFAULT 1,
             subscribed_time INTEGER NOT NULL,
             last_sync_time INTEGER,
             sync_status TEXT NOT NULL DEFAULT 'Pending',
             shot_count INTEGER NOT NULL DEFAULT 0,
             modified_time INTEGER NOT NULL DEFAULT 0,
             deleted_at INTEGER
         );",
        // 0.9.97 — column_definitions snap table gains template_hash
        // (so a peer's promoted-template state replicates) and
        // trashed_at (so trashing on one peer flows to all). Both are
        // included in the copy + restore SQL below.
        "CREATE TABLE IF NOT EXISTS snap.column_definitions (
             id INTEGER PRIMARY KEY AUTOINCREMENT,
             job_path TEXT NOT NULL,
             folder_name TEXT NOT NULL,
             column_name TEXT NOT NULL,
             column_type TEXT NOT NULL DEFAULT 'text',
             column_order INTEGER NOT NULL DEFAULT 0,
             column_width REAL NOT NULL DEFAULT 120.0,
             is_visible INTEGER NOT NULL DEFAULT 1,
             default_value TEXT,
             modified_time INTEGER NOT NULL DEFAULT 0,
             deleted_at INTEGER,
             template_hash TEXT,
             trashed_at INTEGER,
             column_uuid TEXT
         );",
        "CREATE TABLE IF NOT EXISTS snap.column_options (
             id INTEGER PRIMARY KEY AUTOINCREMENT,
             column_id INTEGER NOT NULL,
             option_name TEXT NOT NULL,
             option_color TEXT,
             modified_time INTEGER NOT NULL DEFAULT 0,
             deleted_at INTEGER,
             FOREIGN KEY (column_id) REFERENCES column_definitions(id) ON DELETE CASCADE
         );",
        "CREATE TABLE IF NOT EXISTS snap.item_metadata (
             item_path TEXT NOT NULL,
             job_path TEXT NOT NULL,
             folder_name TEXT NOT NULL,
             metadata_json TEXT NOT NULL DEFAULT '{}',
             is_tracked INTEGER NOT NULL DEFAULT 0,
             created_time INTEGER,
             modified_time INTEGER,
             device_id TEXT,
             deleted_at INTEGER,
             PRIMARY KEY (item_path)
         );",
        // column_presets table CREATE removed alongside the snapshot
        // sync drop — presets moved to filesystem (b0a6c5f).
    ];
    for sql in create_tables {
        if let Err(e) = conn.execute_batch(sql) {
            let _ = conn.execute_batch("DETACH DATABASE snap;");
            return Err(format!("snapshot CREATE TABLE failed: {}", e));
        }
    }

    // The `WHERE job_path LIKE 'vol:%'` filters keep per-machine
    // (`native:`) rows out of the shared snapshot — only volume-backed
    // identities are synced. The snapshot carries tagged identities
    // verbatim (this is a v4-epoch snapshot, read only by new builds).
    let copy_tables: &[(&str, &str)] = &[
        (
            "subscriptions",
            "DELETE FROM snap.subscriptions;
              INSERT OR REPLACE INTO snap.subscriptions (id, job_path, job_name, is_active, subscribed_time, last_sync_time, sync_status, shot_count, modified_time, deleted_at)
                  SELECT id, job_path, job_name, is_active, subscribed_time, last_sync_time, sync_status, shot_count, modified_time, deleted_at
                  FROM main.subscriptions WHERE job_path LIKE 'vol:%';",
        ),
        (
            "column_definitions",
            "DELETE FROM snap.column_definitions;
              INSERT OR REPLACE INTO snap.column_definitions (id, job_path, folder_name, column_name, column_type, column_order, column_width, is_visible, default_value, modified_time, deleted_at, template_hash, trashed_at, column_uuid)
                  SELECT id, job_path, folder_name, column_name, column_type, column_order, column_width, is_visible, default_value, modified_time, deleted_at, template_hash, trashed_at, column_uuid
                  FROM main.column_definitions WHERE job_path LIKE 'vol:%';",
        ),
        (
            "column_options",
            "DELETE FROM snap.column_options;
              INSERT OR REPLACE INTO snap.column_options (id, column_id, option_name, option_color, modified_time, deleted_at)
                  SELECT id, column_id, option_name, option_color, modified_time, deleted_at
                  FROM main.column_options
                  WHERE column_id IN (SELECT id FROM main.column_definitions WHERE job_path LIKE 'vol:%');",
        ),
        (
            "item_metadata",
            "DELETE FROM snap.item_metadata;
              INSERT OR REPLACE INTO snap.item_metadata (item_path, job_path, folder_name, metadata_json, is_tracked, created_time, modified_time, device_id, deleted_at)
                  SELECT item_path, job_path, folder_name, metadata_json, is_tracked, created_time, modified_time, device_id, deleted_at
                  FROM main.item_metadata WHERE item_path LIKE 'vol:%' AND job_path LIKE 'vol:%';",
        ),
        // column_presets copy block removed — see CREATE TABLE
        // comment above. Presets ride the filesystem now.
    ];

    let mut any_copied = false;
    for (name, sql) in copy_tables {
        match conn.execute_batch(sql) {
            Ok(()) => {
                any_copied = true;
                log::debug!("Snapshot: copied {}", name);
            }
            Err(e) => {
                log::warn!("Snapshot: skipping {} ({})", name, e);
            }
        }
    }

    let _ = conn.execute_batch("DETACH DATABASE snap;");

    if !any_copied {
        let _ = std::fs::remove_file(&local_tmp_path);
        return Err("Snapshot: no tables copied".to_string());
    }

    // Push the locally-built snapshot up to the SMB share. The .tmp
    // intermediate on the share lets us do an atomic rename to the
    // final filename — followers polling mtime see either the old
    // snapshot or the new one, never a half-written file.
    if let Err(e) = std::fs::copy(&local_tmp_path, &smb_tmp_path) {
        let _ = std::fs::remove_file(&local_tmp_path);
        let _ = std::fs::remove_file(&smb_tmp_path);
        return Err(format!(
            "Snapshot copy to SMB failed ({} → {}): {}",
            local_tmp_path.display(),
            smb_tmp_path.display(),
            e
        ));
    }
    if let Err(e) = std::fs::rename(&smb_tmp_path, &final_path) {
        let _ = std::fs::remove_file(&local_tmp_path);
        let _ = std::fs::remove_file(&smb_tmp_path);
        return Err(format!("Snapshot rename failed: {}", e));
    }
    let _ = std::fs::remove_file(&local_tmp_path);

    let meta = SnapshotMeta {
        node_id: node_id.to_string(),
        written_at: now_ms,
    };
    match serde_json::to_string(&meta) {
        Ok(meta_json) => {
            let meta_tmp =
                snapshot_dir.join(format!("{}.tmp", super::SNAPSHOT_META_FILENAME));
            let _ = std::fs::remove_file(&meta_tmp);
            if let Err(e) = std::fs::write(&meta_tmp, meta_json) {
                log::warn!("Snapshot: sidecar write failed (non-fatal): {}", e);
            } else if let Err(e) = std::fs::rename(&meta_tmp, &meta_path) {
                log::warn!("Snapshot: sidecar rename failed (non-fatal): {}", e);
                let _ = std::fs::remove_file(&meta_tmp);
            }
        }
        Err(e) => log::warn!("Snapshot: sidecar serialize failed (non-fatal): {}", e),
    }

    log::info!("Snapshot written to {}", final_path.display());
    Ok(())
}

fn restore_snapshot(
    conn: &mut Connection,
    farm_path: &str,
    include_column_tables: bool,
) -> Result<(), String> {
    let snapshot_path = super::snapshot_file_path(farm_path);

    if !snapshot_path.exists() {
        log::info!(
            "No snapshot found at {}, nothing to restore",
            snapshot_path.display()
        );
        return Ok(());
    }

    // Copy the snapshot from NAS to a local temp file BEFORE attaching
    // (avoids holding the writer lock for the duration of a slow SMB read).
    let local_tmp = std::env::temp_dir().join(format!(
        "ufb_snapshot_restore_{}.db",
        crate::utils::current_time_ms()
    ));
    if let Err(e) = std::fs::copy(&snapshot_path, &local_tmp) {
        return Err(format!(
            "Failed to copy snapshot {} → {}: {}",
            snapshot_path.display(),
            local_tmp.display(),
            e
        ));
    }

    let snap_path_str = local_tmp.to_string_lossy().to_string();

    conn.execute_batch(&format!(
        "ATTACH DATABASE '{}' AS snap;",
        snap_path_str.replace('\'', "''")
    ))
    .map_err(|e| {
        let _ = std::fs::remove_file(&local_tmp);
        format!("ATTACH failed: {}", e)
    })?;

    // Legacy-schema compat: pre-0.7.0 snapshots predate the LWW columns
    // (`modified_time`, `deleted_at`) on most tables. The merge SQL below
    // unconditionally references them, so a legacy snap fails every
    // `INSERT WHERE NOT EXISTS` with "no such column". ALTER ADD COLUMN
    // is idempotent enough — duplicate-column errors on modern snaps are
    // expected and ignored. New rows get default 0 (modified_time) /
    // NULL (deleted_at), which makes them lose the LWW guard against
    // any locally-edited rows (safe) while still being inserted via
    // INSERT WHERE NOT EXISTS for anything missing locally.
    let legacy_compat: &[&str] = &[
        "ALTER TABLE snap.subscriptions ADD COLUMN modified_time INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE snap.subscriptions ADD COLUMN deleted_at INTEGER",
        "ALTER TABLE snap.column_definitions ADD COLUMN modified_time INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE snap.column_definitions ADD COLUMN deleted_at INTEGER",
        "ALTER TABLE snap.column_definitions ADD COLUMN template_hash TEXT",
        "ALTER TABLE snap.column_definitions ADD COLUMN trashed_at INTEGER",
        "ALTER TABLE snap.column_definitions ADD COLUMN column_uuid TEXT",
        "ALTER TABLE snap.column_options ADD COLUMN modified_time INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE snap.column_options ADD COLUMN deleted_at INTEGER",
        "ALTER TABLE snap.item_metadata ADD COLUMN deleted_at INTEGER",
        // column_presets ALTER removed — table no longer in snap.
    ];
    for sql in legacy_compat {
        // Errors are expected when the column already exists. Don't
        // log — modern snaps would generate 8 noisy warnings every
        // restore.
        let _ = conn.execute_batch(sql);
    }

    // 0.9.97 — normalise job_path on snap.* BEFORE the merge SQL.
    // Cross-OS peers write paths in their local form (mac
    // `/Volumes/…`, win `C:\Volumes\…`); the merge's
    // `INSERT WHERE NOT EXISTS` natural-key check is a string
    // compare, so a foreign-form snap row never matches a local-
    // form main row → INSERT fires → duplicate. Doing this here on
    // the temp ATTACH'd snapshot DB means by the time the merge
    // runs both sides are already in canonical form and the
    // NOT EXISTS gate works correctly. (The earlier post-restore
    // attempt was wrong — it normalised AFTER the duplicate was
    // already inserted.)
    // Pre-merge normalisation across every path-bearing column of the
    // snap.* tables — same coverage as `Database::normalize_job_paths`
    // for the main DB, but inlined here because we operate on the
    // ATTACH'd `snap.` schema prefix. Tables: column_definitions,
    // item_metadata (job_path + item_path), subscriptions, bookmarks.
    {
        let settings = crate::settings::AppSettings::load();
        let mappings = &settings.path_mappings;
        let canonicalize = |p: &str| -> Option<String> {
            // Tier 2.3: classify the legacy-form snapshot path to the
            // tagged identity the local main DB now uses, so the merge
            // joins (snap.job_path = main.job_path) match.
            let c = crate::utils::to_identity_storage(p, mappings);
            if c != p { Some(c) } else { None }
        };

        // Generic helper: gather (key, new) updates by-id.
        fn gather_id_path(
            conn: &rusqlite::Connection,
            sql: &str,
            canon: &dyn Fn(&str) -> Option<String>,
        ) -> Vec<(i64, String)> {
            let mut out: Vec<(i64, String)> = Vec::new();
            if let Ok(mut stmt) = conn.prepare(sql) {
                if let Ok(rows) = stmt.query_map([], |row| {
                    Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
                }) {
                    for r in rows.flatten() {
                        if let Some(c) = canon(&r.1) {
                            out.push((r.0, c));
                        }
                    }
                }
            }
            out
        }

        let col_updates = gather_id_path(
            conn,
            "SELECT id, job_path FROM snap.column_definitions",
            &canonicalize,
        );
        for (id, c) in &col_updates {
            let _ = conn.execute(
                "UPDATE snap.column_definitions SET job_path = ?1 WHERE id = ?2",
                rusqlite::params![c, id],
            );
        }

        // item_metadata.job_path (key off item_path PK)
        let mut meta_job_updates: Vec<(String, String)> = Vec::new();
        if let Ok(mut stmt) = conn.prepare("SELECT item_path, job_path FROM snap.item_metadata") {
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
        for (ip, c) in &meta_job_updates {
            let _ = conn.execute(
                "UPDATE snap.item_metadata SET job_path = ?1 WHERE item_path = ?2",
                rusqlite::params![c, ip],
            );
        }

        // item_metadata.item_path itself (PK; on collision, drop the
        // foreign-form row — the local-form one is already correct)
        let mut meta_item_updates: Vec<(String, String)> = Vec::new();
        if let Ok(mut stmt) = conn.prepare("SELECT item_path FROM snap.item_metadata") {
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
            let exists: bool = conn
                .query_row(
                    "SELECT 1 FROM snap.item_metadata WHERE item_path = ?1",
                    rusqlite::params![new],
                    |_| Ok(true),
                )
                .unwrap_or(false);
            if exists {
                let _ = conn.execute(
                    "DELETE FROM snap.item_metadata WHERE item_path = ?1",
                    rusqlite::params![old],
                );
            } else if conn
                .execute(
                    "UPDATE snap.item_metadata SET item_path = ?1 WHERE item_path = ?2",
                    rusqlite::params![new, old],
                )
                .unwrap_or(0)
                > 0
            {
                meta_item_done += 1;
            }
        }

        // subscriptions.job_path (UNIQUE; on collision, drop our row —
        // main-DB merge will reconcile by mtime later)
        let sub_updates = gather_id_path(
            conn,
            "SELECT id, job_path FROM snap.subscriptions",
            &canonicalize,
        );
        let mut subs_done = 0usize;
        for (id, c) in &sub_updates {
            let exists: bool = conn
                .query_row(
                    "SELECT 1 FROM snap.subscriptions WHERE job_path = ?1 AND id != ?2",
                    rusqlite::params![c, id],
                    |_| Ok(true),
                )
                .unwrap_or(false);
            if exists {
                let _ = conn.execute(
                    "DELETE FROM snap.subscriptions WHERE id = ?1",
                    rusqlite::params![id],
                );
            } else if conn
                .execute(
                    "UPDATE snap.subscriptions SET job_path = ?1 WHERE id = ?2",
                    rusqlite::params![c, id],
                )
                .unwrap_or(0)
                > 0
            {
                subs_done += 1;
            }
        }

        // bookmarks.path (UNIQUE; same drop-on-collision pattern)
        let bookmark_updates = gather_id_path(
            conn,
            "SELECT id, path FROM snap.bookmarks",
            &canonicalize,
        );
        let mut bookmarks_done = 0usize;
        for (id, c) in &bookmark_updates {
            let exists: bool = conn
                .query_row(
                    "SELECT 1 FROM snap.bookmarks WHERE path = ?1 AND id != ?2",
                    rusqlite::params![c, id],
                    |_| Ok(true),
                )
                .unwrap_or(false);
            if exists {
                let _ = conn.execute(
                    "DELETE FROM snap.bookmarks WHERE id = ?1",
                    rusqlite::params![id],
                );
            } else if conn
                .execute(
                    "UPDATE snap.bookmarks SET path = ?1 WHERE id = ?2",
                    rusqlite::params![c, id],
                )
                .unwrap_or(0)
                > 0
            {
                bookmarks_done += 1;
            }
        }

        if !col_updates.is_empty()
            || !meta_job_updates.is_empty()
            || meta_item_done > 0
            || subs_done > 0
            || bookmarks_done > 0
        {
            log::info!(
                "Snapshot pre-merge: normalised paths — column_defs={} meta_job={} meta_item={} subs={} bookmarks={}",
                col_updates.len(), meta_job_updates.len(), meta_item_done, subs_done, bookmarks_done
            );
        }
    }

    // Granular merge — see plan 11 for the full rationale.
    let tables: &[(&str, &str)] = &[
        (
            "subscriptions",
            "INSERT INTO main.subscriptions (job_path, job_name, is_active, subscribed_time, last_sync_time, sync_status, shot_count, modified_time, deleted_at)
             SELECT s.job_path, s.job_name, s.is_active, s.subscribed_time, s.last_sync_time, s.sync_status, s.shot_count, s.modified_time, s.deleted_at
             FROM snap.subscriptions s
             WHERE NOT EXISTS (SELECT 1 FROM main.subscriptions m WHERE m.job_path = s.job_path);

             UPDATE main.subscriptions
             SET job_name = snap_row.job_name,
                 is_active = snap_row.is_active,
                 last_sync_time = snap_row.last_sync_time,
                 sync_status = snap_row.sync_status,
                 shot_count = snap_row.shot_count,
                 modified_time = snap_row.modified_time,
                 deleted_at = snap_row.deleted_at
             FROM (SELECT * FROM snap.subscriptions) AS snap_row
             WHERE main.subscriptions.job_path = snap_row.job_path
               AND snap_row.modified_time IS NOT NULL
               AND (main.subscriptions.modified_time IS NULL
                    OR snap_row.modified_time > main.subscriptions.modified_time);",
        ),
        (
            "column_definitions",
            // 0.9.97: snap copy + restore now carry template_hash and
            // trashed_at. The merge UPDATE has TWO predicates: an
            // ordinary LWW gate (snap.modified_time > main, applies
            // all field changes), AND a terminal-state override that
            // applies trashed_at / deleted_at transitions when the
            // snap row holds a non-NULL flag the local row doesn't —
            // even if the local row's modified_time is newer. This
            // closes the hole where a peer that locally edited a
            // column after another peer trashed/deleted it would
            // silently drop the trash/delete on snapshot replay.
            // v5: match on the stable column_uuid, not the natural key, so
            // a column renamed on one peer converges (LWW SETs the new
            // column_name) instead of being inserted as a duplicate. v5
            // snapshots always carry column_uuid (Increment B backfills +
            // stores it), so pure uuid matching is correct here.
            "INSERT INTO main.column_definitions
                 (job_path, folder_name, column_name, column_type, column_order, column_width, is_visible, default_value, modified_time, deleted_at, template_hash, trashed_at, column_uuid)
             SELECT s.job_path, s.folder_name, s.column_name, s.column_type, s.column_order, s.column_width, s.is_visible, s.default_value, s.modified_time, s.deleted_at, s.template_hash, s.trashed_at, s.column_uuid
             FROM snap.column_definitions s
             WHERE NOT EXISTS (
                 SELECT 1 FROM main.column_definitions m
                 -- Dedup by stable uuid when the snap row has one.
                 -- Fall back to the natural key when it doesn't:
                 -- `m.column_uuid = s.column_uuid` is three-valued SQL,
                 -- so a NULL-uuid snap row fails the equality and would
                 -- be re-INSERTed on EVERY restore — the boot-time
                 -- per-restore leak that ballooned this table (451
                 -- dupes of one column). The natural-key branch
                 -- makes NULL-uuid rows dedupe against the existing
                 -- (job, folder, name) row instead.
                 WHERE (s.column_uuid IS NOT NULL AND m.column_uuid = s.column_uuid)
                    OR (s.column_uuid IS NULL
                        AND m.job_path = s.job_path
                        AND m.folder_name = s.folder_name
                        AND m.column_name = s.column_name)
             );

             UPDATE main.column_definitions
             SET column_name = snap_row.column_name,
                 column_type = snap_row.column_type,
                 column_order = snap_row.column_order,
                 column_width = snap_row.column_width,
                 is_visible = snap_row.is_visible,
                 default_value = snap_row.default_value,
                 modified_time = snap_row.modified_time,
                 deleted_at = snap_row.deleted_at,
                 template_hash = snap_row.template_hash,
                 trashed_at = snap_row.trashed_at
             FROM (SELECT * FROM snap.column_definitions) AS snap_row
             WHERE main.column_definitions.column_uuid = snap_row.column_uuid
               AND snap_row.modified_time IS NOT NULL
               AND (main.column_definitions.modified_time IS NULL
                    OR snap_row.modified_time > main.column_definitions.modified_time);

             -- Terminal-state override: if the snap row marks the
             -- column trashed and the local row isn't, apply just the
             -- trashed_at transition regardless of modified_time.
             -- Same pattern for permanent-delete (deleted_at).
             UPDATE main.column_definitions
             SET trashed_at = snap_row.trashed_at,
                 modified_time = MAX(snap_row.modified_time,
                                     main.column_definitions.modified_time)
             FROM (SELECT * FROM snap.column_definitions) AS snap_row
             WHERE main.column_definitions.column_uuid = snap_row.column_uuid
               AND snap_row.trashed_at IS NOT NULL
               AND main.column_definitions.trashed_at IS NULL;

             UPDATE main.column_definitions
             SET deleted_at = snap_row.deleted_at,
                 modified_time = MAX(snap_row.modified_time,
                                     main.column_definitions.modified_time)
             FROM (SELECT * FROM snap.column_definitions) AS snap_row
             WHERE main.column_definitions.column_uuid = snap_row.column_uuid
               AND snap_row.deleted_at IS NOT NULL
               AND main.column_definitions.deleted_at IS NULL;",
        ),
        (
            "column_options",
            "INSERT INTO main.column_options (column_id, option_name, option_color, modified_time, deleted_at)
             SELECT m.id, o.option_name, o.option_color, o.modified_time, o.deleted_at
             FROM snap.column_options o
             JOIN snap.column_definitions sc ON sc.id = o.column_id
             JOIN main.column_definitions m
                 ON m.column_uuid = sc.column_uuid
             WHERE NOT EXISTS (
                 SELECT 1 FROM main.column_options mo
                 WHERE mo.column_id = m.id AND mo.option_name = o.option_name
             );

             UPDATE main.column_options
             SET option_color = snap_opt.option_color,
                 modified_time = snap_opt.modified_time,
                 deleted_at = snap_opt.deleted_at
             FROM (
                 SELECT m.id AS main_col_id, o.option_name, o.option_color, o.modified_time, o.deleted_at
                 FROM snap.column_options o
                 JOIN snap.column_definitions sc ON sc.id = o.column_id
                 JOIN main.column_definitions m
                     ON m.column_uuid = sc.column_uuid
             ) AS snap_opt
             WHERE main.column_options.column_id = snap_opt.main_col_id
               AND main.column_options.option_name = snap_opt.option_name
               AND snap_opt.modified_time IS NOT NULL
               AND (main.column_options.modified_time IS NULL
                    OR snap_opt.modified_time > main.column_options.modified_time);",
        ),
        (
            "item_metadata",
            "INSERT INTO main.item_metadata (item_path, job_path, folder_name, metadata_json, is_tracked, created_time, modified_time, device_id, deleted_at)
             SELECT s.item_path, s.job_path, s.folder_name, s.metadata_json, s.is_tracked, s.created_time, s.modified_time, s.device_id, s.deleted_at
             FROM snap.item_metadata s
             WHERE NOT EXISTS (SELECT 1 FROM main.item_metadata m WHERE m.item_path = s.item_path);

             UPDATE main.item_metadata
             SET job_path = snap_row.job_path,
                 folder_name = snap_row.folder_name,
                 metadata_json = snap_row.metadata_json,
                 is_tracked = snap_row.is_tracked,
                 modified_time = snap_row.modified_time,
                 device_id = snap_row.device_id,
                 deleted_at = snap_row.deleted_at
             FROM (SELECT * FROM snap.item_metadata) AS snap_row
             WHERE main.item_metadata.item_path = snap_row.item_path
               AND snap_row.modified_time IS NOT NULL
               AND (main.item_metadata.modified_time IS NULL
                    OR snap_row.modified_time > main.item_metadata.modified_time);",
        ),
        // column_presets restore block removed — presets ride the
        // filesystem (b0a6c5f). The local column_presets table
        // still exists for legacy data but isn't synced via the
        // mesh snapshot path anymore.
    ];

    const COLUMN_TABLES: [&str; 2] = ["column_definitions", "column_options"];
    for (name, sql) in tables {
        if !include_column_tables && COLUMN_TABLES.contains(name) {
            continue;
        }
        let snap_count: i64 = conn
            .query_row(
                &format!("SELECT COUNT(*) FROM snap.{}", name),
                [],
                |row| row.get(0),
            )
            .unwrap_or(-1);
        let main_before: i64 = conn
            .query_row(
                &format!("SELECT COUNT(*) FROM main.{}", name),
                [],
                |row| row.get(0),
            )
            .unwrap_or(-1);
        if snap_count == 0 {
            log::info!(
                "Snapshot restore: snap.{} empty (main={}), skipping",
                name,
                main_before
            );
            continue;
        }
        match conn.execute_batch(sql) {
            Ok(()) => {
                let main_after: i64 = conn
                    .query_row(
                        &format!("SELECT COUNT(*) FROM main.{}", name),
                        [],
                        |row| row.get(0),
                    )
                    .unwrap_or(-1);
                log::info!(
                    "Snapshot restore: {} snap={} main={} → {} (added {})",
                    name,
                    snap_count,
                    main_before,
                    main_after,
                    main_after - main_before
                );
            }
            Err(e) => log::warn!("Snapshot restore: skipping {} ({})", name, e),
        }
    }

    let _ = conn.execute_batch("DETACH DATABASE snap;");
    let _ = std::fs::remove_file(&local_tmp);

    log::info!("Restored from snapshot {}", snapshot_path.display());
    Ok(())
}
