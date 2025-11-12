#pragma once

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace UFB {

// Forward declarations
class SubscriptionManager;

struct TrackedJob {
    std::wstring jobPath;
    std::wstring jobName;
    uint64_t subscribedTime;
    int shotCount;
};

struct ClientTrackingFile {
    std::string version;
    std::string deviceId;
    std::string deviceName;
    std::string mode;  // "client" or "server"
    uint64_t lastUpdated;
    std::vector<TrackedJob> jobs;
};

class ClientTrackingManager {
public:
    ClientTrackingManager();
    ~ClientTrackingManager();

    // Initialize with subscription manager and device ID
    bool Initialize(SubscriptionManager* subscriptionManager, const std::wstring& deviceId, const std::wstring& deviceName);

    // Set tracking directory
    void SetTrackingDirectory(const std::string& directory);
    std::string GetTrackingDirectory() const { return m_trackingDirectory; }

    // Set operating mode
    void SetOperatingMode(const std::string& mode);
    std::string GetOperatingMode() const { return m_operatingMode; }

    // Client mode: Write own tracking file
    bool WriteOwnTrackingFile();

    // Server mode: Read all client tracking files
    std::vector<TrackedJob> ReadAllClientTrackingFiles();

    // Server mode: Prune a job from all client tracking files
    bool PruneJobFromAllClients(const std::wstring& jobPath);

    // Startup: Sync database to match tracking file
    bool SyncDatabaseToTrackingFile();

    // Test directory accessibility
    bool TestDirectoryAccess(const std::string& directory, std::string& errorMessage);

    // Server mode: Start/stop background sync loop
    void StartServerSyncLoop(std::chrono::seconds interval = std::chrono::seconds(30));
    void StopServerSyncLoop();

private:
    SubscriptionManager* m_subscriptionManager;
    std::wstring m_deviceId;
    std::wstring m_deviceName;
    std::string m_trackingDirectory;
    std::string m_operatingMode;  // "client" or "server"

    // Server mode sync loop
    std::thread m_serverSyncThread;
    std::atomic<bool> m_serverSyncRunning;
    std::mutex m_serverSyncMutex;
    std::condition_variable m_serverSyncCV;

    void ServerSyncLoop(std::chrono::seconds interval);

    // Helper methods
    std::string GetOwnTrackingFilePath() const;
    std::vector<std::string> GetAllClientTrackingFilePaths() const;

    bool ReadTrackingFile(const std::string& filePath, ClientTrackingFile& outData);
    bool WriteTrackingFile(const std::string& filePath, const ClientTrackingFile& data);

    std::string WideToUtf8(const std::wstring& wstr) const;
    std::wstring Utf8ToWide(const std::string& str) const;

    uint64_t GetCurrentTimestamp() const;
};

} // namespace UFB
