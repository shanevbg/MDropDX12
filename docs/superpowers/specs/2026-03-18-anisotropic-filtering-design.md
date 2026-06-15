# Anisotropic Filtering Support

**Date:** 2026-03-18
**Status:** Approved
**Context:** [Issue #30](https://github.com/shanevbg/MDropDX12/issues/30) — user asked whether MDropDX12 renders with anisotropy

## Summary

Add a checkbox to the Visual ToolWindow that enables 16x anisotropic filtering on the two linear static samplers (s0, s1). Default: off (preserving current bilinear behavior).

## Current State

- 4 static samplers baked into the root signature at init (`dxcontext.cpp:915-936`)
- s0 (`_samp_lw`): `MIN_MAG_MIP_LINEAR` + WRAP — default for most textures
- s1 (`_samp_lc`): `MIN_MAG_MIP_LINEAR` + CLAMP — fc_main, blur
- s2 (`_samp_pc`): `MIN_MAG_MIP_POINT` + CLAMP
- s3 (`_samp_pw`): `MIN_MAG_MIP_POINT` + WRAP
- `m_bAnisotropicFiltering` exists in `engine.h:672` but is commented out
- INI read/write stubs exist in `engine.cpp:1477,1776` but are commented out
- `m_blurRootSignature` is assigned as a copy of `m_rootSignature` (line 960)

## Design

### Member Variable

Uncomment `m_bAnisotropicFiltering` in `engine.h:672`. Change default from `true` to `false` (in `OverrideDefaults`, `engine.cpp:1193`).

### INI Persistence

Uncomment and update to wide-string API (matching current code style):
- Read: `m_bAnisotropicFiltering = GetPrivateProfileBoolW(L"Settings", L"bAnisotropicFiltering", m_bAnisotropicFiltering, pIni);`
- Write: `WritePrivateProfileIntW(m_bAnisotropicFiltering, L"bAnisotropicFiltering", pIni, L"Settings");`

### Sampler Changes (`dxcontext.cpp`)

Add `bool m_bAnisotropicFiltering` to `DXContext` (in `dxcontext.h`). In `CreateRootSignature()`, after the sampler loop:

```cpp
if (m_bAnisotropicFiltering) {
    staticSamplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    staticSamplers[0].MaxAnisotropy = 16;
    staticSamplers[1].Filter = D3D12_FILTER_ANISOTROPIC;
    staticSamplers[1].MaxAnisotropy = 16;
}
```

Only s0 and s1 are affected — s2/s3 are POINT samplers where anisotropy doesn't apply.

### Runtime Toggle

Static samplers are baked into the root signature, which is referenced by all PSOs. Changing the filter at runtime requires:

1. `WaitForGpu()` — drain the pipeline
2. Release and recreate root signature (with new sampler config)
3. Release and recreate all PSOs (they reference root signature)
4. Rebuild buffers (existing `ResetBufferAndFonts` flow)

Implementation: Add `RenderCmd::ResetPipeline` to `render_commands.h` (after `Quit`, line 31). Add a new window message `WM_MW_RESET_PIPELINE` (`WM_APP + 27`) in `engine_helpers.h`.

Add `RecreateRootSigAndPipelines()` to `DXContext`:
```cpp
bool DXContext::RecreateRootSigAndPipelines() {
    WaitForGpu();
    m_rootSignature.Reset();
    m_blurRootSignature.Reset();
    for (auto& pso : m_PSOs) pso.Reset();
    if (!CreateRootSignature()) return false;
    if (!CreatePipelines()) return false;
    return true;
}
```

**Important:** `m_PSOs[]` only covers the fixed pipeline PSOs from `DX12CreatePipelines`. There are ~15 additional PSOs created in `AllocateMyDX9Stuff`/`LoadShaders` (preset warp/comp, fallback, blur, inject effect, spout, buffer A/B/C/D). These also reference `m_rootSignature` and become invalid after recreation.

The safe approach: the `ResetPipeline` handler does `RecreateRootSigAndPipelines()` (root sig + fixed PSOs), then a full `CleanUpDX9Stuff(0)` + `AllocateDX9Stuff()` cycle (which rebuilds all preset/shader PSOs from scratch). This is what `ResetBufferAndFonts` already does — so the handler becomes:

```cpp
RecreateRootSigAndPipelines()   // root sig + m_PSOs[]
ResetBufferAndFonts()           // CleanUpDX9Stuff + AllocateDX9Stuff (rebuilds all other PSOs)
```

Handler chain:
- `engine_visual_ui.cpp` checkbox toggle → `PostMessage(hw, WM_MW_RESET_PIPELINE, 0, 0)`
- `engine_input.cpp` WM_MW_RESET_PIPELINE → `EnqueueRenderCmd(RenderCmd::ResetPipeline)`
- `engineshell.cpp` RenderCmd::ResetPipeline → `m_lpDX->RecreateRootSigAndPipelines()` + `ResetBufferAndFonts()`

The plugin sets `m_lpDX->m_bAnisotropicFiltering` before posting the message.

### Startup Propagation

At startup, `m_bAnisotropicFiltering` is read from INI into Engine but `CreateRootSignature()` runs on DXContext. The Engine must propagate its value to `m_lpDX->m_bAnisotropicFiltering` before `CreateRootSignature()` is called. This should happen in the same location where other Engine→DXContext settings are propagated (after `MyReadConfig` but before DX12 device init).

### Visual Window UI (`engine_visual_ui.cpp`)

Add checkbox below the Texture Precision combo (after line ~100):

```cpp
TrackControl(CreateCheck(hw, L"Anisotropic Filtering", IDC_MW_ANISO, x, y, 200, lineH, hFont, p->m_bAnisotropicFiltering, true));
y += lineH + spacing;
```

Control ID: `IDC_MW_ANISO` = `2193` (after `IDC_MW_WTP_WINDOWS` = 2192) in `engine_helpers.h`.

Handler in `DoCommand()` BN_CLICKED section:
```cpp
case IDC_MW_ANISO: {
    p->m_bAnisotropicFiltering = IsChecked(IDC_MW_ANISO);
    p->m_lpDX->m_bAnisotropicFiltering = p->m_bAnisotropicFiltering;
    HWND hw = p->GetPluginWindow();
    if (hw) PostMessage(hw, WM_MW_RESET_PIPELINE, 0, 0);
    break;
}
```

Reset button handler: add `SetChecked(IDC_MW_ANISO, false)` and `p->m_bAnisotropicFiltering = false`.

## Files Changed

| File | Change |
|------|--------|
| `engine.h` | Uncomment `m_bAnisotropicFiltering` |
| `engine.cpp` | Uncomment default, INI read, INI write (update to wide-string API) |
| `dxcontext.h` | Add `bool m_bAnisotropicFiltering = false`, declare `RecreateRootSigAndPipelines()` |
| `dxcontext.cpp` | Use flag in `CreateRootSignature()`, implement `RecreateRootSigAndPipelines()` |
| `render_commands.h` | Add `ResetPipeline` enum value |
| `engine_helpers.h` | Add `IDC_MW_ANISO` (2116), `WM_MW_RESET_PIPELINE` (WM_APP + 27) |
| `engine_visual_ui.cpp` | Add checkbox, handler, reset logic |
| `engine_input.cpp` | Handle `WM_MW_RESET_PIPELINE` → enqueue `RenderCmd::ResetPipeline` |
| `engineshell.cpp` | Handle `RenderCmd::ResetPipeline` → recreate root sig + PSOs + buffers |

## Not In Scope

- Per-sampler anisotropy control (overkill — 16x is free on modern GPUs)
- "Texture snapping" control (BeatDrop feature, not requested)
- Anisotropy on POINT samplers (not applicable)
