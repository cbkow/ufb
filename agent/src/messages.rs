use serde::{Deserialize, Serialize};

// ── Agent → UFB ──

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum AgentToUfb {
    MountStateUpdate(MountStateUpdateMsg),
    /// Full per-mount state snapshot. Sent on (a) every new IPC client
    /// connection (so the UI starts with a complete picture rather than
    /// waiting for per-mount updates to drip in), and (b) when a mount
    /// is removed or its orchestrator exits unexpectedly (so clients
    /// can prune stale entries from their local mounts map). UIs
    /// replace their entire mounts map on receipt.
    MountStateSnapshot(MountStateSnapshotMsg),
    Ack(AckMsg),
    Error(ErrorMsg),
    Pong,
    ConflictDetected(ConflictDetectedMsg),
    /// Response to `UfbToAgent::GetCacheStats`. Zero values are emitted
    /// for mounts that have no cache (plain SMB on macOS, Windows without
    /// sync) so the frontend can treat the message as authoritative.
    CacheStats(CacheStatsMsg),
    /// Hydration state changed for a file. Consumed by the FinderSync
    /// extension to paint overlay badges in Finder. Broadcast to every
    /// connected client; non-FinderSync clients can ignore.
    BadgeUpdate(BadgeUpdateMsg),
    /// Reply to `UfbToAgent::TestCredentials`. The `result` enum tells
    /// the GUI whether to render a green check, a red "auth failed"
    /// pill, or a gray "couldn't reach the share" message.
    TestCredentialsResult(TestCredentialsResultMsg),
}

