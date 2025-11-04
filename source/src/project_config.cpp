#include "project_config.h"
#include <fstream>
#include <iostream>
#include <shlobj.h>  // For SHGetFolderPath
#include <windows.h>

namespace UFB {

ProjectConfig::ProjectConfig()
{
}

ProjectConfig::~ProjectConfig()
{
}

std::filesystem::path ProjectConfig::GetLocalAppDataPath() const
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
    {
        return std::filesystem::path(path) / L"ufb";
    }
    return std::filesystem::path();
}

bool ProjectConfig::LoadFromFile(const std::filesystem::path& filePath)
{
    try
    {
        // Read JSON file
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            std::cerr << "[ProjectConfig] Failed to open file: " << filePath << std::endl;
            return false;
        }

        nlohmann::json j;
        file >> j;
        file.close();

        // Parse JSON
        if (!ParseJSON(j))
        {
            std::cerr << "[ProjectConfig] Failed to parse JSON from: " << filePath << std::endl;
            return false;
        }

        m_currentFilePath = filePath;  // Store the path for later saving
        m_loaded = true;
        std::cout << "[ProjectConfig] Loaded config from: " << filePath << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ProjectConfig] Error loading config: " << e.what() << std::endl;
        return false;
    }
}

bool ProjectConfig::LoadGlobalTemplate()
{
    std::filesystem::path templatePath = GetLocalAppDataPath() / "projectTemplate.json";
    return LoadFromFile(templatePath);
}

bool ProjectConfig::LoadProjectConfig(const std::wstring& projectPath)
{
    std::filesystem::path configPath = std::filesystem::path(projectPath) / L".ufb" / L"projectConfig.json";

    // If project config exists, load it; otherwise fall back to global template
    if (std::filesystem::exists(configPath))
    {
        return LoadFromFile(configPath);
    }
    else
    {
        std::wcout << L"[ProjectConfig] Project config not found, using global template" << std::endl;
        return LoadGlobalTemplate();
    }
}

