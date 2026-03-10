# MDropDX12 v1.7.4

Special thanks to [IkeC](https://github.com/IkeC) and [_Incubo](https://github.com/OfficialIncubo) for testing and reporting issues.

## What's New

- **sampler_rand fix** — Presets using `sampler_rand` random textures no longer render as black screen. The random texture directory is now properly searched when loading textures.
- **Non-shader preset rendering** — Video echo, gamma adjustment, and hue rotation now work correctly for presets without comp shaders. Hue shader corner colors are properly applied to the DX12 comp fullscreen quad.
- **EEL regNN multiply fix** — The ns-eel2 optimizer no longer incorrectly compiles `reg10*reg20` as `sqr(reg10)`. This upstream WDL bug affected any preset using `regNN * regNN` with different register numbers (e.g. rotation matrix math).
- **MinPSVersion raised to ps_3_0** — Prevents `ps_2_a` from silently dropping texture bindings on complex shaders.
- **Async .milk2 loading** — `.milk2` presets now load asynchronously like `.milk` presets, preventing UI freezes.

## Bug Fixes

- Fixed `sampler_rand` black screen when random texture not bound
- Fixed random texture directory not searched for `sampler_rand` textures
- Fixed `tex2D` sampler mismatch when space follows opening parenthesis
- Fixed non-shader preset rendering (video echo, gamma, hue)
- Fixed `hue_shader` corner colors not applied to comp fullscreen quad
- Fixed warp mesh decay and blur texture scan
- Fixed ns-eel2 `regNN * regNN` multiply bug (upstream WDL issue)
- Normalized `tex2D` whitespace handling in shader text processing

## Installation

Download `MDropDX12-v1.7.4-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes.
