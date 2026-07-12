// ThumbCache.h — persistent on-disk thumbnail cache.
//
// The thumbnail pipeline (shell stage-0 + the EXR/PSD/PDF/video
// decoders) is expensive: ffmpeg open+seek+decode, OpenEXR/psd_sdk
// reads, shell IPC round-trips. Without a cache every folder revisit
// or scroll-back re-runs the full extraction. This cache stores one
// encoded master thumbnail per file in a dedicated SQLite database so
// repeat views are a single decode of a small PNG/JPEG blob.
//
// Why a separate DB (not the Rust-owned metadata DB): the cache has a
// single owner (this C++ thumbnail subsystem), so keeping it here —
// out of ufb_v4.db — avoids bloating the metadata DB, dodges WAL
// write-contention with metadata, and lets the cache be wiped
// independently. It lives under QStandardPaths::CacheLocation, which
// the OS may reclaim.
//
// Threading: the thumbnail pool runs up to 4 workers (see
// UfbImageProviders.cpp). QSqlDatabase connections are not shareable
// across threads, so each worker gets its own connection (keyed in
// thread-local storage) to the same file. SQLite in WAL mode handles
// concurrent readers + a single writer across those connections.

#pragma once

#include <QImage>
#include <QString>

namespace ufb {

// Canonical master size: every cached thumbnail is generated at this
// max dimension (aspect-preserving). 512 covers a 256 px logical grid
// cell at 2x device-pixel-ratio, so one cached master serves every
// grid size and DPR; the provider downscales per request.
constexpr int kThumbMasterSize = 512;

class ThumbCache {
public:
    static ThumbCache& instance();

    // Returns the cached master QImage iff a row exists for `path`
    // AND the stored (fileSize, mtimeMs) match the current file
    // (cheap staleness check — no content hashing). Null QImage on
    // miss or any DB error. Bumps last-access on hit.
    QImage get(const QString& path, qint64 fileSize, qint64 mtimeMs);

    // Encodes `master` (PNG if it has alpha, else JPEG) and stores it
    // keyed by `path`, replacing any prior row. Runs LRU eviction
    // afterwards. Silently no-ops on encode/DB failure — the cache is
    // an optimization, never a correctness dependency.
    void put(const QString& path, qint64 fileSize, qint64 mtimeMs,
             const QImage& master);

private:
    ThumbCache() = default;

    // Total byte budget for the cache. When exceeded after a put, the
    // least-recently-accessed rows are evicted until back under cap.
    static constexpr qint64 kMaxBytes = 512LL * 1024 * 1024;  // 512 MB
};

}  // namespace ufb
