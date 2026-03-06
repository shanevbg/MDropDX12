# GLSL→HLSL Converter Debugging

## Overview

This document tracks debugging of the GLSL→HLSL converter (`engine_shader_import_ui.cpp`) for Shadertoy shader import. The converter translates Shadertoy GLSL into MilkDrop-compatible HLSL for the Shadertoy render pipeline (`.milk3` format).

## Fixed Issues

### 1. texelFetch whitespace mismatch (FIXED)

**Symptom**: Flat terrain in "Elevated" shader, system stutters from raymarcher non-convergence.

**Root cause**: Shadertoy GLSL uses `texelFetch( iChannel0, ...)` (space after paren). After conversion to `texelFetch_conv( sampler_noise_lq, ...)`, the specialization to `texelFetch_noise` didn't match because it expected no space.

**Fix**: Normalize whitespace before specialization:
```cpp
while (inp.find("texelFetch_conv( ") != std::string::npos)
    replaceAll(inp, "texelFetch_conv( ", "texelFetch_conv(");
```

**Why it matters**: `texelFetch_noise` divides by 256.0 (noise texture size). `texelFetch_conv` uses `texsize.zw` (screen size). With `&255` coordinate wrapping, screen-sized division gives UV ≈ 0.13 instead of ≈ 1.0, sampling a tiny corner.

### 2. Buffer A ch0 default was CHAN_FEEDBACK (FIXED)

**Symptom**: Buffer A shaders that read noise via iChannel0 (e.g., Elevated) got `sampler_feedback` instead, causing wrong texelFetch UV calculation and GPU stalls.

**Root cause**: ADD_PASS handler set `bufA.channels[0] = CHAN_FEEDBACK`. Most Shadertoy Buffer A shaders use noise on ch0, not self-feedback.

**Fix**: Removed the CHAN_FEEDBACK override. ShaderPass defaults to `{CHAN_NOISE_LQ, CHAN_NOISE_LQ, CHAN_NOISE_MQ, CHAN_NOISE_HQ}`. Only Image ch0 is set to CHAN_FEEDBACK when Buffer A exists.

### 3. Vector l-value indexing X3500/X3550 (FIXED)

**Symptom**: Shader compilation error `X3500: array reference cannot be used as an l-value` in loops like:
```hlsl
for (int ch = 0; ch < 3; ch++) {
    result[ch] = overlapPrev2 + overlapPrev1 + overlapCurr;  // X3500
}
```

**Root cause**: HLSL doesn't support writing to `float3` components via dynamic index. `continue` in the loop prevents auto-unrolling (X3511).

**Fix**: Post-processing pass replaces `vec[dynamic_idx] = expr;` with `_setComp(vec, idx, expr);`. Helper functions use static `.x`/`.y`/`.z` access:
```hlsl
void _setComp(inout float3 v, int i, float val) {
    if (i == 0) v.x = val; else if (i == 1) v.y = val; else v.z = val;
}
```

### 4. ReplaceVarName missing patterns (FIXED)

**Symptom**: `undeclared identifier '_uv'` in CRT shader. `tex2Dlod_conv(sampler_noise_lq, uv, 0.0)` — the `uv` between commas wasn't renamed to `uv_conv`.

**Fix**: Added patterns for `, uv,` and ` uv,` to ReplaceVarName.

### 5. Missing sampler declarations (FIXED)

**Symptom**: `undeclared identifier 'sampler_audio'` / `'sampler_rand00'`.

**Fix**: Added declarations to embedded_shaders.h:
```hlsl
sampler2D sampler_rand00;  // through sampler_rand03 (auto-assigned)
sampler2D sampler_feedback : register(s14);
sampler2D sampler_image : register(s15);
sampler2D sampler_audio : register(s10);
```

## Channel Mapping

Per-pass channel configuration stored in `ShaderPass::channels[4]`:

```cpp
enum ChannelSource {
    CHAN_NOISE_LQ = 0,   // sampler_noise_lq_st (256x256, Shadertoy-compatible)
    CHAN_NOISE_MQ,       // sampler_noise_mq_st (256x256)
    CHAN_NOISE_HQ,       // sampler_noise_hq_st (256x256)
    CHAN_FEEDBACK,       // sampler_feedback (screen-sized, Buffer A output)
    CHAN_NOISEVOL_LQ,    // sampler_noisevol_lq_st (3D 32x32x32)
    CHAN_NOISEVOL_HQ,    // sampler_noisevol_hq_st (3D 32x32x32)
    CHAN_IMAGE_PREV,     // sampler_image (previous frame)
    CHAN_AUDIO,          // sampler_audio (512x2 FFT+wave)
    CHAN_RANDOM_TEX,     // sampler_rand00 (random disk texture)
    CHAN_BUFFER_B,       // sampler_bufferB (screen-sized, Buffer B output)
};
```

