//! `Columns` QObject — wraps `core::columns::ColumnConfigManager`.
//!
//! Read-only `get_column_defs` plus add / update / delete + presets.
//! Every mutation also fires a mesh broadcast via
//! `super::mesh::broadcast_table_change` so peers receive the change
//! and the leader marks the snapshot dirty.
//!
//! Singleton; uses `bindings::db::shared_db()` so it shares one DB
//! connection with Bookmarks + Subscription.

use crate::db::shared_db;
use std::pin::Pin;
use std::sync::{Arc, OnceLock};
use ufb_core::columns::{ColumnConfigManager, ColumnDefinition, PresetColumnDef};
use ufb_core::settings::AppSettings;
use ufb_core::templates;
use ufb_core::utils::project_slug_for;

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
        // Monotonic counter — bumped whenever column DEFINITIONS change
        // (add / edit / delete / option / order / visibility), locally or
        // via a mesh push. Mirrors Subscription.metadata_rev (which covers
        // cell VALUES). Consumers (TrackerView, ItemListPanel) observe
        // Changed and rebuild visibleColumns, so remote — and local-while-
        // a-panel-is-open — schema changes surface without a manual
        // refresh or reopen. The single bump point is `invalidate`: local
        // mutations route through ColumnManagerDialog.refresh() (which
        // calls invalidate), and the mesh-receive path already calls
        // Columns.invalidate("","") from Sidebar's table/data-rev handlers.
        #[qproperty(i32, columns_rev)]
        // Monotonic counter — bumped when a BACKGROUND
        // reconcile_options_from_templates actually writes new option
        // rows. Distinct from columns_rev so the panels can re-render
        // cells (picking up freshly-reconciled options) WITHOUT
        // re-triggering another reconcile — that separation is what
        // keeps the async reconcile from looping. Observed via
        // onOptions_revChanged → reconcile-free rebuild.
        #[qproperty(i32, options_rev)]
        type Columns = super::ColumnsRust;

        /// Return the column definitions for `(job_path, folder)` as
        /// a JSON array. Includes wildcard-folder ('*') entries the
        /// legacy uses for "applies to every folder in this job".
        /// Returns "[]" on any error.
        #[qinvokable]
        fn get_column_defs(
            self: &Columns,
            job_path: QString,
            folder: QString,
        ) -> QString;

        /// Drop the cached entry for `(job_path, folder)` so the next
        /// get_column_defs hits the DB, and bump `columns_rev` to notify
        /// observers (panels) that column definitions may have changed.
        /// This is the single reactivity choke point — see the
        /// `columns_rev` note above.
        #[qinvokable]
        fn invalidate(self: Pin<&mut Columns>, job_path: QString, folder: QString);

        /// Add (or upsert by name) a column. Returns the new id, or
        /// -1 on error. Idempotent: re-adding by the same
        /// (jobPath, folder, name) updates rather than duplicates.
        #[qinvokable]
        fn add_column(
            self: &Columns,
            job_path: QString,
            folder_name: QString,
            column_name: QString,
            column_type: QString,
            default_value: QString,
            is_visible: bool,
            column_order: i32,
            column_width: f64,
        ) -> i64;

        /// Update an existing column by id. Editable basic fields:
        /// name / type / default_value / is_visible / column_order /
        /// column_width. Existing column_options are preserved
        /// across updates (the dialog doesn't expose them yet).
        /// Returns true on success.
        #[qinvokable]
        fn update_column(
            self: &Columns,
            id: i64,
            job_path: QString,
            folder_name: QString,
            column_name: QString,
            column_type: QString,
            default_value: QString,
            is_visible: bool,
            column_order: i32,
            column_width: f64,
        ) -> bool;

        /// 0.9.97 — user-facing "Delete column" sends to Trash.
        /// Sets trashed_at locally and broadcasts col_update so every
        /// peer trashes too. Recoverable via `untrash_column`.
        /// Returns true on success.
        #[qinvokable]
        fn delete_column(self: &Columns, id: i64) -> bool;

        /// 0.9.97 — restore a trashed column to active state. When
        /// `new_column_name` is non-empty, renames the column as part
        /// of the untrash (used to resolve a name conflict). Returns
        /// "ok" on success, "RenameRequired: <reason>" when the
        /// current name (or supplied new name) collides with an
        /// active row in the same folder, or "Error: <msg>" for any
        /// other failure. The QML untrash dialog parses the prefix
        /// to decide whether to prompt for a new name.
        #[qinvokable]
        fn untrash_column(
            self: &Columns,
            id: i64,
            new_column_name: QString,
        ) -> QString;

        /// 0.9.97 — clear the column's template_hash so it reads as
        /// unpromoted (hidden from grid by visibility filter).
        /// `column_options` rows are preserved so a later
        /// `promote_column` can re-attach the same options to a
        /// fresh template via lookup_or_mint. Returns true on success.
        #[qinvokable]
        fn unpromote_column(self: &Columns, id: i64) -> bool;

        /// 0.9.97 — promote an unpromoted column. Runs lookup_or_mint
        /// against the column's (common_name_slug, column_type,
        /// project_slug); attaches to an existing matching template
        /// when one exists, otherwise mints a fresh template from the
        /// column's current options. Then broadcasts col_update so
        /// every peer sees the now-promoted state. Returns true on
        /// success.
        #[qinvokable]
        fn promote_column(self: &Columns, id: i64) -> bool;

        /// 0.9.97 — terminal hard-delete (post-empty-trash). Sets
        /// deleted_at, broadcasts col_delete. Wins LWW so the
        /// deletion replays cleanly even when local edits are newer.
        /// Returns true on success.
        #[qinvokable]
        fn permanently_delete_column(self: &Columns, id: i64) -> bool;

        /// 0.9.97 — list trashed columns scoped to a (job, folder)
        /// for the Trash dialog. Returns a JSON array, each entry
        /// `{ id, columnName, columnType, trashedAt }`.
        #[qinvokable]
        fn list_trashed_columns(
            self: &Columns,
            job_path: QString,
            folder_name: QString,
        ) -> QString;

        /// Look up existing v2 templates that match a (commonName,
        /// columnType) pair within the project derived from
        /// `job_path`. Returns a JSON array; empty when no matches
        /// or when the share is unreachable. Used by the lookup-on-
        /// create dialog: when the user types a column name + picks
        /// a type, query before submitting; if matches exist, show
        /// "Use it / Edit it / Create new" UX rather than silently
        /// creating a fork.
        ///
        /// Each result entry: `{ uniqueName, displayName, createdBy,
        /// updatedAt, lastUsedAt, revision }`.
        #[qinvokable]
        fn lookup_template(
            self: &Columns,
            common_name: QString,
            column_type: QString,
            job_path: QString,
        ) -> QString;

        /// Like `add_column`, but forces a fresh template_hash even
        /// if the lookup-on-create dialog showed an existing match
        /// for `(project, common_slug, type)`. The dialog's "Create
        /// new" button calls this; the "Use it" button calls the
        /// regular `add_column` (which reuses existing templates
        /// via `lookup_or_mint`'s default reuse path).
        #[qinvokable]
        fn add_column_force_new(
            self: &Columns,
            job_path: QString,
            folder_name: QString,
            column_name: QString,
            column_type: QString,
            default_value: QString,
            is_visible: bool,
            column_order: i32,
            column_width: f64,
        ) -> i64;

        /// Touch the v2 templates directory on the share. Triggers
        /// `v2::resolve_dir`, which runs the misplaced-file recovery
        /// + (Phase 6) the manifest pull. Wire this into app launch
        /// (Main.qml::Component.onCompleted) so users come up with a
        /// fresh templates view without having to add a column to
        /// kick the lazy resolve.
        #[qinvokable]
        fn refresh_templates(self: &Columns);

        /// Count of column_definitions rows referencing the given
        /// template_hash. Used by the column-edit dialog's "Used on
        /// N folders" propagation banner so the user knows their
        /// option / color edits will affect other folders that
        /// share this template. Returns 0 on any error or empty
        /// hash.
        #[qinvokable]
        fn template_usage_count(self: &Columns, template_hash: QString) -> i32;

        /// Phase 5 — duplicate-to-new-column. Replaces the locked
        /// "rename" path: forks the source column into a new column
        /// with `new_name`, mints a fresh template (force_new), and
        /// optionally migrates per-folder item metadata + deletes
        /// the source.
        ///
        /// Returns the new column id on success, -1 on failure.
        ///
        /// `copy_metadata` (false): every item in (job_path,
        /// folder_name) whose metadata blob has a value under the
        /// source column's display name gets a new entry under the
        /// new column's display name with the same value. Existing
        /// per-folder, no mesh sweep.
        ///
        /// `delete_source` (false): the source column on this
        /// folder is soft-deleted. The underlying source template
        /// stays on the share (other folders still reference it).
        #[qinvokable]
        fn duplicate_column(
            self: &Columns,
            source_id: i64,
            job_path: QString,
            folder_name: QString,
            new_name: QString,
            copy_metadata: bool,
            delete_source: bool,
        ) -> i64;

        /// Item count in a (job_path, folder_name) folder whose
        /// metadata blob has a non-null value under the given
        /// `column_name` key. Drives the "5,247 items have a Status
        /// value" hint in the duplicate dialog so the user knows
        /// the size of the metadata copy operation upfront.
        #[qinvokable]
        fn count_metadata_values(
            self: &Columns,
            job_path: QString,
            folder_name: QString,
            column_name: QString,
        ) -> i32;

        /// Convenience for the "Hide Column" menu — flips is_visible
        /// without requiring the caller to pass every other field.
        /// Per-user overlay only — never mesh-broadcast.
        #[qinvokable]
        fn set_column_visible(
            self: &Columns,
            id: i64,
            job_path: QString,
            folder_name: QString,
            is_visible: bool,
        ) -> bool;

        /// Replace the column's option list (used by dropdown /
        /// priority types). `options_json` is a JSON array of
        /// `{name, color}` objects — anything else is ignored.
        /// Atomically deletes existing options and inserts the new
        /// ones. Returns true on success.
        #[qinvokable]
        fn set_column_options(
            self: &Columns,
            id: i64,
            job_path: QString,
            folder_name: QString,
            options_json: QString,
        ) -> bool;

        /// Pull the latest template files from the share into the
        /// local cache and, for every column in `(job_path,
        /// folder_name)` carrying a `template_hash`, replace
        /// `column_options` from the template's authoritative option
        /// list. Used by `_refreshColumns` on folder open so peers
        /// converge on schema changes they missed via broadcast
        /// (offline window, LWW-race on a back-to-back editor save,
        /// etc.). Returns the number of columns reconciled (0 on any
        /// failure — best-effort).
        #[qinvokable]
        fn reconcile_options_from_templates(
            self: &Columns,
            job_path: QString,
            folder_name: QString,
        ) -> i32;

        /// Reorder configured columns on `(job_path, folder)` to
        /// match the order in `ids_json` (a JSON array of column
        /// IDs). Each listed ID gets its `column_order` set to its
        /// position in the array. IDs not in the array are left
        /// alone. Returns true when every listed column was found
        /// and updated.
        ///
        /// Writes go to the per-user `column_layout_personal`
        /// table only — never mesh-broadcast.
        #[qinvokable]
        fn set_column_order(
            self: &Columns,
            job_path: QString,
            folder_name: QString,
            ids_json: QString,
        ) -> bool;

        /// Set the per-user column width for the (job, folder,
        /// column-by-id) tuple. Writes to `column_layout_personal`
        /// only — width is a personal preference, never broadcast.
        #[qinvokable]
        fn set_column_width(
            self: &Columns,
            id: i64,
            job_path: QString,
            folder_name: QString,
            column_width: f64,
        ) -> bool;

        // ── Presets / templates ─────────────────────────────────────
        // Storage moved off the SQLite `column_presets` table + mesh
        // broadcast, onto the shared filesystem under
        // `<farm_root>/ufb/templates/<unique_name>.json`. The
        // snapshot/restore loop made the old flow leader-only in
        // practice; filesystem-as-source-of-truth makes every peer
        // a first-class writer. See `core::templates`.
        //
        // QML-facing identity: `unique_name` (slug) replaces the old
        // i64 preset_id. The slug is derived from `display_name`
        // (case-folded, alnum + underscore, max 64 chars).

        /// JSON array of `{ uniqueName, displayName, createdBy,
        /// createdAt, updatedAt, columnsJson }` for every template
        /// in the shared folder. Returns "[]" when the share is
        /// unreachable or no templates have been saved.
        #[qinvokable]
        fn get_column_presets(self: &Columns) -> QString;

        /// Save the column at `(job_path, folder, column_id)` as a
        /// template under `display_name`. Returns the template's
        /// unique_name, "" on error, or "__name_collision__" when
        /// `display_name` matches a different existing template
        /// (UI is expected to surface "edit existing instead?").
        #[qinvokable]
        fn save_column_as_preset(
            self: &Columns,
            display_name: QString,
            column_id: i64,
            job_path: QString,
            folder_name: QString,
        ) -> QString;

        /// Look up `unique_name` in `<farm_root>/ufb/templates/`,
        /// read the template, and add its column to `(job_path,
        /// folder)`. Returns the new column id, or -1.
        #[qinvokable]
        fn apply_preset(
            self: &Columns,
            preset_unique_name: QString,
            job_path: QString,
            folder_name: QString,
        ) -> i64;

        /// Delete a template from the shared folder. Returns true
        /// on success (idempotent — missing file is treated as a
        /// successful delete).
        #[qinvokable]
        fn delete_column_preset(self: &Columns, preset_unique_name: QString) -> bool;
    }

    // Lets the background reconcile task (spawn_blocking) marshal an
    // options_rev bump back onto the Qt/GUI thread on completion.
    impl cxx_qt::Threading for Columns {}
}