bool ProjectConfig::SaveToFile(const std::filesystem::path& filePath)
{
    try
    {
        // Create parent directories if they don't exist
        if (filePath.has_parent_path())
        {
            std::filesystem::create_directories(filePath.parent_path());
        }

        // Build JSON object
        nlohmann::json j;
        j["version"] = m_version;

        // Users
        nlohmann::json usersArray = nlohmann::json::array();
        for (const auto& user : m_users)
        {
            nlohmann::json userObj;
            userObj["username"] = user.username;
            userObj["displayName"] = user.displayName;
            usersArray.push_back(userObj);
        }
        j["users"] = usersArray;

        // Folder types
        nlohmann::json folderTypesObj;
        std::cout << "[ProjectConfig] Serializing " << m_folderTypes.size() << " folder types..." << std::endl;
        for (const auto& [typeName, config] : m_folderTypes)
        {
            std::cout << "[ProjectConfig] Processing folder type: '" << typeName << "'" << std::endl;
            nlohmann::json typeObj;

            // Flags
            if (config.isShot) typeObj["isShot"] = true;
            if (config.isAsset) typeObj["isAsset"] = true;
            if (config.isPosting) typeObj["isPosting"] = true;
            if (config.isDoc) typeObj["isDoc"] = true;

            // Add action fields
            if (!config.addAction.empty()) typeObj["addAction"] = config.addAction;
            if (!config.addActionTemplate.empty()) typeObj["addActionTemplate"] = config.addActionTemplate;
            if (!config.addActionTemplateFile.empty()) typeObj["addActionTemplateFile"] = config.addActionTemplateFile;

            // Status options
            nlohmann::json statusArray = nlohmann::json::array();
            for (const auto& status : config.statusOptions)
            {
                nlohmann::json statusObj;
                statusObj["name"] = status.name;
                statusObj["color"] = status.color;
                statusArray.push_back(statusObj);
            }
            typeObj["statusOptions"] = statusArray;

            // Category options
            nlohmann::json categoryArray = nlohmann::json::array();
            for (const auto& category : config.categoryOptions)
            {
                nlohmann::json categoryObj;
                categoryObj["name"] = category.name;
                categoryObj["color"] = category.color;
                categoryArray.push_back(categoryObj);
            }
            typeObj["categoryOptions"] = categoryArray;

            // Default metadata
            nlohmann::json metadataObj;
            try
            {
                metadataObj["Status"] = config.defaultMetadata.status;
                metadataObj["Category"] = config.defaultMetadata.category;
                metadataObj["Priority"] = config.defaultMetadata.priority;
                metadataObj["DueDate"] = config.defaultMetadata.dueDate.empty() ? nullptr : config.defaultMetadata.dueDate;
                metadataObj["Artist"] = config.defaultMetadata.artist;
                metadataObj["Note"] = config.defaultMetadata.note;

                // Safely serialize links vector
                nlohmann::json linksArray = nlohmann::json::array();
                for (const auto& link : config.defaultMetadata.links)
                {
                    linksArray.push_back(link);
                }
                metadataObj["Links"] = linksArray;

                metadataObj["IsTracked"] = config.defaultMetadata.isTracked;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[ProjectConfig] Error serializing defaultMetadata for '" << typeName << "': " << e.what() << std::endl;
                // Continue with partial metadata
            }
            typeObj["defaultMetadata"] = metadataObj;

            // Display metadata (column visibility)
            if (!config.displayMetadata.empty())
            {
                nlohmann::json displayMetadataObj;
                for (const auto& [key, value] : config.displayMetadata)
                {
                    displayMetadataObj[key] = value;
                }
                typeObj["displayMetadata"] = displayMetadataObj;
            }

            folderTypesObj[typeName] = typeObj;
        }
        j["folderTypes"] = folderTypesObj;

        // Priority options
        j["priorityOptions"] = m_priorityOptions;

        // Write to file
        std::cout << "[ProjectConfig] Opening file for writing: " << filePath << std::endl;
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            std::cerr << "[ProjectConfig] Failed to open file for writing: " << filePath << std::endl;
            return false;
        }

        std::cout << "[ProjectConfig] Dumping JSON to string..." << std::endl;
        std::string jsonString;
        try
        {
            jsonString = j.dump(2);  // Pretty print with 2-space indent
            std::cout << "[ProjectConfig] JSON dump successful, size: " << jsonString.size() << " bytes" << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[ProjectConfig] Error dumping JSON: " << e.what() << std::endl;
            file.close();
            return false;
        }

        std::cout << "[ProjectConfig] Writing JSON to file..." << std::endl;
        file << jsonString;
        file.close();

        std::cout << "[ProjectConfig] Saved config to: " << filePath << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ProjectConfig] Error saving config: " << e.what() << std::endl;
        return false;
    }
}

