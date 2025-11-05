#include "everything_index_manager.h"
#include "utils.h"
#include <iostream>
#include <shlobj.h>

namespace UFB {

EverythingIndexManager::EverythingIndexManager()
{
}

EverythingIndexManager::~EverythingIndexManager()
{
    Shutdown();
}

bool EverythingIndexManager::Initialize()
{
    std::cout << "[EverythingIndexManager] Initializing..." << std::endl;

    // Try to locate Everything
    if (!LocateEverything())
    {
        std::cout << "[EverythingIndexManager] Everything not detected - search indexing disabled" << std::endl;
        m_everythingAvailable = false;
        return false;
    }

    std::cout << "[EverythingIndexManager] Everything detected successfully" << std::endl;
    std::wcout << L"[EverythingIndexManager] INI path: " << m_everythingIniPath << std::endl;
    std::wcout << L"[EverythingIndexManager] EXE path: " << m_everythingExePath << std::endl;

    m_everythingAvailable = true;
    return true;
}

void EverythingIndexManager::Shutdown()
{
    // Nothing to clean up currently
}

bool EverythingIndexManager::LocateEverything()
{
    // Strategy 1: Try to find Everything.exe via registry
    HKEY hKey;
    wchar_t exePath[MAX_PATH] = {};
    DWORD bufferSize = sizeof(exePath);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\voidtools\\Everything", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, L"InstallPath", NULL, NULL, (LPBYTE)exePath, &bufferSize) == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            m_everythingExePath = std::filesystem::path(exePath) / L"Everything.exe";

            // Check for portable mode (INI in same directory as exe)
            std::filesystem::path portableIni = std::filesystem::path(exePath) / L"Everything.ini";
            if (std::filesystem::exists(portableIni))
            {
                m_everythingIniPath = portableIni;
                return std::filesystem::exists(m_everythingExePath);
            }
        }
        else
        {
            RegCloseKey(hKey);
        }
    }

    // Strategy 2: Try AppData location
    wchar_t appDataPath[MAX_PATH] = {};
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath) == S_OK)
    {
        m_everythingIniPath = std::filesystem::path(appDataPath) / L"Everything" / L"Everything.ini";

        // If INI exists in AppData, assume Everything.exe is findable (in PATH or common location)
        if (std::filesystem::exists(m_everythingIniPath))
        {
            // Try to find Everything.exe in common locations
            std::vector<std::filesystem::path> possiblePaths = {
                std::filesystem::path(L"C:\\Program Files\\Everything\\Everything.exe"),
                std::filesystem::path(L"C:\\Program Files (x86)\\Everything\\Everything.exe")
            };

            for (const auto& path : possiblePaths)
            {
                if (std::filesystem::exists(path))
                {
                    m_everythingExePath = path;
                    return true;
                }
            }

            // INI exists but can't find exe - still usable if Everything is running
            return true;
        }
    }

    return false;
}

bool EverythingIndexManager::IsEverythingRunning()
{
    HWND hwnd = FindWindowW(L"EVERYTHING", NULL);
    return (hwnd != NULL);
}

bool EverythingIndexManager::StopEverything()
{
    HWND hwnd = FindWindowW(L"EVERYTHING", NULL);
    if (!hwnd)
    {
        return true; // Already stopped
    }

    std::cout << "[EverythingIndexManager] Stopping Everything..." << std::endl;

    // Send close message for graceful shutdown
    SendMessageW(hwnd, WM_CLOSE, 0, 0);

    // Wait up to 5 seconds for graceful shutdown
    for (int i = 0; i < 50; ++i)
    {
        Sleep(100);
        if (!IsEverythingRunning())
        {
            std::cout << "[EverythingIndexManager] Everything stopped gracefully" << std::endl;
            return true;
        }
    }

    // Force terminate if still running
    std::cerr << "[EverythingIndexManager] Warning: Everything did not close gracefully, force terminating..." << std::endl;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != 0)
    {
        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, processId);
        if (hProcess)
        {
            TerminateProcess(hProcess, 0);
            CloseHandle(hProcess);
            Sleep(500);
        }
    }

    return !IsEverythingRunning();
}

