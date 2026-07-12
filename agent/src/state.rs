use crate::config::MountConfig;
use serde::{Deserialize, Serialize};
use std::fmt;

// ── Mount State Machine ──
// Pure transition function — no side effects. Returns new state + effects for orchestrator.

/// Combined lifecycle for a mount. The `Mounted` variant carries a
/// `SyncPhase` so the on-demand sync sub-state lives inside the state
/// where it's meaningful, rather than as a parallel field in the
/// orchestrator. Non-sync mounts hold `SyncPhase::NotApplicable`.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum MountState {
    Initializing,
    Mounting,
    Mounted(SyncPhase),
    Error(MountError),
    Stopped,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum MountError {
    ConfigInvalid { reason: String },
    MountFailed { reason: String },
    /// SMB authentication was rejected by the server. Distinct from
    /// MountFailed so the UI can render a "Fix credentials" pill that
    /// pops the credential prompt instead of just an error toast.
    AuthFailed { reason: String },
}

/// On-demand sync sub-state. Lives inside `MountState::Mounted` since
/// it's only meaningful while the mount itself is up. Replaces the
/// pre-refactor `SyncState` enum and its parallel `sync_state` field on
/// the orchestrator. Variants kept narrow on purpose: failure modes
/// surface as `MountState::Error` at the parent level; `Spawning` and
/// transient teardown windows are captured in-band.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum SyncPhase {
    /// Mount isn't sync-enabled; sync_state is None on the wire.
    NotApplicable,
    /// VFS server is being brought up after a successful SMB mount.
    Spawning,
    /// VFS server is serving requests; NAS is reachable.
    Active,
    /// Heartbeat probe declared the NAS unreachable. VFS ops short-
    /// circuit with JUKEBOX until reconnect succeeds.
    Offline,
    /// Attempting to restore the SMB session after going Offline.
    Reconnecting,
}

impl SyncPhase {
    /// Stable wire name for the `sync_state` field of `MountStateUpdateMsg`.
    /// Returns `None` for `NotApplicable` so non-sync mounts don't surface
    /// a meaningless sub-state to the UI. Other variants map 1:1 to the
    /// pre-refactor `SyncState::state_name()` strings — preserves the
    /// QML / Swift contract without requiring a binding-side update.
    pub fn wire_name(&self) -> Option<&'static str> {
        match self {
            SyncPhase::NotApplicable => None,
            SyncPhase::Spawning => Some("registering"),
            SyncPhase::Active => Some("active"),
            SyncPhase::Offline => Some("offline"),
            SyncPhase::Reconnecting => Some("reconnecting"),
        }
    }
}

impl fmt::Display for MountError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MountError::ConfigInvalid { reason } => write!(f, "invalid config: {}", reason),
            MountError::MountFailed { reason } => write!(f, "mount failed: {}", reason),
            MountError::AuthFailed { reason } => write!(f, "auth failed: {}", reason),
        }
    }
}

impl fmt::Display for SyncPhase {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SyncPhase::NotApplicable => write!(f, "Disabled"),
            SyncPhase::Spawning => write!(f, "Registering"),
            SyncPhase::Active => write!(f, "Active"),
            SyncPhase::Offline => write!(f, "NAS unreachable"),
            SyncPhase::Reconnecting => write!(f, "Reconnecting..."),
        }
    }
}

#[derive(Debug, Clone)]
pub enum MountEvent {
    // Lifecycle
    Start,
    Stop,
    Restart,

    // Config changed while mount is running — orchestrator should
    // tear down the old config and apply the new one.
    ConfigChanged { new_config: MountConfig },

    // Platform errors
    MountFailed { reason: String },
    /// Reachability probe / SMB session establish detected the server
    /// refused our credentials. State transitions to Error(AuthFailed).
    AuthFailed { reason: String },

    // State query
    RequestStateUpdate,

