#include "google_sheets_manager.h"
#include "subscription_manager.h"
#include "project_config.h"
#include "utils.h"
#include <Windows.h>
#include <ShlObj.h>
#include <winhttp.h>
#include <wininet.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <codecvt>
#include <chrono>
#include <set>

#pragma comment(lib, "winhttp.lib")

namespace UFB {

GoogleSheetsManager::GoogleSheetsManager()
    : m_authManager(nullptr)
    , m_subscriptionManager(nullptr)
    , m_enabled(false)
    , m_operatingMode("client")
    , m_consecutiveGlobalFailures(0)
    , m_syncRunning(false)
{
}

GoogleSheetsManager::~GoogleSheetsManager()
{
    StopSyncLoop();
}

bool GoogleSheetsManager::Initialize(IGoogleAuth* authManager)
{
    if (!authManager) {
        std::cerr << "[GoogleSheetsManager] Auth manager cannot be null" << std::endl;
        return false;
    }

    m_authManager = authManager;
    LoadSyncRecords();

    std::cout << "[GoogleSheetsManager] Initialized successfully" << std::endl;
    std::cout << "[GoogleSheetsManager] Current parent folder ID: "
              << (m_parentFolderId.empty() ? "(not set)" : m_parentFolderId) << std::endl;
    return true;
}

void GoogleSheetsManager::SetSubscriptionManager(SubscriptionManager* subscriptionManager)
{
    m_subscriptionManager = subscriptionManager;
}

void GoogleSheetsManager::SetOperatingMode(const std::string& mode)
{
    m_operatingMode = mode;
    std::cout << "[GoogleSheetsManager] Operating mode set to: " << mode << std::endl;

    if (mode != "server") {
        std::cout << "[GoogleSheetsManager] Google Sheets integration disabled in client mode" << std::endl;
    }
}

bool GoogleSheetsManager::IsServerMode() const
{
    return m_operatingMode == "server";
}

void GoogleSheetsManager::SetEnabled(bool enabled)
{
    m_enabled = enabled;

    if (enabled) {
        std::cout << "[GoogleSheetsManager] Google Sheets integration enabled" << std::endl;
    } else {
        std::cout << "[GoogleSheetsManager] Google Sheets integration disabled" << std::endl;
        StopSyncLoop();
    }
}

void GoogleSheetsManager::SetMasterSpreadsheetId(const std::string& spreadsheetId)
{
    std::lock_guard<std::mutex> lock(m_syncMutex);
    m_masterSpreadsheetId = spreadsheetId;
}

void GoogleSheetsManager::SetParentFolderId(const std::string& folderId)
{
    std::lock_guard<std::mutex> lock(m_syncMutex);
    std::cout << "[GoogleSheetsManager] SetParentFolderId called with: " << folderId << std::endl;
    m_parentFolderId = folderId;
    std::cout << "[GoogleSheetsManager] Parent folder ID updated successfully" << std::endl;
}

bool GoogleSheetsManager::CreateSpreadsheet(const std::string& title, GoogleSpreadsheet& outSpreadsheet, const std::string& parentFolderId)
{
    std::cout << "[GoogleSheetsManager] Creating spreadsheet: " << title << std::endl;

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    json requestBody;
    requestBody["properties"]["title"] = title;

    json response;
    if (!ApiPost(BuildSpreadsheetsUrl(), requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to create spreadsheet" << std::endl;
        return false;
    }

    // Check if response contains error
    if (response.contains("error")) {
        std::cerr << "[GoogleSheetsManager] API Error: " << response["error"].dump(2) << std::endl;
        return false;
    }

    // Check required fields exist and are not null
    if (!response.contains("spreadsheetId") || response["spreadsheetId"].is_null()) {
        std::cerr << "[GoogleSheetsManager] Response missing spreadsheetId" << std::endl;
        return false;
    }

    // Extract spreadsheet data from response
    outSpreadsheet.spreadsheetId = response["spreadsheetId"].get<std::string>();
    outSpreadsheet.spreadsheetUrl = response.value("spreadsheetUrl", "");

    if (response.contains("properties") && response["properties"].contains("title")) {
        outSpreadsheet.title = response["properties"]["title"].get<std::string>();
    } else {
        outSpreadsheet.title = title;
    }

    // Parse sheets
    if (response.contains("sheets")) {
        for (const auto& sheet : response["sheets"]) {
            GoogleSheet gs;
            gs.sheetId = std::to_string(sheet["properties"]["sheetId"].get<int>());
            gs.title = sheet["properties"]["title"];
            gs.rowCount = sheet["properties"]["gridProperties"]["rowCount"];
            gs.columnCount = sheet["properties"]["gridProperties"]["columnCount"];
            outSpreadsheet.sheets.push_back(gs);
        }
    }

    std::cout << "[GoogleSheetsManager] Created spreadsheet: " << outSpreadsheet.title
              << " (ID: " << outSpreadsheet.spreadsheetId << ")" << std::endl;

    // If parent folder ID is specified, move spreadsheet to that folder
    if (!parentFolderId.empty()) {
        std::cout << "[GoogleSheetsManager] Attempting to move spreadsheet '" << outSpreadsheet.title
                  << "' (ID: " << outSpreadsheet.spreadsheetId << ") to folder: " << parentFolderId << std::endl;
        if (MoveToFolder(outSpreadsheet.spreadsheetId, parentFolderId)) {
            std::cout << "[GoogleSheetsManager] ✓ Successfully moved spreadsheet to folder: " << parentFolderId << std::endl;
        } else {
            std::cerr << "[GoogleSheetsManager] ✗ WARNING: Failed to move spreadsheet to folder: " << parentFolderId << std::endl;
            std::cerr << "[GoogleSheetsManager] ✗ Spreadsheet may be in 'My Drive' root instead of intended location" << std::endl;
            std::cerr << "[GoogleSheetsManager] ✗ Check that parent folder ID is valid and accessible" << std::endl;
            // Don't fail the entire operation - spreadsheet was still created successfully
        }
    } else {
        std::cout << "[GoogleSheetsManager] No parent folder ID specified - spreadsheet created in My Drive root" << std::endl;
    }

    return true;
}

bool GoogleSheetsManager::CreateSheet(const std::string& spreadsheetId,
                                     const std::string& sheetTitle,
                                     GoogleSheet& outSheet)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    json requestBody;
    requestBody["requests"][0]["addSheet"]["properties"]["title"] = sheetTitle;

    json response;
    if (!ApiPost(BuildBatchUpdateUrl(spreadsheetId), requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to create sheet" << std::endl;
        return false;
    }

    if (response.contains("replies") && response["replies"].size() > 0) {
        const auto& addSheetReply = response["replies"][0]["addSheet"]["properties"];
        outSheet.sheetId = std::to_string(addSheetReply["sheetId"].get<int>());
        outSheet.title = addSheetReply["title"];
        outSheet.rowCount = addSheetReply["gridProperties"]["rowCount"];
        outSheet.columnCount = addSheetReply["gridProperties"]["columnCount"];
    }

    std::cout << "[GoogleSheetsManager] Created sheet: " << sheetTitle << std::endl;
    return true;
}

bool GoogleSheetsManager::GetSpreadsheet(const std::string& spreadsheetId, GoogleSpreadsheet& outSpreadsheet)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    json response;
    if (!ApiGet(BuildSpreadsheetsUrl(spreadsheetId), response)) {
        std::cerr << "[GoogleSheetsManager] Failed to get spreadsheet" << std::endl;
        return false;
    }

    outSpreadsheet.spreadsheetId = response["spreadsheetId"];
    outSpreadsheet.spreadsheetUrl = response["spreadsheetUrl"];
    outSpreadsheet.title = response["properties"]["title"];

    if (response.contains("sheets")) {
        for (const auto& sheet : response["sheets"]) {
            GoogleSheet gs;
            gs.sheetId = std::to_string(sheet["properties"]["sheetId"].get<int>());
            gs.title = sheet["properties"]["title"];
            gs.rowCount = sheet["properties"]["gridProperties"]["rowCount"];
            gs.columnCount = sheet["properties"]["gridProperties"]["columnCount"];
            outSpreadsheet.sheets.push_back(gs);
        }
    }

    return true;
}

bool GoogleSheetsManager::ReadRange(const std::string& spreadsheetId,
                                   const std::string& range,
                                   std::vector<SheetRow>& outRows)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    json response;
    if (!ApiGet(BuildValuesUrl(spreadsheetId, range), response)) {
        std::cerr << "[GoogleSheetsManager] Failed to read range" << std::endl;
        return false;
    }

    if (!response.contains("values")) {
        return true;
    }

    for (const auto& row : response["values"]) {
        SheetRow sheetRow;
        for (const auto& cell : row) {
            if (cell.is_string()) {
                sheetRow.cells.push_back(cell.get<std::string>());
            } else {
                sheetRow.cells.push_back(cell.dump());
            }
        }
        outRows.push_back(sheetRow);
    }

    return true;
}

bool GoogleSheetsManager::WriteRange(const std::string& spreadsheetId,
                                    const std::string& range,
                                    const std::vector<SheetRow>& rows)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    json requestBody;
    requestBody["range"] = range;
    requestBody["majorDimension"] = "ROWS";

    json values = json::array();
    for (const auto& row : rows) {
        json rowValues = json::array();
        for (const auto& cell : row.cells) {
            rowValues.push_back(cell);
        }
        values.push_back(rowValues);
    }
    requestBody["values"] = values;

    std::string url = BuildValuesUrl(spreadsheetId, range) + "?valueInputOption=RAW";

    json response;
    if (!ApiPut(url, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to write range" << std::endl;
        return false;
    }

    return true;
}

bool GoogleSheetsManager::AppendRange(const std::string& spreadsheetId,
                                     const std::string& range,
                                     const std::vector<SheetRow>& rows)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    json requestBody;
    requestBody["range"] = range;
    requestBody["majorDimension"] = "ROWS";

    json values = json::array();
    for (const auto& row : rows) {
        json rowValues = json::array();
        for (const auto& cell : row.cells) {
            rowValues.push_back(cell);
        }
        values.push_back(rowValues);
    }
    requestBody["values"] = values;

    std::string url = BuildValuesUrl(spreadsheetId, range) + ":append?valueInputOption=RAW";

    json response;
    if (!ApiPost(url, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to append range" << std::endl;
        return false;
    }

    return true;
}

bool GoogleSheetsManager::BatchUpdate(const std::string& spreadsheetId,
                                     const std::vector<CellUpdate>& updates)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    json requestBody;
    json data = json::array();

    for (const auto& update : updates) {
        json updateData;
        updateData["range"] = update.range;
        updateData["majorDimension"] = "ROWS";

        json values = json::array();
        for (const auto& row : update.values) {
            json rowValues = json::array();
            for (const auto& cell : row) {
                rowValues.push_back(cell);
            }
            values.push_back(rowValues);
        }
        updateData["values"] = values;
        data.push_back(updateData);
    }

    requestBody["data"] = data;
    requestBody["valueInputOption"] = "RAW";

    std::string url = BuildSpreadsheetsUrl(spreadsheetId) + "/values:batchUpdate";

    json response;
    if (!ApiPost(url, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to batch update" << std::endl;
        return false;
    }

    return true;
}

