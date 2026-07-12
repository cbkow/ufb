// BackupManagerDialog — per-job snapshot history.
//
// Lists every snapshot stored under <jobPath>/.ufb/backups,
// surfaces a "Backup Now" button that captures the current state
// (column defs + tracked + non-tracked metadata for the job), and
// per-row "Restore" with a confirmation dialog.
//
// Restore is a MERGE not a wipe — rows present locally but missing
// from the snapshot are left in place. That matches what the legacy
// app did and avoids the user accidentally nuking edits made since
// the snapshot.
//
// Hosted by JobView's header button.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Dialog {
    id: dialog

    /// Required.
    property string jobPath: ""
    property string jobName: ""

    title: qsTr("Backups — %1").arg(jobName || qsTr("(no job)"))
    modal: true
    parent: Overlay.overlay
    x: (parent ? (parent.width  - width)  / 2 : 0)
    y: (parent ? (parent.height - height) / 2 : 0)
    width: 560
    height: 480
    standardButtons: Dialog.Close

    /// Last-fetched list of {timestamp, filename, createdBy,
    /// shotCount, checksum, uncompressedSize, date}.
    property var backups: []

    function refresh() {
        if (jobPath.length === 0) { backups = []; return }
        try {
            backups = JSON.parse(Backup.list_backups(jobPath))
        } catch (e) {
            console.warn("BackupManagerDialog: list_backups parse failed:", e)
            backups = []
        }
        // Newest first.
        backups.sort(function(a, b) {
            return (b.timestamp || 0) - (a.timestamp || 0)
        })
    }

    onAboutToShow: refresh()

    function _formatBytes(n) {
        if (!n) return "0 B"
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB"
        return (n / (1024 * 1024)).toFixed(2) + " MB"
    }
    function _formatDate(ms) {
        if (!ms) return ""
        var d = new Date(ms)
        return d.toLocaleString(Qt.locale(), "yyyy-MM-dd hh:mm:ss")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            text: qsTr("Snapshots are stored locally inside the job folder under <code>.ufb/backups/</code>. They are not mesh-synced — each device keeps its own history.")
            color: Theme.colors.textMuted
            font.pixelSize: 11
            wrapMode: Text.Wrap
            textFormat: Text.RichText
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label {
                text: qsTr("%1 snapshot(s)").arg(dialog.backups.length)
                color: Theme.colors.textMuted
                font.pixelSize: 11
                Layout.fillWidth: true
            }
            FlatButton {
                Layout.preferredHeight: Theme.dim.toolStripHeight
                iconName: "camera"
                text: qsTr("Backup Now")
                variant: "primary"
                enabled_: dialog.jobPath.length > 0
                tooltip: qsTr("Capture the current columns + tracked items into a new snapshot")
                onClicked: {
                    var infoJson = Backup.create_backup(dialog.jobPath, dialog.jobName)
                    if (infoJson.length === 0) {
                        backupStatus.text = qsTr("Backup failed — see app log")
                    } else {
                        backupStatus.text = qsTr("Snapshot saved")
                    }
                    dialog.refresh()
                }
            }
        }

        Label {
            id: backupStatus
            text: ""
            color: text.indexOf("failed") >= 0 ? Theme.colors.error : Theme.colors.success
            font.pixelSize: 11
            Layout.fillWidth: true
            visible: text.length > 0
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.bg
            border.color: Theme.colors.border
            border.width: 1
            radius: 2

            ListView {
                id: backupList
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: dialog.backups
                spacing: 0
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    width: backupList.width
                    height: 36
                    color: index % 2 === 0 ? Theme.colors.surface : Theme.colors.surfaceAlt
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        Label {
                            text: "📦"
                            color: Theme.colors.accent
                            font.pixelSize: 14
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Label {
                                text: modelData
                                    ? dialog._formatDate(modelData.timestamp)
                                    : ""
                                color: Theme.colors.text
                                font.pixelSize: 12
                                font.family: "Consolas"
                            }
                            Label {
                                text: modelData
                                    ? qsTr("%1 · %2 · by %3")
                                        .arg(modelData.filename)
                                        .arg(dialog._formatBytes(modelData.uncompressedSize))
                                        .arg(modelData.createdBy || "?")
                                    : ""
                                color: Theme.colors.textSubtle
                                font.pixelSize: 10
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                        FlatButton {
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            iconName: "arrow-counter-clockwise"
                            text: qsTr("Restore")
                            tooltip: qsTr("Merge this snapshot back into the local DB. Existing rows not in the snapshot are kept.")
                            onClicked: {
                                restoreConfirm.pendingBackup = modelData
                                restoreConfirm.open()
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: dialog.backups.length === 0
                    text: qsTr("(no snapshots yet — click \"Backup Now\" to capture one)")
                    color: Theme.colors.textSubtle
                    font.pixelSize: 11
                    font.italic: true
                }
            }
        }
    }

    // ── Restore confirmation ─────────────────────────────────────────
    Dialog {
        id: restoreConfirm
        property var pendingBackup: null
        title: qsTr("Restore Snapshot")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        width: 420
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            text: restoreConfirm.pendingBackup
                ? qsTr("Merge snapshot from %1 into the current job?\n\nColumns and item metadata in the snapshot will be re-applied. Local rows not in the snapshot will be kept.")
                    .arg(dialog._formatDate(restoreConfirm.pendingBackup.timestamp))
                : ""
            color: Theme.colors.text
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        onAccepted: {
            if (!pendingBackup) return
            var err = Backup.restore_backup(dialog.jobPath, pendingBackup.filename)
            if (err.length > 0) {
                backupStatus.text = qsTr("Restore failed: %1").arg(err)
            } else {
                backupStatus.text = qsTr("Restore complete")
                // Force-refresh tracked items + columns so observers
                // see the freshly merged data.
                Subscription.refresh_tracked()
            }
        }
    }
}
