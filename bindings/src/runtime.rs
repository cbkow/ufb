//! Shared tokio runtime for the bindings layer.
//!
//! Owned for the app's lifetime. The connection loop, agent IPC
//! handlers, async file ops, and any future background work all run on
//! this runtime's worker threads. The Qt main thread is the
//! `QGuiApplication` event loop and never touches tokio directly —
//! tokio tasks marshal updates back via `cxx_qt::CxxQtThread::queue()`.

use std::sync::OnceLock;

pub fn shared_runtime() -> &'static tokio::runtime::Runtime {
    static RUNTIME: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RUNTIME.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .thread_name("ufb-tokio")
            .build()
            .expect("failed to start tokio runtime")
    })
}
