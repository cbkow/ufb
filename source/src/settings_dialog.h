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
    void RefreshValues(float currentScale, const std::string& currentApiKey);

    // Callback when settings are saved
    std::function<void()> onSettingsSaved;

    // Set initial values from loaded settings
    void SetFontScale(float scale) { m_fontScale = scale; }
    void SetFrameioApiKey(const std::string& apiKey) { m_frameioApiKey = apiKey; }
    void SetFonts(ImFont* regular, ImFont* mono) { m_fontRegular = regular; m_fontMono = mono; }

    // Get current values
    float GetFontScale() const { return m_fontScale; }
    std::string GetFrameioApiKey() const { return m_frameioApiKey; }

private:
    bool m_isOpen = false;
    bool m_shouldOpen = false;  // Delayed opening flag

    // Settings values (local copies for editing)
    float m_fontScale = 1.0f;
    char m_apiKeyBuffer[256] = "";  // ImGui input buffer
    std::string m_frameioApiKey;  // Actual stored API key

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
};
