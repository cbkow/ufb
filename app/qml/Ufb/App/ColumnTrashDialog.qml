// 0.9.97 — Trash dialog for column_definitions.
//
// Lists columns soft-trashed via `Columns.delete_column` (which now
// sends to Trash rather than hard-deleting). Each entry has Restore
// + Delete Forever actions:
//   - Restore calls `Columns.untrash_column(id, "")`. If the column's
//     name conflicts with an active row in the same folder, the
//     binding returns a `RenameRequired:` string; we surface a small
//     rename input for that row and retry on submit.
//   - Delete Forever calls `Columns.permanently_delete_column(id)`.
//     Terminal — wins LWW even against newer local edits on peers.
//
// Open this dialog from the Column Manager via its "View Trash"
// button. Operates on a single (job, folder) scope, matching how the
// Column Manager itself works.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Dialog {
    id: dialog

    property string jobPath: ""
    property string folderName: ""
    /// JSON array from `Columns.list_trashed_columns(...)`. Each entry:
    /// `{ id, columnName, columnType, trashedAt, templateHash }`.
    property var entries: []
    /// Per-row inline rename state. Map of `id` (string) -> input
    /// text. When non-empty, Restore retries with the user-supplied
    /// rename. When empty, Restore retries without a rename (which
    /// will still fail RenameRequired if the original name collides).
    property var _renameInput: ({})
    /// Per-row error string (populated when Restore returns
    /// RenameRequired). Map of `id` (string) -> error message.
    property var _restoreError: ({})

    title: qsTr("Trashed Columns — %1").arg(folderName)
    modal: true
    anchors.centerIn: parent
    standardButtons: Dialog.Close
    width: Math.min(parent.width * 0.8, 720)

    function refresh() {
        try {
            entries = JSON.parse(
                Columns.list_trashed_columns(jobPath, folderName))
        } catch (e) {
            entries = []
        }
        _renameInput = ({})
        _restoreError = ({})
    }

    function _setRename(id, value) {
        var copy = {}
        for (var k in _renameInput) copy[k] = _renameInput[k]
        copy[String(id)] = value
        _renameInput = copy
    }

    function _setError(id, value) {
        var copy = {}
        for (var k in _restoreError) copy[k] = _restoreError[k]
        copy[String(id)] = value
        _restoreError = copy
    }

    function _restore(entry) {
        var rename = _renameInput[String(entry.id)] || ""
        var result = Columns.untrash_column(entry.id, rename)
        if (result === "ok") {
            refresh()
        } else if (result.indexOf("RenameRequired") === 0) {
            _setError(entry.id, result)
        } else {
            _setError(entry.id, result)
        }
    }

    onAboutToShow: refresh()

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            visible: dialog.entries.length === 0
            text: qsTr("(trash is empty for this folder)")
            color: Theme.colors.textSubtle
            font.pixelSize: 11
            font.italic: true
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 24
            Layout.bottomMargin: 24
        }

        ScrollView {
            visible: dialog.entries.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 360
            clip: true

            ColumnLayout {
                width: parent ? parent.width : 600
                spacing: 4

                Repeater {
                    model: dialog.entries

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: hasError ? 64 : 36
                        color: index % 2 === 0
                            ? Theme.colors.surface
                            : Theme.colors.toolbarAlt
                        radius: 4

                        readonly property bool hasError:
                            (dialog._restoreError[String(modelData.id)] || "").length > 0

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Icon {
                                    name: "trash"
                                    size: 14
                                    color: Theme.colors.textMuted
                                }
                                Label {
                                    text: modelData.columnName
                                    color: Theme.colors.text
                                    font.pixelSize: 12
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: modelData.columnType
                                    color: Theme.colors.textMuted
                                    font.pixelSize: 11
                                    Layout.preferredWidth: 80
                                }
                                TextField {
                                    visible: parent.parent.parent.hasError
                                    placeholderText: qsTr("New name")
                                    Layout.preferredWidth: 140
                                    text: dialog._renameInput[String(modelData.id)] || ""
                                    onTextChanged: dialog._setRename(modelData.id, text)
                                    onAccepted: dialog._restore(modelData)
                                }
                                FlatButton {
                                    iconName: "arrow-counter-clockwise"
                                    text: qsTr("Restore")
                                    Layout.preferredHeight: Theme.dim.toolStripHeight
                                    onClicked: dialog._restore(modelData)
                                }
                                FlatButton {
                                    iconName: "x"
                                    tooltip: qsTr("Delete Forever — terminal, replicates as a permanent delete to all peers.")
                                    Layout.preferredHeight: Theme.dim.toolStripHeight
                                    onClicked: {
                                        if (Columns.permanently_delete_column(modelData.id)) {
                                            dialog.refresh()
                                        }
                                    }
                                }
                            }

                            Label {
                                visible: parent.parent.hasError
                                text: dialog._restoreError[String(modelData.id)] || ""
                                color: Theme.colors.error
                                font.pixelSize: 10
                                Layout.fillWidth: true
                                Layout.leftMargin: 22
                                wrapMode: Text.Wrap
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }
    }
}
