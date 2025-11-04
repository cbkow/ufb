#pragma once

#include <string>
#include <vector>
#include <optional>
#include <sqlite3.h>

namespace UFB {

struct Bookmark
{
    int id;
    std::wstring path;          // File path or SMB network share path
    std::wstring displayName;   // User-friendly name
    uint64_t createdTime;
    bool isProjectFolder = false;  // True if this bookmark represents a project folder
};

class BookmarkManager
{
public:
    BookmarkManager();
    ~BookmarkManager();

    // Initialize with database
    bool Initialize(sqlite3* db);

    // Bookmark CRUD
    bool AddBookmark(const std::wstring& path, const std::wstring& displayName, bool isProjectFolder = false);
    bool RemoveBookmark(int bookmarkId);
    bool RemoveBookmark(const std::wstring& path);
    bool UpdateBookmarkName(const std::wstring& path, const std::wstring& newDisplayName);
    std::vector<Bookmark> GetAllBookmarks();
    std::optional<Bookmark> GetBookmark(int bookmarkId);
    std::optional<Bookmark> GetBookmarkByPath(const std::wstring& path);

private:
    sqlite3* m_db = nullptr;

    bool CreateTables();
};

} // namespace UFB
