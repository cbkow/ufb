#pragma once

#include "../thumbnail_extractor.h"
#include <set>

// Thumbnail extractor using Windows Shell API (IShellItemImageFactory)
// Uses Windows thumbnail cache for high performance
// Supports all formats Windows can render (images, videos, PDFs, Office docs, etc.)
class WindowsShellExtractor : public ThumbnailExtractorInterface
{
public:
    WindowsShellExtractor();
    ~WindowsShellExtractor() override;

    bool CanHandle(const std::wstring& extension) override;
    HBITMAP Extract(const std::wstring& path, int size) override;
    int GetPriority() const override { return 100; } // Default priority
    const char* GetName() const override { return "WindowsShellExtractor"; }

private:
    // Set of supported extensions (populated during construction)
    std::set<std::wstring> m_supportedExtensions;

    // Helper: Try to extract from cache only (fast path)
    HBITMAP TryExtractFromCache(const std::wstring& path, int size);

    // Helper: Extract with full generation (slow path)
    HBITMAP ExtractWithGeneration(const std::wstring& path, int size);
};
