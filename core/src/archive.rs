//! Archive jobs: extract (zip / 7z / rar / tar / …) and compress-to-zip,
//! both driven by a bundled 7-Zip CLI (`7zz` on macOS, `7z.exe` on
//! Windows). Structurally a sibling of `transcode.rs`: a sequential
//! worker drains a queue one child process at a time, progress is
//! parsed from 7-Zip's `-bsp1` stream, and cancel kills the child and
//! removes whatever the job had created so far.
//!
//! Placement rules (Finder / Explorer conventions):
//!
//! * Extract: contents land next to the archive. If the archive has a
//!   single top-level entry that doesn't already exist beside it, it is
//!   extracted straight into the parent (no `foo/foo/…` double-nest);
//!   otherwise into a fresh sibling folder named after the archive
//!   (`foo`, `foo 2`, …). Never overwrites — the destination is always
//!   a path that did not exist when the job was planned.
//! * Compress: one `.zip` beside the selection, named after the single
//!   selected item or `Archive.zip` for several, with ` 2`/` 3` suffixes
//!   on collision. Items are stored by name (no absolute paths); items
//!   from different folders are added in one pass per folder so the zip
//!   stays flat, falling back to paths relative to the common ancestor
//!   when basenames would collide.

use crate::events::ArchiveEventsArc;
use serde::{Deserialize, Serialize};
use std::path::{Component, Path, PathBuf};
use std::sync::Arc;
use tokio::io::AsyncReadExt;
use tokio::sync::Mutex;

#[cfg(target_os = "windows")]
const CREATE_NO_WINDOW: u32 = 0x08000000;

/// Extensions the "Extract Here" menu item accepts. Lowercase compare on
/// the final extension only, so `foo.tar.gz` matches through `gz`.
pub const ARCHIVE_EXTENSIONS: &[&str] = &[
    "zip", "7z", "rar", "tar", "gz", "tgz", "bz2", "tbz", "tbz2", "xz", "txz", "zst", "tzst",
    "lz", "lzma", "z", "cab", "iso", "wim", "arj", "lzh", "cpio",
];

/// Suffixes that mean "a tar wrapped in a stream compressor" — 7-Zip
/// unpacks those in two stages (`x -so | x -si -ttar`).
const COMPOUND_TAR_SUFFIXES: &[&str] = &[
    ".tar.gz", ".tgz", ".tar.bz2", ".tbz", ".tbz2", ".tar.xz", ".txz", ".tar.zst", ".tzst",
    ".tar.lz", ".tar.lzma", ".tar.z",
];