    // Cache management
    ClearSyncCache,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Effect {
    /// Connect and map the drive (single operation per platform).
    /// Windows: WNetAddConnection2W with drive letter.
    /// Linux: gio mount + symlink.
    MountDrive,
    /// Disconnect the drive before remounting or stopping.
    /// Windows: WNetCancelConnection2W. Linux: remove symlink.
    DisconnectDrive,
    /// Spawn the VFS server (NFS loopback on macOS, WinFsp on Windows)
    /// for a sync-enabled mount. No-op for non-sync mounts. Fires on
    /// Mounting → Mounted (i.e. as part of RequestStateUpdate's effect
    /// chain). Must run AFTER MountDrive has resolved the backing
    /// filesystem path; the orchestrator owns that resolution and
    /// stashes it in `mounted_at` before dispatching.
    SpawnSyncServer,
    /// Shut down the VFS server. No-op if no handle is live. Always
    /// emitted BEFORE DisconnectDrive so the VFS unmounts its loopback
    /// (NFS) or releases its FileSystemHost (WinFsp) before the SMB
    /// session it depends on goes away. Idempotent.
    TeardownSyncServer,
    UpdateTray,
    LogEvent { level: LogLevel, message: String },
    EmitStateUpdate,
}

#[derive(Debug, Clone, PartialEq)]
pub enum LogLevel {
    Info,
    Error,
}

/// Pure state transition function. No side effects — returns new state + effects.
///
/// Coverage audit (rows = state, cols = event). `*` marks transitions
/// the pre-refactor table dropped silently; the new table handles them.
///
/// |               | Start    | Stop    | Restart  | ConfigChanged | MountFailed | AuthFailed | RequestStateUpdate | ClearSyncCache |
/// |---------------|----------|---------|----------|---------------|-------------|------------|--------------------|-----------------|
/// | Initializing  | Mounting | Stopped | Mounting*| Mounting      | (n/a)       | Error      | self               | self            |
/// | Mounting      | self     | Stopped | self     | Mounting      | Error       | Error      | Mounted            | self            |
/// | Mounted(_)    | Mounting*| Stopped | Mounting | Mounting      | Error       | Error      | self               | self            |
/// | Error(_)      | Mounting | Stopped | Mounting | Mounting      | Error       | Error      | self               | self            |
/// | Stopped       | Mounting | self    | Mounting*| Mounting      | (n/a)       | Error      | self               | self            |
///
/// "self" = no transition, no effects. Auto-progression Mounting →
/// Mounted runs in `Orchestrator::run` via a synthetic
/// RequestStateUpdate event after each Mounting-producing transition.
pub fn transition(
    state: MountState,
    event: MountEvent,
) -> (MountState, Vec<Effect>) {
    use MountEvent::*;
    use MountState::*;

    match (&state, event) {
        // ── Auth failure: route to Error from any state ──
        (_, AuthFailed { reason }) => (
            Error(MountError::AuthFailed {
                reason: reason.clone(),
            }),
            vec![
                Effect::LogEvent {
                    level: LogLevel::Error,
                    message: format!("auth failed: {}", reason),
                },
                Effect::UpdateTray,
                Effect::EmitStateUpdate,
            ],
        ),

        // ── Mount failure: same global treatment as AuthFailed.
        // Slice H race fix: pre-refactor this was only handled from
        // Mounting; if mount_drive emitted MountFailed AFTER the
        // synthetic Mounting→Mounted RequestStateUpdate already
        // landed, the (Mounted, MountFailed) cell was a no-op and
        // state silently stayed Mounted with a broken mount path.
        (_, MountFailed { reason }) => (
            Error(MountError::MountFailed {
                reason: reason.clone(),
            }),
            vec![
                Effect::LogEvent {
                    level: LogLevel::Error,
                    message: format!("mount failed: {}", reason),
                },
                Effect::UpdateTray,
                Effect::EmitStateUpdate,
            ],
        ),

        // ── Initializing ──
        (Initializing, Start) => (
            Mounting,
            vec![
                Effect::MountDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "mounting SMB share".into(),
                },
                Effect::EmitStateUpdate,
            ],
        ),
        // Restart-before-the-initial-Start: treat as Start. Today's
        // table drops this silently; users clicking Restart on a
        // fresh mount would see no response.
        (Initializing, Restart) => (
            Mounting,
            vec![
                Effect::MountDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "restart-before-init treated as start".into(),
                },
                Effect::EmitStateUpdate,
            ],
        ),

