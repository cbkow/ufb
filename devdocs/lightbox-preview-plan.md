# In-app Lightbox / QuickLook preview (images · video · PDF · text)

> **Living document.** Updated as the feature is built — each milestone appends to
> the [Status / changelog](#status--changelog) at the bottom (date, what landed,
> decisions, deviations, follow-ups). This is the inherited context for the
> eventual **Windows build session (M4/M5)**: it must be able to see every step
> taken on macOS and why. Keep it current as part of each milestone's commit.
> Borrowing reference: sister app QCView-Player at
> `../QCView-Player` (a sibling checkout) (code borrowed, nothing shared).

## Context

UFB has no way to preview a file without opening it in an external app. We want
a spacebar-triggered, **in-app** (window-constrained) lightbox — like Finder's
QuickLook — that previews the current selection and lets the user arrow through
the listing. It must cover everything UFB shows: images (incl. EXR/HDR/PSD),
**video with real playback + scrubbing**, and **PDF** (continuous-scroll
reader), plus cheap plain-text/Markdown. Anything else falls back to the grid's
thumbnail/icon — so there is no "supported vs unsupported" index to maintain.

The video player is the hard part. We borrow heavily (code, not a shared lib)
from **QCView-Player**: its FFmpeg decode pipeline, scrub decoder, zero-copy GPU
paths, and transport. We are **SDR-only with no OCIO and no annotations**, which
lets us drop QCView's entire color-management and review-overlay machinery and —
critically — its **native player window** (that existed only for HDR/custom-
colorspace swapchains we don't need). Everything renders into an in-app
`QQuickRhiItem`.

Decisions locked with the user:
- **Zero-copy GPU from day one**, both platforms (no CPU-upload interim).
- **macOS:** VideoToolbox → Metal. **Windows:** D3D11VA for non-ProRes, **FFmpeg
  8.1 Vulkan** decode for ProRes; software fallback. (Qt's QRhi stays **D3D11**
  on Windows — its default — so no global Vulkan switch; Vulkan is a decode-only
  device, as in QCView.)
- **Audio:** yes, video-driven drift-corrected sync (QCView's model).
- **Transport + scrub**, seekbar (not a full timeline). Space opens; **Esc
  closes** (+ top-right close icon); Space = play/pause once a video is open;
  Left/Right switch media; port QCView's transport shortcuts.
- **PDF:** continuous-scroll reader; Up/Down, PageUp/PageDown, Home/End.
- Images: **fit-to-window** for v1 (no zoom/pan). Markdown/plain-text included
  (Qt-native); **RTF/Office deferred to v2** (they hit the thumb/icon fallback).
- **Vendor FFmpeg 8.1** (Vulkan + libplacebo + shaderc). Code lives in UFB,
  independent of QCView (borrow a lot, share nothing).

## Architecture

New independent module **`app/player/`** (C++), exposed to QML as two objects,
composed by one QML overlay:

- **`VideoController`** (QObject, registered via `qmlRegisterType`/`QML_ELEMENT`):
  owns the ported decode stack — streaming `VideoDecoder` (latest-wins publish
  slot), `ScrubDecoder` + `ScrubFrameCache` (the GOP cache that makes scrubbing
  instant), `FrameIndex` (PTS↔frame), `AudioPlayer`. Exposes `open(path)`,
  `close()`, `play/pause/togglePlayback`, `seekToFrame`, `stepFrames`,
  `scrubToFrame`, `setVolume/setMuted`, and properties
  `playing/position/duration/frameCount/hasAudio`.
- **`VideoSurfaceItem : QQuickRhiItem`** (Qt 6.7+; UFB is 6.11): its renderer,
  each frame, pulls the current `FrameHandle` from the controller and turns it
  into a `QRhiTexture`, then draws a fitted quad. Platform frame→texture detail
  below.
- **`PreviewLightbox.qml`** (in `Main.qml`, parented to `Overlay.overlay`, top
  z): dark full-window layer; routes the current path to one of `ImagePreview` /
  `VideoPreview` / `PdfReader` / `TextPreview` / `ThumbIconView` (fallback);
  hosts the seekbar, transport bar, close icon, spinner; owns key handling.

**Graphics backend:** pin explicitly at startup for safety —
`QQuickWindow::setGraphicsApi(Metal)` on macOS, `Direct3D11` on Windows (both
already the Qt defaults; `app/main.cpp` currently sets only `QSurfaceFormat`).

### Per-platform zero-copy frame → QRhiTexture

The renderer gets Qt's native device from `rhi()->nativeHandles()`.

- **macOS (Metal):** port QCView's `ZeroCopyBridgeMetal` /
  `cv_pixbuf_metal_bridge.mm` — already window-decoupled, needs only an
  `MTLDevice` (use Qt's). Wraps `CVPixelBuffer` planes as `MTLTexture`s via
  `CVMetalTextureCache`, returned as `QRhiTexture`s; a ported YUV→RGB shader
  (`yuv_to_rgb_biplanar.frag` for NV12/P010/P210, `yuv_to_rgb_ayuv.frag` for
  ProRes 4444) → RGBA16F. Template: `player_rhi_item.cpp:384–719`.
- **Windows (D3D11), zero-copy on Qt's device:** inject Qt's QRhi `ID3D11Device`
  into the decode side so frames land on Qt's device:
  - **non-ProRes:** FFmpeg D3D11VA `AVHWDeviceContext` wrapping Qt's device
    (`av_hwdevice_ctx_alloc(D3D11VA)` + set `AVD3D11VADeviceContext.device` +
    `av_hwdevice_ctx_init`). NV12 `ID3D11Texture2D`s get native plane SRVs (R8 /
    R8G8) → YUV→RGB → RGBA16F → `QRhiTexture::createFrom`.
  - **ProRes:** decode-only Vulkan device (port `vulkan_device_manager` +
    `vulkan_hw_device_ctx`); port `d3d11_vulkan_decode_bridge` +
    `d3d11_vulkan_yuv_compositor` **initialized with Qt's D3D11 device** (add an
    `initializeWithDevice()` path — consumers already hold the device as
    `void*`). The bridge outputs a **standalone** RGBA16F `ID3D11Texture2D`
    (NT-handle Vulkan↔D3D11 interop) → `QRhiTexture::createFrom`.

### Code to borrow from QCView (strip OCIO / annotations / timeline / dual)

`src/decode/`: `video_decoder.{h,cpp}` (decode loop + `performSeek` +
`initFFmpeg` hwaccel selection at `:111–177, :531–863, :1329–1429`; drop
timecode/OCIO), `frame_handle.{h,mm,_nonapple.cpp}`, `frame_index.{h,cpp}`,
`scrub_decoder.{h,cpp}`, `scrub_frame_cache.{h,cpp}`,
`decoder_cleanup_queue.{h,cpp}`. `src/render/`: Metal zero-copy bridge,
D3D11/Vulkan decode bridge + yuv compositor + `d3d11_device_manager` (add
injection), YUV `.frag` shaders. `src/audio/`: `audio_player.{h,cpp}` +
`AudioDecoder` + `CoreAudioDevice`/`WasapiAudioDevice`.
**Leave behind:** `src/color/` (OCIO), `src/annotations/`, `src/timeline/`,
`src/project/`, dual-compositor/safety-overlay, native window/swapchain, the
inspector/window UI.

## Per-type renderers (QML, in the overlay)

Routing = "rich renderer if type matches, else fallback" (extension sets mirror
`Thumbnailer.cpp:26–84`):
- **ImagePreview** (`jpg/png/tiff/…/exr/hdr/rgbe/psd/psb`): full-res via the new
  preview decode path; `Image` fit-to-window.
- **VideoPreview** (`videoExts`): `VideoSurfaceItem` + `VideoController` +
  transport/seekbar.
- **PdfReader** (`pdf/ai`): continuous scroll.
- **TextPreview** (`txt/md`): `Flickable` + `Text { textFormat:
  path.endsWith('.md') ? Text.MarkdownText : Text.PlainText }`, via a small
  size-capped file reader.
- **ThumbIconView** (everything else): `Image { source:
  "image://ufb-thumbs/"+path }` + `FileTypeIcon` fallback — identical to the grid
  cell (and benefits from the status-gated fallback shipped 2026-06-02).

**Full-res image path:** existing backends (`decodeExr/decodeHdr/decodePsd`,
`QImageReader`) already take a `requestedSize` — call with the view size × DPR
instead of 512. Add a **`ufb-preview`** async provider that **bypasses the 512
master cap and relaxes the 64 MP / 256 MB decline**, guarded by a decoded-output-
bytes ceiling (~1.5 GB → graceful "too large to preview"), on the worker pool,
spinner until ready.

**PDF reader:** add `ufb-pdf` async provider keyed by `path+page+width`
(mutex-serialized like `PdfBackend`, extended with `FPDF_GetPageCount` +
`FPDF_LoadPage(n)` + `FPDF_RenderPageBitmap`). Viewer is a `ListView` (one
delegate per page, each an `Image` from `ufb-pdf`) — only visible pages render,
offscreen bitmaps evict → continuous scroll, bounded memory.

## Memory management

Principle: **one media pipeline alive at a time; close returns to baseline; no
prefetch in v1.**
- **Switch/close teardown** routes the old decoder through the ported
  `decoder_cleanup_queue` (stop decode thread → background join + free FFmpeg/HW
  contexts) so HW frame-pool destruction never stalls the UI/render thread.
- **GPU textures** released on the **render thread** (the `QQuickRhiItem`
  renderer nulls them → QRhi deferred-release); dropping a `FrameHandle` releases
  its native handle (`CFRelease` / `av_frame_unref` / D3D11 ref).
- **Bounded caches:** `ScrubFrameCache` byte-bounded LRU (~256–512 MB); PDF page
  bitmaps bounded by ListView visibility; image previews freed on switch.
- **Guards:** large-image refusal by decoded-output bytes (graceful fallback,
  not OOM); video HW pools freed promptly on every switch (Windows VRAM).
- Closing the lightbox tears down decoder + audio + caches + textures.

## Overlay UX

- **Open:** Space in `FileBrowser._handleKey` (guard against active filter/text
  input) → `window.openPreview(currentPath, paneRef)`.
- **Close:** Esc (always) + top-right close icon. Space closes for non-video;
  Space = play/pause for video.
- **Switch media:** Left/Right → move the source pane's cursor and re-open.
- **Keys dispatched to the active renderer** (context-sensitive): video gets the
  ported transport set (play/pause, Q/E step, A·J / D·L fast-seek, M mute,
  Up/Down volume, V loop, Home/End seek start/end); PDF gets scroll
  (Up/Down/PageUp/PageDown/Home/End). Map ported from
  `window_manager.cpp:5040–5165`.
- **Spinner:** QML `BusyIndicator` (lighter than QCView's HLSL spinner), shown
  after a short delay so fast loads don't flash.

## Build / vendoring

- **FFmpeg 8.1** with **Vulkan + libplacebo + shaderc** (ProRes-on-Vulkan) +
  VideoToolbox (mac): update `scripts/setup-external-mac.sh` /
  `scripts/setup-external.ps1` and vendored libs under `external/ffmpeg/`
  (Windows = BtbN GPL-shared 8.1). Update `cmake/external.cmake` lib-version
  globs.
- **Vulkan SDK** + `Vulkan::Vulkan` link + `VK_USE_PLATFORM_WIN32_KHR` (Windows
  decode path only).
- Add `Qt6::GuiPrivate` (qrhi) + `Qt6::ShaderTools`; `qt_add_shaders` for the YUV
  `.qsb`; register QML types; add `app/player/*` to `qt_add_executable(ufb …)`
  and the new QML files to `QML_FILES`.
- Bundle/copy the new ffmpeg + Vulkan loader in the macOS `.app` and Windows
  dist steps (mirror existing ffmpeg/pdfium copy steps in `app/CMakeLists.txt`).

## Milestones (each independently runnable, none throwaway)

- **M1 — Video core + Mac zero-copy:** decode stack + `VideoController` +
  `VideoSurfaceItem` (Metal) + overlay shell + transport/seekbar + cleanup queue.
  → working video preview + scrub on macOS.
- **M2a — Images:** `ufb-preview` full-res provider + `ImagePreview`
  (fit-to-window) + spinner + large-image guard.
- **M2b — PDF reader:** `ufb-pdf` provider + continuous-scroll `PdfReader`;
  `TextPreview` (md/txt); `ThumbIconView` fallback wired in.
- **M3 — Audio:** `AudioPlayer` + drift-corrected sync + mute/volume.
- **M4 — Windows non-ProRes zero-copy:** pin D3D11; inject Qt device into
  D3D11VA; NV12 plane SRVs → RGBA.
- **M5 — Windows ProRes zero-copy:** FFmpeg 8.1 Vulkan vendoring + decode-only
  Vulkan device + Vulkan→D3D11 bridge (Qt device).
- **M6 — Polish:** scrub tuning, full shortcut set, memory-cap tuning, edge-
  format coverage (10/12-bit, 4444 alpha).

## Files

**Create (UFB):** `app/player/` (ported decode/render/audio C++, independent
copy); `app/qml/Ufb/App/{PreviewLightbox,ImagePreview,VideoPreview,PdfReader,TextPreview,ThumbIconView}.qml`;
shaders under `app/player/shaders/`.
**Modify (UFB):** `app/main.cpp` (pin QRhi); `app/UfbImageProviders.{h,cpp}`
(+`ufb-preview`, +`ufb-pdf`); `app/thumbnails/PdfBackend.{h,cpp}` (multi-page);
`app/qml/Ufb/App/Main.qml` (host overlay + `openPreview`);
`app/qml/Ufb/App/FileBrowser.qml` (Space in `_handleKey`, expose current path);
`app/CMakeLists.txt`, `CMakeLists.txt`, `cmake/external.cmake`, setup scripts.
**Borrow from (QCView, read-only):** `src/decode/*`,
`src/render/{zero_copy_bridge_metal,d3d11/*,metal/*,shaders/*}`, `src/audio/*`,
`src/window/window_manager.cpp:5040–5165` (shortcuts).

## Verification

- **Build:** `QMAKE=~/Qt/6.11.1/macos/bin/qmake cmake --build
  build/mac-debug --target ufb`; launch; smoke-check no QML errors.
- **M1 (mac):** Space on H.264 + ProRes 4444 → plays, scrubs smoothly (drag
  seekbar = instant frames), 4:2:0/4:2:2/4:4:4 + alpha correct; Left/Right switch;
  Esc closes; memory returns to baseline after close (`leaks`/Activity Monitor).
- **M2a/M2b:** large EXR/PSB → spinner then full-res; absurd image → "too large"
  not a crash; multi-page PDF scrolls continuously; unsupported → grid thumb/icon.
- **M3:** audio in sync; mute/volume; sync holds across seeks.
- **M4/M5 (Windows):** D3D11VA + ProRes(Vulkan) display zero-copy (PIX/GPU-Z: no
  per-frame CPU readback); VRAM flat across many Left/Right switches.
- **Parity:** same clip identical mac vs win (SDR BT.601/709 matrix + range).

## Risks / sharp edges

- **D3D11VA → Qt's device** (NV12 plane SRVs on an injected FFmpeg device) is the
  fiddliest piece; fallback = the NT-handle share the ProRes bridge uses.
- **FFmpeg 8.1 Vulkan/libplacebo vendoring** — real build-infra work, largest
  external dependency; do it at M5, isolated.
- **QRhi resource threading** — never free GPU textures off the render thread.
- **First `QQuickRhiItem` in UFB** — validate `GuiPrivate`/`ShaderTools` early
  (M1).

---

## Windows build plan (M3-win / M4 / M5 — re-cut 2026-06-02)

> This section is the inherited context for the Windows session. The original
> M4/M5 framing assumed near-greenfield Windows work; investigation on the
> Windows box shows it is much further along, and one design decision (M4) has
> been **deliberately dropped** to follow QCView's actual pattern. Read this
> before the per-milestone changelog.

### State on the Windows box (verified)
- **QCView-Player is checked out locally** at
  `C:\Users\uniongraphics\Documents\GitHub\QCView-Player` — the borrow source.
- **Toolchain ready:** Vulkan SDK `1.4.350.0` installed (`VULKAN_SDK` set);
  FFmpeg **8.1** vendored under `external/ffmpeg/` (BtbN GPL-shared, `avcodec-62`,
  `hwcontext_vulkan.h` present); `avfilter-11.dll` present. The M5 "FFmpeg 8.1
  Vulkan vendoring" risk is therefore **already retired** — no build-infra work.
- **The decode core's Windows branches are already ported** into
  `app/player/video_decoder.cpp`, `scrub_decoder.cpp`, `audio_player.cpp` (the
  `#if defined(Q_OS_WIN)` blocks: Vulkan handoff, D3D11VA routing, per-codec
  backend selection). They are inert on macOS and were never compiled on Windows.

### Decision: follow QCView's pattern exactly — drop M4 zero-copy
QCView's `FrameHandle` has only `Empty/Cpu/Metal/Vulkan` kinds — **there is no
zero-copy D3D11 kind anywhere in QCView.** Its `get_format` routing
(`src/decode/video_decoder.cpp:122-132`, identical to what was ported into UFB)
is:
- **ProRes → Vulkan-first** (`{VULKAN, D3D11}`) → `publishVulkanFrame` →
  **zero-copy** via the NT-handle Vulkan→D3D11 bridge.
- **Everything else (H.264/HEVC/…) → D3D11-first** (`{D3D11, VULKAN}`) →
  `av_hwframe_transfer_data` → `publishCpuFrame` → **CPU readback** (by design).

QCView confines Vulkan to ProRes deliberately, to sidestep a libplacebo/Vulkan
**H.264 heap-corruption** bug (the `[[intel-arc-vulkan-bridge-crash]]` note;
observed on the Intel-Arc dev box but **never confirmed Intel-only**, so treated
as unsafe everywhere). The plan's original **M4 ("non-ProRes D3D11VA zero-copy
on Qt's device, NV12 plane SRVs")** was never QCView's pattern — it is net-new,
the single fiddliest piece of interop in the effort, and for a path QCView itself
judged not worth it. **We drop M4.** Non-ProRes uses D3D11VA + CPU readback (the
CPU passthrough renderer already shipped on macOS handles it); zero-copy lives
only where QCView trusts it (ProRes/Vulkan). The per-frame readback is well
within budget for a preview lightbox (QCView eats it as a full review tool).

### Re-cut milestones
- **W0 — compile + non-ProRes video + Windows audio.** Port the 3 missing source
  units the ported branches already `#include`; wire CMake; build. Outcome:
  H.264/HEVC preview plays (D3D11VA → CPU readback → existing CPU passthrough
  renderer), WASAPI audio in sync (this is **M3 on Windows**), Left/Right nav,
  Esc/✕ close. No render-side work — the CPU path is already wired.
- **W1 — ProRes Vulkan zero-copy (= M5).** Port the Vulkan→D3D11 display bridge
  and add a Vulkan branch to `VideoSurfaceItem`. Outcome: ProRes displays
  zero-copy; VRAM flat across many Left/Right switches.
- **(M4 dropped.)**

### W0 — files to port (QCView → UFB, namespace `qcv`→`ufbplayer`)
The ported Windows branches `#include` these but they were never copied:
- `src/decode/vulkan/vulkan_device_manager.{h,cpp}` →
  `app/player/vulkan/vulkan_device_manager.{h,cpp}` (shared decode-only
  `VkDevice` singleton; referenced by `video_decoder.cpp:34` and
  `scrub_decoder.cpp:9`).
- `src/decode/vulkan_hw_device_ctx.{h,cpp}` →
  `app/player/vulkan_hw_device_ctx.{h,cpp}` (`AVVulkanDeviceContext` handoff
  helper; referenced by `video_decoder.cpp:35`).
- `src/audio/wasapi_audio_device.{h,cpp}` →
  `app/player/wasapi_audio_device.{h,cpp}` (Windows audio output; referenced by
  `audio_player.cpp:9`).

Strip any OCIO/annotation/timeline/dual/project deps as on macOS (expected
minimal — these are decode/audio-layer units).

### W0 — CMake / link changes (`app/CMakeLists.txt`, under the `if(WIN32)` block)
- Add to `target_sources(ufb …)`: `player/vulkan/vulkan_device_manager.cpp`,
  `player/vulkan_hw_device_ctx.cpp`, `player/wasapi_audio_device.cpp`.
- `find_package(Vulkan REQUIRED)`; link `Vulkan::Vulkan`. Vulkan SDK is at
  `$VULKAN_SDK` (`C:\VulkanSDK\1.4.350.0`).
- Link Windows libs the bridge/decode need: `d3d11 dxgi` (+ `mfplat`/`mfuuid`
  only if pulled in). WASAPI needs `ole32` (already linked) + `avrt` (mmcss);
  confirm against QCView's `src/audio/CMakeLists.txt`.
- `target_compile_definitions(ufb PRIVATE VK_USE_PLATFORM_WIN32_KHR)` (Windows
  decode/bridge only).
- Link `avfilter` on Windows (multi-stream audio) — macOS already does; the DLL
  (`avfilter-11.dll`) is vendored. Add `avfilter` to the Windows ffmpeg lib list
  in `cmake/external.cmake` if absent.
- Deploy step: the new ffmpeg DLLs are already copied; ensure the Vulkan loader
  (`vulkan-1.dll`, system-provided) resolves at runtime.

### W1 — files to port (= M5, ProRes zero-copy)
- `src/render/d3d11/d3d11_device_manager.{h,cpp}` — **add an
  `initializeWithDevice(void*)` path** so it wraps Qt's QRhi `ID3D11Device`
  (from `rhi()->nativeHandles()`) instead of creating its own.
- `src/render/d3d11/d3d11_vulkan_decode_bridge.{h,cpp}` — AVVkFrame → standalone
  RGBA16F `ID3D11Texture2D` via NT-handle Vulkan↔D3D11 interop.
- `src/render/d3d11/d3d11_vulkan_yuv_compositor.{h,cpp}` — YUV→RGB on D3D11,
  initialized with Qt's device.
- **`VideoSurfaceItem.cpp`:** add a `FrameHandle::Kind::Vulkan` branch (mirroring
  the existing Metal branch) that runs the frame through the bridge and wraps the
  resulting `ID3D11Texture2D` as a `QRhiTexture` via `QRhiTexture::createFrom`,
  then draws the fitted quad. Release the wrapped texture on the render thread
  (QRhi deferred-release), same discipline as Metal.

### Windows risks / sharp edges
- **Vulkan↔D3D11 NT-handle interop on Qt's device** (W1) is the main risk — the
  bridge was written against QCView's own device; the injection path
  (`initializeWithDevice`) must use Qt's `ID3D11Device`/context exactly.
- **HW frame-pool teardown on every switch** — route old decoders through the
  ported `decoder_cleanup_queue` (already present) so Vulkan/D3D11 pool
  destruction never stalls the UI thread; watch VRAM across Left/Right.
- **Do not enable Vulkan for non-ProRes** to "get zero-copy everywhere" — that
  re-opens the H.264 corruption bug. The per-codec routing is load-bearing.
- **First `QQuickRhiItem` Vulkan-interop in UFB** — validate the bridge in
  isolation (one ProRes clip) before chasing format coverage.

---

## Status / changelog

Append newest entries at the top. Each milestone updates this as part of its commit.

### 2026-06-04 — Dogfood edge polish (verified, macOS)
Five small UX fixes from a dogfood pass:
- **Refocus the browser on close.** The lightbox grabbed keyboard focus on
  open but never handed it back, so after closing, the still-selected file
  wasn't keyboard-actionable (Space wouldn't re-open the preview until a manual
  click reselected). `PreviewLightbox.close()` → `Main.previewClosed()` →
  `FileBrowser.refocusView()` (focuses the active list/grid/tree view).
- **Persistent loop.** Loop was a per-instance `property bool` that reset every
  clip. Backed it with a real setting: `AppSettings.preview_loop` (serde
  `previewLoop`, default false) + `Settings.preview_loop()`/`set_preview_loop()`
  in the cxx-qt binding; `VideoPreview.loop` seeds from it and `toggleLoop()`
  persists. Sticks across clips + launches.
- **Autoplay settle delay.** Starting the PTS pacer the instant `dec.open()`
  returned made large / networked clips lurch to "catch up." `VideoPreview` now
  defers autoplay by `_autoplayDelayMs` (400 ms) via a one-shot Timer that
  starts video + audio together.
- **Instant overlay (the big perceived-latency fix).** Two synchronous stalls
  were delaying the overlay's *first paint* so Space felt like it did nothing:
  (1) the content `Loader` built the whole VideoPreview/ImagePreview tree
  inline → set `asynchronous: true`; (2) `dec.open()` probes the file
  (avformat stream-info) on the GUI thread → deferred one tick via
  `Qt.callLater(_openClip)`. Now the scrim/filename/close pop immediately.
  Added delayed (150 ms) `BusyIndicator`s at the lightbox level (content
  incubation) and over the video surface (decode of first frame; cleared on
  `frameAvailable`/`Errored`), mirroring ImagePreview's existing spinner.
  **Caveat:** the avformat probe inside `_openClip` still briefly blocks the
  GUI thread, so its spinner can't *animate* during that window (scrim is
  already up). If slow network clips still feel frozen mid-load, the real fix
  is probing on a background thread in `VideoDecoder::open()` — deferred.
- **Loop button spacing.** `Layout.leftMargin: Theme.dim.spacing * 2` between
  the frame counter and the loop toggle.

### 2026-06-03 — EXR layer grid: per-layer decode + contact-sheet picker (commit d95db48)
- Adds EXR layer/channel selection to the still preview (static EXRs), matching
  QCView's channel model.
- **Backend (`ExrBackend`):** `listExrLayers(path)` enumerates layers —
  multi-part part names, or single-part dotted channel-name prefixes (+ `default`
  for bare R/G/B/A), cryptomatte filtered (ported from QCView `discoverLayers`).
  `decodeExr()` gains a `layer` param: empty/default → existing `RgbaInputFile`
  path (plain RGBA + luminance-chroma, **no regression**); named layer →
  `MultiPartInputFile` + `<layer>.R/G/B/A` reader with bare fallback, opaque-alpha
  synthesis, and displayWindow≠dataWindow handling (ported from QCView
  `loadFrame`), tone-mapped to RGBA8.
- **Plumbing:** `UfbExrLayerProvider` → `image://ufb-exr-layer/<layer>/<path>`
  (both segments percent-encoded); `ExrInfo` QML singleton exposes `layers(path)`.
- **UI (`ImagePreview`/`PreviewLightbox`):** EXR with >1 layer opens into a
  contact-sheet grid of layer thumbnails; click one → full frame. Single-layer
  EXR / other images show full-frame directly. A "Layers" button on the top row
  (next to the close X) + Esc/Backspace step back to the grid; Left/Right stay
  file-nav.
- **Parity caveat (= QCView):** non-RGB AOV layers (normals `.X/Y/Z`, etc.) fall
  back to beauty. Future enhancement: map X/Y/Z→RGB and single channel→grayscale.

### 2026-06-03 — Space toggles preview open/close (commit 4e27117)
- Space now consistently **opens** the lightbox from the file browser and
  **closes** it from inside — including over video, where it previously toggled
  play/pause. Play/pause moves to **K / W / S** (video keeps a pause key). Esc and
  the close button unchanged. Shared QML → both platforms get the new behaviour.
- Supersedes the earlier "Space = play/pause once a video is open" decision in
  the locked-decisions list above.

### 2026-06-02 — Alpha compositing fix (ProRes 4444) — shared C++, verified on Windows
- **Bug (both platforms):** alpha-bearing video (ProRes 4444 → AYUV on macOS,
  the Vulkan YUV compositor on Windows) rendered with **blending disabled** —
  `makePipeline` set cull/depth but never `setTargetBlends`, so the quad drew
  opaque and the real (straight) alpha was written but never composited →
  jagged, un-composited edges. Latent on macOS too (4444-with-alpha was never
  tested in the lightbox).
- **Fix (`VideoSurfaceItem.cpp` + one QML line):** `makePipeline` gains a
  `blend` flag → straight-alpha "over" (`SrcAlpha`/`OneMinusSrcAlpha`, with
  `srcAlpha=One`/`dstAlpha=OneMinusSrcAlpha`). Enabled on AYUV, biplanar
  (opaque a=1 → no-op), and the Windows Vulkan pipeline. **CPU passthrough stays
  opaque** so a stray `alpha=0` from a software frame can't make normal mp4
  vanish. Clear the surface to the backdrop fill (not black) so transparent
  regions composite over the panel; `VideoPreview` `fillColor` bound to the
  scrim tone (`Theme.colors.bg`) for a seamless composite + letterbox. Both
  alpha paths emit straight (non-premultiplied) alpha, so one blend mode is
  correct for both.
- Verified on Windows: ProRes 4444 alpha composites smoothly over the panel;
  mp4 + opaque ProRes unchanged. **macOS gets the same fix on next rebuild
  (shared code) — verify there.**

### 2026-06-02 — Windows session opened: plan re-cut (M4 dropped); W0 started
- Investigated the Windows box: QCView checked out locally; Vulkan SDK 1.4.350.0;
  FFmpeg 8.1 (Vulkan-capable) + avfilter already vendored. Decode core's Windows
  branches were already ported but reference 3 source units never copied over.
- **Verified QCView has no D3D11 zero-copy path** — non-ProRes is D3D11VA + CPU
  readback by design; only ProRes is zero-copy (Vulkan). **Dropped the original
  M4** (D3D11VA-on-Qt's-device zero-copy) to follow QCView's pattern; re-cut to
  W0 (compile + non-ProRes CPU path + WASAPI audio) and W1 (= M5, ProRes Vulkan
  zero-copy). See the new "Windows build plan" section above.
- **W0 compiles + links on Windows.** Ported the 3 units into `app/player/`
  (namespace `qcv`→`ufbplayer`, content otherwise verbatim):
  `vulkan/vulkan_device_manager.{h,cpp}`, `vulkan_hw_device_ctx.{h,cpp}`,
  `wasapi_audio_device.{h,cpp}`. Fixed a stale port include in
  `scrub_decoder.cpp` (`"decode/vulkan/…"` → `"vulkan/…"`). Added the Windows
  Vulkan-device soft-init to `main.cpp` (mirrors QCView F.2.4.1). CMake:
  `find_package(Vulkan REQUIRED)` + link `Vulkan::Vulkan`, `avrt` (MMCSS),
  define `VK_USE_PLATFORM_WIN32_KHR`, new sources under `if(WIN32)`; added
  `avfilter` to the Windows ffmpeg lib list in `cmake/external.cmake`
  (`avfilter-11.dll`/`avfilter.lib`, already vendored). `cmake --build
  build/release --target ufb` → all 3 units build, links clean, fresh
  `ufb.exe`. **Runtime verified (user):** stills + mp4 (H.264 via D3D11VA →
  CPU readback → CPU passthrough renderer) preview correctly. ProRes 4444 is
  blank as expected — it publishes `FrameHandle::Kind::Vulkan` and
  `VideoSurfaceItem` has no Vulkan branch yet (that is W1).
- **W1 verified (user): ProRes 4444 displays zero-copy on Windows.** The
  Vulkan→D3D11 bridge path works first-try — correct orientation (no Y-flip
  needed) and the non-ProRes/CPU path still renders after a ProRes frame. This
  completes the original **M5**. Windows now has parity with macOS for video
  (non-ProRes via D3D11VA CPU-readback, ProRes via Vulkan zero-copy), stills,
  PDF, text, and audio.
- **W1 ported + builds.** Copied QCView's three
  render-side units into `app/player/d3d11/` (namespace `qcv`→`ufbplayer`,
  include paths rewritten): `d3d11_device_manager`, `d3d11_vulkan_yuv_compositor`,
  `d3d11_vulkan_decode_bridge`. Added `D3D11DeviceManager::initializeWithDevice
  (dev, ctx)` so it **adopts Qt's QRhi `ID3D11Device`** (from
  `QRhiD3D11NativeHandles`) instead of creating its own — the bridge's output
  texture must live on Qt's device for `QRhiTexture::createFrom` to wrap it.
  `VideoSurfaceItem` gains a `Kind::Vulkan` branch: lazy-inits device-adopt +
  compositor + bridge, then per frame `bridge.consume()` → RGBA16F
  `ID3D11Texture2D` → `QRhiTexture::createFrom` → drawn with the **passthrough**
  pipeline (already RGBA; no YUV shader). The native `consume()` is bracketed in
  `cb->beginExternal()/endExternal()` so QRhi invalidates its cached D3D11 state.
  CMake: new sources under `if(WIN32)`, link `d3d11 dxgi dxguid` +
  `$VULKAN_SDK/Lib/shaderc_combined.lib` (compositor compiles its GLSL compute
  shader at runtime via shaderc). Builds clean; `ufb.exe` links (grows ~5 MB
  from the static shaderc). **Slow-link note:** `shaderc_combined` is a large
  static lib → multi-minute relink; switch to `shaderc_shared` + ship the DLL
  if it becomes painful.
- **Watch-items for runtime verify:** (1) orientation — the bridge's RGBA may be
  Y-flipped vs `passthrough.frag` (which flips Y for the CPU path); ProRes
  upside-down → adjust. (2) QRhi/D3D11 immediate-context sharing across
  `beginExternal` — watch for state corruption on the non-ProRes path after a
  ProRes frame. (3) VRAM flatness across Left/Right switches (HW pool teardown).
- **Next:** runtime-verify ProRes 4444 zero-copy display, then edge formats
  (10/12-bit, 4444 alpha) + VRAM/teardown checks.

### 2026-06-02 — M6: looser preview image caps + cached PDF document (verified, macOS)
- **Full-res preview decode caps.** Threaded a `bool fullResPreview` through
  `Thumbnailer::extract` and a `qint64 maxPixels` through `decodeExr` / `decodeHdr`
  / `decodePsd`. Thumbnails keep the flat 64 MP cap; the lightbox preview raises
  it via a ~1.5 GB decoded-buffer budget converted per backend's worst-case
  bytes/pixel (EXR/HDR 16 bpp → ~96 MP, PSD/qtNative 8 bpp → ~192 MP), and the
  unknown-dims file-size decline rises from 256 MB → 1.5 GB. Large EXR/PSB/TIFF
  now preview full-res; truly enormous files still decline gracefully (→ shell
  thumbnail → file icon) instead of OOM-ing. `UfbPreviewProvider` passes
  `fullResPreview=true`.
- **PSD/PSB size guard added** (it had none): `decodePsd` now checks
  `width·height > maxPixels` *before* parsing/allocating the composite, so a
  giant PSB declines instead of allocating w·h·4 + SDK channel buffers and
  OOM-ing. (Previously big PSBs only survived because the shell thumbnail ran
  first at stage-0; the preview calls the backend directly.)
- **Cached PDF document.** `PdfBackend` keeps the most-recent `FPDF_DOCUMENT`
  open in a single-entry cache keyed by path+mtime (guarded by the existing
  `pdfMutex`). `pdfPageCount` / `pdfPageAspect` / `decodePdfPage` reuse it
  instead of re-opening (re-parsing header/xref) per call — the continuous-scroll
  reader renders many pages of one file. `releasePdfCache()` (exposed as
  `PdfDoc.releaseCache()`, called from `PdfReader.qml` `Component.onDestruction`)
  closes it when the reader goes away so the file isn't held open / lockable on
  Windows after the preview closes.
- Verified: large EXR/PSB preview full-res; multi-page PDF scrolls.

### 2026-06-02 — M6: full transport shortcut set + accelerating fast-seek (verified, macOS)
- Ported QCView's lightbox transport keys (`PreviewLightbox.qml` dispatches to
  `VideoPreview` functions): **Space/K/W/S** play-pause (W/S are WASD-cluster
  aliases, identical to Space), **Q/E** step ∓1 frame, **A·J / D·L** fast-seek,
  **M** mute, **↑/↓** volume ±5%, **V** loop (+ a clickable ↻ button),
  **Home/End** jump to first/last frame. ←/→ stay global media-switch; PDF/text
  keep their scroll keys; non-video Space still closes.
