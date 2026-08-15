//! `Subscription` QObject — wraps `core::subscription::SubscriptionManager`.
//!
//! Subscribe / unsubscribe / per-item metadata. Item-metadata mutations
//! fire a mesh broadcast via `super::mesh::broadcast_metadata_edit` so
//! peers see the change (and the leader marks the snapshot dirty so the
//! next 30-second tick rewrites the NAS snapshot). Subscribe/unsubscribe
//! do NOT broadcast — subscriptions are per-user local state (1.1.1+).
//!
//! Singleton; uses `bindings::db::shared_db()` so it shares one
//! connection with Bookmarks.

use crate::db::shared_db;
use std::pin::Pin;
use std::sync::{Arc, OnceLock};
use ufb_core::subscription::{Subscription, SubscriptionManager, TrackedItemRecord};
use ufb_core::utils::from_canonical_path;

/// Translate a Windows-canonical DB path into the local OS native form.
fn to_native(canonical_path: &str) -> String {
    let settings_arc = crate::services::settings::shared_settings();
    let settings = settings_arc.read().unwrap();
    from_canonical_path(canonical_path, &settings.path_mappings)
}

/// Translate every subscription's `job_path` from canonical (DB) to
/// native (current OS) before serialising for QML. QML clicks the
/// path verbatim — without this, mac users see Windows paths and
/// navigate to non-existent locations.
fn translate_subscriptions_for_native(subs: &mut Vec<Subscription>) {
    let settings_arc = crate::services::settings::shared_settings();
    let settings = settings_arc.read().unwrap();
    for s in subs.iter_mut() {
        s.job_path = from_canonical_path(&s.job_path, &settings.path_mappings);
    }
}