bool ProjectConfig::ParseJSON(const nlohmann::json& j)
{
    try
    {
        // Version
        if (j.contains("version"))
        {
            m_version = j["version"].get<std::string>();
        }

        // Users
        if (j.contains("users") && j["users"].is_array())
        {
            m_users.clear();
            for (const auto& userJson : j["users"])
            {
                User user;
                if (userJson.contains("username"))
                    user.username = userJson["username"].get<std::string>();
                if (userJson.contains("displayName"))
                    user.displayName = userJson["displayName"].get<std::string>();
                m_users.push_back(user);
            }
        }

        // Folder types
        if (j.contains("folderTypes") && j["folderTypes"].is_object())
        {
            m_folderTypes.clear();
            for (const auto& [typeName, typeJson] : j["folderTypes"].items())
            {
                FolderTypeConfig config;

                // Flags
                if (typeJson.contains("isShot"))
                    config.isShot = typeJson["isShot"].get<bool>();
                if (typeJson.contains("isAsset"))
                    config.isAsset = typeJson["isAsset"].get<bool>();
                if (typeJson.contains("isPosting"))
                    config.isPosting = typeJson["isPosting"].get<bool>();
                if (typeJson.contains("isDoc"))
                    config.isDoc = typeJson["isDoc"].get<bool>();

                // Add action fields
                if (typeJson.contains("addAction"))
                    config.addAction = typeJson["addAction"].get<std::string>();
                if (typeJson.contains("addActionTemplate"))
                    config.addActionTemplate = typeJson["addActionTemplate"].get<std::string>();
                if (typeJson.contains("addActionTemplateFile"))
                    config.addActionTemplateFile = typeJson["addActionTemplateFile"].get<std::string>();

                // Status options
                if (typeJson.contains("statusOptions") && typeJson["statusOptions"].is_array())
                {
                    for (const auto& statusJson : typeJson["statusOptions"])
                    {
                        StatusOption status;
                        if (statusJson.contains("name"))
                            status.name = statusJson["name"].get<std::string>();
                        if (statusJson.contains("color"))
                            status.color = statusJson["color"].get<std::string>();
                        config.statusOptions.push_back(status);
                    }
                }

                // Category options
                if (typeJson.contains("categoryOptions") && typeJson["categoryOptions"].is_array())
                {
                    for (const auto& categoryJson : typeJson["categoryOptions"])
                    {
                        CategoryOption category;
                        if (categoryJson.contains("name"))
                            category.name = categoryJson["name"].get<std::string>();
                        if (categoryJson.contains("color"))
                            category.color = categoryJson["color"].get<std::string>();
                        config.categoryOptions.push_back(category);
                    }
                }

                // Default metadata
                if (typeJson.contains("defaultMetadata"))
                {
                    const auto& metadataJson = typeJson["defaultMetadata"];
                    if (metadataJson.contains("Status"))
                        config.defaultMetadata.status = metadataJson["Status"].get<std::string>();
                    if (metadataJson.contains("Category"))
                        config.defaultMetadata.category = metadataJson["Category"].get<std::string>();
                    if (metadataJson.contains("Priority"))
                        config.defaultMetadata.priority = metadataJson["Priority"].get<int>();
                    if (metadataJson.contains("DueDate") && !metadataJson["DueDate"].is_null())
                        config.defaultMetadata.dueDate = metadataJson["DueDate"].get<std::string>();
                    if (metadataJson.contains("Artist"))
                        config.defaultMetadata.artist = metadataJson["Artist"].get<std::string>();
                    if (metadataJson.contains("Note"))
                        config.defaultMetadata.note = metadataJson["Note"].get<std::string>();
                    if (metadataJson.contains("Links") && metadataJson["Links"].is_array())
                        config.defaultMetadata.links = metadataJson["Links"].get<std::vector<std::string>>();
                    if (metadataJson.contains("IsTracked"))
                        config.defaultMetadata.isTracked = metadataJson["IsTracked"].get<bool>();
                }

                // Display metadata (column visibility)
                if (typeJson.contains("displayMetadata") && typeJson["displayMetadata"].is_object())
                {
                    const auto& displayMetadataJson = typeJson["displayMetadata"];
                    for (const auto& [key, value] : displayMetadataJson.items())
                    {
                        if (value.is_boolean())
                        {
                            config.displayMetadata[key] = value.get<bool>();
                        }
                    }
                }

                m_folderTypes[typeName] = config;
            }
        }

        // Priority options
        if (j.contains("priorityOptions") && j["priorityOptions"].is_array())
        {
            m_priorityOptions = j["priorityOptions"].get<std::vector<int>>();
        }

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ProjectConfig] Error parsing JSON: " << e.what() << std::endl;
        return false;
    }
}

