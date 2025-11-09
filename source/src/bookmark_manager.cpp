#include "bookmark_manager.h"
#include "utils.h"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

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

    // Add is_project_folder column if it doesn't exist (migration for existing databases)
    const char* alterSql = "ALTER TABLE bookmarks ADD COLUMN is_project_folder INTEGER DEFAULT 0;";
    rc = sqlite3_exec(m_db, alterSql, nullptr, nullptr, &errMsg);

    // Silently ignore error if column already exists
    if (rc != SQLITE_OK)
    {
        // Column might already exist, which is fine
        sqlite3_free(errMsg);
    }

    return true;
}

bool BookmarkManager::AddBookmark(const std::wstring& path, const std::wstring& displayName, bool isProjectFolder)
{
    const char* sql = "INSERT INTO bookmarks (path, display_name, created_time, is_project_folder) VALUES (?, ?, ?, ?)";

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
    sqlite3_bind_int(stmt, 4, isProjectFolder ? 1 : 0);

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

    const char* sql = "SELECT id, path, display_name, created_time, is_project_folder FROM bookmarks";

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
        bookmark.isProjectFolder = sqlite3_column_int(stmt, 4) != 0;

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
    const char* sql = "SELECT id, path, display_name, created_time, is_project_folder FROM bookmarks WHERE id = ?";

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
        bookmark.isProjectFolder = sqlite3_column_int(stmt, 4) != 0;

        sqlite3_finalize(stmt);
        return bookmark;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

std::optional<Bookmark> BookmarkManager::GetBookmarkByPath(const std::wstring& path)
{
    const char* sql = "SELECT id, path, display_name, created_time, is_project_folder FROM bookmarks WHERE path = ?";

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
        bookmark.isProjectFolder = sqlite3_column_int(stmt, 4) != 0;

        sqlite3_finalize(stmt);
        return bookmark;
    }

    sqlite3_finalize(stmt);
    return std::nullopt;
}

bool BookmarkManager::ExportBookmarksToJSON(const std::wstring& filePath)
{
    try
    {
        // Get all bookmarks
        auto bookmarks = GetAllBookmarks();

        // Create JSON array
        nlohmann::json jsonArray = nlohmann::json::array();

        for (const auto& bookmark : bookmarks)
        {
            nlohmann::json bookmarkJson;
            bookmarkJson["path"] = WideToUtf8(bookmark.path);
            bookmarkJson["name"] = WideToUtf8(bookmark.displayName);
            bookmarkJson["isProject"] = bookmark.isProjectFolder;

            jsonArray.push_back(bookmarkJson);
        }

        // Create root object
        nlohmann::json root;
        root["version"] = 1;
        root["bookmarks"] = jsonArray;

        // Write to file
        std::ofstream outFile(filePath);
        if (!outFile.is_open())
        {
            std::wcerr << L"[BookmarkManager] Failed to open file for writing: " << filePath << std::endl;
            return false;
        }

        outFile << root.dump(2);  // Pretty print with 2-space indent
        outFile.close();

        std::wcout << L"[BookmarkManager] Exported " << bookmarks.size() << L" bookmarks to: " << filePath << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[BookmarkManager] Failed to export bookmarks: " << e.what() << std::endl;
        return false;
    }
}

bool BookmarkManager::ImportBookmarksFromJSON(const std::wstring& filePath)
{
    try
    {
        // Read file
        std::ifstream inFile(filePath);
        if (!inFile.is_open())
        {
            std::wcerr << L"[BookmarkManager] Failed to open file for reading: " << filePath << std::endl;
            return false;
        }

        nlohmann::json root;
        inFile >> root;
        inFile.close();

        // Validate format
        if (!root.contains("bookmarks") || !root["bookmarks"].is_array())
        {
            std::wcerr << L"[BookmarkManager] Invalid bookmark file format" << std::endl;
            return false;
        }

        // Import bookmarks
        const auto& bookmarksArray = root["bookmarks"];
        int imported = 0;
        int skipped = 0;

        for (const auto& bookmarkJson : bookmarksArray)
        {
            if (!bookmarkJson.contains("path") || !bookmarkJson.contains("name"))
            {
                std::wcerr << L"[BookmarkManager] Skipping invalid bookmark entry" << std::endl;
                skipped++;
                continue;
            }

            std::wstring path = Utf8ToWide(bookmarkJson["path"].get<std::string>());
            std::wstring name = Utf8ToWide(bookmarkJson["name"].get<std::string>());
            bool isProject = bookmarkJson.value("isProject", false);

            // Check if bookmark already exists
            auto existing = GetBookmarkByPath(path);
            if (existing.has_value())
            {
                std::wcout << L"[BookmarkManager] Skipping duplicate bookmark: " << path << std::endl;
                skipped++;
                continue;
            }

            // Add bookmark
            if (AddBookmark(path, name, isProject))
            {
                imported++;
            }
            else
            {
                std::wcerr << L"[BookmarkManager] Failed to import bookmark: " << path << std::endl;
                skipped++;
            }
        }

        std::wcout << L"[BookmarkManager] Import complete: " << imported << L" imported, " << skipped << L" skipped" << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[BookmarkManager] Failed to import bookmarks: " << e.what() << std::endl;
        return false;
    }
}

} // namespace UFB
