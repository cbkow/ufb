#include "subscription_manager.h"
#include "metadata_manager.h"
#include "utils.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <windows.h>
#include <nlohmann/json.hpp>

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

    // Configure SQLite for better concurrency
    // Enable WAL mode for concurrent reads/writes
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    // Set busy timeout to 5 seconds (handles lock contention)
    sqlite3_busy_timeout(m_db, 5000);

    // Normal synchronous mode (safe with WAL)
    sqlite3_exec(m_db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    std::cout << "[SubscriptionManager] Configured SQLite: WAL mode, 5s busy timeout" << std::endl;

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

    const char* createShotMetadataTable = R"(
        CREATE TABLE IF NOT EXISTS shot_metadata (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            shot_path TEXT UNIQUE NOT NULL,
            item_type TEXT DEFAULT 'shot',
            folder_type TEXT NOT NULL,
            status TEXT,
            category TEXT,
            priority INTEGER DEFAULT 2,
            due_date INTEGER,
            artist TEXT,
            note TEXT,
            links TEXT,
            is_tracked INTEGER DEFAULT 1,
            created_time INTEGER,
            modified_time INTEGER
        );
    )";

    const char* createIndexes = R"(
        CREATE INDEX IF NOT EXISTS idx_subscriptions_active ON subscriptions(is_active);
        CREATE INDEX IF NOT EXISTS idx_shot_metadata_path ON shot_metadata(shot_path);
        CREATE INDEX IF NOT EXISTS idx_shot_metadata_type ON shot_metadata(folder_type);
        CREATE INDEX IF NOT EXISTS idx_shot_metadata_item_type ON shot_metadata(item_type);
        CREATE INDEX IF NOT EXISTS idx_shot_metadata_tracked ON shot_metadata(is_tracked);
    )";

    // Add item_type column if it doesn't exist (migration for existing databases)
    const char* addItemTypeColumn = "ALTER TABLE shot_metadata ADD COLUMN item_type TEXT DEFAULT 'shot';";

    bool tablesCreated = ExecuteSQL(createSubscriptionsTable) &&
                         ExecuteSQL(createSettingsTable) &&
                         ExecuteSQL(createShotMetadataTable) &&
                         ExecuteSQL(createIndexes);

    // Try to add item_type column for existing databases (will fail silently if column exists)
    sqlite3_exec(m_db, addItemTypeColumn, nullptr, nullptr, nullptr);

    return tablesCreated;
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
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

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

    // Copy global template to project .ufb folder if it doesn't exist
    try
    {
        std::filesystem::path ufbDir = std::filesystem::path(jobPath) / L".ufb";
        std::filesystem::path projectConfigPath = ufbDir / L"projectConfig.json";

        if (!std::filesystem::exists(projectConfigPath))
        {
            // Create .ufb directory
            std::filesystem::create_directories(ufbDir);

            // Get global template path
            std::filesystem::path globalTemplate = GetLocalAppDataPath() / L"projectTemplate.json";

            // Copy template to project folder
            if (std::filesystem::exists(globalTemplate))
            {
                std::filesystem::copy_file(globalTemplate, projectConfigPath);
                std::wcout << L"[SubscriptionManager] Copied project template to: " << projectConfigPath << std::endl;
            }
            else
            {
                std::wcerr << L"[SubscriptionManager] Warning: Global template not found at: " << globalTemplate << std::endl;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SubscriptionManager] Error copying project template: " << e.what() << std::endl;
        // Don't fail the subscription if template copy fails
    }

    return true;
}

