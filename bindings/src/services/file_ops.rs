//! `FileOps` QObject — copy/move/uri conversion for plan-Phase 8 drag/drop.
//!
//! Path-first scope: just the surface the QML drag/drop handlers need.
//! Copy/move are fire-and-forget — errors land in the log, the proper
//! signal-correlation pattern with op IDs comes with plan-Phase 13's
//! status-bar progress strip.
//!
//! Other FileOps surfaces (rename, delete, clipboard, reveal, open)
//! land in their own commits as the relevant UI features come online.

use crate::runtime::shared_runtime;
use serde::Deserialize;
use std::path::Path;
use std::pin::Pin;
use std::sync::{Arc, Mutex, OnceLock};
use ufb_core::events::{FileOpCompleted, FileOpError, FileOpProgress, FileOpStarted, FileOpsEvents};
use ufb_core::file_ops;

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
        #[qproperty(QString, active_ops_json)]
        type FileOps = super::FileOpsRust;

        /// Convert a JSON array of absolute paths into a single
        /// `text/uri-list`-formatted string (newline-separated `file://`
        /// URIs, RFC 2483).
        #[qinvokable]
        fn to_uri_list(self: &FileOps, paths_json: QString) -> QString;

        /// Convert a JSON array of URL strings (as QML's
        /// `DropArea.urls` exposes them) into a JSON array of local
        /// filesystem paths. Strips the `file://` scheme, percent-
        /// decodes, and normalises Windows drive paths.
        #[qinvokable]
        fn urls_to_paths(self: &FileOps, urls_json: QString) -> QString;

        /// True if both paths live on the same volume — used by drop
        /// handlers to pick the default action (move same-volume,
        /// copy cross-volume). Windows: compares drive letters
        /// case-insensitively. Macos: compares the leading mount root.
        #[qinvokable]
        fn same_volume(self: &FileOps, a: QString, b: QString) -> bool;

        /// Async copy. Spawned onto the shared tokio runtime; returns
        /// immediately. Progress is published to `active_ops_json` and
        /// surfaced via the status-bar strip.
        #[qinvokable]
        fn copy_files(
            self: Pin<&mut FileOps>,
            paths_json: QString,
            dest: QString,
        );

        /// Async move. Same fire-and-forget shape as `copy_files`.
        #[qinvokable]
        fn move_files(
            self: Pin<&mut FileOps>,
            paths_json: QString,
            dest: QString,
        );

        /// Async delete-to-trash. Progress published via active_ops_json.
        #[qinvokable]
        fn delete_to_trash(self: Pin<&mut FileOps>, paths_json: QString);

        /// Open `path` in the OS's default application (or descend
        /// into it via the file manager if it's a directory).
        #[qinvokable]
        fn open_file(self: &FileOps, path: QString);

        /// Reveal `path` in Explorer / Finder (selects it in the
        /// parent folder).
        #[qinvokable]
        fn reveal_in_file_manager(self: &FileOps, path: QString);

        /// Reveal multiple paths in Finder, all selected in their
        /// (single) parent folder. macOS uses
        /// `NSWorkspace.activateFileViewerSelectingURLs:` for true
        /// multi-select reveal; Windows currently reveals only the
        /// first path. `paths_json` is a JSON array.
        #[qinvokable]
        fn reveal_paths_in_file_manager(self: &FileOps, paths_json: QString);

        /// Create directory at `path`. Idempotent (mkdir -p semantics).
        /// Returns the empty string on success, an error message on
        /// failure. Emits `dirs_changed` for the parent on success.
        #[qinvokable]
        fn create_directory(self: Pin<&mut FileOps>, path: QString) -> QString;

        /// Rename `old_path` → `new_path`. Returns "" on success or an
        /// error message. Emits `dirs_changed` for the affected parent(s).
        #[qinvokable]
        fn rename_path(
            self: Pin<&mut FileOps>,
            old_path: QString,
            new_path: QString,
        ) -> QString;

        /// Put `paths_json` (JSON array of paths) on the clipboard
        /// for a Copy. Sync.
        #[qinvokable]
        fn clipboard_copy_paths(self: &FileOps, paths_json: QString) -> bool;

        /// Put `paths_json` on the clipboard for a Cut (Explorer
        /// interprets the next paste as a move).
        #[qinvokable]
        fn clipboard_cut_paths(self: &FileOps, paths_json: QString) -> bool;

        /// Read paths from the clipboard. Returns a JSON array of
        /// path strings (empty array if nothing usable is available).
        #[qinvokable]
        fn clipboard_paste_paths(self: &FileOps) -> QString;

        /// Read paths AND move-vs-copy intent from the clipboard.
        /// Returns a JSON object `{"paths": [...], "effect": "copy"|"move"}`.
        /// Empty paths array if the clipboard has nothing usable.
        ///
        /// This is the call paste actions should use — it transparently
        /// honours Explorer's Cut (Windows DROPEFFECT_MOVE) so cross-app
        /// cut/paste = move, and it shares cut state across panes / tabs
        /// via a process-global registry so a Cut in one browser and a
        /// Paste in another will move (not copy).
        #[qinvokable]
        fn clipboard_paste_intent(self: &FileOps) -> QString;

        /// Non-blocking "is there anything Paste could act on?" probe
        /// for enabling menu items. Unlike `clipboard_paste_intent` this
        /// never renders clipboard data (no cross-process wait on the
        /// clipboard owner), so it's safe to call on every menu open.
        /// May return true for text that turns out not to be paths.
        #[qinvokable]
        fn clipboard_has_paths(self: &FileOps) -> bool;

        /// Put a plain-text string on the system clipboard. Used by
        /// "Copy Path" / "Copy Filename" menu actions.
        #[qinvokable]
        fn clipboard_copy_text(self: &FileOps, text: QString) -> bool;

        /// Pop the OS-native shell context menu (Windows IContextMenu
        /// / macOS Finder menu) at the current cursor position for
        /// `path`. Returns immediately: on Windows the menu runs on
        /// its own STA thread so the GUI thread keeps pumping while
        /// the menu is open.
        #[qinvokable]
        fn show_shell_menu(self: &FileOps, path: QString);

        /// Given `parent` and a `base` filename, return a full path
        /// inside `parent` that doesn't collide with anything: tries
        /// `parent/base` first, then `parent/base{a..z}`. Used by
        /// the New Date / New Time folder actions. Returns `parent/base`
        /// (will collide) if all 26 letter slots are also taken.
        #[qinvokable]
        fn find_available_path(self: &FileOps, parent: QString, base: QString) -> QString;

        /// Create a UFB-folder directory using the
        /// `{date_prefix}{letter}_{label}` template. The letter slot
        /// is shared across the date prefix (so `260518a_alpha`
        /// consumes `a` regardless of label) — same semantics as
        /// `create_date_prefixed_item`. Returns the new path on
        /// success or "" on error (all 26 slots taken, parent missing,
        /// IO failure).
        #[qinvokable]
        fn create_ufb_folder(
            self: &FileOps,
            parent: QString,
            date_prefix: QString,
            label: QString,
        ) -> QString;

        /// Build a `ufb:///{os}/{url-encoded-path}` URI for `path`.
        /// Format used for portable references in chat / notes / mesh
        /// peers; recipients use path-mappings (Settings → Paths) to
        /// resolve to a local path on their machine.
        #[qinvokable]
        fn build_ufb_uri(self: &FileOps, path: QString) -> QString;

        /// Build a `union:///{os}/{url-encoded-path}` URI. Same
        /// shape as ufb://; intended for "open in external file
        /// manager" rather than in-app navigation.
        #[qinvokable]
        fn build_union_uri(self: &FileOps, path: QString) -> QString;

        /// Convert a native path to a `file://` URL (drive-letter,
        /// UNC, and POSIX forms all handled). Returns "" for a path
        /// with no safe URL form (e.g. drive-less Windows paths).
        /// Single-path face of the same sanitizer behind to_uri_list —
        /// use this instead of hand-building "file://" + path in QML,
        /// which breaks on Windows (drive letter parses as URL host).
        #[qinvokable]
        fn to_file_url(self: &FileOps, path: QString) -> QString;

        /// Resolve a `ufb://` / `union://` URI (or a plain native
        /// path) to a path local to this OS, applying the user's
        /// settings.json `pathMappings`. Used both for clicking
        /// `ufb://` links produced on another OS and for command-
        /// line arguments. Returns the input as-is if it's already
        /// a local path; returns "" on parse failure.
        #[qinvokable]
        fn resolve_ufb_uri(self: &FileOps, uri_or_path: QString) -> QString;

        /// Returns "B" or "C" — the FolderTabView layout mode for the
        /// given folder. Probes up to 10 child directories looking for
        /// shot-style project/renders structure. Sync.
        #[qinvokable]
        fn detect_folder_layout_mode(self: &FileOps, folder_path: QString) -> QString;

        /// Returns the path of the first PROJECT-named subdirectory
        /// of `item_path` (e.g. project/, scenes/, nuke/, ae/, …),
        /// or the empty string if none.
        #[qinvokable]
        fn find_project_subdir(self: &FileOps, item_path: QString) -> QString;

        /// Returns the path of the first RENDER-named subdirectory
        /// (renders/, comp/, output/, deliverables/, …), or "".
        #[qinvokable]
        fn find_render_subdir(self: &FileOps, item_path: QString) -> QString;

        /// Returns the path of the first PROXIES-named subdirectory
        /// (proxies/, proxy/), or "". Drives the AE tab's bottom-pane
        /// renders/proxies subtab strip in FolderTabView.
        #[qinvokable]
        fn find_proxies_subdir(self: &FileOps, item_path: QString) -> QString;

        /// Returns the Add Item dialog mode for `folder_name`. One of
        /// "shot" | "date_prefixed" | "folder" | "none". QML hides the
        /// Add button when "none"; for other modes it shapes the
        /// dialog label/placeholder.
        #[qinvokable]
        fn detect_add_mode(self: &FileOps, folder_name: QString) -> QString;

        /// Find the bundled shot template directory for `folder_type`
        /// (e.g. "ae" → ".../assets/projectTemplate/ae/_t_project_name").
        /// Returns "" if no template is available.
        #[qinvokable]
        fn resolve_shot_template(self: &FileOps, folder_type: QString) -> QString;

        /// Copy `template_dir` into `dest_parent/{item_name}` recursively,
        /// renaming any `template.<ext>` / `_template.<ext>` files to
        /// `{item_name}_v001.<ext>` and skipping .gitkeep markers.
        /// Returns the new item path on success or an error string.
        #[qinvokable]
        fn copy_template_to(
            self: &FileOps,
            template_dir: QString,
            dest_parent: QString,
            item_name: QString,
        ) -> QString;

        /// Create a date-prefixed item folder using the legacy
        /// `YYMMDD<letter>_<name>` format. Letter is shared across
        /// today's items regardless of base name (so `271015a_foo`
        /// → next is `271015b_bar`). Returns the new path or "" on
        /// error.
        #[qinvokable]
        fn create_date_prefixed_item(
            self: &FileOps,
            parent: QString,
            base_name: QString,
        ) -> QString;

        /// Create a date-prefixed note FILE using the same
        /// `YYMMDD<letter>_<name>` slot walk, but with a `.mndb`
        /// extension (WIP UFB notes format — created empty). Returns
        /// the new path or "" on error.
        #[qinvokable]
        fn create_date_prefixed_note(
            self: &FileOps,
            parent: QString,
            base_name: QString,
        ) -> QString;

        /// Broadcast emitted when a file operation changes the contents
        /// of one or more directories. `dirs_json` is a JSON array of
        /// native directory paths. Every directory view listens and
        /// refreshes itself if its current path is among them — this is
        /// how cross-pane moves and sidebar-drop targets stay in sync.
        #[qsignal]
        fn dirs_changed(self: Pin<&mut FileOps>, dirs_json: QString);
    }

    // Threading lets the FileOpsEvents forwarder (running on tokio
    // tasks) queue updates to active_ops_json back onto the Qt thread.
    impl cxx_qt::Threading for FileOps {}
}

