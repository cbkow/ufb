// Single-pane file browser. Composes a Directory QObject (the
// model) with a path bar, error banner, and a swappable view area
// (list / grid / tree).
//
// Selection state lives on the root component so list and grid views
// share it — list-mode index N and grid-mode index N point at the
// same row in the shared `entriesModel`. Tree view uses its own
// treeCurrentPath to avoid clashing with selectedIndexes when the
// user toggles view modes.
//
// Drag/drop (plan-Phase 8): each delegate is a drag source for the
// current selection; the view panel is a drop target that copies/
// moves dropped paths into the panel's current_path. Modifier
// semantics: Ctrl=copy, Shift=move, default=move-same-volume /
// copy-cross-volume.
//
// Active-pane tracking: when the mouse enters this component or the
// user clicks any row/cell, we emit `activated`. The host (Main)
// listens and uses the active pane as the navigation target for
// sidebar clicks.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window  // Screen.devicePixelRatio (HiDPI thumbnail sizing)

import Ufb.Backend 1.0

import "ListingState.js" as ListingState

Rectangle {
    id: root

    // Required: the Directory backing this pane.
    required property var dir
    // Whether this pane is currently the focused pane (driven by the host).
    property bool active: false

    /// Gate for keyboard shortcuts: only fire when this pane is both
    /// the host-elected active pane AND visible. The visibility check
    /// is critical — without it, every project tab's FolderTabView
    /// keeps a stale active-pane reference, multiple panes across tabs
    /// register the same Ctrl+C/V/X shortcut, and Qt's ambiguous-
    /// shortcut routing disables them all silently.
    readonly property bool _shortcutsLive: root.active && root.visible

    // Per-panel view mode. "list" | "grid" | "tree". Persisted PER
    // FOLDER via browser_folder_prefs (the folder-memory system) —
    // this initializer is only the cold default for a brand-new pane
    // before its first folder prefs apply.
    property string viewMode: "list"
    // Grid cell pixel size (the thumbnail box; the cell itself adds
    // padding for the label). 48–256 per plan, default 128.
    property int gridSize: 128

    // ── List-mode column widths (resizable) ───────────────────────────
    // Name + Size are fixed pixel widths the user can drag-resize via
    // the header handles; Modified takes whatever's left so resizing
    // never pushes the last column off-screen. Persisted PER FOLDER in
    // browser_folder_prefs alongside viewMode/sort; unvisited folders
    // inherit the pane's current widths.
    property real _nameColWidth: 280
    property real _sizeColWidth: 100

    /// X (viewPanel coords) of the column-resize guide line, -1 when
    /// no list-header handle is being dragged. Handles track this
    /// during the drag instead of writing widths live — a live width
    /// write re-flows every visible row per mouse move (visibly
    /// stuttery); the real width commits once on release.
    property real _dragGuideX: -1

    // ── Jobs-folder + subscription lookups ────────────────────────────
    // When the user navigates into a "jobs folder" (a Bookmark with
    // is_project_folder=true), the list view exposes a Synced column
    // showing which immediate subdirs are subscribed. Both lookups
    // are case-insensitive maps populated from the JSON properties
    // on the singleton stores; refreshed when those change.
    property var _projectFolderPaths: ({})  // lowercase path -> true
    property var _subscribedJobPaths: ({})  // lowercase path -> true

    readonly property bool _currentIsProjectFolder: {
        if (!root.dir.current_path) return false
        return !!_projectFolderPaths[root.dir.current_path.toLowerCase()]
    }

    function _refreshProjectFolderLookup() {
        var map = {}
        try {
            var arr = JSON.parse(Bookmarks.bookmarks_json || "[]")
            for (var i = 0; i < arr.length; ++i) {
                if (arr[i].isProjectFolder && arr[i].path) {
                    map[String(arr[i].path).toLowerCase()] = true
                }
            }
        } catch (e) {}
        // Mounts flagged isJobsFolder=true behave the same way as
        // a Bookmark with isProjectFolder=true: they expose the
        // New Job button + the Synced column in the file browser.
        // Use volumePath (the local mount point) rather than the
        // UNC nasSharePath - the user navigates into the local
        // path, so that's the key our lookup needs to match.
        try {
            var marr = JSON.parse(Mount.mount_states_json || "[]")
            for (var j = 0; j < marr.length; ++j) {
                if (marr[j].isJobsFolder && marr[j].volumePath) {
                    map[String(marr[j].volumePath).toLowerCase()] = true
                }
            }
        } catch (e2) {}
        _projectFolderPaths = map
    }

    function _refreshSubscriptionLookup() {
        var map = {}
        try {
            var arr = JSON.parse(Subscription.subscriptions_json || "[]")
            for (var i = 0; i < arr.length; ++i) {
                if (arr[i].jobPath) {
                    map[String(arr[i].jobPath).toLowerCase()] = true
                }
            }
        } catch (e) {}
        _subscribedJobPaths = map
    }

    function _isPathSubscribed(p) {
        return !!_subscribedJobPaths[String(p || "").toLowerCase()]
    }

    Connections {
        target: Bookmarks
        function onBookmarks_jsonChanged() { root._refreshProjectFolderLookup() }
    }
    // Mount config changes contribute mount-rooted jobs folders to
    // the same lookup, so refresh whenever the mount snapshot
    // updates (covers Add Mount + isJobsFolder edits in the
    // MountManagerDialog).
    Connections {
        target: Mount
        function onMount_states_jsonChanged() { root._refreshProjectFolderLookup() }
    }
    Connections {
        target: Subscription
        function onSubscriptions_jsonChanged() { root._refreshSubscriptionLookup() }
    }

    // ── Search filter ─────────────────────────────────────────────────
    // Case-insensitive substring match against entry `name`. Triggers
    // a refreshEntries() when changed, which re-runs sorting + the
    // filter pass against the current dir.entries_json.
    property string _filterText: ""
    // The search bar now runs a *recursive deep search* over the current
    // pane root (see _runDeepSearch), not an in-directory filter. Empty
    // text ends the search immediately and returns to the normal listing;
    // non-empty text is debounced so we don't hit es.exe / mdfind /
    // Tantivy on every keystroke.
    on_FilterTextChanged: {
        if (_filterText.length === 0) { searchDebounce.stop(); _endDeepSearch() }
        else searchDebounce.restart()
    }

    // ── List-mode sort state ──────────────────────────────────────────
    // Click any header label to cycle: asc → desc → unsorted (back to
    // insert order). Folders always sort BEFORE files regardless of
    // direction; sort key only affects the order within each group.
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
        refreshEntries()
    }
    function _sortGlyph(key) {
        if (_sortKey !== key || _sortDir === 0) return ""
        return _sortDir === 1 ? " ▲" : " ▼"
    }
    function _compareEntries(a, b) {
        if (a.isDir !== b.isDir) return a.isDir ? -1 : 1
        var key = root._sortKey
        var av, bv
        if (key === "size")          { av = a.size;     bv = b.size }
        else if (key === "modified") { av = a.modified; bv = b.modified }
        else                         { av = a.name;     bv = b.name }
        if (typeof av === "number" && typeof bv === "number") {
            return av < bv ? -1 : (av > bv ? 1 : 0)
        }
        var as = String(av || "").toLowerCase()
        var bs = String(bv || "").toLowerCase()
        return as < bs ? -1 : (as > bs ? 1 : 0)
    }

    signal activated()
    /// Emitted when the user picks "Open in Other Browser" from a row's
    /// context menu. The host (Main) routes the path to the opposite
    /// pane and activates it.
    signal openInOtherBrowserRequested(string path)
    /// Emitted when the user picks "Open in New Tab". Bubbles up
    /// through DualBrowserView to Main, which spawns a new Files
    /// tab and seeds it with this path.
    signal openInNewTabRequested(string path)
    /// Surfaces the workspace's Transcode Queue tab. Emitted by the
    /// "Transcode to MP4" file-context-menu item below.
    signal openTranscodeQueueRequested()
    /// Hide the editable path field (kept in DualBrowserView, hidden
    /// in FolderTabView panes where the selected item already
    /// conveys location and the toolbar is crowded).
    property bool showPathField: true

    /// Show the "Open in Other Browser" context-menu item. True for
    /// dual-pane browsers; false in FolderTabView panes where the
    /// "other browser" concept doesn't apply (the partner pane is
    /// an ItemListPanel or another mode-specific FileBrowser, not a
    /// peer the user can route folders to).
    property bool allowOpenInOtherBrowser: true

    /// Adds "Open in Left/Right Browser" to the row context menu, routing
    /// the item to the MAIN dual browser's pane (via
    /// Window.window.openPathInBrowser). Off for the main dual panes
    /// (they use allowOpenInOtherBrowser instead); on for the project /
    /// shots browsers in FolderTabView, which have no peer pane of their
    /// own but want to push items into the main browser.
    property bool allowOpenInMainBrowser: false

    /// Adds "Add Note…" to the background context menu — creates a
    /// date-prefixed `.mndb` note file (WIP UFB notes format) via
    /// FileOps.create_date_prefixed_note and opens it in the default
    /// app. On only for the notes tab's browser (FolderTabView gates
    /// on the tab folder's name).
    property bool allowAddNote: false

    /// Toggles the path bar between breadcrumb (default) and a
    /// classic editable TextField. Flipped to true by clicking the
    /// trailing empty area in the breadcrumb row, by Ctrl+L, or by
    /// Ctrl+left-click anywhere on the path bar (failsafe when the
    /// breadcrumb fills the row and leaves no trailing space to click).
    /// Flipped back by Enter (after navigation), Esc (cancels), or
    /// the field losing focus.
    property bool _pathEditing: false

    /// Enter path-edit mode: seed the field with the current path,
    /// focus + select-all so a paste or type replaces it outright.
    /// Shared by Ctrl+L, the trailing click affordance, and the
    /// Ctrl+click overlay.
    function _beginPathEdit() {
        root.activated()
        root._pathEditing = true
        pathField.text = root.dir.current_path
        pathField.forceActiveFocus()
        pathField.selectAll()
    }

    /// Splits `current_path` into a list of `{name, path}` pairs for
    /// the breadcrumb. Each `path` is a navigable absolute path that
    /// the segment click commits to. Handles Windows drive letters
    /// (`C:` -> `C:\`) and POSIX roots (`/` as the first crumb).
    /// UNC paths fall through the generic split, which is good
    /// enough until someone hits a `\\server\share` edge case.
    readonly property var _pathSegments: {
        var p = root.dir ? root.dir.current_path : ""
        if (!p || p.length === 0) return []
        var segments = []
        var sep = p.indexOf("\\") >= 0 ? "\\" : "/"
        var rest = p
        if (p.length >= 2 && p.charAt(1) === ":") {
            // Windows drive letter root.
            segments.push({ name: p.substring(0, 2), path: p.substring(0, 2) + sep })
            rest = p.substring(p.charAt(2) === sep ? 3 : 2)
        } else if (p.charAt(0) === "/") {
            segments.push({ name: "/", path: "/" })
            rest = p.substring(1)
        }
        var parts = rest.split(/[\\\/]/).filter(function(s) { return s.length > 0 })
        var accum = segments.length > 0 ? segments[0].path : ""
        for (var i = 0; i < parts.length; ++i) {
            if (accum.length > 0
                && accum.charAt(accum.length - 1) !== sep) accum += sep
            accum += parts[i]
            segments.push({ name: parts[i], path: accum })
        }
        return segments
    }

    /// Collapse state. When true the pane shrinks to the width of
    /// the toggle button, hiding the toolbar + view + footer; only
    /// the toggle + a vertical strip remains. DualBrowserView binds
    /// the SplitView.preferredWidth + maximumWidth to react.
    property bool collapsed: false

    /// Which edge of the parent SplitView this pane sits on -
    /// places the collapse toggle on the OUTER edge (left toggle on
    /// left pane, right toggle on right pane). "left" | "right".
    property string paneSide: "left"

    /// Allow collapsing this pane. False in project/shot folder tabs
    /// where each pane's body is its own information region (item
    /// list / project / renders) and collapsing it would just leave
    /// a useless strip. Hides the footer collapse toggles + the
    /// re-open strip.
    property bool allowCollapse: true

    function toggleCollapsed() { collapsed = !collapsed }

    // ── Per-pane navigation history ───────────────────────────────────
    // Stack of paths visited from this pane. _navHistory grows on every
    // navigate that wasn't itself a back/forward step; _navIndex points
    // at the current entry. Going somewhere new while not at the tip
    // truncates the future. Capped at 50 to avoid unbounded growth.
    property var _navHistory: []
    property int _navIndex: -1
    property bool _navInTransit: false
    function navBack() {
        if (_navIndex <= 0) return
        _navInTransit = true
        _navIndex -= 1
        root.dir.navigate_to(_navHistory[_navIndex])
        Qt.callLater(function() { _navInTransit = false })
    }
    function navForward() {
        if (_navIndex < 0 || _navIndex >= _navHistory.length - 1) return
        _navInTransit = true
        _navIndex += 1
        root.dir.navigate_to(_navHistory[_navIndex])
        Qt.callLater(function() { _navInTransit = false })
    }
    function _recordHistory(path) {
        if (!path || path.length === 0) return
        if (_navInTransit) return
        if (_navIndex >= 0 && _navHistory[_navIndex] === path) return
        var hist = _navHistory.slice(0, _navIndex + 1)
        hist.push(path)
        if (hist.length > 50) hist = hist.slice(hist.length - 50)
        _navHistory = hist
        _navIndex = hist.length - 1
    }
    Connections {
        target: root.dir
        function onCurrent_pathChanged() {
            root._recordHistory(root.dir.current_path)
            // Navigating away ends an active search — opening a folder from
            // the results returns to a normal listing of the destination.
            if (root._searchActive && root._filterText.length > 0)
                root._filterText = ""
            // Per-folder view prefs (Finder-style): flush any pending
            // save for the folder we're LEAVING (the debounce timer
            // must not write the old folder's values against the new
            // path), then apply the destination's remembered prefs.
            // Folders with no saved prefs inherit the pane's current
            // state — never reset.
            root._flushFolderPrefs()
            root._applyFolderPrefs()
        }
    }

    // ── Deep (recursive) search ───────────────────────────────────────
    // The search bar runs a recursive search over the current pane root:
    // OS-backed by default (es.exe / mdfind), or the Tantivy index when
    // _searchIndexed is toggled. Results (folders + files, anywhere under
    // the root) replace the directory listing while a query is live.
    property bool _searchIndexed: false   // false = OS, true = Tantivy
    property bool _searchActive: false     // a query is live → show results
    Search { id: deepSearch }

    // Debounce keystrokes before hitting the backend.
    Timer {
        id: searchDebounce
        interval: 200
        onTriggered: root._runDeepSearch(root._filterText)
    }

    function _runDeepSearch(text) {
        if (!text || text.length === 0) { root._endDeepSearch(); return }
        if (!root._searchActive) {
            root._searchActive = true
            deepSearch.start(root.dir.current_path, root._searchIndexed)
        }
        deepSearch.set_query_text(text)
    }
    function _endDeepSearch() {
        if (root._searchActive) {
            root._searchActive = false
            deepSearch.stop()
            root.refreshEntries()   // back to the normal directory listing
        }
    }
    function _toggleSearchMode() {
        root._searchIndexed = !root._searchIndexed
        // Restart the session cleanly on the new backend if mid-search.
        if (root._filterText.length > 0) {
            deepSearch.stop()
            root._searchActive = true
            deepSearch.start(root.dir.current_path, root._searchIndexed)
            deepSearch.set_query_text(root._filterText)
        }
    }

    Connections {
        target: deepSearch
        function onResults_jsonChanged() {
            if (root._searchActive) root.refreshEntries()
        }
    }

    /// Extensions we let through the "Transcode to MP4" menu item.
    /// Lowercase compare; matches the legacy app's set.
    readonly property var _videoExtensions: ([
        "mov", "mp4", "mkv", "avi", "mxf", "webm", "m4v",
        "wmv", "flv", "mpeg", "mpg", "ts", "vob", "ogv"
    ])
    function _isVideoPath(p) {
        if (!p) return false
        var idx = p.lastIndexOf(".")
        if (idx < 0) return false
        return _videoExtensions.indexOf(
            p.substring(idx + 1).toLowerCase()) >= 0
    }
    function _selectedVideoPaths() {
        var sel = selectedPaths()
        var out = []
        for (var i = 0; i < sel.length; ++i) {
            if (_isVideoPath(sel[i])) out.push(sel[i])
        }
        return out
    }
    function _selectionContainsVideo() {
        return _selectedVideoPaths().length > 0
    }

    // ── Selection state ───────────────────────────────────────────────
    // Indexes into entriesModel that are currently selected. Plain
    // JS array; small enough that linear search is fine.
    property var selectedIndexes: []

    /// Path to highlight once entriesModel has finished populating
    /// from the next dir.entries_json refresh. Used by the ufb://
    /// deep-link flow to land the user on the parent folder of a
    /// file-target URI with the file already selected. Cleared
    /// after a successful match (or on the next navigation).
    property string selectAfterLoadPath: ""
    /// The path entriesModel was last populated from. Lets refreshEntries
    /// tell an in-place reload (same dir, contents changed by a file op)
    /// from a genuine navigation: the former preserves selection / cursor
    /// / scroll, the latter resets them. "" until the first load.
    property string _lastListedPath: ""
    /// Same idea as _lastListedPath, for the tree view's root: lets
    /// refreshTree preserve scroll on an in-place rebuild but reset to
    /// the top when the root changed (navigation / view switch).
    property string _lastTreeRoot: ""
    // Anchor for shift-range selection. -1 = none.
    property int anchorIndex: -1
    // Cursor index — drives keyboard nav arrows + the "current" highlight.
    property int currentIndex: -1

    function _isSelected(i) { return selectedIndexes.indexOf(i) >= 0 }
    function _setSelection(arr) {
        // Sort + dedupe so the binding sees stable arrays.
        arr = arr.filter((v, idx, a) => a.indexOf(v) === idx).sort((a, b) => a - b)
        selectedIndexes = arr
    }
    function selectOnly(i) {
        if (i < 0) { _setSelection([]); return }
        _setSelection([i])
        anchorIndex = i
        currentIndex = i
    }
    function toggleSelection(i) {
        if (i < 0) return
        var s = selectedIndexes.slice()
        var pos = s.indexOf(i)
        if (pos >= 0) s.splice(pos, 1); else s.push(i)
        _setSelection(s)
        anchorIndex = i
        currentIndex = i
    }
    function selectRange(from, to) {
        if (from < 0) from = to
        var lo = Math.min(from, to), hi = Math.max(from, to)
        var s = []
        for (var i = lo; i <= hi; ++i) s.push(i)
        _setSelection(s)
        currentIndex = to
    }
    function selectAll() {
        var s = []
        for (var i = 0; i < entriesModel.count; ++i) s.push(i)
        _setSelection(s)
    }
    function clearSelection() {
        _setSelection([])
        anchorIndex = -1
        // Tree mode keeps its own selection set; clear it too so
        // copy/delete/etc. don't act on stale tree paths after the
        // user clicks the background or switches view modes.
        root.treeSelectedPaths = []
        root.treeCurrentPath = ""
    }
    function selectionCount() { return selectedIndexes.length }
    function selectedPaths() {
        if (root.viewMode === "tree") {
            // Multi-select set; treeCurrentPath is the anchor, but if
            // somehow the set is empty fall back to the anchor.
            if (root.treeSelectedPaths.length > 0) return root.treeSelectedPaths.slice()
            return root.treeCurrentPath.length > 0 ? [root.treeCurrentPath] : []
        }
        var out = []
        for (var i = 0; i < selectedIndexes.length; ++i) {
            var idx = selectedIndexes[i]
            if (idx >= 0 && idx < entriesModel.count) {
                out.push(entriesModel.get(idx).path)
            }
        }
        return out
    }
    /// The directory an action like Paste / New Folder should target.
    /// In tree mode the user's intent is the selected tree folder (so
    /// you can paste/create into a subfolder without first navigating
    /// into it); everywhere else it's the pane's current path. Falls back
    /// to current_path if the tree selection isn't a directory.
    function _effectiveTargetDir() {
        if (root.viewMode === "tree" && root.treeCurrentPath.length > 0) {
            var idx = root._treeIndexOfPath(root.treeCurrentPath)
            if (idx >= 0) {
                var node = treeModel.get(idx)
                if (node && node.isDir) return root.treeCurrentPath
            }
        }
        return root.dir.current_path
    }
    /// Re-evaluates whenever any of the underlying state changes;
    /// menu items use this for their `enabled` bindings.
    readonly property int _currentSelectionCount: viewMode === "tree"
        ? treeSelectedPaths.length
        : selectedIndexes.length
    function activateRow(i) {
        // Open the row at index i — folders navigate; files hand off
        // to the OS default app via FileOps.open_file.
        if (i < 0 || i >= entriesModel.count) return
        var e = entriesModel.get(i)
        if (e.isDir) {
            root.dir.navigate_to(e.path)
        } else {
            FileOps.open_file(e.path)
        }
    }

    // ── Drag/drop helpers ─────────────────────────────────────────────
    // True while a drag-with-uri-list payload is hovering this pane.
    // Drives the panel border accent.
    property bool dropActive: false

    // Path of the directory row currently under a hovering drag, or ""
    // when the drag is over empty space / a non-folder. Drives the
    // per-row drop highlight and is the target `onDropped` resolves to.
    property string _dropTargetPath: ""

    // True while a system drag this pane started is in flight. Mirrors
    // the persistent `dragProxy`'s Drag.active so model rebuilds can be
    // deferred for its duration (see refreshEntries).
    readonly property bool _systemDragActive: dragProxy.Drag.active
    property bool _refreshDeferred: false
    on_SystemDragActiveChanged: {
        if (!_systemDragActive && root._refreshDeferred) {
            root._refreshDeferred = false
            root.refreshEntries()
        }
    }

    /// Resolve which paths a drag starting on `rowPath` should carry in
    /// the list/grid views: the whole current selection if the pressed
    /// row is part of it, otherwise just that row.
    function _dragPathsForRow(rowPath) {
        var paths = root.selectedPaths()
        if (paths.indexOf(rowPath) === -1) {
            paths = rowPath ? [rowPath] : []
        }
        return paths
    }

    /// Start a system drag carrying `paths` as text/uri-list, using a
    /// snapshot of `item` (the pressed delegate) as the drag pixmap.
    /// Routed through the persistent `dragProxy` rather than the
    /// delegate itself: a delegate is recycled the instant an async
    /// listing lands (refreshEntries → entriesModel.clear()), and
    /// destroying a QQuickItem that holds a live `Drag` attached
    /// property aborts with `QQmlData::destroyed` → qFatal. The proxy
    /// outlives every model rebuild, so the drag it owns can't be pulled
    /// out from under Qt.
    ///
    /// `Drag.imageSource` must be set *before* `Drag.active = true` for
    /// the platform to pick it up, and `grabToImage` is async, so the
    /// activation happens in the grab callback. The grab result is an
    /// independent snapshot — it stays valid even if `item` is recycled
    /// before the drag ends. We always activate, with or without the
    /// image, so a failed/empty grab can never swallow the drag.
    function _startSystemDrag(item, paths) {
        if (!paths || paths.length === 0) return
        // Set mimeData imperatively (not via a declarative
        // `Drag.mimeData: {...}` binding): an object-literal binding that
        // references a changing property doesn't reliably re-evaluate
        // before Drag.active flips, so a second multi-select drag could
        // carry a stale (often single-path) uri-list. Assigning the whole
        // map here, right before activation, guarantees every drag ships
        // the full current selection.
        dragProxy.Drag.mimeData = {
            "text/uri-list": FileOps.to_uri_list(JSON.stringify(paths))
        }
        _dragActivateFallback.stop()
        var grabbed = false
        if (item && item.grabToImage) {
            grabbed = item.grabToImage(function(result) {
                // The fallback timer may have already started the drag if
                // the grab was slow (software rendering); don't restart it.
                if (dragProxy.Drag.active) return
                _dragActivateFallback.stop()
                dragProxy.Drag.imageSource = (result && result.url) ? result.url : ""
                dragProxy.Drag.active = true
            })
        }
        if (!grabbed) {
            // Grab couldn't even be scheduled — activate now, no preview.
            dragProxy.Drag.imageSource = ""
            dragProxy.Drag.active = true
        } else {
            // Grab scheduled; arm the safety net in case its callback is
            // delayed or never delivered.
            _dragActivateFallback.restart()
        }
    }

    /// Map a point in the active view (drop-area coordinates, which
    /// share the view's geometry) to the path of the folder row under
    /// it, or "" if the point isn't over a directory. Lets a drop land
    /// inside the hovered subfolder instead of always the current dir.
    /// Return keyboard focus to the active view (list/grid/tree). Called
    /// when an overlay that grabbed focus (e.g. the preview lightbox) closes,
    /// so the current selection stays keyboard-actionable (Space re-opens the
    /// preview) without the user having to click to reselect.
    function refocusView() {
        var view = root.viewMode === "grid" ? entryGrid
                 : root.viewMode === "tree" ? entryTree
                 : entryList
        if (view) view.forceActiveFocus()
    }

    function _folderPathAt(x, y) {
        var view = root.viewMode === "grid" ? entryGrid
                 : root.viewMode === "tree" ? entryTree
                 : entryList
        if (!view) return ""
        var idx = view.indexAt(x + view.contentX, y + view.contentY)
        if (idx < 0) return ""
        var row = view.model.get(idx)
        if (!row || !row.isDir) return ""
        return row.path || ""
    }

    /// Pull the file paths out of a DragEvent. Qt6 has been observed
    /// to truncate `drop.urls` to a single entry for native multi-file
    /// drags from Explorer / Finder, while the raw `text/uri-list`
    /// MIME string still carries every URI. Read both and prefer
    /// whichever produced more entries.
    function _collectDropPaths(drop) {
        var uris = []
        try {
            if (drop.urls && drop.urls.length > 0) {
                for (var i = 0; i < drop.urls.length; ++i) {
                    uris.push(String(drop.urls[i]))
                }
            }
        } catch (e) {}
        var raw = ""
        try { raw = drop.getDataAsString("text/uri-list") || "" } catch (e) {}
        if (raw.length > 0) {
            var lines = raw.split(/\r?\n/)
            var rawUris = []
            for (var j = 0; j < lines.length; ++j) {
                var line = lines[j].trim()
                if (line.length > 0 && line.charAt(0) !== "#") {
                    rawUris.push(line)
                }
            }
            if (rawUris.length > uris.length) uris = rawUris
        }
        if (uris.length === 0) return []
        try {
            return JSON.parse(FileOps.urls_to_paths(JSON.stringify(uris)))
        } catch (e) {
            console.warn("FileBrowser: urls_to_paths failed:", e)
            return []
        }
    }

    /// Drop dispatcher shared by all view-mode DropAreas. `targetPath`
    /// is the destination folder. Internal UFB→UFB drags pop the
    /// Copy/Move/Cancel dialog; external (Explorer/Finder/etc) drops
    /// always copy without prompting.
    function _handleDrop(drop, targetPath) {
        if (!targetPath || targetPath.length === 0) {
            drop.accepted = false
            return
        }
        var paths = root._collectDropPaths(drop)
        if (paths.length === 0) {
            drop.accepted = false
            return
        }
        // drop.source is the QQuickItem that started the drag for
        // internal Drag.Automatic sources, and null for native OS
        // drags. We use that to decide whether to prompt: external
        // drops always copy (move-from-Explorer is risky and Explorer
        // already handles its own modifier semantics), internal drops
        // ask. We always accept as Qt.CopyAction so Qt's source-side
        // OS layer never does its own MoveAction-driven cleanup —
        // when the user picks "Move", we handle the source delete
        // ourselves via move_files.
        var isExternal = !drop.source
        console.log("[FileBrowser] _handleDrop:",
            "paths=", paths.length,
            "first=", paths[0],
            "target=", targetPath,
            "external=", isExternal)
        drop.accept(Qt.CopyAction)
        if (isExternal) {
            FileOps.copy_files(JSON.stringify(paths), targetPath)
            // Refresh the affected pane shortly to surface the new
            // entries. Proper "known mutation" hooks land with
            // Phase 13's progress strip; this is the minimum that
            // makes the user see results.
            refreshAfterDropTimer.restart()
            return
        }
        root._pendingDropPaths = paths
        root._pendingDropTarget = targetPath
        dragChoiceDialog.open()
        refreshAfterDropTimer.restart()
    }

    // Fallback refresh after locally-initiated ops. The authoritative
    // trigger is now FileOps.onDirsChanged (instant + cross-pane); these
    // timers remain as a safety net for op types that don't yet emit it
    // (e.g. template/date-prefixed folder creation) and for any op whose
    // completion event is somehow missed. A redundant fire is harmless —
    // the reload preserves selection/scroll/expansion.
    Timer {
        id: refreshAfterDropTimer
        interval: 350
        onTriggered: {
            root.dir.refresh()
            if (root.viewMode === "tree" && root.dir.current_path.length > 0) {
                tree.refresh()
            }
            refreshAfterDropTimer2.restart()
        }
    }
    Timer {
        id: refreshAfterDropTimer2
        interval: 1500
        onTriggered: {
            root.dir.refresh()
            if (root.viewMode === "tree" && root.dir.current_path.length > 0) {
                tree.refresh()
            }
        }
    }

    // ── Context menus ────────────────────────────────────────────────
    // Plan-Phase 9 first cut: per-FileBrowser QML Action items rather
    // than the spec'd C++ AppActions singleton. Keeps the surface
    // local to the panel — selection/clipboard state is per-pane and
    // the centralised C++ singleton would need a "focused pane"
    // tracker to do the same job. Promote to C++ when keyboard
    // shortcuts get heavier.

    /// Holds the entry record (single object) the right-click landed
    /// on. Menu actions that need "the row under the cursor" read
    /// from here.
    property var _menuEntry: null
    /// Set true when a row's MouseArea fired _popupRowMenu so the
    /// panel-level TapHandler skips its own _popupBgMenu (otherwise
    /// both menus would race / stack on a single right-click).
    property bool _menuClickConsumed: false

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
    function _joinPath(parent, name) {
        if (!parent) return name
        var sep = parent.indexOf("\\") >= 0 ? "\\" : "/"
        return parent + sep + name
    }
    function _clipboardHasPaths() {
        try {
            var intent = JSON.parse(FileOps.clipboard_paste_intent())
            return intent && intent.paths && intent.paths.length > 0
        } catch (e) { return false }
    }
    function _formatYYMMDD(d) {
        var y = String(d.getFullYear() % 100).padStart(2, "0")
        var m = String(d.getMonth() + 1).padStart(2, "0")
        var dd = String(d.getDate()).padStart(2, "0")
        return y + m + dd
    }
    function _formatHHMM(d) {
        var h = String(d.getHours()).padStart(2, "0")
        var mm = String(d.getMinutes()).padStart(2, "0")
        return h + mm
    }

    /// Right-click handler shared by all three view delegates.
    /// `entry` is a model record; if it isn't already in selection,
    /// promote to single-select before popping the menu. `index` is
    /// the row index in entriesModel for list/grid; pass -1 for tree
    /// (selection there lives in treeCurrentPath, already set by the
    /// caller).
    function _popupRowMenu(index, entry) {
        if (index >= 0 && !root._isSelected(index)) {
            root.selectOnly(index)
        }
        root._menuEntry = {
            path: entry.path,
            name: entry.name,
            isDir: entry.isDir
        }
        // Mark this right-click as consumed so the panel-level
        // TapHandler doesn't also fire _popupBgMenu.
        root._menuClickConsumed = true
        fileMenu.popup()
    }
    function _popupBgMenu() {
        root.clearSelection()
        bgMenu.popup()
    }

    Action {
        id: openAction
        text: qsTr("Open")
        onTriggered: {
            var e = root._menuEntry
            if (!e) return
            if (e.isDir) {
                root.dir.navigate_to(e.path)
            } else {
                FileOps.open_file(e.path)
            }
        }
    }
    Action {
        id: revealAction
        text: qsTr("Reveal in Explorer")
        onTriggered: {
            var e = root._menuEntry
            if (e) FileOps.reveal_in_file_manager(e.path)
        }
    }
    Action {
        id: cutAction
        text: qsTr("Cut")
        shortcut: root._shortcutsLive ? "Ctrl+X" : ""
        onTriggered: {
            var paths = root.selectedPaths()
            if (paths.length === 0) return
            FileOps.clipboard_cut_paths(JSON.stringify(paths))
        }
    }
    Action {
        id: copyAction
        text: qsTr("Copy")
        shortcut: root._shortcutsLive ? "Ctrl+C" : ""
        onTriggered: {
            var paths = root.selectedPaths()
            if (paths.length === 0) return
            FileOps.clipboard_copy_paths(JSON.stringify(paths))
        }
    }
    Action {
        id: pasteAction
        text: qsTr("Paste")
        shortcut: root._shortcutsLive ? "Ctrl+V" : ""
        onTriggered: {
            var intent = { paths: [], effect: "copy" }
            try { intent = JSON.parse(FileOps.clipboard_paste_intent()) } catch (e) {}
            if (!intent.paths || intent.paths.length === 0) return
            var dest = root._effectiveTargetDir()
            if (!dest || dest.length === 0) return
            if (intent.effect === "move") {
                FileOps.move_files(JSON.stringify(intent.paths), dest)
            } else {
                FileOps.copy_files(JSON.stringify(intent.paths), dest)
            }
            refreshAfterDropTimer.restart()
        }
    }
    Action {
        id: renameAction
        text: qsTr("Rename")
        shortcut: root._shortcutsLive ? "F2" : ""
        onTriggered: {
            var paths = root.selectedPaths()
            if (paths.length !== 1) return
            renameDialog.openFor(paths[0])
        }
    }
    Action {
        id: deleteAction
        text: qsTr("Delete")
        shortcut: root._shortcutsLive ? "Del" : ""
        onTriggered: {
            var paths = root.selectedPaths()
            if (paths.length === 0) return
            deleteConfirmDialog.openFor(paths)
        }
    }
    Action {
        id: copyPathAction
        text: qsTr("Copy Path")
        onTriggered: {
            var e = root._menuEntry
            if (e) FileOps.clipboard_copy_text(e.path)
        }
    }
    Action {
        id: copyFilenameAction
        text: qsTr("Copy Filename")
        onTriggered: {
            var e = root._menuEntry
            if (e) FileOps.clipboard_copy_text(e.name)
        }
    }
    Action {
        id: copyUfbLinkAction
        text: qsTr("Copy ufb:// Link")
        onTriggered: {
            var e = root._menuEntry
            if (e) FileOps.clipboard_copy_text(FileOps.build_ufb_uri(e.path))
        }
    }
    Action {
        id: copyUnionLinkAction
        text: qsTr("Copy union:// Link")
        onTriggered: {
            var e = root._menuEntry
            if (e) FileOps.clipboard_copy_text(FileOps.build_union_uri(e.path))
        }
    }
    Action {
        id: copyCurrentUfbLinkAction
        text: qsTr("Copy ufb:// Link")
        onTriggered: {
            if (root.dir.current_path.length > 0) {
                FileOps.clipboard_copy_text(FileOps.build_ufb_uri(root.dir.current_path))
            }
        }
    }
    Action {
        id: newFolderAction
        text: qsTr("New Folder…")
        shortcut: root._shortcutsLive ? "Ctrl+Shift+N" : ""
        onTriggered: newFolderDialog.openFor(root._effectiveTargetDir())
    }
    Action {
        id: newDateFolderAction
        text: qsTr("New Date Folder")
        onTriggered: {
            var d = new Date()
            var base = root._formatYYMMDD(d)
            var newPath = FileOps.find_available_path(root._effectiveTargetDir(), base)
            if (!newPath || newPath.length === 0) return
            var err = FileOps.create_directory(newPath)
            if (err.length > 0) console.warn("New Date Folder failed:", err)
            else refreshAfterDropTimer.restart()
        }
    }
    Action {
        id: newTimeFolderAction
        text: qsTr("New Time Folder")
        onTriggered: {
            var d = new Date()
            var base = root._formatHHMM(d)
            var newPath = FileOps.find_available_path(root._effectiveTargetDir(), base)
            if (!newPath || newPath.length === 0) return
            var err = FileOps.create_directory(newPath)
            if (err.length > 0) console.warn("New Time Folder failed:", err)
            else refreshAfterDropTimer.restart()
        }
    }
    Action {
        id: newUfbFolderAction
        text: qsTr("New UFB Folder…")
        onTriggered: ufbFolderDialog.openFor(root.dir.current_path)
    }
    Action {
        id: addNoteAction
        text: qsTr("Add Note…")
        onTriggered: addNoteDialog.openFor(root.dir.current_path)
    }
    Action {
        id: openInOtherBrowserAction
        text: qsTr("Open in Other Browser")
        onTriggered: {
            var e = root._menuEntry
            if (!e) return
            // The host (Main) routes the path to the *other* pane.
            root.openInOtherBrowserRequested(e.path)
        }
    }
    Action {
        id: openInNewTabAction
        text: qsTr("Open in New Tab")
        onTriggered: {
            var e = root._menuEntry
            if (!e) return
            // For directories, navigate the new tab to the dir
            // itself. For files, fall back to the current dir of
            // this browser - a Files tab is always a folder
            // browser, not a file viewer, so opening the file's
            // containing folder is the right approximation.
            var target = e.isDir ? e.path : root.dir.current_path
            if (!target || target.length === 0) return
            root.openInNewTabRequested(target)
        }
    }
    Action {
        id: syncAsJobAction
        text: qsTr("Sync as Job…")
        onTriggered: {
            var e = root._menuEntry
            if (!e || !e.isDir) return
            syncAsJobDialog.openFor(e.path, e.name)
        }
    }
    Action {
        id: showShellMenuAction
        text: Qt.platform.os === "windows" ? qsTr("Show Shell Menu") : qsTr("Show Finder Menu")
        onTriggered: {
            var e = root._menuEntry
            if (e) FileOps.show_shell_menu(e.path)
        }
    }
    Action {
        id: copyCurrentPathAction
        text: qsTr("Copy Current Path")
        onTriggered: FileOps.clipboard_copy_text(root.dir.current_path)
    }
    Action {
        id: refreshAction
        text: qsTr("Refresh")
        shortcut: root._shortcutsLive ? "F5" : ""
        onTriggered: {
            root.dir.refresh()
            if (root.viewMode === "tree" && root.dir.current_path.length > 0) {
                tree.refresh()
            }
        }
    }
    Action {
        id: editPathAction
        // Same gesture as every browser: Ctrl+L jumps into the path
        // bar with everything selected, ready to type or paste.
        shortcut: root._shortcutsLive && root.showPathField ? "Ctrl+L" : ""
        onTriggered: root._beginPathEdit()
    }

    Menu {
        id: fileMenu
        // HANG FIX (Windows, Qt 6.11, 2026-08-24). This is the
        // FluentWinUI3 style's own contentItem verbatim, plus
        // `cacheBuffer`, which instantiates every delegate up front.
        //
        // ListView.contentHeight is otherwise an *estimate* extrapolated
        // from whichever delegates happen to be instantiated, and our
        // menus have hidden items with height 0, so the estimate depends
        // on which subset is loaded. When the menu can't fit above or
        // below the cursor (long menu, cursor mid-screen on a 1080p
        // display), Qt's popup positioner clamps the height -> fewer
        // delegates -> different estimate -> implicitHeight changes ->
        // the style's `height: __heightScale * implicitHeight` binding
        // fires -> positioner clamps again -> ... The GUI thread never
        // leaves that polish pass and Windows reports the app as not
        // responding (looked like a 30%-of-right-clicks "crash").
        // Confirmed from three minidumps of a hung 1.1.3: identical
        // stack, constant depth, all inside
        // QQuickPopupPositioner::reposition <- QQuickPopup::setHeight
        // <- ListView contentHeight binding. With every delegate
        // instantiated contentHeight is exact and the loop can't start.
        // Native menus (macOS) never go through this path. bgMenu gets
        // the same treatment.
        contentItem: ListView {
            implicitHeight: contentHeight
            model: fileMenu.contentModel
            interactive: Window.window
                         ? contentHeight + fileMenu.topPadding + fileMenu.bottomPadding > fileMenu.height
                         : false
            currentIndex: fileMenu.currentIndex
            spacing: 4
            clip: true
            cacheBuffer: 1000000
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        // `_clipboardHasPaths()` reads from a non-reactive invokable,
        // so binding `enabled:` directly to it would latch at Menu
        // creation time and never refresh — leaving Paste greyed out
        // for the lifetime of any FileBrowser instance whose Menu was
        // built while the clipboard happened to be empty (most
        // commonly: a freshly-opened project tab). Re-evaluate on
        // every popup via aboutToShow.
        property bool _pasteAvailable: false
        onAboutToShow: _pasteAvailable = root._clipboardHasPaths()
        MenuItem { action: openAction }
        MenuItem {
            action: openInOtherBrowserAction
            visible: root.allowOpenInOtherBrowser
            height: visible ? implicitHeight : 0
        }
        MenuItem {
            text: qsTr("Open in Left Browser")
            visible: root.allowOpenInMainBrowser
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (root._menuEntry && Window.window && Window.window.openPathInBrowser)
                    Window.window.openPathInBrowser("left", root._menuEntry.path)
            }
        }
        MenuItem {
            text: qsTr("Open in Right Browser")
            visible: root.allowOpenInMainBrowser
            height: visible ? implicitHeight : 0
            onTriggered: {
                if (root._menuEntry && Window.window && Window.window.openPathInBrowser)
                    Window.window.openPathInBrowser("right", root._menuEntry.path)
            }
        }
        MenuItem { action: openInNewTabAction }
        // "Sync as Job…" is folder-only (project-folder heuristic
        // deferred — show on every directory for now).
        MenuItem {
            action: syncAsJobAction
            visible: root._menuEntry && root._menuEntry.isDir
            height: visible ? implicitHeight : 0
        }
        MenuSeparator {}
        MenuItem { action: cutAction; enabled: root._currentSelectionCount > 0 }
        MenuItem { action: copyAction; enabled: root._currentSelectionCount > 0 }
        MenuItem { action: pasteAction; enabled: fileMenu._pasteAvailable }
        MenuSeparator {}
        MenuItem { action: renameAction; enabled: root._currentSelectionCount === 1 }
        MenuItem { action: deleteAction; enabled: root._currentSelectionCount > 0 }
        MenuSeparator {}
        MenuItem { action: copyPathAction }
        MenuItem { action: copyFilenameAction }
        MenuItem { action: copyUfbLinkAction }
        MenuItem { action: copyUnionLinkAction }
        MenuSeparator {}
        MenuSeparator {}
        MenuItem {
            text: qsTr("Transcode to MP4")
            visible: root._selectionContainsVideo()
            height: visible ? implicitHeight : 0
            onTriggered: {
                var paths = root._selectedVideoPaths()
                if (paths.length === 0) return
                Transcode.add_jobs(JSON.stringify(paths))
                root.openTranscodeQueueRequested()
            }
        }
        MenuSeparator {}
        MenuItem { action: revealAction }
        MenuItem { action: showShellMenuAction }
    }

    Menu {
        id: bgMenu
        // Same hang-proofing as fileMenu — see the comment there.
        contentItem: ListView {
            implicitHeight: contentHeight
            model: bgMenu.contentModel
            interactive: Window.window
                         ? contentHeight + bgMenu.topPadding + bgMenu.bottomPadding > bgMenu.height
                         : false
            currentIndex: bgMenu.currentIndex
            spacing: 4
            clip: true
            cacheBuffer: 1000000
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        // Same Paste-availability story as fileMenu — see comment there.
        property bool _pasteAvailable: false
        onAboutToShow: _pasteAvailable = root._clipboardHasPaths()
        MenuItem { action: newFolderAction }
        MenuItem { action: newUfbFolderAction }
        MenuItem { action: newDateFolderAction }
        MenuItem { action: newTimeFolderAction }
        MenuItem {
            action: addNoteAction
            visible: root.allowAddNote
            height: visible ? implicitHeight : 0
        }
        MenuSeparator {}
        MenuItem { action: pasteAction; enabled: bgMenu._pasteAvailable }
        MenuSeparator {}
        MenuItem { action: copyCurrentPathAction }
        MenuItem { action: copyCurrentUfbLinkAction }
        MenuSeparator {}
        MenuItem { action: refreshAction }
    }

    // ── Modal dialogs ────────────────────────────────────────────────
    NewJobDialog {
        id: newJobDialog
        // Refresh the listing so the new job folder appears
        // immediately. The Rust create_job_from_template returned
        // the path synchronously, but the file system may need a
        // moment to settle - go through refreshAfterDropTimer for
        // consistency with the other ops.
        onCreated: refreshAfterDropTimer.restart()
    }

    Dialog {
        id: newFolderDialog
        property string parentPath: ""
        title: qsTr("New Folder")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(parent) {
            parentPath = parent
            nameField.text = ""
            open()
            nameField.forceActiveFocus()
        }
        ColumnLayout {
            spacing: 8
            Label {
                text: qsTr("Create folder in:\n%1").arg(newFolderDialog.parentPath)
                color: Theme.colors.textMuted
                font.pixelSize: 11
            }
            TextField {
                id: nameField
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                placeholderText: qsTr("Folder name")
                onAccepted: newFolderDialog.accept()
            }
        }
        onAccepted: {
            var name = nameField.text.trim()
            if (name.length === 0) return
            var newPath = root._joinPath(parentPath, name)
            var err = FileOps.create_directory(newPath)
            if (err.length > 0) {
                console.warn("New Folder failed:", err)
                return
            }
            refreshAfterDropTimer.restart()
        }
    }

    Dialog {
        id: syncAsJobDialog
        property string folderPath: ""
        property string defaultName: ""
        title: qsTr("Sync as Job")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(path, defName) {
            folderPath = path
            defaultName = defName
            jobNameField.text = defName
            open()
            jobNameField.forceActiveFocus()
            jobNameField.selectAll()
        }
        ColumnLayout {
            spacing: 8
            Label {
                text: qsTr("Subscribe to job folder:\n%1").arg(syncAsJobDialog.folderPath)
                color: Theme.colors.textMuted
                font.pixelSize: 11
            }
            TextField {
                id: jobNameField
                Layout.fillWidth: true
                Layout.preferredWidth: 360
                placeholderText: qsTr("Display name")
                onAccepted: syncAsJobDialog.accept()
            }
        }
        onAccepted: {
            var name = jobNameField.text.trim()
            if (name.length === 0) name = syncAsJobDialog.defaultName
            Subscription.subscribe_to_job(syncAsJobDialog.folderPath, name)
        }
    }

    Dialog {
        id: ufbFolderDialog
        property string parentPath: ""
        title: qsTr("New UFB Folder")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(parent) {
            parentPath = parent
            ufbLabelField.text = ""
            open()
            ufbLabelField.forceActiveFocus()
        }
        ColumnLayout {
            spacing: 8
            Label {
                text: qsTr("Create UFB folder in:\n%1\n\nFinal name will be %2{a-z}_<label>")
                    .arg(ufbFolderDialog.parentPath)
                    .arg(root._formatYYMMDD(new Date()))
                color: Theme.colors.textMuted
                font.pixelSize: 11
            }
            TextField {
                id: ufbLabelField
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                placeholderText: qsTr("Folder label")
                onAccepted: ufbFolderDialog.accept()
            }
        }
        onAccepted: {
            var label = ufbLabelField.text.trim()
            if (label.length === 0) return
            var prefix = root._formatYYMMDD(new Date())
            var newPath = FileOps.create_ufb_folder(parentPath, prefix, label)
            if (!newPath || newPath.length === 0) {
                console.warn("New UFB Folder failed for", prefix, label)
                return
            }
            refreshAfterDropTimer.restart()
        }
    }

    Dialog {
        id: addNoteDialog
        property string parentPath: ""
        title: qsTr("Add Note")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(parent) {
            parentPath = parent
            noteNameField.text = ""
            open()
            noteNameField.forceActiveFocus()
        }
        ColumnLayout {
            spacing: 8
            Label {
                text: qsTr("Create note in:\n%1\n\nFinal name will be %2{a-z}_<name>.mndb")
                    .arg(addNoteDialog.parentPath)
                    .arg(root._formatYYMMDD(new Date()))
                color: Theme.colors.textMuted
                font.pixelSize: 11
            }
            TextField {
                id: noteNameField
                Layout.fillWidth: true
                Layout.preferredWidth: 320
                placeholderText: qsTr("Note name")
                onAccepted: addNoteDialog.accept()
            }
        }
        onAccepted: {
            var name = noteNameField.text.trim()
            if (name.length === 0) return
            var newPath = FileOps.create_date_prefixed_note(parentPath, name)
            if (!newPath || newPath.length === 0) {
                console.warn("Add Note failed for", name)
                return
            }
            refreshAfterDropTimer.restart()
            // Hand the new note to the default .mndb handler. The UFB
            // notes app is WIP — until it exists (and registers the
            // extension) this open is a harmless no-op/OS shrug.
            FileOps.open_file(newPath)
        }
    }

    Dialog {
        id: renameDialog
        property string oldPath: ""
        title: qsTr("Rename")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(path) {
            oldPath = path
            renameField.text = root._fileName(path)
            open()
            renameField.forceActiveFocus()
            // Select the basename (everything before the final dot)
            var dot = renameField.text.lastIndexOf(".")
            if (dot > 0) {
                renameField.select(0, dot)
            } else {
                renameField.selectAll()
            }
        }
        ColumnLayout {
            spacing: 8
            Label {
                text: qsTr("Rename:\n%1").arg(renameDialog.oldPath)
                color: Theme.colors.textMuted
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
            if (newName.length === 0) return
            if (newName === root._fileName(oldPath)) return
            var newPath = root._joinPath(root._parentDir(oldPath), newName)
            var err = FileOps.rename_path(oldPath, newPath)
            if (err.length > 0) {
                console.warn("Rename failed:", err)
                return
            }
            refreshAfterDropTimer.restart()
        }
    }

    Dialog {
        id: deleteConfirmDialog
        property var pendingPaths: []
        title: qsTr("Delete")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        function openFor(paths) {
            pendingPaths = paths
            open()
        }
        Label {
            text: deleteConfirmDialog.pendingPaths.length === 1
                ? qsTr("Move 1 item to the recycle bin?")
                : qsTr("Move %1 items to the recycle bin?").arg(deleteConfirmDialog.pendingPaths.length)
            color: "#dddddd"
            font.pixelSize: 12
        }
        onAccepted: {
            FileOps.delete_to_trash(JSON.stringify(pendingPaths))
            refreshAfterDropTimer.restart()
        }
    }

    // ── Drag/drop choice dialog ───────────────────────────────────────
    // Shown after every internal drag-and-drop inside UFB. The user
    // explicitly picks Copy or Move — we don't infer from same-volume
    // detection (which had risky multi-URL truncation interactions)
    // or modifier keys (which Qt6 QML's DragEvent doesn't expose).
    // Persistent drag source shared by all three view modes. Lives at
    // panel scope (never recycled) so an async model rebuild can't
    // destroy the item that owns an in-flight Drag — the root cause of
    // the drag-out qFatal. Invisible but rendered (opacity 0, not
    // visible:false) so Qt's automatic drag machinery still engages.
    Item {
        id: dragProxy
        width: 1
        height: 1
        opacity: 0
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        // Drag.mimeData is assigned imperatively in _startSystemDrag so
        // each drag carries the full current selection (see the note
        // there). A declarative object-literal binding here was unreliable
        // for repeated multi-select drags.
        // Drag pixmap — a snapshot of the pressed delegate, set by
        // _startSystemDrag before the drag activates.
        Drag.imageSource: ""
        Drag.hotSpot.x: 8
        Drag.hotSpot.y: 8
    }

    // Activation fallback for _startSystemDrag: grabToImage is async and
    // its callback can be slow or never arrive under software rendering
    // (RDP / GPU-less VM). If the grab hasn't activated the drag by the
    // time this fires, start it without a preview so the gesture is
    // never lost — synchronous activation worked before the preview was
    // reintroduced, this preserves that floor.
    Timer {
        id: _dragActivateFallback
        interval: 80
        repeat: false
        onTriggered: {
            if (!dragProxy.Drag.active) {
                dragProxy.Drag.imageSource = ""
                dragProxy.Drag.active = true
            }
        }
    }

    property var _pendingDropPaths: []
    property string _pendingDropTarget: ""
    Dialog {
        id: dragChoiceDialog
        title: qsTr("Move or Copy?")
        modal: true
        anchors.centerIn: parent
        // Custom footer — we want three explicit choices instead of
        // the Ok/Cancel pair. Cancel quietly drops the operation.
        footer: DialogButtonBox {
            Button {
                text: qsTr("Cancel")
                onClicked: dragChoiceDialog.reject()
            }
            Button {
                text: qsTr("Copy")
                onClicked: {
                    FileOps.copy_files(
                        JSON.stringify(root._pendingDropPaths),
                        root._pendingDropTarget)
                    refreshAfterDropTimer.restart()
                    dragChoiceDialog.accept()
                }
            }
            Button {
                text: qsTr("Move")
                onClicked: {
                    FileOps.move_files(
                        JSON.stringify(root._pendingDropPaths),
                        root._pendingDropTarget)
                    refreshAfterDropTimer.restart()
                    dragChoiceDialog.accept()
                }
            }
        }
        ColumnLayout {
            spacing: 8
            anchors.left: parent.left
            anchors.right: parent.right
            Label {
                text: root._pendingDropPaths.length === 1
                    ? qsTr("Drag 1 item into:")
                    : qsTr("Drag %1 items into:").arg(root._pendingDropPaths.length)
                color: "#dddddd"
                font.pixelSize: 12
            }
            Label {
                text: root._pendingDropTarget
                color: "#bbbbbb"
                font.pixelSize: 11
                font.family: "Consolas, monospace"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
            }
        }
    }

    // ── Shared entries model ──────────────────────────────────────────
    // Hoisted so list view AND grid view share the same backing data.
    ListModel { id: entriesModel }

    // Hidden file/folder names — never shown in any view, regardless
    // of search filter. Keeps job templates from showing their .gitkeep
    // placeholders, and tidies up macOS / Windows shell metadata.
    readonly property var _hiddenNames: ({
        ".gitkeep": true,
        ".ds_store": true,
        "thumbs.db": true,
        "desktop.ini": true
    })

    function refreshEntries() {
        // Never rebuild the model mid-drag: clearing entriesModel
        // recycles every delegate, and if our own drag is in flight the
        // recycle can race the drag teardown. Re-run once the drag ends.
        if (root._systemDragActive) {
            root._refreshDeferred = true
            return
        }
        // Deep-search mode: rows come from the recursive Search backend
        // (whole-tree results), not the current directory listing.
        if (root._searchActive) {
            root._populateSearchResults()
            return
        }
        // In-place reload (same dir, contents changed by a file op)
        // preserves selection / cursor / scroll, keyed by path so it
        // survives row insertions/removals. A genuine navigation resets
        // everything (the new directory is a fresh context).
        var samePath = (root.dir.current_path === root._lastListedPath)
        var snap = null
        var savedTreeSel = []
        var savedTreeCur = ""
        if (samePath) {
            var preView = (root.viewMode === "grid") ? entryGrid : entryList
            var curPath = (root.currentIndex >= 0
                           && root.currentIndex < entriesModel.count)
                ? entriesModel.get(root.currentIndex).path : ""
            snap = ListingState.snapshot(root.selectedPaths(), curPath,
                                         preView ? preView.contentY : 0)
            savedTreeSel = root.treeSelectedPaths.slice()
            savedTreeCur = root.treeCurrentPath
        }
        entriesModel.clear()
        root.clearSelection()
        root.currentIndex = -1
        root._lastListedPath = root.dir.current_path
        // clearSelection() also wipes tree selection; on an in-place
        // reload restore it (path-based, so it survives the rebuild).
        if (samePath) {
            root.treeSelectedPaths = savedTreeSel
            root.treeCurrentPath = savedTreeCur
        }
        if (!root.dir.entries_json) return
        try {
            var arr = JSON.parse(root.dir.entries_json)
            var rows = []
            var needle = root._filterText.toLowerCase()
            for (var i = 0; i < arr.length; ++i) {
                var e = arr[i]
                var name = e.name || ""
                if (root._hiddenNames[name.toLowerCase()] === true) continue
                if (needle.length > 0 && name.toLowerCase().indexOf(needle) < 0)
                    continue
                rows.push({
                    name: name,
                    path: e.path || "",
                    isDir: !!e.isDir,
                    size: typeof e.size === "number" ? e.size : 0,
                    modified: typeof e.modified === "number" ? e.modified : 0,
                    extension: e.extension || "",
                    subtitle: ""   // parent path, populated in search mode
                })
            }
            if (root._sortKey.length > 0 && root._sortDir !== 0) {
                rows.sort(function(a, b) {
                    // Folders-first runs first; only the in-group
                    // comparison flips with direction so dirs stay
                    // above files in both asc and desc.
                    if (a.isDir !== b.isDir) return a.isDir ? -1 : 1
                    return root._sortDir * root._compareEntries(a, b)
                })
            }
            for (var j = 0; j < rows.length; ++j) {
                entriesModel.append(rows[j])
            }
            if (snap) {
                // In-place reload: restore selection / cursor / scroll
                // by path against the rebuilt model.
                var rstate = ListingState.resolve(entriesModel, snap)
                if (rstate.indexes.length > 0) root._setSelection(rstate.indexes)
                root.currentIndex = rstate.currentIndex
                root.anchorIndex = rstate.currentIndex
                var postView = (root.viewMode === "grid") ? entryGrid : entryList
                if (postView) {
                    var maxY = Math.max(0, postView.contentHeight - postView.height)
                    postView.contentY = Math.max(0, Math.min(snap.contentY, maxY))
                }
            } else if (entriesModel.count > 0) {
                root.currentIndex = 0
            }
            // Apply any deep-link "select this file after navigate"
            // hint. Match by either the full path (preferred) or the
            // bare filename (fallback for cross-OS / mapped paths).
            if (root.selectAfterLoadPath.length > 0
                && entriesModel.count > 0) {
                var target = root.selectAfterLoadPath
                var sep = target.indexOf("/") >= 0 ? "/" : "\\"
                var bareName = target.substring(target.lastIndexOf(sep) + 1)
                var hitIdx = -1
                for (var k = 0; k < entriesModel.count; ++k) {
                    var row = entriesModel.get(k)
                    if (row.path === target || row.name === bareName) {
                        hitIdx = k
                        break
                    }
                }
                if (hitIdx >= 0) root.selectOnly(hitIdx)
                root.selectAfterLoadPath = ""
            }
        } catch (e) {
            console.warn("FileBrowser: failed to parse entries_json:", e)
        }
    }

    // Populate the model from recursive Search results (folders + files
    // from anywhere under the root). Each row carries its parent path as
    // `subtitle` so identically-named hits in different folders read
    // unambiguously. No in-place snapshot restore — search results are a
    // fresh set on every query/index update.
    function _populateSearchResults() {
        entriesModel.clear()
        root.clearSelection()
        root.currentIndex = -1
        var json = deepSearch.results_json
        if (!json) return
        try {
            var arr = JSON.parse(json)
            var rows = []
            for (var i = 0; i < arr.length; ++i) {
                var e = arr[i]
                var name = e.name || ""
                if (root._hiddenNames[name.toLowerCase()] === true) continue
                var p = e.path || ""
                var sep = p.indexOf("/") >= 0 ? "/" : "\\"
                var cut = p.lastIndexOf(sep)
                rows.push({
                    name: name,
                    path: p,
                    isDir: !!e.isDir,
                    size: typeof e.size === "number" ? e.size : 0,
                    modified: typeof e.modified === "number" ? e.modified : 0,
                    extension: e.extension || "",
                    subtitle: cut > 0 ? p.substring(0, cut) : ""
                })
            }
            // Folders first, then case-insensitive by name.
            rows.sort(function(a, b) {
                if (a.isDir !== b.isDir) return a.isDir ? -1 : 1
                return (a.name || "").toLowerCase()
                    .localeCompare((b.name || "").toLowerCase())
            })
            for (var j = 0; j < rows.length; ++j) entriesModel.append(rows[j])
            if (entriesModel.count > 0) root.currentIndex = 0
        } catch (err) {
            console.warn("FileBrowser: failed to parse search results:", err)
        }
    }

    Connections {
        target: root.dir
        function onEntries_jsonChanged() { root.refreshEntries() }
        // Tree mode mirrors current_path from the underlying Directory.
        function onCurrent_pathChanged() {
            if (root.viewMode === "tree" && root.dir.current_path.length > 0) {
                tree.set_root(root.dir.current_path)
            }
        }
    }

    // Authoritative post-op refresh: FileOps broadcasts the directories a
    // completed operation changed. Every pane listens; the one(s) showing
    // an affected directory reload in place (state-preserving). This is
    // what makes cross-pane moves and sidebar-bookmark drops refresh the
    // right pane instantly, without the timer guesswork.
    Connections {
        target: FileOps
        function onDirs_changed(dirsJson) {
            if (!root.dir || root.dir.current_path.length === 0) return
            var dirs
            try { dirs = JSON.parse(dirsJson) } catch (e) { return }
            if (!dirs || dirs.length === 0) return
            var cur = root.dir.current_path
            // List/grid: refresh when this exact directory changed.
            if (ListingState.dirInSet(cur, dirs)) {
                root.dir.refresh()
            }
            // Tree: a change in ANY expanded subfolder under the root can
            // affect a visible node, so refresh when the change is at or
            // beneath the tree root. tree.refresh() re-expands open paths.
            if (root.viewMode === "tree" && ListingState.anyUnder(dirs, cur)) {
                tree.refresh()
            }
        }
    }
    Component.onCompleted: {
        // The pane's seed path may already be set (DualBrowserView /
        // FolderTabView assign before we complete) — apply its
        // remembered folder prefs before the change-tracking handlers
        // arm, so the restore doesn't ricochet into a save. Cold
        // defaults come from the property initializers; the legacy
        // settings.json browser_panel globals are gone.
        _applyFolderPrefs()

        refreshEntries()
        _refreshProjectFolderLookup()
        _refreshSubscriptionLookup()

        Qt.callLater(function() { root._panelStateRestored = true })
    }

    /// Gates the panel-state save handlers below so the restore in
    /// Component.onCompleted doesn't kick a no-op write loop.
    property bool _panelStateRestored: false

    /// True while _applyFolderPrefs is writing remembered values into
    /// the pane — those property changes are restores, not user edits,
    /// and must not re-save (harmless but noisy) or re-seed globals.
    property bool _applyingFolderPrefs: false

    /// The folder the debounced save writes prefs against. Tracked
    /// separately from dir.current_path so a save pending at
    /// navigation time flushes against the folder the user was IN
    /// when they made the change, not the destination.
    property string _prefsPath: ""

    /// Apply the destination folder's remembered view prefs
    /// (viewMode / gridSize / sortKey+Dir / column widths). Missing
    /// prefs inherit the pane's current state — Finder semantics,
    /// never a reset.
    function _applyFolderPrefs() {
        var p = root.dir ? root.dir.current_path : ""
        _prefsPath = p || ""
        if (!p || p.length === 0) return
        var prefs = ({})
        try { prefs = JSON.parse(Settings.folder_view_prefs(p)) } catch (e) { return }
        _applyingFolderPrefs = true
        if (prefs.viewMode === "list" || prefs.viewMode === "grid"
            || prefs.viewMode === "tree") {
            root.viewMode = prefs.viewMode
        }
        if (prefs.gridSize > 0) root.gridSize = prefs.gridSize
        if (prefs.sortKey !== undefined) {
            root._sortKey = prefs.sortKey
            root._sortDir = prefs.sortDir || 0
        }
        if (prefs.nameColWidth > 0) root._nameColWidth = prefs.nameColWidth
        if (prefs.sizeColWidth > 0) root._sizeColWidth = prefs.sizeColWidth
        _applyingFolderPrefs = false
    }

    /// Flush a pending debounced save immediately (used when leaving
    /// a folder so its prefs don't land on the destination).
    function _flushFolderPrefs() {
        if (!_browserStateSaveTimer.running) return
        _browserStateSaveTimer.stop()
        _saveBrowserState()
    }

    function _saveBrowserState() {
        // Per-folder prefs — written only after the user actually
        // changed something while in the folder (the handlers below
        // don't fire for inherited state). MERGE into the existing
        // row: job roots share their entry with JobView's
        // activeSubtab, and ItemListPanel adds its own fields for
        // subtab folders — a blind overwrite would drop theirs.
        if (_prefsPath.length === 0) return
        var prefs = ({})
        try { prefs = JSON.parse(Settings.folder_view_prefs(_prefsPath)) } catch (e) { prefs = ({}) }
        prefs.viewMode = root.viewMode
        prefs.gridSize = Math.round(root.gridSize)
        prefs.sortKey = root._sortKey
        prefs.sortDir = root._sortDir
        prefs.nameColWidth = Math.round(root._nameColWidth)
        prefs.sizeColWidth = Math.round(root._sizeColWidth)
        Settings.set_folder_view_prefs(_prefsPath, JSON.stringify(prefs))
    }

    Timer {
        // Column drags fire many property writes; debounce so we
        // only save after the user has settled on a width. View
        // mode + grid size also flow through here for symmetry,
        // even though those fire less frequently.
        id: _browserStateSaveTimer
        interval: 500
        repeat: false
        onTriggered: root._saveBrowserState()
    }

    onGridSizeChanged: if (_panelStateRestored && !_applyingFolderPrefs) _browserStateSaveTimer.restart()
    on_NameColWidthChanged: if (_panelStateRestored && !_applyingFolderPrefs) _browserStateSaveTimer.restart()
    on_SizeColWidthChanged: if (_panelStateRestored && !_applyingFolderPrefs) _browserStateSaveTimer.restart()
    on_SortKeyChanged: if (_panelStateRestored && !_applyingFolderPrefs) _browserStateSaveTimer.restart()
    on_SortDirChanged: if (_panelStateRestored && !_applyingFolderPrefs) _browserStateSaveTimer.restart()

    // ── Tree-mode backing ─────────────────────────────────────────────
    // Per-pane DirectoryTree QObject. Cheap when not in tree mode (no
    // listing happens until set_root is called).
    DirectoryTree {
        id: tree
    }
    // Path of the currently-highlighted tree row (also serves as the
    // anchor for Shift+Click range selection). Tree mode uses these
    // instead of selectedIndexes so list/grid and tree don't fight
    // over the same slot when the user switches view modes.
    property string treeCurrentPath: ""
    // Multi-selection set for tree mode. Plain click resets to
    // [path]; Ctrl+Click toggles; Shift+Click extends from the anchor
    // (treeCurrentPath) to the clicked path along the visible tree.
    property var treeSelectedPaths: []

    function _isTreeSelected(path) {
        return root.treeSelectedPaths.indexOf(path) >= 0
    }
    function _setTreeSelection(paths) {
        // Always assign a new array so QML binding sees the change.
        root.treeSelectedPaths = paths.slice()
    }
    function _treeIndexOfPath(path) {
        for (var i = 0; i < treeModel.count; ++i) {
            if (treeModel.get(i).path === path) return i
        }
        return -1
    }
    function _treeSelectRange(fromPath, toPath) {
        var i0 = _treeIndexOfPath(fromPath)
        var i1 = _treeIndexOfPath(toPath)
        if (i0 < 0 || i1 < 0) {
            _setTreeSelection([toPath])
            return
        }
        if (i0 > i1) { var t = i0; i0 = i1; i1 = t }
        var paths = []
        for (var i = i0; i <= i1; ++i) paths.push(treeModel.get(i).path)
        _setTreeSelection(paths)
    }
    // Mirror tree.flat_json into a ListModel that the tree ListView can
    // consume directly (delegates can't bind into JSON.parse'd arrays
    // with full Qt model semantics).
    ListModel { id: treeModel }
    function refreshTree() {
        // Expansion is preserved server-side (DirectoryTree.refresh re-
        // expands open paths) and tree selection is path-based, so the
        // only view state a rebuild would drop is the scroll offset.
        // Preserve it on an in-place rebuild (same root); reset to the
        // top when the root changed (navigation / view switch).
        var sameRoot = (root.dir.current_path === root._lastTreeRoot)
        var savedY = (sameRoot && entryTree) ? entryTree.contentY : 0
        root._lastTreeRoot = root.dir.current_path
        treeModel.clear()
        if (!tree.flat_json) return
        try {
            var arr = JSON.parse(tree.flat_json)
            for (var i = 0; i < arr.length; ++i) {
                var n = arr[i]
                var name = n.name || ""
                if (root._hiddenNames[name.toLowerCase()] === true) continue
                treeModel.append({
                    path: n.path || "",
                    name: name,
                    isDir: !!n.isDir,
                    depth: typeof n.depth === "number" ? n.depth : 0,
                    expanded: !!n.expanded,
                    hasChildren: !!n.hasChildren,
                    extension: n.extension || ""
                })
            }
            if (entryTree) {
                var maxY = Math.max(0, entryTree.contentHeight - entryTree.height)
                entryTree.contentY = Math.max(0, Math.min(savedY, maxY))
            }
        } catch (e) {
            console.warn("FileBrowser: failed to parse tree.flat_json:", e)
        }
    }
    Connections {
        target: tree
        function onFlat_jsonChanged() { root.refreshTree() }
    }
    // When the user first switches to tree mode, seed the tree from
    // the current Directory path. set_root is a no-op for empty paths.
    // Also kicks the panel-state save timer so the new view mode
    // becomes the shared default for newly-spawned panels.
    onViewModeChanged: {
        if (viewMode === "tree" && root.dir.current_path.length > 0) {
            tree.set_root(root.dir.current_path)
        }
        if (_panelStateRestored && !_applyingFolderPrefs) _browserStateSaveTimer.restart()
    }

    color: Theme.colors.surface
    // No `border` declared on the panel itself — using Rectangle.border
    // here insets the painted region by `border.width` pixels, which
    // (a) costs the inner ColumnLayout 1-2px on every side and (b)
    // gets visually clipped at SplitView seams when the splitter is
    // 0px wide. The focus + drop-target indicators live in the
    // `focusOverlay` child below: an absolute-positioned overlay at
    // z:9999 that paints ON TOP of the children without consuming
    // any layout space.

    // ── Re-open strip (only when collapsed) ──────────────────────────
    // When the pane is collapsed the rest of the chrome is hidden,
    // so the expand button needs a permanent home. The strip lives on
    // the INNER edge of the pane (right side of the left pane, left
    // side of the right pane) so both toggles cluster around the
    // splitter divider - same location they sit in the footer row
    // when expanded. Bottom-anchored so the toggle stays vertically
    // aligned with the footer row instead of jumping to the top.
    readonly property int _stripWidth: Theme.dim.toolStripHeight
    Rectangle {
        id: reopenStrip
        visible: root.collapsed && root.allowCollapse
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: root.paneSide === "right" ? parent.left : undefined
        anchors.right: root.paneSide === "left" ? parent.right : undefined
        width: root._stripWidth
        color: Theme.colors.toolbar
        FlatButton {
            anchors.bottom: parent.bottom
            // Bottom-flush so the collapsed re-open toggle sits at the
            // same Y as the expanded footer toggle (which also sits
            // flush at the panel bottom — no margin on the parent).
            anchors.horizontalCenter: parent.horizontalCenter
            width: root._stripWidth - 4
            height: root._stripWidth
            // Arrow points in the direction the pane will grow.
            iconName: root.paneSide === "left"
                ? "caret-double-right"
                : "caret-double-left"
            // Accent-tint the glyph so the collapsed state reads as a
            // call-to-action against the toolbar-grey strip.
            iconColor: Theme.colors.accent
            tooltip: qsTr("Expand pane")
            onClicked: root.toggleCollapsed()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        // Flush stack — children butt against the pane edges and each
        // other. Internal padding lives inside each child's own layout
        // (e.g. the path bar's RowLayout has its own leftMargin), so
        // text never crashes into a wall.
        spacing: 0

        // ── Path bar ──────────────────────────────────────────────────
        // Breadcrumb by default; click the trailing empty space (or
        // press Ctrl+L) to flip into the editable TextField for
        // pasting/typing a path. Esc or focus-loss flips back. One
        // row of chrome, two display modes.
        //
        // Subtle `bg` tint — slightly darker than the `surface`-coloured
        // nav row + list area below. The tonal cut, not a divider line,
        // is what signals "this is the address area." More honest to
        // the flat goal than a 1px stroke.
        Rectangle {
            id: pathBar
            visible: root.showPathField && !root.collapsed
            Layout.fillWidth: true
            implicitHeight: pathField.implicitHeight
            color: Theme.colors.bg

            // Breadcrumb mode.
            RowLayout {
                anchors.fill: parent
                spacing: 0
                visible: !root._pathEditing

                Repeater {
                    id: crumbsRepeater
                    model: root._pathSegments
                    delegate: RowLayout {
                        required property var modelData
                        required property int index
                        spacing: 0
                        readonly property bool isLast:
                            index === root._pathSegments.length - 1
                        FlatButton {
                            text: parent.modelData.name
                            tooltip: parent.modelData.path
                            padding: 6
                            // No `checked` on the last crumb - that
                            // used to draw an accent border (FlatButton
                            // line 74), which read as a box around the
                            // tail of the trail. The last crumb is
                            // already the current folder; no further
                            // marker needed.
                            onClicked: {
                                root.activated()
                                root.dir.navigate_to(parent.modelData.path)
                            }
                        }
                        Label {
                            visible: !parent.isLast
                            text: "›"
                            color: Theme.colors.textMuted
                            font.pixelSize: Theme.font.sizeBody
                            verticalAlignment: Text.AlignVCenter
                            Layout.preferredWidth: 12
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
                // Trailing fillWidth spacer doubles as the click-to-edit
                // affordance. I-beam cursor signals the implicit
                // "click here to type a path" gesture.
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.IBeamCursor
                        onClicked: root._beginPathEdit()
                    }
                }
            }

            // Failsafe: Ctrl+left-click ANYWHERE on the path bar enters
            // edit mode — covers the case where the breadcrumb fills the
            // whole row and there's no trailing empty space to click.
            // Same gesture on both platforms. Non-Ctrl clicks set
            // accepted=false in onPressed so they fall through to the
            // crumb buttons / trailing affordance beneath (both Mouse
            // area–based, so propagation works).
            MouseArea {
                anchors.fill: parent
                enabled: !root._pathEditing
                acceptedButtons: Qt.LeftButton
                onPressed: (mouse) => {
                    mouse.accepted = (mouse.modifiers & Qt.ControlModifier) !== 0
                }
                onClicked: (mouse) => {
                    if ((mouse.modifiers & Qt.ControlModifier) !== 0)
                        root._beginPathEdit()
                }
            }

            // Edit mode.
            TextField {
                id: pathField
                anchors.fill: parent
                visible: root._pathEditing
                text: root.dir.current_path
                font.family: "Consolas"
                font.pixelSize: 12
                selectByMouse: true
                onAccepted: {
                    root.activated()
                    root.dir.navigate_to(text)
                    root._pathEditing = false
                }
                Keys.onEscapePressed: {
                    text = root.dir.current_path
                    root._pathEditing = false
                }
                onActiveFocusChanged: {
                    // Blur exits edit mode without committing - Esc
                    // already committed the cancellation; here we
                    // just make sure clicking away doesn't strand
                    // the user in edit mode.
                    if (!activeFocus) root._pathEditing = false
                }
            }
        }

        // ── Navigation bar ────────────────────────────────────────────
        // Topaz-style flat strip: full-row-height squared FlatButtons,
        // segmented control for view mode (accent fill on selection),
        // theme-coloured slider. Spacing 0 so adjacent buttons sit
        // flush like the reference.
        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            spacing: 0

            FlatButton {
                iconName: "arrow-left"
                tooltip: qsTr("Back")
                enabled_: root._navIndex > 0
                Layout.preferredHeight: Theme.dim.toolStripHeight
                onClicked: { root.activated(); root.navBack() }
            }
            FlatButton {
                iconName: "arrow-right"
                tooltip: qsTr("Forward")
                enabled_: root._navIndex >= 0
                    && root._navIndex < root._navHistory.length - 1
                Layout.preferredHeight: Theme.dim.toolStripHeight
                onClicked: { root.activated(); root.navForward() }
            }
            FlatButton {
                iconName: "arrow-up"
                tooltip: qsTr("Parent directory")
                Layout.preferredHeight: Theme.dim.toolStripHeight
                onClicked: { root.activated(); root.dir.navigate_up() }
            }
            FlatButton {
                iconName: "arrow-clockwise"
                tooltip: qsTr("Refresh")
                Layout.preferredHeight: Theme.dim.toolStripHeight
                onClicked: { root.activated(); refreshAction.trigger() }
            }

            // "+" — opens the background context menu (new folder, paste,
            // etc., all targeting this view's root). Gives access to those
            // actions when the listing is full and there's no empty space
            // to right-click. Sits immediately left of the search field.
            FlatButton {
                iconName: "plus"
                tooltip: qsTr("New… (actions for this folder)")
                Layout.preferredHeight: Theme.dim.toolStripHeight
                onClicked: { root.activated(); bgMenu.popup() }
            }

            // Search field - case-insensitive substring filter on
            // entry names. Sits between nav and view-mode so it's
            // always visible. fillWidth so it absorbs free space.
            // No resting border — relies on the surface fill (lighter
            // than the panel's bg) to read as an input area. Border
            // appears only on focus, the standard "input lights up"
            // affordance.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.dim.toolStripHeight
                Layout.alignment: Qt.AlignVCenter
                color: Theme.colors.surface
                border.color: searchField.activeFocus
                    ? Theme.colors.accent
                    : "transparent"
                border.width: searchField.activeFocus ? Theme.dim.border : 0

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 4
                    spacing: 6

                    Icon {
                        name: "magnifying-glass"
                        size: Theme.icon.sizeToolbar
                        color: searchField.activeFocus
                            ? Theme.colors.accent
                            : Theme.colors.textMuted
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    TextField {
                        id: searchField
                        width: parent.width - 60
                        anchors.verticalCenter: parent.verticalCenter
                        placeholderText: qsTr("Search…")
                        text: root._filterText
                        onTextChanged: root._filterText = text
                        background: Item {}   // transparent - parent draws it
                        color: Theme.colors.text
                        placeholderTextColor: Theme.colors.textSubtle
                        font.pixelSize: Theme.font.sizeBody
                        selectByMouse: true
                        Keys.onEscapePressed: { text = ""; root.forceActiveFocus() }
                    }
                    // Inset by 2px on each side so the button doesn't
                    // optically poke through the search field border.
                    FlatButton {
                        visible: root._filterText.length > 0
                        iconName: "x"
                        iconSize: Theme.icon.sizeSmall
                        tooltip: qsTr("Clear search")
                        width: 22
                        height: parent.height - 4
                        padding: 0
                        anchors.verticalCenter: parent.verticalCenter
                        onClicked: root._filterText = ""
                    }
                }
            }

            // Indexed-search toggle: OS backend (off) ↔ Tantivy index (on).
            // Sits immediately right of the search field.
            FlatButton {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: Theme.dim.toolStripHeight
                iconName: "lightning"
                iconSize: Theme.icon.sizeToolbar
                checked: root._searchIndexed
                tooltip: root._searchIndexed
                    ? qsTr("Indexed search (Tantivy) — on. Click for OS search.")
                    : qsTr("OS search — on. Click for indexed (Tantivy) search.")
                onClicked: { root.activated(); root._toggleSearchMode() }
            }

            BusyIndicator {
                // Spins for directory loads and while a search index builds.
                visible: root.dir.loading || tree.loading
                         || deepSearch.indexing || deepSearch.searching
                running: visible
                Layout.preferredHeight: 22
                Layout.preferredWidth: 22
                Layout.alignment: Qt.AlignVCenter
            }

            // View-mode toggle - accent fill on selected segment.
            SegmentedControl {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: Theme.dim.toolStripHeight
                variant: "accent"
                value: root.viewMode
                onValueChanged: {
                    root.activated()
                    root.viewMode = value
                }
                options: [
                    { value: "list", icon: "list-bullets", tooltip: qsTr("List view") },
                    { value: "grid", icon: "squares-four", tooltip: qsTr("Grid view") },
                    { value: "tree", icon: "tree-structure", tooltip: qsTr("Tree view") }
                ]
            }

            // Slim grid-size slider - accent-coloured, grid mode only.
            Slider {
                id: gridSlider
                visible: root.viewMode === "grid"
                Layout.preferredWidth: 110
                Layout.preferredHeight: Theme.dim.toolStripHeight
                Layout.alignment: Qt.AlignVCenter
                from: 48; to: 256; stepSize: 8
                value: root.gridSize
                onMoved: root.gridSize = value
                background: Rectangle {
                    x: gridSlider.leftPadding
                    y: gridSlider.topPadding + gridSlider.availableHeight / 2 - height / 2
                    width: gridSlider.availableWidth
                    height: 2
                    color: Theme.colors.borderStrong
                    Rectangle {
                        width: gridSlider.visualPosition * parent.width
                        height: parent.height
                        color: Theme.colors.accent
                    }
                }
                handle: Rectangle {
                    x: gridSlider.leftPadding
                        + gridSlider.visualPosition * (gridSlider.availableWidth - width)
                    y: gridSlider.topPadding + gridSlider.availableHeight / 2 - height / 2
                    width: 10; height: 10
                    color: gridSlider.pressed
                        ? Theme.colors.accentHover
                        : Theme.colors.accent
                }
                ToolTip.text: qsTr("Grid size: %1 px").arg(root.gridSize)
                ToolTip.visible: hovered
                ToolTip.delay: 500
            }

            // Sort dropdown - visible in grid + tree (list mode uses
            // clickable headers). Mirrors the same _sortKey/_sortDir
            // state so toggling either path carries over.
            FlatButton {
                id: sortButton
                visible: root.viewMode !== "list"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: Theme.dim.toolStripHeight
                iconName: root._sortDir === 1
                    ? "sort-ascending"
                    : (root._sortDir === -1 ? "sort-descending" : "funnel")
                text: {
                    switch (root._sortKey) {
                    case "name":     return qsTr("Name")
                    case "size":     return qsTr("Size")
                    case "modified": return qsTr("Modified")
                    default:         return qsTr("Sort")
                    }
                }
                tooltip: qsTr("Sort entries (folders always first)")
                onClicked: sortMenu.popup(0, sortButton.height)
                Menu {
                    id: sortMenu
                    MenuItem {
                        text: qsTr("Name") + (root._sortKey === "name"
                            ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                        onTriggered: root._toggleSort("name")
                    }
                    MenuItem {
                        text: qsTr("Size") + (root._sortKey === "size"
                            ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                        onTriggered: root._toggleSort("size")
                    }
                    MenuItem {
                        text: qsTr("Modified") + (root._sortKey === "modified"
                            ? (root._sortDir === 1 ? "  ▲" : "  ▼") : "")
                        onTriggered: root._toggleSort("modified")
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("Clear sort")
                        enabled: root._sortKey.length > 0
                        onTriggered: {
                            root._sortKey = ""
                            root._sortDir = 0
                            root.refreshEntries()
                        }
                    }
                }
            }
        }

        // ── Error banner ──────────────────────────────────────────────
        Rectangle {
            visible: root.dir.error.length > 0 && !root.collapsed
            Layout.fillWidth: true
            Layout.preferredHeight: errLabel.implicitHeight + 12
            color: "#3a1e1e"
            border.color: "#c04040"
            radius: 3
            Label {
                id: errLabel
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 10
                text: root.dir.error
                color: "#f0d0d0"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }

        // ── List-mode header row (only visible in list mode) ──────────
        // Name + Size are fixed-width with right-edge drag handles.
        // Modified takes whatever's left so dragging the others never
        // pushes the last column off-screen. Each handle has a 2px
        // visible divider with a 10px hot zone so it's both
        // discoverable and easy to grab.
        // Filled tonal bar — same idiom as ItemListPanel's column
        // header. toolbarAlt vs surface contrast separates header
        // from list rows below.
        Rectangle {
            id: listHeader
            visible: root.viewMode === "list" && !root.collapsed
            Layout.fillWidth: true
            Layout.preferredHeight: 22
            color: Theme.colors.toolbarAlt

            Row {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10 + Theme.dim.scrollBarWidth
                spacing: 0

                Item {
                    width: root._nameColWidth
                    height: parent.height
                    Label {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        verticalAlignment: Text.AlignVCenter
                        text: "Name" + root._sortGlyph("name")
                        color: Theme.colors.textMuted
                        font.pixelSize: 11
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    // Click-to-sort hot zone — leaves the right 8px
                    // for the resize handle (which has its own MA
                    // with preventStealing).
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
                            _startWidth = root._nameColWidth
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed) return
                            _dragWidth = Math.max(80,
                                _startWidth + (mouse.x - _startMouseX))
                            root._dragGuideX =
                                parent.mapToItem(viewPanel, _dragWidth, 0).x
                        }
                        onReleased: {
                            if (_dragWidth >= 0)
                                root._nameColWidth = _dragWidth
                            _dragWidth = -1
                            root._dragGuideX = -1
                        }
                        onCanceled: {
                            _dragWidth = -1
                            root._dragGuideX = -1
                        }
                    }
                }

                // Synced column (header). Sits between Name and Size,
                // visible only inside a jobs folder. Fixed 26px wide
                // - no resize handle since it's icon-sized.
                Item {
                    visible: root._currentIsProjectFolder
                    width: visible ? 26 : 0
                    height: parent.height
                    Icon {
                        anchors.centerIn: parent
                        name: "check"
                        size: Theme.icon.sizeSmall
                        color: Theme.colors.textMuted
                    }
                }

                Item {
                    width: root._sizeColWidth
                    height: parent.height
                    Label {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        text: "Size" + root._sortGlyph("size")
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
                        onClicked: root._toggleSort("size")
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.topMargin: 4
                        anchors.bottomMargin: 4
                        width: 2
                        color: sizeHandleMa.containsMouse || sizeHandleMa.pressed
                            ? Theme.colors.accent : Theme.colors.borderStrong
                        z: 10
                    }
                    MouseArea {
                        id: sizeHandleMa
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
                            _startWidth = root._sizeColWidth
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed) return
                            _dragWidth = Math.max(40,
                                _startWidth + (mouse.x - _startMouseX))
                            root._dragGuideX =
                                parent.mapToItem(viewPanel, _dragWidth, 0).x
                        }
                        onReleased: {
                            if (_dragWidth >= 0)
                                root._sizeColWidth = _dragWidth
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
                    width: Math.max(40, listHeader.width - 20
                        - (root._currentIsProjectFolder ? 26 : 0)
                        - root._nameColWidth - root._sizeColWidth)
                    height: parent.height
                    Label {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        text: "Modified" + root._sortGlyph("modified")
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

        // ── View area (list / grid / tree) ────────────────────────────
        // No own border — the panel-root focus overlay (z:9999) paints
        // the dropActive halo on the entire pane, so a second border
        // here would just stack chrome.
        Rectangle {
            id: viewPanel
            visible: !root.collapsed
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.surface

            // Column-resize guide — tracks the proposed column edge
            // during a list-header width drag; the width commits once
            // on release (see _dragGuideX). Extends up over the
            // header bar (viewPanel doesn't clip), z above the views
            // and the panel DropArea.
            Rectangle {
                visible: root._dragGuideX >= 0
                x: root._dragGuideX
                y: -listHeader.height
                width: 2
                height: viewPanel.height + listHeader.height
                color: Theme.colors.accent
                z: 200
            }

            // Drop target — the whole view panel accepts text/uri-list.
            // A drop hovering a folder row lands inside that folder
            // (root._dropTargetPath, set as the drag moves); anywhere
            // else falls back to the panel's current_path / tree root.
            // The DropArea sits above the views (z:100) so a single
            // handler sees every enter/move/exit cleanly — per-row
            // DropAreas underneath would never receive the drag.
            DropArea {
                id: panelDrop
                anchors.fill: parent
                keys: ["text/uri-list"]
                z: 100  // above the views so it sees enter/exit cleanly
                onEntered: (drag) => {
                    root.activated()
                    root.dropActive = true
                    root._dropTargetPath = root._folderPathAt(drag.x, drag.y)
                }
                onPositionChanged: (drag) => {
                    root._dropTargetPath = root._folderPathAt(drag.x, drag.y)
                }
                onExited: {
                    root.dropActive = false
                    root._dropTargetPath = ""
                }
                onDropped: (drop) => {
                    var fallback = root.viewMode === "tree"
                        ? (tree.root_path.length > 0 ? tree.root_path : root.dir.current_path)
                        : root.dir.current_path
                    // Recompute the folder under the cursor from the drop
                    // point itself rather than trusting _dropTargetPath:
                    // onPositionChanged isn't guaranteed to fire during a
                    // platform's modal drag loop (Windows DoDragDrop), so
                    // the cached value can be stale. The coordinates on
                    // the drop event are always current.
                    var folder = root._folderPathAt(drop.x, drop.y)
                    var target = folder.length > 0 ? folder : fallback
                    root._handleDrop(drop, target)
                    root.dropActive = false
                    root._dropTargetPath = ""
                }
            }

            // ── List view ─────────────────────────────────────────
            ListView {
                id: entryList
                anchors.fill: parent
                visible: root.viewMode === "list"
                model: entriesModel
                clip: true
                focus: visible
                keyNavigationEnabled: false  // we drive Up/Down ourselves
                highlightMoveDuration: 0
                currentIndex: root.currentIndex
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: UfbScrollBar {}

                // Inside-the-Flickable TapHandler. The viewPanel-scope
                // handler doesn't see clicks on empty list area because
                // the ListView's internal Flickable grabs them for
                // potential flick-gesture detection. A handler that
                // lives inside the Flickable shares its grab and fires
                // for taps anywhere — including the empty area below
                // the last delegate.
                TapHandler {
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onTapped: (eventPoint, button) => {
                        root.activated()
                        if (button === Qt.RightButton) {
                            if (root._menuClickConsumed) {
                                root._menuClickConsumed = false
                            } else {
                                root._popupBgMenu()
                            }
                        } else {
                            // Left tap on empty area clears the selection.
                            // A tap on a row is owned by the row's MouseArea
                            // (which manages selection); this handler fires
                            // for those too, so gate on "no row was hit."
                            var p = eventPoint.position
                            if (entryList.indexAt(p.x + entryList.contentX,
                                                  p.y + entryList.contentY) < 0)
                                root.clearSelection()
                        }
                    }
                }

                delegate: Rectangle {
                    id: rowItem
                    width: entryList.width
                    // Taller in search mode to fit the parent-path subtitle
                    // under the name (recursive hits span many folders).
                    height: root._searchActive ? 34 : 22
                    property bool selected: root._isSelected(index)
                    property bool isCurrent: root.currentIndex === index
                    // A folder under a hovering drag wins over selection
                    // so the user can see where the drop will land.
                    property bool dropTarget: model.isDir
                        && root._dropTargetPath === model.path
                    color: dropTarget
                        ? Theme.colors.accentSelected
                        : selected
                        ? (root.active ? Theme.colors.accentSelected : Theme.colors.accentMuted)
                        : rowMouseArea.containsMouse
                        ? "#2a2a2a"
                        : (index % 2 === 1 ? Theme.colors.surfaceAlt : "transparent")

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10 + Theme.dim.scrollBarWidth
                        spacing: 0

                        Row {
                            width: root._nameColWidth
                            spacing: 6
                            anchors.verticalCenter: parent.verticalCenter

                            FileTypeIcon {
                                id: rowIcon
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16
                                height: 16
                                isDir: model.isDir
                                extension: model.extension
                                sourceSizePx: 16
                                // Tints the Phosphor fallback glyph only
                                // (the OS icon itself is full-color and
                                // never tinted on selection).
                                fallbackColor: rowItem.selected
                                    ? "#ffffff" : Theme.colors.text
                            }

                            Column {
                                width: parent.width - rowIcon.width - parent.spacing
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 0
                                Label {
                                    width: parent.width
                                    text: model.name
                                    color: rowItem.selected
                                        ? "#ffffff"
                                        : Theme.colors.text
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                                // Parent path — only in search mode, where
                                // results come from many folders. Middle-elide
                                // keeps the leaf folder visible.
                                Label {
                                    visible: model.subtitle && model.subtitle.length > 0
                                    width: parent.width
                                    text: model.subtitle
                                    color: rowItem.selected ? "#cfd9e6" : Theme.colors.textMuted
                                    font.pixelSize: 10
                                    elide: Text.ElideMiddle
                                }
                            }
                        }

                        // Synced cell - check icon when this row is a
                        // subdir whose path matches a subscribed job.
                        // Visible only inside a jobs folder, fixed
                        // width to match the header cell above.
                        Item {
                            visible: root._currentIsProjectFolder
                            width: visible ? 26 : 0
                            height: parent.height
                            Icon {
                                anchors.centerIn: parent
                                visible: model.isDir
                                    && root._isPathSubscribed(model.path)
                                name: "check"
                                size: Theme.icon.sizeSmall
                                color: rowItem.selected
                                    ? Theme.colors.textBright
                                    : Theme.colors.accent
                            }
                        }

                        Label {
                            width: root._sizeColWidth
                            text: model.isDir ? "—" : root.formatSize(model.size)
                            color: rowItem.selected ? "#cfd9e6" : "#888"
                            font.pixelSize: 11
                            font.family: "Consolas"
                            horizontalAlignment: Text.AlignRight
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Label {
                            width: Math.max(40, parent.width
                                - (root._currentIsProjectFolder ? 26 : 0)
                                - root._nameColWidth - root._sizeColWidth)
                            text: root.formatDate(model.modified)
                            color: rowItem.selected ? "#cfd9e6" : "#888"
                            font.pixelSize: 11
                            font.family: "Consolas"
                            horizontalAlignment: Text.AlignRight
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    MouseArea {
                        id: rowMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        // Once pressed, this MouseArea owns the gesture
                        // — without this the parent ListView/GridView
                        // (a Flickable) hijacks vertical drags as scroll
                        // and our drag-out never sees positionChanged.
                        preventStealing: true
                        property point _pressPoint
                        property bool _dragStarted: false
                        onPressed: (mouse) => {
                            root.activated()
                            entryList.forceActiveFocus()
                            _pressPoint = Qt.point(mouse.x, mouse.y)
                            _dragStarted = false
                            if (mouse.button === Qt.RightButton) {
                                root._popupRowMenu(index, model)
                                return
                            }
                            if (mouse.modifiers & Qt.ShiftModifier) {
                                root.selectRange(root.anchorIndex, index)
                            } else if (mouse.modifiers & Qt.ControlModifier) {
                                root.toggleSelection(index)
                            } else if (!root._isSelected(index)) {
                                // Don't clobber an existing multi-select if the
                                // user pressed on one of the selected rows.
                                root.selectOnly(index)
                            }
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed || _dragStarted) return
                            var dx = mouse.x - _pressPoint.x
                            var dy = mouse.y - _pressPoint.y
                            if (dx*dx + dy*dy > 36) {  // 6px threshold
                                _dragStarted = true
                                root._startSystemDrag(rowItem, root._dragPathsForRow(model.path))
                            }
                        }
                        onDoubleClicked: root.activateRow(index)
                    }
                }

                // Background empty-area click clears selection
                // (left); right-click pops the panel bg menu.
                MouseArea {
                    anchors.fill: parent
                    z: -1
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: (mouse) => {
                        root.activated()
                        entryList.forceActiveFocus()
                        if (mouse.button === Qt.RightButton) {
                            root._popupBgMenu()
                            return
                        }
                        root.clearSelection()
                        mouse.accepted = false
                    }
                }

                Keys.onPressed: (event) => root._handleKey(event)
            }

            // ── Grid view ─────────────────────────────────────────
            GridView {
                id: entryGrid
                anchors.fill: parent
                visible: root.viewMode === "grid"
                model: entriesModel
                clip: true
                focus: visible
                keyNavigationEnabled: false
                cellWidth: root.gridSize + 16
                cellHeight: root.gridSize + 36
                currentIndex: root.currentIndex
                boundsBehavior: Flickable.StopAtBounds
                // Reserve gutter so cells don't sit under the scrollbar thumb.
                rightMargin: Theme.dim.scrollBarWidth
                ScrollBar.vertical: UfbScrollBar {}

                TapHandler {
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onTapped: (eventPoint, button) => {
                        root.activated()
                        if (button === Qt.RightButton) {
                            if (root._menuClickConsumed) {
                                root._menuClickConsumed = false
                            } else {
                                root._popupBgMenu()
                            }
                        } else {
                            // Left tap on empty area clears the selection
                            // (cell taps are owned by the cell's MouseArea).
                            var p = eventPoint.position
                            if (entryGrid.indexAt(p.x + entryGrid.contentX,
                                                  p.y + entryGrid.contentY) < 0)
                                root.clearSelection()
                        }
                    }
                }

                delegate: Item {
                    id: cell
                    width: entryGrid.cellWidth
                    height: entryGrid.cellHeight
                    property bool selected: root._isSelected(index)
                    property bool isCurrent: root.currentIndex === index
                    property bool dropTarget: model.isDir
                        && root._dropTargetPath === model.path

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 4
                        radius: 4
                        color: cell.dropTarget
                            ? Theme.colors.accentSelected
                            : cell.selected
                            ? (root.active ? Theme.colors.accentSelected : Theme.colors.accentMuted)
                            : (cellMouseArea.containsMouse ? "#262626" : "transparent")
                    }

                    Item {
                        id: thumbBox
                        width: root.gridSize
                        height: root.gridSize
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.topMargin: 6

                        // System icon - shown when there's no thumbnail.
                        // Hidden only once a real thumb has loaded, so
                        // users don't see icon edges peeking through the
                        // letterbox of an aspect-fit thumbnail.
                        //
                        // Gate on `status === Image.Ready`, NOT sourceSize
                        // or paintedWidth: the Image sets sourceSize
                        // explicitly for HiDPI crispness, which makes
                        // sourceSize/paintedWidth/implicitWidth all report
                        // the REQUEST size even when the provider returns a
                        // null QImage — so they can't tell "loaded" from
                        // "declined". The provider now reports a null decode
                        // as an error (status Error), so Ready means a real
                        // thumbnail and only then do we hide the icon. The
                        // Thumbnailer declines big PSD/PSB/TIFF/EXR/HDR
                        // (>64 MP / >256 MB, or psd_sdk can't open it):
                        // those resolve to Error, keeping the icon visible.
                        // While loading (status Loading) the icon also stays
                        // up as a placeholder, cross-fading out on Ready.
                        FileTypeIcon {
                            id: iconLayer
                            anchors.centerIn: parent
                            width: Math.min(parent.width, 96)
                            height: width
                            isDir: model.isDir
                            extension: model.extension
                            // Fixed 96px texture regardless of gridSize —
                            // avoids re-decoding the OS icon every frame
                            // while the user drags the grid-size slider.
                            sourceSizePx: 96
                            // Tints the Phosphor fallback glyph only.
                            fallbackColor: cell.selected
                                ? Theme.colors.textBright : Theme.colors.text
                            opacity: (thumbLayer.item
                                      && thumbLayer.item.status === Image.Ready)
                                     ? 0 : 1
                            Behavior on opacity {
                                NumberAnimation { duration: 150 }
                            }
                        }

                        // Thumbnail overlay - gated by Thumbnailer.
                        //
                        // The Loader pattern is structural: when
                        // `active` is false there is literally no
                        // Image element in the scene graph, so the
                        // iconLayer underneath cannot be visually
                        // covered. Compare with our earlier attempts
                        // (visible:false / source:""), where the
                        // Image element existed in the tree and
                        // delegate-recycle binding storms briefly
                        // painted placeholder texture over the icon.
                        //
                        // Step 2 of phase 14: Thumbnailer.supports()
                        // returns false unconditionally - no Image
                        // ever instantiates. Steps 3+ light up
                        // backends one at a time.
                        Loader {
                            id: thumbLayer
                            anchors.fill: parent
                            // Gate on viewMode === "grid": GridView still
                            // instantiates delegates for cells within its
                            // layout viewport even when `visible: false`,
                            // so without this gate we'd fire thumbnail
                            // decodes for every file in the directory the
                            // moment the user opened it — even in list /
                            // tree mode, where the thumbnail isn't drawn.
                            // For huge media (32K starmaps, etc.) the
                            // background decode load will OOM the process.
                            active: root.viewMode === "grid"
                                    && !model.isDir
                                    && Thumbnailer.mayHaveThumbnail(model.path || "")
                            sourceComponent: Image {
                                anchors.fill: parent
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                                source: "image://ufb-thumbs/" + model.path
                                // Request at device pixels so thumbnails are
                                // crisp on Retina/HiDPI: Qt does NOT scale an
                                // image-provider's requestedSize by DPR, so
                                // without this the provider hands back a
                                // logical-px image that the GPU upscales (blurry).
                                sourceSize.width: root.gridSize * Screen.devicePixelRatio
                                sourceSize.height: root.gridSize * Screen.devicePixelRatio
                            }
                        }

                        // Subscription badge - small accent-coloured
                        // check on the top-right corner of the
                        // thumbnail box. Only when we're inside a
                        // jobs folder AND this cell's path is a
                        // subscribed job subdir.
                        Rectangle {
                            visible: root._currentIsProjectFolder
                                && model.isDir
                                && root._isPathSubscribed(model.path)
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.topMargin: 2
                            anchors.rightMargin: 2
                            width: 18
                            height: 18
                            radius: 9
                            color: Theme.colors.accent
                            border.color: Theme.colors.surface
                            border.width: 2
                            Icon {
                                anchors.centerIn: parent
                                name: "check"
                                size: 11
                                color: Theme.colors.textBright
                            }
                        }
                    }

                    Label {
                        anchors.top: thumbBox.bottom
                        anchors.topMargin: 4
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        horizontalAlignment: Text.AlignHCenter
                        text: model.name
                        color: cell.selected
                            ? "#ffffff"
                            : Theme.colors.text
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                    }

                    MouseArea {
                        id: cellMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        // Once pressed, this MouseArea owns the gesture
                        // — without this the parent ListView/GridView
                        // (a Flickable) hijacks vertical drags as scroll
                        // and our drag-out never sees positionChanged.
                        preventStealing: true
                        property point _pressPoint
                        property bool _dragStarted: false
                        onPressed: (mouse) => {
                            root.activated()
                            entryGrid.forceActiveFocus()
                            _pressPoint = Qt.point(mouse.x, mouse.y)
                            _dragStarted = false
                            if (mouse.button === Qt.RightButton) {
                                root._popupRowMenu(index, model)
                                return
                            }
                            if (mouse.modifiers & Qt.ShiftModifier) {
                                root.selectRange(root.anchorIndex, index)
                            } else if (mouse.modifiers & Qt.ControlModifier) {
                                root.toggleSelection(index)
                            } else if (!root._isSelected(index)) {
                                root.selectOnly(index)
                            }
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed || _dragStarted || mouse.button === Qt.RightButton) return
                            var dx = mouse.x - _pressPoint.x
                            var dy = mouse.y - _pressPoint.y
                            if (dx*dx + dy*dy > 36) {
                                _dragStarted = true
                                root._startSystemDrag(cell, root._dragPathsForRow(model.path))
                            }
                        }
                        onDoubleClicked: root.activateRow(index)
                    }
                }

                // Background empty-area click clears selection
                // (left); right-click pops the panel bg menu.
                MouseArea {
                    anchors.fill: parent
                    z: -1
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: (mouse) => {
                        root.activated()
                        entryGrid.forceActiveFocus()
                        if (mouse.button === Qt.RightButton) {
                            root._popupBgMenu()
                            return
                        }
                        root.clearSelection()
                        mouse.accepted = false
                    }
                }

                Keys.onPressed: (event) => root._handleKey(event)
            }

            // ── Tree view ─────────────────────────────────────────
            // Flat ListView styled as a tree: depth drives indentation,
            // disclosure triangle calls DirectoryTree.expand/collapse.
            ListView {
                id: entryTree
                anchors.fill: parent
                visible: root.viewMode === "tree"
                model: treeModel
                clip: true
                focus: visible
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: UfbScrollBar {}

                TapHandler {
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onTapped: (eventPoint, button) => {
                        root.activated()
                        if (button === Qt.RightButton) {
                            if (root._menuClickConsumed) {
                                root._menuClickConsumed = false
                            } else {
                                root._popupBgMenu()
                            }
                        } else {
                            // Left tap on empty area clears the selection
                            // (row taps are owned by the row's MouseArea).
                            var p = eventPoint.position
                            if (entryTree.indexAt(p.x + entryTree.contentX,
                                                  p.y + entryTree.contentY) < 0)
                                root.clearSelection()
                        }
                    }
                }

                delegate: Rectangle {
                    id: treeRow
                    width: entryTree.width
                    height: 22
                    property bool selected: root._isTreeSelected(model.path)
                    property bool dropTarget: model.isDir
                        && root._dropTargetPath === model.path
                    color: dropTarget
                        ? Theme.colors.accentSelected
                        : selected
                        ? (root.active ? Theme.colors.accentSelected : Theme.colors.accentMuted)
                        : treeRowHover.containsMouse
                        ? "#262626"
                        : (index % 2 === 1 ? Theme.colors.surfaceAlt : "transparent")

                    // Row-wide MouseArea sits BEHIND the Row layout so
                    // the triangle MouseArea inside Row gets first crack
                    // at clicks. Empty regions (label gaps, padding) fall
                    // through to here.
                    MouseArea {
                        id: treeRowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        preventStealing: true
                        property point _pressPoint
                        property bool _dragStarted: false
                        onPressed: (mouse) => {
                            root.activated()
                            entryTree.forceActiveFocus()
                            // Modifier-aware multi-select.
                            //   Ctrl  - toggle this path in the set
                            //   Shift - extend from current anchor
                            //           (treeCurrentPath) along the
                            //           visible tree to this path
                            //   plain - reset to single selection
                            if (mouse.button === Qt.LeftButton
                                && (mouse.modifiers & Qt.ControlModifier)) {
                                var s = root.treeSelectedPaths.slice()
                                var i = s.indexOf(model.path)
                                if (i >= 0) s.splice(i, 1)
                                else s.push(model.path)
                                root._setTreeSelection(s)
                                root.treeCurrentPath = model.path
                            } else if (mouse.button === Qt.LeftButton
                                       && (mouse.modifiers & Qt.ShiftModifier)
                                       && root.treeCurrentPath.length > 0) {
                                root._treeSelectRange(root.treeCurrentPath, model.path)
                                // Anchor stays where it was so subsequent
                                // Shift+Click extends from the same point.
                            } else if (mouse.button === Qt.LeftButton
                                       && !root._isTreeSelected(model.path)) {
                                // Plain click on an UNselected row → single
                                // select. If the row is already part of a
                                // multi-selection, leave it intact so a drag
                                // carries the whole set (matches list/grid;
                                // also keeps right-click from clobbering a
                                // multi-selection before the context menu).
                                root.treeCurrentPath = model.path
                                root._setTreeSelection([model.path])
                            }
                            _pressPoint = Qt.point(mouse.x, mouse.y)
                            _dragStarted = false
                            if (mouse.button === Qt.RightButton) {
                                // If the pressed row isn't already in
                                // the selection, replace it with this
                                // row so the menu acts on something
                                // meaningful.
                                if (!root._isTreeSelected(model.path)) {
                                    root.treeCurrentPath = model.path
                                    root._setTreeSelection([model.path])
                                }
                                root._popupRowMenu(-1, model)
                            }
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed || _dragStarted || mouse.button === Qt.RightButton) return
                            var dx = mouse.x - _pressPoint.x
                            var dy = mouse.y - _pressPoint.y
                            if (dx*dx + dy*dy > 36) {
                                _dragStarted = true
                                // Drag the full selection if this row
                                // is part of it; otherwise just this
                                // path.
                                var paths = root._isTreeSelected(model.path)
                                    ? root.treeSelectedPaths.slice()
                                    : [model.path]
                                root._startSystemDrag(treeRow, paths)
                            }
                        }
                        onDoubleClicked: {
                            if (model.isDir) {
                                if (model.expanded) {
                                    tree.collapse(model.path)
                                } else {
                                    tree.expand(model.path)
                                }
                            } else {
                                FileOps.open_file(model.path)
                            }
                        }
                    }

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 6 + model.depth * 18
                        anchors.rightMargin: 10 + Theme.dim.scrollBarWidth
                        spacing: 4

                        // Disclosure caret. 18px wide hit area (was 12,
                        // which was hard to land precisely especially
                        // at deep indentation). Phosphor caret icon at
                        // toolbar size for legibility.
                        Item {
                            width: 18
                            height: parent.height
                            anchors.verticalCenter: parent.verticalCenter
                            Icon {
                                anchors.centerIn: parent
                                visible: model.isDir && model.hasChildren
                                name: model.expanded ? "caret-down" : "caret-right"
                                size: Theme.icon.sizeToolbar
                                color: treeRowHover.containsMouse
                                    ? Theme.colors.text
                                    : Theme.colors.textMuted
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: model.isDir && model.hasChildren
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.activated()
                                    if (model.expanded) {
                                        tree.collapse(model.path)
                                    } else {
                                        tree.expand(model.path)
                                    }
                                }
                            }
                        }

                        FileTypeIcon {
                            id: treeRowIcon
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16
                            isDir: model.isDir
                            extension: model.extension
                            sourceSizePx: 16
                            fallbackColor: treeRow.selected
                                ? "#ffffff" : Theme.colors.text
                        }

                        Item { width: 6; height: 1 }

                        Label {
                            text: model.name
                            color: treeRow.selected
                                ? "#ffffff"
                                : Theme.colors.text
                            font.pixelSize: 12
                            anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideRight
                        }
                    }
                }

                // Background click clears tree selection (left);
                // right-click pops the panel bg menu.
                MouseArea {
                    anchors.fill: parent
                    z: -1
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: (mouse) => {
                        root.activated()
                        entryTree.forceActiveFocus()
                        if (mouse.button === Qt.RightButton) {
                            root._popupBgMenu()
                            return
                        }
                        root.treeCurrentPath = ""
                        mouse.accepted = false
                    }
                }
            }

            // Empty state — covers list/grid; tree has its own emptiness
            // handled by the underlying DirectoryTree (no rows = nothing
            // to render, the loading/error state surfaces in the banner).
            Label {
                anchors.centerIn: parent
                visible: root.viewMode !== "tree"
                    && entriesModel.count === 0
                    && !root.dir.loading
                    && root.dir.error.length === 0
                text: qsTr("(empty folder)")
                color: "#555"
                font.pixelSize: 12
                font.italic: true
            }

            // Passive tap handler at the panel scope. The view bodies
            // (entryList/entryGrid/entryTree) are Flickables which
            // swallow press events on empty area for their own
            // gesture detection — a sibling MouseArea at z:-1 never
            // sees those clicks. A TapHandler is a "passive" pointer
            // handler that coexists with Flickable and fires on every
            // tap, including taps that are also exclusively grabbed
            // by a delegate's MouseArea. We use it to:
            //   1. activate this pane on any click
            //   2. surface the bg menu on a right-click that didn't
            //      hit a row (the row delegate's right-click handler
            //      sets _menuClickConsumed to skip our bg-menu fire)
            TapHandler {
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onTapped: (eventPoint, button) => {
                    root.activated()
                    if (button === Qt.RightButton) {
                        if (root._menuClickConsumed) {
                            root._menuClickConsumed = false
                        } else {
                            root._popupBgMenu()
                        }
                    }
                }
            }
        }

        // ── Footer ────────────────────────────────────────────────────
        // Layout (when expanded):
        //   Left pane:  [New Job (fillWidth)]     [Collapse]
        //   Right pane: [Collapse]     [New Job (fillWidth)]
        //
        // Collapse toggles pin to the INNER edge of the dual-browser
        // (toward the SplitView divider) so the two toggles cluster
        // around the splitter instead of hugging the window edges.
        // The collapsed re-open strip uses the same inner-edge anchor
        // so the button doesn't jump horizontally on collapse/expand.
        // New Job appears only inside a bookmarked jobs folder; an
        // Item spacer takes its place otherwise so the collapse
        // toggle stays anchored to the inner edge.
        //
        // Tonal `toolbar` fill makes the action strip read as a
        // distinct footer area against the `surface`-coloured view
        // panel above. Same idiom as the top toolbar block.
        Rectangle {
            id: paneFooter
            visible: !root.collapsed
                && (root.allowCollapse || root._currentIsProjectFolder)
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.dim.toolStripHeight
            color: Theme.colors.toolbar

            RowLayout {
                anchors.fill: parent
                spacing: 0

                // Right pane: collapse on the LEFT - leading position.
                FlatButton {
                    visible: root.allowCollapse && root.paneSide === "right"
                    iconName: "caret-double-right"
                    tooltip: qsTr("Collapse pane")
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    onClicked: root.toggleCollapsed()
                }

                // New Job button (or spacer when not in a jobs folder).
                // Default variant - no accent fill. The button's purpose
                // is clear from icon + label; the prior "primary" variant
                // painted it as an always-on highlight in jobs folders,
                // which read as state rather than an action.
                FlatButton {
                    visible: root._currentIsProjectFolder
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: "briefcase"
                    text: qsTr("New Job")
                    tooltip: qsTr("Create a new job folder from the project template")
                    onClicked: {
                        root.activated()
                        newJobDialog.openFor(root.dir.current_path)
                    }
                }
                Item {
                    visible: !root._currentIsProjectFolder
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                }

                // Left pane: collapse on the RIGHT - trailing position.
                FlatButton {
                    visible: root.allowCollapse && root.paneSide === "left"
                    iconName: "caret-double-left"
                    tooltip: qsTr("Collapse pane")
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    onClicked: root.toggleCollapsed()
                }
            }
        }
    }

    // ── Shared keyboard handler ───────────────────────────────────────
    // Both views forward to this. Grid horizontal nav (Left/Right)
    // operates on flat index — visually that's "previous/next cell"
    // which is what users expect in a wrap-around grid.
    function _handleKey(event) {
        var n
        if (event.key === Qt.Key_Down) {
            n = Math.min(entriesModel.count - 1, Math.max(0, root.currentIndex + (root.viewMode === "grid" ? _gridColCount() : 1)))
            _moveTo(n, event.modifiers)
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            n = Math.max(0, root.currentIndex - (root.viewMode === "grid" ? _gridColCount() : 1))
            _moveTo(n, event.modifiers)
            event.accepted = true
        } else if (event.key === Qt.Key_Right && root.viewMode === "grid") {
            n = Math.min(entriesModel.count - 1, root.currentIndex + 1)
            _moveTo(n, event.modifiers)
            event.accepted = true
        } else if (event.key === Qt.Key_Left && root.viewMode === "grid") {
            n = Math.max(0, root.currentIndex - 1)
            _moveTo(n, event.modifiers)
            event.accepted = true
        } else if (event.key === Qt.Key_Home) {
            _moveTo(0, event.modifiers)
            event.accepted = true
        } else if (event.key === Qt.Key_End) {
            _moveTo(entriesModel.count - 1, event.modifiers)
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.activateRow(root.currentIndex)
            event.accepted = true
        } else if (event.key === Qt.Key_Backspace) {
            root.dir.navigate_up()
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            root.clearSelection()
            event.accepted = true
        } else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
            root.selectAll()
            event.accepted = true
        } else if (event.key === Qt.Key_Space) {
            // QuickLook-style preview of the current item.
            var p = ""
            var isDir = false
            if (root.viewMode === "tree") {
                p = root.treeCurrentPath
                var ti = root._treeIndexOfPath(p)
                if (ti >= 0) isDir = !!treeModel.get(ti).isDir
            } else if (root.currentIndex >= 0
                       && root.currentIndex < entriesModel.count) {
                var e = entriesModel.get(root.currentIndex)
                p = e.path
                isDir = e.isDir
            }
            if (p.length > 0 && !isDir
                && Window.window && Window.window.openPreview) {
                Window.window.openPreview(p, root)
                event.accepted = true
            }
        }
    }

    /// Advance the cursor to the next previewable (non-directory) entry in
    /// `dir` (+1 / -1) and return its path, or "" at an edge. Used by the
    /// lightbox's Left/Right media navigation. List/grid only for now.
    function previewStep(dir) {
        if (root.viewMode === "tree") return ""
        var n = root.currentIndex + dir
        while (n >= 0 && n < entriesModel.count) {
            var e = entriesModel.get(n)
            if (e && !e.isDir) {
                root._moveTo(n, 0)
                return e.path
            }
            n += dir
        }
        return ""
    }

    function _moveTo(n, modifiers) {
        if (modifiers & Qt.ShiftModifier) {
            root.selectRange(root.anchorIndex < 0 ? root.currentIndex : root.anchorIndex, n)
        } else {
            root.selectOnly(n)
        }
        // Keep both views' visible window centred on current.
        if (root.viewMode === "grid") {
            entryGrid.positionViewAtIndex(n, GridView.Contain)
        } else {
            entryList.positionViewAtIndex(n, ListView.Contain)
        }
    }

    function _gridColCount() {
        var w = entryGrid.width
        var cw = entryGrid.cellWidth
        return Math.max(1, Math.floor(w / cw))
    }

    // ─── helpers ───────────────────────────────────────────────────────

    function formatSize(bytes) {
        if (bytes === 0) return "0 B"
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
    }

    function formatDate(epochMs) {
        if (!epochMs) return ""
        var d = new Date(epochMs)
        return d.toLocaleString(Qt.locale(), "yyyy-MM-dd hh:mm")
    }

    // ── Focus / drop-target overlay ──────────────────────────────────
    // Drawn ABOVE every child (z:9999) so the indicator paints on top
    // of the path bar, list/grid/tree, and footer instead of insetting
    // their layout. Inactive panes have a fully transparent overlay
    // (border.width: 0) — zero chrome, zero layout cost. Mirror image
    // of the Topaz-style "halo on focus" pattern.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        z: 9999
        // Pointer events fall through so the user can still click the
        // contents underneath the overlay.
        // (Rectangle has no MouseArea so this is implicit; spelling it
        // out keeps future refactors honest.)
        border.color: root.dropActive
            ? Theme.colors.success
            : (root.active ? Theme.colors.accentSelected : "transparent")
        border.width: root.dropActive ? 2 : (root.active ? Theme.dim.border : 0)
        visible: border.width > 0
    }
}
