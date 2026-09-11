//! `Archive` QObject — wraps `core::archive::ArchiveManager` (extract /
//! compress-to-zip via a bundled 7-Zip). Same shape as the Transcode
//! binding: `queue_json` mirrors the whole queue, invokables block_on
//! the async core, and an events forwarder queues refreshes back onto
//! the Qt thread. One extra: when a job completes it emits
//! `dirs_changed` with the folders whose listings changed; Main.qml
//! re-broadcasts that through `FileOps.notify_dirs_changed` so every
//! open view refreshes the way it does after a copy / move.
//!
//! Binary resolution: `7zz` (official 7-Zip for macOS, universal,
//! built from source by scripts/build-external-7zip-mac.sh) or
//! `7z.exe` + `7z.dll` (Windows, next to ufb.exe). Falls back to PATH
//! so a dev checkout without external/ synced still works if the user
//! has 7-Zip installed.

use crate::runtime::shared_runtime;
use crate::services::transcode::bundled_tool;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::{Arc, Mutex, OnceLock};
use ufb_core::archive::{ArchiveJob, ArchiveManager, JobStatus};
use ufb_core::events::ArchiveEvents;

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
        #[qproperty(QString, queue_json)]
        type Archive = super::ArchiveRust;

        /// Queue one extract job per archive in `paths_json` (JSON
        /// array). Non-archives are ignored. Returns the JSON array of
        /// created jobs.
        #[qinvokable]
        fn add_extract_jobs(self: Pin<&mut Archive>, paths_json: QString) -> QString;

        /// Queue a single job zipping every path in `paths_json` into
        /// one archive beside them. Returns the job as JSON, or "null"
        /// for an empty selection.
        #[qinvokable]
        fn add_compress_job(self: Pin<&mut Archive>, paths_json: QString) -> QString;

        #[qinvokable]
        fn cancel_job(self: Pin<&mut Archive>, id: QString);

        #[qinvokable]
        fn remove_job(self: Pin<&mut Archive>, id: QString);

        #[qinvokable]
        fn clear_completed(self: Pin<&mut Archive>);

        #[qinvokable]
        fn refresh_queue(self: Pin<&mut Archive>);

        /// Comma-joined lowercase extension list for the "Extract
        /// Here" gate (core::archive::ARCHIVE_EXTENSIONS). Read once
        /// by FileBrowser so per-selection checks stay in JS.
        #[qinvokable]
        fn archive_extensions(self: &Archive) -> QString;

        /// Emitted once per successfully completed job with the path
        /// it produced (extracted folder / entry, or the new zip).
        /// Always precedes the matching `dirs_changed`, so a view can
        /// arm a select-after-refresh hint before its listing reloads.
        #[qsignal]
        fn job_completed(self: Pin<&mut Archive>, output_path: QString);

        /// Emitted once per finished job with a JSON array of the
        /// directories whose listings changed.
        #[qsignal]
        fn dirs_changed(self: Pin<&mut Archive>, dirs_json: QString);
    }

    impl cxx_qt::Threading for Archive {}
}

pub struct ArchiveRust {
    pub queue_json: cxx_qt_lib::QString,
}

impl Default for ArchiveRust {
    fn default() -> Self {
        Self {
            queue_json: cxx_qt_lib::QString::from("[]"),
        }
    }
}

struct ArchiveShared {
    mgr: Arc<ArchiveManager>,
    forwarder: Arc<Mutex<Option<Arc<ArchiveEventsForwarder>>>>,
}

/// Does `name` resolve through PATH? (Cheap directory probe, no spawn.)
fn on_path(name: &str) -> bool {
    let Some(path) = std::env::var_os("PATH") else { return false };
    let exe = if cfg!(windows) { format!("{}.exe", name) } else { name.to_string() };
    std::env::split_paths(&path).any(|d| d.join(&exe).is_file())
}

/// Bundled binary first, then PATH, then the bare default name so the
/// spawn error names the missing tool.
fn resolve_sevenzip() -> PathBuf {
    let candidates: &[&str] = if cfg!(windows) { &["7z", "7za"] } else { &["7zz", "7z", "7za"] };
    for c in candidates {
        let p = bundled_tool(c);
        if p.is_absolute() && p.is_file() {
            return p;
        }
    }
    for c in candidates {
        if on_path(c) {
            return PathBuf::from(c);
        }
    }
    PathBuf::from(candidates[0])
}

fn shared() -> &'static ArchiveShared {
    static SHARED: OnceLock<ArchiveShared> = OnceLock::new();
    SHARED.get_or_init(|| {
        let forwarder_slot: Arc<Mutex<Option<Arc<ArchiveEventsForwarder>>>> =
            Arc::new(Mutex::new(None));
        let dispatcher = Arc::new(ArchiveEventsDispatcher {
            inner: Arc::clone(&forwarder_slot),
        });
        let sevenzip = resolve_sevenzip();
        log::info!("archive: 7-Zip={:?}", sevenzip);
        let mgr = Arc::new(ArchiveManager::new(sevenzip, dispatcher));
        let _guard = shared_runtime().enter();
        mgr.start_worker();
        ArchiveShared {
            mgr,
            forwarder: forwarder_slot,
        }
    })
}

fn serialize_queue(jobs: &[ArchiveJob]) -> String {
    serde_json::to_string(jobs).unwrap_or_else(|_| "[]".into())
}

struct ArchiveEventsDispatcher {
    inner: Arc<Mutex<Option<Arc<ArchiveEventsForwarder>>>>,
}

