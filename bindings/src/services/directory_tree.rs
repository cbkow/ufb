//! `DirectoryTree` QObject — flat-list tree backing for plan-Phase 6's
//! tree view mode.
//!
//! Cxx-qt 0.7 doesn't expose `QAbstractItemModel`, so a "real" Qt
//! TreeView isn't available from Rust without writing a C++ subclass
//! by hand. As a path-first first cut we ship a flat `ListModel`-shaped
//! representation: each row is a `{path, name, depth, expanded, …}`
//! record, and the QML side draws it as a tree by indenting on `depth`
//! and rendering a disclosure triangle that calls `expand`/`collapse`.
//!
//! Every mutation re-serialises the entire flat list to JSON and
//! re-emits `flat_json`; QML re-syncs its ListModel. Cheap enough for
//! the typical "few hundred visible nodes" case. A real
//! QAbstractItemModel binding (with beginInsertRows / endInsertRows)
//! is the next step if/when cxx-qt grows model support.

use crate::runtime::shared_runtime;
use serde::Serialize;
use std::pin::Pin;
use ufb_core::file_ops;

#[cxx_qt::bridge]
pub mod qobject {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qproperty(QString, root_path)]
        #[qproperty(QString, flat_json)]
        #[qproperty(bool, loading)]
        #[qproperty(QString, error)]
        type DirectoryTree = super::DirectoryTreeRust;

        /// Reset the tree to a single depth-0 listing of `path`.
        /// Async: `loading` flips true; `flat_json` updates when the
        /// listing returns.
        #[qinvokable]
        fn set_root(self: Pin<&mut DirectoryTree>, path: QString);

        /// Expand the node at `path` if it's a not-yet-expanded
        /// directory. Inserts the child rows after the parent.
        #[qinvokable]
        fn expand(self: Pin<&mut DirectoryTree>, path: QString);

        /// Collapse the node at `path`. Removes all consecutive
        /// rows after it whose depth > parent.depth.
        #[qinvokable]
        fn collapse(self: Pin<&mut DirectoryTree>, path: QString);

        /// Re-list the root and any currently-expanded directories.
        /// Useful after a refresh / mutation. Cheap when nothing is
        /// expanded.
        #[qinvokable]
        fn refresh(self: Pin<&mut DirectoryTree>);
    }

    impl cxx_qt::Threading for DirectoryTree {}
}

/// Backing flat-list row. Mirrors what QML's ListModel needs.
#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct TreeNode {
    path: String,
    name: String,
    is_dir: bool,
    depth: u32,
    /// Currently expanded? Only meaningful when `is_dir` is true.
    expanded: bool,
    /// Best-effort hint for the disclosure-triangle visibility. We set
    /// this to `is_dir` since computing "actually has children" needs
    /// a per-folder probe; QML can hide the triangle on first failed
    /// expand if needed.
    has_children: bool,
    extension: String,
    size: u64,
    modified: i64,
}

pub struct DirectoryTreeRust {
    pub root_path: cxx_qt_lib::QString,
    pub flat_json: cxx_qt_lib::QString,
    pub loading: bool,
    pub error: cxx_qt_lib::QString,
    /// Authoritative state. Re-encoded into `flat_json` on every
    /// mutation. Kept out of the QML-visible properties because cxx-qt
    /// 0.7's `#[qproperty]` doesn't surface arbitrary types.
    nodes: Vec<TreeNode>,
}

impl Default for DirectoryTreeRust {
    fn default() -> Self {
        Self {
            root_path: cxx_qt_lib::QString::from(""),
            flat_json: cxx_qt_lib::QString::from("[]"),
            loading: false,
            error: cxx_qt_lib::QString::from(""),
            nodes: Vec::new(),
        }
    }
}

impl qobject::DirectoryTree {
    fn set_root(mut self: Pin<&mut qobject::DirectoryTree>, path: cxx_qt_lib::QString) {
        let path_str = path.to_string();
        self.as_mut().set_root_path(path);
        self.as_mut().set_error(cxx_qt_lib::QString::from(""));
        spawn_set_root(self.as_mut(), path_str);
    }

