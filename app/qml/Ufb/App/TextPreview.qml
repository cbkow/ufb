// Plain-text / Markdown preview for the lightbox. Reads the file via a
// file:// XMLHttpRequest (no new C++) and renders it with Qt's native
// Markdown/plain text. Size-capped. RTF/rich docs are v2. See
// devdocs/lightbox-preview-plan.md.

import QtQuick
import QtQuick.Controls

Flickable {
    id: root

    property string source: ""
    property string _text: ""
    readonly property bool _isMarkdown: source.toLowerCase().endsWith(".md")
                                        || source.toLowerCase().endsWith(".markdown")

    contentWidth: width
    contentHeight: txt.implicitHeight + 40
    clip: true
    ScrollBar.vertical: UfbScrollBar { policy: ScrollBar.AlwaysOn }

    onSourceChanged: _load()
    function _load() {
        _text = ""
        if (source.length === 0) return
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                var t = xhr.responseText || ""
                if (t.length > 1000000)   // ~1 MB cap for preview
                    t = t.substring(0, 1000000) + "\n\n… (truncated)"
                root._text = t
            }
        }
        xhr.open("GET", "file://" + source)
        xhr.send()
    }

    // Lightbox scroll-key API (same shape as PdfReader).
    function scrollStep(dy) {
        const maxY = Math.max(0, contentHeight - height)
        contentY = Math.max(0, Math.min(contentY + dy, maxY))
    }
    function scrollPage(dir) { scrollStep(dir * height * 0.9) }
    function scrollHome() { contentY = 0 }
    function scrollEnd() { contentY = Math.max(0, contentHeight - height) }

    TextArea {
        id: txt
        x: 24
        y: 20
        width: root.width - 48
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.Wrap
        textFormat: root._isMarkdown ? TextEdit.MarkdownText : TextEdit.PlainText
        color: "#e2e2e2"
        font.pixelSize: 14
        font.family: root._isMarkdown ? font.family : "Menlo"
        text: root._text
        background: null
    }
}
