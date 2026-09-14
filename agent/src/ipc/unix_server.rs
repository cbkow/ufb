use crate::messages::{AgentToUfb, UfbToAgent};
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex};

/// Resolve the socket path for the IPC server.
///
/// On macOS we put it inside the shared App Group container so the
/// sandboxed FinderSync extension can reach it — sandboxed processes
/// can't open sockets in `/tmp`. The tray and main app are not
/// sandboxed but use the same path for consistency. The Group ID is
/// centralised in `crate::platform::macos::APP_GROUP_ID`.
///
/// **Filename is intentionally short.** `sockaddr_un.sun_path` is 104
/// bytes on macOS. The Group Container path is long enough on its own
/// that a verbose socket filename pushes the total over the limit and
/// `bind(2)` rejects it with `EINVAL`/`SUN_LEN`. `a.sock` keeps the
/// full path comfortably under 100 bytes for reasonable home directory
/// lengths.
///
/// Linux / other platforms keep the XDG_RUNTIME_DIR → /tmp fallback.
fn socket_path() -> std::path::PathBuf {
    #[cfg(target_os = "macos")]
    {
        if let Some(home) = std::env::var_os("HOME") {
            let dir = std::path::PathBuf::from(home)
                .join("Library/Group Containers")
                .join(crate::platform::macos::APP_GROUP_ID);
            let _ = std::fs::create_dir_all(&dir);
            return dir.join("a.sock");
        }
    }
    if let Ok(runtime_dir) = std::env::var("XDG_RUNTIME_DIR") {
        let dir = std::path::PathBuf::from(runtime_dir).join("ufb");
        let _ = std::fs::create_dir_all(&dir);
        dir.join("ufb-agent.sock")
    } else {
        std::path::PathBuf::from("/tmp/ufb-agent.sock")
    }
}

/// Get the socket path (for use by the client side too).
pub fn get_socket_path() -> std::path::PathBuf {
    socket_path()
}

/// Per-client write budget. The broadcast writer runs every client's
/// `send_message` back-to-back on one task; a client that stops
/// draining its socket (GUI hung on the main thread, debugger attached)
/// would otherwise park that task in `write_all` forever and, through
/// the bounded response channel, back-pressure the agent's main loop
/// (audit 2026-09-11 P2). A client that can't take a state update in
/// this long is dropped — it reconnects and asks for a fresh snapshot.
const CLIENT_WRITE_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

/// IPC server that listens for connections on a Unix domain socket.
/// Supports multiple concurrent clients (e.g. UFB + Swift tray app).
pub struct IpcServer {
    pub command_rx: mpsc::Receiver<UfbToAgent>,
    response_tx: mpsc::Sender<AgentToUfb>,
    _cancel_tx: tokio::sync::oneshot::Sender<()>,
}