/// Resolve the job root containing `native_path`: the first path
/// component beneath a flagged jobs-folder root (mount-config
/// `is_jobs_folder` volumes, or bookmarks flagged `is_project_folder`).
/// Matching happens in canonical identity space, ASCII-case-insensitive
/// with separators normalised — the same tolerance `is_shared_folder`
/// uses in columns.rs. Returns the NATIVE job-root path, preserving the
/// input path's own casing.
fn job_root_for_native_path(native_path: &str) -> Option<String> {
    let trimmed = native_path.trim();
    if trimmed.is_empty() {
        return None;
    }
    let mappings = {
        let settings_arc = crate::services::settings::shared_settings();
        let settings = settings_arc.read().unwrap();
        settings.path_mappings.clone()
    };
    let input_id = ufb_core::utils::to_identity_storage(trimmed, &mappings).replace('\\', "/");

    // Candidate jobs-folder roots, canonical identity form.
    let mut roots: Vec<String> = super::mount::jobs_folder_volume_paths()
        .iter()
        .map(|r| ufb_core::utils::to_identity_storage(r, &mappings))
        .collect();
    if let Some(db) = shared_db() {
        let bookmarked: Result<Vec<String>, _> = db.with_conn(|conn| {
            let mut stmt =
                conn.prepare("SELECT path FROM bookmarks WHERE is_project_folder = 1")?;
            let rows = stmt.query_map([], |row| row.get::<_, String>(0))?;
            rows.collect()
        });
        match bookmarked {
            Ok(paths) => roots.extend(
                paths
                    .iter()
                    .map(|r| ufb_core::utils::to_identity_storage(r, &mappings)),
            ),
            Err(e) => log::warn!("subscription: job_root bookmarks query failed: {}", e),
        }
    }

    // LONGEST matching root wins: with nested roots (a bookmarked
    // project folder inside a jobs-folder mount, or a year-folder
    // layout) first-match would resolve the link to the container
    // folder and subscribe the user to a non-job.
    let mut best: Option<String> = None;
    let mut best_len = 0usize;
    for root in &roots {
        let root_norm = root.replace('\\', "/");
        let root_norm = root_norm.trim_end_matches('/');
        if root_norm.is_empty()
            || root_norm.len() <= best_len
            || input_id.len() <= root_norm.len()
            || !input_id.is_char_boundary(root_norm.len())
        {
            continue;
        }
        let (head, tail) = input_id.split_at(root_norm.len());
        if !head.eq_ignore_ascii_case(root_norm) || !tail.starts_with('/') {
            continue;
        }
        let first = tail
            .trim_start_matches('/')
            .split('/')
            .next()
            .unwrap_or("");
        if first.is_empty() {
            continue;
        }
        // `head` carries the input's own casing.
        best = Some(format!("{}/{}", head, first));
        best_len = root_norm.len();
    }
    let job_id = best?;

    // Never auto-subscribe over an existing ACTIVE subscription: the
    // caller only reaches here when its QML-side lookup missed, and a
    // miss can be spelling drift on an already-subscribed job — the
    // subscribe upsert would silently reset the user's custom job name.
    if let Some(db) = shared_db() {
        let active: Vec<String> = db
            .with_conn(|conn| {
                let mut stmt = conn
                    .prepare("SELECT job_path FROM subscriptions WHERE deleted_at IS NULL")?;
                let rows = stmt.query_map([], |row| row.get::<_, String>(0))?;
                rows.collect()
            })
            .unwrap_or_default();
        for row in &active {
            let row_id = ufb_core::utils::to_identity_storage(row, &mappings).replace('\\', "/");
            if row_id.eq_ignore_ascii_case(&job_id) {
                return None;
            }
        }
    }

    // Only real directories qualify — a link to a loose file sitting
    // directly under a jobs root must not create a file "job".
    let native = to_native(&job_id);
    if !std::path::Path::new(&native).is_dir() {
        return None;
    }
    Some(native)
}

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
        #[qproperty(QString, subscriptions_json)]
        #[qproperty(QString, tracked_items_json)]
        // Monotonic counter — bumps on every metadata write
        // (set_item_metadata, set_item_metadata_field, set_item_tracked).
        // Consumers (TrackerView, ItemListPanel) observe Changed and
        // refresh, even when tracked_items_json itself is unchanged
        // (the item being edited isn't tracked, so the tracked-items
        // result set doesn't change).
        #[qproperty(i32, metadata_rev)]
        type Subscription = super::SubscriptionRust;

        /// Re-read the subscriptions table.
        #[qinvokable]
        fn refresh(self: Pin<&mut Subscription>);

        /// Re-read all tracked items (across all subscriptions) into
        /// `tracked_items_json`. ItemListPanel + TrackerView filter
        /// the array client-side by job_path and/or folder_name.
        #[qinvokable]
        fn refresh_tracked(self: Pin<&mut Subscription>);

        /// Subscribe to `job_path` with display `job_name`. Triggers
        /// a refresh on success. Idempotent — re-subscribing an
        /// existing path updates the display name and clears any
        /// soft-delete.
        #[qinvokable]
        fn subscribe_to_job(
            self: Pin<&mut Subscription>,
            job_path: QString,
            job_name: QString,
        );

        /// Soft-delete the subscription for `job_path`. Triggers a refresh.
        #[qinvokable]
        fn unsubscribe(self: Pin<&mut Subscription>, job_path: QString);

        /// Create a new job folder under `parent_path` from the
        /// bundled project template, then auto-subscribe to it.
        /// Returns the created folder path on success, or an empty
        /// string on failure (warning logged). The bundled template
        /// lives at `<exe_dir>/templates/projectTemplate/`.
        #[qinvokable]
        fn create_job_from_template(
            self: Pin<&mut Subscription>,
            parent_path: QString,
            job_number: QString,
            job_name: QString,
        ) -> QString;

        /// Toggle the is_tracked flag on an item's metadata row.
        /// Creates the row on first call (with empty metadata `{}`).
        /// Triggers refresh_tracked on success so consumers update.
        #[qinvokable]
        fn set_item_tracked(
            self: Pin<&mut Subscription>,
            job_path: QString,
            item_path: QString,
            folder_name: QString,
            tracked: bool,
        );

        /// Bulk-fetch item metadata for one folder. Returns a JSON
        /// array of {itemPath, folderName, metadataJson, isTracked}
        /// for every item with stored metadata under (job_path,
        /// folder_name). Used by ItemListPanel to display dynamic
        /// metadata columns + populate the orphan-discovery pass for
        /// the column manager.
        #[qinvokable]
        fn folder_item_metadata(
            self: &Subscription,
            job_path: QString,
            folder_name: QString,
        ) -> QString;

        /// Replace an item's metadata blob and explicitly set its
        /// `is_tracked` flag. Used by per-cell edits in TrackerView /
        /// ItemListPanel — the QML side clones the row's metadata,
        /// updates the changed key, and stringifies the result.
        /// Triggers refresh_tracked so consumers redraw.
        #[qinvokable]
        fn set_item_metadata(
            self: Pin<&mut Subscription>,
            job_path: QString,
            item_path: QString,
            folder_name: QString,
            metadata_json: QString,
            is_tracked: bool,
        );

        /// Update a single metadata field on an item, merging against
        /// the current DB state. Avoids the QML-side stale-snapshot
        /// race that bites set_item_metadata when the user edits
        /// several cells in quick succession (each cell clones the
        /// row's metadata before the previous edit's refresh_tracked
        /// has rebuilt it). `value_json` should be a JSON-encoded
        /// scalar (`"\"high\""`, `42`, `true`, `null`); plain non-JSON
        /// strings are accepted as a fallback. Triggers
        /// refresh_tracked.
        #[qinvokable]
        fn set_item_metadata_field(
            self: Pin<&mut Subscription>,
            job_path: QString,
            item_path: QString,
            folder_name: QString,
            field_name: QString,
            value_json: QString,
            is_tracked: bool,
        );

        /// Bulk variant of set_item_metadata_field — applies the same
        /// (field_name, value_json) to every entry in `items_json`,
        /// which is a JSON array of `{jobPath, itemPath, folderName,
        /// isTracked}` records. One refresh_tracked + one
        /// metadata_rev bump at the end so observers don't redraw
        /// per-row. Used by the Tracker's multi-select bulk-edit
        /// menu.
        #[qinvokable]
        fn bulk_set_item_metadata_field(
            self: Pin<&mut Subscription>,
            items_json: QString,
            field_name: QString,
            value_json: QString,
        );

        /// Bulk track/untrack — flip is_tracked on every item in
        /// `items_json` (same shape as bulk_set_item_metadata_field
        /// without isTracked). One refresh + one metadata_rev bump.
        #[qinvokable]
        fn bulk_set_item_tracked(
            self: Pin<&mut Subscription>,
            items_json: QString,
            tracked: bool,
        );

        /// Resolve the job-root folder containing `path`: the first
        /// component under a jobs-folder root (mount config flag or a
        /// bookmark flagged is_project_folder). Returns the NATIVE
        /// job-root path, or "" when `path` isn't inside any flagged
        /// root. Used by the ufb:// deep-link handler to local-auto-
        /// subscribe before routing into a job tab.
        #[qinvokable]
        fn job_root_for_path(self: &Subscription, path: QString) -> QString;
    }
}

