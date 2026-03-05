# Importing Shadertoy Shaders into MDropDX12

So you found a sick shader on Shadertoy and want it running as a music visualizer? MDropDX12 can import Shadertoy GLSL and convert it to HLSL for DirectX 12. This guide covers how it works, what gets translated, and where things can go sideways.

## Where to Find Shaders

These sites have GLSL shaders you can import into MDropDX12:

- **[Shadertoy](https://www.shadertoy.com)** — the big one. Thousands of community shaders. Look for shaders with "Image" only or "Image + Buffer A" — those are the ones MDropDX12 supports right now.
- **[Shadertoy sorted by popularity](https://www.shadertoy.com/results?query=&sort=popular&from=0&num=25)** — start here for the greatest hits
- **[GLSL Sandbox](https://glslsandbox.com)** — simpler shaders, mostly single-pass fullscreen effects. Good for getting started.
- **[Interactiveshaderformat.com](https://www.interactiveshaderformat.com)** — ISF-format shaders, some convertible to Shadertoy style
- **[Shader Park](https://shaderpark.com)** — JavaScript-based but the concepts transfer

### What Works Best

Not every Shadertoy shader will import cleanly. Here's what to look for:

**Great candidates:**
- Single-pass ("Image" tab only) shaders — simplest to import
- "Image + Buffer A" shaders — fully supported (terrain, fluid sims, particle systems)
- Shaders that use `iTime`, `iResolution`, `iMouse` — all mapped
- Audio-reactive shaders using `iChannel0` as audio — `sampler_audio` is wired up

**Won't work (yet):**
- Shaders with Buffer B, C, or D — only Buffer A is supported
- Cubemap inputs — not implemented
- Shaders using `iDate`, `iTimeDelta`, or `iChannelResolution` — not mapped
- Keyboard input (`iChannelN` as keyboard) — not supported
- Video/webcam inputs as iChannel sources — not connected to Shadertoy pipeline

### Tested Shaders

These are confirmed working and make good test cases:

| Shader | Link | Structure | What It Tests |
|--------|------|-----------|---------------|
| Elevated | [shadertoy.com/view/MdX3Rr](https://www.shadertoy.com/view/MdX3Rr) | Buffer A + Image | texelFetch with noise, terrain raymarching, mat2 rotation |
| Seascape | [shadertoy.com/view/Ms2SD1](https://www.shadertoy.com/view/Ms2SD1) | Image only | Ocean raymarching, matrix transforms |
| Clouds | [shadertoy.com/view/XslGRr](https://www.shadertoy.com/view/XslGRr) | Image only | Noise-based volumetric rendering |
| Protean Clouds | [shadertoy.com/view/3l23Rh](https://www.shadertoy.com/view/3l23Rh) | Image only | Complex noise, lots of math |

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

| Channel Source | Sampler | Texture |
|---------------|---------|---------|
| `CHAN_NOISE_LQ` | `sampler_noise_lq` | 256x256 grayscale noise |
| `CHAN_NOISE_MQ` | `sampler_noise_mq` | 256x256 noise (medium quality) |
| `CHAN_NOISE_HQ` | `sampler_noise_hq` | 256x256 noise (high quality) |
| `CHAN_FEEDBACK` | `sampler_feedback` | Screen-sized Buffer A output |
| `CHAN_NOISEVOL_LQ` | `sampler_noisevol_lq` | 3D 32x32x32 noise |
| `CHAN_NOISEVOL_HQ` | `sampler_noisevol_hq` | 3D 32x32x32 noise |
| `CHAN_IMAGE_PREV` | `sampler_image` | Previous frame |
| `CHAN_AUDIO` | `sampler_audio` | 512x2 FFT + waveform |
| `CHAN_RANDOM_TEX` | `sampler_rand00` | Random disk texture |

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

Shadertoy imports run on a completely separate render path from regular MilkDrop presets — no warp mesh, no blur, no custom shapes or waves. Just raw shader math.

```
Frame N:
  Buffer A: reads feedback[read] → writes feedback[write]    (FLOAT32)
  Image:    reads feedback[write] → writes backbuffer         (UNORM)
  Swap feedback indices
```

The feedback buffers are **FLOAT32** — full 32-bit floating point per channel. This matters for shaders that accumulate values over time (motion blur, fluid simulations, particle systems). Regular display is UNORM (8-bit per channel), but the feedback loop preserves all the precision.

Both passes use Shader Model 5.0 (`ps_5_0`) — that's the full DirectX 11+ feature set, including native `break` in loops, integer operations, and bitwise math.

---

## Known Limitations

| Feature | Status |
|---------|--------|
| Buffer B, C, D | Not implemented — only Buffer A + Image |
| Cubemap inputs | Not supported |
| `iDate` | Not mapped |
| `iTimeDelta` | Not mapped |
| `iChannelResolution` | Not mapped |
| Keyboard input | Not supported |
| Video/webcam as iChannel | Not connected to Shadertoy pipeline |
| `dFdx`/`dFdy` | Need `ddx`/`ddy` in HLSL — not currently converted |
| `inversesqrt()` | Not converted (HLSL equivalent is `rsqrt()`) |
| Non-square matrix `M[i]` indexing | Returns wrong vector due to transpose |

---

## Debugging When Things Go Wrong

If a shader doesn't compile or looks wrong, check these diagnostic files in the MDropDX12 working directory:

| File | What's In It |
|------|-------------|
| `diag_converter_image.txt` | The converted HLSL for the Image pass |
| `diag_converter_bufferA.txt` | The converted HLSL for Buffer A |
| `diag_comp_shader_error.txt` | Compilation result for the Image pass |
| `diag_bufferA_shader_error.txt` | Compilation result for Buffer A |

Common issues:
- **"undeclared identifier"** — a variable rename was missed, or a sampler isn't declared
- **"X3500: array reference cannot be used as an l-value"** — dynamic vector indexing that the `_setComp` fix didn't catch
- **"X3020: type mismatch"** — matrix multiplication missing `mul()` wrapper
- **Black screen** — check if the shader needs a specific iChannel input that isn't mapped
- **System stutters / GPU hang** — usually a raymarcher with bad input data (wrong texture, wrong UV scale). Check the channel mapping.

You can also paste GLSL into the Shader Editor window and click Convert to see the HLSL output without applying it. Edit the HLSL directly if the auto-conversion needs tweaking.
