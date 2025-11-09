#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include "imgui.h"
#include "subscription_manager.h"
#include "metadata_manager.h"
#include "project_config.h"

// Forward declarations
namespace UFB {
    // Already included above
}

class AggregatedTrackerView
{
public:
    AggregatedTrackerView();
    ~AggregatedTrackerView();

    // Initialize with managers
    void Initialize(UFB::SubscriptionManager* subscriptionManager,
                    UFB::MetadataManager* metadataManager);

    // Shutdown and cleanup
    void Shutdown();

    // Draw the aggregated tracker view UI
    void Draw(const char* title, HWND hwnd);

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

    // Callback for opening a specific project tracker
    std::function<void(const std::wstring& jobPath, const std::wstring& jobName)> onOpenProjectTracker;

    // Window open state
    bool IsOpen() const { return m_isOpen; }

private:
    // Wrapper struct to associate metadata with project name
    struct TrackedItemWithProject
    {
        UFB::ShotMetadata metadata;
        std::wstring jobPath;
        std::wstring jobName;
    };

    // Window state
    bool m_isOpen = true;
    bool m_isShutdown = false;  // Prevent callbacks during cleanup
    bool m_isRendering = false; // Prevent modifying m_allItems during iteration

    // Manager dependencies
    UFB::SubscriptionManager* m_subscriptionManager = nullptr;
    UFB::MetadataManager* m_metadataManager = nullptr;

    // Project config cache (one per unique project)
    std::map<std::wstring, std::unique_ptr<UFB::ProjectConfig>> m_projectConfigs;

    // Unified table state
    std::vector<TrackedItemWithProject> m_allItems;  // Combined filtered items from all projects
    int m_selectedItemIndex = -1;
    int m_allItemsSortColumn = -1;
    bool m_allItemsSortAscending = true;

    // Filter state
    std::set<std::wstring> m_filterProjects;        // Selected project names
    std::set<std::string> m_filterTypes;            // Selected types: "shot", "asset", "posting", "manual_task"
    std::set<std::string> m_filterArtists;          // Selected artist names
    std::set<int> m_filterPriorities;               // Selected priorities (1, 2, 3)
    int m_filterDueDate = 0;                        // 0=All, 1=Overdue, 2=Today, 3=This Week, 4=This Month

    // Available filter values (populated from data)
    std::set<std::wstring> m_availableProjects;
    std::set<std::string> m_availableArtists;
    std::set<int> m_availablePriorities;

    // Date picker state for unified table
    bool m_showAllItemsDatePicker = false;
    int m_allItemsDatePickerIndex = -1;

    // Note editor modal state
    bool m_showNoteEditor = false;
    int m_noteEditorItemIndex = -1;
    char m_noteEditorBuffer[4096] = "";

    // Link editor modal state
    bool m_showLinkEditor = false;
    int m_linkEditorItemIndex = -1;
    char m_linkEditorBuffer[1024] = "";

    // Helper functions
    void RefreshTrackedItems();
    void ReloadTrackedItems();  // Reload tracked items (called by observer)
    void DrawUnifiedTable();  // Draw single unified table with all items
    void UpdateUnifiedItemsList();  // Combine and filter all items into m_allItems
    bool PassesFilters(const TrackedItemWithProject& item);  // Check if item passes all active filters
    void CollectAvailableFilterValues();  // Collect unique values from all items
    void SortItems(std::vector<TrackedItemWithProject>& items, int column, bool ascending);
    UFB::ProjectConfig* GetConfigForItem(const TrackedItemWithProject& item);  // Load/cache project config
    const char* GetStatusLabel(const std::string& status);
    const char* GetPriorityLabel(int priority);
    ImVec4 GetStatusColor(const std::string& status, const std::string& folderType, UFB::ProjectConfig* config);
    ImVec4 GetCategoryColor(const std::string& category, const std::string& folderType, UFB::ProjectConfig* config);
    ImVec4 GetPriorityColor(int priority);
    ImVec4 GetTypeColor(const std::string& itemType);  // Color for type badges
    std::string FormatDate(uint64_t timestamp);
};
