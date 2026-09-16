#pragma once
// pipe_server.h — Named Pipe IPC server for Milkwave visualizers
// Replaces the hidden WM_COPYDATA IPC window with a named pipe.
// Pipe name: \\.\pipe\Milkwave_<PID>   -- a normal instance
//            \\.\pipe\mDxChild_<PID>   -- a -child instance
//
// The two are deliberately in different namespaces. Milkwave_ is SHARED:
// Milkwave Visualizer publishes it too, and every discovery tool -- Milkwave
// Remote, the MCP server, vis.py, the probe harness -- enumerates it to find
// something to drive. A child must not be in that set. The parent owns every
// external interface, which is already why a child serves no TCP port and no
// mDNS advert (App.cpp); its pipe is an external interface too, and naming it
// like a parent's contradicted that within three lines of the code saying so.
//
// The parent still reaches its children: it gets the pid straight back from
// CreateProcess and builds the same name (child_instances.cpp).
// Supports multiple concurrent clients (e.g., MilkwaveRemote + MCP).

#ifndef PIPE_SERVER_H
#define PIPE_SERVER_H

#include <windows.h>
#include <atomic>
#include <mutex>
#include <queue>
#include <functional>
#include <string>
#include <vector>

// WM_MW_FULLSCREEN wParam convention (forgejo#23).
//
// The Displays tab needs absolute enter/exit, so 1 and 0 were given those
// meanings -- which silently redefined the 0 that every SIGNAL| and script
// caller was already sending. DispatchSignal can only send a wParam a table
// entry carries, so SIGNAL|FULLSCREEN meant "ensure exit" and could never
// enter fullscreen. The toggle needs a value of its own, and every sender has
// to name the same one, hence these live here rather than at each call site.
#define MW_FS_EXIT    0  // ensure exit, whatever the current state
#define MW_FS_ENTER   1  // ensure enter
#define MW_FS_TOGGLE  2  // toggle (SIGNAL|FULLSCREEN, the FULLSCREEN script command)

class PipeServer;

// ─── Pipe name bases ───────────────────────────────────────────────────────────

// The base a -child instance serves when nobody has said otherwise.
#define PIPE_CHILD_BASE_DEFAULT L"mDxChild"

// A base is [A-Za-z0-9_], 1 to 39 characters.
//
// It arrives over IPC from anyone who can reach the pipe, and it becomes a path
// component under \.\pipe\ -- a backslash in it would quietly create a name
// somewhere else. Rejected rather than sanitised: a caller who sent something
// odd should hear about it.
bool PipeNameBaseIsValid(const wchar_t* base);

// The base a CHILD of an instance serving `parentBase` should use.
//
// `parentBase` is what PipeServer::GetNameBase() returns -- "mdx12test_", with
// the separator Start concatenates, or "" for the default. The child's base is
// the parent's word with "Child" on the end, so an instance and its children
// sit in one namespace: rename a parent and its whole tree moves with it, and
// the user's children keep mDxChild_ to themselves.
//
// Exists so the name is derived ONCE. ChildSpawnNow builds the name it will
// wait on and the child's own Start builds the name it will serve, and those
// two never speak; while both were hardcoded that was duplication, but a base
// that varies makes them two things that can disagree -- and the parent then
// waits out its timeout on a name nobody serves and terminates a healthy
// child. The parent derives it here and hands it over on the command line.
//
// Falls back to PIPE_CHILD_BASE_DEFAULT when the result would be a name
// PipeNameBaseIsValid rejects, so the child is never told something it will
// refuse and silently serve the default of instead.
void PipeChildNameBase(const wchar_t* parentBase, wchar_t* out, size_t outCount);

// Per-client connection context — one per connected client, owned by PipeServer.
struct PipeClientContext {
    HANDLE                    hPipe = INVALID_HANDLE_VALUE;
    HANDLE                    hThread = nullptr;
    HANDLE                    hOutEvent = nullptr;   // auto-reset, wakes handler for outgoing
    std::queue<std::wstring>  outQueue;
    std::mutex                outMutex;
    wchar_t                   szClientExePath[MAX_PATH] = {};
    PipeServer*               pServer = nullptr;     // back-pointer for dispatch/shutdown
    int                       nClientId = 0;         // monotonic ID for logging

