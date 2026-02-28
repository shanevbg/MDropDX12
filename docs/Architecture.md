# MDropDX12 Architecture: Why 64-bit DirectX 12

MDropDX12 is a ground-up rebuild of the MilkDrop2 music visualizer engine on DirectX 12 and x64. This document explains the architectural benefits of moving to a modern GPU API and 64-bit native code.

## Why DirectX 12

### Explicit Resource Management

DX9Ex relied on the driver to manage GPU memory, resource transitions, and synchronization behind the scenes. DX12 gives the application direct control over:

- **Resource states**: The engine explicitly transitions render targets between `RENDER_TARGET` and `PRESENT` states via barriers, eliminating hidden driver overhead.
- **Descriptor heaps**: SRV slots for textures, font atlases, and render targets are allocated in a single GPU-visible heap with a deterministic layout.
- **Command lists**: Draw calls are recorded into command lists and submitted to the GPU in batches, reducing per-draw CPU overhead.

### Reduced Driver Overhead

DX9 drivers performed significant behind-the-scenes work on every API call — state validation, hazard tracking, and implicit synchronization. DX12 moves this responsibility to the application, resulting in lower per-frame CPU cost. For a visualizer running at 60+ FPS with continuous shader execution, this matters.

### Modern Shader Compilation

MDropDX12 uses the DirectX Shader Compiler (DXC) targeting Shader Model 6. Benefits include:

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

The x86 process memory limit (~2 GB user space on Windows, ~4 GB with LAA) is gone. This gives headroom for shader caches, texture atlases, audio buffers, and the GDI overlay — all running simultaneously without pressure.

## D3D11On12 Interop

MDropDX12 uses Microsoft's D3D11On12 interop layer for two purposes:

1. **Spout texture sharing**: The Spout2 SDK's `spoutDX12` class wraps DX12 backbuffers as D3D11 resources via `CreateWrappedResource`, enabling zero-copy texture output to OBS, Resolume, and other Spout receivers. No intermediate copies are required.

2. **Direct2D text rendering** (if needed): D3D11On12 can also bridge Direct2D/DirectWrite onto DX12 surfaces, though MDropDX12 currently uses a GDI overlay for text.

The interop layer adds minimal overhead — it shares the same GPU command queue and operates on already-rendered frames.

## TDR Recovery

A Timeout Detection and Recovery (TDR) occurs when Windows resets the GPU after detecting a hang. In DX9, TDR was fatal — the visualizer would crash. MDropDX12 handles TDR gracefully:

- `ID3D12Device::GetDeviceRemovedReason()` detects the lost device
- The entire DX12 pipeline (device, swap chain, render targets, descriptors) is rebuilt
- Window focus is restored if MDropDX12 was the foreground window
- The user can also manually trigger a device restart from the Settings UI

This is especially important for a visualizer that runs for hours and executes arbitrary user-authored shaders.

## GDI Overlay HUD

Text rendering in DX12 typically requires either Direct2D interop or a custom text atlas renderer. MDropDX12 takes a different approach: a transparent layered window (GDI) sits on top of the DX12 render window.

Benefits:

- **Decoupled from the render pipeline**: Text updates don't interfere with DX12 command list recording or GPU timing.
- **Standard GDI text**: Full access to Windows font rendering, anti-aliasing, and Unicode support without managing font atlas textures in the descriptor heap.
- **Independent refresh**: The overlay can update at its own cadence (e.g., FPS counter every 500ms) without forcing a DX12 frame.

The overlay runs on its own thread and communicates with the render thread via atomic flags.

## Future Opportunities

The DX12 base opens paths that were not feasible on DX9:

- **Compute shaders**: Audio analysis (FFT, beat detection) could move to the GPU, freeing CPU cycles.
- **Window-independent Spout resolution**: Rendering to an off-screen render target at a fixed resolution, decoupled from the window size.
- **Multi-adapter**: DX12's explicit adapter enumeration allows targeting specific GPUs on multi-GPU systems.
- **Raytracing (DXR)**: Future presets could incorporate hardware raytracing for reflections, shadows, or volumetric effects via DXR 1.0/1.1.
