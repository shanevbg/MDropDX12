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

    // Spout-specific
    bool bFixedSize = false;
    int nWidth = 1920;
    int nHeight = 1080;
};

// Runtime state for a monitor mirror output
struct MonitorMirrorState {
    HWND hWnd = nullptr;
    ComPtr<IDXGISwapChain4> swapChain;
    ComPtr<ID3D12Resource> backBuffers[DXC_FRAME_COUNT];
    int width = 0;
    int height = 0;
    bool bReady = false;
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
