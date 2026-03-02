# MDropDX12 Preset Debugging Guide

Reference visualizer for comparison: **MilkDrop 3PRO** (MilkDrop3) at `c:\Code\Entertainment\MilkDrop3\`

---

## Rendering Pipeline Overview

```
  VS[0] (previous frame)
    |
    v
  Warp Pass ──> VS[1]       Reads VS[0], applies warp mesh distortion, writes VS[1]
    |
    v
  Blur Passes               Reads VS[0] (pass 0) then chained blur textures
    |
    v
  Custom Shapes/Waves ──> VS[1]   Drawn directly into VS[1] (alpha-blended)
    |
    v
  Comp Pass ──> Backbuffer  Reads VS[1] + blur textures, applies comp shader
```

- **VS[0] and VS[1] ping-pong** each frame. After comp writes to backbuffer, VS[1] becomes VS[0] next frame.
- **Blur passes read VS[0]** (the previous frame), not VS[1]. This is correct and matches MilkDrop3.
- **Comp shader binding = VS[1]**. NEVER change to VS[0].
- **Comp pass uses a fullscreen quad** in MDropDX12 (not the comp grid mesh).

### Two Display Paths

The comp pass has two paths, selected based on whether the preset has a custom comp shader:

| | `ShowToUser_Shaders` (has comp shader) | `ShowToUser_NoShaders` (no comp shader) |
|---|---|---|
| **Gamma** | NOT applied | Applied via additive multi-pass redraws |
| **B/D/S/I** | NOT applied | Applied via blend state effects |
| **Hue Shader** | Vertex diffuse colors (time-varying [0.5, 1.0]) | Vertex diffuse modulated by fShaderAmount |
| **Final output** | Comp pixel shader `ret.xyz` | Texture * vertex color, with gamma + B/D/S/I post |

This is identical to MilkDrop3. **Do not inject gamma_adj or B/D/S/I into the comp shader output.**

---

## Shader Wrapping (plugin.cpp ~line 4585)

MDropDX12 assembles shader text by combining include.fx + defines + the preset shader body + wrapping code.

### Defines (identical to MilkDrop3)

```
Warp: #define uv _uv.xy / #define uv_orig _uv.zw   (float4 _uv : TEXCOORD0)
Comp: #define uv _uv.xy / #define uv_orig _uv.xy   (float2 _uv : TEXCOORD0) [sic]
```

Note: comp uses `float2 _uv` (no zw), so `uv_orig` = `uv` by design.

### First/Last Lines

```hlsl
// First line (both warp and comp):
float3 ret = 0;

// Last line — MDropDX12:
_return_value = float4(shiftHSV(ret.xyz), _vDiffuse.w);

// Last line — MilkDrop3 (for reference):
_return_value = float4(ret.xyz, _vDiffuse.w);
```

- `shiftHSV()` is an MDropDX12 addition for the colshift feature. It early-exits when `colshift_hue`, `colshift_saturation`, and `colshift_brightness` are all 0, so it is harmless at defaults.
- **Do NOT inject `ret *= gamma_adj;`** before the last line. MilkDrop3 does not do this.
- **Do NOT inject `ret = saturate(ret);`** around preset markers like "MilkDrop3 Color Mode" or "MilkDrop3 Burn Mode" — those are just preset author comments, not engine features.

---

## include.fx Differences

MDropDX12's include.fx extends MilkDrop3's with additional constants and features:

| Constant | MilkDrop3 | MDropDX12 | Purpose |
|----------|-----------|-----------|---------|
| `_c0`-`_c13` | Yes | Yes | Standard MilkDrop constants |
| `_c14` | No | `float4(0.5, 0.5, 0, 0)` | mouse_x, mouse_y, mouse_clicked |
| `_c15` | No | `float4(0, 0, 0, 0)` | bass_smooth, mid_smooth, treb_smooth, vol_smooth |
| `_c16` | No | `float4(1, 0, 1, 0)` | vis_intensity, vis_shift, vis_version |
| `_c17` | No | `float4(0, 0, 0, 0)` | colshift_hue, colshift_saturation, colshift_brightness |
| `_c18` | No | `float4(1, 0, 0, 0)` | gamma_adj (default 1.0 = identity) |
| `shiftHSV()` | No | Yes | HSV color shift function |
| `register(sN)` | No | Yes | Explicit sampler register bindings for DX12 |

All defaults are chosen so that the extensions are **no-ops** when not actively used:
- `_c14` mouse defaults to center (0.5, 0.5), not clicked
- `_c15` smooth audio defaults to 0
- `_c16` vis_intensity=1 (identity), vis_version=1 (MilkDrop2)
- `_c17` colshift all 0 (shiftHSV early-exits)
- `_c18` gamma_adj=1.0 (identity multiply if referenced by preset code)

---

## Audio Data Path

### WASAPI Float to Unsigned 8-bit PCM

```
WASAPI float [-1.0, +1.0]
   |
   v
