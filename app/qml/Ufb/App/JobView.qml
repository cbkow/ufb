// JobView — opened from a sidebar subscription click.
//
// Phase 10 slice A: shell only. Renders a header (job name + refresh)
// and a tab bar populated dynamically from the job folder's top-level
// subdirectories (alphabetical), plus a final "Tracker" placeholder
// tab. Each folder tab body is a single FileBrowser pointed at the
// subdir; ItemListPanel + Mode B/C paneling come in slice B/C.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Item {
    id: root

    required property string jobPath
    required property string jobName

    /// Bubbled up from FolderTabView's FileBrowsers "Open in New Tab"
    /// menu item. Main connects this to addFilesTab so a project-tab
    /// browser's right-click can spawn a Files tab just like the dual
    /// browser does.
    signal openInNewTabRequested(string path)

    /// Bubbled up from FolderTabView's FileBrowsers "Transcode to MP4"
    /// menu item. Main connects this to openTranscodeTab so the queue
    /// tab surfaces with the just-queued job visible.
    signal openTranscodeQueueRequested()

    /// List of {name, path} for top-level subfolders of jobPath, in
    /// alphabetical order. Fetched via a private Directory probe.
    ListModel { id: tabsModel }

    /// Hidden Directory instance used purely to list jobPath. We can't
    /// use jobDir for this because jobDir gets re-pointed at each tab's
    /// folder when the user clicks a tab.
    Directory { id: tabsProbe }

    function refreshTabs() {
        tabsModel.clear()
        if (!tabsProbe.entries_json) return
        try {
            var arr = JSON.parse(tabsProbe.entries_json)
            for (var i = 0; i < arr.length; ++i) {
                var e = arr[i]
                if (!e.isDir) continue
                if (e.name.startsWith(".")) continue
                tabsModel.append({ name: e.name, path: e.path })
            }
        } catch (e) {
            console.warn("JobView: tabs parse failed:", e)
        }
        // Default-select the first folder tab on initial load.
        if (jobTabBar.currentIndex < 0 || jobTabBar.currentIndex > tabsModel.count) {
            jobTabBar.currentIndex = 0
        }
        // Per-job subtab memory: the job remembers which folder tab
        // (or the Tracker) was active, stored in the same per-folder
        // prefs entry as the job root's view state. Runs once per
        // JobView; a pending Tracker-click navigation below still
        // wins (it runs after and overrides the index).
        if (!_subtabRestored && tabsModel.count > 0) {
            _subtabRestored = true
            var saved = ""
            try {
                saved = JSON.parse(Settings.folder_view_prefs(root.jobPath)).activeSubtab || ""
            } catch (e) { saved = "" }
            if (saved === "::tracker") {
                jobTabBar.currentIndex = tabsModel.count
            } else if (saved.length > 0) {
                for (var t = 0; t < tabsModel.count; ++t) {
                    if (tabsModel.get(t).name.toLowerCase() === saved.toLowerCase()) {
                        jobTabBar.currentIndex = t
                        break
                    }
                }
            }
        }
        // If a Tracker click queued a "go to item" before our tabs
        // had loaded, apply it now.
        if (_pendingNavFolder.length > 0 && tabsModel.count > 0) {
            var folder = _pendingNavFolder
            var item = _pendingNavItem
            var reveal = _pendingNavReveal
            _pendingNavFolder = ""
            _pendingNavItem = ""
            _pendingNavReveal = ""
            goToFolderItem(folder, item, reveal)
        }
    }

    /// Path of the currently-selected folder tab (or "" when on the
    /// Tracker tab / before any tabs have loaded). Drives the shared
    /// FolderTabView's tabPath property.
    readonly property string _activeFolderPath: {
        var idx = jobTabBar.currentIndex
        if (idx < 0 || idx >= tabsModel.count) return ""
        return tabsModel.get(idx).path
    }
    /// Compatibility no-op kept so older inline references don't break.
    function _syncDirToTab() {}

    /// Gates subtab-memory writes until the one-shot restore in
    /// refreshTabs has run, so restoring doesn't immediately re-save.
    property bool _subtabRestored: false

    /// Debounced write of the active subtab into the job's per-folder
    /// prefs entry. Read-merge-write so it coexists with the job root
    /// folder's own view-state fields in the same row.
    Timer {
        id: _subtabSaveTimer
        interval: 400
        repeat: false
        onTriggered: {
            var idx = jobTabBar.currentIndex
            var name = ""
            if (idx === tabsModel.count) {
                name = "::tracker"
            } else if (idx >= 0 && idx < tabsModel.count) {
                name = tabsModel.get(idx).name
            }
            if (name.length === 0 || root.jobPath.length === 0) return
            var prefs = ({})
            try { prefs = JSON.parse(Settings.folder_view_prefs(root.jobPath)) } catch (e) { prefs = ({}) }
            prefs.activeSubtab = name
            Settings.set_folder_view_prefs(root.jobPath, JSON.stringify(prefs))
        }
    }
    Connections {
        target: jobTabBar
        function onCurrentIndexChanged() {
            if (root._subtabRestored) _subtabSaveTimer.restart()
        }
    }

    /// Buffered pending-navigation slot. When goToFolderItem fires
    /// before the tab list has finished loading (common when JobView
    /// was just instantiated by a Tracker click), we stash the
    /// request here and re-attempt from refreshTabs once the tabs
    /// land. Empty folder = no pending nav.
    property string _pendingNavFolder: ""
    property string _pendingNavItem: ""
    property string _pendingNavReveal: ""

    /// Switch to the inner folder tab matching `folderName` (case-
    /// insensitive) and select `itemPath` in its ItemListPanel.
    /// Used by the per-job Tracker's row-click → "open in project"
    /// navigation, and by Main.qml's aggregated-tracker routing.
    function goToFolderItem(folderName, itemPath, revealPath) {
        if (tabsModel.count === 0) {
            // Tabs haven't loaded yet — buffer and let refreshTabs
            // re-trigger us once they do.
            _pendingNavFolder = folderName
            _pendingNavItem = itemPath
            _pendingNavReveal = revealPath || ""
            return
        }
        var idx = -1
        for (var i = 0; i < tabsModel.count; ++i) {
            if (tabsModel.get(i).name.toLowerCase() === folderName.toLowerCase()) {
                idx = i
                break
            }
        }
        if (idx < 0) {
            console.warn("JobView.goToFolderItem: no tab matches", folderName)
            return
        }
        jobTabBar.currentIndex = idx
        // Defer the selection one event tick so FolderTabView.tabPath
        // updates first (its selectItem assumes ItemListPanel.folderPath
        // already points at the target folder).
        Qt.callLater(function() {
            folderTab.selectItem(itemPath, revealPath)
        })
    }

    onJobPathChanged: {
        if (jobPath.length > 0) tabsProbe.navigate_to(jobPath)
    }
    Component.onCompleted: {
        if (jobPath.length > 0) tabsProbe.navigate_to(jobPath)
    }
    Connections {
        target: tabsProbe
        function onEntries_jsonChanged() { root.refreshTabs() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ────────────────────────────────────────────────────
        // Filled toolbar strip with a single bottom divider — replaces
        // the previous full-border box so the header butts flush against
        // the inner-tab-bar below without a stacked-chrome seam.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            color: Theme.colors.toolbar

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.dim.divider
                color: Theme.colors.divider
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 0
                spacing: 0

                Icon {
                    name: "clipboard-text"
                    size: Theme.icon.sizeMedium
                    color: Theme.colors.textMuted
                    Layout.rightMargin: 6
                }
                Label {
                    text: root.jobName
                    color: Theme.colors.textBright
                    font.pixelSize: Theme.font.sizeHeading
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    text: root.jobPath
                    color: Theme.colors.textSubtle
                    font.pixelSize: Theme.font.sizeSmall
                    font.family: Theme.font.mono
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 360
                    Layout.rightMargin: 8
                }
                // Google Drive notes / folder shortcuts. Both open
                // in the system browser via Apps Script — Google Docs
                // doesn't render usefully in a webview. When the user
                // hasn't filled in scriptUrl / parentFolderId, the
                // first click opens a config dialog instead.
                FlatButton {
                    id: notesDocBtn
                    iconName: "file-text"
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    tooltip: Settings.has_google_drive()
                        ? qsTr("Open project notes doc in browser")
                        : qsTr("Configure Google Drive (click to set up)")
                    onClicked: {
                        if (!Settings.has_google_drive()) {
                            googleDriveDialog.openFor()
                            return
                        }
                        var url = Settings.build_notes_url(root.jobName, "doc")
                        if (url.length > 0) Qt.openUrlExternally(url)
                    }
                }
                FlatButton {
                    iconName: "folder-simple"
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    tooltip: Settings.has_google_drive()
                        ? qsTr("Open project notes folder in browser")
                        : qsTr("Configure Google Drive (click to set up)")
                    onClicked: {
                        if (!Settings.has_google_drive()) {
                            googleDriveDialog.openFor()
                            return
                        }
                        var url = Settings.build_notes_url(root.jobName, "folder")
                        if (url.length > 0) Qt.openUrlExternally(url)
                    }
                }
                FlatButton {
                    iconName: "archive"
                    text: qsTr("Backups")
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    tooltip: qsTr("Manage local snapshot history for this job")
                    onClicked: {
                        backupDialog.jobPath = root.jobPath
                        backupDialog.jobName = root.jobName
                        backupDialog.open()
                    }
                }
                FlatButton {
                    iconName: "arrow-clockwise"
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    tooltip: qsTr("Refresh job (tabs, items, metadata, browsers)")
                    onClicked: {
                        // Top-level subdirs (folder tabs) — pick up
                        // newly-created or renamed top-level folders.
                        tabsProbe.refresh()
                        // Inner tab body — item list + metadata cache
                        // + right-side FileBrowser dirs (Mode B/C).
                        folderTab.refresh()
                        // Tracker data — pull fresh tracked set and
                        // metadata. The job-scoped TrackerView listens
                        // on the singleton's signals, so a singleton
                        // refresh propagates to it automatically.
                        Subscription.refresh_tracked()
                    }
                }
            }
        }

        BackupManagerDialog { id: backupDialog }

        // ── Google Drive config ─────────────────────────────────────
        // Surfaced inline here until a proper Settings dialog exists.
        // Writes to settings.json via Settings.set_google_drive.
        Dialog {
            id: googleDriveDialog
            title: qsTr("Google Drive Integration")
            modal: true
            parent: Overlay.overlay
            x: (parent ? (parent.width  - width)  / 2 : 0)
            y: (parent ? (parent.height - height) / 2 : 0)
            width: 520
            standardButtons: Dialog.Save | Dialog.Cancel
            function openFor() {
                scriptUrlField.text = Settings.google_drive_script_url()
                parentFolderField.text = Settings.google_drive_parent_folder_id()
                gDriveStatus.text = ""
                open()
            }
            ColumnLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 8
                Label {
                    text: qsTr("Apps Script URL and parent folder ID for the project-notes integration. Both buttons in the job header use these to construct a per-job URL that opens in your default browser.")
                    color: "#aaa"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                GridLayout {
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 6
                    Layout.fillWidth: true
                    Label { text: qsTr("Script URL"); color: "#dddddd" }
                    TextField {
                        id: scriptUrlField
                        Layout.fillWidth: true
                        placeholderText: "https://script.google.com/macros/s/…/exec"
                        font.family: "Consolas"
                        font.pixelSize: 11
                    }
                    Label { text: qsTr("Parent Folder ID"); color: "#dddddd" }
                    TextField {
                        id: parentFolderField
                        Layout.fillWidth: true
                        placeholderText: "1AbCdEf…"
                        font.family: "Consolas"
                        font.pixelSize: 11
                    }
                }
                Label {
                    id: gDriveStatus
                    text: ""
                    color: text.indexOf("failed") >= 0 || text.indexOf("error") >= 0
                        ? "#c04040" : "#7ed321"
                    font.pixelSize: 11
                    visible: text.length > 0
                    Layout.fillWidth: true
                }
            }
            onAccepted: {
                var err = Settings.set_google_drive(
                    scriptUrlField.text.trim(),
                    parentFolderField.text.trim())
                if (err.length > 0) {
                    gDriveStatus.text = qsTr("Save failed: %1").arg(err)
                    open()  // re-open so the user can fix
                }
            }
        }

        // ── Inner tab bar ─────────────────────────────────────────────
        // Custom-styled (matches Main.qml top tab bar) — Qt's default
        // TabBar styling renders nearly invisible against our dark
        // theme. tabsModel index N maps to jobTabBar.currentIndex N;
        // tabsModel.count is the Tracker tab.
        // No bottom divider — the active tab's 2px accent underline
        // separates tabs from content. The job header above already
        // carries its own bottom divider, so a second one here would
        // stack chrome at the top of the project view.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            color: Theme.colors.bg

            Row {
                anchors.fill: parent
                spacing: 0
                clip: true

                Repeater {
                    model: tabsModel
                    delegate: Rectangle {
                        readonly property int tabIdx: index
                        height: parent.height
                        width: Math.max(folderLabel.implicitWidth + 22, 80)
                        color: jobTabBar.currentIndex === tabIdx
                            ? Theme.colors.surfaceHover
                            : (folderMa.containsMouse ? Theme.colors.surface : "transparent")

                        Label {
                            id: folderLabel
                            anchors.centerIn: parent
                            text: model.name
                            color: jobTabBar.currentIndex === tabIdx
                                ? Theme.colors.textBright
                                : Theme.colors.textMuted
                            font.pixelSize: Theme.font.sizeBody
                            font.bold: jobTabBar.currentIndex === tabIdx
                        }
                        Rectangle {
                            visible: jobTabBar.currentIndex === tabIdx
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 2
                            color: Theme.colors.accentSelected
                        }
                        MouseArea {
                            id: folderMa
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: jobTabBar.currentIndex = tabIdx
                        }
                    }
                }

                // Tracker tab - same clipboard-text icon as the
                // top-level "All Tracked" tab so they're visually
                // a pair.
                Rectangle {
                    readonly property int tabIdx: tabsModel.count
                    height: parent.height
                    width: innerTrackerRow.implicitWidth + 22
                    color: jobTabBar.currentIndex === tabIdx
                        ? Theme.colors.surfaceHover
                        : (trackerMa.containsMouse ? Theme.colors.surface : "transparent")

                    Row {
                        id: innerTrackerRow
                        anchors.centerIn: parent
                        spacing: 6
                        Icon {
                            name: "clipboard-text"
                            size: Theme.icon.sizeToolbar
                            color: jobTabBar.currentIndex === innerTrackerRow.parent.tabIdx
                                ? Theme.colors.textBright
                                : Theme.colors.textMuted
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            text: qsTr("Tracker")
                            color: jobTabBar.currentIndex === innerTrackerRow.parent.tabIdx
                                ? Theme.colors.textBright
                                : Theme.colors.textMuted
                            font.pixelSize: Theme.font.sizeBody
                            font.bold: jobTabBar.currentIndex === innerTrackerRow.parent.tabIdx
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    Rectangle {
                        visible: jobTabBar.currentIndex === parent.tabIdx
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 2
                        color: Theme.colors.accentSelected
                    }
                    MouseArea {
                        id: trackerMa
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: jobTabBar.currentIndex = parent.tabIdx
                    }
                }
            }
        }

        // Internal index handle (mirrors the visible tab strip above)
        Item {
            id: jobTabBar
            visible: false
            property int currentIndex: 0
            onCurrentIndexChanged: root._syncDirToTab()
        }

        // ── Tab content ───────────────────────────────────────────────
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: {
                // Folder tabs at indices [0, count). Tracker at count.
                var trackerIdx = tabsModel.count
                return jobTabBar.currentIndex === trackerIdx ? 1 : 0
            }

            // Folder tab content — single shared FolderTabView whose
            // tabPath re-points based on the active inner tab. Mode B
            // for now (item list + 1 browser); Mode C lands in slice C.
            FolderTabView {
                id: folderTab
                tabPath: root._activeFolderPath
                jobPath: root.jobPath
                onOpenInNewTabRequested: (path) => root.openInNewTabRequested(path)
                onOpenTranscodeQueueRequested: root.openTranscodeQueueRequested()
            }

            // Tracker tab — slice A: read-only listing of tracked
            // items for this job. Editing + dynamic columns + filter
            // chips land in plan-Phase 11 slice B+.
            TrackerView {
                id: jobTracker
                jobPath: root.jobPath
                jobName: root.jobName
                onGoToItemRequested: (jp, jn, fn, ip) => root.goToFolderItem(fn, ip)
            }
        }
    }
}
