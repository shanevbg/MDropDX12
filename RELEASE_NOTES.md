# MDropDX12 v1.4

## Highlights

- **Shadertoy Import** — Import Shadertoy shaders directly with the new Shader Import window. The GLSL-to-HLSL converter handles multi-pass rendering (Buffer A/B, Image, Common), SM5.0 compilation, and saves as portable `.milk3` JSON presets.
- **Video Effects** — New Video Effects window with transform (scale, rotation, offset), color (brightness, contrast, saturation, hue), and audio-reactive controls. Save and load VFX profiles as JSON.
- **Workspace Layout** — Tile all your tool windows across the screen with one click. Choose corner or fullscreen render placement, pick which windows to open, and Apply.
- **15 Tool Windows** — Settings, Displays, Song Info, Hotkeys, MIDI, Button Board, Presets, Sprites, Messages, Shader Import, Shader Editor, Video Effects, VFX Profiles, Workspace Layout, and Welcome windows all run on their own threads.
- **Light Mode Fix** — Light theme now renders consistently across all windows.
- **No Runtime Dependencies** — VC++ runtime is statically linked. Just extract and run.

## New Features

### Shadertoy / .milk3

- Shadertoy-compatible rendering pipeline with GLSL→HLSL converter
- Multi-pass support: Buffer A, Buffer B, Image, Common tab
- SM5.0 (`ps_5_0`) shaders with FLOAT32 ping-pong feedback buffers
- sRGB gamma correction, Shadertoy-compatible `iMouse`, audio texture
- Channel auto-detection and JSON channel import with named strings
- Two-panel Shader Import UI with Convert & Apply and Save .milk3

### Video Effects & Profiles

- Standalone Video Effects window with transform, color, and audio-reactive controls
- VFX JSON profiles: save/load named effect presets
- Startup preset mode selector

### Workspace Layout

- New window for arranging tool windows across the screen
- Corner mode (TL/TR/BL/BR) with render size slider, or fullscreen on separate display
- Checkbox grid for selecting which windows to open and tile

### ToolWindow Improvements

- `IsChecked()` / `SetChecked()` helpers for owner-draw controls
- Base class auto-toggles checkboxes (subclasses no longer need boilerplate)
- Welcome window for first-run with quick-start buttons
- Version number from single source of truth (`version.h`)

### Theme

- Light mode now works correctly across all ToolWindows and Button Board
- Fixed stale dark class brush showing through after theme switch

## Bug Fixes

- Fixed owner-draw checkboxes always reading as unchecked
- Fixed numerous GLSL converter issues (matrices, structs, vectors, arrays, self-feedback)
- Fixed Shadertoy alpha output, UV calculation, time variable shadowing
- Fixed ResizeBuffers E_INVALIDARG with tearing flag
- Fixed invisible controls in Messages/Sprites windows
- Fixed Ctrl+A/C/V/X/Z in tool window edit boxes

## Installation

Download `MDropDX12-1.4-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.4/docs/Changes.md) for the complete list of changes.
