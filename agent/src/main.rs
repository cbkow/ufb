#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod config;
mod ipc;
mod messages;
mod mount_service;
mod orchestrator;
mod platform;
mod state;
mod sync;

use std::process;

// ── Single-instance mutex ──

#[cfg(windows)]
struct MutexGuard {
    _handle: windows::Win32::Foundation::HANDLE,
}

#[cfg(windows)]
impl Drop for MutexGuard {
    fn drop(&mut self) {
        if !self._handle.is_invalid() {
            unsafe {
                let _ = windows::Win32::Foundation::CloseHandle(self._handle);
            }
        }
    }
}

#[cfg(unix)]
struct MutexGuard {
    _lock_file: std::fs::File,
}

#[cfg(not(any(windows, unix)))]
struct MutexGuard;

#[cfg(windows)]
fn ensure_single_instance() -> MutexGuard {
    use windows::core::PCWSTR;
    use windows::Win32::Foundation::{GetLastError, ERROR_ALREADY_EXISTS};
    use windows::Win32::System::Threading::CreateMutexW;

    let mutex_name: Vec<u16> = "UfbAgent\0".encode_utf16().collect();

    let handle = unsafe { CreateMutexW(None, false, PCWSTR(mutex_name.as_ptr())) };
    match handle {
        Ok(h) => {
            if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
                log::error!("Another ufb-agent is already running");
                process::exit(1);
            }
            MutexGuard { _handle: h }
        }
        Err(e) => {
            log::error!("Failed to create instance mutex: {}", e);
            process::exit(1);
        }
    }
}

#[cfg(unix)]
fn ensure_single_instance() -> MutexGuard {
    use std::os::unix::io::AsRawFd;

    let lock_dir = if let Ok(runtime_dir) = std::env::var("XDG_RUNTIME_DIR") {
        let dir = std::path::PathBuf::from(runtime_dir).join("ufb");
        let _ = std::fs::create_dir_all(&dir);
        dir
    } else {
        std::path::PathBuf::from("/tmp")
    };

    let lock_path = lock_dir.join("ufb-agent.lock");
    let lock_file = std::fs::OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(false)
        .open(&lock_path)
        .unwrap_or_else(|e| {
            eprintln!("Failed to create lock file {}: {}", lock_path.display(), e);
            process::exit(1);
        });

    let fd = lock_file.as_raw_fd();
    let result = unsafe { libc::flock(fd, libc::LOCK_EX | libc::LOCK_NB) };
    if result != 0 {
        log::error!("Another ufb-agent is already running");
        process::exit(1);
    }

    MutexGuard { _lock_file: lock_file }
}

#[cfg(not(any(windows, unix)))]
fn ensure_single_instance() -> MutexGuard {
    MutexGuard
}

// ── PID file ──

/// Where the agent records its process ID for heal-on-open. Both
/// platforms put it next to settings.json so users find it where
/// they expect application state to live.
///
/// Heal-on-open path (Qt app side, `bindings/src/services/mount.rs`):
///   1. Read this file. If the PID is alive, kill it (-9).
///   2. Try `launchctl kickstart` (macOS) / `CreateProcess` (Win).
///   3. Wait for the new agent to write a fresh PID + accept IPC.
///
/// Per macOSplans/03.
fn pid_file_path() -> Option<std::path::PathBuf> {
    #[cfg(windows)]
    {
        if let Ok(local) = std::env::var("LOCALAPPDATA") {
            let dir = std::path::PathBuf::from(local).join("ufb");
            let _ = std::fs::create_dir_all(&dir);
            return Some(dir.join("agent.pid"));
        }
    }
    #[cfg(target_os = "macos")]
    {
        if let Some(home) = std::env::var_os("HOME") {
            let dir = std::path::PathBuf::from(home)
                .join("Library/Application Support/ufb");
            let _ = std::fs::create_dir_all(&dir);
            return Some(dir.join("agent.pid"));
        }
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(home) = std::env::var_os("HOME") {
            let dir = std::path::PathBuf::from(home).join(".local/share/ufb");
            let _ = std::fs::create_dir_all(&dir);
            return Some(dir.join("agent.pid"));
        }
    }
    None
}

/// RAII guard that writes the PID file at construction and removes it
/// on Drop. Crash / SIGKILL leaves it behind; heal-on-open detects the
/// stale file (PID not alive) and overwrites.
struct PidFileGuard {
    path: std::path::PathBuf,
}

impl PidFileGuard {
    fn install() -> Option<Self> {
        let path = pid_file_path()?;
        let pid = std::process::id();
        match std::fs::write(&path, pid.to_string()) {
            Ok(()) => {
                log::info!("Wrote PID file {} (pid {})", path.display(), pid);
                Some(Self { path })
            }
            Err(e) => {
                log::warn!(
                    "Could not write PID file {}: {}",
                    path.display(),
                    e
                );
                None
            }
        }
    }
}

