// Audio-only preview pane for the lightbox (wav/mp3/aiff/flac/…).
// Drives the existing AudioPlayer standalone — no VideoDecoder. The
// player runs free (Phase 5.0.a semantics); a QML wall-clock playhead
// owns time and feeds AudioPlayer.update(), whose drift correction
// re-seeks the decoder whenever it strays >150 ms from us — the exact
// contract VideoPreview's video clock has with the same player.
// Transport function names mirror VideoPreview so PreviewLightbox's
// key routing (K/M/V, A·J/D·L shuttle, Home/End, Q/E) works unchanged.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ufb.Backend 1.0

Item {
    id: root

    // Absolute path of the audio file to play.
    property string source: ""

    // Wall-clock playhead in seconds — the master clock. Advanced by
    // the 33 ms tick below using real elapsed time (not a fixed step,
    // so timer jitter doesn't accumulate into drift).
    property real position: 0
    property real _duration: 0
    property double _lastTickMs: 0

    property bool loop: Settings.preview_loop()

    Component.onDestruction: { audio.close(); audio.shutdown() }

    onSourceChanged: {
        audio.close()
        root.position = 0
        root._duration = 0
        if (source.length === 0)
            return
        audio.initialize()
        if (audio.open(source)) {
            root._duration = audio.duration()
            root._play()
        }
    }

    function _play() {
        if (!audio.hasAudio) return
        // Restart from the top when play is hit at the end of the clip.
        if (root._duration > 0 && root.position >= root._duration - 0.05) {
            root.position = 0
            audio.seek(0)
        }
        root._lastTickMs = Date.now()
        audio.play()
    }

    function _seekTo(s) {
        s = Math.max(0, Math.min(root._duration > 0 ? root._duration : s, s))
        root.position = s
        audio.seek(s)
    }

    // ---- Transport API (the lightbox routes keys to these) ----------------

    function togglePlayback() {
        if (audio.isPlaying) audio.pause()
        else root._play()
    }

    // Q/E: fine step. Frames don't exist here; a second is the audio
    // equivalent of a review nudge.
    function stepFrames(n) { audio.pause(); _seekTo(root.position + n) }

    // A·J / D·L: the same accelerating shuttle as VideoPreview, directly
    // in seconds (2×→32×, doubling per held second).
    property int  _fastSeekDir: 0
    property real _fastSeekSpeed: 2.0
    property real _fastSeekElapsed: 0

    function startFastSeek(dir) {
        dir = dir > 0 ? 1 : -1
        if (_fastSeekDir === dir) return   // auto-repeat of the held key
        audio.pause()
        _fastSeekDir = dir
        _fastSeekSpeed = 2.0
        _fastSeekElapsed = 0
        fastSeekTimer.start()
    }
    function stopFastSeek() {
        _fastSeekDir = 0
        fastSeekTimer.stop()
    }

    function seekToStart() { audio.pause(); _seekTo(0) }
    function seekToEnd()   { audio.pause(); _seekTo(Math.max(0, root._duration - 0.05)) }

    function toggleMute()   { if (audio.hasAudio) audio.setMuted(!audio.muted) }
    function nudgeVolume(d) {
        if (!audio.hasAudio) return
        audio.setVolume(Math.max(0, Math.min(1, audio.volume + d)))
        if (audio.volume > 0) audio.setMuted(false)
    }
    function toggleLoop() {
        loop = !loop
        Settings.set_preview_loop(loop)
    }

    AudioPlayer { id: audio }

    // Playhead tick: advance by real elapsed wall time, hand the position
    // to the player's drift correction, and handle end-of-clip.
    Timer {
        interval: 33
        repeat: true
        running: audio.isPlaying
        onRunningChanged: if (running) root._lastTickMs = Date.now()
        onTriggered: {
            var now = Date.now()
            root.position += (now - root._lastTickMs) / 1000
            root._lastTickMs = now
            if (root._duration > 0 && root.position >= root._duration) {
                if (root.loop) {
                    root.position = 0
                    audio.seek(0)
                } else {
                    root.position = root._duration
                    audio.pause()
                }
            } else {
                audio.update(root.position)
            }
        }
    }

    Timer {
        id: fastSeekTimer
        interval: 33
        repeat: true
        running: false
        onTriggered: {
            if (root._fastSeekDir === 0) return
            var dt = interval / 1000
            root._fastSeekElapsed += dt
            root._fastSeekSpeed = Math.min(32.0, 2.0 * Math.pow(2.0, root._fastSeekElapsed))
            root._seekTo(root.position + dt * root._fastSeekSpeed * root._fastSeekDir)
        }
    }

    // ---- Visuals: file-type icon + big time readout over the scrim --------

    Column {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -transport.height / 2
        spacing: Theme.dim.spacingLoose

        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 128; height: 128
            sourceSize: Qt.size(256, 256)
            source: {
                var i = root.source.lastIndexOf(".")
                var ext = i >= 0 ? root.source.substring(i + 1).toLowerCase() : "file"
                return "image://ufb-icons/" + ext
            }
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            color: Theme.colors.text
            font.family: Theme.font.mono
            font.pixelSize: Theme.font.sizeBody * 2
            text: root._fmt(root.position) + "  /  " + root._fmt(root._duration)
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !audio.hasAudio && root.source.length > 0
            color: Theme.colors.textMuted
            font.family: Theme.font.family
            font.pixelSize: Theme.font.sizeBody
            text: qsTr("No playable audio stream")
        }
    }

    function _fmt(s) {
        if (!(s > 0)) s = 0
        var m = Math.floor(s / 60)
        var sec = Math.floor(s % 60)
        var h = Math.floor(m / 60)
        m = m % 60
        var mm = (m < 10 ? "0" : "") + m
        var ss = (sec < 10 ? "0" : "") + sec
        return h > 0 ? h + ":" + mm + ":" + ss : mm + ":" + ss
    }

    // ---- Transport bar (mirrors VideoPreview's layout, minus frame steps) --

    Rectangle {
        id: transport
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.dim.toolStripHeight + 14
        color: Theme.colors.toolbar

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Theme.dim.divider
            color: Theme.colors.divider
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.dim.padding
            anchors.rightMargin: Theme.dim.padding
            spacing: Theme.dim.spacingTight

            FlatButton {
                iconName: "skip-back"
                tooltip: qsTr("Jump to start (Home)")
                onClicked: root.seekToStart()
            }
            FlatButton {
                iconName: "rewind"
                tooltip: qsTr("Rewind — hold (A / J)")
                onPressed:  root.startFastSeek(-1)
                onReleased: root.stopFastSeek()
            }
            FlatButton {
                iconName: audio.isPlaying ? "pause" : "play"
                tooltip: qsTr("Play / Pause (K)")
                onClicked: root.togglePlayback()
            }
            FlatButton {
                iconName: "fast-forward"
                tooltip: qsTr("Fast-forward — hold (D / L)")
                onPressed:  root.startFastSeek(1)
                onReleased: root.stopFastSeek()
            }
            FlatButton {
                iconName: "skip-forward"
                tooltip: qsTr("Jump to end (End)")
                onClicked: root.seekToEnd()
            }

            FlatSlider {
                id: seek
                Layout.fillWidth: true
                Layout.leftMargin: Theme.dim.spacingLoose
                Layout.rightMargin: Theme.dim.spacingLoose
                from: 0
                to: Math.max(0.001, root._duration)

                property bool _wasPlaying: false
                onPressedChanged: {
                    if (pressed) {
                        _wasPlaying = audio.isPlaying
                        audio.pause()
                    } else if (_wasPlaying) {
                        root._play()
                    }
                }
                onMoved: root._seekTo(value)

                Binding {
                    target: seek
                    property: "value"
                    value: root.position
                    when: !seek.pressed
                }
            }

            Label {
                color: Theme.colors.textMuted
                font.family: Theme.font.mono
                font.pixelSize: Theme.font.sizeSmall
                text: root._fmt(root.position) + " / " + root._fmt(root._duration)
            }

            FlatButton {
                Layout.leftMargin: Theme.dim.spacing * 2
                iconName: "repeat"
                checked: root.loop
                tooltip: qsTr("Loop (V)")
                onClicked: root.toggleLoop()
            }

            FlatButton {
                iconName: (audio.muted || audio.volume <= 0) ? "speaker-x"
                                                             : "speaker-high"
                tooltip: qsTr("Mute (M)")
                onClicked: root.toggleMute()
            }
            FlatSlider {
                visible: audio.hasAudio
                Layout.preferredWidth: 84
                Layout.leftMargin: Theme.dim.spacing
                from: 0; to: 1
                value: audio.volume
                onMoved: { audio.setVolume(value); if (value > 0) audio.setMuted(false) }
            }
        }
    }
}
