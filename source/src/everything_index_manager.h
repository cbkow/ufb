#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>

namespace UFB {

// Manages integration with Everything search indexing
// Automatically adds/removes folders from Everything's index when jobs are subscribed/unsubscribed
class EverythingIndexManager
{
public:
    EverythingIndexManager();
    ~EverythingIndexManager();

    // Initialize and detect Everything installation
    // Returns true if Everything is available, false otherwise
    bool Initialize();

    // Shutdown and cleanup
    void Shutdown();

    // Add folder to Everything index
    // Returns true on success, false on failure (logs errors to console)
    bool AddFolderToIndex(const std::wstring& folderPath);

    // Remove folder from Everything index
    // Returns true on success, false on failure (logs errors to console)
    bool RemoveFolderFromIndex(const std::wstring& folderPath);

    // Sync all active subscriptions to Everything
    // Ensures all provided paths are in Everything's index
    bool SyncAllSubscriptions(const std::vector<std::wstring>& activePaths);

    // Check if Everything is available
    bool IsAvailable() const { return m_everythingAvailable; }

    // Check if a specific folder is already in Everything's index
    bool IsFolderIndexed(const std::wstring& folderPath);

private:
    bool m_everythingAvailable = false;
    std::filesystem::path m_everythingIniPath;
    std::filesystem::path m_everythingExePath;

    // Everything detection and location
    bool LocateEverything();

    // INI file operations
    bool ReadIniFile(std::vector<std::wstring>& folders);
    bool WriteIniFile(const std::vector<std::wstring>& folders);

    // Everything process control
    bool StopEverything();
    bool StartEverything();
    bool IsEverythingRunning();
    bool RescanEverything();

    // Helper methods
    bool NormalizePath(std::wstring& path);
    bool IsFolderInList(const std::vector<std::wstring>& folders, const std::wstring& path);
};

} // namespace UFB