impl Drop for PidFileGuard {
    fn drop(&mut self) {
        if let Err(e) = std::fs::remove_file(&self.path) {
            log::debug!(
                "PID file cleanup failed (non-fatal): {} ({})",
                self.path.display(),
                e
            );
        } else {
            log::debug!("Removed PID file {}", self.path.display());
        }
    }
}

// ── Logging ──

fn log_file_path() -> Option<std::path::PathBuf> {
    #[cfg(windows)]
    {
        if let Ok(local) = std::env::var("LOCALAPPDATA") {
            let dir = std::path::PathBuf::from(local).join("ufb");
            let _ = std::fs::create_dir_all(&dir);
            return Some(dir.join("ufb-agent.log"));
        }
    }
    #[cfg(target_os = "macos")]
    {
        // ~/Library/Logs/ufb/ufb-agent.log per macOSplans/01 (replaces
        // the legacy ~/.local/share/ufb/mediamount-agent.log location).
        // The LaunchAgent plist also writes stdout/stderr alongside —
        // see agent/src/platform/mod.rs::set_auto_start.
        if let Some(home) = std::env::var_os("HOME") {
            let dir = std::path::PathBuf::from(home).join("Library/Logs/ufb");
            let _ = std::fs::create_dir_all(&dir);
            return Some(dir.join("ufb-agent.log"));
        }
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(home) = std::env::var_os("HOME") {
            let dir = std::path::PathBuf::from(home).join(".local/share/ufb");
            let _ = std::fs::create_dir_all(&dir);
            return Some(dir.join("ufb-agent.log"));
        }
    }
    None
}

fn init_logging() {
    use simplelog::*;

    // Honor RUST_LOG=debug (or UFB_DEBUG=1) for verbose diagnostics during
    // development. Default is Info.
    let level = match std::env::var("RUST_LOG")
        .ok()
        .as_deref()
        .map(|s| s.to_lowercase())
        .as_deref()
    {
        Some(s) if s.contains("debug") => LevelFilter::Debug,
        Some(s) if s.contains("trace") => LevelFilter::Trace,
        Some(s) if s.contains("warn") => LevelFilter::Warn,
        _ if std::env::var("UFB_DEBUG").ok().as_deref() == Some("1") => LevelFilter::Debug,
        _ => LevelFilter::Info,
    };
    let config = ConfigBuilder::new().set_time_format_rfc3339().build();

    let mut loggers: Vec<Box<dyn SharedLogger>> = vec![TermLogger::new(
        level,
        config.clone(),
        TerminalMode::Stderr,
        ColorChoice::Auto,
    )];

    if let Some(path) = log_file_path() {
        // Truncate if log is > 2 MB
        if let Ok(meta) = std::fs::metadata(&path) {
            if meta.len() > 2 * 1024 * 1024 {
                let _ = std::fs::remove_file(&path);
            }
        }
        match std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
        {
            Ok(file) => {
                loggers.push(WriteLogger::new(level, config.clone(), file));
                eprintln!(
                    "[ufb-agent] Logging to {}",
                    path.display()
                );
            }
            Err(e) => {
                eprintln!(
                    "[ufb-agent] Warning: could not open log file {}: {}",
                    path.display(),
                    e
                );
            }
        }
    }

    CombinedLogger::init(loggers).unwrap_or_else(|e| {
        eprintln!("[ufb-agent] Failed to init logger: {}", e);
    });
}

fn install_panic_hook() {
    let default_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let location = info
            .location()
            .map(|l| format!("{}:{}:{}", l.file(), l.line(), l.column()))
            .unwrap_or_else(|| "unknown".to_string());
        let payload = if let Some(s) = info.payload().downcast_ref::<&str>() {
            s.to_string()
        } else if let Some(s) = info.payload().downcast_ref::<String>() {
            s.clone()
        } else {
            "unknown panic payload".to_string()
        };
        log::error!("PANIC at {}: {}", location, payload);

        if let Some(path) = log_file_path() {
            let msg = format!("[PANIC] {} at {}\n", payload, location);
            let _ = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(&path)
                .and_then(|mut f| {
                    use std::io::Write;
                    f.write_all(msg.as_bytes())
                });
        }

        default_hook(info);
    }));
}

// ── Main ──

