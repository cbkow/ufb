#pragma once

#include <windows.h>
#include "imgui.h"

// Shared utilities for converting Windows bitmaps/icons to OpenGL textures
namespace TextureUtils {

    // Create ImGui texture from Windows HICON
    // @param hIcon - Windows icon handle
    // @param outGLTexture - Optional output for the OpenGL texture handle (for cleanup)
    // @return ImTextureID for use with ImGui::Image(), or 0 on failure
    ImTextureID CreateTextureFromHICON(HICON hIcon, unsigned int* outGLTexture = nullptr);

    // Create ImGui texture from Windows HBITMAP
    // @param hBitmap - Windows bitmap handle (must be 32-bit BGRA format)
    // @param outGLTexture - Optional output for the OpenGL texture handle (for cleanup)
    // @return ImTextureID for use with ImGui::Image(), or 0 on failure
    ImTextureID CreateTextureFromHBITMAP(HBITMAP hBitmap, unsigned int* outGLTexture = nullptr);

    // Delete OpenGL texture
    // @param glTexture - OpenGL texture handle to delete
    void DeleteTexture(unsigned int glTexture);

} // namespace TextureUtils
