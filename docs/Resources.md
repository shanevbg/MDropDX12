# MDropDX12 Resources

MDropDX12 uses `.milk` preset files and texture images to generate visuals. The exe is self-bootstrapping — it creates the `resources/presets/` and `resources/textures/` directories automatically on first run. No bundled presets are required to get started.

## Getting Presets

### Milkwave Preset Collection (Recommended)

IkeC's [Milkwave](https://github.com/IkeC/Milkwave) project includes an excellent, curated preset collection with thousands of presets organized by author. This is the best starting point for building a preset library.

**Download**: [Milkwave resources](https://github.com/IkeC/Milkwave/tree/main/Visualizer/resources)

To use: download or clone the repository and copy the `Visualizer/resources/presets/` and `Visualizer/resources/textures/` folders into your MDropDX12 directory. MDropDX12 will pick them up immediately — no restart needed.

Milkwave is the project that originally inspired MDropDX12's development. Many thanks to IkeC for maintaining this collection and for the Milkwave Remote IPC protocol that MDropDX12 continues to support.

### projectM Presets

The [projectM](https://github.com/projectM-visualizer/projectm) project maintains a large collection of MilkDrop-compatible presets:

- [projectM preset packs](https://github.com/projectM-visualizer/projectm?tab=readme-ov-file#presets)

These presets are compatible with MDropDX12's `.milk` format.

### MilkDrop2077 / MilkDrop3

The [MilkDrop3](https://github.com/milkdrop2077/MilkDrop3) project by milkdrop2077 includes presets and is another source of MilkDrop-compatible content.

### Community Presets

The MilkDrop preset community has been active for over 20 years. Additional presets can be found at:

- [Butterchurn preset explorer](https://butterchurnviz.com/) — browse presets in the browser (WebGL MilkDrop port)
- Various preset packs shared on forums and social media by preset authors

### Shadertoy Shaders

MDropDX12 can import shaders directly from [Shadertoy](https://www.shadertoy.com/) using the built-in Shader Import window. These are converted from GLSL to HLSL and saved as `.milk3` presets.

1. Copy shader code from Shadertoy
2. Open Shader Import (from Settings or the Hotkeys window)
3. Paste into the editor, click Convert & Apply
4. Save as `.milk3` for permanent use

See [docs/GLSL_importing.md](GLSL_importing.md) for details on the converter.

## Getting Textures

Presets that use texture mixing (filenames containing "texture mix", "random texture", etc.) need texture files in `resources/textures/`. The Milkwave collection above includes the standard texture set.

MDropDX12 supports:
- **JPEG**, **PNG**, **BMP**, **TGA**, **DDS** texture formats
- Fallback texture search paths (configurable in Settings → Files)
- A dedicated **Random Textures Directory** for texture-mixing presets
- Drag-and-drop: drop texture files or folders directly onto the visualizer window

## Adding Presets

1. Create a subfolder in `resources/presets/` (e.g., `resources/presets/MyPresets/`)
2. Copy `.milk` or `.milk3` files into that folder
3. Presets appear immediately — no restart needed

You can also drag and drop `.milk` files or folders directly onto the visualizer window.

## Directory Structure

After setup, your MDropDX12 folder should look like:

```
MDropDX12/
  MDropDX12.exe
  resources/
    presets/           <- .milk and .milk3 preset files (subdirectories OK)
    textures/          <- texture images referenced by presets
```

The exe creates these directories automatically if they don't exist.

## Acknowledgements

Special thanks to:

- **[IkeC](https://github.com/IkeC)** — [Milkwave](https://github.com/IkeC/Milkwave) project, preset collection, and the Remote IPC protocol that MDropDX12 was originally built on. The Milkwave preset library remains the recommended starting point for MDropDX12.
- **[Ryan Geiss](https://www.geisswerks.com/milkdrop/)** — MilkDrop2, the original visualizer that started it all
- **[projectM](https://github.com/projectM-visualizer/projectm)** — cross-platform MilkDrop and preset community
- **[milkdrop2077](https://github.com/milkdrop2077/MilkDrop3)** — MilkDrop3 and continued development
- All the preset authors whose creative work makes the visualizer come alive
