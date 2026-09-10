# macOS session primer — 1.2.0 (2026-09-10)

Written on the Windows dogfood box at the end of the 1.2.0 work session.
Everything below is committed on `main` (bump commit `bd9fbdb`, 25 commits
since the 1.1.6 appcast). All of it was built and verified on Windows;
**none of it has been compiled on macOS yet.** This is the map for the
Mac session that has to happen before 1.2.0 ships.

## 0. What 1.2.0 is

The lightbox player caught up with QCView-Player (copies, never shared —
see `devdocs/lightbox-preview-plan.md`, changelog entry 2026-09-10), plus a
UI pass. In order of risk to the Mac build:

| Area | Commits | Mac status |
|---|---|---|
| FFmpeg 9.0.1 cut-over + threading / swscale / routing / duration-less containers | `5c6ed48` | **unverified — build must be redone (§1)** |
| Audio sync servo (AudioPlayer, ring sizes, seekPending) | `79a7d8b` | unverified; CoreAudio device untouched |
| Alpha on CPU path, padded swscale target | `2740584` | unverified; touches VideoSurfaceItem (shared) |
| Rotation / pixel aspect / RGB legal range | `aadf715` | unverified; edits all three Metal-facing frags (§3) |
| Animated GIF/WebP → video player (`MediaInfo`) | `4cca02e` | needs Qt's qwebp/qgif plugins in the bundle (macdeployqt ships them) |
| D3D11VA zero-copy | `c8d764d` | Windows-only, `#if Q_OS_WIN`; `installD3D11DeviceHook` is a no-op elsewhere |
| Lightbox caption for video + close on minimize/hide | `40b4586` | QML only |
| Sidebar tones/rhythm, list row rhythm, breadcrumb tail, resize-guide fixes | `70660c1`…`3b787c1`, `96619c1`, `76554a7`, `a55b52f` | QML only |
| "Add Note" writes a real minNotes doc | `b0dcb5e` | Rust core; test in `core/tests/minnotes_note.rs` |
| EXR single named layer preview | `ff23549` | C++ thumbnails, platform-neutral |
| Revised project templates | `d73e273` | assets only |

## 1. FFmpeg 9.0.1 — do this first, the tree will not compile otherwise

