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

#ifndef MDROP_DXCONTEXT_H
#define MDROP_DXCONTEXT_H 1

#include <windows.h>
#include "shell_defines.h"

// DirectX 12 headers (part of Windows SDK — no legacy DXSDK_DIR required)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>   // ComPtr
#include <atomic>
#include <functional>     // m_waitForAuxQueues
#include <mutex>          // m_retiredMutex (#214)
#include <vector>         // m_retiredObjects (#214)

using Microsoft::WRL::ComPtr;

#include "dx12helpers.h"   // DX12Texture
#include "dx12pipeline.h"  // PSO IDs, DX12CreatePipelines

// Number of frames in flight (back-buffer count for the swap chain)
#define DXC_FRAME_COUNT 2

// Descriptor heap sizes
// 2 back buffers + dynamic (VS/blur/feedback/title/orient) + reserved mirror RTVs at the top.
// Orient pipe alone needs up to ~16 RTVs (classic: VS pair + display + blur6; milk3: 5 pairs).
// Was 48 (32 dynamic) — primary ~20 + orient failed → black mirrors (RTV heap exhausted).
#define DXC_MIRROR_RTV_RESERVE  16  // up to 5 mirrors × 3 buffers (plus spare)
#define DXC_MAX_RTV  96
#define DXC_MIRROR_RTV_BASE  (DXC_MAX_RTV - DXC_MIRROR_RTV_RESERVE)
// Texture SRVs + 32-slot binding blocks + second-orientation milk3 pipeline (~160 slots).
// Shader-visible CBV/SRV/UAV descriptors.
//
// Was 4096, which is not enough to put an independent mirror on every display.
// Each orient pipe reserves kBindSlots = (6 + DXC_FRAME_COUNT * MAX_BLUR_PASSES)
// * BINDING_BLOCK_SIZE = 576 descriptors, and a normal texture set already
// occupies most of the heap before any mirror exists. Measured on the live
// instance, 2026-09-12: baseline 2534 used, so 1562 free, so exactly TWO orient
// pipes fit and the third failed 76 times with "SRV heap full".
//
// That failure is not graceful. EnsureOrientPipeline returns false, the mirror
// has no pipe of its own and draws the fallback texture while the primary
// stays correct -- which is what "mirror mode and shadowing was not good"
// looks like -- and the render loop then threw three SEH exceptions and the
// device was removed.
//
// D3D12 allows up to 1,000,000 descriptors in this heap at Resource Binding
// Tier 1, so 4096 was conservative rather than required. At 32 bytes each,
// 16384 costs 512 KB of the two heaps combined -- room for a mirror on every
// display of any plausible setup, with the texture set intact.
#define DXC_MAX_SRV  16384
#define DXC_MAX_SAMPLERS 4

#define SNAP_WINDOWED_MODE_BLOCKSIZE  32

// Longest edge a texture loaded from disk may keep (#349). 0 = no cap.
//
// A free function with a file-static behind it, the same shape as
// SetMirrorSwapChainCap, so the value is readable from LoadTextureFromFile
// without any ordering question about when settings load relative to the
// device. Default 1024: measured bit-identical to the full 4160x3120 source at
// both 1200x1600 and 2160x3840, sampled 1:1 with no warp.
void SetMaxTextureDim(int maxDim);
int  GetMaxTextureDim();

typedef struct {
  int  nbackbuf;
  int  allow_page_tearing;
  GUID adapter_guid;
  char adapter_devicename[256];

  UINT   adapter_index;    // DXGI adapter ordinal (replaces DX9 adapter GUID)
  HWND   parent_window;
  int m_dualhead_horz;
  int m_dualhead_vert;
  int m_skin;
} DXCONTEXT_PARAMS;

class DXContext {
public:
  // ----- Public interface -----

  // Create from an already-initialized DX12 device + command queue.
  // The swap chain is created internally from hwnd.
  DXContext(
    ID3D12Device*        device,
    ID3D12CommandQueue*  commandQueue,
    IDXGIFactory4*       factory,
    HWND                 hwnd,
    int                  width,
    int                  height,
    wchar_t*             szIniFile,
    int                  fallbackTexStyle = 0);

  ~DXContext();

  BOOL StartOrRestartDevice(DXCONTEXT_PARAMS* pParams);
  void OnTrulyExiting() { m_truly_exiting = 1; }
  void UpdateMonitorWorkRect();
  int  GetBitDepth() { return m_bpp; }

  void SaveWindow();
  HWND GetHwnd();
  bool OnUserResizeWindow(RECT* w, RECT* c, bool bSetBackBuffer);
  bool TempIgnoreDestroyMessages();

  // Resize the swap chain to match new client dimensions.
  bool ResizeSwapChain(int newWidth, int newHeight);

  // Frame lifecycle — called by the render loop.
  // BeginFrame: transitions back buffer to render-target state, returns true on success.
  bool BeginFrame();
  // ExecuteCommandList: transitions back buffer RT→PRESENT, closes and submits the command list.
  void ExecuteCommandList();
  // EndFrame: presents the swap chain and advances to the next frame.
  void EndFrame();

  // CPU/GPU synchronisation helpers.
  void WaitForGpu();      // flush all in-flight GPU work and idle EVERY queue

  // "All in-flight GPU work" has to mean every queue this process submits to,
  // and DXContext can only see m_commandQueue. The mirror workers own
  // m_mirrorQueue and a fence per surface, which live on the Engine, so the
  // Engine installs a waiter here once and all 20 WaitForGpu call sites cover
  // both without each having to remember (#206).
  //
  // Set on the render thread at mirror-queue creation; called from inside
  // WaitForGpu. It was a use-after-free of the preset PSOs -- released while a
  // mirror list still referenced them -- that showed this was needed: a GPU
  // page fault at VA 0 on a draw in MirrorWork:\\.\DISPLAYn, then a TDR.
  std::function<void()> m_waitForAuxQueues;

  // True when every mirror surface's submitted work has completed. Installed
  // beside m_waitForAuxQueues and retired with it, and absent when there are
  // no mirrors -- in which case there is nothing but the main queue to satisfy.
  std::function<bool()> m_auxQueuesIdle;