bool SubscriptionManager::UnsubscribeFromJob(const std::wstring& jobPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

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

bool SubscriptionManager::CreateOrUpdateShotMetadata(const ShotMetadata& metadata)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    const char* sql = R"(
        INSERT INTO shot_metadata (shot_path, item_type, folder_type, status, category, priority, due_date, artist, note, links, is_tracked, created_time, modified_time)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(shot_path) DO UPDATE SET
            item_type = excluded.item_type,
            folder_type = excluded.folder_type,
            status = excluded.status,
            category = excluded.category,
            priority = excluded.priority,
            due_date = excluded.due_date,
            artist = excluded.artist,
            note = excluded.note,
            links = excluded.links,
            is_tracked = excluded.is_tracked,
            modified_time = excluded.modified_time;
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare shot metadata insert: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    std::string shotPathUtf8 = WideToUtf8(metadata.shotPath);
    std::string itemType = metadata.itemType.empty() ? "shot" : metadata.itemType;
    sqlite3_bind_text(stmt, 1, shotPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, itemType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, metadata.folderType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, metadata.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, metadata.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, metadata.priority);
    sqlite3_bind_int64(stmt, 7, metadata.dueDate);
    sqlite3_bind_text(stmt, 8, metadata.artist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, metadata.note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, metadata.links.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 11, metadata.isTracked ? 1 : 0);
    sqlite3_bind_int64(stmt, 12, metadata.createdTime);
    sqlite3_bind_int64(stmt, 13, metadata.modifiedTime);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "Failed to insert/update shot metadata: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    // Bridge to sync cache: Find which job this shot belongs to and sync
    auto jobPath = GetJobPathForPath(metadata.shotPath);
    if (jobPath.has_value())
    {
        BridgeToSyncCache(metadata, jobPath.value());
    }
    else
    {
        std::wcerr << L"[SubscriptionManager] Warning: Could not find job path for shot: "
                   << metadata.shotPath << L" - not bridging to sync cache" << std::endl;
    }

    return true;
}

std::optional<ShotMetadata> SubscriptionManager::GetShotMetadata(const std::wstring& shotPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    std::string shotPathUtf8 = WideToUtf8(shotPath);

    const char* sql = "SELECT id, shot_path, item_type, folder_type, status, category, priority, due_date, artist, note, links, is_tracked, created_time, modified_time FROM shot_metadata WHERE shot_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare get shot metadata: " << sqlite3_errmsg(m_db) << std::endl;
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, shotPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW)
    {
        ShotMetadata metadata;
        metadata.id = sqlite3_column_int(stmt, 0);
        metadata.shotPath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));

        const char* itemType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        // Always infer from path to fix any corrupted data
        metadata.itemType = InferItemTypeFromPath(metadata.shotPath);

        metadata.folderType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        metadata.status = status ? status : "";

        const char* category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        metadata.category = category ? category : "";

        metadata.priority = sqlite3_column_int(stmt, 6);
        metadata.dueDate = sqlite3_column_int64(stmt, 7);

        const char* artist = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        metadata.artist = artist ? artist : "";

        const char* note = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        metadata.note = note ? note : "";

        const char* links = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        metadata.links = links ? links : "";

        metadata.isTracked = sqlite3_column_int(stmt, 11) != 0;
        metadata.createdTime = sqlite3_column_int64(stmt, 12);
        metadata.modifiedTime = sqlite3_column_int64(stmt, 13);

        sqlite3_finalize(stmt);
        return metadata;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::vector<ShotMetadata> SubscriptionManager::GetAllShotMetadata(const std::wstring& jobPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    std::vector<ShotMetadata> results;
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    // Get all shots where shot_path starts with job_path
    const char* sql = "SELECT id, shot_path, item_type, folder_type, status, category, priority, due_date, artist, note, links, is_tracked, created_time, modified_time FROM shot_metadata WHERE shot_path LIKE ? || '%';";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare get all shot metadata: " << sqlite3_errmsg(m_db) << std::endl;
        return results;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ShotMetadata metadata;
        metadata.id = sqlite3_column_int(stmt, 0);
        metadata.shotPath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));

        const char* itemType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        // Always infer from path to fix any corrupted data
        metadata.itemType = InferItemTypeFromPath(metadata.shotPath);

        metadata.folderType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        metadata.status = status ? status : "";

        const char* category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        metadata.category = category ? category : "";

        metadata.priority = sqlite3_column_int(stmt, 6);
        metadata.dueDate = sqlite3_column_int64(stmt, 7);

        const char* artist = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        metadata.artist = artist ? artist : "";

        const char* note = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        metadata.note = note ? note : "";

        const char* links = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        metadata.links = links ? links : "";

        metadata.isTracked = sqlite3_column_int(stmt, 11) != 0;
        metadata.createdTime = sqlite3_column_int64(stmt, 12);
        metadata.modifiedTime = sqlite3_column_int64(stmt, 13);

        results.push_back(metadata);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<ShotMetadata> SubscriptionManager::GetShotMetadataByType(const std::wstring& jobPath, const std::string& folderType)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    std::vector<ShotMetadata> results;
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "SELECT id, shot_path, item_type, folder_type, status, category, priority, due_date, artist, note, links, is_tracked, created_time, modified_time FROM shot_metadata WHERE shot_path LIKE ? || '%' AND folder_type = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare get shot metadata by type: " << sqlite3_errmsg(m_db) << std::endl;
        return results;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, folderType.c_str(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ShotMetadata metadata;
        metadata.id = sqlite3_column_int(stmt, 0);
        metadata.shotPath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));

        const char* itemType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        // Always infer from path to fix any corrupted data
        metadata.itemType = InferItemTypeFromPath(metadata.shotPath);

        metadata.folderType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        metadata.status = status ? status : "";

        const char* category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        metadata.category = category ? category : "";

        metadata.priority = sqlite3_column_int(stmt, 6);
        metadata.dueDate = sqlite3_column_int64(stmt, 7);

        const char* artist = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        metadata.artist = artist ? artist : "";

        const char* note = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        metadata.note = note ? note : "";

        const char* links = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        metadata.links = links ? links : "";

        metadata.isTracked = sqlite3_column_int(stmt, 11) != 0;
        metadata.createdTime = sqlite3_column_int64(stmt, 12);
        metadata.modifiedTime = sqlite3_column_int64(stmt, 13);

        results.push_back(metadata);
    }

    sqlite3_finalize(stmt);
    return results;
}

