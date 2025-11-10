#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <memory>
#include <functional>
#include <mutex>
#include <sqlite3.h>

namespace UFB {

// Sync status for subscriptions
enum class SyncStatus
{
    Pending,
    Syncing,
    Synced,
    Stale,
    Error
};

// Subscription data structure
struct Subscription
{
    int id = 0;
    std::wstring jobPath;           // Path to project root
    std::wstring jobName;           // User-friendly name
    bool isActive = true;           // Active or inactive
    uint64_t subscribedTime = 0;    // Unix timestamp (ms)
    uint64_t lastSyncTime = 0;      // Last successful sync (ms)
    SyncStatus syncStatus = SyncStatus::Pending;
    int shotCount = 0;              // Cached shot count for UI
};

// Shot metadata structure (for individual shot folders)
// NOTE: This structure is also used for assets, postings, and manual tasks
struct ShotMetadata
{
    int id = 0;
    std::wstring shotPath;          // Absolute path to shot folder (or unique ID for manual tasks)
    std::string itemType;           // Item type: "shot", "asset", "posting", "manual_task"
    std::string folderType;         // Folder type from template (e.g., "3d", "ae")
    std::string status;             // Status from template options
    std::string category;           // Category from template options
    int priority = 2;               // 1=High, 2=Medium, 3=Low
    uint64_t dueDate = 0;           // Unix timestamp (ms)
    std::string artist;             // Assigned artist name
    std::string note;               // User notes
    std::string links;              // JSON array of links
    bool isTracked = false;         // Whether to track this shot
    uint64_t createdTime = 0;       // Unix timestamp (ms)
    uint64_t modifiedTime = 0;      // Unix timestamp (ms)
};

// Typedef for clarity (Shot/Asset/Posting/Task metadata all use same structure)
using ItemMetadata = ShotMetadata;

// Forward declaration
class MetadataManager;

class SubscriptionManager
{
public:
    SubscriptionManager();
    ~SubscriptionManager();

    // Initialize the subscription database
    bool Initialize();

    // Set MetadataManager for bridging shot_metadata <-> shot_cache
    void SetMetadataManager(MetadataManager* metaManager);

    // Shutdown and cleanup
    void Shutdown();

    // Subscription operations
    bool SubscribeToJob(const std::wstring& jobPath, const std::wstring& jobName);
    bool UnsubscribeFromJob(const std::wstring& jobPath);
    bool SetJobActive(const std::wstring& jobPath, bool active);

    // Query subscriptions
    std::vector<Subscription> GetAllSubscriptions();
    std::vector<Subscription> GetActiveSubscriptions();
    std::optional<Subscription> GetSubscription(const std::wstring& jobPath);

    // Update subscription status
    void UpdateSyncStatus(const std::wstring& jobPath, SyncStatus status, uint64_t timestamp);
    void UpdateShotCount(const std::wstring& jobPath, int count);

    // Check if a path is within a subscribed job
    std::optional<std::wstring> GetJobPathForPath(const std::wstring& path);

    // Shot metadata operations
    bool CreateOrUpdateShotMetadata(const ShotMetadata& metadata);
    std::optional<ShotMetadata> GetShotMetadata(const std::wstring& shotPath);
    std::vector<ShotMetadata> GetAllShotMetadata(const std::wstring& jobPath);
    std::vector<ShotMetadata> GetShotMetadataByType(const std::wstring& jobPath, const std::string& folderType);
    bool DeleteShotMetadata(const std::wstring& shotPath);

    // Get tracked items (for Project Tracker view)
    std::vector<ShotMetadata> GetTrackedItems(const std::wstring& jobPath, const std::string& itemType);
    std::vector<ShotMetadata> GetAllTrackedItems(const std::wstring& jobPath);

    // Manual task operations
    bool CreateManualTask(const std::wstring& jobPath, const std::string& taskName, const ShotMetadata& metadata);
    bool DeleteManualTask(int taskId);

    // Metadata bridging (for MetadataManager to call)
    void BridgeFromSyncCache(const struct Shot& shot, const std::wstring& jobPath);

    // Get database handle for other managers (BookmarkManager, etc.)
    sqlite3* GetDatabase() const { return m_db; }

    // Get database mutex for coordinated access (MetadataManager needs this)
    std::recursive_mutex& GetDatabaseMutex() const { return m_dbMutex; }

    // Register callback for when local changes are made (for immediate P2P notifications)
    void RegisterLocalChangeCallback(std::function<void(const std::wstring& jobPath, uint64_t timestamp)> callback)
    {
        m_localChangeCallback = callback;
    }

    // Metadata bridging - convert ShotMetadata to change log entry (for initial discovery)
    void BridgeToSyncCache(const ShotMetadata& metadata, const std::wstring& jobPath);

private:
    sqlite3* m_db = nullptr;
    std::filesystem::path m_dbPath;
    MetadataManager* m_metaManager = nullptr;  // For bridging metadata systems
    std::function<void(const std::wstring& jobPath, uint64_t timestamp)> m_localChangeCallback;  // For immediate P2P notifications

    // Thread safety - protect all database operations
    // Using recursive_mutex to allow re-entrant locking (some functions call other locked functions)
    mutable std::recursive_mutex m_dbMutex;

    // Internal helpers
    bool CreateTables();
    bool ExecuteSQL(const char* sql);
    std::string SyncStatusToString(SyncStatus status);
    SyncStatus StringToSyncStatus(const std::string& str);

    // Path helpers
    std::wstring GetRelativePath(const std::wstring& absolutePath, const std::wstring& jobPath);
    std::wstring GetAbsolutePath(const std::wstring& relativePath, const std::wstring& jobPath);
};

} // namespace UFB