    fn expand(mut self: Pin<&mut qobject::DirectoryTree>, path: cxx_qt_lib::QString) {
        let path_str = path.to_string();
        // Confirm the parent is currently a not-yet-expanded directory.
        // The actual splice depth is re-resolved on the Qt thread when
        // the listing returns (positions could shift during the listing).
        let depth = {
            use cxx_qt::CxxQtType;
            let r = self.as_ref();
            let found = r
                .rust()
                .nodes
                .iter()
                .find(|n| n.path == path_str && n.is_dir && !n.expanded)
                .map(|n| n.depth);
            match found {
                Some(d) => d,
                None => return,
            }
        };
        spawn_expand(self.as_mut(), path_str, depth);
    }

    fn collapse(mut self: Pin<&mut qobject::DirectoryTree>, path: cxx_qt_lib::QString) {
        use cxx_qt::CxxQtType;
        let path_str = path.to_string();
        let new_json = {
            let rust_mut = self.as_mut().rust_mut();
            let rust = unsafe { rust_mut.get_unchecked_mut() };
            let parent_idx = match rust.nodes.iter().position(|n| n.path == path_str) {
                Some(i) => i,
                None => return,
            };
            if !rust.nodes[parent_idx].expanded {
                return;
            }
            let parent_depth = rust.nodes[parent_idx].depth;
            // Remove consecutive rows after parent that are descendants.
            let mut end = parent_idx + 1;
            while end < rust.nodes.len() && rust.nodes[end].depth > parent_depth {
                end += 1;
            }
            rust.nodes.drain((parent_idx + 1)..end);
            rust.nodes[parent_idx].expanded = false;
            serde_json::to_string(&rust.nodes).unwrap_or_else(|_| "[]".into())
        };
        self.as_mut()
            .set_flat_json(cxx_qt_lib::QString::from(&new_json));
    }

    fn refresh(mut self: Pin<&mut qobject::DirectoryTree>) {
        use cxx_qt::CxxQtType;
        let root = self.as_ref().rust().root_path.to_string();
        if root.is_empty() {
            return;
        }
        // Collect currently-expanded paths so we can re-expand after
        // the root rebuild.
        let expanded_paths: Vec<String> = self
            .as_ref()
            .rust()
            .nodes
            .iter()
            .filter(|n| n.is_dir && n.expanded)
            .map(|n| n.path.clone())
            .collect();
        spawn_refresh(self.as_mut(), root, expanded_paths);
    }
}

// ─── async helpers ──────────────────────────────────────────────────

fn serialize(nodes: &[TreeNode]) -> String {
    serde_json::to_string(nodes).unwrap_or_else(|_| "[]".into())
}

fn list_to_nodes(entries: Vec<file_ops::FileEntry>, depth: u32) -> Vec<TreeNode> {
    entries
        .into_iter()
        .map(|e| TreeNode {
            path: e.path,
            name: e.name,
            is_dir: e.is_dir,
            depth,
            expanded: false,
            has_children: e.is_dir,
            extension: e.extension,
            size: e.size,
            modified: e.modified.unwrap_or(0),
        })
        .collect()
}

fn spawn_set_root(mut dir: Pin<&mut qobject::DirectoryTree>, path: String) {
    use cxx_qt::Threading;

    dir.as_mut().set_loading(true);
    let qt_handle = dir.as_ref().qt_thread();

    let _guard = shared_runtime().enter();
    tokio::task::spawn_blocking(move || {
        let result = file_ops::list_directory(&path);
        let _ = qt_handle.queue(move |mut dir| {
            use cxx_qt::CxxQtType;
            match result {
                Ok(entries) => {
                    let new_json = {
                        let rust_mut = dir.as_mut().rust_mut();
                        let rust = unsafe { rust_mut.get_unchecked_mut() };
                        rust.nodes = list_to_nodes(entries, 0);
                        serialize(&rust.nodes)
                    };
                    dir.as_mut()
                        .set_flat_json(cxx_qt_lib::QString::from(&new_json));
                    dir.as_mut().set_error(cxx_qt_lib::QString::from(""));
                }
                Err(e) => {
                    let new_json = {
                        let rust_mut = dir.as_mut().rust_mut();
                        let rust = unsafe { rust_mut.get_unchecked_mut() };
                        rust.nodes.clear();
                        serialize(&rust.nodes)
                    };
                    dir.as_mut()
                        .set_flat_json(cxx_qt_lib::QString::from(&new_json));
                    dir.as_mut().set_error(cxx_qt_lib::QString::from(&e));
                }
            }
            dir.as_mut().set_loading(false);
        });
    });
}

