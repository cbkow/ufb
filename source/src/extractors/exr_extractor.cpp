#define NOMINMAX  // Prevent Windows min/max macros from conflicting with std::min/max
#include "exr_extractor.h"
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfInputPart.h>
#include <Imath/ImathBox.h>
#include <Imath/half.h>
#include <vector>
#include <algorithm>
#include <iostream>
#include <cmath>

EXRExtractor::EXRExtractor()
{
}

EXRExtractor::~EXRExtractor()
{
}

bool EXRExtractor::CanHandle(const std::wstring& extension)
{
    return (extension == L".exr");
}

HBITMAP EXRExtractor::Extract(const std::wstring& path, int size)
{
    try
    {
        // Convert wstring to string for OpenEXR
        std::string path_str(path.begin(), path.end());

        // Open the EXR file
        Imf::MultiPartInputFile file(path_str.c_str());
        const Imf::Header& header = file.header(0);
        const Imath::Box2i displayWindow = header.displayWindow();

        int full_width = displayWindow.max.x - displayWindow.min.x + 1;
        int full_height = displayWindow.max.y - displayWindow.min.y + 1;

        // Calculate skip factor for downsampling
        int max_dim = (std::max)(full_width, full_height);
        int skip_factor = (std::max)(1, max_dim / size);

        int thumb_width = full_width / skip_factor;
        int thumb_height = full_height / skip_factor;

        // Find RGBA channels - try default first, then search for layered channels
        const Imf::ChannelList& channels = header.channels();

        std::string channelR = "R";
        std::string channelG = "G";
        std::string channelB = "B";
        std::string channelA = "A";

        const Imf::Channel* chR = channels.findChannel("R");
        const Imf::Channel* chG = channels.findChannel("G");
        const Imf::Channel* chB = channels.findChannel("B");
        const Imf::Channel* chA = channels.findChannel("A");

        // If default channels not found, search for the first available layer with RGB
        if (!chR || !chG || !chB)
        {
            std::string layerPrefix;
            for (Imf::ChannelList::ConstIterator it = channels.begin(); it != channels.end(); ++it)
            {
                std::string channelName = it.name();

                // Find the LAST dot (for Blender ViewLayer.Combined.R style names)
                size_t dotPos = channelName.find_last_of('.');
                if (dotPos != std::string::npos)
                {
                    // Extract layer prefix (everything before last dot)
                    // E.g., "ViewLayer.Combined.R" -> prefix="ViewLayer.Combined", suffix="R"
                    std::string prefix = channelName.substr(0, dotPos);
                    std::string suffix = channelName.substr(dotPos + 1);

                    // Convert suffix to uppercase for case-insensitive comparison
                    std::string suffixUpper = suffix;
                    std::transform(suffixUpper.begin(), suffixUpper.end(), suffixUpper.begin(), ::toupper);

                    // Check if this is an R channel
                    if (suffixUpper == "R")
                    {
                        // Check if this layer has G and B channels too
                        if (channels.findChannel((prefix + ".G").c_str()) &&
                            channels.findChannel((prefix + ".B").c_str()))
                        {
                            layerPrefix = prefix;
                            break;
                        }
                        // Also check lowercase (some renderers use lowercase)
                        else if (channels.findChannel((prefix + ".g").c_str()) &&
                                 channels.findChannel((prefix + ".b").c_str()))
                        {
                            layerPrefix = prefix;
                            break;
                        }
                    }
                }
            }

            if (!layerPrefix.empty())
            {
                // Try uppercase first
                channelR = layerPrefix + ".R";
                channelG = layerPrefix + ".G";
                channelB = layerPrefix + ".B";
                channelA = layerPrefix + ".A";

                chR = channels.findChannel(channelR.c_str());
                chG = channels.findChannel(channelG.c_str());
                chB = channels.findChannel(channelB.c_str());
                chA = channels.findChannel(channelA.c_str());

                // If uppercase not found, try lowercase
                if (!chR || !chG || !chB)
                {
                    channelR = layerPrefix + ".r";
                    channelG = layerPrefix + ".g";
                    channelB = layerPrefix + ".b";
                    channelA = layerPrefix + ".a";

                    chR = channels.findChannel(channelR.c_str());
                    chG = channels.findChannel(channelG.c_str());
                    chB = channels.findChannel(channelB.c_str());
                    chA = channels.findChannel(channelA.c_str());
                }

                // Successfully found a layer
            }
        }

        if (!chR || !chG || !chB)
        {
            // No suitable RGB channels found - fallback extractor will handle it
            return nullptr;
        }

        bool hasAlpha = (chA != nullptr);

        // Allocate thumbnail buffer (half-float RGBA)
        std::vector<Imath::half> thumb_pixels(thumb_width * thumb_height * 4);

        // Allocate scanline buffer for reading
        std::vector<Imath::half> scanline_buffer(full_width * 4);

        Imf::InputPart part(file, 0);
        Imf::FrameBuffer frameBuffer;

        const size_t channelByteCount = sizeof(Imath::half);
        const size_t cb = 4 * channelByteCount;

        // Setup framebuffer for reading one scanline at a time
        frameBuffer.insert(channelR.c_str(), Imf::Slice(Imf::HALF, (char*)(scanline_buffer.data()) + 0 * channelByteCount, cb, 0, 1, 1, 0.0f));
        frameBuffer.insert(channelG.c_str(), Imf::Slice(Imf::HALF, (char*)(scanline_buffer.data()) + 1 * channelByteCount, cb, 0, 1, 1, 0.0f));
        frameBuffer.insert(channelB.c_str(), Imf::Slice(Imf::HALF, (char*)(scanline_buffer.data()) + 2 * channelByteCount, cb, 0, 1, 1, 0.0f));
        if (hasAlpha)
        {
            frameBuffer.insert(channelA.c_str(), Imf::Slice(Imf::HALF, (char*)(scanline_buffer.data()) + 3 * channelByteCount, cb, 0, 1, 1, 0.0f));
        }

        part.setFrameBuffer(frameBuffer);

        // Read every Nth scanline and downsample horizontally
        for (int thumb_y = 0; thumb_y < thumb_height; thumb_y++)
        {
            int source_y = displayWindow.min.y + (thumb_y * skip_factor);
            if (source_y > displayWindow.max.y) break;

            // Read one scanline
            part.readPixels(source_y, source_y);

            // Downsample horizontally - pick every Nth pixel
            for (int thumb_x = 0; thumb_x < thumb_width; thumb_x++)
            {
                int source_x = thumb_x * skip_factor;
                if (source_x >= full_width) break;

                int src_idx = source_x * 4;
                int dst_idx = (thumb_y * thumb_width + thumb_x) * 4;

                thumb_pixels[dst_idx + 0] = scanline_buffer[src_idx + 0];  // R
                thumb_pixels[dst_idx + 1] = scanline_buffer[src_idx + 1];  // G
                thumb_pixels[dst_idx + 2] = scanline_buffer[src_idx + 2];  // B
                thumb_pixels[dst_idx + 3] = hasAlpha ? scanline_buffer[src_idx + 3] : Imath::half(1.0f);  // A
            }
        }

        // Convert half-float to RGBA8
        std::vector<uint8_t> rgba8;
        ConvertHalfToRGBA8(thumb_pixels.data(), thumb_width, thumb_height, rgba8);

        // Create HBITMAP from RGBA8 data
        HDC hdcScreen = GetDC(nullptr);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = thumb_width;
        bmi.bmiHeader.biHeight = -thumb_height;  // Top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);

        if (hBitmap && pBits)
        {
            // Copy RGBA data to bitmap (convert RGBA to BGRA for Windows)
            uint8_t* dst = (uint8_t*)pBits;
            for (int i = 0; i < thumb_width * thumb_height; i++)
            {
                dst[i * 4 + 0] = rgba8[i * 4 + 2];  // B
                dst[i * 4 + 1] = rgba8[i * 4 + 1];  // G
                dst[i * 4 + 2] = rgba8[i * 4 + 0];  // R
                dst[i * 4 + 3] = rgba8[i * 4 + 3];  // A
            }
        }

        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);

        return hBitmap;
    }
    catch (const std::exception& e)
    {
        // Silently fail - fallback extractor will handle it
        return nullptr;
    }
}

