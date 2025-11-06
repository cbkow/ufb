#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif

#include <glad/gl.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "file_browser.h"
#include "subscription_manager.h"
#include "metadata_manager.h"
#include "backup_manager.h"
#include "sync_manager.h"
#include "bookmark_manager.h"
#include "subscription_panel.h"
#include "transcode_queue_panel.h"
#include "project_config.h"
#include "shot_view.h"
#include "assets_view.h"
#include "postings_view.h"
#include "project_tracker_view.h"
#include "utils.h"

using json = nlohmann::json;

// Windows accent color toggle state
bool use_windows_accent_color = true;

// Font pointers
ImFont* font_regular = nullptr;
ImFont* font_mono = nullptr;
ImFont* font_icons = nullptr;

// Layout persistence
std::string saved_imgui_layout;
bool first_time_setup = true;
bool reset_to_default_layout = false;

// Window state persistence
struct WindowState {
    int x = -1;          // -1 means not set (use default)
    int y = -1;
    int width = 1914;
    int height = 1060;
    bool maximized = false;
};
WindowState window_state;

// Shot view persistence
struct ShotViewData {
    std::wstring categoryPath;
    std::wstring categoryName;
};
std::vector<ShotViewData> saved_shot_views;

// Assets view persistence
struct AssetsViewData {
    std::wstring assetsFolderPath;
    std::wstring jobName;
};
std::vector<AssetsViewData> saved_assets_views;

// Postings view persistence
struct PostingsViewData {
    std::wstring postingsFolderPath;
    std::wstring jobName;
};
std::vector<PostingsViewData> saved_postings_views;

// Project tracker view persistence
struct TrackerViewData {
    std::wstring jobPath;
    std::wstring jobName;
};
std::vector<TrackerViewData> saved_tracker_views;

// Standalone browser window persistence
struct BrowserWindowData {
    std::wstring currentPath;
};
std::vector<BrowserWindowData> saved_browser_windows;

static void glfw_error_callback(int error, const char* description)
{
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

// ============================================================================
// WINDOWS ACCENT COLOR UTILITIES
// ============================================================================
ImVec4 GetFallbackYellowColor() {
    return ImVec4(0.65f, 0.55f, 0.15f, 1.0f);
}

#ifdef _WIN32
ImVec4 GetWindowsAccentColor() {
    if (!use_windows_accent_color) {
        return GetFallbackYellowColor();
    }

    DWORD colorization_color;
    BOOL opaque_blend;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
        // Convert ARGB to ImVec4 RGBA
        float r = ((colorization_color >> 16) & 0xff) / 255.0f;
        float g = ((colorization_color >> 8) & 0xff) / 255.0f;
        float b = (colorization_color & 0xff) / 255.0f;
        return ImVec4(r, g, b, 1.0f);
    }
    return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback blue
}
#else
ImVec4 GetWindowsAccentColor() {
    if (!use_windows_accent_color) {
        return GetFallbackYellowColor();
    }
    return ImVec4(0.65f, 0.55f, 0.15f, 1.0f);
}
#endif

// ============================================================================
// IMGUI STYLING (UnionPlayer inspired)
// ============================================================================
void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.128f, 0.128f, 0.128f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.19f, 0.19f, 0.19f, 0.40f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.060f, 0.060f, 0.060f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.172f, 0.172f, 0.172f, 1.00f);         // Tab bar background
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.172f, 0.172f, 0.172f, 1.00f);  // Active tab bar
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.172f, 0.172f, 0.172f, 0.70f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark] = GetWindowsAccentColor();
    colors[ImGuiCol_SliderGrab] = ImVec4(0.54f, 0.54f, 0.54f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.67f, 0.67f, 0.67f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.19f, 0.19f, 0.19f, 0.55f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.28f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.30f, 0.30f, 0.29f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.28f, 0.28f, 0.29f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.29f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40f, 0.44f, 0.47f, 1.00f);
    // Windows-style tabs - more prominent and taller
    colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);              // Inactive tabs - lighter
    colors[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);      // Hovered - brighter
    colors[ImGuiCol_TabActive] = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);    // Active - matches window bg
    colors[ImGuiCol_TabSelectedOverline] = GetWindowsAccentColor();        // Active tab top line - use accent
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);    // Unfocused window tabs
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f); // Unfocused active
    colors[ImGuiCol_TabDimmedSelectedOverline] = GetWindowsAccentColor();  // Unfocused active tab top line
    colors[ImGuiCol_DockingPreview] = ImVec4(0.60f, 0.60f, 0.60f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.26f, 0.26f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.01f, 0.01f, 0.65f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.31f, 0.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 0.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.01f);

    style.WindowPadding = ImVec2(12.00f, 12.00f);
    style.FramePadding = ImVec2(8.00f, 8.00f);  // Increased for taller tabs and better spacing
    style.CellPadding = ImVec2(8.00f, 8.00f);
    style.ItemSpacing = ImVec2(7.00f, 7.00f);
    style.ItemInnerSpacing = ImVec2(6.00f, 6.00f);
    style.TouchExtraPadding = ImVec2(0.00f, 0.00f);
    style.IndentSpacing = 25;
    style.ScrollbarSize = 15;
    style.GrabMinSize = 10;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.PopupBorderSize = 0;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.WindowRounding = 2;
    style.ChildRounding = 2;
    style.FrameRounding = 2;
    style.PopupRounding = 4;
    style.ScrollbarRounding = 9;
    style.GrabRounding = 3;
    style.LogSliderDeadzone = 4;
    style.TabRounding = 0;  // Slightly more rounded for modern Windows look
}

#ifdef _WIN32
void EnableDarkModeWindow(GLFWwindow* window) {
    HWND hwnd = glfwGetWin32Window(window);
    BOOL value = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
}
#endif

// ============================================================================
// SETTINGS PERSISTENCE
// ============================================================================
std::string GetSettingsPath() {
    const char* localappdata = std::getenv("LOCALAPPDATA");
    if (localappdata) {
        std::string base_path = std::string(localappdata) + "\\ufb";
        std::filesystem::create_directories(base_path);
        return base_path + "\\settings.json";
    }
    return "settings.json";  // Fallback to current directory
}

WindowState GetCurrentWindowState(GLFWwindow* window) {
    WindowState state;

    // Check if window is maximized
    state.maximized = (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE);

    // Get window position and size
    if (!state.maximized) {
        glfwGetWindowPos(window, &state.x, &state.y);
        glfwGetWindowSize(window, &state.width, &state.height);
    } else {
        // If maximized, still get the restored position/size for when it's unmaximized
        // GLFW doesn't provide a direct way to get "restore" bounds, so we'll just
        // save what we have - when restoring, we'll maximize after setting size
        glfwGetWindowPos(window, &state.x, &state.y);
        glfwGetWindowSize(window, &state.width, &state.height);
    }

    return state;
}

