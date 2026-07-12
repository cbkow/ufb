//! `Search` QObject — recursive deep search behind the browser search bar.
//!
//! Two backends, one at a time, chosen by the `indexed` toggle:
//!   - **OS (default):** `core::search::search_files` (es.exe on Windows,
//!     mdfind on macOS, walkdir fallback). No index to maintain.
//!   - **Indexed (alt):** a Tantivy index (`core::search_index`) crawled
//!     from the scope root, giving Everything-style substring matching
//!     that fills in live as the crawl runs.
//!
//! Per-instance (like `Directory`): each pane's search bar owns one, so
//! its root / mode / results are independent. Async via the shared tokio
//! runtime; an `epoch` counter discards results from a superseded session
//! or query so a stale crawl can't overwrite fresh output. `stop()` is the
//! clean teardown the toggle relies on — it cancels the crawl, drops the
//! index handle, and invalidates in-flight work.

use crate::runtime::shared_runtime;
use std::path::Path;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use ufb_core::file_ops::FileEntry;
use ufb_core::search_index::SearchIndex;

const RESULT_LIMIT: usize = 500;

#[cxx_qt::bridge]
pub mod qobject {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qproperty(bool, indexed)] // false = OS backend, true = Tantivy
        #[qproperty(QString, root)] // scope: current pane root
        #[qproperty(QString, query)]
        #[qproperty(QString, results_json)]
        #[qproperty(bool, searching)]
        #[qproperty(bool, indexing)] // a crawl is in progress (indexed mode)
        #[qproperty(i32, index_progress)] // docs indexed so far
        #[qproperty(QString, error)]
        type Search = super::SearchRust;

        /// Begin a search session over `root` with the chosen backend.
        /// Tears down any prior session first. In indexed mode this opens
        /// (or creates) the per-root index and kicks off a background
        /// crawl; results fill in live. Runs an initial query if `query`
        /// is already set.
        #[qinvokable]
        fn start(self: Pin<&mut Search>, root: QString, indexed: bool);

        /// Set the query text and run a search (debounce on the QML side).
        #[qinvokable]
        fn set_query_text(self: Pin<&mut Search>, query: QString);

        /// Clean teardown: cancel the crawl, drop the index, invalidate
        /// in-flight async, clear results. Called on toggle-off / nav-away.
        #[qinvokable]
        fn stop(self: Pin<&mut Search>);
    }

    impl cxx_qt::Threading for Search {}
}

pub struct SearchRust {
    pub indexed: bool,
    pub root: cxx_qt_lib::QString,
    pub query: cxx_qt_lib::QString,
    pub results_json: cxx_qt_lib::QString,
    pub searching: bool,
    pub indexing: bool,
    pub index_progress: i32,
    pub error: cxx_qt_lib::QString,

    // Internal (non-property) state.
    epoch: Arc<AtomicU64>,
    /// Bumped by every `run_search` call. The macOS walkdir fallback can
    /// run for a while on big network trees; this lets a newer keystroke
    /// cancel the older walk (the session-level `epoch` only changes on
    /// start/stop, not per query).
    query_gen: Arc<AtomicU64>,
    cancel: Arc<AtomicBool>,
    index: Option<Arc<SearchIndex>>,
}

impl Default for SearchRust {
    fn default() -> Self {
        Self {
            indexed: false,
            root: cxx_qt_lib::QString::from(""),
            query: cxx_qt_lib::QString::from(""),
            results_json: cxx_qt_lib::QString::from("[]"),
            searching: false,
            indexing: false,
            index_progress: 0,
            error: cxx_qt_lib::QString::from(""),
            epoch: Arc::new(AtomicU64::new(0)),
            query_gen: Arc::new(AtomicU64::new(0)),
            cancel: Arc::new(AtomicBool::new(false)),
            index: None,
        }
    }
}

impl qobject::Search {
    fn start(mut self: Pin<&mut qobject::Search>, root: cxx_qt_lib::QString, indexed: bool) {
        use cxx_qt::{CxxQtType, Threading};

        // Tear down any prior session (invalidate + cancel + drop index).
        teardown(self.as_mut());

        self.as_mut().set_root(root.clone());
        self.as_mut().set_indexed(indexed);
        self.as_mut().set_error(cxx_qt_lib::QString::from(""));

        // New epoch for this session; fresh cancel flag for its crawl.
        let issued = self.as_ref().rust().epoch.fetch_add(1, Ordering::SeqCst) + 1;
        let cancel = Arc::new(AtomicBool::new(false));
        self.as_mut().rust_mut().cancel = cancel.clone();

        let root_str = root.to_string();
        if indexed && !root_str.is_empty() {
            match SearchIndex::open(Path::new(&root_str)) {
                Ok(idx) => {
                    let idx = Arc::new(idx);
                    self.as_mut().rust_mut().index = Some(idx.clone());
                    self.as_mut().set_indexing(true);
                    self.as_mut().set_index_progress(0);

                    let qt = self.as_ref().qt_thread();
                    let epoch = self.as_ref().rust().epoch.clone();
                    let _g = shared_runtime().enter();
                    tokio::task::spawn_blocking(move || {
                        let qt_p = qt.clone();
                        let epoch_p = epoch.clone();
                        let res = idx.rebuild(&cancel, |count| {
                            let ep = epoch_p.clone();
                            let _ = qt_p.queue(move |mut s| {
                                if ep.load(Ordering::SeqCst) != issued {
                                    return;
                                }
                                s.as_mut().set_index_progress(count as i32);
                                run_search(s); // fill results live as we crawl
                            });
                        });
                        let _ = qt.queue(move |mut s| {
                            if epoch.load(Ordering::SeqCst) != issued {
                                return;
                            }
                            s.as_mut().set_indexing(false);
                            if let Err(e) = res {
                                s.as_mut().set_error(cxx_qt_lib::QString::from(&e));
                            }
                            run_search(s);
                        });
                    });
                }
                Err(e) => {
                    self.as_mut().set_error(cxx_qt_lib::QString::from(&e));
                }
            }
        }

        // Initial query (OS mode runs immediately; indexed runs against
        // whatever the crawl has committed so far).
        run_search(self);
    }

