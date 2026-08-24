// UfbMenu — QtQuick.Controls Menu whose content ListView instantiates
// every delegate up front. Use it for EVERY Menu in the app (context
// menus, sort menus, submenus); plain `Menu {}` is a known GUI hang.
//
// Usage is identical to Menu:
//   UfbMenu { id: fileMenu; MenuItem { ... } MenuSeparator {} ... }
//
// Why (Windows, Qt 6.11, found 2026-08-24 — the "right-click crashes
// the app 30% of the time" report; see commit 0731cae): a menu that
// fits neither below nor above the cursor gets its height clamped by
// QQuickPopupPositioner. ListView.contentHeight is an *estimate*
// extrapolated from whichever delegates happen to be instantiated, and
// our menus hide items with `height: visible ? implicitHeight : 0`, so
// the estimate depends on which subset is loaded. Clamp -> fewer
// delegates -> new estimate -> implicitHeight -> the style's
// `height: __heightScale * implicitHeight` binding -> clamp again ->
// ... The GUI thread never leaves that polish pass and Windows reports
// "not responding". Three minidumps of a hung 1.1.3 showed exactly
// that cycle at constant stack depth. With every delegate instantiated
// contentHeight is exact and the loop cannot start; an overflowing
// menu simply scrolls. Native menus (macOS) never enter this path.
//
// The contentItem below is the FluentWinUI3 style's own, verbatim,
// plus `cacheBuffer`. If the style's Menu.qml changes its contentItem
// (spacing, ScrollIndicator, ...), mirror it here.

import QtQuick
import QtQuick.Controls

Menu {
    id: control

    contentItem: ListView {
        implicitHeight: contentHeight
        model: control.contentModel
        interactive: Window.window
                     ? contentHeight + control.topPadding + control.bottomPadding > control.height
                     : false
        currentIndex: control.currentIndex
        spacing: 4
        clip: true
        // The whole point of this file — see header comment.
        cacheBuffer: 1000000
        ScrollIndicator.vertical: ScrollIndicator {}
    }
}