#[derive(Default)]
pub struct ColumnsRust {
    /// Backing field for the `columns_rev` qproperty. Bumped by
    /// `invalidate`; observed by the panels to refresh column layout.
    columns_rev: i32,
    /// Backing field for `options_rev`. Bumped by the background
    /// reconcile task when it writes new option rows.
    options_rev: i32,
}

/// Shared, cache-warm ColumnConfigManager. `pub(crate)` so the
/// subscription binding can translate metadata blob keys (column name ↔
/// stable column_uuid) at the QML boundary — see Increment C.
pub(crate) fn shared_manager() -> Option<Arc<ColumnConfigManager>> {
    static MANAGER: OnceLock<Option<Arc<ColumnConfigManager>>> = OnceLock::new();
    MANAGER
        .get_or_init(|| {
            let mgr = shared_db().map(|db| Arc::new(ColumnConfigManager::new(db)));
            // First-touch migration: seed the per-user
            // column_layout_personal table from existing
            // column_definitions so the upgrade preserves each
            // user's current view. Idempotent — re-running picks
            // up new columns the user hasn't customised yet but
            // never overwrites their personal choices.
            // Run the first-touch seed on the blocking pool, not inline:
            // shared_manager() is first hit from get_column_defs on the
            // GUI thread, and seeding is a DB write that would otherwise
            // block the UI (and contend with the snapshot worker's WAL
            // writer lock) on the cold-start critical path. It's
            // idempotent and only feeds the personal-layout table, so
            // letting it land a beat later is fine.
            if let Some(m) = mgr.as_ref() {
                let m = m.clone();
                let _g = crate::runtime::shared_runtime().enter();
                tokio::task::spawn_blocking(move || {
                    match m.seed_personal_layout_from_definitions() {
                        Ok(n) if n > 0 => log::info!(
                            "columns: seeded {} rows into column_layout_personal", n
                        ),
                        Ok(_) => {}
                        Err(e) => log::warn!(
                            "columns: seed_personal_layout_from_definitions failed: {}", e
                        ),
                    }
                });
            }
            mgr
        })
        .clone()
}

impl qobject::Columns {
    fn get_column_defs(
        self: &qobject::Columns,
        job_path: cxx_qt_lib::QString,
        folder: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let Some(mgr) = shared_manager() else {
            return cxx_qt_lib::QString::from("[]");
        };
        let job_s = job_path.to_string();
        let folder_s = folder.to_string();
        match mgr.get_column_defs(&job_s, &folder_s) {
            Ok(defs) => {
                let json = serde_json::to_string(&defs).unwrap_or_else(|_| "[]".into());
                cxx_qt_lib::QString::from(&json)
            }
            Err(e) => {
                log::warn!(
                    "columns: get_column_defs({}, {}) failed: {}",
                    job_s, folder_s, e
                );
                cxx_qt_lib::QString::from("[]")
            }
        }
    }

    fn invalidate(
        mut self: core::pin::Pin<&mut qobject::Columns>,
        _job_path: cxx_qt_lib::QString,
        _folder: cxx_qt_lib::QString,
    ) {
        // ColumnConfigManager invalidates per-key inside its own
        // CRUD; this is the explicit "drop everything" hook for
        // testing / forced refresh. Per-key invalidation hooks would
        // need a public per-key API on the manager — not worth it
        // until we hit a perf reason.
        if let Some(mgr) = shared_manager() {
            mgr.invalidate_all_caches();
        }
        // Notify observers regardless of cache state: this is also the
        // reactivity signal that column definitions changed (see the
        // `columns_rev` note in the bridge). Bumped even when the manager
        // is unavailable so a forced refresh still nudges the UI.
        let rev = *self.as_ref().columns_rev();
        self.as_mut().set_columns_rev(rev.wrapping_add(1));
    }

    fn add_column(
        self: &qobject::Columns,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        column_name: cxx_qt_lib::QString,
        column_type: cxx_qt_lib::QString,
        default_value: cxx_qt_lib::QString,
        is_visible: bool,
        column_order: i32,
        column_width: f64,
    ) -> i64 {
        add_column_inner(
            job_path,
            folder_name,
            column_name,
            column_type,
            default_value,
            is_visible,
            column_order,
            column_width,
            false, // reuse existing template if a match exists
        )
    }

    fn add_column_force_new(
        self: &qobject::Columns,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        column_name: cxx_qt_lib::QString,
        column_type: cxx_qt_lib::QString,
        default_value: cxx_qt_lib::QString,
        is_visible: bool,
        column_order: i32,
        column_width: f64,
    ) -> i64 {
        add_column_inner(
            job_path,
            folder_name,
            column_name,
            column_type,
            default_value,
            is_visible,
            column_order,
            column_width,
            true, // mint a fresh template even if a match exists
        )
    }