bool SubscriptionManager::DeleteShotMetadata(const std::wstring& shotPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    std::string shotPathUtf8 = WideToUtf8(shotPath);

    const char* sql = "DELETE FROM shot_metadata WHERE shot_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare delete shot metadata: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, shotPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "Failed to delete shot metadata: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    return true;
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

std::string SubscriptionManager::InferItemTypeFromPath(const std::wstring& shotPath)
{
    // Check for manual task marker
    if (shotPath.find(L"/__task_") != std::wstring::npos)
    {
        return "manual_task";
    }

    // Normalize path for comparison (convert to lowercase and use consistent separators)
    std::wstring lowerPath = shotPath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);

    // Check for assets folder (case-insensitive)
    if (lowerPath.find(L"/assets/") != std::wstring::npos ||
        lowerPath.find(L"\\assets\\") != std::wstring::npos ||
        lowerPath.ends_with(L"/assets") ||
        lowerPath.ends_with(L"\\assets"))
    {
        std::wcout << L"[InferItemType] Detected ASSET from path: " << shotPath << std::endl;
        return "asset";
    }

    // Check for postings folder (case-insensitive)
    if (lowerPath.find(L"/postings/") != std::wstring::npos ||
        lowerPath.find(L"\\postings\\") != std::wstring::npos ||
        lowerPath.ends_with(L"/postings") ||
        lowerPath.ends_with(L"\\postings"))
    {
        std::wcout << L"[InferItemType] Detected POSTING from path: " << shotPath << std::endl;
        return "posting";
    }

    // Default to shot (includes ae/, 3d/, comp/, etc.)
    std::wcout << L"[InferItemType] Detected SHOT from path: " << shotPath << std::endl;
    return "shot";
}