impl IpcServer {
    pub fn start() -> Self {
        let (cmd_tx, cmd_rx) = mpsc::channel::<UfbToAgent>(64);
        // 1024: one-shot replies (Ack, CacheStats, snapshots) must not
        // be lost behind a burst of per-file badge updates (review
        // 2026-09-11 #3); the writer drains one message per
        // spawn_blocking round trip.
        let (resp_tx, mut resp_rx) = mpsc::channel::<AgentToUfb>(1024);
        let (cancel_tx, _cancel_rx) = tokio::sync::oneshot::channel::<()>();

        // Shared list of connected client write streams
        let writers: Arc<Mutex<Vec<UnixStream>>> = Arc::new(Mutex::new(Vec::new()));

        // Response writer task — broadcasts to all connected clients
        let writer_handle = writers.clone();
        tokio::spawn(async move {
            while let Some(msg) = resp_rx.recv().await {
                // The writes are blocking socket I/O (bounded by
                // CLIENT_WRITE_TIMEOUT per client) — keep them off the
                // async workers so a slow client can't stall the
                // runtime the orchestrators share.
                let writers = Arc::clone(&writer_handle);
                let _ = tokio::task::spawn_blocking(move || {
                    let mut lock = writers.blocking_lock();
                    let mut failed = Vec::new();
                    for (i, stream) in lock.iter_mut().enumerate() {
                        if let Err(e) = super::send_message(stream, &msg) {
                            let slow = matches!(
                                e.kind(),
                                std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                            );
                            if slow {
                                log::warn!(
                                    "IPC client {} not draining (write timed out) — dropping it",
                                    i
                                );
                            } else {
                                log::debug!("Failed to send to client {}: {}", i, e);
                            }
                            failed.push(i);
                        }
                    }
                    // Remove disconnected clients (reverse order to preserve indices)
                    for i in failed.into_iter().rev() {
                        log::info!("Removing disconnected client {}", i);
                        lock.remove(i);
                    }
                })
                .await;
            }
        });

        // Connection listener task
        let listener_handle = writers;
        tokio::spawn(async move {
            let sock_path = socket_path();

            // Clean up stale socket
            if sock_path.exists() {
                let _ = std::fs::remove_file(&sock_path);
            }

            let listener = match UnixListener::bind(&sock_path) {
                Ok(l) => {
                    log::info!("IPC listening on {}", sock_path.display());
                    l
                }
                Err(e) => {
                    log::error!("Failed to bind Unix socket at {}: {}", sock_path.display(), e);
                    return;
                }
            };

            // Set permissions so only current user can connect
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let _ = std::fs::set_permissions(&sock_path, std::fs::Permissions::from_mode(0o700));
            }

            loop {
                // Accept connection (blocking)
                let (stream, _addr) = match tokio::task::spawn_blocking({
                    let listener_fd = listener.try_clone().expect("Failed to clone listener");
                    move || listener_fd.accept()
                })
                .await
                {
                    Ok(Ok(pair)) => pair,
                    Ok(Err(e)) => {
                        log::error!("Accept failed: {}", e);
                        tokio::time::sleep(std::time::Duration::from_secs(1)).await;
                        continue;
                    }
                    Err(e) => {
                        log::error!("Accept task panicked: {}", e);
                        break;
                    }
                };

                log::info!("IPC client connected");

                // Clone stream for writing
                let write_stream = match stream.try_clone() {
                    Ok(s) => s,
                    Err(e) => {
                        log::error!("Failed to clone stream: {}", e);
                        continue;
                    }
                };
                if let Err(e) = write_stream.set_write_timeout(Some(CLIENT_WRITE_TIMEOUT)) {
                    log::warn!("IPC: could not set client write timeout: {}", e);
                }

                // Add write half to client list
                {
                    let mut lock = listener_handle.lock().await;
                    lock.push(write_stream);
                }

                // Read commands in blocking thread
                let cmd_tx_clone = cmd_tx.clone();
                tokio::task::spawn_blocking(move || {
                    let mut reader = stream;
                    loop {
                        match super::recv_message::<_, UfbToAgent>(&mut reader) {
                            Ok(msg) => {
                                if cmd_tx_clone.blocking_send(msg).is_err() {
                                    break;
                                }
                            }
                            Err(_) => {
                                log::info!("IPC client disconnected");
                                break;
                            }
                        }
                    }
                });
            }
        });

        Self {
            command_rx: cmd_rx,
            response_tx: resp_tx,
            _cancel_tx: cancel_tx,
        }
    }

    /// Queue a message for every connected client. Reliable (awaited):
    /// one-shot replies — Ack, Error, CacheStats, TestCredentialsResult,
    /// MountStateSnapshot, Pong, ConflictDetected — and state
    /// transitions must reach the GUI or its 10s command timeout renders
    /// success as failure (review 2026-09-11 #3). The channel is 1024
    /// deep and a wedged client is evicted by the per-client write
    /// timeout, so this only ever waits briefly.
    pub async fn send(&self, msg: AgentToUfb) -> Result<(), String> {
        self.response_tx
            .send(msg)
            .await
            .map_err(|e| format!("Failed to queue response: {}", e))
    }

    /// Non-blocking variant for high-frequency, idempotent traffic
    /// (per-file BadgeUpdate): dropped when the channel is full — the
    /// next update supersedes it — so a burst can never back-pressure
    /// the agent's main loop (audit 2026-09-11 P2).
    pub fn send_droppable(&self, msg: AgentToUfb) -> Result<(), String> {
        self.response_tx
            .try_send(msg)
            .map_err(|e| format!("Failed to queue response: {}", e))
    }
}
