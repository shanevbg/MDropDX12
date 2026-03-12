// pipe_server.cpp — Named Pipe IPC server for Milkwave visualizers
// See pipe_server.h for overview.

#include "pipe_server.h"
#include <process.h>   // _beginthreadex
#include <sddl.h>      // ConvertStringSecurityDescriptorToSecurityDescriptor
#include <tlhelp32.h>  // CreateToolhelp32Snapshot
#include <cstdlib>     // _wtoi, wcstof
#include <cstring>     // wcsncmp, wcsstr
#include <algorithm>   // std::find

// Self-contained logging — uses OutputDebugStringA (works in all projects)
#include <cstdio>  // vsnprintf
#include <cstdarg>
static void PipeLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}

// ─── PipeServer ────────────────────────────────────────────────────────────────

PipeServer::PipeServer() {}

PipeServer::~PipeServer() {
    Stop();
}

void PipeServer::Start(HWND hTargetWindow, UINT wmIPCMessage, UINT wmSignalBase) {
    if (m_bRunning.load())
        return;

    m_hTargetWindow = hTargetWindow;
    m_wmIPCMessage = wmIPCMessage;
    m_wmSignalBase = wmSignalBase;
    m_bShutdown.store(false);

    // Build pipe name: \\.\pipe\Milkwave_<PID>
    swprintf_s(m_szPipeName, L"\\\\.\\pipe\\Milkwave_%u", GetCurrentProcessId());

    // Create shutdown event (manual-reset, shared across all threads)
    m_hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    // Start accept thread
    m_hServerThread = (HANDLE)_beginthreadex(
        nullptr, 0, &PipeServer::ServerThread, this, 0, nullptr);

    if (m_hServerThread) {
        m_bRunning.store(true);
        PipeLog("PipeServer: started on %ls\n", m_szPipeName);
    } else {
        PipeLog("PipeServer: failed to start thread\n");
    }
}

void PipeServer::Stop() {
    if (!m_bRunning.load())
        return;

    m_bShutdown.store(true);
    if (m_hShutdownEvent)
        SetEvent(m_hShutdownEvent);

    // Snapshot client handles for cancellation and joining
    std::vector<HANDLE> clientPipes;
    std::vector<HANDLE> clientThreads;
    std::vector<HANDLE> clientOutEvents;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto* ctx : m_clients) {
            clientPipes.push_back(ctx->hPipe);
            clientThreads.push_back(ctx->hThread);
            clientOutEvents.push_back(ctx->hOutEvent);
        }
    }

    // Cancel pending I/O and wake all client handlers
    for (HANDLE h : clientPipes)
        CancelIoEx(h, NULL);
    for (HANDLE h : clientOutEvents)
        SetEvent(h);

    // Wait for accept thread
    if (m_hServerThread) {
        WaitForSingleObject(m_hServerThread, 5000);
        CloseHandle(m_hServerThread);
        m_hServerThread = nullptr;
    }

    // Wait for client handler threads
    for (HANDLE h : clientThreads)
        WaitForSingleObject(h, 3000);

    // Clean up any remaining clients (handlers should have finished by now)
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto* ctx : m_clients) {
            if (ctx->hPipe != INVALID_HANDLE_VALUE) {
                DisconnectNamedPipe(ctx->hPipe);
                CloseHandle(ctx->hPipe);
            }
            if (ctx->hThread) CloseHandle(ctx->hThread);
            if (ctx->hOutEvent) CloseHandle(ctx->hOutEvent);
            delete ctx;
        }
        m_clients.clear();

        // Also clean up finished clients
        for (auto* ctx : m_finishedClients) {
            if (ctx->hThread) {
                WaitForSingleObject(ctx->hThread, 100);
                CloseHandle(ctx->hThread);
            }
            if (ctx->hOutEvent) CloseHandle(ctx->hOutEvent);
            delete ctx;
        }
        m_finishedClients.clear();
    }

    if (m_hShutdownEvent) { CloseHandle(m_hShutdownEvent); m_hShutdownEvent = nullptr; }

    m_bRunning.store(false);
    m_bClientConnected.store(false);
    PipeLog("PipeServer: stopped\n");
}

