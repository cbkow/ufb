// Video preview pane for the lightbox: a VideoDecoder driving a
// VideoSurfaceItem (zero-copy on macOS), an AudioPlayer kept in sync, and a
// play/pause + seekbar + mute/volume transport. Video is the master clock;
// audio follows via AudioPlayer.update(videoSeconds) (drift-corrected).
// See devdocs/lightbox-preview-plan.md.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ufb.Backend 1.0

Item {
    id: root

    // Absolute path of the clip to play.
    property string source: ""

    readonly property real _videoSeconds: dec.fps > 0 ? dec.currentFrame / dec.fps : 0

    Component.onDestruction: { dec.close(); audio.close(); audio.shutdown() }

    // Beat between opening a clip and starting playback. Large clips —
    // especially over the network — need a moment to open + decode the
    // first frames; starting the PTS pacer the instant open() returns makes
    // playback lurch to "catch up." Deferring autoplay lets the pipeline
    // fill so playback (and audio) start cleanly on the first frame.
    readonly property int _autoplayDelayMs: 400

    // Loading feedback. True from the moment a clip is requested until its
    // first frame paints (or open fails), so a slow-opening clip still shows
    // the lightbox registered the request and is working on it.
    property bool _loading: false
    property bool _firstFrameSeen: false

    onSourceChanged: {
        autoplayTimer.stop()
        if (source.length > 0) {
            // Paint the overlay + spinner first, THEN open. dec.open()
            // probes the file synchronously on the GUI thread (avformat
            // stream-info), which takes a beat on large / networked clips;
            // doing it inline blocks the lightbox's first paint, so the user
            // sees nothing and thinks Space didn't register. Defer it one
            // tick so the scrim, filename, and loading spinner show first.
            root._firstFrameSeen = false
            root._loading = true
            Qt.callLater(root._openClip)
        } else {
            dec.close()
            audio.close()
            root._loading = false
        }
    }

    // Deferred open (see onSourceChanged). Qt.callLater coalesces repeated
    // calls, so rapid Left/Right nav only opens the final selection.
    function _openClip() {
        if (source.length === 0)
            return
        // initialize() is idempotent; call it here (not in
        // Component.onCompleted) so the device exists before open(),
        // since the open can fire during construction.
        var opened = dec.open(source)
        audio.initialize()
        audio.open(source)
        if (opened)
            autoplayTimer.restart()
        else
            root._loading = false   // open failed → stop the spinner
    }

    // Fires once per opened clip; starts video + audio together so they
    // stay in sync. Cancelled/restarted on each source change and on close.
    Timer {
        id: autoplayTimer
        interval: root._autoplayDelayMs
        repeat: false
        onTriggered: {
            if (root.source.length === 0)
                return
            dec.play()
            if (audio.hasAudio)
                audio.play()
        }
    }

    // Loop toggle (V). When on, EndOfStream restarts from frame 0.
    // Persisted across previews + launches via Settings.preview_loop —
    // seeded here so every newly-opened clip inherits the last choice.
    property bool loop: Settings.preview_loop()

    // ---- Transport API (the lightbox routes keys to these) ----------------
    //
    // Frame navigation (step / fast-seek / Home / End) drives the GOP-cached
    // ScrubDecoder via scrubToFrame — instant on ProRes (intra-direct) and
    // smooth-within-GOP on inter codecs, exactly like dragging the seekbar.
    // scrubToFrame only updates the displayed frame; the *streaming* decoder
    // stays where it was, so we track the intended frame in _scrubTarget and
    // reposition the streaming decoder lazily, the moment playback resumes
    // (the keyboard analogue of the seekbar's reposition-on-release).

    // -1 = not scrubbing (streaming decoder is at the live playhead). Otherwise
    // the frame the user has scrubbed to but not yet resumed playback from.
    property int _scrubTarget: -1
    function _intendedFrame() { return _scrubTarget >= 0 ? _scrubTarget : dec.currentFrame }

    function _scrubTo(f) {
        f = Math.max(0, Math.min(dec.frameCount - 1, f))
        _scrubTarget = f
        dec.scrubToFrame(f)
        if (dec.fps > 0) audio.seek(f / dec.fps)
    }

    // Before resuming play, sync the streaming decoder to where we scrubbed.
    function _syncStreamingForResume() {
        if (_scrubTarget < 0) return
        dec.seekToFrame(_scrubTarget)
        if (dec.fps > 0) audio.seek(_scrubTarget / dec.fps)
        _scrubTarget = -1
    }

    // Space / K / W / S: play/pause.
    function togglePlayback() {
        if (dec.isPlaying) {
            dec.pause(); audio.pause()
        } else {
            _syncStreamingForResume()
            dec.play()
            if (audio.hasAudio) audio.play()
        }
    }

    // Q/E: frame-accurate step. Stepping implies review, so pause first.
    function stepFrames(n) {
        dec.pause(); audio.pause()
        _scrubTo(_intendedFrame() + n)
    }

    // A·J / D·L: accelerating fast-seek shuttle (ported from QCView's
    // start/stopFastSeek). A 33 ms timer advances a position at a geometric
    // 2×→32× rate (doubles per second) and scrubs to it every tick — smooth
    // continuous motion, unlike OS key-repeat's irregular discrete jumps.
    // Held key → start; release → stop (wired in PreviewLightbox).
    property int  _fastSeekDir: 0      // -1 rewind, +1 forward, 0 idle
    property real _fastSeekSpeed: 2.0  // current multiplier
    property real _fastSeekElapsed: 0  // seconds since gesture start (ramp)
    property real _fastSeekPos: 0      // current position, seconds

    function startFastSeek(dir) {
        dir = dir > 0 ? 1 : -1
        if (_fastSeekDir === dir) return   // already shuttling this way (repeat)
        dec.pause(); audio.pause()
        _fastSeekDir = dir
        _fastSeekSpeed = 2.0
        _fastSeekElapsed = 0
        _fastSeekPos = (dec.fps > 0) ? _intendedFrame() / dec.fps : 0
        fastSeekTimer.start()
    }
    function stopFastSeek() {
        // Leave _scrubTarget at the last scrubbed frame so resuming play
        // repositions the streaming decoder there.
        _fastSeekDir = 0
        fastSeekTimer.stop()
    }
    function _fastSeekTick() {
        if (_fastSeekDir === 0 || dec.fps <= 0 || dec.frameCount <= 0) return
        var dt = fastSeekTimer.interval / 1000
        _fastSeekElapsed += dt
        _fastSeekSpeed = Math.min(32.0, 2.0 * Math.pow(2.0, _fastSeekElapsed))
        _fastSeekPos += dt * _fastSeekSpeed * _fastSeekDir
        var maxSec = (dec.frameCount - 1) / dec.fps
        if (_fastSeekPos < 0)      _fastSeekPos = 0
        if (_fastSeekPos > maxSec) _fastSeekPos = maxSec
        _scrubTo(Math.round(_fastSeekPos * dec.fps))
    }

    // Home / End.
    function seekToStart() { dec.pause(); audio.pause(); _scrubTo(0) }
    function seekToEnd()   { dec.pause(); audio.pause(); _scrubTo(dec.frameCount - 1) }

    // M: mute toggle. Up/Down: volume nudge.
    function toggleMute()    { if (audio.hasAudio) audio.setMuted(!audio.muted) }
    function nudgeVolume(d)  {
        if (!audio.hasAudio) return
        audio.setVolume(Math.max(0, Math.min(1, audio.volume + d)))
        if (audio.volume > 0) audio.setMuted(false)
    }

    // V: loop toggle. Persist the choice so it sticks for the next clip
    // and across launches.
    function toggleLoop() {
        loop = !loop
        Settings.set_preview_loop(loop)
    }

    VideoDecoder { id: dec }
    AudioPlayer  { id: audio }

    // Loop: when the stream ends, restart from the top if loop is enabled.
    // Also clears the loading spinner: once the first frame paints (or the
    // open errors out) there's something on screen, so feedback can stop.
    Connections {
        target: dec
        function onStateChanged() {
            if (root.loop && dec.state === VideoDecoder.EndOfStream) {
                root._scrubTarget = -1
                dec.seekToFrame(0)
                audio.seek(0)
                dec.play()
                if (audio.hasAudio) audio.play()
            }
            if (dec.state === VideoDecoder.Errored)
                root._loading = false
        }
        function onFrameAvailable() {
            root._firstFrameSeen = true
            root._loading = false
        }
    }

    // Keep audio aligned to the video playhead while playing (drift-corrected
    // inside AudioPlayer::update). ~30 Hz is plenty for the correction loop.
    Timer {
        interval: 33
        repeat: true
        running: dec.isPlaying && audio.hasAudio
        onTriggered: audio.update(root._videoSeconds)
    }

    // Drives the accelerating fast-seek shuttle (A·J / D·L held).
    Timer {
        id: fastSeekTimer
        interval: 33
        repeat: true
        running: false
        onTriggered: root._fastSeekTick()
    }

    VideoSurfaceItem {
        id: surface
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: transport.top
        videoDecoder: dec
        // Match the lightbox scrim tone so letterbox bars and the
        // composited backdrop of alpha clips (ProRes 4444) blend
        // seamlessly into the surrounding panel.
        fillColor: Qt.rgba(Theme.colors.bg.r, Theme.colors.bg.g,
                           Theme.colors.bg.b, 1.0)
    }

    // Spinner while a clip opens / decodes its first frame. Delayed slightly
    // so quick local loads don't flash it (mirrors ImagePreview).
    BusyIndicator {
        anchors.centerIn: surface
        running: root._loading && _vidSpinDelay.fired
        visible: running
        Timer {
            id: _vidSpinDelay
            property bool fired: false
            interval: 150
            running: root._loading
            onTriggered: fired = true
            onRunningChanged: if (!running) fired = false
        }
    }

    Rectangle {
        id: transport
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.dim.toolStripHeight + 14
        color: Theme.colors.toolbar

        // Hairline separating the bar from the video, sister-app style.
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

            // ── Transport cluster (mirrors QCView's order) ──────────────
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
                iconName: "caret-left"
                tooltip: qsTr("Step back (Q)")
                onClicked: root.stepFrames(-1)
            }
            FlatButton {
                iconName: dec.isPlaying ? "pause" : "play"
                tooltip: qsTr("Play / Pause (Space)")
                onClicked: root.togglePlayback()
            }
            FlatButton {
                iconName: "caret-right"
                tooltip: qsTr("Step forward (E)")
                onClicked: root.stepFrames(1)
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
                to: Math.max(1, dec.frameCount - 1)

                // Pause while scrubbing so each seek lands and holds.
                property bool _wasPlaying: false
                onPressedChanged: {
                    if (pressed) {
                        _wasPlaying = dec.isPlaying
                        dec.pause()
                        audio.pause()
                    } else if (_wasPlaying) {
                        // Resume: _syncStreamingForResume repositions the
                        // streaming decoder to the scrubbed frame, then play.
                        root._syncStreamingForResume()
                        dec.play()
                        if (audio.hasAudio) audio.play()
                    }
                }
                // Live drag → instant GOP-cached scrub frames (shared with the
                // keyboard transport: _scrubTo updates the display + records
                // _scrubTarget so a later resume repositions the streaming
                // decoder to here).
                onMoved: root._scrubTo(Math.round(value))

                Connections {
                    target: dec
                    function onCurrentFrameChanged() {
                        if (!seek.pressed)
                            seek.value = dec.currentFrame
                    }
                }
            }

            Label {
                color: Theme.colors.textMuted
                font.family: Theme.font.mono
                font.pixelSize: Theme.font.sizeSmall
                text: dec.currentFrame + " / " + Math.max(0, dec.frameCount - 1)
            }

            // Loop toggle (also bound to V). Checked → accent fill.
            FlatButton {
                Layout.leftMargin: Theme.dim.spacing * 2
                iconName: "repeat"
                checked: root.loop
                tooltip: qsTr("Loop (V)")
                onClicked: root.toggleLoop()
            }

            // Mute + volume — only when the clip has an audio track.
            FlatButton {
                visible: audio.hasAudio
                iconName: (audio.muted || audio.volume <= 0) ? "speaker-x"
                                                             : "speaker-high"
                tooltip: qsTr("Mute (M)")
                onClicked: root.toggleMute()
            }
            FlatSlider {
                id: vol
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
