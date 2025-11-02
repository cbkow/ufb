#include "svg_thumbnail_extractor.h"
#include <windows.h>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fstream>

// Define implementation macros before including nanosvg
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg.h>
#include <nanosvgrast.h>

SvgThumbnailExtractor::SvgThumbnailExtractor()
{
    // Initialize supported extensions
    m_supportedExtensions = {
        L".svg",
        L".svgz"  // Compressed SVG (if we want to support it later)
    };

}

SvgThumbnailExtractor::~SvgThumbnailExtractor()
{
}

bool SvgThumbnailExtractor::CanHandle(const std::wstring& extension)
{
    std::wstring ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return m_supportedExtensions.find(ext) != m_supportedExtensions.end();
}

HBITMAP SvgThumbnailExtractor::Extract(const std::wstring& path, int size)
{
    // Convert wide string to UTF-8
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string pathUtf8(utf8Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &pathUtf8[0], utf8Size, nullptr, nullptr);
    pathUtf8.resize(utf8Size - 1);

    // Parse SVG file
    // Note: "px" is the units parameter (96 DPI)
    NSVGimage* image = nsvgParseFromFile(pathUtf8.c_str(), "px", 96.0f);
    if (!image)
    {
        return nullptr;
    }

    // Calculate scale to fit thumbnail size
    float scale = 1.0f;
    if (image->width > 0 && image->height > 0)
    {
        float scaleX = (float)size / image->width;
        float scaleY = (float)size / image->height;
        scale = (scaleX < scaleY) ? scaleX : scaleY;
    }

    int width = (int)(image->width * scale);
    int height = (int)(image->height * scale);

    // Ensure we have valid dimensions
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
    {
        nsvgDelete(image);
        return nullptr;
    }

    // Create rasterizer
    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    if (!rasterizer)
    {
        nsvgDelete(image);
        return nullptr;
    }

    // Allocate RGBA buffer
    unsigned char* imageData = new unsigned char[width * height * 4];
    memset(imageData, 0, width * height * 4);

    // Rasterize SVG to RGBA
    nsvgRasterize(rasterizer, image, 0, 0, scale, imageData, width, height, width * 4);

    // Clean up nanosvg objects
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);

    // Create HBITMAP from RGBA data
    HBITMAP hBitmap = CreateHBITMAPFromRGBA(imageData, width, height);

    delete[] imageData;

    if (!hBitmap)
    {
    }

    return hBitmap;
}

HBITMAP SvgThumbnailExtractor::CreateHBITMAPFromRGBA(unsigned char* data, int width, int height)
{
    // Create DIB section
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    HBITMAP hBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);

    if (!hBitmap || !pBits)
    {
        return nullptr;
    }

    // Convert RGBA to BGRA for Windows
    unsigned char* dst = (unsigned char*)pBits;
    for (int i = 0; i < width * height; i++)
    {
        dst[i * 4 + 0] = data[i * 4 + 2]; // B
        dst[i * 4 + 1] = data[i * 4 + 1]; // G
        dst[i * 4 + 2] = data[i * 4 + 0]; // R
        dst[i * 4 + 3] = data[i * 4 + 3]; // A
    }

    return hBitmap;
}
