// MeshSettingsDialog — enable/disable + farm path + node ID + peer
// list for the mesh sync subsystem. Opened from the Sidebar's mesh
// footer gear button.
//
// Save flow: Settings.set_mesh writes to settings.json; Mesh.reinit
// rebuilds the manager with the new config and applies the enabled
// flag. Mesh.refresh_status repopulates the peer list shown below.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Dialog {
    id: dialog

    title: qsTr("Mesh Sync")
    modal: true
    parent: Overlay.overlay
    x: (parent ? (parent.width  - width)  / 2 : 0)
    y: (parent ? (parent.height - height) / 2 : 0)
    width: 600
    // 520 was tight before the Tags row landed; bumped so the
    // peer list at the bottom keeps at least a few rows of breathing
    // room without forcing its inner scrollbar.
    height: 600
    standardButtons: Dialog.Save | Dialog.Close

    /// Fired after a successful save / reinit so the host can re-pull
    /// the mesh status display.
    signal changed()

    /// Live mirror of Mesh.peers_json + status fields shown below.
    property var status: ({})
    property var peers: []
    // Latest degraded-sync warning ({kind, detail, ts}) pushed by the
    // core layer via Mesh.last_warning_json. Surfaced below so silent
    // failures (dropped edits, etc.) are visible, not log-only.
    property var lastWarning: ({})

    function openFor() {
        // Seed the form from current settings + status.
        enabledBox.checked = Settings.mesh_enabled()
        farmPathField.text = Settings.mesh_farm_path()
        nodeIdField.text = Settings.mesh_node_id()
        tagsField.text = Settings.mesh_tags()
        _refresh()
        open()
    }
    function _refresh() {
        Mesh.refresh_status()
        try { status = JSON.parse(Mesh.status_json) } catch (e) { status = {} }
        try { peers  = JSON.parse(Mesh.peers_json)  } catch (e) { peers  = [] }
    }

    Connections {
        target: Mesh
        function onStatus_jsonChanged() {
            try { dialog.status = JSON.parse(Mesh.status_json) } catch (e) {}
        }
        function onPeers_jsonChanged() {
            try { dialog.peers = JSON.parse(Mesh.peers_json) } catch (e) {}
        }
        function onWarning_revChanged() {
            try { dialog.lastWarning = JSON.parse(Mesh.last_warning_json) } catch (e) {}
        }
    }

    function _formatTime(ms) {
        if (!ms) return qsTr("never")
        return new Date(ms).toLocaleString(Qt.locale(), "yyyy-MM-dd hh:mm:ss")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            text: qsTr("Mesh sync replicates metadata, columns, and presets across nodes that share a farm path. Each node writes its endpoint + snapshots into <code>&lt;farm path&gt;/nodes/&lt;node id&gt;/</code>; the leader writes a periodic snapshot the others restore.")
            color: Theme.colors.textMuted
            font.pixelSize: 11
            wrapMode: Text.Wrap
            textFormat: Text.RichText
            Layout.fillWidth: true
        }

        // ── Settings form ────────────────────────────────────────────
        GridLayout {
            columns: 2
            columnSpacing: 8
            rowSpacing: 6
            Layout.fillWidth: true

            Label { text: qsTr("Enabled"); color: Theme.colors.text }
            CheckBox {
                id: enabledBox
                text: qsTr("Run mesh sync at app launch")
            }

            Label { text: qsTr("Farm path"); color: Theme.colors.text }
            TextField {
                id: farmPathField
                Layout.fillWidth: true
                placeholderText: "\\\\nas\\jobs\\.ufb-mesh"
                font.family: "Consolas"
                font.pixelSize: 11
            }

            Label { text: qsTr("Node ID"); color: Theme.colors.text }
            TextField {
                id: nodeIdField
                Layout.fillWidth: true
                placeholderText: qsTr("Unique per machine, e.g. workstation-04")
                font.family: "Consolas"
                font.pixelSize: 11
            }

            Label { text: qsTr("Tags"); color: Theme.colors.text }
            TextField {
                id: tagsField
                Layout.fillWidth: true
                // Comma-separated free-form labels. Surfaces in the
                // peer list (right column) and on heartbeat broadcasts
                // so other nodes can filter/target by role. Common
                // values: "render", "gpu", "client", "nyc".
                placeholderText: qsTr("Comma-separated, e.g. render, gpu")
                font.family: "Consolas"
                font.pixelSize: 11
            }
        }

        Label {
            id: saveStatus
            text: ""
            color: text.indexOf("error") >= 0 || text.indexOf("failed") >= 0
                ? "#c04040" : "#7ed321"
            font.pixelSize: 11
            visible: text.length > 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }

        // ── Status block ─────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: Theme.colors.surface
            border.color: Theme.colors.border
            border.width: 1
            radius: 2
            GridLayout {
                anchors.fill: parent
                anchors.margins: 8
                columns: 4
                rowSpacing: 2
                columnSpacing: 12
                Label { text: qsTr("Status"); color: Theme.colors.textSubtle; font.pixelSize: 10 }
                Label {
                    text: dialog.status.statusMessage || ""
                    color: Theme.colors.text; font.pixelSize: 11; font.bold: true
                    Layout.fillWidth: true
                }
                Label { text: qsTr("Leader"); color: Theme.colors.textSubtle; font.pixelSize: 10 }
                Label {
                    text: dialog.status.leaderId || qsTr("(none)")
                    color: dialog.status.isLeader ? "#7ed321" : "#dddddd"
                    font.pixelSize: 11; font.family: "Consolas"
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label { text: qsTr("Peers"); color: Theme.colors.textSubtle; font.pixelSize: 10 }
                Label {
                    text: String(dialog.status.peerCount || 0)
                    color: Theme.colors.text; font.pixelSize: 11
                }
                Label { text: qsTr("Last snapshot"); color: Theme.colors.textSubtle; font.pixelSize: 10 }
                Label {
                    text: dialog._formatTime(dialog.status.lastSnapshotTime)
                    color: Theme.colors.text; font.pixelSize: 11; font.family: "Consolas"
                }
                // Degraded-sync warning — only shown when one has fired.
                // Surfaces dropped edits / outbox overflows / template
                // fetch failures that were previously log-only.
                Label {
                    text: qsTr("Last warning")
                    color: Theme.colors.textSubtle; font.pixelSize: 10
                    visible: !!(dialog.lastWarning && dialog.lastWarning.detail)
                }
                Label {
                    visible: !!(dialog.lastWarning && dialog.lastWarning.detail)
                    text: (dialog.lastWarning && dialog.lastWarning.detail
                            ? dialog._formatTime(dialog.lastWarning.ts) + " — " + dialog.lastWarning.detail
                            : "")
                    color: Theme.colors.error || "#d0021b"
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            FlatButton {
                Layout.preferredHeight: Theme.dim.toolStripHeight
                iconName: "camera"
                text: qsTr("Trigger snapshot")
                enabled_: dialog.status.isLeader === true
                tooltip: enabled_
                    ? qsTr("Force-write a snapshot now")
                    : qsTr("Only the leader can write snapshots")
                onClicked: {
                    Mesh.trigger_snapshot()
                    saveStatus.text = qsTr("Snapshot queued")
                }
            }
            FlatButton {
                Layout.preferredHeight: Theme.dim.toolStripHeight
                iconName: "arrow-clockwise"
                text: qsTr("Refresh")
                onClicked: dialog._refresh()
            }
            Item { Layout.fillWidth: true }
        }

        // ── Peer list ───────────────────────────────────────────────
        Label {
            text: qsTr("PEERS")
            color: Theme.colors.textSubtle
            font.pixelSize: 10
            font.bold: true
            Layout.fillWidth: true
            Layout.topMargin: 4
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.surface
            border.color: Theme.colors.border
            border.width: 1
            radius: 2
            ListView {
                id: peerList
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: dialog.peers
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    width: peerList.width
                    height: 28
                    color: index % 2 === 0 ? "#1a1a1a" : "#1d1d1d"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8
                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            Layout.alignment: Qt.AlignVCenter
                            radius: 4
                            color: modelData.isAlive
                                ? (modelData.isLeader ? "#7ed321" : "#4a90e2")
                                : "#666"
                        }
                        Label {
                            text: modelData.nodeId || "?"
                            color: Theme.colors.text
                            font.pixelSize: 12
                            font.family: "Consolas"
                            font.bold: modelData.isLeader === true
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: modelData.isLeader ? qsTr("LEADER") : ""
                            color: Theme.colors.success
                            font.pixelSize: 10
                            font.bold: true
                        }
                        Label {
                            text: (modelData.tags || []).join(", ")
                            color: Theme.colors.textSubtle
                            font.pixelSize: 10
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: dialog.peers.length === 0
                    text: dialog.status.isEnabled
                        ? qsTr("(no peers seen yet — verify other nodes share the same farm path)")
                        : qsTr("(mesh disabled)")
                    color: Theme.colors.textSubtle
                    font.pixelSize: 11
                    font.italic: true
                }
            }
        }
    }

    onAccepted: {
        var err = Settings.set_mesh(
            enabledBox.checked,
            farmPathField.text.trim(),
            nodeIdField.text.trim(),
            tagsField.text.trim())
        if (err.length > 0) {
            saveStatus.text = qsTr("Save failed: %1").arg(err)
            open()
            return
        }
        // Rebuild the manager with the new config + apply the toggle.
        Mesh.reinit()
        // Re-apply enabled (reinit honors the saved enabled flag, but
        // call again so the status refresh below sees the new state).
        var setErr = Mesh.set_enabled(enabledBox.checked)
        if (setErr.length > 0) {
            saveStatus.text = setErr
            open()
            return
        }
        saveStatus.text = qsTr("Mesh settings saved")
        dialog._refresh()
        dialog.changed()
    }
}
