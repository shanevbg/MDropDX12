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
#define DXC_MAX_SRV  4096
#define DXC_MAX_SAMPLERS 4

#define SNAP_WINDOWED_MODE_BLOCKSIZE  32

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
  void WaitForGpu();      // flush all in-flight GPU work and idle the queue
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

  // ----- DX12 objects (public for access from plugin render code) -----
  ComPtr<ID3D12Device>               m_device;
  ComPtr<ID3D12CommandQueue>         m_commandQueue;
  ComPtr<IDXGISwapChain4>            m_swapChain;
  ComPtr<ID3D12GraphicsCommandList>  m_commandList;
  // When set (e.g. independent-mirror orient pass), DrawVertices / TransitionResource
  // and DX12 draw helpers record into this list instead of the closed main list.
  ID3D12GraphicsCommandList*         m_cmdListOverride = nullptr;
  ID3D12GraphicsCommandList* GetActiveCmdList() const {
    return m_cmdListOverride ? m_cmdListOverride : m_commandList.Get();
  }

  // Per-frame resources (indexed by m_frameIndex)
  ComPtr<ID3D12CommandAllocator>     m_commandAllocators[DXC_FRAME_COUNT];

  // Back-buffer render targets
  ComPtr<ID3D12Resource>             m_renderTargets[DXC_FRAME_COUNT];

  // Descriptor heaps
  ComPtr<ID3D12DescriptorHeap>       m_rtvHeap;          // RTV heap (DXC_MAX_RTV entries)
  ComPtr<ID3D12DescriptorHeap>       m_srvHeap;          // SRV/CBV/UAV heap (shader-visible)
  ComPtr<ID3D12DescriptorHeap>       m_samplerHeap;      // Sampler heap (shader-visible)
  UINT                               m_rtvDescriptorSize;
  UINT                               m_srvDescriptorSize;
  UINT                               m_samplerDescriptorSize;

  // Bump allocators for descriptor slots (slots 0..N-1 for back buffers already taken)
  UINT                               m_nextFreeRtvSlot;  // starts at DXC_FRAME_COUNT
  UINT                               m_nextFreeSrvSlot;  // starts at 0

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

  // Helpers for allocating descriptor slots
  void DumpDredToLog(const char* whence = nullptr); // last GPU ops + page-fault VA after a device removal
  D3D12_CPU_DESCRIPTOR_HANDLE AllocateRtv();   // returns CPU handle, bumps m_nextFreeRtvSlot
  D3D12_CPU_DESCRIPTOR_HANDLE AllocateSrvCpu(); // returns CPU handle at m_nextFreeSrvSlot
  D3D12_GPU_DESCRIPTOR_HANDLE AllocateSrvGpu(); // returns GPU handle, bumps m_nextFreeSrvSlot
  D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandleAt(UINT srvIndex); // handle at specific index, no bump
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

  // Texture creation helpers (Phase 2)
  DX12Texture CreateRenderTargetTexture(UINT width, UINT height, DXGI_FORMAT format);

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
  ComPtr<ID3D12Resource> m_mirrorUploadBuffer;
  BYTE*                  m_mirrorUploadPtr = nullptr;
  UINT                   m_mirrorUploadOffset = 0;
  DWORD                  m_mirrorUploadThreadId = 0;
  bool IsMirrorUploadThread() const {
    return m_mirrorUploadThreadId != 0 && GetCurrentThreadId() == m_mirrorUploadThreadId;
  }
  // Allocated only while independent mirrors are on; released with the thread.
  bool CreateMirrorUploadBuffer();
  void ReleaseMirrorUploadBuffer();
  void SetMirrorUploadThreadId(DWORD tid) { m_mirrorUploadThreadId = tid; }
  // Picks main / aux / mirror ring for the calling thread.
  void SelectUploadRing(BYTE*& basePtr, ID3D12Resource*& baseRes,
                        UINT*& pOffset, UINT& bufSize);

  bool CreateUploadBuffers();
  void ResetUploadBuffer(); // called at BeginFrame — resets offset to 0
  void BeginAuxUpload() {
    // The worker owns its own ring and must never touch the shared aux state.
    if (IsMirrorUploadThread()) {
      m_mirrorUploadOffset = 0;
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
    if (IsMirrorUploadThread() && m_mirrorUploadPtr) {
      if (m_mirrorUploadOffset >= AUX_UPLOAD_BUFFER_SIZE)
        return 0;
      return AUX_UPLOAD_BUFFER_SIZE - m_mirrorUploadOffset;
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
    if (IsMirrorUploadThread() && m_mirrorUploadPtr) return AUX_UPLOAD_BUFFER_SIZE;
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
  // Load a texture from an image file via WIC
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
  void UpdateBlurPassBinding(UINT passIndex, UINT sourceSrvIndex);
  D3D12_GPU_DESCRIPTOR_HANDLE GetBlurPassBindingGpuHandle(UINT passIndex);

  // Current frame index within [0, DXC_FRAME_COUNT)
  UINT  m_frameIndex;

  // Fence for CPU/GPU synchronisation
  ComPtr<ID3D12Fence> m_fence;
  UINT64              m_fenceValues[DXC_FRAME_COUNT];
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

inline void CreateSRV2D(ID3D12Device* dev, ID3D12Resource* res,
    DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE cpu, UINT mipLevels = 1) {
  D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
  d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  d.Format = fmt;
  d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  // Disk textures now carry a full mip chain (see CreateTextureFromPixels
  // genMips); an SRV pinned to 1 level would hide it and tex2Dlod would clamp
  // to the sharp top mip -- which is exactly how Heartfelt I lost its fog.
  d.Texture2D.MipLevels = mipLevels;
  dev->CreateShaderResourceView(res, &d, cpu);
}

#endif // MDROP_DXCONTEXT_H
