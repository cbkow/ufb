#include "backup_manager.h"
#include "utils.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <thread>
#include <chrono>

namespace UFB {

BackupManager::BackupManager()
{
}

BackupManager::~BackupManager()
{
}

bool BackupManager::CreateBackup(const std::wstring& jobPath)
{
    // Validate current shots.json
    std::filesystem::path shotsJsonPath = std::filesystem::path(jobPath) / L".ufb" / L"shots.json";

    ValidationResult validation = ValidateJSON(shotsJsonPath.wstring());
    if (validation != ValidationResult::Valid)
    {
        std::cerr << "Cannot backup: shots.json is invalid" << std::endl;
        return false;
    }

    // Ensure backup directory exists
    std::filesystem::path backupDir = GetBackupDirectory(jobPath);
    if (!EnsureDirectoryExists(backupDir))
    {
        std::cerr << "Failed to create backup directory" << std::endl;
        return false;
    }

    // Create backup filename with timestamp
    std::wstring timestamp = GetTimestampString();
    std::wstring backupFilename = L"shots_" + timestamp + L".json"; // .gz extension for future compression
    std::filesystem::path backupPath = backupDir / backupFilename;

    // Read shots.json
    std::ifstream sourceFile(shotsJsonPath, std::ios::binary);
    if (!sourceFile.is_open())
    {
        std::cerr << "Failed to open shots.json for backup" << std::endl;
        return false;
    }

    // Get file size and shot count
    sourceFile.seekg(0, std::ios::end);
    size_t fileSize = sourceFile.tellg();
    sourceFile.seekg(0, std::ios::beg);

    // Parse JSON to get shot count
    nlohmann::json doc;
    try
    {
        sourceFile >> doc;
        sourceFile.close();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to parse JSON for backup: " << e.what() << std::endl;
        return false;
    }

    int shotCount = doc.contains("shots") ? doc["shots"].size() : 0;

    // Copy file (TODO: compress in future)
    std::filesystem::copy_file(shotsJsonPath, backupPath, std::filesystem::copy_options::overwrite_existing);

    // NEW: Backup change logs, archives, and snapshot
    std::filesystem::path changesDir = std::filesystem::path(jobPath) / L".ufb" / L"changes";
    if (std::filesystem::exists(changesDir))
    {
        // Create changes backup subdirectory
        std::wstring changesBackupDirName = L"changes_" + timestamp;
        std::filesystem::path changesBackupDir = backupDir / changesBackupDirName;

        try
        {
            // Copy entire changes directory (includes active logs, archives, and snapshot)
            std::filesystem::copy(changesDir, changesBackupDir,
                                  std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);

            std::cout << "Backed up change logs and archives to: " << WideToUtf8(changesBackupDirName) << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Warning: Failed to backup change logs: " << e.what() << std::endl;
            // Continue with backup even if this fails
        }
    }

    // Update backup metadata
    nlohmann::json metadata = ReadBackupMetadata(jobPath);

    BackupInfo info;
    info.timestamp = GetCurrentTimeMs();
    info.filename = backupFilename;
    info.createdBy = GetDeviceID();
    info.shotCount = shotCount;
    info.uncompressedSize = fileSize;
    info.date = GetDateString();

    nlohmann::json backupEntry;
    backupEntry["timestamp"] = info.timestamp;
    backupEntry["filename"] = WideToUtf8(info.filename);
    backupEntry["created_by"] = info.createdBy;
    backupEntry["shot_count"] = info.shotCount;
    backupEntry["uncompressed_size"] = info.uncompressedSize;

    metadata["backups"].push_back(backupEntry);
    metadata["last_backup_date"] = WideToUtf8(GetDateString());

    if (!WriteBackupMetadata(jobPath, metadata))
    {
        std::cerr << "Failed to update backup metadata" << std::endl;
        return false;
    }

    std::cout << "Backup created: " << WideToUtf8(backupFilename) << " (" << shotCount << " shots)" << std::endl;

    return true;
}

bool BackupManager::ShouldBackupToday(const std::wstring& jobPath)
{
    nlohmann::json metadata = ReadBackupMetadata(jobPath);

    std::string today = WideToUtf8(GetDateString());
    std::string lastBackupDate = metadata.value("last_backup_date", "");

    return (lastBackupDate != today);
}

bool BackupManager::TryAcquireBackupLock(const std::wstring& jobPath, int timeoutSec)
{
    std::filesystem::path lockFile = GetLockFilePath(jobPath);

    // Check if lock exists
    if (std::filesystem::exists(lockFile))
    {
        // Check if stale (> 5 minutes old)
        if (IsStalelock(lockFile))
        {
            // Remove stale lock
            std::filesystem::remove(lockFile);
        }
        else
        {
            // Active lock, cannot acquire
            return false;
        }
    }

    // Create lock file
    std::ofstream file(lockFile);
    if (!file.is_open())
    {
        return false;
    }

    file << GetDeviceID() << ":" << GetCurrentTimeMs();
    file.close();

    return true;
}

void BackupManager::ReleaseBackupLock(const std::wstring& jobPath)
{
    std::filesystem::path lockFile = GetLockFilePath(jobPath);

    if (std::filesystem::exists(lockFile))
    {
        std::filesystem::remove(lockFile);
    }
}

ValidationResult BackupManager::ValidateJSON(const std::wstring& jsonPath, int maxRetries)
{
    for (int attempt = 1; attempt <= maxRetries; ++attempt)
    {
        // 1. File exists?
        if (!std::filesystem::exists(jsonPath))
        {
            if (attempt < maxRetries)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000 * attempt));
                continue;
            }
            return ValidationResult::Missing;
        }

        // 2. File size > 0?
        size_t fileSize = std::filesystem::file_size(jsonPath);
        if (fileSize == 0)
        {
            if (attempt == 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            return ValidationResult::Empty;
        }

        // 3. Valid JSON syntax?
        nlohmann::json doc;
        try
        {
            std::ifstream file(jsonPath);
            if (!file.is_open())
            {
                return ValidationResult::Corrupt;
            }
            file >> doc;
        }
        catch (const std::exception&)
        {
            return ValidationResult::Corrupt;
        }

        // 4. Has required fields?
        if (!doc.contains("version") || !doc.contains("shots"))
        {
            return ValidationResult::VersionMismatch;
        }

        // 5. Sanity check: shot count
        if (doc["shots"].size() > 100000)
        {
            return ValidationResult::Corrupt;
        }

        return ValidationResult::Valid;
    }

    return ValidationResult::Missing;
}

