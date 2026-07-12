//! Sync commands, queue entries, status, and the standalone
//! `apply_table_change` function.
//!
//! Extracted from the original `mesh_sync.rs` so that `http_server.rs`
//! and `coordinator.rs` can both depend on these types without one
//! pulling the other.

use crate::db::Database;
use serde::{Deserialize, Serialize};
use std::sync::Arc;

// ----------------------------------------------------------------------------
// Shared reqwest client builder.
//
// `no_proxy` is critical: `reqwest::Client::new()` auto-detects OS proxy
// config (Windows IE / registry, macOS scutil, env vars). On a Windows
// box that has ever been touched by a corp proxy / Zscaler / captive
// portal, that silently sends mesh POSTs to an upstream that can't route
// to a LAN-local IP — connect hangs.
// ----------------------------------------------------------------------------

pub fn build_mesh_http_client() -> reqwest::Client {
    reqwest::Client::builder()
        .no_proxy()
        .build()
        .unwrap_or_else(|e| {
            log::warn!(
                "Failed to build custom reqwest client ({}), falling back to default",
                e
            );
            reqwest::Client::new()
        })
}

// ----------------------------------------------------------------------------
// Sync commands — sent into the coordinator's mpsc.
// ----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum SyncCommand {
    EditMetadata {
        job_path: String,
        item_path: String,
        metadata_json: String,
        folder_name: String,
        is_tracked: bool,
        /// Edit timestamp (ms) coupled to `metadata_json`, for peer LWW.
        #[serde(default)]
        modified_time: i64,
    },
    BroadcastEdit {
        job_path: String,
        item_path: String,
        metadata_json: String,
        folder_name: String,
        is_tracked: bool,
        #[serde(default)]
        modified_time: i64,
    },
    TableChange {
        change_json: String,
    },
    BroadcastTableChange {
        change_json: String,
    },
    TakeSnapshot,
    RestoreSnapshot,
    FlushEditQueue,
    Shutdown,
}

// ----------------------------------------------------------------------------
// Persisted queue entries.
// ----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EditQueueEntry {
    pub job_path: String,
    pub item_path: String,
    pub metadata_json: String,
    pub folder_name: String,
    pub is_tracked: bool,
    /// Edit timestamp (ms) coupled to `metadata_json`, replayed on the
    /// wire so a delayed retry still carries the original LWW timestamp.
    #[serde(default)]
    pub modified_time: i64,
    pub queued_time: i64,
    pub retry_count: i32,
}

/// Queue entry for follower→leader table-change POSTs that failed to
/// deliver (leader unreachable, timeout, non-200). Persisted to disk so
/// a column add/delete/preset op survives app restart and VPN outages.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TableChangeQueueEntry {
    pub change_json: String,
    pub queued_time: i64,
    pub retry_count: i32,
}

// ----------------------------------------------------------------------------
// Status reported up to the UI.
// ----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MeshSyncStatus {
    pub is_leader: bool,
    pub leader_id: String,
    pub peer_count: usize,
    pub last_snapshot_time: Option<i64>,
    pub pending_edits_count: usize,
    pub status_message: String,
    pub is_enabled: bool,
    pub is_configured: bool,
}

