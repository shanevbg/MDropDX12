// engine_displays.cpp — Display output management (monitor mirrors + Spout senders)
//
// Part of the MDropDX12 unified display output system.
// Manages enumeration, INI persistence, init/destroy, and per-frame send.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "json_utils.h"
#include "utility.h"
#include <algorithm>
#include <dxgi1_4.h>
#include <process.h>
#include <mutex>

namespace mdrop {

// Used by InitDisplayOutput and ApplyMirrorWindowStyles (defined later)
static void ComputeMirrorLayout(const DisplayOutputConfig& cfg,
                                int& outX, int& outY, int& outW, int& outH);

// Never allocate a mirror SC larger than 1920 on a side. 2160x3840 primary
// plus matching mirror SCs TDRs (idle-timer FS at 12:22).
static void CapMirrorSwapChainDim(int& w, int& h)
{
    const int maxDim = 1920;
    if (w > maxDim || h > maxDim) {
        float sc = (float)maxDim / (float)((w > h) ? w : h);
        w = max(1, (int)(w * sc + 0.5f));
        h = max(1, (int)(h * sc + 0.5f));
    }
    w = ((w + 15) / 16) * 16;
    h = ((h + 15) / 16) * 16;
}

// Copy-mode / same-aspect independent: primary BB size (capped).
// Opposite-aspect independent: monitor aspect, long side <= 1920.
static void MirrorSwapChainSize(const DisplayOutput& out, int primW, int primH,
                                int& outW, int& outH)
{
    int x = 0, y = 0, layW = 0, layH = 0;
    ComputeMirrorLayout(out.config, x, y, layW, layH);
    const bool primPortrait = primH > primW;
    const bool panelPortrait = layH > layW;
    if (out.config.bIndependentRender && layW > 0 && layH > 0 &&
        (panelPortrait != primPortrait)) {
        outW = layW;
        outH = layH;
    } else {
        outW = primW;
        outH = primH;
    }
    CapMirrorSwapChainDim(outW, outH);
}

// ─── Mirror Window Proc ──────────────────────────────────────────────────────
// GWLP_USERDATA holds the primary render HWND (set at CreateWindow).
// Mirrors use WS_EX_NOACTIVATE so they never become the key window; without
// explicit focus hand-off, clicks on a mirror leave local hotkeys dead.

static void RequestPrimaryFocus(HWND hMirror)
{
    HWND primary = (HWND)GetWindowLongPtrW(hMirror, GWLP_USERDATA);
    if (!primary || !IsWindow(primary))
        return;
    // Cross-thread: primary is owned by the UI thread — never SetFocus here.
    PostMessageW(primary, WM_MW_FOCUS_PRIMARY, 0, 0);
}

static LRESULT CALLBACK MirrorWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CLOSE:
        return 0; // Prevent user from closing the mirror window
    case WM_ERASEBKGND:
        return 1; // DX12 handles rendering; skip GDI erase

    case WM_MOUSEACTIVATE:
        // Do not activate the mirror; ask primary to take keyboard focus.
        RequestPrimaryFocus(hWnd);
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        RequestPrimaryFocus(hWnd);
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    {
        // If keys ever land here, forward to primary so hotkeys still work.
        HWND primary = (HWND)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (primary && IsWindow(primary))
            PostMessageW(primary, uMsg, wParam, lParam);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Helper: find a monitor by device name and return its current rect
struct FindMonitorCtx {
    const wchar_t* szDeviceName;
    RECT rcResult;
    bool bFound;
};

static BOOL CALLBACK FindMonitorCB(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindMonitorCtx*>(lp);
    MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (wcscmp(mi.szDevice, ctx->szDeviceName) == 0) {
            ctx->rcResult = mi.rcMonitor;
            ctx->bFound = true;
            return FALSE; // stop enumeration
        }
    }
    return TRUE;
}

// ─── Monitor Enumeration ──────────────────────────────────────────────────────

struct EnumMonitorCtx {
    Engine* engine;
    HMONITOR hRenderMonitor; // the monitor hosting the render window (for bSkippedSameMonitor)
};

static BOOL CALLBACK EnumMonitorCB(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<EnumMonitorCtx*>(lp);

    MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
    if (!GetMonitorInfoW(hMon, &mi))
        return TRUE;

    DisplayOutput out;
    out.config.type = DisplayOutputType::Monitor;
    out.config.bEnabled = false;
    out.config.bFullscreen = true;
    out.config.rcMonitor = mi.rcMonitor;
    wcsncpy_s(out.config.szDeviceName, mi.szDevice, _TRUNCATE);

    // Mark the render window's monitor as skipped (dynamic check in SendToDisplayOutputs
    // will update this at runtime when the render window moves between monitors)
    if (hMon == ctx->hRenderMonitor)
        out.bSkippedSameMonitor = true;

    // Get friendly display name from DISPLAY_DEVICEW
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0))
        wcsncpy_s(out.config.szName, dd.DeviceString, _TRUNCATE);
    else
        wcsncpy_s(out.config.szName, mi.szDevice, _TRUNCATE);

    // Append device name for disambiguation
    wchar_t label[128];
    swprintf(label, 128, L"%s (%s)", out.config.szName, mi.szDevice);
    wcsncpy_s(out.config.szName, label, _TRUNCATE);

    ctx->engine->m_displayOutputs.push_back(std::move(out));
    return TRUE;
}

void Engine::EnumerateDisplayOutputs()
{
    // Save existing monitor configs so we can preserve enabled state after re-enumeration
    // Mirrors are session-lived: carry the live MonitorMirrorState across
    // re-enumeration instead of letting the erase below destroy it. A plain
    // unique_ptr reset never runs DestroyMonitorMirror, so every enumeration
    // used to leak that mirror's reserved RTV block (16 slots total → mirrors
    // stopped coming back after ~4 idle "Mirror all" cycles) and its HWND.
    struct SavedMonitorConfig {
        wchar_t szDeviceName[32];
        RECT rcMonitor;  // Match by display rect (more reliable than device names)
        bool bEnabled;
        bool bFullscreen;
        int nOpacity;
        bool bClickThrough;
        bool bIndependentRender;
        std::unique_ptr<MonitorMirrorState> monitorState;
    };
    std::vector<SavedMonitorConfig> saved;
    for (auto& o : m_displayOutputs) {
        if (o.config.type == DisplayOutputType::Monitor) {
            SavedMonitorConfig s = {};
            wcsncpy_s(s.szDeviceName, o.config.szDeviceName, _TRUNCATE);
            s.rcMonitor = o.config.rcMonitor;
            s.bEnabled = o.config.bEnabled;
            s.bFullscreen = o.config.bFullscreen;
            s.nOpacity = o.config.nOpacity;
            s.bClickThrough = o.config.bClickThrough;
            s.bIndependentRender = o.config.bIndependentRender;
            s.monitorState = std::move(o.monitorState);
            saved.push_back(std::move(s));
        }
    }

    // Remove existing monitor entries (keep Spout outputs)
    m_displayOutputs.erase(
        std::remove_if(m_displayOutputs.begin(), m_displayOutputs.end(),
            [](const DisplayOutput& o) { return o.config.type == DisplayOutputType::Monitor; }),
        m_displayOutputs.end());

    // Determine which monitor hosts the render window
    HMONITOR hRenderMon = nullptr;
    if (m_lpDX && m_lpDX->GetHwnd())
        hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);

    EnumMonitorCtx ctx = { this, hRenderMon };
    EnumDisplayMonitors(NULL, NULL, EnumMonitorCB, reinterpret_cast<LPARAM>(&ctx));

    // Restore saved config for monitors that still exist (match by display rect first,
    // fall back to device name — Windows can reassign device names across enumerations)
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        for (auto& s : saved) {
            bool rectMatch = (out.config.rcMonitor.left == s.rcMonitor.left &&
                              out.config.rcMonitor.top == s.rcMonitor.top &&
                              out.config.rcMonitor.right == s.rcMonitor.right &&
                              out.config.rcMonitor.bottom == s.rcMonitor.bottom);
            if (rectMatch || wcscmp(out.config.szDeviceName, s.szDeviceName) == 0) {
                out.config.bEnabled = s.bEnabled;
                out.config.bFullscreen = s.bFullscreen;
                out.config.nOpacity = s.nOpacity;
                out.config.bClickThrough = s.bClickThrough;
                out.config.bIndependentRender = s.bIndependentRender;
                // Same physical rect → reuse the live window + swap chain.
                // Rect changed (resolution / rearrange) → let it be rebuilt at
                // the new size; the old one is destroyed on the render thread.
                if (s.monitorState && rectMatch)
                    out.monitorState = std::move(s.monitorState);
                break;
            }
        }
    }

    // Anything not carried over (monitor unplugged, rect changed) still owns a
    // swap chain, an RTV block and an HWND. WaitForGpu/DestroyWindow are
    // render-thread-only, so hand them off instead of destroying them here.
    {
        std::lock_guard<std::mutex> lk(m_orphanMirrorMutex);
        for (auto& s : saved) {
            if (s.monitorState)
                m_orphanMirrors.push_back(std::move(s.monitorState));
        }
    }
}

// ─── Mirror Activation Failsafe ──────────────────────────────────────────────

Engine::MirrorActivateResult Engine::TryActivateMirrors(HWND hRenderWnd)
{
    // Which monitor hosts the render window (mirrors of that one are skipped)
    wchar_t renderDevice[32] = {};
    if (hRenderWnd) {
        HMONITOR hRenderMon = MonitorFromWindow(hRenderWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = { sizeof(mi) };
        if (hRenderMon && GetMonitorInfoW(hRenderMon, &mi))
            wcscpy_s(renderDevice, mi.szDevice);
    }

    int totalOther = 0;   // monitors that are not the render surface
    int enabledOther = 0;
    for (auto& o : m_displayOutputs) {
        if (o.config.type != DisplayOutputType::Monitor)
            continue;
        const bool isPrimary = renderDevice[0] &&
            wcscmp(o.config.szDeviceName, renderDevice) == 0;
        if (isPrimary)
            continue;
        totalOther++;
        if (o.config.bEnabled)
            enabledOther++;
    }

    // Case 1: User already enabled one or more non-primary monitors — keep that selection
    if (enabledOther > 0)
        return MirrorActivated;

    // Case 2: No other monitors at all — fullscreen only
    if (totalOther == 0)
        return MirrorFullscreenOnly;

    // Case 3: Other monitors exist but none enabled — auto-enable only non-primary
    // (do NOT force-enable the render monitor; that never gets a mirror window)
    for (auto& o : m_displayOutputs) {
        if (o.config.type != DisplayOutputType::Monitor)
            continue;
        const bool isPrimary = renderDevice[0] &&
            wcscmp(o.config.szDeviceName, renderDevice) == 0;
        o.config.bEnabled = !isPrimary;
    }
    SaveDisplayOutputSettings();
    return MirrorActivated;
}

// ─── INI Persistence ──────────────────────────────────────────────────────────

void Engine::LoadDisplayOutputSettings()
{
    wchar_t* pIni = GetConfigIniFile();

    int count = GetPrivateProfileIntW(L"DisplayOutputs", L"Count", -1, pIni);
    int legacyOpacity = GetPrivateProfileIntW(L"DisplayOutputs", L"MirrorOpacity", 100, pIni);
    if (legacyOpacity < 1) legacyOpacity = 1;
    if (legacyOpacity > 100) legacyOpacity = 100;
    m_bMirrorModeForAltS = GetPrivateProfileBoolW(L"DisplayOutputs", L"MirrorModeForAltS", false, pIni);
    m_bMirrorPromptDisabled = GetPrivateProfileBoolW(L"DisplayOutputs", L"MirrorPromptDisabled", false, pIni);
    m_bMirrorIndependentDefault = GetPrivateProfileBoolW(L"DisplayOutputs", L"MirrorIndependentDefault", false, pIni);
    {
        int fps = GetPrivateProfileIntW(L"DisplayOutputs", L"MirrorMaxFps", 0, pIni);
        if (fps < 0) fps = 0;
        if (fps > 0 && fps < 5) fps = 5;
        if (fps > 240) fps = 240;
        m_nMirrorMaxFps.store(fps);
    }

    if (count < 0) {
        // Legacy migration: no [DisplayOutputs] section yet.
        // Create a default Spout output from the old settings.
        DisplayOutput spout;
        spout.config.type = DisplayOutputType::Spout;
        spout.config.bEnabled = bSpoutOut;
        spout.config.bFixedSize = bSpoutFixedSize;
        spout.config.nWidth = nSpoutFixedWidth;
        spout.config.nHeight = nSpoutFixedHeight;
        wcscpy_s(spout.config.szName, L"MDropDX12");
        m_displayOutputs.insert(m_displayOutputs.begin(), std::move(spout));
        return;
    }

    for (int i = 0; i < count; i++) {
        wchar_t section[64];
        swprintf(section, 64, L"DisplayOutput_%d", i);

        wchar_t typeBuf[32] = {};
        GetPrivateProfileStringW(section, L"Type", L"Spout", typeBuf, 32, pIni);

        DisplayOutput out;
        if (wcscmp(typeBuf, L"Monitor") == 0)
            out.config.type = DisplayOutputType::Monitor;
        else
            out.config.type = DisplayOutputType::Spout;

        out.config.bEnabled = GetPrivateProfileBoolW(section, L"Enabled", false, pIni);

        wchar_t nameBuf[128] = {};
        GetPrivateProfileStringW(section, L"Name", L"MDropDX12", nameBuf, 128, pIni);
        wcsncpy_s(out.config.szName, nameBuf, _TRUNCATE);

        if (out.config.type == DisplayOutputType::Monitor) {
            wchar_t devBuf[32] = {};
            GetPrivateProfileStringW(section, L"DeviceName", L"", devBuf, 32, pIni);
            wcsncpy_s(out.config.szDeviceName, devBuf, _TRUNCATE);
            out.config.bFullscreen = GetPrivateProfileBoolW(section, L"Fullscreen", true, pIni);
            out.config.nOpacity = GetPrivateProfileIntW(section, L"Opacity", 100, pIni);
            if (out.config.nOpacity < 1) out.config.nOpacity = 1;
            if (out.config.nOpacity > 100) out.config.nOpacity = 100;
            out.config.bClickThrough = GetPrivateProfileBoolW(section, L"ClickThrough", false, pIni);
            out.config.bIndependentRender = GetPrivateProfileBoolW(section, L"IndependentRender",
                m_bMirrorIndependentDefault, pIni);
        }
        else {
            out.config.bFixedSize = GetPrivateProfileBoolW(section, L"FixedSize", false, pIni);
            out.config.nWidth = GetPrivateProfileIntW(section, L"Width", 1920, pIni);
            out.config.nHeight = GetPrivateProfileIntW(section, L"Height", 1080, pIni);
        }

        // For monitors, try to match to an already-enumerated monitor by DeviceName
        if (out.config.type == DisplayOutputType::Monitor) {
            bool matched = false;
            for (auto& existing : m_displayOutputs) {
                if (existing.config.type == DisplayOutputType::Monitor &&
                    wcscmp(existing.config.szDeviceName, out.config.szDeviceName) == 0) {
                    // Update the enumerated entry with saved settings
                    existing.config.bEnabled = out.config.bEnabled;
                    existing.config.bFullscreen = out.config.bFullscreen;
                    existing.config.nOpacity = out.config.nOpacity;
                    existing.config.bClickThrough = out.config.bClickThrough;
                    existing.config.bIndependentRender = out.config.bIndependentRender;
                    matched = true;
                    break;
                }
            }
            // If monitor not currently connected, skip it
            if (!matched) continue;
        }
        else {
            // Spout outputs: insert at the beginning (before monitors)
            m_displayOutputs.insert(m_displayOutputs.begin(), std::move(out));
        }
    }

    // Sync legacy variables from first Spout output (backward compat)
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Spout) {
            bSpoutOut = out.config.bEnabled;
            bSpoutFixedSize = out.config.bFixedSize;
            nSpoutFixedWidth = out.config.nWidth;
            nSpoutFixedHeight = out.config.nHeight;
            break;
        }
    }
}

