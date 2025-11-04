#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "imgui.h"

// Forward declarations
namespace UFB {
    class SubscriptionManager;
    class ProjectConfig;
    struct ShotMetadata;
}

class ProjectTrackerView
{
public:
    ProjectTrackerView();
    ~ProjectTrackerView();

    // Initialize with job path and name
    void Initialize(const std::wstring& jobPath, const std::wstring& jobName,
                    UFB::SubscriptionManager* subscriptionManager, UFB::ProjectConfig* projectConfig);

    // Shutdown and cleanup
    void Shutdown();

    // Draw the tracker view UI
    void Draw(const char* title, HWND hwnd);

    // Get the job path
    const std::wstring& GetJobPath() const { return m_jobPath; }

    // Get the job name (for window title)
    const std::wstring& GetJobName() const { return m_jobName; }

    // Callback for closing this view
    std::function<void()> onClose;

    // Callbacks for opening items in their respective views
    std::function<void(const std::wstring& shotPath)> onOpenShot;
    std::function<void(const std::wstring& assetPath)> onOpenAsset;
    std::function<void(const std::wstring& postingPath)> onOpenPosting;

    // Window open state
    bool IsOpen() const { return m_isOpen; }

private:
    // Window state
    bool m_isOpen = true;
    // Job path and name
    std::wstring m_jobPath;      // e.g., "D:\Projects\MyJob"
    std::wstring m_jobName;      // e.g., "MyJob"

    // Manager dependencies
    UFB::SubscriptionManager* m_subscriptionManager = nullptr;
    UFB::ProjectConfig* m_projectConfig = nullptr;

    // Tracked items cache
    std::vector<UFB::ShotMetadata> m_trackedShots;
    std::vector<UFB::ShotMetadata> m_trackedAssets;
    std::vector<UFB::ShotMetadata> m_trackedPostings;
    std::vector<UFB::ShotMetadata> m_manualTasks;

    // UI state
    int m_selectedShotIndex = -1;
    int m_selectedAssetIndex = -1;
    int m_selectedPostingIndex = -1;
    int m_selectedTaskIndex = -1;

    // Sorting state for each table
    int m_shotsSortColumn = -1;
    bool m_shotsSortAscending = true;
    int m_assetsSortColumn = -1;
    bool m_assetsSortAscending = true;
    int m_postingsSortColumn = -1;
    bool m_postingsSortAscending = true;
    int m_tasksSortColumn = -1;
    bool m_tasksSortAscending = true;

    // Manual task dialog
    bool m_showAddTaskDialog = false;
    char m_taskNameBuffer[256] = "";
    char m_taskNoteBuffer[512] = "";

    // Date picker state for each table
    bool m_showShotDatePicker = false;
    int m_shotDatePickerIndex = -1;
    bool m_showAssetDatePicker = false;
    int m_assetDatePickerIndex = -1;
    bool m_showPostingDatePicker = false;
    int m_postingDatePickerIndex = -1;
    bool m_showTaskDatePicker = false;
    int m_taskDatePickerIndex = -1;

    // Note editor modal state
    bool m_showNoteEditor = false;
    int m_noteEditorItemIndex = -1;
    std::vector<UFB::ShotMetadata>* m_noteEditorItemList = nullptr;
    char m_noteEditorBuffer[4096] = "";

    // Helper functions
    void RefreshTrackedItems();
    void DrawItemsTable(const char* tableName, const char* itemType, std::vector<UFB::ShotMetadata>& items,
                       int& selectedIndex, int& sortColumn, bool& sortAscending,
                       bool& showDatePicker, int& datePickerIndex);
    void DrawManualTasksTable();
    void SortItems(std::vector<UFB::ShotMetadata>& items, int column, bool ascending);
    const char* GetStatusLabel(const std::string& status);
    const char* GetPriorityLabel(int priority);
    ImVec4 GetStatusColor(const std::string& status, const std::string& folderType = "");
    ImVec4 GetCategoryColor(const std::string& category, const std::string& folderType = "");
    ImVec4 GetPriorityColor(int priority);
    std::string FormatDate(uint64_t timestamp);
};
