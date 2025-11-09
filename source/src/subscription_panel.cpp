#include "subscription_panel.h"
#include "imgui.h"
#include "utils.h"
#include <iostream>

// C++20 changed u8"" literals to char8_t, need to cast for ImGui
#define U8(x) reinterpret_cast<const char*>(u8##x)

// External function from main.cpp
extern ImVec4 GetWindowsAccentColor();

// External font pointers (from main.cpp)
extern ImFont* font_regular;
extern ImFont* font_icons;

namespace UFB {

SubscriptionPanel::SubscriptionPanel()
{
}

SubscriptionPanel::~SubscriptionPanel()
{
}

void SubscriptionPanel::Initialize(BookmarkManager* bookmarkManager, SubscriptionManager* subscriptionManager, IconManager* iconManager)
{
    m_bookmarkManager = bookmarkManager;
    m_subscriptionManager = subscriptionManager;
    m_iconManager = iconManager;
}

void SubscriptionPanel::Draw(const char* title, bool withWindow)
{
    if (withWindow)
        ImGui::Begin(title);

    // Create nested child window with padding for highlight border
    float contentPadding = 6.0f;
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.x -= contentPadding * 2;
    contentSize.y -= contentPadding * 2;

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + contentPadding, ImGui::GetCursorPosY() + contentPadding));

    ImGui::BeginChild("##subscription_content", contentSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Header with add buttons
    ImGui::Text("Bookmarks & Jobs");
    ImGui::Separator();

    // Bookmarks section
    DrawBookmarksSection();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Jobs section
    DrawJobsSection();

    ImGui::EndChild();

    if (withWindow)
        ImGui::End();

    // Draw modals
    DrawAddBookmarkModal();
    DrawDeleteConfirmModal();
}

