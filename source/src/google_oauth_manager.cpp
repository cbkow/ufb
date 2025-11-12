#include "google_oauth_manager.h"
#include "utils.h"
#include "nlohmann/json.hpp"
#include <Windows.h>
#include <winhttp.h>
#include <wininet.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>
#include <codecvt>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

using json = nlohmann::json;

namespace UFB {

GoogleOAuthManager::GoogleOAuthManager()
    : m_status(AuthStatus::NotAuthenticated)
    , m_serverRunning(false)
    , m_callbackPort(8080)
{
}

GoogleOAuthManager::~GoogleOAuthManager()
{
    StopCallbackServer();
}

bool GoogleOAuthManager::Initialize(const std::string& clientId, const std::string& clientSecret)
{
    try {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (clientId.empty() || clientSecret.empty()) {
            std::cout << "[GoogleOAuthManager] Client ID and Secret cannot be empty" << std::endl;
            return false;
        }

        m_config.clientId = clientId;
        m_config.clientSecret = clientSecret;

        // Try to load stored refresh token
        std::cout << "[GoogleOAuthManager] Attempting to load stored refresh token..." << std::endl;
        if (LoadStoredRefreshToken()) {
            std::cout << "[GoogleOAuthManager] Refresh token loaded, attempting to refresh access token..." << std::endl;
            // Try to refresh access token
            // NOTE: We already hold the mutex, so call ExchangeRefreshTokenForAccessToken directly
            // instead of RefreshAccessToken() which would try to acquire the lock again
            if (ExchangeRefreshTokenForAccessToken()) {
                UpdateStatus(AuthStatus::Authenticated);
                std::cout << "[GoogleOAuthManager] Restored session from stored refresh token" << std::endl;
                return true;
            } else {
                std::cout << "[GoogleOAuthManager] Failed to refresh access token (token may be expired or invalid)" << std::endl;
                // Clear invalid token
                ClearStoredRefreshToken();
            }
        } else {
            std::cout << "[GoogleOAuthManager] No stored refresh token found" << std::endl;
        }

        UpdateStatus(AuthStatus::NotAuthenticated);
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[GoogleOAuthManager] Exception in Initialize: " << e.what() << std::endl;
        UpdateStatus(AuthStatus::Failed);
        return false;
    }
    catch (...) {
        std::cerr << "[GoogleOAuthManager] Unknown exception in Initialize" << std::endl;
        UpdateStatus(AuthStatus::Failed);
        return false;
    }
}

bool GoogleOAuthManager::StartAuthFlow()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_status == AuthStatus::Authenticating) {
        std::cout << "[GoogleOAuthManager] Authentication already in progress" << std::endl;
        return false;
    }

    // Generate state parameter for CSRF protection
    m_currentState = GenerateStateParameter();

    // Start local HTTP server for OAuth callback
    StartCallbackServer();

    // Wait for server to be ready (give it 500ms to bind and start listening)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Generate authorization URL
    std::string authUrl = GenerateAuthUrl();

    // Debug: Print the OAuth URL
    std::cout << "[GoogleOAuthManager] Generated OAuth URL: " << authUrl << std::endl;
    std::cout << "[GoogleOAuthManager] Client ID: " << m_config.clientId.substr(0, 20) << "..." << std::endl;
    std::cout << "[GoogleOAuthManager] Redirect URI: " << m_config.redirectUri << std::endl;

    // Open browser
    if (!OpenUrlInBrowser(authUrl)) {
        std::cout << "[GoogleOAuthManager] Failed to open browser" << std::endl;
        StopCallbackServer();
        return false;
    }

    UpdateStatus(AuthStatus::Authenticating);
    std::cout << "[GoogleOAuthManager] OAuth flow started - waiting for user authorization" << std::endl;
    return true;
}

bool GoogleOAuthManager::IsAuthenticated() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status == AuthStatus::Authenticated && !m_tokens.accessToken.empty();
}

