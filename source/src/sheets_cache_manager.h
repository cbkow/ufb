#pragma once

#include <windows.h>
#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include "nlohmann/json.hpp"

// Structure representing a cached row from Google Sheets (15 columns A-O)
// Note: Column A (Name) is not cached, it's derived from itemPath for display only
struct CachedSheetRow
{
    std::string itemPath;           // Column B: Shot Path (unique identifier)
    std::string itemType;           // Column C: Item Type
    std::string folderType;         // Column D: Folder Type
    std::string status;             // Column E: Status
    std::string category;           // Column F: Category
    std::string priority;           // Column G: Priority (HIGH/MEDIUM/LOW)
    std::string deliveryDate;       // Column H: Due Date (YYYY-MM-DD)
    std::string assignedArtist;     // Column I: Artist
    std::string notes;              // Column J: Note
    std::string clientApproval;     // Column K: Links (field name is legacy, contains links)
    std::string modifiedTimeStr;    // Column L: Last Modified (human-readable)
    uint64_t modifiedTime = 0;      // Column M: ModifiedTime (ms, tracking - hidden)
    uint64_t syncedTime = 0;        // Column N: SyncedTime (ms, tracking - hidden)
    std::string deviceId;           // Column O: Device ID (tracking - hidden)

    // Generate hash for change detection
    std::string Hash() const;

    // Convert to/from JSON
    nlohmann::json ToJson() const;
    static CachedSheetRow FromJson(const nlohmann::json& j);
};

// Cache for a single sheet tab
struct SheetTabCache
{
    std::string spreadsheetId;
    std::string tabName;
    std::map<std::string, CachedSheetRow> rows;  // Key: itemPath
    uint64_t lastSyncTime = 0;

    nlohmann::json ToJson() const;
    static SheetTabCache FromJson(const nlohmann::json& j);
};

class SheetsCacheManager
{
public:
    SheetsCacheManager();
    ~SheetsCacheManager();

    // Initialize with job path
    void Initialize(const std::wstring& jobPath);

    // Load cache from disk for a specific spreadsheet/tab
    bool LoadCache(const std::string& spreadsheetId, const std::string& tabName, SheetTabCache& outCache);

    // Save cache to disk
    bool SaveCache(const SheetTabCache& cache);

    // Clear cache for a specific spreadsheet/tab
    void ClearCache(const std::string& spreadsheetId, const std::string& tabName);

    // Clear all caches for current job
    void ClearAllCaches();

    // Detect changes between cached state and new data
    struct ChangeDetection
    {
        std::vector<CachedSheetRow> addedRows;
        std::vector<CachedSheetRow> modifiedRows;
        std::vector<std::string> deletedPaths;
    };
    ChangeDetection DetectChanges(const SheetTabCache& oldCache, const SheetTabCache& newCache);

    // Get cache directory path
    std::wstring GetCacheDirectory() const { return m_cacheDir; }

private:
    std::wstring m_jobPath;
    std::wstring m_cacheDir;  // .ufb/sheets_cache/

    // Generate cache file path for spreadsheet/tab
    std::wstring GetCacheFilePath(const std::string& spreadsheetId, const std::string& tabName) const;
};