    // STRICT dispatch, for THIS connection only (issue 102).
    //
    // An unrecognised keyword normally falls through to the script engine, and
    // that is load-bearing -- Milkwave Remote depends on it, and NEXT, PREV,
    // LOCK and everything else the button board speaks arrive that way. But it
    // also means a caller's typo is neither rejected nor ignored: it becomes a
    // script line, and an unrecognised script line is DRAWN ON THE VISUALIZER.
    // A probe that sent LOGLEVEL=4 (the real keyword is SET_LOGLEVEL=) painted
    // itself across the render window mid-run.
    //
    // So strictness lives here rather than in a setting: it belongs to one
    // client for the life of its connection, it cannot be persisted, and it
    // cannot outlive the caller that asked for it. A client that never mentions
    // STRICT is never affected, which is what lets the default rule and this
    // feature both be true at once.
    bool                      bStrict = false;
};

class PipeServer {
public:
    PipeServer();
    ~PipeServer();

    // Start the pipe server. hTargetWindow receives wmIPCMessage posts.
    // wmIPCMessage: the custom IPC message constant (WM_APP+9 in MDropDX12, WM_USER+200 in Milkwave).
    // wmSignalBase: base for SIGNAL| dispatch (WM_APP in MDropDX12, WM_USER in Milkwave).
    //   SIGNAL|NEXT_PRESET → PostMessage(wmSignalBase + 100), etc.
    // bChildMode picks the mDxChild_ namespace. dwOnlyFromPid, when non-zero,
    // refuses every client that is not that process -- a child accepts its
    // parent and nobody else. Both are passed in rather than read from engine
    // state, so the pipe server keeps depending on nothing.
    void Start(HWND hTargetWindow, UINT wmIPCMessage,
               UINT wmSignalBase = 0x8000 /*WM_APP*/, bool bChildMode = false,
               DWORD dwOnlyFromPid = 0);
    void Stop();

    // Serve \.\pipe\<base>_<pid> instead of the default name, from now on.
    //
    // The PID is always appended and is never the caller's to choose: two
    // instances must not be able to claim one pipe, and a name that already
    // exists fails to create rather than doing something interesting.
    //
    // Why this exists: the test harness needs its instance to be
    // unmistakeable. Discovery keyed on the shared "Milkwave_" prefix finds the
    // user's copy and the harness's alike, and the harness driving the window
    // somebody is watching is a fault that has happened. A separate namespace
    // makes them different things rather than two things that look the same.
    //
    // Stops and restarts the accept thread, so any connected client is
    // disconnected -- send the reply BEFORE calling this. Returns false and
    // changes nothing if the base is empty or not [A-Za-z0-9_].
    bool Rename(const wchar_t* base);

    // What a NEW connection starts at. On for -child and -test, where every
    // message has a known sender who would rather hear about a typo than have
    // it painted across the frame -- there is no human at those connections for
    // the fall-through to serve. Existing connections keep whatever they have.
    void SetStrictDefault(bool bStrict) { m_bStrictDefault = bStrict; }
    bool GetStrictDefault() const { return m_bStrictDefault; }

    // The same, for a server that has not been started yet.
    //
    // Rename cannot do this job: it stops and restarts the server, and before
    // Start there is no window handle or message id to restart it WITH -- so it
    // would bring the pipe up bound to nothing. A child is told its base on the
    // command line, which is long before its window exists.
    //
    // Returns false and changes nothing on a base PipeNameBaseIsValid rejects.
    bool SetNameBase(const wchar_t* base);

    // The base in force, WITH its trailing separator ("mdx12test_"), or "" for
    // the default. What PipeChildNameBase takes.
    const wchar_t* GetNameBase() const { return m_szNameBase; }

    // Enqueue an outgoing message to all connected clients (fire-and-forget).
    void Send(const wchar_t* message);
    void Send(const std::wstring& message);

