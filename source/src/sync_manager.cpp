// Ensure WinSock2 is included before windows.h (needed by p2p_manager.h)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <combaseapi.h>  // For CoCreateGuid

#include "sync_manager.h"
#include "utils.h"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <filesystem>

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

    // Initialize file watcher for real-time sync
    m_fileWatcher = std::make_unique<FileWatcher>();
    std::cout << "SyncManager: FileWatcher initialized" << std::endl;

    // Initialize archival manager for change log compression
    m_archivalManager = std::make_unique<ArchivalManager>();
    std::cout << "SyncManager: ArchivalManager initialized" << std::endl;

    // Load or create device ID
    m_deviceId = GetOrCreateDeviceId();
    std::wcout << L"SyncManager: Device ID: " << m_deviceId << std::endl;

    // Initialize single global P2P manager
    m_p2pManager = std::make_unique<P2PManager>();
    if (!m_p2pManager->Initialize(m_deviceId))
    {
        std::cerr << "[SyncManager] Failed to initialize P2P manager" << std::endl;
        m_p2pManager.reset();
        return false;
    }

    // Start listening on a dynamic port
    if (!m_p2pManager->StartListening(49152))
    {
        std::cerr << "[SyncManager] Failed to start P2P listening" << std::endl;
        m_p2pManager.reset();
        return false;
    }

    std::cout << "[SyncManager] P2P listening on port " << m_p2pManager->GetListeningPort() << std::endl;

    // Register callback for remote P2P change notifications
    m_p2pManager->RegisterChangeCallback([this](const std::wstring& changedJobPath, const std::wstring& peerDeviceId, uint64_t timestamp) {
        OnP2PChangeReceived(changedJobPath, peerDeviceId, timestamp);
    });

    // Register callback for immediate P2P notifications when local changes are made
    m_subManager->RegisterLocalChangeCallback([this](const std::wstring& jobPath, uint64_t timestamp) {
        // Notify P2P peers immediately when a local change is written to the change log
        if (m_p2pManager)
        {
            m_p2pManager->NotifyPeersOfChange(jobPath, timestamp);
            std::wcout << L"[SyncManager] Immediately notified P2P peers of local change to: " << jobPath
                       << L" (timestamp: " << timestamp << L")" << std::endl;
        }
    });

    return true;
}

void SyncManager::Shutdown()
{
    // Stop P2P manager
    if (m_p2pManager)
    {
        m_p2pManager->Shutdown();
        m_p2pManager.reset();
    }

    // Stop file watching
    if (m_fileWatcher)
    {
        m_fileWatcher->StopWatching();
    }

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

    // Start sync worker thread (processes queue)
    m_syncWorkerThread = std::thread(&SyncManager::SyncWorkerThread, this);

    // Start sync thread (fallback polling)
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

    // Wake up the worker thread immediately
    m_queueCV.notify_one();

    // Join sync thread
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

    // Join worker thread
    if (m_syncWorkerThread.joinable())
    {
        try
        {
            m_syncWorkerThread.join();
        }
        catch (const std::system_error& e)
        {
            std::cerr << "SyncManager: Worker thread join error: " << e.what() << std::endl;
            try { m_syncWorkerThread.detach(); } catch (...) {}
        }
        catch (...)
        {
            std::cerr << "SyncManager: Unknown worker thread join error" << std::endl;
            try { m_syncWorkerThread.detach(); } catch (...) {}
        }
    }

    // Ensure threads are fully cleaned up
    m_syncThread = std::thread();
    m_syncWorkerThread = std::thread();

    std::cout << "SyncManager: Stopped" << std::endl;
}

void SyncManager::ForceSyncJob(const std::wstring& jobPath)
{
    // Post to queue instead of blocking
    PostSyncJob(jobPath);
}

