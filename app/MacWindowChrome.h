// MacWindowChrome — Cocoa-side window-styling for top-level Qt
// windows. Applies the Finder-like blended-toolbar look:
//
//   • titlebarAppearsTransparent = YES
//   • titleVisibility = NSWindowTitleHidden
//   • styleMask |= NSWindowStyleMaskFullSizeContentView
//
// QML side can lay its toolbar over the title-bar region; traffic
// lights stay in their default top-left position.
//
// Per macOSplans/04 §3.

#pragma once

#ifdef __APPLE__

class QQuickWindow;

namespace ufb {

/// Apply native chrome to `window`. Idempotent — safe to call more
/// than once on the same window. No-op if `window` is null or hasn't
/// realized its native NSView yet.
void applyMacWindowChrome(QQuickWindow* window);

}  // namespace ufb

#endif  // __APPLE__