  // DEFERRED RELEASE of GPU objects (#214).
  //
  // The teardown audit's recurring shape is: the render thread drops the last
  // reference to something a submitted command list still names. WaitForGpu
  // fixes that by stalling, which is right once per preset load and wrong at a
  // site that fires every frame or every blend.
  //
  // So hold the reference instead, and drop it once the GPU has moved past the
  // frame that used it. Same shape as the sprite retirement in #225, one level
  // down, so it works for any COM-refcounted GPU object: PSOs, root signatures,
  // resources, heaps.
  //
  // Both queues have to be satisfied, and the main-queue fence alone does NOT
  // imply the mirrors are done -- they run their own queue and their own fence.
  // A retirement therefore clears only when the recorded main fence has passed
  // AND every surface reports idle.
  void RetireComObject(const ComPtr<IUnknown>& obj);
  void CollectRetiredObjects();

  // Retire p and clear the caller's handle, so the ONLY surviving reference is
  // the retirement queue's. Callers should reach for this rather than Reset().
  template <class T>
  void RetireAndReset(ComPtr<T>& p) {
    if (!p) return;
    RetireComObject(p);
    p.Reset();
  }

  std::mutex m_retiredMutex;
  std::vector<std::pair<UINT64, ComPtr<IUnknown>>> m_retiredObjects;
  void MoveToNextFrame(); // advance frame index; waits if the next slot is still in use

  // ----- Public data (read-only from outside) -----
  int m_ready;
  HRESULT m_lastErr;
  int m_nPresentFailCount;
  int m_window_width;
  int m_window_height;
  int m_backbuffer_width;
  int m_backbuffer_height;
  int m_client_width;
  int m_client_height;
  int m_REAL_client_width;
  int m_REAL_client_height;
  int m_fake_fs_covers_all;
  int m_frame_delay;
  RECT m_all_monitors_rect;
  RECT m_monitor_rect;
  RECT m_monitor_rect_orig;
  RECT m_monitor_work_rect;
  RECT m_monitor_work_rect_orig;
  DXCONTEXT_PARAMS m_current_mode;

  // Drains any messages the DX12 debug layer queued since the last call and
  // logs them (ERROR/CORRUPTION at LOG_ERROR, WARNING at LOG_WARN, everything
  // else at LOG_VERBOSE) -- forgejo#16. A no-op when the debug layer isn't
  // running (Release, or a Debug build where device creation didn't find it),
  // since m_infoQueue is then null. Called once per frame from EndFrame();
  // exposed publicly so a diagnostic command can drain on demand too.
  void DrainDebugMessages();

  // Is the primary command list OPEN for recording right now?
  //
  // Call sites used to ask `m_lpDX->m_commandList` — whether the list EXISTS —
  // and treat that as permission to record into it. The list exists from
  // InitD3d onwards and spends most of its life CLOSED: everything outside a
  // BeginFrame/EndFrame pair, which includes the whole of startup, a window
  // resize, and device recovery. Recording there is silently dropped, and the
  // debug layer's only complaint is four unattributed "This API cannot be
  // called on a closed command list" errors (forgejo#16).
  //
  // Maintained at every Reset and Close of m_commandList, so this answers the
  // question the call sites were actually trying to ask.
  bool IsCommandListRecording() const { return m_cmdListRecording; }

  // ----- DX12 objects (public for access from plugin render code) -----
  ComPtr<ID3D12Device>               m_device;
  ComPtr<ID3D12InfoQueue>            m_infoQueue;  // null unless the debug layer is active (forgejo#16)
  ComPtr<ID3D12CommandQueue>         m_commandQueue;
  ComPtr<IDXGISwapChain4>            m_swapChain;
  ComPtr<ID3D12GraphicsCommandList>  m_commandList;
  // Whether m_commandList is between a Reset and its Close. Read through
  // IsCommandListRecording(); see the comment there for why it exists.
  bool                               m_cmdListRecording = false;
  // When set (e.g. independent-mirror orient pass), DrawVertices / TransitionResource
  // and DX12 draw helpers record into this list instead of the closed main list.
  //
  // THREAD-LOCAL, which is the whole point (#206). As a shared member this was
  // one of the two reasons a mirror record had to hold m_mirrorEngineMutex for
  // its entire duration: two workers, or a worker and the primary, would each
  // be redirecting the same pointer.
  //
  // Plumbing an explicit list through every caller was the obvious alternative
  // and is not worth it -- TransitionResource alone has 90 call sites and
  // DrawVertices 43, and a missed one records into the wrong list and dies at
  // execute time. This is the same answer the upload ring already got: the
  // worker stopped contending for shared state by having its own, which is
  // what MirrorRingForThisThread and SelectUploadRing do a few lines below.
  //
  // The member remains for the primary. A thread that never sets the
  // thread-local reads nullptr and behaves exactly as before.
  ID3D12GraphicsCommandList*         m_cmdListOverride = nullptr;
  // Per-thread override, consulted before the shared one. Defined in
  // dxcontext.cpp; declared here so the hot accessor stays inline.
  static thread_local ID3D12GraphicsCommandList* t_cmdListOverride;
  // Set/clear the CALLING THREAD's override. A worker uses this; the primary
  // keeps using m_cmdListOverride.
  void SetThreadCmdListOverride(ID3D12GraphicsCommandList* cl) {
    t_cmdListOverride = cl;
  }
  ID3D12GraphicsCommandList* GetActiveCmdList() const {
    // A MIRROR WORKER must never end up here (#206).
    //
    // A worker records into its own list, which it is handed on the
    // RenderContext. Reaching for the shared one means a site that has a
    // context and asked a global anyway -- and the failure mode is silent:
    // the work is recorded into the wrong list and dies at EXECUTE time, far
    // from the call, which is how this codebase has been caught before.
    //
    // So it is reported rather than inspected for. m_cmdListOverride exists to
    // paper over exactly these sites and is being removed one at a time; every
    // one still outstanding shows up here as a count on DIAG_MIRRORS and a
    // one-shot error in the log.
    if (t_cmdListOverride)
      return t_cmdListOverride;
    if (IsMirrorUploadThread())
      NoteActiveCmdListOnWorker();
    return m_cmdListOverride ? m_cmdListOverride : m_commandList.Get();
  }
  // Out of line: the header cannot log. One-shot warning, plus a count so a
  // whole phase can be checked with one DIAG read instead of a log trawl.
  void NoteActiveCmdListOnWorker() const;
  UINT ActiveCmdListOnWorkerCount() const {
    return m_activeCmdListOnWorker.load(std::memory_order_relaxed);
  }
  mutable std::atomic<UINT> m_activeCmdListOnWorker{0};

