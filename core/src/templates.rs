//! Filesystem-backed shared-column-setup storage. Two coexisting
//! systems with separate paths and roles:
//!
//! - **v1 presets** at `<farm>/ufb/presets/<slug>.json` — the
//!   user-curated, named, manually-saved setups. Cross-project.
//!   Editable. The flat functions in this module (top of file) are
//!   the v1 surface.
//! - **v2 templates** at `<farm>/ufb/templates/<uuid>.json` — the
//!   auto-promoted, hash-keyed, project-scoped column setups that
//!   every column gets a copy of. Live by way of `mod v2`. UI never
//!   surfaces these as a "templates library" — they exist as the
//!   sync substrate for the lookup-on-create flow.
//!
//! Why two systems instead of one: presets are stable bookmarks the
//! user explicitly curates ("save the QC Workflow Status v3"), while
//! templates are the sync mechanism for "every column anyone makes is
//! instantly available to the team." Different lifetimes, different
//! UX, different storage shapes — collapsing them muddied both.
//!
//! Both systems pre-date the mesh-broadcast preset_save flow that
//! used to fail in practice (snapshot/restore loop made follower
//! saves silently lose to leader-written snapshots). Filesystem-as-
//! manifest sidesteps that entirely.
//!
//! ## v1 / v2 path migration
//!
//! v1 originally lived at `<farm>/ufb/templates/`. v2 takes that path,
//! v1 moves to `<farm>/ufb/presets/`. `migrate_legacy_preset_path()`
//! does the one-shot move on first call. Idempotent.
//!
//! ## Concurrency (v1)
//!
//! Writes are atomic via tempfile + rename. Two users editing the
//! same preset simultaneously: the second `rename()` to land wins,
//! the first writer's edits are lost silently. We accept that as a
//! rare coordination-by-humans concern; v2 adds explicit revision-CAS
//! for the same-template case where atomicity isn't enough.

use crate::columns::PresetColumnDef;
use serde::{Deserialize, Serialize};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

const SCHEMA_VERSION: u32 = 1;
/// v1 preset path. Renamed from `ufb/templates` so v2 can take that
/// path. `migrate_legacy_preset_path()` moves any pre-existing files
/// from the old location.
const PRESETS_SUBDIR: &str = "ufb/presets";
const LEGACY_PRESETS_SUBDIR: &str = "ufb/templates";

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TemplateFile {
    pub schema_version: u32,
    pub unique_name: String,
    pub display_name: String,
    pub created_by: String,
    pub created_at: i64,
    pub updated_at: i64,
    pub data: PresetColumnDef,
}

#[derive(Debug, thiserror::Error)]
pub enum TemplateError {
    #[error("templates directory unavailable: {0}")]
    DirUnavailable(String),
    #[error("template not found: {0}")]
    NotFound(String),
    #[error("template name collision: '{display_name}' already exists at {existing_path}")]
    NameCollision {
        display_name: String,
        existing_path: String,
    },
    #[error("template parse failed: {0}")]
    ParseError(String),
    /// v2 only: revision-CAS detected concurrent edit. Caller is
    /// expected to re-read the file and surface a "reload?" dialog.
    #[error("revision mismatch: expected {expected}, found {found}")]
    RevisionMismatch { expected: u32, found: u32 },
    #[error("filesystem error: {0}")]
    Io(#[from] std::io::Error),
    #[error("serde error: {0}")]
    Serde(#[from] serde_json::Error),
}

pub type Result<T> = std::result::Result<T, TemplateError>;

/// Slug a display name to a safe filename stem. Lowercase, alnum +
/// underscore only, max 64 chars.
pub fn slug_for(display_name: &str) -> String {
    let s: String = display_name
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() {
                c.to_ascii_lowercase()
            } else {
                '_'
            }
        })
        .collect();
    // Collapse runs of underscores + trim.
    let mut out = String::with_capacity(s.len());
    let mut prev_under = false;
    for c in s.chars() {
        if c == '_' {
            if !prev_under {
                out.push(c);
            }
            prev_under = true;
        } else {
            out.push(c);
            prev_under = false;
        }
    }
    let trimmed = out.trim_matches('_').to_string();
    if trimmed.is_empty() {
        // All-special-char display names fall back to a placeholder.
        // The UI should reject these before they reach this layer.
        "untitled".into()
    } else if trimmed.len() > 64 {
        trimmed.chars().take(64).collect()
    } else {
        trimmed
    }
}

/// Resolve the v1 presets directory under `farm_root`. Creates the
/// `ufb/presets/` subtree on first call. Returns `DirUnavailable`
/// when `farm_root` is empty (mesh not configured) or unreachable.
///
/// Also runs the one-shot legacy-path migration that moves any files
/// from the old `ufb/templates/` location to the new `ufb/presets/`
/// location. Idempotent: skips files that already exist at the new
/// location.
pub fn resolve_dir(farm_root: &str) -> Result<PathBuf> {
    if farm_root.trim().is_empty() {
        return Err(TemplateError::DirUnavailable(
            "farm_root is empty — mesh sync not configured".into(),
        ));
    }
    let dir = Path::new(farm_root).join(PRESETS_SUBDIR);
    fs::create_dir_all(&dir)
        .map_err(|e| TemplateError::DirUnavailable(format!("{}: {}", dir.display(), e)))?;
    let _ = migrate_legacy_preset_path(farm_root, &dir);
    Ok(dir)
}

/// One-shot move of legacy v1 presets from `<farm>/ufb/templates/`
/// to `<farm>/ufb/presets/`. v2 templates ALSO live at
/// `<farm>/ufb/templates/`, so this migration is content-aware: it
/// only moves files whose JSON has `schemaVersion: 1`. v2 files
/// (schemaVersion: 2, UUID-named) and the v2 manifest (`_index.json`)
/// are left where they are.
///
/// Best-effort + idempotent: skips files that already exist at the
/// new location, logs + continues on individual parse / IO failures,
/// safe to run repeatedly.
fn migrate_legacy_preset_path(farm_root: &str, presets_dir: &Path) -> Result<()> {
    let legacy = Path::new(farm_root).join(LEGACY_PRESETS_SUBDIR);
    if !legacy.exists() {
        return Ok(());
    }
    let mut moved = 0usize;
    for entry in fs::read_dir(&legacy)? {
        let entry = entry?;
        let src = entry.path();
        let Some(name) = src.file_name().and_then(|s| s.to_str()) else {
            continue;
        };
        if !name.ends_with(".json") || name.ends_with(".json.tmp") {
            continue;
        }
        // Skip the v2 manifest by name — fast-path before the
        // schema-version parse below.
        if name == "_index.json" {
            continue;
        }
        // Read the file once + check schemaVersion. Files we can't
        // parse get conservatively skipped — the user can re-save
        // them through the preset UI if needed.
        let raw = match fs::read_to_string(&src) {
            Ok(s) => s,
            Err(e) => {
                log::warn!(
                    "templates: legacy migration read failed {}: {}",
                    src.display(),
                    e
                );
                continue;
            }
        };
        let schema_version = peek_schema_version(&raw);
        if schema_version != Some(1) {
            // schemaVersion: 2 → leave in place (v2 template file).
            // schemaVersion missing / other → conservatively skip.
            continue;
        }
        let dst = presets_dir.join(name);
        if dst.exists() {
            if let Err(e) = fs::remove_file(&src) {
                log::warn!(
                    "templates: legacy cleanup remove failed {}: {}",
                    src.display(),
                    e
                );
            }
            continue;
        }
        // Write + delete rather than rename — the legacy dir is the
        // v2 templates dir now, and a half-renamed file (rename
        // succeeded, source not yet deleted) would confuse v2's
        // listing.
        match fs::write(&dst, raw).and_then(|_| fs::remove_file(&src)) {
            Ok(()) => {
                moved += 1;
            }
            Err(e) => {
                log::warn!(
                    "templates: legacy migration failed for {}: {}",
                    src.display(),
                    e
                );
            }
        }
    }
    if moved > 0 {
        log::info!(
            "templates: migrated {} v1 preset(s) from {} to {}",
            moved,
            legacy.display(),
            presets_dir.display()
        );
    }
    Ok(())
}

/// Recover any v2 template files (schemaVersion: 2) that were
/// erroneously placed in the v1 presets dir by a buggy version of
/// `migrate_legacy_preset_path` (pre-fix it indiscriminately moved
/// every JSON, including v2 templates that v2 had just written).
///
/// Idempotent: runs on every `v2::resolve_dir` call but does
/// nothing when the presets dir holds only v1 files. Best-effort:
/// logs + continues on individual failures.
fn recover_misplaced_v2_files(farm_root: &str, templates_v2_dir: &Path) -> Result<()> {
    let presets_dir = Path::new(farm_root).join(PRESETS_SUBDIR);
    if !presets_dir.exists() {
        return Ok(());
    }
    let mut recovered = 0usize;
    for entry in fs::read_dir(&presets_dir)? {
        let entry = entry?;
        let src = entry.path();
        let Some(name) = src.file_name().and_then(|s| s.to_str()) else {
            continue;
        };
        if !name.ends_with(".json") || name.ends_with(".json.tmp") {
            continue;
        }
        let raw = match fs::read_to_string(&src) {
            Ok(s) => s,
            Err(_) => continue,
        };
        let schema_version = peek_schema_version(&raw);
        if schema_version != Some(2) && name != "_index.json" {
            continue;
        }
        let dst = templates_v2_dir.join(name);
        if dst.exists() {
            // v2 already has this file (or the manifest) at the
            // correct location. Just clean up the duplicate.
            if let Err(e) = fs::remove_file(&src) {
                log::warn!(
                    "templates v2 recovery: cleanup remove failed {}: {}",
                    src.display(),
                    e
                );
            }
            continue;
        }
        match fs::write(&dst, raw).and_then(|_| fs::remove_file(&src)) {
            Ok(()) => {
                recovered += 1;
            }
            Err(e) => {
                log::warn!(
                    "templates v2 recovery: move failed for {}: {}",
                    src.display(),
                    e
                );
            }
        }
    }
    if recovered > 0 {
        log::info!(
            "templates v2: recovered {} misplaced template file(s) from {} to {}",
            recovered,
            presets_dir.display(),
            templates_v2_dir.display()
        );
    }
    Ok(())
}

