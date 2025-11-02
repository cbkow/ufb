#include "image_thumbnail_extractor.h"
#include <windows.h>
#include <iostream>
#include <algorithm>
#include <filesystem>

// libjpeg-turbo
#include <jpeglib.h>
#include <jerror.h>

// libpng
#include <png.h>

// libtiff
#include <tiffio.h>

// Define static mutex
std::mutex ImageThumbnailExtractor::s_tiffMutex;

ImageThumbnailExtractor::ImageThumbnailExtractor()
{
    // Initialize supported extensions
    m_supportedExtensions = {
        L".jpg", L".jpeg", L".jpe", L".jfif",  // JPEG formats
        L".png",                                  // PNG
        L".tif", L".tiff"                        // TIFF
    };
}

ImageThumbnailExtractor::~ImageThumbnailExtractor()
{
}

bool ImageThumbnailExtractor::CanHandle(const std::wstring& extension)
{
    std::wstring ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return m_supportedExtensions.find(ext) != m_supportedExtensions.end();
}

HBITMAP ImageThumbnailExtractor::Extract(const std::wstring& path, int size)
{
    // Get file extension
    std::filesystem::path filePath(path);
    std::wstring ext = filePath.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif")
    {
        return ExtractJPEG(path, size);
    }
    else if (ext == L".png")
    {
        return ExtractPNG(path, size);
    }
    else if (ext == L".tif" || ext == L".tiff")
    {
        return ExtractTIFF(path, size);
    }

    return nullptr;
}

HBITMAP ImageThumbnailExtractor::ExtractJPEG(const std::wstring& path, int size)
{
    // Convert wide string to UTF-8
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string pathUtf8(utf8Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &pathUtf8[0], utf8Size, nullptr, nullptr);
    pathUtf8.resize(utf8Size - 1);

    // Open JPEG file
    FILE* infile = nullptr;
    if (_wfopen_s(&infile, path.c_str(), L"rb") != 0 || !infile)
    {
        return nullptr;
    }

    // Setup libjpeg decompression
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, infile);
    jpeg_read_header(&cinfo, TRUE);

    // Calculate scale factor for fast decoding
    int scale = 1;
    while (cinfo.image_width / (scale * 2) >= size && cinfo.image_height / (scale * 2) >= size && scale < 8)
    {
        scale *= 2;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = scale;

    // Request RGB output
    cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    int width = cinfo.output_width;
    int height = cinfo.output_height;
    int channels = cinfo.output_components;

    // Allocate row buffer
    unsigned char* rowData = new unsigned char[width * channels];
    unsigned char* imageData = new unsigned char[width * height * channels];

    // Read scanlines
    int row = 0;
    while (cinfo.output_scanline < cinfo.output_height)
    {
        unsigned char* rowPtr = rowData;
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);
        memcpy(imageData + row * width * channels, rowData, width * channels);
        row++;
    }

    delete[] rowData;

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(infile);

    // Create HBITMAP
    HBITMAP hBitmap = CreateHBITMAPFromRGB(imageData, width, height, channels);

    delete[] imageData;

    return hBitmap;
}

HBITMAP ImageThumbnailExtractor::ExtractPNG(const std::wstring& path, int size)
{
    // Open PNG file
    FILE* infile = nullptr;
    if (_wfopen_s(&infile, path.c_str(), L"rb") != 0 || !infile)
    {
        return nullptr;
    }

    // Check PNG signature
    unsigned char header[8];
    fread(header, 1, 8, infile);
    if (png_sig_cmp(header, 0, 8))
    {
        fclose(infile);
        return nullptr;
    }

    // Create PNG structures
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        fclose(infile);
        return nullptr;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        fclose(infile);
        return nullptr;
    }

    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(infile);
        return nullptr;
    }

    png_init_io(png_ptr, infile);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    // Convert to RGBA
    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    // Allocate row pointers
    png_bytep* row_pointers = new png_bytep[height];
    unsigned char* imageData = new unsigned char[width * height * 4];

    for (int y = 0; y < height; y++)
    {
        row_pointers[y] = imageData + y * width * 4;
    }

    png_read_image(png_ptr, row_pointers);

    delete[] row_pointers;

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(infile);

    // Convert RGBA to BGRA (libpng outputs RGBA, Windows needs BGRA)
    for (int i = 0; i < width * height; i++)
    {
        unsigned char r = imageData[i * 4 + 0];
        unsigned char b = imageData[i * 4 + 2];
        imageData[i * 4 + 0] = b;  // Swap R and B
        imageData[i * 4 + 2] = r;
    }

    // Create HBITMAP
    HBITMAP hBitmap = CreateHBITMAPFromRGB(imageData, width, height, 4);

    delete[] imageData;

    return hBitmap;
}

