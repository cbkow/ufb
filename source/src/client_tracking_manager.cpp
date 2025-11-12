#include "client_tracking_manager.h"
#include "subscription_manager.h"
#include "utils.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>

namespace UFB {

ClientTrackingManager::ClientTrackingManager()
    : m_subscriptionManager(nullptr)
    , m_operatingMode("client")
    , m_serverSyncRunning(false)
{
}

ClientTrackingManager::~ClientTrackingManager()
{
    StopServerSyncLoop();
}

bool ClientTrackingManager::Initialize(SubscriptionManager* subscriptionManager,
                                        const std::wstring& deviceId,
                                        const std::wstring& deviceName)
{
    if (!subscriptionManager) {
        std::cerr << "[ClientTrackingManager] ERROR: SubscriptionManager is null" << std::endl;
        return false;
    }

    m_subscriptionManager = subscriptionManager;
    m_deviceId = deviceId;
    m_deviceName = deviceName;

    std::cout << "[ClientTrackingManager] Initialized with device: "
              << WideToUtf8(deviceName) << " (" << WideToUtf8(deviceId) << ")" << std::endl;

    return true;
}

void ClientTrackingManager::SetTrackingDirectory(const std::string& directory)
{
    m_trackingDirectory = directory;
    std::cout << "[ClientTrackingManager] Tracking directory set to: " << directory << std::endl;
}

void ClientTrackingManager::SetOperatingMode(const std::string& mode)
{
    if (mode != "client" && mode != "server") {
        std::cerr << "[ClientTrackingManager] ERROR: Invalid operating mode: " << mode << std::endl;
        return;
    }

    std::string oldMode = m_operatingMode;
    m_operatingMode = mode;

    std::cout << "[ClientTrackingManager] Operating mode changed: " << oldMode << " -> " << mode << std::endl;
}

std::string ClientTrackingManager::GetOwnTrackingFilePath() const
{
    if (m_trackingDirectory.empty()) {
        return "";
    }

    std::string deviceIdStr = WideToUtf8(m_deviceId);
    return m_trackingDirectory + "\\client-tracking-" + deviceIdStr + ".json";
}

std::vector<std::string> ClientTrackingManager::GetAllClientTrackingFilePaths() const
{
    std::vector<std::string> paths;

    if (m_trackingDirectory.empty()) {
        return paths;
    }

    try {
        std::filesystem::path dirPath(m_trackingDirectory);

        if (!std::filesystem::exists(dirPath)) {
            std::cerr << "[ClientTrackingManager] WARNING: Tracking directory does not exist: "
                      << m_trackingDirectory << std::endl;
            return paths;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();

                // Only include client-tracking-*.json files, skip .tmp files
                if (filename.starts_with("client-tracking-") &&
                    filename.ends_with(".json") &&
                    !filename.ends_with(".tmp")) {
                    paths.push_back(entry.path().string());
                }
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[ClientTrackingManager] ERROR: Failed to scan tracking directory: "
                  << e.what() << std::endl;
    }

    return paths;
}

bool ClientTrackingManager::ReadTrackingFile(const std::string& filePath, ClientTrackingFile& outData)
{
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            std::cerr << "[ClientTrackingManager] WARNING: Could not open tracking file: "
                      << filePath << std::endl;
            return false;
        }

        json j;
        file >> j;
        file.close();

        outData.version = j.value("version", "1");
        outData.deviceId = j.value("deviceId", "");
        outData.deviceName = j.value("deviceName", "");
        outData.mode = j.value("mode", "client");
        outData.lastUpdated = j.value("lastUpdated", 0ULL);

        outData.jobs.clear();
        if (j.contains("jobs") && j["jobs"].is_array()) {
            for (const auto& jobJson : j["jobs"]) {
                TrackedJob job;
                job.jobPath = Utf8ToWide(jobJson.value("jobPath", ""));
                job.jobName = Utf8ToWide(jobJson.value("jobName", ""));
                job.subscribedTime = jobJson.value("subscribedTime", 0ULL);
                job.shotCount = jobJson.value("shotCount", 0);

                if (!job.jobPath.empty()) {
                    outData.jobs.push_back(job);
                }
            }
        }

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[ClientTrackingManager] ERROR: Failed to read tracking file: "
                  << filePath << " - " << e.what() << std::endl;
        return false;
    }
}

