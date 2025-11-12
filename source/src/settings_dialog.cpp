#include "settings_dialog.h"
#include "imgui.h"
#include "utils.h"
#include <cstring>
#include <iostream>

SettingsDialog::SettingsDialog()
{
}

SettingsDialog::~SettingsDialog()
{
}

void SettingsDialog::Show()
{
    m_shouldOpen = true;
}

void SettingsDialog::RefreshValues(float currentScale, const std::string& currentApiKey,
                                    const std::string& operatingMode, const std::string& trackingDirectory,
                                    const std::string& googleClientId, const std::string& googleClientSecret,
                                    bool googleSheetsEnabled, const std::string& masterSpreadsheetId,
                                    const std::string& parentFolderId, const std::string& authStatus)
{
    m_fontScale = currentScale;
    m_frameioApiKey = currentApiKey;
    m_operatingMode = operatingMode;
    m_clientTrackingDirectory = trackingDirectory;
    m_googleClientId = googleClientId;
    m_googleClientSecret = googleClientSecret;
    m_googleSheetsEnabled = googleSheetsEnabled;
    m_masterSpreadsheetId = masterSpreadsheetId;
    m_parentFolderId = parentFolderId;
    m_googleAuthStatus = authStatus;
}

void SettingsDialog::Draw()
{
    //static int frameCount = 0;
    //frameCount++;
    //if (frameCount % 60 == 0) {  // Log every 60 frames to avoid spam
    //    std::cout << "[SettingsDialog::Draw] Called (frame " << frameCount << "), m_isOpen=" << (m_isOpen ? "true" : "false") << ", m_shouldOpen=" << (m_shouldOpen ? "true" : "false") << std::endl;
    //}

    // Handle delayed opening
    if (m_shouldOpen)
    {
        std::cout << "[SettingsDialog] Opening settings dialog..." << std::endl;
        ImGui::OpenPopup("Settings");
        m_shouldOpen = false;
        m_isOpen = true;

        // Center the modal
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 work_pos = viewport->WorkPos;
        ImVec2 work_size = viewport->WorkSize;
        ImVec2 center = ImVec2(work_pos.x + work_size.x * 0.5f, work_pos.y + work_size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        // Initialize API key buffer from stored value
        strncpy_s(m_apiKeyBuffer, m_frameioApiKey.c_str(), sizeof(m_apiKeyBuffer) - 1);
        m_apiKeyBuffer[sizeof(m_apiKeyBuffer) - 1] = '\0';

        // Initialize tracking directory buffer from stored value
        strncpy_s(m_trackingDirBuffer, m_clientTrackingDirectory.c_str(), sizeof(m_trackingDirBuffer) - 1);
        m_trackingDirBuffer[sizeof(m_trackingDirBuffer) - 1] = '\0';

        // Initialize Google Sheets buffers from stored values
        strncpy_s(m_googleClientIdBuffer, m_googleClientId.c_str(), sizeof(m_googleClientIdBuffer) - 1);
        m_googleClientIdBuffer[sizeof(m_googleClientIdBuffer) - 1] = '\0';

        strncpy_s(m_googleClientSecretBuffer, m_googleClientSecret.c_str(), sizeof(m_googleClientSecretBuffer) - 1);
        m_googleClientSecretBuffer[sizeof(m_googleClientSecretBuffer) - 1] = '\0';

        strncpy_s(m_masterSpreadsheetIdBuffer, m_masterSpreadsheetId.c_str(), sizeof(m_masterSpreadsheetIdBuffer) - 1);
        m_masterSpreadsheetIdBuffer[sizeof(m_masterSpreadsheetIdBuffer) - 1] = '\0';

        strncpy_s(m_parentFolderIdBuffer, m_parentFolderId.c_str(), sizeof(m_parentFolderIdBuffer) - 1);
        m_parentFolderIdBuffer[sizeof(m_parentFolderIdBuffer) - 1] = '\0';
    }

    // Modal settings
    ImGui::SetNextWindowSize(ImVec2(750, 1100), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Settings", &m_isOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::Spacing();

        // Font Size Section
        DrawFontSizeSection();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Font Preview Section
        DrawFontPreview();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Frame.io API Key Section
        DrawFrameioSection();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Operating Mode Section
        DrawOperatingModeSection();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Google Sheets Section
        DrawGoogleSheetsSection();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Bottom buttons
        float buttonWidth = 120.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float totalWidth = buttonWidth * 2 + spacing;
        float offsetX = (ImGui::GetContentRegionAvail().x - totalWidth) * 0.5f;

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);

        if (ImGui::Button("Save", ImVec2(buttonWidth, 0)))
        {
            // Update API key from buffer
            m_frameioApiKey = std::string(m_apiKeyBuffer);

            // Update tracking directory from buffer
            m_clientTrackingDirectory = std::string(m_trackingDirBuffer);

            // Update Google Sheets settings from buffers
            m_googleClientId = std::string(m_googleClientIdBuffer);
            m_googleClientSecret = std::string(m_googleClientSecretBuffer);
            m_masterSpreadsheetId = std::string(m_masterSpreadsheetIdBuffer);
            m_parentFolderId = std::string(m_parentFolderIdBuffer);

            // Trigger callback to save settings
            if (onSettingsSaved)
            {
                onSettingsSaved();
            }

            ImGui::CloseCurrentPopup();
            m_isOpen = false;
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)))
        {
            ImGui::CloseCurrentPopup();
            m_isOpen = false;
        }

        ImGui::EndPopup();
    }
    else
    {
        m_isOpen = false;
    }
}