void PipeServer::Send(const wchar_t* message) {
    if (!message || !*message || m_bShutdown.load())
        return;

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto* ctx : m_clients) {
        {
            std::lock_guard<std::mutex> qlock(ctx->outMutex);
            ctx->outQueue.emplace(message);
        }
        SetEvent(ctx->hOutEvent);
    }
}

void PipeServer::Send(const std::wstring& message) {
    Send(message.c_str());
}

int PipeServer::GetClientCount() const {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return (int)m_clients.size();
}

void PipeServer::RemoveClient(PipeClientContext* ctx) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = std::find(m_clients.begin(), m_clients.end(), ctx);
    if (it != m_clients.end())
        m_clients.erase(it);
    m_finishedClients.push_back(ctx);
    m_bClientConnected.store(!m_clients.empty());
}

void PipeServer::SweepFinished() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto* ctx : m_finishedClients) {
        if (ctx->hThread) {
            WaitForSingleObject(ctx->hThread, 1000);
            CloseHandle(ctx->hThread);
        }
        if (ctx->hOutEvent)
            CloseHandle(ctx->hOutEvent);
        delete ctx;
    }
    m_finishedClients.clear();
}

unsigned __stdcall PipeServer::ServerThread(void* pParam) {
    PipeServer* self = static_cast<PipeServer*>(pParam);
    self->ServerLoop();
    return 0;
}

unsigned __stdcall PipeServer::ClientThread(void* pParam) {
    PipeClientContext* ctx = static_cast<PipeClientContext*>(pParam);
    ctx->pServer->ClientHandler(ctx);
    return 0;
}

// ─── Accept loop ────────────────────────────────────────────────────────────────

void PipeServer::ServerLoop() {
    // Security: allow same-user access (handles admin/non-admin mismatch)
    SECURITY_ATTRIBUTES sa = {};
    PSECURITY_DESCRIPTOR pSD = nullptr;
    // SDDL: D:(A;;GA;;;WD) — grant all access to Everyone
    // This is safe because named pipes are local-only and the pipe name
    // includes the PID, so only someone who can enumerate processes can connect.
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GA;;;WD)", SDDL_REVISION_1, &pSD, NULL)) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = pSD;
        sa.bInheritHandle = FALSE;
    }

    while (!m_bShutdown.load()) {
        // Clean up finished handler threads
        SweepFinished();

        // Create a pipe instance for the next client
        HANDLE hPipe = CreateNamedPipeW(
            m_szPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,   // out buffer
            65536,   // in buffer
            0,       // default timeout
            pSD ? &sa : NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            PipeLog("PipeServer: CreateNamedPipe failed, err=%u\n", GetLastError());
            Sleep(1000);
            continue;
        }

        // Wait for client connection (overlapped so we can cancel on shutdown)
        OVERLAPPED ovConnect = {};
        ovConnect.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

        BOOL connected = ConnectNamedPipe(hPipe, &ovConnect);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                // Wait for either connection or shutdown
                HANDLE waitHandles[] = { ovConnect.hEvent, m_hShutdownEvent };
                DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

                if (waitResult == WAIT_OBJECT_0 + 1 || m_bShutdown.load()) {
                    // Shutdown requested
                    CancelIoEx(hPipe, &ovConnect);
                    CloseHandle(ovConnect.hEvent);
                    CloseHandle(hPipe);
                    break;
                }

                // Check if ConnectNamedPipe completed successfully
                DWORD bytesTransferred;
                if (!GetOverlappedResult(hPipe, &ovConnect, &bytesTransferred, FALSE)) {
                    CloseHandle(ovConnect.hEvent);
                    CloseHandle(hPipe);
                    continue;
                }
            } else if (err == ERROR_PIPE_CONNECTED) {
                // Client already connected before ConnectNamedPipe was called
            } else {
                // Real error
                CloseHandle(ovConnect.hEvent);
                CloseHandle(hPipe);
                Sleep(100);
                continue;
            }
        }
        CloseHandle(ovConnect.hEvent);

        // Client connected — create context and spawn handler thread
        PipeClientContext* ctx = new PipeClientContext();
        ctx->hPipe = hPipe;
        ctx->pServer = this;
        ctx->hOutEvent = CreateEventW(NULL, FALSE, FALSE, NULL);  // auto-reset

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            ctx->nClientId = m_nNextClientId++;
            m_clients.push_back(ctx);
            m_bClientConnected.store(true);
        }

        // Capture client exe path
        ULONG clientPid = 0;
        if (GetNamedPipeClientProcessId(hPipe, &clientPid) && clientPid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, clientPid);
            if (hProc) {
                DWORD pathLen = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, ctx->szClientExePath, &pathLen)) {
                    // Update shared last-client path (most recent wins)
                    wcscpy_s(m_szLastClientExePath, ctx->szClientExePath);
                    PipeLog("PipeServer: client #%d exe: %ls\n", ctx->nClientId, ctx->szClientExePath);
                }
                CloseHandle(hProc);
            }
        }

        // Start handler thread
        ctx->hThread = (HANDLE)_beginthreadex(
            nullptr, 0, &PipeServer::ClientThread, ctx, 0, nullptr);

        if (!ctx->hThread) {
            PipeLog("PipeServer: failed to start handler for client #%d\n", ctx->nClientId);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            CloseHandle(ctx->hOutEvent);
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                auto it = std::find(m_clients.begin(), m_clients.end(), ctx);
                if (it != m_clients.end()) m_clients.erase(it);
                m_bClientConnected.store(!m_clients.empty());
            }
            delete ctx;
            continue;
        }

        PipeLog("PipeServer: client #%d connected (total: %d)\n",
                ctx->nClientId, GetClientCount());
    }

    if (pSD)
        LocalFree(pSD);
}

