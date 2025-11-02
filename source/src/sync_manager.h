#pragma once

#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include "metadata_manager.h"
#include "subscription_manager.h"
#include "backup_manager.h"

namespace UFB {

// Sync diff result
struct SyncDiff
{
    std::vector<Shot> remoteChanges;    // Shots updated/added from remote
    std::vector<Shot> localChanges;     // Shots updated locally (to push)
    std::vector<std::wstring> deletions; // Shots deleted (in cache but not in shared)
};

class SyncManager
{
public:
    SyncManager();
    ~SyncManager();

    // Initialize with dependencies
    bool Initialize(SubscriptionManager* subManager, MetadataManager* metaManager, BackupManager* backupManager);

    // Shutdown and cleanup
    void Shutdown();

    // Sync control
    void StartSync(std::chrono::seconds tickInterval = std::chrono::seconds(5));
    void StopSync();
    void ForceSyncJob(const std::wstring& jobPath);
    void ForceSyncAll();

    // Check if sync is running
    bool IsSyncing() const { return m_isRunning; }

private:
    // Dependencies
    SubscriptionManager* m_subManager = nullptr;
    MetadataManager* m_metaManager = nullptr;
    BackupManager* m_backupManager = nullptr;

    // Sync thread
    std::thread m_syncThread;
    std::atomic<bool> m_isRunning{false};
    std::chrono::seconds m_tickInterval;
    std::mutex m_syncMutex;
    std::condition_variable m_shutdownCV;
    std::mutex m_shutdownMutex;

    // Staggered sync state
    std::vector<std::wstring> m_activeJobPaths;
    size_t m_currentIndex = 0;
    std::map<std::wstring, uint64_t> m_lastSyncTimes;
    std::map<std::wstring, bool> m_firstSyncDone; // Track if first sync completed

    // Sync loop
    void SyncLoop();
    void SyncTick();

    // Per-job sync
    void SyncJob(const std::wstring& jobPath);
    SyncDiff ComputeDiff(const std::map<std::wstring, Shot>& cached,
                         const std::map<std::wstring, Shot>& shared);
    void ApplyRemoteChanges(const std::wstring& jobPath, const std::vector<Shot>& changes);
    void WriteLocalChangesToSharedJSON(const std::wstring& jobPath);

    // Conflict resolution
    bool ShouldAcceptRemoteChange(const Shot& local, const Shot& remote);

    // Backup integration
    void CreateBackupIfNeeded(const std::wstring& jobPath);

    // Helpers
    bool ShouldSync(const std::wstring& jobPath);
    bool HasLocalChanges(const std::wstring& jobPath);
    uint64_t GetLastSyncTime(const std::wstring& jobPath);
    void UpdateSyncTime(const std::wstring& jobPath, uint64_t timestamp);
    void RefreshActiveJobs();
};

} // namespace UFB
