// ItemListPanel — left column of FolderTabView. Lists "items" (top-
// level subdirectories) inside a folder, excluding dot-folders and
// the legacy _t_* / 000000* template placeholders that ufb-tauri
// uses as project-template seeds.
//
// First cut: just the item name + modified date. Star toggle (track/
// untrack), per-folder metadata columns, due-date dots, and the
// per-row "Add Item" affordance all come with later slices alongside
// the project_config + columns wiring.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window  // Window.window → top-level openPathInBrowser()

import Ufb.Backend 1.0

import "ListingState.js" as ListingState

Rectangle {
    id: root

    /// Folder whose subdirectories form the item list.
    required property string folderPath
    /// Job path that owns this folder — needed for the Mark Tracked
    /// invokable. The host (FolderTabView → JobView) wires this through.
    property string jobPath: ""

    /// Path of the currently-highlighted item ("" if none). Two-way
    /// — host can set to drive selection from outside.
    property string selectedItemPath: ""

    /// Resizable Name column width (right-edge handle in the header).
    /// Modified is the last column and stays at a fixed pixel width.
    /// 240 default: generous room for shot names without an auto-fill
    /// scheme that would shove the metadata columns to the far right.
    /// Persisted per FOLDER in browser_folder_prefs (ilpNameColWidth
    /// field — distinct from the FileBrowser pane's nameColWidth so
    /// the two tables sharing one folder row don't fight). The
    /// metadata columns to the right persist separately in the
    /// Columns DB per (job, folder) as before.
    property real nameColWidth: 240

    /// Restore the folder's remembered Name width. Runs on folderPath
    /// changes; missing entry keeps the current width (inherit).
    function _restoreNameColWidth() {
        if (!folderPath || folderPath.length === 0) return
        try {
            var prefs = JSON.parse(Settings.folder_view_prefs(folderPath))
            if (prefs.ilpNameColWidth > 0) nameColWidth = prefs.ilpNameColWidth
        } catch (e) {}
    }
    /// Merge-save the Name width into the folder's prefs row (which
    /// FileBrowser panes / JobView activeSubtab also share).
    function _saveNameColWidth() {
        if (!folderPath || folderPath.length === 0) return
        var prefs = ({})
        try { prefs = JSON.parse(Settings.folder_view_prefs(folderPath)) } catch (e) { prefs = ({}) }
        prefs.ilpNameColWidth = Math.round(nameColWidth)
        Settings.set_folder_view_prefs(folderPath, JSON.stringify(prefs))
    }

    /// X (root coords) of the column-resize guide line, -1 when no
    /// header handle is being dragged. Handles track this during the
    /// drag instead of writing widths live — a live width write
    /// re-flows every visible row's Row per mouse move (visibly
    /// stuttery once metadata cells sit to the right); the real
    /// width commits once on release.
    property real _dragGuideX: -1

    /// Set of item paths (within this folder) that are currently
    /// tracked. Built inside _refreshMetadataAndColumns from the same
    /// folder_item_metadata pull that feeds the cells (per-(job,folder)
    /// query, no subscriptions dependency — tracked stars show for ANY
    /// browsed job, subscribed or not, and two jobs sharing a folder
    /// name can no longer cross-contaminate). Used for the row star +
    /// the menu item label flip.
    property var _trackedSet: ({})
    function _isTracked(path) { return _trackedSet[path] === true }
    Connections {
        target: Subscription
        function onTracked_items_jsonChanged() {
            // tracked_items_json fires whenever set_item_tracked
            // runs; re-pull our metadata cache (which also rebuilds
            // _trackedSet) so cells redraw with the new values.
            // (Untracked-item metadata edits go through
            // set_item_metadata_field which doesn't fire
            // tracked_items_json — those rely on the explicit refresh
            // call from the cell's onCommit handler + metadata_rev.)
            root._refreshMetadataAndColumns()
        }
        // Fires on every metadata write, including ones from other
        // panels (e.g. user edits a cell in TrackerView while this
        // ItemListPanel is also open). Keeps cell display + discovered
        // columns in sync without requiring a tab swap.
        function onMetadata_revChanged() { root._refreshMetadataAndColumns() }
    }
    Connections {
        target: Columns
        // Column DEFINITIONS changed — locally (Column Manager) or via a
        // mesh push (Sidebar calls Columns.invalidate on remote col_*).
        // Rebuild the column layout so new/edited columns appear live.
        function onColumns_revChanged() { root._refreshColumns() }
        // A background reconcile finished and wrote new options — repaint
        // cells WITHOUT reconciling again (false), so this can't loop.
        function onOptions_revChanged() { root._refreshColumns(false) }
    }
    // Merged with the existing onFolderPathChanged below — QML allows
    // only one handler per property.

    /// Fired on click. Host (FolderTabView) responds by re-targeting
    /// the right-side FileBrowser(s).
    signal itemSelected(string itemPath)
    signal itemActivated(string itemPath)  // double-click

    /// Add-Item dialog mode for the current folder. Re-evaluated
    /// whenever folderPath changes. One of "shot" | "date_prefixed" |
    /// "folder" | "none". Drives the Add button label / placeholder
    /// and hides the button entirely for "none".
    readonly property string _addMode: {
        if (folderPath.length === 0) return "folder"
        var name = _basename(folderPath)
        return FileOps.detect_add_mode(name)
    }
    function _basename(p) {
        var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
        return idx >= 0 ? p.substring(idx + 1) : p
    }
    function _joinPath(parent, name) {
        if (!parent) return name
        var sep = parent.indexOf("\\") >= 0 ? "\\" : "/"
        return parent + sep + name
    }
    function _formatYYMMDD(d) {
        var y = String(d.getFullYear() % 100).padStart(2, "0")
        var m = String(d.getMonth() + 1).padStart(2, "0")
        var dd = String(d.getDate()).padStart(2, "0")
        return y + m + dd
    }
    function _parentDir(p) {
        if (!p) return ""
        var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
        return idx > 0 ? p.substring(0, idx) : ""
    }
    function _fileName(p) {
        if (!p) return ""
        var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
        return idx >= 0 ? p.substring(idx + 1) : p
    }

    /// Backs the right-click menu — set on row press so each menu
    /// action can read which item the user targeted.
    property var _menuItem: null

    // ── Dynamic metadata columns ──────────────────────────────────────
    //
    // Mirrors the legacy ItemListPanel: each item row shows its
    // tracked-flag star, name, modified, plus columns the user has
    // configured for this (jobPath, folder) — plus auto-discovered
    // keys present in stored metadata but missing from the column
    // definitions (the legacy hid these completely; we surface them
    // as faded text columns and offer "Promote" in the manager
    // dialog).
    //
    // _metadataByPath is the lookup table for cell values. JS object
    // keyed by item path → parsed metadata blob.
    property var _metadataByPath: ({})
    /// Same shape as TrackerView.visibleColumns (see that file's
    /// _refreshColumns docstring).
    property var visibleColumns: []

    /// Column-width overrides keyed by columnName (same shape as
    /// TrackerView's). The handle's release writes the dragged width
    /// here so rows repaint immediately, then _commitColumnWidth
    /// persists via Columns.update_column and clears the entry.
    /// During the drag itself only the guide line moves — see
    /// _dragGuideX.
    property var _columnWidthOverrides: ({})
    function _setLiveColumnWidth(name, w) {
        var m = Object.assign({}, _columnWidthOverrides)
        m[name] = Math.max(40, w)
        _columnWidthOverrides = m
    }
    function _commitColumnWidth(col) {
        if (!col) return
        var override = _columnWidthOverrides[col.columnName]
        if (override === undefined) return
        if (col.columnId >= 0 && !col.discovered) {
            Columns.set_column_width(
                col.columnId, col.sourceJobPath, col.sourceFolderName,
                override)
        }
        var m = Object.assign({}, _columnWidthOverrides)
        delete m[col.columnName]
        _columnWidthOverrides = m
        _refreshMetadataAndColumns()
    }

    function _refreshMetadataAndColumns() {
        console.log("[ItemListPanel] _refreshMetadataAndColumns fired job="
            + jobPath + " folder=" + folderPath)
        // Load all items' metadata for this folder in one go. The same
        // records carry isTracked, so the star set rebuilds here too.
        _metadataByPath = ({})
        _trackedSet = ({})
        if (jobPath.length > 0 && folderPath.length > 0) {
            var folderName = _basename(folderPath)
            var arr = []
            try {
                arr = JSON.parse(Subscription.folder_item_metadata(jobPath, folderName))
            } catch (e) { arr = [] }
            var byPath = {}
            var trackedSet = {}
            for (var i = 0; i < arr.length; ++i) {
                var rec = arr[i]
                if (!rec.itemPath) continue
                try {
                    byPath[rec.itemPath] = JSON.parse(rec.metadataJson || "{}")
                } catch (e) {
                    byPath[rec.itemPath] = {}
                }
                if (rec.isTracked) trackedSet[rec.itemPath] = true
            }
            _metadataByPath = byPath
            _trackedSet = trackedSet
        }
        _refreshColumns()
        // If the active sort is by a metadata column, the new
        // metadata may have changed row order — re-run refresh()
        // so the rows re-sort. Cheap when sort is off.
        if (_sortKey.length > 0 && _sortDir !== 0
            && _sortKey !== "name" && _sortKey !== "modified") {
            refresh()
        }
    }

    // doReconcile (default true): kick the background template→options
    // reconcile. Pass false from the options_rev handler so re-rendering
    // after a reconcile completes doesn't trigger another reconcile —
    // that separation is what keeps the now-async reconcile loop-free.
    function _refreshColumns(doReconcile) {
        if (doReconcile === undefined) doReconcile = true
        console.log("[ItemListPanel] _refreshColumns fired job="
            + jobPath + " folder=" + folderPath)
        if (jobPath.length === 0 || folderPath.length === 0) {
            visibleColumns = []
            return
        }
        var folderName = _basename(folderPath)
        // Pull the authoritative option lists from the template files
        // on the share before reading column_options. Catches the case
        // where a peer's broadcast got LWW-rejected (back-to-back
        // editor-save timestamp collision) or where this peer was
        // offline when the option was added. Now async/non-blocking —
        // returns immediately and re-renders via onOptions_revChanged.
        if (doReconcile)
            try { Columns.reconcile_options_from_templates(jobPath, folderName) } catch (e) {}
        var configured = []
        var configuredNames = {}
        var defs = []
        try { defs = JSON.parse(Columns.get_column_defs(jobPath, folderName)) }
        catch (e) {}
        for (var d = 0; d < defs.length; ++d) {
            var col = defs[d]
            var name = col.columnName || ""
            // Mark configured-name BEFORE the visibility skip — a
            // hidden column is still "configured", not orphaned.
            // Otherwise the discovered-keys loop below would see the
            // hidden column's metadata key and re-surface it as a
            // discovered column (defeating the hide).
            configuredNames[name] = true
            if (col.isVisible === false) continue
            // 0.9.97: hide unpromoted columns from the grid. They
            // remain visible in the Column Manager so the user can
            // promote them or send them to Trash.
            if (!col.templateHash) continue
            configured.push({
                columnId: typeof col.id === "number" ? col.id : -1,
                columnName: name,
                columnType: col.columnType || "text",
                columnOrder: typeof col.columnOrder === "number" ? col.columnOrder : 0,
                columnWidth: typeof col.columnWidth === "number" && col.columnWidth > 0
                    ? col.columnWidth : 110,
                sourceJobPath: jobPath,
                sourceFolderName: folderName,
                discovered: false,
                options: Array.isArray(col.options) ? col.options : []
            })
        }
        configured.sort(function(a, b) { return a.columnOrder - b.columnOrder })

        // 0.9.97 — discovered-keys pass removed from the main grid.
        // It was surfacing orphan item_metadata keys (cross-OS path
        // mismatch + freshly-unpromoted columns) as italic columns,
        // which the user could neither edit nor delete. Discovered
        // keys still appear in the Column Manager's view so the user
        // can promote / clean them up; the file browser grid now
        // shows only fully-promoted, non-trashed columns.
        visibleColumns = configured
        console.log("[ItemListPanel] _refreshColumns done — visibleColumns.length="
            + visibleColumns.length
            + " names=" + visibleColumns.map(c => c.columnName).join(","))
    }

    function _cellValue(meta, col) {
        if (!meta || !col) return ""
        var raw = meta[col.columnName]
        if (raw === undefined || raw === null) return ""
        if (col.columnType === "checkbox") return raw ? "✓" : ""
        if (col.columnType === "date" && typeof raw === "number") return formatDate(raw)
        return String(raw)
    }

    // No root border — ItemListPanel sits inside a SplitView whose
    // splitter is the separator from the FileBrowser pane on the right.
    // Header strip + column header + list each carry their own surface
    // tone for differentiation; the panel root just provides the
    // bg-color floor (visible only at panel edges with margins:0).
    color: Theme.colors.bg

    /// Per-instance Directory used purely to list this folder. Its
    /// `current_path` is bound to folderPath so changing folderPath
    /// triggers a re-list.
    Directory { id: itemsProbe }

    ListModel { id: itemsModel }

    function _isTemplateName(name) {
        // Legacy project-template seed folders (per ufb-tauri
        // ItemListPanel.tsx:43–47).
        if (name.startsWith("_t_")) return true
        if (name.startsWith("000000")) return true
        return false
    }

    /// Sort state — same idiom as TrackerView. _sortKey is "name" |
    /// "modified" | "<columnName>"; _sortDir 1=asc, -1=desc, 0=off.
    property string _sortKey: ""
    property int _sortDir: 0
    function _toggleSort(key) {
        if (_sortKey === key) {
            _sortDir = _sortDir === 1 ? -1 : (_sortDir === -1 ? 0 : 1)
            if (_sortDir === 0) _sortKey = ""
        } else {
            _sortKey = key
            _sortDir = 1
        }
        refresh()
    }
    function _sortGlyph(key) {
        if (_sortKey !== key || _sortDir === 0) return ""
        return _sortDir === 1 ? " ▲" : " ▼"
    }
    function _compareItems(a, b) {
        var key = root._sortKey
        var av, bv
        if (key === "name") { av = a.name; bv = b.name }
        else if (key === "modified") { av = a.modified; bv = b.modified }
        else {
            // Metadata column lookup — _metadataByPath is keyed by
            // item path so the comparator stays cheap (no per-row JSON
            // parse).
            var ma = root._metadataByPath[a.path] || {}
            var mb = root._metadataByPath[b.path] || {}
            av = ma[key]
            bv = mb[key]
        }
        var aEmpty = (av === undefined || av === null || av === "")
        var bEmpty = (bv === undefined || bv === null || bv === "")
        if (aEmpty && bEmpty) return 0
        if (aEmpty) return root._sortDir
        if (bEmpty) return -root._sortDir
        if (typeof av === "number" && typeof bv === "number") {
            return av < bv ? -1 : (av > bv ? 1 : 0)
        }
        var as = String(av).toLowerCase()
        var bs = String(bv).toLowerCase()
        return as < bs ? -1 : (as > bs ? 1 : 0)
    }

    function refresh() {
        itemsModel.clear()
        if (!itemsProbe.entries_json) return
        try {
            var arr = JSON.parse(itemsProbe.entries_json)
            var rows = []
            for (var i = 0; i < arr.length; ++i) {
                var e = arr[i]
                if (!e.isDir) continue
                if (e.name.startsWith(".")) continue
                if (_isTemplateName(e.name)) continue
                rows.push({
                    name: e.name,
                    path: e.path,
                    modified: typeof e.modified === "number" ? e.modified : 0
                })
            }
            if (root._sortKey.length > 0 && root._sortDir !== 0) {
                rows.sort(function(a, b) {
                    return root._sortDir * root._compareItems(a, b)
                })
            }
            for (var j = 0; j < rows.length; ++j) {
                itemsModel.append(rows[j])
            }
        } catch (e) {
            console.warn("ItemListPanel: parse failed:", e)
        }
    }

    /// Public refresh entry point used by the host's Refresh button.
    /// Re-reads the directory from disk, refetches tracked-set state,
    /// and reloads the metadata cache. The disk re-read fires
    /// `entries_json` change → `refresh()` rebuilds the row model.
    function refreshAll() {
        if (folderPath.length > 0) itemsProbe.refresh()
        _refreshMetadataAndColumns()
    }

    onFolderPathChanged: {
        if (folderPath.length > 0) {
            itemsProbe.navigate_to(folderPath)
        } else {
            itemsModel.clear()
        }
        _refreshMetadataAndColumns()
        _restoreNameColWidth()
    }
    onJobPathChanged: _refreshMetadataAndColumns()
    Component.onCompleted: {
        if (folderPath.length > 0) itemsProbe.navigate_to(folderPath)
        _refreshMetadataAndColumns()
        _restoreNameColWidth()
    }
    Connections {
        target: itemsProbe
        function onEntries_jsonChanged() { root.refresh() }
    }

    // Refresh when a file operation (in this or any other view) changes
    // this panel's folder. itemsModel selection is path-based and
    // survives the rebuild, so a plain re-probe is enough.
    Connections {
        target: FileOps
        function onDirs_changed(dirsJson) {
            if (!root.folderPath || root.folderPath.length === 0) return
            var dirs
            try { dirs = JSON.parse(dirsJson) } catch (e) { return }
            if (!dirs || dirs.length === 0) return
            if (ListingState.dirInSet(root.folderPath, dirs)) {
                itemsProbe.refresh()
            }
        }
    }

    // Column-resize guide — tracks the proposed column edge during a
    // header width drag (see _dragGuideX). Sibling of the ColumnLayout
    // with a high z so it draws over the header + list.
    Rectangle {
        visible: root._dragGuideX >= 0
        x: root._dragGuideX
        y: columnHeaderBar.y
        height: root.height - columnHeaderBar.y
        width: 2
        color: Theme.colors.accent
        z: 100
    }

    ColumnLayout {
        anchors.fill: parent
        // Flush stack — no margins or spacing. Each child Rectangle's
        // own fill colour (toolbar / toolbarAlt / surface) provides the
        // visual separation; gaps would re-introduce the "phantom
        // border" the borders themselves used to draw.
        spacing: 0

        // ── Header strip ──────────────────────────────────────────────
        // Filled bar, no border. The 1px separation from the column-
        // header strip below comes from the surface tone change
        // (toolbar → toolbarAlt) — sister-app idiom.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            color: Theme.colors.toolbar
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 0
                spacing: 0
                Label {
                    text: qsTr("Items (%1)").arg(itemsModel.count)
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                    font.bold: true
                    Layout.fillWidth: true
                }
                BusyIndicator {
                    visible: itemsProbe.loading
                    running: visible
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    Layout.alignment: Qt.AlignVCenter
                }
                FlatButton {
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    Layout.alignment: Qt.AlignVCenter
                    iconName: root._sortDir === 1
                        ? "sort-ascending"
                        : (root._sortDir === -1 ? "sort-descending" : "funnel")
                    text: {
                        if (root._sortKey === "name")     return qsTr("Name")
                        if (root._sortKey === "modified") return qsTr("Modified")
                        if (root._sortKey.length > 0)     return root._sortKey
                        return qsTr("Sort")
                    }
                    tooltip: qsTr("Sort items")
                    onClicked: itemSortMenu.popup(0, height)
                    UfbMenu {
                        id: itemSortMenu
                        y: parent.height
                        MenuItem {
                            text: qsTr("Name") + (root._sortKey === "name"
                                ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                            onTriggered: root._toggleSort("name")
                        }
                        MenuItem {
                            text: qsTr("Modified") + (root._sortKey === "modified"
                                ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                            onTriggered: root._toggleSort("modified")
                        }
                        MenuSeparator { visible: root.visibleColumns.length > 0 }
                        Instantiator {
                            model: root.visibleColumns
                            onObjectAdded: (idx, obj) => itemSortMenu.insertItem(idx + 3, obj)
                            onObjectRemoved: (idx, obj) => itemSortMenu.removeItem(obj)
                            delegate: MenuItem {
                                required property var modelData
                                text: (modelData ? modelData.columnName : "")
                                    + (modelData && root._sortKey === modelData.columnName
                                        ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                                onTriggered: {
                                    if (modelData) root._toggleSort(modelData.columnName)
                                }
                            }
                        }
                        MenuSeparator {}
                        MenuItem {
                            text: qsTr("Clear sort")
                            enabled: root._sortKey.length > 0
                            onTriggered: {
                                root._sortKey = ""
                                root._sortDir = 0
                                root.refresh()
                            }
                        }
                    }
                }
            }
        }

        // ── Column header row (mirrors the row delegate's grid) ──────
        // Filled tonal bar — relies on toolbarAlt vs toolbar contrast
        // for separation rather than its own border. Matches MinRender's
        // JobListPanel column-header pattern.
        Rectangle {
            id: columnHeaderBar
            Layout.fillWidth: true
            Layout.preferredHeight: 20
            color: Theme.colors.toolbarAlt
            // Always shown (not gated on visibleColumns) so the Name
            // resize handle + Name/Modified sort toggles work even in
            // folders with no metadata columns configured.
            Row {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8 + Theme.dim.scrollBarWidth
                spacing: 6
                // Star (22) + row spacing (6) → name column leads here.
                Item { width: 22; height: parent.height }
                Item {
                    width: root.nameColWidth
                    height: parent.height
                    Label {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Name") + root._sortGlyph("name")
                        color: Theme.colors.textMuted
                        font.pixelSize: Theme.font.sizeSmall
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._toggleSort("name")
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.topMargin: 4
                        anchors.bottomMargin: 4
                        width: 2
                        color: itemNameHandleMa.containsMouse || itemNameHandleMa.pressed
                            ? Theme.colors.accent : Theme.colors.borderStrong
                        z: 10
                    }
                    MouseArea {
                        id: itemNameHandleMa
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.rightMargin: -3
                        width: 10
                        hoverEnabled: true
                        cursorShape: Qt.SizeHorCursor
                        preventStealing: true
                        z: 11
                        property real _startMouseX: 0
                        property real _startWidth: 0
                        property real _dragWidth: -1
                        onPressed: (mouse) => {
                            _startMouseX = mouse.x
                            _startWidth = root.nameColWidth
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed) return
                            _dragWidth = Math.max(60,
                                _startWidth + (mouse.x - _startMouseX))
                            root._dragGuideX =
                                parent.mapToItem(root, _dragWidth, 0).x
                        }
                        onReleased: {
                            if (_dragWidth >= 0)
                                root.nameColWidth = _dragWidth
                            _dragWidth = -1
                            root._dragGuideX = -1
                        }
                        onCanceled: {
                            _dragWidth = -1
                            root._dragGuideX = -1
                        }
                    }
                }
                Repeater {
                    model: root.visibleColumns
                    delegate: Item {
                        id: itemHeaderCell
                        width: {
                            if (!modelData) return 0
                            var ov = root._columnWidthOverrides[modelData.columnName]
                            return ov !== undefined ? ov : modelData.columnWidth
                        }
                        height: parent.height
                        Label {
                            anchors.left: parent.left
                            anchors.right: itemHeaderHandle.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                                ? (modelData.discovered
                                    ? modelData.columnName + "  ⓘ"
                                    : modelData.columnName)
                                  + root._sortGlyph(modelData ? modelData.columnName : "")
                                : ""
                            color: modelData && modelData.discovered
                                ? Theme.colors.textSubtle
                                : Theme.colors.textMuted
                            font.pixelSize: Theme.font.sizeSmall
                            font.bold: !(modelData && modelData.discovered)
                            font.italic: modelData && modelData.discovered
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            anchors.left: parent.left
                            anchors.right: itemHeaderHandle.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (modelData) root._toggleSort(modelData.columnName)
                            }
                        }
                        Rectangle {
                            id: itemHeaderHandle
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.topMargin: 4
                            anchors.bottomMargin: 4
                            width: 2
                            color: itemHeaderHandleMa.containsMouse || itemHeaderHandleMa.pressed
                                ? Theme.colors.accent : Theme.colors.borderStrong
                            z: 10
                        }
                        MouseArea {
                            id: itemHeaderHandleMa
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.rightMargin: -3
                            width: 10
                            hoverEnabled: true
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            z: 11
                            property real _startMouseX: 0
                            property real _startWidth: 0
                            property real _dragWidth: -1
                            onPressed: (mouse) => {
                                _startMouseX = mouse.x
                                _startWidth = itemHeaderCell.width
                            }
                            onPositionChanged: (mouse) => {
                                if (!pressed) return
                                _dragWidth = Math.max(40,
                                    _startWidth + (mouse.x - _startMouseX))
                                root._dragGuideX = itemHeaderCell
                                    .mapToItem(root, _dragWidth, 0).x
                            }
                            onReleased: {
                                if (_dragWidth >= 0) {
                                    root._setLiveColumnWidth(
                                        modelData.columnName, _dragWidth)
                                    root._commitColumnWidth(modelData)
                                }
                                _dragWidth = -1
                                root._dragGuideX = -1
                            }
                            onCanceled: {
                                _dragWidth = -1
                                root._dragGuideX = -1
                            }
                        }
                    }
                }
                Item {
                    width: 80
                    height: parent.height
                    Label {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        text: qsTr("Modified") + root._sortGlyph("modified")
                        color: Theme.colors.textMuted
                        font.pixelSize: Theme.font.sizeSmall
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._toggleSort("modified")
                    }
                }
            }
        }

        // ── Item list ─────────────────────────────────────────────────
        // No border — the panel root already has one, and a second
        // border on the inner list would re-stack chrome. Surface tone
        // gives the list area its own subtle lift from the bg root.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.surface

            ListView {
                id: itemView
                anchors.fill: parent
                model: itemsModel
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: UfbScrollBar {}

                delegate: Rectangle {
                    id: itemRow
                    // Capture model roles as explicit row properties.
                    // Inside the inner CellEditor Repeater below,
                    // `model` shadows to visibleColumns — without
                    // these promoted handles, model.path would resolve
                    // to undefined (the columns model has no `path`).
                    required property string path
                    required property string name
                    required property real modified
                    // Required-property delegates don't get implicit
                    // context injection — index must be explicit for
                    // the zebra striping below.
                    required property int index
                    width: itemView.width
                    height: 26
                    property bool selected: itemRow.path === root.selectedItemPath
                    color: selected
                        ? Theme.colors.accentSelected
                        : rowMa.containsMouse
                        ? Theme.colors.surfaceHover
                        : (index % 2 === 1 ? Theme.colors.surfaceAlt : "transparent")
                    /// Per-row metadata, looked up from the bulk-fetch
                    /// table on root. Recomputed when _metadataByPath
                    /// changes (re-evaluation triggered by the parent
                    /// onChanged binding).
                    readonly property var rowMetadata: {
                        var m = root._metadataByPath[itemRow.path]
                        return m || {}
                    }

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8 + Theme.dim.scrollBarWidth
                        spacing: 6

                        // Star indicator + click target. Yellow when
                        // tracked, dim outline otherwise. Click toggles.
                        Item {
                            width: 22
                            height: parent.height
                            anchors.verticalCenter: parent.verticalCenter
                            Label {
                                anchors.centerIn: parent
                                text: root._isTracked(itemRow.path) ? "★" : "☆"
                                color: root._isTracked(itemRow.path)
                                    ? "#e6c84a" // tracked-star yellow — no theme token, intentional brand
                                    : (starMa.containsMouse ? Theme.colors.textMuted : Theme.colors.borderStrong)
                                font.pixelSize: Theme.font.sizeHeading
                            }
                            MouseArea {
                                id: starMa
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    var folderName = root._basename(root.folderPath)
                                    Subscription.set_item_tracked(
                                        root.jobPath,
                                        itemRow.path,
                                        folderName,
                                        !root._isTracked(itemRow.path))
                                }
                            }
                        }
                        Label {
                            width: root.nameColWidth
                            text: itemRow.name
                            color: itemRow.selected ? Theme.colors.textBright : Theme.colors.text
                            font.pixelSize: Theme.font.sizeBody
                            elide: Text.ElideRight
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Repeater {
                            model: root.visibleColumns
                            delegate: CellEditor {
                                // Live-width override mirrors the
                                // header so dragging stays aligned.
                                width: {
                                    if (!modelData) return 0
                                    var ov = root._columnWidthOverrides[modelData.columnName]
                                    return ov !== undefined ? ov : modelData.columnWidth
                                }
                                height: parent.height
                                col: modelData
                                value: (modelData && itemRow.rowMetadata)
                                    ? itemRow.rowMetadata[modelData.columnName]
                                    : null
                                selected: itemRow.selected
                                onCommit: (newValue) => {
                                    // `root` can be gone when the commit
                                    // fires during panel teardown (tab
                                    // close mid-edit) — the editor's
                                    // popup outlives the delegate scope.
                                    if (!modelData || !root) return
                                    // Use itemRow.path (promoted from
                                    // model role) — `model` inside
                                    // this inner Repeater shadows to
                                    // visibleColumns, where there's
                                    // no `path` role.
                                    Subscription.set_item_metadata_field(
                                        root.jobPath,
                                        itemRow.path,
                                        root._basename(root.folderPath),
                                        modelData.columnName,
                                        JSON.stringify(newValue),
                                        root._isTracked(itemRow.path))
                                    // Explicit local refresh — Subscription
                                    // only fires tracked_items_json when an
                                    // item's tracked-status actually changes,
                                    // so untracked-item metadata edits would
                                    // otherwise leave our cell display stale
                                    // (we've seen the data after a tab swap
                                    // proves the DB write landed).
                                    root._refreshMetadataAndColumns()
                                }
                            }
                        }
                        Label {
                            width: 80
                            text: root.formatDate(itemRow.modified)
                            color: itemRow.selected ? Theme.colors.textBright : Theme.colors.textSubtle
                            font.pixelSize: Theme.font.sizeTiny
                            font.family: Theme.font.mono
                            horizontalAlignment: Text.AlignRight
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    // Row-wide click target. Sits BEHIND the Row's
                    // cells so per-cell MouseAreas (in CellEditor) get
                    // first crack at left clicks for in-place edit
                    // mode. Clicks on non-cell areas (star, folder
                    // icon, name, modified) fall through here for row
                    // selection / right-click menu / double-click
                    // activation.
                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        z: -1
                        onPressed: (mouse) => {
                            root.selectedItemPath = itemRow.path
                            root.itemSelected(itemRow.path)
                            if (mouse.button === Qt.RightButton) {
                                root._menuItem = {
                                    path: itemRow.path,
                                    name: itemRow.name
                                }
                                itemMenu.popup()
                            }
                        }
                        onDoubleClicked: (mouse) => {
                            if (mouse.button === Qt.LeftButton) {
                                root.itemActivated(itemRow.path)
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: itemsModel.count === 0
                        && !itemsProbe.loading
                        && itemsProbe.error.length === 0
                    text: qsTr("(no items)")
                    color: Theme.colors.textSubtle
                    font.pixelSize: Theme.font.sizeSmall
                    font.italic: true
                }
                Label {
                    anchors.centerIn: parent
                    visible: itemsProbe.error.length > 0
                    text: itemsProbe.error
                    color: Theme.colors.error
                    font.pixelSize: Theme.font.sizeSmall
                    wrapMode: Text.Wrap
                    width: parent.width - 24
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        // ── Bottom action row ────────────────────────────────────────
        // Full-flush FlatButtons: Add Item + Manage Metadata. Tonal
        // `toolbar` fill matches the FileBrowser pane footer — both
        // action strips read as the same kind of element.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            color: Theme.colors.toolbar
            RowLayout {
                anchors.fill: parent
                spacing: 0
                FlatButton {
                    visible: root._addMode !== "none" && root.folderPath.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: "plus"
                    text: {
                        switch (root._addMode) {
                        case "shot":          return qsTr("Add Shot…")
                        case "date_prefixed": return qsTr("Add Item…")
                        default:              return qsTr("Add Folder…")
                        }
                    }
                    onClicked: addDialog.openFor(root._addMode, root.folderPath)
                }
                FlatButton {
                    visible: root.jobPath.length > 0 && root.folderPath.length > 0
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: "table"
                    text: qsTr("Manage Metadata")
                    tooltip: qsTr("Add / edit / delete metadata columns + promote discovered keys")
                    onClicked: {
                        // Build the metadata-blobs list from our cache so
                        // the dialog can surface orphan keys.
                        var blobs = []
                        for (var p in root._metadataByPath) {
                            try { blobs.push(JSON.stringify(root._metadataByPath[p])) }
                            catch (e) {}
                        }
                        columnManager.jobPath = root.jobPath
                        columnManager.folderName = root._basename(root.folderPath)
                        columnManager.metadataBlobs = blobs
                        columnManager.open()
                    }
                }
            }
        }
    }

    ColumnManagerDialog {
        id: columnManager
        // Refresh once when the user dismisses the dialog. Per-
        // toggle live refresh was attempted via a custom signal but
        // QML silently swallowed the connection (Dialog-rooted
        // component); the built-in `closed` signal works and the
        // close-time refresh is good enough UX.
        onClosed: root._refreshMetadataAndColumns()
    }

    // ── Right-click context menu ─────────────────────────────────────
    UfbMenu {
        id: itemMenu
        MenuItem {
            text: root._menuItem && root._isTracked(root._menuItem.path)
                ? qsTr("Untrack")
                : qsTr("Mark as Tracked")
            onTriggered: {
                if (!root._menuItem) return
                var folderName = root._basename(root.folderPath)
                Subscription.set_item_tracked(
                    root.jobPath,
                    root._menuItem.path,
                    folderName,
                    !root._isTracked(root._menuItem.path))
            }
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Open in Left Browser")
            onTriggered: {
                if (root._menuItem && Window.window && Window.window.openPathInBrowser)
                    Window.window.openPathInBrowser("left", root._menuItem.path)
            }
        }
        MenuItem {
            text: qsTr("Open in Right Browser")
            onTriggered: {
                if (root._menuItem && Window.window && Window.window.openPathInBrowser)
                    Window.window.openPathInBrowser("right", root._menuItem.path)
            }
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Reveal in Explorer")
            onTriggered: {
                if (root._menuItem) FileOps.reveal_in_file_manager(root._menuItem.path)
            }
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Cut")
            onTriggered: {
                if (root._menuItem) {
                    FileOps.clipboard_cut_paths(JSON.stringify([root._menuItem.path]))
                }
            }
        }
        MenuItem {
            text: qsTr("Copy")
            onTriggered: {
                if (root._menuItem) {
                    FileOps.clipboard_copy_paths(JSON.stringify([root._menuItem.path]))
                }
            }
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Rename")
            onTriggered: {
                if (root._menuItem) renameDialog.openFor(root._menuItem.path)
            }
        }
        MenuItem {
            text: qsTr("Delete")
            onTriggered: {
                if (root._menuItem) deleteConfirm.openFor(root._menuItem.path)
            }
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Copy Path")
            onTriggered: {
                if (root._menuItem) FileOps.clipboard_copy_text(root._menuItem.path)
            }
        }
        MenuItem {
            text: qsTr("Copy ufb:// Link")
            onTriggered: {
                if (root._menuItem) FileOps.clipboard_copy_text(FileOps.build_ufb_uri(root._menuItem.path))
            }
        }
        MenuItem {
            text: qsTr("Copy union:// Link")
            onTriggered: {
                if (root._menuItem) FileOps.clipboard_copy_text(FileOps.build_union_uri(root._menuItem.path))
            }
        }
    }

    // ── Rename dialog ───────────────────────────────────────────────
    Dialog {
        id: renameDialog
        property string oldPath: ""
        title: qsTr("Rename Item")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(path) {
            oldPath = path
            renameField.text = root._fileName(path)
            open()
            renameField.forceActiveFocus()
            renameField.selectAll()
        }
        ColumnLayout {
            spacing: 8
            Label {
                text: qsTr("Rename:\n%1").arg(renameDialog.oldPath)
                color: "#aaa"
                font.pixelSize: 11
            }
            TextField {
                id: renameField
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                onAccepted: renameDialog.accept()
            }
        }
        onAccepted: {
            var newName = renameField.text.trim()
            if (newName.length === 0 || newName === root._fileName(oldPath)) return
            var newPath = root._joinPath(root._parentDir(oldPath), newName)
            var err = FileOps.rename_path(oldPath, newPath)
            if (err.length > 0) {
                console.warn("ItemListPanel rename failed:", err)
                return
            }
            // Selection follows the renamed item.
            if (root.selectedItemPath === oldPath) {
                root.selectedItemPath = newPath
                root.itemSelected(newPath)
            }
            itemsProbe.refresh()
        }
    }

    // ── Delete confirm ──────────────────────────────────────────────
    Dialog {
        id: deleteConfirm
        property string itemPath: ""
        title: qsTr("Delete Item")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(path) {
            itemPath = path
            open()
        }
        Label {
            text: qsTr("Move to recycle bin?\n%1").arg(deleteConfirm.itemPath)
            color: "#dddddd"
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        onAccepted: {
            FileOps.delete_to_trash(JSON.stringify([itemPath]))
            // Clear selection if the deleted item was selected.
            if (root.selectedItemPath === itemPath) {
                root.selectedItemPath = ""
            }
            // Delete is async; refresh after a beat to give it time to land.
            deleteRefreshTimer.restart()
        }
    }
    Timer {
        id: deleteRefreshTimer
        interval: 350
        onTriggered: itemsProbe.refresh()
    }

    // ── Add Item dialog ─────────────────────────────────────────────
    // Single dialog parameterised by mode so we don't define three
    // near-identical dialogs. The accept handler dispatches per mode.
    Dialog {
        id: addDialog
        property string mode: "folder"
        property string parentPath: ""
        title: {
            switch (mode) {
            case "shot":          return qsTr("New Shot")
            case "date_prefixed": return qsTr("New Item")
            default:              return qsTr("New Folder")
            }
        }
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(mode, parent) {
            addDialog.mode = mode
            addDialog.parentPath = parent
            addNameField.text = ""
            open()
            addNameField.forceActiveFocus()
        }
        ColumnLayout {
            spacing: 8
            Label {
                text: {
                    var msg
                    switch (addDialog.mode) {
                    case "shot":          msg = qsTr("Create shot in:\n%1")
                                          break
                    case "date_prefixed": msg = qsTr("Create date-prefixed item in:\n%1")
                                          break
                    default:              msg = qsTr("Create folder in:\n%1")
                    }
                    return msg.arg(addDialog.parentPath)
                }
                color: "#aaa"
                font.pixelSize: 11
            }
            // Live name preview for date-prefixed mode. Shows the
            // shape `YYMMDDa_<typed>` as the user types, matching the
            // legacy app's modal hint. The "a" is approximate — the
            // backend picks the actual next available letter at submit
            // time, which can differ if other items get added in
            // between.
            Label {
                visible: addDialog.mode === "date_prefixed"
                text: {
                    var typed = addNameField.text.trim()
                    var stem = typed.length > 0 ? typed : qsTr("name")
                    return qsTr("Will create: %1a_%2")
                        .arg(root._formatYYMMDD(new Date()))
                        .arg(stem)
                }
                color: Theme.colors.accent
                font.pixelSize: 11
                font.italic: true
            }
            TextField {
                id: addNameField
                Layout.fillWidth: true
                Layout.preferredWidth: 360
                placeholderText: {
                    switch (addDialog.mode) {
                    case "shot":          return qsTr("Shot name")
                    case "date_prefixed": return qsTr("Item base name")
                    default:              return qsTr("Folder name")
                    }
                }
                onAccepted: addDialog.accept()
            }
        }
        onAccepted: {
            var name = addNameField.text.trim()
            if (name.length === 0) return

            var created = ""

            // Shot mode: try to find a bundled template for this
            // folder type and copy it. Falls back to plain mkdir if
            // no matching template ships with the build.
            if (addDialog.mode === "shot") {
                var folderName = root._basename(root.folderPath)
                var template = FileOps.resolve_shot_template(folderName)
                if (template.length > 0) {
                    var copied = FileOps.copy_template_to(template, addDialog.parentPath, name)
                    if (copied.length === 0) {
                        // copy_template_to logged its own error — just
                        // surface the failure here without a plain-mkdir
                        // fallback (would mask the issue).
                        console.warn("Add Shot from template failed for", name)
                        return
                    }
                    created = copied
                }
                // No template for this folder type → fall through to mkdir.
            }

            if (created.length === 0 && addDialog.mode === "date_prefixed") {
                created = FileOps.create_date_prefixed_item(addDialog.parentPath, name)
                if (created.length === 0) {
                    console.warn("Add date-prefixed item failed for", name)
                    return
                }
            }

            if (created.length === 0) {
                var newPath = root._joinPath(addDialog.parentPath, name)
                var err = FileOps.create_directory(newPath)
                if (err.length > 0) {
                    console.warn("Add Item failed:", err)
                    return
                }
                created = newPath
            }

            // Match legacy behavior: select + open the new item so the
            // right-side browser navigates straight into it. The probe
            // refresh is async — the listing might not include the new
            // row yet by the time we set selectedItemPath, but the
            // ItemListPanel's selected-row binding is path-based, so
            // it'll latch onto the row as soon as the refresh lands.
            itemsProbe.refresh()
            root.selectedItemPath = created
            root.itemSelected(created)
        }
    }

    function formatDate(epochMs) {
        if (!epochMs) return ""
        var d = new Date(epochMs)
        return d.toLocaleString(Qt.locale(), "yyyy-MM-dd")
    }
}
