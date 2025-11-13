#include "backup_restore_view.h"
#include "utils.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <nlohmann/json.hpp>

BackupRestoreView::BackupRestoreView()
{
}

BackupRestoreView::~BackupRestoreView()
{
    Shutdown();
}

void BackupRestoreView::Initialize(const std::wstring& jobPath, const std::wstring& jobName,
                                     UFB::BackupManager* backupManager)
{
    m_jobPath = jobPath;
    m_jobName = jobName;
    m_backupManager = backupManager;
    m_isOpen = true;
    m_isShutdown = false;

    // Load backup list
    RefreshBackupList();
}

void BackupRestoreView::Shutdown()
{
    if (m_isShutdown)
        return;

    m_isShutdown = true;
    m_backups.clear();
}

void BackupRestoreView::Draw(const char* title, HWND hwnd)
{
    if (!m_isOpen || m_isShutdown)
        return;

    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(title, &m_isOpen, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    // Header section
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Backup & Restore Manager");
    ImGui::Text("Job: %s", UFB::WideToUtf8(m_jobName).c_str());
    ImGui::Separator();

    // Status message
    if (!m_statusMessage.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
        ImGui::TextWrapped("%s", m_statusMessage.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // Toolbar
    if (ImGui::Button("Refresh"))
    {
        RefreshBackupList();
        m_statusMessage = "Backup list refreshed";
    }

    ImGui::SameLine();
    if (ImGui::Button("Create Backup Now"))
    {
        if (m_backupManager)
        {
            m_statusMessage = "Creating backup...";
            bool success = m_backupManager->CreateBackup(m_jobPath);
            if (success)
            {
                m_statusMessage = "Backup created successfully!";
                RefreshBackupList();
            }
            else
            {
                m_statusMessage = "Failed to create backup. Check console for errors.";
            }
        }
    }

    ImGui::Separator();

    // Main content - split into left (table) and right (preview) panels
    ImGui::BeginChild("BackupTablePanel", ImVec2(ImGui::GetContentRegionAvail().x * 0.3f, 0), true);
    DrawBackupTable();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("PreviewPanel", ImVec2(0, 0), true);
    DrawPreviewPanel();
    ImGui::EndChild();

    // Restore confirmation modal
    DrawRestoreConfirmModal();

    // Handle window close
    if (!m_isOpen && onClose)
    {
        onClose();
    }

    ImGui::End();
}

void BackupRestoreView::RefreshBackupList()
{
    if (!m_backupManager)
        return;

    m_backups = m_backupManager->ListBackups(m_jobPath);

    // Sort by timestamp descending (newest first)
    std::sort(m_backups.begin(), m_backups.end(),
              [](const UFB::BackupInfo& a, const UFB::BackupInfo& b) {
                  return a.timestamp > b.timestamp;
              });

    m_selectedBackupIndex = -1;
}

void BackupRestoreView::LoadBackupContents(int backupIndex)
{
    m_selectedBackupItems.clear();
    m_itemsLoaded = false;

    if (backupIndex < 0 || backupIndex >= (int)m_backups.size())
    {
        m_itemsLoaded = true;  // Mark as loaded (empty)
        return;
    }

    const auto& backup = m_backups[backupIndex];

    // Extract timestamp from filename to find the backup directory
    std::wstring timestamp = backup.filename;
    if (timestamp.find(L"backup_") == 0)
    {
        timestamp = timestamp.substr(7);  // Remove "backup_" prefix
    }

    // Build path to backup change logs directory
    std::filesystem::path backupDir = std::filesystem::path(m_jobPath) / L".ufb" / L"backups";
    std::filesystem::path changesBackupDir = backupDir / (L"changes_" + timestamp);

    if (!std::filesystem::exists(changesBackupDir))
    {
        std::cout << "[BackupRestore] Change logs not found at: " << changesBackupDir << std::endl;
        m_itemsLoaded = true;  // Mark as loaded (empty)
        return;
    }

    // Parse all change log files and collect unique items
    std::map<std::string, BackupItemInfo> itemsMap;  // shotPath -> item info
    std::set<std::string> deletedPaths;  // Track deleted items

    try
    {
        // First pass: collect all entries and track operations
        for (const auto& entry : std::filesystem::directory_iterator(changesBackupDir))
        {
            if (entry.path().extension() != ".json")
                continue;

            std::ifstream file(entry.path());
            if (!file.is_open())
                continue;

            nlohmann::json doc;
            try
            {
                file >> doc;
            }
            catch (const std::exception& e)
            {
                std::cout << "[BackupRestore] Failed to parse " << entry.path().filename() << ": " << e.what() << std::endl;
                continue;
            }

            // Change logs are arrays of change entries
            if (!doc.is_array())
                continue;

            for (const auto& changeEntry : doc)
            {
                if (!changeEntry.contains("shotPath"))
                    continue;

                std::string shotPath = changeEntry["shotPath"];
                std::string operation = changeEntry.value("operation", "update");
                uint64_t timestamp = changeEntry.value("timestamp", 0ULL);

                // Track deletions
                if (operation == "delete")
                {
                    deletedPaths.insert(shotPath);
                    itemsMap.erase(shotPath);  // Remove if we already added it
                    continue;
                }

                // Skip if this item was deleted (by a later entry we already processed)
                if (deletedPaths.count(shotPath) > 0)
                    continue;

                // Update or add item (only if we have data)
                if (!changeEntry.contains("data"))
                    continue;

                // If item already exists, only update if this entry is newer
                if (itemsMap.count(shotPath) > 0)
                {
                    if (timestamp <= itemsMap[shotPath].modifiedTime)
                        continue;  // Skip older entries
                }

                // Extract item info
                BackupItemInfo item;
                item.shotPath = shotPath;
                item.name = GetItemDisplayName(shotPath);

                const auto& data = changeEntry["data"];
                if (data.contains("metadata"))
                {
                    const auto& metadata = data["metadata"];
                    item.itemType = metadata.value("itemType", "shot");
                    item.status = metadata.value("status", "");
                    item.artist = metadata.value("artist", "");
                    item.priority = metadata.value("priority", 0);
                }

                item.modifiedTime = data.value("modified_time", 0ULL);

                itemsMap[shotPath] = item;
            }
        }

        // Convert map to vector (only non-deleted items)
        for (const auto& pair : itemsMap)
        {
            m_selectedBackupItems.push_back(pair.second);
        }

        m_itemsLoaded = true;
        std::cout << "[BackupRestore] Loaded " << m_selectedBackupItems.size() << " items from backup" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cout << "[BackupRestore] Error loading backup contents: " << e.what() << std::endl;
    }
}

void BackupRestoreView::DrawBackupTable()
{
    ImGui::Text("Available Backups (%d)", (int)m_backups.size());
    ImGui::Separator();

    if (m_backups.empty())
    {
        ImGui::TextDisabled("No backups found for this job.");
        ImGui::TextDisabled("Create a backup using the button above.");
        return;
    }

    // Table with backups
    if (ImGui::BeginTable("BackupsTable", 4,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_Resizable))
    {
        // Headers
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Rows
        for (int i = 0; i < (int)m_backups.size(); i++)
        {
            const auto& backup = m_backups[i];

            // Push unique ID for this row to avoid conflicts
            ImGui::PushID(i);

            ImGui::TableNextRow();

            // Selectable row
            ImGui::TableNextColumn();
            bool isSelected = (m_selectedBackupIndex == i);

            std::string dateStr = UFB::WideToUtf8(backup.date);
            if (ImGui::Selectable(dateStr.c_str(), isSelected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick))
            {
                // Selection changed - load backup contents
                if (m_selectedBackupIndex != i)
                {
                    m_selectedBackupIndex = i;
                    m_itemsLoaded = false;  // Mark for reload
                    LoadBackupContents(i);
                }

                // Double-click to show restore confirm
                if (ImGui::IsMouseDoubleClicked(0))
                {
                    m_showRestoreConfirm = true;
                }
            }

            // Time column
            ImGui::TableNextColumn();
            std::string timeStr = FormatTimestamp(backup.timestamp);
            ImGui::Text("%s", timeStr.c_str());

            // Items column
            ImGui::TableNextColumn();
            ImGui::Text("%d", backup.shotCount);

            // Size column
            ImGui::TableNextColumn();
            std::string sizeStr = FormatFileSize(backup.uncompressedSize);
            ImGui::Text("%s", sizeStr.c_str());

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

void BackupRestoreView::DrawPreviewPanel()
{
    ImGui::Text("Backup Contents");
    ImGui::Separator();

    if (m_selectedBackupIndex < 0 || m_selectedBackupIndex >= (int)m_backups.size())
    {
        ImGui::TextDisabled("Select a backup to view its contents");
        return;
    }

    const auto& backup = m_backups[m_selectedBackupIndex];

    // Compact backup info header
    ImGui::Text("Date: %s  |  Items: %d  |  Created by: %s",
                UFB::WideToUtf8(backup.date).c_str(),
                backup.shotCount,
                backup.createdBy.c_str());

    ImGui::Separator();
    ImGui::Spacing();

    // Items table
    if (m_itemsLoaded && !m_selectedBackupItems.empty())
    {
        ImGui::Text("Items in this backup: %d", (int)m_selectedBackupItems.size());

        if (ImGui::BeginTable("BackupItemsTable", 5,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_Sortable,
                              ImVec2(0, ImGui::GetContentRegionAvail().y - 100)))
        {
            // Headers
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // Rows
            for (size_t i = 0; i < m_selectedBackupItems.size(); i++)
            {
                const auto& item = m_selectedBackupItems[i];

                ImGui::PushID((int)i);
                ImGui::TableNextRow();

                // Name
                ImGui::TableNextColumn();
                ImGui::Text("%s", item.name.c_str());

                // Type
                ImGui::TableNextColumn();
                ImGui::Text("%s", item.itemType.c_str());

                // Status
                ImGui::TableNextColumn();
                ImGui::Text("%s", item.status.empty() ? "-" : item.status.c_str());

                // Artist
                ImGui::TableNextColumn();
                ImGui::Text("%s", item.artist.empty() ? "-" : item.artist.c_str());

                // Priority
                ImGui::TableNextColumn();
                if (item.priority > 0)
                    ImGui::Text("%d", item.priority);
                else
                    ImGui::Text("-");

                ImGui::PopID();
            }

            ImGui::EndTable();
        }
    }
    else if (!m_itemsLoaded)
    {
        ImGui::TextDisabled("Loading backup contents...");
    }
    else
    {
        ImGui::TextDisabled("No items found in this backup");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Restore button
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.3f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.2f, 0.1f, 1.0f));

    if (ImGui::Button("Restore This Backup", ImVec2(-1, 40)))
    {
        m_showRestoreConfirm = true;
    }

    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::TextDisabled("Tip: Double-click a backup in the table to restore");
}

void BackupRestoreView::DrawRestoreConfirmModal()
{
    if (!m_showRestoreConfirm)
        return;

    ImGui::OpenPopup("Restore Backup?");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Restore Backup?", &m_showRestoreConfirm,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_selectedBackupIndex >= 0 && m_selectedBackupIndex < (int)m_backups.size())
        {
            const auto& backup = m_backups[m_selectedBackupIndex];

            ImGui::Text("You are about to restore the following backup:");
            ImGui::Separator();

            ImGui::Text("Date: %s at %s", UFB::WideToUtf8(backup.date).c_str(),
                       FormatTimestamp(backup.timestamp).c_str());
            ImGui::Text("Items: %d", backup.shotCount);
            ImGui::Text("Created by: %s", backup.createdBy.c_str());

            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
            ImGui::TextWrapped("This will replace all current data with the backup!");
            ImGui::TextWrapped("A safety backup will be created first.");
            ImGui::TextWrapped("Restored items will be timestamped as 'latest' to override sync.");
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Restore button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.3f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.2f, 0.1f, 1.0f));

            if (ImGui::Button("Confirm Restore", ImVec2(200, 40)))
            {
                // Perform restore
                if (m_backupManager)
                {
                    m_statusMessage = "Restoring backup...";
                    bool success = m_backupManager->RestoreBackup(m_jobPath, backup.filename);

                    if (success)
                    {
                        m_statusMessage = "Backup restored successfully! Please reload views to see changes.";
                    }
                    else
                    {
                        m_statusMessage = "Failed to restore backup. Check console for errors.";
                    }
                }

                m_showRestoreConfirm = false;
                RefreshBackupList();
            }

            ImGui::PopStyleColor(3);

            ImGui::SameLine();

            if (ImGui::Button("Cancel", ImVec2(200, 40)))
            {
                m_showRestoreConfirm = false;
            }
        }
        else
        {
            ImGui::Text("Error: No backup selected");
            if (ImGui::Button("Close"))
            {
                m_showRestoreConfirm = false;
            }
        }

        ImGui::EndPopup();
    }
}

std::string BackupRestoreView::GetItemDisplayName(const std::string& shotPath)
{
    // Extract filename from path
    std::filesystem::path path(shotPath);
    std::string filename = path.filename().string();

    // Remove __task_ prefix if present
    if (filename.find("__task_") == 0)
    {
        filename = filename.substr(7);  // Remove "__task_"

        // Remove UUID suffix if present (pattern: _UUID)
        size_t lastUnderscore = filename.rfind('_');
        if (lastUnderscore != std::string::npos)
        {
            // Check if what follows looks like a UUID (has hyphens)
            std::string potentialUUID = filename.substr(lastUnderscore + 1);
            if (potentialUUID.find('-') != std::string::npos)
            {
                filename = filename.substr(0, lastUnderscore);
            }
        }
    }

    return filename;
}

std::string BackupRestoreView::FormatTimestamp(uint64_t timestamp)
{
    if (timestamp == 0)
        return "N/A";

    std::time_t time = static_cast<std::time_t>(timestamp / 1000);
    std::tm tm;
    localtime_s(&tm, &time);

    std::stringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
}

std::string BackupRestoreView::FormatFileSize(size_t bytes)
{
    if (bytes == 0)
        return "N/A";

    const char* units[] = { "B", "KB", "MB", "GB" };
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 3)
    {
        size /= 1024.0;
        unitIndex++;
    }

    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
    return ss.str();
}
