#pragma once

#include <string>
#include <functional>
#include "imgui.h"

class SettingsDialog
{
public:
    SettingsDialog();
    ~SettingsDialog();

    void Show();  // Trigger dialog to open
    void Draw();  // Call every frame in main loop
    bool IsOpen() const { return m_isOpen; }

    // Refresh current values from globals
    void RefreshValues(float currentScale, const std::string& currentApiKey,
                       const std::string& operatingMode, const std::string& trackingDirectory,
                       const std::string& googleClientId, const std::string& googleClientSecret,
                       bool googleSheetsEnabled, const std::string& masterSpreadsheetId,
                       const std::string& parentFolderId, const std::string& authStatus);

    // Callback when settings are saved
    std::function<void()> onSettingsSaved;

    // Set initial values from loaded settings
    void SetFontScale(float scale) { m_fontScale = scale; }
    void SetFrameioApiKey(const std::string& apiKey) { m_frameioApiKey = apiKey; }
    void SetOperatingMode(const std::string& mode) { m_operatingMode = mode; }
    void SetClientTrackingDirectory(const std::string& dir) { m_clientTrackingDirectory = dir; }
    void SetFonts(ImFont* regular, ImFont* mono) { m_fontRegular = regular; m_fontMono = mono; }
    void SetGoogleClientId(const std::string& clientId) { m_googleClientId = clientId; }
    void SetGoogleClientSecret(const std::string& clientSecret) { m_googleClientSecret = clientSecret; }
    void SetGoogleSheetsEnabled(bool enabled) { m_googleSheetsEnabled = enabled; }
    void SetMasterSpreadsheetId(const std::string& id) { m_masterSpreadsheetId = id; }
    void SetParentFolderId(const std::string& id) { m_parentFolderId = id; }
    void SetGoogleAuthStatus(const std::string& status) { m_googleAuthStatus = status; }

    // Get current values
    float GetFontScale() const { return m_fontScale; }
    std::string GetFrameioApiKey() const { return m_frameioApiKey; }
    std::string GetOperatingMode() const { return m_operatingMode; }
    std::string GetClientTrackingDirectory() const { return m_clientTrackingDirectory; }
    std::string GetGoogleClientId() const { return m_googleClientId; }
    std::string GetGoogleClientSecret() const { return m_googleClientSecret; }
    bool GetGoogleSheetsEnabled() const { return m_googleSheetsEnabled; }
    std::string GetMasterSpreadsheetId() const { return m_masterSpreadsheetId; }
    std::string GetParentFolderId() const { return m_parentFolderId; }

    // Callbacks for Google Sheets actions
    std::function<void()> onGoogleLogin;
    std::function<void()> onGoogleLogout;
    std::function<void()> onCreateMasterSpreadsheet;
    std::function<void()> onResetGoogleSheetsErrors;
    std::function<void()> onFullResetGoogleSheets;

private:
    bool m_isOpen = false;
    bool m_shouldOpen = false;  // Delayed opening flag

    // Settings values (local copies for editing)
    float m_fontScale = 1.0f;
    char m_apiKeyBuffer[256] = "";  // ImGui input buffer
    std::string m_frameioApiKey;  // Actual stored API key
    std::string m_operatingMode = "client";  // "client" or "server"
    char m_trackingDirBuffer[512] = "";  // ImGui input buffer
    std::string m_clientTrackingDirectory;  // Actual stored directory

    // Google Sheets settings
    char m_googleClientIdBuffer[512] = "";
    char m_googleClientSecretBuffer[512] = "";
    char m_masterSpreadsheetIdBuffer[512] = "";
    char m_parentFolderIdBuffer[512] = "";
    std::string m_googleClientId;
    std::string m_googleClientSecret;
    std::string m_masterSpreadsheetId;
    std::string m_parentFolderId;
    std::string m_googleAuthStatus;
    bool m_googleSheetsEnabled = false;

    // Font pointers for preview
    ImFont* m_fontRegular = nullptr;
    ImFont* m_fontMono = nullptr;

    // Font size preset values
    static constexpr float FONT_SCALE_SMALL = 0.75f;
    static constexpr float FONT_SCALE_MEDIUM = 1.0f;
    static constexpr float FONT_SCALE_LARGE = 1.25f;
    static constexpr float FONT_SCALE_XLARGE = 1.5f;

    // Draw sections
    void DrawFontSizeSection();
    void DrawFontPreview();
    void DrawFrameioSection();
    void DrawOperatingModeSection();
    void DrawGoogleSheetsSection();
};