bool EverythingIndexManager::StartEverything()
{
    if (m_everythingExePath.empty() || !std::filesystem::exists(m_everythingExePath))
    {
        std::cerr << "[EverythingIndexManager] Cannot start Everything - exe path not found" << std::endl;
        return false;
    }

    std::wcout << L"[EverythingIndexManager] Starting Everything: " << m_everythingExePath << std::endl;

    // Use ShellExecute to start Everything
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = m_everythingExePath.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei))
    {
        std::cerr << "[EverythingIndexManager] Failed to start Everything" << std::endl;
        return false;
    }

    if (sei.hProcess)
    {
        CloseHandle(sei.hProcess);
    }

    // Wait for Everything to start (up to 3 seconds)
    for (int i = 0; i < 30; ++i)
    {
        Sleep(100);
        if (IsEverythingRunning())
        {
            std::cout << "[EverythingIndexManager] Everything started successfully" << std::endl;
            return true;
        }
    }

    bool running = IsEverythingRunning();
    if (!running)
    {
        std::cerr << "[EverythingIndexManager] Warning: Everything may not have started" << std::endl;
    }

    return running;
}

bool EverythingIndexManager::RescanEverything()
{
    if (m_everythingExePath.empty() || !std::filesystem::exists(m_everythingExePath))
    {
        std::cerr << "[EverythingIndexManager] Cannot reindex - exe path not found" << std::endl;
        return false;
    }

    if (!IsEverythingRunning())
    {
        std::cerr << "[EverythingIndexManager] Everything is not running, cannot reindex" << std::endl;
        return false;
    }

    std::wcout << L"[EverythingIndexManager] Triggering reindex: " << m_everythingExePath << std::endl;

    // Use ShellExecute to send reindex command to running instance
    // -reindex forces a database rebuild to pick up new folders from config
    // -instance sends the command to the running instance instead of launching a new one
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = m_everythingExePath.c_str();
    sei.lpParameters = L"-instance 1.5a -reindex";  // -reindex forces full database rebuild
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei))
    {
        std::cerr << "[EverythingIndexManager] Failed to trigger reindex" << std::endl;
        return false;
    }

    if (sei.hProcess)
    {
        CloseHandle(sei.hProcess);
    }

    std::cout << "[EverythingIndexManager] Reindex triggered successfully" << std::endl;
    return true;
}

