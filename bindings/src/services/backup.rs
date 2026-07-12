//! `Backup` QObject — wraps `core::backup::BackupManager` and the
//! shared `ColumnConfigManager` + `SubscriptionManager` to assemble
//! / restore per-job snapshots.
//!
//! Snapshot format (v1):
//! ```json
//! {
//!   "version": 1,
//!   "createdAt": <epoch ms>,
//!   "jobPath": "...",
//!   "jobName": "...",
//!   "columns": [ ColumnDefinition, ... ],
//!   "items":   [ {itemPath, folderName, metadataJson, isTracked}, ... ]
//! }
//! ```
//!
//! Restore atomically re-applies columns then items. Existing rows
//! are upserted (manager methods already merge), and rows present
//! locally but missing from the snapshot are left alone — backup is
//! a "merge in" operation, not a "replace all" wipe.

use crate::db::shared_db;
use std::sync::{Arc, OnceLock};
use ufb_core::backup::BackupManager;
use ufb_core::columns::{ColumnConfigManager, ColumnDefinition};
use ufb_core::subscription::SubscriptionManager;

#[cxx_qt::bridge]
pub mod qobject {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qml_singleton]
        type Backup = super::BackupRust;

        /// JSON array of {timestamp, filename, createdBy, shotCount,
        /// checksum, uncompressedSize, date} for the job's local
        /// backup history. Empty array on first call (no `.ufb/backups`
        /// directory yet).
        #[qinvokable]
        fn list_backups(self: &Backup, job_path: QString) -> QString;

        /// Snapshot the job's columns + tracked items + any other
        /// item_metadata, write it to .ufb/backups inside the job
        /// folder, return the JSON of the new BackupInfo (or "" on
        /// error).
        #[qinvokable]
        fn create_backup(
            self: &Backup,
            job_path: QString,
            job_name: QString,
        ) -> QString;

        /// Read a backup snapshot from disk and re-apply it: upsert
        /// every column + item from the snapshot. Existing rows not
        /// in the snapshot are left alone. Returns "" on success or
        /// an error message.
        #[qinvokable]
        fn restore_backup(
            self: &Backup,
            job_path: QString,
            filename: QString,
        ) -> QString;
    }
}

#[derive(Default)]
pub struct BackupRust;

fn shared_backup_mgr() -> Arc<BackupManager> {
    static MGR: OnceLock<Arc<BackupManager>> = OnceLock::new();
    MGR.get_or_init(|| Arc::new(BackupManager::new(ufb_core::utils::get_device_id())))
        .clone()
}

fn shared_columns_mgr() -> Option<Arc<ColumnConfigManager>> {
    static MGR: OnceLock<Option<Arc<ColumnConfigManager>>> = OnceLock::new();
    MGR.get_or_init(|| shared_db().map(|db| Arc::new(ColumnConfigManager::new(db))))
        .clone()
}

fn shared_subscription_mgr() -> Option<Arc<SubscriptionManager>> {
    static MGR: OnceLock<Option<Arc<SubscriptionManager>>> = OnceLock::new();
    MGR.get_or_init(|| shared_db().map(|db| Arc::new(SubscriptionManager::new(db))))
        .clone()
}

impl qobject::Backup {
    fn list_backups(
        self: &qobject::Backup,
        job_path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let mgr = shared_backup_mgr();
        match mgr.list_backups(&job_path.to_string()) {
            Ok(list) => {
                let json = serde_json::to_string(&list).unwrap_or_else(|_| "[]".into());
                cxx_qt_lib::QString::from(&json)
            }
            Err(e) => {
                log::warn!("backup: list_backups failed: {}", e);
                cxx_qt_lib::QString::from("[]")
            }
        }
    }

    fn create_backup(
        self: &qobject::Backup,
        job_path: cxx_qt_lib::QString,
        job_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let job_s = job_path.to_string();
        let job_n = job_name.to_string();

        // Pull the column defs scoped to this job. ColumnConfigManager
        // doesn't expose an "all columns for job" call directly, so
        // we walk the DB by listing every distinct (job_path, folder)
        // pair represented in column_definitions and aggregate.
        let columns = collect_columns_for_job(&job_s);
        let items = collect_items_for_job(&job_s);

        let snapshot = serde_json::json!({
            "version": 1,
            "createdAt": ufb_core::utils::current_time_ms(),
            "jobPath": job_s,
            "jobName": job_n,
            "columns": columns,
            "items": items,
        });
        let json = match serde_json::to_string(&snapshot) {
            Ok(s) => s,
            Err(e) => {
                log::warn!("backup: serialize snapshot failed: {}", e);
                return cxx_qt_lib::QString::from("");
            }
        };

        let mgr = shared_backup_mgr();
        match mgr.create_backup(&job_s, &json) {
            Ok(info) => {
                let info_json = serde_json::to_string(&info).unwrap_or_else(|_| "{}".into());
                log::info!("backup: created {} ({} cols, {} items)",
                    info.filename, columns.len(), items.len());
                cxx_qt_lib::QString::from(&info_json)
            }
            Err(e) => {
                log::warn!("backup: create_backup failed: {}", e);
                cxx_qt_lib::QString::from("")
            }
        }
    }

