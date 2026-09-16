// child_instances.cpp — spawning, configuring and retiring per-display children.
// See child_instances.h for the overview. Everything below runs on the child
// worker thread unless the comment says otherwise.

#include "engine.h"
#include "child_instances.h"
#include "pipe_server.h"
#include "utility.h"
#include "format_to.h"

#include <process.h>   // _beginthreadex
#include <algorithm>

// One second between status polls. The Displays list shows what each display is
// playing; that does not need to be frame-accurate, and a tighter loop would
// reconnect to every child's pipe several times a second to learn nothing.
static const DWORD kPollIntervalMs = 1000;

// A cold start compiles shaders and enumerates audio devices before it serves
// a pipe. Generous, because the cost of being wrong is killing a child that was
// about to come up.
static const DWORD kPipeAppearTimeoutMs = 60000;

// Engine lives in namespace mdrop (engine.h), so its members must be
// defined there too -- every other engine_*.cpp does the same.
namespace mdrop {

// ─── One-shot send, no reply ───────────────────────────────────────────────────
//
// Deliberately fire-and-forget. Replies from a child are broadcast to everyone
// connected to that child, and blocking here for one would mean a synchronous
// read per child per poll on a thread that also has to stay responsive to
// spawn and kill requests.
static bool ChildSend(const wchar_t* pipeName, const std::wstring& msg) {
    HANDLE h = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    DWORD written = 0;
    const DWORD cb = (DWORD)((msg.size() + 1) * sizeof(wchar_t));
    const BOOL ok = WriteFile(h, msg.c_str(), cb, &written, NULL);
    CloseHandle(h);
    return ok != FALSE;
}

// ─── Send and wait for one reply ───────────────────────────────────────────────
//
// A child's PipeServer broadcasts every reply to all of its connected clients,
// so keeping this connection open after the write is all it takes to receive
// one. Non-blocking reads with a deadline, because a child that is mid-preset-
// compile can take a moment and the worker must not park on it.
static bool ChildRequest(const wchar_t* pipeName, const std::wstring& msg,
                         const wchar_t* wantPrefix, std::wstring& reply,
                         DWORD timeoutMs) {
    HANDLE h = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);

    DWORD written = 0;
    const DWORD cb = (DWORD)((msg.size() + 1) * sizeof(wchar_t));
    if (!WriteFile(h, msg.c_str(), cb, &written, NULL)) {
        CloseHandle(h);
        return false;
    }

    mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);

    const size_t wantLen = wcslen(wantPrefix);
    const DWORD deadline = GetTickCount() + timeoutMs;
    bool got = false;
    std::vector<wchar_t> buf(8192);
    while (GetTickCount() < deadline) {
        DWORD read = 0;
        if (ReadFile(h, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t) - sizeof(wchar_t)),
                     &read, NULL) && read > 0) {
            buf[read / sizeof(wchar_t)] = L'\0';
            if (wcsncmp(buf.data(), wantPrefix, wantLen) == 0) {
                reply = buf.data();
                got = true;
                break;
            }
            continue;   // some other broadcast; keep looking
        }
        Sleep(20);
    }
    CloseHandle(h);
    return got;
}

// ─── Worker lifecycle ──────────────────────────────────────────────────────────

// A job the children are assigned to, so the OS cleans them up if we do not.
//
// The parent already runs a watchdog INSIDE each child (App.cpp WatchParent),
// which closes it when the parent handle signals. That covers the ordinary
// case and nothing else: it needs the child to have got far enough to start
// the watchdog, and to be responsive enough to act on it. A child that hangs
// during startup, or is wedged, outlives its parent and sits there as a window
// nobody owns -- which is exactly how three unexplained MDropDX12 processes
// ended up on screen.
//
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE needs no cooperation from the child at
// all. The kernel terminates every process in the job when the last handle to
// it closes, and that happens however the parent dies, including a crash.
static HANDLE CreateKillOnCloseJob() {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &info, sizeof(info))) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

void Engine::StartChildWorker() {
    if (m_hChildWorker) return;
    if (!m_hChildJob) {
        m_hChildJob = CreateKillOnCloseJob();
        if (!m_hChildJob)
            DLOG_WARN("child: no job object; children will rely on the "
                      "in-child parent watchdog alone");
    }
    m_bChildWorkerStop.store(false);
    m_hChildCmdEvent = CreateEventW(NULL, FALSE, FALSE, NULL);   // auto-reset
    m_hChildWorker = (HANDLE)_beginthreadex(
        nullptr, 0, &Engine::ChildWorkerThread, this, 0, nullptr);
    if (!m_hChildWorker)
        DLOG_ERROR("child worker: failed to start");
}

void Engine::StopChildWorker() {
    if (!m_hChildWorker) return;
    m_bChildWorkerStop.store(true);
    if (m_hChildCmdEvent) SetEvent(m_hChildCmdEvent);
    WaitForSingleObject(m_hChildWorker, 20000);
    CloseHandle(m_hChildWorker);
    m_hChildWorker = nullptr;

    // Retire every child explicitly. They die with the parent anyway -- each one
    // watches the parent handle and closes when it signals -- but a SHUTDOWN
    // runs the child's real exit path, and being killed by the watchdog does
    // not.
    std::vector<std::wstring> devices;
    {
        std::lock_guard<std::mutex> lock(m_childrenMutex);
        for (auto& ci : m_children) devices.push_back(ci.szDeviceName);
    }
    for (auto& d : devices) ChildKillNow(d.c_str());

    if (m_hChildCmdEvent) { CloseHandle(m_hChildCmdEvent); m_hChildCmdEvent = nullptr; }

    // Closing this kills anything still in the job. By here every child has
    // been asked to shut down and joined, so it should be empty -- this is the
    // backstop for one that ignored us.
    if (m_hChildJob) { CloseHandle(m_hChildJob); m_hChildJob = nullptr; }
}

unsigned __stdcall Engine::ChildWorkerThread(void* pParam) {
    static_cast<Engine*>(pParam)->ChildWorkerLoop();
    return 0;
}

void Engine::ChildWorkerLoop() {
    while (!m_bChildWorkerStop.load()) {
        // Drain every queued command before polling. Spawning is slow, so a
        // burst of them -- loading a profile that names four displays -- must
        // not interleave with a second of polling each time.
        for (;;) {
            ChildCommand cmd;
            {
                std::lock_guard<std::mutex> lock(m_childCmdMutex);
                if (m_childCmdQueue.empty()) break;
                cmd = m_childCmdQueue.front();
                m_childCmdQueue.pop();
            }
            if (m_bChildWorkerStop.load()) return;
            // The gap between "enqueued" and "picked up" is exactly where
            // forgejo#57 lost every command, and nothing logged either end of
            // it. Both ends log now, so a queue nobody drains is one grep away.
            DLOG_INFO("child worker: %ls %ls",
                      cmd.cmd == ChildCmd::Spawn       ? L"spawn"
                      : cmd.cmd == ChildCmd::Kill      ? L"kill"
                      : cmd.cmd == ChildCmd::Reconfigure ? L"reconfigure"
                                                         : L"send",
                      cmd.szDeviceName);
            switch (cmd.cmd) {
                case ChildCmd::Spawn:
                    ChildSpawnNow(cmd.szDeviceName);
                    ClearChildSpawnPending(cmd.szDeviceName);
                    break;
                case ChildCmd::Kill:  ChildKillNow(cmd.szDeviceName);  break;
                case ChildCmd::Reconfigure: {
                    ChildInstance ci;
                    if (GetChildInfo(cmd.szDeviceName, ci))
                        ChildConfigureNow(ci, false);
                    break;
                }
                case ChildCmd::Send: {
                    ChildInstance ci;
                    if (GetChildInfo(cmd.szDeviceName, ci) && ci.bReady)
                        ChildSend(ci.szPipeName, cmd.sPayload);
                    break;
                }
            }
        }
        if (m_bChildWorkerStop.load()) return;
        ChildPollAll();
        WaitForSingleObject(m_hChildCmdEvent, kPollIntervalMs);
    }
}

// ─── Spawn ─────────────────────────────────────────────────────────────────────

