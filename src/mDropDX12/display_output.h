#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <vector>
#include "spout_bridge.h"

// Forward declaration
#ifndef DXC_FRAME_COUNT
#define DXC_FRAME_COUNT 2
#endif

using Microsoft::WRL::ComPtr;

// ─── Display Output Types ─────────────────────────────────────────────────────

enum class DisplayOutputType { Monitor, Spout };

// ─── Preset mode sentinels ────────────────────────────────────────────────────
//
// szPresetDir and szStartupPreset below each hold EITHER a literal path OR one
// of these three modes. Angle brackets are reserved characters in a Windows
// filename, so a sentinel can never collide with a real path or preset name --
// which is what lets one field carry both without a second "mode" member to
// keep in sync with it.
//
// Resolved at spawn by Engine::ResolveDisplayPresetDir /
// ResolveDisplayStartupPreset; nothing downstream of those ever sees a
// sentinel. An EMPTY field is the pre-v3 form and still means what it always
// did: a dir inherits, a startup preset picks at random.
inline constexpr const wchar_t* kDispModeDefault = L"<Default>";
inline constexpr const wchar_t* kDispModeRandom  = L"<Random>";
inline constexpr const wchar_t* kDispModeCurrent = L"<Current>";
// Not a mode: an ACTION. Picking it opens the same browse dialog the "..."
// button does and replaces itself with whatever was chosen, so it is never
// stored and never reaches the resolvers. It is in the list because the list
// is where people look for "let me pick one" -- the button beside the field
// is easy to miss, and the field is otherwise the only route to a real path.
inline constexpr const wchar_t* kDispModeBrowse  = L"<Browse...>";

// Persisted configuration for a single display output
struct DisplayOutputConfig {
    DisplayOutputType type = DisplayOutputType::Spout;
    bool bEnabled = false;
    // NOT PERSISTED for monitors: the friendly name comes from the enumeration,
    // which reports the device present NOW. See rcMonitor for the contract.
    wchar_t szName[128] = {};          // Friendly name (monitors) or sender name (Spout)

    // Monitor-specific
    wchar_t szDeviceName[32] = {};     // e.g. L"\\\\.\\DISPLAY2"
    bool bFullscreen = true;
    // NOT PERSISTED. Filled by the monitor enumeration and absent from
    // displays.json, so a config built by the LOADER has it zeroed.
    //
    // That matters to anyone copying one config over another: assigning a
    // loaded config wholesale onto an enumerated entry zeroes this, and a
    // display with no rect cannot be placed -- the Displays window's layout
    // panel simply stopped drawing display 2 (#297). LoadDisplayOutputSettings
    // saves and restores it, along with szName and rcTestWindow; those three
    // are the entire set that does NOT come from the file.
    RECT rcMonitor = {};               // Cached monitor rect
    int nOpacity = 100;                // 1-100%; per-mirror opacity
    bool bClickThrough = false;        // Per-mirror click-through
    // Keep THIS display above other windows, independently of the global
    // always-on-top and of watermark mode (#276).
    //
    // Click-through without it is close to useless: the window cannot be
    // clicked, so nothing the user does will ever raise it, and the first
    // thing opened on that monitor buries it permanently. Watermark mode
    // forces on-top for exactly this reason, but that also forces
    // click-through and the watermark opacity, so there was no way to say
    // "on top, and otherwise leave it alone". The style pass's own comment
    // named the gap: the global flag "cannot express this one".
    bool bAlwaysOnTop = false;
    // Watermark this ONE display: click-through, always on top, and the
    // watermark opacity, without disturbing what the display was set to
    // otherwise.
    //
    // Watermark used to exist only as a global gesture that looped over every
    // monitor writing bClickThrough = true and nOpacity = the watermark value
    // straight into each config. That is why it could not be set per display
    // and why turning it on destroyed whatever click-through and opacity each
    // display already had -- there was no watermark STATE to turn off, only
    // two overwritten fields with no memory of what they had been.
    //
    // As its own flag it composes instead of clobbering: the style pass reads
    // it, and bClickThrough / nOpacity keep saying what the user chose for
    // when the watermark comes off again.
    bool bWatermark = false;
    // This display's watermark opacity, 1-100. -1 means "use the global
    // watermark opacity" (#276).
    //
    // Separate from nOpacity rather than replacing it, because the two answer
    // different questions and both have to survive: nOpacity is what the
    // display looks like NORMALLY and is what watermark mode restores on the
    // way out, so watermark cannot simply write its value there. Without a
    // second field the Opacity spinner edited nOpacity while the style pass
    // ignored it, which is exactly what "I can't adjust the opacity in
    // watermark mode" looked like from outside: a live control with no effect.
    int nWatermarkOpacity = -1;
    // When true: same-orient blits primary; opposite orientation runs a lagged
    // warp+comp (classic/milk2) or Image (milk3) at panel aspect. Not frame-locked.
    // When false: copy/stretch main. Audio is always shared.
    bool bIndependentRender = false;