impl ArchiveEvents for ArchiveEventsDispatcher {
    fn job_updated(&self, job: &ArchiveJob) {
        if let Some(f) = self.inner.lock().unwrap().clone() {
            f.job_updated(job);
        }
    }
    fn progress(&self, job: &ArchiveJob) {
        if let Some(f) = self.inner.lock().unwrap().clone() {
            f.progress(job);
        }
    }
}

struct ArchiveEventsForwarder {
    qt_handle: cxx_qt::CxxQtThread<qobject::Archive>,
    mgr: Arc<ArchiveManager>,
}

impl ArchiveEventsForwarder {
    fn refresh(&self, completed_output: Option<String>, dirs: Option<Vec<String>>) {
        let mgr = Arc::clone(&self.mgr);
        let _ = self.qt_handle.queue(move |mut a| {
            let queue = shared_runtime().block_on(mgr.get_queue());
            a.as_mut()
                .set_queue_json(cxx_qt_lib::QString::from(&serialize_queue(&queue)));
            if let Some(out) = completed_output {
                a.as_mut().job_completed(cxx_qt_lib::QString::from(&out));
            }
            if let Some(dirs) = dirs {
                let json = serde_json::to_string(&dirs).unwrap_or_else(|_| "[]".into());
                a.as_mut().dirs_changed(cxx_qt_lib::QString::from(&json));
            }
        });
    }
}

impl ArchiveEvents for ArchiveEventsForwarder {
    fn job_updated(&self, job: &ArchiveJob) {
        // Completed jobs, and cancelled / failed ones that may have left
        // (then removed) a partial result, both invalidate the parent.
        let dirs = if !job.changed_dirs.is_empty()
            && matches!(
                job.status,
                JobStatus::Completed | JobStatus::Cancelled | JobStatus::Failed
            ) {
            Some(job.changed_dirs.clone())
        } else {
            None
        };
        let completed = (job.status == JobStatus::Completed).then(|| job.output_path.clone());
        self.refresh(completed, dirs);
    }
    fn progress(&self, _job: &ArchiveJob) {
        self.refresh(None, None);
    }
}

fn parse_paths(paths_json: &cxx_qt_lib::QString) -> Vec<String> {
    let s = paths_json.to_string();
    serde_json::from_str::<Vec<String>>(&s).unwrap_or_else(|_| {
        if s.trim().is_empty() {
            Vec::new()
        } else {
            vec![s]
        }
    })
}

impl qobject::Archive {
    fn add_extract_jobs(
        mut self: Pin<&mut qobject::Archive>,
        paths_json: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        ensure_forwarder(self.as_mut());
        let paths = parse_paths(&paths_json);
        if paths.is_empty() {
            return cxx_qt_lib::QString::from("[]");
        }
        let mgr = shared().mgr.clone();
        let added = shared_runtime().block_on(mgr.add_extract_jobs(paths));
        log::info!("archive: queued {} extract job(s)", added.len());
        let json = serde_json::to_string(&added).unwrap_or_else(|_| "[]".into());
        self.as_mut().refresh_queue();
        cxx_qt_lib::QString::from(&json)
    }

    fn add_compress_job(
        mut self: Pin<&mut qobject::Archive>,
        paths_json: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        ensure_forwarder(self.as_mut());
        let paths = parse_paths(&paths_json);
        if paths.is_empty() {
            return cxx_qt_lib::QString::from("null");
        }
        let mgr = shared().mgr.clone();
        let added = shared_runtime().block_on(mgr.add_compress_job(paths));
        if let Some(j) = &added {
            log::info!("archive: queued compress job ({} item(s)) → {}", j.inputs.len(), j.output_path);
        }
        let json = serde_json::to_string(&added).unwrap_or_else(|_| "null".into());
        self.as_mut().refresh_queue();
        cxx_qt_lib::QString::from(&json)
    }

    fn cancel_job(mut self: Pin<&mut qobject::Archive>, id: cxx_qt_lib::QString) {
        let mgr = shared().mgr.clone();
        shared_runtime().block_on(mgr.cancel_job(&id.to_string()));
        self.as_mut().refresh_queue();
    }

    fn remove_job(mut self: Pin<&mut qobject::Archive>, id: cxx_qt_lib::QString) {
        let mgr = shared().mgr.clone();
        shared_runtime().block_on(mgr.remove_job(&id.to_string()));
        self.as_mut().refresh_queue();
    }

    fn clear_completed(mut self: Pin<&mut qobject::Archive>) {
        let mgr = shared().mgr.clone();
        shared_runtime().block_on(mgr.clear_completed());
        self.as_mut().refresh_queue();
    }

    fn refresh_queue(mut self: Pin<&mut qobject::Archive>) {
        let mgr = shared().mgr.clone();
        let queue = shared_runtime().block_on(mgr.get_queue());
        self.as_mut()
            .set_queue_json(cxx_qt_lib::QString::from(&serialize_queue(&queue)));
    }

    fn archive_extensions(&self) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from(&ufb_core::archive::ARCHIVE_EXTENSIONS.join(","))
    }
}

fn ensure_forwarder(aobj: Pin<&mut qobject::Archive>) {
    use cxx_qt::Threading;
    let s = shared();
    let mut slot = s.forwarder.lock().unwrap();
    if slot.is_some() {
        return;
    }
    let qt_handle: cxx_qt::CxxQtThread<qobject::Archive> = aobj.as_ref().qt_thread();
    *slot = Some(Arc::new(ArchiveEventsForwarder {
        qt_handle,
        mgr: Arc::clone(&s.mgr),
    }));
}
