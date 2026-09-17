# MDropDX12 Architecture: Why 64-bit DirectX 12

MDropDX12 is a ground-up rebuild of the MilkDrop2 music visualizer engine on DirectX 12 and x64. This document explains the architectural benefits of moving to a modern GPU API and 64-bit native code.

## Why DirectX 12

### Explicit Resource Management

DX9Ex relied on the driver to manage GPU memory, resource transitions, and synchronization behind the scenes. DX12 gives the application direct control over:

- **Resource states**: The engine explicitly transitions render targets between `RENDER_TARGET` and `PRESENT` states via barriers, eliminating hidden driver overhead.
- **Descriptor heaps**: SRV slots for textures, font atlases, and render targets are allocated in a single GPU-visible heap with a deterministic layout.
- **Command lists**: Draw calls are recorded into command lists and submitted to the GPU in batches, reducing per-draw CPU overhead.

### Reduced Driver Overhead

DX9 drivers performed significant behind-the-scenes work on every API call — state validation, hazard tracking, and implicit synchronization. DX12 moves this responsibility to the application. For a visualizer running at 60+ FPS with continuous shader execution, the cumulative savings are substantial.

**Pipeline State Objects.** In DX9, every draw call required the driver to validate dozens of individual state settings — blend mode, rasterizer state, depth/stencil, shader pairs, input layout, and more. The driver maintained an internal state machine and re-validated consistency on each draw. MDropDX12 bakes all of this into immutable Pipeline State Objects at initialization time. At draw time, a single `SetPipelineState()` call replaces what was previously 50+ individual state-setting calls, all pre-validated.

**Command list batching.** DX9 validated every `DrawPrimitive()` call individually as it was issued. MDropDX12 records all draw calls into a single command list per frame and submits the entire list to the GPU in one `ExecuteCommandLists()` call. The warp pass alone records over 200 mesh draw calls — all batched with no per-draw driver validation. The GPU processes the entire pre-recorded sequence without CPU intervention.

**Explicit resource barriers.** DX9 drivers implicitly tracked when textures were being read versus written, and silently inserted GPU stalls when hazards were detected. This hazard tracking ran on every draw call, hidden from the application. MDropDX12 instead places explicit resource barriers at render pass boundaries — transitioning VS[0] to shader-readable before the warp pass, VS[1] to render-target, and so on. The GPU sees the full data dependency graph upfront and can pipeline operations without hidden stalls.

**Descriptor heap binding.** In DX9, each `SetTexture()` call required the driver to validate the texture format against shader expectations, update internal binding tables, and check sampler compatibility. With 16 texture slots used across multiple render passes, this added up to thousands of validation operations per frame. MDropDX12 pre-allocates a single GPU-visible descriptor heap at startup, copies texture descriptors into per-frame binding blocks once, and sets one descriptor table per render pass — replacing thousands of individual binding calls with a handful of bulk operations.

**Static samplers.** DX9 required separate `SetSamplerState()` calls to configure filtering, address mode, and anisotropy for each texture slot on each draw. MDropDX12 compiles all 16 sampler configurations into the root signature at initialization — wrap samplers for most slots, clamp samplers for blur textures. At draw time, there are zero sampler state calls.

**Fence-based frame synchronization.** DX9's `Present()` blocked the CPU until the GPU finished rendering. MDropDX12 uses per-frame fence values and double-buffered command allocators. The CPU signals a fence after submitting each frame, then begins preparing the next frame immediately. It only waits if the GPU is still using the target backbuffer. This lets the CPU stay one frame ahead of the GPU instead of idling during GPU work.

The net effect is that per-frame driver overhead drops from thousands of validation operations to a small number of explicit, pre-validated commands — freeing CPU time for audio processing, expression evaluation, and shader compilation.

### Modern Shader Compilation

MDropDX12 compiles preset HLSL at runtime with `D3DCompile` (`d3dcompiler_47.dll`), targeting Shader Model 5.0. The profile a preset declares — `ps_2_0` through `ps_5_0`, bounded by the `MinPSVersion` / `MaxPSVersion` settings — selects how the preset's source is translated, and the compatibility shim compiles the result at `ps_5_0` with `D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY` so legacy `.milk`/`.milk2` shader code builds unchanged. `.milk3` Shadertoy presets declare Shader Model 5.0 outright. Benefits include:

- **Async compilation**: Preset transitions compile the next preset's shaders on a background thread. The visualizer never stalls waiting for a shader to compile.
- **Compile timeout**: If a shader takes too long (malformed or pathologically complex), it is automatically skipped — preventing GPU hangs.
- **Shader caching**: Compiled shader bytecode is cached to disk, so presets load instantly on subsequent runs.

## Why x64

### Larger Address Space

MilkDrop2 on x86 was limited to ~2 GB of virtual address space. Texture-heavy presets that reference multiple high-resolution images could approach this limit. The x64 build eliminates this constraint entirely.

### Native 64-bit JIT (ns-eel2)

MDropDX12 uses ns-eel2 for real-time expression evaluation in presets (per-frame and per-point equations). The ns-eel2 JIT compiler generates native x64 machine code, giving:

