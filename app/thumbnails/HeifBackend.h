#pragma once

// HEIF/HEIC still-image extraction via libheif (+ libde265 for HEVC).
//
// iPhone photos land in job folders both under their real .heic name
// and — routinely — renamed to .jpg/.jpeg by transfer tooling, so
// Thumbnailer routes here by extension AND by content sniff (ftyp
// brand). libheif composes the tile grid iPhone files use for the
// main image and applies irot/imir orientation transforms itself,
// which is why this backend exists instead of a hand-rolled ffmpeg
// tile compositor.
//
// Built against libheif only when vcpkg provides it
// (UFB_HAVE_LIBHEIF); otherwise decodeHeif compiles to a null-return
// stub and HEIC files fall back to the OS shell / file icon.

#include <QImage>
#include <QString>
#include <QSize>

namespace ufb {

// Decode the HEIF/HEIC at `path` and return a QImage at most
// `requestedSize`. Returns null QImage on failure (missing decoder,
// corrupt file, over the pixel cap); the caller translates that to
// "fall back to system icon".
// maxPixels caps the decoded area (checked against the primary
// image's dimensions before any pixel allocation). Default =
// thumbnail policy; preview passes a larger, byte-budgeted value.
QImage decodeHeif(const QString& path, QSize requestedSize,
                  qint64 maxPixels = 64LL * 1024 * 1024);

} // namespace ufb
