#include "p2p_manager.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <set>
#include <algorithm>

using json = nlohmann::json;

namespace UFB {

// Utility: Get current timestamp in milliseconds
uint64_t P2PManager::GetCurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// Utility: Get computer name as device name
std::wstring P2PManager::GetDeviceName()
{
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(computerName, &size))
    {
        return std::wstring(computerName);
    }
    return L"Unknown";
}

// Utility: Get all local network IP addresses (excludes 127.0.0.1)
std::vector<std::string> P2PManager::GetAllLocalIPs()
{
    // Check cache first (5 minute TTL)
    {
        std::lock_guard<std::mutex> lock(m_ipCacheMutex);
        uint64_t now = GetCurrentTimestamp();
        uint64_t cacheAge = (m_lastIPRefresh > 0) ? (now - m_lastIPRefresh) : UINT64_MAX;

        // Cache TTL: 5 minutes (300,000 ms)
        if (cacheAge < 300000 && !m_cachedIPs.empty())
        {
            // Cache is still valid
            return m_cachedIPs;
        }
    }

    // Cache miss or stale - enumerate IP addresses
    std::vector<std::string> ips;

    // Get hostname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Failed to get hostname" << std::endl;
        return ips;
    }

    // Resolve all addresses for this hostname
    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;  // IPv4 only
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0)
    {
        std::cerr << "[P2P] Failed to resolve local addresses" << std::endl;
        return ips;
    }

    // Collect all non-loopback IPv4 addresses
    std::set<std::string> uniqueIPs;  // Use set to avoid duplicates
    for (auto ptr = result; ptr != nullptr; ptr = ptr->ai_next)
    {
        sockaddr_in* addr = (sockaddr_in*)ptr->ai_addr;
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, INET_ADDRSTRLEN);

        std::string ip(ipStr);

        // Skip loopback
        if (ip != "127.0.0.1" && ip != "0.0.0.0")
        {
            uniqueIPs.insert(ip);
        }
    }

    freeaddrinfo(result);

    // Convert set to vector and sort by priority
    // Priority: 192.168.x.x (common LAN) > 10.x.x.x (VPN/corporate) > 172.16-31.x.x > others
    for (const auto& ip : uniqueIPs)
    {
        ips.push_back(ip);
    }

    // Sort by preference
    std::sort(ips.begin(), ips.end(), [](const std::string& a, const std::string& b) {
        // Prefer 192.168.x.x (typical home/office LAN)
        bool aIs192 = (a.substr(0, 8) == "192.168.");
        bool bIs192 = (b.substr(0, 8) == "192.168.");
        if (aIs192 && !bIs192) return true;
        if (!aIs192 && bIs192) return false;

        // Then prefer 10.x.x.x (VPN/corporate)
        bool aIs10 = (a.substr(0, 3) == "10.");
        bool bIs10 = (b.substr(0, 3) == "10.");
        if (aIs10 && !bIs10) return true;
        if (!aIs10 && bIs10) return false;

        // Then prefer 172.16-31.x.x (corporate)
        bool aIs172 = (a.substr(0, 4) == "172.");
        bool bIs172 = (b.substr(0, 4) == "172.");
        if (aIs172 && !bIs172) return true;
        if (!aIs172 && bIs172) return false;

        return a < b;  // Lexicographic for others
    });

    if (!ips.empty())
    {
        std::cout << "[P2P] Refreshed local IP addresses (" << ips.size() << "):";
        for (const auto& ip : ips)
        {
            std::cout << " " << ip;
        }
        std::cout << std::endl;
    }
    else
    {
        std::cerr << "[P2P] WARNING: No local IP addresses found!" << std::endl;
    }

    // Update cache
    {
        std::lock_guard<std::mutex> lock(m_ipCacheMutex);
        m_cachedIPs = ips;
        m_lastIPRefresh = GetCurrentTimestamp();
    }

    return ips;
}

// Utility: Get primary local network IP address (not 127.0.0.1)
// Kept for backward compatibility - returns first IP from GetAllLocalIPs()
std::string P2PManager::GetLocalIPAddress()
{
    auto ips = GetAllLocalIPs();
    return ips.empty() ? "127.0.0.1" : ips[0];
}

// PeerInfo JSON serialization
json PeerInfo::ToJson() const
{
    return json{
        {"deviceId", std::string(deviceId.begin(), deviceId.end())},
        {"deviceName", std::string(deviceName.begin(), deviceName.end())},
        {"ipAddresses", ipAddresses},  // Array of IPs
        {"port", port},
        {"lastSeen", lastSeen}
    };
}

PeerInfo PeerInfo::FromJson(const json& j)
{
    PeerInfo info;
    std::string deviceIdStr = j.value("deviceId", "");
    std::string deviceNameStr = j.value("deviceName", "");
    info.deviceId = std::wstring(deviceIdStr.begin(), deviceIdStr.end());
    info.deviceName = std::wstring(deviceNameStr.begin(), deviceNameStr.end());

    // Support both new format (ipAddresses array) and old format (single ipAddress)
    if (j.contains("ipAddresses") && j["ipAddresses"].is_array())
    {
        info.ipAddresses = j["ipAddresses"].get<std::vector<std::string>>();
    }
    else if (j.contains("ipAddress"))
    {
        // Backward compatibility: single IP from old format
        info.ipAddresses.push_back(j.value("ipAddress", "127.0.0.1"));
    }

    info.port = j.value("port", 0);
    info.lastSeen = j.value("lastSeen", 0ULL);
    info.isActive = false;
    return info;
}

// Constructor
P2PManager::P2PManager()
    : m_listenSocket(INVALID_SOCKET)
    , m_iocpHandle(NULL)
    , m_listeningPort(0)
    , m_isRunning(false)
    , m_lastWrittenPort(0)
    , m_lastIPRefresh(0)
{
    // Initialize WinSock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::cerr << "[P2P] WSAStartup failed: " << result << std::endl;
    }
}

// Destructor
P2PManager::~P2PManager()
{
    Shutdown();
    WSACleanup();
}

// Initialize P2P (global, not per-project)
bool P2PManager::Initialize(const std::wstring& deviceId)
{
    std::wcout << L"[P2P] Initializing global P2P manager..." << std::endl;

    m_deviceId = deviceId;
    m_deviceName = GetDeviceName();

    // Create IOCP handle
    m_iocpHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (m_iocpHandle == NULL)
    {
        std::cerr << "[P2P] Failed to create IOCP handle: " << GetLastError() << std::endl;
        return false;
    }

    // Start IOCP worker thread
    m_isRunning = true;
    m_iocpWorkerThread = std::thread(&P2PManager::IOCPWorkerThread, this);

    // Start heartbeat thread (handles peer discovery and keepalives)
    m_heartbeatThread = std::thread(&P2PManager::HeartbeatThread, this);

    std::wcout << L"[P2P] Initialized successfully. Device: " << m_deviceName << L" (" << deviceId << L")" << std::endl;
    return true;
}