- **Loop** implemented at the QML level (decoder has no native loop): a
  `Connections` on `dec.state === VideoDecoder.EndOfStream` restarts from frame 0
  when `loop` is on.
- **Frame nav routes through the ScrubDecoder, not the streaming decoder.**
  Step / fast-seek / Home / End call `scrubToFrame` (instant on ProRes,
  smooth-within-GOP on inter) and record `_scrubTarget`; the streaming decoder is
  repositioned **lazily on the next resume** (`_syncStreamingForResume` →
  `seekToFrame`), the keyboard analogue of the seekbar's reposition-on-release.
  The seekbar now shares the same `_scrubTo` / `_syncStreamingForResume` helpers.
- **Accelerating fast-seek shuttle** (the smoothness fix): per-keypress seeking
  was choppy (OS key-repeat = irregular discrete 1 s jumps). Ported QCView's
  `start/stopFastSeek`: a 33 ms `Timer` advances a position at a geometric
  2×→32× rate (doubles per second) and `scrubTo`s every tick → continuous motion.
  Held A/J/D/L starts it (no-op on auto-repeat, restarts on direction flip);
  `Keys.onReleased` (ignoring auto-repeat releases) stops it. Uses our instant
  `scrubToFrame` per tick instead of QCView's `seekToFrame`, so it's even
  smoother on ProRes.