std::vector<BackupInfo> BackupManager::ListBackups(const std::wstring& jobPath)
{
    std::vector<BackupInfo> backups;

    nlohmann::json metadata = ReadBackupMetadata(jobPath);

    if (!metadata.contains("backups") || !metadata["backups"].is_array())
    {
        return backups;
    }

    for (const auto& entry : metadata["backups"])
    {
        BackupInfo info;
        info.timestamp = entry.value("timestamp", 0ULL);
        info.filename = Utf8ToWide(entry.value("filename", ""));
        info.createdBy = entry.value("created_by", "");
        info.shotCount = entry.value("shot_count", 0);
        info.uncompressedSize = entry.value("uncompressed_size", 0ULL);

        // Calculate date from timestamp
        std::time_t time = info.timestamp / 1000;
        std::tm tm;
        localtime_s(&tm, &time);
        std::wstringstream ss;
        ss << std::put_time(&tm, L"%Y-%m-%d");
        info.date = ss.str();

        backups.push_back(info);
    }

    // Sort by timestamp (newest first)
    std::sort(backups.begin(), backups.end(), [](const BackupInfo& a, const BackupInfo& b) {
        return a.timestamp > b.timestamp;
    });

    return backups;
}

bool BackupManager::RestoreBackup(const std::wstring& jobPath, const std::wstring& backupFilename)
{
    std::filesystem::path backupDir = GetBackupDirectory(jobPath);
    std::filesystem::path backupPath = backupDir / backupFilename;
    std::filesystem::path shotsJsonPath = std::filesystem::path(jobPath) / L".ufb" / L"shots.json";

    // Validate backup file
    ValidationResult validation = ValidateJSON(backupPath.wstring());
    if (validation != ValidationResult::Valid)
    {
        std::cerr << "Backup file is corrupted!" << std::endl;
        return false;
    }

    // Create "corrupt" backup of current file (if it exists)
    if (std::filesystem::exists(shotsJsonPath))
    {
        std::wstring corruptBackup = L"corrupt_" + GetTimestampString() + L".json";
        std::filesystem::path corruptPath = backupDir / corruptBackup;
        std::filesystem::copy_file(shotsJsonPath, corruptPath, std::filesystem::copy_options::overwrite_existing);
    }

    // Copy backup to shots.json
    std::filesystem::copy_file(backupPath, shotsJsonPath, std::filesystem::copy_options::overwrite_existing);

    // Log restoration
    LogRestoration(jobPath, backupFilename);

    std::cout << "Backup restored: " << WideToUtf8(backupFilename) << std::endl;

    return true;
}

