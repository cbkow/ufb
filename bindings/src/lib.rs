//! UFB bindings — cxx-qt QObjects wrapping `ufb-core` for QML.
//!
//! Phase 2: 12 service QObjects declared as singletons. Each is a
//! minimal stub exposing a single `placeholder()` Q_INVOKABLE so the
//! QML side can verify imports + the cxx-qt build pipeline. Real
//! property/signal/invokable surfaces land in Phase 3 along with
//! AppCore wiring.
//!
//! Models (DirectoryModel, MountStatesModel, MeshPeersModel, etc.)
//! are deferred to a separate pass — they require cxx-qt's
//! QAbstractListModel/QAbstractItemModel integration which is more
//! involved than a plain QObject singleton.

use std::sync::Once;

static INIT: Once = Once::new();

pub mod db;
#[cfg(any(target_os = "macos", windows))]
pub mod local_mounts;
pub mod runtime;

pub mod services {
    pub mod backup;
    pub mod bookmarks;
    pub mod columns;
    pub mod directory;
    pub mod directory_tree;
    pub mod file_ops;
    pub mod mesh;
    pub mod metadata;
    pub mod mount;
    pub mod paths;
    pub mod search;
    pub mod settings;
    pub mod subscription;
    pub mod transcode;
}

/// Phase 0 carry-over: a no-op smoke test exposed via `extern "C"` so
/// the C++ side can verify Rust is linked.
#[unsafe(no_mangle)]
pub extern "C" fn ufb_bindings_phase0_smoke_test() {
    INIT.call_once(|| {
        let _ = env_logger::Builder::from_default_env()
            .filter_level(log::LevelFilter::Info)
            .try_init();
    });

    log::info!(
        "ufb-bindings phase 2 smoke test — core v{}",
        ufb_core::version()
    );
}

/// Tell the ufb-agent to shut down. Called from C++ on
/// `QGuiApplication::aboutToQuit` so closing UFB also stops the agent
/// — by default the agent stays running headless (mounts survive UI
/// restarts), but the user expects close-window to be a full quit.
#[unsafe(no_mangle)]
pub extern "C" fn ufb_shutdown_agent_blocking() {
    services::mount::shutdown_agent_for_quit();
}