bool Engine::ChildSpawnNow(const wchar_t* deviceName) {
    // Never on the display the render window is already on.
    //
    // The IPC verb refuses this (forgejo#34), but three other callers do not:
    // the startup loop, the display-profile loop and the Displays checkbox all
    // spawn straight from bEnabled && bOwnProcess. A persisted OwnProcess=1 on
    // the render display therefore put a full second instance on top of the
    // main window at startup -- borderless-fullscreen, because
    // ChildConfigureNow places it -- giving more processes than there are
    // outputs to show them on. The check belongs here, at the one point every
    // caller goes through, not in each of them.
    {
        wchar_t renderDev[32];
        if (GetRenderMonitorDevice(renderDev) &&
            wcscmp(renderDev, deviceName) == 0) {
            DLOG_WARN("child: refusing to spawn on %ls -- it hosts the render "
                      "window, which already draws its own preset",
                      deviceName);
            SetChildSpawnFailure(deviceName, L"hosts_render_window");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_childrenMutex);
        for (auto& ci : m_children)
            if (wcscmp(ci.szDeviceName, deviceName) == 0 && !ci.bFailed) {
                // The one exit that used to leave no trace. Ruling it out cost
                // more than the line costs (forgejo#57).
                DLOG_INFO("child: %ls already has pid %lu; spawn request ignored",
                          deviceName, (unsigned long)ci.pid);
                return true;                      // already have a live one
            }

        // A FAILED entry for this device falls through the check above, and the
        // push_back at the end of this function would then leave two entries
        // for one display. GetChildInfo returns the first match, so every later
        // send, kill and reconfigure would resolve to the dead pipe while a
        // healthy child rendered unaddressed. Drop the corpse first.
        m_children.erase(
            std::remove_if(m_children.begin(), m_children.end(),
                [deviceName](const ChildInstance& ci) {
                    return wcscmp(ci.szDeviceName, deviceName) == 0;
                }),
            m_children.end());
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    // -display tells the child where it belongs BEFORE it has a window, which
    // is the only moment early enough. Placement over the pipe cannot do it:
    // the child's own start-up restores the shared settings.ini geometry after
    // its pipe starts answering, so anything sent in between is overwritten
    // (see ChildPlaceNow), and until then a second visualizer sits on the main
    // window's monitor. Told up front, it is built on the right display, at the
    // right size, hidden, and shown only once it has a frame.
    // Fade in ONLY if there is a mirror on that display to dissolve from.
    //
    // The fade exists to make the handover from the mirrored image to the
    // display's own preset a dissolve rather than a jump. With nothing on the
    // display -- it was off, or mirrors are not running -- there is no image to
    // dissolve from, so a fade is a fade up from the DESKTOP, which is worse
    // than simply appearing. Observed exactly that: a child started on a
    // display that had been switched off faded in over the wallpaper.
    //
    // The parent decides because only the parent knows; the child is told a
    // duration and obeys it. 0 means appear at once.
    bool mirrorLive = false;
    bool wantFullscreen = false;
    {
        // m_displayOutputsMutex: #27. m_childrenMutex is not held here
        // (released above, at line 268's closing brace), so this cannot nest
        // with it in either order.
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor) continue;
            if (wcscmp(out.config.szDeviceName, deviceName) != 0) continue;
            mirrorLive = m_bMirrorsActive && out.config.bEnabled &&
                         !out.bSkippedSameMonitor && out.monitorState &&
                         out.monitorState->bReady &&
                         !out.monitorState->bSoftDisabled;
            wantFullscreen = out.config.bFullscreen;
            break;
        }
    }
    int fadeMs = 0;
    if (mirrorLive) {
        fadeMs = Config().GetInt(L"Settings", L"nChildFadeMs", 1500);
        if (fadeMs < 0) fadeMs = 0;
        if (fadeMs > 10000) fadeMs = 10000;
    }

    // The child's namespace, derived from ours so a renamed instance takes its
    // whole tree with it. Worked out ONCE here and used twice below -- on the
    // command line, and to build the name we then wait on -- because those two
    // agreeing is the entire requirement (PipeChildNameBase).
    wchar_t childBase[48];
    {
        extern PipeServer g_pipeServer;
        PipeChildNameBase(g_pipeServer.GetNameBase(), childBase, _countof(childBase));
    }

    // Command-line extras travel on the COMMAND LINE, not over the pipe, for
    // the same reason -display does: the child's own start-up restores
    // geometry after its pipe starts answering, so anything sent in between is
    // overwritten. See the note above CreateWindowAndRun's child block -- that
    // is the measured 1.8s wrong-display flash, and a rect sent late would
    // reproduce it exactly.
    wchar_t extraArgs[64] = L"";
    const int winMode = EffectiveChildWindowMode();
    // Only the real, non-test spawn of a fullscreen display asks for the
    // foreground -- a test window never does (kChildWindowTiny/Hidden), and
    // neither does a windowed one, matching "if it says full screen start full
    // screen in foreground" and nothing else. forgejo#85.
    const bool wantForeground = (winMode == kChildWindowNormal) && wantFullscreen;
    if (winMode == kChildWindowHidden) {
        FormatTo(extraArgs, L" -hidewindow");
    } else if (winMode == kChildWindowTiny) {
        RECT r;
        if (TestChildRect(deviceName, r))
            FormatTo(extraArgs, L" -rect=%d,%d,%d,%d",
                     r.left, r.top, r.right - r.left, r.bottom - r.top);
        else
            // No rect to place it at -- e.g. this display has no rcMonitor
            // yet -- so fall back to hidden rather than leave extraArgs
            // empty, which used to mean the SAME full-monitor sizing a real,
            // non-test spawn gets (App.cpp's f.got-but-no-test-rect branch).
            // A test window that cannot be sized small must not become a
            // fullscreen one: disappearing is the safe failure here, not
            // covering a monitor.
            FormatTo(extraArgs, L" -hidewindow");
    } else if (wantForeground) {
        FormatTo(extraArgs, L" -foreground");
    }

    // The slot this child answers to in its own window caption. Lowest free
    // first, so retiring the child on one display and starting another does not
    // walk the numbers upward forever -- N identifies a child, it does not
    // count them.
    //
    // Safe without holding m_childrenMutex across the spawn: every Spawn is
    // processed by the single child worker thread, so no second spawn can claim
    // the same slot between here and the push_back below. The lock is still
    // taken for the read itself, because the poll loop writes m_children.
    int childIndex = 1;
    {
        std::lock_guard<std::mutex> lock(m_childrenMutex);
        for (;; childIndex++) {
            bool taken = false;
            for (const auto& ci : m_children)
                if (ci.nIndex == childIndex) { taken = true; break; }
            if (!taken) break;
        }
    }

    wchar_t cmdLine[MAX_PATH + 256];
    FormatTo(cmdLine, L"\"%s\" -child=%u -childindex=%d -display=%d -fade=%d -pipe=%s%s",
             exePath, GetCurrentProcessId(), childIndex,
             DisplayNumberFromDevice(deviceName),
             fadeMs, childBase, extraArgs);
    DLOG_INFO("child: %ls will %ls", deviceName,
              fadeMs ? L"fade in over the mirror"
                     : L"appear at once (no mirror to dissolve from)");

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    // Appear without taking the foreground BY DEFAULT. A parent whose window
    // loses focus every time a display is enabled is unusable, and the same
    // treatment the probe harness gives a launch.
    //
    // A fullscreen display is the deliberate exception (forgejo#85): -foreground
    // above tells the child to take it anyway, once, at reveal -- covering the
    // whole screen is worth the interruption in a way a windowed child sharing
    // the desktop is not. SW_SHOWNOACTIVATE still governs the WINDOWS-level
    // launch flag here (a suspended process has no window yet to activate);
    // AllowSetForegroundWindow below is what keeps that later, deliberate
    // SetForegroundWindow call from being silently dropped by Windows' own
    // foreground lock, which blocks an unrelated process's attempt to activate
    // itself unless explicitly granted.
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        DLOG_ERROR("child: CreateProcess failed for %ls, err=%lu",
                   deviceName, (unsigned long)GetLastError());
        SetChildSpawnFailure(deviceName, L"create_process_failed");
        return false;
    }
    // Into the job BEFORE it runs, so it cannot outlive us and cannot start
    // anything that escapes the job either.
    if (m_hChildJob && !AssignProcessToJobObject(m_hChildJob, pi.hProcess)) {
        DLOG_WARN("child: could not assign pid %lu to the job, err=%lu; it will "
                  "rely on the in-child parent watchdog",
                  (unsigned long)pi.dwProcessId, (unsigned long)GetLastError());
    }
    // Grants the SPECIFIC pid the right to foreground itself, once, at any
    // later moment -- not merely "right now". Without this, -foreground's
    // SetForegroundWindow at reveal is exactly the kind of call Windows'
    // foreground lock exists to block: an unrelated process trying to steal
    // activation from whatever the user is doing. This call is what turns it
    // into a deliberate handoff instead, and it is only made when this display
    // is actually going to ask for it. forgejo#85.
    if (wantForeground)
        AllowSetForegroundWindow(pi.dwProcessId);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    ChildInstance ci;
    ci.pid = pi.dwProcessId;
    ci.hProcess = pi.hProcess;
    ci.nIndex = childIndex;
    CopyTo(ci.szDeviceName, deviceName);
    FormatTo(ci.szPipeName, L"\\\\.\\pipe\\%s_%u", childBase, ci.pid);

    // Wait for its pipe. Nothing to discover -- the name is derived from the pid
    // CreateProcess just handed back.
    const DWORD deadline = GetTickCount() + kPipeAppearTimeoutMs;
    while (GetTickCount() < deadline && !m_bChildWorkerStop.load()) {
        if (WaitForSingleObject(ci.hProcess, 0) == WAIT_OBJECT_0) {
            DLOG_ERROR("child: pid %lu exited before serving its pipe",
                       (unsigned long)ci.pid);
            CloseHandle(ci.hProcess);
            SetChildSpawnFailure(deviceName, L"exited_before_pipe");
            return false;
        }
        if (WaitNamedPipeW(ci.szPipeName, 250)) { ci.bReady = true; break; }
        Sleep(150);
    }

    if (!ci.bReady) {
        // Terminate rather than leave it running: an instance the parent cannot
        // reach is an unmanaged window on a display the user asked us to drive.
        DLOG_ERROR("child: pid %lu never served %ls; terminating",
                   (unsigned long)ci.pid, ci.szPipeName);
        TerminateProcess(ci.hProcess, 1);
        CloseHandle(ci.hProcess);
        SetChildSpawnFailure(deviceName, L"pipe_timeout");
        return false;
    }

    ChildConfigureNow(ci, true);
    {
        std::lock_guard<std::mutex> lock(m_childrenMutex);
        m_children.push_back(ci);
    }

    // The display is NOT handed over here. Ownership is announced by the poll
    // loop once the child has answered and been placed, so the mirror keeps the
    // monitor covered for the seconds a start-up takes.

    SetChildSpawnFailure(deviceName, nullptr);
    DLOG_INFO("child: pid %lu on %ls, pipe %ls",
              (unsigned long)ci.pid, deviceName, ci.szPipeName);
    return true;
}

