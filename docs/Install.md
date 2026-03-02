# MDropDX12 Installation Guide

## System Requirements

- **OS**: Windows 10 64-bit or higher (Windows 11 recommended)
- **GPU**: DirectX 12 compatible graphics card (dedicated GPU recommended; Intel UHD integrated graphics will work but may struggle with complex presets)
- **Runtime**: [Microsoft Visual C++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe) (x64) -- required if not already installed
- **Audio**: Any audio output device (speakers, headphones, virtual audio cable)
- **Disk**: ~2 MB for base install (portable zip); add preset packs as needed

MDropDX12 captures audio from your system's default output device via WASAPI loopback — no special audio setup is needed. If sound is playing on your PC, MDropDX12 can visualize it.

## Download

Download `MDropDX12-1.3-Portable.zip` from the [GitHub Releases](https://github.com/shanevbg/MDropDX12/releases/latest) page.

## Installation

1. Extract the zip to any folder where you have write access, for example:
   - `C:\Tools\MDropDX12`
   - `D:\Apps\MDropDX12`
   - A USB drive

   > **Important**: Do NOT extract into `C:\Program Files` or any other protected location. MDropDX12 needs full write access to its directory for configuration files, screenshots, logs, and preset caching.

2. Run `MDropDX12.exe` from the extracted folder.

No registry entries are created and nothing is installed system-wide. To remove, simply delete the folder.

## First Run

When MDropDX12 starts, it immediately begins visualizing audio from your default output device. Play some music and you should see the visualization respond.

**Essential shortcuts to get started:**

| Key | Action |
|-----|--------|
| **F1** | Show/hide help overlay (2 pages of shortcuts) |
| **F8** or **Ctrl+L** | Open Settings window |
| **SPACE** | Next preset (soft transition) |
| **H** | Hard cut to next preset |
| **ALT+ENTER** | Toggle fullscreen |
| **F2** | Toggle borderless window |
| **ESC** | Exit (or press twice in fullscreen) |

Press **F1** to see the full list of keyboard shortcuts at any time.

## Directory Structure

After extracting, your MDropDX12 folder contains:

```
MDropDX12/
  MDropDX12.exe          — The visualizer (self-bootstrapping, shaders embedded)
  README.txt             — Quick reference guide
  settings.ini           — Main configuration (auto-created on first run)
  messages.ini           — Custom message definitions (slots 00-99)
  sprites.ini            — Sprite overlay definitions
  script-default.txt     — Example BPM-timed preset script
  midi-default.txt       — MIDI automation mappings
  precompile.txt         — Presets to precompile on startup
  resources/
    presets/             — Preset collections (.milk files)
    textures/            — Texture files referenced by presets
```

The exe embeds all required shaders (blur, warp, comp, include.fx). No external `resources/data/` directory is needed. If a `resources/data/` directory exists with .fx files, those serve as user overrides.

## Configuration Files

MDropDX12 stores all configuration in plain text files in its directory:

| File | Purpose |
|------|---------|
| `settings.ini` | All visualizer settings (window size, audio, presets, display, etc.) |
| `messages.ini` | Custom text messages (up to 100 slots) with font, color, and animation |
| `sprites.ini` | Image overlay definitions with blend modes, positioning, and EEL code |
| `script-default.txt` | Example BPM-synchronized preset sequence script |
| `midi-default.txt` | MIDI controller automation mappings |
| `precompile.txt` | List of presets to compile shaders for at startup (reduces stutter) |
| `controller.json` | Game controller button mappings (auto-created when controller connected) |

All files are human-readable and can be edited with any text editor. The Settings window (F8) provides a GUI for most options.

## Adding Presets

MDropDX12 uses `.milk` preset files compatible with MilkDrop2.

To add presets:

1. Create a subfolder in `resources/presets/` (e.g., `resources/presets/MyPresets/`)
2. Copy `.milk` files into that folder
3. The new presets appear immediately — no restart needed

You can also drag and drop `.milk` files or folders directly onto the visualizer window.

Thousands of additional presets are available from the [projectM preset collection](https://github.com/projectM-visualizer/projectm?tab=readme-ov-file#presets).

## Updating

Extract the new zip over the existing folder. Your configuration files will not be overwritten if you skip existing files during extraction.

## Uninstalling

Delete the folder. No registry entries or system files are affected.

## Troubleshooting

### Black screen / no visuals

- Ensure your GPU supports DirectX 12. Check with `dxdiag` (Start → type "dxdiag").
- Update your graphics drivers to the latest version.
- Try pressing **SPACE** to load a different preset — some presets may fail to compile on certain GPUs.

### No audio response

- MDropDX12 captures audio from your default output device. Make sure audio is actually playing.
- Open Settings (F8) → General tab and verify the correct audio device is selected.
- If using a virtual audio cable or routing software, ensure audio is passing through the default device.

### GPU timeout / TDR crash

- MDropDX12 includes TDR recovery and will automatically restart the GPU device.
- If crashes persist, open Settings (F8) → System tab and enable "Skip Heavy Presets" or reduce the shader timeout.
- Some complex presets may exceed your GPU's capabilities — press SPACE to skip them.

### VCRUNTIME140_1.dll not found

- Download and install the [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- Restart MDropDX12 after installing.

### Visualizer won't start

- Make sure you're running Windows 10 64-bit or higher.
- Verify the directory has write permissions (not in Program Files).
- Check `debug.log` in the application directory for error details.
- Try deleting `settings.ini` to reset to defaults.

### Presets not showing up

- Presets must be `.milk` files placed inside `resources/presets/` or a subdirectory.
- Open Settings (F8) → Files tab to verify the preset directory path.
- Check the Settings preset browser to confirm presets are listed.

## Support

- **GitHub Issues**: [github.com/shanevbg/MDropDX12/issues](https://github.com/shanevbg/MDropDX12/issues)
