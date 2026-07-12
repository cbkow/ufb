//! Tagged path identity — the OS-agnostic key the path-identity
//! migration replaces raw path strings with.
//! See `plans/path-identity-migration.md`.
//!
//! Two variants:
//!   * `vol:{uuid}/{rel}`   — a path under a registered volume.
//!                            OS-agnostic, mesh-eligible, dedup-able.
//!   * `native:{os}/{path}` — anything else (Desktop, local disk, USB).
//!                            Per-machine, never synced. The common
//!                            case for a file browser — not a failure.
//!
//! `classify` (native path → Identity) runs on every write; `resolve`
//! (Identity → native path) runs on every read. Both are pure
//! functions over a `VolumeView` snapshot, so they unit-test without a
//! live DB. As of Tier 1 nothing in the running app calls them yet —
//! this module is exercised only by tests and the shadow dry-run.

use std::collections::{HashMap, HashSet};

/// A stored path identity. Serialised to/from a single TEXT column via
/// [`Identity::to_storage`] / [`Identity::from_storage`].
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Identity {
    /// A path under a registered volume. `rel` is forward-slash, with
    /// no leading slash; an empty `rel` means the volume root.
    Volume { uuid: String, rel: String },
    /// A path not under any registered volume. `os` is `"win"` or
    /// `"mac"`; `path` is that OS's native form.
    Native { os: String, path: String },
}

impl Identity {
    /// Serialise to the single-string storage form.
    pub fn to_storage(&self) -> String {
        match self {
            Identity::Volume { uuid, rel } => {
                if rel.is_empty() {
                    format!("vol:{uuid}")
                } else {
                    format!("vol:{uuid}/{rel}")
                }
            }
            Identity::Native { os, path } => format!("native:{os}/{path}"),
        }
    }

    /// Parse a storage string. Returns `None` for any string that is
    /// not already a tagged identity — the caller treats that as a
    /// legacy/unmigrated path and runs [`classify`] on it.
    pub fn from_storage(s: &str) -> Option<Identity> {
        if let Some(rest) = s.strip_prefix("vol:") {
            let (uuid, rel) = match rest.split_once('/') {
                Some((u, r)) => (u.to_string(), r.to_string()),
                None => (rest.to_string(), String::new()),
            };
            if uuid.is_empty() {
                return None;
            }
            Some(Identity::Volume { uuid, rel })
        } else if let Some(rest) = s.strip_prefix("native:") {
            let (os, path) = rest.split_once('/')?;
            if os.is_empty() || path.is_empty() {
                return None;
            }
            Some(Identity::Native {
                os: os.to_string(),
                path: path.to_string(),
            })
        } else {
            None
        }
    }

    pub fn is_volume(&self) -> bool {
        matches!(self, Identity::Volume { .. })
    }
}

/// One registered volume, as [`VolumeView::build`] consumes it.
#[derive(Debug, Clone)]
pub struct VolumeSpec {
    pub uuid: String,
    /// Native-path prefixes that classify a path INTO this volume —
    /// typically the win + mac connection hints. Matching tolerates
    /// separator, drive-letter and case differences, so every legacy
    /// form of a path resolves to the same volume.
    pub prefixes: Vec<String>,
    /// This machine's local mount root for `resolve`, or `None` when
    /// the volume is not mounted here.
    pub local_root: Option<String>,
}

/// In-memory snapshot of the volume registry + this machine's
/// bindings. Built by `crate::volumes::load_view`; consumed by the
/// pure `classify`/`resolve` functions.
#[derive(Debug, Clone, Default)]
pub struct VolumeView {
    /// (uuid, raw prefix) — sorted so the longest (most specific)
    /// normalised prefix is tried first.
    prefixes: Vec<(String, String)>,
    /// uuid → local mount root.
    roots: HashMap<String, String>,
}

impl VolumeView {
    pub fn build(specs: Vec<VolumeSpec>) -> Self {
        let mut prefixes: Vec<(String, String)> = Vec::new();
        let mut roots: HashMap<String, String> = HashMap::new();
        for spec in specs {
            if let Some(root) = spec.local_root {
                if !root.trim().is_empty() {
                    roots.insert(spec.uuid.clone(), root);
                }
            }
            // Skip empty prefixes and within-volume duplicates (the
            // win + mac hints often normalise identically).
            let mut seen: HashSet<String> = HashSet::new();
            for p in spec.prefixes {
                let key = norm_for_match(&p);
                if key.is_empty() || !seen.insert(key) {
                    continue;
                }
                prefixes.push((spec.uuid.clone(), p));
            }
        }
        // Longest normalised prefix first — a volume nested under
        // another must win over its parent.
        prefixes.sort_by(|a, b| {
            norm_for_match(&b.1)
                .len()
                .cmp(&norm_for_match(&a.1).len())
        });
        VolumeView { prefixes, roots }
    }