/// The async event loop — runs on the main thread (Windows/Linux) or a background thread (macOS).
async fn run_event_loop() {
    // Start IPC server
    #[cfg(windows)]
    let mut ipc_server = ipc::server::IpcServer::start();

    #[cfg(unix)]
    let mut ipc_server = ipc::unix_server::IpcServer::start();


    #[cfg(not(any(windows, unix)))]
    {
        log::error!("IPC server not implemented for this platform");
        process::exit(1);
    }

    #[cfg(any(windows, unix))]
    {
        // Channel for agent→UFB messages from mount orchestrators
        let (state_tx, mut state_rx) = tokio::sync::mpsc::channel::<messages::AgentToUfb>(128);

        // Shared config cache — loaded once at startup, refreshed when the
        // config file changes (watcher below). Reloaded into the NFS server
        // + mount service on config change.
        let config_cache: std::sync::Arc<std::sync::RwLock<config::MountsConfig>> =
            std::sync::Arc::new(std::sync::RwLock::new(config::load_config()));

        // Shared per-domain cache map — populated on VFS server startup,
        // queried by mount_service for UI drain/stats.
        #[cfg(any(target_os = "macos", windows))]
        let shared_caches: sync::SharedCaches = std::sync::Arc::new(
            std::sync::RwLock::new(std::collections::HashMap::new()),
        );

        // The agent's own tray is gone (plans/17 slice E/F2): the GUI's
        // in-app tray is the only user-facing surface; the agent is a
        // faceless sync VFS host.

        // Start mount service. Orchestrators own their own sync server
        // handles after Slice B — no separate SyncServersRegistry, no
        // spawner closures here, no respawn channel. Each orchestrator
        // spawns / tears down its VFS server via Effect::SpawnSyncServer
        // / TeardownSyncServer in its FSM.
        let mut mount_service = mount_service::MountService::new(state_tx);
        #[cfg(any(target_os = "macos", windows))]
        mount_service.set_shared_caches(std::sync::Arc::clone(&shared_caches));

        mount_service.start_from_config().await;

        // Config file watcher — polls mtime every 5 seconds
        let (config_reload_tx, mut config_reload_rx) = tokio::sync::mpsc::channel::<()>(1);
        if let Some(config_path) = config::config_file_path() {
            tokio::spawn(async move {
                let mut last_mtime = std::fs::metadata(&config_path)
                    .and_then(|m| m.modified())
                    .ok();

                loop {
                    tokio::time::sleep(std::time::Duration::from_secs(5)).await;

                    let current_mtime = std::fs::metadata(&config_path)
                        .and_then(|m| m.modified())
                        .ok();

                    if current_mtime != last_mtime && current_mtime.is_some() {
                        last_mtime = current_mtime;
                        log::info!("Config file changed, triggering reload");
                        if config_reload_tx.send(()).await.is_err() {
                            break;
                        }
                    }
                }
            });
        }

        log::info!("ufb-agent ready");

        // SIGTERM / SIGINT shutdown channel. tokio::select! arms can't
        // be `#[cfg]`-gated, so we spawn signal listeners into a task
        // that funnels both into a single channel the main loop polls.
        // Without this, launchctl bootout (which sends SIGTERM, not
        // SIGINT) would kill the agent abruptly and leave dead NFS
        // loopback mounts behind that hang Finder.
        let (shutdown_tx, mut shutdown_rx) = tokio::sync::mpsc::channel::<&'static str>(2);
        {
            let tx = shutdown_tx.clone();
            tokio::spawn(async move {
                let _ = tokio::signal::ctrl_c().await;
                let _ = tx.send("SIGINT").await;
            });
        }
        #[cfg(unix)]
        {
            let tx = shutdown_tx.clone();
            tokio::spawn(async move {
                if let Ok(mut s) = tokio::signal::unix::signal(
                    tokio::signal::unix::SignalKind::terminate(),
                ) {
                    s.recv().await;
                    let _ = tx.send("SIGTERM").await;
                }
            });
        }

        // Main event loop
        loop {
            tokio::select! {
                // Commands from UFB via IPC
                Some(cmd) = ipc_server.command_rx.recv() => {
                    log::debug!("IPC command received: {:?}", cmd);
                    mount_service.handle_command(cmd).await;
                }

                // Outgoing state updates to forward to UFB
                Some(msg) = state_rx.recv() => {
                    log::debug!("Forwarding to UFB: {:?}", msg);
                    if let Err(e) = ipc_server.send(msg).await {
                        log::warn!("Failed to forward to UFB: {}", e);
                    }
                }

                // Config file changed on disk
                Some(()) = config_reload_rx.recv() => {
                    *config_cache.write().unwrap() = config::load_config();
                    mount_service.reload_config().await;
                }

                // Shutdown signal (SIGINT or SIGTERM). The funneling
                // tasks above name the actual signal in the channel.
                Some(sig) = shutdown_rx.recv() => {
                    log::info!("{} received", sig);
                    // Slice G: per-orchestrator SyncServerHandle owns
                    // the NFS unmount; mount_service.shutdown drains
                    // every handle before returning, so the explicit
                    // unmount_all() helper was redundant.
                    mount_service.shutdown().await;
                    break;
                }
            }
        }
    }

    log::info!("ufb-agent exiting");
    process::exit(0);
}

