#pragma once

#include <string>
#include <filesystem>
#include <optional>

namespace UFB {

// Get the %localappdata%/ufb/ directory path
std::filesystem::path GetLocalAppDataPath();

// Get or generate persistent device ID (stored in %localappdata%/ufb/device_id.txt)
std::string GetDeviceID();

// Generate a new GUID-based device ID
std::string GenerateDeviceID();

// Ensure directory exists (create if needed)
bool EnsureDirectoryExists(const std::filesystem::path& path);

// Get current timestamp in milliseconds since epoch
uint64_t GetCurrentTimeMs();

// Convert wstring to UTF-8 string
std::string WideToUtf8(const std::wstring& wstr);

// Convert UTF-8 string to wstring
std::wstring Utf8ToWide(const std::string& str);

// URI encoding/decoding for path sharing
std::string EncodeURIComponent(const std::string& str);
std::string DecodeURIComponent(const std::string& str);
std::string BuildPathURI(const std::wstring& path);
std::wstring ParsePathURI(const std::string& uri);

// Base64 encoding/decoding for API keys
std::string Base64Encode(const std::string& input);
std::string Base64Decode(const std::string& input);

} // namespace UFB
