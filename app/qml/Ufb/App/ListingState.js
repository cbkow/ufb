// View-state preservation for in-place directory reloads.
//
// When a file operation changes a directory we're already viewing, the
// model is rebuilt (clear + append) — which would otherwise drop the
// user's selection, cursor and scroll position. These helpers capture
// that state keyed by PATH (not row index, which shifts when rows are
// inserted/removed) before the rebuild, and resolve it back to indices
// against the rebuilt model afterward. Paths that no longer exist
// (e.g. the file that was just deleted) are silently dropped.
//
// Pure / stateless so it can live in a `.pragma library` shared by
// FileBrowser and any other directory view.
.pragma library

// Capture a snapshot before a rebuild.
//   selectedPaths : array of currently-selected paths
//   currentPath   : path under the cursor (currentIndex), or ""
//   contentY      : current scroll offset of the view
function snapshot(selectedPaths, currentPath, contentY) {
    return {
        paths: (selectedPaths || []).slice(),
        current: currentPath || "",
        contentY: (typeof contentY === "number") ? contentY : 0
    }
}

// Resolve a snapshot against the rebuilt `model` (a ListModel exposing
// count and get(i).path). Returns:
//   { indexes: [int], currentIndex: int, contentY: number }
// - indexes: surviving selection, as fresh row indices
// - currentIndex: the snapshot's cursor path if it survived, else the
//   first surviving selected row, else 0 (or -1 for an empty model)
// - contentY: the saved scroll offset (caller clamps to the new
//   contentHeight)
function resolve(model, snap) {
    var byPath = ({})
    for (var i = 0; i < model.count; ++i) {
        byPath[model.get(i).path] = i
    }
    var indexes = []
    for (var j = 0; j < snap.paths.length; ++j) {
        var idx = byPath[snap.paths[j]]
        if (idx !== undefined) indexes.push(idx)
    }
    var currentIndex = -1
    if (snap.current.length > 0 && byPath[snap.current] !== undefined) {
        currentIndex = byPath[snap.current]
    } else if (indexes.length > 0) {
        currentIndex = indexes[0]
    } else if (model.count > 0) {
        currentIndex = 0
    }
    return { indexes: indexes, currentIndex: currentIndex, contentY: snap.contentY }
}

// Normalize a directory path for cross-form comparison: strip a single
// trailing separator, unify separators to "/", and (since UFB targets
// macOS + Windows, both case-insensitive filesystems) lowercase. Used
// to test whether a view's current directory is among an op's
// changed_dirs set.
function normDir(p) {
    if (!p) return ""
    var s = String(p).replace(/\\/g, "/")
    if (s.length > 1 && s.charAt(s.length - 1) === "/") {
        s = s.substring(0, s.length - 1)
    }
    return s.toLowerCase()
}

// True if `dir` is a member of the `dirs` array, compared normalized.
function dirInSet(dir, dirs) {
    var target = normDir(dir)
    if (target.length === 0) return false
    for (var i = 0; i < dirs.length; ++i) {
        if (normDir(dirs[i]) === target) return true
    }
    return false
}

// True if `child` is `ancestor` or lives somewhere beneath it. Used by
// tree mode: a change in any expanded subfolder under the tree root
// should refresh the tree.
function dirUnder(child, ancestor) {
    var a = normDir(ancestor)
    var c = normDir(child)
    if (a.length === 0 || c.length === 0) return false
    return c === a || c.indexOf(a + "/") === 0
}

// True if any dir in `dirs` is at or under `ancestor`.
function anyUnder(dirs, ancestor) {
    for (var i = 0; i < dirs.length; ++i) {
        if (dirUnder(dirs[i], ancestor)) return true
    }
    return false
}