void Engine::SaveDisplayOutputSettings()
{
    wchar_t* pIni = GetConfigIniFile();

    int count = (int)m_displayOutputs.size();
    wchar_t buf[64];
    swprintf(buf, 64, L"%d", count);
    WritePrivateProfileStringW(L"DisplayOutputs", L"Count", buf, pIni);
    swprintf(buf, 64, L"%d", m_bMirrorModeForAltS ? 1 : 0);
    WritePrivateProfileStringW(L"DisplayOutputs", L"MirrorModeForAltS", buf, pIni);
    swprintf(buf, 64, L"%d", m_bMirrorPromptDisabled ? 1 : 0);
    WritePrivateProfileStringW(L"DisplayOutputs", L"MirrorPromptDisabled", buf, pIni);
    swprintf(buf, 64, L"%d", m_bMirrorIndependentDefault ? 1 : 0);
    WritePrivateProfileStringW(L"DisplayOutputs", L"MirrorIndependentDefault", buf, pIni);
    swprintf(buf, 64, L"%d", m_nMirrorMaxFps.load());
    WritePrivateProfileStringW(L"DisplayOutputs", L"MirrorMaxFps", buf, pIni);

    for (int i = 0; i < count; i++) {
        auto& cfg = m_displayOutputs[i].config;
        wchar_t section[64];
        swprintf(section, 64, L"DisplayOutput_%d", i);

        WritePrivateProfileStringW(section, L"Type",
            cfg.type == DisplayOutputType::Monitor ? L"Monitor" : L"Spout", pIni);
        swprintf(buf, 64, L"%d", cfg.bEnabled ? 1 : 0);
        WritePrivateProfileStringW(section, L"Enabled", buf, pIni);
        WritePrivateProfileStringW(section, L"Name", cfg.szName, pIni);

        if (cfg.type == DisplayOutputType::Monitor) {
            WritePrivateProfileStringW(section, L"DeviceName", cfg.szDeviceName, pIni);
            swprintf(buf, 64, L"%d", cfg.bFullscreen ? 1 : 0);
            WritePrivateProfileStringW(section, L"Fullscreen", buf, pIni);
            swprintf(buf, 64, L"%d", cfg.nOpacity);
            WritePrivateProfileStringW(section, L"Opacity", buf, pIni);
            swprintf(buf, 64, L"%d", cfg.bClickThrough ? 1 : 0);
            WritePrivateProfileStringW(section, L"ClickThrough", buf, pIni);
            swprintf(buf, 64, L"%d", cfg.bIndependentRender ? 1 : 0);
            WritePrivateProfileStringW(section, L"IndependentRender", buf, pIni);
        }
        else {
            swprintf(buf, 64, L"%d", cfg.bFixedSize ? 1 : 0);
            WritePrivateProfileStringW(section, L"FixedSize", buf, pIni);
            swprintf(buf, 64, L"%d", cfg.nWidth);
            WritePrivateProfileStringW(section, L"Width", buf, pIni);
            swprintf(buf, 64, L"%d", cfg.nHeight);
            WritePrivateProfileStringW(section, L"Height", buf, pIni);
        }
    }

    // Also sync legacy INI keys from first Spout output
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Spout) {
            bSpoutOut = out.config.bEnabled;
            bSpoutFixedSize = out.config.bFixedSize;
            nSpoutFixedWidth = out.config.nWidth;
            nSpoutFixedHeight = out.config.nHeight;
            break;
        }
    }
}

// ─── Display Output Init / Destroy ────────────────────────────────────────────

void Engine::InitDisplayOutput(DisplayOutput& out)
{
    if (out.config.type == DisplayOutputType::Spout) {
        if (out.spoutState && out.spoutState->bReady)
            return; // already live — never thrash Spout wraps mid-session
        out.spoutState = std::make_unique<SpoutOutputState>();
        auto& ss = *out.spoutState;

        if (!m_lpDX || !m_lpDX->m_device.Get())
            return;

        // Convert wide name to ANSI for Spout API
        char senderNameA[256] = {};
        WideCharToMultiByte(CP_ACP, 0, out.config.szName, -1, senderNameA, 256, NULL, NULL);
        ss.sender.SetSenderName(senderNameA);

        if (!ss.sender.OpenDirectX12(
                m_lpDX->m_device.Get(),
                (IUnknown**)m_lpDX->m_commandQueue.GetAddressOf())) {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: OpenDirectX12 failed for '%s'\n", senderNameA);
            DebugLogA(logBuf, LOG_ERROR);
            return;
        }

        for (int n = 0; n < DXC_FRAME_COUNT; n++) {
            if (!ss.sender.WrapDX12Resource(
                    m_lpDX->m_renderTargets[n].Get(),
                    &ss.wrappedBackBuffers[n],
                    D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                char logBuf[512];
                sprintf(logBuf, "InitDisplayOutput: WrapDX12Resource failed [%d]\n", n);
                DebugLogA(logBuf, LOG_ERROR);
                // Cleanup partial wraps
                for (int j = 0; j < n; j++) {
                    if (ss.wrappedBackBuffers[j]) {
                        ss.wrappedBackBuffers[j]->Release();
                        ss.wrappedBackBuffers[j] = nullptr;
                    }
                }
                ss.sender.CloseDirectX12();
                return;
            }
        }
        ss.bReady = true;
        { char logBuf[512]; sprintf(logBuf, "InitDisplayOutput: Spout sender '%s' ready\n", senderNameA); DebugLogA(logBuf); }
    }
    else if (out.config.type == DisplayOutputType::Monitor) {
        if (!m_lpDX || !m_lpDX->m_device.Get() || !m_lpDX->m_swapChain.Get())
            return;

        // Idempotent: if a session-lived mirror already exists, never recreate.
        // Destroying + CreateSwapChain while the GPU still references back buffers
        // is what TDRs the device after a couple of enable/disable toggles.
        if (out.monitorState && out.monitorState->bReady && out.monitorState->swapChain) {
            out.monitorState->bSoftDisabled = false;
            out.monitorState->nFramesUntilHardDestroy = 0;
            if (out.monitorState->hWnd && !IsWindowVisible(out.monitorState->hWnd))
                ShowWindow(out.monitorState->hWnd, SW_SHOWNOACTIVATE);
            return;
        }
        // Stale half-init (failed earlier) — drop without WaitForGpu (no live SC)
        if (out.monitorState)
            out.monitorState.reset();

        {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: Starting init for %ls (%ls)\n",
                out.config.szName, out.config.szDeviceName);
            DebugLogA(logBuf, LOG_WARN);
        }

        // Safety: don't mirror the monitor hosting the render window.
        // In watermark mode, use the stored target device name for deterministic detection.
        {
            wchar_t renderDevice[32] = {};
            if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
                wcscpy_s(renderDevice, m_szWatermarkRenderDevice);
            } else if (m_lpDX->GetHwnd()) {
                HMONITOR hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
                if (hRenderMon) {
                    MONITORINFOEXW mi = { sizeof(mi) };
                    if (GetMonitorInfoW(hRenderMon, &mi))
                        wcscpy_s(renderDevice, mi.szDevice);
                }
            }
            if (renderDevice[0] && wcscmp(renderDevice, out.config.szDeviceName) == 0) {
                char logBuf[256];
                sprintf(logBuf, "InitDisplayOutput: Skipping %ls — render window's monitor\n",
                    out.config.szDeviceName);
                DebugLogA(logBuf, LOG_WARN);
                out.bSkippedSameMonitor = true;
                return;
            }
        }

        // Do NOT WaitForGpu here — it freezes the whole machine when called often
        // (enable/disable cycles). Soft-destroy already waits multiple frames before
        // releasing swap chains. PeekMessage/Dispatch is also avoided: re-entering
        // the message pump from the render thread can deadlock DXGI.

        out.monitorState = std::make_unique<MonitorMirrorState>();
        auto& ms = *out.monitorState;
        ms.bSoftDisabled = false;
        ms.nFramesUntilHardDestroy = 0;

        // Register mirror window class (once)
        if (!m_bMirrorClassRegistered) {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = MirrorWndProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = L"MDropDX12_Mirror";
            if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
                m_bMirrorClassRegistered = true;
        }

        // Re-query the monitor rect fresh by device name (cached rect may be stale)
        FindMonitorCtx fmc = { out.config.szDeviceName, {}, false };
        EnumDisplayMonitors(NULL, NULL, FindMonitorCB, reinterpret_cast<LPARAM>(&fmc));
        if (fmc.bFound) {
            out.config.rcMonitor = fmc.rcResult;
        } else {
            char logBuf[256];
            sprintf(logBuf, "InitDisplayOutput: WARNING — monitor %ls not found by EnumDisplayMonitors!\n",
                out.config.szDeviceName);
            DebugLogA(logBuf, LOG_WARN);
        }

        RECT rc = out.config.rcMonitor;
        int monW = rc.right - rc.left;
        int monH = rc.bottom - rc.top;
        {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: %ls rect = (%d,%d)-(%d,%d) size %dx%d\n",
                out.config.szDeviceName,
                (int)rc.left, (int)rc.top, (int)rc.right, (int)rc.bottom, monW, monH);
            DebugLogA(logBuf, LOG_WARN);
        }

        if (monW <= 0 || monH <= 0) {
            DebugLogA("InitDisplayOutput: Invalid monitor rect, skipping\n", LOG_WARN);
            out.monitorState.reset();
            return;
        }

        // Create borderless popup covering the target monitor.
        // IMPORTANT: Do NOT use WS_EX_LAYERED when opacity is 100%. Flip-model
        // swap chains + layered windows let DWM composite other monitors' content
        // through the mirror (landscape "ghost" strip on portrait, often flickering).
        // Layered is only for partial opacity or click-through.
        const bool needLayered = (out.config.nOpacity < 100) || out.config.bClickThrough;
        DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        if (needLayered)
            exStyle |= WS_EX_LAYERED;
        if (out.config.bClickThrough)
            exStyle |= WS_EX_TRANSPARENT;
        // Fullscreen checkbox: full monitor vs centered windowed layout
        int layX = rc.left, layY = rc.top, layW = monW, layH = monH;
        ComputeMirrorLayout(out.config, layX, layY, layW, layH);

        ms.hWnd = CreateWindowExW(
            exStyle,
            L"MDropDX12_Mirror",
            L"MDropDX12 Mirror",
            WS_POPUP,
            layX, layY, layW, layH,
            nullptr, nullptr, GetModuleHandle(NULL), nullptr);
        if (!ms.hWnd) {
            DebugLogA("InitDisplayOutput: CreateWindowExW failed for mirror\n", LOG_ERROR);
            out.monitorState.reset();
            return;
        }
        // Primary HWND for focus hand-off on click (local hotkeys need primary focus).
        HWND hPrimary = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
        SetWindowLongPtrW(ms.hWnd, GWLP_USERDATA, (LONG_PTR)hPrimary);
        if (needLayered) {
            BYTE alpha = (BYTE)(out.config.nOpacity * 255 / 100);
            if (alpha < 3) alpha = 3;
            SetLayeredWindowAttributes(ms.hWnd, 0, alpha, LWA_ALPHA);
        }
        // Mirrors must stay above other windows on their monitor; otherwise the
        // landscape primary (or desktop) can show through as a ghost.
        SetWindowPos(ms.hWnd, HWND_TOPMOST,
                     layX, layY, layW, layH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!m_bAlwaysOnTop)
            SetWindowPos(ms.hWnd, HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);

        // Get DXGI factory from existing swap chain
        ComPtr<IDXGIFactory4> factory;
        HRESULT hr = m_lpDX->m_swapChain->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            DebugLogA("InitDisplayOutput: GetParent(IDXGIFactory4) failed\n", LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }

        // Copy mode: SC = primary BB (CopyResource + DXGI stretch).
        // Independent opposite-aspect: SC = monitor aspect, long side capped at
        // 1920 (full 2160x3840 + a second classic pass TDRs). Same-aspect
        // independent still uses primary size and blits.
        const int primW = (m_lpDX->m_client_width > 0) ? m_lpDX->m_client_width : layW;
        const int primH = (m_lpDX->m_client_height > 0) ? m_lpDX->m_client_height : layH;
        MirrorSwapChainSize(out, primW, primH, ms.width, ms.height);
        ms.bIndependentSized = (ms.width != primW || ms.height != primH);

        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Width = (UINT)ms.width;
        scDesc.Height = (UINT)ms.height;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = DXC_FRAME_COUNT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        scDesc.Flags = 0;
        scDesc.Scaling = DXGI_SCALING_STRETCH;

        ComPtr<IDXGISwapChain1> sc1;
        hr = factory->CreateSwapChainForHwnd(
            m_lpDX->m_commandQueue.Get(), ms.hWnd, &scDesc, nullptr, nullptr, &sc1);
        ms.bufferCount = scDesc.BufferCount;
        if (FAILED(hr)) {
            char logBuf[256]; sprintf(logBuf, "InitDisplayOutput: CreateSwapChainForHwnd failed (0x%08X)\n", (unsigned)hr);
            DebugLogA(logBuf, LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }
        factory->MakeWindowAssociation(ms.hWnd, DXGI_MWA_NO_ALT_ENTER);
        hr = sc1.As(&ms.swapChain);
        if (FAILED(hr)) {
            DebugLogA("InitDisplayOutput: QueryInterface IDXGISwapChain4 failed\n", LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }

        // Get back buffers (+ permanent-reserve RTVs that survive resize rewinds)
        ms.bHasRtv = false;
        ms.rtvSlotBase = m_lpDX->AllocateMirrorRtvBlock(ms.bufferCount);
        if (ms.rtvSlotBase == UINT_MAX) {
            DebugLogA("InitDisplayOutput: no reserved RTV slots for mirror\n", LOG_ERROR);
            ms.swapChain.Reset();
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }
        for (UINT i = 0; i < ms.bufferCount && i < MIRROR_BUFFER_COUNT; i++) {
            hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
            if (FAILED(hr)) {
                char logBuf[256]; sprintf(logBuf, "InitDisplayOutput: GetBuffer(%d) failed\n", i);
                DebugLogA(logBuf, LOG_ERROR);
                m_lpDX->FreeMirrorRtvBlock(ms.rtvSlotBase, ms.bufferCount);
                ms.rtvSlotBase = UINT_MAX;
                ms.swapChain.Reset();
                DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
                out.monitorState.reset();
                return;
            }
            ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
            m_lpDX->m_device->CreateRenderTargetView(ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
        }
        ms.bHasRtv = true;
        ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
        ms.bNeedsFullChainClear = true;
        ms.paintedBufferMask = 0;
        for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
            ms.bbState[bi] = D3D12_RESOURCE_STATE_COMMON;

        ms.bReady = true;
        {
            char logBuf[256];
            sprintf(logBuf, "InitDisplayOutput: Mirror %ls READY (hwnd=%p, swapchain=%p, %dx%d copy-size)\n",
                out.config.szDeviceName, (void*)ms.hWnd, (void*)ms.swapChain.Get(),
                ms.width, ms.height);
            DebugLogA(logBuf, LOG_WARN);
        }
    }
}

void Engine::DestroyDisplayOutput(DisplayOutput& out)
{
    if (out.spoutState) {
        auto& ss = *out.spoutState;
        for (int n = 0; n < DXC_FRAME_COUNT; n++) {
            if (ss.wrappedBackBuffers[n]) {
                ss.wrappedBackBuffers[n]->Release();
                ss.wrappedBackBuffers[n] = nullptr;
            }
        }
        if (ss.bReady) {
            ss.sender.CloseDirectX12();
            ss.bReady = false;
        }
        out.spoutState.reset();
    }
    if (out.monitorState) {
        DestroyMonitorMirror(*out.monitorState);
        out.monitorState.reset();
    }
}

// Single teardown path for a mirror's GPU + window resources. Callers must be
// on the render thread: WaitForGpu freezes the UI thread, and DestroyWindow
// only works from the thread that created the window.
void Engine::DestroyMonitorMirror(MonitorMirrorState& ms)
{
    ms.bReady = false;
    ms.bSoftDisabled = true;
    // Must idle the GPU before releasing swap-chain buffers. Without this,
    // in-flight mirror command lists TDR the device (seen as lockup after
    // 1–2 toggles when destroy ran mid-session). Only called on profile
    // load / refresh / app shutdown — never on enable/disable toggle.
    if (m_lpDX && ms.swapChain)
        m_lpDX->WaitForGpu();
    if (m_lpDX && ms.rtvSlotBase != UINT_MAX) {
        m_lpDX->FreeMirrorRtvBlock(ms.rtvSlotBase, ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT);
        ms.rtvSlotBase = UINT_MAX;
    }
    ms.bHasRtv = false;
    for (UINT i = 0; i < MIRROR_BUFFER_COUNT; i++)
        ms.backBuffers[i].Reset();
    ms.swapChain.Reset();
    if (ms.hWnd) {
        DestroyWindow(ms.hWnd);
        ms.hWnd = nullptr;
    }
}

// Render-thread drain for mirrors detached by EnumerateDisplayOutputs.
void Engine::DrainOrphanedMirrors()
{
    std::vector<std::unique_ptr<MonitorMirrorState>> orphans;
    {
        std::lock_guard<std::mutex> lk(m_orphanMirrorMutex);
        if (m_orphanMirrors.empty())
            return;
        orphans.swap(m_orphanMirrors);
    }
    for (auto& ms : orphans) {
        if (ms)
            DestroyMonitorMirror(*ms);
    }
    char logBuf[128];
    sprintf(logBuf, "DrainOrphanedMirrors: destroyed %d re-enumerated mirror(s)\n", (int)orphans.size());
    DebugLogA(logBuf, LOG_WARN);
}

void Engine::ReleaseDisplayOutputWraps()
{
    for (auto& out : m_displayOutputs) {
        if (out.spoutState) {
            auto& ss = *out.spoutState;
            for (int n = 0; n < DXC_FRAME_COUNT; n++) {
                if (ss.wrappedBackBuffers[n]) {
                    ss.wrappedBackBuffers[n]->Release();
                    ss.wrappedBackBuffers[n] = nullptr;
                }
            }
            ss.sender.CloseDirectX12();
            ss.bReady = false;
            out.spoutState.reset();
        }
    }
}

void Engine::DestroyAllDisplayOutputs()
{
    StopMirrorThread();
    DrainOrphanedMirrors();
    for (auto& out : m_displayOutputs)
        DestroyDisplayOutput(out);

    // Release mirror command objects
    m_mirrorCmdList.Reset();
    for (int i = 0; i < DXC_FRAME_COUNT; i++)
        m_mirrorCmdAllocators[i].Reset();
    if (m_lpDX && !LagIndepFenceIdle())
        m_lpDX->WaitForGpu();
    ReleaseLagIndepObjects();
}

bool Engine::LagIndepFenceIdle() const
{
    if (!m_lagIndepFence)
        return true;
    return m_lagIndepFence->GetCompletedValue() >= m_lagIndepSubmitted;
}

bool Engine::EnsureLagIndepObjects()
{
    if (!m_lpDX || !m_lpDX->m_device)
        return false;
    if (!m_lagIndepFence) {
        HRESULT hr = m_lpDX->m_device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_lagIndepFence));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateFence failed\n", LOG_ERROR);
            return false;
        }
        m_lagIndepSubmitted = 0;
        m_lagIndepSignal = 0;
    }
    if (!m_lagIndepAlloc) {
        HRESULT hr = m_lpDX->m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_lagIndepAlloc));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateCommandAllocator failed\n", LOG_ERROR);
            return false;
        }
    }
    if (!m_lagIndepList) {
        HRESULT hr = m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_lagIndepAlloc.Get(), nullptr, IID_PPV_ARGS(&m_lagIndepList));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateCommandList failed\n", LOG_ERROR);
            return false;
        }
        m_lagIndepList->Close();
    }
    return true;
}