// ─── Configure ─────────────────────────────────────────────────────────────────

// One random preset from a directory, top level only -- the same set SET_DIR
// gives the child, so the parent and the child are choosing from the same list.
//
// Modelled on PickRandomTextureFile in dxcontext.cpp, and deliberately a plain
// directory walk rather than a call into the engine's preset list: m_presets
// belongs to the render thread and describes the PARENT's directory, and this
// runs on the child worker for whatever directory the display was given.
template <size_t N>
static bool PickRandomPresetFile(const wchar_t* szDir, wchar_t (&szOut)[N]) {
    if (!szDir || !szDir[0]) return false;
    std::wstring base = szDir;
    if (base.back() != L'\\' && base.back() != L'/') base += L'\\';

    // One pass with an explicit extension test, rather than a mask per
    // extension. Three masks would need dedup afterwards -- FindFirstFile
    // matches 8.3 short names as well as long ones, so which files a mask
    // like "*.milk" also catches depends on whether short-name generation is
    // on for the volume. Deciding that per machine is not worth it when the
    // suffix can simply be compared.
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
        if (!dot) continue;
        if (_wcsicmp(dot, L".milk")  == 0 ||
            _wcsicmp(dot, L".milk2") == 0 ||
            _wcsicmp(dot, L".milk3") == 0)
            files.push_back(base + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (files.empty()) return false;
    CopyTo(szOut, files[rand() % files.size()].c_str());
    return true;
}

// One random IMMEDIATE subfolder of szDir, or false if it has none.
//
// Immediate rather than recursive on purpose: <Random> for a directory exists
// so several displays land on DIFFERENT folders, and a recursive walk of a
// library organised by artist would keep picking leaf folders holding two
// presets each. One level down is the level a person actually organises at.
template <size_t N>
static bool PickRandomSubdir(const wchar_t* szDir, wchar_t (&szOut)[N]) {
    if (!szDir || !szDir[0]) return false;
    std::wstring base = szDir;
    if (base.back() != L'\\' && base.back() != L'/') base += L'\\';

    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        dirs.push_back(base + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (dirs.empty()) return false;
    CopyTo(szOut, dirs[rand() % dirs.size()].c_str());
    return true;
}

// ─── Preset mode sentinels → real values ──────────────────────────────────────
//
// The two per-display preset fields each hold a literal path or one of the
// kDispMode* sentinels (display_output.h). These are the ONLY two functions
// that know that; everything downstream -- the pipe, the child, the profile
// writer -- deals in resolved paths, so a sentinel can never reach a child and
// be taken for a filename.
//
// Resolution happens at SPAWN rather than when the field is set, which is the
// whole point of a mode: <Current> means the preset the parent is showing when
// this display starts, not the one it happened to be showing when the combo
// was last touched.

static bool IsDispMode(const wchar_t* s, const wchar_t* mode) {
    return s && _wcsicmp(s, mode) == 0;
}

std::wstring Engine::ResolveDisplayPresetDir(const DisplayOutputConfig& cfg) const {
    const wchar_t* v = cfg.szPresetDir;

    // Empty is the pre-v3 form of <Default>, and always meant "inherit".
    if (!v[0] || IsDispMode(v, kDispModeDefault))
        return m_szPresetDir;

    if (IsDispMode(v, kDispModeRandom)) {
        wchar_t pick[MAX_PATH] = {};
        if (PickRandomSubdir(m_szPresetDir, pick))
            return pick;
        // A library with no subfolders is not an error, it is a library with
        // nothing to choose between. Fall back rather than leave the child
        // with no directory at all.
        DLOG_INFO("display preset dir <Random>: %ls has no subfolders; using it "
                  "directly", m_szPresetDir);
        return m_szPresetDir;
    }

    if (IsDispMode(v, kDispModeCurrent)) {
        // m_szCurrentPresetFile is documented "w/o path" but is a full path on
        // the paths that matter here (LoadPreset stores what it was given), so
        // it is tested rather than trusted -- with recursive preset lists on,
        // the folder it names can be a subfolder of m_szPresetDir, which is
        // exactly what makes <Current> different from <Default>.
        return CurrentPresetDir();
    }

    return v;   // a literal path
}

std::wstring Engine::ResolveDisplayStartupPreset(const DisplayOutputConfig& cfg,
                                                 const wchar_t* dir) const {
    const wchar_t* v = cfg.szStartupPreset;

    auto randomFrom = [dir]() -> std::wstring {
        wchar_t pick[MAX_PATH] = {};
        if (dir && PickRandomPresetFile(dir, pick)) return pick;
        return std::wstring();   // caller keeps whatever the child loaded
    };

    // Empty is the pre-v3 form, and it meant a random pick -- NOT <Default>.
    // Mapping it anywhere else would silently change what every existing
    // settings.ini and display profile does.
    if (!v[0] || IsDispMode(v, kDispModeRandom))
        return randomFrom();

    if (IsDispMode(v, kDispModeDefault)) {
        // The global startup preset, on the same terms the main window uses it:
        // the checkbox has to be on and the path non-empty, else it is random.
        if (m_bEnablePresetStartup && m_szPresetStartup[0])
            return m_szPresetStartup;
        return randomFrom();
    }

    if (IsDispMode(v, kDispModeCurrent)) {
        if (!m_szCurrentPresetFile[0])
            return randomFrom();
        // A bare filename is joined to the directory this display will use, so
        // <Current> still names a file the child can open.
        if (wcschr(m_szCurrentPresetFile, L'\\') ||
            wcschr(m_szCurrentPresetFile, L'/'))
            return m_szCurrentPresetFile;
        std::wstring base = (dir && dir[0]) ? dir : m_szPresetDir;
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
            base += L'\\';
        return base + m_szCurrentPresetFile;
    }

    return v;   // a literal path
}

void Engine::ChildConfigureNow(ChildInstance& ci, bool bInitial) {
    // Copy every value out of the config FIRST, then talk to the pipe holding
    // nothing. Sending takes milliseconds per message and there are six of
    // them; iterating m_displayOutputs across all of that would widen the
    // window below for no benefit.
    //
    // Every structural mutation of m_displayOutputs (insert/erase, i.e.
    // anything that can reallocate the buffer) is now render-thread-only and
    // takes m_displayOutputsMutex around just that mutation -- this is the
    // one reader on a different thread, so the lock below is all it needs to
    // take (#27; the field-level SET_MIRROR_* writes this used to cite
    // don't reallocate and this loop only reads the fields they touch, so
    // they were never actually the hazard).
    wchar_t dir[MAX_PATH] = {};
    wchar_t startup[MAX_PATH] = {};
    // The RAW field values, kept alongside the resolved ones purely so the log
    // below can say which mode produced which path. Diagnosing "why did this
    // display start there" needs both halves.
    wchar_t cfgDir[MAX_PATH] = {};
    wchar_t cfgStartup[MAX_PATH] = {};
    float   interval = 0.0f;
    bool    sequential = false;
    bool    presetLock = false;
    bool    fullscreen = false;
    int     opacity = 100;
    bool    found = false;
    RECT    mon = {};
    DisplayOutputConfig cfg;   // POD-ish copy, so resolution (below, possibly a
                               // directory scan for <Random>) runs unlocked
    {
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor) continue;
            if (wcscmp(out.config.szDeviceName, ci.szDeviceName) != 0) continue;
            cfg = out.config;
            found = true;
            break;
        }
    }
    if (!found) return;

    // Both fields may hold a mode rather than a path; resolve here, once, so
    // nothing below this line has to know that. The dir is resolved first
    // because <Random> and <Current> for the preset pick FROM it.
    CopyTo(cfgDir, cfg.szPresetDir);
    CopyTo(cfgStartup, cfg.szStartupPreset);
    CopyTo(dir, ResolveDisplayPresetDir(cfg).c_str());
    CopyTo(startup, ResolveDisplayStartupPreset(cfg, dir).c_str());
    interval = EffectiveChildInterval(cfg);
    sequential = cfg.bSequentialOrder;
    presetLock = EffectiveChildPresetLock(cfg);
    opacity = cfg.nOpacity;
    fullscreen = cfg.bFullscreen;
    mon = cfg.rcMonitor;

    // SET_DIR='s own handler (engine_messages.cpp) offloads the real work --
    // validation, the settings.ini write, UpdatePresetList's directory scan
    // -- to a DETACHED background thread, so it never stalls the child's
    // render thread. ChildSend's fire-and-forget pattern only guarantees the
    // MESSAGE was written, not that the directory has actually changed by
    // the time this function's very next send (PRESET=, below) reaches the
    // child: PRESET='s handler changes m_szPresetDir itself, synchronously,
    // when the paths differ -- and if SET_DIR='s slower background thread
    // finishes afterward, it silently overwrites PRESET='s directory back to
    // this one. Measured 100% reproducible, not a rare race: the detached
    // thread's directory scan reliably outlives the synchronous handler it
    // was sent before (forgejo#90). Wait for the reply SET_DIR='s handler
    // sends once its thread actually finishes, so everything sent after
    // this line lands on the directory this call actually asked for.
    {
        std::wstring reply;
        ChildRequest(ci.szPipeName, std::wstring(L"SET_DIR=") + dir,
                     L"SET_DIR_RESULT=", reply, 20000);
    }
    ChildSend(ci.szPipeName, L"SET_TIME_BETWEEN_PRESETS=" + std::to_wstring(interval));
    ChildSend(ci.szPipeName, std::wstring(L"SET_PRESET_ORDER=") +
                             (sequential ? L"1" : L"0"));

    // A child inherits bPresetLockOnAtStartup from the shared settings.ini, so
    // the lock is always sent explicitly rather than left to a global startup
    // preference. What is sent is the RESOLVED value: the main window's current
    // lock, unless this display has pinned its own.
    //
    // It used to be a hardcoded 0. That made the lock a main-window-only
    // gesture: with the lock on, the main window sat still while every child
    // went on cycling, which reads as displays advancing on their own.
    ChildSend(ci.szPipeName, presetLock ? L"SET_PRESET_LOCK=1"
                                        : L"SET_PRESET_LOCK=0");

    // The HUD state, for the same reason as the lock: settings.ini is read
    // once at startup and cannot carry a later change. Debug info is the case
    // that needs it most -- it is neither loaded from settings.ini nor written
    // to it, so a child spawned while the overlay is up would come back
    // without it and there would be no way to ask for it.
    //
    // Sent here rather than through RelayHudStateToChildren, which addresses
    // children by device through the worker queue: this child's pipe is right
    // here and its display's entry may not be reconciled yet.
    ChildSend(ci.szPipeName, m_bDisableMirrorHud ? L"SET_HUD_PRIMARY_ONLY=1"
                                                 : L"SET_HUD_PRIMARY_ONLY=0");
    ChildSend(ci.szPipeName, m_bShowPresetInfo ? L"SET_SHOW_PRESET_INFO=1"
                                               : L"SET_SHOW_PRESET_INFO=0");
    ChildSend(ci.szPipeName, m_bShowFPS ? L"SET_SHOW_FPS=1"
                                        : L"SET_SHOW_FPS=0");
    ChildSend(ci.szPipeName, m_bShowDebugInfo ? L"SET_SHOW_DEBUG=1"
                                              : L"SET_SHOW_DEBUG=0");

    // Keep on top, for the same reason as the lock and the HUD: this display's
    // window belongs to the child, so the parent's style pass cannot reach it,
    // and settings.ini carries the GLOBAL flag rather than this display's
    // (#338). Without this the checkbox was inert on a child-owned display --
    // and unlike a mirror, toggling it off and on did not fix it either.
    //
    // The child's own m_bAlwaysOnTop is what its WM_MW_RAISE_WINDOW consults
    // before dropping back out of the topmost band, so setting it here is also
    // what stops the child lowering itself on the next raise.
    ChildSend(ci.szPipeName, cfg.bAlwaysOnTop ? L"SET_ALWAYS_ON_TOP=1"
                                              : L"SET_ALWAYS_ON_TOP=0");

    // The display's own opacity, which nothing was sending. A child inherits
    // WindowOpacity from the shared settings.ini instead, so the Opacity
    // control on the Displays tab said 100% while the child used whatever the
    // main window happened to be set to. Sent before the reveal, so the fade
    // has the right number to arrive at rather than fading to full and then
    // being corrected.
    {
        int pct = opacity;
        if (pct < 1) pct = 1;
        if (pct > 100) pct = 100;
        wchar_t buf[32];
        FormatTo(buf, L"OPACITY=%.3f", (float)pct / 100.0f);
        ChildSend(ci.szPipeName, buf);
    }

    // The rest is placement and the starting frame, which belong to a SPAWN and
    // not to a settings change. These used to run on every reconfigure, and
    // five Displays-UI controls call RequestChildReconfigure -- so ticking the
    // cycle checkbox on a running child yanked it back to its startup preset
    // and re-placed its window mid-show.
    if (!bInitial) return;

    // A child reads the SHARED settings.ini, so its own start-up loads the
    // preset the parent last saved -- which is the preset the parent is
    // showing. "Own preset" then put the same frame on both screens, which is
    // what it was reported as not doing. Something is therefore always sent.
    //
    // ResolveDisplayStartupPreset has already turned <Random> into an actual
    // file. It picks rather than sending the child a next-preset signal: the
    // child's advance honours ITS order setting, so a display set to sequential
    // would start at the same index as every other one. A pick per child is
    // per-display whatever the order, and a sequential display then walks on
    // from a different place each run.
    if (startup[0]) {
        ChildSend(ci.szPipeName, std::wstring(L"PRESET=") + startup);
        DLOG_INFO("child: pid %lu on %ls starts on %ls (dir '%ls' from '%ls', "
                  "preset from '%ls')", (unsigned long)ci.pid, ci.szDeviceName,
                  startup, dir, cfgDir, cfgStartup);
    } else {
        DLOG_WARN("child: pid %lu on %ls resolved '%ls' to no preset and no "
                  "preset files were found in %ls; it keeps the one it loaded "
                  "from the shared settings",
                  (unsigned long)ci.pid, ci.szDeviceName, cfgStartup, dir);
    }

    // Placement is NOT done here. See ChildPlaceNow: the pipe answers long
    // before the child's window has finished starting, and its own start-up
    // then restores the geometry saved in the shared settings.ini, overwriting
    // anything sent at this point. It is applied on the first successful poll
    // instead, which is proof the child is actually up.
}

