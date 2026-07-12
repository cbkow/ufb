// NewJobDialog — "Add full job from template".
//
// Asks for Job Number + Job Name; previews the resulting folder
// name (`{number}_{name}` with whitespace replaced by underscores)
// before creating it. On Save, calls
// Subscription.create_job_from_template(parent, number, name) which
// copies the bundled template tree into the new folder and
// auto-subscribes.
//
// Visible from the FileBrowser toolbar only when the current
// directory is a bookmarked jobs folder (see _currentIsProjectFolder
// in FileBrowser.qml).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Dialog {
    id: dialog

    /// Parent jobs-folder path the new job will be created in.
    /// Set by openFor() before each show.
    property string parentPath: ""

    /// Emitted on successful creation so the host (FileBrowser)
    /// can refresh its directory listing. Carries the new folder
    /// path in case callers want to navigate into it.
    signal created(string newPath)

    title: qsTr("New Job from Template")
    modal: true
    parent: Overlay.overlay
    x: (parent ? (parent.width  - width)  / 2 : 0)
    y: (parent ? (parent.height - height) / 2 : 0)
    width: 500
    standardButtons: Dialog.Save | Dialog.Cancel

    function _sanitise(s) {
        // Mirror core::jobs::sanitise + folder-name rules: strip
        // path-illegal chars, then replace whitespace with _.
        return String(s || "")
            .replace(/[\\/:*?"<>|]/g, "")
            .trim()
            .replace(/\s+/g, "_")
    }
    function _previewName() {
        var n = dialog._sanitise(numberField.text)
        var name = dialog._sanitise(nameField.text)
        if (n.length === 0 && name.length === 0) return ""
        if (n.length === 0)    return name
        if (name.length === 0) return n
        return n + "_" + name
    }
    /// Open the dialog for `parent` (the jobs folder the new
    /// job will sit inside). Resets the form.
    function openFor(parent) {
        parentPath = parent
        numberField.text = ""
        nameField.text = ""
        open()
        numberField.forceActiveFocus()
    }

    onAccepted: {
        if (parentPath.length === 0) return
        var n = numberField.text.trim()
        var name = nameField.text.trim()
        if (n.length === 0 && name.length === 0) return
        var path = Subscription.create_job_from_template(parentPath, n, name)
        if (path.length === 0) {
            errLabel.text = qsTr("Failed to create job - check the log for details.")
            errLabel.visible = true
        } else {
            dialog.created(path)
        }
    }
    onRejected: {
        errLabel.visible = false
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 8

        Label {
            text: qsTr("Creates a new job folder under:")
            color: Theme.colors.textMuted
            font.pixelSize: Theme.font.sizeSmall
            Layout.fillWidth: true
        }
        Label {
            text: dialog.parentPath
            color: Theme.colors.text
            font.family: Theme.font.mono
            font.pixelSize: Theme.font.sizeSmall
            elide: Text.ElideMiddle
            Layout.fillWidth: true
        }

        GridLayout {
            columns: 2
            columnSpacing: 8
            rowSpacing: 6
            Layout.fillWidth: true
            Layout.topMargin: 4

            Label {
                text: qsTr("Job number")
                color: Theme.colors.text
                font.pixelSize: Theme.font.sizeSmall
            }
            TextField {
                id: numberField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.dim.toolStripHeight
                placeholderText: qsTr("261234")
                placeholderTextColor: Theme.colors.textSubtle
                color: Theme.colors.text
                font.family: Theme.font.mono
                font.pixelSize: Theme.font.sizeBody
                selectByMouse: true
            }

            Label {
                text: qsTr("Job name")
                color: Theme.colors.text
                font.pixelSize: Theme.font.sizeSmall
            }
            TextField {
                id: nameField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.dim.toolStripHeight
                placeholderText: qsTr("Glenlivet")
                placeholderTextColor: Theme.colors.textSubtle
                color: Theme.colors.text
                font.pixelSize: Theme.font.sizeBody
                selectByMouse: true
            }
        }

        // Live preview of the resulting folder name.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            Layout.topMargin: 6
            color: Theme.colors.surface
            border.color: Theme.colors.border
            border.width: Theme.dim.border
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 6
                Label {
                    text: qsTr("Folder:")
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                }
                Label {
                    text: dialog._previewName().length > 0
                        ? dialog._previewName()
                        : qsTr("(enter number and/or name)")
                    color: dialog._previewName().length > 0
                        ? Theme.colors.textBright
                        : Theme.colors.textSubtle
                    font.family: Theme.font.mono
                    font.pixelSize: Theme.font.sizeBody
                    Layout.fillWidth: true
                }
            }
        }

        Label {
            id: errLabel
            visible: false
            text: ""
            color: Theme.colors.error
            font.pixelSize: Theme.font.sizeSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
}
