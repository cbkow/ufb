// PdfDoc — tiny QML singleton exposing PDF page metadata to the lightbox's
// continuous-scroll reader (page count + per-page aspect). Page bitmaps come
// from the image://ufb-pdf provider; this just sizes the page list.
// See devdocs/lightbox-preview-plan.md.

#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration>

class PdfDoc : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit PdfDoc(QObject *parent = nullptr) : QObject(parent) {}

    // Number of pages in `path` (0 on failure). Synchronous (parses the PDF
    // header); fast enough for the GUI thread for preview use.
    Q_INVOKABLE int pageCount(const QString &path) const;

    // height/width of page `page` (0 on failure).
    Q_INVOKABLE double pageAspect(const QString &path, int page) const;

    // Close the backend's cached open document. The reader calls this on
    // destruction so the PDF isn't held open after the preview closes.
    Q_INVOKABLE void releaseCache() const;
};
