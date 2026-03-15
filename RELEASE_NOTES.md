# MDropDX12 v2.4.0

Preset compatibility and visual accuracy release. Fixes two rendering bugs that caused presets to render differently from the reference Milkwave Visualizer, and adds a visual comparison document with side-by-side screenshots.

Special thanks to [IkeC](https://github.com/IkeC) for all his brilliant work on [Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer, testing feedback, and tireless collaboration that continues to drive MDropDX12 forward.

## Preset Rendering Fixes

- **Fix alpha blend feedback amplification in textured shapes**: SPRITEVERTEX PSOs used `SrcBlendAlpha=ONE` instead of `SRC_ALPHA`, causing textured shapes to write excess alpha that compounded through the feedback loop. DX9 has no separate alpha blend, so alpha uses the same factors as color. Fixes "BrainStain - re entry" and other presets with textured shapes appearing much brighter than reference.
- **Fix HLSL variable shadowing user-defined functions**: Added `FixShadowedUserFunctions()` to rename local variables that reuse user-defined function names (valid in GLSL, rejected by HLSL with error X3005). The existing `FixShadowedBuiltins` only handled intrinsic functions. Fixes "Marex + IkeC - Shadow Party Shader Jam 2025" rendering as black screen.

## Documentation

- **Visual comparison document**: New [docs/comparison.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/comparison.md) with side-by-side screenshots of 11 presets rendered on both MDropDX12 and Milkwave Visualizer. All 11 presets now render equivalently.

## Installation

Download the portable zip below, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes across all releases.
