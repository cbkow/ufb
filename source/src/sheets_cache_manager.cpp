#include "sheets_cache_manager.h"
#include "utils.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

// Helper to compute simple hash for change detection
std::string CachedSheetRow::Hash() const
{
    std::ostringstream oss;
    oss << itemPath << "|"
        << itemType << "|"
        << folderType << "|"
        << status << "|"
        << category << "|"
        << notes << "|"
        << assignedArtist << "|"
        << priority << "|"
        << deliveryDate << "|"
        << clientApproval << "|"
        << modifiedTime;
    return oss.str();
}

nlohmann::json CachedSheetRow::ToJson() const
{
    nlohmann::json j;
    j["itemPath"] = itemPath;
    j["itemType"] = itemType;
    j["folderType"] = folderType;
    j["status"] = status;
    j["category"] = category;
    j["notes"] = notes;
    j["assignedArtist"] = assignedArtist;
    j["priority"] = priority;
    j["deliveryDate"] = deliveryDate;
    j["clientApproval"] = clientApproval;
    j["modifiedTimeStr"] = modifiedTimeStr;
    j["modifiedTime"] = modifiedTime;
    j["syncedTime"] = syncedTime;
    j["deviceId"] = deviceId;
    return j;
}

CachedSheetRow CachedSheetRow::FromJson(const nlohmann::json& j)
{
    CachedSheetRow row;
    row.itemPath = j.value("itemPath", "");
    row.itemType = j.value("itemType", "");
    row.folderType = j.value("folderType", "");
    row.status = j.value("status", "");
    row.category = j.value("category", "");
    row.notes = j.value("notes", "");
    row.assignedArtist = j.value("assignedArtist", "");
    row.priority = j.value("priority", "");
    row.deliveryDate = j.value("deliveryDate", "");
    row.clientApproval = j.value("clientApproval", "");
    row.modifiedTimeStr = j.value("modifiedTimeStr", "");
    row.modifiedTime = j.value("modifiedTime", 0ULL);
    row.syncedTime = j.value("syncedTime", 0ULL);
    row.deviceId = j.value("deviceId", "");
    return row;
}

nlohmann::json SheetTabCache::ToJson() const
{
    nlohmann::json j;
    j["spreadsheetId"] = spreadsheetId;
    j["tabName"] = tabName;
    j["lastSyncTime"] = lastSyncTime;

    nlohmann::json rowsArray = nlohmann::json::array();
    for (const auto& [path, row] : rows)
    {
        rowsArray.push_back(row.ToJson());
    }
    j["rows"] = rowsArray;

    return j;
}

SheetTabCache SheetTabCache::FromJson(const nlohmann::json& j)
{
    SheetTabCache cache;
    cache.spreadsheetId = j.value("spreadsheetId", "");
    cache.tabName = j.value("tabName", "");
    cache.lastSyncTime = j.value("lastSyncTime", 0ULL);

    if (j.contains("rows") && j["rows"].is_array())
    {
        for (const auto& rowJson : j["rows"])
        {
            CachedSheetRow row = CachedSheetRow::FromJson(rowJson);
            cache.rows[row.itemPath] = row;
        }
    }

    return cache;
}

SheetsCacheManager::SheetsCacheManager()
{
}

SheetsCacheManager::~SheetsCacheManager()
{
}

void SheetsCacheManager::Initialize(const std::wstring& jobPath)
{
    m_jobPath = jobPath;

    // Cache directory: <jobPath>/.ufb/sheets_cache/
    m_cacheDir = jobPath + L"\\.ufb\\sheets_cache";

    // Create directory if it doesn't exist
    if (!std::filesystem::exists(m_cacheDir))
    {
        try
        {
            std::filesystem::create_directories(m_cacheDir);
            std::wcout << L"[SheetsCacheManager] Created cache directory: " << m_cacheDir << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[SheetsCacheManager] Failed to create cache directory: " << e.what() << std::endl;
        }
    }
}

