// DocxDoc — QML singleton rendering a Word document (.docx/.docm/.dotx,
// an OOXML zip) into a staged HTML page for the lightbox's HtmlPreview
// (WebEngine). Same shape as MndbDoc's .mnpkg route: vendored miniz reads
// the zip, the render lands in a per-document temp stage.
//
// Why UFB renders these itself: without it a .docx preview is only the
// OS shell thumbnail. macOS QuickLook renders page 1 for any .docx, but
// the Windows shell handler only returns the thumbnail Word embeds when
// "Save Thumbnail" was ticked (and only with Office installed) — so on
// Windows nearly every .docx fell through to the file icon.
//
// Scope — a readable approximation, not a layout engine: paragraphs,
// heading styles (styles.xml, basedOn chains, docDefaults, theme fonts),
// run formatting, numbered/bulleted lists (numbering.xml counters),
// tables (gridSpan / vMerge / shading / style borders), hyperlinks,
// embedded raster images, text boxes, page size + margins from the last
// sectPr. Not rendered: headers/footers, footnotes, comments, tracked
// deletions, fields' codes (their cached results are), EMF/WMF/TIFF
// images (placeholder), floating-object positioning, columns, math.
//
// Document content is untrusted (files sync from teammates and render
// in a JS-enabled file:// WebEngine page): every string is HTML-escaped,
// colors go through QColor, font names are character-allowlisted, hrefs
// are scheme-allowlisted (http/https/mailto), external relationship
// targets are never fetched, zip entries are size-capped and their
// names never reach the filesystem.

#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration>

class DocxDoc : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit DocxDoc(QObject *parent = nullptr) : QObject(parent) {}

    // Renders `docxPath` to a temp HTML file and returns its path, or ""
    // when the file isn't a readable OOXML word document (the lightbox
    // then falls back to the shell thumbnail / file icon). Synchronous;
    // a fresh stage (same size + mtime) is reused without re-rendering.
    Q_INVOKABLE QString htmlPreviewPath(const QString &docxPath) const;
};