    fn duplicate_column(
        self: &qobject::Columns,
        source_id: i64,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        new_name: cxx_qt_lib::QString,
        copy_metadata: bool,
        delete_source: bool,
    ) -> i64 {
        let Some(mgr) = shared_manager() else { return -1; };
        let job_s = to_storage(&job_path.to_string());
        let folder_s = folder_name.to_string();
        let new_name_s = new_name.to_string();
        if new_name_s.trim().is_empty() {
            log::warn!("columns: duplicate_column rejected — empty new_name");
            return -1;
        }

        // Source column lookup — we need its column_type, options,
        // default_value, and column_name to seed the new column +
        // drive the metadata copy.
        let Some(source) = mgr
            .get_column_defs(&job_s, &folder_s)
            .ok()
            .and_then(|defs| defs.into_iter().find(|d| d.id == Some(source_id)))
        else {
            log::warn!(
                "columns: duplicate_column source {} not found in {}/{}",
                source_id, job_s, folder_s
            );
            return -1;
        };

        // Reject duplication onto the same name as the source — the
        // intent of duplicate is to rename, and we don't want to
        // accidentally produce two columns with the same display
        // name in the same folder (item_metadata would also collide).
        if new_name_s.trim().to_lowercase()
            == source.column_name.trim().to_lowercase()
        {
            log::warn!(
                "columns: duplicate_column rejected — new_name matches source column name"
            );
            return -1;
        }

        // Pick the next column_order so the new column lands after
        // existing ones rather than overlapping orders.
        let next_order = mgr
            .get_column_defs(&job_s, &folder_s)
            .ok()
            .and_then(|defs| defs.iter().map(|d| d.column_order).max())
            .map(|m| m + 1)
            .unwrap_or(0);

        // Fork: add_column_force_new path mints a fresh template
        // hash (not reusing any existing same-name match in the
        // project's namespace).
        let new_id = add_column_inner(
            cxx_qt_lib::QString::from(&job_s),
            cxx_qt_lib::QString::from(&folder_s),
            cxx_qt_lib::QString::from(&new_name_s),
            cxx_qt_lib::QString::from(&source.column_type),
            cxx_qt_lib::QString::from(
                source.default_value.as_deref().unwrap_or(""),
            ),
            true, // is_visible
            next_order,
            source.column_width,
            true, // force_new — fork rather than reuse
        );
        if new_id < 0 {
            log::warn!("columns: duplicate_column add_column_inner failed");
            return -1;
        }

        // Seed the new column's options from the source. Build a
        // ColumnDefinition and call update_column directly — that
        // updates the column_definitions row + replaces options
        // atomically, then auto_promote_on_update CAS-saves to
        // the freshly-minted template.
        if !source.options.is_empty() {
            let new_def = ColumnDefinition {
                id: Some(new_id),
                job_path: job_s.clone(),
                folder_name: folder_s.clone(),
                column_name: new_name_s.clone(),
                column_type: source.column_type.clone(),
                column_order: next_order,
                column_width: source.column_width,
                is_visible: true,
                default_value: source.default_value.clone(),
                options: source.options.clone(),
                modified_time: 0,
                deleted_at: None,
                template_hash: None,
                trashed_at: None,
                column_uuid: None, // manager derives + stores on write
            };
            if let Err(e) = mgr.update_column(&new_def) {
                log::warn!(
                    "columns: duplicate_column failed to seed options on new column {}: {}",
                    new_id, e
                );
            } else {
                auto_promote_on_update(&mgr, new_id, &new_def);
            }
        }

        // Optional metadata copy — for every item in this folder
        // whose metadata blob has the source column's name as a
        // key, add the new column's name with the same value.
        // SQLite's json_set + json_extract handles the JSON
        // transformation in one UPDATE.
        //
        // Uses named params (`:dst`, `:src`, …) instead of `?N`
        // numbered placeholders because the source key path is
        // referenced twice (json_extract in SET and again in WHERE)
        // and named binding makes the reuse explicit. The earlier
        // numbered version silently matched 0 rows on some Windows
        // builds — symptom looked like "duplicate option doesn't
        // work."
        if copy_metadata {
            let Some(db) = shared_db() else { return new_id };
            // v5: cell values are keyed by column_uuid. Copy from the
            // source column's uuid to the new column's uuid (the same
            // deterministic value add_column_inner just stored for it).
            // Fall back to the source name only if it somehow lacks a uuid.
            let src_uuid = source
                .column_uuid
                .clone()
                .unwrap_or_else(|| source.column_name.clone());
            let dst_uuid = ufb_core::columns::derive_column_uuid(
                &job_s,
                &folder_s,
                &new_name_s,
                &source.column_type,
            );
            let src_key_path = format!("${}", json_escape_key(&src_uuid));
            let dst_key_path = format!("${}", json_escape_key(&dst_uuid));

            // Diagnostic: how many candidate rows COULD copy? Helps
            // distinguish "WHERE matched none" from "json_extract
            // returned NULL for every match". Cheap (one COUNT, same
            // index path the UPDATE walks).
            let candidates: i64 = db
                .with_conn(|conn| {
                    conn.query_row(
                        "SELECT COUNT(*) FROM item_metadata
                         WHERE job_path = :job
                           AND folder_name = :folder
                           AND deleted_at IS NULL",
                        rusqlite::named_params! {
                            ":job": &job_s,
                            ":folder": &folder_s,
                        },
                        |row| row.get(0),
                    )
                })
                .unwrap_or(0);
            let with_src_value: i64 = db
                .with_conn(|conn| {
                    conn.query_row(
                        "SELECT COUNT(*) FROM item_metadata
                         WHERE job_path = :job
                           AND folder_name = :folder
                           AND deleted_at IS NULL
                           AND json_extract(metadata_json, :src) IS NOT NULL",
                        rusqlite::named_params! {
                            ":job": &job_s,
                            ":folder": &folder_s,
                            ":src": &src_key_path,
                        },
                        |row| row.get(0),
                    )
                })
                .unwrap_or(0);
            log::info!(
                "columns: duplicate_column metadata copy pre-flight: \
                 job={} folder={} candidates={} with_src_value={} src_key={} dst_key={}",
                job_s, folder_s, candidates, with_src_value, src_key_path, dst_key_path
            );

            let copied = match db.with_conn(|conn| {
                conn.execute(
                    "UPDATE item_metadata
                     SET metadata_json = json_set(
                             metadata_json,
                             :dst,
                             json_extract(metadata_json, :src)),
                         modified_time = :now
                     WHERE job_path = :job
                       AND folder_name = :folder
                       AND deleted_at IS NULL
                       AND json_extract(metadata_json, :src) IS NOT NULL",
                    rusqlite::named_params! {
                        ":dst": &dst_key_path,
                        ":src": &src_key_path,
                        ":job": &job_s,
                        ":folder": &folder_s,
                        ":now": ufb_core::utils::current_time_ms(),
                    },
                )
            }) {
                Ok(n) => n,
                Err(e) => {
                    log::warn!(
                        "columns: duplicate_column metadata copy failed: {}",
                        e
                    );
                    0
                }
            };
            log::info!(
                "columns: duplicate_column copied {} item metadata values from `{}` to `{}` (pre-flight: {} of {} items had a source value)",
                copied, source.column_name, new_name_s, with_src_value, candidates
            );
        }

        // Optional source delete — sends to Trash (recoverable) so a
        // mis-clicked "delete source" during duplicate isn't terminal.
        // 0.9.97: replaces the old `delete_column` (col_delete) path.
        if delete_source {
            let def_before = mgr.get_column_by_id(source_id);
            match mgr.trash_column(source_id) {
                Ok(key) => {
                    if let (true, Some(mut def)) = (is_shared_folder(&key.0), def_before) {
                        let now = ufb_core::utils::current_time_ms();
                        def.trashed_at = Some(now);
                        def.modified_time = now;
                        let def_str = serde_json::to_string(&def)
                            .unwrap_or_else(|_| "{}".to_string());
                        let change = serde_json::json!({
                            "action": "col_update",
                            "def": def_str,
                            "modified_time": now,
                        })
                        .to_string();
                        super::mesh::broadcast_table_change(change);
                    }
                }
                Err(e) => {
                    log::warn!(
                        "columns: duplicate_column source trash failed: {}",
                        e
                    );
                }
            }
        }

        new_id
    }

    fn count_metadata_values(
        self: &qobject::Columns,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        column_name: cxx_qt_lib::QString,
    ) -> i32 {
        let Some(db) = shared_db() else { return 0; };
        let job_s = to_storage(&job_path.to_string());
        let folder_s = folder_name.to_string();
        let column_s = column_name.to_string();
        if column_s.trim().is_empty() {
            return 0;
        }
        // v5: cell values are keyed by column_uuid — resolve the name to
        // its uuid and count under that. No such column → no values.
        let Some(uuid) = shared_manager()
            .and_then(|cm| cm.column_uuid_for_name(&job_s, &folder_s, &column_s))
        else {
            return 0;
        };
        let key_path = format!("${}", json_escape_key(&uuid));
        match db.with_conn(|conn| {
            conn.query_row(
                "SELECT COUNT(*) FROM item_metadata
                 WHERE job_path = ?1
                   AND folder_name = ?2
                   AND deleted_at IS NULL
                   AND json_extract(metadata_json, ?3) IS NOT NULL",
                [&job_s, &folder_s, &key_path],
                |row| row.get::<_, i64>(0),
            )
        }) {
            Ok(n) => n as i32,
            Err(e) => {
                log::warn!("columns: count_metadata_values failed: {}", e);
                0
            }
        }
    }

    fn template_usage_count(
        self: &qobject::Columns,
        template_hash: cxx_qt_lib::QString,
    ) -> i32 {
        let hash = template_hash.to_string();
        if hash.is_empty() {
            return 0;
        }
        let Some(db) = shared_db() else { return 0; };
        match db.with_conn(|conn| {
            conn.query_row(
                "SELECT COUNT(*) FROM column_definitions
                 WHERE template_hash = ?1 AND deleted_at IS NULL",
                [hash.as_str()],
                |row| row.get::<_, i64>(0),
            )
        }) {
            Ok(n) => n as i32,
            Err(e) => {
                log::warn!("columns: template_usage_count failed: {}", e);
                0
            }
        }
    }

    fn refresh_templates(self: &qobject::Columns) {
        let cache = templates_v2_cache_dir();
        let Some(remote) = templates_v2_dir() else {
            log::info!(
                "columns: refresh_templates skipped — share unavailable; \
                 lookup will fall back to local cache."
            );
            return;
        };
        // Phase 7 — once-per-machine backfill of template_hash on
        // pre-existing column_definitions rows. Runs before the
        // pull so the manifest the pull reads back has any newly-
        // minted templates from the migration.
        run_v2_inline_schema_migration_once(&remote);

        // Resolving the remote dir already runs the misplaced-file
        // recovery. Now diff manifests and pull any newer template
        // files from the share into the local mirror.
        match ufb_core::templates::v2::pull_to_local_cache(&remote, &cache) {
            Ok(n) => {
                log::info!(
                    "columns: refresh_templates pulled {} template file(s) into local cache",
                    n
                );
            }
            Err(e) => {
                log::warn!("columns: refresh_templates pull failed: {}", e);
            }
        }
    }

