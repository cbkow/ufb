#include "deadline_queue_panel.h"
#include "utils.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <algorithm>

// External accent color function (from main.cpp)
extern ImVec4 GetWindowsAccentColor();

DeadlineQueuePanel::DeadlineQueuePanel()
{
}

DeadlineQueuePanel::~DeadlineQueuePanel()
{
    Shutdown();
}

void DeadlineQueuePanel::Initialize()
{
    std::cout << "[DeadlineQueuePanel] Initializing..." << std::endl;

    // Initialize poll timer
    m_lastPollTime = std::chrono::steady_clock::now();

    // Find deadlinecommand.exe
    if (!FindDeadlineCommand())
    {
        std::cerr << "[DeadlineQueuePanel] Warning: deadlinecommand.exe not found in PATH" << std::endl;
    }
    else
    {
        std::wcout << L"[DeadlineQueuePanel] Found deadlinecommand.exe at: " << m_deadlineCommandPath << std::endl;
    }
}

void DeadlineQueuePanel::Shutdown()
{
    std::cout << "[DeadlineQueuePanel] Shutting down..." << std::endl;

    std::lock_guard<std::mutex> lock(m_jobsMutex);
    m_jobs.clear();
}

bool DeadlineQueuePanel::FindDeadlineCommand()
{
    // Try common Deadline installation paths
    std::vector<std::wstring> possiblePaths = {
        L"C:\\Program Files\\Thinkbox\\Deadline10\\bin\\deadlinecommand.exe",
        L"C:\\Program Files\\Thinkbox\\Deadline\\bin\\deadlinecommand.exe",
        L"deadlinecommand.exe"  // Try PATH
    };

    for (const auto& path : possiblePaths)
    {
        if (path == L"deadlinecommand.exe")
        {
            // Check if it's in PATH by trying to execute with -help
            std::string output;
            if (ExecuteDeadlineCommand(L"-help", output))
            {
                m_deadlineCommandPath = path;
                return true;
            }
        }
        else
        {
            // Check if file exists
            if (std::filesystem::exists(path))
            {
                m_deadlineCommandPath = path;
                return true;
            }
        }
    }

    return false;
}

void DeadlineQueuePanel::Draw(const char* title)
{
    if (!m_isOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(title, &m_isOpen))
    {
        RenderToolbar();
        ImGui::Separator();

        // Queue table (takes remaining space minus details panel)
        float availableHeight = ImGui::GetContentRegionAvail().y;
        float tableHeight = availableHeight - m_detailsPanelHeight - 10.0f;

        ImGui::BeginChild("QueueTableRegion", ImVec2(0, tableHeight), true, ImGuiWindowFlags_NoScrollbar);
        DrawJobsTable();
        ImGui::EndChild();

        ImGui::Separator();

        // Details panel at bottom
        ImGui::BeginChild("DetailsPanel", ImVec2(0, m_detailsPanelHeight), true, ImGuiWindowFlags_NoScrollbar);
        RenderJobDetailsPanel();
        ImGui::EndChild();
    }

    ImGui::End();

    // Handle deferred cancel operation (after rendering to avoid mutex issues)
    if (m_jobIndexToCancel >= 0)
    {
        CancelJob(m_jobIndexToCancel);
        m_jobIndexToCancel = -1;
    }
}

