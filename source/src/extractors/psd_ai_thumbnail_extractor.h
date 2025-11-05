#pragma once

#include "../thumbnail_extractor.h"
#include <set>
#include <string>
#include <mutex>

// PSD, AI, WebP, AVIF, JXL, JP2 thumbnail extractor using ImageMagick and Ghostscript
class PsdAiThumbnailExtractor : public ThumbnailExtractorInterface
{
public:
    PsdAiThumbnailExtractor();
    ~PsdAiThumbnailExtractor();

    // Get priority (higher = checked first)
    int GetPriority() const override { return 60; }  // Higher than video (50)

    // Get extractor name
    const char* GetName() const override { return "PsdAiThumbnailExtractor"; }

    // Check if this extractor can handle the file extension
    bool CanHandle(const std::wstring& extension) override;

    // Extract thumbnail from supported file formats
    HBITMAP Extract(const std::wstring& path, int size) override;

private:
    // Supported extensions
    std::set<std::wstring> m_supportedExtensions;

    // Paths to external tools
    std::wstring m_magickPath;
    std::wstring m_ghostscriptPath;

    // Static mutex to limit concurrent extractions (prevents memory exhaustion)
    static std::mutex s_extractionMutex;

    // Extract using ImageMagick (PSD, HDR, PIC, WebP, AVIF, JXL, JP2)
    HBITMAP ExtractWithMagick(const std::wstring& path, int size);

    // Extract AI/EPS/PDF using Ghostscript
    HBITMAP ExtractAI(const std::wstring& path, int size);

    // Run command and wait for completion
    bool RunCommand(const std::wstring& command, const std::wstring& args);

    // Load PNG file into HBITMAP
    HBITMAP LoadPNGToHBITMAP(const std::wstring& pngPath);
};
