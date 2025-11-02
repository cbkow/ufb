#include "fallback_icon_extractor.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <filesystem>
#include <iostream>

FallbackIconExtractor::FallbackIconExtractor(IconManager* iconManager)
    : m_iconManager(iconManager)
{
}

FallbackIconExtractor::~FallbackIconExtractor()
{
}

bool FallbackIconExtractor::CanHandle(const std::wstring& extension)
{
    // Can handle any extension as a fallback (uses system icons)
    return true;
}

HBITMAP FallbackIconExtractor::Extract(const std::wstring& path, int size)
{
    // Try modern IShellItemImageFactory API first (Vista+)
    HRESULT hr;
    IShellItem* pItem = nullptr;
    hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&pItem));
    if (SUCCEEDED(hr) && pItem)
    {
        IShellItemImageFactory* pImageFactory = nullptr;
        hr = pItem->QueryInterface(IID_PPV_ARGS(&pImageFactory));
        if (SUCCEEDED(hr) && pImageFactory)
        {
            SIZE thumbSize = { size, size };
            HBITMAP hBitmap = nullptr;

            // Request high-quality icon at requested size
            // SIIGBF_ICONONLY = Use icon (high-quality, size-aware)
            // SIIGBF_RESIZETOFIT = Scale to fit requested size
            // SIIGBF_BIGGERSIZEOK = Allow larger size if available
            DWORD flags = SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK;
            hr = pImageFactory->GetImage(thumbSize, flags, &hBitmap);

            pImageFactory->Release();
            pItem->Release();

            if (SUCCEEDED(hr) && hBitmap)
            {
                return hBitmap;
            }
        }
        else
        {
            pItem->Release();
        }
    }

    // Fallback to old SHGetFileInfo API (legacy, returns small 32x32 icons)
    bool isDirectory = std::filesystem::is_directory(path);
    SHFILEINFOW shfi = {0};
    DWORD_PTR result = SHGetFileInfoW(
        path.c_str(),
        isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
        &shfi,
        sizeof(shfi),
        SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES
    );

    if (result == 0 || !shfi.hIcon)
        return nullptr;

    // Convert HICON to HBITMAP
    ICONINFO iconInfo = {};
    if (!GetIconInfo(shfi.hIcon, &iconInfo))
    {
        DestroyIcon(shfi.hIcon);
        return nullptr;
    }

    // Get icon size
    BITMAP bm = {};
    if (!GetObject(iconInfo.hbmColor ? iconInfo.hbmColor : iconInfo.hbmMask, sizeof(bm), &bm))
    {
        DeleteObject(iconInfo.hbmColor);
        DeleteObject(iconInfo.hbmMask);
        DestroyIcon(shfi.hIcon);
        return nullptr;
    }

    int iconWidth = bm.bmWidth;
    int iconHeight = bm.bmHeight;

    // Create a device context
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    // Create a 32-bit bitmap at requested size
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hBitmap || !pBits)
    {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        DeleteObject(iconInfo.hbmColor);
        DeleteObject(iconInfo.hbmMask);
        DestroyIcon(shfi.hIcon);
        return nullptr;
    }

    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // Draw icon centered (scaled to fit if needed)
    int drawX = (size - iconWidth) / 2;
    int drawY = (size - iconHeight) / 2;
    DrawIconEx(hdcMem, drawX, drawY, shfi.hIcon, iconWidth, iconHeight, 0, nullptr, DI_NORMAL);

    SelectObject(hdcMem, hOldBitmap);

    // Cleanup
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);
    DestroyIcon(shfi.hIcon);

    return hBitmap;
}