    fn lookup_template(
        self: &qobject::Columns,
        common_name: cxx_qt_lib::QString,
        column_type: cxx_qt_lib::QString,
        job_path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let s = AppSettings::load();
        let project_slug = project_slug_for(&job_path.to_string(), &s.mesh_sync.farm_path);
        let common_slug = ufb_core::templates::slug_for(&common_name.to_string());
        let column_type_s = column_type.to_string();

        // Try the share first; fall back to the local cache if the
        // share is unreachable or its manifest read fails. Either
        // way the user sees a stable view of the latest templates
        // they've successfully pulled.
        let (lookup_dir, manifest) = match templates_v2_dir() {
            Some(remote) => match ufb_core::templates::v2::read_manifest(&remote) {
                Ok(m) => (remote, m),
                Err(e) => {
                    log::warn!(
                        "columns: lookup_template remote manifest read failed: {} — falling back to local cache",
                        e
                    );
                    let cache = templates_v2_cache_dir();
                    match ufb_core::templates::v2::read_manifest(&cache) {
                        Ok(m) => (cache, m),
                        Err(_) => return cxx_qt_lib::QString::from("[]"),
                    }
                }
            },
            None => {
                let cache = templates_v2_cache_dir();
                match ufb_core::templates::v2::read_manifest(&cache) {
                    Ok(m) => (cache, m),
                    Err(_) => return cxx_qt_lib::QString::from("[]"),
                }
            }
        };

        let matches: Vec<serde_json::Value> = manifest
            .templates
            .iter()
            .filter(|t| {
                t.project_slug == project_slug
                    && t.common_name_slug == common_slug
                    && t.column_type == column_type_s
            })
            .map(|t| {
                serde_json::json!({
                    "uniqueName": t.uuid,
                    "displayName": t.display_name,
                    "columnType": t.column_type,
                    // The manifest doesn't store created_by — that
                    // lives in the template file itself. Read it
                    // lazily so the lookup can stay manifest-only
                    // for the common case (uncommon: 0 matches).
                    "createdBy": ufb_core::templates::v2::read_template(&lookup_dir, &t.uuid)
                        .map(|full| full.created_by)
                        .unwrap_or_default(),
                    "updatedAt": t.updated_at,
                    "lastUsedAt": t.last_used_at,
                    "revision": t.revision,
                })
            })
            .collect();

        let json = serde_json::to_string(&matches).unwrap_or_else(|_| "[]".into());
        cxx_qt_lib::QString::from(&json)
    }

