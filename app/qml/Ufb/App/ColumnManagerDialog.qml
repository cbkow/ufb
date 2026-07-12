// ColumnManagerDialog — discoverable, button-driven UI for managing
// metadata columns on a (jobPath, folder) pair. Replaces the
// previous header right-click menus that were too hidden.
//
// Lists every column for the folder: configured ones first
// (ordered by columnOrder), discovered orphan keys after, separated
// visually. Per-row controls are inline (visibility checkbox, Edit /
// Delete buttons, or Promote for discovered keys). A bottom button
// row hosts the "+ Add Column" affordance. Hosts an embedded
// ColumnEditor for the actual add/edit/delete dialogs.
//
// Hosted by both ItemListPanel and TrackerView (per-job mode).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Dialog {
    id: dialog

    /// Required. Job + folder this dialog is editing columns for.
    property string jobPath: ""
    property string folderName: ""
    /// Optional: a JS array of metadata blob strings (one per item)
    /// the host already has in memory. Used to discover orphan keys
    /// without re-fetching the items table. Empty = no discovery.
    property var metadataBlobs: []

    title: qsTr("Manage Metadata — %1").arg(folderName || qsTr("(no folder)"))
    modal: true
    width: 620
    height: 600
    anchors.centerIn: parent
    standardButtons: Dialog.Close

    /// JS array of {columnId, columnName, columnType, columnOrder,
    /// columnWidth, isVisible, sourceJobPath, sourceFolderName,
    /// discovered}. Re-built whenever the dialog opens or a child
    /// editor reports a change.
    property var entries: []

    /// JS array of {id, presetName, columnsJson, …} from
    /// Columns.get_column_presets. Refreshed alongside `entries`.
    property var presets: []

    /// Drag-reorder transient state. `_dragSrcIndex` is the row
    /// being dragged (>= 0 while pressed), `_dragHoverIndex` is the
    /// drop target (the dragged row will be inserted *before* this
    /// index — set to entries.length to drop at the end). Both are
    /// reset to -1 on release.
    property int _dragSrcIndex: -1
    property int _dragHoverIndex: -1

    /// Count of configured (non-discovered) entries. Drag-reorder
    /// is only valid within this range — the discovered tail must
    /// stay below.
    function _configuredCount() {
        var n = 0
        for (var i = 0; i < entries.length; ++i) {
            if (!entries[i].discovered) ++n
        }
        return n
    }

    /// Splice `_dragSrcIndex` → `_dragHoverIndex` and persist the
    /// resulting order. Called on drag release. Drops outside the
    /// configured range are clamped to the last configured slot.
    function _commitDragReorder() {
        var src = _dragSrcIndex
        var dst = _dragHoverIndex
        _dragSrcIndex = -1
        _dragHoverIndex = -1
        if (src < 0 || dst < 0) return
        var maxConfigured = _configuredCount()
        if (dst > maxConfigured) dst = maxConfigured
        if (dst === src || dst === src + 1) return  // no-op
        var arr = entries.slice()
        var moved = arr.splice(src, 1)[0]
        // After removing the source row, indices > src shift left.
        var insertAt = dst > src ? dst - 1 : dst
        arr.splice(insertAt, 0, moved)
        entries = arr
        var ids = []
        for (var i = 0; i < arr.length; ++i) {
            var e = arr[i]
            if (e.discovered) continue
            if (typeof e.columnId !== "number" || e.columnId < 0) continue
            ids.push(e.columnId)
        }
        Columns.set_column_order(jobPath, folderName, JSON.stringify(ids))
    }

    function refresh() {
        // Drop caches AND bump Columns.columns_rev so any open table
        // panel (TrackerView / ItemListPanel) rebuilds its columns live.
        // This is how local add/edit/delete/duplicate/promote mutations —
        // which all funnel through editor.onChanged → refresh() — surface
        // in the grid without waiting for the manager to be closed.
        try { Columns.invalidate(dialog.jobPath, dialog.folderName) } catch (e) {}
        var configured = []
        var configuredNames = {}
        var defs = []
        try {
            defs = JSON.parse(Columns.get_column_defs(dialog.jobPath, dialog.folderName))
        } catch (e) {}
        // 0.9.97 debug: log what we got back from the binding so we
        // can diagnose "trashed columns still showing in manager"
        // and "no trash button on unpromoted" reports.
        console.log("[ColumnManager] refresh job=" + dialog.jobPath
            + " folder=" + dialog.folderName
            + " got " + defs.length + " defs")
        for (var dbg = 0; dbg < defs.length; ++dbg) {
            var dd = defs[dbg]
            console.log("  def["+dbg+"] id=" + dd.id
                + " name='" + dd.columnName + "'"
                + " type=" + dd.columnType
                + " templateHash=" + (dd.templateHash || "(none)")
                + " trashedAt=" + (dd.trashedAt || "(none)")
                + " deletedAt=" + (dd.deletedAt || "(none)"))
        }
        for (var i = 0; i < defs.length; ++i) {
            var d = defs[i]
            var name = d.columnName || ""
            configuredNames[name] = true
            // 0.9.97: a column is "promoted" iff its template_hash is
            // set. Unpromoted rows surface only here in the Column
            // Manager (the file browser grid filters them out) so the
            // user can pick Promote, Trash, or rename — without this
            // they'd be stranded.
            configured.push({
                columnId: typeof d.id === "number" ? d.id : -1,
                columnName: name,
                columnType: d.columnType || "text",
                columnOrder: typeof d.columnOrder === "number" ? d.columnOrder : 0,
                columnWidth: typeof d.columnWidth === "number" && d.columnWidth > 0
                    ? d.columnWidth : 110,
                isVisible: d.isVisible !== false,
                sourceJobPath: dialog.jobPath,
                sourceFolderName: dialog.folderName,
                discovered: false,
                promoted: typeof d.templateHash === "string"
                    && d.templateHash.length > 0,
                options: Array.isArray(d.options) ? d.options : []
            })
        }
        configured.sort(function(a, b) { return a.columnOrder - b.columnOrder })

        // 0.9.97 — discovered-keys pass removed. It was a legacy
        // safety net for "metadata exists but no column row" cases
        // that modern UFB no longer hits (mesh sync replicates
        // column rows; permanent-delete now scrubs item_metadata
        // keys; cross-OS path drift is canonicalized). It also
        // visually overlapped with unpromoted/trashed states and
        // gave the user no clean way to delete an orphan. The
        // underlying metadata stays in `item_metadata`; if the user
        // ever re-creates a column with the same name, the values
        // populate automatically (key match).
        entries = configured
        _refreshPresets()
    }

    function _refreshPresets() {
        try { presets = JSON.parse(Columns.get_column_presets()) }
        catch (e) { presets = [] }
    }

    onAboutToShow: {
        // Pull v2 templates from the share into the local cache
        // before the dialog renders, so the lookup-on-create flow
        // (when the user clicks "+ Add Column") sees the freshest
        // team-wide templates. Non-blocking: returns immediately
        // if the share is unreachable.
        try { Columns.refresh_templates() } catch (e) {}
        refresh()
    }

    function _nextOrder() {
        var max = -1
        for (var i = 0; i < entries.length; ++i) {
            if (!entries[i].discovered && entries[i].columnOrder > max) {
                max = entries[i].columnOrder
            }
        }
        return max + 1
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: qsTr("%1 column%2")
                    .arg(dialog.entries.length)
                    .arg(dialog.entries.length === 1 ? "" : "s")
                color: Theme.colors.textMuted
                font.pixelSize: 11
                Layout.fillWidth: true
            }
            FlatButton {
                iconName: "trash"
                text: qsTr("View Trash")
                tooltip: qsTr("Show columns sent to Trash. Restore or Delete Forever.")
                Layout.preferredHeight: Theme.dim.toolStripHeight
                onClicked: trashDialog.open()
            }
        }

        // ── Presets section ──────────────────────────────────────────
        // Each preset is a single saved column definition (name,
        // type, options, etc.) reusable across folders. Per-row Apply
        // adds it here; Delete removes the preset entirely. Save a
        // column as a preset via its row's "Save as Preset" button
        // below.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: qsTr("PRESETS")
                color: Theme.colors.textSubtle
                font.pixelSize: 10
                font.bold: true
                Layout.fillWidth: true
            }
            // Pull team-wide templates from the share into the local
            // cache. Templates aren't surfaced as a list in this
            // dialog (they appear via the lookup-on-create flow when
            // adding a column), but this button ensures freshness
            // when a user expects to see another peer's recently-
            // added "Status" / "Priority" / etc. setup.
            FlatButton {
                iconName: "arrow-clockwise"
                text: qsTr("Refresh from share")
                tooltip: qsTr("Pull the latest team setups from the shared folder")
                Layout.preferredHeight: 22
                onClicked: {
                    try { Columns.refresh_templates() } catch (e) {}
                    dialog._refreshPresets()
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(120, Math.max(28, presetList.contentHeight + 2))
            color: Theme.colors.bg
            border.color: Theme.colors.border
            border.width: 1

            ListView {
                id: presetList
                anchors.fill: parent
                anchors.margins: 1
                model: dialog.presets
                clip: true
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    width: presetList.width
                    height: 28
                    color: index % 2 === 0 ? Theme.colors.surface : Theme.colors.surfaceAlt
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 6
                        Label {
                            text: "📌"
                            color: Theme.colors.accent
                            font.pixelSize: 11
                            Layout.alignment: Qt.AlignVCenter
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                text: modelData.displayName || modelData.presetName || ""
                                color: Theme.colors.text
                                font.pixelSize: 12
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            // Provenance hint — saved-by + when. Helps
                            // multi-user teams know whose template this
                            // is before applying / deleting.
                            Label {
                                visible: !!modelData.createdBy
                                text: qsTr("by %1").arg(modelData.createdBy || "")
                                color: Theme.colors.textSubtle
                                font.pixelSize: 10
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                        FlatButton {
                            iconName: "plus"
                            text: qsTr("Apply")
                            enabled_: dialog.jobPath.length > 0 && dialog.folderName.length > 0
                            tooltip: qsTr("Add this template's column to %1").arg(dialog.folderName)
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                Columns.apply_preset(modelData.uniqueName, dialog.jobPath, dialog.folderName)
                                dialog.refresh()
                            }
                        }
                        FlatButton {
                            iconName: "trash"
                            tooltip: qsTr("Delete template")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                Columns.delete_column_preset(modelData.uniqueName)
                                dialog._refreshPresets()
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: dialog.presets.length === 0
                    text: qsTr("(no presets — save a column below to create one)")
                    color: Theme.colors.textSubtle
                    font.pixelSize: 11
                    font.italic: true
                }
            }
        }

        Label {
            text: qsTr("COLUMNS")
            color: Theme.colors.textSubtle
            font.pixelSize: 10
            font.bold: true
            Layout.fillWidth: true
            Layout.topMargin: 4
        }

        // ── Column list ──────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.bg
            border.color: Theme.colors.border
            border.width: 1

            ListView {
                id: entryList
                anchors.fill: parent
                anchors.margins: 1
                model: dialog.entries
                clip: true
                spacing: 0
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    id: rowRoot
                    width: entryList.width
                    height: 32
                    color: index % 2 === 0 ? Theme.colors.surface : Theme.colors.surfaceAlt
                    opacity: dialog._dragSrcIndex === index ? 0.4 : 1.0

                    // Drop-indicator line — drawn at the top edge when
                    // the hover index points at this row, or at the
                    // bottom edge when it points one past the last row.
                    Rectangle {
                        height: 2
                        width: parent.width
                        color: Theme.colors.accent
                        visible: dialog._dragSrcIndex >= 0
                            && (dialog._dragHoverIndex === index
                                || (dialog._dragHoverIndex === index + 1
                                    && index === dialog._configuredCount() - 1))
                        y: dialog._dragHoverIndex === index ? 0 : parent.height - 2
                        z: 5
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        anchors.rightMargin: 8
                        spacing: 6

                        // Drag handle (configured only). Press-and-
                        // drag the icon vertically to reorder. The
                        // host panel re-reads on dialog close.
                        Item {
                            visible: !modelData.discovered
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 24
                            Icon {
                                anchors.centerIn: parent
                                name: "dots-six-vertical"
                                size: 14
                                color: dragArea.containsMouse || dragArea.pressed
                                    ? Theme.colors.text
                                    : Theme.colors.textMuted
                            }
                            MouseArea {
                                id: dragArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                preventStealing: true
                                onPressed: {
                                    dialog._dragSrcIndex = index
                                    dialog._dragHoverIndex = index
                                }
                                onPositionChanged: {
                                    if (!pressed) return
                                    var pt = mapToItem(entryList.contentItem, mouseX, mouseY)
                                    var rowH = rowRoot.height
                                    if (rowH <= 0) return
                                    var raw = Math.round(pt.y / rowH)
                                    var maxConfigured = dialog._configuredCount()
                                    if (raw < 0) raw = 0
                                    if (raw > maxConfigured) raw = maxConfigured
                                    dialog._dragHoverIndex = raw
                                }
                                onReleased: dialog._commitDragReorder()
                                onCanceled: {
                                    dialog._dragSrcIndex = -1
                                    dialog._dragHoverIndex = -1
                                }
                            }
                        }
                        // Discovered placeholder so columns line up
                        // with configured rows' drag-handle slot.
                        Item {
                            visible: modelData.discovered
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 1
                        }

                        // Visibility toggle (configured only).
                        CheckBox {
                            visible: !modelData.discovered
                            checked: modelData.isVisible
                            onToggled: {
                                Columns.set_column_visible(
                                    modelData.columnId,
                                    modelData.sourceJobPath,
                                    modelData.sourceFolderName,
                                    checked)
                                dialog.refresh()
                            }
                            ToolTip.text: qsTr("Visible")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                        }
                        // Discovered placeholder so columns line up.
                        Item {
                            visible: modelData.discovered
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 1
                        }

                        // Unpromoted-state badge — small warning icon
                        // before the name. Tooltip carries the full
                        // explanation so the row stays compact and the
                        // name itself remains readable. Promoted +
                        // discovered rows skip the icon entirely.
                        Icon {
                            visible: !modelData.discovered
                                && modelData.promoted === false
                            name: "warning"
                            size: 12
                            color: Theme.colors.warning
                            ToolTip.text: qsTr("Unpromoted — hidden from the file grid until you click Promote.")
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            HoverHandler { id: unpromHover }
                            property bool hovered: unpromHover.hovered
                        }
                        Label {
                            text: modelData.columnName
                            color: modelData.discovered
                                ? Theme.colors.textMuted
                                : (modelData.promoted === false
                                    ? Theme.colors.warning
                                    : Theme.colors.text)
                            font.pixelSize: 12
                            font.italic: modelData.discovered || modelData.promoted === false
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: modelData.discovered
                                ? qsTr("(discovered)")
                                : modelData.columnType
                            color: modelData.discovered
                                ? Theme.colors.textSubtle
                                : Theme.colors.textMuted
                            font.pixelSize: 11
                            font.italic: modelData.discovered
                            Layout.preferredWidth: 90
                        }

                        // Discovered: column exists as a metadata key but
                        // has no row in column_definitions yet. Promote
                        // = create a real column with this name as a
                        // brand-new local row, then auto_promote_on_add
                        // attaches a template.
                        FlatButton {
                            visible: modelData.discovered
                            iconName: "arrow-up"
                            text: qsTr("Promote")
                            tooltip: qsTr("Create a real text column with this name; you can refine the type with Edit afterwards.")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                Columns.add_column(
                                    modelData.sourceJobPath,
                                    modelData.sourceFolderName,
                                    modelData.columnName,
                                    "text",
                                    "",
                                    true,
                                    dialog._nextOrder(),
                                    110)
                                dialog.refresh()
                            }
                        }
                        // Unpromoted-but-existing: column row exists in
                        // column_definitions but has no template_hash
                        // (mesh receive failed to fetch the template, OR
                        // user explicitly Unpromoted earlier). Promote
                        // here calls lookup_or_mint via the existing
                        // auto-promote pipeline.
                        FlatButton {
                            visible: !modelData.discovered && modelData.promoted === false
                            iconName: "arrow-up"
                            text: qsTr("Promote")
                            tooltip: qsTr("Attach this column to a shared template (or mint a fresh one). Without promotion the column is hidden from the grid.")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                if (Columns.promote_column(modelData.columnId)) {
                                    dialog.refresh()
                                }
                            }
                        }
                        FlatButton {
                            visible: !modelData.discovered
                            iconName: "bookmark-simple"
                            tooltip: qsTr("Save this column as a reusable preset to apply in other folders")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: savePresetDialog.openFor(dialog.entries[index])
                        }
                        FlatButton {
                            visible: !modelData.discovered
                            iconName: "pencil-simple"
                            tooltip: qsTr("Edit column")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            // Pull the entry from the source JS array
                            // by index — QML coerces `modelData` to a
                            // QVariantMap which silently drops nested
                            // arrays (so `modelData.options` is always
                            // empty), but `dialog.entries[index]` is
                            // the original object reference with
                            // `options` intact.
                            onClicked: editor.openForEdit(dialog.entries[index])
                        }
                        FlatButton {
                            visible: !modelData.discovered
                            iconName: "copy"
                            tooltip: qsTr("Duplicate as new column (rename + fork the underlying setup)")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: editor.openForDuplicate(dialog.entries[index])
                        }
                        FlatButton {
                            visible: !modelData.discovered
                            iconName: "arrow-down"
                            tooltip: qsTr("Unpromote column — detach from its template and hide from the grid. Options are kept; you can re-promote via lookup.")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                if (Columns.unpromote_column(modelData.columnId)) {
                                    dialog.refresh()
                                }
                            }
                        }
                        FlatButton {
                            visible: !modelData.discovered
                            iconName: "trash"
                            tooltip: qsTr("Send column to Trash. Recoverable from the Trash dialog.")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: editor.openForDelete(dialog.entries[index])
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: dialog.entries.length === 0
                    text: qsTr("(no columns yet — click \"+ Add Column\" to create one)")
                    color: Theme.colors.textSubtle
                    font.pixelSize: 11
                    font.italic: true
                }
            }
        }

        FlatButton {
            iconName: "plus"
            text: qsTr("Add Column")
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            variant: "primary"
            enabled_: dialog.jobPath.length > 0 && dialog.folderName.length > 0
            onClicked: editor.openForAdd(dialog.jobPath, dialog.folderName, dialog._nextOrder())
        }
    }

    ColumnEditor {
        id: editor
        onChanged: dialog.refresh()
    }

    ColumnTrashDialog {
        id: trashDialog
        jobPath: dialog.jobPath
        folderName: dialog.folderName
        onClosed: dialog.refresh()
    }

    Dialog {
        id: savePresetDialog
        property var pendingCol: null
        title: qsTr("Save Column as Template")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(col) {
            pendingCol = col
            presetNameField.text = col ? col.columnName : ""
            saveErrorLabel.text = ""
            open()
            presetNameField.forceActiveFocus()
            presetNameField.selectAll()
        }
        ColumnLayout {
            spacing: 8
            width: 360
            Label {
                text: qsTr("Save “%1” as a reusable template. Templates live in the shared folder so every peer can use them.")
                    .arg(savePresetDialog.pendingCol ? savePresetDialog.pendingCol.columnName : "")
                color: Theme.colors.textMuted
                font.pixelSize: 11
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            TextField {
                id: presetNameField
                Layout.fillWidth: true
                placeholderText: qsTr("Template name")
                onAccepted: savePresetDialog.accept()
            }
            // Inline error / collision message. Empty by default; the
            // onAccepted handler populates this on bindings-side
            // failure (name collision, share unavailable, etc.) and
            // re-opens the dialog so the user can fix it.
            Label {
                id: saveErrorLabel
                visible: text.length > 0
                color: Theme.colors.error
                font.pixelSize: 11
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
        onAccepted: {
            var name = presetNameField.text.trim()
            var col = pendingCol
            if (name.length === 0 || !col || col.columnId < 0) return
            var result = Columns.save_column_as_preset(
                name, col.columnId,
                col.sourceJobPath, col.sourceFolderName)
            if (result === "__name_collision__") {
                saveErrorLabel.text = qsTr(
                    "A template named “%1” already exists. " +
                    "Edit that one instead, or pick a different name."
                ).arg(name)
                open()  // re-open so the user can correct
                presetNameField.forceActiveFocus()
                presetNameField.selectAll()
                return
            }
            if (result.length === 0) {
                saveErrorLabel.text = qsTr(
                    "Couldn't save the template — the shared folder " +
                    "may be unreachable. Check Mesh Sync settings."
                )
                open()
                return
            }
            dialog._refreshPresets()
        }
    }
}
