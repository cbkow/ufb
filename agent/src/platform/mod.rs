#[cfg(windows)]
pub mod windows;

#[cfg(target_os = "macos")]
pub mod macos;

/// Trait for managing mount-point mappings.
/// Windows impl deleted in slice B (mounts are drive letters, no
/// link layer); survives for the macOS/Linux arms.
#[cfg_attr(windows, allow(dead_code))]
pub trait DriveMapping: Send + Sync {
    fn switch(&self, mount_point: &str, target_path: &str) -> Result<(), String>;
    fn read_target(&self, mount_point: &str) -> Result<String, String>;
    fn remove(&self, mount_point: &str) -> Result<(), String>;
    fn verify(&self, mount_point: &str, expected_target: &str) -> Result<bool, String>;
}

/// Trait for establishing SMB sessions (Linux arms only).
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
pub trait SmbSession: Send + Sync {
    /// Ensure an authenticated SMB session exists for the given share.
    fn ensure_session(
        &self,
        share_path: &str,
        mount_point: &str,
        username: &str,
        password: &str,
    ) -> Result<(), String>;
}

/// Trait for credential storage. Windows impl deleted in slice B
/// (credentials are OS-owned); survives for macOS/Linux stores.
#[cfg_attr(windows, allow(dead_code))]
pub trait CredentialStore: Send + Sync {
    fn store(&self, key: &str, username: &str, password: &str) -> Result<(), String>;
    fn retrieve(&self, key: &str) -> Result<(String, String), String>;
    fn delete(&self, key: &str) -> Result<(), String>;
}

// set_auto_start / is_auto_start_enabled (both OS variants) deleted
// 2026-07-11: nothing called them. Autostart is owned by the installer
// Run key (Windows, `ufb --background`) and SMAppService (macOS, slice
// F2); the agent never manages its own login registration again.

