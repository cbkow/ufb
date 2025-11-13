#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "imgui.h"
#include "backup_manager.h"

// Forward declarations
namespace UFB {
    class BackupManager;
}

// Structure to hold backup item info for display
struct BackupItemInfo
{
    std::string name;
    std::string itemType;  // shot, asset, posting, manual_task
    std::string status;
    std::string artist;
    int priority = 0;
    uint64_t modifiedTime = 0;
    std::string shotPath;
};

class BackupRestoreView
{
public:
    BackupRestoreView();
    ~BackupRestoreView();

    // Initialize with job path and name
    void Initialize(const std::wstring& jobPath, const std::wstring& jobName,
                    UFB::BackupManager* backupManager);

    // Shutdown and cleanup
    void Shutdown();

    // Draw the backup restore view UI
    void Draw(const char* title, HWND hwnd);

    // Get the job path
    const std::wstring& GetJobPath() const { return m_jobPath; }

    // Get the job name (for window title)
    const std::wstring& GetJobName() const { return m_jobName; }

    // Callback for closing this view
    std::function<void()> onClose;

    // Window open state
    bool IsOpen() const { return m_isOpen; }

private:
    // Window state
    bool m_isOpen = true;
    bool m_isShutdown = false;

    // Job info
    std::wstring m_jobPath;
    std::wstring m_jobName;

    // Manager dependency
    UFB::BackupManager* m_backupManager = nullptr;

    // Backup list
    std::vector<UFB::BackupInfo> m_backups;
    int m_selectedBackupIndex = -1;

    // Selected backup contents
    std::vector<BackupItemInfo> m_selectedBackupItems;
    bool m_itemsLoaded = false;

    // UI state
    bool m_showRestoreConfirm = false;
    bool m_restoreInProgress = false;
    std::string m_statusMessage;

    // Helper functions
    void RefreshBackupList();
    void LoadBackupContents(int backupIndex);
    void DrawBackupTable();
    void DrawPreviewPanel();
    void DrawRestoreConfirmModal();
    std::string FormatTimestamp(uint64_t timestamp);
    std::string FormatFileSize(size_t bytes);
    std::string GetItemDisplayName(const std::string& shotPath);
};