        // ── Mounting ──
        (Mounting, RequestStateUpdate) => (
            // Mounting → Mounted with NotApplicable phase by default;
            // the orchestrator's sync-spawn path mutates the phase
            // imperatively (start_sync etc.) for sync-enabled mounts.
            Mounted(SyncPhase::NotApplicable),
            vec![
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "mounted".into(),
                },
                Effect::UpdateTray,
                // SpawnSyncServer is a no-op for non-sync mounts. For
                // sync mounts the orchestrator's effect handler reads
                // `mounted_at` (set by MountDrive) and brings up the
                // NFS/WinFsp server bound to that path. Eliminates the
                // pre-refactor 5s sleep + 60s poll race in main.rs.
                Effect::SpawnSyncServer,
                Effect::EmitStateUpdate,
            ],
        ),
        // (Mounting, MountFailed) — handled by the global arm above.

        // ── Mounted ──
        // TeardownSyncServer ALWAYS precedes DisconnectDrive — the
        // NFS/WinFsp server backs onto the SMB mount and must shut
        // down first so it doesn't serve from a vanishing fs root.
        (Mounted(_), Restart) => (
            Mounting,
            vec![
                Effect::TeardownSyncServer,
                Effect::DisconnectDrive,
                Effect::MountDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "restart requested".into(),
                },
                Effect::EmitStateUpdate,
            ],
        ),
        // Start while already Mounted: treat as Restart. Users who
        // click "Start" expecting recovery from a half-broken state
        // (e.g. NAS came back) get the same effect as Restart.
        (Mounted(_), Start) => (
            Mounting,
            vec![
                Effect::TeardownSyncServer,
                Effect::DisconnectDrive,
                Effect::MountDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "start-while-mounted treated as restart".into(),
                },
                Effect::EmitStateUpdate,
            ],
        ),

        // ── Error ──
        (Error(_), Start) | (Error(_), Restart) => (
            Mounting,
            vec![
                Effect::TeardownSyncServer,
                Effect::DisconnectDrive,
                Effect::MountDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "retrying from error state".into(),
                },
                Effect::EmitStateUpdate,
            ],
        ),

        // ── Stopped ──
        (Stopped, Start) => (
            Mounting,
            vec![
                Effect::MountDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "starting from stopped state".into(),
                },
                Effect::EmitStateUpdate,
            ],
        ),
        // Restart-from-Stopped: treat as Start. Today's table drops
        // this; users hitting Restart on a stopped mount see nothing.
        (Stopped, Restart) => (
            Mounting,
            vec![
                Effect::MountDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "restart-from-stopped treated as start".into(),
                },
                Effect::EmitStateUpdate,
            ],
        ),

        // ── Global: Config changed — restart to apply new config ──
        (_, ConfigChanged { .. }) => (
            Mounting,
            vec![
                Effect::TeardownSyncServer,
                Effect::DisconnectDrive,
                Effect::MountDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "config changed, restarting".into(),
                },
                Effect::EmitStateUpdate,
            ],
        ),

        // ── Global: Request state update from any state (no transition) ──
        (_, RequestStateUpdate) => (
            state,
            vec![Effect::EmitStateUpdate],
        ),

        // ── Global: Stop from any state ──
        (_, Stop) => (
            Stopped,
            vec![
                Effect::TeardownSyncServer,
                Effect::DisconnectDrive,
                Effect::LogEvent {
                    level: LogLevel::Info,
                    message: "mount stopped".into(),
                },
                Effect::UpdateTray,
                Effect::EmitStateUpdate,
            ],
        ),

        // ── Invalid / no-op transitions (exhaustive fallthrough) ──
        _ => (state, vec![]),
    }
}