void Engine::ReleaseLagIndepObjects()
{
    m_lagIndepList.Reset();
    m_lagIndepAlloc.Reset();
    m_lagIndepFence.Reset();
    m_lagIndepSubmitted = 0;
    m_lagIndepSignal = 0;
    m_lagIndepAuxFrame = UINT_MAX;
}

void Engine::LockMirrorEngine() {
    // Give a waiting opposite-orient worker the lock first. Without this the
    // render thread wins every re-acquire and the worker starves (frozen mirrors).
    for (int i = 0; i < 8 && m_bMirrorWorkerWantsLock.load(std::memory_order_acquire); i++)
        Sleep(1);
    m_mirrorEngineMutex.lock();
}
void Engine::UnlockMirrorEngine() { m_mirrorEngineMutex.unlock(); }

static unsigned __stdcall MirrorThreadThunk(void* self)
{
    reinterpret_cast<Engine*>(self)->MirrorThreadMain();
    return 0;
}

void Engine::StartMirrorThread()
{
    if (m_hMirrorThread || !m_lpDX || !m_lpDX->m_device)
        return;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(m_lpDX->m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_mirrorQueue)))) {
        DebugLogA("StartMirrorThread: CreateCommandQueue failed\n", LOG_ERROR);
        return;
    }
    if (FAILED(m_lpDX->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_snapReadyFence)))) {
        m_mirrorQueue.Reset();
        return;
    }
    if (FAILED(m_lpDX->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_mirrorWorkFence)))) {
        m_snapReadyFence.Reset();
        m_mirrorQueue.Reset();
        return;
    }
    if (FAILED(m_lpDX->m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mirrorWorkAlloc)))) {
        StopMirrorThread();
        return;
    }
    if (FAILED(m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_mirrorWorkAlloc.Get(), nullptr, IID_PPV_ARGS(&m_mirrorWorkList)))) {
        StopMirrorThread();
        return;
    }
    m_mirrorWorkList->Close();

    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upDesc = {};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = 256 * 1024;
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.SampleDesc.Count = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(m_lpDX->m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &upDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_mirrorWorkUpload)))) {
        D3D12_RANGE range = {};
        m_mirrorWorkUpload->Map(0, &range, reinterpret_cast<void**>(&m_mirrorWorkUploadPtr));
    }

    m_hMirrorWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_hMirrorWake) {
        StopMirrorThread();
        return;
    }
    // Optional (worker falls back to skipping the wake if creation failed).
    m_hMirrorFenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_orientPublishedIdx.store(-1);
    m_orientLastWrite = -1;
    // The worker must have its own upload ring before it runs a single frame.
    // Sharing the aux ring let it reset the render thread's offsets mid-frame
    // (black presets, then TDR) — that is why this thread stayed unstarted.
    if (!m_lpDX->CreateMirrorUploadBuffer()) {
        DebugLogA("StartMirrorThread: mirror upload ring alloc failed\n", LOG_ERROR);
        StopMirrorThread();
        return;
    }

    m_bMirrorThreadQuit.store(false);
    unsigned tid = 0;
    m_hMirrorThread = (HANDLE)_beginthreadex(nullptr, 0, MirrorThreadThunk, this, 0, &tid);
    m_nMirrorThreadId = tid;
    // Publish before the worker can reach any upload helper.
    m_lpDX->SetMirrorUploadThreadId((DWORD)tid);
    if (!m_hMirrorThread) {
        DebugLogA("StartMirrorThread: _beginthreadex failed\n", LOG_ERROR);
        StopMirrorThread();
        return;
    }
    DebugLogA("StartMirrorThread: mirror worker running\n", LOG_INFO);
}

void Engine::StopMirrorThread()
{
    m_bMirrorThreadQuit.store(true);
    if (m_hMirrorWake)
        SetEvent(m_hMirrorWake);
    if (m_hMirrorThread) {
        WaitForSingleObject(m_hMirrorThread, 4000);
        CloseHandle(m_hMirrorThread);
        m_hMirrorThread = nullptr;
    }
    if (m_hMirrorWake) {
        CloseHandle(m_hMirrorWake);
        m_hMirrorWake = nullptr;
    }
    if (m_hMirrorFenceEvt) {
        CloseHandle(m_hMirrorFenceEvt);
        m_hMirrorFenceEvt = nullptr;
    }
    if (m_mirrorWorkUpload && m_mirrorWorkUploadPtr) {
        m_mirrorWorkUpload->Unmap(0, nullptr);
        m_mirrorWorkUploadPtr = nullptr;
    }
    if (m_lpDX) {
        m_lpDX->SetMirrorUploadThreadId(0);
        m_lpDX->ReleaseMirrorUploadBuffer();
    }
    m_nMirrorThreadId = 0;
    m_mirrorWorkUpload.Reset();
    m_mirrorWorkList.Reset();
    m_mirrorWorkAlloc.Reset();
    m_mirrorWorkFence.Reset();
    m_snapReadyFence.Reset();
    m_mirrorQueue.Reset();
    m_snapReadyValue = 0;
    m_mirrorWorkSubmitted = 0;
    m_orientPublishedIdx.store(-1);
    // Sim context: states + per-context EEL storage die with the thread.
    MirrorSimFree(m_mirrorSim);
}

void Engine::SignalMirrorSnapAndWake()
{
    if (!m_lpDX || !m_lpDX->m_commandQueue || !m_snapReadyFence || !m_hMirrorWake)
        return;
    m_snapReadyValue++;
    m_lpDX->m_commandQueue->Signal(m_snapReadyFence.Get(), m_snapReadyValue);
    SetEvent(m_hMirrorWake);
}

void Engine::MirrorThreadMain()
{
    DebugLogA("MirrorThreadMain: enter\n", LOG_INFO);
    while (!m_bMirrorThreadQuit.load()) {
        // Poll at 2 ms: nothing ever wired SignalMirrorSnapAndWake, so this
        // timeout IS the worker's loop pacer — 50 ms capped mirrors at ~20 fps
        // (11 fps backgrounded, when a fence-busy miss cost another 50 ms).
        // The actual rate comes from the orient GPU fence + engine-lock
        // contention in MirrorThreadDrawAndPresent (no rate cap, by request).
        WaitForSingleObject(m_hMirrorWake, 2);
        if (m_bMirrorThreadQuit.load())
            break;
        if (!m_bMirrorsActive)
            continue;
        try {
            MirrorThreadDrawAndPresent();
        } catch (...) {
            DebugLogA("MirrorThreadMain: exception — skip frame\n", LOG_ERROR);
        }
    }
    DebugLogA("MirrorThreadMain: exit\n", LOG_INFO);
}

void Engine::MirrorThreadDrawAndPresent()
{
    // Opposite-orientation render only. Same-orient mirrors stretch/copy the
    // primary on the main thread.
    // Quit checks: StopMirrorThread can run on the render thread WHILE that
    // thread holds the engine mutex — its join times out if we are blocked on
    // that mutex, and it then frees the sim context and D3D objects. Every
    // resume point below must bail on quit before touching anything.
    if (m_bMirrorThreadQuit.load())
        return;
    if (!m_lpDX || !m_lpDX->m_device || !m_mirrorQueue || !m_mirrorWorkList)
        return;
    if (m_mirrorWorkFence && m_mirrorWorkSubmitted > 0 &&
        m_mirrorWorkFence->GetCompletedValue() < m_mirrorWorkSubmitted) {
        // In-flight orient GPU work: wait briefly instead of burning the whole
        // wake. A hard return cost a full primary frame per miss — backgrounded
        // (wakes at ~44/s, GPU deprioritized) mirrors sat at 11 fps.
        if (!m_hMirrorFenceEvt ||
            FAILED(m_mirrorWorkFence->SetEventOnCompletion(
                m_mirrorWorkSubmitted, m_hMirrorFenceEvt)) ||
            WaitForSingleObject(m_hMirrorFenceEvt, 20) != WAIT_OBJECT_0)
            return;
    }

    const int rw = m_orientNeedW.load();
    const int rh = m_orientNeedH.load();
    if (rw <= 0 || rh <= 0 || !m_orientPipe.ready)
        return;

    // Independent simulation: adopt the latest preset bundle and advance the
    // context OUTSIDE the engine lock — ctx-pure work (own states/VMs/mesh),
    // imports serialize on m_stateImportMutex only. Never nest those locks
    // (see engine.h at m_stateImportMutex).
    if (!m_bShadertoyMode) {
        const int gw = m_orientPipe.ready ? m_orientPipe.w : rw;
        const int gh = m_orientPipe.ready ? m_orientPipe.h : rh;
        MirrorSimEnsureGrid(m_mirrorSim, gw, gh,
                            m_orientPanelW.load(), m_orientPanelH.load());
        if (!MirrorSimAdoptPreset(m_mirrorSim))
            return; // no bundle yet (startup) — keep last presented image
        MirrorSimStepFrame(m_mirrorSim);
    }

    // User rate cap (Displays dropdown / SET_MIRROR_MAXFPS). 0 = parity: the
    // worker free-runs, paced by its own GPU fence above and by engine-lock
    // contention with the render thread. Deliberate (Shane, 2026-08-21): the
    // orient pipeline integrates its own feedback once per orient frame, so
    // its rate IS the mirror's simulation rate — capping at panel refresh
    // made mirror renders visibly chunkier than the primary. The cost is a
    // lock-held CPU record per orient frame stolen from the primary; the cap
    // is for trading that back.
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    const int maxFps = m_nMirrorMaxFps.load(std::memory_order_relaxed);
    if (maxFps > 0) {
        static const LONGLONG s_qpf = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
        }();
        // -1.5 ms so the 2 ms wake quantization lands ON the target period
        // instead of one poll past it (a 60 fps cap otherwise runs ~53).
        const double minMs = 1000.0 / (double)maxFps - 1.5;
        if (m_llLastOrientQpc != 0 &&
            (double)(qpcNow.QuadPart - m_llLastOrientQpc) * 1000.0 / (double)s_qpf < minMs)
            return;
    }

    // Announce intent, then spin-acquire. A plain blocking lock could park us
    // here across StopMirrorThread (which runs on the render thread WHILE it
    // holds this mutex): its join timed out and it freed everything under us
    // (0xC0000409 on mirror disable, 2026-08-21). try_lock + quit checks keep
    // the worker join-able; the render thread's LockMirrorEngine still yields
    // to the wants-lock flag, so acquisition stays prompt.
    m_bMirrorWorkerWantsLock.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> lk(m_mirrorEngineMutex, std::defer_lock);
    for (int spin = 0; !lk.try_lock(); spin++) {
        if (m_bMirrorThreadQuit.load()) {
            m_bMirrorWorkerWantsLock.store(false, std::memory_order_release);
            return;
        }
        Sleep(spin < 8 ? 0 : 1);
    }
    m_bMirrorWorkerWantsLock.store(false, std::memory_order_release);
    // Re-check after acquisition: a Stop may have freed members while we spun.
    if (m_bMirrorThreadQuit.load() || !m_mirrorWorkAlloc || !m_mirrorWorkList)
        return;
    m_llLastOrientQpc = qpcNow.QuadPart;

    // The fence gate above proved last iteration's orient frame complete on
    // the GPU — publish its disp[] index for the render thread's blit and
    // advance the triple rotation. All panel blits/presents stay on the
    // render thread (worker-side main-queue submits deadlocked D3D12Core's
    // queue mutex; mirror-queue writes to flip buffers TDR'd — 2026-08-21).
    if (m_orientLastWrite >= 0) {
        m_orientPublishedIdx.store(m_orientLastWrite, std::memory_order_release);
        m_orientPipe.dispWrite = (m_orientLastWrite + 1) % 3;
    }

    if (FAILED(m_mirrorWorkAlloc->Reset()))
        return;
    if (FAILED(m_mirrorWorkList->Reset(m_mirrorWorkAlloc.Get(), nullptr)))
        return;

    ID3D12GraphicsCommandList* cmd = m_mirrorWorkList.Get();
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

    m_bOrientOppositeAspect = true;
    m_lpDX->BeginAuxUpload();
    const bool ok = m_bShadertoyMode
        ? RenderMilk3OrientPipeline(cmd, rw, rh)
        : RenderClassicOrientPipeline(cmd, rw, rh);
    m_lpDX->EndAuxUpload();
    m_bOrientOppositeAspect = false;

    if (FAILED(cmd->Close()))
        return;
    if (!ok) {
        m_bOrientImageReady.store(false);
        return;
    }

    ID3D12CommandList* lists[] = { cmd };
    m_mirrorQueue->ExecuteCommandLists(1, lists);
    m_mirrorWorkSubmitted++;
    m_nOrientFrameAccum.fetch_add(1, std::memory_order_relaxed);
    m_mirrorQueue->Signal(m_mirrorWorkFence.Get(), m_mirrorWorkSubmitted);
    // Remember which disp[] face this frame wrote; it is published for the
    // render thread at the NEXT iteration, after the fence proves it done.
    m_orientLastWrite = m_orientPipe.dispWrite % 3;
    if (m_orientPipe.frames >= 2)
        m_bOrientImageReady.store(true);
}

void Engine::CopyPrimaryToMirrorSrc()
{
    m_bMirrorSrcCopiedThisFrame = false;
    if (!m_bMirrorsActive || !m_lpDX || !m_lpDX->m_commandList || !m_lpDX->m_device)
        return;

    bool anyMirror = false;
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.config.bEnabled)
            continue;
        if (out.bSkippedSameMonitor)
            continue;
        if (out.monitorState && out.monitorState->bReady && !out.monitorState->bSoftDisabled)
            anyMirror = true;
        else if (!out.monitorState)
            anyMirror = true; // will Init this frame
    }
    if (!anyMirror)
        return;

    auto* cmd = m_lpDX->m_commandList.Get();
    ID3D12Resource* bb = m_lpDX->m_renderTargets[m_lpDX->m_frameIndex].Get();
    if (!cmd || !bb)
        return;

    // CopyResource requires exact size match. Client size can disagree with the
    // flip BB for a frame after fullscreen (that mismatch TDRs the GPU).
    const D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
    const UINT bw = (UINT)bbDesc.Width;
    const UINT bh = bbDesc.Height;
    if (bw == 0 || bh == 0)
        return;

    static UINT s_mirrorSrcEpoch = UINT_MAX;
    const bool srcStale = !m_mirrorSrcTex.IsValid() ||
        m_mirrorSrcTex.width != bw ||
        m_mirrorSrcTex.height != bh ||
        s_mirrorSrcEpoch != m_lpDX->m_descriptorEpoch;
    if (srcStale) {
        m_mirrorSrcTex.Reset();
        m_mirrorSrcTex = m_lpDX->CreateRenderTargetTexture(
            bw, bh, DXGI_FORMAT_R8G8B8A8_UNORM);
        s_mirrorSrcEpoch = m_lpDX->m_descriptorEpoch;
        if (!m_mirrorSrcTex.IsValid())
            return;
    }
    if (!m_mirrorSrcTex.resource ||
        m_mirrorSrcTex.width != bw || m_mirrorSrcTex.height != bh)
        return;

    D3D12_RESOURCE_BARRIER bars[2] = {};
    bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[0].Transition.pResource = bb;
    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[1].Transition.pResource = m_mirrorSrcTex.resource.Get();
    bars[1].Transition.StateBefore = m_mirrorSrcTex.currentState;
    bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (bars[1].Transition.StateBefore == bars[1].Transition.StateAfter)
        cmd->ResourceBarrier(1, &bars[0]);
    else
        cmd->ResourceBarrier(2, bars);

    cmd->CopyResource(m_mirrorSrcTex.resource.Get(), bb);
    m_mirrorSrcTex.currentState = D3D12_RESOURCE_STATE_COPY_DEST;

    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(2, bars);
    m_mirrorSrcTex.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_bMirrorSrcCopiedThisFrame = true;
}