// ─── Retire ────────────────────────────────────────────────────────────────────

// Dissolve a child out over the mirror that is taking its display back (#230).
//
// The other direction already existed: a child spawning over a live mirror
// fades IN over nChildFadeMs, precisely so the handover reads as a dissolve
// rather than a jump. Going the other way it simply vanished, which is the same
// jump with the sign flipped -- and with presets now moving displays between
// process and mirror automatically, that transition happens without anyone
// pressing anything.
//
// Driven from the PARENT rather than by asking the child to fade itself: the
// parent already owns the child's placement, SetLayeredWindowAttributes works
// across processes on a window that has WS_EX_LAYERED (the fade-in put it
// there), and a child that is about to be shut down is the worst possible thing
// to depend on for a smooth exit. No new IPC verb, and nothing to go wrong if
// the child is already unresponsive.
//
// Runs on the child worker thread, so sleeping here costs the render thread
// nothing.
static void FadeChildWindowOut(DWORD pid, int fadeMs, float fromOpacity) {
    if (pid == 0 || fadeMs <= 0) return;

    struct Ctx { DWORD pid; HWND found; } ctx = { pid, NULL };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        DWORD owner = 0;
        GetWindowThreadProcessId(h, &owner);
        if (owner != c->pid) return TRUE;
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"Direct3DWindowClass") != 0) return TRUE;
        c->found = h;
        return FALSE;
    }, (LPARAM)&ctx);
    if (!ctx.found || !IsWindow(ctx.found)) return;

    // Same step as the fade-in, so the two directions match by eye.
    const int kStepMs = 16;
    const int steps = (fadeMs + kStepMs - 1) / kStepMs;
    const float top = max(0.0f, min(1.0f, fromOpacity));
    for (int i = 1; i <= steps; i++) {
        if (!IsWindow(ctx.found)) return;
        const float t = 1.0f - (float)i / (float)steps;
        SetLayeredWindowAttributes(ctx.found, 0,
                                   (BYTE)(top * t * 255.0f + 0.5f), LWA_ALPHA);
        Sleep(kStepMs);
    }
}