std::string GoogleOAuthManager::GetAccessToken()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_tokens.accessToken.empty()) {
        return "";
    }

    // Check if token is expired and refresh if needed
    if (m_tokens.IsAccessTokenExpired()) {
        std::cout << "[GoogleOAuthManager] Access token expired, refreshing..." << std::endl;
        if (!ExchangeRefreshTokenForAccessToken()) {
            std::cout << "[GoogleOAuthManager] Failed to refresh access token" << std::endl;
            UpdateStatus(AuthStatus::RefreshNeeded);
            return "";
        }
    }

    return m_tokens.accessToken;
}

bool GoogleOAuthManager::RefreshAccessToken()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_tokens.refreshToken.empty()) {
        // Try to load from storage
        std::string storedRefreshToken;
        if (LoadRefreshToken(storedRefreshToken)) {
            m_tokens.refreshToken = storedRefreshToken;
        } else {
            std::cout << "[GoogleOAuthManager] No refresh token available" << std::endl;
            return false;
        }
    }

    return ExchangeRefreshTokenForAccessToken();
}

void GoogleOAuthManager::Logout()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    StopCallbackServer();
    m_tokens = OAuthTokens();
    ClearStoredRefreshToken();
    UpdateStatus(AuthStatus::NotAuthenticated);
    std::cout << "[GoogleOAuthManager] Logged out and cleared tokens" << std::endl;
}

AuthStatus GoogleOAuthManager::GetStatus() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void GoogleOAuthManager::SetAuthStatusCallback(std::function<void(AuthStatus)> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_statusCallback = callback;
}

bool GoogleOAuthManager::LoadStoredRefreshToken()
{
    std::string refreshToken;
    if (LoadRefreshToken(refreshToken)) {
        m_tokens.refreshToken = refreshToken;
        return true;
    }
    return false;
}

bool GoogleOAuthManager::TestConnection()
{
    std::string accessToken = GetAccessToken();
    if (accessToken.empty()) {
        return false;
    }

    // Test connection by making a simple API call
    std::string response;
    std::string authHeader = "Bearer " + accessToken;

    // Test with a simple Drive API call
    return HttpGet("https://www.googleapis.com/drive/v3/about?fields=user",
                   authHeader,
                   response);
}

// Private methods

void GoogleOAuthManager::StartCallbackServer()
{
    if (m_serverRunning) {
        return;
    }

    m_serverRunning = true;
    m_callbackServerThread = std::thread(&GoogleOAuthManager::CallbackServerLoop, this);
}

void GoogleOAuthManager::StopCallbackServer()
{
    if (m_serverRunning) {
        m_serverRunning = false;
        if (m_callbackServerThread.joinable()) {
            m_callbackServerThread.join();
        }
    }
}

