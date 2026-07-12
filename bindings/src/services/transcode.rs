//! `Transcode` QObject — wraps `core::transcode::TranscodeManager`.
//!
//! Async core; this binding bridges to QML by:
//!  * exposing `queue_json` (full queue as JSON, refreshed after every
//!    invokable call and on each event from core)
//!  * `add_jobs` / `cancel_job` / `remove_job` / `clear_completed`
//!    invokables that block_on the async core via the shared tokio
//!    runtime
//!  * a `TranscodeEventsForwarder` that runs on tokio threads and
//!    queues queue_json refreshes back onto the Qt thread via
//!    `cxx_qt::CxxQtThread::queue`
//!
//! ffmpeg / ffprobe / exiftool are resolved at QObject construction.
//! `bundled_tool` checks alongside the running ufb.exe for a sibling
//! `<name>.exe` (Windows) or `<name>` (macOS/Linux) and uses the
//! absolute path when found — matches the legacy Tauri build's layout
//! where ffmpeg.exe and friends sit next to the main exe in {app}\.
//! On macOS it also checks `<exe_dir>/../Resources/<name>/<name>` so
//! exiftool (a Perl script + lib/ tree, can't live in Contents/MacOS/
//! without tripping codesign --strict) can ship under Contents/
//! Resources/exiftool/. Falls back to the bare name (PATH lookup)
//! when the bundled binary is missing, so dev builds without an
//! `external/` sync still work as long as the user has the tool on
//! PATH.

use crate::runtime::shared_runtime;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::{Arc, Mutex, OnceLock};
use ufb_core::events::TranscodeEvents;
use ufb_core::transcode::{TranscodeJob, TranscodeManager};

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
        type Transcode = super::TranscodeRust;

        /// Queue `paths_json` (a JSON array of input paths) for
        /// transcode. Returns the JSON array of newly created jobs
        /// (each carries the assigned id). Worker is started lazily
        /// on first call.
        #[qinvokable]
        fn add_jobs(self: Pin<&mut Transcode>, paths_json: QString) -> QString;

        /// Cancel a job by id. Mid-process jobs get TerminateProcess /
        /// SIGTERM; queued jobs flip to Cancelled in place.
        #[qinvokable]
        fn cancel_job(self: Pin<&mut Transcode>, id: QString);

        /// Remove a finished/failed/cancelled job from the visible queue.
        #[qinvokable]
        fn remove_job(self: Pin<&mut Transcode>, id: QString);

        /// Clear all Completed / Failed / Cancelled jobs.
        #[qinvokable]
        fn clear_completed(self: Pin<&mut Transcode>);

        /// Force re-pull the queue (e.g. on view open).
        #[qinvokable]
        fn refresh_queue(self: Pin<&mut Transcode>);
    }

    // Threading required so the events forwarder can queue closures
    // back onto the Qt thread from tokio worker tasks.
    impl cxx_qt::Threading for Transcode {}
}

pub struct TranscodeRust {
    pub queue_json: cxx_qt_lib::QString,
}

impl Default for TranscodeRust {
    fn default() -> Self {
        Self {
            queue_json: cxx_qt_lib::QString::from("[]"),
        }
    }
}

/// Holds the manager + a slot for the live event-forwarder. The
/// dispatcher passed into `TranscodeManager::new` holds an
/// `Arc<Mutex<Option<…>>>` pointing at the SAME slot, so writing the
/// forwarder here lights up event delivery for both call sites.
struct TranscodeShared {
    mgr: Arc<TranscodeManager>,
    forwarder: Arc<Mutex<Option<Arc<TranscodeEventsForwarder>>>>,
}

/// Look for `name.exe` (or bare `name` on non-Windows) next to the
/// running ufb.exe. Returns the absolute path on hit, the bare name
/// on miss — bare name lets std::process::Command fall back to PATH
/// resolution so a dev environment without external/ synced still
/// works for users who have the tool installed system-wide.
///
/// macOS-only second probe: `<exe_dir>/../Resources/<name>/<name>`.
/// ffmpeg + ffprobe are Mach-O and live in Contents/MacOS/ next to
/// the ufb binary; exiftool is a Perl script with a sibling lib/
/// tree and lives under Contents/Resources/exiftool/ so codesign
/// --strict doesn't reject Contents/MacOS/lib/ subdirectories.
fn bundled_tool(name: &str) -> PathBuf {
    let exe_name = if cfg!(target_os = "windows") {
        format!("{}.exe", name)
    } else {
        name.to_string()
    };
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let local = dir.join(&exe_name);
            if local.is_file() {
                return local;
            }
            #[cfg(target_os = "macos")]
            {
                // Contents/MacOS/<exe> → Contents/Resources/<name>/<name>
                let resources = dir.join("..").join("Resources").join(name).join(&exe_name);
                if resources.is_file() {
                    return resources;
                }
            }
        }
    }
    PathBuf::from(name)
}

fn shared() -> &'static TranscodeShared {
    static SHARED: OnceLock<TranscodeShared> = OnceLock::new();
    SHARED.get_or_init(|| {
        let forwarder_slot: Arc<Mutex<Option<Arc<TranscodeEventsForwarder>>>> =
            Arc::new(Mutex::new(None));
        let dispatcher = Arc::new(TranscodeEventsDispatcher {
            inner: Arc::clone(&forwarder_slot),
        });
        let ffmpeg = bundled_tool("ffmpeg");
        let ffprobe = bundled_tool("ffprobe");
        let exiftool = bundled_tool("exiftool");
        log::info!(
            "transcode: ffmpeg={:?} ffprobe={:?} exiftool={:?}",
            ffmpeg, ffprobe, exiftool
        );
        let mgr = Arc::new(TranscodeManager::new(
            ffmpeg, ffprobe, exiftool, dispatcher,
        ));
        // Worker runs on the shared tokio runtime. start_worker spawns
        // a long-lived task that pulls Queued jobs serially.
        let _guard = shared_runtime().enter();
        mgr.start_worker();
        TranscodeShared {
            mgr,
            forwarder: forwarder_slot,
        }
    })
}

