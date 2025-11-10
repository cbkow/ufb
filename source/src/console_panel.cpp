#include "console_panel.h"
#include "utils.h"
#include <imgui.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace UFB {

// Global stream buffer instances (need to persist for lifetime of redirection)
static std::unique_ptr<ConsoleStreamBuf> g_coutBuf;
static std::unique_ptr<ConsoleStreamBuf> g_cerrBuf;
static std::unique_ptr<ConsoleWStreamBuf> g_wcoutBuf;
static std::unique_ptr<ConsoleWStreamBuf> g_wcerrBuf;

ConsolePanel::ConsolePanel()
    : m_isVisible(false)
    , m_autoScroll(true)
    , m_maxEntries(10000)
    , m_showInfo(true)
    , m_showWarnings(true)
    , m_showErrors(true)
{
    m_filterText[0] = '\0';
}

ConsolePanel::~ConsolePanel()
{
    Shutdown();
}

void ConsolePanel::Initialize()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_entries.reserve(1000);  // Pre-allocate for performance
    }
    // Log after releasing the lock to avoid deadlock
    LogInfo("Console panel initialized");
}

void ConsolePanel::Shutdown()
{
    RestoreStreams();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

void ConsolePanel::Log(LogLevel level, const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    uint64_t timestamp = GetCurrentTimeMs();
    m_entries.emplace_back(timestamp, level, message);

    // Trim old entries if we exceed max
    if (m_entries.size() > m_maxEntries)
    {
        TrimOldEntries();
    }
}

void ConsolePanel::LogInfo(const std::string& message)
{
    Log(LogLevel::Info, message);
}

void ConsolePanel::LogWarning(const std::string& message)
{
    Log(LogLevel::Warning, message);
}

void ConsolePanel::LogError(const std::string& message)
{
    Log(LogLevel::Error, message);
}

void ConsolePanel::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

void ConsolePanel::TrimOldEntries()
{
    // Remove oldest 20% of entries when we hit the limit
    size_t removeCount = m_maxEntries / 5;
    if (removeCount > 0 && m_entries.size() > removeCount)
    {
        m_entries.erase(m_entries.begin(), m_entries.begin() + removeCount);
    }
}

std::string ConsolePanel::FormatTimestamp(uint64_t timestamp) const
{
    auto ms = std::chrono::milliseconds(timestamp);
    auto tp = std::chrono::system_clock::time_point(ms);
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);

    std::tm tm_val;
#ifdef _WIN32
    localtime_s(&tm_val, &time_t_val);
#else
    localtime_r(&time_t_val, &tm_val);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%H:%M:%S");
    return oss.str();
}

std::string ConsolePanel::ExportToString() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ostringstream oss;

    oss << "Console Log Export\n";
    oss << "==================\n";
    oss << "Total entries: " << m_entries.size() << "\n";
    oss << "Export time: " << FormatTimestamp(GetCurrentTimeMs()) << "\n\n";

    for (const auto& entry : m_entries)
    {
        std::string levelStr;
        switch (entry.level)
        {
        case LogLevel::Info:    levelStr = "[INFO]    "; break;
        case LogLevel::Warning: levelStr = "[WARNING] "; break;
        case LogLevel::Error:   levelStr = "[ERROR]   "; break;
        }

        oss << FormatTimestamp(entry.timestamp) << " " << levelStr << entry.message << "\n";
    }

    return oss.str();
}

bool ConsolePanel::ExportToClipboard()
{
    std::string content = ExportToString();
    ImGui::SetClipboardText(content.c_str());
    LogInfo("Console log copied to clipboard");
    return true;
}

bool ConsolePanel::ExportToDesktop(const std::string& filename)
{
#ifdef _WIN32
    // Get desktop path
    PWSTR desktopPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktopPath);
    if (FAILED(hr) || !desktopPath)
    {
        LogError("Failed to get desktop path");
        return false;
    }

    std::wstring desktopWide(desktopPath);
    CoTaskMemFree(desktopPath);

    // Generate timestamp prefix (YYMMDD-HHMMSS)
    auto now = std::chrono::system_clock::now();
    auto time_t_val = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val;