void SaveSettings(GLFWwindow* window, bool showSubscriptions, bool showBrowser1, bool showBrowser2, bool showTranscodeQueue,
                  const std::vector<std::unique_ptr<ShotView>>& shotViews,
                  const std::vector<std::unique_ptr<AssetsView>>& assetsViews,
                  const std::vector<std::unique_ptr<PostingsView>>& postingsViews,
                  const std::vector<std::unique_ptr<ProjectTrackerView>>& trackerViews,
                  const std::vector<std::unique_ptr<FileBrowser>>& standaloneBrowsers) {
    try {
        json j;

        // Save ImGui layout to memory
        size_t ini_size = 0;
        const char* ini_data = ImGui::SaveIniSettingsToMemory(&ini_size);
        if (ini_data && ini_size > 0) {
            j["imgui_layout"] = std::string(ini_data, ini_size);
        }

        // Save window state
        WindowState current_state = GetCurrentWindowState(window);
        j["window"]["x"] = current_state.x;
        j["window"]["y"] = current_state.y;
        j["window"]["width"] = current_state.width;
        j["window"]["height"] = current_state.height;
        j["window"]["maximized"] = current_state.maximized;

        // Save panel visibility states
        j["panels"]["show_subscriptions"] = showSubscriptions;
        j["panels"]["show_browser1"] = showBrowser1;
        j["panels"]["show_browser2"] = showBrowser2;
        j["panels"]["show_transcode_queue"] = showTranscodeQueue;
        j["panels"]["use_windows_accent"] = use_windows_accent_color;

        // Save open shot views (category path and name)
        j["shot_views"] = json::array();
        for (const auto& shotView : shotViews) {
            if (shotView) {
                json shotViewData;
                shotViewData["category_path"] = UFB::WideToUtf8(shotView->GetCategoryPath());
                shotViewData["category_name"] = UFB::WideToUtf8(shotView->GetCategoryName());
                j["shot_views"].push_back(shotViewData);
            }
        }

        // Save open assets views
        j["assets_views"] = json::array();
        for (const auto& assetsView : assetsViews) {
            if (assetsView) {
                json assetsViewData;
                assetsViewData["folder_path"] = UFB::WideToUtf8(assetsView->GetAssetsFolderPath());
                assetsViewData["job_name"] = UFB::WideToUtf8(assetsView->GetJobName());
                j["assets_views"].push_back(assetsViewData);
            }
        }

        // Save open postings views
        j["postings_views"] = json::array();
        for (const auto& postingsView : postingsViews) {
            if (postingsView) {
                json postingsViewData;
                postingsViewData["folder_path"] = UFB::WideToUtf8(postingsView->GetPostingsFolderPath());
                postingsViewData["job_name"] = UFB::WideToUtf8(postingsView->GetJobName());
                j["postings_views"].push_back(postingsViewData);
            }
        }

        // Save open tracker views
        j["tracker_views"] = json::array();
        for (const auto& trackerView : trackerViews) {
            if (trackerView) {
                json trackerViewData;
                trackerViewData["job_path"] = UFB::WideToUtf8(trackerView->GetJobPath());
                trackerViewData["job_name"] = UFB::WideToUtf8(trackerView->GetJobName());
                j["tracker_views"].push_back(trackerViewData);
            }
        }

        // Save open standalone browser windows
        j["standalone_browsers"] = json::array();
        for (const auto& browser : standaloneBrowsers) {
            if (browser && browser->IsOpen()) {
                json browserData;
                browserData["current_path"] = UFB::WideToUtf8(browser->GetCurrentDirectory());
                j["standalone_browsers"].push_back(browserData);
            }
        }

        // Write to file
        std::string settings_path = GetSettingsPath();
        std::ofstream file(settings_path);
        if (file.is_open()) {
            file << j.dump(2);  // Pretty-print with 2-space indent
            file.close();
            std::cout << "Settings saved to: " << settings_path << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to save settings: " << e.what() << std::endl;
    }
}

void LoadSettings(bool& showSubscriptions, bool& showBrowser1, bool& showBrowser2, bool& showTranscodeQueue) {
    try {
        std::string settings_path = GetSettingsPath();
        std::ifstream file(settings_path);
        if (!file.is_open()) {
            std::cout << "No saved settings found, using defaults" << std::endl;
            first_time_setup = true;
            return;
        }

        json j;
        file >> j;
        file.close();

        // Load ImGui layout
        if (j.contains("imgui_layout")) {
            saved_imgui_layout = j["imgui_layout"].get<std::string>();
            if (!saved_imgui_layout.empty()) {
                first_time_setup = false;  // We have a saved layout
                std::cout << "Found saved ImGui layout" << std::endl;
            }
        } else {
            first_time_setup = true;
        }

        // Load window state
        if (j.contains("window")) {
            window_state.x = j["window"].value("x", -1);
            window_state.y = j["window"].value("y", -1);
            window_state.width = j["window"].value("width", 1280);
            window_state.height = j["window"].value("height", 720);
            window_state.maximized = j["window"].value("maximized", false);
            std::cout << "Loaded window state: " << window_state.width << "x" << window_state.height
                      << " at (" << window_state.x << ", " << window_state.y << ")"
                      << (window_state.maximized ? " [maximized]" : "") << std::endl;
        }

        // Load panel visibility states
        if (j.contains("panels")) {
            showSubscriptions = j["panels"].value("show_subscriptions", true);
            showBrowser1 = j["panels"].value("show_browser1", true);
            showBrowser2 = j["panels"].value("show_browser2", true);
            showTranscodeQueue = j["panels"].value("show_transcode_queue", false);
            use_windows_accent_color = j["panels"].value("use_windows_accent", true);
        }

        // Load shot views
        saved_shot_views.clear();
        if (j.contains("shot_views") && j["shot_views"].is_array()) {
            std::cout << "[LoadSettings] Found shot_views array with " << j["shot_views"].size() << " entries" << std::endl;
            for (const auto& shotViewJson : j["shot_views"]) {
                std::cout << "[LoadSettings] Processing shot view entry..." << std::endl;
                std::cout << "[LoadSettings]   Has category_path: " << shotViewJson.contains("category_path") << std::endl;
                std::cout << "[LoadSettings]   Has category_name: " << shotViewJson.contains("category_name") << std::endl;
                if (shotViewJson.contains("category_path") && shotViewJson.contains("category_name")) {
                    ShotViewData data;
                    std::string catPath = shotViewJson["category_path"].get<std::string>();
                    std::string catName = shotViewJson["category_name"].get<std::string>();
                    std::cout << "[LoadSettings]   category_path: " << catPath << std::endl;
                    std::cout << "[LoadSettings]   category_name: " << catName << std::endl;
                    data.categoryPath = UFB::Utf8ToWide(catPath);
                    data.categoryName = UFB::Utf8ToWide(catName);
                    saved_shot_views.push_back(data);
                    std::cout << "[LoadSettings]   Added to saved_shot_views" << std::endl;
                }
            }
            std::cout << "Loaded " << saved_shot_views.size() << " shot view(s) to restore" << std::endl;
        } else {
            std::cout << "[LoadSettings] No shot_views array found in settings" << std::endl;
        }

        // Load assets views
        saved_assets_views.clear();
        if (j.contains("assets_views") && j["assets_views"].is_array()) {
            for (const auto& assetsViewJson : j["assets_views"]) {
                if (assetsViewJson.contains("folder_path") && assetsViewJson.contains("job_name")) {
                    AssetsViewData data;
                    data.assetsFolderPath = UFB::Utf8ToWide(assetsViewJson["folder_path"].get<std::string>());
                    data.jobName = UFB::Utf8ToWide(assetsViewJson["job_name"].get<std::string>());
                    saved_assets_views.push_back(data);
                }
            }
            std::cout << "Loaded " << saved_assets_views.size() << " assets view(s) to restore" << std::endl;
        }

        // Load postings views
        saved_postings_views.clear();
        if (j.contains("postings_views") && j["postings_views"].is_array()) {
            for (const auto& postingsViewJson : j["postings_views"]) {
                if (postingsViewJson.contains("folder_path") && postingsViewJson.contains("job_name")) {
                    PostingsViewData data;
                    data.postingsFolderPath = UFB::Utf8ToWide(postingsViewJson["folder_path"].get<std::string>());
                    data.jobName = UFB::Utf8ToWide(postingsViewJson["job_name"].get<std::string>());
                    saved_postings_views.push_back(data);
                }
            }
            std::cout << "Loaded " << saved_postings_views.size() << " postings view(s) to restore" << std::endl;
        }

        // Load tracker views
        saved_tracker_views.clear();
        if (j.contains("tracker_views") && j["tracker_views"].is_array()) {
            for (const auto& trackerViewJson : j["tracker_views"]) {
                if (trackerViewJson.contains("job_path") && trackerViewJson.contains("job_name")) {
                    TrackerViewData data;
                    data.jobPath = UFB::Utf8ToWide(trackerViewJson["job_path"].get<std::string>());
                    data.jobName = UFB::Utf8ToWide(trackerViewJson["job_name"].get<std::string>());
                    saved_tracker_views.push_back(data);
                }
            }
            std::cout << "Loaded " << saved_tracker_views.size() << " tracker view(s) to restore" << std::endl;
        }

        // Load standalone browser windows
        saved_browser_windows.clear();
        if (j.contains("standalone_browsers") && j["standalone_browsers"].is_array()) {
            for (const auto& browserJson : j["standalone_browsers"]) {
                if (browserJson.contains("current_path")) {
                    BrowserWindowData data;
                    data.currentPath = UFB::Utf8ToWide(browserJson["current_path"].get<std::string>());
                    saved_browser_windows.push_back(data);
                }
            }
            std::cout << "Loaded " << saved_browser_windows.size() << " standalone browser(s) to restore" << std::endl;
        }

        std::cout << "Settings loaded from: " << settings_path << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to load settings: " << e.what() << std::endl;
        first_time_setup = true;
    }
}

// ============================================================================
// DEFAULT LAYOUT SETUP
// ============================================================================
void SetupDefaultLayout(ImGuiID dockspace_id, const ImVec2& viewport_size) {
    // Setup main dockspace - Browser window fills everything
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport_size);

    // Dock the Browser window and Transcode Queue as tabs in the same dockspace
    ImGui::DockBuilderDockWindow("Browser", dockspace_id);
    ImGui::DockBuilderDockWindow("Transcode Queue", dockspace_id);

    ImGui::DockBuilderFinish(dockspace_id);
    std::cout << "Main layout setup complete" << std::endl;
}