bool GoogleSheetsManager::DeleteRows(const std::string& spreadsheetId,
                                    const std::string& sheetId,
                                    int startIndex,
                                    int endIndex)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    json requestBody;
    requestBody["requests"][0]["deleteDimension"]["range"]["sheetId"] = std::stoi(sheetId);
    requestBody["requests"][0]["deleteDimension"]["range"]["dimension"] = "ROWS";
    requestBody["requests"][0]["deleteDimension"]["range"]["startIndex"] = startIndex;
    requestBody["requests"][0]["deleteDimension"]["range"]["endIndex"] = endIndex;

    json response;
    if (!ApiPost(BuildBatchUpdateUrl(spreadsheetId), requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to delete rows" << std::endl;
        return false;
    }

    return true;
}

bool GoogleSheetsManager::ClearRange(const std::string& spreadsheetId,
                                    const std::string& range)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    std::string url = BuildValuesUrl(spreadsheetId, range) + ":clear";

    json requestBody = json::object();  // Google Sheets API requires an empty object, not null
    json response;

    if (!ApiPost(url, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to clear range" << std::endl;
        return false;
    }

    return true;
}

bool GoogleSheetsManager::SyncAllJobs()
{
    // Server mode only - client mode should never sync
    if (!IsServerMode()) {
        return false;
    }

    if (!m_enabled || !m_subscriptionManager) {
        return false;
    }

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated - cannot sync" << std::endl;
        return false;
    }

    auto jobs = m_subscriptionManager->GetActiveSubscriptions();
    std::cout << "[GoogleSheetsManager] Starting sync for " << jobs.size() << " job(s)..." << std::endl;

    int successCount = 0;
    int failureCount = 0;
    int totalJobs = 0;

    for (const auto& job : jobs) {
        // Skip jobs that are disabled due to errors
        auto it = m_syncRecords.find(job.jobPath);
        if (it != m_syncRecords.end() && it->second.disabledDueToErrors) {
            continue;  // Don't count disabled jobs
        }

        totalJobs++;

        if (SyncJob(job.jobPath)) {
            successCount++;
        } else {
            failureCount++;
        }
    }

    std::cout << "[GoogleSheetsManager] Sync complete: " << successCount << " succeeded, "
              << failureCount << " failed" << std::endl;

    // Track consecutive full-cycle failures
    const int GLOBAL_FAILURE_THRESHOLD = 3;

    if (totalJobs > 0 && successCount == 0) {
        // All jobs failed this cycle
        m_consecutiveGlobalFailures++;
        std::cerr << "[GoogleSheetsManager] ⚠ All jobs failed to sync ("
                  << m_consecutiveGlobalFailures << "/" << GLOBAL_FAILURE_THRESHOLD << ")" << std::endl;

        if (m_consecutiveGlobalFailures >= GLOBAL_FAILURE_THRESHOLD) {
            std::cerr << "[GoogleSheetsManager] ✗ TOO MANY CONSECUTIVE GLOBAL FAILURES - STOPPING SYNC LOOP" << std::endl;
            std::cerr << "[GoogleSheetsManager] Check authentication, network, and Google Drive permissions" << std::endl;
            std::cerr << "[GoogleSheetsManager] Re-enable Google Sheets in settings to restart sync" << std::endl;
            StopSyncLoop();
            SetEnabled(false);
            SaveSyncRecords();
            return false;
        }
    } else if (successCount > 0) {
        // At least one job succeeded - reset global failure counter
        m_consecutiveGlobalFailures = 0;
    }

    SaveSyncRecords();
    return successCount > 0;
}

bool GoogleSheetsManager::SyncJob(const std::wstring& jobPath)
{
    // Server mode only - client mode should never sync
    if (!IsServerMode()) {
        return false;
    }

    if (!m_enabled || !m_subscriptionManager) {
        return false;
    }

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated - cannot sync job" << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(m_syncMutex);

    // Extract job name from path
    size_t lastSlash = jobPath.find_last_of(L"\\/");
    std::wstring jobName = (lastSlash != std::wstring::npos) ? jobPath.substr(lastSlash + 1) : jobPath;
    std::string jobNameUtf8 = WideToUtf8(jobName);

    // Check if this job is disabled due to too many errors
    auto it = m_syncRecords.find(jobPath);
    if (it != m_syncRecords.end() && it->second.disabledDueToErrors) {
        std::cout << "[GoogleSheetsManager] Job '" << jobNameUtf8 << "' is disabled due to errors - skipping" << std::endl;
        return false;
    }

    // Validate parent folder ID is set
    if (m_parentFolderId.empty()) {
        std::cerr << "[GoogleSheetsManager] ✗ Parent folder ID required for Google Sheets sync" << std::endl;
        std::cerr << "[GoogleSheetsManager] ✗ Set parent folder ID in Settings → Google Sheets section" << std::endl;
        if (it != m_syncRecords.end()) {
            it->second.consecutiveErrorCount++;
            CheckAndDisableJob(it->second, jobNameUtf8);
        }
        return false;
    }

    std::cout << "[GoogleSheetsManager] Parent Folder ID: " << m_parentFolderId << std::endl;

    // Get or create job folder
    // Check if we have a cached jobFolderId and validate it's not trashed
    std::string jobFolderId;
    if (it != m_syncRecords.end() && !it->second.jobFolderId.empty()) {
        std::cout << "[GoogleSheetsManager] Checking cached job folder ID: " << it->second.jobFolderId << std::endl;
        if (IsFolderTrashed(it->second.jobFolderId)) {
            std::cout << "[GoogleSheetsManager] Cached folder is trashed - will create new folder" << std::endl;
            it->second.jobFolderId = "";  // Clear cached ID
            jobFolderId = "";
        } else {
            std::cout << "[GoogleSheetsManager] Cached folder is valid - reusing" << std::endl;
            jobFolderId = it->second.jobFolderId;
        }
    }

    // If no valid cached folder, search or create
    if (jobFolderId.empty()) {
        std::cout << "[GoogleSheetsManager] Getting/creating job folder '" << jobNameUtf8
                  << "' inside parent folder: " << m_parentFolderId << std::endl;
        jobFolderId = GetOrCreateJobFolder(jobNameUtf8, m_parentFolderId);
    }

    if (jobFolderId.empty()) {
        std::cerr << "[GoogleSheetsManager] ✗ Failed to get/create job folder: " << jobNameUtf8 << std::endl;
        std::cerr << "[GoogleSheetsManager] ✗ Check that parent folder ID is valid and accessible" << std::endl;
        if (it != m_syncRecords.end()) {
            it->second.consecutiveErrorCount++;
            CheckAndDisableJob(it->second, jobNameUtf8);
        }
        return false;
    }

    std::cout << "[GoogleSheetsManager] ✓ Job folder ID: " << jobFolderId << std::endl;

    // Create or update job sync record
    if (it == m_syncRecords.end()) {
        // New job - create spreadsheet and sheets
        std::cout << "[GoogleSheetsManager] Creating new spreadsheet for job: " << jobNameUtf8 << std::endl;

        GoogleSpreadsheet spreadsheet;
        if (!CreateJobSpreadsheet(jobNameUtf8, jobFolderId, jobPath, spreadsheet)) {
            std::cerr << "[GoogleSheetsManager] Failed to create job spreadsheet" << std::endl;
            return false;
        }

        // Create new sync record
        JobSyncRecord record;
        record.jobPath = jobPath;
        record.spreadsheetId = spreadsheet.spreadsheetId;
        record.jobFolderId = jobFolderId;
        record.lastSyncTime = 0;
        record.status = SheetSyncStatus::Syncing;
        record.consecutiveErrorCount = 0;
        record.disabledDueToErrors = false;

        // Map sheet names to IDs
        for (const auto& sheet : spreadsheet.sheets) {
            if (sheet.title == "Shots") {
                record.sheetIds["shot"] = sheet.sheetId;
            } else if (sheet.title == "Assets") {
                record.sheetIds["asset"] = sheet.sheetId;
            } else if (sheet.title == "Postings") {
                record.sheetIds["posting"] = sheet.sheetId;
            } else if (sheet.title == "Tasks") {
                record.sheetIds["manual_task"] = sheet.sheetId;
            }
        }

        m_syncRecords[jobPath] = record;
        it = m_syncRecords.find(jobPath);
    }

    JobSyncRecord& record = it->second;
    record.status = SheetSyncStatus::Syncing;

    // Sync each item type to its own sheet
    std::vector<std::string> itemTypes = {"shot", "asset", "posting", "manual_task"};
    std::map<std::string, std::string> sheetNames = {
        {"shot", "Shots"},
        {"asset", "Assets"},
        {"posting", "Postings"},
        {"manual_task", "Tasks"}
    };

    bool allSucceeded = true;
    int totalRowsSynced = 0;

    for (const std::string& itemType : itemTypes) {
        std::string sheetName = sheetNames[itemType];
        std::string sheetId = record.sheetIds[itemType];

        if (sheetId.empty()) {
            std::cerr << "[GoogleSheetsManager] Warning: No sheet ID for " << sheetName << std::endl;
            continue;
        }

        // Get data for this item type
        std::vector<SheetRow> rows = ConvertJobToSheetRows(jobPath, itemType);

        // Read existing sheet data for smart merge
        std::string readRange = sheetName + "!A:K";
        std::vector<SheetRow> existingData;
        ReadRange(record.spreadsheetId, readRange, existingData);

        // Build map of existing items by path (column A)
        std::map<std::string, SheetRow> existingItems;
        for (size_t i = 1; i < existingData.size(); ++i) {  // Skip header
            if (!existingData[i].cells.empty()) {
                existingItems[existingData[i].cells[0]] = existingData[i];
            }
        }

        // Smart merge: preserve user edits in columns D-J
        std::vector<SheetRow> mergedRows;
        for (auto& newRow : rows) {
            if (newRow.cells.empty()) continue;

            std::string itemPath = newRow.cells[0];
            auto existingIt = existingItems.find(itemPath);

            if (existingIt != existingItems.end()) {
                SheetRow& existingRow = existingIt->second;
                SheetRow mergedRow = newRow;

                // Preserve columns D-J if not empty
                for (int col = 3; col <= 9; col++) {
                    if (existingRow.cells.size() > col && !existingRow.cells[col].empty()) {
                        mergedRow.cells[col] = existingRow.cells[col];
                    }
                }

                mergedRows.push_back(mergedRow);
            } else {
                mergedRows.push_back(newRow);
            }
        }

        // Clear and write data (not append - fixes duplicate rows)
        std::string clearRange = sheetName + "!A2:K";
        if (!ClearRange(record.spreadsheetId, clearRange)) {
            std::cerr << "[GoogleSheetsManager] Failed to clear range for " << sheetName << std::endl;
            allSucceeded = false;
            continue;
        }

        if (!mergedRows.empty()) {
            std::string writeRange = sheetName + "!A2:K";
            if (!WriteRange(record.spreadsheetId, writeRange, mergedRows)) {
                std::cerr << "[GoogleSheetsManager] Failed to write data to " << sheetName << std::endl;
                allSucceeded = false;
                continue;
            }
            totalRowsSynced += mergedRows.size();
        }

        std::cout << "[GoogleSheetsManager] ✓ Synced " << mergedRows.size() << " " << sheetName << std::endl;
    }

    // Update sync record based on success/failure
    if (allSucceeded) {
        record.lastSyncTime = GetCurrentTimestamp();
        record.status = SheetSyncStatus::Synced;
        record.consecutiveErrorCount = 0;  // Reset on success
        record.disabledDueToErrors = false;
        std::cout << "[GoogleSheetsManager] ✓ Successfully synced job '" << jobNameUtf8
                  << "' (" << totalRowsSynced << " total rows)" << std::endl;
        SaveSyncRecords();
        return true;
    } else {
        record.status = SheetSyncStatus::Error;
        record.consecutiveErrorCount++;
        CheckAndDisableJob(record, jobNameUtf8);
        std::cerr << "[GoogleSheetsManager] ✗ Failed to sync job '" << jobNameUtf8 << "'" << std::endl;
        SaveSyncRecords();
        return false;
    }
}

