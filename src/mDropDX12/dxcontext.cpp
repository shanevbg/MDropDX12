/*
  Plugin module: DX12 Device Context
  Device, swap chains, descriptors, uploads and texture creation.
  Contains: Device/swap-chain init + cleanup (Internal_Init,
            StartOrRestartDevice, ResizeSwapChain), the frame loop
            (BeginFrame, EndFrame, WaitForGpu, MoveToNextFrame),
            descriptor allocation (RTV/SRV heaps, mirror RTV blocks),
            retired-object collection + texture recycling, GPU timing
            and timestamps, DRED dump, root signature + upload rings,
            draw + constant upload (DrawVertices, UploadConstantBuffer),
            texture creation/loading (CreateTextureFromPixels, volume
            textures, LoadTextureFromFile, DDS/TGA decode, fallback/null
            textures), SRV binding blocks + per-frame bindings
*/

/*
LICENSE
-------
Copyright 2005-2013 Nullsoft, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

* Neither the name of Nullsoft nor the names of its contributors may be used to
endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "DXContext.h"
#include <dbghelp.h>   // #222: resolve each draw's return address to file:line
#include <intrin.h>    // _ReturnAddress
#include "utility.h"
#include "shell_defines.h"
#include "format_to.h"
#include <strsafe.h>
#include <cassert>
#include <cstdio>
#include <atomic>
#include <wincodec.h>  // WIC for LoadTextureFromFile
#include <vector>
#include "config_store.h"

using mdrop::Config;
using mdrop::ConfigFile;

// Set by App.cpp on WM_POWERBROADCAST suspend — skip GPU fence waits while asleep.
extern std::atomic<bool> g_bPowerSuspended;
// Set by App.cpp on TDR — Signal/wait would stall and freeze shutdown.
extern std::atomic<bool> g_bGpuHung;

// Swap chain back-buffer pixel format
static const DXGI_FORMAT k_BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

DXContext::DXContext(
    ID3D12Device*        device,
    ID3D12CommandQueue*  commandQueue,
    IDXGIFactory4*       factory,
    HWND                 hwnd,
    int                  width,
    int                  height,
    wchar_t*             szIniFile,
    int                  fallbackTexStyle)
{
    m_nFallbackTexStyle = fallbackTexStyle;
    m_szWindowCaption[0] = 0;
    m_hwnd               = hwnd;
    m_truly_exiting      = 0;
    m_bpp                = 32;
    m_frame_delay        = 0;
    m_ready              = FALSE;
    m_lastErr            = S_OK;
    m_nPresentFailCount  = 0;
    m_frameIndex         = 0;
    m_fenceEvent         = nullptr;
    m_rtvDescriptorSize     = 0;
    m_srvDescriptorSize     = 0;
    m_samplerDescriptorSize = 0;
    m_nextFreeRtvSlot       = DXC_FRAME_COUNT;  // slots 0..1 reserved for back buffers
    m_nextFreeSrvSlot       = 0;

    for (int i = 0; i < DXC_FRAME_COUNT; i++)
        m_fenceValues[i] = 0;

    ZeroMemory(&m_current_mode, sizeof(m_current_mode));

    StringCbCopyW(m_szIniFile, sizeof(m_szIniFile), szIniFile);

    // Read fallback texture paths from INI for styles 3-5
    ConfigFile(m_szIniFile).GetStringTo(L"FallbackPaths", L"RandomTexDir", L"", m_szFallbackRandomTexDir, MAX_PATH);
    ConfigFile(m_szIniFile).GetStringTo(L"FallbackPaths", L"FallbackTexFile", L"", m_szFallbackCustomFile, MAX_PATH);
    // Construct textures dir from exe path (m_szIniFile is in the base dir)
    {
        wchar_t szDir[MAX_PATH];
        CopyTo(szDir, m_szIniFile);
        wchar_t* lastSlash = wcsrchr(szDir, L'\\');
        if (lastSlash) lastSlash[1] = L'\0';
        swprintf(m_szFallbackTexturesDir, MAX_PATH, L"%sresources\\textures\\", szDir);
    }

    // Read anisotropic filtering setting from INI (needed before CreateRootSignature in Internal_Init)
    m_bAnisotropicFiltering = ConfigFile(m_szIniFile).GetInt(L"Settings", L"bAnisotropicFiltering", 0) != 0;

    // Store device references passed from the initializer
    m_device       = device;
    m_commandQueue = commandQueue;

#ifdef _DEBUG
    // Only succeeds when the debug layer was enabled before device creation
    // (App.cpp's InitD3d, also _DEBUG-gated) -- forgejo#16. A silent no-op
    // otherwise: m_infoQueue stays null and DrainDebugMessages() does nothing.
    if (SUCCEEDED(m_device.As(&m_infoQueue)) && m_infoQueue) {
        DebugLogA("DX12: debug layer active, InfoQueue attached", LOG_INFO);
    }
#endif

    // Create swap chain, RTVs, command allocators, command list, fence
    if (!Internal_Init(factory, hwnd, width, height)) {
        m_lastErr = DXC_ERR_CREATE3D;
        return;
    }

    m_client_width       = m_REAL_client_width  = width;
    m_client_height      = m_REAL_client_height = height;
    m_window_width       = width;
    m_window_height      = height;
    m_backbuffer_width   = width;
    m_backbuffer_height  = height;
    m_ready              = TRUE;

    {
        char buf[256];
        sprintf(buf, "DX12: DXContext init: swap chain %dx%d, client %dx%d\n",
                width, height, m_client_width, m_client_height);
        DebugLogA(buf);
    }
}

DXContext::~DXContext()
{
    Internal_CleanUp();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

bool DXContext::Internal_Init(IDXGIFactory4* factory, HWND hwnd, int width, int height)
{
    HRESULT hr;

    // 1. RTV descriptor heap (expanded: back buffers + render target textures)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = DXC_MAX_RTV;
        rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
        if (FAILED(hr)) return false;
        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // 1b. SRV/CBV/UAV descriptor heap (shader-visible, for texture SRVs and CBVs)
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = DXC_MAX_SRV;
        srvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
        if (FAILED(hr)) return false;
        m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // 1b-cpu. A second, NON-shader-visible mirror of m_srvHeap, indexed
    // identically (forgejo#16). A shader-visible heap's CPU descriptor
    // handles are write-only -- CreateShaderResourceView into one is the
    // documented, correct way to populate it, but reading one back (as the
    // source of CopyDescriptorsSimple, which is exactly what every binding-
    // block/binding-refresh helper below does to assemble a per-draw
    // descriptor table from an already-loaded texture's slot) is invalid
    // D3D12 usage. With no debug layer running that produced silent
    // corruption instead of an error; WITH it running (this build), it was
    // ~80,000 "SrcDescriptorRangeStart points to a descriptor heap type that
    // is CPU write only" errors in an 8-second test, almost entirely from
    // the two per-frame binding-refresh paths (UpdatePerFrameBindings,
    // UpdateBlurPassBinding) that run every frame. Every write that creates
    // an SRV in m_srvHeap at index N (see CreateSRV2D) now ALSO creates the
    // identical view in m_srvHeapCpu at index N; every CopyDescriptorsSimple
    // that used to source from m_srvHeap now sources from m_srvHeapCpu
    // instead. m_srvHeap remains the only heap ever bound for rendering
    // (SetDescriptorHeaps) -- this one exists purely to be read from.
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapCpuDesc = {};
        srvHeapCpuDesc.NumDescriptors = DXC_MAX_SRV;
        srvHeapCpuDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapCpuDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = m_device->CreateDescriptorHeap(&srvHeapCpuDesc, IID_PPV_ARGS(&m_srvHeapCpu));
        if (FAILED(hr)) return false;
    }

    // 1c. Sampler descriptor heap (shader-visible)
    {
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
        samplerHeapDesc.NumDescriptors = DXC_MAX_SAMPLERS;
        samplerHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = m_device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&m_samplerHeap));
        if (FAILED(hr)) return false;
        m_samplerDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }

    // 2. Check tearing support
    {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
            BOOL allowTearing = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
                m_tearingSupported = (allowTearing != FALSE);
        }
    }

    // 3. Swap chain
    {
        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Width              = (UINT)width;
        scDesc.Height             = (UINT)height;
        scDesc.Format             = k_BackBufferFormat;
        scDesc.Stereo             = FALSE;
        scDesc.SampleDesc.Count   = 1;
        scDesc.SampleDesc.Quality = 0;
        scDesc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount        = DXC_FRAME_COUNT;
        scDesc.Scaling            = DXGI_SCALING_STRETCH;
        scDesc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;
        scDesc.Flags              = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        // No fullscreen desc, and this is a decision rather than a gap: DXGI
        // EXCLUSIVE fullscreen is unavailable to this app by design, because it
        // takes the display away from DWM.
        //
        // Window transparency (WS_EX_LAYERED alpha, i.e. watermark mode and the
        // per-display Opacity setting) and click-through (WS_EX_TRANSPARENT) are
        // DWM composition features. On a display DWM does not own, all of them
        // stop working. Wiring up SetFullscreenState would therefore delete
        // three shipped features on whichever display used it.
        //
        // The narrower form of this conflict has already been met and measured.
        // InitDisplayOutput (engine_displays.cpp, the "Do NOT use WS_EX_LAYERED
        // when opacity is 100%" comment) records that flip-model swap chains
        // plus layered windows let DWM composite other monitors' content
        // through a mirror -- a ghost strip, often flickering -- which is why
        // layered is applied ONLY when opacity < 100 or click-through is on.
        // Exclusive fullscreen is the extreme end of the same incompatibility.
        //
        // It is also better for frame rate NOT to have it. Windows classifies a
        // process's QoS from window state, and an exclusive-fullscreen window
        // that loses focus reads as fully occluded -> Low, where this borderless
        // DWM-composed window reads as visible-but-not-focused -> Medium. See
        // #121; the previous wording here, "windowed only for now", invited
        // exactly the change that would have made both problems worse.
        ComPtr<IDXGISwapChain1> swapChain1;
        hr = factory->CreateSwapChainForHwnd(
            m_commandQueue.Get(),
            hwnd,
            &scDesc,
            nullptr,  // fullscreen desc: never, see above
            nullptr,
            &swapChain1);
        if (FAILED(hr)) return false;

        // Disable Alt+Enter fullscreen toggle (handled by the app)
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        hr = swapChain1.As(&m_swapChain);
        if (FAILED(hr)) return false;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // 3. RTVs for each back buffer
    CreateRtvsForSwapChain();

    // 4. Per-frame command allocators
    for (UINT i = 0; i < DXC_FRAME_COUNT; i++) {
        hr = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i]));
        if (FAILED(hr)) return false;
    }

    // 5. Command list (initially closed after creation)
    hr = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[m_frameIndex].Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList));
    // Named so DRED can say WHICH list hung — an unnamed breadcrumb node only
    // tells you a draw stalled, not whose draw it was.
    if (m_commandList)
        m_commandList->SetName(L"PrimaryCmdList");
    if (FAILED(hr)) return false;
    m_commandList->Close();
    m_cmdListRecording = false;

    // 5b. Dedicated upload allocator + command list + fence for texture creation
    hr = m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_uploadAllocator));
    if (FAILED(hr)) return false;

    hr = m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_uploadAllocator.Get(), nullptr,
        IID_PPV_ARGS(&m_uploadCommandList));
    if (FAILED(hr)) return false;
    m_uploadCommandList->Close();

    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_uploadFence));
    if (FAILED(hr)) return false;
    m_uploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_uploadFenceEvent) return false;

    // 6. Fence + event for CPU/GPU synchronisation
    {
        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        if (FAILED(hr)) return false;
        m_fenceValues[m_frameIndex] = 1;

        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent) return false;
    }

    // 7. Root signature (Phase 3)
    if (!CreateRootSignature()) return false;

    // 8. Upload heap buffers (Phase 3)
    if (!CreateUploadBuffers()) return false;

    // 9. Pipeline state objects (Phase 3)
    if (!CreatePipelines()) return false;

    // 10. Null texture for filling unused SRV slots (Phase 5)
    if (!CreateNullTexture(m_nFallbackTexStyle)) return false;

    // 11. Per-frame binding block ranges (Phase 5)
    if (!AllocatePerFrameBindings()) return false;

    // 11b. Per-frame blur binding block ranges
    if (!AllocateBlurBindings()) return false;

    // 12. Populate monitor geometry
    UpdateMonitorWorkRect();

    // 13. Snapshot allocator positions — everything above is permanent.
    //     Dynamic textures (VS, blur) allocated in AllocateMyDX9Stuff
    //     will be reclaimed on resize/toggle by resetting to these baselines.
    m_rtvSlotBaseline = m_nextFreeRtvSlot;
    m_srvSlotBaseline = m_nextFreeSrvSlot;

    // DXContext-level baselines (before font atlas / help texture).
    // EngineShell::AllocateDX9Stuff() will advance m_srvSlotBaseline past fonts;
    // these let ResetBufferAndFonts() rewind all the way back to reclaim font atlas slots.
    m_rtvSlotInitBaseline = m_nextFreeRtvSlot;
    m_srvSlotInitBaseline = m_nextFreeSrvSlot;

    return true;
}

void DXContext::CreateRtvsForSwapChain()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < DXC_FRAME_COUNT; i++) {
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (SUCCEEDED(hr)) {
            m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        }
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

void DXContext::ReleaseSwapChainRtvs()
{
    for (UINT i = 0; i < DXC_FRAME_COUNT; i++)
        m_renderTargets[i].Reset();
}

void DXContext::Internal_CleanUp()
{
    if (m_ready) {
        WaitForGpu();
        m_ready = FALSE;
    }

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    if (m_uploadFenceEvent) {
        CloseHandle(m_uploadFenceEvent);
        m_uploadFenceEvent = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

bool DXContext::BeginFrame()
{
    if (!m_ready) return false;

    // Suspended (sleep/hibernate): skip all GPU work until resume recovery.
    if (g_bPowerSuspended.load())
        return false;

    // One-time dimension sanity check: verify swap chain matches stored dimensions
    static bool s_checkedOnce = false;
    if (!s_checkedOnce && m_swapChain && m_hwnd) {
        s_checkedOnce = true;
        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        m_swapChain->GetDesc1(&scDesc);
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        int clientW = rc.right - rc.left;
        int clientH = rc.bottom - rc.top;
        char buf[512];
        sprintf(buf, "DX12: BeginFrame check: swapchain=%ux%u, m_client=%dx%d, actual_client=%dx%d\n",
                scDesc.Width, scDesc.Height, m_client_width, m_client_height, clientW, clientH);
        DebugLogA(buf);
    }

    // Reset command allocator and command list for this frame.
    //
    // BOTH failures below record m_lastErr, because that is the only thing that
    // arms device recovery: EngineShell::PluginRender tests m_lastErr != S_OK
    // and sets m_bDeviceRecoveryPending. Present() sets it, but BeginFrame runs
    // FIRST and returns false, so on a dead device Present is never reached and
    // the Present hook can never fire.
    //
    // Without this the render loop spins here forever -- one failed allocator
    // reset and one identical log line per frame, at 60Hz, with the app still
    // fully responsive because only the GPU device is gone. That is exactly
    // what a DEVICE_HUNG left behind: "render dead, tool windows still work".
    HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
    if (FAILED(hr)) {
        // Prefer the removal reason: E_FAIL from Reset says the allocator is
        // still in use OR the device is gone, and the two want the same
        // response but read very differently in a log.
        const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : S_OK;
        char buf[256];
        sprintf(buf, "DX12: BeginFrame: CommandAllocator[%u]->Reset() FAILED hr=0x%08X removedReason=0x%08X",
                m_frameIndex, (unsigned)hr, (unsigned)reason);
        DebugLogA(buf, LOG_ERROR);
        m_lastErr = (reason != S_OK) ? reason : hr;
        return false;
    }

    hr = m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);
    m_cmdListRecording = SUCCEEDED(hr);
    if (SUCCEEDED(hr)) {
        // Read LAST time this frame index ran, before the counter is reset --
        // MoveToNextFrame has already waited for that fence, so the numbers are
        // there and mapping them costs nothing (#230).
        ReportGpuTimestamps();
        ReadFrameTimestamp();     // #245: the whole-frame pair, same fence rule
        m_tsCount = 0;
        ResetStageMap();          // #222: a new frame's worth of draws
        CollectRetiredObjects();  // #214: release what the GPU has finished with
        BeginFrameTimestamp();    // first thing recorded, so it covers the frame
    }
    if (FAILED(hr)) {
        const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : S_OK;
        char buf[256];
        sprintf(buf, "DX12: BeginFrame: CommandList->Reset() FAILED hr=0x%08X removedReason=0x%08X",
                (unsigned)hr, (unsigned)reason);
        DebugLogA(buf, LOG_ERROR);
        m_lastErr = (reason != S_OK) ? reason : hr;
        return false;
    }

    // Transition back buffer: PRESENT → RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // Set back buffer as the render target
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (SIZE_T)m_frameIndex * m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Reset upload buffer for this frame
    ResetUploadBuffer();

    return true;
}

void DXContext::ExecuteCommandList()
{
    if (!m_ready) return;

    // Transition back buffer RENDER_TARGET → PRESENT before closing the command list.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // Last thing recorded, so it covers every stage (#230).
    ResolveGpuTimestamps();
    EndFrameTimestamp();      // #245: closes the whole-frame pair and resolves it

    // Close and execute the DX12 command list.
    HRESULT hrClose = m_commandList->Close();
    m_cmdListRecording = false;
    if (FAILED(hrClose)) {
        char buf[256];
        sprintf(buf, "DX12: ExecuteCommandList: Close FAILED hr=0x%08X devRemoved=0x%08X",
                (unsigned)hrClose, (unsigned)m_device->GetDeviceRemovedReason());
        DebugLogA(buf, LOG_ERROR);
        if (m_device->GetDeviceRemovedReason() != S_OK)
            DumpDredToLog("ExecuteCommandList/Close");
    }

    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
}

// forgejo#16: the debug layer validates as work is recorded/submitted, not at
// Present, but draining once a frame (rather than after every single DX12
// call) is enough to attribute a message to "this frame" for diagnosis
// without the overhead of checking after each individual command.
void DXContext::DrainDebugMessages()
{
    if (!m_infoQueue) return;

    const UINT64 n = m_infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        if (FAILED(m_infoQueue->GetMessage(i, nullptr, &len)) || len == 0)
            continue;
        std::vector<BYTE> storage(len);
        D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(m_infoQueue->GetMessage(i, msg, &len)))
            continue;

        int level;
        switch (msg->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:   level = LOG_ERROR; break;
            case D3D12_MESSAGE_SEVERITY_WARNING: level = LOG_WARN;  break;
            case D3D12_MESSAGE_SEVERITY_INFO:    level = LOG_VERBOSE; break;
            default:                             level = LOG_VERBOSE; break;
        }
        char buf[1200];
        FormatToA(buf, "DX12 %s [%d]: %.*s",
                  (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) ? "CORRUPTION"
                  : (msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR)    ? "ERROR"
                  : (msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING)  ? "WARNING" : "INFO",
                  (int)msg->ID, (int)msg->DescriptionByteLength, msg->pDescription);
        DebugLogA(buf, level);
    }
    m_infoQueue->ClearStoredMessages();
}

void DXContext::EndFrame()
{
    if (!m_ready) return;

    DrainDebugMessages();

    // Suspended (sleep/hibernate): never Present on the pre-sleep device.
    if (g_bPowerSuspended.load())
        return;

    // Present: respect VSync setting; use tearing flag when VSync is off.
    // Multi-monitor mirrors force vsync + no tearing so primary and mirrors do not
    // show different frames (cross-monitor "ghost" when focused / high FPS).
    bool vsync = (m_pVSync ? *m_pVSync : true) || m_bSerializeWithMirrors;
    UINT syncInterval = vsync ? 1 : 0;
    UINT presentFlags = (!vsync && m_tearingSupported && !m_bSerializeWithMirrors)
        ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hrPresent = m_swapChain->Present(syncInterval, presentFlags);

    // If Present failed with ALLOW_TEARING, retry without it — the flag can
    // return E_FAIL when the window is in certain states after mode switches.
    if (FAILED(hrPresent) && presentFlags != 0) {
        hrPresent = m_swapChain->Present(syncInterval, 0);
        if (SUCCEEDED(hrPresent)) {
            if (m_nPresentFailCount > 0) {
                DebugLogA("DX12: Present recovered after dropping ALLOW_TEARING flag", LOG_ERROR);
            }
            m_nPresentFailCount = 0;
            MoveToNextFrame();
            return;
        }
    }

    if (FAILED(hrPresent)) {
        m_nPresentFailCount++;
        if (m_nPresentFailCount <= 3 || (m_nPresentFailCount % 100 == 0)) {
            char buf[256];
            sprintf(buf, "DX12: Present FAILED hr=0x%08X (consecutive=%d)", (unsigned)hrPresent, m_nPresentFailCount);
            DebugLogA(buf, LOG_ERROR);
        }
        if (hrPresent == DXGI_ERROR_DEVICE_REMOVED || hrPresent == DXGI_ERROR_DEVICE_RESET) {
            HRESULT reason = m_device->GetDeviceRemovedReason();
            char buf[256];
            sprintf(buf, "DX12: Device removed reason: 0x%08X (TDR — GPU timeout or driver crash)", (unsigned)reason);
            DebugLogA(buf, LOG_ERROR);
            DumpDredToLog("Present");
            m_lastErr = hrPresent;
            m_ready = 0;
            m_nPresentFailCount = 0;
            return;
        }

        // After many consecutive failures, the swap chain is stuck.
        // Force a resize to the current client rect to reset it.
        if (m_nPresentFailCount >= 10 && m_hwnd) {
            DebugLogA("DX12: Present stuck — forcing swap chain resize to recover", LOG_ERROR);
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                // Force resize even if dimensions match by going through the full path
                WaitForGpu();
                ReleaseSwapChainRtvs();
                m_commandList.Reset();
                for (UINT i = 0; i < DXC_FRAME_COUNT; i++) {
                    m_commandAllocators[i].Reset();
                    m_fenceValues[i] = m_fenceValues[m_frameIndex];
                }
                UINT swapFlags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
                HRESULT hr = m_swapChain->ResizeBuffers(DXC_FRAME_COUNT, (UINT)w, (UINT)h, k_BackBufferFormat, swapFlags);
                if (SUCCEEDED(hr)) {
                    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
                    m_client_width = m_REAL_client_width = w;
                    m_client_height = m_REAL_client_height = h;
                    m_window_width = w;
                    m_window_height = h;
                    CreateRtvsForSwapChain();
                    for (UINT i = 0; i < DXC_FRAME_COUNT; i++) {
                        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]));
                    }
                    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        m_commandAllocators[m_frameIndex].Get(), nullptr, IID_PPV_ARGS(&m_commandList));
                    if (m_commandList)
                        m_commandList->SetName(L"PrimaryCmdList");   // DRED attribution
                    m_commandList->Close();
                    m_cmdListRecording = false;
                    SetViewport(w, h);
                    DebugLogA("DX12: Swap chain reset succeeded — resuming", LOG_ERROR);
                    m_nPresentFailCount = 0;
                } else {
                    char buf[256];
                    sprintf(buf, "DX12: Recovery ResizeBuffers FAILED hr=0x%08X", (unsigned)hr);
                    DebugLogA(buf, LOG_ERROR);
                    m_lastErr = hr;
                    m_ready = 0;
                    m_nPresentFailCount = 0;
                }
            }
            return;
        }

        // For the first few failures, just skip MoveToNextFrame and retry next frame.
        // Don't call WaitForGpu every frame — it's expensive.
        return;
    }

    m_nPresentFailCount = 0;
    MoveToNextFrame();
}

// ---------------------------------------------------------------------------
// Synchronisation
// ---------------------------------------------------------------------------

void DXContext::EnsureMultiSwapChainFrameLatency(int presentOpsPerFrame, bool serializeFrames)
{
    if (!m_device)
        return;
    // Latency is device-wide across all flip-model swap chains.
    // Default 2. When opposite-orient independent mirrors are active, use 1:
    // at high FPS (window focused) latency≥2 allowed a stale flip face to remain
    // on-screen every other frame (landscape strip on portrait). When the OS
    // backgrounds the app (~22 FPS) that queue never builds up and ghost vanishes.
    UINT want = serializeFrames ? 1u : 2u;

    static UINT s_lastSet = 0;
    if (s_lastSet == want)
        return;

    ComPtr<IDXGIDevice1> dxgiDev;
    if (FAILED(m_device.As(&dxgiDev)) || !dxgiDev)
        return;
    if (SUCCEEDED(dxgiDev->SetMaximumFrameLatency(want))) {
        s_lastSet = want;
        char buf[128];
        sprintf(buf, "DX12: SetMaximumFrameLatency=%u (presents/frame≈%d serialize=%d)\n",
                want, presentOpsPerFrame, serializeFrames ? 1 : 0);
        DebugLogA(buf, LOG_INFO);
    }
}

void DXContext::WaitForGpu()
{
    // After sleep/hibernate the device may be dead — don't block on fences.
    if (g_bPowerSuspended.load() || g_bGpuHung.load())
        return;
    if (m_device) {
        HRESULT gone = m_device->GetDeviceRemovedReason();
        if (gone != S_OK) {
            DumpDredToLog("WaitForGpu");
            return; // TDR — Signal/wait would stall 5s and freeze the UI
        }
    }

    // Every OTHER queue this process submits to, first -- see the note on
    // m_waitForAuxQueues. This runs before the main-queue wait so that a
    // caller which is about to free a GPU object is covered even when the main
    // queue has nothing outstanding, and so that the missing-objects early
    // return below cannot skip it (#206).
    if (m_waitForAuxQueues)
        m_waitForAuxQueues();

    if (!m_commandQueue || !m_fence || !m_fenceEvent) return;

    // Signal a fence value from the GPU
    UINT64 fenceValue = m_fenceValues[m_frameIndex];
    m_commandQueue->Signal(m_fence.Get(), fenceValue);

    // Wait for the GPU to reach that value (5s timeout)
    if (m_fence->GetCompletedValue() < fenceValue) {
        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        DWORD result = WaitForSingleObjectEx(m_fenceEvent, 5000, FALSE);
        if (result == WAIT_TIMEOUT) {
            DebugLogA("DX12: WaitForGpu TIMEOUT (5s) — possible GPU hang", LOG_ERROR);
            HRESULT reason = m_device->GetDeviceRemovedReason();
            if (reason != S_OK) {
                char buf[256];
                sprintf(buf, "DX12: Device removed reason: 0x%08X", (unsigned)reason);
                DebugLogA(buf, LOG_ERROR);
                DumpDredToLog("WaitForGpu/timeout");
            }
        }
    }

    // Advance the fence value for the current frame
    m_fenceValues[m_frameIndex]++;
}

void DXContext::MoveToNextFrame()
{
    // Signal the current fence value
    const UINT64 currentFenceValue = m_fenceValues[m_frameIndex];
    m_commandQueue->Signal(m_fence.Get(), currentFenceValue);

    // Advance frame index
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Wait if the next frame is still in use by the GPU (5s timeout to detect TDR)
    UINT64 waitTarget = m_fenceValues[m_frameIndex];
    if (m_fence->GetCompletedValue() < waitTarget) {
        m_fence->SetEventOnCompletion(waitTarget, m_fenceEvent);
        DWORD result = WaitForSingleObjectEx(m_fenceEvent, 5000, FALSE);
        if (result == WAIT_TIMEOUT) {
            HRESULT reason = m_device->GetDeviceRemovedReason();
            char buf[256];
            sprintf(buf, "DX12: MoveToNextFrame TIMEOUT (5s) target=%llu reason=0x%08X",
                    (unsigned long long)waitTarget, (unsigned)reason);
            DebugLogA(buf, LOG_ERROR);
            if (reason != S_OK)
                DumpDredToLog("MoveToNextFrame/timeout");
        }
    }

    // Set fence value for the upcoming frame
    m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

// ---------------------------------------------------------------------------
// Swap chain resize
// ---------------------------------------------------------------------------

bool DXContext::ResizeSwapChain(int newWidth, int newHeight)
{
    if (!m_ready) return false;

    // Compare against actual swap chain buffer dimensions, not tracked m_client_width/height.
    // OnUserResizeWindow(bSetBackBuffer=false) may have already updated m_client_width/height
    // before this call, which would cause a false match and skip the actual resize.
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    if (m_swapChain) m_swapChain->GetDesc1(&scDesc);
    if ((int)scDesc.Width == newWidth && (int)scDesc.Height == newHeight) {
        m_client_width  = m_REAL_client_width  = newWidth;
        m_client_height = m_REAL_client_height = newHeight;
        return true;
    }

    {
        char buf[256];
        sprintf(buf, "DX12: ResizeSwapChain: swap=%ux%u -> %dx%d\n",
                scDesc.Width, scDesc.Height, newWidth, newHeight);
        DebugLogA(buf);
    }

    // Flush GPU before resizing
    WaitForGpu();

    // Release references to back-buffer resources
    ReleaseSwapChainRtvs();
    m_commandList.Reset();
    for (UINT i = 0; i < DXC_FRAME_COUNT; i++) {
        m_commandAllocators[i].Reset();
        m_fenceValues[i] = m_fenceValues[m_frameIndex];
    }

    UINT swapFlags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = m_swapChain->ResizeBuffers(
        DXC_FRAME_COUNT,
        (UINT)newWidth,
        (UINT)newHeight,
        k_BackBufferFormat,
        swapFlags);
    if (FAILED(hr)) {
        HRESULT devReason = m_device->GetDeviceRemovedReason();
        char buf[256];
        sprintf(buf, "DX12: ResizeBuffers FAILED hr=0x%08X deviceRemovedReason=0x%08X",
                (unsigned)hr, (unsigned)devReason);
        DebugLogA(buf, LOG_ERROR);
        if (devReason != S_OK)
            DumpDredToLog("ResizeBuffers");

        // Restore command infrastructure so the render loop doesn't crash
        // accessing null objects while recovery is pending.
        CreateRtvsForSwapChain();
        for (UINT i = 0; i < DXC_FRAME_COUNT; i++)
            m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]));
        m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_commandAllocators[m_frameIndex].Get(), nullptr, IID_PPV_ARGS(&m_commandList));
        if (m_commandList) m_commandList->Close();
        m_cmdListRecording = false;

        m_lastErr = DXC_ERR_RESIZEFAILED;
        return false;
    }

    m_frameIndex    = m_swapChain->GetCurrentBackBufferIndex();
    m_client_width  = m_REAL_client_width  = newWidth;
    m_client_height = m_REAL_client_height = newHeight;
    m_window_width  = newWidth;
    m_window_height = newHeight;

    // Recreate RTVs, command allocators, and command list
    CreateRtvsForSwapChain();

    for (UINT i = 0; i < DXC_FRAME_COUNT; i++) {
        hr = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i]));
        if (FAILED(hr)) return false;
    }

    hr = m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[m_frameIndex].Get(),
        nullptr, IID_PPV_ARGS(&m_commandList));
    if (FAILED(hr)) return false;
    m_commandList->Close();
    m_cmdListRecording = false;

    SetViewport(newWidth, newHeight);
    UpdateMonitorWorkRect();
    return true;
}

// ---------------------------------------------------------------------------
// Miscellaneous
// ---------------------------------------------------------------------------

BOOL DXContext::StartOrRestartDevice(DXCONTEXT_PARAMS* pParams)
{
    memcpy(&m_current_mode, pParams, sizeof(DXCONTEXT_PARAMS));
    m_ready = TRUE;
    return TRUE;
}

void DXContext::SetViewport(int width, int height)
{
    // Viewport is set per-draw-call on the command list in DX12, not on the device.
    // This is a no-op here; callers set it on m_commandList directly.
    (void)width; (void)height;
}

HWND DXContext::GetHwnd()              { return m_hwnd; }
bool DXContext::TempIgnoreDestroyMessages() { return false; }
void DXContext::SaveWindow()           {}
void DXContext::UpdateMonitorWorkRect()
{
    if (!m_hwnd) return;

    // Current monitor geometry
    HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
        m_monitor_rect      = mi.rcMonitor;
        m_monitor_rect_orig = mi.rcMonitor;
        m_monitor_work_rect      = mi.rcWork;
        m_monitor_work_rect_orig = mi.rcWork;
    }

    // Virtual screen (all monitors combined)
    m_all_monitors_rect.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_all_monitors_rect.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_all_monitors_rect.right  = m_all_monitors_rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_all_monitors_rect.bottom = m_all_monitors_rect.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// ---------------------------------------------------------------------------
// Descriptor heap allocators (simple bump allocators)
// ---------------------------------------------------------------------------

// Occupancy of the high RTV range reserved for session-lived mirrors.
// Survives ResetDynamicDescriptors / window resize (unlike the bump allocator).
static bool s_mirrorRtvUsed[DXC_MIRROR_RTV_RESERVE] = {};

D3D12_CPU_DESCRIPTOR_HANDLE DXContext::AllocateRtv()
{
    // Dynamic RTVs must never enter the mirror-reserved range at the top of the heap
    assert(m_nextFreeRtvSlot < DXC_MIRROR_RTV_BASE);
    if (m_nextFreeRtvSlot >= DXC_MIRROR_RTV_BASE) {
        DebugLogA("DX12: AllocateRtv exhausted dynamic range (before mirror reserve)", LOG_ERROR);
        D3D12_CPU_DESCRIPTOR_HANDLE bad = {};
        return bad;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)m_nextFreeRtvSlot * m_rtvDescriptorSize;
    m_nextFreeRtvSlot++;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DXContext::GetRtvCpuHandleAt(UINT rtvIndex)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)rtvIndex * m_rtvDescriptorSize;
    return handle;
}

UINT DXContext::AllocateMirrorRtvBlock(UINT count)
{
    if (count == 0 || count > DXC_MIRROR_RTV_RESERVE)
        return UINT_MAX;
    for (UINT start = 0; start + count <= DXC_MIRROR_RTV_RESERVE; start++) {
        bool free = true;
        for (UINT i = 0; i < count; i++) {
            if (s_mirrorRtvUsed[start + i]) { free = false; break; }
        }
        if (!free) continue;
        for (UINT i = 0; i < count; i++)
            s_mirrorRtvUsed[start + i] = true;
        return DXC_MIRROR_RTV_BASE + start;
    }
    DebugLogA("DX12: AllocateMirrorRtvBlock: reserve exhausted", LOG_ERROR);
    return UINT_MAX;
}

void DXContext::FreeMirrorRtvBlock(UINT base, UINT count)
{
    if (base == UINT_MAX || count == 0 || base < DXC_MIRROR_RTV_BASE)
        return;
    UINT start = base - DXC_MIRROR_RTV_BASE;
    for (UINT i = 0; i < count && start + i < DXC_MIRROR_RTV_RESERVE; i++)
        s_mirrorRtvUsed[start + i] = false;
}

D3D12_CPU_DESCRIPTOR_HANDLE DXContext::AllocateSrvCpu()
{
    // A recycled slot in preference to a fresh one (#312). Without this the
    // heap only ever grows, and evicting textures so they can be re-loaded
    // exhausts it inside a few hundred distinct images.
    if (!m_freeSrvSlots.empty()) {
        m_pendingSrvSlot = m_freeSrvSlots.back();
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)m_pendingSrvSlot * m_srvDescriptorSize;
        return handle;   // AllocateSrvGpu pops it
    }
    if (m_nextFreeSrvSlot >= DXC_MAX_SRV) {
        DebugLogA("ERROR: SRV heap overflow in AllocateSrvCpu!", LOG_ERROR);
        assert(false && "SRV heap overflow");
        // Clamp to last valid slot to avoid out-of-bounds write
        m_pendingSrvSlot = DXC_MAX_SRV - 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)(DXC_MAX_SRV - 1) * m_srvDescriptorSize;
        return handle;
    }
    m_pendingSrvSlot = m_nextFreeSrvSlot;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)m_nextFreeSrvSlot * m_srvDescriptorSize;
    return handle;  // does NOT consume — caller must call AllocateSrvGpu
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::AllocateSrvGpu()
{
    // Consumes whatever AllocateSrvCpu set aside: a recycled slot is popped,
    // a fresh one bumps the high-water mark.
    if (!m_freeSrvSlots.empty() && m_pendingSrvSlot == m_freeSrvSlots.back()) {
        m_freeSrvSlots.pop_back();
        D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)m_pendingSrvSlot * m_srvDescriptorSize;
        return handle;
    }
    if (m_nextFreeSrvSlot >= DXC_MAX_SRV) {
        DebugLogA("ERROR: SRV heap overflow in AllocateSrvGpu!", LOG_ERROR);
        assert(false && "SRV heap overflow");
        D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)(DXC_MAX_SRV - 1) * m_srvDescriptorSize;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)m_nextFreeSrvSlot * m_srvDescriptorSize;
    m_nextFreeSrvSlot++;
    return handle;
}

// Release a texture's GPU memory and hand its descriptor slots back (#312).
//
// NULLED BEFORE RELEASED, and that order is the entire safety argument. Every
// descriptor this texture owns is pointed at m_nullTexture first, so a command
// list already recorded against one of those slots samples the null texture
// rather than freed memory. Releasing first is what produced
// "DRED: page fault VA = 0x0" when eviction was first attempted.
//
// The caller is still responsible for the other half: no eviction may happen
// while the GPU has work in flight that could be referencing this texture. See
// Engine::EnforceTextureCacheBudget, which drains first.
void DXContext::RecycleTexture(DX12Texture& tex)
{
    if (!m_device) return;

    const bool haveNull = (m_nullTexture.srvIndex != UINT_MAX);
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrvCpu = {};
    if (haveNull) {
        nullSrvCpu.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                         (SIZE_T)m_nullTexture.srvIndex * m_srvDescriptorSize;
    }

    auto pointAtNull = [&](UINT slot) {
        if (!haveNull || slot == UINT_MAX) return;
        D3D12_CPU_DESCRIPTOR_HANDLE dst;
        dst.ptr = m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                  (SIZE_T)slot * m_srvDescriptorSize;
        m_device->CopyDescriptorsSimple(1, dst, nullSrvCpu,
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        // The readable mirror too, or a later CreateBindingBlockForTexture
        // would copy this slot's stale descriptor out of it (forgejo#16).
        D3D12_CPU_DESCRIPTOR_HANDLE mirror;
        mirror.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                     (SIZE_T)slot * m_srvDescriptorSize;
        m_device->CopyDescriptorsSimple(1, mirror, nullSrvCpu,
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    };

    if (tex.bindingBlockStart != UINT_MAX) {
        for (UINT i = 0; i < BINDING_BLOCK_SIZE; i++)
            pointAtNull(tex.bindingBlockStart + i);
        m_freeBindingBlocks.push_back(tex.bindingBlockStart);
    }
    if (tex.srvIndex != UINT_MAX) {
        pointAtNull(tex.srvIndex);
        m_freeSrvSlots.push_back(tex.srvIndex);
    }

    tex.Reset();   // releases the resource: this is the VRAM coming back
}

// Dump DRED after a device removal: the last GPU ops still in flight, and the
// page-fault VA with the resources that lived there. "Recent freed" entries are
// the tell for a use-after-free / aliasing bug — the resource the GPU faulted on
// had already been destroyed on the CPU side.
void DXContext::RetireComObject(const ComPtr<IUnknown>& obj)
{
    if (!obj) return;
    std::lock_guard<std::mutex> lk(m_retiredMutex);
    m_retiredObjects.emplace_back(CurrentFrameFenceValue(), obj);
}

void DXContext::CollectRetiredObjects()
{
    std::unique_lock<std::mutex> lk(m_retiredMutex);
    if (m_retiredObjects.empty())
        return;

    // The mirrors run their own queue and their own fence, so the main-queue
    // value says nothing about them. Ask, and if any is still busy leave the
    // whole queue for the next frame -- these are a handful of objects and a
    // frame is a millisecond.
    if (m_auxQueuesIdle && !m_auxQueuesIdle()) {
        // Unless the queue is running away, which would mean a surface that
        // never reports idle. Bound it rather than let it grow without limit:
        // drain both queues once, release everything, and say so. A silent,
        // unbounded buffer of GPU resources is the worse failure, and a log
        // line here names a real bug rather than papering over it.
        if (m_retiredObjects.size() < 256)
            return;
        char buf[160];
        sprintf(buf, "DX12: retired GPU objects reached %u with a mirror never "
                     "idle -- draining (#214)", (unsigned)m_retiredObjects.size());
        DebugLogA(buf, LOG_WARN);
        lk.unlock();
        WaitForGpu();
        lk.lock();
        m_retiredObjects.clear();
        return;
    }

    // Compact in place, keeping only what the GPU has not finished with. The
    // skipped entries are released as they are overwritten or as the vector
    // shrinks. Guard the self-assignment: when nothing has been dropped yet,
    // keep == i and moving an element onto itself is not safe for ComPtr.
    const UINT64 done = CompletedFenceValue();
    size_t keep = 0;
    for (size_t i = 0; i < m_retiredObjects.size(); i++) {
        if (m_retiredObjects[i].first <= done)
            continue;
        if (keep != i)
            m_retiredObjects[keep] = std::move(m_retiredObjects[i]);
        keep++;
    }
    m_retiredObjects.resize(keep);
}

bool DXContext::CreateGpuTimingResources()
{
    if (!m_device || !m_commandQueue) return false;
    if (m_tsHeap) return true;

    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kMaxStamps;
    if (FAILED(m_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_tsHeap)))) {
        DebugLogA("GPU timing: CreateQueryHeap failed", LOG_ERROR);
        return false;
    }

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = sizeof(UINT64) * kMaxStamps;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (int i = 0; i < DXC_FRAME_COUNT; i++) {
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&m_tsReadback[i])))) {
            DebugLogA("GPU timing: readback buffer failed", LOG_ERROR);
            m_tsHeap.Reset();
            return false;
        }
    }

    // Ticks per second on the QUEUE's clock. Not the CPU's, and not constant
    // between queues, so it is read from the queue the stamps are written on.
    if (FAILED(m_commandQueue->GetTimestampFrequency(&m_tsFrequency)) ||
        m_tsFrequency == 0) {
        DebugLogA("GPU timing: GetTimestampFrequency failed", LOG_ERROR);
        m_tsHeap.Reset();
        return false;
    }
    return true;
}

void DXContext::EnableGpuStageTiming(bool on)
{
    if (on && !CreateGpuTimingResources()) {
        m_bGpuTiming = false;
        return;
    }
    m_bGpuTiming = on;
    char buf[96];
    sprintf(buf, "GPU timing: %s (report >= %.0f ms)",
            on ? "ON" : "off", m_gpuTimingReportMs);
    DebugLogA(buf, LOG_ERROR);
}

// ─── Whole-frame GPU time for the primary (#245) ───────────────────────────
//
// Same shape as a mirror surface's pair, and created lazily for the same
// reason: the timestamp frequency has to be asked of the QUEUE, and asking
// once before the queue exists leaves it 0 forever -- which is how gpuUs read
// 0 on the surfaces until it was moved to record time.
bool DXContext::CreateFrameTimingResources()
{
    if (m_frameTsHeap) return true;
    if (!m_device) return false;

    D3D12_QUERY_HEAP_DESC td = {};
    td.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    td.Count = 2;                       // one at entry, one at exit
    if (FAILED(m_device->CreateQueryHeap(&td, IID_PPV_ARGS(&m_frameTsHeap))))
        return false;

    for (int i = 0; i < DXC_FRAME_COUNT; i++) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(UINT64) * 2;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&m_frameTsReadback[i])))) {
            m_frameTsHeap.Reset();
            return false;
        }
    }
    return true;
}

void DXContext::BeginFrameTimestamp()
{
    if (!m_commandList || !m_cmdListRecording) return;
    if (!CreateFrameTimingResources()) return;
    // Asked of the QUEUE, and at record time rather than at creation: the
    // queue may not exist when the resources are first made.
    if (m_frameTsFreq == 0 && m_commandQueue &&
        FAILED(m_commandQueue->GetTimestampFrequency(&m_frameTsFreq)))
        m_frameTsFreq = 0;
    if (m_frameTsFreq == 0) return;
    m_commandList->EndQuery(m_frameTsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
}

void DXContext::EndFrameTimestamp()
{
    if (!m_commandList || !m_cmdListRecording) return;
    if (!m_frameTsHeap || m_frameTsFreq == 0) return;
    m_commandList->EndQuery(m_frameTsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    m_commandList->ResolveQueryData(m_frameTsHeap.Get(),
                                    D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                    m_frameTsReadback[m_frameIndex].Get(), 0);
    m_frameTsPending[m_frameIndex] = true;
}

void DXContext::ReadFrameTimestamp()
{
    if (!m_frameTsHeap || m_frameTsFreq == 0) return;
    if (!m_frameTsPending[m_frameIndex]) return;
    m_frameTsPending[m_frameIndex] = false;
    // Safe to map without stalling: this runs from BeginFrame for THIS frame
    // index, and MoveToNextFrame has already waited out its fence.
    D3D12_RANGE rr = { 0, sizeof(UINT64) * 2 };
    void* p = nullptr;
    if (FAILED(m_frameTsReadback[m_frameIndex]->Map(0, &rr, &p)) || !p) return;
    const UINT64* ts = (const UINT64*)p;
    if (ts[1] > ts[0]) {
        m_lastFrameGpuUs.store(
            (unsigned)((ts[1] - ts[0]) * 1000000ull / m_frameTsFreq),
            std::memory_order_relaxed);
    }
    D3D12_RANGE none = { 0, 0 };
    m_frameTsReadback[m_frameIndex]->Unmap(0, &none);
}

void DXContext::ResolveGpuTimestamps()
{
    if (!m_bGpuTiming || !m_tsHeap || m_tsCount <= 0) return;
    if (!m_commandList || !m_cmdListRecording) return;
    m_commandList->ResolveQueryData(m_tsHeap.Get(),
                                    D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                    (UINT)m_tsCount,
                                    m_tsReadback[m_frameIndex].Get(), 0);
    m_tsPending[m_frameIndex] = m_tsCount;
    for (int i = 0; i < m_tsCount && i < kMaxStamps; i++)
        m_tsPendingNames[m_frameIndex][i] = m_tsNames[i];
}

void DXContext::ReportGpuTimestamps()
{
    if (!m_bGpuTiming || !m_tsHeap) return;
    const int n = m_tsPending[m_frameIndex];
    if (n < 2) return;                       // need a pair to measure anything
    m_tsPending[m_frameIndex] = 0;

    // Safe to map: this runs from BeginFrame for THIS frame index, and
    // MoveToNextFrame has already waited for that frame's fence, so the resolve
    // that wrote these values has completed.
    void* p = nullptr;
    D3D12_RANGE rr = { 0, sizeof(UINT64) * (size_t)n };
    if (FAILED(m_tsReadback[m_frameIndex]->Map(0, &rr, &p)) || !p) return;
    const UINT64* ticks = (const UINT64*)p;

    double worstMs = 0.0;
    const char* worstName = nullptr;
    double totalMs = 0.0;
    for (int i = 1; i < n; i++) {
        if (ticks[i] < ticks[i - 1]) continue;          // clock reset; ignore
        const double ms = (double)(ticks[i] - ticks[i - 1]) * 1000.0
                        / (double)m_tsFrequency;
        totalMs += ms;
        if (ms > worstMs) {
            worstMs = ms;
            worstName = m_tsPendingNames[m_frameIndex][i - 1];
        }
    }
    D3D12_RANGE none = { 0, 0 };
    m_tsReadback[m_frameIndex]->Unmap(0, &none);

    // Only the frames that matter. A normal frame prints nothing at all, so
    // this can be left on while waiting for a device to die.
    if (worstMs >= m_gpuTimingReportMs) {
        char buf[256];
        sprintf(buf, "GPU timing: frame %.1f ms total, WORST STAGE '%s' %.1f ms "
                     "(%d stamps)",
                totalMs, worstName ? worstName : "?", worstMs, n);
        DebugLogA(buf, LOG_ERROR);
    }
}

void DXContext::ResetStageMap()
{
    // Carry the finished frame over before clearing: the GPU is typically a
    // frame behind, so when a hang is noticed the interesting map is this one.
    m_prevStageMarkCount = m_stageMarkCount;
    m_prevFrameDrawCount = m_frameDrawOrdinal;
    for (int i = 0; i < m_stageMarkCount; i++)
        m_prevStageMarks[i] = m_stageMarks[i];
    for (int i = 0; i < kMaxTrackedDraws; i++) {
        m_prevDrawRet[i] = m_frameDrawRet[i];
        m_frameDrawRet[i] = nullptr;
    }
    m_stageMarkCount = 0;
    m_frameDrawOrdinal = 0;
}

void DXContext::BeginStage(const char* name)
{
    // Only the primary list is mapped. Mirror workers record into their own
    // command lists on their own threads, and interleaving their stages into
    // one shared array would produce a map that describes no frame at all.
    //
    // Tests the THREAD-LOCAL override, not m_cmdListOverride. That member has
    // zero assignments anywhere in the tree, so this guard never once fired:
    // mirror workers fell straight through it, appended their stages to the
    // shared map (which is why a DRED stage map shows three warp passes), and
    // -- once #230 added timestamps here -- called EndQuery on the PRIMARY's
    // command list from a worker thread while the render thread was recording
    // into it. A command list is not thread-safe, so that is a data race, and
    // the timings it produced described no single list.
    if (t_cmdListOverride || m_cmdListOverride) return;

    // A timestamp at the same boundary (#230). The stage map says WHICH stage
    // issued a draw; this says how long the GPU spent in it, which is the half
    // that was missing.
    if (m_bGpuTiming && m_tsHeap && m_tsCount < kMaxStamps &&
        m_commandList && m_cmdListRecording) {
        m_commandList->EndQuery(m_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                (UINT)m_tsCount);
        m_tsNames[m_tsCount] = name;
        m_tsCount++;
    }

    if (m_stageMarkCount >= kMaxStageMarks) return;
    m_stageMarks[m_stageMarkCount].draw = m_frameDrawOrdinal;
    m_stageMarks[m_stageMarkCount].name = name;   // literal, not copied
    m_stageMarkCount++;
}

// D3D12_AUTO_BREADCRUMB_OP as a name. The raw integers are unreadable and are
// easy to decode wrongly from memory -- DRAWINSTANCED (3) and
// CLEARDEPTHSTENCILVIEW (14) in particular have been mixed up here before,
// which sends an investigation at the wrong stage entirely.
static const char* DredOpName(int op)
{
    static const char* kNames[] = {
        "SETMARKER", "BEGINEVENT", "ENDEVENT", "DRAWINSTANCED",
        "DRAWINDEXEDINSTANCED", "EXECUTEINDIRECT", "DISPATCH",
        "COPYBUFFERREGION", "COPYTEXTUREREGION", "COPYRESOURCE", "COPYTILES",
        "RESOLVESUBRESOURCE", "CLEARRENDERTARGETVIEW",
        "CLEARUNORDEREDACCESSVIEW", "CLEARDEPTHSTENCILVIEW", "RESOURCEBARRIER",
        "EXECUTEBUNDLE", "PRESENT", "RESOLVEQUERYDATA", "BEGINSUBMISSION",
        "ENDSUBMISSION", "DECODEFRAME", "PROCESSFRAMES",
        "ATOMICCOPYBUFFERUINT", "ATOMICCOPYBUFFERUINT64",
        "RESOLVESUBRESOURCEREGION", "WRITEBUFFERIMMEDIATE", "DECODEFRAME1",
        "SETPROTECTEDRESOURCESESSION", "DECODEFRAME2", "PROCESSFRAMES1",
        "BUILDRAYTRACINGACCELERATIONSTRUCTURE",
        "EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO",
        "COPYRAYTRACINGACCELERATIONSTRUCTURE", "DISPATCHRAYS",
        "INITIALIZEMETACOMMAND", "EXECUTEMETACOMMAND", "ESTIMATEMOTION",
        "RESOLVEMOTIONVECTORHEAP", "SETPIPELINESTATE1",
        "INITIALIZEEXTENSIONCOMMAND", "EXECUTEEXTENSIONCOMMAND",
        "DISPATCHMESH", "ENCODEFRAME", "RESOLVEENCODEROUTPUTMETADATA",
        "BARRIER", "BEGIN_COMMAND_LIST", "DISPATCHGRAPH", "SETPROGRAM",
    };
    if (op >= 0 && op < (int)(sizeof(kNames) / sizeof(kNames[0])))
        return kNames[op];
    static char other[32];
    sprintf(other, "op#%d", op);
    return other;
}

void DXContext::DumpDredToLog(const char* whence)
{
    if (!m_device)
        return;
    // Once per device — see m_pDredDumpedFor. The breadcrumbs do not change after
    // the removal, so the first site to notice is as good as any, and it is the
    // one whose name is worth recording.
    ID3D12Device* dumped = m_pDredDumpedFor.load(std::memory_order_relaxed);
    if (dumped == m_device.Get())
        return;
    if (!m_pDredDumpedFor.compare_exchange_strong(dumped, m_device.Get()))
        return;   // another thread got there first

    {
        char who[256];
        sprintf(who, "DRED: dump triggered from %s (removed reason=0x%08X)",
                whence ? whence : "<unspecified>", (unsigned)m_device->GetDeviceRemovedReason());
        DebugLogA(who, LOG_ERROR);
    }

    // The stage map first: it is what makes the breadcrumbs below readable
    // (#222). Draw ordinals count DRAWINSTANCED and DRAWINDEXEDINSTANCED only,
    // so match them against the DRAW ops in the dump, not against op indices —
    // barriers, clears and copies occupy op slots but no draw ordinal.
    {
        char sbuf[512];
        auto dumpMap = [&](const StageMark* marks, int count, UINT draws,
                           const char* which) {
            if (count <= 0) return;
            sprintf(sbuf, "DRED: stage map (%s frame, %u draws):", which, draws);
            DebugLogA(sbuf, LOG_ERROR);
            for (int i = 0; i < count; i++) {
                const UINT end = (i + 1 < count) ? marks[i + 1].draw : draws;
                if (end == marks[i].draw)
                    continue;                      // stage emitted no draws
                sprintf(sbuf, "DRED:   draw[%u..%u] = %s",
                        marks[i].draw, end - 1, marks[i].name ? marks[i].name : "?");
                DebugLogA(sbuf, LOG_ERROR);
            }
        };
        dumpMap(m_prevStageMarks, m_prevStageMarkCount, m_prevFrameDrawCount, "previous");
        dumpMap(m_stageMarks, m_stageMarkCount, m_frameDrawOrdinal, "current");

        // Per-draw call sites. This is the authoritative half of the map: the
        // stage names above are a convenience, these are ground truth.
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(proc, nullptr, TRUE);   // harmless if already initialised
        auto dumpSites = [&](void* const* rets, UINT count, const char* which) {
            if (count == 0) return;
            sprintf(sbuf, "DRED: draw call sites (%s frame):", which);
            DebugLogA(sbuf, LOG_ERROR);
            const UINT n = (count < kMaxTrackedDraws) ? count : kMaxTrackedDraws;
            for (UINT i = 0; i < n; i++) {
                if (!rets[i]) continue;
                const DWORD64 addr = (DWORD64)rets[i];
                char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
                SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen   = 255;
                DWORD64 disp = 0;
                const char* name = SymFromAddr(proc, addr, &disp, sym) ? sym->Name : "?";
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisp = 0;
                if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
                    const char* base = strrchr(line.FileName, '\\');
                    sprintf(sbuf, "DRED:   draw[%u] <- %s (%s:%lu)", i, name,
                            base ? base + 1 : line.FileName, line.LineNumber);
                } else {
                    sprintf(sbuf, "DRED:   draw[%u] <- %s", i, name);
                }
                DebugLogA(sbuf, LOG_ERROR);
            }
        };
        dumpSites(m_prevDrawRet, m_prevFrameDrawCount, "previous");
        dumpSites(m_frameDrawRet, m_frameDrawOrdinal, "current");
    }

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        DebugLogA("DRED: not available (needs Win10 1809+ and FORCED_ON at init)", LOG_ERROR);
        return;
    }
    char buf[512];

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&bc))) {
        int nList = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc.pHeadAutoBreadcrumbNode;
             n && nList < 8; n = n->pNext, nList++) {
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            char nameA[128] = "<unnamed>";
            if (n->pCommandListDebugNameA)
                strncpy_s(nameA, n->pCommandListDebugNameA, _TRUNCATE);
            else if (n->pCommandListDebugNameW)
                wcstombs(nameA, n->pCommandListDebugNameW, sizeof(nameA) - 1);
            sprintf(buf, "DRED: list '%s' completed %u of %u ops", nameA, done, n->BreadcrumbCount);
            DebugLogA(buf, LOG_ERROR);
            // The ops around the stall are the ones that matter.
            //
            // pLastBreadcrumbValue is a COUNT of completed operations, and
            // pCommandHistory is indexed from zero. So ops [0, done) finished
            // and pCommandHistory[done] is the FIRST ONE THAT DID NOT -- the
            // op the GPU faulted on.
            //
            // This used to tag index `done` as "last completed", which is off
            // by one in the worst possible direction: it reads as an alibi for
            // precisely the operation that crashed. #195 was a CopyResource
            // whose destination had been freed mid-frame, and the breadcrumb
            // said that copy had completed successfully, so the investigation
            // went looking at the ops after it instead. Three of five parallel
            // investigators were sent the same wrong way by the same label.
            // Print the WHOLE list, decoded, not a window around the fault
            // (#222). A window is enough when you already know what the list
            // contains; it is useless when the question IS what the list
            // contains. The #222 frame is 21 ops and every hang produces the
            // identical list, so the full dump identifies the faulting stage
            // outright -- from the window alone it was guesswork, and two
            // plausible stages were blamed and exonerated in turn.
            //
            // Bounded at 128: a Shadertoy or heavy-shape frame runs to
            // thousands of ops and the tail is what matters there, so past the
            // bound this falls back to the window around the fault.
            const bool full = (n->BreadcrumbCount <= 128);
            const UINT lo = full ? 0 : ((done > 3) ? done - 3 : 0);
            const UINT hi = full ? n->BreadcrumbCount
                                 : ((done + 4 < n->BreadcrumbCount) ? done + 4
                                                                    : n->BreadcrumbCount);
            if (!full)
                DebugLogA("DRED:   (list too long to dump whole -- window around the fault)",
                          LOG_ERROR);
            for (UINT i = lo; i < hi; i++) {
                const char* tag = "";
                if (done > 0 && i == done - 1)
                    tag = "   <-- last completed";
                else if (i == done && done < n->BreadcrumbCount)
                    tag = "   <-- FAULTED HERE (first op that did not complete)";
                const int op = (int)n->pCommandHistory[i];
                sprintf(buf, "DRED:   op[%u] = %-26s%s", i, DredOpName(op), tag);
                DebugLogA(buf, LOG_ERROR);
            }
            if (done >= n->BreadcrumbCount)
                DebugLogA("DRED:   (every op on this list completed)", LOG_ERROR);
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pf))) {
        sprintf(buf, "DRED: page fault VA = 0x%llX", (unsigned long long)pf.PageFaultVA);
        DebugLogA(buf, LOG_ERROR);
        auto dumpNodes = [&](const D3D12_DRED_ALLOCATION_NODE1* head, const char* what) {
            int i = 0;
            for (const D3D12_DRED_ALLOCATION_NODE1* n = head; n && i < 16; n = n->pNext, i++) {
                char nameA[128] = "<unnamed>";
                if (n->ObjectNameA)
                    strncpy_s(nameA, n->ObjectNameA, _TRUNCATE);
                else if (n->ObjectNameW)
                    wcstombs(nameA, n->ObjectNameW, sizeof(nameA) - 1);
                sprintf(buf, "DRED:   %s: '%s' type=%d", what, nameA, (int)n->AllocationType);
                DebugLogA(buf, LOG_ERROR);
            }
        };
        dumpNodes(pf.pHeadExistingAllocationNode, "existing at VA");
        dumpNodes(pf.pHeadRecentFreedAllocationNode, "RECENTLY FREED at VA");
    }
}

void DXContext::ResetDynamicDescriptors()
{
    // Rewind bump allocators to the post-init baseline so that dynamic textures
    // (VS, blur, etc.) can be re-created without leaking descriptor heap slots.
    // Mirror-reserved RTV slots (high end of heap) are NOT rewound.
    m_nextFreeRtvSlot = m_rtvSlotBaseline;
    m_nextFreeSrvSlot = m_srvSlotBaseline;
    m_descriptorEpoch++;
}

D3D12_CPU_DESCRIPTOR_HANDLE DXContext::GetSrvCpuHandleAt(UINT srvIndex)
{
    assert(srvIndex < DXC_MAX_SRV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)srvIndex * m_srvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DXContext::GetSrvCpuHandleAtReadable(UINT srvIndex)
{
    assert(srvIndex < DXC_MAX_SRV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart();
    // m_srvDescriptorSize: both heaps are D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    // and the increment size for a given heap type is a per-device constant,
    // not per-heap -- one size serves both.
    handle.ptr += (SIZE_T)srvIndex * m_srvDescriptorSize;
    return handle;
}

void DXContext::UpdateBindingBlockTexture(UINT blockStart, UINT texSrvIndex)
{
    if (blockStart == UINT_MAX) return;

    // dst is m_srvHeap (shader-visible, the only heap ever bound for
    // rendering); src is m_srvHeapCpu (its readable mirror) -- reading
    // m_srvHeap's own CPU handles as a copy source is invalid D3D12 usage
    // (forgejo#16).
    D3D12_CPU_DESCRIPTOR_HANDLE heapStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE heapStartReadable = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart();

    // Source handle for the texture (or null if UINT_MAX)
    D3D12_CPU_DESCRIPTOR_HANDLE texSrc;
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrc;
    nullSrc.ptr = heapStartReadable.ptr + (SIZE_T)m_nullTexture.srvIndex * m_srvDescriptorSize;

    if (texSrvIndex != UINT_MAX)
        texSrc.ptr = heapStartReadable.ptr + (SIZE_T)texSrvIndex * m_srvDescriptorSize;
    else
        texSrc = nullSrc;

    // Overwrite all 32 slots: slot 0 = texture, slots 1-31 = null
    for (UINT i = 0; i < BINDING_BLOCK_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst;
        dst.ptr = heapStart.ptr + (SIZE_T)(blockStart + i) * m_srvDescriptorSize;
        m_device->CopyDescriptorsSimple(1, dst, (i == 0) ? texSrc : nullSrc,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

// ---------------------------------------------------------------------------
// Texture creation helpers
// ---------------------------------------------------------------------------

DX12Texture DXContext::CreateRenderTargetTexture(UINT width, UINT height, DXGI_FORMAT format,
                                                 float clearAlpha)
{
    DX12Texture tex;
    if (!m_device || width == 0 || height == 0) return tex;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width              = width;
    desc.Height             = height;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = format;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format   = format;
    clearValue.Color[0] = 0.f;
    clearValue.Color[1] = 0.f;
    clearValue.Color[2] = 0.f;
    clearValue.Color[3] = clearAlpha;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearValue, IID_PPV_ARGS(&tex.resource));
    if (FAILED(hr)) return tex;

    tex.width  = width;
    tex.height = height;
    tex.depth  = 1;
    tex.format = format;
    tex.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Allocate RTV descriptor — fail cleanly if dynamic RTV range is exhausted
    // (invalid CreateRenderTargetView was SEH-crashing the render loop).
    if (m_nextFreeRtvSlot >= DXC_MIRROR_RTV_BASE) {
        DebugLogA("DX12: CreateRenderTargetTexture: RTV heap exhausted", LOG_ERROR);
        tex.Reset();
        return tex;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = AllocateRtv();
    tex.rtvIndex = m_nextFreeRtvSlot - 1;
    m_device->CreateRenderTargetView(tex.resource.Get(), nullptr, rtvHandle);

    // Allocate SRV descriptor
    if (m_nextFreeSrvSlot >= DXC_MAX_SRV) {
        DebugLogA("DX12: CreateRenderTargetTexture: SRV heap exhausted", LOG_ERROR);
        tex.Reset();
        return tex;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle = AllocateSrvCpu();
    tex.srvIndex = PendingSrvSlot();

    CreateSRV2D(m_device.Get(), tex.resource.Get(), format, srvCpuHandle,
                GetSrvCpuHandleAtReadable(tex.srvIndex));

    AllocateSrvGpu();  // bump the slot counter

    return tex;
}

// ---------------------------------------------------------------------------
// Resource state transitions + descriptor handle helpers (Phase 4)
// ---------------------------------------------------------------------------

void DXContext::TransitionResource(DX12Texture& tex, D3D12_RESOURCE_STATES newState,
                                   ID3D12GraphicsCommandList* cmdList)
{
    if (!tex.IsValid() || tex.currentState == newState)
        return;
    ID3D12GraphicsCommandList* cl = cmdList ? cmdList : GetActiveCmdList();
    if (!cl)
        return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = tex.resource.Get();
    barrier.Transition.StateBefore = tex.currentState;
    barrier.Transition.StateAfter  = newState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrier);
    tex.currentState = newState;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetSrvGpuHandle(const DX12Texture& tex)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)tex.srvIndex * m_srvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DXContext::GetRtvCpuHandle(const DX12Texture& tex)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)tex.rtvIndex * m_rtvDescriptorSize;
    return handle;
}

// ---------------------------------------------------------------------------
// Root signature creation (Phase 3, updated Phase 4)
// ---------------------------------------------------------------------------

bool DXContext::CreateRootSignature()
{
    // Root parameter 0: CBV at b0 ($Globals constant buffer)
    D3D12_ROOT_PARAMETER rootParams[2] = {};

    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace  = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Root parameter 1: Descriptor table — 32 SRVs (t0-t31)
    // Preset shaders may reference multiple textures (main, blur1-3, noise, custom)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 32;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers — 4 shared sampler states (s0-s3).
    // All textures are declared as Texture2D (t-registers, no s-register limit).
    // The 4 SamplerState objects in the include file reference these:
    //   s0 = _samp_lw  (LINEAR + WRAP)   — default for most textures
    //   s1 = _samp_lc  (LINEAR + CLAMP)  — fc_main, blur
    //   s2 = _samp_pc  (POINT  + CLAMP)  — pc_main
    //   s3 = _samp_pw  (POINT  + WRAP)   — pw_main
    //   s4 = _samp_lm  (LINEAR + MIRROR) — presets' own sampler_state blocks
    //   s5 = _samp_pm  (POINT  + MIRROR)
    //
    // The mirror pair exists because DX9 presets declare their addressing
    // inline -- `sampler X = sampler_state { AddressU = mirror; }` -- and
    // MIRROR is the mode that had nowhere to go (#318). Without it the state
    // block was simply dropped and the texture sampled WRAP, which is a
    // different picture, silently.
    D3D12_STATIC_SAMPLER_DESC staticSamplers[6] = {};
    for (int i = 0; i < 6; i++) {
        staticSamplers[i].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplers[i].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[i].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[i].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[i].MaxLOD           = D3D12_FLOAT32_MAX;
        staticSamplers[i].ShaderRegister   = i;
        staticSamplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // s1: LINEAR + CLAMP
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    // s2: POINT + CLAMP
    staticSamplers[2].Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    // s3: POINT + WRAP
    staticSamplers[3].Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
    // s4: LINEAR + MIRROR
    staticSamplers[4].AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    staticSamplers[4].AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    staticSamplers[4].AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    // s5: POINT + MIRROR
    staticSamplers[5].Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
    staticSamplers[5].AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    staticSamplers[5].AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    staticSamplers[5].AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;

    // Anisotropic filtering: upgrade LINEAR samplers (s0, s1, s4) to ANISOTROPIC
    if (m_bAnisotropicFiltering) {
        staticSamplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
        staticSamplers[0].MaxAnisotropy = 16;
        staticSamplers[1].Filter = D3D12_FILTER_ANISOTROPIC;
        staticSamplers[1].MaxAnisotropy = 16;
        staticSamplers[4].Filter = D3D12_FILTER_ANISOTROPIC;
        staticSamplers[4].MaxAnisotropy = 16;
    }

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters     = 2;
    rootSigDesc.pParameters       = rootParams;
    rootSigDesc.NumStaticSamplers = 6;
    rootSigDesc.pStaticSamplers   = staticSamplers;
    rootSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &signature, &error);
    if (FAILED(hr)) {
        if (error) DebugLogA((const char*)error->GetBufferPointer(), LOG_ERROR);
        return false;
    }

    hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(),
                                        signature->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr)) return false;

    // Blur root signature: same as main. Blur shaders use _samp_lc (s1) directly
    // via text substitution in LoadShaderFromMemory, so no special s0 override needed.
    m_blurRootSignature = m_rootSignature;
    return true;
}

bool DXContext::RecreateRootSigAndPipelines()
{
    WaitForGpu();
    m_rootSignature.Reset();
    m_blurRootSignature.Reset();
    for (auto& pso : m_PSOs)
        pso.Reset();
    if (!CreateRootSignature()) return false;
    if (!CreatePipelines()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Upload heap ring buffers (Phase 3)
// ---------------------------------------------------------------------------

bool DXContext::CreateUploadBuffers()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = UPLOAD_BUFFER_SIZE;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (UINT i = 0; i < DXC_FRAME_COUNT; i++) {
        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_uploadBuffer[i]));
        if (FAILED(hr)) return false;

        // Persistently map the upload buffer
        void* pData = nullptr;
        D3D12_RANGE readRange = { 0, 0 }; // CPU won't read from it
        hr = m_uploadBuffer[i]->Map(0, &readRange, &pData);
        if (FAILED(hr)) return false;
        m_uploadBufferPtr[i] = (BYTE*)pData;
        m_uploadBufferOffset[i] = 0;
    }

    // Aux upload for mirror independent draws (after main list has been submitted).
    // One slot per frame index — safe once MoveToNextFrame waited on that slot.
    {
        D3D12_RESOURCE_DESC auxDesc = bufDesc;
        auxDesc.Width = AUX_UPLOAD_BUFFER_SIZE;
        for (UINT i = 0; i < DXC_FRAME_COUNT; i++) {
            HRESULT hr = m_device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &auxDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&m_auxUploadBuffer[i]));
            if (FAILED(hr)) return false;
            void* pData = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            hr = m_auxUploadBuffer[i]->Map(0, &readRange, &pData);
            if (FAILED(hr)) return false;
            m_auxUploadPtr[i] = (BYTE*)pData;
            m_auxUploadOffset[i] = 0;
        }
        m_bUseAuxUpload = false;
    }
    return true;
}

// Mirror worker ring. Created when the opposite-orient thread starts and freed
// when it stops, so the 32 MB is only resident while independent mirrors are on.
// Allocates THIS THREAD's ring. Each mirror worker calls it once, so the 32 MB
// is per worker and only resident while that worker is running.
bool DXContext::CreateMirrorUploadBuffer(DWORD tid)
{
    MirrorUploadRing* ring = MirrorRingForThread(tid);
    if (!ring)
        return false;          // no slot claimed: caller runs on the shared ring
    if (ring->buffer)
        return true;
    if (!m_device)
        return false;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = AUX_UPLOAD_BUFFER_SIZE;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&ring->buffer));
    if (FAILED(hr)) {
        DebugLogA("DX12: CreateMirrorUploadBuffer failed", LOG_ERROR);
        return false;
    }
    void* pData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = ring->buffer->Map(0, &readRange, &pData);
    if (FAILED(hr)) {
        ring->buffer.Reset();
        return false;
    }
    ring->ptr = (BYTE*)pData;
    ring->offset = 0;
    return true;
}

void DXContext::ReleaseMirrorUploadBuffer(DWORD tid)
{
    MirrorUploadRing* ring = MirrorRingForThread(tid);
    if (!ring)
        return;
    if (ring->buffer && ring->ptr)
        ring->buffer->Unmap(0, nullptr);
    ring->ptr = nullptr;
    ring->offset = 0;
    ring->buffer.Reset();
}

// Claim or release a ring slot for `tid`. Called by a worker as it starts and
// stops; 0 releases the calling thread's slot.
bool DXContext::SetMirrorUploadThreadId(DWORD tid)
{
    if (tid == 0) {
        const DWORD me = GetCurrentThreadId();
        for (int i = 0; i < MAX_MIRROR_UPLOAD_RINGS; i++)
            if (m_mirrorRings[i].threadId.load(std::memory_order_acquire) == me)
                m_mirrorRings[i].threadId.store(0, std::memory_order_release);
        return true;
    }
    for (int i = 0; i < MAX_MIRROR_UPLOAD_RINGS; i++)
        if (m_mirrorRings[i].threadId.load(std::memory_order_acquire) == tid)
            return true;                       // already claimed by this thread
    for (int i = 0; i < MAX_MIRROR_UPLOAD_RINGS; i++) {
        DWORD expected = 0;
        if (m_mirrorRings[i].threadId.compare_exchange_strong(
                expected, tid, std::memory_order_acq_rel))
            return true;
    }
    // Every slot taken. The thread still records -- it falls through to the
    // aux/main ring, which is what an unregistered thread has always done.
    DebugLogA("DX12: no mirror upload ring free; worker uses the shared ring",
              LOG_WARN);
    return false;
}

// Main / aux / mirror ring selection, shared by DrawVertices and
// UploadConstantBuffer so the two can never disagree about which ring is live.
void DXContext::SelectUploadRing(BYTE*& basePtr, ID3D12Resource*& baseRes,
                                 UINT*& pOffset, UINT& bufSize)
{
    if (MirrorUploadRing* ring = MirrorRingForThisThread()) {
        if (ring->ptr && ring->buffer) {
            basePtr = ring->ptr;
            baseRes = ring->buffer.Get();
            pOffset = &ring->offset;
            bufSize = AUX_UPLOAD_BUFFER_SIZE;
            return;
        }
    }
    if (m_bUseAuxUpload && m_auxUploadPtr[m_frameIndex] && m_auxUploadBuffer[m_frameIndex]) {
        basePtr = m_auxUploadPtr[m_frameIndex];
        baseRes = m_auxUploadBuffer[m_frameIndex].Get();
        pOffset = &m_auxUploadOffset[m_frameIndex];
        bufSize = AUX_UPLOAD_BUFFER_SIZE;
        return;
    }
    const UINT fi = m_frameIndex;
    basePtr = m_uploadBufferPtr[fi];
    baseRes = m_uploadBuffer[fi].Get();
    pOffset = &m_uploadBufferOffset[fi];
    bufSize = UPLOAD_BUFFER_SIZE;
}

thread_local ID3D12GraphicsCommandList* DXContext::t_cmdListOverride = nullptr;

void DXContext::NoteActiveCmdListOnWorker() const
{
  const UINT n = m_activeCmdListOnWorker.fetch_add(1, std::memory_order_relaxed);
  if (n == 0)
    DebugLogA("GetActiveCmdList called on a MIRROR WORKER thread -- that site "
              "should take its list from its RenderContext (#206). Counted in "
              "DIAG_MIRRORS as workerCmdList=.\n", LOG_ERROR);
}

void DXContext::ResetUploadBuffer()
{
    m_uploadBufferOffset[m_frameIndex] = 0;
}

void DXContext::DrawVertices(D3D12_PRIMITIVE_TOPOLOGY topology, const void* vertexData,
                              UINT vertexCount, UINT vertexStride,
                              ID3D12GraphicsCommandList* cmdList)
{
    if (vertexCount == 0) return;
    ID3D12GraphicsCommandList* cl = cmdList ? cmdList : GetActiveCmdList();
    if (!cl) return;

    UINT totalBytes = vertexCount * vertexStride;
    BYTE* basePtr = nullptr;
    ID3D12Resource* baseRes = nullptr;
    UINT* pOffset = nullptr;
    UINT bufSize = 0;

    SelectUploadRing(basePtr, baseRes, pOffset, bufSize);

    if (*pOffset + totalBytes > bufSize) {
        // Rate-limit: heavy milk2 orient re-renders can hit this thousands of times/frame.
        static UINT s_lastExhaustFrame = UINT_MAX;
        static int s_exhaustCount = 0;
        if (s_lastExhaustFrame != m_frameIndex) {
            s_lastExhaustFrame = m_frameIndex;
            s_exhaustCount = 0;
        }
        if (s_exhaustCount++ < 3) {
            char buf[160];
            FormatToA(buf, "DX12: Upload buffer exhausted! offset=%u + needed=%u > %u (aux=%d)%s",
                      *pOffset, totalBytes, bufSize, m_bUseAuxUpload ? 1 : 0,
                      s_exhaustCount == 3 ? " (further messages suppressed this frame)" : "");
            DebugLogA(buf, LOG_ERROR);
        }
        return;
    }

    BYTE* dest = basePtr + *pOffset;
    memcpy(dest, vertexData, totalBytes);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = baseRes->GetGPUVirtualAddress() + *pOffset;
    vbv.SizeInBytes    = totalBytes;
    vbv.StrideInBytes  = vertexStride;
    *pOffset += totalBytes;

    cl->IASetPrimitiveTopology(topology);
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->DrawInstanced(vertexCount, 1, 0, 0);
    if (!m_cmdListOverride) {                       // #222 stage map
        if (m_frameDrawOrdinal < kMaxTrackedDraws)
            m_frameDrawRet[m_frameDrawOrdinal] = _ReturnAddress();
        m_frameDrawOrdinal++;
    }
}

void DXContext::DrawIndexedVertices(D3D12_PRIMITIVE_TOPOLOGY topology,
                                    const void* vertexData, UINT vertexCount, UINT vertexStride,
                                    const UINT16* indexData, UINT indexCount,
                                    ID3D12GraphicsCommandList* cmdList)
{
    if (vertexCount == 0 || indexCount == 0 || !vertexData || !indexData) return;
    ID3D12GraphicsCommandList* cl = cmdList ? cmdList : GetActiveCmdList();
    if (!cl) return;

    BYTE* basePtr = nullptr;
    ID3D12Resource* baseRes = nullptr;
    UINT* pOffset = nullptr;
    UINT bufSize = 0;

    SelectUploadRing(basePtr, baseRes, pOffset, bufSize);

    const UINT vbBytes = vertexCount * vertexStride;
    const UINT ibBytes = indexCount * (UINT)sizeof(UINT16);
    // Both views need their GPU VA aligned; 16 is comfortably above what either requires.
    const UINT vbOffset = (*pOffset + 15u) & ~15u;
    const UINT ibOffset = (vbOffset + vbBytes + 15u) & ~15u;

    if (ibOffset + ibBytes > bufSize) {
        // Same rate limit as DrawVertices — one exhausted frame must not flood the log.
        static UINT s_lastExhaustFrame = UINT_MAX;
        static int s_exhaustCount = 0;
        if (s_lastExhaustFrame != m_frameIndex) {
            s_lastExhaustFrame = m_frameIndex;
            s_exhaustCount = 0;
        }
        if (s_exhaustCount++ < 3) {
            char buf[192];
            FormatToA(buf, "DX12: Upload buffer exhausted (indexed)! offset=%u + vb=%u + ib=%u > %u (aux=%d)%s",
                      *pOffset, vbBytes, ibBytes, bufSize, m_bUseAuxUpload ? 1 : 0,
                      s_exhaustCount == 3 ? " (further messages suppressed this frame)" : "");
            DebugLogA(buf, LOG_ERROR);
        }
        return;
    }

    memcpy(basePtr + vbOffset, vertexData, vbBytes);
    memcpy(basePtr + ibOffset, indexData, ibBytes);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = baseRes->GetGPUVirtualAddress() + vbOffset;
    vbv.SizeInBytes    = vbBytes;
    vbv.StrideInBytes  = vertexStride;

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = baseRes->GetGPUVirtualAddress() + ibOffset;
    ibv.SizeInBytes    = ibBytes;
    ibv.Format         = DXGI_FORMAT_R16_UINT;

    *pOffset = ibOffset + ibBytes;

    cl->IASetPrimitiveTopology(topology);
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->IASetIndexBuffer(&ibv);
    cl->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    if (!m_cmdListOverride) {                       // #222 stage map
        if (m_frameDrawOrdinal < kMaxTrackedDraws)
            m_frameDrawRet[m_frameDrawOrdinal] = _ReturnAddress();
        m_frameDrawOrdinal++;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS DXContext::UploadConstantBuffer(const void* data, UINT sizeBytes) {
    BYTE* basePtr = nullptr;
    ID3D12Resource* baseRes = nullptr;
    UINT* pOffset = nullptr;
    UINT bufSize = 0;

    SelectUploadRing(basePtr, baseRes, pOffset, bufSize);

    UINT alignedOffset = (*pOffset + 255) & ~255;
    UINT alignedSize   = (sizeBytes + 255) & ~255;

    if (alignedOffset + alignedSize > bufSize)
        return 0;

    BYTE* dest = basePtr + alignedOffset;
    memcpy(dest, data, sizeBytes);
    if (alignedSize > sizeBytes)
        memset(dest + sizeBytes, 0, alignedSize - sizeBytes);

    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = baseRes->GetGPUVirtualAddress() + alignedOffset;
    *pOffset = alignedOffset + alignedSize;
    return gpuAddr;
}

// ---------------------------------------------------------------------------
// Null texture + binding block (Phase 5)
// ---------------------------------------------------------------------------

// Pick a random image file from a directory (jpg/png/bmp/tga/dds)
template <size_t N>
static bool PickRandomTextureFile(const wchar_t* szDir, wchar_t (&szOut)[N])
{
    if (!szDir || !szDir[0]) return false;
    std::vector<std::wstring> files;
    const wchar_t* exts[] = { L"*.jpg", L"*.jpeg", L"*.png", L"*.bmp", L"*.tga", L"*.dds" };
    for (auto ext : exts) {
        wchar_t mask[MAX_PATH];
        swprintf(mask, MAX_PATH, L"%s%s", szDir, ext);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(mask, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::wstring full = szDir;
                    full += fd.cFileName;
                    files.push_back(full);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (files.empty()) return false;
    CopyTo(szOut, files[rand() % files.size()].c_str());
    return true;
}

// Procedural fallback-texture pixels for styles 0-2. Shared by CreateNullTexture
// (device init) and RefreshFallbackTexture (live SET_FALLBACK_TEX) so the two can
// never drift apart. Styles 3-5 load an image file instead and never reach here.
static void MakeFallbackPixels(int fallbackStyle, UINT& fbW, UINT& fbH,
                               std::vector<UINT8>& fbPixels)
{
    fbW = 1;
    fbH = 1;
    fbPixels.clear();

    if (fallbackStyle == 0) {
        // Hue Gradient: 256x256 horizontal HSV sweep (S=1, V=1)
        fbW = 256; fbH = 256;
        fbPixels.resize((size_t)fbW * fbH * 4);
        for (UINT y = 0; y < fbH; y++) {
            for (UINT xx = 0; xx < fbW; xx++) {
                float h = (float)xx / (float)fbW * 360.0f;  // 0..360
                // HSV to RGB (S=1, V=1)
                float c = 1.0f;
                float x2 = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
                float r = 0, g = 0, b = 0;
                if (h < 60)       { r = c; g = x2; }
                else if (h < 120) { r = x2; g = c; }
                else if (h < 180) { g = c; b = x2; }
                else if (h < 240) { g = x2; b = c; }
                else if (h < 300) { r = x2; b = c; }
                else              { r = c; b = x2; }
                size_t idx = ((size_t)y * fbW + xx) * 4;
                fbPixels[idx + 0] = (UINT8)(r * 255.0f);
                fbPixels[idx + 1] = (UINT8)(g * 255.0f);
                fbPixels[idx + 2] = (UINT8)(b * 255.0f);
                fbPixels[idx + 3] = 255;
            }
        }
    } else if (fallbackStyle == 1) {
        // White: 1x1 (multiplicative identity)
        fbPixels = { 255, 255, 255, 255 };
    } else {
        // Black: 1x1
        fbPixels = { 0, 0, 0, 255 };
    }
}

// Swap the missing-disk-texture fallback without a device reinit. The settings
// UI only writes the INI (the change lands on next launch); this is the live
// path, used by SET_FALLBACK_TEX to tell "the preset is sampling the fallback"
// apart from "the preset is rendering that" in one step.
bool DXContext::RefreshFallbackTexture(int fallbackStyle, const wchar_t* customFile)
{
    if (!m_device)
        return false;
    if (fallbackStyle < 0 || fallbackStyle > 5)
        return false;

    if (customFile)
        CopyTo(m_szFallbackCustomFile, customFile);

    DX12Texture built;

    if (fallbackStyle >= 3 && fallbackStyle <= 5) {
        wchar_t szPath[MAX_PATH] = {};
        bool gotFile = false;
        if (fallbackStyle == 3)
            gotFile = PickRandomTextureFile(m_szFallbackRandomTexDir, szPath);
        else if (fallbackStyle == 4)
            gotFile = PickRandomTextureFile(m_szFallbackTexturesDir, szPath);
        else if (m_szFallbackCustomFile[0])
            gotFile = CopyTo(szPath, m_szFallbackCustomFile);
        if (gotFile)
            built = LoadTextureFromFile(szPath);
        if (!built.IsValid())
            fallbackStyle = 0;  // load failed — same degrade as CreateNullTexture
    }

    if (!built.IsValid()) {
        UINT fbW = 1, fbH = 1;
        std::vector<UINT8> fbPixels;
        MakeFallbackPixels(fallbackStyle, fbW, fbH, fbPixels);
        built = CreateTextureFromPixels(fbPixels.data(), fbW, fbH, fbW * 4,
                                        DXGI_FORMAT_R8G8B8A8_UNORM, false);
    }

    if (!built.IsValid()) {
        DebugLogA("DX12: RefreshFallbackTexture: build failed — keeping previous", LOG_ERROR);
        return false;
    }

    // The outgoing resource may still be referenced by an in-flight frame, and
    // its descriptor still sits in every binding block filled this frame.
    WaitForGpu();

    m_fallbackTexture = built;
    m_nFallbackTexStyle = fallbackStyle;

    char buf[256];
    sprintf(buf, "DX12: RefreshFallbackTexture: style=%d %ux%u srv=%u (srvUsed=%u/%u)",
            fallbackStyle, m_fallbackTexture.width, m_fallbackTexture.height,
            m_fallbackTexture.srvIndex, m_nextFreeSrvSlot, (UINT)DXC_MAX_SRV);
    DebugLogA(buf, LOG_WARN);
    return true;
}

bool DXContext::CreateNullTexture(int fallbackStyle)
{
    // Create a 1x1 black RGBA texture for filling unused SRV slots
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_nullTexture.resource));
    if (FAILED(hr)) return false;

    // Upload 1 black pixel (0,0,0,255)
    UINT8 blackPixel[4] = { 0, 0, 0, 255 };

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = 256; // minimum upload alignment
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> uploadBuf;
    hr = m_device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuf));
    if (FAILED(hr)) return false;

    UINT8* mapped = nullptr;
    uploadBuf->Map(0, nullptr, (void**)&mapped);
    memcpy(mapped, blackPixel, 4);
    uploadBuf->Unmap(0, nullptr);

    // Copy to the texture via a temporary command list
    m_commandAllocators[0]->Reset();
    m_commandList->Reset(m_commandAllocators[0].Get(), nullptr);
    m_cmdListRecording = true;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuf.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = 1;
    src.PlacedFootprint.Footprint.Height = 1;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = 256; // minimum row pitch

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_nullTexture.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Transition to PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_nullTexture.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->Close();
    m_cmdListRecording = false;
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // Wait for copy to complete
    m_fenceValues[0]++;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValues[0]);
    m_fence->SetEventOnCompletion(m_fenceValues[0], m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);

    // Create SRV for the null texture
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle = AllocateSrvCpu();
    m_nullTexture.srvIndex = PendingSrvSlot();

    CreateSRV2D(m_device.Get(), m_nullTexture.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                srvCpuHandle, GetSrvCpuHandleAtReadable(m_nullTexture.srvIndex));

    AllocateSrvGpu(); // bump counter

    m_nullTexture.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_nullTexture.width = 1;
    m_nullTexture.height = 1;
    m_nullTexture.depth = 1;
    m_nullTexture.format = DXGI_FORMAT_R8G8B8A8_UNORM;

    // --- Fallback texture for missing disk textures ---
    // For styles 3-5, try to load an image file. Fall back to Hue Gradient on failure.
    if (fallbackStyle >= 3 && fallbackStyle <= 5) {
        wchar_t szPath[MAX_PATH] = {};
        bool gotFile = false;
        if (fallbackStyle == 3)
            gotFile = PickRandomTextureFile(m_szFallbackRandomTexDir, szPath);
        else if (fallbackStyle == 4)
            gotFile = PickRandomTextureFile(m_szFallbackTexturesDir, szPath);
        else if (fallbackStyle == 5 && m_szFallbackCustomFile[0])
            gotFile = CopyTo(szPath, m_szFallbackCustomFile);

        if (gotFile) {
            DX12Texture loaded = LoadTextureFromFile(szPath);
            if (loaded.resource) {
                m_fallbackTexture = loaded;
                return true;
            }
        }
        // File load failed — fall through to Hue Gradient (style 0)
        fallbackStyle = 0;
    }

    // Generate pixel data based on style
    UINT fbW = 1, fbH = 1;
    std::vector<UINT8> fbPixels;
    MakeFallbackPixels(fallbackStyle, fbW, fbH, fbPixels);

    // Create GPU texture resource
    D3D12_RESOURCE_DESC fbTexDesc = {};
    fbTexDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    fbTexDesc.Width = fbW;
    fbTexDesc.Height = fbH;
    fbTexDesc.DepthOrArraySize = 1;
    fbTexDesc.MipLevels = 1;
    fbTexDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    fbTexDesc.SampleDesc.Count = 1;
    fbTexDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &fbTexDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_fallbackTexture.resource));
    if (FAILED(hr)) return false;

    // Upload pixel data
    UINT rowPitch = ((fbW * 4 + 255) & ~255u);  // align to 256 bytes
    UINT64 uploadSize = (UINT64)rowPitch * fbH;
    if (uploadSize < 256) uploadSize = 256;

    D3D12_RESOURCE_DESC fbUploadDesc = {};
    fbUploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    fbUploadDesc.Width = uploadSize;
    fbUploadDesc.Height = 1;
    fbUploadDesc.DepthOrArraySize = 1;
    fbUploadDesc.MipLevels = 1;
    fbUploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    fbUploadDesc.SampleDesc.Count = 1;
    fbUploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBufFb;
    hr = m_device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &fbUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBufFb));
    if (FAILED(hr)) return false;

    UINT8* mappedFb = nullptr;
    uploadBufFb->Map(0, nullptr, (void**)&mappedFb);
    // Copy row by row (source stride = fbW*4, dest stride = rowPitch)
    for (UINT row = 0; row < fbH; row++)
        memcpy(mappedFb + row * rowPitch, fbPixels.data() + row * fbW * 4, fbW * 4);
    uploadBufFb->Unmap(0, nullptr);

    m_commandAllocators[0]->Reset();
    m_commandList->Reset(m_commandAllocators[0].Get(), nullptr);
    m_cmdListRecording = true;

    D3D12_TEXTURE_COPY_LOCATION srcFb = {};
    srcFb.pResource = uploadBufFb.Get();
    srcFb.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcFb.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcFb.PlacedFootprint.Footprint.Width = fbW;
    srcFb.PlacedFootprint.Footprint.Height = fbH;
    srcFb.PlacedFootprint.Footprint.Depth = 1;
    srcFb.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstFb = {};
    dstFb.pResource = m_fallbackTexture.resource.Get();
    dstFb.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstFb.SubresourceIndex = 0;

    m_commandList->CopyTextureRegion(&dstFb, 0, 0, 0, &srcFb, nullptr);

    D3D12_RESOURCE_BARRIER barrierFb = {};
    barrierFb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrierFb.Transition.pResource = m_fallbackTexture.resource.Get();
    barrierFb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrierFb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrierFb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrierFb);

    m_commandList->Close();
    m_cmdListRecording = false;
    ID3D12CommandList* listsFb[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, listsFb);

    m_fenceValues[0]++;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValues[0]);
    m_fence->SetEventOnCompletion(m_fenceValues[0], m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);

    D3D12_CPU_DESCRIPTOR_HANDLE fbSrvCpu = AllocateSrvCpu();
    m_fallbackTexture.srvIndex = PendingSrvSlot();

    CreateSRV2D(m_device.Get(), m_fallbackTexture.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                fbSrvCpu, GetSrvCpuHandleAtReadable(m_fallbackTexture.srvIndex));
    AllocateSrvGpu();

    m_fallbackTexture.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_fallbackTexture.width = fbW;
    m_fallbackTexture.height = fbH;
    m_fallbackTexture.depth = 1;
    m_fallbackTexture.format = DXGI_FORMAT_R8G8B8A8_UNORM;

    return true;
}

// ---------------------------------------------------------------------------
// CreateTextureFromPixels — synchronous GPU upload from CPU pixel data
// ---------------------------------------------------------------------------
DX12Texture DXContext::CreateTextureFromPixels(const void* pixels, UINT width, UINT height,
                                               UINT srcRowPitch, DXGI_FORMAT format,
                                               bool genMips)
{
    DX12Texture tex;
    if (!pixels || width == 0 || height == 0) return tex;

    UINT bytesPerPixel = 4; // BGRA / RGBA

    // Full box-filtered mip chain, built on the CPU. DX9's
    // D3DXCreateTextureFromFileEx generated mips for every disk texture, and
    // preset blur-by-LOD (tex2Dlod/tex2Dbias with a mip index) depends on
    // them: Heartfelt I's fog IS mips 2..7 of its background photo, and with
    // a single-mip texture the LOD clamps to the sharp image and the whole
    // fog/heart story silently disappears. mips[0] aliases the caller's
    // pixels; levels are tightly packed.
    UINT mipLevels = 1;
    std::vector<std::vector<BYTE>> mips;
    if (genMips) {
        while ((width >> mipLevels) > 0 || (height >> mipLevels) > 0)
            mipLevels++;
        mips.resize(mipLevels);
        UINT pw = width, ph = height;
        const BYTE* prev = (const BYTE*)pixels;
        UINT prevPitch = srcRowPitch;
        for (UINT l = 1; l < mipLevels; l++) {
            UINT w2 = max(1u, width >> l), h2 = max(1u, height >> l);
            mips[l].resize((size_t)w2 * h2 * bytesPerPixel);
            for (UINT y = 0; y < h2; y++) {
                UINT sy0 = min(2 * y, ph - 1), sy1 = min(2 * y + 1, ph - 1);
                for (UINT x = 0; x < w2; x++) {
                    UINT sx0 = min(2 * x, pw - 1), sx1 = min(2 * x + 1, pw - 1);
                    for (UINT c = 0; c < 4; c++) {
                        UINT sum = prev[sy0 * prevPitch + sx0 * 4 + c]
                                 + prev[sy0 * prevPitch + sx1 * 4 + c]
                                 + prev[sy1 * prevPitch + sx0 * 4 + c]
                                 + prev[sy1 * prevPitch + sx1 * 4 + c];
                        mips[l][((size_t)y * w2 + x) * 4 + c] = (BYTE)(sum / 4);
                    }
                }
            }
            prev = mips[l].data();
            prevPitch = w2 * bytesPerPixel;
            pw = w2; ph = h2;
        }
    }

    // 1. Create the GPU texture resource
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = (UINT16)mipLevels;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&tex.resource));
    if (FAILED(hr)) return tex;

    // 2. Per-level placed footprints (rows 256-aligned, level offsets 512-aligned)
    struct LevelFoot { UINT w, h, pitch; UINT64 offset; };
    std::vector<LevelFoot> feet(mipLevels);
    UINT64 uploadSize = 0;
    for (UINT l = 0; l < mipLevels; l++) {
        UINT wl = max(1u, width >> l), hl = max(1u, height >> l);
        UINT pitch = (wl * bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                     & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        uploadSize = (uploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
                     & ~((UINT64)D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        feet[l] = { wl, hl, pitch, uploadSize };
        uploadSize += (UINT64)pitch * hl;
    }
    UINT alignedRowPitch = feet[0].pitch;

    // 3. Create upload buffer
    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> uploadBuf;
    hr = m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuf));
    if (FAILED(hr)) { tex.resource.Reset(); return tex; }

    // 4. Map and copy pixel rows with alignment padding, all mip levels
    UINT8* mapped = nullptr;
    uploadBuf->Map(0, nullptr, (void**)&mapped);
    for (UINT l = 0; l < mipLevels; l++) {
        const UINT8* src = (l == 0) ? (const UINT8*)pixels : mips[l].data();
        UINT srcPitch = (l == 0) ? srcRowPitch : feet[l].w * bytesPerPixel;
        UINT copyPitch = feet[l].w * bytesPerPixel;
        for (UINT y = 0; y < feet[l].h; y++) {
            memcpy(mapped + feet[l].offset + (UINT64)y * feet[l].pitch,
                   src + (UINT64)y * srcPitch, copyPitch);
        }
    }
    uploadBuf->Unmap(0, nullptr);

    // 5. Record copies + barrier on the dedicated upload command list
    m_uploadAllocator->Reset();
    m_uploadCommandList->Reset(m_uploadAllocator.Get(), nullptr);

    for (UINT l = 1; l < mipLevels; l++) {
        D3D12_TEXTURE_COPY_LOCATION s = {};
        s.pResource = uploadBuf.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Offset = feet[l].offset;
        s.PlacedFootprint.Footprint.Format = format;
        s.PlacedFootprint.Footprint.Width = feet[l].w;
        s.PlacedFootprint.Footprint.Height = feet[l].h;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = feet[l].pitch;
        D3D12_TEXTURE_COPY_LOCATION d = {};
        d.pResource = tex.resource.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = l;
        m_uploadCommandList->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    }

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuf.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = feet[0].offset;
    srcLoc.PlacedFootprint.Footprint.Format = format;
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = tex.resource.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    m_uploadCommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_uploadCommandList->ResourceBarrier(1, &barrier);

    m_uploadCommandList->Close();
    ID3D12CommandList* lists[] = { m_uploadCommandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // 6. Wait synchronously for the upload to finish (dedicated fence, 5s timeout)
    m_uploadFenceValue++;
    m_commandQueue->Signal(m_uploadFence.Get(), m_uploadFenceValue);
    m_uploadFence->SetEventOnCompletion(m_uploadFenceValue, m_uploadFenceEvent);
    DWORD waitResult = WaitForSingleObjectEx(m_uploadFenceEvent, 5000, FALSE);
    if (waitResult == WAIT_TIMEOUT) {
        DebugLogA("DX12: CreateTextureFromPixels TIMEOUT (5s) — possible GPU hang", LOG_ERROR);
        tex.resource.Reset();
        return tex;
    }

    // 7. Create SRV descriptor (exposing the whole mip chain when present)
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = AllocateSrvCpu();
    tex.srvIndex = PendingSrvSlot();

    CreateSRV2D(m_device.Get(), tex.resource.Get(), format, srvCpu,
                GetSrvCpuHandleAtReadable(tex.srvIndex), mipLevels);
    AllocateSrvGpu(); // bump counter

    tex.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    tex.width = width;
    tex.height = height;
    tex.depth = 1;
    tex.format = format;

    return tex;
}

// ---------------------------------------------------------------------------
// CreateVolumeTextureFromPixels — synchronous GPU upload of 3D volume texture
// ---------------------------------------------------------------------------
DX12Texture DXContext::CreateVolumeTextureFromPixels(const void* pixels, UINT width, UINT height, UINT depth,
                                                      UINT srcRowPitch, DXGI_FORMAT format)
{
    DX12Texture tex;
    if (!pixels || width == 0 || height == 0 || depth == 0) return tex;

    // 1. Create the GPU texture resource (TEXTURE3D)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = (UINT16)depth;
    texDesc.MipLevels        = 1;
    texDesc.Format           = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&tex.resource));
    if (FAILED(hr)) return tex;

    // 2. Compute aligned row pitch and slice pitch
    UINT bytesPerPixel   = 4; // BGRA / RGBA
    UINT alignedRowPitch = (width * bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                           & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT slicePitch      = alignedRowPitch * height;
    UINT64 uploadSize    = (UINT64)slicePitch * depth;

    // 3. Create upload buffer
    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = uploadSize;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> uploadBuf;
    hr = m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuf));
    if (FAILED(hr)) { tex.resource.Reset(); return tex; }

    // 4. Map and copy pixel data slice-by-slice, row-by-row with pitch alignment
    UINT8* mapped = nullptr;
    uploadBuf->Map(0, nullptr, (void**)&mapped);
    const UINT8* src = (const UINT8*)pixels;
    UINT copyPitch = width * bytesPerPixel;
    UINT srcSlicePitch = srcRowPitch * height;
    for (UINT z = 0; z < depth; z++) {
        for (UINT y = 0; y < height; y++) {
            memcpy(mapped + z * slicePitch + y * alignedRowPitch,
                   src + z * srcSlicePitch + y * srcRowPitch,
                   copyPitch);
        }
    }
    uploadBuf->Unmap(0, nullptr);

    // 5. Record copy + barrier on the dedicated upload command list
    m_uploadAllocator->Reset();
    m_uploadCommandList->Reset(m_uploadAllocator.Get(), nullptr);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource                               = uploadBuf.Get();
    srcLoc.Type                                    = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset                  = 0;
    srcLoc.PlacedFootprint.Footprint.Format        = format;
    srcLoc.PlacedFootprint.Footprint.Width         = width;
    srcLoc.PlacedFootprint.Footprint.Height        = height;
    srcLoc.PlacedFootprint.Footprint.Depth         = depth;
    srcLoc.PlacedFootprint.Footprint.RowPitch      = alignedRowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = tex.resource.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    m_uploadCommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = tex.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_uploadCommandList->ResourceBarrier(1, &barrier);

    m_uploadCommandList->Close();
    ID3D12CommandList* lists[] = { m_uploadCommandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // 6. Wait synchronously for the upload to finish (dedicated fence, 5s timeout)
    m_uploadFenceValue++;
    m_commandQueue->Signal(m_uploadFence.Get(), m_uploadFenceValue);
    m_uploadFence->SetEventOnCompletion(m_uploadFenceValue, m_uploadFenceEvent);
    DWORD waitResult = WaitForSingleObjectEx(m_uploadFenceEvent, 5000, FALSE);
    if (waitResult == WAIT_TIMEOUT) {
        DebugLogA("DX12: CreateVolumeTextureFromPixels TIMEOUT (5s) — possible GPU hang", LOG_ERROR);
        tex.resource.Reset();
        return tex;
    }

    // 7. Create SRV descriptor (TEXTURE3D)
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = AllocateSrvCpu();
    tex.srvIndex = PendingSrvSlot();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                        = format;
    srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture3D.MipLevels           = 1;
    srvDesc.Texture3D.MostDetailedMip     = 0;
    srvDesc.Texture3D.ResourceMinLODClamp = 0.0f;
    m_device->CreateShaderResourceView(tex.resource.Get(), &srvDesc, srvCpu);
    // The readable mirror (forgejo#16) -- see CreateSRV2D's comment.
    m_device->CreateShaderResourceView(tex.resource.Get(), &srvDesc,
                                       GetSrvCpuHandleAtReadable(tex.srvIndex));
    AllocateSrvGpu(); // bump counter

    tex.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    tex.width  = width;
    tex.height = height;
    tex.depth  = depth;
    tex.format = format;

    return tex;
}

// ---------------------------------------------------------------------------
// DecodeDDS / DecodeTGA — the two formats WIC hands back nothing for (#319)
//
// `texture_exts` offers dds and tga alongside jpg and png, so the search finds
// such a file, the load returns empty, and the caller reports "texture NOT
// found" about a file that is sitting right there. Two different holes:
//
//   TGA   Windows ships no TGA codec at all — WINCODEC_ERR_COMPONENTNOTFOUND.
//   DDS   there IS a DDS codec, and it rejects 24bpp with BADHEADER. It maps a
//         DDS onto a DXGI format, and DXGI has no 24-bit format to map to. A
//         block-compressed or 32bpp DDS goes through WIC fine, so only the
//         uncompressed mask-described flavours are handled here.
//
// Both decode to tightly packed BGRA, which is what the WIC path produces and
// what CreateTextureFromPixels expects, so there is one upload path and disk
// textures keep their mip chain either way.
// ---------------------------------------------------------------------------
namespace {

bool ReadWholeFile(const wchar_t* path, std::vector<BYTE>& out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 &&
              size.QuadPart < (LONGLONG)256 * 1024 * 1024;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) &&
             got == out.size();
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// A channel mask describes where a component sits and how wide it is. Widths
// other than 8 have to be rescaled so that "all ones" comes out as 255 rather
// than as whatever the narrow value happened to be.
struct Channel {
    // Not named `max`: windows.h defines that as a macro unless NOMINMAX is set.
    UINT mask = 0, shift = 0, maxVal = 0;
    void Set(UINT m) {
        mask = m;
        if (!m) return;
        while (!((m >> shift) & 1)) shift++;
        maxVal = m >> shift;
    }
    BYTE Get(UINT px) const {
        if (!mask) return 255;
        const UINT v = (px & mask) >> shift;
        return maxVal == 255 ? (BYTE)v : (BYTE)((v * 255 + maxVal / 2) / maxVal);
    }
};

bool DecodeDDS(const std::vector<BYTE>& f, std::vector<BYTE>& bgra,
               UINT& outW, UINT& outH, const char** why)
{
    *why = nullptr;
    if (f.size() < 128 || memcmp(f.data(), "DDS ", 4) != 0) { *why = "not a DDS"; return false; }

    UINT hdrSize, flags, h, w, pitchOrLinear;
    memcpy(&hdrSize, &f[4], 4);  memcpy(&flags, &f[8], 4);
    memcpy(&h, &f[12], 4);       memcpy(&w, &f[16], 4);
    memcpy(&pitchOrLinear, &f[20], 4);
    if (hdrSize != 124) { *why = "header size is not 124"; return false; }
    if (!w || !h || w > 16384 || h > 16384) { *why = "implausible dimensions"; return false; }

    UINT pfFlags, bits, rm, gm, bm, am;
    memcpy(&pfFlags, &f[80], 4);  memcpy(&bits, &f[88], 4);
    memcpy(&rm, &f[92], 4);       memcpy(&gm, &f[96], 4);
    memcpy(&bm, &f[100], 4);      memcpy(&am, &f[104], 4);

    if (pfFlags & 0x4) { *why = "block-compressed (WIC's job)"; return false; }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        *why = "unsupported bit depth"; return false;
    }

    // DDPF_LUMINANCE and DDPF_ALPHA carry no usable RGB masks; treat a single
    // 8-bit channel as grey so a height/mask texture still arrives.
    const bool luminance = (pfFlags & 0x20000) != 0 || ((pfFlags & 0x2) && bits == 8);
    if (!(pfFlags & 0x40) && !luminance) { *why = "no RGB or luminance channels"; return false; }

    Channel cr, cg, cb, ca;
    if (luminance) { cr.Set(rm ? rm : 0xFF); cg = cb = cr; ca.Set(am); }
    else           { cr.Set(rm); cg.Set(gm); cb.Set(bm); ca.Set(am); }

    const UINT bpp = bits / 8;
    // DDSD_PITCH says the stride is explicit; otherwise rows are tightly packed.
    const UINT pitch = ((flags & 0x8) && pitchOrLinear) ? pitchOrLinear : w * bpp;
    // 'DX10' would put another 20 bytes here, but that is always a fourCC and
    // fourCC was rejected above, so the data starts right after the header.
    const size_t start = 128;
    if (pitch < w * bpp || start + (size_t)pitch * h > f.size()) {
        *why = "truncated pixel data"; return false;
    }

    bgra.resize((size_t)w * h * 4);
    for (UINT y = 0; y < h; y++) {
        const BYTE* src = f.data() + start + (size_t)pitch * y;
        BYTE* dst = bgra.data() + (size_t)w * 4 * y;
        for (UINT x = 0; x < w; x++, src += bpp, dst += 4) {
            UINT px = 0;
            memcpy(&px, src, bpp);          // little-endian, fewer bytes than a UINT
            dst[0] = cb.Get(px);
            dst[1] = cg.Get(px);
            dst[2] = cr.Get(px);
            dst[3] = am ? ca.Get(px) : 255;
        }
    }
    outW = w; outH = h;
    return true;
}

bool DecodeTGA(const std::vector<BYTE>& f, std::vector<BYTE>& bgra,
               UINT& outW, UINT& outH, const char** why)
{
    *why = nullptr;
    if (f.size() < 18) { *why = "shorter than a TGA header"; return false; }

    const BYTE idLen = f[0], cmapType = f[1], imgType = f[2];
    UINT16 cmapLen; memcpy(&cmapLen, &f[5], 2);
    const BYTE cmapBits = f[7];
    UINT16 w16, h16; memcpy(&w16, &f[12], 2); memcpy(&h16, &f[14], 2);
    const BYTE bits = f[16], desc = f[17];
    const UINT w = w16, h = h16;

    // 2/10 truecolour, 3/11 greyscale; the 10s and 11s are run-length encoded.
    const bool rle  = (imgType == 10 || imgType == 11);
    const bool grey = (imgType == 3  || imgType == 11);
    if (imgType != 2 && imgType != 3 && imgType != 10 && imgType != 11) {
        *why = "colour-mapped or unknown image type"; return false;
    }
    if (!w || !h || w > 16384 || h > 16384) { *why = "implausible dimensions"; return false; }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        *why = "unsupported bit depth"; return false;
    }

    size_t p = 18 + (size_t)idLen;
    if (cmapType) p += (size_t)cmapLen * ((cmapBits + 7) / 8);
    if (p >= f.size()) { *why = "no pixel data after the header"; return false; }

    const UINT bpp = bits / 8;
    const size_t count = (size_t)w * h;
    bgra.assign(count * 4, 0);

    // One pixel, whatever the depth, expanded into BGRA.
    auto emit = [&](const BYTE* s, BYTE* d) {
        if (grey)            { d[0] = d[1] = d[2] = s[0]; d[3] = 255; }
        else if (bits == 16) {                       // A1R5G5B5, little-endian
            const UINT v = (UINT)s[0] | ((UINT)s[1] << 8);
            d[0] = (BYTE)(((v & 0x001F) * 255 + 15) / 31);
            d[1] = (BYTE)((((v >> 5) & 0x1F) * 255 + 15) / 31);
            d[2] = (BYTE)((((v >> 10) & 0x1F) * 255 + 15) / 31);
            // Bit 15 is a 1-bit alpha that writers set inconsistently, and a
            // texture that came out fully transparent would read as a bug.
            d[3] = 255;
        }
        else { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = (bits == 32) ? s[3] : 255; }
    };

    size_t done = 0;
    if (!rle) {
        if (p + count * bpp > f.size()) { *why = "truncated pixel data"; return false; }
        for (; done < count; done++) emit(&f[p + done * bpp], &bgra[done * 4]);
    } else {
        while (done < count) {
            if (p >= f.size()) { *why = "truncated RLE stream"; return false; }
            const BYTE packet = f[p++];
            const UINT run = (packet & 0x7F) + 1u;
            if (done + run > count) { *why = "RLE run overruns the image"; return false; }
            if (packet & 0x80) {                      // one pixel, repeated
                if (p + bpp > f.size()) { *why = "truncated RLE run"; return false; }
                for (UINT i = 0; i < run; i++) emit(&f[p], &bgra[(done + i) * 4]);
                p += bpp;
            } else {                                  // `run` literal pixels
                if (p + (size_t)run * bpp > f.size()) { *why = "truncated RLE literals"; return false; }
                for (UINT i = 0; i < run; i++) emit(&f[p + (size_t)i * bpp], &bgra[(done + i) * 4]);
                p += (size_t)run * bpp;
            }
            done += run;
        }
    }

    // Bit 5 of the descriptor means the first row stored is the TOP row. It is
    // clear far more often than not, and getting it wrong flips the picture.
    if (!(desc & 0x20)) {
        std::vector<BYTE> row((size_t)w * 4);
        for (UINT y = 0; y < h / 2; y++) {
            BYTE* a = &bgra[(size_t)y * w * 4];
            BYTE* b = &bgra[(size_t)(h - 1 - y) * w * 4];
            memcpy(row.data(), a, row.size());
            memcpy(a, b, row.size());
            memcpy(b, row.data(), row.size());
        }
    }

    outW = w; outH = h;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// LoadTextureFromFile — WIC-based image loading to DX12 texture
// ---------------------------------------------------------------------------
// See the note on the declaration (#349). Clamped, not trusted: a tiny cap
// would make every texture a smear, and a negative one is meaningless. 0 is
// deliberately allowed through as "no cap".
static std::atomic<int> g_maxTextureDim{ 1024 };

void SetMaxTextureDim(int maxDim)
{
    if (maxDim < 0) maxDim = 0;
    if (maxDim > 0 && maxDim < 64) maxDim = 64;
    if (maxDim > 16384) maxDim = 16384;
    g_maxTextureDim.store(maxDim, std::memory_order_relaxed);
}

int GetMaxTextureDim()
{
    return g_maxTextureDim.load(std::memory_order_relaxed);
}

DX12Texture DXContext::LoadTextureFromFile(const wchar_t* szFilename)
{
    DX12Texture tex;
    if (!szFilename || !szFilename[0]) return tex;

    HRESULT hr;
    UINT w = 0, h = 0;
    std::vector<BYTE> pixels;         // BGRA, tightly packed
    const char* step = nullptr;       // which WIC call failed, for the log

    // 1. Create WIC factory
    ComPtr<IWICImagingFactory> wicFactory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) step = "CoCreateInstance(WICImagingFactory)";

    // 2. Create decoder from file
    ComPtr<IWICBitmapDecoder> decoder;
    if (!step) {
        hr = wicFactory->CreateDecoderFromFilename(szFilename, nullptr,
                                                    GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr)) step = "CreateDecoderFromFilename";
    }

    // 3. Get first frame
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!step) {
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) step = "GetFrame";
    }

    // 4. Convert to 32bpp BGRA
    ComPtr<IWICFormatConverter> converter;
    if (!step) {
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) step = "CreateFormatConverter";
    }
    if (!step) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) step = "FormatConverter::Initialize";
    }

    // 5. Get dimensions and copy pixels
    if (!step) {
        converter->GetSize(&w, &h);
        pixels.resize((size_t)w * 4 * h);
        hr = converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
        if (FAILED(hr)) step = "CopyPixels";
    }

    // 5b. WIC declined. It has no TGA codec at all, and its DDS codec rejects
    // 24bpp because no DXGI format matches — yet both extensions are offered by
    // texture_exts, so without this the caller says "texture NOT found" about a
    // file that exists and is perfectly good (#319).
    if (step) {
        const HRESULT wicHr = hr;
        const char* why = "unreadable";
        std::vector<BYTE> raw;
        bool got = false;
        if (ReadWholeFile(szFilename, raw)) {
            // A DDS says so in its first four bytes; a TGA has no magic number
            // at all, so it is only ever the second thing tried.
            const char* ddsWhy = nullptr;
            const char* tgaWhy = nullptr;
            const bool isDds = raw.size() >= 4 && memcmp(raw.data(), "DDS ", 4) == 0;
            got = isDds ? DecodeDDS(raw, pixels, w, h, &ddsWhy)
                        : DecodeTGA(raw, pixels, w, h, &tgaWhy);
            if (!got) why = isDds ? ddsWhy : tgaWhy;
            if (!why) why = "no decoder";
        } else {
            why = "could not be read";
        }

        if (!got) {
            DebugLogWFmt(LOG_WARN, L"DX12: texture load failed: %s (WIC %S hr=0x%08X, %S)",
                         szFilename, step, (unsigned)wicHr, why);
            return tex;
        }
        DebugLogWFmt(LOG_INFO, L"DX12: %s decoded without WIC (%ux%u; WIC %S hr=0x%08X)",
                     szFilename, w, h, step, (unsigned)wicHr);
    }

    // 5c. Cap the longest edge (#349).
    //
    // Nothing used to limit this, so a texture cost whatever the camera
    // produced. Measured on the live random-texture library: ~2,500 photos at
    // 4160x3120, which is 66 MB of VRAM EACH once mipped, whether the JPEG on
    // disk is 0.7 MB or 8.6 MB. A preset naming two to five of them cost
    // 100-300 MB, and because the selection is random the cache never scored a
    // hit -- it filled with one-shot textures and churned at its cap.
    //
    // A MilkDrop texture is sampled across a warped mesh at render resolution,
    // so anything above that is decoded, uploaded, mipped, evicted and reloaded
    // without ever reaching a pixel. Measured: capping that same photo to 1024
    // and sampling it 1:1 with no warp -- the hardest case for a downscale to
    // survive -- is BIT-IDENTICAL at 1200x1600 and at 2160x3840. Max absolute
    // difference zero across every channel.
    //
    // Box filter rather than anything cleverer: each destination pixel averages
    // exactly the source rect it covers, which is what a mip reduction does
    // anyway, and CreateTextureFromPixels builds the real chain from the result.
    if (GetMaxTextureDim() > 0 &&
        (w > (UINT)GetMaxTextureDim() || h > (UINT)GetMaxTextureDim())) {
        const UINT cap = (UINT)GetMaxTextureDim();
        const UINT longest = (w > h) ? w : h;
        UINT nw = (UINT)(((UINT64)w * cap) / longest);
        UINT nh = (UINT)(((UINT64)h * cap) / longest);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        std::vector<BYTE> shrunk((size_t)nw * 4 * nh);
        for (UINT y = 0; y < nh; y++) {
            const UINT sy0 = (UINT)(((UINT64)y * h) / nh);
            UINT sy1 = (UINT)(((UINT64)(y + 1) * h) / nh);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            for (UINT x = 0; x < nw; x++) {
                const UINT sx0 = (UINT)(((UINT64)x * w) / nw);
                UINT sx1 = (UINT)(((UINT64)(x + 1) * w) / nw);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                UINT32 acc[4] = { 0, 0, 0, 0 };
                UINT n = 0;
                for (UINT sy = sy0; sy < sy1 && sy < h; sy++) {
                    const BYTE* row = pixels.data() + (size_t)sy * w * 4;
                    for (UINT sx = sx0; sx < sx1 && sx < w; sx++) {
                        const BYTE* p = row + (size_t)sx * 4;
                        acc[0] += p[0]; acc[1] += p[1];
                        acc[2] += p[2]; acc[3] += p[3];
                        n++;
                    }
                }
                if (!n) n = 1;
                BYTE* d = shrunk.data() + ((size_t)y * nw + x) * 4;
                d[0] = (BYTE)(acc[0] / n); d[1] = (BYTE)(acc[1] / n);
                d[2] = (BYTE)(acc[2] / n); d[3] = (BYTE)(acc[3] / n);
            }
        }
        DebugLogWFmt(LOG_INFO, L"DX12: capped %s %ux%u -> %ux%u (%.1f MB -> %.1f MB)",
                     szFilename, w, h, nw, nh,
                     (double)w * h * 4 * 4 / 3 / 1048576.0,
                     (double)nw * nh * 4 * 4 / 3 / 1048576.0);
        pixels.swap(shrunk);
        w = nw;
        h = nh;
    }

    // 6. Upload via CreateTextureFromPixels (BGRA matches DXGI_FORMAT_B8G8R8A8_UNORM).
    // genMips: disk textures need the full chain for preset LOD-blur sampling.
    tex = CreateTextureFromPixels(pixels.data(), w, h, w * 4,
                                  DXGI_FORMAT_B8G8R8A8_UNORM, true);

    if (tex.resource) {
        char buf[512];
        sprintf(buf, "DX12: LoadTextureFromFile: %dx%d srv=%u", w, h, tex.srvIndex);
        DebugLogA(buf);
    }

    return tex;
}

void DXContext::CreateBindingBlockForTexture(DX12Texture& tex)
{
    if (m_nullTexture.srvIndex == UINT_MAX || tex.srvIndex == UINT_MAX) return;

    // A recycled block first (#312). Blocks must be contiguous, so they are
    // kept on their own free list and never satisfied from single slots.
    bool recycled = false;
    if (!m_freeBindingBlocks.empty()) {
        tex.bindingBlockStart = m_freeBindingBlocks.back();
        m_freeBindingBlocks.pop_back();
        recycled = true;
    } else {
        // Reserve 32 contiguous SRV slots for this texture's binding block
        if (m_nextFreeSrvSlot + BINDING_BLOCK_SIZE > DXC_MAX_SRV) {
            DebugLogA("ERROR: SRV heap overflow in CreateBindingBlockForTexture!", LOG_ERROR);
            return;
        }
        tex.bindingBlockStart = m_nextFreeSrvSlot;
    }

    // Sources are m_srvHeapCpu (readable mirror), not m_srvHeap -- reading
    // m_srvHeap's own CPU handles as a copy source is invalid D3D12 usage
    // (forgejo#16). dst stays on m_srvHeap: that's the actual binding block
    // a draw call later gets bound from.
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrvCpu;
    nullSrvCpu.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                     (SIZE_T)m_nullTexture.srvIndex * m_srvDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE texSrvCpu;
    texSrvCpu.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)tex.srvIndex * m_srvDescriptorSize;

    for (UINT i = 0; i < BINDING_BLOCK_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst;
        dst.ptr = m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                  (SIZE_T)(tex.bindingBlockStart + i) * m_srvDescriptorSize;
        // Slot 0 = the actual texture, slots 1-31 = null (safe placeholder)
        m_device->CopyDescriptorsSimple(1, dst, (i == 0) ? texSrvCpu : nullSrvCpu,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    // Only a FRESH block advances the high-water mark; a recycled one was
    // already accounted for when it was first handed out.
    if (!recycled)
        m_nextFreeSrvSlot += BINDING_BLOCK_SIZE;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetBindingBlockGpuHandle(const DX12Texture& tex)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    gpuHandle.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)tex.bindingBlockStart * m_srvDescriptorSize;
    return gpuHandle;
}

UINT DXContext::CreateBindingBlockAtSlot(const DX12Texture& tex, UINT mainSlot)
{
    if (m_nullTexture.srvIndex == UINT_MAX || tex.srvIndex == UINT_MAX)
        return UINT_MAX;
    if (mainSlot >= BINDING_BLOCK_SIZE) mainSlot = 0;

    UINT blockStart = m_nextFreeSrvSlot;

    // Sources are m_srvHeapCpu, not m_srvHeap -- forgejo#16, see
    // CreateBindingBlockForTexture's comment just above.
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrvCpu;
    nullSrvCpu.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                     (SIZE_T)m_nullTexture.srvIndex * m_srvDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE texSrvCpu;
    texSrvCpu.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)tex.srvIndex * m_srvDescriptorSize;

    for (UINT i = 0; i < BINDING_BLOCK_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst;
        dst.ptr = m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                  (SIZE_T)m_nextFreeSrvSlot * m_srvDescriptorSize;
        m_device->CopyDescriptorsSimple(1, dst, (i == mainSlot) ? texSrvCpu : nullSrvCpu,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_nextFreeSrvSlot++;
    }

    char buf[128];
    sprintf(buf, "DX12: CreateBindingBlockAtSlot: tex srv=%u at slot %u, blockStart=%u",
            tex.srvIndex, mainSlot, blockStart);
    DebugLogA(buf);
    return blockStart;
}

void DXContext::FillSrvBindingBlock(UINT baseIndex, const UINT slots[32])
{
    if (!m_device || !m_srvHeap || !m_srvHeapCpu || baseIndex == UINT_MAX)
        return;
    // dst on m_srvHeap (the binding block a draw call gets bound from); src
    // on m_srvHeapCpu, its readable mirror -- forgejo#16.
    D3D12_CPU_DESCRIPTOR_HANDLE heapStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE heapStartReadable = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrc;
    nullSrc.ptr = heapStartReadable.ptr + (SIZE_T)m_nullTexture.srvIndex * m_srvDescriptorSize;
    for (UINT i = 0; i < BINDING_BLOCK_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst;
        dst.ptr = heapStart.ptr + (SIZE_T)(baseIndex + i) * m_srvDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE src;
        if (slots && slots[i] != UINT_MAX)
            src.ptr = heapStartReadable.ptr + (SIZE_T)slots[i] * m_srvDescriptorSize;
        else
            src = nullSrc;
        m_device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetSrvBlockGpuHandle(UINT baseIndex)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = {};
    if (m_srvHeap && baseIndex != UINT_MAX)
        h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
                (SIZE_T)baseIndex * m_srvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetBindingBlockGpuHandleByIndex(UINT blockStart)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    gpuHandle.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)blockStart * m_srvDescriptorSize;
    return gpuHandle;
}

bool DXContext::AllocatePerFrameBindings()
{
    // Reserve 96 contiguous SRV slots: 2 frames × 3 passes (warp+bufferA+comp) × 16 descriptors
    m_perFrameBindingBase = m_nextFreeSrvSlot;
    m_nextFreeSrvSlot += DXC_FRAME_COUNT * PASSES_PER_FRAME * BINDING_BLOCK_SIZE;
    char buf[128];
    sprintf(buf, "DX12: AllocatePerFrameBindings: base=%u (reserved %u slots)",
            m_perFrameBindingBase, DXC_FRAME_COUNT * PASSES_PER_FRAME * BINDING_BLOCK_SIZE);
    DebugLogA(buf);
    return true;
}

void DXContext::UpdatePerFrameBindings(const UINT warpSrvSlots[32], const UINT bufferASrvSlots[32],
                                      const UINT bufferBSrvSlots[32], const UINT bufferCSrvSlots[32],
                                      const UINT bufferDSrvSlots[32], const UINT compSrvSlots[32],
                                      const UINT oldWarpSrvSlots[32], const UINT oldCompSrvSlots[32])
{
    if (m_perFrameBindingBase == UINT_MAX) return;

    // Each frame gets 256 slots: [0..31] = warp, [32..63] = bufferA, [64..95] = bufferB,
    // [96..127] = bufferC, [128..159] = bufferD, [160..191] = comp, [192..223] = oldWarp, [224..255] = oldComp
    UINT frameBase = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE;

    // Sources are m_srvHeapCpu, not m_srvHeap -- forgejo#16. This runs every
    // frame (8 passes x 32 slots = 256 copies), so it was the single largest
    // contributor to the ~80,000 debug-layer errors an 8-second run produced.
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrc;
    nullSrc.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                  (SIZE_T)m_nullTexture.srvIndex * m_srvDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE heapStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE heapStartReadable = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart();

    // Helper to fill a 32-slot binding block
    auto fillBlock = [&](UINT base, const UINT slots[32]) {
        for (UINT i = 0; i < BINDING_BLOCK_SIZE; i++) {
            D3D12_CPU_DESCRIPTOR_HANDLE dst;
            dst.ptr = heapStart.ptr + (SIZE_T)(base + i) * m_srvDescriptorSize;
            D3D12_CPU_DESCRIPTOR_HANDLE src;
            if (slots[i] != UINT_MAX)
                src.ptr = heapStartReadable.ptr + (SIZE_T)slots[i] * m_srvDescriptorSize;
            else
                src = nullSrc;
            m_device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    };

    fillBlock(frameBase, warpSrvSlots);                              // pass 0: warp
    fillBlock(frameBase + BINDING_BLOCK_SIZE, bufferASrvSlots);      // pass 1: bufferA
    fillBlock(frameBase + 2 * BINDING_BLOCK_SIZE, bufferBSrvSlots);  // pass 2: bufferB
    fillBlock(frameBase + 3 * BINDING_BLOCK_SIZE, bufferCSrvSlots);  // pass 3: bufferC
    fillBlock(frameBase + 4 * BINDING_BLOCK_SIZE, bufferDSrvSlots);  // pass 4: bufferD
    fillBlock(frameBase + 5 * BINDING_BLOCK_SIZE, compSrvSlots);     // pass 5: comp
    fillBlock(frameBase + 6 * BINDING_BLOCK_SIZE, oldWarpSrvSlots);  // pass 6: oldWarp
    fillBlock(frameBase + 7 * BINDING_BLOCK_SIZE, oldCompSrvSlots);  // pass 7: oldComp
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetWarpBindingGpuHandle()
{
    UINT slot = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetBufferABindingGpuHandle()
{
    UINT slot = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE + BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetBufferBBindingGpuHandle()
{
    UINT slot = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE + 2 * BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetBufferCBindingGpuHandle()
{
    UINT slot = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE + 3 * BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetBufferDBindingGpuHandle()
{
    UINT slot = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE + 4 * BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetCompBindingGpuHandle()
{
    UINT slot = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE + 5 * BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetOldWarpBindingGpuHandle()
{
    UINT slot = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE + 6 * BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetOldCompBindingGpuHandle()
{
    UINT slot = m_perFrameBindingBase + m_frameIndex * PASSES_PER_FRAME * BINDING_BLOCK_SIZE + 7 * BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

// ---------------------------------------------------------------------------
// Per-frame blur bindings
// ---------------------------------------------------------------------------

bool DXContext::AllocateBlurBindings()
{
    // Reserve 2 frames × MAX_BLUR_PASSES × 16 SRV descriptors
    m_blurBindingBase = m_nextFreeSrvSlot;
    m_nextFreeSrvSlot += DXC_FRAME_COUNT * MAX_BLUR_PASSES * BINDING_BLOCK_SIZE;
    char buf[128];
    sprintf(buf, "DX12: AllocateBlurBindings: base=%u (reserved %u slots)",
            m_blurBindingBase, DXC_FRAME_COUNT * MAX_BLUR_PASSES * BINDING_BLOCK_SIZE);
    DebugLogA(buf, LOG_VERBOSE);
    return true;
}

void DXContext::UpdateBlurPassBinding(UINT passIndex, UINT sourceSrvIndex,
                                      UINT baseOverride)
{
    if (passIndex >= MAX_BLUR_PASSES) return;
    if (baseOverride == UINT_MAX && m_blurBindingBase == UINT_MAX) return;

    // A caller with its own blocks brings its own base, already carrying its
    // frame rotation -- and note m_frameIndex is NOT read on that path. It is
    // the render thread's, written non-atomically, so a mirror worker reading
    // it mid-record could bind a block it never wrote (#209).
    //
    // Each frame gets MAX_BLUR_PASSES x BINDING_BLOCK_SIZE slots.
    const UINT blockBase = (baseOverride != UINT_MAX)
        ? baseOverride + passIndex * BINDING_BLOCK_SIZE
        : m_blurBindingBase +
          m_frameIndex * MAX_BLUR_PASSES * BINDING_BLOCK_SIZE +
          passIndex * BINDING_BLOCK_SIZE;

    // Sources are m_srvHeapCpu, not m_srvHeap -- forgejo#16. Also runs every
    // frame (MAX_BLUR_PASSES x 32 slots), the second-largest contributor
    // alongside UpdatePerFrameBindings.
    D3D12_CPU_DESCRIPTOR_HANDLE nullSrc;
    nullSrc.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                  (SIZE_T)m_nullTexture.srvIndex * m_srvDescriptorSize;

    for (UINT i = 0; i < BINDING_BLOCK_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst;
        dst.ptr = m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                  (SIZE_T)(blockBase + i) * m_srvDescriptorSize;

        if (i == 0 && sourceSrvIndex != UINT_MAX) {
            // Slot 0 = source texture (t0).
            // SM5.0 backwards compat assigns textures sequentially from t0 regardless
            // of sampler register annotations. The blur shader's single texture always
            // lands at t0. Sampler addressing (CLAMP at s12) is separate.
            D3D12_CPU_DESCRIPTOR_HANDLE src;
            src.ptr = m_srvHeapCpu->GetCPUDescriptorHandleForHeapStart().ptr +
                      (SIZE_T)sourceSrvIndex * m_srvDescriptorSize;
            m_device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        } else {
            m_device->CopyDescriptorsSimple(1, dst, nullSrc, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE DXContext::GetBlurPassBindingGpuHandle(UINT passIndex,
                                                                   UINT baseOverride)
{
    UINT slot = (baseOverride != UINT_MAX)
        ? baseOverride + passIndex * BINDING_BLOCK_SIZE
        : m_blurBindingBase +
          m_frameIndex * MAX_BLUR_PASSES * BINDING_BLOCK_SIZE +
          passIndex * BINDING_BLOCK_SIZE;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * m_srvDescriptorSize;
    return h;
}

// ---------------------------------------------------------------------------
// Pipeline state objects (Phase 3)
// ---------------------------------------------------------------------------

bool DXContext::CreatePipelines()
{
    return DX12CreatePipelines(m_device.Get(), m_rootSignature.Get(),
                                k_BackBufferFormat, m_PSOs);
}

// ---------------------------------------------------------------------------

void DXContext::WriteSafeWindowPos()
{
    ConfigFile(m_szIniFile).SetInt(L"Settings", L"nMainWndTop", 64);
    ConfigFile(m_szIniFile).SetInt(L"Settings", L"nMainWndLeft", 64);
    ConfigFile(m_szIniFile).SetInt(L"Settings", L"nMainWndRight", 64 + 256);
    ConfigFile(m_szIniFile).SetInt(L"Settings", L"nMainWndBottom", 64 + 256);
}

bool DXContext::OnUserResizeWindow(RECT* new_window_rect, RECT* new_client_rect, bool bSetBackBuffer)
{
    if (!m_ready) return false;

    int newW = new_client_rect->right  - new_client_rect->left;
    int newH = new_client_rect->bottom - new_client_rect->top;

    if (newW == m_client_width && newH == m_client_height &&
        (new_window_rect->right - new_window_rect->left) == m_window_width &&
        (new_window_rect->bottom - new_window_rect->top) == m_window_height)
    {
        return true;
    }

    m_window_width  = new_window_rect->right  - new_window_rect->left;
    m_window_height = new_window_rect->bottom - new_window_rect->top;

    if (bSetBackBuffer) {
        return ResizeSwapChain(newW, newH);
    }

    m_client_width  = m_REAL_client_width  = newW;
    m_client_height = m_REAL_client_height = newH;
    return true;
}
