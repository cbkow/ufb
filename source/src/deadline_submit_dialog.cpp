#include "deadline_submit_dialog.h"
#include "utils.h"
#include <iostream>
#include <sstream>

DeadlineSubmitDialog::DeadlineSubmitDialog()
{
}

DeadlineSubmitDialog::~DeadlineSubmitDialog()
{
}

void DeadlineSubmitDialog::Show(const std::wstring& blendFilePath, const std::wstring& jobName)
{
    m_blendFilePath = blendFilePath;
    m_jobName = jobName;
    m_shouldOpen = true;
}

void DeadlineSubmitDialog::Draw()
{
    // Handle delayed opening (to avoid opening in same frame as menu click)
    if (m_shouldOpen)
    {
        ImGui::SetNextWindowSize(ImVec2(376, 360), ImGuiCond_Always);

        // Center the modal on screen
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (viewport)
        {
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }

        ImGui::OpenPopup("Submit Blender Job");
        m_isOpen = true;
        m_shouldOpen = false;
    }

    // Draw the modal dialog with darkened background
    if (ImGui::BeginPopupModal("Submit Blender Job", &m_isOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::Spacing();
        ImGui::Spacing();

        // Frame Start
        ImGui::Text("Frame Start:");
        ImGui::SetNextItemWidth(350.0f);
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("##framestart", m_frameStart, sizeof(m_frameStart));

        ImGui::Spacing();
        ImGui::Spacing();

        // Frame End
        ImGui::Text("Frame End:");
        ImGui::SetNextItemWidth(350.0f);
        ImGui::InputText("##frameend", m_frameEnd, sizeof(m_frameEnd));

        ImGui::Spacing();
        ImGui::Spacing();

        // Chunk Size
        ImGui::Text("Chunk Size:");
        ImGui::SetNextItemWidth(350.0f);
        bool enterPressed = ImGui::InputText("##chunksize", m_chunkSize, sizeof(m_chunkSize), ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        // Buttons
        bool doSubmit = false;
        if (ImGui::Button("Submit Job", ImVec2(350, 0)) || enterPressed)
        {
            doSubmit = true;
        }

        // Handle submission
        if (doSubmit)
        {
            // Validate and parse inputs
            std::string startStr(m_frameStart);
            std::string endStr(m_frameEnd);
            std::string chunkSizeStr(m_chunkSize);

            // Trim whitespace
            startStr.erase(0, startStr.find_first_not_of(" \t"));
            startStr.erase(startStr.find_last_not_of(" \t") + 1);
            endStr.erase(0, endStr.find_first_not_of(" \t"));
            endStr.erase(endStr.find_last_not_of(" \t") + 1);
            chunkSizeStr.erase(0, chunkSizeStr.find_first_not_of(" \t"));
            chunkSizeStr.erase(chunkSizeStr.find_last_not_of(" \t") + 1);

            if (startStr.empty() || endStr.empty() || chunkSizeStr.empty())
            {
                std::cerr << "[DeadlineSubmitDialog] Invalid frame range or chunk size" << std::endl;
                ImGui::EndPopup();
                return;
            }

            try
            {
                int frameStart = std::stoi(startStr);
                int frameEnd = std::stoi(endStr);
                int chunkSize = std::stoi(chunkSizeStr);

                // Create DeadlineJob
                DeadlineJob job;
                job.blendFilePath = m_blendFilePath;
                job.jobName = m_jobName;
                job.frameStart = frameStart;
                job.frameEnd = frameEnd;
                job.chunkSize = chunkSize;
                job.pool = "c4d";      // Hardcoded like old app
                job.priority = 50;     // Hardcoded like old app
                job.status = DeadlineJobStatus::QUEUED;

                // Notify callback
                if (onJobSubmitted)
                {
                    onJobSubmitted(job);
                }

                m_isOpen = false;
                ImGui::CloseCurrentPopup();
            }
            catch (const std::exception& e)
            {
                std::cerr << "[DeadlineSubmitDialog] Failed to parse frame range or chunk size: " << e.what() << std::endl;
            }
        }

        ImGui::EndPopup();
    }

    // Handle close from X button
    if (!m_isOpen)
    {
        // Dialog was closed
    }
}