void Engine::ResizeMirrorSwapChain(MonitorMirrorState& ms, int newW, int newH)
{
    // Prefer soft-recreate (caller sets bSoftDisabled) over ResizeBuffers+WaitForGpu.
    // Kept for rare callers; never block the GPU for 5s here.
    if (!ms.swapChain || !m_lpDX || newW <= 0 || newH <= 0) {
        ms.bReady = false;
        return;
    }
    if (ms.width == newW && ms.height == newH && ms.bHasRtv)
        return;

    if (m_lpDX->m_device && m_lpDX->m_device->GetDeviceRemovedReason() != S_OK) {
        ms.bReady = false;
        return;
    }

    // GPU must be idle before releasing back buffers
    m_lpDX->WaitForGpu();
    if (m_lpDX->m_device && m_lpDX->m_device->GetDeviceRemovedReason() != S_OK) {
        ms.bReady = false;
        return;
    }

    for (UINT i = 0; i < MIRROR_BUFFER_COUNT; i++)
        ms.backBuffers[i].Reset();
    ms.bHasRtv = false;

    // Must match CreateSwapChain flags (copy-mode SCs are created with Flags=0).
    UINT scFlags = 0;
    UINT nBuf = ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT;
    HRESULT hr = ms.swapChain->ResizeBuffers(
        nBuf, (UINT)newW, (UINT)newH,
        DXGI_FORMAT_R8G8B8A8_UNORM, scFlags);
    if (FAILED(hr)) {
        char logBuf[256]; sprintf(logBuf, "ResizeMirrorSwapChain: ResizeBuffers failed (0x%08X)\n", (unsigned)hr);
        DebugLogA(logBuf, LOG_ERROR);
        ms.bReady = false;
        return;
    }

    for (UINT i = 0; i < nBuf && i < MIRROR_BUFFER_COUNT; i++) {
        hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
        if (FAILED(hr)) {
            ms.bReady = false;
            return;
        }
        if (ms.rtvSlotBase != UINT_MAX) {
            ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
            m_lpDX->m_device->CreateRenderTargetView(
                ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
        }
    }
    ms.bHasRtv = (ms.rtvSlotBase != UINT_MAX);
    ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
    ms.width = newW;
    ms.height = newH;
    ms.bEverPresented = false;
    ms.bNeedsFullChainClear = true;
    ms.paintedBufferMask = 0;
    for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
        ms.bbState[bi] = D3D12_RESOURCE_STATE_COMMON;
}

// ─── Per-Frame Send ───────────────────────────────────────────────────────────

void Engine::SendToDisplayOutputs()
{
    if (!m_lpDX) return;

    // Free any mirror handed over by a re-enumeration before doing anything
    // else — their RTV blocks are needed by the InitDisplayOutput below.
    DrainOrphanedMirrors();

    // Mirror throughput, sampled once a second. orient is the opposite-orient
    // worker (its own thread, own rate); present is mirror swap-chain flips.
    // A large gap between them means panels are re-showing the same orient frame.
    {
        const DWORD nowMs = GetTickCount();
        if (m_dwMirrorFpsTick == 0)
            m_dwMirrorFpsTick = nowMs;
        const DWORD elapsed = nowMs - m_dwMirrorFpsTick;
        if (elapsed >= 1000) {
            const float secs = (float)elapsed / 1000.0f;
            const unsigned o = m_nOrientFrameAccum.exchange(0, std::memory_order_relaxed);
            const unsigned pr = m_nMirrorPresentAccum.exchange(0, std::memory_order_relaxed);
            m_fOrientFps = (float)o / secs;
            m_fMirrorPresentFps = (float)pr / secs;
            m_dwMirrorFpsTick = nowMs;
            if (m_bMirrorsActive) {
                char logBuf[160];
                sprintf(logBuf, "MirrorFPS: orient=%.1f present=%.1f\n",
                        m_fOrientFps, m_fMirrorPresentFps);
                DebugLogA(logBuf, LOG_INFO);
            }
        }
    }

    // Primary geom changed (resize / Windows primary-monitor switch): never leave
    // orient SizeGuard aspect or opposite-orient warp mesh on the primary path.
    if (m_bPrimaryGeomDirty.exchange(false)) {
        ClearOutputSizeOverride();
        RestorePrimaryTexSizeFromVS();
        // Soft orient reset — keep RTs if size still matches; force warmup clear.
        if (m_orientPipe.ready) {
            m_orientPipe.frames = 0;
            m_orientPipe.fbIdx = 0;
        }
        // Drop primary-BB copy so next frame rebuilds at the new size (stale
        // m_mirrorSrcTex after landscape↔portrait caused wrong stretch samples).
        m_mirrorSrcTex.Reset();
        m_bMirrorSrcCopiedThisFrame = false;
        m_bMirrorResetOrientNextFrame.store(true);
        m_nDeferMirrorResize.store(8);
        DebugLogA("SendToDisplayOutputs: primary geom dirty — skip mirror init/draw this frame\n", LOG_INFO);
        return;
    }

    // Finish deferred activate only on a stable-size frame (not the FS resize).
    {
        int defer = m_nDeferMirrorActivate.load();
        if (defer > 0) {
            if (m_nDeferMirrorActivate.fetch_sub(1) == 1 && !m_bMirrorsActive) {
                m_bMirrorsActive = true;
                m_bMirrorStylesDirty.store(true);
                m_bRaiseMirrorsNextFrame.store(true);
                DebugLogA("SendToDisplayOutputs: deferred mirror activate\n", LOG_INFO);
            } else {
                return;
            }
        }
    }

    // Apply deferred mirror style changes (set by UI thread via m_bMirrorStylesDirty)
    if (m_bMirrorStylesDirty.exchange(false))
        ApplyMirrorWindowStyles();

    // Apply pending fullscreen/windowed layout (size may require SC resize)
    {
        bool anyLayout = false;
        for (auto& out : m_displayOutputs) {
            if (out.monitorState && out.monitorState->bPendingLayout) {
                anyLayout = true;
                break;
            }
        }
        if (anyLayout) {
            for (auto& out : m_displayOutputs) {
                if (!out.monitorState || !out.monitorState->bPendingLayout)
                    continue;
                auto& ms = *out.monitorState;
                ms.bPendingLayout = false;
                if (!ms.hWnd || !ms.swapChain) continue;
                const int x = ms.pendingX, y = ms.pendingY;
                const int w = ms.pendingW, h = ms.pendingH;
                if (w <= 0 || h <= 0) continue;
                SetWindowPos(ms.hWnd, m_bAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                             x, y, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                if (ms.width != w || ms.height != h)
                    ResizeMirrorSwapChain(ms, w, h);
            }
        }
    }

    // Soft reset orient history when re-entering independent (do NOT free RTs —
    // CreateRenderTargetTexture RTV slots are never reclaimed; free+recreate
    // exhausted the dynamic RTV heap → permanent black until full app restart.
    if (m_bMirrorResetOrientNextFrame.exchange(false)) {
        if (m_orientPipe.ready) {
            m_orientPipe.frames = 0;
            m_orientPipe.fbIdx = 0;
        }
    }

    // After mirror on (and first Init of HWNDs), bring surfaces to front once.
    const bool raiseThisFrame = m_bRaiseMirrorsNextFrame.exchange(false);

    // Force reinit: tear down session-lived monitor mirrors so SC/window style
    // changes (layered, flip model, buffer count) actually take effect.
    // Without this, InitDisplayOutput early-returns and leaves the old SC forever.
    // NOTE: Independent toggle must NOT set this flag — only opacity/layer style.
    if (m_bMirrorForceReinit.exchange(false)) {
        for (auto& out : m_displayOutputs) {
            if (out.config.type == DisplayOutputType::Monitor && out.monitorState)
                DestroyDisplayOutput(out);
        }
        ReleaseOrientPipeline();
        m_bRaiseMirrorsNextFrame.store(true);
        DebugLogA("SendToDisplayOutputs: force-reinit destroyed monitor mirrors for recreate\n", LOG_WARN);
    }

    // After resize / ResetDynamicDescriptors the SRV bump rewinds — letterbox block
    // must be reallocated (RTVs use a permanent reserve and only need re-CreateRTV).
    if (m_mirrorLetterboxSrvEpoch != m_lpDX->m_descriptorEpoch) {
        m_mirrorLetterboxSrvBase = UINT_MAX;
        m_mirrorLetterboxSrvEpoch = m_lpDX->m_descriptorEpoch;
    }

    int mainW = m_lpDX->m_client_width;
    int mainH = m_lpDX->m_client_height;
    UINT fi = m_lpDX->m_frameIndex;
    bool hasActiveMonitors = false;

    // Primary monitor detection (never mirror onto the render window's display)
    wchar_t renderDevice[32] = {};
    if (m_lpDX->GetHwnd()) {
        if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
            wcscpy_s(renderDevice, m_szWatermarkRenderDevice);
        } else {
            HMONITOR hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW rmi = { sizeof(rmi) };
            if (hRenderMon && GetMonitorInfoW(hRenderMon, &rmi))
                wcscpy_s(renderDevice, rmi.szDevice);
        }
    }
    const bool gotDevice = renderDevice[0] != L'\0';

    // ── Session-lived mirrors: show/hide only — never Destroy on per-monitor toggle ──
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor)
            continue;

        const bool isPrimary = gotDevice &&
            wcscmp(out.config.szDeviceName, renderDevice) == 0;
        if (isPrimary)
            out.bSkippedSameMonitor = true;
        else if (m_bMirrorsActive && out.config.bEnabled)
            out.bSkippedSameMonitor = false;

        const bool wantVisible = m_bMirrorsActive && out.config.bEnabled &&
            !out.bSkippedSameMonitor;

        if (!out.monitorState) {
            // Create once when first needed
            if (wantVisible)
                InitDisplayOutput(out);
        }

        if (!out.monitorState || !out.monitorState->bReady)
            continue;

        auto& ms = *out.monitorState;
        if (wantVisible) {
            ms.bSoftDisabled = false;
            ms.nFramesUntilHardDestroy = 0;
            // Re-bind RTVs after any descriptor epoch change (window resize rebuilds
            // dynamic RTVs and used to overwrite bump-allocated mirror slots → SEH crash).
            if (ms.bHasRtv && ms.rtvSlotBase != UINT_MAX &&
                ms.rtvEpoch != m_lpDX->m_descriptorEpoch) {
                for (UINT i = 0; i < ms.bufferCount && i < MIRROR_BUFFER_COUNT; i++) {
                    if (!ms.backBuffers[i])
                        continue;
                    ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
                    m_lpDX->m_device->CreateRenderTargetView(
                        ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
                }
                ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
                DebugLogA("SendToDisplayOutputs: refreshed mirror RTVs after descriptor rewind\n", LOG_WARN);
            }
            if (ms.hWnd) {
                // Raise only on hide→show (monitor just enabled). Never every frame
                // (DWM thrashing floors FPS at ~30). Same TOPMOST→NOTOPMOST hop as
                // RaiseMirrorSurfaces so the surface is frontmost without sticky AOT
                // unless Always On Top is on.
                const bool wasHidden = !IsWindowVisible(ms.hWnd);
                if (wasHidden) {
                    ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
                    const UINT zflags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
                    SetWindowPos(ms.hWnd, HWND_TOPMOST, 0, 0, 0, 0, zflags);
                    if (!m_bAlwaysOnTop)
                        SetWindowPos(ms.hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, zflags);
                }
            }
            hasActiveMonitors = true;
        } else {
            // Park the mirror: hide + skip draws. Keep SC alive for the session.
            ms.bSoftDisabled = true;
            if (ms.hWnd && IsWindowVisible(ms.hWnd))
                ShowWindow(ms.hWnd, SW_HIDE);
        }
    }

    // Independent opposite-aspect SCs are capped native; copy-mode is primary
    // size. Resize on the render thread only (UI must not WaitForGpu).
    if (m_nDeferMirrorResize.load() > 0)
        m_nDeferMirrorResize.fetch_sub(1);
    if (m_nDeferMirrorResize.load() <= 0 && m_bMirrorIndepSizeDirty.exchange(false)) {
        if (m_mirrorWorkFence && m_mirrorWorkSubmitted > 0 &&
            m_mirrorWorkFence->GetCompletedValue() < m_mirrorWorkSubmitted) {
            m_bMirrorIndepSizeDirty.store(true); // worker still presenting
        } else {
        const int primW = m_lpDX->m_client_width;
        const int primH = m_lpDX->m_client_height;
        const bool lagIdleForResize = LagIndepFenceIdle();
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor || !out.monitorState)
                continue;
            if (!out.monitorState->bReady || !out.monitorState->swapChain)
                continue;
            int wantW = primW, wantH = primH;
            MirrorSwapChainSize(out, primW, primH, wantW, wantH);
            if (wantW > 0 && wantH > 0 &&
                (out.monitorState->width != wantW || out.monitorState->height != wantH)) {
                if (!lagIdleForResize && out.config.bIndependentRender) {
                    m_bMirrorIndepSizeDirty.store(true); // retry when lag fence is idle
                    continue;
                }
                ResizeMirrorSwapChain(*out.monitorState, wantW, wantH);
            }
        }
        }
    }

    // Mirror on: raise mirror HWNDs here (render-owned). Ask UI thread to raise primary
    // — never SetWindowPos the primary from this thread (cross-thread deadlock).
    if (raiseThisFrame) {
        RaiseMirrorSurfaces(nullptr); // mirrors only
        HWND hPrimary = m_lpDX->GetHwnd();
        if (hPrimary)
            PostMessage(hPrimary, WM_MW_RAISE_PRIMARY, 0, 0);
    }

    // Never hard-destroy monitor mirrors while the app is running — CreateSwapChain
    // thrash is what locked the PC after two toggles. Park = hide only.

    // Spout cleanup when disabled
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor &&
            !out.config.bEnabled && out.spoutState)
            DestroyDisplayOutput(out);
    }

    // Spout send
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Spout)
            continue;
        if (!out.spoutState || !out.spoutState->bReady) {
            if (!out.spoutState)
                InitDisplayOutput(out);
            if (!out.spoutState || !out.spoutState->bReady)
                continue;
        }
        out.spoutState->sender.SendDX11Resource(out.spoutState->wrappedBackBuffers[fi]);
    }

    if (!hasActiveMonitors) {
        // Restore default DXGI latency after a multi-mirror session (stuck high
        // latency was observed as a permanent ~30–33 FPS ceiling).
        m_lpDX->m_bSerializeWithMirrors = false;
        m_lpDX->EnsureMultiSwapChainFrameLatency(1, false);
        return;
    }

    // Stretch/copy on the *same* queue (pre-July-29 path). CopyResource when
    // the mirror SC matches the flip BB; stretch-blit otherwise. Present is
    // non-blocking. Independent re-render is not in this path.
    ID3D12Resource* flipBB = m_lpDX->m_renderTargets[fi].Get();
    if (!flipBB)
        return;
    const D3D12_RESOURCE_DESC flipDesc = flipBB->GetDesc();
    const int bbW = (int)flipDesc.Width;
    const int bbH = (int)flipDesc.Height;
    if (bbW <= 0 || bbH <= 0)
        return;

    if (!m_mirrorCmdAllocators[0]) {
        for (int i = 0; i < DXC_FRAME_COUNT; i++) {
            if (FAILED(m_lpDX->m_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mirrorCmdAllocators[i]))))
                return;
        }
        if (FAILED(m_lpDX->m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_mirrorCmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_mirrorCmdList))))
            return;
        m_mirrorCmdList->Close();
    }

    HRESULT hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
    if (FAILED(hrAlloc)) {
        m_lpDX->WaitForGpu();
        hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
    }
    if (FAILED(hrAlloc))
        return;
    HRESULT hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
    if (FAILED(hrList)) {
        m_lpDX->WaitForGpu();
        hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
    }
    if (FAILED(hrList))
        return;

    ID3D12GraphicsCommandList* cmd = m_mirrorCmdList.Get();
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
    m_lpDX->BeginAuxUpload();

    // Prefer the pre-Execute snapshot (same size as flip BB). Else copy the flip BB.
    ID3D12Resource* src = flipBB;
    D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_PRESENT;
    int srcW = bbW, srcH = bbH;
    const bool haveSnap = m_bMirrorSrcCopiedThisFrame && m_mirrorSrcTex.IsValid() &&
        m_mirrorSrcTex.resource &&
        (int)m_mirrorSrcTex.width == bbW && (int)m_mirrorSrcTex.height == bbH;
    if (haveSnap) {
        src = m_mirrorSrcTex.resource.Get();
        srcState = m_mirrorSrcTex.currentState;
        srcW = (int)m_mirrorSrcTex.width;
        srcH = (int)m_mirrorSrcTex.height;
    }

    auto isPortrait = [](int w, int h) { return h > w; };
    const bool mainPortrait = isPortrait(bbW, bbH);
    bool anyOppositeIndep = false;
    int oppW = 0, oppH = 0;
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.config.bIndependentRender)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool port = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                            : isPortrait(out.monitorState->width, out.monitorState->height);
        if (port == mainPortrait)
            continue;
        anyOppositeIndep = true;
        int rawW = (lw > 0) ? lw : out.monitorState->width;
        int rawH = (lh > 0) ? lh : out.monitorState->height;
        int tw = rawW, th = rawH;
        CapMirrorSwapChainDim(tw, th);
        if ((long long)tw * th > (long long)oppW * oppH) {
            oppW = tw;
            oppH = th;
            m_orientPanelW.store(rawW);
            m_orientPanelH.store(rawH);
        }
    }
    // Match EnsureOrientPipeline (cap 1920 + 16-align) or we recreate+WaitForGpu
    // every frame and TDR.
    if (oppW > 0 && oppH > 0) {
        const int maxDim = 1920;
        if (oppW > maxDim || oppH > maxDim) {
            float sc = (float)maxDim / (float)((oppW > oppH) ? oppW : oppH);
            oppW = max(1, (int)(oppW * sc + 0.5f));
            oppH = max(1, (int)(oppH * sc + 0.5f));
        }
        oppW = ((oppW + 15) / 16) * 16;
        oppH = ((oppH + 15) / 16) * 16;
    }

    const bool workerIdle = !m_mirrorWorkFence || m_mirrorWorkSubmitted == 0 ||
        m_mirrorWorkFence->GetCompletedValue() >= m_mirrorWorkSubmitted;
    if (anyOppositeIndep && oppW > 0 && oppH > 0 && workerIdle) {
        m_orientNeedW.store(oppW);
        m_orientNeedH.store(oppH);
        const bool needPipe = !m_orientPipe.ready ||
            m_orientPipe.w != oppW || m_orientPipe.h != oppH ||
            m_orientPipe.bindEpoch != m_lpDX->m_descriptorEpoch;
        if (needPipe) {
            m_lpDX->WaitForGpu();
            EnsureOrientPipeline(oppW, oppH);
            m_bOrientImageReady.store(false);
        }
        // The worker now owns a dedicated upload ring (StartMirrorThread), so it
        // can no longer stomp the render thread's offsets. Start it once an
        // opposite-orient independent panel exists; it free-runs from here and is
        // deliberately not frame-locked to the primary.
        if (!m_hMirrorThread)
            StartMirrorThread();
    }

    // No workerIdle requirement: the blit reads the PUBLISHED (double-buffered,
    // fence-proven) display face, never the one in flight. Requiring idle here
    // made the path fall back to a stretched primary whenever the worker was
    // busy — the panels alternated two different images (~20 Hz flicker).
    const bool orientReady = anyOppositeIndep && m_bOrientImageReady.load() &&
        m_orientPipe.ready && m_orientPipe.frames >= 2 &&
        m_orientPublishedIdx.load(std::memory_order_acquire) >= 0;

    if (anyOppositeIndep && m_nDeferMirrorResize.load() <= 0 && workerIdle) {
        for (auto& out : m_displayOutputs) {
            if (!out.config.bIndependentRender || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || !out.monitorState->swapChain)
                continue;
            int wantW = bbW, wantH = bbH;
            MirrorSwapChainSize(out, bbW, bbH, wantW, wantH);
            if (wantW > 0 && wantH > 0 &&
                (out.monitorState->width != wantW || out.monitorState->height != wantH))
                ResizeMirrorSwapChain(*out.monitorState, wantW, wantH);
        }
    }

    bool anyCopy = false;
    bool anyBlit = false;
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool panelPort = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                                 : isPortrait(ms.width, ms.height);
        const bool oppositeIndep = out.config.bIndependentRender &&
            (panelPort != mainPortrait);
        if (oppositeIndep || ms.width != srcW || ms.height != srcH)
            anyBlit = true;
        else
            anyCopy = true;
    }

    if (anyCopy && srcState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.StateBefore = srcState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        srcState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (anyBlit && srcState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.StateBefore = srcState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        srcState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        UINT mirrorFI = ms.swapChain->GetCurrentBackBufferIndex();
        if (mirrorFI >= ms.bufferCount || mirrorFI >= MIRROR_BUFFER_COUNT ||
            !ms.backBuffers[mirrorFI])
            continue;
        ID3D12Resource* mirrorBB = ms.backBuffers[mirrorFI].Get();
        const D3D12_RESOURCE_STATES before = ms.bEverPresented
            ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;

        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool panelPort = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                                 : isPortrait(ms.width, ms.height);
        const bool oppositeIndep = out.config.bIndependentRender &&
            (panelPort != mainPortrait);
        const bool useOrient = oppositeIndep && orientReady;
        ms.bFreshDraw = true;
        // Present gating: no new sim frame published since this panel's last
        // draw → keep the presented image (skip draw AND present). Re-blitting
        // and re-presenting identical content every render frame was ~2x the
        // sim rate in pure waste.
        if (useOrient && ms.bEverPresented &&
            ms.lastPubSeen == m_orientPublishedIdx.load(std::memory_order_acquire)) {
            ms.bFreshDraw = false;
            continue;
        }
        // Never 1:1-copy a portrait primary into a landscape SC (or vice versa).
        const bool doCopy = !oppositeIndep && (ms.width == srcW && ms.height == srcH);
        D3D12_RESOURCE_BARRIER mb = {};
        mb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        mb.Transition.pResource = mirrorBB;
        mb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        if (doCopy) {
            if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
                mb.Transition.StateBefore = before;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                cmd->ResourceBarrier(1, &mb);
            }
            cmd->CopyResource(mirrorBB, src);
            if (out.config.bIndependentRender) {
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cmd->ResourceBarrier(1, &mb);
                DrawOverlaysToMirror(cmd, ms.width, ms.height, false);
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                cmd->ResourceBarrier(1, &mb);
            } else {
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                cmd->ResourceBarrier(1, &mb);
            }
            ms.lastPath = 4;
        } else {
            if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                mb.Transition.StateBefore = before;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cmd->ResourceBarrier(1, &mb);
            }
            if (useOrient) {
                if (!BlitOrientOutputToMirror(cmd, ms.rtvHandles[mirrorFI],
                                              ms.width, ms.height))
                    BlitMainToMirror(cmd, src, srcW, srcH,
                                     ms.rtvHandles[mirrorFI], ms.width, ms.height, 0);
                // Orient pass is warp+comp only — draw messages/sprites at panel size.
                DrawOverlaysToMirror(cmd, ms.width, ms.height, true);
                ms.lastPath = 1;
                ms.lastPubSeen = m_orientPublishedIdx.load(std::memory_order_acquire);
            } else {
                // Opposite independent without a ready orient image: if this
                // panel EVER showed a sim frame, hold it — a stretch-filled
                // portrait frame here is exactly the "infrequent whole-screen
                // stretched flash" Shane pinned down (fires for 1-2 worker
                // periods on every orient-pipe recreate: descriptor-heap
                // rewind, preset mode switch, resize). Stretch only serves the
                // very first light-up, before any sim frame exists.
                if (oppositeIndep && ms.bEverPresented) {
                    ms.bFreshDraw = false;
                    ms.lastPath = 6;
                    // Undo the RT transition recorded above for this face.
                    mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                    cmd->ResourceBarrier(1, &mb);
                    if (mirrorFI < MIRROR_BUFFER_COUNT)
                        ms.bbState[mirrorFI] = D3D12_RESOURCE_STATE_PRESENT;
                    continue;
                }
                // Stretch-fill (letterbox looked like an unstretched portrait copy).
                const int scale = oppositeIndep ? 0 : (out.config.bIndependentRender ? 1 : 0);
                BlitMainToMirror(cmd, src, srcW, srcH,
                                 ms.rtvHandles[mirrorFI], ms.width, ms.height, scale);
                if (out.config.bIndependentRender)
                    DrawOverlaysToMirror(cmd, ms.width, ms.height, false);
                ms.lastPath = 2;
            }
            mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            cmd->ResourceBarrier(1, &mb);
        }
        if (mirrorFI < MIRROR_BUFFER_COUNT)
            ms.bbState[mirrorFI] = D3D12_RESOURCE_STATE_PRESENT;
        ms.lastDrawFI = mirrorFI;
        ms.drawCount++;
        if (ms.hWnd && !IsWindowVisible(ms.hWnd))
            ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
    }

    if (haveSnap)
        m_mirrorSrcTex.currentState = srcState;
    else if (srcState != D3D12_RESOURCE_STATE_PRESENT) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.StateBefore = srcState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    }

    cmd->Close();
    m_lpDX->EndAuxUpload();
    ID3D12CommandList* lists[] = { cmd };
    m_lpDX->m_commandQueue->ExecuteCommandLists(1, lists);

    // DXGI frame latency is DEVICE-wide. At the default (2-3), multi-swapchain
    // flip queues can re-scan a STALE face every other refresh — visible as a
    // two-image flicker on the panels that composition-level captures
    // (PrintWindow/ImageGrab) can NOT see, since DWM composes the newest
    // frame while scanout flips the queue. The latency=1 call lived only in
    // the dead slot-path since the July rework; restore it here (2026-08-21).
    {
        int nMirrorPresents = 0;
        for (auto& out : m_displayOutputs)
            if (out.config.bEnabled && out.config.type == DisplayOutputType::Monitor &&
                out.monitorState && out.monitorState->bReady && !out.monitorState->bSoftDisabled)
                nMirrorPresents++;
        m_lpDX->EnsureMultiSwapChainFrameLatency(1 + nMirrorPresents, true);
    }

    // SC is created with Flags=0 — ALLOW_TEARING is invalid and Present fails
    // (invisible HWND). First present is blocking so flip-model actually shows.
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        if (!ms.bFreshDraw)
            continue; // nothing new drawn this frame — keep the shown image
        if (ms.hWnd && !IsWindowVisible(ms.hWnd))
            ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
        const UINT flags = ms.bEverPresented ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
        HRESULT hr = ms.swapChain->Present(0, flags);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            hr = ms.swapChain->Present(0, 0);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                ms.presentSkipCount++;
                ms.lastPresentHr = hr;
                continue;
            }
        }
        ms.lastPresentHr = hr;
        if (SUCCEEDED(hr)) {
            ms.bEverPresented = true;
            ms.presentOkCount++;
            m_nMirrorPresentAccum.fetch_add(1, std::memory_order_relaxed);
        } else {
            char logBuf[256];
            sprintf(logBuf, "Mirror Present failed (0x%08X) on %ls\n",
                    (unsigned)hr, out.config.szDeviceName);
            DebugLogA(logBuf, LOG_ERROR);
            ms.presentFailCount++;
        }
    }

    return;

