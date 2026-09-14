# Windows live-sync (WinFsp) audit — 2026-09-11

Fresh full audit of the Windows on-demand sync mount path, run on the NY
dogfood box (`NY-GFX03-ACECA`) against the live `Jobs_Live` mount at `U:`,
cache root `D:\z_UFB`, agent `v1.2.0`, WinFsp `2.1.25156`. Measurements are
from real I/O through the mount and from the live cache DB, not code
reading alone.

Companion to `devdocs/windows-mount-sync-audit.md` (the port-from-macOS
checklist). That doc tracked whether the shared-refactor fixes reached
Windows; this one measures the data path end to end and finds what the
checklist did not cover.

## Architecture (as built)

- **Agent owns sync mounts only.** Plain drive mounts (F/G/H) are
  GUI-owned WNet mappings (`core/src/windows_mounts.rs` +
  `bindings/src/local_mounts.rs`). Sync mounts (only `Jobs_Live`/`U:`
  here) run through the agent: an FSM (`agent/src/state.rs`) → orchestrator
  (`agent/src/orchestrator.rs`) → `winfsp_server::start`.
- **WinFsp passthrough VFS.** `agent/src/sync/winfsp_server.rs`
  (`PassthroughFs`) mounts straight to a drive letter and answers each
  callback: metadata + enumeration from a per-domain SQLite cache
  (`agent/src/sync/windows_cache.rs`, `CacheIndex`) when warm, live SMB on
  the `\\?\UNC\...` backing root otherwise. Content is a 1 MiB-chunk
  block cache in `{cacheRoot}\by_key\{domain}\{rowid}`.
- **Heartbeat** (30 s) probes the mount with a real `read_dir` and feeds a
  shared `NasHealth` atomic; every SMB-touching callback fast-fails with
  `STATUS_DEVICE_NOT_CONNECTED` while offline instead of parking a
  dispatcher thread on the redirector timeout.

The lifecycle layer is genuinely solid: readiness gating on the dispatcher
thread, error-state auto-retry with exponential backoff, sleep/wake
detection, deterministic teardown ordering (`TeardownSyncServer` before
`DisconnectDrive`), orphan-prune and suspicious-empty guards, per-domain
blob namespacing, and the NOCASE path-key migration are all correct and
well-commented. The defects below are all in the **data path**.

## Measured baseline (this box)

Cache DB `D:\z_UFB\Jobs_Live.db`: 44,390 rows, 5,918 dirs, 6,274 fully
hydrated (18.8 GB), 5,622 partially hydrated. Blob store on disk: 11,896
blobs, **104.5 GB allocated for 18.8 GB of real cached content.**

| Operation | Through `U:` | Direct UNC | Ratio |
|---|---|---|---|
| Write 200 MB (Explorer copy, buffered) | 240 s* | 0.31 s (639 MB/s) | — |
| Write 20 MB, 64 KB buffered writes | 241 s* | — | — |
| Write 2 MB, 64 KB buffered writes | 240 s* | — | — |
| Write 20 MB, 1 MiB buffered writes | 0.14 s (139 MB/s) | 0.03 s | ~5× |
| Write 2 MB, 64 KB write-through | 0.002 s | — | — |
| Read 718 MB cold (chunk hydrate) | 4.05 s (177 MB/s) | 0.07 s | — |
| Read 718 MB warm (hydrated) | 0.50 s (1.4 GB/s) | — | par |
| Read 200 MB warm, 4 KB app reads | 22.5 s (8.9 MB/s) | 0.28 s (723 MB/s) | ~80× |
| Read 200 MB warm, 64 KB app reads | 1.35 s (149 MB/s) | — | — |
| Read 200 MB warm, 1 MiB app reads | 0.17 s (1.2 GB/s) | — | par |

The read path is excellent once the app reads in ≥64 KB blocks.
\* The buffered-write rows all hang exactly 240 s **at close**, independent
of size — a fixed timeout, not a rate (see finding 1). Write-through and
large buffered writes close instantly.

## Blocking

### 1. Buffered-write files hang ~240 s at close and wedge the file — FIXED 2026-09-11

