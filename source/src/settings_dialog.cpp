#include "settings_dialog.h"
#include "imgui.h"
#include "utils.h"
#include <cstring>

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

void SettingsDialog::RefreshValues(float currentScale, const std::string& currentApiKey)
{
    m_fontScale = currentScale;
    m_frameioApiKey = currentApiKey;
}

void SettingsDialog::Draw()
{
    // Handle delayed opening
    if (m_shouldOpen)
    {
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
    }

    // Modal settings
    ImGui::SetNextWindowSize(ImVec2(600, 730), ImGuiCond_Always);

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
