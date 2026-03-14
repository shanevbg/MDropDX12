# MDropDX12 v2.2.0

Preset rendering accuracy release. Fixes long-standing math and shader issues that caused many presets to render darker, incorrectly filtered, or visually different compared to the reference Milkwave Visualizer.

Special thanks to [IkeC](https://github.com/IkeC) for all his brilliant work on [Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer, testing feedback, and tireless collaboration that continues to drive MDropDX12 forward.

## Preset Rendering Fixes

- **Fix sqrt() emulation to match DX9 hardware behavior**: DX9 compiles `sqrt(x)` as `x * rsq(x)`, returning `sign(x) * sqrt(|x|)` for negative inputs. The previous `_safe_sqrt` used `sqrt(abs(x))`, shifting feedback equilibria and causing darkness/brightness issues across many presets with complex feedback loops. Now matches DX9 exactly.
- **Fix sampler addressing for prefixed noise/random textures**: Presets using `sampler_pw_*`, `sampler_fc_*`, `sampler_pc_*` prefixed samplers were falling through to the default LINEAR+WRAP sampler instead of POINT+WRAP, LINEAR+CLAMP, or POINT+CLAMP. Added 36 `replaceTex2D` entries for all prefixed noise, noisevol, and random texture variants. Fixes excessive glow and incorrect filtering in affected presets.
- **Fix vertex decay color for custom warp shader presets**: Custom warp shader presets now receive white vertices (shader handles decay internally), matching Milkwave's `WarpedBlit_Shaders` behavior. Previously the `cDecay` vertex color was incorrectly applied, causing premature darkening.
- **Add NaN-safe shader intrinsics for DX12 IEEE 754 compliance**: DX12 strict IEEE 754 math produces NaN where DX9 NVIDIA hardware returned finite values. Safe wrappers (`_safe_sqrt`, `_safe_tan`, `_safe_pow`, `_safe_asin`, `_safe_acos`, `_safe_normalize`) prevent NaN propagation through the feedback loop.
- **Fix comp vertex shader uv_orig binding**: Warp and comp vertex shaders now use separate TEXCOORD0/1/2 inputs matching the vertex layout descriptor.

## Other Fixes

- **Fix screenshot red/blue channel swap**: WIC PNG encoder silently reinterprets RGBA as BGRA. Screenshots now correctly swap R↔B before writing.
- **Increase default ToolWindow font size**: Default 16px → 20px, max 24px → 32px for better readability on high-DPI displays.

## Diagnostics

- Added `DIAG_DISPLAY_MODE` IPC command for raw render target inspection
- Shader text dump diagnostics for warp and comp shaders

## Installation

Download the portable zip below, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes across all releases.
