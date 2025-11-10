#include "metadata_manager.h"
#include "subscription_manager.h"
#include "utils.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

namespace UFB {

// Clock skew tolerance for P2P sync (in milliseconds)
// Allows for minor differences in system clocks between devices
// Set to 10 seconds to handle typical clock drift
constexpr uint64_t CLOCK_SKEW_TOLERANCE_MS = 10000;

MetadataManager::MetadataManager()
{
    m_lastFlush = std::chrono::steady_clock::now();
}

MetadataManager::~MetadataManager()
{
    Shutdown();
}

bool MetadataManager::Initialize(SubscriptionManager* subManager)
{
    m_subManager = subManager;

    if (!m_subManager)
    {
        std::cerr << "[MetadataManager] ERROR: SubscriptionManager is null!" << std::endl;
        return false;
    }

    std::cout << "[MetadataManager] Using shared database connection from SubscriptionManager" << std::endl;

    // Create cache table if it doesn't exist
    if (!CreateCacheTable())
    {
        std::cerr << "[MetadataManager] Failed to create cache table" << std::endl;
        return false;
    }

    return true;
}

void MetadataManager::Shutdown()
{
    // Flush any pending writes
    FlushAllPendingWrites();

    // Don't close database - it's owned by SubscriptionManager
    std::cout << "[MetadataManager] Shutdown complete (database owned by SubscriptionManager)" << std::endl;
}

// Get database handle from SubscriptionManager (shared connection)
sqlite3* MetadataManager::GetDatabase() const
{
    if (!m_subManager)
    {
        std::cerr << "[MetadataManager] ERROR: SubscriptionManager is null!" << std::endl;
        return nullptr;
    }
    return m_subManager->GetDatabase();
}

bool MetadataManager::CreateCacheTable()
{
    std::lock_guard<std::recursive_mutex> lock(m_subManager->GetDatabaseMutex());

    const char* createCacheTable = R"(
        CREATE TABLE IF NOT EXISTS shot_cache (
            job_path TEXT NOT NULL,
            shot_path TEXT NOT NULL,
            shot_type TEXT NOT NULL,
            display_name TEXT,
            metadata TEXT NOT NULL,
            created_time INTEGER NOT NULL,
            modified_time INTEGER NOT NULL,
            device_id TEXT NOT NULL,
            cached_at INTEGER NOT NULL,
            PRIMARY KEY (job_path, shot_path)
        );
    )";

    const char* createIndexes = R"(
        CREATE INDEX IF NOT EXISTS idx_cache_job ON shot_cache(job_path);
        CREATE INDEX IF NOT EXISTS idx_cache_modified ON shot_cache(modified_time);
    )";

    return ExecuteSQL(createCacheTable) && ExecuteSQL(createIndexes);
}