- Verified: FF/RW ramps smoothly like QCView; resume continues from the scrubbed
  frame; all keys behave.

### 2026-06-02 — M6: instant GOP-cached scrubbing (verified, macOS)
- Integrated QCView's `ScrubDecoder` into `VideoDecoder` for QCView-smooth
  seekbar dragging. `video_decoder.h`: `std::unique_ptr<ScrubDecoder> m_scrubDecoder`
  + `Q_INVOKABLE void scrubToFrame(int)`. `video_decoder.cpp`: created/opened
  alongside the streaming decoder in `open()`, closed in `close()`; `scrubToFrame`
  forwards to `m_scrubDecoder->requestFrame()`. The scrub decoder shares the
  streaming decoder's `frameIndex()` and publishes into the same latest-wins slot
  via `publishExternalFrame`, so the surface shows the scrubbed frame with no
  separate render path.
- `VideoPreview.qml` seekbar: live `onMoved → dec.scrubToFrame(round(value))`
  (instant within a GOP); press pauses both decoders + audio; release calls
  `dec.seekToFrame` + `audio.seek` and resumes, so the streaming decoder
  repositions to where the drag ended.
- Applied the same FFmpeg-7.1 timestamp fixes to `scrub_decoder.cpp` that M1
  needed for the streaming decoder: set `m_cctx->pkt_timebase = stream->time_base`
  before `avcodec_open2`, and prefer `frame->pts` over `best_effort_timestamp`
  (which returns constant garbage on 7.1 + VideoToolbox).
