// engine_displays.cpp — Display output management (monitor mirrors + Spout senders)
//
// Part of the MDropDX12 unified display output system.
// Manages enumeration, INI persistence, init/destroy, and per-frame send.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
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
    HMONITOR hRenderMonitor; // exclude the monitor hosting the render window
};

static BOOL CALLBACK EnumMonitorCB(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<EnumMonitorCtx*>(lp);

    MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
    if (!GetMonitorInfoW(hMon, &mi))
        return TRUE;

    // Skip the monitor that hosts the main render window
    if (hMon == ctx->hRenderMonitor)
        return TRUE;

    DisplayOutput out;
    out.config.type = DisplayOutputType::Monitor;
    out.config.bEnabled = false;
    out.config.bFullscreen = true;
    out.config.rcMonitor = mi.rcMonitor;
    wcsncpy_s(out.config.szDeviceName, mi.szDevice, _TRUNCATE);

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

    // Case 3: Monitors exist but none enabled
    if (m_bMirrorPromptDisabled) {
        // Auto-enable all monitors
        for (auto& o : m_displayOutputs)
            if (o.config.type == DisplayOutputType::Monitor)
                o.config.bEnabled = true;
        SaveDisplayOutputSettings();
        return MirrorActivated;
    }

    // Show prompt
    wchar_t msg[256];
    swprintf(msg, 256,
        L"No mirrors enabled.\nFound %d display(s).\n\nMirror to all?",
        totalMonitors);
    int result = MessageBoxW(hRenderWnd, msg, L"MDropDX12",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST);

    if (result == IDYES) {
        for (auto& o : m_displayOutputs)
            if (o.config.type == DisplayOutputType::Monitor)
                o.config.bEnabled = true;
        SaveDisplayOutputSettings();
        return MirrorActivated;
    }

    if (result == IDNO)
        return MirrorFullscreenOnly;

    return MirrorCancelled;
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
            out.config.nOpacity = GetPrivateProfileIntW(section, L"Opacity", legacyOpacity, pIni);
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
            DebugLogA(logBuf);
            return;
        }

        for (int n = 0; n < DXC_FRAME_COUNT; n++) {
            if (!ss.sender.WrapDX12Resource(
                    m_lpDX->m_renderTargets[n].Get(),
                    &ss.wrappedBackBuffers[n],
                    D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                char logBuf[512];
                sprintf(logBuf, "InitDisplayOutput: WrapDX12Resource failed [%d]\n", n);
                DebugLogA(logBuf);
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

        // Safety: don't mirror the monitor hosting the render window
        if (m_lpDX->GetHwnd()) {
            HMONITOR hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
            if (hRenderMon) {
                MONITORINFOEXW mi = { sizeof(mi) };
                if (GetMonitorInfoW(hRenderMon, &mi) &&
                    wcscmp(mi.szDevice, out.config.szDeviceName) == 0) {
                    DebugLogA("InitDisplayOutput: Skipping mirror on render window's monitor\n");
                    out.bSkippedSameMonitor = true;
                    return;
                }
            }
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
        }

        RECT rc = out.config.rcMonitor;
        int monW = rc.right - rc.left;
        int monH = rc.bottom - rc.top;
        {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: Monitor rect = (%d,%d)-(%d,%d) size %dx%d\n",
                (int)rc.left, (int)rc.top, (int)rc.right, (int)rc.bottom, monW, monH);
            DebugLogA(logBuf);
        }

        if (monW <= 0 || monH <= 0) {
            DebugLogA("InitDisplayOutput: Invalid monitor rect, skipping\n");
            out.monitorState.reset();
            return;
        }

        // Create borderless popup window covering the target monitor
        // WS_EX_LAYERED enables SetLayeredWindowAttributes for opacity control.
        // WS_EX_TRANSPARENT (click-through) is added dynamically via UpdateMirrorWindowStyles().
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
            DebugLogA("InitDisplayOutput: CreateWindowExW failed for mirror\n");
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
            DebugLogA("InitDisplayOutput: GetParent(IDXGIFactory4) failed\n");
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
            DebugLogA(logBuf);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }
        factory->MakeWindowAssociation(ms.hWnd, DXGI_MWA_NO_ALT_ENTER);
        hr = sc1.As(&ms.swapChain);
        if (FAILED(hr)) {
            DebugLogA("InitDisplayOutput: QueryInterface IDXGISwapChain4 failed\n");
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }

        // Get back buffers
        for (int i = 0; i < DXC_FRAME_COUNT; i++) {
            hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
            if (FAILED(hr)) {
                char logBuf[256]; sprintf(logBuf, "InitDisplayOutput: GetBuffer(%d) failed\n", i);
                DebugLogA(logBuf);
                ms.swapChain.Reset();
                DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
                out.monitorState.reset();
                return;
            }
        }

        ms.bReady = true;
        DebugLogA("InitDisplayOutput: Mirror window created and ready\n");
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
    // Release back buffer references before resize
    for (int i = 0; i < DXC_FRAME_COUNT; i++)
        ms.backBuffers[i].Reset();

    HRESULT hr = ms.swapChain->ResizeBuffers(
        DXC_FRAME_COUNT, (UINT)newW, (UINT)newH,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        char logBuf[256]; sprintf(logBuf, "ResizeMirrorSwapChain: ResizeBuffers failed (0x%08X)\n", (unsigned)hr);
        DebugLogA(logBuf);
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

    // Cleanup: destroy outputs that are disabled or (for monitors) globally deactivated
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Monitor) {
            if ((!m_bMirrorsActive || !out.config.bEnabled) && out.monitorState) {
                DestroyDisplayOutput(out);
                out.bSkippedSameMonitor = false;  // Re-evaluate on next activation
            }
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
            // Skip if already determined to be on render window's monitor
            if (out.bSkippedSameMonitor)
                continue;
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
                DebugLogA("SendToDisplayOutputs: CreateCommandAllocator failed\n");
                return;
            }
        }
        HRESULT hr = m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_mirrorCmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_mirrorCmdList));
        if (FAILED(hr)) {
            DebugLogA("SendToDisplayOutputs: CreateCommandList failed\n");
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
        out.monitorState->swapChain->Present(0, 0);
    }
}

// ─── Mirror Window Style Updates ─────────────────────────────────────────────

void Engine::UpdateMirrorWindowStyles()
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

} // namespace mdrop
