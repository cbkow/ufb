#include "thumbnail_manager.h"
#include "texture_utils.h"
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <cmath>

ThumbnailManager::ThumbnailManager()
{
}

ThumbnailManager::~ThumbnailManager()
{
    Shutdown();
}

void ThumbnailManager::Initialize(int numThreads)
{
    if (m_running)
    {
        std::cerr << "ThumbnailManager: Already initialized" << std::endl;
        return;
    }

    m_running = true;

    // Create worker threads
    for (int i = 0; i < numThreads; ++i)
    {
        m_workers.emplace_back(&ThumbnailManager::WorkerThread, this);
    }

    std::cout << "ThumbnailManager: Initialized with " << numThreads << " worker threads" << std::endl;
}

void ThumbnailManager::Shutdown()
{
    if (!m_running)
        return;

    std::cout << "ThumbnailManager: Shutting down..." << std::endl;

    // Signal threads to stop
    m_running = false;
    m_requestCV.notify_all();

    // Wait for all threads to finish with exception handling
    for (auto& thread : m_workers)
    {
        if (thread.joinable())
        {
            try
            {
                thread.join();
            }
            catch (const std::system_error& e)
            {
                std::cerr << "ThumbnailManager: Thread join error: " << e.what() << std::endl;
                // Detach to prevent terminate() call in destructor
                try { thread.detach(); } catch (...) {}
            }
            catch (...)
            {
                std::cerr << "ThumbnailManager: Unknown thread join error" << std::endl;
                // Detach to prevent terminate() call in destructor
                try { thread.detach(); } catch (...) {}
            }
        }
    }

    // Clear vector and shrink to free memory
    m_workers.clear();
    m_workers.shrink_to_fit();

    // Clear cache and free OpenGL textures
    ClearCache();

    std::cout << "ThumbnailManager: Shutdown complete" << std::endl;
}

void ThumbnailManager::RegisterExtractor(std::unique_ptr<ThumbnailExtractorInterface> extractor)
{
    m_extractors.push_back(std::move(extractor));

    // Sort by priority (highest first)
    std::sort(m_extractors.begin(), m_extractors.end(),
              [](const auto& a, const auto& b) {
                  return a->GetPriority() > b->GetPriority();
              });

    std::cout << "ThumbnailManager: Registered " << m_extractors.back()->GetName()
              << " (priority: " << m_extractors.back()->GetPriority() << ")" << std::endl;
}

bool ThumbnailManager::RequestThumbnail(const std::wstring& path, int size, bool highPriority)
{
    // Check if already in cache (completed thumbnail)
    unsigned int textureToDelete = 0;  // Texture to delete outside lock
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_cache.find(path);
        if (it != m_cache.end())
        {
            // Check if cached size is close enough to requested size
            // If difference > 25%, evict and re-extract at new size
            int cachedSize = it->second.extractedSize;
            if (cachedSize > 0)
            {
                float sizeDiff = std::abs(size - cachedSize) / (float)cachedSize;
                if (sizeDiff > 0.25f)
                {
                    // Size changed significantly - evict old thumbnail and re-extract
                    // Save texture ID, erase from cache, then delete OUTSIDE lock
                    // This prevents race where another thread has texture ID but we delete it
                    textureToDelete = it->second.glTexture;
                    m_cache.erase(it);
                    // Fall through to request new thumbnail
                }
                else
                {
                    // Size close enough, use cached version
                    it->second.lastAccessTime = std::chrono::steady_clock::now();  // Update LRU
                    return false;  // Not queued (already have it)
                }
            }
            else
            {
                // Cached thumbnail (update LRU)
                it->second.lastAccessTime = std::chrono::steady_clock::now();
                return false;  // Not queued (already have it)
            }
        }
    }

    // Delete texture AFTER releasing lock (if we evicted one)
    if (textureToDelete)
    {
        TextureUtils::DeleteTexture(textureToDelete);
    }

    // Check if already being extracted (in-flight)
    {
        std::lock_guard<std::mutex> lock(m_inFlightMutex);
        if (m_inFlightPaths.find(path) != m_inFlightPaths.end())
        {
            return false;  // Already being extracted, don't re-queue
        }

        // Mark as in-flight
        m_inFlightPaths.insert(path);
    }

    // Add to request queue
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        m_requestQueue.push({path, size, highPriority});
    }

    m_requestCV.notify_one();
    return true;  // Successfully queued
}