  // Per-frame resources (indexed by m_frameIndex)
  ComPtr<ID3D12CommandAllocator>     m_commandAllocators[DXC_FRAME_COUNT];

  // Back-buffer render targets
  ComPtr<ID3D12Resource>             m_renderTargets[DXC_FRAME_COUNT];

  // Descriptor heaps
  ComPtr<ID3D12DescriptorHeap>       m_rtvHeap;          // RTV heap (DXC_MAX_RTV entries)
  ComPtr<ID3D12DescriptorHeap>       m_srvHeap;          // SRV/CBV/UAV heap (shader-visible)
  // Non-shader-visible mirror of m_srvHeap, same NumDescriptors, indexed
  // identically -- the readable source for every CopyDescriptorsSimple that
  // used to (invalidly) read m_srvHeap itself. forgejo#16.
  ComPtr<ID3D12DescriptorHeap>       m_srvHeapCpu;
  ComPtr<ID3D12DescriptorHeap>       m_samplerHeap;      // Sampler heap (shader-visible)
  UINT                               m_rtvDescriptorSize;
  UINT                               m_srvDescriptorSize;
  UINT                               m_samplerDescriptorSize;

  // Bump allocators for descriptor slots (slots 0..N-1 for back buffers already taken)
  UINT                               m_nextFreeRtvSlot;  // starts at DXC_FRAME_COUNT
  UINT                               m_nextFreeSrvSlot;  // starts at 0
  // Descriptor slots handed back by RecycleTexture (#312). Single slots and
  // BINDING_BLOCK_SIZE blocks are kept apart because a block has to be
  // contiguous and a single slot cannot satisfy one.
  UINT                               m_pendingSrvSlot = 0;
  std::vector<UINT>                  m_freeSrvSlots;
  std::vector<UINT>                  m_freeBindingBlocks;

  // Baseline values after one-time init (null texture + per-frame bindings).
  // ResetDynamicDescriptors() rewinds the bump allocators to these values so that
  // textures allocated in AllocateMyDX9Stuff can be safely re-created on resize/toggle.
  UINT                               m_rtvSlotBaseline = 0;
  UINT                               m_srvSlotBaseline = 0;

  // DXContext-level baselines set once in Init() — before font atlas / help texture.
  // Used by ResetBufferAndFonts() to fully reclaim font atlas SRV slots on rebuild.
  UINT                               m_rtvSlotInitBaseline = 0;
  UINT                               m_srvSlotInitBaseline = 0;

  // DRED is dumped at most once per device. Every device-removal detection site
  // calls DumpDredToLog(), including the polling predicates, so without this the
  // one site that noticed first would be drowned by a dozen identical repeats.
  // Keyed on the device pointer, so a device created by recovery gets its own dump.
  // (2026-08-24: a GPU hang was lost because only the Present path dumped.)
  // Atomic: the detection sites run on the render thread, the UI thread
  // (IsDeviceHungOrRemoved) and the mirror worker, and a removal is noticed by
  // all three at once.
  std::atomic<ID3D12Device*>         m_pDredDumpedFor{ nullptr };

  // STAGE MAP (#222) — which render stage emitted which draw.
  //
  // DRED names the op that hung ("op[14] = DRAWINSTANCED") but not what drew
  // it, and a frame is a dozen stages deep. Guessing from the op pattern sent
  // two separate investigations at the wrong stage. This records, per frame,
  // the draw ordinal at which each stage began; DumpDredToLog prints it beside
  // the breadcrumbs, so the faulting op resolves to a named stage.
  //
  // Costs one increment per draw and a pointer store per stage, and logs
  // nothing unless the device is actually removed — so it stays in for good
  // rather than being re-added by hand the next time a GPU hangs.
  // Stage names cover the big passes, but a name recorded at function ENTRY
  // also swallows any later draw that no stage claims -- which is how the #222
  // fault first read as "usersprites" on a machine with no sprites.ini at all.
  // So every draw ALSO records its own return address, resolved through DbgHelp
  // at dump time into the exact function and line that issued it. That needs no
  // call-site changes and cannot silently mislabel.
  void BeginStage(const char* name);   // call at each render stage boundary
  void ResetStageMap();                // call once per frame, before recording

  // GPU TIMESTAMPS PER STAGE (#230).
  //
  // Every engine-level measurement so far averages over a whole frame, and the
  // thing that kills the device is ONE DRAW inside one frame. primDt sat flat
  // at 22 ms for 135 seconds and then the device was gone between two samples,
  // so the spike is invisible at that resolution -- not absent, just finer than
  // the instrument.
  //
  // This puts a timestamp at each stage boundary on the GPU's own clock, so a
  // stage that takes 700 ms shows up as 700 ms instead of being averaged into a
  // 22 ms frame. Resolved into a readback buffer at the end of the frame and
  // read once that frame's fence has passed, so nothing stalls to collect it.
  //
  // Off unless asked for: it costs a query per stage and a resolve per frame,
  // which is small but not free, and nobody needs it until a device is dying.
  void EnableGpuStageTiming(bool on);
  bool GpuStageTimingOn() const { return m_bGpuTiming; }
  // Report any stage at or above this many milliseconds. A frame at 60 fps is
  // 16 ms, so 50 is already pathological and 2000 is fatal.
  double m_gpuTimingReportMs = 50.0;

