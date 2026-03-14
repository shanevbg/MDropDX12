#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")

enum class TcpAuthState { Unauthenticated, Pending, Authenticated };

struct TcpClientConnection {
    SOCKET socket = INVALID_SOCKET;
    TcpAuthState authState = TcpAuthState::Unauthenticated;
    std::string deviceId;
    std::string deviceName;
    ULONGLONG lastActivity = 0; // GetTickCount64
    std::vector<uint8_t> readBuffer;
    std::vector<uint8_t> writeBuffer; // Pending outgoing data for WOULDBLOCK handling
};

// Thread-local pointer to the TCP client that initiated the current command.
// Set before PostMessage dispatch, read by response-sending code to route
// replies to the requesting client instead of broadcasting.
extern thread_local TcpClientConnection* g_respondingTcpClient;

class TcpServer {
public:
    using MessageHandler = std::function<void(TcpClientConnection& client, const std::wstring& message)>;
    using AuthRequestHandler = std::function<void(TcpClientConnection& client, const std::string& pin,
                                                   const std::string& deviceId, const std::string& deviceName)>;

    TcpServer();
    ~TcpServer();

    bool Start(int port, MessageHandler onMessage, AuthRequestHandler onAuthRequest);
    void Stop();
    void Poll();  // Called from main loop — non-blocking select
    void Broadcast(const std::wstring& message);  // Send to all authenticated clients
    void SendTo(TcpClientConnection& client, const std::string& utf8Message);
    void SendTo(TcpClientConnection& client, const std::wstring& message);
    void ApproveDevice(const std::string& deviceId);
    void DenyDevice(const std::string& deviceId);
    void DisconnectDevice(const std::string& deviceId);
    bool IsRunning() const { return m_running.load(); }
    int GetPort() const { return m_port; }

private:
    void AcceptNewClients();
    void ReadFromClients();
    void ProcessFrames(TcpClientConnection& client);
    void RemoveClient(size_t index);
    void SendRaw(SOCKET sock, const uint8_t* data, int len);
    void CheckTimeouts();

    static std::string WideToUtf8(const std::wstring& wide);
    static std::wstring Utf8ToWide(const std::string& utf8);

    SOCKET m_listenSocket = INVALID_SOCKET;
    int m_port = 9270;
    std::atomic<bool> m_running{false};
    std::vector<TcpClientConnection> m_clients;
    std::mutex m_clientsMutex;
    MessageHandler m_onMessage;
    AuthRequestHandler m_onAuthRequest;

    static constexpr int RECV_BUFFER_SIZE = 65536;
    static constexpr ULONGLONG CLIENT_TIMEOUT_MS = 60000;
    static constexpr size_t MAX_CLIENTS = 16;
};