FltToInt() — applies gain (fixed or adaptive), clamps, returns int8_t [-128, +127]
   |
   v
SetAudioBuf() — stores as unsigned char: (int8_t value) + 128
   Result: unsigned [0, 255] centered at 128 (silence = 128)
```

**Key parameters:**
- `mdropdx12_audio_sensitivity` — fixed gain (default 1.0). Set via INI `AudioSensitivity=N`.
- `mdropdx12_audio_adaptive` — when true (INI `AudioSensitivity=-1`), auto-normalizes:
  - Tracks peak level, computes `gain = 0.10 / peak`, clamped to [0.1, 8.0]
  - Instant attack, slow decay (~1.5s at 48kHz)
- `mdropdx12_amp_left/right` — per-channel amplitude (default 1.0)
- Downsampling: supports 96kHz/192kHz by averaging samples

### Unsigned 8-bit PCM to FFT Spectrum

```
AnalyzeNewSound(unsigned char* pWaveL, unsigned char* pWaveR)
   |
   v
fWaveform[ch][i] = (float)((int)pWaveL[i] - 128)    // signed float [-128, +127]
   |
   v
temp_wave[i] = 0.5 * (fWaveform[i] + fWaveform[i-1])  // simple low-pass
   |
   v
m_fftobj.time_to_frequency_domain(temp_wave, fSpectrum)  // FFT with equalization
   |
   v
fSpectrum[ch][0..NUM_FREQUENCIES-1]                     // equalized magnitude spectrum
```

**MilkDrop3 difference:** Uses `(pWaveL[i] ^ 128) - 128` (XOR conversion from Winamp's signed format). This inverts the signal and adds a DC offset of -128, but:
- Signal inversion doesn't affect FFT magnitude (only phase)
- DC offset is zeroed by the FFT equalization table (`equalize[0] = 0`)
- So **spectrum magnitudes are equivalent** between both implementations

### 3-Band Analysis

Logarithmically spaced across the frequency range:
- MDropDX12: 20 Hz - 20,000 Hz (5.78 octaves, ~1.93 per band)
- MilkDrop3: 200 Hz - 11,025 Hz (different range but same math)

Each band = mean of FFT bins in range, normalized by empirical averages (bass/0.327, mid/0.381, treb/0.200).

**Note:** The band range difference only affects `bass`, `mid`, `treb`, `bass_att`, etc. It does NOT affect raw `fSpectrum[]` data used by custom waves in spectrum mode.

### Custom Wave Spectrum Access

Custom waves in spectrum mode (`bSpectrum=1`) read directly from `m_sound.fSpectrum[ch]`:

```cpp
float mult = 0.15f * scaling * m_fWaveScale;  // spectrum base scale
float* pdata1 = m_sound.fSpectrum[0];         // left channel
float* pdata2 = m_sound.fSpectrum[1];         // right channel
// Resampled into tempdata[2][nSamples] with smoothing
// Multiplied by mult
// Fed to per-point code as value1/value2
```

---

## Custom Shape Rendering

### Vertex Position Math (DX12)

```cpp
// Center vertex:
v[0].x = (float)(*var_pf_x * 2 - 1);   // [0,1] -> [-1,1] clip space
v[0].y = (float)(*var_pf_y * 2 - 1);   // DX12: no Y-flip

