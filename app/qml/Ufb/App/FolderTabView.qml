// FolderTabView — two layout modes per legacy ufb-tauri.
//
// Mode B (flat folders):  ItemListPanel | FileBrowser
// Mode C (shot folders):  ItemListPanel | (project FileBrowser /
//                                          renders FileBrowser)
//
// Auto-detection: when tabPath changes, FileOps.detect_folder_layout_mode
// probes the folder and decides which layout to render. Mode C looks
// for a PROJECT-named subfolder (project, scenes, nuke, ae, …) and a
// RENDER-named subfolder (renders, comp, output, …) inside each item;
// when either is missing, the corresponding browser falls back to
// the item folder itself.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Item {
    id: root

    /// Folder the item list reads from.
    required property string tabPath
    /// Job that owns this folder. Forwarded to ItemListPanel for the
    /// "Mark as Tracked" action so it can call set_item_tracked with
    /// the right job_path. Defaults to "" — host wires this through.
    property string jobPath: ""
    /// Detected layout — "B" or "C". Auto-set when tabPath changes;
    /// the host can also set this manually to force a layout.
    property string mode: "B"

    /// Bubbled up from FileBrowser's "Open in New Tab" menu item.
    /// JobView re-emits to Main, which spawns a new Files tab.
    signal openInNewTabRequested(string path)

    /// Bubbled up from FileBrowser's "Transcode to MP4" menu item.
    /// JobView re-emits to Main, which surfaces the Transcode Queue
    /// tab so the user sees the job they just queued.
    signal openTranscodeQueueRequested()

    /// Right-pane Directories. In Mode B only `rightDir` is used;
    /// in Mode C `topDir` is the project subdir, `bottomDir` the
    /// renders subdir (each falls back to the item folder if its
    /// matching subdir doesn't exist).
    Directory { id: rightDir }
    Directory { id: topDir }
    Directory { id: bottomDir }

    /// Tracks which of the right-side FileBrowsers currently owns the
    /// keyboard focus / clipboard / shortcut routing. In Mode B only
    /// rightBrowser is used. In Mode C the user clicks topBrowser or
    /// bottomBrowser to switch — same handshake DualBrowserView uses.
    /// Defaults to the top browser in Mode C; rightBrowser in Mode B.
    property var activeBrowser: rightBrowser
    onModeChanged: {
        // Reset active-browser focus when mode flips so we don't keep
        // a now-hidden browser as the active target.
        activeBrowser = (mode === "C") ? topBrowser : rightBrowser
    }

    /// Session memory per subtab, keyed by tabPath: the ItemListPanel
    /// selection and each right-pane Directory position. The job's
    /// subtabs share this ONE FolderTabView (deliberate — keeps a job
    /// tab light), so without this, every subtab switch wiped the
    /// other subtabs' working state. In-memory only; per-folder VIEW
    /// prefs (mode/sort/grid) already persist via the browser panes.
    property var _tabMemory: ({})
    /// The tabPath the live pane state belongs to (stash target when
    /// tabPath flips to a new value).
    property string _memPath: ""

    onTabPathChanged: {
        // Stash the outgoing subtab's state before anything resets.
        if (_memPath.length > 0) {
            _tabMemory[_memPath] = {
                sel: itemList.selectedItemPath,
                right: rightDir.current_path,
                top: topDir.current_path,
                bottom: bottomDir.current_path
            }
        }
        // Re-probe layout mode and clear any prior selection state.
        if (tabPath.length > 0) {
            mode = FileOps.detect_folder_layout_mode(tabPath)
        } else {
            mode = "B"
        }
        itemList.selectedItemPath = ""
        // clear() resets state without triggering a list_directory("")
        // (which would produce a "Not a directory" error banner).
        rightDir.clear()
        topDir.clear()
        bottomDir.clear()
        // Returning to a subtab we've been in this session: restore
        // its selection + pane positions. Directories navigate straight
        // to the remembered (possibly deep) paths; a Tracker-driven
        // selectItem that follows overrides this, as it should.
        var m = _tabMemory[tabPath]
        if (m) {
            if (m.sel && m.sel.length > 0) itemList.selectedItemPath = m.sel
            if (root.mode === "C") {
                if (m.top && m.top.length > 0) topDir.navigate_to(m.top)
                if (m.bottom && m.bottom.length > 0) bottomDir.navigate_to(m.bottom)
            } else if (m.right && m.right.length > 0) {
                rightDir.navigate_to(m.right)
            }
        }
        _memPath = tabPath
    }

    function _onItemSelected(itemPath) {
        if (root.mode === "C") {
            var proj = FileOps.find_project_subdir(itemPath)
            var rend = FileOps.find_render_subdir(itemPath)
            topDir.navigate_to(proj && proj.length > 0 ? proj : itemPath)
            bottomDir.navigate_to(rend && rend.length > 0 ? rend : itemPath)
        } else {
            rightDir.navigate_to(itemPath)
        }
    }

    /// Programmatic selection — used by JobView when the Tracker
    /// asks for a "go to item" navigation. Highlights the row in
    /// ItemListPanel and runs the right-pane navigation as if the
    /// user had clicked it.
    function selectItem(itemPath, revealPath) {
        itemList.selectedItemPath = itemPath
        _onItemSelected(itemPath)
        // Independent, best-effort: if the link pointed deeper than the
        // item (a specific file/subfolder), reveal + select it on the
        // right. Left selection above and this are not codependent — each
        // does what it can; if the target isn't there, nothing happens.
        if (revealPath && revealPath.length > 0 && revealPath !== itemPath)
            _revealOnRight(itemPath, revealPath)
    }

    /// Navigate the appropriate right pane to the parent of `revealPath`
    /// and queue selection of `revealPath` once that listing loads. For
    /// shots (Mode C) the pane is chosen by whether the target sits under
    /// the item's project vs renders subdir; Mode B uses the single pane.
    /// Falls back to the top pane when the target isn't under a detected
    /// subdir — still reveals it rather than guessing.
    function _revealOnRight(itemPath, revealPath) {
        var parent = Paths.parent_of(revealPath)
        if (!parent || parent.length === 0) return
        function norm(p) {
            return p.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase()
        }
        var rp = norm(revealPath)
        if (root.mode === "C") {
            var proj = FileOps.find_project_subdir(itemPath)
            var rend = FileOps.find_render_subdir(itemPath)
            if (rend && rend.length > 0 && rp.indexOf(norm(rend) + "/") === 0) {
                bottomBrowser.selectAfterLoadPath = revealPath
                bottomDir.navigate_to(parent)
            } else {
                // Under the project subdir, or anywhere else under the
                // item — reveal on the top (project) pane.
                topBrowser.selectAfterLoadPath = revealPath
                topDir.navigate_to(parent)
            }
        } else {
            rightBrowser.selectAfterLoadPath = revealPath
            rightDir.navigate_to(parent)
        }
    }

    /// Refresh everything visible in this tab body: item list (with
    /// tracked-set + metadata cache) and the right-side Directory
    /// listings for the current mode. Used by JobView's Refresh
    /// button so a single click rehydrates content + metadata across
    /// every panel without the user having to switch folders.
    function refresh() {
        itemList.refreshAll()
        if (mode === "C") {
            if (topDir.current_path.length > 0) topDir.refresh()
            if (bottomDir.current_path.length > 0) bottomDir.refresh()
        } else {
            if (rightDir.current_path.length > 0) rightDir.refresh()
        }
    }

    SplitView {
        id: outerSplit
        anchors.fill: parent
        orientation: Qt.Horizontal

        // Flat splitter — see Main.qml outerSplit's handle.
        handle: Rectangle {
            id: folderTabSplitHandle
            implicitWidth: 6
            implicitHeight: 6
            color: "transparent"
            readonly property bool _active: SplitHandle.hovered || SplitHandle.pressed
            Rectangle {
                anchors.centerIn: parent
                width: folderTabSplitHandle.width <= folderTabSplitHandle.height ? 1 : folderTabSplitHandle.width
                height: folderTabSplitHandle.width <= folderTabSplitHandle.height ? folderTabSplitHandle.height : 1
                color: folderTabSplitHandle._active
                    ? Theme.colors.accent
                    : Theme.colors.divider
            }
        }

        ItemListPanel {
            id: itemList
            folderPath: root.tabPath
            jobPath: root.jobPath
            // Default 50/50 split — same idiom DualBrowserView uses.
            // Drag the splitter and the binding gives way to imperative
            // values; switching folder tabs resets to 50/50.
            SplitView.preferredWidth: outerSplit.width / 2
            SplitView.minimumWidth: 180

            onItemSelected: (itemPath) => root._onItemSelected(itemPath)
            onItemActivated: (itemPath) => root._onItemSelected(itemPath)
        }

        // Right side: a Loader-like swap between Mode B and Mode C
        // bodies. Both are always present in the scene graph but only
        // one is visible — keeps state on the inactive side cheap and
        // avoids destroying / re-creating FileBrowsers as mode flips.
        Item {
            id: rightHost
            SplitView.fillWidth: true
            SplitView.minimumWidth: 320

            // ── Mode B ─────────────────────────────────────────────
            FileBrowser {
                id: rightBrowser
                anchors.fill: parent
                visible: root.mode !== "C"
                dir: rightDir
                // Column widths persist per FOLDER via the shared
                // browser_folder_prefs memory (see FileBrowser) — the
                // per-pane settingsPaneKey system is retired.
                // "Add Note…" (date-prefixed .mndb file) only in the
                // docs/notes tab. Mode C never applies — both are
                // flat folders, so they always render as Mode B.
                allowAddNote: {
                    var p = root.tabPath
                    var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
                    var name = idx >= 0 ? p.substring(idx + 1) : p
                    return name.toLowerCase() === "notes"
                        || name.toLowerCase() === "docs"
                }
                // Path field is hidden in shot/folder subtabs — the
                // selected ItemListPanel row already conveys location
                // and the toolbar gets crowded otherwise.
                showPathField: false
                // No collapse in project/shot tabs — the pane is
                // partnered with an ItemListPanel, not another
                // FileBrowser, so collapsing it would just leave
                // a dead strip.
                allowCollapse: false
                // No "Open in Other Browser" — the partner pane is
                // an ItemListPanel (Mode B) or a mode-specific
                // FileBrowser (Mode C), not a peer to route folders
                // into the way DualBrowserView does.
                allowOpenInOtherBrowser: false
                // …but it CAN push items into the main dual browser.
                allowOpenInMainBrowser: true
                // Mode B has only one browser; it's always the active
                // one (and there's nothing to ambiguate with).
                active: root.activeBrowser === rightBrowser
                onActivated: root.activeBrowser = rightBrowser
                onOpenInNewTabRequested: (path) => root.openInNewTabRequested(path)
                onOpenTranscodeQueueRequested: root.openTranscodeQueueRequested()
            }

            // ── Mode C ─────────────────────────────────────────────
            SplitView {
                id: cSplit
                anchors.fill: parent
                visible: root.mode === "C"
                orientation: Qt.Vertical

                // Flat splitter — same handle pattern as the horizontal
                // SplitViews above; the `width <= height` check inside
                // adapts the line to a horizontal stroke automatically.
                handle: Rectangle {
                    id: cSplitHandle
                    implicitWidth: 6
                    implicitHeight: 6
                    color: "transparent"
                    readonly property bool _active: SplitHandle.hovered || SplitHandle.pressed
                    Rectangle {
                        anchors.centerIn: parent
                        width: cSplitHandle.width <= cSplitHandle.height ? 1 : cSplitHandle.width
                        height: cSplitHandle.width <= cSplitHandle.height ? cSplitHandle.height : 1
                        color: cSplitHandle._active
                            ? Theme.colors.accent
                            : Theme.colors.divider
                    }
                }

                FileBrowser {
                    id: topBrowser
                    // Pin the top to half the vertical SplitView height
                    // and let the bottom fill — same fix as the
                    // DualBrowserView 50/50 split. Drag the handle and
                    // the binding gives way to imperative.
                    SplitView.preferredHeight: cSplit.height / 2
                    SplitView.minimumHeight: 180
                    dir: topDir
                    showPathField: false
                    allowCollapse: false
                    allowOpenInOtherBrowser: false
                    allowOpenInMainBrowser: true
                    // Both Mode-C browsers participate in active-pane
                    // tracking — registering the same Ctrl+C/X/V/F2
                    // shortcut on both panes was confusing Qt's
                    // QShortcut routing and triggering ops on the
                    // wrong pane. Click either to make it the active
                    // target.
                    active: root.activeBrowser === topBrowser
                    onActivated: root.activeBrowser = topBrowser
                    onOpenInNewTabRequested: (path) => root.openInNewTabRequested(path)
                    onOpenTranscodeQueueRequested: root.openTranscodeQueueRequested()
                }
                FileBrowser {
                    id: bottomBrowser
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 180
                    dir: bottomDir
                    showPathField: false
                    allowCollapse: false
                    allowOpenInOtherBrowser: false
                    allowOpenInMainBrowser: true
                    active: root.activeBrowser === bottomBrowser
                    onActivated: root.activeBrowser = bottomBrowser
                    onOpenInNewTabRequested: (path) => root.openInNewTabRequested(path)
                    onOpenTranscodeQueueRequested: root.openTranscodeQueueRequested()
                }
            }
        }
    }
}
