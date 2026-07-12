#include "ExrBackend.h"

// Reference for the loader pattern:
// QCView-Player/src/decode/exr_image_loader.cpp::EXRImageLoader.
//
// Default/empty layer → RgbaInputFile (auto-converts any channel layout,
// incl. luminance-chroma, to RGBA — part 0). A named layer → the
// MultiPartInputFile + "<layer>.R/G/B/A" reader ported from QCView (bare
// R/G/B/A fallback, opaque-alpha synthesis, displayWindow≠dataWindow
// handling). Both tone-map clamp + gamma-2.2 to 8-bit for display.

#include <ImfRgbaFile.h>
#include <ImfArray.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImathBox.h>
#include <half.h>

#include <QtCore/QtMath>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ufb {

namespace {

// Linear scene-referred float -> 8-bit display value. Clamp,
// then apply 2.2 gamma. Approximates what RV/DJV show by default
// for an unflagged EXR thumbnail. Real colour pipelines use OCIO
// for view transforms - overkill for a file browser.
inline uint8_t toneMap(float v) {
    if (!std::isfinite(v) || v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    const float gamma = std::pow(v, 1.0f / 2.2f);
    return uint8_t(gamma * 255.0f + 0.5f);
}

// Cryptomatte filter (QCView exr_image_loader.cpp:364) — case-insensitive
// substring "crypto". These carry hash IDs, not displayable image data.
bool isCryptomatte(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower.find("crypto") != std::string::npos;
}

// Locate the part in a multi-part EXR whose header name matches `layer`.
// Returns 0 (first part) for single-part files or no match.
int partIndexForLayer(const Imf::MultiPartInputFile& file,
                      const std::string& layer) {
    if (layer.empty()) return 0;
    const int parts = file.parts();
    if (parts <= 1) return 0;
    for (int p = 0; p < parts; ++p) {
        const Imf::Header& h = file.header(p);
        if (h.hasName() && h.name() == layer) return p;
    }
    return 0;
}

// The original RGBA path (unchanged behaviour): RgbaInputFile auto-converts
// whatever channel layout the file has into half-float RGBA, including
// luminance-chroma reconstruction. Used for the default/beauty view.
QImage decodeExrRgba(const QString& path, QSize requestedSize, qint64 maxPixels) {
    Imf::RgbaInputFile file(path.toStdString().c_str());

    const Imath::Box2i dw = file.dataWindow();
    const int w = dw.max.x - dw.min.x + 1;
    const int h = dw.max.y - dw.min.y + 1;
    if (w <= 0 || h <= 0) {
        qWarning("ExrBackend: empty data window for %s", qPrintable(path));
        return QImage();
    }
    if (qint64(w) * qint64(h) > maxPixels) {
        qWarning("ExrBackend: %s is %dx%d (over %lld MP cap), skipping",
                 qPrintable(path), w, h, (long long)(maxPixels / (1024 * 1024)));
        return QImage();
    }

    Imf::Array2D<Imf::Rgba> pixels(h, w);
    file.setFrameBuffer(&pixels[0][0] - dw.min.x - dw.min.y * w, 1, w);
    file.readPixels(dw.min.y, dw.max.y);

    QImage img(w, h, QImage::Format_RGBA8888);
    if (img.isNull()) return QImage();
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        const Imf::Rgba* src = pixels[y];
        for (int x = 0; x < w; ++x) {
            line[x * 4 + 0] = toneMap(float(src[x].r));
            line[x * 4 + 1] = toneMap(float(src[x].g));
            line[x * 4 + 2] = toneMap(float(src[x].b));
            line[x * 4 + 3] = toneMap(float(src[x].a));
        }
    }
    if (requestedSize.isValid() && requestedSize.width() > 0
        && requestedSize.height() > 0
        && (w > requestedSize.width() || h > requestedSize.height())) {
        img = img.scaled(requestedSize, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    }
    return img;
}

// Named-layer path (ported from QCView EXRImageLoader::loadFrame): read
// "<layer>.R/G/B/A" of the matching part, falling back to bare R/G/B/A.
QImage decodeExrLayer(const QString& path, QSize requestedSize,
                      qint64 maxPixels, const std::string& layer) {
    Imf::MultiPartInputFile file(path.toStdString().c_str());

    const int targetPart = partIndexForLayer(file, layer);
    const Imf::Header& header = file.header(targetPart);
    const Imath::Box2i dispWin = header.displayWindow();
    const Imath::Box2i dataWin = header.dataWindow();
    const bool fastPath = (dispWin == dataWin);

    const int width  = dispWin.max.x - dispWin.min.x + 1;
    const int height = dispWin.max.y - dispWin.min.y + 1;
    if (width <= 0 || height <= 0) return QImage();
    if (qint64(width) * qint64(height) > maxPixels) {
        qWarning("ExrBackend: %s layer '%s' is %dx%d (over cap), skipping",
                 qPrintable(path), layer.c_str(), width, height);
        return QImage();
    }

    // Interleaved RGBA half destination.
    std::vector<Imath::half> halves(
        static_cast<std::size_t>(width) * height * 4);

    // Channel discovery: "<layer>.R/G/B/A" first; bare "R/G/B/A" fallback.
    const Imf::ChannelList& channels = header.channels();
    const std::string prefix = layer.empty() ? std::string() : layer + ".";
    std::string nR = prefix + "R", nG = prefix + "G",
                nB = prefix + "B", nA = prefix + "A";
    const Imf::Channel* cR = channels.findChannel(nR.c_str());
    const Imf::Channel* cG = channels.findChannel(nG.c_str());
    const Imf::Channel* cB = channels.findChannel(nB.c_str());
    const Imf::Channel* cA = channels.findChannel(nA.c_str());
    if (!cR && !layer.empty()) {
        nR = "R"; nG = "G"; nB = "B"; nA = "A";
        cR = channels.findChannel("R");
        cG = channels.findChannel("G");
        cB = channels.findChannel("B");
        cA = channels.findChannel("A");
    }
    if (!cR || !cG || !cB) {
        qWarning("ExrBackend: missing RGB channels in '%s' (layer='%s')",
                 qPrintable(path), layer.c_str());
        return QImage();
    }
    const bool hasAlpha = (cA != nullptr);
    const std::string nameByCh[4] = { nR, nG, nB, nA };
    const int numCh = hasAlpha ? 4 : 3;

    char* base = reinterpret_cast<char*>(halves.data());
    const std::size_t channelBytes = sizeof(Imath::half);
    const std::size_t cb  = 4 * channelBytes;                  // per-pixel
    const std::size_t scb = static_cast<std::size_t>(width) * cb; // per-row

    Imf::FrameBuffer fb;
    Imf::InputPart part(file, targetPart);

    if (fastPath) {
        for (int c = 0; c < numCh; ++c) {
            fb.insert(nameByCh[c].c_str(),
                Imf::Slice(Imf::HALF, base + (c * channelBytes),
                           cb, scb, 1, 1, 0.0f));
        }
        if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i)
                halves[i * 4 + 3] = Imath::half(1.0f);
        }
        part.setFrameBuffer(fb);
        part.readPixels(dispWin.min.y, dispWin.max.y);
    } else {
        // displayWindow ≠ dataWindow: read scanline-by-scanline into a
        // dataWindow-keyed temp row, memcpy the intersection into the
        // destination, zero-fill the gaps.
        const Imath::Box2i isect(
            Imath::V2i(std::max(dispWin.min.x, dataWin.min.x),
                       std::max(dispWin.min.y, dataWin.min.y)),
            Imath::V2i(std::min(dispWin.max.x, dataWin.max.x),
                       std::min(dispWin.max.y, dataWin.max.y)));
        const int dataWidth = dataWin.max.x - dataWin.min.x + 1;
        std::vector<char> buf(static_cast<std::size_t>(dataWidth) * cb);
        for (int c = 0; c < numCh; ++c) {
            fb.insert(nameByCh[c].c_str(),
                Imf::Slice(Imf::HALF,
                    buf.data() - (static_cast<std::ptrdiff_t>(dataWin.min.x) * cb)
                               + (c * channelBytes),
                    cb, 0, 1, 1, 0.0f));
        }
        part.setFrameBuffer(fb);
        for (int y = dispWin.min.y; y <= dispWin.max.y; ++y) {
            char* p = base + static_cast<std::size_t>(y - dispWin.min.y) * scb;
            char* end = p + scb;
            if (y >= isect.min.y && y <= isect.max.y) {
                const std::size_t leftPad =
                    static_cast<std::size_t>(isect.min.x - dispWin.min.x) * cb;
                std::memset(p, 0, leftPad);
                p += leftPad;
                const std::size_t copyBytes =
                    static_cast<std::size_t>(isect.max.x - isect.min.x + 1) * cb;
                part.readPixels(y, y);
                std::memcpy(p,
                    buf.data() + std::max(dispWin.min.x - dataWin.min.x, 0) * cb,
                    copyBytes);
                p += copyBytes;
            }
            std::memset(p, 0, end - p);
        }
        if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i)
                halves[i * 4 + 3] = Imath::half(1.0f);
        }
    }

    // Tone-map the half RGBA into a display QImage.
    QImage img(width, height, QImage::Format_RGBA8888);
    if (img.isNull()) return QImage();
    for (int y = 0; y < height; ++y) {
        uchar* line = img.scanLine(y);
        const Imath::half* src = halves.data()
            + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            line[x * 4 + 0] = toneMap(float(src[x * 4 + 0]));
            line[x * 4 + 1] = toneMap(float(src[x * 4 + 1]));
            line[x * 4 + 2] = toneMap(float(src[x * 4 + 2]));
            line[x * 4 + 3] = toneMap(float(src[x * 4 + 3]));
        }
    }
    if (requestedSize.isValid() && requestedSize.width() > 0
        && requestedSize.height() > 0
        && (width > requestedSize.width() || height > requestedSize.height())) {
        img = img.scaled(requestedSize, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    }
    return img;
}

} // namespace

