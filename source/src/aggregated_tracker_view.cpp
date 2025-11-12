#include "aggregated_tracker_view.h"
#include "subscription_manager.h"
#include "metadata_manager.h"
#include "project_config.h"
#include "utils.h"
#include "ImGuiDatePicker.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

// C++20 changed u8"" literals to char8_t, need to cast for ImGui
#define U8(x) reinterpret_cast<const char*>(u8##x)

// External function from main.cpp
extern ImVec4 GetWindowsAccentColor();

// External font pointers (from main.cpp)
extern ImFont* font_regular;
extern ImFont* font_mono;
extern ImFont* font_icons;

// Helper functions to convert between tm and uint64_t Unix timestamps
static tm TimestampToTm(uint64_t timestampMillis)
{
    // Clamp to valid range (1970-3000) to prevent crashes from corrupted data
    const uint64_t MAX_TIMESTAMP_MS = 32503680000000ULL;  // Year 3000
    if (timestampMillis > MAX_TIMESTAMP_MS) {
        timestampMillis = MAX_TIMESTAMP_MS;
    }

    time_t timeSeconds = static_cast<time_t>(timestampMillis / 1000);
    tm result = {};
    #ifdef _WIN32
        localtime_s(&result, &timeSeconds);
    #else
        localtime_r(&timeSeconds, &result);
    #endif
    return result;
}

static uint64_t TmToTimestamp(const tm& time)
{
    tm copy = time;
    time_t timeSeconds = mktime(&copy);
    return static_cast<uint64_t>(timeSeconds) * 1000;
}