void DeadlineQueuePanel::RenderToolbar()
{
    // Status indicator
    const char* statusText = m_isProcessing ? "PROCESSING" : "IDLE";
    ImVec4 accentColor = GetWindowsAccentColor();
    ImVec4 statusColor = m_isProcessing ? accentColor : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

    ImGui::TextColored(statusColor, "STATUS: %s", statusText);
    ImGui::SameLine();

    // Statistics
    std::lock_guard<std::mutex> lock(m_jobsMutex);
    size_t total = m_jobs.size();
    size_t queued = 0;
    size_t rendering = 0;
    size_t completed = 0;
    size_t failed = 0;
    size_t cancelled = 0;

    for (const auto& job : m_jobs)
    {
        switch (job.status)
        {
            case DeadlineJobStatus::QUEUED:
            case DeadlineJobStatus::SUBMITTING:
                queued++;
                break;
            case DeadlineJobStatus::RENDERING:
            case DeadlineJobStatus::SUBMITTED:
                rendering++;
                break;
            case DeadlineJobStatus::COMPLETED:
                completed++;
                break;
            case DeadlineJobStatus::FAILED:
                failed++;
                break;
            case DeadlineJobStatus::CANCELLED:
                cancelled++;
                break;
        }
    }

    ImGui::Text(" | Total: %zu  Queued: %zu  Rendering: %zu  Completed: %zu  Failed: %zu  Cancelled: %zu",
                total, queued, rendering, completed, failed, cancelled);

    ImGui::SameLine(ImGui::GetWindowWidth() - 320);

    // Control buttons
    if (ImGui::Button("Clear Completed", ImVec2(140, 0)))
    {
        ClearCompleted();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All", ImVec2(130, 0)))
    {
        ClearAll();
    }
}

void DeadlineQueuePanel::DrawJobsTable()
{
    std::lock_guard<std::mutex> lock(m_jobsMutex);

    if (m_jobs.empty())
    {
        ImGui::TextDisabled("No jobs in queue");
        ImGui::TextDisabled("Right-click on .blend files in the browser and select 'Submit to Deadline' to add jobs.");
        return;
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

    // Push table colors for subtle borders
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ImVec4(0.31f, 0.31f, 0.31f, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(0.23f, 0.23f, 0.23f, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.00f, 1.00f, 1.00f, 0.03f));

    // Push larger cell padding for taller rows
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 8.0f));

    if (ImGui::BeginTable("DeadlineJobsTable", 5, flags))
    {
        ImGui::TableSetupColumn("Job Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Job ID", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        ImVec4 accentColor = GetWindowsAccentColor();

        for (int i = 0; i < m_jobs.size(); i++)
        {
            const auto& job = m_jobs[i];

            // Set minimum row height
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 35.0f);

            bool isSelected = (i == m_selectedJobIndex);

            // Column 0: Job Name (selectable)
            ImGui::TableSetColumnIndex(0);
            std::string jobNameUtf8 = UFB::WideToUtf8(job.jobName);

            // Use explicit height to match row height
            if (ImGui::Selectable(jobNameUtf8.c_str(), isSelected,
                                 ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                 ImVec2(0, 35.0f)))
            {
                m_selectedJobIndex = i;
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem())
            {
                // Open blend file location (available for all jobs)
                if (ImGui::MenuItem("Open Blend File Location"))
                {
                    std::filesystem::path blendPath(job.blendFilePath);
                    ShellExecuteW(nullptr, L"open", blendPath.parent_path().wstring().c_str(),
                                 nullptr, nullptr, SW_SHOW);
                }

                // Copy Deadline Job ID (available for submitted/rendering/completed jobs)
                if (!job.deadlineJobId.empty() && ImGui::MenuItem("Copy Job ID"))
                {
                    // Copy job ID to clipboard
                    if (OpenClipboard(nullptr))
                    {
                        EmptyClipboard();
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, job.deadlineJobId.size() + 1);
                        if (hMem)
                        {
                            memcpy(GlobalLock(hMem), job.deadlineJobId.c_str(), job.deadlineJobId.size() + 1);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_TEXT, hMem);
                        }
                        CloseClipboard();
                    }
                }

                ImGui::Separator();

                // Cancel (available for queued/submitting/submitted/rendering jobs)
                if ((job.status == DeadlineJobStatus::QUEUED ||
                     job.status == DeadlineJobStatus::SUBMITTING ||
                     job.status == DeadlineJobStatus::SUBMITTED ||
                     job.status == DeadlineJobStatus::RENDERING) && ImGui::MenuItem("Cancel"))
                {
                    // Defer cancel to after rendering to avoid mutex deadlock
                    m_jobIndexToCancel = i;
                }

                // Remove (available for completed/failed/cancelled jobs)
                if ((job.status == DeadlineJobStatus::COMPLETED ||
                     job.status == DeadlineJobStatus::FAILED ||
                     job.status == DeadlineJobStatus::CANCELLED) && ImGui::MenuItem("Remove"))
                {
                    RemoveJob(i);
                    if (m_selectedJobIndex == i) m_selectedJobIndex = -1;
                }

                ImGui::EndPopup();
            }

            // Show file path as tooltip
            if (ImGui::IsItemHovered())
            {
                std::string filePathUtf8 = UFB::WideToUtf8(job.blendFilePath);
                ImGui::SetTooltip("%s", filePathUtf8.c_str());
            }

            // Column 1: Status
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            std::string statusStr = GetStatusString(job.status);
            ImVec4 statusColor = GetStatusColor(job.status);
            ImGui::TextColored(statusColor, "%s", statusStr.c_str());

            // Column 2: Progress
            ImGui::TableSetColumnIndex(2);

            if (job.status == DeadlineJobStatus::RENDERING || job.status == DeadlineJobStatus::SUBMITTING)
            {
                float cellWidth = ImGui::GetContentRegionAvail().x;
                ImVec2 progressSize = ImVec2(cellWidth, 35.0f);  // Match row height
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);

                char progressText[128];
                if (job.status == DeadlineJobStatus::RENDERING)
                {
                    snprintf(progressText, sizeof(progressText), "%.1f%% - %s",
                            job.progress, job.statusMessage.c_str());
                }
                else
                {
                    snprintf(progressText, sizeof(progressText), "%s", job.statusMessage.c_str());
                }

                ImGui::ProgressBar(job.progress / 100.0f, progressSize, progressText);
                ImGui::PopStyleColor();
            }
            else if (job.status == DeadlineJobStatus::COMPLETED)
            {
                float cellWidth = ImGui::GetContentRegionAvail().x;
                ImVec2 progressSize = ImVec2(cellWidth, 35.0f);  // Match row height
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
                ImGui::ProgressBar(1.0f, progressSize, "Complete");
                ImGui::PopStyleColor();
            }
            else if (job.status == DeadlineJobStatus::FAILED)
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", job.errorMessage.c_str());
            }
            else if (job.status == DeadlineJobStatus::QUEUED)
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Waiting...");
            }
            else if (job.status == DeadlineJobStatus::SUBMITTED)
            {
                ImGui::AlignTextToFramePadding();
                if (!job.statusMessage.empty())
                {
                    ImGui::TextDisabled("%s", job.statusMessage.c_str());
                }
                else
                {
                    ImGui::TextDisabled("Submitted to Deadline...");
                }
            }

            // Column 3: Frame Range
            ImGui::TableSetColumnIndex(3);
            ImGui::AlignTextToFramePadding();
            std::string frameRange = std::to_string(job.frameStart) + "-" + std::to_string(job.frameEnd);
            ImGui::TextUnformatted(frameRange.c_str());

            // Column 4: Job ID
            ImGui::TableSetColumnIndex(4);
            ImGui::AlignTextToFramePadding();
            if (!job.deadlineJobId.empty())
            {
                ImGui::TextDisabled("%s", job.deadlineJobId.c_str());
            }
            else
            {
                ImGui::TextDisabled("-");
            }
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar(); // Pop CellPadding
    ImGui::PopStyleColor(3); // Pop table colors
}

