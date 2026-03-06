# Importing Shadertoy Shaders into MDropDX12

So you found a sick shader on Shadertoy and want it running as a music visualizer? MDropDX12 can import Shadertoy GLSL and convert it to HLSL for DirectX 12. This guide covers how it works, what gets translated, and where things can go sideways.

## Where to Find Shaders

These sites have GLSL shaders you can import into MDropDX12:

- **[Shadertoy](https://www.shadertoy.com)** — the big one. Thousands of community shaders. MDropDX12 supports Image only, Image + Buffer A, and Common + Buffer A + Buffer B + Image multi-pass.
- **[Shadertoy sorted by popularity](https://www.shadertoy.com/results?query=&sort=popular&from=0&num=25)** — start here for the greatest hits
- **[GLSL Sandbox](https://glslsandbox.com)** — simpler shaders, mostly single-pass fullscreen effects. Good for getting started.
- **[Interactiveshaderformat.com](https://www.interactiveshaderformat.com)** — ISF-format shaders, some convertible to Shadertoy style
- **[Shader Park](https://shaderpark.com)** — JavaScript-based but the concepts transfer

### What Works Best

Not every Shadertoy shader will import cleanly. Here's what to look for:

**Great candidates:**
- Single-pass ("Image" tab only) shaders — simplest to import
- "Image + Buffer A" shaders — fully supported (terrain, fluid sims, particle systems)
- "Common + Buffer A + Buffer B + Image" shaders — classic multi-pass (fluid dynamics, particle systems from ~2013-2016 era)
- Shaders that use `iTime`, `iResolution`, `iMouse` — all mapped
- Audio-reactive shaders using `iChannel0` as audio — `sampler_audio` is wired up

**Won't work (yet):**
- Shaders with Buffer C or D — only up to Buffer B is supported
- Cubemap inputs — not implemented
- Shaders using `iDate`, `iTimeDelta` — not mapped
- `iChannelResolution` — partially mapped via `_texSize()` helper (returns render target size for 2D, hardcoded 32³ for 3D volumes)
- Keyboard input (`iChannelN` as keyboard) — not supported
- Video/webcam inputs as iChannel sources — not connected to Shadertoy pipeline

### Channel Auto-Detection

MDropDX12 automatically detects the correct iChannel mappings when you import shaders. Detection runs through a priority chain of patterns:

**When you add passes ([+] button):**

- Buffer A gets self-feedback on ch0
- Buffer B gets self-feedback on ch0
- Image gets Buffer A on ch0, Buffer B on ch1
- Buffer passes get cross-references set up automatically

**When you paste GLSL:**

- If the pasted code has no `mainImage` function, it's auto-detected as Common code and a Common pass is created
- Channel types are inferred from the GLSL source (see pattern chain below)
- Only default channels are overwritten — if you manually set a channel, auto-detection won't change it

**When you load a .milk3 JSON:**

- JSON channel values are trusted by default (`channelsFromJSON` flag)
- High-confidence patterns (audio, 3D texture usage) still override wrong JSON values
- Low-confidence guessing (Pattern 3: self-feedback heuristic) is skipped for JSON-loaded passes
- Pattern 2d validates `CHAN_FEEDBACK` entries: on Buffer A, if no `texelFetch(iChannelN)` is found for that channel, it's downgraded to a noise default

**When you click Convert:**

- All passes are re-analyzed for channel patterns before conversion
- Channel combos update automatically

**Pattern priority chain (AnalyzeChannels):**

1. **Pattern 1 — Audio**: `texelFetch(iChannelN, ivec2(...))` with small integer coords → `CHAN_AUDIO`
2. **Pattern 2 — 3D texture**: `iChannelN` used with `vec3`/`float3` coordinates → `CHAN_NOISEVOL_LQ`
3. **Pattern 2b — sampler3D functions**: `iChannelN` used in `tex3D`, `tex3Dlod`, `fbm1` etc. → `CHAN_NOISEVOL_LQ`
4. **Pattern 2c — Buffer B cross-ref**: Buffer A referencing Buffer B or vice versa → `CHAN_BUFFER_B`
5. **Pattern 2d — JSON feedback validation**: `CHAN_FEEDBACK` in Buffer A without matching `texelFetch(iChannelN)` → downgraded to noise default
6. **Pattern 3 — Self-feedback guess** (skipped for JSON): heuristic based on `textureLod`/`texture` usage → `CHAN_FEEDBACK`

For most Shadertoy shaders, you can just paste the GLSL for each tab and the channels will be configured correctly. If the auto-detection gets it wrong, you can always override by selecting a different channel in the combo box.

### Tested Shaders

These are confirmed working and make good test cases:

| Shader | Link | Structure | What It Tests |
|--------|------|-----------|---------------|
| Elevated | [shadertoy.com/view/MdX3Rr](https://www.shadertoy.com/view/MdX3Rr) | Buffer A + Image | texelFetch with noise, terrain raymarching, mat2 rotation |
| Seascape | [shadertoy.com/view/Ms2SD1](https://www.shadertoy.com/view/Ms2SD1) | Image only | Ocean raymarching, matrix transforms |
| Clouds | [shadertoy.com/view/XslGRr](https://www.shadertoy.com/view/XslGRr) | Image only | Noise-based volumetric rendering |
| Protean Clouds | [shadertoy.com/view/3l23Rh](https://www.shadertoy.com/view/3l23Rh) | Image only | Complex noise, lots of math |
| Butterfly Flock | (local butterflyflock.json) | Image only | Array of structs, nested loops, camera rotation |
| Selfie Girl | (local selfie.json) | Common + Buffer A + Buffer B + Image | Full multi-pass, temporal accumulation, 3D noise, camera reprojection |

---

## How the Converter Works

The converter lives in `engine_shader_import_ui.cpp` and runs in multiple phases. Here's the high-level flow:

```
Your Shadertoy GLSL
       ↓
  Phase 1: Global text replacements (types, functions, uniforms)
       ↓
  Phase 1b: Matrix variable detection + mul() insertion
       ↓
  Phase 2: Extract mainImage(), wrap in shader_body
       ↓
  Phase 3: Per-line fixes (matrix ops, float expansion, atan)
       ↓
  Post-processing: backslash joins, vector l-value fix, constructor fix
       ↓
  MDropDX12-compatible HLSL
```

---

## The Tricky Parts

### 1. Matrices — The Hardest Problem

GLSL and HLSL store matrices in opposite order. GLSL is column-major, HLSL is row-major. A naive find-and-replace of `mat3(` → `float3x3(` gives you a **transposed** matrix — every multiplication comes out wrong.

The converter uses two different strategies depending on the matrix shape:

**Square matrices (mat2, mat3, mat4) — swap the mul() arguments:**

```
GLSL:  vec3 rd = ca * normalize(p);
HLSL:  float3 rd = mul(normalize(p), ca);  // arguments swapped!
```

This way `ca[0]` still returns the same vector in both languages. If we used `transpose()` instead, indexing with `M[i]` would break (and a lot of shaders use that for camera basis vectors).

**Non-square matrices (mat3x4, etc.) — wrap with transpose():**

```
GLSL:  mat3x4 M = mat3x4(a, b, c);
HLSL:  float4x3 M = transpose(float3x4(a, b, c));
```

Non-square matrices need this because the element layout gets completely scrambled otherwise — you'd get mixed-up values from different vectors ending up in the wrong rows.

### 2. Type and Function Swaps

Most of these are straightforward find-and-replace:

| GLSL | HLSL | Notes |
|------|------|-------|
| `vec2/3/4` | `float2/3/4` | |
| `ivec2/3/4` | `int2/3/4` | Replaced **before** `vec` → `float` so `ivec2` doesn't become `ifloat2` |
| `mat2/3/4` | `float2x2/3x3/4x4` | See matrix section above |
| `fract()` | `frac()` | |
| `mix()` | `lerp()` | |
| `mod()` | `mod_conv()` | Custom helper — HLSL's `fmod` handles negative numbers differently |
| `atan(y,x)` | `atan2(y,x)` | Only the 2-argument version; single-arg `atan()` stays as-is |
| `texture()` | `tex2D()` | |
| `textureLod()` | `tex2Dlod_conv()` | Custom wrapper that reformats the arguments |
| `texelFetch()` | `texelFetch_conv()` or `texelFetch_noise()` | See texelFetch section below |
| `highp`/`mediump`/`lowp` | *(removed)* | HLSL doesn't have precision qualifiers |

### 3. Shadertoy Uniforms

| Shadertoy | What It Becomes | What It Is |
|-----------|-----------------|------------|
| `iResolution` | `float3(texsize.x, texsize.y, 1.0)` | Screen size (width, height, pixel aspect ratio) |
| `iTime` | `time` | Seconds since start |
| `iFrame` | `frame` | Frame counter |
| `iMouse` | `mouse` | Mouse position (pixel coords, Shadertoy-compatible click behavior) |
| `iChannel0-3` | Depends on channel config | See channel mapping below |

### 4. Variable Name Collisions

MDropDX12 uses `#define` macros for audio data like `bass`, `mid`, `treb`, and `vol`. If your shader has a variable called `mid` (super common — think "midpoint"), it gets macro-expanded into a constant buffer swizzle and everything breaks.

The converter auto-renames these: `mid` → `_st_mid`, `bass` → `_st_bass`, etc. Same deal with `time` → `time_conv` since `time` is already a MDropDX12 uniform.

### 5. Vector Component Writing (X3500 Error)

HLSL can't write to vector components with a dynamic index:

```hlsl
// This works in GLSL but HLSL says no:
for (int ch = 0; ch < 3; ch++) {
    result[ch] = someValue;  // X3500: array reference cannot be used as an l-value
}
```

The converter detects this pattern and rewrites it using helper functions:

```hlsl
// Converted to:
_setComp(result, ch, someValue);

// Where _setComp uses static member access:
void _setComp(inout float3 v, int i, float val) {
    if (i == 0) v.x = val; else if (i == 1) v.y = val; else v.z = val;
}
```

### 6. Over-Specified Constructors

GLSL lets you pass more components than needed — `vec3(vec3_a, vec3_b, vec3_c)` just takes the first 3 values total. HLSL rejects this. The converter detects when all arguments to a constructor are the same expression and simplifies it to a cast: `((float3)(expr))`.

---

## Channel Mapping

Each pass has 4 input channels (`iChannel0` through `iChannel3`). The converter maps these to MDropDX12 samplers based on the channel configuration:

| Channel Source | Index | Sampler | Texture |
|---------------|-------|---------|---------|
| `CHAN_NOISE_LQ` | 0 | `sampler_noise_lq_st` | 256×256 uniform white noise |
| `CHAN_NOISE_MQ` | 1 | `sampler_noise_mq_st` | 256×256 noise (medium quality) |
| `CHAN_NOISE_HQ` | 2 | `sampler_noise_hq_st` | 256×256 noise (high quality) |
| `CHAN_FEEDBACK` | 3 | `sampler_feedback` | Screen-sized Buffer A output |
| `CHAN_NOISEVOL_LQ` | 4 | `sampler_noisevol_lq_st` | 3D 32×32×32 volume noise |
| `CHAN_NOISEVOL_HQ` | 5 | `sampler_noisevol_hq_st` | 3D 32×32×32 volume noise |
| `CHAN_IMAGE_PREV` | 6 | `sampler_image` | Previous Image pass output |
| `CHAN_AUDIO` | 7 | `sampler_audio` | 512×2 FFT + waveform |
| `CHAN_RANDOM_TEX` | 8 | `sampler_rand00` | Random disk texture |
| `CHAN_BUFFER_B` | 9 | `sampler_bufferB` | Screen-sized Buffer B output |

**Shadertoy-compatible noise (`_st` suffix):** Shadertoy noise textures use uniform white noise (full [0,255] range, no cubic interpolation, deterministic seed). MDropDX12's regular noise textures (`sampler_noise_lq` etc.) use cubically interpolated, centered-range noise that MilkDrop presets expect. Shadertoy imports automatically use the `_st` variants to match Shadertoy's noise characteristics.

**Default channels**: `{NOISE_LQ, NOISE_LQ, NOISE_MQ, NOISE_HQ}`

When you add a Buffer A pass, the **Image pass** iChannel0 automatically switches to `CHAN_FEEDBACK` so it reads Buffer A's output. Buffer A itself keeps `CHAN_NOISE_LQ` on ch0 — most Shadertoy Buffer A shaders use noise for terrain generation, fluid simulation, etc.

### texelFetch — Getting the Right UV Scale

`texelFetch` in GLSL reads a specific pixel by integer coordinates. Since HLSL's `tex2Dlod` needs UV coordinates (0.0–1.0), the converter has to divide by the texture size. But which size?

- **Noise textures** (256x256): divide by 256.0
- **Screen-sized textures** (feedback buffer): divide by screen resolution via `texsize.zw`

The converter picks the right one automatically based on which sampler is being used:

```hlsl
// For noise:
texelFetch_noise(sampler_noise_lq, coords, 0)  // divides by 256.0

// For feedback:
texelFetch_conv(sampler_feedback, coords, 0)    // divides by texsize.zw
```

---

## The Shadertoy Render Pipeline

Shadertoy imports run on a completely separate render path from regular MilkDrop presets — no warp mesh, no blur, no custom shapes or waves. Just raw shader math. Controlled by `m_bShadertoyMode` flag, rendering goes through `RenderFrameShadertoy()`.

```
Frame N:
  Buffer A: reads FeedbackA[read] + FeedbackB[read] → writes FeedbackA[write]   (FLOAT32)
  Buffer B: reads FeedbackA[read] + FeedbackB[read] → writes FeedbackB[write]   (FLOAT32)
  Image:    reads FeedbackA[write] + FeedbackB[write] → writes backbuffer        (UNORM)
  Swap feedback indices (m_nFeedbackIdx ^= 1)
```

All buffer passes read from the **previous frame** (`fbRead`). The Image pass reads from the **current frame** (`fbWrite`) to display the just-rendered results.

The feedback buffers are **FLOAT32** (`R32G32B32A32_FLOAT`) at VS resolution (`m_nTexSizeX × m_nTexSizeY`). This matters for shaders that accumulate values over time (motion blur, fluid simulations, temporal anti-aliasing). Regular display is UNORM (8-bit per channel), but the feedback loop preserves all the precision.

**sRGB gamma correction** is applied via `RenderInjectEffect()` post-process (`mode.z=1`) when in Shadertoy mode. Shadertoy.com uses an sRGB framebuffer; MDropDX12 uses `R8G8B8A8_UNORM`, so `pow(x, 1/2.2)` is applied in the inject effect shader to match Shadertoy's brightness.

All passes use Shader Model 5.0 (`ps_5_0`) — the full DirectX 11+ feature set, including native `break` in loops, integer operations, and bitwise math.

### Common Tab

The Common tab contains shared GLSL code (helper functions, constants, struct definitions) that gets prepended to all other passes **before** GLSL→HLSL conversion. It's not a render pass — it just holds shared code. Stored as the `common` key in `.milk3` JSON.

---

## Known Limitations

| Feature | Status |
|---------|--------|
| Buffer C, D | Not implemented — only up to Buffer A + Buffer B + Image |
| Cubemap inputs | Not supported |
| `iDate` | Not mapped |
| `iTimeDelta` | Not mapped |
| `iChannelResolution` | Partial — `textureSize()` → `_texSize()` returns render target size (2D) or hardcoded 32 (3D) |
| Keyboard input | Not supported |
| Video/webcam as iChannel | Not connected to Shadertoy pipeline |
| `dFdx`/`dFdy` | Need `ddx`/`ddy` in HLSL — not currently converted |
| `inversesqrt()` | Not converted (HLSL equivalent is `rsqrt()`) |
| Non-square matrix `M[i]` indexing | Returns wrong vector due to transpose |

---

## Debugging When Things Go Wrong

If a shader doesn't compile or looks wrong, check these diagnostic files in the MDropDX12 working directory (`m_szBaseDir`, same directory as the exe / debug.log):

| File | What's In It |
|------|-------------|
| `diag_converter_image.txt` | The converted HLSL for the Image pass |
| `diag_converter_bufferA.txt` | The converted HLSL for Buffer A |
| `diag_converter_bufferB.txt` | The converted HLSL for Buffer B |
| `diag_converter_common.txt` | The Common tab GLSL (prepended to all passes) |
| `diag_comp_shader.txt` | Full compiled Image shader (with headers) |
| `diag_comp_shader_error.txt` | Compilation result for the Image pass |
| `diag_bufferA_shader.txt` | Full compiled Buffer A shader (with headers) |
| `diag_bufferA_shader_error.txt` | Compilation result for Buffer A |
| `diag_bufferB_shader.txt` | Full compiled Buffer B shader (with headers) |
| `diag_bufferB_shader_error.txt` | Compilation result for Buffer B |

Common issues:
- **"undeclared identifier"** — a variable rename was missed, or a sampler isn't declared
- **"X3500: array reference cannot be used as an l-value"** — dynamic vector indexing that the `_setComp` fix didn't catch
- **"X3020: type mismatch"** — matrix multiplication missing `mul()` wrapper
- **Black screen** — check if the shader needs a specific iChannel input that isn't mapped
- **System stutters / GPU hang** — usually a raymarcher with bad input data (wrong texture, wrong UV scale). Check the channel mapping.

You can also paste GLSL into the Shader Editor window and click Convert to see the HLSL output without applying it. Edit the HLSL directly if the auto-conversion needs tweaking.
