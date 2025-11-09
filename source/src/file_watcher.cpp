#include "file_watcher.h"
#include <iostream>
#include <filesystem>

namespace UFB {

FileWatcher::FileWatcher()
{
    m_isRunning = false;
}

FileWatcher::~FileWatcher()
{
    StopWatching();
}

bool FileWatcher::WatchFile(const std::wstring& filePath, std::function<void()> callback)
{
    if (!std::filesystem::exists(filePath))
    {
        std::wcerr << L"[FileWatcher] File does not exist: " << filePath << std::endl;
        return false;
    }

    m_isRunning = true;

    // Get or create watched directory for this file
    WatchedDirectory* watchDir = GetOrCreateWatchedDirectory(filePath);
    if (!watchDir)
    {
        std::wcerr << L"[FileWatcher] Failed to create watched directory" << std::endl;
        return false;
    }

    // Add callback for this specific file
    std::wstring filename = GetFilename(filePath);
    {
        std::lock_guard<std::mutex> lock(watchDir->callbacksMutex);
        watchDir->fileCallbacks[filename] = callback;
    }

    std::wcout << L"[FileWatcher] Now watching: " << filePath << std::endl;
    return true;
}

void FileWatcher::StopWatchingFile(const std::wstring& filePath)
{
    std::wstring dirPath = GetDirectory(filePath);
    std::wstring filename = GetFilename(filePath);

    std::lock_guard<std::mutex> lock(m_watchedDirsMutex);
    auto it = m_watchedDirectories.find(dirPath);
    if (it != m_watchedDirectories.end())
    {
        std::lock_guard<std::mutex> callbackLock(it->second->callbacksMutex);
        it->second->fileCallbacks.erase(filename);

        // If no more files to watch in this directory, stop watching
        if (it->second->fileCallbacks.empty())
        {
            it->second->isRunning = false;
            if (it->second->hDirectory != INVALID_HANDLE_VALUE)
            {
                CloseHandle(it->second->hDirectory);
            }
            if (it->second->watchThread.joinable())
            {
                it->second->watchThread.join();
            }
            m_watchedDirectories.erase(it);
        }
    }
}

void FileWatcher::StopWatching()
{
    m_isRunning = false;

    std::lock_guard<std::mutex> lock(m_watchedDirsMutex);
    for (auto& [dirPath, watchDir] : m_watchedDirectories)
    {
        watchDir->isRunning = false;

        // Close directory handle to interrupt ReadDirectoryChangesW
        if (watchDir->hDirectory != INVALID_HANDLE_VALUE)
        {
            CloseHandle(watchDir->hDirectory);
        }

        // Wait for thread to finish
        if (watchDir->watchThread.joinable())
        {
            watchDir->watchThread.join();
        }
    }

    m_watchedDirectories.clear();
    std::cout << "[FileWatcher] Stopped watching all files" << std::endl;
}

void FileWatcher::WatchThread(WatchedDirectory* watchDir)
{
    const DWORD bufferSize = 1024 * 4;  // 4KB buffer
    BYTE buffer[bufferSize];

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!overlapped.hEvent)
    {
        std::cerr << "[FileWatcher] Failed to create event for overlapped I/O" << std::endl;
        return;
    }

    std::wcout << L"[FileWatcher] Watch thread started for: " << watchDir->directoryPath << std::endl;

    while (watchDir->isRunning)
    {
        DWORD bytesReturned = 0;

        // Request notification of changes
        BOOL success = ReadDirectoryChangesW(
            watchDir->hDirectory,
            buffer,
            bufferSize,
            FALSE,  // Don't watch subdirectories
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            NULL,
            &overlapped,
            NULL
        );

        if (!success)
        {
            DWORD error = GetLastError();
            if (error != ERROR_INVALID_HANDLE)  // Expected when stopping
            {
                std::cerr << "[FileWatcher] ReadDirectoryChangesW failed: " << error << std::endl;
            }
            break;
        }

        // Wait for the event (with timeout)
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 500);  // 500ms timeout

        if (waitResult == WAIT_OBJECT_0)
        {
            // Get the result
            success = GetOverlappedResult(watchDir->hDirectory, &overlapped, &bytesReturned, FALSE);

            if (success && bytesReturned > 0)
            {
                // Process notifications
                FILE_NOTIFY_INFORMATION* notification = (FILE_NOTIFY_INFORMATION*)buffer;

                while (true)
                {
                    // Extract filename from notification
                    std::wstring filename(notification->FileName, notification->FileNameLength / sizeof(WCHAR));

                    // Check if we're watching this specific file
                    {
                        std::lock_guard<std::mutex> lock(watchDir->callbacksMutex);
                        auto it = watchDir->fileCallbacks.find(filename);
                        if (it != watchDir->fileCallbacks.end())
                        {
                            // File changed - invoke callback
                            try
                            {
                                it->second();
                            }
                            catch (const std::exception& e)
                            {
                                std::cerr << "[FileWatcher] Callback exception: " << e.what() << std::endl;
                            }
                        }
                    }

                    // Move to next notification (if any)
                    if (notification->NextEntryOffset == 0)
                        break;

                    notification = (FILE_NOTIFY_INFORMATION*)((BYTE*)notification + notification->NextEntryOffset);
                }
            }
        }
        else if (waitResult == WAIT_TIMEOUT)
        {
            // Timeout - check if we should continue
            continue;
        }
        else
        {
            // Error or abandoned
            break;
        }
    }

    CloseHandle(overlapped.hEvent);
    std::wcout << L"[FileWatcher] Watch thread stopped for: " << watchDir->directoryPath << std::endl;
}

FileWatcher::WatchedDirectory* FileWatcher::GetOrCreateWatchedDirectory(const std::wstring& filePath)
{
    std::wstring dirPath = GetDirectory(filePath);

    std::lock_guard<std::mutex> lock(m_watchedDirsMutex);

    // Check if already watching this directory
    auto it = m_watchedDirectories.find(dirPath);
    if (it != m_watchedDirectories.end())
    {
        return it->second.get();
    }

    // Create new watched directory
    auto watchDir = std::make_unique<WatchedDirectory>();
    watchDir->directoryPath = dirPath;

    // Open directory handle with FILE_FLAG_BACKUP_SEMANTICS and FILE_FLAG_OVERLAPPED
    watchDir->hDirectory = CreateFileW(
        dirPath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (watchDir->hDirectory == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        std::wcerr << L"[FileWatcher] Failed to open directory: " << dirPath
                   << L" Error: " << error << std::endl;
        return nullptr;
    }

    // Start watch thread
    watchDir->isRunning = true;
    watchDir->watchThread = std::thread(&FileWatcher::WatchThread, this, watchDir.get());

    WatchedDirectory* result = watchDir.get();
    m_watchedDirectories[dirPath] = std::move(watchDir);

    return result;
}

std::wstring FileWatcher::GetFilename(const std::wstring& filePath)
{
    std::filesystem::path path(filePath);
    return path.filename().wstring();
}

std::wstring FileWatcher::GetDirectory(const std::wstring& filePath)
{
    std::filesystem::path path(filePath);
    return path.parent_path().wstring();
}

} // namespace UFB
