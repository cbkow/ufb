#include "transcode_queue_panel.h"
#include "imgui.h"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <random>
#include <windows.h>

// Undefine Windows macros that conflict with our method names
#ifdef AddJob
#undef AddJob
#endif

namespace fs = std::filesystem;

// Helper to get Windows accent color (global function from main.cpp)
extern ImVec4 GetWindowsAccentColor();

// Mono font from main.cpp
extern ImFont* font_mono;

namespace UFB {

// Helper: Convert wstring to UTF-8 string
static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], size, nullptr, nullptr);
    return result;
}

// Helper: Convert UTF-8 to wstring
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], size);
    return result;
}

// TranscodeJob helper methods
std::wstring TranscodeJob::GetInputFileName() const {
    return fs::path(inputPath).filename().wstring();
}

std::wstring TranscodeJob::GetOutputFileName() const {
    return fs::path(outputPath).filename().wstring();
}

const char* TranscodeJob::GetStatusString() const {
    switch (status) {
        case Status::QUEUED: return "Queued";
        case Status::PROCESSING: return "Processing";
        case Status::COPYING_METADATA: return "Copying Metadata";
        case Status::COMPLETED: return "Completed";
        case Status::FAILED: return "Failed";
        case Status::CANCELLED: return "Cancelled";
        default: return "Unknown";
    }
}

float TranscodeJob::GetElapsedSeconds() const {
    if (status == Status::QUEUED) return 0.0f;

    auto endTime = (status == Status::COMPLETED || status == Status::FAILED || status == Status::CANCELLED)
                   ? completedTime : std::chrono::system_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startedTime);
    return duration.count() / 1000.0f;
}

// TranscodeQueuePanel implementation
TranscodeQueuePanel::TranscodeQueuePanel() {
    // Set up tool paths - FFmpeg is copied to exe directory by CMake, Exiftool in assets subfolder
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();

    // FFmpeg and ffprobe are in the same directory as the executable (copied by CMake build)
    m_ffmpegPath = (exeDir / "ffmpeg.exe").wstring();
    m_ffprobePath = (exeDir / "ffprobe.exe").wstring();
    m_exiftoolPath = (exeDir / "assets" / "exiftool" / "exiftool.exe").wstring();
}

TranscodeQueuePanel::~TranscodeQueuePanel() {
    // Clean up any running process
    if (m_ffmpegProcess) {
        TerminateProcess((HANDLE)m_ffmpegProcess, 1);
        CloseHandle((HANDLE)m_ffmpegProcess);
        m_ffmpegProcess = nullptr;
    }
    if (m_ffmpegPipe) {
        CloseHandle((HANDLE)m_ffmpegPipe);
        m_ffmpegPipe = nullptr;
    }
}

void TranscodeQueuePanel::Render() {
    if (!m_isOpen) return;

    // Make it a regular dockable window (not modal)
    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Transcode Queue", &m_isOpen, ImGuiWindowFlags_None)) {
        RenderToolbar();
        ImGui::Separator();

        // Queue table (takes remaining space minus details panel)
        float availableHeight = ImGui::GetContentRegionAvail().y;
        float tableHeight = availableHeight - m_detailsPanelHeight - 10.0f;

        ImGui::BeginChild("QueueTableRegion", ImVec2(0, tableHeight), true, ImGuiWindowFlags_NoScrollbar);
        RenderQueueTable();
        ImGui::EndChild();

        ImGui::Separator();

        // Details panel at bottom
        ImGui::BeginChild("DetailsPanel", ImVec2(0, m_detailsPanelHeight), true, ImGuiWindowFlags_NoScrollbar);
        RenderJobDetailsPanel();
        ImGui::EndChild();
    }
    ImGui::End();
}

void TranscodeQueuePanel::RenderToolbar() {
    // Status indicator with accent color
    const char* statusText = m_isProcessing ? "PROCESSING" : "IDLE";
    ImVec4 accentColor = GetWindowsAccentColor();
    ImVec4 statusColor = m_isProcessing ? accentColor : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

    ImGui::TextColored(statusColor, "STATUS: %s", statusText);
    ImGui::SameLine();

    // Statistics
    size_t total = m_jobs.size();
    size_t completed = GetCompletedCount();
    size_t failed = GetFailedCount();
    size_t queued = 0;
    for (const auto& job : m_jobs) {
        if (job->status == TranscodeJob::Status::QUEUED) queued++;
    }

    ImGui::Text(" | Total: %zu  Queued: %zu  Completed: %zu  Failed: %zu", total, queued, completed, failed);

    ImGui::SameLine(ImGui::GetWindowWidth() - 320);

    // Control buttons with wider widths
    if (ImGui::Button("Clear Completed", ImVec2(140, 0))) {
        ClearCompleted();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All", ImVec2(130, 0))) {
        ClearAll();
    }
}