void ThumbnailManager::ProcessCompletedThumbnails()
{
    // Process all completed thumbnails this frame
    std::vector<ThumbnailResult> results;

    {
        std::lock_guard<std::mutex> lock(m_completedMutex);
        if (!m_completedQueue.empty())
        {
            std::cout << "[ThumbnailManager] Processing " << m_completedQueue.size() << " completed thumbnails" << std::endl;
        }
        while (!m_completedQueue.empty())
        {
            results.push_back(m_completedQueue.front());
            m_completedQueue.pop();
        }
    }

    // Convert HBITMAP to OpenGL texture (must be done on main thread)
    for (auto& result : results)
    {
        // Remove from in-flight tracking
        {
            std::lock_guard<std::mutex> lock(m_inFlightMutex);
            m_inFlightPaths.erase(result.path);
        }

        if (result.success && result.hBitmap)
        {
            // Create new cache entry for completed thumbnail
            ThumbnailEntry entry;

            // Use TextureUtils to create OpenGL texture
            unsigned int glTexture = 0;
            ImTextureID texID = TextureUtils::CreateTextureFromHBITMAP(result.hBitmap, &glTexture);

            if (texID)
            {
                entry.texID = texID;
                entry.glTexture = glTexture;
                entry.width = result.width;
                entry.height = result.height;
                entry.extractedSize = result.requestedSize; // Store requested size (not actual bitmap size)
                entry.lastAccessTime = std::chrono::steady_clock::now();

                // Add to cache
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                m_cache[result.path] = entry;
            }

            // Clean up HBITMAP
            DeleteObject(result.hBitmap);
        }
        else if (result.hBitmap)
        {
            // Extraction succeeded but no valid bitmap, clean up
            DeleteObject(result.hBitmap);
        }
        // If extraction failed (result.success == false), just remove from in-flight (already done above)
    }

    // Evict old thumbnails AFTER adding new ones if we're over capacity
    // This prevents constant eviction/creation churn
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_cache.size() > m_maxCacheSize)
        {
            std::cout << "[ThumbnailManager] Cache over capacity (" << m_cache.size()
                      << "/" << m_maxCacheSize << "), evicting LRU entries" << std::endl;
            EvictLRU();
        }
    }
}

ImTextureID ThumbnailManager::GetThumbnail(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    auto it = m_cache.find(path);
    if (it != m_cache.end())
    {
        // Update LRU timestamp
        it->second.lastAccessTime = std::chrono::steady_clock::now();
        return it->second.texID;
    }

    return 0;
}

ImTextureID ThumbnailManager::GetThumbnail(const std::wstring& path, int& outWidth, int& outHeight)
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    auto it = m_cache.find(path);
    if (it != m_cache.end())
    {
        // Update LRU timestamp
        it->second.lastAccessTime = std::chrono::steady_clock::now();

        outWidth = it->second.width;
        outHeight = it->second.height;
        return it->second.texID;
    }

    outWidth = 0;
    outHeight = 0;
    return 0;
}

bool ThumbnailManager::IsLoading(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(m_inFlightMutex);
    return m_inFlightPaths.find(path) != m_inFlightPaths.end();
}

void ThumbnailManager::ClearCache()
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    // Delete all OpenGL textures
    for (auto& [path, entry] : m_cache)
    {
        if (entry.glTexture)
        {
            TextureUtils::DeleteTexture(entry.glTexture);
        }
    }

    m_cache.clear();
    std::cout << "ThumbnailManager: Cache cleared" << std::endl;
}

void ThumbnailManager::ClearPendingRequests()
{
    // Clear pending requests from the request queue
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        std::queue<ThumbnailRequest> emptyQueue;
        std::swap(m_requestQueue, emptyQueue);
    }

    // Clear in-flight tracking
    {
        std::lock_guard<std::mutex> lock(m_inFlightMutex);
        m_inFlightPaths.clear();
    }

    // Clear completed queue
    {
        std::lock_guard<std::mutex> lock(m_completedMutex);
        while (!m_completedQueue.empty())
        {
            ThumbnailResult result = m_completedQueue.front();
            m_completedQueue.pop();
            if (result.success && result.hBitmap)
            {
                DeleteObject(result.hBitmap);
            }
        }
    }

    std::cout << "ThumbnailManager: Pending requests cleared" << std::endl;
}

int ThumbnailManager::GetPendingRequests() const
{
    std::lock_guard<std::mutex> lock(m_requestMutex);
    return static_cast<int>(m_requestQueue.size());
}