void Engine::ChildKillNow(const wchar_t* deviceName) {
    ChildInstance ci;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_childrenMutex);
        auto it = std::find_if(m_children.begin(), m_children.end(),
            [&](const ChildInstance& c) {
                return wcscmp(c.szDeviceName, deviceName) == 0;
            });
        if (it != m_children.end()) { ci = *it; found = true; m_children.erase(it); }
    }
    // Release the display first: the mirror is what will show this monitor once
    // the child is gone, and letting it come back while the child is still
    // shutting down is a brief overlap rather than a black screen.
    SetChildDisplayReady(deviceName, false);
    // Nothing is being started here any more, so neither answer to "why is
    // there no child" applies. Leaving a failure record behind would make a
    // display somebody switched to mirror keep reporting a spawn that failed
    // before they changed their mind.
    ClearChildSpawnPending(deviceName);
    SetChildSpawnFailure(deviceName, nullptr);
    if (!found) return;

    // The mirror was released just above, so it is drawing this display again
    // underneath. Dissolve the child out over it before shutting it down --
    // otherwise the handover is a jump, which is exactly what the fade-in
    // exists to avoid in the other direction.
    FadeChildWindowOut(ci.pid,
                       (m_nChildFadeMs > 0) ? m_nChildFadeMs : 0,
                       fOpacity);

    // SHUTDOWN runs the child's real exit path. TerminateProcess skips it, so it
    // is the fallback, never the method.
    ChildSend(ci.szPipeName, L"SHUTDOWN");
    if (ci.hProcess) {
        if (WaitForSingleObject(ci.hProcess, 8000) != WAIT_OBJECT_0) {
            DLOG_WARN("child: pid %lu did not exit on SHUTDOWN; terminating",
                      (unsigned long)ci.pid);
            TerminateProcess(ci.hProcess, 0);
        }
        CloseHandle(ci.hProcess);
    }
    DLOG_INFO("child: pid %lu on %ls retired", (unsigned long)ci.pid, deviceName);
}

// ─── Poll ──────────────────────────────────────────────────────────────────────

void Engine::ChildPollAll() {
    std::vector<ChildInstance> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_childrenMutex);
        snapshot = m_children;
    }

    for (auto& ci : snapshot) {
        if (ci.hProcess && WaitForSingleObject(ci.hProcess, 0) == WAIT_OBJECT_0) {
            // It died. Mark it and report it -- do NOT respawn here. A child
            // that dies during startup would otherwise flap forever, and a
            // window reappearing every second is worse than one that is gone.
            std::lock_guard<std::mutex> lock(m_childrenMutex);
            for (auto& c : m_children) {
                if (c.pid != ci.pid) continue;
                if (!c.bFailed)
                    DLOG_ERROR("child: pid %lu on %ls exited unexpectedly",
                               (unsigned long)ci.pid, ci.szDeviceName);
                c.bFailed = true;
                c.bReady = false;
            }
            // Its display is unowned again, so the mirror may come back rather
            // than leaving the monitor showing whatever the dead child left.
            SetChildDisplayReady(ci.szDeviceName, false);
            continue;
        }
        if (!ci.bReady || ci.bFailed)
            continue;

        // Ask what it is playing, so the Displays list can show it. Half the
        // original complaint was not knowing which preset a display was on.
        std::wstring reply;
        if (!ChildRequest(ci.szPipeName, L"GET_PRESET_STATUS", L"PRESET_STATUS",
                          reply, 400))
            continue;

        // PRESET_STATUS|file=<path>|desc=...|...
        const wchar_t* f = wcsstr(reply.c_str(), L"|file=");
        if (!f) continue;
        f += 6;
        const wchar_t* end = wcschr(f, L'|');
        std::wstring path = end ? std::wstring(f, end) : std::wstring(f);
        SetChildCurrentPreset(ci.pid, path.c_str());

        // And the rate it is achieving, out of the SAME reply (#246). A child
        // renders in another process, so nothing the parent measures -- every
        // per-surface figure in DIAG_MIRRORS is in-process -- can see it, and
        // a display moved to its own process disappeared from every frame-rate
        // number the app could produce. Free here: this reply was already
        // being fetched and parsed once per poll.
        if (const wchar_t* fp = wcsstr(reply.c_str(), L"|fps=")) {
            const float v = (float)_wtof(fp + 5);
            if (v >= 0.0f) SetChildFps(ci.pid, v);
        }

        // Placement converges rather than being fired once.
        //
        // Answering a request is NOT proof the window is settled: the child's
        // IPC thread replies while its start-up is still running, and that
        // start-up restores the geometry saved in the shared settings.ini,
        // landing on top of whatever the parent sent. Measured: a placement
        // accepted at t+3s was silently undone, leaving the child on the
        // parent's monitor.
        //
        // So ask the child where it actually IS, and re-place it until the
        // answer is the display it was assigned. Ownership -- and therefore
        // retiring the mirror -- is announced only once that is true, which is
        // what makes this a replacement rather than a gap (forgejo#38).
        if (!ci.bPlaced) {
            bool onTarget = false;
            bool visible = false;
            std::wstring diag;
            if (ChildRequest(ci.szPipeName, L"DIAG_MIRRORS", L"MIRRORS", diag, 900)) {
                const wchar_t* ro = wcsstr(diag.c_str(), L"render_on=");
                if (ro) {
                    ro += 10;
                    const wchar_t* comma = wcschr(ro, L',');
                    const std::wstring dev = comma ? std::wstring(ro, comma)
                                                   : std::wstring(ro);
                    onTarget = (_wcsicmp(dev.c_str(), ci.szDeviceName) == 0);
                }
                // revealed=, not visible=: a child fades in over the mirror,
                // and the mirror has to stay underneath for the whole fade.
                visible = wcsstr(diag.c_str(), L",revealed=1") != nullptr;
            }

            // On the right monitor is no longer proof there is anything to see.
            // A child built hidden is on target from its first instant, so
            // handing the display over on that alone would retire the mirror in
            // favour of a window that has not drawn yet -- the black gap this
            // whole sequence exists to avoid. Wait for the frame.
            if (onTarget && !visible)
                continue;

            if (!onTarget) {
                ChildPlaceNow(ci);      // and check again next poll
            } else {
                bool marked = false;
                {
                    std::lock_guard<std::mutex> lock(m_childrenMutex);
                    for (auto& c : m_children)
                        if (c.pid == ci.pid) { c.bPlaced = true; marked = true; }
                }
                if (marked) {
                    SetChildDisplayReady(ci.szDeviceName, true);
                    DLOG_INFO("child: pid %lu settled on %ls; mirror stands down",
                              (unsigned long)ci.pid, ci.szDeviceName);
                }
            }
        }
    }
}

// ─── Request API (callable from any thread) ────────────────────────────────────

void Engine::PushChildCommand(ChildCmd c, const wchar_t* deviceName,
                              const wchar_t* payload) {
    if (m_bChildMode || !deviceName || !*deviceName) return;

    // Queueing into a void used to be silent, which is what made forgejo#57 a
    // whole diagnosis rather than a log line. The worker's lifetime is now the
    // process's, so this should be unreachable -- and if it ever is reached
    // again it says so instead of accepting the command and dropping it.
    if (!m_hChildWorker)
        DLOG_ERROR("child: no worker thread; '%ls' for %ls is queued but "
                   "nothing will act on it",
                   c == ChildCmd::Spawn ? L"spawn"
                   : c == ChildCmd::Kill ? L"kill"
                   : c == ChildCmd::Reconfigure ? L"reconfigure" : L"send",
                   deviceName);

    ChildCommand cmd;
    cmd.cmd = c;
    CopyTo(cmd.szDeviceName, deviceName);
    if (payload) cmd.sPayload = payload;
    {
        std::lock_guard<std::mutex> lock(m_childCmdMutex);
        m_childCmdQueue.push(cmd);

        // A spawn is "pending" from the moment it is accepted until the worker
        // has finished acting on it, so a remote can show "starting" for the
        // seconds a CreateProcess-plus-pipe-wait actually takes and something
        // else for a display nothing intends to start.
        if (c == ChildCmd::Spawn) {
            bool have = false;
            for (auto& d : m_childSpawnPending)
                if (_wcsicmp(d.c_str(), deviceName) == 0) { have = true; break; }
            if (!have) m_childSpawnPending.push_back(deviceName);
            // Asking again clears the last failure: the answer to "why is there
            // no child" is now this attempt, not the previous one.
            m_childSpawnFailed.erase(
                std::remove_if(m_childSpawnFailed.begin(), m_childSpawnFailed.end(),
                    [deviceName](const std::pair<std::wstring, std::wstring>& f) {
                        return _wcsicmp(f.first.c_str(), deviceName) == 0;
                    }),
                m_childSpawnFailed.end());
        }
    }
    DLOG_INFO("child: queued %ls for %ls",
              c == ChildCmd::Spawn ? L"spawn"
              : c == ChildCmd::Kill ? L"kill"
              : c == ChildCmd::Reconfigure ? L"reconfigure" : L"send",
              deviceName);
    if (m_hChildCmdEvent) SetEvent(m_hChildCmdEvent);
}

