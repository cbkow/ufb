#include "project_tracker_view.h"
#include "subscription_manager.h"
#include "project_config.h"
#include "utils.h"
#include "ImGuiDatePicker.hpp"
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

ProjectTrackerView::ProjectTrackerView()
{
}

ProjectTrackerView::~ProjectTrackerView()
{
    Shutdown();
}

void ProjectTrackerView::Initialize(const std::wstring& jobPath, const std::wstring& jobName,
                                    UFB::SubscriptionManager* subscriptionManager, UFB::ProjectConfig* projectConfig)
{
    m_jobPath = jobPath;
    m_jobName = jobName;
    m_subscriptionManager = subscriptionManager;

    // Load or create ProjectConfig for this job
    m_projectConfig = new UFB::ProjectConfig();
    bool configLoaded = m_projectConfig->LoadProjectConfig(jobPath);

    if (!configLoaded)
    {
        // Try loading global template as fallback
        m_projectConfig->LoadGlobalTemplate();
    }

    // Load tracked items
    RefreshTrackedItems();
}

void ProjectTrackerView::Shutdown()
{
    m_trackedShots.clear();
    m_trackedAssets.clear();
    m_trackedPostings.clear();
    m_manualTasks.clear();

    if (m_projectConfig)
    {
        delete m_projectConfig;
        m_projectConfig = nullptr;
    }
}

void ProjectTrackerView::RefreshTrackedItems()
{
    if (!m_subscriptionManager)
        return;

    // Load tracked items by type
    m_trackedShots = m_subscriptionManager->GetTrackedItems(m_jobPath, "shot");
    m_trackedAssets = m_subscriptionManager->GetTrackedItems(m_jobPath, "asset");
    m_trackedPostings = m_subscriptionManager->GetTrackedItems(m_jobPath, "posting");
    m_manualTasks = m_subscriptionManager->GetTrackedItems(m_jobPath, "manual_task");
}