    pub fn is_empty(&self) -> bool {
        self.prefixes.is_empty() && self.roots.is_empty()
    }
}

/// Classify a native filesystem path into a tagged [`Identity`].
/// A path under a registered volume becomes `Volume`; anything else
/// becomes `Native` tagged with the current OS.
pub fn classify(native_path: &str, view: &VolumeView) -> Identity {
    for (uuid, prefix) in &view.prefixes {
        if let Some(rel) = rel_under(native_path, prefix) {
            return Identity::Volume {
                uuid: uuid.clone(),
                rel,
            };
        }
    }
    Identity::Native {
        os: crate::utils::current_os_tag().to_string(),
        path: native_path.to_string(),
    }
}

/// Resolve a tagged [`Identity`] back to a native path for this
/// machine. Returns `None` when a `Volume` is not bound here, or when
/// a `Native` identity belongs to a different OS — an honest "not
/// available here" rather than a fabricated path.
pub fn resolve(id: &Identity, view: &VolumeView) -> Option<String> {
    match id {
        Identity::Volume { uuid, rel } => {
            let root = view.roots.get(uuid)?;
            let joined = if rel.is_empty() {
                root.clone()
            } else {
                format!("{}/{}", root.trim_end_matches(['/', '\\']), rel)
            };
            Some(crate::utils::to_native_path(
                &joined,
                crate::utils::current_os_tag(),
            ))
        }
        Identity::Native { os, path } => {
            if os == crate::utils::current_os_tag() {
                Some(path.clone())
            } else {
                None
            }
        }
    }
}

// ── matching helpers ──────────────────────────────────────────────

/// Strip a leading `X:` drive letter, if present.
fn strip_drive(s: &str) -> &str {
    let b = s.as_bytes();
    if b.len() >= 2 && b[1] == b':' && b[0].is_ascii_alphabetic() {
        &s[2..]
    } else {
        s
    }
}

/// Normalise a path/prefix for comparison: backslashes → slashes,
/// leading drive letter dropped, leading/trailing slashes trimmed.
/// Case is preserved here (callers lowercase separately).
fn norm_for_match(s: &str) -> String {
    let slashed = s.replace('\\', "/");
    strip_drive(&slashed)
        .trim_start_matches('/')
        .trim_end_matches('/')
        .to_string()
}

