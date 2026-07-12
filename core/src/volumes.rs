//! Volume registry seeding + the per-machine binding view.
//!
//! `volumes` (mesh-synced) records what storage exists; `volume_bindings`
//! (per-machine, never synced) records where this machine has each
//! volume mounted. Both tables are created in `db::run_migrations`.
//! See `plans/path-identity-migration.md`.

use crate::identity::{VolumeSpec, VolumeView};
use crate::settings::PathMapping;
use rusqlite::Connection;

/// Fixed namespace for deterministic volume UUIDs. NEVER change this —
/// every machine must derive the identical UUID for the same share.
const VOLUME_NS: uuid::Uuid =
    uuid::Uuid::from_u128(0x9e1d_4f60_8b2a_4c17_a3e5_6f0c_2d7b_41a8);

/// Deterministic volume UUID for a (win, mac) connection-hint pair.
/// Two machines independently produce the same UUID for the same
/// share, with no coordination — separator style and a trailing slash
/// do not affect the result.
pub fn volume_uuid(win_hint: &str, mac_hint: &str) -> String {
    fn key(s: &str) -> String {
        s.trim()
            .replace('\\', "/")
            .trim_end_matches('/')
            .to_lowercase()
    }
    let name = format!("{}|{}", key(win_hint), key(mac_hint));
    uuid::Uuid::new_v5(&VOLUME_NS, name.as_bytes()).to_string()
}

fn local_root_for(m: &PathMapping) -> String {
    match crate::utils::current_os_tag() {
        "win" => m.win.clone(),
        _ => m.mac.clone(),
    }
}

fn usable(m: &PathMapping) -> bool {
    m.enabled && !m.win.trim().is_empty() && !m.mac.trim().is_empty()
}

/// Build a `VolumeView` directly from path mappings, without touching
/// the DB. Used by the shadow dry-run and by tests; also the shared
/// shape of `seed_from_path_mappings`.
pub fn view_from_path_mappings(mappings: &[PathMapping]) -> VolumeView {
    let specs: Vec<VolumeSpec> = mappings
        .iter()
        .filter(|m| usable(m))
        .map(|m| VolumeSpec {
            uuid: volume_uuid(&m.win, &m.mac),
            prefixes: vec![m.win.clone(), m.mac.clone()],
            local_root: Some(local_root_for(m)),
        })
        .collect();
    VolumeView::build(specs)
}

/// Seed the `volumes` registry and this machine's `volume_bindings`
/// from the user's configured path mappings. Idempotent: deterministic
/// UUIDs + `INSERT OR IGNORE` mean re-running never duplicates a row or
/// disturbs an existing one. Returns the number of new `volumes` rows.
pub fn seed_from_path_mappings(conn: &Connection) -> rusqlite::Result<usize> {
    let settings = crate::settings::AppSettings::load();
    let now = crate::utils::current_time_ms();
    let mut seeded = 0usize;
    for m in settings.path_mappings.iter().filter(|m| usable(m)) {
        let uuid = volume_uuid(&m.win, &m.mac);
        let display = if m.label.trim().is_empty() {
            m.win
                .trim_end_matches(['/', '\\'])
                .rsplit(['/', '\\'])
                .next()
                .unwrap_or("volume")
                .to_string()
        } else {
            m.label.clone()
        };
        let hints =
            serde_json::json!({ "win": m.win, "mac": m.mac }).to_string();
        seeded += conn.execute(
            "INSERT OR IGNORE INTO volumes
                 (uuid, display_name, kind, connection_hints, modified_time, deleted_at)
             VALUES (?1, ?2, 'smb', ?3, ?4, NULL)",
            rusqlite::params![uuid, display, hints, now],
        )?;
        conn.execute(
            "INSERT OR IGNORE INTO volume_bindings
                 (uuid, local_mount_root, bound_time, auto_discovered)
             VALUES (?1, ?2, ?3, 0)",
            rusqlite::params![
                uuid,
                local_root_for(m).trim_end_matches(['/', '\\']),
                now
            ],
        )?;
    }
    Ok(seeded)
}

