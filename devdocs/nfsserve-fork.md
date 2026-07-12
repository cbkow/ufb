# nfsserve fork (UFB)

We vendor a patched copy of [`nfsserve`](https://crates.io/crates/nfsserve)
under `external/nfsserve/` and force cargo to use it via
`[patch.crates-io]` in `agent/Cargo.toml`. Forked from upstream **0.10.2**
during Slice F of the mount + sync refactor (see
`macOSplans/11-mount-and-sync-refactor.md`).

## Why

Upstream 0.10.2 has two related shortcomings that murder write
throughput against an SMB-backed VFS:

1. **No `NFSPROC3_COMMIT` dispatch.** The handler dispatcher's `match
   prog` table doesn't include the COMMIT arm — every COMMIT the
   kernel sends back gets a `proc_unavail_reply`.
2. **WRITE response hardcodes `stable_how::FILE_SYNC`** regardless of
   what the client asked for. Combined with (1), this forces the macOS
   NFS client to treat every WRITE as fully sync'd — which means our
   `PassthroughFs::write` had to `f.sync_all()` over SMB per chunk.
   For a 100 MB save that's dozens of fsync round-trips to the NAS.

## What the fork adds

Diff vs upstream is intentionally small:

| File | Change |
|---|---|
| `src/lib.rs` | `mod nfs_handlers` → `pub mod nfs_handlers` so downstream impls can name `stable_how`. |
| `src/vfs.rs` | Added two trait methods on `NFSFileSystem` with safe defaults: `write_with_stable(id, offset, data, stable) -> (fattr3, stable_how)` and `commit(id, offset, count) -> writeverf3`. Default impls preserve pre-fork behaviour, so any existing impl Just Works. |
| `src/nfs_handlers.rs` | (a) `nfsproc3_write` calls `vfs.write_with_stable(...)` with the client's requested `stable` and echoes back the impl-reported `actual_stable` in the response. (b) New `nfsproc3_commit` handler + `COMMIT3args`/`COMMIT3resok` struct definitions + dispatch arm in `handle_nfs`'s match table. |

That's it — three files touched in `external/nfsserve/`.

## How the agent uses it

`agent/src/sync/nfs_server.rs` overrides `write_with_stable` and
`commit` on `PassthroughFs`:

- `write_with_stable` honors `UNSTABLE` by **skipping** the SMB
  `f.sync_all()`. Returns `UNSTABLE` so the client knows to follow
  with COMMIT. For `DATA_SYNC` / `FILE_SYNC` it fsyncs and returns
  `FILE_SYNC` (matching prior behaviour).
- `commit` fsyncs the SMB file (range arg advisory — SMB doesn't
  expose range-fsync) and returns the server verf. Takes the per-fh
  write lock so a concurrent UNSTABLE WRITE doesn't race.
- Legacy `write` is now a thin wrapper that delegates to
  `write_with_stable(..., FILE_SYNC)` to keep callers happy.

Net effect: writing a 100 MB file goes from "100× WRITE+FILE_SYNC
round-trips" to "100× WRITE (UNSTABLE) + 1× COMMIT" — same durability,
roughly an order of magnitude less SMB chatter.

## Upgrade procedure

To bump to a newer upstream nfsserve:

1. Copy the new upstream source into `external/nfsserve/`, replacing
   everything except this doc.
2. Re-apply the three diffs above. They're small enough to re-do by
   hand; if upstream restructured the handlers significantly the
   easiest path is to recreate `nfsproc3_commit` from the matching
   shape of `nfsproc3_write`.
3. `cargo check --manifest-path agent/Cargo.toml` to verify the
   `write_with_stable` signature on `PassthroughFs` still matches the
   trait. If upstream lands its own COMMIT support, the fork can be
   collapsed — bump the version in `agent/Cargo.toml` and delete
   `external/nfsserve/` + the `[patch.crates-io]` entry.
4. Keep the licence (Apache 2.0 in upstream) intact; `external/nfsserve/LICENSE`
   carries the original header.

## Why not upstream the fork

We will if the upstream maintainers want it. The PR shape is small
and the changes are backward-compatible (default-method impls keep
all existing downstream nfsserve users working). Until then, the
vendored copy keeps our maintenance burden bounded — three files,
~150 LOC of additions, no architectural changes.
