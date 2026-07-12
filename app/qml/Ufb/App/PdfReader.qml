// Continuous-scroll PDF reader for the lightbox. A ListView of page images
// (lazy-rendered via image://ufb-pdf, evicted when offscreen → bounded
// memory). Wheel/trackpad scrolls natively; the lightbox routes Up/Down/
// PageUp/PageDown/Home/End to the scroll* functions below (Left/Right stay
// media navigation). See devdocs/lightbox-preview-plan.md.

import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: root

    // Absolute path of the PDF/AI to read.
    property string source: ""

    // Release the backend's cached open document when the reader closes, so
    // the PDF isn't held open (and lockable on Windows) after the preview.
    Component.onDestruction: PdfDoc.releaseCache()

    readonly property int _count: source.length > 0 ? PdfDoc.pageCount(source) : 0
    // Assume a uniform page aspect (page 0) to size delegates without a
    // reflow as pages render. Mixed-orientation docs letterbox per page.
    readonly property real _aspect: (source.length > 0 && _count > 0)
                                    ? PdfDoc.pageAspect(source, 0) : 1.3
    readonly property real _dpr: Screen.devicePixelRatio

    // Scroll API driven by the lightbox's key handler.
    function scrollStep(dy) {
        const maxY = Math.max(0, list.contentHeight - list.height)
        list.contentY = Math.max(0, Math.min(list.contentY + dy, maxY))
    }
    function scrollPage(dir) { scrollStep(dir * list.height * 0.9) }
    function scrollHome() { list.contentY = 0 }
    function scrollEnd() { list.contentY = Math.max(0, list.contentHeight - list.height) }

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: root._count
        spacing: 12
        topMargin: 12
        bottomMargin: 12
        cacheBuffer: Math.round(height * 1.5)   // render a little ahead/behind
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: UfbScrollBar { policy: ScrollBar.AlwaysOn }

        delegate: Item {
            width: list.width
            height: Math.round((list.width - Theme.dim.scrollBarWidth)
                               * (root._aspect > 0 ? root._aspect : 1.3))
            Image {
                anchors.fill: parent
                anchors.rightMargin: Theme.dim.scrollBarWidth   // gutter for the bar
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                smooth: true
                sourceSize.width: Math.round(list.width * root._dpr)
                source: "image://ufb-pdf/" + index + "/" + root.source
            }
        }
    }
}
