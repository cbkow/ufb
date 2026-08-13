use crate::db::Database;
use serde::{Deserialize, Serialize};
use std::sync::Arc;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum SyncStatus {
    Pending,
    Syncing,
    Synced,
    Stale,
    Error,
}

impl std::fmt::Display for SyncStatus {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SyncStatus::Pending => write!(f, "Pending"),
            SyncStatus::Syncing => write!(f, "Syncing"),
            SyncStatus::Synced => write!(f, "Synced"),
            SyncStatus::Stale => write!(f, "Stale"),
            SyncStatus::Error => write!(f, "Error"),
        }
    }
}

impl From<String> for SyncStatus {
    fn from(s: String) -> Self {
        match s.as_str() {
            "Syncing" => SyncStatus::Syncing,
            "Synced" => SyncStatus::Synced,
            "Stale" => SyncStatus::Stale,
            "Error" => SyncStatus::Error,
            _ => SyncStatus::Pending,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Subscription {
    pub id: i64,
    pub job_path: String,
    pub job_name: String,
    pub is_active: bool,
    pub subscribed_time: i64,
    pub last_sync_time: Option<i64>,
    pub sync_status: SyncStatus,
    pub shot_count: i64,
    #[serde(default)]
    pub modified_time: i64,
    #[serde(default)]
    pub deleted_at: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TrackedItemRecord {
    pub item_path: String,
    pub job_path: String,
    pub job_name: String,
    pub folder_name: String,
    pub metadata_json: String,
    pub modified_time: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ItemMetadataRecord {
    pub item_path: String,
    pub folder_name: String,
    pub metadata_json: String,
    pub is_tracked: bool,
}

/// Canonicalise a path argument to tagged-identity storage form
/// (`vol:…` / `native:…`) for DB reads + writes. Idempotent — a string
/// already in tagged form is returned unchanged.
fn to_storage(p: &str) -> String {
    let mappings = crate::settings::AppSettings::load().path_mappings;
    crate::utils::to_identity_storage(p, &mappings)
}

pub struct SubscriptionManager {
    db: Arc<Database>,
}

impl SubscriptionManager {
    pub fn new(db: Arc<Database>) -> Self {
        Self { db }
    }

    pub fn subscribe_to_job(
        &self,
        job_path: &str,
        job_name: &str,
    ) -> Result<Subscription, String> {
        let job_path = &to_storage(job_path);
        let now = chrono::Utc::now().timestamp_millis();
        self.db
            .with_conn(|conn| {
                conn.execute(
                    "INSERT INTO subscriptions (job_path, job_name, subscribed_time, modified_time, deleted_at)
                     VALUES (?1, ?2, ?3, ?3, NULL)
                     ON CONFLICT(job_path) DO UPDATE SET
                         job_name = excluded.job_name,
                         modified_time = excluded.modified_time,
                         deleted_at = NULL",
                    rusqlite::params![job_path, job_name, now],
                )?;
                // Recovery path: builds ≤1.1.0 tombstoned the job's whole
                // item_metadata on unsubscribe with no undo. Nothing else
                // writes item tombstones, so clearing them here only ever
                // resurrects that damage. Stamp modified_time so the
                // un-delete wins the LWW merge against the old tombstones.
                conn.execute(
                    "UPDATE item_metadata SET deleted_at = NULL, modified_time = ?1
                     WHERE job_path = ?2 AND deleted_at IS NOT NULL",
                    rusqlite::params![now, job_path],
                )?;
                let sub = conn.query_row(
                    "SELECT id, job_path, job_name, is_active, subscribed_time,
                            last_sync_time, sync_status, shot_count, modified_time
                     FROM subscriptions WHERE job_path = ?1",
                    [job_path],
                    |row| {
                        Ok(Subscription {
                            id: row.get(0)?,
                            job_path: row.get(1)?,
                            job_name: row.get(2)?,
                            is_active: row.get::<_, i64>(3)? != 0,
                            subscribed_time: row.get(4)?,
                            last_sync_time: row.get(5)?,
                            sync_status: SyncStatus::from(row.get::<_, String>(6)?),
                            shot_count: row.get(7)?,
                            modified_time: row.get(8)?,
                            deleted_at: None,
                        })
                    },
                )?;
                Ok(sub)
            })
            .map_err(|e| e.to_string())
    }

    pub fn unsubscribe_from_job(&self, job_path: &str) -> Result<(), String> {
        let job_path = &to_storage(job_path);
        let now = chrono::Utc::now().timestamp_millis();
        self.db
            .with_conn(|conn| {
                // Sidebar-only: item_metadata is deliberately untouched.
                // Unsubscribe used to blanket-tombstone the job's metadata
                // mesh-wide (with no undo on resubscribe) — one node removing
                // a job it couldn't even access erased everyone's metadata.
                conn.execute(
                    "UPDATE subscriptions SET modified_time = ?1, deleted_at = ?1 WHERE job_path = ?2",
                    rusqlite::params![now, job_path],
                )?;
                Ok(())
            })
            .map_err(|e| e.to_string())
    }

    pub fn get_all_subscriptions(&self) -> Result<Vec<Subscription>, String> {
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare(
                    "SELECT id, job_path, job_name, is_active, subscribed_time,
                            last_sync_time, sync_status, shot_count, modified_time
                     FROM subscriptions
                     WHERE deleted_at IS NULL
                     ORDER BY job_name",
                )?;
                let subs = stmt
                    .query_map([], |row| {
                        Ok(Subscription {
                            id: row.get(0)?,
                            job_path: row.get(1)?,
                            job_name: row.get(2)?,
                            is_active: row.get::<_, i64>(3)? != 0,
                            subscribed_time: row.get(4)?,
                            last_sync_time: row.get(5)?,
                            sync_status: SyncStatus::from(row.get::<_, String>(6)?),
                            shot_count: row.get(7)?,
                            modified_time: row.get(8)?,
                            deleted_at: None,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(subs)
            })
            .map_err(|e| e.to_string())
    }

    pub fn update_sync_status(
        &self,
        job_path: &str,
        status: SyncStatus,
    ) -> Result<(), String> {
        let job_path = &to_storage(job_path);
        let now = chrono::Utc::now().timestamp_millis();
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "UPDATE subscriptions SET sync_status = ?1, last_sync_time = ?2, modified_time = ?2
                     WHERE job_path = ?3",
                )?;
                stmt.execute(rusqlite::params![status.to_string(), now, job_path])?;
                Ok(())
            })
            .map_err(|e| e.to_string())
    }

    pub fn update_shot_count(&self, job_path: &str, count: i64) -> Result<(), String> {
        let job_path = &to_storage(job_path);
        let now = chrono::Utc::now().timestamp_millis();
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "UPDATE subscriptions SET shot_count = ?1, modified_time = ?2 WHERE job_path = ?3",
                )?;
                stmt.execute(rusqlite::params![count, now, job_path])?;
                Ok(())
            })
            .map_err(|e| e.to_string())
    }

    // --- Item Metadata ---

    /// Upsert a local metadata edit. Returns the `modified_time` stamped
    /// on the row so the caller can broadcast it on the mesh COUPLED to
    /// the json it describes — peers gate on this timestamp (LWW), and a
    /// replayed/older edit must not clobber a newer value. Reading the
    /// mtime from the DB after the fact would race a concurrent edit
    /// (newer ts glued to older json), so it's returned from the write.
    pub fn upsert_item_metadata(
        &self,
        job_path: &str,
        item_path: &str,
        folder_name: &str,
        metadata_json: &str,
        is_tracked: bool,
    ) -> Result<i64, String> {
        let job_path = &to_storage(job_path);
        let item_path = &to_storage(item_path);
        let now = chrono::Utc::now().timestamp_millis();
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "INSERT INTO item_metadata (item_path, job_path, folder_name, metadata_json, is_tracked, modified_time, deleted_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, NULL)
                     ON CONFLICT(item_path) DO UPDATE SET
                         metadata_json = excluded.metadata_json,
                         is_tracked = excluded.is_tracked,
                         modified_time = excluded.modified_time,
                         deleted_at = NULL",
                )?;
                stmt.execute(rusqlite::params![
                    item_path, job_path, folder_name, metadata_json, is_tracked as i64, now
                ])?;
                Ok(now)
            })
            .map_err(|e| e.to_string())
    }

    pub fn get_item_metadata(&self, item_path: &str) -> Result<Option<String>, String> {
        let item_path = &to_storage(item_path);
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "SELECT metadata_json FROM item_metadata WHERE item_path = ?1 AND deleted_at IS NULL",
                )?;
                let result = stmt.query_row([item_path], |row| row.get::<_, String>(0));
                match result {
                    Ok(json) => Ok(Some(json)),
                    Err(rusqlite::Error::QueryReturnedNoRows) => Ok(None),
                    Err(e) => Err(e),
                }
            })
            .map_err(|e| e.to_string())
    }

    pub fn get_tracked_items(&self, job_path: &str) -> Result<Vec<TrackedItemRecord>, String> {
        let job_path = &to_storage(job_path);
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "SELECT im.item_path, im.job_path, s.job_name, im.folder_name, im.metadata_json, im.modified_time
                     FROM item_metadata im
                     JOIN subscriptions s ON im.job_path = s.job_path
                     WHERE im.job_path = ?1 AND im.is_tracked = 1
                       AND im.deleted_at IS NULL AND s.deleted_at IS NULL",
                )?;
                let items = stmt
                    .query_map([job_path], |row| {
                        Ok(TrackedItemRecord {
                            item_path: row.get(0)?,
                            job_path: row.get(1)?,
                            job_name: row.get(2)?,
                            folder_name: row.get(3)?,
                            metadata_json: row.get(4)?,
                            modified_time: row.get(5)?,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(items)
            })
            .map_err(|e| e.to_string())
    }

    pub fn get_all_tracked_items(&self) -> Result<Vec<TrackedItemRecord>, String> {
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "SELECT im.item_path, im.job_path, s.job_name, im.folder_name, im.metadata_json, im.modified_time
                     FROM item_metadata im
                     JOIN subscriptions s ON im.job_path = s.job_path
                     WHERE im.is_tracked = 1
                       AND im.deleted_at IS NULL AND s.deleted_at IS NULL",
                )?;
                let items = stmt
                    .query_map([], |row| {
                        Ok(TrackedItemRecord {
                            item_path: row.get(0)?,
                            job_path: row.get(1)?,
                            job_name: row.get(2)?,
                            folder_name: row.get(3)?,
                            metadata_json: row.get(4)?,
                            modified_time: row.get(5)?,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(items)
            })
            .map_err(|e| e.to_string())
    }

    pub fn delete_item_metadata(&self, item_path: &str) -> Result<(), String> {
        let item_path = &to_storage(item_path);
        let now = chrono::Utc::now().timestamp_millis();
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "UPDATE item_metadata SET modified_time = ?1, deleted_at = ?1 WHERE item_path = ?2",
                )?;
                stmt.execute(rusqlite::params![now, item_path])?;
                Ok(())
            })
            .map_err(|e| e.to_string())
    }

    pub fn get_all_item_metadata_for_job(
        &self,
        job_path: &str,
    ) -> Result<Vec<ItemMetadataRecord>, String> {
        let job_path = &to_storage(job_path);
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "SELECT item_path, folder_name, metadata_json, is_tracked
                     FROM item_metadata WHERE job_path = ?1 AND deleted_at IS NULL",
                )?;
                let items = stmt
                    .query_map([job_path], |row| {
                        Ok(ItemMetadataRecord {
                            item_path: row.get(0)?,
                            folder_name: row.get(1)?,
                            metadata_json: row.get(2)?,
                            is_tracked: row.get::<_, i64>(3)? != 0,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(items)
            })
            .map_err(|e| e.to_string())
    }

    pub fn get_folder_item_metadata(
        &self,
        job_path: &str,
        folder_name: &str,
    ) -> Result<Vec<ItemMetadataRecord>, String> {
        let job_path = &to_storage(job_path);
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare_cached(
                    "SELECT item_path, folder_name, metadata_json, is_tracked
                     FROM item_metadata WHERE job_path = ?1 AND folder_name = ?2 AND deleted_at IS NULL",
                )?;
                let items = stmt
                    .query_map(rusqlite::params![job_path, folder_name], |row| {
                        Ok(ItemMetadataRecord {
                            item_path: row.get(0)?,
                            folder_name: row.get(1)?,
                            metadata_json: row.get(2)?,
                            is_tracked: row.get::<_, i64>(3)? != 0,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(items)
            })
            .map_err(|e| e.to_string())
    }
}