std::wstring SheetsCacheManager::GetCacheFilePath(const std::string& spreadsheetId, const std::string& tabName) const
{
    // Sanitize tab name for filename
    std::string sanitized = tabName;
    for (char& c : sanitized)
    {
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
    }

    std::wstring filename = UFB::Utf8ToWide(spreadsheetId + "_" + sanitized + ".json");
    return m_cacheDir + L"\\" + filename;
}

bool SheetsCacheManager::LoadCache(const std::string& spreadsheetId, const std::string& tabName, SheetTabCache& outCache)
{
    std::wstring cachePath = GetCacheFilePath(spreadsheetId, tabName);

    if (!std::filesystem::exists(cachePath))
    {
        // No cache exists - return empty cache
        outCache.spreadsheetId = spreadsheetId;
        outCache.tabName = tabName;
        outCache.rows.clear();
        outCache.lastSyncTime = 0;
        return false;
    }

    try
    {
        std::ifstream file(cachePath);
        if (!file.is_open())
        {
            std::wcerr << L"[SheetsCacheManager] Failed to open cache file: " << cachePath << std::endl;
            return false;
        }

        nlohmann::json j;
        file >> j;
        file.close();

        outCache = SheetTabCache::FromJson(j);

        std::wcout << L"[SheetsCacheManager] Loaded cache for " << UFB::Utf8ToWide(tabName)
                   << L" (" << outCache.rows.size() << L" rows)" << std::endl;

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SheetsCacheManager] Failed to load cache: " << e.what() << std::endl;
        return false;
    }
}

bool SheetsCacheManager::SaveCache(const SheetTabCache& cache)
{
    std::wstring cachePath = GetCacheFilePath(cache.spreadsheetId, cache.tabName);

    try
    {
        nlohmann::json j = cache.ToJson();

        std::ofstream file(cachePath);
        if (!file.is_open())
        {
            std::wcerr << L"[SheetsCacheManager] Failed to open cache file for writing: " << cachePath << std::endl;
            return false;
        }

        file << j.dump(2);
        file.close();

        std::wcout << L"[SheetsCacheManager] Saved cache for " << UFB::Utf8ToWide(cache.tabName)
                   << L" (" << cache.rows.size() << L" rows)" << std::endl;

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SheetsCacheManager] Failed to save cache: " << e.what() << std::endl;
        return false;
    }
}

void SheetsCacheManager::ClearCache(const std::string& spreadsheetId, const std::string& tabName)
{
    std::wstring cachePath = GetCacheFilePath(spreadsheetId, tabName);

    if (std::filesystem::exists(cachePath))
    {
        try
        {
            std::filesystem::remove(cachePath);
            std::wcout << L"[SheetsCacheManager] Cleared cache for " << UFB::Utf8ToWide(tabName) << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[SheetsCacheManager] Failed to clear cache: " << e.what() << std::endl;
        }
    }
}

void SheetsCacheManager::ClearAllCaches()
{
    if (!std::filesystem::exists(m_cacheDir))
        return;

    try
    {
        int count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(m_cacheDir))
        {
            if (entry.path().extension() == ".json")
            {
                std::filesystem::remove(entry.path());
                count++;
            }
        }

        std::wcout << L"[SheetsCacheManager] Cleared " << count << L" cache files" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SheetsCacheManager] Failed to clear all caches: " << e.what() << std::endl;
    }
}

SheetsCacheManager::ChangeDetection SheetsCacheManager::DetectChanges(const SheetTabCache& oldCache, const SheetTabCache& newCache)
{
    ChangeDetection changes;

    // Find added and modified rows
    for (const auto& [path, newRow] : newCache.rows)
    {
        auto it = oldCache.rows.find(path);
        if (it == oldCache.rows.end())
        {
            // New row added
            changes.addedRows.push_back(newRow);
        }
        else
        {
            // Check if modified (compare hashes)
            if (newRow.Hash() != it->second.Hash())
            {
                changes.modifiedRows.push_back(newRow);
            }
        }
    }

    // Find deleted rows
    for (const auto& [path, oldRow] : oldCache.rows)
    {
        if (newCache.rows.find(path) == newCache.rows.end())
        {
            changes.deletedPaths.push_back(path);
        }
    }

    return changes;
}