void BackupManager::EvictOldBackups(const std::wstring& jobPath, int retentionDays)
{
    auto backups = ListBackups(jobPath);
    std::vector<std::wstring> keepFilenames;

    uint64_t now = GetCurrentTimeMs();

    for (const auto& backup : backups)
    {
        int daysOld = GetDaysOld(backup.timestamp);

        if (daysOld <= 7)
        {
            // Keep all backups from last 7 days
            keepFilenames.push_back(backup.filename);
        }
        else if (daysOld <= 30)
        {
            // Keep one backup per week (Sunday)
            if (IsSunday(backup.timestamp))
            {
                keepFilenames.push_back(backup.filename);
            }
        }
        // Older than 30 days: don't keep
    }

    // Delete backups not in keepFilenames
    std::filesystem::path backupDir = GetBackupDirectory(jobPath);

    for (const auto& backup : backups)
    {
        if (std::find(keepFilenames.begin(), keepFilenames.end(), backup.filename) == keepFilenames.end())
        {
            std::filesystem::path backupPath = backupDir / backup.filename;
            if (std::filesystem::exists(backupPath))
            {
                std::filesystem::remove(backupPath);
                std::cout << "Evicted old backup: " << WideToUtf8(backup.filename) << std::endl;
            }
        }
    }

    // Update backup metadata to remove evicted entries
    nlohmann::json metadata = ReadBackupMetadata(jobPath);
    nlohmann::json newBackups = nlohmann::json::array();

    for (const auto& entry : metadata["backups"])
    {
        std::wstring filename = Utf8ToWide(entry.value("filename", ""));
        if (std::find(keepFilenames.begin(), keepFilenames.end(), filename) != keepFilenames.end())
        {
            newBackups.push_back(entry);
        }
    }

    metadata["backups"] = newBackups;
    WriteBackupMetadata(jobPath, metadata);
}

void BackupManager::LogRestoration(const std::wstring& jobPath, const std::wstring& backupFile)
{
    std::stringstream ss;
    ss << "[" << GetDeviceID() << "] BACKUP RESTORED from " << WideToUtf8(backupFile);
    WriteSyncLog(jobPath, ss.str());
}

void BackupManager::WriteSyncLog(const std::wstring& jobPath, const std::string& message)
{
    std::filesystem::path logPath = GetSyncLogPath(jobPath);

    std::ofstream file(logPath, std::ios::app);
    if (!file.is_open())
    {
        std::cerr << "Failed to write sync log" << std::endl;
        return;
    }

    // Get current time string
    std::time_t now = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &now);
    char timeStr[32];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tm);

    file << timeStr << " " << message << std::endl;
    file.close();
}