std::optional<FolderTypeConfig> ProjectConfig::GetFolderTypeConfig(const std::string& folderType) const
{
    auto it = m_folderTypes.find(folderType);
    if (it != m_folderTypes.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::vector<StatusOption> ProjectConfig::GetStatusOptions(const std::string& folderType) const
{
    auto config = GetFolderTypeConfig(folderType);
    if (config.has_value())
    {
        return config->statusOptions;
    }
    return {};
}

std::vector<CategoryOption> ProjectConfig::GetCategoryOptions(const std::string& folderType) const
{
    auto config = GetFolderTypeConfig(folderType);
    if (config.has_value())
    {
        return config->categoryOptions;
    }
    return {};
}

std::optional<std::string> ProjectConfig::GetStatusColor(const std::string& folderType, const std::string& statusName) const
{
    auto statusOptions = GetStatusOptions(folderType);
    for (const auto& status : statusOptions)
    {
        if (status.name == statusName)
        {
            return status.color;
        }
    }
    return std::nullopt;
}

std::optional<std::string> ProjectConfig::GetCategoryColor(const std::string& folderType, const std::string& categoryName) const
{
    auto categoryOptions = GetCategoryOptions(folderType);
    for (const auto& category : categoryOptions)
    {
        if (category.name == categoryName)
        {
            return category.color;
        }
    }
    return std::nullopt;
}

std::optional<DefaultMetadata> ProjectConfig::GetDefaultMetadata(const std::string& folderType) const
{
    auto config = GetFolderTypeConfig(folderType);
    if (config.has_value())
    {
        return config->defaultMetadata;
    }
    return std::nullopt;
}

bool ProjectConfig::IsShot(const std::string& folderType) const
{
    auto config = GetFolderTypeConfig(folderType);
    return config.has_value() && config->isShot;
}

bool ProjectConfig::IsAsset(const std::string& folderType) const
{
    auto config = GetFolderTypeConfig(folderType);
    return config.has_value() && config->isAsset;
}

bool ProjectConfig::IsPosting(const std::string& folderType) const
{
    auto config = GetFolderTypeConfig(folderType);
    return config.has_value() && config->isPosting;
}

bool ProjectConfig::IsDoc(const std::string& folderType) const
{
    auto config = GetFolderTypeConfig(folderType);
    return config.has_value() && config->isDoc;
}

std::vector<User> ProjectConfig::GetUsers() const
{
    return m_users;
}

std::vector<std::string> ProjectConfig::GetAllFolderTypes() const
{
    std::vector<std::string> types;
    for (const auto& [typeName, _] : m_folderTypes)
    {
        types.push_back(typeName);
    }
    return types;
}

std::vector<int> ProjectConfig::GetPriorityOptions() const
{
    return m_priorityOptions;
}

std::map<std::string, bool> ProjectConfig::GetDisplayMetadata(const std::string& folderType) const
{
    auto config = GetFolderTypeConfig(folderType);
    if (config.has_value())
    {
        return config->displayMetadata;
    }
    return {};  // Return empty map if folder type not found
}

void ProjectConfig::SetDisplayMetadata(const std::string& folderType, const std::map<std::string, bool>& displayMetadata)
{
    std::cout << "[ProjectConfig] SetDisplayMetadata called for folder type: " << folderType << std::endl;

    // Find the folder type config
    auto it = m_folderTypes.find(folderType);
    if (it != m_folderTypes.end())
    {
        std::cout << "[ProjectConfig] Found folder type '" << folderType << "', updating displayMetadata" << std::endl;

        // Update the display metadata IN MEMORY ONLY
        it->second.displayMetadata = displayMetadata;

        std::cout << "[ProjectConfig] displayMetadata updated in memory (NOT saving to disk to prevent crashes)" << std::endl;
        std::cout << "[ProjectConfig] Note: displayMetadata changes are temporary and will not persist" << std::endl;

        // TODO: Implement safe saving that doesn't crash
        // For now, we skip saving to avoid the crash
        // The displayMetadata will be lost when you close the shot view

        /* DISABLED TO PREVENT CRASH:
        // Save to disk if we have a current file path
        if (!m_currentFilePath.empty())
        {
            std::cout << "[ProjectConfig] Saving to: " << m_currentFilePath << std::endl;

            try
            {
                bool saveSuccess = SaveToFile(m_currentFilePath);
                if (saveSuccess)
                {
                    std::cout << "[ProjectConfig] Successfully saved displayMetadata to disk" << std::endl;
                }
                else
                {
                    std::cerr << "[ProjectConfig] Failed to save displayMetadata to disk" << std::endl;
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "[ProjectConfig] Exception while saving: " << e.what() << std::endl;
            }
        }
        else
        {
            std::cerr << "[ProjectConfig] WARNING: m_currentFilePath is empty, cannot save to disk" << std::endl;
        }
        */
    }
    else
    {
        std::cerr << "[ProjectConfig] ERROR: Folder type '" << folderType << "' not found in m_folderTypes" << std::endl;
        std::cerr << "[ProjectConfig] Available folder types: ";
        for (const auto& [type, _] : m_folderTypes)
        {
            std::cerr << "'" << type << "' ";
        }
        std::cerr << std::endl;
    }
}

} // namespace UFB
