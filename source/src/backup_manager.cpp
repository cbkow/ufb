#include "backup_manager.h"
#include "utils.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <thread>
#include <chrono>
#include <set>

namespace UFB {

BackupManager::BackupManager()
{
}

BackupManager::~BackupManager()
{
}

bool BackupManager::CreateBackup(const std::wstring& jobPath)
{
    // Ensure backup directory exists
    std::filesystem::path backupDir = GetBackupDirectory(jobPath);
    if (!EnsureDirectoryExists(backupDir))
    {
        std::cerr << "Failed to create backup directory" << std::endl;
        return false;
    }

    // Create backup filename with timestamp
    std::wstring timestamp = GetTimestampString();

    int shotCount = 0;
    size_t shotsJsonSize = 0;

    // OPTIONAL: Backup legacy shots.json if it exists (for migration compatibility)
    std::filesystem::path shotsJsonPath = std::filesystem::path(jobPath) / L".ufb" / L"shots.json";
    if (std::filesystem::exists(shotsJsonPath))
    {
        ValidationResult validation = ValidateJSON(shotsJsonPath.wstring());
        if (validation == ValidationResult::Valid)
        {
            std::wstring backupFilename = L"shots_" + timestamp + L".json";
            std::filesystem::path backupPath = backupDir / backupFilename;

            // Read shots.json
            std::ifstream sourceFile(shotsJsonPath, std::ios::binary);
            if (sourceFile.is_open())
            {
                // Get file size and shot count
                sourceFile.seekg(0, std::ios::end);
                shotsJsonSize = sourceFile.tellg();
                sourceFile.seekg(0, std::ios::beg);

                // Parse JSON to get shot count
                try
                {
                    nlohmann::json doc;
                    sourceFile >> doc;
                    sourceFile.close();
                    shotCount = doc.contains("shots") ? doc["shots"].size() : 0;

                    // Copy file
                    std::filesystem::copy_file(shotsJsonPath, backupPath, std::filesystem::copy_options::overwrite_existing);
                    std::cout << "Backed up legacy shots.json (" << shotCount << " shots)" << std::endl;
                }
                catch (const std::exception& e)
                {
                    std::cerr << "Warning: Failed to backup legacy shots.json: " << e.what() << std::endl;
                    // Continue with other backups
                }
            }
        }
        else
        {
            std::cout << "Skipping invalid shots.json file" << std::endl;
        }
    }

    // Backup change logs, archives, and snapshot
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

            // Count items from change logs (for new architecture)
            std::set<std::string> allUniquePaths;  // Collect unique paths across ALL change logs
            int changeLogFilesProcessed = 0;
            size_t changeLogSize = 0;

            std::cout << "[Backup] Counting items from change logs in: " << WideToUtf8(changesDir.wstring()) << std::endl;

            for (const auto& entry : std::filesystem::directory_iterator(changesDir))
            {
                if (entry.path().extension() == ".json")
                {
                    try
                    {
                        changeLogSize += entry.file_size();
                        changeLogFilesProcessed++;

                        std::ifstream file(entry.path());
                        if (file.is_open())
                        {
                            nlohmann::json doc;
                            file >> doc;

                            // Change logs are arrays of change entries
                            if (doc.is_array())
                            {
                                // Collect all shotPaths from this file
                                int entriesInThisFile = 0;
                                for (const auto& changeEntry : doc)
                                {
                                    if (changeEntry.contains("shotPath"))
                                    {
                                        allUniquePaths.insert(changeEntry["shotPath"].get<std::string>());
                                        entriesInThisFile++;
                                    }
                                }

                                std::cout << "[Backup]   - " << entry.path().filename() << ": " << entriesInThisFile << " entries" << std::endl;
                            }
                            else
                            {
                                std::cout << "[Backup]   - " << entry.path().filename() << ": Not an array (unexpected format)" << std::endl;
                            }
                        }
                    }
                    catch (const std::exception& e)
                    {
                        // Skip invalid files
                        std::cerr << "[Backup] Warning: Failed to parse change log " << entry.path() << ": " << e.what() << std::endl;
                    }
                }
            }

            int changeLogItemCount = allUniquePaths.size();
            std::cout << "[Backup] Processed " << changeLogFilesProcessed << " change log files, " << changeLogItemCount << " unique items total" << std::endl;
            std::cout << "[Backup] shotCount before update: " << shotCount << std::endl;

            // Use change log count if we didn't get a count from legacy shots.json
            if (shotCount == 0 && changeLogItemCount > 0)
            {
                shotCount = changeLogItemCount;
                shotsJsonSize = changeLogSize;
                std::cout << "[Backup] Updated shotCount to " << shotCount << " from change logs" << std::endl;
            }
            else if (shotCount > 0)
            {
                std::cout << "[Backup] Keeping shotCount=" << shotCount << " from legacy shots.json" << std::endl;
            }
            else
            {
                std::cout << "[Backup] Warning: No items found in change logs or shots.json" << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "Warning: Failed to backup change logs: " << e.what() << std::endl;
            // Continue with backup even if this fails
        }
    }
    else
    {
        std::cout << "Warning: No change logs directory found to backup" << std::endl;
    }

