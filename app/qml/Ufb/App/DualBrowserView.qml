// The legacy DualBrowserView — sidebar | left FileBrowser | right
// FileBrowser. Lives as the default ("Files") tab in the main window.
// Sidebar lives at the application scope so it persists across tabs;
// this component just owns the two FileBrowsers and the active-pane
// handshake between them.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Item {
    id: root

    // The two backing models. Per-instance now that Directory is no
    // longer a singleton.
    property alias leftDir: leftDirObj
    property alias rightDir: rightDirObj
    /// Currently-focused pane. The host (Main) reads this to route
    /// sidebar clicks; each FileBrowser flips it on activation.
    property var activePane: leftPane

    /// Bubbled up from FileBrowser's "Transcode to MP4" menu item.
    signal openTranscodeQueueRequested()
    /// Bubbled up from FileBrowser's "Open in New Tab" menu item.
    /// Main spawns a new Files tab and seeds it with `path`.
    signal openInNewTabRequested(string path)

    /// When true, the right pane starts collapsed on first
    /// construction. Set by Main on tabs spawned via "+" or
    /// "Open in New Tab" so a freshly-opened tab feels uncluttered;
    /// the user can re-expand the right pane via its strip toggle.
    property bool startWithRightCollapsed: false

    /// Initial left-pane width in logical pixels. 0 = use 50/50.
    /// Read once from `Settings.inner_left_width()` at construction;
    /// any drag from there on writes back to Settings, so all
    /// DualBrowserViews share a single user-tuned default per the
    /// Option-A persistence design.
    property int initialLeftPaneWidth: 0

    /// Read/write aliases to each pane's collapse state. Main reads
    /// these when serializing tab state for persistence and writes
    /// them when restoring a saved session.
    property alias leftPaneCollapsed: leftPane.collapsed
    property alias rightPaneCollapsed: rightPane.collapsed
    /// Convenience: "left" / "right" form for status-strip display
    /// without poking into private children.
    readonly property string activeSide: activePane === leftPane ? "left" : "right"
    /// Current path of the active pane — convenience for window title.
    readonly property string activePath: activePane && activePane.dir
        ? activePane.dir.current_path : ""

    Directory { id: leftDirObj }
    Directory { id: rightDirObj }

    // Seed both panes at the user's home dir on first construction.
    // Per-pane path persistence (settings.browserPanels[id].path) is
    // a later concern — see plan-Phase 5 / 14.
    Component.onCompleted: {
        var home = Paths.home_path()
        leftDirObj.navigate_to(home)
        rightDirObj.navigate_to(home)
        if (startWithRightCollapsed) rightPane.collapsed = true
    }

    /// Forwards a navigate request from the host (sidebar click) to
    /// whichever pane is currently active. `selectAfterPath` is
    /// optional - when set, the pane's FileBrowser will highlight
    /// the matching row after the listing finishes loading. Used by
    /// the ufb:// deep-link flow so a file-target URI lands on its
    /// parent folder with the file already selected.
    function navigateActivePane(path, selectAfterPath) {
        var pane = root.activePane
        if (selectAfterPath && selectAfterPath.length > 0) {
            pane.selectAfterLoadPath = selectAfterPath
        }
        pane.dir.navigate_to(path)
    }

    /// Open a folder in a specific pane ("left" / "right"), expanding it
    /// first if it's collapsed, and make it the active pane. Optional
    /// `selectAfterPath` highlights a file once the listing loads. Used by
    /// the "Open in Left/Right Browser" actions in the project views.
    function openInPane(side, path, selectAfterPath) {
        var pane = (side === "right") ? rightPane : leftPane
        if (pane.collapsed) pane.collapsed = false
        if (selectAfterPath && selectAfterPath.length > 0) {
            pane.selectAfterLoadPath = selectAfterPath
        }
        pane.dir.navigate_to(path)
        root.activePane = pane
    }

    SplitView {
        id: panesSplit
        anchors.fill: parent
        orientation: Qt.Horizontal

        // Flat splitter — see Main.qml outerSplit's handle for the full
        // breakdown. 6px transparent hit zone, centered 1px hairline,
        // accent on hover/drag.
        handle: Rectangle {
            id: panesSplitHandle
            implicitWidth: 6
            implicitHeight: 6
            color: "transparent"
            readonly property bool _active: SplitHandle.hovered || SplitHandle.pressed
            Rectangle {
                anchors.centerIn: parent
                width: panesSplitHandle.width <= panesSplitHandle.height ? 1 : panesSplitHandle.width
                height: panesSplitHandle.width <= panesSplitHandle.height ? panesSplitHandle.height : 1
                color: panesSplitHandle._active
                    ? Theme.colors.accent
                    : Theme.colors.divider
            }
        }

        // Width when a pane is collapsed - just enough for the
        // toggle button at toolStripHeight to sit on its own.
        readonly property int _collapsedWidth: Theme.dim.toolStripHeight + 2
        // Sentinel for "no maximum constraint". SplitView's default
        // is a huge number; -1 is NOT the unset sentinel for max, it
        // pins the pane to 0 width.
        readonly property int _noMaxWidth: 0x7fffffff

        // Drag-end → write the new shared default to Settings. Every
        // DualBrowserView in the app reads from the same setting on
        // construction, so this drag becomes the new starting point
        // for any tab spawned afterward.
        onResizingChanged: {
            if (!resizing
                && !leftPane.collapsed
                && !rightPane.collapsed
                && leftPane.width > 0) {
                Settings.set_inner_left_width(leftPane.width)
            }
        }

        FileBrowser {
            id: leftPane
            dir: leftDirObj
            active: root.activePane === leftPane
            paneSide: "left"
            // Width modes:
            //   - both expanded:    saved shared default, else 50/50
            //   - left collapsed:   locked to strip width
            //   - right collapsed:  fillWidth, no preferred (take all
            //                       leftover space)
            //
            // The expanded preferredWidth is a binding; user splitter
            // drags break the binding and imperative widths take over
            // from then on. The drag-end save above re-syncs all
            // future tabs to whatever the user just dragged to.
            SplitView.fillWidth: !collapsed && rightPane.collapsed
            SplitView.preferredWidth: {
                if (collapsed) return panesSplit._collapsedWidth
                if (rightPane.collapsed) return -1   // sentinel: use fillWidth
                return root.initialLeftPaneWidth > 0
                    ? root.initialLeftPaneWidth
                    : panesSplit.width / 2
            }
            SplitView.minimumWidth: collapsed ? panesSplit._collapsedWidth : 280
            SplitView.maximumWidth: collapsed
                ? panesSplit._collapsedWidth
                : panesSplit._noMaxWidth

            onActivated: root.activePane = leftPane
            // If the user collapses the active pane, the active-pane
            // border (accent outline) and any keyboard focus would be
            // wasted on a strip the user can't see. Hand activation
            // off to the still-visible pane.
            onCollapsedChanged: {
                if (collapsed && root.activePane === leftPane && !rightPane.collapsed)
                    root.activePane = rightPane
            }
            onOpenInOtherBrowserRequested: (path) => {
                rightDirObj.navigate_to(path)
                root.activePane = rightPane
            }
            onOpenInNewTabRequested: (path) => root.openInNewTabRequested(path)
            onOpenTranscodeQueueRequested: root.openTranscodeQueueRequested()
        }

        FileBrowser {
            id: rightPane
            dir: rightDirObj
            active: root.activePane === rightPane
            paneSide: "right"
            // Right pane fills by default; its preferredWidth gets
            // pinned only when collapsed.
            SplitView.fillWidth: !collapsed
            SplitView.preferredWidth: collapsed
                ? panesSplit._collapsedWidth
                : -1
            SplitView.minimumWidth: collapsed ? panesSplit._collapsedWidth : 280
            SplitView.maximumWidth: collapsed
                ? panesSplit._collapsedWidth
                : panesSplit._noMaxWidth

            onActivated: root.activePane = rightPane
            onCollapsedChanged: {
                if (collapsed && root.activePane === rightPane && !leftPane.collapsed)
                    root.activePane = leftPane
            }
            onOpenInOtherBrowserRequested: (path) => {
                leftDirObj.navigate_to(path)
                root.activePane = leftPane
            }
            onOpenInNewTabRequested: (path) => root.openInNewTabRequested(path)
            onOpenTranscodeQueueRequested: root.openTranscodeQueueRequested()
        }
    }
}