    fn update_column(
        self: &qobject::Columns,
        id: i64,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        column_name: cxx_qt_lib::QString,
        column_type: cxx_qt_lib::QString,
        default_value: cxx_qt_lib::QString,
        is_visible: bool,
        column_order: i32,
        column_width: f64,
    ) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        let job_s = job_path.to_string();
        let folder_s = folder_name.to_string();
        // Preserve existing options across updates — the basic-Edit
        // dialog doesn't touch them, but core::update_column treats
        // an empty options Vec as "delete all options".
        let existing_options = mgr
            .get_column_defs(&job_s, &folder_s)
            .ok()
            .and_then(|defs| defs.into_iter().find(|d| d.id == Some(id)))
            .map(|d| d.options)
            .unwrap_or_default();
        let def = ColumnDefinition {
            id: Some(id),
            job_path: job_s,
            folder_name: folder_s,
            column_name: column_name.to_string(),
            column_type: column_type.to_string(),
            column_order,
            column_width,
            is_visible,
            default_value: optional_string(&default_value),
            options: existing_options,
            modified_time: 0,
            deleted_at: None,
            template_hash: None, // preserved by SQL UPDATE — not touched here
            trashed_at: None,    // preserved by SQL UPDATE — not touched here
            column_uuid: None,   // preserved by SQL UPDATE — not touched here
        };
        if let Err(e) = mgr.update_column(&def) {
            log::warn!("columns: update_column({}) failed: {}", id, e);
            return false;
        }
        if is_shared_folder(&def.job_path) {
            auto_promote_on_update(&mgr, id, &def);
            broadcast_col_change("col_update", &mgr, &def.job_path, &def.folder_name, id);
        }
        true
    }

    /// 0.9.97 — user-facing "Delete column" sends to Trash. Sets
    /// `trashed_at` locally and broadcasts as `col_update` (carrying
    /// the trashed_at field) so every peer trashes too. Recoverable
    /// from the Trash dialog via `untrash_column`. Permanent removal
    /// is the separate `permanently_delete_column` path that empty-
    /// trash invokes.
    fn delete_column(self: &qobject::Columns, id: i64) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        // Snapshot the full def BEFORE trashing so the broadcast carries
        // a complete payload (col_update receivers do a full replace).
        let def_before = match mgr.get_column_by_id(id) {
            Some(d) => d,
            None => {
                log::warn!("columns: delete_column({}) — no row to trash", id);
                return false;
            }
        };
        let key = match mgr.trash_column(id) {
            Ok(key) => key,
            Err(e) => {
                log::warn!("columns: trash_column({}) failed: {}", id, e);
                return false;
            }
        };
        if is_shared_folder(&key.0) {
            let now = ufb_core::utils::current_time_ms();
            let mut def = def_before;
            def.trashed_at = Some(now);
            def.modified_time = now;
            let def_str = serde_json::to_string(&def).unwrap_or_else(|_| "{}".to_string());
            let change = serde_json::json!({
                "action": "col_update",
                "def": def_str,
                "modified_time": now,
            })
            .to_string();
            super::mesh::broadcast_table_change(change);
        }
        true
    }

    fn untrash_column(
        self: &qobject::Columns,
        id: i64,
        new_column_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let Some(mgr) = shared_manager() else {
            return cxx_qt_lib::QString::from("Error: column manager unavailable");
        };
        let new_name = new_column_name.to_string();
        let new_name_opt = if new_name.trim().is_empty() {
            None
        } else {
            Some(new_name.trim())
        };
        match mgr.untrash_column(id, new_name_opt) {
            Ok(key) => {
                if is_shared_folder(&key.0) {
                    if let Some(def) = mgr.get_column_by_id(id) {
                        let now = ufb_core::utils::current_time_ms();
                        let def_str = serde_json::to_string(&def)
                            .unwrap_or_else(|_| "{}".to_string());
                        let change = serde_json::json!({
                            "action": "col_update",
                            "def": def_str,
                            "modified_time": now,
                        })
                        .to_string();
                        super::mesh::broadcast_table_change(change);
                    }
                }
                cxx_qt_lib::QString::from("ok")
            }
            Err(e) => {
                if e.starts_with("RenameRequired") {
                    cxx_qt_lib::QString::from(&e)
                } else {
                    cxx_qt_lib::QString::from(&format!("Error: {}", e))
                }
            }
        }
    }

    fn unpromote_column(self: &qobject::Columns, id: i64) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        let def_before = mgr.get_column_by_id(id);
        let key = match mgr.unpromote_column(id) {
            Ok(key) => key,
            Err(e) => {
                log::warn!("columns: unpromote_column({}) failed: {}", id, e);
                return false;
            }
        };
        if let (true, Some(mut def)) = (is_shared_folder(&key.0), def_before) {
            let now = ufb_core::utils::current_time_ms();
            def.template_hash = None;
            def.modified_time = now;
            let def_str = serde_json::to_string(&def).unwrap_or_else(|_| "{}".to_string());
            let change = serde_json::json!({
                "action": "col_update",
                "def": def_str,
                "modified_time": now,
            })
            .to_string();
            super::mesh::broadcast_table_change(change);
        }
        true
    }

    fn promote_column(self: &qobject::Columns, id: i64) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        let Some(def) = mgr.get_column_by_id(id) else {
            log::warn!("columns: promote_column({}) — no row", id);
            return false;
        };
        // Re-uses the same flow new columns hit on add: lookup_or_mint
        // against (common_name_slug, column_type, project_slug). On
        // success the column gets a template_hash + options sync; on
        // failure (share unreachable) leaves the column unpromoted
        // and logs.
        auto_promote_on_add(&mgr, id, &def, /* force_new */ false);
        // Broadcast the post-promotion state. Even when promotion
        // failed, broadcasting the row is a no-op for peers — but on
        // success peers receive the new template_hash and the
        // receive-side fetch populates their column_options.
        if is_shared_folder(&def.job_path) {
            broadcast_col_change("col_update", &mgr, &def.job_path, &def.folder_name, id);
        }
        true
    }

    fn permanently_delete_column(self: &qobject::Columns, id: i64) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        // Capture the column's stable uuid BEFORE deleting — cell values
        // are stored uuid-keyed (v5), so the scrub below must remove the
        // uuid key. Falls back to the display name if somehow absent.
        let col_uuid = mgr.get_column_by_id(id).and_then(|d| d.column_uuid);
        let key = match mgr.permanently_delete_column(id) {
            Ok(key) => key,
            Err(e) => {
                log::warn!("columns: permanently_delete_column({}) failed: {}", id, e);
                return false;
            }
        };
        // 0.9.97 — strip the now-orphan metadata keys from
        // `item_metadata` for items in this folder. Without this the
        // discovered-keys pass in the Column Manager re-surfaces the
        // column as italic "(discovered)" forever after the user
        // empty-trashed it. Trash (recoverable) preserves metadata so
        // an Untrash can restore values; permanent delete is the
        // explicit "scrub it" path.
        if let Some(db) = shared_db() {
            let scrub_key = col_uuid.clone().unwrap_or_else(|| key.2.clone());
            let key_path = format!("${}", json_escape_key(&scrub_key));
            let now = ufb_core::utils::current_time_ms();
            let removed = db
                .with_conn(|conn| {
                    conn.execute(
                        "UPDATE item_metadata
                            SET metadata_json = json_remove(metadata_json, :k),
                                modified_time = :now
                          WHERE job_path = :job
                            AND folder_name = :folder
                            AND deleted_at IS NULL
                            AND json_extract(metadata_json, :k) IS NOT NULL",
                        rusqlite::named_params! {
                            ":k": &key_path,
                            ":now": now,
                            ":job": &key.0,
                            ":folder": &key.1,
                        },
                    )
                })
                .unwrap_or(0);
            if removed > 0 {
                log::info!(
                    "columns: permanently_delete_column scrubbed `{}` from {} item_metadata rows in {}/{}",
                    key.2, removed, key.0, key.1
                );
            }
        }
        if is_shared_folder(&key.0) {
            let now = ufb_core::utils::current_time_ms();
            let change = serde_json::json!({
                "action": "col_delete",
                "job_path": key.0,
                "folder_name": key.1,
                "column_name": key.2,
                // v5: carry the stable uuid so the delete lands on the
                // right row even on a peer that renamed the column.
                "column_uuid": col_uuid,
                "modified_time": now,
                "deleted_at": now,
            })
            .to_string();
            super::mesh::broadcast_table_change(change);
        }
        true
    }

    fn list_trashed_columns(
        self: &qobject::Columns,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let Some(mgr) = shared_manager() else {
            return cxx_qt_lib::QString::from("[]");
        };
        let job_s = job_path.to_string();
        let folder_s = folder_name.to_string();
        let defs = match mgr.list_trashed(&job_s, &folder_s) {
            Ok(v) => v,
            Err(e) => {
                log::warn!("columns: list_trashed_columns failed: {}", e);
                return cxx_qt_lib::QString::from("[]");
            }
        };
        let arr: Vec<serde_json::Value> = defs
            .into_iter()
            .map(|d| {
                serde_json::json!({
                    "id": d.id.unwrap_or(0),
                    "columnName": d.column_name,
                    "columnType": d.column_type,
                    "trashedAt": d.trashed_at.unwrap_or(0),
                    "templateHash": d.template_hash,
                })
            })
            .collect();
        cxx_qt_lib::QString::from(
            &serde_json::to_string(&arr).unwrap_or_else(|_| "[]".to_string()),
        )
    }

    fn set_column_visible(
        self: &qobject::Columns,
        id: i64,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        is_visible: bool,
    ) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        let job_s = job_path.to_string();
        let folder_s = folder_name.to_string();
        let Some(def) = mgr
            .get_column_defs(&job_s, &folder_s)
            .ok()
            .and_then(|defs| defs.into_iter().find(|d| d.id == Some(id)))
        else {
            log::warn!("columns: set_column_visible({}) — column not found", id);
            return false;
        };
        // Per-user overlay only — never broadcast. The team-shared
        // column_definitions row keeps its original visibility as
        // the default for new peers; this user's preference lives
        // in column_layout_personal.
        if let Err(e) = mgr.set_column_layout_personal(
            &def.job_path,
            &def.folder_name,
            &def.column_name,
            None,
            None,
            Some(is_visible),
        ) {
            log::warn!("columns: set_column_visible({}) failed: {}", id, e);
            return false;
        }
        true
    }

    fn set_column_width(
        self: &qobject::Columns,
        id: i64,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        column_width: f64,
    ) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        let job_s = job_path.to_string();
        let folder_s = folder_name.to_string();
        let Some(def) = mgr
            .get_column_defs(&job_s, &folder_s)
            .ok()
            .and_then(|defs| defs.into_iter().find(|d| d.id == Some(id)))
        else {
            log::warn!("columns: set_column_width({}) — column not found", id);
            return false;
        };
        if let Err(e) = mgr.set_column_layout_personal(
            &def.job_path,
            &def.folder_name,
            &def.column_name,
            None,
            Some(column_width),
            None,
        ) {
            log::warn!("columns: set_column_width({}) failed: {}", id, e);
            return false;
        }
        true
    }

    fn set_column_order(
        self: &qobject::Columns,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        ids_json: cxx_qt_lib::QString,
    ) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        let job_s = job_path.to_string();
        let folder_s = folder_name.to_string();
        let ids_s = ids_json.to_string();

        let ids: Vec<i64> = match serde_json::from_str(&ids_s) {
            Ok(v) => v,
            Err(e) => {
                log::warn!("columns: set_column_order — bad ids_json: {}", e);
                return false;
            }
        };

        let defs = match mgr.get_column_defs(&job_s, &folder_s) {
            Ok(d) => d,
            Err(e) => {
                log::warn!("columns: set_column_order — get_column_defs failed: {}", e);
                return false;
            }
        };

        let mut all_ok = true;
        for (pos, id) in ids.iter().enumerate() {
            let Some(def) = defs.iter().find(|d| d.id == Some(*id)).cloned() else {
                log::warn!("columns: set_column_order — id {} not found", id);
                all_ok = false;
                continue;
            };
            let new_order = pos as i32;
            if def.column_order == new_order {
                continue;
            }
            // Per-user overlay only — never broadcast.
            if let Err(e) = mgr.set_column_layout_personal(
                &def.job_path,
                &def.folder_name,
                &def.column_name,
                Some(new_order),
                None,
                None,
            ) {
                log::warn!(
                    "columns: set_column_order — set_column_layout_personal({}) failed: {}",
                    id,
                    e
                );
                all_ok = false;
                continue;
            }
        }
        all_ok
    }

    fn set_column_options(
        self: &qobject::Columns,
        id: i64,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        options_json: cxx_qt_lib::QString,
    ) -> bool {
        let Some(mgr) = shared_manager() else { return false };
        let job_s = job_path.to_string();
        let folder_s = folder_name.to_string();
        let opts_s = options_json.to_string();

        // Parse incoming options. We accept the in-the-wild shapes
        // {name, color} and ignore extras (id / modified_time /
        // deleted_at) — those are managed Rust-side.
        let raw: Vec<serde_json::Value> = serde_json::from_str(&opts_s)
            .unwrap_or_default();
        let new_options: Vec<ufb_core::columns::ColumnOption> = raw
            .into_iter()
            .filter_map(|v| {
                let obj = v.as_object()?;
                let name = obj.get("name")?.as_str()?.to_string();
                if name.is_empty() { return None; }
                let color = obj.get("color")
                    .and_then(|c| c.as_str())
                    .map(|s| s.to_string())
                    .filter(|s| !s.is_empty());
                Some(ufb_core::columns::ColumnOption {
                    id: None,
                    name,
                    color,
                    modified_time: 0,
                    deleted_at: None,
                })
            })
            .collect();

        let Some(mut def) = mgr
            .get_column_defs(&job_s, &folder_s)
            .ok()
            .and_then(|defs| defs.into_iter().find(|d| d.id == Some(id)))
        else {
            log::warn!("columns: set_column_options({}) — column not found", id);
            return false;
        };
        def.options = new_options;
        if let Err(e) = mgr.update_column(&def) {
            log::warn!("columns: set_column_options({}) failed: {}", id, e);
            return false;
        }
        if is_shared_folder(&def.job_path) {
            // Options are part of the schema template — push the
            // change up to the shared file so other peers see it.
            auto_promote_on_update(&mgr, id, &def);
            broadcast_col_change("col_update", &mgr, &def.job_path, &def.folder_name, id);
        }
        true
    }

    // Fire-and-forget. The actual work — a NAS template pull plus
    // DELETE/INSERT of column_options — does network + DB I/O and must
    // NEVER run on the Qt/GUI thread (it was the cold-start freeze: each
    // open folder blocked the UI for a NAS round-trip + up to the 5s WAL
    // busy_timeout). It now runs on the blocking pool; on completion it
    // bumps `options_rev` so the panels re-render with the reconciled
    // options. Returns immediately (the old i32 count is unused by QML).
    fn reconcile_options_from_templates(
        self: &qobject::Columns,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
    ) -> i32 {
        use cxx_qt::Threading;
        let job = job_path.to_string();
        let folder = folder_name.to_string();

        // Coalesce: if a reconcile for this (job, folder) is already in
        // flight, drop this one. Stops the cold-start storm where every
        // restored job tab spawns its own overlapping NAS pull + write.
        {
            let mut inflight = reconcile_inflight().lock().unwrap();
            if !inflight.insert((job.clone(), folder.clone())) {
                return 0;
            }
        }

        let qt = self.qt_thread();
        let _g = crate::runtime::shared_runtime().enter();
        tokio::task::spawn_blocking(move || {
            if let Some(mgr) = shared_manager() {
                match mgr.reconcile_options_from_templates(&job, &folder) {
                    Ok(n) if n > 0 => {
                        let _ = qt.queue(|mut this| {
                            let next = this.as_ref().options_rev().wrapping_add(1);
                            this.as_mut().set_options_rev(next);
                        });
                    }
                    Ok(_) => {}
                    Err(e) => log::warn!(
                        "columns: reconcile_options_from_templates failed: {}", e
                    ),
                }
            }
            reconcile_inflight().lock().unwrap().remove(&(job, folder));
        });
        0
    }
}

/// In-flight (job_path, folder_name) reconciles — see
/// `reconcile_options_from_templates`. Coalesces duplicate requests so a
/// burst of folder opens doesn't fan out into N overlapping NAS pulls.
fn reconcile_inflight() -> &'static std::sync::Mutex<std::collections::HashSet<(String, String)>>
{
    static INFLIGHT: OnceLock<std::sync::Mutex<std::collections::HashSet<(String, String)>>> =
        OnceLock::new();
    INFLIGHT.get_or_init(|| std::sync::Mutex::new(std::collections::HashSet::new()))
}

/// Empty-string → None for default_value (db nullable column).
fn optional_string(s: &cxx_qt_lib::QString) -> Option<String> {
    let s = s.to_string();
    if s.is_empty() { None } else { Some(s) }
}

/// Wrap a column-name string for SQLite's JSON path syntax. Names
/// with spaces / special chars need bracket-quoted form
/// (`["foo bar"]`) so json_extract doesn't choke. Bare alnum keys
/// can use dotted form but the bracket form works for both, so we
/// always use it.
fn json_escape_key(name: &str) -> String {
    // Escape internal `"` to keep the JSON path literal valid.
    let escaped = name.replace('"', "\\\"");
    format!("[\"{}\"]", escaped)
}