void SyncManager::ForceSyncAll()
{
    RefreshActiveJobs();

    for (const auto& jobPath : m_activeJobPaths)
    {
        // Post to queue instead of blocking
        PostSyncJob(jobPath);
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
    // Refresh active jobs list
    RefreshActiveJobs();

    if (m_activeJobPaths.empty())
    {
        return;
    }

    // Sync 1-2 jobs per tick (staggered)
    int jobsToSync = (std::min)(2, static_cast<int>(m_activeJobPaths.size()));

    for (int i = 0; i < jobsToSync; ++i)
    {
        std::wstring jobPath = m_activeJobPaths[m_currentIndex];

        if (ShouldSync(jobPath))
        {
            // Post to queue instead of blocking
            PostSyncJob(jobPath);
        }

        m_currentIndex = (m_currentIndex + 1) % m_activeJobPaths.size();
    }
}

void SyncManager::SyncWorkerThread()
{
    std::wcout << L"[SyncManager] Sync worker thread started" << std::endl;

    try
    {
        while (m_isRunning)
        {
            std::wstring jobPath;

            // Wait for work
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);

                // Wait until there's work or we're shutting down
                m_queueCV.wait(lock, [this]() {
                    return !m_syncQueue.empty() || !m_isRunning;
                });

                // Check if we're shutting down
                if (!m_isRunning && m_syncQueue.empty())
                {
                    break;
                }

                // Get next job from queue
                if (!m_syncQueue.empty())
                {
                    jobPath = m_syncQueue.front();
                    m_syncQueue.pop();

                    // Remove from deduplication set
                    m_queuedJobs.erase(jobPath);
                }
            }

            // Process the job (outside of lock to avoid blocking queue)
            if (!jobPath.empty())
            {
                try
                {
                    std::wcout << L"[SyncManager] Processing sync job from queue: " << jobPath << std::endl;
                    SyncJob(jobPath);
                }
                catch (const std::exception& e)
                {
                    std::cerr << "[SyncManager] Exception in sync worker: " << e.what() << std::endl;
                }
                catch (...)
                {
                    std::cerr << "[SyncManager] Unknown exception in sync worker" << std::endl;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SyncManager] Sync worker thread exception: " << e.what() << std::endl;
    }

    std::wcout << L"[SyncManager] Sync worker thread stopped" << std::endl;
}

void SyncManager::PostSyncJob(const std::wstring& jobPath)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    // Check if already queued (deduplication)
    if (m_queuedJobs.find(jobPath) != m_queuedJobs.end())
    {
        // Already in queue, skip
        return;
    }

    // Add to queue and deduplication set
    m_syncQueue.push(jobPath);
    m_queuedJobs.insert(jobPath);

    // Wake up worker thread
    m_queueCV.notify_one();
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

    // NOTE: Filesystem scanning removed from sync - metadata is source of truth, not filesystem
    // Shot folders are created on-demand when user accesses them
    // For manual discovery, use DiscoverAndTrackShots() via explicit user action

    // Update subscription status
    m_subManager->UpdateSyncStatus(jobPath, SyncStatus::Syncing, GetCurrentTimeMs());

    // Check if we have an expected change for content verification
    std::wstring expectedDeviceId;
    uint64_t minTimestamp = 0;
    {
        std::lock_guard<std::mutex> lock(m_expectedChangesMutex);
        auto it = m_expectedChanges.find(jobPath);
        if (it != m_expectedChanges.end())
        {
            expectedDeviceId = it->second.deviceId;
            minTimestamp = it->second.timestamp;
            std::wcout << L"[SyncJob] Will verify expected change: deviceId=" << expectedDeviceId
                       << L" timestamp=" << minTimestamp << std::endl;
            // Clear after retrieval (one-time verification)
            m_expectedChanges.erase(it);
        }
    }

    // IMPORTANT: On first sync OR if database has no metadata, ignore expectedDeviceId filter
    // and read ALL change logs. This handles the case where the local database was reset but
    // this device's change log still exists on the network share and is the source of truth.
    auto cachedShots = m_metaManager->GetCachedShots(jobPath);
    if ((isFirstSync || cachedShots.empty()) && !expectedDeviceId.empty())
    {
        std::wcout << L"[SyncJob] First sync or database empty - ignoring expectedDeviceId filter to ensure our own change log is read" << std::endl;
        expectedDeviceId.clear();
        minTimestamp = 0;
    }

    // NEW ARCHITECTURE: Read all device change logs (active + archived) and merge
    std::cout << "  Reading change logs from all devices (including archives)..." << std::endl;
    std::map<std::wstring, Shot> sharedShots = m_archivalManager->ReadAllChangeLogs(jobPath, expectedDeviceId, minTimestamp);

    // Migration path: Also check for legacy shots.json
    if (sharedShots.empty())
    {
        std::filesystem::path legacyJsonPath = std::filesystem::path(jobPath) / L".ufb" / L"shots.json";
        if (std::filesystem::exists(legacyJsonPath))
        {
            std::cout << "  Found legacy shots.json, migrating..." << std::endl;
            if (!m_metaManager->ReadSharedJSON(jobPath, sharedShots))
            {
                std::cerr << "  ERROR: Failed to read legacy shots.json" << std::endl;
            }
        }
        else
        {
            std::cout << "  No change logs found (new job)" << std::endl;
        }
    }

    // Read cached shots (already read earlier for empty check, reuse it)
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

    // NEW ARCHITECTURE: Local changes already written to change logs via BridgeToSyncCache
    // No need to write to shared JSON anymore - change logs are the source of truth
    // P2P notifications are sent immediately when changes are made (via callback), not here
    if (!diff.localChanges.empty())
    {
        std::cout << "  Local changes already in change logs (no write needed)" << std::endl;
    }

    // Update subscription status and shot count
    auto updatedShots = m_metaManager->GetCachedShots(jobPath);
    m_subManager->UpdateShotCount(jobPath, static_cast<int>(updatedShots.size()));
    m_subManager->UpdateSyncStatus(jobPath, SyncStatus::Synced, GetCurrentTimeMs());

    // Update sync time
    UpdateSyncTime(jobPath, GetCurrentTimeMs());

    // Run periodic archival (if needed)
    if (ShouldRunArchival(jobPath))
    {
        RunArchival(jobPath);
    }

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
    std::wcout << L"[SyncManager] ApplyRemoteChanges: Applying " << changes.size() << L" remote changes to: " << jobPath << std::endl;

    for (const auto& shot : changes)
    {
        // Update cache with remote shot
        // This will be handled by UpdateCache which does bulk update
        std::wcout << L"  - Remote change for: " << shot.shotPath << L" (modified: " << shot.modifiedTime << L")" << std::endl;
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

    // Update cache (but don't notify observers yet - wait until shot_metadata is also updated)
    std::wcout << L"[SyncManager] Calling UpdateCache with " << updatedShots.size() << L" total shots (deferred notify)" << std::endl;
    m_metaManager->UpdateCache(jobPath, updatedShots, false);

    // Bridge ONLY the remote changes from sync cache to shot_metadata table (so UI can see them!)
    std::wcout << L"[SyncManager] Bridging " << changes.size() << L" remote changes from sync cache to shot_metadata" << std::endl;
    for (const auto& remoteShot : changes)
    {
        m_subManager->BridgeFromSyncCache(remoteShot, jobPath);
    }

    // NOW notify observers - both shot_cache and shot_metadata are updated
    std::wcout << L"[SyncManager] Notifying observers after metadata tables are fully updated" << std::endl;
    m_metaManager->NotifyObservers(jobPath);

    std::wcout << L"[SyncManager] ApplyRemoteChanges completed" << std::endl;
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

    // Notify P2P peers of changes (legacy path - use current timestamp)
    if (m_p2pManager)
    {
        m_p2pManager->NotifyPeersOfChange(jobPath, GetCurrentTimeMs());
        std::wcout << L"[SyncManager] Notified P2P peers of changes to: " << jobPath << std::endl;
    }
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

        // Setup file watcher for this job (if not already watching)
        SetupFileWatcher(sub.jobPath);

        // Setup P2P networking for this job (if not already setup)
        SetupP2PForJob(sub.jobPath);
    }

    // Reset index if it's out of bounds
    if (m_currentIndex >= m_activeJobPaths.size() && !m_activeJobPaths.empty())
    {
        m_currentIndex = 0;
    }
}