bool GoogleSheetsManager::RemoveJobFromSheets(const std::wstring& jobPath)
{
    if (!m_enabled) {
        return false;
    }

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(m_syncMutex);

    auto it = m_syncRecords.find(jobPath);
    if (it == m_syncRecords.end()) {
        return true;
    }

    JobSyncRecord& record = it->second;

    int rowIndex = FindJobRowIndex(record.spreadsheetId, record.sheetTitle, jobPath);

    if (rowIndex >= 0) {
        if (DeleteRows(record.spreadsheetId, record.sheetId, rowIndex, rowIndex + 1)) {
            m_syncRecords.erase(it);
            SaveSyncRecords();
            std::cout << "[GoogleSheetsManager] Removed job from sheets: " << WideToUtf8(jobPath) << std::endl;
            return true;
        }
    }

    return false;
}

void GoogleSheetsManager::StartSyncLoop(std::chrono::seconds interval)
{
    // Server mode only - client mode should never run sync loop
    if (!IsServerMode()) {
        std::cout << "[GoogleSheetsManager] Sync loop not started - client mode" << std::endl;
        return;
    }

    if (m_syncRunning) {
        return;
    }

    m_syncRunning = true;
    m_syncThread = std::thread(&GoogleSheetsManager::SyncLoop, this, interval);
    std::cout << "[GoogleSheetsManager] Started sync loop with " << interval.count() << "s interval" << std::endl;
}

void GoogleSheetsManager::StopSyncLoop()
{
    if (m_syncRunning) {
        m_syncRunning = false;
        m_syncCV.notify_all();

        if (m_syncThread.joinable()) {
            m_syncThread.join();
        }

        std::cout << "[GoogleSheetsManager] Stopped sync loop" << std::endl;
    }
}

std::map<std::wstring, JobSyncRecord> GoogleSheetsManager::GetSyncRecords() const
{
    std::lock_guard<std::mutex> lock(m_syncMutex);
    return m_syncRecords;
}

void GoogleSheetsManager::ResetAllErrors()
{
    std::lock_guard<std::mutex> lock(m_syncMutex);

    int resetCount = 0;
    for (auto& [jobPath, record] : m_syncRecords) {
        if (record.disabledDueToErrors || record.consecutiveErrorCount > 0) {
            record.consecutiveErrorCount = 0;
            record.disabledDueToErrors = false;
            if (record.status == SheetSyncStatus::Error) {
                record.status = SheetSyncStatus::NotSynced;
            }
            resetCount++;
        }
    }

    // Reset global failure counter
    m_consecutiveGlobalFailures = 0;

    std::cout << "[GoogleSheetsManager] Reset errors for " << resetCount << " job(s)" << std::endl;
    std::cout << "[GoogleSheetsManager] Global failure counter reset" << std::endl;

    // Re-enable Google Sheets if it was disabled due to global failures
    if (resetCount > 0) {
        SetEnabled(true);
        std::cout << "[GoogleSheetsManager] Google Sheets re-enabled" << std::endl;
    }

    SaveSyncRecords();
}

void GoogleSheetsManager::ResetAllSyncData()
{
    std::lock_guard<std::mutex> lock(m_syncMutex);

    int recordCount = m_syncRecords.size();

    // Clear all sync records from memory
    m_syncRecords.clear();

    // Reset global failure counter
    m_consecutiveGlobalFailures = 0;

    std::cout << "[GoogleSheetsManager] Cleared " << recordCount << " sync record(s) from memory" << std::endl;
    std::cout << "[GoogleSheetsManager] Global failure counter reset" << std::endl;

    // Delete the sync records file
    wchar_t* appDataPathStr = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &appDataPathStr) != S_OK) {
        std::cerr << "[GoogleSheetsManager] Failed to get LocalAppData path" << std::endl;
        return;
    }

    std::filesystem::path appDataPath = std::filesystem::path(appDataPathStr) / L"ufb";
    CoTaskMemFree(appDataPathStr);

    std::wstring recordsPath = (appDataPath / L"google_sheets_sync_records.json").wstring();

    // Delete the file if it exists
    try {
        if (std::filesystem::exists(recordsPath)) {
            std::filesystem::remove(recordsPath);
            std::wcout << L"[GoogleSheetsManager] Deleted sync records file: " << recordsPath << std::endl;
        } else {
            std::cout << "[GoogleSheetsManager] Sync records file does not exist (already cleared)" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[GoogleSheetsManager] Failed to delete sync records file: " << e.what() << std::endl;
    }

    std::cout << "[GoogleSheetsManager] ✓ Full reset complete - all sync data cleared" << std::endl;
}

bool GoogleSheetsManager::CreateMasterSpreadsheet(const std::string& parentFolderId)
{
    GoogleSpreadsheet spreadsheet;

    if (CreateSpreadsheet("UFB All Projects Tracker", spreadsheet, parentFolderId)) {
        SetMasterSpreadsheetId(spreadsheet.spreadsheetId);
        std::cout << "[GoogleSheetsManager] Created master spreadsheet: " << spreadsheet.spreadsheetUrl << std::endl;
        return true;
    }

    std::cerr << "[GoogleSheetsManager] Failed to create master spreadsheet" << std::endl;
    return false;
}

