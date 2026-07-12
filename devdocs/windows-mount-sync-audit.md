# Windows mount + sync audit checklist

Companion to the macOS mount+sync refactor (`macOSplans/11-mount-and-sync-refactor.md`).
Most of the structural fixes are already cross-platform via the shared FSM
(`agent/src/state.rs`), shared orchestrator (`agent/src/orchestrator.rs`),
and shared `MountService::route_event`. This doc collects the **Windows-
specific** audits and ports — the macOS-only slices (E, F, G's macOS
bits, H) all have direct equivalents in `winfsp_server.rs` /
`windows_cache.rs` that warrant the same treatment.

Reference implementations on the macOS side, to crib patterns from:
- Write-through cache + per-fh write lock: `agent/src/sync/nfs_server.rs::write_with_stable`
- Orphan-delete guard + spawn_blocking patterns: `agent/src/sync/macos_cache.rs::record_enumeration`
- Stale-stat removal + locally-computed metadata: `agent/src/sync/nfs_server.rs::write_with_stable` (post-write metadata path)

## What's already fixed by the cross-platform refactor

These don't need Windows-side work — they ship via the shared codebase.
Validate on Windows that they actually behave correctly, but no edits.

1. **Stop/Start zombification**: orchestrator no longer exits on `Stopped`.
   The pre-refactor "Stop the orchestrator, then Start silently no-ops"
   bug was identical on Windows.
