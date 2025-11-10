#include "archival_manager.h"
#include "metadata_manager.h"
#include "utils.h"  // For WideToUtf8, Utf8ToWide
#include <fstream>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <set>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif
#include <nlohmann/json.hpp>

// TODO: Add zlib for gzip compression in future iteration
// For MVP, we'll use uncompressed archives (.json instead of .json.gz)
// This still achieves the main goal of separating old/new entries

namespace UFB {

// Clock skew tolerance for P2P sync (in milliseconds)
// Must match the tolerance in metadata_manager.cpp and sync_manager.cpp
constexpr uint64_t CLOCK_SKEW_TOLERANCE_MS = 10000;

ArchivalManager::ArchivalManager()
{
}

ArchivalManager::~ArchivalManager()
{
}

std::map<std::wstring, Shot> ArchivalManager::ReadAllChangeLogs(
    const std::wstring& jobPath,
    const std::wstring& expectedDeviceId,
    uint64_t minTimestamp)
{
    // OPTIMIZATION: Load bootstrap snapshot first as baseline state
    // This provides instant access to all shots on first sync from a new device
    std::map<std::wstring, Shot> bootstrapState = ReadBootstrapSnapshot(jobPath);

    // Retry logic for P2P sync: if we're expecting a specific change and the file content
    // is stale (doesn't contain the expected entry yet), retry the entire read operation
    int maxRetries = 9;
    for (int attempt = 0; attempt < maxRetries; ++attempt)
    {
        if (attempt > 0)
        {
            int delayMs = (std::min)(100 * (1 << attempt), 3000);
            std::wcout << L"[ArchivalManager] Retrying change log read after " << delayMs
                       << L"ms (attempt " << (attempt + 1) << L"/" << maxRetries << L")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }

    std::vector<ChangeLogEntry> allEntries;

    // Find all device change logs (both active and archived)
    std::filesystem::path changesDir = std::filesystem::path(jobPath) / L".ufb" / L"changes";

    if (!std::filesystem::exists(changesDir))
    {
        // No change logs yet - return bootstrap state if available
        if (!bootstrapState.empty())
        {
            std::wcout << L"[ArchivalManager] No change logs found, returning bootstrap state with "
                       << bootstrapState.size() << L" shots" << std::endl;
        }
        return bootstrapState;
    }

    // Collect all device IDs from file names
    std::set<std::string> deviceIds;

    // IMPORTANT: If we're expecting a specific device, add it to the set even if the file
    // doesn't exist yet. This ensures the retry logic in ReadActiveLog gets a chance to
    // wait for the file to sync from the network share or cloud service.
    if (!expectedDeviceId.empty())
    {
        std::string expectedDeviceIdUtf8 = WideToUtf8(expectedDeviceId);
        deviceIds.insert(expectedDeviceIdUtf8);
        std::wcout << L"[ArchivalManager] Expecting change from device: " << expectedDeviceId
                   << L", will retry even if file doesn't exist yet" << std::endl;
    }

    for (const auto& entry : std::filesystem::directory_iterator(changesDir))
    {
        if (entry.is_regular_file())
        {
            std::string filename = entry.path().filename().string();

            // Match pattern: device-{uuid}.json
            if (filename.starts_with("device-") && filename.ends_with(".json"))
            {
                std::string deviceId = filename.substr(7, filename.length() - 12); // Extract UUID
                deviceIds.insert(deviceId);
            }
        }
    }

    // Also check archive directory for additional device IDs
    std::filesystem::path archiveDir = changesDir / L"archive";
    if (std::filesystem::exists(archiveDir))
    {
        for (const auto& entry : std::filesystem::directory_iterator(archiveDir))
        {
            if (entry.is_regular_file())
            {
                std::string filename = entry.path().filename().string();

                // Match pattern: device-{uuid}-YYYY-MM.json
                if (filename.starts_with("device-") && filename.ends_with(".json"))
                {
                    size_t dashPos = filename.find('-', 7);
                    if (dashPos != std::string::npos)
                    {
                        std::string deviceId = filename.substr(7, dashPos - 7);
                        deviceIds.insert(deviceId);
                    }
                }
            }
        }
    }

    // Read all change logs for each device
    for (const auto& deviceId : deviceIds)
    {
        // Skip if we have an expected device ID filter
        if (!expectedDeviceId.empty())
        {
            std::string expectedDeviceIdUtf8 = WideToUtf8(expectedDeviceId);
            if (deviceId != expectedDeviceIdUtf8)
            {
                continue;
            }
        }

        auto deviceEntries = ReadDeviceChangeLogs(jobPath, deviceId);

        // Apply timestamp filter
        if (minTimestamp > 0)
        {
            deviceEntries.erase(
                std::remove_if(deviceEntries.begin(), deviceEntries.end(),
                    [minTimestamp](const ChangeLogEntry& e) { return e.timestamp < minTimestamp; }),
                deviceEntries.end());
        }

        allEntries.insert(allEntries.end(), deviceEntries.begin(), deviceEntries.end());
    }

    // Sort all entries by timestamp (chronological order)
    std::sort(allEntries.begin(), allEntries.end(),
        [](const ChangeLogEntry& a, const ChangeLogEntry& b) {
            if (a.timestamp != b.timestamp)
                return a.timestamp < b.timestamp;
            // Tie-breaker: device ID (alphabetical)
            return a.deviceId < b.deviceId;
        });

        // Content verification: if expecting a specific change, verify it's present in the entries
        // If not found, the file content is stale (hasn't synced yet), so retry
        bool foundExpectedChange = true;  // Assume success if not verifying
        if (!expectedDeviceId.empty() && minTimestamp > 0)
        {
            foundExpectedChange = false;
            uint64_t adjustedMinTimestamp = (minTimestamp > CLOCK_SKEW_TOLERANCE_MS)
                ? (minTimestamp - CLOCK_SKEW_TOLERANCE_MS)
                : 0;

            for (const auto& entry : allEntries)
            {
                std::string deviceIdUtf8 = WideToUtf8(expectedDeviceId);
                if (entry.deviceId == deviceIdUtf8 && entry.data.modifiedTime >= adjustedMinTimestamp)
                {
                    foundExpectedChange = true;
                    std::wcout << L"[ArchivalManager] ✓ Verified: Found expected change with modifiedTime "
                               << entry.data.modifiedTime << L" (>= " << adjustedMinTimestamp << L" with tolerance)" << std::endl;
                    break;
                }
            }

            if (!foundExpectedChange && attempt < maxRetries - 1)
            {
                std::wcerr << L"[ArchivalManager] Expected change not found, content may be stale (will retry)" << std::endl;
                std::wcerr << L"[ArchivalManager] Looking for: deviceId=" << expectedDeviceId
                           << L" modifiedTime>=" << adjustedMinTimestamp << std::endl;
                continue;  // Retry - read the change logs again
            }
            else if (!foundExpectedChange)
            {
                std::wcerr << L"[ArchivalManager] WARNING: Expected change still not found after "
                           << maxRetries << L" attempts" << std::endl;
            }
        }

        // Expected change found (or not verifying) - materialize and return
        return MaterializeState(allEntries, bootstrapState);
    }

    // Should never reach here (loop always returns inside)
    return bootstrapState;
}

std::vector<ChangeLogEntry> ArchivalManager::ReadDeviceChangeLogs(
    const std::wstring& jobPath,
    const std::string& deviceId)
{
    std::vector<ChangeLogEntry> entries;

    // Read archived logs first (chronological order)
    auto archives = FindDeviceArchives(jobPath, deviceId);
    for (const auto& archivePath : archives)
    {
        auto archivedEntries = ReadArchivedLog(archivePath);
        entries.insert(entries.end(), archivedEntries.begin(), archivedEntries.end());
    }

    // Read active log
    // Don't check exists() - let ReadActiveLog's retry logic handle waiting for file to sync
    // This is crucial for P2P sync where files may not be visible yet on network shares
    auto activePath = GetActiveChangeLogPath(jobPath, deviceId);
    auto activeEntries = ReadActiveLog(activePath);
    entries.insert(entries.end(), activeEntries.begin(), activeEntries.end());

    return entries;
}

bool ArchivalManager::ArchiveOldEntries(
    const std::wstring& jobPath,
    const std::string& deviceId,
    int daysThreshold)
{
    // Read active change log
    auto activePath = GetActiveChangeLogPath(jobPath, deviceId);
    if (!std::filesystem::exists(activePath))
    {
        return true; // Nothing to archive
    }

    auto allEntries = ReadActiveLog(activePath);
    if (allEntries.empty())
    {
        return true; // Nothing to archive
    }

    // Calculate threshold timestamp
    auto now = std::chrono::system_clock::now();
    auto thresholdTime = now - std::chrono::hours(24 * daysThreshold);
    uint64_t thresholdMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        thresholdTime.time_since_epoch()).count();

    // Separate old vs recent entries
    std::vector<ChangeLogEntry> oldEntries;
    std::vector<ChangeLogEntry> recentEntries;

    for (const auto& entry : allEntries)
    {
        if (entry.timestamp < thresholdMs)
        {
            oldEntries.push_back(entry);
        }
        else
        {
            recentEntries.push_back(entry);
        }
    }

    if (oldEntries.empty())
    {
        std::cout << "[ArchivalManager] No entries to archive for device " << deviceId << std::endl;
        return true; // Nothing to archive
    }

    // Group old entries by year-month
    std::map<std::pair<int, int>, std::vector<ChangeLogEntry>> entriesByMonth;

    for (const auto& entry : oldEntries)
    {
        int year, month;
        TimestampToYearMonth(entry.timestamp, year, month);
        entriesByMonth[{year, month}].push_back(entry);
    }

    // Write each month to archive
    std::filesystem::path archiveDir = GetArchiveDirectory(jobPath);
    if (!std::filesystem::exists(archiveDir))
    {
        std::filesystem::create_directories(archiveDir);
    }

    for (const auto& [yearMonth, entries] : entriesByMonth)
    {
        auto [year, month] = yearMonth;
        auto archivePath = GetArchivePath(jobPath, deviceId, year, month);

        // If archive already exists, merge with existing entries
        std::vector<ChangeLogEntry> mergedEntries = entries;
        if (std::filesystem::exists(archivePath))
        {
            auto existingEntries = ReadArchivedLog(archivePath);
            mergedEntries.insert(mergedEntries.begin(), existingEntries.begin(), existingEntries.end());

            // Sort by timestamp
            std::sort(mergedEntries.begin(), mergedEntries.end(),
                [](const ChangeLogEntry& a, const ChangeLogEntry& b) {
                    return a.timestamp < b.timestamp;
                });
        }

        if (!WriteArchivedLog(archivePath, mergedEntries))
        {
            std::cerr << "[ArchivalManager] Failed to write archive: " << archivePath << std::endl;
            return false;
        }

        std::cout << "[ArchivalManager] Archived " << entries.size()
                  << " entries to " << archivePath.filename().string() << std::endl;
    }

    // Rewrite active log with only recent entries
    nlohmann::json activeJson = nlohmann::json::array();
    for (const auto& entry : recentEntries)
    {
        nlohmann::json entryJson;
        entryJson["deviceId"] = entry.deviceId;
        entryJson["timestamp"] = entry.timestamp;
        entryJson["operation"] = entry.operation;
        entryJson["shotPath"] = WideToUtf8(entry.shotPath);

        if (entry.operation == "update")
        {
            nlohmann::json shotJson;
            shotJson["shotPath"] = WideToUtf8(entry.data.shotPath);
            shotJson["shotType"] = entry.data.shotType;
            shotJson["displayName"] = WideToUtf8(entry.data.displayName);
            shotJson["metadata"] = nlohmann::json::parse(entry.data.metadata);
            shotJson["createdTime"] = entry.data.createdTime;
            shotJson["modifiedTime"] = entry.data.modifiedTime;
            shotJson["deviceId"] = entry.data.deviceId;
            entryJson["data"] = shotJson;
        }

        activeJson.push_back(entryJson);
    }

    // Write updated active log
    std::ofstream outFile(activePath);
    if (!outFile.is_open())
    {
        std::cerr << "[ArchivalManager] Failed to write active log: " << activePath << std::endl;
        return false;
    }

    outFile << activeJson.dump(2);

    // Explicitly flush to ensure data is written to OS before closing
    outFile.flush();
    if (!outFile.good())
    {
        std::cerr << "[ArchivalManager] Failed to flush active log file" << std::endl;
        outFile.close();
        return false;
    }
    outFile.close();

    // Force OS to sync file to disk/network share (Windows-specific)
#ifdef _WIN32
    HANDLE hFile = CreateFileW(activePath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        if (!FlushFileBuffers(hFile))
        {
            std::wcerr << L"[ArchivalManager] Warning: FlushFileBuffers failed for active log" << std::endl;
        }
        CloseHandle(hFile);
    }
#endif

    std::cout << "[ArchivalManager] Archived " << oldEntries.size()
              << " old entries, kept " << recentEntries.size() << " recent entries" << std::endl;

    return true;
}

std::vector<std::filesystem::path> ArchivalManager::GetArchiveFiles(const std::wstring& jobPath)
{
    std::vector<std::filesystem::path> archives;

    std::filesystem::path archiveDir = GetArchiveDirectory(jobPath);
    if (!std::filesystem::exists(archiveDir))
    {
        return archives;
    }

    for (const auto& entry : std::filesystem::directory_iterator(archiveDir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            archives.push_back(entry.path());
        }
    }

    // Sort by filename (chronological)
    std::sort(archives.begin(), archives.end());

    return archives;
}

// ========================================
// Private Helpers
// ========================================

std::filesystem::path ArchivalManager::GetActiveChangeLogPath(
    const std::wstring& jobPath,
    const std::string& deviceId)
{
    return std::filesystem::path(jobPath) / L".ufb" / L"changes" /
           (L"device-" + std::wstring(deviceId.begin(), deviceId.end()) + L".json");
}

std::filesystem::path ArchivalManager::GetArchiveDirectory(const std::wstring& jobPath)
{
    return std::filesystem::path(jobPath) / L".ufb" / L"changes" / L"archive";
}

std::filesystem::path ArchivalManager::GetArchivePath(
    const std::wstring& jobPath,
    const std::string& deviceId,
    int year,
    int month)
{
    std::ostringstream oss;
    oss << "device-" << deviceId << "-"
        << std::setfill('0') << std::setw(4) << year << "-"
        << std::setfill('0') << std::setw(2) << month << ".json";

    return GetArchiveDirectory(jobPath) / oss.str();
}

std::vector<ChangeLogEntry> ArchivalManager::ReadActiveLog(const std::filesystem::path& path)
{
    std::vector<ChangeLogEntry> entries;

    // Retry logic for file sync services (Dropbox, OneDrive, etc.)
    // File might be locked, partially written, or not yet synced from remote
    // Exponential backoff with cap: 200ms, 400ms, 800ms, 1600ms, 3000ms, 3000ms...
    // Total max wait: ~15 seconds (next sync runs in 30 seconds, so don't block too long)
    int maxRetries = 9;
    bool success = false;

    for (int attempt = 0; attempt < maxRetries && !success; ++attempt)
    {
        if (attempt > 0)
        {
            // Exponential backoff with 3000ms cap (2^attempt * 100, capped at 3000)
            int delayMs = (std::min)(100 * (1 << attempt), 3000);
            std::wcout << L"[ArchivalManager] Retrying after " << delayMs << L"ms (attempt "
                       << (attempt + 1) << L"/" << maxRetries << L")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }

        std::ifstream inFile(path);
        if (!inFile.is_open())
        {
            if (attempt < maxRetries - 1)
            {
                std::wcerr << L"[ArchivalManager] File locked or not found, will retry: " << path << std::endl;
                continue;
            }
            else
            {
                std::wcerr << L"[ArchivalManager] Failed to open file after " << maxRetries << L" attempts: " << path << std::endl;
                return entries;
            }
        }

        try
        {
            // Read entire file content
            std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
            inFile.close();

            // Check if empty (file sync in progress)
            if (content.empty())
            {
                if (attempt < maxRetries - 1)
                {
                    std::wcout << L"[ArchivalManager] File empty (sync in progress), will retry: " << path << std::endl;
                    continue;
                }
                else
                {
                    std::wcerr << L"[ArchivalManager] File still empty after " << maxRetries << L" attempts: " << path << std::endl;
                    return entries;
                }
            }

            // Parse JSON
            nlohmann::json jsonData = nlohmann::json::parse(content);

        if (!jsonData.is_array())
        {
            std::cerr << "[ArchivalManager] Invalid change log format: " << path << std::endl;
            return entries;
        }

        for (const auto& entryJson : jsonData)
        {
            ChangeLogEntry entry;
            entry.deviceId = entryJson.value("deviceId", "");
            entry.timestamp = entryJson.value("timestamp", 0ULL);
            entry.operation = entryJson.value("operation", "");

            std::string shotPathUtf8 = entryJson.value("shotPath", "");
            entry.shotPath = std::wstring(shotPathUtf8.begin(), shotPathUtf8.end());

            if (entry.operation == "update" && entryJson.contains("data"))
            {
                const auto& dataJson = entryJson["data"];

                // shotPath is stored at entry level, not in data object - use entry.shotPath that was already read above
                entry.data.shotPath = entry.shotPath;

                // Read fields using snake_case names as they're stored in the JSON
                entry.data.shotType = dataJson.value("shot_type", "");

                std::string displayNameUtf8 = dataJson.value("display_name", "");
                entry.data.displayName = std::wstring(displayNameUtf8.begin(), displayNameUtf8.end());

                if (dataJson.contains("metadata"))
                {
                    entry.data.metadata = dataJson["metadata"].dump();
                }

                entry.data.createdTime = dataJson.value("created_time", 0ULL);
                entry.data.modifiedTime = dataJson.value("modified_time", 0ULL);
                entry.data.deviceId = dataJson.value("device_id", "");
            }

            entries.push_back(entry);
        }

            // Success! Mark as successful and break out of retry loop
            success = true;
            if (attempt > 0)
            {
                std::wcout << L"[ArchivalManager] Successfully read change log after " << (attempt + 1) << L" attempts" << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            if (attempt < maxRetries - 1)
            {
                std::wcerr << L"[ArchivalManager] Parse error, will retry: " << path
                           << L" - " << e.what() << std::endl;
                entries.clear();  // Clear partial data
                continue;
            }
            else
            {
                std::cerr << "[ArchivalManager] Failed to parse change log after " << maxRetries
                          << " attempts: " << path << " - " << e.what() << std::endl;
            }
        }
    }

    return entries;
}

std::vector<ChangeLogEntry> ArchivalManager::ReadArchivedLog(const std::filesystem::path& path)
{
    // For MVP, archives are uncompressed JSON files
    // TODO: Add gzip decompression when compression is implemented
    return ReadActiveLog(path);
}

bool ArchivalManager::WriteArchivedLog(
    const std::filesystem::path& path,
    const std::vector<ChangeLogEntry>& entries)
{
    // For MVP, write uncompressed JSON
    // TODO: Add gzip compression when compression is implemented

    nlohmann::json archiveJson = nlohmann::json::array();

    for (const auto& entry : entries)
    {
        nlohmann::json entryJson;
        entryJson["deviceId"] = entry.deviceId;
        entryJson["timestamp"] = entry.timestamp;
        entryJson["operation"] = entry.operation;
        entryJson["shotPath"] = WideToUtf8(entry.shotPath);

        if (entry.operation == "update")
        {
            nlohmann::json shotJson;
            shotJson["shotPath"] = WideToUtf8(entry.data.shotPath);
            shotJson["shotType"] = entry.data.shotType;
            shotJson["displayName"] = WideToUtf8(entry.data.displayName);
            shotJson["metadata"] = nlohmann::json::parse(entry.data.metadata);
            shotJson["createdTime"] = entry.data.createdTime;
            shotJson["modifiedTime"] = entry.data.modifiedTime;
            shotJson["deviceId"] = entry.data.deviceId;
            entryJson["data"] = shotJson;
        }

        archiveJson.push_back(entryJson);
    }

    std::ofstream outFile(path);
    if (!outFile.is_open())
    {
        return false;
    }

    outFile << archiveJson.dump(2);

    // Explicitly flush to ensure data is written to OS before closing
    outFile.flush();
    if (!outFile.good())
    {
        std::cerr << "[ArchivalManager] Failed to flush archived log file" << std::endl;
        outFile.close();
        return false;
    }
    outFile.close();

    // Force OS to sync file to disk/network share (Windows-specific)
#ifdef _WIN32
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        if (!FlushFileBuffers(hFile))
        {
            std::wcerr << L"[ArchivalManager] Warning: FlushFileBuffers failed for archived log" << std::endl;
        }
        CloseHandle(hFile);
    }
#endif

    return true;
}

std::vector<std::filesystem::path> ArchivalManager::FindDeviceArchives(
    const std::wstring& jobPath,
    const std::string& deviceId)
{
    std::vector<std::filesystem::path> archives;

    std::filesystem::path archiveDir = GetArchiveDirectory(jobPath);
    if (!std::filesystem::exists(archiveDir))
    {
        return archives;
    }

    std::string prefix = "device-" + deviceId + "-";

    for (const auto& entry : std::filesystem::directory_iterator(archiveDir))
    {
        if (entry.is_regular_file())
        {
            std::string filename = entry.path().filename().string();

            if (filename.starts_with(prefix) && filename.ends_with(".json"))
            {
                archives.push_back(entry.path());
            }
        }
    }

    // Sort chronologically by filename (YYYY-MM is sortable)
    std::sort(archives.begin(), archives.end());

    return archives;
}

bool ArchivalManager::ParseArchiveDate(
    const std::string& filename,
    int& outYear,
    int& outMonth)
{
    // Format: device-{uuid}-YYYY-MM.json
    // Find the last occurrence of pattern YYYY-MM

    size_t lastDash = filename.rfind('-');
    if (lastDash == std::string::npos || lastDash < 5)
    {
        return false;
    }

    size_t secondLastDash = filename.rfind('-', lastDash - 1);
    if (secondLastDash == std::string::npos)
    {
        return false;
    }

    try
    {
        std::string yearStr = filename.substr(secondLastDash + 1, 4);
        std::string monthStr = filename.substr(lastDash + 1, 2);

        outYear = std::stoi(yearStr);
        outMonth = std::stoi(monthStr);

        return (outYear >= 2000 && outYear <= 2100 && outMonth >= 1 && outMonth <= 12);
    }
    catch (...)
    {
        return false;
    }
}

void ArchivalManager::TimestampToYearMonth(
    uint64_t timestampMs,
    int& outYear,
    int& outMonth)
{
    std::chrono::milliseconds ms(timestampMs);
    std::chrono::system_clock::time_point tp(ms);
    std::time_t time = std::chrono::system_clock::to_time_t(tp);

    std::tm* tm = std::gmtime(&time);
    outYear = tm->tm_year + 1900;
    outMonth = tm->tm_mon + 1;
}

std::map<std::wstring, Shot> ArchivalManager::MaterializeState(
    const std::vector<ChangeLogEntry>& entries,
    std::map<std::wstring, Shot> initialState)
{
    // Start with initial state (e.g., from bootstrap snapshot)
    std::map<std::wstring, Shot> state = std::move(initialState);

    // Apply all change log entries on top of initial state
    for (const auto& entry : entries)
    {
        if (entry.operation == "update")
        {
            // Last-write-wins: just overwrite
            state[entry.shotPath] = entry.data;
        }
        else if (entry.operation == "delete")
        {
            // Remove from state
            state.erase(entry.shotPath);
        }
    }

    return state;
}

std::vector<uint8_t> ArchivalManager::CompressGzip(const std::string& jsonStr)
{
    // TODO: Implement gzip compression using zlib
    // For now, return empty vector (compression not implemented)
    std::cerr << "[ArchivalManager] Gzip compression not yet implemented" << std::endl;
    return {};
}

std::string ArchivalManager::DecompressGzip(const std::vector<uint8_t>& compressedData)
{
    // TODO: Implement gzip decompression using zlib
    // For now, return empty string (decompression not implemented)
    std::cerr << "[ArchivalManager] Gzip decompression not yet implemented" << std::endl;
    return "";
}

// ========================================
// Bootstrap Snapshot
// ========================================

bool ArchivalManager::CreateBootstrapSnapshot(const std::wstring& jobPath)
{
    std::wcout << L"[ArchivalManager] Creating bootstrap snapshot for: " << jobPath << std::endl;

    // Materialize current state from all change logs
    auto currentState = ReadAllChangeLogs(jobPath);

    if (currentState.empty())
    {
        std::wcout << L"[ArchivalManager] No shots to snapshot, skipping" << std::endl;
        return true; // Not an error, just nothing to snapshot
    }

    // Convert to JSON
    nlohmann::json snapshotJson;
    snapshotJson["version"] = 1;
    snapshotJson["created"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    nlohmann::json shotsArray = nlohmann::json::array();
    for (const auto& [shotPath, shot] : currentState)
    {
        nlohmann::json shotJson;
        shotJson["shotPath"] = WideToUtf8(shot.shotPath);
        shotJson["shotType"] = shot.shotType;
        shotJson["displayName"] = WideToUtf8(shot.displayName);
        shotJson["metadata"] = nlohmann::json::parse(shot.metadata);
        shotJson["createdTime"] = shot.createdTime;
        shotJson["modifiedTime"] = shot.modifiedTime;
        shotJson["deviceId"] = shot.deviceId;
        shotsArray.push_back(shotJson);
    }

    snapshotJson["shots"] = shotsArray;
    snapshotJson["shotCount"] = shotsArray.size();

    // Write snapshot file
    std::filesystem::path snapshotPath = GetBootstrapSnapshotPath(jobPath);

    // Ensure directory exists
    std::filesystem::path changesDir = snapshotPath.parent_path();
    if (!std::filesystem::exists(changesDir))
    {
        std::filesystem::create_directories(changesDir);
    }

    std::ofstream outFile(snapshotPath);
    if (!outFile.is_open())
    {
        std::wcerr << L"[ArchivalManager] Failed to create snapshot file: " << snapshotPath << std::endl;
        return false;
    }

    outFile << snapshotJson.dump(2);
    outFile.close();

    std::wcout << L"[ArchivalManager] Created bootstrap snapshot with " << currentState.size()
               << L" shots" << std::endl;

    return true;
}

bool ArchivalManager::HasRecentBootstrapSnapshot(const std::wstring& jobPath, int maxAgeHours)
{
    std::filesystem::path snapshotPath = GetBootstrapSnapshotPath(jobPath);

    if (!std::filesystem::exists(snapshotPath))
    {
        return false;
    }

    // Check file modification time
    auto fileTime = std::filesystem::last_write_time(snapshotPath);
    auto now = std::filesystem::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::hours>(now - fileTime);

    return (age.count() < maxAgeHours);
}

std::filesystem::path ArchivalManager::GetBootstrapSnapshotPath(const std::wstring& jobPath)
{
    return std::filesystem::path(jobPath) / L".ufb" / L"changes" / L"bootstrap-snapshot.json";
}

std::map<std::wstring, Shot> ArchivalManager::ReadBootstrapSnapshot(const std::wstring& jobPath)
{
    std::map<std::wstring, Shot> result;
    std::filesystem::path snapshotPath = GetBootstrapSnapshotPath(jobPath);

    if (!std::filesystem::exists(snapshotPath))
    {
        std::wcout << L"[ArchivalManager] No bootstrap snapshot found at: " << snapshotPath << std::endl;
        return result;
    }

    std::wcout << L"[ArchivalManager] Loading bootstrap snapshot from: " << snapshotPath << std::endl;

    try
    {
        std::ifstream inFile(snapshotPath);
        if (!inFile.is_open())
        {
            std::wcerr << L"[ArchivalManager] Failed to open bootstrap snapshot file" << std::endl;
            return result;
        }

        nlohmann::json snapshotJson;
        inFile >> snapshotJson;

        if (!snapshotJson.contains("shots") || !snapshotJson["shots"].is_array())
        {
            std::wcerr << L"[ArchivalManager] Invalid bootstrap snapshot format" << std::endl;
            return result;
        }

        const auto& shotsArray = snapshotJson["shots"];

        for (const auto& shotJson : shotsArray)
        {
            Shot shot;

            std::string shotPathUtf8 = shotJson.value("shotPath", "");
            shot.shotPath = std::wstring(shotPathUtf8.begin(), shotPathUtf8.end());
            shot.shotType = shotJson.value("shotType", "");

            std::string displayNameUtf8 = shotJson.value("displayName", "");
            shot.displayName = std::wstring(displayNameUtf8.begin(), displayNameUtf8.end());

            if (shotJson.contains("metadata"))
            {
                shot.metadata = shotJson["metadata"].dump();
            }

            shot.createdTime = shotJson.value("createdTime", 0ULL);
            shot.modifiedTime = shotJson.value("modifiedTime", 0ULL);
            shot.deviceId = shotJson.value("deviceId", "");

            result[shot.shotPath] = shot;
        }

        std::wcout << L"[ArchivalManager] Loaded " << result.size() << L" shots from bootstrap snapshot" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ArchivalManager] Failed to parse bootstrap snapshot: " << e.what() << std::endl;
    }

    return result;
}

} // namespace UFB
