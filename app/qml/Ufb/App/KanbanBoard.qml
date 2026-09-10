// KanbanBoard — the project panel's alternative "board" view: the folder's
// items as cards in lanes, one lane per option of a chosen dropdown /
// priority column (plus Unset), or Checked / Unchecked for a checkbox
// column. Dragging one or many cards to another lane sets that field on
// each of them.
//
// Pure view. ItemListPanel owns the data (items in its Sort order, the
// metadata table, the tracked set, the lane column) and every write: this
// component only emits signals. Selection: `selectedItemPath` stays the
// panel's single source of truth (the browser panes follow it);
// `selectedItemPaths` is the board's multi-select on top of that.
//
// Drag is INTERNAL (Drag.Internal on one persistent proxy item), not an OS
// drag: no Windows DoDragDrop modal loop, no stale hover state, and the
// proxy outlives any card delegate a metadata refresh might rebuild
// mid-gesture (same lesson as FileBrowser's dragProxy). Lane rebuilds are
// deferred while a drag is active.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Rectangle {
    id: board
    color: Theme.colors.surface

    // ── Inputs ──────────────────────────────────────────────────────
    /// [{ name, path, modified }] — already in the panel's Sort order;
    /// the board only partitions, never sorts.
    property var items: []
    /// { [itemPath]: parsedMetadataBlob }
    property var metadataByPath: ({})
    /// { [itemPath]: true } for tracked items.
    property var trackedSet: ({})
    /// The lane column: an ItemListPanel.visibleColumns element
    /// ({ columnName, columnType, options: [{name, color}] }) or null.
    property var laneColumn: null
    property string selectedItemPath: ""
    property var selectedItemPaths: []
    property int laneWidth: 220

    // ── Outputs ─────────────────────────────────────────────────────
    signal cardClicked(string path)
    signal cardActivated(string path)
    signal cardContextMenu(string path, string name)
    signal starToggled(string path)
    /// `paths` are the items to move; `valueJson` is the JSON-encoded
    /// value for the lane column ("\"Rendered\"", "true", "null").
    signal laneDropRequested(var paths, string valueJson)

    // ── Lane model ──────────────────────────────────────────────────
    /// [{ key, label, pill, color, valueJson, droppable, items: [rows] }]
    property var lanes: []
    property string _lanesSig: ""
    property bool _laneRebuildDeferred: false

    // Functions, not bindings: onLaneColumnChanged runs before
    // dependent bindings re-evaluate, so a bound flag would still
    // describe the previous column during the rebuild it triggers
    // (switching dropdown -> Tracked produced a lone Unset lane).
    function _laneType() {
        return (laneColumn !== null && laneColumn !== undefined)
            ? String(laneColumn.columnType).toLowerCase() : ""
    }
    function _isCheckbox() { return _laneType() === "checkbox" }
    /// The panel's synthetic "Tracked" column: lanes come from the
    /// tracked set, not from a metadata field.
    function _isTrackedCol() { return _laneType() === "tracked" }

    /// Lane key for an item path (tracked column reads trackedSet,
    /// everything else the metadata cell).
    function _laneKeyForPath(p) {
        if (_isTrackedCol()) return trackedSet[p] === true ? "tracked" : "untracked"
        var md = metadataByPath[p] || {}
        return _laneKeyFor(md[laneColumn.columnName])
    }
    /// Lane key for a raw cell value. Option match is plain string
    /// equality on the option name (same rule as CellEditor.valueColor).
    function _laneKeyFor(raw) {
        if (_isCheckbox())
            return (raw === true || raw === "true") ? "checked" : "unchecked"
        if (raw === undefined || raw === null || raw === "") return "unset"
        var s = String(raw)
        var opts = (laneColumn && laneColumn.options) ? laneColumn.options : []
        for (var i = 0; i < opts.length; ++i)
            if (opts[i] && opts[i].name === s) return "opt:" + s
        return "other:" + s
    }

    function _rebuildLanes() {
        if (!visible) return
        if (dragProxy.Drag.active) { _laneRebuildDeferred = true; return }
        if (!laneColumn) {
            if (lanes.length > 0) { lanes = []; _lanesSig = "" }
            return
        }
        var fixed = []
        var byKey = ({})
        function add(l) { l.items = []; fixed.push(l); byKey[l.key] = l }
        if (_isTrackedCol()) {
            // The card's own star already carries the state: no pill.
            add({ key: "tracked",   label: qsTr("Tracked"),   pill: "",
                  color: "#e6c84a", valueJson: "true",  droppable: true })
            add({ key: "untracked", label: qsTr("Untracked"), pill: "",
                  color: "", valueJson: "false", droppable: true })
        } else if (_isCheckbox()) {
            add({ key: "checked",   label: qsTr("Checked"),   pill: "✓",
                  color: Theme.colors.accent, valueJson: "true",  droppable: true })
            add({ key: "unchecked", label: qsTr("Unchecked"), pill: "",
                  color: "", valueJson: "false", droppable: true })
        } else {
            var opts = laneColumn.options || []
            for (var i = 0; i < opts.length; ++i) {
                var o = opts[i]
                if (!o || !o.name) continue
                add({ key: "opt:" + o.name, label: o.name, pill: o.name,
                      color: o.color || "", valueJson: JSON.stringify(o.name),
                      droppable: true })
            }
            add({ key: "unset", label: qsTr("Unset"), pill: "",
                  color: "", valueJson: "null", droppable: true })
        }
        // Values that match no option get their own trailing lane, not
        // droppable: stale/renamed values stay visible instead of being
        // folded into Unset (a drop onto Unset writes null).
        var others = []
        for (var j = 0; j < items.length; ++j) {
            var it = items[j]
            var key = _laneKeyForPath(it.path)
            var lane = byKey[key]
            if (!lane) {
                var v = key.substring(6)
                lane = { key: key, label: qsTr("Other: %1").arg(v), pill: v,
                         color: "", valueJson: "", droppable: false, items: [] }
                byKey[key] = lane
                others.push(lane)
            }
            lane.items.push(it)
        }
        var all = fixed.concat(others)
        // Assigning a JS array re-instantiates every lane delegate (and
        // resets lane scroll), so skip when nothing visible changed.
        var sig = all.map(function(l) {
            return l.key + "\t" + l.label + "\t" + l.color + "="
                 + l.items.map(function(r) { return r.path }).join("|")
        }).join("\n")
        if (sig === _lanesSig) return
        _lanesSig = sig
        lanes = all

        // Prune the multi-select to items still present.
        var present = ({})
        for (var k = 0; k < items.length; ++k) present[items[k].path] = true
        var kept = selectedItemPaths.filter(function(p) { return present[p] === true })
        if (kept.length !== selectedItemPaths.length) selectedItemPaths = kept
    }
    onItemsChanged: _rebuildLanes()
    onMetadataByPathChanged: _rebuildLanes()
    onTrackedSetChanged: if (_isTrackedCol()) _rebuildLanes()
    onLaneColumnChanged: { _lanesSig = ""; _rebuildLanes() }
    onVisibleChanged: if (visible) _rebuildLanes()

    // ── Selection (TrackerView idiom, lane-scoped anchor) ───────────
    property var _anchor: ({ laneKey: "", index: -1 })
    function _isSelected(p) { return selectedItemPaths.indexOf(p) >= 0 }
    function _selectOnly(laneKey, idx, p) {
        selectedItemPaths = [p]
        _anchor = { laneKey: laneKey, index: idx }
    }
    function _toggleSelected(laneKey, idx, p) {
        var s = selectedItemPaths.slice()
        var pos = s.indexOf(p)
        if (pos >= 0) s.splice(pos, 1)
        else s.push(p)
        selectedItemPaths = s
        _anchor = { laneKey: laneKey, index: idx }
    }
    /// Shift-range within one lane; across lanes it degrades to a plain
    /// select of the clicked card.
    function _selectRange(laneKey, toIdx, laneItems) {
        if (_anchor.laneKey !== laneKey || _anchor.index < 0
                || _anchor.index >= laneItems.length) {
            _selectOnly(laneKey, toIdx, laneItems[toIdx].path)
            return
        }
        var lo = Math.min(_anchor.index, toIdx)
        var hi = Math.max(_anchor.index, toIdx)
        var s = []
        for (var i = lo; i <= hi && i < laneItems.length; ++i) s.push(laneItems[i].path)
        selectedItemPaths = s
    }
    function _clearSelection() {
        selectedItemPaths = []
        _anchor = { laneKey: "", index: -1 }
    }

    /// Same rendering as ItemListPanel.formatDate (epoch ms -> yyyy-MM-dd).
    function _formatDate(epochMs) {
        if (!epochMs) return ""
        return new Date(epochMs).toLocaleString(Qt.locale(), "yyyy-MM-dd")
    }

    /// Crude luminance check for legible pill text (copied from
    /// CellEditor — an instance function there, not shareable).
    function _isDarkColor(hex) {
        if (!hex || hex.length === 0) return true
        var s = hex.replace("#", "")
        if (s.length === 3) s = s[0]+s[0]+s[1]+s[1]+s[2]+s[2]
        if (s.length !== 6) return true
        var r = parseInt(s.substr(0,2), 16)
        var g = parseInt(s.substr(2,2), 16)
        var b = parseInt(s.substr(4,2), 16)
        return (0.299*r + 0.587*g + 0.114*b) < 140
    }

    // ── Drag machinery (internal) ───────────────────────────────────
    property var _dragPaths: []
    property string _dragName: ""
    property var _pendingDrop: null         // { laneKey, valueJson } set by a lane's DropArea
    readonly property bool dragActive: dragProxy.Drag.active
    onDragActiveChanged: {
        if (!dragActive && _laneRebuildDeferred) {
            _laneRebuildDeferred = false
            _rebuildLanes()
        }
    }
    // Window deactivation mid-drag: the release never arrives.
    readonly property bool _winActive: Window.active
    on_WinActiveChanged: if (!_winActive && dragActive) _cancelDrag()
    Shortcut {
        sequences: ["Escape"]
        enabled: board.dragActive
        onActivated: board._cancelDrag()
    }

    /// Drag set = the selection when the pressed card is part of it,
    /// else just that card (FileBrowser's rule).
    function _beginDrag(path, name, pt) {
        _dragPaths = _isSelected(path) ? selectedItemPaths.slice() : [path]
        _dragName = name
        _pendingDrop = null
        dragProxy.x = pt.x
        dragProxy.y = pt.y
        dragProxy.Drag.active = true
    }
    function _moveDrag(pt) {
        dragProxy.x = pt.x
        dragProxy.y = pt.y
    }
    function _finishDrag() {
        if (!dragProxy.Drag.active) return
        dragProxy.Drag.drop()
        dragProxy.Drag.active = false
        var pd = _pendingDrop
        var paths = _dragPaths
        _pendingDrop = null
        _dragPaths = []
        if (!pd || !laneColumn || !pd.valueJson) return
        // Cards already in the target lane (origin-lane drop, or a
        // Ctrl-selection that spans lanes) are left alone.
        var moving = paths.filter(function(p) {
            return _laneKeyForPath(p) !== pd.laneKey
        })
        if (moving.length === 0) return
        var vj = pd.valueJson
        // Deferred: the panel's write bumps metadata_rev synchronously,
        // which rebuilds the lanes and would tear the card delegate
        // whose handler we are in out from under us.
        Qt.callLater(function() { board.laneDropRequested(moving, vj) })
    }
    function _cancelDrag() {
        _pendingDrop = null
        _dragPaths = []
        if (dragProxy.Drag.active) dragProxy.Drag.cancel()
        dragProxy.Drag.active = false
    }

    // ── Lanes ───────────────────────────────────────────────────────
    Flickable {
        id: laneFlick
        anchors.fill: parent
        clip: true
        contentWidth: laneRow.width
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: UfbScrollBar {}

        Row {
            id: laneRow
            spacing: Theme.dim.padding
            padding: Theme.dim.padding
            Repeater {
                model: board.lanes
                delegate: Rectangle {
                    id: lane
                    required property var modelData
                    required property int index
                    readonly property bool hasColor: modelData.color.length > 0
                    width: board.laneWidth
                    height: laneFlick.height - Theme.dim.padding * 2 - Theme.dim.scrollBarWidth
                    radius: Theme.dim.radius
                    color: (laneDrop.containsDrag && modelData.droppable)
                        ? Theme.colors.accentMuted : Theme.colors.surfaceAlt

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0
                        // Header: swatch + label + count.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 26
                            Rectangle {
                                id: swatch
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 10; height: 10; radius: 5
                                color: lane.hasColor ? lane.modelData.color : Theme.colors.borderStrong
                            }
                            Label {
                                anchors.left: swatch.right
                                anchors.leftMargin: 6
                                anchors.right: countLabel.left
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                text: lane.modelData.label
                                color: lane.modelData.droppable ? Theme.colors.text : Theme.colors.textMuted
                                font.pixelSize: Theme.font.sizeSmall
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Label {
                                id: countLabel
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: lane.modelData.items.length
                                color: Theme.colors.textSubtle
                                font.pixelSize: Theme.font.sizeSmall
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: Theme.dim.divider
                            color: Theme.colors.divider
                        }

                        ListView {
                            id: cardList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: lane.modelData.items
                            spacing: 4
                            clip: true
                            leftMargin: 6
                            rightMargin: 6
                            topMargin: 6
                            bottomMargin: 6
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: UfbScrollBar {}

                            delegate: Rectangle {
                                id: card
                                required property var modelData
                                required property int index
                                readonly property string itemPath: modelData.path
                                readonly property bool selected:
                                    board._isSelected(itemPath) || itemPath === board.selectedItemPath
                                width: cardList.width - cardList.leftMargin - cardList.rightMargin
                                height: Theme.dim.itemRowHeight * 2
                                radius: Theme.dim.radius
                                color: selected
                                    ? Theme.colors.accentSelected
                                    : (cardMa.containsMouse ? Theme.colors.surfaceHover : Theme.colors.surface)
                                border.width: 1
                                border.color: selected ? Theme.colors.accent : Theme.colors.divider

                                // Declared first so the star's MouseArea (later) sits above it.
                                MouseArea {
                                    id: cardMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    // The lane ListView would otherwise steal a vertical
                                    // drag as a flick and starve positionChanged.
                                    preventStealing: true
                                    cursorShape: dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor
                                    property real _px: 0
                                    property real _py: 0
                                    property bool dragging: false
                                    onPressed: (mouse) => {
                                        _px = mouse.x; _py = mouse.y; dragging = false
                                        var p = card.itemPath, n = card.modelData.name
                                        var key = lane.modelData.key, idx = card.index
                                        if (mouse.button === Qt.RightButton) {
                                            if (!board._isSelected(p)) board._selectOnly(key, idx, p)
                                            board.cardClicked(p)
                                            board.cardContextMenu(p, n)
                                            return
                                        }
                                        if (mouse.modifiers & Qt.ControlModifier) {
                                            board._toggleSelected(key, idx, p)
                                            if (board._isSelected(p)) board.cardClicked(p)
                                            return
                                        }
                                        if (mouse.modifiers & Qt.ShiftModifier) {
                                            board._selectRange(key, idx, lane.modelData.items)
                                            board.cardClicked(p)
                                            return
                                        }
                                        // Plain press on an already-selected card keeps the
                                        // multi-select (so a drag moves all of it); the
                                        // release below collapses it if no drag happened.
                                        if (!board._isSelected(p)) board._selectOnly(key, idx, p)
                                        board.cardClicked(p)
                                    }
                                    onPositionChanged: (mouse) => {
                                        if (!pressed || (mouse.buttons & Qt.LeftButton) === 0) return
                                        var pt = cardMa.mapToItem(board, mouse.x, mouse.y)
                                        if (!dragging) {
                                            var dx = mouse.x - _px, dy = mouse.y - _py
                                            if (dx * dx + dy * dy <= 36) return
                                            dragging = true
                                            board._beginDrag(card.itemPath, card.modelData.name, pt)
                                        }
                                        board._moveDrag(pt)
                                    }
                                    onReleased: (mouse) => {
                                        if (dragging) {
                                            dragging = false
                                            board._finishDrag()
                                        } else if (mouse.button === Qt.LeftButton
                                                   && !(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))
                                                   && board.selectedItemPaths.length > 1) {
                                            board._selectOnly(lane.modelData.key, card.index, card.itemPath)
                                        }
                                    }
                                    // A press that ends without a release (stolen grab,
                                    // window deactivated) must not leave a drag hanging.
                                    // Deferred so a normal release runs first.
                                    onPressedChanged: {
                                        if (!pressed && dragging) {
                                            Qt.callLater(function() {
                                                if (cardMa.dragging) { cardMa.dragging = false; board._cancelDrag() }
                                            })
                                        }
                                    }
                                    onDoubleClicked: (mouse) => {
                                        if (mouse.button === Qt.LeftButton) board.cardActivated(card.itemPath)
                                    }
                                }

                                // Row 1: star + name.
                                Label {
                                    id: starLabel
                                    x: 8
                                    y: 5
                                    text: board.trackedSet[card.itemPath] === true ? "★" : "☆"
                                    color: board.trackedSet[card.itemPath] === true
                                        ? "#e6c84a" // tracked-star yellow — same as the list row
                                        : (starMa.containsMouse ? Theme.colors.textMuted : Theme.colors.borderStrong)
                                    font.pixelSize: Theme.font.sizeHeading
                                    MouseArea {
                                        id: starMa
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        hoverEnabled: true
                                        onClicked: board.starToggled(card.itemPath)
                                    }
                                }
                                Label {
                                    anchors.left: starLabel.right
                                    anchors.leftMargin: 6
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: starLabel.verticalCenter
                                    text: card.modelData.name
                                    color: card.selected ? Theme.colors.textBright : Theme.colors.text
                                    font.pixelSize: Theme.font.sizeBody
                                    elide: Text.ElideMiddle
                                }
                                // Row 2: the lane column's pill (CellEditor's dropdown
                                // pill without the chevron). Hidden for value-less lanes.
                                Rectangle {
                                    id: pill
                                    visible: lane.modelData.pill.length > 0
                                    anchors.left: parent.left
                                    anchors.leftMargin: 8
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 7
                                    height: 18
                                    width: Math.min(pillLabel.implicitWidth + 12,
                                                    card.width - 16 - dateLabel.width - 8)
                                    radius: 3
                                    color: lane.hasColor ? lane.modelData.color : "transparent"
                                    border.width: 1
                                    border.color: lane.hasColor ? "#222" : "#333"
                                    Label {
                                        id: pillLabel
                                        anchors.fill: parent
                                        anchors.leftMargin: 6
                                        anchors.rightMargin: 6
                                        verticalAlignment: Text.AlignVCenter
                                        text: lane.modelData.pill
                                        color: !lane.hasColor
                                            ? (card.selected ? "#cfd9e6" : "#cccccc")
                                            : (board._isDarkColor(lane.modelData.color) ? "#ffffff" : "#111111")
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                                // Modified date, bottom-right (the list's
                                // Modified column, same tiny mono treatment).
                                Label {
                                    id: dateLabel
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: pill.verticalCenter
                                    text: board._formatDate(card.modelData.modified)
                                    color: card.selected ? Theme.colors.textBright : Theme.colors.textSubtle
                                    font.pixelSize: Theme.font.sizeTiny
                                    font.family: Theme.font.mono
                                }
                            }
                        }
                    }

                    // Whole lane is the drop target (header included).
                    DropArea {
                        id: laneDrop
                        anchors.fill: parent
                        keys: ["ufb/kanban-card"]
                        enabled: lane.modelData.droppable
                        onDropped: (drop) => {
                            board._pendingDrop = { laneKey: lane.modelData.key,
                                                   valueJson: lane.modelData.valueJson }
                            drop.accept()
                        }
                    }
                }
            }
        }

        // Empty states.
        Label {
            anchors.centerIn: parent
            visible: board.lanes.length === 0
            text: board.laneColumn
                ? qsTr("(no items)")
                : qsTr("(pick a dropdown, priority or checkbox column for the lanes)")
            color: Theme.colors.textSubtle
            font.pixelSize: Theme.font.sizeSmall
            font.italic: true
        }
    }

    // ── Drag proxy + ghost ──────────────────────────────────────────
    // Persistent (never a delegate child): a metadata refresh can rebuild
    // every card mid-gesture, and destroying the item that carries a live
    // Drag attached property aborts the app. Lives outside the Flickable so
    // board coordinates are its coordinates.
    Item {
        id: dragProxy
        width: 1
        height: 1
        z: 1000
        visible: Drag.active
        Drag.dragType: Drag.Internal
        Drag.keys: ["ufb/kanban-card"]
        Drag.hotSpot: Qt.point(0, 0)
        Drag.supportedActions: Qt.MoveAction
        Drag.proposedAction: Qt.MoveAction
        Rectangle {
            x: 12
            y: 12
            radius: 3
            color: Theme.colors.accent
            opacity: 0.92
            width: ghostLabel.implicitWidth + 16
            height: 22
            Label {
                id: ghostLabel
                anchors.centerIn: parent
                color: Theme.colors.textBright
                font.pixelSize: Theme.font.sizeSmall
                text: board._dragPaths.length > 1
                    ? qsTr("%1 items").arg(board._dragPaths.length)
                    : board._dragName
            }
        }
    }
}
