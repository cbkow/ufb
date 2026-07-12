//! `Settings` QObject — wraps `core::settings::AppSettings`.
//!
//! Phase 12 first slice: just enough to construct the per-job
//! Google Drive notes / folder URLs that JobView's header buttons
//! open in the system browser. Full settings UI lands later.

use std::sync::{Arc, OnceLock, RwLock};
use ufb_core::settings::AppSettings;

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
        type Settings = super::SettingsRust;

        /// Build the Google Drive Apps Script URL for a job's notes
        /// document or notes folder. Returns "" if either the
        /// scriptUrl or parentFolderId setting is unset (caller
        /// should display a "configure in settings" prompt).
        ///
        /// `mode` is "doc" or "folder" — matches the Apps Script's
        /// expected query parameter.
        #[qinvokable]
        fn build_notes_url(
            self: &Settings,
            job_name: QString,
            mode: QString,
        ) -> QString;

        /// True when both scriptUrl and parentFolderId are populated.
        /// JobView uses this to gate visibility / enabled state of
        /// the notes / folder buttons.
        #[qinvokable]
        fn has_google_drive(self: &Settings) -> bool;

        /// Read-only accessors for the Google Drive config dialog.
        #[qinvokable]
        fn google_drive_script_url(self: &Settings) -> QString;
        #[qinvokable]
        fn google_drive_parent_folder_id(self: &Settings) -> QString;

        /// Persist the Google Drive Apps Script URL + parent folder
        /// ID. Writes to settings.json. Returns "" on success or an
        /// error message.
        #[qinvokable]
        fn set_google_drive(
            self: &Settings,
            script_url: QString,
            parent_folder_id: QString,
        ) -> QString;

        // ── Mesh sync ───────────────────────────────────────────────
        // Backing settings live in settings.json under `meshSync`.
        // The Mesh QObject reads these on init / re-init.

        #[qinvokable]
        fn mesh_enabled(self: &Settings) -> bool;
        #[qinvokable]
        fn mesh_farm_path(self: &Settings) -> QString;
        #[qinvokable]
        fn mesh_node_id(self: &Settings) -> QString;
        /// Comma-separated node-tag list (used by peer filtering /
        /// targeting in the mesh - e.g. "render,gpu" tags a render
        /// node, "client" tags a workstation). Stored verbatim;
        /// downstream split + trim happens in the coordinator.
        #[qinvokable]
        fn mesh_tags(self: &Settings) -> QString;
        /// Persist mesh settings + return "" on success / error msg.
        #[qinvokable]
        fn set_mesh(
            self: &Settings,
            enabled: bool,
            farm_path: QString,
            node_id: QString,
            tags: QString,
        ) -> QString;

        // ── Path swaps ──────────────────────────────────────────────
        // Cross-OS prefix mappings used to translate ufb:// links
        // when a Windows-saved path needs opening on a Mac (or v.v.).
        // Stored under `pathMappings` in settings.json. Each row has
        // {label, win, mac, enabled}.

        /// JSON array of all PathMappings as `[{label, win, mac, enabled}, …]`.
        #[qinvokable]
        fn path_mappings_json(self: &Settings) -> QString;

        /// Number of mappings currently flagged enabled. The sidebar
        /// uses this to show an "Active / None added" status indicator
        /// without parsing the full JSON.
        #[qinvokable]
        fn path_mappings_active_count(self: &Settings) -> i32;

        /// Persist a JSON array of path mappings. Same shape as
        /// path_mappings_json. Returns "" on success / error msg.
        #[qinvokable]
        fn set_path_mappings(self: &Settings, json: QString) -> QString;

        // ── Window geometry ────────────────────────────────────────
        // Persisted under `window` in settings.json (already on the
        // AppSettings struct - we're just exposing it). Restored at
        // launch by Main.qml; saved (debounced) on resize/move.

        /// JSON {x, y, width, height, maximized}. Width/height are
        /// always meaningful; x/y are -1 when never positioned (let
        /// the OS place the window) and otherwise the saved screen
        /// coordinates.
        #[qinvokable]
        fn window_state_json(self: &Settings) -> QString;

        /// Persist window geometry. Returns "" on success / error msg.
        #[qinvokable]
        fn set_window_state(
            self: &Settings,
            x: i32,
            y: i32,
            width: i32,
            height: i32,
            maximized: bool,
        ) -> QString;

        // ── Tab strip ──────────────────────────────────────────────
        // Persisted under `tabState`. Captures Files + job tabs +
        // tracker/transcode open state + the current index. Replayed
        // by Main.qml at launch; saved (debounced) on tab/path
        // changes.

        /// JSON {currentIndex, files: [{leftPath, rightPath,
        /// leftCollapsed, rightCollapsed}, ...], jobs: [{jobPath}],
        /// trackerOpen, transcodeOpen}.
        #[qinvokable]
        fn tab_state_json(self: &Settings) -> QString;

        /// Persist tab state. Same JSON shape as the getter.
        /// Returns "" on success / error msg.
        #[qinvokable]
        fn set_tab_state(self: &Settings, json: QString) -> QString;

        // ── Splitter state ─────────────────────────────────────────
        // Two persisted dimensions: the outer sidebar/content split
        // and the inner DualBrowserView L/R split (shared across
        // every Files tab per Option-A design). Returned as plain
        // integers - 0 = "use UI default".

        #[qinvokable]
        fn outer_sidebar_width(self: &Settings) -> i32;
        #[qinvokable]
        fn set_outer_sidebar_width(self: &Settings, width: i32) -> QString;

        #[qinvokable]
        fn inner_left_width(self: &Settings) -> i32;
        #[qinvokable]
        fn set_inner_left_width(self: &Settings, width: i32) -> QString;

        // ── Per-view memory (the ONE browser view-state store) ─────
        // View mode, grid size, sort, and column widths are per-VIEW,
        // stored as JSON blobs in the shared SQLite DB
        // (browser_folder_prefs). Keys are identity-spelled folder
        // paths (survive drive-letter changes / mount renames) plus
        // reserved "view:*" keys for singleton tables (aggregated
        // tracker etc.). Consumers: FileBrowser (viewMode/gridSize/
        // sort/nameColWidth/sizeColWidth), ItemListPanel
        // (ilpNameColWidth), TrackerView (trk*W), JobView
        // (activeSubtab). Writers MERGE (read-modify-write) — several
        // components share one row. Missing entry = INHERIT current
        // state, never reset. The legacy settings.json browser_panel
        // globals + per-pane widths were deleted 2026-07-11.

        /// Read a view's prefs JSON ("{}" when none saved).
        #[qinvokable]
        fn folder_view_prefs(self: &Settings, path: QString) -> QString;
        /// Upsert a view's prefs (JSON object). Empty path: no-op.
        #[qinvokable]
        fn set_folder_view_prefs(self: &Settings, path: QString, prefs_json: QString);

        // ── Preview lightbox preferences ───────────────────────────
        // Loop video playback in the in-app preview; remembered across
        // previews and launches.
        #[qinvokable]
        fn preview_loop(self: &Settings) -> bool;
        #[qinvokable]
        fn set_preview_loop(self: &Settings, on: bool) -> QString;
    }
}