/// Cheap peek at a JSON document's `schemaVersion` field without
/// fully parsing the payload. Returns None when the field isn't
/// present or isn't an integer. Used by migration helpers to route
/// files between the v1 / v2 dirs without paying full deserialise.
fn peek_schema_version(raw: &str) -> Option<u32> {
    let v: serde_json::Value = serde_json::from_str(raw).ok()?;
    let n = v.get("schemaVersion").or_else(|| v.get("schema_version"))?;
    n.as_u64().map(|x| x as u32)
}

/// Path to a template's JSON file, given the unique name (already
/// slugged).
pub fn template_path(dir: &Path, unique_name: &str) -> PathBuf {
    dir.join(format!("{}.json", unique_name))
}

/// Read a single template by unique name.
pub fn read(dir: &Path, unique_name: &str) -> Result<TemplateFile> {
    let path = template_path(dir, unique_name);
    if !path.exists() {
        return Err(TemplateError::NotFound(unique_name.into()));
    }
    let raw = fs::read_to_string(&path)?;
    let parsed: TemplateFile = serde_json::from_str(&raw)
        .map_err(|e| TemplateError::ParseError(format!("{}: {}", path.display(), e)))?;
    Ok(parsed)
}

/// List all templates in the directory. Skips files that fail to
/// parse (logs a warning for each so users can find malformed ones)
/// rather than failing the whole listing.
pub fn list(dir: &Path) -> Result<Vec<TemplateFile>> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut out = Vec::new();
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.extension().and_then(|s| s.to_str()) != Some("json") {
            continue;
        }
        // Skip the tempfiles in-flight rename leaves (`.json.tmp`).
        if path.to_string_lossy().ends_with(".json.tmp") {
            continue;
        }
        match fs::read_to_string(&path) {
            Ok(raw) => match serde_json::from_str::<TemplateFile>(&raw) {
                Ok(t) => out.push(t),
                Err(e) => {
                    log::warn!("templates: skipping {}: parse error: {}", path.display(), e);
                }
            },
            Err(e) => {
                log::warn!("templates: skipping {}: read error: {}", path.display(), e);
            }
        }
    }
    out.sort_by(|a, b| a.display_name.to_lowercase().cmp(&b.display_name.to_lowercase()));
    Ok(out)
}

/// Look for a display-name collision. Returns the existing template
/// if any other file in `dir` has the same `display_name` (case-
/// insensitive) but a different `unique_name`. Used by the UI to
/// surface the "edit existing instead?" warning.
pub fn find_display_collision(
    dir: &Path,
    display_name: &str,
    own_unique_name: &str,
) -> Result<Option<TemplateFile>> {
    let needle = display_name.trim().to_lowercase();
    for t in list(dir)? {
        if t.unique_name == own_unique_name {
            continue;
        }
        if t.display_name.trim().to_lowercase() == needle {
            return Ok(Some(t));
        }
    }
    Ok(None)
}

/// Write a template to disk atomically (tempfile + rename).
///
/// Caller must have already done a display-name collision check via
/// `find_display_collision()` and decided to proceed (e.g. user
/// chose "edit existing"). This function does NOT enforce uniqueness
/// — it overwrites whatever sits at `<unique_name>.json`.
pub fn write_atomic(dir: &Path, t: &TemplateFile) -> Result<()> {
    fs::create_dir_all(dir)?;
    let path = template_path(dir, &t.unique_name);
    let tmp = dir.join(format!("{}.json.tmp", t.unique_name));
    {
        let json = serde_json::to_string_pretty(t)?;
        let mut f = fs::File::create(&tmp)?;
        f.write_all(json.as_bytes())?;
        f.sync_all()?;
    }
    // POSIX `rename(2)` is atomic when both paths are on the same
    // filesystem. The tempfile sits in the same directory, so this
    // holds. On Windows, rename over an existing file fails — we
    // remove the destination first if needed (small race window if
    // a second writer lands between remove + rename, which is
    // acceptable given the LWW design).
    #[cfg(windows)]
    {
        if path.exists() {
            let _ = fs::remove_file(&path);
        }
    }
    fs::rename(&tmp, &path)?;
    Ok(())
}

/// Delete a template by unique name. Missing file is not an error —
/// idempotent semantics.
pub fn delete(dir: &Path, unique_name: &str) -> Result<()> {
    let path = template_path(dir, unique_name);
    if path.exists() {
        fs::remove_file(&path)?;
    }
    Ok(())
}

/// Resolve the user identity to record in `created_by`. Tries
/// $USER, then $USERNAME, then "unknown".
pub fn current_user() -> String {
    std::env::var("USER")
        .or_else(|_| std::env::var("USERNAME"))
        .unwrap_or_else(|_| "unknown".into())
}

/// Migrate any pre-existing rows from the legacy SQLite
/// `column_presets` table out to the filesystem template store.
/// Idempotent: skips rows whose `<unique_name>.json` already exists.
/// Returns the number of templates written.
///
/// Called once per machine, gated by
/// `AppSettings::templates_migrated_v1`. Safe to re-run if the flag
/// is reset (e.g. dev rebuild flushes settings).
pub fn migrate_v1_legacy_presets(
    db: &crate::db::Database,
    farm_root: &str,
) -> Result<usize> {
    let dir = match resolve_dir(farm_root) {
        Ok(d) => d,
        Err(e) => {
            // Share unreachable — defer the migration until next
            // launch when the share is back. Don't flip the flag.
            log::warn!("templates: migration deferred (share unavailable): {}", e);
            return Ok(0);
        }
    };

    let rows: Vec<(String, String, i64, i64)> = db
        .with_conn(|conn| {
            // Older databases may not have `deleted_at`; the
            // `add_column_if_missing` migration in db.rs handles that
            // before this function runs, so we can rely on the column.
            let mut stmt = conn.prepare(
                "SELECT preset_name, columns_json, created_time, modified_time
                 FROM column_presets
                 WHERE deleted_at IS NULL",
            )?;
            let iter = stmt.query_map([], |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, i64>(2)?,
                    row.get::<_, i64>(3)?,
                ))
            })?;
            let mut out = Vec::new();
            for r in iter {
                out.push(r?);
            }
            Ok(out)
        })
        .map_err(|e| TemplateError::ParseError(format!("legacy preset query: {}", e)))?;

    if rows.is_empty() {
        log::info!("templates: nothing to migrate");
        return Ok(0);
    }

    let user = current_user();
    let mut written = 0usize;
    for (preset_name, columns_json, created_time, modified_time) in rows {
        let unique = slug_for(&preset_name);
        let target = template_path(&dir, &unique);
        if target.exists() {
            // Already migrated (or someone else's same-name template
            // already on disk). Skip — don't risk clobbering with
            // local-cache stale data.
            log::info!(
                "templates: migration skip — {} (file already exists)",
                unique
            );
            continue;
        }
        let preset_def: crate::columns::PresetColumnDef =
            match serde_json::from_str(&columns_json) {
                Ok(d) => d,
                Err(e) => {
                    log::warn!(
                        "templates: migration skip {} (parse error: {})",
                        preset_name,
                        e
                    );
                    continue;
                }
            };
        let template = TemplateFile {
            schema_version: SCHEMA_VERSION,
            unique_name: unique.clone(),
            display_name: preset_name.clone(),
            created_by: user.clone(),
            created_at: created_time,
            updated_at: modified_time,
            data: preset_def,
        };
        match write_atomic(&dir, &template) {
            Ok(()) => {
                written += 1;
                log::info!("templates: migrated `{}` → {}.json", preset_name, unique);
            }
            Err(e) => {
                log::warn!("templates: migration write failed for {}: {}", preset_name, e);
            }
        }
    }
    Ok(written)
}

// ─── v2: auto-promote templates ─────────────────────────────────────
//
// Hash-keyed, project-scoped, automatically managed for every column.
// Filename is a pure UUID v4 — never moves, never renamed. All mutable
// state (displayName, options, colors) lives inside the JSON. Edits go
// through `cas_save_template` for revision-CAS conflict detection.
//
// A manifest at `<dir>/_index.json` carries lightweight `(uuid,
// displayName, revision, ...)` entries so peers can do fast list-
// changed without parsing every template file.
//
// Public API on the v2 path is exposed via the `v2` submodule.

pub mod v2 {
    use super::{TemplateError, Result, current_user};
    use crate::columns::ColumnOption;
    use serde::{Deserialize, Serialize};
    use std::fs;
    use std::io::Write;
    use std::path::{Path, PathBuf};