void GoogleOAuthManager::CallbackServerLoop()
{
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cout << "[GoogleOAuthManager] Failed to initialize Winsock" << std::endl;
        m_serverRunning = false;
        return;
    }

    // Create socket
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cout << "[GoogleOAuthManager] Failed to create socket" << std::endl;
        WSACleanup();
        m_serverRunning = false;
        return;
    }

    // Set socket to non-blocking mode for timeout support
    u_long mode = 1;
    ioctlsocket(listenSocket, FIONBIO, &mode);

    // Bind socket
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port = htons(m_callbackPort);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        std::cerr << "[GoogleOAuthManager] Failed to bind socket to port " << m_callbackPort << " (error: " << error << ")" << std::endl;
        closesocket(listenSocket);
        WSACleanup();
        m_serverRunning = false;
        return;
    }

    std::cout << "[GoogleOAuthManager] Socket successfully bound to 127.0.0.1:" << m_callbackPort << std::endl;

    // Listen
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        std::cout << "[GoogleOAuthManager] Failed to listen on socket (error: " << error << ")" << std::endl;
        closesocket(listenSocket);
        WSACleanup();
        m_serverRunning = false;
        return;
    }

    std::cout << "[GoogleOAuthManager] Callback server listening on port " << m_callbackPort << std::endl;
    std::cout << "[GoogleOAuthManager] Ready to accept connections..." << std::endl;

    // Accept connections
    while (m_serverRunning) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);

        timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int result = select(0, &readSet, nullptr, nullptr, &timeout);
        if (result > 0) {
            std::cout << "[GoogleOAuthManager] Incoming connection detected..." << std::endl;
            SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
            if (clientSocket != INVALID_SOCKET) {
                // Set client socket to blocking mode (inherited non-blocking from listen socket)
                u_long blockingMode = 0;
                ioctlsocket(clientSocket, FIONBIO, &blockingMode);

                std::cout << "[GoogleOAuthManager] Connection accepted, processing request..." << std::endl;
                std::string authCode;
                if (HandleHttpRequest(clientSocket, authCode)) {
                    std::cout << "[GoogleOAuthManager] Received authorization code" << std::endl;

                    // Exchange code for tokens
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (ExchangeAuthCodeForTokens(authCode)) {
                        UpdateStatus(AuthStatus::Authenticated);
                        std::cout << "[GoogleOAuthManager] Successfully authenticated!" << std::endl;
                    } else {
                        UpdateStatus(AuthStatus::Failed);
                        std::cout << "[GoogleOAuthManager] Failed to exchange authorization code" << std::endl;
                    }

                    // Stop server after receiving callback
                    m_serverRunning = false;
                }
                closesocket(clientSocket);
            }
        }
    }

    closesocket(listenSocket);
    WSACleanup();
    std::cout << "[GoogleOAuthManager] Callback server stopped" << std::endl;
}

bool GoogleOAuthManager::HandleHttpRequest(int clientSocket, std::string& outAuthCode)
{
    char buffer[4096] = {0};
    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

    std::cout << "[GoogleOAuthManager] recv() returned: " << bytesReceived << " bytes" << std::endl;

    if (bytesReceived <= 0) {
        int error = WSAGetLastError();
        std::cerr << "[GoogleOAuthManager] recv() failed or connection closed (error: " << error << ")" << std::endl;
        return false;
    }

    std::string request(buffer);
    std::cout << "[GoogleOAuthManager] Received request: " << request.substr(0, 200) << "..." << std::endl;

    // Parse request line: GET /oauth2callback?code=xxx&state=yyy HTTP/1.1
    size_t firstSpace = request.find(' ');
    size_t secondSpace = request.find(' ', firstSpace + 1);

    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        return false;
    }

    std::string requestLine = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);

    // Debug: Print what path was requested
    std::cout << "[GoogleOAuthManager] Received HTTP request for: " << requestLine << std::endl;

    // Check if this is our callback path
    if (requestLine.find("/oauth2callback") != 0) {
        // Send 404 for other paths (e.g., favicon.ico)
        std::string response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                              "<html><body><h1>404 Not Found</h1></body></html>";
        send(clientSocket, response.c_str(), response.length(), 0);
        return false;
    }

    // Parse query parameters
    size_t queryStart = requestLine.find('?');
    if (queryStart == std::string::npos) {
        return false;
    }

    std::string queryString = requestLine.substr(queryStart + 1);

    // Extract code and state
    std::string code;
    std::string state;

    size_t pos = 0;
    while (pos < queryString.length()) {
        size_t ampPos = queryString.find('&', pos);
        std::string param = queryString.substr(pos, ampPos == std::string::npos ? std::string::npos : ampPos - pos);

        size_t eqPos = param.find('=');
        if (eqPos != std::string::npos) {
            std::string key = param.substr(0, eqPos);
            std::string value = param.substr(eqPos + 1);

            if (key == "code") {
                code = value;
            } else if (key == "state") {
                state = value;
            }
        }

        if (ampPos == std::string::npos) break;
        pos = ampPos + 1;
    }

    // Verify state parameter
    if (state != m_currentState) {
        std::cout << "[GoogleOAuthManager] State parameter mismatch - possible CSRF attack" << std::endl;

        // Send error response
        std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                              "<html><body><h1>Authentication Failed</h1><p>State verification failed.</p></body></html>";
        send(clientSocket, response.c_str(), response.length(), 0);
        return false;
    }

    if (code.empty()) {
        // Send error response
        std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                              "<html><body><h1>Authentication Failed</h1><p>No authorization code received.</p></body></html>";
        send(clientSocket, response.c_str(), response.length(), 0);
        return false;
    }

    // Send success response
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                          "<html><body><h1>Authentication Successful!</h1>"
                          "<p>You can close this window and return to the application.</p>"
                          "<script>window.close();</script></body></html>";
    send(clientSocket, response.c_str(), response.length(), 0);

    outAuthCode = code;
    return true;
}