  // WHOLE-FRAME GPU TIME FOR THE PRIMARY, ALWAYS ON (#245).
  //
  // Deliberately NOT part of the per-stage machinery above. That one is a
  // diagnostic: a query per stage plus a resolve, off unless a device is
  // dying. This is one pair around the frame, which the mirror surfaces
  // already pay for themselves, and it has to be always on because something
  // depends on it continuously.
  //
  // What depends on it: the GPU-cost isolation gate. peakGpuUs was fed only by
  // INDEPENDENT mirror surfaces, so with none live -- every extra display
  // already a child, or copy mirrors, or skipped as the render window's own
  // monitor -- it read 0. PresetRisksTheDevice() then cannot tell
  // 0-because-cheap from 0-because-nothing-was-measured, and treats both as
  // cheap. Measured: a copy mirror gives surfaces=0 peakGpuUs=0 on the same
  // preset an independent one reports at 30542 us.
  //
  // The primary renders that same preset and is never absent, so feeding it in
  // means a reading always exists and "unknown" stops being representable.
  unsigned LastFrameGpuUs() const {
    return m_lastFrameGpuUs.load(std::memory_order_relaxed);
  }

private:
  bool CreateFrameTimingResources();
  void BeginFrameTimestamp();    // first thing recorded
  void EndFrameTimestamp();      // last thing recorded, then resolved
  void ReadFrameTimestamp();     // in BeginFrame, after this index's fence
  ComPtr<ID3D12QueryHeap> m_frameTsHeap;
  ComPtr<ID3D12Resource>  m_frameTsReadback[DXC_FRAME_COUNT];
  UINT64                  m_frameTsFreq = 0;
  bool                    m_frameTsPending[DXC_FRAME_COUNT] = {};
  std::atomic<unsigned>   m_lastFrameGpuUs{0};

  bool CreateGpuTimingResources();
  void ResolveGpuTimestamps();   // end of frame, into the readback buffer
  void ReportGpuTimestamps();    // after that frame's fence, out of it

  static const int kMaxStamps = 64;      // stages per frame; the rest are dropped
  bool m_bGpuTiming = false;
  ComPtr<ID3D12QueryHeap>  m_tsHeap;
  ComPtr<ID3D12Resource>   m_tsReadback[DXC_FRAME_COUNT];
  UINT64                   m_tsFrequency = 0;
  int                      m_tsCount = 0;             // stamps this frame
  const char*              m_tsNames[kMaxStamps] = {};
  int                      m_tsPending[DXC_FRAME_COUNT] = {};  // stamps awaiting readback
  const char*              m_tsPendingNames[DXC_FRAME_COUNT][kMaxStamps] = {};

public:
  // 64 was too small and #225 proved it: a frame with 850 visible shape
  // instances ran to 878 draws, so the call-site table stopped long before the
  // faulting sprite draw and only the coarser stage map could name it. Two
  // arrays of pointers, so 1024 costs 16 KB and covers a heavy frame whole.
  static const int kMaxTrackedDraws = 1024;
  void* m_frameDrawRet[kMaxTrackedDraws] = {};
  void* m_prevDrawRet[kMaxTrackedDraws] = {};
  UINT m_frameDrawOrdinal = 0;
  struct StageMark { UINT draw; const char* name; };
  static const int kMaxStageMarks = 48;
  StageMark m_stageMarks[kMaxStageMarks] = {};
  int       m_stageMarkCount = 0;
  // The CPU runs ahead of the GPU, so the frame that hung is usually the one
  // BEFORE the map being built. Keep both and print both.
  StageMark m_prevStageMarks[kMaxStageMarks] = {};
  int       m_prevStageMarkCount = 0;
  UINT      m_prevFrameDrawCount = 0;

  // Helpers for allocating descriptor slots
  void DumpDredToLog(const char* whence = nullptr); // last GPU ops + page-fault VA after a device removal
  D3D12_CPU_DESCRIPTOR_HANDLE AllocateRtv();   // returns CPU handle, bumps m_nextFreeRtvSlot
  D3D12_CPU_DESCRIPTOR_HANDLE AllocateSrvCpu(); // returns CPU handle at PendingSrvSlot()
  D3D12_GPU_DESCRIPTOR_HANDLE AllocateSrvGpu(); // returns GPU handle, consumes the pending slot

  // ── Reclaiming descriptor slots, so a texture can be evicted (#312) ──
  //
  // The SRV heap was a pure bump allocator: m_nextFreeSrvSlot only ever went
  // up. That is fine while nothing is ever released, which was the case --
  // EvictSomeTexture had no callers. It stops being fine the moment textures
  // are evicted and re-loaded, because each disk texture costs one SRV slot
  // plus a BINDING_BLOCK_SIZE block, and DXC_MAX_SRV / 33 is about 500
  // textures before the heap is exhausted.
  //
  // The slot a creation path is about to use. AllocateSrvCpu sets it -- to a
  // recycled slot when one is free, otherwise to m_nextFreeSrvSlot -- and
  // AllocateSrvGpu consumes it. Call sites read this instead of
  // m_nextFreeSrvSlot, which is no longer the same thing.
  UINT PendingSrvSlot() const { return m_pendingSrvSlot; }

  // Release a texture's GPU memory and return its descriptor slots to the free
  // lists. Points every descriptor it owned at the null texture FIRST, so a
  // command list still in flight that names one samples null instead of freed
  // memory. That ordering is the whole safety argument.
  void RecycleTexture(DX12Texture& tex);

  UINT RecycledSrvSlotCount() const { return (UINT)m_freeSrvSlots.size(); }
  UINT RecycledBlockCount() const { return (UINT)m_freeBindingBlocks.size(); }
  D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandleAt(UINT srvIndex); // handle at specific index, no bump
  // Same index, m_srvHeapCpu instead of m_srvHeap -- the one that is
  // actually legal to read (as a CopyDescriptorsSimple source). forgejo#16.
  D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandleAtReadable(UINT srvIndex);
  D3D12_CPU_DESCRIPTOR_HANDLE GetRtvCpuHandleAt(UINT rtvIndex); // fixed slot, no bump
  void UpdateBindingBlockTexture(UINT blockStart, UINT texSrvIndex); // overwrite existing block in-place

  // Permanent RTV slots above the dynamic bump range (survive ResetDynamicDescriptors /
  // resize). Returns base index or UINT_MAX if the reserve is exhausted.
  UINT AllocateMirrorRtvBlock(UINT count);
  void FreeMirrorRtvBlock(UINT base, UINT count);