#if 0 // old same-thread independent path — kept for reference, not compiled
    bool anyIndepActive = false;
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.config.bEnabled)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        if (out.config.bIndependentRender)
            anyIndepActive = true;
    }

    // Independent off: copy when SC == primary, else blit (SC is capped at 1920
    // so a 2160x3840 primary must not ResizeBuffers to 4K — that TDRs).
    if (!anyIndepActive) {
        if (m_nDeferMirrorResize.load() <= 0) {
            for (auto& out : m_displayOutputs) {
                if (out.config.type != DisplayOutputType::Monitor || !out.config.bEnabled)
                    continue;
                if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
                    continue;
                int wantW = mainW, wantH = mainH;
                MirrorSwapChainSize(out, mainW, mainH, wantW, wantH);
                auto& ms = *out.monitorState;
                if (wantW > 0 && wantH > 0 && (ms.width != wantW || ms.height != wantH))
                    ResizeMirrorSwapChain(ms, wantW, wantH);
            }
        }

        bool allSameSize = true;
        for (auto& out : m_displayOutputs) {
            if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
                continue;
            if (out.monitorState->width != mainW || out.monitorState->height != mainH) {
                allSameSize = false;
                break;
            }
        }
        // Same-size CopyResource of the flip BB after Execute used to be the
        // July path. It freezes when client size != BB desc, and the cheap
        // blit below already covers copy-mode. Always fall through.
        if (false && allSameSize) {
        if (!m_mirrorCmdAllocators[0]) {
            for (int i = 0; i < DXC_FRAME_COUNT; i++) {
                HRESULT hr = m_lpDX->m_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mirrorCmdAllocators[i]));
                if (FAILED(hr)) {
                    DebugLogA("SendToDisplayOutputs: CreateCommandAllocator failed\n", LOG_ERROR);
                    return;
                }
            }
            HRESULT hr = m_lpDX->m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_mirrorCmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_mirrorCmdList));
            if (FAILED(hr)) {
                DebugLogA("SendToDisplayOutputs: CreateCommandList failed\n", LOG_ERROR);
                return;
            }
            m_mirrorCmdList->Close();
        }

        HRESULT hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
        if (FAILED(hrAlloc)) {
            m_lpDX->WaitForGpu();
            hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
        }
        if (FAILED(hrAlloc))
            return;
        HRESULT hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
        if (FAILED(hrList)) {
            m_lpDX->WaitForGpu();
            hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
        }
        if (FAILED(hrList))
            return;

        ID3D12Resource* mainBB = m_lpDX->m_renderTargets[fi].Get();
        D3D12_RESOURCE_STATES mainBefore = D3D12_RESOURCE_STATE_PRESENT;
        if (m_bDisableMirrorHud && m_bMirrorSrcCopiedThisFrame &&
            m_mirrorSrcTex.IsValid() && m_mirrorSrcTex.resource &&
            m_mirrorSrcTex.width == (UINT)mainW && m_mirrorSrcTex.height == (UINT)mainH) {
            mainBB = m_mirrorSrcTex.resource.Get();
            mainBefore = m_mirrorSrcTex.currentState;
        }
        if (!mainBB) {
            m_mirrorCmdList->Close();
            return;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = mainBB;
        barrier.Transition.StateBefore = mainBefore;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_mirrorCmdList->ResourceBarrier(1, &barrier);

        for (auto& out : m_displayOutputs) {
            if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
                continue;
            auto& ms = *out.monitorState;
            if (ms.width != mainW || ms.height != mainH)
                continue;
            UINT mirrorFI = ms.swapChain->GetCurrentBackBufferIndex();
            if (mirrorFI >= ms.bufferCount || mirrorFI >= MIRROR_BUFFER_COUNT ||
                !ms.backBuffers[mirrorFI])
                continue;
            ID3D12Resource* mirrorBB = ms.backBuffers[mirrorFI].Get();
            const D3D12_RESOURCE_STATES before = ms.bEverPresented
                ? D3D12_RESOURCE_STATE_PRESENT
                : D3D12_RESOURCE_STATE_COMMON;

            D3D12_RESOURCE_BARRIER mirrorBarrier = {};
            mirrorBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            mirrorBarrier.Transition.pResource = mirrorBB;
            mirrorBarrier.Transition.StateBefore = before;
            mirrorBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            mirrorBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_mirrorCmdList->ResourceBarrier(1, &mirrorBarrier);

            m_mirrorCmdList->CopyResource(mirrorBB, mainBB);

            mirrorBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            mirrorBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            m_mirrorCmdList->ResourceBarrier(1, &mirrorBarrier);
            if (mirrorFI < MIRROR_BUFFER_COUNT)
                ms.bbState[mirrorFI] = D3D12_RESOURCE_STATE_PRESENT;
            ms.lastPath = 4;
            ms.lastDrawFI = mirrorFI;
            ms.drawCount++;
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = mainBefore;
        m_mirrorCmdList->ResourceBarrier(1, &barrier);
        if (mainBB == m_mirrorSrcTex.resource.Get())
            m_mirrorSrcTex.currentState = mainBefore;
        m_mirrorCmdList->Close();

        ID3D12CommandList* lists[] = { m_mirrorCmdList.Get() };
        m_lpDX->m_commandQueue->ExecuteCommandLists(1, lists);

        int nMirrorPresents = 0;
        for (auto& out : m_displayOutputs) {
            if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
                continue;
            nMirrorPresents++;
            HRESULT hr = out.monitorState->swapChain->Present(0, 0);
            if (FAILED(hr)) {
                char logBuf[256];
                sprintf(logBuf, "Mirror Present failed (0x%08X) on %ls (no destroy)\n",
                        (unsigned)hr, out.config.szDeviceName);
                DebugLogA(logBuf, LOG_ERROR);
                out.monitorState->presentFailCount++;
            } else {
                out.monitorState->bEverPresented = true;
                out.monitorState->presentOkCount++;
            }
        }
        m_lpDX->m_bSerializeWithMirrors = true;
        m_lpDX->EnsureMultiSwapChainFrameLatency(1 + nMirrorPresents, true);
        return;
        } // allSameSize: else fall through and blit
    }

    // Collect active monitor slots; decide main-BB sampling needs before opening the list.
    struct MirrorSlot {
        DisplayOutput* out = nullptr;
        MonitorMirrorState* ms = nullptr;
        UINT mirrorFI = 0;
        ID3D12Resource* bb = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
        bool doIndepReRender = false; // milk3 or classic independent re-render
        bool portrait = false;
        bool oppositeOrient = false; // independent re-render at opposite aspect
        int  pipeW = 0, pipeH = 0;   // capped panel size (aspect of the HWND)
    };
    auto isPortrait = [](int w, int h) { return h > w; };

    std::vector<MirrorSlot> slots;
    slots.reserve(m_displayOutputs.size());
    bool needMainAsCopySrc = false;
    bool needMainAsSrv = false;
    bool anyIndepMilk3 = false;
    bool anyIndepReRender = false;
    bool anyOppositeOrient = false;
    const bool mainPortrait = isPortrait(mainW, mainH);
    // Independent: milk3 (orient pipe Image) OR classic .milk/.milk2 (warp+comp;
    // milk2 is frozen dual-preset blend on the classic path, not Shadertoy).
    const bool canIndepMilk3 = m_bShadertoyMode && m_dx12CompPSO;
    const bool canIndepClassic = !m_bShadertoyMode && m_lpDX && m_lpDX->m_device;

    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        UINT mirrorFI = ms.swapChain->GetCurrentBackBufferIndex();
        if (mirrorFI >= ms.bufferCount || mirrorFI >= MIRROR_BUFFER_COUNT ||
            !ms.backBuffers[mirrorFI] || !ms.bHasRtv)
            continue;

        MirrorSlot s;
        s.out = &out;
        s.ms = &ms;
        s.mirrorFI = mirrorFI;
        s.bb = ms.backBuffers[mirrorFI].Get();
        s.rtv = ms.rtvHandles[mirrorFI];
        // Aspect from the HWND/panel, not the SC. Copy-mode SCs match the
        // primary; using ms.size then never saw opposite-orient milk2.
        int layX = 0, layY = 0, layW = 0, layH = 0;
        ComputeMirrorLayout(out.config, layX, layY, layW, layH);
        s.portrait = (layW > 0 && layH > 0) ? isPortrait(layW, layH)
                                            : isPortrait(ms.width, ms.height);
        s.pipeW = (layW > 0) ? layW : ms.width;
        s.pipeH = (layH > 0) ? layH : ms.height;
        CapMirrorSwapChainDim(s.pipeW, s.pipeH);
        s.doIndepReRender = out.config.bIndependentRender &&
            (canIndepMilk3 || canIndepClassic);
        s.oppositeOrient = out.config.bIndependentRender && (s.portrait != mainPortrait);
        if (s.oppositeOrient)
            anyOppositeOrient = true;
        if (s.doIndepReRender) {
            anyIndepReRender = true;
            if (canIndepMilk3)
                anyIndepMilk3 = true;
        } else if (ms.width == mainW && ms.height == mainH && !out.config.bIndependentRender) {
            needMainAsCopySrc = true;
        } else {
            needMainAsSrv = true;
        }
        slots.push_back(s);
    }
    if (slots.empty())
        return;

    // Same-orient independent always blits primary (cheap). Opposite uses the
    // lagged pass — never a second warp+comp on this list.
    if (anyIndepReRender) {
        for (const auto& s : slots) {
            if (s.doIndepReRender && s.portrait == mainPortrait)
                needMainAsSrv = true;
        }
    }

    const bool lagIdle = LagIndepFenceIdle();
    const bool auxSlotBusy = !lagIdle && m_lagIndepAuxFrame == (UINT)fi;

    // Pre-create opposite-orient pipe only when we can record a lag pass.
    // WaitForGpu here is rare (size/epoch change) and only while the lag fence is idle.
    if (anyOppositeOrient && anyIndepReRender && lagIdle && !auxSlotBusy) {
        int needW = 0, needH = 0;
        for (const auto& s : slots) {
            if (!s.doIndepReRender || !s.oppositeOrient) continue;
            long long a = (long long)s.pipeW * (long long)s.pipeH;
            long long best = (long long)needW * (long long)needH;
            if (a > best) {
                needW = s.pipeW;
                needH = s.pipeH;
            }
        }
        if (needW > 0 && needH > 0) {
            int capW = needW, capH = needH;
            const int maxDim = 1920; // match EnsureOrientPipeline (2560 TDRd with 4K primary)
            if (capW > maxDim || capH > maxDim) {
                float sc = (float)maxDim / (float)((capW > capH) ? capW : capH);
                capW = max(1, (int)(capW * sc + 0.5f));
                capH = max(1, (int)(capH * sc + 0.5f));
            }
            capW = ((capW + 15) / 16) * 16;
            capH = ((capH + 15) / 16) * 16;
            const bool needCreate = !m_orientPipe.ready ||
                m_orientPipe.w != capW || m_orientPipe.h != capH ||
                m_orientPipe.bindEpoch != m_lpDX->m_descriptorEpoch;
            if (needCreate) {
                m_lpDX->WaitForGpu();
                EnsureOrientPipeline(needW, needH);
            }
        }
    }

    // Lazy-create mirror command objects
    if (!m_mirrorCmdAllocators[0]) {
        for (int i = 0; i < DXC_FRAME_COUNT; i++) {
            HRESULT hr = m_lpDX->m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mirrorCmdAllocators[i]));
            if (FAILED(hr)) {
                DebugLogA("SendToDisplayOutputs: CreateCommandAllocator failed\n", LOG_ERROR);
                return;
            }
        }
        HRESULT hr = m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_mirrorCmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_mirrorCmdList));
        if (FAILED(hr)) {
            DebugLogA("SendToDisplayOutputs: CreateCommandList failed\n", LOG_ERROR);
            return;
        }
        m_mirrorCmdList->Close();
    }

    // Cheap blit list. Retry once with WaitForGpu so a busy allocator cannot
    // freeze the last presented frame.
    bool cheapOk = false;
    if (!auxSlotBusy) {
        HRESULT hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
        if (FAILED(hrAlloc)) {
            m_lpDX->WaitForGpu();
            hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
        }
        HRESULT hrList = FAILED(hrAlloc) ? E_FAIL
            : m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
        if (FAILED(hrList)) {
            m_lpDX->WaitForGpu();
            hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
        }
        if (SUCCEEDED(hrAlloc) && SUCCEEDED(hrList))
            cheapOk = true;
        else
            m_mirrorDiagSkipFrames++;
    }

    // Isolate mirror-path failures so they cannot cascade into safe mode.
    // Outer size guard: whatever SizeGuard/ImageToMirror does, leave primary texsize clean.
    struct PrimarySizeGuard {
        Engine* e;
        explicit PrimarySizeGuard(Engine* eng) : e(eng) {}
        ~PrimarySizeGuard() { e->RestorePrimaryTexSizeFromVS(); }
    } primarySizeGuard(this);

    try {

    ID3D12Resource* mainBB = m_lpDX->m_renderTargets[fi].Get();
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };

    bool beganAux = false;
    auto ensureAux = [&]() {
        if (!beganAux) {
            m_lpDX->BeginAuxUpload();
            beganAux = true;
        }
    };

    // Snapshot size is the flip-BB desc, which can disagree with client
    // (DPI / FS). Using client as the match left mainBB null → no draw →
    // no Present → invisible click-blocking HWND.
    ID3D12Resource* snapBB = nullptr;
    D3D12_RESOURCE_STATES snapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    int snapW = 0, snapH = 0;
    if (m_bMirrorSrcCopiedThisFrame && m_mirrorSrcTex.IsValid() &&
        m_mirrorSrcTex.resource) {
        snapBB = m_mirrorSrcTex.resource.Get();
        snapState = m_mirrorSrcTex.currentState;
        snapW = (int)m_mirrorSrcTex.width;
        snapH = (int)m_mirrorSrcTex.height;
    }
    (void)needMainAsSrv;
    (void)needMainAsCopySrc;
    (void)mainBB;

    bool processed[32] = {};
    bool cheapDrew[32] = {};
    bool lagDrewOpposite = false;
    const size_t nSlots = slots.size();
    if (nSlots > 32)
        DebugLogA("SendToDisplayOutputs: too many mirrors for coalesce mask\n", LOG_WARN);

    auto transitionOn = [&](ID3D12GraphicsCommandList* cmd, MirrorSlot& s,
                            D3D12_RESOURCE_STATES after) {
        if (!cmd || s.mirrorFI >= MIRROR_BUFFER_COUNT)
            return;
        D3D12_RESOURCE_STATES before = s.ms->bbState[s.mirrorFI];
        if (before == after)
            return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = s.bb;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        s.ms->bbState[s.mirrorFI] = after;
    };

    // ── Cheap path: copy / letterbox primary. Never warp+comp here. ──
    if (cheapOk) {
        m_mirrorCmdList->SetDescriptorHeaps(1, heaps);
        m_mirrorCmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
        ensureAux();

        for (size_t i = 0; i < nSlots; i++) {
            MirrorSlot& s = slots[i];
            // Blit every face, including opposite-orient. Lag may overwrite
            // those after; if it skips they stay the live primary (not black).

            if (s.mirrorFI < MIRROR_BUFFER_COUNT) {
                s.ms->bbState[s.mirrorFI] = s.ms->bEverPresented
                    ? D3D12_RESOURCE_STATE_PRESENT
                    : D3D12_RESOURCE_STATE_COMMON;
            }

            const bool sameSize = snapBB &&
                (s.ms->width == snapW && s.ms->height == snapH);
            const bool doCopy = sameSize && !s.out->config.bIndependentRender &&
                snapState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            const bool doBlit = snapBB && snapW > 0 && snapH > 0 && !doCopy;

            processed[i] = true;
            if (doCopy) {
                D3D12_RESOURCE_BARRIER toCopy = {};
                toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toCopy.Transition.pResource = snapBB;
                toCopy.Transition.StateBefore = snapState;
                toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                if (snapState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    m_mirrorCmdList->ResourceBarrier(1, &toCopy);
                snapState = D3D12_RESOURCE_STATE_COPY_SOURCE;
                transitionOn(m_mirrorCmdList.Get(), s, D3D12_RESOURCE_STATE_COPY_DEST);
                m_mirrorCmdList->CopyResource(s.bb, snapBB);
                transitionOn(m_mirrorCmdList.Get(), s, D3D12_RESOURCE_STATE_PRESENT);
                s.ms->lastPath = 4;
                cheapDrew[i] = true;
            } else if (doBlit) {
                if (snapState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                    D3D12_RESOURCE_BARRIER toSrv = {};
                    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    toSrv.Transition.pResource = snapBB;
                    toSrv.Transition.StateBefore = snapState;
                    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    m_mirrorCmdList->ResourceBarrier(1, &toSrv);
                    snapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                }
                transitionOn(m_mirrorCmdList.Get(), s, D3D12_RESOURCE_STATE_RENDER_TARGET);
                const int scaleMode = s.out->config.bIndependentRender ? 1 : 0;
                BlitMainToMirror(m_mirrorCmdList.Get(), snapBB, snapW, snapH,
                                 s.rtv, s.ms->width, s.ms->height, scaleMode);
                transitionOn(m_mirrorCmdList.Get(), s, D3D12_RESOURCE_STATE_PRESENT);
                s.ms->lastPath = scaleMode == 1 ? 3 : 2;
                cheapDrew[i] = true;
            } else {
                continue;
            }
            s.ms->lastDrawFI = s.mirrorFI;
            s.ms->drawCount++;
        }

        if (snapBB && snapState != m_mirrorSrcTex.currentState)
            m_mirrorSrcTex.currentState = snapState;

        m_mirrorCmdList->Close();
        ID3D12CommandList* cheapLists[] = { m_mirrorCmdList.Get() };
        m_lpDX->m_commandQueue->ExecuteCommandLists(1, cheapLists);
    }

    // ── Lagged opposite-orient: skip if previous fence still in flight. ──
    if (anyOppositeOrient && anyIndepReRender && lagIdle && !auxSlotBusy &&
        EnsureLagIndepObjects()) {
        HRESULT hrA = m_lagIndepAlloc->Reset();
        HRESULT hrL = FAILED(hrA) ? E_FAIL
            : m_lagIndepList->Reset(m_lagIndepAlloc.Get(), nullptr);
        if (SUCCEEDED(hrA) && SUCCEEDED(hrL)) {
            m_lagIndepList->SetDescriptorHeaps(1, heaps);
            m_lagIndepList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
            ensureAux();

            int leaderIdx = -1;
            long long bestArea = 0;
            for (size_t i = 0; i < nSlots; i++) {
                if (!slots[i].doIndepReRender || !slots[i].oppositeOrient)
                    continue;
                long long a = (long long)slots[i].pipeW * (long long)slots[i].pipeH;
                if (leaderIdx < 0 || a > bestArea) {
                    bestArea = a;
                    leaderIdx = (int)i;
                }
            }

            bool ok = false;
            if (leaderIdx >= 0) {
                MirrorSlot& leader = slots[leaderIdx];
                const int rw = leader.pipeW > 0 ? leader.pipeW : leader.ms->width;
                const int rh = leader.pipeH > 0 ? leader.pipeH : leader.ms->height;
                m_bOrientOppositeAspect = true;
                ok = canIndepMilk3
                    ? RenderMilk3OrientPipeline(m_lagIndepList.Get(), rw, rh)
                    : RenderClassicOrientPipeline(m_lagIndepList.Get(), rw, rh);
                m_bOrientOppositeAspect = false;
            }
            const bool showHud = ok && m_orientPipe.frames >= 2;
            // Warmup clears are black — do not overwrite the cheap snapshot blit.
            const bool useLagImage = ok && m_orientPipe.frames >= 2;

            if (useLagImage) {
            for (size_t i = 0; i < nSlots; i++) {
                MirrorSlot& s = slots[i];
                if (!s.doIndepReRender || !s.oppositeOrient)
                    continue;
                if (s.mirrorFI < MIRROR_BUFFER_COUNT) {
                    s.ms->bbState[s.mirrorFI] = s.ms->bEverPresented
                        ? D3D12_RESOURCE_STATE_PRESENT
                        : D3D12_RESOURCE_STATE_COMMON;
                }
                processed[i] = true;
                transitionOn(m_lagIndepList.Get(), s, D3D12_RESOURCE_STATE_RENDER_TARGET);
                bool drew = false;
                if (BlitOrientOutputToMirror(m_lagIndepList.Get(), s.rtv,
                                             s.ms->width, s.ms->height)) {
                    drew = true;
                    if (showHud)
                        DrawOverlaysToMirror(m_lagIndepList.Get(),
                                             s.ms->width, s.ms->height);
                }
                transitionOn(m_lagIndepList.Get(), s, D3D12_RESOURCE_STATE_PRESENT);
                s.ms->lastPath = drew ? 1 : 6;
                s.ms->lastDrawFI = s.mirrorFI;
                s.ms->drawCount++;
            }
            }

            m_lagIndepList->Close();
            ID3D12CommandList* lagLists[] = { m_lagIndepList.Get() };
            m_lpDX->m_commandQueue->ExecuteCommandLists(1, lagLists);
            m_lagIndepSignal++;
            m_lpDX->m_commandQueue->Signal(m_lagIndepFence.Get(), m_lagIndepSignal);
            m_lagIndepSubmitted = m_lagIndepSignal;
            m_lagIndepAuxFrame = fi;
            lagDrewOpposite = useLagImage;
        } else {
            m_mirrorDiagSkipFrames++;
        }
    } else if (anyOppositeOrient && anyIndepReRender) {
        m_mirrorDiagSkipFrames++;
        for (size_t i = 0; i < nSlots; i++) {
            if (slots[i].doIndepReRender && slots[i].oppositeOrient)
                processed[i] = true; // keep last presented image
        }
    }

    if (beganAux)
        m_lpDX->EndAuxUpload();

    // DIAG snapshot (observability only)
    m_mirrorDiagMainW = mainW;
    m_mirrorDiagMainH = mainH;
    m_mirrorDiagMainPortrait = mainPortrait ? 1 : 0;
    m_mirrorDiagNeedMainSrv = snapBB ? 1 : 0;
    m_mirrorDiagCanSampleMain = snapBB ? 1 : 0;
    m_mirrorDiagAnyOpposite = anyOppositeOrient ? 1 : 0;
    m_mirrorDiagAnyIndepMilk3 = anyIndepMilk3 ? 1 : 0;
    m_mirrorDiagShadertoy = m_bShadertoyMode ? 1 : 0;
    m_mirrorDiagCompPso = m_dx12CompPSO ? 1 : 0;
    m_mirrorDiagSlotCount = (int)nSlots;
    m_mirrorDiagFrameCounter++;
    if (m_lpDX->m_frameIndex < DXC_FRAME_COUNT)
        m_mirrorDiagAuxUsed = m_lpDX->m_auxUploadOffset[m_lpDX->m_frameIndex];

    // Device-wide present queue: 1 main + N mirrors. Keep latency modest; also
    // call with 1 when no mirrors so we restore default after multi-SC sessions.
    int nMirrorPresents = 0;
    for (auto& out : m_displayOutputs) {
        if (out.config.bEnabled && out.config.type == DisplayOutputType::Monitor &&
            out.monitorState && out.monitorState->bReady && !out.monitorState->bSoftDisabled)
            nMirrorPresents++;
    }
    // Latency 1 whenever mirrors are up. Latency 2 kept a stale landscape
    // flip face on the portrait primary (ghost strip) at high focused FPS.
    m_lpDX->m_bSerializeWithMirrors = true;
    m_lpDX->EnsureMultiSwapChainFrameLatency(1 + nMirrorPresents, true);

    // First Present must complete or the HWND stays a transparent click overlay.
    // After that, skip Present if we did not draw (keep last image). Hide until
    // the first successful present so an empty popup cannot eat clicks.
    const UINT tearFlag = m_lpDX->m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    for (size_t si = 0; si < nSlots; si++) {
        MirrorSlot& s = slots[si];
        auto& ms = *s.ms;
        const bool oppositeIndep = s.doIndepReRender && s.oppositeOrient;
        const bool drewThis = oppositeIndep ? lagDrewOpposite : cheapDrew[si];
        ms.lastOppositeIndep = oppositeIndep;
        if (!drewThis) {
            if (!ms.bEverPresented && ms.hWnd && IsWindowVisible(ms.hWnd))
                ShowWindow(ms.hWnd, SW_HIDE);
            ms.presentSkipCount++;
            continue;
        }
        if (ms.hWnd && !IsWindowVisible(ms.hWnd))
            ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
        if (ms.hWnd) {
            LONG_PTR ex = GetWindowLongPtrW(ms.hWnd, GWL_EXSTYLE);
            ms.lastLayered = (ex & WS_EX_LAYERED) != 0;
        }
        const UINT nBuf = ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT;
        const UINT allBits = (nBuf >= 32) ? 0xFFFFFFFFu : ((1u << nBuf) - 1u);
        const bool firstPresent = !ms.bEverPresented;
        ms.lastMustBlock = firstPresent;
        const UINT flags = firstPresent ? 0u : (tearFlag | DXGI_PRESENT_DO_NOT_WAIT);
        const UINT curIdx = ms.swapChain->GetCurrentBackBufferIndex();
        ms.lastPresentFI = curIdx;
        HRESULT hr = ms.swapChain->Present(0, flags);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && !firstPresent) {
            ms.presentSkipCount++;
            ms.lastPresentHr = hr;
            continue;
        }
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            hr = ms.swapChain->Present(0, 0);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                ms.presentSkipCount++;
                ms.lastPresentHr = hr;
                continue;
            }
        }
        ms.lastPresentHr = hr;
        if (FAILED(hr)) {
            char logBuf[256];
            sprintf(logBuf, "Mirror Present failed (0x%08X) on %ls (no destroy)\n",
                    (unsigned)hr, s.out->config.szDeviceName);
            DebugLogA(logBuf, LOG_ERROR);
            ms.presentFailCount++;
        } else {
            ms.bEverPresented = true;
            ms.presentOkCount++;
            if (curIdx < 32)
                ms.paintedBufferMask |= (1u << curIdx);
            if ((ms.paintedBufferMask & allBits) == allBits)
                ms.bNeedsFullChainClear = false;
        }
    }
    } catch (const std::exception& e) {
        char buf[256];
        sprintf(buf, "SendToDisplayOutputs: exception: %s\n", e.what());
        DebugLogA(buf, LOG_ERROR);
        m_lpDX->EndAuxUpload();
        RestorePrimaryTexSizeFromVS();
        if (m_mirrorCmdList)
            m_mirrorCmdList->Close();
        if (m_lagIndepList)
            m_lagIndepList->Close();
        if (m_lpDX)
            m_lpDX->WaitForGpu();
        m_mirrorCmdList.Reset();
        for (int i = 0; i < DXC_FRAME_COUNT; i++)
            m_mirrorCmdAllocators[i].Reset();
        ReleaseLagIndepObjects();
        ReleaseOrientPipeline();
        m_bMirrorForceReinit.store(true);
    } catch (...) {
        DebugLogA("SendToDisplayOutputs: unknown exception — force mirror reinit\n", LOG_ERROR);
        m_lpDX->EndAuxUpload();
        RestorePrimaryTexSizeFromVS();
        if (m_mirrorCmdList)
            m_mirrorCmdList->Close();
        if (m_lagIndepList)
            m_lagIndepList->Close();
        if (m_lpDX)
            m_lpDX->WaitForGpu();
        m_mirrorCmdList.Reset();
        for (int i = 0; i < DXC_FRAME_COUNT; i++)
            m_mirrorCmdAllocators[i].Reset();
        ReleaseLagIndepObjects();
        ReleaseOrientPipeline();
        m_bMirrorForceReinit.store(true);
    }
