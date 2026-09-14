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
    pub mod archive;
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

/// The GUI's `QGuiApplication::aboutToQuit` hook (app/main.cpp).
/// audit 2026-09-11 M-3: cancels in-flight NetFS mount requests (a
/// process dying with one pending wedges NetAuthSysAgent machine-wide)
/// and stops the local mount tasks WITHOUT unmounting — plain mounts
/// are ordinary OS mounts the user expects to keep. Does NOT stop the
/// agent: it is the sync host and outlives GUI quits by design.
/// Blocking, bounded by the NetFS cancel grace (10s) only when a
/// request refuses to acknowledge its cancel.
#[unsafe(no_mangle)]
pub extern "C" fn ufb_gui_about_to_quit() {
    services::mount::about_to_quit();
}

/// OPT-IN, not wired: tell the ufb-agent to shut down. Nothing calls
/// this today — closing UFB leaves the agent running as the sync host
/// (see `ufb_gui_about_to_quit`). Kept for a future explicit
/// "Quit and stop syncing" action.
#[unsafe(no_mangle)]
pub extern "C" fn ufb_shutdown_agent_blocking() {
    services::mount::shutdown_agent_for_quit();
}