void ProjectTrackerView::Draw(const char* title, HWND hwnd)
{
    // Use close button and check if window was closed
    bool windowOpen = ImGui::Begin(title, &m_isOpen, ImGuiWindowFlags_None);

    // If window was closed, trigger onClose callback
    if (!m_isOpen && onClose)
    {
        onClose();
        ImGui::End();
        return;
    }

    if (!windowOpen)
    {
        ImGui::End();
        return;
    }

    // Header with job name and refresh button (flush right)
    ImGui::Text("Project: %ls", m_jobName.c_str());

    // Refresh button flush right
    float buttonWidth = 30.0f;  // Icon button width
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttonWidth + ImGui::GetCursorPosX());

    if (font_icons)
        ImGui::PushFont(font_icons);

    if (ImGui::Button(U8("\uE5D5")))  // Material Icons refresh symbol
    {
        RefreshTrackedItems();
    }

    if (font_icons)
        ImGui::PopFont();

    ImGui::Separator();

    // Draw tables in collapsing headers (Manual Tasks first)
    if (ImGui::CollapsingHeader("Manual Tasks", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawManualTasksTable();
    }

    if (ImGui::CollapsingHeader("Shots", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawItemsTable("ShotsTable", "shots", m_trackedShots, m_selectedShotIndex, m_shotsSortColumn, m_shotsSortAscending,
                      m_showShotDatePicker, m_shotDatePickerIndex);
    }

    if (ImGui::CollapsingHeader("Assets", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawItemsTable("AssetsTable", "assets", m_trackedAssets, m_selectedAssetIndex, m_assetsSortColumn, m_assetsSortAscending,
                      m_showAssetDatePicker, m_assetDatePickerIndex);
    }

    if (ImGui::CollapsingHeader("Postings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawItemsTable("PostingsTable", "postings", m_trackedPostings, m_selectedPostingIndex, m_postingsSortColumn, m_postingsSortAscending,
                      m_showPostingDatePicker, m_postingDatePickerIndex);
    }

    ImGui::End();

    // Add Task Dialog
    if (m_showAddTaskDialog)
    {
        ImGui::OpenPopup("Add Manual Task");
        m_showAddTaskDialog = false;
    }

    if (ImGui::BeginPopupModal("Add Manual Task", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Create a new manual task");
        ImGui::Separator();

        ImGui::InputText("Task Name", m_taskNameBuffer, sizeof(m_taskNameBuffer));
        ImGui::InputTextMultiline("Notes", m_taskNoteBuffer, sizeof(m_taskNoteBuffer), ImVec2(400, 100));

        ImGui::Separator();

        if (ImGui::Button("Create", ImVec2(120, 0)))
        {
            if (strlen(m_taskNameBuffer) > 0 && m_subscriptionManager)
            {
                // Create new manual task
                UFB::ShotMetadata taskMeta;
                taskMeta.note = m_taskNoteBuffer;
                taskMeta.priority = 2;  // Medium priority

                m_subscriptionManager->CreateManualTask(m_jobPath, m_taskNameBuffer, taskMeta);

                // Clear buffers and refresh
                m_taskNameBuffer[0] = '\0';
                m_taskNoteBuffer[0] = '\0';
                RefreshTrackedItems();

                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_taskNameBuffer[0] = '\0';
            m_taskNoteBuffer[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // Note Editor Modal
    if (m_showNoteEditor)
    {
        ImGui::OpenPopup("Edit Note");
        m_showNoteEditor = false;  // Only open once
    }

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
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
            if (m_noteEditorItemList && m_noteEditorItemIndex >= 0 && m_noteEditorItemIndex < m_noteEditorItemList->size())
            {
                (*m_noteEditorItemList)[m_noteEditorItemIndex].note = m_noteEditorBuffer;
                if (m_subscriptionManager)
                {
                    m_subscriptionManager->CreateOrUpdateShotMetadata((*m_noteEditorItemList)[m_noteEditorItemIndex]);
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

void ProjectTrackerView::DrawItemsTable(const char* tableName, const char* itemType, std::vector<UFB::ShotMetadata>& items,
                                       int& selectedIndex, int& sortColumn, bool& sortAscending,
                                       bool& showDatePicker, int& datePickerIndex)
{
    if (items.empty())
    {
        ImGui::TextDisabled("No tracked items");
        return;
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;

    // Calculate table height: header + all rows (no scrolling)
    float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    float tableHeight = rowHeight * (items.size() + 1); // +1 for header

    if (ImGui::BeginTable(tableName, 7, flags, ImVec2(0, tableHeight)))
    {
        // Setup columns
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 120.0f, 1);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 120.0f, 2);
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 100.0f, 3);
        ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthFixed, 120.0f, 4);
        ImGui::TableSetupColumn("Due Date", ImGuiTableColumnFlags_WidthFixed, 120.0f, 5);
        ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch, 0.0f, 6);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty)
            {
                if (sortSpecs->SpecsCount > 0)
                {
                    sortColumn = sortSpecs->Specs[0].ColumnUserID;
                    sortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                    SortItems(items, sortColumn, sortAscending);
                }
                sortSpecs->SpecsDirty = false;
            }
        }

        // Get users list (same for all items)
        std::vector<UFB::User> users;
        if (m_projectConfig && m_projectConfig->IsLoaded())
        {
            users = m_projectConfig->GetUsers();
        }

        // Match Shot View styling: larger cell padding for taller rows
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 8.0f));

        // Draw rows
        for (int i = 0; i < items.size(); i++)
        {
            auto& item = items[i];
            bool metadataChanged = false;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            ImGui::PushID(i);

            bool isSelected = (i == selectedIndex);

            // Path column (selectable - only in this column, not spanning all columns)
            // Use regular font for shot name
            if (font_regular)
                ImGui::PushFont(font_regular);

            std::wstring displayPath = item.shotPath;

            // For manual tasks, clean up the display
            if (item.itemType == "manual_task")
            {
                // Extract task name from path (format: jobPath/__task_TaskName)
                size_t taskPos = displayPath.find(L"/__task_");
                if (taskPos != std::wstring::npos)
                {
                    displayPath = displayPath.substr(taskPos + 8);  // Skip "/__task_"
                }
            }
            else
            {
                // For regular items, remove job path prefix
                if (displayPath.find(m_jobPath) == 0)
                {
                    displayPath = displayPath.substr(m_jobPath.length());
                    if (!displayPath.empty() && displayPath[0] == L'\\')
                        displayPath = displayPath.substr(1);
                }
            }

            char pathUtf8[512];
            WideCharToMultiByte(CP_UTF8, 0, displayPath.c_str(), -1, pathUtf8, sizeof(pathUtf8), nullptr, nullptr);

            // Use accent color for selected items (matching Shot View)
            ImVec4 accentColor = GetWindowsAccentColor();
            accentColor.w = 0.3f;  // Set transparency
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, accentColor);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accentColor.x * 1.1f, accentColor.y * 1.1f, accentColor.z * 1.1f, accentColor.w));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accentColor.x * 1.2f, accentColor.y * 1.2f, accentColor.z * 1.2f, accentColor.w));
            }

            // SpanAllColumns makes the highlight span the entire row, AllowOverlap allows clicking other columns
            // Use explicit height to match Shot View row height (35.0f)
            if (ImGui::Selectable(pathUtf8, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 35.0f)))
            {
                selectedIndex = i;
            }

            // Right-click context menu on the name column
            std::string contextMenuId = "row_context_" + std::to_string(i);
            if (ImGui::BeginPopupContextItem(contextMenuId.c_str()))
            {
                if (item.itemType == "manual_task")
                {
                    // For manual tasks: Delete option
                    if (ImGui::MenuItem("Delete Task"))
                    {
                        if (m_subscriptionManager && m_subscriptionManager->DeleteManualTask(item.id))
                        {
                            selectedIndex = -1;
                            RefreshTrackedItems();
                        }
                    }
                }
                else
                {
                    // For shots/assets/postings: Open and Un-track options
                    const char* openLabel = (item.itemType == "shot") ? "Open Shot" :
                                           (item.itemType == "asset") ? "Open Asset" : "Open Posting";

                    if (ImGui::MenuItem(openLabel))
                    {
                        // Call the appropriate callback based on item type
                        if (item.itemType == "shot" && onOpenShot)
                        {
                            onOpenShot(item.shotPath);
                        }
                        else if (item.itemType == "asset" && onOpenAsset)
                        {
                            onOpenAsset(item.shotPath);
                        }
                        else if (item.itemType == "posting" && onOpenPosting)
                        {
                            onOpenPosting(item.shotPath);
                        }
                    }

                    if (ImGui::MenuItem("Un-track"))
                    {
                        item.isTracked = false;
                        if (m_subscriptionManager)
                        {
                            m_subscriptionManager->CreateOrUpdateShotMetadata(item);
                            RefreshTrackedItems();
                        }
                    }
                }
                ImGui::EndPopup();
            }

            if (isSelected)
            {
                ImGui::PopStyleColor(3);  // Pop Header, HeaderHovered, HeaderActive
            }

            if (font_regular)
                ImGui::PopFont();

            // Push mono font for all other columns
            if (font_mono)
                ImGui::PushFont(font_mono);

            // Status column (editable)
            ImGui::TableNextColumn();
            {
                // Get status options for this specific item's folderType
                std::vector<UFB::StatusOption> statusOptions;
                if (m_projectConfig && m_projectConfig->IsLoaded() && !item.folderType.empty())
                {
                    statusOptions = m_projectConfig->GetStatusOptions(item.folderType);
                }

                std::string comboId = "##status_" + std::to_string(i);
                const char* currentStatus = statusOptions.empty() ? "(No options)" :
                                           (item.status.empty() ? statusOptions[0].name.c_str() : item.status.c_str());

                ImVec4 statusColor = GetStatusColor(item.status, item.folderType);
                ImGui::PushStyleColor(ImGuiCol_Text, statusColor);

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(comboId.c_str(), currentStatus, ImGuiComboFlags_HeightLarge))
                {
                    for (const auto& statusOption : statusOptions)
                    {
                        ImVec4 optionColor = GetStatusColor(statusOption.name, item.folderType);
                        ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                        bool isStatusSelected = (item.status == statusOption.name);
                        if (ImGui::Selectable(statusOption.name.c_str(), isStatusSelected))
                        {
                            item.status = statusOption.name;
                            metadataChanged = true;
                        }
                        if (isStatusSelected)
                            ImGui::SetItemDefaultFocus();

                        ImGui::PopStyleColor();
                    }
                    ImGui::EndCombo();
                }

                ImGui::PopStyleColor();
            }

            // Category column (editable)
            ImGui::TableNextColumn();
            {
                // Get category options for this specific item's folderType
                std::vector<UFB::CategoryOption> categoryOptions;
                if (m_projectConfig && m_projectConfig->IsLoaded() && !item.folderType.empty())
                {
                    categoryOptions = m_projectConfig->GetCategoryOptions(item.folderType);
                }

                std::string comboId = "##category_" + std::to_string(i);
                const char* currentCategory = categoryOptions.empty() ? "(No options)" :
                                             (item.category.empty() ? categoryOptions[0].name.c_str() : item.category.c_str());

                ImVec4 categoryColor = GetCategoryColor(item.category, item.folderType);
                ImGui::PushStyleColor(ImGuiCol_Text, categoryColor);

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(comboId.c_str(), currentCategory, ImGuiComboFlags_HeightLarge))
                {
                    for (const auto& categoryOption : categoryOptions)
                    {
                        ImVec4 optionColor = GetCategoryColor(categoryOption.name, item.folderType);
                        ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                        bool isCategorySelected = (item.category == categoryOption.name);
                        if (ImGui::Selectable(categoryOption.name.c_str(), isCategorySelected))
                        {
                            item.category = categoryOption.name;
                            metadataChanged = true;
                        }
                        if (isCategorySelected)
                            ImGui::SetItemDefaultFocus();

                        ImGui::PopStyleColor();
                    }
                    ImGui::EndCombo();
                }

                ImGui::PopStyleColor();
            }

            // Priority column (editable)
            ImGui::TableNextColumn();
            {
                const char* priorityLabels[] = { "High", "Medium", "Low" };  // Index 0=High(1), 1=Medium(2), 2=Low(3)
                const int priorityValues[] = { 1, 2, 3 };  // Map index to priority value
                const char* currentPriority = priorityLabels[item.priority - 1];  // priority 1=High, 2=Medium, 3=Low

                std::string comboId = "##priority_" + std::to_string(i);

                // Get color for current priority
                ImVec4 priorityColor = GetPriorityColor(item.priority);
                ImGui::PushStyleColor(ImGuiCol_Text, priorityColor);

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(comboId.c_str(), currentPriority))
                {
                    for (int p = 0; p < 3; p++)
                    {
                        ImVec4 optionColor = GetPriorityColor(priorityValues[p]);
                        ImGui::PushStyleColor(ImGuiCol_Text, optionColor);

                        bool isPrioritySelected = (item.priority == priorityValues[p]);
                        if (ImGui::Selectable(priorityLabels[p], isPrioritySelected))
                        {
                            item.priority = priorityValues[p];
                            metadataChanged = true;
                        }
                        if (isPrioritySelected)
                            ImGui::SetItemDefaultFocus();

                        ImGui::PopStyleColor();
                    }
                    ImGui::EndCombo();
                }

                ImGui::PopStyleColor();
            }

            // Artist column (editable)
            ImGui::TableNextColumn();
            {
                std::string comboId = "##artist_" + std::to_string(i);
                const char* currentArtist = users.empty() ? "(No options)" :
                                           (item.artist.empty() ? users[0].displayName.c_str() : item.artist.c_str());

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(comboId.c_str(), currentArtist, ImGuiComboFlags_HeightLarge))
                {
                    for (const auto& user : users)
                    {
                        bool isArtistSelected = (item.artist == user.displayName);
                        if (ImGui::Selectable(user.displayName.c_str(), isArtistSelected))
                        {
                            item.artist = user.displayName;
                            metadataChanged = true;
                        }
                        if (isArtistSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            // Due Date column (editable with date picker)
            ImGui::TableNextColumn();
            {
                std::string dateStr = FormatDate(item.dueDate);
                std::string buttonLabel = dateStr.empty() ? "Set Date##" + std::to_string(i) : dateStr + "##" + std::to_string(i);

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Button(buttonLabel.c_str()))
                {
                    showDatePicker = true;
                    datePickerIndex = i;
                }

                // Date picker popup
                if (showDatePicker && datePickerIndex == i)
                {
                    std::string popupId = "DatePicker##" + std::to_string(i);
                    ImGui::OpenPopup(popupId.c_str());

                    if (ImGui::BeginPopup(popupId.c_str()))
                    {
                        tm currentDate = TimestampToTm(item.dueDate > 0 ? item.dueDate : static_cast<uint64_t>(std::time(nullptr)) * 1000);

                        if (ImGui::DatePicker("##datepicker", currentDate, false))
                        {
                            item.dueDate = TmToTimestamp(currentDate);
                            metadataChanged = true;
                        }

                        if (ImGui::Button("Clear"))
                        {
                            item.dueDate = 0;
                            metadataChanged = true;
                            showDatePicker = false;
                            ImGui::CloseCurrentPopup();
                        }

                        ImGui::SameLine();

                        if (ImGui::Button("Close"))
                        {
                            showDatePicker = false;
                            ImGui::CloseCurrentPopup();
                        }

                        ImGui::EndPopup();
                    }
                    else
                    {
                        showDatePicker = false;
                    }
                }
            }

            // Notes column (compact preview, click to edit)
            ImGui::TableNextColumn();
            {
                // Pop mono font and push regular font for notes
                if (font_mono)
                    ImGui::PopFont();
                if (font_regular)
                    ImGui::PushFont(font_regular);

                // Create a one-line preview
                std::string preview = item.note.empty() ? "(click to add note)" : item.note;

                // Truncate to first line only
                size_t newlinePos = preview.find('\n');
                if (newlinePos != std::string::npos)
                {
                    preview = preview.substr(0, newlinePos) + "...";
                }

                // Truncate if too long (max 50 chars for preview)
                if (preview.length() > 50)
                {
                    preview = preview.substr(0, 47) + "...";
                }

                // Make it selectable to detect clicks
                std::string selectableId = "##note_preview_" + std::to_string(i);
                ImVec4 textColor = item.note.empty() ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, textColor);

                if (ImGui::Selectable(preview.c_str(), false, ImGuiSelectableFlags_AllowOverlap))
                {
                    // Open note editor modal
                    m_showNoteEditor = true;
                    m_noteEditorItemIndex = i;
                    m_noteEditorItemList = &items;
                    strncpy_s(m_noteEditorBuffer, item.note.c_str(), sizeof(m_noteEditorBuffer) - 1);
                    m_noteEditorBuffer[sizeof(m_noteEditorBuffer) - 1] = '\0';
                }

                ImGui::PopStyleColor();

                // Show full note on hover
                if (ImGui::IsItemHovered() && !item.note.empty())
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(400.0f);
                    ImGui::TextUnformatted(item.note.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }

                // Pop regular font and push mono font back for next row
                if (font_regular)
                    ImGui::PopFont();
                if (font_mono)
                    ImGui::PushFont(font_mono);
            }

            // Save metadata changes
            if (metadataChanged && m_subscriptionManager)
            {
                m_subscriptionManager->CreateOrUpdateShotMetadata(item);
            }

            // Pop mono font
            if (font_mono)
                ImGui::PopFont();

            ImGui::PopID();
        }

        // Pop frame padding style
        ImGui::PopStyleVar();

        ImGui::EndTable();
    }
}