void DeadlineQueuePanel::RenderJobDetailsPanel()
{
    std::lock_guard<std::mutex> lock(m_jobsMutex);

    if (m_selectedJobIndex < 0 || m_selectedJobIndex >= m_jobs.size())
    {
        ImGui::TextDisabled("No job selected");
        return;
    }

    const auto& job = m_jobs[m_selectedJobIndex];

    ImGui::Text("Job Name: %s", UFB::WideToUtf8(job.jobName).c_str());
    ImGui::Separator();

    // Blend file path with "Open in Browser" buttons
    ImGui::Text("Blend File: %s", UFB::WideToUtf8(job.blendFilePath).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Left Browser##LB"))
    {
        if (onOpenInLeftBrowser)
        {
            std::filesystem::path blendPath(job.blendFilePath);
            onOpenInLeftBrowser(blendPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Open blend file location in the Left Browser");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Right Browser##RB"))
    {
        if (onOpenInRightBrowser)
        {
            std::filesystem::path blendPath(job.blendFilePath);
            onOpenInRightBrowser(blendPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Open blend file location in the Right Browser");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("New Window##NW"))
    {
        if (onOpenInNewWindow)
        {
            std::filesystem::path blendPath(job.blendFilePath);
            onOpenInNewWindow(blendPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Open blend file location in a new window");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Shot View##SV"))
    {
        if (onOpenInShotView)
        {
            std::filesystem::path blendPath(job.blendFilePath);
            onOpenInShotView(blendPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Open blend file location in Shot View");
    }

    ImGui::Separator();

    // Status and progress
    ImGui::Text("Status: %s", GetStatusString(job.status).c_str());
    ImGui::Text("Frame Range: %d - %d", job.frameStart, job.frameEnd);
    ImGui::Text("Chunk Size: %d", job.chunkSize);
    ImGui::Text("Pool: %s", job.pool.c_str());
    ImGui::Text("Priority: %d", job.priority);

    if (!job.deadlineJobId.empty())
    {
        ImGui::Text("Deadline Job ID: %s", job.deadlineJobId.c_str());
    }

    if (job.status == DeadlineJobStatus::RENDERING || job.status == DeadlineJobStatus::SUBMITTED)
    {
        ImGui::Text("Progress: %.1f%%", job.progress);

        if (!job.statusMessage.empty())
        {
            ImGui::Text("Details: %s", job.statusMessage.c_str());
        }
    }

    // Timing information
    if (job.submitTime > 0)
    {
        auto submitTimePoint = std::chrono::system_clock::time_point(std::chrono::seconds(job.submitTime));
        std::time_t submitTime_t = std::chrono::system_clock::to_time_t(submitTimePoint);
        char timeBuffer[64];
        std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", std::localtime(&submitTime_t));
        ImGui::Text("Submitted: %s", timeBuffer);

        if (job.completeTime > 0)
        {
            uint64_t duration = job.completeTime - job.submitTime;
            ImGui::Text("Duration: %llu seconds", duration);
        }
        else if (job.status == DeadlineJobStatus::RENDERING || job.status == DeadlineJobStatus::SUBMITTED)
        {
            auto now = std::chrono::system_clock::now();
            auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            uint64_t elapsed = nowSeconds - job.submitTime;
            ImGui::Text("Elapsed: %llu seconds", elapsed);
        }
    }

    if (!job.errorMessage.empty())
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error:");
        ImGui::TextWrapped("%s", job.errorMessage.c_str());
    }
}

void DeadlineQueuePanel::AddRenderJob(const DeadlineJob& job)
{
    std::lock_guard<std::mutex> lock(m_jobsMutex);
    m_jobs.push_back(job);
    m_asyncOperations.push_back(nullptr);  // No async operation yet
    std::cout << "[DeadlineQueuePanel] Job added to queue. Total jobs: " << m_jobs.size() << std::endl;
}

void DeadlineQueuePanel::ProcessQueue()
{
    if (m_deadlineCommandPath.empty())
    {
        static bool warnedOnce = false;
        if (!warnedOnce)
        {
            std::cout << "[DeadlineQueuePanel] Deadline command not found, skipping ProcessQueue" << std::endl;
            warnedOnce = true;
        }
        return;  // Deadline not available
    }

    // Check if it's time to poll (every 5 seconds like old app)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPollTime).count();

    bool shouldPoll = (elapsed >= 5000);  // 5 seconds

    // Lock only to read current state
    m_jobsMutex.lock();
    size_t jobCount = m_jobs.size();
    m_jobsMutex.unlock();

    for (size_t i = 0; i < jobCount; i++)
    {
        m_jobsMutex.lock();
        auto asyncOp = m_asyncOperations[i];  // Copy shared_ptr
        auto currentStatus = m_jobs[i].status;
        m_jobsMutex.unlock();

        // Check if there's an async operation running for this job
        bool isOperationRunning = false;
        if (asyncOp != nullptr)
        {
            if (asyncOp->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                isOperationRunning = true;
            }
            else
            {
                // Clear completed async operation
                m_jobsMutex.lock();
                m_asyncOperations[i] = nullptr;
                m_jobsMutex.unlock();
            }
        }

        if (isOperationRunning)
        {
            // Still processing, skip
            continue;
        }

        if (currentStatus == DeadlineJobStatus::QUEUED)
        {
            std::cout << "[DeadlineQueuePanel] Launching async submission for job..." << std::endl;
            // Launch async submission (NO lock during execution!)
            auto future = std::make_shared<std::future<void>>(std::async(std::launch::async, [this, i]() {
                // Get job data outside lock
                m_jobsMutex.lock();
                if (i >= m_jobs.size())
                {
                    m_jobsMutex.unlock();
                    return;
                }
                DeadlineJob jobCopy = m_jobs[i];
                m_jobsMutex.unlock();

                // Execute command WITHOUT holding lock
                SubmitJobToDeadline(jobCopy);

                // Update job WITH lock
                m_jobsMutex.lock();
                if (i < m_jobs.size())
                {
                    m_jobs[i] = jobCopy;
                }
                m_jobsMutex.unlock();
            }));

            m_jobsMutex.lock();
            m_asyncOperations[i] = future;
            m_jobsMutex.unlock();
        }
        else if (shouldPoll && (currentStatus == DeadlineJobStatus::SUBMITTED || currentStatus == DeadlineJobStatus::RENDERING))
        {
            // Launch async poll (NO lock during execution!)
            auto future = std::make_shared<std::future<void>>(std::async(std::launch::async, [this, i]() {
                // Get job data outside lock
                m_jobsMutex.lock();
                if (i >= m_jobs.size())
                {
                    m_jobsMutex.unlock();
                    return;
                }
                DeadlineJob jobCopy = m_jobs[i];
                m_jobsMutex.unlock();

                // Execute poll WITHOUT holding lock
                PollJobProgress(jobCopy);

                // Update job WITH lock
                m_jobsMutex.lock();
                if (i < m_jobs.size())
                {
                    m_jobs[i] = jobCopy;
                }
                m_jobsMutex.unlock();
            }));

            m_jobsMutex.lock();
            m_asyncOperations[i] = future;
            m_jobsMutex.unlock();
        }
    }

    if (shouldPoll)
    {
        m_lastPollTime = now;
    }
}

void DeadlineQueuePanel::SubmitJobToDeadline(DeadlineJob& job)
{
    job.status = DeadlineJobStatus::SUBMITTING;
    job.statusMessage = "Creating job files...";

    // Create job info and plugin info files
    std::wstring jobInfoPath, pluginInfoPath;

    if (!CreateJobInfoFile(job, jobInfoPath))
    {
        job.status = DeadlineJobStatus::FAILED;
        job.errorMessage = "Failed to create job_info.job";
        return;
    }

    if (!CreatePluginInfoFile(job, pluginInfoPath))
    {
        job.status = DeadlineJobStatus::FAILED;
        job.errorMessage = "Failed to create plugin_info.job";
        return;
    }

    // Execute deadlinecommand to submit
    job.statusMessage = "Submitting to Deadline...";

    std::wstring args = L"\"" + jobInfoPath + L"\" \"" + pluginInfoPath + L"\"";
    std::string output;

    if (!ExecuteDeadlineCommand(args, output))
    {
        job.status = DeadlineJobStatus::FAILED;
        job.errorMessage = "Failed to execute deadlinecommand";
        return;
    }

    // Extract job ID from output
    std::string jobId = ExtractJobIdFromOutput(output);

    if (jobId.empty())
    {
        job.status = DeadlineJobStatus::FAILED;
        job.errorMessage = "Failed to extract Job ID from Deadline output";
        return;
    }

    // Successfully submitted
    job.deadlineJobId = jobId;
    job.status = DeadlineJobStatus::SUBMITTED;
    job.statusMessage = "Submitted - waiting in queue";
    job.submitTime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::cout << "[DeadlineQueuePanel] Job submitted successfully with ID: " << jobId << std::endl;

    // Clean up temp files
    try
    {
        std::filesystem::remove(jobInfoPath);
        std::filesystem::remove(pluginInfoPath);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[DeadlineQueuePanel] Warning: Failed to delete temp files: " << e.what() << std::endl;
    }
}

void DeadlineQueuePanel::PollJobProgress(DeadlineJob& job)
{
    if (job.deadlineJobId.empty())
        return;

    std::cout << "[DeadlineQueuePanel] Polling job " << job.deadlineJobId << std::endl;

    // Execute deadlinecommand -GetJobDetails {jobId}
    std::wstring args = L"-GetJobDetails " + UFB::Utf8ToWide(job.deadlineJobId);
    std::string output;

    if (!ExecuteDeadlineCommand(args, output))
    {
        std::cout << "[DeadlineQueuePanel] Failed to execute GetJobDetails command" << std::endl;
        // Failed to get details, but don't mark as failed - might be temporary network issue
        return;
    }

    if (output.empty())
    {
        std::cout << "[DeadlineQueuePanel] GetJobDetails returned empty output" << std::endl;
        return;
    }

    std::cout << "[DeadlineQueuePanel] GetJobDetails output (" << output.length() << " chars):" << std::endl;
    std::cout << output << std::endl;
    std::cout << "[DeadlineQueuePanel] --- End of output ---" << std::endl;

    // Helper to extract field value (matches old C# app pattern)
    auto ExtractField = [](const std::string& output, const std::string& key) -> std::string {
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line))
        {
            // Trim line
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            if (line.find(key) == 0)
            {
                // Split by first colon
                size_t colonPos = line.find(':');
                if (colonPos != std::string::npos && colonPos + 1 < line.length())
                {
                    std::string value = line.substr(colonPos + 1);
                    // Trim value
                    value.erase(0, value.find_first_not_of(" \t\r\n"));
                    value.erase(value.find_last_not_of(" \t\r\n") + 1);
                    return value;
                }
            }
        }
        return "";
    };

    // Extract fields like old C# app
    std::string status = ExtractField(output, "Status:");
    std::string progressStr = ExtractField(output, "Progress:");

    std::cout << "[DeadlineQueuePanel] Poll result - Status: '" << status << "', Progress: '" << progressStr << "'" << std::endl;

    // Parse progress percentage (e.g., "45.5% (10/22)")
    float progressPercent = 0.0f;
    if (!progressStr.empty())
    {
        // Extract numeric percentage at beginning
        size_t percentPos = progressStr.find('%');
        if (percentPos != std::string::npos)
        {
            try
            {
                progressPercent = std::stof(progressStr.substr(0, percentPos));
                std::cout << "[DeadlineQueuePanel] Parsed progress: " << progressPercent << "%" << std::endl;
            }
            catch (...)
            {
                std::cout << "[DeadlineQueuePanel] Failed to parse progress percentage" << std::endl;
            }
        }
        else
        {
            std::cout << "[DeadlineQueuePanel] No '%' found in progress string" << std::endl;
        }
    }

    // Update progress
    job.progress = progressPercent;
    job.statusMessage = status;

    // Update status based on Deadline's status and progress
    // Match old C# app logic: if we have progress, we're rendering
    if (status == "Completed")
    {
        job.status = DeadlineJobStatus::COMPLETED;
        job.progress = 100.0f;
        job.completeTime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::cout << "[DeadlineQueuePanel] Job completed" << std::endl;
    }
    else if (status == "Failed" || status.find("Error") != std::string::npos)
    {
        job.status = DeadlineJobStatus::FAILED;
        job.errorMessage = "Render failed (see Deadline Monitor for details)";
        job.completeTime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::cout << "[DeadlineQueuePanel] Job failed" << std::endl;
    }
    else if (progressPercent > 0.0f)
    {
        // If we have any progress, consider it rendering (like old app)
        job.status = DeadlineJobStatus::RENDERING;
        std::cout << "[DeadlineQueuePanel] Job is rendering (progress: " << progressPercent << "%)" << std::endl;
    }
    else
    {
        // No progress yet, still waiting
        job.status = DeadlineJobStatus::SUBMITTED;
        std::cout << "[DeadlineQueuePanel] Job still queued/waiting" << std::endl;
    }
}

bool DeadlineQueuePanel::ExecuteDeadlineCommand(const std::wstring& arguments, std::string& output)
{
    // Build command line
    std::wstring commandLine = L"\"" + m_deadlineCommandPath + L"\" " + arguments;

    // Create process to execute deadlinecommand
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // Create pipe for stdout
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
    {
        std::cerr << "[DeadlineQueuePanel] Failed to create pipe" << std::endl;
        return false;
    }

    // Ensure read handle is not inherited
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // Create a null stdin pipe so child process doesn't wait for input
    HANDLE hStdinRead, hStdinWrite;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0))
    {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        std::cerr << "[DeadlineQueuePanel] Failed to create stdin pipe" << std::endl;
        return false;
    }
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
    CloseHandle(hStdinWrite);  // Close write end immediately so child gets EOF on stdin

    // Setup startup info
    STARTUPINFOW si = {};
    si.cb = sizeof(STARTUPINFOW);
    si.hStdInput = hStdinRead;   // Provide stdin handle (will get EOF immediately)
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    // Create process
    wchar_t* cmdLine = _wcsdup(commandLine.c_str());
    BOOL success = CreateProcessW(
        NULL,
        cmdLine,
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    free(cmdLine);
    CloseHandle(hWritePipe);
    CloseHandle(hStdinRead);  // Close stdin read handle in parent

    if (!success)
    {
        CloseHandle(hReadPipe);
        std::cerr << "[DeadlineQueuePanel] Failed to create process. Error: " << GetLastError() << std::endl;
        return false;
    }

    // Read output
    output.clear();
    char buffer[4096];
    DWORD bytesRead;

    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
    {
        buffer[bytesRead] = '\0';
        output += buffer;
    }

    // Wait for process to complete (30 second timeout)
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 30000);

    if (waitResult == WAIT_TIMEOUT)
    {
        std::cerr << "[DeadlineQueuePanel] Deadline command timed out after 30 seconds" << std::endl;
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(hReadPipe);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }

    // Get exit code
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0)
    {
        std::cerr << "[DeadlineQueuePanel] Deadline command failed with exit code: " << exitCode << std::endl;
        std::cerr << "[DeadlineQueuePanel] Output: " << output << std::endl;
    }

    return (exitCode == 0);
}

bool DeadlineQueuePanel::CreateJobInfoFile(const DeadlineJob& job, std::wstring& outPath)
{
    // Create temp file in system temp directory
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);

    // Generate unique filename
    std::wstring filename = std::wstring(tempPath) + L"deadline_job_" +
        std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count()) + L".job";

    // Get just the filename without extension for the job name
    std::filesystem::path blendPath(job.blendFilePath);
    std::string fileName = blendPath.stem().string();

    // Create job_info.job content (match old app exactly)
    std::ostringstream content;
    content << "Plugin=Blender\n";
    content << "Name=" << fileName << "\n";
    content << "Comment=\n";
    content << "Department=\n";
    content << "Pool=" << job.pool << "\n";
    content << "Priority=" << job.priority << "\n";
    content << "Frames=" << job.frameStart << "-" << job.frameEnd << "\n";
    content << "ChunkSize=" << job.chunkSize << "\n";

    // Write to file
    std::ofstream file(filename);
    if (!file.is_open())
    {
        std::cerr << "[DeadlineQueuePanel] Failed to create job_info file: " << UFB::WideToUtf8(filename) << std::endl;
        return false;
    }

    file << content.str();
    file.close();

    outPath = filename;
    return true;
}