  // Bumped whenever RTV/SRV bump allocators rewind — session-lived mirrors must
  // re-CreateRenderTargetView (and letterbox SRV) against surviving slots.
  UINT m_descriptorEpoch = 0;
  void BumpDescriptorEpoch() { m_descriptorEpoch++; }

  // Reset bump allocators to post-init baseline (call before re-creating dynamic textures)
  void ResetDynamicDescriptors();

  // What a texture ACTUALLY costs in VRAM: every mip level, with the driver's
  // own alignment, asked of the device rather than estimated (#347).
  //
  // The texture cache used to compute `w * h * 4 + 16384` -- the base level and
  // a flat 16 KB. CreateTextureFromFile uploads a full mip chain, which is 4/3
  // of the base, and 64 KB subresource alignment on the small mips takes the
  // real figure to about 1.4x what was recorded. Measured across nine preset
  // loads on a live instance, the unrecorded part was 39-40% of the reported
  // cache size on EVERY step, and it showed up outside texMB looking like a
  // separate leak. It also made `MaxBytes` mean roughly 1.4x what it says, so
  // eviction ran that much later than configured.
  //
  // GetResourceAllocationInfo is the device's own answer for the desc it was
  // given, so mips, padding and alignment are included by construction instead
  // of being approximated by a constant that has to be kept in step.
  UINT64 TextureVramBytes(const DX12Texture& tex) {
    if (!tex.resource || !m_device)
      return 0;
    const D3D12_RESOURCE_DESC rd = tex.resource->GetDesc();
    const D3D12_RESOURCE_ALLOCATION_INFO ai =
        m_device->GetResourceAllocationInfo(0, 1, &rd);
    // UINT64_MAX is the documented "invalid desc" answer; fall back to the old
    // estimate rather than recording a zero, which would make the entry look
    // free and never evictable.
    if (ai.SizeInBytes == UINT64_MAX)
      return (UINT64)tex.width * tex.height * 4ull + 16384ull;
    return ai.SizeInBytes;
  }

  // Texture creation helpers (Phase 2)
  // clearAlpha is the ALPHA THIS TARGET WILL ACTUALLY BE CLEARED WITH, and it
  // is a parameter because the answer genuinely differs by target.
  //
  // A committed render target carries an optimized clear value, and clearing to
  // a different one still produces the right pixels but gives up the driver's
  // fast path -- "[820] the clear values do not match those passed to resource
  // creation", once per clear (forgejo#104).
  //
  // RGB is always black; only alpha varies. Shadertoy feedback and Buffer A-D
  // targets are cleared to 0 because their alpha carries accumulation DATA and
  // a fresh buffer must read as (0,0,0,0). Ordinary colour targets are cleared
  // to opaque black. Passing the wrong one here does not change a pixel -- it
  // only re-earns the warning -- so it is safe, but keep it honest.
  DX12Texture CreateRenderTargetTexture(UINT width, UINT height,
                                        DXGI_FORMAT format,
                                        float clearAlpha = 1.0f);

  // Resource state transitions (Phase 4).
  // cmdList: if null, uses the main per-frame list (m_commandList).
  // Mirror/orient work runs AFTER ExecuteCommandList() on m_mirrorCmdList —
  // always pass that list or barriers are recorded on a closed main list and
  // never reach the GPU (stale feedback / landscape ghosts on portrait).
  void TransitionResource(DX12Texture& tex, D3D12_RESOURCE_STATES newState,
                          ID3D12GraphicsCommandList* cmdList = nullptr);
  D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle(const DX12Texture& tex);
  D3D12_CPU_DESCRIPTOR_HANDLE GetRtvCpuHandle(const DX12Texture& tex);

  // Root signature (Phase 3, updated Phase 4: static samplers + 1-SRV table)
  // Layout: [0] CBV (b0), [1] descriptor table 1 SRV (t0), + 4 static samplers s0-s3
  ComPtr<ID3D12RootSignature> m_rootSignature;
  ComPtr<ID3D12RootSignature> m_blurRootSignature; // Same layout but s0 = CLAMP (blur passes need CLAMP, not WRAP)
  bool CreateRootSignature();
  bool RecreateRootSigAndPipelines();
  bool m_bAnisotropicFiltering = false;

  // Pipeline state objects (Phase 3)
  ComPtr<ID3D12PipelineState> m_PSOs[PSO_COUNT];
  bool CreatePipelines();

  // Upload heap ring buffers (Phase 3) — per-frame suballocation for DrawPrimitiveUP replacement
  // Particle-heavy presets (4×512 thick custom waves + shapes) can exceed 8 MB/frame.
  // The warp mesh is an unindexed triangle list: nMeshSize 192 → 192*144*2 tris * 3 verts
  // * 40 B (MYVERTEX) = 6.33 MB for ONE draw. A blending .milk2 double preset draws it
  // twice (6.33*2 = 12.67 MB) before a single shape is queued, so 16 MB left no room for
  // ~4 MB of 100-gon shapes plus the comp mesh. Comp is issued last, so it was the draw
  // that got silently dropped → black screen (MilkDrop2077 Snake Textures at mesh 192).
  // 32 MB holds the mesh-192 dual-preset worst case with headroom; matches the aux ring.
  static const UINT UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024; // 32 MB vertex upload per frame
  ComPtr<ID3D12Resource> m_uploadBuffer[DXC_FRAME_COUNT];
  BYTE*                  m_uploadBufferPtr[DXC_FRAME_COUNT]; // persistently mapped
  UINT                   m_uploadBufferOffset[DXC_FRAME_COUNT]; // current write position
  // Dedicated upload for post-Execute work (independent mirrors) — double-buffered
  // by frame index so we never WaitForGpu the main list every frame.
  // Classic opposite-orient re-renders shapes/waves (milk2 Cubes ~592 instances × blend
  // passes). 8 MB exhausted → silent DrawVertices drops → black mirrors. 32 MB holds
  // heavy dual-preset shape dumps with headroom for blur CBs + HUD.
  static const UINT AUX_UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024; // 32 MB per frame slot
  ComPtr<ID3D12Resource> m_auxUploadBuffer[DXC_FRAME_COUNT];
  BYTE*                  m_auxUploadPtr[DXC_FRAME_COUNT] = {};
  UINT                   m_auxUploadOffset[DXC_FRAME_COUNT] = {};
  bool                   m_bUseAuxUpload = false;

