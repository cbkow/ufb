#pragma once

#include "../thumbnail_extractor.h"
#include "../icon_manager.h"

// Fallback thumbnail extractor using IconManager
// Used when other extractors fail or for files with no thumbnail support
class FallbackIconExtractor : public ThumbnailExtractorInterface
{
public:
    FallbackIconExtractor(IconManager* iconManager);
    ~FallbackIconExtractor() override;

    bool CanHandle(const std::wstring& extension) override;
    HBITMAP Extract(const std::wstring& path, int size) override;
    int GetPriority() const override { return 0; } // Lowest priority (last resort)
    const char* GetName() const override { return "FallbackIconExtractor"; }

private:
    IconManager* m_iconManager;
};
