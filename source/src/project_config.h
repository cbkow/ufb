#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace UFB {

// Status option with name and color
struct StatusOption
{
    std::string name;
    std::string color;  // Hex color (e.g., "#3B82F6")
};

// Category option with name and color
struct CategoryOption
{
    std::string name;
    std::string color;  // Hex color (e.g., "#8B5CF6")
};

// Default metadata for a folder type
struct DefaultMetadata
{
    std::string status;
    std::string category;
    int priority = 2;
    std::string dueDate;  // ISO date string or empty
    std::string artist;
    std::string note;
    std::vector<std::string> links;
    bool isTracked = false;  // Default to NOT tracked - user must explicitly add to tracker
};

// Folder type configuration
struct FolderTypeConfig
{
    bool isShot = false;
    bool isAsset = false;
    bool isPosting = false;
    bool isDoc = false;
    std::string addAction;  // "newShot", "newAsset", "newPosting", or empty
    std::string addActionTemplate;  // Path to template folder (e.g., "projectTemplate/3d/_t_project_name")
    std::string addActionTemplateFile;  // Path to template file (e.g., "projectTemplate/3d/_t_project_name/project/template.blend")
    std::vector<StatusOption> statusOptions;
    std::vector<CategoryOption> categoryOptions;
    DefaultMetadata defaultMetadata;
    std::map<std::string, bool> displayMetadata;  // Column visibility settings
};

// User information
struct User
{
    std::string username;
    std::string displayName;
};

// Project configuration manager
class ProjectConfig
{
public:
    ProjectConfig();
    ~ProjectConfig();

    // Load configuration from file
    bool LoadFromFile(const std::filesystem::path& filePath);

    // Load from global template in %localappdata%/ufb/
    bool LoadGlobalTemplate();

    // Load from project-specific config
    bool LoadProjectConfig(const std::wstring& projectPath);

    // Save configuration to file
    bool SaveToFile(const std::filesystem::path& filePath);

    // Get folder type configuration
    std::optional<FolderTypeConfig> GetFolderTypeConfig(const std::string& folderType) const;

    // Get status options for a folder type
    std::vector<StatusOption> GetStatusOptions(const std::string& folderType) const;

    // Get category options for a folder type
    std::vector<CategoryOption> GetCategoryOptions(const std::string& folderType) const;

    // Get status color by name
    std::optional<std::string> GetStatusColor(const std::string& folderType, const std::string& statusName) const;

    // Get category color by name
    std::optional<std::string> GetCategoryColor(const std::string& folderType, const std::string& categoryName) const;

    // Get default metadata for a folder type
    std::optional<DefaultMetadata> GetDefaultMetadata(const std::string& folderType) const;

    // Check if folder type is a shot type
    bool IsShot(const std::string& folderType) const;

    // Check if folder type is an asset type
    bool IsAsset(const std::string& folderType) const;

    // Check if folder type is a posting type
    bool IsPosting(const std::string& folderType) const;

    // Check if folder type is a doc type
    bool IsDoc(const std::string& folderType) const;

    // Get all users
    std::vector<User> GetUsers() const;

    // Get all folder types
    std::vector<std::string> GetAllFolderTypes() const;

    // Get priority options
    std::vector<int> GetPriorityOptions() const;

    // Get display metadata configuration for a folder type
    std::map<std::string, bool> GetDisplayMetadata(const std::string& folderType) const;

    // Set display metadata configuration for a folder type (and save to disk)
    void SetDisplayMetadata(const std::string& folderType, const std::map<std::string, bool>& displayMetadata);

    // Get version
    std::string GetVersion() const { return m_version; }

    // Check if config is loaded
    bool IsLoaded() const { return m_loaded; }

private:
    bool m_loaded = false;
    std::string m_version;
    std::vector<User> m_users;
    std::map<std::string, FolderTypeConfig> m_folderTypes;
    std::vector<int> m_priorityOptions;
    std::filesystem::path m_currentFilePath;  // Path to currently loaded config file

    // Parse JSON into internal structures
    bool ParseJSON(const nlohmann::json& j);

    // Helper to get %localappdata% path
    std::filesystem::path GetLocalAppDataPath() const;
};

} // namespace UFB