void ProjectTrackerView::DrawManualTasksTable()
{
    // Add Task button
    ImVec4 accentColor = GetWindowsAccentColor();
    ImVec4 brightAccent = ImVec4(accentColor.x * 1.1f, accentColor.y * 1.1f, accentColor.z * 1.1f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, brightAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(brightAccent.x * 1.0f, brightAccent.y * 1.0f, brightAccent.z * 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(brightAccent.x * 0.7f, brightAccent.y * 0.7f, brightAccent.z * 0.7f, 1.0f));

    if (ImGui::Button("Add Task"))
    {
        m_showAddTaskDialog = true;
    }

    ImGui::PopStyleColor(3);

    // Delete button (only if selection exists)
    if (m_selectedTaskIndex >= 0 && m_selectedTaskIndex < m_manualTasks.size())
    {
        ImGui::SameLine();
        if (ImGui::Button("Delete Task"))
        {
            int taskId = m_manualTasks[m_selectedTaskIndex].id;
            if (m_subscriptionManager && m_subscriptionManager->DeleteManualTask(taskId))
            {
                m_selectedTaskIndex = -1;
                RefreshTrackedItems();
            }
        }
    }

    // Draw table
    DrawItemsTable("ManualTasksTable", "manual_task", m_manualTasks, m_selectedTaskIndex, m_tasksSortColumn, m_tasksSortAscending,
                  m_showTaskDatePicker, m_taskDatePickerIndex);
}

void ProjectTrackerView::SortItems(std::vector<UFB::ShotMetadata>& items, int column, bool ascending)
{
    std::sort(items.begin(), items.end(), [column, ascending](const UFB::ShotMetadata& a, const UFB::ShotMetadata& b) {
        bool result = false;
        switch (column)
        {
            case 0: result = a.shotPath < b.shotPath; break;
            case 1: result = a.status < b.status; break;
            case 2: result = a.category < b.category; break;
            case 3: result = a.priority < b.priority; break;
            case 4: result = a.artist < b.artist; break;
            case 5: result = a.dueDate < b.dueDate; break;
            case 6: result = a.note < b.note; break;
            default: result = false; break;
        }
        return ascending ? result : !result;
    });
}

const char* ProjectTrackerView::GetStatusLabel(const std::string& status)
{
    if (status.empty())
        return "-";
    return status.c_str();
}

const char* ProjectTrackerView::GetPriorityLabel(int priority)
{
    switch (priority)
    {
        case 1: return "High";
        case 2: return "Medium";
        case 3: return "Low";
        default: return "Unknown";
    }
}

ImVec4 ProjectTrackerView::GetStatusColor(const std::string& status, const std::string& folderType)
{
    if (m_projectConfig && m_projectConfig->IsLoaded() && !folderType.empty())
    {
        // Get color from the specific folder type
        auto colorOpt = m_projectConfig->GetStatusColor(folderType, status);
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

ImVec4 ProjectTrackerView::GetCategoryColor(const std::string& category, const std::string& folderType)
{
    if (m_projectConfig && m_projectConfig->IsLoaded() && !folderType.empty())
    {
        // Get color from the specific folder type
        auto colorOpt = m_projectConfig->GetCategoryColor(folderType, category);
        if (colorOpt.has_value())
        {
            return HexToImVec4(colorOpt.value());
        }
    }

    // Fallback default color
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

ImVec4 ProjectTrackerView::GetPriorityColor(int priority)
{
    switch (priority)
    {
        case 1: return ImVec4(0.9f, 0.2f, 0.2f, 1.0f);  // Red (High)
        case 2: return ImVec4(0.9f, 0.7f, 0.2f, 1.0f);  // Yellow (Medium)
        case 3: return ImVec4(0.2f, 0.7f, 0.9f, 1.0f);  // Blue (Low)
        default: return ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // White (Unknown)
    }
}

std::string ProjectTrackerView::FormatDate(uint64_t timestamp)
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
