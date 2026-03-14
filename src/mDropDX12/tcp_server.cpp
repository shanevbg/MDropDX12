#include "tcp_server.h"
#include <algorithm>

thread_local TcpClientConnection* g_respondingTcpClient = nullptr;

TcpServer::TcpServer() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

TcpServer::~TcpServer() {
    Stop();
}

bool TcpServer::Start(int port, MessageHandler onMessage, AuthRequestHandler onAuthRequest) {
    if (m_running.load()) return false;
    m_port = port;
    m_onMessage = std::move(onMessage);
    m_onAuthRequest = std::move(onAuthRequest);

    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) return false;

    int opt = 1;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    u_long mode = 1;
    ioctlsocket(m_listenSocket, FIONBIO, &mode);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(m_listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    m_running.store(true);
    return true;
}

void TcpServer::Stop() {
    m_running.store(false);
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (c.socket != INVALID_SOCKET) closesocket(c.socket);
    }
    m_clients.clear();
    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }
    WSACleanup();
}

void TcpServer::AcceptNewClients() {
    // Enforce max client limit
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        if (m_clients.size() >= MAX_CLIENTS) return;
    }

    sockaddr_in clientAddr{};
    int addrLen = sizeof(clientAddr);
    SOCKET clientSocket = accept(m_listenSocket, (sockaddr*)&clientAddr, &addrLen);
    if (clientSocket == INVALID_SOCKET) return;

    u_long mode = 1;
    ioctlsocket(clientSocket, FIONBIO, &mode);

    TcpClientConnection conn;
    conn.socket = clientSocket;
    conn.lastActivity = GetTickCount64();

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    m_clients.push_back(std::move(conn));
}

void TcpServer::ReadFromClients() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    uint8_t buf[RECV_BUFFER_SIZE];

    for (size_t i = 0; i < m_clients.size(); ) {
        auto& c = m_clients[i];
        int bytesRead = recv(c.socket, (char*)buf, RECV_BUFFER_SIZE, 0);

        if (bytesRead > 0) {
            c.lastActivity = GetTickCount64();
            c.readBuffer.insert(c.readBuffer.end(), buf, buf + bytesRead);
            ProcessFrames(c);
            ++i;
        } else if (bytesRead == 0) {
            RemoveClient(i);
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                ++i;
            } else {
                RemoveClient(i);
            }
        }
    }
}

void TcpServer::ProcessFrames(TcpClientConnection& client) {
    while (client.readBuffer.size() >= 4) {
        uint32_t payloadLen = 0;
        memcpy(&payloadLen, client.readBuffer.data(), 4);

        if (payloadLen > 4 * 1024 * 1024) {
            closesocket(client.socket);
            client.socket = INVALID_SOCKET;
            return;
        }

        if (client.readBuffer.size() < 4 + payloadLen) break;

        std::string utf8((char*)client.readBuffer.data() + 4, payloadLen);
        client.readBuffer.erase(client.readBuffer.begin(), client.readBuffer.begin() + 4 + payloadLen);

        // Handle AUTH specially
        if (utf8.rfind("AUTH|", 0) == 0) {
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t pos = 0; pos <= utf8.size(); ++pos) {
                if (pos == utf8.size() || utf8[pos] == '|') {
                    parts.push_back(utf8.substr(start, pos - start));
                    start = pos + 1;
                }
            }
            if (parts.size() >= 4) {
                m_onAuthRequest(client, parts[1], parts[2], parts[3]);
            } else {
                SendTo(client, "AUTH_FAIL|MALFORMED");
            }
            continue;
        }

        // Drop all non-AUTH commands from unauthenticated clients
        if (client.authState != TcpAuthState::Authenticated) continue;

        // Handle PING (authenticated only)
        if (utf8 == "PING") {
            SendTo(client, "PONG");
            continue;
        }

        // Convert to wide and dispatch
        std::wstring wide = Utf8ToWide(utf8);
        if (!wide.empty() && m_onMessage) {
            m_onMessage(client, wide);
        }
    }
}

std::string TcpServer::WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &utf8[0], len, nullptr, nullptr);
    return utf8;
}

std::wstring TcpServer::Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wide[0], len);
    return wide;
}

void TcpServer::SendRaw(SOCKET sock, const uint8_t* data, int len) {
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    int sent = 0;
    while (sent < len) {
        int r = send(sock, (const char*)(data + sent), len - sent, 0);
        if (r == SOCKET_ERROR) break;
        sent += r;
    }
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
}

void TcpServer::SendTo(TcpClientConnection& client, const std::string& utf8Message) {
    uint32_t payloadLen = (uint32_t)utf8Message.size();
    uint8_t header[4];
    memcpy(header, &payloadLen, 4);
    SendRaw(client.socket, header, 4);
    SendRaw(client.socket, (const uint8_t*)utf8Message.data(), (int)payloadLen);
}

void TcpServer::SendTo(TcpClientConnection& client, const std::wstring& message) {
    SendTo(client, WideToUtf8(message));
}

void TcpServer::Broadcast(const std::wstring& message) {
    std::string utf8 = WideToUtf8(message);
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (c.authState == TcpAuthState::Authenticated) {
            SendTo(c, utf8);
        }
    }
}

void TcpServer::Poll() {
    if (!m_running.load()) return;
    AcceptNewClients();
    ReadFromClients();
    CheckTimeouts();
}