    pub const SCHEMA_VERSION: u32 = 2;
    // Templates live under the shared-folder epoch umbrella
    // (`<farm>/<epoch>/templates`) alongside snapshots + nodes, so
    // peers on different epochs never share files. Derived from
    // `mesh::FARM_VERSION_SUBDIR` — the v5→v6 break moved snapshots +
    // nodes but left a hardcoded "v5/templates" behind, stranding
    // templates outside the epoch tree; deriving keeps the next bump
    // from repeating that. `resolve_dir` copies the previous epoch's
    // files forward on first touch (see
    // `migrate_previous_epoch_templates`).
    fn templates_subdir() -> String {
        format!("{}/templates", crate::mesh::FARM_VERSION_SUBDIR)
    }
    /// Where pre-derivation builds kept templates (the stranded v5
    /// location). Read-only migration source — old-build peers still
    /// write here during a mixed-fleet window, so it is never deleted.
    const LEGACY_TEMPLATES_SUBDIR: &str = "v5/templates";
    const MANIFEST_NAME: &str = "_index.json";

    /// Auto-promoted column template. Hash-keyed, project-scoped.
    /// `revision` is bumped on every edit; reads capture the
    /// revision and writes go through `cas_save_template` to detect
    /// concurrent edits.
    #[derive(Debug, Clone, Serialize, Deserialize)]
    #[serde(rename_all = "camelCase")]
    pub struct TemplateV2 {
        pub schema_version: u32,
        /// Pure UUID v4. Filename = `<template_hash>.json`. Never
        /// changes for this template's lifetime.
        pub template_hash: String,
        /// User-facing label. Immutable post-create — the codebase
        /// elsewhere blocks displayName edits and routes the user
        /// to "Duplicate as new column" instead.
        pub display_name: String,
        /// Slug derived from `display_name`. Used by the lookup-on-
        /// create dialog to match "is there already a template named
        /// 'Status' in this project?". Immutable alongside displayName.
        pub common_name_slug: String,
        /// Project namespace. Lookup-on-create only matches templates
        /// where `project_slug` matches the current folder's project.
        /// Cross-project sharing happens via presets, not by listing
        /// templates outside the current project.
        pub project_slug: String,
        pub column_type: String,
        #[serde(default)]
        pub default_value: Option<String>,
        #[serde(default)]
        pub options: Vec<ColumnOption>,
        pub created_by: String,
        pub created_at: i64,
        pub updated_at: i64,
        /// Monotonic edit counter. Starts at 1 on create, +1 on each
        /// successful `cas_save_template`. Used for optimistic-CC
        /// detection of concurrent edits without file locks.
        pub revision: u32,
    }

    /// Lightweight per-template entry in the shared manifest. Carries
    /// just enough to power the lookup-on-create dialog and the
    /// pull-to-cache delta without reading every JSON file.
    #[derive(Debug, Clone, Serialize, Deserialize)]
    #[serde(rename_all = "camelCase")]
    pub struct ManifestEntry {
        pub uuid: String,
        pub display_name: String,
        pub common_name_slug: String,
        pub project_slug: String,
        pub column_type: String,
        pub revision: u32,
        pub updated_at: i64,
        /// Most recent time this template was used (referenced by a
        /// new column). Picker sorts by this so unused legacy templates
        /// fall to the bottom.
        #[serde(default)]
        pub last_used_at: i64,
    }

    /// Top-level v2 manifest. `manifest_revision` is bumped on every
    /// successful `cas_save_manifest` so concurrent template-creates
    /// from multiple peers serialise without losing entries.
    #[derive(Debug, Clone, Serialize, Deserialize, Default)]
    #[serde(rename_all = "camelCase")]
    pub struct Manifest {
        #[serde(default)]
        pub manifest_revision: u32,
        #[serde(default)]
        pub manifest_updated_at: i64,
        #[serde(default)]
        pub templates: Vec<ManifestEntry>,
    }

    /// Resolve `<farm_root>/<epoch>/templates/`. Creates it on first
    /// call. Returns `DirUnavailable` for an empty / unreachable
    /// `farm_root`.
    ///
    /// Also runs `recover_misplaced_v2_files` to repair any v2
    /// templates that were mis-routed to the presets dir by a
    /// pre-fix version of the v1 migration, and copies the previous
    /// epoch's template files forward on first touch. Both are
    /// idempotent: no-op once everything's in the right dir.
    pub fn resolve_dir(farm_root: &str) -> Result<PathBuf> {
        if farm_root.trim().is_empty() {
            return Err(TemplateError::DirUnavailable(
                "farm_root is empty — mesh sync not configured".into(),
            ));
        }
        let dir = Path::new(farm_root).join(templates_subdir());
        fs::create_dir_all(&dir)
            .map_err(|e| TemplateError::DirUnavailable(format!("{}: {}", dir.display(), e)))?;
        migrate_previous_epoch_templates_once(farm_root, &dir);
        let _ = super::recover_misplaced_v2_files(farm_root, &dir);
        Ok(dir)
    }

    /// Once-per-process wrapper around
    /// `migrate_previous_epoch_templates`. Doesn't latch on a failed
    /// legacy-dir read so a transient share error retries on the next
    /// resolve.
    fn migrate_previous_epoch_templates_once(farm_root: &str, dir: &Path) {
        static DONE: std::sync::OnceLock<()> = std::sync::OnceLock::new();
        if DONE.get().is_some() {
            return;
        }
        if migrate_previous_epoch_templates(farm_root, dir).is_ok() {
            let _ = DONE.set(());
        }
    }

    /// Copy template files (incl. the manifest) from the previous
    /// epoch's templates dir into `dir` — the current epoch's — when
    /// they aren't there yet. COPY, not move: peers still on an
    /// old build keep reading/writing the legacy dir during the
    /// mixed-fleet window; once a file exists in the new dir the new
    /// dir wins and later legacy edits are ignored. Idempotent.
    pub(crate) fn migrate_previous_epoch_templates(
        farm_root: &str,
        dir: &Path,
    ) -> std::io::Result<()> {
        if templates_subdir() == LEGACY_TEMPLATES_SUBDIR {
            return Ok(());
        }
        let legacy = Path::new(farm_root).join(LEGACY_TEMPLATES_SUBDIR);
        if !legacy.is_dir() {
            return Ok(());
        }
        let mut copied = 0usize;
        for entry in fs::read_dir(&legacy)? {
            let Ok(entry) = entry else { continue };
            let src = entry.path();
            let Some(name) = src.file_name().and_then(|s| s.to_str()) else {
                continue;
            };
            // Real payloads only — skips tmp/bak debris from crashed
            // writers.
            if !name.ends_with(".json") {
                continue;
            }
            let dst = dir.join(name);
            if dst.exists() {
                continue;
            }
            match fs::copy(&src, &dst) {
                Ok(_) => copied += 1,
                Err(e) => log::warn!(
                    "templates v2: epoch migration copy failed for {}: {}",
                    name, e
                ),
            }
        }
        if copied > 0 {
            log::info!(
                "templates v2: migrated {} template file(s) from {} to {}",
                copied,
                legacy.display(),
                dir.display()
            );
        }
        Ok(())
    }

    /// Local cache mirror of the remote v2 templates dir, located at
    /// `<app_data_dir>/templates/`. Created on first call. Used as the
    /// offline-fallback read source by `fetch_template` and as the
    /// destination of `pull_to_local_cache` / cache-warm writes.
    pub fn local_cache_dir() -> PathBuf {
        let dir = crate::utils::get_app_data_dir().join("templates");
        let _ = fs::create_dir_all(&dir);
        dir
    }

    /// Path to a single template's JSON file.
    pub fn template_path(dir: &Path, template_hash: &str) -> PathBuf {
        dir.join(format!("{}.json", template_hash))
    }

    /// Path to the manifest file (`_index.json` in the templates
    /// dir).
    pub fn manifest_path(dir: &Path) -> PathBuf {
        dir.join(MANIFEST_NAME)
    }

    /// Read a template by its hash.
    pub fn read_template(dir: &Path, template_hash: &str) -> Result<TemplateV2> {
        let path = template_path(dir, template_hash);
        if !path.exists() {
            return Err(TemplateError::NotFound(template_hash.into()));
        }
        let raw = fs::read_to_string(&path)?;
        let parsed: TemplateV2 = serde_json::from_str(&raw)
            .map_err(|e| TemplateError::ParseError(format!("{}: {}", path.display(), e)))?;
        Ok(parsed)
    }

    /// 0.9.97 — receive-side template fetch for the mesh path.
    ///
    /// Used by `apply_table_change` when an incoming `col_add` /
    /// `col_update` carries a `template_hash`. Tries the local cache
    /// first (offline-fast path) and falls back to the remote shared
    /// dir on miss; on a successful remote read, warms the local
    /// cache for future hits. Returns `NotFound` when neither
    /// location holds the template — the caller turns that into
    /// "insert column row with template_hash = NULL" so the row stays
    /// hidden from the grid until the template becomes reachable on
    /// a later snapshot replay.
    pub fn fetch_template(
        cache_dir: &Path,
        remote_dir: Option<&Path>,
        template_hash: &str,
    ) -> Result<TemplateV2> {
        if let Ok(t) = read_template(cache_dir, template_hash) {
            return Ok(t);
        }
        if let Some(remote) = remote_dir {
            let t = read_template(remote, template_hash)?;
            // Best-effort cache warm. Failure to mirror just means the
            // next fetch goes back to the remote — non-fatal.
            if let Err(e) = write_template_atomic(cache_dir, &t) {
                log::warn!(
                    "v2::fetch_template: cache warm failed for {}: {}",
                    template_hash, e
                );
            }
            return Ok(t);
        }
        Err(TemplateError::NotFound(template_hash.into()))
    }