bool DeadlineQueuePanel::CreatePluginInfoFile(const DeadlineJob& job, std::wstring& outPath)
{
    // Create temp file in system temp directory
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);

    // Generate unique filename
    std::wstring filename = std::wstring(tempPath) + L"deadline_plugin_" +
        std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count()) + L".job";

    // Convert blend file path to UTF-8 and escape backslashes (\ -> \\)
    std::string blendPathUtf8 = UFB::WideToUtf8(job.blendFilePath);

    // Escape backslashes for Deadline
    std::string escapedPath;
    for (char c : blendPathUtf8)
    {
        if (c == '\\')
            escapedPath += "\\\\";
        else
            escapedPath += c;
    }

    // Create plugin_info.job content (match old app exactly)
    std::ostringstream content;
    content << "SceneFile=" << escapedPath << "\n";
    content << "Threads=0\n";
    content << "Build=0\n";

    // Write to file
    std::ofstream file(filename);
    if (!file.is_open())
    {
        std::cerr << "[DeadlineQueuePanel] Failed to create plugin_info file: " << UFB::WideToUtf8(filename) << std::endl;
        return false;
    }

    file << content.str();
    file.close();

    outPath = filename;
    return true;
}

std::string DeadlineQueuePanel::ExtractJobIdFromOutput(const std::string& output)
{
    // Deadline output typically contains a line like:
    // "JobID=5f8a9b2c3d4e5f6a7b8c9d0e"
    // or
    // "Result=Success (Job ID: 5f8a9b2c3d4e5f6a7b8c9d0e)"

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line))
    {
        // Look for "JobID=" pattern
        size_t pos = line.find("JobID=");
        if (pos != std::string::npos)
        {
            std::string jobId = line.substr(pos + 6);
            // Trim whitespace
            jobId.erase(0, jobId.find_first_not_of(" \t\r\n"));
            jobId.erase(jobId.find_last_not_of(" \t\r\n") + 1);
            return jobId;
        }

        // Look for "Job ID:" pattern
        pos = line.find("Job ID:");
        if (pos != std::string::npos)
        {
            std::string jobId = line.substr(pos + 7);
            // Extract until closing parenthesis or end
            size_t endPos = jobId.find(')');
            if (endPos != std::string::npos)
            {
                jobId = jobId.substr(0, endPos);
            }
            // Trim whitespace
            jobId.erase(0, jobId.find_first_not_of(" \t\r\n"));
            jobId.erase(jobId.find_last_not_of(" \t\r\n") + 1);
            return jobId;
        }
    }

    return "";
}