// ============================================================================
// FONT LOADING
// ============================================================================
void LoadCustomFonts() {
    ImGuiIO& io = ImGui::GetIO();

    // Get the executable directory for font loading
#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
#else
    std::filesystem::path exeDir = std::filesystem::current_path();
#endif

    // Build font paths relative to executable
    std::filesystem::path interPath = exeDir / "assets" / "fonts" / "Inter_18pt-Regular.ttf";
    std::filesystem::path monoPath = exeDir / "assets" / "fonts" / "JetBrainsMono-Regular.ttf";
    std::filesystem::path iconsPath = exeDir / "assets" / "fonts" / "MaterialSymbolsSharp-Regular.ttf";

    // Load fonts (smaller sizes)
    if (std::filesystem::exists(interPath)) {
        font_regular = io.Fonts->AddFontFromFileTTF(interPath.string().c_str(), 18.0f);
    }

    if (std::filesystem::exists(monoPath)) {
        font_mono = io.Fonts->AddFontFromFileTTF(monoPath.string().c_str(), 15.0f);
    }

    // Load Material Icons font
    if (std::filesystem::exists(iconsPath)) {
        ImFontConfig icons_config;
        icons_config.MergeMode = false;
        icons_config.PixelSnapH = true;
        static const ImWchar icons_ranges[] = { 0xE000, 0xF8FF, 0 };
        font_icons = io.Fonts->AddFontFromFileTTF(iconsPath.string().c_str(), 18.0f, &icons_config, icons_ranges);
    }

    // Fallback to default if Inter didn't load
    if (!font_regular) {
        font_regular = io.Fonts->AddFontDefault();
    }

    io.FontDefault = font_regular;
}

int main(int argc, char** argv)
{
    // Setup window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // GL 3.3 + GLSL 330
    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(1280, 720, "u.f.b.", nullptr, nullptr);
    if (window == nullptr)
        return 1;

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Initialize OpenGL loader
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize OpenGL loader!" << std::endl;
        return 1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // Disable automatic .ini file - we'll save layout manually to settings.json
    io.IniFilename = nullptr;
    std::cout << "ImGui layout will be saved to settings.json (not imgui.ini)" << std::endl;

    // Load custom fonts (UnionPlayer fonts)
    LoadCustomFonts();

    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Setup Dear ImGui style (UnionPlayer theme)
    SetupImGuiStyle();

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Get HWND for Windows shell integration
    HWND hwnd = glfwGetWin32Window(window);

#ifdef _WIN32
    // Enable Windows dark mode title bar
    EnableDarkModeWindow(window);

    // Load and set application icon from resources
    HINSTANCE hInstance = GetModuleHandle(NULL);
    HICON hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101)); // IDI_ICON1 = 101
    if (hIcon)
    {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }
#endif

    // Initialize global project template on first run
    {
        wchar_t localAppData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)))
        {
            std::filesystem::path ufbDir = std::filesystem::path(localAppData) / L"ufb";
            std::filesystem::path templatePath = ufbDir / L"projectTemplate.json";

            // Check if template already exists
            if (!std::filesystem::exists(templatePath))
            {
                std::cout << "[Main] First run detected - initializing global project template..." << std::endl;

                // Create ufb directory if it doesn't exist
                std::filesystem::create_directories(ufbDir);

                // Copy template from assets folder
                std::filesystem::path sourceTemplate = "assets/projectTemplate.json";
                if (std::filesystem::exists(sourceTemplate))
                {
                    std::filesystem::copy_file(sourceTemplate, templatePath);
                    std::cout << "[Main] Global template initialized at: " << templatePath << std::endl;
                }
                else
                {
                    std::cerr << "[Main] Warning: Could not find assets/projectTemplate.json" << std::endl;
                }
            }
        }
    }

    // Initialize subscription manager
    UFB::SubscriptionManager subscriptionManager;
    if (!subscriptionManager.Initialize())
    {
        std::cerr << "Failed to initialize SubscriptionManager" << std::endl;
        return 1;
    }

    // Initialize metadata manager
    UFB::MetadataManager metadataManager;
    if (!metadataManager.Initialize(&subscriptionManager))
    {
        std::cerr << "Failed to initialize MetadataManager" << std::endl;
        return 1;
    }

    // Initialize backup manager
    UFB::BackupManager backupManager;

    // Initialize sync manager
    UFB::SyncManager syncManager;
    if (!syncManager.Initialize(&subscriptionManager, &metadataManager, &backupManager))
    {
        std::cerr << "Failed to initialize SyncManager" << std::endl;
        return 1;
    }

    // Start background sync (5 second interval)
    syncManager.StartSync(std::chrono::seconds(5));

    // Initialize bookmark manager
    UFB::BookmarkManager bookmarkManager;
    if (!bookmarkManager.Initialize(subscriptionManager.GetDatabase()))
    {
        std::cerr << "Failed to initialize BookmarkManager" << std::endl;
        return 1;
    }

    // Auto-add system drives to bookmarks
    DWORD driveMask = GetLogicalDrives();
    for (int i = 0; i < 26; i++)
    {
        if (driveMask & (1 << i))
        {
            wchar_t driveLetter = L'A' + i;
            std::wstring drivePath = std::wstring(1, driveLetter) + L":\\";

            // Get drive type for better naming
            UINT driveType = GetDriveTypeW(drivePath.c_str());
            std::wstring driveName;

            switch (driveType)
            {
            case DRIVE_FIXED:     driveName = driveLetter + std::wstring(L": Drive"); break;
            case DRIVE_REMOVABLE: driveName = driveLetter + std::wstring(L": Removable Drive"); break;
            case DRIVE_REMOTE:    driveName = driveLetter + std::wstring(L": Network Drive"); break;
            case DRIVE_CDROM:     driveName = driveLetter + std::wstring(L": CD-ROM"); break;
            case DRIVE_RAMDISK:   driveName = driveLetter + std::wstring(L": RAM Disk"); break;
            default:              driveName = driveLetter + std::wstring(L": Drive"); break;
            }

            // Check if drive already exists in bookmarks
            auto existingBookmark = bookmarkManager.GetBookmarkByPath(drivePath);
            if (existingBookmark.has_value())
            {
                // Update the name if it's using old format (e.g., "Drive C" instead of "C: Drive")
                if (existingBookmark->displayName != driveName)
                {
                    bookmarkManager.UpdateBookmarkName(drivePath, driveName);
                    std::wcout << L"Updated drive bookmark: " << existingBookmark->displayName << L" -> " << driveName << std::endl;
                }
            }
            else
            {
                // Add new bookmark
                bookmarkManager.AddBookmark(drivePath, driveName);
                std::wcout << L"Auto-added drive: " << driveName << L" (" << drivePath << L")" << std::endl;
            }
        }
    }

    // Auto-add special folders to bookmarks (Desktop, Documents, Downloads)
    wchar_t pathBuffer[MAX_PATH];

    // Desktop
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, pathBuffer)))
    {
        std::wstring desktopPath = pathBuffer;
        auto existingBookmark = bookmarkManager.GetBookmarkByPath(desktopPath);
        if (!existingBookmark.has_value())
        {
            bookmarkManager.AddBookmark(desktopPath, L"Desktop");
            std::wcout << L"Auto-added bookmark: Desktop (" << desktopPath << L")" << std::endl;
        }
    }

    // Documents
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, pathBuffer)))
    {
        std::wstring documentsPath = pathBuffer;
        auto existingBookmark = bookmarkManager.GetBookmarkByPath(documentsPath);
        if (!existingBookmark.has_value())
        {
            bookmarkManager.AddBookmark(documentsPath, L"Documents");
            std::wcout << L"Auto-added bookmark: Documents (" << documentsPath << L")" << std::endl;
        }
    }

    // Downloads
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, pathBuffer)))
    {
        std::wstring downloadsPath = std::wstring(pathBuffer) + L"\\Downloads";
        auto existingBookmark = bookmarkManager.GetBookmarkByPath(downloadsPath);
        if (!existingBookmark.has_value())
        {
            bookmarkManager.AddBookmark(downloadsPath, L"Downloads");
            std::wcout << L"Auto-added bookmark: Downloads (" << downloadsPath << L")" << std::endl;
        }
    }

    // Initialize icon manager for subscription panel
    IconManager subscriptionIconManager;
    subscriptionIconManager.Initialize();

    // Initialize file browsers with manager dependencies
    FileBrowser fileBrowser1;
    FileBrowser fileBrowser2;
    fileBrowser1.Initialize(&bookmarkManager, &subscriptionManager);
    fileBrowser2.Initialize(&bookmarkManager, &subscriptionManager);

    // Initialize subscription panel
    UFB::SubscriptionPanel subscriptionPanel;
    subscriptionPanel.Initialize(&bookmarkManager, &subscriptionManager, &subscriptionIconManager);

    // Initialize transcode queue panel
    UFB::TranscodeQueuePanel transcodeQueuePanel;

    // Set up callbacks for subscription panel
    subscriptionPanel.onNavigateToPath = [&fileBrowser1](const std::wstring& path) {
        fileBrowser1.SetCurrentDirectory(path);  // Default to Browser 1
    };

    subscriptionPanel.onNavigateToBrowser1 = [&fileBrowser1](const std::wstring& path) {
        fileBrowser1.SetCurrentDirectory(path);
    };

    subscriptionPanel.onNavigateToBrowser2 = [&fileBrowser2](const std::wstring& path) {
        fileBrowser2.SetCurrentDirectory(path);
    };

    // Set up transcode callbacks for both file browsers
    fileBrowser1.onTranscodeToMP4 = [&transcodeQueuePanel](const std::vector<std::wstring>& paths) {
        transcodeQueuePanel.AddMultipleJobs(paths);
        transcodeQueuePanel.Show();  // Automatically show the queue panel when jobs are added
        ImGui::SetWindowFocus("Transcode Queue");  // Switch to the Transcode Queue tab
    };

    fileBrowser2.onTranscodeToMP4 = [&transcodeQueuePanel](const std::vector<std::wstring>& paths) {
        transcodeQueuePanel.AddMultipleJobs(paths);
        transcodeQueuePanel.Show();  // Automatically show the queue panel when jobs are added
        ImGui::SetWindowFocus("Transcode Queue");  // Switch to the Transcode Queue tab
    };

    // Set up callbacks to open file location in browsers and switch to Browser tab
    transcodeQueuePanel.onOpenInBrowser1 = [&fileBrowser1](const std::wstring& path) {
        fileBrowser1.SetCurrentDirectory(path);
        // Focus the Browser window to switch to that tab
        ImGui::SetWindowFocus("Browser");
    };

    transcodeQueuePanel.onOpenInBrowser2 = [&fileBrowser2](const std::wstring& path) {
        fileBrowser2.SetCurrentDirectory(path);
        // Focus the Browser window to switch to that tab
        ImGui::SetWindowFocus("Browser");
    };

    // Shot views management
    std::vector<std::unique_ptr<ShotView>> shotViews;

    // Assets views management
    std::vector<std::unique_ptr<AssetsView>> assetsViews;

    // Postings views management
    std::vector<std::unique_ptr<PostingsView>> postingsViews;

    // Project tracker views management
    std::vector<std::unique_ptr<ProjectTrackerView>> trackerViews;

    // Standalone browser windows management
    std::vector<std::unique_ptr<FileBrowser>> standaloneBrowsers;

    // Set up shot view callbacks for both file browsers
    auto openShotViewCallback = [&shotViews, &bookmarkManager, &subscriptionManager, &fileBrowser1, &fileBrowser2, &transcodeQueuePanel, hwnd](const std::wstring& categoryPath, const std::wstring& categoryName) {
        // Check if this shot view is already open
        for (const auto& sv : shotViews) {
            if (sv && sv->GetCategoryPath() == categoryPath) {
                std::wcout << L"[Main] Shot view already open for: " << categoryPath << std::endl;
                return;
            }
        }

        // Create new shot view
        auto shotView = std::make_unique<ShotView>();
        shotView->Initialize(categoryPath, categoryName, &bookmarkManager, &subscriptionManager);

        // Set up close callback
        shotView->onClose = [&shotViews, categoryPath]() {
            // Find and remove this shot view
            for (auto it = shotViews.begin(); it != shotViews.end(); ++it) {
                if ((*it) && (*it)->GetCategoryPath() == categoryPath) {
                    shotViews.erase(it);
                    break;
                }
            }
        };

        // Set up "Open in Browser" callbacks and switch to Browser tab
        shotView->onOpenInBrowser1 = [&fileBrowser1](const std::wstring& path) {
            std::wcout << L"[ShotView] Opening in Browser 1: " << path << std::endl;
            fileBrowser1.SetCurrentDirectory(path);
            ImGui::SetWindowFocus("Browser");
        };

        shotView->onOpenInBrowser2 = [&fileBrowser2](const std::wstring& path) {
            std::wcout << L"[ShotView] Opening in Browser 2: " << path << std::endl;
            fileBrowser2.SetCurrentDirectory(path);
            ImGui::SetWindowFocus("Browser");
        };
        std::wcout << L"[ShotView] Callbacks set for: " << categoryPath << std::endl;

        // Set up transcode callback
        shotView->onTranscodeToMP4 = [&transcodeQueuePanel](const std::vector<std::wstring>& paths) {
            transcodeQueuePanel.AddMultipleJobs(paths);
            transcodeQueuePanel.Show();
            ImGui::SetWindowFocus("Transcode Queue");
        };

        shotViews.push_back(std::move(shotView));
        std::wcout << L"[Main] Opened shot view for: " << categoryPath << std::endl;
    };

    fileBrowser1.onOpenShotView = openShotViewCallback;
    fileBrowser2.onOpenShotView = openShotViewCallback;

    // Set up assets view callbacks for both file browsers
    auto openAssetsViewCallback = [&assetsViews, &bookmarkManager, &subscriptionManager, &fileBrowser1, &fileBrowser2, &transcodeQueuePanel, hwnd](const std::wstring& assetsFolderPath, const std::wstring& jobName) {
        // Check if this assets view is already open
        for (const auto& av : assetsViews) {
            if (av && av->GetAssetsFolderPath() == assetsFolderPath) {
                std::wcout << L"[Main] Assets view already open for: " << assetsFolderPath << std::endl;
                return;
            }
        }

        // Create new assets view
        auto assetsView = std::make_unique<AssetsView>();
        assetsView->Initialize(assetsFolderPath, jobName, &bookmarkManager, &subscriptionManager);

        // Set up close callback
        assetsView->onClose = [&assetsViews, assetsFolderPath]() {
            for (auto it = assetsViews.begin(); it != assetsViews.end(); ++it) {
                if ((*it) && (*it)->GetAssetsFolderPath() == assetsFolderPath) {
                    assetsViews.erase(it);
                    break;
                }
            }
        };

        // Set up browser callbacks
        assetsView->onOpenInBrowser1 = [&fileBrowser1](const std::wstring& path) {
            std::wcout << L"[AssetsView] Opening in Browser 1: " << path << std::endl;
            fileBrowser1.SetCurrentDirectory(path);
            ImGui::SetWindowFocus("Browser");
        };

        assetsView->onOpenInBrowser2 = [&fileBrowser2](const std::wstring& path) {
            std::wcout << L"[AssetsView] Opening in Browser 2: " << path << std::endl;
            fileBrowser2.SetCurrentDirectory(path);
            ImGui::SetWindowFocus("Browser");
        };

        // Set up transcode callback
        assetsView->onTranscodeToMP4 = [&transcodeQueuePanel](const std::vector<std::wstring>& paths) {
            std::wcout << L"[Main] AssetsView transcode callback received " << paths.size() << L" files" << std::endl;
            for (const auto& path : paths) {
                std::wcout << L"  - " << path << std::endl;
            }
            transcodeQueuePanel.AddMultipleJobs(paths);
            transcodeQueuePanel.Show();
            ImGui::SetWindowFocus("Transcode Queue");
        };

        assetsViews.push_back(std::move(assetsView));
        std::wcout << L"[Main] Opened assets view for: " << assetsFolderPath << std::endl;
    };

    fileBrowser1.onOpenAssetsView = openAssetsViewCallback;
    fileBrowser2.onOpenAssetsView = openAssetsViewCallback;

    // Set up postings view callbacks for both file browsers
    auto openPostingsViewCallback = [&postingsViews, &bookmarkManager, &subscriptionManager, &fileBrowser1, &fileBrowser2, &transcodeQueuePanel, hwnd](const std::wstring& postingsFolderPath, const std::wstring& jobName) {
        // Check if this postings view is already open
        for (const auto& pv : postingsViews) {
            if (pv && pv->GetPostingsFolderPath() == postingsFolderPath) {
                std::wcout << L"[Main] Postings view already open for: " << postingsFolderPath << std::endl;
                return;
            }
        }

        // Create new postings view
        auto postingsView = std::make_unique<PostingsView>();
        postingsView->Initialize(postingsFolderPath, jobName, &bookmarkManager, &subscriptionManager);

        // Set up close callback
        postingsView->onClose = [&postingsViews, postingsFolderPath]() {
            for (auto it = postingsViews.begin(); it != postingsViews.end(); ++it) {
                if ((*it) && (*it)->GetPostingsFolderPath() == postingsFolderPath) {
                    postingsViews.erase(it);
                    break;
                }
            }
        };

        // Set up browser callbacks
        postingsView->onOpenInBrowser1 = [&fileBrowser1](const std::wstring& path) {
            std::wcout << L"[PostingsView] Opening in Browser 1: " << path << std::endl;
            fileBrowser1.SetCurrentDirectory(path);
            ImGui::SetWindowFocus("Browser");
        };

        postingsView->onOpenInBrowser2 = [&fileBrowser2](const std::wstring& path) {
            std::wcout << L"[PostingsView] Opening in Browser 2: " << path << std::endl;
            fileBrowser2.SetCurrentDirectory(path);
            ImGui::SetWindowFocus("Browser");
        };

        // Set up transcode callback
        postingsView->onTranscodeToMP4 = [&transcodeQueuePanel](const std::vector<std::wstring>& paths) {
            transcodeQueuePanel.AddMultipleJobs(paths);
            transcodeQueuePanel.Show();
            ImGui::SetWindowFocus("Transcode Queue");
        };

        postingsViews.push_back(std::move(postingsView));
        std::wcout << L"[Main] Opened postings view for: " << postingsFolderPath << std::endl;
    };

    fileBrowser1.onOpenPostingsView = openPostingsViewCallback;
    fileBrowser2.onOpenPostingsView = openPostingsViewCallback;

    // Set up project tracker view callback for subscription panel
    subscriptionPanel.onOpenProjectTracker = [&trackerViews, &subscriptionManager, &openShotViewCallback, &openAssetsViewCallback, &openPostingsViewCallback, &shotViews, &assetsViews, &postingsViews, hwnd](const std::wstring& jobPath, const std::wstring& jobName) {
        // Check if this tracker view is already open
        for (const auto& tv : trackerViews) {
            if (tv && tv->GetJobPath() == jobPath) {
                std::wcout << L"[Main] Tracker view already open for: " << jobPath << std::endl;
                return;
            }
        }

        // Create new tracker view
        auto trackerView = std::make_unique<ProjectTrackerView>();
        trackerView->Initialize(jobPath, jobName, &subscriptionManager, nullptr);

        // Set up close callback
        trackerView->onClose = [&trackerViews, jobPath]() {
            for (auto it = trackerViews.begin(); it != trackerViews.end(); ++it) {
                if ((*it) && (*it)->GetJobPath() == jobPath) {
                    trackerViews.erase(it);
                    break;
                }
            }
        };

        // Set up callbacks for opening items in their respective views
        trackerView->onOpenShot = [&openShotViewCallback, &shotViews](const std::wstring& shotPath) {
            // Extract parent category path (e.g., "D:\Job\ae") from shot path (e.g., "D:\Job\ae\shot01")
            std::filesystem::path path(shotPath);
            std::wstring categoryPath = path.parent_path().wstring();
            std::wstring categoryName = path.parent_path().filename().wstring();

            // Open or focus the shot view for this category
            openShotViewCallback(categoryPath, categoryName);

            // Set the selected shot in the newly opened/focused view and focus the window
            for (const auto& sv : shotViews) {
                if (sv && sv->GetCategoryPath() == categoryPath) {
                    sv->SetSelectedShot(shotPath);
                    // Focus the window (window title is "JobName - CategoryName")
                    std::string windowTitle = UFB::WideToUtf8(sv->GetJobName()) + " - " + UFB::WideToUtf8(categoryName);
                    ImGui::SetWindowFocus(windowTitle.c_str());
                    break;
                }
            }
        };

        trackerView->onOpenAsset = [&openAssetsViewCallback, &assetsViews](const std::wstring& assetPath) {
            // Extract parent folder path (e.g., "D:\Job\assets") from asset path (e.g., "D:\Job\assets\asset01")
            std::filesystem::path path(assetPath);
            std::wstring assetsFolderPath = path.parent_path().wstring();
            std::wstring jobName = path.parent_path().parent_path().filename().wstring();

            // Open or focus the assets view for this folder
            openAssetsViewCallback(assetsFolderPath, jobName);

            // Set the selected asset in the newly opened/focused view and focus the window
            for (const auto& av : assetsViews) {
                if (av && av->GetAssetsFolderPath() == assetsFolderPath) {
                    av->SetSelectedAsset(assetPath);
                    // Focus the window (window title is "JobName - Assets")
                    std::string windowTitle = UFB::WideToUtf8(jobName) + " - Assets";
                    ImGui::SetWindowFocus(windowTitle.c_str());
                    break;
                }
            }
        };

        trackerView->onOpenPosting = [&openPostingsViewCallback, &postingsViews](const std::wstring& postingPath) {
            // Extract parent folder path (e.g., "D:\Job\postings") from posting path (e.g., "D:\Job\postings\posting01")
            std::filesystem::path path(postingPath);
            std::wstring postingsFolderPath = path.parent_path().wstring();
            std::wstring jobName = path.parent_path().parent_path().filename().wstring();

            // Open or focus the postings view for this folder
            openPostingsViewCallback(postingsFolderPath, jobName);

            // Set the selected posting in the newly opened/focused view and focus the window
            for (const auto& pv : postingsViews) {
                if (pv && pv->GetPostingsFolderPath() == postingsFolderPath) {
                    pv->SetSelectedPosting(postingPath);
                    // Focus the window (window title is "JobName - Postings")
                    std::string windowTitle = UFB::WideToUtf8(jobName) + " - Postings";
                    ImGui::SetWindowFocus(windowTitle.c_str());
                    break;
                }
            }
        };

        trackerViews.push_back(std::move(trackerView));
        std::wcout << L"[Main] Opened tracker view for: " << jobPath << std::endl;
    };

    // Set up "Open in Other Browser" callbacks
    fileBrowser1.onOpenInOtherBrowser = [&fileBrowser2](const std::wstring& path) {
        fileBrowser2.SetCurrentDirectory(path);
    };

    fileBrowser2.onOpenInOtherBrowser = [&fileBrowser1](const std::wstring& path) {
        fileBrowser1.SetCurrentDirectory(path);
    };

    // Set up "Open in New Window" callback for all browsers
    auto openInNewWindowCallback = [&standaloneBrowsers, &bookmarkManager, &subscriptionManager](const std::wstring& path) {
        // Create new standalone browser
        auto browser = std::make_unique<FileBrowser>();

        // Initialize with shared managers
        browser->Initialize(&bookmarkManager, &subscriptionManager);

        // Set the path
        browser->SetCurrentDirectory(path);

        // Store and log
        standaloneBrowsers.push_back(std::move(browser));
        std::wcout << L"[Main] Opened standalone browser for: " << path << std::endl;
    };

    fileBrowser1.onOpenInNewWindow = openInNewWindowCallback;
    fileBrowser2.onOpenInNewWindow = openInNewWindowCallback;

    // Setup GLFW drop callback for external drag-drop
    struct DropCallbackData {
        FileBrowser* browser1;
        FileBrowser* browser2;
        std::vector<std::unique_ptr<AssetsView>>* assetsViews;
        std::vector<std::unique_ptr<PostingsView>>* postingsViews;
    };
    DropCallbackData dropData = { &fileBrowser1, &fileBrowser2, &assetsViews, &postingsViews };
    glfwSetWindowUserPointer(window, &dropData);

    glfwSetDropCallback(window, [](GLFWwindow* win, int count, const char** paths) {
        auto* data = static_cast<DropCallbackData*>(glfwGetWindowUserPointer(win));
        if (!data || count == 0)
            return;

        // Convert file paths to wide strings
        std::vector<std::wstring> droppedPaths;
        for (int i = 0; i < count; i++)
        {
            int wideSize = MultiByteToWideChar(CP_UTF8, 0, paths[i], -1, nullptr, 0);
            std::wstring widePath(wideSize, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, paths[i], -1, &widePath[0], wideSize);
            widePath.resize(wideSize - 1);
            droppedPaths.push_back(widePath);
        }

        // Use hover state to determine drop target (ImGui already handles coordinate conversion)
        // Check browsers first
        if (data->browser2->IsHovered())
        {
            std::cout << "[Main] Drop into Browser 2" << std::endl;
            data->browser2->HandleExternalDrop(droppedPaths);
            return;
        }
        if (data->browser1->IsHovered())
        {
            std::cout << "[Main] Drop into Browser 1" << std::endl;
            data->browser1->HandleExternalDrop(droppedPaths);
            return;
        }

        // Check assets views
        if (data->assetsViews) {
            for (const auto& assetsView : *data->assetsViews) {
                if (assetsView && assetsView->IsBrowserHovered()) {
                    std::wcout << L"[Main] Drop into Assets View: " << assetsView->GetJobName() << std::endl;
                    assetsView->HandleExternalDrop(droppedPaths);
                    return;
                }
            }
        }

        // Check postings views
        if (data->postingsViews) {
            for (const auto& postingsView : *data->postingsViews) {
                if (postingsView && postingsView->IsBrowserHovered()) {
                    std::wcout << L"[Main] Drop into Postings View: " << postingsView->GetJobName() << std::endl;
                    postingsView->HandleExternalDrop(droppedPaths);
                    return;
                }
            }
        }

        std::cout << "[Main] Drop ignored (no target browser)" << std::endl;
    });

    // Load settings BEFORE creating window so we can use saved dimensions
    // Note: Individual panel toggles no longer used - all panels always shown
    bool unused1 = true, unused2 = true, unused3 = true;  // Temporary for compatibility
    bool showTranscodeQueue = false;
    LoadSettings(unused1, unused2, unused3, showTranscodeQueue);

    // Apply saved window size
    glfwSetWindowSize(window, window_state.width, window_state.height);

    // Apply saved window position if valid
    if (window_state.x >= 0 && window_state.y >= 0) {
        glfwSetWindowPos(window, window_state.x, window_state.y);
    }

    // Apply maximized state
    if (window_state.maximized) {
        glfwMaximizeWindow(window);
    }

    // Apply transcode queue panel visibility state BEFORE loading layout
    // This ensures ImGui can properly restore which tab was active
    if (showTranscodeQueue) {
        transcodeQueuePanel.Show();
        std::cout << "Restored Transcode Queue panel visibility" << std::endl;
    }

    // Load saved ImGui layout if we have one
    if (!saved_imgui_layout.empty()) {
        // Check if the saved layout contains our new unified Browser window
        // If not, discard it and use default layout (architectural change)
        if (saved_imgui_layout.find("[Window][Browser]") != std::string::npos) {
            ImGui::LoadIniSettingsFromMemory(saved_imgui_layout.c_str(), saved_imgui_layout.size());
            std::cout << "Loaded ImGui layout from settings" << std::endl;
        } else {
            std::cout << "Saved layout is outdated (pre-unified Browser), using default layout" << std::endl;
            first_time_setup = true;
            saved_imgui_layout.clear();
        }
    }

    // Restore saved shot views after loading settings
    std::cout << "[Main] Restoring " << saved_shot_views.size() << " saved shot view(s)..." << std::endl;
    for (const auto& savedShotView : saved_shot_views) {
        std::wcout << L"[Main] Checking shot view: " << savedShotView.categoryName << std::endl;
        // Check if the category path still exists
        if (std::filesystem::exists(savedShotView.categoryPath)) {
            std::wcout << L"[Main] Path exists, restoring..." << std::endl;
            openShotViewCallback(savedShotView.categoryPath, savedShotView.categoryName);
            std::wcout << L"[Main] Restored shot view: " << savedShotView.categoryName
                       << L" (" << savedShotView.categoryPath << L")" << std::endl;
        } else {
            std::wcout << L"[Main] Skipping shot view (path no longer exists): "
                       << savedShotView.categoryPath << std::endl;
        }
    }

    // Restore saved assets views
    std::cout << "[Main] Restoring " << saved_assets_views.size() << " saved assets view(s)..." << std::endl;
    for (const auto& savedAssetsView : saved_assets_views) {
        if (std::filesystem::exists(savedAssetsView.assetsFolderPath)) {
            openAssetsViewCallback(savedAssetsView.assetsFolderPath, savedAssetsView.jobName);
            std::wcout << L"[Main] Restored assets view: " << savedAssetsView.jobName
                       << L" - Assets (" << savedAssetsView.assetsFolderPath << L")" << std::endl;
        } else {
            std::wcout << L"[Main] Skipping assets view (path no longer exists): "
                       << savedAssetsView.assetsFolderPath << std::endl;
        }
    }

    // Restore saved postings views
    std::cout << "[Main] Restoring " << saved_postings_views.size() << " saved postings view(s)..." << std::endl;
    for (const auto& savedPostingsView : saved_postings_views) {
        if (std::filesystem::exists(savedPostingsView.postingsFolderPath)) {
            openPostingsViewCallback(savedPostingsView.postingsFolderPath, savedPostingsView.jobName);
            std::wcout << L"[Main] Restored postings view: " << savedPostingsView.jobName
                       << L" - Postings (" << savedPostingsView.postingsFolderPath << L")" << std::endl;
        } else {
            std::wcout << L"[Main] Skipping postings view (path no longer exists): "
                       << savedPostingsView.postingsFolderPath << std::endl;
        }
    }

    // Restore saved tracker views
    std::cout << "[Main] Restoring " << saved_tracker_views.size() << " saved tracker view(s)..." << std::endl;
    for (const auto& savedTrackerView : saved_tracker_views) {
        if (std::filesystem::exists(savedTrackerView.jobPath)) {
            subscriptionPanel.onOpenProjectTracker(savedTrackerView.jobPath, savedTrackerView.jobName);
            std::wcout << L"[Main] Restored tracker view: " << savedTrackerView.jobName
                       << L" - Tracker (" << savedTrackerView.jobPath << L")" << std::endl;
        } else {
            std::wcout << L"[Main] Skipping tracker view (path no longer exists): "
                       << savedTrackerView.jobPath << std::endl;
        }
    }

    // Restore saved standalone browser windows
    std::cout << "[Main] Restoring " << saved_browser_windows.size() << " saved standalone browser(s)..." << std::endl;
    for (const auto& savedBrowser : saved_browser_windows) {
        if (std::filesystem::exists(savedBrowser.currentPath)) {
            openInNewWindowCallback(savedBrowser.currentPath);
            std::wcout << L"[Main] Restored standalone browser: " << savedBrowser.currentPath << std::endl;
        } else {
            std::wcout << L"[Main] Skipping standalone browser (path no longer exists): "
                       << savedBrowser.currentPath << std::endl;
        }
    }

    // Track if we need to focus Browser on first frame
    bool needsInitialBrowserFocus = true;

    // Main loop
    ImVec4 clear_color = ImVec4(0.128f, 0.128f, 0.128f, 1.00f);
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Enable docking over the main viewport
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGui::Begin("DockSpace", nullptr, window_flags);
        ImGui::PopStyleVar(3);

        // DockSpace
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

        // Menu Bar
        if (ImGui::BeginMenuBar())
        {
            // Custom submenu background color (UnionPlayer style)
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));

            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Exit"))
                    glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Reset to Default Layout")) {
                    reset_to_default_layout = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Show Hidden Files", nullptr, &FileBrowser::showHiddenFiles)) {
                    // Toggle for all browsers - static variable shared across instances
                }
                ImGui::Separator();
                bool transcodeQueueOpen = transcodeQueuePanel.IsOpen();
                if (ImGui::MenuItem("Transcode Queue", nullptr, &transcodeQueueOpen)) {
                    transcodeQueuePanel.Toggle();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Windows Accent Color", nullptr, use_windows_accent_color)) {
                    use_windows_accent_color = !use_windows_accent_color;
                    SetupImGuiStyle(); // Reapply style with new accent color
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help"))
            {
                ImGui::BeginDisabled();
                ImGui::MenuItem("u.f.b. v0.1.4", nullptr, false);
                ImGui::EndDisabled();
                ImGui::Separator();

                if (ImGui::MenuItem("About"))
                {
                    ShellExecuteW(nullptr, L"open", L"https://github.com/cbkow/ufb", nullptr, nullptr, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem("License"))
                {
                    ShellExecuteW(nullptr, L"open", L"https://github.com/cbkow/ufb/blob/main/LICENSE", nullptr, nullptr, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem("Check for Updates"))
                {
                    ShellExecuteW(nullptr, L"open", L"https://github.com/cbkow/ufb/releases", nullptr, nullptr, SW_SHOWNORMAL);
                }
                ImGui::EndMenu();
            }

            ImGui::PopStyleColor();
            ImGui::EndMenuBar();
        }

        // Setup default docking layout when needed
        if (first_time_setup || reset_to_default_layout)
        {
            SetupDefaultLayout(dockspace_id, viewport->WorkSize);

            first_time_setup = false;
            reset_to_default_layout = false;
        }

        ImGui::End();

        // Single unified Browser window with three panels side-by-side
        ImGui::Begin("Browser", nullptr, ImGuiWindowFlags_None);

        // Get available content region
        ImVec2 contentRegion = ImGui::GetContentRegionAvail();
        float panelSpacing = 8.0f;  // Space between panels
        ImVec2 windowPos = ImGui::GetCursorScreenPos();

        // Calculate widths for three panels: 20%, 40%, 40%
        float leftWidth = contentRegion.x * 0.20f - panelSpacing;
        float middleWidth = contentRegion.x * 0.40f - panelSpacing;
        float rightWidth = contentRegion.x * 0.40f;

        // Panel 1 - Subscriptions (no border)
        ImGui::BeginChild("SubscriptionsPanel", ImVec2(leftWidth, 0), false);
        subscriptionPanel.Draw("Subscriptions", false);
        ImGui::EndChild();

        ImGui::SameLine(0.0f, panelSpacing);

        // Draw first vertical separator line
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float line1_x = windowPos.x + leftWidth + panelSpacing / 2.0f;
        ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        drawList->AddLine(ImVec2(line1_x, windowPos.y), ImVec2(line1_x, windowPos.y + contentRegion.y), lineColor, 1.0f);

        // Panel 2 - File Browser 1 (no border)
        ImGui::BeginChild("Browser1Panel", ImVec2(middleWidth, 0), false);
        fileBrowser1.Draw("File Browser 1", hwnd, false);
        ImGui::EndChild();

        ImGui::SameLine(0.0f, panelSpacing);

        // Draw second vertical separator line
        float line2_x = windowPos.x + leftWidth + panelSpacing + middleWidth + panelSpacing / 2.0f;
        drawList->AddLine(ImVec2(line2_x, windowPos.y), ImVec2(line2_x, windowPos.y + contentRegion.y), lineColor, 1.0f);

        // Panel 3 - File Browser 2 (no border)
        ImGui::BeginChild("Browser2Panel", ImVec2(rightWidth, 0), false);
        fileBrowser2.Draw("File Browser 2", hwnd, false);

        ImGui::EndChild();

        ImGui::End();

        // Transcode Queue Panel (dockable window)
        transcodeQueuePanel.Update();  // Process jobs

        // Dock into main dockspace on first use
        ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);

        transcodeQueuePanel.Render();  // Render UI

        // Render all shot views (dockable windows)
        for (size_t i = 0; i < shotViews.size(); ++i) {
            if (shotViews[i]) {
                // Format: "JobName - CategoryName" (e.g., "000000_OH - ae")
                std::string windowTitle = UFB::WideToUtf8(shotViews[i]->GetJobName()) + " - " + UFB::WideToUtf8(shotViews[i]->GetCategoryName());

                // Dock into main dockspace on first use
                ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);

                shotViews[i]->Draw(windowTitle.c_str(), hwnd);
            }
        }

        // Render all assets views (dockable windows)
        for (size_t i = 0; i < assetsViews.size(); ++i) {
            if (assetsViews[i]) {
                // Format: "JobName - Assets" (e.g., "MyJob - Assets")
                std::string windowTitle = UFB::WideToUtf8(assetsViews[i]->GetJobName()) + " - Assets";

                // Dock into main dockspace on first use
                ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);

                assetsViews[i]->Draw(windowTitle.c_str(), hwnd);
            }
        }

        // Render all postings views (dockable windows)
        for (size_t i = 0; i < postingsViews.size(); ++i) {
            if (postingsViews[i]) {
                // Format: "JobName - Postings" (e.g., "MyJob - Postings")
                std::string windowTitle = UFB::WideToUtf8(postingsViews[i]->GetJobName()) + " - Postings";

                // Dock into main dockspace on first use
                ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);

                postingsViews[i]->Draw(windowTitle.c_str(), hwnd);
            }
        }

        // Render all tracker views (dockable windows)
        for (size_t i = 0; i < trackerViews.size(); ++i) {
            if (trackerViews[i]) {
                // Format: "JobName - Tracker" (e.g., "MyJob - Tracker")
                std::string windowTitle = UFB::WideToUtf8(trackerViews[i]->GetJobName()) + " - Tracker";

                // Dock into main dockspace on first use
                ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);

                trackerViews[i]->Draw(windowTitle.c_str(), hwnd);
            }
        }

        // Render all standalone browser windows (dockable windows)
        for (size_t i = 0; i < standaloneBrowsers.size(); ++i) {
            if (standaloneBrowsers[i]) {
                // Get current directory path for window title
                std::wstring currentPath = standaloneBrowsers[i]->GetCurrentDirectory();
                std::filesystem::path path(currentPath);
                std::string windowTitle = "Browser - " + UFB::WideToUtf8(path.filename().wstring());

                // Dock into main dockspace on first use
                ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);

                // Draw the browser (single panel, with window)
                standaloneBrowsers[i]->Draw(windowTitle.c_str(), hwnd, true);
            }
        }

        // Remove closed standalone browser windows
        standaloneBrowsers.erase(
            std::remove_if(standaloneBrowsers.begin(), standaloneBrowsers.end(),
                [](const std::unique_ptr<FileBrowser>& browser) {
                    return browser && !browser->IsOpen();
                }),
            standaloneBrowsers.end()
        );

        // Always focus Browser tab on first frame
        if (needsInitialBrowserFocus) {
            ImGui::SetWindowFocus("Browser");
            needsInitialBrowserFocus = false;
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                     clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        glfwSwapBuffers(window);
    }

    // Save settings before cleanup
    // Save settings including transcode queue panel state and open views
    SaveSettings(window, true, true, true, transcodeQueuePanel.IsOpen(), shotViews, assetsViews, postingsViews, trackerViews, standaloneBrowsers);

    // Cleanup
    try
    {
        std::cout << "Shutting down SyncManager..." << std::endl;
        syncManager.Shutdown();

        std::cout << "Shutting down FileBrowser 1..." << std::endl;
        fileBrowser1.Shutdown();

        std::cout << "Shutting down FileBrowser 2..." << std::endl;
        fileBrowser2.Shutdown();

        std::cout << "Shutting down ImGui OpenGL..." << std::endl;
        ImGui_ImplOpenGL3_Shutdown();

        std::cout << "Shutting down ImGui GLFW..." << std::endl;
        ImGui_ImplGlfw_Shutdown();

        std::cout << "Destroying ImGui context..." << std::endl;
        ImGui::DestroyContext();

        std::cout << "Destroying GLFW window..." << std::endl;
        glfwDestroyWindow(window);

        std::cout << "Terminating GLFW..." << std::endl;
        glfwTerminate();

        std::cout << "Cleanup complete" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Cleanup unknown exception" << std::endl;
    }

    return 0;
}
