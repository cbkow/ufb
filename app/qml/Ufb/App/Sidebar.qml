// Sidebar — bookmarks · subscriptions · trackers · mounts · mesh.
// All icons via Phosphor; visual tokens via Theme; primitives via
// SectionHeader / FlatButton / Icon. Click a row → emits a signal
// the host (Main) routes (navigate / open job / open tracker tab).
//
// Bookmarks come from the Bookmarks singleton. Subscriptions from
// Subscription. Mounts from Mount.mount_states_json. Mesh status
// from Mesh.status_json (polled via the footer timer).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Rectangle {
    id: root

    // No root border — Sidebar sits inside the outer SplitView, whose
    // splitter is the separator from the main column. Bg color +
    // internal section headers carry the visual structure.
    color: Theme.colors.bg

    signal navigateRequested(string path)
    /// Sidebar row right-click → "Open in New Tab". Host appends a
    /// new Files tab and navigates the active pane there.
    signal openInNewTabRequested(string path)

    /// Pull the file paths out of a DragEvent. Qt6 has been observed
    /// to truncate `drop.urls` to a single entry for native multi-file
    /// drags from Explorer / Finder, while the raw `text/uri-list`
    /// MIME string still carries every URI. Read both and prefer
    /// whichever produced more entries. Mirrors the helper in
    /// FileBrowser.qml.
    function _collectDropPaths(drop) {
        var uris = []
        try {
            if (drop.urls && drop.urls.length > 0) {
                for (var i = 0; i < drop.urls.length; ++i) {
                    uris.push(String(drop.urls[i]))
                }
            }
        } catch (e) {}
        var raw = ""
        try { raw = drop.getDataAsString("text/uri-list") || "" } catch (e) {}
        if (raw.length > 0) {
            var lines = raw.split(/\r?\n/)
            var rawUris = []
            for (var j = 0; j < lines.length; ++j) {
                var line = lines[j].trim()
                if (line.length > 0 && line.charAt(0) !== "#") {
                    rawUris.push(line)
                }
            }
            if (rawUris.length > uris.length) uris = rawUris
        }
        if (uris.length === 0) return []
        try {
            return JSON.parse(FileOps.urls_to_paths(JSON.stringify(uris)))
        } catch (e) {
            return []
        }
    }

    /// Shared drop handler for the bookmark / subscription / mount
    /// rows below. Mirrors FileBrowser._handleDrop so the user gets
    /// the same modifier semantics: Ctrl forces copy, Shift forces
    /// move, otherwise default = move-on-same-volume / copy-cross.
    function _handleDrop(drop, targetPath) {
        if (!targetPath || targetPath.length === 0) {
            drop.accepted = false
            return false
        }
        var paths = root._collectDropPaths(drop)
        if (paths.length === 0) {
            drop.accepted = false
            return false
        }
        // External drags (from Finder/Explorer/other apps) have no
        // source item — always copy them. Moving files out of another
        // app's window on a plain drop is destructive and surprising,
        // and the source app already owns its own modifier semantics.
        // Only UFB-internal drags honour the Ctrl/Shift/same-volume
        // rules, matching FileBrowser._handleDrop.
        var copy
        if (!drop.source) copy = true
        else if (drop.modifiers & Qt.ControlModifier) copy = true
        else if (drop.modifiers & Qt.ShiftModifier) copy = false
        else copy = !FileOps.same_volume(paths[0], targetPath)

        if (copy) {
            FileOps.copy_files(JSON.stringify(paths), targetPath)
        } else {
            FileOps.move_files(JSON.stringify(paths), targetPath)
        }
        drop.accept(copy ? Qt.CopyAction : Qt.MoveAction)
        return true
    }
    signal openJobRequested(string jobPath, string jobName)
    signal openTrackerRequested()
    signal openTranscodeRequested()

    ListModel { id: bookmarksModel }
    ListModel { id: subscriptionsModel }
    ListModel { id: mountsModel }

    // Built-in bookmark rows — Desktop / Downloads / Documents at the
    // top of the BOOKMARKS list. Always present, can't be removed.
    // Each has its own Phosphor icon so they read as system folders
    // distinct from the user's own bookmarks.
    function _systemBookmarks() {
        var home = Paths.home_path()
        if (!home || home.length === 0) return []
        var sep = home.indexOf("\\") >= 0 ? "\\" : "/"
        function join(parent, child) {
            return parent.replace(/[\\\/]+$/, "") + sep + child
        }
        return [
            { displayName: qsTr("Desktop"),   path: join(home, "Desktop"),
              icon: "desktop", system: true },
            { displayName: qsTr("Downloads"), path: join(home, "Downloads"),
              icon: "download-simple", system: true },
            { displayName: qsTr("Documents"), path: join(home, "Documents"),
              icon: "file-text", system: true },
        ]
    }

    function refreshBookmarks() {
        bookmarksModel.clear()
        // Always-present system rows first.
        var sys = _systemBookmarks()
        for (var k = 0; k < sys.length; ++k) {
            bookmarksModel.append({
                displayName: sys[k].displayName,
                path: sys[k].path,
                isProjectFolder: false,
                icon: sys[k].icon,
                system: true
            })
        }
        if (!Bookmarks.bookmarks_json) return
        try {
            var arr = JSON.parse(Bookmarks.bookmarks_json)
            for (var i = 0; i < arr.length; ++i) {
                var b = arr[i]
                bookmarksModel.append({
                    displayName: b.displayName || b.path,
                    path: b.path,
                    isProjectFolder: !!b.isProjectFolder,
                    icon: "",          // delegate falls back to type-based default
                    system: false
                })
            }
        } catch (e) {
            console.warn("Sidebar: bookmarks parse failed:", e)
        }
    }

    function refreshSubscriptions() {
        subscriptionsModel.clear()
        if (!Subscription.subscriptions_json) return
        try {
            var arr = JSON.parse(Subscription.subscriptions_json)
            for (var i = 0; i < arr.length; ++i) {
                var s = arr[i]
                subscriptionsModel.append({
                    jobPath: s.jobPath || "",
                    jobName: s.jobName || s.jobPath || "(job)"
                })
            }
        } catch (e) {
            console.warn("Sidebar: subscriptions parse failed:", e)
        }
    }

    function refreshMounts() {
        mountsModel.clear()
        if (!Mount.mount_states_json) return
        try {
            var arr = JSON.parse(Mount.mount_states_json)
            for (var i = 0; i < arr.length; ++i) {
                var m = arr[i]
                mountsModel.append({
                    mountId: m.mountId || "",
                    displayName: m.displayName || m.mountId || "(mount)",
                    mountPath: m.volumePath || "",
                    credentialKey: m.credentialKey || "",
                    // Unmanaged bookmarks have no lifecycle: show
                    // "bookmark" as their status, always navigable.
                    unmanaged: m.unmanaged === true,
                    notice: m.notice || "",
                    noticeFixable: m.noticeFixable === true,
                    state: m.unmanaged === true ? qsTr("bookmark") : (m.state || "(waiting)")
                })
            }
        } catch (e) {
            console.warn("Sidebar: mounts parse failed:", e)
        }
    }

    function _mountStateColor(state) {
        switch (String(state || "").toLowerCase()) {
        case "mounted":     return Theme.colors.success
        case "mounting":    return Theme.colors.warning
        case "unmounted":   return Theme.colors.textSubtle
        case "unmounting":  return Theme.colors.warning
        case "error":       return Theme.colors.error
        case "auth_error":  return Theme.colors.error
        case "unreachable": return Theme.colors.textMuted
        }
        return Theme.colors.textMuted
    }

    Connections {
        target: Bookmarks
        function onBookmarks_jsonChanged() { root.refreshBookmarks() }
    }
    Connections {
        target: Subscription
        function onSubscriptions_jsonChanged() { root.refreshSubscriptions() }
    }
    Connections {
        target: Mount
        function onMount_states_jsonChanged() { root.refreshMounts() }
    }
    // Inbound mesh refresh — peers pushed a sub_add/sub_remove or a
    // snapshot just landed. Re-read the local subscriptions table so
    // the sidebar reflects what's now in the DB. The QObject's
    // refresh() is cheap (one indexed query); fire on either rev.
    Connections {
        target: Mesh
        // sub_*/col_*/preset_* peer push. Drop the column cache so
        // open ItemListPanels re-pull defs on next paint, and
        // refresh subscriptions so the sidebar list reflects sub_add/
        // sub_remove from peers.
        function onTable_change_revChanged() {
            Subscription.refresh()
            Columns.invalidate("", "")
        }
        // Per-item edit pushed by a peer — refresh_tracked rebuilds
        // tracked_items_json which TrackerView / ItemListPanel observe.
        function onMetadata_change_revChanged() { Subscription.refresh_tracked() }
        // Snapshot restore landed on the local DB — coarse refresh
        // everything mesh-managed.
        function onData_refresh_revChanged() {
            Subscription.refresh()
            Subscription.refresh_tracked()
            Columns.invalidate("", "")
        }
    }
    Component.onCompleted: {
        Bookmarks.refresh()
        Subscription.refresh()
        refreshBookmarks()
        refreshSubscriptions()
        refreshMounts()
    }

    AddBookmarkDialog { id: addBookmarkDialog }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Bookmarks ───────────────────────────────────────────────
        SectionHeader {
            Layout.fillWidth: true
            Layout.topMargin: Theme.dim.padding
            text: qsTr("BOOKMARKS")
            FlatButton {
                iconName: "plus"
                tooltip: qsTr("Add bookmark…")
                width: 22
                height: 18
                padding: 0
                iconSize: Theme.icon.sizeSmall
                onClicked: addBookmarkDialog.openForBookmark()
            }
        }
        ListView {
            id: bookmarksList
            Layout.fillWidth: true
            // Cap matches the wider expected count (~12 bookmarks
            // visible without scrolling at the dense 22px row).
            Layout.preferredHeight: Math.min(contentHeight, 280)
            model: bookmarksModel
            interactive: contentHeight > height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: UfbScrollBar {}

            delegate: Rectangle {
                id: bmRow
                width: bookmarksList.width
                height: Theme.dim.rowHeightDense
                color: bmDropTarget.containsDrag
                    ? Theme.colors.accentMuted
                    : (bmHover.containsMouse
                        ? Theme.colors.surfaceHover
                        : "transparent")
                property string bmPath: model.path
                property string bmName: model.displayName
                property bool bmSystem: !!model.system

                DropArea {
                    id: bmDropTarget
                    anchors.fill: parent
                    keys: ["text/uri-list"]
                    z: 100
                    onDropped: (drop) => root._handleDrop(drop, bmRow.bmPath)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.dim.padding
                    anchors.rightMargin: Theme.dim.padding + Theme.dim.scrollBarWidth
                    spacing: Theme.dim.spacingLoose
                    Icon {
                        // System rows carry their own icon name (Desktop /
                        // Downloads / Documents); user rows fall back to
                        // briefcase for project folders, star otherwise.
                        name: model.icon && model.icon.length > 0
                            ? model.icon
                            : (model.isProjectFolder ? "briefcase" : "star")
                        size: Theme.icon.sizeToolbar
                        color: Theme.colors.textMuted
                    }
                    Label {
                        Layout.fillWidth: true
                        text: model.displayName
                        color: Theme.colors.text
                        font.pixelSize: Theme.font.sizeBody
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    id: bmHover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton) bmMenu.popup()
                        else root.navigateRequested(bmRow.bmPath)
                    }
                }
                Menu {
                    id: bmMenu
                    MenuItem { text: qsTr("Open"); onTriggered: root.navigateRequested(bmRow.bmPath) }
                    MenuItem { text: qsTr("Open in New Tab"); onTriggered: root.openInNewTabRequested(bmRow.bmPath) }
                    MenuItem { text: qsTr("Reveal in Explorer"); onTriggered: FileOps.reveal_in_file_manager(bmRow.bmPath) }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Copy ufb:// Link"); onTriggered: FileOps.clipboard_copy_text(FileOps.build_ufb_uri(bmRow.bmPath)) }
                    MenuItem { text: qsTr("Copy Path"); onTriggered: FileOps.clipboard_copy_text(bmRow.bmPath) }
                    MenuSeparator {}
                    MenuItem {
                        // System rows (Desktop / Downloads / Documents)
                        // can't be removed - hide Remove for them.
                        visible: !bmRow.bmSystem
                        height: visible ? implicitHeight : 0
                        text: qsTr("Remove")
                        onTriggered: Bookmarks.remove(bmRow.bmPath)
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: bookmarksModel.count === 0
                text: qsTr("(no bookmarks)")
                color: Theme.colors.textSubtle
                font.pixelSize: Theme.font.sizeSmall
                font.italic: true
            }
        }

        // ── Subscriptions ───────────────────────────────────────────
        // No "+" here: subscriptions are individual jobs being
        // tracked, created via the New Job template flow or
        // automatically when a job is opened. Manually adding a
        // path to subscribe doesn't match how the rest of the app
        // produces them.
        SectionHeader {
            Layout.fillWidth: true
            Layout.topMargin: Theme.dim.padding
            text: qsTr("SUBSCRIPTIONS")
        }
        ListView {
            id: subscriptionsList
            Layout.fillWidth: true
            // Stacked rows are 38px each; cap roughly fits 8–9 rows
            // before the inner scrollbar kicks in. Was 220 (only 5
            // rows fit at the new height — felt truncated).
            Layout.preferredHeight: Math.min(contentHeight, 360)
            model: subscriptionsModel
            interactive: contentHeight > height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: UfbScrollBar {}

            delegate: Rectangle {
                id: subRow
                width: subscriptionsList.width
                height: Theme.dim.rowHeight
                color: subDropTarget.containsDrag
                    ? Theme.colors.accentMuted
                    : (subHover.containsMouse
                        ? Theme.colors.surfaceHover
                        : "transparent")
                property string sJobPath: model.jobPath
                property string sJobName: model.jobName

                DropArea {
                    id: subDropTarget
                    anchors.fill: parent
                    keys: ["text/uri-list"]
                    z: 100
                    onDropped: (drop) => root._handleDrop(drop, subRow.sJobPath)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.dim.padding
                    anchors.rightMargin: Theme.dim.padding + Theme.dim.scrollBarWidth
                    spacing: Theme.dim.spacingLoose
                    Icon {
                        name: "clipboard-text"
                        size: Theme.icon.sizeToolbar
                        color: Theme.colors.textMuted
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Label {
                        text: model.jobName
                        color: Theme.colors.text
                        font.pixelSize: Theme.font.sizeBody
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                    }
                }
                MouseArea {
                    id: subHover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton) subMenu.popup()
                        else root.openJobRequested(subRow.sJobPath, subRow.sJobName)
                    }
                }
                Menu {
                    id: subMenu
                    MenuItem { text: qsTr("Open Job"); onTriggered: root.openJobRequested(subRow.sJobPath, subRow.sJobName) }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Reveal Folder in Pane"); onTriggered: root.navigateRequested(subRow.sJobPath) }
                    MenuItem { text: qsTr("Reveal in Explorer"); onTriggered: FileOps.reveal_in_file_manager(subRow.sJobPath) }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Copy ufb:// Link"); onTriggered: FileOps.clipboard_copy_text(FileOps.build_ufb_uri(subRow.sJobPath)) }
                    MenuItem { text: qsTr("Copy Path"); onTriggered: FileOps.clipboard_copy_text(subRow.sJobPath) }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("Unsubscribe")
                        onTriggered: {
                            unsubscribeConfirm.pendingPath = subRow.sJobPath
                            unsubscribeConfirm.pendingName = subRow.sJobName
                            unsubscribeConfirm.open()
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: subscriptionsModel.count === 0
                text: qsTr("(no subscriptions)")
                color: Theme.colors.textSubtle
                font.pixelSize: Theme.font.sizeSmall
                font.italic: true
            }
        }

        // ── Trackers ────────────────────────────────────────────────
        SectionHeader {
            Layout.fillWidth: true
            Layout.topMargin: Theme.dim.padding
            text: qsTr("TRACKERS")
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.rowHeight
            color: trackerHover.containsMouse
                ? Theme.colors.surfaceHover
                : "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.dim.padding
                spacing: Theme.dim.spacingLoose
                Icon {
                    name: "list-checks"
                    size: Theme.icon.sizeToolbar
                    color: Theme.colors.textMuted
                }
                Label {
                    text: qsTr("All Tracked")
                    color: Theme.colors.text
                    font.pixelSize: Theme.font.sizeBody
                    Layout.fillWidth: true
                }
            }
            MouseArea {
                id: trackerHover
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.openTrackerRequested()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.rowHeight
            color: transcodeHover.containsMouse
                ? Theme.colors.surfaceHover
                : "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.dim.padding
                spacing: Theme.dim.spacingLoose
                Icon {
                    name: "stack"
                    size: Theme.icon.sizeToolbar
                    color: Theme.colors.textMuted
                }
                Label {
                    text: qsTr("Transcode Queue")
                    color: Theme.colors.text
                    font.pixelSize: Theme.font.sizeBody
                    Layout.fillWidth: true
                }
            }
            MouseArea {
                id: transcodeHover
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.openTranscodeRequested()
            }
        }

        // ── Mounts ──────────────────────────────────────────────────
        SectionHeader {
            Layout.fillWidth: true
            Layout.topMargin: Theme.dim.padding
            text: qsTr("MOUNTS")
            FlatButton {
                iconName: "gear-six"
                iconSize: Theme.icon.sizeSmall
                tooltip: qsTr("Mount management")
                implicitHeight: 18
                implicitWidth: 22
                padding: 2
                onClicked: mountManagerDialog.open()
            }
        }
        MountManagerDialog { id: mountManagerDialog }

        ListView {
            id: mountsList
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: mountsModel
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: UfbScrollBar {}

            delegate: Rectangle {
                id: mountRow
                width: mountsList.width
                height: Theme.dim.rowHeightStacked
                color: mtDropTarget.containsDrag
                    ? Theme.colors.accentMuted
                    : (mtHover.containsMouse
                        ? Theme.colors.surfaceHover
                        : "transparent")
                opacity: model.mountPath.length > 0 ? 1.0 : 0.5

                readonly property string mountId: model.mountId || ""
                readonly property string mountPath: model.mountPath || ""
                readonly property string mountState: model.state || ""
                readonly property bool mountReady: mountPath.length > 0

                DropArea {
                    id: mtDropTarget
                    anchors.fill: parent
                    keys: ["text/uri-list"]
                    z: 100
                    enabled: mountRow.mountReady
                    onDropped: (drop) => root._handleDrop(drop, mountRow.mountPath)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.dim.padding
                    anchors.rightMargin: Theme.dim.padding + Theme.dim.scrollBarWidth
                    spacing: Theme.dim.spacingLoose
                    // Above mtHover (the later sibling that otherwise
                    // covers the whole row): the inline pills' own
                    // MouseAreas must win the click. Non-interactive
                    // children (labels, icons) don't accept mouse
                    // events, so row clicks still fall through to
                    // mtHover everywhere except on a pill.
                    z: 1

                    Rectangle {
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4
                        // Bookmark-only entries get the accent blue —
                        // visually distinct from lifecycle states
                        // (green/amber/red) since they have none.
                        color: model.unmanaged === true
                            ? Theme.colors.accent
                            : root._mountStateColor(model.state)
                    }
                    Icon {
                        name: model.unmanaged === true ? "bookmark-simple" : "hard-drives"
                        size: Theme.icon.sizeToolbar
                        color: Theme.colors.textMuted
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 2
                        Label {
                            text: model.displayName
                            color: Theme.colors.text
                            font.pixelSize: Theme.font.sizeBody
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            // Promote auth_error from internal slug to
                            // a friendlier label - the inline pill on
                            // the right tells the user what to do.
                            // Windows: mounted rows append the live
                            // drive letter ("mounted · F:") — letters
                            // are user-chosen, facility-standardized
                            // facts now (slice B), worth surfacing
                            // without opening the editor or Explorer.
                            text: {
                                var st = String(model.state).toLowerCase()
                                if (st === "auth_error") return qsTr("Authentication failed")
                                if (Qt.platform.os === "windows"
                                    && st === "mounted"
                                    && /^[A-Za-z]:/.test(model.mountPath || "")) {
                                    return model.state + " · "
                                        + model.mountPath.substring(0, 2).toUpperCase()
                                }
                                return model.state
                            }
                            color: String(model.state).toLowerCase() === "auth_error"
                                ? Theme.colors.error
                                : Theme.colors.textSubtle
                            font.pixelSize: Theme.font.sizeTiny
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                    // Mount-drift notice (plans/17 slice C): the share
                    // came up at a dedup-suffixed /Volumes name because
                    // the expected one was taken. Everything still
                    // works (identity resolution follows the real
                    // location) — this ⚠ just explains the odd path.
                    Icon {
                        visible: (model.notice || "").length > 0
                        name: "warning"
                        size: Theme.icon.sizeSmall
                        color: Theme.colors.warning
                        Layout.alignment: Qt.AlignVCenter
                        ToolTip.text: model.notice || ""
                        ToolTip.visible: noticeMa.containsMouse
                        ToolTip.delay: 300
                        MouseArea {
                            id: noticeMa
                            anchors.fill: parent
                            hoverEnabled: true
                        }
                    }
                    // Actionable half of the drift notice: the
                    // expected /Volumes name is blocked by a stale
                    // leftover folder (empty, unmounted). The pill
                    // removes it with admin rights (one system auth
                    // prompt) and restarts the mount so it lands on
                    // the clean name. A live foreign volume never
                    // sets noticeFixable — no pill, tooltip only.
                    Rectangle {
                        visible: model.noticeFixable === true
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredHeight: 22
                        implicitWidth: fixDriftLabel.implicitWidth + 16
                        radius: 11
                        color: fixDriftHover.containsMouse
                            ? Theme.colors.warning
                            : Theme.colors.warningMuted || Theme.colors.warning
                        opacity: fixDriftHover.containsMouse ? 1.0 : 0.85
                        Label {
                            id: fixDriftLabel
                            anchors.centerIn: parent
                            text: qsTr("Fix…")
                            color: "#ffffff"
                            font.pixelSize: Theme.font.sizeTiny
                            font.bold: true
                        }
                        MouseArea {
                            id: fixDriftHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: (mouse) => {
                                // Eat the click so the row's own
                                // navigate-on-click MouseArea doesn't
                                // also fire.
                                mouse.accepted = true
                                console.log("[Sidebar] Fix pill clicked for", model.mountId)
                                var err = Mount.fix_blocked_mountpoint(model.mountId)
                                if (err && err.length > 0)
                                    console.warn("[Sidebar] fix_blocked_mountpoint:", err)
                            }
                        }
                    }
                    // Inline auth-recovery pill - the user's path out
                    // of an auth_error state. Both OSes: reconnecting
                    // IS the sign-in (plans/17 decision 4). The restart
                    // rides allow_ui, so the OS credential dialog
                    // (NetAuthAgent / the standard Windows prompt with
                    // "Remember my credentials") appears exactly when
                    // the server rejects stored credentials.
                    Rectangle {
                        visible: String(model.state).toLowerCase() === "auth_error"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredHeight: 22
                        implicitWidth: fixCredsLabel.implicitWidth + 16
                        radius: 11
                        color: fixCredsHover.containsMouse
                            ? Theme.colors.error
                            : Theme.colors.errorMuted || Theme.colors.error
                        opacity: fixCredsHover.containsMouse ? 1.0 : 0.85
                        Label {
                            id: fixCredsLabel
                            anchors.centerIn: parent
                            text: qsTr("Sign in…")
                            color: "#ffffff"
                            font.pixelSize: Theme.font.sizeTiny
                            font.bold: true
                        }
                        MouseArea {
                            id: fixCredsHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: (mouse) => {
                                // Eat the click so the row's own
                                // navigate-on-click MouseArea below
                                // doesn't also fire and try to open
                                // a folder we can't read.
                                mouse.accepted = true
                                Mount.restart_mount(model.mountId)
                            }
                        }
                    }
                }
                MouseArea {
                    id: mtHover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: mountRow.mountReady
                        ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            mountMenu.popup()
                            return
                        }
                        if (mountRow.mountReady) {
                            root.navigateRequested(mountRow.mountPath)
                        }
                    }
                }

                // Right-click menu. Available even when the mount
                // isn't yet mounted (mountPath empty) so the user can
                // start it / open logs to diagnose.
                Menu {
                    id: mountMenu
                    MenuItem {
                        text: qsTr("Open")
                        enabled: mountRow.mountReady
                        onTriggered: root.navigateRequested(mountRow.mountPath)
                    }
                    MenuItem {
                        text: qsTr("Open in New Tab")
                        enabled: mountRow.mountReady
                        onTriggered: root.openInNewTabRequested(mountRow.mountPath)
                    }
                    MenuItem {
                        text: qsTr("Reveal in Explorer")
                        enabled: mountRow.mountReady
                        onTriggered: FileOps.reveal_in_file_manager(mountRow.mountPath)
                    }
                    MenuSeparator { visible: model.unmanaged !== true }
                    MenuItem {
                        visible: model.unmanaged !== true
                        height: visible ? implicitHeight : 0
                        text: qsTr("Start")
                        onTriggered: Mount.start_mount(mountRow.mountId)
                    }
                    MenuItem {
                        visible: model.unmanaged !== true
                        height: visible ? implicitHeight : 0
                        text: qsTr("Stop")
                        onTriggered: Mount.stop_mount(mountRow.mountId)
                    }
                    MenuItem {
                        visible: model.unmanaged !== true
                        height: visible ? implicitHeight : 0
                        text: qsTr("Restart")
                        onTriggered: Mount.restart_mount(mountRow.mountId)
                    }
                    MenuSeparator { visible: model.unmanaged !== true }
                    // "Set/Update Credentials" is gone (plans/17
                    // decision 4, Windows completed in slice B):
                    // authentication is a property of connecting.
                    // Restart rides allow_ui and the OS dialog appears
                    // exactly when stored credentials are rejected.
                    MenuItem {
                        text: qsTr("Drain Cache…")
                        onTriggered: {
                            mountDrainConfirm.pendingId = mountRow.mountId
                            mountDrainConfirm.pendingName = model.displayName
                            mountDrainConfirm.open()
                        }
                    }
                    MenuItem {
                        text: qsTr("Open Agent Log")
                        onTriggered: {
                            var err = Mount.open_logs(mountRow.mountId)
                            if (err.length > 0) {
                                console.warn("Sidebar: open_logs:", err)
                            }
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: mountsModel.count === 0
                text: Mount.connected ? qsTr("(no mounts)") : qsTr("(agent waiting)")
                color: Theme.colors.textSubtle
                font.pixelSize: Theme.font.sizeSmall
                font.italic: true
            }
        }

        // ── Path swaps footer ───────────────────────────────────────
        // Cross-OS prefix mappings used to translate ufb:// links.
        // The status text just says active/none-added; the detail
        // (window/mac path pairs) lives in the dialog.
        Rectangle {
            id: pathSwapsFooter
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            color: Theme.colors.toolbar

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: Theme.dim.divider
                color: Theme.colors.divider
            }

            // Live count of enabled mappings. Refreshed when the
            // dialog's `changed` signal fires.
            property int activeCount: 0
            function _refresh() {
                activeCount = Settings.path_mappings_active_count()
            }
            Component.onCompleted: _refresh()

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.dim.padding
                anchors.rightMargin: Theme.dim.spacingTight
                spacing: Theme.dim.spacingLoose

                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    Layout.alignment: Qt.AlignVCenter
                    radius: 4
                    color: pathSwapsFooter.activeCount > 0
                        ? Theme.colors.success
                        : Theme.colors.textSubtle
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        text: qsTr("Path swaps")
                        color: Theme.colors.text
                        font.pixelSize: Theme.font.sizeSmall
                        font.bold: true
                    }
                    Label {
                        text: pathSwapsFooter.activeCount > 0
                            ? qsTr("Active · %1 mapping(s)").arg(pathSwapsFooter.activeCount)
                            : qsTr("None added")
                        color: Theme.colors.textSubtle
                        font.pixelSize: Theme.font.sizeTiny
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
                FlatButton {
                    iconName: "gear-six"
                    iconSize: Theme.icon.sizeSmall
                    tooltip: qsTr("Path-swap settings")
                    implicitHeight: parent.height - 6
                    implicitWidth: 26
                    padding: 4
                    onClicked: pathSwapsDialog.openFor()
                }
            }
        }
        PathSwapsDialog {
            id: pathSwapsDialog
            onChanged: pathSwapsFooter._refresh()
        }

        // ── Mesh sync footer ────────────────────────────────────────
        // No top divider — the Path-swaps footer above already carries
        // one, and both share Theme.colors.toolbar so the boundary
        // between them needs no second line.
        Rectangle {
            id: meshFooter
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            color: Theme.colors.toolbar

            property var meshStatus: ({})
            function _refresh() {
                Mesh.refresh_status()
                try { meshStatus = JSON.parse(Mesh.status_json) }
                catch (e) { meshStatus = {} }
            }
            Component.onCompleted: _refresh()
            Timer {
                interval: 4000
                running: meshFooter.visible
                repeat: true
                onTriggered: meshFooter._refresh()
            }
            Connections {
                target: Mesh
                function onStatus_jsonChanged() {
                    try { meshFooter.meshStatus = JSON.parse(Mesh.status_json) }
                    catch (e) {}
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.dim.padding
                anchors.rightMargin: Theme.dim.spacingTight
                spacing: Theme.dim.spacingLoose

                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    Layout.alignment: Qt.AlignVCenter
                    radius: 4
                    color: {
                        var s = meshFooter.meshStatus || {}
                        if (!s.isEnabled) return Theme.colors.textSubtle
                        if (s.isLeader)   return Theme.colors.success
                        return Theme.colors.accent
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        text: qsTr("Mesh sync")
                        color: Theme.colors.text
                        font.pixelSize: Theme.font.sizeSmall
                        font.bold: true
                    }
                    Label {
                        text: {
                            var s = meshFooter.meshStatus || {}
                            var msg = s.statusMessage || qsTr("Not configured")
                            if (s.isEnabled && s.peerCount > 0) {
                                msg += " · " + qsTr("%1 peer(s)").arg(s.peerCount)
                            }
                            return msg
                        }
                        color: Theme.colors.textSubtle
                        font.pixelSize: Theme.font.sizeTiny
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
                FlatButton {
                    iconName: "gear-six"
                    iconSize: Theme.icon.sizeSmall
                    tooltip: qsTr("Mesh sync settings")
                    implicitHeight: parent.height - 6
                    implicitWidth: 26
                    padding: 4
                    onClicked: meshSettingsDialog.openFor()
                }
            }
        }
        MeshSettingsDialog {
            id: meshSettingsDialog
            onChanged: meshFooter._refresh()
        }
    }

    // Mount cache drain confirm. Lives at the sidebar root so it
    // survives the per-row delegate going out of scope while the
    // dialog is open.
    Dialog {
        id: mountDrainConfirm
        title: qsTr("Drain mount cache?")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        standardButtons: Dialog.Yes | Dialog.Cancel
        property string pendingId: ""
        property string pendingName: ""
        onAccepted: {
            if (pendingId.length > 0) {
                Mount.clear_sync_cache(pendingId)
            }
            pendingId = ""
            pendingName = ""
        }
        onRejected: {
            pendingId = ""
            pendingName = ""
        }
        contentItem: Label {
            text: qsTr("Clear the local cache for \"%1\"? Files synced back to the source are kept; pending uploads are flushed.")
                .arg(mountDrainConfirm.pendingName.length > 0
                    ? mountDrainConfirm.pendingName
                    : qsTr("this mount"))
            color: Theme.colors.text
            font.pixelSize: Theme.font.sizeBody
            wrapMode: Text.Wrap
        }
    }

    // Unsubscribe confirm (plans/12). Same root-level Overlay pattern as
    // mountDrainConfirm so it survives the per-row delegate closing.
    // Subscriptions are per-user (1.1.1+), so this only affects THIS
    // user's sidebar + All Tracker scope — the job's metadata keeps
    // syncing and resubscribing restores the view.
    Dialog {
        id: unsubscribeConfirm
        title: qsTr("Unsubscribe from job?")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        standardButtons: Dialog.Yes | Dialog.Cancel
        property string pendingPath: ""
        property string pendingName: ""
        onAccepted: {
            if (pendingPath.length > 0) {
                Subscription.unsubscribe(pendingPath)
            }
            pendingPath = ""
            pendingName = ""
        }
        onRejected: {
            pendingPath = ""
            pendingName = ""
        }
        contentItem: Label {
            text: qsTr("Remove \"%1\" from your sidebar and tracker? This only affects you — the job's data stays shared, and resubscribing restores it.")
                .arg(unsubscribeConfirm.pendingName.length > 0
                    ? unsubscribeConfirm.pendingName
                    : qsTr("this job"))
            color: Theme.colors.text
            font.pixelSize: Theme.font.sizeBody
            wrapMode: Text.Wrap
        }
    }
}