// ── Live mount roots ──────────────────────────────────────────────
//
// The mappings above are static user config; where a share is
// *actually* mounted right now is reported by the agent
// (`MountStateUpdateMsg.mounted_at`) and can differ — `/Volumes`
// dedup suffixes on macOS, the UNC on Windows, an NFS mountpoint for
// sync mounts. This registry holds that live truth, keyed by
// lowercased share leaf name, and overlays it onto the mapping-derived
// view so `resolve` follows the mount wherever it landed and
// `classify` recognises paths under the live root.
// See plans/17-os-native-simplification.md, slice A.

/// How identity resolution consumes live mount roots.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LiveRootMode {
    /// Ignore live roots entirely (pre-slice-A behavior).
    Off,
    /// Resolve via static mappings, but also compute the live-root
    /// answer and log when the two disagree. The rollout default.
    Shadow,
    /// Resolve via live roots, falling back to the static mapping
    /// when a share isn't currently mounted.
    Live,
}

impl LiveRootMode {
    pub fn from_str(s: &str) -> LiveRootMode {
        match s.trim().to_ascii_lowercase().as_str() {
            "off" => LiveRootMode::Off,
            "live" => LiveRootMode::Live,
            _ => LiveRootMode::Shadow,
        }
    }
}

struct LiveRootsState {
    mode: LiveRootMode,
    /// lowercased share leaf → live mount root.
    roots: std::collections::HashMap<String, String>,
}

fn live_state() -> &'static std::sync::RwLock<LiveRootsState> {
    static LIVE: std::sync::OnceLock<std::sync::RwLock<LiveRootsState>> =
        std::sync::OnceLock::new();
    LIVE.get_or_init(|| {
        // Initial mode comes from settings on first touch; tests and
        // the app can override via set_live_root_mode.
        let mode = LiveRootMode::from_str(
            &crate::settings::AppSettings::load().identity_live_roots,
        );
        log::info!("volumes: identity live-root mode = {:?} (from settings)", mode);
        std::sync::RwLock::new(LiveRootsState {
            mode,
            roots: std::collections::HashMap::new(),
        })
    })
}

/// Lowercased leaf component of a path/UNC — the key live roots are
/// registered under. `~/ufb/mounts/Projects`, `C:\Volumes\ufb\Projects`
/// and `\\nas\Projects` all yield `projects`.
pub fn share_leaf(s: &str) -> Option<String> {
    s.trim_end_matches(['/', '\\'])
        .rsplit(['/', '\\'])
        .next()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .map(str::to_lowercase)
}

/// Replace the live-root registry. Called by the bindings whenever
/// agent mount state changes; `roots` maps lowercased share leaf →
/// mounted_at.
pub fn set_live_mount_roots(roots: std::collections::HashMap<String, String>) {
    if let Ok(mut st) = live_state().write() {
        if st.roots != roots {
            log::debug!("volumes: live roots updated: {:?}", roots);
            st.roots = roots;
        }
    }
}

pub fn set_live_root_mode(mode: LiveRootMode) {
    if let Ok(mut st) = live_state().write() {
        if st.mode != mode {
            log::info!("volumes: live-root mode -> {:?}", mode);
            st.mode = mode;
        }
    }
}

pub fn live_root_mode() -> LiveRootMode {
    live_state().read().map(|st| st.mode).unwrap_or(LiveRootMode::Shadow)
}

/// Snapshot of the live-root registry (lowercased share leaf → mount
/// root). For callers that need the raw locations rather than a
/// resolution view — e.g. prefix lists that must match browsing paths
/// under either the legacy alias or the live mount location.
pub fn live_mount_roots() -> std::collections::HashMap<String, String> {
    live_state()
        .read()
        .map(|st| st.roots.clone())
        .unwrap_or_default()
}