fn spawn_expand(
    mut dir: Pin<&mut qobject::DirectoryTree>,
    path: String,
    parent_depth: u32,
) {
    use cxx_qt::Threading;

    dir.as_mut().set_loading(true);
    let qt_handle = dir.as_ref().qt_thread();
    let path_for_log = path.clone();

    let _guard = shared_runtime().enter();
    tokio::task::spawn_blocking(move || {
        let result = file_ops::list_directory(&path);
        let _ = qt_handle.queue(move |mut dir| {
            use cxx_qt::CxxQtType;
            match result {
                Ok(entries) => {
                    let new_json: Option<String> = {
                        let rust_mut = dir.as_mut().rust_mut();
                        let rust = unsafe { rust_mut.get_unchecked_mut() };
                        // Re-find the parent index: another expand/collapse
                        // could have shifted positions while we were listing.
                        match rust
                            .nodes
                            .iter()
                            .position(|n| n.path == path && n.is_dir && !n.expanded)
                        {
                            Some(idx) => {
                                let children = list_to_nodes(entries, parent_depth + 1);
                                rust.nodes
                                    .splice((idx + 1)..(idx + 1), children.into_iter());
                                rust.nodes[idx].expanded = true;
                                Some(serialize(&rust.nodes))
                            }
                            None => {
                                log::debug!(
                                    "expand: parent index for {} no longer matches; dropping result",
                                    path_for_log
                                );
                                None
                            }
                        }
                    };
                    if let Some(json) = new_json {
                        dir.as_mut()
                            .set_flat_json(cxx_qt_lib::QString::from(&json));
                        dir.as_mut().set_error(cxx_qt_lib::QString::from(""));
                    }
                }
                Err(e) => {
                    dir.as_mut().set_error(cxx_qt_lib::QString::from(&e));
                }
            }
            dir.as_mut().set_loading(false);
        });
    });
}

fn spawn_refresh(
    mut dir: Pin<&mut qobject::DirectoryTree>,
    root: String,
    expanded: Vec<String>,
) {
    use cxx_qt::Threading;

    dir.as_mut().set_loading(true);
    let qt_handle = dir.as_ref().qt_thread();

    let _guard = shared_runtime().enter();
    tokio::task::spawn_blocking(move || {
        // Re-list the root.
        let root_result = file_ops::list_directory(&root);
        // Re-list each previously-expanded path, in pre-order (parents
        // before children) — sort by depth via path-prefix length,
        // good enough since shallower paths sort shorter for nested
        // dirs under the same root.
        let mut sorted = expanded.clone();
        sorted.sort_by_key(|p| p.len());
        let mut child_results: Vec<(String, Result<Vec<file_ops::FileEntry>, String>)> =
            Vec::with_capacity(sorted.len());
        for p in sorted {
            let r = file_ops::list_directory(&p);
            child_results.push((p, r));
        }

        let _ = qt_handle.queue(move |mut dir| {
            use cxx_qt::CxxQtType;
            let new_json = {
                let rust_mut = dir.as_mut().rust_mut();
                let rust = unsafe { rust_mut.get_unchecked_mut() };
                rust.nodes.clear();
                if let Ok(entries) = root_result {
                    rust.nodes.extend(list_to_nodes(entries, 0));
                }
                // Re-apply expansions in order. Each expansion finds
                // the parent in the current flat list and splices.
                for (path, r) in child_results {
                    let entries = match r {
                        Ok(e) => e,
                        Err(_) => continue,
                    };
                    let (idx, depth) = match rust
                        .nodes
                        .iter()
                        .enumerate()
                        .find(|(_, n)| n.path == path && n.is_dir)
                    {
                        Some((i, n)) => (i, n.depth),
                        None => continue,
                    };
                    let children = list_to_nodes(entries, depth + 1);
                    rust.nodes
                        .splice((idx + 1)..(idx + 1), children.into_iter());
                    rust.nodes[idx].expanded = true;
                }
                serialize(&rust.nodes)
            };
            dir.as_mut()
                .set_flat_json(cxx_qt_lib::QString::from(&new_json));
            dir.as_mut().set_loading(false);
        });
    });
}