std::vector<ShotMetadata> SubscriptionManager::GetTrackedItems(const std::wstring& jobPath, const std::string& itemType)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    std::vector<ShotMetadata> results;
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "SELECT id, shot_path, item_type, folder_type, status, category, priority, due_date, artist, note, links, is_tracked, created_time, modified_time FROM shot_metadata WHERE shot_path LIKE ? || '%' AND item_type = ? AND is_tracked = 1;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare get tracked items: " << sqlite3_errmsg(m_db) << std::endl;
        return results;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, itemType.c_str(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ShotMetadata metadata;
        metadata.id = sqlite3_column_int(stmt, 0);
        metadata.shotPath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));

        const char* itemTypeStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        // Always infer from path to fix any corrupted data
        metadata.itemType = InferItemTypeFromPath(metadata.shotPath);

        metadata.folderType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        metadata.status = status ? status : "";

        const char* category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        metadata.category = category ? category : "";

        metadata.priority = sqlite3_column_int(stmt, 6);
        metadata.dueDate = sqlite3_column_int64(stmt, 7);

        const char* artist = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        metadata.artist = artist ? artist : "";

        const char* note = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        metadata.note = note ? note : "";

        const char* links = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        metadata.links = links ? links : "";

        metadata.isTracked = sqlite3_column_int(stmt, 11) != 0;
        metadata.createdTime = sqlite3_column_int64(stmt, 12);
        metadata.modifiedTime = sqlite3_column_int64(stmt, 13);

        results.push_back(metadata);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<ShotMetadata> SubscriptionManager::GetAllTrackedItems(const std::wstring& jobPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    std::vector<ShotMetadata> results;
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "SELECT id, shot_path, item_type, folder_type, status, category, priority, due_date, artist, note, links, is_tracked, created_time, modified_time FROM shot_metadata WHERE shot_path LIKE ? || '%' AND is_tracked = 1;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare get all tracked items: " << sqlite3_errmsg(m_db) << std::endl;
        return results;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ShotMetadata metadata;
        metadata.id = sqlite3_column_int(stmt, 0);
        metadata.shotPath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));

        const char* itemType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        // Always infer from path to fix any corrupted data
        metadata.itemType = InferItemTypeFromPath(metadata.shotPath);

        metadata.folderType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        metadata.status = status ? status : "";

        const char* category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        metadata.category = category ? category : "";

        metadata.priority = sqlite3_column_int(stmt, 6);
        metadata.dueDate = sqlite3_column_int64(stmt, 7);

        const char* artist = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        metadata.artist = artist ? artist : "";

        const char* note = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        metadata.note = note ? note : "";

        const char* links = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        metadata.links = links ? links : "";

        metadata.isTracked = sqlite3_column_int(stmt, 11) != 0;
        metadata.createdTime = sqlite3_column_int64(stmt, 12);
        metadata.modifiedTime = sqlite3_column_int64(stmt, 13);

        results.push_back(metadata);
    }

    sqlite3_finalize(stmt);
    return results;
}

bool SubscriptionManager::CreateManualTask(const std::wstring& jobPath, const std::string& taskName, const ShotMetadata& metadata)
{
    // Create a unique path for the manual task using job path + task name
    uint64_t timestamp = GetCurrentTimeMs();

    // Convert task name to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, taskName.c_str(), -1, nullptr, 0);
    std::wstring taskNameWide(wideLen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, taskName.c_str(), -1, &taskNameWide[0], wideLen);

    // Use task name in path for better display
    std::wstring taskPath = jobPath + L"/__task_" + taskNameWide;

    ShotMetadata taskMetadata = metadata;
    taskMetadata.shotPath = taskPath;
    taskMetadata.itemType = "manual_task";
    taskMetadata.folderType = "ae";  // Use AE folder type for colors and dropdowns
    taskMetadata.createdTime = timestamp;
    taskMetadata.modifiedTime = timestamp;
    taskMetadata.isTracked = true;  // Manual tasks are always tracked

    return CreateOrUpdateShotMetadata(taskMetadata);
}

bool SubscriptionManager::DeleteManualTask(int taskId)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    const char* sql = "DELETE FROM shot_metadata WHERE id = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare delete manual task: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, taskId);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "Failed to delete manual task: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    return true;
}

void SubscriptionManager::SetMetadataManager(MetadataManager* metaManager)
{
    m_metaManager = metaManager;
}

