# Textures in MDropDX12

Presets use textures to add images, patterns, and visual detail to their effects. MDropDX12 supports both **built-in textures** (procedurally generated noise and blur) and **user textures** (image files loaded from disk). This guide explains how textures work and how to set up your texture library.

## Supported Formats

| Extension | Format |
|-----------|--------|
| `.jpg` `.jpeg` `.jfif` | JPEG |
| `.png` | PNG (with alpha) |
| `.bmp` `.dib` | Windows Bitmap |
| `.tga` | Targa |
| `.dds` | DirectDraw Surface |

All formats are loaded via Windows Imaging Component (WIC) and converted to 32-bit BGRA internally. If a format fails to decode (some DDS variants), the engine tries the next extension automatically.

## Where to Put Textures

When a preset requests a texture named `clouds`, MDropDX12 searches for a matching file in this order:

| Priority | Location | Example |
|----------|----------|---------|
| 1 | Built-in textures directory | `<MDropDX12>\textures\clouds.png` |
| 2 | Preset's own directory | `<PresetDir>\clouds.png` |
| 3 | `textures\` sibling of preset directory | `<PresetDir>\..\textures\clouds.png` |
| 4 | Content Base Path (Settings) | `<ContentBasePath>\clouds.png` |
| 5 | Fallback Paths (Settings) | Each path in the list, in order |

At each location, all supported extensions are tried (`.jpg`, `.jpeg`, `.jfif`, `.dds`, `.png`, `.tga`, `.bmp`, `.dib`). The first file found is used.

### Recommended Layout

A common way to organize textures alongside presets:

```
MyPresetPack\
  presets\
    cool_effect.milk
    another_preset.milk2
  textures\
    clouds.jpg
    metal.png
    fire.tga
```

The sibling `textures\` folder (priority 3) makes this work automatically — presets in `presets\` will find textures in the adjacent `textures\` folder.

### Configuring Texture Paths

Open **Settings** (F8) → **Files** tab:

- **Content Base Path** — A directory to search for textures (and other content). Useful if you keep all your textures in one central folder.
- **Fallback Paths** — Additional directories to search. Add as many as you need.
- **Random Textures Directory** — A dedicated directory for random texture selection (see below).

## Built-in Textures

These textures are always available to presets. They are generated procedurally at startup and don't need files on disk.

### 2D Noise Textures

| Sampler Name | Size | Description |
|-------------|------|-------------|
| `noise_lq` | 256x256 | Low-quality Perlin-like noise |
| `noise_lq_lite` | 256x256 | Low-quality noise (narrower range) |
| `noise_mq` | 256x256 | Mid-quality noise |
| `noise_hq` | 256x256 | High-quality noise (cubic interpolation) |

### 3D Volume Textures

| Sampler Name | Size | Description |
|-------------|------|-------------|
| `noisevol_lq` | 32x32x32 | Low-quality 3D noise volume |
| `noisevol_hq` | 32x32x32 | High-quality 3D noise volume (cubic interpolation) |

Volume textures are sampled with `tex3D()` in shader code and are useful for smooth, organic 3D noise effects.

### Blur Textures

| Sampler Name | Description |
|-------------|-------------|
| `blur1` | First blur level (lightest blur) |
| `blur2` | Second blur level |
| `blur3` | Third blur level (heaviest blur) |

Up to 6 blur levels may be available (`blur1`–`blur6`) depending on build configuration. These provide Gaussian-blurred versions of the current frame at decreasing resolutions.

### Frame Textures

| Sampler Name | Description |
|-------------|-------------|
| `main` | The previous frame's rendered output |

The `main` sampler gives presets access to the previous frame for feedback effects, trails, and motion blur.

## Random Textures

Presets can request a random texture using the `rand##` naming convention:

```
sampler_rand05              — random texture from slot 5
sampler_rand05_smalltiled   — random texture with "smalltiled" prefix filter
```

The number after `rand` (00–15) identifies a slot. Each slot picks a random file independently. The optional suffix after the underscore filters files by filename prefix — `rand05_smalltiled` only picks from files whose names start with "smalltiled".

### Random Texture Search Order

