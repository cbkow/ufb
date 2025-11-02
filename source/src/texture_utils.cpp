#include "texture_utils.h"
#include <glad/gl.h>
#include <vector>
#include <iostream>

namespace TextureUtils {

ImTextureID CreateTextureFromHICON(HICON hIcon, unsigned int* outGLTexture)
{
    if (!hIcon)
        return 0;

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

    if (outGLTexture)
        *outGLTexture = texture;

    return (ImTextureID)(intptr_t)texture;
}

ImTextureID CreateTextureFromHBITMAP(HBITMAP hBitmap, unsigned int* outGLTexture)
{
    if (!hBitmap)
    {
        std::cout << "[TextureUtils] NULL HBITMAP" << std::endl;
        return 0;
    }

    // Get bitmap info
    BITMAP bm = {};
    if (GetObject(hBitmap, sizeof(bm), &bm) == 0)
    {
        std::cout << "[TextureUtils] Failed to get bitmap info" << std::endl;
        return 0;
    }

    int width = bm.bmWidth;
    int height = abs(bm.bmHeight);
    std::cout << "[TextureUtils] Creating texture from HBITMAP: " << width << "x" << height << std::endl;

    // Create a device context
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    // Create a 32-bit DIB section to read the bitmap data
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDIB = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hDIB || !pBits)
    {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return 0;
    }

    // Copy source bitmap to DIB
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hDIB);

    HDC hdcSrc = CreateCompatibleDC(hdcScreen);
    HBITMAP hOldSrc = (HBITMAP)SelectObject(hdcSrc, hBitmap);

    BitBlt(hdcMem, 0, 0, width, height, hdcSrc, 0, 0, SRCCOPY);

    SelectObject(hdcSrc, hOldSrc);
    SelectObject(hdcMem, hOldBitmap);
    DeleteDC(hdcSrc);

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
    DeleteObject(hDIB);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    if (outGLTexture)
        *outGLTexture = texture;

    std::cout << "[TextureUtils] Successfully created GL texture: " << texture << std::endl;
    return (ImTextureID)(intptr_t)texture;
}

void DeleteTexture(unsigned int glTexture)
{
    if (glTexture)
    {
        std::cout << "[TextureUtils] Deleting GL texture: " << glTexture << std::endl;
        glDeleteTextures(1, &glTexture);
    }
}

} // namespace TextureUtils
