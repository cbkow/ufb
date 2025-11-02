#include "subscription_manager.h"
#include "utils.h"
#include <iostream>

namespace UFB {

SubscriptionManager::SubscriptionManager()
{
}

SubscriptionManager::~SubscriptionManager()
{
    Shutdown();
}

bool SubscriptionManager::Initialize()
{
    // Get database path in %localappdata%/ufb/
    m_dbPath = GetLocalAppDataPath() / L"ufb.db";

    // Open or create database
    int rc = sqlite3_open(m_dbPath.string().c_str(), &m_db);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to open database: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    // Create tables if they don't exist
    if (!CreateTables())
    {
        std::cerr << "Failed to create tables" << std::endl;
        return false;
    }

    return true;
}

void SubscriptionManager::Shutdown()
{
    if (m_db)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool SubscriptionManager::CreateTables()
{
    const char* createSubscriptionsTable = R"(
        CREATE TABLE IF NOT EXISTS subscriptions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            job_path TEXT UNIQUE NOT NULL,
            job_name TEXT NOT NULL,
            is_active INTEGER DEFAULT 1,
            subscribed_time INTEGER NOT NULL,
            last_sync_time INTEGER,
            sync_status TEXT DEFAULT 'pending',
            shot_count INTEGER DEFAULT 0
        );
    )";

    const char* createSettingsTable = R"(
        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT
        );
    )";

    const char* createIndexes = R"(
        CREATE INDEX IF NOT EXISTS idx_subscriptions_active ON subscriptions(is_active);
    )";

    return ExecuteSQL(createSubscriptionsTable) &&
           ExecuteSQL(createSettingsTable) &&
           ExecuteSQL(createIndexes);
}

bool SubscriptionManager::ExecuteSQL(const char* sql)
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK)
    {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool SubscriptionManager::SubscribeToJob(const std::wstring& jobPath, const std::wstring& jobName)
{
    // Convert wide strings to UTF-8
    std::string jobPathUtf8 = WideToUtf8(jobPath);
    std::string jobNameUtf8 = WideToUtf8(jobName);
    uint64_t timestamp = GetCurrentTimeMs();

    const char* sql = R"(
        INSERT INTO subscriptions (job_path, job_name, subscribed_time, is_active)
        VALUES (?, ?, ?, 1)
        ON CONFLICT(job_path) DO UPDATE SET
            is_active = 1,
            job_name = excluded.job_name;
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, jobNameUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, timestamp);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "Failed to insert subscription: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    return true;
}