- Full use of 64-bit registers and SSE/AVX instructions
- No WoW64 translation overhead
- Larger JIT code buffers for complex preset expressions

The original MilkDrop2 used ns-eel2 in x86 mode. MilkDrop3 still ships as x86. MDropDX12 is the first MilkDrop-based visualizer to run ns-eel2 natively on x64.

### No 4 GB Memory Limit

The x86 process memory limit (~2 GB user space on Windows, ~4 GB with LAA) is gone. This gives headroom for shader caches, texture atlases, audio buffers, and per-display render surfaces — all running simultaneously without pressure.

## D3D11On12 Interop

MDropDX12 uses Microsoft's D3D11On12 interop layer for Spout texture sharing
only, in both directions: the Spout2 SDK's `spoutDX12` class wraps DX12
backbuffers as D3D11 resources via `CreateWrappedResource` for sending to OBS,
Resolume and other receivers, and the Spout input path bridges received
textures in the same way. No intermediate copies are required. It was formerly
also the path for Direct2D/DirectWrite text, which the font atlas renderer
below replaced.

The interop layer adds minimal overhead — it shares the same GPU command queue and operates on already-rendered frames.

## TDR Recovery

A Timeout Detection and Recovery (TDR) occurs when Windows resets the GPU after
detecting a hang. In DX9 this was fatal. MDropDX12 recovers — but **the two
kinds of device loss are recovered differently, and conflating them is what made
an earlier attempt fail.**

`ID3D12Device::GetDeviceRemovedReason()` detects the loss and distinguishes them.

**`DXGI_ERROR_DEVICE_REMOVED` / `_RESET` — recovered in place.** A driver
update, a reset, an adapter change. The whole DX12 pipeline (device, swap chain,
render targets, descriptors, per-surface mirror objects) is rebuilt and the
visualizer carries on. `PerformDeviceRecovery` in `App.cpp` does this, and the
user can trigger the same path deliberately from the Settings UI or over IPC
with `RESTART_DEVICE`.

**`DXGI_ERROR_DEVICE_HUNG` — the process relaunches itself.** In-process
recovery was implemented for this case and then measured: after a hang the
adapter refuses **the offending process** for around 120 seconds, while a
freshly started process is served a device in about 22 seconds. Relaunching
clears it; whether a genuinely complete in-process release would too has never
been tested — the process still holds device objects at that point. So a hang
starts a successor and exits, because a fresh process is the one teardown that
is provably complete.

Three things cross to the successor on its command line as
`-tdrrelaunch=<oldpid>,<attempt>,<ceiling>`, because a relaunch that carries
nothing simply repeats the death:

| | why |
|---|---|
| old pid | the successor waits for its predecessor to exit before asking for the adapter, rather than racing a dying process for it |
| attempt | the budget is a **session** budget of three, not three per process — otherwise every successor starts fresh and the bound is an infinite loop |
| ceiling | the mirror resolution steps down, 1920 → 1280 → 960 → mirrors off, so the successor does not restore the arrangement that just killed the GPU |

After three the application exits as it used to. The budget resets once an
instance has run healthily for five minutes, because what is bounded is a
*loop*, not a rare event — a machine that hangs once a month should not stop
recovering on the fourth month.

`DIAG_SIMULATE_TDR` (testing mode only) drives the whole response with no GPU
involved, so the chain can be exercised without resetting the display driver for
the entire machine.

This matters for a visualizer that runs for hours executing arbitrary
user-authored shaders.

## Font Atlas HUD Text

HUD text — preset name, FPS, menus, playlist, tooltips, notifications — is
rendered by `CTextManager` (`textmgr.cpp`) as DX12 sprite quads sampled from
font atlas textures, drawn into the back buffer within the normal frame. Each
font's glyphs are rasterized once with GDI into an offscreen DIB and uploaded
as a permanent atlas texture; per-frame drawing is a queue of text entries
flushed by `DrawNow()` into the render thread's command list, which is why
screenshots and mirror panels include the HUD.

Message and song-title supertexts follow the same pattern at a larger grain:
GDI rasterizes the string into a DIB that is uploaded to a title texture,
which the engine draws warped.

There is no overlay window and no separate text thread. This replaced an
earlier D3D11On12 + Direct2D/DirectWrite pipeline; GDI survives only as an
offline glyph rasterizer feeding DX12 textures, and as ordinary Win32 painting
inside the tool windows.

## Future Opportunities

The DX12 base opens paths that were not feasible on DX9:

- **Compute shaders**: Audio analysis (FFT, beat detection) could move to the GPU, freeing CPU cycles.
- **Window-independent Spout resolution**: Rendering to an off-screen render target at a fixed resolution, decoupled from the window size.
- **Multi-adapter**: DX12's explicit adapter enumeration allows targeting specific GPUs on multi-GPU systems.
- **Raytracing (DXR)**: Future presets could incorporate hardware raytracing for reflections, shadows, or volumetric effects via DXR 1.0/1.1.