- **Orientation fix:** inter-frame mp4 scrubbing uses the scrub decoder's
  *software* path → a CPU `FrameHandle` → the **passthrough** shader, which
  (unlike the Metal YUV shaders) wasn't flipping Y, so scrubbed mp4 frames came
  out upside down. Flipped Y in `passthrough.frag` (`vec2(v_uv.x, 1.0 - v_uv.y)`)
  to match the YUV shaders. In our pipeline passthrough is used *only* for the CPU
  video frame, so the flip is correctly scoped; the Metal zero-copy path is
  untouched. ProRes (all-keyframe, intra) scrubbing was already instant + correct.
- Verified: mp4 scrubbing is smooth and right-side up; ProRes unchanged.

### 2026-06-02 — M3: audio playback + sync (verified, macOS)
- Ported QCView's audio subsystem to `app/player/` (namespace ufbplayer):
  `audio_ring_buffer.h`, `i_audio_source.h`, `audio_decoder.{h,cpp}` (swr resample
  + 5.1→stereo downmix), `multi_stream_audio_decoder.{h,cpp}` (libavfilter graph
  for 2+ track broadcast masters), `audio_player.{h,cpp}`, `coreaudio_device.{h,mm}`.
  `AudioRoutingMode` copied into a standalone `audio_routing.h` (the only
  `project/media_item.h` dependency).
