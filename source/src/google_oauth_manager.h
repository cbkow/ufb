#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

namespace UFB {

// OAuth token response structure
struct OAuthTokens {
    std::string accessToken;
    std::string refreshToken;
    std::string tokenType;
    int expiresIn;  // Seconds until access token expires
    std::chrono::system_clock::time_point obtainedAt;

    bool IsAccessTokenExpired() const {
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - obtainedAt).count();
        // Consider token expired 5 minutes before actual expiry for safety margin
        return elapsed >= (expiresIn - 300);
    }
};

// OAuth configuration
struct OAuthConfig {
    std::string clientId;
    std::string clientSecret;
    std::string redirectUri = "http://localhost:8080/oauth2callback";
    std::string scope = "https://www.googleapis.com/auth/spreadsheets https://www.googleapis.com/auth/drive.file";
    std::string authUri = "https://accounts.google.com/o/oauth2/v2/auth";
    std::string tokenUri = "https://oauth2.googleapis.com/token";
};

// Authentication status
enum class AuthStatus {
    NotAuthenticated,
    Authenticating,
    Authenticated,
    Failed,
    RefreshNeeded
};

// Interface for Google authentication
class IGoogleAuth {
public:
    virtual ~IGoogleAuth() = default;
    virtual bool Initialize(const std::string& clientId, const std::string& clientSecret) = 0;
    virtual bool StartAuthFlow() = 0;
    virtual bool IsAuthenticated() const = 0;
    virtual std::string GetAccessToken() = 0;
    virtual bool RefreshAccessToken() = 0;
    virtual void Logout() = 0;
    virtual AuthStatus GetStatus() const = 0;
    virtual bool TestConnection() = 0;
};

// Google OAuth 2.0 Manager
class GoogleOAuthManager : public IGoogleAuth {
public:
    GoogleOAuthManager();
    ~GoogleOAuthManager();

    // Initialize with OAuth credentials
    bool Initialize(const std::string& clientId, const std::string& clientSecret) override;

    // Start the OAuth authentication flow
    // Returns true if flow was initiated successfully
    // This will open a browser window and start local HTTP server
    bool StartAuthFlow() override;

    // Check if currently authenticated
    bool IsAuthenticated() const override;

    // Get current access token (refreshes if expired)
    std::string GetAccessToken() override;

    // Manually refresh the access token
    bool RefreshAccessToken() override;

    // Logout and clear stored tokens
    void Logout() override;

    // Get current authentication status
    AuthStatus GetStatus() const override;

    // Test connection to Google APIs
    bool TestConnection() override;

    // Set callback for auth status changes
    void SetAuthStatusCallback(std::function<void(AuthStatus)> callback);

    // Load stored refresh token from encrypted storage
    bool LoadStoredRefreshToken();

private:
    OAuthConfig m_config;
    OAuthTokens m_tokens;
    AuthStatus m_status;
    std::function<void(AuthStatus)> m_statusCallback;
    mutable std::mutex m_mutex;

    // Local HTTP server for OAuth callback
    std::thread m_callbackServerThread;
    std::atomic<bool> m_serverRunning;
    int m_callbackPort;

    // Start local HTTP server to receive OAuth callback
    void StartCallbackServer();
    void StopCallbackServer();
    void CallbackServerLoop();

    // Handle incoming HTTP request
    bool HandleHttpRequest(int clientSocket, std::string& outAuthCode);

    // Exchange authorization code for tokens
    bool ExchangeAuthCodeForTokens(const std::string& authCode);

    // Exchange refresh token for new access token
    bool ExchangeRefreshTokenForAccessToken();

    // Store refresh token securely using Windows DPAPI
    bool StoreRefreshToken(const std::string& refreshToken);

    // Load refresh token from Windows DPAPI
    bool LoadRefreshToken(std::string& outRefreshToken);

    // Clear stored refresh token
    void ClearStoredRefreshToken();

    // Generate authorization URL
    std::string GenerateAuthUrl() const;

    // Open URL in default browser
    bool OpenUrlInBrowser(const std::string& url);

    // HTTP helpers
    bool HttpPost(const std::string& url,
                  const std::string& postData,
                  const std::string& contentType,
                  std::string& outResponse);

    bool HttpGet(const std::string& url,
                 const std::string& authHeader,
                 std::string& outResponse);

    // JSON parsing helpers
    bool ParseTokenResponse(const std::string& response, OAuthTokens& outTokens);

    // URL encoding
    std::string UrlEncode(const std::string& value) const;

    // Update status and notify callback
    void UpdateStatus(AuthStatus newStatus);

    // Generate random state parameter for CSRF protection
    std::string GenerateStateParameter() const;
    std::string m_currentState;
};

} // namespace UFB