    // NEW: Backup manual task folders (.ufb/tasks/)
    std::filesystem::path tasksDir = std::filesystem::path(jobPath) / L".ufb" / L"tasks";
    if (std::filesystem::exists(tasksDir))
    {
        // Create tasks backup subdirectory
        std::wstring tasksBackupDirName = L"tasks_" + timestamp;
        std::filesystem::path tasksBackupDir = backupDir / tasksBackupDirName;

        try
        {
            // Copy entire tasks directory (includes all UUID-based task folders)
            std::filesystem::copy(tasksDir, tasksBackupDir,
                                  std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);

            std::cout << "Backed up manual task folders to: " << WideToUtf8(tasksBackupDirName) << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Warning: Failed to backup task folders: " << e.what() << std::endl;
            // Continue with backup even if this fails
        }
    }

    // Update backup metadata
    nlohmann::json metadata = ReadBackupMetadata(jobPath);

    BackupInfo info;
    info.timestamp = GetCurrentTimeMs();
    info.filename = L"backup_" + timestamp;  // Generic backup name (includes change logs + tasks)
    info.createdBy = GetDeviceID();
    info.shotCount = shotCount;  // From change logs (or legacy shots.json)
    info.uncompressedSize = shotsJsonSize;  // From change logs (or legacy shots.json)
    info.date = GetDateString();

    std::cout << "[Backup] Writing metadata - shotCount: " << info.shotCount << ", size: " << info.uncompressedSize << " bytes" << std::endl;

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

    std::cout << "[Backup] Backup created successfully with " << info.shotCount << " items at: " << WideToUtf8(timestamp) << std::endl;

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

    // Extract timestamp from backupFilename (format: backup_YYYY-MM-DD_HHMMSS or shots_YYYY-MM-DD_HHMMSS.json)
    std::wstring timestamp;
    size_t underscorePos = backupFilename.find(L'_');
    if (underscorePos != std::wstring::npos)
    {
        timestamp = backupFilename.substr(underscorePos + 1);
        // Remove .json extension if present
        size_t dotPos = timestamp.find(L'.');
        if (dotPos != std::wstring::npos)
        {
            timestamp = timestamp.substr(0, dotPos);
        }
    }
    else
    {
        std::cerr << "Invalid backup filename format" << std::endl;
        return false;
    }

    bool restoredAny = false;

    // OPTIONAL: Restore legacy shots.json if it exists
    std::wstring shotsJsonFilename = L"shots_" + timestamp + L".json";
    std::filesystem::path shotsJsonBackupPath = backupDir / shotsJsonFilename;
    std::filesystem::path shotsJsonPath = std::filesystem::path(jobPath) / L".ufb" / L"shots.json";

    if (std::filesystem::exists(shotsJsonBackupPath))
    {
        // Validate backup file
        ValidationResult validation = ValidateJSON(shotsJsonBackupPath.wstring());
        if (validation == ValidationResult::Valid)
        {
            // Create "corrupt" backup of current file (if it exists)
            if (std::filesystem::exists(shotsJsonPath))
            {
                std::wstring corruptBackup = L"corrupt_" + GetTimestampString() + L".json";
                std::filesystem::path corruptPath = backupDir / corruptBackup;
                try
                {
                    std::filesystem::copy_file(shotsJsonPath, corruptPath, std::filesystem::copy_options::overwrite_existing);
                }
                catch (const std::exception& e)
                {
                    std::cerr << "Warning: Failed to backup current shots.json: " << e.what() << std::endl;
                }
            }

            // Copy backup to shots.json
            try
            {
                std::filesystem::copy_file(shotsJsonBackupPath, shotsJsonPath, std::filesystem::copy_options::overwrite_existing);
                std::cout << "Restored legacy shots.json" << std::endl;
                restoredAny = true;
            }
            catch (const std::exception& e)
            {
                std::cerr << "Warning: Failed to restore shots.json: " << e.what() << std::endl;
            }
        }
        else
        {
            std::cerr << "Warning: Backup shots.json is corrupted, skipping" << std::endl;
        }
    }

