#pragma once

// Phase 14 - C++ system-icon extraction.
//
// Replaces core/src/system_icons.rs + the Rust FFI that
// UfbIconProvider used to call. Same Windows Shell APIs
// (SHGetFileInfoW + IImageList::GetIcon), called directly
// from C++. Returns a QImage so the provider doesn't have
// to round-trip through PNG bytes.
//
// Special "extensions":
//   "folder" - returns the folder icon (uses
//              FILE_ATTRIBUTE_DIRECTORY trick)
//   "file"   - or any unknown extension - returns Windows'
//              generic file icon
//   ".jpg" etc. - the file-type icon Windows associates
//              with that extension
//
// Returns a null QImage on any failure; the provider
// translates that to "no image available", which QML's
// Image element renders as transparent.

#include <QImage>
#include <QString>

namespace ufb {

// Extract the Windows shell icon for `extension` at the
// closest native size bucket >= `pixelSize`. Pixel buckets:
//   <=16   SHIL_SMALL
//   <=32   SHIL_LARGE
//   <=48   SHIL_EXTRALARGE
//   else   SHIL_JUMBO (256x256)
// Caller is responsible for any final scaling - QML's
// Image.sourceSize handles GPU-side downsampling.
//
// Thread-safe: safe to call from any thread (provider
// worker threads in particular). COM is initialised lazily
// once per thread.
QImage extractSystemIcon(const QString& extension, int pixelSize);

} // namespace ufb