void SyncManager::DiscoverAndTrackShots(const std::wstring& jobPath)
{
    // Load project config for this job
    std::filesystem::path configPath = std::filesystem::path(jobPath) / L".ufb" / L"projectConfig.json";

    ProjectConfig config;
    if (!config.LoadFromFile(configPath.wstring()))
    {
        // No config found, skip shot discovery
        return;
    }

    // Get all folder types that should be tracked
    auto folderTypes = config.GetAllFolderTypes();

    for (const auto& typeName : folderTypes)
    {
        // Get config for this type
        auto typeConfigOpt = config.GetFolderTypeConfig(typeName);
        if (!typeConfigOpt.has_value())
            continue;

        const auto& typeConfig = typeConfigOpt.value();

        // Only track shot folder types
        if (!typeConfig.isShot)
            continue;

        // Check if category folder exists in job root
        std::wstring typeNameWide = Utf8ToWide(typeName);
        std::filesystem::path categoryPath = std::filesystem::path(jobPath) / typeNameWide;

        if (std::filesystem::exists(categoryPath) && std::filesystem::is_directory(categoryPath))
        {
            DiscoverShotsInCategory(categoryPath.wstring(), jobPath, typeName, config);
        }
    }
}

void SyncManager::DiscoverShotsInCategory(const std::wstring& categoryPath, const std::wstring& jobPath, const std::string& folderType, const ProjectConfig& config)
{
    try
    {
        // Iterate through all subdirectories in the category folder
        for (const auto& entry : std::filesystem::directory_iterator(categoryPath))
        {
            if (!entry.is_directory())
                continue;

            std::wstring shotPath = entry.path().wstring();

            // Check if metadata already exists for this shot
            auto existingMetadata = m_subManager->GetShotMetadata(shotPath);

            if (!existingMetadata.has_value())
            {
                // Create new metadata entry with defaults from template
                ShotMetadata metadata;
                metadata.shotPath = shotPath;
                metadata.folderType = folderType;
                metadata.priority = 2; // Default to medium priority
                metadata.isTracked = false; // Default to NOT tracked - user must explicitly add to tracker
                metadata.createdTime = GetCurrentTimeMs();
                metadata.modifiedTime = GetCurrentTimeMs();

                // Apply default status and category from template
                auto folderConfigOpt = config.GetFolderTypeConfig(folderType);
                if (folderConfigOpt.has_value())
                {
                    const auto& folderConfig = folderConfigOpt.value();

                    if (!folderConfig.statusOptions.empty())
                    {
                        metadata.status = folderConfig.statusOptions[0].name;
                    }

                    if (!folderConfig.categoryOptions.empty())
                    {
                        metadata.category = folderConfig.categoryOptions[0].name;
                    }
                }

                // Create the metadata entry in local SQLite
                m_subManager->CreateOrUpdateShotMetadata(metadata);

                // Bridge to change logs immediately (for sync and P2P)
                m_subManager->BridgeToSyncCache(metadata, jobPath);

                std::wcout << L"[SyncManager] Discovered new shot: " << shotPath << L" (" << Utf8ToWide(folderType) << L") - added to change logs" << std::endl;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SyncManager] Error discovering shots in " << WideToUtf8(categoryPath) << ": " << e.what() << std::endl;
    }
}

//=============================================================================
// File Watching Integration
//=============================================================================

void SyncManager::SetupFileWatcher(const std::wstring& jobPath)
{
    if (!m_fileWatcher)
        return;

    // Check if already watching this job
    if (m_watchedJobs[jobPath])
        return;

    // NEW ARCHITECTURE: Watch changes/ directory for any device's change logs
    std::filesystem::path changesDir = std::filesystem::path(jobPath) / L".ufb" / L"changes";

    // Create changes directory if it doesn't exist yet
    if (!std::filesystem::exists(changesDir))
    {
        try
        {
            std::filesystem::create_directories(changesDir);
            std::wcout << L"[SyncManager] Created changes directory: " << changesDir << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[SyncManager] Failed to create changes directory: " << e.what() << std::endl;
            return;
        }
    }

    // Watch for any .json file in changes/ directory
    // Note: FileWatcher monitors a directory and filters by filename
    // For now, we'll watch our own device's change log file
    std::wstring deviceFilename = L"device-" + Utf8ToWide(GetDeviceID()) + L".json";
    std::filesystem::path markerPath = changesDir / deviceFilename;

    // Create marker file if it doesn't exist (empty array)
    if (!std::filesystem::exists(markerPath))
    {
        std::ofstream file(markerPath);
        if (file.is_open())
        {
            file << "[]";  // Empty JSON array
            file.close();
        }
    }

    // Setup file watcher with lambda callback
    bool success = m_fileWatcher->WatchFile(markerPath.wstring(), [this, jobPath]() {
        OnFileChanged(jobPath);
    });

    if (success)
    {
        m_watchedJobs[jobPath] = true;
        std::wcout << L"[SyncManager] File watcher setup for: " << jobPath << std::endl;
    }
    else
    {
        std::wcerr << L"[SyncManager] Failed to setup file watcher for: " << jobPath << std::endl;
    }
}

void SyncManager::OnFileChanged(const std::wstring& jobPath)
{
    std::wcout << L"[SyncManager] File change detected for: " << jobPath << L" - triggering immediate sync" << std::endl;

    // Trigger immediate sync (bypass 30s delay)
    ForceSyncJob(jobPath);
}

// ============================================================================
// P2P Networking Methods
// ============================================================================

void SyncManager::SetupP2PForJob(const std::wstring& jobPath)
{
    if (!m_p2pManager)
    {
        std::wcerr << L"[SyncManager] P2P manager not initialized" << std::endl;
        return;
    }

    // Check if already subscribed (avoid duplicate subscriptions)
    auto subscribedProjects = m_p2pManager->GetSubscribedProjects();
    if (std::find(subscribedProjects.begin(), subscribedProjects.end(), jobPath) != subscribedProjects.end())
    {
        // Already subscribed
        return;
    }

    std::wcout << L"[SyncManager] Setting up P2P for: " << jobPath << std::endl;

    // Subscribe this project to the global P2P manager
    m_p2pManager->SubscribeToProject(jobPath);

    // Immediately write our peer info to peers.json so other devices can discover us
    // Don't wait for the first heartbeat (30 seconds)
    m_p2pManager->WritePeerRegistry();

    // Immediately try to connect to any existing peers
    // Don't wait for the first heartbeat (30 seconds)
    m_p2pManager->UpdatePeerRegistry();

    std::wcout << L"[SyncManager] P2P setup complete for: " << jobPath << std::endl;
}

void SyncManager::OnP2PChangeReceived(const std::wstring& jobPath, const std::wstring& peerDeviceId, uint64_t timestamp)
{
    try
    {
        std::wcout << L"[SyncManager] P2P change notification received for: " << jobPath
                   << L" from device: " << peerDeviceId
                   << L" timestamp: " << timestamp << std::endl;

        // Validate input
        if (jobPath.empty())
        {
            std::cerr << "[SyncManager] ERROR: Empty jobPath in P2P notification" << std::endl;
            return;
        }

        // Check if sync is running
        if (!m_isRunning)
        {
            std::cerr << "[SyncManager] WARNING: P2P notification received but sync not running" << std::endl;
            return;
        }

        // Store expected change for content verification
        {
            std::lock_guard<std::mutex> lock(m_expectedChangesMutex);
            m_expectedChanges[jobPath] = {peerDeviceId, timestamp};
            std::wcout << L"[SyncManager] Stored expected change for verification: deviceId=" << peerDeviceId
                       << L" timestamp=" << timestamp << std::endl;
        }

        // Trigger immediate sync (bypass 30s delay)
        ForceSyncJob(jobPath);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[SyncManager] Exception in OnP2PChangeReceived: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[SyncManager] Unknown exception in OnP2PChangeReceived" << std::endl;
    }
}

std::wstring SyncManager::GetOrCreateDeviceId()
{
    // Device ID is stored in %LOCALAPPDATA%/ufb/device_id.txt
    // Note: GetLocalAppDataPath() already returns the ufb folder
    std::filesystem::path ufbDir = GetLocalAppDataPath();
    std::filesystem::path deviceIdFile = ufbDir / L"device_id.txt";

    // Check if device ID file exists
    if (std::filesystem::exists(deviceIdFile))
    {
        std::wifstream file(deviceIdFile);
        std::wstring deviceId;
        if (std::getline(file, deviceId) && !deviceId.empty())
        {
            return deviceId;
        }
    }

    // Generate new device ID (GUID)
    GUID guid;
    CoCreateGuid(&guid);

    wchar_t guidStr[40];
    swprintf_s(guidStr, L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               guid.Data1, guid.Data2, guid.Data3,
               guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
               guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);

    std::wstring deviceId = guidStr;

    // Ensure directory exists
    if (!std::filesystem::exists(ufbDir))
    {
        std::filesystem::create_directories(ufbDir);
    }

    // Write device ID to file
    std::wofstream file(deviceIdFile);
    file << deviceId;

    std::wcout << L"[SyncManager] Created new device ID: " << deviceId << std::endl;

    return deviceId;
}

// ========================================
// Archival Helpers
// ========================================

bool SyncManager::ShouldRunArchival(const std::wstring& jobPath)
{
    // Run archival once per day per job
    constexpr uint64_t ARCHIVAL_INTERVAL_MS = 24 * 60 * 60 * 1000; // 24 hours

    auto it = m_lastArchivalTimes.find(jobPath);
    if (it == m_lastArchivalTimes.end())
    {
        // First time - run archival
        return true;
    }

    uint64_t now = GetCurrentTimeMs();
    uint64_t timeSinceLastArchival = now - it->second;

    return (timeSinceLastArchival >= ARCHIVAL_INTERVAL_MS);
}

void SyncManager::RunArchival(const std::wstring& jobPath)
{
    std::wcout << L"[SyncManager] Running archival for job: " << jobPath << std::endl;

    // Convert device ID to UTF-8 for ArchivalManager
    std::string deviceIdUtf8 = WideToUtf8(m_deviceId);

    // Run archival with 90-day threshold
    bool success = m_archivalManager->ArchiveOldEntries(jobPath, deviceIdUtf8, 90);

    if (success)
    {
        // Update last archival time
        m_lastArchivalTimes[jobPath] = GetCurrentTimeMs();
        std::wcout << L"[SyncManager] Archival completed successfully for job: " << jobPath << std::endl;

        // Create/update bootstrap snapshot if needed (for fast cold starts)
        if (!m_archivalManager->HasRecentBootstrapSnapshot(jobPath, 24))
        {
            std::wcout << L"[SyncManager] Creating bootstrap snapshot..." << std::endl;
            if (m_archivalManager->CreateBootstrapSnapshot(jobPath))
            {
                std::wcout << L"[SyncManager] Bootstrap snapshot created successfully" << std::endl;
            }
            else
            {
                std::wcerr << L"[SyncManager] Failed to create bootstrap snapshot" << std::endl;
            }
        }
    }
    else
    {
        std::wcerr << L"[SyncManager] Archival failed for job: " << jobPath << std::endl;
    }
}

} // namespace UFB