bool GoogleSheetsManager::CreateJobSpreadsheet(const std::string& jobName, const std::string& jobFolderId, const std::wstring& jobPath, GoogleSpreadsheet& outSpreadsheet)
{
    std::cout << "[GoogleSheetsManager] CreateJobSpreadsheet: " << jobName << " in folder: " << jobFolderId << std::endl;

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    if (jobFolderId.empty()) {
        std::cerr << "[GoogleSheetsManager] CreateJobSpreadsheet failed - jobFolderId is required" << std::endl;
        return false;
    }

    // Create spreadsheet with job name
    std::string spreadsheetTitle = jobName + " Tracker";
    if (!CreateSpreadsheet(spreadsheetTitle, outSpreadsheet, jobFolderId)) {
        std::cerr << "[GoogleSheetsManager] Failed to create spreadsheet for job: " << jobName << std::endl;
        return false;
    }

    // At this point, spreadsheet has been created and moved to job folder
    // It has one default sheet (usually called "Sheet1")
    // We will create 4 new sheets (Shots, Assets, Postings, Tasks) and delete the default sheet

    std::string spreadsheetId = outSpreadsheet.spreadsheetId;
    std::string defaultSheetId = outSpreadsheet.sheets.empty() ? "" : outSpreadsheet.sheets[0].sheetId;

    // Build batch update request to:
    // 1. Add "Shots" sheet
    // 2. Add "Assets" sheet
    // 3. Add "Postings" sheet
    // 4. Add "Tasks" sheet
    // 5. Delete the default Sheet1
    json requests = json::array();

    // Requests 1-4: Add all 4 sheets
    std::vector<std::string> newSheetNames = {"Shots", "Assets", "Postings", "Tasks"};
    for (const auto& sheetName : newSheetNames) {
        json addSheetRequest;
        addSheetRequest["addSheet"]["properties"]["title"] = sheetName;
        addSheetRequest["addSheet"]["properties"]["gridProperties"]["rowCount"] = 1000;
        addSheetRequest["addSheet"]["properties"]["gridProperties"]["columnCount"] = 11;
        requests.push_back(addSheetRequest);
    }

    // Request 5: Delete the default sheet (Sheet1) if it exists
    if (!defaultSheetId.empty()) {
        json deleteRequest;
        deleteRequest["deleteSheet"]["sheetId"] = std::stoi(defaultSheetId);
        requests.push_back(deleteRequest);
        std::cout << "[GoogleSheetsManager] Queued deletion of default sheet (ID: " << defaultSheetId << ")" << std::endl;
    }

    // Execute batch update
    json requestBody;
    requestBody["requests"] = requests;

    json response;
    std::string batchUpdateUrl = BuildBatchUpdateUrl(spreadsheetId);
    if (!ApiPost(batchUpdateUrl, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to create sheets for job: " << jobName << std::endl;
        return false;
    }

    // Update outSpreadsheet with new sheet info
    // Re-fetch spreadsheet to get updated sheet list with correct IDs
    if (!GetSpreadsheet(spreadsheetId, outSpreadsheet)) {
        std::cerr << "[GoogleSheetsManager] Failed to fetch updated spreadsheet info" << std::endl;
        return false;
    }

    std::cout << "[GoogleSheetsManager] Created job spreadsheet with " << outSpreadsheet.sheets.size() << " sheets" << std::endl;

    // Apply formatting and validation to all sheets (skip Sheet1 if it still exists)
    for (const auto& sheet : outSpreadsheet.sheets) {
        // Determine itemType from sheet title
        std::string itemType;
        if (sheet.title == "Shots") {
            itemType = "shot";
        } else if (sheet.title == "Assets") {
            itemType = "asset";
        } else if (sheet.title == "Postings") {
            itemType = "posting";
        } else if (sheet.title == "Tasks") {
            itemType = "manual_task";
        } else {
            // Skip unknown sheets (like Sheet1 if deletion hasn't propagated yet)
            std::cout << "[GoogleSheetsManager] Skipping unknown sheet: " << sheet.title << " (ID: " << sheet.sheetId << ")" << std::endl;
            continue;
        }

        std::cout << "[GoogleSheetsManager] Applying formatting to sheet: " << sheet.title << " (ID: " << sheet.sheetId << ")" << std::endl;

        // Write headers for this sheet
        std::vector<SheetRow> headerRows;
        SheetRow header;
        header.cells = {"Shot Path", "Item Type", "Folder Type", "Status", "Category",
                       "Priority", "Due Date", "Artist", "Note", "Links", "Last Modified"};
        headerRows.push_back(header);

        std::string headerRange = sheet.title + "!A1:K1";
        if (!WriteRange(spreadsheetId, headerRange, headerRows)) {
            std::cerr << "[GoogleSheetsManager] Warning: Failed to write headers for sheet: " << sheet.title << std::endl;
        }

        // Apply formatting and data validation with per-job, per-itemType options
        if (!SetupSheetFormatting(spreadsheetId, sheet.sheetId, jobPath, itemType)) {
            std::cerr << "[GoogleSheetsManager] Warning: Failed to apply formatting to sheet: " << sheet.title << std::endl;
        }
    }

    std::cout << "[GoogleSheetsManager] Job spreadsheet created successfully: " << outSpreadsheet.spreadsheetUrl << std::endl;
    return true;
}

bool GoogleSheetsManager::TestConnection()
{
    if (!m_authManager) {
        return false;
    }

    return m_authManager->TestConnection();
}

// Private methods

void GoogleSheetsManager::SyncLoop(std::chrono::seconds interval)
{
    while (m_syncRunning) {
        if (m_enabled && m_authManager && m_authManager->IsAuthenticated()) {
            SyncAllJobs();
        }

        std::mutex waitMutex;
        std::unique_lock<std::mutex> lock(waitMutex);
        m_syncCV.wait_for(lock, interval, [this]() { return !m_syncRunning; });
    }
}

bool GoogleSheetsManager::LoadSyncRecords()
{
    std::filesystem::path appDataPath = UFB::GetLocalAppDataPath();
    std::wstring recordsPath = (appDataPath / L"google_sheets_sync_records.json").wstring();

    std::ifstream file(recordsPath);
    if (!file.is_open()) {
        return false;
    }

    try {
        json j;
        file >> j;

        if (j.contains("masterSpreadsheetId")) {
            m_masterSpreadsheetId = j["masterSpreadsheetId"];
        }

        if (j.contains("syncRecords")) {
            for (const auto& record : j["syncRecords"]) {
                JobSyncRecord sr;
                sr.jobPath = Utf8ToWide(record["jobPath"]);
                sr.spreadsheetId = record["spreadsheetId"];
                sr.jobFolderId = record.value("jobFolderId", "");  // NEW: Load job folder ID
                sr.sheetId = record.value("sheetId", "");  // Legacy, optional
                sr.sheetTitle = record.value("sheetTitle", "");  // Legacy, optional
                sr.lastSyncTime = record["lastSyncTime"];
                sr.status = SheetSyncStatus::NotSynced;

                // NEW: Load sheetIds map (itemType -> sheetId)
                if (record.contains("sheetIds") && record["sheetIds"].is_object()) {
                    for (auto& [itemType, sheetId] : record["sheetIds"].items()) {
                        sr.sheetIds[itemType] = sheetId.get<std::string>();
                    }
                }

                m_syncRecords[sr.jobPath] = sr;
            }
        }

        std::cout << "[GoogleSheetsManager] Loaded sync records" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[GoogleSheetsManager] Failed to load sync records: " << e.what() << std::endl;
        return false;
    }
}

bool GoogleSheetsManager::SaveSyncRecords()
{
    std::filesystem::path appDataPath = UFB::GetLocalAppDataPath();
    std::wstring recordsPath = (appDataPath / L"google_sheets_sync_records.json").wstring();

    json j;
    j["masterSpreadsheetId"] = m_masterSpreadsheetId;

    json records = json::array();
    for (const auto& pair : m_syncRecords) {
        json record;
        record["jobPath"] = WideToUtf8(pair.second.jobPath);
        record["spreadsheetId"] = pair.second.spreadsheetId;
        record["jobFolderId"] = pair.second.jobFolderId;  // NEW: Save job folder ID
        record["sheetId"] = pair.second.sheetId;  // Legacy
        record["sheetTitle"] = pair.second.sheetTitle;  // Legacy
        record["lastSyncTime"] = pair.second.lastSyncTime;

        // NEW: Save sheetIds map (itemType -> sheetId)
        json sheetIdsMap = json::object();
        for (const auto& sheetPair : pair.second.sheetIds) {
            sheetIdsMap[sheetPair.first] = sheetPair.second;
        }
        record["sheetIds"] = sheetIdsMap;

        records.push_back(record);
    }
    j["syncRecords"] = records;

    std::ofstream file(recordsPath);
    if (!file.is_open()) {
        return false;
    }

    file << j.dump(2);
    return true;
}

bool GoogleSheetsManager::ApiGet(const std::string& endpoint, json& outResponse)
{
    std::cout << "[ApiGet] Called with endpoint: " << endpoint << std::endl;
    std::cout.flush();

    try {
        if (!m_authManager) {
            std::cerr << "[ApiGet] No auth manager" << std::endl;
            std::cerr.flush();
            return false;
        }

        std::cout << "[ApiGet] Getting access token..." << std::endl;
        std::cout.flush();
        std::string accessToken = m_authManager->GetAccessToken();
        if (accessToken.empty()) {
            std::cerr << "[ApiGet] Access token is empty" << std::endl;
            std::cerr.flush();
            return false;
        }

        std::cout << "[ApiGet] Parsing URL..." << std::endl;
        std::cout.flush();

        // Parse URL using WinHTTP functions
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        std::wstring wendpoint = converter.from_bytes(endpoint);

        std::cout << "[ApiGet] URL converted to wide string" << std::endl;
        std::cout.flush();

    URL_COMPONENTS urlComponents = {0};
    urlComponents.dwStructSize = sizeof(urlComponents);

    wchar_t hostname[256] = {0};
    wchar_t path[1024] = {0};

    urlComponents.lpszHostName = hostname;
    urlComponents.dwHostNameLength = sizeof(hostname) / sizeof(wchar_t);
    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = sizeof(path) / sizeof(wchar_t);

    if (!WinHttpCrackUrl(wendpoint.c_str(), 0, 0, &urlComponents)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"UFB/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession) {
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession,
                                       hostname,
                                       urlComponents.nPort,
                                       0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                           L"GET",
                                           path,
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring authHeader = converter.from_bytes("Authorization: Bearer " + accessToken);
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    bool success = WinHttpSendRequest(hRequest,
                                     WINHTTP_NO_ADDITIONAL_HEADERS,
                                     0,
                                     WINHTTP_NO_REQUEST_DATA,
                                     0,
                                     0,
                                     0);

    if (success) {
        success = WinHttpReceiveResponse(hRequest, nullptr);
    }

    if (success) {
        DWORD bytesAvailable = 0;
        std::string response;

        do {
            bytesAvailable = 0;
            if (WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                if (bytesAvailable > 0) {
                    std::vector<char> buffer(bytesAvailable + 1);
                    DWORD bytesRead = 0;

                    if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                        buffer[bytesRead] = 0;
                        response += buffer.data();
                    }
                }
            }
        } while (bytesAvailable > 0);

        try {
            outResponse = json::parse(response);
        }
        catch (const std::exception& e) {
            std::cerr << "[GoogleSheetsManager] Failed to parse response: " << e.what() << std::endl;
            success = false;
        }
    }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        return success;
    }
    catch (const std::exception& e) {
        std::cerr << "[ApiGet] Exception: " << e.what() << std::endl;
        std::cerr.flush();
        return false;
    }
    catch (...) {
        std::cerr << "[ApiGet] Unknown exception" << std::endl;
        std::cerr.flush();
        return false;
    }
}

bool GoogleSheetsManager::ApiPost(const std::string& endpoint, const json& requestBody, json& outResponse)
{
    std::cout << "[ApiPost] Called with endpoint: " << endpoint << std::endl;
    std::cout.flush();

    try {
        if (!m_authManager) {
            std::cerr << "[ApiPost] No auth manager" << std::endl;
            std::cerr.flush();
            return false;
        }

        std::cout << "[ApiPost] Getting access token..." << std::endl;
        std::cout.flush();
        std::string accessToken = m_authManager->GetAccessToken();
        if (accessToken.empty()) {
            std::cerr << "[ApiPost] Access token is empty" << std::endl;
            std::cerr.flush();
            return false;
        }

        std::cout << "[ApiPost] Dumping request body..." << std::endl;
        std::cout.flush();
        std::string postData = requestBody.dump();

        std::cout << "[ApiPost] Parsing URL..." << std::endl;
        std::cout.flush();

    // Parse URL using WinHTTP functions
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::wstring wendpoint = converter.from_bytes(endpoint);

    URL_COMPONENTS urlComponents = {0};
    urlComponents.dwStructSize = sizeof(urlComponents);

    wchar_t hostname[256] = {0};
    wchar_t path[1024] = {0};

    urlComponents.lpszHostName = hostname;
    urlComponents.dwHostNameLength = sizeof(hostname) / sizeof(wchar_t);
    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = sizeof(path) / sizeof(wchar_t);

    if (!WinHttpCrackUrl(wendpoint.c_str(), 0, 0, &urlComponents)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"UFB/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession) {
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession,
                                       hostname,
                                       urlComponents.nPort,
                                       0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                           L"POST",
                                           path,
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring authHeader = converter.from_bytes("Authorization: Bearer " + accessToken);
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json", -1, WINHTTP_ADDREQ_FLAG_ADD);

    bool success = WinHttpSendRequest(hRequest,
                                     WINHTTP_NO_ADDITIONAL_HEADERS,
                                     0,
                                     (LPVOID)postData.c_str(),
                                     (DWORD)postData.length(),
                                     (DWORD)postData.length(),
                                     0);

    if (success) {
        success = WinHttpReceiveResponse(hRequest, nullptr);
    }

    DWORD statusCode = 0;
    if (success) {
        // Check HTTP status code
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX,
                           &statusCode,
                           &statusCodeSize,
                           WINHTTP_NO_HEADER_INDEX);

        DWORD bytesAvailable = 0;
        std::string response;

        do {
            bytesAvailable = 0;
            if (WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                if (bytesAvailable > 0) {
                    std::vector<char> buffer(bytesAvailable + 1);
                    DWORD bytesRead = 0;

                    if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                        buffer[bytesRead] = 0;
                        response += buffer.data();
                    }
                }
            }
        } while (bytesAvailable > 0);

        try {
            outResponse = json::parse(response);

            // Check for rate limiting (HTTP 429)
            if (statusCode == 429) {
                std::cerr << "[ApiPost] Rate limit exceeded (HTTP 429)" << std::endl;
                if (outResponse.contains("error") && outResponse["error"].contains("message")) {
                    std::cerr << "[ERROR] " << outResponse["error"].dump(2) << std::endl;
                }
                success = false;
            }
            else if (statusCode >= 400) {
                std::cerr << "[ApiPost] HTTP error " << statusCode << std::endl;
                if (outResponse.contains("error")) {
                    std::cerr << "[ERROR] " << outResponse["error"].dump(2) << std::endl;
                }
                success = false;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[GoogleSheetsManager] Failed to parse response: " << e.what() << std::endl;
            success = false;
        }
    }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        // Retry logic for rate limiting (HTTP 429)
        static thread_local int retryCount = 0; // Thread-local to avoid interference
        if (!success && statusCode == 429) {
            const int maxRetries = 4;
            const int baseDelay = 5; // 5 seconds

            if (retryCount < maxRetries) {
                int delay = baseDelay * (1 << retryCount); // Exponential backoff: 5, 10, 20, 40
                retryCount++;
                std::cout << "[ApiPost] Rate limited. Retry " << retryCount << "/" << maxRetries
                         << " after " << delay << " seconds..." << std::endl;

                std::this_thread::sleep_for(std::chrono::seconds(delay));

                // Recursive retry
                return ApiPost(endpoint, requestBody, outResponse);
            } else {
                std::cerr << "[ApiPost] Max retries (" << maxRetries << ") exceeded for rate limiting" << std::endl;
                retryCount = 0; // Reset for next operation
                return false;
            }
        } else {
            // Reset retry count on success or non-429 errors
            retryCount = 0;
        }

        return success;
    }
    catch (const std::exception& e) {
        std::cerr << "[ApiPost] Exception: " << e.what() << std::endl;
        std::cerr.flush();
        return false;
    }
    catch (...) {
        std::cerr << "[ApiPost] Unknown exception" << std::endl;
        std::cerr.flush();
        return false;
    }
}

bool GoogleSheetsManager::ApiPut(const std::string& endpoint, const json& requestBody, json& outResponse)
{
    if (!m_authManager) {
        return false;
    }

    std::string accessToken = m_authManager->GetAccessToken();
    if (accessToken.empty()) {
        return false;
    }

    std::string postData = requestBody.dump();

    URL_COMPONENTSA urlComponents = {0};
    urlComponents.dwStructSize = sizeof(urlComponents);

    char hostname[256] = {0};
    char path[1024] = {0};

    urlComponents.lpszHostName = hostname;
    urlComponents.dwHostNameLength = sizeof(hostname);
    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = sizeof(path);

    if (!InternetCrackUrlA(endpoint.c_str(), 0, 0, &urlComponents)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"UFB/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession) {
        return false;
    }

    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::wstring whostname = converter.from_bytes(hostname);

    HINTERNET hConnect = WinHttpConnect(hSession,
                                       whostname.c_str(),
                                       urlComponents.nPort,
                                       0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring wpath = converter.from_bytes(path);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                           L"PUT",
                                           wpath.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring authHeader = converter.from_bytes("Authorization: Bearer " + accessToken);
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json", -1, WINHTTP_ADDREQ_FLAG_ADD);

    bool success = WinHttpSendRequest(hRequest,
                                     WINHTTP_NO_ADDITIONAL_HEADERS,
                                     0,
                                     (LPVOID)postData.c_str(),
                                     (DWORD)postData.length(),
                                     (DWORD)postData.length(),
                                     0);

    if (success) {
        success = WinHttpReceiveResponse(hRequest, nullptr);
    }

    if (success) {
        DWORD bytesAvailable = 0;
        std::string response;

        do {
            bytesAvailable = 0;
            if (WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                if (bytesAvailable > 0) {
                    std::vector<char> buffer(bytesAvailable + 1);
                    DWORD bytesRead = 0;

                    if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                        buffer[bytesRead] = 0;
                        response += buffer.data();
                    }
                }
            }
        } while (bytesAvailable > 0);

        try {
            outResponse = json::parse(response);
        }
        catch (const std::exception& e) {
            std::cerr << "[GoogleSheetsManager] Failed to parse response: " << e.what() << std::endl;
            success = false;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return success;
}

bool GoogleSheetsManager::ApiPatch(const std::string& endpoint, const json& requestBody, json& outResponse)
{
    if (!m_authManager) {
        return false;
    }

    std::string accessToken = m_authManager->GetAccessToken();
    if (accessToken.empty()) {
        return false;
    }

    std::string postData = requestBody.dump();

    // Parse URL using WinHTTP functions
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::wstring wendpoint = converter.from_bytes(endpoint);

    URL_COMPONENTS urlComponents = {0};
    urlComponents.dwStructSize = sizeof(urlComponents);

    wchar_t hostname[256] = {0};
    wchar_t path[1024] = {0};

    urlComponents.lpszHostName = hostname;
    urlComponents.dwHostNameLength = sizeof(hostname) / sizeof(wchar_t);
    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = sizeof(path) / sizeof(wchar_t);

    if (!WinHttpCrackUrl(wendpoint.c_str(), 0, 0, &urlComponents)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"UFB/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession) {
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession,
                                       hostname,
                                       urlComponents.nPort,
                                       0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                           L"PATCH",
                                           path,
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring authHeader = converter.from_bytes("Authorization: Bearer " + accessToken);
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json", -1, WINHTTP_ADDREQ_FLAG_ADD);

    bool success = WinHttpSendRequest(hRequest,
                                     WINHTTP_NO_ADDITIONAL_HEADERS,
                                     0,
                                     (LPVOID)postData.c_str(),
                                     (DWORD)postData.length(),
                                     (DWORD)postData.length(),
                                     0);

    if (success) {
        success = WinHttpReceiveResponse(hRequest, nullptr);
    }

    if (success) {
        DWORD bytesAvailable = 0;
        std::string response;

        do {
            bytesAvailable = 0;
            if (WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                if (bytesAvailable > 0) {
                    std::vector<char> buffer(bytesAvailable + 1);
                    DWORD bytesRead = 0;

                    if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                        buffer[bytesRead] = 0;
                        response += buffer.data();
                    }
                }
            }
        } while (bytesAvailable > 0);

        try {
            outResponse = json::parse(response);
            success = true;
        }
        catch (const std::exception& e) {
            std::cerr << "[GoogleSheetsManager] Failed to parse response: " << e.what() << std::endl;
            success = false;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return success;
}

bool GoogleSheetsManager::ApiDelete(const std::string& endpoint, json& outResponse)
{
    return false;
}

bool GoogleSheetsManager::MoveToFolder(const std::string& fileId, const std::string& parentFolderId)
{
    std::cout << "[GoogleSheetsManager] MoveToFolder called - fileId: " << fileId
              << ", parentFolderId: " << parentFolderId << std::endl;

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] MoveToFolder failed - not authenticated" << std::endl;
        return false;
    }

    // STEP 1: Get current parents of the file
    std::string getEndpoint = "https://www.googleapis.com/drive/v3/files/" + fileId + "?fields=parents&supportsAllDrives=true";
    json getResponse;

    std::cout << "[GoogleSheetsManager] Getting current parents..." << std::endl;
    if (!ApiGet(getEndpoint, getResponse)) {
        std::cerr << "[GoogleSheetsManager] ✗ Failed to get current parents" << std::endl;
        return false;
    }

    // Extract current parents
    std::string currentParents = "";
    if (getResponse.contains("parents") && getResponse["parents"].is_array()) {
        for (size_t i = 0; i < getResponse["parents"].size(); i++) {
            if (i > 0) currentParents += ",";
            currentParents += getResponse["parents"][i].get<std::string>();
        }
        std::cout << "[GoogleSheetsManager] Current parents: " << currentParents << std::endl;
    } else {
        std::cout << "[GoogleSheetsManager] No existing parents found" << std::endl;
    }

    // STEP 2: Move file by adding new parent and removing old parents
    // Use Drive API v3 to MOVE the file (not just add parent)
    // supportsAllDrives=true is required for Shared Drive support
    std::string endpoint = "https://www.googleapis.com/drive/v3/files/" + fileId
                         + "?addParents=" + parentFolderId
                         + (currentParents.empty() ? "" : "&removeParents=" + currentParents)
                         + "&supportsAllDrives=true";
    std::cout << "[GoogleSheetsManager] MoveToFolder endpoint: " << endpoint << std::endl;

    json requestBody = json::object();
    json response;

    std::cout << "[GoogleSheetsManager] Calling ApiPatch to move file..." << std::endl;
    if (!ApiPatch(endpoint, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] ✗ Failed to move file to folder" << std::endl;
        if (response.contains("error")) {
            std::cerr << "[GoogleSheetsManager] ✗ API Error: " << response["error"].dump(2) << std::endl;
        }
        return false;
    }

    // Check for errors in response even if ApiPatch returned true
    if (response.contains("error")) {
        std::cerr << "[GoogleSheetsManager] ✗ MoveToFolder API returned error: " << response["error"].dump(2) << std::endl;
        return false;
    }

    std::cout << "[GoogleSheetsManager] ✓ MoveToFolder succeeded - file now in folder " << parentFolderId << std::endl;
    return true;
}

std::string GoogleSheetsManager::CreateFolder(const std::string& title, const std::string& parentFolderId)
{
    std::cout << "[GoogleSheetsManager] CreateFolder: " << title << " in parent: " << parentFolderId << std::endl;

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] CreateFolder failed - not authenticated" << std::endl;
        return "";
    }

    if (parentFolderId.empty()) {
        std::cerr << "[GoogleSheetsManager] CreateFolder failed - parentFolderId is empty" << std::endl;
        return "";
    }

    // Build request to create folder
    json requestBody;
    requestBody["name"] = title;
    requestBody["mimeType"] = "application/vnd.google-apps.folder";
    requestBody["parents"] = json::array({parentFolderId});

    // Use Drive API v3 with supportsAllDrives=true for Shared Drive support
    std::string endpoint = "https://www.googleapis.com/drive/v3/files?supportsAllDrives=true";
    std::cout << "[GoogleSheetsManager] CreateFolder endpoint: " << endpoint << std::endl;
    json response;

    if (!ApiPost(endpoint, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] ✗ Failed to create folder" << std::endl;
        if (response.contains("error")) {
            std::cerr << "[GoogleSheetsManager] ✗ API Error: " << response["error"].dump(2) << std::endl;
        }
        return "";
    }

    // Check for errors in response
    if (response.contains("error")) {
        std::cerr << "[GoogleSheetsManager] ✗ CreateFolder API returned error: " << response["error"].dump(2) << std::endl;
        return "";
    }

    if (response.contains("id")) {
        std::string folderId = response["id"];
        std::cout << "[GoogleSheetsManager] ✓ Created folder: " << title << " (ID: " << folderId << ")" << std::endl;
        return folderId;
    }

    std::cerr << "[GoogleSheetsManager] ✗ Folder created but no ID in response" << std::endl;
    return "";
}

bool GoogleSheetsManager::IsFolderTrashed(const std::string& folderId)
{
    if (folderId.empty()) {
        return true;  // Treat empty folder ID as trashed
    }

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] IsFolderTrashed failed - not authenticated" << std::endl;
        return true;  // Fail-safe: treat as trashed if can't verify
    }

    // Query Drive API to check if folder is trashed
    std::string endpoint = "https://www.googleapis.com/drive/v3/files/" + folderId + "?fields=trashed&supportsAllDrives=true";
    json response;

    if (!ApiGet(endpoint, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to check if folder is trashed (ID: " << folderId << ")" << std::endl;
        return true;  // Fail-safe: treat as trashed if API call fails
    }

    if (response.contains("trashed") && response["trashed"].is_boolean()) {
        bool isTrashed = response["trashed"].get<bool>();
        if (isTrashed) {
            std::cout << "[GoogleSheetsManager] Folder is trashed (ID: " << folderId << ")" << std::endl;
        }
        return isTrashed;
    }

    // If no "trashed" field, assume not trashed
    return false;
}

