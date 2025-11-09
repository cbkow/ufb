#pragma once

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <chrono>

namespace UFB {

// Forward declarations
struct Shot;
struct ChangeLogEntry;

/**
 * ArchivalManager handles compression and archival of old change log entries
 * to prevent unbounded growth while maintaining complete history.
 *
 * Design Principles:
 * - Each device archives ONLY its own change logs (no coordination needed)
 * - Archives are compressed monthly files: archive/device-{id}-YYYY-MM.json.gz
 * - Archival threshold: 90 days (configurable)
 * - Archives are immutable once created (safe for concurrent access)
 */
class ArchivalManager
{
public:
    ArchivalManager();
    ~ArchivalManager();

    /**
     * Read all change logs (active + archived) for a job and materialize current state.
     * This replaces MetadataManager::ReadAllChangeLogs with archive support.
     *
     * @param jobPath Path to the job root
     * @param expectedDeviceId Optional device ID for verification (empty = accept all)
     * @param minTimestamp Optional minimum timestamp filter (0 = no filter)
     * @return Materialized map of current shot state (shotPath -> Shot)
     */
    std::map<std::wstring, Shot> ReadAllChangeLogs(
        const std::wstring& jobPath,
        const std::wstring& expectedDeviceId = L"",
        uint64_t minTimestamp = 0);

    /**
     * Archive old entries from this device's change log to compressed monthly files.
     * Only processes entries older than the threshold.
     *
     * @param jobPath Path to the job root
     * @param deviceId This device's ID (will only archive own entries)
     * @param daysThreshold Age threshold in days (default 90)
     * @return True if archival succeeded (or no entries to archive)
     */
    bool ArchiveOldEntries(
        const std::wstring& jobPath,
        const std::string& deviceId,
        int daysThreshold = 90);

    /**
     * Get list of all archive files for a job (for backup inclusion).
     *
     * @param jobPath Path to the job root
     * @return Vector of archive file paths
     */
    std::vector<std::filesystem::path> GetArchiveFiles(const std::wstring& jobPath);

    /**
     * Create a bootstrap snapshot from current materialized state.
     * This is a pre-computed view of all shots for fast cold starts.
     *
     * @param jobPath Path to the job root
     * @return True if snapshot created successfully
     */
    bool CreateBootstrapSnapshot(const std::wstring& jobPath);

    /**
     * Check if a bootstrap snapshot exists and is recent.
     *
     * @param jobPath Path to the job root
     * @param maxAgeHours Maximum age in hours (default 24)
     * @return True if snapshot exists and is within max age
     */
    bool HasRecentBootstrapSnapshot(const std::wstring& jobPath, int maxAgeHours = 24);

    /**
     * Read bootstrap snapshot to get baseline state.
     * Used on first sync to quickly load existing metadata.
     *
     * @param jobPath Path to the job root
     * @return Map of shots from bootstrap, or empty if not found
     */
    std::map<std::wstring, Shot> ReadBootstrapSnapshot(const std::wstring& jobPath);

    /**
     * Read a single device's change log (active + archived).
     * Helper for targeted reads.
     *
     * @param jobPath Path to the job root
     * @param deviceId Device ID to read
     * @return Vector of change log entries in chronological order
     */
    std::vector<ChangeLogEntry> ReadDeviceChangeLogs(
        const std::wstring& jobPath,
        const std::string& deviceId);

private:
    /**
     * Get path to active change log for a device.
     * Format: {jobPath}/.ufb/changes/device-{deviceId}.json
     */
    std::filesystem::path GetActiveChangeLogPath(
        const std::wstring& jobPath,
        const std::string& deviceId);

    /**
     * Get path to archive directory.
     * Format: {jobPath}/.ufb/changes/archive/
     */
    std::filesystem::path GetArchiveDirectory(const std::wstring& jobPath);

    /**
     * Get path to bootstrap snapshot file.
     * Format: {jobPath}/.ufb/changes/bootstrap-snapshot.json
     */
    std::filesystem::path GetBootstrapSnapshotPath(const std::wstring& jobPath);

    /**
     * Get path to monthly archive file.
     * Format: {jobPath}/.ufb/changes/archive/device-{deviceId}-YYYY-MM.json.gz
     */
    std::filesystem::path GetArchivePath(
        const std::wstring& jobPath,
        const std::string& deviceId,
        int year,
        int month);

    /**
     * Read active change log (uncompressed JSON).
     *
     * @param path Path to active log file
     * @return Vector of change log entries
     */
    std::vector<ChangeLogEntry> ReadActiveLog(const std::filesystem::path& path);

    /**
     * Read archived change log (compressed JSON).
     *
     * @param path Path to archive file (.json.gz)
     * @return Vector of change log entries
     */
    std::vector<ChangeLogEntry> ReadArchivedLog(const std::filesystem::path& path);

    /**
     * Write entries to compressed archive file.
     *
     * @param path Path to archive file (.json.gz)
     * @param entries Entries to write
     * @return True if write succeeded
     */
    bool WriteArchivedLog(
        const std::filesystem::path& path,
        const std::vector<ChangeLogEntry>& entries);

    /**
     * Find all archive files for a specific device.
     *
     * @param jobPath Path to the job root
     * @param deviceId Device ID to search for
     * @return Vector of archive paths in chronological order
     */
    std::vector<std::filesystem::path> FindDeviceArchives(
        const std::wstring& jobPath,
        const std::string& deviceId);

    /**
     * Parse year/month from archive filename.
     * Format: device-{id}-YYYY-MM.json.gz
     *
     * @param filename Archive filename
     * @param outYear Output year
     * @param outMonth Output month
     * @return True if parse succeeded
     */
    bool ParseArchiveDate(
        const std::string& filename,
        int& outYear,
        int& outMonth);

    /**
     * Convert timestamp to year/month.
     *
     * @param timestampMs Unix timestamp in milliseconds
     * @param outYear Output year
     * @param outMonth Output month
     */
    void TimestampToYearMonth(
        uint64_t timestampMs,
        int& outYear,
        int& outMonth);

    /**
     * Compress JSON data using gzip.
     *
     * @param jsonStr JSON string to compress
     * @return Compressed binary data
     */
    std::vector<uint8_t> CompressGzip(const std::string& jsonStr);

    /**
     * Decompress gzip data to JSON string.
     *
     * @param compressedData Compressed binary data
     * @return Decompressed JSON string
     */
    std::string DecompressGzip(const std::vector<uint8_t>& compressedData);

    /**
     * Materialize current state from chronologically ordered change log entries.
     * Applies last-write-wins with device ID tie-breaker.
     *
     * @param entries Change log entries (must be sorted by timestamp)
     * @return Materialized shot state
     */
    std::map<std::wstring, Shot> MaterializeState(const std::vector<ChangeLogEntry>& entries,
                                                   std::map<std::wstring, Shot> initialState = {});
};

} // namespace UFB