bool GoogleOAuthManager::ExchangeAuthCodeForTokens(const std::string& authCode)
{
    // Prepare POST data
    std::string postData = "code=" + UrlEncode(authCode) +
                          "&client_id=" + UrlEncode(m_config.clientId) +
                          "&client_secret=" + UrlEncode(m_config.clientSecret) +
                          "&redirect_uri=" + UrlEncode(m_config.redirectUri) +
                          "&grant_type=authorization_code";

    std::string response;
    if (!HttpPost(m_config.tokenUri, postData, "application/x-www-form-urlencoded", response)) {
        std::cout << "[GoogleOAuthManager] Failed to exchange authorization code" << std::endl;
        return false;
    }

    // Parse response
    if (!ParseTokenResponse(response, m_tokens)) {
        std::cout << "[GoogleOAuthManager] Failed to parse token response" << std::endl;
        return false;
    }

    // Store refresh token securely
    if (!m_tokens.refreshToken.empty()) {
        StoreRefreshToken(m_tokens.refreshToken);
    }

    return true;
}

bool GoogleOAuthManager::ExchangeRefreshTokenForAccessToken()
{
    try {
        if (m_tokens.refreshToken.empty()) {
            std::cout << "[GoogleOAuthManager] No refresh token available for exchange" << std::endl;
            return false;
        }

        // Prepare POST data
        std::string postData = "refresh_token=" + UrlEncode(m_tokens.refreshToken) +
                              "&client_id=" + UrlEncode(m_config.clientId) +
                              "&client_secret=" + UrlEncode(m_config.clientSecret) +
                              "&grant_type=refresh_token";

        std::string response;
        if (!HttpPost(m_config.tokenUri, postData, "application/x-www-form-urlencoded", response)) {
            std::cout << "[GoogleOAuthManager] Failed to refresh access token (HTTP request failed)" << std::endl;
            return false;
        }

        // Parse response (refresh doesn't return new refresh token)
        OAuthTokens newTokens;
        if (!ParseTokenResponse(response, newTokens)) {
            std::cout << "[GoogleOAuthManager] Failed to parse token refresh response" << std::endl;
            return false;
        }

        // Update access token but keep existing refresh token
        m_tokens.accessToken = newTokens.accessToken;
        m_tokens.tokenType = newTokens.tokenType;
        m_tokens.expiresIn = newTokens.expiresIn;
        m_tokens.obtainedAt = newTokens.obtainedAt;

        std::cout << "[GoogleOAuthManager] Access token refreshed successfully" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[GoogleOAuthManager] Exception in ExchangeRefreshTokenForAccessToken: " << e.what() << std::endl;
        return false;
    }
    catch (...) {
        std::cerr << "[GoogleOAuthManager] Unknown exception in ExchangeRefreshTokenForAccessToken" << std::endl;
        return false;
    }
}

