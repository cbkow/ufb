#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <thread>
#include <future>
#include "imgui.h"

// Deadline job status
enum class DeadlineJobStatus
{
    QUEUED,      // Waiting to be submitted
    SUBMITTING,  // Currently submitting to Deadline
    SUBMITTED,   // Submitted to Deadline, waiting for render start
    RENDERING,   // Actively rendering
    COMPLETED,   // Render completed successfully
    FAILED,      // Render failed
    CANCELLED    // Job cancelled by user
};

// Deadline job information
struct DeadlineJob
{
    std::wstring blendFilePath;     // Path to .blend file
    std::wstring jobName;           // Display name for job
    std::string deadlineJobId;      // Deadline's job ID (empty until submitted)

    // Render parameters
    int frameStart = 1;
    int frameEnd = 1;
    int chunkSize = 1;
    std::string pool = "none";
    int priority = 50;

    // Status tracking
    DeadlineJobStatus status = DeadlineJobStatus::QUEUED;
    float progress = 0.0f;          // 0.0 to 100.0
    std::string statusMessage;      // Current status text
    std::string errorMessage;       // Error details if failed

    // Timing
    uint64_t submitTime = 0;        // Timestamp when submitted
    uint64_t completeTime = 0;      // Timestamp when completed/failed
};

class DeadlineQueuePanel
{
public:
    DeadlineQueuePanel();
    ~DeadlineQueuePanel();

    // Initialize the panel
    void Initialize();

    // Shutdown and cleanup
    void Shutdown();

    // Draw the queue panel UI
    void Draw(const char* title);

    // Add a job to the queue
    void AddRenderJob(const DeadlineJob& job);

    // Check if panel is open
    bool IsOpen() const { return m_isOpen; }

    // Show the panel
    void Show() { m_isOpen = true; }

    // Toggle panel visibility
    void Toggle() { m_isOpen = !m_isOpen; }

    // Process queue (submit jobs and poll for progress)
    void ProcessQueue();

    // Callbacks for opening file location in browsers and views
    std::function<void(const std::wstring&)> onOpenInLeftBrowser;
    std::function<void(const std::wstring&)> onOpenInRightBrowser;
    std::function<void(const std::wstring&)> onOpenInNewWindow;
    std::function<void(const std::wstring&)> onOpenInShotView;

private:
    // Window state
    bool m_isOpen = false;  // Start hidden, show when first job added

    // Job queue
    std::vector<DeadlineJob> m_jobs;
    mutable std::mutex m_jobsMutex;  // Protects m_jobs from concurrent access

    // Async operation tracking (one per job index)
    std::vector<std::shared_ptr<std::future<void>>> m_asyncOperations;

    // Processing state
    bool m_isProcessing = false;
    int m_currentJobIndex = -1;

    // Polling timer (poll every 5 seconds like old app)
    std::chrono::steady_clock::time_point m_lastPollTime;

    // Deadline command path
    std::wstring m_deadlineCommandPath;

    // UI state
    int m_selectedJobIndex = -1;
    float m_detailsPanelHeight = 200.0f;
    int m_jobIndexToCancel = -1;  // Job to cancel after rendering

    // Helper methods
    void RenderToolbar();
    void DrawJobsTable();
    void RenderJobDetailsPanel();
    void SubmitJobToDeadline(DeadlineJob& job);
    void PollJobProgress(DeadlineJob& job);
    std::string GetStatusString(DeadlineJobStatus status);
    ImVec4 GetStatusColor(DeadlineJobStatus status);

    // Deadline command execution
    bool ExecuteDeadlineCommand(const std::wstring& arguments, std::string& output);
    bool CreateJobInfoFile(const DeadlineJob& job, std::wstring& outPath);
    bool CreatePluginInfoFile(const DeadlineJob& job, std::wstring& outPath);
    std::string ExtractJobIdFromOutput(const std::string& output);

    // Job control actions
    void RemoveJob(int index);
    void CancelJob(int index);
    void ClearCompleted();
    void ClearAll();

    // Find deadlinecommand.exe path
    bool FindDeadlineCommand();
};