std::string DeadlineQueuePanel::GetStatusString(DeadlineJobStatus status)
{
    switch (status)
    {
    case DeadlineJobStatus::QUEUED:     return "Queued";
    case DeadlineJobStatus::SUBMITTING: return "Submitting";
    case DeadlineJobStatus::SUBMITTED:  return "Submitted";
    case DeadlineJobStatus::RENDERING:  return "Rendering";
    case DeadlineJobStatus::COMPLETED:  return "Completed";
    case DeadlineJobStatus::FAILED:     return "Failed";
    case DeadlineJobStatus::CANCELLED:  return "Cancelled";
    default:                            return "Unknown";
    }
}

ImVec4 DeadlineQueuePanel::GetStatusColor(DeadlineJobStatus status)
{
    ImVec4 accentColor = GetWindowsAccentColor();

    switch (status)
    {
    case DeadlineJobStatus::QUEUED:     return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);  // Gray
    case DeadlineJobStatus::SUBMITTING: return ImVec4(0.9f, 0.7f, 0.3f, 1.0f);  // Orange
    case DeadlineJobStatus::SUBMITTED:  return ImVec4(0.9f, 0.7f, 0.3f, 1.0f);  // Orange
    case DeadlineJobStatus::RENDERING:  return ImVec4(accentColor.x * 1.3f, accentColor.y * 1.3f, accentColor.z * 1.3f, 1.0f);  // Bright accent (like transcode)
    case DeadlineJobStatus::COMPLETED:  return accentColor;  // Accent color (like transcode)
    case DeadlineJobStatus::FAILED:     return ImVec4(0.9f, 0.3f, 0.3f, 1.0f);  // Red
    case DeadlineJobStatus::CANCELLED:  return ImVec4(0.7f, 0.5f, 0.3f, 1.0f);  // Brown/Orange
    default:                            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White
    }
}

