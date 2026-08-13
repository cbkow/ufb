// In-app QuickLook-style preview overlay (spacebar). Routes the current
// file to a per-type renderer; video plays via VideoPreview (zero-copy on
// macOS). Images / PDF / text renderers + thumb-icon fallback land in M2;
// for now non-video shows the thumbnail as a stand-in.
// See devdocs/lightbox-preview-plan.md.

import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: root
    visible: false
    z: 100000

    property string currentPath: ""

    // Lowercased extension of the current item.
    readonly property string _ext: {
        var i = currentPath.lastIndexOf(".")
        return i >= 0 ? currentPath.substring(i + 1).toLowerCase() : ""
    }
    // dpx/cin are Thumbnailer videoExts (single-frame ffmpeg stills) but
    // deliberately NOT here — they preview as stills, not in the player.
    readonly property var _videoExts: ["mp4", "m4v", "mov", "qt", "mkv",
        "webm", "ogv", "avi", "wmv", "mpg", "mpeg", "ts", "m2ts", "mts",
        "mxf", "flv", "3gp", "3g2"]
    readonly property var _audioExts: ["wav", "wave", "bwf", "mp3", "aiff",
        "aif", "aifc", "flac", "m4a", "aac", "ogg", "oga", "opus", "wma",
        "caf"]
    readonly property bool _isVideo: _videoExts.indexOf(_ext) >= 0
    readonly property bool _isAudio: _audioExts.indexOf(_ext) >= 0
    readonly property bool _isPdf: _ext === "pdf" || _ext === "ai"
    // Rendered HTML needs the optional WebEngine module; without it,
    // html/htm fall through to _isText → TextPreview's rich-text
    // approximation (TextInfo.isText also returns true for them).
    readonly property bool _isHtml: (_ext === "html" || _ext === "htm")
        && typeof _webEngineAvailable !== "undefined" && _webEngineAvailable
    // Text routing lives in C++ (TextInfo.isText): known text/code exts,
    // plus a bounded content sniff so extensionless files (README,
    // .gitignore, renamed logs) preview instead of icon-ing. The guards
    // short-circuit so media paths never pay the sniff's 4 KB read.
    readonly property bool _isText: !_isVideo && !_isAudio && !_isPdf
        && currentPath.length > 0 && TextInfo.isText(currentPath)

    function open(path) {
        root.currentPath = path
        root.visible = true
        root.forceActiveFocus()
    }
    function close() {
        root.visible = false
        root.currentPath = ""
        // Return keyboard focus to the originating browser pane so the
        // still-selected item stays actionable (Space re-opens the preview).
        if (Window.window && Window.window.previewClosed)
            Window.window.previewClosed()
    }

    focus: visible
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Escape) {
            // EXR layer grid: Esc first steps back from a drilled-in layer
            // to the grid; a second Esc (nothing to step back to) closes.
            if (content.item && content.item.canGoBack === true
                && content.item.backToGrid()) {
                // consumed — returned to the layer grid
            } else {
                root.close()
            }
            event.accepted = true
        } else if (event.key === Qt.Key_Backspace) {
            // Backspace is back-to-grid only (never closes the lightbox).
            if (content.item && content.item.canGoBack === true)
                content.item.backToGrid()
            event.accepted = true
        } else if (event.key === Qt.Key_Space) {
            // Space is the open/close toggle for the lightbox on both
            // platforms (Space opens it from the file browser, Space
            // closes it here) — including for video. Play/pause lives on
            // K / W / S instead, so Space no longer double-duties.
            root.close()
            event.accepted = true
        } else if (event.key === Qt.Key_Left) {
            if (Window.window && Window.window.previewStep)
                Window.window.previewStep(-1)
            event.accepted = true
        } else if (event.key === Qt.Key_Right) {
            if (Window.window && Window.window.previewStep)
                Window.window.previewStep(1)
            event.accepted = true
        } else if ((root._isVideo || root._isAudio)
                   && content.item && content.item.togglePlayback) {
            // Video/audio transport (ported from QCView). Q/E step a frame; A·J /
            // D·L relative-seek (auto-repeat = continuous); K play/pause; M
            // mute; Up/Down volume; V loop; Home/End jump to ends.
            var v = content.item
            if (event.key === Qt.Key_Q)         { v.stepFrames(-1); event.accepted = true }
            else if (event.key === Qt.Key_E)    { v.stepFrames(1);  event.accepted = true }
            // A·J rewind / D·L fast-forward: start the accelerating shuttle;
            // released in Keys.onReleased. startFastSeek no-ops on auto-repeat.
            else if (event.key === Qt.Key_A || event.key === Qt.Key_J) { v.startFastSeek(-1); event.accepted = true }
            else if (event.key === Qt.Key_D || event.key === Qt.Key_L) { v.startFastSeek(1);  event.accepted = true }
            else if (event.key === Qt.Key_K || event.key === Qt.Key_W || event.key === Qt.Key_S) { v.togglePlayback(); event.accepted = true }
            else if (event.key === Qt.Key_M)    { v.toggleMute();     event.accepted = true }
            else if (event.key === Qt.Key_V)    { v.toggleLoop();     event.accepted = true }
            else if (event.key === Qt.Key_Up)   { v.nudgeVolume(0.05);  event.accepted = true }
            else if (event.key === Qt.Key_Down) { v.nudgeVolume(-0.05); event.accepted = true }
            else if (event.key === Qt.Key_Home) { v.seekToStart(); event.accepted = true }
            else if (event.key === Qt.Key_End)  { v.seekToEnd();   event.accepted = true }
        } else if (content.item && content.item.scrollStep) {
            // Scrollable content (PDF reader / text): route the vertical
            // navigation keys. Wheel/trackpad scroll works natively too.
            if (event.key === Qt.Key_Down)      { content.item.scrollStep(80);  event.accepted = true }
            else if (event.key === Qt.Key_Up)   { content.item.scrollStep(-80); event.accepted = true }
            else if (event.key === Qt.Key_PageDown) { content.item.scrollPage(1);  event.accepted = true }
            else if (event.key === Qt.Key_PageUp)   { content.item.scrollPage(-1); event.accepted = true }
            else if (event.key === Qt.Key_Home) { content.item.scrollHome(); event.accepted = true }
            else if (event.key === Qt.Key_End)  { content.item.scrollEnd();  event.accepted = true }
        }
    }

    // Stop the fast-seek shuttle when A/D/J/L is released. Ignore auto-repeat
    // releases (those are part of the hold, not the real key-up).
    Keys.onReleased: (event) => {
        if (event.isAutoRepeat) return
        if ((root._isVideo || root._isAudio)
            && content.item && content.item.stopFastSeek
            && (event.key === Qt.Key_A || event.key === Qt.Key_J
                || event.key === Qt.Key_D || event.key === Qt.Key_L)) {
            content.item.stopFastSeek()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        // Scrim tied to the app background tone (sister-app palette), nearly
        // opaque so the preview reads as a modal layer.
        color: Qt.rgba(Theme.colors.bg.r, Theme.colors.bg.g,
                       Theme.colors.bg.b, 0.94)
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: {} // swallow
        }
    }

    // Per-type content, inset from the edges. Built ASYNCHRONOUSLY: the
    // content tree (VideoPreview's surface + decoder + transport, etc.) is
    // non-trivial to instantiate, and doing it synchronously delays the
    // overlay's first paint — so Space feels like it did nothing until the
    // whole thing is ready. Async incubation lets the scrim, filename, and
    // close button pop instantly while the content fills in a beat later.
    Loader {
        id: content
        anchors.fill: parent
        anchors.margins: 40
        anchors.bottomMargin: 64
        active: root.visible && root.currentPath.length > 0
        asynchronous: true
        sourceComponent: root._isVideo ? videoComp
                         : root._isAudio ? audioComp
                         : root._isPdf  ? pdfComp
                         : root._isHtml ? htmlComp
                         : root._isText ? textComp
                                        : stillComp
    }

    // Spinner covering the gap while the content tree incubates (async
    // Loader above). Delayed so quick loads don't flash it; the per-type
    // renderers show their own decode spinner once they exist.
    BusyIndicator {
        anchors.centerIn: parent
        running: content.active && content.status === Loader.Loading
                 && _loadSpinDelay.fired
        visible: running
        Timer {
            id: _loadSpinDelay
            property bool fired: false
            interval: 150
            running: content.active && content.status === Loader.Loading
            onTriggered: fired = true
            onRunningChanged: if (!running) fired = false
        }
    }

    Component {
        id: videoComp
        VideoPreview { source: root.currentPath }
    }

    Component {
        id: audioComp
        AudioPreview { source: root.currentPath }
    }

    Component {
        id: htmlComp
        HtmlPreview { source: root.currentPath }
    }

    Component {
        id: pdfComp
        PdfReader { source: root.currentPath }
    }

    Component {
        id: textComp
        TextPreview { source: root.currentPath }
    }

    // Stills: full-res image (EXR/PSD/TIFF/JPEG/PNG…) via ImagePreview, which
    // falls back to the OS file icon for types it can't decode. PDF/text get
    // dedicated renderers in M2b; for now they also route here (PDF shows
    // page 0 full-res via the backend).
    Component {
        id: stillComp
        ImagePreview { source: root.currentPath }
    }

    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 22
        width: parent.width - 200
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideMiddle
        color: Theme.colors.textMuted
        font.family: Theme.font.family
        font.pixelSize: Theme.font.sizeBody
        text: root.currentPath
        visible: !root._isVideo
    }

    FlatButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.dim.padding
        iconName: "x"
        iconSize: Theme.icon.sizeMedium
        tooltip: qsTr("Close (Esc)")
        onClicked: root.close()
    }

    // Back-to-grid affordance for a drilled-in EXR layer. Lives on the
    // top row (opposite the close ✕) rather than inside ImagePreview, so
    // it floats above the image instead of under it.
    FlatButton {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: Theme.dim.padding
        visible: content.item && content.item.canGoBack === true
        iconName: "squares-four"
        text: qsTr("Layers")
        tooltip: qsTr("Back to layer grid (Esc)")
        onClicked: if (content.item) content.item.backToGrid()
    }
}