/// `view_from_path_mappings` with the live-root overlay applied: a
/// mapping whose share leaf has a registered live root gets that root
/// as its resolution target AND as an extra classify prefix (so paths
/// under the live location — e.g. `/Volumes/Share-1/...` — classify
/// into the volume). Mappings without a live root behave exactly as
/// in the plain view. Volume UUIDs derive from the mapping hints only
/// and are unaffected by where the mount landed.
pub fn view_from_path_mappings_live(mappings: &[PathMapping]) -> VolumeView {
    let roots = live_state()
        .read()
        .map(|st| st.roots.clone())
        .unwrap_or_default();
    let specs: Vec<VolumeSpec> = mappings
        .iter()
        .filter(|m| usable(m))
        .map(|m| {
            let live_root = share_leaf(&m.mac)
                .or_else(|| share_leaf(&m.win))
                .and_then(|leaf| roots.get(&leaf))
                .cloned();
            let mut prefixes = vec![m.win.clone(), m.mac.clone()];
            if let Some(r) = &live_root {
                prefixes.push(r.clone());
            }
            VolumeSpec {
                uuid: volume_uuid(&m.win, &m.mac),
                prefixes,
                local_root: Some(
                    live_root.unwrap_or_else(|| local_root_for(m)),
                ),
            }
        })
        .collect();
    VolumeView::build(specs)
}

