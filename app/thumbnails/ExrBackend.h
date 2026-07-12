#pragma once

// Phase 14 step 5 - EXR thumbnail extraction via OpenEXR.
//
// Handles single-part RGBA EXRs and the common case of multi-part
// scanline files where the user just wants to see *something*
// (composites the first part). Multi-layer / deep / tiled
// production EXRs render via the same path - we just read the
// "RGBA" channels of the first part.
//
// Tone-map: linear scene-referred values are clamped + gamma-2.2
// encoded for display. Not colour-accurate but matches what
// thumbnails in DJV/RV/Nuke roughly look like.

#include <QImage>
#include <QString>
#include <QStringList>
#include <QSize>

namespace ufb {

// maxPixels caps the source area we'll decode (OpenEXR reads the full data
// window into a float buffer before scaling, so this bounds peak RAM). The
// default mirrors the thumbnail policy; the lightbox preview passes a larger,
// byte-budgeted value.
//
// `layer` selects an EXR layer (named sub-layer like "diffuse", or a
// multi-part part name). Empty / "default" uses the robust RgbaInputFile
// path (plain RGBA + luminance-chroma reconstruction, part 0). A named
// layer reads "<layer>.R/G/B/A" (with bare R/G/B/A fallback), mirroring
// QCView's EXRImageLoader. See listExrLayers() for enumeration.
QImage decodeExr(const QString& path, QSize requestedSize,
                 qint64 maxPixels = 64LL * 1024 * 1024,
                 const QString& layer = QString());

// Enumerate the selectable layers in an EXR (cheap header-only read).
// Multi-part files → each part's name ("default (part N)" if unnamed);
// single-part files → "default" (if bare R/G/B/A present) + each dotted
// channel-name prefix, in discovery order. Cryptomatte layers filtered.
// Always returns at least ["default"] for a valid EXR; empty on error.
QStringList listExrLayers(const QString& path);

} // namespace ufb