QImage decodeExr(const QString& path, QSize requestedSize, qint64 maxPixels,
                 const QString& layer) {
    qInfo("ExrBackend: start path=%s layer=%s",
          qPrintable(path), qPrintable(layer));
    try {
        const std::string l = layer.toStdString();
        QImage img = (l.empty() || l == "default")
            ? decodeExrRgba(path, requestedSize, maxPixels)
            : decodeExrLayer(path, requestedSize, maxPixels, l);
        if (!img.isNull())
            qInfo("ExrBackend: ok path=%s -> %dx%d",
                  qPrintable(path), img.width(), img.height());
        return img;
    } catch (const std::exception& e) {
        qWarning("ExrBackend: %s failed: %s", qPrintable(path), e.what());
        return QImage();
    } catch (...) {
        qWarning("ExrBackend: unknown exception for %s", qPrintable(path));
        return QImage();
    }
}

QStringList listExrLayers(const QString& path) {
    QStringList out;
    try {
        Imf::MultiPartInputFile file(path.toStdString().c_str());
        const int parts = file.parts();

        // Multi-part: each part's name() is a selectable layer.
        if (parts > 1) {
            for (int p = 0; p < parts; ++p) {
                const Imf::Header& h = file.header(p);
                const std::string name =
                    (h.hasName() && !h.name().empty())
                    ? h.name()
                    : ("default (part " + std::to_string(p) + ")");
                if (isCryptomatte(name)) continue;
                out << QString::fromStdString(name);
            }
            return out;
        }

        // Single-part: layer = channel name before the LAST dot; bare
        // channels (no dot) → "default". Dedupe in discovery order.
        const Imf::ChannelList& channels = file.header(0).channels();
        std::vector<std::string> uniq;
        bool sawBare = false;
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            const std::string n = it.name();
            const std::size_t dot = n.find_last_of('.');
            if (dot == std::string::npos) { sawBare = true; continue; }
            const std::string prefix = n.substr(0, dot);
            if (isCryptomatte(prefix)) continue;
            if (std::find(uniq.begin(), uniq.end(), prefix) == uniq.end())
                uniq.push_back(prefix);
        }
        if (sawBare) out << QStringLiteral("default");
        for (const auto& p : uniq) out << QString::fromStdString(p);
        if (out.isEmpty()) out << QStringLiteral("default");
        return out;
    } catch (const std::exception& e) {
        qWarning("ExrBackend: listExrLayers %s failed: %s",
                 qPrintable(path), e.what());
        return {};
    } catch (...) {
        return {};
    }
}

} // namespace ufb
