#include "utils.h"
#include <Windows.h>
#include <ShlObj.h>
#include <rpc.h>
#include <fstream>
#include <sstream>
#include <chrono>

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

} // namespace UFB
