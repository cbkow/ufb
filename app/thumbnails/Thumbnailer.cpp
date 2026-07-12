#include "Thumbnailer.h"
#include "PsdBackend.h"
#include "ExrBackend.h"
#include "HdrBackend.h"
#include "PdfBackend.h"
#include "VideoBackend.h"

#include <QFileInfo>
#include <QImageReader>
#include <QSet>

// Backend roster.
//
// `kQtNativeExts` covers everything QImageReader can decode with
// the Qt 6 image format plugin pack that ships with Qt 6.11
// (qjpeg/qgif/qico/qsvg/qtga/qtiff/qwbmp/qwebp/qicns) plus PNG
// and BMP which are built into QtGui. The `extract` path uses
// QImageReader::setScaledSize for cheap downsampling - much
// cheaper than decoding full-res then scaling, especially for
// large source files.
//
// Other backends (PSD via GraphicsMagick, EXR via OpenEXR,
// PDF via PDFium, video via ffmpeg, .blend custom) land in
// later steps and add their extensions to the supports() set.

static const QSet<QString>& qtNativeExts() {
    static const QSet<QString> s {
        "jpg", "jpeg", "jpe", "jfif",
        "png", "gif", "bmp", "ico", "icns",
        "svg",
        "webp", "tiff", "tif",
        "tga", "wbmp",
    };
    return s;
}

static const QSet<QString>& psdExts() {
    static const QSet<QString> s { "psd", "psb" };
    return s;
}

static const QSet<QString>& exrExts() {
    static const QSet<QString> s { "exr" };
    return s;
}

static const QSet<QString>& hdrExts() {
    // Greg Ward's Radiance RGBE format. Some renders save as .rgbe.
    static const QSet<QString> s { "hdr", "rgbe" };
    return s;
}

static const QSet<QString>& pdfExts() {
    // .ai files are PDF containers; PDFium reads them as PDFs.
    static const QSet<QString> s { "pdf", "ai" };
    return s;
}

static const QSet<QString>& videoExts() {
    static const QSet<QString> s {
        "mp4", "m4v", "mov", "qt",
        "mkv", "webm", "ogv",
        "avi", "wmv",
        "mpg", "mpeg", "ts", "m2ts", "mts",
        "mxf", "flv", "3gp", "3g2",
    };
    return s;
}

static const QSet<QString>& osThumbnailExts() {
    // File types the OS shell can thumbnail but UFB has no backend
    // for. Used ONLY to widen the grid Loader gate (mayHaveThumbnail);
    // extract() never dispatches to these — a null extract() result
    // is fine because the OS-first stage already produced the thumb.
    static const QSet<QString> s {
        // RAW — Windows Raw Image Extension / macOS QuickLook.
        "cr2", "cr3", "nef", "arw", "dng", "raf", "orf", "rw2", "pef", "srw",
        // Office — shell thumbnail handlers.
        "doc", "docx", "xls", "xlsx", "ppt", "pptx",
        // Misc shell-thumbnailed formats.
        "heic", "heif", "avif",
    };
    return s;
}

static QString lowerExt(const QString& path) {
    return QFileInfo(path).suffix().toLower();
}

Thumbnailer::Thumbnailer(QObject* parent) : QObject(parent) {}

bool Thumbnailer::supports(const QString& path) const {
    if (path.isEmpty()) return false;
    const QString ext = lowerExt(path);
    if (ext.isEmpty()) return false;
    if (qtNativeExts().contains(ext)) return true;
    if (psdExts().contains(ext)) return true;
    if (exrExts().contains(ext)) return true;
    if (hdrExts().contains(ext)) return true;
    if (pdfExts().contains(ext)) return true;
    if (videoExts().contains(ext)) return true;
    return false;
}

bool Thumbnailer::isQtNative(const QString& path) const {
    if (path.isEmpty()) return false;
    return qtNativeExts().contains(lowerExt(path));
}

bool Thumbnailer::mayHaveThumbnail(const QString& path) const {
    // Everything a UFB backend handles, plus the OS-shell-only set.
    if (supports(path)) return true;
    const QString ext = lowerExt(path);
    if (ext.isEmpty()) return false;
    return osThumbnailExts().contains(ext);
}

