#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <functional>

namespace UFB {

// Single transcode job
struct TranscodeJob {
    enum class Status {
        QUEUED,
        PROCESSING,
        COPYING_METADATA,
        COMPLETED,
        FAILED,
        CANCELLED
    };

    std::string id;                 // Unique ID
    std::wstring inputPath;         // Source file path
    std::wstring outputPath;        // Destination MP4 file path
    Status status = Status::QUEUED;

    // Progress tracking
    int currentFrame = 0;
    int totalFrames = 0;
    float progressPercent = 0.0f;
    float encodingFps = 0.0f;

    // Timestamps
    std::chrono::system_clock::time_point queuedTime;
    std::chrono::system_clock::time_point startedTime;
    std::chrono::system_clock::time_point completedTime;

    // Error info
    std::string errorMessage;

    // Helper methods
    std::wstring GetInputFileName() const;
    std::wstring GetOutputFileName() const;
    const char* GetStatusString() const;
    float GetElapsedSeconds() const;
};

class TranscodeQueuePanel {
public:
    TranscodeQueuePanel();
    ~TranscodeQueuePanel();

    // Panel control
    void Show() { m_isOpen = true; }
    void Hide() { m_isOpen = false; }
    void Toggle() { m_isOpen = !m_isOpen; }
    bool IsOpen() const { return m_isOpen; }

    // Main render method
    void Render();

    // Queue operations
    void AddJob(const std::wstring& inputPath);
    void AddMultipleJobs(const std::vector<std::wstring>& inputPaths);
    void RemoveJob(const std::string& jobId);
    void CancelCurrentJob();
    void ClearCompleted();
    void ClearAll();

    // Queue state
    bool IsProcessing() const { return m_isProcessing; }
    size_t GetQueueSize() const { return m_jobs.size(); }
    size_t GetCompletedCount() const;
    size_t GetFailedCount() const;

    // Update (call each frame)
    void Update();

    // Callbacks for opening file location in browsers
    std::function<void(const std::wstring&)> onOpenInBrowser1;
    std::function<void(const std::wstring&)> onOpenInBrowser2;

private:
    // UI Rendering
    void RenderToolbar();
    void RenderQueueTable();
    void RenderJobDetailsPanel();

    // Processing
    void ProcessNextJob();
    void StartFFmpeg(TranscodeJob& job);
    void UpdateFFmpegProgress(TranscodeJob& job);
    void CopyMetadataWithExiftool(TranscodeJob& job);
    void CompleteJob(TranscodeJob& job, bool success, const std::string& errorMsg = "");

    // Helpers
    std::wstring CreateOutputPath(const std::wstring& inputPath);
    int GetTotalFrames(const std::wstring& inputPath);
    std::string GenerateJobId();

    // State
    bool m_isOpen = false;
    bool m_isProcessing = false;
    std::vector<std::unique_ptr<TranscodeJob>> m_jobs;
    std::string m_selectedJobId;

    // FFmpeg process state
    void* m_ffmpegProcess = nullptr;  // HANDLE on Windows
    void* m_ffmpegPipe = nullptr;      // HANDLE for stdout pipe

    // Paths
    std::wstring m_ffmpegPath;
    std::wstring m_ffprobePath;
    std::wstring m_exiftoolPath;

    // UI State
    float m_detailsPanelHeight = 200.0f;
    bool m_autoClearCompleted = false;
};

} // namespace UFB
