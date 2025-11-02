#include "metadata_manager.h"
#include "subscription_manager.h"
#include "utils.h"
#include <fstream>
#include <iostream>

namespace UFB {

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

    // Use the same database as SubscriptionManager
    m_dbPath = GetLocalAppDataPath() / L"ufb.db";

    // Open database (should already exist from SubscriptionManager)
    int rc = sqlite3_open(m_dbPath.string().c_str(), &m_db);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to open database: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    // Create cache table if it doesn't exist
    if (!CreateCacheTable())
    {
        std::cerr << "Failed to create cache table" << std::endl;
        return false;
    }

    return true;
}

void MetadataManager::Shutdown()
{
    // Flush any pending writes
    FlushAllPendingWrites();

    if (m_db)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool MetadataManager::CreateCacheTable()
{
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

bool MetadataManager::AssignShot(const std::wstring& jobPath, const std::wstring& shotPath, const std::string& shotType)
{
    Shot shot;
    shot.shotPath = shotPath;
    shot.shotType = shotType;
    shot.displayName = std::filesystem::path(shotPath).filename().wstring();
    shot.metadata = "{}"; // Empty JSON object
    shot.createdTime = GetCurrentTimeMs();
    shot.modifiedTime = shot.createdTime;
    shot.deviceId = GetDeviceID();

    // Insert into local cache
    if (!InsertOrUpdateCache(jobPath, shot))
    {
        return false;
    }

    // Queue write to shared JSON
    QueueWrite(jobPath, shot);

    return true;
}

bool MetadataManager::UpdateShotMetadata(const std::wstring& jobPath, const std::wstring& shotPath, const std::string& metadataJson)
{
    // Get existing shot from cache
    auto existingShot = GetShot(jobPath, shotPath);
    if (!existingShot.has_value())
    {
        std::cerr << "Shot not found in cache: " << WideToUtf8(shotPath) << std::endl;
        return false;
    }

    // Update metadata
    Shot updatedShot = existingShot.value();
    updatedShot.metadata = metadataJson;
    updatedShot.modifiedTime = GetCurrentTimeMs();
    updatedShot.deviceId = GetDeviceID();

    // Update cache
    if (!InsertOrUpdateCache(jobPath, updatedShot))
    {
        return false;
    }

    // Queue write to shared JSON
    QueueWrite(jobPath, updatedShot);

    return true;
}

std::optional<Shot> MetadataManager::GetShot(const std::wstring& jobPath, const std::wstring& shotPath)
{
    std::string jobPathUtf8 = WideToUtf8(jobPath);
    std::string shotPathUtf8 = WideToUtf8(shotPath);

    const char* sql = "SELECT shot_path, shot_type, display_name, metadata, created_time, modified_time, device_id FROM shot_cache WHERE job_path = ? AND shot_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
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
    std::vector<Shot> shots;
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "SELECT shot_path, shot_type, display_name, metadata, created_time, modified_time, device_id FROM shot_cache WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
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

void MetadataManager::UpdateCache(const std::wstring& jobPath, const std::vector<Shot>& shots)
{
    // Clear existing cache for this job
    ClearCache(jobPath);

    // Insert all shots
    for (const auto& shot : shots)
    {
        InsertOrUpdateCache(jobPath, shot);
    }
}

void MetadataManager::ClearCache(const std::wstring& jobPath)
{
    std::string jobPathUtf8 = WideToUtf8(jobPath);

    const char* sql = "DELETE FROM shot_cache WHERE job_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
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
        file.close();

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
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
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

    return (rc == SQLITE_DONE);
}

bool MetadataManager::DeleteFromCache(const std::wstring& jobPath, const std::wstring& shotPath)
{
    std::string jobPathUtf8 = WideToUtf8(jobPath);
    std::string shotPathUtf8 = WideToUtf8(shotPath);

    const char* sql = "DELETE FROM shot_cache WHERE job_path = ? AND shot_path = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
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

} // namespace UFB