std::string GoogleSheetsManager::FindFolderByName(const std::string& folderName, const std::string& parentFolderId)
{
    std::cout << "[GoogleSheetsManager] FindFolderByName: " << folderName << " in parent: " << parentFolderId << std::endl;

    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] FindFolderByName failed - not authenticated" << std::endl;
        return "";
    }

    if (parentFolderId.empty()) {
        std::cerr << "[GoogleSheetsManager] FindFolderByName failed - parentFolderId is empty" << std::endl;
        return "";
    }

    // Build query to search for folder
    // Query: name='folderName' and 'parentId' in parents and mimeType='application/vnd.google-apps.folder' and trashed=false
    std::string query = "name='" + folderName + "' and '" + parentFolderId + "' in parents and mimeType='application/vnd.google-apps.folder' and trashed=false";

    // URL encode the query
    std::string encodedQuery;
    for (char c : query) {
        if (c == ' ') {
            encodedQuery += "%20";
        } else if (c == '\'') {
            encodedQuery += "%27";
        } else if (c == '=') {
            encodedQuery += "%3D";
        } else {
            encodedQuery += c;
        }
    }

    // Use Drive API v3 with supportsAllDrives=true for Shared Drive support
    std::string endpoint = "https://www.googleapis.com/drive/v3/files?q=" + encodedQuery + "&supportsAllDrives=true&includeItemsFromAllDrives=true";
    json response;

    if (!ApiGet(endpoint, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to search for folder" << std::endl;
        return "";
    }

    if (response.contains("files") && response["files"].is_array() && !response["files"].empty()) {
        std::string folderId = response["files"][0]["id"];
        std::cout << "[GoogleSheetsManager] Found existing folder: " << folderName << " (ID: " << folderId << ")" << std::endl;
        return folderId;
    }

    std::cout << "[GoogleSheetsManager] Folder not found: " << folderName << std::endl;
    return "";
}