#ifdef _WIN32
    localtime_s(&tm_val, &time_t_val);
#else
    localtime_r(&time_t_val, &tm_val);
#endif

    std::ostringstream timestampPrefix;
    timestampPrefix << std::setfill('0')
                    << std::setw(2) << (tm_val.tm_year % 100)  // YY
                    << std::setw(2) << (tm_val.tm_mon + 1)     // MM
                    << std::setw(2) << tm_val.tm_mday          // DD
                    << "-"
                    << std::setw(2) << tm_val.tm_hour          // HH
                    << std::setw(2) << tm_val.tm_min           // MM
                    << std::setw(2) << tm_val.tm_sec           // SS
                    << "_";

    // Construct full path with timestamp prefix
    std::filesystem::path outputPath = desktopWide;
    outputPath /= (timestampPrefix.str() + filename);

    // Write to file
    std::ofstream outFile(outputPath);
    if (!outFile.is_open())
    {
        LogError("Failed to create log file on desktop");
        return false;
    }

    outFile << ExportToString();
    outFile.close();

    LogInfo("Console log exported to: " + outputPath.string());
    return true;
#else
    LogError("Export to desktop not implemented for this platform");
    return false;
#endif
}

void ConsolePanel::RenderEntry(const ConsoleEntry& entry)
{
    // Apply filter
    if (m_filterText[0] != '\0')
    {
        if (entry.message.find(m_filterText) == std::string::npos)
        {
            return;  // Skip entries that don't match filter
        }
    }

    // Apply level filter
    if (entry.level == LogLevel::Info && !m_showInfo) return;
    if (entry.level == LogLevel::Warning && !m_showWarnings) return;
    if (entry.level == LogLevel::Error && !m_showErrors) return;

    // Color-code based on level
    ImVec4 color;
    switch (entry.level)
    {
    case LogLevel::Info:
        color = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);  // Light gray
        break;
    case LogLevel::Warning:
        color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);  // Yellow
        break;
    case LogLevel::Error:
        color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);  // Red
        break;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, color);

    // Format: [HH:MM:SS] [LEVEL] Message
    std::string timestamp = FormatTimestamp(entry.timestamp);
    std::string levelStr;
    switch (entry.level)
    {
    case LogLevel::Info:    levelStr = "[INFO]   "; break;
    case LogLevel::Warning: levelStr = "[WARN]   "; break;
    case LogLevel::Error:   levelStr = "[ERROR]  "; break;
    }

    ImGui::TextUnformatted((timestamp + " " + levelStr + entry.message).c_str());

    ImGui::PopStyleColor();
}

void ConsolePanel::Render(bool* p_open)
{
    if (!m_isVisible)
        return;

    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Console", p_open ? p_open : &m_isVisible))
    {
        ImGui::End();
        return;
    }

    // Toolbar
    if (ImGui::Button("Clear"))
    {
        Clear();
    }

    ImGui::SameLine();
    if (ImGui::Button("Copy to Clipboard"))
    {
        ExportToClipboard();
    }

    ImGui::SameLine();
    if (ImGui::Button("Export to Desktop"))
    {
        ExportToDesktop();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);

    // Filters
    ImGui::SameLine();
    ImGui::Checkbox("Info", &m_showInfo);

    ImGui::SameLine();
    ImGui::Checkbox("Warnings", &m_showWarnings);

    ImGui::SameLine();
    ImGui::Checkbox("Errors", &m_showErrors);

    // Search filter
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##filter", m_filterText, sizeof(m_filterText));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Filter messages (case-sensitive)");
    }

    ImGui::Separator();

    // Console output area
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (const auto& entry : m_entries)
        {
            RenderEntry(entry);
        }
    }

    // Auto-scroll to bottom
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::End();
}