/// Shared core for `add_column` and `add_column_force_new`. The
/// only difference between the two surfaces is the `force_new`
/// flag — both run the same SQL insert + auto-promote pipeline.
fn add_column_inner(
    job_path: cxx_qt_lib::QString,
    folder_name: cxx_qt_lib::QString,
    column_name: cxx_qt_lib::QString,
    column_type: cxx_qt_lib::QString,
    default_value: cxx_qt_lib::QString,
    is_visible: bool,
    column_order: i32,
    column_width: f64,
    force_new: bool,
) -> i64 {
    let Some(mgr) = shared_manager() else { return -1; };
    let def = ColumnDefinition {
        id: None,
        job_path: job_path.to_string(),
        folder_name: folder_name.to_string(),
        column_name: column_name.to_string(),
        column_type: column_type.to_string(),
        column_order,
        column_width,
        is_visible,
        default_value: optional_string(&default_value),
        options: vec![],
        modified_time: 0,
        deleted_at: None,
        template_hash: None, // populated post-insert by auto_promote_on_add
        trashed_at: None,
        column_uuid: None, // manager derives + stores on the INSERT path
    };
    let id = match mgr.add_column(&def) {
        Ok(id) => id,
        Err(e) => {
            log::warn!("columns: add_column failed: {}", e);
            return -1;
        }
    };
    // Auto-promote + mesh broadcast are gated on the folder being a
    // "shared" location (subscribed job, project-flagged bookmark,
    // or jobs-folder mount). Local-only folders skip both — the
    // column row stays in the SQLite DB but doesn't propagate.
    if is_shared_folder(&def.job_path) {
        auto_promote_on_add(&mgr, id, &def, force_new);
        broadcast_col_change("col_add", &mgr, &def.job_path, &def.folder_name, id);
    } else {
        log::info!(
            "columns: add_column local-only ({}/{} not in a shared folder)",
            def.job_path, def.folder_name
        );
    }
    id
}

/// On column add: lookup-or-mint a v2 template for `(project,
/// common_slug, type)` and write `column_definitions.template_hash`
/// to point at it. `force_new = true` mints a fresh hash even when
/// a matching template exists — used by the dialog's "Create new"
/// branch to fork. Best-effort: template-write failures log a
/// warning but the column row stays in place (a future edit will
/// retry the template promotion).
fn auto_promote_on_add(
    mgr: &Arc<ColumnConfigManager>,
    column_id: i64,
    def: &ColumnDefinition,
    force_new: bool,
) {
    let Some(dir) = templates_v2_dir() else { return };
    let s = AppSettings::load();
    let project_slug = project_slug_for(&def.job_path, &s.mesh_sync.farm_path);
    let common_slug = ufb_core::templates::slug_for(&def.column_name);
    let now = ufb_core::utils::current_time_ms();
    match ufb_core::templates::v2::lookup_or_mint_detailed(
        &dir,
        &def.column_name,
        &common_slug,
        &project_slug,
        &def.column_type,
        def.default_value.clone(),
        def.options.clone(),
        now,
        force_new,
    ) {
        Ok((template, was_minted)) => {
            if let Err(e) = mgr.set_template_hash(column_id, Some(&template.template_hash)) {
                log::warn!("columns: set_template_hash failed: {}", e);
            }
            // "Use it" / reuse path: the new column's column_options
            // table is empty (add_column_inner inserted with no
            // options) and the QML's later set_column_options call
            // will be suppressed when "Use it" was clicked. Populate
            // column_options from the existing shared template here
            // so the user's brand-new column shows the team's
            // dropdown options immediately. Skipped on the freshly-
            // minted path (was_minted=true) because there the user's
            // typed options ARE the canonical setup and will land
            // via the normal set_column_options flow.
            if !was_minted && !template.options.is_empty() {
                if let Err(e) =
                    mgr.set_options_for_column(column_id, &template.options)
                {
                    log::warn!(
                        "columns: auto-promote (reuse) options sync failed for {}: {}",
                        column_id, e
                    );
                }
            }
            mirror_to_local_cache(&dir);
        }
        Err(e) => {
            log::warn!(
                "columns: auto-promote (add) failed for column {} ({}/{}): {}",
                column_id, def.job_path, def.folder_name, e
            );
        }
    }
}

/// After a successful share write, pull the updated manifest +
/// changed templates into the local cache. Idempotent diff via
/// `pull_to_local_cache` — only the file we just wrote (whose
/// revision bumped) actually transfers. Best-effort.
fn mirror_to_local_cache(remote_dir: &std::path::Path) {
    let cache = templates_v2_cache_dir();
    if let Err(e) = ufb_core::templates::v2::pull_to_local_cache(remote_dir, &cache) {
        log::warn!("columns: mirror_to_local_cache failed: {}", e);
    }
}

/// On column update: if the column already references a v2 template
/// (template_hash set), push schema changes (default_value, options)
/// to that template via revision-CAS. If the column has no
/// template_hash yet (legacy or local-only-elevated), mint a fresh
/// template via `auto_promote_on_add`-style flow.
///
/// Concurrent-edit handling: a `RevisionMismatch` from the CAS save
/// just logs a warning here. Phase 4 wires the "reload?" dialog;
/// for now the local row stays correct (column_definitions has the
/// user's edit) and the template-side update is dropped.
fn auto_promote_on_update(
    mgr: &Arc<ColumnConfigManager>,
    column_id: i64,
    def: &ColumnDefinition,
) {
    let Some(dir) = templates_v2_dir() else { return };
    let s = AppSettings::load();
    let project_slug = project_slug_for(&def.job_path, &s.mesh_sync.farm_path);
    let common_slug = ufb_core::templates::slug_for(&def.column_name);
    let now = ufb_core::utils::current_time_ms();

    let existing_hash = mgr
        .get_template_hash(column_id)
        .ok()
        .flatten();

    if let Some(hash) = existing_hash {
        // Read current revision from disk so the CAS save knows
        // which version we're updating from.
        let expected_revision = match ufb_core::templates::v2::read_template(&dir, &hash) {
            Ok(t) => t.revision,
            Err(ufb_core::templates::TemplateError::NotFound(_)) => {
                // The share lost this template file (crashed/raced
                // writer) while columns still reference the hash.
                // Before this self-heal, every option edit silently
                // no-op'd here forever and folder-open reconciles
                // kept reverting the user's changes to the stale
                // cache. Rebuild the file — from the local cache
                // copy when one exists (its revision keeps peers'
                // pull diffs advancing), else from this column's own
                // definition — carrying the user's pending edit.
                let cache = templates_v2_cache_dir();
                let base = match ufb_core::templates::v2::read_template(&cache, &hash) {
                    Ok(mut cached) => {
                        cached.default_value = def.default_value.clone();
                        cached.options = def.options.clone();
                        cached
                    }
                    Err(_) => ufb_core::templates::v2::TemplateV2 {
                        schema_version: ufb_core::templates::v2::SCHEMA_VERSION,
                        template_hash: hash.clone(),
                        display_name: def.column_name.clone(),
                        common_name_slug: common_slug.clone(),
                        project_slug: project_slug.clone(),
                        column_type: def.column_type.clone(),
                        default_value: def.default_value.clone(),
                        options: def.options.clone(),
                        created_by: ufb_core::templates::current_user(),
                        created_at: now,
                        updated_at: now,
                        revision: 0, // restore bumps to 1
                    },
                };
                match ufb_core::templates::v2::restore_template(&dir, base, now) {
                    Ok(t) => {
                        log::info!(
                            "columns: auto-promote restored missing template {} (rev {}) from {}",
                            hash,
                            t.revision,
                            if t.revision > 1 { "local cache" } else { "column definition" }
                        );
                        mirror_to_local_cache(&dir);
                    }
                    Err(ufb_core::templates::TemplateError::RevisionMismatch { found, .. }) => {
                        // The file is back (transient share error, or a
                        // peer restored it first) — restore is create-
                        // only and touched nothing. Same outcome as the
                        // ordinary CAS conflict below: local SQL row
                        // stays correct, template untouched.
                        log::info!(
                            "columns: template {} reappeared at rev {} — restore skipped",
                            hash, found
                        );
                    }
                    Err(e) => log::warn!(
                        "columns: auto-promote restore of missing template {} failed: {}",
                        hash, e
                    ),
                }
                return;
            }
            Err(e) => {
                log::warn!(
                    "columns: auto-promote (update) read failed for {}: {}",
                    hash, e
                );
                return;
            }
        };
        match ufb_core::templates::v2::update_template_options(
            &dir,
            &hash,
            def.default_value.clone(),
            def.options.clone(),
            expected_revision,
            now,
        ) {
            Ok(_) => {
                mirror_to_local_cache(&dir);
            }
            Err(ufb_core::templates::TemplateError::RevisionMismatch { expected, found }) => {
                log::warn!(
                    "columns: auto-promote (update) revision mismatch for {} \
                     (expected {}, found {}) — template not updated, local SQL row \
                     still correct. Phase 4 wires the reload-conflict UI.",
                    hash, expected, found
                );
            }
            Err(e) => {
                log::warn!(
                    "columns: auto-promote (update) failed for {} ({}/{}): {}",
                    hash, def.job_path, def.folder_name, e
                );
            }
        }
    } else {
        // No template_hash yet — column was created on a non-shared
        // folder and just elevated, OR it's a legacy row pre-Phase-7.
        // Mint one now.
        match ufb_core::templates::v2::lookup_or_mint(
            &dir,
            &def.column_name,
            &common_slug,
            &project_slug,
            &def.column_type,
            def.default_value.clone(),
            def.options.clone(),
            now,
            false,
        ) {
            Ok(template) => {
                if let Err(e) = mgr.set_template_hash(column_id, Some(&template.template_hash)) {
                    log::warn!("columns: set_template_hash failed: {}", e);
                }
                mirror_to_local_cache(&dir);
            }
            Err(e) => {
                log::warn!(
                    "columns: auto-promote lazy-mint failed for {} ({}/{}): {}",
                    column_id, def.job_path, def.folder_name, e
                );
            }
        }
    }
}

