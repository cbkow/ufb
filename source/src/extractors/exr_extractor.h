#pragma once

#include "../thumbnail_extractor.h"
#include <string>
#include <vector>

// EXR Thumbnail Extractor - Uses OpenEXR library for extracting thumbnails
// from OpenEXR (.exr) High Dynamic Range image files
class EXRExtractor : public ThumbnailExtractorInterface
{
public:
    EXRExtractor();
    ~EXRExtractor() override;

    bool CanHandle(const std::wstring& extension) override;
    HBITMAP Extract(const std::wstring& path, int size) override;
    const char* GetName() const override { return "EXR"; }
    int GetPriority() const override { return 80; }  // High priority (before image extractor)

private:
    // Helper to convert half-float to RGBA8
    void ConvertHalfToRGBA8(const void* halfData, int width, int height, std::vector<uint8_t>& rgba8);
};
