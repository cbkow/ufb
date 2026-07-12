// vulkan_hw_device_ctx — Phase F.2.12.a.
//
// Public helper that wraps ufbplayer::VulkanDeviceManager as an
// AVVulkanDeviceContext, so FFmpeg decodes (Vulkan hwaccel) land on
// the same VkDevice the D3D11VulkanDecodeBridge consumes from. Lifted
// out of video_decoder.cpp's anonymous namespace so it can be shared
// with the scrub decoder — no copy-paste, no parallel implementation.
//
// Windows-only: this header gates everything on Q_OS_WIN because the
// Vulkan-decode handoff is a Windows-specific tier (macOS uses
// VideoToolbox). FFmpeg + Vulkan headers stay private to the cpp;
// this header forward-declares AVBufferRef only.
//
// Ported into UFB (app/player/) from QCView-Player
// (src/decode/vulkan_hw_device_ctx.*); namespace qcv -> ufbplayer.

#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

extern "C" {
struct AVBufferRef;
}

namespace ufbplayer {

// Allocates an AVBufferRef wrapping an AVVulkanDeviceContext that
// points at the shared VkDevice / VkInstance / physical device owned
// by VulkanDeviceManager. Returns nullptr on failure (caller is
// expected to fall back to software decode).
//
// Lifecycle: caller owns the returned buffer ref and must release
// with `av_buffer_unref` (or hand it to FFmpeg via
// `m_cctx->hw_device_ctx = av_buffer_ref(ref)`).
//
// Requires VulkanDeviceManager::instance().isInitialized() at call
// time — main.cpp's startup init must have run.
AVBufferRef *createSharedVulkanHwDeviceCtx();

} // namespace ufbplayer

#endif // Q_OS_WIN
