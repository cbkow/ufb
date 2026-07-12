use crate::db::Database;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Arc;

/// Metadata JSON structure stored per item.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ItemMetadata {
    #[serde(default)]
    pub status: String,
    #[serde(default)]
    pub category: String,
    #[serde(default)]
    pub priority: i32,
    #[serde(default)]
    pub due_date: String,
    #[serde(default)]
    pub artist: String,
    #[serde(default)]
    pub note: String,
    #[serde(default)]
    pub links: Vec<String>,
    #[serde(default)]
    pub is_tracked: bool,
    /// Additional dynamic column values keyed by column name
    #[serde(flatten)]
    pub extra: HashMap<String, serde_json::Value>,
}

/// Canonicalise a path argument to tagged-identity storage form
/// (`vol:…` / `native:…`). Idempotent.
fn to_storage(p: &str) -> String {
    let mappings = crate::settings::AppSettings::load().path_mappings;
    crate::utils::to_identity_storage(p, &mappings)
}

pub struct MetadataManager {
    db: Arc<Database>,
}

impl MetadataManager {
    pub fn new(db: Arc<Database>) -> Self {
        Self { db }
    }

    /// Write metadata directly to the database.
    pub fn write_immediate(
        &self,
        job_path: &str,
        item_path: &str,
        folder_name: &str,
        metadata_json: &str,
        is_tracked: bool,
    ) -> Result<(), String> {
        let job_path = &to_storage(job_path);
        let item_path = &to_storage(item_path);
        let now = chrono::Utc::now().timestamp_millis();

        self.db
            .with_conn(|conn| {
                // Clear any existing tombstone on UPSERT — re-writing metadata
                // after a soft-delete is an explicit re-activation.
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
                Ok(())
            })
            .map_err(|e| e.to_string())
    }

    /// Get metadata for a single item. Reads SQLite directly — no cache.
    ///
    /// A previous version kept a lazy in-memory HashMap that was populated
    /// on first read and only updated by `write_immediate`. HTTP broadcast
    /// receives, snapshot restore, and `apply_table_change` all wrote the
    /// DB directly, never touching the cache, so reads returned stale
    /// values forever. The cache has been removed; SQLite point reads via
    /// the pooled connection and `prepare_cached` are fast enough.
    pub fn get_metadata(&self, item_path: &str) -> Result<Option<String>, String> {
        let item_path = &to_storage(item_path);
        self.db
            .with_conn(|conn| {
                let mut stmt = conn
                    .prepare_cached("SELECT metadata_json FROM item_metadata WHERE item_path = ?1 AND deleted_at IS NULL")?;
                match stmt.query_row([item_path], |row| row.get::<_, String>(0)) {
                    Ok(json) => Ok(Some(json)),
                    Err(rusqlite::Error::QueryReturnedNoRows) => Ok(None),
                    Err(e) => Err(e),
                }
            })
            .map_err(|e| e.to_string())
    }
}