bool SubscriptionManager::UnsubscribeFromJob(const std::wstring& jobPath)
{
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "DELETE FROM subscriptions WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

bool SubscriptionManager::SetJobActive(const std::wstring& jobPath, bool active)
{
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "UPDATE subscriptions SET is_active = ? WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, active ? 1 : 0);
    sqlite3_bind_text(stmt, 2, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

std::vector<Subscription> SubscriptionManager::GetAllSubscriptions()
{
    std::vector<Subscription> subscriptions;

    const char* sql = "SELECT id, job_path, job_name, is_active, subscribed_time, last_sync_time, sync_status, shot_count FROM subscriptions ORDER BY subscribed_time DESC;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return subscriptions;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        Subscription sub;
        sub.id = sqlite3_column_int(stmt, 0);

        const char* jobPathUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* jobNameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        sub.jobPath = Utf8ToWide(jobPathUtf8 ? jobPathUtf8 : "");
        sub.jobName = Utf8ToWide(jobNameUtf8 ? jobNameUtf8 : "");
        sub.isActive = sqlite3_column_int(stmt, 3) != 0;
        sub.subscribedTime = sqlite3_column_int64(stmt, 4);
        sub.lastSyncTime = sqlite3_column_int64(stmt, 5);

        const char* syncStatusStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        sub.syncStatus = StringToSyncStatus(syncStatusStr ? syncStatusStr : "pending");

        sub.shotCount = sqlite3_column_int(stmt, 7);

        subscriptions.push_back(sub);
    }

    sqlite3_finalize(stmt);

    return subscriptions;
}

std::vector<Subscription> SubscriptionManager::GetActiveSubscriptions()
{
    std::vector<Subscription> subscriptions;

    const char* sql = "SELECT id, job_path, job_name, is_active, subscribed_time, last_sync_time, sync_status, shot_count FROM subscriptions WHERE is_active = 1 ORDER BY subscribed_time DESC;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return subscriptions;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        Subscription sub;
        sub.id = sqlite3_column_int(stmt, 0);

        const char* jobPathUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* jobNameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        sub.jobPath = Utf8ToWide(jobPathUtf8 ? jobPathUtf8 : "");
        sub.jobName = Utf8ToWide(jobNameUtf8 ? jobNameUtf8 : "");
        sub.isActive = sqlite3_column_int(stmt, 3) != 0;
        sub.subscribedTime = sqlite3_column_int64(stmt, 4);
        sub.lastSyncTime = sqlite3_column_int64(stmt, 5);

        const char* syncStatusStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        sub.syncStatus = StringToSyncStatus(syncStatusStr ? syncStatusStr : "pending");

        sub.shotCount = sqlite3_column_int(stmt, 7);

        subscriptions.push_back(sub);
    }

    sqlite3_finalize(stmt);

    return subscriptions;
}

std::optional<Subscription> SubscriptionManager::GetSubscription(const std::wstring& jobPath)
{
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "SELECT id, job_path, job_name, is_active, subscribed_time, last_sync_time, sync_status, shot_count FROM subscriptions WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Subscription sub;
        sub.id = sqlite3_column_int(stmt, 0);

        const char* jobPathUtf8Result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* jobNameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        sub.jobPath = Utf8ToWide(jobPathUtf8Result ? jobPathUtf8Result : "");
        sub.jobName = Utf8ToWide(jobNameUtf8 ? jobNameUtf8 : "");
        sub.isActive = sqlite3_column_int(stmt, 3) != 0;
        sub.subscribedTime = sqlite3_column_int64(stmt, 4);
        sub.lastSyncTime = sqlite3_column_int64(stmt, 5);

        const char* syncStatusStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        sub.syncStatus = StringToSyncStatus(syncStatusStr ? syncStatusStr : "pending");

        sub.shotCount = sqlite3_column_int(stmt, 7);

        sqlite3_finalize(stmt);
        return sub;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

void SubscriptionManager::UpdateSyncStatus(const std::wstring& jobPath, SyncStatus status, uint64_t timestamp)
{
    std::string jobPathUtf8 = WideToUtf8(jobPath);
    std::string statusStr = SyncStatusToString(status);

    const char* sql = "UPDATE subscriptions SET sync_status = ?, last_sync_time = ? WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return;
    }

    sqlite3_bind_text(stmt, 1, statusStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, timestamp);
    sqlite3_bind_text(stmt, 3, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SubscriptionManager::UpdateShotCount(const std::wstring& jobPath, int count)
{
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "UPDATE subscriptions SET shot_count = ? WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return;
    }

    sqlite3_bind_int(stmt, 1, count);
    sqlite3_bind_text(stmt, 2, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<std::wstring> SubscriptionManager::GetJobPathForPath(const std::wstring& path)
{
    // Get all active subscriptions and check if path is within any of them
    auto subscriptions = GetActiveSubscriptions();

    std::filesystem::path fsPath(path);

    for (const auto& sub : subscriptions)
    {
        std::filesystem::path jobPath(sub.jobPath);

        // Check if path starts with jobPath
        auto [pathIt, jobIt] = std::mismatch(fsPath.begin(), fsPath.end(), jobPath.begin(), jobPath.end());

        if (jobIt == jobPath.end())
        {
            // Path starts with jobPath
            return sub.jobPath;
        }
    }

    return std::nullopt;
}

std::string SubscriptionManager::SyncStatusToString(SyncStatus status)
{
    switch (status)
    {
    case SyncStatus::Pending: return "pending";
    case SyncStatus::Syncing: return "syncing";
    case SyncStatus::Synced: return "synced";
    case SyncStatus::Stale: return "stale";
    case SyncStatus::Error: return "error";
    default: return "pending";
    }
}

SyncStatus SubscriptionManager::StringToSyncStatus(const std::string& str)
{
    if (str == "pending") return SyncStatus::Pending;
    if (str == "syncing") return SyncStatus::Syncing;
    if (str == "synced") return SyncStatus::Synced;
    if (str == "stale") return SyncStatus::Stale;
    if (str == "error") return SyncStatus::Error;
    return SyncStatus::Pending;
}

} // namespace UFB
