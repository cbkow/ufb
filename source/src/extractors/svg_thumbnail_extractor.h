#pragma once

#include "../thumbnail_extractor.h"
#include <set>
#include <string>

// SVG thumbnail extractor using nanosvg
class SvgThumbnailExtractor : public ThumbnailExtractorInterface
{
public:
    SvgThumbnailExtractor();
    ~SvgThumbnailExtractor();

    // Get priority (highest = checked first)
    int GetPriority() const override { return 85; }  // Between ImageThumbnailExtractor (90) and WindowsShell (100)

    // Get extractor name
    const char* GetName() const override { return "SvgThumbnailExtractor"; }

    // Check if this extractor can handle the file extension
    bool CanHandle(const std::wstring& extension) override;

    // Extract thumbnail from SVG file
    HBITMAP Extract(const std::wstring& path, int size) override;

private:
    // Supported extensions
    std::set<std::wstring> m_supportedExtensions;

    // Helper: Create HBITMAP from RGBA data
    HBITMAP CreateHBITMAPFromRGBA(unsigned char* data, int width, int height);
};