**Status: fixed and verified live.** The open context now holds a single
backing handle opened with the granted access in `open`/`create`, reused
by `read`/`write`/`flush`, replacing the per-callback fresh SMB handle
(and the redundant read handle held alongside it). After the fix, the
same repro closes in **0.012 s** (was 240 s), the file is immediately
reopenable and overwritable (no more wedge), and a 200 MB buffered copy
lands in 0.67 s with a matching MD5 through both `U:` and the UNC path. So
the second open handle during cleanup was indeed what blocked the
flush-and-purge. Ships in agent ≥1.2.1. Original analysis below.



This is one bug, not the two I first split it into. A file that received
**buffered (cached) writes** stalls for a fixed ~240 s when the app closes
it, and afterward the file cannot be reopened, overwritten, or deleted for
10+ minutes ("The requested operation cannot be performed on a file with a
user-mapped section open").

Step-timed repro (2 MB written three ways, same total):

| Write style | close() | reopen |
|---|---|---|
| 32 × 64 KB buffered, flush at close | **240.0 s** | wedged (user-mapped section) |
| 2 × 1 MiB buffered | 0.001 s | ok |
| 32 × 64 KB write-through (unbuffered) | 0.002 s | ok |

The 240 s is a **fixed timeout, not throughput**: 200 MB, 20 MB, and 2 MB
buffered writes all took exactly 240 s. So the individual `write` callbacks
are not the cost. What stalls is **cleanup/close of a file with a dirty
memory-mapped section**. `VolumeParams` sets
`flush_and_purge_on_cleanup(true)`; at cleanup WinFsp asks the OS to flush
and purge that section, and the purge blocks — most likely because
`OpenCtx::File` **still holds a second handle open** on the same path (the
read handle opened in `open`), so the section can't be torn down until a
~4-minute timeout elapses. Buffered large writes (B) and write-through (C)
never build the small-write dirty section, so they skip it. Explorer copies
and most app saves are buffered small writes — the common case is the slow
one.

Secondary: `write()` also opens a fresh SMB handle per callback and the
write-through cache mirror never sets a chunk bit (it only marks
fully-covered 1 MiB chunks, never true for sub-1 MiB callbacks), so files
written through `U:` come back `is_hydrated=0`/`chunk_bitmap=NULL` and
re-fetch from the wire on next read. Inefficient, but not the 240 s cause.

**Fix:** open **one** backing handle with the granted access in
`open`/`create`, store it in `OpenCtx::File`, reuse it for read and write,
and drop the separate read-only handle — so nothing holds the section open
at cleanup. Then re-test with `flush_and_purge_on_cleanup` toggled to
confirm the purge completes. For the mirror, track written byte ranges and
set a chunk bit once its whole logical range is covered across callbacks.

**This supersedes the earlier "writes run at 0.8 MB/s" framing** — the
0.8 MB/s number was the 240 s close stall divided into the file size, not a
per-operation rate.

### 2. Cache blobs are not sparse; the budget ignores partial blobs — FIXED 2026-09-11

**Status: fixed and verified live.** Blobs are now marked sparse
(`FSCTL_SET_SPARSE`) before the `set_len(nas_size)` pre-extend at both
creation sites, so only fetched chunks occupy disk. Verified: a 500 MB
file read for its first 128 KB produced a blob that is flagged sparse and
allocates **2.00 MB on disk** (was 500 MB). The evictor now sizes and
evicts partial blobs too — its budget query covers `is_hydrated=1 OR
chunk_bitmap IS NOT NULL` and sizes partials by `popcount(bitmap) *
CHUNK_SIZE` (capped at nas_size) instead of counting only fully-hydrated
rows. Ships in agent ≥1.2.1.

Caveat: this box's ~5,100 pre-existing partial blobs were written
non-sparse and stay fully allocated until they are re-fetched (which
writes a fresh sparse blob) or the cache is cleared/drained. New blobs
are sparse from here on; no retro-sparse pass was added (deallocating
already-written ranges would mean rewriting each blob). Original analysis
below.



Blob files are created and `set_len()` to `nas_size` — on NTFS that
**allocates the full length**. A 2.9 GB movie with one 1 MiB header chunk
fetched occupies 2.9 GB on disk (verified: `fsutil sparse queryflag` =
"NOT sparse", one allocated range spanning the whole file). Across 5,622
partially hydrated files that is ~86 GB of disk reserved for a few MB of
real data.

The evictor cannot claw it back: `total_cached_bytes()` and
`evict_over_budget_now()` both filter `is_hydrated=1` and sum
`hydrated_size`, so partial blobs are invisible to the 50 GiB default
budget. On `D:` with 3.5 TB free this went unnoticed; on a laptop `C:` it
fills the disk with no eviction ever firing.

**Fix:** `FSCTL_SET_SPARSE` on blob creation before `set_len` (the read
path already relies on sparse-hole-reads-as-zeros semantics, so the intent
was there — the flag is just missing). Count real allocated bytes (or
bitmap popcount × chunk size) in the budget, and let the evictor consider
partially-hydrated rows.

## Correctness

### 3. Two path-key formats coexist in the cache DB — FIXED 2026-09-11

**Status: fixed and verified live.** `rel_from_abs` now trims the leading
separator that verbatim `strip_prefix` leaves; a one-time
`migrate_path_key_normalize` (gated on the `path_key_norm` flag,
NOCASE-aware dedup keeping the hydrated row) collapsed the live DB from
44,411 → 37,342 rows with zero hydrated bytes lost (6,279 files / 19,931 MB
intact), and a fresh write through `U:` now produces exactly one no-slash
row. Ships in agent ≥1.2.1. Original analysis below.



`rel_from_abs()` strips the canonicalized `\\?\UNC\server\share` prefix and
is left with a **leading separator**, producing keys like `/job/file.mov`.
`rel_path()` (from the `U16CStr` name) trims leading separators, producing
`job/file.mov`. The two are used by different callbacks:

- slash-prefixed: `read`, `write`, `overwrite`, `cleanup`, `get_file_info`
  (via `rel_from_abs`)
- no-slash: `get_security_by_name`, `open`, `create`, `rename`
  (via `rel_path`)

Live DB: **28,666 rows with a leading slash, ~15,700 without** — every file
touched through the mount exists under both keys, one of them a size-zero
ghost that no prune removes. Consequences:

- `get_security_by_name` never hits the warm path (always stats SMB).
- `rename` updates the no-slash row while reads key off the slash row: the
  rename-over-existing dedup protection (the editor atomic-save guard the
  code comments describe) targets the wrong key. An atomic save can leave
  the old hydrated blob serving pre-save bytes until the folder is next
  enumerated.

**Fix:** make `rel_from_abs` strip the leading separator so both formats
match `rel_path` (canonicalize to no-slash, root = `""`). One-time
migration: strip the leading `/` from `known_files.path`/`parent_path` and
`visited_folders.nas_path`, dedupe collisions keeping the hydrated row.

### 4. File metadata has no freshness path of its own — FIXED 2026-09-11

**Status: fixed and verified live.** The existing `stat_and_refresh`
primitive keys by full path and never matched the WinFsp rel keys, so a
rel-keyed reconcile was wired in instead. `open` already pays an SMB
stat, so it now calls `reconcile_drift`: on a size/mtime change from the
cached row it refreshes the cached metadata and drops the block cache, so
reads on that handle see the new length and re-hydrate. `get_file_info`
does the same behind a `last_verified_at` TTL gate (`FRESHNESS_TTL_SECS`,
5 s) so a held-open handle picks up peer edits without a stat storm.
Verified with no directory listing: a peer overwrite (same size, new
mtime) is read back correctly through `U:`, and a peer append is seen at
the new length. Ships in agent ≥1.2.1. Original analysis below.



A peer appended 1 MB to a file via UNC. Through `U:`, `open` returned the
new size but reads stayed bounded by the **cached** size (old length), and
the reported length was still stale 12 s later. Only a directory
enumeration refreshes a file row, and only when the folder actually
re-enumerates. The purpose-built `stat_and_refresh()` /
`needs_verification()` TTL primitive in `windows_cache.rs` is **never
called** by any callback.

Same-file atomic-save coherence (test c): after `MoveFileEx(REPLACE_EXISTING)`
of new content over a hydrated file, a read through `U:` **without an
intervening listing** returned the OLD content, still old after 11 s, and
only corrected after a directory listing forced re-enum + drift-invalidate.
For an editor that writes tmp+rename and immediately re-reads, that is a
stale read of its own save.

**Fix:** call `stat_and_refresh` (short TTL, e.g. 3–5 s) from
`get_file_info` and `open`; on drift, update size/mtime and invalidate the
block cache so the next read re-hydrates. This is the file-level analog of
the folder-mtime re-enum that already works.

## Should fix

- **Small-read / small-write cliff.** 4 KB reads → 8.9 MB/s, 64 KB writes →
  0.1 MB/s. Each read callback does two SQLite queries + a `touch_by_rel`
  UPDATE + a `CreateFile` on the blob. Keeping the blob handle in
  `OpenCtx` (fix 1) and rate-limiting the LRU `touch` (e.g. once per N s
  per fh) removes most of it. Raising WinFsp's read/write transfer size in
  `VolumeParams` would also help the app-block-size sensitivity.

- **Classic Windows delete fails ("Incorrect function") — FIXED 2026-09-11.**
  Surfaced once fix 1 let files reach a normal (non-wedged) state. The
  real cause was NOT the POSIX flag (a red herring — flipping it changed
  nothing): the winfsp 0.12 crate's default `set_basic_info` returns
  `STATUS_INVALID_DEVICE_REQUEST`, and `del` / `Remove-Item -Force`
  clear the file's attributes via `set_basic_info` BEFORE deleting, so
  every classic delete aborted there (cleanup then fired with
  `pending_delete=false`). Fix: implement `set_basic_info` — apply
  attributes via `SetFileAttributesW` (path-based, works for files and
  dirs) and timestamps via `SetFileTime` on the reused handle,
  best-effort, always returning Ok. Verified: `Remove-Item -Force`,
  read-only-file delete, `.NET File.Delete`, and rename all work.
  Residual below.

- **cmd.exe `dir`/`del` exact-name + `Remove-Item -Recurse` — OPEN.**
  A separate, pre-existing enumeration quirk: `FindFirstFileW` with an
  exact name (what cmd `dir`/`del` and recursive `Remove-Item` issue)
  finds nothing, while `Get-ChildItem`, `Test-Path`, `Remove-Item -Force`
  and `.NET File.Delete` all work. Diagnostic logging proved our
  `read_directory` returns the matching entry (matched=1), and it made no
  difference whether WinFsp did the pattern match
  (`pass_query_directory_pattern(false)`) or we did — the entry never
  reached the FindFirst client. Points at a cmd-specific / 8.3-short-name
  behavior of the passthrough (we advertise no short names), not the
  delete path. Left as-is; needs a WinFsp-level investigation.

- **Single-strike offline.** One failed 30 s heartbeat flips the whole
  mount offline and fast-fails all SMB ops for up to 30 s. A busy NAS
  during a render push reads as an outage. Use a 2-of-N failure threshold
  (the deleted `NasConnectivity` had `FAILURE_THRESHOLD`) and trip offline
  reactively from callback SMB errors, not only from the tick.

- **Log spam eats history.** With the GUI closed the agent logs
  `Failed to forward to UFB: sending on a closed channel` every 2 s — 187
  of 224 lines in the current log. The file truncates at 2 MB, so real
  history is gone within hours. Gate the periodic `emit_state_update` on a
  connected IPC client. `read_directory` also logs one INFO line per
  listing — move to DEBUG.

- **Dead / misleading code.** `core/src/sync_aware.rs` still targets
  `C:\Volumes\ufb` and the Cloud Files API, so its instant in-root
  move/copy never triggers on the WinFsp letter — a copy inside `U:` reads
  and rewrites through the mount at the speeds above instead of an SMB
  server-side copy. `handle_freshness_sweep` is a no-op on Windows.
  `windows_cache.rs` carries a file-wide `#![allow(dead_code)]` hiding the
  unused freshness primitive (fix 4).

## Working well (validated, no change)

Readiness gating, error auto-retry backoff, sleep/wake reset, orphan +
suspicious-empty prune guards, 60 s prune grace window, per-domain blob
namespacing, NOCASE migration, conflict sidecars that refuse to truncate on
copy failure, drift-detect-on-reenum with cache invalidation (observed
firing correctly in the log), warm read throughput at native speed. NAS
clock matched local time exactly, so the local-mtime-vs-SMB drift concern
is theoretical at this site.

## Suggested fix order

1. Fix 3 (path-key unification + migration) — small, unblocks correct
   warm metadata and rename safety, prerequisite for trusting the cache.
2. Fix 1 (single reused handle in OpenCtx) — resolves both the 240 s
   close hang and the user-mapped-section wedge; re-test with
   `flush_and_purge_on_cleanup` toggled to confirm.
3. Fix 2 (sparse blobs + budget accounting) — prevents disk-fill on laptops.
4. Fix 4 (file-level freshness TTL) — closes the stale-own-save window.
5. Hygiene: log gating, offline threshold, dead-code removal.

## Reproduction

Test scripts live in the session scratchpad (`wbench*.ps1`, `rbench.ps1`)
and drive `U:\z_CBtemp\_ufb_audit_tmp` (Chris-approved scratch folder).
Cache DB queried directly with `sqlite3 D:\z_UFB\Jobs_Live.db`. Sparse
check: `fsutil sparse queryflag D:\z_UFB\by_key\Jobs_Live\<rowid-hex>`.

## Cross-platform reconciliation with the macOS audit (2026-09-14)

The macOS live-sync audit landed on `main` first (927d0e3). This Windows
branch was rebased onto it. Key facts for the joint merge:

- **File sets are disjoint.** The Windows fixes touch only Windows-only
  files (`winfsp_server.rs`, `windows_cache.rs`, `ipc/server.rs`,
  `Cargo.toml` windows block) plus `UfbMenu.qml` and the shared
  `cache_core.rs` (additive helpers only). The macOS commit changed the
  shared orchestrator/state/config/mount_service/mount_client and the
  macOS twins. No file is edited by both — the rebase was conflict-free.
- **The macOS commit's Windows `cfg` arms compile.** It added Windows
  arms it could not build on a Mac (deferred WinFsp start failure,
  Handoff/release_drive, GetDiskFreeSpaceExW cache clamp, `load_config`
  returning Result, persist_drive_letter, GUI mount-service heal). Agent
  and full app both build clean on Windows after the rebase.
- **Cache-twin bugs ported** (macOS is the reference; see the commit
  "port macOS cache-audit twin fixes"): LIKE escaping (C-2), case-only
  rename source-row deletion (C-3/C-4b), rename conflict sidecar (C-4a),
  drift-adopted-before-invalidation, evictor partial-blob accounting via
  the shared `bitmap_cached_bytes`, and the orphan-prune per-key lock.
  Verified live: renaming `shot_010`→`shot_020` leaves `shot-010_*`
  siblings untouched (the LIKE trap), cascade re-paths descendants, 0
  slash rows.
- **Shared helpers** `like_escape` / `like_prefix` / `bitmap_cached_bytes`
  now live in `cache_core.rs`. macOS still has identical local copies in
  `macos_cache.rs`; **at merge, dedup those to the shared ones.**

### Deferred to the joint review
- **`db()` pool-exhaustion panic** (`windows_cache.rs`): still
  `expect("SQLite pool exhausted")`. macOS made its `conn()` return a
  Result. The Windows equivalent is a ~50-callsite refactor; low
  frequency at POOL_SIZE=32. Do it in coordination.
- **`stat_and_refresh`** in `windows_cache.rs` is dead (never called;
  the freshness path uses `reconcile_drift`). Delete or wire it.
- **cmd.exe exact-name enumeration** (`dir`/`del`, `Remove-Item -Recurse`)
  — the open FindFirstFile/8.3-short-name residual noted above.

### Still to verify on Windows (macOS reviewer's list)
- 7-Zip Extract/Compress — smoke-tested working earlier this cycle.
- Web links (.url create, double-click, glyph, clipboard) — pending.
- Menu icons + footer chips at 150% scaling — pending.
