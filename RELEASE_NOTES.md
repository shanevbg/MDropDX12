# MDropDX12 v1.5.1

## What's New

- **Sampler register rearchitecture** — Switched .milk/.milk2 pipeline from `sampler2D` (1 s-register per texture, 16 limit) to separated `Texture2D` + 4 shared `SamplerState` objects. Textures now use t-registers (~128 limit), eliminating "maximum sampler register exceeded" errors on texture-heavy presets. Old preset code preserved via compatibility macros (`sampler2D`, `tex2D`, `tex2Dlod`).
- **tex2Dbias support** — Added `tex2Dbias` compatibility macro for presets using the DX9 `tex2Dbias` intrinsic.
- **Expanded texture binding capacity** — Increased texture binding arrays from 16 to 32 slots, descriptor table from 16 to 32 SRVs, with SRV heap overflow protection.
- **ToolWindow improvements** — Added dialog base class for modal popups, text shrink-to-fit for messages/titles/preset names, and Animations tab to Message Overrides.

## Bug Fixes

- Fixed all owner-draw checkbox/radio controls across ToolWindow tabs (Sequential Preset Order, Hard Cuts, Script Loop, Sprite flip/burn, Video Effects checkboxes, VFX Profiles, MIDI Enable, Messages Autoplay) — `BM_GETCHECK`/`IsDlgButtonChecked`/`CheckDlgButton` do not work with BS_OWNERDRAW controls
- Fixed `sampler_rand` redefinition errors (~20 presets) by stripping include declarations when preset declares its own
- Fixed `line` keyword conflict by adding it to the shadowed builtins list
- Stripped Shadertoy-specific texture declarations for non-Shadertoy presets to avoid register pressure
- Fixed ToolWindow resize bugs
- Improved shader error logging: errors now logged at LOG_ERROR with preset filename

## Installation

Download `MDropDX12-1.5.1-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.6/docs/Changes.md) for the complete list of changes.