    /// List every template in the directory by parsing each JSON.
    /// Skips the manifest, tempfiles, and malformed files (logs a
    /// warning rather than failing the whole listing). Used during
    /// migrations + initial cache population; routine listings should
    /// go through the manifest instead for speed.
    pub fn list_templates(dir: &Path) -> Result<Vec<TemplateV2>> {
        if !dir.exists() {
            return Ok(Vec::new());
        }
        let mut out = Vec::new();
        for entry in fs::read_dir(dir)? {
            let entry = entry?;
            let path = entry.path();
            let Some(name) = path.file_name().and_then(|s| s.to_str()) else {
                continue;
            };
            if name == MANIFEST_NAME {
                continue;
            }
            if !name.ends_with(".json") || name.ends_with(".json.tmp") {
                continue;
            }
            match fs::read_to_string(&path) {
                Ok(raw) => match serde_json::from_str::<TemplateV2>(&raw) {
                    Ok(t) => out.push(t),
                    Err(e) => log::warn!("templates v2: skip {}: {}", path.display(), e),
                },
                Err(e) => log::warn!("templates v2: skip {}: {}", path.display(), e),
            }
        }
        Ok(out)
    }

    /// Read the shared manifest. Returns an empty default Manifest
    /// when the file doesn't exist yet (first run on a freshly-created
    /// templates dir).
    pub fn read_manifest(dir: &Path) -> Result<Manifest> {
        let path = manifest_path(dir);
        if !path.exists() {
            return Ok(Manifest::default());
        }
        let raw = fs::read_to_string(&path)?;
        let parsed: Manifest = serde_json::from_str(&raw)
            .map_err(|e| TemplateError::ParseError(format!("{}: {}", path.display(), e)))?;
        Ok(parsed)
    }

