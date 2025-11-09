#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>

namespace UFB {

/**
 * FileWatcher - Watches files for changes using Windows ReadDirectoryChangesW
 *
 * Usage:
 *   FileWatcher watcher;
 *   watcher.WatchFile(L"C:\\path\\to\\file.json", []() {
 *       std::cout << "File changed!" << std::endl;
 *   });
 *   // ... do work ...
 *   watcher.StopWatching();
 */
class FileWatcher
{
public:
    FileWatcher();
    ~FileWatcher();

    /**
     * Watch a specific file for changes
     * @param filePath Absolute path to file to watch
     * @param callback Function to call when file changes
     * @return true if watching started successfully
     */
    bool WatchFile(const std::wstring& filePath, std::function<void()> callback);

    /**
     * Stop watching a specific file
     * @param filePath Absolute path to file to stop watching
     */
    void StopWatchingFile(const std::wstring& filePath);

    /**
     * Stop watching all files and cleanup
     */
    void StopWatching();

    /**
     * Check if currently watching any files
     */
    bool IsWatching() const { return m_isRunning.load(); }

private:
    struct WatchedDirectory
    {
        std::wstring directoryPath;
        HANDLE hDirectory;
        std::thread watchThread;
        std::atomic<bool> isRunning{false};
        std::map<std::wstring, std::function<void()>> fileCallbacks;  // filename -> callback
        std::mutex callbacksMutex;
    };

    /**
     * Background thread that monitors directory changes
     */
    void WatchThread(WatchedDirectory* watchDir);

    /**
     * Get or create a WatchedDirectory for a given file path
     */
    WatchedDirectory* GetOrCreateWatchedDirectory(const std::wstring& filePath);

    /**
     * Extract filename from full path
     */
    std::wstring GetFilename(const std::wstring& filePath);

    /**
     * Extract directory from full path
     */
    std::wstring GetDirectory(const std::wstring& filePath);

    std::map<std::wstring, std::unique_ptr<WatchedDirectory>> m_watchedDirectories;
    std::mutex m_watchedDirsMutex;
    std::atomic<bool> m_isRunning{false};
};

} // namespace UFB
