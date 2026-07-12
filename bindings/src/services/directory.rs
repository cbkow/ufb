//! `Directory` QObject — minimal file-system browsing.
//!
//! Per-instance as of Phase 5: each pane / tab owns one. QML
//! instantiates with `Directory { id: leftDir }`. Use
//! `Component.onCompleted: leftDir.navigate_to(...)` for an
//! initial path.
//!
//! A real `QAbstractListModel` is deferred — cxx-qt 0.7 doesn't ship
//! one and writing the cxx-bridge for the virtual functions is a
//! focused research task. The JSON shim works fine for "Explorer of
//! 1994" minimum and the QML delegates absorb the swap cleanly when
//! it lands.

use crate::runtime::shared_runtime;
use std::path::Path;
use std::pin::Pin;
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
        #[qproperty(QString, current_path)]
        #[qproperty(QString, entries_json)]
        #[qproperty(bool, loading)]
        #[qproperty(QString, error)]
        type Directory = super::DirectoryRust;

        /// Navigate to an absolute path. Async — `loading` flips true,
        /// then `entries_json` updates and `loading` flips false.
        #[qinvokable]
        fn navigate_to(self: Pin<&mut Directory>, path: QString);

        /// Navigate to the parent of `current_path`. No-op at the
        /// filesystem root.
        #[qinvokable]
        fn navigate_up(self: Pin<&mut Directory>);

        /// Refresh the current path without changing it.
        #[qinvokable]
        fn refresh(self: Pin<&mut Directory>);

        /// Reset state without triggering a listing — current_path,
        /// entries_json, and error all clear. Used when a host wants
        /// to "blank" a Directory without producing the "Not a
        /// directory" error that `navigate_to("")` would.
        #[qinvokable]
        fn clear(self: Pin<&mut Directory>);
    }

    impl cxx_qt::Threading for Directory {}
}

pub struct DirectoryRust {
    pub current_path: cxx_qt_lib::QString,
    pub entries_json: cxx_qt_lib::QString,
    pub loading: bool,
    pub error: cxx_qt_lib::QString,
}

impl Default for DirectoryRust {
    fn default() -> Self {
        // Per-instance default = empty. QML calls navigate_to(...) once
        // it knows which path this instance should show.
        Self {
            current_path: cxx_qt_lib::QString::from(""),
            entries_json: cxx_qt_lib::QString::from("[]"),
            loading: false,
            error: cxx_qt_lib::QString::from(""),
        }
    }
}

impl qobject::Directory {
    fn navigate_to(mut self: Pin<&mut qobject::Directory>, path: cxx_qt_lib::QString) {
        let path_str = path.to_string();
        self.as_mut().set_current_path(path);
        self.as_mut().set_error(cxx_qt_lib::QString::from(""));
        spawn_list(self.as_mut(), path_str);
    }

    fn navigate_up(mut self: Pin<&mut qobject::Directory>) {
        use cxx_qt::CxxQtType;
        let current = self.as_ref().rust().current_path.to_string();
        let parent = Path::new(&current)
            .parent()
            .map(|p| p.to_string_lossy().to_string());
        if let Some(p) = parent {
            self.as_mut().navigate_to(cxx_qt_lib::QString::from(&p));
        }
    }

    fn refresh(mut self: Pin<&mut qobject::Directory>) {
        use cxx_qt::CxxQtType;
        let current = self.as_ref().rust().current_path.to_string();
        spawn_list(self.as_mut(), current);
    }

    fn clear(mut self: Pin<&mut qobject::Directory>) {
        self.as_mut()
            .set_current_path(cxx_qt_lib::QString::from(""));
        self.as_mut()
            .set_entries_json(cxx_qt_lib::QString::from("[]"));
        self.as_mut().set_error(cxx_qt_lib::QString::from(""));
        self.as_mut().set_loading(false);
    }
}

/// Spawn a blocking list_directory on the tokio runtime; result
/// queues back onto the Qt thread to update the QObject.
fn spawn_list(mut dir: Pin<&mut qobject::Directory>, path: String) {
    use cxx_qt::Threading;

    dir.as_mut().set_loading(true);
    let qt_handle = dir.as_ref().qt_thread();

    let _guard = shared_runtime().enter();
    tokio::task::spawn_blocking(move || {
        let result = file_ops::list_directory(&path);
        let _ = qt_handle.queue(move |mut dir| {
            match result {
                Ok(entries) => {
                    let json = serde_json::to_string(&entries).unwrap_or_else(|_| "[]".into());
                    dir.as_mut().set_entries_json(cxx_qt_lib::QString::from(&json));
                    dir.as_mut().set_error(cxx_qt_lib::QString::from(""));
                }
                Err(e) => {
                    dir.as_mut().set_entries_json(cxx_qt_lib::QString::from("[]"));
                    dir.as_mut().set_error(cxx_qt_lib::QString::from(&e));
                }
            }
            dir.as_mut().set_loading(false);
        });
    });
}