#[derive(Default)]
pub struct SettingsRust;

pub(crate) fn shared_settings() -> Arc<RwLock<AppSettings>> {
    static MGR: OnceLock<Arc<RwLock<AppSettings>>> = OnceLock::new();
    MGR.get_or_init(|| Arc::new(RwLock::new(AppSettings::load())))
        .clone()
}

/// Refresh the in-memory cache from disk. Other QObjects (Mesh in
/// particular) call AppSettings::load() directly on demand instead
/// of going through `shared_settings()`, which means any field they
/// write while the app is running gets out of sync with our cache.
/// Callers that are about to mutate-and-save should refresh first
/// to avoid clobbering those independent writes.
fn refresh_cache_from_disk() -> Arc<RwLock<AppSettings>> {
    let arc = shared_settings();
    {
        let mut s = arc.write().unwrap();
        *s = AppSettings::load();
    }
    arc
}

/// DB key for per-folder browser prefs: identity-spelled (survives
/// drive-letter changes / mount renames — same treatment the Columns
/// service gives job paths), trailing separators trimmed, lowercased
/// (Windows paths are case-insensitive and macOS defaults that way).
fn folder_prefs_key(path: &str) -> Option<String> {
    let trimmed = path.trim().trim_end_matches(['/', '\\']);
    if trimmed.is_empty() {
        return None;
    }
    let mappings = AppSettings::load().path_mappings;
    Some(ufb_core::utils::to_identity_storage(trimmed, &mappings).to_lowercase())
}