bool ClientTrackingManager::WriteTrackingFile(const std::string& filePath, const ClientTrackingFile& data)
{
    try {
        json j;
        j["version"] = data.version;
        j["deviceId"] = data.deviceId;
        j["deviceName"] = data.deviceName;
        j["mode"] = data.mode;
        j["lastUpdated"] = data.lastUpdated;

        j["jobs"] = json::array();
        for (const auto& job : data.jobs) {
            json jobJson;
            jobJson["jobPath"] = WideToUtf8(job.jobPath);
            jobJson["jobName"] = WideToUtf8(job.jobName);
            jobJson["subscribedTime"] = job.subscribedTime;
            jobJson["shotCount"] = job.shotCount;
            j["jobs"].push_back(jobJson);
        }

        // Atomic write: write to .tmp file then rename
        std::string tmpFilePath = filePath + ".tmp";
        std::ofstream file(tmpFilePath);
        if (!file.is_open()) {
            std::cerr << "[ClientTrackingManager] ERROR: Could not open temp file for writing: "
                      << tmpFilePath << std::endl;
            return false;
        }

        file << j.dump(2);  // Pretty print with 2-space indent
        file.close();

        // Rename temp file to final file
        std::filesystem::rename(tmpFilePath, filePath);

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[ClientTrackingManager] ERROR: Failed to write tracking file: "
                  << filePath << " - " << e.what() << std::endl;
        return false;
    }
}

bool ClientTrackingManager::WriteOwnTrackingFile()
{
    // Guard: Only write in client mode
    if (m_operatingMode != "client") {
        return false;
    }

    if (m_trackingDirectory.empty()) {
        std::cerr << "[ClientTrackingManager] WARNING: Cannot write tracking file - directory not set" << std::endl;
        return false;
    }

    if (!m_subscriptionManager) {
        std::cerr << "[ClientTrackingManager] ERROR: SubscriptionManager is null" << std::endl;
        return false;
    }

    try {
        // Ensure tracking directory exists
        std::filesystem::create_directories(m_trackingDirectory);

        // Get all current subscriptions
        auto subscriptions = m_subscriptionManager->GetAllSubscriptions();

        // Build tracking file data
        ClientTrackingFile trackingData;
        trackingData.version = "1";
        trackingData.deviceId = WideToUtf8(m_deviceId);
        trackingData.deviceName = WideToUtf8(m_deviceName);
        trackingData.mode = m_operatingMode;
        trackingData.lastUpdated = GetCurrentTimestamp();

        for (const auto& sub : subscriptions) {
            TrackedJob job;
            job.jobPath = sub.jobPath;
            job.jobName = sub.jobName;
            job.subscribedTime = sub.subscribedTime;
            job.shotCount = sub.shotCount;
            trackingData.jobs.push_back(job);
        }

        // Write to file
        std::string filePath = GetOwnTrackingFilePath();
        if (WriteTrackingFile(filePath, trackingData)) {
            std::cout << "[ClientTrackingManager] Wrote tracking file with "
                      << trackingData.jobs.size() << " jobs" << std::endl;
            return true;
        }

        return false;
    }
    catch (const std::exception& e) {
        std::cerr << "[ClientTrackingManager] ERROR: Failed to write own tracking file: "
                  << e.what() << std::endl;
        return false;
    }
}

std::vector<TrackedJob> ClientTrackingManager::ReadAllClientTrackingFiles()
{
    std::vector<TrackedJob> allJobs;

    // Guard: Only read in server mode
    if (m_operatingMode != "server") {
        return allJobs;
    }

    if (m_trackingDirectory.empty()) {
        std::cerr << "[ClientTrackingManager] WARNING: Cannot read tracking files - directory not set" << std::endl;
        return allJobs;
    }

    try {
        auto filePaths = GetAllClientTrackingFilePaths();

        std::cout << "[ClientTrackingManager] Reading " << filePaths.size()
                  << " client tracking files..." << std::endl;

        // Use a map to deduplicate jobs by path
        std::map<std::wstring, TrackedJob> uniqueJobs;

        for (const auto& filePath : filePaths) {
            ClientTrackingFile trackingData;
            if (ReadTrackingFile(filePath, trackingData)) {
                // Filter out server-mode files and own device
                if (trackingData.mode == "server" || trackingData.deviceId == WideToUtf8(m_deviceId)) {
                    std::cout << "[ClientTrackingManager] Skipping server/own file: "
                              << filePath << std::endl;
                    continue;
                }

                // Add all jobs from this client
                for (const auto& job : trackingData.jobs) {
                    uniqueJobs[job.jobPath] = job;
                }

                std::cout << "[ClientTrackingManager] Read " << trackingData.jobs.size()
                          << " jobs from client: " << trackingData.deviceName << std::endl;
            }
        }

        // Convert map to vector
        for (const auto& [jobPath, job] : uniqueJobs) {
            allJobs.push_back(job);
        }

        std::cout << "[ClientTrackingManager] Total unique jobs from all clients: "
                  << allJobs.size() << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "[ClientTrackingManager] ERROR: Failed to read client tracking files: "
                  << e.what() << std::endl;
    }

    return allJobs;
}