bool GoogleOAuthManager::StoreRefreshToken(const std::string& refreshToken)
{
    DATA_BLOB dataIn;
    DATA_BLOB dataOut;

    dataIn.pbData = (BYTE*)refreshToken.data();
    dataIn.cbData = (DWORD)refreshToken.size();

    if (!CryptProtectData(&dataIn, L"UFB Google Refresh Token", nullptr, nullptr, nullptr, 0, &dataOut)) {
        std::cout << "[GoogleOAuthManager] Failed to encrypt refresh token" << std::endl;
        return false;
    }

    // Write encrypted data to file
    std::filesystem::path appDataPath = UFB::GetLocalAppDataPath();
    std::wstring tokenPath = (appDataPath / L"google_refresh_token.dat").wstring();

    HANDLE hFile = CreateFileW(tokenPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        LocalFree(dataOut.pbData);
        std::cout << "[GoogleOAuthManager] Failed to create token file" << std::endl;
        return false;
    }

    DWORD bytesWritten;
    bool success = WriteFile(hFile, dataOut.pbData, dataOut.cbData, &bytesWritten, nullptr);

    CloseHandle(hFile);
    LocalFree(dataOut.pbData);

    if (success) {
        std::cout << "[GoogleOAuthManager] Refresh token stored securely" << std::endl;
    }

    return success;
}

bool GoogleOAuthManager::LoadRefreshToken(std::string& outRefreshToken)
{
    std::filesystem::path appDataPath = UFB::GetLocalAppDataPath();
    std::wstring tokenPath = (appDataPath / L"google_refresh_token.dat").wstring();

    // Check if file exists
    HANDLE hFile = CreateFileW(tokenPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Get file size
    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return false;
    }

    // Read encrypted data
    std::vector<BYTE> encryptedData(fileSize);
    DWORD bytesRead;
    if (!ReadFile(hFile, encryptedData.data(), fileSize, &bytesRead, nullptr)) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    // Decrypt data
    DATA_BLOB dataIn;
    DATA_BLOB dataOut;

    dataIn.pbData = encryptedData.data();
    dataIn.cbData = fileSize;

    if (!CryptUnprotectData(&dataIn, nullptr, nullptr, nullptr, nullptr, 0, &dataOut)) {
        std::cout << "[GoogleOAuthManager] Failed to decrypt refresh token" << std::endl;
        return false;
    }

    outRefreshToken = std::string((char*)dataOut.pbData, dataOut.cbData);
    LocalFree(dataOut.pbData);

    std::cout << "[GoogleOAuthManager] Refresh token loaded from storage" << std::endl;
    return true;
}

void GoogleOAuthManager::ClearStoredRefreshToken()
{
    std::filesystem::path appDataPath = UFB::GetLocalAppDataPath();
    std::wstring tokenPath = (appDataPath / L"google_refresh_token.dat").wstring();
    DeleteFileW(tokenPath.c_str());
}

std::string GoogleOAuthManager::GenerateAuthUrl() const
{
    std::stringstream ss;
    ss << m_config.authUri << "?"
       << "client_id=" << UrlEncode(m_config.clientId)
       << "&redirect_uri=" << UrlEncode(m_config.redirectUri)
       << "&response_type=code"
       << "&scope=" << UrlEncode(m_config.scope)
       << "&access_type=offline"  // Request refresh token
       << "&prompt=consent"        // Force consent screen to get refresh token
       << "&state=" << UrlEncode(m_currentState);

    return ss.str();
}

bool GoogleOAuthManager::OpenUrlInBrowser(const std::string& url)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::wstring wurl = converter.from_bytes(url);

    HINSTANCE result = ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)result > 32;
}

