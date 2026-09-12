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
// menu simply scrolls. Applies on both OSes: the FluentWinUI3 style
// never requests native menus, so macOS renders these too (which is
// also why UfbMenuItem's icons and styling work identically there).
//
// The contentItem below is the FluentWinUI3 style's own, verbatim,
// plus `cacheBuffer`. If the style's Menu.qml changes its contentItem
// (spacing, ScrollIndicator, ...), mirror it here.

import QtQuick
import QtQuick.Controls

Menu {
    id: control

    // Widest visible item, so labels never elide. The style's Menu has
    // no implicit content width (a ListView reports 0) and falls back
    // to its 200px background, which clipped "Compress 2 Items to ZIP".
    // Reading each item's implicitWidth/visible inside the binding
    // keeps it live as items are toggled in onAboutToShow.
    readonly property int maxContentWidth: 420
    function _widestItem() {
        let w = 0
        for (let i = 0; i < control.count; ++i) {
            const it = control.itemAt(i)
            if (it && it.visible)
                w = Math.max(w, it.implicitWidth)
        }
        return Math.min(w, maxContentWidth)
    }

    contentItem: ListView {
        implicitHeight: contentHeight
        implicitWidth: control._widestItem()
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
