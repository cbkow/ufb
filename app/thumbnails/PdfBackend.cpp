#include "PdfBackend.h"

// PDFium pulls in windows.h transitively. Block its min/max
// macros so std::min/max work below.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <fpdfview.h>

#include <QFileInfo>
#include <QDateTime>

#include <algorithm>
#include <mutex>

namespace ufb {

namespace {

// PDFium has process-global state (FPDF_InitLibrary). Calls to
// LoadDocument / RenderPage must serialise through a single
// mutex; PDFium is documented as not thread-safe.
//
// Init once on first use, never destroy - the library lives for
// the app's lifetime which is fine for our use case.
std::mutex& pdfMutex() {
    static std::mutex m;
    return m;
}

void ensurePdfiumInitialised() {
    static std::once_flag once;
    std::call_once(once, []() {
        FPDF_LIBRARY_CONFIG cfg = {};
        cfg.version = 2;
        cfg.m_pUserFontPaths    = nullptr;
        cfg.m_pIsolate          = nullptr;
        cfg.m_v8EmbedderSlot    = 0;
        FPDF_InitLibraryWithConfig(&cfg);
    });
}

// Single-entry open-document cache. The continuous-scroll reader renders
// many pages of one PDF and also queries page count + per-page aspect, each
// of which previously re-opened (re-parsed header/xref of) the whole file.
// We keep the most-recently-used document open so those calls are cheap.
// Keyed by path + mtime so an edited file re-opens. All access is under
// pdfMutex (PDFium isn't thread-safe), so a single entry is plenty — the
// lightbox views one document at a time. releasePdfCache() closes it when
// the reader goes away, so the file isn't held open (and lockable on
// Windows) after the preview closes.
struct DocCache {
    QString       path;
    qint64        mtimeMs = -1;
    FPDF_DOCUMENT doc     = nullptr;
};
DocCache& docCache() {
    static DocCache c;
    return c;
}

// Returns a cached or freshly-loaded document for `path` (caller MUST hold
// pdfMutex). The returned doc is owned by the cache — do NOT close it.
FPDF_DOCUMENT acquireDocLocked(const QString& path) {
    const qint64 mtimeMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    DocCache& c = docCache();
    if (c.doc && c.path == path && c.mtimeMs == mtimeMs)
        return c.doc;
    if (c.doc) {
        FPDF_CloseDocument(c.doc);
        c.doc = nullptr;
    }
    c.doc = FPDF_LoadDocument(path.toUtf8().constData(), nullptr);
    c.path = path;
    c.mtimeMs = mtimeMs;
    return c.doc;
}

} // namespace

void releasePdfCache() {
    std::lock_guard<std::mutex> lock(pdfMutex());
    DocCache& c = docCache();
    if (c.doc) {
        FPDF_CloseDocument(c.doc);
        c.doc = nullptr;
    }
    c.path.clear();
    c.mtimeMs = -1;
}

int pdfPageCount(const QString& path) {
    ensurePdfiumInitialised();
    std::lock_guard<std::mutex> lock(pdfMutex());
    FPDF_DOCUMENT doc = acquireDocLocked(path);
    if (!doc) return 0;
    const int n = FPDF_GetPageCount(doc);
    return n < 0 ? 0 : n;
}

double pdfPageAspect(const QString& path, int pageIndex) {
    ensurePdfiumInitialised();
    std::lock_guard<std::mutex> lock(pdfMutex());
    FPDF_DOCUMENT doc = acquireDocLocked(path);
    if (!doc) return 0.0;
    double aspect = 0.0;
    if (pageIndex >= 0 && pageIndex < FPDF_GetPageCount(doc)) {
        if (FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex)) {
            const double w = FPDF_GetPageWidth(page);
            const double h = FPDF_GetPageHeight(page);
            if (w > 0 && h > 0) aspect = h / w;
            FPDF_ClosePage(page);
        }
    }
    return aspect;
}

QImage decodePdf(const QString& path, QSize requestedSize) {
    return decodePdfPage(path, 0, requestedSize);
}

QImage decodePdfPage(const QString& path, int pageIndex, QSize requestedSize) {
    ensurePdfiumInitialised();

    qInfo("PdfBackend: render path=%s page=%d", qPrintable(path), pageIndex);

    // Single-threaded for the entire pipeline - PDFium globals.
    std::lock_guard<std::mutex> lock(pdfMutex());

    FPDF_DOCUMENT doc = acquireDocLocked(path);  // cache-owned; do not close
    if (!doc) {
        const unsigned long err = FPDF_GetLastError();
        qWarning("PdfBackend: LoadDocument failed for %s (err=%lu)",
                 qPrintable(path), err);
        return QImage();
    }

    if (pageIndex < 0 || pageIndex >= FPDF_GetPageCount(doc)) {
        qWarning("PdfBackend: page %d out of range in %s", pageIndex, qPrintable(path));
        return QImage();
    }

    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) {
        qWarning("PdfBackend: LoadPage %d failed for %s", pageIndex, qPrintable(path));
        return QImage();
    }

    const double srcW = FPDF_GetPageWidth(page);
    const double srcH = FPDF_GetPageHeight(page);
    if (srcW <= 0 || srcH <= 0) {
        FPDF_ClosePage(page);
        return QImage();
    }

    // Compute target size preserving aspect. The page's native size is in
    // points (~72 DPI), so rendering at native size looks low-res — scale to
    // the requested pixel size. Honor a width-only OR height-only request
    // (the PDF reader passes only sourceSize.width and derives height from
    // the page aspect), not just the both-set case.
    int tgtW = int(srcW);
    int tgtH = int(srcH);
    const int reqW = requestedSize.width();
    const int reqH = requestedSize.height();
    double s = 0.0;
    if (reqW > 0 && reqH > 0)
        s = std::min(double(reqW) / srcW, double(reqH) / srcH);
    else if (reqW > 0)
        s = double(reqW) / srcW;
    else if (reqH > 0)
        s = double(reqH) / srcH;
    if (s > 0.0) {
        tgtW = std::max(1, int(srcW * s));
        tgtH = std::max(1, int(srcH * s));
    }

    // FPDFBitmap_BGRA: BGRA bytes in memory. Matches QImage's
    // Format_ARGB32 byte order on little-endian (Windows is LE).
    FPDF_BITMAP bmp = FPDFBitmap_Create(tgtW, tgtH, 1);
    if (!bmp) {
        FPDF_ClosePage(page);
        FPDF_CloseDocument(doc);
        return QImage();
    }
    // White background so transparent PDFs (vector logos etc.)
    // render readable rather than as black-on-black.
    FPDFBitmap_FillRect(bmp, 0, 0, tgtW, tgtH, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, page, 0, 0, tgtW, tgtH, 0,
                          FPDF_ANNOT | FPDF_LCD_TEXT);

    void* buf = FPDFBitmap_GetBuffer(bmp);
    const int stride = FPDFBitmap_GetStride(bmp);
    const QImage view(static_cast<const uchar*>(buf),
                      tgtW, tgtH, stride, QImage::Format_ARGB32);
    QImage out = view.copy();

    FPDFBitmap_Destroy(bmp);
    FPDF_ClosePage(page);
    // doc stays open in docCache() for subsequent page/aspect calls.

    qInfo("PdfBackend: ok path=%s -> %dx%d",
          qPrintable(path), out.width(), out.height());
    return out;
}

} // namespace ufb
