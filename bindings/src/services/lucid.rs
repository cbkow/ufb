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

        /// Share links for a JSON array of paths (the extension's "Copy
        /// link"): JSON {links: [...], error: ""}. Stops at the first
        /// failure with its message in `error`. A local round trip.
        #[qinvokable]
        fn direct_links(self: &Lucid, paths_json: QString) -> QString;

        /// Open the desktop app's guest-link sheet for one path (the
        /// extension's "Create guest link"). Returns "" or the error.
        #[qinvokable]
        fn create_guest_link(self: &Lucid, path: QString) -> QString;
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

    fn direct_links(&self, paths_json: cxx_qt_lib::QString) -> cxx_qt_lib::QString {
        let mut links = Vec::new();
        let mut error = String::new();
        for p in parse_paths(&paths_json) {
            match ufb_core::lucidlink::direct_link(&p) {
                Ok(l) => links.push(l),
                Err(e) => {
                    log::warn!("lucidlink: direct link for {}: {}", p, e);
                    error = e;
                    break;
                }
            }
        }
        let json = serde_json::json!({ "links": links, "error": error });
        cxx_qt_lib::QString::from(&json.to_string())
    }

    fn create_guest_link(&self, path: cxx_qt_lib::QString) -> cxx_qt_lib::QString {
        match ufb_core::lucidlink::create_guest_link(&path.to_string()) {
            Ok(()) => cxx_qt_lib::QString::from(""),
            Err(e) => {
                log::warn!("lucidlink: guest link: {}", e);
                cxx_qt_lib::QString::from(&e)
            }
        }
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