/// macOS: headless agent — tray UI handled by companion Swift MenuBarExtra app.
/// The Swift app communicates with this agent via the same Unix socket IPC.
#[cfg(target_os = "macos")]
#[tokio::main]
async fn main() {
    init_logging();
    install_panic_hook();

    log::info!(
        "ufb-agent v{} starting (headless — tray via companion app)",
        env!("CARGO_PKG_VERSION")
    );

    // Single-instance guard MUST come before the stale-mount cleanup:
    // cleanup force-unmounts every localhost NFS mount under the mount
    // root, and a second agent launch that runs it before losing the
    // single-instance race would rip the LIVE mounts out from under the
    // healthy first instance.
    let _mutex_guard = ensure_single_instance();

    // Force-unmount any leftover localhost: NFS mounts under
    // ~/ufb/mounts/ from a crashed previous agent BEFORE anything
    // else stats the filesystem. A dead loopback NFS mount will
    // hang Finder / Spotlight / mds / `ls` indefinitely as soon as
    // anything tries to enumerate the mount point — the agent's
    // own mid-startup cleanup runs too late, after orchestrator
    // setup has already given the OS time to wander in. `umount -f`
    // does not block on the dead server, so this is safe even with
    // a fully-hung mount.
    sync::nfs_server::cleanup_stale_mounts_on_startup();
    let _pid_guard = PidFileGuard::install();
    ensure_macos_mount_dir();

    // (The credentials.json → Keychain migration ran in 1.0.5/1.0.6
    // on every fleet mac and was deleted in 1.0.7 with the whole
    // macOS credential store — the OS owns SMB credentials now.)

    run_event_loop().await;
}

/// Windows/Linux: tokio runs on the main thread, tray on a spawned thread.
#[cfg(not(target_os = "macos"))]
#[tokio::main]
async fn main() {
    // --prime-smartscreen runs at install time so Windows finishes its
    // first-run reputation check on this EXE before the user ever
    // launches it. Without the prime, the first traversal of any
    // junction/symlink/WinFsp mount from this process returns
    // ERROR_UNTRUSTED_MOUNT_POINT (448) until SmartScreen completes —
    // the retry in core/src/file_ops::read_dir_with_448_retry handles
    // it but its budget is finite. Stay alive a few seconds with no
    // side effects, then exit. Must come BEFORE init_logging so we
    // don't pollute the log file.
    #[cfg(windows)]
    if std::env::args().any(|a| a == "--prime-smartscreen") {
        std::thread::sleep(std::time::Duration::from_secs(3));
        return;
    }

    init_logging();
    install_panic_hook();

    // --create-symlinks (the elevated symlink worker) is gone — slice B
    // deleted the C:\Volumes\ufb symlink layer. An old GUI invoking it
    // just gets a normal agent start, which is harmless.

    log::info!(
        "ufb-agent v{} starting",
        env!("CARGO_PKG_VERSION")
    );

    let _mutex_guard = ensure_single_instance();
    let _pid_guard = PidFileGuard::install();

    run_event_loop().await;
}

/// macOS: ensure the user-facing mount directories exist.
///
/// Both live under user-owned paths (no admin required):
/// - `~/ufb/mounts/` — user-facing symlinks to actual mount points
/// - `~/.local/share/ufb/smb-mounts/` — private mountpoints for `mount_smbfs` targets
///
/// Legacy `/opt/ufb/mounts/` is left in place if present (harmless on upgrade);
/// future installs will never need admin privileges again.
#[cfg(target_os = "macos")]
fn ensure_macos_mount_dir() {
    let volumes_base = crate::config::MountConfig::volumes_base();
    if let Err(e) = std::fs::create_dir_all(&volumes_base) {
        log::error!(
            "Failed to create {}: {}",
            volumes_base.display(),
            e
        );
    }

    let smb_base = crate::config::MountConfig::smb_mount_base();
    if let Err(e) = std::fs::create_dir_all(&smb_base) {
        log::error!("Failed to create {}: {}", smb_base.display(), e);
    }
}

// `open_ufb` / `open_log` deleted with the agent tray (slice E/F2) —
// they were tray-menu actions; the in-app tray owns those affordances.

// `resolve_mount_root_for_nfs` removed in Slice B — the orchestrator
// captures the resolved mount path directly from `macos_smb_mount`'s
// return value into `self.mounted_at`, eliminating the bounded-poll
// race that this helper used to bridge.