void TranscodeQueuePanel::RenderQueueTable() {
    if (m_jobs.empty()) {
        ImGui::TextDisabled("No jobs in queue");
        ImGui::TextDisabled("Right-click on video files in the browser and select 'Transcode to MP4' to add jobs.");
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

    if (ImGui::BeginTable("TranscodeJobsTable", 5, flags)) {
        // Make Filename and Progress both stretch, giving Progress 50% of available width
        ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 1.0f);  // 50% width (shared with Filename)
        ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < m_jobs.size(); ++i) {
            const auto& job = m_jobs[i];
            // Set minimum row height (hard-coded for better cell height)
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 35.0f);

            bool isSelected = (job->id == m_selectedJobId);

            // Row coloring based on status using accent colors
            ImVec4 accentColor = GetWindowsAccentColor();
            ImVec4 rowColor;
            ImVec4 textColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Default white text

            switch (job->status) {
                case TranscodeJob::Status::QUEUED:
                    rowColor = ImVec4(0.3f, 0.3f, 0.3f, 0.3f);
                    textColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                    break;
                case TranscodeJob::Status::PROCESSING:
                case TranscodeJob::Status::COPYING_METADATA:
                    rowColor = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.4f);
                    textColor = ImVec4(accentColor.x * 1.3f, accentColor.y * 1.3f, accentColor.z * 1.3f, 1.0f);
                    break;
                case TranscodeJob::Status::COMPLETED:
                    // Use regular accent color for completed (not green)
                    rowColor = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.3f);
                    textColor = accentColor;
                    break;
                case TranscodeJob::Status::FAILED:
                case TranscodeJob::Status::CANCELLED:
                    rowColor = ImVec4(0.8f, 0.2f, 0.2f, 0.3f);
                    textColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                    break;
            }

            // Filename column (selectable)
            ImGui::TableSetColumnIndex(0);
            std::string filenameUtf8 = WideToUtf8(job->GetInputFileName());

            // Use explicit height to match row height
            if (ImGui::Selectable(filenameUtf8.c_str(), isSelected,
                                 ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                 ImVec2(0, 35.0f))) {
                m_selectedJobId = job->id;
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem()) {
                if (job->status == TranscodeJob::Status::PROCESSING && ImGui::MenuItem("Cancel")) {
                    CancelCurrentJob();
                }
                if ((job->status == TranscodeJob::Status::COMPLETED ||
                     job->status == TranscodeJob::Status::FAILED ||
                     job->status == TranscodeJob::Status::CANCELLED) && ImGui::MenuItem("Remove")) {
                    RemoveJob(job->id);
                }
                if (job->status == TranscodeJob::Status::COMPLETED && ImGui::MenuItem("Open Output Folder")) {
                    ShellExecuteW(nullptr, L"open", fs::path(job->outputPath).parent_path().wstring().c_str(),
                                 nullptr, nullptr, SW_SHOW);
                }
                ImGui::EndPopup();
            }

            // Status column
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(rowColor, "%s", job->GetStatusString());

            // Progress column with accent color
            ImGui::TableSetColumnIndex(2);
            if (job->status == TranscodeJob::Status::PROCESSING) {
                char progressText[64];
                snprintf(progressText, sizeof(progressText), "%.1f%% (%d/%d)",
                        job->progressPercent, job->currentFrame, job->totalFrames);

                // Use accent color for progress bar, with full cell width and taller height to fill row
                float cellWidth = ImGui::GetContentRegionAvail().x;
                ImVec2 progressSize = ImVec2(cellWidth, 35.0f);  // Match row height for better fit
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
                ImGui::ProgressBar(job->progressPercent / 100.0f, progressSize, progressText);
                ImGui::PopStyleColor();
            } else if (job->status == TranscodeJob::Status::COPYING_METADATA) {
                float cellWidth = ImGui::GetContentRegionAvail().x;
                ImVec2 progressSize = ImVec2(cellWidth, 35.0f);  // Match row height for better fit
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
                ImGui::ProgressBar(0.95f, progressSize, "Copying metadata...");
                ImGui::PopStyleColor();
            } else if (job->status == TranscodeJob::Status::COMPLETED) {
                // Use accent color for completed (not green)
                float cellWidth = ImGui::GetContentRegionAvail().x;
                ImVec2 progressSize = ImVec2(cellWidth, 35.0f);  // Match row height for better fit
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accentColor);
                ImGui::ProgressBar(1.0f, progressSize, "Complete");
                ImGui::PopStyleColor();
            } else {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("--");
            }

            // Speed column
            ImGui::TableSetColumnIndex(3);
            ImGui::AlignTextToFramePadding();
            if (font_mono) ImGui::PushFont(font_mono);
            if (job->status == TranscodeJob::Status::PROCESSING && job->encodingFps > 0.0f) {
                ImGui::TextDisabled("%.1f fps", job->encodingFps);
            } else {
                ImGui::TextDisabled("--");
            }
            if (font_mono) ImGui::PopFont();

            // Time column
            ImGui::TableSetColumnIndex(4);
            ImGui::AlignTextToFramePadding();
            if (font_mono) ImGui::PushFont(font_mono);
            if (job->status != TranscodeJob::Status::QUEUED) {
                float elapsed = job->GetElapsedSeconds();
                int minutes = (int)(elapsed / 60.0f);
                int seconds = (int)elapsed % 60;
                ImGui::TextDisabled("%dm %ds", minutes, seconds);
            } else {
                ImGui::TextDisabled("--");
            }
            if (font_mono) ImGui::PopFont();
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar(); // Pop CellPadding
    ImGui::PopStyleColor(3); // Pop table colors
}