  // Dedicated upload ring for the mirror worker thread. The opposite-orient
  // pipeline re-renders shapes/waves off the render thread; sharing the aux ring
  // let the worker reset the render thread's offsets mid-frame (black presets,
  // then TDR). Single-slot is safe: the worker never submits again until its own
  // fence has signalled, so there is never a second use in flight.
  // ONE RING PER WORKER THREAD (#186 phase 5).
  //
  // This was a single ring owned by a single registered thread id. That was
  // right while one worker recorded every surface; with a worker per surface,
  // two of them sharing a ring reproduces exactly the failure the note above
  // describes -- one resetting the other's offset mid-record.
  //
  // A small fixed array rather than a map: the slot is looked up on every
  // upload, from the worker threads, and a lock or an allocation there would
  // sit in the hottest path in the record. MAX_MIRROR_UPLOAD_RINGS is the cap
  // on concurrent mirror workers; a thread that cannot claim one falls through
  // to the aux/main ring exactly as an unregistered thread always has.
  static const int MAX_MIRROR_UPLOAD_RINGS = 8;
  struct MirrorUploadRing {
    ComPtr<ID3D12Resource> buffer;
    BYTE*                  ptr = nullptr;
    UINT                   offset = 0;
    std::atomic<DWORD>     threadId{0};
  };
  MirrorUploadRing m_mirrorRings[MAX_MIRROR_UPLOAD_RINGS];