void ThumbnailManager::WorkerThread()
{
    try
    {
        // Initialize COM for this thread
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr))
        {
            std::cerr << "ThumbnailManager: Failed to initialize COM on worker thread" << std::endl;
            return;
        }

        // Set thread priority to below normal for smooth UI
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        while (m_running)
        {
            ThumbnailRequest request;

            // Wait for request
            {
                std::unique_lock<std::mutex> lock(m_requestMutex);
                m_requestCV.wait(lock, [this]() {
                    return !m_running || !m_requestQueue.empty();
                });

                if (!m_running)
                    break;

                if (m_requestQueue.empty())
                    continue;

                request = m_requestQueue.front();
                m_requestQueue.pop();
            }

            // Check if completed queue has capacity BEFORE extracting (to prevent memory exhaustion)
            {
                std::lock_guard<std::mutex> lock(m_completedMutex);
                if (m_completedQueue.size() >= 20)
                {
                    // Completed queue is full - skip extraction to prevent allocating huge HBITMAP
                    std::wcout << L"[ThumbnailManager] Completed queue full, skipping extraction: " << request.path << std::endl;

                    // Remove from in-flight so it can be requested again later
                    {
                        std::lock_guard<std::mutex> inFlightLock(m_inFlightMutex);
                        m_inFlightPaths.erase(request.path);
                    }
                    continue;  // Skip to next request
                }
            }

            // Extract thumbnail with exception handling
            int width = 0, height = 0;
            HBITMAP hBitmap = nullptr;

            try
            {
                hBitmap = ExtractThumbnail(request.path, request.size, width, height);
            }
            catch (const std::exception& e)
            {
                std::wcerr << L"ThumbnailManager: Failed to extract thumbnail for " << request.path
                          << L": " << e.what() << std::endl;
                hBitmap = nullptr;
            }
            catch (...)
            {
                std::wcerr << L"ThumbnailManager: Unknown error extracting thumbnail for " << request.path << std::endl;
                hBitmap = nullptr;
            }

            // Push result to completion queue (with backpressure to prevent memory exhaustion)
            {
                std::lock_guard<std::mutex> lock(m_completedMutex);

                // If completed queue is too large, drop this result to prevent memory exhaustion
                // Each HBITMAP can be 50-100MB of uncompressed data
                if (m_completedQueue.size() >= 20)
                {
                    std::wcout << L"[ThumbnailManager] Completed queue full, dropping: " << request.path << std::endl;
                    if (hBitmap)
                        DeleteObject(hBitmap);  // Must free HBITMAP to prevent leak

                    // Remove from in-flight so it can be requested again later
                    {
                        std::lock_guard<std::mutex> inFlightLock(m_inFlightMutex);
                        m_inFlightPaths.erase(request.path);
                    }
                }
                else
                {
                    m_completedQueue.push({request.path, hBitmap, width, height, request.size, (hBitmap != nullptr)});
                }
            }
        }

        // Uninitialize COM
        CoUninitialize();
    }
    catch (const std::exception& e)
    {
        std::cerr << "ThumbnailManager: Worker thread exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "ThumbnailManager: Worker thread unknown exception" << std::endl;
    }
}

HBITMAP ThumbnailManager::ExtractThumbnail(const std::wstring& path, int size, int& outWidth, int& outHeight)
{
    // Get file extension
    std::filesystem::path p(path);
    std::wstring extension = p.extension().wstring();

    // Convert extension to lowercase for comparison
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);

    // Try each extractor in priority order
    for (auto& extractor : m_extractors)
    {
        if (extractor->CanHandle(extension))
        {
            HBITMAP hBitmap = extractor->Extract(path, size);
            if (hBitmap)
            {
                // Get bitmap dimensions
                BITMAP bm;
                if (GetObject(hBitmap, sizeof(bm), &bm))
                {
                    outWidth = bm.bmWidth;
                    outHeight = abs(bm.bmHeight);
                    return hBitmap;
                }
                else
                {
                    DeleteObject(hBitmap);
                }
            }
        }
    }

    return nullptr;
}

void ThumbnailManager::EvictLRU()
{
    // Build list of entries sorted by last access time (oldest first)
    std::vector<std::pair<std::wstring, std::chrono::steady_clock::time_point>> entries;
    entries.reserve(m_cache.size());

    // Cache only stores completed thumbnails, so all entries are evictable
    for (const auto& [path, entry] : m_cache)
    {
        entries.push_back({path, entry.lastAccessTime});
    }

    // Sort by access time (oldest first)
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  return a.second < b.second;
              });

    // Evict oldest entries until we're under the limit
    int numToEvict = static_cast<int>(m_cache.size()) - m_maxCacheSize;
    int evicted = 0;

    for (int i = 0; i < numToEvict && i < entries.size(); ++i)
    {
        const auto& path = entries[i].first;
        auto it = m_cache.find(path);

        if (it != m_cache.end())
        {
            // Delete OpenGL texture
            if (it->second.glTexture)
            {
                TextureUtils::DeleteTexture(it->second.glTexture);
            }

            m_cache.erase(it);
            evicted++;
        }
    }

    if (evicted > 0)
    {
        std::cout << "ThumbnailManager: Evicted " << evicted << " thumbnails (cache size: "
                  << m_cache.size() << "/" << m_maxCacheSize << ")" << std::endl;
    }
}

bool ThumbnailManager::CreateTextureFromResult(const ThumbnailResult& result, ThumbnailEntry& entry)
{
    if (!result.hBitmap)
        return false;

    // Use TextureUtils to create OpenGL texture
    unsigned int glTexture = 0;
    ImTextureID texID = TextureUtils::CreateTextureFromHBITMAP(result.hBitmap, &glTexture);

    if (texID)
    {
        entry.texID = texID;
        entry.glTexture = glTexture;
        entry.width = result.width;
        entry.height = result.height;
        return true;
    }

    return false;
}