bool GoogleOAuthManager::HttpPost(const std::string& url,
                                  const std::string& postData,
                                  const std::string& contentType,
                                  std::string& outResponse)
{
    // Parse URL
    URL_COMPONENTSA urlComponents = {0};
    urlComponents.dwStructSize = sizeof(urlComponents);

    char hostname[256] = {0};
    char path[1024] = {0};

    urlComponents.lpszHostName = hostname;
    urlComponents.dwHostNameLength = sizeof(hostname);
    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = sizeof(path);

    if (!InternetCrackUrlA(url.c_str(), 0, 0, &urlComponents)) {
        return false;
    }

    // Initialize WinHTTP
    HINTERNET hSession = WinHttpOpen(L"UFB/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession) {
        return false;
    }

    // Convert hostname to wide string
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::wstring whostname = converter.from_bytes(hostname);

    // Connect
    HINTERNET hConnect = WinHttpConnect(hSession, whostname.c_str(), urlComponents.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Open request
    std::wstring wpath = converter.from_bytes(path);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                           L"POST",
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

    // Set headers
    std::wstring wcontentType = converter.from_bytes("Content-Type: " + contentType);
    WinHttpAddRequestHeaders(hRequest,
                            wcontentType.c_str(),
                            -1,
                            WINHTTP_ADDREQ_FLAG_ADD);

    // Send request
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
        // Read response
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

        outResponse = response;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return success;
}

bool GoogleOAuthManager::HttpGet(const std::string& url,
                                const std::string& authHeader,
                                std::string& outResponse)
{
    // Parse URL
    URL_COMPONENTSA urlComponents = {0};
    urlComponents.dwStructSize = sizeof(urlComponents);

    char hostname[256] = {0};
    char path[1024] = {0};

    urlComponents.lpszHostName = hostname;
    urlComponents.dwHostNameLength = sizeof(hostname);
    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = sizeof(path);

    if (!InternetCrackUrlA(url.c_str(), 0, 0, &urlComponents)) {
        return false;
    }

    // Initialize WinHTTP
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

    HINTERNET hConnect = WinHttpConnect(hSession, whostname.c_str(), urlComponents.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring wpath = converter.from_bytes(path);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                           L"GET",
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

    // Add authorization header
    std::wstring wauthHeader = converter.from_bytes("Authorization: " + authHeader);
    WinHttpAddRequestHeaders(hRequest,
                            wauthHeader.c_str(),
                            -1,
                            WINHTTP_ADDREQ_FLAG_ADD);

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

        outResponse = response;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return success;
}

bool GoogleOAuthManager::ParseTokenResponse(const std::string& response, OAuthTokens& outTokens)
{
    try {
        json j = json::parse(response);

        if (j.contains("error")) {
            std::cerr << "[GoogleOAuthManager] Token error: " << j["error"].get<std::string>() << std::endl;
            return false;
        }

        outTokens.accessToken = j.value("access_token", "");
        outTokens.tokenType = j.value("token_type", "Bearer");
        outTokens.expiresIn = j.value("expires_in", 3600);
        outTokens.obtainedAt = std::chrono::system_clock::now();

        // Refresh token is only provided on initial authorization
        if (j.contains("refresh_token")) {
            outTokens.refreshToken = j["refresh_token"];
        }

        return !outTokens.accessToken.empty();
    }
    catch (const std::exception& e) {
        std::cerr << "[GoogleOAuthManager] Failed to parse token response: " << e.what() << std::endl;
        return false;
    }
}

std::string GoogleOAuthManager::UrlEncode(const std::string& value) const
{
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        // Keep alphanumeric and other safe characters
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        }
        else {
            // Encode special characters
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int((unsigned char)c);
            escaped << std::nouppercase;
        }
    }

    return escaped.str();
}

void GoogleOAuthManager::UpdateStatus(AuthStatus newStatus)
{
    m_status = newStatus;
    if (m_statusCallback) {
        m_statusCallback(newStatus);
    }
}

std::string GoogleOAuthManager::GenerateStateParameter() const
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::stringstream ss;
    for (int i = 0; i < 32; ++i) {
        ss << std::hex << dis(gen);
    }

    return ss.str();
}

} // namespace UFB
