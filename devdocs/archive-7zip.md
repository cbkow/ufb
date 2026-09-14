# Archive extract / compress via bundled 7-Zip

Added 2026-09-11 (shipped 1.2.2). "Extract Here" and "Compress to ZIP"
in the file-browser context menus, jobs in the Task Queue tab, progress
chips in the footer.

## Pieces

| Layer | File | Notes |
|---|---|---|
| Engine | `core/src/archive.rs` | Sequential worker, one 7-Zip child at a time; unit tests + a live end-to-end test (`UFB_7ZIP=external/7zip/bin/7zz cargo test -p ufb-core --lib archive::live -- --ignored`) |
| Binding | `bindings/src/services/archive.rs` | `Archive` singleton: `add_extract_jobs`, `add_compress_job`, cancel/remove/clear, `queue_json`, signals `job_completed(path)` then `dirs_changed(json)` |
| UI | `FileBrowser.qml` (menu items), `TaskQueue.qml` (queue tab, shared with transcode), `TaskJobsModel.qml` + footer chips in `Main.qml` | Queuing never switches tabs |
| Refresh | `FileOps.notify_dirs_changed` | Main re-broadcasts `Archive.dirs_changed` so every view refreshes like after a copy/move |

## The binary

- **macOS:** `external/7zip/bin/7zz`, universal, **built from the official
  source tarball** by `scripts/build-external-7zip-mac.sh` (called from
  `setup-external-mac.sh`). Do NOT ship the prebuilt `7zNNNN-mac.tar.xz`:
  it is stamped `minos 26.0` (build machine's SDK), fails
  `release-mac.sh`'s minos gate and won't load on macOS 14/15. The
  makefile only threads `LOCAL_FLAGS` into compile steps, so the pin is
  `MACOSX_DEPLOYMENT_TARGET=14.0` in the environment. ~2 min for both
  arches. Copied into `UFB.app/Contents/MacOS/` beside ffmpeg; signed by
  `sign-mac-dev.sh`'s Contents/MacOS sweep.
- **Windows:** `external/7zip/bin/7z.exe` + `7z.dll` (the console tool
  needs the DLL for RAR; the "extra" package's `7za.exe` reads no RAR).
  `setup-external.ps1` copies them from an installed 7-Zip in Program
  Files, else downloads the official installer and unpacks it with the
  built-in `tar`. Copied next to `ufb.exe`; listed in `ufb_installer.iss`.
- Resolution at run time: `bundled_tool("7zz")` / `("7z")`, then PATH
  (`7zz`, `7z`, `7za`), so dev checkouts without `external/` still work.

## Behaviour contract (keep)

- Extract: never overwrite; single root entry → straight into the
  parent, else `<stem>`, `<stem> 2`, …; compound tar via
  `x -so | x -si -ttar`; encrypted → "password-protected" error;
  cancel/fail → remove what the job created (always a path that did not
  exist at plan time).
- Compress: zip beside the **shallowest** selected item (never inside a
  selected folder); lone item → `<name>.zip`, several → `Archive.zip`;
  one `7z a` per source folder (flat) unless basenames collide (then
  relative to the common ancestor); `-xr!.DS_Store`.
- Switches: `-y -p -aou -bso0 -bse2 -bsp1 -sccUTF-8 --`; exit 1 =
  Completed-with-warning, ≥2 = Failed; stderr summarised to the first
  `ERROR:` line.

## Licensing

7-Zip is LGPL-2.1+ with the **unRAR license restriction** (the RAR
decoder may not be used to build a RAR *creator* — UFB extracts RAR and
only ever creates ZIP). Texts: `LICENSES/SevenZip-LICENSE.txt` (7-Zip's
own notice) + `LICENSES/LGPL-2.1.txt`; entry 4b in
`THIRD_PARTY_NOTICES.txt` records the version and source. Each release
attaches the exact source tarball via `scripts/fetch-third-party-sources.sh`.
Bump `SEVENZIP_VERSION` in the build script, the version in
`setup-external.ps1`, `fetch-third-party-sources.sh` and the notices
entry together (CVE fixes land in 7-Zip point releases).

## Follow-ups

- Password prompt for encrypted archives (currently a readable failure).
- Symlink policy inside archives (7-Zip defaults today).
- Windows `tar -xf` unpack of the SFX installer is an untested fallback;
  the Program Files path is what the win box actually exercised.
