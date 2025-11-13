#pragma once

#include <string>
#include <functional>
#include "bookmark_manager.h"
#include "subscription_manager.h"
#include "icon_manager.h"

namespace UFB {

class SubscriptionPanel
{
public:
    SubscriptionPanel();
    ~SubscriptionPanel();

    // Initialize with dependencies
    void Initialize(BookmarkManager* bookmarkManager, SubscriptionManager* subscriptionManager, IconManager* iconManager);

    // Draw the subscription panel
    void Draw(const char* title, bool withWindow = true);

    // Callbacks
    std::function<void(const std::wstring& path)> onNavigateToPath;  // Default navigation (Browser 1)
    std::function<void(const std::wstring& path)> onNavigateToBrowser1;
    std::function<void(const std::wstring& path)> onNavigateToBrowser2;
    std::function<void(const std::wstring& jobPath, const std::wstring& jobName)> onOpenProjectTracker;
    std::function<void(const std::wstring& jobPath, const std::wstring& jobName)> onOpenBackupRestore;

private:
    BookmarkManager* m_bookmarkManager = nullptr;
    SubscriptionManager* m_subscriptionManager = nullptr;
    IconManager* m_iconManager = nullptr;

    // UI state
    bool m_showAddBookmarkModal = false;
    bool m_showDeleteConfirmModal = false;

    // Modal input buffers
    char m_bookmarkPath[512] = {0};
    char m_bookmarkName[256] = {0};
    bool m_bookmarkIsProjectFolder = false;

    // Delete confirmation state
    enum class DeleteType { None, Bookmark, Job };
    DeleteType m_deleteType = DeleteType::None;
    int m_deleteBookmarkId = -1;
    std::wstring m_deleteJobPath;
    std::wstring m_deleteItemName;

    // UI helpers
    void DrawBookmarksSection();
    void DrawJobsSection();
    void DrawAddBookmarkModal();
    void DrawDeleteConfirmModal();

    const char* GetSyncStatusIcon(SyncStatus status);
    const char* GetSyncStatusText(SyncStatus status);
};

} // namespace UFB
