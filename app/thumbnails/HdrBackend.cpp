#include "HdrBackend.h"

// stb_image is included with STB_IMAGE_IMPLEMENTATION in EXACTLY one
// translation unit (this one) so the symbols compile in. Other files
// that need stb_image stuff would include it without the macro.
//
// Disabling the formats stb_image already covers via other backends
// keeps our binary smaller and avoids accidental routing surprises
// (jpg/png/etc. go through QImageReader; .hdr is the only thing we
// actually need stb_image for).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#define STBI_NO_STDIO_OPEN  /* keep stb_image's fopen path; just no extras */
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ufb {

namespace {

// Linear scene-referred float -> 8-bit display value. Same shape as
// the EXR backend so the two HDR formats render consistently.
inline uint8_t toneMap(float v) {
    if (!std::isfinite(v) || v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    const float gamma = std::pow(v, 1.0f / 2.2f);
    return static_cast<uint8_t>(gamma * 255.0f + 0.5f);
}

} // namespace

QImage decodeHdr(const QString& path, QSize requestedSize, qint64 maxPixels) {
    qInfo("HdrBackend: start path=%s", qPrintable(path));

    int w = 0, h = 0, comp = 0;
    // Read header dimensions first so we can size-cap BEFORE allocating
    // the full float buffer. A 32K×16K Radiance file would have stb load
    // 32768·16384·3·4 ≈ 6 GB of float pixels and OOM the process before
    // we reached any post-load guard.
    if (!stbi_info(path.toUtf8().constData(), &w, &h, &comp)) {
        qWarning("HdrBackend: stbi_info failed for %s: %s",
                 qPrintable(path), stbi_failure_reason());
        return QImage();
    }
    if (w <= 0 || h <= 0) return QImage();
    // Sanity guard - caller's maxPixels (64 MP thumbnails; larger for the
    // full-res lightbox preview).
    if (qint64(w) * qint64(h) > maxPixels) {
        qWarning("HdrBackend: %s is %dx%d (over %lld MP cap), skipping",
                 qPrintable(path), w, h, (long long)(maxPixels / (1024 * 1024)));
        return QImage();
    }

    // stbi_loadf decodes Radiance RGBE into linear float RGB. `comp`
    // ends up 3 for normal HDR; pass 3 to force RGB if the file has
    // any other channel count.
    float* data = stbi_loadf(path.toUtf8().constData(), &w, &h, &comp, 3);
    if (!data) {
        qWarning("HdrBackend: stbi_loadf failed for %s: %s",
                 qPrintable(path), stbi_failure_reason());
        return QImage();
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(data);
        return QImage();
    }

    QImage img(w, h, QImage::Format_RGBA8888);
    if (img.isNull()) {
        stbi_image_free(data);
        return QImage();
    }

    // Tone-map RGB -> 8-bit, fill alpha with 255 (HDR has no alpha).
    const float* src = data;
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            line[x * 4 + 0] = toneMap(src[0]);
            line[x * 4 + 1] = toneMap(src[1]);
            line[x * 4 + 2] = toneMap(src[2]);
            line[x * 4 + 3] = 255;
            src += 3;
        }
    }
    stbi_image_free(data);

    if (requestedSize.isValid()
        && requestedSize.width() > 0
        && requestedSize.height() > 0
        && (w > requestedSize.width() || h > requestedSize.height())) {
        img = img.scaled(requestedSize, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    }

    qInfo("HdrBackend: ok path=%s -> %dx%d",
          qPrintable(path), img.width(), img.height());
    return img;
}

} // namespace ufb
