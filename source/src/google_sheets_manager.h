#pragma once

#include "google_oauth_manager.h"
#include "project_config.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>
#include <map>
#include <functional>

using json = nlohmann::json;

namespace UFB {

// Forward declarations
class SubscriptionManager;

// Structure representing a row in a Google Sheet
struct SheetRow {
    std::vector<std::string> cells;
};

// Structure representing a Google Sheet
struct GoogleSheet {
    std::string sheetId;      // Sheet ID (tab ID within spreadsheet)
    std::string title;        // Sheet title
    int rowCount;
    int columnCount;
};

// Structure representing a Google Spreadsheet
struct GoogleSpreadsheet {
    std::string spreadsheetId;
    std::string spreadsheetUrl;
    std::string title;
    std::vector<GoogleSheet> sheets;
};

// Structure for batch update request
struct CellUpdate {
    std::string range;        // A1 notation (e.g., "Sheet1!A1:C1")
    std::vector<std::vector<std::string>> values;
};

// Sync status for individual jobs
enum class SheetSyncStatus {
    NotSynced,
    Syncing,
    Synced,
    Error
};

// Job sync record
struct JobSyncRecord {
    std::wstring jobPath;
    std::string spreadsheetId;
    std::string jobFolderId;  // Drive folder ID for this job
    std::map<std::string, std::string> sheetIds;  // Map: itemType → sheetId (e.g., "shot" → "123456")
    uint64_t lastSyncTime;
    SheetSyncStatus status;
    int consecutiveErrorCount = 0;  // Error tracking for this job
    bool disabledDueToErrors = false;  // Auto-disabled after too many errors

    // Legacy fields (deprecated, will be removed after migration)
    std::string sheetId;  // OLD: single sheet ID (replaced by sheetIds map)
    std::string sheetTitle;  // OLD: single sheet title (replaced by per-sheet titles)
};

// Google Sheets API Manager
class GoogleSheetsManager {
public:
    GoogleSheetsManager();
    ~GoogleSheetsManager();

    // Initialize with OAuth manager
    bool Initialize(IGoogleAuth* authManager);

    // Set subscription manager for reading job data
    void SetSubscriptionManager(SubscriptionManager* subscriptionManager);

    // Set operating mode (client/server) - only server mode can use Google Sheets
    void SetOperatingMode(const std::string& mode);
    bool IsServerMode() const;

    // Enable/disable Google Sheets integration
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    // Create a new spreadsheet
    bool CreateSpreadsheet(const std::string& title, GoogleSpreadsheet& outSpreadsheet, const std::string& parentFolderId = "");

    // Create a new sheet within an existing spreadsheet
    bool CreateSheet(const std::string& spreadsheetId,
                    const std::string& sheetTitle,
                    GoogleSheet& outSheet);

    // Get spreadsheet metadata
    bool GetSpreadsheet(const std::string& spreadsheetId, GoogleSpreadsheet& outSpreadsheet);

    // Read data from a sheet
    bool ReadRange(const std::string& spreadsheetId,
                  const std::string& range,
                  std::vector<SheetRow>& outRows);

    // Write data to a sheet (overwrites existing data)
    bool WriteRange(const std::string& spreadsheetId,
                   const std::string& range,
                   const std::vector<SheetRow>& rows);

    // Append data to a sheet
    bool AppendRange(const std::string& spreadsheetId,
                    const std::string& range,
                    const std::vector<SheetRow>& rows);

    // Update specific cells (batch update)
    bool BatchUpdate(const std::string& spreadsheetId,
                    const std::vector<CellUpdate>& updates);

    // Delete rows from a sheet
    bool DeleteRows(const std::string& spreadsheetId,
                   const std::string& sheetId,
                   int startIndex,
                   int endIndex);

    // Clear a range
    bool ClearRange(const std::string& spreadsheetId,
                   const std::string& range);

    // Sync all subscribed jobs to Google Sheets
    bool SyncAllJobs();

    // Sync a specific job to Google Sheets
    bool SyncJob(const std::wstring& jobPath);

    // Remove job from Google Sheets (delete row)
    bool RemoveJobFromSheets(const std::wstring& jobPath);

    // Start background sync loop
    void StartSyncLoop(std::chrono::seconds interval = std::chrono::seconds(60));
    void StopSyncLoop();

    // Get sync records
    std::map<std::wstring, JobSyncRecord> GetSyncRecords() const;

    // Reset all error counts and re-enable disabled jobs
    void ResetAllErrors();

    // Reset ALL sync data (delete sync records file, clear all cached data)
    void ResetAllSyncData();

    // Get master spreadsheet ID
    std::string GetMasterSpreadsheetId() const { return m_masterSpreadsheetId; }
    void SetMasterSpreadsheetId(const std::string& spreadsheetId);