// ─── Per-client read/write loop ─────────────────────────────────────────────────

void PipeServer::ClientHandler(PipeClientContext* ctx) {
    OVERLAPPED ovRead = {};
    ovRead.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    wchar_t readBuf[32768];  // 64KB in wchars
    bool readPending = false;

    while (!m_bShutdown.load()) {
        // Start an async read if not already pending
        if (!readPending) {
            ResetEvent(ovRead.hEvent);
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(ctx->hPipe, readBuf, sizeof(readBuf) - sizeof(wchar_t),
                               &bytesRead, &ovRead);
            if (ok) {
                // Completed immediately
                readBuf[bytesRead / sizeof(wchar_t)] = L'\0';
                DispatchMessage(readBuf, bytesRead / sizeof(wchar_t));
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    readPending = true;
                } else if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                    break;  // client disconnected
                } else {
                    break;  // unexpected error
                }
            }
        }

        // Wait on: read completion, outgoing data, or shutdown
        HANDLE waitHandles[] = { ovRead.hEvent, ctx->hOutEvent, m_hShutdownEvent };
        DWORD nHandles = readPending ? 3 : 2;
        DWORD waitIdx = WaitForMultipleObjects(
            nHandles, readPending ? waitHandles : waitHandles + 1,
            FALSE, 50);  // 50ms timeout for responsiveness

        if (m_bShutdown.load())
            break;

        // Check read completion
        if (readPending) {
            if (waitIdx == WAIT_OBJECT_0) {
                DWORD bytesRead = 0;
                if (GetOverlappedResult(ctx->hPipe, &ovRead, &bytesRead, FALSE)) {
                    readBuf[bytesRead / sizeof(wchar_t)] = L'\0';
                    DispatchMessage(readBuf, bytesRead / sizeof(wchar_t));
                    readPending = false;
                } else {
                    DWORD err = GetLastError();
                    if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                        break;
                    } else if (err == ERROR_MORE_DATA) {
                        // Message too large for buffer — dispatch what we have, discard rest
                        readBuf[bytesRead / sizeof(wchar_t)] = L'\0';
                        DispatchMessage(readBuf, bytesRead / sizeof(wchar_t));
                        readPending = false;
                    }
                }
            }
        }

        // Drain outgoing queue
        {
            std::lock_guard<std::mutex> lock(ctx->outMutex);
            while (!ctx->outQueue.empty() && !m_bShutdown.load()) {
                std::wstring& msg = ctx->outQueue.front();
                DWORD bytesWritten = 0;
                DWORD cbWrite = (DWORD)((msg.size() + 1) * sizeof(wchar_t));
                OVERLAPPED ovWrite = {};
                ovWrite.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
                BOOL ok = WriteFile(ctx->hPipe, msg.c_str(), cbWrite, &bytesWritten, &ovWrite);
                if (!ok) {
                    DWORD err = GetLastError();
                    if (err == ERROR_IO_PENDING) {
                        DWORD wr = WaitForSingleObject(ovWrite.hEvent, 1000);
                        if (wr == WAIT_OBJECT_0)
                            GetOverlappedResult(ctx->hPipe, &ovWrite, &bytesWritten, FALSE);
                    } else if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                        CloseHandle(ovWrite.hEvent);
                        ctx->outQueue.pop();
                        break;  // client disconnected during write
                    }
                }
                CloseHandle(ovWrite.hEvent);
                ctx->outQueue.pop();
            }
        }
    }

    // Cancel pending read
    if (readPending)
        CancelIoEx(ctx->hPipe, &ovRead);
    CloseHandle(ovRead.hEvent);

    // Disconnect pipe
    DisconnectNamedPipe(ctx->hPipe);
    CloseHandle(ctx->hPipe);
    ctx->hPipe = INVALID_HANDLE_VALUE;

    // Move to finished list (accept loop will clean up)
    RemoveClient(ctx);
    PipeLog("PipeServer: client #%d disconnected\n", ctx->nClientId);
}

