use crate::db::Database;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ColumnOption {
    pub id: Option<i64>,
    pub name: String,
    pub color: Option<String>,
    /// Logical timestamp for cross-node merge. Defaults to 0 for payloads
    /// from pre-0.7.0 nodes, which makes them lose every merge (safer than
    /// overwriting a local non-zero value).
    #[serde(default)]
    pub modified_time: i64,
    /// `None` = live row. `Some(t)` = tombstone deleted at `t`. Carried in
    /// snapshots so deletions propagate even when broadcasts are lost.
    #[serde(default)]
    pub deleted_at: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ColumnDefinition {
    pub id: Option<i64>,
    pub job_path: String,
    pub folder_name: String,
    pub column_name: String,
    pub column_type: String,
    pub column_order: i32,
    pub column_width: f64,
    pub is_visible: bool,
    pub default_value: Option<String>,
    pub options: Vec<ColumnOption>,
    #[serde(default)]
    pub modified_time: i64,
    #[serde(default)]
    pub deleted_at: Option<i64>,
    /// Phase 2 — points at a v2 template (`<farm>/ufb/templates/
    /// <uuid>.json`) when this column has been auto-promoted to a
    /// shared schema definition. NULL for legacy / local-only rows.
    /// Phase 7 backfills NULL rows on first launch under a flag.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub template_hash: Option<String>,
    /// 0.9.97 — non-NULL marks the column as trashed (hidden from
    /// grids on every peer, recoverable from the Trash dialog). Mesh-
    /// broadcast on every change so trashing on one peer trashes on
    /// all. Independent from `deleted_at`, which now means
    /// "permanently deleted" (post-empty-trash).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub trashed_at: Option<i64>,
    /// v5 epoch — stable, cross-peer-deterministic column identity
    /// (see `derive_column_uuid`). Assigned once at creation, carried on
    /// the wire + in snapshots, and used as the merge key (instead of the
    /// natural key) and as the per-item metadata-blob key. `None` only
    /// transiently for a row that predates the field; resolved on read.
    #[serde(default)]
    pub column_uuid: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ColumnPreset {
    pub id: Option<i64>,
    pub preset_name: String,
    pub columns_json: String,
    pub created_time: i64,
    pub modified_time: i64,
    #[serde(default)]
    pub deleted_at: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PresetColumnDef {
    pub column_name: String,
    pub column_type: String,
    pub column_order: i32,
    pub column_width: f64,
    pub is_visible: bool,
    pub default_value: Option<String>,
    pub options: Vec<ColumnOption>,
}

/// Cache key: (job_path, folder_name)
type CacheKey = (String, String);

/// Canonicalise a path argument to tagged-identity storage form
/// (`vol:…` / `native:…`) for DB reads + writes AND the in-memory cache
/// key. Idempotent. Applying it everywhere a `job_path` enters keeps
/// the cache key consistent whether the source was a QML-native path or
/// a string read back from a DB row.
fn to_storage(p: &str) -> String {
    let mappings = crate::settings::AppSettings::load().path_mappings;
    crate::utils::to_identity_storage(p, &mappings)
}

/// Namespace for deterministic column identities (UUIDv5). Fixed forever;
/// changing it would re-id every column. Distinct from the volume
/// namespace in `volumes.rs`.
const COLUMN_UUID_NS: uuid::Uuid =
    uuid::Uuid::from_u128(0x4b2c_9a17_6f3d_4e88_b1a0_5c7e_9d24_38f1);

/// Derive a STABLE, cross-peer-deterministic identity for a column from
/// its logical key. Two machines (incl. macOS ↔ Windows) that hold the
/// "same" column independently produce the SAME uuid, so they converge
/// instead of splitting into duplicate columns. The name + type are
/// case-folded so "Status"/"status" collapse (the cross-OS case hazard
/// characterised in `identity::tests`). `job_path` MUST already be in
/// tagged-identity form (`vol:`/`native:`) — pass the stored value, which
/// both OSes derive identically via `identity::classify`.
///
/// This value is assigned ONCE (at column creation, and as a one-time
/// backfill for legacy rows) and then STORED; renaming a column updates
/// `column_name` only and never re-derives the uuid — so a column's
/// identity is stable across renames even though the derivation reads the
/// name. The derivation is only the initial-value picker, and is the
/// mechanism by which two peers that independently create the same
/// logical column land on one identity.
pub fn derive_column_uuid(
    job_path: &str,
    folder_name: &str,
    column_name: &str,
    column_type: &str,
) -> String {
    // \x1f (ASCII unit separator) cannot occur in a path/name/type, so
    // the joined key is unambiguous (no delimiter-collision aliasing).
    let key = format!(
        "{}\x1f{}\x1f{}\x1f{}",
        job_path,
        folder_name,
        column_name.to_lowercase(),
        column_type.to_lowercase(),
    );
    uuid::Uuid::new_v5(&COLUMN_UUID_NS, key.as_bytes()).to_string()
}

pub struct ColumnConfigManager {
    db: Arc<Database>,
    cache: Mutex<HashMap<CacheKey, Vec<ColumnDefinition>>>,
}

impl ColumnConfigManager {
    pub fn new(db: Arc<Database>) -> Self {
        Self {
            db,
            cache: Mutex::new(HashMap::new()),
        }
    }

    pub fn get_column_defs(
        &self,
        job_path: &str,
        folder_name: &str,
    ) -> Result<Vec<ColumnDefinition>, String> {
        let job_path = &to_storage(job_path);
        // Check cache first
        let key = (job_path.to_string(), folder_name.to_string());
        {
            let cache = self.cache.lock().unwrap();
            if let Some(defs) = cache.get(&key) {
                return Ok(defs.clone());
            }
        }

        let defs = self
            .db
            .with_conn(|conn| {
                // 0.9.97 — return all non-trashed, non-permanently-
                // deleted rows. The template_hash filter (promoted-
                // only) lives on the consumer side now: grid views
                // (ItemListPanel, TrackerView) skip rows with NULL
                // template_hash to keep unpromoted state out of the
                // user-facing grid; the Column Manager renders them
                // so the user can promote, trash, or rename. Without
                // this, unpromoted rows had no UI surface at all and
                // were stranded.
                let mut stmt = conn.prepare_cached(
                    "SELECT id, job_path, folder_name, column_name, column_type,
                            column_order, column_width, is_visible, default_value,
                            modified_time, template_hash, trashed_at, column_uuid
                     FROM column_definitions
                     WHERE job_path = ?1 AND (folder_name = ?2 OR folder_name = '*')
                       AND deleted_at IS NULL
                       AND trashed_at IS NULL
                     ORDER BY
                         CASE WHEN folder_name = ?2 THEN 0 ELSE 1 END,
                         column_order",
                )?;
                let mut defs: Vec<ColumnDefinition> = stmt
                    .query_map(rusqlite::params![job_path, folder_name], |row| {
                        Ok(ColumnDefinition {
                            id: Some(row.get(0)?),
                            job_path: row.get(1)?,
                            folder_name: row.get(2)?,
                            column_name: row.get(3)?,
                            column_type: row.get(4)?,
                            column_order: row.get(5)?,
                            column_width: row.get(6)?,
                            is_visible: row.get::<_, i64>(7)? != 0,
                            default_value: row.get(8)?,
                            options: vec![],
                            modified_time: row.get(9)?,
                            deleted_at: None,
                            template_hash: row.get::<_, Option<String>>(10)?,
                            trashed_at: row.get::<_, Option<i64>>(11)?,
                            column_uuid: row.get::<_, Option<String>>(12)?,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;

                for def in &mut defs {
                    if let Some(col_id) = def.id {
                        let mut opt_stmt = conn.prepare_cached(
                            "SELECT id, option_name, option_color, modified_time
                             FROM column_options
                             WHERE column_id = ?1 AND deleted_at IS NULL",
                        )?;
                        def.options = opt_stmt
                            .query_map([col_id], |row| {
                                Ok(ColumnOption {
                                    id: Some(row.get(0)?),
                                    name: row.get(1)?,
                                    color: row.get(2)?,
                                    modified_time: row.get(3)?,
                                    deleted_at: None,
                                })
                            })?
                            .collect::<Result<Vec<_>, _>>()?;
                    }
                }

                // Overlay per-user layout (column_order / column_width
                // / is_visible) — these are personal preferences and
                // shadow the team-shared values from column_definitions
                // when present. See `column_layout_personal` table.
                let mut layout_stmt = conn.prepare_cached(
                    "SELECT column_name, column_order, column_width, is_visible
                     FROM column_layout_personal
                     WHERE job_path = ?1 AND folder_name = ?2",
                )?;
                let mut overlay: HashMap<String, (Option<i32>, Option<f64>, Option<i64>)> =
                    HashMap::new();
                let rows = layout_stmt.query_map(
                    rusqlite::params![job_path, folder_name],
                    |row| {
                        Ok((
                            row.get::<_, String>(0)?,
                            row.get::<_, Option<i32>>(1)?,
                            row.get::<_, Option<f64>>(2)?,
                            row.get::<_, Option<i64>>(3)?,
                        ))
                    },
                )?;
                for r in rows {
                    let (name, order, width, visible) = r?;
                    overlay.insert(name, (order, width, visible));
                }
                for def in &mut defs {
                    if let Some((order, width, visible)) = overlay.get(&def.column_name) {
                        if let Some(o) = order {
                            def.column_order = *o;
                        }
                        if let Some(w) = width {
                            def.column_width = *w;
                        }
                        if let Some(v) = visible {
                            def.is_visible = *v != 0;
                        }
                    }
                }
                // Re-sort after overlay; the SQL ORDER BY used the
                // pre-overlay order.
                defs.sort_by_key(|d| d.column_order);

                Ok(defs)
            })
            .map_err(|e| e.to_string())?;

        {
            let mut cache = self.cache.lock().unwrap();
            cache.insert(key, defs.clone());
        }

        Ok(defs)
    }

    // ── v5 metadata key translation ──────────────────────────────────
    // Cell values in item_metadata.metadata_json are stored keyed by the
    // column's stable `column_uuid`; QML works in display names. These
    // helpers bridge the two at the binding boundary so a rename never
    // orphans values and cross-OS peers align by id, not case-sensitive
    // name. All read from the cached `get_column_defs`.

    /// Display name → stable column_uuid for (job, folder). Columns
    /// without a uuid (shouldn't occur post-v5) are skipped.
    pub fn name_to_uuid_map(&self, job_path: &str, folder_name: &str) -> HashMap<String, String> {
        let mut m = HashMap::new();
        if let Ok(defs) = self.get_column_defs(job_path, folder_name) {
            for d in defs {
                if let Some(u) = d.column_uuid {
                    m.insert(d.column_name, u);
                }
            }
        }
        m
    }

    /// Stored column_uuid → display name for (job, folder). Inverse of
    /// `name_to_uuid_map`.
    pub fn uuid_to_name_map(&self, job_path: &str, folder_name: &str) -> HashMap<String, String> {
        let mut m = HashMap::new();
        if let Ok(defs) = self.get_column_defs(job_path, folder_name) {
            for d in defs {
                if let Some(u) = d.column_uuid {
                    m.insert(u, d.column_name);
                }
            }
        }
        m
    }

    /// Resolve one column display name to its stable uuid, or None when no
    /// such column exists in (job, folder).
    pub fn column_uuid_for_name(
        &self,
        job_path: &str,
        folder_name: &str,
        name: &str,
    ) -> Option<String> {
        self.get_column_defs(job_path, folder_name)
            .ok()?
            .into_iter()
            .find(|d| d.column_name == name)
            .and_then(|d| d.column_uuid)
    }

    /// Rename the top-level keys of a JSON object string per `map`. Keys
    /// absent from `map` pass through unchanged (so a value for a
    /// since-deleted column survives, and an already-correct key is
    /// harmless). Non-object / unparseable input is returned verbatim.
    pub fn remap_top_level_keys(blob: &str, map: &HashMap<String, String>) -> String {
        let v: serde_json::Value = match serde_json::from_str(blob) {
            Ok(v) => v,
            Err(_) => return blob.to_string(),
        };
        let obj = match v.as_object() {
            Some(o) => o,
            None => return blob.to_string(),
        };
        let mut out = serde_json::Map::with_capacity(obj.len());
        for (k, val) in obj {
            let nk = map.get(k).cloned().unwrap_or_else(|| k.clone());
            out.insert(nk, val.clone());
        }
        serde_json::to_string(&serde_json::Value::Object(out)).unwrap_or_else(|_| blob.to_string())
    }

    pub fn add_column(&self, def: &ColumnDefinition) -> Result<i64, String> {
        use rusqlite::OptionalExtension;
        // Canonicalise job_path to tagged-identity form so DB writes +
        // the cache key match reads. Shadowing `def` propagates it to
        // every field access below.
        let def = &ColumnDefinition {
            job_path: to_storage(&def.job_path),
            ..def.clone()
        };
        let now = crate::utils::current_time_ms();
        // Stable identity, derived from the canonical logical key so two
        // peers (incl. macOS↔Windows) that create this column converge on
        // one id. Assigned only on the INSERT path; the UPDATE-existing
        // branch leaves the row's stored uuid untouched (rename-safe).
        let column_uuid =
            derive_column_uuid(&def.job_path, &def.folder_name, &def.column_name, &def.column_type);
        let id = self
            .db
            .with_conn(|conn| {
                let existing: Option<i64> = conn
                    .query_row(
                        "SELECT id FROM column_definitions
                         WHERE job_path = ?1 AND folder_name = ?2 AND column_name = ?3",
                        rusqlite::params![def.job_path, def.folder_name, def.column_name],
                        |row| row.get(0),
                    )
                    .optional()?;

                let col_id = if let Some(existing_id) = existing {
                    conn.execute(
                        "UPDATE column_definitions SET
                             column_type = ?1, column_order = ?2, column_width = ?3,
                             is_visible = ?4, default_value = ?5,
                             modified_time = ?6, deleted_at = NULL
                         WHERE id = ?7",
                        rusqlite::params![
                            def.column_type,
                            def.column_order,
                            def.column_width,
                            def.is_visible as i64,
                            def.default_value,
                            now,
                            existing_id,
                        ],
                    )?;
                    conn.execute(
                        "DELETE FROM column_options WHERE column_id = ?1",
                        [existing_id],
                    )?;
                    existing_id
                } else {
                    conn.execute(
                        "INSERT INTO column_definitions
                         (job_path, folder_name, column_name, column_type, column_order, column_width, is_visible, default_value, modified_time, deleted_at, column_uuid)
                         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, NULL, ?10)",
                        rusqlite::params![
                            def.job_path,
                            def.folder_name,
                            def.column_name,
                            def.column_type,
                            def.column_order,
                            def.column_width,
                            def.is_visible as i64,
                            def.default_value,
                            now,
                            column_uuid,
                        ],
                    )?;
                    conn.last_insert_rowid()
                };

                for opt in &def.options {
                    conn.execute(
                        "INSERT INTO column_options (column_id, option_name, option_color, modified_time, deleted_at) VALUES (?1, ?2, ?3, ?4, NULL)",
                        rusqlite::params![col_id, opt.name, opt.color, now],
                    )?;
                }

                Ok(col_id)
            })
            .map_err(|e| e.to_string())?;

        self.invalidate_cache(&def.job_path, &def.folder_name);
        Ok(id)
    }

    pub fn update_column(&self, def: &ColumnDefinition) -> Result<(), String> {
        let def = &ColumnDefinition {
            job_path: to_storage(&def.job_path),
            ..def.clone()
        };
        let now = crate::utils::current_time_ms();
        self.db
            .with_conn(|conn| {
                let col_id = def.id.ok_or(rusqlite::Error::InvalidParameterName(
                    "missing column id".to_string(),
                ))?;
                conn.execute(
                    "UPDATE column_definitions SET
                         column_name = ?1, column_type = ?2, column_order = ?3,
                         column_width = ?4, is_visible = ?5, default_value = ?6,
                         modified_time = ?7, deleted_at = NULL
                     WHERE id = ?8",
                    rusqlite::params![
                        def.column_name,
                        def.column_type,
                        def.column_order,
                        def.column_width,
                        def.is_visible as i64,
                        def.default_value,
                        now,
                        col_id,
                    ],
                )?;

                conn.execute(
                    "DELETE FROM column_options WHERE column_id = ?1",
                    [col_id],
                )?;
                for opt in &def.options {
                    conn.execute(
                        "INSERT INTO column_options (column_id, option_name, option_color, modified_time, deleted_at) VALUES (?1, ?2, ?3, ?4, NULL)",
                        rusqlite::params![col_id, opt.name, opt.color, now],
                    )?;
                }

                Ok(())
            })
            .map_err(|e| e.to_string())?;

        self.invalidate_cache(&def.job_path, &def.folder_name);
        Ok(())
    }

    /// Replace `column_options` for a single column id with the
    /// given option list. Atomically deletes existing rows and
    /// inserts the new ones. Used by the v2 templates "Use it"
    /// flow when a freshly-added column should pick up its options
    /// from an existing shared template rather than from the
    /// (empty / user-typed) form input.
    pub fn set_options_for_column(
        &self,
        column_id: i64,
        options: &[ColumnOption],
    ) -> Result<(), String> {
        let now = crate::utils::current_time_ms();
        self.db
            .with_conn(|conn| {
                conn.execute(
                    "DELETE FROM column_options WHERE column_id = ?1",
                    [column_id],
                )?;
                for opt in options {
                    conn.execute(
                        "INSERT INTO column_options
                         (column_id, option_name, option_color, modified_time, deleted_at)
                         VALUES (?1, ?2, ?3, ?4, NULL)",
                        rusqlite::params![column_id, opt.name, opt.color, now],
                    )?;
                }
                Ok(())
            })
            .map_err(|e| e.to_string())?;
        // Cache flush — caller typically holds a (job, folder) but
        // we don't have it here; conservative full-flush via the
        // existing helper. This path is rare (only "Use it" + a
        // small subset of migration cases).
        self.invalidate_all_caches();
        Ok(())
    }

    /// For every column in `(job_path, folder_name)` carrying a
    /// `template_hash`, fetch the authoritative template file from the
    /// shared `<farm>/ufb/templates/` (with local-cache fallback) and
    /// replace `column_options` from it. Used by `_refreshColumns` on
    /// folder open so peers converge on the schema-of-record after
    /// missing a broadcast (offline window, LWW-race, or any other
    /// reason the live `col_update` path didn't update them).
    ///
    /// Best-effort: pulls the manifest + changed templates into the
    /// local cache first, then reads cache-first via
    /// `templates::v2::fetch_template`. A fetch failure for a given
    /// column is **silent** — we do NOT clear `template_hash`, since
    /// a transient share outage shouldn't unpromote rows the user can
    /// already see. Returns the number of columns reconciled.
    pub fn reconcile_options_from_templates(
        &self,
        job_path: &str,
        folder_name: &str,
    ) -> Result<usize, String> {
        let job_path = &to_storage(job_path);
        let cache = crate::templates::v2::local_cache_dir();
        let settings = crate::settings::AppSettings::load();
        let remote = if settings.mesh_sync.farm_path.trim().is_empty() {
            None
        } else {
            crate::templates::v2::resolve_dir(&settings.mesh_sync.farm_path).ok()
        };

        // Best-effort manifest+files pull so the per-column reads below
        // see what's currently on the share. Non-fatal — we still
        // reconcile from whatever the cache has if this fails.
        if let Some(ref r) = remote {
            let _ = crate::templates::v2::pull_to_local_cache(r, &cache);
        }

        let count = self
            .db
            .with_conn(|conn| {
                let mut stmt = conn.prepare(
                    "SELECT id, template_hash FROM column_definitions
                     WHERE job_path = ?1 AND folder_name = ?2
                       AND template_hash IS NOT NULL
                       AND deleted_at IS NULL
                       AND trashed_at IS NULL",
                )?;
                let rows: Vec<(i64, String)> = stmt
                    .query_map(rusqlite::params![job_path, folder_name], |row| {
                        Ok((row.get(0)?, row.get(1)?))
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                drop(stmt);
                let now = crate::utils::current_time_ms();
                let mut n = 0usize;
                for (col_id, hash) in rows {
                    let Ok(t) = crate::templates::v2::fetch_template(
                        &cache,
                        remote.as_deref(),
                        &hash,
                    ) else {
                        continue;
                    };
                    conn.execute(
                        "DELETE FROM column_options WHERE column_id = ?1",
                        [col_id],
                    )?;
                    for opt in &t.options {
                        let opt_ts = if opt.modified_time > 0 {
                            opt.modified_time
                        } else {
                            now
                        };
                        conn.execute(
                            "INSERT INTO column_options
                             (column_id, option_name, option_color, modified_time, deleted_at)
                             VALUES (?1, ?2, ?3, ?4, NULL)",
                            rusqlite::params![col_id, opt.name, opt.color, opt_ts],
                        )?;
                    }
                    n += 1;
                }
                Ok::<usize, rusqlite::Error>(n)
            })
            .map_err(|e| e.to_string())?;
        self.invalidate_cache(job_path, folder_name);
        Ok(count)
    }

    /// Set the `template_hash` on a column row. Used by the
    /// auto-promote flow (bindings layer) after a column is added or
    /// updated and a template UUID is determined. NULL clears the
    /// link (the row falls back to its inline schema).
    pub fn set_template_hash(&self, id: i64, hash: Option<&str>) -> Result<(), String> {
        self.db
            .with_conn(|conn| {
                conn.execute(
                    "UPDATE column_definitions SET template_hash = ?1 WHERE id = ?2",
                    rusqlite::params![hash, id],
                )?;
                Ok(())
            })
            .map_err(|e| e.to_string())?;
        // Cache invalidation: we don't know the (job, folder) from
        // just an id without a query, but next get_column_defs() on
        // the affected (job, folder) will see the updated row.
        // Caller already typically invalidates via the surrounding
        // add/update flow.
        Ok(())
    }

    /// Read the `template_hash` for a single column row (None when
    /// NULL or row missing). Used when bindings layer needs to
    /// decide between "update the linked template" vs "lookup-or-
    /// mint a fresh one" on column update.
    pub fn get_template_hash(&self, id: i64) -> Result<Option<String>, String> {
        self.db
            .with_conn(|conn| {
                use rusqlite::OptionalExtension;
                let hash: Option<Option<String>> = conn
                    .query_row(
                        "SELECT template_hash FROM column_definitions WHERE id = ?1",
                        [id],
                        |row| row.get::<_, Option<String>>(0),
                    )
                    .optional()?;
                Ok(hash.flatten())
            })
            .map_err(|e| e.to_string())
    }

    /// Write the per-user layout overlay for a (job, folder, column).
    /// Each `Option<…>` parameter is "leave unchanged" when `None`,
    /// "set to this value" when `Some`. Never mesh-broadcasts —
    /// these fields are intentionally personal. Cache for the
    /// folder is invalidated on success.
    pub fn set_column_layout_personal(
        &self,
        job_path: &str,
        folder_name: &str,
        column_name: &str,
        column_order: Option<i32>,
        column_width: Option<f64>,
        is_visible: Option<bool>,
    ) -> Result<(), String> {
        let job_path = &to_storage(job_path);
        let now = crate::utils::current_time_ms();
        self.db
            .with_conn(|conn| {
                conn.execute(
                    "INSERT INTO column_layout_personal
                        (job_path, folder_name, column_name,
                         column_order, column_width, is_visible,
                         modified_time)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                     ON CONFLICT(job_path, folder_name, column_name)
                     DO UPDATE SET
                        column_order = COALESCE(excluded.column_order, column_layout_personal.column_order),
                        column_width = COALESCE(excluded.column_width, column_layout_personal.column_width),
                        is_visible = COALESCE(excluded.is_visible, column_layout_personal.is_visible),
                        modified_time = excluded.modified_time",
                    rusqlite::params![
                        job_path,
                        folder_name,
                        column_name,
                        column_order,
                        column_width,
                        is_visible.map(|b| b as i64),
                        now,
                    ],
                )?;
                Ok(())
            })
            .map_err(|e: rusqlite::Error| e.to_string())?;
        self.invalidate_cache(job_path, folder_name);
        Ok(())
    }

    /// One-time migration: copy each existing column_definitions row's
    /// (column_order, column_width, is_visible) into
    /// column_layout_personal, so users keep their current view when
    /// upgrading. Idempotent (INSERT OR IGNORE) — running again on a
    /// machine that already migrated is a no-op for previously-seen
    /// (job, folder, column) tuples but does pick up newly-arrived
    /// columns the user hasn't customised yet.
    pub fn seed_personal_layout_from_definitions(&self) -> Result<u64, String> {
        let now = crate::utils::current_time_ms();
        let inserted = self
            .db
            .with_conn(|conn| {
                conn.execute(
                    "INSERT OR IGNORE INTO column_layout_personal
                        (job_path, folder_name, column_name,
                         column_order, column_width, is_visible,
                         modified_time)
                     SELECT job_path, folder_name, column_name,
                            column_order, column_width, is_visible, ?1
                     FROM column_definitions
                     WHERE deleted_at IS NULL",
                    [now],
                )
            })
            .map_err(|e| e.to_string())?;
        Ok(inserted as u64)
    }

    /// Read a single column row by id including options. Used by the
    /// 0.9.97 lifecycle actions (trash/untrash/promote/unpromote) to
    /// snapshot the current state for the mesh broadcast payload.
    /// Ignores trash / delete state — callers may need to broadcast
    /// transitions on already-trashed rows.
    pub fn get_column_by_id(&self, id: i64) -> Option<ColumnDefinition> {
        self.db
            .with_conn(|conn| {
                let mut def = conn.query_row(
                    "SELECT id, job_path, folder_name, column_name, column_type,
                            column_order, column_width, is_visible, default_value,
                            modified_time, template_hash, trashed_at, deleted_at, column_uuid
                     FROM column_definitions WHERE id = ?1",
                    [id],
                    |row| {
                        Ok(ColumnDefinition {
                            id: Some(row.get(0)?),
                            job_path: row.get(1)?,
                            folder_name: row.get(2)?,
                            column_name: row.get(3)?,
                            column_type: row.get(4)?,
                            column_order: row.get(5)?,
                            column_width: row.get(6)?,
                            is_visible: row.get::<_, i64>(7)? != 0,
                            default_value: row.get(8)?,
                            options: vec![],
                            modified_time: row.get(9)?,
                            deleted_at: row.get::<_, Option<i64>>(12)?,
                            template_hash: row.get::<_, Option<String>>(10)?,
                            trashed_at: row.get::<_, Option<i64>>(11)?,
                            column_uuid: row.get::<_, Option<String>>(13)?,
                        })
                    },
                )?;
                let mut opt_stmt = conn.prepare_cached(
                    "SELECT id, option_name, option_color, modified_time
                     FROM column_options WHERE column_id = ?1 AND deleted_at IS NULL",
                )?;
                def.options = opt_stmt
                    .query_map([id], |row| {
                        Ok(ColumnOption {
                            id: Some(row.get(0)?),
                            name: row.get(1)?,
                            color: row.get(2)?,
                            modified_time: row.get(3)?,
                            deleted_at: None,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(def)
            })
            .ok()
    }

    /// 0.9.97 — user-facing "Delete column" now sends the column to
    /// the Trash. Sets `trashed_at`, bumps `modified_time`. Mesh-
    /// broadcast as a `col_update` so every peer trashes too.
    /// Recoverable via `untrash_column`.
    pub fn trash_column(&self, id: i64) -> Result<(String, String, String), String> {
        let now = crate::utils::current_time_ms();
        let key = self
            .db
            .with_conn(|conn| {
                let key = conn.query_row(
                    "SELECT job_path, folder_name, column_name FROM column_definitions WHERE id = ?1",
                    [id],
                    |row| Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, String>(2)?,
                    )),
                )?;
                conn.execute(
                    "UPDATE column_definitions SET modified_time = ?1, trashed_at = ?1 WHERE id = ?2",
                    rusqlite::params![now, id],
                )?;
                Ok(key)
            })
            .map_err(|e| e.to_string())?;

        self.invalidate_cache(&key.0, &key.1);
        Ok(key)
    }

    /// 0.9.97 — restore a trashed column. Pre-flight checks for a
    /// natural-key collision with an active (non-trashed, non-deleted)
    /// row at the same (job_path, folder_name, column_name). When the
    /// caller supplies `new_column_name`, the rename is applied as
    /// part of the untrash; otherwise the original column_name is
    /// reused. On collision returns `Err("RenameRequired: ...")` so
    /// the QML untrash dialog can prompt the user for a new name.
    pub fn untrash_column(
        &self,
        id: i64,
        new_column_name: Option<&str>,
    ) -> Result<(String, String, String), String> {
        use rusqlite::OptionalExtension;
        let now = crate::utils::current_time_ms();
        let (job_path, folder_name, current_name): (String, String, String) = self
            .db
            .with_conn(|conn| {
                conn.query_row(
                    "SELECT job_path, folder_name, column_name
                     FROM column_definitions WHERE id = ?1",
                    [id],
                    |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
                )
            })
            .map_err(|e| e.to_string())?;
        let target_name = new_column_name.unwrap_or(&current_name).to_string();

        let collision: Option<i64> = self
            .db
            .with_conn(|conn| {
                conn.query_row(
                    "SELECT id FROM column_definitions
                     WHERE job_path = ?1 AND folder_name = ?2 AND column_name = ?3
                       AND id != ?4
                       AND trashed_at IS NULL AND deleted_at IS NULL",
                    rusqlite::params![job_path, folder_name, target_name, id],
                    |row| row.get(0),
                )
                .optional()
            })
            .map_err(|e| e.to_string())?;
        if collision.is_some() {
            // Stash the rejected name so the QML untrash dialog can
            // surface it on the next attempt. Cleared on the
            // successful UPDATE below.
            let _ = self.db.with_conn(|conn| {
                conn.execute(
                    "UPDATE column_definitions SET name_conflict_marker = ?1 WHERE id = ?2",
                    rusqlite::params![target_name, id],
                )
            });
            return Err(format!(
                "RenameRequired: an active column named {:?} already exists in {}/{}",
                target_name, job_path, folder_name
            ));
        }

        self.db
            .with_conn(|conn| {
                conn.execute(
                    "UPDATE column_definitions SET
                         column_name = ?1,
                         trashed_at = NULL,
                         name_conflict_marker = NULL,
                         modified_time = ?2
                     WHERE id = ?3",
                    rusqlite::params![target_name, now, id],
                )
            })
            .map_err(|e| e.to_string())?;
        self.invalidate_cache(&job_path, &folder_name);
        Ok((job_path, folder_name, target_name))
    }

    /// 0.9.97 — clear `template_hash` on a column so it reads as
    /// unpromoted (hidden from grids by the visibility filter).
    /// `column_options` rows are preserved so a subsequent
    /// `promote_column` can re-attach the same options to a fresh
    /// template via lookup_or_mint.
    pub fn unpromote_column(&self, id: i64) -> Result<(String, String, String), String> {
        let now = crate::utils::current_time_ms();
        let key = self
            .db
            .with_conn(|conn| {
                let key = conn.query_row(
                    "SELECT job_path, folder_name, column_name
                     FROM column_definitions WHERE id = ?1",
                    [id],
                    |row| Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, String>(2)?,
                    )),
                )?;
                conn.execute(
                    "UPDATE column_definitions SET
                         template_hash = NULL,
                         modified_time = ?1
                     WHERE id = ?2",
                    rusqlite::params![now, id],
                )?;
                Ok(key)
            })
            .map_err(|e| e.to_string())?;
        self.invalidate_cache(&key.0, &key.1);
        Ok(key)
    }

    /// 0.9.97 — terminal hard-delete (post-empty-trash). Sets
    /// `deleted_at`, bumps `modified_time`. Mesh-broadcast as
    /// `col_delete` (terminal action — wins LWW). After a future
    /// janitor pass, rows with `deleted_at` older than the retention
    /// window are removed from the DB entirely.
    pub fn permanently_delete_column(
        &self,
        id: i64,
    ) -> Result<(String, String, String), String> {
        let now = crate::utils::current_time_ms();
        let key = self
            .db
            .with_conn(|conn| {
                let key = conn.query_row(
                    "SELECT job_path, folder_name, column_name FROM column_definitions WHERE id = ?1",
                    [id],
                    |row| Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, String>(2)?,
                    )),
                )?;
                conn.execute(
                    "UPDATE column_definitions SET modified_time = ?1, deleted_at = ?1 WHERE id = ?2",
                    rusqlite::params![now, id],
                )?;
                conn.execute(
                    "UPDATE column_options SET modified_time = ?1, deleted_at = ?1 WHERE column_id = ?2",
                    rusqlite::params![now, id],
                )?;
                Ok(key)
            })
            .map_err(|e| e.to_string())?;
        self.invalidate_cache(&key.0, &key.1);
        Ok(key)
    }

    /// List trashed columns scoped to a single (job, folder) for the
    /// Trash dialog. Mirrors `get_column_defs` but flips the visibility
    /// filter — only `trashed_at IS NOT NULL AND deleted_at IS NULL`
    /// rows are returned.
    pub fn list_trashed(
        &self,
        job_path: &str,
        folder_name: &str,
    ) -> Result<Vec<ColumnDefinition>, String> {
        let job_path = &to_storage(job_path);
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare(
                    "SELECT id, job_path, folder_name, column_name, column_type,
                            column_order, column_width, is_visible, default_value,
                            modified_time, template_hash, trashed_at, column_uuid
                     FROM column_definitions
                     WHERE job_path = ?1 AND (folder_name = ?2 OR folder_name = '*')
                       AND trashed_at IS NOT NULL
                       AND deleted_at IS NULL
                     ORDER BY trashed_at DESC",
                )?;
                let defs: Vec<ColumnDefinition> = stmt
                    .query_map(rusqlite::params![job_path, folder_name], |row| {
                        Ok(ColumnDefinition {
                            id: Some(row.get(0)?),
                            job_path: row.get(1)?,
                            folder_name: row.get(2)?,
                            column_name: row.get(3)?,
                            column_type: row.get(4)?,
                            column_order: row.get(5)?,
                            column_width: row.get(6)?,
                            is_visible: row.get::<_, i64>(7)? != 0,
                            default_value: row.get(8)?,
                            options: vec![],
                            modified_time: row.get(9)?,
                            deleted_at: None,
                            template_hash: row.get::<_, Option<String>>(10)?,
                            trashed_at: row.get::<_, Option<i64>>(11)?,
                            column_uuid: row.get::<_, Option<String>>(12)?,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(defs)
            })
            .map_err(|e| e.to_string())
    }

    fn invalidate_cache(&self, job_path: &str, folder_name: &str) {
        let job_path = &to_storage(job_path);
        let mut cache = self.cache.lock().unwrap();
        cache.remove(&(job_path.to_string(), folder_name.to_string()));
    }

    pub fn invalidate_all_caches(&self) {
        let mut cache = self.cache.lock().unwrap();
        cache.clear();
    }

    pub fn reset_local_columns_and_presets(&self) -> Result<(), String> {
        self.db
            .with_conn(|conn| {
                conn.execute("DELETE FROM column_options", [])?;
                conn.execute("DELETE FROM column_definitions", [])?;
                conn.execute("DELETE FROM column_presets", [])?;
                Ok(())
            })
            .map_err(|e| e.to_string())?;
        self.invalidate_all_caches();
        Ok(())
    }

    // ── Column Presets ──

    pub fn get_column_presets(&self) -> Result<Vec<ColumnPreset>, String> {
        self.db
            .with_conn(|conn| {
                let mut stmt = conn.prepare(
                    "SELECT id, preset_name, columns_json, created_time, modified_time
                     FROM column_presets
                     WHERE deleted_at IS NULL
                     ORDER BY preset_name",
                )?;
                let presets = stmt
                    .query_map([], |row| {
                        Ok(ColumnPreset {
                            id: Some(row.get(0)?),
                            preset_name: row.get(1)?,
                            columns_json: row.get(2)?,
                            created_time: row.get(3)?,
                            modified_time: row.get(4)?,
                            deleted_at: None,
                        })
                    })?
                    .collect::<Result<Vec<_>, _>>()?;
                Ok(presets)
            })
            .map_err(|e| e.to_string())
    }

    pub fn save_column_preset(
        &self,
        name: &str,
        column: &ColumnDefinition,
    ) -> Result<(i64, String), String> {
        let preset_def = PresetColumnDef {
            column_name: column.column_name.clone(),
            column_type: column.column_type.clone(),
            column_order: column.column_order,
            column_width: column.column_width,
            is_visible: column.is_visible,
            default_value: column.default_value.clone(),
            options: column.options.clone(),
        };

        let json = serde_json::to_string(&preset_def)
            .map_err(|e| format!("Failed to serialize preset: {}", e))?;

        let now = crate::utils::current_time_ms();

        let id = self
            .db
            .with_conn(|conn| {
                conn.execute(
                    "INSERT INTO column_presets (preset_name, columns_json, created_time, modified_time, deleted_at)
                     VALUES (?1, ?2, ?3, ?4, NULL)
                     ON CONFLICT(preset_name) DO UPDATE SET
                         columns_json = excluded.columns_json,
                         modified_time = excluded.modified_time,
                         deleted_at = NULL",
                    rusqlite::params![name, json, now, now],
                )?;
                Ok(conn.last_insert_rowid())
            })
            .map_err(|e| e.to_string())?;

        Ok((id, json))
    }

    pub fn delete_column_preset(&self, id: i64) -> Result<Option<String>, String> {
        use rusqlite::OptionalExtension;
        let now = crate::utils::current_time_ms();
        self.db
            .with_conn(|conn| {
                let name: Option<String> = conn
                    .query_row(
                        "SELECT preset_name FROM column_presets WHERE id = ?1 AND deleted_at IS NULL",
                        [id],
                        |row| row.get::<_, String>(0),
                    )
                    .optional()?;
                if name.is_some() {
                    conn.execute(
                        "UPDATE column_presets SET modified_time = ?1, deleted_at = ?1 WHERE id = ?2",
                        rusqlite::params![now, id],
                    )?;
                }
                Ok(name)
            })
            .map_err(|e| e.to_string())
    }

    /// Legacy entry point — kept for the migration path that reads
    /// pre-template-flow rows from `column_presets`. New code should
    /// call `add_preset_column_from_def` directly with a definition
    /// loaded from the filesystem template store.
    pub fn add_preset_column(
        &self,
        preset_id: i64,
        job_path: &str,
        folder_name: &str,
    ) -> Result<ColumnDefinition, String> {
        let columns_json: String = self
            .db
            .with_conn(|conn| {
                conn.query_row(
                    "SELECT columns_json FROM column_presets WHERE id = ?1 AND deleted_at IS NULL",
                    [preset_id],
                    |row| row.get(0),
                )
            })
            .map_err(|e| format!("Preset not found: {}", e))?;

        let preset_def: PresetColumnDef = serde_json::from_str(&columns_json)
            .map_err(|e| format!("Failed to parse preset JSON: {}", e))?;
        self.add_preset_column_from_def(&preset_def, job_path, folder_name)
    }

    /// Apply a preset definition to `(job_path, folder_name)` —
    /// inserts (or revives + updates) the column row + replaces its
    /// options. Used by the new filesystem-backed template apply
    /// flow in bindings/src/services/columns.rs.
    pub fn add_preset_column_from_def(
        &self,
        preset_def: &PresetColumnDef,
        job_path: &str,
        folder_name: &str,
    ) -> Result<ColumnDefinition, String> {
        let job_path = &to_storage(job_path);
        let next_order: i32 = self
            .db
            .with_conn(|conn| {
                conn.query_row(
                    "SELECT COALESCE(MAX(column_order), -1) + 1 FROM column_definitions
                     WHERE job_path = ?1 AND folder_name = ?2 AND deleted_at IS NULL",
                    rusqlite::params![job_path, folder_name],
                    |row| row.get(0),
                )
            })
            .map_err(|e| e.to_string())?;

        let now = crate::utils::current_time_ms();
        // Same stable identity derivation as add_column — a preset applied
        // on two peers lands on one column id.
        let column_uuid = derive_column_uuid(
            job_path,
            folder_name,
            &preset_def.column_name,
            &preset_def.column_type,
        );
        let col_id = self
            .db
            .with_conn(|conn| {
                use rusqlite::OptionalExtension;
                let existing: Option<i64> = conn
                    .query_row(
                        "SELECT id FROM column_definitions
                         WHERE job_path = ?1 AND folder_name = ?2 AND column_name = ?3",
                        rusqlite::params![job_path, folder_name, preset_def.column_name],
                        |row| row.get(0),
                    )
                    .optional()?;

                let col_id = if let Some(existing_id) = existing {
                    conn.execute(
                        "UPDATE column_definitions SET
                             column_type = ?1, column_order = ?2, column_width = ?3,
                             is_visible = ?4, default_value = ?5,
                             modified_time = ?6, deleted_at = NULL
                         WHERE id = ?7",
                        rusqlite::params![
                            preset_def.column_type,
                            next_order,
                            preset_def.column_width,
                            preset_def.is_visible as i64,
                            preset_def.default_value,
                            now,
                            existing_id,
                        ],
                    )?;
                    conn.execute(
                        "DELETE FROM column_options WHERE column_id = ?1",
                        [existing_id],
                    )?;
                    existing_id
                } else {
                    conn.execute(
                        "INSERT INTO column_definitions
                         (job_path, folder_name, column_name, column_type, column_order, column_width, is_visible, default_value, modified_time, deleted_at, column_uuid)
                         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, NULL, ?10)",
                        rusqlite::params![
                            job_path,
                            folder_name,
                            preset_def.column_name,
                            preset_def.column_type,
                            next_order,
                            preset_def.column_width,
                            preset_def.is_visible as i64,
                            preset_def.default_value,
                            now,
                            column_uuid,
                        ],
                    )?;
                    conn.last_insert_rowid()
                };

                for opt in &preset_def.options {
                    conn.execute(
                        "INSERT INTO column_options (column_id, option_name, option_color, modified_time, deleted_at) VALUES (?1, ?2, ?3, ?4, NULL)",
                        rusqlite::params![col_id, opt.name, opt.color, now],
                    )?;
                }

                Ok(col_id)
            })
            .map_err(|e| e.to_string())?;

        self.invalidate_cache(job_path, folder_name);

        Ok(ColumnDefinition {
            id: Some(col_id),
            job_path: job_path.to_string(),
            folder_name: folder_name.to_string(),
            column_name: preset_def.column_name.clone(),
            column_type: preset_def.column_type.clone(),
            column_order: next_order,
            column_width: preset_def.column_width,
            is_visible: preset_def.is_visible,
            default_value: preset_def.default_value.clone(),
            options: preset_def
                .options
                .iter()
                .cloned()
                .map(|o| ColumnOption {
                    id: None,
                    name: o.name,
                    color: o.color,
                    modified_time: now,
                    deleted_at: None,
                })
                .collect(),
            modified_time: now,
            deleted_at: None,
            template_hash: None,
            trashed_at: None,
            column_uuid: Some(column_uuid),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::{derive_column_uuid, ColumnConfigManager};
    use std::collections::HashMap;

    #[test]
    fn remap_top_level_keys_renames_and_passes_through() {
        // name→uuid for two known columns; a third key has no mapping.
        let mut m = HashMap::new();
        m.insert("Status".to_string(), "uuid-a".to_string());
        m.insert("Priority".to_string(), "uuid-b".to_string());
        let blob = r#"{"Status":"High","Priority":3,"Legacy":"keep"}"#;
        let out = ColumnConfigManager::remap_top_level_keys(blob, &m);
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["uuid-a"], "High");
        assert_eq!(v["uuid-b"], 3);
        assert_eq!(v["Legacy"], "keep"); // unmapped key passes through
        assert!(v.get("Status").is_none());
    }

    #[test]
    fn remap_round_trips_name_uuid_name() {
        let mut to_uuid = HashMap::new();
        to_uuid.insert("Status".to_string(), "uuid-a".to_string());
        let mut to_name = HashMap::new();
        to_name.insert("uuid-a".to_string(), "Status".to_string());
        let blob = r#"{"Status":"High"}"#;
        let stored = ColumnConfigManager::remap_top_level_keys(blob, &to_uuid);
        let shown = ColumnConfigManager::remap_top_level_keys(&stored, &to_name);
        let v: serde_json::Value = serde_json::from_str(&shown).unwrap();
        assert_eq!(v["Status"], "High");
    }

    #[test]
    fn remap_leaves_non_object_untouched() {
        let m = HashMap::new();
        assert_eq!(ColumnConfigManager::remap_top_level_keys("[1,2]", &m), "[1,2]");
        assert_eq!(ColumnConfigManager::remap_top_level_keys("not json", &m), "not json");
    }

    // job_path is already in identity form (vol:…) on BOTH machines —
    // `identity::classify` collapses mac /Volumes and win drive/UNC forms
    // to the same string — so these derivations agree cross-OS.
    #[test]
    fn column_uuid_converges_for_same_logical_column() {
        let a = derive_column_uuid("vol:vol-union/261301_pmkn", "3d", "Status", "dropdown");
        let b = derive_column_uuid("vol:vol-union/261301_pmkn", "3d", "Status", "dropdown");
        assert_eq!(a, b);
    }

    #[test]
    fn column_uuid_is_case_insensitive_on_name_and_type() {
        // The cross-OS "Status" vs "status" split must NOT happen.
        let a = derive_column_uuid("vol:v/j", "f", "Status", "Dropdown");
        let b = derive_column_uuid("vol:v/j", "f", "status", "dropdown");
        assert_eq!(a, b, "name/type case must not split identity");
    }

    #[test]
    fn column_uuid_distinguishes_different_columns() {
        let status = derive_column_uuid("vol:v/j", "f", "Status", "dropdown");
        let prio = derive_column_uuid("vol:v/j", "f", "Priority", "dropdown");
        let other_folder = derive_column_uuid("vol:v/j", "f2", "Status", "dropdown");
        assert_ne!(status, prio, "different names must differ");
        assert_ne!(status, other_folder, "different folders must differ");
    }

    #[test]
    fn column_uuid_is_a_valid_uuid_string() {
        let u = derive_column_uuid("vol:v/j", "f", "Status", "dropdown");
        assert!(uuid::Uuid::parse_str(&u).is_ok(), "must be a parseable uuid");
        assert_eq!(u.len(), 36);
    }
}