/// One-time schema for the per-folder prefs table (shared DB).
fn ensure_folder_prefs_table(db: &ufb_core::db::Database) {
    static ONCE: OnceLock<()> = OnceLock::new();
    ONCE.get_or_init(|| {
        let res = db.with_conn(|conn| {
            conn.execute(
                "CREATE TABLE IF NOT EXISTS browser_folder_prefs (
                     path       TEXT PRIMARY KEY,
                     prefs      TEXT NOT NULL,
                     updated_at INTEGER NOT NULL
                 )",
                [],
            )?;
            Ok(())
        });
        if let Err(e) = res {
            log::warn!("settings: browser_folder_prefs table init failed: {}", e);
        }
    });
}

impl qobject::Settings {
    fn build_notes_url(
        self: &qobject::Settings,
        job_name: cxx_qt_lib::QString,
        mode: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        let script = s.google_drive.script_url.trim();
        let parent = s.google_drive.parent_folder_id.trim();
        if script.is_empty() || parent.is_empty() {
            return cxx_qt_lib::QString::from("");
        }
        let url = format!(
            "{}?job={}&parent={}&mode={}",
            script,
            urlencode(&job_name.to_string()),
            urlencode(parent),
            urlencode(&mode.to_string())
        );
        cxx_qt_lib::QString::from(&url)
    }

    fn has_google_drive(self: &qobject::Settings) -> bool {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        !s.google_drive.script_url.trim().is_empty()
            && !s.google_drive.parent_folder_id.trim().is_empty()
    }

