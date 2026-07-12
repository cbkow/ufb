// Full-resolution still preview for the lightbox.
//
// Two states for EXRs with more than one layer:
//   * Layer grid  — a contact sheet of every layer thumbnail (filling the
//                   frame); click one to drill into it.
//   * Full-frame  — the selected layer (or, for non-EXR / single-layer
//                   EXRs, the image itself) fit-to-window.
// Back button (top-left) + Esc step back from full-frame to the grid;
// the host (PreviewLightbox) routes Esc/Backspace here via backToGrid().
//
// Layer pixels come from image://ufb-exr-layer/<layer>/<path>; the default
// view uses image://ufb-preview/<path>. Layer names: ExrInfo.layers().
// See devdocs/lightbox-preview-plan.md.

import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: root

    // Absolute path of the still to show.
    property string source: ""

    readonly property string _ext: {
        var i = source.lastIndexOf(".")
        return i >= 0 ? source.substring(i + 1).toLowerCase() : ""
    }

    // ── EXR layer state ───────────────────────────────────────────────
    property var _layers: []
    readonly property bool _hasLayers: _layers.length > 1
    property bool _gridMode: false           // showing the contact sheet
    property string _selectedLayer: ""        // drilled-into layer ("" = default)
    // True when the full-frame view can step back to the layer grid.
    readonly property bool canGoBack: _hasLayers && !_gridMode

    onSourceChanged: _refreshLayers()
    Component.onCompleted: _refreshLayers()

    function _refreshLayers() {
        _selectedLayer = ""
        _layers = (_ext === "exr" && source.length > 0)
            ? ExrInfo.layers(source) : []
        // Entry: grid only if >1 layer; otherwise straight to full-frame.
        _gridMode = _layers.length > 1
    }

    // Step back from a drilled-in layer to the grid. Returns true if it
    // consumed the action (host uses this to decide Esc = back vs close).
    function backToGrid() {
        if (_hasLayers && !_gridMode) {
            _selectedLayer = ""
            _gridMode = true
            return true
        }
        return false
    }

    function _fullSource() {
        if (source.length === 0) return ""
        if (_selectedLayer.length > 0)
            return "image://ufb-exr-layer/"
                 + encodeURIComponent(_selectedLayer) + "/"
                 + encodeURIComponent(source)
        return "image://ufb-preview/" + source
    }

    // ── Full-frame view ───────────────────────────────────────────────
    Image {
        id: img
        anchors.fill: parent
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: false   // one image alive at a time; don't grow QML's pixmap cache
        smooth: true
        source: root._gridMode ? "" : root._fullSource()
        // Decode at device pixels for crispness on HiDPI; bounded by the
        // on-screen view size so we don't over-decode.
        sourceSize.width: Math.round(root.width * Screen.devicePixelRatio)
        sourceSize.height: Math.round(root.height * Screen.devicePixelRatio)
        visible: !root._gridMode && status === Image.Ready
    }

    // Spinner while decoding. Delayed slightly so fast loads don't flash it.
    BusyIndicator {
        anchors.centerIn: parent
        running: !root._gridMode && img.status === Image.Loading && _spinDelay.fired
        visible: running
        Timer {
            id: _spinDelay
            property bool fired: false
            interval: 150
            running: img.status === Image.Loading
            onTriggered: fired = true
            onRunningChanged: if (!running) fired = false
        }
    }

    // Decode failed (provider returned null → Error): show the OS file-type
    // icon so the view never goes empty (the universal fallback).
    FileTypeIcon {
        anchors.centerIn: parent
        width: Math.min(parent.width, 160)
        height: width
        extension: root._ext
        visible: !root._gridMode
                 && (img.status === Image.Error || root.source.length === 0)
    }

    // ── Layer grid (contact sheet) ────────────────────────────────────
    GridView {
        id: layerGrid
        anchors.fill: parent
        anchors.margins: 12
        visible: root._gridMode
        clip: true
        model: root._gridMode ? root._layers : []
        cellWidth: 208
        cellHeight: 168
        // Center the rows of cells horizontally within the frame.
        readonly property int _cols: Math.max(1, Math.floor(width / cellWidth))
        leftMargin: Math.max(0, (width - _cols * cellWidth) / 2)

        delegate: Item {
            id: cell
            required property var modelData
            required property int index
            width: layerGrid.cellWidth
            height: layerGrid.cellHeight

            Rectangle {
                anchors.fill: parent
                anchors.margins: 6
                radius: 4
                color: "#0b0b0b"
                border.color: cellHover.containsMouse
                    ? Theme.colors.accent : Theme.colors.divider
                border.width: 1

                Column {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 4

                    Image {
                        id: thumb
                        width: parent.width
                        height: parent.height - nameLabel.height - parent.spacing
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                        smooth: true
                        source: "image://ufb-exr-layer/"
                              + encodeURIComponent(cell.modelData) + "/"
                              + encodeURIComponent(root.source)
                        sourceSize.width: Math.round(
                            layerGrid.cellWidth * Screen.devicePixelRatio)
                        sourceSize.height: Math.round(
                            layerGrid.cellHeight * Screen.devicePixelRatio)
                        BusyIndicator {
                            anchors.centerIn: parent
                            running: thumb.status === Image.Loading
                            visible: running
                            implicitWidth: 28; implicitHeight: 28
                        }
                    }
                    Label {
                        id: nameLabel
                        width: parent.width
                        text: cell.modelData
                        color: Theme.colors.text
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        horizontalAlignment: Text.AlignHCenter
                    }
                }

                MouseArea {
                    id: cellHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root._selectedLayer = cell.modelData
                        root._gridMode = false
                    }
                }
            }
        }
    }

    // The back-to-grid affordance lives in PreviewLightbox's top row
    // (next to the close ✕), driven by canGoBack / backToGrid() above.
}