void ConsolePanel::RedirectStreams()
{
    // Save old buffers
    m_oldCoutBuf = std::cout.rdbuf();
    m_oldCerrBuf = std::cerr.rdbuf();
    m_oldWcoutBuf = std::wcout.rdbuf();
    m_oldWcerrBuf = std::wcerr.rdbuf();

    // Create new buffers
    g_coutBuf = std::make_unique<ConsoleStreamBuf>(this, LogLevel::Info);
    g_cerrBuf = std::make_unique<ConsoleStreamBuf>(this, LogLevel::Error);
    g_wcoutBuf = std::make_unique<ConsoleWStreamBuf>(this, LogLevel::Info);
    g_wcerrBuf = std::make_unique<ConsoleWStreamBuf>(this, LogLevel::Error);

    // Redirect streams
    std::cout.rdbuf(g_coutBuf.get());
    std::cerr.rdbuf(g_cerrBuf.get());
    std::wcout.rdbuf(g_wcoutBuf.get());
    std::wcerr.rdbuf(g_wcerrBuf.get());

    // Log AFTER redirection is complete (this will go through the redirected stream)
    LogInfo("Console stream redirection enabled");
}

void ConsolePanel::RestoreStreams()
{
    if (m_oldCoutBuf)
    {
        std::cout.rdbuf(m_oldCoutBuf);
        m_oldCoutBuf = nullptr;
    }
    if (m_oldCerrBuf)
    {
        std::cerr.rdbuf(m_oldCerrBuf);
        m_oldCerrBuf = nullptr;
    }
    if (m_oldWcoutBuf)
    {
        std::wcout.rdbuf(m_oldWcoutBuf);
        m_oldWcoutBuf = nullptr;
    }
    if (m_oldWcerrBuf)
    {
        std::wcerr.rdbuf(m_oldWcerrBuf);
        m_oldWcerrBuf = nullptr;
    }

    // Clean up buffers
    g_coutBuf.reset();
    g_cerrBuf.reset();
    g_wcoutBuf.reset();
    g_wcerrBuf.reset();
}

// ConsoleStreamBuf implementation
int ConsoleStreamBuf::overflow(int_type c)
{
    if (c != EOF)
    {
        m_buffer += static_cast<char>(c);

        // If we hit a newline, flush the buffer
        if (c == '\n')
        {
            sync();
        }
    }
    return c;
}

int ConsoleStreamBuf::sync()
{
    if (!m_buffer.empty())
    {
        // Remove trailing newline if present
        if (m_buffer.back() == '\n')
        {
            m_buffer.pop_back();
        }

        // Remove trailing carriage return if present
        if (!m_buffer.empty() && m_buffer.back() == '\r')
        {
            m_buffer.pop_back();
        }

        // Log the message if it's not empty
        if (!m_buffer.empty() && m_panel)
        {
            m_panel->Log(m_level, m_buffer);
        }

        m_buffer.clear();
    }
    return 0;
}

// ConsoleWStreamBuf implementation
std::wstreambuf::int_type ConsoleWStreamBuf::overflow(int_type c)
{
    if (c != WEOF)
    {
        m_buffer += static_cast<wchar_t>(c);

        // If we hit a newline, flush the buffer
        if (c == L'\n')
        {
            sync();
        }
    }
    return c;
}

int ConsoleWStreamBuf::sync()
{
    if (!m_buffer.empty())
    {
        // Remove trailing newline if present
        if (m_buffer.back() == L'\n')
        {
            m_buffer.pop_back();
        }

        // Remove trailing carriage return if present
        if (!m_buffer.empty() && m_buffer.back() == L'\r')
        {
            m_buffer.pop_back();
        }

        // Log the message if it's not empty (convert to UTF-8)
        if (!m_buffer.empty() && m_panel)
        {
            // Convert wide string to UTF-8
            std::string utf8Message;
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, m_buffer.c_str(), (int)m_buffer.size(), NULL, 0, NULL, NULL);
            if (size_needed > 0)
            {
                utf8Message.resize(size_needed);
                WideCharToMultiByte(CP_UTF8, 0, m_buffer.c_str(), (int)m_buffer.size(), &utf8Message[0], size_needed, NULL, NULL);
            }

            if (!utf8Message.empty())
            {
                m_panel->Log(m_level, utf8Message);
            }
        }

        m_buffer.clear();
    }
    return 0;
}

} // namespace UFB
