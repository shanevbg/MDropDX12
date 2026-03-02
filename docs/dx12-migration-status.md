# DX9→DX12 Migration Status

## Completed Phases

### Phase 1-4: Core DX12 Rendering
- DX12 swap chain, command lists, PSOs, basic rendering all working
- `dx12pipeline.h` / `dx12pipeline.cpp` — PSO management, embedded HLSL vertex/pixel shaders
- PSO_PREMULALPHA_SPRITEVERTEX for text rendering

### Phase 5: Text Pipeline (CTextManager)
- `textmgr.h` / `textmgr.cpp` — GDI→DX12 text rendering pipeline
- GDI fonts render to DIB section → alpha fix → upload to DX12 texture → draw quad
- `GetFont()` returns font-index sentinel `(LPD3DXFONT)(intptr_t)(idx+1)` not real DX9 fonts
- Menu, help screen, preset info, FPS overlay all working

### Phase 6: HUD & Overlay
- GDI overlay window for HUD text (preset name, FPS, debug info, notifications)
- Song title animation — SKIPPED (never worked, low priority)
- Sprite system — SKIPPED (low priority)

### Phase 7: Texture System
- Custom disk texture loading via DX12 (WIC-based)
- Procedural noise textures: `noise_lq`, `noise_mq`, `noise_hq`, `noisevol_lq`, `noisevol_hq`
- 3D volume texture support for noise volumes
- Texture search paths: `m_szMilkdrop2Path\textures\`, `m_szPresetDir`, sibling `textures\` dir, fallback paths
- Clipboard textures (`sampler_clipboard00-15`) — standard disk textures, NOT render targets
- 1x1 white fallback texture for missing disk textures (prevents `ret *= 0` black-screen in multiplicative presets)

### Phase 8: Post-Processing Effects
- `RenderInjectEffect()` in plugin.cpp (~line 5387) — shader-based implementation
- Pixel shader applies: brighten (`ret.rgb * (2.0 - ret.rgb)`), darken (`ret.rgb * ret.rgb`), solarize (`ret.rgb * (1.0 - ret.rgb) * 2.0`), invert (`1.0 - ret.rgb`)
- Controlled by preset bitmask: `bBrighten`, `bDarken`, `bSolarize`, `bInvert`

## Recent Fixes (Feb 2026)

### DX9 Half-Texel Offset Removal
DX9 pixel centers are at integers; DX12 at integer+0.5. Legacy `0.5f / texSize` offsets caused sub-pixel misalignment with bilinear filtering → universal blur.

Fixed in 3 locations (all use `m_lpDX->m_device` check to keep DX9 path intact):
- **Comp mesh positions** — `plugin.cpp` ~line 2678: `fHalfTexelW`/`fHalfTexelH` set to 0.0 in DX12
- **Warp mesh `tu_orig`/`tv_orig`** — `plugin.cpp` ~line 2966: `texel_offset_x`/`y` set to 0.0 in DX12
- **Per-vertex warp UVs** — `milkdropfs.cpp` ~line 3405: `texel_offset_x`/`y` set to 0.0 in DX12

### DX9 Y-Flip Compensation Removal
DX9 `D3DXMatrixOrthoLH(2.0f, -2.0f, ...)` negated Y for all fixed-function rendering. Original code added explicit Y-flips to compensate. DX12 vertex shaders bypass projection (`output.pos = float4(input.pos, 1.0)`), so these compensations were removed.

Fixed in 4 locations:
- **Warp mesh vertices** — `milkdropfs.cpp` ~line 1971: Removed `tempv[i-1].y *= -1`
- **Wave vertices** — `milkdropfs.cpp` ~line 2879: Removed `v1[i].y = -v1[i].y`
- **Custom shape center** — `milkdropfs.cpp` ~line 3132: Changed `pf_y * -2 + 1` → `pf_y * 2 - 1`
- **Custom wave vertices** — `milkdropfs.cpp` ~lines 3325/3328: Changed `pp_y * -2 + 1` → `pp_y * 2 - 1`

### Comp Mesh Upgrade
- Upgraded from 2-triangle fullscreen quad to 32x24 grid (matches MilkDrop3)
- Enables per-vertex hue coloring used by comp shaders

### Texture Search Diagnostics
- `CacheParams()` logs texture search attempts to `debug.log` via `DebugLogA()`
- Logs: texture name being searched, paths tried, found/not-found result

## Ongoing Investigation: Spectrum Dots Rendering

MilkDrop2077 presets (e.g., `MilkDrop2077.Classic.Spectrum.Dots.Middle.004.milk`) render differently in MDropDX12 vs MilkDrop 3PRO:
- **MilkDrop3PRO**: Crisp distinct blue/cyan dots with clean edges, visible separation
- **MDropDX12**: Blurry pinkish smeared columns

### What we know
- Spectrum dots are **custom shapes** (`shapecode_0`, 1024 instances, 4-sided shapes), NOT the wave system
- The comp shader UV transform `uv = 1-abs(frac((uv-.5)*1.5*0.5)*2-1); uv = 1-uv;` is symmetric: f(t) = f(1-t). This makes Y-flip changes invisible for this specific preset.
- Half-texel offset removal should improve sharpness — needs runtime verification
- Presets use `sampler_clipboard03` for color grading (multiplicative lookup from `clipboard03.jpg`)

### If still blurry after half-texel fix, investigate
1. Render target resolution (`m_nTexSizeX`/`m_nTexSizeY`) vs display resolution
2. `m_bScreenDependentRenderMode` effect on shape sizing
3. Blur texture computation (GetBlur1/2/3) — comp shader depends on blur passes
4. Static sampler filter mode (`D3D12_FILTER_MIN_MAG_MIP_LINEAR`) — may need point filtering option for certain passes
5. Aspect ratio handling differences between MDropDX12 and MilkDrop3

## Key Code Locations

| Area | File | Location |
|------|------|----------|
| DX12 render loop | `milkdropfs.cpp` | `DX12_RenderWarpAndComposite()` ~line 1858 |
| Warp mesh init | `plugin.cpp` | `MakeGridVB()` ~line 2940 |
| Comp mesh init | `plugin.cpp` | Comp mesh setup ~line 2660 |
| Shader compilation | `plugin.cpp` | `CShaderParams::CacheParams()` ~line 4005 |
| Texture search | `plugin.cpp` | CacheParams pass 1 ~line 4008 |
| Post-processing | `plugin.cpp` | `RenderInjectEffect()` ~line 5387 |
| PSO definitions | `dx12pipeline.h` | Embedded HLSL + PSO enums |
| PSO creation | `dx12pipeline.cpp` | `CreatePSO()` variants |
| Custom shapes (DX12) | `milkdropfs.cpp` | Inside `DX12_RenderWarpAndComposite()` ~line 3100 |
| Custom waves (DX12) | `milkdropfs.cpp` | Inside `DX12_RenderWarpAndComposite()` ~line 3300 |
| Noise texture gen | `plugin.cpp` | `AddNoiseTex()` ~line 2788, `AddNoiseVol()` ~line 2927 |
| White fallback tex | `dxcontext.cpp` | `CreateWhiteFallbackTexture()` |
| Texture registry | `plugin.h` | `m_textures` — `std::vector<TexInfo>` ~line 470 |

## Build Notes

- Platform: x64
- Toolset: v143 (VS2022)
- Build command: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
- Output: `src/mDropDX12/Release_x64/MDropDX12.exe`
