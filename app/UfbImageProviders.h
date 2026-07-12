// Qt-side image providers for thumbnails and OS-native file-type icons.
//
// Registers two URL schemes with the QML engine:
//   image://ufb-thumbs/<absolute-file-path>  — real file previews
//   image://ufb-icons/<extension>            — OS file-type icons
//
// Both subclass QQuickAsyncImageProvider so QML loads them off the
// GUI thread. Thumbnail requests dispatch a QRunnable onto a
// dedicated thumbnail thread pool (Thumbnailer::extract). Icon
// requests run on the global pool (ufb::extractSystemIcon) and are
// cached by extension, since list / tree views render hundreds of
// rows that share a handful of file types.

#pragma once

#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QSize>
#include <QString>

class UfbThumbnailProvider final : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse* requestImageResponse(const QString& id,
                                              const QSize& requestedSize) override;
};

class UfbIconProvider final : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse* requestImageResponse(const QString& id,
                                              const QSize& requestedSize) override;
};

// image://ufb-preview/<absolute-file-path> — FULL-resolution still preview
// for the in-app lightbox. Unlike ufb-thumbs (which caches a 512px master and
// downscales), this extracts at the requested view size so large images stay
// sharp. No persistent cache (one image alive at a time in the lightbox).
class UfbPreviewProvider final : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse* requestImageResponse(const QString& id,
                                              const QSize& requestedSize) override;
};

// image://ufb-pdf/<page>/<absolute-file-path> — renders one PDF page at the
// requested width for the lightbox's continuous-scroll reader. The leading
// integer (before the first '/') is the 0-based page index; the rest is the
// path (which itself starts with '/' on macOS, so a double slash is normal).
class UfbPdfProvider final : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse* requestImageResponse(const QString& id,
                                              const QSize& requestedSize) override;
};

// Renders a specific EXR layer (named sub-layer or multi-part part) for the
// lightbox's EXR layer grid + selected-layer full view. id is
// "<encodedLayer>/<encodedPath>" (both percent-encoded via encodeURIComponent
// on the QML side, so the layer never contains a raw '/').
class UfbExrLayerProvider final : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse* requestImageResponse(const QString& id,
                                              const QSize& requestedSize) override;
};
