#include "utils.h"
#include <Windows.h>
#include <ShlObj.h>
#include <rpc.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "Rpcrt4.lib")

namespace UFB {

std::filesystem::path GetLocalAppDataPath()
{
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)))
    {
        std::filesystem::path ufbPath = std::filesystem::path(localAppData) / L"ufb";
        EnsureDirectoryExists(ufbPath);
        return ufbPath;
    }

    // Fallback to current directory if unable to get localappdata
    return std::filesystem::current_path() / L"ufb_data";
}

std::string GenerateDeviceID()
{
    UUID uuid;
    UuidCreate(&uuid);

    RPC_CSTR uuidStr;
    UuidToStringA(&uuid, &uuidStr);

    std::string result(reinterpret_cast<char*>(uuidStr));
    RpcStringFreeA(&uuidStr);

    return result;
}

std::string GetDeviceID()
{
    std::filesystem::path deviceIdPath = GetLocalAppDataPath() / L"device_id.txt";

    // Try to read existing device ID
    if (std::filesystem::exists(deviceIdPath))
    {
        std::ifstream file(deviceIdPath);
        if (file.is_open())
        {
            std::string deviceId;
            std::getline(file, deviceId);
            if (!deviceId.empty())
            {
                return deviceId;
            }
        }
    }

    // Generate new device ID
    std::string newDeviceId = GenerateDeviceID();

    // Save to file
    std::ofstream file(deviceIdPath);
    if (file.is_open())
    {
        file << newDeviceId;
    }

    return newDeviceId;
}

bool EnsureDirectoryExists(const std::filesystem::path& path)
{
    try
    {
        if (!std::filesystem::exists(path))
        {
            return std::filesystem::create_directories(path);
        }
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

uint64_t GetCurrentTimeMs()
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

std::string WideToUtf8(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), nullptr, 0, nullptr, nullptr);
    std::string result(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), &result[0], sizeNeeded, nullptr, nullptr);

    return result;
}

std::wstring Utf8ToWide(const std::string& str)
{
    if (str.empty()) return std::wstring();

    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), nullptr, 0);
    std::wstring result(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), &result[0], sizeNeeded);

    return result;
}

// ============================================================================
// URI ENCODING/DECODING FOR PATH SHARING
// ============================================================================

std::string EncodeURIComponent(const std::string& str)
{
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : str) {
        // Keep alphanumeric and certain safe characters
        // Note: Keeping / \ and : for path compatibility
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == '\\' || c == ':') {
            escaped << c;
        } else {
            // Encode other characters as %XX
            escaped << '%' << std::setw(2) << int((unsigned char)c);
        }
    }

    return escaped.str();
}

std::string DecodeURIComponent(const std::string& str)
{
    std::ostringstream decoded;
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            // Decode hex sequence
            int value;
            std::istringstream is(str.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                decoded << static_cast<char>(value);
                i += 2;
            }
        } else {
            decoded << str[i];
        }
    }
    return decoded.str();
}

std::string BuildPathURI(const std::wstring& path)
{
    // Convert wide string to UTF-8
    std::string utf8_path = WideToUtf8(path);

    // Convert backslashes to forward slashes for URI compatibility
    std::string normalized_path = utf8_path;
    std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');

    // Encode the path for URI
    std::string encoded_path = EncodeURIComponent(normalized_path);

    // Build URI: ufb:///path
    return "ufb:///" + encoded_path;
}

std::wstring ParsePathURI(const std::string& uri)
{
    // Check if it starts with ufb:///
    if (uri.substr(0, 7) != "ufb:///") {
        return L"";
    }

    // Extract path after ufb:///
    std::string encoded_path = uri.substr(7);

    // Decode the path
    std::string decoded_path = DecodeURIComponent(encoded_path);

    // Convert back to Windows path (forward slashes to backslashes on Windows)
    #ifdef _WIN32
    std::replace(decoded_path.begin(), decoded_path.end(), '/', '\\');
    #endif

    // Convert to wide string
    return Utf8ToWide(decoded_path);
}

// ============================================================================
// BASE64 ENCODING/DECODING FOR API KEYS
// ============================================================================

static const std::string base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

std::string Base64Encode(const std::string& input)
{
    std::string result;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];
    size_t in_len = input.length();
    const unsigned char* bytes_to_encode = reinterpret_cast<const unsigned char*>(input.c_str());

    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; i < 4; i++)
                result += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

        for (j = 0; j < i + 1; j++)
            result += base64_chars[char_array_4[j]];

        while (i++ < 3)
            result += '=';
    }

    return result;
}

std::string Base64Decode(const std::string& input)
{
    size_t in_len = input.length();
    int i = 0;
    int j = 0;
    int in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];
    std::string result;

    while (in_len-- && (input[in_] != '=')) {
        if (!isalnum(input[in_]) && input[in_] != '+' && input[in_] != '/') {
            in_++;
            continue;
        }

        char_array_4[i++] = input[in_]; in_++;
        if (i == 4) {
            for (i = 0; i < 4; i++)
                char_array_4[i] = static_cast<unsigned char>(base64_chars.find(char_array_4[i]));

            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

            for (i = 0; i < 3; i++)
                result += char_array_3[i];
            i = 0;
        }
    }

    if (i) {
        for (j = 0; j < i; j++)
            char_array_4[j] = static_cast<unsigned char>(base64_chars.find(char_array_4[j]));

        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);

        for (j = 0; j < i - 1; j++)
            result += char_array_3[j];
    }

    return result;
}

} // namespace UFB
