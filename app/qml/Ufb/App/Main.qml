// Phase 10 slice A — main window with tab bar.
//
// Layout: SplitView { Sidebar | ColumnLayout { TabBar + StackLayout } }
// First tab is the legacy DualBrowserView (sidebar lives outside, so
// effectively just two FileBrowsers in a SplitView). Subsequent tabs
// are JobView instances opened from sidebar subscription clicks.
//
// Tab-close: each JobView tab has a small × button on the right side
// that removes it from `openJobs`. The Files tab is non-closeable.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Qt.labs.platform as Labs

import Ufb.Backend 1.0

ApplicationWindow {
    id: window
    title: {
        if (tabBar.currentIndex < filesTabs.count) {
            var v = _currentFilesView
            return v ? qsTr("UFB — %1").arg(v.activePath) : qsTr("UFB")
        }
        var jobIdx = tabBar.currentIndex - filesTabs.count
        if (jobIdx >= 0 && jobIdx < openJobs.count) {
            return qsTr("UFB — %1").arg(openJobs.get(jobIdx).jobName)
        }
        return qsTr("UFB")
    }
    // Start hidden so the size restore in Component.onCompleted lands
    // before the window paints — otherwise the user sees the default
    // 1480×880 paint, then a resize flash to the persisted size.
    visible: false
    width: 1480
    height: 880
    minimumWidth: 900
    minimumHeight: 540

    color: Theme.colors.surface

    // ── Window geometry persistence ──────────────────────────────────
    // Restored from settings.json's `window` field on launch and saved
    // back (debounced) on resize/move. The `_geometryRestored` flag
    // gates the save handlers so the initial restore doesn't kick off
    // a no-op write loop.
    //
    // The _restored* fields hold the latest *windowed-mode* geometry.
    // Reading window.x/y/width/height while the window is maximized
    // gives us the maximized rect, not the un-maximize rect. If we
    // save those, the next launch un-maximizes to (0, 0, screen, screen)
    // - obviously wrong. By only updating _restored* when visibility
    // is Windowed, we preserve a usable "restore to" geometry across
    // a maximize/save/relaunch cycle.
    property bool _geometryRestored: false
    // Windows only: onCompleted leaves the window hidden and stashes the
    // restored maximized state here; main.cpp shows the window (cloaked
    // until its first frame, to kill the white startup flash) and reads
    // this to choose Maximized vs Windowed. Other platforms show + set
    // visibility directly in onCompleted.
    property bool startMaximized: false
    property int _restoredX: -1
    property int _restoredY: -1
    property int _restoredW: 1480
    property int _restoredH: 880

    /// Returns true if (x, y, w, h) overlaps any current screen by at
    /// least 50px on each axis. Used to fall back to default placement
    /// when the saved coordinates were on a now-disconnected monitor.
    function _windowFitsAnyScreen(x, y, w, h) {
        var screens = Qt.application.screens
        for (var i = 0; i < screens.length; ++i) {
            var s = screens[i].virtualGeometry
            var ix = Math.max(x, s.x)
            var iy = Math.max(y, s.y)
            var ax = Math.min(x + w, s.x + s.width)
            var ay = Math.min(y + h, s.y + s.height)
            if (ax - ix > 50 && ay - iy > 50) return true
        }
        return false
    }

    // ── One-app tray (plans/17 slice E) ───────────────────────────────
    // The GUI hosts the tray itself: UFBTray.app (macOS) and the
    // agent's Win32 tray are superseded. Closing the window keeps the
    // app resident (quit-on-last-window-closed off in main.cpp);
    // Quit here is the real exit.
    property var _trayMounts: {
        try { return JSON.parse(Mount.mount_states_json || "[]") }
        catch (e) { return [] }
    }
    function _showFromTray() {
        window.show()
        window.raise()
        window.requestActivate()
    }
    /// True once Component.onCompleted has made the initial show/hide
    /// decision. Gates the reactivation hook below so it can't show
    /// the window early (before geometry restore) during launch.
    property bool _startupShowDone: false
    // One-app lifecycle: reactivating an already-running instance
    // (Dock / Finder / Launchpad click) sends a reopen on macOS — no
    // second process, no singleton IPC — and Qt shows nothing for it.
    // With close-to-tray that made clicking the app appear dead. Any
    // activation while the window is closed brings it back. Login-item
    // --background launches start Inactive, so tray-only startup is
    // unaffected. (Second-process launches are covered separately by
    // the singleton forward → onUriRequested.)
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive
                    && window._startupShowDone
                    && !window.visible) {
                window._showFromTray()
            }
        }
    }
    // Dock icon follows the window (macOS): menu-bar-only while the
    // window is closed, Regular (Dock + Cmd-Tab) while it's up.
    // AppCtl.setDockIconVisible is a no-op on other platforms.
    onVisibleChanged: {
        if (typeof AppCtl !== "undefined" && AppCtl !== null)
            AppCtl.setDockIconVisible(window.visible)
    }
    Labs.SystemTrayIcon {
        id: trayIcon
        visible: true
        icon.source: "qrc:/icons/icon.png"
        // macOS: flag as a template so the menu bar tints it; the
        // colored png still reads fine on Windows.
        icon.mask: Qt.platform.os === "osx"
        tooltip: {
            var up = 0
            for (var i = 0; i < window._trayMounts.length; ++i)
                if (window._trayMounts[i].state === "mounted") up++
            return qsTr("UFB — %1/%2 mounts up").arg(up).arg(window._trayMounts.length)
        }
        onActivated: (reason) => {
            // Windows convention: click opens the app. macOS shows the
            // menu on any click (handled natively); Trigger only fires
            // where menus aren't click-bound.
            if (reason === Labs.SystemTrayIcon.Trigger
                    || reason === Labs.SystemTrayIcon.DoubleClick)
                window._showFromTray()
        }
        menu: Labs.Menu {
            Labs.MenuItem {
                text: qsTr("Open UFB")
                onTriggered: window._showFromTray()
            }
            Labs.MenuSeparator {}
            Labs.Menu {
                id: trayMountsMenu
                title: qsTr("Mounts")
            }
            Labs.MenuSeparator {}
            Labs.MenuItem {
                text: qsTr("Quit UFB")
                onTriggered: Qt.quit()
            }
        }
    }
    Instantiator {
        model: window._trayMounts
        delegate: Labs.MenuItem {
            text: (modelData.unmanaged === true
                       ? "◆ "
                       : (modelData.state === "mounted" ? "● " : "○ "))
                  + (modelData.displayName || modelData.mountId)
            onTriggered: {
                // Bookmark-only entries have no lifecycle — clicking
                // one opens the app at that location.
                if (modelData.unmanaged === true) {
                    window._showFromTray()
                    if ((modelData.volumePath || "").length > 0)
                        window._routeResolvedPath(modelData.volumePath, 0)
                    return
                }
                if (modelData.state === "mounted")
                    Mount.stop_mount(modelData.mountId)
                else
                    Mount.start_mount(modelData.mountId)
            }
        }
        onObjectAdded: (index, object) => trayMountsMenu.insertItem(index, object)
        onObjectRemoved: (index, object) => trayMountsMenu.removeItem(object)
    }

    Timer {
        id: _windowSaveTimer
        interval: 500
        repeat: false
        onTriggered: {
            // Don't persist a minimized window as minimized - that
            // would resurrect to a hidden taskbar icon next launch.
            // Only Maximized is meaningful for restoration.
            var maximized = window.visibility === Window.Maximized
            // Save the windowed geometry, NOT the live x/y/width/height
            // - those are the maximized rect when visibility is
            // Maximized, which would corrupt the un-maximize state.
            Settings.set_window_state(
                window._restoredX, window._restoredY,
                window._restoredW, window._restoredH,
                maximized
            )
        }
    }

    function _captureWindowedGeometry() {
        // Only update the "restore to" geometry when the window is
        // actually windowed - capturing while Maximized would store
        // the maximized rect as the un-maximize target.
        if (window.visibility === Window.Windowed) {
            window._restoredX = window.x
            window._restoredY = window.y
            window._restoredW = window.width
            window._restoredH = window.height
        }
    }

    onWidthChanged: {
        _captureWindowedGeometry()
        if (_geometryRestored) _windowSaveTimer.restart()
    }
    onHeightChanged: {
        _captureWindowedGeometry()
        if (_geometryRestored) _windowSaveTimer.restart()
    }
    onXChanged: {
        _captureWindowedGeometry()
        if (_geometryRestored) _windowSaveTimer.restart()
    }
    onYChanged: {
        _captureWindowedGeometry()
        if (_geometryRestored) _windowSaveTimer.restart()
    }
    onVisibilityChanged: {
        _captureWindowedGeometry()
        if (_geometryRestored) _windowSaveTimer.restart()
    }

    // ── Tab state persistence ─────────────────────────────────────────
    // Saved on tab open/close (immediate via _saveTabState) and on
    // path-nav / index changes (debounced 500ms via _tabSaveTimer).
    // Restored on launch by _restoreTabState() in Component.onCompleted.
    property bool _tabsRestored: false

    Timer {
        id: _tabSaveTimer
        interval: 500
        repeat: false
        onTriggered: window._saveTabState()
    }

    function _saveTabState() {
        if (!window._tabsRestored) return
        var files = []
        for (var i = 0; i < filesRepeater.count; ++i) {
            var v = filesRepeater.itemAt(i)
            if (!v) continue
            files.push({
                leftPath: v.leftDir ? v.leftDir.current_path : "",
                rightPath: v.rightDir ? v.rightDir.current_path : "",
                leftCollapsed: v.leftPaneCollapsed === true,
                rightCollapsed: v.rightPaneCollapsed === true,
            })
        }
        var jobs = []
        for (var j = 0; j < openJobs.count; ++j) {
            jobs.push({ jobPath: openJobs.get(j).jobPath })
        }
        var state = {
            currentIndex: tabBar.currentIndex,
            files: files,
            jobs: jobs,
            trackerOpen: window.trackerTabOpen === true,
            transcodeOpen: window.transcodeTabOpen === true,
        }
        Settings.set_tab_state(JSON.stringify(state))
    }

    function _restoreTabState() {
        var state
        try {
            state = JSON.parse(Settings.tab_state_json())
        } catch (e) {
            console.warn("Main: tab-state parse failed:", e)
            window._tabsRestored = true
            return
        }
        if (!state || !state.files || state.files.length === 0) {
            // No saved state - leave the seeded first tab alone.
            window._tabsRestored = true
            return
        }

        // Apply state.files[0] to the existing seeded first tab. Its
        // DualBrowserView's Component.onCompleted has already run with
        // home-dir defaults and startWithRightCollapsed=false; we
        // override here.
        var first = filesRepeater.itemAt(0)
        if (first) {
            if (state.files[0].leftPath && state.files[0].leftPath.length > 0)
                first.leftDir.navigate_to(state.files[0].leftPath)
            if (state.files[0].rightPath && state.files[0].rightPath.length > 0)
                first.rightDir.navigate_to(state.files[0].rightPath)
            first.leftPaneCollapsed = state.files[0].leftCollapsed === true
            first.rightPaneCollapsed = state.files[0].rightCollapsed === true
        }

        // Append additional Files tabs. Bypasses addFilesTab() so we
        // don't fight the deferred currentIndex assignment - we'll
        // set it ourselves at the end.
        for (var i = 1; i < state.files.length; ++i) {
            var ft = state.files[i]
            filesTabs.append({
                tabId: window._nextFilesTabId++,
                startWithRightCollapsed: ft.rightCollapsed === true,
            });
            // Defer the path nav + collapse-restore until the
            // Repeater has produced the delegate. The leading `;`
            // above is load-bearing: without it, JS ASI parses the
            // IIFE that follows as `append(...)(...)` — a call on
            // the undefined return of append — which throws and
            // aborts the rest of the restore (jobs / tracker /
            // transcode / currentIndex would all be skipped).
            (function(idx, savedFt) {
                Qt.callLater(function() {
                    var view = filesRepeater.itemAt(idx)
                    if (!view) return
                    if (savedFt.leftPath && savedFt.leftPath.length > 0)
                        view.leftDir.navigate_to(savedFt.leftPath)
                    if (savedFt.rightPath && savedFt.rightPath.length > 0)
                        view.rightDir.navigate_to(savedFt.rightPath)
                    view.leftPaneCollapsed = savedFt.leftCollapsed === true
                    view.rightPaneCollapsed = savedFt.rightCollapsed === true
                })
            })(filesTabs.count - 1, ft)
        }

        // Restore job tabs. Skip any whose source path no longer
        // exists - silently dropped per design.
        if (state.jobs && state.jobs.length > 0) {
            for (var k = 0; k < state.jobs.length; ++k) {
                var jp = state.jobs[k].jobPath
                if (!jp || jp.length === 0) continue
                if (!Paths.path_exists(jp)) continue
                var jn = Paths.basename_of(jp)
                openJobs.append({ jobPath: jp, jobName: jn })
            }
        }

        // Restore tracker/transcode singletons.
        if (state.trackerOpen) window.trackerTabOpen = true
        if (state.transcodeOpen) window.transcodeTabOpen = true

        // Restore active tab. Defer so all Repeater delegates have
        // materialised - StackLayout would otherwise pin to a not-
        // yet-created slot and fall back to the previous index.
        Qt.callLater(function() {
            var ci = state.currentIndex || 0
            // Clamp to the actual new tab count to avoid pointing
            // at a tab the user closed before the previous shutdown.
            var maxIdx = filesTabs.count + openJobs.count
                + (window.trackerTabOpen ? 1 : 0)
                + (window.transcodeTabOpen ? 1 : 0) - 1
            if (ci > maxIdx) ci = 0
            tabBar.currentIndex = Math.max(0, ci)
            window._tabsRestored = true
        })
    }

    Connections {
        target: tabBar
        function onCurrentIndexChanged() {
            if (window._tabsRestored) _tabSaveTimer.restart()
        }
    }
    onTrackerTabOpenChanged: if (_tabsRestored) _saveTabState()
    onTranscodeTabOpenChanged: if (_tabsRestored) _saveTabState()

    // ── Splitter persistence ─────────────────────────────────────────
    // Two dimensions, both stored on Settings: the outer sidebar width
    // (this file) and the inner DualBrowser left-pane width (read by
    // each DualBrowserView delegate at construction). Per Option-A
    // design, all DualBrowserViews share the same inner default - new
    // tabs from "+" or "Open in New Tab" inherit whichever value the
    // user last dragged any DualBrowserView's splitter to.
    property int _sidebarWidth: 0       // 0 = use 220 default
    property int _filesLeftWidth: 0     // 0 = use 50/50 default
    property bool _splittersRestored: false

    function _restoreSplitterState() {
        window._sidebarWidth = Settings.outer_sidebar_width()
        window._filesLeftWidth = Settings.inner_left_width()
        Qt.callLater(function() { window._splittersRestored = true })
    }

    /// Open job tabs. Each entry: { jobPath, jobName }.
    /// Open Files tabs. Always >= 1 - the user can spawn additional
    /// Files tabs via the "+" button in the tab bar, and close any
    /// of them as long as one remains. Each Files tab is backed by
    /// an independent DualBrowserView in the StackLayout, so their
    /// active-pane state, navigation history, and selection don't
    /// cross-contaminate.
    /// Tab indices in the StackLayout:
    ///   0..F-1            = filesTabs[0..F-1]   (DualBrowserViews)
    ///   F..F+J-1          = openJobs[0..J-1]    (JobViews)
    ///   F+J               = aggregated TrackerView (always present)
    ///   F+J+1             = TranscodeQueue          (always present)
    /// The tab-strip controls (`*TabOpen`) gate which pills show in
    /// the visual tab bar; the underlying StackLayout slots are
    /// fixed and the index helpers below resolve to those slots.
    ListModel {
        id: filesTabs
        // First tab keeps both panes visible. Subsequent tabs (added
        // via "+" or "Open in New Tab") flip startWithRightCollapsed
        // to true so a freshly-spawned tab feels uncluttered. Declared
        // as a ListElement so the row exists at parse time - using
        // Component.onCompleted to seed the row meant the Repeater
        // delegate's binding read the role before append landed,
        // which left the first tab in an inconsistent layout state.
        ListElement { tabId: 0; startWithRightCollapsed: false }
    }
    property int _nextFilesTabId: 1
    /// Tracks the most-recently-active Files tab. The sidebar uses
    /// this to route navigate-requests when the user has a non-Files
    /// tab currently visible: rather than always falling back to
    /// index 0, navigation lands in the Files tab they last had open.
    property int _lastActiveFilesTabIndex: 0
    ListModel { id: openJobs }
    property bool trackerTabOpen: false
    property bool transcodeTabOpen: false
    readonly property int _trackerTabIndex:
        trackerTabOpen ? filesTabs.count + openJobs.count : -1
    readonly property int _transcodeTabIndex:
        transcodeTabOpen ? filesTabs.count + openJobs.count + 1 : -1

    /// The currently-visible DualBrowserView, or null if a non-Files
    /// tab is active. Title bar + footer + sidebar routing read this
    /// instead of a hard-coded `dualBrowser` id.
    /// Note: itemAt() doesn't notify on its own, but the binding
    /// re-evaluates whenever currentIndex / filesTabs.count change,
    /// which covers every transition that affects the result.
    readonly property var _currentFilesView: {
        var i = tabBar.currentIndex
        if (i < 0 || i >= filesTabs.count) return null
        return filesRepeater.itemAt(i)
    }
    /// Most-recently-active Files tab as a DualBrowserView. Used by
    /// the sidebar when no Files tab is currently in front.
    readonly property var _lastFilesView: {
        var i = _lastActiveFilesTabIndex
        if (i < 0 || i >= filesTabs.count) i = 0
        return filesRepeater.itemAt(i)
    }

    Connections {
        target: tabBar
        function onCurrentIndexChanged() {
            // Track the last Files tab the user had focus on so the
            // sidebar can route navigation back to it from a job tab.
            if (tabBar.currentIndex >= 0
                && tabBar.currentIndex < filesTabs.count) {
                window._lastActiveFilesTabIndex = tabBar.currentIndex
            }
        }
    }

    /// Refresh the currently-visible Files tab's panes whenever the
    /// app regains focus. Catches changes the user made via Explorer,
    /// another tool, or a network drop while UFB was in the background.
    /// Non-Files tabs (Job / Tracker / Transcode) are left to their
    /// own logic - cheap on focus regain since they don't hammer disk.
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state !== Qt.ApplicationActive) return
            var v = window._currentFilesView
            if (!v) return
            if (v.leftDir) v.leftDir.refresh()
            if (v.rightDir) v.rightDir.refresh()
        }
    }

    /// Single-instance deep-link delivery. AppController (registered
    /// as `AppCtl` from main.cpp) emits `uriRequested` whenever a
    /// secondary UFB process forwards its launch URI to us via
    /// QLocalSocket. We raise the window, resolve the URI through
    /// path-mapping swaps, and navigate the active Files tab.
    Connections {
        target: typeof AppCtl !== "undefined" ? AppCtl : null
        function onUriRequested(path) {
            // Always raise/focus, even on an empty payload - the user
            // re-launched and the most useful thing is the existing
            // window coming forward.
            window.show()
            window.raise()
            window.requestActivate()
            if (!path || path.length === 0) return
            var resolved = FileOps.resolve_ufb_uri(path)
            if (resolved.length === 0) {
                console.warn("Main: AppCtl uri unresolvable:", path)
                return
            }
            // File-target URIs (e.g. links straight to a .mov / .ai)
            // can't be passed to navigate_to - it'd hit list_directory
            // on a regular file and surface "Not a directory or cannot
            // read". Drop to the parent folder and ask the FileBrowser
            // to select the file after the listing loads.
            // Subscription-aware: a link under a subscribed job opens in
            // that project's tab + panel; otherwise the browser navigates.
            window._routeResolvedPath(resolved, window._lastActiveFilesTabIndex)
        }
    }

    /// Resolve a path the URI/launch-arg flow has produced into a
    /// `{ folder, select }` tuple for `navigateActivePane`. Mirrors
    /// the Tauri build's deep-link semantics:
    ///   * directory     → navigate to it; no selection
    ///   * existing file → navigate to parent; select the file
    ///   * missing path  → navigate to parent if it exists (selection
    ///                     attempted by name-match in case of cross-
    ///                     machine path swap); else hand the raw path
    ///                     to navigate_to so the user sees the error.
    function _navigateTargetFor(resolved) {
        if (!resolved || resolved.length === 0) {
            return { folder: "", select: "" }
        }
        if (Paths.is_directory(resolved)) {
            return { folder: resolved, select: "" }
        }
        if (Paths.path_exists(resolved)) {
            // Existing non-directory: a file. Use the parent + select.
            return { folder: Paths.parent_of(resolved), select: resolved }
        }
        var parent = Paths.parent_of(resolved)
        if (parent.length > 0 && Paths.is_directory(parent)) {
            console.warn(
                "Main: target path missing, falling back to parent:",
                resolved, "->", parent)
            // Pass the original path as the select hint - FileBrowser
            // will fall back to a bare-filename match if the full
            // path doesn't line up (cross-machine layouts).
            return { folder: parent, select: resolved }
        }
        console.warn("Main: target path doesn't exist on this machine.",
                     "Configure path swaps if this is a cross-machine link:",
                     resolved)
        return { folder: resolved, select: "" }
    }

    /// Append a new Files tab and switch to it. If `initialPath` is
    /// non-empty the new tab navigates its active pane there; used by
    /// the "Open in New Tab" context-menu action. Newly-spawned tabs
    /// start with the right pane collapsed by default - signalled via
    /// the model's `startWithRightCollapsed` role, read by the
    /// DualBrowserView delegate at construction. Defer the index
    /// assignment so the Repeater can produce the new delegate
    /// before StackLayout latches onto it.
    function addFilesTab(initialPath) {
        var newIdx = filesTabs.count
        filesTabs.append({
            tabId: window._nextFilesTabId++,
            startWithRightCollapsed: true,
        })
        Qt.callLater(function() {
            tabBar.currentIndex = newIdx
            if (initialPath && initialPath.length > 0) {
                var view = filesRepeater.itemAt(newIdx)
                if (view) view.navigateActivePane(initialPath)
            }
            window._saveTabState()
        })
    }

    /// Open `path` in the main dual browser's left or right pane.
    /// `side` is "left" / "right". Switches to a Files tab (the
    /// last-active one, or tab 0), expands the target pane if it's
    /// collapsed, navigates it to the folder, and selects the file when
    /// `path` is a file. Backs the "Open in Left/Right Browser" context-
    /// menu items in the project views. Spawns a Files tab if none exist.
    function openPathInBrowser(side, path) {
        if (!path || path.length === 0) return
        var t = window._navigateTargetFor(path)
        if (t.folder.length === 0) return

        function deliver(idx) {
            tabBar.currentIndex = idx
            var v = filesRepeater.itemAt(idx)
            if (v && v.openInPane) {
                v.openInPane(side, t.folder, t.select)
            } else {
                // Delegate not realized yet (just-switched / just-spawned);
                // retry on the next tick once the Repeater produces it.
                Qt.callLater(function() {
                    var v2 = filesRepeater.itemAt(idx)
                    if (v2 && v2.openInPane) v2.openInPane(side, t.folder, t.select)
                })
            }
        }

        if (filesTabs.count === 0) {
            filesTabs.append({
                tabId: window._nextFilesTabId++,
                startWithRightCollapsed: false,
            })
            Qt.callLater(function() { deliver(0); window._saveTabState() })
            return
        }
        var idx = window._lastActiveFilesTabIndex
        if (idx < 0 || idx >= filesTabs.count) idx = 0
        deliver(idx)
    }

    /// Close every tab except the first Files tab. The first Files
    /// tab keeps its current paths - we don't reset it to home,
    /// which would destroy whatever the user was just looking at.
    /// Reachable from the broom button at the far right of the tab
    /// strip; the button is gated to "non-trivial" cases (more than
    /// just the first Files tab open) so an idle click is a no-op.
    function closeAllTabs() {
        // Drop additional Files tabs (work back-to-front so indices
        // don't shift mid-loop).
        while (filesTabs.count > 1) {
            filesTabs.remove(filesTabs.count - 1)
        }
        // Drop all job tabs.
        while (openJobs.count > 0) {
            openJobs.remove(openJobs.count - 1)
        }
        // Singletons.
        window.trackerTabOpen = false
        window.transcodeTabOpen = false
        // Land on the surviving first tab.
        tabBar.currentIndex = 0
        window._lastActiveFilesTabIndex = 0
        window._saveTabState()
    }

    /// Returns true when the broom click would actually close
    /// something. Avoids prompting the user for a no-op.
    function _hasClosableTabs() {
        return filesTabs.count > 1
            || openJobs.count > 0
            || window.trackerTabOpen
            || window.transcodeTabOpen
    }

    /// Close every tab EXCEPT the first Files tab and the tab at
    /// `keepTabIdx` (the one the context menu was opened on). The first
    /// Files tab is always kept (it's the app's non-closeable home, like
    /// closeAllTabs); the kept tab survives alongside it. If `keepTabIdx`
    /// IS the first Files tab, this collapses to "close everything else."
    function closeOtherTabs(keepTabIdx) {
        var F = filesTabs.count
        var J = openJobs.count
        var isFiles = keepTabIdx >= 0 && keepTabIdx < F
        var isJob   = keepTabIdx >= F && keepTabIdx < F + J
        var isTracker   = window.trackerTabOpen
                          && keepTabIdx === window._trackerTabIndex
        var isTranscode = window.transcodeTabOpen
                          && keepTabIdx === window._transcodeTabIndex
        var keptFilesIdx = isFiles ? keepTabIdx : -1
        var keptJobIdx   = isJob ? (keepTabIdx - F) : -1

        // Files: keep index 0 plus the kept Files tab (if any). Work
        // back-to-front so removals don't shift surviving indices.
        for (var i = filesTabs.count - 1; i >= 1; --i) {
            if (i !== keptFilesIdx)
                filesTabs.remove(i)
        }
        // Jobs: keep only the kept job tab (if any).
        for (var j = openJobs.count - 1; j >= 0; --j) {
            if (j !== keptJobIdx)
                openJobs.remove(j)
        }
        // Singletons survive only if they're the kept tab.
        window.trackerTabOpen = isTracker
        window.transcodeTabOpen = isTranscode

        // Land on the kept tab: the first Files tab stays at index 0;
        // anything else now sits at index 1 (right after it).
        tabBar.currentIndex = (keepTabIdx === 0) ? 0 : 1
        window._lastActiveFilesTabIndex = (isFiles && keptFilesIdx > 0) ? 1 : 0
        window._saveTabState()
    }

    /// True when "Close Other Tabs" (keeping the first Files tab + the
    /// tab at `keepTabIdx`) would actually close something — so the menu
    /// item can disable itself for a no-op.
    function _hasOtherTabs(keepTabIdx) {
        var total = filesTabs.count + openJobs.count
            + (window.trackerTabOpen ? 1 : 0)
            + (window.transcodeTabOpen ? 1 : 0)
        // Always keep the first Files tab; plus the kept tab if it isn't
        // already the first Files tab.
        var keep = (keepTabIdx === 0) ? 1 : 2
        return total > keep
    }

    /// Close the Files tab at `idx`. Refuses to close the last one
    /// (the app always keeps at least one Files tab around).
    function closeFilesTab(idx) {
        if (filesTabs.count <= 1) return
        if (idx < 0 || idx >= filesTabs.count) return
        var currentTab = tabBar.currentIndex
        filesTabs.remove(idx)
        // Adjust currentIndex for the shift.
        if (currentTab === idx) {
            tabBar.currentIndex = Math.max(0, idx - 1)
        } else if (currentTab > idx) {
            tabBar.currentIndex = currentTab - 1
        }
        // Same shift for the last-active pointer.
        if (window._lastActiveFilesTabIndex === idx) {
            window._lastActiveFilesTabIndex = Math.max(0, idx - 1)
        } else if (window._lastActiveFilesTabIndex > idx) {
            window._lastActiveFilesTabIndex = window._lastActiveFilesTabIndex - 1
        }
        window._saveTabState()
    }

    /// Live mirror of FileOps.active_ops_json — JS array of
    /// {id, operation, itemsTotal, itemsDone, totalBytes,
    /// copiedBytes, currentFile, progress}. Repeater in the footer
    /// renders one chip per active op.
    property var _activeOps: []
    /// Brief "X file(s) copied" / "Y deleted" message that lingers
    /// for ~3 seconds after the last op completes, so the user gets
    /// confirmation even if the chips disappear quickly.
    property string _lastOpStatus: ""
    Timer {
        id: _opStatusTimer
        interval: 3500
        onTriggered: window._lastOpStatus = ""
    }
    Connections {
        target: FileOps
        function onActive_ops_jsonChanged() {
            try {
                var arr = JSON.parse(FileOps.active_ops_json)
                if (window._activeOps.length > 0 && arr.length === 0) {
                    // Last op just finished. Show a brief confirmation.
                    var prev = window._activeOps[window._activeOps.length - 1]
                    var verb = prev.operation === "copy"   ? qsTr("copied")
                             : prev.operation === "move"   ? qsTr("moved")
                             : prev.operation === "delete" ? qsTr("deleted")
                             : qsTr("done")
                    window._lastOpStatus = qsTr("✓ %1 %2 file(s)")
                        .arg(prev.itemsTotal || prev.itemsDone || 0)
                        .arg(verb)
                    _opStatusTimer.restart()
                }
                window._activeOps = arr
            } catch (e) {
                console.warn("Main: active_ops_json parse failed:", e)
            }
        }
    }

    Component.onCompleted: {
        // Touch the v2 templates dir on startup so the
        // misplaced-file recovery and (Phase 6) manifest pull run
        // before any column-add path needs them. Cheap on a healthy
        // share (one stat + a tiny file read); skips silently when
        // the share is unreachable.
        try { Columns.refresh_templates() } catch (e) {}

        // Restore window SIZE + maximized state only.
        //
        // We deliberately skip restoring x/y. On Windows multi-monitor
        // setups with mixed DPI scaling, fighting Qt's initial show-on-
        // primary-screen placement to drop the window onto a different
        // monitor's coords is a tar pit:
        //   - Qt 6 defaults to PassThrough DPI rounding, but logical
        //     coords from a 150%-scaled monitor mean different physical
        //     pixels than the same logical coords from a 100% monitor.
        //   - setGeometry() before the window has been shown can be
        //     reinterpreted on whatever screen Qt picks first.
        //   - QWindow doesn't expose saveGeometry()/restoreGeometry()
        //     (those are QMainWindow-only) which is the only reliable
        //     way Qt knows how to round-trip multi-screen state.
        //
        // The pragmatic choice: let Qt place the window where it likes
        // on launch. The user adjusts once per session; size +
        // maximized state DO restore correctly, which covers the
        // common "I want my big window back" case. Position data is
        // still shipped in the settings struct in case we find a
        // clean restore path later (Phase 14+ macOS work may need it).
        try {
            var ws = JSON.parse(Settings.window_state_json())
            var w = (ws.width > 0) ? ws.width : 1480
            var h = (ws.height > 0) ? ws.height : 880
            window.width = w
            window.height = h
            window._restoredW = w
            window._restoredH = h
            window._restoredX = -1
            window._restoredY = -1
            if (ws.maximized) {
                // On Windows defer the actual show to main.cpp (flash
                // fix); elsewhere set visibility directly here.
                if (Qt.platform.os === "windows")
                    window.startMaximized = true
                else
                    window.visibility = Window.Maximized
            }
        } catch (e) {
            console.warn("Main: window-state restore failed:", e)
        }
        // Defer the "we're done restoring" flag so any synchronous
        // resize/move triggered by the lines above doesn't kick the
        // save timer.
        Qt.callLater(function() { window._geometryRestored = true })

        // Auto-spawn the agent + open the IPC connection so mounts
        // come up without manual intervention.
        Mount.start()
        // Auto-enable mesh sync if the user opted in via the
        // settings dialog. No-op when not configured.
        Mesh.start()

        // Restore splitter widths before tab content materialises so
        // each DualBrowserView delegate reads the right shared default
        // for its inner L/R split during construction.
        _restoreSplitterState()

        // Restore tab strip BEFORE launchPath nav. The launchPath
        // logic switches to Files tab 0 and navigates its active
        // pane; if we restored after, the saved currentIndex would
        // immediately get clobbered. _restoreTabState() leaves the
        // _tabsRestored flag pending until its deferred currentIndex
        // assignment lands.
        _restoreTabState()

        // Honour an argv path/URI passed at launch. main.cpp sets
        // _launchPath from argv[1]; FileOps.resolve_ufb_uri parses
        // ufb:// URIs and applies path-mapping swaps when the source
        // OS differs from this one. Plain paths go through unchanged.
        if (typeof _launchPath !== "undefined" && _launchPath.length > 0) {
            launchNavTimer.start()
        }

        // Now that size + maximized state are restored, show the window.
        // On Windows, main.cpp shows it instead (creating the native
        // window dark + DWM-cloaked, then revealing it after the first
        // frame) so there's no white startup flash. Other platforms show
        // here so the first paint is at the persisted geometry.
        // --background (login-item / tray-only launch) leaves the window
        // hidden; the tray's "Open UFB" shows it on demand.
        if (Qt.platform.os !== "windows"
                && !(typeof _startInBackground !== "undefined" && _startInBackground))
            window.visible = true
        // Arm the reactivation hook only now — an activation event
        // during the hidden startup phase must not show the window
        // before the geometry restore above has landed.
        window._startupShowDone = true
    }

    // Slight delay so the SplitView + DualBrowser have laid out.
    // Without this, navigateActivePane() runs before the Directory
    // QObject has produced its initial entries_json.
    Timer {
        id: launchNavTimer
        interval: 200
        repeat: false
        onTriggered: {
            var resolved = FileOps.resolve_ufb_uri(_launchPath)
            if (resolved.length === 0) {
                console.warn("Main: could not resolve launch arg:", _launchPath)
                return
            }
            // Subscription-aware routing (project tab) with browser
            // fallback to the first Files tab at cold start.
            window._routeResolvedPath(resolved, 0)
        }
    }

    /// Open the in-app preview (lightbox) overlay for `path`. `pane` is the
    /// originating FileBrowser, remembered so the lightbox's Left/Right keys
    /// can step to the next/prev item in that pane's listing.
    property var _previewPane: null
    function openPreview(path, pane) {
        if (pane) window._previewPane = pane
        previewLightbox.open(path)
    }
    /// Step the originating pane's cursor to the prev/next previewable item
    /// (dir = -1 / +1) and re-open the lightbox on it.
    function previewStep(dir) {
        if (!window._previewPane) return
        var p = window._previewPane.previewStep(dir)
        if (p && p.length > 0)
            previewLightbox.open(p)
    }
    /// The lightbox grabbed keyboard focus when it opened; hand focus back to
    /// the originating pane on close so the still-selected item stays
    /// keyboard-actionable (e.g. Space immediately re-opens the preview).
    function previewClosed() {
        if (window._previewPane && window._previewPane.refocusView)
            window._previewPane.refocusView()
    }

    /// Open (or re-focus) a JobView tab for `jobPath`. If a tab for
    /// this path already exists, switch to it; otherwise append a new
    /// tab and switch to it.
    function openJobTab(jobPath, jobName) {
        for (var i = 0; i < openJobs.count; ++i) {
            if (openJobs.get(i).jobPath === jobPath) {
                tabBar.currentIndex = filesTabs.count + i
                return
            }
        }
        var newIdx = filesTabs.count + openJobs.count
        openJobs.append({ jobPath: jobPath, jobName: jobName })
        // Defer the currentIndex assignment until the Repeater has
        // actually created the new JobView delegate. Setting it
        // synchronously can leave StackLayout pointing at whatever
        // child currently sits at that slot (e.g. the aggregated-
        // Tracker Loader, which sits AFTER the job repeater) until
        // the Repeater catches up — visible as "tab opens but stays
        // on the previous tab".
        Qt.callLater(function() {
            tabBar.currentIndex = newIdx
            window._saveTabState()
        })
    }

    /// Close the JobView tab at `jobIdx` (0-based into openJobs,
    /// not tabBar.currentIndex). Re-targets currentIndex if needed.
    function closeJobTab(jobIdx) {
        if (jobIdx < 0 || jobIdx >= openJobs.count) return
        var currentTab = tabBar.currentIndex
        var closedTab = filesTabs.count + jobIdx
        openJobs.remove(jobIdx)
        if (currentTab === closedTab) {
            // Fall back to the previous tab (or last Files if this was the first job).
            tabBar.currentIndex = Math.max(0, currentTab - 1)
        } else if (currentTab > closedTab) {
            tabBar.currentIndex = currentTab - 1
        }
        window._saveTabState()
    }

    /// Open or focus the aggregated Tracker top-level tab.
    function openTrackerTab() {
        if (!trackerTabOpen) trackerTabOpen = true
        tabBar.currentIndex = filesTabs.count + openJobs.count
    }

    /// Close the aggregated Tracker top-level tab.
    function closeTrackerTab() {
        if (!trackerTabOpen) return
        var idx = filesTabs.count + openJobs.count
        if (tabBar.currentIndex === idx) {
            tabBar.currentIndex = Math.max(0, idx - 1)
        }
        trackerTabOpen = false
    }

    /// Open or focus the Transcode Queue top-level tab.
    function openTranscodeTab() {
        if (!transcodeTabOpen) transcodeTabOpen = true
        // Tab sits at openJobs.count + 1 (Files) + maybe Tracker.
        Qt.callLater(function() {
            tabBar.currentIndex = _transcodeTabIndex
        })
    }
    function closeTranscodeTab() {
        if (!transcodeTabOpen) return
        var idx = _transcodeTabIndex
        if (tabBar.currentIndex === idx) {
            tabBar.currentIndex = Math.max(0, idx - 1)
        }
        transcodeTabOpen = false
    }

    /// Tracker → "Open in Project" routing. Opens (or focuses) the
    /// JobView tab, then asks it to switch to the matching folder
    /// inner tab and select the item. Used by both the per-job
    /// Tracker (covered by JobView's own listener) and the
    /// aggregated Tracker (this path).
    function goToTrackedItem(jobPath, jobName, folderName, itemPath, revealPath) {
        if (!jobPath || jobPath.length === 0) return
        openJobTab(jobPath, jobName)
        // Find the JobView delegate for this job and route the call.
        var jobIdx = -1
        for (var i = 0; i < openJobs.count; ++i) {
            if (openJobs.get(i).jobPath === jobPath) { jobIdx = i; break }
        }
        if (jobIdx < 0) return
        Qt.callLater(function() {
            var jobView = jobsRepeater.itemAt(jobIdx)
            if (jobView && jobView.goToFolderItem) {
                jobView.goToFolderItem(folderName, itemPath, revealPath)
            }
        })
    }

    /// If `resolved` (a native path from a ufb:// link) lives under a
    /// subscribed job root, return the routing tuple
    /// { jobPath, jobName, folderName, itemPath } so the link can open in
    /// the project tab instead of the generic browser. folderName is the
    /// first path component under the job; itemPath is the item-level
    /// folder (job/folder/item) — empty when the link points at the job
    /// root or only a top-level folder (→ open the project tab, no item
    /// selection). Returns jobPath:"" when no subscription contains it.
    function _findSubscriptionForPath(resolved) {
        var none = { jobPath: "", jobName: "", folderName: "", itemPath: "", revealPath: "" }
        if (!resolved || resolved.length === 0) return none
        // Compare separator- and case-insensitively (Windows always, mac
        // default); slice the ORIGINAL path so item rows match natively.
        function norm(p) {
            return p.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase()
        }
        var r = norm(resolved)
        try {
            var subs = JSON.parse(Subscription.subscriptions_json)
            for (var i = 0; i < subs.length; ++i) {
                var jp = subs[i].jobPath || ""
                if (jp.length === 0) continue
                var nj = norm(jp)
                if (r !== nj && r.indexOf(nj + "/") !== 0) continue

                // Native separator + components from the original path.
                var sep = (jp.indexOf("\\") >= 0) ? "\\" : "/"
                var rel = resolved.substring(jp.length)
                while (rel.charAt(0) === "/" || rel.charAt(0) === "\\")
                    rel = rel.substring(1)
                var parts = rel.length > 0 ? rel.split(/[\/\\]/) : []
                var folderName = parts.length >= 1 ? parts[0] : ""
                var itemPath = (parts.length >= 2)
                    ? jp + sep + parts[0] + sep + parts[1]
                    : ""
                // revealPath = the link's full target when it points
                // deeper than the item (shot) — used to reveal + select
                // the exact file/subfolder on the right. Empty when the
                // link is the item itself.
                var revealPath = (itemPath.length > 0
                                  && resolved.length > itemPath.length)
                    ? resolved : ""
                return {
                    jobPath: jp,
                    jobName: subs[i].jobName || "",
                    folderName: folderName,
                    itemPath: itemPath,
                    revealPath: revealPath
                }
            }
        } catch (e) {
            console.warn("Main: _findSubscriptionForPath failed:", e)
        }
        return none
    }

    /// Route a resolved ufb:// path: into the owning project tab when it's
    /// under a subscription, else navigate the browser (fallbackTabIdx is
    /// the Files tab to use — last-active for deep links, 0 at cold start).
    function _routeResolvedPath(resolved, fallbackTabIdx) {
        var m = window._findSubscriptionForPath(resolved)
        if (m.jobPath.length > 0) {
            if (m.folderName.length > 0 && m.itemPath.length > 0)
                window.goToTrackedItem(m.jobPath, m.jobName, m.folderName,
                                       m.itemPath, m.revealPath)
            else
                window.openJobTab(m.jobPath, m.jobName)
            return
        }
        var t = window._navigateTargetFor(resolved)
        if (t.folder.length === 0) return
        var idx = fallbackTabIdx
        if (idx < 0 || idx >= filesTabs.count) idx = 0
        tabBar.currentIndex = idx
        var view = filesRepeater.itemAt(idx)
        if (view) view.navigateActivePane(t.folder, t.select)
    }

    SplitView {
        id: outerSplit
        anchors.fill: parent
        // No outer gutter — content runs flush to the window edge.
        // The macOS / Windows window frame is the only chrome around
        // the shell. Mirrors MinRender's app-shell layout.
        orientation: Qt.Horizontal

        // Flat splitter — 6px transparent hit zone with a centered
        // 1px hairline. Hairline brightens to accent on hover/drag
        // for clear affordance. Same handle is reused across the
        // dual-browser, folder-tab, and Mode-C SplitViews.
        // The `width <= height` check picks the perpendicular axis
        // for the line so the same delegate works for both
        // horizontal (vertical line) and vertical (horizontal line)
        // orientations.
        handle: Rectangle {
            id: outerSplitHandle
            implicitWidth: 6
            implicitHeight: 6
            color: "transparent"
            readonly property bool _active: SplitHandle.hovered || SplitHandle.pressed
            Rectangle {
                anchors.centerIn: parent
                width: outerSplitHandle.width <= outerSplitHandle.height ? 1 : outerSplitHandle.width
                height: outerSplitHandle.width <= outerSplitHandle.height ? outerSplitHandle.height : 1
                color: outerSplitHandle._active
                    ? Theme.colors.accent
                    : Theme.colors.divider
            }
        }

        // Persist the sidebar width on drag-end. SplitView.resizing
        // flips false when the user releases the handle - that's our
        // cue to capture the user's intent rather than every
        // intermediate pixel during the drag.
        onResizingChanged: {
            if (!resizing && window._splittersRestored) {
                Settings.set_outer_sidebar_width(sidebar.width)
            }
        }

        Sidebar {
            id: sidebar
            SplitView.preferredWidth:
                window._sidebarWidth > 0 ? window._sidebarWidth : 220
            SplitView.minimumWidth: 160

            onNavigateRequested: (path) => {
                // Sidebar bookmark/mount click → route to whichever
                // Files tab the user last had focused. Switching to
                // a hardcoded index 0 would steal navigation away
                // from a tab they were actively using.
                var idx = window._lastActiveFilesTabIndex
                if (idx < 0 || idx >= filesTabs.count) idx = 0
                tabBar.currentIndex = idx
                var view = filesRepeater.itemAt(idx)
                if (view) view.navigateActivePane(path)
            }
            onOpenInNewTabRequested: (path) => window.addFilesTab(path)
            onOpenJobRequested: (jobPath, jobName) => window.openJobTab(jobPath, jobName)
            onOpenTrackerRequested: () => window.openTrackerTab()
            onOpenTranscodeRequested: () => window.openTranscodeTab()
        }

        ColumnLayout {
            SplitView.fillWidth: true
            spacing: 0

            // ── Top tab bar ──────────────────────────────────────────
            // Custom-styled to fit the dark theme — FluentWinUI3 / macOS
            // default TabBar styling renders nearly invisible against
            // our background. We compose the tab strip ourselves: a
            // ListView of clickable cells with an underline indicator
            // for the active tab.
            //
            // tabsModel layout: index 0 = "Files" (non-closeable), then
            // one entry per openJobs row. We mirror openJobs into here
            // so the Repeater pattern stays simple.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: Theme.colors.bg

                // No bottom divider — the active tab's 2px accent
                // underline separates tabs from the content below.
                // A full-width line here would stack chrome above
                // every browser/job toolbar that follows.

                // Horizontal-scroll wrapper so the tab strip degrades
                // gracefully when the user opens enough tabs to overflow
                // the available width. Flickable + WheelHandler keeps
                // ALL existing tab delegates intact (no per-tab width
                // changes); plain mouse-wheel scrolls left/right and
                // touch / trackpad two-finger flick works for free.
                Flickable {
                    id: tabFlick
                    anchors.fill: parent
                    contentWidth: tabRow.width
                    contentHeight: height
                    clip: true
                    flickableDirection: Flickable.HorizontalFlick
                    boundsBehavior: Flickable.StopAtBounds

                    // Translate vertical wheel ticks to horizontal scroll
                    // (the tab strip is one-dimensional). Horizontal
                    // ticks from a trackpad pass through directly.
                    WheelHandler {
                        onWheel: (wheel) => {
                            var delta = wheel.angleDelta.x !== 0
                                ? wheel.angleDelta.x
                                : wheel.angleDelta.y
                            var maxScroll = Math.max(0,
                                tabFlick.contentWidth - tabFlick.width)
                            tabFlick.contentX = Math.max(0,
                                Math.min(maxScroll,
                                         tabFlick.contentX - delta))
                            wheel.accepted = true
                        }
                    }

                Row {
                    id: tabRow
                    height: tabFlick.height
                    spacing: 0

                    // Files tabs — one pill per filesTabs entry. The
                    // last surviving Files tab is non-closeable so the
                    // app always has somewhere to land sidebar nav.
                    Repeater {
                        id: filesTabsRepeater
                        model: filesTabs
                        delegate: Rectangle {
                            id: filesTab
                            readonly property int filesIdx: index
                            readonly property int tabIdx: index
                            height: tabRow.height
                            width: filesTabContent.implicitWidth
                                + (filesTabs.count > 1 ? 36 : 24)
                            color: tabBar.currentIndex === tabIdx
                                ? Theme.colors.surfaceHover
                                : (filesMa.containsMouse ? Theme.colors.surface : "transparent")

                            Row {
                                id: filesTabContent
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                Icon {
                                    name: "folder-simple"
                                    size: Theme.icon.sizeToolbar
                                    color: tabBar.currentIndex === filesTab.tabIdx
                                        ? Theme.colors.textBright
                                        : Theme.colors.textMuted
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Label {
                                    text: qsTr("Files")
                                    color: tabBar.currentIndex === filesTab.tabIdx
                                        ? Theme.colors.textBright
                                        : Theme.colors.textMuted
                                    font.pixelSize: Theme.font.sizeBody
                                    font.bold: tabBar.currentIndex === filesTab.tabIdx
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                            // × close button. Only when more than one
                            // Files tab is open. Z-stacked above the
                            // tab-select MouseArea so the X wins
                            // hit-testing without propagation games.
                            Rectangle {
                                id: filesCloseBg
                                visible: filesTabs.count > 1
                                z: 10
                                anchors.right: parent.right
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16
                                height: 16
                                radius: 3
                                color: filesCloseMa.containsMouse
                                    ? Theme.colors.surfaceHover : "transparent"
                                Label {
                                    anchors.centerIn: parent
                                    text: "×"
                                    color: Theme.colors.text
                                    font.pixelSize: 13
                                }
                                MouseArea {
                                    id: filesCloseMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: (mouse) => {
                                        mouse.accepted = true
                                        window.closeFilesTab(filesTab.filesIdx)
                                    }
                                }
                            }
                            // Active-tab underline.
                            Rectangle {
                                visible: tabBar.currentIndex === filesTab.tabIdx
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 2
                                color: Theme.colors.accentSelected
                            }
                            MouseArea {
                                id: filesMa
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                propagateComposedEvents: true
                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.RightButton) {
                                        tabContextMenu.targetTabIdx = filesTab.tabIdx
                                        tabContextMenu.popup()
                                        return
                                    }
                                    if (filesCloseBg.visible && filesCloseMa.containsMouse) {
                                        mouse.accepted = false
                                        return
                                    }
                                    tabBar.currentIndex = filesTab.tabIdx
                                }
                            }
                        }
                    }

                    // Job tabs — closeable.
                    Repeater {
                        id: jobTabsRepeater
                        model: openJobs
                        delegate: Rectangle {
                            id: jobTab
                            readonly property int jobIdx: index
                            readonly property int tabIdx: filesTabs.count + index
                            height: tabRow.height
                            width: Math.min(180, jobLabel.implicitWidth + 44)
                            color: tabBar.currentIndex === tabIdx
                                ? Theme.colors.surfaceHover
                                : (jobMa.containsMouse ? Theme.colors.surface : "transparent")

                            Row {
                                id: jobLabel
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: closeBg.left
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                Icon {
                                    name: "clipboard-text"
                                    size: Theme.icon.sizeToolbar
                                    color: tabBar.currentIndex === jobTab.tabIdx
                                        ? Theme.colors.textBright
                                        : Theme.colors.textMuted
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Label {
                                    text: model.jobName
                                    width: jobLabel.width - 22
                                    color: tabBar.currentIndex === jobTab.tabIdx
                                        ? Theme.colors.textBright
                                        : Theme.colors.textMuted
                                    font.pixelSize: Theme.font.sizeBody
                                    font.bold: tabBar.currentIndex === jobTab.tabIdx
                                    elide: Text.ElideMiddle
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                            // × close button. z above the tab-select
                            // MouseArea so closeMa actually wins
                            // hit-testing on the X (without this,
                            // jobMa's anchors.fill captures the click
                            // first and the propagation back through
                            // closeMa.containsMouse never resolves).
                            Rectangle {
                                id: closeBg
                                z: 10
                                anchors.right: parent.right
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16
                                height: 16
                                radius: 3
                                color: closeMa.containsMouse ? Theme.colors.surfaceHover : "transparent"
                                Label {
                                    anchors.centerIn: parent
                                    text: "×"
                                    color: Theme.colors.text
                                    font.pixelSize: 13
                                }
                                MouseArea {
                                    id: closeMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: (mouse) => {
                                        mouse.accepted = true
                                        window.closeJobTab(jobTab.jobIdx)
                                    }
                                }
                            }
                            // Active-tab underline.
                            Rectangle {
                                visible: tabBar.currentIndex === jobTab.tabIdx
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 2
                                color: Theme.colors.accentSelected
                            }
                            MouseArea {
                                id: jobMa
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                propagateComposedEvents: true
                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.RightButton) {
                                        tabContextMenu.targetTabIdx = jobTab.tabIdx
                                        tabContextMenu.popup()
                                        return
                                    }
                                    // Don't steal clicks the close MouseArea
                                    // already handled.
                                    if (closeMa.containsMouse) {
                                        mouse.accepted = false
                                        return
                                    }
                                    tabBar.currentIndex = jobTab.tabIdx
                                }
                            }
                        }
                    }

                    // Aggregated Tracker tab (singleton — opens via
                    // Sidebar's "All Tracked" entry; closes via the ×).
                    Loader {
                        active: window.trackerTabOpen
                        sourceComponent: trackerTabComponent
                    }

                    // Transcode Queue tab (singleton — opens via
                    // Sidebar's "Transcode Queue" entry).
                    Loader {
                        active: window.transcodeTabOpen
                        sourceComponent: transcodeTabComponent
                    }

                    // "+" — spawn a new Files tab. Sits at the far
                    // right of every other tab pill so it never moves
                    // around as job/tracker/transcode tabs come and go.
                    Rectangle {
                        height: tabRow.height
                        width: 28
                        color: addFilesMa.containsMouse
                            ? Theme.colors.surface : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            name: "plus"
                            size: Theme.icon.sizeToolbar
                            color: Theme.colors.textMuted
                        }
                        MouseArea {
                            id: addFilesMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: window.addFilesTab()
                            ToolTip.text: qsTr("New Files tab")
                            ToolTip.visible: containsMouse
                            ToolTip.delay: 500
                        }
                    }

                    // Close All — broom icon. Sits to the right of
                    // "+" so it's the very last cell. Disabled
                    // (greyed) when there's nothing to close so an
                    // idle click is a visible no-op.
                    Rectangle {
                        height: tabRow.height
                        width: 28
                        opacity: window._hasClosableTabs() ? 1 : 0.35
                        color: closeAllMa.containsMouse && window._hasClosableTabs()
                            ? Theme.colors.surface : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            name: "x-circle"
                            size: Theme.icon.sizeToolbar
                            color: Theme.colors.textMuted
                        }
                        MouseArea {
                            id: closeAllMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: window._hasClosableTabs()
                                ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: {
                                if (!window._hasClosableTabs()) return
                                closeAllConfirmDialog.open()
                            }
                            ToolTip.text: qsTr("Close all tabs")
                            ToolTip.visible: containsMouse
                            ToolTip.delay: 500
                        }
                    }
                }
                }  // Flickable
            }

            Component {
                id: transcodeTabComponent
                Rectangle {
                    readonly property int tabIdx: window._transcodeTabIndex
                    height: tabRow.height
                    width: transcodeTabLabel.implicitWidth + 44
                    color: tabBar.currentIndex === tabIdx
                        ? Theme.colors.surfaceHover
                        : (transcodeTabMa.containsMouse ? Theme.colors.surface : "transparent")
                    Row {
                        id: transcodeTabLabel
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: transcodeCloseBg.left
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Icon {
                            name: "film-strip"
                            size: Theme.icon.sizeToolbar
                            color: tabBar.currentIndex === transcodeTabLabel.parent.tabIdx
                                ? Theme.colors.textBright
                                : Theme.colors.textMuted
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            text: qsTr("Transcode")
                            color: tabBar.currentIndex === transcodeTabLabel.parent.tabIdx
                                ? Theme.colors.textBright
                                : Theme.colors.textMuted
                            font.pixelSize: Theme.font.sizeBody
                            font.bold: tabBar.currentIndex === transcodeTabLabel.parent.tabIdx
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    Rectangle {
                        id: transcodeCloseBg
                        z: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16; height: 16; radius: 3
                        color: transcodeCloseMa.containsMouse ? Theme.colors.surfaceHover : "transparent"
                        Label {
                            anchors.centerIn: parent
                            text: "×"; color: Theme.colors.text; font.pixelSize: 13
                        }
                        MouseArea {
                            id: transcodeCloseMa
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: (mouse) => {
                                mouse.accepted = true
                                window.closeTranscodeTab()
                            }
                        }
                    }
                    Rectangle {
                        visible: tabBar.currentIndex === parent.tabIdx
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 2
                        color: Theme.colors.accentSelected
                    }
                    MouseArea {
                        id: transcodeTabMa
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton) {
                                tabContextMenu.targetTabIdx = parent.tabIdx
                                tabContextMenu.popup()
                                return
                            }
                            if (transcodeCloseMa.containsMouse) {
                                mouse.accepted = false
                                return
                            }
                            tabBar.currentIndex = parent.tabIdx
                        }
                    }
                }
            }

            Component {
                id: trackerTabComponent
                Rectangle {
                    readonly property int tabIdx: window._trackerTabIndex
                    height: tabRow.height
                    width: trackerTabLabel.implicitWidth + 44
                    color: tabBar.currentIndex === tabIdx
                        ? Theme.colors.surfaceHover
                        : (trackerTabMa.containsMouse ? Theme.colors.surface : "transparent")

                    Row {
                        id: trackerTabLabel
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: trackerCloseBg.left
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Icon {
                            name: "clipboard-text"
                            size: Theme.icon.sizeToolbar
                            color: tabBar.currentIndex === trackerTabLabel.parent.tabIdx
                                ? Theme.colors.textBright
                                : Theme.colors.textMuted
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            text: qsTr("All Tracked")
                            color: tabBar.currentIndex === trackerTabLabel.parent.tabIdx
                                ? Theme.colors.textBright
                                : Theme.colors.textMuted
                            font.pixelSize: Theme.font.sizeBody
                            font.bold: tabBar.currentIndex === trackerTabLabel.parent.tabIdx
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    Rectangle {
                        id: trackerCloseBg
                        z: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        radius: 3
                        color: trackerCloseMa.containsMouse ? Theme.colors.surfaceHover : "transparent"
                        Label {
                            anchors.centerIn: parent
                            text: "×"
                            color: Theme.colors.text
                            font.pixelSize: 13
                        }
                        MouseArea {
                            id: trackerCloseMa
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: (mouse) => {
                                mouse.accepted = true
                                window.closeTrackerTab()
                            }
                        }
                    }
                    Rectangle {
                        visible: tabBar.currentIndex === parent.tabIdx
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 2
                        color: Theme.colors.accentSelected
                    }
                    MouseArea {
                        id: trackerTabMa
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton) {
                                tabContextMenu.targetTabIdx = parent.tabIdx
                                tabContextMenu.popup()
                                return
                            }
                            if (trackerCloseMa.containsMouse) {
                                mouse.accepted = false
                                return
                            }
                            tabBar.currentIndex = parent.tabIdx
                        }
                    }
                }
            }

            // Internal "tab bar" state. We don't actually use a Qt
            // TabBar (the styled version above is custom), but keeping
            // a tiny Item with a currentIndex property gives the rest
            // of the file (StackLayout binding, footer, openJobTab /
            // closeJobTab) a stable handle.
            Item {
                id: tabBar
                visible: false
                property int currentIndex: 0
            }

            // ── Tab content ──────────────────────────────────────────
            StackLayout {
                id: tabStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: tabBar.currentIndex

                // Indices 0..F-1: one DualBrowserView per Files tab.
                // Each instance owns its own activePane / Directory
                // state so multi-tab focus stays cleanly partitioned.
                Repeater {
                    id: filesRepeater
                    model: filesTabs
                    delegate: DualBrowserView {
                        id: dbDelegate
                        // Read the model's per-tab default. Set at
                        // append time by addFilesTab().
                        startWithRightCollapsed:
                            model.startWithRightCollapsed === true
                        // Shared inner-split default (Option A).
                        // 0 means "use 50/50".
                        initialLeftPaneWidth: window._filesLeftWidth
                        onOpenTranscodeQueueRequested: window.openTranscodeTab()
                        onOpenInNewTabRequested: (path) => window.addFilesTab(path)
                        // Persist on pane navigation. Debounced via the
                        // shared timer so a chain of clicks doesn't
                        // hammer disk.
                        Connections {
                            target: dbDelegate.leftDir
                            function onCurrent_pathChanged() {
                                if (window._tabsRestored) _tabSaveTimer.restart()
                            }
                        }
                        Connections {
                            target: dbDelegate.rightDir
                            function onCurrent_pathChanged() {
                                if (window._tabsRestored) _tabSaveTimer.restart()
                            }
                        }
                        onLeftPaneCollapsedChanged:
                            if (window._tabsRestored) window._saveTabState()
                        onRightPaneCollapsedChanged:
                            if (window._tabsRestored) window._saveTabState()
                    }
                }

                // Indices F..F+J-1: one JobView per open subscription.
                // JobView already declares required properties for
                // jobPath / jobName; Qt auto-binds them from the
                // matching ListModel role names. (Trying to reference
                // `model.jobPath` from inside an inline delegate that's
                // a separate component throws "model is not defined".)
                Repeater {
                    id: jobsRepeater
                    model: openJobs
                    delegate: JobView {
                        onOpenInNewTabRequested: (path) => window.addFilesTab(path)
                        onOpenTranscodeQueueRequested: window.openTranscodeTab()
                    }
                }

                // Index N+1: aggregated Tracker tab content. Loader
                // keeps the body absent until the tab is opened.
                // Aggregated Tracker — embedded directly (not in a
                // Loader) because Loader-inside-StackLayout doesn't
                // size its loaded Item correctly: the load fires at
                // 0×0 and the inner ListView never creates delegates.
                // Always-instantiated is fine here; the QObject is
                // cheap and only does work when the tab is visible.
                TrackerView {
                    jobPath: ""
                    jobName: ""
                    onGoToItemRequested: (jp, jn, fn, ip) => window.goToTrackedItem(jp, jn, fn, ip)
                }

                // Transcode Queue — same reasoning. Direct embed so
                // StackLayout sizes it correctly when its tab is
                // selected.
                TranscodeQueue {
                    // "Open in Browser" — switch to the most-recent
                    // Files tab and navigate its active pane to the
                    // output file's parent folder, with the file
                    // pre-selected. Mirrors the deep-link / URI flow.
                    onOpenInBrowserRequested: (path) => {
                        var t = window._navigateTargetFor(path)
                        if (t.folder.length === 0) return
                        var idx = window._lastActiveFilesTabIndex
                        if (idx < 0 || idx >= filesTabs.count) idx = 0
                        tabBar.currentIndex = idx
                        var view = filesRepeater.itemAt(idx)
                        if (view) view.navigateActivePane(t.folder, t.select)
                    }
                }
            }
        }
    }

    // ── Status strip ─────────────────────────────────────────────────
    footer: Rectangle {
        height: 22
        color: Theme.colors.bg

        // Top divider only — separates the footer from the tab content
        // above. No bottom border (the window edge is the only chrome
        // below us).
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Theme.dim.divider
            color: Theme.colors.divider
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10

            Label {
                text: {
                    if (tabBar.currentIndex >= filesTabs.count) {
                        var jobIdx = tabBar.currentIndex - filesTabs.count
                        if (jobIdx >= 0 && jobIdx < openJobs.count) {
                            return qsTr("job: %1").arg(openJobs.get(jobIdx).jobName)
                        }
                        return ""
                    }
                    var v = window._currentFilesView
                    return v ? qsTr("active: %1").arg(v.activeSide) : ""
                }
                color: Theme.colors.textSubtle
                font.pixelSize: Theme.font.sizeSmall
            }
            Label {
                text: {
                    if (tabBar.currentIndex >= filesTabs.count) return ""
                    var v = window._currentFilesView
                    if (!v) return ""
                    var pane = v.activePane
                    var n = (pane && pane.selectedIndexes) ? pane.selectedIndexes.length : 0
                    return n > 0 ? qsTr("· %1 selected").arg(n) : ""
                }
                color: Theme.colors.textMuted
                font.pixelSize: Theme.font.sizeSmall
            }
            Item { Layout.fillWidth: true }

            // ── File-op progress strip ───────────────────────────────
            // Compact per-op chips driven by FileOps.active_ops_json.
            // Each chip shows operation icon + count + a 60px progress
            // bar + percentage. When idle the row collapses.
            Repeater {
                model: window._activeOps
                delegate: Rectangle {
                    required property var modelData
                    Layout.preferredHeight: 16
                    Layout.preferredWidth: chipRow.implicitWidth + 12
                    Layout.alignment: Qt.AlignVCenter
                    radius: 8
                    // Flat dimmed-accent pill — no border. Same "info
                    // chip" idiom but without the chrome.
                    color: Theme.colors.accentMuted
                    RowLayout {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 4
                        Label {
                            text: modelData.operation === "copy"   ? "📋"
                                : modelData.operation === "move"   ? "✂️"
                                : modelData.operation === "delete" ? "🗑️"
                                : "⏳"
                            font.pixelSize: Theme.font.sizeTiny
                        }
                        Label {
                            text: qsTr("%1 / %2")
                                .arg(modelData.itemsDone || 0)
                                .arg(modelData.itemsTotal || 0)
                            color: Theme.colors.text
                            font.pixelSize: Theme.font.sizeTiny
                            font.family: Theme.font.mono
                        }
                        Rectangle {
                            Layout.preferredWidth: 60
                            Layout.preferredHeight: 4
                            radius: 2
                            color: Theme.colors.toolbarAlt
                            Rectangle {
                                height: parent.height
                                width: parent.width * Math.max(0,
                                    Math.min(1, modelData.progress || 0))
                                radius: 2
                                color: Theme.colors.accent
                            }
                        }
                        Label {
                            text: Math.round((modelData.progress || 0) * 100) + "%"
                            color: Theme.colors.textMuted
                            font.pixelSize: Theme.font.sizeTiny
                            font.family: Theme.font.mono
                            Layout.preferredWidth: 28
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }
            Label {
                visible: window._activeOps.length === 0 && window._lastOpStatus.length > 0
                text: window._lastOpStatus
                color: Theme.colors.textSubtle
                font.pixelSize: Theme.font.sizeSmall
            }

            Rectangle {
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                Layout.alignment: Qt.AlignVCenter
                radius: 4
                // agent_required false = GUI owns all mounts (F1) and
                // no sync host is needed — idle gray, not a warning.
                color: !Mount.agent_required ? "#555"
                     : Mount.connected ? "#4cb050" : "#888"
            }
            Label {
                text: !Mount.agent_required ? "agent idle"
                    : Mount.connected ? "agent connected" : "agent waiting"
                color: "#888"
                font.pixelSize: 11
            }
            // Quick relaunch when the agent has died or never started.
            Button {
                visible: Mount.agent_required && !Mount.connected
                text: qsTr("Launch")
                padding: 2
                Layout.preferredHeight: 18
                ToolTip.text: qsTr("Spawn the ufb-agent subprocess")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: Mount.launch_agent()
            }
            Label { text: " · "; color: "#444" }
            Label {
                text: qsTr("ufb-core %1").arg(Paths.core_version())
                color: "#666"
                font.pixelSize: 11
            }
            // Auto-update check (Sparkle/WinSparkle). Hidden when no
            // update backend is compiled in (Updater.available == false).
            // Styled as a muted status-bar text link (matches the
            // "ufb-core <ver>" label beside it), accenting on hover.
            // Null-guarded: the Updater context property reads as null
            // while the engine tears down, and these bindings re-evaluate.
            Label { visible: Updater ? Updater.available : false; text: " · "; color: "#444" }
            Label {
                visible: Updater ? Updater.available : false
                text: qsTr("Check for Updates")
                font.pixelSize: 11
                color: updateCheckHover.containsMouse
                    ? Theme.colors.accent : Theme.colors.textSubtle
                ToolTip.text: qsTr("Check the update server for a newer UFB")
                ToolTip.visible: updateCheckHover.containsMouse
                ToolTip.delay: 500
                MouseArea {
                    id: updateCheckHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Updater.checkForUpdates()
                }
            }
        }
    }

    // Close-all confirm. Surfaces the count up-front so the user
    // knows what they're throwing away.
    Dialog {
        id: closeAllConfirmDialog
        title: qsTr("Close all tabs?")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width - width) / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: window.closeAllTabs()

        readonly property int _extraFilesCount: Math.max(0, filesTabs.count - 1)
        readonly property int _totalClosing:
            _extraFilesCount + openJobs.count
            + (window.trackerTabOpen ? 1 : 0)
            + (window.transcodeTabOpen ? 1 : 0)

        contentItem: Label {
            text: qsTr("This will close %1 tab(s) and return to the first Files tab. The first tab keeps its current paths.")
                .arg(closeAllConfirmDialog._totalClosing)
            color: Theme.colors.text
            font.pixelSize: Theme.font.sizeBody
            wrapMode: Text.Wrap
        }
    }

    // Right-click context menu for tab pills. `targetTabIdx` is set by the
    // pill that opened it; "Close Other Tabs" keeps the first Files tab and
    // that pill, closing the rest.
    Menu {
        id: tabContextMenu
        property int targetTabIdx: -1
        MenuItem {
            text: qsTr("Close Other Tabs")
            enabled: window._hasOtherTabs(tabContextMenu.targetTabIdx)
            onTriggered: window.closeOtherTabs(tabContextMenu.targetTabIdx)
        }
    }

    // In-app QuickLook-style preview overlay. Parented to the window overlay
    // so it covers the whole app; opened via window.openPreview(path).
    PreviewLightbox {
        id: previewLightbox
        parent: Overlay.overlay
        anchors.fill: parent
    }
}
