#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
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

class SubscriptionManager
{
public:
    SubscriptionManager();
    ~SubscriptionManager();

    // Initialize the subscription database
    bool Initialize();

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

    // Get database handle for other managers (BookmarkManager, etc.)
    sqlite3* GetDatabase() const { return m_db; }

private:
    sqlite3* m_db = nullptr;
    std::filesystem::path m_dbPath;

    // Internal helpers
    bool CreateTables();
    bool ExecuteSQL(const char* sql);
    std::string SyncStatusToString(SyncStatus status);
    SyncStatus StringToSyncStatus(const std::string& str);
};

} // namespace UFB