void SubscriptionPanel::DrawBookmarksSection()
{
    // Section header
    if (ImGui::CollapsingHeader("Bookmarks", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        // Add bookmark button
        if (ImGui::Button("+ Add Bookmark"))
        {
            m_showAddBookmarkModal = true;
            memset(m_bookmarkPath, 0, sizeof(m_bookmarkPath));
            memset(m_bookmarkName, 0, sizeof(m_bookmarkName));
            m_bookmarkIsProjectFolder = false;
        }

        ImGui::Spacing();

        // List bookmarks
        auto bookmarks = m_bookmarkManager->GetAllBookmarks();

        if (bookmarks.empty())
        {
            ImGui::TextDisabled("No bookmarks");
        }
        else
        {
            for (const auto& bookmark : bookmarks)
            {
                ImGui::PushID(bookmark.id);

                // Get icon for this bookmark
                ImTextureID icon = 0;
                if (m_iconManager)
                {
                    // Check if it's a drive (path is "X:\")
                    bool isDrive = (bookmark.path.length() == 3 && bookmark.path[1] == L':' && bookmark.path[2] == L'\\');
                    icon = m_iconManager->GetFileIcon(bookmark.path, true, 16);
                }

                // Bookmark item with icon
                std::string displayName = WideToUtf8(bookmark.displayName);
                bool isSelected = false;

                // Draw icon if available
                if (icon)
                {
                    ImGui::Image(icon, ImVec2(16, 16));
                    ImGui::SameLine();
                }

                if (ImGui::Selectable(displayName.c_str(), isSelected))
                {
                    // Navigate to bookmarked path
                    if (onNavigateToPath)
                    {
                        onNavigateToPath(bookmark.path);
                    }
                }

                // Context menu
                if (ImGui::BeginPopupContextItem())
                {
                    ImGui::Text("%s", displayName.c_str());
                    ImGui::Separator();

                    if (ImGui::MenuItem("Open in Browser 1"))
                    {
                        if (onNavigateToBrowser1)
                        {
                            onNavigateToBrowser1(bookmark.path);
                        }
                    }

                    if (ImGui::MenuItem("Open in Browser 2"))
                    {
                        if (onNavigateToBrowser2)
                        {
                            onNavigateToBrowser2(bookmark.path);
                        }
                    }

                    ImGui::Separator();

                    if (ImGui::MenuItem("Delete"))
                    {
                        m_deleteType = DeleteType::Bookmark;
                        m_deleteBookmarkId = bookmark.id;
                        m_deleteItemName = bookmark.displayName;
                        m_showDeleteConfirmModal = true;
                    }

                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }
        }

        ImGui::Unindent();
    }
}

void SubscriptionPanel::DrawJobsSection()
{
    // Section header
    if (ImGui::CollapsingHeader("Synced Jobs", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        // List active subscriptions
        auto subscriptions = m_subscriptionManager->GetActiveSubscriptions();

        if (subscriptions.empty())
        {
            ImGui::TextDisabled("No synced jobs");
        }
        else
        {
            for (const auto& sub : subscriptions)
            {
                ImGui::PushID(sub.id);

                // Job item with sync status icon
                const char* statusIcon = GetSyncStatusIcon(sub.syncStatus);
                std::string displayName = WideToUtf8(sub.jobName);

                bool isSelected = false;

                // Render icon with icon font
                if (font_icons)
                    ImGui::PushFont(font_icons);

                ImGui::Text("%s", statusIcon);

                if (font_icons)
                    ImGui::PopFont();

                ImGui::SameLine();

                // Render selectable item for the job name
                if (ImGui::Selectable(displayName.c_str(), isSelected))
                {
                    // Navigate to job path
                    if (onNavigateToPath)
                    {
                        onNavigateToPath(sub.jobPath);
                    }
                }

                // Context menu
                if (ImGui::BeginPopupContextItem())
                {
                    ImGui::Text("%s", displayName.c_str());
                    ImGui::Separator();

                    if (ImGui::MenuItem("Open in Browser 1"))
                    {
                        if (onNavigateToBrowser1)
                        {
                            onNavigateToBrowser1(sub.jobPath);
                        }
                    }

                    if (ImGui::MenuItem("Open in Browser 2"))
                    {
                        if (onNavigateToBrowser2)
                        {
                            onNavigateToBrowser2(sub.jobPath);
                        }
                    }

                    ImGui::Separator();

                    // Project Tracker menu item with bright accent color
                    ImVec4 accentColor = GetWindowsAccentColor();
                    ImVec4 brightAccent = ImVec4(accentColor.x * 1.3f, accentColor.y * 1.3f, accentColor.z * 1.3f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, brightAccent);

                    if (ImGui::MenuItem("Project Tracker"))
                    {
                        if (onOpenProjectTracker)
                        {
                            onOpenProjectTracker(sub.jobPath, sub.jobName);
                        }
                    }

                    ImGui::PopStyleColor();

                    ImGui::Separator();

                    if (ImGui::MenuItem("Unsubscribe"))
                    {
                        m_deleteType = DeleteType::Job;
                        m_deleteJobPath = sub.jobPath;
                        m_deleteItemName = sub.jobName;
                        m_showDeleteConfirmModal = true;
                    }

                    ImGui::EndPopup();
                }

                // Tooltip with status and path
                if (ImGui::IsItemHovered())
                {
                    std::string path = WideToUtf8(sub.jobPath);
                    const char* statusText = GetSyncStatusText(sub.syncStatus);
                    ImGui::BeginTooltip();
                    ImGui::Text("Path: %s", path.c_str());
                    ImGui::Text("Status: %s", statusText);
                    ImGui::Text("Shots: %d", sub.shotCount);
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
            }
        }

        ImGui::Unindent();
    }
}

void SubscriptionPanel::DrawAddBookmarkModal()
{
    if (m_showAddBookmarkModal)
    {
        ImGui::OpenPopup("Add Bookmark");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Add Bookmark", &m_showAddBookmarkModal, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Add a new bookmark");
        ImGui::Separator();

        ImGui::InputText("Path", m_bookmarkPath, sizeof(m_bookmarkPath));
        ImGui::InputText("Name", m_bookmarkName, sizeof(m_bookmarkName));
        ImGui::Checkbox("Is Project Folder", &m_bookmarkIsProjectFolder);

        ImGui::Spacing();

        if (ImGui::Button("Add", ImVec2(120, 0)))
        {
            if (strlen(m_bookmarkPath) > 0 && strlen(m_bookmarkName) > 0)
            {
                std::wstring path = Utf8ToWide(m_bookmarkPath);
                std::wstring name = Utf8ToWide(m_bookmarkName);

                if (m_bookmarkManager->AddBookmark(path, name, m_bookmarkIsProjectFolder))
                {
                    std::cout << "Bookmark added: " << m_bookmarkName << std::endl;
                    m_showAddBookmarkModal = false;
                }
                else
                {
                    std::cerr << "Failed to add bookmark" << std::endl;
                }
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_showAddBookmarkModal = false;
        }

        ImGui::EndPopup();
    }
}

void SubscriptionPanel::DrawDeleteConfirmModal()
{
    if (m_showDeleteConfirmModal)
    {
        ImGui::OpenPopup("Confirm Delete");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Confirm Delete", &m_showDeleteConfirmModal, ImGuiWindowFlags_AlwaysAutoResize))
    {
        std::string itemName = WideToUtf8(m_deleteItemName);

        if (m_deleteType == DeleteType::Bookmark)
        {
            ImGui::Text("Delete bookmark '%s'?", itemName.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("This will remove the bookmark from your list.");
        }
        else if (m_deleteType == DeleteType::Job)
        {
            ImGui::Text("Unsubscribe from job '%s'?", itemName.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("This will stop syncing and remove the job from your subscriptions.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Delete", ImVec2(120, 0)))
        {
            if (m_deleteType == DeleteType::Bookmark)
            {
                if (m_bookmarkManager->RemoveBookmark(m_deleteBookmarkId))
                {
                    std::cout << "Bookmark deleted: " << itemName << std::endl;
                }
                else
                {
                    std::cerr << "Failed to delete bookmark" << std::endl;
                }
            }
            else if (m_deleteType == DeleteType::Job)
            {
                if (m_subscriptionManager->UnsubscribeFromJob(m_deleteJobPath))
                {
                    std::cout << "Unsubscribed from job: " << itemName << std::endl;
                }
                else
                {
                    std::cerr << "Failed to unsubscribe from job" << std::endl;
                }
            }

            m_showDeleteConfirmModal = false;
            m_deleteType = DeleteType::None;
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_showDeleteConfirmModal = false;
            m_deleteType = DeleteType::None;
        }

        ImGui::EndPopup();
    }
}

const char* SubscriptionPanel::GetSyncStatusIcon(SyncStatus status)
{
    switch (status)
    {
    case SyncStatus::Pending:  return U8("\uE836"); // radio_button_unchecked
    case SyncStatus::Syncing:  return U8("\uE863"); // autorenew
    case SyncStatus::Synced:   return U8("\uE86C"); // check_circle
    case SyncStatus::Stale:    return U8("\uE002"); // access_time
    case SyncStatus::Error:    return U8("\uE000"); // error
    default:                   return "?";
    }
}

const char* SubscriptionPanel::GetSyncStatusText(SyncStatus status)
{
    switch (status)
    {
    case SyncStatus::Pending:  return "Pending";
    case SyncStatus::Syncing:  return "Syncing";
    case SyncStatus::Synced:   return "Synced";
    case SyncStatus::Stale:    return "Stale";
    case SyncStatus::Error:    return "Error";
    default:                   return "Unknown";
    }
}

} // namespace UFB