2. **Dual command-path bug**: `handle_command` for `StartMount`/`StopMount`/
   `RestartMount` now routes through `route_event`. The `tray_cmd_rx` arm
   on Windows already used `route_event` pre-refactor (Windows has a
   real Rust-side win32 tray, unlike macOS's no-op stub), so that path
   always worked — the QML path was broken on both platforms.
3. **`Effect::SpawnSyncServer` / `Effect::TeardownSyncServer`**: the FSM
   transitions fire these on Mounting→Mounted / Stop / Restart /
   ConfigChanged. `Orchestrator::spawn_sync_server` has a `#[cfg(windows)]`
   arm that calls `winfsp_server::start`. Sync server lifecycle is now
   deterministic against the FSM on Windows.
4. **`(_, MountFailed)` global arm**: pre-refactor only `(Mounting,
   MountFailed)` was handled; if the symlink-creation or session-establish
   failed AFTER the synthetic `Mounting→Mounted` `RequestStateUpdate`
   landed, the failure was silently swallowed. Fixed in the shared FSM.
5. **`MountStateSnapshot` IPC**: clients (QML binding + win32 tray) now
   replace their entire mounts map on connect rather than waiting for
   per-mount drips. UI should populate immediately.
6. **`pending_commands_json` + `last_command_error_json`** in the QML
   binding: same QML file renders on Windows. Spinners + error pills
   should Just Work.
7. **`SyncPhase::Spawning → Active`** transition: pre-fix, Windows
   `start_sync` also set `Mounted(SyncPhase::Spawning)` at the end —
   and there was no transition out. UI showed "Registering" forever.
   Fixed in the post-Slice-B spawn_sync_server (set to Active directly).
   The legacy `start_sync` function is still cfg(windows)-only;
   it's now unreachable from the FSM-driven path but the same fix
   was applied there for safety.

## Audits that need a Windows-side pass

Priority order. (1) and (2) are the user-visible reliability/perf
items; (3)-(5) are correctness hardening; (6) is hygiene.

### (1) Cache write-through (port of Slice H)

**Symptom on macOS pre-fix**: save a 200 MB render, scrub it back →
re-fetch all 200 MB from SMB. Read-after-write doubled SMB traffic.

**Likely on Windows**: `winfsp_server.rs::write` probably calls some
form of `invalidate_cache` (or doesn't update the cache file at all)
on every write. Same waste.

**Fix shape** (copy from `agent/src/sync/nfs_server.rs::write_with_stable`):
- After the SMB write succeeds, mirror the same bytes into the per-fh
  cache file at the same offset. Cheap local-disk pwrite.
- Update chunk bitmap for *fully-covered* chunks. Partial leading/
  trailing slivers stay unset.
- If bitmap now complete, call `mark_fully_hydrated`. Common path for
  full-file overwrites of small/medium files.
- **Drop the `invalidate_cache` call** on the write path. Cache stays
  authoritative for what's been written through it.
- Honor `is_hydrated=true` pre-state: write through the bytes but
  don't touch the bitmap (it's NULL when hydrated). File stays
  hydrated.

**Grep first**:
```sh
grep -n "invalidate_cache\|invalidate\b" agent/src/sync/winfsp_server.rs agent/src/sync/windows_cache.rs
```

### (2) Per-fh write lock (port of Slice E lock fix)

**Symptom on macOS pre-fix**: reader takes `fh_lock.read().await`; writer
skipped the lock. Concurrent read could see `is_hydrated=true`, open
the cache file, then `invalidate_cache` deleted it under them →
mid-stream ENOENT.

**Likely on Windows**: same asymmetry. WinFsp's read callback probably
takes a guard while the write callback doesn't.

**Fix shape**:
```rust
// In the write callback, before any cache mutation:
let fh_lock = self.cache.fh_lock(file_id);
let _write_guard = fh_lock.write().await;  // or .blocking_write() if not async
```

**Grep first**:
```sh
grep -n "fh_lock\|write_guard\|read_guard" agent/src/sync/winfsp_server.rs agent/src/sync/windows_cache.rs
```

### (3) Stale post-write SMB stat (port of Slice E)

**Symptom on macOS pre-fix**: post-write `fs::metadata(&abs)` to refresh
cached size/mtime. macOS SMB client caches positive attribute lookups
for ~10s; the stat could return PRE-write metadata, leaving the
cache's `nas_size` stale until drift detection healed it.

**Likely on Windows**: SMB client caching behavior is similar (Windows
SMB caches attrs aggressively). Same staleness window.

**Fix shape**:
```rust
// Don't stat SMB after write. Compute locally:
let new_size = prev_size.max(offset + data.len() as u64);
let mtime = SystemTime::now().duration_since(UNIX_EPOCH)...;
self.cache.update_nas_metadata(&rel, new_size, mtime);
```

**Grep first**:
```sh
grep -n "fs::metadata\|GetFileInformation\|fstat\|file_metadata" agent/src/sync/winfsp_server.rs
```

### (4) Orphan-delete guard on transient empty enumerations (Slice E)

**Symptom on macOS pre-fix**: if SMB returned an empty `read_dir` mid-
cycle (network blip, server overload), `record_enumeration` would
prune every cached child of that folder. Slow re-hydrate on next read.

**Fix shape** (already applied to `macos_cache.rs::record_enumeration`):
```rust
let suspicious_empty = entries.is_empty() && !orphans.is_empty();
if suspicious_empty {
    log::warn!("[{}] {} appears suddenly empty (had {} cached children) \
                — skipping orphan prune; will retry on next enumeration",
               domain, parent, orphans.len());
} else if !orphans.is_empty() {
    // existing delete logic
}
```

**Grep first**:
```sh
grep -n "DELETE FROM.*known_files\|orphan\|prune\|record_enumeration" agent/src/sync/windows_cache.rs
```

### (5) Y2038 saturation in attr conversions

**Symptom**: `as u32` cast on seconds-since-epoch wraps silently past
2038-01-19. NFS3 `nfstime3.seconds` is u32 (protocol limit), so the
saturate helper applies there. WinFsp callbacks use `FILETIME`
(100-ns ticks since 1601) which is 64-bit and doesn't wrap — but if
you ever convert through u32 anywhere, apply the same clamp.

**Reference**: `agent/src/sync/nfs_server.rs::saturate_secs_u32`.

**Grep first**:
```sh
grep -n " as u32\|to_u32\|u32::try_from.*sec" agent/src/sync/winfsp_server.rs
```

### (6) Conflict sidecar bail-loudly (Slice G semantic)

**Symptom on macOS pre-fix**: `preserve_conflict_sidecar_if_drifted`
logged on copy failure and proceeded with the truncate → silent
data loss when the network blipped during a save that conflicted
with another writer.

**Fix shape** (already in `nfs_server.rs::preserve_conflict_sidecar_if_drifted`):
returns `Result<(), nfsstat3>`; setattr's truncate path `?`-propagates.
The caller surfaces JUKEBOX to the client and refuses to truncate
when the sidecar copy failed.

**Check on Windows**: does `winfsp_server.rs` have conflict-detection
logic? If yes, apply same bail-loudly semantics. If no, no fix
needed.

**Grep first**:
```sh
grep -n "conflict\|sidecar\|preserve" agent/src/sync/winfsp_server.rs agent/src/sync/macos_cache.rs
```

## Things that explicitly do NOT apply

Don't waste cycles on these — they have no Windows analog:

- **Tokio worker starvation (Slice E spawn_blocking pass)**: WinFsp
  uses its own native thread pool. Blocking I/O in its callbacks
  doesn't starve the agent's tokio runtime. The macOS NFS server
  needed it because `nfsserve` dispatches handlers on `tokio::spawn`.
  *Caveat*: if any agent code calls into the FSP layer from an
  `async fn`, that path could starve workers — audit if you find such
  call sites, but probably none exist.
- **nfsserve fork (Slice F — NFSPROC3_COMMIT + stable_how)**: WinFsp
  has its own commit/flush API (`Flush` callback, etc.). Different
  semantics entirely. If you have write-amplification issues, look at
  WinFsp's `FileSystemHostFlags` and the `Flush` callback config, not
  the nfsserve fork.
- **macOS-specific cleanups (Slice G's macOS bits)**:
  - `MacosNasWatcher` deletion — no Windows analog (Windows uses
    WinFsp's notification model, not FSEvents).
  - `fileprovider_domain_path` — was a Tauri-era FileProvider remnant.
  - `/Volumes/<share>` dedup-suffix handling — macOS NetFSMountURLSync
    quirk only.
  - Empty-mount-point-dir cleanup in `disconnect_drive` — Windows uses
    symlinks under `C:\Volumes\ufb\`, not mount-point directories.
- **NetFS stale-mount detection** (orchestrator's reconnect
  pre-flight): macOS-only quirk where kernel says mounted but SMB
  session is dead. Windows' `WNetAddConnection2W` failure modes are
  different — its session is either alive (returns ERROR_ALREADY_ASSIGNED)
  or it isn't.

## Lifecycle gotcha — known but unfixed on Windows

The macOS sync-toggle bug we just fixed had a Windows-side analog
that I haven't checked: when a config edit toggles `sync_enabled`
from true to false (or vice versa), `disconnect_drive` runs the
NEW config's branch against on-disk state from the OLD config.
On macOS that left `/Volumes/<share>` stuck mounted; on Windows it
might leave the symlink at `C:\Volumes\ufb\<share>` pointing at a
torn-down WinFsp mount, or fail to disconnect the SMB session.

Validate: in the Windows mount manager, take a non-sync mount, toggle
sync_enabled to true → save → toggle back to false. If state goes
weird, the disconnect_drive Windows section needs the same uniform-
unmount treatment as the macOS section got (use `self.mounted_at`
regardless of `self.config.is_sync_mode()`).

Reference: `agent/src/orchestrator.rs::disconnect_drive` macOS section
post-Slice-H — the unified path that doesn't branch on the post-
ConfigChanged config.

## Build / dev-loop gotchas (Windows side)

Cribbed from the macOS dev-loop memory entries:

- **No `.app` re-stage on Windows**: the agent locator searches
  `agent\target\release\ufb-agent.exe` directly (no `.app`-wrapped
  variant). So after every `cargo build --release` of the agent,
  no extra staging step is needed — the bare exe is what gets
  spawned. Unlike macOS where `sign-mac-dev.sh --only-agent` is
  required after every cargo rebuild.
- **WinFsp install dependency**: ensure WinFsp is installed +
  service running before launching. Pre-refactor logs mention
  `WinFsp install state` checks; same applies.
- **Tray heartbeat file**: Windows tray (`agent/src/tray.rs::windows_tray`)
  uses a different heartbeat mechanism than the Swift tray. Verify
  it's still aligned with whatever the orchestrator expects after
  the refactor.

## Quick smoke test sequence on Windows

After pulling, build, and launching UFB.exe:

1. **Lifecycle**: Stop a sync mount from the QML mount manager →
   pill turns Stopped, busy spinner shows + clears. Click Start
   → pill turns Mounting → Mounted. Repeat 5x. No "Orchestrator
   stopping" log line should appear between cycles.
2. **Config-toggle**: Edit `mounts.json`, flip a mount's `syncEnabled`,
   save. Should see `tearing down sync server` (if was sync) +
   `WinFsp sync server spawned` (if now sync) cleanly, no error.
3. **MountStateSnapshot**: Restart `UFB.exe` (not the agent). On
   relaunch, the mount manager should populate immediately with
   all configured mounts (not just slowly drip in).
4. **Write-through (after Slice H port)**: Write a moderate file
   to a sync mount, then `Get-FileHash` or open it in an app
   that re-reads. Should see `fully hydrated via write-through`
   in the agent log, and the re-read should be fast.

If anything weird, grep `~\AppData\Local\ufb\logs\ufb-agent.log`
(or wherever Windows puts the agent log) for the same patterns
the macOS log uses: `[winfsp]`, `[sync]`, `[mount]`, `[orchestrator]`,
`[<mount-id>]`.
