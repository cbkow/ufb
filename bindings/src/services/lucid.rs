//! `Lucid` QObject — the LucidLink rows of the file context menu
//! (core::lucidlink). `selection_state` answers synchronously from the
//! core's caches (one `list` + `status` round on a cold cache, ~100 ms,
//! then nothing for 30 s); `set_pinned` runs the CLI on a worker thread
//! and reports through `pin_done`.

use cxx_qt::Threading;
use std::pin::Pin;

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
        type Lucid = super::LucidRust;

        /// True when a LucidLink command-line client was found.
        #[qinvokable]
        fn is_available(self: &Lucid) -> bool;

        /// JSON {available, onFilespace, filespace, pinnedAll, pinnedAny}
        /// for a JSON array of absolute paths. onFilespace is true only
        /// when every path sits on the same filespace.
        #[qinvokable]
        fn selection_state(self: &Lucid, paths_json: QString) -> QString;

        /// Pin (`on`) or unpin every path on a worker thread; `pin_done`
        /// follows on the Qt thread.
        #[qinvokable]
        fn set_pinned(self: Pin<&mut Lucid>, paths_json: QString, on: bool);

        #[qsignal]
        fn pin_done(self: Pin<&mut Lucid>, ok: bool, message: QString);
    }

    impl cxx_qt::Threading for Lucid {}
}

#[derive(Default)]
pub struct LucidRust;

fn parse_paths(paths_json: &cxx_qt_lib::QString) -> Vec<String> {
    let s = paths_json.to_string();
    serde_json::from_str::<Vec<String>>(&s).unwrap_or_else(|_| {
        if s.trim().is_empty() { Vec::new() } else { vec![s] }
    })
}

impl qobject::Lucid {
    fn is_available(&self) -> bool {
        ufb_core::lucidlink::available()
    }

    fn selection_state(&self, paths_json: cxx_qt_lib::QString) -> cxx_qt_lib::QString {
        let st = ufb_core::lucidlink::selection_state(&parse_paths(&paths_json));
        let json = serde_json::json!({
            "available": st.available,
            "onFilespace": st.on_filespace,
            "filespace": st.filespace,
            "pinnedAll": st.pinned_all,
            "pinnedAny": st.pinned_any,
        });
        cxx_qt_lib::QString::from(&json.to_string())
    }

    fn set_pinned(self: Pin<&mut qobject::Lucid>, paths_json: cxx_qt_lib::QString, on: bool) {
        let paths = parse_paths(&paths_json);
        if paths.is_empty() {
            return;
        }
        let qt = self.qt_thread();
        std::thread::spawn(move || {
            let (ok, msg) = match ufb_core::lucidlink::set_pinned(&paths, on) {
                Ok(m) => {
                    log::info!("lucidlink: {}", m);
                    (true, m)
                }
                Err(e) => {
                    log::warn!("lucidlink: pin failed: {}", e);
                    (false, e)
                }
            };
            let _ = qt.queue(move |mut o| {
                o.as_mut().pin_done(ok, cxx_qt_lib::QString::from(&msg));
            });
        });
    }
}
