// WindowChrome — the macOS half (see WindowChrome.h). Qt's
// ExpandedClientAreaHint + NoTitleBarBackgroundHint (set in Main.qml) extend
// the content under a transparent title bar and keep the native traffic
// lights; what Qt leaves on is the TITLE TEXT, which AppKit still centres
// in the (now transparent) title bar over our row. Hide it — the window
// title stays set for the Dock / app switcher. Everything else is AppKit's:
// zoom, fullscreen, tiling. (Replaces MacWindowChrome.mm, which set the same
// two properties but deliberately kept the OS title-bar strip because the
// toolbar's MouseAreas ate the drag — the row's own startSystemMove handle
// is the answer to that.)
#include "WindowChrome.h"

#include <QWindow>

#import <AppKit/AppKit.h>

void WindowChrome::attachMac(QWindow* w) {
    NSView* view = reinterpret_cast<NSView*>(w->winId());
    NSWindow* win = view ? view.window : nil;
    if (!win) return;
    win.titleVisibility = NSWindowTitleHidden;
    win.titlebarAppearsTransparent = YES;
}
