#pragma once

// Phase 14 step 6 - PDF/AI thumbnails via PDFium.
//
// PDFium is Chromium's PDF rendering engine; we vendor the
// bblanchon/pdfium-binaries Windows prebuilt (BSD-3). Renders
// the first page of a PDF to a bitmap. AI files are PDF
// containers - PDFium reads them too.

#include <QImage>
#include <QString>
#include <QSize>

namespace ufb {

// First-page render (thumbnails). Equivalent to decodePdfPage(path, 0, size).
QImage decodePdf(const QString& path, QSize requestedSize);

// Render page `pageIndex` (0-based) fitted into requestedSize. Used by the
// in-app PDF reader (image://ufb-pdf). Returns null on error / bad index.
QImage decodePdfPage(const QString& path, int pageIndex, QSize requestedSize);

// Number of pages, or 0 on failure. For the reader's page model.
int pdfPageCount(const QString& path);

// Page height / width (aspect) for `pageIndex`, or 0 on failure. Lets the
// reader size page delegates before the bitmaps render.
double pdfPageAspect(const QString& path, int pageIndex);

// Close the cached open document (see PdfBackend.cpp). Call when the reader
// closes so the file isn't held open after the preview goes away.
void releasePdfCache();

} // namespace ufb