bool MetadataManager::ExecuteSQL(const char* sql)
{
    // Mutex already held by caller
    char* errMsg = nullptr;
    int rc = sqlite3_exec(GetDatabase(), sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK)
    {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool MetadataManager::AssignShot(const std::wstring& jobPath, const std::wstring& shotPath, const std::string& shotType)
{
    std::lock_guard<std::recursive_mutex> lock(m_subManager->GetDatabaseMutex());

    // BEGIN TRANSACTION for atomic cross-table update
    char* errMsg = nullptr;
    int rc = sqlite3_exec(GetDatabase(), "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "[MetadataManager] Failed to begin transaction: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    Shot shot;
    shot.shotPath = shotPath;
    shot.shotType = shotType;
    shot.displayName = std::filesystem::path(shotPath).filename().wstring();
    shot.metadata = "{}"; // Empty JSON object
    shot.createdTime = GetCurrentTimeMs();
    shot.modifiedTime = shot.createdTime;
    shot.deviceId = GetDeviceID();

    // Insert into local cache (also updates shot_metadata via BridgeFromSyncCache)
    if (!InsertOrUpdateCache(jobPath, shot))
    {
        sqlite3_exec(GetDatabase(), "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    // COMMIT TRANSACTION
    rc = sqlite3_exec(GetDatabase(), "COMMIT;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "[MetadataManager] Failed to commit transaction: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        sqlite3_exec(GetDatabase(), "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    // Queue write to shared JSON
    QueueWrite(jobPath, shot);

    return true;
}

bool MetadataManager::UpdateShotMetadata(const std::wstring& jobPath, const std::wstring& shotPath, const std::string& metadataJson)
{
    std::lock_guard<std::recursive_mutex> lock(m_subManager->GetDatabaseMutex());

    // Get existing shot from cache
    auto existingShot = GetShot(jobPath, shotPath);
    if (!existingShot.has_value())
    {
        std::cerr << "Shot not found in cache: " << WideToUtf8(shotPath) << std::endl;
        return false;
    }

    // BEGIN TRANSACTION for atomic cross-table update
    char* errMsg = nullptr;
    int rc = sqlite3_exec(GetDatabase(), "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "[MetadataManager] Failed to begin transaction: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    // Update metadata
    Shot updatedShot = existingShot.value();
    updatedShot.metadata = metadataJson;
    updatedShot.modifiedTime = GetCurrentTimeMs();
    updatedShot.deviceId = GetDeviceID();

    // Update cache (also updates shot_metadata via BridgeFromSyncCache)
    if (!InsertOrUpdateCache(jobPath, updatedShot))
    {
        sqlite3_exec(GetDatabase(), "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    // COMMIT TRANSACTION
    rc = sqlite3_exec(GetDatabase(), "COMMIT;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "[MetadataManager] Failed to commit transaction: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        sqlite3_exec(GetDatabase(), "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    // Queue write to shared JSON
    QueueWrite(jobPath, updatedShot);

    return true;
}

std::optional<Shot> MetadataManager::GetShot(const std::wstring& jobPath, const std::wstring& shotPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_subManager->GetDatabaseMutex());

    std::string jobPathUtf8 = WideToUtf8(jobPath);
    std::string shotPathUtf8 = WideToUtf8(shotPath);

    const char* sql = "SELECT shot_path, shot_type, display_name, metadata, created_time, modified_time, device_id FROM shot_cache WHERE job_path = ? AND shot_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(GetDatabase(), sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(GetDatabase()) << std::endl;
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, shotPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Shot shot;
        shot.shotPath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        shot.shotType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        const char* displayName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        shot.displayName = displayName ? Utf8ToWide(displayName) : L"";

        shot.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        shot.createdTime = sqlite3_column_int64(stmt, 4);
        shot.modifiedTime = sqlite3_column_int64(stmt, 5);
        shot.deviceId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));

        sqlite3_finalize(stmt);
        return shot;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::vector<Shot> MetadataManager::GetAllShots(const std::wstring& jobPath)
{
    return GetCachedShots(jobPath);
}

bool MetadataManager::RemoveShot(const std::wstring& jobPath, const std::wstring& shotPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_subManager->GetDatabaseMutex());

    if (!DeleteFromCache(jobPath, shotPath))
    {
        return false;
    }

    // Queue removal (write full state without this shot)
    FlushPendingWrites(jobPath);

    return true;
}

void MetadataManager::QueueWrite(const std::wstring& jobPath, const Shot& shot)
{
    std::lock_guard<std::mutex> lock(m_writeMutex);

    // Check if this shot is already in the queue - update instead of adding
    for (auto& entry : m_writeQueue)
    {
        if (entry.jobPath == jobPath && entry.shotPath == shot.shotPath)
        {
            entry.shot = shot;
            entry.queuedTime = GetCurrentTimeMs();
            return;
        }
    }

    // Add new entry
    WriteQueueEntry entry;
    entry.jobPath = jobPath;
    entry.shotPath = shot.shotPath;
    entry.shot = shot;
    entry.queuedTime = GetCurrentTimeMs();

    m_writeQueue.push_back(entry);

    // Check if we should flush (>= 100 pending writes)
    if (m_writeQueue.size() >= 100)
    {
        FlushAllPendingWrites();
    }
}

void MetadataManager::FlushPendingWrites(const std::wstring& jobPath)
{
    std::lock_guard<std::mutex> lock(m_writeMutex);

    // Gather all shots for this job from cache
    auto cachedShots = GetCachedShots(jobPath);

    // Convert to map
    std::map<std::wstring, Shot> shotMap;
    for (const auto& shot : cachedShots)
    {
        shotMap[shot.shotPath] = shot;
    }

    // Write to shared JSON
    if (!WriteSharedJSON(jobPath, shotMap))
    {
        std::cerr << "Failed to write shared JSON for job: " << WideToUtf8(jobPath) << std::endl;
    }

    // Remove flushed entries from queue
    m_writeQueue.erase(
        std::remove_if(m_writeQueue.begin(), m_writeQueue.end(),
            [&jobPath](const WriteQueueEntry& entry) { return entry.jobPath == jobPath; }),
        m_writeQueue.end()
    );

    m_lastFlush = std::chrono::steady_clock::now();
}

void MetadataManager::FlushAllPendingWrites()
{
    std::lock_guard<std::mutex> lock(m_writeMutex);

    // Get unique job paths from queue
    std::vector<std::wstring> jobPaths;
    for (const auto& entry : m_writeQueue)
    {
        if (std::find(jobPaths.begin(), jobPaths.end(), entry.jobPath) == jobPaths.end())
        {
            jobPaths.push_back(entry.jobPath);
        }
    }

    // Flush each job
    for (const auto& jobPath : jobPaths)
    {
        // Gather all shots for this job from cache
        auto cachedShots = GetCachedShots(jobPath);

        // Convert to map
        std::map<std::wstring, Shot> shotMap;
        for (const auto& shot : cachedShots)
        {
            shotMap[shot.shotPath] = shot;
        }

        // Write to shared JSON
        if (!WriteSharedJSON(jobPath, shotMap))
        {
            std::cerr << "Failed to write shared JSON for job: " << WideToUtf8(jobPath) << std::endl;
        }
    }

    m_writeQueue.clear();
    m_lastFlush = std::chrono::steady_clock::now();
}

std::vector<Shot> MetadataManager::GetCachedShots(const std::wstring& jobPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_subManager->GetDatabaseMutex());

    std::vector<Shot> shots;
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "SELECT shot_path, shot_type, display_name, metadata, created_time, modified_time, device_id FROM shot_cache WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(GetDatabase(), sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(GetDatabase()) << std::endl;
        return shots;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        Shot shot;
        shot.shotPath = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        shot.shotType = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        const char* displayName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        shot.displayName = displayName ? Utf8ToWide(displayName) : L"";

        shot.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        shot.createdTime = sqlite3_column_int64(stmt, 4);
        shot.modifiedTime = sqlite3_column_int64(stmt, 5);
        shot.deviceId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));

        shots.push_back(shot);
    }

    sqlite3_finalize(stmt);
    return shots;
}

void MetadataManager::UpdateCache(const std::wstring& jobPath, const std::vector<Shot>& shots, bool notifyObservers)
{
    std::lock_guard<std::recursive_mutex> lock(m_subManager->GetDatabaseMutex());

    std::wcout << L"[MetadataManager] UpdateCache called for: " << jobPath << L" with " << shots.size() << L" shots (notify=" << notifyObservers << L")" << std::endl;

    // BEGIN TRANSACTION for atomic cross-table updates
    char* errMsg = nullptr;
    int rc = sqlite3_exec(GetDatabase(), "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        std::cerr << "[MetadataManager] Failed to begin transaction: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return;
    }

    try
    {
        // Clear existing cache for this job
        ClearCache(jobPath);

        // Insert all shots (this also calls BridgeFromSyncCache for each shot)
        for (const auto& shot : shots)
        {
            if (!InsertOrUpdateCache(jobPath, shot))
            {
                // Rollback on error
                sqlite3_exec(GetDatabase(), "ROLLBACK;", nullptr, nullptr, nullptr);
                std::cerr << "[MetadataManager] Failed to insert shot, transaction rolled back" << std::endl;
                return;
            }
        }

        // COMMIT TRANSACTION
        rc = sqlite3_exec(GetDatabase(), "COMMIT;", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK)
        {
            std::cerr << "[MetadataManager] Failed to commit transaction: " << errMsg << std::endl;
            sqlite3_free(errMsg);
            sqlite3_exec(GetDatabase(), "ROLLBACK;", nullptr, nullptr, nullptr);
            return;
        }

        std::wcout << L"[MetadataManager] Transaction committed successfully" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MetadataManager] Exception during UpdateCache: " << e.what() << std::endl;
        sqlite3_exec(GetDatabase(), "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }

    // Optionally notify observers that metadata has changed
    if (notifyObservers)
    {
        std::wcout << L"[MetadataManager] Calling NotifyObservers for: " << jobPath << std::endl;
        NotifyObservers(jobPath);
        std::wcout << L"[MetadataManager] NotifyObservers completed" << std::endl;
    }
    else
    {
        std::wcout << L"[MetadataManager] Skipping NotifyObservers (caller will notify manually)" << std::endl;
    }
}

void MetadataManager::ClearCache(const std::wstring& jobPath)
{
    // Mutex already held by caller (UpdateCache or public caller)
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "DELETE FROM shot_cache WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(GetDatabase(), sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(GetDatabase()) << std::endl;
        return;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool MetadataManager::ReadSharedJSON(const std::wstring& jobPath, std::map<std::wstring, Shot>& outShots)
{
    std::filesystem::path jsonPath = GetSharedJSONPath(jobPath);

    if (!std::filesystem::exists(jsonPath))
    {
        // No JSON file yet, return empty
        return true;
    }

    try
    {
        std::ifstream file(jsonPath);
        if (!file.is_open())
        {
            std::cerr << "Failed to open JSON file: " << jsonPath << std::endl;
            return false;
        }

        nlohmann::json doc = nlohmann::json::parse(file);

        if (!doc.contains("shots") || !doc["shots"].is_object())
        {
            std::cerr << "Invalid JSON structure" << std::endl;
            return false;
        }

        for (auto& [key, value] : doc["shots"].items())
        {
            std::wstring shotPath = Utf8ToWide(key);
            Shot shot = JsonToShot(value, shotPath);
            outShots[shotPath] = shot;
        }

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to parse JSON: " << e.what() << std::endl;
        return false;
    }
}

bool MetadataManager::WriteSharedJSON(const std::wstring& jobPath, const std::map<std::wstring, Shot>& shots)
{
    // Ensure .ufb directory exists
    if (!EnsureUFBDirectory(jobPath))
    {
        std::cerr << "Failed to create .ufb directory" << std::endl;
        return false;
    }

    std::filesystem::path jsonPath = GetSharedJSONPath(jobPath);
    std::filesystem::path tempPath = jsonPath;
    tempPath += L".tmp";

    try
    {
        // Build JSON document
        nlohmann::json doc;
        doc["version"] = 1;
        doc["last_updated"] = GetCurrentTimeMs();
        doc["shots"] = nlohmann::json::object();

        for (const auto& [shotPath, shot] : shots)
        {
            std::string shotPathUtf8 = WideToUtf8(shotPath);
            doc["shots"][shotPathUtf8] = ShotToJson(shot);
        }

        // Write to temp file
        std::ofstream file(tempPath);
        if (!file.is_open())
        {
            std::cerr << "Failed to open temp file for writing: " << tempPath << std::endl;
            return false;
        }

        file << doc.dump(2); // Pretty print with 2-space indent

        // Explicitly flush to ensure data is written to OS before closing
        file.flush();
        if (!file.good())
        {
            std::cerr << "Failed to flush shared JSON file" << std::endl;
            file.close();
            return false;
        }
        file.close();

        // Force OS to sync file to disk/network share (Windows-specific)
#ifdef _WIN32
        HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            if (!FlushFileBuffers(hFile))
            {
                std::wcerr << L"[MetadataManager] Warning: FlushFileBuffers failed for shared JSON" << std::endl;
            }
            CloseHandle(hFile);
        }
#endif

        // Atomic rename
        std::filesystem::rename(tempPath, jsonPath);

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to write JSON: " << e.what() << std::endl;
        return false;
    }
}

bool MetadataManager::InsertOrUpdateCache(const std::wstring& jobPath, const Shot& shot)
{
    // Mutex already held by caller (UpdateCache or public caller with lock)
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

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(GetDatabase(), sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(GetDatabase()) << std::endl;
        return false;
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
        return false;
    }

    // Bridge to shot_metadata: sync cache updates to UI metadata table
    if (m_subManager)
    {
        m_subManager->BridgeFromSyncCache(shot, jobPath);
    }

    return true;
}

bool MetadataManager::DeleteFromCache(const std::wstring& jobPath, const std::wstring& shotPath)
{
    // Mutex already held by caller (RemoveShot)
    std::string jobPathUtf8 = WideToUtf8(jobPath);
    std::string shotPathUtf8 = WideToUtf8(shotPath);

    const char* sql = "DELETE FROM shot_cache WHERE job_path = ? AND shot_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(GetDatabase(), sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(GetDatabase()) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, jobPathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, shotPathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

nlohmann::json MetadataManager::ShotToJson(const Shot& shot)
{
    nlohmann::json json;
    json["shot_type"] = shot.shotType;
    json["display_name"] = WideToUtf8(shot.displayName);
    json["metadata"] = nlohmann::json::parse(shot.metadata);
    json["created_time"] = shot.createdTime;
    json["modified_time"] = shot.modifiedTime;
    json["device_id"] = shot.deviceId;
    return json;
}

Shot MetadataManager::JsonToShot(const nlohmann::json& json, const std::wstring& shotPath)
{
    Shot shot;
    shot.shotPath = shotPath;
    shot.shotType = json.value("shot_type", "");
    shot.displayName = Utf8ToWide(json.value("display_name", ""));
    shot.metadata = json.value("metadata", nlohmann::json::object()).dump();
    shot.createdTime = json.value("created_time", 0ULL);
    shot.modifiedTime = json.value("modified_time", 0ULL);
    shot.deviceId = json.value("device_id", "");
    return shot;
}

std::filesystem::path MetadataManager::GetSharedJSONPath(const std::wstring& jobPath)
{
    return std::filesystem::path(jobPath) / L".ufb" / L"shots.json";
}

bool MetadataManager::EnsureUFBDirectory(const std::wstring& jobPath)
{
    std::filesystem::path ufbDir = std::filesystem::path(jobPath) / L".ufb";
    return EnsureDirectoryExists(ufbDir);
}

//=============================================================================
// Change Log Implementation (Per-Device Append-Only)
//=============================================================================

std::filesystem::path MetadataManager::GetChangesDirectory(const std::wstring& jobPath)
{
    return std::filesystem::path(jobPath) / L".ufb" / L"changes";
}

std::filesystem::path MetadataManager::GetChangeLogPath(const std::wstring& jobPath, const std::string& deviceId)
{
    std::filesystem::path changesDir = GetChangesDirectory(jobPath);
    std::wstring filename = L"device-" + Utf8ToWide(deviceId) + L".json";
    return changesDir / filename;
}

nlohmann::json MetadataManager::ChangeLogEntryToJson(const ChangeLogEntry& entry)
{
    nlohmann::json json;
    json["deviceId"] = entry.deviceId;
    json["timestamp"] = entry.timestamp;
    json["operation"] = entry.operation;
    json["shotPath"] = WideToUtf8(entry.shotPath);

    if (entry.operation == "update")
    {
        json["data"] = ShotToJson(entry.data);
    }

    return json;
}

ChangeLogEntry MetadataManager::JsonToChangeLogEntry(const nlohmann::json& json)
{
    ChangeLogEntry entry;
    entry.deviceId = json.value("deviceId", "");
    entry.timestamp = json.value("timestamp", 0ULL);
    entry.operation = json.value("operation", "");
    entry.shotPath = Utf8ToWide(json.value("shotPath", ""));

    if (json.contains("data"))
    {
        entry.data = JsonToShot(json["data"], entry.shotPath);
    }

    return entry;
}

bool MetadataManager::AppendToChangeLog(const std::wstring& jobPath, const ChangeLogEntry& entry)
{
    try
    {
        // Ensure changes directory exists
        std::filesystem::path changesDir = GetChangesDirectory(jobPath);
        if (!std::filesystem::exists(changesDir))
        {
            std::filesystem::create_directories(changesDir);
        }

        // Get change log path for this device
        std::filesystem::path logPath = GetChangeLogPath(jobPath, entry.deviceId);

        // Read existing entries (if file exists)
        std::vector<nlohmann::json> entries;
        if (std::filesystem::exists(logPath))
        {
            std::ifstream inFile(logPath);
            if (inFile.is_open())
            {
                nlohmann::json doc = nlohmann::json::parse(inFile);
                if (doc.is_array())
                {
                    entries = doc.get<std::vector<nlohmann::json>>();
                }
                inFile.close();
            }
        }

        // Append new entry
        entries.push_back(ChangeLogEntryToJson(entry));

        // Write back to file
        std::ofstream outFile(logPath);
        if (!outFile.is_open())
        {
            std::cerr << "[MetadataManager] Failed to open change log for writing: " << logPath << std::endl;
            return false;
        }

        nlohmann::json doc = entries;
        outFile << doc.dump(2);  // Pretty print with 2-space indent

        // Explicitly flush to ensure data is written to OS before closing
        outFile.flush();
        if (!outFile.good())
        {
            std::cerr << "[MetadataManager] Failed to flush change log file" << std::endl;
            outFile.close();
            return false;
        }
        outFile.close();

        // Force OS to sync file to disk/network share (Windows-specific)
        // This ensures cloud sync services (Dropbox, OneDrive) detect the file change immediately
#ifdef _WIN32
        HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            if (!FlushFileBuffers(hFile))
            {
                std::wcerr << L"[MetadataManager] Warning: FlushFileBuffers failed for change log" << std::endl;
            }
            CloseHandle(hFile);
        }
#endif

        std::wcout << L"[MetadataManager] Appended change log entry: " << entry.shotPath << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MetadataManager] Failed to append change log: " << e.what() << std::endl;
        return false;
    }
}

std::map<std::wstring, Shot> MetadataManager::ReadAllChangeLogs(const std::wstring& jobPath,
                                                                 const std::wstring& expectedDeviceId,
                                                                 uint64_t minTimestamp)
{
    std::map<std::wstring, Shot> shots;
    bool hasExpectedChange = !expectedDeviceId.empty() && minTimestamp > 0;

    if (hasExpectedChange)
    {
        std::wcout << L"[MetadataManager] Content verification enabled: expecting change from device "
                   << expectedDeviceId << L" with timestamp >= " << minTimestamp << std::endl;
    }

    try
    {
        std::filesystem::path changesDir = GetChangesDirectory(jobPath);

        if (!std::filesystem::exists(changesDir))
        {
            // No changes directory yet - return empty
            return shots;
        }

        // Collect all change log entries from all devices
        std::vector<ChangeLogEntry> allEntries;

        for (const auto& entry : std::filesystem::directory_iterator(changesDir))
        {
            // Only process files matching pattern: device-*.json
            // Skip bootstrap-snapshot.json and any other non-device files
            if (entry.is_regular_file() && entry.path().extension() == L".json")
            {
                std::wstring filename = entry.path().stem().wstring();
                if (filename.find(L"device-") != 0)
                {
                    // Skip files that don't start with "device-"
                    std::wcout << L"[MetadataManager] Skipping non-device file: " << entry.path().filename() << std::endl;
                    continue;
                }

                std::wcout << L"[MetadataManager] Reading change log: " << entry.path().filename() << std::endl;

                // Retry logic for file sync services (Dropbox, OneDrive, etc.)
                // File might be locked, partially written, or not yet synced from remote
                // Exponential backoff with cap: 200ms, 400ms, 800ms, 1600ms, 3000ms, 3000ms...
                // Total max wait: ~15 seconds (next sync runs in 30 seconds, so don't block too long)
                bool success = false;
                int maxRetries = 9;

                for (int attempt = 0; attempt < maxRetries && !success; ++attempt)
                {
                    if (attempt > 0)
                    {
                        // Exponential backoff with 3000ms cap (2^attempt * 100, capped at 3000)
                        int delayMs = (std::min)(100 * (1 << attempt), 3000);
                        std::wcout << L"[MetadataManager] Retrying after " << delayMs << L"ms (attempt " << (attempt + 1) << L"/" << maxRetries << L")" << std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                    }

                    try
                    {
                        std::ifstream file(entry.path(), std::ios::binary);
                        if (!file.is_open())
                        {
                            if (attempt < maxRetries - 1)
                            {
                                std::wcerr << L"[MetadataManager] File locked, will retry: " << entry.path().filename() << std::endl;
                                continue;
                            }
                            else
                            {
                                std::wcerr << L"[MetadataManager] Failed to open change log after " << maxRetries << L" attempts: " << entry.path().filename() << std::endl;
                                break;
                            }
                        }

                        // Read entire file
                        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                        file.close();

                        // Check if empty (file sync in progress)
                        if (content.empty())
                        {
                            if (attempt < maxRetries - 1)
                            {
                                std::wcout << L"[MetadataManager] File empty (sync in progress), will retry: " << entry.path().filename() << std::endl;
                                continue;
                            }
                            else
                            {
                                std::wcerr << L"[MetadataManager] File still empty after " << maxRetries << L" attempts: " << entry.path().filename() << std::endl;
                                break;
                            }
                        }

                        // Parse JSON
                        nlohmann::json doc = nlohmann::json::parse(content);
                        std::vector<ChangeLogEntry> parsedEntries;
                        if (doc.is_array())
                        {
                            for (const auto& jsonEntry : doc)
                            {
                                parsedEntries.push_back(JsonToChangeLogEntry(jsonEntry));
                            }
                        }

                        // Content verification: if we're expecting a specific change, verify it's present
                        if (hasExpectedChange)
                        {
                            // Extract device ID from filename (device-{id}.json)
                            std::wstring filename = entry.path().stem().wstring();
                            std::wstring prefix = L"device-";
                            std::wstring fileDeviceId;

                            if (filename.find(prefix) == 0)
                            {
                                fileDeviceId = filename.substr(prefix.length());
                                // Convert underscores back to hyphens (device-0C81F55D_4DB3... -> 0C81F55D-4DB3...)
                                std::replace(fileDeviceId.begin(), fileDeviceId.end(), L'_', L'-');
                            }

                            // Check if this is the file from the expected device
                            if (fileDeviceId == expectedDeviceId)
                            {
                                std::wcout << L"[MetadataManager] Checking expected device file: " << entry.path().filename()
                                           << L" (extracted ID: " << fileDeviceId << L")" << std::endl;

                                // Apply clock skew tolerance to handle devices with slightly different system times
                                uint64_t adjustedMinTimestamp = (minTimestamp > CLOCK_SKEW_TOLERANCE_MS)
                                    ? (minTimestamp - CLOCK_SKEW_TOLERANCE_MS)
                                    : 0;

                                std::wcout << L"[MetadataManager] Looking for timestamp >= " << minTimestamp
                                           << L" (with " << CLOCK_SKEW_TOLERANCE_MS << L"ms tolerance: >= " << adjustedMinTimestamp << L")"
                                           << L", found " << parsedEntries.size() << L" entries" << std::endl;

                                // Verify the parsed entries contain the expected change
                                bool foundExpectedChange = false;
                                for (const auto& parsedEntry : parsedEntries)
                                {
                                    std::wstring deviceIdWide(parsedEntry.deviceId.begin(), parsedEntry.deviceId.end());
                                    std::wcout << L"[MetadataManager]   Entry: deviceId=" << deviceIdWide
                                               << L" timestamp=" << parsedEntry.timestamp
                                               << L" (" << (parsedEntry.timestamp >= adjustedMinTimestamp ? L"MATCH" : L"too old") << L")"
                                               << std::endl;

                                    if (parsedEntry.timestamp >= adjustedMinTimestamp)
                                    {
                                        foundExpectedChange = true;
                                        std::wcout << L"[MetadataManager] ✓ Verified: Found expected change with timestamp "
                                                   << parsedEntry.timestamp << L" (>= " << adjustedMinTimestamp << L" with tolerance)" << std::endl;
                                        break;
                                    }
                                }

                                if (!foundExpectedChange && attempt < maxRetries - 1)
                                {
                                    std::wcout << L"[MetadataManager] Content verification failed: expected change not found (stale read), will retry"
                                               << std::endl;
                                    continue;  // Retry - file exists but content is stale
                                }
                                else if (!foundExpectedChange)
                                {
                                    std::wcerr << L"[MetadataManager] WARNING: Expected change still not found after "
                                               << maxRetries << L" attempts (cloud sync may be slower than expected)" << std::endl;
                                    // Don't fail - proceed with what we have
                                }
                            }
                        }

                        // Add parsed entries to the collection
                        allEntries.insert(allEntries.end(), parsedEntries.begin(), parsedEntries.end());

                        success = true;
                        if (attempt > 0)
                        {
                            std::wcout << L"[MetadataManager] Successfully read change log after " << (attempt + 1) << L" attempts" << std::endl;
                        }
                    }
                    catch (const nlohmann::json::parse_error& e)
                    {
                        if (attempt < maxRetries - 1)
                        {
                            std::wcerr << L"[MetadataManager] JSON parse error (partial sync?), will retry: " << entry.path().filename() << L" - " << e.what() << std::endl;
                            continue;
                        }
                        else
                        {
                            std::wcerr << L"[MetadataManager] Failed to parse change log after " << maxRetries << L" attempts: " << entry.path().filename() << L" - " << e.what() << std::endl;
                            break;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        std::wcerr << L"[MetadataManager] Unexpected error reading change log: " << entry.path().filename() << L" - " << e.what() << std::endl;
                        break;
                    }
                }
            }
        }

        // Sort all entries by timestamp (chronological order)
        std::sort(allEntries.begin(), allEntries.end(),
            [](const ChangeLogEntry& a, const ChangeLogEntry& b) {
                if (a.timestamp != b.timestamp)
                    return a.timestamp < b.timestamp;
                // Tie-breaker: device ID lexicographic order
                return a.deviceId < b.deviceId;
            });

        // Apply changes in chronological order (last write wins)
        for (const auto& entry : allEntries)
        {
            if (entry.operation == "update")
            {
                shots[entry.shotPath] = entry.data;
            }
            else if (entry.operation == "delete")
            {
                shots.erase(entry.shotPath);
            }
        }

        std::wcout << L"[MetadataManager] Merged " << allEntries.size()
                   << L" change log entries into " << shots.size() << L" shots" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MetadataManager] Failed to read change logs: " << e.what() << std::endl;
    }

    return shots;
}

//=============================================================================
// Observer Pattern Implementation
//=============================================================================

void MetadataManager::RegisterObserver(MetadataObserver observer)
{
    std::lock_guard<std::mutex> lock(m_observersMutex);
    m_observers.push_back(observer);
}

void MetadataManager::UnregisterAllObservers()
{
    std::lock_guard<std::mutex> lock(m_observersMutex);
    m_observers.clear();
}

void MetadataManager::NotifyObservers(const std::wstring& jobPath)
{
    std::lock_guard<std::mutex> lock(m_observersMutex);

    std::wcout << L"[MetadataManager] NotifyObservers: Notifying " << m_observers.size() << L" observers for: " << jobPath << std::endl;

    int observerIndex = 0;
    for (const auto& observer : m_observers)
    {
        try
        {
            std::wcout << L"[MetadataManager] Calling observer #" << observerIndex << std::endl;
            observer(jobPath);
            std::wcout << L"[MetadataManager] Observer #" << observerIndex << L" completed" << std::endl;
            observerIndex++;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[MetadataManager] Observer exception: " << e.what() << std::endl;
        }
    }

    std::wcout << L"[MetadataManager] All observers notified successfully" << std::endl;
}

} // namespace UFB
