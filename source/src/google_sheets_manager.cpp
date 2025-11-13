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

namespace {
    // Only these folder types are synced to Google Sheets
    const std::vector<std::string> SYNCED_FOLDER_TYPES = {
        "3d", "ae", "premiere",      // Shot types we sync
        "assets", "postings"          // Always synced
    };
}

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

bool GoogleSheetsManager::BatchGet(const std::string& spreadsheetId,
                                  const std::vector<std::string>& ranges,
                                  std::vector<std::vector<SheetRow>>& outResults)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    if (ranges.empty()) {
        std::cerr << "[GoogleSheetsManager] BatchGet: No ranges specified" << std::endl;
        return false;
    }

    // Build batch get URL with multiple ranges
    std::string url = BuildSpreadsheetsUrl(spreadsheetId) + "/values:batchGet?";
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (i > 0) url += "&";
        url += "ranges=" + ranges[i];
    }

    json response;
    if (!ApiGet(url, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to batch get ranges" << std::endl;
        return false;
    }

    if (!response.contains("valueRanges")) {
        std::cerr << "[GoogleSheetsManager] BatchGet: No valueRanges in response" << std::endl;
        return false;
    }

    // Parse each range result
    outResults.clear();
    outResults.resize(ranges.size());

    for (size_t i = 0; i < response["valueRanges"].size() && i < ranges.size(); ++i) {
        const auto& valueRange = response["valueRanges"][i];

        if (!valueRange.contains("values")) {
            // Empty range - add empty result
            outResults[i] = std::vector<SheetRow>();
            continue;
        }

        std::vector<SheetRow> rows;
        for (const auto& row : valueRange["values"]) {
            SheetRow sheetRow;
            for (const auto& cell : row) {
                if (cell.is_string()) {
                    sheetRow.cells.push_back(cell.get<std::string>());
                } else {
                    sheetRow.cells.push_back(cell.dump());
                }
            }
            rows.push_back(sheetRow);
        }
        outResults[i] = rows;
    }

    std::cout << "[GoogleSheetsManager] BatchGet: Successfully read " << ranges.size() << " ranges in 1 API call" << std::endl;
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

bool GoogleSheetsManager::BatchClear(const std::string& spreadsheetId,
                                    const std::vector<std::string>& ranges)
{
    if (!m_authManager || !m_authManager->IsAuthenticated()) {
        std::cerr << "[GoogleSheetsManager] Not authenticated" << std::endl;
        return false;
    }

    if (ranges.empty()) {
        return true;  // Nothing to clear
    }

    // Build batch clear request
    json requestBody;
    requestBody["ranges"] = ranges;

    std::string url = BuildSpreadsheetsUrl(spreadsheetId) + "/values:batchClear";
    json response;

    if (!ApiPost(url, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to batch clear ranges" << std::endl;
        return false;
    }

    std::cout << "[GoogleSheetsManager] BatchClear: Successfully cleared " << ranges.size() << " ranges in 1 API call" << std::endl;
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

        // Map sheet titles to folderType IDs
        for (const auto& sheet : spreadsheet.sheets) {
            std::string folderType;
            if (sheet.title == "Tasks") {
                folderType = "manual_task";  // Special case
            } else {
                // Convert to lowercase: "3D" → "3d", "AE" → "ae", "ASSETS" → "assets"
                folderType = sheet.title;
                std::transform(folderType.begin(), folderType.end(), folderType.begin(), ::tolower);
            }

            // Only map known folder types
            bool isKnownType = (folderType == "manual_task") ||
                              (std::find(SYNCED_FOLDER_TYPES.begin(), SYNCED_FOLDER_TYPES.end(), folderType) != SYNCED_FOLDER_TYPES.end());

            if (isKnownType) {
                record.sheetIds[folderType] = sheet.sheetId;
            }
        }

        m_syncRecords[jobPath] = record;
        it = m_syncRecords.find(jobPath);
    }

    JobSyncRecord& record = it->second;
    record.status = SheetSyncStatus::Syncing;

    // Build list of folder types to sync from the sync record's sheetIds
    std::vector<std::string> folderTypes;
    std::map<std::string, std::string> sheetNames;  // folderType → sheet display name

    for (const auto& [folderType, sheetId] : record.sheetIds) {
        folderTypes.push_back(folderType);

        // Build display name (capitalize folder type)
        if (folderType == "manual_task") {
            sheetNames[folderType] = "Tasks";
        } else {
            std::string displayName = folderType;
            std::transform(displayName.begin(), displayName.end(), displayName.begin(), ::toupper);
            sheetNames[folderType] = displayName;
        }
    }

    bool allSucceeded = true;
    int totalRowsSynced = 0;

    // ===== BATCH READ: Read all tabs at once (1 API call) =====
    std::vector<std::string> readRanges;
    for (const std::string& folderType : folderTypes) {
        std::string sheetName = sheetNames[folderType];
        readRanges.push_back(sheetName + "!A:O");
    }

    std::vector<std::vector<SheetRow>> allExistingData;
    if (!BatchGet(record.spreadsheetId, readRanges, allExistingData)) {
        std::cerr << "[GoogleSheetsManager] Failed to batch read existing data" << std::endl;

        // Check if this is an old-format spreadsheet (has "Shots" sheet instead of folder-type sheets)
        GoogleSpreadsheet spreadsheet;
        if (GetSpreadsheet(record.spreadsheetId, spreadsheet)) {
            bool hasOldFormat = false;
            for (const auto& sheet : spreadsheet.sheets) {
                if (sheet.title == "Shots") {
                    hasOldFormat = true;
                    break;
                }
            }

            if (hasOldFormat) {
                std::cout << "[GoogleSheetsManager] ⚠ Detected old spreadsheet format for job '" << jobNameUtf8
                          << "' - deleting sync record to trigger recreation" << std::endl;
                m_syncRecords.erase(jobPath);
                SaveSyncRecords();
                return false;  // Skip this sync, will recreate on next cycle
            }
        }

        allSucceeded = false;
    }

    // Map folderType → existing sheet data
    std::map<std::string, std::vector<SheetRow>> existingDataByType;
    for (size_t i = 0; i < folderTypes.size() && i < allExistingData.size(); ++i) {
        existingDataByType[folderTypes[i]] = allExistingData[i];
    }

    // ===== BIDIRECTIONAL SYNC: Detect and apply remote changes =====
    SheetsCacheManager* cacheManager = GetOrCreateCacheManager(jobPath);

    for (const std::string& folderType : folderTypes) {
        std::string sheetName = sheetNames[folderType];
        std::vector<SheetRow>& existingData = existingDataByType[folderType];

        // Convert existing sheet data to CachedSheetRow format for comparison
        SheetTabCache newCache;
        newCache.spreadsheetId = record.spreadsheetId;
        newCache.tabName = sheetName;
        newCache.lastSyncTime = GetCurrentTimestamp();

        for (size_t i = 1; i < existingData.size(); ++i) {  // Skip header
            const SheetRow& row = existingData[i];
            if (row.cells.size() < 12) continue;  // Need at least columns A-L

            CachedSheetRow cachedRow;
            // Column A (index 0) is Name - we skip it and use Shot Path as itemPath
            cachedRow.itemPath = row.cells.size() > 1 ? row.cells[1] : "";
            cachedRow.itemType = row.cells.size() > 2 ? row.cells[2] : "";
            cachedRow.folderType = row.cells.size() > 3 ? row.cells[3] : "";
            cachedRow.status = row.cells.size() > 4 ? row.cells[4] : "";
            cachedRow.category = row.cells.size() > 5 ? row.cells[5] : "";
            cachedRow.priority = row.cells.size() > 6 ? row.cells[6] : "";
            cachedRow.deliveryDate = row.cells.size() > 7 ? row.cells[7] : "";
            cachedRow.assignedArtist = row.cells.size() > 8 ? row.cells[8] : "";
            cachedRow.notes = row.cells.size() > 9 ? row.cells[9] : "";
            cachedRow.clientApproval = row.cells.size() > 10 ? row.cells[10] : "";
            cachedRow.modifiedTimeStr = row.cells.size() > 11 ? row.cells[11] : "";

            // Parse tracking columns M, N, O
            if (row.cells.size() > 12) {
                try { cachedRow.modifiedTime = std::stoull(row.cells[12]); } catch (...) { cachedRow.modifiedTime = 0; }
            }
            if (row.cells.size() > 13) {
                try { cachedRow.syncedTime = std::stoull(row.cells[13]); } catch (...) { cachedRow.syncedTime = 0; }
            }
            if (row.cells.size() > 14) {
                cachedRow.deviceId = row.cells[14];
            }

            if (!cachedRow.itemPath.empty()) {
                newCache.rows[cachedRow.itemPath] = cachedRow;
            }
        }

        // Load old cache for change detection
        SheetTabCache oldCache;
        cacheManager->LoadCache(record.spreadsheetId, sheetName, oldCache);

        // Detect remote changes
        auto changes = cacheManager->DetectChanges(oldCache, newCache);

        std::cout << "[GoogleSheetsManager] Remote changes for " << sheetName << ": "
                  << changes.addedRows.size() << " added, "
                  << changes.modifiedRows.size() << " modified, "
                  << changes.deletedPaths.size() << " deleted" << std::endl;

        // Apply remote changes to local database
        std::vector<CachedSheetRow> remoteChangesToApply;
        remoteChangesToApply.insert(remoteChangesToApply.end(), changes.addedRows.begin(), changes.addedRows.end());
        remoteChangesToApply.insert(remoteChangesToApply.end(), changes.modifiedRows.begin(), changes.modifiedRows.end());

        if (!remoteChangesToApply.empty()) {
            ApplyRemoteChangesToLocal(jobPath, folderType, remoteChangesToApply);
        }

        // Don't save cache here - we'll save it AFTER the push phase
        // (so it reflects the data we actually pushed, including updated modifiedTime)
    }

    // Prepare batch write operations and cache data for after push
    std::vector<CellUpdate> batchUpdates;
    std::map<std::string, std::vector<SheetRow>> pushedDataByType;  // Store for cache saving

    for (const std::string& folderType : folderTypes) {
        std::string sheetName = sheetNames[folderType];
        std::string sheetId = record.sheetIds[folderType];

        if (sheetId.empty()) {
            std::cerr << "[GoogleSheetsManager] Warning: No sheet ID for " << sheetName << std::endl;
            continue;
        }

        // Get data for this folder type from local database
        // (PULL phase already applied any remote Sheets changes to local)
        std::vector<SheetRow> rows = ConvertJobToSheetRows(jobPath, folderType);

        // Store for cache saving after push
        pushedDataByType[folderType] = rows;

        // Add to batch update (will clear and write all sheets in one batch)
        if (true) {  // Always clear and rewrite, even if empty (clears deleted items)
            CellUpdate update;
            update.range = sheetName + "!A2:O";

            // Convert SheetRow to vector<vector<string>>
            for (const auto& row : rows) {
                update.values.push_back(row.cells);
            }

            // If empty, still add empty update to clear the sheet
            batchUpdates.push_back(update);
            totalRowsSynced += rows.size();
        }

        std::cout << "[GoogleSheetsManager] Prepared " << rows.size() << " rows for " << sheetName << std::endl;
    }

    // ===== BATCH CLEAR & WRITE: Clear and write all 4 tabs (2 API calls instead of 8) =====
    if (!batchUpdates.empty()) {
        // Step 1: Batch clear all ranges (1 API call)
        std::vector<std::string> clearRanges;
        for (const auto& update : batchUpdates) {
            clearRanges.push_back(update.range);
        }

        if (!BatchClear(record.spreadsheetId, clearRanges)) {
            std::cerr << "[GoogleSheetsManager] Failed to batch clear ranges" << std::endl;
            allSucceeded = false;
        }

        // Step 2: Batch write all data (1 API call)
        if (allSucceeded && !BatchUpdate(record.spreadsheetId, batchUpdates)) {
            std::cerr << "[GoogleSheetsManager] Failed to batch write data" << std::endl;
            allSucceeded = false;
        }

        if (allSucceeded) {
            std::cout << "[GoogleSheetsManager] ✓ Synced " << totalRowsSynced
                      << " total rows across " << batchUpdates.size() << " sheets (3 API calls: 1 batchGet + 1 batchClear + 1 batchUpdate)" << std::endl;

            // Save caches with the data we just pushed (includes updated modifiedTime)
            for (const auto& [folderType, rows] : pushedDataByType) {
                std::string sheetName = sheetNames[folderType];

                // Build cache from pushed data
                SheetTabCache pushedCache;
                pushedCache.spreadsheetId = record.spreadsheetId;
                pushedCache.tabName = sheetName;
                pushedCache.lastSyncTime = GetCurrentTimestamp();

                for (const auto& row : rows) {
                    if (row.cells.size() < 2) continue;

                    CachedSheetRow cachedRow;
                    cachedRow.itemPath = row.cells.size() > 1 ? row.cells[1] : "";
                    cachedRow.itemType = row.cells.size() > 2 ? row.cells[2] : "";
                    cachedRow.folderType = row.cells.size() > 3 ? row.cells[3] : "";
                    cachedRow.status = row.cells.size() > 4 ? row.cells[4] : "";
                    cachedRow.category = row.cells.size() > 5 ? row.cells[5] : "";
                    cachedRow.priority = row.cells.size() > 6 ? row.cells[6] : "";
                    cachedRow.deliveryDate = row.cells.size() > 7 ? row.cells[7] : "";
                    cachedRow.assignedArtist = row.cells.size() > 8 ? row.cells[8] : "";
                    cachedRow.notes = row.cells.size() > 9 ? row.cells[9] : "";
                    cachedRow.clientApproval = row.cells.size() > 10 ? row.cells[10] : "";
                    cachedRow.modifiedTimeStr = row.cells.size() > 11 ? row.cells[11] : "";

                    if (row.cells.size() > 12) {
                        try { cachedRow.modifiedTime = std::stoull(row.cells[12]); } catch (...) { cachedRow.modifiedTime = 0; }
                    }
                    if (row.cells.size() > 13) {
                        try { cachedRow.syncedTime = std::stoull(row.cells[13]); } catch (...) { cachedRow.syncedTime = 0; }
                    }
                    if (row.cells.size() > 14) {
                        cachedRow.deviceId = row.cells[14];
                    }

                    if (!cachedRow.itemPath.empty()) {
                        pushedCache.rows[cachedRow.itemPath] = cachedRow;
                    }
                }

                cacheManager->SaveCache(pushedCache);
            }
        }
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

    // Load ProjectConfig to determine which folder types exist for this job
    UFB::ProjectConfig config;
    config.LoadProjectConfig(jobPath);

    // Build list of sheets to create based on SYNCED_FOLDER_TYPES whitelist
    std::vector<std::string> newSheetNames;
    std::vector<std::string> folderTypesToSync;

    for (const auto& folderType : SYNCED_FOLDER_TYPES) {
        // Check if this folder type exists in project config
        if (config.GetFolderTypeConfig(folderType).has_value()) {
            // Capitalize for sheet name: "3d" → "3D", "ae" → "AE", "assets" → "ASSETS"
            std::string sheetName = folderType;
            std::transform(sheetName.begin(), sheetName.end(), sheetName.begin(), ::toupper);

            newSheetNames.push_back(sheetName);
            folderTypesToSync.push_back(folderType);
        }
    }

    // Always add Tasks sheet for manual tasks
    newSheetNames.push_back("Tasks");
    folderTypesToSync.push_back("manual_task");

    std::cout << "[GoogleSheetsManager] Creating " << newSheetNames.size() << " sheets: ";
    for (size_t i = 0; i < newSheetNames.size(); ++i) {
        std::cout << newSheetNames[i];
        if (i < newSheetNames.size() - 1) std::cout << ", ";
    }
    std::cout << std::endl;

    // Build batch update request to add all sheets and delete the default sheet
    json requests = json::array();

    // Add all sheets
    for (const auto& sheetName : newSheetNames) {
        json addSheetRequest;
        addSheetRequest["addSheet"]["properties"]["title"] = sheetName;
        addSheetRequest["addSheet"]["properties"]["gridProperties"]["rowCount"] = 1000;
        addSheetRequest["addSheet"]["properties"]["gridProperties"]["columnCount"] = 15;  // A-O: Name + Shot Path + metadata + tracking columns
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
        // Determine folderType from sheet title (convert to lowercase)
        std::string folderType;
        if (sheet.title == "Tasks") {
            folderType = "manual_task";  // Special case
        } else {
            // Convert sheet title to lowercase: "3D" → "3d", "AE" → "ae", "ASSETS" → "assets"
            folderType = sheet.title;
            std::transform(folderType.begin(), folderType.end(), folderType.begin(), ::tolower);
        }

        // Verify this is a synced folder type
        bool isKnownSheet = (folderType == "manual_task") ||
                           (std::find(SYNCED_FOLDER_TYPES.begin(), SYNCED_FOLDER_TYPES.end(), folderType) != SYNCED_FOLDER_TYPES.end());

        if (!isKnownSheet) {
            // Skip unknown sheets (like Sheet1 if deletion hasn't propagated yet)
            std::cout << "[GoogleSheetsManager] Skipping unknown sheet: " << sheet.title << " (ID: " << sheet.sheetId << ")" << std::endl;
            continue;
        }

        std::cout << "[GoogleSheetsManager] Applying formatting to sheet: " << sheet.title << " (ID: " << sheet.sheetId << ")" << std::endl;

        // Write headers for this sheet
        std::vector<SheetRow> headerRows;
        SheetRow header;
        header.cells = {"Name", "Shot Path", "Item Type", "Folder Type", "Status", "Category",
                       "Priority", "Due Date", "Artist", "Note", "Links", "Last Modified",
                       "ModifiedTime (ms)", "SyncedTime (ms)", "Device ID"};
        headerRows.push_back(header);

        std::string headerRange = sheet.title + "!A1:O1";
        if (!WriteRange(spreadsheetId, headerRange, headerRows)) {
            std::cerr << "[GoogleSheetsManager] Warning: Failed to write headers for sheet: " << sheet.title << std::endl;
        }

        // Apply formatting and data validation with per-job, per-folderType options
        if (!SetupSheetFormatting(spreadsheetId, sheet.sheetId, jobPath, folderType)) {
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

        // Column A: Name (extracted from path)
        row.cells.push_back(ExtractNameFromPath(shot.shotPath));

        // Column B: Shot Path (full path)
        row.cells.push_back(WideToUtf8(shot.shotPath));

        // Column C: Item Type
        row.cells.push_back(shot.itemType.empty() ? "" : shot.itemType);

        // Column D: Folder Type
        row.cells.push_back(shot.folderType.empty() ? "" : shot.folderType);

        // Column E: Status
        row.cells.push_back(shot.status.empty() ? "" : shot.status);

        // Column F: Category
        row.cells.push_back(shot.category.empty() ? "" : shot.category);

        // Column G: Priority (convert to HIGH/MEDIUM/LOW)
        std::string priorityStr = "";
        if (shot.priority == 1) {
            priorityStr = "HIGH";
        } else if (shot.priority == 2) {
            priorityStr = "MEDIUM";
        } else if (shot.priority == 3) {
            priorityStr = "LOW";
        }
        row.cells.push_back(priorityStr);

        // Column H: Due Date (format as readable date if set)
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

        // Column I: Artist
        row.cells.push_back(shot.artist.empty() ? "" : shot.artist);

        // Column J: Note
        row.cells.push_back(shot.note.empty() ? "" : shot.note);

        // Column K: Links
        row.cells.push_back(shot.links.empty() ? "" : shot.links);

        // Column L: Last Modified (human-readable format)
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

        // Column M: ModifiedTime (ms) - raw timestamp for tracking
        row.cells.push_back(std::to_string(shot.modifiedTime));

        // Column N: SyncedTime (ms) - current sync timestamp
        uint64_t syncTime = GetCurrentTimeMs();
        row.cells.push_back(std::to_string(syncTime));

        // Column O: Device ID - originating device
        row.cells.push_back(GetDeviceID());

        rows.push_back(row);
    }

    return rows;
}

std::vector<SheetRow> GoogleSheetsManager::ConvertJobToSheetRows(const std::wstring& jobPath, const std::string& folderType)
{
    std::vector<SheetRow> rows;

    if (!m_subscriptionManager) {
        return rows;
    }

    // Get all tracked items for this job
    auto allItems = m_subscriptionManager->GetAllTrackedItems(jobPath);

    // Filter by folderType
    for (const auto& item : allItems) {
        // Special case for manual tasks (they have itemType="manual_task")
        if (folderType == "manual_task") {
            if (item.itemType != "manual_task") {
                continue;
            }
        } else {
            // Normal case: match folderType (but exclude manual tasks)
            if (item.itemType == "manual_task") {
                continue;  // Manual tasks only go in Tasks sheet, not in other sheets
            }
            if (item.folderType != folderType) {
                continue;
            }
        }

        SheetRow row;

        // Column A: Name (extracted from path)
        row.cells.push_back(ExtractNameFromPath(item.shotPath));

        // Column B: Shot Path (or item path for assets/postings/tasks)
        row.cells.push_back(WideToUtf8(item.shotPath));

        // Column C: Item Type
        row.cells.push_back(item.itemType.empty() ? "" : item.itemType);

        // Column D: Folder Type
        row.cells.push_back(item.folderType.empty() ? "" : item.folderType);

        // Column E: Status
        row.cells.push_back(item.status.empty() ? "" : item.status);

        // Column F: Category
        row.cells.push_back(item.category.empty() ? "" : item.category);

        // Column G: Priority (convert to HIGH/MEDIUM/LOW)
        std::string priorityStr = "";
        if (item.priority == 1) {
            priorityStr = "HIGH";
        } else if (item.priority == 2) {
            priorityStr = "MEDIUM";
        } else if (item.priority == 3) {
            priorityStr = "LOW";
        }
        row.cells.push_back(priorityStr);

        // Column H: Due Date (format as readable date if set)
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

        // Column I: Artist
        row.cells.push_back(item.artist.empty() ? "" : item.artist);

        // Column J: Note
        row.cells.push_back(item.note.empty() ? "" : item.note);

        // Column K: Links
        row.cells.push_back(item.links.empty() ? "" : item.links);

        // Column L: Last Modified (human-readable format)
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

        // Column M: ModifiedTime (ms) - raw timestamp for tracking
        row.cells.push_back(std::to_string(item.modifiedTime));

        // Column N: SyncedTime (ms) - current sync timestamp
        uint64_t syncTime = GetCurrentTimeMs();
        row.cells.push_back(std::to_string(syncTime));

        // Column O: Device ID - originating device
        row.cells.push_back(GetDeviceID());

        rows.push_back(row);
    }

    std::cout << "[GoogleSheetsManager] ConvertJobToSheetRows: Found " << rows.size()
              << " items of type '" << folderType << "' for job" << std::endl;

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
        if (sheetData[i].cells.size() > 1) {
            // Shot path is in column B (index 1) - column A is Name
            if (sheetData[i].cells[1] == shotPathUtf8) {
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

std::vector<StatusOption> GoogleSheetsManager::GetStatusOptionsForFolderType(const std::wstring& jobPath, const std::string& folderType)
{
    std::vector<StatusOption> options;

    // Load per-job ProjectConfig
    UFB::ProjectConfig config;
    if (!config.LoadProjectConfig(jobPath)) {
        std::cerr << "[GoogleSheetsManager] Failed to load project config for job: " << WideToUtf8(jobPath) << std::endl;
        // Return defaults
        return {{"Not Started", "#94A3B8"}, {"In Progress", "#3B82F6"}, {"For Review", "#F59E0B"}, {"Complete", "#10B981"}};
    }

    // Special case: manual_task aggregates all status options
    if (folderType == "manual_task") {
        auto folderTypes = config.GetAllFolderTypes();
        for (const auto& ft : folderTypes) {
            auto statusOptions = config.GetStatusOptions(ft);
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
    } else {
        // Normal case: get options for specific folderType
        options = config.GetStatusOptions(folderType);
    }

    // If no options found, use defaults
    if (options.empty()) {
        return {{"Not Started", "#94A3B8"}, {"In Progress", "#3B82F6"}, {"For Review", "#F59E0B"}, {"Complete", "#10B981"}};
    }

    return options;
}

std::vector<CategoryOption> GoogleSheetsManager::GetCategoryOptionsForFolderType(const std::wstring& jobPath, const std::string& folderType)
{
    std::vector<CategoryOption> options;

    // Load per-job ProjectConfig
    UFB::ProjectConfig config;
    if (!config.LoadProjectConfig(jobPath)) {
        std::cerr << "[GoogleSheetsManager] Failed to load project config for job: " << WideToUtf8(jobPath) << std::endl;
        // Return defaults
        return {{"Offline", "#8B5CF6"}, {"Online", "#EC4899"}, {"On Hold", "#F59E0B"}, {"Killed", "#EF4444"}};
    }

    // Special case: manual_task aggregates all category options
    if (folderType == "manual_task") {
        auto folderTypes = config.GetAllFolderTypes();
        for (const auto& ft : folderTypes) {
            auto categoryOptions = config.GetCategoryOptions(ft);
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
    } else {
        // Normal case: get options for specific folderType
        options = config.GetCategoryOptions(folderType);
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
                                               const std::string& folderType)
{
    std::cout << "[GoogleSheetsManager] Setting up sheet formatting for folderType: " << folderType << std::endl;

    // Build ALL formatting requests in ONE array (reduces 4 API calls to 1)
    json allRequests = json::array();

    // ===== COLUMN E: Status dropdown with colors (index 4) =====
    auto statusOptions = GetStatusOptionsForFolderType(jobPath, folderType);
    if (!statusOptions.empty()) {
        // Status validation request
        json statusValidationRequest;
        statusValidationRequest["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
        statusValidationRequest["setDataValidation"]["range"]["startRowIndex"] = 1;
        statusValidationRequest["setDataValidation"]["range"]["startColumnIndex"] = 4;
        statusValidationRequest["setDataValidation"]["range"]["endColumnIndex"] = 5;
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

    // ===== COLUMN F: Category dropdown with colors (index 5) =====
    auto categoryOptions = GetCategoryOptionsForFolderType(jobPath, folderType);
    if (!categoryOptions.empty()) {
        // Category validation request
        json categoryValidationRequest;
        categoryValidationRequest["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
        categoryValidationRequest["setDataValidation"]["range"]["startRowIndex"] = 1;
        categoryValidationRequest["setDataValidation"]["range"]["startColumnIndex"] = 5;
        categoryValidationRequest["setDataValidation"]["range"]["endColumnIndex"] = 6;
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
            range["startColumnIndex"] = 5;
            range["endColumnIndex"] = 6;
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

    // ===== COLUMN G: Priority dropdown with colors (index 6) =====
    // HIGH=Red, MEDIUM=Yellow, LOW=Grey

    // Priority validation request
    json priorityValidationRequest;
    priorityValidationRequest["setDataValidation"]["range"]["sheetId"] = std::stoi(sheetId);
    priorityValidationRequest["setDataValidation"]["range"]["startRowIndex"] = 1;
    priorityValidationRequest["setDataValidation"]["range"]["startColumnIndex"] = 6;
    priorityValidationRequest["setDataValidation"]["range"]["endColumnIndex"] = 7;
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
    highRange["startColumnIndex"] = 6;
    highRange["endColumnIndex"] = 7;
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
    medRange["startColumnIndex"] = 6;
    medRange["endColumnIndex"] = 7;
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
    lowRange["startColumnIndex"] = 6;
    lowRange["endColumnIndex"] = 7;
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

    // ===== COLUMN K: Links formatting (index 10) =====
    // Format links column with blue text and underline
    json linksFormatRequest;
    linksFormatRequest["repeatCell"]["range"]["sheetId"] = std::stoi(sheetId);
    linksFormatRequest["repeatCell"]["range"]["startRowIndex"] = 1;  // Skip header
    linksFormatRequest["repeatCell"]["range"]["startColumnIndex"] = 10;  // Column K
    linksFormatRequest["repeatCell"]["range"]["endColumnIndex"] = 11;
    linksFormatRequest["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["blue"] = 1.0;
    linksFormatRequest["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["red"] = 0.0;
    linksFormatRequest["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["foregroundColor"]["green"] = 0.0;
    linksFormatRequest["repeatCell"]["cell"]["userEnteredFormat"]["textFormat"]["underline"] = true;
    linksFormatRequest["repeatCell"]["fields"] = "userEnteredFormat.textFormat";
    allRequests.push_back(linksFormatRequest);

    // ===== HIDE TRACKING COLUMNS M, N, O (indices 12, 13, 14) =====
    json hideColumnsRequest;
    hideColumnsRequest["updateDimensionProperties"]["range"]["sheetId"] = std::stoi(sheetId);
    hideColumnsRequest["updateDimensionProperties"]["range"]["dimension"] = "COLUMNS";
    hideColumnsRequest["updateDimensionProperties"]["range"]["startIndex"] = 12;  // Column M
    hideColumnsRequest["updateDimensionProperties"]["range"]["endIndex"] = 15;    // Up to O (exclusive)
    hideColumnsRequest["updateDimensionProperties"]["properties"]["hiddenByUser"] = true;
    hideColumnsRequest["updateDimensionProperties"]["fields"] = "hiddenByUser";
    allRequests.push_back(hideColumnsRequest);

    // ===== SEND ALL FORMATTING IN ONE BATCH REQUEST =====
    json requestBody;
    requestBody["requests"] = allRequests;
    json response;
    std::string url = BuildBatchUpdateUrl(spreadsheetId);

    if (!ApiPost(url, requestBody, response)) {
        std::cerr << "[GoogleSheetsManager] Failed to apply sheet formatting" << std::endl;
        return false;
    }

    std::cout << "[GoogleSheetsManager] ✓ Sheet formatting complete for " << folderType
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

std::string GoogleSheetsManager::ExtractNameFromPath(const std::wstring& path) const
{
    // Extract the last folder/file name from the path
    // E.g., "C:\jobs\project\shots\shot_001" -> "shot_001"
    // E.g., "tasks\task_123" -> "task_123"

    if (path.empty()) {
        return "";
    }

    // Find the last backslash or forward slash
    size_t lastSlash = path.find_last_of(L"\\/");

    if (lastSlash == std::wstring::npos) {
        // No slash found - return the whole path
        return WideToUtf8(path);
    }

    // Extract everything after the last slash
    std::wstring name = path.substr(lastSlash + 1);
    return WideToUtf8(name);
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

SheetsCacheManager* GoogleSheetsManager::GetOrCreateCacheManager(const std::wstring& jobPath)
{
    auto it = m_cacheManagers.find(jobPath);
    if (it != m_cacheManagers.end()) {
        return it->second.get();
    }

    // Create new cache manager for this job
    auto cacheManager = std::make_unique<SheetsCacheManager>();
    cacheManager->Initialize(jobPath);
    SheetsCacheManager* ptr = cacheManager.get();
    m_cacheManagers[jobPath] = std::move(cacheManager);

    std::wcout << L"[GoogleSheetsManager] Created cache manager for job: " << jobPath << std::endl;
    return ptr;
}

bool GoogleSheetsManager::ConflictResolve(const CachedSheetRow& localRow, const CachedSheetRow& remoteRow,
                                         CachedSheetRow& outWinner, bool& outLocalWins)
{
    // Latest timestamp wins
    if (localRow.modifiedTime > remoteRow.modifiedTime) {
        outWinner = localRow;
        outLocalWins = true;
        std::cout << "[GoogleSheetsManager] Conflict: Local wins (" << localRow.itemPath
                  << ") - Local:" << localRow.modifiedTime << " > Remote:" << remoteRow.modifiedTime << std::endl;
        return true;
    } else if (remoteRow.modifiedTime > localRow.modifiedTime) {
        outWinner = remoteRow;
        outLocalWins = false;
        std::cout << "[GoogleSheetsManager] Conflict: Remote wins (" << remoteRow.itemPath
                  << ") - Remote:" << remoteRow.modifiedTime << " > Local:" << localRow.modifiedTime << std::endl;
        return true;
    } else {
        // Equal timestamps - no conflict, no change needed
        outWinner = localRow;
        outLocalWins = true;
        return false;  // No actual conflict
    }
}

bool GoogleSheetsManager::ApplyRemoteChangesToLocal(const std::wstring& jobPath, const std::string& folderType,
                                                   const std::vector<CachedSheetRow>& remoteChanges)
{
    if (!m_subscriptionManager) {
        std::cerr << "[GoogleSheetsManager] No subscription manager - cannot apply remote changes" << std::endl;
        return false;
    }

    if (remoteChanges.empty()) {
        return true;  // Nothing to apply
    }

    std::cout << "[GoogleSheetsManager] Applying " << remoteChanges.size()
              << " remote changes for " << folderType << std::endl;

    // Apply each remote change to local database
    for (const auto& remoteRow : remoteChanges) {
        // Convert CachedSheetRow to ShotMetadata for subscription manager
        ShotMetadata item;
        item.shotPath = Utf8ToWide(remoteRow.itemPath);
        item.itemType = remoteRow.itemType;
        item.folderType = remoteRow.folderType;
        item.status = remoteRow.status;
        item.category = remoteRow.category;
        item.note = remoteRow.notes;
        item.artist = remoteRow.assignedArtist;
        item.links = remoteRow.clientApproval;  // Note: This might need adjustment based on schema
        // Set modifiedTime to NOW because this change just came from Sheets
        // (Users don't update the hidden Column M when editing, so remoteRow.modifiedTime is stale)
        item.modifiedTime = GetCurrentTimestamp();

        // Parse priority
        if (remoteRow.priority == "HIGH") item.priority = 1;
        else if (remoteRow.priority == "MEDIUM") item.priority = 2;
        else if (remoteRow.priority == "LOW") item.priority = 3;
        else item.priority = 0;

        // Parse delivery date (Column G in sheets)
        if (!remoteRow.deliveryDate.empty()) {
            // Parse date string "YYYY-MM-DD" to timestamp
            std::tm tm = {};
            std::istringstream ss(remoteRow.deliveryDate);
            ss >> std::get_time(&tm, "%Y-%m-%d");
            if (!ss.fail()) {
                item.dueDate = std::mktime(&tm) * 1000ULL;  // Convert to milliseconds
            }
        }

        // Update item in local database (this will also write to change log)
        if (!m_subscriptionManager->UpdateTrackedItemFromSheets(jobPath, item)) {
            std::cerr << "[GoogleSheetsManager] Failed to apply remote change: " << remoteRow.itemPath << std::endl;
            // Continue with other items even if one fails
        } else {
            std::cout << "[GoogleSheetsManager] ✓ Applied remote change: " << remoteRow.itemPath << std::endl;
        }
    }

    return true;
}

} // namespace UFB