// ─── Message dispatch ───────────────────────────────────────────────────────────

void PipeServer::DispatchMessage(const wchar_t* message, size_t len) {
    if (!message || len == 0 || !m_hTargetWindow) {
        PipeLog("PipeServer::DispatchMessage: null/empty (msg=%p len=%zu hwnd=%p)\n",
                message, len, m_hTargetWindow);
        return;
    }

    PipeLog("PipeServer: received [%ls] len=%zu\n", message, len);

    // Check for SIGNAL| prefix — these map to PostMessage calls
    if (wcsncmp(message, L"SIGNAL|", 7) == 0) {
        if (DispatchSignal(message + 7))
            return;
    }

    // Check for SPOUT_SENDER= prefix — maps to WM_MW_IPC_MESSAGE with dwData=WM_MW_SETSPOUTSENDER
    if (wcsncmp(message, L"SPOUT_SENDER=", 13) == 0) {
        const wchar_t* name = message + 13;
        size_t nameLen = wcslen(name);
        wchar_t* copy = (wchar_t*)malloc((nameLen + 1) * sizeof(wchar_t));
        if (copy) {
            wcscpy_s(copy, nameLen + 1, name);
            // Use the SETSPOUTSENDER constant as dwData (base + 108)
            // The engine handler checks for this in the IPC message
            UINT wmSetSpoutSender = m_wmSignalBase + 108;
            if (!PostMessageW(m_hTargetWindow, m_wmIPCMessage,
                              (WPARAM)wmSetSpoutSender, (LPARAM)copy)) {
                free(copy);
            }
        }
        return;
    }

    // All other messages: heap-copy and post as WM_MW_IPC_MESSAGE with dwData=1
    size_t msgLen = wcslen(message);
    wchar_t* copy = (wchar_t*)malloc((msgLen + 1) * sizeof(wchar_t));
    if (copy) {
        wcscpy_s(copy, msgLen + 1, message);
        if (!PostMessageW(m_hTargetWindow, m_wmIPCMessage, (WPARAM)1, (LPARAM)copy)) {
            PipeLog("PipeServer: PostMessage FAILED err=%u\n", GetLastError());
            free(copy);
        } else {
            PipeLog("PipeServer: posted IPC message to hwnd=%p\n", m_hTargetWindow);
        }
    }
}

// ─── Signal dispatch table ─────────────────────────────────────────────────────
struct SignalEntry {
    const wchar_t* name;
    int offset;       // added to m_wmSignalBase
    bool hasValue;    // true = uses KEY=VALUE format (wcsncmp + send parsed value)
};