std::string GoogleSheetsManager::GetOrCreateJobFolder(const std::string& jobName, const std::string& parentFolderId)
{
    std::cout << "[GoogleSheetsManager] GetOrCreateJobFolder: '" << jobName << "'" << std::endl;
    std::cout << "[GoogleSheetsManager] Using parent folder ID: " << parentFolderId << std::endl;

    if (parentFolderId.empty()) {
        std::cerr << "[GoogleSheetsManager] ERROR: GetOrCreateJobFolder failed - parentFolderId is required" << std::endl;
        return "";
    }

    // First, try to find existing folder
    std::cout << "[GoogleSheetsManager] Searching for existing job folder..." << std::endl;
    std::string folderId = FindFolderByName(jobName, parentFolderId);
    if (!folderId.empty()) {
        std::cout << "[GoogleSheetsManager] ✓ Found existing job folder (ID: " << folderId << ")" << std::endl;
        return folderId;
    }

    // If not found, create new folder
    std::cout << "[GoogleSheetsManager] No existing folder found, creating new job folder..." << std::endl;
    folderId = CreateFolder(jobName, parentFolderId);
    if (folderId.empty()) {
        std::cerr << "[GoogleSheetsManager] ERROR: Failed to create job folder: " << jobName << std::endl;
        return "";
    }

    std::cout << "[GoogleSheetsManager] ✓ Created new job folder (ID: " << folderId << ")" << std::endl;
    return folderId;
}

std::string GoogleSheetsManager::BuildSpreadsheetsUrl(const std::string& spreadsheetId) const
{
    std::string base = "https://sheets.googleapis.com/v4/spreadsheets";
    if (spreadsheetId.empty()) {
        return base;
    }
    return base + "/" + spreadsheetId;
}

std::string GoogleSheetsManager::BuildValuesUrl(const std::string& spreadsheetId, const std::string& range) const
{
    return BuildSpreadsheetsUrl(spreadsheetId) + "/values/" + range;
}

std::string GoogleSheetsManager::BuildBatchUpdateUrl(const std::string& spreadsheetId) const
{
    return BuildSpreadsheetsUrl(spreadsheetId) + ":batchUpdate";
}

std::vector<SheetRow> GoogleSheetsManager::ConvertJobToSheetRows(const std::wstring& jobPath)
{
    std::vector<SheetRow> rows;

    if (!m_subscriptionManager) {
        return rows;
    }

    auto shots = m_subscriptionManager->GetAllTrackedItems(jobPath);

    for (const auto& shot : shots) {
        SheetRow row;

        // Column A: Shot Path
        row.cells.push_back(WideToUtf8(shot.shotPath));

        // Column B: Item Type
        row.cells.push_back(shot.itemType.empty() ? "" : shot.itemType);

        // Column C: Folder Type
        row.cells.push_back(shot.folderType.empty() ? "" : shot.folderType);

        // Column D: Status
        row.cells.push_back(shot.status.empty() ? "" : shot.status);

        // Column E: Category
        row.cells.push_back(shot.category.empty() ? "" : shot.category);

        // Column F: Priority (convert to HIGH/MEDIUM/LOW)
        std::string priorityStr = "";
        if (shot.priority == 1) {
            priorityStr = "HIGH";
        } else if (shot.priority == 2) {
            priorityStr = "MEDIUM";
        } else if (shot.priority == 3) {
            priorityStr = "LOW";
        }
        row.cells.push_back(priorityStr);

        // Column G: Due Date (format as readable date if set)
        std::string dueDateStr = "";
        if (shot.dueDate > 0) {
            time_t dueDateTime = shot.dueDate / 1000;  // Convert ms to seconds
            std::tm tm = {};
            if (localtime_s(&tm, &dueDateTime) == 0) {
                char buffer[20];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
                dueDateStr = buffer;
            }
        }
        row.cells.push_back(dueDateStr);

        // Column H: Artist
        row.cells.push_back(shot.artist.empty() ? "" : shot.artist);

        // Column I: Note
        row.cells.push_back(shot.note.empty() ? "" : shot.note);

        // Column J: Links
        row.cells.push_back(shot.links.empty() ? "" : shot.links);

        // Column K: Last Modified (human-readable format)
        std::string modifiedStr = "";
        if (shot.modifiedTime > 0) {
            time_t modifiedDateTime = shot.modifiedTime / 1000;  // Convert ms to seconds
            std::tm tm = {};
            if (localtime_s(&tm, &modifiedDateTime) == 0) {
                char buffer[32];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d %I:%M %p", &tm);
                modifiedStr = buffer;
            }
        }
        row.cells.push_back(modifiedStr);

        rows.push_back(row);
    }

    return rows;
}

std::vector<SheetRow> GoogleSheetsManager::ConvertJobToSheetRows(const std::wstring& jobPath, const std::string& itemType)
{
    std::vector<SheetRow> rows;

    if (!m_subscriptionManager) {
        return rows;
    }

    // Get all tracked items for this job
    auto allItems = m_subscriptionManager->GetAllTrackedItems(jobPath);

    // Filter by itemType
    for (const auto& item : allItems) {
        // Skip items that don't match the requested type
        if (item.itemType != itemType) {
            continue;
        }

        SheetRow row;

        // Column A: Shot Path (or item path for assets/postings/tasks)
        row.cells.push_back(WideToUtf8(item.shotPath));

        // Column B: Item Type
        row.cells.push_back(item.itemType.empty() ? "" : item.itemType);

        // Column C: Folder Type
        row.cells.push_back(item.folderType.empty() ? "" : item.folderType);

        // Column D: Status
        row.cells.push_back(item.status.empty() ? "" : item.status);

        // Column E: Category
        row.cells.push_back(item.category.empty() ? "" : item.category);

        // Column F: Priority (convert to HIGH/MEDIUM/LOW)
        std::string priorityStr = "";
        if (item.priority == 1) {
            priorityStr = "HIGH";
        } else if (item.priority == 2) {
            priorityStr = "MEDIUM";
        } else if (item.priority == 3) {
            priorityStr = "LOW";
        }
        row.cells.push_back(priorityStr);

        // Column G: Due Date (format as readable date if set)
        std::string dueDateStr = "";
        if (item.dueDate > 0) {
            time_t dueDateTime = item.dueDate / 1000;  // Convert ms to seconds
            std::tm tm = {};
            if (localtime_s(&tm, &dueDateTime) == 0) {
                char buffer[20];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
                dueDateStr = buffer;
            }
        }
        row.cells.push_back(dueDateStr);

        // Column H: Artist
        row.cells.push_back(item.artist.empty() ? "" : item.artist);

        // Column I: Note
        row.cells.push_back(item.note.empty() ? "" : item.note);

        // Column J: Links
        row.cells.push_back(item.links.empty() ? "" : item.links);

        // Column K: Last Modified (human-readable format)
        std::string modifiedStr = "";
        if (item.modifiedTime > 0) {
            time_t modifiedDateTime = item.modifiedTime / 1000;  // Convert ms to seconds
            std::tm tm = {};
            if (localtime_s(&tm, &modifiedDateTime) == 0) {
                char buffer[32];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d %I:%M %p", &tm);
                modifiedStr = buffer;
            }
        }
        row.cells.push_back(modifiedStr);

        rows.push_back(row);
    }

    std::cout << "[GoogleSheetsManager] ConvertJobToSheetRows: Found " << rows.size()
              << " items of type '" << itemType << "' for job" << std::endl;

    return rows;
}

int GoogleSheetsManager::FindJobRowIndex(const std::string& spreadsheetId,
                                        const std::string& sheetTitle,
                                        const std::wstring& jobPath)
{
    // This function is deprecated - returning 1 to indicate data starts at row 2
    // Individual shot row finding is now handled in SyncJob
    return 1;
}

int GoogleSheetsManager::FindShotRowIndex(const std::vector<SheetRow>& sheetData,
                                         const std::wstring& shotPath)
{
    std::string shotPathUtf8 = WideToUtf8(shotPath);

    // Start from row 1 (skip header row at index 0)
    for (size_t i = 1; i < sheetData.size(); ++i) {
        if (!sheetData[i].cells.empty()) {
            // Shot path is in column A (index 0)
            if (sheetData[i].cells[0] == shotPathUtf8) {
                return (int)i;
            }
        }
    }

    return -1;  // Not found
}

std::vector<std::string> GoogleSheetsManager::GetAllStatusOptions()
{
    std::set<std::string> uniqueStatuses;

    if (!m_subscriptionManager) {
        // Default fallback options
        return {"Not Started", "In Progress", "For Review", "Complete"};
    }

    // Get all subscribed jobs
    auto subscriptions = m_subscriptionManager->GetAllSubscriptions();

    // For each job, load its ProjectConfig and collect status options
    for (const auto& sub : subscriptions) {
        UFB::ProjectConfig config;
        if (config.LoadProjectConfig(sub.jobPath)) {
            auto folderTypes = config.GetAllFolderTypes();
            for (const auto& folderType : folderTypes) {
                auto statusOptions = config.GetStatusOptions(folderType);
                for (const auto& status : statusOptions) {
                    uniqueStatuses.insert(status.name);
                }
            }
        }
    }

    // If no options found, use defaults
    if (uniqueStatuses.empty()) {
        return {"Not Started", "In Progress", "For Review", "Complete"};
    }

    return std::vector<std::string>(uniqueStatuses.begin(), uniqueStatuses.end());
}

std::vector<std::string> GoogleSheetsManager::GetAllCategoryOptions()
{
    std::set<std::string> uniqueCategories;

    if (!m_subscriptionManager) {
        // Default fallback options
        return {"Offline", "Online", "On Hold", "Killed"};
    }

    // Get all subscribed jobs
    auto subscriptions = m_subscriptionManager->GetAllSubscriptions();

    // For each job, load its ProjectConfig and collect category options
    for (const auto& sub : subscriptions) {
        UFB::ProjectConfig config;
        if (config.LoadProjectConfig(sub.jobPath)) {
            auto folderTypes = config.GetAllFolderTypes();
            for (const auto& folderType : folderTypes) {
                auto categoryOptions = config.GetCategoryOptions(folderType);
                for (const auto& category : categoryOptions) {
                    uniqueCategories.insert(category.name);
                }
            }
        }
    }

    // If no options found, use defaults
    if (uniqueCategories.empty()) {
        return {"Offline", "Online", "On Hold", "Killed"};
    }

    return std::vector<std::string>(uniqueCategories.begin(), uniqueCategories.end());
}