pub struct FileOpsRust {
    pub active_ops_json: cxx_qt_lib::QString,
}

impl Default for FileOpsRust {
    fn default() -> Self {
        Self {
            active_ops_json: cxx_qt_lib::QString::from("[]"),
        }
    }
}

impl qobject::FileOps {
    fn to_uri_list(self: &qobject::FileOps, paths_json: cxx_qt_lib::QString) -> cxx_qt_lib::QString {
        let paths: Vec<String> = serde_json::from_str(&paths_json.to_string()).unwrap_or_default();
        // RFC 2483 text/uri-list: each URI on its own line terminated
        // by CRLF, INCLUDING the last one. Without a trailing CRLF
        // some Qt code paths drop the final URL when translating to
        // the native Windows CF_HDROP drop format — symptom on
        // multi-select drag-out is that only one file gets carried
        // even though the source MIME string had several. Always
        // emit the closing CRLF.
        let mut out = String::new();
        for p in paths {
            // Drive-less / unrepresentable paths return None and are
            // skipped — emitting `file:////…` for them wedges Windows
            // drag-and-drop. See ufb_core::utils::path_to_file_url.
            if let Some(url) = ufb_core::utils::path_to_file_url(&p) {
                out.push_str(&url);
                out.push_str("\r\n");
            }
        }
        cxx_qt_lib::QString::from(&out)
    }