static const SignalEntry s_signalTable[] = {
    // Simple signals (exact match, no value)
    { L"NEXT_PRESET",          100, false },
    { L"PREV_PRESET",          101, false },
    { L"COVER_CHANGED",        102, false },
    { L"SPRITE_MODE",          103, false },
    { L"MESSAGE_MODE",         104, false },
    { L"CAPTURE",              105, false },
    { L"SHOW_COVER",           110, false },
    // KEY=VALUE signals (prefix match, value parsed as int into WPARAM)
    { L"SETVIDEODEVICE",       106, true  },
    { L"ENABLEVIDEOMIX",       107, true  },
    { L"ENABLESPOUTMIX",       109, true  },
    { L"SET_INPUTMIX_OPACITY", 150, true  },
    { L"SET_INPUTMIX_ONTOP",   152, true  },
};

bool PipeServer::DispatchSignal(const wchar_t* signal) {
    if (!signal || !m_hTargetWindow)
        return false;

    // Parse: NEXT_PRESET, PREV_PRESET, etc.
    // Uses m_wmSignalBase (WM_APP for MDropDX12, WM_USER for Milkwave)
    for (const auto& entry : s_signalTable) {
        if (!entry.hasValue) {
            // Exact match for simple signals
            if (wcscmp(signal, entry.name) == 0) {
                PostMessageW(m_hTargetWindow, m_wmSignalBase + entry.offset, 0, 0);
                return true;
            }
        } else {
            // Prefix match for KEY=VALUE signals
            size_t nameLen = wcslen(entry.name);
            if (wcsncmp(signal, entry.name, nameLen) == 0 && signal[nameLen] == L'=') {
                const wchar_t* value = signal + nameLen + 1;
                PostMessageW(m_hTargetWindow, m_wmSignalBase + entry.offset, (WPARAM)_wtoi(value), 0);
                return true;
            }
        }
    }

    // Special case: SET_INPUTMIX_LUMAKEY parses threshold|softness into WPARAM and LPARAM
    const wchar_t kLumaKey[] = L"SET_INPUTMIX_LUMAKEY";
    const size_t kLumaKeyLen = _countof(kLumaKey) - 1;
    if (wcsncmp(signal, kLumaKey, kLumaKeyLen) == 0 && signal[kLumaKeyLen] == L'=') {
        const wchar_t* value = signal + kLumaKeyLen + 1;
        int threshold = _wtoi(value);
        int softness = 0;
        const wchar_t* pipe = wcschr(value, L'|');
        if (pipe)
            softness = _wtoi(pipe + 1);
        PostMessageW(m_hTargetWindow, m_wmSignalBase + 151, (WPARAM)threshold, (LPARAM)softness);
        return true;
    }

    return false;  // unknown signal
}

// ─── Second-instance forwarding ────────────────────────────────────────────────

bool PipeSendToExistingInstance(const wchar_t* message) {
    if (!message || !*message)
        return false;

    // Enumerate processes to find other instances of the current exe
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return false;

    DWORD myPid = GetCurrentProcessId();

    // Get our exe filename (just the name, no path)
    wchar_t myExePath[MAX_PATH];
    GetModuleFileNameW(NULL, myExePath, MAX_PATH);
    const wchar_t* myExeName = wcsrchr(myExePath, L'\\');
    myExeName = myExeName ? myExeName + 1 : myExePath;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool sent = false;

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == myPid)
                continue;
            if (_wcsicmp(pe.szExeFile, myExeName) != 0)
                continue;

            // Found another instance — try to connect to its pipe
            wchar_t pipeName[64];
            swprintf_s(pipeName, L"\\\\.\\pipe\\Milkwave_%u", pe.th32ProcessID);

            HANDLE hPipe = CreateFileW(
                pipeName,
                GENERIC_READ | GENERIC_WRITE,
                0, NULL,
                OPEN_EXISTING,
                0, NULL);

            if (hPipe != INVALID_HANDLE_VALUE) {
                // Set message read mode
                DWORD mode = PIPE_READMODE_MESSAGE;
                SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

                DWORD cbWrite = (DWORD)((wcslen(message) + 1) * sizeof(wchar_t));
                DWORD written = 0;
                WriteFile(hPipe, message, cbWrite, &written, NULL);
                CloseHandle(hPipe);
                sent = true;
                break;  // sent to first found instance
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return sent;
}