// Outer vertices (fan):
float angle = (j-1)/(float)sides * 2*PI + pf_ang + PI*0.25;

if (m_bScreenDependentRenderMode)
    v[j].x = v[0].x + pf_rad * cos(angle);            // no aspect correction
else
    v[j].x = v[0].x + pf_rad * cos(angle) * m_fAspectY;  // aspect corrected
v[j].y = v[0].y + pf_rad * sin(angle);
```

**MilkDrop3 differences:**
- Y position: `pf_y * -2 + 1` (compensates for DX9 OrthoLH Y-flip). MDropDX12 uses `pf_y * 2 - 1`.
- Aspect correction: MilkDrop3 ALWAYS applies `* m_fAspectY` to X radius. MDropDX12 skips it when `m_bScreenDependentRenderMode = true`.

### Alpha and Blend

```cpp
float shapeA  = (float)*var_pf_a  * alpha_mult;   // center alpha
float shapeA2 = (float)*var_pf_a2 * alpha_mult;   // outer alpha
if (shapeA <= 0.0f && shapeA2 <= 0.0f) continue;  // skip invisible

// Colors packed as ARGB:
v[0].Diffuse = (A << 24) | (R << 16) | (G << 8) | B;   // center
v[j].Diffuse = (A2 << 24) | (R2 << 16) | (G2 << 8) | B2;  // outer
```

PSO selection:
- Textured + additive: `PSO_ADDITIVE_SPRITEVERTEX`
- Textured + alpha: `PSO_TEXTURED_SPRITEVERTEX`
- Untextured + additive: `PSO_ADDITIVE_WFVERTEX`
- Untextured + alpha: `PSO_ALPHABLEND_WFVERTEX`

Blend states: SrcAlpha/InvSrcAlpha for alpha blend; One/One for additive.

---

## Common Debugging Scenarios

### Colors Wrong (e.g., red/cyan instead of white/grey)

1. **Check for injected code in shader output.** Read `diag_comp_shader.txt` and `diag_warp_shader.txt` (dumped to `m_szBaseDir`). Look for unexpected `saturate()`, `gamma_adj`, or other modifications around the preset's `ret` assignment.

2. **Trace the comp shader math manually.** The comp shader reads from VS[1] via `sampler_main` and blur textures. If the warp shader only sets `ret.r` (common), then VS[1].gb will only have contributions from shapes/waves.

3. **Check shape alpha values.** Low shape alpha means the shape contribution to VS[1] channels is small. Comp shader math that divides by different per-channel values (like `ret /= float3(-0.3, -2.2, -1.4)`) will amplify channel imbalance at low alpha.

4. **Verify gamma_adj is NOT injected into comp shaders.** The `ShowToUser_Shaders` path should NOT apply gamma. Gamma is only for `ShowToUser_NoShaders`.

### Dots/Shapes Too Dim or Too Bright

1. **Check audio sensitivity.** WASAPI loopback levels depend on system volume. If `AudioSensitivity=1.0` (default) and system volume is low, spectrum magnitudes will be small, leading to low shape alphas.

2. **Try adaptive mode.** Set `AudioSensitivity=-1` in settings.ini to enable auto-normalization.

3. **Compare spectrum magnitudes.** Add diagnostic logging to `FltToInt()` or `AnalyzeNewSound()` to see actual peak values. WASAPI typically peaks at 0.01-0.3 for normal listening volumes.

4. **Check `m_fWaveScale`.** This global multiplier affects custom wave spectrum data. Set in per-frame code or defaults.

### Shapes Wrong Size

1. **Check `m_bScreenDependentRenderMode`.** When true, MDropDX12 skips `m_fAspectY` correction on shape X radius (MilkDrop3 always applies it).

2. **Check texture size vs window size.** `m_fAspectY = min(nTexSizeY/nTexSizeX, 1.0)`. If tex size differs from MilkDrop3, aspect correction will differ.

3. **Verify `pf_rad` value.** The preset's per-frame code may override `shapecode_N_rad`. Check both the .milk file and per-frame eval output.

### Preset Works in MilkDrop3 But Not MDropDX12

1. **Dump and compare shaders.** MDropDX12 dumps assembled shader text to `diag_comp_shader.txt` and `diag_warp_shader.txt`. Compare with MilkDrop3's shader assembly (add logging to MilkDrop3's `RecompileShader` if needed).

2. **Check include.fx extensions.** MDropDX12's extra constants (`_c14`-`_c18`) should have safe defaults, but if a preset references `gamma_adj` or `vis_intensity`, the behavior will differ from MilkDrop3 which doesn't define them.

3. **Verify sampler bindings.** MDropDX12 uses explicit `register(sN)` annotations. If a preset declares custom samplers, check for register conflicts with the built-in samplers (s0-s10, s13-s15; s11-s12 reserved).

4. **Check for HLSL variable shadowing.** MDropDX12 has `FixShadowedBuiltins()` which renames variables that shadow HLSL built-in functions. If this changes a variable the preset depends on, the output will differ.

5. **Video echo.** Not implemented in DX12 warp pass. Only matters for non-comp-shader presets that use echo_alpha > 0.

---

## Key Constants Reference

```
_c0     = aspect ratio
_c1     = (unused in defines, but set by ApplyShaderParams)
_c2     = (time, fps, frame, progress)
_c3     = (bass, mid, treb, vol)
_c4     = (bass_att, mid_att, treb_att, vol_att)
_c5     = (blur1 scale+bias in .xy, blur2 scale+bias in .zw)
_c6     = (blur3 scale in .x, blur3 bias in .y, blur1_min in .z, blur1_max in .w)
_c7     = texsize (width, height, 1/width, 1/height)
_c8     = roam_cos (4 values)
_c9     = roam_sin (4 values)
_c10    = slow_roam_cos (4 values)
_c11    = slow_roam_sin (4 values)
_c12    = (mip_x, mip_y, mip_avg, unused)
_c13    = (blur2_min, blur2_max, blur3_min, blur3_max)