    fn urls_to_paths(
        self: &qobject::FileOps,
        urls_json: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let urls: Vec<String> = serde_json::from_str(&urls_json.to_string()).unwrap_or_default();
        let paths: Vec<String> = urls.into_iter().filter_map(|u| file_url_to_path(&u)).collect();
        let json = serde_json::to_string(&paths).unwrap_or_else(|_| "[]".into());
        cxx_qt_lib::QString::from(&json)
    }

    fn same_volume(
        self: &qobject::FileOps,
        a: cxx_qt_lib::QString,
        b: cxx_qt_lib::QString,
    ) -> bool {
        same_volume_impl(&a.to_string(), &b.to_string())
    }

    fn copy_files(
        self: Pin<&mut qobject::FileOps>,
        paths_json: cxx_qt_lib::QString,
        dest: cxx_qt_lib::QString,
    ) {
        let Some(req) = parse_copy_move(&paths_json, &dest, "copy") else {
            return;
        };
        ensure_events_forwarder(self);
        let events = shared_events();
        let _guard = shared_runtime().enter();
        tokio::spawn(async move {
            if let Err(e) = file_ops::copy_files(req.sources, req.dest, events).await {
                log::warn!("FileOps.copy_files: {}", e);
            }
        });
    }

    fn move_files(
        self: Pin<&mut qobject::FileOps>,
        paths_json: cxx_qt_lib::QString,
        dest: cxx_qt_lib::QString,
    ) {
        let Some(req) = parse_copy_move(&paths_json, &dest, "move") else {
            return;
        };
        ensure_events_forwarder(self);
        let events = shared_events();
        let _guard = shared_runtime().enter();
        tokio::spawn(async move {
            if let Err(e) = file_ops::move_files(req.sources, req.dest, events).await {
                log::warn!("FileOps.move_files: {}", e);
            }
        });
    }