impl fmt::Display for MountState {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MountState::Initializing => write!(f, "Initializing"),
            MountState::Mounting => write!(f, "Mounting"),
            MountState::Mounted(phase) => match phase {
                SyncPhase::NotApplicable => write!(f, "Mounted"),
                other => write!(f, "Mounted ({})", other),
            },
            MountState::Error(e) => write!(f, "Error: {}", e),
            MountState::Stopped => write!(f, "Stopped"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_init_to_mounting() {
        let (state, effects) = transition(MountState::Initializing, MountEvent::Start);
        assert!(matches!(state, MountState::Mounting));
        assert!(effects.contains(&Effect::MountDrive));
        assert!(effects.contains(&Effect::EmitStateUpdate));
    }

    #[test]
    fn test_init_restart_treated_as_start() {
        let (state, effects) = transition(MountState::Initializing, MountEvent::Restart);
        assert!(matches!(state, MountState::Mounting));
        assert!(effects.contains(&Effect::MountDrive));
    }

    #[test]
    fn test_mounting_to_mounted() {
        let (state, effects) =
            transition(MountState::Mounting, MountEvent::RequestStateUpdate);
        assert!(matches!(state, MountState::Mounted(SyncPhase::NotApplicable)));
        assert!(effects.contains(&Effect::UpdateTray));
        assert!(effects.contains(&Effect::EmitStateUpdate));
    }

    #[test]
    fn test_mounting_failed() {
        let (state, _effects) = transition(
            MountState::Mounting,
            MountEvent::MountFailed { reason: "network error".into() },
        );
        assert!(matches!(state, MountState::Error(MountError::MountFailed { .. })));
    }

    #[test]
    fn test_mounted_restart() {
        let (state, effects) = transition(
            MountState::Mounted(SyncPhase::Active),
            MountEvent::Restart,
        );
        assert!(matches!(state, MountState::Mounting));
        assert!(effects.contains(&Effect::DisconnectDrive));
        assert!(effects.contains(&Effect::MountDrive));
    }

    #[test]
    fn test_mounted_start_treated_as_restart() {
        let (state, effects) = transition(
            MountState::Mounted(SyncPhase::Active),
            MountEvent::Start,
        );
        assert!(matches!(state, MountState::Mounting));
        assert!(effects.contains(&Effect::DisconnectDrive));
        assert!(effects.contains(&Effect::MountDrive));
    }

    #[test]
    fn test_mounted_stop() {
        let (state, effects) =
            transition(MountState::Mounted(SyncPhase::Active), MountEvent::Stop);
        assert!(matches!(state, MountState::Stopped));
        assert!(effects.contains(&Effect::DisconnectDrive));
    }

    #[test]
    fn test_error_retry() {
        let (state, _) = transition(
            MountState::Error(MountError::MountFailed { reason: "test".into() }),
            MountEvent::Start,
        );
        assert!(matches!(state, MountState::Mounting));
    }

    #[test]
    fn test_stopped_start() {
        let (state, _) = transition(MountState::Stopped, MountEvent::Start);
        assert!(matches!(state, MountState::Mounting));
    }

    #[test]
    fn test_stopped_restart_treated_as_start() {
        let (state, effects) = transition(MountState::Stopped, MountEvent::Restart);
        assert!(matches!(state, MountState::Mounting));
        assert!(effects.contains(&Effect::MountDrive));
        // No DisconnectDrive on restart-from-stopped — there's nothing
        // mounted to disconnect, and running it racily creates errors
        // in logs.
        assert!(!effects.contains(&Effect::DisconnectDrive));
    }

    #[test]
    fn test_global_stop() {
        let states = vec![
            MountState::Mounting,
            MountState::Mounted(SyncPhase::Active),
            MountState::Mounted(SyncPhase::Offline),
            MountState::Error(MountError::MountFailed { reason: "x".into() }),
        ];
        for s in states {
            let (new_state, effects) = transition(s, MountEvent::Stop);
            assert!(matches!(new_state, MountState::Stopped));
            assert!(effects.contains(&Effect::EmitStateUpdate));
        }
    }

    #[test]
    fn test_invalid_event_is_noop() {
        // Mounting + Start is a no-op (already mounting; another Start
        // would be redundant).
        let (state, effects) = transition(MountState::Mounting, MountEvent::Start);
        assert!(matches!(state, MountState::Mounting));
        assert!(effects.is_empty());
    }

    #[test]
    fn test_mount_failed_from_any_state() {
        // Slice H regression guard: pre-fix (Mounted, MountFailed)
        // was a silent no-op because the arm only matched Mounting.
        // After a sync-toggle disconnect race, the synthetic
        // RequestStateUpdate that fires Mounting→Mounted ran BEFORE
        // the MountFailed event in the channel was processed, so
        // state lied about a broken mount.
        for s in [
            MountState::Initializing,
            MountState::Mounting,
            MountState::Mounted(SyncPhase::Active),
            MountState::Mounted(SyncPhase::NotApplicable),
            MountState::Stopped,
        ] {
            let (new_state, _) = transition(
                s,
                MountEvent::MountFailed { reason: "boom".into() },
            );
            assert!(matches!(
                new_state,
                MountState::Error(MountError::MountFailed { .. })
            ));
        }
    }

    #[test]
    fn test_auth_failed_from_any_state() {
        for s in [
            MountState::Initializing,
            MountState::Mounting,
            MountState::Mounted(SyncPhase::Active),
            MountState::Stopped,
        ] {
            let (new_state, _) = transition(
                s,
                MountEvent::AuthFailed { reason: "bad creds".into() },
            );
            assert!(matches!(
                new_state,
                MountState::Error(MountError::AuthFailed { .. })
            ));
        }
    }

    #[test]
    fn test_config_changed_restarts_from_any_state() {
        use crate::config::MountConfig;
        let new_config = MountConfig::default();
        for s in [
            MountState::Initializing,
            MountState::Mounting,
            MountState::Mounted(SyncPhase::Active),
            MountState::Stopped,
            MountState::Error(MountError::MountFailed { reason: "x".into() }),
        ] {
            let (new_state, effects) = transition(
                s,
                MountEvent::ConfigChanged { new_config: new_config.clone() },
            );
            assert!(matches!(new_state, MountState::Mounting));
            assert!(effects.contains(&Effect::DisconnectDrive));
            assert!(effects.contains(&Effect::MountDrive));
        }
    }

    #[test]
    fn test_sync_phase_wire_name() {
        assert_eq!(SyncPhase::NotApplicable.wire_name(), None);
        assert_eq!(SyncPhase::Spawning.wire_name(), Some("registering"));
        assert_eq!(SyncPhase::Active.wire_name(), Some("active"));
        assert_eq!(SyncPhase::Offline.wire_name(), Some("offline"));
        assert_eq!(SyncPhase::Reconnecting.wire_name(), Some("reconnecting"));
    }
}
