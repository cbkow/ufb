// MacWindowChrome.mm — see MacWindowChrome.h for the contract.

#include "MacWindowChrome.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include <QQuickWindow>

namespace ufb {

void applyMacWindowChrome(QQuickWindow* window) {
    if (!window) return;
    // Ensure native window creation so winId() returns a valid NSView.
    // QQuickWindow::create() is a no-op if already realized.
    window->create();
    NSView* view = reinterpret_cast<NSView*>(window->winId());
    if (!view) return;
    NSWindow* nsWin = [view window];
    if (!nsWin) return;

    nsWin.titlebarAppearsTransparent = YES;
    nsWin.titleVisibility = NSWindowTitleHidden;
    // INTENTIONALLY NOT setting NSWindowStyleMaskFullSizeContentView.
    // With it set, content extends under the titlebar and our QML
    // toolbar's MouseAreas capture the events that would otherwise
    // drag the window. Surfaced during slice-07 dogfood test.
    // The transparent-titlebar + hidden-title combo still gives the
    // Finder-like blended look; we just leave the OS-managed
    // titlebar strip with its native drag behaviour.
}

}  // namespace ufb

#endif  // __APPLE__
