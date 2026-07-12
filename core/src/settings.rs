use serde::{Deserialize, Serialize};

// Mesh-sync ports re-exported through `crate::mesh` (the canonical source).
use crate::mesh::{DEFAULT_HTTP_CONTROL_PORT, DEFAULT_HTTP_DATA_PORT};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WindowState {
    #[serde(default = "default_neg_one")]
    pub x: i32,
    #[serde(default = "default_neg_one")]
    pub y: i32,
    #[serde(default = "default_width")]
    pub width: u32,
    #[serde(default = "default_height")]
    pub height: u32,
    #[serde(default)]
    pub maximized: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PanelVisibility {
    #[serde(default = "default_true", alias = "show_subscriptions")]
    pub show_subscriptions: bool,
    #[serde(default = "default_true", alias = "show_browser1")]
    pub show_browser1: bool,
    #[serde(default = "default_true", alias = "show_browser2")]
    pub show_browser2: bool,
    #[serde(default, alias = "show_transcode_queue")]
    pub show_transcode_queue: bool,
    #[serde(default = "default_true", alias = "use_windows_accent")]
    pub use_windows_accent: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AppearanceSettings {
    #[serde(default = "default_true", alias = "use_windows_accent_color")]
    pub use_windows_accent_color: bool,
    #[serde(default = "default_neg_one_i32", alias = "custom_accent_color_index")]
    pub custom_accent_color_index: i32,
    #[serde(default = "default_half", alias = "custom_picker_color_r")]
    pub custom_picker_color_r: f32,
    #[serde(default = "default_half", alias = "custom_picker_color_g")]
    pub custom_picker_color_g: f32,
    #[serde(default = "default_half", alias = "custom_picker_color_b")]
    pub custom_picker_color_b: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BrowserColumnVisibility {
    #[serde(default = "default_true")]
    pub size: bool,
    #[serde(default = "default_true")]
    pub modified: bool,
    #[serde(default = "default_true", alias = "r#type")]
    pub r#type: bool,
}

impl Default for BrowserColumnVisibility {
    fn default() -> Self {
        Self {
            size: true,
            modified: true,
            r#type: true,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UiSettings {
    #[serde(default = "default_one_f32", alias = "font_scale")]
    pub font_scale: f32,
    #[serde(default = "default_panel_ratios", alias = "browser_panel_ratios")]
    pub browser_panel_ratios: Vec<f32>,
    #[serde(default)]
    pub browser_columns: BrowserColumnVisibility,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MeshSyncSettings {
    #[serde(default, alias = "node_id")]
    pub node_id: String,
    #[serde(default, alias = "farm_path")]
    pub farm_path: String,
    /// Control-plane HTTP port (`/api/status`).
    #[serde(default = "default_http_port", alias = "http_port")]
    pub http_port: u16,
    /// Data-plane HTTP port (`/api/metadata/*`, `/api/table/*`, `/api/snapshot/notify`).
    #[serde(default = "default_data_port", alias = "data_port")]
    pub data_port: u16,
    #[serde(default)]
    pub tags: String,
    /// User-toggled "start mesh sync at app launch" flag. Persisted
    /// here so toggling from the Settings UI survives a restart.
    /// Default false — explicit opt-in.
    #[serde(default)]
    pub enabled: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GoogleDriveSettings {
    #[serde(default, alias = "script_url")]
    pub script_url: String,
    #[serde(default, alias = "parent_folder_id")]
    pub parent_folder_id: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SyncSettings {
    #[serde(default)]
    pub enabled: bool,
}

/// A path mapping rule for cross-OS URI translation.
/// Each pair maps equivalent paths across Windows and macOS.
/// Mappings can be toggled on/off and labeled for roaming users who switch locations.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PathMapping {
    #[serde(default)]
    pub win: String,
    #[serde(default)]
    pub mac: String,
    #[serde(default = "default_true")]
    pub enabled: bool,
    #[serde(default)]
    pub label: String,
}

impl Default for PathMapping {
    fn default() -> Self {
        Self {
            win: String::new(),
            mac: String::new(),
            enabled: true,
            label: String::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct JobViewState {
    #[serde(alias = "job_path")]
    pub job_path: String,
    #[serde(alias = "job_name")]
    pub job_name: String,
}

/// Per-Files-tab restoration data. Captures both pane paths and
/// their collapsed state so a saved-and-reopened tab looks the same
/// as the user left it.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FilesTabState {
    #[serde(default)]
    pub left_path: String,
    #[serde(default)]
    pub right_path: String,
    #[serde(default)]
    pub left_collapsed: bool,
    #[serde(default)]
    pub right_collapsed: bool,
}

/// Persisted job-tab entry. Only the path is stored; the display
/// name is re-derived from the subscription metadata at restore
/// time. If the source folder has been deleted the tab is silently
/// skipped on the next launch.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct JobTabState {
    #[serde(default)]
    pub job_path: String,
}

/// SplitView positions persisted across launches. Per the
/// Option-A persistence design, the inner DualBrowserView L/R
/// width is a SHARED default applied to every Files tab on
/// construction - tabs aren't individually configurable workspaces.
///
/// `0` for either field means "use UI default" (220px sidebar,
/// 50/50 inner split).
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SplitterState {
    #[serde(default)]
    pub outer_sidebar_width: i32,
    #[serde(default)]
    pub inner_left_width: i32,
}

// BrowserPanelState / PaneColWidths deleted (2026-07-11): browser
// view state (view mode, grid size, sort, column widths) is per-VIEW
// now, stored in the shared SQLite DB's browser_folder_prefs table
// (see bindings/src/services/settings.rs). Stale `browserPanel` keys
// in existing settings.json files are ignored on load and dropped on
// the next save.

/// Top-level tab restoration state. Held on `AppSettings.tab_state`
/// and replayed on launch by Main.qml.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TabState {
    #[serde(default)]
    pub current_index: i32,
    /// At least one Files tab is always present at runtime; an
    /// empty Vec on disk just means "first launch", restored as a
    /// single home-dir Files tab.
    #[serde(default)]
    pub files: Vec<FilesTabState>,
    #[serde(default)]
    pub jobs: Vec<JobTabState>,
    #[serde(default)]
    pub tracker_open: bool,
    #[serde(default)]
    pub transcode_open: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AppSettings {
    #[serde(default)]
    pub window: WindowState,
    #[serde(default)]
    pub panels: PanelVisibility,
    #[serde(default)]
    pub appearance: AppearanceSettings,
    #[serde(default)]
    pub ui: UiSettings,
    #[serde(default)]
    pub sync: SyncSettings,
    #[serde(default, alias = "mesh_sync")]
    pub mesh_sync: MeshSyncSettings,
    #[serde(default, alias = "google_drive")]
    pub google_drive: GoogleDriveSettings,
    #[serde(default, alias = "path_mappings")]
    pub path_mappings: Vec<PathMapping>,
    #[serde(default, alias = "job_views")]
    pub job_views: Vec<JobViewState>,
    #[serde(default, alias = "aggregated_tracker_open")]
    pub aggregated_tracker_open: bool,
    /// Persisted tab strip state - replayed in Main.qml at launch.
    /// See TabState for shape.
    #[serde(default, alias = "tab_state")]
    pub tab_state: TabState,
    #[serde(default, alias = "splitter_state")]
    pub splitter_state: SplitterState,
    /// One-shot flag — set to true after the v1 templates migration
    /// has copied any pre-existing rows from `column_presets` out
    /// to `<farm_root>/ufb/templates/<unique_name>.json`. The
    /// migration runs once per machine; subsequent launches see
    /// the flag set and skip the scan. See
    /// `core::templates::migrate_v1_legacy_presets`.
    #[serde(default, alias = "templates_migrated_v1")]
    pub templates_migrated_v1: bool,
    /// Phase 7 — set to true after the v2 inline-schema backfill
    /// has assigned a template_hash to every shared-folder column
    /// in `column_definitions`. The migration runs once per
    /// machine; subsequent launches skip the scan. Deferred (flag
    /// stays false) when the share is unreachable so the next
    /// launch retries.
    #[serde(default, alias = "templates_v2_migrated")]
    pub templates_v2_migrated: bool,
    /// Persisted user preference: loop video playback in the in-app
    /// preview lightbox. Off by default; toggled by the lightbox's
    /// loop button / V key and remembered across previews + launches.
    #[serde(default, alias = "preview_loop")]
    pub preview_loop: bool,
    /// Live-root overlay mode for path-identity resolution:
    /// "off" | "shadow" (default — resolve via static mappings, log
    /// disagreements with the live-mount answer) | "live" (resolve
    /// via agent-reported mount locations, mapping fallback when a
    /// share is unmounted). See plans/17 slice A.
    #[serde(
        default = "default_identity_live_roots",
        alias = "identity_live_roots"
    )]
    pub identity_live_roots: String,
}

fn default_identity_live_roots() -> String {
    // "live" since 1.0.7: both OSes ran live through 1.0.5/1.0.6 with
    // clean shadow logs first. "shadow"/"off" remain as escape hatches.
    "live".to_string()
}

impl Default for WindowState {
    fn default() -> Self {
        Self {
            x: -1,
            y: -1,
            width: 1914,
            height: 1060,
            maximized: false,
        }
    }
}

impl Default for PanelVisibility {
    fn default() -> Self {
        Self {
            show_subscriptions: true,
            show_browser1: true,
            show_browser2: true,
            show_transcode_queue: false,
            use_windows_accent: true,
        }
    }
}

impl Default for AppearanceSettings {
    fn default() -> Self {
        Self {
            use_windows_accent_color: true,
            custom_accent_color_index: -1,
            custom_picker_color_r: 0.5,
            custom_picker_color_g: 0.5,
            custom_picker_color_b: 0.5,
        }
    }
}

impl Default for UiSettings {
    fn default() -> Self {
        Self {
            font_scale: 1.0,
            browser_panel_ratios: vec![0.20, 0.40, 0.40],
            browser_columns: BrowserColumnVisibility::default(),
        }
    }
}

impl Default for AppSettings {
    fn default() -> Self {
        Self {
            window: WindowState::default(),
            panels: PanelVisibility::default(),
            appearance: AppearanceSettings::default(),
            ui: UiSettings::default(),
            sync: SyncSettings::default(),
            mesh_sync: MeshSyncSettings::default(),
            google_drive: GoogleDriveSettings::default(),
            path_mappings: vec![],
            job_views: vec![],
            aggregated_tracker_open: false,
            tab_state: TabState::default(),
            splitter_state: SplitterState::default(),
            templates_migrated_v1: false,
            templates_v2_migrated: false,
            preview_loop: false,
            identity_live_roots: default_identity_live_roots(),
        }
    }
}

impl AppSettings {
    /// Load settings from disk, falling back to defaults.
    ///
    /// Tries `settings.json` first; on parse-failure (corruption,
    /// half-written file from a power loss mid-save) falls back to
    /// `settings.json.bak`. Only returns `Self::default()` when both
    /// files are missing or unreadable.
    pub fn load() -> Self {
        let path = crate::utils::get_settings_path();
        let bak_path = path.with_extension("json.bak");

        let mut settings: Self =
            if let Some(s) = Self::try_read_from(&path, "main") {
                s
            } else if let Some(s) = Self::try_read_from(&bak_path, "backup") {
                log::warn!(
                    "Settings: main file unreadable; restored from backup ({})",
                    bak_path.display()
                );
                s
            } else {
                Self::default()
            };
        let home_changed = settings.collapse_home_in_mappings();
        let ports_changed = settings.migrate_stale_ports();
        if home_changed || ports_changed {
            let _ = settings.save();
        }
        settings
    }

    fn try_read_from(p: &std::path::Path, label: &str) -> Option<Self> {
        if !p.exists() {
            return None;
        }
        match std::fs::read_to_string(p) {
            Ok(content) => match serde_json::from_str::<Self>(&content) {
                Ok(s) => Some(s),
                Err(e) => {
                    log::warn!(
                        "Settings: parse failed for {} ({}): {}",
                        label,
                        p.display(),
                        e
                    );
                    None
                }
            },
            Err(e) => {
                log::warn!(
                    "Settings: read failed for {} ({}): {}",
                    label,
                    p.display(),
                    e
                );
                None
            }
        }
    }

    /// One-shot migration from the pre-Wave-A port layout (single HTTP
    /// port 49200) to the split layout (control 49201 + data 49202).
    fn migrate_stale_ports(&mut self) -> bool {
        let mut changed = false;
        if self.mesh_sync.http_port == 49200 {
            self.mesh_sync.http_port = DEFAULT_HTTP_CONTROL_PORT;
            changed = true;
            log::info!(
                "Settings migration: mesh http_port 49200 → {}",
                DEFAULT_HTTP_CONTROL_PORT
            );
        }
        if self.mesh_sync.data_port == 0 {
            self.mesh_sync.data_port = DEFAULT_HTTP_DATA_PORT;
            changed = true;
        }
        // v5 epoch: move the v4-default ports (49201 control / 49202 data)
        // to the v5 defaults so an upgraded install stops listening on the
        // old epoch's ports. Only the OLD DEFAULTS are moved — a user who
        // deliberately picked some other port is left alone.
        if self.mesh_sync.http_port == 49201 {
            self.mesh_sync.http_port = DEFAULT_HTTP_CONTROL_PORT;
            changed = true;
            log::info!(
                "Settings migration: mesh http_port 49201 → {} (v5 epoch)",
                DEFAULT_HTTP_CONTROL_PORT
            );
        }
        if self.mesh_sync.data_port == 49202 {
            self.mesh_sync.data_port = DEFAULT_HTTP_DATA_PORT;
            changed = true;
            log::info!(
                "Settings migration: mesh data_port 49202 → {} (v5 epoch)",
                DEFAULT_HTTP_DATA_PORT
            );
        }
        changed
    }

    /// Save settings to disk atomically with one rolling backup.
    ///
    /// 1. Serialize to `settings.json.tmp` and `fsync` so the bytes
    ///    are durable across a power loss before we touch anything
    ///    the loader will read.
    /// 2. Rotate any existing `settings.json` to `settings.json.bak`,
    ///    overwriting a prior backup. Best-effort - if rotation fails
    ///    (locked, permission, etc.) we still try to land the new
    ///    main file rather than abort.
    /// 3. Atomic rename `settings.json.tmp` -> `settings.json`. On
    ///    Windows this is `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`
    ///    semantics; on Unix it's `rename(2)`.
    ///
    /// Worst-case interruption windows:
    /// - Power loss between step 1 and 2: `.tmp` exists with the new
    ///   data, `.bak` is whatever it was before, main is the old one.
    ///   Loader reads the old main fine.
    /// - Power loss between step 2 and 3: `.tmp` has the new data,
    ///   `.bak` has the old data, main is missing. Loader falls
    ///   through to `.bak`.
    /// - Power loss during step 3: rename is atomic on the supported
    ///   platforms; either the new main is in place or it isn't.
    pub fn save(&self) -> Result<(), String> {
        use std::io::Write;
        let path = crate::utils::get_settings_path();
        let tmp_path = path.with_extension("json.tmp");
        let bak_path = path.with_extension("json.bak");

        let json = serde_json::to_string_pretty(self)
            .map_err(|e| format!("Failed to serialize settings: {}", e))?;

        // Step 1: write + fsync the temp file before touching anything else.
        {
            let mut file = std::fs::File::create(&tmp_path).map_err(|e| {
                format!(
                    "Failed to create temp settings file ({}): {}",
                    tmp_path.display(),
                    e
                )
            })?;
            file.write_all(json.as_bytes()).map_err(|e| {
                format!("Failed to write temp settings: {}", e)
            })?;
            // Best-effort fsync - on some Windows filesystems sync_all
            // is a no-op, but where it's honored it gives us durability.
            let _ = file.sync_all();
        }

        // Step 2: rotate the existing main file to .bak. Best-effort -
        // on first save the main file doesn't exist yet; on a Windows
        // antivirus lock it may transiently fail. Either case is safe
        // to fall through.
        if path.exists() {
            if let Err(e) = std::fs::rename(&path, &bak_path) {
                log::warn!(
                    "Settings: backup rotation failed ({} -> {}): {}",
                    path.display(),
                    bak_path.display(),
                    e
                );
            }
        }

        // Step 3: atomic publish.
        std::fs::rename(&tmp_path, &path).map_err(|e| {
            format!(
                "Failed to commit settings ({} -> {}): {}",
                tmp_path.display(),
                path.display(),
                e
            )
        })
    }

    /// One-shot migration: rewrite stored `/Users/<me>/...` (mac) prefixes
    /// in path mappings to `~/...` so the settings file is portable across
    /// machines with different usernames.
    #[cfg(target_os = "macos")]
    fn collapse_home_in_mappings(&mut self) -> bool {
        let Some(home) = dirs::home_dir() else {
            return false;
        };
        let home_str = home.to_string_lossy().into_owned();
        if home_str.is_empty() {
            return false;
        }
        let prefix = format!("{}/", home_str);
        let mut changed = false;
        for m in self.path_mappings.iter_mut() {
            let field = &mut m.mac;

            if let Some(rest) = field.strip_prefix(&prefix) {
                *field = format!("~/{}", rest);
                changed = true;
            } else if field.as_str() == home_str {
                *field = "~".to_string();
                changed = true;
            }
        }
        changed
    }

    #[cfg(not(target_os = "macos"))]
    fn collapse_home_in_mappings(&mut self) -> bool {
        false
    }
}

// Default value helpers for serde
fn default_neg_one() -> i32 {
    -1
}
fn default_neg_one_i32() -> i32 {
    -1
}
fn default_width() -> u32 {
    1914
}
fn default_height() -> u32 {
    1060
}
fn default_true() -> bool {
    true
}
fn default_half() -> f32 {
    0.5
}
fn default_one_f32() -> f32 {
    1.0
}
fn default_panel_ratios() -> Vec<f32> {
    vec![0.20, 0.40, 0.40]
}
fn default_http_port() -> u16 {
    DEFAULT_HTTP_CONTROL_PORT
}

fn default_data_port() -> u16 {
    DEFAULT_HTTP_DATA_PORT
}