  // The calling thread's ring, or nullptr if it does not own one.
  MirrorUploadRing* MirrorRingForThisThread() {
    const DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < MAX_MIRROR_UPLOAD_RINGS; i++)
      if (m_mirrorRings[i].threadId.load(std::memory_order_acquire) == tid)
        return &m_mirrorRings[i];
    return nullptr;
  }
  const MirrorUploadRing* MirrorRingForThisThread() const {
    const DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < MAX_MIRROR_UPLOAD_RINGS; i++)
      if (m_mirrorRings[i].threadId.load(std::memory_order_acquire) == tid)
        return &m_mirrorRings[i];
    return nullptr;
  }
  bool IsMirrorUploadThread() const {
    return MirrorRingForThisThread() != nullptr;
  }
  // Allocated only while independent mirrors are on; released with the thread.
  //
  // Both take the OWNING thread id rather than keying on the caller: the ring
  // has to exist before the worker runs a single frame, so the render thread
  // creates it on the worker's behalf while the worker is still suspended.
  bool CreateMirrorUploadBuffer(DWORD tid);
  void ReleaseMirrorUploadBuffer(DWORD tid);
  // The ring owned by `tid`, or nullptr.
  MirrorUploadRing* MirrorRingForThread(DWORD tid) {
    if (!tid) return nullptr;
    for (int i = 0; i < MAX_MIRROR_UPLOAD_RINGS; i++)
      if (m_mirrorRings[i].threadId.load(std::memory_order_acquire) == tid)
        return &m_mirrorRings[i];
    return nullptr;
  }
  // Claim a ring for `tid` (0 releases the calling thread's). Returns false when
  // every slot is taken -- the caller still runs, on the shared ring.
  bool SetMirrorUploadThreadId(DWORD tid);
  // Picks main / aux / mirror ring for the calling thread.
  void SelectUploadRing(BYTE*& basePtr, ID3D12Resource*& baseRes,
                        UINT*& pOffset, UINT& bufSize);

  bool CreateUploadBuffers();
  void ResetUploadBuffer(); // called at BeginFrame — resets offset to 0
  void BeginAuxUpload() {
    // The worker owns its own ring and must never touch the shared aux state.
    if (MirrorUploadRing* r = MirrorRingForThisThread()) {
      r->offset = 0;
      return;
    }
    m_bUseAuxUpload = true;
    m_auxUploadOffset[m_frameIndex] = 0;
  }
  void EndAuxUpload() {
    if (IsMirrorUploadThread())
      return;
    m_bUseAuxUpload = false;
  }
  // Bytes left in the active upload ring (aux when m_bUseAuxUpload, else main).
  // Used by mirror shape caps so heavy milk2 won't silent-drop to black.
  UINT GetUploadBytesRemaining() const {
    if (const MirrorUploadRing* r = MirrorRingForThisThread(); r && r->ptr) {
      if (r->offset >= AUX_UPLOAD_BUFFER_SIZE)
        return 0;
      return AUX_UPLOAD_BUFFER_SIZE - r->offset;
    }
    if (m_bUseAuxUpload) {
      if (!m_auxUploadPtr[m_frameIndex] || m_auxUploadOffset[m_frameIndex] >= AUX_UPLOAD_BUFFER_SIZE)
        return 0;
      return AUX_UPLOAD_BUFFER_SIZE - m_auxUploadOffset[m_frameIndex];
    }
    if (!m_uploadBufferPtr[m_frameIndex] || m_uploadBufferOffset[m_frameIndex] >= UPLOAD_BUFFER_SIZE)
      return 0;
    return UPLOAD_BUFFER_SIZE - m_uploadBufferOffset[m_frameIndex];
  }
  UINT GetUploadBufferCapacity() const {
    if (const MirrorUploadRing* r = MirrorRingForThisThread(); r && r->ptr)
      return AUX_UPLOAD_BUFFER_SIZE;
    return m_bUseAuxUpload ? AUX_UPLOAD_BUFFER_SIZE : UPLOAD_BUFFER_SIZE;
  }

  // Draw helper: suballocates from upload buffer, copies vertices, issues draw call.
  // cmdList: if null, uses the main per-frame list (m_commandList).
  void DrawVertices(D3D12_PRIMITIVE_TOPOLOGY topology, const void* vertexData,
                    UINT vertexCount, UINT vertexStride,
                    ID3D12GraphicsCommandList* cmdList = nullptr);

  // Indexed variant: suballocates the vertex data AND a 16-bit index list from the same
  // ring, then issues one DrawIndexedInstanced. The warp mesh reuses each vertex ~6x, so
  // submitting it expanded as a triangle list copied 6.33 MB/draw at nMeshSize 192;
  // indexed it is 1.12 MB of verts + <=331 KB of indices.
  void DrawIndexedVertices(D3D12_PRIMITIVE_TOPOLOGY topology,
                           const void* vertexData, UINT vertexCount, UINT vertexStride,
                           const UINT16* indexData, UINT indexCount,
                           ID3D12GraphicsCommandList* cmdList = nullptr);

  // CBV upload helper: suballocates from upload buffer with 256-byte alignment, returns GPU VA
  D3D12_GPU_VIRTUAL_ADDRESS UploadConstantBuffer(const void* data, UINT sizeBytes);

  // Dedicated upload command allocator for texture creation (won't conflict with per-frame allocators)
  ComPtr<ID3D12CommandAllocator>    m_uploadAllocator;
  ComPtr<ID3D12GraphicsCommandList> m_uploadCommandList;
  ComPtr<ID3D12Fence>              m_uploadFence;
  UINT64                           m_uploadFenceValue = 0;
  HANDLE                           m_uploadFenceEvent = nullptr;

  // Create a DX12 texture from CPU pixel data (synchronous GPU upload).
  // genMips builds a full box-filtered mip chain on the CPU first: DX9's
  // D3DXCreateTextureFromFileEx mip-mapped every disk texture automatically,
  // and preset LOD sampling (tex2Dlod/tex2Dbias blur effects) depends on it.
  // Only 4-byte-per-pixel formats are eligible; render targets and dynamic
  // textures stay single-mip.
  DX12Texture CreateTextureFromPixels(const void* pixels, UINT width, UINT height,
                                      UINT srcRowPitch, DXGI_FORMAT format,
                                      bool genMips = false);
  // Create a DX12 3D volume texture from CPU pixel data (synchronous GPU upload)
  DX12Texture CreateVolumeTextureFromPixels(const void* pixels, UINT width, UINT height, UINT depth,
                                             UINT srcRowPitch, DXGI_FORMAT format);
  // Load a texture from an image file via WIC, falling back to our own DDS and
  // TGA decoders for the flavours WIC returns nothing for — 24bpp DDS, which
  // has no DXGI format to map onto, and TGA, for which Windows ships no codec.
  DX12Texture LoadTextureFromFile(const wchar_t* szFilename);

  // Null texture (1x1 black) for filling unused SRV slots
  DX12Texture m_nullTexture;
  // Fallback texture for missing disk textures (style set in settings)
  DX12Texture m_fallbackTexture;
  int m_nFallbackTexStyle = 0;  // 0=Hue Gradient, 1=White, 2=Black, 3=Random(RandTexDir), 4=Random(TexDir), 5=Custom File
  wchar_t m_szFallbackRandomTexDir[MAX_PATH] = {};
  wchar_t m_szFallbackTexturesDir[MAX_PATH] = {};
  wchar_t m_szFallbackCustomFile[MAX_PATH] = {};
  bool CreateNullTexture(int fallbackStyle = 0);
  // Rebuild m_fallbackTexture live, without a device reinit (SET_FALLBACK_TEX
  // over the pipe). Same style codes as CreateNullTexture; customFile applies
  // to style 5 only. Idles the GPU first — the outgoing resource may still be
  // bound in flight — and takes a fresh SRV slot rather than rewriting the old
  // one, so descriptors already copied into binding blocks stay valid until
  // their next refill.
  bool RefreshFallbackTexture(int fallbackStyle, const wchar_t* customFile);

  // Create a 32-entry binding block for a texture (all slots = tex for simple passthrough)
  // Writes to tex.bindingBlockStart. Call after CreateNullTexture().
  static const UINT BINDING_BLOCK_SIZE = 32;
  void CreateBindingBlockForTexture(DX12Texture& tex);

  // Create a 32-entry binding block with tex at a specific slot, null elsewhere.
  // Returns the starting SRV heap index for the block. Used for preset shaders
  // where sampler_main maps to a t-register other than t0.
  UINT CreateBindingBlockAtSlot(const DX12Texture& tex, UINT mainSlot);

  // Get GPU handle for a texture's binding block (all 32 SRV slots)
  D3D12_GPU_DESCRIPTOR_HANDLE GetBindingBlockGpuHandle(const DX12Texture& tex);

  // Get GPU handle for a binding block by its starting SRV index
  D3D12_GPU_DESCRIPTOR_HANDLE GetBindingBlockGpuHandleByIndex(UINT blockStart);
  // Fill 32 contiguous SRV slots starting at baseIndex from per-texture srv indices
  // (UINT_MAX → null texture). Used by secondary orientation milk3 pipeline.
  void FillSrvBindingBlock(UINT baseIndex, const UINT slots[32]);
  D3D12_GPU_DESCRIPTOR_HANDLE GetSrvBlockGpuHandle(UINT baseIndex);

  // Per-frame binding blocks: avoids GPU race by using separate descriptor ranges per frame.
  // 2 frames × 8 passes (warp + bufferA + bufferB + bufferC + bufferD + comp + oldWarp + oldComp) × 32 SRV descriptors.
  static const UINT PASSES_PER_FRAME = 8; // warp, bufferA, bufferB, bufferC, bufferD, comp, oldWarp, oldComp
  UINT m_perFrameBindingBase = UINT_MAX;
  bool AllocatePerFrameBindings(); // call once at init, after CreateNullTexture
  void UpdatePerFrameBindings(const UINT warpSrvSlots[32], const UINT bufferASrvSlots[32],
                              const UINT bufferBSrvSlots[32], const UINT bufferCSrvSlots[32],
                              const UINT bufferDSrvSlots[32], const UINT compSrvSlots[32],
                              const UINT oldWarpSrvSlots[32], const UINT oldCompSrvSlots[32]);
  D3D12_GPU_DESCRIPTOR_HANDLE GetWarpBindingGpuHandle();
  D3D12_GPU_DESCRIPTOR_HANDLE GetBufferABindingGpuHandle();
  D3D12_GPU_DESCRIPTOR_HANDLE GetBufferBBindingGpuHandle();
  D3D12_GPU_DESCRIPTOR_HANDLE GetBufferCBindingGpuHandle();
  D3D12_GPU_DESCRIPTOR_HANDLE GetBufferDBindingGpuHandle();
  D3D12_GPU_DESCRIPTOR_HANDLE GetCompBindingGpuHandle();
  D3D12_GPU_DESCRIPTOR_HANDLE GetOldWarpBindingGpuHandle();
  D3D12_GPU_DESCRIPTOR_HANDLE GetOldCompBindingGpuHandle();

  // Per-frame blur binding blocks: 2 frames × 6 blur passes × 16 SRV descriptors = 192 total.
  static const UINT MAX_BLUR_PASSES = 6; // NUM_BLUR_TEX
  UINT m_blurBindingBase = UINT_MAX;
  bool AllocateBlurBindings();
  // baseOverride: the caller's own reserved block set, already offset for its
  // frame rotation. UINT_MAX keeps the shared engine-wide blocks, which is the
  // primary's path and is unchanged (#209).
  void UpdateBlurPassBinding(UINT passIndex, UINT sourceSrvIndex,
                             UINT baseOverride = UINT_MAX);
  D3D12_GPU_DESCRIPTOR_HANDLE GetBlurPassBindingGpuHandle(UINT passIndex,
                             UINT baseOverride = UINT_MAX);

  // Current frame index within [0, DXC_FRAME_COUNT)
  UINT  m_frameIndex;

  // Fence for CPU/GPU synchronisation
  ComPtr<ID3D12Fence> m_fence;
  UINT64              m_fenceValues[DXC_FRAME_COUNT];

  // Deferred-release fences (#225).
  //
  // MoveToNextFrame signals m_fenceValues[m_frameIndex] for the frame being
  // recorded, so a GPU object that frame references is safe to free only once
  // the fence has reached that value. Callers that must drop something from
  // inside a frame record this value and act on it later.
  UINT64 CurrentFrameFenceValue() const { return m_fenceValues[m_frameIndex]; }

  // UINT64_MAX when there is no fence, and the D3D12 runtime also returns
  // UINT64_MAX once the device is removed. Both mean the same thing to a
  // caller holding retired objects -- nothing is executing, release them --
  // so this deliberately reports "everything has completed" in both cases
  // rather than stranding resources that would then leak at shutdown.
  UINT64 CompletedFenceValue() const {
    return m_fence ? m_fence->GetCompletedValue() : UINT64_MAX;
  }
  HANDLE              m_fenceEvent;

  bool  m_tearingSupported = false;
  // Raise when multiple flip-model swap chains Present each frame (mirrors).
  // DXGI frame latency is per-device, not per swap chain — default 3 saturates
  // at 1 main + 2 mirrors and Present starts blocking (~20–25 fps in background).
  // presentOpsPerFrame: used for logging. serializeFrames: MaxFrameLatency=1 so
  // multi-SC flip queues cannot show a stale face every other frame at high FPS.
  void EnsureMultiSwapChainFrameLatency(int presentOpsPerFrame, bool serializeFrames = false);

  // When true, EndFrame presents with vsync and no tearing (set while multi-monitor
  // mirrors are active so main + mirrors flip the same frame cadence).
  bool m_bSerializeWithMirrors = false;
  bool* m_pVSync = nullptr;  // points to engine's m_bEnableVSync

