#pragma once

// OS-native thumbnail extraction — stage 0 of the thumbnail pipeline.
//
// Asks the OS shell for a real thumbnail before UFB falls back to its
// own decoder backends (PSD / EXR / HDR / PDF / video / QImageReader).
//
//   Windows: IShellItemImageFactory::GetImage with SIIGBF_THUMBNAILONLY
//            — a genuine thumbnail or nothing. No file-type-icon
//            substitution; the icon comes from the separate
//            image://ufb-icons/ provider.
//   macOS:   QLThumbnailGenerator (added in a later phase).
//
// Returns a null QImage when the OS has no thumbnail for `path`; the
// caller then falls through to Thumbnailer::extract.

#include <QImage>
#include <QSize>
#include <QString>

namespace ufb {

// Extract the OS shell thumbnail for `path`, sized roughly to
// `requested`. Worker-thread only. Returns a null QImage if the OS
// can't produce a thumbnail (the caller falls back to a UFB backend).
QImage extractShellThumbnail(const QString& path, QSize requested);

} // namespace ufb
