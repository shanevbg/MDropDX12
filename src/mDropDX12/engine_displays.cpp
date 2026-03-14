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

namespace mdrop {

// ─── Mirror Window Proc ──────────────────────────────────────────────────────

static LRESULT CALLBACK MirrorWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CLOSE:
        return 0; // Prevent user from closing the mirror window
    case WM_ERASEBKGND:
        return 1; // DX12 handles rendering; skip GDI erase
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
    struct SavedMonitorConfig {
        wchar_t szDeviceName[32];
        RECT rcMonitor;  // Match by display rect (more reliable than device names)
        bool bEnabled;
        bool bFullscreen;
        int nOpacity;
        bool bClickThrough;
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
            saved.push_back(s);
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
                break;
            }
        }
    }
}

// ─── Mirror Activation Failsafe ──────────────────────────────────────────────

Engine::MirrorActivateResult Engine::TryActivateMirrors(HWND hRenderWnd)
{
    // Count monitor outputs
    int totalMonitors = 0;
    int enabledMonitors = 0;
    for (auto& o : m_displayOutputs) {
        if (o.config.type == DisplayOutputType::Monitor) {
            totalMonitors++;
            if (o.config.bEnabled)
                enabledMonitors++;
        }
    }

    // Case 1: Some monitors already enabled — proceed normally
    if (enabledMonitors > 0)
        return MirrorActivated;

    // Case 2: No other monitors at all — fullscreen only
    if (totalMonitors == 0)
        return MirrorFullscreenOnly;

    // Case 3: Monitors exist but none enabled — auto-enable all
    for (auto& o : m_displayOutputs)
        if (o.config.type == DisplayOutputType::Monitor)
            o.config.bEnabled = true;
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

        // Flush GPU so CreateSwapChainForHwnd has a clean command queue
        m_lpDX->WaitForGpu();

        // Drain any pending messages on this thread before creating windows/swap chains.
        // Without this, re-activation after DestroyWindow can deadlock in CreateSwapChainForHwnd
        // because DXGI may SendMessage to the window and stall on unprocessed messages.
        {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
                DispatchMessage(&msg);
        }

        out.monitorState = std::make_unique<MonitorMirrorState>();
        auto& ms = *out.monitorState;

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

        // Create borderless popup window covering the target monitor
        // WS_EX_LAYERED enables SetLayeredWindowAttributes for opacity control.
        // WS_EX_TRANSPARENT (click-through) is added dynamically via ApplyMirrorWindowStyles().
        DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
        if (out.config.bClickThrough)
            exStyle |= WS_EX_TRANSPARENT;
        ms.hWnd = CreateWindowExW(
            exStyle,
            L"MDropDX12_Mirror",
            L"MDropDX12 Mirror",
            WS_POPUP,
            rc.left, rc.top, monW, monH,
            nullptr, nullptr, GetModuleHandle(NULL), nullptr);
        if (!ms.hWnd) {
            DebugLogA("InitDisplayOutput: CreateWindowExW failed for mirror\n", LOG_ERROR);
            out.monitorState.reset();
            return;
        }
        // Apply opacity from per-output settings (1-100% → 3-255)
        BYTE alpha = (BYTE)(out.config.nOpacity * 255 / 100);
        if (alpha < 3) alpha = 3;
        SetLayeredWindowAttributes(ms.hWnd, 0, alpha, LWA_ALPHA);
        SetWindowPos(ms.hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

        // Create swap chain at main backbuffer size; DXGI_SCALING_STRETCH fills the window
        ms.width = m_lpDX->m_client_width;
        ms.height = m_lpDX->m_client_height;

        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Width = (UINT)ms.width;
        scDesc.Height = (UINT)ms.height;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = DXC_FRAME_COUNT;
        scDesc.Scaling = DXGI_SCALING_STRETCH;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

        ComPtr<IDXGISwapChain1> sc1;
        hr = factory->CreateSwapChainForHwnd(
            m_lpDX->m_commandQueue.Get(), ms.hWnd, &scDesc, nullptr, nullptr, &sc1);
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

        // Get back buffers
        for (int i = 0; i < DXC_FRAME_COUNT; i++) {
            hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
            if (FAILED(hr)) {
                char logBuf[256]; sprintf(logBuf, "InitDisplayOutput: GetBuffer(%d) failed\n", i);
                DebugLogA(logBuf, LOG_ERROR);
                ms.swapChain.Reset();
                DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
                out.monitorState.reset();
                return;
            }
        }

        ms.bReady = true;
        {
            char logBuf[256];
            sprintf(logBuf, "InitDisplayOutput: Mirror %ls READY (hwnd=%p, swapchain=%p, %dx%d)\n",
                out.config.szDeviceName, (void*)ms.hWnd, (void*)ms.swapChain.Get(), ms.width, ms.height);
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
        auto& ms = *out.monitorState;
        ms.bReady = false;
        for (int i = 0; i < DXC_FRAME_COUNT; i++)
            ms.backBuffers[i].Reset();
        ms.swapChain.Reset();
        if (ms.hWnd) {
            DestroyWindow(ms.hWnd);
            ms.hWnd = nullptr;
        }
        out.monitorState.reset();
    }
}

void Engine::DestroyAllDisplayOutputs()
{
    for (auto& out : m_displayOutputs)
        DestroyDisplayOutput(out);

    // Release mirror command objects
    m_mirrorCmdList.Reset();
    for (int i = 0; i < DXC_FRAME_COUNT; i++)
        m_mirrorCmdAllocators[i].Reset();
}

void Engine::ResizeMirrorSwapChain(MonitorMirrorState& ms, int newW, int newH)
{
    // Flush GPU before releasing backbuffers — mirror commands may still reference them
    if (m_lpDX) m_lpDX->WaitForGpu();

    // Release back buffer references before resize
    for (int i = 0; i < DXC_FRAME_COUNT; i++)
        ms.backBuffers[i].Reset();

    HRESULT hr = ms.swapChain->ResizeBuffers(
        DXC_FRAME_COUNT, (UINT)newW, (UINT)newH,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        char logBuf[256]; sprintf(logBuf, "ResizeMirrorSwapChain: ResizeBuffers failed (0x%08X)\n", (unsigned)hr);
        DebugLogA(logBuf, LOG_ERROR);
        ms.bReady = false;
        return;
    }

    for (int i = 0; i < DXC_FRAME_COUNT; i++) {
        hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
        if (FAILED(hr)) {
            ms.bReady = false;
            return;
        }
    }
    ms.width = newW;
    ms.height = newH;
}

// ─── Per-Frame Send ───────────────────────────────────────────────────────────

void Engine::SendToDisplayOutputs()
{
    if (!m_lpDX) return;

    // Apply deferred mirror style changes (set by UI thread via m_bMirrorStylesDirty)
    if (m_bMirrorStylesDirty.exchange(false))
        ApplyMirrorWindowStyles();

    // Cleanup: destroy outputs that are disabled or (for monitors) globally deactivated
    // Must flush GPU first — previous frame's mirror commands may still be in flight
    {
        bool needsFlush = false;
        for (auto& out : m_displayOutputs) {
            if (out.config.type == DisplayOutputType::Monitor &&
                (!m_bMirrorsActive || !out.config.bEnabled) && out.monitorState)
                needsFlush = true;
            else if (out.config.type != DisplayOutputType::Monitor &&
                     !out.config.bEnabled && out.spoutState)
                needsFlush = true;
        }
        if (needsFlush && m_lpDX)
            m_lpDX->WaitForGpu();
    }
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Monitor) {
            if ((!m_bMirrorsActive || !out.config.bEnabled) && out.monitorState) {
                DestroyDisplayOutput(out);
            }
            // Always clear skip flag when mirrors deactivate or output disabled —
            // the render window may have moved to a different monitor since the flag was set
            if (!m_bMirrorsActive || !out.config.bEnabled)
                out.bSkippedSameMonitor = false;
        }
        else {
            if (!out.config.bEnabled && out.spoutState)
                DestroyDisplayOutput(out);
        }
    }

    int mainW = m_lpDX->m_client_width;
    int mainH = m_lpDX->m_client_height;
    UINT fi = m_lpDX->m_frameIndex;
    bool hasActiveMonitors = false;

    // Dynamically re-check which monitor the render window is on.
    // If the render window moved, clear stale skip flags and mark the new monitor.
    if (m_bMirrorsActive && m_lpDX->GetHwnd()) {
        // In watermark mode, use the stored target device name — MonitorFromWindow()
        // can return wrong results during window transitions, causing inconsistent skips
        wchar_t renderDevice[32] = {};
        if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
            wcscpy_s(renderDevice, m_szWatermarkRenderDevice);
        } else {
            HMONITOR hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW rmi = { sizeof(rmi) };
            if (hRenderMon && GetMonitorInfoW(hRenderMon, &rmi))
                wcscpy_s(renderDevice, rmi.szDevice);
        }
        bool gotDevice = renderDevice[0] != L'\0';
        RECT renderRect;
        GetWindowRect(m_lpDX->GetHwnd(), &renderRect);
        static wchar_t lastDetected[32] = {};
        if (gotDevice && wcscmp(lastDetected, renderDevice) != 0) {
            DLOG_INFO("Mirror skip: render at (%d,%d)-(%d,%d), device=%ls%ls",
                renderRect.left, renderRect.top, renderRect.right, renderRect.bottom,
                renderDevice, m_bMirrorWatermarkActive ? L" (watermark)" : L"");
            wcscpy_s(lastDetected, renderDevice);
        }
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor || !out.config.bEnabled)
                continue;
            bool isRenderMon = gotDevice && wcscmp(renderDevice, out.config.szDeviceName) == 0;
            if (isRenderMon && !out.bSkippedSameMonitor) {
                // Render window moved TO this monitor — skip it and destroy its mirror
                DLOG_INFO("Mirror skip: marking %ls as render monitor (skipping)", out.config.szDeviceName);
                out.bSkippedSameMonitor = true;
                if (out.monitorState) {
                    m_lpDX->WaitForGpu();
                    DestroyDisplayOutput(out);
                }
            } else if (!isRenderMon && out.bSkippedSameMonitor) {
                // Render window moved AWAY — clear skip so mirror can be created
                DLOG_INFO("Mirror skip: clearing skip for %ls (render moved away)", out.config.szDeviceName);
                out.bSkippedSameMonitor = false;
            }
        }
    }

    // Check if any mirrors need initialization — flush GPU first so
    // CreateSwapChainForHwnd doesn't race with in-flight render commands.
    // Without this, some drivers produce non-functional swap chains.
    if (m_bMirrorsActive) {
        bool needsInit = false;
        for (auto& out : m_displayOutputs) {
            if (out.config.bEnabled &&
                out.config.type == DisplayOutputType::Monitor &&
                !out.bSkippedSameMonitor && !out.monitorState) {
                needsInit = true;
                break;
            }
        }
        if (needsInit)
            m_lpDX->WaitForGpu();
    }

    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled)
            continue;

        if (out.config.type == DisplayOutputType::Spout) {
            if (!out.spoutState || !out.spoutState->bReady) {
                if (!out.spoutState)
                    InitDisplayOutput(out);
                if (!out.spoutState || !out.spoutState->bReady)
                    continue;
            }
            out.spoutState->sender.SendDX11Resource(out.spoutState->wrappedBackBuffers[fi]);
        }
        else if (out.config.type == DisplayOutputType::Monitor) {
            // Skip mirror creation when mirrors are globally deactivated (F9)
            if (!m_bMirrorsActive)
                continue;
            // Skip if on render window's monitor (updated dynamically above)
            if (out.bSkippedSameMonitor)
                continue;
            // Destroy stale mirrors (e.g. Present failed) so they can be re-created
            if (out.monitorState && !out.monitorState->bReady) {
                m_lpDX->WaitForGpu();
                DestroyDisplayOutput(out);
            }
            // Lazy init
            if (!out.monitorState) {
                InitDisplayOutput(out);
            }
            if (!out.monitorState || !out.monitorState->bReady)
                continue;

            // Resize mirror swap chain if main backbuffer size changed
            auto& ms = *out.monitorState;
            if (ms.width != mainW || ms.height != mainH)
                ResizeMirrorSwapChain(ms, mainW, mainH);

            if (ms.bReady)
                hasActiveMonitors = true;
        }
    }

    if (!hasActiveMonitors)
        return;

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

    // Reset and open the mirror command list for this frame
    m_mirrorCmdAllocators[fi]->Reset();
    m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);

    ID3D12Resource* mainBB = m_lpDX->m_renderTargets[fi].Get();

    // Transition main backbuffer PRESENT → COPY_SOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = mainBB;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_mirrorCmdList->ResourceBarrier(1, &barrier);

    // Copy to each active mirror
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady)
            continue;

        auto& ms = *out.monitorState;
        UINT mirrorFI = ms.swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* mirrorBB = ms.backBuffers[mirrorFI].Get();

        // Transition mirror backbuffer PRESENT → COPY_DEST
        D3D12_RESOURCE_BARRIER mirrorBarrier = {};
        mirrorBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        mirrorBarrier.Transition.pResource = mirrorBB;
        mirrorBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        mirrorBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        mirrorBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_mirrorCmdList->ResourceBarrier(1, &mirrorBarrier);

        m_mirrorCmdList->CopyResource(mirrorBB, mainBB);

        // Transition mirror backbuffer COPY_DEST → PRESENT
        mirrorBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        mirrorBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_mirrorCmdList->ResourceBarrier(1, &mirrorBarrier);
    }

    // Transition main backbuffer COPY_SOURCE → PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_mirrorCmdList->ResourceBarrier(1, &barrier);

    m_mirrorCmdList->Close();

    // Execute mirror commands
    ID3D12CommandList* lists[] = { m_mirrorCmdList.Get() };
    m_lpDX->m_commandQueue->ExecuteCommandLists(1, lists);

    // Present each mirror swap chain
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady)
            continue;
        HRESULT hr = out.monitorState->swapChain->Present(0, 0);
        if (FAILED(hr)) {
            char logBuf[256];
            sprintf(logBuf, "Mirror Present failed (0x%08X) on %ls — destroying\n",
                    (unsigned)hr, out.config.szDeviceName);
            DebugLogA(logBuf, LOG_ERROR);
            // Mark as not ready; cleanup will happen next frame
            out.monitorState->bReady = false;
        }
    }
}

