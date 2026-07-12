#include "PsdBackend.h"

// Phase 14 step 4 (revised again) - PSD/PSB via psd_sdk.
//
// History:
//   - GraphicsMagick: crashed inside Magick::InitializeMagick on
//     vcpkg build despite finding config files
//   - Windows WIC: requires a third-party PSD codec (Photoshop or
//     Adobe Camera Raw installs one); not standalone
//   - psd_sdk (Molecule, BSD-2): focused C++ PSD/PSB reader.
//     Pulled via FetchContent. Reads the merged composite section
//     Photoshop saves with "Maximize Compatibility" - which is on
//     by default for ~99% of PSDs in the wild.
//
// Cross-platform: psd_sdk has Win/Mac/Linux NativeFile impls so
// the same code works on macOS when the macOS port lands; no
// per-platform branching needed here.

#include "Psd.h"
#include "PsdMallocAllocator.h"
#include "PsdDocument.h"
#include "PsdParseDocument.h"
#include "PsdParseImageDataSection.h"
#include "PsdImageDataSection.h"
#include "PsdPlanarImage.h"
#include "PsdColorMode.h"

// macOS uses our SyncPsdFile (plain pread, no GCD). Other platforms
// keep ps::NativeFile — Windows uses overlapped I/O with explicit
// handles, which doesn't have the UAF that the macOS async-block
// implementation has.
#ifdef __APPLE__
#  include "SyncPsdFile.h"
   using PsdFileImpl = ufb::SyncPsdFile;
#else
#  include "PsdNativeFile.h"
   using PsdFileImpl = PSD_NAMESPACE::NativeFile;
#endif

#include <QtCore/QtMath>
#include <cstdint>