Two sampler name tables exist:
- `kChannelSamplers[]` — MilkDrop noise names (cubically interpolated, centered-range)
- `kChannelSamplers_ST[]` — Shadertoy-compatible noise names (`_st` suffix: uniform white noise, full [0,255], deterministic seed)

Shadertoy imports use `kChannelSamplers_ST[]` for iChannel→sampler mapping.

**Defaults**: ShaderPass initializes to `{NOISE_LQ, NOISE_LQ, NOISE_MQ, NOISE_HQ}`.
When adding Buffer A, only **Image ch0** is set to CHAN_FEEDBACK (reads Buffer A output).
Buffer A ch0 stays at CHAN_NOISE_LQ (most Shadertoy shaders use noise, not self-feedback).

**`channelsFromJSON` flag**: When a .milk3 JSON has a `channels` block, `ShaderPass::channelsFromJSON` is set to `true`. This tells `AnalyzeChannels()` to trust the JSON values and skip low-confidence guessing (Pattern 3), while still allowing high-confidence overrides (audio, 3D texture detection).

**Elevated shader example:**

- **Buffer A**: iChannel0 → `sampler_noise_lq_st` (256x256 noise for terrain generation)
- **Image**: iChannel0 → `sampler_feedback` (reads Buffer A output for motion blur)

**Selfie shader example (multi-pass):**

- **Buffer A**: iChannel0 → `sampler_noisevol_lq_st` (3D noise), iChannel1 → `sampler_bufferB` (temporal feedback from Buffer B)
- **Buffer B**: iChannel0 → `sampler_feedback` (reads Buffer A output for DoF blur)
- **Image**: iChannel0 → `sampler_feedback` (reads Buffer A), iChannel1 → `sampler_bufferB` (reads Buffer B)

## texelFetch Helpers

Two helpers are injected based on the sampler's known texture dimensions:

```hlsl
// For noise textures (256x256):
float4 texelFetch_noise(sampler2D s, int2 c, int l) {
    return tex2Dlod(s, float4((float2(c) + 0.5) / 256.0, 0, l));
}

// For screen-sized textures (feedback buffer):
float4 texelFetch_conv(sampler2D s, int2 c, int l) {
    return tex2Dlod(s, float4((float2(c) + 0.5) * texsize.zw, 0, l));
}
```

Specialization runs AFTER iChannel→sampler replacement and whitespace normalization.
Both regular and `_st` noise sampler names are specialized:

```cpp
replaceAll(inp, "texelFetch_conv(sampler_noise_lq,", "texelFetch_noise(sampler_noise_lq,");
replaceAll(inp, "texelFetch_conv(sampler_noise_lq_st,", "texelFetch_noise(sampler_noise_lq_st,");
// ... same for noise_mq, noise_hq and their _st variants
```

## Register Slot Layout

```
s0-s4   : main samplers (main/FW/FC/PW/PC from preset)
s5-s8   : auto-assigned (noise_lq/mq/hq, noisevol_lq/hq, _st variants)
s9      : sampler_bufferB
s10     : sampler_audio
s11-s13 : sampler_blur1/2/3
s14     : sampler_feedback
s15     : sampler_image
(auto)  : sampler_rand00-03
```

## Conversion Pipeline Order

1. Comment stripping and line ending normalization
2. **Phase 1**: Global text replacements (GLSL→HLSL types, functions, uniforms)
   - `texelFetch(` → `texelFetch_conv(` (then normalize whitespace, then specialize for noise)
   - iChannel0-3 → configured sampler names from ShaderPass::channels[]
   - Matrix type replacements (mat2→float2x2, etc.)
3. **Phase 1b**: Matrix variable/function collection, mul-swap for square matrices
4. **Phase 2**: Extract `mainImage()` body, build `shader_body` wrapper with helpers
5. **Phase 3**: Per-line processing (FixMatrixMultiplication, FixFloatNumberOfArguments, FixAtan)
6. Backslash continuation joining
7. Post-processing:
   - Vector l-value fix (`_setComp` helpers)
   - Over-specified constructors (`float3(expr,expr,expr)` → `((float3)(expr))`)
   - Inout struct parameter transformation

