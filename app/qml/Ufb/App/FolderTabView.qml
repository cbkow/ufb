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
//
// AE tabs only: the bottom (renders) pane grows a renders/proxies
// subtab strip. Both subtabs behave identically — they only change
// which item subfolder the pane targets (renders/ vs proxies/, same
// item-folder fallback). The choice persists per folder via
// Settings.folder_view_prefs and is also stashed in the in-session
// _tabMemory alongside the pane paths.

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

    /// True when `p` is an AE folder tab (folder named "ae", like
    /// detect_add_mode keys on name, not contents). Plain function so
    /// onTabPathChanged can evaluate the INCOMING path directly —
    /// reading the bottomSubtabsEnabled binding inside that handler
    /// is order-dependent (the binding may not have re-evaluated
    /// yet), which left the subtab restore gated on the OUTGOING
    /// tab's value and the strip highlighting "renders" while the
    /// pane showed proxies.
    function _isAeFolder(p) {
        if (!p || p.length === 0) return false
        var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
        var name = idx >= 0 ? p.substring(idx + 1) : p
        return name.toLowerCase() === "ae"
    }
    /// AE folder tabs get a renders/proxies subtab strip on the
    /// bottom (renders) pane; every other folder type renders the
    /// plain single renders pane.
    readonly property bool bottomSubtabsEnabled: _isAeFolder(tabPath)
    /// Active bottom-pane subtab — "renders" | "proxies". Only
    /// meaningful when bottomSubtabsEnabled. Persisted per folder in
    /// the same folder_view_prefs row the browser panes use (read on
    /// tab switch in onTabPathChanged, written on subtab click).
    property string bottomSubtab: "renders"

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
                bottom: bottomDir.current_path,
                bottomTab: bottomSubtab
            }
        }
        // Re-probe layout mode and clear any prior selection state.
        if (tabPath.length > 0) {
            mode = FileOps.detect_folder_layout_mode(tabPath)
        } else {
            mode = "B"
        }
        // Bottom-pane subtab: load the persisted per-folder choice
        // (default "renders"). The session-memory restore below may
        // override it — the two normally agree since clicks write
        // both, but memory wins within a session (matches how the
        // pane paths behave). NOTE: gate on _isAeFolder(tabPath), not
        // the bottomSubtabsEnabled binding — see _isAeFolder.
        var subtabsOn = _isAeFolder(tabPath)
        var savedSub = "renders"
        if (subtabsOn) {
            try {
                savedSub = JSON.parse(Settings.folder_view_prefs(tabPath)).bottomSubtab || "renders"
            } catch (e) { savedSub = "renders" }
        }
        bottomSubtab = savedSub
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
                if (m.bottomTab && subtabsOn) bottomSubtab = m.bottomTab
                if (m.top && m.top.length > 0) topDir.navigate_to(m.top)
                if (m.bottom && m.bottom.length > 0) bottomDir.navigate_to(m.bottom)
            } else if (m.right && m.right.length > 0) {
                rightDir.navigate_to(m.right)
            }
        }
        _memPath = tabPath
    }

    /// Resolve the bottom pane's target subfolder for `itemPath`
    /// under the active subtab. Same fallback rule the renders pane
    /// always had: missing subfolder → the item folder itself.
    function _bottomTargetFor(itemPath) {
        var sub = (bottomSubtabsEnabled && bottomSubtab === "proxies")
            ? FileOps.find_proxies_subdir(itemPath)
            : FileOps.find_render_subdir(itemPath)
        return (sub && sub.length > 0) ? sub : itemPath
    }

    /// Switch the bottom-pane subtab: persist the choice for this
    /// folder (read-merge-write, coexists with the folder's other
    /// view-prefs fields) and re-point the bottom browser for the
    /// currently selected item. Pass persist=false for transient
    /// switches that shouldn't overwrite the user's saved choice.
    function _selectBottomSubtab(name, persist) {
        if (bottomSubtab === name) return
        bottomSubtab = name
        if (persist !== false && tabPath.length > 0) {
            var prefs = ({})
            try { prefs = JSON.parse(Settings.folder_view_prefs(tabPath)) } catch (e) { prefs = ({}) }
            prefs.bottomSubtab = name
            Settings.set_folder_view_prefs(tabPath, JSON.stringify(prefs))
        }
        var sel = itemList.selectedItemPath
        if (root.mode === "C" && sel && sel.length > 0) {
            bottomDir.navigate_to(_bottomTargetFor(sel))
        }
    }

    function _onItemSelected(itemPath) {
        if (root.mode === "C") {
            var proj = FileOps.find_project_subdir(itemPath)
            topDir.navigate_to(proj && proj.length > 0 ? proj : itemPath)
            bottomDir.navigate_to(_bottomTargetFor(itemPath))
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
            var prox = root.bottomSubtabsEnabled
                ? FileOps.find_proxies_subdir(itemPath) : ""
            if (prox && prox.length > 0 && rp.indexOf(norm(prox) + "/") === 0) {
                // Under the item's proxies subdir — flip the bottom
                // pane to the proxies subtab (transient: a link click
                // shouldn't overwrite the saved choice) and reveal
                // there.
                _selectBottomSubtab("proxies", false)
                bottomBrowser.selectAfterLoadPath = revealPath
                bottomDir.navigate_to(parent)
            } else if (rend && rend.length > 0 && rp.indexOf(norm(rend) + "/") === 0) {
                if (root.bottomSubtabsEnabled) _selectBottomSubtab("renders", false)
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
                // Bottom pane. For AE tabs a renders/proxies subtab
                // strip sits above the browser (same styling as the
                // JobView inner tab bar); every other folder type
                // renders the browser alone — the strip collapses to
                // zero height when hidden.
                Item {
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 180

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            visible: root.bottomSubtabsEnabled
                            Layout.fillWidth: true
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            color: Theme.colors.bg

                            Row {
                                anchors.fill: parent
                                spacing: 0
                                clip: true

                                Repeater {
                                    model: ["renders", "proxies"]
                                    delegate: Rectangle {
                                        required property string modelData
                                        readonly property bool isActive: root.bottomSubtab === modelData
                                        height: parent.height
                                        width: Math.max(subtabLabel.implicitWidth + 22, 80)
                                        color: isActive
                                            ? Theme.colors.surfaceHover
                                            : (subtabMa.containsMouse ? Theme.colors.surface : "transparent")

                                        Label {
                                            id: subtabLabel
                                            anchors.centerIn: parent
                                            text: modelData
                                            color: isActive
                                                ? Theme.colors.textBright
                                                : Theme.colors.textMuted
                                            font.pixelSize: Theme.font.sizeBody
                                            font.bold: isActive
                                        }
                                        Rectangle {
                                            visible: isActive
                                            anchors.bottom: parent.bottom
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            height: 2
                                            color: Theme.colors.accentSelected
                                        }
                                        MouseArea {
                                            id: subtabMa
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onClicked: root._selectBottomSubtab(parent.modelData)
                                        }
                                    }
                                }
                            }
                        }

                        FileBrowser {
                            id: bottomBrowser
                            Layout.fillWidth: true
                            Layout.fillHeight: true
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
    }
}
