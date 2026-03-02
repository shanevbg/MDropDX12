#pragma once

// ─── Render Command Queue ────────────────────────────────────────────────────
// Commands enqueued from the message pump thread (WndProc handlers) and
// processed by the render thread at the top of each frame.  This decouples
// GPU work from Windows message handling so that message storms don't block
// rendering.

#include <queue>
#include <mutex>
#include <string>

enum class RenderCmd : uint8_t {
    ResetBuffers,        // ResetBufferAndFonts()
    ResizeWindow,        // OnUserResizeWindow()
    DeviceRecovery,      // PerformDeviceRecovery()
    ToggleSpout,         // ToggleSpout()
    SpoutFixedSize,      // SetSpoutFixedSize(iParam1)
    RefreshDisplays,     // EnumerateDisplayOutputs + RefreshDisplaysTab
    NextPreset,          // LoadRandomPreset(fParam)
    PrevPreset,          // PrevPreset(fParam)
    LoadPreset,          // LoadPresetByIndex(iParam1, fParam)
    CaptureScreenshot,   // CaptureScreenshotWithFilename(sParam)
    IPCMessage,          // LaunchMessage(sParam)
    PushSprite,          // LaunchSprite(iParam1=sprite#, iParam2=slot)
    KillSprite,          // KillSprite(iParam1=slot)
    LoadShaders,         // LoadShaders + CreateDX12PresetPSOs
    DisableAllOutputs,   // Ctrl+F2 kill switch
    Quit,                // Clean shutdown
};

struct RenderCommand {
    RenderCmd cmd;
    int iParam1 = 0;
    int iParam2 = 0;
    float fParam = 0.0f;
    std::wstring sParam;
};