    fn delete_to_trash(self: Pin<&mut qobject::FileOps>, paths_json: cxx_qt_lib::QString) {
        let paths: Vec<String> = match serde_json::from_str(&paths_json.to_string()) {
            Ok(v) => v,
            Err(e) => {
                log::warn!("FileOps.delete_to_trash: bad paths_json: {}", e);
                return;
            }
        };
        if paths.is_empty() {
            return;
        }
        log::info!("FileOps.delete_to_trash: {} paths", paths.len());
        ensure_events_forwarder(self);
        let events = shared_events();
        let _guard = shared_runtime().enter();
        tokio::spawn(async move {
            if let Err(e) = file_ops::delete_to_trash_with_progress(paths, events).await {
                log::warn!("FileOps.delete_to_trash: {}", e);
            }
        });
    }

    fn open_file(self: &qobject::FileOps, path: cxx_qt_lib::QString) {
        let p = path.to_string();
        if let Err(e) = file_ops::open_file(&p) {
            log::warn!("FileOps.open_file({}): {}", p, e);
        }
    }

    fn reveal_in_file_manager(self: &qobject::FileOps, path: cxx_qt_lib::QString) {
        let p = path.to_string();
        if let Err(e) = file_ops::reveal_in_file_manager(&p) {
            log::warn!("FileOps.reveal_in_file_manager({}): {}", p, e);
        }
    }

    fn reveal_paths_in_file_manager(
        self: &qobject::FileOps,
        paths_json: cxx_qt_lib::QString,
    ) {
        let paths: Vec<String> = match serde_json::from_str(&paths_json.to_string()) {
            Ok(v) => v,
            Err(e) => {
                log::warn!("FileOps.reveal_paths_in_file_manager: bad JSON: {}", e);
                return;
            }
        };
        if let Err(e) = file_ops::reveal_paths_in_file_manager(&paths) {
            log::warn!("FileOps.reveal_paths_in_file_manager: {}", e);
        }
    }

