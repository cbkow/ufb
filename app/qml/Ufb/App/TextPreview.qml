// Plain-text / Markdown / HTML preview for the lightbox. Content comes
// from TextInfo.readHead — a capped read (1 MB) so a multi-GB render log
// never materialises in RAM (the old file:// XMLHttpRequest read the
// whole file before truncating). Routing (which files land here,
// including the content-sniff for extensionless text) lives in
// TextInfo.isText; see devdocs/lightbox-preview-plan.md.

import QtQuick
import QtQuick.Controls
import Ufb.Backend 1.0

Flickable {
    id: root

    property string source: ""
    property string _text: ""

    // Directory of the previewed file WITH its trailing separator —
    // baseUrl needs the slash kept or relative hrefs resolve against
    // the parent. Split on both separators for Windows paths.
    readonly property string _sourceDir: {
        var i = Math.max(source.lastIndexOf("/"), source.lastIndexOf("\\"))
        return i >= 0 ? source.substring(0, i + 1) : ""
    }

    readonly property string _ext: {
        var i = source.lastIndexOf(".")
        var s = i >= 0 ? source.substring(i + 1).toLowerCase() : ""
        return s.indexOf("/") >= 0 ? "" : s   // dot in a dir name, not an ext
    }
    readonly property bool _isMarkdown: _ext === "md" || _ext === "markdown"
    // Rendered, not source — Qt's rich text engine handles the HTML subset
    // typical of notes/exports; full web pages degrade to readable text.
    readonly property bool _isHtml: _ext === "html" || _ext === "htm"

    contentWidth: width
    contentHeight: txt.implicitHeight + 40
    clip: true
    ScrollBar.vertical: UfbScrollBar { policy: ScrollBar.AlwaysOn }

    // Read caps. Logs/code want a tight cap (first MB is plenty and keeps
    // the TextArea snappy); HTML is a *document* — note exports (minNotes)
    // routinely embed screenshots as multi-MB base64 data: URIs, and a
    // 1 MB cut lands mid-image and beheads the file. 32 MB renders those
    // whole while still declining pathological giants.
    readonly property int _readCap: _isHtml ? 32000000 : 1000000

    onSourceChanged: {
        contentY = 0
        _text = source.length > 0 ? TextInfo.readHead(source, _readCap) : ""
    }
    Component.onCompleted: if (source.length > 0 && _text.length === 0)
        _text = TextInfo.readHead(source, _readCap)

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
        // Resolve relative <img src>/<a href> against the document's own
        // folder, not this QML file's qrc: URL (the default), so HTML
        // exports that ship an assets/ dir next to them show their images.
        // FileOps.to_file_url, not "file://" + path: a hand-built URL
        // parses the Windows drive letter as the URL host, so relative
        // images silently dropped on Windows.
        baseUrl: root._sourceDir.length > 0
                 ? FileOps.to_file_url(root._sourceDir)
                 : ""
        textFormat: root._isMarkdown ? TextEdit.MarkdownText
                  : root._isHtml     ? TextEdit.RichText
                                     : TextEdit.PlainText
        color: "#e2e2e2"
        font.pixelSize: 14
        font.family: (root._isMarkdown || root._isHtml) ? font.family
                                                        : Theme.font.mono
        text: root._text
        background: null
    }
}
