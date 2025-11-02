#pragma once

#include "../thumbnail_extractor.h"
#include <set>
#include <string>
#include <mutex>

// Fast image thumbnail extractor using libjpeg, libpng, and libtiff
class ImageThumbnailExtractor : public ThumbnailExtractorInterface
{
public:
    ImageThumbnailExtractor();
    ~ImageThumbnailExtractor();

    // Get priority (highest = checked first)
    int GetPriority() const override { return 90; }  // Higher than WindowsShell (100 is reserved for Windows Shell for non-image files)

    // Get extractor name
    const char* GetName() const override { return "ImageThumbnailExtractor"; }

    // Check if this extractor can handle the file extension
    bool CanHandle(const std::wstring& extension) override;

    // Extract thumbnail from image file
    HBITMAP Extract(const std::wstring& path, int size) override;

private:
    // Supported extensions
    std::set<std::wstring> m_supportedExtensions;

    // Static mutex to serialize large TIFF extractions (prevents concurrent multi-GB allocations)
    static std::mutex s_tiffMutex;

    // Extract JPEG using libjpeg-turbo
    HBITMAP ExtractJPEG(const std::wstring& path, int size);

    // Extract PNG using libpng
    HBITMAP ExtractPNG(const std::wstring& path, int size);

    // Extract TIFF using libtiff
    HBITMAP ExtractTIFF(const std::wstring& path, int size);

    // Helper: Create HBITMAP from RGB(A) data
    HBITMAP CreateHBITMAPFromRGB(unsigned char* data, int width, int height, int channels);
};