    fn set_query_text(mut self: Pin<&mut qobject::Search>, query: cxx_qt_lib::QString) {
        self.as_mut().set_query(query);
        run_search(self);
    }

    fn stop(mut self: Pin<&mut qobject::Search>) {
        teardown(self.as_mut());
        self.as_mut().set_results_json(cxx_qt_lib::QString::from("[]"));
        self.as_mut().set_searching(false);
        self.as_mut().set_indexing(false);
        self.as_mut().set_index_progress(0);
    }
}

/// Invalidate in-flight async (bump epoch), cancel the running crawl, and
/// drop the index handle.
fn teardown(mut s: Pin<&mut qobject::Search>) {
    use cxx_qt::CxxQtType;
    s.as_ref().rust().epoch.fetch_add(1, Ordering::SeqCst);
    s.as_ref().rust().cancel.store(true, Ordering::SeqCst);
    s.as_mut().rust_mut().index = None;
}

/// Run the current query on the current backend; queue results back,
/// dropping them if the session/query epoch has advanced meanwhile.
fn run_search(mut s: Pin<&mut qobject::Search>) {
    use cxx_qt::{CxxQtType, Threading};

    let (query, root, indexed, index, epoch, query_gen) = {
        let this = s.as_ref();
        let r = this.rust();
        (
            r.query.to_string(),
            r.root.to_string(),
            r.indexed,
            r.index.clone(),
            r.epoch.clone(),
            r.query_gen.clone(),
        )
    };

    if query.trim().is_empty() {
        query_gen.fetch_add(1, Ordering::SeqCst); // invalidate in-flight walks
        s.as_mut().set_results_json(cxx_qt_lib::QString::from("[]"));
        s.as_mut().set_searching(false);
        return;
    }

    let issued = epoch.load(Ordering::SeqCst);
    let issued_gen = query_gen.fetch_add(1, Ordering::SeqCst) + 1;
    s.as_mut().set_searching(true);
    let qt = s.as_ref().qt_thread();

    let _g = shared_runtime().enter();
    tokio::task::spawn_blocking(move || {
        let stale = {
            let epoch = epoch.clone();
            let query_gen = query_gen.clone();
            move || {
                epoch.load(Ordering::SeqCst) != issued
                    || query_gen.load(Ordering::SeqCst) != issued_gen
            }
        };
        let result: Result<Vec<FileEntry>, String> = if indexed {
            match index {
                Some(idx) => idx.query(&query, RESULT_LIMIT),
                None => Ok(Vec::new()),
            }
        } else {
            let scope = if root.is_empty() { None } else { Some(root.as_str()) };
            // Stream walkdir batches into the UI as they arrive — a deep
            // network walk shouldn't sit on a spinner until it finishes.
            let mut acc: Vec<FileEntry> = Vec::new();
            let qt_stream = qt.clone();
            let stream_epoch = epoch.clone();
            let stream_gen = query_gen.clone();
            let mut on_batch = |new_items: Vec<FileEntry>| {
                acc.extend(new_items);
                if stale() {
                    return;
                }
                let json = serde_json::to_string(&acc).unwrap_or_else(|_| "[]".into());
                let ep = stream_epoch.clone();
                let qg = stream_gen.clone();
                let _ = qt_stream.queue(move |mut s| {
                    if ep.load(Ordering::SeqCst) != issued
                        || qg.load(Ordering::SeqCst) != issued_gen
                    {
                        return;
                    }
                    s.as_mut().set_results_json(cxx_qt_lib::QString::from(&json));
                });
            };
            ufb_core::search::search_files_cancellable(&query, scope, &stale, &mut on_batch)
        };

        let _ = qt.queue(move |mut s| {
            if epoch.load(Ordering::SeqCst) != issued
                || query_gen.load(Ordering::SeqCst) != issued_gen
            {
                return; // superseded by a newer session/query
            }
            match result {
                Ok(entries) => {
                    let json = serde_json::to_string(&entries).unwrap_or_else(|_| "[]".into());
                    s.as_mut().set_results_json(cxx_qt_lib::QString::from(&json));
                    s.as_mut().set_error(cxx_qt_lib::QString::from(""));
                }
                Err(e) => {
                    s.as_mut().set_results_json(cxx_qt_lib::QString::from("[]"));
                    s.as_mut().set_error(cxx_qt_lib::QString::from(&e));
                }
            }
            s.as_mut().set_searching(false);
        });
    });
}
