#include "windows_shell_extractor.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <commoncontrols.h>
#include <iostream>
#include <io.h>
#include <fcntl.h>
#include <algorithm>

// IShellItemImageFactory interface (defined in shobjidl.h for Vista+)
#include <shobjidl.h>

WindowsShellExtractor::WindowsShellExtractor()
{
    // Populate supported extensions
    // Windows Shell can handle many formats, but we'll list common ones
    // Note: JPEG, PNG, TIFF are handled by ImageThumbnailExtractor for better performance

    // Images (common formats handled by ImageThumbnailExtractor: .jpg, .jpeg, .png, .tif, .tiff)
    m_supportedExtensions.insert(L".bmp");
    m_supportedExtensions.insert(L".gif");
    m_supportedExtensions.insert(L".webp");
    m_supportedExtensions.insert(L".ico");

    // RAW formats (if codec installed)
    m_supportedExtensions.insert(L".cr2");
    m_supportedExtensions.insert(L".nef");
    m_supportedExtensions.insert(L".arw");
    m_supportedExtensions.insert(L".dng");
    m_supportedExtensions.insert(L".raf");
    m_supportedExtensions.insert(L".orf");

    // Video formats - REMOVED (handled by VideoThumbnailExtractor with FFmpeg)
    // Windows Shell video thumbnails are unreliable, let FFmpeg handle them instead

    // Documents (PDF removed - handled by PsdAiThumbnailExtractor with Ghostscript)
    m_supportedExtensions.insert(L".docx");
    m_supportedExtensions.insert(L".doc");
    m_supportedExtensions.insert(L".xlsx");
    m_supportedExtensions.insert(L".xls");
    m_supportedExtensions.insert(L".pptx");
    m_supportedExtensions.insert(L".ppt");
}

WindowsShellExtractor::~WindowsShellExtractor()
{
}

bool WindowsShellExtractor::CanHandle(const std::wstring& extension)
{
    // Check if this extension is in our supported list
    // This prevents us from intercepting video files that should go to VideoThumbnailExtractor
    std::wstring ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return m_supportedExtensions.find(ext) != m_supportedExtensions.end();
}

HBITMAP WindowsShellExtractor::Extract(const std::wstring& path, int size)
{
    // Try cache first (fast path)
    HBITMAP hBitmap = TryExtractFromCache(path, size);
    if (hBitmap)
    {
        return hBitmap;
    }

    // Generate thumbnail (slow path)
    hBitmap = ExtractWithGeneration(path, size);

    return hBitmap;
}

HBITMAP WindowsShellExtractor::TryExtractFromCache(const std::wstring& path, int size)
{
    HRESULT hr;

    // Create IShellItem from path
    IShellItem* pItem = nullptr;
    hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&pItem));
    if (FAILED(hr) || !pItem)
        return nullptr;

    // Query for IShellItemImageFactory
    IShellItemImageFactory* pImageFactory = nullptr;
    hr = pItem->QueryInterface(IID_PPV_ARGS(&pImageFactory));
    if (FAILED(hr) || !pImageFactory)
    {
        pItem->Release();
        return nullptr;
    }

    // Request thumbnail from cache only (fast path)
    SIZE thumbSize = { size, size };
    HBITMAP hBitmap = nullptr;

    // SIIGBF_INCACHEONLY = Only return if already cached (doesn't generate)
    // SIIGBF_RESIZETOFIT = Scale to fit requested size
    DWORD flags = SIIGBF_INCACHEONLY | SIIGBF_RESIZETOFIT;

    hr = pImageFactory->GetImage(thumbSize, flags, &hBitmap);

    pImageFactory->Release();
    pItem->Release();

    if (SUCCEEDED(hr) && hBitmap)
        return hBitmap;

    return nullptr;
}

HBITMAP WindowsShellExtractor::ExtractWithGeneration(const std::wstring& path, int size)
{
    HRESULT hr;

    // Create IShellItem from path
    IShellItem* pItem = nullptr;
    hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&pItem));
    if (FAILED(hr) || !pItem)
        return nullptr;

    // Query for IShellItemImageFactory
    IShellItemImageFactory* pImageFactory = nullptr;
    hr = pItem->QueryInterface(IID_PPV_ARGS(&pImageFactory));
    if (FAILED(hr) || !pImageFactory)
    {
        pItem->Release();
        return nullptr;
    }

    // Request thumbnail with generation (slow path)
    SIZE thumbSize = { size, size };
    HBITMAP hBitmap = nullptr;

    // First try: Get thumbnail if available
    // SIIGBF_THUMBNAILONLY = Only thumbnail (not icon fallback)
    // SIIGBF_RESIZETOFIT = Scale to fit requested size
    // SIIGBF_BIGGERSIZEOK = Allow larger size if available
    DWORD flags = SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK;
    hr = pImageFactory->GetImage(thumbSize, flags, &hBitmap);

    // If no thumbnail available, try getting high-quality icon instead
    if (FAILED(hr) || !hBitmap)
    {
        // SIIGBF_ICONONLY = Use icon (high-quality, size-aware)
        // SIIGBF_RESIZETOFIT = Scale to fit requested size
        // SIIGBF_BIGGERSIZEOK = Allow larger size if available
        flags = SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK;
        hr = pImageFactory->GetImage(thumbSize, flags, &hBitmap);
    }

    pImageFactory->Release();
    pItem->Release();

    if (SUCCEEDED(hr) && hBitmap)
        return hBitmap;

    return nullptr;
}