#endif
}

// ─── Independent mirror rendering ────────────────────────────────────────────

bool Engine::AnyIndependentMirrorEnabled() const
{
    if (!m_bMirrorsActive)
        return false;
    for (const auto& out : m_displayOutputs) {
        if (out.config.bEnabled &&
            out.config.type == DisplayOutputType::Monitor &&
            out.config.bIndependentRender)
            return true;
    }
    return false;
}

void Engine::DrawDeferredMessages()
{
    if (!MessagesEnabled() || !m_lpDX)
        return;
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
        if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
            float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
            if (fProgress <= 1.0f)
                ShowSongTitleAnim(GetWidth(), GetHeight(), min(fProgress, 0.9999f), i);
        }
    }
}

void Engine::DrawOverlaysToMirror(ID3D12GraphicsCommandList* cmdList, int monW, int monH,
                                  bool drawSprites)
{
    if (!cmdList || !m_lpDX || monW <= 0 || monH <= 0)
        return;

    // Viewport must match mirror RT (milk3 path already set it; re-assert for safety)
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

    // messages.ini / song-title anims stay on mirrors even when HUD is off.
    if (MessagesEnabled()) {
        for (int i = 0; i < NUM_SUPERTEXTS; i++) {
            if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
                float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
                if (fProgress <= 1.0f)
                    ShowSongTitleAnim(monW, monH, min(fProgress, 0.9999f), i, cmdList);
            }
        }
    }

    if (drawSprites && SpritesEnabled()) {
        DrawUserSprites(0, cmdList);
        DrawUserSprites(1, cmdList);
    }

    if (m_bDisableMirrorHud)
        return;

    // HUD / preset / notifications: queue was laid out in primary client pixels.
    // Pass primary as layout size so positions and font scale map to monW×monH.
    const int layoutW = m_lpDX->m_client_width > 0 ? m_lpDX->m_client_width : monW;
    const int layoutH = m_lpDX->m_client_height > 0 ? m_lpDX->m_client_height : monH;
    m_text.DrawNow(cmdList, monW, monH, false, layoutW, layoutH);
}

