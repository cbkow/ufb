pub mod fallback;
pub(crate) mod netfs;
// credentials / smb_probe / mountpoint deleted 1.0.7: OS owns
// credentials (slice C), TestCredentials is a wire tombstone, and
// the plain-mount symlink layer retired with GUI-owned mounts (F1).

pub use fallback::find_existing_volume;
pub use fallback::macos_smb_mount;
pub use fallback::macos_smb_unmount;
pub use fallback::mount_at_path_is_ours;
pub use fallback::stale_dir_blocking;
pub use fallback::MacosMountError;

/// macOS App Group identifier shared by ufb-agent (this binary), the Qt
/// main app (UFB.app), the Swift menu-bar tray (UFBTray.app), and the
/// Finder Sync extension (UFBFinderSync.appex). The Group Container
/// directory under `~/Library/Group Containers/<APP_GROUP_ID>/` is
/// where the IPC sockets (`a.sock`, `ufb-app.sock`) live so the
/// sandboxed FinderSync extension can reach them.
///
/// Renamed from the legacy `…unionfiles.mediamount-tray` Tauri-era group
/// in macOSplans/01. The team-id prefix `5Z4S9VHV56` must match the
/// Apple Developer Team that signs all four bundles.
///
/// **MUST stay in lock-step with**:
///   - `core/src/mount_client.rs::agent_socket_path()`
///   - the entitlements files under `macos-helpers/UFBTray/`
///     and `macos-helpers/UFBFinderSync/`
///   - `app/UFB.entitlements` and the future `installer/macos/`
///     ufb-agent entitlements
#[cfg(target_os = "macos")]
pub const APP_GROUP_ID: &str = "5Z4S9VHV56.group.com.unionfiles.ufb";

/// Credential key prefix the agent and Qt app both use when reading /
/// writing macOS Keychain (or Windows Credential Manager) entries.
/// Bare credential keys in `mounts.json` (e.g. `nas-prod-creds`) get
/// prefixed to e.g. `ufb_nas-prod-creds` for the actual keystore
/// service name. Renamed from the legacy `mediamount_` prefix in
/// macOSplans/01.
///
/// **MUST stay in lock-step with**:
///   - `agent/src/orchestrator.rs::Orchestrator::CRED_PREFIX`
///   - `agent/src/mount_service.rs::CRED_PREFIX`
///   - `app/CredentialPrompt.cpp::_prefixedTarget`
///
/// (The duplication is intentional — `core/` and `app/` aren't in this
/// crate. Search for `CRED_PREFIX` if you change this and update every
/// site.)
pub const CRED_PREFIX: &str = "ufb_";
