// TranscodeQueue — top-level workspace tab listing every active /
// queued / completed transcode job. Mirrors Subscription.queue_json
// onto a ListModel; per-row delegate shows progress + status pill +
// action buttons (Cancel / Remove / Reveal output).
//
// Sourcing: Transcode QObject is a singleton with `queue_json`
// updated on every job_updated / progress event from the Rust
// worker. We poll a Timer in addition because the events forwarder
// only lights up after the user adds the first job.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Rectangle {
    id: root

    color: Theme.colors.bg

    /// Emitted when the user clicks "Open in Browser" on a completed
    /// row. Main routes this to navigateActivePane on the most-recent
    /// Files tab so the output file lands in UFB's own FileBrowser
    /// with the file pre-selected.
    signal openInBrowserRequested(string path)

    /// Live mirror of Transcode.queue_json into a ListModel so the
    /// ListView delegate can use proper required properties + per-row
    /// dataChanged-style updates as queue_json re-emits.
    ListModel { id: queueModel }

    function refresh() {
        queueModel.clear()
        if (!Transcode.queue_json) return
        try {
            var arr = JSON.parse(Transcode.queue_json)
            for (var i = 0; i < arr.length; ++i) {
                var j = arr[i] || {}
                // `jobId` not `id` — `id` is reserved as QML's
                // element identifier, and a delegate's `required
                // property string id` silently conflicts with the
                // delegate's own `id: jobRow`, leaving rows unrendered.
                queueModel.append({
                    jobId: String(j.id || ""),
                    inputPath: String(j.inputPath || ""),
                    outputPath: String(j.outputPath || ""),
                    status: String(j.status || "Queued"),
                    progress: typeof j.progress === "number" ? j.progress : 0,
                    currentFrame: typeof j.currentFrame === "number" ? j.currentFrame : 0,
                    totalFrames: typeof j.totalFrames === "number" ? j.totalFrames : 0,
                    fps: typeof j.fps === "number" ? j.fps : 0,
                    error: String(j.error || "")
                })
            }
        } catch (e) {
            console.warn("TranscodeQueue: parse failed:", e)
        }
    }

    function _basename(p) {
        if (!p) return ""
        var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
        return idx >= 0 ? p.substring(idx + 1) : p
    }

    function _statusColor(s) {
        // Theme tokens for the status pill backgrounds.
        switch (s) {
        case "queued":
        case "Queued":          return Theme.colors.textMuted
        case "processing":
        case "Processing":      return Theme.colors.accent
        case "copyingMetadata":
        case "CopyingMetadata": return Theme.colors.success
        case "completed":
        case "Completed":       return Theme.colors.success
        case "failed":
        case "Failed":          return Theme.colors.error
        case "cancelled":
        case "Cancelled":       return Theme.colors.textMuted
        }
        return Theme.colors.textMuted
    }

    function _isFinal(s) {
        return s === "completed" || s === "Completed"
            || s === "failed" || s === "Failed"
            || s === "cancelled" || s === "Cancelled"
    }

    Component.onCompleted: {
        Transcode.refresh_queue()
        refresh()
    }
    Connections {
        target: Transcode
        function onQueue_jsonChanged() { root.refresh() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header strip ─────────────────────────────────────────────
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
                    name: "film-strip"
                    size: Theme.icon.sizeToolbar
                    color: Theme.colors.textMuted
                    Layout.rightMargin: 6
                    Layout.alignment: Qt.AlignVCenter
                }
                Label {
                    text: qsTr("Transcode Queue")
                    color: Theme.colors.textBright
                    font.pixelSize: Theme.font.sizeHeading
                    font.bold: true
                    Layout.fillWidth: true
                }

                Label {
                    text: {
                        var active = 0, queued = 0, done = 0, failed = 0
                        for (var i = 0; i < queueModel.count; ++i) {
                            var s = queueModel.get(i).status
                            switch (s) {
                            case "Processing":
                            case "processing":
                            case "CopyingMetadata":
                            case "copyingMetadata":   ++active; break
                            case "Queued": case "queued":      ++queued; break
                            case "Completed": case "completed": ++done; break
                            case "Failed": case "failed":      ++failed; break
                            }
                        }
                        return qsTr("%1 active · %2 queued · %3 done · %4 failed")
                            .arg(active).arg(queued).arg(done).arg(failed)
                    }
                    color: Theme.colors.textMuted
                    font.pixelSize: Theme.font.sizeSmall
                    Layout.rightMargin: 8
                }

                FlatButton {
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: "broom"
                    text: qsTr("Clear Completed")
                    onClicked: Transcode.clear_completed()
                }
                FlatButton {
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: "arrow-clockwise"
                    tooltip: qsTr("Refresh")
                    onClicked: Transcode.refresh_queue()
                }
            }
        }

        // ── Job list ────────────────────────────────────────────────
        // No border — same flush-stack idiom as Tracker / ItemListPanel.
        // Surface tone separates the list area from the toolbar above.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colors.surface

            ListView {
                id: jobList
                anchors.fill: parent
                clip: true
                model: queueModel
                spacing: 0
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    id: jobRow
                    required property int index
                    required property string jobId
                    required property string inputPath
                    required property string outputPath
                    required property string status
                    required property real progress
                    required property real currentFrame
                    required property real totalFrames
                    required property real fps
                    required property string error

                    // ListView.view.width is the canonical parent-width
                    // reference inside a delegate; using `jobList.width`
                    // resolved to -2 (parent width was 0 at creation
                    // time, minus the ListView's 1px anchor margins on
                    // each side), and the binding never recovered.
                    width: ListView.view ? Math.max(0, ListView.view.width) : 0
                    height: 56
                    color: index % 2 === 0 ? Theme.colors.surface : Theme.colors.surfaceAlt

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        // Status pill.
                        Rectangle {
                            Layout.preferredWidth: 88
                            Layout.preferredHeight: 22
                            radius: 10
                            color: root._statusColor(jobRow.status)
                            Label {
                                anchors.centerIn: parent
                                text: jobRow.status
                                color: Theme.colors.textBright
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }

                        // Filename + paths.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Label {
                                text: root._basename(jobRow.inputPath)
                                color: Theme.colors.text
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Label {
                                text: jobRow.error.length > 0
                                    ? qsTr("%1 → %2  ·  %3")
                                        .arg(jobRow.inputPath)
                                        .arg(jobRow.outputPath)
                                        .arg(jobRow.error)
                                    : qsTr("%1 → %2")
                                        .arg(jobRow.inputPath)
                                        .arg(jobRow.outputPath)
                                color: jobRow.error.length > 0 ? Theme.colors.error : Theme.colors.textSubtle
                                font.pixelSize: 10
                                font.family: "Consolas"
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            // Progress bar — inline under the paths.
                            // Core stores progress as a percentage
                            // (0–100); divide before clamping so a
                            // mid-job value doesn't slam to 100% width.
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 4
                                radius: 2
                                color: Theme.colors.surfaceHover
                                Rectangle {
                                    height: parent.height
                                    width: parent.width
                                        * Math.max(0, Math.min(1, jobRow.progress / 100))
                                    radius: 2
                                    color: root._statusColor(jobRow.status)
                                }
                            }
                        }

                        // Frame / fps.
                        Label {
                            text: jobRow.totalFrames > 0
                                ? qsTr("%1 / %2  · %3 fps")
                                    .arg(Math.round(jobRow.currentFrame))
                                    .arg(Math.round(jobRow.totalFrames))
                                    .arg(jobRow.fps.toFixed(1))
                                : ""
                            color: Theme.colors.textMuted
                            font.pixelSize: 10
                            font.family: "Consolas"
                            Layout.preferredWidth: 130
                            horizontalAlignment: Text.AlignRight
                        }

                        // Action buttons — context-dependent.
                        FlatButton {
                            iconName: "x"
                            tooltip: qsTr("Cancel")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            visible: !root._isFinal(jobRow.status)
                            onClicked: Transcode.cancel_job(jobRow.jobId)
                        }
                        FlatButton {
                            iconName: "app-window"
                            tooltip: qsTr("Open in Browser")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            visible: jobRow.status === "Completed"
                                || jobRow.status === "completed"
                            onClicked: root.openInBrowserRequested(jobRow.outputPath)
                        }
                        FlatButton {
                            iconName: "folder-simple"
                            tooltip: qsTr("Reveal in file manager")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            visible: jobRow.status === "Completed"
                                || jobRow.status === "completed"
                            onClicked: FileOps.reveal_in_file_manager(jobRow.outputPath)
                        }
                        FlatButton {
                            iconName: "trash"
                            tooltip: qsTr("Remove from queue")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            visible: root._isFinal(jobRow.status)
                            onClicked: Transcode.remove_job(jobRow.jobId)
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: queueModel.count === 0
                    text: qsTr("(no transcode jobs — right-click a video file and choose “Transcode to MP4”)")
                    color: Theme.colors.textSubtle
                    font.pixelSize: 11
                    font.italic: true
                    width: parent.width - 24
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
