//! cxx-qt build orchestration.
//!
//! Walks every #[cxx_qt::bridge] in `src/` and emits the matching
//! C++ header + source pair into the cargo OUT_DIR. With the
//! `link_qt_object_files` feature on `cxx-qt-build`, the resulting
//! Rust static library carries Qt's own object files internally, so
//! corrosion + CMake just link the static library and get everything.

use cxx_qt_build::{CxxQtBuilder, QmlModule};

fn main() {
    CxxQtBuilder::new()
        // The Qt modules we use across all QObjects. Adding more here
        // (e.g. "Sql", "Network") is only needed if a binding actually
        // imports types from that module.
        .qt_module("Quick")
        .qt_module("QuickControls2")
        // Backend QML module — every cxx-qt QObject we register goes
        // here. QML imports it via `import Ufb.Backend 1.0`.
        .qml_module::<&str, &str>(QmlModule {
            uri: "Ufb.Backend",
            rust_files: &[
                "src/services/backup.rs",
                "src/services/bookmarks.rs",
                "src/services/columns.rs",
                "src/services/directory.rs",
                "src/services/directory_tree.rs",
                "src/services/file_ops.rs",
                "src/services/mesh.rs",
                "src/services/metadata.rs",
                "src/services/mount.rs",
                "src/services/paths.rs",
                "src/services/search.rs",
                "src/services/settings.rs",
                "src/services/subscription.rs",
                "src/services/transcode.rs",
            ],
            qml_files: &[],
            ..Default::default()
        })
        .build();
}