// ----------------------------------------------------------------------------
// `apply_table_change` — DB-only side effect of an inbound table change.
//
// Pure rusqlite, no networking, no events. Called by:
//   * http_server's POST /api/table/update handler
//   * coordinator's TableChange command handler (local-origin write)
//
// Each action is gated by an LWW timestamp check against the local row's
// `modified_time`; older incoming changes are logged and discarded.
// Soft deletes carry `deleted_at` so the next snapshot propagates them.
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// 0.9.97 — receive-side template promotion.
//
// When a `col_add` / `col_update` arrives carrying a `template_hash`, we
// try to load the matching template JSON from `<app_data>/templates/`
// (cache-first) and fall back to `<farm_root>/ufb/templates/` on miss.
// On success the local row's `column_options` is REPLACED with the
// template's authoritative options (template wins over whatever the
// sender's broadcast carried). On failure (template file missing on
// share + not yet cached locally) the receiver clears `template_hash`
// to NULL so the row reads as unpromoted and the grid filter hides it
// — recoverable on the next snapshot replay once the share is
// reachable.
// ----------------------------------------------------------------------------
/// Softer cousin of `promote_or_unset_hash`. Tries to load the template
/// for `hash` and overwrites this column's `column_options` from its
/// authoritative option list. **Unlike `promote_or_unset_hash`, a fetch
/// failure here is a silent no-op** — we don't clear `template_hash`,
/// because this path runs on broadcasts the LWW gate marked stale (the
/// peer's options should already be reasonable) or on folder-open
/// resyncs where a transient share outage shouldn't unpromote rows the
/// user can already see.
fn reconcile_options_from_template(
    conn: &rusqlite::Connection,
    col_id: i64,
    hash: &str,
    ts: i64,
) -> rusqlite::Result<()> {
    let cache = crate::templates::v2::local_cache_dir();
    let settings = crate::settings::AppSettings::load();
    let remote = if settings.mesh_sync.farm_path.trim().is_empty() {
        None
    } else {
        crate::templates::v2::resolve_dir(&settings.mesh_sync.farm_path).ok()
    };
    let Ok(t) = crate::templates::v2::fetch_template(&cache, remote.as_deref(), hash) else {
        return Ok(());
    };
    conn.execute(
        "DELETE FROM column_options WHERE column_id = ?1",
        [col_id],
    )?;
    for opt in &t.options {
        let opt_ts = if opt.modified_time > 0 { opt.modified_time } else { ts };
        conn.execute(
            "INSERT INTO column_options (column_id, option_name, option_color, modified_time, deleted_at)
             VALUES (?1, ?2, ?3, ?4, NULL)",
            rusqlite::params![col_id, opt.name, opt.color, opt_ts],
        )?;
    }
    Ok(())
}

fn promote_or_unset_hash(
    conn: &rusqlite::Connection,
    col_id: i64,
    hash: Option<&str>,
    ts: i64,
) -> rusqlite::Result<()> {
    let Some(hash) = hash else { return Ok(()); };
    let cache = crate::templates::v2::local_cache_dir();
    let settings = crate::settings::AppSettings::load();
    let remote = if settings.mesh_sync.farm_path.trim().is_empty() {
        None
    } else {
        crate::templates::v2::resolve_dir(&settings.mesh_sync.farm_path).ok()
    };
    match crate::templates::v2::fetch_template(&cache, remote.as_deref(), hash) {
        Ok(t) => {
            // Replace local options with the template's authoritative set.
            conn.execute(
                "DELETE FROM column_options WHERE column_id = ?1",
                [col_id],
            )?;
            for opt in &t.options {
                let opt_ts = if opt.modified_time > 0 { opt.modified_time } else { ts };
                conn.execute(
                    "INSERT INTO column_options (column_id, option_name, option_color, modified_time, deleted_at)
                     VALUES (?1, ?2, ?3, ?4, NULL)",
                    rusqlite::params![col_id, opt.name, opt.color, opt_ts],
                )?;
            }
            log::info!(
                "Sync: promoted column {} via template {} ({} options)",
                col_id, hash, t.options.len()
            );
            Ok(())
        }
        Err(e) => {
            log::warn!(
                "Sync: template fetch failed for hash={} ({}); clearing template_hash on column {} so the grid hides it",
                hash, e, col_id
            );
            conn.execute(
                "UPDATE column_definitions SET template_hash = NULL WHERE id = ?1",
                [col_id],
            )?;
            Ok(())
        }
    }
}

/// Canonicalise a path received over the mesh to tagged-identity
/// storage form (`vol:…` / `native:…`). The wire carries legacy
/// Windows-form during rollout; this classifies it to the local DB's
/// identity form. Idempotent — an already-tagged string passes through.
fn to_identity(p: &str) -> String {
    let mappings = crate::settings::AppSettings::load().path_mappings;
    crate::utils::to_identity_storage(p, &mappings)
}