1. **Random Textures Directory** (if configured in Settings → Files)
2. **Content Base Path** (if configured)
3. **Fallback Paths**
4. **Built-in textures directory** (`<MDropDX12>\textures\`)

Random textures are re-scanned each time a preset loads, so adding files to your random textures directory takes effect on the next preset change.

## Missing Texture Fallback

When a texture file can't be found in any search path, MDropDX12 substitutes a fallback texture instead of crashing or showing a black screen.

Configure the fallback style in **Settings** (F8) → **Files** tab → **Fallback Texture for Missing Textures**:

| Style | Name | Description |
|-------|------|-------------|
| 0 | Hue Gradient | 256x256 horizontal rainbow (default) |
| 1 | White | 1x1 white pixel — multiplicative identity, preserves brightness |
| 2 | Black | 1x1 black pixel |
| 3 | Random (Random Tex Dir) | Random file from your Random Textures Directory |
| 4 | Random (Textures Dir) | Random file from the built-in textures directory |
| 5 | Custom File | A specific image file you choose |

**Recommended**: Style **1 (White)** is a safe default. Since many presets multiply textures with other colors, a white fallback preserves the preset's intended brightness. The Hue Gradient (style 0) is useful for debugging since it makes missing textures visually obvious.

When a texture is missing, a warning is logged to `debug.log` and briefly shown on screen.

## Sampler Options (Preset Authors)

Preset shaders declare texture samplers with an optional two-letter prefix that controls filtering and edge behavior:

| Prefix | Filtering | Wrapping | Use Case |
|--------|-----------|----------|----------|
| `FW_` (or `WF_`) | Bilinear | Wrap (tile) | Tiling textures (default for disk textures) |
| `FC_` (or `CF_`) | Bilinear | Clamp | Non-tiling textures, photo overlays |
| `PW_` (or `WP_`) | Point (nearest) | Wrap | Pixel-art textures that tile |
| `PC_` (or `CP_`) | Point (nearest) | Clamp | Pixel-art, lookup tables |

Examples in HLSL:
```hlsl
sampler2D sampler_fw_clouds;       // bilinear + wrap (tiling clouds)
sampler2D sampler_pc_lut;          // point + clamp (color lookup table)
sampler2D sampler_clouds;          // no prefix = bilinear + wrap (default)
```

Blur textures default to **bilinear + clamp** regardless of prefix, since clamping prevents edge artifacts on blurred frames.

## Texture Cache

MDropDX12 keeps loaded textures in a GPU memory cache to avoid reloading the same file repeatedly. The cache uses LRU (Least Recently Used) eviction when limits are exceeded.

Built-in textures (noise, volume, blur) are never evicted. Only user disk textures participate in cache eviction.

When you load a preset that needs many textures, older textures from previous presets may be evicted to stay within the cache budget. They will be reloaded automatically if a future preset needs them.

## Settings Reference

All texture settings are in `settings.ini`. Configure via **Settings** (F8) → **Files** tab.

| Setting | INI Section | INI Key | Description |
|---------|-------------|---------|-------------|
| Random Textures Dir | `[FallbackPaths]` | `RandomTexDir` | Dedicated directory for random texture selection |
| Content Base Path | `[FallbackPaths]` | `ContentBasePath` | Base search path for textures |
| Fallback Path count | `[FallbackPaths]` | `Count` | Number of additional search paths |
| Fallback Path N | `[FallbackPaths]` | `Path_0`, `Path_1`, ... | Additional search directories |
| Fallback Texture Style | `[Milkwave]` | `FallbackTexStyle` | 0–5 (see Missing Texture Fallback above) |
| Custom Fallback File | `[FallbackPaths]` | `FallbackTexFile` | Image path (used when style = 5) |

## Tips

- **Keep textures small.** 256x256 or 512x512 is ideal for most uses. Large textures consume GPU memory and can drop the frame rate.
- **Use power-of-two dimensions** (64, 128, 256, 512, 1024) to avoid tiling artifacts on some GPUs.
- **PNG for transparency.** If your texture needs an alpha channel (transparency), use PNG format.
- **Check debug.log** if a texture isn't loading. Set `LogLevel=2` in `settings.ini` for verbose texture search logging.
- **Drag-and-drop presets** onto the MDropDX12 window to load them. Textures referenced by the preset will be found automatically if they're in the search paths.
- **One texture, many presets.** If multiple presets use the same texture name, they share the same GPU resource automatically (instancing). No extra memory is used.
