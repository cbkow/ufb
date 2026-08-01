#include "HeifBackend.h"

#include <QFile>

#ifdef UFB_HAVE_LIBHEIF
#include <libheif/heif.h>
#include <cstring>
#endif

namespace ufb {

#ifndef UFB_HAVE_LIBHEIF

QImage decodeHeif(const QString& path, QSize, qint64) {
    qWarning("HeifBackend: built without libheif — cannot decode %s",
             qPrintable(path));
    return QImage();
}

#else

namespace {

// One-time library init held for the process lifetime. The vcpkg
// build has dynamic plugin loading enabled, and its default plugin
// path resolves next to the exe — so heif_init tries LoadLibrary on
// every co-located DLL (Qt, ffmpeg, ~70 of them). Worse, ONE failed
// load makes heif_init return an error BEFORE incrementing its init
// counter (libheif init.cc), so every heif_context_alloc re-runs the
// whole scan: ~70 LoadLibrary attempts per thumbnail.
//
// Fix: point LIBHEIF_PLUGIN_PATH at a directory that cannot contain
// DLLs. Empty scan → heif_init returns ok → counter increments →
// later context allocs skip plugin loading entirely. Must be a
// NON-EMPTY value: qputenv(name, "") deletes the variable on Windows
// and libheif falls back to the compiled-in default path. The codec
// we need (libde265) is statically registered, not a plugin.
struct HeifLibInit {
    HeifLibInit() {
        qputenv("LIBHEIF_PLUGIN_PATH",
                QByteArrayLiteral("/nonexistent-ufb-heif-plugins"));
        heif_init(nullptr);
    }
    ~HeifLibInit() { heif_deinit(); }
};

void ensureHeifInit() {
    static HeifLibInit init;  // thread-safe magic static
}

struct CtxGuard {
    heif_context* p = nullptr;
    ~CtxGuard() { if (p) heif_context_free(p); }
};
struct HandleGuard {
    heif_image_handle* p = nullptr;
    ~HandleGuard() { if (p) heif_image_handle_release(p); }
};
struct ImgGuard {
    heif_image* p = nullptr;
    ~ImgGuard() { if (p) heif_image_release(p); }
};

// Decode one image handle to a QImage, downscaled to fit
// `requestedSize`. heif_decode_image applies the file's irot/imir
// orientation transforms and composes tiled (grid) images — the
// iPhone layout — before we ever see pixels.
QImage decodeHandle(heif_image_handle* handle, QSize requestedSize) {
    // RGB for opaque content so ThumbCache's hasAlphaChannel() check
    // picks the (much smaller) JPEG cache encoding for photos; RGBA
    // only when the file actually carries alpha.
    const bool alpha = heif_image_handle_has_alpha_channel(handle) != 0;

    ImgGuard img;
    heif_error err = heif_decode_image(
        handle, &img.p, heif_colorspace_RGB,
        alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
        nullptr);
    if (err.code != heif_error_Ok || !img.p) {
        qWarning("HeifBackend: decode failed: %s",
                 err.message ? err.message : "(no message)");
        return QImage();
    }

    const int w = heif_image_get_width(img.p, heif_channel_interleaved);
    const int h = heif_image_get_height(img.p, heif_channel_interleaved);
    if (w <= 0 || h <= 0) return QImage();

    int stride = 0;
    const uint8_t* data =
        heif_image_get_plane_readonly(img.p, heif_channel_interleaved, &stride);
    if (!data || stride <= 0) return QImage();

    QImage out(w, h, alpha ? QImage::Format_RGBA8888
                           : QImage::Format_RGB888);
    if (out.isNull()) return QImage();
    const int rowBytes = w * (alpha ? 4 : 3);
    for (int y = 0; y < h; ++y) {
        std::memcpy(out.scanLine(y), data + qint64(y) * stride,
                    size_t(rowBytes));
    }

    if (requestedSize.isValid()
        && requestedSize.width() > 0 && requestedSize.height() > 0
        && (w > requestedSize.width() || h > requestedSize.height())) {
        out = out.scaled(requestedSize, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    }
    return out;
}

} // namespace

QImage decodeHeif(const QString& path, QSize requestedSize, qint64 maxPixels) {
    qInfo("HeifBackend: start path=%s", qPrintable(path));
    ensureHeifInit();

    // Read through QFile instead of heif_context_read_from_file:
    // libheif opens the char* path with the ANSI codepage on Windows,
    // so non-ASCII job-folder paths would fail. Files are a few MB;
    // the buffer must outlive the context (read_from_memory keeps
    // referencing it during decode).
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("HeifBackend: cannot open %s", qPrintable(path));
        return QImage();
    }
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.isEmpty()) return QImage();

    CtxGuard ctx;
    ctx.p = heif_context_alloc();
    if (!ctx.p) return QImage();
    heif_error err = heif_context_read_from_memory_without_copy(
        ctx.p, bytes.constData(), size_t(bytes.size()), nullptr);
    if (err.code != heif_error_Ok) {
        qWarning("HeifBackend: read failed for %s: %s", qPrintable(path),
                 err.message ? err.message : "(no message)");
        return QImage();
    }

    HandleGuard handle;
    err = heif_context_get_primary_image_handle(ctx.p, &handle.p);
    if (err.code != heif_error_Ok || !handle.p) {
        qWarning("HeifBackend: no primary image in %s", qPrintable(path));
        return QImage();
    }

    const int w = heif_image_handle_get_width(handle.p);
    const int h = heif_image_handle_get_height(handle.p);
    if (w <= 0 || h <= 0) return QImage();
    if (qint64(w) * qint64(h) > maxPixels) {
        qWarning("HeifBackend: %s is %dx%d (over %lld MP cap), skipping",
                 qPrintable(path), w, h,
                 (long long)(maxPixels / (1024 * 1024)));
        return QImage();
    }

    // Embedded-thumbnail shortcut: decoding the thmb item skips the
    // full tile-grid decode when it's already big enough for the
    // request. iPhone thumbs are ~320 px, so this fires for small
    // requests, not the 512 px cache master.
    if (requestedSize.isValid()
        && requestedSize.width() > 0 && requestedSize.height() > 0) {
        const int reqMax = qMax(requestedSize.width(), requestedSize.height());
        heif_item_id thumbId = 0;
        if (heif_image_handle_get_list_of_thumbnail_IDs(handle.p, &thumbId, 1)
            > 0) {
            HandleGuard thumb;
            if (heif_image_handle_get_thumbnail(handle.p, thumbId, &thumb.p)
                        .code == heif_error_Ok
                && thumb.p
                && qMax(heif_image_handle_get_width(thumb.p),
                        heif_image_handle_get_height(thumb.p)) >= reqMax) {
                QImage out = decodeHandle(thumb.p, requestedSize);
                if (!out.isNull()) {
                    qInfo("HeifBackend: ok path=%s -> %dx%d (embedded thumb)",
                          qPrintable(path), out.width(), out.height());
                    return out;
                }
            }
        }
    }

    QImage out = decodeHandle(handle.p, requestedSize);
    if (out.isNull()) {
        qWarning("HeifBackend: no image decoded from %s", qPrintable(path));
    } else {
        qInfo("HeifBackend: ok path=%s -> %dx%d",
              qPrintable(path), out.width(), out.height());
    }
    return out;
}

#endif  // UFB_HAVE_LIBHEIF

} // namespace ufb