    // Restore change logs
    std::wstring changesBackupDirName = L"changes_" + timestamp;
    std::filesystem::path changesBackupDir = backupDir / changesBackupDirName;
    std::filesystem::path changesDir = std::filesystem::path(jobPath) / L".ufb" / L"changes";

    if (std::filesystem::exists(changesBackupDir))
    {
        try
        {
            // Remove existing changes directory
            if (std::filesystem::exists(changesDir))
            {
                std::filesystem::remove_all(changesDir);
            }

            // Copy backup to changes directory
            std::filesystem::copy(changesBackupDir, changesDir,
                                  std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);

            std::cout << "Restored change logs and archives" << std::endl;

            // Update all timestamps to "now" to make restored items the latest version
            // This ensures the restored backup overrides any changes on other devices during sync
            uint64_t now = GetCurrentTimeMs();
            UpdateChangeLogTimestamps(changesDir, now);
            std::cout << "Updated all restored items to current timestamp (force as latest)" << std::endl;

            restoredAny = true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error: Failed to restore change logs: " << e.what() << std::endl;
            return false;
        }
    }
    else
    {
        std::cout << "Warning: No change logs backup found for this timestamp" << std::endl;
    }

    // NEW: Restore task folders
    std::wstring tasksBackupDirName = L"tasks_" + timestamp;
    std::filesystem::path tasksBackupDir = backupDir / tasksBackupDirName;
    std::filesystem::path tasksDir = std::filesystem::path(jobPath) / L".ufb" / L"tasks";

    if (std::filesystem::exists(tasksBackupDir))
    {
        try
        {
            // Remove existing tasks directory
            if (std::filesystem::exists(tasksDir))
            {
                std::filesystem::remove_all(tasksDir);
            }

            // Copy backup to tasks directory
            std::filesystem::copy(tasksBackupDir, tasksDir,
                                  std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);

            std::cout << "Restored manual task folders" << std::endl;
            restoredAny = true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Warning: Failed to restore task folders: " << e.what() << std::endl;
            // Continue - not fatal
        }
    }

    if (!restoredAny)
    {
        std::cerr << "Error: No backup components found to restore" << std::endl;
        return false;
    }

    // Log restoration
    LogRestoration(jobPath, backupFilename);

    std::cout << "Backup restored successfully from: " << WideToUtf8(timestamp) << std::endl;

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

void BackupManager::UpdateChangeLogTimestamps(const std::filesystem::path& changesDir, uint64_t newTimestamp)
{
    try
    {
        // Iterate through all .json files in changes directory
        for (const auto& entry : std::filesystem::directory_iterator(changesDir))
        {
            if (entry.path().extension() != ".json")
                continue;

            // Read the change log JSON
            std::ifstream file(entry.path());
            if (!file.is_open())
            {
                std::cerr << "[BackupManager] Failed to open change log: " << entry.path() << std::endl;
                continue;
            }

            nlohmann::json doc;
            try
            {
                file >> doc;
                file.close();
            }
            catch (const std::exception& e)
            {
                std::cerr << "[BackupManager] Failed to parse change log: " << entry.path() << " - " << e.what() << std::endl;
                continue;
            }

            // Update modifiedTime for all items in the change log
            bool modified = false;
            if (doc.contains("items") && doc["items"].is_array())
            {
                for (auto& item : doc["items"])
                {
                    if (item.is_object())
                    {
                        item["modifiedTime"] = newTimestamp;
                        modified = true;
                    }
                }
            }

            // Write back if we made changes
            if (modified)
            {
                std::ofstream outFile(entry.path());
                if (outFile.is_open())
                {
                    outFile << doc.dump(2);
                    outFile.close();
                    std::cout << "[BackupManager] Updated timestamps in: " << entry.path().filename() << std::endl;
                }
                else
                {
                    std::cerr << "[BackupManager] Failed to write updated change log: " << entry.path() << std::endl;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[BackupManager] Error updating change log timestamps: " << e.what() << std::endl;
    }
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
