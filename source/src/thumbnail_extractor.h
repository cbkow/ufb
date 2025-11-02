#pragma once

#include <windows.h>
#include <string>

// Abstract interface for thumbnail extraction
// Allows pluggable thumbnail extraction strategies with priority-based selection
// Note: Renamed to ThumbnailExtractorInterface to avoid conflict with Windows SDK IThumbnailExtractor
class ThumbnailExtractorInterface
{
public:
    virtual ~ThumbnailExtractorInterface() = default;

    // Check if this extractor can handle the given file extension
    // @param extension - File extension including the dot (e.g., ".jpg", ".png")
    // @return true if this extractor supports the format
    virtual bool CanHandle(const std::wstring& extension) = 0;

    // Extract thumbnail from file
    // Called on worker thread - must be thread-safe
    // @param path - Full path to the file
    // @param size - Desired thumbnail size (width/height in pixels)
    // @return HBITMAP containing the thumbnail, or nullptr on failure
    //         Caller is responsible for calling DeleteObject() on the returned HBITMAP
    virtual HBITMAP Extract(const std::wstring& path, int size) = 0;

    // Get extractor priority (higher values are tried first)
    // @return Priority value (0-1000, where 100 is default)
    virtual int GetPriority() const = 0;

    // Get extractor name for debugging
    virtual const char* GetName() const = 0;
};