void Engine::RenderMilk3ImageToMirror(ID3D12GraphicsCommandList* cmdList,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv, int monW, int monH)
{
    if (!cmdList || !m_lpDX || monW <= 0 || monH <= 0)
        return;

    // RAII: restore primary VS texsize — portrait dims must not stick into next primary frame.
    struct SizeOverrideGuard {
        Engine* e;
        explicit SizeOverrideGuard(Engine* eng, int w, int h) : e(eng) {
            e->m_nTexSizeX = w;
            e->m_nTexSizeY = h;
            e->SetOutputSizeOverride(w, h);
            e->m_fAspectX = (h > w) ? w / (float)h : 1.0f;
            e->m_fAspectY = (w > h) ? h / (float)w : 1.0f;
            e->m_fInvAspectX = 1.0f / e->m_fAspectX;
            e->m_fInvAspectY = 1.0f / e->m_fAspectY;
        }
        ~SizeOverrideGuard() { e->RestorePrimaryTexSizeFromVS(); }
    } sizeGuard(this, monW, monH);

    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    if (!m_dx12CompPSO)
        return;
    cmdList->SetPipelineState(m_dx12CompPSO.Get());

    PShaderInfo* compSI = &m_shaders.comp;
    if (compSI->CT) {
        ApplyShaderParams(&compSI->params, compSI->CT, m_pState);
        DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(compSI->CT);
        if (ct->GetShadowSize() > 0) {
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
                m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
            if (cbAddr)
                cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        }
    } else {
        BYTE zeros[256] = {};
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
        if (cbAddr)
            cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
    }

    // Bindings = primary Image pass (audio shared). Do not write feedback here —
    // OM is the mirror RT only.
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetCompBindingGpuHandle());

    MYVERTEX v[4];
    ZeroMemory(v, sizeof(v));
    const float cx[4] = { -1.f, 1.f, -1.f, 1.f };
    const float cy[4] = { 1.f, 1.f, -1.f, -1.f };
    const float cu[4] = { 0.f, 1.f, 0.f, 1.f };
    const float cv[4] = { 0.f, 0.f, 1.f, 1.f };
    for (int i = 0; i < 4; i++) {
        v[i].x = cx[i]; v[i].y = cy[i]; v[i].z = 0.f;
        v[i].Diffuse = 0xFFFFFFFFu;
        v[i].tu = cu[i]; v[i].tv = cv[i];
        v[i].tu_orig = cu[i]; v[i].tv_orig = cv[i];
    }
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
}

void Engine::BlitMainToMirror(ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* mainBB, int mainW, int mainH,
                              D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv, int monW, int monH,
                              int scaleMode)
{
    if (!cmdList || !m_lpDX || !mainBB || monW <= 0 || monH <= 0 || mainW <= 0 || mainH <= 0)
        return;

    // Root table is 32 SRVs — reuse one permanent block (allocate once).
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    if (m_mirrorLetterboxSrvBase == UINT_MAX) {
        D3D12_CPU_DESCRIPTOR_HANDLE first = m_lpDX->AllocateSrvCpu();
        m_mirrorLetterboxSrvBase = m_lpDX->m_nextFreeSrvSlot;
        m_lpDX->AllocateSrvGpu();
        for (UINT i = 1; i < DXContext::BINDING_BLOCK_SIZE; i++) {
            m_lpDX->AllocateSrvCpu();
            m_lpDX->AllocateSrvGpu();
        }
        (void)first;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE blockCpu = m_lpDX->GetSrvCpuHandleAt(m_mirrorLetterboxSrvBase);
    for (UINT i = 0; i < DXContext::BINDING_BLOCK_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE slot = blockCpu;
        slot.ptr += (SIZE_T)i * m_lpDX->m_srvDescriptorSize;
        m_lpDX->m_device->CreateShaderResourceView(mainBB, &srvDesc, slot);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE blockGpu =
        m_lpDX->GetBindingBlockGpuHandleByIndex(m_mirrorLetterboxSrvBase);

    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->OMSetRenderTargets(1, &mirrorRtv, FALSE, nullptr);
    cmdList->ClearRenderTargetView(mirrorRtv, black, 0, nullptr);
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    cmdList->SetGraphicsRootDescriptorTable(1, blockGpu);
    BYTE zeros[256] = {};
    const bool workerBlit = m_nMirrorThreadId &&
        GetCurrentThreadId() == m_nMirrorThreadId && m_mirrorWorkUpload && m_mirrorWorkUploadPtr;
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = 0;
    if (workerBlit) {
        memcpy(m_mirrorWorkUploadPtr, zeros, 256);
        cbAddr = m_mirrorWorkUpload->GetGPUVirtualAddress();
    } else {
        cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
    }
    if (cbAddr)
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);

    // scaleMode 0 = stretch (full UV, full NDC)
    // scaleMode 1 = letterbox/fit (shrink NDC, full UV — bars)
    // scaleMode 2 = cover/crop (full NDC, center-crop UV — fills screen)
    float nx = 1.f, ny = 1.f;
    float u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
    const float srcAr = (float)mainW / (float)mainH;
    const float dstAr = (float)monW / (float)monH;
    if (scaleMode == 1) {
        if (srcAr > dstAr)
            ny = dstAr / srcAr; // source wider → letterbox top/bottom
        else
            nx = srcAr / dstAr; // source taller → pillarbox left/right
    } else if (scaleMode == 2) {
        if (srcAr > dstAr) {
            // Source wider than dest — crop left/right, fill height
            const float visible = dstAr / srcAr;
            u0 = (1.f - visible) * 0.5f;
            u1 = 1.f - u0;
        } else {
            // Source taller than dest — crop top/bottom, fill width
            const float visible = srcAr / dstAr;
            v0 = (1.f - visible) * 0.5f;
            v1 = 1.f - v0;
        }
    }

    MYVERTEX v[4];
    ZeroMemory(v, sizeof(v));
    const float px[4] = { -nx, nx, -nx, nx };
    const float py[4] = { ny, ny, -ny, -ny };
    // TRIANGLESTRIP order: TL, TR, BL, BR
    const float pu[4] = { u0, u1, u0, u1 };
    const float pv[4] = { v0, v0, v1, v1 };
    for (int i = 0; i < 4; i++) {
        v[i].x = px[i]; v[i].y = py[i]; v[i].z = 0.f;
        v[i].Diffuse = 0xFFFFFFFFu;
        v[i].tu = pu[i]; v[i].tv = pv[i];
        v[i].tu_orig = pu[i]; v[i].tv_orig = pv[i];
    }
    if (workerBlit) {
        memcpy(m_mirrorWorkUploadPtr + 256, v, sizeof(v));
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = m_mirrorWorkUpload->GetGPUVirtualAddress() + 256;
        vbv.SizeInBytes = sizeof(v);
        vbv.StrideInBytes = sizeof(MYVERTEX);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->DrawInstanced(4, 1, 0, 0);
    } else {
        m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
    }
}

void Engine::ClearMirrorSwapChainAllBuffers(ID3D12GraphicsCommandList* cmdList,
                                            MonitorMirrorState& ms, UINT keepAsRtvIndex)
{
    // Intentionally only clears the current back buffer. Transitioning non-current
    // flip-model buffers (assumed PRESENT while DWM still owns them) TDRs the GPU.
    // Reset painted mask so Present path will block-cycle until every face is redrawn.
    (void)keepAsRtvIndex;
    if (!cmdList || !ms.bHasRtv)
        return;
    float black[] = { 0.f, 0.f, 0.f, 1.f };
    UINT cur = ms.swapChain ? ms.swapChain->GetCurrentBackBufferIndex() : 0;
    if (cur < ms.bufferCount && cur < MIRROR_BUFFER_COUNT && ms.backBuffers[cur])
        cmdList->ClearRenderTargetView(ms.rtvHandles[cur], black, 0, nullptr);
    ms.bNeedsFullChainClear = true;
    ms.paintedBufferMask = 0;
}

void Engine::SetMirrorIndependentRender(bool enable)
{
    if (m_bMirrorIndependentDefault == enable) {
        // Already at requested state — still report so remote clients get a response path
        return;
    }
    m_bMirrorIndependentDefault = enable;
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Monitor)
            out.config.bIndependentRender = m_bMirrorIndependentDefault;
    }
    SaveDisplayOutputSettings();

    // Independent is a DRAW PATH only. Do not destroy swap chains (that froze
    // the UI). Copy mode resizes any leftover native-sized SC to primary size
    // on the render thread (see SendToDisplayOutputs).
    for (auto& out : m_displayOutputs) {
        if (!out.monitorState) continue;
        auto& ms = *out.monitorState;
        ms.bNeedsFullChainClear = true;
        ms.paintedBufferMask = 0;
        ms.lastPath = 0;
        ms.lastDrawFI = UINT_MAX;
        ms.drawCount = 0;
        ms.presentOkCount = 0;
        ms.presentFailCount = 0;
        ms.presentSkipCount = 0;
        for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
            ms.bbState[bi] = ms.bEverPresented
                ? D3D12_RESOURCE_STATE_PRESENT
                : D3D12_RESOURCE_STATE_COMMON;
    }

    if (m_bMirrorIndependentDefault)
        m_bMirrorResetOrientNextFrame.store(true);
    else
        ClearOutputSizeOverride();
    m_bMirrorIndepSizeDirty.store(true);

    // Ensure windows stay visible after mode flip (no hide/recreate)
    m_bRaiseMirrorsNextFrame.store(true);
    m_bMirrorStylesDirty.store(true);
    RefreshDisplaysTab();
    AddNotification(m_bMirrorIndependentDefault
        ? L"Mirrors: independent (own feedback per orientation)"
        : L"Mirrors: copy mode (stretch)");
}

void Engine::ToggleMirrorIndependentRender()
{
    SetMirrorIndependentRender(!m_bMirrorIndependentDefault);
}

// ─── Mirror Window Style Updates ─────────────────────────────────────────────

// Compute desired mirror HWND rect from config (fullscreen = full monitor,
// windowed = centered ~80% of work area, min 640x360).
static void ComputeMirrorLayout(const DisplayOutputConfig& cfg,
                                int& outX, int& outY, int& outW, int& outH)
{
    RECT mon = cfg.rcMonitor;
    // Refresh monitor rect by device name when possible
    struct FindCtx { const wchar_t* name; RECT rc; bool found; };
    FindCtx ctx = { cfg.szDeviceName, mon, false };
    EnumDisplayMonitors(NULL, NULL,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<FindCtx*>(lp);
            MONITORINFOEXW mi = { sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi) &&
                wcscmp(mi.szDevice, c->name) == 0) {
                c->rc = mi.rcMonitor;
                c->found = true;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found)
        mon = ctx.rc;

    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;
    if (cfg.bFullscreen || monW <= 0 || monH <= 0) {
        outX = mon.left;
        outY = mon.top;
        outW = max(1, monW);
        outH = max(1, monH);
        return;
    }
    // Windowed: use work area if available, else 80% of monitor
    RECT work = mon;
    {
        HMONITOR hMon = MonitorFromRect(&mon, MONITOR_DEFAULTTONULL);
        if (hMon) {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi))
                work = mi.rcWork;
        }
    }
    int workW = work.right - work.left;
    int workH = work.bottom - work.top;
    outW = max(640, (workW * 4) / 5);
    outH = max(360, (workH * 4) / 5);
    if (outW > workW) outW = workW;
    if (outH > workH) outH = workH;
    outX = work.left + (workW - outW) / 2;
    outY = work.top + (workH - outH) / 2;
}

