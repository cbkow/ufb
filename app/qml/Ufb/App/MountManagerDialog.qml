// MountManagerDialog — list / start / stop / restart / add / edit /
// remove mount configs. Opened from the sidebar's MOUNTS gear.
//
// Model: full MountsConfig JSON read via Mount.read_mount_configs.
// Edits go to a local mutable copy; Save serialises back via
// Mount.write_mount_configs which persists to mounts.json AND tells
// the agent to reload. Per-mount Start/Stop/Restart/ClearCache
// commands fire-and-forget — state updates flow back via the
// existing mount_states_json events.

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ufb.Backend 1.0

Dialog {
    id: dialog

    title: qsTr("Mount Management")
    modal: true
    parent: Overlay.overlay
    x: (parent ? (parent.width  - width)  / 2 : 0)
    y: (parent ? (parent.height - height) / 2 : 0)
    width: 820
    height: 560
    standardButtons: Dialog.Close

    /// Full mounts.json content. Parsed on open, edited in place by
    /// the inline editor / Add button, written back via Save (which
    /// the per-row Apply button on the editor wires up).
    property var configsObj: ({ version: 1, mounts: [] })
    /// Live mount-state map keyed by id (from Mount.mount_states_json
    /// merged data). Used to render the state pill per row.
    property var statesByMount: ({})

    function _refresh() {
        try {
            configsObj = JSON.parse(Mount.read_mount_configs())
        } catch (e) {
            console.warn("MountManagerDialog: parse failed:", e)
            configsObj = { version: 1, mounts: [] }
        }
        _refreshStates()
    }
    function _refreshStates() {
        var map = {}
        try {
            var arr = JSON.parse(Mount.mount_states_json)
            for (var i = 0; i < arr.length; ++i) {
                var s = arr[i]
                if (s && s.mountId) map[s.mountId] = s
            }
        } catch (e) {}
        statesByMount = map
    }

    onAboutToShow: _refresh()
    Connections {
        target: Mount
        function onMount_states_jsonChanged() { dialog._refreshStates() }
    }

    function _stateColor(state) {
        switch (String(state || "").toLowerCase()) {
        case "mounted":     return Theme.colors.success
        case "mounting":    return Theme.colors.warning
        case "unmounted":   return Theme.colors.textSubtle
        case "unmounting":  return Theme.colors.warning
        case "auth_error":  return Theme.colors.error
        case "error":       return Theme.colors.error
        case "unreachable": return Theme.colors.textMuted
        }
        return Theme.colors.textMuted
    }

    /// Leaf names of enabled mounts that collide (two shares both
    /// ending in "Jobs" would fight over /Volumes/Jobs — the loser
    /// mounts at a dedup-suffixed name). Returns [] when clean.
    function _collidingLeafNames() {
        var seen = {}
        var dupes = {}
        var ms = dialog.configsObj.mounts || []
        for (var i = 0; i < ms.length; ++i) {
            var m = ms[i]
            if (!m || m.enabled === false || !m.nasSharePath) continue
            var p = String(m.nasSharePath).replace(/[\\\/]+$/, "")
            var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
            var leaf = (idx >= 0 ? p.substring(idx + 1) : p).toLowerCase()
            if (leaf.length === 0) continue
            if (seen[leaf]) dupes[leaf] = true
            seen[leaf] = true
        }
        return Object.keys(dupes)
    }

    /// Drive letters claimed by more than one enabled mount (Windows).
    /// One of them will land elsewhere with a drift notice.
    function _collidingLetters() {
        var seen = {}
        var dupes = {}
        var ms = dialog.configsObj.mounts || []
        for (var i = 0; i < ms.length; ++i) {
            var m = ms[i]
            if (!m || m.enabled === false || m.unmanaged === true) continue
            var letter = String(m.mountDriveLetter || "").trim().charAt(0).toUpperCase()
            if (letter.length === 0) continue
            if (seen[letter]) dupes[letter] = true
            seen[letter] = true
        }
        return Object.keys(dupes)
    }

    function _saveAll() {
        var json = JSON.stringify(dialog.configsObj)
        var err = Mount.write_mount_configs(json)
        if (err.length > 0) {
            saveStatus.text = qsTr("Save failed: %1").arg(err)
        } else {
            var warnings = []
            var dupes = dialog._collidingLeafNames()
            if (dupes.length > 0) {
                warnings.push(qsTr("mounts share the name “%1” — one will mount at a suffixed path").arg(dupes.join("”, “")))
            }
            var letterDupes = dialog._collidingLetters()
            if (letterDupes.length > 0) {
                warnings.push(qsTr("two mounts claim drive letter %1: — one will land on another letter").arg(letterDupes.join(":, ")))
            }
            saveStatus.text = warnings.length > 0
                ? qsTr("Saved · warning: %1").arg(warnings.join(" · "))
                : qsTr("Saved · agent reloading")
            saveStatusTimer.restart()
        }
    }
    function _anySyncEnabled() {
        var ms = dialog.configsObj && dialog.configsObj.mounts
        if (!ms) return false
        for (var i = 0; i < ms.length; ++i) {
            if (ms[i] && ms[i].syncEnabled === true) return true
        }
        return false
    }
    /// Resolved default sync cache path (matches the agent's logic).
    /// Already env-expanded — safe to pass to reveal_in_file_manager
    /// or create_directory.
    function _defaultCacheRoot() {
        return Paths.default_sync_cache_root()
    }
    function _urlToLocalPath(u) {
        var s = String(u)
        var p = s.replace(/^file:[\/\\]+/, "")
        if (s.indexOf("file:////") === 0) p = "\\\\" + p
        return Paths.platform() === "windows" ? p.replace(/\//g, "\\") : "/" + p
    }
    Timer {
        id: saveStatusTimer
        interval: 3000
        onTriggered: saveStatus.text = ""
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            text: qsTr("Configure NAS / SMB mounts the agent should manage. Edits write to <code>mounts.json</code> and the agent reloads automatically.")
            color: Theme.colors.textMuted
            font.pixelSize: 11
            wrapMode: Text.Wrap
            textFormat: Text.RichText
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            Label {
                text: qsTr("%1 mount(s) · agent %2")
                    .arg(dialog.configsObj.mounts ? dialog.configsObj.mounts.length : 0)
                    .arg(Mount.connected ? qsTr("connected") : qsTr("waiting"))
                color: Theme.colors.textMuted
                font.pixelSize: Theme.font.sizeSmall
                Layout.fillWidth: true
            }
            FlatButton {
                Layout.preferredHeight: Theme.dim.toolStripHeight
                iconName: "plus"
                text: qsTr("Add Mount")
                variant: "primary"
                onClicked: editor.openForAdd()
            }
        }

        Label {
            id: saveStatus
            text: ""
            color: text.indexOf("failed") >= 0 ? Theme.colors.error : Theme.colors.success
            font.pixelSize: 11
            visible: text.length > 0
            Layout.fillWidth: true
        }

        // ── Sync Cache ──────────────────────────────────────────────
        // Global cache location for sync mounts. Only shown when a
        // sync mount exists (or a custom root was set) — with sync off
        // everywhere this whole section is dormant plumbing.
        Rectangle {
            visible: dialog._anySyncEnabled() || !!dialog.configsObj.syncCacheRoot
            Layout.fillWidth: true
            color: Theme.colors.bg
            border.color: Theme.colors.border
            border.width: Theme.dim.border
            radius: Theme.dim.radius
            implicitHeight: cacheCol.implicitHeight + 16

            ColumnLayout {
                id: cacheCol
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Sync Cache Location")
                        font.bold: true
                        font.pixelSize: Theme.font.sizeSmall
                        Layout.fillWidth: true
                    }
                    Label {
                        text: dialog.configsObj.syncCacheRoot
                            ? qsTr("custom")
                            : qsTr("default")
                        color: Theme.colors.textMuted
                        font.pixelSize: 10
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.configsObj.syncCacheRoot
                        || dialog._defaultCacheRoot()
                    color: Theme.colors.text
                    font.family: "Consolas, monospace"
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Cached blocks live here. Changing the path tears down active sync mounts and re-hydrates at the new location.")
                    color: Theme.colors.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    FlatButton {
                        Layout.preferredHeight: Theme.dim.toolStripHeight
                        iconName: "folder-open"
                        text: qsTr("Reveal")
                        tooltip: qsTr("Open the current cache folder in Explorer / Finder. Creates the folder if the agent hasn't materialised it yet.")
                        onClicked: {
                            var p = dialog.configsObj.syncCacheRoot
                                || dialog._defaultCacheRoot()
                            if (!p || p.length === 0) return
                            // The agent only creates the cache folder
                            // on first hydration of a sync mount, so on
                            // a fresh setup the path doesn't exist yet
                            // and reveal would fail. mkdir -p is
                            // idempotent and matches what the agent
                            // would do anyway.
                            if (!Paths.path_exists(p)) {
                                FileOps.create_directory(p)
                            }
                            FileOps.reveal_in_file_manager(p)
                        }
                    }
                    FlatButton {
                        Layout.preferredHeight: Theme.dim.toolStripHeight
                        iconName: "folders"
                        text: qsTr("Browse…")
                        tooltip: qsTr("Pick a different folder for the sync cache")
                        onClicked: cacheFolderPicker.open()
                    }
                    FlatButton {
                        Layout.preferredHeight: Theme.dim.toolStripHeight
                        iconName: "arrow-counter-clockwise"
                        text: qsTr("Reset")
                        tooltip: qsTr("Clear the override and use the default cache location")
                        enabled: !!dialog.configsObj.syncCacheRoot
                        onClicked: {
                            var copy = Object.assign({ version: 1, mounts: [] },
                                dialog.configsObj)
                            copy.syncCacheRoot = null
                            dialog.configsObj = copy
                            dialog._saveAll()
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }

        FolderDialog {
            id: cacheFolderPicker
            title: qsTr("Pick sync cache folder")
            onAccepted: {
                var p = dialog._urlToLocalPath(selectedFolder)
                if (!p || p.length === 0) return
                var copy = Object.assign({ version: 1, mounts: [] },
                    dialog.configsObj)
                copy.syncCacheRoot = p
                dialog.configsObj = copy
                dialog._saveAll()
            }
        }

        // ── Mount list ───────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.bg
            border.color: Theme.colors.border
            border.width: Theme.dim.border
            radius: Theme.dim.radius

            ListView {
                id: mountList
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: dialog.configsObj.mounts || []
                spacing: 0
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    width: ListView.view ? ListView.view.width : 0
                    height: 56
                    color: index % 2 === 0 ? Theme.colors.surface : Theme.colors.surfaceAlt

                    readonly property var liveState: dialog.statesByMount[modelData.id]
                    readonly property string stateName: liveState
                        ? String(liveState.state || "")
                        : qsTr("unknown")
                    // Slice C: derive per-row busy state from
                    // `Mount.pending_commands_json` so action buttons
                    // can render a spinner while the agent processes
                    // a Start/Stop/Restart issued from this row.
                    readonly property bool busy: {
                        try {
                            const arr = JSON.parse(Mount.pending_commands_json)
                            for (var i = 0; i < arr.length; ++i) {
                                if (arr[i].mountId === modelData.id) return true
                            }
                        } catch (e) {}
                        return false
                    }
                    readonly property var lastError: {
                        try {
                            const m = JSON.parse(Mount.last_command_error_json)
                            return m[modelData.id] || null
                        } catch (e) { return null }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        // State dot. Colour-only indicator; hover
                        // tooltip carries the readable state name so
                        // the row stays compact.
                        Rectangle {
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                            Layout.alignment: Qt.AlignVCenter
                            radius: 6
                            color: modelData.unmanaged === true
                                ? Theme.colors.accent
                                : dialog._stateColor(parent.parent.stateName)
                            border.color: Theme.colors.border
                            border.width: 1
                            ToolTip.text: modelData.unmanaged === true
                                ? qsTr("bookmark")
                                : parent.parent.stateName
                            ToolTip.visible: dotMa.containsMouse
                            ToolTip.delay: 300
                            MouseArea {
                                id: dotMa
                                anchors.fill: parent
                                hoverEnabled: true
                            }
                        }
                        // Display name + path.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Label {
                                text: (modelData.enabled === false ? "🚫 " : "")
                                    + (modelData.displayName || modelData.id || "?")
                                color: Theme.colors.text
                                font.pixelSize: 12
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Label {
                                text: modelData.nasSharePath || ""
                                color: Theme.colors.textSubtle
                                font.pixelSize: 10
                                font.family: "Consolas"
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }

                        // Actions - flat row, accent for primary,
                        // muted for destructive (use danger variant
                        // only on the confirm dialog).
                        // Slice C: a small busy indicator next to the
                        // action row. Visible while any of this
                        // mount's lifecycle commands is in flight;
                        // disappears on Ack/Error/timeout.
                        BusyIndicator {
                            running: parent.parent.busy
                            visible: parent.parent.busy
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            Layout.alignment: Qt.AlignVCenter
                        }
                        // Single connect/disconnect toggle — the sidebar
                        // context menu and the tray cover the same
                        // lifecycle, so the modal keeps just the
                        // essentials (plans/17 simplification pass).
                        // Unmanaged ("bookmark only") rows have no
                        // lifecycle at all.
                        FlatButton {
                            visible: modelData.unmanaged !== true
                            readonly property bool up: {
                                var s = String(parent.parent.stateName || "").toLowerCase()
                                return s === "mounted" || s === "mounting"
                            }
                            iconName: up ? "stop" : "play"
                            tooltip: up
                                ? qsTr("Disconnect")
                                : (parent.parent.lastError
                                    ? qsTr("Connect (last error: %1)").arg(parent.parent.lastError.message)
                                    : qsTr("Connect"))
                            enabled: !parent.parent.busy
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: up
                                ? Mount.stop_mount(modelData.id)
                                : Mount.start_mount(modelData.id)
                        }
                        FlatButton {
                            visible: modelData.unmanaged !== true
                            iconName: "arrow-clockwise"
                            tooltip: qsTr("Restart")
                            enabled: !parent.parent.busy
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: Mount.restart_mount(modelData.id)
                        }
                        FlatButton {
                            iconName: "pencil-simple"
                            tooltip: qsTr("Edit")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: editor.openForEdit(modelData, index)
                        }
                        FlatButton {
                            iconName: "trash"
                            tooltip: qsTr("Delete")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                deleteConfirm.pendingIndex = index
                                deleteConfirm.pendingName = modelData.displayName || modelData.id
                                deleteConfirm.open()
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: !dialog.configsObj.mounts || dialog.configsObj.mounts.length === 0
                    text: qsTr("(no mounts configured — click \"+ Add Mount\")")
                    color: Theme.colors.textSubtle
                    font.pixelSize: 11
                    font.italic: true
                }
            }
        }
    }

    // ── Add / Edit mount dialog ─────────────────────────────────────
    Dialog {
        id: editor
        property string mode: "add"
        property int editIndex: -1
        title: mode === "edit"
            ? qsTr("Edit Mount")
            : qsTr("Add Mount")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        // Wide enough for the 4-checkbox row (Enabled / Is jobs folder /
        // On-demand sync / Bookmark only) — at 540 the last one clipped.
        width: 640
        standardButtons: Dialog.Save | Dialog.Cancel

        /// Derive a mount id from the share path: leaf component,
        /// lowercased, non-url-ish chars folded to '-', deduped
        /// against existing ids with a numeric suffix. Users never
        /// see or type this — it's config plumbing.
        function _deriveId(unc) {
            var leaf = String(unc || "").replace(/[\\\/]+$/, "")
            var idx = Math.max(leaf.lastIndexOf("\\"), leaf.lastIndexOf("/"))
            leaf = (idx >= 0 ? leaf.substring(idx + 1) : leaf)
                .toLowerCase().replace(/[^a-z0-9._-]+/g, "-")
                .replace(/^-+|-+$/g, "")
            if (leaf.length === 0) return ""
            var taken = {}
            var ms = dialog.configsObj.mounts || []
            for (var i = 0; i < ms.length; ++i) {
                if (ms[i] && ms[i].id) taken[ms[i].id] = true
            }
            if (!taken[leaf]) return leaf
            for (var n = 2; n < 100; ++n) {
                if (!taken[leaf + "-" + n]) return leaf + "-" + n
            }
            return leaf + "-" + Date.now()
        }

        function openForAdd() {
            mode = "add"
            editIndex = -1
            editingId = ""
            displayNameField.text = ""
            nasPathField.text = ""
            enabledBox.checked = true
            isJobsBox.checked = true
            // On-demand sync defaults OFF for new mounts: plain SMB is
            // the production mode (plans/17); sync opts in explicitly.
            syncEnabledBox.checked = false
            unmanagedBox.checked = false
            _loadedDriveLetter = ""
            _refreshUsedLetters()
            letterCombo.currentIndex = 0
            open()
            nasPathField.forceActiveFocus()
        }
        function openForEdit(cfg, idx) {
            mode = "edit"
            editIndex = idx
            editingId = cfg.id || ""
            displayNameField.text = cfg.displayName || ""
            nasPathField.text = cfg.nasSharePath || ""
            enabledBox.checked = cfg.enabled !== false
            isJobsBox.checked = cfg.isJobsFolder !== false
            syncEnabledBox.checked = cfg.syncEnabled === true
            unmanagedBox.checked = cfg.unmanaged === true
            _loadedDriveLetter = (cfg.mountDriveLetter || "").trim().charAt(0).toUpperCase()
            _refreshUsedLetters()
            letterCombo.currentIndex = _loadedDriveLetter.length > 0
                ? Math.max(0, letterCombo.model.indexOf(_loadedDriveLetter + ":"))
                : 0
            // Ask the agent for fresh cache stats for this mount.
            // Agent replies asynchronously; the binding below picks
            // up the result on landing.
            if (editingId.length > 0) {
                Mount.request_cache_stats(editingId)
            }
            open()
        }

        /// Mount ID currently being edited. Used by the cache-stats
        /// line below to pick the right entry out of the global
        /// Mount.cache_stats_json map.
        property string editingId: ""

        /// The mount's configured drive letter as loaded ("" = auto).
        /// Its own letter stays selectable in the picker even though
        /// it reads as in-use on this machine.
        property string _loadedDriveLetter: ""

        /// Letters occupied on this machine (local volumes + network
        /// mappings), refreshed each time the editor opens.
        property var _usedLetters: []
        function _refreshUsedLetters() {
            try {
                _usedLetters = JSON.parse(Mount.used_drive_letters())
            } catch (e) {
                _usedLetters = []
            }
        }

        /// Resolved cache-stats snapshot for `editingId`, or null if
        /// the agent hasn't responded yet. Re-evaluates whenever the
        /// global `Mount.cache_stats_json` property changes.
        readonly property var editingCacheStats: {
            if (mode !== "edit" || editingId.length === 0) return null
            try {
                var all = JSON.parse(Mount.cache_stats_json)
                return all[editingId] || null
            } catch (e) {
                return null
            }
        }

        function _formatBytes(n) {
            if (!n || n <= 0) return "0 B"
            var units = ["B", "KB", "MB", "GB", "TB"]
            var i = 0
            var v = n
            while (v >= 1024 && i < units.length - 1) {
                v /= 1024
                i++
            }
            return (i === 0 ? v : v.toFixed(v < 10 ? 1 : 0)) + " " + units[i]
        }

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 6

            // Cache stats — edit mode AND sync-enabled only: without
            // on-demand sync there is no cache, and the row would
            // forever read "0 B" plumbing noise.
            Rectangle {
                visible: editor.mode === "edit" && syncEnabledBox.checked
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: Theme.colors.surface
                border.color: Theme.colors.border
                border.width: Theme.dim.border
                radius: Theme.dim.radius

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8

                    Icon {
                        name: "database"
                        size: Theme.icon.sizeSmall
                        color: Theme.colors.textMuted
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Label {
                        text: {
                            var s = editor.editingCacheStats
                            if (!s) return qsTr("Cache: …")
                            return qsTr("Cache: %1 · %2 file(s)")
                                .arg(editor._formatBytes(s.hydratedBytes || 0))
                                .arg((s.hydratedCount || 0).toLocaleString(Qt.locale(), "f", 0))
                        }
                        color: Theme.colors.text
                        font.pixelSize: Theme.font.sizeSmall
                        Layout.alignment: Qt.AlignVCenter
                        Layout.fillWidth: true
                    }
                    FlatButton {
                        text: qsTr("Drain…")
                        iconName: "trash"
                        Layout.preferredHeight: 24
                        padding: 6
                        onClicked: {
                            // Defer the actual drain to a separate
                            // confirm dialog so the user can back
                            // out. Close the editor first - stacking
                            // two modals fights focus on Windows.
                            var id = editor.editingId
                            editor.close()
                            drainConfirm.pendingId = id
                            drainConfirm.open()
                        }
                    }
                    FlatButton {
                        iconName: "arrow-clockwise"
                        tooltip: qsTr("Refresh stats")
                        Layout.preferredHeight: 24
                        padding: 6
                        onClicked: {
                            if (editor.editingId.length > 0) {
                                Mount.request_cache_stats(editor.editingId)
                            }
                        }
                    }
                }
            }

            GridLayout {
                columns: 2
                columnSpacing: 8
                rowSpacing: 4
                Layout.fillWidth: true

                // Mount ID is plumbing, not a user choice: derived from
                // the share leaf on Add (deduped), immutable on Edit.
                // The old "stable identifier" TextField is gone
                // (plans/17 simplification pass); hand-edited
                // mounts.json ids still round-trip untouched.

                Label {
                    text: unmanagedBox.checked ? qsTr("Path") : qsTr("NAS share path")
                    color: Theme.colors.text
                }
                TextField {
                    id: nasPathField
                    Layout.fillWidth: true
                    placeholderText: unmanagedBox.checked
                        ? (Qt.platform.os === "osx" ? "/Volumes/SomeVolume or /Users/you/Projects" : "D:\\Projects")
                        : "\\\\nas\\share"
                    font.family: "Consolas"
                    font.pixelSize: 11
                }

                Label { text: qsTr("Display name"); color: Theme.colors.text }
                TextField {
                    id: displayNameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Friendly label for sidebar (optional)")
                }

                // Drive letter (Windows): facilities standardize letters
                // across seats so external apps' saved paths (Premiere/
                // AE/Flame projects) match on every machine. Automatic
                // = first free letter Z:→D:, persisted on first mount
                // so it stays stable per machine thereafter. In-use
                // letters render disabled; the mount's own current
                // letter stays selectable. The letter is a preference,
                // not a hard requirement — if it's taken at mount time
                // the mount lands on a free letter with a notice.
                Label {
                    visible: Qt.platform.os === "windows" && !unmanagedBox.checked
                    text: qsTr("Drive letter")
                    color: Theme.colors.text
                }
                ComboBox {
                    id: letterCombo
                    visible: Qt.platform.os === "windows" && !unmanagedBox.checked
                    Layout.preferredWidth: 140
                    model: {
                        var items = [qsTr("Automatic")]
                        for (var c = "Z".charCodeAt(0); c >= "D".charCodeAt(0); --c) {
                            items.push(String.fromCharCode(c) + ":")
                        }
                        return items
                    }
                    delegate: ItemDelegate {
                        required property int index
                        required property var modelData
                        width: letterCombo.width
                        text: modelData
                        enabled: {
                            if (index === 0) return true
                            var letter = modelData[0]
                            if (letter === editor._loadedDriveLetter) return true
                            return editor._usedLetters.indexOf(letter) === -1
                        }
                        highlighted: letterCombo.highlightedIndex === index
                    }
                }

                // The credential key is derived from the mount id and
                // never shown in the UI. It used to be a visible field
                // - users had to invent a string with no UX cues for
                // why - so the Authentication block below now owns
                // that responsibility entirely. Existing mounts.json
                // values are preserved on round-trip via _loadedCredKey.
                //
                // Drive letter / SMB mount path / sync root path
                // were here previously. The agent derives these
                // automatically (volumes_base + mount id), so they
                // weren't real user choices - dropped from the form.
                // Existing values in mounts.json are preserved on
                // round-trip via Object.assign(existing, ...) below,
                // so legacy configs aren't disturbed.
            }

            // Authentication is not a UFB concept (plans/17 decision 4,
            // completed for Windows in slice B): connecting IS the
            // sign-in on both OSes. Any user-initiated Connect/Restart
            // permits the OS auth dialog (NetAuthAgent + "Remember in
            // my keychain" on macOS; the standard Windows credential
            // dialog with "Remember my credentials" on Windows), which
            // appears only when the server rejects stored credentials.

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                CheckBox { id: enabledBox; text: qsTr("Enabled") }
                CheckBox { id: isJobsBox; text: qsTr("Is jobs folder") }
                CheckBox {
                    id: syncEnabledBox
                    text: qsTr("On-demand sync")
                    // A bookmark has nothing to sync.
                    enabled: !unmanagedBox.checked
                }
                CheckBox {
                    id: unmanagedBox
                    text: qsTr("Bookmark only")
                    ToolTip.text: qsTr("List this path under Mounts without UFB mounting or monitoring it — for local folders or volumes another tool manages.")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onCheckedChanged: if (checked) syncEnabledBox.checked = false
                }
            }
        }

        onAccepted: {
            // ID: immutable on edit, derived from the share leaf on add.
            var name = (editor.mode === "edit" && editor.editingId.length > 0)
                ? editor.editingId
                : editor._deriveId(nasPathField.text)
            if (name.length === 0 || nasPathField.text.trim().length === 0) return
            // Build the config object — preserve existing fields when
            // editing so we don't drop legacy keys (probe intervals,
            // rclone flags, etc.). Add mode starts fresh.
            var existing = (editor.mode === "edit" && editor.editIndex >= 0
                && dialog.configsObj.mounts && dialog.configsObj.mounts[editor.editIndex])
                ? dialog.configsObj.mounts[editor.editIndex]
                : {}
            // Object.assign(existing, ...) preserves any field the
            // editor no longer surfaces (drive letter, smb mount
            // path, sync root path, legacy rclone/cache tuning,
            // legacy credentialKey) so round-tripping a hand-edited
            // mounts.json doesn't silently drop those entries.
            // credentialKey stopped being written for new mounts in
            // slice B — credentials are OS-owned, keyed by server.
            // Drive letter: "Automatic" writes "" (auto-pick, then the
            // first assignment persists itself); an explicit letter is
            // the cross-machine standardization knob. Non-Windows
            // preserves whatever the config had (the picker is hidden
            // and letters mean nothing there).
            var chosenLetter = Qt.platform.os === "windows"
                ? (letterCombo.currentIndex > 0
                    ? String(letterCombo.model[letterCombo.currentIndex])[0]
                    : "")
                : (existing.mountDriveLetter || "")
            var cfg = Object.assign({}, existing, {
                id: name,
                displayName: displayNameField.text.trim(),
                nasSharePath: nasPathField.text.trim(),
                enabled: enabledBox.checked,
                isJobsFolder: isJobsBox.checked,
                syncEnabled: syncEnabledBox.checked,
                unmanaged: unmanagedBox.checked,
                mountDriveLetter: chosenLetter
            })
            // Splice into the configs object.
            var copy = Object.assign({ version: 1, mounts: [] }, dialog.configsObj)
            copy.mounts = (copy.mounts || []).slice()
            if (editor.mode === "edit" && editor.editIndex >= 0) {
                copy.mounts[editor.editIndex] = cfg
            } else {
                copy.mounts.push(cfg)
            }
            dialog.configsObj = copy
            dialog._saveAll()
            dialog._refresh()
            // No credential prompt here: the mount's first Start
            // attempts NULL credentials (Credential Manager / Keychain
            // supplies them) and a rejection surfaces the auth_error
            // pill, whose Connect action permits the OS dialog.
        }
    }

    // ── Delete confirm ──────────────────────────────────────────────
    Dialog {
        id: deleteConfirm
        property int pendingIndex: -1
        property string pendingName: ""
        title: qsTr("Remove Mount")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        width: 380
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            text: qsTr("Remove mount “%1” from mounts.json?\n\nThe agent will unmount it. Stored credentials in the OS keychain are kept untouched.").arg(deleteConfirm.pendingName)
            color: Theme.colors.text
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        onAccepted: {
            if (pendingIndex < 0 || !dialog.configsObj.mounts) return
            var copy = Object.assign({ version: 1, mounts: [] }, dialog.configsObj)
            copy.mounts = (copy.mounts || []).slice()
            copy.mounts.splice(pendingIndex, 1)
            dialog.configsObj = copy
            dialog._saveAll()
            dialog._refresh()
        }
    }

    // ── Drain confirm ───────────────────────────────────────────────
    // Drain is destructive (locally) - hydrated files have to come
    // back over the network on next access. Confirm before firing
    // ClearSyncCache.
    Dialog {
        id: drainConfirm
        property string pendingId: ""
        title: qsTr("Drain Cache")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        width: 420
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            text: qsTr("Clear the local cache for “%1”?\n\nFiles that have been synced back to the source are kept; pending uploads are flushed. Subsequent reads will re-fetch over the network.")
                .arg(drainConfirm.pendingId)
            color: Theme.colors.text
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        onAccepted: {
            if (drainConfirm.pendingId.length === 0) return
            Mount.clear_sync_cache(drainConfirm.pendingId)
            // Re-poll so the editor's stats line reflects the drain
            // once the agent finishes.
            Mount.request_cache_stats(drainConfirm.pendingId)
            drainConfirm.pendingId = ""
        }
        onRejected: drainConfirm.pendingId = ""
    }
}