HBITMAP ImageThumbnailExtractor::ExtractTIFF(const std::wstring& path, int size)
{
    // Check file size first (compressed size on disk)
    try
    {
        uintmax_t fileSize = std::filesystem::file_size(path);
        constexpr uintmax_t maxFileSize = 500ULL * 1024 * 1024; // 500MB

        if (fileSize > maxFileSize)
        {
            return nullptr;
        }
    }
    catch (...)
    {
        return nullptr;
    }

    // Convert wide string to UTF-8
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string pathUtf8(utf8Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &pathUtf8[0], utf8Size, nullptr, nullptr);
    pathUtf8.resize(utf8Size - 1);

    // Open TIFF file
    TIFF* tif = TIFFOpen(pathUtf8.c_str(), "r");
    if (!tif)
    {
        return nullptr;
    }

    uint32 width, height;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);

    // Check memory requirements BEFORE allocating
    // We need 2 buffers: raster (width × height × 4) + imageData (width × height × 4) = 8 bytes per pixel
    uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    uint64_t memoryNeeded = pixelCount * 8; // 8 bytes per pixel (two RGBA buffers)
    constexpr uint64_t maxMemory = 200ULL * 1024 * 1024; // 200MB limit

    if (memoryNeeded > maxMemory)
    {
        TIFFClose(tif);
        return nullptr;
    }

    // Lock mutex to serialize large TIFF extractions (prevent concurrent multi-GB allocations)
    std::lock_guard<std::mutex> lock(s_tiffMutex);

    // Allocate raster
    uint32* raster = (uint32*)_TIFFmalloc(width * height * sizeof(uint32));
    if (!raster)
    {
        TIFFClose(tif);
        return nullptr;
    }

    // Read RGBA image
    if (!TIFFReadRGBAImageOriented(tif, width, height, raster, ORIENTATION_TOPLEFT))
    {
        _TIFFfree(raster);
        TIFFClose(tif);
        return nullptr;
    }

    TIFFClose(tif);

    // Convert RGBA to BGRA (Windows format)
    unsigned char* imageData = new unsigned char[width * height * 4];
    for (uint32 i = 0; i < width * height; i++)
    {
        imageData[i * 4 + 0] = TIFFGetB(raster[i]);  // B
        imageData[i * 4 + 1] = TIFFGetG(raster[i]);  // G
        imageData[i * 4 + 2] = TIFFGetR(raster[i]);  // R
        imageData[i * 4 + 3] = TIFFGetA(raster[i]);  // A
    }

    _TIFFfree(raster);

    // Create HBITMAP
    HBITMAP hBitmap = CreateHBITMAPFromRGB(imageData, width, height, 4);

    delete[] imageData;

    return hBitmap;
}

HBITMAP ImageThumbnailExtractor::CreateHBITMAPFromRGB(unsigned char* data, int width, int height, int channels)
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

    // Copy and convert to BGRA
    unsigned char* dst = (unsigned char*)pBits;

    if (channels == 3)
    {
        // RGB to BGRA
        for (int i = 0; i < width * height; i++)
        {
            dst[i * 4 + 0] = data[i * 3 + 2]; // B
            dst[i * 4 + 1] = data[i * 3 + 1]; // G
            dst[i * 4 + 2] = data[i * 3 + 0]; // R
            dst[i * 4 + 3] = 255;              // A
        }
    }
    else if (channels == 4)
    {
        // Already BGRA (converted before calling this function)
        memcpy(dst, data, width * height * 4);
    }

    return hBitmap;
}
