#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "imgui.h"
#include "deadline_queue_panel.h"

class DeadlineSubmitDialog
{
public:
    DeadlineSubmitDialog();
    ~DeadlineSubmitDialog();

    // Show the dialog for submitting a .blend file
    void Show(const std::wstring& blendFilePath, const std::wstring& jobName);

    // Draw the dialog (call every frame)
    void Draw();

    // Check if dialog is open
    bool IsOpen() const { return m_isOpen; }

    // Callback when job is submitted
    std::function<void(const DeadlineJob&)> onJobSubmitted;

private:
    // Dialog state
    bool m_isOpen = false;
    bool m_shouldOpen = false;  // Flag to open on next frame

    // File being submitted
    std::wstring m_blendFilePath;
    std::wstring m_jobName;

    // Render parameters (user editable)
    char m_frameStart[32] = "1";      // Start frame
    char m_frameEnd[32] = "250";      // End frame
    char m_chunkSize[32] = "10";      // Chunk size as string
};
