#pragma once

// Define WIN32_LEAN_AND_MEAN to prevent windows.h from including winsock.h (old API)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// IMPORTANT: WinSock2 must be included before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>  // For AcceptEx and other extension functions
#include <windows.h>
#include <string>
#include <vector>
#include <list>
#include <map>
#include <set>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <filesystem>
#include "nlohmann/json.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace UFB {

// P2P Message Types
enum class P2PMessageType : uint32_t
{
    HELLO = 1,              // Initial handshake with peer info
    CHANGE_NOTIFY = 2,      // Notify peers of metadata change
    SYNC_REQUEST = 3,       // Request full sync of change logs
    SYNC_RESPONSE = 4,      // Response with change log data
    PING = 5,               // Keepalive ping
    PONG = 6,               // Keepalive response
    GOODBYE = 7             // Clean disconnect notification
};

// Peer information
struct PeerInfo
{
    std::wstring deviceId;           // Unique device identifier
    std::wstring deviceName;         // Human-readable device name
    std::vector<std::string> ipAddresses;  // Multiple IPv4 addresses (LAN, VPN, etc.)
    uint16_t port;                   // TCP port
    uint64_t lastSeen;               // Timestamp (milliseconds since epoch)
    bool isActive;                   // Currently connected

    nlohmann::json ToJson() const;
    static PeerInfo FromJson(const nlohmann::json& j);
};

// I/O Operation types for IOCP
enum class IOOperation
{
    Accept,
    Receive,
    Send,
    Connect
};

// Overlapped structure for async I/O
struct IOContext
{
    OVERLAPPED overlapped;
    IOOperation operation;
    SOCKET socket;
    WSABUF wsaBuf;
    std::vector<char> buffer;
    size_t totalBytes;          // Total bytes to send/receive
    size_t processedBytes;      // Bytes already processed
    std::wstring peerDeviceId;  // Associated peer

    IOContext(IOOperation op, SOCKET s)
        : operation(op), socket(s), totalBytes(0), processedBytes(0)
    {
        ZeroMemory(&overlapped, sizeof(OVERLAPPED));
        buffer.resize(8192); // Default buffer size
        wsaBuf.buf = buffer.data();
        wsaBuf.len = static_cast<ULONG>(buffer.size());
    }
};

// P2P Manager - Handles peer-to-peer networking using WinSock2 + IOCP
class P2PManager
{
public:
    P2PManager();
    ~P2PManager();

    // Initialize P2P networking (global, not per-project)
    bool Initialize(const std::wstring& deviceId);

    // Shutdown P2P networking
    void Shutdown();

    // Start listening for incoming connections
    bool StartListening(uint16_t preferredPort = 0); // 0 = auto-select

    // Get the port we're listening on
    uint16_t GetListeningPort() const { return m_listeningPort; }

    // Project subscription management
    void SubscribeToProject(const std::wstring& projectPath);
    void UnsubscribeFromProject(const std::wstring& projectPath);
    std::vector<std::wstring> GetSubscribedProjects() const;

    // Send a change notification to all active peers
    void NotifyPeersOfChange(const std::wstring& jobPath, uint64_t timestamp);

    // Register callback for when remote changes are detected
    void RegisterChangeCallback(std::function<void(const std::wstring& jobPath, const std::wstring& peerDeviceId, uint64_t timestamp)> callback);

    // Register callback for when a peer successfully connects (handshake complete)
    void RegisterPeerConnectedCallback(std::function<void(const std::wstring& peerDeviceId, const std::wstring& peerDeviceName)> callback);

    // Peer discovery and management
    void UpdatePeerRegistry();          // Read peers.json from all subscribed projects and connect to new peers
    void WritePeerRegistry();           // Write our info to peers.json in all subscribed projects
    std::vector<PeerInfo> GetActivePeers() const;

    // Connection management
    bool ConnectToPeer(const std::string& ipAddress, uint16_t port);
    void DisconnectPeer(const std::wstring& deviceId);

    // Get statistics
    size_t GetPeerCount() const;
    bool IsRunning() const { return m_isRunning; }

private:
    // Core networking
    SOCKET m_listenSocket;
    HANDLE m_iocpHandle;
    uint16_t m_listeningPort;
    std::atomic<bool> m_isRunning;

