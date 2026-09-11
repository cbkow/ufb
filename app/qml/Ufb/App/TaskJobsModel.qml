// TaskJobsModel — non-visual mirror of the two background-job queues
// (Transcode + Archive) in the shape the footer status strip and the
// Tasks tab chip want: a flat list of active jobs, a list of
// unacknowledged failures, and a "just finished" signal per job so the
// footer can show the same lingering "… copied" confirmation it shows for
// copy / move / delete.
//
// Queuing a job never switches tabs; this model is how the user sees
// what's running without leaving their folder.

import QtQuick

import Ufb.Backend 1.0

QtObject {
    id: root

    /// [{id, kind, verb, title, progress(0–100)}] — queued or running.
    property var active: []
    /// [{id, kind, title, error}] — failed and not yet acknowledged.
    property var failed: []
    readonly property int activeCount: active.length

    /// One emission per job that just reached Completed, with a ready
    /// footer message ("Extracted sample.zip").
    signal completed(string message)

    /// Clear a failure from `failed` (the Tasks tab keeps the row).
    function acknowledge(id) {
        var acked = root._ackedIds
        acked[String(id)] = true
        root._ackedIds = acked
        root.refresh()
    }
    function acknowledgeAll() {
        var acked = root._ackedIds
        for (var i = 0; i < root.failed.length; ++i)
            acked[root.failed[i].id] = true
        root._ackedIds = acked
        root.refresh()
    }

    property var _ackedIds: ({})
    /// id → true for every job seen in an active state; a job leaving
    /// that set as Completed is what fires `completed`.
    property var _wasActive: ({})

    function _basename(p) {
        if (!p) return ""
        var idx = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"))
        return idx >= 0 ? p.substring(idx + 1) : p
    }
    function _norm(s) { return String(s || "").toLowerCase() }
    function _isActive(s) {
        var n = _norm(s)
        return n === "queued" || n === "processing" || n === "copyingmetadata"
    }

    function refresh() {
        var active = [], failed = [], done = []
        var lists = [
            { json: Transcode.queue_json, kind: "transcode" },
            { json: Archive.queue_json,   kind: "archive" }
        ]
        for (var l = 0; l < lists.length; ++l) {
            if (!lists[l].json) continue
            var arr
            try { arr = JSON.parse(lists[l].json) } catch (e) { continue }
            for (var i = 0; i < arr.length; ++i) {
                var j = arr[i] || {}
                var id = String(j.id || "")
                var kind, title, verb, pastVerb
                if (lists[l].kind === "transcode") {
                    kind = "transcode"
                    title = _basename(j.inputPath)
                    verb = qsTr("Transcoding"); pastVerb = qsTr("Transcoded")
                } else {
                    kind = j.kind === "compress" ? "compress" : "extract"
                    var inputs = j.inputs || []
                    title = _basename(inputs.length > 0 ? String(inputs[0]) : "")
                    if (kind === "compress" && inputs.length > 1)
                        title = qsTr("%1 items").arg(inputs.length)
                    verb = kind === "compress" ? qsTr("Zipping") : qsTr("Extracting")
                    pastVerb = kind === "compress" ? qsTr("Zipped") : qsTr("Extracted")
                }
                var st = _norm(j.status)
                if (_isActive(st)) {
                    active.push({
                        id: id, kind: kind, title: title,
                        verb: st === "queued" ? qsTr("Queued") : verb,
                        progress: typeof j.progress === "number" ? j.progress : 0
                    })
                } else if (st === "failed") {
                    if (!root._ackedIds[id])
                        failed.push({ id: id, kind: kind, title: title,
                                      error: String(j.error || "") })
                } else if (st === "completed") {
                    done.push({ id: id, message: qsTr("%1 %2").arg(pastVerb).arg(title) })
                }
            }
        }
        // Fire `completed` for jobs that were active last time we
        // looked and are Completed now.
        var was = root._wasActive
        var now = {}
        for (var a = 0; a < active.length; ++a) now[active[a].id] = true
        for (var d = 0; d < done.length; ++d) {
            if (was[done[d].id]) root.completed(done[d].message)
        }
        root._wasActive = now
        root.active = active
        root.failed = failed
    }

    property Connections _transcodeConn: Connections {
        target: Transcode
        function onQueue_jsonChanged() { root.refresh() }
    }
    property Connections _archiveConn: Connections {
        target: Archive
        function onQueue_jsonChanged() { root.refresh() }
    }
    Component.onCompleted: refresh()
}
