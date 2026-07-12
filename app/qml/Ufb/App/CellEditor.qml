// CellEditor — typed display + inline editor for one tracker cell.
//
// Picks an inline component by `col.columnType`:
//   text / number / date / links / notes  →  click to edit, TextField
//   dropdown / priority (with options)    →  colored value pill, click → ComboBox
//   checkbox                              →  always-on toggle
//   discovered + everything else          →  text fallback
//
// commit(newValue) is the host's persist hook.
//
// Important: every binding here references Item properties directly
// (cell.value, cell.col.columnType, cell.displayText) so the QML
// engine tracks them. An earlier version called helper functions
// (cell._displayString(), cell._colorFor(...)) inside bindings —
// QML doesn't track property reads inside function calls and the
// cell stopped re-rendering when value/col changed (visible as
// "metadata vanished from cells" after the typed-cells slice).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: cell

    /// Column descriptor — { columnName, columnType, columnWidth,
    /// options[], discovered, ... }. Not `required` so Repeater
    /// model swaps don't ReferenceError mid-rebuild; bindings below
    /// guard with optional chaining.
    property var col: null
    /// Current value. Display + editor coerce as appropriate.
    property var value: null
    /// True when the row that owns this cell is the visually-selected
    /// row. Drives text color so we don't get dark-on-dark.
    property bool selected: false
    /// True suppresses click-to-edit (used for read-only contexts).
    property bool readOnly: false

    /// Fired when the user finishes an edit.
    signal commit(var newValue)

    implicitHeight: 22

    // ── Computed properties (binding-trackable, null-safe) ──────────
    // columnType is normalized to lowercase — the legacy DB has a
    // mix of "text" / "Text" / "TEXT" depending on which version of
    // the legacy app wrote the row, and the switch below is
    // lowercase only.
    readonly property string columnType: col
        ? String(col.columnType || "text").toLowerCase()
        : "text"
    readonly property bool discoveredCol: col ? col.discovered === true : false

    /// Stringified display value for text-like cells.
    readonly property string displayText: {
        if (value === undefined || value === null) return ""
        if (columnType === "checkbox") return value ? "✓" : ""
        if (columnType === "date" && typeof value === "number") {
            var d = new Date(value)
            return d.toLocaleString(Qt.locale(), "yyyy-MM-dd")
        }
        return String(value)
    }
    /// Hex color string from the matching option, or "" if unmatched.
    readonly property string valueColor: {
        if (!col || !col.options) return ""
        for (var i = 0; i < col.options.length; ++i) {
            var o = col.options[i]
            if (!o) continue
            if (o.name === value) return o.color || ""
        }
        return ""
    }
    /// True when this column should render with a dropdown control.
    readonly property bool isDropdownLike:
        col !== null && col !== undefined
        && (columnType === "dropdown" || columnType === "priority")
        && Array.isArray(col.options) && col.options.length > 0

    /// Crude luminance check used to pick legible text color over a
    /// dropdown's option-color background.
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

    Loader {
        id: editorLoader
        anchors.fill: parent
        sourceComponent: {
            if (cell.discoveredCol) return textComp
            switch (cell.columnType) {
            case "checkbox": return checkboxComp
            case "dropdown":
            case "priority":
                // Always use dropdownComp for dropdown/priority so
                // the cell *looks* like a dropdown even when no
                // options are defined yet — the popup will be empty
                // and the user gets a clear "configure options" cue
                // instead of silently degrading to a text field.
                return dropdownComp
            case "number":   return numberComp
            case "date":     return dateComp
            case "notes":
            case "note":     return notesComp
            case "links":    return linksComp
            default:         return textComp
            }
        }
    }

    // ── text / fallback ─────────────────────────────────────────────
    Component {
        id: textComp
        Item {
            anchors.fill: parent
            property bool editing: false
            Label {
                visible: !parent.editing
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                verticalAlignment: Text.AlignVCenter
                text: cell.displayText
                color: cell.selected ? "#cfd9e6" : "#cccccc"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            MouseArea {
                anchors.fill: parent
                visible: !parent.editing && !cell.readOnly
                onClicked: parent.editing = true
            }
            Loader {
                anchors.fill: parent
                active: parent.editing
                sourceComponent: TextField {
                    text: cell.displayText
                    selectByMouse: true
                    Component.onCompleted: {
                        forceActiveFocus()
                        selectAll()
                    }
                    onAccepted: {
                        cell.commit(text)
                        parent.parent.editing = false
                    }
                    onActiveFocusChanged: {
                        if (!activeFocus && parent.parent.editing) {
                            cell.commit(text)
                            parent.parent.editing = false
                        }
                    }
                    Keys.onEscapePressed: parent.parent.editing = false
                }
            }
        }
    }

    // ── number ──────────────────────────────────────────────────────
    Component {
        id: numberComp
        Item {
            anchors.fill: parent
            property bool editing: false
            Label {
                visible: !parent.editing
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignRight
                text: cell.displayText
                color: cell.selected ? "#cfd9e6" : "#cccccc"
                font.pixelSize: 11
                font.family: "Consolas"
            }
            MouseArea {
                anchors.fill: parent
                visible: !parent.editing && !cell.readOnly
                onClicked: parent.editing = true
            }
            Loader {
                anchors.fill: parent
                active: parent.editing
                sourceComponent: TextField {
                    text: cell.displayText
                    selectByMouse: true
                    horizontalAlignment: TextInput.AlignRight
                    validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                    Component.onCompleted: {
                        forceActiveFocus()
                        selectAll()
                    }
                    function _doCommit() {
                        var num = parseFloat(text)
                        cell.commit(isNaN(num) ? null : num)
                        parent.parent.editing = false
                    }
                    onAccepted: _doCommit()
                    onActiveFocusChanged: { if (!activeFocus && parent.parent.editing) _doCommit() }
                    Keys.onEscapePressed: parent.parent.editing = false
                }
            }
        }
    }

    // ── checkbox ────────────────────────────────────────────────────
    Component {
        id: checkboxComp
        Item {
            anchors.fill: parent
            CheckBox {
                anchors.centerIn: parent
                checked: cell.value === true || cell.value === "true" || cell.value === 1
                enabled: !cell.readOnly
                onToggled: cell.commit(checked)
            }
        }
    }

    // ── dropdown ────────────────────────────────────────────────────
    Component {
        id: dropdownComp
        Item {
            anchors.fill: parent
            property bool editing: false
            // Display: colored pill (when value matches an option
            // color) + a small ▼ chevron so dropdown cells are
            // visually distinct from text cells even when empty.
            Rectangle {
                visible: !parent.editing
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                anchors.topMargin: 2
                anchors.bottomMargin: 2
                radius: 3
                color: cell.valueColor.length > 0 ? cell.valueColor : "transparent"
                border.color: cell.valueColor.length > 0 ? "#222" : "#333"
                border.width: 1
                Label {
                    id: dropdownValueLabel
                    anchors.left: parent.left
                    anchors.right: dropdownChevron.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 6
                    anchors.rightMargin: 4
                    verticalAlignment: Text.AlignVCenter
                    text: cell.displayText
                    color: cell.valueColor.length === 0
                        ? (cell.selected ? "#cfd9e6" : "#cccccc")
                        : (cell._isDarkColor(cell.valueColor) ? "#ffffff" : "#111111")
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Label {
                    id: dropdownChevron
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 4
                    text: "▼"
                    color: cell.valueColor.length === 0
                        ? "#777"
                        : (cell._isDarkColor(cell.valueColor) ? "#cccccc" : "#444444")
                    font.pixelSize: 8
                }
            }
            MouseArea {
                anchors.fill: parent
                visible: !parent.editing && !cell.readOnly
                onClicked: parent.editing = true
            }
            Loader {
                anchors.fill: parent
                active: parent.editing
                sourceComponent: ComboBox {
                    model: {
                        var arr = []
                        var opts = (cell.col && cell.col.options) ? cell.col.options : []
                        for (var i = 0; i < opts.length; ++i) {
                            arr.push(opts[i].name)
                        }
                        return arr
                    }
                    currentIndex: {
                        var arr = []
                        var opts = (cell.col && cell.col.options) ? cell.col.options : []
                        for (var i = 0; i < opts.length; ++i) {
                            arr.push(opts[i].name)
                        }
                        return Math.max(0, arr.indexOf(String(cell.value)))
                    }
                    Component.onCompleted: {
                        forceActiveFocus()
                        // Auto-pop the dropdown so a single cell click
                        // shows the choices instead of requiring a
                        // second click on the ComboBox arrow.
                        popup.open()
                    }
                    onActivated: {
                        var opts = (cell.col && cell.col.options) ? cell.col.options : []
                        if (currentIndex < 0 || currentIndex >= opts.length) return
                        cell.commit(opts[currentIndex].name)
                        parent.parent.editing = false
                    }
                    // Closing the popup (pick or click-out) ends edit
                    // mode. We rely on popup closing rather than
                    // activeFocus because focus toggles internally as
                    // the popup is being constructed.
                    Connections {
                        target: parent ? parent.parent : null
                        ignoreUnknownSignals: true
                    }
                    onPressedChanged: { /* keep alive; commit on activated */ }
                    onActiveFocusChanged: {
                        // Only end editing if popup is also closed —
                        // otherwise pressing the arrow steals focus
                        // briefly and we'd flap.
                        if (!activeFocus && !popup.opened) {
                            parent.parent.editing = false
                        }
                    }
                }
            }
        }
    }

    // ── date ────────────────────────────────────────────────────────
    // Click the cell → opens a calendar popup anchored below the cell.
    // Picking a date commits the timestamp (epoch ms) and closes the
    // popup. Stored values are read as either a number (epoch ms) or a
    // YYYY-MM-DD string — the legacy app wrote both shapes depending
    // on which version did the edit.
    Component {
        id: dateComp
        Item {
            id: dateRoot
            anchors.fill: parent
            // Parse the current value into a Date for the calendar's
            // initial position. Falls back to today when empty / unparseable.
            function _currentDate() {
                var v = cell.value
                if (typeof v === "number" && v > 0) return new Date(v)
                if (typeof v === "string" && v.length > 0) {
                    var d = new Date(v)
                    if (!isNaN(d.getTime())) return d
                }
                return new Date()
            }
            Label {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                verticalAlignment: Text.AlignVCenter
                text: cell.displayText
                color: cell.selected ? "#cfd9e6" : "#cccccc"
                font.pixelSize: 11
                font.family: "Consolas"
                elide: Text.ElideRight
            }
            MouseArea {
                anchors.fill: parent
                visible: !cell.readOnly
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    var d = dateRoot._currentDate()
                    calendarPopup.year = d.getFullYear()
                    calendarPopup.month = d.getMonth()
                    calendarPopup.open()
                }
            }
            Popup {
                id: calendarPopup
                property int year: new Date().getFullYear()
                property int month: new Date().getMonth()
                x: 0
                y: dateRoot.height
                width: 240
                height: 240
                padding: 6
                modal: false
                background: Rectangle {
                    color: Theme.colors.toolbar
                    border.color: Theme.colors.borderStrong
                    border.width: 1
                    radius: 3
                }
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        FlatButton {
                            iconName: "caret-left"
                            tooltip: qsTr("Previous month")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                if (calendarPopup.month === 0) {
                                    calendarPopup.month = 11
                                    calendarPopup.year -= 1
                                } else {
                                    calendarPopup.month -= 1
                                }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: Qt.locale().monthName(calendarPopup.month, Locale.LongFormat)
                                + " " + calendarPopup.year
                            color: Theme.colors.text
                            font.pixelSize: 12
                            font.bold: true
                        }
                        FlatButton {
                            iconName: "caret-right"
                            tooltip: qsTr("Next month")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                if (calendarPopup.month === 11) {
                                    calendarPopup.month = 0
                                    calendarPopup.year += 1
                                } else {
                                    calendarPopup.month += 1
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
                            font.pixelSize: 10
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                    MonthGrid {
                        id: monthGrid
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        month: calendarPopup.month
                        year: calendarPopup.year
                        locale: Qt.locale()
                        delegate: Rectangle {
                            implicitHeight: 22
                            implicitWidth: 28
                            readonly property bool _today: {
                                var t = new Date()
                                return model.date.getFullYear() === t.getFullYear()
                                    && model.date.getMonth() === t.getMonth()
                                    && model.date.getDate() === t.getDate()
                            }
                            readonly property bool _selected: {
                                var v = cell.value
                                if (typeof v !== "number" || v <= 0) return false
                                var d = new Date(v)
                                return model.date.getFullYear() === d.getFullYear()
                                    && model.date.getMonth() === d.getMonth()
                                    && model.date.getDate() === d.getDate()
                            }
                            color: _selected ? Theme.colors.accent
                                : (cellMa.containsMouse ? Theme.colors.surfaceHover : "transparent")
                            border.color: _today ? Theme.colors.accent : "transparent"
                            border.width: 1
                            radius: 3
                            Label {
                                anchors.centerIn: parent
                                text: model.day
                                color: model.month === calendarPopup.month
                                    ? (parent._selected ? Theme.colors.textBright : Theme.colors.text)
                                    : Theme.colors.textSubtle
                                font.pixelSize: 11
                            }
                            MouseArea {
                                id: cellMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    cell.commit(model.date.getTime())
                                    calendarPopup.close()
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        FlatButton {
                            text: qsTr("Today")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: {
                                var t = new Date()
                                t.setHours(0, 0, 0, 0)
                                cell.commit(t.getTime())
                                calendarPopup.close()
                            }
                        }
                        Item { Layout.fillWidth: true }
                        FlatButton {
                            text: qsTr("Clear")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            enabled_: typeof cell.value === "number" && cell.value > 0
                            onClicked: {
                                cell.commit(null)
                                calendarPopup.close()
                            }
                        }
                    }
                }
            }
        }
    }

    // ── notes ─────────────────────────────────────────────────────
    // Multi-line text. Cell shows truncated single-line preview;
    // click to open a popup TextArea below the cell. Save on
    // Ctrl+Enter or popup close; Esc cancels (reverts text).
    Component {
        id: notesComp
        Item {
            id: notesRoot
            anchors.fill: parent
            // Single-line preview — collapse newlines so the cell
            // stays tidy. Popup shows the full multi-line text.
            readonly property string previewText: {
                var v = cell.value
                if (v === undefined || v === null) return ""
                return String(v).replace(/\s*\n\s*/g, " ⏎ ")
            }
            Label {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                verticalAlignment: Text.AlignVCenter
                text: notesRoot.previewText
                color: cell.selected ? "#cfd9e6" : "#cccccc"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            MouseArea {
                anchors.fill: parent
                visible: !cell.readOnly
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    notesArea.text = cell.value === undefined || cell.value === null
                        ? "" : String(cell.value)
                    notesPopup.open()
                }
            }
            Popup {
                id: notesPopup
                x: 0
                y: notesRoot.height
                width: Math.max(280, notesRoot.width)
                height: 200
                padding: 6
                modal: false
                onClosed: {
                    var current = cell.value === undefined || cell.value === null
                        ? "" : String(cell.value)
                    if (notesArea.text !== current) {
                        cell.commit(notesArea.text)
                    }
                }
                background: Rectangle {
                    color: Theme.colors.toolbar
                    border.color: Theme.colors.borderStrong
                    border.width: 1
                    radius: 3
                }
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        TextArea {
                            id: notesArea
                            wrapMode: TextArea.Wrap
                            placeholderText: qsTr("Notes — Ctrl+Enter saves, Esc cancels")
                            font.pixelSize: 11
                            background: Rectangle {
                                color: Theme.colors.bg
                                border.color: Theme.colors.border
                                border.width: 1
                                radius: 2
                            }
                            Keys.onPressed: (event) => {
                                if (event.key === Qt.Key_Return
                                    && (event.modifiers & Qt.ControlModifier)) {
                                    notesPopup.close()
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Escape) {
                                    // Revert before close so the
                                    // diff check skips commit.
                                    notesArea.text = cell.value === undefined || cell.value === null
                                        ? "" : String(cell.value)
                                    notesPopup.close()
                                    event.accepted = true
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Item { Layout.fillWidth: true }
                        FlatButton {
                            iconName: "check"
                            text: qsTr("Save")
                            variant: "primary"
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: notesPopup.close()
                        }
                    }
                }
            }
        }
    }

    // ── links ─────────────────────────────────────────────────────
    // Stored as a string of URLs separated by newlines or commas.
    // Cell shows count + first URL preview; click opens a popup with
    // an editable list — clicking a parsed URL opens it in the
    // default browser via Qt.openUrlExternally.
    Component {
        id: linksComp
        Item {
            id: linksRoot
            anchors.fill: parent
            function _parseLinks(v) {
                if (v === undefined || v === null) return []
                return String(v).split(/[\n,]+/).map(function(s) {
                    return s.trim()
                }).filter(function(s) { return s.length > 0 })
            }
            readonly property var linkArr: _parseLinks(cell.value)
            Label {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                verticalAlignment: Text.AlignVCenter
                text: linksRoot.linkArr.length === 0
                    ? ""
                    : (linksRoot.linkArr.length === 1
                        ? "🔗 " + linksRoot.linkArr[0]
                        : qsTr("🔗 %1 links").arg(linksRoot.linkArr.length))
                color: cell.selected ? "#9cc9ff" : "#5b8ed1"
                font.pixelSize: 11
                font.underline: linksRoot.linkArr.length > 0
                elide: Text.ElideRight
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    linksArea.text = cell.value === undefined || cell.value === null
                        ? "" : String(cell.value)
                    linksPopup.open()
                }
            }
            Popup {
                id: linksPopup
                x: 0
                y: linksRoot.height
                width: Math.max(320, linksRoot.width)
                height: 240
                padding: 6
                modal: false
                onClosed: {
                    var current = cell.value === undefined || cell.value === null
                        ? "" : String(cell.value)
                    if (linksArea.text !== current) {
                        cell.commit(linksArea.text)
                    }
                }
                background: Rectangle {
                    color: Theme.colors.toolbar
                    border.color: Theme.colors.borderStrong
                    border.width: 1
                    radius: 3
                }
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    Label {
                        text: qsTr("Click any link to open. Edit below — one URL per line.")
                        color: Theme.colors.textMuted
                        font.pixelSize: 10
                        font.italic: true
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(80,
                            Math.max(20, linksRoot._parseLinks(linksArea.text).length * 22))
                        color: Theme.colors.bg
                        border.color: Theme.colors.border
                        border.width: 1
                        radius: 2
                        ListView {
                            anchors.fill: parent
                            anchors.margins: 2
                            clip: true
                            model: linksRoot._parseLinks(linksArea.text)
                            delegate: Item {
                                required property string modelData
                                width: ListView.view.width
                                height: 22
                                Label {
                                    anchors.fill: parent
                                    anchors.leftMargin: 6
                                    anchors.rightMargin: 6
                                    verticalAlignment: Text.AlignVCenter
                                    text: modelData
                                    color: openMa.containsMouse ? "#9cc9ff" : "#5b8ed1"
                                    font.pixelSize: 11
                                    font.underline: openMa.containsMouse
                                    elide: Text.ElideRight
                                }
                                MouseArea {
                                    id: openMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Qt.openUrlExternally(modelData)
                                }
                            }
                        }
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        TextArea {
                            id: linksArea
                            wrapMode: TextArea.Wrap
                            placeholderText: qsTr("https://example.com/…\nOne URL per line")
                            font.pixelSize: 11
                            background: Rectangle {
                                color: Theme.colors.bg
                                border.color: Theme.colors.border
                                border.width: 1
                                radius: 2
                            }
                            Keys.onPressed: (event) => {
                                if (event.key === Qt.Key_Escape) {
                                    linksArea.text = cell.value === undefined || cell.value === null
                                        ? "" : String(cell.value)
                                    linksPopup.close()
                                    event.accepted = true
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Item { Layout.fillWidth: true }
                        FlatButton {
                            iconName: "check"
                            text: qsTr("Save")
                            variant: "primary"
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            onClicked: linksPopup.close()
                        }
                    }
                }
            }
        }
    }
}