bool EverythingIndexManager::NormalizePath(std::wstring& path)
{
    try
    {
        std::filesystem::path fsPath(path);

        // Convert to absolute path
        if (!fsPath.is_absolute())
        {
            fsPath = std::filesystem::absolute(fsPath);
        }

        // Get canonical path (resolve symlinks, etc.) if exists
        if (std::filesystem::exists(fsPath))
        {
            fsPath = std::filesystem::canonical(fsPath);
        }

        // Convert to wstring and remove trailing separator
        path = fsPath.wstring();
        if (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        {
            path.pop_back();
        }

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[EverythingIndexManager] Path normalization failed: " << e.what() << std::endl;
        return false;
    }
}

bool EverythingIndexManager::IsFolderInList(const std::vector<std::wstring>& folders, const std::wstring& path)
{
    std::wstring normalizedPath = path;
    if (!NormalizePath(normalizedPath))
    {
        return false;
    }

    for (const auto& folder : folders)
    {
        std::wstring normalizedFolder = folder;
        NormalizePath(normalizedFolder);

        // Case-insensitive comparison on Windows
        if (_wcsicmp(normalizedPath.c_str(), normalizedFolder.c_str()) == 0)
        {
            return true;
        }
    }

    return false;
}

bool EverythingIndexManager::ReadIniFile(std::vector<std::wstring>& folders)
{
    folders.clear();

    if (!std::filesystem::exists(m_everythingIniPath))
    {
        std::wcerr << L"[EverythingIndexManager] INI file not found: " << m_everythingIniPath << std::endl;
        return false;
    }

    // Read folder_count
    int folderCount = GetPrivateProfileIntW(L"Folders", L"folder_count", 0, m_everythingIniPath.c_str());

    std::cout << "[EverythingIndexManager] Reading " << folderCount << " folders from INI" << std::endl;

    // Read each folder
    for (int i = 0; i < folderCount; ++i)
    {
        std::wstring key = L"folder" + std::to_wstring(i);
        wchar_t buffer[4096] = {};
        DWORD result = GetPrivateProfileStringW(L"Folders", key.c_str(), L"", buffer, 4096, m_everythingIniPath.c_str());

        if (result > 0 && wcslen(buffer) > 0)
        {
            folders.push_back(buffer);
        }
    }

    return true;
}

bool EverythingIndexManager::WriteIniFile(const std::vector<std::wstring>& folders)
{
    if (!std::filesystem::exists(m_everythingIniPath))
    {
        std::wcerr << L"[EverythingIndexManager] INI file not found: " << m_everythingIniPath << std::endl;
        return false;
    }

    std::cout << "[EverythingIndexManager] Writing " << folders.size() << " folders to INI" << std::endl;

    // Write folder_count
    std::wstring countStr = std::to_wstring(folders.size());
    if (!WritePrivateProfileStringW(L"Folders", L"folder_count", countStr.c_str(), m_everythingIniPath.c_str()))
    {
        std::cerr << "[EverythingIndexManager] Failed to write folder_count" << std::endl;
        return false;
    }

    // Write each folder
    for (size_t i = 0; i < folders.size(); ++i)
    {
        std::wstring key = L"folder" + std::to_wstring(i);
        if (!WritePrivateProfileStringW(L"Folders", key.c_str(), folders[i].c_str(), m_everythingIniPath.c_str()))
        {
            std::wcerr << L"[EverythingIndexManager] Failed to write " << key << std::endl;
            return false;
        }

        // Also write subfolders flag (always 1 to include subfolders)
        std::wstring subfoldersKey = key + L"_subfolders";
        WritePrivateProfileStringW(L"Folders", subfoldersKey.c_str(), L"1", m_everythingIniPath.c_str());
    }

    return true;
}

bool EverythingIndexManager::AddFolderToIndex(const std::wstring& folderPath)
{
    if (!m_everythingAvailable)
    {
        return false; // Silently fail if Everything not available
    }

    std::wstring normalizedPath = folderPath;
    if (!NormalizePath(normalizedPath))
    {
        std::wcerr << L"[EverythingIndexManager] Failed to normalize path: " << folderPath << std::endl;
        return false;
    }

    std::wcout << L"[EverythingIndexManager] Adding folder to index: " << normalizedPath << std::endl;

    // Check if Everything is running
    bool wasRunning = IsEverythingRunning();

    // If Everything is running, stop it to safely modify INI
    if (wasRunning && !StopEverything())
    {
        std::cerr << "[EverythingIndexManager] Failed to stop Everything" << std::endl;
        return false;
    }

    // Read current folders
    std::vector<std::wstring> folders;
    if (!ReadIniFile(folders))
    {
        if (wasRunning) StartEverything();
        return false;
    }

    // Check if already exists
    if (IsFolderInList(folders, normalizedPath))
    {
        std::wcout << L"[EverythingIndexManager] Folder already in index: " << normalizedPath << std::endl;
        if (wasRunning) StartEverything();
        return true; // Already added, not an error
    }

    // Add folder
    folders.push_back(normalizedPath);

    // Write back
    if (!WriteIniFile(folders))
    {
        if (wasRunning) StartEverything();
        return false;
    }

    // Restart Everything and trigger rescan if it was running
    if (wasRunning)
    {
        if (!StartEverything())
        {
            std::cerr << "[EverythingIndexManager] Warning: Failed to restart Everything" << std::endl;
            return false;
        }

        // Wait a moment for Everything to fully start
        Sleep(1000);

        // Trigger a rescan to immediately index the new folder
        RescanEverything();
    }

    std::wcout << L"[EverythingIndexManager] Successfully added folder to index: " << normalizedPath << std::endl;
    return true;
}

bool EverythingIndexManager::RemoveFolderFromIndex(const std::wstring& folderPath)
{
    if (!m_everythingAvailable)
    {
        return false; // Silently fail if Everything not available
    }

    std::wstring normalizedPath = folderPath;
    if (!NormalizePath(normalizedPath))
    {
        std::wcerr << L"[EverythingIndexManager] Failed to normalize path: " << folderPath << std::endl;
        return false;
    }

    std::wcout << L"[EverythingIndexManager] Removing folder from index: " << normalizedPath << std::endl;

    // Check if Everything is running
    bool wasRunning = IsEverythingRunning();

    // If Everything is running, stop it to safely modify INI
    if (wasRunning && !StopEverything())
    {
        std::cerr << "[EverythingIndexManager] Failed to stop Everything" << std::endl;
        return false;
    }

    // Read current folders
    std::vector<std::wstring> folders;
    if (!ReadIniFile(folders))
    {
        if (wasRunning) StartEverything();
        return false;
    }

    // Remove folder if it exists
    bool found = false;
    std::vector<std::wstring> newFolders;
    for (const auto& folder : folders)
    {
        std::wstring normalizedFolder = folder;
        NormalizePath(normalizedFolder);

        if (_wcsicmp(normalizedPath.c_str(), normalizedFolder.c_str()) != 0)
        {
            newFolders.push_back(folder);
        }
        else
        {
            found = true;
        }
    }

    if (!found)
    {
        std::wcout << L"[EverythingIndexManager] Folder not in index: " << normalizedPath << std::endl;
        if (wasRunning) StartEverything();
        return true; // Not found, but not an error
    }

    // Write back
    if (!WriteIniFile(newFolders))
    {
        if (wasRunning) StartEverything();
        return false;
    }

    // Restart Everything and trigger rescan if it was running
    if (wasRunning)
    {
        if (!StartEverything())
        {
            std::cerr << "[EverythingIndexManager] Warning: Failed to restart Everything" << std::endl;
            return false;
        }

        // Wait a moment for Everything to fully start
        Sleep(1000);

        // Trigger a rescan to immediately update the index
        RescanEverything();
    }

    std::wcout << L"[EverythingIndexManager] Successfully removed folder from index: " << normalizedPath << std::endl;
    return true;
}

bool EverythingIndexManager::SyncAllSubscriptions(const std::vector<std::wstring>& activePaths)
{
    if (!m_everythingAvailable)
    {
        return false; // Silently fail if Everything not available
    }

    if (activePaths.empty())
    {
        std::cout << "[EverythingIndexManager] No active subscriptions to sync" << std::endl;
        return true;
    }

    std::cout << "[EverythingIndexManager] Syncing " << activePaths.size() << " active subscriptions" << std::endl;

    // Stop Everything once
    bool wasRunning = IsEverythingRunning();
    if (wasRunning && !StopEverything())
    {
        std::cerr << "[EverythingIndexManager] Failed to stop Everything" << std::endl;
        return false;
    }

    // Read current folders
    std::vector<std::wstring> folders;
    if (!ReadIniFile(folders))
    {
        if (wasRunning) StartEverything();
        return false;
    }

    // Add all active subscriptions that aren't already present
    bool modified = false;
    for (const auto& path : activePaths)
    {
        std::wstring normalizedPath = path;
        if (NormalizePath(normalizedPath) && !IsFolderInList(folders, normalizedPath))
        {
            std::wcout << L"[EverythingIndexManager] Adding missing subscription: " << normalizedPath << std::endl;
            folders.push_back(normalizedPath);
            modified = true;
        }
    }

    // Write back if modified
    if (modified)
    {
        if (!WriteIniFile(folders))
        {
            if (wasRunning) StartEverything();
            return false;
        }
        std::cout << "[EverythingIndexManager] Successfully synced subscriptions" << std::endl;
    }
    else
    {
        std::cout << "[EverythingIndexManager] All subscriptions already in sync" << std::endl;
    }

    // Restart Everything and trigger rescan
    if (wasRunning)
    {
        if (!StartEverything())
        {
            std::cerr << "[EverythingIndexManager] Warning: Failed to restart Everything" << std::endl;
            return false;
        }

        // If we modified the index, trigger a rescan
        if (modified)
        {
            // Wait a moment for Everything to fully start
            Sleep(1000);

            // Trigger a rescan to immediately index new folders
            RescanEverything();
        }
    }

    return true;
}

bool EverythingIndexManager::IsFolderIndexed(const std::wstring& folderPath)
{
    if (!m_everythingAvailable)
    {
        return false;
    }

    std::wstring normalizedPath = folderPath;
    if (!NormalizePath(normalizedPath))
    {
        return false;
    }

    // Read current folders
    std::vector<std::wstring> folders;
    if (!ReadIniFile(folders))
    {
        return false;
    }

    return IsFolderInList(folders, normalizedPath);
}

} // namespace UFB
