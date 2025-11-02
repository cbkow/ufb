#pragma once

#include <windows.h>
#include <string>
#include <map>
#include "imgui.h"

struct IconEntry {
    HICON hIcon = nullptr;
    ImTextureID texID = 0;
    unsigned int glTexture = 0;
};

class IconManager
{
public:
    IconManager() = default;
    ~IconManager();

    // Initialize the icon manager
    void Initialize();

    // Shutdown and cleanup resources
    void Shutdown();

    // Get icon for a file path (with caching by extension)
    // @param size - Desired icon size (16=small, 32=large, 48=extralarge, 256=jumbo)
    ImTextureID GetFileIcon(const std::wstring& path, bool isDirectory, int size = 32);

private:
    // Convert HICON to OpenGL texture and return ImTextureID
    ImTextureID CreateImGuiIconFromHICON(HICON hIcon);

    // Cache of icons by extension or folder
    std::map<std::wstring, IconEntry> m_iconCache;
};