bool ClientTrackingManager::PruneJobFromAllClients(const std::wstring& jobPath)
{
    // Guard: Only prune in server mode
    if (m_operatingMode != "server") {
        std::cerr << "[ClientTrackingManager] WARNING: Cannot prune - only available in server mode" << std::endl;
        return false;
    }

    if (m_trackingDirectory.empty()) {
        std::cerr << "[ClientTrackingManager] WARNING: Cannot prune - directory not set" << std::endl;
        return false;
    }

    try {
        auto filePaths = GetAllClientTrackingFilePaths();

        int successCount = 0;
        int failCount = 0;
        std::vector<std::string> failedFiles;

        std::cout << "[ClientTrackingManager] Pruning job from " << filePaths.size()
                  << " client tracking files..." << std::endl;

        for (const auto& filePath : filePaths) {
            ClientTrackingFile trackingData;
            if (!ReadTrackingFile(filePath, trackingData)) {
                failCount++;
                failedFiles.push_back(filePath);
                continue;
            }

            // Remove the job from the jobs list
            auto it = std::remove_if(trackingData.jobs.begin(), trackingData.jobs.end(),
                [&jobPath](const TrackedJob& job) {
                    return job.jobPath == jobPath;
                });

            if (it != trackingData.jobs.end()) {
                trackingData.jobs.erase(it, trackingData.jobs.end());
                trackingData.lastUpdated = GetCurrentTimestamp();

                if (WriteTrackingFile(filePath, trackingData)) {
                    successCount++;
                    std::cout << "[ClientTrackingManager] Pruned job from: " << filePath << std::endl;
                } else {
                    failCount++;
                    failedFiles.push_back(filePath);
                }
            }
        }

        std::cout << "[ClientTrackingManager] Prune complete: " << successCount << " succeeded, "
                  << failCount << " failed" << std::endl;

        if (!failedFiles.empty()) {
            std::cerr << "[ClientTrackingManager] WARNING: Failed to prune from "
                      << failedFiles.size() << " files" << std::endl;
        }

        return failCount == 0;
    }
    catch (const std::exception& e) {
        std::cerr << "[ClientTrackingManager] ERROR: Failed to prune job: " << e.what() << std::endl;
        return false;
    }
}

bool ClientTrackingManager::SyncDatabaseToTrackingFile()
{
    if (m_trackingDirectory.empty()) {
        std::cout << "[ClientTrackingManager] Tracking directory not set - skipping database sync" << std::endl;
        return true;  // Not an error, just not configured
    }

    if (!m_subscriptionManager) {
        std::cerr << "[ClientTrackingManager] ERROR: SubscriptionManager is null" << std::endl;
        return false;
    }

    try {
        std::string filePath = GetOwnTrackingFilePath();

        // Check if tracking file exists
        if (!std::filesystem::exists(filePath)) {
            std::cout << "[ClientTrackingManager] Tracking file does not exist - creating initial file" << std::endl;
            return WriteOwnTrackingFile();
        }

        // Read tracking file
        ClientTrackingFile trackingData;
        if (!ReadTrackingFile(filePath, trackingData)) {
            std::cerr << "[ClientTrackingManager] ERROR: Failed to read tracking file for sync" << std::endl;
            return false;
        }

        // Get current subscriptions from database
        auto currentSubscriptions = m_subscriptionManager->GetAllSubscriptions();

        // Build maps for easy lookup
        std::map<std::wstring, bool> fileJobs;
        for (const auto& job : trackingData.jobs) {
            fileJobs[job.jobPath] = true;
        }

        std::map<std::wstring, bool> dbJobs;
        for (const auto& sub : currentSubscriptions) {
            dbJobs[sub.jobPath] = true;
        }

        // Add jobs that are in file but not in database
        for (const auto& job : trackingData.jobs) {
            if (dbJobs.find(job.jobPath) == dbJobs.end()) {
                std::cout << "[ClientTrackingManager] Adding job from tracking file: "
                          << WideToUtf8(job.jobName) << std::endl;
                m_subscriptionManager->SubscribeToJob(job.jobPath, job.jobName);
            }
        }

        // Remove jobs that are in database but not in file
        for (const auto& sub : currentSubscriptions) {
            if (fileJobs.find(sub.jobPath) == fileJobs.end()) {
                std::cout << "[ClientTrackingManager] Removing job not in tracking file: "
                          << WideToUtf8(sub.jobName) << std::endl;
                m_subscriptionManager->UnsubscribeFromJob(sub.jobPath);
            }
        }

        std::cout << "[ClientTrackingManager] Database synced to tracking file successfully" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[ClientTrackingManager] ERROR: Failed to sync database to tracking file: "
                  << e.what() << std::endl;
        return false;
    }
}