    fn create_directory(
        mut self: Pin<&mut qobject::FileOps>,
        path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let p = path.to_string();
        match file_ops::create_directory(&p) {
            Ok(()) => {
                emit_dirs_changed(self.as_mut(), &[&p]);
                cxx_qt_lib::QString::from("")
            }
            Err(e) => {
                log::warn!("FileOps.create_directory({}): {}", p, e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn rename_path(
        mut self: Pin<&mut qobject::FileOps>,
        old_path: cxx_qt_lib::QString,
        new_path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let old_s = old_path.to_string();
        let new_s = new_path.to_string();
        match file_ops::rename_path(&old_s, &new_s) {
            Ok(()) => {
                // Both parents (same for an in-place rename; distinct if
                // the rename also moved the item across directories).
                emit_dirs_changed(self.as_mut(), &[&old_s, &new_s]);
                cxx_qt_lib::QString::from("")
            }
            Err(e) => {
                log::warn!("FileOps.rename_path({} → {}): {}", old_s, new_s, e);
                cxx_qt_lib::QString::from(&e)
            }
        }
    }

    fn clipboard_copy_paths(self: &qobject::FileOps, paths_json: cxx_qt_lib::QString) -> bool {
        let paths: Vec<String> = match serde_json::from_str(&paths_json.to_string()) {
            Ok(v) => v,
            Err(_) => return false,
        };
        if paths.is_empty() {
            return false;
        }
        log::info!(
            "FileOps.clipboard_copy_paths: {} path(s) — first={:?}",
            paths.len(),
            paths.first()
        );
        match file_ops::clipboard_copy_paths(&paths) {
            Ok(()) => true,
            Err(e) => {
                log::warn!("FileOps.clipboard_copy_paths: {}", e);
                false
            }
        }
    }

    fn clipboard_cut_paths(self: &qobject::FileOps, paths_json: cxx_qt_lib::QString) -> bool {
        let paths: Vec<String> = match serde_json::from_str(&paths_json.to_string()) {
            Ok(v) => v,
            Err(_) => return false,
        };
        if paths.is_empty() {
            return false;
        }
        log::info!(
            "FileOps.clipboard_cut_paths: {} path(s) — first={:?}",
            paths.len(),
            paths.first()
        );
        match file_ops::clipboard_cut_paths(&paths) {
            Ok(()) => true,
            Err(e) => {
                log::warn!("FileOps.clipboard_cut_paths: {}", e);
                false
            }
        }
    }

    fn clipboard_paste_paths(self: &qobject::FileOps) -> cxx_qt_lib::QString {
        match file_ops::clipboard_paste_paths() {
            Ok(paths) => {
                log::info!(
                    "FileOps.clipboard_paste_paths: {} path(s) — first={:?}",
                    paths.len(),
                    paths.first()
                );
                let json = serde_json::to_string(&paths).unwrap_or_else(|_| "[]".into());
                cxx_qt_lib::QString::from(&json)
            }
            Err(e) => {
                log::debug!("FileOps.clipboard_paste_paths: {}", e);
                cxx_qt_lib::QString::from("[]")
            }
        }
    }

    fn clipboard_paste_intent(self: &qobject::FileOps) -> cxx_qt_lib::QString {
        match file_ops::clipboard_paste_intent() {
            Ok(intent) => {
                log::info!(
                    "FileOps.clipboard_paste_intent: {} path(s), effect={:?} — first={:?}",
                    intent.paths.len(),
                    intent.effect,
                    intent.paths.first()
                );
                let json = serde_json::to_string(&intent)
                    .unwrap_or_else(|_| r#"{"paths":[],"effect":"copy"}"#.into());
                cxx_qt_lib::QString::from(&json)
            }
            Err(e) => {
                log::debug!("FileOps.clipboard_paste_intent: {}", e);
                cxx_qt_lib::QString::from(r#"{"paths":[],"effect":"copy"}"#)
            }
        }
    }

    fn clipboard_has_paths(self: &qobject::FileOps) -> bool {
        file_ops::clipboard_has_paths()
    }

    fn clipboard_copy_text(self: &qobject::FileOps, text: cxx_qt_lib::QString) -> bool {
        match file_ops::clipboard_copy_text(&text.to_string()) {
            Ok(()) => true,
            Err(e) => {
                log::warn!("FileOps.clipboard_copy_text: {}", e);
                false
            }
        }
    }

    fn show_shell_menu(self: &qobject::FileOps, path: cxx_qt_lib::QString) {
        let p = path.to_string();
        if let Err(e) = ufb_core::shell_context_menu::show_shell_context_menu(&p) {
            log::warn!("FileOps.show_shell_menu({}): {}", p, e);
        }
    }

    fn find_available_path(
        self: &qobject::FileOps,
        parent: cxx_qt_lib::QString,
        base: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let result = file_ops::find_available_path(&parent.to_string(), &base.to_string());
        cxx_qt_lib::QString::from(&result)
    }

    fn create_ufb_folder(
        self: &qobject::FileOps,
        parent: cxx_qt_lib::QString,
        date_prefix: cxx_qt_lib::QString,
        label: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let result = file_ops::create_ufb_folder(
            &parent.to_string(),
            &date_prefix.to_string(),
            &label.to_string(),
        );
        match result {
            Ok(path) => cxx_qt_lib::QString::from(&path),
            Err(e) => {
                log::warn!("FileOps.create_ufb_folder: {}", e);
                cxx_qt_lib::QString::from("")
            }
        }
    }

    fn build_ufb_uri(self: &qobject::FileOps, path: cxx_qt_lib::QString) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(&ufb_core::utils::build_path_uri(&path.to_string()))
    }

    fn build_union_uri(self: &qobject::FileOps, path: cxx_qt_lib::QString) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(&ufb_core::utils::build_union_uri(&path.to_string()))
    }

    fn to_file_url(self: &qobject::FileOps, path: cxx_qt_lib::QString) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(
            &ufb_core::utils::path_to_file_url(&path.to_string()).unwrap_or_default(),
        )
    }

    fn resolve_ufb_uri(
        self: &qobject::FileOps,
        uri_or_path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let s = uri_or_path.to_string();
        let trimmed = s.trim();
        if trimmed.is_empty() {
            return cxx_qt_lib::QString::from("");
        }
        // Plain native path - just normalise separators for this OS.
        let is_uri = trimmed.starts_with("ufb://") || trimmed.starts_with("union://");
        if !is_uri {
            return cxx_qt_lib::QString::from(
                &ufb_core::utils::to_native_path(trimmed, ufb_core::utils::current_os_tag())
            );
        }
        // URI path - parse, then translate using the user's mappings.
        let Some(parsed) = ufb_core::utils::parse_path_uri(trimmed) else {
            log::warn!("FileOps.resolve_ufb_uri: parse failed for {:?}", trimmed);
            return cxx_qt_lib::QString::from("");
        };
        // Pull the live path mappings from the settings cache.
        let settings_arc = crate::services::settings::shared_settings();
        let settings = settings_arc.read().unwrap();
        let translated = ufb_core::utils::translate_path_to(
            &parsed.source_os,
            ufb_core::utils::current_os_tag(),
            &parsed.path,
            &settings.path_mappings,
        );
        cxx_qt_lib::QString::from(&translated)
    }

    fn detect_folder_layout_mode(
        self: &qobject::FileOps,
        folder_path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(file_ops::detect_folder_layout_mode(
            &folder_path.to_string(),
        ))
    }

    fn find_project_subdir(
        self: &qobject::FileOps,
        item_path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(
            &file_ops::find_project_subdir(&item_path.to_string()).unwrap_or_default(),
        )
    }

    fn find_render_subdir(
        self: &qobject::FileOps,
        item_path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(
            &file_ops::find_render_subdir(&item_path.to_string()).unwrap_or_default(),
        )
    }

    fn find_proxies_subdir(
        self: &qobject::FileOps,
        item_path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(
            &file_ops::find_proxies_subdir(&item_path.to_string()).unwrap_or_default(),
        )
    }

    fn detect_add_mode(
        self: &qobject::FileOps,
        folder_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(file_ops::detect_add_mode(&folder_name.to_string()))
    }

    fn resolve_shot_template(
        self: &qobject::FileOps,
        folder_type: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(
            &file_ops::resolve_shot_template(&folder_type.to_string()).unwrap_or_default(),
        )
    }

    fn copy_template_to(
        self: &qobject::FileOps,
        template_dir: cxx_qt_lib::QString,
        dest_parent: cxx_qt_lib::QString,
        item_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let result = file_ops::copy_template_to(
            &template_dir.to_string(),
            &dest_parent.to_string(),
            &item_name.to_string(),
        );
        match result {
            Ok(path) => cxx_qt_lib::QString::from(&path),
            Err(e) => {
                log::warn!("FileOps.copy_template_to: {}", e);
                cxx_qt_lib::QString::from("")
            }
        }
    }

    fn create_date_prefixed_item(
        self: &qobject::FileOps,
        parent: cxx_qt_lib::QString,
        base_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let result = file_ops::create_date_prefixed_item(
            &parent.to_string(),
            &base_name.to_string(),
        );
        match result {
            Ok(path) => cxx_qt_lib::QString::from(&path),
            Err(e) => {
                log::warn!("FileOps.create_date_prefixed_item: {}", e);
                cxx_qt_lib::QString::from("")
            }
        }
    }

    fn create_date_prefixed_note(
        self: &qobject::FileOps,
        parent: cxx_qt_lib::QString,
        base_name: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let result = file_ops::create_date_prefixed_note(
            &parent.to_string(),
            &base_name.to_string(),
        );
        match result {
            Ok(path) => cxx_qt_lib::QString::from(&path),
            Err(e) => {
                log::warn!("FileOps.create_date_prefixed_note: {}", e);
                cxx_qt_lib::QString::from("")
            }
        }
    }
}

// ─── helpers ──────────────────────────────────────────────────────────

#[derive(Deserialize)]
struct CopyMoveRequest {
    sources: Vec<String>,
    dest: String,
}

fn parse_copy_move(
    paths_json: &cxx_qt_lib::QString,
    dest: &cxx_qt_lib::QString,
    op: &str,
) -> Option<CopyMoveRequest> {
    let dest = dest.to_string();
    let sources: Vec<String> = match serde_json::from_str(&paths_json.to_string()) {
        Ok(v) => v,
        Err(e) => {
            log::warn!("FileOps.{}_files: bad paths_json: {}", op, e);
            return None;
        }
    };
    if sources.is_empty() {
        log::debug!("FileOps.{}_files: empty source list, ignoring", op);
        return None;
    }
    if dest.is_empty() {
        log::warn!("FileOps.{}_files: empty dest, ignoring", op);
        return None;
    }
    log::info!(
        "FileOps.{}_files: {} sources → {}",
        op,
        sources.len(),
        dest
    );
    Some(CopyMoveRequest { sources, dest })
}

/// Reverse: a `file://...` URL → local path. Returns None for non-file
/// URLs or anything that can't be made into a path.
fn file_url_to_path(url: &str) -> Option<String> {
    let stripped = url.strip_prefix("file://")?;
    // Windows: `file:///C:/foo` → strip the leading `/` so we get `C:/foo`.
    let body = if cfg!(target_os = "windows") {
        stripped
            .strip_prefix('/')
            .filter(|s| s.len() >= 2 && s.as_bytes()[1] == b':')
            .unwrap_or(stripped)
    } else {
        stripped
    };
    let decoded = percent_decode(body);
    if decoded.is_empty() {
        return None;
    }
    Some(if cfg!(target_os = "windows") {
        decoded.replace('/', "\\")
    } else {
        decoded
    })
}

/// Tiny percent-decoder. We only need ASCII bytes back to ASCII; UTF-8
/// pass-through happens for free since we operate on bytes.
fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hi = (bytes[i + 1] as char).to_digit(16);
            let lo = (bytes[i + 2] as char).to_digit(16);
            if let (Some(h), Some(l)) = (hi, lo) {
                out.push(((h << 4) | l) as u8);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn same_volume_impl(a: &str, b: &str) -> bool {
    if a.is_empty() || b.is_empty() {
        return false;
    }
    if cfg!(target_os = "windows") {
        // Drive letter compare. Network paths starting with `\\server\share`
        // count as same-volume when the share matches.
        match (drive_or_unc(a), drive_or_unc(b)) {
            (Some(va), Some(vb)) => va.eq_ignore_ascii_case(&vb),
            _ => false,
        }
    } else {
        // Crude same-mount heuristic: walk parents of both paths until
        // the metadata's `dev` field matches. Without that field on
        // stable Rust we fall back to comparing the leading path
        // segment. Good enough for the path-first first cut.
        Path::new(a)
            .components()
            .next()
            .zip(Path::new(b).components().next())
            .map(|(ca, cb)| ca == cb)
            .unwrap_or(false)
    }
}

#[cfg(target_os = "windows")]
fn drive_or_unc(p: &str) -> Option<String> {
    let trimmed = p.trim_start();
    if trimmed.starts_with("\\\\") || trimmed.starts_with("//") {
        // \\server\share\rest → \\server\share
        let bytes = trimmed.as_bytes();
        let mut slash_count = 0;
        for (i, b) in bytes.iter().enumerate() {
            if *b == b'\\' || *b == b'/' {
                slash_count += 1;
                if slash_count == 4 {
                    return Some(trimmed[..i].to_string());
                }
            }
        }
        // No fourth slash — entire string is `\\server\share`.
        return Some(trimmed.to_string());
    }
    let bytes = trimmed.as_bytes();
    if bytes.len() >= 2 && bytes[1] == b':' && bytes[0].is_ascii_alphabetic() {
        Some(trimmed[..2].to_string())
    } else {
        None
    }
}

#[cfg(not(target_os = "windows"))]
fn drive_or_unc(_p: &str) -> Option<String> {
    None
}

// ─── Active-ops state + events forwarder ──────────────────────────────
// Tracks the live set of file operations for the status-bar strip.
// Operations enter on op_started, get progress ticks, and leave on
// op_completed / op_error. Per-op state is kept on the Rust side so
// the forwarder can rebuild the entire active_ops_json snapshot in one
// call (avoids partial-update races on the Qt thread).

#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct ActiveOp {
    id: String,
    operation: String,
    items_total: usize,
    items_done: usize,
    total_bytes: u64,
    copied_bytes: u64,
    current_file: String,
    /// 0.0–1.0. Computed from items_done/items_total when bytes_total
    /// is 0 (delete operations don't have byte totals).
    progress: f64,
}

/// Held on a OnceLock so the dispatcher (set on the Manager that
/// outlives any single QObject instance) can always find the latest
/// forwarder. The forwarder updates this same map and re-serialises
/// to active_ops_json on the Qt thread.
struct ActiveOpsState {
    ops: Mutex<std::collections::BTreeMap<String, ActiveOp>>,
    forwarder: Mutex<Option<Arc<FileOpsForwarder>>>,
}

fn active_ops_state() -> &'static ActiveOpsState {
    static STATE: OnceLock<ActiveOpsState> = OnceLock::new();
    STATE.get_or_init(|| ActiveOpsState {
        ops: Mutex::new(Default::default()),
        forwarder: Mutex::new(None),
    })
}

/// Returned from event invokables. Looks up the live forwarder and
/// routes calls there; falls back to a logging stub when no forwarder
/// is wired (events still print but don't reach the UI).
struct FileOpsDispatcher;

impl FileOpsEvents for FileOpsDispatcher {
    fn op_started(&self, started: FileOpStarted) {
        if let Some(f) = active_ops_state().forwarder.lock().unwrap().clone() {
            f.op_started(started);
        } else {
            log::info!("fileop {} started ({})", started.id, started.operation);
        }
    }
    fn op_progress(&self, progress: FileOpProgress) {
        if let Some(f) = active_ops_state().forwarder.lock().unwrap().clone() {
            f.op_progress(progress);
        }
    }
    fn op_completed(&self, completed: FileOpCompleted) {
        if let Some(f) = active_ops_state().forwarder.lock().unwrap().clone() {
            f.op_completed(completed);
        } else if completed.failed > 0 {
            log::warn!(
                "fileop {} completed: {} ok, {} failed",
                completed.id, completed.succeeded, completed.failed
            );
        }
    }
    fn op_error(&self, error: FileOpError) {
        if let Some(f) = active_ops_state().forwarder.lock().unwrap().clone() {
            f.op_error(error);
        } else {
            log::warn!("fileop {} error: {}", error.id, error.error);
        }
    }
}

fn shared_events() -> Arc<dyn FileOpsEvents> {
    Arc::new(FileOpsDispatcher)
}

/// Forwarder bound to a specific FileOps QObject's qt_thread.
/// Rebuilds active_ops_json on the Qt thread whenever an event lands.
struct FileOpsForwarder {
    qt_handle: cxx_qt::CxxQtThread<qobject::FileOps>,
}

impl FileOpsForwarder {
    fn write_snapshot(&self) {
        let snapshot = {
            let ops = active_ops_state().ops.lock().unwrap();
            let list: Vec<&ActiveOp> = ops.values().collect();
            serde_json::to_string(&list).unwrap_or_else(|_| "[]".into())
        };
        let _ = self.qt_handle.queue(move |mut fo| {
            fo.as_mut()
                .set_active_ops_json(cxx_qt_lib::QString::from(&snapshot));
        });
    }
}

impl FileOpsEvents for FileOpsForwarder {
    fn op_started(&self, started: FileOpStarted) {
        log::info!(
            "fileop {} started ({}): {} item(s)",
            started.id, started.operation, started.items_total
        );
        {
            let mut ops = active_ops_state().ops.lock().unwrap();
            ops.insert(
                started.id.clone(),
                ActiveOp {
                    id: started.id,
                    operation: started.operation,
                    items_total: started.items_total,
                    items_done: 0,
                    total_bytes: 0,
                    copied_bytes: 0,
                    current_file: String::new(),
                    progress: 0.0,
                },
            );
        }
        self.write_snapshot();
    }

    fn op_progress(&self, progress: FileOpProgress) {
        {
            let mut ops = active_ops_state().ops.lock().unwrap();
            if let Some(op) = ops.get_mut(&progress.id) {
                op.items_total = progress.items_total;
                op.items_done = progress.items_done;
                op.total_bytes = progress.total_bytes;
                op.copied_bytes = progress.copied_bytes;
                op.current_file = progress.current_file;
                // For delete (no bytes), progress comes from items.
                // For copy/move, blend bytes-of-current-item with
                // already-finished items so progress isn't bumpy when
                // moving from one big file to many small ones.
                op.progress = if progress.items_total == 0 {
                    0.0
                } else if progress.total_bytes > 0 {
                    let item_share = 1.0 / progress.items_total as f64;
                    let done_share = progress.items_done as f64 * item_share;
                    let cur_share = (progress.copied_bytes as f64
                        / progress.total_bytes as f64)
                        * item_share;
                    (done_share + cur_share).min(1.0)
                } else {
                    progress.items_done as f64 / progress.items_total as f64
                };
            }
        }
        self.write_snapshot();
    }

    fn op_completed(&self, completed: FileOpCompleted) {
        if completed.failed > 0 {
            log::warn!(
                "fileop {} completed: {} ok, {} failed: {}",
                completed.id,
                completed.succeeded,
                completed.failed,
                completed.errors.join("; ")
            );
        } else {
            log::info!(
                "fileop {} completed: {} ok",
                completed.id, completed.succeeded
            );
        }
        {
            let mut ops = active_ops_state().ops.lock().unwrap();
            ops.remove(&completed.id);
        }
        self.write_snapshot();
        // Broadcast the affected directories so any view showing one of
        // them refreshes itself. Queued onto the Qt thread (we're on a
        // tokio worker here).
        if !completed.changed_dirs.is_empty() {
            let json = serde_json::to_string(&completed.changed_dirs)
                .unwrap_or_else(|_| "[]".into());
            let _ = self.qt_handle.queue(move |mut fo| {
                fo.as_mut().dirs_changed(cxx_qt_lib::QString::from(&json));
            });
        }
    }

    fn op_error(&self, error: FileOpError) {
        log::warn!("fileop {} error: {}", error.id, error.error);
        {
            let mut ops = active_ops_state().ops.lock().unwrap();
            ops.remove(&error.id);
        }
        self.write_snapshot();
    }
}

/// Wire the forwarder once; subsequent calls are no-ops.
fn ensure_events_forwarder(fo: Pin<&mut qobject::FileOps>) {
    use cxx_qt::Threading;
    let state = active_ops_state();
    let mut slot = state.forwarder.lock().unwrap();
    if slot.is_some() {
        return;
    }
    let qt_handle: cxx_qt::CxxQtThread<qobject::FileOps> = fo.as_ref().qt_thread();
    *slot = Some(Arc::new(FileOpsForwarder { qt_handle }));
}

/// Emit `dirs_changed` for the deduped parent directories of
/// `file_paths`. For use from synchronous invokables that already run
/// on the Qt thread (create_directory / rename_path); async ops emit
/// via the forwarder's queued closure instead.
fn emit_dirs_changed(fo: Pin<&mut qobject::FileOps>, file_paths: &[&str]) {
    let mut dirs: Vec<String> = Vec::new();
    for p in file_paths {
        if let Some(parent) = Path::new(p).parent() {
            let s = parent.to_string_lossy().to_string();
            if !s.is_empty() && !dirs.contains(&s) {
                dirs.push(s);
            }
        }
    }
    if dirs.is_empty() {
        return;
    }
    let json = serde_json::to_string(&dirs).unwrap_or_else(|_| "[]".into());
    fo.dirs_changed(cxx_qt_lib::QString::from(&json));
}

// ─── Logging events sink ──────────────────────────────────────────────
// Used as a fallback when the forwarder isn't yet wired (events still
// print but don't reach the UI).

#[allow(dead_code)]
struct LoggingEvents;

impl FileOpsEvents for LoggingEvents {
    fn op_started(&self, started: FileOpStarted) {
        log::info!(
            "fileop {} started ({}): {} item(s)",
            started.id,
            started.operation,
            started.items_total
        );
    }
    fn op_progress(&self, _progress: FileOpProgress) {
        // Per-tick progress is too chatty for log::info; surfaces in
        // QML when the proper events trait→Q_SIGNAL bridge lands.
    }
    fn op_completed(&self, completed: FileOpCompleted) {
        if completed.failed > 0 {
            log::warn!(
                "fileop {} completed: {} ok, {} failed: {}",
                completed.id,
                completed.succeeded,
                completed.failed,
                completed.errors.join("; ")
            );
        } else {
            log::info!(
                "fileop {} completed: {} ok",
                completed.id,
                completed.succeeded
            );
        }
    }
    fn op_error(&self, error: FileOpError) {
        log::warn!("fileop {} error: {}", error.id, error.error);
    }
}
