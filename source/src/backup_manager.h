#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace UFB {

// JSON validation results
enum class ValidationResult
{
    Valid,              // JSON is valid and well-formed
    Corrupt,            // Invalid JSON syntax or structure
    Missing,            // File not found (after retries)
    Empty,              // File exists but 0 bytes
    VersionMismatch     // Wrong schema version
};

// Backup information
struct BackupInfo
{
    uint64_t timestamp = 0;
    std::wstring filename;
    std::string createdBy;      // Device ID
    int shotCount = 0;
    std::string checksum;       // SHA-256 (future)
    size_t uncompressedSize = 0;
    std::wstring date;          // Human-readable date (e.g., "2025-10-30")
};

class BackupManager
{
public:
    BackupManager();
    ~BackupManager();

    // Backup operations
    bool CreateBackup(const std::wstring& jobPath);
    bool ShouldBackupToday(const std::wstring& jobPath);

    // Lock coordination
    bool TryAcquireBackupLock(const std::wstring& jobPath, int timeoutSec);
    void ReleaseBackupLock(const std::wstring& jobPath);

    // Validation
    ValidationResult ValidateJSON(const std::wstring& jsonPath, int maxRetries = 3);

    // Restoration
    std::vector<BackupInfo> ListBackups(const std::wstring& jobPath);
    bool RestoreBackup(const std::wstring& jobPath, const std::wstring& backupFilename);

    // Retention policy
    void EvictOldBackups(const std::wstring& jobPath, int retentionDays);

    // Logging
    void LogRestoration(const std::wstring& jobPath, const std::wstring& backupFile);
    void WriteSyncLog(const std::wstring& jobPath, const std::string& message);

private:
    // Paths
    std::filesystem::path GetBackupDirectory(const std::wstring& jobPath);
    std::filesystem::path GetBackupMetadataPath(const std::wstring& jobPath);
    std::filesystem::path GetLockFilePath(const std::wstring& jobPath);
    std::filesystem::path GetSyncLogPath(const std::wstring& jobPath);

    // Backup metadata
    nlohmann::json ReadBackupMetadata(const std::wstring& jobPath);
    bool WriteBackupMetadata(const std::wstring& jobPath, const nlohmann::json& metadata);
    bool UpdateLastBackupDate(const std::wstring& jobPath, const std::wstring& date);

    // Helpers
    std::wstring GetDateString();
    std::wstring GetTimestampString();
    bool IsStalelock(const std::filesystem::path& lockFile);
    int GetDaysOld(uint64_t timestamp);
    bool IsSunday(uint64_t timestamp);

    // Compression (future - for now just copy)
    bool CompressFile(const std::filesystem::path& source, const std::filesystem::path& dest);
    bool DecompressFile(const std::filesystem::path& source, const std::filesystem::path& dest);
};

} // namespace UFB