void TranscodeQueuePanel::RenderJobDetailsPanel() {
    if (m_selectedJobId.empty() || m_jobs.empty()) {
        ImGui::TextDisabled("No job selected");
        return;
    }

    // Find selected job
    TranscodeJob* selectedJob = nullptr;
    for (const auto& job : m_jobs) {
        if (job->id == m_selectedJobId) {
            selectedJob = job.get();
            break;
        }
    }

    if (!selectedJob) {
        ImGui::TextDisabled("Job not found");
        return;
    }

    ImGui::Text("Job ID: %s", selectedJob->id.c_str());
    ImGui::Separator();

    // Input path with "Open Location" buttons
    ImGui::Text("Input:  %s", WideToUtf8(selectedJob->inputPath).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Left Browser##InputLB")) {
        if (onOpenInLeftBrowser) {
            std::filesystem::path inputPath(selectedJob->inputPath);
            onOpenInLeftBrowser(inputPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open input file location in the Left Browser");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Right Browser##InputRB")) {
        if (onOpenInRightBrowser) {
            std::filesystem::path inputPath(selectedJob->inputPath);
            onOpenInRightBrowser(inputPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open input file location in the Right Browser");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("New Window##InputNW")) {
        if (onOpenInNewWindow) {
            std::filesystem::path inputPath(selectedJob->inputPath);
            onOpenInNewWindow(inputPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open input file location in a new window");
    }

    // Output path with "Open in Browser" buttons
    ImGui::Text("Output: %s", WideToUtf8(selectedJob->outputPath).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Left Browser##OutputLB")) {
        if (onOpenInLeftBrowser) {
            std::filesystem::path outputPath(selectedJob->outputPath);
            onOpenInLeftBrowser(outputPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open output file location (MP4 folder) in the Left Browser");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Right Browser##OutputRB")) {
        if (onOpenInRightBrowser) {
            std::filesystem::path outputPath(selectedJob->outputPath);
            onOpenInRightBrowser(outputPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open output file location (MP4 folder) in the Right Browser");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("New Window##OutputNW")) {
        if (onOpenInNewWindow) {
            std::filesystem::path outputPath(selectedJob->outputPath);
            onOpenInNewWindow(outputPath.parent_path().wstring());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open output file location (MP4 folder) in a new window");
    }

    ImGui::Separator();

    ImGui::Text("Status: %s", selectedJob->GetStatusString());

    if (selectedJob->status == TranscodeJob::Status::PROCESSING) {
        ImGui::Text("Progress: %.1f%% (%d / %d frames)",
                   selectedJob->progressPercent, selectedJob->currentFrame, selectedJob->totalFrames);
        ImGui::Text("Speed: %.1f fps", selectedJob->encodingFps);

        float elapsed = selectedJob->GetElapsedSeconds();
        if (selectedJob->encodingFps > 0.0f) {
            float remaining = (selectedJob->totalFrames - selectedJob->currentFrame) / selectedJob->encodingFps;
            ImGui::Text("Elapsed: %.0fs  |  ETA: %.0fs", elapsed, remaining);
        } else {
            ImGui::Text("Elapsed: %.0fs", elapsed);
        }
    } else if (selectedJob->status == TranscodeJob::Status::COMPLETED ||
               selectedJob->status == TranscodeJob::Status::FAILED ||
               selectedJob->status == TranscodeJob::Status::CANCELLED) {
        float elapsed = selectedJob->GetElapsedSeconds();
        ImGui::Text("Time: %.0fs", elapsed);
    }

    if (!selectedJob->errorMessage.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error:");
        ImGui::TextWrapped("%s", selectedJob->errorMessage.c_str());
    }
}

void TranscodeQueuePanel::AddJob(const std::wstring& inputPath) {
    auto job = std::make_unique<TranscodeJob>();
    job->id = GenerateJobId();
    job->inputPath = inputPath;
    job->outputPath = CreateOutputPath(inputPath);
    job->status = TranscodeJob::Status::QUEUED;
    job->queuedTime = std::chrono::system_clock::now();

    m_jobs.push_back(std::move(job));
}

void TranscodeQueuePanel::AddMultipleJobs(const std::vector<std::wstring>& inputPaths) {
    for (const auto& path : inputPaths) {
        AddJob(path);
    }
}

void TranscodeQueuePanel::RemoveJob(const std::string& jobId) {
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                [&jobId](const std::unique_ptr<TranscodeJob>& job) {
                                    return job->id == jobId;
                                }), m_jobs.end());

    if (m_selectedJobId == jobId) {
        m_selectedJobId.clear();
    }
}

void TranscodeQueuePanel::CancelCurrentJob() {
    if (!m_isProcessing) return;

    for (auto& job : m_jobs) {
        if (job->status == TranscodeJob::Status::PROCESSING ||
            job->status == TranscodeJob::Status::COPYING_METADATA) {

            // Terminate FFmpeg process if running
            if (m_ffmpegProcess) {
                TerminateProcess((HANDLE)m_ffmpegProcess, 1);
                CloseHandle((HANDLE)m_ffmpegProcess);
                m_ffmpegProcess = nullptr;
            }
            if (m_ffmpegPipe) {
                CloseHandle((HANDLE)m_ffmpegPipe);
                m_ffmpegPipe = nullptr;
            }

            CompleteJob(*job, false, "Cancelled by user");
            job->status = TranscodeJob::Status::CANCELLED;
            m_isProcessing = false;
            break;
        }
    }
}

void TranscodeQueuePanel::ClearCompleted() {
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                [](const std::unique_ptr<TranscodeJob>& job) {
                                    return job->status == TranscodeJob::Status::COMPLETED;
                                }), m_jobs.end());
}

void TranscodeQueuePanel::ClearAll() {
    CancelCurrentJob();
    m_jobs.clear();
    m_selectedJobId.clear();
}

size_t TranscodeQueuePanel::GetCompletedCount() const {
    size_t count = 0;
    for (const auto& job : m_jobs) {
        if (job->status == TranscodeJob::Status::COMPLETED) count++;
    }
    return count;
}

size_t TranscodeQueuePanel::GetFailedCount() const {
    size_t count = 0;
    for (const auto& job : m_jobs) {
        if (job->status == TranscodeJob::Status::FAILED) count++;
    }
    return count;
}

void TranscodeQueuePanel::Update() {
    // Process next job if not currently processing
    if (!m_isProcessing) {
        ProcessNextJob();
    } else {
        // Update current job progress
        for (auto& job : m_jobs) {
            if (job->status == TranscodeJob::Status::PROCESSING) {
                UpdateFFmpegProgress(*job);
                break;
            }
        }
    }
}

void TranscodeQueuePanel::ProcessNextJob() {
    // Find next queued job
    for (auto& job : m_jobs) {
        if (job->status == TranscodeJob::Status::QUEUED) {
            job->startedTime = std::chrono::system_clock::now();
            m_isProcessing = true;
            StartFFmpeg(*job);
            return;
        }
    }
}

std::wstring TranscodeQueuePanel::CreateOutputPath(const std::wstring& inputPath) {
    fs::path inputFilePath(inputPath);
    fs::path sourceDir = inputFilePath.parent_path();
    fs::path mp4Dir = sourceDir / L"MP4";

    // Create MP4 directory if it doesn't exist
    if (!fs::exists(mp4Dir)) {
        fs::create_directory(mp4Dir);
    }

    // Create output filename (same name but .mp4 extension)
    std::wstring outputFilename = inputFilePath.stem().wstring() + L".mp4";
    return (mp4Dir / outputFilename).wstring();
}

int TranscodeQueuePanel::GetTotalFrames(const std::wstring& inputPath) {
    // Use ffprobe to get total frames
    std::wstring cmdLine = L"\"" + m_ffprobePath + L"\" -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 \"" + inputPath + L"\"";

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return 0;
    }

    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return 0;
    }

    CloseHandle(hWritePipe);

    // Read output
    char buffer[256] = {};
    DWORD bytesRead = 0;
    ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
    CloseHandle(hReadPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return atoi(buffer);
}

std::string TranscodeQueuePanel::GenerateJobId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    ss << "job_" << std::hex;
    for (int i = 0; i < 8; ++i) {
        ss << dis(gen);
    }
    return ss.str();
}

void TranscodeQueuePanel::StartFFmpeg(TranscodeJob& job) {
    job.status = TranscodeJob::Status::PROCESSING;
    job.totalFrames = GetTotalFrames(job.inputPath);

    // Build FFmpeg command line (same as videoTranscoderApp)
    std::wstring cmdLine = L"\"" + m_ffmpegPath + L"\" -v quiet -progress pipe:1 -i \"" +
                          job.inputPath + L"\" -c:v libx264 -pix_fmt yuv420p -crf 25 -preset fast -c:a aac -b:a 192k -y \"" +
                          job.outputPath + L"\"";

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        CompleteJob(job, false, "Failed to create pipe");
        return;
    }

    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        CompleteJob(job, false, "Failed to start FFmpeg");
        return;
    }

    CloseHandle(hWritePipe);
    CloseHandle(pi.hThread);

    m_ffmpegProcess = pi.hProcess;
    m_ffmpegPipe = hReadPipe;
}

void TranscodeQueuePanel::UpdateFFmpegProgress(TranscodeJob& job) {
    if (!m_ffmpegPipe || !m_ffmpegProcess) return;

    // Check if process is still running
    DWORD exitCode = 0;
    GetExitCodeProcess((HANDLE)m_ffmpegProcess, &exitCode);

    if (exitCode != STILL_ACTIVE) {
        // Process finished
        CloseHandle((HANDLE)m_ffmpegProcess);
        CloseHandle((HANDLE)m_ffmpegPipe);
        m_ffmpegProcess = nullptr;
        m_ffmpegPipe = nullptr;

        if (exitCode == 0) {
            // Success - now copy metadata
            CopyMetadataWithExiftool(job);
        } else {
            CompleteJob(job, false, "FFmpeg failed with exit code " + std::to_string(exitCode));
        }
        return;
    }

    // Read progress output
    char buffer[4096] = {};
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;

    if (PeekNamedPipe((HANDLE)m_ffmpegPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr) && bytesAvailable > 0) {
        if (ReadFile((HANDLE)m_ffmpegPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buffer[bytesRead] = '\0';

            // Parse ffmpeg progress output (looking for "frame=XXX")
            char* framePos = strstr(buffer, "frame=");
            if (framePos) {
                int frame = atoi(framePos + 6);
                job.currentFrame = frame;
                if (job.totalFrames > 0) {
                    job.progressPercent = (frame * 100.0f) / job.totalFrames;
                }
            }

            // Parse fps
            char* fpsPos = strstr(buffer, "fps=");
            if (fpsPos) {
                job.encodingFps = (float)atof(fpsPos + 4);
            }
        }
    }
}

void TranscodeQueuePanel::CopyMetadataWithExiftool(TranscodeJob& job) {
    job.status = TranscodeJob::Status::COPYING_METADATA;

    // Build Exiftool command (same as videoTranscoderApp)
    std::wstring cmdLine = L"\"" + m_exiftoolPath + L"\" -TagsFromFile \"" + job.inputPath +
                          L"\" \"-AeProjectLinkFullPath>AeProjectLinkFullPath\" -overwrite_original \"" +
                          job.outputPath + L"\"";

    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CompleteJob(job, true, ""); // Continue anyway, metadata copy is optional
        return;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    CompleteJob(job, true, "");
}

void TranscodeQueuePanel::CompleteJob(TranscodeJob& job, bool success, const std::string& errorMsg) {
    job.completedTime = std::chrono::system_clock::now();

    if (success) {
        job.status = TranscodeJob::Status::COMPLETED;
        job.progressPercent = 100.0f;
        job.currentFrame = job.totalFrames;
    } else {
        job.status = TranscodeJob::Status::FAILED;
        job.errorMessage = errorMsg;
    }

    m_isProcessing = false;
}

} // namespace UFB