/// Load the volume registry + this machine's bindings into a
/// `VolumeView`. Tolerant of missing tables (returns an empty view) so
/// it is safe to call against an old DB.
pub fn load_view(conn: &Connection) -> VolumeView {
    use std::collections::HashMap;
    let mut roots: HashMap<String, String> = HashMap::new();
    if let Ok(mut stmt) =
        conn.prepare("SELECT uuid, local_mount_root FROM volume_bindings")
    {
        if let Ok(rows) = stmt.query_map([], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
        }) {
            for row in rows.flatten() {
                roots.insert(row.0, row.1);
            }
        }
    }
    let mut specs: Vec<VolumeSpec> = Vec::new();
    if let Ok(mut stmt) = conn.prepare(
        "SELECT uuid, connection_hints FROM volumes WHERE deleted_at IS NULL",
    ) {
        if let Ok(rows) = stmt.query_map([], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
        }) {
            for (uuid, hints_json) in rows.flatten() {
                let hints: serde_json::Value =
                    serde_json::from_str(&hints_json)
                        .unwrap_or_else(|_| serde_json::json!({}));
                let mut prefixes = Vec::new();
                for k in ["win", "mac"] {
                    if let Some(v) = hints.get(k).and_then(|v| v.as_str()) {
                        if !v.trim().is_empty() {
                            prefixes.push(v.to_string());
                        }
                    }
                }
                let local_root = roots.get(&uuid).cloned();
                specs.push(VolumeSpec {
                    uuid,
                    prefixes,
                    local_root,
                });
            }
        }
    }
    VolumeView::build(specs)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::{classify, Identity};

    #[test]
    fn volume_uuid_is_deterministic_and_separator_insensitive() {
        let a = volume_uuid("C:\\Volumes\\x", "/Volumes/x");
        assert_eq!(a, volume_uuid("C:\\Volumes\\x", "/Volumes/x"));
        // Trailing slash + separator style must not change the UUID.
        assert_eq!(a, volume_uuid("C:/Volumes/x/", "/Volumes/x"));
        // A different share is a different UUID.
        assert_ne!(a, volume_uuid("C:\\Volumes\\y", "/Volumes/y"));
        // Looks like a hyphenated UUID.
        assert_eq!(a.len(), 36);
    }

    #[test]
    fn view_from_mappings_classifies_legacy_form() {
        let maps = [PathMapping {
            win: "C:\\Volumes\\studio-nas\\jobs\\".to_string(),
            mac: "/Volumes/studio-nas/jobs".to_string(),
            enabled: true,
            label: "lucid".to_string(),
        }];
        let view = view_from_path_mappings(&maps);
        let id = classify(
            "\\Volumes\\studio-nas\\jobs\\250101_demo",
            &view,
        );
        assert!(id.is_volume());
        assert!(id.to_storage().ends_with("/250101_demo"));
    }

    #[test]
    fn share_leaf_normalises_all_forms() {
        assert_eq!(share_leaf("~/ufb/mounts/Projects"), Some("projects".into()));
        assert_eq!(share_leaf("C:\\Volumes\\ufb\\Projects"), Some("projects".into()));
        assert_eq!(share_leaf("\\\\nas\\Projects\\"), Some("projects".into()));
        assert_eq!(share_leaf("/Volumes/Projects/"), Some("projects".into()));
        assert_eq!(share_leaf("Projects"), Some("projects".into()));
        assert_eq!(share_leaf(""), None);
    }

    // One combined test: the live-roots registry is process-global, so
    // parallel test fns mutating it would race each other.
    #[test]
    fn live_overlay_follows_mount_and_falls_back() {
        let maps = [PathMapping {
            win: "C:\\Volumes\\ufb\\Projects".to_string(),
            mac: "~/ufb/mounts/Projects".to_string(),
            enabled: true,
            label: "Synology".to_string(),
        }];
        let uuid = volume_uuid(&maps[0].win, &maps[0].mac);

        // No live root registered → overlay view ≡ plain view.
        set_live_mount_roots(std::collections::HashMap::new());
        let plain = view_from_path_mappings(&maps);
        let overlay = view_from_path_mappings_live(&maps);
        let id = Identity::Volume { uuid: uuid.clone(), rel: "job1".into() };
        assert_eq!(
            crate::identity::resolve(&id, &overlay),
            crate::identity::resolve(&id, &plain)
        );

        // Dedup-drift case: the share mounted at /Volumes/Projects-1.
        let mut roots = std::collections::HashMap::new();
        roots.insert("projects".to_string(), "/Volumes/Projects-1".to_string());
        set_live_mount_roots(roots);
        let overlay = view_from_path_mappings_live(&maps);
        // resolve follows the live mount…
        let resolved = crate::identity::resolve(&id, &overlay).unwrap();
        assert!(
            resolved.replace('\\', "/").starts_with("/Volumes/Projects-1"),
            "resolved={resolved}"
        );
        // …and classify recognises paths under the live root (which no
        // static hint covers) as the same volume.
        assert_eq!(
            classify("/Volumes/Projects-1/job2/shot", &overlay),
            Identity::Volume { uuid: uuid.clone(), rel: "job2/shot".into() }
        );
        // UUID derivation ignores where the mount landed: hint-form
        // paths still classify to the identical volume.
        assert_eq!(
            classify("C:\\Volumes\\ufb\\Projects\\job2\\shot", &overlay),
            Identity::Volume { uuid, rel: "job2/shot".into() }
        );

        // Leave the registry clean for any other test in this process.
        set_live_mount_roots(std::collections::HashMap::new());
    }

    #[test]
    fn disabled_mapping_is_skipped() {
        let maps = [PathMapping {
            win: "C:\\Volumes\\x".to_string(),
            mac: "/Volumes/x".to_string(),
            enabled: false,
            label: String::new(),
        }];
        assert!(view_from_path_mappings(&maps).is_empty());
    }

    #[test]
    fn load_view_round_trips_through_db() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE volumes (uuid TEXT PRIMARY KEY, display_name TEXT,
                 kind TEXT, connection_hints TEXT, modified_time INTEGER,
                 deleted_at INTEGER);
             CREATE TABLE volume_bindings (uuid TEXT PRIMARY KEY,
                 local_mount_root TEXT, bound_time INTEGER,
                 auto_discovered INTEGER);",
        )
        .unwrap();
        conn.execute(
            "INSERT INTO volumes VALUES ('u1','lucid','smb',?1,0,NULL)",
            [r#"{"win":"C:\\Volumes\\x","mac":"/Volumes/x"}"#],
        )
        .unwrap();
        conn.execute(
            "INSERT INTO volume_bindings VALUES ('u1',?1,0,0)",
            [if cfg!(windows) { "C:\\Volumes\\x" } else { "/Volumes/x" }],
        )
        .unwrap();
        let view = load_view(&conn);
        assert!(!view.is_empty());
        let native = if cfg!(windows) {
            "C:\\Volumes\\x\\job1"
        } else {
            "/Volumes/x/job1"
        };
        assert_eq!(
            classify(native, &view),
            Identity::Volume {
                uuid: "u1".to_string(),
                rel: "job1".to_string(),
            }
        );
    }
}
