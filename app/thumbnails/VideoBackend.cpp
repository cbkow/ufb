#include "VideoBackend.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>

namespace ufb {

namespace {

// RAII for ffmpeg handles. Each thumbnail extraction allocates
// fresh contexts so we don't share state between threads -
// concurrent calls on different files are safe.
struct FmtCtxGuard {
    AVFormatContext* p = nullptr;
    ~FmtCtxGuard() { if (p) avformat_close_input(&p); }
};
struct CodecCtxGuard {
    AVCodecContext* p = nullptr;
    ~CodecCtxGuard() { if (p) avcodec_free_context(&p); }
};
struct PacketGuard {
    AVPacket* p = av_packet_alloc();
    ~PacketGuard() { if (p) av_packet_free(&p); }
};
struct FrameGuard {
    AVFrame* p = av_frame_alloc();
    ~FrameGuard() { if (p) av_frame_free(&p); }
};
struct SwsGuard {
    SwsContext* p = nullptr;
    ~SwsGuard() { if (p) sws_freeContext(p); }
};

// Convert a decoded AVFrame to an RGBA QImage, scaled to fit
// `tgt`. Uses libswscale for both colour conversion (most
// codecs decode to YUV) and resizing.
QImage frameToQImage(AVFrame* frame, QSize tgt) {
    SwsGuard sws;
    sws.p = sws_getContext(frame->width, frame->height,
                           static_cast<AVPixelFormat>(frame->format),
                           tgt.width(), tgt.height(),
                           AV_PIX_FMT_RGBA,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws.p) return QImage();

    QImage img(tgt.width(), tgt.height(), QImage::Format_RGBA8888);
    if (img.isNull()) return QImage();

    uint8_t* dst[4] = { img.bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { int(img.bytesPerLine()), 0, 0, 0 };
    sws_scale(sws.p, frame->data, frame->linesize, 0,
              frame->height, dst, dstStride);
    return img;
}

} // namespace

QImage decodeVideo(const QString& path, QSize requestedSize) {
    qInfo("VideoBackend: start path=%s", qPrintable(path));

    FmtCtxGuard fmt;
    if (avformat_open_input(&fmt.p, path.toUtf8().constData(),
                            nullptr, nullptr) < 0) {
        qWarning("VideoBackend: avformat_open_input failed for %s",
                 qPrintable(path));
        return QImage();
    }
    if (avformat_find_stream_info(fmt.p, nullptr) < 0) {
        qWarning("VideoBackend: find_stream_info failed for %s",
                 qPrintable(path));
        return QImage();
    }

    const int streamIdx = av_find_best_stream(
        fmt.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIdx < 0) {
        qWarning("VideoBackend: no video stream in %s",
                 qPrintable(path));
        return QImage();
    }
    AVStream* stream = fmt.p->streams[streamIdx];

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        qWarning("VideoBackend: no decoder for codec_id=%d in %s",
                 int(stream->codecpar->codec_id), qPrintable(path));
        return QImage();
    }

    CodecCtxGuard ctx;
    ctx.p = avcodec_alloc_context3(codec);
    if (!ctx.p
        || avcodec_parameters_to_context(ctx.p, stream->codecpar) < 0) {
        qWarning("VideoBackend: codec context setup failed for %s",
                 qPrintable(path));
        return QImage();
    }
    // Single-threaded decode for thread safety + thumbnail-fast
    // mode (slightly less precise but faster).
    ctx.p->thread_count = 1;
    ctx.p->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(ctx.p, codec, nullptr) < 0) {
        qWarning("VideoBackend: avcodec_open2 failed for %s",
                 qPrintable(path));
        return QImage();
    }

    // Seek ~0.5s in for a representative frame. Some files have
    // black frames or splash screens at the very start; 0.5s is
    // far enough to skip those but close enough that we don't
    // wait on slow seeks for short files. AVSEEK_FLAG_BACKWARD
    // lands on the nearest preceding keyframe.
    int64_t targetUs = AV_TIME_BASE / 2;  // 0.5 s
    if (stream->start_time != AV_NOPTS_VALUE) {
        targetUs += av_rescale_q(stream->start_time,
                                 stream->time_base, AV_TIME_BASE_Q);
    }
    const int64_t targetTs = av_rescale_q(targetUs, AV_TIME_BASE_Q,
                                          stream->time_base);
    av_seek_frame(fmt.p, streamIdx, targetTs, AVSEEK_FLAG_BACKWARD);

    PacketGuard pkt;
    FrameGuard frame;
    if (!pkt.p || !frame.p) return QImage();

    QImage out;
    while (av_read_frame(fmt.p, pkt.p) >= 0) {
        if (pkt.p->stream_index == streamIdx
            && avcodec_send_packet(ctx.p, pkt.p) == 0) {
            const int r = avcodec_receive_frame(ctx.p, frame.p);
            if (r == 0) {
                // Compute target size preserving aspect.
                int srcW = frame.p->width;
                int srcH = frame.p->height;
                int tgtW = srcW, tgtH = srcH;
                if (requestedSize.isValid()
                    && requestedSize.width() > 0
                    && requestedSize.height() > 0) {
                    const double sx = double(requestedSize.width())  / srcW;
                    const double sy = double(requestedSize.height()) / srcH;
                    const double s  = std::min(sx, sy);
                    if (s > 0 && s < 1.0) {
                        tgtW = std::max(1, int(srcW * s));
                        tgtH = std::max(1, int(srcH * s));
                    }
                }
                out = frameToQImage(frame.p, QSize(tgtW, tgtH));
                av_packet_unref(pkt.p);
                break;
            }
        }
        av_packet_unref(pkt.p);
    }

    if (out.isNull()) {
        qWarning("VideoBackend: no frame decoded from %s",
                 qPrintable(path));
    } else {
        qInfo("VideoBackend: ok path=%s -> %dx%d",
              qPrintable(path), out.width(), out.height());
    }
    return out;
}

} // namespace ufb
