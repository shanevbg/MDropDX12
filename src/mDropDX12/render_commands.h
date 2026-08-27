#pragma once

// ─── Render Command Queue ────────────────────────────────────────────────────
// Commands enqueued from the message pump thread (WndProc handlers) and
// processed by the render thread at the top of each frame.  This decouples
// GPU work from Windows message handling so that message storms don't block
// rendering.

#include <queue>
#include <mutex>
#include <string>

// Which code section of the running preset an ApplyPresetCode command targets.
// iParam2 carries the wave/shape index (0..MAX_CUSTOM_WAVES-1) for the indexed
// sections.  Note there is no SHAPE_PER_POINT: CShape has m_szInit and
// m_szPerFrame only -- its per-point buffer is commented out in state.h.
enum PresetCodeSection : int {
    PCS_PRESET_INIT = 0,
    PCS_PER_FRAME,
    PCS_PER_PIXEL,
    PCS_WAVE_INIT,
    PCS_WAVE_PER_FRAME,
    PCS_WAVE_PER_POINT,
    PCS_SHAPE_INIT,
    PCS_SHAPE_PER_FRAME,
    PCS_WARP_SHADER,
    PCS_COMP_SHADER,
    PCS_COUNT
};

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
    LoadPresetPath,      // LoadPreset(sParam, fParam); iParam1 >= 0 sets m_nCurrentPreset first
    NavPreset,           // iParam1 > 0 ? NextPreset(fParam) : PrevPreset(fParam)
    CaptureScreenshot,   // CaptureScreenshotWithFilename(sParam)
    IPCMessage,          // LaunchMessage(sParam)
    PushSprite,          // LaunchSprite(iParam1=sprite#, iParam2=slot)
    KillSprite,          // KillSprite(iParam1=slot)
    LoadShaders,         // LoadShaders + CreateDX12PresetPSOs
    RecompileCompShader, // Recompile comp shader from m_pState text + rebuild PSOs
    ApplyShaderOverride, // Recompile warp+comp for the running preset, honouring
                         // the active shader override (shader_overrides.h)
    DisableAllOutputs,   // Ctrl+F2 kill switch
    ResetPipeline,       // RecreateRootSigAndPipelines() + ResetBufferAndFonts()
    ApplyPresetCode,     // Preset Editor: copy sParam into the CState buffer named
                         // by iParam1 (PresetCodeSection) / iParam2 (wave or shape
                         // index) and fire the matching OnUserEdited* recompile.
    ApplyPresetText,     // Preset Editor whole-file edit: sParam is complete
                         // .milk text.  Written to a temp file and re-imported,
                         // because CState::Import is the only thing that knows
                         // how to read the scalars back.
    SavePresetFile,      // Preset Editor: CState::Export(sParam).  Export reads
                         // every CState buffer, which the render thread rewrites
                         // on preset load, so it cannot run on a UI thread.
    ReallocCanvas,       // CleanUpDX9Stuff(0) + AllocateDX9Stuff(): rebuild the
                         // feedback canvas after a canvas-limit change. Must go
                         // through the queue -- tearing down DX12 resources from
                         // the message pump while the render thread is mid-frame
                         // is a crash, and OnUserResizeWindow cannot be reused
                         // because it early-outs unless the WINDOW size changed.
    Quit,                // Clean shutdown
};

// Which preset an ApplyPresetCode command targets. A frozen .milk2 keeps two
// presets on screen at once: the live one and the blend-from one, so an edit
// has to say which it means.
enum PresetSide : int {
    PSIDE_LIVE      = 0,   // m_pState  -- the only preset for a .milk/.milk3,
                           // and preset 2 (blend-to) for a .milk2
    PSIDE_BLENDFROM = 1,   // m_pOldState -- preset 1 of a frozen .milk2
};

struct RenderCommand {
    RenderCmd cmd;
    int iParam1 = 0;
    int iParam2 = 0;
    int iParam3 = 0;   // ApplyPresetCode: a PresetSide value
    float fParam = 0.0f;
    std::wstring sParam;
};