    fn google_drive_script_url(self: &qobject::Settings) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        cxx_qt_lib::QString::from(&s.google_drive.script_url)
    }

    fn google_drive_parent_folder_id(self: &qobject::Settings) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        cxx_qt_lib::QString::from(&s.google_drive.parent_folder_id)
    }

    fn set_google_drive(
        self: &qobject::Settings,
        script_url: cxx_qt_lib::QString,
        parent_folder_id: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let settings = refresh_cache_from_disk();
        let mut s = settings.write().unwrap();
        s.google_drive.script_url = script_url.to_string();
        s.google_drive.parent_folder_id = parent_folder_id.to_string();
        match s.save() {
            Ok(()) => {
                log::info!("settings: google_drive updated");
                cxx_qt_lib::QString::from("")
            }
            Err(e) => {
                log::warn!("settings: google_drive save failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn mesh_enabled(self: &qobject::Settings) -> bool {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        s.mesh_sync.enabled
    }
    fn mesh_farm_path(self: &qobject::Settings) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        cxx_qt_lib::QString::from(&s.mesh_sync.farm_path)
    }
    fn mesh_node_id(self: &qobject::Settings) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        cxx_qt_lib::QString::from(&s.mesh_sync.node_id)
    }
    fn mesh_tags(self: &qobject::Settings) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        cxx_qt_lib::QString::from(&s.mesh_sync.tags)
    }
    fn set_mesh(
        self: &qobject::Settings,
        enabled: bool,
        farm_path: cxx_qt_lib::QString,
        node_id: cxx_qt_lib::QString,
        tags: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let settings = refresh_cache_from_disk();
        let mut s = settings.write().unwrap();
        s.mesh_sync.enabled = enabled;
        s.mesh_sync.farm_path = farm_path.to_string();
        s.mesh_sync.node_id = node_id.to_string();
        s.mesh_sync.tags = tags.to_string();
        match s.save() {
            Ok(()) => {
                log::info!("settings: mesh updated (enabled={})", enabled);
                cxx_qt_lib::QString::from("")
            }
            Err(e) => {
                log::warn!("settings: mesh save failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn path_mappings_json(self: &qobject::Settings) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        let json = serde_json::to_string(&s.path_mappings)
            .unwrap_or_else(|_| "[]".into());
        cxx_qt_lib::QString::from(&json)
    }

    fn path_mappings_active_count(self: &qobject::Settings) -> i32 {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        s.path_mappings.iter().filter(|m| m.enabled).count() as i32
    }

    fn window_state_json(self: &qobject::Settings) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        let json = serde_json::json!({
            "x": s.window.x,
            "y": s.window.y,
            "width": s.window.width,
            "height": s.window.height,
            "maximized": s.window.maximized,
        });
        cxx_qt_lib::QString::from(&json.to_string())
    }

    fn set_window_state(
        self: &qobject::Settings,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        maximized: bool,
    ) -> cxx_qt_lib::QString {
        // Width/height come in as i32 from QML's int but the settings
        // struct stores u32. Clamp so negative bogus values don't end
        // up wrapping to giant uints on disk.
        let w = width.max(0) as u32;
        let h = height.max(0) as u32;
        let settings = refresh_cache_from_disk();
        let mut s = settings.write().unwrap();
        s.window.x = x;
        s.window.y = y;
        s.window.width = w;
        s.window.height = h;
        s.window.maximized = maximized;
        match s.save() {
            Ok(()) => cxx_qt_lib::QString::from(""),
            Err(e) => {
                log::warn!("settings: window save failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn tab_state_json(self: &qobject::Settings) -> cxx_qt_lib::QString {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        let json = serde_json::to_string(&s.tab_state)
            .unwrap_or_else(|_| "{}".into());
        cxx_qt_lib::QString::from(&json)
    }

    fn set_tab_state(
        self: &qobject::Settings,
        json: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let parsed: Result<ufb_core::settings::TabState, _> =
            serde_json::from_str(&json.to_string());
        let tab_state = match parsed {
            Ok(t) => t,
            Err(e) => {
                let msg = format!("tab-state parse failed: {}", e);
                log::warn!("settings: {}", msg);
                return cxx_qt_lib::QString::from(&msg);
            }
        };
        let settings = refresh_cache_from_disk();
        let mut s = settings.write().unwrap();
        s.tab_state = tab_state;
        match s.save() {
            Ok(()) => cxx_qt_lib::QString::from(""),
            Err(e) => {
                log::warn!("settings: tab_state save failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn outer_sidebar_width(self: &qobject::Settings) -> i32 {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        s.splitter_state.outer_sidebar_width
    }

    fn set_outer_sidebar_width(
        self: &qobject::Settings,
        width: i32,
    ) -> cxx_qt_lib::QString {
        let settings = refresh_cache_from_disk();
        let mut s = settings.write().unwrap();
        s.splitter_state.outer_sidebar_width = width.max(0);
        match s.save() {
            Ok(()) => cxx_qt_lib::QString::from(""),
            Err(e) => {
                log::warn!("settings: outer_sidebar_width save failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn inner_left_width(self: &qobject::Settings) -> i32 {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        s.splitter_state.inner_left_width
    }

    fn set_inner_left_width(
        self: &qobject::Settings,
        width: i32,
    ) -> cxx_qt_lib::QString {
        let settings = refresh_cache_from_disk();
        let mut s = settings.write().unwrap();
        s.splitter_state.inner_left_width = width.max(0);
        match s.save() {
            Ok(()) => cxx_qt_lib::QString::from(""),
            Err(e) => {
                log::warn!("settings: inner_left_width save failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    // browser_view_mode / browser_grid_size / browser_*_col_width
    // deleted (2026-07-11): browser view state — view mode, grid
    // size, sort, column widths — is per-FOLDER now, in the
    // browser_folder_prefs store below. The settings.json
    // browser_panel block is gone with them.

    fn folder_view_prefs(
        self: &qobject::Settings,
        path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let Some(key) = folder_prefs_key(&path.to_string()) else {
            return cxx_qt_lib::QString::from("{}");
        };
        let Some(db) = crate::db::shared_db() else {
            return cxx_qt_lib::QString::from("{}");
        };
        ensure_folder_prefs_table(&db);
        let prefs: Option<String> = db
            .with_conn(|conn| {
                use rusqlite::OptionalExtension;
                conn.query_row(
                    "SELECT prefs FROM browser_folder_prefs WHERE path = ?1",
                    rusqlite::params![key],
                    |r| r.get(0),
                )
                .optional()
            })
            .ok()
            .flatten();
        cxx_qt_lib::QString::from(prefs.as_deref().unwrap_or("{}"))
    }

    fn set_folder_view_prefs(
        self: &qobject::Settings,
        path: cxx_qt_lib::QString,
        prefs_json: cxx_qt_lib::QString,
    ) {
        let Some(key) = folder_prefs_key(&path.to_string()) else { return };
        let prefs = prefs_json.to_string();
        // Only store well-formed JSON objects — a malformed write here
        // would poison every future read for the folder.
        if serde_json::from_str::<serde_json::Map<String, serde_json::Value>>(&prefs).is_err() {
            log::warn!("settings: rejecting malformed folder prefs for {}", key);
            return;
        }
        let Some(db) = crate::db::shared_db() else { return };
        ensure_folder_prefs_table(&db);
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0);
        let res = db.with_conn(|conn| {
            conn.execute(
                "INSERT INTO browser_folder_prefs (path, prefs, updated_at)
                 VALUES (?1, ?2, ?3)
                 ON CONFLICT(path) DO UPDATE SET prefs = ?2, updated_at = ?3",
                rusqlite::params![key, prefs, now],
            )?;
            // Occasional prune: keep the newest 2000 entries. Cheap
            // (~1/64 of writes) and bounds an otherwise per-folder-
            // unbounded table.
            if now % 64 == 0 {
                conn.execute(
                    "DELETE FROM browser_folder_prefs WHERE path NOT IN (
                         SELECT path FROM browser_folder_prefs
                         ORDER BY updated_at DESC LIMIT 2000)",
                    [],
                )?;
            }
            Ok(())
        });
        if let Err(e) = res {
            log::warn!("settings: folder prefs write failed: {}", e);
        }
    }

    fn preview_loop(self: &qobject::Settings) -> bool {
        let settings = shared_settings();
        let s = settings.read().unwrap();
        s.preview_loop
    }

    fn set_preview_loop(
        self: &qobject::Settings,
        on: bool,
    ) -> cxx_qt_lib::QString {
        let settings = refresh_cache_from_disk();
        let mut s = settings.write().unwrap();
        s.preview_loop = on;
        match s.save() {
            Ok(()) => cxx_qt_lib::QString::from(""),
            Err(e) => {
                log::warn!("settings: preview_loop save failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn set_path_mappings(
        self: &qobject::Settings,
        json: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let parsed: Result<Vec<ufb_core::settings::PathMapping>, _> =
            serde_json::from_str(&json.to_string());
        let mappings = match parsed {
            Ok(m) => m,
            Err(e) => {
                let msg = format!("path-mappings parse failed: {}", e);
                log::warn!("settings: {}", msg);
                return cxx_qt_lib::QString::from(&msg);
            }
        };
        let settings = refresh_cache_from_disk();
        let mut s = settings.write().unwrap();
        s.path_mappings = mappings;
        match s.save() {
            Ok(()) => {
                log::info!(
                    "settings: path_mappings updated ({} rows)",
                    s.path_mappings.len()
                );
                cxx_qt_lib::QString::from("")
            }
            Err(e) => {
                log::warn!("settings: path_mappings save failed: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }
}

/// Minimal URL component encoder for the small set of characters
/// likely to appear in job names / folder IDs (spaces, ampersands,
/// equals signs). Avoids pulling a full url-encoding crate for one
/// callsite.
fn urlencode(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char);
            }
            _ => {
                out.push_str(&format!("%{:02X}", b));
            }
        }
    }
    out
}