// Shutdown P2P
void P2PManager::Shutdown()
{
    if (!m_isRunning)
        return;

    std::cout << "[P2P] Shutting down..." << std::endl;
    m_isRunning = false;

    // Send GOODBYE to all connected peers
    {
        std::lock_guard<std::mutex> lock(m_peersMutex);
        json payload = {
            {"deviceId", std::string(m_deviceId.begin(), m_deviceId.end())},
            {"timestamp", GetCurrentTimestamp()}
        };

        for (auto& [deviceId, socket] : m_peerToSocket)
        {
            try
            {
                SendMessage(socket, P2PMessageType::GOODBYE, payload);
                std::cout << "[P2P] Sent GOODBYE to peer" << std::endl;
            }
            catch (const std::exception& e)
            {
                // Ignore errors during shutdown
                std::cerr << "[P2P] Error sending GOODBYE: " << e.what() << std::endl;
            }
        }
    }

    // Give a brief moment for GOODBYE messages to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Close all peer sockets using proper cleanup
    std::vector<SOCKET> socketsToClose;
    {
        std::lock_guard<std::mutex> lock(m_peersMutex);
        for (auto& [deviceId, socket] : m_peerToSocket)
        {
            socketsToClose.push_back(socket);
        }
    }

    for (SOCKET socket : socketsToClose)
    {
        CloseSocket(socket);
    }

    // Close listen socket
    if (m_listenSocket != INVALID_SOCKET)
    {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    // Signal IOCP to wake up worker thread
    if (m_iocpHandle != NULL)
    {
        PostQueuedCompletionStatus(m_iocpHandle, 0, 0, NULL);
    }

    // Wait for threads to finish
    if (m_iocpWorkerThread.joinable())
        m_iocpWorkerThread.join();

    if (m_heartbeatThread.joinable())
        m_heartbeatThread.join();

    // Clear all remaining contexts
    {
        std::lock_guard<std::mutex> lock(m_contextsMutex);
        m_activeContexts.clear();
    }

    // Clear all receive buffers
    {
        std::lock_guard<std::mutex> lock(m_buffersMutex);
        m_receiveBuffers.clear();
    }

    // Close IOCP handle
    if (m_iocpHandle != NULL)
    {
        CloseHandle(m_iocpHandle);
        m_iocpHandle = NULL;
    }

    // Cleanup WinSock (only call this once per process, ideally in the destructor)
    // Note: We don't call WSACleanup() here because other parts of the app might use WinSock
    // It will be cleaned up automatically when the process exits

    std::cout << "[P2P] Shutdown complete" << std::endl;
}

// Subscribe to a project (add to list of projects we're syncing)
void P2PManager::SubscribeToProject(const std::wstring& projectPath)
{
    std::lock_guard<std::mutex> lock(m_projectsMutex);

    if (m_subscribedProjects.find(projectPath) == m_subscribedProjects.end())
    {
        m_subscribedProjects.insert(projectPath);
        std::wcout << L"[P2P] Subscribed to project: " << projectPath << L" (total: " << m_subscribedProjects.size() << L")" << std::endl;
    }
}

// Unsubscribe from a project
void P2PManager::UnsubscribeFromProject(const std::wstring& projectPath)
{
    std::lock_guard<std::mutex> lock(m_projectsMutex);

    if (m_subscribedProjects.erase(projectPath) > 0)
    {
        std::wcout << L"[P2P] Unsubscribed from project: " << projectPath << L" (remaining: " << m_subscribedProjects.size() << L")" << std::endl;
    }
}

// Get list of subscribed projects
std::vector<std::wstring> P2PManager::GetSubscribedProjects() const
{
    std::lock_guard<std::mutex> lock(m_projectsMutex);
    return std::vector<std::wstring>(m_subscribedProjects.begin(), m_subscribedProjects.end());
}

// Start listening for connections
bool P2PManager::StartListening(uint16_t preferredPort)
{
    // Create TCP socket
    m_listenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (m_listenSocket == INVALID_SOCKET)
    {
        std::cerr << "[P2P] Failed to create listen socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    // NOTE: SO_REUSEADDR removed - each P2P manager needs a unique port
    // With SO_REUSEADDR, multiple managers thought they were on the same port,
    // causing cross-wired connections and lost messages.

    // Bind to port
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;

    // Try preferred port, or find available port starting from 49152
    uint16_t portToTry = (preferredPort > 0) ? preferredPort : 49152;
    bool bound = false;

    for (int attempts = 0; attempts < 100; attempts++)
    {
        addr.sin_port = htons(portToTry);
        if (bind(m_listenSocket, (sockaddr*)&addr, sizeof(addr)) == 0)
        {
            bound = true;
            m_listeningPort = portToTry;
            break;
        }
        portToTry++;
    }

    if (!bound)
    {
        std::cerr << "[P2P] Failed to bind to any port" << std::endl;
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    // Listen for connections
    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Listen failed: " << WSAGetLastError() << std::endl;
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    // Associate listen socket with IOCP
    if (CreateIoCompletionPort((HANDLE)m_listenSocket, m_iocpHandle, (ULONG_PTR)m_listenSocket, 0) == NULL)
    {
        std::cerr << "[P2P] Failed to associate listen socket with IOCP: " << GetLastError() << std::endl;
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    // Post initial accept operations
    for (int i = 0; i < 5; i++)
    {
        PostAccept();
    }

    std::cout << "[P2P] Listening on port " << m_listeningPort << std::endl;

    // Write our peer info to peers.json
    WritePeerRegistry();

    return true;
}

// Post an accept operation
bool P2PManager::PostAccept()
{
    // Create accept socket
    SOCKET acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (acceptSocket == INVALID_SOCKET)
    {
        std::cerr << "[P2P] Failed to create accept socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    // Create IO context
    auto context = std::make_shared<IOContext>(IOOperation::Accept, acceptSocket);

    // AcceptEx requires buffer for local and remote addresses
    context->buffer.resize((sizeof(sockaddr_in) + 16) * 2);
    context->wsaBuf.buf = context->buffer.data();
    context->wsaBuf.len = static_cast<ULONG>(context->buffer.size());

    // Load AcceptEx function
    LPFN_ACCEPTEX lpfnAcceptEx = NULL;
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD dwBytes = 0;

    if (WSAIoctl(m_listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guidAcceptEx, sizeof(guidAcceptEx),
                 &lpfnAcceptEx, sizeof(lpfnAcceptEx),
                 &dwBytes, NULL, NULL) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Failed to load AcceptEx: " << WSAGetLastError() << std::endl;
        closesocket(acceptSocket);
        return false;
    }

    // Call AcceptEx
    DWORD bytesReceived = 0;
    if (!lpfnAcceptEx(m_listenSocket, acceptSocket, context->buffer.data(), 0,
                      sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
                      &bytesReceived, &context->overlapped))
    {
        int error = WSAGetLastError();
        if (error != ERROR_IO_PENDING)
        {
            std::cerr << "[P2P] AcceptEx failed: " << error << std::endl;
            closesocket(acceptSocket);
            return false;
        }
    }

    // Store context in list to keep it alive
    {
        std::lock_guard<std::mutex> lock(m_contextsMutex);
        m_activeContexts.push_back(context);
    }

    return true;
}

// Post a receive operation
bool P2PManager::PostReceive(SOCKET socket)
{
    auto context = std::make_shared<IOContext>(IOOperation::Receive, socket);

    DWORD flags = 0;
    DWORD bytesReceived = 0;

    if (WSARecv(socket, &context->wsaBuf, 1, &bytesReceived, &flags, &context->overlapped, NULL) == SOCKET_ERROR)
    {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING)
        {
            std::cerr << "[P2P] WSARecv failed: " << error << std::endl;
            return false;
        }
    }

    // Store context in list to keep it alive
    {
        std::lock_guard<std::mutex> lock(m_contextsMutex);
        m_activeContexts.push_back(context);
    }

    return true;
}

// Post a send operation
bool P2PManager::PostSend(SOCKET socket, const std::vector<char>& data)
{
    auto context = std::make_shared<IOContext>(IOOperation::Send, socket);
    context->buffer = data;
    context->wsaBuf.buf = context->buffer.data();
    context->wsaBuf.len = static_cast<ULONG>(context->buffer.size());
    context->totalBytes = data.size();
    context->processedBytes = 0;

    DWORD bytesSent = 0;
    if (WSASend(socket, &context->wsaBuf, 1, &bytesSent, 0, &context->overlapped, NULL) == SOCKET_ERROR)
    {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING)
        {
            std::cerr << "[P2P] WSASend failed: " << error << std::endl;
            return false;
        }
    }

    // Store context in list to keep it alive
    {
        std::lock_guard<std::mutex> lock(m_contextsMutex);
        m_activeContexts.push_back(context);
    }

    return true;
}

// IOCP worker thread
void P2PManager::IOCPWorkerThread()
{
    std::cout << "[P2P] IOCP worker thread started" << std::endl;

    while (m_isRunning)
    {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED overlapped = NULL;

        BOOL result = GetQueuedCompletionStatus(m_iocpHandle, &bytesTransferred, &completionKey, &overlapped, 1000);

        if (!result)
        {
            if (overlapped == NULL)
            {
                // Timeout or error
                continue;
            }

            // I/O operation failed
            int error = GetLastError();
            if (error != ERROR_OPERATION_ABORTED)
            {
                std::cerr << "[P2P] GetQueuedCompletionStatus failed: " << error << std::endl;
            }
            continue;
        }

        if (bytesTransferred == 0 && completionKey == 0 && overlapped == NULL)
        {
            // Shutdown signal
            break;
        }

        // Get IOContext from overlapped
        IOContext* context = CONTAINING_RECORD(overlapped, IOContext, overlapped);

        // Handle operation
        try
        {
            switch (context->operation)
            {
            case IOOperation::Accept:
                HandleAccept(context);
                break;
            case IOOperation::Receive:
                HandleReceive(context, bytesTransferred);
                break;
            case IOOperation::Send:
                HandleSend(context, bytesTransferred);
                break;
            default:
                break;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[P2P] Exception in IOCP worker: " << e.what() << std::endl;
        }

        // Remove completed context from list (only if operation is complete)
        // For partial sends, don't remove - they'll be resubmitted
        bool shouldRemove = true;
        if (context->operation == IOOperation::Send)
        {
            // Check if this is a partial send that needs continuation
            if (context->processedBytes < context->totalBytes)
            {
                shouldRemove = false; // Keep context alive for partial send continuation
            }
        }

        if (shouldRemove)
        {
            std::lock_guard<std::mutex> lock(m_contextsMutex);
            m_activeContexts.remove_if([context](const std::shared_ptr<IOContext>& ctx) {
                return ctx.get() == context;
            });
        }
    }

    std::cout << "[P2P] IOCP worker thread stopped" << std::endl;
}

// Handle accept completion
void P2PManager::HandleAccept(IOContext* context)
{
    SOCKET acceptSocket = context->socket;

    // Validate socket is still active
    if (acceptSocket == INVALID_SOCKET)
    {
        std::cerr << "[P2P] HandleAccept: Invalid socket" << std::endl;
        PostAccept(); // Post new accept
        return;
    }

    std::cout << "[P2P] Accepted new connection" << std::endl;

    // Update accept socket context
    if (setsockopt(acceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                   (char*)&m_listenSocket, sizeof(m_listenSocket)) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] SO_UPDATE_ACCEPT_CONTEXT failed: " << WSAGetLastError() << std::endl;
        closesocket(acceptSocket);
        PostAccept(); // Post new accept
        return;
    }

    // Set TCP_NODELAY to disable Nagle's algorithm (critical for VPNs and real-time)
    int nodelay = 1;
    if (setsockopt(acceptSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay)) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Warning: Failed to set TCP_NODELAY on accepted socket: " << WSAGetLastError() << std::endl;
    }

    // Set SO_KEEPALIVE to detect dead connections over VPN
    int keepalive = 1;
    if (setsockopt(acceptSocket, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive, sizeof(keepalive)) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Warning: Failed to set SO_KEEPALIVE on accepted socket: " << WSAGetLastError() << std::endl;
    }

    // Associate accepted socket with IOCP
    if (CreateIoCompletionPort((HANDLE)acceptSocket, m_iocpHandle, (ULONG_PTR)acceptSocket, 0) == NULL)
    {
        std::cerr << "[P2P] Failed to associate accepted socket with IOCP: " << GetLastError() << std::endl;
        closesocket(acceptSocket);
        PostAccept(); // Post new accept
        return;
    }

    // Send HELLO message
    SendHello(acceptSocket);

    // Start receiving from this socket
    PostReceive(acceptSocket);

    // Post new accept
    PostAccept();
}

// Handle receive completion
void P2PManager::HandleReceive(IOContext* context, DWORD bytesTransferred)
{
    SOCKET socket = context->socket;

    // Validate socket is still active
    if (socket == INVALID_SOCKET)
    {
        std::cerr << "[P2P] HandleReceive: Invalid socket" << std::endl;
        return;
    }

    if (bytesTransferred == 0)
    {
        // Connection closed
        std::cout << "[P2P] Connection closed by peer" << std::endl;
        CloseSocket(socket);
        return;
    }

    // Flag for closing socket (to avoid deadlock)
    bool shouldClose = false;
    std::string closeReason;

    // Append received data to buffer
    {
        std::lock_guard<std::mutex> lock(m_buffersMutex);
        auto& buffer = m_receiveBuffers[socket];
        buffer.insert(buffer.end(), context->buffer.begin(), context->buffer.begin() + bytesTransferred);

        // Process complete messages (length-prefixed)
        while (buffer.size() >= 4)
        {
            // Read message length (first 4 bytes, network byte order)
            uint32_t messageLength = 0;
            memcpy(&messageLength, buffer.data(), 4);
            messageLength = ntohl(messageLength);

            // Sanity check: reject unreasonably large messages (> 10MB)
            if (messageLength > 10 * 1024 * 1024)
            {
                std::cerr << "[P2P] ERROR: Invalid message length " << messageLength << " bytes, closing connection" << std::endl;
                shouldClose = true;
                closeReason = "invalid message length";
                break;
            }

            // Check for zero-length message (malformed)
            if (messageLength == 0)
            {
                // Track consecutive zero-length messages
                m_zeroLengthMessageCount[socket]++;

                std::cerr << "[P2P] WARNING: Received zero-length message (count: "
                          << m_zeroLengthMessageCount[socket] << "), skipping frame" << std::endl;

                // If we've received too many consecutive zero-length messages, the connection is broken
                if (m_zeroLengthMessageCount[socket] >= 10)
                {
                    std::cerr << "[P2P] ERROR: Too many consecutive zero-length messages, closing connection" << std::endl;
                    std::cerr << "[P2P] Buffer state - size: " << buffer.size() << " bytes" << std::endl;

                    // Print first 64 bytes of buffer for debugging
                    if (buffer.size() > 0)
                    {
                        size_t previewSize = (buffer.size() < 64) ? buffer.size() : 64;
                        std::cerr << "[P2P] Buffer preview (first " << previewSize << " bytes):";
                        for (size_t i = 0; i < previewSize; i++)
                        {
                            std::cerr << " " << std::hex << std::setw(2) << std::setfill('0')
                                      << (int)(unsigned char)buffer[i];
                        }
                        std::cerr << std::dec << std::endl;
                    }

                    shouldClose = true;
                    closeReason = "too many zero-length messages";
                    break;
                }

                buffer.erase(buffer.begin(), buffer.begin() + 4);
                continue;
            }

            // Reset zero-length counter on valid message
            m_zeroLengthMessageCount[socket] = 0;

            // Check if we have the complete message
            if (buffer.size() >= 4 + messageLength)
            {
                // Extract message data
                std::vector<char> messageData(buffer.begin() + 4, buffer.begin() + 4 + messageLength);

                // Process message
                ProcessMessage(socket, messageData);

                // Remove processed message from buffer
                buffer.erase(buffer.begin(), buffer.begin() + 4 + messageLength);
            }
            else
            {
                // Incomplete message, wait for more data
                break;
            }
        }
    } // Release lock before closing socket

    // Close socket if needed (outside of lock to prevent deadlock)
    if (shouldClose)
    {
        std::cerr << "[P2P] Closing socket due to: " << closeReason << std::endl;
        CloseSocket(socket);
        return;
    }

    // Continue receiving
    PostReceive(socket);
}

// Handle send completion
void P2PManager::HandleSend(IOContext* context, DWORD bytesTransferred)
{
    SOCKET socket = context->socket;
    context->processedBytes += bytesTransferred;

    // Check if all data was sent
    if (context->processedBytes < context->totalBytes)
    {
        // Partial send - need to send remaining data
        size_t remaining = context->totalBytes - context->processedBytes;
        std::cerr << "[P2P] Partial send: sent " << context->processedBytes << "/"
                  << context->totalBytes << " bytes, " << remaining << " remaining" << std::endl;

        // Reset overlapped structure for reuse
        ZeroMemory(&context->overlapped, sizeof(OVERLAPPED));

        // Update buffer pointers for remaining data
        context->wsaBuf.buf = context->buffer.data() + context->processedBytes;
        context->wsaBuf.len = static_cast<ULONG>(remaining);

        // Re-post send for remaining data
        DWORD bytesSent = 0;
        int result = WSASend(socket, &context->wsaBuf, 1, &bytesSent, 0, &context->overlapped, NULL);
        if (result == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error != WSA_IO_PENDING)
            {
                std::cerr << "[P2P] WSASend failed on partial send retry: " << error << std::endl;
                CloseSocket(socket);
                return;
            }
        }
        // Note: Context is kept alive by m_activeContexts until completion
    }
    else
    {
        // Send completed successfully - all bytes sent
        // Context will be automatically cleaned up when it goes out of scope
    }
}

// Process received message
void P2PManager::ProcessMessage(SOCKET socket, const std::vector<char>& messageData)
{
    try
    {
        // Check for empty message
        if (messageData.empty())
        {
            std::cerr << "[P2P] WARNING: Received empty message, ignoring" << std::endl;
            return;
        }

        // Parse JSON message
        std::string jsonStr(messageData.begin(), messageData.end());

        // Check for empty JSON string
        if (jsonStr.empty())
        {
            std::cerr << "[P2P] WARNING: Empty JSON string, ignoring" << std::endl;
            return;
        }

        json message = json::parse(jsonStr);

        uint32_t typeValue = message.value("type", 0);
        P2PMessageType type = static_cast<P2PMessageType>(typeValue);
        json payload = message.value("payload", json::object());

        // Handle based on message type
        switch (type)
        {
        case P2PMessageType::HELLO:
            OnHelloReceived(socket, payload);
            break;
        case P2PMessageType::CHANGE_NOTIFY:
            OnChangeNotifyReceived(socket, payload);
            break;
        case P2PMessageType::PING:
            OnPingReceived(socket, payload);
            break;
        case P2PMessageType::PONG:
            OnPongReceived(socket, payload);
            break;
        case P2PMessageType::GOODBYE:
            OnGoodbyeReceived(socket, payload);
            break;
        default:
            std::cerr << "[P2P] Unknown message type: " << typeValue << std::endl;
            break;
        }
    }
    catch (const json::parse_error& e)
    {
        std::cerr << "[P2P] JSON parse error: " << e.what() << std::endl;
        std::cerr << "[P2P] Message size: " << messageData.size() << " bytes" << std::endl;
        if (!messageData.empty() && messageData.size() < 1000)
        {
            std::string preview(messageData.begin(), messageData.end());
            std::cerr << "[P2P] Message content: " << preview << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[P2P] Failed to process message: " << e.what() << std::endl;
    }
}

// Create a framed message (length-prefix + JSON)
std::vector<char> P2PManager::CreateMessage(P2PMessageType type, const json& payload)
{
    // Create message JSON
    json message = {
        {"type", static_cast<uint32_t>(type)},
        {"payload", payload}
    };

    std::string jsonStr = message.dump();

    // Create framed message: [4-byte length][JSON data]
    uint32_t messageLength = static_cast<uint32_t>(jsonStr.size());
    uint32_t networkLength = htonl(messageLength);

    std::vector<char> framedMessage(4 + jsonStr.size());
    memcpy(framedMessage.data(), &networkLength, 4);
    memcpy(framedMessage.data() + 4, jsonStr.data(), jsonStr.size());

    return framedMessage;
}

// Send a message to a peer
void P2PManager::SendMessage(SOCKET socket, P2PMessageType type, const json& payload)
{
    std::vector<char> message = CreateMessage(type, payload);
    PostSend(socket, message);
}

// Send HELLO message
void P2PManager::SendHello(SOCKET socket)
{
    json payload = {
        {"deviceId", std::string(m_deviceId.begin(), m_deviceId.end())},
        {"deviceName", std::string(m_deviceName.begin(), m_deviceName.end())},
        {"port", m_listeningPort},
        {"timestamp", GetCurrentTimestamp()}
    };

    SendMessage(socket, P2PMessageType::HELLO, payload);
    std::cout << "[P2P] Sent HELLO to peer" << std::endl;
}

// Send change notification
void P2PManager::SendChangeNotify(SOCKET socket, const std::wstring& jobPath, uint64_t timestamp)
{
    std::string jobPathStr(jobPath.begin(), jobPath.end());

    json payload = {
        {"jobPath", jobPathStr},
        {"deviceId", std::string(m_deviceId.begin(), m_deviceId.end())},
        {"timestamp", timestamp}  // Use exact timestamp from change log entry
    };

    SendMessage(socket, P2PMessageType::CHANGE_NOTIFY, payload);
}

// Send PING
void P2PManager::SendPing(SOCKET socket)
{
    json payload = {
        {"timestamp", GetCurrentTimestamp()}
    };

    SendMessage(socket, P2PMessageType::PING, payload);
}

// Handle HELLO received
void P2PManager::OnHelloReceived(SOCKET socket, const json& payload)
{
    try
    {
        std::string deviceIdStr = payload.value("deviceId", "");
        std::string deviceNameStr = payload.value("deviceName", "");
        uint16_t port = payload.value("port", 0);

        std::wstring deviceId(deviceIdStr.begin(), deviceIdStr.end());
        std::wstring deviceName(deviceNameStr.begin(), deviceNameStr.end());

        std::wcout << L"[P2P] Received HELLO from " << deviceName << L" (" << deviceId << L")" << std::endl;

        // Get peer IP address from the actual socket connection
        sockaddr_in peerAddr;
        int peerAddrLen = sizeof(peerAddr);
        if (getpeername(socket, (sockaddr*)&peerAddr, &peerAddrLen) == 0)
        {
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peerAddr.sin_addr, ipStr, INET_ADDRSTRLEN);

            // Determine if this is a new connection (peer was not previously active)
            bool wasInactive = false;
            {
                std::lock_guard<std::mutex> lock(m_peersMutex);

                // Check if we already have this peer's info from peers.json
                auto existingPeer = m_peers.find(deviceId);
                PeerInfo peer;

                if (existingPeer != m_peers.end())
                {
                    // Keep existing IPs but ensure the connected IP is first
                    peer = existingPeer->second;
                    wasInactive = !peer.isActive;  // Track if peer was previously inactive

                    // Remove the connected IP if it's already in the list
                    auto it = std::find(peer.ipAddresses.begin(), peer.ipAddresses.end(), ipStr);
                    if (it != peer.ipAddresses.end())
                    {
                        peer.ipAddresses.erase(it);
                    }

                    // Add connected IP at the front (highest priority)
                    peer.ipAddresses.insert(peer.ipAddresses.begin(), ipStr);
                }
                else
                {
                    // New peer - create entry with connected IP
                    peer.deviceId = deviceId;
                    peer.deviceName = deviceName;
                    peer.ipAddresses.push_back(ipStr);
                    wasInactive = true;  // Brand new peer
                }

                peer.port = port;
                peer.lastSeen = GetCurrentTimestamp();
                peer.isActive = true;

                m_peers[deviceId] = peer;
                m_socketToPeer[socket] = deviceId;
                m_peerToSocket[deviceId] = socket;

                std::wcout << L"[P2P] Registered peer: " << deviceName << L" at " << std::wstring(ipStr, ipStr + strlen(ipStr)).c_str() << L":" << port << std::endl;
            }

            // Trigger peer connected callback if this is a new/reconnected peer
            // Call outside of mutex to avoid deadlock
            if (wasInactive)
            {
                std::function<void(const std::wstring&, const std::wstring&)> callback;
                {
                    std::lock_guard<std::mutex> lock(m_callbackMutex);
                    callback = m_peerConnectedCallback;
                }

                if (callback)
                {
                    try
                    {
                        std::wcout << L"[P2P] Triggering peer connected callback for: " << deviceName << std::endl;
                        callback(deviceId, deviceName);
                    }
                    catch (const std::exception& e)
                    {
                        std::cerr << "[P2P] Exception in peer connected callback: " << e.what() << std::endl;
                    }
                    catch (...)
                    {
                        std::cerr << "[P2P] Unknown exception in peer connected callback" << std::endl;
                    }
                }
            }
        }

        // Don't send HELLO back - we already sent it when accepting or connecting
        // Sending another HELLO would create an infinite loop
    }
    catch (const std::exception& e)
    {
        std::cerr << "[P2P] Error handling HELLO: " << e.what() << std::endl;
    }
}

// Handle CHANGE_NOTIFY received
void P2PManager::OnChangeNotifyReceived(SOCKET socket, const json& payload)
{
    try
    {
        // Validate payload
        if (!payload.contains("jobPath") || !payload.contains("deviceId") || !payload.contains("timestamp"))
        {
            std::cerr << "[P2P] ERROR: CHANGE_NOTIFY missing required fields" << std::endl;
            return;
        }

        std::string jobPathStr = payload.value("jobPath", "");
        std::string peerDeviceIdStr = payload.value("deviceId", "");
        uint64_t timestamp = payload.value("timestamp", uint64_t(0));

        if (jobPathStr.empty() || peerDeviceIdStr.empty() || timestamp == 0)
        {
            std::cerr << "[P2P] ERROR: CHANGE_NOTIFY has empty fields" << std::endl;
            return;
        }

        std::wstring jobPath(jobPathStr.begin(), jobPathStr.end());
        std::wstring peerDeviceId(peerDeviceIdStr.begin(), peerDeviceIdStr.end());

        std::wcout << L"[P2P] Received CHANGE_NOTIFY for job: " << jobPath << L" from device: " << peerDeviceId
                   << L" timestamp: " << timestamp << std::endl;

        // Trigger change callback (copy callback to avoid holding lock during call)
        std::function<void(const std::wstring&, const std::wstring&, uint64_t)> callback;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            callback = m_changeCallback;
        }

        // Call callback outside of lock to prevent deadlock
        if (callback)
        {
            try
            {
                callback(jobPath, peerDeviceId, timestamp);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[P2P] Exception in change callback: " << e.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[P2P] Unknown exception in change callback" << std::endl;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[P2P] Error handling CHANGE_NOTIFY: " << e.what() << std::endl;
    }
}

// Handle PING received
void P2PManager::OnPingReceived(SOCKET socket, const json& payload)
{
    // Send PONG response
    json pongPayload = {
        {"timestamp", GetCurrentTimestamp()}
    };

    SendMessage(socket, P2PMessageType::PONG, pongPayload);
}

// Handle PONG received
void P2PManager::OnPongReceived(SOCKET socket, const json& payload)
{
    // Update last seen time for this peer
    std::lock_guard<std::mutex> lock(m_peersMutex);
    auto it = m_socketToPeer.find(socket);
    if (it != m_socketToPeer.end())
    {
        auto peerIt = m_peers.find(it->second);
        if (peerIt != m_peers.end())
        {
            peerIt->second.lastSeen = GetCurrentTimestamp();
        }
    }
}

// Handle GOODBYE received
void P2PManager::OnGoodbyeReceived(SOCKET socket, const json& payload)
{
    std::string deviceIdStr = payload.value("deviceId", "");
    std::wstring deviceId(deviceIdStr.begin(), deviceIdStr.end());

    std::wcout << L"[P2P] Received GOODBYE from " << deviceId << L", closing connection" << std::endl;

    // Close the socket gracefully
    CloseSocket(socket);
}

// Notify all peers of a change
void P2PManager::NotifyPeersOfChange(const std::wstring& jobPath, uint64_t timestamp)
{
    std::lock_guard<std::mutex> lock(m_peersMutex);

    std::wcout << L"[P2P] Notifying " << m_peerToSocket.size() << L" peers of change to: " << jobPath
               << L" (timestamp: " << timestamp << L")" << std::endl;

    for (auto& [deviceId, socket] : m_peerToSocket)
    {
        SendChangeNotify(socket, jobPath, timestamp);
    }
}

// Register change callback
void P2PManager::RegisterChangeCallback(std::function<void(const std::wstring&, const std::wstring&, uint64_t)> callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_changeCallback = callback;
}

// Register peer connected callback
void P2PManager::RegisterPeerConnectedCallback(std::function<void(const std::wstring&, const std::wstring&)> callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_peerConnectedCallback = callback;
}

// Heartbeat thread
void P2PManager::HeartbeatThread()
{
    std::cout << "[P2P] Heartbeat thread started" << std::endl;

    int heartbeatCounter = 0;

    while (m_isRunning)
    {
        // Update peer registry (read peers.json, connect to new peers)
        UpdatePeerRegistry();

        // Send PING to all connected peers
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            for (auto& [deviceId, socket] : m_peerToSocket)
            {
                SendPing(socket);
            }
        }

        // Cleanup stale peers (not seen in 60 seconds)
        CleanupStalePeers();

        // Update our entry in peers.json
        WritePeerRegistry();

        // Cleanup stale peer files every 10 minutes (20 heartbeat cycles)
        // This prevents accumulation of old peer files on disk
        heartbeatCounter++;
        if (heartbeatCounter >= 20)
        {
            CleanupStalePeerFiles();
            heartbeatCounter = 0;
        }

        // Sleep for 30 seconds
        for (int i = 0; i < 30 && m_isRunning; i++)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::cout << "[P2P] Heartbeat thread stopped" << std::endl;
}

// Load peers from ALL subscribed projects (aggregate peer discovery)
void P2PManager::LoadPeersFromFile()
{
    // Get copy of subscribed projects to avoid holding lock during file I/O
    std::vector<std::wstring> projects = GetSubscribedProjects();

    if (projects.empty())
    {
        // No projects subscribed yet
        return;
    }

    int loadedCount = 0;
    int skippedCount = 0;
    int unchangedCount = 0;
    int networkErrorCount = 0;

    // Aggregate peers from all subscribed projects
    for (const auto& projectPath : projects)
    {
        std::filesystem::path peersDir = std::filesystem::path(projectPath) / L".ufb" / L"peers";

        // Check if peers directory exists (with network error handling)
        try
        {
            if (!std::filesystem::exists(peersDir))
            {
                // Project doesn't have peers directory yet
                continue;
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            // Network share might be unavailable
            std::wcerr << L"[P2P] Cannot access peers directory (network issue?): " << peersDir.wstring()
                       << L" - " << e.what() << std::endl;
            networkErrorCount++;
            continue;  // Use cached peer data
        }

        // Read all .json files in this project's peers directory
        try
        {
            for (const auto& entry : std::filesystem::directory_iterator(peersDir))
            {
                // Skip temp files
                if (entry.path().extension() == L".tmp")
                {
                    continue;
                }

                if (!entry.is_regular_file() || entry.path().extension() != L".json")
                {
                    continue;
                }

                // Check file timestamp - only read if changed
                try
                {
                    auto lastWriteTime = std::filesystem::last_write_time(entry);
                    std::wstring filePath = entry.path().wstring();

                    // Check timestamp cache
                    {
                        std::lock_guard<std::mutex> timestampLock(m_fileTimestampMutex);
                        auto cachedTime = m_peerFileTimestamps.find(filePath);

                        if (cachedTime != m_peerFileTimestamps.end() && cachedTime->second == lastWriteTime)
                        {
                            // File hasn't changed - skip reading
                            unchangedCount++;
                            continue;
                        }
                    }

                    // File is new or changed - read it
                    std::ifstream file(entry.path(), std::ios::in);
                    if (!file.is_open())
                    {
                        // File is locked - skip it and use cached peer data
                        skippedCount++;
                        continue;
                    }

                    // Read JSON
                    json peerJson;
                    file >> peerJson;
                    file.close();

                    PeerInfo peer = PeerInfo::FromJson(peerJson);

                    // Don't add ourselves to the peer list
                    if (peer.deviceId != m_deviceId)
                    {
                        // Update peer info
                        {
                            std::lock_guard<std::mutex> lock(m_peersMutex);

                            // Deduplicate: if peer already exists, keep the one with latest lastSeen
                            auto existingPeer = m_peers.find(peer.deviceId);
                            if (existingPeer == m_peers.end() || peer.lastSeen > existingPeer->second.lastSeen)
                            {
                                m_peers[peer.deviceId] = peer;
                                loadedCount++;
                            }
                        }

                        // Update timestamp cache
                        {
                            std::lock_guard<std::mutex> timestampLock(m_fileTimestampMutex);
                            m_peerFileTimestamps[filePath] = lastWriteTime;
                        }
                    }
                }
                catch (const std::filesystem::filesystem_error& e)
                {
                    // File access error (might be locked or network issue)
                    skippedCount++;
                }
                catch (const std::exception& e)
                {
                    // JSON parse error or other issue - skip this file
                    std::cerr << "[P2P] Failed to parse peer file " << entry.path() << ": " << e.what() << std::endl;
                    skippedCount++;
                }
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            // Directory iteration failed (network issue?)
            std::wcerr << L"[P2P] Cannot iterate peers directory (network issue?): " << peersDir.wstring()
                       << L" - " << e.what() << std::endl;
            networkErrorCount++;
            continue;  // Use cached peer data
        }
    }

    // Log results
    if (loadedCount > 0 || skippedCount > 0 || unchangedCount > 0)
    {
        std::cout << "[P2P] Peer file scan: loaded=" << loadedCount
                  << " unchanged=" << unchangedCount
                  << " skipped=" << skippedCount;
        if (networkErrorCount > 0)
        {
            std::cout << " network_errors=" << networkErrorCount << " (using cached peers)";
        }
        std::cout << std::endl;
    }
}

// Save peers to per-device file to avoid file contention
// Write our peer info to ALL subscribed projects
void P2PManager::SavePeersToFile()
{
    // Get copy of subscribed projects
    std::vector<std::wstring> projects = GetSubscribedProjects();

    if (projects.empty())
    {
        // No projects subscribed yet
        return;
    }

    try
    {
        // Get current state
        std::vector<std::string> currentIPs = GetAllLocalIPs();
        uint16_t currentPort = m_listeningPort;

        // Check if data has changed since last write
        bool hasChanged = false;

        // Check if port changed
        if (currentPort != m_lastWrittenPort)
        {
            hasChanged = true;
        }

        // Check if IPs changed (order doesn't matter)
        if (!hasChanged && currentIPs.size() != m_lastWrittenIPs.size())
        {
            hasChanged = true;
        }

        if (!hasChanged)
        {
            // Check if IP addresses are different (sort both for comparison)
            std::vector<std::string> sortedCurrent = currentIPs;
            std::vector<std::string> sortedLast = m_lastWrittenIPs;
            std::sort(sortedCurrent.begin(), sortedCurrent.end());
            std::sort(sortedLast.begin(), sortedLast.end());

            if (sortedCurrent != sortedLast)
            {
                hasChanged = true;
            }
        }

        // Only write if something changed
        if (!hasChanged)
        {
            // No changes - skip write
            return;
        }

        // Prepare our peer info
        PeerInfo ourInfo;
        ourInfo.deviceId = m_deviceId;
        ourInfo.deviceName = m_deviceName;
        ourInfo.ipAddresses = currentIPs;
        ourInfo.port = currentPort;
        ourInfo.lastSeen = GetCurrentTimestamp();
        ourInfo.isActive = true;

        // Use per-device file to avoid write contention between peers
        // Note: Device IDs (GUIDs) are already filename-safe on Windows
        int successCount = 0;
        int failCount = 0;

        // Write to ALL subscribed projects
        for (const auto& projectPath : projects)
        {
            std::filesystem::path ufbDir = std::filesystem::path(projectPath) / L".ufb" / L"peers";

            // Ensure peers directory exists
            try
            {
                if (!std::filesystem::exists(ufbDir))
                {
                    std::filesystem::create_directories(ufbDir);
                }
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                std::cerr << "[P2P] Failed to create peers directory (network issue?): " << e.what() << std::endl;
                failCount++;
                continue;  // Skip this project
            }

            std::filesystem::path ourPeerFile = ufbDir / (m_deviceId + L".json");
            std::filesystem::path tempFile = ufbDir / (m_deviceId + L".json.tmp");

            // Atomic write: write to temp file, then rename
            try
            {
                {
                    std::ofstream file(tempFile);
                    if (!file.is_open())
                    {
                        std::wcerr << L"[P2P] Failed to open temp file for writing (network issue?): " << tempFile.wstring() << std::endl;
                        failCount++;
                        continue;
                    }
                    file << ourInfo.ToJson().dump(2);
                    file.close();

                    // Verify file was written successfully
                    if (file.fail())
                    {
                        std::wcerr << L"[P2P] Failed to write temp file (network issue?): " << tempFile.wstring() << std::endl;
                        failCount++;
                        continue;
                    }
                }

                // Atomic rename (overwrites existing file)
                std::filesystem::rename(tempFile, ourPeerFile);
                successCount++;
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                std::cerr << "[P2P] Filesystem error writing peer file (network issue?): " << e.what() << std::endl;
                failCount++;

                // Try to clean up temp file
                try { std::filesystem::remove(tempFile); } catch (...) {}
            }
            catch (const std::exception& e)
            {
                std::cerr << "[P2P] Error writing peer file: " << e.what() << std::endl;
                failCount++;

                // Try to clean up temp file
                try { std::filesystem::remove(tempFile); } catch (...) {}
            }
        }

        if (successCount > 0)
        {
            // Update last written state
            m_lastWrittenPort = currentPort;
            m_lastWrittenIPs = currentIPs;

            std::cout << "[P2P] Saved our peer info to " << successCount << " project(s)";
            if (failCount > 0)
            {
                std::cout << " (failed: " << failCount << ")";
            }
            std::cout << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[P2P] Failed to save peer info: " << e.what() << std::endl;
    }
}

// Update peer registry from file and connect to new peers
void P2PManager::UpdatePeerRegistry()
{
    LoadPeersFromFile();

    // Build a list of peers to connect to (without holding lock)
    std::vector<std::pair<std::wstring, PeerInfo>> peersToConnect;

    {
        std::lock_guard<std::mutex> lock(m_peersMutex);

        for (auto& [deviceId, peer] : m_peers)
        {
            if (!peer.isActive && m_peerToSocket.find(deviceId) == m_peerToSocket.end())
            {
                peersToConnect.push_back({deviceId, peer});
            }
        }
    }

    // Try to connect to each peer (without holding lock to avoid blocking)
    for (const auto& [deviceId, peer] : peersToConnect)
    {
        // Check if shutting down
        if (!m_isRunning)
        {
            std::cout << "[P2P] Shutdown detected, stopping peer connections" << std::endl;
            break;
        }

        // Try all IPs for this peer, stop on first success
        bool connected = false;
        for (const auto& ip : peer.ipAddresses)
        {
            // Check if shutting down before each connection attempt
            if (!m_isRunning)
            {
                break;
            }

            std::cout << "[P2P] Attempting to connect to peer "
                      << std::string(peer.deviceName.begin(), peer.deviceName.end())
                      << " at " << ip << ":" << peer.port << std::endl;

            if (ConnectToPeer(ip, peer.port))
            {
                std::cout << "[P2P] Successfully connected to " << ip << ":" << peer.port << std::endl;
                connected = true;
                break;  // Connected successfully, don't try other IPs
            }

            std::cout << "[P2P] Failed to connect to " << ip << ":" << peer.port << std::endl;
        }

        if (!connected && !peer.ipAddresses.empty() && m_isRunning)
        {
            std::cout << "[P2P] Could not connect to peer "
                      << std::string(peer.deviceName.begin(), peer.deviceName.end())
                      << " on any of " << peer.ipAddresses.size() << " IP address(es)" << std::endl;
        }
    }
}

// Write our peer info to peers.json
void P2PManager::WritePeerRegistry()
{
    SavePeersToFile();
}

// Cleanup stale peers
void P2PManager::CleanupStalePeers()
{
    uint64_t now = GetCurrentTimestamp();
    uint64_t staleThreshold = 60000; // 60 seconds

    std::lock_guard<std::mutex> lock(m_peersMutex);

    std::vector<std::wstring> stalePeers;

    for (auto& [deviceId, peer] : m_peers)
    {
        if (peer.isActive && (now - peer.lastSeen) > staleThreshold)
        {
            std::wcout << L"[P2P] Peer stale: " << peer.deviceName << std::endl;
            stalePeers.push_back(deviceId);
        }
    }

    // Remove stale peers
    for (const auto& deviceId : stalePeers)
    {
        RemovePeer(deviceId);
    }
}

// Cleanup stale peer files from disk
void P2PManager::CleanupStalePeerFiles()
{
    // Get copy of subscribed projects
    std::vector<std::wstring> projects = GetSubscribedProjects();

    if (projects.empty())
    {
        return;
    }

    uint64_t now = GetCurrentTimestamp();
    uint64_t staleFileThreshold = 7 * 24 * 60 * 60 * 1000; // 7 days in milliseconds

    int deletedCount = 0;
    int skippedCount = 0;

    for (const auto& projectPath : projects)
    {
        std::filesystem::path peersDir = std::filesystem::path(projectPath) / L".ufb" / L"peers";

        // Check if peers directory exists (with network error handling)
        try
        {
            if (!std::filesystem::exists(peersDir))
            {
                continue;
            }
        }
        catch (const std::filesystem::filesystem_error&)
        {
            // Network share unavailable - skip cleanup for this project
            continue;
        }

        // Scan peer files
        try
        {
            for (const auto& entry : std::filesystem::directory_iterator(peersDir))
            {
                if (!entry.is_regular_file() || entry.path().extension() != L".json")
                {
                    continue;
                }

                try
                {
                    // Read peer file to get lastSeen timestamp
                    std::ifstream file(entry.path());
                    if (!file.is_open())
                    {
                        // File locked - skip it
                        skippedCount++;
                        continue;
                    }

                    json peerJson;
                    file >> peerJson;
                    file.close();

                    uint64_t lastSeen = peerJson.value("lastSeen", 0ULL);

                    // Check if file is stale
                    if (lastSeen > 0 && (now - lastSeen) > staleFileThreshold)
                    {
                        std::wstring deviceId(peerJson.value("deviceId", "").begin(), peerJson.value("deviceId", "").end());

                        // Don't delete our own file
                        if (deviceId == m_deviceId)
                        {
                            continue;
                        }

                        // Delete stale peer file
                        std::wcout << L"[P2P] Deleting stale peer file (> 7 days): " << entry.path().filename().wstring() << std::endl;
                        std::filesystem::remove(entry.path());
                        deletedCount++;

                        // Remove from timestamp cache
                        {
                            std::lock_guard<std::mutex> lock(m_fileTimestampMutex);
                            m_peerFileTimestamps.erase(entry.path().wstring());
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    // Error reading/parsing file - skip it
                    std::cerr << "[P2P] Error checking peer file " << entry.path() << ": " << e.what() << std::endl;
                    skippedCount++;
                }
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            // Directory iteration failed - skip this project
            std::wcerr << L"[P2P] Cannot iterate peers directory: " << peersDir.wstring() << std::endl;
        }
    }

    if (deletedCount > 0)
    {
        std::cout << "[P2P] Deleted " << deletedCount << " stale peer file(s) (> 7 days old)" << std::endl;
    }
}

// Connect to a peer
bool P2PManager::ConnectToPeer(const std::string& ipAddress, uint16_t port)
{
    // Check if shutting down
    if (!m_isRunning)
    {
        return false;
    }

    // Create socket
    SOCKET socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET)
    {
        std::cerr << "[P2P] Failed to create connect socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    // Set non-blocking mode for connect timeout
    u_long nonBlocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Failed to set non-blocking mode: " << WSAGetLastError() << std::endl;
        closesocket(socket);
        return false;
    }

    // Set TCP_NODELAY to disable Nagle's algorithm (critical for VPNs and real-time)
    int nodelay = 1;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay)) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Warning: Failed to set TCP_NODELAY: " << WSAGetLastError() << std::endl;
    }

    // Set SO_KEEPALIVE to detect dead connections over VPN
    int keepalive = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive, sizeof(keepalive)) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Warning: Failed to set SO_KEEPALIVE: " << WSAGetLastError() << std::endl;
    }

    // Connect to peer (non-blocking)
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ipAddress.c_str(), &addr.sin_addr);

    int result = connect(socket, (sockaddr*)&addr, sizeof(addr));
    if (result == SOCKET_ERROR)
    {
        int error = WSAGetLastError();

        // WSAEWOULDBLOCK is expected for non-blocking connect
        if (error != WSAEWOULDBLOCK)
        {
            std::cerr << "[P2P] Failed to initiate connection to " << ipAddress << ":" << port << " - " << error << std::endl;
            closesocket(socket);
            return false;
        }

        // Wait for connection to complete (with 3 second timeout)
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socket, &writeSet);

        timeval timeout;
        timeout.tv_sec = 3;  // 3 second timeout instead of 20-30
        timeout.tv_usec = 0;

        result = select(0, nullptr, &writeSet, nullptr, &timeout);
        if (result == 0)
        {
            // Timeout
            std::cerr << "[P2P] Connection to " << ipAddress << ":" << port << " timed out (firewall/unreachable)" << std::endl;
            closesocket(socket);
            return false;
        }
        else if (result == SOCKET_ERROR)
        {
            std::cerr << "[P2P] select() failed for " << ipAddress << ":" << port << " - " << WSAGetLastError() << std::endl;
            closesocket(socket);
            return false;
        }

        // Check if connection succeeded or failed
        int soError = 0;
        int soErrorLen = sizeof(soError);
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR, (char*)&soError, &soErrorLen) == SOCKET_ERROR)
        {
            std::cerr << "[P2P] getsockopt() failed for " << ipAddress << ":" << port << " - " << WSAGetLastError() << std::endl;
            closesocket(socket);
            return false;
        }

        if (soError != 0)
        {
            std::cerr << "[P2P] Connection to " << ipAddress << ":" << port << " failed - " << soError << std::endl;
            closesocket(socket);
            return false;
        }
    }

    // Set back to blocking mode for IOCP operations
    nonBlocking = 0;
    if (ioctlsocket(socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        std::cerr << "[P2P] Failed to set blocking mode: " << WSAGetLastError() << std::endl;
        closesocket(socket);
        return false;
    }

    std::cout << "[P2P] Connected to " << ipAddress << ":" << port << std::endl;

    // Associate with IOCP
    if (CreateIoCompletionPort((HANDLE)socket, m_iocpHandle, (ULONG_PTR)socket, 0) == NULL)
    {
        std::cerr << "[P2P] Failed to associate connect socket with IOCP: " << GetLastError() << std::endl;
        closesocket(socket);
        return false;
    }

    // Send HELLO
    SendHello(socket);

    // Start receiving
    PostReceive(socket);

    return true;
}