## Diagnostic Files

Per-pass diagnostic dumps written to `m_szBaseDir` (same dir as exe / debug.log):

- `diag_converter_image.txt` — Image pass converted HLSL
- `diag_converter_bufferA.txt` — Buffer A pass converted HLSL
- `diag_converter_bufferB.txt` — Buffer B pass converted HLSL
- `diag_converter_common.txt` — Common tab GLSL (prepended to all passes)
- `diag_comp_shader.txt` — Full compiled Image shader (with headers)
- `diag_comp_shader_error.txt` — Image shader compilation result
- `diag_bufferA_shader.txt` — Full compiled Buffer A shader (with headers)
- `diag_bufferA_shader_error.txt` — Buffer A shader compilation result
- `diag_bufferB_shader.txt` — Full compiled Buffer B shader (with headers)
- `diag_bufferB_shader_error.txt` — Buffer B shader compilation result

## Shadertoy Render Pipeline

```text
Frame N:
  Buffer A: reads FeedbackA[read] + FeedbackB[read] → writes FeedbackA[write]   (FLOAT32)
  Buffer B: reads FeedbackA[read] + FeedbackB[read] → writes FeedbackB[write]   (FLOAT32)
  Image:    reads FeedbackA[write] + FeedbackB[write] → writes backbuffer        (UNORM)
  Swap feedback indices (m_nFeedbackIdx ^= 1)
```

- All buffer passes read from the **previous frame** (`fbRead`)
- Image reads from the **current frame** (`fbWrite`)
- Feedback buffers are `R32G32B32A32_FLOAT` at VS resolution (`m_nTexSizeX × m_nTexSizeY`)
- sRGB gamma correction applied via `RenderInjectEffect()` post-process (`mode.z=1`)
- `PASSES_PER_FRAME = 4` (warp + bufferA + bufferB + comp) for descriptor heap layout

## Shadertoy-Compatible Noise Textures

MilkDrop noise textures (`sampler_noise_lq` etc.) use cubically interpolated, centered-range noise — not what Shadertoy shaders expect. Shadertoy noise is uniform white noise, full [0,255] range, no interpolation, deterministic seed.

The `_st` suffix textures match Shadertoy's noise characteristics:

- `noise_lq_st`, `noise_mq_st`, `noise_hq_st` — 2D 256×256 (`AddNoiseTex_ST`)
- `noisevol_lq_st`, `noisevol_hq_st` — 3D 32×32×32 (`AddNoiseVol_ST`)

Generated in `engine_textures.cpp`, registered as built-in in `kBuiltinNoise[]` (engine_shaders.cpp) to prevent disk-load attempts. Declared in `embedded_shaders.h`.

## Test Shaders

| Shader | URL | Structure | Tests |
| --- | --- | --- | --- |
| Elevated | shadertoy.com/view/MdX3Rr | Buffer A + Image | texelFetch noise, terrain raymarching, mat2 mul |
| CRT simulation | (local crt.json) | Image only | Vector l-value indexing, `_setComp` fix |
| Urchin Sound | (local urchinsound.json) | Image only | Audio texture, sampler_audio binding |
| Butterfly Flock | (local butterflyflock.json) | Image only | Array of structs, nested loops, camera rotation |
| Selfie Girl | (local selfie.json) | Common + Buffer A + Buffer B + Image | Temporal accumulation, 3D noise, camera reprojection |

## Key Files

| File | Role |
| --- | --- |
| `src/mDropDX12/engine_shader_import_ui.cpp` | GLSL→HLSL converter, UI, save/load |
| `src/mDropDX12/tool_window.h` | ShaderPass struct, ShaderEditorWindow, ShaderImportWindow |
| `src/mDropDX12/engine_helpers.h` | Control IDs for shader import/editor UI |
| `src/mDropDX12/embedded_shaders.h` | Sampler declarations with register assignments |
| `src/mDropDX12/engine_textures.cpp` | Noise texture generation (including `_st` variants) |
| `src/mDropDX12/engine_shaders.cpp` | Shader compilation, texture binding, `kBuiltinNoise[]` |
| `src/mDropDX12/milkdropfs.cpp` | Shadertoy render pipeline (`RenderFrameShadertoy`) |
| `docs/GLSL_importing.md` | General GLSL→HLSL conversion reference |