fn serialize_queue(jobs: &[TranscodeJob]) -> String {
    serde_json::to_string(jobs).unwrap_or_else(|_| "[]".into())
}

/// Held by the manager. Looks up the latest TranscodeEventsForwarder
/// (set when a QObject instance attaches) and routes events to it.
/// This indirection lets the manager outlive any individual QObject
/// instance — relevant if a hot-reload ever rebuilds the QML root.
struct TranscodeEventsDispatcher {
    inner: Arc<Mutex<Option<Arc<TranscodeEventsForwarder>>>>,
}

impl TranscodeEvents for TranscodeEventsDispatcher {
    fn job_updated(&self, job: &TranscodeJob) {
        if let Some(f) = self.inner.lock().unwrap().clone() {
            f.job_updated(job);
        }
    }
    fn progress(&self, job: &TranscodeJob) {
        if let Some(f) = self.inner.lock().unwrap().clone() {
            f.progress(job);
        }
    }
}

/// Translates events into queued queue_json refreshes on the Qt thread.
struct TranscodeEventsForwarder {
    qt_handle: cxx_qt::CxxQtThread<qobject::Transcode>,
    mgr: Arc<TranscodeManager>,
}

impl TranscodeEventsForwarder {
    fn refresh(&self) {
        let mgr = Arc::clone(&self.mgr);
        let _ = self.qt_handle.queue(move |mut t| {
            // We're on the Qt thread now; block_on a quick getter is
            // fine because the manager's lock is short-held.
            let queue = shared_runtime().block_on(mgr.get_queue());
            t.as_mut()
                .set_queue_json(cxx_qt_lib::QString::from(&serialize_queue(&queue)));
        });
    }
}

impl TranscodeEvents for TranscodeEventsForwarder {
    fn job_updated(&self, _job: &TranscodeJob) {
        self.refresh();
    }
    fn progress(&self, _job: &TranscodeJob) {
        self.refresh();
    }
}

impl qobject::Transcode {
    fn add_jobs(
        mut self: Pin<&mut qobject::Transcode>,
        paths_json: cxx_qt_lib::QString,
    ) -> cxx_qt_lib::QString {
        ensure_forwarder(self.as_mut());

        let paths_s = paths_json.to_string();
        let paths: Vec<String> = serde_json::from_str(&paths_s).unwrap_or_else(|_| {
            // Allow passing a single bare path string for convenience.
            vec![paths_s]
        });
        if paths.is_empty() {
            return cxx_qt_lib::QString::from("[]");
        }
        let mgr = shared().mgr.clone();
        let added = shared_runtime().block_on(mgr.add_jobs(paths));
        log::info!("transcode: queued {} job(s)", added.len());
        let added_json = serde_json::to_string(&added).unwrap_or_else(|_| "[]".into());
        // After enqueue, refresh the visible queue so the new jobs
        // show up before the worker emits its first event.
        let queue = shared_runtime().block_on(mgr.get_queue());
        self.as_mut()
            .set_queue_json(cxx_qt_lib::QString::from(&serialize_queue(&queue)));
        cxx_qt_lib::QString::from(&added_json)
    }

    fn cancel_job(mut self: Pin<&mut qobject::Transcode>, id: cxx_qt_lib::QString) {
        let id_s = id.to_string();
        let mgr = shared().mgr.clone();
        if let Err(e) = shared_runtime().block_on(mgr.cancel_job(&id_s)) {
            log::warn!("transcode: cancel_job({}) failed: {}", id_s, e);
        }
        self.as_mut().refresh_queue();
    }

    fn remove_job(mut self: Pin<&mut qobject::Transcode>, id: cxx_qt_lib::QString) {
        let id_s = id.to_string();
        let mgr = shared().mgr.clone();
        shared_runtime().block_on(mgr.remove_job(&id_s));
        self.as_mut().refresh_queue();
    }

    fn clear_completed(mut self: Pin<&mut qobject::Transcode>) {
        let mgr = shared().mgr.clone();
        shared_runtime().block_on(mgr.clear_completed());
        self.as_mut().refresh_queue();
    }

    fn refresh_queue(mut self: Pin<&mut qobject::Transcode>) {
        let mgr = shared().mgr.clone();
        let queue = shared_runtime().block_on(mgr.get_queue());
        self.as_mut()
            .set_queue_json(cxx_qt_lib::QString::from(&serialize_queue(&queue)));
    }
}

/// Wire the forwarder if it hasn't been wired yet. Called lazily
/// from the first invokable rather than from a `Default` constructor
/// hook because we need a `Pin<&mut Transcode>` to grab a
/// `qt_thread()` handle. Writing to `s.forwarder` also lights up the
/// dispatcher (same slot Arc).
fn ensure_forwarder(tobj: Pin<&mut qobject::Transcode>) {
    use cxx_qt::Threading;
    let s = shared();
    let mut slot = s.forwarder.lock().unwrap();
    if slot.is_some() {
        return;
    }
    let qt_handle: cxx_qt::CxxQtThread<qobject::Transcode> = tobj.as_ref().qt_thread();
    let f = Arc::new(TranscodeEventsForwarder {
        qt_handle,
        mgr: Arc::clone(&s.mgr),
    });
    *slot = Some(f);
}