void DeadlineQueuePanel::RemoveJob(int index)
{
    // Note: Caller should have lock on m_jobsMutex
    if (index >= 0 && index < m_jobs.size())
    {
        m_jobs.erase(m_jobs.begin() + index);
        m_asyncOperations.erase(m_asyncOperations.begin() + index);
    }
}

void DeadlineQueuePanel::CancelJob(int index)
{
    // Get job ID without holding lock for long
    std::string jobId;
    {
        std::lock_guard<std::mutex> lock(m_jobsMutex);

        if (index < 0 || index >= m_jobs.size())
            return;

        jobId = m_jobs[index].deadlineJobId;
    }

    // Execute Deadline command WITHOUT holding the lock (to prevent UI freeze and deadlock)
    if (!jobId.empty())
    {
        std::wstring args = L"-SuspendJob " + UFB::Utf8ToWide(jobId);
        std::string output;

        if (ExecuteDeadlineCommand(args, output))
        {
            std::cout << "[DeadlineQueuePanel] Job suspended in Deadline: " << jobId << std::endl;
        }
        else
        {
            std::cerr << "[DeadlineQueuePanel] Failed to suspend job in Deadline: " << jobId << std::endl;
            // Continue anyway to mark as cancelled locally
        }
    }

    // Now update the job status WITH lock
    {
        std::lock_guard<std::mutex> lock(m_jobsMutex);

        if (index < 0 || index >= m_jobs.size())
            return;

        auto& job = m_jobs[index];
        job.status = DeadlineJobStatus::CANCELLED;
        job.errorMessage = "Cancelled by user";
        job.completeTime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

void DeadlineQueuePanel::ClearCompleted()
{
    std::lock_guard<std::mutex> lock(m_jobsMutex);

    // Remove completed/failed/cancelled jobs and their async operations
    for (int i = static_cast<int>(m_jobs.size()) - 1; i >= 0; i--)
    {
        if (m_jobs[i].status == DeadlineJobStatus::COMPLETED ||
            m_jobs[i].status == DeadlineJobStatus::FAILED ||
            m_jobs[i].status == DeadlineJobStatus::CANCELLED)
        {
            m_jobs.erase(m_jobs.begin() + i);
            m_asyncOperations.erase(m_asyncOperations.begin() + i);
        }
    }
}

void DeadlineQueuePanel::ClearAll()
{
    std::lock_guard<std::mutex> lock(m_jobsMutex);
    m_jobs.clear();
    m_asyncOperations.clear();
}