    fn restore_backup(
        self: &qobject::Backup,
        job_path: cxx_qt_lib::QString,
        filename: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let job_s = job_path.to_string();
        let file_s = filename.to_string();

        let backup_mgr = shared_backup_mgr();
        let json = match backup_mgr.restore_backup(&job_s, &file_s) {
            Ok(s) => s,
            Err(e) => {
                log::warn!("backup: restore read failed: {}", e);
                return cxx_qt_lib::QString::from(&format!("Read failed: {}", e));
            }
        };
        let snapshot: serde_json::Value = match serde_json::from_str(&json) {
            Ok(v) => v,
            Err(e) => {
                return cxx_qt_lib::QString::from(&format!("Parse failed: {}", e));
            }
        };

        // Apply columns first so any items that reference a column
        // see it land in the same restore pass.
        let cols_mgr = match shared_columns_mgr() {
            Some(m) => m,
            None => return cxx_qt_lib::QString::from("DB unavailable"),
        };
        let subs_mgr = match shared_subscription_mgr() {
            Some(m) => m,
            None => return cxx_qt_lib::QString::from("DB unavailable"),
        };

        let mut col_count = 0usize;
        if let Some(cols) = snapshot.get("columns").and_then(|v| v.as_array()) {
            for c in cols {
                let def: ColumnDefinition = match serde_json::from_value(c.clone()) {
                    Ok(d) => d,
                    Err(e) => {
                        log::warn!("backup: skipping bad column: {}", e);
                        continue;
                    }
                };
                // add_column upserts by (job, folder, name) — safe
                // for both new and existing columns.
                if cols_mgr.add_column(&def).is_ok() {
                    col_count += 1;
                }
            }
        }

        let mut item_count = 0usize;
        if let Some(items) = snapshot.get("items").and_then(|v| v.as_array()) {
            for it in items {
                let Some(obj) = it.as_object() else { continue };
                let Some(item_p) = obj.get("itemPath").and_then(|v| v.as_str()) else { continue };
                let Some(folder_n) = obj.get("folderName").and_then(|v| v.as_str()) else { continue };
                let metadata = obj
                    .get("metadataJson")
                    .and_then(|v| v.as_str())
                    .unwrap_or("{}");
                let is_tracked = obj
                    .get("isTracked")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                if subs_mgr
                    .upsert_item_metadata(&job_s, item_p, folder_n, metadata, is_tracked)
                    .is_ok()
                {
                    item_count += 1;
                }
            }
        }

        log::info!(
            "backup: restored {} ({} columns, {} items)",
            file_s, col_count, item_count
        );
        cxx_qt_lib::QString::from("")
    }
}

/// Walk every unique (job_path, folder) pair represented in
/// item_metadata for this job, plus any folder with column defs, and
/// aggregate the column definitions from each. The ColumnConfigManager
/// API is folder-scoped so this is the simplest way to collect "all
/// columns for the job".
fn collect_columns_for_job(job_path: &str) -> Vec<ColumnDefinition> {
    let Some(mgr) = shared_columns_mgr() else { return vec![]; };
    let Some(db) = shared_db() else { return vec![]; };
    let folders: Vec<String> = db
        .with_conn(|conn| {
            let mut stmt = conn.prepare(
                "SELECT DISTINCT folder_name FROM column_definitions
                 WHERE job_path = ?1 AND deleted_at IS NULL
                 UNION
                 SELECT DISTINCT folder_name FROM item_metadata
                 WHERE job_path = ?1 AND deleted_at IS NULL",
            )?;
            let rows = stmt
                .query_map([job_path], |row| row.get::<_, String>(0))?
                .collect::<Result<Vec<_>, _>>()?;
            Ok(rows)
        })
        .unwrap_or_default();
    let mut out = Vec::new();
    for folder in folders {
        if let Ok(defs) = mgr.get_column_defs(job_path, &folder) {
            out.extend(defs);
        }
    }
    out
}

/// Pull every item_metadata row scoped to `job_path` (tracked or not).
fn collect_items_for_job(job_path: &str) -> Vec<serde_json::Value> {
    let Some(db) = shared_db() else { return vec![]; };
    db.with_conn(|conn| {
        let mut stmt = conn.prepare(
            "SELECT item_path, folder_name, metadata_json, is_tracked
             FROM item_metadata
             WHERE job_path = ?1 AND deleted_at IS NULL",
        )?;
        let rows = stmt
            .query_map([job_path], |row| {
                Ok(serde_json::json!({
                    "itemPath": row.get::<_, String>(0)?,
                    "folderName": row.get::<_, String>(1)?,
                    "metadataJson": row.get::<_, String>(2)?,
                    "isTracked": row.get::<_, i64>(3)? != 0,
                }))
            })?
            .collect::<Result<Vec<_>, _>>()?;
        Ok(rows)
    })
    .unwrap_or_default()
}