/// Resolve the v2 templates directory `<farm_root>/ufb/templates/`.
/// Returns None when the farm root is unconfigured / unreachable —
/// callers treat that as "skip auto-promote, the SQL row + mesh
/// broadcast still landed."
fn templates_v2_dir() -> Option<std::path::PathBuf> {
    let s = AppSettings::load();
    let farm = s.mesh_sync.farm_path.clone();
    if farm.trim().is_empty() {
        return None;
    }
    match ufb_core::templates::v2::resolve_dir(&farm) {
        Ok(p) => Some(p),
        Err(e) => {
            log::warn!("columns: templates_v2_dir unreachable: {}", e);
            None
        }
    }
}

/// Resolve the local cache mirror of the v2 templates directory at
/// `<app_data_dir>/templates/`. Created on first call. Used as the
/// fallback read source when the share is briefly unreachable, and
/// as the destination of `pull_to_local_cache` calls.
///
/// Cache writes are best-effort — failing to mkdir or to mirror a
/// file just degrades to "no offline view" without breaking the
/// authoritative share write.
fn templates_v2_cache_dir() -> std::path::PathBuf {
    let dir = ufb_core::utils::get_app_data_dir().join("templates");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

/// Returns true when `(job_path)` is in a folder that should
/// auto-promote columns to shared templates. Definition: the folder
/// is a subscribed job, OR a descendant of a bookmarked
/// "is_project_folder" jobs folder, OR a descendant of a mount
/// flagged `is_jobs_folder`. Anything outside that set is local-
/// only — column writes don't broadcast and don't write template
/// files.
///
/// Local-only columns are how the user keeps "scratch" folders
/// (Desktop / random local dirs) from polluting the team's
/// templates library. The user can still elevate them with an
/// explicit "Save as Preset" action.
///
/// Path-identity note: subscriptions and bookmarks store paths as
/// tagged identities (`vol:…` / `native:…`) post-migration, or legacy
/// raw paths pre-migration. The input AND each stored path go through
/// `to_identity_storage` (idempotent — passes a tagged string through,
/// classifies a legacy one) so the comparison is era-agnostic. Mount
/// jobs-folder paths come from local mount config in native form, so
/// those still compare against the native input.

/// Canonicalise a QML-native path to tagged-identity storage form
/// (`vol:…` / `native:…`) for DB reads/writes. Idempotent.
fn to_storage(native: &str) -> String {
    let mappings = AppSettings::load().path_mappings;
    ufb_core::utils::to_identity_storage(native, &mappings)
}

fn is_shared_folder(job_path: &str) -> bool {
    if job_path.trim().is_empty() {
        return false;
    }
    let Some(db) = shared_db() else { return false; };
    let settings = AppSettings::load();

    let native_lower = job_path.trim_end_matches(['/', '\\']).to_lowercase();
    let job_id = ufb_core::utils::to_identity_storage(
        job_path,
        &settings.path_mappings,
    )
    .to_lowercase();

    // Subscribed jobs: stored canonical, compare canonical.
    let subscriptions: Vec<String> = match db.with_conn(|conn| {
        let mut stmt = conn.prepare(
            "SELECT job_path FROM subscriptions WHERE deleted_at IS NULL",
        )?;
        let rows = stmt.query_map([], |row| row.get::<_, String>(0))?;
        let mut out = Vec::new();
        for r in rows {
            out.push(r?);
        }
        Ok(out)
    }) {
        Ok(v) => v,
        Err(e) => {
            log::warn!("columns: is_shared subs query failed: {}", e);
            Vec::new()
        }
    };
    for sub in &subscriptions {
        let sub_id =
            ufb_core::utils::to_identity_storage(sub, &settings.path_mappings);
        if path_matches(&job_id, &sub_id) {
            return true;
        }
    }

    // Bookmarks flagged is_project_folder: stored canonical, compare
    // canonical. Note `bookmarks` table has no `deleted_at` column —
    // mark-and-sweep deletes happen elsewhere.
    let project_roots: Vec<String> = match db.with_conn(|conn| {
        let mut stmt = conn.prepare(
            "SELECT path FROM bookmarks WHERE is_project_folder = 1",
        )?;
        let rows = stmt.query_map([], |row| row.get::<_, String>(0))?;
        let mut out = Vec::new();
        for r in rows {
            out.push(r?);
        }
        Ok(out)
    }) {
        Ok(v) => v,
        Err(e) => {
            log::warn!("columns: is_shared bookmarks query failed: {}", e);
            Vec::new()
        }
    };
    for root in &project_roots {
        let root_id =
            ufb_core::utils::to_identity_storage(root, &settings.path_mappings);
        if path_matches(&job_id, &root_id) {
            return true;
        }
    }

    // Mount-config-flagged jobs folders: paths come from local mount
    // config in NATIVE form, so compare against native input.
    let mount_roots = super::mount::jobs_folder_volume_paths();
    for root in &mount_roots {
        if path_matches(&native_lower, root) {
            return true;
        }
    }

    false
}

/// Lowercase + trim-trailing-separator + descendant-or-equal
/// check. `needle_lower` is already lowercased + trimmed; `haystack`
/// gets the same treatment inside.
fn path_matches(needle_lower: &str, haystack: &str) -> bool {
    let h = haystack.trim_end_matches(['/', '\\']).to_lowercase();
    needle_lower == h
        || needle_lower.starts_with(&format!("{}/", h))
        || needle_lower.starts_with(&format!("{}\\", h))
}

/// Resolve `<farm_root>/ufb/templates/`. Returns None when the
/// share isn't reachable; callers should treat that as "no
/// templates available right now" rather than an error.
///
/// First call per process also triggers the v1 legacy-presets
/// migration (idempotent — gated on
/// `AppSettings.templates_migrated_v1`). The migration is best-
/// effort: failures log and the flag stays unset so we retry on
/// the next launch.
fn templates_dir() -> Option<std::path::PathBuf> {
    let s = AppSettings::load();
    let farm = s.mesh_sync.farm_path.clone();
    if farm.trim().is_empty() {
        return None;
    }
    let dir = match templates::resolve_dir(&farm) {
        Ok(p) => p,
        Err(e) => {
            log::warn!("columns: templates_dir unreachable: {}", e);
            return None;
        }
    };
    run_v1_migration_once(&farm);
    Some(dir)
}

/// One-shot backfill: scan `column_definitions` for rows with
/// `template_hash IS NULL`, assign each shared-folder row a v2
/// template_hash via lookup-or-mint. Rows in non-shared folders
/// (Desktop, scratch dirs) keep `template_hash = NULL` and remain
/// local-only. Idempotent + once-per-process; persistent gate via
/// `AppSettings::templates_v2_migrated`. Defers when the share is
/// unreachable so the next launch retries.
///
/// Grouping: rows sharing the same `(project_slug, common_slug,
/// column_type)` get the SAME template_hash. First row in each
/// group mints the template (with that row's options); subsequent
/// rows reuse the existing template via `lookup_or_mint`. Means a
/// project's "Status" column on five different folders ends up
/// referencing one shared template after migration, ready for any
/// of those five users to edit through to the team.
fn run_v2_inline_schema_migration_once(remote_dir: &std::path::Path) {
    static MIGRATED: OnceLock<()> = OnceLock::new();
    if MIGRATED.get().is_some() {
        return;
    }
    let s = AppSettings::load();
    if s.templates_v2_migrated {
        let _ = MIGRATED.set(());
        return;
    }
    let Some(mgr) = shared_manager() else { return };
    let Some(db) = shared_db() else { return };

    // Pull every NULL-template_hash row's identity. Options are
    // fetched per-row via mgr.get_column_defs to ride the existing
    // join with column_options.
    let rows: Vec<(i64, String, String, String, String, Option<String>)> = match db
        .with_conn(|conn| {
            let mut stmt = conn.prepare(
                "SELECT id, job_path, folder_name, column_name, column_type, default_value
                 FROM column_definitions
                 WHERE template_hash IS NULL AND deleted_at IS NULL",
            )?;
            let iter = stmt.query_map([], |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, String>(4)?,
                    row.get::<_, Option<String>>(5)?,
                ))
            })?;
            let mut out = Vec::new();
            for r in iter {
                out.push(r?);
            }
            Ok(out)
        }) {
        Ok(v) => v,
        Err(e) => {
            log::warn!("columns: v2 migration row query failed: {}", e);
            return;
        }
    };

    if rows.is_empty() {
        log::info!("columns: v2 migration — no rows to backfill");
        let mut s2 = AppSettings::load();
        s2.templates_v2_migrated = true;
        if let Err(e) = s2.save() {
            log::warn!("columns: failed to persist v2 migration flag: {}", e);
            return;
        }
        let _ = MIGRATED.set(());
        return;
    }

    let total = rows.len();
    let mut migrated = 0usize;
    let mut skipped_nonshared = 0usize;
    let mut failed = 0usize;
    let now = ufb_core::utils::current_time_ms();

    for (id, job_path, folder_name, column_name, column_type, default_value) in rows {
        if !is_shared_folder(&job_path) {
            skipped_nonshared += 1;
            continue;
        }
        // Pull the row's options via the manager so legacy
        // column_options join + caching work transparently.
        let options = match mgr
            .get_column_defs(&job_path, &folder_name)
            .ok()
            .and_then(|defs| defs.into_iter().find(|d| d.id == Some(id)))
            .map(|d| d.options)
        {
            Some(o) => o,
            None => {
                log::warn!(
                    "columns: v2 migration — couldn't read options for column {} ({}/{})",
                    id, job_path, folder_name
                );
                failed += 1;
                continue;
            }
        };
        let project_slug = project_slug_for(&job_path, &s.mesh_sync.farm_path);
        let common_slug = ufb_core::templates::slug_for(&column_name);
        match ufb_core::templates::v2::lookup_or_mint(
            remote_dir,
            &column_name,
            &common_slug,
            &project_slug,
            &column_type,
            default_value.clone(),
            options,
            now,
            false,
        ) {
            Ok(template) => {
                if let Err(e) = mgr.set_template_hash(id, Some(&template.template_hash)) {
                    log::warn!(
                        "columns: v2 migration set_template_hash failed for {}: {}",
                        id, e
                    );
                    failed += 1;
                    continue;
                }
                migrated += 1;
            }
            Err(e) => {
                log::warn!(
                    "columns: v2 migration lookup_or_mint failed for {} ({}/{}): {}",
                    id, job_path, folder_name, e
                );
                failed += 1;
            }
        }
    }

    log::info!(
        "columns: v2 migration complete — total={} migrated={} non_shared_skipped={} failed={}",
        total, migrated, skipped_nonshared, failed
    );

    // Flip the flag even when some rows failed — those rows stay at
    // template_hash NULL and will be lazy-promoted via
    // auto_promote_on_update on the user's next edit. Forcing the
    // whole migration to retry every launch on a single failed row
    // would be worse UX.
    let mut s2 = AppSettings::load();
    s2.templates_v2_migrated = true;
    if let Err(e) = s2.save() {
        log::warn!("columns: failed to persist v2 migration flag: {}", e);
        return;
    }
    let _ = MIGRATED.set(());
    // Mirror the freshly-minted templates into the local cache so
    // the pull-to-cache call right after the migration sees them
    // as already-local and doesn't re-fetch.
    mirror_to_local_cache(remote_dir);
}

