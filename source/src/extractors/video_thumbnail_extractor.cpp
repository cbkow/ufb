#include "video_thumbnail_extractor.h"
#include <windows.h>
#include <iostream>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

VideoThumbnailExtractor::VideoThumbnailExtractor()
{
    // Initialize supported video extensions (lowercase)
    m_videoExtensions = {
        L".mp4", L".mov", L".avi", L".mkv", L".wmv", L".flv", L".webm",
        L".m4v", L".mpg", L".mpeg", L".3gp", L".mxf", L".mts", L".m2ts"
    };
}

VideoThumbnailExtractor::~VideoThumbnailExtractor()
{
}

bool VideoThumbnailExtractor::CanHandle(const std::wstring& extension)
{
    // Convert to lowercase
    std::wstring ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return m_videoExtensions.find(ext) != m_videoExtensions.end();
}

HBITMAP VideoThumbnailExtractor::Extract(const std::wstring& path, int size)
{
    int outWidth, outHeight;

    // Try to extract a frame at 1 second into the video
    HBITMAP hBitmap = ExtractFrame(path, 1.0, size, outWidth, outHeight);

    if (!hBitmap)
    {
        // If that fails, try at the beginning
        hBitmap = ExtractFrame(path, 0.0, size, outWidth, outHeight);
    }

    return hBitmap;
}

HBITMAP VideoThumbnailExtractor::ExtractFrame(const std::wstring& path, double timestamp, int size, int& outWidth, int& outHeight)
{
    // Convert wide string to UTF-8
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Size == 0)
        return nullptr;

    std::string pathUtf8(utf8Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &pathUtf8[0], utf8Size, nullptr, nullptr);
    pathUtf8.resize(utf8Size - 1); // Remove null terminator

    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgbFrame = nullptr;
    SwsContext* swsCtx = nullptr;
    HBITMAP hBitmap = nullptr;

    // Open video file
    if (avformat_open_input(&formatCtx, pathUtf8.c_str(), nullptr, nullptr) != 0)
    {
        return nullptr;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(formatCtx, nullptr) < 0)
    {
        avformat_close_input(&formatCtx);
        return nullptr;
    }

    // Find the first video stream
    int videoStreamIndex = -1;
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++)
    {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIndex = i;
            break;
        }
    }

    if (videoStreamIndex == -1)
    {
        avformat_close_input(&formatCtx);
        return nullptr;
    }

    AVStream* videoStream = formatCtx->streams[videoStreamIndex];
    AVCodecParameters* codecParams = videoStream->codecpar;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec)
    {
        avformat_close_input(&formatCtx);
        return nullptr;
    }

    // Allocate codec context
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
        avformat_close_input(&formatCtx);
        return nullptr;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(codecCtx, codecParams) < 0)
    {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return nullptr;
    }

    // Open codec
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return nullptr;
    }

    // Allocate frames
    frame = av_frame_alloc();
    rgbFrame = av_frame_alloc();
    if (!frame || !rgbFrame)
    {
        if (frame) av_frame_free(&frame);
        if (rgbFrame) av_frame_free(&rgbFrame);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return nullptr;
    }

    // Calculate output dimensions maintaining aspect ratio
    int srcWidth = codecCtx->width;
    int srcHeight = codecCtx->height;
    float aspectRatio = (float)srcWidth / (float)srcHeight;

    int dstWidth, dstHeight;
    if (aspectRatio > 1.0f)
    {
        // Landscape
        dstWidth = size;
        dstHeight = (int)(size / aspectRatio);
    }
    else
    {
        // Portrait or square
        dstHeight = size;
        dstWidth = (int)(size * aspectRatio);
    }

    // Allocate buffer for RGB frame
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_BGRA, dstWidth, dstHeight, 1);
    uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, buffer, AV_PIX_FMT_BGRA, dstWidth, dstHeight, 1);

    // Create scaling context
    swsCtx = sws_getContext(
        srcWidth, srcHeight, codecCtx->pix_fmt,
        dstWidth, dstHeight, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsCtx)
    {
        av_free(buffer);
        av_frame_free(&frame);
        av_frame_free(&rgbFrame);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return nullptr;
    }

    // Seek to desired timestamp
    if (timestamp > 0.0)
    {
        int64_t seekTarget = (int64_t)(timestamp * AV_TIME_BASE);
        av_seek_frame(formatCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx);
    }

    // Read frames until we get a valid one
    AVPacket packet;
    bool gotFrame = false;
    int framesRead = 0;
    const int maxFramesToRead = 100; // Safety limit

    while (av_read_frame(formatCtx, &packet) >= 0 && framesRead < maxFramesToRead)
    {
        if (packet.stream_index == videoStreamIndex)
        {
            // Send packet to decoder
            if (avcodec_send_packet(codecCtx, &packet) == 0)
            {
                // Receive decoded frame
                if (avcodec_receive_frame(codecCtx, frame) == 0)
                {
                    // Scale and convert to BGRA
                    sws_scale(
                        swsCtx,
                        frame->data, frame->linesize, 0, srcHeight,
                        rgbFrame->data, rgbFrame->linesize
                    );

                    // Convert to HBITMAP
                    hBitmap = ConvertFrameToHBITMAP(rgbFrame, dstWidth, dstHeight);

                    if (hBitmap)
                    {
                        outWidth = dstWidth;
                        outHeight = dstHeight;
                        gotFrame = true;
                    }

                    av_packet_unref(&packet);
                    break;
                }
            }
            framesRead++;
        }
        av_packet_unref(&packet);
    }

    // Cleanup
    sws_freeContext(swsCtx);
    av_free(buffer);
    av_frame_free(&frame);
    av_frame_free(&rgbFrame);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);

    return hBitmap;
}

HBITMAP VideoThumbnailExtractor::ConvertFrameToHBITMAP(AVFrame* frame, int width, int height)
{
    // Create DIB section
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    HBITMAP hBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);

    if (!hBitmap || !pBits)
    {
        return nullptr;
    }

    // Copy frame data to bitmap
    // AVFrame data is already in BGRA format with linesize padding
    uint8_t* dst = (uint8_t*)pBits;
    uint8_t* src = frame->data[0];
    int srcLineSize = frame->linesize[0];

    for (int y = 0; y < height; y++)
    {
        memcpy(dst + y * width * 4, src + y * srcLineSize, width * 4);
    }

    return hBitmap;
}