// Close socket and cleanup
void P2PManager::CloseSocket(SOCKET socket)
{
    if (socket == INVALID_SOCKET)
        return;

    std::lock_guard<std::mutex> lock(m_peersMutex);

    // Find peer associated with this socket
    auto it = m_socketToPeer.find(socket);
    if (it != m_socketToPeer.end())
    {
        std::wstring deviceId = it->second;

        // Mark peer as inactive
        auto peerIt = m_peers.find(deviceId);
        if (peerIt != m_peers.end())
        {
            peerIt->second.isActive = false;
        }

        // Remove mappings
        m_peerToSocket.erase(deviceId);
        m_socketToPeer.erase(socket);
    }

    // Cancel all pending I/O operations on this socket
    // This ensures IOCP won't complete operations after we close the socket
    CancelIoEx((HANDLE)socket, NULL);

    // Close socket
    closesocket(socket);

    // Remove from receive buffers and counters
    {
        std::lock_guard<std::mutex> lock(m_buffersMutex);
        m_receiveBuffers.erase(socket);
        m_zeroLengthMessageCount.erase(socket);
    }

    // Remove all contexts for this socket from the list
    {
        std::lock_guard<std::mutex> lock(m_contextsMutex);
        m_activeContexts.remove_if([socket](const std::shared_ptr<IOContext>& ctx) {
            return ctx->socket == socket;
        });
    }
}

// Remove peer
void P2PManager::RemovePeer(const std::wstring& deviceId)
{
    // Find socket
    auto socketIt = m_peerToSocket.find(deviceId);
    if (socketIt != m_peerToSocket.end())
    {
        SOCKET socket = socketIt->second;
        CloseSocket(socket);
    }

    // Remove from peers map
    m_peers.erase(deviceId);
}

// Disconnect peer
void P2PManager::DisconnectPeer(const std::wstring& deviceId)
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    RemovePeer(deviceId);
}

// Get active peers
std::vector<PeerInfo> P2PManager::GetActivePeers() const
{
    std::lock_guard<std::mutex> lock(m_peersMutex);

    std::vector<PeerInfo> activePeers;
    for (const auto& [deviceId, peer] : m_peers)
    {
        if (peer.isActive)
        {
            activePeers.push_back(peer);
        }
    }

    return activePeers;
}

// Get peer count
size_t P2PManager::GetPeerCount() const
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    return m_peerToSocket.size();
}

} // namespace UFB