std::vector<StatusOption> GoogleSheetsManager::GetStatusOptionsForItemType(const std::wstring& jobPath, const std::string& itemType)
{
    std::vector<StatusOption> options;

    // Load per-job ProjectConfig
    UFB::ProjectConfig config;
    if (!config.LoadProjectConfig(jobPath)) {
        std::cerr << "[GoogleSheetsManager] Failed to load project config for job: " << WideToUtf8(jobPath) << std::endl;
        // Return defaults
        return {{"Not Started", "#94A3B8"}, {"In Progress", "#3B82F6"}, {"For Review", "#F59E0B"}, {"Complete", "#10B981"}};
    }

    // Get all folder types for this job
    auto folderTypes = config.GetAllFolderTypes();

    // Filter by itemType
    for (const auto& folderType : folderTypes) {
        auto folderConfig = config.GetFolderTypeConfig(folderType);
        if (!folderConfig.has_value()) {
            continue;
        }

        bool matchesItemType = false;
        if (itemType == "shot" && folderConfig->isShot) {
            matchesItemType = true;
        } else if (itemType == "asset" && folderConfig->isAsset) {
            matchesItemType = true;
        } else if (itemType == "posting" && folderConfig->isPosting) {
            matchesItemType = true;
        } else if (itemType == "manual_task") {
            // Tasks don't have specific folder types, use all options
            matchesItemType = true;
        }

        if (matchesItemType) {
            auto statusOptions = config.GetStatusOptions(folderType);
            for (const auto& status : statusOptions) {
                // Check if not already added
                bool exists = false;
                for (const auto& existing : options) {
                    if (existing.name == status.name) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    options.push_back(status);
                }
            }
        }
    }

    // If no options found, use defaults
    if (options.empty()) {
        return {{"Not Started", "#94A3B8"}, {"In Progress", "#3B82F6"}, {"For Review", "#F59E0B"}, {"Complete", "#10B981"}};
    }

    return options;
}

std::vector<CategoryOption> GoogleSheetsManager::GetCategoryOptionsForItemType(const std::wstring& jobPath, const std::string& itemType)
{
    std::vector<CategoryOption> options;

    // Load per-job ProjectConfig
    UFB::ProjectConfig config;
    if (!config.LoadProjectConfig(jobPath)) {
        std::cerr << "[GoogleSheetsManager] Failed to load project config for job: " << WideToUtf8(jobPath) << std::endl;
        // Return defaults
        return {{"Offline", "#8B5CF6"}, {"Online", "#EC4899"}, {"On Hold", "#F59E0B"}, {"Killed", "#EF4444"}};
    }

    // Get all folder types for this job
    auto folderTypes = config.GetAllFolderTypes();

    // Filter by itemType
    for (const auto& folderType : folderTypes) {
        auto folderConfig = config.GetFolderTypeConfig(folderType);
        if (!folderConfig.has_value()) {
            continue;
        }

        bool matchesItemType = false;
        if (itemType == "shot" && folderConfig->isShot) {
            matchesItemType = true;
        } else if (itemType == "asset" && folderConfig->isAsset) {
            matchesItemType = true;
        } else if (itemType == "posting" && folderConfig->isPosting) {
            matchesItemType = true;
        } else if (itemType == "manual_task") {
            // Tasks don't have specific folder types, use all options
            matchesItemType = true;
        }

        if (matchesItemType) {
            auto categoryOptions = config.GetCategoryOptions(folderType);
            for (const auto& category : categoryOptions) {
                // Check if not already added
                bool exists = false;
                for (const auto& existing : options) {
                    if (existing.name == category.name) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    options.push_back(category);
                }
            }
        }
    }

    // If no options found, use defaults
    if (options.empty()) {
        return {{"Offline", "#8B5CF6"}, {"Online", "#EC4899"}, {"On Hold", "#F59E0B"}, {"Killed", "#EF4444"}};
    }

    return options;
}

