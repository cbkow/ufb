#pragma once

#include "../thumbnail_extractor.h"
#include <set>
#include <string>

// Forward declarations for FFmpeg types to avoid including headers here
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

// Video thumbnail extractor using FFmpeg
class VideoThumbnailExtractor : public ThumbnailExtractorInterface
{
public:
    VideoThumbnailExtractor();
    ~VideoThumbnailExtractor();

    // Get priority (higher = checked first)
    int GetPriority() const override { return 50; }  // Between shell (100) and fallback (0)

    // Get extractor name
    const char* GetName() const override { return "VideoThumbnailExtractor"; }

    // Check if this extractor can handle the file extension
    bool CanHandle(const std::wstring& extension) override;

    // Extract thumbnail from video file
    // Returns HBITMAP on success, nullptr on failure
    HBITMAP Extract(const std::wstring& path, int size) override;

private:
    // Supported video extensions
    std::set<std::wstring> m_videoExtensions;

    // Extract a frame from the video at the specified timestamp (in seconds)
    HBITMAP ExtractFrame(const std::wstring& path, double timestamp, int size, int& outWidth, int& outHeight);

    // Convert AVFrame to HBITMAP
    HBITMAP ConvertFrameToHBITMAP(AVFrame* frame, int width, int height);
};