void TcpServer::CheckTimeouts() {
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (size_t i = 0; i < m_clients.size(); ) {
        if (now - m_clients[i].lastActivity > CLIENT_TIMEOUT_MS) {
            RemoveClient(i);
        } else {
            ++i;
        }
    }
}

void TcpServer::RemoveClient(size_t index) {
    if (index < m_clients.size()) {
        closesocket(m_clients[index].socket);
        m_clients.erase(m_clients.begin() + index);
    }
}

void TcpServer::ApproveDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (c.deviceId == deviceId && c.authState == TcpAuthState::Pending) {
            c.authState = TcpAuthState::Authenticated;
            SendTo(c, "AUTH_OK");
            break;
        }
    }
}

void TcpServer::DenyDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (size_t i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].deviceId == deviceId && m_clients[i].authState == TcpAuthState::Pending) {
            SendTo(m_clients[i], "AUTH_FAIL|DENIED");
            RemoveClient(i);
            break;
        }
    }
}

void TcpServer::DisconnectDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (size_t i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].deviceId == deviceId) {
            RemoveClient(i);
            break;
        }
    }
}

void TcpServer::LoadAuthorizedDevices(const std::wstring& iniPath) {
    m_iniPath = iniPath;
    m_authorizedDevices.clear();

    wchar_t countBuf[32] = {};
    GetPrivateProfileStringW(L"AuthorizedDevices", L"count", L"0", countBuf, 32, iniPath.c_str());
    int count = _wtoi(countBuf);

    for (int i = 0; i < count; ++i) {
        wchar_t keyBuf[64];
        wchar_t valBuf[512];

        swprintf(keyBuf, 64, L"device%d_id", i);
        GetPrivateProfileStringW(L"AuthorizedDevices", keyBuf, L"", valBuf, 512, iniPath.c_str());
        std::wstring wId(valBuf);

        swprintf(keyBuf, 64, L"device%d_name", i);
        GetPrivateProfileStringW(L"AuthorizedDevices", keyBuf, L"", valBuf, 512, iniPath.c_str());
        std::wstring wName(valBuf);

        swprintf(keyBuf, 64, L"device%d_added", i);
        GetPrivateProfileStringW(L"AuthorizedDevices", keyBuf, L"", valBuf, 512, iniPath.c_str());
        std::wstring wAdded(valBuf);

        if (wId.empty()) continue;

        AuthorizedDevice dev;
        dev.id      = WideToUtf8(wId);
        dev.name    = WideToUtf8(wName);
        dev.dateAdded = WideToUtf8(wAdded);
        m_authorizedDevices.push_back(std::move(dev));
    }
}

void TcpServer::SaveAuthorizedDevices(const std::wstring& iniPath) {
    wchar_t countBuf[32];
    swprintf(countBuf, 32, L"%d", (int)m_authorizedDevices.size());
    WritePrivateProfileStringW(L"AuthorizedDevices", L"count", countBuf, iniPath.c_str());

    for (int i = 0; i < (int)m_authorizedDevices.size(); ++i) {
        const auto& dev = m_authorizedDevices[i];
        wchar_t keyBuf[64];

        swprintf(keyBuf, 64, L"device%d_id", i);
        WritePrivateProfileStringW(L"AuthorizedDevices", keyBuf, Utf8ToWide(dev.id).c_str(), iniPath.c_str());

        swprintf(keyBuf, 64, L"device%d_name", i);
        WritePrivateProfileStringW(L"AuthorizedDevices", keyBuf, Utf8ToWide(dev.name).c_str(), iniPath.c_str());

        swprintf(keyBuf, 64, L"device%d_added", i);
        WritePrivateProfileStringW(L"AuthorizedDevices", keyBuf, Utf8ToWide(dev.dateAdded).c_str(), iniPath.c_str());
    }
}

bool TcpServer::IsDeviceAuthorized(const std::string& deviceId) const {
    for (const auto& dev : m_authorizedDevices) {
        if (dev.id == deviceId) return true;
    }
    return false;
}

void TcpServer::AddAuthorizedDevice(const std::string& id, const std::string& name) {
    // Replace if already present, otherwise append
    for (auto& dev : m_authorizedDevices) {
        if (dev.id == id) {
            dev.name = name;
            if (!m_iniPath.empty()) SaveAuthorizedDevices(m_iniPath);
            return;
        }
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char dateBuf[16];
    sprintf(dateBuf, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    AuthorizedDevice dev;
    dev.id        = id;
    dev.name      = name;
    dev.dateAdded = dateBuf;
    m_authorizedDevices.push_back(std::move(dev));

    if (!m_iniPath.empty()) SaveAuthorizedDevices(m_iniPath);
}

void TcpServer::RemoveAuthorizedDevice(const std::string& id) {
    auto it = std::remove_if(m_authorizedDevices.begin(), m_authorizedDevices.end(),
        [&](const AuthorizedDevice& dev) { return dev.id == id; });
    if (it != m_authorizedDevices.end()) {
        m_authorizedDevices.erase(it, m_authorizedDevices.end());
        if (!m_iniPath.empty()) SaveAuthorizedDevices(m_iniPath);
    }
}

std::vector<AuthorizedDevice> TcpServer::GetAuthorizedDevices() const {
    return m_authorizedDevices;
}