QImage Thumbnailer::extract(const QString& path, QSize requestedSize,
                            bool fullResPreview) const {
    if (path.isEmpty()) return QImage();
    const QString ext = lowerExt(path);

    // Decode-size ceiling. Thumbnails cap uniformly at 64 MP. The full-res
    // lightbox preview raises the cap via a ~1.5 GB decoded-buffer budget,
    // converted to a pixel count per backend's worst-case bytes/pixel, so a
    // large EXR/PSB/TIFF still previews while a truly enormous file declines
    // gracefully (→ shell thumbnail / file icon) instead of OOM-ing.
    constexpr qint64 kThumbMaxPixels = 64LL * 1024 * 1024;
    constexpr qint64 kPreviewBudget  = 1536LL * 1024 * 1024;  // ~1.5 GB
    auto capForBpp = [&](int bytesPerPixel) -> qint64 {
        return fullResPreview ? (kPreviewBudget / bytesPerPixel) : kThumbMaxPixels;
    };

    if (qtNativeExts().contains(ext)) {
        QImageReader reader(path);
        reader.setAutoTransform(true);  // honor EXIF orientation
        // Pre-scale at decode time. JPEG decoders use this to
        // skip DCT levels, so a 4000x4000 JPEG decodes to 256x256
        // far faster than decoding then scaling.
        const QSize src = reader.size();
        qInfo("Thumbnailer[qtNative]: start path=%s src=%dx%d (valid=%d)",
              qPrintable(path), src.width(), src.height(), src.isValid());
        // Sanity guard. Most Qt image plugins (notably libtiff) ignore
        // setScaledSize and decode the full source into RAM before scaling,
        // so a 16K×8K TIFF allocates ~512 MB just for the source buffer and
        // crashes the process. Cap at maxQtNativePixels — 64 MP for
        // thumbnails, a larger byte-budgeted value for the full-res preview
        // (worst-case ~8 bytes/px: plugins decode to ARGB32 and may
        // double-buffer).
        const qint64 maxQtNativePixels = capForBpp(8);
        if (src.isValid()
            && qint64(src.width()) * qint64(src.height()) > maxQtNativePixels) {
            qWarning("Thumbnailer: %s is %dx%d (over %lld MP cap), skipping",
                     qPrintable(path), src.width(), src.height(),
                     (long long)(maxQtNativePixels / (1024 * 1024)));
            return QImage();
        }
        // Belt-and-suspenders: if the plugin couldn't report a size
        // (unsupported variant, header parse failure), check on-disk file
        // size — a giant uncompressed pixel dump with unknown dims would OOM.
        // ~256 MB for thumbnails; raised for the full-res preview.
        if (!src.isValid()) {
            const qint64 fileBytes = QFileInfo(path).size();
            const qint64 fileCap = fullResPreview ? 1536LL * 1024 * 1024
                                                  : 256LL * 1024 * 1024;
            if (fileBytes > fileCap) {
                qWarning("Thumbnailer: %s has unknown dims and is %lld bytes (over cap), skipping",
                         qPrintable(path), (long long)fileBytes);
                return QImage();
            }
        }
        if (src.isValid() && requestedSize.isValid()
            && (requestedSize.width() > 0 || requestedSize.height() > 0)) {
            const QSize tgt = src.scaled(requestedSize, Qt::KeepAspectRatio);
            if (tgt.width() > 0 && tgt.height() > 0
                && (tgt.width() < src.width() || tgt.height() < src.height())) {
                reader.setScaledSize(tgt);
            }
        }
        return reader.read();
    }

    if (psdExts().contains(ext)) {
        // Composite is RGBA8888 (4 bpp) but the SDK holds intermediate
        // 16-bit channel buffers; budget at 8 bpp.
        return ufb::decodePsd(path, requestedSize, capForBpp(8));
    }

    if (exrExts().contains(ext)) {
        // OpenEXR reads the full data window as half-float RGBA (8 bpp) and
        // we convert via a float intermediate; budget at 16 bpp.
        return ufb::decodeExr(path, requestedSize, capForBpp(16));
    }

    if (hdrExts().contains(ext)) {
        // stb decodes to float RGB(A); budget at 16 bpp.
        return ufb::decodeHdr(path, requestedSize, capForBpp(16));
    }

    if (pdfExts().contains(ext)) {
        return ufb::decodePdf(path, requestedSize);
    }

    if (videoExts().contains(ext)) {
        return ufb::decodeVideo(path, requestedSize);
    }

    return QImage();
}
