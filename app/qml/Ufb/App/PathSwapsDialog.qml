// PathSwapsDialog — manage cross-OS path mappings.
//
// Each row maps a Windows prefix to its macOS counterpart. When a
// `ufb://` link is opened on a different OS than the one that
// produced it, the matching prefix is swapped so the link still
// opens the same logical location.
//
// Storage: settings.json `pathMappings` (read+written via Settings
// QObject). Loaded on `openFor()`, written back on Save.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Dialog {
    id: dialog

    title: qsTr("Path Swaps")
    modal: true
    parent: Overlay.overlay
    x: (parent ? (parent.width  - width)  / 2 : 0)
    y: (parent ? (parent.height - height) / 2 : 0)
    width: 720
    height: 520
    standardButtons: Dialog.Save | Dialog.Cancel

    /// Local mutable copy. Save writes to Settings; Cancel discards.
    /// Reassign to a new array to trigger QML reactivity (in-place
    /// push/splice doesn't notify bindings on `var` properties).
    property var mappings: []

    signal changed()

    function openFor() {
        try {
            mappings = JSON.parse(Settings.path_mappings_json())
        } catch (e) {
            mappings = []
        }
        open()
    }

    function _addRow() {
        // QML doesn't observe in-place mutations of `var` arrays, so
        // replace the whole reference. Without this the ListView's
        // `model: mappings.length` binding doesn't re-evaluate and
        // the new row never appears.
        mappings = mappings.concat([{ enabled: true, label: "", win: "", mac: "" }])
    }
    function _removeRow(idx) {
        if (idx < 0 || idx >= mappings.length) return
        var next = mappings.slice(0, idx).concat(mappings.slice(idx + 1))
        mappings = next
    }
    function _setField(idx, key, val) {
        if (idx < 0 || idx >= mappings.length) return
        // Mutate in-place: rebinding the whole array on each
        // keystroke steals TextField focus.
        mappings[idx][key] = val
    }

    onAccepted: {
        var json = JSON.stringify(dialog.mappings)
        var err = Settings.set_path_mappings(json)
        if (err.length > 0) {
            errLabel.text = qsTr("Save failed: %1").arg(err)
            errLabel.visible = true
            return
        }
        dialog.changed()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            text: qsTr("Map equivalent paths between Windows and macOS so " +
                "<code>ufb://</code> links produced on one platform open the " +
                "right local folder on another.")
            color: Theme.colors.textMuted
            font.pixelSize: Theme.font.sizeSmall
            wrapMode: Text.Wrap
            textFormat: Text.RichText
            Layout.fillWidth: true
        }

        // ── Header row ──────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.headerHeight
            color: Theme.colors.toolbar
            border.color: Theme.colors.border
            border.width: Theme.dim.border
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8
                Label {
                    text: qsTr("On")
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                    font.bold: true
                    Layout.preferredWidth: 30
                }
                Label {
                    text: qsTr("Label")
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                    font.bold: true
                    Layout.preferredWidth: 130
                }
                Label {
                    text: qsTr("Windows path")
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                    font.bold: true
                    Layout.fillWidth: true
                }
                Label {
                    text: qsTr("macOS path")
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                    font.bold: true
                    Layout.fillWidth: true
                }
                Item { Layout.preferredWidth: 32 }   // remove-button slot
            }
        }

        // ── Mappings list ──────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.bg
            border.color: Theme.colors.border
            border.width: Theme.dim.border

            ListView {
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                spacing: 0
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                // ListView with an integer model count - one delegate
                // per row index. Reassigning `dialog.mappings` to a
                // new array bumps `length` and triggers re-render.
                model: dialog.mappings.length
                delegate: Rectangle {
                    required property int index
                    width: ListView.view ? ListView.view.width : 0
                    height: 32
                    color: index % 2 === 0 ? Theme.colors.surface : Theme.colors.surfaceAlt

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        CheckBox {
                            checked: dialog.mappings[index].enabled === true
                            Layout.preferredWidth: 30
                            onToggled: dialog._setField(index, "enabled", checked)
                        }
                        TextField {
                            Layout.preferredWidth: 130
                            placeholderText: qsTr("e.g. union-jobs")
                            text: dialog.mappings[index].label || ""
                            onTextChanged: dialog._setField(index, "label", text)
                        }
                        TextField {
                            Layout.fillWidth: true
                            font.family: Theme.font.mono
                            font.pixelSize: Theme.font.sizeSmall
                            placeholderText: "Z:\\union-jobs"
                            text: dialog.mappings[index].win || ""
                            onTextChanged: dialog._setField(index, "win", text)
                        }
                        TextField {
                            Layout.fillWidth: true
                            font.family: Theme.font.mono
                            font.pixelSize: Theme.font.sizeSmall
                            placeholderText: "/Volumes/union-jobs"
                            text: dialog.mappings[index].mac || ""
                            onTextChanged: dialog._setField(index, "mac", text)
                        }
                        FlatButton {
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            iconName: "trash"
                            tooltip: qsTr("Remove")
                            onClicked: dialog._removeRow(index)
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: dialog.mappings.length === 0
                    text: qsTr("(no mappings — click \"Add Mapping\" to create one)")
                    color: Theme.colors.textSubtle
                    font.pixelSize: Theme.font.sizeSmall
                    font.italic: true
                }
            }
        }

        FlatButton {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            iconName: "plus"
            text: qsTr("Add Mapping")
            variant: "primary"
            onClicked: dialog._addRow()
        }

        Label {
            text: qsTr("<strong>Example:</strong> If your job files are at " +
                "<code>Z:\\union-jobs</code> on Windows and " +
                "<code>/Volumes/union-jobs</code> on macOS, add one row with " +
                "those two values and the same label.")
            color: Theme.colors.textSubtle
            font.pixelSize: Theme.font.sizeSmall
            wrapMode: Text.Wrap
            textFormat: Text.RichText
            Layout.fillWidth: true
        }

        Label {
            id: errLabel
            visible: false
            text: ""
            color: Theme.colors.error
            font.pixelSize: Theme.font.sizeSmall
            Layout.fillWidth: true
        }
    }
}
