#pragma once

#include <string>
#include <vector>
#include <map>
#include <queue>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include "metadata_manager.h"
#include "subscription_manager.h"
#include "backup_manager.h"
#include "archival_manager.h"
#include "project_config.h"
#include "file_watcher.h"
#include "p2p_manager.h"

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

    // Get device ID
    std::wstring GetDeviceId() const { return m_deviceId; }

private:
    // Dependencies
    SubscriptionManager* m_subManager = nullptr;
    MetadataManager* m_metaManager = nullptr;
    BackupManager* m_backupManager = nullptr;
    std::unique_ptr<ArchivalManager> m_archivalManager;  // Change log archival and compression

    // P2P networking for real-time notifications (single global manager)
    std::unique_ptr<P2PManager> m_p2pManager;  // Single P2P manager for all projects
    std::wstring m_deviceId;  // Unique device identifier

    // Sync thread (fallback polling)
    std::thread m_syncThread;
    std::atomic<bool> m_isRunning{false};
    std::chrono::seconds m_tickInterval;
    std::condition_variable m_shutdownCV;
    std::mutex m_shutdownMutex;

    // Work queue for async sync operations
    std::thread m_syncWorkerThread;
    std::queue<std::wstring> m_syncQueue;
    std::set<std::wstring> m_queuedJobs;  // Deduplication set
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;

    // File watching for real-time sync
    std::unique_ptr<FileWatcher> m_fileWatcher;
    std::map<std::wstring, bool> m_watchedJobs;  // Track which jobs have file watchers

    // Staggered sync state
    std::vector<std::wstring> m_activeJobPaths;
    size_t m_currentIndex = 0;
    std::map<std::wstring, uint64_t> m_lastSyncTimes;
    std::map<std::wstring, uint64_t> m_lastArchivalTimes;  // Track last archival per job
    std::map<std::wstring, bool> m_firstSyncDone; // Track if first sync completed

    // P2P change tracking (for content verification)
    struct ExpectedChange {
        std::wstring deviceId;
        uint64_t timestamp;
    };
    std::map<std::wstring, ExpectedChange> m_expectedChanges;  // jobPath -> expected change
    std::mutex m_expectedChangesMutex;

    // Sync loop (fallback polling)
    void SyncLoop();
    void SyncTick();

    // Work queue processing
    void SyncWorkerThread();
    void PostSyncJob(const std::wstring& jobPath);

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

    // Archival helpers
    bool ShouldRunArchival(const std::wstring& jobPath);
    void RunArchival(const std::wstring& jobPath);

    // File watching
    void SetupFileWatcher(const std::wstring& jobPath);
    void OnFileChanged(const std::wstring& jobPath);

    // P2P networking
    void SetupP2PForJob(const std::wstring& jobPath);
    void OnP2PChangeReceived(const std::wstring& jobPath, const std::wstring& peerDeviceId, uint64_t timestamp);
    std::wstring GetOrCreateDeviceId();

    // Shot metadata discovery
    void DiscoverAndTrackShots(const std::wstring& jobPath);
    void DiscoverShotsInCategory(const std::wstring& categoryPath, const std::wstring& jobPath, const std::string& folderType, const ProjectConfig& config);
};

} // namespace UFB