fn run_v1_migration_once(farm_root: &str) {
    static MIGRATED: OnceLock<()> = OnceLock::new();
    if MIGRATED.get().is_some() {
        return;
    }
    let s = AppSettings::load();
    if s.templates_migrated_v1 {
        let _ = MIGRATED.set(());
        return;
    }
    let Some(db) = shared_db() else { return };
    match ufb_core::templates::migrate_v1_legacy_presets(&db, farm_root) {
        Ok(n) => {
            log::info!("templates: v1 migration wrote {} template(s)", n);
            // Flag the migration done. We treat "0 templates
            // migrated" the same as "all templates migrated" — the
            // migration is idempotent so a subsequent launch with
            // new legacy rows would still re-write them, but the
            // intent is "we ran the scan, no need to scan again."
            let mut s2 = AppSettings::load();
            s2.templates_migrated_v1 = true;
            if let Err(e) = s2.save() {
                log::warn!("templates: failed to persist migration flag: {}", e);
                return;
            }
            let _ = MIGRATED.set(());
        }
        Err(e) => {
            log::warn!("templates: v1 migration failed: {} (will retry next launch)", e);
        }
    }
}

impl qobject::Columns {
    fn get_column_presets(self: &qobject::Columns) -> cxx_qt_lib::QString {
        let Some(dir) = templates_dir() else {
            return cxx_qt_lib::QString::from("[]");
        };
        let templates = match templates::list(&dir) {
            Ok(v) => v,
            Err(e) => {
                log::warn!("columns: list templates failed: {}", e);
                return cxx_qt_lib::QString::from("[]");
            }
        };
        // Reshape for QML: include the `data` payload as a JSON-
        // string field `columnsJson` for backwards compat with
        // ColumnManagerDialog's existing parse path.
        let view: Vec<serde_json::Value> = templates
            .iter()
            .map(|t| {
                let columns_json =
                    serde_json::to_string(&t.data).unwrap_or_else(|_| "{}".into());
                serde_json::json!({
                    "uniqueName": t.unique_name,
                    "displayName": t.display_name,
                    "createdBy": t.created_by,
                    "createdAt": t.created_at,
                    "updatedAt": t.updated_at,
                    "columnsJson": columns_json,
                })
            })
            .collect();
        let json = serde_json::to_string(&view).unwrap_or_else(|_| "[]".into());
        cxx_qt_lib::QString::from(&json)
    }

    fn save_column_as_preset(
        self: &qobject::Columns,
        display_name: cxx_qt_lib::QString,
        column_id: i64,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let Some(mgr) = shared_manager() else {
            return cxx_qt_lib::QString::from("");
        };
        let Some(dir) = templates_dir() else {
            log::warn!(
                "columns: save_column_as_preset — shared folder unavailable; \
                 template not saved"
            );
            return cxx_qt_lib::QString::from("");
        };
        let display_s = display_name.to_string();
        let unique_s = templates::slug_for(&display_s);

        // Display-name collision check — surface the warning string
        // back to QML so it can route the user into "edit existing
        // instead?" without a follow-up call.
        match templates::find_display_collision(&dir, &display_s, &unique_s) {
            Ok(Some(_existing)) => {
                return cxx_qt_lib::QString::from("__name_collision__");
            }
            Ok(None) => {}
            Err(e) => {
                log::warn!("columns: collision check failed: {}", e);
                // Soft-fail open — proceed with the write rather than
                // blocking the user when the dir is briefly stat'able
                // but listing failed.
            }
        }

        let job_s = job_path.to_string();
        let folder_s = folder_name.to_string();
        let Some(def) = mgr
            .get_column_defs(&job_s, &folder_s)
            .ok()
            .and_then(|defs| defs.into_iter().find(|d| d.id == Some(column_id)))
        else {
            log::warn!(
                "columns: save_column_as_preset({}) — column {} not found in {}/{}",
                display_s, column_id, job_s, folder_s
            );
            return cxx_qt_lib::QString::from("");
        };

        let preset_def = PresetColumnDef {
            column_name: def.column_name.clone(),
            column_type: def.column_type.clone(),
            column_order: def.column_order,
            column_width: def.column_width,
            is_visible: def.is_visible,
            default_value: def.default_value.clone(),
            options: def.options.clone(),
        };

        let now = ufb_core::utils::current_time_ms();
        // Preserve original createdAt + createdBy when overwriting an
        // existing template — only the editor and updatedAt advance.
        let (created_at, created_by) = match templates::read(&dir, &unique_s) {
            Ok(existing) => (existing.created_at, existing.created_by),
            Err(_) => (now, templates::current_user()),
        };

        let template = templates::TemplateFile {
            schema_version: 1,
            unique_name: unique_s.clone(),
            display_name: display_s,
            created_by,
            created_at,
            updated_at: now,
            data: preset_def,
        };

        if let Err(e) = templates::write_atomic(&dir, &template) {
            log::warn!("columns: write_atomic({}) failed: {}", unique_s, e);
            return cxx_qt_lib::QString::from("");
        }
        cxx_qt_lib::QString::from(&unique_s)
    }

    fn apply_preset(
        self: &qobject::Columns,
        preset_unique_name: cxx_qt_lib::QString,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
    ) -> i64 {
        let Some(mgr) = shared_manager() else { return -1; };
        let Some(dir) = templates_dir() else { return -1; };
        let unique_s = preset_unique_name.to_string();
        let template = match templates::read(&dir, &unique_s) {
            Ok(t) => t,
            Err(e) => {
                log::warn!("columns: apply_preset({}) read failed: {}", unique_s, e);
                return -1;
            }
        };
        let job_s = job_path.to_string();
        let folder_s = folder_name.to_string();
        let def = match mgr.add_preset_column_from_def(&template.data, &job_s, &folder_s) {
            Ok(def) => def,
            Err(e) => {
                log::warn!("columns: apply_preset({}) add failed: {}", unique_s, e);
                return -1;
            }
        };
        let new_id = def.id.unwrap_or(-1);
        // Per-folder col_add still goes through the mesh broadcast —
        // only the template definition itself moved off-mesh.
        if new_id > 0 {
            broadcast_col_change("col_add", &mgr, &def.job_path, &def.folder_name, new_id);
        }
        new_id
    }

    fn delete_column_preset(
        self: &qobject::Columns,
        preset_unique_name: cxx_qt_lib::QString,
    ) -> bool {
        let Some(dir) = templates_dir() else { return false };
        let unique_s = preset_unique_name.to_string();
        match templates::delete(&dir, &unique_s) {
            Ok(_) => true,
            Err(e) => {
                log::warn!(
                    "columns: delete_column_preset({}) failed: {}",
                    unique_s,
                    e
                );
                false
            }
        }
    }
}

/// Re-fetch the canonical `ColumnDefinition` for `(job_path, folder_name, id)`
/// after a successful add/update/delete and broadcast a `col_add` /
/// `col_update` table change. Re-fetching avoids the bindings layer having
/// to construct a `modified_time` that disagrees with what the manager just
/// stamped (the manager uses `crate::utils::current_time_ms()`); the
/// LWW-by-modified_time merge on the receive side requires the broadcast
/// timestamp match the local row, otherwise a follower's later snapshot
/// would re-apply with `modified_time=0`.
fn broadcast_col_change(
    action: &str,
    mgr: &Arc<ColumnConfigManager>,
    job_path: &str,
    folder_name: &str,
    id: i64,
) {
    // 0.9.97 — `get_column_defs` filters to grid-visible rows
    // (template_hash IS NOT NULL AND trashed_at IS NULL AND
    // deleted_at IS NULL). The mesh broadcast needs to fire on
    // freshly-added rows that are unpromoted (no template_hash yet,
    // pending auto-promote) and on trashed-via-broadcast rows that
    // the dedicated lifecycle paths handle separately. Use the
    // direct id-lookup helper instead so the warning "column id=N
    // not found" stops firing on every legitimate add/update.
    let _ = job_path;
    let _ = folder_name;
    let Some(def) = mgr.get_column_by_id(id) else {
        log::warn!(
            "columns: broadcast_col_change — column id={} not found by id (row was deleted before broadcast?)",
            id
        );
        return;
    };
    let def_str = match serde_json::to_string(&def) {
        Ok(s) => s,
        Err(e) => {
            log::warn!("columns: broadcast_col_change serialize failed: {}", e);
            return;
        }
    };
    let change = serde_json::json!({
        "action": action,
        "def": def_str,
        "modified_time": def.modified_time,
    })
    .to_string();
    super::mesh::broadcast_table_change(change);
}
