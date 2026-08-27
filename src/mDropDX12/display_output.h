#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <vector>
#include "SpoutDX12.h"

// Forward declaration
#ifndef DXC_FRAME_COUNT
#define DXC_FRAME_COUNT 2
#endif

using Microsoft::WRL::ComPtr;

// ─── Display Output Types ─────────────────────────────────────────────────────

enum class DisplayOutputType { Monitor, Spout };

// Persisted configuration for a single display output
struct DisplayOutputConfig {
    DisplayOutputType type = DisplayOutputType::Spout;
    bool bEnabled = false;
    wchar_t szName[128] = {};          // Friendly name (monitors) or sender name (Spout)

    // Monitor-specific
    wchar_t szDeviceName[32] = {};     // e.g. L"\\\\.\\DISPLAY2"
    bool bFullscreen = true;
    RECT rcMonitor = {};               // Cached monitor rect
    int nOpacity = 100;                // 1-100%; per-mirror opacity
    bool bClickThrough = false;        // Per-mirror click-through
    // When true: same-orient blits primary; opposite orientation runs a lagged
    // warp+comp (classic/milk2) or Image (milk3) at panel aspect. Not frame-locked.
    // When false: copy/stretch main. Audio is always shared.
    bool bIndependentRender = false;

    // Spout-specific
    bool bFixedSize = false;
    int nWidth = 1920;
    int nHeight = 1080;
};

// Mirror swap chains use 3 buffers (MS multi-SC flip guidance); main stays at DXC_FRAME_COUNT.
#ifndef MIRROR_BUFFER_COUNT
#define MIRROR_BUFFER_COUNT 3
#endif

// Runtime state for a monitor mirror output
struct MonitorMirrorState {
    HWND hWnd = nullptr;
    ComPtr<IDXGISwapChain4> swapChain;
    ComPtr<ID3D12Resource> backBuffers[MIRROR_BUFFER_COUNT];
    // RTVs in the permanent high-heap reserve (survive resize / descriptor rewind)
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[MIRROR_BUFFER_COUNT] = {};
    UINT rtvSlotBase = UINT_MAX; // DXC_MIRROR_RTV_BASE + offset, or UINT_MAX
    UINT bufferCount = MIRROR_BUFFER_COUNT;
    UINT rtvEpoch = 0; // matches DXContext::m_descriptorEpoch after last CreateRTV
    bool bHasRtv = false;
    int width = 0;
    int height = 0;
    bool bReady = false;
    bool bIndependentSized = false; // SC sized to monitor native (not main BB)
    // After CreateSwapChain, buffers are COMMON; after first Present they are PRESENT.
    bool bEverPresented = false;
    // Soft-disable: hide + skip draws for N frames, then hard-destroy without
    // WaitForGpu (avoids freezes when toggling enable repeatedly).
    bool bSoftDisabled = false;
    int  nFramesUntilHardDestroy = 0;
    // CPU-tracked state of each flip face. A mid-frame exception used to leave a
    // face in RT while the next frame assumed PRESENT → illegal barrier forever.
    D3D12_RESOURCE_STATES bbState[MIRROR_BUFFER_COUNT] = {
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
    };
    // After enabling independent/orient (or recreate): flip-chain faces may still
    // hold letterboxed landscape frames. We cannot barrier non-current faces (TDR),
    // so clear+redraw the *current* face each frame until every buffer index has
    // been successfully Presented with the new mode. Bit i = buffer i is clean.
    // Stale letterbox on portrait is a ~1/3-height landscape strip (srcAr/dstAr).
    bool bNeedsFullChainClear = false;
    UINT paintedBufferMask = 0;
    // Deferred window/SC layout (fullscreen checkbox). Applied on render thread.
    bool bPendingLayout = false;
    int  pendingX = 0, pendingY = 0, pendingW = 0, pendingH = 0;

    // Orient-path present gating: last published disp[] index this panel drew,
    // and whether this frame drew fresh content (present only then). Presenting
    // identical content every render frame (~390/s for ~200 sim fps) is wasted
    // work and a flash suspect on high-rate interval-0 flips.
    unsigned lastPubSeq = 0;   // last m_orientPublishedSeq this panel drew
    bool bFreshDraw = false;

    // ── IPC / DIAG_MIRRORS (observability only; does not change draw path) ──
    // lastPath: 0=none 1=orient 2=stretchMain 3=letterbox 4=copy 5=black 6=orientFail
    int  lastPath = 0;
    UINT lastDrawFI = UINT_MAX;
    UINT lastPresentFI = UINT_MAX;
    HRESULT lastPresentHr = S_OK;
    UINT presentOkCount = 0;
    UINT presentFailCount = 0;
    UINT presentSkipCount = 0;
    UINT drawCount = 0;
    bool lastMustBlock = false;
    bool lastOppositeIndep = false;
    bool lastLayered = false;
};

// Runtime state for a Spout output
struct SpoutOutputState {
    spoutDX12 sender;
    ID3D11Resource* wrappedBackBuffers[DXC_FRAME_COUNT] = {};
    bool bReady = false;
};

// A single display output (monitor mirror or Spout sender)
struct DisplayOutput {
    DisplayOutputConfig config;

    // Runtime state — only one is active at a time, managed by type
    std::unique_ptr<MonitorMirrorState> monitorState;
    std::unique_ptr<SpoutOutputState>   spoutState;

    bool bSkippedSameMonitor = false;  // Mirror is on render window's monitor; cleared on window move
};