/// If `native_path` lies under `prefix`, return the path relative to
/// it — forward-slash, no leading slash, case preserved. Comparison is
/// separator-, drive-letter-, leading-slash- and case-insensitive and
/// only matches on a path-component boundary, so `/Volumes/share` never
/// matches a volume rooted at `/Volumes/share-2`.
fn rel_under(native_path: &str, prefix: &str) -> Option<String> {
    let p_norm = norm_for_match(native_path);
    let pre_norm = norm_for_match(prefix);
    if pre_norm.is_empty() {
        return None;
    }
    let p_key = p_norm.to_lowercase();
    let pre_key = pre_norm.to_lowercase();
    if p_key == pre_key {
        // The path IS the volume root.
        return Some(String::new());
    }
    let pre_key_sep = format!("{pre_key}/");
    if p_key.starts_with(&pre_key_sep) {
        // Slice the case-preserved remainder. A mount root is ASCII in
        // practice, so its lowercased byte length equals its original
        // byte length — the same assumption the `translate_path_to`
        // matcher makes.
        let rel = &p_norm[pre_key_sep.len()..];
        return Some(rel.trim_start_matches('/').to_string());
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn union_view() -> VolumeView {
        VolumeView::build(vec![VolumeSpec {
            uuid: "vol-union".to_string(),
            prefixes: vec![
                "C:\\Volumes\\studio-nas\\jobs".to_string(),
                "/Volumes/studio-nas/jobs".to_string(),
            ],
            local_root: Some(
                if cfg!(target_os = "windows") {
                    "C:\\Volumes\\studio-nas\\jobs"
                } else {
                    "/Volumes/studio-nas/jobs"
                }
                .to_string(),
            ),
        }])
    }

    #[test]
    fn storage_round_trip_volume() {
        let id = Identity::Volume {
            uuid: "abc-123".to_string(),
            rel: "jobs/250101_demo".to_string(),
        };
        let s = id.to_storage();
        assert_eq!(s, "vol:abc-123/jobs/250101_demo");
        assert_eq!(Identity::from_storage(&s), Some(id));
    }

    #[test]
    fn storage_round_trip_volume_root() {
        let id = Identity::Volume {
            uuid: "abc-123".to_string(),
            rel: String::new(),
        };
        assert_eq!(id.to_storage(), "vol:abc-123");
        assert_eq!(Identity::from_storage("vol:abc-123"), Some(id));
    }

    #[test]
    fn storage_round_trip_native() {
        let id = Identity::Native {
            os: "win".to_string(),
            path: "C:\\Users\\alice\\Desktop".to_string(),
        };
        let s = id.to_storage();
        assert_eq!(s, "native:win/C:\\Users\\alice\\Desktop");
        assert_eq!(Identity::from_storage(&s), Some(id));
    }

    #[test]
    fn from_storage_rejects_legacy_string() {
        // A raw, untagged path is not an identity — caller runs classify.
        assert_eq!(Identity::from_storage("C:\\Volumes\\x"), None);
        assert_eq!(Identity::from_storage("/Volumes/x"), None);
        assert_eq!(Identity::from_storage("\\Volumes\\x"), None);
    }

    #[test]
    fn classify_three_legacy_forms_collapse_to_one_identity() {
        let view = union_view();
        let want = "vol:vol-union/250101_demo";
        for p in [
            "C:\\Volumes\\studio-nas\\jobs\\250101_demo",
            "\\Volumes\\studio-nas\\jobs\\250101_demo",
            "/Volumes/studio-nas/jobs/250101_demo",
        ] {
            assert_eq!(classify(p, &view).to_storage(), want, "for {p:?}");
        }
    }

    #[test]
    fn classify_is_case_insensitive() {
        let view = union_view();
        let id = classify(
            "C:\\VOLUMES\\Studio-NAS\\jobs\\250101_demo",
            &view,
        );
        assert_eq!(id.to_storage(), "vol:vol-union/250101_demo");
    }

    #[test]
    fn classify_volume_root_has_empty_rel() {
        let view = union_view();
        let id = classify("C:\\Volumes\\studio-nas\\jobs", &view);
        assert_eq!(
            id,
            Identity::Volume {
                uuid: "vol-union".to_string(),
                rel: String::new(),
            }
        );
    }

    #[test]
    fn classify_respects_component_boundary() {
        // A sibling dir that merely shares a prefix string must NOT
        // classify into the volume.
        let view = union_view();
        let id = classify(
            "C:\\Volumes\\studio-nas\\jobs-archive\\x",
            &view,
        );
        assert!(!id.is_volume(), "got {id:?}");
    }

    #[test]
    fn classify_unrelated_path_is_native() {
        let view = union_view();
        let id = classify("C:\\Users\\alice\\Desktop\\notes.txt", &view);
        assert!(matches!(id, Identity::Native { .. }));
    }

    #[test]
    fn classify_with_empty_view_is_always_native() {
        let view = VolumeView::default();
        assert!(matches!(
            classify("C:\\anything", &view),
            Identity::Native { .. }
        ));
    }

    #[test]
    fn most_specific_volume_wins() {
        // A nested volume must win over the parent that contains it.
        let view = VolumeView::build(vec![
            VolumeSpec {
                uuid: "parent".to_string(),
                prefixes: vec!["C:\\Volumes\\share".to_string()],
                local_root: Some("C:\\Volumes\\share".to_string()),
            },
            VolumeSpec {
                uuid: "child".to_string(),
                prefixes: vec!["C:\\Volumes\\share\\jobs".to_string()],
                local_root: Some("C:\\Volumes\\share\\jobs".to_string()),
            },
        ]);
        let id = classify("C:\\Volumes\\share\\jobs\\261301", &view);
        assert_eq!(
            id,
            Identity::Volume {
                uuid: "child".to_string(),
                rel: "261301".to_string(),
            }
        );
    }

    #[test]
    fn resolve_native_other_os_is_none() {
        let view = VolumeView::default();
        let other = if cfg!(target_os = "windows") { "mac" } else { "win" };
        let id = Identity::Native {
            os: other.to_string(),
            path: "/some/path".to_string(),
        };
        assert_eq!(resolve(&id, &view), None);
    }

    #[test]
    fn resolve_native_same_os_returns_path() {
        let view = VolumeView::default();
        let id = Identity::Native {
            os: crate::utils::current_os_tag().to_string(),
            path: "the/path".to_string(),
        };
        assert_eq!(resolve(&id, &view).as_deref(), Some("the/path"));
    }

    #[test]
    fn resolve_unbound_volume_is_none() {
        let view = VolumeView::default();
        let id = Identity::Volume {
            uuid: "no-binding".to_string(),
            rel: "x".to_string(),
        };
        assert_eq!(resolve(&id, &view), None);
    }

    #[test]
    fn classify_resolve_round_trips_under_volume() {
        let view = union_view();
        let native = if cfg!(target_os = "windows") {
            "C:\\Volumes\\studio-nas\\jobs\\250101_demo"
        } else {
            "/Volumes/studio-nas/jobs/250101_demo"
        };
        let id = classify(native, &view);
        assert!(id.is_volume());
        assert_eq!(resolve(&id, &view).as_deref(), Some(native));
    }

    // ── Cross-platform interop guards (added 2026-05-30) ──────────────
    // These lock the behaviour the metadata mesh relies on for a macOS
    // node and a Windows node to agree on the SAME identity for the same
    // shared folder. They also pin a known hazard (rel case) so the
    // Phase-3 UUID/identity rework has a regression net.

    /// A volume registered with BOTH a UNC prefix (`\\nas\share`) and the
    /// macOS mount prefix classifies a Windows-UNC path and a macOS path
    /// for the same folder to the SAME identity. Without this, the two
    /// boxes would split every column/cell row for the same folder.
    #[test]
    fn classify_unc_and_mac_converge() {
        let view = VolumeView::build(vec![VolumeSpec {
            uuid: "vol-nas".to_string(),
            prefixes: vec![
                "\\\\nas\\jobs".to_string(),
                "/Volumes/jobs".to_string(),
            ],
            local_root: Some("\\\\nas\\jobs".to_string()),
        }]);
        let want = "vol:vol-nas/250101_demo";
        for p in [
            "\\\\nas\\jobs\\250101_demo",
            "/Volumes/jobs/250101_demo",
        ] {
            assert_eq!(classify(p, &view).to_storage(), want, "for {p:?}");
        }
    }

    /// HAZARD (characterization): a volume prefix that is a BARE mapped
    /// drive letter (`Z:\`) normalises to the empty string (drive
    /// stripped, slashes trimmed) and is rejected as a prefix — so a
    /// path like `Z:\261301` falls back to `native:` and is NOT shared.
    /// Windows users who register a share by its mapped drive letter
    /// instead of its UNC path silently won't sync. Workaround today:
    /// register the UNC form (`\\nas\share`). Phase 3 should either
    /// reject bare-drive prefixes at registration with a clear error or
    /// support them explicitly.
    #[test]
    fn classify_bare_mapped_drive_prefix_is_not_shared() {
        let view = VolumeView::build(vec![VolumeSpec {
            uuid: "vol-nas".to_string(),
            prefixes: vec!["Z:\\".to_string()],
            local_root: Some("Z:\\".to_string()),
        }]);
        assert!(
            matches!(classify("Z:\\250101_demo", &view), Identity::Native { .. }),
            "bare mapped-drive prefix unexpectedly classified as a volume"
        );
    }

    /// HAZARD (characterization, not endorsement): the volume PREFIX is
    /// matched case-insensitively, but the `rel` AFTER it is preserved
    /// verbatim — so two references to the same on-disk folder that
    /// differ only in case (trivial to produce on a case-insensitive
    /// APFS/NTFS volume, e.g. a bookmark typed `MyJob` vs `myjob`)
    /// classify to DIFFERENT identities and split their metadata. The
    /// volume root case does NOT matter; only the rel does. Phase 3 must
    /// decide whether to case-fold / on-disk-canonicalise the rel; this
    /// test documents the current split so a fix is detectable.
    #[test]
    fn classify_rel_case_currently_splits_identity() {
        let view = union_view();
        let upper = classify(
            "/Volumes/studio-nas/jobs/MyJob",
            &view,
        )
        .to_storage();
        let lower = classify(
            "/Volumes/studio-nas/jobs/myjob",
            &view,
        )
        .to_storage();
        assert_eq!(upper, "vol:vol-union/MyJob");
        assert_eq!(lower, "vol:vol-union/myjob");
        // Same folder on a case-insensitive volume, two identities:
        assert_ne!(upper, lower, "rel case split — Phase 3 to address");
    }
}
