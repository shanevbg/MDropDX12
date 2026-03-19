# MDropDX12 v2.6.0

Shader compatibility, rendering quality, and .milk2 blend improvements. Adds anisotropic filtering toggle, safe division for DX9-to-DX12 NaN prevention, and improved .milk2 double-preset blending patterns.

Special thanks to [IkeC](https://github.com/IkeC) for [Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer and tireless collaboration partner. Thanks to [Incubo_](https://github.com/OfficialIncubo) for [BeatDrop](https://github.com/OfficialIncubo/BeatDrop-Music-Visualizer) testing and comparison reports ([#30](https://github.com/shanevbg/MDropDX12/issues/30)).

## New Features

- **Anisotropic Filtering** (Visual window): Checkbox to enable 16x anisotropic filtering on the two linear DX12 samplers. Improves texture sharpness on warped/stretched surfaces. Off by default.

## Preset Rendering Fixes

- **Safe division for DX9 compatibility**: DX9 returns 0 for `0/0`; DX12 returns NaN which propagates through the feedback loop. Added `_safe_denom()` wrapper applied to divisions inside shader bodies — fixes presets that produce NaN from zero-denominator expressions (e.g. flash effects with unused q-variables).
- **Initialize comp quad Diffuse**: Fixed uninitialized vertex diffuse color on the fullscreen comp quad, ensuring consistent alpha and color values for comp shader presets.

## .milk2 Blend Improvements

- **Fixed wipe patterns for horizontal/vertical**: `.milk2` double-preset files with `blending_pattern=horizontal` or `blending_pattern=vertical` now use dedicated fixed-position wipe shaders instead of recycling animated wipe types.

## Related Apps

- **[Milkwave](https://github.com/IkeC/Milkwave)** — Windows companion app with Remote control, wave manipulation, messaging, and more
- **[MilkRemote](https://github.com/shanevbg/MilkRemote)** — Android remote control app for MDropDX12 ([download latest APK](https://github.com/shanevbg/MilkRemote/releases/latest))

## Installation

Download the portable zip below, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes across all releases.