- **Linked `avfilter`** (added to the macOS ffmpeg lib list in
  `cmake/external.cmake`; dylib was already vendored/bundled) — multi-stream
  decoder needs it. Windows avfilter link deferred to M4/M5.
- `VideoPreview` owns a `VideoDecoder` + `AudioPlayer`: opens/plays both,
  pauses audio while scrubbing (seeks on release), pumps
  `audio.update(videoSeconds)` at ~30 Hz (video master, audio drift-corrected),
  mute + volume controls (shown only when the clip has audio).
- **Two gotchas fixed:** (1) `AudioPlayer` methods weren't `Q_INVOKABLE`
  (QCView drove it from C++), so QML calls failed with "Property 'initialize'
  … is not a function" and audio never started — added Q_INVOKABLE to the
  transport surface. (2) `initialize()` moved into `onSourceChanged` (idempotent)
  since `onSourceChanged` can fire before `Component.onCompleted`.
- Verified: audio plays in sync (incl. ProRes .mov), mute/volume work.

### 2026-06-02 — M2b: PDF continuous-scroll reader + Markdown/text (verified)
- `PdfBackend` extended: `pdfPageCount`, `pdfPageAspect`, `decodePdfPage(path,
  page, size)`; `decodePdf` now delegates to page 0. Each call opens/closes the
  doc (per-call; a shared-open-doc cache is a later perf refinement).
