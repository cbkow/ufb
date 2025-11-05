#include "psd_ai_thumbnail_extractor.h"
#include <windows.h>
#include <gdiplus.h>
#include <iostream>
#include <filesystem>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

// Define static mutex
std::mutex PsdAiThumbnailExtractor::s_extractionMutex;

PsdAiThumbnailExtractor::PsdAiThumbnailExtractor()
{
    // Initialize supported extensions
    m_supportedExtensions = {
        L".psd",    // Photoshop
        L".ai",     // Illustrator
        L".eps",    // Encapsulated PostScript
        L".pdf",    // PDF documents
        L".hdr",    // Radiance HDR
        L".pic",    // Softimage PIC
        L".webp",   // WebP
        L".avif",   // AVIF
        L".heic",   // HEIC (same decoder as AVIF)
        L".heif",   // HEIF (same decoder as AVIF)
        L".jxl",    // JPEG XL
        L".jp2",    // JPEG 2000
        L".j2k",    // JPEG 2000 codestream
        L".jpf",    // JPEG 2000
        L".jpx"     // JPEG 2000 extended
    };

    // Get paths to external tools (relative to executable)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

    m_magickPath = (exeDir / "magick" / "magick.exe").wstring();
    m_ghostscriptPath = (exeDir / "ghostscript" / "bin" / "gswin64c.exe").wstring();

    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
}

PsdAiThumbnailExtractor::~PsdAiThumbnailExtractor()
{
}

bool PsdAiThumbnailExtractor::CanHandle(const std::wstring& extension)
{
    std::wstring ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return m_supportedExtensions.find(ext) != m_supportedExtensions.end();
}

HBITMAP PsdAiThumbnailExtractor::Extract(const std::wstring& path, int size)
{
    // Get file extension
    std::filesystem::path filePath(path);
    std::wstring ext = filePath.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Check file size - skip files larger than 500MB (prevent memory exhaustion)
    try
    {
        uintmax_t fileSize = std::filesystem::file_size(filePath);
        constexpr uintmax_t maxFileSize = 500ULL * 1024 * 1024; // 500MB

        if (fileSize > maxFileSize)
        {
            // Skip large files to prevent memory exhaustion
            return nullptr;
        }
    }
    catch (...)
    {
        return nullptr;
    }

    // Lock mutex to serialize extractions (prevents concurrent Ghostscript/ImageMagick processes)
    // This prevents 4 threads from each spawning heavy processes simultaneously
    std::lock_guard<std::mutex> lock(s_extractionMutex);

    if (ext == L".ai" || ext == L".eps" || ext == L".pdf")
    {
        // Use Ghostscript for vector/PDF formats
        return ExtractAI(path, size);
    }
    else
    {
        // Use ImageMagick for all other formats (PSD, HDR, PIC, WebP, AVIF, HEIC, HEIF, JXL, JP2)
        return ExtractWithMagick(path, size);
    }
}

HBITMAP PsdAiThumbnailExtractor::ExtractWithMagick(const std::wstring& path, int size)
{
    // Create temp output path with unique filename (process + thread + tick count)
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring uniqueId = std::to_wstring(GetCurrentProcessId()) + L"_" +
                            std::to_wstring(GetCurrentThreadId()) + L"_" +
                            std::to_wstring(GetTickCount64());
    std::wstring outputPath = std::wstring(tempPath) + L"magick_thumb_" + uniqueId + L".png";

    // Build ImageMagick command
    // For multi-page formats (PSD, TIFF), use [0] to get first page/layer
    // For single-page formats (WebP, AVIF, JXL, JP2), [0] is harmless
    // magick "input[0]" -resize 256x256 -background white -flatten output.png
    std::wstring args = L"\"" + path + L"[0]\" -resize " + std::to_wstring(size) + L"x" + std::to_wstring(size) +
                        L" -background white -flatten \"" + outputPath + L"\"";

    if (!RunCommand(m_magickPath, args))
    {
        return nullptr;
    }

    // Load the PNG
    HBITMAP hBitmap = LoadPNGToHBITMAP(outputPath);

    // Delete temp file
    DeleteFileW(outputPath.c_str());

    return hBitmap;
}

HBITMAP PsdAiThumbnailExtractor::ExtractAI(const std::wstring& path, int size)
{
    // Create temp output path with unique filename (process + thread + tick count)
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring uniqueId = std::to_wstring(GetCurrentProcessId()) + L"_" +
                            std::to_wstring(GetCurrentThreadId()) + L"_" +
                            std::to_wstring(GetTickCount64());
    std::wstring outputPath = std::wstring(tempPath) + L"ai_thumb_" + uniqueId + L".png";

    // Render with Ghostscript DIRECTLY to thumbnail size (no intermediate resize needed)
    // -dDEVICEWIDTHPOINTS/-dDEVICEHEIGHTPOINTS forces output size regardless of input dimensions
    // This dramatically reduces memory usage for large format PDFs/AI files
    std::wstring gsArgs = std::wstring(L"-dNOPAUSE -dBATCH -sDEVICE=png16m ") +
                          L"-dDEVICEWIDTHPOINTS=" + std::to_wstring(size) + L" " +
                          L"-dDEVICEHEIGHTPOINTS=" + std::to_wstring(size) + L" " +
                          L"-dPDFFitPage -dTextAlphaBits=4 -dGraphicsAlphaBits=4 " +
                          L"-sOutputFile=\"" + outputPath + L"\" \"" + path + L"\"";

    if (!RunCommand(m_ghostscriptPath, gsArgs))
    {
        return nullptr;
    }

    // Load the PNG directly (no resize needed)
    HBITMAP hBitmap = LoadPNGToHBITMAP(outputPath);

    // Delete temp file
    DeleteFileW(outputPath.c_str());

    return hBitmap;
}

bool PsdAiThumbnailExtractor::RunCommand(const std::wstring& command, const std::wstring& args)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;  // Hide the console window

    std::wstring cmdLine = L"\"" + command + L"\" " + args;

    // CreateProcess modifies the command line, so make a copy
    std::vector<wchar_t> cmdLineCopy(cmdLine.begin(), cmdLine.end());
    cmdLineCopy.push_back(L'\0');

    if (!CreateProcessW(
        nullptr,
        cmdLineCopy.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi))
    {
        return false;
    }

    // Wait for the process to finish (with timeout)
    DWORD result = WaitForSingleObject(pi.hProcess, 10000);  // 10 second timeout

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (result == WAIT_OBJECT_0 && exitCode == 0);
}

HBITMAP PsdAiThumbnailExtractor::LoadPNGToHBITMAP(const std::wstring& pngPath)
{
    // Load PNG using GDI+
    Gdiplus::Bitmap* gdiBitmap = Gdiplus::Bitmap::FromFile(pngPath.c_str());
    if (!gdiBitmap || gdiBitmap->GetLastStatus() != Gdiplus::Ok)
    {
        if (gdiBitmap)
            delete gdiBitmap;
        return nullptr;
    }

    // Convert to HBITMAP
    HBITMAP hBitmap = nullptr;
    Gdiplus::Color background(255, 255, 255, 255);  // White background
    gdiBitmap->GetHBITMAP(background, &hBitmap);

    delete gdiBitmap;

    return hBitmap;
}
