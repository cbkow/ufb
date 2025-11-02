#include "icon_manager.h"
#include <glad/gl.h>
#include <windows.h>
#include <shellapi.h>
#include <comdef.h>
#include <filesystem>
#include <vector>
#include <iostream>
#include <algorithm>

// Include IImageList interface - must come before commoncontrols.h
#include <shobjidl.h>
#include <commoncontrols.h>

// Define SHIL constants if not already defined
#ifndef SHIL_JUMBO
#define SHIL_JUMBO 0x4
#endif

#ifndef SHIL_EXTRALARGE
#define SHIL_EXTRALARGE 0x2
#endif

// IImageList GUID - define it directly to avoid linker issues
static const GUID IID_IImageList_Local = { 0x46EB5926, 0x582E, 0x4017, { 0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50 } };

IconManager::~IconManager()
{
    Shutdown();
}

void IconManager::Initialize()
{
    // Nothing special to initialize for now
}

void IconManager::Shutdown()
{
    try
    {
        // Clean up all cached icons
        for (auto& [ext, entry] : m_iconCache)
        {
            if (entry.hIcon)
            {
                DestroyIcon(entry.hIcon);
                entry.hIcon = nullptr;
            }
            if (entry.glTexture)
            {
                glDeleteTextures(1, &entry.glTexture);
                entry.glTexture = 0;
            }
        }
        m_iconCache.clear();
    }
    catch (const std::exception& e)
    {
        std::cerr << "IconManager: Shutdown exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "IconManager: Shutdown unknown exception" << std::endl;
    }
}

ImTextureID IconManager::GetFileIcon(const std::wstring& path, bool isDirectory, int size)
{
    // Determine cache key
    std::wstring cacheKey;

    // For drives (e.g., "C:\") and network paths (e.g., "\\server\share"), use the full path as cache key
    // This ensures each drive/network location gets its own icon
    bool isDrive = (path.length() == 3 && path[1] == L':' && path[2] == L'\\');
    bool isNetworkPath = (path.length() >= 2 && path[0] == L'\\' && path[1] == L'\\');

    // Helper to check if this is a special Windows folder (Desktop, Documents, Downloads, etc.)
    // These folders need unique icons from the shell
    auto isSpecialFolder = [](const std::wstring& folderPath) -> bool {
        std::wstring lowerPath = folderPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);

        // Check for common special folders
        return (lowerPath.find(L"\\desktop") != std::wstring::npos ||
                lowerPath.find(L"\\documents") != std::wstring::npos ||
                lowerPath.find(L"\\downloads") != std::wstring::npos ||
                lowerPath.find(L"\\pictures") != std::wstring::npos ||
                lowerPath.find(L"\\music") != std::wstring::npos ||
                lowerPath.find(L"\\videos") != std::wstring::npos ||
                lowerPath.find(L"\\favorites") != std::wstring::npos ||
                lowerPath.find(L"\\onedrive") != std::wstring::npos);
    };

    if (isDrive)
    {
        cacheKey = path + L"_" + std::to_wstring(size);  // e.g., "C:\_16"
    }
    else if (isNetworkPath)
    {
        cacheKey = path + L"_" + std::to_wstring(size);  // e.g., "\\server\share_16"
    }
    else if (isDirectory && isSpecialFolder(path))
    {
        // Special folders get their own cache key to preserve unique icons
        cacheKey = path + L"_" + std::to_wstring(size);  // e.g., "C:\Users\Name\Desktop_16"
    }
    else if (isDirectory)
    {
        cacheKey = L"[folder]_" + std::to_wstring(size);  // Generic folder icon
    }
    else
    {
        std::filesystem::path p(path);
        cacheKey = p.extension().wstring() + L"_" + std::to_wstring(size);
        if (p.extension().empty())
            cacheKey = L"[no_ext]_" + std::to_wstring(size);
    }

    // Check cache
    auto it = m_iconCache.find(cacheKey);
    if (it != m_iconCache.end())
        return it->second.texID;

    HICON hIcon = nullptr;

    // For grid mode (size >= 64), always use jumbo icons for best quality
    // Downscaling from 256x256 looks better than upscaling from 32x32
    if (size >= 64)
    {
        // Get icon index using SHGetFileInfo (use actual file, not attributes)
        SHFILEINFOW shfi = {0};
        DWORD_PTR result = SHGetFileInfoW(
            path.c_str(),
            0,  // Don't pass FILE_ATTRIBUTE flags
            &shfi,
            sizeof(shfi),
            SHGFI_SYSICONINDEX  // Get icon index from actual file
        );

        if (result != 0)
        {
            //std::wcout << L"[IconManager] Getting jumbo icon for: " << path << L" (icon index: " << shfi.iIcon << L")" << std::endl;

            // Get the jumbo image list
            IImageList* pImageList = nullptr;
            HRESULT hr = SHGetImageList(SHIL_JUMBO, IID_IImageList_Local, (void**)&pImageList);

            if (SUCCEEDED(hr) && pImageList)
            {
                hr = pImageList->GetIcon(shfi.iIcon, ILD_TRANSPARENT, &hIcon);
                pImageList->Release();

                if (SUCCEEDED(hr) && hIcon)
                {
                    //std::cout << "[IconManager] Successfully got jumbo icon (256x256)" << std::endl;
                }
                else
                {
                    //std::cout << "[IconManager] Failed to get icon from IImageList, hr=" << std::hex << hr << std::endl;
                }
            }
            else
            {
                //std::cout << "[IconManager] Failed to get IImageList, hr=" << std::hex << hr << std::endl;
            }
        }
        else
        {
            //std::wcout << L"[IconManager] SHGetFileInfoW failed for: " << path << std::endl;
        }

        // Fallback if jumbo icon failed
        if (!hIcon)
        {
            //std::cout << "[IconManager] Falling back to large icon" << std::endl;
            result = SHGetFileInfoW(
                path.c_str(),
                0,
                &shfi,
                sizeof(shfi),
                SHGFI_ICON | SHGFI_LARGEICON
            );

            if (result != 0 && shfi.hIcon)
                hIcon = shfi.hIcon;
        }
    }
    else
    {
        // Standard icon sizes - use actual file for better quality
        UINT iconFlags = SHGFI_ICON;

        if (size >= 32)
        {
            iconFlags |= SHGFI_LARGEICON;  // 32x32 or 48x48
        }
        else
        {
            iconFlags |= SHGFI_SMALLICON;  // 16x16
        }

        SHFILEINFOW shfi = {0};
        DWORD_PTR result = SHGetFileInfoW(
            path.c_str(),
            0,  // Use actual file, not generic attributes
            &shfi,
            sizeof(shfi),
            iconFlags
        );

        if (result != 0 && shfi.hIcon)
            hIcon = shfi.hIcon;
    }

    if (!hIcon)
    {
        // Failed to get icon, return 0
        return 0;
    }

    // Create ImGui texture from HICON
    IconEntry entry;
    entry.hIcon = hIcon;
    entry.texID = CreateImGuiIconFromHICON(hIcon);

    // Cache it
    m_iconCache[cacheKey] = entry;

    return entry.texID;
}