MDropDX12 extensions (NOT in MilkDrop3):
_c14    = (mouse_x, mouse_y, mouse_clicked, 0)     default: (0.5, 0.5, 0, 0)
_c15    = (bass_smooth, mid_smooth, treb_smooth, vol_smooth)  default: (0,0,0,0)
_c16    = (vis_intensity, vis_shift, vis_version, 0)  default: (1, 0, 1, 0)
_c17    = (colshift_hue, colshift_saturation, colshift_brightness, 0)  default: (0,0,0,0)
_c18    = (gamma_adj, 0, 0, 0)                     default: (1, 0, 0, 0)
```

## Sampler Register Map

```
s0  = sampler_main         (VS[1] for comp, VS[0] for warp — WRAP filter)
s1  = sampler_fc_main      (VS[1]/VS[0] — CLAMP filter)
s2  = sampler_pc_main      (VS[1]/VS[0] — CLAMP, POINT filter)
s3  = sampler_fw_main      (VS[1]/VS[0] — WRAP filter)
s4  = sampler_pw_main      (VS[1]/VS[0] — WRAP, POINT filter)
s5  = sampler_noise_lq     (256x256 noise, bilinear)
s6  = sampler_noise_lq_lite (32x32 noise, bilinear)
s7  = sampler_noise_mq     (256x256 noise, bilinear)
s8  = sampler_noise_hq     (256x256 noise, bilinear)
s9  = sampler_noisevol_lq  (32x32x32 3D noise)
s10 = sampler_noisevol_hq  (32x32x32 3D noise)
s11 = (reserved)
s12 = (reserved)
s13 = sampler_blur1        (blur level 1)
s14 = sampler_blur2        (blur level 2)
s15 = sampler_blur3        (blur level 3)
```
