#pragma once

// Phase 14 — central thumbnail extraction surface.
//
// Two roles in one class:
//
//   1. C++ side — `extract(path, size)` runs synchronously on a
//      worker thread, dispatches to a backend by file extension,
//      returns a QImage (null if no backend handles the path or
//      the backend failed). UfbThumbProvider calls this from
//      QThreadPool::globalInstance().
//
//   2. QML side — `supports(path)` is a pure extension check, no
//      I/O. The grid delegate uses it to decide whether to
//      INSTANTIATE the thumb Image element (via Loader.active).
//      Files we can't thumbnail never make a request, never
//      cause delegate-recycle binding storms over the system icon.
//
// This is a SKELETON in step 2 of the migration: `supports`
// returns false for all paths and `extract` returns a null
// QImage. Backend registrations (QImageReader, GraphicsMagick,
// OpenEXR, PDFium, ffmpeg, .blend) land in steps 3-8.

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QtQml/qqmlregistration.h>

class Thumbnailer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit Thumbnailer(QObject* parent = nullptr);

    // QML-callable extension lookup. Pure function of `path`. Safe
    // from any thread. True when a UFB decoder backend can extract
    // this file — drives extract()'s dispatch.
    Q_INVOKABLE bool supports(const QString& path) const;

    // Broader QML-callable gate for the grid delegate's Loader.active.
    // Superset of supports(): also true for types the OS shell can
    // thumbnail even though UFB has no backend (RAW, Office, AVIF).
    // Pure extension check, no I/O — keeps the Loader from
    // instantiating an Image (and its delegate-recycle binding
    // storm) for files nothing can preview.
    Q_INVOKABLE bool mayHaveThumbnail(const QString& path) const;

    // C++-only synchronous extraction. Worker thread only — never
    // call from the QML thread. Dispatches by file extension.
    // Returns a null QImage when no backend matches or the backend
    // fails; callers translate that to "fall back to system icon".
    //
    // fullResPreview=false (default) caps every backend at 64 MP — the
    // thumbnail-grid policy. fullResPreview=true (the lightbox preview)
    // raises the cap via a ~1.5 GB decoded-buffer budget converted per
    // backend's worst-case bytes/pixel, so large EXR/PSB/TIFF preview
    // sharply while genuinely enormous files still decline gracefully.
    QImage extract(const QString& path, QSize requestedSize,
                   bool fullResPreview = false) const;

    // True for formats QImageReader decodes natively in-process
    // (jpg/png/gif/bmp/webp/tiff/…). The provider uses this to skip
    // the OS-shell stage-0 for these: the in-process QImageReader
    // DCT-skip path is faster than a shell/QuickLook IPC round-trip
    // and immune to a hung thumbnaild / shell handler. Pure extension
    // check, safe from any thread.
    bool isQtNative(const QString& path) const;
};