- `PdfDoc` QML singleton (`app/PdfDoc.{h,cpp}`) exposes pageCount/pageAspect.
- `UfbPdfProvider` (`image://ufb-pdf/<page>/<path>`): renders one page at the
  requested width on the worker pool.
- `PdfReader.qml`: ListView of page images (lazy render + offscreen eviction =
  bounded memory), uniform delegate aspect from page 0, `UfbScrollBar`
  AlwaysOn + page gutter, scroll API (`scrollStep/scrollPage/scrollHome/
  scrollEnd`). `TextPreview.qml`: file:// XHR + `TextArea` MarkdownText/PlainText
  (1 MB cap), same scrollbar + scroll API. `PreviewLightbox` routes pdf/ai →
  PdfReader, txt/md/log/json/… → TextPreview, else → ImagePreview; routes
  Up/Down/PageUp/PageDown/Home/End to the content's scroll API (Left/Right stay
  media nav; wheel/trackpad scroll natively).
- **Resolution bug fixed**: PDF pages rendered at native point size (~72 DPI)
  because `decodePdfPage`'s scale required BOTH requestedSize dims > 0, but the
  reader passes only `sourceSize.width`. Now honors a width-only / height-only
  request → renders at view-width × DPR (crisp). Verified.
- Refinements noted: per-call PDF doc open (cache later); GUI-thread parse on
  open of huge PDFs (move to worker if it stalls); mixed-orientation pages
  letterbox (uniform page-0 aspect).