bool GoogleSheetsManager::SetColumnDataValidation(const std::string& spreadsheetId,
                                                  const std::string& sheetId,
                                                  int columnIndex,
                                                  const std::vector<std::string>& options)
{
    if (options.empty()) {
        return true;  // Nothing to validate
    }

    // Build setDataValidation request
    json request;
    request["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
    request["setDataValidation"]["range"]["startRowIndex"] = 1;  // Skip header row
    request["setDataValidation"]["range"]["startColumnIndex"] = columnIndex;
    request["setDataValidation"]["range"]["endColumnIndex"] = columnIndex + 1;

    // Set validation rule with dropdown
    request["setDataValidation"]["rule"]["condition"]["type"] = "ONE_OF_LIST";
    request["setDataValidation"]["rule"]["condition"]["values"] = json::array();
    for (const auto& option : options) {
        json value;
        value["userEnteredValue"] = option;
        request["setDataValidation"]["rule"]["condition"]["values"].push_back(value);
    }
    request["setDataValidation"]["rule"]["showCustomUi"] = true;  // Show dropdown arrow
    request["setDataValidation"]["rule"]["strict"] = false;  // Allow custom values

    // Send batchUpdate request
    json requestBody;
    requestBody["requests"] = json::array({request});

    json response;
    std::string url = BuildBatchUpdateUrl(spreadsheetId);
    return ApiPost(url, requestBody, response);
}

// Helper function to convert hex color to RGB (0.0-1.0)
static void HexToRgb(const std::string& hex, float& r, float& g, float& b)
{
    // Remove '#' if present
    std::string colorStr = hex;
    if (!colorStr.empty() && colorStr[0] == '#') {
        colorStr = colorStr.substr(1);
    }

    // Parse hex values
    if (colorStr.length() == 6) {
        unsigned int hexValue = std::stoul(colorStr, nullptr, 16);
        r = ((hexValue >> 16) & 0xFF) / 255.0f;
        g = ((hexValue >> 8) & 0xFF) / 255.0f;
        b = (hexValue & 0xFF) / 255.0f;
    } else {
        // Default to gray if invalid
        r = g = b = 0.5f;
    }
}

bool GoogleSheetsManager::SetColumnDataValidationWithColors(const std::string& spreadsheetId,
                                                             const std::string& sheetId,
                                                             int columnIndex,
                                                             const std::vector<StatusOption>& options)
{
    if (options.empty()) {
        return true;  // Nothing to validate
    }

    json requests = json::array();

    // 1. Add data validation dropdown
    json validationRequest;
    validationRequest["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
    validationRequest["setDataValidation"]["range"]["startRowIndex"] = 1;  // Skip header row
    validationRequest["setDataValidation"]["range"]["startColumnIndex"] = columnIndex;
    validationRequest["setDataValidation"]["range"]["endColumnIndex"] = columnIndex + 1;

    validationRequest["setDataValidation"]["rule"]["condition"]["type"] = "ONE_OF_LIST";
    validationRequest["setDataValidation"]["rule"]["condition"]["values"] = json::array();
    for (const auto& option : options) {
        json value;
        value["userEnteredValue"] = option.name;
        validationRequest["setDataValidation"]["rule"]["condition"]["values"].push_back(value);
    }
    validationRequest["setDataValidation"]["rule"]["showCustomUi"] = true;
    validationRequest["setDataValidation"]["rule"]["strict"] = false;

    requests.push_back(validationRequest);

    // 2. Add conditional formatting rules for each status option
    for (const auto& option : options) {
        json formatRequest;

        // Condition: when cell text equals option.name
        formatRequest["addConditionalFormatRule"]["rule"]["ranges"] = json::array();
        json range;
        range["sheetId"] = std::stoi(sheetId);
        range["startRowIndex"] = 1;  // Skip header
        range["startColumnIndex"] = columnIndex;
        range["endColumnIndex"] = columnIndex + 1;
        formatRequest["addConditionalFormatRule"]["rule"]["ranges"].push_back(range);

        // Boolean condition: TEXT_EQ
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["type"] = "TEXT_EQ";
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"] = json::array();
        json condValue;
        condValue["userEnteredValue"] = option.name;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"].push_back(condValue);

        // Format: background color from option.color
        float r, g, b;
        HexToRgb(option.color, r, g, b);
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["red"] = r;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["green"] = g;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["blue"] = b;

        // Add white text for better contrast
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["red"] = 1.0;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["green"] = 1.0;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["blue"] = 1.0;

        requests.push_back(formatRequest);
    }

    // Send batchUpdate request
    json requestBody;
    requestBody["requests"] = requests;

    json response;
    std::string url = BuildBatchUpdateUrl(spreadsheetId);
    return ApiPost(url, requestBody, response);
}

bool GoogleSheetsManager::SetColumnDataValidationWithColors(const std::string& spreadsheetId,
                                                             const std::string& sheetId,
                                                             int columnIndex,
                                                             const std::vector<CategoryOption>& options)
{
    if (options.empty()) {
        return true;  // Nothing to validate
    }

    json requests = json::array();

    // 1. Add data validation dropdown
    json validationRequest;
    validationRequest["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
    validationRequest["setDataValidation"]["range"]["startRowIndex"] = 1;  // Skip header row
    validationRequest["setDataValidation"]["range"]["startColumnIndex"] = columnIndex;
    validationRequest["setDataValidation"]["range"]["endColumnIndex"] = columnIndex + 1;

    validationRequest["setDataValidation"]["rule"]["condition"]["type"] = "ONE_OF_LIST";
    validationRequest["setDataValidation"]["rule"]["condition"]["values"] = json::array();
    for (const auto& option : options) {
        json value;
        value["userEnteredValue"] = option.name;
        validationRequest["setDataValidation"]["rule"]["condition"]["values"].push_back(value);
    }
    validationRequest["setDataValidation"]["rule"]["showCustomUi"] = true;
    validationRequest["setDataValidation"]["rule"]["strict"] = false;

    requests.push_back(validationRequest);

    // 2. Add conditional formatting rules for each category option
    for (const auto& option : options) {
        json formatRequest;

        // Condition: when cell text equals option.name
        formatRequest["addConditionalFormatRule"]["rule"]["ranges"] = json::array();
        json range;
        range["sheetId"] = std::stoi(sheetId);
        range["startRowIndex"] = 1;  // Skip header
        range["startColumnIndex"] = columnIndex;
        range["endColumnIndex"] = columnIndex + 1;
        formatRequest["addConditionalFormatRule"]["rule"]["ranges"].push_back(range);

        // Boolean condition: TEXT_EQ
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["type"] = "TEXT_EQ";
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"] = json::array();
        json condValue;
        condValue["userEnteredValue"] = option.name;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"].push_back(condValue);

        // Format: background color from option.color
        float r, g, b;
        HexToRgb(option.color, r, g, b);
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["red"] = r;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["green"] = g;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["blue"] = b;

        // Add white text for better contrast
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["red"] = 1.0;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["green"] = 1.0;
        formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["blue"] = 1.0;

        requests.push_back(formatRequest);
    }

    // Send batchUpdate request
    json requestBody;
    requestBody["requests"] = requests;

    json response;
    std::string url = BuildBatchUpdateUrl(spreadsheetId);
    return ApiPost(url, requestBody, response);
}

bool GoogleSheetsManager::FormatLinksAsHyperlinks(const std::string& spreadsheetId,
                                                  const std::string& sheetId)
{
    // Set text color to blue and underline for links column (column J, index 9)
    json request;
    request["repeatCell"]["range"]["sheetId"] = std::stoi(sheetId);
    request["repeatCell"]["range"]["startRowIndex"] = 1;  // Skip header
    request["repeatCell"]["range"]["startColumnIndex"] = 9;  // Column J
    request["repeatCell"]["range"]["endColumnIndex"] = 10;

    request["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["blue"] = 1.0;
    request["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["red"] = 0.0;
    request["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["green"] = 0.0;
    request["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["underline"] = true;

    request["repeatCell"]["fields"] = "userEnteredFormat.textFormat";

    // Send batchUpdate request
    json requestBody;
    requestBody["requests"] = json::array({request});

    json response;
    std::string url = BuildBatchUpdateUrl(spreadsheetId);
    return ApiPost(url, requestBody, response);
}

bool GoogleSheetsManager::SetupSheetFormatting(const std::string& spreadsheetId,
                                               const std::string& sheetId,
                                               const std::wstring& jobPath,
                                               const std::string& itemType)
{
    std::cout << "[GoogleSheetsManager] Setting up sheet formatting for itemType: " << itemType << std::endl;

    // Build ALL formatting requests in ONE array (reduces 4 API calls to 1)
    json allRequests = json::array();

    // ===== COLUMN D: Status dropdown with colors (index 3) =====
    auto statusOptions = GetStatusOptionsForItemType(jobPath, itemType);
    if (!statusOptions.empty()) {
        // Status validation request
        json statusValidationRequest;
        statusValidationRequest["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
        statusValidationRequest["setDataValidation"]["range"]["startRowIndex"] = 1;
        statusValidationRequest["setDataValidation"]["range"]["startColumnIndex"] = 3;
        statusValidationRequest["setDataValidation"]["range"]["endColumnIndex"] = 4;
        statusValidationRequest["setDataValidation"]["rule"]["condition"]["type"] = "ONE_OF_LIST";
        statusValidationRequest["setDataValidation"]["rule"]["condition"]["values"] = json::array();
        for (const auto& option : statusOptions) {
            json value;
            value["userEnteredValue"] = option.name;
            statusValidationRequest["setDataValidation"]["rule"]["condition"]["values"].push_back(value);
        }
        statusValidationRequest["setDataValidation"]["rule"]["showCustomUi"] = true;
        statusValidationRequest["setDataValidation"]["rule"]["strict"] = false;
        allRequests.push_back(statusValidationRequest);

        // Status conditional formatting (one rule per option)
        for (const auto& option : statusOptions) {
            json formatRequest;
            formatRequest["addConditionalFormatRule"]["rule"]["ranges"] = json::array();
            json range;
            range["sheetId"] = std::stoi(sheetId);
            range["startRowIndex"] = 1;
            range["startColumnIndex"] = 3;
            range["endColumnIndex"] = 4;
            formatRequest["addConditionalFormatRule"]["rule"]["ranges"].push_back(range);
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["type"] = "TEXT_EQ";
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"] = json::array();
            json condValue;
            condValue["userEnteredValue"] = option.name;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"].push_back(condValue);
            float r, g, b;
            HexToRgb(option.color, r, g, b);
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["red"] = r;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["green"] = g;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["blue"] = b;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["red"] = 1.0;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["green"] = 1.0;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["blue"] = 1.0;
            allRequests.push_back(formatRequest);
        }
    }

    // ===== COLUMN E: Category dropdown with colors (index 4) =====
    auto categoryOptions = GetCategoryOptionsForItemType(jobPath, itemType);
    if (!categoryOptions.empty()) {
        // Category validation request
        json categoryValidationRequest;
        categoryValidationRequest["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
        categoryValidationRequest["setDataValidation"]["range"]["startRowIndex"] = 1;
        categoryValidationRequest["setDataValidation"]["range"]["startColumnIndex"] = 4;
        categoryValidationRequest["setDataValidation"]["range"]["endColumnIndex"] = 5;
        categoryValidationRequest["setDataValidation"]["rule"]["condition"]["type"] = "ONE_OF_LIST";
        categoryValidationRequest["setDataValidation"]["rule"]["condition"]["values"] = json::array();
        for (const auto& option : categoryOptions) {
            json value;
            value["userEnteredValue"] = option.name;
            categoryValidationRequest["setDataValidation"]["rule"]["condition"]["values"].push_back(value);
        }
        categoryValidationRequest["setDataValidation"]["rule"]["showCustomUi"] = true;
        categoryValidationRequest["setDataValidation"]["rule"]["strict"] = false;
        allRequests.push_back(categoryValidationRequest);

        // Category conditional formatting (one rule per option)
        for (const auto& option : categoryOptions) {
            json formatRequest;
            formatRequest["addConditionalFormatRule"]["rule"]["ranges"] = json::array();
            json range;
            range["sheetId"] = std::stoi(sheetId);
            range["startRowIndex"] = 1;
            range["startColumnIndex"] = 4;
            range["endColumnIndex"] = 5;
            formatRequest["addConditionalFormatRule"]["rule"]["ranges"].push_back(range);
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["type"] = "TEXT_EQ";
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"] = json::array();
            json condValue;
            condValue["userEnteredValue"] = option.name;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"].push_back(condValue);
            float r, g, b;
            HexToRgb(option.color, r, g, b);
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["red"] = r;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["green"] = g;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["blue"] = b;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["red"] = 1.0;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["green"] = 1.0;
            formatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["blue"] = 1.0;
            allRequests.push_back(formatRequest);
        }
    }

    // ===== COLUMN F: Priority dropdown with colors (index 5) =====
    // HIGH=Red, MEDIUM=Yellow, LOW=Grey

    // Priority validation request
    json priorityValidationRequest;
    priorityValidationRequest["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
    priorityValidationRequest["setDataValidation"]["range"]["startRowIndex"] = 1;
    priorityValidationRequest["setDataValidation"]["range"]["startColumnIndex"] = 5;
    priorityValidationRequest["setDataValidation"]["range"]["endColumnIndex"] = 6;
    priorityValidationRequest["setDataValidation"]["rule"]["condition"]["type"] = "ONE_OF_LIST";
    priorityValidationRequest["setDataValidation"]["rule"]["condition"]["values"] = json::array();

    json highVal, medVal, lowVal;
    highVal["userEnteredValue"] = "HIGH";
    medVal["userEnteredValue"] = "MEDIUM";
    lowVal["userEnteredValue"] = "LOW";
    priorityValidationRequest["setDataValidation"]["rule"]["condition"]["values"].push_back(highVal);
    priorityValidationRequest["setDataValidation"]["rule"]["condition"]["values"].push_back(medVal);
    priorityValidationRequest["setDataValidation"]["rule"]["condition"]["values"].push_back(lowVal);
    priorityValidationRequest["setDataValidation"]["rule"]["showCustomUi"] = true;
    priorityValidationRequest["setDataValidation"]["rule"]["strict"] = false;
    allRequests.push_back(priorityValidationRequest);

    // Priority conditional formatting for HIGH (Red background)
    json highFormatRequest;
    highFormatRequest["addConditionalFormatRule"]["rule"]["ranges"] = json::array();
    json highRange;
    highRange["sheetId"] = std::stoi(sheetId);
    highRange["startRowIndex"] = 1;
    highRange["startColumnIndex"] = 5;
    highRange["endColumnIndex"] = 6;
    highFormatRequest["addConditionalFormatRule"]["rule"]["ranges"].push_back(highRange);
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["type"] = "TEXT_EQ";
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"] = json::array();
    json highCondValue;
    highCondValue["userEnteredValue"] = "HIGH";
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"].push_back(highCondValue);
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["red"] = 0.956;
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["green"] = 0.263;
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["blue"] = 0.211;
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["red"] = 1.0;
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["green"] = 1.0;
    highFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["blue"] = 1.0;
    allRequests.push_back(highFormatRequest);

    // Priority conditional formatting for MEDIUM (Yellow background)
    json medFormatRequest;
    medFormatRequest["addConditionalFormatRule"]["rule"]["ranges"] = json::array();
    json medRange;
    medRange["sheetId"] = std::stoi(sheetId);
    medRange["startRowIndex"] = 1;
    medRange["startColumnIndex"] = 5;
    medRange["endColumnIndex"] = 6;
    medFormatRequest["addConditionalFormatRule"]["rule"]["ranges"].push_back(medRange);
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["type"] = "TEXT_EQ";
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"] = json::array();
    json medCondValue;
    medCondValue["userEnteredValue"] = "MEDIUM";
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"].push_back(medCondValue);
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["red"] = 0.984;
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["green"] = 0.737;
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["blue"] = 0.019;
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["red"] = 0.0;
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["green"] = 0.0;
    medFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["blue"] = 0.0;
    allRequests.push_back(medFormatRequest);

    // Priority conditional formatting for LOW (Grey background)
    json lowFormatRequest;
    lowFormatRequest["addConditionalFormatRule"]["rule"]["ranges"] = json::array();
    json lowRange;
    lowRange["sheetId"] = std::stoi(sheetId);
    lowRange["startRowIndex"] = 1;
    lowRange["startColumnIndex"] = 5;
    lowRange["endColumnIndex"] = 6;
    lowFormatRequest["addConditionalFormatRule"]["rule"]["ranges"].push_back(lowRange);
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["type"] = "TEXT_EQ";
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"] = json::array();
    json lowCondValue;
    lowCondValue["userEnteredValue"] = "LOW";
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["condition"]["values"].push_back(lowCondValue);
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["red"] = 0.663;
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["green"] = 0.663;
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["backgroundColor"]["blue"] = 0.663;
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["red"] = 1.0;
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["green"] = 1.0;
    lowFormatRequest["addConditionalFormatRule"]["rule"]["booleanRule"]["format"]["textFormat"]["foregroundColor"]["blue"] = 1.0;
    allRequests.push_back(lowFormatRequest);

    // ===== COLUMN J: Links formatting (index 9) =====
    // Format links column with blue text and underline
    json linksFormatRequest;
    linksFormatRequest["repeatCell"]["range"]["sheetId"] = std::stoi(sheetId);
    linksFormatRequest["repeatCell"]["range"]["startRowIndex"] = 1;  // Skip header
    linksFormatRequest["repeatCell"]["range"]["startColumnIndex"] = 9;  // Column J
    linksFormatRequest["repeatCell"]["range"]["endColumnIndex"] = 10;
    linksFormatRequest["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["blue"] = 1.0;
    linksFormatRequest["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["red"] = 0.0;
    linksFormatRequest["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["green"] = 0.0;
    linksFormatRequest["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["underline"] = true;
    linksFormatRequest["repeatCell"]["fields"] = "userEnteredFormat.textFormat";
    allRequests.push_back(linksFormatRequest);

    // ===== SEND ALL FORMATTING IN ONE BATCH REQUEST =====
    json requestBody;
    requestBody["requests"] = allRequests;
    json response;
    std::string url = BuildBatchUpdateUrl(spreadsheetId);

    if (!ApiPost(url, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to apply sheet formatting" << std::endl;
        return false;
    }

    std::cout << "[GoogleSheetsManager] ✓ Sheet formatting complete for " << itemType
              << " (" << allRequests.size() << " formatting rules in 1 API call)" << std::endl;
    return true;
}

std::string GoogleSheetsManager::WideToUtf8(const std::wstring& wstr) const
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wstr);
}

std::wstring GoogleSheetsManager::Utf8ToWide(const std::string& str) const
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(str);
}

void GoogleSheetsManager::CheckAndDisableJob(JobSyncRecord& record, const std::string& jobName)
{
    const int ERROR_THRESHOLD = 5;

    if (record.consecutiveErrorCount >= ERROR_THRESHOLD) {
        record.disabledDueToErrors = true;
        std::cerr << "[GoogleSheetsManager] ✗ Job '" << jobName << "' disabled after "
                  << ERROR_THRESHOLD << " consecutive sync failures" << std::endl;
        std::cerr << "[GoogleSheetsManager] Check job folder, permissions, and network connectivity" << std::endl;
    }
}

uint64_t GoogleSheetsManager::GetCurrentTimestamp() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace UFB