// A spawn stops being pending when the worker has finished with it, whether it
// produced a child or a failure record. Both answers are better than "pending".
void Engine::ClearChildSpawnPending(const wchar_t* deviceName) {
    std::lock_guard<std::mutex> lock(m_childCmdMutex);
    m_childSpawnPending.erase(
        std::remove_if(m_childSpawnPending.begin(), m_childSpawnPending.end(),
            [deviceName](const std::wstring& d) {
                return _wcsicmp(d.c_str(), deviceName) == 0;
            }),
        m_childSpawnPending.end());
}

bool Engine::ChildSpawnPending(const wchar_t* deviceName) const {
    if (!deviceName) return false;
    std::lock_guard<std::mutex> lock(m_childCmdMutex);
    for (auto& d : m_childSpawnPending)
        if (_wcsicmp(d.c_str(), deviceName) == 0) return true;
    return false;
}

bool Engine::ChildSpawnFailure(const wchar_t* deviceName,
                               std::wstring& reason) const {
    if (!deviceName) return false;
    std::lock_guard<std::mutex> lock(m_childCmdMutex);
    for (auto& f : m_childSpawnFailed)
        if (_wcsicmp(f.first.c_str(), deviceName) == 0) {
            reason = f.second;
            return true;
        }
    return false;
}

void Engine::SetChildSpawnFailure(const wchar_t* deviceName,
                                  const wchar_t* reason) {
    if (!deviceName || !*deviceName) return;
    std::lock_guard<std::mutex> lock(m_childCmdMutex);
    m_childSpawnFailed.erase(
        std::remove_if(m_childSpawnFailed.begin(), m_childSpawnFailed.end(),
            [deviceName](const std::pair<std::wstring, std::wstring>& f) {
                return _wcsicmp(f.first.c_str(), deviceName) == 0;
            }),
        m_childSpawnFailed.end());
    if (reason && *reason)
        m_childSpawnFailed.emplace_back(deviceName, reason);
}

size_t Engine::ChildQueueDepth() const {
    std::lock_guard<std::mutex> lock(m_childCmdMutex);
    return m_childCmdQueue.size();
}

// The lock is global, so a child hears about every change to it -- unless that
// display has pinned its own value, which is what nPresetLock >= 0 means.
//
// Deliberately not BroadcastToChildren: that would send the same byte to a
// display the user has explicitly set to "never advance", overriding an
// override. The whole point of the per-display setting is that it wins.
void Engine::RelayPresetLockToChildren() {
    if (m_bChildMode) return;
    // m_displayOutputsMutex: reachable from the IPC thread (SET_PRESET_LOCK=
    // in engine_messages.cpp calls SetUserPresetLock, which calls this), not
    // just the render thread. #27.
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        if (!out.config.bOwnProcess) continue;
        if (out.config.nPresetLock >= 0) continue;      // pinned; not ours
        RequestChildSend(out.config.szDeviceName,
                         m_bPresetLockedByUser ? L"SET_PRESET_LOCK=1"
                                               : L"SET_PRESET_LOCK=0");
    }
}

// Push the whole HUD state to every child.
//
// Same reasoning as the preset lock above, and the same failure it fixes: all
// four of these flags were assigned directly by a UI handler or a hotkey and
// read from settings.ini only at process start, so a child kept whatever was
// on disk when it spawned. Unticking "show HUD only on the primary display"
// left every running child still hiding its HUD, because nothing could tell
// it the setting had moved.
//
// UNCONDITIONAL, unlike the lock: these have no per-display override to
// respect, so there is no "pinned; not ours" case to skip. Every child shows
// what the parent shows.
//
// Sent whole rather than one changed flag at a time, and cheaply enough to do
// so: four short messages down an already-open pipe, only when something
// actually changed or a child has just come up. Sending the complete state
// means a child that missed a message converges on the next one instead of
// staying wrong until the next restart.
void Engine::RelayHudStateToChildren() {
    if (m_bChildMode) return;
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        if (!out.config.bOwnProcess) continue;
        SendHudStateToChild(out.config.szDeviceName);
    }
}

// The same four messages, to ONE child.
//
// Split out because a freshly spawned child needs them too, and needs them
// from the spawn path rather than from a later toggle: m_bShowDebugInfo is
// neither loaded from settings.ini nor written to it -- see the commented-out
// line in LoadSettings -- so a child always starts with the debug overlay off
// and would never show it, no matter what the parent had on screen.
void Engine::SendHudStateToChild(const wchar_t* device) {
    if (m_bChildMode || !device || !device[0]) return;
    RequestChildSend(device, m_bDisableMirrorHud ? L"SET_HUD_PRIMARY_ONLY=1"
                                                 : L"SET_HUD_PRIMARY_ONLY=0");
    RequestChildSend(device, m_bShowPresetInfo ? L"SET_SHOW_PRESET_INFO=1"
                                               : L"SET_SHOW_PRESET_INFO=0");
    RequestChildSend(device, m_bShowFPS ? L"SET_SHOW_FPS=1"
                                        : L"SET_SHOW_FPS=0");
    RequestChildSend(device, m_bShowDebugInfo ? L"SET_SHOW_DEBUG=1"
                                              : L"SET_SHOW_DEBUG=0");
}

// ─── Child -> parent: the keyboard ─────────────────────────────────────────────
//
// A child owns no external interfaces, so it binds no hotkeys and, with testing
// mode pinned on for its whole life, swallowed every key it received. But it is
// a real window on a real monitor: click it and it has the focus, and from then
// on the keyboard did nothing at all -- no tool windows, no next preset, no way
// back to the main window short of clicking it. Reported as "children should
// forward keypresses to parent not completely ignore them ... otherwise can't
// bring up parent tool windows".
//
// So the key is handed to the parent and the PARENT's bindings decide. Not
// executed locally: a child deliberately has no hotkey table of its own, and
// giving it one would mean two places to configure the same key.
//
// PostMessage, not the pipe. This runs on the child's UI thread inside a
// keystroke, and a pipe write is a blocking round trip to a process that might
// be busy -- a slow parent would stutter the child's input. The message is
// fire-and-forget by construction.
//
// The MOUSE is deliberately NOT forwarded. Presets read mouse position and
// buttons (the [MOUSE] presets), and those belong to whichever preset is on
// that display -- which is the child's own, not the parent's.
bool Engine::ForwardKeyToParent(unsigned int vk, unsigned int mods) {
    if (!m_bChildMode || !m_nParentPid) return false;

    // Cached: EnumWindows on every keystroke is wasteful, and the parent's
    // render window does not move between processes. Re-resolved if it dies.
    static HWND s_hParent = NULL;
    if (!s_hParent || !IsWindow(s_hParent)) {
        struct Ctx { DWORD pid; HWND found; } ctx = { m_nParentPid, NULL };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            Ctx* c = (Ctx*)lp;
            DWORD owner = 0;
            GetWindowThreadProcessId(h, &owner);
            if (owner != c->pid) return TRUE;
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, L"Direct3DWindowClass") != 0) return TRUE;
            c->found = h;
            return FALSE;
        }, (LPARAM)&ctx);
        s_hParent = ctx.found;
    }
    if (!s_hParent) return false;

    PostMessage(s_hParent, WM_MW_CHILD_KEY, (WPARAM)vk, (LPARAM)mods);
    return true;
}

void Engine::SetUserPresetLock(bool bLocked) {
    // Early out on no change, because HK_SCROLL_LOCK assigns the key state on
    // every press and the auto-lock-on-silence timer re-evaluates constantly --
    // without this, each would queue a pipe message per call.
    if (m_bPresetLockedByUser == bLocked) return;
    m_bPresetLockedByUser = bLocked;
    RelayPresetLockToChildren();
}