### 2026-06-02 — M2a: full-res still preview (verified)
- New `UfbPreviewProvider` (`image://ufb-preview/<path>`): extracts at the
  requested view size via `Thumbnailer::extract` (no 512 master, no persistent
  cache), shell-thumbnail fallback for backend-undecodable types (RAW/HEIC/
  Office), null → QML shows the file icon. Registered in main.cpp.
- `ImagePreview.qml`: fit-to-window `Image` at device-pixel `sourceSize`,
  `BusyIndicator` (150ms-delayed) while decoding, `FileTypeIcon` fallback on
  Error/empty. `PreviewLightbox` routes all non-video to it (PDF currently shows
  page 0 full-res; real reader is M2b).
- Verified: EXR/PSD/TIFF/JPEG/PNG show full-res; large files spin then resolve;
  EXR/HDR tonemap looks right; Left/Right mixes video + stills.
- Follow-up (noted): the over-size guards are still the thumbnail caps (64 MP /
  256 MB); "looser preview caps for very large PSB/EXR" is a later refinement
  (needs a higher cap threaded through Thumbnailer + backends). Spinner +
  icon-fallback handle the decline gracefully meanwhile.

### 2026-06-02 — M1 core complete (macOS): media nav + all ProRes formats
- **Left/Right media navigation**: `FileBrowser.previewStep(dir)` advances the
  pane cursor to the next/prev non-dir entry and returns its path; `Main` keeps
  the originating pane and re-opens the lightbox on step. Wired to Left/Right in
  `PreviewLightbox` (swaps video↔still renderers; skips folders; stops at ends).
- **ProRes 4444 (no alpha) was blank** — VideoToolbox outputs `'sv44'`
  (`kCVPixelFormatType_444YpCbCr16BiPlanarVideoRange`). `cvPixelBufferIsZeroCopy
  SupportedRaw` (frame_handle.mm) advertised it, but the ported QRhi bridge's
  `layoutForCvFormat` didn't map it → Unsupported → blank. (QCView's full Metal
  renderer covers it; the QRhiItem bridge I ported was the narrower mid-migration
  one.) Fix: widened `layoutForCvFormat` to the full set
  `cvPixelBufferIsZeroCopySupportedRaw` advertises — 8-bit 4:2:0/4:2:2/4:4:4 →
  R8/RG8 (NV12 layout); 10/16-bit 4:2:0/4:2:2/4:4:4 incl. sv22/sv44 → R16/RG16
  (P210 layout); AYpCbCr16 packed. No new shaders needed (plane textures sized
  from CVPixelBuffer plane dims; shader samples normalized UV). Keeps zero-copy.
  LESSON: the decoder's zero-copy-support check and the bridge's format support
  MUST stay in lockstep, or "supported but unbridgeable" formats go blank.
- **M1 core is feature-complete on macOS**: spacebar open, real-time playback
  (H.264/HEVC/ProRes 422/4444), seekbar scrub (pause-on-drag), Space play/pause,
  Esc/✕ close, Left/Right media nav. Remaining M1-adjacent: audio (M3), images
  (M2a), PDF/text (M2b), and the GOP-cache ScrubDecoder smoothness (M6).

### 2026-06-02 — M1: real-time playback working (macOS); FFmpeg 7.1 timestamp fixes
- Verified live: H.264 (NV12 '420v' zero-copy) and ProRes both decode + display;
  playback now runs at correct real-time speed.
- **Two FFmpeg-7.1-vs-8.1 adaptations** (UFB vendors 7.1 today; QCView targets
  8.1). Not logic rewrites — version compatibility:
  1. Set `m_cctx->pkt_timebase = st->time_base` before `avcodec_open2` (7.1 needs
     it for correct packet→frame PTS; 8.1 tolerates omitting).
  2. `framePresentationPts()` helper prefers `frame->pts` over
     `best_effort_timestamp`. On 7.1 + VideoToolbox, `best_effort_timestamp`
     returns a CONSTANT garbage value (`0x100000001`) while `frame->pts`
     advances correctly — the constant flatlined the PTS-delta pacer → "plays
     as fast as it can." Used at all 4 derivation sites (publish cpu/metal/vulkan
     + performSeek). When we move to FFmpeg 8.1 (M5) this stays correct.
- Autoplay: `VideoPreview` calls `dec.play()` after `open()` (decoder opens
  PAUSED by QCView's review-default; faithful port).
- Pacing is PTS-driven (handles all fps incl. VFR + odd timebases + non-zero
  start PTS; drift-free). Caveat: lags rather than drops frames if decode can't
  keep real-time (M6 polish).
- **Scrub:** seekbar now pauses playback while dragging (each seek lands + holds;
  seek frames publish immediately since the pacer is gated on isPlaying). mp4 +
  ProRes both scrub. Verified working — "pretty good, not quite as smooth as
  QCView." The remaining smoothness gap is the **GOP-cache `ScrubDecoder`** (LRU
  of decoded GOP frames → instant drag within a GOP); ported but not yet wired.
  Deferred to **M6 (scrub tuning)** — needs the surface to show scrub frames
  from that second decoder.

### 2026-06-02 — M1: render pipeline wired (macOS) — builds + loads clean
- Ported the Metal zero-copy bridge `zero_copy_bridge_metal.{h,mm}` (the
  QRhiTexture-producing variant — pulls the MTLDevice from
  `QRhiMetalNativeHandles`, no `MetalDeviceManager` dep) and the 4 GLSL shaders
  (`fullscreen.vert`, `passthrough.frag`, `yuv_to_rgb_biplanar.frag`,
  `yuv_to_rgb_ayuv.frag`) → `app/player/shaders/`, compiled via `qt_add_shaders`
  (PREFIX `/ufb` → `:/ufb/player/shaders/*.qsb`).
