//! UFB core library — pure Rust, Qt-unaware.
//!
//! Phase 1 in progress: porting modules from src-tauri/src/ over to here
//! with Tauri scaffolding stripped. See `plans/02-rust-core-services.md`
//! for the per-module porting status.

#![allow(clippy::too_many_arguments)]
#![allow(clippy::result_large_err)]

// Phase 1a — no Tauri coupling
pub mod backup;
pub mod bookmarks;
pub mod db;
pub mod project_config;
pub mod settings;
pub mod utils;

// Path-identity migration — tagged identity + volume registry.
// See plans/path-identity-migration.md.
pub mod identity;
pub mod volumes;

// Phase 1b — DB-backed managers + search
pub mod columns;
pub mod file_ops; // stub: just FileEntry; full surface in 1d
pub mod metadata;
pub mod search;
pub mod search_index;
pub mod subscription;
/// Filesystem-backed template (preset) storage. Replaces the
/// mesh-broadcast `column_presets` flow whose snapshot/restore
/// loop made follower saves silently fail.
pub mod templates;

// Phase 1c — mesh subsystem
pub mod events;
pub mod mesh;

// Phase 1e — shared application core (replaces Tauri-era AppState)
pub mod app_state;

// Phase 1d — platform / native modules
// explorer_pins deleted (2026-07-11): the Explorer nav-pane CLSID pin
// hack is retired — mounts are ordinary drive letters now, which
// Explorer lists natively. The installer scrubs legacy {0FB…} pin
// registry entries on install/uninstall. (Its stray #[cfg(windows)]
// briefly gated `jobs` out of macOS builds — caught by the first mac
// compile of the Windows-session range.)
pub mod jobs;
/// GUI-owned plain SMB mounting (plans/17 F1) — the app mounts
/// non-sync shares itself; the agent only hosts the sync VFS.
#[cfg(target_os = "macos")]
pub mod macos_mounts;
/// Windows twin (plans/17 slice B): persistent drive letters via WNet,
/// Credential Manager credentials, native auth dialog on allow_ui.
#[cfg(windows)]
pub mod windows_mounts;
pub mod mount_client;
pub mod shell_context_menu;
pub mod sync_aware;
pub mod transcode;

pub fn version() -> &'static str {
    env!("CARGO_PKG_VERSION")
}
