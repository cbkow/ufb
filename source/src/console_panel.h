#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <sstream>
#include <streambuf>
#include <iostream>

namespace UFB {

// Forward declaration
class ConsolePanel;

// Log level for console messages
enum class LogLevel
{
    Info,
    Warning,
    Error
};

// Console entry structure
struct ConsoleEntry
{
    uint64_t timestamp;      // Unix timestamp in milliseconds
    LogLevel level;
    std::string message;

    ConsoleEntry(uint64_t ts, LogLevel lvl, const std::string& msg)
        : timestamp(ts), level(lvl), message(msg) {}
};

class ConsolePanel
{
public:
    ConsolePanel();
    ~ConsolePanel();

    // Initialize the console panel
    void Initialize();

    // Shutdown and cleanup
    void Shutdown();

    // Add a log entry (thread-safe)
    void Log(LogLevel level, const std::string& message);
    void LogInfo(const std::string& message);
    void LogWarning(const std::string& message);
    void LogError(const std::string& message);

    // Clear all entries (thread-safe)
    void Clear();

    // Export functionality
    std::string ExportToString() const;
    bool ExportToClipboard();
    bool ExportToDesktop(const std::string& filename = "console_log.txt");

    // ImGui rendering
    void Render(bool* p_open = nullptr);

    // Visibility control
    void Show() { m_isVisible = true; }
    void Hide() { m_isVisible = false; }
    void Toggle() { m_isVisible = !m_isVisible; }
    bool IsVisible() const { return m_isVisible; }

    // Settings
    void SetAutoScroll(bool enabled) { m_autoScroll = enabled; }
    bool GetAutoScroll() const { return m_autoScroll; }

    void SetMaxEntries(size_t max) { m_maxEntries = max; }
    size_t GetMaxEntries() const { return m_maxEntries; }

    // Stream redirection support
    void RedirectStreams();
    void RestoreStreams();

private:
    std::vector<ConsoleEntry> m_entries;
    mutable std::mutex m_mutex;

    bool m_isVisible;
    bool m_autoScroll;
    size_t m_maxEntries;

    // Filter settings
    bool m_showInfo;
    bool m_showWarnings;
    bool m_showErrors;
    char m_filterText[256];

    // Helper methods
    void TrimOldEntries();
    std::string FormatTimestamp(uint64_t timestamp) const;
    void RenderEntry(const ConsoleEntry& entry);

    // Stream redirection
    std::streambuf* m_oldCoutBuf = nullptr;
    std::streambuf* m_oldCerrBuf = nullptr;
    std::wstreambuf* m_oldWcoutBuf = nullptr;
    std::wstreambuf* m_oldWcerrBuf = nullptr;
};

// Custom stream buffer that routes output to ConsolePanel
class ConsoleStreamBuf : public std::streambuf
{
public:
    ConsoleStreamBuf(ConsolePanel* panel, LogLevel level, bool isWide = false)
        : m_panel(panel), m_level(level), m_isWide(isWide) {}

protected:
    virtual int_type overflow(int_type c) override;
    virtual int sync() override;

private:
    ConsolePanel* m_panel;
    LogLevel m_level;
    bool m_isWide;
    std::string m_buffer;
};

// Wide character stream buffer
class ConsoleWStreamBuf : public std::wstreambuf
{
public:
    ConsoleWStreamBuf(ConsolePanel* panel, LogLevel level)
        : m_panel(panel), m_level(level) {}

protected:
    virtual int_type overflow(int_type c) override;
    virtual int sync() override;

private:
    ConsolePanel* m_panel;
    LogLevel m_level;
    std::wstring m_buffer;
};

} // namespace UFB
