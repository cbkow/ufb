//! `Paths` QObject — utility singleton exposing platform info, special
//! paths, drive enumeration, and URI builders/resolvers to QML.
//!
//! Phase 2 stub: just `platform()` to prove the cxx-qt pipeline. Real
//! methods (`specialPaths`, `drives`, `pickFolder`, `buildUfbUri`,
//! `resolveUfbUri`, etc.) land alongside the rest of the QObjects.

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
        type Paths = super::PathsRust;

        /// Returns "windows" or "macos" — used by QML to gate
        /// platform-specific UI affordances.
        #[qinvokable]
        fn platform(self: &Paths) -> QString;

        /// `ufb-core` library version string.
        #[qinvokable]
        fn core_version(self: &Paths) -> QString;

        /// User's home directory, or "" if unavailable.
        #[qinvokable]
        fn home_path(self: &Paths) -> QString;

        /// Last path component (filename or final folder name).
        /// Returns "" for empty input. Strips trailing separators
        /// before computing the basename.
        #[qinvokable]
        fn basename_of(self: &Paths, path: QString) -> QString;

        /// True iff `path` exists on the local filesystem (file or
        /// directory). Used to silently drop saved tabs whose source
        /// folder has been removed since last launch.
        #[qinvokable]
        fn path_exists(self: &Paths, path: QString) -> bool;

        /// True iff `path` exists AND is a directory. Used by the
        /// URI handler to detect file-target ufb:// links so it can
        /// navigate to the parent folder instead of trying to read
        /// a regular file as a directory.
        #[qinvokable]
        fn is_directory(self: &Paths, path: QString) -> bool;

        /// Parent path of `path` (whatever the platform-native parent
        /// computation yields, no trailing-separator trim). Empty
        /// string when there is no parent (root or empty input).
        #[qinvokable]
        fn parent_of(self: &Paths, path: QString) -> QString;

        /// Resolved default sync cache folder — same logic the agent
        /// uses when `MountsConfig.syncCacheRoot` is unset. Windows:
        /// `%LOCALAPPDATA%\ufb\sync`. macOS/Linux: `~/.local/share/ufb/sync`.
        /// Already env-expanded so the QML side can pass it straight to
        /// reveal_in_file_manager / create_directory.
        #[qinvokable]
        fn default_sync_cache_root(self: &Paths) -> QString;

        /// Open the system QuickLook preview for the given paths.
        /// macOS only — shells out to `/usr/bin/qlmanage -p` against
        /// the path list. No-op on other platforms (Win has its own
        /// in-Explorer preview that doesn't have an equivalent
        /// arbitrary-process trigger).
        ///
        /// `paths_json` is a JSON array of absolute file paths.
        /// QML's Space-bar shortcut wires here from FileBrowser.
        ///
        /// Fire-and-forget: returns immediately. The user closes the
        /// preview window themselves (Esc). Per macOSplans/07.
        #[qinvokable]
        fn quicklook_preview(self: &Paths, paths_json: QString);
    }
}

/// Backing Rust struct. cxx-qt wires this up as the data of the
/// `Paths` QObject via the `type Paths = super::PathsRust` line in
/// the bridge.
#[derive(Default)]
pub struct PathsRust;

impl qobject::Paths {
    fn platform(self: &qobject::Paths) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(if cfg!(target_os = "windows") {
            "windows"
        } else if cfg!(target_os = "macos") {
            "macos"
        } else {
            "unknown"
        })
    }

    fn core_version(self: &qobject::Paths) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(ufb_core::version())
    }

    fn home_path(self: &qobject::Paths) -> cxx_qt_lib::QString {
        let home = dirs::home_dir()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_default();
        cxx_qt_lib::QString::from(&home)
    }

    fn basename_of(
        self: &qobject::Paths,
        path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let s = path.to_string();
        let trimmed = s.trim_end_matches(|c| c == '/' || c == '\\');
        let basename = std::path::Path::new(trimmed)
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_else(|| trimmed.to_string());
        cxx_qt_lib::QString::from(&basename)
    }

    fn path_exists(
        self: &qobject::Paths,
        path: cxx_qt_lib::QString,
    ) -> bool {
        let s = path.to_string();
        if s.is_empty() {
            return false;
        }
        std::path::Path::new(&s).exists()
    }

    fn is_directory(
        self: &qobject::Paths,
        path: cxx_qt_lib::QString,
    ) -> bool {
        let s = path.to_string();
        if s.is_empty() {
            return false;
        }
        std::path::Path::new(&s).is_dir()
    }

    fn parent_of(
        self: &qobject::Paths,
        path: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        let s = path.to_string();
        let parent = std::path::Path::new(&s)
            .parent()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_default();
        cxx_qt_lib::QString::from(&parent)
    }

    fn quicklook_preview(
        self: &qobject::Paths,
        paths_json: cxx_qt_lib::QString,
    ) {
        let paths: Vec<String> = match serde_json::from_str(&paths_json.to_string()) {
            Ok(v) => v,
            Err(e) => {
                log::warn!("Paths.quicklook_preview: bad JSON: {}", e);
                return;
            }
        };
        if paths.is_empty() {
            return;
        }

        #[cfg(target_os = "macos")]
        {
            // qlmanage -p <path1> <path2> ... opens the system
            // QuickLook preview window with multi-item navigation
            // arrows. Fire-and-forget; the user closes the window.
            // qlmanage logs verbose noise to stderr — silence it.
            let mut cmd = std::process::Command::new("/usr/bin/qlmanage");
            cmd.arg("-p");
            for p in &paths {
                cmd.arg(p);
            }
            cmd.stdout(std::process::Stdio::null());
            cmd.stderr(std::process::Stdio::null());
            if let Err(e) = cmd.spawn() {
                log::warn!(
                    "Paths.quicklook_preview: failed to spawn qlmanage: {}",
                    e
                );
            }
        }
        #[cfg(not(target_os = "macos"))]
        {
            // No equivalent on Windows from outside Explorer. Win
            // PowerToys QuickLook fires from Explorer's Space key
            // and isn't directly invocable from here.
            let _ = paths;
        }
    }

    fn default_sync_cache_root(
        self: &qobject::Paths,
    ) -> cxx_qt_lib::QString {
        // Mirrors agent::config::MountConfig::default_cache_root().
        // Kept as a separate copy here because the agent crate isn't
        // a build-time dependency of bindings — the agent ships as a
        // separate binary the main app drives over IPC.
        let path = {
            #[cfg(windows)]
            {
                if let Ok(local) = std::env::var("LOCALAPPDATA") {
                    std::path::PathBuf::from(local)
                        .join("ufb")
                        .join("sync")
                } else {
                    std::path::PathBuf::from(r"C:\ufb\sync")
                }
            }
            #[cfg(not(windows))]
            {
                if let Some(home) = dirs::home_dir() {
                    home.join(".local").join("share").join("ufb").join("sync")
                } else {
                    std::path::PathBuf::from("/tmp/ufb-sync")
                }
            }
        };
        cxx_qt_lib::QString::from(&path.to_string_lossy().to_string())
    }
}