- Rewrote `VideoSurfaceItem` with a real renderer: `synchronize()` fetches the
  latest `FrameHandle`; `render()` does a **single pass** into the item's render
  target — Metal frames bridge to plane textures → biplanar/AYUV YUV→RGB shader;
  CPU frames upload RGBA8 → passthrough — drawn fitted (letterboxed). Dropped
  QCView's compositor/OCIO passes. Added a `videoDecoder` property; repaints on
  the decoder's `frameAvailable`.
- `VideoPreview.qml` (VideoDecoder + VideoSurfaceItem + play/pause + seekbar);
  `PreviewLightbox` routes `videoExts` → `VideoPreview` (Space = play/pause),
  other types → thumbnail stand-in for now.
- **Gotcha fixed:** a `VideoDecoder*` Q_PROPERTY on `VideoSurfaceItem` needs the
  full type AND `VideoDecoder` registered as a *module* `QML_ELEMENT` (a runtime
  `qmlRegisterType` is invisible to the static qmltyperegistrar that processes
  the property) — else "VideoSurfaceItem is not a type". Fix: `QML_ELEMENT` on
  `VideoDecoder` + its sources in the qml module SOURCES; dropped the runtime
  register. Also: `qt_add_shaders` exposes PREFIX + the given FILE path, so
  PREFIX `/ufb` + `player/shaders/x` → `:/ufb/player/shaders/x.qsb` (don't
  double the `player/`).
- Builds clean; launches with no QML/type errors. **Interactive video playback
  not yet user-verified** (needs Space on a real clip).
- Watch-item: macdeployqt logs `Cannot resolve rpath @rpath/libavcodec…` during
  fixup (same ffmpeg the thumbnail path already loads at runtime) — confirm the
  preview decodes a real clip; revisit bundling if not.
- **Next:** verify playback/scrub on real clips (H.264 + ProRes 4444); then
  Left/Right media nav, audio (M3), and image/PDF renderers (M2).

### 2026-06-02 — M1: decode core ported + compiling (macOS)
- Copied QCView's decode stack into `app/player/` as an **independent copy**
  (namespace `qcv` → `ufbplayer`): `frame_handle.{h,mm,_nonapple.cpp}`,
  `frame_index.{h,cpp}`, `scrub_frame_cache.{h,cpp}`, `simple_lru.h`,
  `decoder_cleanup_queue.{h,cpp}`, `timecode_formatter.{h,cpp}`,
  `video_decoder.{h,cpp}`, `scrub_decoder.{h,cpp}`.
- Entanglement turned out minimal: **no OCIO/annotation/image-sequence in the
  decode layer** (only a comment). Kept `timecode_formatter` (small, standalone)
  rather than gutting timecode out of `video_decoder`. Windows Vulkan/D3D11
  branches are `#if defined(Q_OS_WIN)` — inert on macOS, wait for M4/M5 (they
  reference an unported `VulkanDeviceManager`).
- CMake: added all decode-core sources to `qt_add_executable(ufb)`; platform
  split for `frame_handle` (`.mm` on APPLE + `-framework CoreVideo`,
  `_nonapple.cpp` elsewhere). Builds clean on macOS (VideoToolbox + software
  paths compile; ffmpeg already vendored/linked via `ufb::ffmpeg`).
- Decode core is compiled but **not yet wired** to a renderer — it's the
  dependency base for the next step.
- **Next:** QML-register `VideoDecoder` (or a thin `VideoController`); port the
  Metal zero-copy bridge (`ZeroCopyBridgeMetal`/`cv_pixbuf_metal_bridge`) + YUV
  `.qsb` shaders; make `VideoSurfaceItem` fetch `FrameHandle`s and render real
  frames; then `VideoPreview.qml` transport + seekbar.

### 2026-06-02 — M1 scaffolding landed (spine validated)
- **Build spine proven** (the flagged M1 risk): a `QQuickRhiItem` compiles,
  links, registers as a QML type, and instantiates inside an in-app overlay in
  UFB. Added `Qt6::GuiPrivate` (`rhi/qrhi.h`) + `Qt6::ShaderTools` to the root
  `find_package`; linked `Qt6::GuiPrivate` to `ufb`; added `app/player/` include
  dir. App builds clean and launches with no QML/type errors.
- **Graphics API pinned** in `app/main.cpp` before app construction:
  `QSGRendererInterface::Metal` (macOS) / `Direct3D11` (Windows). (Both Qt
  defaults; pinned for deterministic zero-copy interop.)
- **`app/player/VideoSurfaceItem.{h,cpp}`** — minimal `QQuickRhiItem` whose
  renderer clears to a `fillColor` (no decode yet). Registered via `QML_ELEMENT`
  in the `Ufb.App` module SOURCES (mirrors the `Thumbnailer` pattern).
- **`PreviewLightbox.qml`** — overlay shell (dark backdrop, centered
  `VideoSurfaceItem`, filename label, top-right close icon, Esc/Space close),
  parented to `Overlay.overlay` in `Main.qml` via `window.openPreview(path)`.
- **Spacebar** wired in `FileBrowser._handleKey` → `Window.window.openPreview(path)`
  for the current non-dir item (list/grid via `entriesModel`, tree via
  `treeCurrentPath`).
- Files: `app/player/VideoSurfaceItem.{h,cpp}`,
  `app/qml/Ufb/App/PreviewLightbox.qml`; modified `CMakeLists.txt`,
  `app/CMakeLists.txt`, `app/main.cpp`, `app/qml/Ufb/App/{Main,FileBrowser}.qml`.
- **Not yet done in M1:** port the decode stack (`VideoDecoder`/`ScrubDecoder`/
  `FrameIndex`/`FrameHandle`/`decoder_cleanup_queue`), the `VideoController`
  QObject, the macOS Metal zero-copy frame→texture bridge + YUV shaders, and the
  transport/seekbar. The surface currently just shows a flat color when opened.
- **Next:** port `FrameHandle` + `FrameIndex` + `VideoDecoder` (software +
  VideoToolbox) into `app/player/`, wrap in `VideoController`, feed frames to
  `VideoSurfaceItem` via the Metal bridge.

### 2026-06-02 — Plan approved, not yet started
- Full design agreed with the user after exploring both UFB and QCView-Player.
- Key architectural conclusions that de-risked the effort:
  - No native window needed (SDR-only, no OCIO) → in-app `QQuickRhiItem` on both
    platforms.
  - UFB already uses Qt's **D3D11** QRhi on Windows (Qt default) → no global
    Vulkan switch; Vulkan is decode-only (ProRes).
  - QCView's Metal zero-copy bridge and D3D11/Vulkan decode bridge are
    window-decoupled, standalone texture producers → portable to a QQuickRhiItem
    with a one-time Qt-`ID3D11Device` injection.
- Decisions: zero-copy from day one; audio in v1; seekbar (not full timeline);
  PDF continuous-scroll; images fit-to-window v1; Markdown/text in v1, RTF/Office
  v2; vendor FFmpeg 8.1; code independent of QCView.
- **Next:** M1 (video core + macOS zero-copy). No code written yet.