void EXRExtractor::ConvertHalfToRGBA8(const void* halfData, int width, int height, std::vector<uint8_t>& rgba8)
{
    const unsigned short* src = (const unsigned short*)halfData;  // half is 16-bit
    rgba8.resize(width * height * 4);

    for (int i = 0; i < width * height; i++)
    {
        // Manual half-to-float conversion (simple version)
        auto halfToFloat = [](unsigned short h) -> float {
            unsigned int sign = (h >> 15) & 0x1;
            unsigned int exp = (h >> 10) & 0x1F;
            unsigned int mant = h & 0x3FF;

            if (exp == 0) {
                if (mant == 0) return sign ? -0.0f : 0.0f;
                // Denormalized
                float val = mant / 1024.0f;
                return sign ? -val / 16384.0f : val / 16384.0f;
            }
            if (exp == 31) {
                return mant ? NAN : (sign ? -INFINITY : INFINITY);
            }

            int e = exp - 15;
            float val = (1.0f + mant / 1024.0f) * powf(2.0f, (float)e);
            return sign ? -val : val;
        };

        float r = halfToFloat(src[i * 4 + 0]);
        float g = halfToFloat(src[i * 4 + 1]);
        float b = halfToFloat(src[i * 4 + 2]);
        float a = halfToFloat(src[i * 4 + 3]);

        // Simple clamp to [0, 1] range (could use more sophisticated tone mapping)
        r = (std::max)(0.0f, (std::min)(1.0f, r));
        g = (std::max)(0.0f, (std::min)(1.0f, g));
        b = (std::max)(0.0f, (std::min)(1.0f, b));
        a = (std::max)(0.0f, (std::min)(1.0f, a));

        rgba8[i * 4 + 0] = (uint8_t)(r * 255.0f);
        rgba8[i * 4 + 1] = (uint8_t)(g * 255.0f);
        rgba8[i * 4 + 2] = (uint8_t)(b * 255.0f);
        rgba8[i * 4 + 3] = (uint8_t)(a * 255.0f);
    }
}