// Does this preset's shader carry a loop big enough to be dangerous? (#230)
//
// Static, and deliberately so: the cost that kills the device arrives with no
// warning -- measured repeatedly at milliseconds to 2400 ms in ONE frame -- so
// anything that measures first and reacts afterwards is too late by
// construction. Three such mitigations were built and all three failed that
// way. The shader text, by contrast, can be read before a single pixel is drawn.
//
// The signal is a loop with a large literal bound. In Clouds1b:
//
//     for(q.zy=float2(-1,-1);i++<99.;){      99 iterations
//         for(e=--p.y;s<6e3;s+=s)            nested, ~13 more
//
// roughly 1,300 iterations per pixel, times 8.3M pixels.
//
// A loop ALONE is far too common to act on -- 404 of the 803 presets in the
// library have one, half of them. A loop with a bound of 32 or more picks out
// 74, about 9%, which is the population worth isolating.
//
// Crude on purpose. A false positive costs one display a process it did not
// strictly need; a false negative costs the whole session. The asymmetry says
// to keep the test simple and slightly eager rather than clever.
//
// REMOVED 2026-09-12. The asymmetry above is real but the arithmetic behind it
// was not: "slightly eager" turned out to be 216 of 1,532 presets, one in
// seven, each spawning child processes it did not need. And the premise --
// that measuring is always too late because the spike has no ramp -- is true
// of the SPIKE and false of the COST. gpuUs reports what the GPU charged for a
// surface's frame, steadily and from the first completed frame: smoke0 2-3 ms,
// Clouds1b 26-29 ms. That separates them where the shader text cannot, so
// PresetRisksTheDevice reads it instead.

// Is this preset expensive enough that the extra displays are better off in
// their own processes?
//
// MEASURED, not read off the shader.
//
// This used to be ShaderHasBigLoop on the warp and comp text, which is wrong
// about which presets are expensive: it fires on any loop header carrying a
// literal >= 32, which is 216 of 1,532 presets here, and 'Saturday smoke0'
// outscores Clouds1b on static cost while costing 2-3 ms of GPU against
// Clouds1b's 26-29 ms. Reading the shader cannot tell those apart; the surface
// timestamps can.
unsigned Engine::MeasuredPeakGpuUs() const {
    // The mirror surfaces' high-water mark, and the PRIMARY's own frame, which
    // is the half that makes this answerable at all (#245).
    //
    // m_nMirrorPeakGpuUs is fed only by INDEPENDENT mirror surfaces. With none
    // live -- every extra display already a child, or copy mirrors, which have
    // no worker and no timestamps, or skipped as the render window's own
    // monitor -- it stays 0 however expensive the preset is. Measured: the same
    // preset reads surfaces=0 peakGpuUs=0 as a copy mirror and 30542 us as an
    // independent one.
    //
    // The primary renders that same preset and is never absent, so including it
    // means a reading always exists a frame or two after a preset load, and
    // "nothing was measured" stops being a state the gate has to guess about.
    unsigned peak = m_nMirrorPeakGpuUs.load(std::memory_order_relaxed);
    if (m_lpDX) {
        const unsigned prim = m_lpDX->LastFrameGpuUs();
        if (prim > peak) peak = prim;
    }
    return peak;
}

// PresetRisksTheDevice and ApplyGpuRiskIsolation lived here and are gone
// (#261). They moved a display into its own process when the measured GPU cost
// crossed kIsolateGpuUs; the answer to an expensive preset is now adaptive rate
// balancing in engine_displays.cpp, because Windows classifies QoS from the
// focused window and only one process can own it -- so a child was throttled
// to about 66 fps precisely when it was asked to carry the load.
//
// The user-chosen own-process tier is untouched. What is gone is the app
// deciding it on its own.

void Engine::RequestChildSpawn(const wchar_t* d) {
    // UN-NEUTERED (#230). #186 phase 4 short-circuited this with a log line and
    // a bare `return`, on the reasoning that a display wanting its own preset
    // gets a MirrorSurface rather than a second MDropDX12.exe. Phase 6 then
    // deleted the file; this restores it, and the early-out with it, so the
    // early-out has to go or the verb is a no-op that reports success.
    //
    // Why the in-process answer is not enough after all: every surface shares
    // one device and one command queue, so all of them are in the SAME command
    // packet, and the GPU watchdog kills a packet -- not a surface. One
    // preset's expensive frame therefore takes the whole session down.
    //
    // Measured on YoheiNishitsuji - MilkDrop2077 - Clouds1b at 2160x3840:
    //
    //     primary alone                    survives 150 s
    //     + one in-process mirror          survives 90 s
    //     + two in-process mirrors         DEAD in 13 s
    //
    // and MilkDrop 3 PRO renders the same preset at the same size at 36 fps
    // against our 33, so this is not our per-frame cost being poor -- it is
    // three surfaces sharing one packet where the reference engine has one.
    //
    // A child owns its own device and queue, so its bad frame is its own: the
    // display goes, the session stays. That is worth a process for the ~9% of
    // presets whose shaders carry a big loop, and it is not worth one for the
    // rest -- children are QoS-throttled to about 66 fps when unfocused, which
    // is exactly why phase 4 moved away from them. Selective, not a reversal.

    // -nostartupchildren, gated HERE rather than at each caller, because
    // startup has two of them and gating the obvious one is not enough: the
    // loop in engine.cpp is the visible path, but the startup display profile
    // reaches this too (LoadDisplayProfile brings running children into line
    // with what the profile asked for), and that one produced the children
    // anyway. Both merely enqueue -- the worker does not exist yet -- so the
    // queue is the single point every startup spawn passes through.
    //
    // The latch is dropped once startup is done, so a spawn asked for later
    // still works. See Engine::m_bNoStartupChildren.
    if (m_bNoStartupChildren) {
        DLOG_INFO("child: -nostartupchildren, so the spawn queued for %ls at "
                  "startup is dropped", d);
        return;
    }
    PushChildCommand(ChildCmd::Spawn, d);
}
void Engine::RequestChildKill(const wchar_t* d)        { PushChildCommand(ChildCmd::Kill, d); }

int Engine::SuspendChildrenForDisplayToggle() {
    // Everything currently running, whatever the config says. A child that is
    // up is a window on a screen, and this key is about the screens.
    std::vector<std::wstring> live;
    {
        std::lock_guard<std::mutex> lock(m_childrenMutex);
        for (const auto& ci : m_children)
            if (!ci.bFailed && ci.szDeviceName[0])
                live.emplace_back(ci.szDeviceName);
    }
    if (live.empty())
        return 0;

    // Appended, not replaced. Toggling mirror on and then stretch on must not
    // lose the first set -- the second call finds nothing running and would
    // otherwise overwrite the record with an empty one.
    for (const auto& d : live) {
        bool have = false;
        for (const auto& seen : m_childrenSuspended)
            if (_wcsicmp(seen.c_str(), d.c_str()) == 0) { have = true; break; }
        if (!have) m_childrenSuspended.push_back(d);
        RequestChildKill(d.c_str());
    }
    DLOG_INFO("display toggle: stood down %d child instance%ls; the display "
              "settings are untouched, so toggling back restores them",
              (int)live.size(), live.size() == 1 ? L"" : L"s");
    return (int)live.size();
}

int Engine::ResumeChildrenAfterDisplayToggle() {
    if (m_childrenSuspended.empty())
        return 0;
    // Moved out first: a spawn request can fail and log, and re-entering here
    // with the list half-consumed would ask twice for the same display.
    std::vector<std::wstring> restore;
    restore.swap(m_childrenSuspended);

    int asked = 0;
    for (const auto& d : restore) {
        // Only if the display still wants one. Between the two keypresses the
        // user may have set that display to mirror or off in the Displays
        // window, and putting a child back over the top of that would be this
        // toggle overruling a deliberate choice.
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor) continue;
            if (_wcsicmp(out.config.szDeviceName, d.c_str()) != 0) continue;
            if (out.config.bEnabled && out.config.bOwnProcess) {
                RequestChildSpawn(d.c_str());
                asked++;
            } else {
                DLOG_INFO("display toggle: %ls is no longer set to run its own "
                          "instance, so it is left as it is now", d.c_str());
            }
            break;
        }
    }
    DLOG_INFO("display toggle: asked %d display%ls to run its own instance again",
              asked, asked == 1 ? L"" : L"s");
    return asked;
}
void Engine::RequestChildReconfigure(const wchar_t* d) { PushChildCommand(ChildCmd::Reconfigure, d); }

// One message to one child, off the caller's thread. The worker already blocks
// on pipes for a living, so a slow or dead child cannot stall a render frame or
// a UI click.
void Engine::RequestChildSend(const wchar_t* d, const wchar_t* payload) {
    PushChildCommand(ChildCmd::Send, d, payload);
}

int Engine::GetChildCount() const {
    std::lock_guard<std::mutex> lock(m_childrenMutex);
    return (int)m_children.size();
}

// ─── Placement ─────────────────────────────────────────────────────────────────

