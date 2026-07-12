//! `Bookmarks` QObject — wraps `core::bookmarks::BookmarkManager`.
//!
//! Phase 5: real implementation backed by the same SQLite database
//! the legacy Tauri app used (so any pre-existing bookmarks the user
//! had appear immediately on first launch). Uses the shared
//! `bindings::db::shared_db()` so multiple services share one
//! connection pool.

use crate::db::shared_db;
use std::pin::Pin;
use std::sync::{Arc, OnceLock};
use ufb_core::bookmarks::{Bookmark, BookmarkManager};
use ufb_core::utils::from_canonical_path;

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
        #[qproperty(QString, bookmarks_json)]
        type Bookmarks = super::BookmarksRust;

        /// Re-read the bookmark table. Synchronous — bookmark counts
        /// are tiny.
        #[qinvokable]
        fn refresh(self: Pin<&mut Bookmarks>);

        /// Add a bookmark. Triggers a refresh on success.
        /// `is_project_folder` flags the bookmark as a "jobs folder"
        /// (a parent directory that contains many job subfolders);
        /// the sidebar uses a different icon for these.
        #[qinvokable]
        fn add(self: Pin<&mut Bookmarks>,
               path: QString,
               display_name: QString,
               is_project_folder: bool);

        /// Remove the bookmark with this path. Triggers a refresh.
        #[qinvokable]
        fn remove(self: Pin<&mut Bookmarks>, path: QString);
    }
}

pub struct BookmarksRust {
    pub bookmarks_json: cxx_qt_lib::QString,
}

impl Default for BookmarksRust {
    fn default() -> Self {
        Self {
            bookmarks_json: cxx_qt_lib::QString::from("[]"),
        }
    }
}

/// Lazy single BookmarkManager. Wraps the shared DB so other services
/// (Thumbnails, Metadata, …) share the same connection.
fn shared_manager() -> Option<Arc<BookmarkManager>> {
    static MANAGER: OnceLock<Option<Arc<BookmarkManager>>> = OnceLock::new();
    MANAGER
        .get_or_init(|| shared_db().map(|db| Arc::new(BookmarkManager::new(db))))
        .clone()
}

fn serialize(bookmarks: &[Bookmark]) -> String {
    serde_json::to_string(bookmarks).unwrap_or_else(|_| "[]".into())
}

/// Translate every bookmark's `path` from canonical (Windows) DB
/// form to the local OS's native form, applying the user's path
/// mappings. Mirrors the convention from `core/src/utils.rs`: DB
/// stores Windows-canonical paths so they round-trip across mac+win
/// installs of the same DB; the QObject layer translates at the
/// boundary so QML sees native paths.
///
/// Without this, bookmarks created on Windows (e.g. `R:\Projects`)
/// arrive in the macOS QML side verbatim and clicking them tries to
/// navigate to a non-existent path.
fn translate_bookmarks_for_native(bookmarks: &mut Vec<Bookmark>) {
    let settings_arc = crate::services::settings::shared_settings();
    let settings = settings_arc.read().unwrap();
    for bm in bookmarks.iter_mut() {
        bm.path = from_canonical_path(&bm.path, &settings.path_mappings);
    }
}

impl qobject::Bookmarks {
    fn refresh(mut self: Pin<&mut qobject::Bookmarks>) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        match mgr.get_all_bookmarks() {
            Ok(mut list) => {
                translate_bookmarks_for_native(&mut list);
                let json = serialize(&list);
                self.as_mut()
                    .set_bookmarks_json(cxx_qt_lib::QString::from(&json));
            }
            Err(e) => log::warn!("bookmarks: refresh failed: {}", e),
        }
    }

    fn add(
        self: Pin<&mut qobject::Bookmarks>,
        path: cxx_qt_lib::QString,
        display_name: cxx_qt_lib::QString,
        is_project_folder: bool,
    ) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        let native_path = path.to_string();
        let name_s = display_name.to_string();
        let name = if name_s.is_empty() {
            // Fall back to the trailing path segment of the NATIVE
            // path so the auto-name is what the user sees in their
            // own filesystem, not the canonical Win form.
            std::path::Path::new(&native_path)
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_else(|| native_path.clone())
        } else {
            name_s
        };
        if let Err(e) = mgr.add_bookmark(&native_path, &name, is_project_folder) {
            log::warn!("bookmarks: add failed: {}", e);
            return;
        }
        self.refresh();
    }

    fn remove(self: Pin<&mut qobject::Bookmarks>, path: cxx_qt_lib::QString) {
        let Some(mgr) = shared_manager() else {
            return;
        };
        let path_s = path.to_string();
        if let Err(e) = mgr.remove_bookmark(&path_s) {
            log::warn!("bookmarks: remove failed: {}", e);
            return;
        }
        self.refresh();
    }
}
