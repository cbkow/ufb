#include "bookmark_manager.h"
#include "utils.h"
#include <iostream>
#include <algorithm>

namespace UFB {

BookmarkManager::BookmarkManager()
{
}

BookmarkManager::~BookmarkManager()
{
}

bool BookmarkManager::Initialize(sqlite3* db)
{
    if (!db)
    {
        std::cerr << "BookmarkManager: Invalid database" << std::endl;
        return false;
    }

    m_db = db;
    return CreateTables();
}

bool BookmarkManager::CreateTables()
{
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS bookmarks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT NOT NULL UNIQUE,
            display_name TEXT NOT NULL,
            created_time INTEGER NOT NULL
        );
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK)
    {
        std::cerr << "BookmarkManager: Failed to create tables: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool BookmarkManager::AddBookmark(const std::wstring& path, const std::wstring& displayName)
{
    const char* sql = "INSERT INTO bookmarks (path, display_name, created_time) VALUES (?, ?, ?)";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "BookmarkManager: Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    std::string pathUtf8 = WideToUtf8(path);
    std::string displayNameUtf8 = WideToUtf8(displayName);
    uint64_t currentTime = GetCurrentTimeMs();

    sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, displayNameUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, currentTime);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        std::cerr << "BookmarkManager: Failed to insert bookmark: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    return true;
}

bool BookmarkManager::RemoveBookmark(int bookmarkId)
{
    const char* sql = "DELETE FROM bookmarks WHERE id = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "BookmarkManager: Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, bookmarkId);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool BookmarkManager::RemoveBookmark(const std::wstring& path)
{
    const char* sql = "DELETE FROM bookmarks WHERE path = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "BookmarkManager: Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    std::string pathUtf8 = WideToUtf8(path);
    sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool BookmarkManager::UpdateBookmarkName(const std::wstring& path, const std::wstring& newDisplayName)
{
    const char* sql = "UPDATE bookmarks SET display_name = ? WHERE path = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "BookmarkManager: Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    std::string displayNameUtf8 = WideToUtf8(newDisplayName);
    std::string pathUtf8 = WideToUtf8(path);

    sqlite3_bind_text(stmt, 1, displayNameUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::vector<Bookmark> BookmarkManager::GetAllBookmarks()
{
    std::vector<Bookmark> bookmarks;

    const char* sql = "SELECT id, path, display_name, created_time FROM bookmarks";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "BookmarkManager: Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return bookmarks;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Bookmark bookmark;
        bookmark.id = sqlite3_column_int(stmt, 0);

        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* displayName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        bookmark.path = Utf8ToWide(path ? path : "");
        bookmark.displayName = Utf8ToWide(displayName ? displayName : "");
        bookmark.createdTime = sqlite3_column_int64(stmt, 3);

        bookmarks.push_back(bookmark);
    }

    sqlite3_finalize(stmt);

    // Sort: drives first (alphabetically by drive letter), then other bookmarks (alphabetically by name)
    std::sort(bookmarks.begin(), bookmarks.end(), [](const Bookmark& a, const Bookmark& b) {
        // Helper to check if path is a drive (e.g., "C:\")
        auto isDrive = [](const std::wstring& path) {
            return path.length() == 3 && path[1] == L':' && path[2] == L'\\';
        };

        bool aDrive = isDrive(a.path);
        bool bDrive = isDrive(b.path);

        // Drives come before non-drives
        if (aDrive && !bDrive) return true;
        if (!aDrive && bDrive) return false;

        // Both drives or both non-drives - sort alphabetically by display name
        return a.displayName < b.displayName;
    });

    return bookmarks;
}

std::optional<Bookmark> BookmarkManager::GetBookmark(int bookmarkId)
{
    const char* sql = "SELECT id, path, display_name, created_time FROM bookmarks WHERE id = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "BookmarkManager: Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return std::nullopt;
    }

    sqlite3_bind_int(stmt, 1, bookmarkId);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Bookmark bookmark;
        bookmark.id = sqlite3_column_int(stmt, 0);

        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* displayName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        bookmark.path = Utf8ToWide(path ? path : "");
        bookmark.displayName = Utf8ToWide(displayName ? displayName : "");
        bookmark.createdTime = sqlite3_column_int64(stmt, 3);

        sqlite3_finalize(stmt);
        return bookmark;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::optional<Bookmark> BookmarkManager::GetBookmarkByPath(const std::wstring& path)
{
    const char* sql = "SELECT id, path, display_name, created_time FROM bookmarks WHERE path = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);

    if (rc != SQLITE_OK)
    {
        std::cerr << "BookmarkManager: Failed to prepare statement: " << sqlite3_errmsg(m_db) << std::endl;
        return std::nullopt;
    }

    std::string pathUtf8 = WideToUtf8(path);
    sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Bookmark bookmark;
        bookmark.id = sqlite3_column_int(stmt, 0);

        const char* pathStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* displayName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        bookmark.path = Utf8ToWide(pathStr ? pathStr : "");
        bookmark.displayName = Utf8ToWide(displayName ? displayName : "");
        bookmark.createdTime = sqlite3_column_int64(stmt, 3);

        sqlite3_finalize(stmt);
        return bookmark;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

} // namespace UFB