pub struct SubscriptionRust {
    pub subscriptions_json: cxx_qt_lib::QString,
    pub tracked_items_json: cxx_qt_lib::QString,
    pub metadata_rev: i32,
}

impl Default for SubscriptionRust {
    fn default() -> Self {
        Self {
            subscriptions_json: cxx_qt_lib::QString::from("[]"),
            tracked_items_json: cxx_qt_lib::QString::from("[]"),
            metadata_rev: 0,
        }
    }
}

fn shared_manager() -> Option<Arc<SubscriptionManager>> {
    static MANAGER: OnceLock<Option<Arc<SubscriptionManager>>> = OnceLock::new();
    MANAGER
        .get_or_init(|| shared_db().map(|db| Arc::new(SubscriptionManager::new(db))))
        .clone()
}

fn serialize(subs: &[Subscription]) -> String {
    serde_json::to_string(subs).unwrap_or_else(|_| "[]".into())
}

fn serialize_tracked(items: &[TrackedItemRecord]) -> String {
    serde_json::to_string(items).unwrap_or_else(|_| "[]".into())
}

impl qobject::Subscription {
    fn refresh(mut self: Pin<&mut qobject::Subscription>) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        match mgr.get_all_subscriptions() {
            Ok(mut list) => {
                // Convert canonical (Win) DB paths → native paths for
                // QML so click-to-navigate hits a path that actually
                // exists on this OS.
                translate_subscriptions_for_native(&mut list);
                let json = serialize(&list);
                self.as_mut()
                    .set_subscriptions_json(cxx_qt_lib::QString::from(&json));
            }
            Err(e) => log::warn!("subscription: refresh failed: {}", e),
        }
    }

    fn subscribe_to_job(
        self: Pin<&mut qobject::Subscription>,
        job_path: cxx_qt_lib::QString,
        job_name: cxx_qt_lib::QString,
    ) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        let native_path = job_path.to_string();
        let name_s = job_name.to_string();
        // Auto-name from the NATIVE path's basename so the user sees
        // their own filesystem's folder name, not the canonical Win
        // basename (which on mac would be a .replace('\\','/') artifact).
        let display = if name_s.is_empty() {
            std::path::Path::new(&native_path)
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_else(|| native_path.clone())
        } else {
            name_s
        };
        // Subscriptions are per-user state — never broadcast to the mesh.
        match mgr.subscribe_to_job(&native_path, &display) {
            Ok(sub) => {
                log::info!("subscription: subscribed to {} ({})", sub.job_path, sub.job_name);
            }
            Err(e) => {
                log::warn!("subscription: subscribe failed: {}", e);
                return;
            }
        }
        self.refresh();
    }

    fn unsubscribe(self: Pin<&mut qobject::Subscription>, job_path: cxx_qt_lib::QString) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        let native_path = job_path.to_string();
        if let Err(e) = mgr.unsubscribe_from_job(&native_path) {
            log::warn!("subscription: unsubscribe {} failed: {}", native_path, e);
            return;
        }
        log::info!("subscription: unsubscribed {}", native_path);
        // Subscriptions are per-user state — never broadcast to the mesh.
        self.refresh();
    }

    fn create_job_from_template(
        mut self: Pin<&mut qobject::Subscription>,
        parent_path: cxx_qt_lib::QString,
        job_number: cxx_qt_lib::QString,
        job_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let parent = parent_path.to_string();
        let number  = job_number.to_string();
        let name    = job_name.to_string();

        // Locate the bundled template directory relative to the
        // running executable. Two layouts:
        //   • Windows / Linux: `<exe_dir>/templates/projectTemplate/`
        //   • macOS:           `<exe_dir>/../Resources/templates/projectTemplate/`
        // Loose dirs under .app/Contents/MacOS/ break codesign, so the
        // .app puts templates in Resources/. Try both — first hit wins.
        let exe = match std::env::current_exe() {
            Ok(p) => p,
            Err(e) => {
                log::warn!("create_job_from_template: current_exe failed: {}", e);
                return cxx_qt_lib::QString::from("");
            }
        };
        let exe_dir = exe.parent();
        let candidates: Vec<std::path::PathBuf> = exe_dir
            .into_iter()
            .flat_map(|d| {
                // `mut` only exercised by the macOS Resources push below.
                #[cfg_attr(not(target_os = "macos"), allow(unused_mut))]
                let mut v = vec![d.join("templates").join("projectTemplate")];
                #[cfg(target_os = "macos")]
                v.push(
                    d.join("..")
                        .join("Resources")
                        .join("templates")
                        .join("projectTemplate"),
                );
                v
            })
            .collect();
        let template_dir = match candidates.into_iter().find(|p| p.is_dir()) {
            Some(p) => p,
            None => {
                log::warn!(
                    "create_job_from_template: template dir missing (exe_dir={:?})",
                    exe_dir
                );
                return cxx_qt_lib::QString::from("");
            }
        };

        // Copy the tree first.
        let parent_p = std::path::Path::new(&parent);
        let new_path = match ufb_core::jobs::create_from_template(
            parent_p,
            &template_dir,
            &number,
            &name,
        ) {
            Ok(p) => p,
            Err(e) => {
                log::warn!("create_job_from_template: {}", e);
                return cxx_qt_lib::QString::from("");
            }
        };
        let new_path_s = new_path.to_string_lossy().to_string();

        // Auto-subscribe so the new job appears under SUBSCRIPTIONS
        // immediately. Use the folder basename as the display name -
        // that matches what the user typed.
        let display = new_path
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_else(|| new_path_s.clone());
        if let Some(mgr) = shared_manager() {
            match mgr.subscribe_to_job(&new_path_s, &display) {
                Ok(_) => {
                    log::info!(
                        "create_job_from_template: created + subscribed {}",
                        new_path_s
                    );
                    self.as_mut().refresh();
                }
                Err(e) => {
                    log::warn!(
                        "create_job_from_template: created {} but auto-subscribe failed: {}",
                        new_path_s,
                        e
                    );
                }
            }
        }

        // Return the NATIVE path so QML can navigate to it.
        cxx_qt_lib::QString::from(&new_path_s)
    }

    fn refresh_tracked(mut self: Pin<&mut qobject::Subscription>) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        match mgr.get_all_tracked_items() {
            Ok(mut items) => {
                // DB stores canonical (Win) paths; QML compares against
                // native paths it gets from the file system. Translate
                // both job_path and item_path before serialising.
                let settings_arc = crate::services::settings::shared_settings();
                let settings = settings_arc.read().unwrap();
                // v5: blobs are stored uuid-keyed; translate to display
                // names for QML. Tracked items span folders, so cache one
                // uuid→name map per (canonical job, folder). Do this BEFORE
                // rewriting job_path to native, since get_column_defs keys
                // on the stored (canonical) job_path.
                let cm = super::columns::shared_manager();
                let mut map_cache: std::collections::HashMap<
                    (String, String),
                    std::collections::HashMap<String, String>,
                > = std::collections::HashMap::new();
                for it in items.iter_mut() {
                    if let Some(ref cm) = cm {
                        let map = map_cache
                            .entry((it.job_path.clone(), it.folder_name.clone()))
                            .or_insert_with(|| cm.uuid_to_name_map(&it.job_path, &it.folder_name));
                        it.metadata_json =
                            ufb_core::columns::ColumnConfigManager::remap_top_level_keys(
                                &it.metadata_json,
                                map,
                            );
                    }
                    it.job_path = from_canonical_path(&it.job_path, &settings.path_mappings);
                    it.item_path = from_canonical_path(&it.item_path, &settings.path_mappings);
                }
                drop(settings);
                let json = serialize_tracked(&items);
                self.as_mut()
                    .set_tracked_items_json(cxx_qt_lib::QString::from(&json));
            }
            Err(e) => log::warn!("subscription: refresh_tracked failed: {}", e),
        }
    }

    fn folder_item_metadata(
        self: &qobject::Subscription,
        job_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let Some(mgr) = shared_manager() else {
            return cxx_qt_lib::QString::from("[]");
        };
        // QML → native, DB → canonical: translate before the SELECT so
        // the WHERE matches.
        let job_canon = job_path.to_string();
        let folder_s = folder_name.to_string();
        match mgr.get_folder_item_metadata(&job_canon, &folder_s) {
            Ok(mut items) => {
                // v5: blobs are stored uuid-keyed; translate back to
                // display-name keys for QML (one map for this folder).
                let name_map = super::columns::shared_manager()
                    .map(|cm| cm.uuid_to_name_map(&job_canon, &folder_s));
                // Translate item_path back to native for QML consumers.
                for it in items.iter_mut() {
                    it.item_path = to_native(&it.item_path);
                    if let Some(ref m) = name_map {
                        it.metadata_json =
                            ufb_core::columns::ColumnConfigManager::remap_top_level_keys(
                                &it.metadata_json,
                                m,
                            );
                    }
                }
                let json = serde_json::to_string(&items).unwrap_or_else(|_| "[]".into());
                cxx_qt_lib::QString::from(&json)
            }
            Err(e) => {
                log::warn!("subscription: folder_item_metadata failed: {}", e);
                cxx_qt_lib::QString::from("[]")
            }
        }
    }

    fn job_root_for_path(
        self: &qobject::Subscription,
        path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(
            &job_root_for_native_path(&path.to_string()).unwrap_or_default(),
        )
    }

    fn set_item_metadata_field(
        mut self: Pin<&mut qobject::Subscription>,
        job_path: cxx_qt_lib::QString,
        item_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        field_name: cxx_qt_lib::QString,
        value_json: cxx_qt_lib::QString,
        is_tracked: bool,
    ) {
        let Some(mgr) = shared_manager() else { return };
        // QML hands us native paths; DB + mesh use canonical (Win) form.
        let job_s = job_path.to_string();
        let item_s = item_path.to_string();
        let folder_s = folder_name.to_string();
        let field_s = field_name.to_string();
        let value_s = value_json.to_string();

        // Read current metadata blob from DB (source of truth).
        let existing = mgr
            .get_item_metadata(&item_s)
            .ok()
            .flatten()
            .unwrap_or_else(|| "{}".into());
        let mut meta: serde_json::Value = serde_json::from_str(&existing)
            .unwrap_or_else(|_| serde_json::json!({}));
        if !meta.is_object() {
            meta = serde_json::json!({});
        }

        // Parse incoming value as JSON; fall back to raw string when
        // it isn't valid JSON (older callers / bare text).
        let value_v: serde_json::Value = serde_json::from_str(&value_s)
            .unwrap_or_else(|_| serde_json::Value::String(value_s.clone()));

        // v5: cell values are stored keyed by the column's stable
        // column_uuid, not its display name — so a rename never orphans
        // the value and peers align by identity (the existing blob read
        // above is already uuid-keyed). Fall back to the raw name when the
        // column has no def (defensive; the cell UI only edits existing
        // columns).
        let field_key = super::columns::shared_manager()
            .and_then(|cm| cm.column_uuid_for_name(&job_s, &folder_s, &field_s))
            .unwrap_or_else(|| field_s.clone());

        if let Some(obj) = meta.as_object_mut() {
            obj.insert(field_key, value_v);
        }

        let new_json = serde_json::to_string(&meta).unwrap_or_else(|_| "{}".into());
        let mtime = match mgr.upsert_item_metadata(&job_s, &item_s, &folder_s, &new_json, is_tracked) {
            Ok(m) => m,
            Err(e) => {
                log::warn!(
                    "subscription: set_item_metadata_field({}, {}) failed: {}",
                    item_s,
                    field_s,
                    e
                );
                return;
            }
        };
        log::info!(
            "subscription: set_item_metadata_field {} {} = {}",
            item_s,
            field_s,
            value_s
        );
        super::mesh::broadcast_metadata_edit(
            job_s,
            item_s,
            new_json,
            folder_s,
            is_tracked,
            mtime,
        );
        self.as_mut().refresh_tracked();
        let rev = *self.as_ref().metadata_rev();
        self.as_mut().set_metadata_rev(rev.wrapping_add(1));
    }

    fn set_item_metadata(
        mut self: Pin<&mut qobject::Subscription>,
        job_path: cxx_qt_lib::QString,
        item_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        metadata_json: cxx_qt_lib::QString,
        is_tracked: bool,
    ) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        // Native → canonical for DB write + mesh broadcast.
        let job_s = job_path.to_string();
        let item_s = item_path.to_string();
        let folder_s = folder_name.to_string();
        // QML hands a NAME-keyed blob; the DB stores UUID-keyed. Remap the
        // whole blob before persisting/broadcasting (v5). Unmapped keys
        // pass through unchanged.
        let meta_s = {
            let raw = metadata_json.to_string();
            match super::columns::shared_manager() {
                Some(cm) => {
                    let map = cm.name_to_uuid_map(&job_s, &folder_s);
                    ufb_core::columns::ColumnConfigManager::remap_top_level_keys(&raw, &map)
                }
                None => raw,
            }
        };
        let mtime = match mgr.upsert_item_metadata(&job_s, &item_s, &folder_s, &meta_s, is_tracked) {
            Ok(m) => m,
            Err(e) => {
                log::warn!(
                    "subscription: set_item_metadata({}) failed: {}",
                    item_s,
                    e
                );
                return;
            }
        };
        log::info!("subscription: set_item_metadata {} = {}", item_s, meta_s);
        super::mesh::broadcast_metadata_edit(
            job_s,
            item_s,
            meta_s,
            folder_s,
            is_tracked,
            mtime,
        );
        self.as_mut().refresh_tracked();
        let rev = *self.as_ref().metadata_rev();
        self.as_mut().set_metadata_rev(rev.wrapping_add(1));
    }

    fn bulk_set_item_metadata_field(
        mut self: Pin<&mut qobject::Subscription>,
        items_json: cxx_qt_lib::QString,
        field_name: cxx_qt_lib::QString,
        value_json: cxx_qt_lib::QString,
    ) {
        let Some(mgr) = shared_manager() else { return };
        let items_s = items_json.to_string();
        let field_s = field_name.to_string();
        let value_s = value_json.to_string();

        let parsed: Vec<serde_json::Value> = serde_json::from_str(&items_s)
            .unwrap_or_default();
        if parsed.is_empty() { return; }

        let value_v: serde_json::Value = serde_json::from_str(&value_s)
            .unwrap_or_else(|_| serde_json::Value::String(value_s.clone()));

        let mut applied = 0usize;
        for entry in &parsed {
            let Some(obj) = entry.as_object() else { continue };
            let Some(job_p) = obj.get("jobPath").and_then(|v| v.as_str()) else { continue };
            let Some(item_p) = obj.get("itemPath").and_then(|v| v.as_str()) else { continue };
            let Some(folder_n) = obj.get("folderName").and_then(|v| v.as_str()) else { continue };
            let is_tracked = obj.get("isTracked").and_then(|v| v.as_bool()).unwrap_or(true);

            // QML hands us native paths; DB + mesh use canonical.
            let job_canon = job_p.to_string();
            let item_canon = item_p.to_string();

            let existing = mgr
                .get_item_metadata(&item_canon)
                .ok()
                .flatten()
                .unwrap_or_else(|| "{}".into());
            let mut meta: serde_json::Value = serde_json::from_str(&existing)
                .unwrap_or_else(|_| serde_json::json!({}));
            if !meta.is_object() { meta = serde_json::json!({}); }
            // v5: resolve the column NAME to its stable uuid PER ITEM —
            // the bulk selection can span folders, and a column's uuid is
            // per (job, folder). Fall back to the name if no def.
            let field_key = super::columns::shared_manager()
                .and_then(|cm| cm.column_uuid_for_name(&job_canon, folder_n, &field_s))
                .unwrap_or_else(|| field_s.clone());
            if let Some(o) = meta.as_object_mut() {
                o.insert(field_key, value_v.clone());
            }
            let new_json = serde_json::to_string(&meta).unwrap_or_else(|_| "{}".into());
            let mtime = match mgr.upsert_item_metadata(&job_canon, &item_canon, folder_n, &new_json, is_tracked) {
                Ok(m) => m,
                Err(e) => {
                    log::warn!(
                        "subscription: bulk_set_item_metadata_field({}, {}) row failed: {}",
                        item_p, field_s, e
                    );
                    continue;
                }
            };
            super::mesh::broadcast_metadata_edit(
                job_canon,
                item_canon,
                new_json,
                folder_n.to_string(),
                is_tracked,
                mtime,
            );
            applied += 1;
        }
        log::info!(
            "subscription: bulk_set_item_metadata_field {} = {} ({} of {} items)",
            field_s, value_s, applied, parsed.len()
        );
        self.as_mut().refresh_tracked();
        let rev = *self.as_ref().metadata_rev();
        self.as_mut().set_metadata_rev(rev.wrapping_add(1));
    }

    fn bulk_set_item_tracked(
        mut self: Pin<&mut qobject::Subscription>,
        items_json: cxx_qt_lib::QString,
        tracked: bool,
    ) {
        let Some(mgr) = shared_manager() else { return };
        let items_s = items_json.to_string();
        let parsed: Vec<serde_json::Value> = serde_json::from_str(&items_s)
            .unwrap_or_default();
        if parsed.is_empty() { return; }

        let mut applied = 0usize;
        for entry in &parsed {
            let Some(obj) = entry.as_object() else { continue };
            let Some(job_p) = obj.get("jobPath").and_then(|v| v.as_str()) else { continue };
            let Some(item_p) = obj.get("itemPath").and_then(|v| v.as_str()) else { continue };
            let Some(folder_n) = obj.get("folderName").and_then(|v| v.as_str()) else { continue };

            // QML hands us native paths; DB + mesh use canonical.
            let job_canon = job_p.to_string();
            let item_canon = item_p.to_string();

            let existing = mgr.get_item_metadata(&item_canon).ok().flatten();
            let metadata = existing.unwrap_or_else(|| "{}".to_string());
            let mtime = match mgr.upsert_item_metadata(&job_canon, &item_canon, folder_n, &metadata, tracked) {
                Ok(m) => m,
                Err(e) => {
                    log::warn!(
                        "subscription: bulk_set_item_tracked({}, tracked={}) row failed: {}",
                        item_p, tracked, e
                    );
                    continue;
                }
            };
            super::mesh::broadcast_metadata_edit(
                job_canon,
                item_canon,
                metadata,
                folder_n.to_string(),
                tracked,
                mtime,
            );
            applied += 1;
        }
        log::info!(
            "subscription: bulk_set_item_tracked tracked={} ({} of {} items)",
            tracked, applied, parsed.len()
        );
        self.as_mut().refresh_tracked();
        let rev = *self.as_ref().metadata_rev();
        self.as_mut().set_metadata_rev(rev.wrapping_add(1));
    }

    fn set_item_tracked(
        mut self: Pin<&mut qobject::Subscription>,
        job_path: cxx_qt_lib::QString,
        item_path: cxx_qt_lib::QString,
        folder_name: cxx_qt_lib::QString,
        tracked: bool,
    ) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        // Native → canonical for DB write + mesh broadcast.
        let job_s = job_path.to_string();
        let item_s = item_path.to_string();
        let folder_s = folder_name.to_string();
        // Preserve any existing metadata payload — only flip is_tracked.
        let existing = mgr.get_item_metadata(&item_s).ok().flatten();
        let metadata = existing.unwrap_or_else(|| "{}".to_string());
        let mtime = match mgr.upsert_item_metadata(&job_s, &item_s, &folder_s, &metadata, tracked) {
            Ok(m) => m,
            Err(e) => {
                log::warn!(
                    "subscription: set_item_tracked({}, tracked={}) failed: {}",
                    item_s,
                    tracked,
                    e
                );
                return;
            }
        };
        log::debug!(
            "subscription: set_item_tracked {} → {}",
            item_s,
            tracked
        );
        super::mesh::broadcast_metadata_edit(
            job_s,
            item_s,
            metadata,
            folder_s,
            tracked,
            mtime,
        );
        self.as_mut().refresh_tracked();
        let rev = *self.as_ref().metadata_rev();
        self.as_mut().set_metadata_rev(rev.wrapping_add(1));
    }
}