    /// Atomic write of `payload` to `dest` via tempfile + rename.
    ///
    /// The temp name is unique PER WRITE (uuid), not derived from the
    /// destination: the old scheme gave every writer of a given file —
    /// fleet-wide, on the share — the SAME temp path, so two peers
    /// replacing the same template could interleave create/remove/
    /// rename on one temp file and delete the destination with nothing
    /// left to rename over it (the lost `_index.json` / hot-template
    /// incident of 2026-08-26). With private temps, concurrent
    /// replacers serialise to last-writer-wins, which the caller-layer
    /// revision-CAS already handles. `tmp_suffix` is kept only as a
    /// human-readable marker in the temp name for debugging debris.
    fn atomic_write_json<T: Serialize>(
        dest: &Path,
        payload: &T,
        tmp_suffix: &str,
    ) -> Result<()> {
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent)?;
        }
        let tmp = dest.with_extension(format!(
            "json.tmp.{}.{}",
            tmp_suffix,
            uuid::Uuid::new_v4().simple()
        ));
        {
            let json = serde_json::to_string_pretty(payload)?;
            let mut f = fs::File::create(&tmp)?;
            f.write_all(json.as_bytes())?;
            f.sync_all()?;
        }
        if let Err(e) = replace_file(&tmp, dest) {
            let _ = fs::remove_file(&tmp);
            return Err(e);
        }
        Ok(())
    }

    /// Move `tmp` over `dest`, replacing any existing file. On Unix a
    /// plain `rename` replaces atomically. On Windows `rename` refuses
    /// to overwrite, so an existing `dest` is first SET ASIDE under a
    /// unique backup name and restored if the final rename fails —
    /// unlike the old remove-then-rename, `dest`'s content is never
    /// deleted while the replacement could still be lost. Retries
    /// absorb another writer racing the same destination.
    #[cfg(not(windows))]
    fn replace_file(tmp: &Path, dest: &Path) -> Result<()> {
        fs::rename(tmp, dest)?;
        Ok(())
    }

    #[cfg(windows)]
    fn replace_file(tmp: &Path, dest: &Path) -> Result<()> {
        let mut last_err: Option<std::io::Error> = None;
        for _ in 0..5 {
            // Fast path — dest absent (fresh create, or a racing
            // writer's set-aside is in flight).
            match fs::rename(tmp, dest) {
                Ok(()) => return Ok(()),
                Err(e) => last_err = Some(e),
            }
            if !dest.exists() {
                // Rename failed with no dest in the way — transient
                // share error or a racer mid-replace; retry.
                continue;
            }
            let bak = dest.with_extension(format!(
                "json.bak.{}",
                uuid::Uuid::new_v4().simple()
            ));
            match fs::rename(dest, &bak) {
                Ok(()) => match fs::rename(tmp, dest) {
                    Ok(()) => {
                        let _ = fs::remove_file(&bak);
                        return Ok(());
                    }
                    Err(e) => {
                        // Roll the previous content back so dest never
                        // stays missing.
                        let _ = fs::rename(&bak, dest);
                        last_err = Some(e);
                    }
                },
                // dest vanished between exists() and rename — racing
                // writer took it; retry the fast path.
                Err(e) => last_err = Some(e),
            }
        }
        Err(last_err
            .unwrap_or_else(|| {
                std::io::Error::new(
                    std::io::ErrorKind::Other,
                    "replace_file: retries exhausted",
                )
            })
            .into())
    }

    /// Write a template atomically without revision check. Used by
    /// fresh creates (caller has just minted a new UUID; the file
    /// can't conflict).
    pub fn write_template_atomic(dir: &Path, t: &TemplateV2) -> Result<()> {
        let path = template_path(dir, &t.template_hash);
        atomic_write_json(&path, t, &t.template_hash)
    }

    /// Compare-and-swap save: re-read the file, ensure its `revision`
    /// matches `expected_revision`, then write `next` (which should
    /// already carry `revision = expected_revision + 1`). Returns
    /// `Err(TemplateError::RevisionMismatch)` on conflict so the UI
    /// surfaces a "reload?" dialog.
    pub fn cas_save_template(
        dir: &Path,
        next: &TemplateV2,
        expected_revision: u32,
    ) -> Result<()> {
        let path = template_path(dir, &next.template_hash);
        if path.exists() {
            let current = read_template(dir, &next.template_hash)?;
            if current.revision != expected_revision {
                return Err(TemplateError::RevisionMismatch {
                    expected: expected_revision,
                    found: current.revision,
                });
            }
        } else if expected_revision != 0 {
            // Caller expected an existing template at revision N but
            // the file is gone (deleted by another peer, or never
            // existed). Surface as a conflict — the caller should
            // re-evaluate (likely by switching to a fresh create).
            return Err(TemplateError::RevisionMismatch {
                expected: expected_revision,
                found: 0,
            });
        }
        write_template_atomic(dir, next)
    }

    /// Atomic write of the manifest. Same pattern as templates.
    pub fn write_manifest_atomic(dir: &Path, m: &Manifest) -> Result<()> {
        let path = manifest_path(dir);
        atomic_write_json(&path, m, "manifest")
    }

    /// CAS save the manifest, expecting `expected_revision`. Same
    /// shape as `cas_save_template`.
    pub fn cas_save_manifest(
        dir: &Path,
        next: &Manifest,
        expected_revision: u32,
    ) -> Result<()> {
        let manifest_p = manifest_path(dir);
        if manifest_p.exists() {
            let current = read_manifest(dir)?;
            if current.manifest_revision != expected_revision {
                return Err(TemplateError::RevisionMismatch {
                    expected: expected_revision,
                    found: current.manifest_revision,
                });
            }
        } else if expected_revision != 0 {
            return Err(TemplateError::RevisionMismatch {
                expected: expected_revision,
                found: 0,
            });
        }
        write_manifest_atomic(dir, next)
    }

    /// Look up an existing template in the manifest by
    /// `(project_slug, common_name_slug, column_type)`. Returns the
    /// matching ManifestEntry on hit, None on miss. The caller
    /// resolves the full template via `read_template` if needed.
    /// Used by Phase 2's "lookup-or-mint" flow.
    pub fn lookup_in_manifest(
        m: &Manifest,
        project_slug: &str,
        common_name_slug: &str,
        column_type: &str,
    ) -> Option<ManifestEntry> {
        m.templates
            .iter()
            .find(|t| {
                t.project_slug == project_slug
                    && t.common_name_slug == common_name_slug
                    && t.column_type == column_type
            })
            .cloned()
    }

    /// Pull v2 templates from `remote_dir` to `local_dir`, fetching
    /// only files whose manifest revision is newer than the local
    /// cache copy. Returns the count of files written. Used as the
    /// background "refresh" path (app launch / dialog open / refresh
    /// button).
    ///
    /// Best-effort: per-file errors are logged but don't fail the
    /// pull. Treats a missing remote manifest as "no templates,
    /// nothing to pull" rather than an error.
    pub fn pull_to_local_cache(remote_dir: &Path, local_dir: &Path) -> Result<usize> {
        fs::create_dir_all(local_dir)?;
        // A missing remote manifest means "fresh dir" — or a manifest
        // lost to a crashed/raced writer. Either way there is nothing
        // trustworthy to diff against, and mirroring the empty default
        // over the local cache manifest would erase the cache's pull
        // baseline (forcing needless re-pulls AND blanking the
        // offline lookup view). Leave the cache untouched.
        if !manifest_path(remote_dir).exists() {
            return Ok(0);
        }
        let remote_manifest = match read_manifest(remote_dir) {
            Ok(m) => m,
            Err(TemplateError::Io(_)) => return Ok(0),
            Err(e) => return Err(e),
        };
        let local_manifest = read_manifest(local_dir).unwrap_or_default();
        let mut local_by_uuid: std::collections::HashMap<&str, &ManifestEntry> =
            std::collections::HashMap::new();
        for e in &local_manifest.templates {
            local_by_uuid.insert(e.uuid.as_str(), e);
        }

        let mut pulled = 0usize;
        for remote_entry in &remote_manifest.templates {
            let needs_pull = match local_by_uuid.get(remote_entry.uuid.as_str()) {
                Some(local) => local.revision < remote_entry.revision,
                None => true,
            };
            if !needs_pull {
                continue;
            }
            // Fetch the individual template file.
            match read_template(remote_dir, &remote_entry.uuid) {
                Ok(t) => match write_template_atomic(local_dir, &t) {
                    Ok(()) => pulled += 1,
                    Err(e) => log::warn!(
                        "templates v2: pull write failed for {}: {}",
                        remote_entry.uuid,
                        e
                    ),
                },
                Err(e) => log::warn!(
                    "templates v2: pull read failed for {}: {}",
                    remote_entry.uuid,
                    e
                ),
            }
        }
        // Mirror the manifest into the local cache so subsequent
        // pulls have an accurate baseline.
        let _ = write_manifest_atomic(local_dir, &remote_manifest);
        Ok(pulled)
    }

    /// Mint a fresh UUID v4 string — used by Phase 2 when a column
    /// add doesn't find a matching existing template.
    pub fn new_template_hash() -> String {
        uuid::Uuid::new_v4().to_string()
    }

    /// Namespace for deterministic template hashes (UUIDv5). Fixed.
    const TEMPLATE_HASH_NS: uuid::Uuid =
        uuid::Uuid::from_u128(0x7c1e_3b95_42a8_5d6f_9e04_8a17_2c5b_6d39);

    /// Deterministic template hash from the logical template key
    /// (project, common-name slug, column type). Two peers that mint the
    /// "same" template independently — e.g. in the simultaneous-create
    /// race the manifest lookup can't yet resolve — derive the SAME hash,
    /// so they converge on one shared template file + options instead of
    /// two. Slugs are already case-folded by `slug_for`, so this is
    /// case-stable across macOS/Windows. `force_new` (fork) bypasses this
    /// and uses a random hash so a deliberate fork stays distinct.
    fn deterministic_template_hash(
        project_slug: &str,
        common_name_slug: &str,
        column_type: &str,
    ) -> String {
        let key = format!(
            "{}\x1f{}\x1f{}",
            project_slug,
            common_name_slug,
            column_type.to_lowercase()
        );
        uuid::Uuid::new_v5(&TEMPLATE_HASH_NS, key.as_bytes()).to_string()
    }

    /// Build a minimal ManifestEntry from a template payload. Used
    /// when creating or updating manifest entries during a v2 write
    /// flow.
    pub fn entry_for(t: &TemplateV2) -> ManifestEntry {
        ManifestEntry {
            uuid: t.template_hash.clone(),
            display_name: t.display_name.clone(),
            common_name_slug: t.common_name_slug.clone(),
            project_slug: t.project_slug.clone(),
            column_type: t.column_type.clone(),
            revision: t.revision,
            updated_at: t.updated_at,
            last_used_at: t.updated_at,
        }
    }

    /// Apply `entry` to `m` — replaces an existing entry with the
    /// same uuid or appends a new one. Bumps `manifest_revision` and
    /// `manifest_updated_at`. Caller is responsible for the
    /// `cas_save_manifest` round-trip.
    pub fn apply_entry(m: &mut Manifest, entry: ManifestEntry, now: i64) {
        if let Some(existing) = m
            .templates
            .iter_mut()
            .find(|t| t.uuid == entry.uuid)
        {
            *existing = entry;
        } else {
            m.templates.push(entry);
        }
        m.manifest_revision = m.manifest_revision.saturating_add(1);
        m.manifest_updated_at = now;
    }

    /// Construct a fresh template + matching manifest entry. Common
    /// helper for "auto-promote on add_column" — caller writes the
    /// template via `write_template_atomic`, then folds the entry
    /// into the manifest via `apply_entry` + `cas_save_manifest`.
    pub fn fresh(
        display_name: String,
        common_name_slug: String,
        project_slug: String,
        column_type: String,
        default_value: Option<String>,
        options: Vec<ColumnOption>,
        now: i64,
        force_new: bool,
    ) -> TemplateV2 {
        // Deterministic hash for ordinary mints (peers converge); random
        // for a deliberate fork (force_new) so it stays distinct.
        let template_hash = if force_new {
            new_template_hash()
        } else {
            deterministic_template_hash(&project_slug, &common_name_slug, &column_type)
        };
        TemplateV2 {
            schema_version: SCHEMA_VERSION,
            template_hash,
            display_name,
            common_name_slug,
            project_slug,
            column_type,
            default_value,
            options,
            created_by: current_user(),
            created_at: now,
            updated_at: now,
            revision: 1,
        }
    }

    /// Look up an existing template by (project, common_slug, type),
    /// or mint and write a new one when no match exists. Used by
    /// `add_column`'s auto-promote path:
    ///
    ///   - `force_new = false` (default) → reuse if found.
    ///   - `force_new = true` → mint a new UUID even if a match
    ///     exists (the lookup-on-create dialog's "Create new" path).
    ///
    /// On mint: writes the template file + folds an entry into the
    /// manifest with revision-CAS retry. Manifest revision retries
    /// up to 5 times to handle peers concurrently creating templates
    /// under the same project — the per-template `<uuid>.json` files
    /// can never collide so each create succeeds, and the manifest
    /// CAS just serialises their entries.
    pub fn lookup_or_mint(
        dir: &Path,
        display_name: &str,
        common_name_slug: &str,
        project_slug: &str,
        column_type: &str,
        default_value: Option<String>,
        options: Vec<ColumnOption>,
        now: i64,
        force_new: bool,
    ) -> Result<TemplateV2> {
        lookup_or_mint_detailed(
            dir, display_name, common_name_slug, project_slug,
            column_type, default_value, options, now, force_new,
        )
        .map(|(t, _)| t)
    }

    /// Like `lookup_or_mint` but also returns whether the template
    /// was freshly minted (`true`) or reused from an existing
    /// manifest entry (`false`). The "Use it" flow needs to know
    /// this so it can populate the new column's `column_options`
    /// from the existing template's options instead of the user's
    /// (empty / partially-filled) form input.
    pub fn lookup_or_mint_detailed(
        dir: &Path,
        display_name: &str,
        common_name_slug: &str,
        project_slug: &str,
        column_type: &str,
        default_value: Option<String>,
        options: Vec<ColumnOption>,
        now: i64,
        force_new: bool,
    ) -> Result<(TemplateV2, bool)> {
        if !force_new {
            let manifest = read_manifest(dir).unwrap_or_default();
            if let Some(entry) = lookup_in_manifest(
                &manifest, project_slug, common_name_slug, column_type,
            ) {
                let existing = read_template(dir, &entry.uuid)?;
                // Bump the manifest's lastUsedAt for this template
                // so future picker UI can sort by recent activity.
                // Best-effort — failure here doesn't block the
                // reuse return, since the template content is
                // already loaded and the SQL flow above continues.
                let _ = bump_last_used_at(dir, &existing.template_hash, now);
                return Ok((existing, false)); // reused
            }
        }
        let template = fresh(
            display_name.to_string(),
            common_name_slug.to_string(),
            project_slug.to_string(),
            column_type.to_string(),
            default_value,
            options,
            now,
            force_new,
        );
        write_template_atomic(dir, &template)?;
        // Manifest CAS retry — fetches the latest, applies, saves.
        // Up to 5 retries handles peer-concurrent creates without
        // losing entries.
        for attempt in 0..5 {
            let mut manifest = read_manifest(dir).unwrap_or_default();
            let expected = manifest.manifest_revision;
            apply_entry(&mut manifest, entry_for(&template), now);
            match cas_save_manifest(dir, &manifest, expected) {
                Ok(()) => return Ok((template, true)), // freshly minted
                Err(TemplateError::RevisionMismatch { .. }) if attempt < 4 => {
                    log::info!(
                        "templates v2: manifest CAS retry {} for new {}",
                        attempt + 1,
                        template.template_hash
                    );
                    continue;
                }
                Err(e) => return Err(e),
            }
        }
        // Should never hit — the loop above either returns Ok or
        // propagates the error.
        Err(TemplateError::ParseError(
            "manifest CAS exhausted retries".into(),
        ))
    }

    /// Re-create a template file that has gone missing from the share
    /// while column rows still reference its hash (file lost to a
    /// crashed or raced writer). `base` supplies the content — callers
    /// pass their local-cache copy when one exists, so the revision
    /// line stays ahead of peers' caches and `pull_to_local_cache`'s
    /// revision diff still advances; otherwise a reconstruction from
    /// the column row (revision 0). Keeps `template_hash` STABLE —
    /// peers reference it — bumps revision + updated_at, writes the
    /// file, and folds the entry back into the manifest (CAS-retried,
    /// recreating the manifest too when it is also missing).
    pub fn restore_template(dir: &Path, base: TemplateV2, now: i64) -> Result<TemplateV2> {
        let mut t = base;
        t.revision = t.revision.saturating_add(1);
        t.updated_at = now;
        write_template_atomic(dir, &t)?;
        for attempt in 0..5 {
            let mut manifest = read_manifest(dir).unwrap_or_default();
            let expected = manifest.manifest_revision;
            apply_entry(&mut manifest, entry_for(&t), now);
            match cas_save_manifest(dir, &manifest, expected) {
                Ok(()) => return Ok(t),
                Err(TemplateError::RevisionMismatch { .. }) if attempt < 4 => continue,
                Err(e) => return Err(e),
            }
        }
        Err(TemplateError::ParseError(
            "manifest CAS exhausted retries during restore".into(),
        ))
    }

    /// Bump the manifest's `lastUsedAt` for `template_hash` to
    /// `now`. Used by the "reuse" branch of `lookup_or_mint_detailed`
    /// so the picker can later sort templates by recent activity.
    /// CAS-retried up to 5 times to handle peer-concurrent updates.
    /// No-op if the manifest doesn't list this hash (template was
    /// deleted, or hash unknown). Errors are surfaced but the
    /// caller is expected to swallow them — failing to bump
    /// `lastUsedAt` doesn't break any user-facing behaviour.
    pub fn bump_last_used_at(dir: &Path, template_hash: &str, now: i64) -> Result<()> {
        for attempt in 0..5 {
            let mut manifest = read_manifest(dir).unwrap_or_default();
            let expected = manifest.manifest_revision;
            let Some(entry) = manifest
                .templates
                .iter_mut()
                .find(|t| t.uuid == template_hash)
            else {
                // Manifest doesn't have this template — nothing to
                // bump. Not an error per se; just exit quietly.
                return Ok(());
            };
            // Skip the rewrite when the timestamp wouldn't actually
            // change — saves an atomic write + CAS round-trip when
            // multiple reuses fire within the same millisecond.
            if entry.last_used_at >= now {
                return Ok(());
            }
            entry.last_used_at = now;
            manifest.manifest_revision = manifest.manifest_revision.saturating_add(1);
            manifest.manifest_updated_at = now;
            match cas_save_manifest(dir, &manifest, expected) {
                Ok(()) => return Ok(()),
                Err(TemplateError::RevisionMismatch { .. }) if attempt < 4 => {
                    continue;
                }
                Err(e) => return Err(e),
            }
        }
        Err(TemplateError::ParseError(
            "manifest CAS exhausted retries during lastUsedAt bump".into(),
        ))
    }

    /// Update an existing template's options + default_value via
    /// revision-CAS. Caller passes the expected current revision
    /// (read from the local cache or from a previous read).
    /// Increments revision and updated_at on success.
    /// Returns the updated TemplateV2.
    ///
    /// On `RevisionMismatch`: caller should re-read the template
    /// (reload local cache from share via pull_to_local_cache),
    /// surface a "this template was changed by another user" dialog,
    /// and let the user decide whether to merge their edits.
    pub fn update_template_options(
        dir: &Path,
        template_hash: &str,
        new_default_value: Option<String>,
        new_options: Vec<ColumnOption>,
        expected_revision: u32,
        now: i64,
    ) -> Result<TemplateV2> {
        let mut current = read_template(dir, template_hash)?;
        if current.revision != expected_revision {
            return Err(TemplateError::RevisionMismatch {
                expected: expected_revision,
                found: current.revision,
            });
        }
        current.default_value = new_default_value;
        current.options = new_options;
        current.revision = current.revision.saturating_add(1);
        current.updated_at = now;
        cas_save_template(dir, &current, expected_revision)?;
        // Manifest entry update — same retry loop as create.
        for attempt in 0..5 {
            let mut manifest = read_manifest(dir).unwrap_or_default();
            let expected_m = manifest.manifest_revision;
            apply_entry(&mut manifest, entry_for(&current), now);
            match cas_save_manifest(dir, &manifest, expected_m) {
                Ok(()) => return Ok(current),
                Err(TemplateError::RevisionMismatch { .. }) if attempt < 4 => {
                    continue;
                }
                Err(e) => return Err(e),
            }
        }
        Err(TemplateError::ParseError(
            "manifest CAS exhausted retries during update".into(),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::columns::PresetColumnDef;
    use tempfile::TempDir;

    fn sample_preset() -> PresetColumnDef {
        PresetColumnDef {
            column_name: "Status".into(),
            column_type: "option".into(),
            column_order: 0,
            column_width: 120.0,
            is_visible: true,
            default_value: None,
            options: vec![],
        }
    }

    fn sample_template(unique: &str, display: &str) -> TemplateFile {
        TemplateFile {
            schema_version: SCHEMA_VERSION,
            unique_name: unique.into(),
            display_name: display.into(),
            created_by: "tester".into(),
            created_at: 1000,
            updated_at: 1000,
            data: sample_preset(),
        }
    }

    #[test]
    fn slug_basic() {
        assert_eq!(slug_for("Status Column"), "status_column");
        assert_eq!(slug_for("Status — Column!"), "status_column");
        assert_eq!(slug_for("__hello__"), "hello");
        assert_eq!(slug_for(""), "untitled");
        assert_eq!(slug_for("---"), "untitled");
    }

    #[test]
    fn write_and_read_roundtrip() {
        let tmp = TempDir::new().unwrap();
        let t = sample_template("status", "Status");
        write_atomic(tmp.path(), &t).unwrap();
        let back = read(tmp.path(), "status").unwrap();
        assert_eq!(back.display_name, "Status");
        assert_eq!(back.data.column_name, "Status");
    }

    #[test]
    fn list_sorts_by_display_name() {
        let tmp = TempDir::new().unwrap();
        write_atomic(tmp.path(), &sample_template("zed", "Zed")).unwrap();
        write_atomic(tmp.path(), &sample_template("alpha", "Alpha")).unwrap();
        let names: Vec<String> = list(tmp.path())
            .unwrap()
            .into_iter()
            .map(|t| t.display_name)
            .collect();
        assert_eq!(names, vec!["Alpha", "Zed"]);
    }

    #[test]
    fn list_skips_malformed() {
        let tmp = TempDir::new().unwrap();
        write_atomic(tmp.path(), &sample_template("good", "Good")).unwrap();
        fs::write(tmp.path().join("bad.json"), "not json").unwrap();
        let templates = list(tmp.path()).unwrap();
        assert_eq!(templates.len(), 1);
        assert_eq!(templates[0].unique_name, "good");
    }

    #[test]
    fn list_skips_tempfiles() {
        let tmp = TempDir::new().unwrap();
        write_atomic(tmp.path(), &sample_template("good", "Good")).unwrap();
        // Simulate a crashed write that left a tempfile.
        fs::write(tmp.path().join("crash.json.tmp"), r#"{"x": 1}"#).unwrap();
        let templates = list(tmp.path()).unwrap();
        assert_eq!(templates.len(), 1);
    }

    #[test]
    fn find_display_collision_finds_other() {
        let tmp = TempDir::new().unwrap();
        write_atomic(tmp.path(), &sample_template("status_a", "Status")).unwrap();
        let hit = find_display_collision(tmp.path(), "Status", "status_b").unwrap();
        assert!(hit.is_some());
        assert_eq!(hit.unwrap().unique_name, "status_a");
    }

    #[test]
    fn find_display_collision_ignores_self() {
        let tmp = TempDir::new().unwrap();
        write_atomic(tmp.path(), &sample_template("status", "Status")).unwrap();
        let hit = find_display_collision(tmp.path(), "Status", "status").unwrap();
        assert!(hit.is_none());
    }

    #[test]
    fn delete_is_idempotent() {
        let tmp = TempDir::new().unwrap();
        write_atomic(tmp.path(), &sample_template("status", "Status")).unwrap();
        delete(tmp.path(), "status").unwrap();
        delete(tmp.path(), "status").unwrap(); // not-found OK
        assert!(matches!(read(tmp.path(), "status"), Err(TemplateError::NotFound(_))));
    }

    #[test]
    fn resolve_dir_rejects_empty_root() {
        let err = resolve_dir("").unwrap_err();
        assert!(matches!(err, TemplateError::DirUnavailable(_)));
    }

    // ─── v2 templates tests ─────────────────────────────────────
    mod v2_tests {
        use super::super::v2;
        use super::super::TemplateError;
        use crate::columns::ColumnOption;
        use std::fs;
        use tempfile::TempDir;

        fn fresh_template(common: &str, project: &str) -> v2::TemplateV2 {
            v2::fresh(
                common.to_string(),
                common.to_lowercase(),
                project.to_string(),
                "option".to_string(),
                None,
                vec![ColumnOption {
                    id: None,
                    name: "Todo".to_string(),
                    color: Some("#888888".to_string()),
                    modified_time: 0,
                    deleted_at: None,
                }],
                1000,
                true, // random hash — keeps each seeded template distinct
            )
        }

        #[test]
        fn fresh_assigns_uuid_and_revision_one() {
            let t = fresh_template("Status", "vfx_2025");
            assert!(!t.template_hash.is_empty());
            assert_eq!(t.revision, 1);
            assert_eq!(t.schema_version, v2::SCHEMA_VERSION);
            assert_eq!(t.common_name_slug, "status");
        }

        #[test]
        fn write_read_template_roundtrip() {
            let tmp = TempDir::new().unwrap();
            let t = fresh_template("Status", "vfx_2025");
            v2::write_template_atomic(tmp.path(), &t).unwrap();
            let back = v2::read_template(tmp.path(), &t.template_hash).unwrap();
            assert_eq!(back.display_name, "Status");
            assert_eq!(back.options.len(), 1);
            assert_eq!(back.revision, 1);
        }

        #[test]
        fn cas_save_succeeds_when_revision_matches() {
            let tmp = TempDir::new().unwrap();
            let mut t = fresh_template("Status", "vfx_2025");
            v2::write_template_atomic(tmp.path(), &t).unwrap();
            // Edit + bump revision + CAS save with the OLD revision
            // (1) as the expected — it's still 1 on disk.
            t.options.push(ColumnOption {
                id: None,
                name: "Doing".to_string(),
                color: Some("#f5a623".to_string()),
                modified_time: 0,
                deleted_at: None,
            });
            t.revision = 2;
            v2::cas_save_template(tmp.path(), &t, 1).unwrap();
            let back = v2::read_template(tmp.path(), &t.template_hash).unwrap();
            assert_eq!(back.options.len(), 2);
            assert_eq!(back.revision, 2);
        }

        #[test]
        fn atomic_replace_overwrites_and_leaves_no_debris() {
            let tmp = TempDir::new().unwrap();
            let mut t = fresh_template("Status", "vfx_2025");
            v2::write_template_atomic(tmp.path(), &t).unwrap();
            t.revision = 2;
            // Second write exercises the replace-existing path.
            v2::write_template_atomic(tmp.path(), &t).unwrap();
            let back = v2::read_template(tmp.path(), &t.template_hash).unwrap();
            assert_eq!(back.revision, 2);
            for entry in fs::read_dir(tmp.path()).unwrap() {
                let name = entry.unwrap().file_name().to_string_lossy().to_string();
                assert!(name.ends_with(".json"), "debris left behind: {}", name);
            }
        }

        #[test]
        fn pull_missing_remote_manifest_leaves_local_cache_alone() {
            let remote = TempDir::new().unwrap(); // no _index.json
            let local = TempDir::new().unwrap();
            let t = fresh_template("Status", "vfx_2025");
            v2::write_template_atomic(local.path(), &t).unwrap();
            let mut m = v2::Manifest::default();
            v2::apply_entry(&mut m, v2::entry_for(&t), 1000);
            v2::write_manifest_atomic(local.path(), &m).unwrap();

            let pulled = v2::pull_to_local_cache(remote.path(), local.path()).unwrap();
            assert_eq!(pulled, 0);
            let back = v2::read_manifest(local.path()).unwrap();
            assert_eq!(
                back.templates.len(),
                1,
                "local cache manifest must not be clobbered by an empty default"
            );
        }

        #[test]
        fn epoch_migration_copies_legacy_templates_forward() {
            let farm = TempDir::new().unwrap();
            let legacy = farm.path().join("v5/templates");
            fs::create_dir_all(&legacy).unwrap();
            let t = fresh_template("Status", "vfx_2025");
            v2::write_template_atomic(&legacy, &t).unwrap();
            let mut m = v2::Manifest::default();
            v2::apply_entry(&mut m, v2::entry_for(&t), 1000);
            v2::write_manifest_atomic(&legacy, &m).unwrap();
            // Writer debris must NOT be copied forward.
            fs::write(legacy.join("junk.json.tmp.manifest.abc"), "x").unwrap();

            let new_dir = farm
                .path()
                .join(format!("{}/templates", crate::mesh::FARM_VERSION_SUBDIR));
            fs::create_dir_all(&new_dir).unwrap();
            v2::migrate_previous_epoch_templates(farm.path().to_str().unwrap(), &new_dir)
                .unwrap();

            assert!(v2::read_template(&new_dir, &t.template_hash).is_ok());
            assert_eq!(v2::read_manifest(&new_dir).unwrap().templates.len(), 1);
            assert!(!new_dir.join("junk.json.tmp.manifest.abc").exists());
            // Legacy stays intact — old-build peers still use it.
            assert!(v2::read_template(&legacy, &t.template_hash).is_ok());
            // Idempotent.
            v2::migrate_previous_epoch_templates(farm.path().to_str().unwrap(), &new_dir)
                .unwrap();
            assert_eq!(v2::read_manifest(&new_dir).unwrap().templates.len(), 1);
        }

        #[test]
        fn restore_template_bumps_revision_and_reindexes() {
            let tmp = TempDir::new().unwrap();
            let mut base = fresh_template("Status", "vfx_2025");
            base.revision = 41; // the local-cache copy of a lost file
            // Nothing on disk — the "file lost from the share" state.
            let restored = v2::restore_template(tmp.path(), base.clone(), 2000).unwrap();
            assert_eq!(restored.revision, 42);
            assert_eq!(restored.template_hash, base.template_hash);
            let back = v2::read_template(tmp.path(), &base.template_hash).unwrap();
            assert_eq!(back.revision, 42);
            assert_eq!(back.updated_at, 2000);
            let m = v2::read_manifest(tmp.path()).unwrap();
            assert_eq!(m.templates.len(), 1);
            assert_eq!(m.templates[0].revision, 42);
        }

        #[test]
        fn cas_save_rejects_when_revision_mismatches() {
            let tmp = TempDir::new().unwrap();
            // Two peers both fetched revision 1. A saves first
            // (becomes revision 2). B's stale CAS save with expected=1
            // must reject.
            let mut t1 = fresh_template("Status", "vfx_2025");
            v2::write_template_atomic(tmp.path(), &t1).unwrap();
            // A's edit:
            t1.options.push(ColumnOption {
                id: None,
                name: "A-edit".to_string(),
                color: None,
                modified_time: 0,
                deleted_at: None,
            });
            t1.revision = 2;
            v2::cas_save_template(tmp.path(), &t1, 1).unwrap();
            // B's stale-revision-1 edit attempt:
            let mut t2 = fresh_template("Status", "vfx_2025");
            t2.template_hash = t1.template_hash.clone();
            t2.options.push(ColumnOption {
                id: None,
                name: "B-edit".to_string(),
                color: None,
                modified_time: 0,
                deleted_at: None,
            });
            t2.revision = 2;
            let err = v2::cas_save_template(tmp.path(), &t2, 1).unwrap_err();
            assert!(matches!(
                err,
                TemplateError::RevisionMismatch { expected: 1, found: 2 }
            ));
            // File on disk still has A's edit, not B's.
            let back = v2::read_template(tmp.path(), &t1.template_hash).unwrap();
            assert!(back.options.iter().any(|o| o.name == "A-edit"));
            assert!(!back.options.iter().any(|o| o.name == "B-edit"));
        }

        #[test]
        fn manifest_apply_entry_bumps_revision() {
            let mut m = v2::Manifest::default();
            assert_eq!(m.manifest_revision, 0);
            let t = fresh_template("Status", "vfx_2025");
            v2::apply_entry(&mut m, v2::entry_for(&t), 2000);
            assert_eq!(m.manifest_revision, 1);
            assert_eq!(m.templates.len(), 1);
            // Updating same uuid replaces, doesn't append.
            v2::apply_entry(&mut m, v2::entry_for(&t), 3000);
            assert_eq!(m.manifest_revision, 2);
            assert_eq!(m.templates.len(), 1);
        }

        #[test]
        fn manifest_lookup_matches_project_common_type_triple() {
            let mut m = v2::Manifest::default();
            v2::apply_entry(
                &mut m,
                v2::entry_for(&fresh_template("Status", "vfx_2025")),
                1000,
            );
            v2::apply_entry(
                &mut m,
                v2::entry_for(&fresh_template("Status", "editorial_2025")),
                1000,
            );
            // Match in vfx_2025
            assert!(v2::lookup_in_manifest(&m, "vfx_2025", "status", "option").is_some());
            // No match in a third project
            assert!(v2::lookup_in_manifest(&m, "color_2025", "status", "option").is_none());
            // No match for wrong column_type
            assert!(v2::lookup_in_manifest(&m, "vfx_2025", "status", "text").is_none());
        }

        #[test]
        fn pull_to_local_cache_diffs_by_revision() {
            let remote = TempDir::new().unwrap();
            let local = TempDir::new().unwrap();
            // Write 2 templates + a manifest on the remote.
            let t_a = fresh_template("Status", "vfx_2025");
            let t_b = fresh_template("Priority", "vfx_2025");
            v2::write_template_atomic(remote.path(), &t_a).unwrap();
            v2::write_template_atomic(remote.path(), &t_b).unwrap();
            let mut remote_m = v2::Manifest::default();
            v2::apply_entry(&mut remote_m, v2::entry_for(&t_a), 1000);
            v2::apply_entry(&mut remote_m, v2::entry_for(&t_b), 1000);
            v2::write_manifest_atomic(remote.path(), &remote_m).unwrap();

            // First pull — local is empty, expect both files pulled.
            let pulled = v2::pull_to_local_cache(remote.path(), local.path()).unwrap();
            assert_eq!(pulled, 2);
            assert!(v2::read_template(local.path(), &t_a.template_hash).is_ok());

            // Second pull — local matches remote, expect 0 files
            // pulled (manifest still mirrored, but no template
            // re-fetches).
            let pulled = v2::pull_to_local_cache(remote.path(), local.path()).unwrap();
            assert_eq!(pulled, 0);

            // Bump remote t_a's revision; pull should fetch only t_a.
            let mut t_a_v2 = t_a.clone();
            t_a_v2.options.push(ColumnOption {
                id: None,
                name: "newopt".to_string(),
                color: None,
                modified_time: 0,
                deleted_at: None,
            });
            t_a_v2.revision = 2;
            v2::cas_save_template(remote.path(), &t_a_v2, 1).unwrap();
            v2::apply_entry(&mut remote_m, v2::entry_for(&t_a_v2), 2000);
            v2::cas_save_manifest(remote.path(), &remote_m, 2).unwrap();

            let pulled = v2::pull_to_local_cache(remote.path(), local.path()).unwrap();
            assert_eq!(pulled, 1);
            let local_a = v2::read_template(local.path(), &t_a.template_hash).unwrap();
            assert_eq!(local_a.revision, 2);
        }

        #[test]
        fn list_templates_skips_manifest() {
            let tmp = TempDir::new().unwrap();
            let t = fresh_template("Status", "vfx_2025");
            v2::write_template_atomic(tmp.path(), &t).unwrap();
            let m = v2::Manifest::default();
            v2::write_manifest_atomic(tmp.path(), &m).unwrap();
            let listed = v2::list_templates(tmp.path()).unwrap();
            assert_eq!(listed.len(), 1);
            assert_eq!(listed[0].template_hash, t.template_hash);
        }

        #[test]
        fn legacy_path_migration_moves_only_v1_files() {
            let farm = TempDir::new().unwrap();
            let legacy = farm.path().join("ufb/templates");
            fs::create_dir_all(&legacy).unwrap();
            // v1 preset file (schemaVersion: 1) → should be moved
            // to the new ufb/presets/ location.
            fs::write(
                legacy.join("status.json"),
                r#"{"schemaVersion": 1, "uniqueName": "status", "displayName": "Status"}"#,
            )
            .unwrap();
            // v2 template file (schemaVersion: 2) erroneously placed
            // in the legacy dir → must be left in place; v2 will own
            // this dir.
            fs::write(
                legacy.join("550e8400-e29b-41d4-a716-446655440000.json"),
                r#"{"schemaVersion": 2, "templateHash": "550e8400-..."}"#,
            )
            .unwrap();
            // v2 manifest → must be left in place.
            fs::write(legacy.join("_index.json"), r#"{"manifestRevision": 1}"#).unwrap();

            let presets_dir = super::super::resolve_dir(farm.path().to_str().unwrap()).unwrap();
            // v1 moved.
            assert!(presets_dir.join("status.json").exists());
            assert!(!legacy.join("status.json").exists());
            // v2 + manifest stayed.
            assert!(legacy.join("550e8400-e29b-41d4-a716-446655440000.json").exists());
            assert!(legacy.join("_index.json").exists());
            // Presets dir must NOT have the v2 file accidentally
            // mirrored over.
            assert!(!presets_dir.join("550e8400-e29b-41d4-a716-446655440000.json").exists());
        }

        #[test]
        fn recover_misplaced_v2_files_moves_them_back() {
            let farm = TempDir::new().unwrap();
            // Pretend an earlier buggy migration moved a v2 file
            // into presets/. Place it there now.
            let presets_dir = farm.path().join("ufb/presets");
            fs::create_dir_all(&presets_dir).unwrap();
            fs::write(
                presets_dir.join("550e8400-e29b-41d4-a716-446655440000.json"),
                r#"{"schemaVersion": 2, "templateHash": "550e8400-..."}"#,
            )
            .unwrap();
            // Also drop a real v1 preset alongside — must NOT be
            // moved by the recovery (recovery only touches v2).
            fs::write(
                presets_dir.join("legitimate_preset.json"),
                r#"{"schemaVersion": 1}"#,
            )
            .unwrap();

            // Resolve v2 templates dir → triggers recovery.
            let v2_dir = v2::resolve_dir(farm.path().to_str().unwrap()).unwrap();

            // v2 file ended up in templates/.
            assert!(v2_dir.join("550e8400-e29b-41d4-a716-446655440000.json").exists());
            // v2 file removed from presets/.
            assert!(!presets_dir.join("550e8400-e29b-41d4-a716-446655440000.json").exists());
            // v1 preset stayed put.
            assert!(presets_dir.join("legitimate_preset.json").exists());
        }

        #[test]
        fn lookup_or_mint_returns_existing_match() {
            let tmp = TempDir::new().unwrap();
            let first = v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![],
                1000,
                false,
            )
            .unwrap();
            // Second call with same triple should return the same
            // template (same uuid).
            let second = v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![],
                2000,
                false,
            )
            .unwrap();
            assert_eq!(first.template_hash, second.template_hash);
            // Manifest should have only ONE entry for this triple.
            let m = v2::read_manifest(tmp.path()).unwrap();
            assert_eq!(m.templates.len(), 1);
        }

        #[test]
        fn lookup_or_mint_reuse_bumps_last_used_at() {
            let tmp = TempDir::new().unwrap();
            v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![],
                1000,
                false,
            )
            .unwrap();
            // After the initial mint, lastUsedAt == createdAt == 1000.
            let m1 = v2::read_manifest(tmp.path()).unwrap();
            assert_eq!(m1.templates[0].last_used_at, 1000);

            // Reuse at a later timestamp — manifest's lastUsedAt
            // should bump to the reuse moment (5000), giving the
            // future picker UI a real "recently active" signal.
            v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![],
                5000,
                false,
            )
            .unwrap();
            let m2 = v2::read_manifest(tmp.path()).unwrap();
            assert_eq!(m2.templates[0].last_used_at, 5000);
            // Manifest revision also bumped (not just the
            // template's content; the bump uses cas_save_manifest).
            assert!(m2.manifest_revision > m1.manifest_revision);
        }

        #[test]
        fn bump_last_used_at_skips_when_already_newer() {
            let tmp = TempDir::new().unwrap();
            let t = v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![],
                5000,
                false,
            )
            .unwrap();
            let m_before = v2::read_manifest(tmp.path()).unwrap();
            // Bump with an EARLIER timestamp — should be a no-op
            // (manifest revision unchanged, last_used_at unchanged).
            v2::bump_last_used_at(tmp.path(), &t.template_hash, 1000).unwrap();
            let m_after = v2::read_manifest(tmp.path()).unwrap();
            assert_eq!(
                m_after.manifest_revision, m_before.manifest_revision,
                "manifest should NOT bump when timestamp is older"
            );
            assert_eq!(m_after.templates[0].last_used_at, 5000);
        }

        #[test]
        fn lookup_or_mint_force_new_creates_distinct_template() {
            let tmp = TempDir::new().unwrap();
            let first = v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![],
                1000,
                false,
            )
            .unwrap();
            // force_new = true: even though a "Status" exists, mint a
            // fresh UUID. The "Create new" path of the lookup-on-
            // create dialog uses this to fork.
            let forked = v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![],
                2000,
                true,
            )
            .unwrap();
            assert_ne!(first.template_hash, forked.template_hash);
            let m = v2::read_manifest(tmp.path()).unwrap();
            assert_eq!(m.templates.len(), 2);
        }

        #[test]
        fn update_template_options_bumps_revision() {
            let tmp = TempDir::new().unwrap();
            let t = v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![ColumnOption {
                    id: None,
                    name: "Todo".into(),
                    color: None,
                    modified_time: 0,
                    deleted_at: None,
                }],
                1000,
                false,
            )
            .unwrap();
            // Update with a second option.
            let updated = v2::update_template_options(
                tmp.path(),
                &t.template_hash,
                None,
                vec![
                    ColumnOption {
                        id: None,
                        name: "Todo".into(),
                        color: None,
                        modified_time: 0,
                        deleted_at: None,
                    },
                    ColumnOption {
                        id: None,
                        name: "Doing".into(),
                        color: None,
                        modified_time: 0,
                        deleted_at: None,
                    },
                ],
                1, // expected revision
                2000,
            )
            .unwrap();
            assert_eq!(updated.revision, 2);
            assert_eq!(updated.options.len(), 2);
            // Manifest entry's revision also bumped.
            let m = v2::read_manifest(tmp.path()).unwrap();
            assert_eq!(m.templates[0].revision, 2);
        }

        #[test]
        fn update_template_options_rejects_stale_revision() {
            let tmp = TempDir::new().unwrap();
            let t = v2::lookup_or_mint(
                tmp.path(),
                "Status",
                "status",
                "vfx_2025",
                "option",
                None,
                vec![],
                1000,
                false,
            )
            .unwrap();
            // First update lands at revision 2.
            v2::update_template_options(
                tmp.path(),
                &t.template_hash,
                None,
                vec![ColumnOption {
                    id: None,
                    name: "X".into(),
                    color: None,
                    modified_time: 0,
                    deleted_at: None,
                }],
                1,
                2000,
            )
            .unwrap();
            // Stale-revision-1 update attempt rejects.
            let err = v2::update_template_options(
                tmp.path(),
                &t.template_hash,
                None,
                vec![ColumnOption {
                    id: None,
                    name: "Y".into(),
                    color: None,
                    modified_time: 0,
                    deleted_at: None,
                }],
                1,
                3000,
            )
            .unwrap_err();
            assert!(matches!(
                err,
                TemplateError::RevisionMismatch { expected: 1, found: 2 }
            ));
        }
    }
}