protected:
  HWND    m_hwnd;
  wchar_t m_szIniFile[MAX_PATH];
  int     m_truly_exiting;
  int     m_bpp;
  char    m_szWindowCaption[512];

  void WriteSafeWindowPos();
  bool Internal_Init(IDXGIFactory4* factory, HWND hwnd, int width, int height);
  void Internal_CleanUp();
  void SetViewport(int width, int height);
  void CreateRtvsForSwapChain();
  void ReleaseSwapChainRtvs();
};

// Error codes (kept for compatibility)
#define DXC_ERR_REGWIN    -2
#define DXC_ERR_CREATEWIN -3
#define DXC_ERR_CREATE3D  -4
#define DXC_ERR_GETFORMAT -5
#define DXC_ERR_FORMAT    -6
#define DXC_ERR_CREATEDEV_PROBABLY_OUTOFVIDEOMEMORY -7
#define DXC_ERR_RESIZEFAILED -8
#define DXC_ERR_CAPSFAIL  -9
#define DXC_ERR_BAD_FS_DISPLAYMODE -10
#define DXC_ERR_USER_CANCELED -11
#define DXC_ERR_CREATEDEV_NOT_AVAIL -12
#define DXC_ERR_CREATEDDRAW  -13

// --- DX12 helpers to reduce duplication ---

inline void SetViewportAndScissor(ID3D12GraphicsCommandList* cl, UINT w, UINT h) {
  D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
  D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
  cl->RSSetViewports(1, &vp);
  cl->RSSetScissorRects(1, &sc);
}

// cpuReadable: the SAME view, written a second time into m_srvHeapCpu at the
// matching index -- the readable mirror any later CopyDescriptorsSimple must
// source from instead of `cpu` itself (forgejo#16; `cpu` is m_srvHeap, whose
// CPU handles are write-only once the heap is shader-visible). Every caller
// has both handles already, from the same GetSrvCpuHandleAt(index) /
// GetSrvCpuHandleAtReadable(index) pair used to pick `cpu` in the first
// place, so this is never optional -- a texture created without its mirror
// is a texture nothing can ever legally bind into a binding block.
inline void CreateSRV2D(ID3D12Device* dev, ID3D12Resource* res,
    DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
    D3D12_CPU_DESCRIPTOR_HANDLE cpuReadable, UINT mipLevels = 1) {
  D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
  d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  d.Format = fmt;
  d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  // Disk textures now carry a full mip chain (see CreateTextureFromPixels
  // genMips); an SRV pinned to 1 level would hide it and tex2Dlod would clamp
  // to the sharp top mip -- which is exactly how Heartfelt I lost its fog.
  d.Texture2D.MipLevels = mipLevels;
  dev->CreateShaderResourceView(res, &d, cpu);
  dev->CreateShaderResourceView(res, &d, cpuReadable);
}

#endif // MDROP_DXCONTEXT_H