std::filesystem::path BackupManager::GetBackupDirectory(const std::wstring& jobPath)
{
    return std::filesystem::path(jobPath) / L".ufb" / L"backups";
}

std::filesystem::path BackupManager::GetBackupMetadataPath(const std::wstring& jobPath)
{
    return GetBackupDirectory(jobPath) / L"backup_metadata.json";
}

std::filesystem::path BackupManager::GetLockFilePath(const std::wstring& jobPath)
{
    return std::filesystem::path(jobPath) / L".ufb" / L"backup.lock";
}

std::filesystem::path BackupManager::GetSyncLogPath(const std::wstring& jobPath)
{
    return std::filesystem::path(jobPath) / L".ufb" / L"sync.log";
}

nlohmann::json BackupManager::ReadBackupMetadata(const std::wstring& jobPath)
{
    std::filesystem::path metadataPath = GetBackupMetadataPath(jobPath);

    if (!std::filesystem::exists(metadataPath))
    {
        // Create default metadata
        nlohmann::json metadata;
        metadata["backups"] = nlohmann::json::array();
        metadata["last_backup_date"] = "";
        metadata["retention_days"] = 30;
        return metadata;
    }

    try
    {
        std::ifstream file(metadataPath);
        nlohmann::json metadata;
        file >> metadata;
        return metadata;
    }
    catch (const std::exception&)
    {
        // Return empty metadata on error
        nlohmann::json metadata;
        metadata["backups"] = nlohmann::json::array();
        metadata["last_backup_date"] = "";
        metadata["retention_days"] = 30;
        return metadata;
    }
}

bool BackupManager::WriteBackupMetadata(const std::wstring& jobPath, const nlohmann::json& metadata)
{
    std::filesystem::path metadataPath = GetBackupMetadataPath(jobPath);

    // Ensure backup directory exists
    std::filesystem::path backupDir = GetBackupDirectory(jobPath);
    if (!EnsureDirectoryExists(backupDir))
    {
        return false;
    }

    try
    {
        std::ofstream file(metadataPath);
        if (!file.is_open())
        {
            return false;
        }
        file << metadata.dump(2);
        file.close();
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::wstring BackupManager::GetDateString()
{
    std::time_t now = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &now);
    std::wstringstream ss;
    ss << std::put_time(&tm, L"%Y-%m-%d");
    return ss.str();
}

std::wstring BackupManager::GetTimestampString()
{
    std::time_t now = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &now);
    std::wstringstream ss;
    ss << std::put_time(&tm, L"%Y-%m-%d_%H%M%S");
    return ss.str();
}

bool BackupManager::IsStalelock(const std::filesystem::path& lockFile)
{
    auto lastWrite = std::filesystem::last_write_time(lockFile);
    auto now = std::filesystem::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - lastWrite).count();

    return age > 300; // 5 minutes
}

int BackupManager::GetDaysOld(uint64_t timestamp)
{
    uint64_t now = GetCurrentTimeMs();
    uint64_t ageMs = now - timestamp;
    return static_cast<int>(ageMs / (1000 * 60 * 60 * 24));
}

bool BackupManager::IsSunday(uint64_t timestamp)
{
    std::time_t time = timestamp / 1000;
    std::tm tm;
    localtime_s(&tm, &time);
    return tm.tm_wday == 0; // Sunday = 0
}

bool BackupManager::CompressFile(const std::filesystem::path& source, const std::filesystem::path& dest)
{
    // TODO: Implement gzip compression
    // For now, just copy
    std::filesystem::copy_file(source, dest, std::filesystem::copy_options::overwrite_existing);
    return true;
}

bool BackupManager::DecompressFile(const std::filesystem::path& source, const std::filesystem::path& dest)
{
    // TODO: Implement gzip decompression
    // For now, just copy
    std::filesystem::copy_file(source, dest, std::filesystem::copy_options::overwrite_existing);
    return true;
}

} // namespace UFB
