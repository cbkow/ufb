#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <chrono>
#include "imgui.h"
#include "thumbnail_extractor.h"

// Thumbnail cache entry (only stores COMPLETED thumbnails)
struct ThumbnailEntry
{
    ImTextureID texID = 0;           // ImGui texture ID
    unsigned int glTexture = 0;       // OpenGL texture handle
    int width = 0;
    int height = 0;
    int extractedSize = 0;           // Size this thumbnail was extracted at
    std::chrono::steady_clock::time_point lastAccessTime;  // For LRU eviction
};

// Request for thumbnail extraction (queued by main thread)
struct ThumbnailRequest
{
    std::wstring path;
    int size;
    bool highPriority = false;  // For visible items
};

// Result from thumbnail extraction (queued by worker thread)
struct ThumbnailResult
{
    std::wstring path;
    HBITMAP hBitmap;
    int width;
    int height;
    int requestedSize;  // The size that was originally requested
    bool success;
};

// Manages thumbnail extraction with background thread pool
class ThumbnailManager
{
public:
    ThumbnailManager();
    ~ThumbnailManager();

    // Initialize with number of worker threads
    // @param numThreads - Number of worker threads (default: 4)
    void Initialize(int numThreads = 4);

    // Shutdown and cleanup all resources
    void Shutdown();

    // Register a thumbnail extractor (sorted by priority)
    void RegisterExtractor(std::unique_ptr<ThumbnailExtractorInterface> extractor);

    // Request thumbnail extraction (non-blocking, queued for worker threads)
    // @param path - Full path to file
    // @param size - Desired thumbnail size (512 recommended for 4K displays)
    // @param highPriority - If true, process before normal requests
    // @return true if request was queued, false if dropped (queue full or already cached/loading)
    bool RequestThumbnail(const std::wstring& path, int size, bool highPriority = false);

    // Process completed thumbnails (call from main thread each frame)
    // Converts HBITMAP to OpenGL texture and updates cache
    void ProcessCompletedThumbnails();

    // Get cached thumbnail (returns nullptr if not yet loaded)
    ImTextureID GetThumbnail(const std::wstring& path);

    // Get cached thumbnail with dimensions (returns nullptr if not yet loaded)
    ImTextureID GetThumbnail(const std::wstring& path, int& outWidth, int& outHeight);

    // Check if thumbnail is currently loading
    bool IsLoading(const std::wstring& path);

    // Clear all cached thumbnails
    void ClearCache();

    // Clear all pending thumbnail requests and in-flight tracking
    void ClearPendingRequests();

    // Get cache statistics
    int GetCacheSize() const { return static_cast<int>(m_cache.size()); }
    int GetPendingRequests() const;

private:
    // Thread pool
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_running{false};

    // Request queue (accessed by main thread and workers)
    std::queue<ThumbnailRequest> m_requestQueue;
    mutable std::mutex m_requestMutex;  // Mutable to allow locking in const methods
    std::condition_variable m_requestCV;

    // Completion queue (accessed by workers and main thread)
    std::queue<ThumbnailResult> m_completedQueue;
    std::mutex m_completedMutex;

    // Thumbnail cache (per-file, not per-extension) - ONLY completed thumbnails
    std::map<std::wstring, ThumbnailEntry> m_cache;
    std::mutex m_cacheMutex;

    // In-flight requests tracking (paths currently being extracted)
    std::set<std::wstring> m_inFlightPaths;
    std::mutex m_inFlightMutex;

    // Registered extractors (sorted by priority, highest first)
    std::vector<std::unique_ptr<ThumbnailExtractorInterface>> m_extractors;

    // LRU cache management
    int m_maxCacheSize = 100;  // Max 100 thumbnails (prevent memory exhaustion)

    // Worker thread function
    void WorkerThread();

    // Extract thumbnail using registered extractors
    HBITMAP ExtractThumbnail(const std::wstring& path, int size, int& outWidth, int& outHeight);

    // Evict least recently used thumbnails if cache is full
    void EvictLRU();

    // Convert HBITMAP to OpenGL texture (main thread only)
    bool CreateTextureFromResult(const ThumbnailResult& result, ThumbnailEntry& entry);
};