pub fn is_archive_path(path: &str) -> bool {
    let name = Path::new(path)
        .file_name()
        .map(|n| n.to_string_lossy().to_lowercase())
        .unwrap_or_default();
    match name.rsplit_once('.') {
        Some((stem, ext)) if !stem.is_empty() => ARCHIVE_EXTENSIONS.contains(&ext),
        _ => false,
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum ArchiveKind {
    Extract,
    Compress,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum JobStatus {
    Queued,
    Processing,
    Completed,
    Failed,
    Cancelled,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ArchiveJob {
    pub id: String,
    pub kind: ArchiveKind,
    /// Extract: the single archive. Compress: every selected item.
    pub inputs: Vec<String>,
    /// Extract: the folder (or single entry) the contents land in.
    /// Compress: the `.zip` being written. Provisional until the job is
    /// planned; final once it reaches Processing.
    pub output_path: String,
    pub status: JobStatus,
    /// 0–100.
    pub progress: f64,
    /// Entry 7-Zip most recently reported working on (display only).
    pub current_item: String,
    pub error: Option<String>,
    /// Non-fatal 7-Zip warnings (exit code 1) on an otherwise complete job.
    pub warning: Option<String>,
    /// Directories whose listings changed when the job completed. The
    /// binding forwards these to `FileOps.dirs_changed` so open views
    /// refresh themselves.
    pub changed_dirs: Vec<String>,
}

struct ArchiveState {
    queue: Vec<ArchiveJob>,
    active_child: Option<tokio::process::Child>,
    events: ArchiveEventsArc,
}

impl ArchiveState {
    fn update_job(&mut self, job_id: &str, f: impl FnOnce(&mut ArchiveJob)) -> Option<ArchiveJob> {
        let job = self.queue.iter_mut().find(|j| j.id == job_id)?;
        f(job);
        let snapshot = job.clone();
        self.events.job_updated(&snapshot);
        Some(snapshot)
    }

    fn is_cancelled(&self, job_id: &str) -> bool {
        self.queue
            .iter()
            .any(|j| j.id == job_id && j.status == JobStatus::Cancelled)
    }
}

pub struct ArchiveManager {
    state: Arc<Mutex<ArchiveState>>,
    sevenzip_path: PathBuf,
}

/// Where an extraction will write, decided before the child starts.
#[derive(Debug, Clone, PartialEq, Eq)]
struct ExtractPlan {
    /// Passed to 7-Zip as `-o<dest>`.
    dest: PathBuf,
    /// The path the job creates (shown to the user, removed on cancel /
    /// failure). Either `dest` itself or `dest/<single root entry>`.
    created: PathBuf,
    /// `x -so | x -si -ttar` two-stage unpack.
    compound_tar: bool,
}

/// One `7z a` invocation: `names` relative to `cwd`.
#[derive(Debug, Clone, PartialEq, Eq)]
struct CompressGroup {
    cwd: PathBuf,
    names: Vec<String>,
}

impl ArchiveManager {
    pub fn new(sevenzip_path: PathBuf, events: ArchiveEventsArc) -> Self {
        Self {
            state: Arc::new(Mutex::new(ArchiveState {
                queue: Vec::new(),
                active_child: None,
                events,
            })),
            sevenzip_path,
        }
    }

    /// Queue one extract job per archive path. Non-archives are skipped.
    pub async fn add_extract_jobs(&self, paths: Vec<String>) -> Vec<ArchiveJob> {
        let mut state = self.state.lock().await;
        let mut added = Vec::new();
        for p in paths {
            if !is_archive_path(&p) {
                continue;
            }
            let archive = Path::new(&p);
            let parent = archive.parent().unwrap_or(archive);
            let provisional = parent.join(archive_stem(archive));
            let job = ArchiveJob {
                id: uuid::Uuid::new_v4().to_string(),
                kind: ArchiveKind::Extract,
                inputs: vec![p.clone()],
                output_path: provisional.to_string_lossy().into_owned(),
                status: JobStatus::Queued,
                progress: 0.0,
                current_item: String::new(),
                error: None,
                warning: None,
                changed_dirs: Vec::new(),
            };
            added.push(job.clone());
            state.queue.push(job);
        }
        added
    }

    /// Queue a single job that zips every path in `paths` into one
    /// archive beside them. Returns `None` for an empty selection.
    pub async fn add_compress_job(&self, paths: Vec<String>) -> Option<ArchiveJob> {
        let paths: Vec<String> = paths.into_iter().filter(|p| !p.is_empty()).collect();
        if paths.is_empty() {
            return None;
        }
        let output = plan_zip_output(&paths);
        let job = ArchiveJob {
            id: uuid::Uuid::new_v4().to_string(),
            kind: ArchiveKind::Compress,
            inputs: paths,
            output_path: output.to_string_lossy().into_owned(),
            status: JobStatus::Queued,
            progress: 0.0,
            current_item: String::new(),
            error: None,
            warning: None,
            changed_dirs: Vec::new(),
        };
        let mut state = self.state.lock().await;
        state.queue.push(job.clone());
        Some(job)
    }

    pub async fn get_queue(&self) -> Vec<ArchiveJob> {
        self.state.lock().await.queue.clone()
    }

    pub async fn cancel_job(&self, id: &str) {
        let mut state = self.state.lock().await;
        let was_processing = state
            .queue
            .iter()
            .any(|j| j.id == id && j.status == JobStatus::Processing);
        state.update_job(id, |job| {
            if matches!(job.status, JobStatus::Queued | JobStatus::Processing) {
                job.status = JobStatus::Cancelled;
            }
        });
        if was_processing {
            if let Some(child) = state.active_child.as_mut() {
                let _ = child.kill().await;
            }
        }
    }

    pub async fn remove_job(&self, id: &str) {
        self.state.lock().await.queue.retain(|j| j.id != id);
    }

    pub async fn clear_completed(&self) {
        self.state.lock().await.queue.retain(|j| {
            !matches!(
                j.status,
                JobStatus::Completed | JobStatus::Failed | JobStatus::Cancelled
            )
        });
    }

    /// Background worker: processes queued jobs one at a time, forever.
    pub fn start_worker(self: &Arc<Self>) {
        let mgr = Arc::clone(self);
        tokio::spawn(async move {
            loop {
                let next = {
                    let state = mgr.state.lock().await;
                    state
                        .queue
                        .iter()
                        .find(|j| j.status == JobStatus::Queued)
                        .map(|j| (j.id.clone(), j.kind, j.inputs.clone()))
                };
                match next {
                    Some((id, ArchiveKind::Extract, inputs)) => mgr.process_extract(&id, &inputs).await,
                    Some((id, ArchiveKind::Compress, inputs)) => mgr.process_compress(&id, &inputs).await,
                    None => tokio::time::sleep(std::time::Duration::from_millis(500)).await,
                }
            }
        });
    }

    // ── Extract ──────────────────────────────────────────────────────

    async fn process_extract(&self, job_id: &str, inputs: &[String]) {
        let Some(archive_s) = inputs.first().cloned() else {
            self.fail_job(job_id, "No archive path").await;
            return;
        };
        let archive = PathBuf::from(&archive_s);
        if !archive.is_file() {
            self.fail_job(job_id, &format!("Archive not found: {}", archive_s)).await;
            return;
        }
        if self.mark_processing(job_id).await.is_none() {
            return; // cancelled while queued
        }

        let plan = match self.plan_extract(&archive).await {
            Ok(p) => p,
            Err(e) => {
                self.fail_job(job_id, &e).await;
                return;
            }
        };
        {
            let mut state = self.state.lock().await;
            state.update_job(job_id, |job| {
                job.output_path = plan.created.to_string_lossy().into_owned();
            });
        }
        if let Err(e) = tokio::fs::create_dir_all(&plan.dest).await {
            self.fail_job(job_id, &format!("Cannot create {}: {}", plan.dest.display(), e)).await;
            return;
        }

        let result = if plan.compound_tar {
            self.run_compound_tar_extract(job_id, &archive, &plan.dest).await
        } else {
            let dest_switch = format!("-o{}", plan.dest.display());
            let args = [
                "x", "-y", "-p", "-aou", "-bso0", "-bse2", "-bsp1", "-sccUTF-8",
                dest_switch.as_str(), "--", archive_s.as_str(),
            ];
            self.run_sevenzip(job_id, &args, None, 0.0, 1.0).await
        };

        let parent = archive
            .parent()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        self.finish_job(job_id, result, &plan.created, vec![parent]).await;
    }

    /// Decide where the archive's contents go. Lists the archive first
    /// (also the cheapest way to detect encryption / corruption up front).
    async fn plan_extract(&self, archive: &Path) -> Result<ExtractPlan, String> {
        let parent = archive
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .ok_or_else(|| "Archive has no parent directory".to_string())?
            .to_path_buf();
        let stem = archive_stem(archive);
        let compound_tar = is_compound_tar(archive);

        let entries = if compound_tar {
            self.list_compound_tar(archive).await?
        } else {
            let archive_s = archive.to_string_lossy().into_owned();
            let out = self
                .run_capture(&["l", "-slt", "-ba", "-p", "-sccUTF-8", "--", archive_s.as_str()], None)
                .await?;
            parse_listing(&out)
        };

        let created = match single_root_entry(&entries) {
            Some(root) if !parent.join(&root).exists() => {
                return Ok(ExtractPlan {
                    dest: parent.clone(),
                    created: parent.join(root),
                    compound_tar,
                });
            }
            _ => unique_child(&parent, &stem, None),
        };
        Ok(ExtractPlan {
            dest: created.clone(),
            created,
            compound_tar,
        })
    }

    /// `7z x -so <outer> | 7z l -si -ttar -slt -ba` — list the inner tar
    /// without materialising it.
    async fn list_compound_tar(&self, archive: &Path) -> Result<Vec<ListedEntry>, String> {
        let archive_s = archive.to_string_lossy().into_owned();
        let outer = ["x", "-so", "-p", "-bso0", "-bse2", "-bsp0", "--", archive_s.as_str()];
        let inner = ["l", "-si", "-ttar", "-slt", "-ba", "-sccUTF-8"];
        let (stdout, stderr, status) = self.run_pipeline_capture(&outer, &inner).await?;
        if !status.success() {
            return Err(describe_failure(status.code(), &stderr));
        }
        Ok(parse_listing(&stdout))
    }

    /// Two-stage unpack. Progress comes from the inner tar reader; with a
    /// streamed input 7-Zip can't size the job, so the bar mostly shows
    /// the current entry rather than a percentage.
    async fn run_compound_tar_extract(
        &self,
        job_id: &str,
        archive: &Path,
        dest: &Path,
    ) -> Result<Option<String>, String> {
        let archive_s = archive.to_string_lossy().into_owned();
        let dest_switch = format!("-o{}", dest.display());
        let outer = ["x", "-so", "-p", "-bso0", "-bse2", "-bsp0", "--", archive_s.as_str()];
        let inner = [
            "x", "-si", "-ttar", "-y", "-aou", "-bso0", "-bse2", "-bsp1", "-sccUTF-8",
            dest_switch.as_str(),
        ];
        self.run_pipeline(job_id, &outer, &inner).await
    }

    // ── Compress ─────────────────────────────────────────────────────

    async fn process_compress(&self, job_id: &str, inputs: &[String]) {
        let output = {
            let state = self.state.lock().await;
            state
                .queue
                .iter()
                .find(|j| j.id == job_id)
                .map(|j| PathBuf::from(&j.output_path))
        };
        let Some(output) = output else { return };
        for p in inputs {
            if !Path::new(p).exists() {
                self.fail_job(job_id, &format!("Not found: {}", p)).await;
                return;
            }
        }
        if self.mark_processing(job_id).await.is_none() {
            return;
        }
        // Re-resolve the collision-free name now, in case something
        // appeared between enqueue and start.
        let output = if output.exists() {
            let fresh = plan_zip_output(inputs);
            let mut state = self.state.lock().await;
            state.update_job(job_id, |job| {
                job.output_path = fresh.to_string_lossy().into_owned();
            });
            fresh
        } else {
            output
        };

        let groups = match plan_compress_groups(inputs) {
            Ok(g) => g,
            Err(e) => {
                self.fail_job(job_id, &e).await;
                return;
            }
        };
        let output_s = output.to_string_lossy().into_owned();
        let n = groups.len().max(1) as f64;
        let mut result: Result<Option<String>, String> = Ok(None);
        let mut warnings: Vec<String> = Vec::new();
        for (i, group) in groups.iter().enumerate() {
            let mut args: Vec<&str> = vec![
                "a", "-tzip", "-y", "-bso0", "-bse2", "-bsp1", "-sccUTF-8", "-xr!.DS_Store",
                "--", output_s.as_str(),
            ];
            args.extend(group.names.iter().map(|s| s.as_str()));
            let base = i as f64 / n;
            match self.run_sevenzip(job_id, &args, Some(&group.cwd), base, 1.0 / n).await {
                Ok(Some(w)) => warnings.push(w),
                Ok(None) => {}
                Err(e) => {
                    result = Err(e);
                    break;
                }
            }
        }
        let result = result.map(|_| {
            if warnings.is_empty() {
                None
            } else {
                Some(warnings.join("; "))
            }
        });
        let parent = output
            .parent()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        self.finish_job(job_id, result, &output, vec![parent]).await;
    }

    // ── Shared plumbing ──────────────────────────────────────────────

    /// Flip Queued → Processing. Returns None if the job was cancelled or
    /// removed while waiting its turn.
    async fn mark_processing(&self, job_id: &str) -> Option<()> {
        let mut state = self.state.lock().await;
        let job = state.queue.iter().find(|j| j.id == job_id)?;
        if job.status != JobStatus::Queued {
            return None;
        }
        state.update_job(job_id, |job| job.status = JobStatus::Processing);
        Some(())
    }

    async fn fail_job(&self, job_id: &str, error: &str) {
        log::warn!("archive job {} failed: {}", job_id, error);
        let mut state = self.state.lock().await;
        let msg = error.to_string();
        state.update_job(job_id, |job| {
            if job.status != JobStatus::Cancelled {
                job.status = JobStatus::Failed;
                job.error = Some(msg);
            }
        });
    }

    /// Apply the outcome of the child run(s): success → Completed with
    /// `changed_dirs`; failure / cancel → remove `created` so a half-
    /// written result never lingers beside the user's files.
    async fn finish_job(
        &self,
        job_id: &str,
        result: Result<Option<String>, String>,
        created: &Path,
        changed_dirs: Vec<String>,
    ) {
        let cancelled = self.state.lock().await.is_cancelled(job_id);
        match result {
            Ok(warning) if !cancelled => {
                let mut state = self.state.lock().await;
                state.update_job(job_id, |job| {
                    job.status = JobStatus::Completed;
                    job.progress = 100.0;
                    job.current_item.clear();
                    job.warning = warning;
                    job.changed_dirs = changed_dirs;
                });
            }
            Ok(_) => {
                remove_created(created).await;
                // Status already Cancelled; still tell the UI the parent
                // may have briefly shown a partial result.
                let mut state = self.state.lock().await;
                state.update_job(job_id, |job| job.changed_dirs = changed_dirs);
            }
            Err(e) => {
                remove_created(created).await;
                if cancelled {
                    let mut state = self.state.lock().await;
                    state.update_job(job_id, |job| job.changed_dirs = changed_dirs);
                } else {
                    self.fail_job(job_id, &e).await;
                }
            }
        }
    }

    fn command(&self, args: &[&str], cwd: Option<&Path>) -> tokio::process::Command {
        let mut cmd = tokio::process::Command::new(&self.sevenzip_path);
        cmd.args(args);
        if let Some(d) = cwd {
            cmd.current_dir(d);
        }
        cmd.stdin(std::process::Stdio::null());
        #[cfg(target_os = "windows")]
        cmd.creation_flags(CREATE_NO_WINDOW);
        cmd.kill_on_drop(true);
        cmd
    }

    /// Run 7-Zip to completion and return its stdout. Used for listings.
    async fn run_capture(&self, args: &[&str], cwd: Option<&Path>) -> Result<String, String> {
        let output = self
            .command(args, cwd)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .output()
            .await
            .map_err(|e| format!("Failed to run 7-Zip ({}): {}", self.sevenzip_path.display(), e))?;
        let stderr = String::from_utf8_lossy(&output.stderr).into_owned();
        if !output.status.success() {
            return Err(describe_failure(output.status.code(), &stderr));
        }
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    }

    /// Run one 7-Zip invocation as the job's active child, streaming
    /// progress into the job. `base` + `span` map this invocation's 0–100
    /// onto the job's overall bar (multi-group compress).
    async fn run_sevenzip(
        &self,
        job_id: &str,
        args: &[&str],
        cwd: Option<&Path>,
        base: f64,
        span: f64,
    ) -> Result<Option<String>, String> {
        let mut child = self
            .command(args, cwd)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .map_err(|e| format!("Failed to run 7-Zip ({}): {}", self.sevenzip_path.display(), e))?;
        let stdout = child.stdout.take();
        let stderr = child.stderr.take();
        {
            let mut state = self.state.lock().await;
            if state.is_cancelled(job_id) {
                let _ = child.kill().await;
                return Err("Cancelled".into());
            }
            state.active_child = Some(child);
        }
        let stderr_task = tokio::spawn(read_to_string_capped(stderr));
        if let Some(out) = stdout {
            self.pump_progress(job_id, out, base, span).await;
        }
        let status = self.wait_active_child().await;
        let stderr_text = stderr_task.await.unwrap_or_default();
        interpret_exit(status, &stderr_text)
    }

    /// `outer | inner`, with `inner` as the active (killable) child. The
    /// outer producer dies of SIGPIPE / ERROR_BROKEN_PIPE once the inner
    /// is killed; `kill_on_drop` covers the rest.
    async fn run_pipeline(
        &self,
        job_id: &str,
        outer: &[&str],
        inner: &[&str],
    ) -> Result<Option<String>, String> {
        let mut producer = self
            .command(outer, None)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .map_err(|e| format!("Failed to run 7-Zip: {}", e))?;
        let feed: std::process::Stdio = producer
            .stdout
            .take()
            .ok_or("7-Zip pipe missing")?
            .try_into()
            .map_err(|e| format!("pipe: {}", e))?;
        let producer_err = tokio::spawn(read_to_string_capped(producer.stderr.take()));
        let mut child = self
            .command(inner, None)
            .stdin(feed)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .map_err(|e| format!("Failed to run 7-Zip: {}", e))?;
        let stdout = child.stdout.take();
        let stderr = child.stderr.take();
        {
            let mut state = self.state.lock().await;
            if state.is_cancelled(job_id) {
                let _ = child.kill().await;
                let _ = producer.kill().await;
                return Err("Cancelled".into());
            }
            state.active_child = Some(child);
        }
        let stderr_task = tokio::spawn(read_to_string_capped(stderr));
        if let Some(out) = stdout {
            self.pump_progress(job_id, out, 0.0, 1.0).await;
        }
        let status = self.wait_active_child().await;
        let _ = producer.kill().await;
        let producer_status = producer.wait().await;
        let mut stderr_text = stderr_task.await.unwrap_or_default();
        let outer_err = producer_err.await.unwrap_or_default();
        if !outer_err.trim().is_empty() {
            stderr_text.push('\n');
            stderr_text.push_str(&outer_err);
        }
        // A broken outer stage (wrong password on the .gz layer, say)
        // surfaces as the inner tar reader failing on truncated input.
        // Prefer the outer's exit code so the message points at the cause.
        match producer_status {
            Ok(s) if !s.success() && s.code().map_or(false, |c| c >= 2) => {
                Err(describe_failure(s.code(), &stderr_text))
            }
            _ => interpret_exit(status, &stderr_text),
        }
    }

    async fn run_pipeline_capture(
        &self,
        outer: &[&str],
        inner: &[&str],
    ) -> Result<(String, String, std::process::ExitStatus), String> {
        let mut producer = self
            .command(outer, None)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .map_err(|e| format!("Failed to run 7-Zip: {}", e))?;
        let feed: std::process::Stdio = producer
            .stdout
            .take()
            .ok_or("7-Zip pipe missing")?
            .try_into()
            .map_err(|e| format!("pipe: {}", e))?;
        let producer_err = tokio::spawn(read_to_string_capped(producer.stderr.take()));
        let consumer = self
            .command(inner, None)
            .stdin(feed)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .output()
            .await
            .map_err(|e| format!("Failed to run 7-Zip: {}", e))?;
        let producer_status = producer.wait().await;
        let mut stderr = String::from_utf8_lossy(&consumer.stderr).into_owned();
        stderr.push('\n');
        stderr.push_str(&producer_err.await.unwrap_or_default());
        let status = match producer_status {
            Ok(s) if !s.success() && s.code().map_or(false, |c| c >= 2) => s,
            _ => consumer.status,
        };
        Ok((String::from_utf8_lossy(&consumer.stdout).into_owned(), stderr, status))
    }

    async fn wait_active_child(&self) -> Result<std::process::ExitStatus, String> {
        let child = {
            let mut state = self.state.lock().await;
            state.active_child.take()
        };
        match child {
            Some(mut c) => c.wait().await.map_err(|e| format!("Failed to wait for 7-Zip: {}", e)),
            None => Err("No active 7-Zip process".into()),
        }
    }

    /// Read 7-Zip's `-bsp1` stream and mirror `NN%` / current entry into
    /// the job. Stops early (and lets the caller kill the child) if the
    /// job was cancelled.
    async fn pump_progress(
        &self,
        job_id: &str,
        mut stdout: tokio::process::ChildStdout,
        base: f64,
        span: f64,
    ) {
        let mut parser = ProgressParser::default();
        let mut buf = [0u8; 4096];
        let mut last_emit = std::time::Instant::now() - std::time::Duration::from_secs(1);
        let mut last_pct: i64 = -1;
        loop {
            let n = match stdout.read(&mut buf).await {
                Ok(0) | Err(_) => break,
                Ok(n) => n,
            };
            let mut newest: Option<ProgressTick> = None;
            for tick in parser.feed(&buf[..n]) {
                newest = Some(tick);
            }
            let Some(tick) = newest else { continue };
            let pct = tick.percent.unwrap_or(last_pct.max(0) as u8) as i64;
            let changed = pct != last_pct;
            let due = last_emit.elapsed() >= std::time::Duration::from_millis(150);
            if !(changed || due) {
                continue;
            }
            last_pct = pct;
            last_emit = std::time::Instant::now();
            let mut state = self.state.lock().await;
            if state.is_cancelled(job_id) {
                break;
            }
            if let Some(job) = state.queue.iter_mut().find(|j| j.id == job_id) {
                job.progress = ((base + span * pct as f64 / 100.0) * 100.0).clamp(0.0, 100.0);
                if let Some(item) = tick.item {
                    job.current_item = item;
                }
                let snapshot = job.clone();
                state.events.progress(&snapshot);
            }
        }
    }
}

// ── Pure helpers (unit-tested below) ─────────────────────────────────

#[derive(Debug, Clone, PartialEq, Eq)]
struct ListedEntry {
    path: String,
    is_dir: bool,
}

/// Parse `7z l -slt -ba` output: blank-line-separated records of
/// `Key = Value` lines. Only `Path` and `Folder` matter here.
fn parse_listing(text: &str) -> Vec<ListedEntry> {
    let mut out = Vec::new();
    let mut path: Option<String> = None;
    let mut is_dir = false;
    let flush = |path: &mut Option<String>, is_dir: &mut bool, out: &mut Vec<ListedEntry>| {
        if let Some(p) = path.take() {
            out.push(ListedEntry { path: p, is_dir: *is_dir });
        }
        *is_dir = false;
    };
    for line in text.lines() {
        let line = line.trim_end_matches('\r');
        if line.trim().is_empty() {
            flush(&mut path, &mut is_dir, &mut out);
            continue;
        }
        if let Some((k, v)) = line.split_once(" = ") {
            match k.trim() {
                "Path" => {
                    // A new Path inside the same record means the record
                    // separator was missing — flush defensively.
                    if path.is_some() {
                        flush(&mut path, &mut is_dir, &mut out);
                    }
                    path = Some(v.to_string());
                }
                "Folder" => is_dir = v.trim() == "+",
                _ => {}
            }
        }
    }
    flush(&mut path, &mut is_dir, &mut out);
    out
}

/// First path component of an archive entry, normalising both slash
/// styles and stripping a leading `./`.
fn top_component(path: &str) -> Option<String> {
    path.split(['/', '\\'])
        .map(str::trim)
        .find(|c| !c.is_empty() && *c != ".")
        .map(str::to_string)
}

/// If every entry lives under one top-level name, return it. A single
/// loose file counts too (a zip holding just `report.pdf` lands as
/// `report.pdf`, not `report/report.pdf`). Entries with unsafe
/// components (`..`, absolute) veto the shortcut so those always land in
/// a fresh folder where 7-Zip's own sanitising applies.
fn single_root_entry(entries: &[ListedEntry]) -> Option<String> {
    let mut root: Option<String> = None;
    for e in entries {
        let raw = e.path.as_str();
        if raw.starts_with('/') || raw.starts_with('\\') || raw.contains("..") || raw.contains(':') {
            return None;
        }
        let top = top_component(raw)?;
        match &root {
            None => root = Some(top),
            Some(r) if *r == top => {}
            Some(_) => return None,
        }
    }
    root
}

fn is_compound_tar(archive: &Path) -> bool {
    let name = archive
        .file_name()
        .map(|n| n.to_string_lossy().to_lowercase())
        .unwrap_or_default();
    COMPOUND_TAR_SUFFIXES.iter().any(|s| name.ends_with(s))
}

/// `foo.zip` → `foo`, `foo.tar.gz` → `foo`, `foo` → `foo`.
fn archive_stem(archive: &Path) -> String {
    let name = archive
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let lower = name.to_lowercase();
    for s in COMPOUND_TAR_SUFFIXES {
        if lower.ends_with(s) && lower.len() > s.len() {
            return name[..name.len() - s.len()].to_string();
        }
    }
    match name.rsplit_once('.') {
        Some((stem, _)) if !stem.is_empty() => stem.to_string(),
        _ => name,
    }
}

/// First of `base`, `base 2`, `base 3`, … (plus `.ext`) that doesn't
/// exist under `parent`.
fn unique_child(parent: &Path, base: &str, ext: Option<&str>) -> PathBuf {
    let with_ext = |stem: &str| match ext {
        Some(e) => format!("{}.{}", stem, e),
        None => stem.to_string(),
    };
    let first = parent.join(with_ext(base));
    if !first.exists() {
        return first;
    }
    for n in 2..10_000u32 {
        let candidate = parent.join(with_ext(&format!("{} {}", base, n)));
        if !candidate.exists() {
            return candidate;
        }
    }
    first
}

fn component_count(p: &Path) -> usize {
    p.components().count()
}

/// Zip lands beside the shallowest selected item (so it can never end up
/// inside a selected folder), named after a lone item or `Archive`.
fn plan_zip_output(inputs: &[String]) -> PathBuf {
    let paths: Vec<PathBuf> = inputs.iter().map(PathBuf::from).collect();
    let shallowest = paths
        .iter()
        .min_by_key(|p| component_count(p))
        .cloned()
        .unwrap_or_default();
    let parent = shallowest
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .map(Path::to_path_buf)
        .unwrap_or_else(|| shallowest.clone());
    let base = if paths.len() == 1 {
        let p = &paths[0];
        let name = p
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| "Archive".into());
        if p.is_dir() {
            name
        } else {
            match name.rsplit_once('.') {
                Some((stem, _)) if !stem.is_empty() => stem.to_string(),
                _ => name,
            }
        }
    } else {
        "Archive".to_string()
    };
    unique_child(&parent, &base, Some("zip"))
}

/// Group inputs by parent folder so each `7z a` pass stores bare names.
/// If two inputs share a basename the flat layout would clobber, so fall
/// back to one pass rooted at the common ancestor with relative paths.
fn plan_compress_groups(inputs: &[String]) -> Result<Vec<CompressGroup>, String> {
    let paths: Vec<PathBuf> = inputs.iter().map(PathBuf::from).collect();
    let mut groups: Vec<CompressGroup> = Vec::new();
    let mut seen_names: Vec<String> = Vec::new();
    let mut collision = false;
    for p in &paths {
        let parent = p
            .parent()
            .filter(|d| !d.as_os_str().is_empty())
            .ok_or_else(|| format!("Cannot zip a root path: {}", p.display()))?
            .to_path_buf();
        let name = p
            .file_name()
            .ok_or_else(|| format!("Cannot zip a root path: {}", p.display()))?
            .to_string_lossy()
            .into_owned();
        let key = if cfg!(windows) { name.to_lowercase() } else { name.clone() };
        if seen_names.contains(&key) {
            collision = true;
        }
        seen_names.push(key);
        match groups.iter_mut().find(|g| g.cwd == parent) {
            Some(g) => g.names.push(name),
            None => groups.push(CompressGroup { cwd: parent, names: vec![name] }),
        }
    }
    if !collision || groups.len() <= 1 {
        if collision {
            // Same folder can't hold two identically named entries, so
            // this is a duplicate selection — dedupe silently.
            for g in &mut groups {
                let mut uniq = Vec::new();
                for n in g.names.drain(..) {
                    if !uniq.contains(&n) {
                        uniq.push(n);
                    }
                }
                g.names = uniq;
            }
        }
        return Ok(groups);
    }
    let ancestor = common_ancestor(&paths)
        .ok_or_else(|| "Selected items don't share a common folder".to_string())?;
    let mut names = Vec::new();
    for p in &paths {
        let rel = p
            .strip_prefix(&ancestor)
            .map_err(|_| format!("{} is outside {}", p.display(), ancestor.display()))?;
        names.push(rel.to_string_lossy().into_owned());
    }
    Ok(vec![CompressGroup { cwd: ancestor, names }])
}

fn common_ancestor(paths: &[PathBuf]) -> Option<PathBuf> {
    let first = paths.first()?;
    let mut prefix: Vec<Component> = first.components().collect();
    for p in &paths[1..] {
        let comps: Vec<Component> = p.components().collect();
        let n = prefix.iter().zip(comps.iter()).take_while(|(a, b)| a == b).count();
        prefix.truncate(n);
    }
    // If one of the paths *is* the prefix (a folder selected together
    // with something inside it), step up once so it is stored by name
    // rather than becoming the cwd.
    if paths.iter().any(|p| p.components().count() == prefix.len()) {
        prefix.pop();
    }
    if prefix.is_empty() {
        return None;
    }
    Some(prefix.iter().collect())
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ProgressTick {
    percent: Option<u8>,
    item: Option<String>,
}

/// Incremental parser for 7-Zip's `-bsp1` stream. Segments are separated
/// by backspaces / CR / LF and look like ` 45% 7 + path/to/file` (the
/// count and `+`/`-` marker are optional, and a segment can be just
/// ` 45%` or the entry name). Only segments terminated by a separator
/// are consumed; the tail stays buffered until the next chunk.
#[derive(Default)]
struct ProgressParser {
    pending: Vec<u8>,
}

impl ProgressParser {
    fn feed(&mut self, chunk: &[u8]) -> Vec<ProgressTick> {
        self.pending.extend_from_slice(chunk);
        let mut ticks = Vec::new();
        let mut start = 0usize;
        for (i, b) in self.pending.iter().enumerate() {
            if matches!(b, b'\x08' | b'\r' | b'\n') {
                if i > start {
                    if let Some(t) = parse_segment(&self.pending[start..i]) {
                        ticks.push(t);
                    }
                }
                start = i + 1;
            }
        }
        self.pending.drain(..start);
        if self.pending.len() > 8192 {
            self.pending.clear();
        }
        ticks
    }
}

fn parse_segment(seg: &[u8]) -> Option<ProgressTick> {
    let s = String::from_utf8_lossy(seg);
    let s = s.trim();
    if s.is_empty() {
        return None;
    }
    let (percent, rest) = match s.split_once('%') {
        Some((num, rest)) if num.trim().chars().all(|c| c.is_ascii_digit()) && !num.trim().is_empty() => {
            (num.trim().parse::<u8>().ok().map(|p| p.min(100)), rest.trim())
        }
        _ => (None, s),
    };
    // rest: "" | "7 + name" | "7 - name" | "name" | "12M Scan" (the
    // size-scan phase prefixes a running byte count instead of a %).
    let item = if rest.is_empty() {
        None
    } else {
        let is_count = |c: &str| {
            let core = c.trim_end_matches('M');
            !core.is_empty() && core.chars().all(|ch| ch.is_ascii_digit())
        };
        let after_count = rest
            .split_once(' ')
            .filter(|(c, _)| is_count(c))
            .map(|(_, r)| r.trim())
            .unwrap_or(rest);
        let name = after_count
            .strip_prefix("+ ")
            .or_else(|| after_count.strip_prefix("- "))
            .or_else(|| after_count.strip_prefix("U "))
            .unwrap_or(after_count)
            .trim();
        if name.is_empty() || is_count(name) {
            None
        } else {
            Some(name.to_string())
        }
    };
    if percent.is_none() && item.is_none() {
        return None;
    }
    Some(ProgressTick { percent, item })
}

/// Map a 7-Zip exit status onto Ok(warning) / Err(message).
/// 0 ok · 1 warning (non-fatal: some entries skipped) · 2 fatal ·
/// 7 bad command line · 8 out of memory · 255 user break.
fn interpret_exit(
    status: Result<std::process::ExitStatus, String>,
    stderr: &str,
) -> Result<Option<String>, String> {
    let status = status?;
    match status.code() {
        Some(0) => Ok(None),
        Some(1) => Ok(Some(summarise_stderr(stderr).unwrap_or_else(|| "7-Zip reported warnings".into()))),
        code => Err(describe_failure(code, stderr)),
    }
}

fn describe_failure(code: Option<i32>, stderr: &str) -> String {
    let lower = stderr.to_lowercase();
    if lower.contains("wrong password") || lower.contains("cannot open encrypted") {
        return "Archive is password-protected (passwords aren't supported yet)".into();
    }
    if lower.contains("is not archive") || lower.contains("can not open the file as archive") {
        return "File is not a readable archive (unsupported or damaged)".into();
    }
    if lower.contains("unexpected end") || lower.contains("data error") || lower.contains("crc failed") {
        return "Archive is damaged or truncated".into();
    }
    if lower.contains("there is not enough space") || lower.contains("no space left") {
        return "Not enough free space on the destination volume".into();
    }
    match (code, summarise_stderr(stderr)) {
        (None, Some(s)) => format!("7-Zip was terminated: {}", s),
        (None, None) => "7-Zip was terminated".into(),
        (Some(c), Some(s)) => format!("7-Zip failed (exit {}): {}", c, s),
        (Some(c), None) => format!("7-Zip failed (exit {})", c),
    }
}

/// Most useful line of 7-Zip's stderr: the first `ERROR:`/`WARNING:`
/// line, else the last non-empty one.
fn summarise_stderr(stderr: &str) -> Option<String> {
    let lines: Vec<&str> = stderr
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .collect();
    lines
        .iter()
        .find(|l| l.starts_with("ERROR:") || l.starts_with("WARNING:"))
        .or_else(|| lines.last())
        .map(|l| l.to_string())
}

async fn read_to_string_capped(stream: Option<tokio::process::ChildStderr>) -> String {
    let Some(mut s) = stream else { return String::new() };
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        match s.read(&mut chunk).await {
            Ok(0) | Err(_) => break,
            Ok(n) => {
                if buf.len() < 64 * 1024 {
                    buf.extend_from_slice(&chunk[..n]);
                }
            }
        }
    }
    String::from_utf8_lossy(&buf).into_owned()
}

/// Remove what a failed / cancelled job created. Guarded by the plan:
/// `created` is always a path that did not exist when the job started.
async fn remove_created(created: &Path) {
    if created.as_os_str().is_empty() || !created.exists() {
        return;
    }
    let r = if created.is_dir() {
        tokio::fs::remove_dir_all(created).await
    } else {
        tokio::fs::remove_file(created).await
    };
    if let Err(e) = r {
        log::warn!("archive: could not remove partial output {}: {}", created.display(), e);
    }
    // 7-Zip stages an updated zip as `<name>.tmp` beside the target.
    let tmp = PathBuf::from(format!("{}.tmp", created.display()));
    if tmp.is_file() {
        let _ = tokio::fs::remove_file(&tmp).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn archive_extension_gate() {
        assert!(is_archive_path("/x/Foo.ZIP"));
        assert!(is_archive_path("C:\\x\\foo.tar.gz"));
        assert!(is_archive_path("/x/foo.rar"));
        assert!(!is_archive_path("/x/foo.mov"));
        assert!(!is_archive_path("/x/.zip"));
        assert!(!is_archive_path("/x/noext"));
    }

    #[test]
    fn stems() {
        assert_eq!(archive_stem(Path::new("/a/foo.zip")), "foo");
        assert_eq!(archive_stem(Path::new("/a/foo.tar.gz")), "foo");
        assert_eq!(archive_stem(Path::new("/a/Foo.TGZ")), "Foo");
        assert_eq!(archive_stem(Path::new("/a/foo.bar.7z")), "foo.bar");
        assert!(is_compound_tar(Path::new("x.tar.xz")));
        assert!(!is_compound_tar(Path::new("x.tar")));
    }

    #[test]
    fn listing_parse_and_root_detection() {
        let text = "Path = src\nFolder = +\n\nPath = src/a.txt\nFolder = -\nSize = 3\n\nPath = src/sub/b.txt\nFolder = -\n";
        let entries = parse_listing(text);
        assert_eq!(entries.len(), 3);
        assert!(entries[0].is_dir);
        assert_eq!(single_root_entry(&entries).as_deref(), Some("src"));

        let flat = parse_listing("Path = a.txt\nFolder = -\n\nPath = b.txt\nFolder = -\n");
        assert_eq!(single_root_entry(&flat), None);

        let lone = parse_listing("Path = report.pdf\nFolder = -\n");
        assert_eq!(single_root_entry(&lone).as_deref(), Some("report.pdf"));

        let dotted = parse_listing("Path = ./src/a\n\nPath = src\\b\n");
        assert_eq!(single_root_entry(&dotted).as_deref(), Some("src"));

        let evil = parse_listing("Path = ../escape\n");
        assert_eq!(single_root_entry(&evil), None);
        assert_eq!(single_root_entry(&[]), None);
    }

    #[test]
    fn progress_segments() {
        assert_eq!(
            parse_segment(b" 45% 7 + src/sub/deep.bin"),
            Some(ProgressTick { percent: Some(45), item: Some("src/sub/deep.bin".into()) })
        );
        assert_eq!(parse_segment(b"  0%"), Some(ProgressTick { percent: Some(0), item: None }));
        assert_eq!(parse_segment(b" 12% 3 - a.txt").unwrap().item.as_deref(), Some("a.txt"));
        assert_eq!(parse_segment(b" 0M Scan"), Some(ProgressTick { percent: None, item: Some("Scan".into()) }));
        assert_eq!(parse_segment(b"   "), None);
        assert_eq!(parse_segment(b"120%").unwrap().percent, Some(100));

        let mut p = ProgressParser::default();
        let mut ticks = p.feed(b" 10% 1 + a\x08\x08\x08 20% 1 + b\x08 3");
        assert_eq!(ticks.len(), 2);
        assert_eq!(ticks[1].percent, Some(20));
        ticks = p.feed(b"0% 2 + c\r");
        assert_eq!(ticks, vec![ProgressTick { percent: Some(30), item: Some("c".into()) }]);
    }

    #[test]
    fn zip_output_naming() {
        let dir = std::env::temp_dir().join(format!("ufb-archive-test-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(dir.join("Renders")).unwrap();
        std::fs::write(dir.join("clip.mov"), b"x").unwrap();
        std::fs::write(dir.join("Renders/clip.mov"), b"x").unwrap();

        let one_file = plan_zip_output(&[dir.join("clip.mov").to_string_lossy().into_owned()]);
        assert_eq!(one_file, dir.join("clip.zip"));
        let one_dir = plan_zip_output(&[dir.join("Renders").to_string_lossy().into_owned()]);
        assert_eq!(one_dir, dir.join("Renders.zip"));
        let many = plan_zip_output(&[
            dir.join("Renders/clip.mov").to_string_lossy().into_owned(),
            dir.join("clip.mov").to_string_lossy().into_owned(),
        ]);
        // Beside the shallowest item, not inside Renders/.
        assert_eq!(many, dir.join("Archive.zip"));

        std::fs::write(dir.join("Archive.zip"), b"x").unwrap();
        assert_eq!(plan_zip_output(&[
            dir.join("clip.mov").to_string_lossy().into_owned(),
            dir.join("Renders").to_string_lossy().into_owned(),
        ]), dir.join("Archive 2.zip"));

        assert_eq!(unique_child(&dir, "Renders", None), dir.join("Renders 2"));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn compress_grouping() {
        let sep = std::path::MAIN_SEPARATOR;
        let a = format!("{0}x{0}proj{0}a.txt", sep);
        let b = format!("{0}x{0}proj{0}b.txt", sep);
        let c = format!("{0}x{0}other{0}c.txt", sep);
        let groups = plan_compress_groups(&[a.clone(), b.clone(), c.clone()]).unwrap();
        assert_eq!(groups.len(), 2);
        assert_eq!(groups[0].names, vec!["a.txt", "b.txt"]);
        assert_eq!(groups[1].names, vec!["c.txt"]);

        // Basename collision across folders → single pass from the common ancestor.
        let a2 = format!("{0}x{0}other{0}a.txt", sep);
        let groups = plan_compress_groups(&[a.clone(), a2]).unwrap();
        assert_eq!(groups.len(), 1);
        assert_eq!(groups[0].cwd, PathBuf::from(format!("{0}x", sep)));
        assert_eq!(groups[0].names[0], format!("proj{0}a.txt", sep));

        // Duplicate selection of the same path dedupes.
        let groups = plan_compress_groups(&[a.clone(), a.clone()]).unwrap();
        assert_eq!(groups[0].names, vec!["a.txt"]);
    }

    #[test]
    fn exit_interpretation() {
        assert_eq!(
            describe_failure(Some(2), "ERROR: Wrong password : src/f1.bin\n"),
            "Archive is password-protected (passwords aren't supported yet)"
        );
        assert!(describe_failure(Some(2), "ERROR: Data Error : x\n").contains("damaged"));
        assert_eq!(describe_failure(Some(7), ""), "7-Zip failed (exit 7)");
        assert_eq!(summarise_stderr("\nSub items Errors: 1\n\nERROR: foo\n").as_deref(), Some("ERROR: foo"));
    }
}

/// End-to-end against a real 7-Zip. Opt in with `UFB_7ZIP=/path/to/7zz`:
/// `UFB_7ZIP=external/7zip/bin/7zz cargo test -p ufb-core --lib archive::live -- --ignored --nocapture`
#[cfg(test)]
mod live {
    use super::*;
    use crate::events::NoopArchiveEvents;

    fn sevenzip() -> Option<PathBuf> {
        std::env::var_os("UFB_7ZIP").map(PathBuf::from).filter(|p| p.is_file())
    }

    async fn wait_final(mgr: &ArchiveManager, id: &str) -> ArchiveJob {
        for _ in 0..600 {
            let q = mgr.get_queue().await;
            let j = q.iter().find(|j| j.id == id).cloned().expect("job vanished");
            if matches!(j.status, JobStatus::Completed | JobStatus::Failed | JobStatus::Cancelled) {
                return j;
            }
            tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        }
        panic!("job {} never finished", id);
    }

    #[tokio::test]
    #[ignore]
    async fn roundtrip_zip_extract_and_compound_tar() {
        let Some(bin) = sevenzip() else { eprintln!("UFB_7ZIP unset; skipping"); return };
        let dir = std::env::temp_dir().join(format!("ufb-archive-live-{}", uuid::Uuid::new_v4()));
        let src = dir.join("proj");
        std::fs::create_dir_all(src.join("sub")).unwrap();
        std::fs::write(src.join("a.txt"), b"alpha").unwrap();
        std::fs::write(src.join("sub").join("b.txt"), b"bravo").unwrap();
        std::fs::write(dir.join("loose.txt"), b"loose").unwrap();
        std::fs::write(dir.join(".DS_Store"), b"junk").unwrap();

        let mgr = Arc::new(ArchiveManager::new(bin.clone(), Arc::new(NoopArchiveEvents)));
        mgr.start_worker();

        // 1. Compress folder + loose file (same parent) → dir/proj.zip? No:
        //    two items → Archive.zip beside them, flat layout.
        let job = mgr
            .add_compress_job(vec![
                src.to_string_lossy().into_owned(),
                dir.join("loose.txt").to_string_lossy().into_owned(),
            ])
            .await
            .unwrap();
        let done = wait_final(&mgr, &job.id).await;
        assert_eq!(done.status, JobStatus::Completed, "{:?}", done.error);
        let zip = PathBuf::from(&done.output_path);
        assert_eq!(zip, dir.join("Archive.zip"));
        assert!(zip.is_file());
        assert_eq!(done.changed_dirs, vec![dir.to_string_lossy().into_owned()]);

        // 2. Extract it: two top-level entries → fresh "Archive/" folder.
        let jobs = mgr.add_extract_jobs(vec![zip.to_string_lossy().into_owned()]).await;
        let done = wait_final(&mgr, &jobs[0].id).await;
        assert_eq!(done.status, JobStatus::Completed, "{:?}", done.error);
        let out = PathBuf::from(&done.output_path);
        assert_eq!(out, dir.join("Archive"));
        assert_eq!(std::fs::read(out.join("proj/sub/b.txt")).unwrap(), b"bravo");
        assert_eq!(std::fs::read(out.join("loose.txt")).unwrap(), b"loose");
        assert!(!out.join(".DS_Store").exists(), ".DS_Store should be excluded");

        // 3. Extract again → collision → "Archive 2".
        let jobs = mgr.add_extract_jobs(vec![zip.to_string_lossy().into_owned()]).await;
        let done = wait_final(&mgr, &jobs[0].id).await;
        assert_eq!(done.status, JobStatus::Completed, "{:?}", done.error);
        assert_eq!(PathBuf::from(&done.output_path), dir.join("Archive 2"));

        // 4. Single-folder zip: proj.zip → extracted straight into a
        //    sibling "proj" — but proj exists, so "proj 2/proj/…".
        let job = mgr.add_compress_job(vec![src.to_string_lossy().into_owned()]).await.unwrap();
        let done = wait_final(&mgr, &job.id).await;
        assert_eq!(done.status, JobStatus::Completed, "{:?}", done.error);
        let pzip = PathBuf::from(&done.output_path);
        assert_eq!(pzip, dir.join("proj.zip"));
        let jobs = mgr.add_extract_jobs(vec![pzip.to_string_lossy().into_owned()]).await;
        let done = wait_final(&mgr, &jobs[0].id).await;
        assert_eq!(done.status, JobStatus::Completed, "{:?}", done.error);
        assert_eq!(PathBuf::from(&done.output_path), dir.join("proj 2"));
        assert!(dir.join("proj 2/proj/a.txt").is_file());
        // …and with the original moved away, straight into the parent.
        std::fs::rename(&src, dir.join("proj-moved")).unwrap();
        let jobs = mgr.add_extract_jobs(vec![pzip.to_string_lossy().into_owned()]).await;
        let done = wait_final(&mgr, &jobs[0].id).await;
        assert_eq!(done.status, JobStatus::Completed, "{:?}", done.error);
        assert_eq!(PathBuf::from(&done.output_path), dir.join("proj"));
        assert!(dir.join("proj/sub/b.txt").is_file());

        // 5. Compound tar (.tar.gz made by 7-Zip itself) → pipeline path.
        let tar = dir.join("bundle.tar");
        let st = std::process::Command::new(&bin)
            .current_dir(&dir)
            .args(["a", "-ttar", "-bso0", "-bse0", "bundle.tar", "proj-moved"])
            .status().unwrap();
        assert!(st.success());
        let st = std::process::Command::new(&bin)
            .current_dir(&dir)
            .args(["a", "-tgzip", "-bso0", "-bse0", "bundle.tar.gz", "bundle.tar"])
            .status().unwrap();
        assert!(st.success());
        std::fs::remove_file(&tar).unwrap();
        let jobs = mgr.add_extract_jobs(vec![dir.join("bundle.tar.gz").to_string_lossy().into_owned()]).await;
        let done = wait_final(&mgr, &jobs[0].id).await;
        assert_eq!(done.status, JobStatus::Completed, "{:?}", done.error);
        // single root "proj-moved" already exists → "bundle/proj-moved/…"
        assert_eq!(PathBuf::from(&done.output_path), dir.join("bundle"));
        assert!(dir.join("bundle/proj-moved/a.txt").is_file());

        // 6. Password-protected → readable failure, nothing left behind.
        let st = std::process::Command::new(&bin)
            .current_dir(&dir)
            .args(["a", "-tzip", "-pSECRET", "-bso0", "-bse0", "locked.zip", "loose.txt"])
            .status().unwrap();
        assert!(st.success());
        let jobs = mgr.add_extract_jobs(vec![dir.join("locked.zip").to_string_lossy().into_owned()]).await;
        let done = wait_final(&mgr, &jobs[0].id).await;
        assert_eq!(done.status, JobStatus::Failed);
        assert!(done.error.as_deref().unwrap_or("").contains("password"), "{:?}", done.error);
        assert!(!dir.join("locked").exists());

        // 7. Garbage with a .zip name → failure, nothing left behind.
        std::fs::write(dir.join("bogus.zip"), b"not a zip at all").unwrap();
        let jobs = mgr.add_extract_jobs(vec![dir.join("bogus.zip").to_string_lossy().into_owned()]).await;
        let done = wait_final(&mgr, &jobs[0].id).await;
        assert_eq!(done.status, JobStatus::Failed, "{:?}", done);
        assert!(!dir.join("bogus").exists());

        // 8. Cancel mid-compress of something big enough to take a moment.
        let big = dir.join("big");
        std::fs::create_dir_all(&big).unwrap();
        let mut blob = vec![0u8; 40 * 1024 * 1024];
        for (i, b) in blob.iter_mut().enumerate() { *b = (i * 2654435761usize >> 13) as u8; }
        for i in 0..4 { std::fs::write(big.join(format!("blob{}.bin", i)), &blob).unwrap(); }
        let job = mgr.add_compress_job(vec![big.to_string_lossy().into_owned()]).await.unwrap();
        tokio::time::sleep(std::time::Duration::from_millis(250)).await;
        mgr.cancel_job(&job.id).await;
        let done = wait_final(&mgr, &job.id).await;
        assert_eq!(done.status, JobStatus::Cancelled);
        tokio::time::sleep(std::time::Duration::from_millis(300)).await;
        assert!(!dir.join("big.zip").exists(), "partial zip should be removed");

        std::fs::remove_dir_all(&dir).unwrap();
    }
}