void Engine::ApplyMirrorWindowStyles()
{
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.monitorState)
            continue;
        auto& ms = *out.monitorState;
        HWND hWnd = ms.hWnd;
        if (!hWnd) continue;

        // Layered only when needed — flip + WS_EX_LAYERED ghosts other monitors' pixels.
        const bool needLayered = (out.config.nOpacity < 100) || out.config.bClickThrough;
        LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        if (needLayered)
            ex |= WS_EX_LAYERED;
        else
            ex &= ~WS_EX_LAYERED;
        if (out.config.bClickThrough)
            ex |= WS_EX_TRANSPARENT;
        else
            ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, ex);
        if (needLayered) {
            BYTE alpha = (BYTE)(out.config.nOpacity * 255 / 100);
            if (alpha < 3) alpha = 3;
            SetLayeredWindowAttributes(hWnd, 0, alpha, LWA_ALPHA);
        }

        // Fullscreen checkbox: full monitor vs windowed layout
        int x = 0, y = 0, w = 0, h = 0;
        ComputeMirrorLayout(out.config, x, y, w, h);
        if (w != ms.width || h != ms.height) {
            ms.pendingX = x;
            ms.pendingY = y;
            ms.pendingW = w;
            ms.pendingH = h;
            ms.bPendingLayout = true;
        } else {
            const UINT zflags = SWP_NOACTIVATE | SWP_FRAMECHANGED;
            SetWindowPos(hWnd, m_bAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         x, y, w, h, zflags);
        }
    }
}

// ─── Displays Tab Refresh ─────────────────────────────────────────────────────

void Engine::RefreshDisplaysTab()
{
    HWND hWnd = m_displaysWindow ? m_displaysWindow->GetHWND() : NULL;
    if (!hWnd) return;

    HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
    if (!hList) return;

    SendMessage(hList, LB_RESETCONTENT, 0, 0);

    // Resolve primary (render) monitor for clearer list status
    wchar_t renderDevice[32] = {};
    if (m_lpDX && m_lpDX->GetHwnd()) {
        HMONITOR hMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = { sizeof(mi) };
        if (hMon && GetMonitorInfoW(hMon, &mi))
            wcscpy_s(renderDevice, mi.szDevice);
    }

    for (size_t i = 0; i < m_displayOutputs.size(); i++) {
        auto& out = m_displayOutputs[i];
        auto& cfg = out.config;
        wchar_t label[256];
        const wchar_t* prefix = (cfg.type == DisplayOutputType::Monitor) ? L"[Monitor]" : L"[Spout]";
        const wchar_t* status;
        if (cfg.type == DisplayOutputType::Monitor) {
            const bool isPrimary = renderDevice[0] &&
                wcscmp(cfg.szDeviceName, renderDevice) == 0;
            if (isPrimary)
                status = L"PRIMARY (no mirror)";
            else if (!cfg.bEnabled)
                status = L"OFF";
            else if (!m_bMirrorsActive)
                status = L"ON (not active)";
            else if (out.bSkippedSameMonitor)
                status = L"SKIPPED";
            else
                status = L"MIRRORING";
        }
        else {
            status = cfg.bEnabled ? L"ON" : L"OFF";
        }
        swprintf(label, 256, L"%s %s  (%s)", prefix, cfg.szName, status);
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)label);
    }

    // Sync Activate Mirrors button text
    HWND hBtn = GetDlgItem(hWnd, IDC_MW_DISP_ACTIVATE);
    if (hBtn) SetWindowTextW(hBtn, m_bMirrorsActive ? L"Deactivate Mirrors" : L"Activate Mirrors");

}

void Engine::UpdateDisplaysTabSelection(int sel)
{
    HWND hWnd = m_displaysWindow ? m_displaysWindow->GetHWND() : NULL;
    if (!hWnd) return;
    m_nDisplaysTabSel = sel;

    HWND hEnable    = GetDlgItem(hWnd, IDC_MW_DISP_ENABLE);
    HWND hFullscr   = GetDlgItem(hWnd, IDC_MW_DISP_FULLSCREEN);
    HWND hClickThru = GetDlgItem(hWnd, IDC_MW_DISP_CLICKTHRU);
    HWND hOpacity   = GetDlgItem(hWnd, IDC_MW_DISP_OPACITY);
    HWND hOpSpin    = GetDlgItem(hWnd, IDC_MW_DISP_OPACITY_SPIN);
    HWND hName      = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_NAME);
    HWND hFixed     = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_FIXED);
    HWND hW         = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_W);
    HWND hH         = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_H);

    // Helper: sync custom owner-drawn checkbox property + visual state
    auto SetCheckbox = [](HWND hCtrl, bool checked) {
        if (!hCtrl) return;
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
        InvalidateRect(hCtrl, NULL, TRUE);
    };

    if (sel < 0 || sel >= (int)m_displayOutputs.size()) {
        // Nothing selected — clear/disable controls
        if (hEnable)    { SetCheckbox(hEnable, false); EnableWindow(hEnable, FALSE); }
        if (hFullscr)   { SetCheckbox(hFullscr, false); EnableWindow(hFullscr, FALSE); }
        if (hClickThru) { SetCheckbox(hClickThru, false); EnableWindow(hClickThru, FALSE); }
        if (hOpacity)   { SetWindowTextW(hOpacity, L""); EnableWindow(hOpacity, FALSE); }
        if (hOpSpin)    { EnableWindow(hOpSpin, FALSE); }
        if (hName)      { SetWindowTextW(hName, L""); EnableWindow(hName, FALSE); }
        if (hFixed)     { SetCheckbox(hFixed, false); EnableWindow(hFixed, FALSE); }
        if (hW)         { SetWindowTextW(hW, L""); EnableWindow(hW, FALSE); }
        if (hH)         { SetWindowTextW(hH, L""); EnableWindow(hH, FALSE); }
        return;
    }

    auto& cfg = m_displayOutputs[sel].config;
    bool isSpout = (cfg.type == DisplayOutputType::Spout);
    bool isMon = !isSpout;

    // Enable checkbox — always available
    if (hEnable)  { SetCheckbox(hEnable, cfg.bEnabled); EnableWindow(hEnable, TRUE); }

    // Fullscreen — only for monitors
    if (hFullscr) { SetCheckbox(hFullscr, cfg.bFullscreen); EnableWindow(hFullscr, isMon); }

    // Click-through and opacity — only for monitors
    if (hClickThru) { SetCheckbox(hClickThru, cfg.bClickThrough); EnableWindow(hClickThru, isMon); }
    if (hOpacity) {
        wchar_t buf[8];
        swprintf(buf, 8, L"%d", cfg.nOpacity);
        SetWindowTextW(hOpacity, isMon ? buf : L"");
        EnableWindow(hOpacity, isMon);
    }
    if (hOpSpin) {
        if (isMon) SendMessage(hOpSpin, UDM_SETPOS32, 0, cfg.nOpacity);
        EnableWindow(hOpSpin, isMon);
    }

    // Spout-specific fields
    if (hName) { SetWindowTextW(hName, isSpout ? cfg.szName : L""); EnableWindow(hName, isSpout); }
    if (hFixed) { SetCheckbox(hFixed, cfg.bFixedSize); EnableWindow(hFixed, isSpout); }
    wchar_t buf[32];
    if (hW) { swprintf(buf, 32, L"%d", cfg.nWidth); SetWindowTextW(hW, isSpout ? buf : L""); EnableWindow(hW, isSpout); }
    if (hH) { swprintf(buf, 32, L"%d", cfg.nHeight); SetWindowTextW(hH, isSpout ? buf : L""); EnableWindow(hH, isSpout); }
}

// ─── Display Profile Save / Load ─────────────────────────────────────────────

bool Engine::SaveDisplayProfile(const wchar_t* filePath)
{
    JsonWriter w;
    w.BeginObject();
    w.Int(L"version", 1);
    w.Float(L"mainWindowOpacity", fOpacity);
    w.Bool(L"mirrorsActive", m_bMirrorsActive);
    w.Bool(L"mirrorModeForAltS", m_bMirrorModeForAltS);

    w.BeginArray(L"displays");
    for (auto& out : m_displayOutputs) {
        auto& cfg = out.config;
        w.BeginObject();
        w.String(L"type", cfg.type == DisplayOutputType::Monitor ? L"Monitor" : L"Spout");
        w.String(L"name", cfg.szName);
        w.Bool(L"enabled", cfg.bEnabled);

        if (cfg.type == DisplayOutputType::Monitor) {
            w.String(L"deviceName", cfg.szDeviceName);
            w.Bool(L"fullscreen", cfg.bFullscreen);
            w.Int(L"opacity", cfg.nOpacity);
            w.Bool(L"clickThrough", cfg.bClickThrough);
            w.Bool(L"independentRender", cfg.bIndependentRender);
        } else {
            w.Bool(L"fixedSize", cfg.bFixedSize);
            w.Int(L"width", cfg.nWidth);
            w.Int(L"height", cfg.nHeight);
        }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();

    return w.SaveToFile(filePath);
}

bool Engine::LoadDisplayProfile(const wchar_t* filePath)
{
    JsonValue root = JsonLoadFile(filePath);
    if (!root.isObject()) return false;

    int version = root[L"version"].asInt(0);
    if (version < 1) return false;

    // Soft-park only — never Destroy from the UI thread (WaitForGpu + DestroyWindow
    // races with Present and TDRs). Render thread will show/hide from new flags.
    for (auto& out : m_displayOutputs) {
        if (out.monitorState) {
            out.monitorState->bSoftDisabled = true;
            if (out.monitorState->hWnd)
                ShowWindow(out.monitorState->hWnd, SW_HIDE);
        }
    }

    // Main window opacity
    if (root.has(L"mainWindowOpacity")) {
        fOpacity = root[L"mainWindowOpacity"].asFloat(1.0f);
        if (fOpacity < 0.0f) fOpacity = 0.0f;
        if (fOpacity > 1.0f) fOpacity = 1.0f;
        // Apply via message to render window (owns the HWND)
        HWND hw = GetPluginWindow();
        if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
    }

    // Global flags
    m_bMirrorsActive = root[L"mirrorsActive"].asBool(false);
    m_bMirrorModeForAltS = root[L"mirrorModeForAltS"].asBool(false);

    // Apply per-display settings
    const auto& displays = root[L"displays"];
    for (size_t i = 0; i < displays.size(); i++) {
        const auto& d = displays.at(i);
        std::wstring type = d[L"type"].asString(L"");

        if (type == L"Monitor") {
            std::wstring devName = d[L"deviceName"].asString(L"");
            // Match to currently enumerated monitor
            for (auto& out : m_displayOutputs) {
                if (out.config.type != DisplayOutputType::Monitor) continue;
                if (devName != out.config.szDeviceName) continue;
                out.config.bEnabled     = d[L"enabled"].asBool(false);
                out.config.bFullscreen  = d[L"fullscreen"].asBool(true);
                out.config.nOpacity     = d[L"opacity"].asInt(100);
                if (out.config.nOpacity < 1) out.config.nOpacity = 1;
                if (out.config.nOpacity > 100) out.config.nOpacity = 100;
                out.config.bClickThrough = d[L"clickThrough"].asBool(false);
                out.config.bIndependentRender = d[L"independentRender"].asBool(false);
                break;
            }
        } else if (type == L"Spout") {
            std::wstring name = d[L"name"].asString(L"");
            // Try to match existing Spout output by name
            bool matched = false;
            for (auto& out : m_displayOutputs) {
                if (out.config.type != DisplayOutputType::Spout) continue;
                if (name != out.config.szName) continue;
                out.config.bEnabled   = d[L"enabled"].asBool(false);
                out.config.bFixedSize = d[L"fixedSize"].asBool(false);
                out.config.nWidth     = d[L"width"].asInt(1920);
                out.config.nHeight    = d[L"height"].asInt(1080);
                matched = true;
                break;
            }
            // If no existing Spout output matched, add a new one
            if (!matched && !name.empty()) {
                DisplayOutput newOut;
                newOut.config.type      = DisplayOutputType::Spout;
                newOut.config.bEnabled  = d[L"enabled"].asBool(false);
                newOut.config.bFixedSize = d[L"fixedSize"].asBool(false);
                newOut.config.nWidth    = d[L"width"].asInt(1920);
                newOut.config.nHeight   = d[L"height"].asInt(1080);
                wcsncpy_s(newOut.config.szName, name.c_str(), _TRUNCATE);
                m_displayOutputs.insert(m_displayOutputs.begin(), std::move(newOut));
            }
        }
    }

    // Request render-thread mirror style refresh and save to INI
    m_bMirrorStylesDirty.store(true);
    SaveDisplayOutputSettings();
    RefreshDisplaysTab();
    return true;
}

//======================================================================
// Spout output — sender lifecycle and control
//======================================================================

bool Engine::OpenSender(unsigned int width, unsigned int height) {
  SpoutLogNotice("Engine::OpenSender(%d, %d)", width, height);

  // Close existing sender
  SpoutReleaseWraps();
  if (bInitialized) {
    spoutsender.CloseDirectX12();
    bInitialized = false;
  }

  if (!m_lpDX || !m_lpDX->m_device || !m_lpDX->m_commandQueue) {
    DebugLogA("Spout: OpenSender failed - no DX12 device/queue", LOG_ERROR);
    return false;
  }

  // Give the sender a name
  spoutsender.SetSenderName(WinampSenderName);

  // Initialize SpoutDX12 with our DX12 device + command queue
  if (!spoutsender.OpenDirectX12(m_lpDX->m_device.Get(),
          reinterpret_cast<IUnknown**>(m_lpDX->m_commandQueue.GetAddressOf()))) {
    DebugLogA("Spout: OpenDirectX12 failed", LOG_ERROR);
    return false;
  }

  // Wrap each swap chain backbuffer for DX11 access
  for (int n = 0; n < DXC_FRAME_COUNT; n++) {
    if (!spoutsender.WrapDX12Resource(
            m_lpDX->m_renderTargets[n].Get(),
            &m_pWrappedBackBuffers[n],
            D3D12_RESOURCE_STATE_RENDER_TARGET)) {
      DebugLogA("Spout: WrapDX12Resource failed for backbuffer", LOG_ERROR);
      SpoutReleaseWraps();
      spoutsender.CloseDirectX12();
      return false;
    }
  }

  g_Width = width;
  g_Height = height;
  bSpoutOut = true;
  bInitialized = true;
  m_bSpoutDX12Ready = true;

  DebugLogA("Spout: DX12 sender initialized successfully");

  return true;

} // end OpenSender

// Release wrapped DX12 backbuffers
void Engine::SpoutReleaseWraps() {
  for (auto& w : m_pWrappedBackBuffers) {
    if (w) { w->Release(); w = nullptr; }
  }
  m_bSpoutDX12Ready = false;
}

int Engine::ToggleSpout() {
  bSpoutChanged = true; // write config on exit
  bSpoutOut = !bSpoutOut;
  if (bSpoutOut) {
    AddNotification(L"Spout output enabled");
  }
  else {
    AddNotification(L"Spout output disabled");
  }

  // Sync first Spout output in m_displayOutputs
  for (auto& o : m_displayOutputs) {
    if (o.config.type == DisplayOutputType::Spout) {
      o.config.bEnabled = bSpoutOut;
      if (!bSpoutOut && o.spoutState) {
        DestroyDisplayOutput(o);
      }
      break;
    }
  }

  SetSpoutFixedSize(false, false);

  if (bInitialized || m_bSpoutDX12Ready) {
    SpoutReleaseWraps();
    spoutsender.CloseDirectX12();
    bInitialized = false;
  }

  ResetBufferAndFonts();
  SendSettingsInfoToMDropDX12Remote();
  return 0;
}

int Engine::SetSpoutFixedSize(bool toggleSwitch, bool showNotifications) {
  bSpoutChanged = true; // write config on exit
  if (toggleSwitch) {
    bSpoutFixedSize = !bSpoutFixedSize;
  }
  // Sync first Spout output in m_displayOutputs
  for (auto& o : m_displayOutputs) {
    if (o.config.type == DisplayOutputType::Spout) {
      o.config.bFixedSize = bSpoutFixedSize;
      o.config.nWidth = nSpoutFixedWidth;
      o.config.nHeight = nSpoutFixedHeight;
      break;
    }
  }
  if (IsSpoutActiveAndFixed()) {
    if (toggleSwitch && showNotifications) {
      std::wstring msg = L"Fixed Spout output size enabled ("
        + std::to_wstring(nSpoutFixedWidth) + L"x"
        + std::to_wstring(nSpoutFixedHeight) + L")";
      AddNotification(msg.data());
    }
    else if (showNotifications) {
      std::wstring msg = L"Spout output size set to "
        + std::to_wstring(nSpoutFixedWidth) + L"x"
        + std::to_wstring(nSpoutFixedHeight);
      AddNotification(msg.data());
    }
    // DX12 TODO: Fixed-size Spout requires a separate render target + copy/scale.
    // For now, Spout sends at window resolution regardless of fixed-size setting.
    ResetBufferAndFonts();
  }
  else {
    // bSpoutFixedSize OR bSpoutOut is false
    if (toggleSwitch && showNotifications && bSpoutOut) {
      AddNotification(L"Fixed Spout output size disabled");
    }
    ResetBufferAndFonts();
  }
  SendSettingsInfoToMDropDX12Remote();
  return 0;
}

} // namespace mdrop