// Hex color string to ImVec4
static ImVec4 HexToImVec4(const std::string& hex)
{
    if (hex.empty() || hex[0] != '#' || hex.length() != 7)
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    int r, g, b;
    sscanf_s(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

AggregatedTrackerView::AggregatedTrackerView()
{
}

AggregatedTrackerView::~AggregatedTrackerView()
{
    Shutdown();
}

void AggregatedTrackerView::Initialize(UFB::SubscriptionManager* subscriptionManager,
                                       UFB::MetadataManager* metadataManager)
{
    m_subscriptionManager = subscriptionManager;
    m_metadataManager = metadataManager;

    // Register observer for real-time metadata updates
    if (m_metadataManager)
    {
        m_metadataManager->RegisterObserver([this](const std::wstring& changedJobPath) {
            // Reload all tracked items when any job changes
            std::wcout << L"[AggregatedTrackerView] Metadata changed for job, reloading..." << std::endl;
            ReloadTrackedItems();
        });
    }

    // Load tracked items
    RefreshTrackedItems();

    // Collect available filter values from loaded items
    CollectAvailableFilterValues();
}

void AggregatedTrackerView::Shutdown()
{
    // Prevent double-shutdown
    if (m_isShutdown)
        return;

    // Set shutdown flag FIRST - this prevents observer callbacks from doing any work
    m_isShutdown = true;

    // Clear tracked items
    m_allItems.clear();

    // Clear filter state
    m_filterProjects.clear();
    m_filterTypes.clear();
    m_filterArtists.clear();
    m_filterPriorities.clear();

    // Clear available filter values
    m_availableProjects.clear();
    m_availableArtists.clear();
    m_availablePriorities.clear();

    // Clear project config cache
    m_projectConfigs.clear();
}

void AggregatedTrackerView::RefreshTrackedItems()
{
    // Don't refresh if we're shutting down
    if (m_isShutdown)
        return;

    if (!m_subscriptionManager)
        return;

    // Clear current items
    m_allItems.clear();

    // Get all subscribed jobs
    std::vector<UFB::Subscription> subscriptions = m_subscriptionManager->GetAllSubscriptions();

    // Load tracked items from each subscribed job
    for (const auto& subscription : subscriptions)
    {
        const std::wstring& jobPath = subscription.jobPath;
        const std::wstring& jobName = subscription.jobName;

        // Load tracked items by type for this job
        std::vector<UFB::ShotMetadata> trackedShots = m_subscriptionManager->GetTrackedItems(jobPath, "shot");
        std::vector<UFB::ShotMetadata> trackedAssets = m_subscriptionManager->GetTrackedItems(jobPath, "asset");
        std::vector<UFB::ShotMetadata> trackedPostings = m_subscriptionManager->GetTrackedItems(jobPath, "posting");
        std::vector<UFB::ShotMetadata> manualTasks = m_subscriptionManager->GetTrackedItems(jobPath, "manual_task");

        // Wrap each item with job information
        for (const auto& item : trackedShots)
        {
            TrackedItemWithProject wrapped;
            wrapped.metadata = item;
            wrapped.jobPath = jobPath;
            wrapped.jobName = jobName;
            m_allItems.push_back(wrapped);
        }

        for (const auto& item : trackedAssets)
        {
            TrackedItemWithProject wrapped;
            wrapped.metadata = item;
            wrapped.jobPath = jobPath;
            wrapped.jobName = jobName;
            m_allItems.push_back(wrapped);
        }

        for (const auto& item : trackedPostings)
        {
            TrackedItemWithProject wrapped;
            wrapped.metadata = item;
            wrapped.jobPath = jobPath;
            wrapped.jobName = jobName;
            m_allItems.push_back(wrapped);
        }

        for (const auto& item : manualTasks)
        {
            TrackedItemWithProject wrapped;
            wrapped.metadata = item;
            wrapped.jobPath = jobPath;
            wrapped.jobName = jobName;
            m_allItems.push_back(wrapped);
        }
    }

    // Update unified items list with filters applied
    UpdateUnifiedItemsList();

    // Collect available filter values
    CollectAvailableFilterValues();
}

void AggregatedTrackerView::ReloadTrackedItems()
{
    // CRITICAL: Check shutdown flag FIRST before accessing any members
    if (m_isShutdown)
        return;

    // Reload tracked items from database (called by observer when remote changes arrive)
    RefreshTrackedItems();

    // The UI will pick up the new tracked items on next frame
    std::wcout << L"[AggregatedTrackerView] Tracked items reloaded successfully" << std::endl;
}

void AggregatedTrackerView::Draw(const char* title, HWND hwnd)
{
    // CRITICAL: Don't draw if we're shutting down
    if (m_isShutdown)
        return;

    // Use close button and check if window was closed
    bool windowOpen = ImGui::Begin(title, &m_isOpen, ImGuiWindowFlags_None);

    if (!windowOpen)
    {
        ImGui::End();
        return;
    }

    // Header with "All Projects" label
    ImGui::Text("Project: All Projects");
    ImGui::Separator();

    // Filter toolbar

    // Type filter button
    {
        int activeFilters = static_cast<int>(m_filterTypes.size());
        std::string typeLabel = "Type" + (activeFilters > 0 ? " (" + std::to_string(activeFilters) + ")" : "");

        if (ImGui::Button(typeLabel.c_str()))
        {
            ImGui::OpenPopup("TypeFilter");
        }

        if (ImGui::BeginPopup("TypeFilter"))
        {
            ImGui::Text("Filter by Type:");
            ImGui::Separator();

            bool hasShot = m_filterTypes.find("shot") != m_filterTypes.end();
            if (ImGui::Checkbox("Shots", &hasShot))
            {
                if (hasShot) m_filterTypes.insert("shot");
                else m_filterTypes.erase("shot");
                UpdateUnifiedItemsList();
            }

            bool hasAsset = m_filterTypes.find("asset") != m_filterTypes.end();
            if (ImGui::Checkbox("Assets", &hasAsset))
            {
                if (hasAsset) m_filterTypes.insert("asset");
                else m_filterTypes.erase("asset");
                UpdateUnifiedItemsList();
            }

            bool hasPosting = m_filterTypes.find("posting") != m_filterTypes.end();
            if (ImGui::Checkbox("Postings", &hasPosting))
            {
                if (hasPosting) m_filterTypes.insert("posting");
                else m_filterTypes.erase("posting");
                UpdateUnifiedItemsList();
            }

            bool hasTask = m_filterTypes.find("manual_task") != m_filterTypes.end();
            if (ImGui::Checkbox("Custom Tasks", &hasTask))
            {
                if (hasTask) m_filterTypes.insert("manual_task");
                else m_filterTypes.erase("manual_task");
                UpdateUnifiedItemsList();
            }

            ImGui::Separator();
            if (ImGui::Button("Clear All"))
            {
                m_filterTypes.clear();
                UpdateUnifiedItemsList();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();

    // Project filter button
    {
        int activeFilters = static_cast<int>(m_filterProjects.size());
        std::string projectLabel = "Project" + (activeFilters > 0 ? " (" + std::to_string(activeFilters) + ")" : "");

        if (ImGui::Button(projectLabel.c_str()))
        {
            ImGui::OpenPopup("ProjectFilter");
        }

        if (ImGui::BeginPopup("ProjectFilter"))
        {
            ImGui::Text("Filter by Project:");
            ImGui::Separator();

            for (const auto& project : m_availableProjects)
            {
                char projectUtf8[256];
                WideCharToMultiByte(CP_UTF8, 0, project.c_str(), -1, projectUtf8, sizeof(projectUtf8), nullptr, nullptr);

                bool isSelected = m_filterProjects.find(project) != m_filterProjects.end();
                if (ImGui::Checkbox(projectUtf8, &isSelected))
                {
                    if (isSelected) m_filterProjects.insert(project);
                    else m_filterProjects.erase(project);
                    UpdateUnifiedItemsList();
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Clear All"))
            {
                m_filterProjects.clear();
                UpdateUnifiedItemsList();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();

    // Artist filter button
    {
        int activeFilters = static_cast<int>(m_filterArtists.size());
        std::string artistLabel = "Artist" + (activeFilters > 0 ? " (" + std::to_string(activeFilters) + ")" : "");

        if (ImGui::Button(artistLabel.c_str()))
        {
            ImGui::OpenPopup("ArtistFilter");
        }

        if (ImGui::BeginPopup("ArtistFilter"))
        {
            ImGui::Text("Filter by Artist:");
            ImGui::Separator();

            for (const auto& artist : m_availableArtists)
            {
                bool isSelected = m_filterArtists.find(artist) != m_filterArtists.end();
                if (ImGui::Checkbox(artist.c_str(), &isSelected))
                {
                    if (isSelected) m_filterArtists.insert(artist);
                    else m_filterArtists.erase(artist);
                    UpdateUnifiedItemsList();
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Clear All"))
            {
                m_filterArtists.clear();
                UpdateUnifiedItemsList();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();

    // Priority filter button
    {
        int activeFilters = static_cast<int>(m_filterPriorities.size());
        std::string priorityLabel = "Priority" + (activeFilters > 0 ? " (" + std::to_string(activeFilters) + ")" : "");

        if (ImGui::Button(priorityLabel.c_str()))
        {
            ImGui::OpenPopup("PriorityFilter");
        }

        if (ImGui::BeginPopup("PriorityFilter"))
        {
            ImGui::Text("Filter by Priority:");
            ImGui::Separator();

            bool hasHigh = m_filterPriorities.find(1) != m_filterPriorities.end();
            if (ImGui::Checkbox("High", &hasHigh))
            {
                if (hasHigh) m_filterPriorities.insert(1);
                else m_filterPriorities.erase(1);
                UpdateUnifiedItemsList();
            }

            bool hasMedium = m_filterPriorities.find(2) != m_filterPriorities.end();
            if (ImGui::Checkbox("Medium", &hasMedium))
            {
                if (hasMedium) m_filterPriorities.insert(2);
                else m_filterPriorities.erase(2);
                UpdateUnifiedItemsList();
            }

            bool hasLow = m_filterPriorities.find(3) != m_filterPriorities.end();
            if (ImGui::Checkbox("Low", &hasLow))
            {
                if (hasLow) m_filterPriorities.insert(3);
                else m_filterPriorities.erase(3);
                UpdateUnifiedItemsList();
            }

            ImGui::Separator();
            if (ImGui::Button("Clear All"))
            {
                m_filterPriorities.clear();
                UpdateUnifiedItemsList();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();

    // Due Date filter button
    {
        std::string dueDateLabel = "Due Date";
        if (m_filterDueDate > 0)
        {
            const char* labels[] = {"All", "Overdue", "Today", "This Week", "This Month"};
            dueDateLabel = std::string(labels[m_filterDueDate]);
        }

        if (ImGui::Button(dueDateLabel.c_str()))
        {
            ImGui::OpenPopup("DueDateFilter");
        }

        if (ImGui::BeginPopup("DueDateFilter"))
        {
            ImGui::Text("Filter by Due Date:");
            ImGui::Separator();

            if (ImGui::RadioButton("All", m_filterDueDate == 0))
            {
                m_filterDueDate = 0;
                UpdateUnifiedItemsList();
            }

            if (ImGui::RadioButton("Overdue", m_filterDueDate == 1))
            {
                m_filterDueDate = 1;
                UpdateUnifiedItemsList();
            }

            if (ImGui::RadioButton("Today", m_filterDueDate == 2))
            {
                m_filterDueDate = 2;
                UpdateUnifiedItemsList();
            }

            if (ImGui::RadioButton("This Week", m_filterDueDate == 3))
            {
                m_filterDueDate = 3;
                UpdateUnifiedItemsList();
            }

            if (ImGui::RadioButton("This Month", m_filterDueDate == 4))
            {
                m_filterDueDate = 4;
                UpdateUnifiedItemsList();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::SameLine();

    // Refresh button
    if (font_icons)
        ImGui::PushFont(font_icons);

    if (ImGui::Button(U8("\uE5D5")))  // Material Icons refresh symbol
    {
        RefreshTrackedItems();
    }

    if (font_icons)
        ImGui::PopFont();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refresh");

    ImGui::Separator();

    // Draw unified table
    DrawUnifiedTable();

    ImGui::End();

    // Date Picker Modal
    if (m_showAllItemsDatePicker)
    {
        ImGui::OpenPopup("Select Due Date");
        m_showAllItemsDatePicker = false;  // Only open once
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Select Due Date", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Reduce padding to make date picker more compact
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 4.0f));

        if (m_allItemsDatePickerIndex >= 0 && m_allItemsDatePickerIndex < static_cast<int>(m_allItems.size()))
        {
            auto& item = m_allItems[m_allItemsDatePickerIndex];

            tm currentDate = TimestampToTm(item.metadata.dueDate > 0 ? item.metadata.dueDate : static_cast<uint64_t>(std::time(nullptr)) * 1000);

            if (ImGui::DatePicker("##datepicker", currentDate, false))
            {
                item.metadata.dueDate = TmToTimestamp(currentDate);
                if (m_subscriptionManager)
                {
                    m_subscriptionManager->CreateOrUpdateShotMetadata(item.metadata);
                }
            }

            if (ImGui::Button("Clear", ImVec2(120, 0)))
            {
                item.metadata.dueDate = 0;
                if (m_subscriptionManager)
                {
                    m_subscriptionManager->CreateOrUpdateShotMetadata(item.metadata);
                }
                m_allItemsDatePickerIndex = -1;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Close", ImVec2(120, 0)))
            {
                m_allItemsDatePickerIndex = -1;
                ImGui::CloseCurrentPopup();
            }
        }

        // Pop style variables (3: FramePadding, ItemSpacing, CellPadding)
        ImGui::PopStyleVar(3);

        ImGui::EndPopup();
    }

    // Note Editor Modal
    if (m_showNoteEditor)
    {
        ImGui::OpenPopup("Edit Note");
        m_showNoteEditor = false;  // Only open once
    }

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Edit Note", nullptr, ImGuiWindowFlags_NoScrollbar))
    {
        // Use regular font for the editor
        if (font_regular)
            ImGui::PushFont(font_regular);

        ImGui::TextWrapped("Edit note:");
        ImGui::Spacing();

        // Calculate available space for the text editor (reserve space for buttons and spacing)
        float availHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2;

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextMultiline("##noteeditor", m_noteEditorBuffer, sizeof(m_noteEditorBuffer),
            ImVec2(-FLT_MIN, availHeight), ImGuiInputTextFlags_WordWrap);

        if (ImGui::Button("Save", ImVec2(120, 0)))
        {
            if (m_noteEditorItemIndex >= 0 && m_noteEditorItemIndex < m_allItems.size())
            {
                m_allItems[m_noteEditorItemIndex].metadata.note = m_noteEditorBuffer;
                if (m_subscriptionManager)
                {
                    m_subscriptionManager->CreateOrUpdateShotMetadata(m_allItems[m_noteEditorItemIndex].metadata);
                }
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        if (font_regular)
            ImGui::PopFont();

        ImGui::EndPopup();
    }

    // Link Editor Modal
    if (m_showLinkEditor)
    {
        ImGui::OpenPopup("Edit Link");
        m_showLinkEditor = false;  // Only open once
    }

    ImGui::SetNextWindowSize(ImVec2(500, 150), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Edit Link", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Use regular font for the editor
        if (font_regular)
            ImGui::PushFont(font_regular);

        ImGui::TextWrapped("Enter URL:");
        ImGui::Spacing();

        ImGui::SetNextItemWidth(450);
        ImGui::InputText("##linkeditor", m_linkEditorBuffer, sizeof(m_linkEditorBuffer));

        ImGui::Spacing();

        if (ImGui::Button("Save", ImVec2(120, 0)))
        {
            if (m_linkEditorItemIndex >= 0 && m_linkEditorItemIndex < m_allItems.size())
            {
                m_allItems[m_linkEditorItemIndex].metadata.links = m_linkEditorBuffer;
                if (m_subscriptionManager)
                {
                    m_subscriptionManager->CreateOrUpdateShotMetadata(m_allItems[m_linkEditorItemIndex].metadata);
                }
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Clear", ImVec2(120, 0)))
        {
            if (m_linkEditorItemIndex >= 0 && m_linkEditorItemIndex < m_allItems.size())
            {
                m_allItems[m_linkEditorItemIndex].metadata.links = "";
                if (m_subscriptionManager)
                {
                    m_subscriptionManager->CreateOrUpdateShotMetadata(m_allItems[m_linkEditorItemIndex].metadata);
                }
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        if (font_regular)
            ImGui::PopFont();

        ImGui::EndPopup();
    }

}

void AggregatedTrackerView::SortItems(std::vector<TrackedItemWithProject>& items, int column, bool ascending)
{
    std::sort(items.begin(), items.end(), [column, ascending](const TrackedItemWithProject& a, const TrackedItemWithProject& b) {
        bool result = false;
        switch (column)
        {
            case 0: result = a.metadata.itemType < b.metadata.itemType; break;      // Type
            case 1: result = a.jobName < b.jobName; break;                          // Project
            case 2: result = a.metadata.shotPath < b.metadata.shotPath; break;      // Path
            case 3: result = a.metadata.status < b.metadata.status; break;          // Status
            case 4: result = a.metadata.category < b.metadata.category; break;      // Category
            case 5: result = a.metadata.priority < b.metadata.priority; break;      // Priority
            case 6: result = a.metadata.artist < b.metadata.artist; break;          // Artist
            case 7: result = a.metadata.dueDate < b.metadata.dueDate; break;        // Due Date
            case 8: result = a.metadata.modifiedTime < b.metadata.modifiedTime; break; // Modified Date
            case 9: result = a.metadata.links < b.metadata.links; break;            // Links
            case 10: result = a.metadata.note < b.metadata.note; break;             // Notes
            default: result = false; break;
        }
        return ascending ? result : !result;
    });
}

const char* AggregatedTrackerView::GetStatusLabel(const std::string& status)
{
    if (status.empty())
        return "-";
    return status.c_str();
}

const char* AggregatedTrackerView::GetPriorityLabel(int priority)
{
    switch (priority)
    {
        case 1: return "High";
        case 2: return "Medium";
        case 3: return "Low";
        default: return "Unknown";
    }
}

ImVec4 AggregatedTrackerView::GetStatusColor(const std::string& status, const std::string& folderType, UFB::ProjectConfig* config)
{
    if (config && config->IsLoaded() && !folderType.empty())
    {
        // Get color from the specific folder type
        auto colorOpt = config->GetStatusColor(folderType, status);
        if (colorOpt.has_value())
        {
            return HexToImVec4(colorOpt.value());
        }
    }

    // Fallback color coding
    if (status == "Complete" || status == "Done")
        return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);  // Green
    else if (status == "In Progress" || status == "WIP")
        return ImVec4(0.2f, 0.6f, 0.9f, 1.0f);  // Blue
    else if (status == "Blocked" || status == "On Hold")
        return ImVec4(0.9f, 0.5f, 0.2f, 1.0f);  // Orange
    else if (status == "Not Started")
        return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);  // Gray
    else
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White (default)
}

ImVec4 AggregatedTrackerView::GetCategoryColor(const std::string& category, const std::string& folderType, UFB::ProjectConfig* config)
{
    if (config && config->IsLoaded() && !folderType.empty())
    {
        // Get color from the specific folder type
        auto colorOpt = config->GetCategoryColor(folderType, category);
        if (colorOpt.has_value())
        {
            return HexToImVec4(colorOpt.value());
        }
    }

    // Fallback default color
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

ImVec4 AggregatedTrackerView::GetPriorityColor(int priority)
{
    switch (priority)
    {
        case 1: return ImVec4(0.9f, 0.2f, 0.2f, 1.0f);  // Red (High)
        case 2: return ImVec4(0.9f, 0.7f, 0.2f, 1.0f);  // Yellow (Medium)
        case 3: return ImVec4(0.2f, 0.7f, 0.9f, 1.0f);  // Blue (Low)
        default: return ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // White (Unknown)
    }
}

std::string AggregatedTrackerView::FormatDate(uint64_t timestamp)
{
    if (timestamp == 0)
        return "-";

    std::time_t time = static_cast<std::time_t>(timestamp / 1000);
    std::tm tm;
    localtime_s(&tm, &time);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d");
    return oss.str();
}

ImVec4 AggregatedTrackerView::GetTypeColor(const std::string& itemType)
{
    if (itemType == "shot")
        return ImVec4(0.2f, 0.6f, 1.0f, 1.0f);  // Blue
    else if (itemType == "asset")
        return ImVec4(0.7f, 0.4f, 1.0f, 1.0f);  // Purple
    else if (itemType == "posting")
        return ImVec4(0.3f, 0.8f, 0.3f, 1.0f);  // Green
    else if (itemType == "manual_task")
        return ImVec4(1.0f, 0.6f, 0.2f, 1.0f);  // Orange

    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);  // Gray (fallback)
}

void AggregatedTrackerView::CollectAvailableFilterValues()
{
    m_availableProjects.clear();
    m_availableArtists.clear();
    m_availablePriorities.clear();

    // Collect from all loaded items
    for (const auto& item : m_allItems)
    {
        // Collect project names
        if (!item.jobName.empty())
            m_availableProjects.insert(item.jobName);

        // Collect artists
        if (!item.metadata.artist.empty())
            m_availableArtists.insert(item.metadata.artist);

        // Collect priorities
        if (item.metadata.priority > 0)
            m_availablePriorities.insert(item.metadata.priority);
    }
}

UFB::ProjectConfig* AggregatedTrackerView::GetConfigForItem(const TrackedItemWithProject& item)
{
    // Check if we've already loaded this project's config
    auto it = m_projectConfigs.find(item.jobPath);
    if (it != m_projectConfigs.end())
    {
        return it->second.get();
    }

    // Load and cache the config for this project
    auto config = std::make_unique<UFB::ProjectConfig>();
    bool loaded = config->LoadProjectConfig(item.jobPath);

    if (!loaded)
    {
        // Fallback to global template if project config not found
        config->LoadGlobalTemplate();
    }

    // Store in cache and return pointer
    auto* ptr = config.get();
    m_projectConfigs[item.jobPath] = std::move(config);
    return ptr;
}

bool AggregatedTrackerView::PassesFilters(const TrackedItemWithProject& item)
{
    const auto& metadata = item.metadata;

    // Only show tracked items
    if (!metadata.isTracked)
        return false;

    // Ensure item has a valid path
    if (metadata.shotPath.empty())
        return false;

    // Filter out items whose path is exactly the job path (ghost entries)
    if (metadata.shotPath == item.jobPath)
        return false;

    // For manual tasks, ensure the path contains the task marker
    if (metadata.itemType == "manual_task")
    {
        if (metadata.shotPath.find(L"/__task_") == std::wstring::npos)
            return false;
    }
    else
    {
        // For regular items (shot/asset/posting), ensure path is longer than job path
        if (metadata.shotPath.length() <= item.jobPath.length() + 1)
            return false;
    }

    // Ensure item has a valid type
    if (metadata.itemType.empty())
        return false;

    // Ensure item type is valid
    if (metadata.itemType != "shot" && metadata.itemType != "asset" &&
        metadata.itemType != "posting" && metadata.itemType != "manual_task")
        return false;

    // Filter by project
    if (!m_filterProjects.empty())
    {
        if (m_filterProjects.find(item.jobName) == m_filterProjects.end())
            return false;
    }

    // Filter by item type
    if (!m_filterTypes.empty())
    {
        if (m_filterTypes.find(metadata.itemType) == m_filterTypes.end())
            return false;
    }

    // Filter by artist
    if (!m_filterArtists.empty())
    {
        if (m_filterArtists.find(metadata.artist) == m_filterArtists.end())
            return false;
    }

    // Filter by priority
    if (!m_filterPriorities.empty())
    {
        if (m_filterPriorities.find(metadata.priority) == m_filterPriorities.end())
            return false;
    }

    // Filter by due date
    if (m_filterDueDate > 0 && metadata.dueDate > 0)
    {
        auto now = std::chrono::system_clock::now();
        auto nowTime = std::chrono::system_clock::to_time_t(now);

        // Convert due date timestamp (milliseconds) to time_t
        std::time_t dueDateTime = static_cast<std::time_t>(metadata.dueDate / 1000);

        // Calculate difference in days
        double diffDays = std::difftime(dueDateTime, nowTime) / 86400.0;

        bool passes = false;
        switch (m_filterDueDate)
        {
            case 1: // Overdue
                passes = (diffDays < 0);
                break;
            case 2: // Today
                passes = (diffDays >= 0 && diffDays <= 1);
                break;
            case 3: // This Week
                passes = (diffDays >= 0 && diffDays <= 7);
                break;
            case 4: // This Month
                passes = (diffDays >= 0 && diffDays <= 30);
                break;
            default:
                passes = true;
                break;
        }

        if (!passes)
            return false;
    }

    return true;
}

void AggregatedTrackerView::UpdateUnifiedItemsList()
{
    // CRITICAL: Don't modify m_allItems while it's being iterated in DrawUnifiedTable()
    if (m_isRendering)
        return;

    // Store current items temporarily
    std::vector<TrackedItemWithProject> allLoadedItems = m_allItems;

    // Clear and rebuild with filtered items
    m_allItems.clear();
    std::set<int> addedIds;  // Track IDs to prevent duplicates

    for (const auto& item : allLoadedItems)
    {
        if (PassesFilters(item))
        {
            // Only add if we haven't seen this ID before
            if (addedIds.find(item.metadata.id) == addedIds.end())
            {
                m_allItems.push_back(item);
                addedIds.insert(item.metadata.id);
            }
        }
    }

    // Reset selection if out of bounds
    if (m_selectedItemIndex >= static_cast<int>(m_allItems.size()))
    {
        m_selectedItemIndex = -1;
    }
}

void AggregatedTrackerView::DrawUnifiedTable()
{
    // CRITICAL: Set rendering flag to prevent m_allItems modification during iteration
    m_isRendering = true;

    // Empty state
    if (m_allItems.empty())
    {
        m_isRendering = false;
        ImGui::TextDisabled("No tracked items found (or all filtered out)");
        return;
    }

    // Create table with 11 columns (added Project column)
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    // Push larger cell padding for taller rows (matching shot_view)
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 8.0f));

    if (ImGui::BeginTable("##AggregatedTrackerTable", 11, flags))
    {
        // Setup columns (widths matching shot_view, with Project column added at index 1)
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f, 0);
        ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthFixed, 150.0f, 1);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 0.0f, 2);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 140.0f, 3);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 150.0f, 4);
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 110.0f, 5);
        ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthFixed, 150.0f, 6);
        ImGui::TableSetupColumn("Due Date", ImGuiTableColumnFlags_WidthFixed, 110.0f, 7);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 80.0f, 8);
        ImGui::TableSetupColumn("Links", ImGuiTableColumnFlags_WidthFixed, 120.0f, 9);
        ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthFixed, 250.0f, 10);
        ImGui::TableSetupScrollFreeze(0, 1);

        // Handle sorting
        if (ImGuiTableSortSpecs* sortsSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortsSpecs->SpecsDirty && sortsSpecs->SpecsCount > 0)
            {
                m_allItemsSortColumn = sortsSpecs->Specs[0].ColumnIndex;
                m_allItemsSortAscending = (sortsSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                SortItems(m_allItems, m_allItemsSortColumn, m_allItemsSortAscending);
                sortsSpecs->SpecsDirty = false;
            }
        }

        ImGui::TableHeadersRow();

        // Draw rows
        for (int i = 0; i < static_cast<int>(m_allItems.size()); ++i)
        {
            auto& item = m_allItems[i];
            auto& metadata = item.metadata;

            // Set minimum row height (matching shot_view)
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 35.0f);

            ImGui::PushID(i);

            // Column 0: Type (with color badge)
            ImGui::TableSetColumnIndex(0);
            ImVec4 typeColor = GetTypeColor(metadata.itemType);
            ImGui::PushStyleColor(ImGuiCol_Text, typeColor);

            std::string typeLabel = "Unknown";
            if (metadata.itemType == "shot") typeLabel = "Shot";
            else if (metadata.itemType == "asset") typeLabel = "Asset";
            else if (metadata.itemType == "posting") typeLabel = "Posting";
            else if (metadata.itemType == "manual_task") typeLabel = "Task";

            ImGui::TextUnformatted(typeLabel.c_str());
            ImGui::PopStyleColor();

            // Column 1: Project
            ImGui::TableSetColumnIndex(1);
            char projectUtf8[256];
            WideCharToMultiByte(CP_UTF8, 0, item.jobName.c_str(), -1, projectUtf8, sizeof(projectUtf8), nullptr, nullptr);
            ImGui::TextUnformatted(projectUtf8);

            // Column 2: Path
            ImGui::TableSetColumnIndex(2);

            std::wstring displayPath = metadata.shotPath;
            if (metadata.itemType == "manual_task")
            {
                // Extract task name from path: jobPath/__task_TaskName
                size_t taskMarkerPos = metadata.shotPath.find(L"/__task_");
                if (taskMarkerPos != std::wstring::npos)
                {
                    displayPath = metadata.shotPath.substr(taskMarkerPos + 8);  // Skip "/__task_"
                }
            }
            else
            {
                // Remove job path prefix
                if (displayPath.find(item.jobPath) == 0)
                {
                    displayPath = displayPath.substr(item.jobPath.length());
                    if (!displayPath.empty() && (displayPath[0] == L'\\' || displayPath[0] == L'/'))
                        displayPath = displayPath.substr(1);
                }
            }

            char pathUtf8[4096];
            WideCharToMultiByte(CP_UTF8, 0, displayPath.c_str(), -1, pathUtf8, sizeof(pathUtf8), nullptr, nullptr);

            bool isSelected = (i == m_selectedItemIndex);

            // Use accent color for selected items (matching shot_view)
            ImVec4 accentColor = GetWindowsAccentColor();
            accentColor.w = 0.3f;  // Set transparency
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, accentColor);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accentColor.x * 1.1f, accentColor.y * 1.1f, accentColor.z * 1.1f, accentColor.w));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accentColor.x * 1.2f, accentColor.y * 1.2f, accentColor.z * 1.2f, accentColor.w));
            }

            // Use explicit height to match row height (35.0f)
            if (ImGui::Selectable(pathUtf8, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 35.0f)))
            {
                m_selectedItemIndex = i;

                // Double-click to open
                if (ImGui::IsMouseDoubleClicked(0))
                {
                    if (metadata.itemType == "shot" && onOpenShot)
                        onOpenShot(metadata.shotPath);
                    else if (metadata.itemType == "asset" && onOpenAsset)
                        onOpenAsset(metadata.shotPath);
                    else if (metadata.itemType == "posting" && onOpenPosting)
                        onOpenPosting(metadata.shotPath);
                }
            }

            if (isSelected)
            {
                ImGui::PopStyleColor(3);  // Pop Header, HeaderHovered, HeaderActive
            }

            // Context menu
            if (ImGui::BeginPopupContextItem())
            {
                if (metadata.itemType != "manual_task")
                {
                    if (ImGui::MenuItem("Open in Project View"))
                    {
                        if (metadata.itemType == "shot" && onOpenShot)
                            onOpenShot(metadata.shotPath);
                        else if (metadata.itemType == "asset" && onOpenAsset)
                            onOpenAsset(metadata.shotPath);
                        else if (metadata.itemType == "posting" && onOpenPosting)
                            onOpenPosting(metadata.shotPath);
                    }

                    ImGui::Separator();

                    // Reveal in Explorer
                    if (ImGui::MenuItem("Reveal in Explorer"))
                    {
                        std::wstring command = L"/select,\"" + metadata.shotPath + L"\"";
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOW);
                    }

                    // Open in New Window
                    if (onOpenInNewWindow)
                    {
                        if (ImGui::MenuItem("Open in New Window"))
                        {
                            onOpenInNewWindow(metadata.shotPath);
                        }
                    }

                    // Open in Browser 1 and Browser 2
                    if (onOpenInBrowser1)
                    {
                        if (ImGui::MenuItem("Open in the Left Browser"))
                        {
                            onOpenInBrowser1(metadata.shotPath);
                        }
                    }

                    if (onOpenInBrowser2)
                    {
                        if (ImGui::MenuItem("Open in the Right Browser"))
                        {
                            onOpenInBrowser2(metadata.shotPath);
                        }
                    }

                    ImGui::Separator();
                }

                // Open project tracker for this item's project
                if (onOpenProjectTracker)
                {
                    if (ImGui::MenuItem("Open Project Tracker"))
                    {
                        onOpenProjectTracker(item.jobPath, item.jobName);
                    }

                    ImGui::Separator();
                }

                if (ImGui::MenuItem("Un-track"))
                {
                    metadata.isTracked = false;
                    if (m_subscriptionManager)
                    {
                        m_subscriptionManager->CreateOrUpdateShotMetadata(metadata);
                    }
                    RefreshTrackedItems();
                }

                if (metadata.itemType == "manual_task")
                {
                    if (ImGui::MenuItem("Delete Task"))
                    {
                        if (m_subscriptionManager)
                        {
                            m_subscriptionManager->DeleteShotMetadata(metadata.shotPath);
                        }
                        RefreshTrackedItems();
                    }
                }

                ImGui::EndPopup();
            }

            // Push mono font for all metadata columns
            if (font_mono)
                ImGui::PushFont(font_mono);

            // Column 3: Status (editable dropdown)
            ImGui::TableSetColumnIndex(3);

            // Get config for this item's project
            UFB::ProjectConfig* config = GetConfigForItem(item);

            // Get status options from config
            std::vector<UFB::StatusOption> statusOptions;
            if (config && config->IsLoaded() && !metadata.folderType.empty())
            {
                statusOptions = config->GetStatusOptions(metadata.folderType);
            }

            std::string displayStatus = metadata.status.empty() ? "Not Set" : metadata.status;
            ImVec4 statusColor = metadata.status.empty() ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : GetStatusColor(metadata.status, metadata.folderType, config);
            ImGui::PushStyleColor(ImGuiCol_Text, statusColor);

            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(("##status" + std::to_string(i)).c_str(), displayStatus.c_str()))
            {
                if (statusOptions.empty())
                {
                    ImGui::TextDisabled("(No options configured)");
                }
                else
                {
                    for (const auto& statusOpt : statusOptions)
                    {
                        bool isSelected = (metadata.status == statusOpt.name);
                        ImVec4 optionColor = GetStatusColor(statusOpt.name, metadata.folderType, config);
                        ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                        if (ImGui::Selectable(statusOpt.name.c_str(), isSelected))
                        {
                            metadata.status = statusOpt.name;
                            if (m_subscriptionManager)
                            {
                                m_subscriptionManager->CreateOrUpdateShotMetadata(metadata);
                            }
                        }

                        ImGui::PopStyleColor();

                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleColor();

            // Column 4: Category (editable dropdown)
            ImGui::TableSetColumnIndex(4);

            // Get category options from config
            std::vector<UFB::CategoryOption> categoryOptions;
            if (config && config->IsLoaded() && !metadata.folderType.empty())
            {
                categoryOptions = config->GetCategoryOptions(metadata.folderType);
            }

            std::string displayCategory = metadata.category.empty() ? "Not Set" : metadata.category;
            ImVec4 categoryColor = metadata.category.empty() ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : GetCategoryColor(metadata.category, metadata.folderType, config);
            ImGui::PushStyleColor(ImGuiCol_Text, categoryColor);

            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(("##category" + std::to_string(i)).c_str(), displayCategory.c_str()))
            {
                if (categoryOptions.empty())
                {
                    ImGui::TextDisabled("(No options configured)");
                }
                else
                {
                    for (const auto& catOpt : categoryOptions)
                    {
                        bool isSelected = (metadata.category == catOpt.name);
                        ImVec4 optionColor = GetCategoryColor(catOpt.name, metadata.folderType, config);
                        ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                        if (ImGui::Selectable(catOpt.name.c_str(), isSelected))
                        {
                            metadata.category = catOpt.name;
                            if (m_subscriptionManager)
                            {
                                m_subscriptionManager->CreateOrUpdateShotMetadata(metadata);
                            }
                        }

                        ImGui::PopStyleColor();

                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleColor();

            // Column 5: Priority (editable dropdown)
            ImGui::TableSetColumnIndex(5);

            const char* priorityLabel = GetPriorityLabel(metadata.priority);
            ImVec4 priorityColor = GetPriorityColor(metadata.priority);
            ImGui::PushStyleColor(ImGuiCol_Text, priorityColor);

            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(("##priority" + std::to_string(i)).c_str(), priorityLabel))
            {
                // High Priority
                bool isHighSelected = (metadata.priority == 1);
                ImVec4 highColor = GetPriorityColor(1);
                ImGui::PushStyleColor(ImGuiCol_Text, highColor);
                if (ImGui::Selectable("High", isHighSelected))
                {
                    metadata.priority = 1;
                    if (m_subscriptionManager)
                    {
                        m_subscriptionManager->CreateOrUpdateShotMetadata(metadata);
                    }
                }
                ImGui::PopStyleColor();
                if (isHighSelected)
                    ImGui::SetItemDefaultFocus();

                // Medium Priority
                bool isMediumSelected = (metadata.priority == 2);
                ImVec4 mediumColor = GetPriorityColor(2);
                ImGui::PushStyleColor(ImGuiCol_Text, mediumColor);
                if (ImGui::Selectable("Medium", isMediumSelected))
                {
                    metadata.priority = 2;
                    if (m_subscriptionManager)
                    {
                        m_subscriptionManager->CreateOrUpdateShotMetadata(metadata);
                    }
                }
                ImGui::PopStyleColor();
                if (isMediumSelected)
                    ImGui::SetItemDefaultFocus();

                // Low Priority
                bool isLowSelected = (metadata.priority == 3);
                ImVec4 lowColor = GetPriorityColor(3);
                ImGui::PushStyleColor(ImGuiCol_Text, lowColor);
                if (ImGui::Selectable("Low", isLowSelected))
                {
                    metadata.priority = 3;
                    if (m_subscriptionManager)
                    {
                        m_subscriptionManager->CreateOrUpdateShotMetadata(metadata);
                    }
                }
                ImGui::PopStyleColor();
                if (isLowSelected)
                    ImGui::SetItemDefaultFocus();

                ImGui::EndCombo();
            }
            ImGui::PopStyleColor();

            // Column 6: Artist (editable dropdown)
            ImGui::TableSetColumnIndex(6);

            // Get users list from config
            std::vector<UFB::User> users;
            if (config && config->IsLoaded())
            {
                users = config->GetUsers();
            }

            std::string displayArtist = metadata.artist.empty() ? "Not Set" : metadata.artist;

            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo(("##artist" + std::to_string(i)).c_str(), displayArtist.c_str()))
            {
                // "Not Set" option to clear the artist
                bool isNotSetSelected = metadata.artist.empty();
                if (ImGui::Selectable("Not Set", isNotSetSelected))
                {
                    metadata.artist = "";
                    if (m_subscriptionManager)
                    {
                        m_subscriptionManager->CreateOrUpdateShotMetadata(metadata);
                    }
                }
                if (isNotSetSelected)
                    ImGui::SetItemDefaultFocus();

                if (!users.empty())
                {
                    ImGui::Separator();

                    for (const auto& user : users)
                    {
                        bool isSelected = (metadata.artist == user.displayName);
                        if (ImGui::Selectable(user.displayName.c_str(), isSelected))
                        {
                            metadata.artist = user.displayName;
                            if (m_subscriptionManager)
                            {
                                m_subscriptionManager->CreateOrUpdateShotMetadata(metadata);
                            }
                        }

                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                }
                else
                {
                    ImGui::TextDisabled("(No users configured)");
                }

                ImGui::EndCombo();
            }

            // Column 7: Due Date (clickable button like shot_view)
            ImGui::TableSetColumnIndex(7);

            std::string dueDateStr = (metadata.dueDate > 0) ? FormatDate(metadata.dueDate) : "Not Set";
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Button((dueDateStr + "##duedate" + std::to_string(i)).c_str(), ImVec2(-FLT_MIN, 0)))
            {
                m_showAllItemsDatePicker = true;
                m_allItemsDatePickerIndex = i;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Click to select date");

            // Column 8: Modified Date (read-only, disabled text color)
            ImGui::TableSetColumnIndex(8);

            std::string modifiedDateStr = (metadata.modifiedTime > 0) ? FormatDate(metadata.modifiedTime) : "-";
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(modifiedDateStr.c_str());
            ImGui::PopStyleColor();

            // Column 9: Links (clickable, similar to notes)
            ImGui::TableSetColumnIndex(9);

            // Pop mono font before Links/Notes columns
            if (font_mono)
                ImGui::PopFont();

            // Check if link exists (links field is a string, not empty means we have a link)
            bool hasLink = !metadata.links.empty();

            if (hasLink)
            {
                // 50/50 split for Edit and Link buttons (120px column / 2 = ~55px each with spacing)
                float buttonWidth = 55.0f;

                // Show "Edit" button (left side)
                if (ImGui::Button(("Edit##linkedit" + std::to_string(i)).c_str(), ImVec2(buttonWidth, 0)))
                {
                    m_showLinkEditor = true;
                    m_linkEditorItemIndex = i;
                    strncpy_s(m_linkEditorBuffer, sizeof(m_linkEditorBuffer), metadata.links.c_str(), sizeof(m_linkEditorBuffer) - 1);
                    m_linkEditorBuffer[sizeof(m_linkEditorBuffer) - 1] = '\0';
                }

                ImGui::SameLine();

                // Show "Link" button in accent color (right side)
                ImVec4 accentColor = GetWindowsAccentColor();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));  // Transparent background
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_Text, accentColor);

                if (ImGui::Button(("Link##linkopen" + std::to_string(i)).c_str(), ImVec2(buttonWidth, 0)))
                {
                    // Open link in default browser
                    std::wstring wideLink(metadata.links.begin(), metadata.links.end());
                    ShellExecuteW(nullptr, L"open", wideLink.c_str(), nullptr, nullptr, SW_SHOW);
                }

                ImGui::PopStyleColor(4);

                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("Click to open: %s", metadata.links.c_str());
                    ImGui::EndTooltip();
                }
            }
            else
            {
                // Show "(click to add link)" in disabled color
                ImVec4 textColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, textColor);

                if (ImGui::Selectable(("(click to add link)##linkadd" + std::to_string(i)).c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 0)))
                {
                    m_showLinkEditor = true;
                    m_linkEditorItemIndex = i;
                    m_linkEditorBuffer[0] = '\0';
                }

                ImGui::PopStyleColor();
            }

            // Column 10: Notes (clickable text with tooltip)
            ImGui::TableSetColumnIndex(10);

            // Create a one-line preview
            std::string notePreview = metadata.note.empty() ? "(click to add note)" : metadata.note;

            // Truncate to first line only
            size_t newlinePos = notePreview.find('\n');
            if (newlinePos != std::string::npos)
            {
                notePreview = notePreview.substr(0, newlinePos) + "...";
            }

            // Truncate if too long (max 50 chars for preview)
            if (notePreview.length() > 50)
            {
                notePreview = notePreview.substr(0, 47) + "...";
            }

            // Make it selectable to detect clicks, fill the full cell height
            // Adjust height to account for cell padding (8.0f top + 8.0f bottom = 16.0f)
            std::string selectableId = "##note_preview_" + std::to_string(i);
            ImVec4 textColor = metadata.note.empty() ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, textColor);

            // Disable frame padding for this Selectable to fill the cell properly
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 0.0f));
            if (ImGui::Selectable(notePreview.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 0)))
            {
                // Open note editor modal
                m_showNoteEditor = true;
                m_noteEditorItemIndex = i;
                strncpy_s(m_noteEditorBuffer, sizeof(m_noteEditorBuffer), metadata.note.c_str(), sizeof(m_noteEditorBuffer) - 1);
                m_noteEditorBuffer[sizeof(m_noteEditorBuffer) - 1] = '\0';
            }
            ImGui::PopStyleVar();

            ImGui::PopStyleColor();

            // Show full note on hover
            if (ImGui::IsItemHovered() && !metadata.note.empty())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(400.0f);
                ImGui::TextUnformatted(metadata.note.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // Pop cell padding style var
    ImGui::PopStyleVar();

    // Clear rendering flag
    m_isRendering = false;
}