// Put a child onto its monitor and take the frame off it. Called once, from the
// poll loop, after the child has answered a request -- never straight after the
// pipe appears, because the child's own start-up restores its saved geometry
// and would land on top of anything sent before that.
//
// By rectangle, never MOVE_TO_DISPLAY: that takes a 1-based position in the
// monitor enumeration rather than the DISPLAYn number this code used to pass
// (forgejo#37), and it centres with SWP_NOSIZE so it never sized the window to
// the monitor anyway. The parent holds the exact rect, so there is no numbering
// scheme left to get wrong.
int Engine::EffectiveChildWindowMode() const {
    if (m_nChildTestWindows != kChildWindowAuto) return m_nChildTestWindows;
    // Auto. A child holds testing mode permanently (see m_bTestingMode's note),
    // so this must read the PARENT's flag -- which it does, because only a
    // parent ever spawns.
    //
    // m_bTestingMode is enough BECAUSE `-test` now raises it before startup
    // does any window work; see the SetTestingMode call in WinMain. It used to
    // run near the end of startup, which is why a startup-spawned child was
    // decided with this flag still false.
    return m_bTestingMode ? kChildWindowTiny : kChildWindowNormal;
}

bool Engine::TestChildRect(const wchar_t* deviceName, RECT& out) const {
    // m_displayOutputsMutex: #27. Recursive, so a caller (ChildPlaceNow)
    // that already holds it for its own read is unaffected.
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
    for (const auto& o : m_displayOutputs) {
        if (o.config.type != DisplayOutputType::Monitor) continue;
        if (wcscmp(o.config.szDeviceName, deviceName) != 0) continue;

        // A rect the harness chose wins, unconditionally. That is the whole
        // point of being able to set one: a test that puts a child at a chosen
        // spot and reads back where it landed is how per-display placement gets
        // covered at all.
        const RECT& want = o.config.rcTestWindow;
        if (want.right > want.left && want.bottom > want.top) {
            out = want;
            return true;
        }

        const RECT& m = o.config.rcMonitor;
        if (m.right <= m.left || m.bottom <= m.top) return false;

        // Bottom-right of the assigned monitor, inset a little. The corner
        // rather than the centre for the same reason the Remote window uses
        // one: it leaves the middle of the screen usable. Clamped so a monitor
        // smaller than the default still gets a window inside it.
        int w = kTestChildW, h = kTestChildH;
        if (w > m.right - m.left)  w = m.right - m.left;
        if (h > m.bottom - m.top)  h = m.bottom - m.top;
        const int inset = 24;
        out.right  = m.right  - (m.right - m.left > w + inset ? inset : 0);
        out.bottom = m.bottom - (m.bottom - m.top > h + inset ? inset : 0);
        out.left   = out.right - w;
        out.top    = out.bottom - h;
        return true;
    }
    return false;
}

void Engine::ChildPlaceNow(const ChildInstance& ci) {
    RECT mon = {};
    bool fullscreen = false;
    bool found = false;
    {
        // m_displayOutputsMutex: #27.
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor) continue;
            if (wcscmp(out.config.szDeviceName, ci.szDeviceName) != 0) continue;
            mon = out.config.rcMonitor;
            fullscreen = out.config.bFullscreen;
            found = true;
            break;
        }
    }
    if (!found) return;

    // A test child is placed by the SAME rules that built it, or this undoes
    // them a second later. This function runs off a poll, and everything below
    // is about covering a monitor completely and then raising the result --
    // exactly the three things a test window must not do. A child built tiny
    // and then placed by the code below snapped to fullscreen and jumped to the
    // front on the next tick, which is the bug this branch exists to avoid.
    const int winMode = EffectiveChildWindowMode();
    if (winMode == kChildWindowHidden) {
        // Nothing to place, and nothing to send. The child was built with
        // -hidewindow and never shows itself, so every verb below would undo
        // that: SET_WINDOW and SET_BORDERLESS_FS are harmless on a window
        // nobody can see, but RAISE_WINDOW is precisely how a hidden child
        // stops being hidden. SET_MINIMIZED is not the answer either -- a
        // window that has never been shown would acquire a taskbar button on
        // its way to being minimised.
        return;
    }
    if (winMode == kChildWindowTiny) {
        RECT r;
        if (TestChildRect(ci.szDeviceName, r)) {
            ChildSend(ci.szPipeName, L"SET_BORDERLESS_FS=0");
            wchar_t place[96];
            FormatTo(place, L"SET_WINDOW=%d,%d,%d,%d",
                     r.left, r.top, r.right - r.left, r.bottom - r.top);
            ChildSend(ci.szPipeName, place);
            // Deliberately no RAISE_WINDOW: a test window belongs at the bottom
            // of the z-order, which is where SW_SHOWNOACTIVATE already left it.
            DLOG_INFO("child: pid %lu test-placed on %ls (%d,%d) %dx%d",
                      (unsigned long)ci.pid, ci.szDeviceName, r.left, r.top,
                      r.right - r.left, r.bottom - r.top);
        }
        return;
    }

    // Borderless FIRST, then the rectangle. ToggleBorderlessFullscreen sizes to
    // the monitor's WORK AREA, deliberately, so the main window never hides the
    // taskbar -- which means running it after SET_WINDOW silently undid the
    // placement and left the child 48px short on the monitor holding the
    // taskbar. A display handed over to a child should be covered completely,
    // so the rectangle is applied last and wins.
    ChildSend(ci.szPipeName, fullscreen ? L"SET_BORDERLESS_FS=1"
                                        : L"SET_BORDERLESS_FS=0");

    if (mon.right > mon.left && mon.bottom > mon.top) {
        wchar_t place[96];
        FormatTo(place, L"SET_WINDOW=%d,%d,%d,%d",
                 mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top);
        ChildSend(ci.szPipeName, place);
    }

    // And bring it to the front, once.
    //
    // Spawned with SW_SHOWNOACTIVATE so the parent keeps the foreground, a child
    // lands at the BOTTOM of the z-order. On an empty monitor that is
    // invisible; on the monitor the user actually works on, it renders
    // perfectly underneath their editor and the display looks dead. Observed
    // exactly that -- DISPLAY1 showed its child while DISPLAY3 showed VS Code
    // with the child behind it.
    //
    // Once, and no more. Pinning a child topmost was tried and was worse than
    // the bug: it covered everything else on that monitor with no way to get at
    // it, and it overrode an always-on-top setting the user had turned off.
    // RAISE_WINDOW raises through the topmost band and then restores whatever
    // the flag says, so the child arrives in front and afterwards behaves like
    // any other window.
    ChildSend(ci.szPipeName, L"RAISE_WINDOW");

    DLOG_INFO("child: pid %lu placed on %ls (%d,%d)-(%d,%d)%ls",
              (unsigned long)ci.pid, ci.szDeviceName,
              mon.left, mon.top, mon.right, mon.bottom,
              fullscreen ? L" borderless" : L"");
}

// ─── Which displays a child currently owns ─────────────────────────────────────

int Engine::DisplayNumberFromDevice(const wchar_t* deviceName) {
    if (!deviceName) return 0;
    const wchar_t* d = deviceName;
    while (*d && (*d < L'0' || *d > L'9')) d++;
    return *d ? _wtoi(d) : 0;
}

bool Engine::ChildOwnsDisplay(const wchar_t* deviceName) const {
    const int n = DisplayNumberFromDevice(deviceName);
    if (n < 1 || n > 32) return false;
    return (m_childReadyMask.load(std::memory_order_relaxed) & (1u << (n - 1))) != 0;
}

void Engine::SetChildDisplayReady(const wchar_t* deviceName, bool bReady) {
    const int n = DisplayNumberFromDevice(deviceName);
    if (n < 1 || n > 32) return;
    const uint32_t bit = 1u << (n - 1);
    if (bReady)
        m_childReadyMask.fetch_or(bit, std::memory_order_relaxed);
    else
        m_childReadyMask.fetch_and(~bit, std::memory_order_relaxed);
}

bool Engine::GetChildInfo(const wchar_t* deviceName, ChildInstance& out) const {
    std::lock_guard<std::mutex> lock(m_childrenMutex);
    for (const auto& ci : m_children)
        if (wcscmp(ci.szDeviceName, deviceName) == 0) { out = ci; return true; }
    return false;
}

void Engine::SetChildFps(DWORD pid, float fps) {
    std::lock_guard<std::mutex> lock(m_childrenMutex);
    for (auto& ci : m_children)
        if (ci.pid == pid) { ci.fFps = fps; return; }
}

void Engine::SetChildCurrentPreset(DWORD pid, const wchar_t* presetPath) {
    std::lock_guard<std::mutex> lock(m_childrenMutex);
    for (auto& ci : m_children)
        if (ci.pid == pid) { wcsncpy_s(ci.szCurrentPreset, presetPath, _TRUNCATE); return; }
}

void Engine::BroadcastToChildren(const wchar_t* message) {
    if (m_bChildMode || !message) return;
    std::vector<ChildInstance> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_childrenMutex);
        snapshot = m_children;
    }
    for (auto& ci : snapshot)
        if (ci.bReady && !ci.bFailed)
            ChildSend(ci.szPipeName, message);
}

} // namespace mdrop