    // Every outgoing message is also handed to this hook, if one is installed
    // (forgejo#33).
    //
    // Handlers call Send() directly at 127 sites and SendMessageToMDropDX12Remote
    // at 12, and only the latter reached TCP -- so roughly 91% of the reply
    // surface, including every GET_ and DIAG_, was invisible to the Android
    // remote. It could send commands and almost never read anything back.
    //
    // A hook rather than a direct call into TcpServer, because this file is
    // shared with the other Milkwave visualizers and has no business knowing
    // what a TcpServer is. The engine installs one at startup.
    //
    // Called OUTSIDE m_clientsMutex: the hook takes the TCP client lock, and
    // holding two client locks in one order here and the other order there is
    // how deadlocks are built.
    using OutboundHook = std::function<void(const std::wstring&)>;
    void SetOutboundHook(OutboundHook hook) { m_outboundHook = std::move(hook); }

    bool IsRunning() const { return m_bRunning.load(); }
    bool IsConnected() const { return m_bClientConnected.load(); }
    int  GetClientCount() const;

    // Get the pipe name (for display in settings UI)
    const wchar_t* GetPipeName() const { return m_szPipeName; }

    // Get the full exe path of the last connected client (empty if never connected)
    const wchar_t* GetLastClientExePath() const { return m_szLastClientExePath; }

    // Parse SIGNAL| messages into PostMessage calls (public for LaunchMessage routing)
    bool DispatchSignal(const wchar_t* signal);

private:
    static unsigned __stdcall ServerThread(void* pParam);
    static unsigned __stdcall ClientThread(void* pParam);
    void ServerLoop();
    void ClientHandler(PipeClientContext* ctx);

    // Parse incoming pipe messages and dispatch to the target window
    // ctx is the connection the message arrived on -- it decides whether an
    // unmatched keyword falls through to the script engine or comes back as an
    // error, and it is where STRICT= is handled, since that verb never needs to
    // reach the engine at all.
    void DispatchMessage(const wchar_t* message, size_t len,
                         PipeClientContext* ctx = nullptr);

    // Move client from active to finished list (called by handler on disconnect)
    void RemoveClient(PipeClientContext* ctx);

    // Join and delete finished client handler threads
    void SweepFinished();

    HWND   m_hTargetWindow = nullptr;
    UINT   m_wmIPCMessage = 0;      // WM_MW_IPC_MESSAGE equivalent
    UINT   m_wmSignalBase = 0x8000; // base for SIGNAL| dispatch (WM_APP or WM_USER)

    std::atomic<bool> m_bRunning{false};
    std::atomic<bool> m_bShutdown{false};
    std::atomic<bool> m_bClientConnected{false};

    // Client tracking (protected by m_clientsMutex)
    std::vector<PipeClientContext*> m_clients;          // active clients
    std::vector<PipeClientContext*> m_finishedClients;  // awaiting cleanup
    mutable std::mutex              m_clientsMutex;
    int                             m_nNextClientId = 0;
    bool                            m_bStrictDefault = false;  // see SetStrictDefault

    // Shutdown event (manual-reset, shared across all threads)
    HANDLE m_hShutdownEvent = nullptr;

    HANDLE m_hServerThread = nullptr;

    // Set once at startup, before any client can connect, and never again.
    OutboundHook m_outboundHook;

    wchar_t m_szPipeName[64] = {};
    // Empty means "the default for this mode". Remembered so Rename can put
    // the server back up exactly as Start had it.
    wchar_t m_szNameBase[48] = {};
    bool    m_bChildMode = false;
    // 0 = accept anyone. Non-zero = the only client pid allowed to connect.
    DWORD m_dwOnlyFromPid = 0;
    wchar_t m_szLastClientExePath[MAX_PATH] = {};
};

// Connect to a running visualizer's pipe by PID and send a message.
// Used for second-instance forwarding (e.g., double-click .milk file).
// Returns true if message was sent successfully.
bool PipeSendToExistingInstance(const wchar_t* message);

#endif // PIPE_SERVER_H