bool ClientTrackingManager::TestDirectoryAccess(const std::string& directory, std::string& errorMessage)
{
    try {
        // Check if directory exists or can be created
        std::filesystem::path dirPath(directory);

        if (!std::filesystem::exists(dirPath)) {
            // Try to create it
            if (!std::filesystem::create_directories(dirPath)) {
                errorMessage = "Failed to create directory";
                return false;
            }
        }

        // Try to write a test file
        std::string testFilePath = directory + "\\test-access.tmp";
        std::ofstream testFile(testFilePath);
        if (!testFile.is_open()) {
            errorMessage = "Cannot write to directory";
            return false;
        }
        testFile << "test";
        testFile.close();

        // Try to delete the test file
        std::filesystem::remove(testFilePath);

        errorMessage = "Success";
        return true;
    }
    catch (const std::exception& e) {
        errorMessage = std::string("Exception: ") + e.what();
        return false;
    }
}

std::string ClientTrackingManager::WideToUtf8(const std::wstring& wstr) const
{
    return UFB::WideToUtf8(wstr);
}

std::wstring ClientTrackingManager::Utf8ToWide(const std::string& str) const
{
    return UFB::Utf8ToWide(str);
}

uint64_t ClientTrackingManager::GetCurrentTimestamp() const
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}

void ClientTrackingManager::StartServerSyncLoop(std::chrono::seconds interval)
{
    // Guard: Only start in server mode
    if (m_operatingMode != "server") {
        std::cout << "[ClientTrackingManager] Not starting server sync loop - not in server mode" << std::endl;
        return;
    }

    if (m_serverSyncRunning) {
        std::cout << "[ClientTrackingManager] Server sync loop already running" << std::endl;
        return;
    }

    m_serverSyncRunning = true;
    m_serverSyncThread = std::thread(&ClientTrackingManager::ServerSyncLoop, this, interval);

    std::cout << "[ClientTrackingManager] Server sync loop started (interval: "
              << interval.count() << " seconds)" << std::endl;
}

void ClientTrackingManager::StopServerSyncLoop()
{
    if (!m_serverSyncRunning) {
        return;
    }

    m_serverSyncRunning = false;
    m_serverSyncCV.notify_all();

    if (m_serverSyncThread.joinable()) {
        m_serverSyncThread.join();
    }

    std::cout << "[ClientTrackingManager] Server sync loop stopped" << std::endl;
}

void ClientTrackingManager::ServerSyncLoop(std::chrono::seconds interval)
{
    std::cout << "[ClientTrackingManager] Server sync loop thread started" << std::endl;

    while (m_serverSyncRunning)
    {
        try {
            // Guard: Only sync in server mode
            if (m_operatingMode != "server") {
                std::cout << "[ClientTrackingManager] Server sync loop detected mode change - exiting" << std::endl;
                break;
            }

            // Read all client tracking files
            auto clientJobs = ReadAllClientTrackingFiles();

            if (!clientJobs.empty())
            {
                // Get current subscriptions
                auto currentSubs = m_subscriptionManager->GetAllSubscriptions();

                // Build maps for comparison
                std::map<std::wstring, TrackedJob> clientJobsMap;
                for (const auto& job : clientJobs) {
                    clientJobsMap[job.jobPath] = job;
                }

                std::map<std::wstring, bool> currentSubsMap;
                for (const auto& sub : currentSubs) {
                    currentSubsMap[sub.jobPath] = true;
                }

                // Subscribe to jobs that clients have but we don't
                for (const auto& [jobPath, job] : clientJobsMap) {
                    if (currentSubsMap.find(jobPath) == currentSubsMap.end()) {
                        std::wcout << L"[ServerSyncLoop] Subscribing to client job: "
                                   << job.jobName << std::endl;
                        m_subscriptionManager->SubscribeToJob(job.jobPath, job.jobName);
                    }
                }

                // Unsubscribe from jobs that we have but no clients have
                for (const auto& sub : currentSubs) {
                    if (clientJobsMap.find(sub.jobPath) == clientJobsMap.end()) {
                        std::wcout << L"[ServerSyncLoop] Unsubscribing from job (no clients): "
                                   << sub.jobName << std::endl;
                        m_subscriptionManager->UnsubscribeFromJob(sub.jobPath);
                    }
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[ClientTrackingManager] Server sync loop error: " << e.what() << std::endl;
        }

        // Wait for interval or until stopped
        std::unique_lock<std::mutex> lock(m_serverSyncMutex);
        m_serverSyncCV.wait_for(lock, interval, [this] { return !m_serverSyncRunning; });
    }

    std::cout << "[ClientTrackingManager] Server sync loop thread ended" << std::endl;
}

} // namespace UFB
