// ColumnEditor — shared add / edit / delete dialogs for metadata
// columns. Hosted by ColumnManagerDialog and (transitively) by both
// ItemListPanel and TrackerView so the per-pane manager UI stays
// consistent.
//
// Usage:
//   ColumnEditor {
//       id: editor
//       onChanged: someParent.refresh()
//   }
//   ...
//   editor.openForAdd(jobPath, folderName, nextOrder)
//   editor.openForEdit(colDescriptor)
//   editor.openForDelete(colDescriptor)
//
// `colDescriptor` is the same shape ColumnManagerDialog and
// TrackerView use:
//   { columnId, columnName, columnType, columnOrder, columnWidth,
//     sourceJobPath, sourceFolderName, discovered }

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Item {
    id: root

    /// Fired after any successful add / edit / delete so the host can
    /// re-fetch + redraw.
    signal changed()

    function openForAdd(jobPath, folderName, nextOrder) {
        addEdit.mode = "add"
        addEdit.editId = -1
        addEdit.editJobPath = jobPath || ""
        addEdit.editFolderName = folderName || ""
        addEdit.editOrder = typeof nextOrder === "number" ? nextOrder : 0
        addEdit.editWidth = 110
        addEdit._nameField.text = ""
        addEdit._typeBox.currentIndex = 0
        addEdit._defaultField.text = ""
        addEdit._visibleBox.checked = true
        addEdit.optionsModel.clear()
        addEdit.open()
        addEdit._nameField.forceActiveFocus()
    }
    function openForEdit(col) {
        if (!col) return
        addEdit.mode = "edit"
        addEdit.editId = col.columnId
        addEdit.editJobPath = col.sourceJobPath
        addEdit.editFolderName = col.sourceFolderName
        addEdit.editOrder = col.columnOrder
        addEdit.editWidth = col.columnWidth
        addEdit._nameField.text = col.columnName
        // Normalize to lowercase — legacy data may have any casing.
        var normalized = String(col.columnType || "text").toLowerCase()
        var typeIdx = addEdit._typeBox.model.indexOf(normalized)
        addEdit._typeBox.currentIndex = typeIdx >= 0 ? typeIdx : 0
        addEdit._defaultField.text = ""    // not exposed yet
        addEdit._visibleBox.checked = true
        // Phase 4 — locked-template state. When the column has a
        // template_hash, displayName + columnType + existing option
        // labels become read-only (renames force "Duplicate to new
        // column" via Phase 5). Per-option colors and new-option
        // additions stay editable. The propagation banner reports
        // how many folders share this template so the user
        // understands the blast radius of an edit.
        addEdit.editTemplateHash = String(col.templateHash || "")
        addEdit._existingOptionCount = 0
        // Seed options editor from the column descriptor (caller
        // typically sources this from Columns.get_column_defs which
        // includes the parsed options array).
        addEdit.optionsModel.clear()
        var existing = (col.options && Array.isArray(col.options)) ? col.options : []
        for (var i = 0; i < existing.length; ++i) {
            var o = existing[i] || {}
            addEdit.optionsModel.append({
                name: String(o.name || ""),
                color: String(o.color || ""),
                _existing: true
            })
        }
        addEdit._existingOptionCount = existing.length
        if (addEdit.editTemplateHash.length > 0) {
            try {
                addEdit.editTemplateUsageCount =
                    Columns.template_usage_count(addEdit.editTemplateHash)
            } catch (e) {
                addEdit.editTemplateUsageCount = 0
            }
        } else {
            addEdit.editTemplateUsageCount = 0
        }
        addEdit.open()
        if (addEdit.editTemplateHash.length === 0) {
            addEdit._nameField.forceActiveFocus()
            addEdit._nameField.selectAll()
        }
    }
    function openForDelete(col) {
        if (!col) return
        deleteConfirm.pendingCol = col
        deleteConfirm.open()
    }
    function openForDuplicate(col) {
        if (!col) return
        duplicateDialog.pendingCol = col
        duplicateDialog.newNameField.text = ""
        duplicateDialog.copyMetadataBox.checked = true
        duplicateDialog.deleteSourceBox.checked = false
        try {
            duplicateDialog.metadataValueCount = Columns.count_metadata_values(
                col.sourceJobPath, col.sourceFolderName, col.columnName)
        } catch (e) {
            duplicateDialog.metadataValueCount = 0
        }
        duplicateDialog.open()
        duplicateDialog.newNameField.forceActiveFocus()
    }

    Dialog {
        id: addEdit
        property string mode: "add"           // "add" | "edit"
        property int editId: -1
        property string editJobPath: ""
        property string editFolderName: ""
        property int editOrder: 0
        property real editWidth: 110
        // Phase 4 — populated by openForEdit when the column being
        // edited has an associated v2 template. Empty string =
        // not-templated (renames + type changes still allowed,
        // legacy / local-only column). When set, displayName +
        // type + existing option labels become read-only and the
        // propagation banner appears.
        property string editTemplateHash: ""
        property int editTemplateUsageCount: 0
        // Number of options that were in the model at openForEdit
        // time. New options appended via "Add Option" have indices
        // >= this; existing ones have indices < this. Used to gate
        // per-option name-field editability in templated mode.
        property int _existingOptionCount: 0
        readonly property bool _isTemplated: editTemplateHash.length > 0

        // Aliases for the helper functions above so the field IDs are
        // local to this Dialog rather than polluting outer scope.
        property alias _nameField: nameField
        property alias _typeBox: typeBox
        property alias _defaultField: defaultField
        property alias _visibleBox: visibleBox
        property alias optionsModel: optionsModel

        // Live editing buffer for dropdown / priority options. Each
        // row: { name, color }. Persisted on accept via
        // Columns.set_column_options.
        ListModel { id: optionsModel }

        readonly property bool _isDropdown:
            typeBox.currentText === "dropdown" || typeBox.currentText === "priority"

        title: mode === "edit" ? qsTr("Edit Column") : qsTr("Add Column")
        modal: true
        // Parent to the application Overlay so we center on the
        // window — `parent` here is ColumnEditor (a 0×0 Item nested
        // inside ColumnManagerDialog), so anchors.centerIn: parent
        // would dock the dialog at the top-left corner.
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        width: 460
        standardButtons: Dialog.Ok | Dialog.Cancel
        // Extra breathing room above the OK/Cancel button row — the
        // options list pushes the bottom of the content layout right
        // up against the standardButtons strip otherwise.
        bottomPadding: 16

        ColumnLayout {
            // Fill the dialog's available width — the Dialog itself
            // sets `width: 460` and that minus padding is what we
            // get here. Hard-coding `width: 420` overflowed inside
            // narrower dialog widths.
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 8
            Label {
                text: qsTr("Column in:\n%1  /  %2")
                    .arg(addEdit.editJobPath || qsTr("(no job)"))
                    .arg(addEdit.editFolderName || qsTr("(no folder)"))
                color: Theme.colors.textMuted
                font.pixelSize: 11
                Layout.fillWidth: true
            }

            // ── Templated propagation banner ─────────────────────────
            // Surfaces when this column is linked to a shared v2
            // template. Tells the user how many folders will pick
            // up their option / color / default edits, and that
            // displayName / type / existing option labels are
            // immutable (Phase 5's "Duplicate as new column" is
            // the workaround for renames).
            Rectangle {
                visible: addEdit._isTemplated
                Layout.fillWidth: true
                Layout.preferredHeight: bannerCol.implicitHeight + 12
                color: Theme.colors.accentMuted
                radius: 3
                ColumnLayout {
                    id: bannerCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 2
                    Label {
                        text: addEdit.editTemplateUsageCount > 1
                            ? qsTr("Used on %1 folders. Option / color / default edits propagate to all of them.")
                                .arg(addEdit.editTemplateUsageCount)
                            : qsTr("Shared template. Option / color / default edits propagate to peers using this setup.")
                        color: Theme.colors.textBright
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("Name and type are locked. Use “Duplicate as…” to fork.")
                        color: Theme.colors.text
                        font.pixelSize: 10
                        font.italic: true
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }

            GridLayout {
                columns: 2
                columnSpacing: 8
                rowSpacing: 6
                Layout.fillWidth: true

                Label { text: qsTr("Name"); color: Theme.colors.text }
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Status, Priority, Due Date, …")
                    onAccepted: addEdit.accept()
                    // Templated columns: name is the shared template's
                    // displayName, immutable per the locked design.
                    readOnly: addEdit._isTemplated
                    color: addEdit._isTemplated
                        ? Theme.colors.textMuted
                        : Theme.colors.text
                }

                Label { text: qsTr("Type"); color: Theme.colors.text }
                ComboBox {
                    id: typeBox
                    Layout.fillWidth: true
                    model: ["text", "dropdown", "date", "priority",
                            "number", "checkbox", "links", "notes"]
                    // Templated columns: type is part of the template
                    // identity (the manifest filters by columnType).
                    // Allowing a type change would orphan the column
                    // from its template; force "Duplicate as…" instead.
                    enabled: !addEdit._isTemplated
                }

                Label { text: qsTr("Default"); color: Theme.colors.text }
                TextField {
                    id: defaultField
                    Layout.fillWidth: true
                    placeholderText: qsTr("(optional)")
                }

                Label { text: qsTr("Visible"); color: Theme.colors.text }
                CheckBox { id: visibleBox; checked: true }
            }

            // ── Options editor (dropdown / priority only) ───────────
            ColumnLayout {
                visible: addEdit._isDropdown
                spacing: 4
                Layout.fillWidth: true

                Label {
                    text: qsTr("Options")
                    color: Theme.colors.text
                    font.pixelSize: 11
                    font.bold: true
                }
                Label {
                    text: qsTr("Each option has a name and an optional color hex (e.g. #4a90e2). Empty rows are dropped on save.")
                    color: Theme.colors.textMuted
                    font.pixelSize: 10
                    font.italic: true
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(
                        180, Math.max(28, optionsModel.count * 30 + 4))
                    color: Theme.colors.surface
                    border.color: Theme.colors.borderStrong
                    border.width: Theme.dim.border
                    ListView {
                        id: optionsList
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true
                        model: optionsModel
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: Item {
                            id: optionRow
                            required property int index
                            required property string name
                            required property string color
                            // Templated mode: rows seeded from the
                            // existing template at openForEdit time
                            // get their name TextField locked.
                            // Newly-added options (index >=
                            // _existingOptionCount) are fully
                            // editable.
                            readonly property bool _isExistingInTemplate:
                                addEdit._isTemplated
                                    && index < addEdit._existingOptionCount
                            width: optionsList.width
                            height: 28
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                spacing: 4
                                // Color swatch — click to cycle through
                                // a small palette. Lightweight stand-in
                                // for a real color picker; the user
                                // can also type the hex into the field.
                                Rectangle {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    radius: 3
                                    color: optionRow.color.length > 0 ? optionRow.color : "transparent"
                                    border.color: Theme.colors.borderStrong
                                    border.width: 1
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            var palette = [
                                                "", "#4a90e2", "#7ed321", "#f5a623",
                                                "#d0021b", "#9013fe", "#50e3c2",
                                                "#bd10e0", "#9b9b9b", "#000000", "#ffffff"
                                            ]
                                            var idx = palette.indexOf(optionRow.color)
                                            var next = palette[(idx + 1 + palette.length) % palette.length]
                                            optionsModel.setProperty(optionRow.index, "color", next)
                                        }
                                    }
                                }
                                TextField {
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("Option name")
                                    text: optionRow.name
                                    readOnly: optionRow._isExistingInTemplate
                                    color: optionRow._isExistingInTemplate
                                        ? Theme.colors.textMuted
                                        : Theme.colors.text
                                    onTextChanged: {
                                        if (!optionRow._isExistingInTemplate)
                                            optionsModel.setProperty(optionRow.index, "name", text)
                                    }
                                }
                                TextField {
                                    Layout.preferredWidth: 80
                                    placeholderText: "#hex"
                                    text: optionRow.color
                                    onTextChanged: optionsModel.setProperty(optionRow.index, "color", text)
                                }
                                FlatButton {
                                    iconName: "x"
                                    iconSize: Theme.icon.sizeSmall
                                    tooltip: optionRow._isExistingInTemplate
                                        ? qsTr("Removing a shared option orphans existing item values across folders. Use Duplicate As… for major edits.")
                                        : qsTr("Remove option")
                                    Layout.preferredHeight: Theme.dim.toolStripHeight
                                    onClicked: optionsModel.remove(optionRow.index)
                                }
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Item { Layout.fillWidth: true }
                    FlatButton {
                        iconName: "plus"
                        text: qsTr("Add Option")
                        Layout.preferredHeight: Theme.dim.toolStripHeight
                        onClicked: optionsModel.append({ name: "", color: "" })
                    }
                }
            }
        }

        onAccepted: {
            var name = nameField.text.trim()
            if (name.length === 0) return
            var type = typeBox.currentText
            // On Add: query the v2 templates manifest for an existing
            // column with the same (commonName, type) in this project.
            // If matches exist, pop the lookup-on-create dialog and
            // let the user decide Use it vs Create new. The actual
            // save runs from the conflict dialog's branches.
            //
            // On Edit: skip the lookup — we're modifying an existing
            // row, not creating a new one.
            if (mode === "add") {
                var hits = []
                try {
                    hits = JSON.parse(Columns.lookup_template(name, type, editJobPath))
                } catch (e) { hits = [] }
                if (hits.length > 0) {
                    templateConflict.pendingName = name
                    templateConflict.pendingType = type
                    templateConflict.pendingDefault = defaultField.text
                    templateConflict.pendingVisible = visibleBox.checked
                    templateConflict.pendingOrder = editOrder
                    templateConflict.pendingWidth = editWidth
                    templateConflict.pendingJobPath = editJobPath
                    templateConflict.pendingFolderName = editFolderName
                    // Snapshot options so the user's option-list edits
                    // ride through to whichever branch they pick.
                    var snap = []
                    for (var i = 0; i < optionsModel.count; ++i) {
                        var r = optionsModel.get(i)
                        snap.push({ name: r.name || "", color: r.color || "" })
                    }
                    templateConflict.pendingOptions = snap
                    templateConflict.pendingIsDropdown = _isDropdown
                    templateConflict.matches = hits
                    // Defer the open to the next event-loop tick. We are
                    // inside addEdit's onAccepted, and addEdit (modal) is
                    // mid-close from the OK button. Opening a second modal
                    // synchronously during the first's close transition
                    // makes Qt Quick Controls silently drop the open() —
                    // and since this dialog carries the ONLY save call for
                    // the template-match path, the column never persists
                    // ("OK closes without saving"). callLater runs after
                    // addEdit has fully closed, so the open lands.
                    Qt.callLater(function() { templateConflict.open() })
                    return
                }
            }
            // No conflict — proceed with the original add/update path.
            _proceedSave(false, false)
        }

        // Factored out of onAccepted so the templateConflict dialog
        // can call this with `forceNew = true` for the "Create new"
        // branch, or with `suppressOptionSet = true` for the "Use
        // it" branch (where bindings populate column_options from
        // the existing shared template).
        function _proceedSave(forceNew, suppressOptionSet) {
            var name = nameField.text.trim()
            if (name.length === 0) return
            var type = typeBox.currentText
            var def = defaultField.text
            var vis = visibleBox.checked
            var savedId = -1
            if (mode === "add") {
                if (forceNew) {
                    savedId = Columns.add_column_force_new(
                        editJobPath, editFolderName, name, type, def, vis,
                        editOrder, editWidth)
                } else {
                    savedId = Columns.add_column(
                        editJobPath, editFolderName, name, type, def, vis,
                        editOrder, editWidth)
                }
            } else {
                if (Columns.update_column(
                        editId, editJobPath, editFolderName, name, type, def, vis,
                        editOrder, editWidth)) {
                    savedId = editId
                }
            }
            // A failed save used to vanish silently (the dialog had
            // already closed on OK). Surface it so it's diagnosable
            // rather than looking like "OK did nothing".
            if (savedId < 0) {
                console.warn("ColumnEditor: save failed for '"
                    + name + "' (" + type + ") in "
                    + editJobPath + "/" + editFolderName
                    + " — mode=" + mode + " forceNew=" + forceNew)
            }
            // Persist option list for dropdown / priority types.
            // Suppressed when the user clicked "Use it" on the
            // lookup-on-create dialog: in that case the new
            // column's column_options have already been populated
            // by auto_promote_on_add (reuse path) from the existing
            // shared template, and running set_column_options here
            // would silently overwrite them with whatever the user
            // typed (or didn't) before the conflict dialog
            // interrupted.
            if (!suppressOptionSet && savedId >= 0 && _isDropdown) {
                var opts = []
                for (var i = 0; i < optionsModel.count; ++i) {
                    var r = optionsModel.get(i)
                    var nm = (r.name || "").trim()
                    if (nm.length === 0) continue
                    opts.push({ name: nm, color: (r.color || "").trim() })
                }
                Columns.set_column_options(
                    savedId, editJobPath, editFolderName,
                    JSON.stringify(opts))
            }
            root.changed()
        }
    }

    // ── Lookup-on-create conflict dialog ───────────────────────────
    // Surfaces when the user types a column name that matches an
    // existing v2 template in this project. Two branches:
    //   • Use it       — apply the existing template (reuses its
    //                    schema, hash, options).
    //   • Create new   — fork: mint a fresh template with the user's
    //                    own option list.
    // "Edit it" lands in Phase 4 alongside the proper template-edit
    // dialog; for now the user adds with "Use it" and edits the
    // shared options via Manage Metadata.
    Dialog {
        id: templateConflict
        title: qsTr("Setup already exists")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        width: 480
        bottomPadding: 16

        // Stashed form state from addEdit — these ride through to
        // _proceedSave so the user's edits aren't lost.
        property string pendingName: ""
        property string pendingType: ""
        property string pendingDefault: ""
        property bool pendingVisible: true
        property int pendingOrder: 0
        property real pendingWidth: 110
        property string pendingJobPath: ""
        property string pendingFolderName: ""
        property var pendingOptions: []
        property bool pendingIsDropdown: false
        property var matches: []

        ColumnLayout {
            width: 440
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("A “%1” setup already exists in this project. Use the team's existing version or create your own?")
                    .arg(templateConflict.pendingName)
                color: Theme.colors.textMuted
                font.pixelSize: Theme.font.sizeBody
                wrapMode: Text.Wrap
            }

            // Match list — typically 1 entry, occasionally more (e.g.
            // forks coexist). Show creator + last-used so the user
            // can pick the right one if there's ambiguity.
            Repeater {
                model: templateConflict.matches
                delegate: Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    color: Theme.colors.surfaceAlt
                    radius: Theme.dim.radius
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 8
                        Icon {
                            name: "users"
                            size: Theme.icon.sizeSmall
                            color: Theme.colors.accent
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Label {
                            text: modelData.displayName || templateConflict.pendingName
                            color: Theme.colors.text
                            font.pixelSize: Theme.font.sizeBody
                            Layout.fillWidth: true
                        }
                        Label {
                            text: modelData.createdBy
                                ? qsTr("by %1").arg(modelData.createdBy)
                                : ""
                            color: Theme.colors.textSubtle
                            font.pixelSize: Theme.font.sizeSmall
                        }
                    }
                }
            }
        }

        // Three explicit choices instead of standard OK/Cancel — the
        // wording matters for clarity. Dialog.accept()/reject() bind
        // to our chosen-action enum via templateConflict._chosen.
        property string _chosen: ""
        footer: DialogButtonBox {
            Button {
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                text: qsTr("Create new")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    templateConflict._chosen = "new"
                    templateConflict.accept()
                }
            }
            Button {
                text: qsTr("Use it")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                highlighted: true
                onClicked: {
                    templateConflict._chosen = "use"
                    templateConflict.accept()
                }
            }
        }

        onAccepted: {
            // Restore the form values we stashed at lookup time so
            // _proceedSave reads from the same fields it normally
            // would. addEdit was already closed by Dialog.Ok above;
            // re-seeding the fields keeps _proceedSave's reads valid.
            addEdit._nameField.text = pendingName
            // typeBox is bound by index — find the matching string.
            var idx = addEdit._typeBox.model.indexOf(pendingType)
            addEdit._typeBox.currentIndex = idx >= 0 ? idx : 0
            addEdit._defaultField.text = pendingDefault
            addEdit._visibleBox.checked = pendingVisible
            addEdit.editOrder = pendingOrder
            addEdit.editWidth = pendingWidth
            addEdit.editJobPath = pendingJobPath
            addEdit.editFolderName = pendingFolderName
            addEdit.optionsModel.clear()
            for (var i = 0; i < pendingOptions.length; ++i) {
                addEdit.optionsModel.append(pendingOptions[i])
            }
            // "Use it" → suppress option-set so the bindings'
            // template-reuse path (set_options_for_column from
            // auto_promote_on_add) is the source of truth for the
            // new column's options.
            // "Create new" → fork: user's typed options become the
            // new template's seed options via the normal
            // set_column_options call.
            var forceNew = _chosen === "new"
            var suppressOptionSet = _chosen === "use"
            addEdit._proceedSave(forceNew, suppressOptionSet)
            _chosen = ""
        }
    }

    Dialog {
        id: deleteConfirm
        property var pendingCol: null
        title: qsTr("Delete Column")
        modal: true
        // See addEdit above for why we re-parent to Overlay.overlay.
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            text: deleteConfirm.pendingCol
                ? qsTr("Delete column “%1”?\n\nThis removes the column from %2 / %3 but leaves stored values in place — they'll re-appear as a discovered column.")
                    .arg(deleteConfirm.pendingCol.columnName)
                    .arg(deleteConfirm.pendingCol.sourceJobPath || qsTr("(no job)"))
                    .arg(deleteConfirm.pendingCol.sourceFolderName || qsTr("(no folder)"))
                : ""
            color: Theme.colors.text
            font.pixelSize: 12
            wrapMode: Text.Wrap
            width: 380
        }
        onAccepted: {
            if (pendingCol && pendingCol.columnId >= 0) {
                Columns.delete_column(pendingCol.columnId)
                root.changed()
            }
        }
    }

    // ── Duplicate-as-new-column dialog ─────────────────────────────
    // Replaces the locked rename. Forks the source column into a
    // new column on the same folder with a fresh template hash
    // (force_new). Optional metadata copy + source delete give the
    // user a one-step "actually rename" workflow when needed.
    Dialog {
        id: duplicateDialog
        property var pendingCol: null
        property int metadataValueCount: 0
        property alias newNameField: dupNameField
        property alias copyMetadataBox: dupCopyMetadata
        property alias deleteSourceBox: dupDeleteSource
        title: qsTr("Duplicate Column")
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        width: 480
        bottomPadding: 16
        standardButtons: Dialog.Ok | Dialog.Cancel

        ColumnLayout {
            width: 440
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: duplicateDialog.pendingCol
                    ? qsTr("Duplicate “%1” as a new column on this folder. The new column gets its own shared template — edits to it won't affect the original.")
                        .arg(duplicateDialog.pendingCol.columnName)
                    : ""
                color: Theme.colors.textMuted
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            GridLayout {
                columns: 2
                columnSpacing: 8
                rowSpacing: 6
                Layout.fillWidth: true

                Label { text: qsTr("New name"); color: Theme.colors.text }
                TextField {
                    id: dupNameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("e.g. QC Status, Reviewed By, …")
                    onAccepted: duplicateDialog.accept()
                }

                Label { text: qsTr("Type"); color: Theme.colors.text }
                Label {
                    text: duplicateDialog.pendingCol
                        ? duplicateDialog.pendingCol.columnType + qsTr(" (locked — same as source)")
                        : ""
                    color: Theme.colors.textMuted
                    font.italic: true
                    font.pixelSize: 11
                    Layout.fillWidth: true
                }
            }

            CheckBox {
                id: dupCopyMetadata
                checked: true
                text: duplicateDialog.metadataValueCount > 0
                    ? qsTr("Copy metadata values from “%1” to the new column (%2 items)")
                        .arg(duplicateDialog.pendingCol ? duplicateDialog.pendingCol.columnName : "")
                        .arg(duplicateDialog.metadataValueCount)
                    : qsTr("Copy metadata values from “%1” to the new column")
                        .arg(duplicateDialog.pendingCol ? duplicateDialog.pendingCol.columnName : "")
            }
            CheckBox {
                id: dupDeleteSource
                checked: false
                text: qsTr("Delete “%1” from this folder after copy")
                    .arg(duplicateDialog.pendingCol ? duplicateDialog.pendingCol.columnName : "")
            }
            Label {
                visible: dupDeleteSource.checked
                Layout.fillWidth: true
                text: qsTr("(The underlying template stays on the share. Other folders that use “%1” keep working.)")
                    .arg(duplicateDialog.pendingCol ? duplicateDialog.pendingCol.columnName : "")
                color: Theme.colors.textSubtle
                font.pixelSize: 10
                font.italic: true
                wrapMode: Text.Wrap
            }
        }

        onAccepted: {
            if (!pendingCol || pendingCol.columnId < 0) return
            var newName = dupNameField.text.trim()
            if (newName.length === 0) return
            var newId = Columns.duplicate_column(
                pendingCol.columnId,
                pendingCol.sourceJobPath,
                pendingCol.sourceFolderName,
                newName,
                dupCopyMetadata.checked,
                dupDeleteSource.checked)
            if (newId >= 0) {
                root.changed()
            }
        }
    }
}