/// Authoritative snapshot of every known mount's state. The `mounts`
/// vec lists every mount currently tracked by `MountService`; any
/// mount in the client's local map that is NOT in this snapshot has
/// been removed (config-deleted, disabled, or orchestrator died) and
/// should be pruned.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountStateSnapshotMsg {
    pub mounts: Vec<MountStateUpdateMsg>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BadgeKind {
    /// Fully hydrated — all bytes cached locally.
    Hydrated,
    /// Partial — some chunks cached (chunk_bitmap has bits set).
    Partial,
    /// No local cache — reads will proxy to SMB. FinderSync should drop
    /// any existing badge for this path.
    Uncached,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BadgeUpdateMsg {
    pub domain: String,
    /// Path relative to the share root.
    pub relpath: String,
    pub badge: BadgeKind,
}

/// Result kind for `TestCredentialsResultMsg.kind`. String-typed so
/// hand-written test fixtures and any future debug tooling stay
/// readable in raw JSON form.
///
/// - `ok`            — share reachable + credentials accepted.
/// - `auth_failed`   — share reachable but creds rejected.
/// - `network_error` — share unreachable (DNS / firewall / down).
/// - `no_credentials`— keystore had no entry for credential_key.
/// - `no_mount`      — mount_id not in current configs.
/// - `unsupported`   — platform doesn't implement probing yet.
/// - `internal_error`— unexpected; `message` carries the detail.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TestCredentialsResultMsg {
    pub mount_id: String,
    pub kind: String,
    /// Human-readable detail for the GUI's error pill. Empty on `ok`.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub message: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub command_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CacheStatsMsg {
    pub mount_id: String,
    /// Total bytes of hydrated (locally cached) file content for this share.
    pub hydrated_bytes: u64,
    /// Number of files currently hydrated.
    pub hydrated_count: u64,
    /// Command ID to correlate with the triggering GetCacheStats request.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub command_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ConflictDetectedMsg {
    /// Domain or share name where the conflict occurred.
    pub domain: String,
    /// Path the user was writing to (canonical relative path inside the share).
    pub original_path: String,
    /// Path where the conflicting write was preserved (sidecar file name).
    pub conflict_path: String,
    /// Hostname of this machine — included in the sidecar name for traceability.
    pub host: String,
    /// Unix epoch seconds when the conflict was detected.
    pub detected_at: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountStateUpdateMsg {
    pub mount_id: String,
    pub state: String,
    pub state_detail: String,
    /// On-demand sync state: "disabled", "registering", "active", "error", "deregistering"
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sync_state: Option<String>,
    /// Human-readable sync status detail
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sync_state_detail: Option<String>,
    /// True if symlink creation requires elevation (Windows)
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub needs_elevation: Option<bool>,
    /// Where the mount actually landed on this machine (resolved
    /// `/Volumes/<share>` on macOS — including any `-1` dedup suffix —
    /// the UNC on Windows, the NFS mountpoint for sync mounts). The
    /// GUI feeds this into the volume registry so path-identity
    /// resolution follows the live mount instead of a static mapping.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mounted_at: Option<String>,
    /// Informational mount-drift notice (e.g. "Mounted as Share-1 —
    /// /Volumes/Share was taken"). UI renders it as a low-key ⚠ on
    /// the mount row; absence means the mount landed where expected.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub notice: Option<String>,
    /// True when the drift is caused by a stale leftover directory
    /// squatting the expected mountpoint (empty, not in the mount
    /// table) — i.e. the GUI can offer a "Fix…" action that removes it
    /// with admin rights and remounts. False/absent for a live
    /// foreign occupant, which must never be touched.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub notice_fixable: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AckMsg {
    pub command_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ErrorMsg {
    pub command_id: String,
    pub message: String,
}

// ── UFB → Agent ──

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum UfbToAgent {
    StartMount(MountIdMsg),
    StopMount(MountIdMsg),
    RestartMount(MountIdMsg),
    ClearSyncCache(MountIdMsg),
    /// Ask the agent how much content is currently cached for a share.
    /// Agent replies with `AgentToUfb::CacheStats`. Cheap (one indexed
    /// SUM query); safe to poll on dialog open.
    GetCacheStats(MountIdMsg),
    CreateSymlinks,
    ReloadConfig,
    GetStates,
    Ping,
    Quit,
    /// Tell the agent that something user-facing happened (window focus,
    /// refresh button, tab switch). Agent routes the signal to the platform's
    /// freshness mechanism — Darwin notification on macOS (extension picks it
    /// up and signals .workingSet), watcher hint on Windows.
    FreshnessSweep(FreshnessSweepMsg),
    /// Probe whether the credentials saved for `mount_id` actually
    /// authenticate against the share. Agent attempts an SMB session
    /// (WNetUseConnectionW on Windows, scoped TEMPORARY) and replies
    /// with `AgentToUfb::TestCredentialsResult`. The probe is non-
    /// destructive — established session is canceled immediately.
    TestCredentials(MountIdMsg),
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct FreshnessSweepMsg {
    /// Optional domain / share name to scope the sweep. `None` = all enabled mounts.
    #[serde(default)]
    pub domain: Option<String>,
    #[serde(default)]
    pub command_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountIdMsg {
    pub mount_id: String,
    #[serde(default)]
    pub command_id: String,
    /// Start/Restart only: this attempt came from an explicit user
    /// action (Fix-credentials pill), so the mount may present the OS
    /// auth dialog (macOS NetAuthAgent). Background attempts leave it
    /// false and fail silently to the auth_error pill. plans/17 slice C.
    #[serde(default)]
    pub allow_ui: bool,
}

// The `FileOpsRequest` / `FileOpsResponse` IPC surface (ListDir, Stat,
// ReadFile, WriteFile, DeleteItem, RenameItem, ClearCache, EvictAll,
// RecordEnumeration, GetChanges + their responses) existed solely to serve
// the macOS FileProvider extension. Slice 5 retired that extension — the
// NFS loopback server owns the macOS filesystem surface now, so these
// types + `ipc/fileops_server.rs` + the Swift FileProviderExtension
// target were all removed together.

/// A single directory entry. Shared by NFS enumeration + cache record
/// paths; originally lived in the FileOps IPC surface but survived the
/// retirement because both sides of the cache/NFS boundary consume it.
/// Consumers are macOS-only (nfs_server + macos_cache) — Windows
/// builds see it as dead.
#[cfg_attr(windows, allow(dead_code))]
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DirEntry {
    pub name: String,
    pub is_dir: bool,
    pub size: u64,
    /// Seconds since Unix epoch
    pub modified: f64,
    /// Seconds since Unix epoch
    pub created: f64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_agent_to_ufb_serde() {
        let msg = AgentToUfb::MountStateUpdate(MountStateUpdateMsg {
            mount_id: "primary-nas".into(),
            state: "mounted".into(),
            state_detail: "Mounted".into(),
            sync_state: None,
            sync_state_detail: None,
            needs_elevation: None,
            mounted_at: None,
            notice: None,
            notice_fixable: None,
        });

        let json = serde_json::to_string(&msg).unwrap();
        let parsed: AgentToUfb = serde_json::from_str(&json).unwrap();
        assert!(matches!(parsed, AgentToUfb::MountStateUpdate(_)));

        // Verify type tag
        let value: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(value["type"], "mount_state_update");
    }

    #[test]
    fn test_ufb_to_agent_serde() {
        let msg = UfbToAgent::StartMount(MountIdMsg {
            mount_id: "primary-nas".into(),
            command_id: "cmd-123".into(),
            allow_ui: false,
        });
        let json = serde_json::to_string(&msg).unwrap();
        let parsed: UfbToAgent = serde_json::from_str(&json).unwrap();
        assert!(matches!(parsed, UfbToAgent::StartMount(_)));
    }

    #[test]
    fn test_ping_pong() {
        let ping = serde_json::to_string(&UfbToAgent::Ping).unwrap();
        let pong = serde_json::to_string(&AgentToUfb::Pong).unwrap();

        let parsed_ping: UfbToAgent = serde_json::from_str(&ping).unwrap();
        let parsed_pong: AgentToUfb = serde_json::from_str(&pong).unwrap();

        assert!(matches!(parsed_ping, UfbToAgent::Ping));
        assert!(matches!(parsed_pong, AgentToUfb::Pong));
    }

    #[test]
    fn test_reload_config() {
        let msg = UfbToAgent::ReloadConfig;
        let json = serde_json::to_string(&msg).unwrap();
        let parsed: UfbToAgent = serde_json::from_str(&json).unwrap();
        assert!(matches!(parsed, UfbToAgent::ReloadConfig));
    }

    #[test]
    fn test_all_ufb_to_agent_variants() {
        let variants = vec![
            UfbToAgent::StartMount(MountIdMsg { mount_id: "x".into(), command_id: "1".into(), allow_ui: false }),
            UfbToAgent::StopMount(MountIdMsg { mount_id: "x".into(), command_id: "2".into(), allow_ui: false }),
            UfbToAgent::RestartMount(MountIdMsg { mount_id: "x".into(), command_id: "3".into(), allow_ui: false }),
            UfbToAgent::ReloadConfig,
            UfbToAgent::GetStates,
            UfbToAgent::Ping,
        ];
        for v in variants {
            let json = serde_json::to_string(&v).unwrap();
            let _: UfbToAgent = serde_json::from_str(&json).unwrap();
        }
    }

    #[test]
    fn test_all_agent_to_ufb_variants() {
        let variants = vec![
            AgentToUfb::Pong,
            AgentToUfb::Ack(AckMsg { command_id: "1".into() }),
            AgentToUfb::Error(ErrorMsg { command_id: "2".into(), message: "fail".into() }),
            AgentToUfb::MountStateUpdate(MountStateUpdateMsg {
                mount_id: "x".into(),
                state: "s".into(),
                state_detail: "d".into(),
                sync_state: Some("active".into()),
                sync_state_detail: Some("Watching 3 folders".into()),
                needs_elevation: None,
                mounted_at: Some("/Volumes/x".into()),
                notice: None,
            notice_fixable: None,
            }),
        ];
        for v in variants {
            let json = serde_json::to_string(&v).unwrap();
            let _: AgentToUfb = serde_json::from_str(&json).unwrap();
        }
    }
}