    // Global context (supports multiple projects)
    std::set<std::wstring> m_subscribedProjects;  // Projects we're syncing
    mutable std::mutex m_projectsMutex;
    std::wstring m_deviceId;
    std::wstring m_deviceName;

    // Peer management
    std::map<std::wstring, PeerInfo> m_peers;           // deviceId -> PeerInfo
    std::map<SOCKET, std::wstring> m_socketToPeer;      // socket -> deviceId
    std::map<std::wstring, SOCKET> m_peerToSocket;      // deviceId -> socket
    mutable std::mutex m_peersMutex;

    // Peer file state tracking (to avoid unnecessary writes)
    uint16_t m_lastWrittenPort;
    std::vector<std::string> m_lastWrittenIPs;

    // IP address caching (avoid expensive enumeration every 30s)
    std::vector<std::string> m_cachedIPs;
    uint64_t m_lastIPRefresh;
    std::mutex m_ipCacheMutex;

    // File timestamp caching (avoid re-reading unchanged peer files)
    std::map<std::wstring, std::filesystem::file_time_type> m_peerFileTimestamps;
    std::mutex m_fileTimestampMutex;

    // I/O contexts - use list to allow multiple concurrent operations per socket
    std::list<std::shared_ptr<IOContext>> m_activeContexts;
    std::mutex m_contextsMutex;

    // Message handling
    std::map<SOCKET, std::vector<char>> m_receiveBuffers; // Partial message buffers
    std::map<SOCKET, int> m_zeroLengthMessageCount; // Track consecutive zero-length messages per socket
    std::mutex m_buffersMutex;

    // Callbacks
    std::function<void(const std::wstring& jobPath, const std::wstring& peerDeviceId, uint64_t timestamp)> m_changeCallback;
    std::function<void(const std::wstring& peerDeviceId, const std::wstring& peerDeviceName)> m_peerConnectedCallback;
    std::mutex m_callbackMutex;

    // Worker threads
    std::thread m_iocpWorkerThread;
    std::thread m_heartbeatThread;

    // Peer discovery
    void LoadPeersFromFile();
    void SavePeersToFile();
    void CleanupStalePeers();
    void CleanupStalePeerFiles();  // Delete old peer files from disk

    // IOCP worker
    void IOCPWorkerThread();

    // Async operations
    bool PostAccept();
    bool PostReceive(SOCKET socket);
    bool PostSend(SOCKET socket, const std::vector<char>& data);

    // Message handling
    void HandleAccept(IOContext* context);
    void HandleReceive(IOContext* context, DWORD bytesTransferred);
    void HandleSend(IOContext* context, DWORD bytesTransferred);
    void ProcessMessage(SOCKET socket, const std::vector<char>& messageData);

    // Protocol
    std::vector<char> CreateMessage(P2PMessageType type, const nlohmann::json& payload);
    void SendMessage(SOCKET socket, P2PMessageType type, const nlohmann::json& payload);
    void SendHello(SOCKET socket);
    void SendChangeNotify(SOCKET socket, const std::wstring& jobPath, uint64_t timestamp);
    void SendPing(SOCKET socket);

    // Message handlers
    void OnHelloReceived(SOCKET socket, const nlohmann::json& payload);
    void OnChangeNotifyReceived(SOCKET socket, const nlohmann::json& payload);
    void OnPingReceived(SOCKET socket, const nlohmann::json& payload);
    void OnPongReceived(SOCKET socket, const nlohmann::json& payload);
    void OnGoodbyeReceived(SOCKET socket, const nlohmann::json& payload);

    // Heartbeat
    void HeartbeatThread();

    // Cleanup
    void CloseSocket(SOCKET socket);
    void RemovePeer(const std::wstring& deviceId);

    // Utility
    std::wstring GetDeviceName();
    std::vector<std::string> GetAllLocalIPs();
    std::string GetLocalIPAddress();  // Deprecated - kept for backward compatibility
    uint64_t GetCurrentTimestamp();
    std::string SocketToString(SOCKET socket);
};

} // namespace UFB
