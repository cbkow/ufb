#pragma once

// Phase 14 step 4 - PSD/PSB thumbnail extraction via GraphicsMagick.
//
// GM's built-in PSD coder reads Photoshop and Photoshop-Big files
// and gives us the composited (flattened) bitmap. That's exactly
// what a file-browser thumbnail needs - we don't care about layers.
//
// Lives behind a small interface so the rest of Thumbnailer doesn't
// need to know about GraphicsMagick types or InitializeMagick().

#include <QImage>
#include <QString>
#include <QSize>

namespace ufb {

// Decode the PSD/PSB at `path` and return a QImage at most
// `requestedSize`. Returns null QImage on failure (file not
// found, corrupt, format not actually PSD, etc.); the caller
// translates that to "fall back to system icon".
// maxPixels caps the composite area we'll allocate/decode (the merged
// composite is read at full resolution before scaling, so this bounds peak
// RAM for huge PSBs). Default = thumbnail policy; preview passes a larger,
// byte-budgeted value.
QImage decodePsd(const QString& path, QSize requestedSize,
                 qint64 maxPixels = 64LL * 1024 * 1024);

} // namespace ufb