namespace ufb {

namespace {

namespace ps = PSD_NAMESPACE;

// Interleave planar 8-bit channels into RGBA. PSD files store
// channels separately (all R, then all G, etc.); QImage wants
// them interleaved as RGBA per pixel.
void interleavePlanarRgba8(const uint8_t* r, const uint8_t* g,
                           const uint8_t* b, const uint8_t* a,
                           uint8_t* dst, size_t pixelCount) {
    for (size_t i = 0; i < pixelCount; ++i) {
        dst[i * 4 + 0] = r[i];
        dst[i * 4 + 1] = g[i];
        dst[i * 4 + 2] = b[i];
        dst[i * 4 + 3] = a ? a[i] : 0xFF;
    }
}

// 16-bit per channel -> 8-bit downconvert + interleave. PSD
// stores 16-bit values big-endian on disk but psd_sdk has
// already byte-swapped them in memory. Downconvert with >>8
// (drops the low byte, which is the right-shift truncation -
// good enough for thumbnails).
void interleavePlanarRgba16(const uint16_t* r, const uint16_t* g,
                            const uint16_t* b, const uint16_t* a,
                            uint8_t* dst, size_t pixelCount) {
    for (size_t i = 0; i < pixelCount; ++i) {
        dst[i * 4 + 0] = uint8_t(r[i] >> 8);
        dst[i * 4 + 1] = uint8_t(g[i] >> 8);
        dst[i * 4 + 2] = uint8_t(b[i] >> 8);
        dst[i * 4 + 3] = a ? uint8_t(a[i] >> 8) : 0xFF;
    }
}

// 8-bit CMYK -> RGB conversion. PSD stores CMYK *inverted*:
// channel value 0 = 100% ink, 255 = no ink. The Adobe-naive
// (no ICC profile) conversion below treats the inverted bytes
// as already representing (255 - ink), so we just multiply
// CMY by K to model the black-modulated CMY-to-RGB mapping:
//   R = (Cinv * Kinv) / 255
//   G = (Minv * Kinv) / 255
//   B = (Yinv * Kinv) / 255
// Not colour-accurate but fine for a thumbnail. CMYK files in
// the wild for thumbnailing purposes are usually rare prints
// where the user mostly cares "what's in this file".
void convertCmykToRgba8(const uint8_t* c, const uint8_t* m,
                        const uint8_t* y, const uint8_t* k,
                        uint8_t* dst, size_t pixelCount) {
    for (size_t i = 0; i < pixelCount; ++i) {
        const unsigned ci = c[i];
        const unsigned mi = m[i];
        const unsigned yi = y[i];
        const unsigned ki = k[i];
        dst[i * 4 + 0] = uint8_t((ci * ki) / 255);
        dst[i * 4 + 1] = uint8_t((mi * ki) / 255);
        dst[i * 4 + 2] = uint8_t((yi * ki) / 255);
        dst[i * 4 + 3] = 0xFF;
    }
}

} // namespace

QImage decodePsd(const QString& path, QSize requestedSize, qint64 maxPixels) {
    qInfo("PsdBackend: start path=%s", qPrintable(path));

    ps::MallocAllocator allocator;
    PsdFileImpl file(&allocator);

    if (!file.OpenRead(path.toStdWString().c_str())) {
        qWarning("PsdBackend: OpenRead failed for %s", qPrintable(path));
        return QImage();
    }

    ps::Document* doc = ps::CreateDocument(&file, &allocator);
    if (!doc) {
        qWarning("PsdBackend: CreateDocument returned null for %s",
                 qPrintable(path));
        file.Close();
        return QImage();
    }

    // Need the merged composite. PSDs without "Maximize Compatibility"
    // skip this section, in which case we'd have to composite layers
    // ourselves - too much work for a thumbnail. Bail and fall back
    // to system icon.
    if (doc->imageDataSection.length == 0) {
        qWarning("PsdBackend: no merged composite in %s "
                 "(saved without Maximize Compatibility)",
                 qPrintable(path));
        ps::DestroyDocument(doc, &allocator);
        file.Close();
        return QImage();
    }

    // Supported: 8/16-bit RGB(A), 8-bit CMYK. 32-bit float, Lab,
    // duotone, indexed, multichannel, grayscale all skipped for
    // now - they're rare in practice and need format-specific
    // handling. CMYK uses the naive Adobe formula (no ICC).
    const bool isRgb  = doc->colorMode == ps::colorMode::RGB;
    const bool isCmyk = doc->colorMode == ps::colorMode::CMYK;
    const bool supportedDepth = (doc->bitsPerChannel == 8
                                 || doc->bitsPerChannel == 16);
    const bool supported = (isRgb && supportedDepth)
                        || (isCmyk && doc->bitsPerChannel == 8);
    if (!supported) {
        qWarning("PsdBackend: skip %s - bpc=%u colorMode=%u "
                 "(supported: 8/16-bit RGB, 8-bit CMYK)",
                 qPrintable(path), doc->bitsPerChannel,
                 unsigned(doc->colorMode));
        ps::DestroyDocument(doc, &allocator);
        file.Close();
        return QImage();
    }

    // Size guard BEFORE parsing/allocating the composite: a huge PSB would
    // otherwise allocate w·h·4 bytes (plus the SDK's intermediate channel
    // buffers) and OOM. The ceiling is the caller's maxPixels — 64 MP for
    // thumbnails, a larger byte-budgeted value for the full-res preview.
    if (qint64(doc->width) * qint64(doc->height) > maxPixels) {
        qWarning("PsdBackend: %s is %ux%u (over %lld MP cap), skipping",
                 qPrintable(path), doc->width, doc->height,
                 (long long)(maxPixels / (1024 * 1024)));
        ps::DestroyDocument(doc, &allocator);
        file.Close();
        return QImage();
    }

    ps::ImageDataSection* imageData =
        ps::ParseImageDataSection(doc, &file, &allocator);
    if (!imageData) {
        qWarning("PsdBackend: ParseImageDataSection failed for %s",
                 qPrintable(path));
        ps::DestroyDocument(doc, &allocator);
        file.Close();
        return QImage();
    }

    const unsigned int w = doc->width;
    const unsigned int h = doc->height;
    const size_t pixelCount = size_t(w) * size_t(h);

    QImage img(int(w), int(h), QImage::Format_RGBA8888);
    if (img.isNull()) {
        ps::DestroyImageDataSection(imageData, &allocator);
        ps::DestroyDocument(doc, &allocator);
        file.Close();
        return QImage();
    }

    if (isCmyk) {
        // CMYK has 4 mandatory channels (C, M, Y, K). PSD stores
        // them inverted - 0=full ink, 255=no ink.
        if (imageData->imageCount < 4) {
            qWarning("PsdBackend: CMYK %s has only %u channels",
                     qPrintable(path), imageData->imageCount);
            ps::DestroyImageDataSection(imageData, &allocator);
            ps::DestroyDocument(doc, &allocator);
            file.Close();
            return QImage();
        }
        const uint8_t* c = static_cast<const uint8_t*>(imageData->images[0].data);
        const uint8_t* m = static_cast<const uint8_t*>(imageData->images[1].data);
        const uint8_t* y = static_cast<const uint8_t*>(imageData->images[2].data);
        const uint8_t* k = static_cast<const uint8_t*>(imageData->images[3].data);
        convertCmykToRgba8(c, m, y, k, img.bits(), pixelCount);
    } else {
        const bool hasAlpha = imageData->imageCount >= 4;
        if (doc->bitsPerChannel == 8) {
            const uint8_t* r = static_cast<const uint8_t*>(imageData->images[0].data);
            const uint8_t* g = static_cast<const uint8_t*>(imageData->images[1].data);
            const uint8_t* b = static_cast<const uint8_t*>(imageData->images[2].data);
            const uint8_t* a = hasAlpha
                ? static_cast<const uint8_t*>(imageData->images[3].data) : nullptr;
            interleavePlanarRgba8(r, g, b, a, img.bits(), pixelCount);
        } else { // 16-bit RGB(A)
            const uint16_t* r = static_cast<const uint16_t*>(imageData->images[0].data);
            const uint16_t* g = static_cast<const uint16_t*>(imageData->images[1].data);
            const uint16_t* b = static_cast<const uint16_t*>(imageData->images[2].data);
            const uint16_t* a = hasAlpha
                ? static_cast<const uint16_t*>(imageData->images[3].data) : nullptr;
            interleavePlanarRgba16(r, g, b, a, img.bits(), pixelCount);
        }
    }

    ps::DestroyImageDataSection(imageData, &allocator);
    ps::DestroyDocument(doc, &allocator);
    file.Close();

    // Resize for the thumbnail. Smooth scale - the source is
    // already in RAM so cost is bounded, and we've already done
    // the heavy parse work.
    if (requestedSize.isValid()
        && requestedSize.width() > 0
        && requestedSize.height() > 0
        && (int(w) > requestedSize.width()
            || int(h) > requestedSize.height())) {
        img = img.scaled(requestedSize, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    }

    qInfo("PsdBackend: ok path=%s -> %dx%d",
          qPrintable(path), img.width(), img.height());
    return img;
}

} // namespace ufb