ImTextureID IconManager::CreateImGuiIconFromHICON(HICON hIcon)
{
    // Get icon info
    ICONINFO iconInfo = {};
    if (!GetIconInfo(hIcon, &iconInfo))
        return 0;

    // Get bitmap info
    BITMAP bm = {};
    if (!GetObject(iconInfo.hbmColor ? iconInfo.hbmColor : iconInfo.hbmMask, sizeof(bm), &bm))
    {
        DeleteObject(iconInfo.hbmColor);
        DeleteObject(iconInfo.hbmMask);
        return 0;
    }

    int width = bm.bmWidth;
    int height = bm.bmHeight;

    // Create a device context
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    // Create a 32-bit bitmap for the icon
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
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
        return 0;
    }

    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // Draw icon into bitmap
    DrawIconEx(hdcMem, 0, 0, hIcon, width, height, 0, nullptr, DI_NORMAL);

    SelectObject(hdcMem, hOldBitmap);

    // Create OpenGL texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Upload texture data (convert BGRA to RGBA)
    std::vector<unsigned char> rgba_data(width * height * 4);
    unsigned char* src = (unsigned char*)pBits;
    for (int i = 0; i < width * height; ++i)
    {
        rgba_data[i * 4 + 0] = src[i * 4 + 2]; // R = B
        rgba_data[i * 4 + 1] = src[i * 4 + 1]; // G = G
        rgba_data[i * 4 + 2] = src[i * 4 + 0]; // B = R
        rgba_data[i * 4 + 3] = src[i * 4 + 3]; // A = A
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_data.data());

    // Cleanup
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);

    // Store texture handle in cache
    for (auto& [key, entry] : m_iconCache)
    {
        if (entry.hIcon == hIcon)
        {
            entry.glTexture = texture;
            break;
        }
    }

    return (ImTextureID)(intptr_t)texture;
}