    // Get/set parent folder ID
    std::string GetParentFolderId() const { return m_parentFolderId; }
    void SetParentFolderId(const std::string& folderId);

    // Create master spreadsheet if it doesn't exist (DEPRECATED - use CreateJobSpreadsheet)
    bool CreateMasterSpreadsheet(const std::string& parentFolderId = "");

    // Create job-specific spreadsheet with 4 sheets (Shots, Assets, Postings, Tasks)
    bool CreateJobSpreadsheet(const std::string& jobName, const std::string& jobFolderId, const std::wstring& jobPath, GoogleSpreadsheet& outSpreadsheet);

    // Test connection to Google Sheets API
    bool TestConnection();

private:
    IGoogleAuth* m_authManager;
    SubscriptionManager* m_subscriptionManager;
    bool m_enabled;
    std::string m_masterSpreadsheetId;
    std::string m_parentFolderId;  // Parent folder ID for job folders

    // Operating mode and error tracking
    std::string m_operatingMode;  // "client" or "server"
    int m_consecutiveGlobalFailures;  // Track full-cycle failures across all jobs

    // Sync records tracking
    std::map<std::wstring, JobSyncRecord> m_syncRecords;
    mutable std::mutex m_syncMutex;

    // Background sync loop
    std::thread m_syncThread;
    std::atomic<bool> m_syncRunning;
    std::condition_variable m_syncCV;

    void SyncLoop(std::chrono::seconds interval);

    // Load/save sync records
    bool LoadSyncRecords();
    bool SaveSyncRecords();

    // Helper methods for API calls
    bool ApiGet(const std::string& endpoint, json& outResponse);
    bool ApiPost(const std::string& endpoint, const json& requestBody, json& outResponse);
    bool ApiPut(const std::string& endpoint, const json& requestBody, json& outResponse);
    bool ApiPatch(const std::string& endpoint, const json& requestBody, json& outResponse);
    bool ApiDelete(const std::string& endpoint, json& outResponse);

    // Drive API methods
    bool MoveToFolder(const std::string& fileId, const std::string& parentFolderId);
    std::string CreateFolder(const std::string& title, const std::string& parentFolderId);
    std::string FindFolderByName(const std::string& folderName, const std::string& parentFolderId);
    std::string GetOrCreateJobFolder(const std::string& jobName, const std::string& parentFolderId);
    bool IsFolderTrashed(const std::string& folderId);

    // Build API endpoint URLs
    std::string BuildSpreadsheetsUrl(const std::string& spreadsheetId = "") const;
    std::string BuildValuesUrl(const std::string& spreadsheetId, const std::string& range) const;
    std::string BuildBatchUpdateUrl(const std::string& spreadsheetId) const;

    // Convert job data to sheet rows
    std::vector<SheetRow> ConvertJobToSheetRows(const std::wstring& jobPath);
    std::vector<SheetRow> ConvertJobToSheetRows(const std::wstring& jobPath, const std::string& itemType);

    // Find row index for a job in the sheet (deprecated)
    int FindJobRowIndex(const std::string& spreadsheetId,
                       const std::string& sheetTitle,
                       const std::wstring& jobPath);

    // Find row index for a specific shot/asset in sheet data
    int FindShotRowIndex(const std::vector<SheetRow>& sheetData,
                        const std::wstring& shotPath);

    // Sheet formatting
    bool SetupSheetFormatting(const std::string& spreadsheetId, const std::string& sheetId, const std::wstring& jobPath, const std::string& itemType);
    bool SetColumnDataValidation(const std::string& spreadsheetId, const std::string& sheetId,
                                 int columnIndex, const std::vector<std::string>& options);
    bool SetColumnDataValidationWithColors(const std::string& spreadsheetId, const std::string& sheetId,
                                            int columnIndex, const std::vector<StatusOption>& options);
    bool SetColumnDataValidationWithColors(const std::string& spreadsheetId, const std::string& sheetId,
                                            int columnIndex, const std::vector<CategoryOption>& options);
    bool FormatLinksAsHyperlinks(const std::string& spreadsheetId, const std::string& sheetId);

    // Get dropdown options from ProjectConfig
    std::vector<std::string> GetAllStatusOptions();
    std::vector<std::string> GetAllCategoryOptions();

    // Get item-type-specific options with colors from per-job config
    std::vector<StatusOption> GetStatusOptionsForItemType(const std::wstring& jobPath, const std::string& itemType);
    std::vector<CategoryOption> GetCategoryOptionsForItemType(const std::wstring& jobPath, const std::string& itemType);

    // Error handling helper
    void CheckAndDisableJob(JobSyncRecord& record, const std::string& jobName);

    // Utility functions
    std::string WideToUtf8(const std::wstring& wstr) const;
    std::wstring Utf8ToWide(const std::string& str) const;

    uint64_t GetCurrentTimestamp() const;
};

} // namespace UFB
