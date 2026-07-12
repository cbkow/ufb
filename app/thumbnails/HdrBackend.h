#pragma once

// Phase 14 follow-up - Radiance HDR (.hdr / .rgbe) thumbnails.
//
// Greg Ward's Radiance RGBE format. Like EXR it's HDR linear scene-
// referred, so we tone-map down (clamp + gamma 2.2) the same way the
// EXR backend does, then return an 8-bit RGBA QImage at the requested
// size.
//
// Reader is stb_image (vendored single-header in this directory).
// MIT/public-domain, no link deps.

#include <QImage>
#include <QString>
#include <QSize>

namespace ufb {

// maxPixels caps the source area (stb_image decodes the full image into a
// float buffer). Default = thumbnail policy; preview passes a larger budget.
QImage decodeHdr(const QString& path, QSize requestedSize,
                 qint64 maxPixels = 64LL * 1024 * 1024);

} // namespace ufb
