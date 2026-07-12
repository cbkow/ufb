// Add Bookmark dialog.
//
// Always saves to the Bookmarks store. The "Jobs folder" checkbox
// flags the bookmark with `is_project_folder=true`, which only
// changes its sidebar icon and lets the FileBrowser surface a
// "Synced" column when the user opens it.
//
// Subscriptions are NOT created here — a subscription is an
// individual job being watched, produced via the New Job template
// flow (or auto-tracked when the user opens a job tab). A jobs
// folder *contains* subscriptions; it isn't one itself.
//
// Folder picker is Qt's native FolderDialog. Display name auto-fills
// from the picked folder's basename.

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ufb.Backend 1.0

Dialog {
    id: dialog

    title: qsTr("Add Bookmark")
    modal: true
    parent: Overlay.overlay
    x: (parent ? (parent.width  - width)  / 2 : 0)
    y: (parent ? (parent.height - height) / 2 : 0)
    width: 480
    standardButtons: Dialog.Save | Dialog.Cancel

    function _basename(p) {
        if (!p || p.length === 0) return ""
        var s = String(p).replace(/\/+$/, "").replace(/\\+$/, "")
        var slash = Math.max(s.lastIndexOf("/"), s.lastIndexOf("\\"))
        return slash >= 0 ? s.substring(slash + 1) : s
    }

    /// Open with the "Jobs folder" toggle pre-set to `asJobFolder`.
    /// Sidebar's BOOKMARKS "+" passes false; future flows that want
    /// to seed it true (e.g. a "Bookmark this jobs folder" right-
    /// click on a directory) can pass true.
    function open_(asJobFolder) {
        jobToggle.checked = !!asJobFolder
        pathField.text = ""
        nameField.text = ""
        open()
        pathField.forceActiveFocus()
    }
    function openForBookmark()  { open_(false) }
    function openForJobFolder() { open_(true) }

    onAccepted: {
        var p = pathField.text.trim()
        if (p.length === 0) return
        var n = nameField.text.trim()
        if (n.length === 0) n = _basename(p)
        Bookmarks.add(p, n, jobToggle.checked)
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 8

        Label {
            text: jobToggle.checked
                ? qsTr("Pick the parent folder that contains your jobs. The Browser will show which subfolders are subscribed when you navigate inside.")
                : qsTr("Pick any folder to bookmark for quick access from the sidebar.")
            color: Theme.colors.textMuted
            font.pixelSize: Theme.font.sizeSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Label {
            text: qsTr("Folder")
            color: Theme.colors.text
            font.pixelSize: Theme.font.sizeSmall
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            TextField {
                id: pathField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.dim.toolStripHeight
                font.family: Theme.font.mono
                font.pixelSize: Theme.font.sizeBody
                color: Theme.colors.text
                placeholderText: qsTr("\\\\nas\\share\\path or C:\\folder")
                placeholderTextColor: Theme.colors.textSubtle
                selectByMouse: true
                onTextChanged: {
                    if (nameField.text.length === 0) {
                        nameField.placeholderText = dialog._basename(text)
                    }
                }
            }
            FlatButton {
                Layout.preferredHeight: Theme.dim.toolStripHeight
                iconName: "folder-simple"
                text: qsTr("Browse…")
                onClicked: folderPicker.open()
            }
        }

        Label {
            text: qsTr("Display name")
            color: Theme.colors.text
            font.pixelSize: Theme.font.sizeSmall
        }
        TextField {
            id: nameField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            font.pixelSize: Theme.font.sizeBody
            color: Theme.colors.text
            placeholderText: qsTr("(auto from folder name)")
            placeholderTextColor: Theme.colors.textSubtle
            selectByMouse: true
        }

        // "Jobs folder" flag — controls is_project_folder on the
        // saved bookmark. A jobs folder is a parent that contains
        // many job subfolders; the sidebar gives it a clipboard
        // icon and the Browser lights up the synced/subscribed
        // column when you navigate into it.
        CheckBox {
            id: jobToggle
            text: qsTr("This is a jobs folder (parent of many projects)")
            checked: false
            Layout.topMargin: 4
            ToolTip.text: qsTr("Tags this bookmark as a parent of jobs. Doesn't subscribe — subscriptions are individual jobs created or opened later.")
            ToolTip.visible: hovered
            ToolTip.delay: 500
        }
    }

    FolderDialog {
        id: folderPicker
        title: dialog.title
        onAccepted: {
            var u = String(selectedFolder)
            var p = u.replace(/^file:[\/\\]+/, "")
            if (u.indexOf("file:////") === 0) p = "\\\\" + p
            pathField.text = p.replace(/\//g, "\\")
            if (nameField.text.length === 0) {
                nameField.text = dialog._basename(pathField.text)
            }
        }
    }
}