    // Render this display with its own MDropDX12.exe -child process, so it
    // holds its OWN preset and cycles on its own timer (forgejo#22). Mutually
    // exclusive with bIndependentRender: that is the in-process mirror sim,
    // which re-renders the PRIMARY's preset at another orientation and cannot
    // hold a preset of its own.
    // This display shows a preset of its OWN rather than following the
    // primary. Says nothing about where it renders.
    //
    // Splitting this out is what makes a middle tier possible. The machinery
    // for an in-process surface to hold its own preset was built by #186 phase
    // 4 -- ownPresetPath, PostSurfacePreset, per-surface cycling and lock --
    // and it runs, but DisplayHoldsOwnPreset asked for bOwnProcess, so wanting
    // a different preset on a display always cost a whole second instance. The
    // in-process surface would pick the preset up, render it, and then stand
    // down as soon as the child was ready ("mirror stands down").
    //
    // Three tiers now, cheapest first:
    //   neither          mirror: follows the primary, one render and N blits
    //   bOwnPreset       its own preset, in process: a thread and a sim
    //   + bOwnProcess    its own preset, in its own instance
    bool bOwnPreset = false;

    // Render this display in its own MDropDX12.exe -child process. Implies
    // bOwnPreset in practice.
    //
    // ALWAYS the user's choice now. bAutoOwnProcess sat beside this and marked
    // a display the ENGINE had moved, so an automatic move could be undone
    // without clearing a child the user had chosen; the cost gate that made
    // those moves is gone (#261), nothing sets it, and the two flags have
    // collapsed back into one.
    bool bOwnProcess = false;
    // A literal path, or one of the kDispMode* sentinels above. Empty is the
    // pre-v3 form and reads as <Default> for the dir, <Random> for the preset.
    wchar_t szPresetDir[MAX_PATH] = {};
    wchar_t szStartupPreset[MAX_PATH] = {};
    float   fTimeBetweenPresets = -1.0f;     // -1 = inherit, 0 = no cycling
    bool    bSequentialOrder = false;
    // Preset lock for this display: -1 inherit the main window's, 0 unlocked,
    // 1 locked. Same three-valued shape as fTimeBetweenPresets above, and for
    // the same reason -- "not set" and "set to off" are different answers, and
    // a display that has never been touched must follow the main window rather
    // than pin a value nobody chose.
    int     nPresetLock = -1;

    // How many messages this display may show at once: -1 inherit the global
    // Messages setting, otherwise 1..NUM_SUPERTEXTS. Third field with this
    // shape, for the reason above (#255).
    //
    // Per display rather than global because a display running its own preset
    // now runs its own message loop, and the supertext SLOTS are one shared
    // pool -- so without a per-display limit one busy panel can take every slot
    // and the others fall silent. The limit is what keeps the loops
    // independent in practice and not just in principle.
    int     nMsgMaxOnScreen = -1;

    // Where this display's child window goes when the harness has asked for
    // TEST WINDOWS -- small and out of the way rather than covering a monitor.
    // Empty (right <= left) means "no rect chosen", and the child falls back to
    // a corner of the display it was assigned.
    //
    // Deliberately NOT persisted, and never written to settings.ini. It exists
    // only for a test run, the same way testing mode's other concessions do,
    // and a value surviving into a real session would produce a visualizer that
    // covers a corner of one monitor with no way to explain itself.
    RECT rcTestWindow = {};

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
    mdrop::SpoutSender sender;
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
