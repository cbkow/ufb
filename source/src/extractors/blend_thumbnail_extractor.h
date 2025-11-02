#pragma once

#include "../thumbnail_extractor.h"
#include <set>
#include <string>

// Blender thumbnail extractor - extracts embedded thumbnail from .blend files
class BlendThumbnailExtractor : public ThumbnailExtractorInterface
{
public:
    BlendThumbnailExtractor();
    ~BlendThumbnailExtractor();

    // Get priority (highest = checked first)
    int GetPriority() const override { return 70; }  // Between PsdAi (60) and SVG (85)

    // Get extractor name
    const char* GetName() const override { return "BlendThumbnailExtractor"; }

    // Check if this extractor can handle the file extension
    bool CanHandle(const std::wstring& extension) override;

    // Extract thumbnail from .blend file
    HBITMAP Extract(const std::wstring& path, int size) override;

private:
    // Supported extensions
    std::set<std::wstring> m_supportedExtensions;

    // Helper: Create HBITMAP from RGBA data
    HBITMAP CreateHBITMAPFromRGBA(unsigned char* data, int width, int height);
};
