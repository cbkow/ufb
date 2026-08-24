// TrackerView — table-styled list of tracked items with dynamic
// metadata columns + column management.
//
// Phase 11 slices A/B/C: read-only display of values, dynamic
// columns from ColumnDefinitions, plus auto-discovered orphan
// metadata keys surfaced as "discovered" columns. Header right-
// click manages columns: Edit / Hide / Delete / Add / Promote
// (orphan → real column).
//
// Inline cell editing per type, sort, filter chips, presets, bulk-
// edit all still defer to later slices.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window  // Window.window → top-level openPathInBrowser()

import Ufb.Backend 1.0

Rectangle {
    id: root

    /// Job to scope the view to. "" = aggregated mode (all jobs).
    required property string jobPath
    required property string jobName

    /// Fired when the user clicks a tracked row.
    signal goToItemRequested(string jobPath, string jobName, string folderName, string itemPath)

    /// Active due-date filter chip. One of:
    ///   "" (all) | "overdue" | "today" | "week" | "month"
    /// Filters trackedModel rows to those whose any date-typed
    /// metadata column matches the bucket. "" means show everything.
    property string _dueFilter: ""
    function _matchesDueFilter(row) {
        if (_dueFilter.length === 0) return true
        var meta = {}
        try { meta = JSON.parse(row.metadataJsonStr || "{}") } catch (e) {}
        // Walk every visible date-typed column on this row. Any match
        // counts — a row with two date columns where either one is
        // "due today" passes the today filter.
        var now = new Date()
        var startOfToday = new Date(now.getFullYear(), now.getMonth(), now.getDate())
        var startOfTomorrow = new Date(startOfToday.getTime() + 24 * 3600 * 1000)
        var endOfWeek = new Date(startOfToday.getTime() + 7 * 24 * 3600 * 1000)
        var endOfMonth = new Date(startOfToday.getFullYear(),
            startOfToday.getMonth() + 1, startOfToday.getDate())
        for (var i = 0; i < visibleColumns.length; ++i) {
            var col = visibleColumns[i]
            var t = String(col.columnType || "").toLowerCase()
            if (t !== "date") continue
            var raw = meta[col.columnName]
            if (raw === undefined || raw === null || raw === "") continue
            var d
            if (typeof raw === "number") d = new Date(raw)
            else d = new Date(String(raw))
            if (isNaN(d.getTime())) continue
            switch (_dueFilter) {
            case "overdue": if (d.getTime() < startOfToday.getTime())  return true; break
            case "today":   if (d.getTime() >= startOfToday.getTime()
                                && d.getTime() < startOfTomorrow.getTime()) return true; break
            case "week":    if (d.getTime() >= startOfToday.getTime()
                                && d.getTime() < endOfWeek.getTime()) return true; break
            case "month":   if (d.getTime() >= startOfToday.getTime()
                                && d.getTime() < endOfMonth.getTime()) return true; break
            }
        }
        return false
    }
    /// True when at least one configured visible column is a date —
    /// drives the visibility of the chip strip.
    readonly property bool _hasDateColumn: {
        for (var i = 0; i < visibleColumns.length; ++i) {
            if (String(visibleColumns[i].columnType || "").toLowerCase() === "date") return true
        }
        return false
    }

    // No root border — TrackerView fills its tab area; the surrounding
    // tab-bar bottom-divider is the chrome above us, the window edge
    // is below. Tonal stack inside (toolbar / surface / toolbarAlt)
    // does the rest.
    color: Theme.colors.bg

    /// Fixed columns flanking the dynamic metadata block. Resizable
    /// via right-edge drag handles (same idiom as the metadata
    /// columns and FileBrowser's Name/Size). Project is only shown
    /// in aggregated mode (jobPath === "") — in per-job mode the
    /// value is constant across every row, so the column would just
    /// be wasted horizontal space.
    ///
    /// Widths persist in the shared view-memory store
    /// (browser_folder_prefs): per-job trackers merge trk* fields into
    /// the job's row; the aggregated tracker uses the reserved
    /// "view:tracker" key. Metadata-column widths persist separately
    /// in the Columns DB, unchanged.
    property real projectColWidth: 160
    property real nameColWidth: 240
    property real folderColWidth: 140
    property real modifiedColWidth: 160

    readonly property string _prefsKey: jobPath.length > 0 ? jobPath : "view:tracker"
    property bool _widthsRestored: false

    function _restoreColWidths() {
        _widthsRestored = false
        try {
            var prefs = JSON.parse(Settings.folder_view_prefs(_prefsKey))
            if (prefs.trkProjectW > 0) projectColWidth = prefs.trkProjectW
            if (prefs.trkNameW > 0) nameColWidth = prefs.trkNameW
            if (prefs.trkFolderW > 0) folderColWidth = prefs.trkFolderW
        } catch (e) {}
        _widthsRestored = true
    }
    Timer {
        id: _colSaveTimer
        interval: 500
        repeat: false
        onTriggered: {
            var prefs = ({})
            try { prefs = JSON.parse(Settings.folder_view_prefs(root._prefsKey)) } catch (e) { prefs = ({}) }
            prefs.trkProjectW = Math.round(root.projectColWidth)
            prefs.trkNameW = Math.round(root.nameColWidth)
            prefs.trkFolderW = Math.round(root.folderColWidth)
            Settings.set_folder_view_prefs(root._prefsKey, JSON.stringify(prefs))
        }
    }
    onProjectColWidthChanged: if (_widthsRestored) _colSaveTimer.restart()
    onNameColWidthChanged: if (_widthsRestored) _colSaveTimer.restart()
    onFolderColWidthChanged: if (_widthsRestored) _colSaveTimer.restart()
    on_PrefsKeyChanged: _restoreColWidths()

    /// Per-row data from Subscription.tracked_items_json filtered to
    /// this job (or all if jobPath is "").
    ListModel { id: trackedModel }

    /// JS array of column descriptors. Each entry:
    ///   { columnName, columnType, columnOrder, columnWidth,
    ///     columnId,            // -1 when discovered
    ///     sourceJobPath,       // (jobPath, folderName) of the def
    ///     sourceFolderName,    //   used when editing/deleting
    ///     discovered           // true → orphan key, no def in DB
    ///     options              // [] when no dropdown choices
    ///   }
    property var visibleColumns: []

    /// Multi-select state. selectedItemPaths is the set of currently
    /// highlighted row itemPaths (JS array used as a set). Anchor is
    /// the model index for shift-range selection.
    property var selectedItemPaths: []
    property int _selectionAnchor: -1
    function _isSelected(p) { return selectedItemPaths.indexOf(p) >= 0 }
    function _selectOnly(idx, p) {
        selectedItemPaths = [p]
        _selectionAnchor = idx
    }
    function _toggleSelected(idx, p) {
        var s = selectedItemPaths.slice()
        var pos = s.indexOf(p)
        if (pos >= 0) s.splice(pos, 1)
        else s.push(p)
        selectedItemPaths = s
        _selectionAnchor = idx
    }
    function _selectRange(toIdx) {
        var anchor = _selectionAnchor < 0 ? 0 : _selectionAnchor
        var lo = Math.min(anchor, toIdx)
        var hi = Math.max(anchor, toIdx)
        var s = []
        for (var i = lo; i <= hi && i < trackedModel.count; ++i) {
            s.push(trackedModel.get(i).itemPath)
        }
        selectedItemPaths = s
    }
    function _clearSelection() {
        selectedItemPaths = []
        _selectionAnchor = -1
    }
    /// Build a Rust-friendly JSON payload for the bulk invokables.
    /// Each entry: {jobPath, itemPath, folderName, isTracked}.
    function _selectedItemsJson() {
        var out = []
        var paths = selectedItemPaths
        for (var i = 0; i < trackedModel.count; ++i) {
            var r = trackedModel.get(i)
            if (paths.indexOf(r.itemPath) >= 0) {
                out.push({
                    jobPath: r.jobPathStr,
                    itemPath: r.itemPath,
                    folderName: r.folderName,
                    isTracked: true
                })
            }
        }
        return JSON.stringify(out)
    }
    /// Fetched-on-demand list of {column, columnName, columnType,
    /// options} for the bulk-edit submenu's per-column submenus.
    /// Filtered to types that make sense to bulk-set.
    function _bulkEditableColumns() {
        var out = []
        for (var i = 0; i < visibleColumns.length; ++i) {
            var c = visibleColumns[i]
            if (c.discovered) continue
            var t = String(c.columnType || "text").toLowerCase()
            if (t === "dropdown" || t === "priority"
                || t === "checkbox" || t === "date"
                || t === "text" || t === "number") {
                out.push(c)
            }
        }
        return out
    }

    /// Active sort key + direction. _sortKey is one of:
    ///   "name" | "folder" | "modified" | "<columnName>"
    /// _sortDir: 1 = asc, -1 = desc, 0 = unsorted (use insert order).
    /// Click the same header to cycle asc → desc → unsorted; click a
    /// different header to start at asc.
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

    /// Column-width overrides keyed by columnName. The handle's
    /// release writes the dragged width here so rows repaint
    /// immediately, then _commitColumnWidth persists via
    /// Columns.update_column and clears the entry. During the drag
    /// itself only the guide line moves — see _dragGuideX.
    property var _columnWidthOverrides: ({})

    /// X (tableArea coords) of the column-resize guide line, -1 when
    /// no header handle is being dragged. Handles track this during
    /// the drag instead of writing widths live — a live width write
    /// re-flows every visible row's Row per mouse move (visibly
    /// stuttery); the real width commits once on release.
    property real _dragGuideX: -1
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
        // Clear override after commit; refresh repulls the new width.
        var m = Object.assign({}, _columnWidthOverrides)
        delete m[col.columnName]
        _columnWidthOverrides = m
        _refreshColumns()
    }

    // Personal-scope set for the aggregated All Tracker: tracked_items_json
    // carries EVERY tracked row mesh-wide (subscriptions are per-user, the
    // core query LEFT JOINs), so the "" instance filters to the user's own
    // subscribed jobs here. Per-job instances filter by jobPath and show
    // their rows regardless of subscription. Lowercased native paths; both
    // JSONs come from the same canonical→native translation, so they agree
    // on separators.
    // Same normalization Main.qml's _findSubscriptionForPath uses —
    // separator- and case-insensitive, trailing separators stripped —
    // so spelling drift between the two JSONs can't drop a job's rows.
    function _normJobPath(p) {
        return p.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase()
    }
    function _subscribedJobSet() {
        var set = ({})
        try {
            var subs = JSON.parse(Subscription.subscriptions_json)
            for (var i = 0; i < subs.length; ++i) {
                if (subs[i].jobPath) set[_normJobPath(subs[i].jobPath)] = true
            }
        } catch (e) {
            console.warn("TrackerView: subscriptions parse failed:", e)
        }
        return set
    }

    function refresh() {
        trackedModel.clear()
        if (Subscription.tracked_items_json) {
            try {
                var arr = JSON.parse(Subscription.tracked_items_json)
                var aggregated = root.jobPath.length === 0
                var subSet = aggregated ? _subscribedJobSet() : null
                var rows = []
                for (var i = 0; i < arr.length; ++i) {
                    var t = arr[i]
                    if (!aggregated && t.jobPath !== root.jobPath) continue
                    if (aggregated && !subSet[_normJobPath(t.jobPath || "")]) continue
                    rows.push({
                        itemPath: t.itemPath || "",
                        name: _basename(t.itemPath || ""),
                        folderName: t.folderName || "",
                        jobPathStr: t.jobPath || "",
                        jobNameStr: t.jobName || _basename(t.jobPath || ""),
                        metadataJsonStr: t.metadataJson || "{}",
                        modified: typeof t.modifiedTime === "number" ? t.modifiedTime : 0
                    })
                }
                if (root._sortKey.length > 0 && root._sortDir !== 0) {
                    rows.sort(function(a, b) {
                        return root._sortDir * root._compareRows(a, b)
                    })
                }
                for (var j = 0; j < rows.length; ++j) {
                    trackedModel.append(rows[j])
                }
            } catch (e) {
                console.warn("TrackerView: tracked parse failed:", e)
            }
        }
        // _refreshColumns reads the populated trackedModel to
        // discover (jobPath, folderName) pairs and any orphan keys,
        // so it must run BEFORE the date filter (which walks
        // visibleColumns to pick out the date-typed columns).
        _refreshColumns()
        // Due-date chip filter — applied last by removing
        // non-matching rows from trackedModel in place. Walk from
        // the back so the indices we still need to visit don't shift.
        if (_dueFilter.length > 0) {
            for (var k = trackedModel.count - 1; k >= 0; --k) {
                if (!_matchesDueFilter(trackedModel.get(k))) {
                    trackedModel.remove(k)
                }
            }
        }
    }

    /// Comparator for the active _sortKey. Returns negative/0/positive
    /// like Array.prototype.sort. Built-in keys ("name", "folder",
    /// "modified") are read directly from the row; everything else is
    /// looked up inside the row's parsed metadata JSON. Numbers/dates
    /// compare numerically; everything else compares as a
    /// case-insensitive string.
    function _compareRows(a, b) {
        var key = root._sortKey
        var av, bv
        if (key === "name") { av = a.name; bv = b.name }
        else if (key === "project") { av = a.jobNameStr; bv = b.jobNameStr }
        else if (key === "folder") { av = a.folderName; bv = b.folderName }
        else if (key === "modified") { av = a.modified; bv = b.modified }
        else {
            var ma = {}, mb = {}
            try { ma = JSON.parse(a.metadataJsonStr || "{}") } catch (e) {}
            try { mb = JSON.parse(b.metadataJsonStr || "{}") } catch (e) {}
            av = ma[key]
            bv = mb[key]
        }
        // Empty / missing values sort last regardless of direction so
        // populated rows always cluster at the top of an asc sort.
        var aEmpty = (av === undefined || av === null || av === "")
        var bEmpty = (bv === undefined || bv === null || bv === "")
        if (aEmpty && bEmpty) return 0
        if (aEmpty) return root._sortDir === -1 ? -1 : 1
        if (bEmpty) return root._sortDir === -1 ? 1 : -1
        if (typeof av === "number" && typeof bv === "number") {
            return av < bv ? -1 : (av > bv ? 1 : 0)
        }
        var as = String(av).toLowerCase()
        var bs = String(bv).toLowerCase()
        return as < bs ? -1 : (as > bs ? 1 : 0)
    }

    /// Build the unified column list. Two passes:
    ///   1. Pull ColumnDefinitions for every (jobPath, folderName)
    ///      represented in trackedModel; merge by (name + type).
    ///   2. Walk every row's metadataJson for keys not yet in the
    ///      list — surface them as `discovered` text columns. Keeps
    ///      stored data visible even when no ColumnDefinition exists
    ///      for it (the legacy app's biggest UX rough edge).
    // doReconcile (default true): kick the background template→options
    // reconcile per folder. Pass false from onOptions_revChanged so the
    // post-reconcile repaint doesn't re-trigger reconcile (loop-free).
    function _refreshColumns(doReconcile) {
        if (doReconcile === undefined) doReconcile = true
        var seenFolders = {}
        var seenColumns = {}    // key = name|type
        var out = []
        // Hidden-column names captured here so the discovered-keys
        // pass below knows to skip them. Without this, a hidden
        // column with metadata values gets re-surfaced as a
        // discovered (italic) column, defeating the hide.
        var hiddenConfiguredNames = {}

        // Pass 1: ColumnDefinitions.
        for (var i = 0; i < trackedModel.count; ++i) {
            var row = trackedModel.get(i)
            var folderKey = row.jobPathStr + "|" + row.folderName
            if (seenFolders[folderKey]) continue
            seenFolders[folderKey] = true

            // Pull authoritative option lists from template files
            // before reading column_options. Catches peers that
            // missed a broadcast (offline window or LWW-rejected
            // back-to-back save). Best-effort: any failure is silent
            // and falls through to the normal get_column_defs read.
            // Async/non-blocking — returns immediately; a reconcile that
            // writes new options bumps options_rev → repaint.
            if (doReconcile)
                try {
                    Columns.reconcile_options_from_templates(
                        row.jobPathStr, row.folderName)
                } catch (e) {}
            var defs = []
            try {
                defs = JSON.parse(
                    Columns.get_column_defs(row.jobPathStr, row.folderName))
            } catch (e) { continue }

            for (var d = 0; d < defs.length; ++d) {
                var col = defs[d]
                var name = col.columnName || ""
                if (col.isVisible === false) {
                    hiddenConfiguredNames[name] = true
                    continue
                }
                // 0.9.97: skip unpromoted columns. They surface only
                // in the Column Manager so the user can act on them.
                // Also stash the name in hiddenConfiguredNames so
                // pass-2 (the "discovered keys" walk that surfaces
                // any metadata key without a column row) doesn't
                // re-surface them as orphan columns — without this,
                // unpromoted columns immediately reappear in the
                // shot table as italicised "discovered" entries.
                if (!col.templateHash) {
                    hiddenConfiguredNames[name] = true
                    continue
                }
                var type = col.columnType || "text"
                var colKey = name + "|" + type
                if (seenColumns[colKey]) continue
                seenColumns[colKey] = true
                out.push({
                    columnName: name,
                    columnType: type,
                    columnOrder: typeof col.columnOrder === "number" ? col.columnOrder : 0,
                    columnWidth: typeof col.columnWidth === "number" && col.columnWidth > 0
                        ? col.columnWidth : 110,
                    columnId: typeof col.id === "number" ? col.id : -1,
                    sourceJobPath: row.jobPathStr,
                    sourceFolderName: row.folderName,
                    discovered: false,
                    options: Array.isArray(col.options) ? col.options : []
                })
            }
        }

        // 0.9.97 — pass-2 discovered-keys removed from the tracker
        // grid. Same reasoning as ItemListPanel: orphan metadata
        // keys (cross-OS paths, freshly-unpromoted columns) were
        // being surfaced as italic columns the user couldn't act on.
        // The Column Manager still surfaces them so the user can
        // promote / clean them up there.

        out.sort(function(a, b) { return a.columnOrder - b.columnOrder })
        visibleColumns = out
    }

    function _basename(p) {
        var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
        return idx >= 0 ? p.substring(idx + 1) : p
    }
    function formatDate(epochMs) {
        if (!epochMs) return ""
        var d = new Date(epochMs)
        return d.toLocaleString(Qt.locale(), "yyyy-MM-dd hh:mm")
    }

    function _cellValue(metadata, col) {
        if (!metadata || !col) return ""
        var raw = metadata[col.columnName]
        if (raw === undefined || raw === null) return ""
        if (col.columnType === "checkbox") return raw ? "✓" : ""
        if (col.columnType === "date" && typeof raw === "number") return formatDate(raw)
        return String(raw)
    }

    /// Best-effort "where should new columns live?" target. In per-
    /// job mode we use this job + the first folder represented in
    /// trackedModel. Empty in aggregated mode (Add Column disabled
    /// from the header menu there).
    function _addContext() {
        if (root.jobPath.length === 0) return null
        var folder = ""
        if (trackedModel.count > 0) {
            folder = trackedModel.get(0).folderName
        }
        if (folder.length === 0) return null
        return { jobPath: root.jobPath, folderName: folder }
    }

    onJobPathChanged: refresh()
    Connections {
        target: Subscription
        // tracked_items_json fires when the tracked-items result set
        // itself changes (subscribe / untrack / a tracked item's blob
        // updated). metadata_rev fires on EVERY metadata write —
        // including untracked-item edits made via ItemListPanel — so
        // discovered/orphan columns and any tracked rows that share
        // a folder with the edited item refresh immediately. Both
        // route through refresh() which is cheap (rebuilds from the
        // already-cached tracked_items_json).
        function onTracked_items_jsonChanged() { root.refresh() }
        function onMetadata_revChanged() {
            Subscription.refresh_tracked()
            root.refresh()
        }
        // Subscriptions are per-user scope for the aggregated view —
        // re-filter when the user (un)subscribes.
        function onSubscriptions_jsonChanged() {
            if (root.jobPath.length === 0) root.refresh()
        }
    }
    Connections {
        target: Columns
        // Column DEFINITIONS changed — locally (add/edit/delete via the
        // Column Manager) or via a mesh push (Sidebar calls
        // Columns.invalidate on remote col_*). Rebuild the column layout
        // so new/edited columns appear live instead of only after the
        // manager closes or the folder is reopened.
        function onColumns_revChanged() { root._refreshColumns() }
        // Background reconcile wrote new options — repaint WITHOUT
        // reconciling again (false) so this can't loop.
        function onOptions_revChanged() { root._refreshColumns(false) }
    }
    Component.onCompleted: {
        Subscription.refresh_tracked()
        refresh()
        _restoreColWidths()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            color: Theme.colors.toolbar

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 0
                spacing: 0
                Icon {
                    name: "clipboard-text"
                    size: Theme.icon.sizeToolbar
                    color: Theme.colors.textMuted
                    Layout.rightMargin: 6
                    Layout.alignment: Qt.AlignVCenter
                }
                Label {
                    text: root.jobPath.length > 0
                        ? qsTr("Tracker — %1").arg(root.jobName)
                        : qsTr("Tracker (all jobs)")
                    color: Theme.colors.textBright
                    font.pixelSize: Theme.font.sizeHeading
                    font.bold: true
                    Layout.fillWidth: true
                }
                Label {
                    text: qsTr("%1 item(s)").arg(trackedModel.count)
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                    Layout.rightMargin: 8
                }
                FlatButton {
                    visible: root.jobPath.length > 0
                    enabled_: root._addContext() !== null
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: "table"
                    text: qsTr("Manage Metadata")
                    tooltip: qsTr("Add / edit / delete columns + promote discovered metadata keys")
                    onClicked: {
                        var ctx = root._addContext()
                        if (!ctx) return
                        var blobs = []
                        for (var i = 0; i < trackedModel.count; ++i) {
                            blobs.push(trackedModel.get(i).metadataJsonStr)
                        }
                        columnManager.jobPath = ctx.jobPath
                        columnManager.folderName = ctx.folderName
                        columnManager.metadataBlobs = blobs
                        columnManager.open()
                    }
                }
                FlatButton {
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: root._sortDir === 1
                        ? "sort-ascending"
                        : (root._sortDir === -1 ? "sort-descending" : "funnel")
                    text: {
                        if (root._sortKey === "name")     return qsTr("Name")
                        if (root._sortKey === "project")  return qsTr("Project")
                        if (root._sortKey === "folder")   return qsTr("Folder")
                        if (root._sortKey === "modified") return qsTr("Modified")
                        if (root._sortKey.length > 0)     return root._sortKey
                        return qsTr("Sort")
                    }
                    tooltip: qsTr("Sort tracked items")
                    onClicked: trackerSortMenu.popup(0, height)
                    UfbMenu {
                        id: trackerSortMenu
                        y: parent.height
                        MenuItem {
                            visible: root.jobPath.length === 0
                            height: visible ? implicitHeight : 0
                            text: qsTr("Project") + (root._sortKey === "project"
                                ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                            onTriggered: root._toggleSort("project")
                        }
                        MenuItem {
                            text: qsTr("Name") + (root._sortKey === "name"
                                ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                            onTriggered: root._toggleSort("name")
                        }
                        MenuItem {
                            text: qsTr("Folder") + (root._sortKey === "folder"
                                ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                            onTriggered: root._toggleSort("folder")
                        }
                        MenuItem {
                            text: qsTr("Modified") + (root._sortKey === "modified"
                                ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                            onTriggered: root._toggleSort("modified")
                        }
                        MenuSeparator { visible: root.visibleColumns.length > 0 }
                        // One MenuItem per metadata column. The
                        // Instantiator pattern keeps the menu in sync
                        // with column add/edit/delete without
                        // needing manual rebuilds.
                        Instantiator {
                            model: root.visibleColumns
                            // Index = number of fixed entries above
                            // (Project + Name + Folder + Modified +
                            // separator) + dynamic column index.
                            onObjectAdded: (idx, obj) => trackerSortMenu.insertItem(idx + 5, obj)
                            onObjectRemoved: (idx, obj) => trackerSortMenu.removeItem(obj)
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
                FlatButton {
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: "arrow-clockwise"
                    tooltip: qsTr("Refresh tracked items")
                    onClicked: Subscription.refresh_tracked()
                }
            }
        }

        // ── Due-date filter chip strip ───────────────────────────────
        // Only shown when at least one configured visible column is a
        // date — otherwise the chips can't filter anything. Each chip
        // sets _dueFilter and refreshes; clicking the active chip
        // again clears the filter.
        Rectangle {
            visible: root._hasDateColumn
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            color: Theme.colors.toolbarAlt
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 4
                Label {
                    text: qsTr("Due:")
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                }
                Repeater {
                    model: [
                        { key: "",        label: qsTr("All") },
                        { key: "overdue", label: qsTr("Overdue") },
                        { key: "today",   label: qsTr("Today") },
                        { key: "week",    label: qsTr("This Week") },
                        { key: "month",   label: qsTr("This Month") }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool active: root._dueFilter === modelData.key
                        Layout.preferredHeight: 20
                        implicitWidth: chipLabel.implicitWidth + 16
                        radius: 10
                        // No border — fill alone communicates state.
                        // Active chip: accent fill. Hover: surfaceHover.
                        // Resting: transparent (chip strip's toolbarAlt
                        // shows through).
                        color: active
                            ? Theme.colors.accent
                            : (chipMa.containsMouse ? Theme.colors.surfaceHover : "transparent")
                        Label {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: modelData.label
                            color: parent.active ? Theme.colors.textBright : Theme.colors.text
                            font.pixelSize: Theme.font.sizeSmall
                        }
                        MouseArea {
                            id: chipMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root._dueFilter = modelData.key
                                root.refresh()
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: root._dueFilter.length > 0
                        ? qsTr("(%1 shown)").arg(trackedModel.count)
                        : ""
                    color: Theme.colors.textSubtle
                    font.pixelSize: Theme.font.sizeSmall
                }
            }
        }

        Rectangle {
            id: tableArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.surface

            // Column-resize guide — tracks the proposed column edge
            // during a header width drag; the width commits once on
            // release (see _dragGuideX). Spans header + rows.
            Rectangle {
                visible: root._dragGuideX >= 0
                x: root._dragGuideX
                y: 0
                width: 2
                height: parent.height
                color: Theme.colors.accent
                z: 100
            }

            // Direct ColumnLayout (no Flickable wrapper). Wide column
            // blocks clip horizontally for now — horizontal scroll
            // would require a Flickable with explicit dimensions and
            // re-anchoring the inner ColumnLayout, which collapsed
            // ListView.fillHeight to 0 in an earlier attempt.
            ColumnLayout {
                id: rowsCol
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.preferredHeight: 22
                    Layout.fillWidth: true
                    color: Theme.colors.toolbarAlt
                        Row {
                            id: headerRow
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 0
                            // Project header — only in aggregated mode
                            // (the per-job Tracker shows one job's
                            // items, so the column would just repeat
                            // the same name on every row).
                            Item {
                                visible: root.jobPath.length === 0
                                width: visible ? root.projectColWidth : 0
                                height: parent.height
                                Label {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.rightMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    verticalAlignment: Text.AlignVCenter
                                    text: qsTr("Project") + root._sortGlyph("project")
                                    color: Theme.colors.textMuted
                                    font.pixelSize: 11
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
                                    onClicked: root._toggleSort("project")
                                }
                                Rectangle {
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    anchors.topMargin: 4
                                    anchors.bottomMargin: 4
                                    width: 2
                                    color: projectHandleMa.containsMouse || projectHandleMa.pressed
                                        ? Theme.colors.accent : Theme.colors.borderStrong
                                    z: 10
                                }
                                MouseArea {
                                    id: projectHandleMa
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
                                        _startWidth = root.projectColWidth
                                    }
                                    onPositionChanged: (mouse) => {
                                        if (!pressed) return
                                        _dragWidth = Math.max(60,
                                            _startWidth + (mouse.x - _startMouseX))
                                        root._dragGuideX = parent
                                            .mapToItem(tableArea, _dragWidth, 0).x
                                    }
                                    onReleased: {
                                        if (_dragWidth >= 0)
                                            root.projectColWidth = _dragWidth
                                        _dragWidth = -1
                                        root._dragGuideX = -1
                                    }
                                    onCanceled: {
                                        _dragWidth = -1
                                        root._dragGuideX = -1
                                    }
                                }
                            }
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
                                    font.pixelSize: 11
                                    font.bold: true
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
                                    color: nameHandleMa.containsMouse || nameHandleMa.pressed
                                        ? Theme.colors.accent : Theme.colors.borderStrong
                                    z: 10
                                }
                                MouseArea {
                                    id: nameHandleMa
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
                                        _dragWidth = Math.max(80,
                                            _startWidth + (mouse.x - _startMouseX))
                                        root._dragGuideX = parent
                                            .mapToItem(tableArea, _dragWidth, 0).x
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
                                    id: headerCell
                                    // Live width — picks up override
                                    // during drag, falls back to def.
                                    width: {
                                        if (!modelData) return 0
                                        var ov = root._columnWidthOverrides[modelData.columnName]
                                        return ov !== undefined ? ov : modelData.columnWidth
                                    }
                                    height: parent.height
                                    Label {
                                        anchors.left: parent.left
                                        anchors.right: handle.left
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
                                        font.pixelSize: 11
                                        font.bold: !(modelData && modelData.discovered)
                                        font.italic: modelData && modelData.discovered
                                        elide: Text.ElideRight
                                    }
                                    // Header sort. Sits BELOW the drag
                                    // handle MouseArea (handle has its
                                    // own preventStealing: true) so
                                    // clicks on the column body sort,
                                    // clicks on the right edge resize.
                                    MouseArea {
                                        anchors.left: parent.left
                                        anchors.right: handle.left
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            if (modelData) root._toggleSort(modelData.columnName)
                                        }
                                    }
                                    // Visible divider + 10px hot zone.
                                    Rectangle {
                                        id: handle
                                        anchors.right: parent.right
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        anchors.topMargin: 4
                                        anchors.bottomMargin: 4
                                        width: 2
                                        color: handleMa.containsMouse || handleMa.pressed
                                            ? Theme.colors.accent : Theme.colors.borderStrong
                                        z: 10
                                    }
                                    MouseArea {
                                        id: handleMa
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
                                            _startWidth = headerCell.width
                                        }
                                        onPositionChanged: (mouse) => {
                                            if (!pressed) return
                                            _dragWidth = Math.max(40,
                                                _startWidth + (mouse.x - _startMouseX))
                                            root._dragGuideX = headerCell
                                                .mapToItem(tableArea, _dragWidth, 0).x
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
                                width: root.folderColWidth
                                height: parent.height
                                Label {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.rightMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    verticalAlignment: Text.AlignVCenter
                                    text: qsTr("Folder") + root._sortGlyph("folder")
                                    color: Theme.colors.textMuted
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                                MouseArea {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root._toggleSort("folder")
                                }
                                Rectangle {
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    anchors.topMargin: 4
                                    anchors.bottomMargin: 4
                                    width: 2
                                    color: folderHandleMa.containsMouse || folderHandleMa.pressed
                                        ? Theme.colors.accent : Theme.colors.borderStrong
                                    z: 10
                                }
                                MouseArea {
                                    id: folderHandleMa
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
                                        _startWidth = root.folderColWidth
                                    }
                                    onPositionChanged: (mouse) => {
                                        if (!pressed) return
                                        _dragWidth = Math.max(60,
                                            _startWidth + (mouse.x - _startMouseX))
                                        root._dragGuideX = parent
                                            .mapToItem(tableArea, _dragWidth, 0).x
                                    }
                                    onReleased: {
                                        if (_dragWidth >= 0)
                                            root.folderColWidth = _dragWidth
                                        _dragWidth = -1
                                        root._dragGuideX = -1
                                    }
                                    onCanceled: {
                                        _dragWidth = -1
                                        root._dragGuideX = -1
                                    }
                                }
                            }
                            Item {
                                width: root.modifiedColWidth
                                height: parent.height
                                Label {
                                    anchors.fill: parent
                                    verticalAlignment: Text.AlignVCenter
                                    horizontalAlignment: Text.AlignRight
                                    text: qsTr("Modified") + root._sortGlyph("modified")
                                    color: Theme.colors.textMuted
                                    font.pixelSize: 11
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

                    ListView {
                        id: trackedList
                        Layout.fillWidth: true
                        Layout.minimumWidth: headerRow.implicitWidth
                        Layout.fillHeight: true
                        model: trackedModel
                        clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: Rectangle {
                            id: rowItem
                            required property int index
                            required property string itemPath
                            required property string name
                            required property string folderName
                            required property string jobPathStr
                            required property string jobNameStr
                            required property string metadataJsonStr
                            required property real modified

                            readonly property bool isSelected: root._isSelected(itemPath)

                            width: ListView.view.contentWidth > 0
                                ? ListView.view.contentWidth
                                : trackedList.width
                            height: 24
                            color: isSelected
                                ? Theme.colors.accentMuted
                                : rowMa.containsMouse
                                ? Theme.colors.surfaceHover
                                : (index % 2 === 1 ? Theme.colors.surfaceAlt : "transparent")

                            readonly property var rowMetadata: {
                                try { return JSON.parse(metadataJsonStr || "{}") }
                                catch (e) { return {} }
                            }

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 0
                                Label {
                                    visible: root.jobPath.length === 0
                                    width: visible ? root.projectColWidth : 0
                                    text: rowItem.jobNameStr
                                    color: Theme.colors.info
                                    font.pixelSize: Theme.font.sizeSmall
                                    elide: Text.ElideRight
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Label {
                                    width: root.nameColWidth
                                    text: "★ " + rowItem.name
                                    color: Theme.colors.text
                                    font.pixelSize: Theme.font.sizeBody
                                    elide: Text.ElideRight
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Repeater {
                                    model: root.visibleColumns
                                    delegate: CellEditor {
                                        // Repeater's modelData is briefly
                                        // undefined during model swaps;
                                        // guard every access. Live-width
                                        // override mirrors the header so
                                        // dragging stays aligned.
                                        width: {
                                            if (!modelData) return 0
                                            var ov = root._columnWidthOverrides[modelData.columnName]
                                            return ov !== undefined ? ov : modelData.columnWidth
                                        }
                                        height: parent.height
                                        col: modelData
                                        value: (modelData && rowItem.rowMetadata)
                                            ? rowItem.rowMetadata[modelData.columnName]
                                            : null
                                        selected: rowMa.containsMouse
                                        readOnly: false
                                        onCommit: (newValue) => {
                                            if (!modelData) return
                                            // JSON-encode the value so booleans /
                                            // numbers / null travel through the
                                            // QString invokable correctly.
                                            Subscription.set_item_metadata_field(
                                                rowItem.jobPathStr,
                                                rowItem.itemPath,
                                                rowItem.folderName,
                                                modelData.columnName,
                                                JSON.stringify(newValue),
                                                true)
                                        }
                                    }
                                }
                                Label {
                                    width: root.folderColWidth
                                    text: rowItem.folderName
                                    color: Theme.colors.info
                                    font.pixelSize: Theme.font.sizeSmall
                                    elide: Text.ElideRight
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Label {
                                    width: root.modifiedColWidth
                                    text: root.formatDate(rowItem.modified)
                                    color: Theme.colors.textMuted
                                    font.pixelSize: Theme.font.sizeSmall
                                    font.family: Theme.font.mono
                                    horizontalAlignment: Text.AlignRight
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            // Row-wide click target. Sits BEHIND the
                            // Row's cells so per-cell MouseAreas (in
                            // CellEditor) get first crack at left
                            // clicks for in-place edit mode. Clicks on
                            // non-cell areas (Name / Folder / Modified
                            // labels) fall through here for row-level
                            // navigation. Right-clicks always reach
                            // here because cells default to LeftButton
                            // only.
                            MouseArea {
                                id: rowMa
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                z: -1
                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.RightButton) {
                                        // If the row isn't already in the
                                        // selection, treat right-click as
                                        // a single-select. Right-clicking
                                        // an already-selected row keeps
                                        // the multi-select intact (so the
                                        // bulk menu applies to the whole
                                        // selection).
                                        if (!rowItem.isSelected) {
                                            root._selectOnly(rowItem.index, rowItem.itemPath)
                                        }
                                        rowMenu.itemPath = rowItem.itemPath
                                        rowMenu.jobPath = rowItem.jobPathStr
                                        rowMenu.jobName = rowItem.jobNameStr
                                        rowMenu.folderName = rowItem.folderName
                                        rowMenu.popup()
                                    } else if (mouse.modifiers & Qt.ControlModifier) {
                                        root._toggleSelected(rowItem.index, rowItem.itemPath)
                                    } else if (mouse.modifiers & Qt.ShiftModifier) {
                                        root._selectRange(rowItem.index)
                                    } else {
                                        // Single left-click just
                                        // selects (so the user can
                                        // build a multi-select +
                                        // right-click for bulk edit).
                                        // Double-click navigates.
                                        root._selectOnly(rowItem.index, rowItem.itemPath)
                                    }
                                }
                                onDoubleClicked: (mouse) => {
                                    if (mouse.button !== Qt.LeftButton) return
                                    root.goToItemRequested(
                                        rowItem.jobPathStr,
                                        rowItem.jobNameStr,
                                        rowItem.folderName,
                                        rowItem.itemPath)
                                }
                            }
                        }

                    Label {
                        anchors.centerIn: parent
                        visible: trackedModel.count === 0
                        text: qsTr("(no tracked items — right-click an item in the left panel and choose “Mark as Tracked”)")
                        color: Theme.colors.textSubtle
                        font.pixelSize: Theme.font.sizeSmall
                        font.italic: true
                        width: parent.width - 24
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    // ── Row context menu ────────────────────────────────────────────
    // When more than one row is selected, the single-row entries are
    // hidden and the bulk submenus take over. Per-column submenus are
    // built dynamically from visibleColumns via Instantiator.
    UfbMenu {
        id: rowMenu
        property string itemPath: ""
        property string jobPath: ""
        property string jobName: ""
        property string folderName: ""
        readonly property int selCount: root.selectedItemPaths.length

        // ── Single-row navigation + clipboard (count == 1) ──────────
        MenuItem {
            visible: rowMenu.selCount <= 1
            height: visible ? implicitHeight : 0
            text: qsTr("Open in Project")
            onTriggered: root.goToItemRequested(rowMenu.jobPath, rowMenu.jobName, rowMenu.folderName, rowMenu.itemPath)
        }
        MenuItem {
            visible: rowMenu.selCount <= 1
            height: visible ? implicitHeight : 0
            text: qsTr("Reveal in Explorer")
            onTriggered: FileOps.reveal_in_file_manager(rowMenu.itemPath)
        }
        MenuItem {
            visible: rowMenu.selCount <= 1
            height: visible ? implicitHeight : 0
            text: qsTr("Open in Left Browser")
            onTriggered: {
                if (Window.window && Window.window.openPathInBrowser)
                    Window.window.openPathInBrowser("left", rowMenu.itemPath)
            }
        }
        MenuItem {
            visible: rowMenu.selCount <= 1
            height: visible ? implicitHeight : 0
            text: qsTr("Open in Right Browser")
            onTriggered: {
                if (Window.window && Window.window.openPathInBrowser)
                    Window.window.openPathInBrowser("right", rowMenu.itemPath)
            }
        }
        MenuSeparator { visible: rowMenu.selCount <= 1; height: visible ? implicitHeight : 0 }
        MenuItem {
            visible: rowMenu.selCount <= 1
            height: visible ? implicitHeight : 0
            text: qsTr("Copy Path")
            onTriggered: FileOps.clipboard_copy_text(rowMenu.itemPath)
        }
        MenuItem {
            visible: rowMenu.selCount <= 1
            height: visible ? implicitHeight : 0
            text: qsTr("Copy ufb:// Link")
            onTriggered: FileOps.clipboard_copy_text(FileOps.build_ufb_uri(rowMenu.itemPath))
        }
        MenuSeparator { visible: rowMenu.selCount <= 1; height: visible ? implicitHeight : 0 }

        // ── Bulk header label (count > 1) ────────────────────────────
        MenuItem {
            visible: rowMenu.selCount > 1
            height: visible ? implicitHeight : 0
            enabled: false
            text: qsTr("%1 items selected").arg(rowMenu.selCount)
        }

        // ── Per-column "Set X" submenus ──────────────────────────────
        Instantiator {
            model: root._bulkEditableColumns()
            onObjectAdded: (idx, obj) => rowMenu.insertMenu(rowMenu.count - 2, obj)
            onObjectRemoved: (idx, obj) => rowMenu.removeMenu(obj)
            delegate: UfbMenu {
                required property var modelData
                title: modelData
                    ? qsTr("Set %1").arg(modelData.columnName)
                    : ""
                visible: !!modelData
                height: visible ? implicitHeight : 0
                // Dropdown / priority — one entry per option.
                Instantiator {
                    model: modelData
                        && (String(modelData.columnType).toLowerCase() === "dropdown"
                            || String(modelData.columnType).toLowerCase() === "priority")
                        && Array.isArray(modelData.options)
                        ? modelData.options : []
                    onObjectAdded: (idx, obj) => parent.menu.insertItem(idx, obj)
                    onObjectRemoved: (idx, obj) => parent.menu.removeItem(obj)
                    delegate: MenuItem {
                        required property var modelData
                        text: modelData ? modelData.name : ""
                        onTriggered: {
                            // Capture the parent Menu's modelData
                            // (the column descriptor) before calling
                            // — modelData inside this MenuItem refers
                            // to the option, not the column.
                            var col = parent.parent.modelData
                            if (!col || !modelData) return
                            Subscription.bulk_set_item_metadata_field(
                                root._selectedItemsJson(),
                                col.columnName,
                                JSON.stringify(modelData.name))
                        }
                    }
                }
                // Checkbox — true / false / clear.
                MenuItem {
                    visible: modelData && String(modelData.columnType).toLowerCase() === "checkbox"
                    height: visible ? implicitHeight : 0
                    text: qsTr("Checked")
                    onTriggered: {
                        if (!modelData) return
                        Subscription.bulk_set_item_metadata_field(
                            root._selectedItemsJson(),
                            modelData.columnName, "true")
                    }
                }
                MenuItem {
                    visible: modelData && String(modelData.columnType).toLowerCase() === "checkbox"
                    height: visible ? implicitHeight : 0
                    text: qsTr("Unchecked")
                    onTriggered: {
                        if (!modelData) return
                        Subscription.bulk_set_item_metadata_field(
                            root._selectedItemsJson(),
                            modelData.columnName, "false")
                    }
                }
                // Date — opens a calendar dialog.
                MenuItem {
                    visible: modelData && String(modelData.columnType).toLowerCase() === "date"
                    height: visible ? implicitHeight : 0
                    text: qsTr("Pick date…")
                    onTriggered: {
                        if (!modelData) return
                        bulkDateDialog.openFor(modelData.columnName)
                    }
                }
                MenuItem {
                    visible: modelData && String(modelData.columnType).toLowerCase() === "date"
                    height: visible ? implicitHeight : 0
                    text: qsTr("Set to today")
                    onTriggered: {
                        if (!modelData) return
                        var t = new Date(); t.setHours(0,0,0,0)
                        Subscription.bulk_set_item_metadata_field(
                            root._selectedItemsJson(),
                            modelData.columnName, JSON.stringify(t.getTime()))
                    }
                }
                // Text / number — opens a text dialog.
                MenuItem {
                    visible: modelData && (String(modelData.columnType).toLowerCase() === "text"
                        || String(modelData.columnType).toLowerCase() === "number")
                    height: visible ? implicitHeight : 0
                    text: qsTr("Type value…")
                    onTriggered: {
                        if (!modelData) return
                        bulkTextDialog.openFor(modelData.columnName,
                            String(modelData.columnType).toLowerCase() === "number")
                    }
                }
                // Universal: clear the field on selected rows.
                MenuSeparator {}
                MenuItem {
                    text: qsTr("Clear")
                    onTriggered: {
                        if (!modelData) return
                        Subscription.bulk_set_item_metadata_field(
                            root._selectedItemsJson(),
                            modelData.columnName, "null")
                    }
                }
            }
        }

        MenuSeparator {}
        MenuItem {
            text: rowMenu.selCount > 1
                ? qsTr("Untrack %1 items").arg(rowMenu.selCount)
                : qsTr("Untrack")
            onTriggered: {
                if (rowMenu.selCount > 1) {
                    Subscription.bulk_set_item_tracked(
                        root._selectedItemsJson(), false)
                } else {
                    Subscription.set_item_tracked(
                        rowMenu.jobPath, rowMenu.itemPath, rowMenu.folderName, false)
                }
            }
        }
    }

    // ── Bulk date-picker dialog ─────────────────────────────────────
    Dialog {
        id: bulkDateDialog
        property string fieldName: ""
        title: qsTr("Set %1 for %2 items")
            .arg(fieldName)
            .arg(root.selectedItemPaths.length)
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        width: 280
        function openFor(name) {
            fieldName = name
            var t = new Date()
            bulkMonthGrid.year = t.getFullYear()
            bulkMonthGrid.month = t.getMonth()
            bulkYear = t.getFullYear()
            bulkMonth = t.getMonth()
            open()
        }
        property int bulkYear: new Date().getFullYear()
        property int bulkMonth: new Date().getMonth()

        ColumnLayout {
            anchors.fill: parent
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Button {
                    text: "‹"; padding: 0
                    Layout.preferredWidth: 24; Layout.preferredHeight: 22
                    onClicked: {
                        if (bulkDateDialog.bulkMonth === 0) {
                            bulkDateDialog.bulkMonth = 11
                            bulkDateDialog.bulkYear -= 1
                        } else {
                            bulkDateDialog.bulkMonth -= 1
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: Qt.locale().monthName(bulkDateDialog.bulkMonth, Locale.LongFormat)
                        + " " + bulkDateDialog.bulkYear
                    color: Theme.colors.text
                    font.pixelSize: Theme.font.sizeBody
                    font.bold: true
                }
                Button {
                    text: "›"; padding: 0
                    Layout.preferredWidth: 24; Layout.preferredHeight: 22
                    onClicked: {
                        if (bulkDateDialog.bulkMonth === 11) {
                            bulkDateDialog.bulkMonth = 0
                            bulkDateDialog.bulkYear += 1
                        } else {
                            bulkDateDialog.bulkMonth += 1
                        }
                    }
                }
            }
            DayOfWeekRow {
                Layout.fillWidth: true
                locale: Qt.locale()
                delegate: Label {
                    text: model.shortName
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeTiny
                    horizontalAlignment: Text.AlignHCenter
                }
            }
            MonthGrid {
                id: bulkMonthGrid
                Layout.fillWidth: true
                Layout.preferredHeight: 180
                month: bulkDateDialog.bulkMonth
                year: bulkDateDialog.bulkYear
                locale: Qt.locale()
                delegate: Rectangle {
                    implicitHeight: 24
                    implicitWidth: 32
                    color: cellMa.containsMouse ? Theme.colors.accentMuted : "transparent"
                    radius: 3
                    Label {
                        anchors.centerIn: parent
                        text: model.day
                        color: model.month === bulkDateDialog.bulkMonth
                            ? Theme.colors.text : Theme.colors.textSubtle
                        font.pixelSize: Theme.font.sizeSmall
                    }
                    MouseArea {
                        id: cellMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            Subscription.bulk_set_item_metadata_field(
                                root._selectedItemsJson(),
                                bulkDateDialog.fieldName,
                                JSON.stringify(model.date.getTime()))
                            bulkDateDialog.close()
                        }
                    }
                }
            }
        }
    }

    // ── Bulk text/number dialog ─────────────────────────────────────
    Dialog {
        id: bulkTextDialog
        property string fieldName: ""
        property bool numericOnly: false
        title: qsTr("Set %1 for %2 items")
            .arg(fieldName)
            .arg(root.selectedItemPaths.length)
        modal: true
        parent: Overlay.overlay
        x: (parent ? (parent.width  - width)  / 2 : 0)
        y: (parent ? (parent.height - height) / 2 : 0)
        width: 320
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(name, numeric) {
            fieldName = name
            numericOnly = !!numeric
            bulkValueField.text = ""
            open()
            bulkValueField.forceActiveFocus()
        }
        TextField {
            id: bulkValueField
            anchors.left: parent.left
            anchors.right: parent.right
            placeholderText: bulkTextDialog.numericOnly
                ? qsTr("Number") : qsTr("Value")
            validator: bulkTextDialog.numericOnly
                ? doubleVal : null
            DoubleValidator { id: doubleVal; notation: DoubleValidator.StandardNotation }
            onAccepted: bulkTextDialog.accept()
        }
        onAccepted: {
            var raw = bulkValueField.text
            var v
            if (numericOnly) {
                var n = parseFloat(raw)
                v = isNaN(n) ? "null" : JSON.stringify(n)
            } else {
                v = JSON.stringify(raw)
            }
            Subscription.bulk_set_item_metadata_field(
                root._selectedItemsJson(), fieldName, v)
        }
    }

    // ── Column manager dialog ──────────────────────────────────────
    // Discoverable button-driven UI (replaces the old header right-
    // click menus). The host's "Manage Metadata" toolbar button
    // configures + opens this.
    ColumnManagerDialog {
        id: columnManager
        // Built-in Dialog `closed` signal — fires reliably; refresh
        // once on dismiss. See ItemListPanel.qml for why we don't
        // try a per-mutation live refresh here.
        onClosed: root._refreshColumns()
    }
}