/// Find a column row's `(modified_time, id)` for an incoming col_* change.
/// v5 payloads carry a stable `column_uuid` — match on it first, which is
/// what lets a RENAME (or any edit) on one peer land on the right row of
/// another (the display name, the legacy natural key, has changed). Falls
/// back to the natural key for a payload without a uuid (defensive; all
/// same-epoch v5 peers send one).
fn find_col_row(
    conn: &rusqlite::Connection,
    job_path: &str,
    folder_name: &str,
    column_name: &str,
    column_uuid: Option<&str>,
) -> rusqlite::Result<Option<(i64, i64)>> {
    use rusqlite::OptionalExtension;
    if let Some(u) = column_uuid.filter(|s| !s.is_empty()) {
        if let Some(row) = conn
            .query_row(
                "SELECT modified_time, id FROM column_definitions WHERE column_uuid = ?1",
                rusqlite::params![u],
                |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?)),
            )
            .optional()?
        {
            return Ok(Some(row));
        }
    }
    conn.query_row(
        "SELECT modified_time, id FROM column_definitions
         WHERE job_path = ?1 AND folder_name = ?2 AND column_name = ?3",
        rusqlite::params![job_path, folder_name, column_name],
        |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?)),
    )
    .optional()
}

pub fn apply_table_change(db: &Arc<Database>, change_json: &str) -> Result<(), String> {
    use rusqlite::OptionalExtension;
    let change: serde_json::Value = serde_json::from_str(change_json)
        .map_err(|e| format!("Invalid table change JSON: {}", e))?;

    let action = change.get("action").and_then(|v| v.as_str()).unwrap_or("");

    // Extract timestamps from payload (default 0 = from pre-0.7.0 peer).
    let incoming_mod: i64 = change
        .get("modified_time")
        .and_then(|v| v.as_i64())
        .unwrap_or(0);
    let incoming_deleted: Option<i64> = change.get("deleted_at").and_then(|v| v.as_i64());

    match action {
        "sub_add" => {
            let job_path = change.get("job_path").and_then(|v| v.as_str()).unwrap_or("");
            let job_name = change.get("job_name").and_then(|v| v.as_str()).unwrap_or("");
            if job_path.is_empty() {
                return Err("sub_add: missing job_path".to_string());
            }
            let job_path = &to_identity(job_path);
            let ts = if incoming_mod > 0 { incoming_mod } else { chrono::Utc::now().timestamp_millis() };
            db.with_conn(|conn| {
                let local_mod: Option<i64> = conn
                    .query_row(
                        "SELECT modified_time FROM subscriptions WHERE job_path = ?1",
                        [job_path],
                        |row| row.get(0),
                    )
                    .optional()?;
                if let Some(local) = local_mod {
                    if ts <= local {
                        log::info!("Sync: sub_add stale (incoming={}, local={}), ignoring", ts, local);
                        return Ok(());
                    }
                }
                conn.execute(
                    "INSERT INTO subscriptions (job_path, job_name, subscribed_time, modified_time, deleted_at)
                     VALUES (?1, ?2, ?3, ?3, NULL)
                     ON CONFLICT(job_path) DO UPDATE SET
                         job_name = excluded.job_name,
                         modified_time = excluded.modified_time,
                         deleted_at = NULL",
                    rusqlite::params![job_path, job_name, ts],
                )?;
                Ok(())
            }).map_err(|e| e.to_string())
        }
        "sub_remove" => {
            let job_path = change.get("job_path").and_then(|v| v.as_str()).unwrap_or("");
            if job_path.is_empty() {
                return Err("sub_remove: missing job_path".to_string());
            }
            let job_path = &to_identity(job_path);
            let ts = if incoming_mod > 0 { incoming_mod } else { chrono::Utc::now().timestamp_millis() };
            let deleted_ts = incoming_deleted.unwrap_or(ts);
            db.with_conn(|conn| {
                let local_mod: Option<i64> = conn
                    .query_row(
                        "SELECT modified_time FROM subscriptions WHERE job_path = ?1",
                        [job_path],
                        |row| row.get(0),
                    )
                    .optional()?;
                if let Some(local) = local_mod {
                    if ts <= local {
                        log::info!("Sync: sub_remove stale (incoming={}, local={}), ignoring", ts, local);
                        return Ok(());
                    }
                }
                conn.execute(
                    "UPDATE subscriptions SET modified_time = ?1, deleted_at = ?2 WHERE job_path = ?3",
                    rusqlite::params![ts, deleted_ts, job_path],
                )?;
                conn.execute(
                    "UPDATE item_metadata SET modified_time = ?1, deleted_at = ?2 WHERE job_path = ?3",
                    rusqlite::params![ts, deleted_ts, job_path],
                )?;
                Ok(())
            }).map_err(|e| e.to_string())
        }
        "col_add" => {
            let def_str = change.get("def").and_then(|v| v.as_str()).unwrap_or("");
            let mut def: crate::columns::ColumnDefinition = serde_json::from_str(def_str)
                .map_err(|e| format!("col_add: invalid def: {}", e))?;
            // 0.9.97: normalise sender's job_path to local-canonical
            // form so cross-OS mesh writes don't leave macOS-form
            // strings in a Windows DB (silently invisible to
            // Windows-form queries).
            def.job_path = to_identity(&def.job_path);
            let ts = if def.modified_time > 0 {
                def.modified_time
            } else if incoming_mod > 0 {
                incoming_mod
            } else {
                chrono::Utc::now().timestamp_millis()
            };
            let col_uuid = def.column_uuid.clone();
            db.with_conn(|conn| {
                let local = find_col_row(
                    conn, &def.job_path, &def.folder_name, &def.column_name,
                    col_uuid.as_deref(),
                )?;
                if let Some((local_mod, local_col_id)) = local {
                    if ts <= local_mod {
                        log::info!(
                            "Sync: col_add stale for {} (incoming={}, local={}), ignoring",
                            def.column_name, ts, local_mod
                        );
                        return Ok(());
                    }
                    // Match + update by id (found via uuid), so a rename
                    // propagates: column_name is SET, not used as the key.
                    // COALESCE keeps a known-good local uuid if the payload
                    // somehow omitted one.
                    conn.execute(
                        "UPDATE column_definitions SET
                             column_name = ?1, column_type = ?2, column_order = ?3,
                             column_width = ?4, is_visible = ?5, default_value = ?6,
                             modified_time = ?7, deleted_at = NULL,
                             template_hash = ?8, trashed_at = ?9,
                             column_uuid = COALESCE(?10, column_uuid)
                         WHERE id = ?11",
                        rusqlite::params![
                            def.column_name, def.column_type, def.column_order,
                            def.column_width, def.is_visible as i64, def.default_value, ts,
                            def.template_hash, def.trashed_at, col_uuid, local_col_id,
                        ],
                    )?;
                    conn.execute("DELETE FROM column_options WHERE column_id = ?1", [local_col_id])?;
                    for opt in &def.options {
                        let opt_ts = if opt.modified_time > 0 { opt.modified_time } else { ts };
                        conn.execute(
                            "INSERT INTO column_options (column_id, option_name, option_color, modified_time, deleted_at) VALUES (?1, ?2, ?3, ?4, NULL)",
                            rusqlite::params![local_col_id, opt.name, opt.color, opt_ts],
                        )?;
                    }
                    // 0.9.97: promote-on-receive — try the template
                    // fetch and override broadcast options with the
                    // template's authoritative set (or unset the hash
                    // and hide the row if fetch fails).
                    promote_or_unset_hash(conn, local_col_id, def.template_hash.as_deref(), ts)?;
                    return Ok(());
                }

                conn.execute(
                    "INSERT INTO column_definitions
                     (job_path, folder_name, column_name, column_type, column_order, column_width, is_visible, default_value, modified_time, deleted_at, template_hash, trashed_at, column_uuid)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, NULL, ?10, ?11, ?12)",
                    rusqlite::params![
                        def.job_path, def.folder_name, def.column_name, def.column_type,
                        def.column_order, def.column_width, def.is_visible as i64, def.default_value, ts,
                        def.template_hash, def.trashed_at, col_uuid,
                    ],
                )?;
                let col_id = conn.last_insert_rowid();
                for opt in &def.options {
                    let opt_ts = if opt.modified_time > 0 { opt.modified_time } else { ts };
                    conn.execute(
                        "INSERT INTO column_options (column_id, option_name, option_color, modified_time, deleted_at) VALUES (?1, ?2, ?3, ?4, NULL)",
                        rusqlite::params![col_id, opt.name, opt.color, opt_ts],
                    )?;
                }
                promote_or_unset_hash(conn, col_id, def.template_hash.as_deref(), ts)?;
                Ok(())
            }).map_err(|e| e.to_string())
        }
        "col_update" => {
            let def_str = change.get("def").and_then(|v| v.as_str()).unwrap_or("");
            let mut def: crate::columns::ColumnDefinition = serde_json::from_str(def_str)
                .map_err(|e| format!("col_update: invalid def: {}", e))?;
            if def.job_path.is_empty() || def.folder_name.is_empty() || def.column_name.is_empty() {
                return Err("col_update: missing natural key (job_path/folder_name/column_name)".to_string());
            }
            def.job_path = to_identity(&def.job_path);
            let ts = if def.modified_time > 0 {
                def.modified_time
            } else if incoming_mod > 0 {
                incoming_mod
            } else {
                chrono::Utc::now().timestamp_millis()
            };
            let col_uuid = def.column_uuid.clone();
            db.with_conn(|conn| {
                let local_row = find_col_row(
                    conn, &def.job_path, &def.folder_name, &def.column_name,
                    col_uuid.as_deref(),
                )?;
                let Some((local, local_col_id_pre)) = local_row else {
                    log::warn!(
                        "col_update: no local row for {}/{}/{} — ignoring",
                        def.job_path, def.folder_name, def.column_name
                    );
                    return Ok(());
                };
                if ts <= local {
                    // Stale broadcast for the col_definitions row — but
                    // the template file on the share may have advanced
                    // (the sender's two-write editor pattern produces
                    // two broadcasts that can collide at ms-precision,
                    // so the second one — carrying the new option — gets
                    // rejected here). Treat the broadcast as a "go
                    // re-read the template" ping: options are
                    // authoritative on disk, so pull them from there.
                    if let Some(hash) = def.template_hash.as_deref() {
                        let _ = reconcile_options_from_template(
                            conn, local_col_id_pre, hash, ts,
                        );
                    }
                    log::info!(
                        "Sync: col_update stale for {} (incoming={}, local={}), reconciled options from template",
                        def.column_name, ts, local
                    );
                    return Ok(());
                }
                // 0.9.97: deleted_at cleared on update (live edit
                // resurrects from "permanently deleted" — caller side
                // shouldn't be sending col_update on a permadeleted
                // row, but if it does the row comes back). trashed_at
                // and template_hash are mirrored straight from the
                // payload so trash/untrash + promote/unpromote
                // propagate via ordinary col_update.
                // Update by id (matched via uuid) so a rename propagates:
                // column_name is SET, not the key. COALESCE preserves a
                // known-good local uuid if the payload omitted one.
                conn.execute(
                    "UPDATE column_definitions SET
                         column_name = ?1, column_type = ?2, column_order = ?3,
                         column_width = ?4, is_visible = ?5, default_value = ?6,
                         modified_time = ?7, deleted_at = NULL,
                         template_hash = ?8, trashed_at = ?9,
                         column_uuid = COALESCE(?10, column_uuid)
                     WHERE id = ?11",
                    rusqlite::params![
                        def.column_name, def.column_type, def.column_order,
                        def.column_width, def.is_visible as i64, def.default_value, ts,
                        def.template_hash, def.trashed_at, col_uuid, local_col_id_pre,
                    ],
                )?;
                let local_col_id: i64 = local_col_id_pre;
                conn.execute("DELETE FROM column_options WHERE column_id = ?1", [local_col_id])?;
                for opt in &def.options {
                    let opt_ts = if opt.modified_time > 0 { opt.modified_time } else { ts };
                    conn.execute(
                        "INSERT INTO column_options (column_id, option_name, option_color, modified_time, deleted_at) VALUES (?1, ?2, ?3, ?4, NULL)",
                        rusqlite::params![local_col_id, opt.name, opt.color, opt_ts],
                    )?;
                }
                promote_or_unset_hash(conn, local_col_id, def.template_hash.as_deref(), ts)?;
                Ok(())
            }).map_err(|e| e.to_string())
        }
        "col_delete" => {
            let job_path = change.get("job_path").and_then(|v| v.as_str()).unwrap_or("");
            let folder_name = change.get("folder_name").and_then(|v| v.as_str()).unwrap_or("");
            let column_name = change.get("column_name").and_then(|v| v.as_str()).unwrap_or("");
            // v5: col_delete carries the column's stable uuid so a delete
            // lands on the right row even if the receiving peer renamed it.
            let column_uuid = change.get("column_uuid").and_then(|v| v.as_str());
            if job_path.is_empty() || folder_name.is_empty() || column_name.is_empty() {
                return Err("col_delete: missing natural key (job_path/folder_name/column_name)".to_string());
            }
            let job_path = &to_identity(job_path);
            let ts = if incoming_mod > 0 { incoming_mod } else { chrono::Utc::now().timestamp_millis() };
            let deleted_ts = incoming_deleted.unwrap_or(ts);
            db.with_conn(|conn| {
                // 0.9.97: col_delete is now "permanently delete" (post-
                // empty-trash). Terminal state — wins over local edits
                // regardless of modified_time. Recoverability is gone
                // by design at this stage; the user emptied the trash
                // intentionally. We still gate on idempotency (skip if
                // already permanently deleted with a >= timestamp) so
                // re-broadcasts don't churn the row.
                let Some((_, target_id)) =
                    find_col_row(conn, job_path, folder_name, column_name, column_uuid)?
                else {
                    log::warn!(
                        "col_delete: no local row for {}/{}/{} — ignoring",
                        job_path, folder_name, column_name
                    );
                    return Ok(());
                };
                // Row presence was already established by find_col_row;
                // this reads its current deleted_at for the idempotency
                // gate (skip if already permanently deleted at-or-after
                // this timestamp).
                let local_deleted: Option<i64> = conn.query_row(
                    "SELECT deleted_at FROM column_definitions WHERE id = ?1",
                    rusqlite::params![target_id],
                    |row| row.get::<_, Option<i64>>(0),
                )?;
                if let Some(local_d) = local_deleted {
                    if local_d >= deleted_ts {
                        // already deleted at this time-or-later; idempotent skip
                        return Ok(());
                    }
                }
                conn.execute(
                    "UPDATE column_definitions SET modified_time = ?1, deleted_at = ?2
                     WHERE id = ?3",
                    rusqlite::params![ts, deleted_ts, target_id],
                )?;
                conn.execute(
                    "UPDATE column_options SET modified_time = ?1, deleted_at = ?2
                     WHERE column_id = ?3",
                    rusqlite::params![ts, deleted_ts, target_id],
                )?;
                Ok(())
            }).map_err(|e| e.to_string())
        }
        // `preset_save` / `preset_delete` actions removed in the
        // 2026-05 templates rework. Templates (column presets) now
        // live as JSON files under `<farm_root>/ufb/templates/` and
        // never touch the mesh broadcast / snapshot path. See
        // `core::templates`. Old peers running a pre-rework build
        // may still emit these — log + drop rather than reviving the
        // broken-in-practice column_presets sync logic.
        "preset_save" | "preset_delete" => {
            log::warn!(
                "Sync: ignoring legacy `{}` table change — templates moved to filesystem (core::templates)",
                action
            );
            Ok(())
        }
        _ => Err(format!("Unknown table change action: {}", action)),
    }
}
