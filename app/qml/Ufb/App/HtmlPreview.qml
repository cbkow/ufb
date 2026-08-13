// Rendered HTML preview for the lightbox, backed by Qt WebEngine
// (Chromium) — real CSS, embedded data: URI images, the works. Only
// routed to when the optional WebEngine module was compiled in
// (PreviewLightbox gates on _webEngineAvailable); TextPreview's
// QTextDocument rendering is the fallback everywhere else.
//
// Scope decisions:
//  - JavaScript stays ON: previews are job-folder files from the
//    user's own team (not mail attachments), scripts are part of
//    faithful rendering, and the scroll-key API below needs
//    runJavaScript. Revisit if previews ever cover untrusted inboxes.
//  - Navigation is frozen to the previewed document: clicking a link
//    opens it in the system browser instead of browsing inside the
//    lightbox (a preview is a viewer, not a tab).

import QtQuick
import QtQuick.Controls
import QtWebEngine

Item {
    id: root

    property string source: ""

    // canGoBack/backToGrid deliberately absent — Esc closes (lightbox
    // checks for those before treating Esc as close).

    // UfbScrollBar, transcribed to CSS. Chromium honours the
    // ::-webkit-scrollbar pseudo-elements, so the preview's scrollbar
    // matches the app: flat squared thumb in borderStrong, textSubtle
    // on hover, textMuted while dragging. (Styling these also opts out
    // of macOS overlay scrollbars — same always-visible-when-overflowing
    // behaviour as UfbScrollBar.) Qt colors stringify as #AARRGGBB,
    // which CSS rejects — hence the rgba() helper.
    //
    // The track/gutter is painted with the PAGE's own computed
    // background (resolved at inject time in the user script) rather
    // than a theme color — a classic scrollbar reserves layout space,
    // and any color other than the page's own reads as a hole beside
    // the content.
    function _cssColor(c) {
        return "rgba(" + Math.round(c.r * 255) + "," + Math.round(c.g * 255)
             + "," + Math.round(c.b * 255) + "," + c.a + ")"
    }
    readonly property string _scrollbarCss:
        "::-webkit-scrollbar{width:" + Theme.dim.scrollBarWidth + "px;height:"
            + Theme.dim.scrollBarWidth + "px;background:__PAGE_BG__}"
        + "::-webkit-scrollbar-track{background:__PAGE_BG__}"
        + "::-webkit-scrollbar-thumb{background:" + _cssColor(Theme.colors.borderStrong)
            + ";border-radius:" + Theme.dim.radius + "px}"
        + "::-webkit-scrollbar-thumb:hover{background:" + _cssColor(Theme.colors.textSubtle) + "}"
        + "::-webkit-scrollbar-thumb:active{background:" + _cssColor(Theme.colors.textMuted) + "}"
        + "::-webkit-scrollbar-corner{background:__PAGE_BG__}"

    // Lightbox scroll-key API (same shape as PdfReader/TextPreview).
    // Chromium owns the scroll state, so these go through page JS.
    function _js(code) { if (webLoader.item) webLoader.item.runJavaScript(code) }
    function scrollStep(dy) { _js("window.scrollBy(0," + dy + ")") }
    function scrollPage(dir) {
        _js("window.scrollBy(0," + (dir > 0 ? 1 : -1)
            + " * window.innerHeight * 0.9)")
    }
    function scrollHome() { _js("window.scrollTo(0,0)") }
    function scrollEnd() { _js("window.scrollTo(0, document.body.scrollHeight)") }

    // Chromium allocates its render surface at the size the view has
    // when it initializes. The lightbox's content Loader incubates us
    // ASYNCHRONOUSLY, so at Component creation our geometry can still
    // be settling — the view then keeps the too-narrow surface, and the
    // missed right-hand column shows the view background instead of
    // page pixels (a 100px+ "hole" beside the page). Defer the
    // WebEngineView until the container has real, settled geometry so
    // Chromium initializes at final size.
    // Letterboxed like a document viewer: minNotes exports (and our
    // .mndb render) use a left-anchored ~808px measure (760 + padding)
    // on a full-bleed dark canvas, so an edge-to-edge view leaves a
    // huge empty slab on the right that reads as a rendering hole. Cap
    // the view CLOSE to the measure — a loose cap just re-creates the
    // margin at every window width — and center it so the scrim frames
    // the page evenly. 940 = measure + minNotes' block-number rail +
    // scrollbar, with a slim breathing edge.
    Loader {
        id: webLoader
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.min(root.width, 940)
        active: root.width > 1 && root.height > 1
        sourceComponent: webComp
    }

    Component {
        id: webComp

        WebEngineView {
            // Transparent, not theme grey: any region Chromium hasn't
            // painted (initial frames) shows the lightbox scrim behind
            // instead of flashing black.
            backgroundColor: "transparent"
            url: root.source.length > 0
                 ? "file://" + encodeURI(root.source)
                 : ""

            // Scrollbar skin. Injected via runJavaScript at load-finish
            // rather than userScripts — assigning plain JS objects to
            // userScripts.collection is silently dropped (verified via
            // screenshot harness: stock scrollbars survived), and the
            // brief default-scrollbar flash before LoadSucceeded is
            // invisible in practice for local files.
            onLoadingChanged: (info) => {
                if (info.status !== WebEngineView.LoadSucceededStatus)
                    return
                runJavaScript("(function(){"
                    // Effective page background: body's, falling back to
                    // html's, falling back to white (Chromium's default
                    // canvas for pages that set neither).
                    + "function bg(el){var c=getComputedStyle(el).backgroundColor;"
                    +   "return (c&&c!=='rgba(0, 0, 0, 0)'&&c!=='transparent')?c:null}"
                    + "var pageBg=bg(document.body)||bg(document.documentElement)||'#ffffff';"
                    + "var s=document.createElement('style');"
                    + "s.textContent=" + JSON.stringify(root._scrollbarCss)
                    +   ".split('__PAGE_BG__').join(pageBg);"
                    + "(document.head||document.documentElement).appendChild(s);"
                    + "})()")
            }

            onNavigationRequested: (request) => {
                // First load of the previewed file is fine; anything after
                // that (link clicks, redirects) leaves the app instead.
                var target = request.url.toString()
                var self = "file://" + encodeURI(root.source)
                if (target !== self && request.navigationType
                        !== WebEngineNavigationRequest.ReloadNavigation) {
                    request.reject()
                    Qt.openUrlExternally(request.url)
                }
            }
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: webLoader.item ? webLoader.item.loading : false
        visible: running
    }
}
