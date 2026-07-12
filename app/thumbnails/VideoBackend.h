#pragma once

// Phase 14 step 7 - video thumbnails via ffmpeg.
//
// Decodes one frame ~0.5s into the file (or earlier if shorter)
// and converts it to an RGBA QImage at the requested size. Uses
// the bundled ffmpeg DLLs in external/ffmpeg/ (avformat,
// avcodec, avutil, swscale).
//
// Per-call state only - each invocation gets a fresh
// AVFormatContext / AVCodecContext, so concurrent extraction
// from QThreadPool workers is safe.

#include <QImage>
#include <QString>
#include <QSize>

namespace ufb {

QImage decodeVideo(const QString& path, QSize requestedSize);

} // namespace ufb