std::wstring SubscriptionManager::GetRelativePath(const std::wstring& absolutePath, const std::wstring& jobPath)
{
    try
    {
        std::filesystem::path absPath(absolutePath);
        std::filesystem::path job(jobPath);
        return std::filesystem::relative(absPath, job).wstring();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SubscriptionManager] Failed to compute relative path: " << e.what() << std::endl;
        // Return absolute path as fallback
        return absolutePath;
    }
}

std::wstring SubscriptionManager::GetAbsolutePath(const std::wstring& relativePath, const std::wstring& jobPath)
{
    try
    {
        std::filesystem::path job(jobPath);
        std::filesystem::path rel(relativePath);
        return (job / rel).wstring();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SubscriptionManager] Failed to compute absolute path: " << e.what() << std::endl;
        // Return relative path as fallback
        return relativePath;
    }
}

void SubscriptionManager::BridgeToSyncCache(const ShotMetadata& metadata, const std::wstring& jobPath)
{
    if (!m_metaManager)
    {
        std::wcerr << L"[SubscriptionManager] MetadataManager not set, cannot bridge to sync cache" << std::endl;
        return;
    }

    // Convert ShotMetadata → Shot
    Shot shot;
    shot.shotPath = GetRelativePath(metadata.shotPath, jobPath);
    shot.shotType = metadata.folderType;

    // Extract display name from path
    std::filesystem::path path(metadata.shotPath);
    shot.displayName = path.filename().wstring();

    // Create JSON metadata blob
    nlohmann::json metaJson;
    metaJson["status"] = metadata.status;
    metaJson["category"] = metadata.category;
    metaJson["priority"] = metadata.priority;
    metaJson["dueDate"] = metadata.dueDate;
    metaJson["artist"] = metadata.artist;
    metaJson["note"] = metadata.note;
    metaJson["links"] = metadata.links;
    metaJson["isTracked"] = metadata.isTracked;
    metaJson["itemType"] = metadata.itemType;
    shot.metadata = metaJson.dump();

    shot.createdTime = metadata.createdTime;
    shot.modifiedTime = metadata.modifiedTime;
    shot.deviceId = GetDeviceID();

    // NEW ARCHITECTURE: Write to per-device change log (append-only, no contention!)
    ChangeLogEntry entry;
    entry.deviceId = shot.deviceId;
    entry.timestamp = GetCurrentTimeMs();
    entry.operation = "update";
    entry.shotPath = shot.shotPath;
    entry.data = shot;

    if (!m_metaManager->AppendToChangeLog(jobPath, entry))
    {
        std::cerr << "[SubscriptionManager] Failed to append to change log" << std::endl;
        return;
    }

    // Brief delay to allow cloud sync services (Dropbox, OneDrive) to detect file change
    // This gives them time to start propagating the file before remote peers try to read it
    // 500ms is small enough to not impact UX but large enough for file system watchers
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Trigger P2P notification with shot's modification time (if callback is registered)
    // IMPORTANT: Send shot.modifiedTime (not entry.timestamp) because that's what
    // the remote peer will find in the Shot objects after reading change logs
    if (m_localChangeCallback)
    {
        m_localChangeCallback(jobPath, shot.modifiedTime);
    }

    // Also update local cache immediately for local UI responsiveness
    // We write directly to shot_cache to avoid infinite loop with BridgeFromSyncCache
    std::string jobPathUtf8 = WideToUtf8(jobPath);
    std::string shotPathUtf8 = WideToUtf8(shot.shotPath);
    std::string displayNameUtf8 = WideToUtf8(shot.displayName);
    uint64_t cachedAt = GetCurrentTimeMs();

    const char* sql = R"(
        INSERT INTO shot_cache (job_path, shot_path, shot_type, display_name, metadata, created_time, modified_time, device_id, cached_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(job_path, shot_path) DO UPDATE SET
            shot_type = excluded.shot_type,
            display_name = excluded.display_name,
            metadata = excluded.metadata,
            created_time = excluded.created_time,
            modified_time = excluded.modified_time,
            device_id = excluded.device_id,
            cached_at = excluded.cached_at;
    )";

    sqlite3* db = m_metaManager->GetDatabase();
    if (!db)
    {
        std::cerr << "[SubscriptionManager] Failed to get database handle" << std::endl;
        return;
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "[SubscriptionManager] Failed to prepare cache update" << std::endl;
        return;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, shotPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, shot.shotType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, displayNameUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, shot.metadata.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, shot.createdTime);
    sqlite3_bind_int64(stmt, 7, shot.modifiedTime);
    sqlite3_bind_text(stmt, 8, shot.deviceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 9, cachedAt);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "[SubscriptionManager] Failed to update local cache" << std::endl;
        return;
    }

    std::wcout << L"[SubscriptionManager] Bridged metadata to change log: " << shot.shotPath << std::endl;
}

