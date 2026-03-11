# MDropDX12 v1.7.7

## What's New

- **Fixed fullscreen black rendering** — Dot-based presets (e.g., "Eo.S. - multisphere") now render correctly at high resolutions. DX12 custom wave dots were always 1px; now emulates DX9 point size (2-3px) for proper feedback loop accumulation.
- **Fixed dark/incorrect blur presets** — Removed leftover DX9 half-texel UV offsets from blur shaders that shifted blur by 1 texel, compounding through the feedback loop (fixes Flexi and other gradient-dependent presets).
- **Fixed non-shader preset rendering** — Auto-gen comp shader binds the correct post-warp texture, shapes use proper alpha blending, comp shader reads correct input texture.
- **Fixed pre-MilkDrop2 presets** — Clamp sampler, Y-flip, and warp decay now work correctly for legacy presets.
- **Mirror and Stretch hotkeys** — Separate hotkey actions for mirror and stretch display modes (unbound by default, bind via Ctrl+F7).
- **Global hotkey fix** — Fixed global hotkeys not dispatching most actions.

## Installation

Download the latest portable zip from the [latest release](https://github.com/shanevbg/MDropDX12/releases/latest), extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes.
