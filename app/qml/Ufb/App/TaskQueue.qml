// TaskQueue — top-level workspace tab listing every active / queued /
// completed background job: transcodes (Transcode singleton) and
// archive extract / compress jobs (Archive singleton). Both queues are
// mirrored into one ListModel; per-row delegate shows progress + status
// pill + action buttons (Cancel / Remove / Open in Browser / Reveal),
// dispatching back to the owning singleton by `kind`.
//
// Sourcing: each singleton's `queue_json` re-emits on every job_updated
// / progress event from its Rust worker. We also pull once on open
// because the events forwarders only light up after the first job.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ufb.Backend 1.0

Rectangle {
    id: root

    color: Theme.colors.bg

    /// Emitted when the user clicks "Open in Browser" on a completed
    /// row. Main routes this to navigateActivePane on the most-recent
    /// Files tab so the output lands in UFB's own FileBrowser with the
    /// item pre-selected.
    signal openInBrowserRequested(string path)

    ListModel { id: queueModel }

    function refresh() {
        queueModel.clear()
        _appendTranscodeJobs()
        _appendArchiveJobs()
    }

    function _appendTranscodeJobs() {
        if (!Transcode.queue_json) return
        try {
            var arr = JSON.parse(Transcode.queue_json)
            for (var i = 0; i < arr.length; ++i) {
                var j = arr[i] || {}
                // `jobId` not `id` — `id` is reserved as QML's element
                // identifier, and a delegate's `required property
                // string id` silently conflicts with the delegate's own
                // `id: jobRow`, leaving rows unrendered.
                queueModel.append({
                    jobId: String(j.id || ""),
                    kind: "transcode",
                    title: root._basename(j.inputPath),
                    inputPath: String(j.inputPath || ""),
                    outputPath: String(j.outputPath || ""),
                    status: String(j.status || "Queued"),
                    progress: typeof j.progress === "number" ? j.progress : 0,
                    detail: (typeof j.totalFrames === "number" && j.totalFrames > 0)
                        ? qsTr("%1 / %2  · %3 fps")
                            .arg(Math.round(j.currentFrame || 0))
                            .arg(Math.round(j.totalFrames))
                            .arg((j.fps || 0).toFixed(1))
                        : "",
                    error: String(j.error || ""),
                    warning: ""
                })
            }
        } catch (e) {
            console.warn("TaskQueue: transcode parse failed:", e)
        }
    }

    function _appendArchiveJobs() {
        if (!Archive.queue_json) return
        try {
            var arr = JSON.parse(Archive.queue_json)
            for (var i = 0; i < arr.length; ++i) {
                var j = arr[i] || {}
                var inputs = j.inputs || []
                var first = inputs.length > 0 ? String(inputs[0]) : ""
                var title = root._basename(first)
                if (inputs.length > 1)
                    title = qsTr("%1 (+%2 more)").arg(title).arg(inputs.length - 1)
                queueModel.append({
                    jobId: String(j.id || ""),
                    kind: j.kind === "compress" ? "compress" : "extract",
                    title: title,
                    inputPath: first,
                    outputPath: String(j.outputPath || ""),
                    status: String(j.status || "Queued"),
                    progress: typeof j.progress === "number" ? j.progress : 0,
                    detail: String(j.currentItem || ""),
                    error: String(j.error || ""),
                    warning: String(j.warning || "")
                })
            }
        } catch (e) {
            console.warn("TaskQueue: archive parse failed:", e)
        }
    }

    function _basename(p) {
        if (!p) return ""
        var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
        return idx >= 0 ? p.substring(idx + 1) : p
    }

    function _kindLabel(k) {
        switch (k) {
        case "transcode": return qsTr("Transcode")
        case "extract":   return qsTr("Extract")
        case "compress":  return qsTr("Zip")
        }
        return k
    }
    function _kindIcon(k) {
        switch (k) {
        case "transcode": return "film-strip"
        case "extract":   return "file-archive"
        case "compress":  return "file-zip"
        }
        return "stack"
    }
    function _norm(s) { return String(s || "").toLowerCase() }

    function _statusColor(s) {
        switch (_norm(s)) {
        case "queued":          return Theme.colors.textMuted
        case "processing":      return Theme.colors.accent
        case "copyingmetadata": return Theme.colors.success
        case "completed":       return Theme.colors.success
        case "failed":          return Theme.colors.error
        case "cancelled":       return Theme.colors.textMuted
        }
        return Theme.colors.textMuted
    }
    function _statusText(s) {
        switch (_norm(s)) {
        case "queued":          return qsTr("Queued")
        case "processing":      return qsTr("Processing")
        case "copyingmetadata": return qsTr("Metadata")
        case "completed":       return qsTr("Completed")
        case "failed":          return qsTr("Failed")
        case "cancelled":       return qsTr("Cancelled")
        }
        return s
    }
    function _isFinal(s) {
        var n = _norm(s)
        return n === "completed" || n === "failed" || n === "cancelled"
    }
    function _isDone(s) { return _norm(s) === "completed" }

    function _cancel(kind, id) {
        if (kind === "transcode") Transcode.cancel_job(id)
        else Archive.cancel_job(id)
    }
    function _remove(kind, id) {
        if (kind === "transcode") Transcode.remove_job(id)
        else Archive.remove_job(id)
    }

    Component.onCompleted: {
        Transcode.refresh_queue()
        Archive.refresh_queue()
        refresh()
    }
    Connections {
        target: Transcode
        function onQueue_jsonChanged() { root.refresh() }
    }
    Connections {
        target: Archive
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
                    name: "stack"
                    size: Theme.icon.sizeToolbar
                    color: Theme.colors.textMuted
                    Layout.rightMargin: 6
                    Layout.alignment: Qt.AlignVCenter
                }
                Label {
                    text: qsTr("Task Queue")
                    color: Theme.colors.textBright
                    font.pixelSize: Theme.font.sizeHeading
                    font.bold: true
                    Layout.fillWidth: true
                }

                Label {
                    text: {
                        var active = 0, queued = 0, done = 0, failed = 0
                        for (var i = 0; i < queueModel.count; ++i) {
                            switch (root._norm(queueModel.get(i).status)) {
                            case "processing":
                            case "copyingmetadata": ++active; break
                            case "queued":          ++queued; break
                            case "completed":       ++done; break
                            case "failed":          ++failed; break
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
                    onClicked: {
                        Transcode.clear_completed()
                        Archive.clear_completed()
                    }
                }
                FlatButton {
                    Layout.preferredHeight: Theme.dim.toolStripHeight
                    iconName: "arrow-clockwise"
                    tooltip: qsTr("Refresh")
                    onClicked: {
                        Transcode.refresh_queue()
                        Archive.refresh_queue()
                    }
                }
            }
        }

        // ── Job list ────────────────────────────────────────────────
        // No border — same flush-stack idiom as Tracker / ItemListPanel.
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
                    required property string kind
                    required property string title
                    required property string inputPath
                    required property string outputPath
                    required property string status
                    required property real progress
                    required property string detail
                    required property string error
                    required property string warning

                    // ListView.view.width is the canonical parent-width
                    // reference inside a delegate (see git history for
                    // the -2px binding trap this avoids).
                    width: ListView.view ? Math.max(0, ListView.view.width) : 0
                    height: 56
                    color: index % 2 === 0 ? Theme.colors.surface : Theme.colors.surfaceAlt

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        // Kind icon.
                        Icon {
                            name: root._kindIcon(jobRow.kind)
                            size: Theme.icon.sizeToolbar
                            color: Theme.colors.textMuted
                            Layout.alignment: Qt.AlignVCenter
                        }

                        // Status pill.
                        Rectangle {
                            Layout.preferredWidth: 88
                            Layout.preferredHeight: 22
                            radius: 10
                            color: root._statusColor(jobRow.status)
                            Label {
                                anchors.centerIn: parent
                                text: root._statusText(jobRow.status)
                                color: Theme.colors.textBright
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }

                        // Title + paths + progress.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Label {
                                text: qsTr("%1  ·  %2")
                                    .arg(root._kindLabel(jobRow.kind))
                                    .arg(jobRow.title)
                                color: Theme.colors.text
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Label {
                                text: {
                                    var base = qsTr("%1 → %2")
                                        .arg(jobRow.inputPath)
                                        .arg(jobRow.outputPath)
                                    if (jobRow.error.length > 0)
                                        return qsTr("%1  ·  %2").arg(base).arg(jobRow.error)
                                    if (jobRow.warning.length > 0)
                                        return qsTr("%1  ·  %2").arg(base).arg(jobRow.warning)
                                    return base
                                }
                                color: jobRow.error.length > 0
                                    ? Theme.colors.error
                                    : (jobRow.warning.length > 0
                                        ? Theme.colors.textMuted
                                        : Theme.colors.textSubtle)
                                font.pixelSize: 10
                                font.family: "Consolas"
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            // Progress bar — core stores 0–100.
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

                        // Right column: frames/fps for transcode, the
                        // current entry for archive jobs.
                        Label {
                            text: jobRow.detail
                            color: Theme.colors.textMuted
                            font.pixelSize: 10
                            font.family: "Consolas"
                            elide: Text.ElideLeft
                            Layout.preferredWidth: 160
                            horizontalAlignment: Text.AlignRight
                        }

                        // Action buttons — context-dependent.
                        FlatButton {
                            iconName: "x"
                            tooltip: qsTr("Cancel")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            visible: !root._isFinal(jobRow.status)
                            onClicked: root._cancel(jobRow.kind, jobRow.jobId)
                        }
                        FlatButton {
                            iconName: "app-window"
                            tooltip: qsTr("Open in Browser")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            visible: root._isDone(jobRow.status)
                            onClicked: root.openInBrowserRequested(jobRow.outputPath)
                        }
                        FlatButton {
                            iconName: "folder-simple"
                            tooltip: qsTr("Reveal in file manager")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            visible: root._isDone(jobRow.status)
                            onClicked: FileOps.reveal_in_file_manager(jobRow.outputPath)
                        }
                        FlatButton {
                            iconName: "trash"
                            tooltip: qsTr("Remove from queue")
                            Layout.preferredHeight: Theme.dim.toolStripHeight
                            visible: root._isFinal(jobRow.status)
                            onClicked: root._remove(jobRow.kind, jobRow.jobId)
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: queueModel.count === 0
                    text: qsTr("(no jobs — right-click files and choose “Transcode to MP4”, “Extract Here” or “Compress to ZIP”)")
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
