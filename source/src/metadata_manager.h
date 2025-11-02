#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

namespace UFB {

class SubscriptionManager; // Forward declaration

// Shot data structure
struct Shot
{
    std::wstring shotPath;          // Relative path within job (e.g., "seq01/shot010")
    std::string shotType;           // Template ID (e.g., "vfx_shot")
    std::wstring displayName;       // User-friendly name
    std::string metadata;           // JSON blob of type-specific fields
    uint64_t createdTime = 0;       // Unix timestamp (ms)
    uint64_t modifiedTime = 0;      // Unix timestamp (ms)
    std::string deviceId;           // Device that made last change
};

// Write queue entry for batching
struct WriteQueueEntry
{
    std::wstring jobPath;
    std::wstring shotPath;
    Shot shot;
    uint64_t queuedTime;
};

class MetadataManager
{
public:
    MetadataManager();
    ~MetadataManager();

    // Initialize with subscription manager reference
    bool Initialize(SubscriptionManager* subManager);

    // Shutdown and cleanup
    void Shutdown();

    // Shot operations (within a job)
    bool AssignShot(const std::wstring& jobPath, const std::wstring& shotPath, const std::string& shotType);
    bool UpdateShotMetadata(const std::wstring& jobPath, const std::wstring& shotPath, const std::string& metadataJson);
    std::optional<Shot> GetShot(const std::wstring& jobPath, const std::wstring& shotPath);
    std::vector<Shot> GetAllShots(const std::wstring& jobPath);
    bool RemoveShot(const std::wstring& jobPath, const std::wstring& shotPath);

    // Batching (write to shared JSON)
    void QueueWrite(const std::wstring& jobPath, const Shot& shot);
    void FlushPendingWrites(const std::wstring& jobPath);
    void FlushAllPendingWrites();

    // Sync support - cache operations
    std::vector<Shot> GetCachedShots(const std::wstring& jobPath);
    void UpdateCache(const std::wstring& jobPath, const std::vector<Shot>& shots);
    void ClearCache(const std::wstring& jobPath);

    // Shared JSON operations
    bool ReadSharedJSON(const std::wstring& jobPath, std::map<std::wstring, Shot>& outShots);
    bool WriteSharedJSON(const std::wstring& jobPath, const std::map<std::wstring, Shot>& shots);

private:
    SubscriptionManager* m_subManager = nullptr;
    sqlite3* m_db = nullptr;
    std::filesystem::path m_dbPath;
    std::mutex m_writeMutex;

    // Write queue for batching
    std::vector<WriteQueueEntry> m_writeQueue;
    std::chrono::steady_clock::time_point m_lastFlush;

    // Internal helpers
    bool CreateCacheTable();
    bool ExecuteSQL(const char* sql);

    // Cache operations (internal)
    bool InsertOrUpdateCache(const std::wstring& jobPath, const Shot& shot);
    bool DeleteFromCache(const std::wstring& jobPath, const std::wstring& shotPath);

    // JSON helpers
    nlohmann::json ShotToJson(const Shot& shot);
    Shot JsonToShot(const nlohmann::json& json, const std::wstring& shotPath);
    std::filesystem::path GetSharedJSONPath(const std::wstring& jobPath);
    bool EnsureUFBDirectory(const std::wstring& jobPath);
};

} // namespace UFB