New headers (`hw_routing.h` uses `AV_CODEC_ID_PRORES_RAW`, `sws_threaded.h`
uses swscale 10's public `SwsContext` fields) require the 9.0 headers.
The Mac vendors FFmpeg by building it from source, and
`scripts/setup-external-mac.sh` only rebuilds when `external/ffmpeg` is
**missing** — an existing 8.1.2 tree is silently kept. So:

```bash
rm -rf external/ffmpeg /tmp/ufb-ffmpeg-build
scripts/setup-external-mac.sh          # → build-external-ffmpeg-mac.sh @ 9.0.1
external/ffmpeg/bin/ffmpeg -version    # expect "ffmpeg version 9.0.1"
```

`build-external-ffmpeg-mac.sh` now pins `FFMPEG_VERSION="9.0.1"` and, right
after extracting the tarball, applies the three patches in
`scripts/ffmpeg-patches/` (`patch -d $SOURCE_DIR -p1 --forward`). The
patches dry-ran cleanly against the 9.0.1 release tarball on Windows (two
small offsets); watch the `[ffmpeg] applying …` lines. They are copies of
QCView-Player's `external/patches/ffmpeg/`: DNxHR 444 adaptive colour
transform + Avid legal-range tag, MXF RGBA range tag, ProRes RAW Bayer
patterns. Without them the app still works; DNxHR 444 / MXF RGBA just come
up untagged (no legal-range expansion).

Then the normal build (`cmake --preset mac-release` etc.). Expect **zero**
source changes needed for the cut-over on the audio side — the
`av_opt_set_array` abuffersink form and `pkt_timebase` are already in.
If something FFmpeg-9-specific does fail on clang, QCView's own macOS
cut-over is commit `50105c14` in `../QCView-Player`; diff its
`src/decode/video_decoder.cpp` against ours before inventing a fix.

## 2. What is Windows-only and should not be touched

`app/player/d3d11va_hw_device_ctx.*`, `app/player/d3d11/d3d11va_decode_bridge.*`,
`FrameHandle::Kind::D3D11` (declared in the header, defined only in
`frame_handle_nonapple.cpp`, never referenced outside `Q_OS_WIN`),
`VideoSurfaceItem::installD3D11DeviceHook` (no-op body on Apple),
everything Vulkan. `main.cpp` includes `player/VideoSurfaceItem.h` on all
platforms now — that is intended.

## 3. Verification checklist (lightbox, in the order most likely to break)

Run the app from Terminal so the Qt message handler's stderr lands in the
console (no `AttachConsole` dance needed on macOS). Test clips: generate
with the vendored `external/ffmpeg/bin/ffmpeg` the way the Windows session
did (recipes at the end of the changelog entry in
`lightbox-preview-plan.md`), or use real job files:
- ProRes 4444 with alpha: `261318_acura/postings/260909a_AlphasForLA/*.mov`
- Blender single-layer EXR: `261301_pmkn/3d/0210c_A_BoysAtMic/renders/0210c_A_BoysAtMic_v003/*_0008.exr`
- Multi-layer EXR: `261318_acura/3d/HeadTrack/renders/HeadTrack_v007/`

1. **ProRes 4444 alpha, play then scrub (K, then Q/E).** Metal zero-copy
   path is unchanged, but the CPU passthrough pipeline now blends; if VT
   ever hands back a non-zero-copy surface the readback frame must
   composite over the panel, not go opaque. Log: `VideoDecoder … METAL
   zero-copy publish` (or the VT→CPU fallback line).
2. **H.264 / HEVC via VideoToolbox.** Unchanged path but on new FFmpeg.
   Log: `get_format codec=h264 offered=[videotoolbox,…] picked=videotoolbox`,
   `threading …`. Play, step, Left/Right between clips, no stutter.
3. **Rotation + pixel aspect.** An iPhone portrait MP4 must display
   upright; an anamorphic clip un-squeezed. All three `.frag` files gained
   a `rotQ` UBO field + `rotatedSrcUv()`, and the CPU/Vulkan SRBs moved the
   texture to binding 1 — the Metal biplanar/AYUV SRBs already had the UBO
   at 0. Verify 90 and 270 explicitly (180 passes even with a wrong swap).
4. **Audio servo.** ProRes with audio ≥15 s: log `AudioPlayer: servo
   drift ±x ms ratio 1.000xx`, no `re-seeking audio` lines. Mute/unmute
   resumes at the current position. A WAV in the audio-only preview plays
   to the tail without seek thrash (EOF freeze).
5. **Animated GIF / WebP** open in the video player with a frame count;
   a static GIF/WebP stays on the image preview. If WebP always shows the
   still, the qwebp plugin is missing from `Contents/PlugIns/imageformats`.
6. **EXR**: the Blender single-layer file previews (log: `no bare R/G/B
   channels; using layer 'ViewLayer.Combined'`); the multi-layer file still
   shows the contact sheet.
7. **Odd-width clip** (portrait 1080-wide H.264) scrubs without a crash.
8. **Minimize with a video playing** → audio stops, lightbox gone on
   restore. Close-to-tray equivalent on macOS is Cmd+W → same.
9. **Add Note** in a job's docs tab → the `.mndb` opens in minNotes with an
   empty paragraph (minNotes is the sister repo `../minNotes`).
10. Sidebar (panel tone, dividers, glyph colours), breadcrumb tail,
    column resize guide never sticks after release.

## 4. Release

Version is already bumped (7 pins + Cargo.lock; `bump-version.sh` no
longer lists the retired UFBTray plist). Sequence per
`windows-agent-build-setup` memory / `scripts/release-mac.sh`:

1. Mac: `scripts/release-mac.sh` → `dist/UFB-1.2.0-<arch>.dmg`.
2. Windows (this box): the installer was **not** built yet — run the
   streamlined sequence after the Mac session lands any fixes, since Mac
   fixes may touch shared code. Remember to delete stale `av*-6x.dll`
   from `build/release` if a fresh configure ever re-globs (the 8.1.2
   DLLs were removed once already).
3. `scripts/make-appcast.sh` on the Mac, then the `appcast: 1.2.0
   (mac + win)` commit to `docs/`.

Commits are local to the Windows box's clone until pushed.
