#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include "imgui.h"

// Forward declarations
namespace UFB {
    class SubscriptionManager;
    class MetadataManager;
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
                    UFB::SubscriptionManager* subscriptionManager,
                    UFB::MetadataManager* metadataManager,
                    UFB::ProjectConfig* projectConfig);

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

    // Callbacks for browser/window opening
    std::function<void(const std::wstring& path)> onOpenInBrowser1;
    std::function<void(const std::wstring& path)> onOpenInBrowser2;
    std::function<void(const std::wstring& path)> onOpenInNewWindow;

    // Window open state
    bool IsOpen() const { return m_isOpen; }

private:
    // Window state
    bool m_isOpen = true;
    bool m_isShutdown = false;  // Prevent callbacks during cleanup
    bool m_isRendering = false; // Prevent modifying m_allItems during iteration
    // Job path and name
    std::wstring m_jobPath;      // e.g., "D:\Projects\MyJob"
    std::wstring m_jobName;      // e.g., "MyJob"

    // Manager dependencies
    UFB::SubscriptionManager* m_subscriptionManager = nullptr;
    UFB::MetadataManager* m_metadataManager = nullptr;
    UFB::ProjectConfig* m_projectConfig = nullptr;

    // Tracked items cache (loaded from database)
    std::vector<UFB::ShotMetadata> m_trackedShots;
    std::vector<UFB::ShotMetadata> m_trackedAssets;
    std::vector<UFB::ShotMetadata> m_trackedPostings;
    std::vector<UFB::ShotMetadata> m_manualTasks;

    // Unified table state
    std::vector<UFB::ShotMetadata> m_allItems;  // Combined filtered items
    int m_selectedItemIndex = -1;
    int m_allItemsSortColumn = -1;
    bool m_allItemsSortAscending = true;

    // Filter state
    std::set<std::string> m_filterTypes;        // Selected types: "shot", "asset", "posting", "manual_task"
    std::set<std::string> m_filterArtists;      // Selected artist names
    std::set<int> m_filterPriorities;           // Selected priorities (1, 2, 3)
    int m_filterDueDate = 0;                    // 0=All, 1=Overdue, 2=Today, 3=This Week, 4=This Month

    // Available filter values (populated from data)
    std::set<std::string> m_availableArtists;
    std::set<int> m_availablePriorities;

    // Manual task dialog
    bool m_showAddTaskDialog = false;
    char m_taskNameBuffer[256] = "";
    char m_taskNoteBuffer[512] = "";

    // Date picker state for unified table
    bool m_showAllItemsDatePicker = false;
    int m_allItemsDatePickerIndex = -1;

    // Note editor modal state
    bool m_showNoteEditor = false;
    int m_noteEditorItemIndex = -1;
    std::vector<UFB::ShotMetadata>* m_noteEditorItemList = nullptr;
    char m_noteEditorBuffer[4096] = "";

    // Link editor modal state
    bool m_showLinkEditor = false;
    int m_linkEditorItemIndex = -1;
    std::vector<UFB::ShotMetadata>* m_linkEditorItemList = nullptr;
    char m_linkEditorBuffer[1024] = "";

    // Helper functions
    void RefreshTrackedItems();
    void ReloadTrackedItems();  // Reload tracked items (called by observer)
    void DrawUnifiedTable();  // Draw single unified table with all items
    void UpdateUnifiedItemsList();  // Combine and filter all items into m_allItems
    bool PassesFilters(const UFB::ShotMetadata& item);  // Check if item passes all active filters
    void CollectAvailableFilterValues();  // Collect unique values from all items
    void SortItems(std::vector<UFB::ShotMetadata>& items, int column, bool ascending);
    const char* GetStatusLabel(const std::string& status);
    const char* GetPriorityLabel(int priority);
    ImVec4 GetStatusColor(const std::string& status, const std::string& folderType = "");
    ImVec4 GetCategoryColor(const std::string& category, const std::string& folderType = "");
    ImVec4 GetPriorityColor(int priority);
    ImVec4 GetTypeColor(const std::string& itemType);  // Color for type badges
    std::string FormatDate(uint64_t timestamp);
};
