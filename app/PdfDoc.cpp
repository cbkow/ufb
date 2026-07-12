#include "PdfDoc.h"

#include "PdfBackend.h"   // thumbnails/ is on the include path

int PdfDoc::pageCount(const QString &path) const
{
    return ufb::pdfPageCount(path);
}

double PdfDoc::pageAspect(const QString &path, int page) const
{
    return ufb::pdfPageAspect(path, page);
}

void PdfDoc::releaseCache() const
{
    ufb::releasePdfCache();
}