// ─── Mirror Window Style Updates ─────────────────────────────────────────────

void Engine::ApplyMirrorWindowStyles()
{
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.monitorState)
            continue;
        HWND hWnd = out.monitorState->hWnd;
        if (!hWnd) continue;

        // Compute alpha from per-output opacity percentage (1-100 → 3-255)
        BYTE alpha = (BYTE)(out.config.nOpacity * 255 / 100);
        if (alpha < 3) alpha = 3;

        LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        if (out.config.bClickThrough)
            ex |= WS_EX_TRANSPARENT;
        else
            ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, ex);
        SetLayeredWindowAttributes(hWnd, 0, alpha, LWA_ALPHA);

        // Mirrors are always topmost — settings window (also topmost) sits above them naturally.
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

    for (size_t i = 0; i < m_displayOutputs.size(); i++) {
        auto& cfg = m_displayOutputs[i].config;
        wchar_t label[256];
        const wchar_t* prefix = (cfg.type == DisplayOutputType::Monitor) ? L"[Monitor]" : L"[Spout]";
        const wchar_t* status;
        if (cfg.type == DisplayOutputType::Monitor) {
            if (!cfg.bEnabled)
                status = L"OFF";
            else if (!m_bMirrorsActive)
                status = L"ON (not active)";
            else
                status = L"ACTIVE";
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

    // Tear down existing mirror windows before applying new settings
    for (auto& out : m_displayOutputs) {
        if (out.monitorState)
            DestroyDisplayOutput(out);
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