void SettingsDialog::DrawFontSizeSection()
{
    ImGui::TextUnformatted("Font Size");
    ImGui::Spacing();

    // Preset buttons
    ImGui::Text("Presets:");
    ImGui::SameLine();

    if (ImGui::Button("Small"))
    {
        m_fontScale = FONT_SCALE_SMALL;
    }
    ImGui::SameLine();

    if (ImGui::Button("Medium"))
    {
        m_fontScale = FONT_SCALE_MEDIUM;
    }
    ImGui::SameLine();

    if (ImGui::Button("Large"))
    {
        m_fontScale = FONT_SCALE_LARGE;
    }
    ImGui::SameLine();

    if (ImGui::Button("X-Large"))
    {
        m_fontScale = FONT_SCALE_XLARGE;
    }

    ImGui::Spacing();

    // Slider for fine control
    ImGui::Text("Custom Scale:");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##fontscale", &m_fontScale, 0.5f, 2.0f, "%.2fx");

    ImGui::Spacing();
    ImGui::TextDisabled("(Changes apply immediately when you click Save)");
}

void SettingsDialog::DrawFontPreview()
{
    ImGui::TextUnformatted("Font Preview");
    ImGui::Spacing();

    // Save current font scale
    float originalScale = ImGui::GetIO().FontGlobalScale;

    // Preview box with border - apply scale only inside this child window
    ImGui::BeginChild("FontPreview", ImVec2(-1, 120), true);

    // Apply preview scale only within this child window
    ImGui::GetIO().FontGlobalScale = m_fontScale;

    // Regular font preview
    if (m_fontRegular) {
        ImGui::PushFont(m_fontRegular);
        ImGui::Text("Regular Font: The quick brown fox jumps over the lazy dog");
        ImGui::PopFont();
    } else {
        ImGui::Text("Regular Font: The quick brown fox jumps over the lazy dog");
    }

    ImGui::Spacing();

    // Mono font preview
    if (m_fontMono) {
        ImGui::PushFont(m_fontMono);
        ImGui::Text("Mono Font: function main() { return 0; }");
        ImGui::PopFont();
    } else {
        ImGui::Text("Mono Font: function main() { return 0; }");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Scale: %.2fx", m_fontScale);

    // Restore original scale BEFORE ending child window
    ImGui::GetIO().FontGlobalScale = originalScale;

    ImGui::EndChild();
}

void SettingsDialog::DrawFrameioSection()
{
    ImGui::TextUnformatted("Frame.io API Key");
    ImGui::Spacing();

    ImGui::Text("API Key:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##frameio_api_key", m_apiKeyBuffer, sizeof(m_apiKeyBuffer), ImGuiInputTextFlags_Password);

    ImGui::Spacing();

    if (ImGui::Button("Clear API Key"))
    {
        memset(m_apiKeyBuffer, 0, sizeof(m_apiKeyBuffer));
        m_frameioApiKey.clear();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("API key is stored locally with base64 encoding.");
}

void SettingsDialog::DrawOperatingModeSection()
{
    ImGui::TextUnformatted("Operating Mode");
    ImGui::Spacing();

    // Mode selection radio buttons
    bool isClient = (m_operatingMode == "client");
    bool isServer = (m_operatingMode == "server");

    if (ImGui::RadioButton("Client Mode (default)", isClient))
    {
        m_operatingMode = "client";
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text("Client mode: App writes its own tracking file when jobs are synced/unsynced.");
        ImGui::EndTooltip();
    }

    if (ImGui::RadioButton("Server Mode", isServer))
    {
        m_operatingMode = "server";
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text("Server mode: App reads all client tracking files and syncs to their union.");
        ImGui::Text("Unsyncing a job will remove it from ALL client tracking files.");
        ImGui::EndTooltip();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Tracking directory
    ImGui::Text("Client Tracking Directory:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##trackingdir", m_trackingDirBuffer, sizeof(m_trackingDirBuffer));
    ImGui::TextDisabled("Example: Z:\\UFB-Central\\tracking");

    ImGui::Spacing();

    // Warning for server mode
    if (m_operatingMode == "server")
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.0f, 1.0f));
        ImGui::TextWrapped("WARNING: Server Mode - Unsyncing a job will remove it from ALL client tracking files.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Client tracking files are used to coordinate job subscriptions across multiple machines.");
}

void SettingsDialog::DrawGoogleSheetsSection()
{
    ImGui::TextUnformatted("Google Sheets Integration");
    ImGui::Spacing();

    // Check if client mode - Google Sheets only works in server mode
    if (m_operatingMode == "client")
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.0f, 1.0f));
        ImGui::TextWrapped("Google Sheets integration is only available in Server mode.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextDisabled("Switch to Server mode in the Operating Mode section to use Google Sheets.");
        return;
    }

    // Enable/Disable checkbox
    ImGui::Checkbox("Enable Google Sheets Sync", &m_googleSheetsEnabled);
    ImGui::Spacing();

    if (!m_googleSheetsEnabled)
    {
        ImGui::TextDisabled("(Enable to configure Google Sheets integration)");
        return;
    }

    ImGui::Separator();
    ImGui::Spacing();

    // Authentication status
    ImGui::Text("Authentication Status:");
    ImGui::SameLine();
    if (m_googleAuthStatus == "Authenticated")
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
        ImGui::Text("Authenticated");
        ImGui::PopStyleColor();
    }
    else if (m_googleAuthStatus == "Authenticating")
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
        ImGui::Text("Authenticating...");
        ImGui::PopStyleColor();
    }
    else if (m_googleAuthStatus == "Failed")
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        ImGui::Text("Failed");
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("Not Authenticated");
    }

    ImGui::Spacing();

    // OAuth Client ID
    ImGui::Text("Google OAuth Client ID:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##google_client_id", m_googleClientIdBuffer, sizeof(m_googleClientIdBuffer));
    ImGui::TextDisabled("From Google Cloud Console > APIs & Services > Credentials");

    ImGui::Spacing();

    // OAuth Client Secret
    ImGui::Text("Google OAuth Client Secret:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##google_client_secret", m_googleClientSecretBuffer, sizeof(m_googleClientSecretBuffer), ImGuiInputTextFlags_Password);
    ImGui::TextDisabled("Keep this secret!");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Login/Logout buttons
    if (m_googleAuthStatus == "Authenticated")
    {
        if (ImGui::Button("Logout", ImVec2(120, 0)))
        {
            if (onGoogleLogout)
            {
                onGoogleLogout();
            }
        }
    }
    else
    {
        if (ImGui::Button("Login with Google", ImVec2(160, 0)))
        {
            if (onGoogleLogin)
            {
                onGoogleLogin();
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");;
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("This will open a browser window for Google authentication.");
            ImGui::Text("Make sure Client ID and Secret are saved first!");
            ImGui::EndTooltip();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Parent Folder ID (now REQUIRED)
    ImGui::Text("Parent Folder ID:");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
    ImGui::Text("REQUIRED");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");;
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text("Paste a Google Drive folder ID where job folders will be created.");
        ImGui::Text("Works for both 'My Drive' folders and Shared Drives.");
        ImGui::Text("Get folder ID from URL: drive.google.com/drive/folders/[FOLDER_ID]");
        ImGui::Text("");
        ImGui::Text("Example structure:");
        ImGui::Text("  Parent Folder -> Job Folders -> Spreadsheets (4 sheets each)");
        ImGui::EndTooltip();
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##parent_folder_id", m_parentFolderIdBuffer, sizeof(m_parentFolderIdBuffer));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Master Spreadsheet ID (DEPRECATED)
    ImGui::TextDisabled("Master Spreadsheet ID (deprecated):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##master_spreadsheet_id", m_masterSpreadsheetIdBuffer, sizeof(m_masterSpreadsheetIdBuffer));
    ImGui::TextDisabled("This field is no longer used - each job gets its own spreadsheet");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Reset errors button
    if (ImGui::Button("Reset All Sync Errors", ImVec2(180, 0)))
    {
        if (onResetGoogleSheetsErrors)
        {
            onResetGoogleSheetsErrors();
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");;
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text("Re-enables all jobs that were disabled due to sync errors");
        ImGui::Text("and resets the global failure counter.");
        ImGui::EndTooltip();
    }

    ImGui::Spacing();

    // Full Reset button (with confirmation dialog)
    if (ImGui::Button("Full Reset", ImVec2(180, 0)))
    {
        ImGui::OpenPopup("ConfirmFullReset");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text("Deletes ALL sync records and cached data.");
        ImGui::Text("Use this to start fresh if you moved folders or changed structure.");
        ImGui::EndTooltip();
    }

    // Confirmation modal for Full Reset
    if (ImGui::BeginPopupModal("ConfirmFullReset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("WARNING: This will delete ALL Google Sheets sync records, including:");
        ImGui::Spacing();
        ImGui::BulletText("All spreadsheet IDs");
        ImGui::BulletText("All job folder IDs");
        ImGui::BulletText("All sheet IDs");
        ImGui::BulletText("All sync timestamps");
        ImGui::BulletText("All error counters");
        ImGui::Spacing();
        ImGui::TextWrapped("You will need to re-sync all jobs from scratch.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "This action cannot be undone!");
        ImGui::Spacing();

        if (ImGui::Button("Yes, Delete Everything", ImVec2(200, 0)))
        {
            if (onFullResetGoogleSheets)
            {
                onFullResetGoogleSheets();
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Info text
    ImGui::TextWrapped("NEW ARCHITECTURE: Each job gets its own folder containing a spreadsheet with 4 sheets: Shots, Assets, Postings, and Tasks.");
    ImGui::Spacing();
    ImGui::TextWrapped("Structure: Parent Folder → Job Folders → Spreadsheet (with 4 sheets)");
    ImGui::Spacing();
    ImGui::TextDisabled("Sync interval: 60 seconds (when authenticated, enabled, and parent folder set)");
    ImGui::Spacing();
    ImGui::TextDisabled("Error limits: 5 consecutive failures per job → job disabled, 3 global failure cycles → sync stopped");
}

