#include "blend_thumbnail_extractor.h"
#include <windows.h>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <vector>

BlendThumbnailExtractor::BlendThumbnailExtractor()
{
    // Initialize supported extensions
    m_supportedExtensions = {
        L".blend"
    };
}

BlendThumbnailExtractor::~BlendThumbnailExtractor()
{
}

bool BlendThumbnailExtractor::CanHandle(const std::wstring& extension)
{
    std::wstring ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return m_supportedExtensions.find(ext) != m_supportedExtensions.end();
}

HBITMAP BlendThumbnailExtractor::Extract(const std::wstring& path, int size)
{
    // Open .blend file
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return nullptr;
    }

    // Read header (12 bytes)
    char header[12];
    file.read(header, 12);
    if (file.gcount() != 12)
    {
        return nullptr;
    }

    // Check signature
    if (strncmp(header, "BLENDER", 7) != 0)
    {
        return nullptr;
    }

    // Parse header
    char pointerSize = header[7];  // '_' = 32-bit (4 bytes), '-' = 64-bit (8 bytes)
    char endianness = header[8];   // 'v' = little-endian, 'V' = big-endian
    int ptrSize = (pointerSize == '_') ? 4 : 8;
    bool littleEndian = (endianness == 'v');

    // Helper to read int32 with endianness
    auto readInt32 = [&](std::ifstream& f) -> int32_t {
        unsigned char bytes[4];
        f.read(reinterpret_cast<char*>(bytes), 4);
        if (littleEndian)
            return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
        else
            return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
    };

    // Helper to read uint32 with endianness
    auto readUInt32 = [&](std::ifstream& f) -> uint32_t {
        unsigned char bytes[4];
        f.read(reinterpret_cast<char*>(bytes), 4);
        if (littleEndian)
            return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
        else
            return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
    };

    // Search for TEST block (thumbnail)
    bool found = false;
    std::vector<unsigned char> thumbnailData;
    uint32_t thumbWidth = 0;
    uint32_t thumbHeight = 0;

    while (file.good() && !found)
    {
        // Read block code (4 bytes)
        char blockCode[5] = {0};
        file.read(blockCode, 4);
        if (file.gcount() != 4)
            break;

        // Read block size
        int32_t blockSize = readInt32(file);

        // Read old memory address (skip)
        file.seekg(ptrSize, std::ios::cur);

        // Read SDNA index
        readInt32(file);

        // Read count
        int32_t count = readInt32(file);

        // Check if this is the TEST block (thumbnail)
        if (strncmp(blockCode, "TEST", 4) == 0)
        {
            // Read thumbnail dimensions (stored as unsigned 32-bit integers)
            thumbWidth = readUInt32(file);
            thumbHeight = readUInt32(file);

            // Validate dimensions
            if (thumbWidth == 0 || thumbHeight == 0 || thumbWidth > 1024 || thumbHeight > 1024)
            {
                break;
            }

            // Read RGBA data
            int dataSize = thumbWidth * thumbHeight * 4;
            thumbnailData.resize(dataSize);
            file.read(reinterpret_cast<char*>(thumbnailData.data()), dataSize);

            if (file.gcount() == dataSize)
            {
                found = true;
            }
            break;
        }
        else if (strncmp(blockCode, "ENDB", 4) == 0)
        {
            // End of file blocks
            break;
        }
        else
        {
            // Skip this block's data
            file.seekg(blockSize, std::ios::cur);
        }
    }

    file.close();

    if (!found || thumbnailData.empty())
    {
        return nullptr;
    }

    // Create HBITMAP from RGBA data
    HBITMAP hBitmap = CreateHBITMAPFromRGBA(thumbnailData.data(), thumbWidth, thumbHeight);

    return hBitmap;
}

HBITMAP BlendThumbnailExtractor::CreateHBITMAPFromRGBA(unsigned char* data, int width, int height)
{
    // Create DIB section
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = height; // Bottom-up DIB (Blender thumbnails are stored bottom-up)
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
