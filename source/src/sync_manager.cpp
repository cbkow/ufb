#include "sync_manager.h"
#include "utils.h"
#include <iostream>
#include <algorithm>

namespace UFB {

SyncManager::SyncManager()
{
}

SyncManager::~SyncManager()
{
    Shutdown();
}

bool SyncManager::Initialize(SubscriptionManager* subManager, MetadataManager* metaManager, BackupManager* backupManager)
{
    m_subManager = subManager;
    m_metaManager = metaManager;
    m_backupManager = backupManager;

    if (!m_subManager || !m_metaManager || !m_backupManager)
    {
        std::cerr << "SyncManager: Invalid dependencies" << std::endl;
        return false;
    }

    return true;
}

void SyncManager::Shutdown()
{
    StopSync();
}

void SyncManager::StartSync(std::chrono::seconds tickInterval)
{
    if (m_isRunning)
    {
        std::cerr << "SyncManager: Already running" << std::endl;
        return;
    }

    m_tickInterval = tickInterval;
    m_isRunning = true;

    // Start sync thread
    m_syncThread = std::thread(&SyncManager::SyncLoop, this);

    std::cout << "SyncManager: Started (tick interval: " << tickInterval.count() << "s)" << std::endl;
}

void SyncManager::StopSync()
{
    if (!m_isRunning)
    {
        return;
    }

    m_isRunning = false;

    // Wake up the sync thread immediately
    m_shutdownCV.notify_one();

    if (m_syncThread.joinable())
    {
        try
        {
            m_syncThread.join();
        }
        catch (const std::system_error& e)
        {
            std::cerr << "SyncManager: Thread join error: " << e.what() << std::endl;
            // Detach to prevent terminate() call in destructor
            try { m_syncThread.detach(); } catch (...) {}
        }
        catch (...)
        {
            std::cerr << "SyncManager: Unknown thread join error" << std::endl;
            // Detach to prevent terminate() call in destructor
            try { m_syncThread.detach(); } catch (...) {}
        }
    }

    // Ensure thread is fully cleaned up
    m_syncThread = std::thread();

    std::cout << "SyncManager: Stopped" << std::endl;
}

void SyncManager::ForceSyncJob(const std::wstring& jobPath)
{
    std::lock_guard<std::mutex> lock(m_syncMutex);
    SyncJob(jobPath);
}

void SyncManager::ForceSyncAll()
{
    std::lock_guard<std::mutex> lock(m_syncMutex);

    RefreshActiveJobs();

    for (const auto& jobPath : m_activeJobPaths)
    {
        SyncJob(jobPath);
    }
}

void SyncManager::SyncLoop()
{
    try
    {
        while (m_isRunning)
        {
            SyncTick();

            // Wait for tick interval (or until shutdown is requested)
            std::unique_lock<std::mutex> lock(m_shutdownMutex);
            m_shutdownCV.wait_for(lock, m_tickInterval, [this]() { return !m_isRunning; });
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "SyncManager: Sync loop exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "SyncManager: Sync loop unknown exception" << std::endl;
    }
}

void SyncManager::SyncTick()
{
    std::lock_guard<std::mutex> lock(m_syncMutex);

    // Refresh active jobs list
    RefreshActiveJobs();

    if (m_activeJobPaths.empty())
    {
        return;
    }

    // Sync 1-2 jobs per tick (staggered)
    int jobsToSync = std::min(2, static_cast<int>(m_activeJobPaths.size()));

    for (int i = 0; i < jobsToSync; ++i)
    {
        std::wstring jobPath = m_activeJobPaths[m_currentIndex];

        if (ShouldSync(jobPath))
        {
            SyncJob(jobPath);
        }

        m_currentIndex = (m_currentIndex + 1) % m_activeJobPaths.size();
    }
}

void SyncManager::SyncJob(const std::wstring& jobPath)
{
    std::cout << "Syncing job: " << WideToUtf8(jobPath) << std::endl;

    // Check if this is first sync for this job
    bool isFirstSync = (m_firstSyncDone.find(jobPath) == m_firstSyncDone.end());

    if (isFirstSync)
    {
        std::cout << "  First sync for this job - creating backup" << std::endl;
        CreateBackupIfNeeded(jobPath);
        m_firstSyncDone[jobPath] = true;
    }

    // Update subscription status
    m_subManager->UpdateSyncStatus(jobPath, SyncStatus::Syncing, GetCurrentTimeMs());

    // Read shared JSON
    std::map<std::wstring, Shot> sharedShots;
    ValidationResult validation = m_backupManager->ValidateJSON(
        (std::filesystem::path(jobPath) / L".ufb" / L"shots.json").wstring()
    );

    if (validation != ValidationResult::Valid)
    {
        if (validation == ValidationResult::Missing)
        {
            // No shared JSON yet - this is OK for new jobs
            std::cout << "  No shared JSON found (new job)" << std::endl;
        }
        else
        {
            // Corruption detected - update status and return
            std::cerr << "  ERROR: Shared JSON is corrupted/invalid" << std::endl;
            m_subManager->UpdateSyncStatus(jobPath, SyncStatus::Error, GetCurrentTimeMs());
            return;
        }
    }
    else
    {
        // Read shared JSON
        if (!m_metaManager->ReadSharedJSON(jobPath, sharedShots))
        {
            std::cerr << "  ERROR: Failed to read shared JSON" << std::endl;
            m_subManager->UpdateSyncStatus(jobPath, SyncStatus::Error, GetCurrentTimeMs());
            return;
        }
    }

    // Read cached shots
    auto cachedShots = m_metaManager->GetCachedShots(jobPath);
    std::map<std::wstring, Shot> cachedMap;
    for (const auto& shot : cachedShots)
    {
        cachedMap[shot.shotPath] = shot;
    }

    // Compute diff
    SyncDiff diff = ComputeDiff(cachedMap, sharedShots);

    std::cout << "  Remote changes: " << diff.remoteChanges.size() << std::endl;
    std::cout << "  Local changes: " << diff.localChanges.size() << std::endl;

    // Apply remote changes to cache
    if (!diff.remoteChanges.empty())
    {
        ApplyRemoteChanges(jobPath, diff.remoteChanges);
    }

    // Write local changes to shared JSON
    if (!diff.localChanges.empty())
    {
        WriteLocalChangesToSharedJSON(jobPath);
    }

    // Update subscription status and shot count
    auto updatedShots = m_metaManager->GetCachedShots(jobPath);
    m_subManager->UpdateShotCount(jobPath, static_cast<int>(updatedShots.size()));
    m_subManager->UpdateSyncStatus(jobPath, SyncStatus::Synced, GetCurrentTimeMs());

    // Update sync time
    UpdateSyncTime(jobPath, GetCurrentTimeMs());

    std::cout << "  Sync complete" << std::endl;
}

SyncDiff SyncManager::ComputeDiff(const std::map<std::wstring, Shot>& cached,
                                   const std::map<std::wstring, Shot>& shared)
{
    SyncDiff diff;

    // Find remote changes (new or updated in shared)
    for (const auto& [path, sharedShot] : shared)
    {
        auto it = cached.find(path);

        if (it == cached.end())
        {
            // New shot from remote
            diff.remoteChanges.push_back(sharedShot);
        }
        else if (ShouldAcceptRemoteChange(it->second, sharedShot))
        {
            // Remote is newer
            diff.remoteChanges.push_back(sharedShot);
        }
        else if (it->second.modifiedTime > sharedShot.modifiedTime)
        {
            // Local is newer
            diff.localChanges.push_back(it->second);
        }
        // If equal modified_time, no change needed
    }

    // Find local-only shots (need to be written to shared)
    for (const auto& [path, cachedShot] : cached)
    {
        if (shared.find(path) == shared.end())
        {
            diff.localChanges.push_back(cachedShot);
        }
    }

    return diff;
}

void SyncManager::ApplyRemoteChanges(const std::wstring& jobPath, const std::vector<Shot>& changes)
{
    for (const auto& shot : changes)
    {
        // Update cache with remote shot
        // This will be handled by UpdateCache which does bulk update
    }

    // Bulk update cache
    // Read all current cached shots
    auto cachedShots = m_metaManager->GetCachedShots(jobPath);
    std::map<std::wstring, Shot> updatedCache;

    // Add existing shots
    for (const auto& shot : cachedShots)
    {
        updatedCache[shot.shotPath] = shot;
    }

    // Apply remote changes
    for (const auto& remoteShot : changes)
    {
        updatedCache[remoteShot.shotPath] = remoteShot;
    }

    // Convert back to vector
    std::vector<Shot> updatedShots;
    for (const auto& [path, shot] : updatedCache)
    {
        updatedShots.push_back(shot);
    }

    // Update cache
    m_metaManager->UpdateCache(jobPath, updatedShots);
}

void SyncManager::WriteLocalChangesToSharedJSON(const std::wstring& jobPath)
{
    // Read current shared JSON
    std::map<std::wstring, Shot> sharedShots;
    m_metaManager->ReadSharedJSON(jobPath, sharedShots);

    // Read all cached shots
    auto cachedShots = m_metaManager->GetCachedShots(jobPath);

    // Merge: local shots override shared if newer
    for (const auto& cachedShot : cachedShots)
    {
        auto it = sharedShots.find(cachedShot.shotPath);

        if (it == sharedShots.end())
        {
            // New shot
            sharedShots[cachedShot.shotPath] = cachedShot;
        }
        else if (cachedShot.modifiedTime > it->second.modifiedTime)
        {
            // Local is newer
            sharedShots[cachedShot.shotPath] = cachedShot;
        }
        // Else: remote is newer, don't overwrite
    }

    // Write merged shots to shared JSON
    m_metaManager->WriteSharedJSON(jobPath, sharedShots);
}

bool SyncManager::ShouldAcceptRemoteChange(const Shot& local, const Shot& remote)
{
    // Simple last-write-wins
    if (remote.modifiedTime > local.modifiedTime)
        return true;

    // Tie-breaker: lexicographic device ID comparison
    if (remote.modifiedTime == local.modifiedTime)
        return remote.deviceId > local.deviceId;

    return false;
}

void SyncManager::CreateBackupIfNeeded(const std::wstring& jobPath)
{
    // Check if backup needed today
    if (!m_backupManager->ShouldBackupToday(jobPath))
    {
        std::cout << "    Backup already done today" << std::endl;
        return;
    }

    // Try to acquire backup lock
    if (!m_backupManager->TryAcquireBackupLock(jobPath, 60))
    {
        std::cout << "    Could not acquire backup lock (another user is backing up)" << std::endl;
        return;
    }

    // Create backup
    if (m_backupManager->CreateBackup(jobPath))
    {
        std::cout << "    Backup created successfully" << std::endl;

        // Evict old backups
        m_backupManager->EvictOldBackups(jobPath, 30);
    }

    // Release lock
    m_backupManager->ReleaseBackupLock(jobPath);
}

bool SyncManager::ShouldSync(const std::wstring& jobPath)
{
    // Prioritize:
    // 1. Jobs with local pending changes
    // 2. Jobs not synced in > 30s

    if (HasLocalChanges(jobPath))
    {
        return true;
    }

    uint64_t lastSync = GetLastSyncTime(jobPath);
    uint64_t now = GetCurrentTimeMs();

    return (now - lastSync) > 30000; // 30 seconds
}

bool SyncManager::HasLocalChanges(const std::wstring& jobPath)
{
    // Check if there are any shots in cache that are newer than last sync
    auto cachedShots = m_metaManager->GetCachedShots(jobPath);
    uint64_t lastSync = GetLastSyncTime(jobPath);

    for (const auto& shot : cachedShots)
    {
        if (shot.modifiedTime > lastSync)
        {
            return true;
        }
    }

    return false;
}

uint64_t SyncManager::GetLastSyncTime(const std::wstring& jobPath)
{
    auto it = m_lastSyncTimes.find(jobPath);
    if (it != m_lastSyncTimes.end())
    {
        return it->second;
    }

    // Get from subscription manager
    auto sub = m_subManager->GetSubscription(jobPath);
    if (sub.has_value())
    {
        return sub->lastSyncTime;
    }

    return 0;
}

void SyncManager::UpdateSyncTime(const std::wstring& jobPath, uint64_t timestamp)
{
    m_lastSyncTimes[jobPath] = timestamp;
}

void SyncManager::RefreshActiveJobs()
{
    auto activeSubscriptions = m_subManager->GetActiveSubscriptions();

    m_activeJobPaths.clear();
    for (const auto& sub : activeSubscriptions)
    {
        m_activeJobPaths.push_back(sub.jobPath);
    }

    // Reset index if it's out of bounds
    if (m_currentIndex >= m_activeJobPaths.size() && !m_activeJobPaths.empty())
    {
        m_currentIndex = 0;
    }
}

} // namespace UFB