void SubscriptionManager::BridgeFromSyncCache(const Shot& shot, const std::wstring& jobPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_dbMutex);

    // Convert Shot → ShotMetadata
    ShotMetadata metadata;
    metadata.shotPath = GetAbsolutePath(shot.shotPath, jobPath);
    metadata.folderType = shot.shotType;
    metadata.createdTime = shot.createdTime;
    metadata.modifiedTime = shot.modifiedTime;

    // Parse JSON metadata blob
    try
    {
        nlohmann::json metaJson = nlohmann::json::parse(shot.metadata);

        metadata.status = metaJson.value("status", "");
        metadata.category = metaJson.value("category", "");
        metadata.priority = metaJson.value("priority", 2);
        metadata.dueDate = metaJson.value("dueDate", 0);
        metadata.artist = metaJson.value("artist", "");
        metadata.note = metaJson.value("note", "");
        metadata.links = metaJson.value("links", "");
        metadata.isTracked = metaJson.value("isTracked", false);
        // Always infer itemType from path (ignore stored value to fix corrupted data)
        metadata.itemType = InferItemTypeFromPath(metadata.shotPath);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SubscriptionManager] Failed to parse metadata JSON: " << e.what() << std::endl;
        // Continue with default values - infer itemType from path
        metadata.itemType = InferItemTypeFromPath(metadata.shotPath);
        metadata.priority = 2;
        metadata.isTracked = false;
    }

    // Write to shot_metadata table (but don't call BridgeToSyncCache again to avoid infinite loop!)
    const char* sql = R"(
        INSERT INTO shot_metadata (shot_path, item_type, folder_type, status, category, priority, due_date, artist, note, links, is_tracked, created_time, modified_time)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(shot_path) DO UPDATE SET
            item_type = excluded.item_type,
            folder_type = excluded.folder_type,
            status = excluded.status,
            category = excluded.category,
            priority = excluded.priority,
            due_date = excluded.due_date,
            artist = excluded.artist,
            note = excluded.note,
            links = excluded.links,
            is_tracked = excluded.is_tracked,
            modified_time = excluded.modified_time;
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "[SubscriptionManager] Failed to prepare bridge from sync cache: " << sqlite3_errmsg(m_db) << std::endl;
        return;
    }

    std::string shotPathUtf8 = WideToUtf8(metadata.shotPath);
    std::string itemType = metadata.itemType.empty() ? "shot" : metadata.itemType;
    sqlite3_bind_text(stmt, 1, shotPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, itemType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, metadata.folderType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, metadata.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, metadata.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, metadata.priority);
    sqlite3_bind_int64(stmt, 7, metadata.dueDate);
    sqlite3_bind_text(stmt, 8, metadata.artist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, metadata.note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, metadata.links.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 11, metadata.isTracked ? 1 : 0);
    sqlite3_bind_int64(stmt, 12, metadata.createdTime);
    sqlite3_bind_int64(stmt, 13, metadata.modifiedTime);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "[SubscriptionManager] Failed to bridge from sync cache: " << sqlite3_errmsg(m_db) << std::endl;
        return;
    }

    std::wcout << L"[SubscriptionManager] Bridged from sync cache to shot_metadata: " << metadata.shotPath << std::endl;
}

} // namespace UFB
