# MDropDX12 v2.3.0

Shader compatibility and rendering completeness release. Fixes preset rendering failures, adds missing DX12 motion vector rendering, and improves shader compilation compatibility for complex raymarching presets.

Special thanks to [IkeC](https://github.com/IkeC) for all his brilliant work on [Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer, testing feedback, and tireless collaboration that continues to drive MDropDX12 forward.

## Preset Rendering Fixes

- **Fix `_safe_sqrt` to return always-positive values**: Changed from `sign(x)*sqrt(abs(x))` to `sqrt(abs(x))`, matching DX9 SM3.0 native sqrt behavior. The sign-preserving form created singularities in presets like "martin - axon3" where `sqrt(negative_uv)+offset` crossed zero, producing infinity that blew out to white through the feedback loop.
- **Implement DX12 motion vector rendering**: Ported `DrawMotionVectors()` to DX12 using `PSO_LINE_ALPHABLEND_WFVERTEX`. Motion vectors are now drawn into VS[0] before the warp pass, entering the feedback loop as persistent colored traces. Fixes presets like "Illusion & Rovastar - Clouded Bottle" which appeared too dark because their motion vector lines (the primary light source) were missing.
- **Fix `[loop]` attribute injection for shader compatibility**: Inject `[loop]` only on `while` loops (raymarching constructs), not `for` loops. Small fixed-count `for` loops caused `error X3531` when marked `[loop]` because the compiler insists on unrolling them. Fixes "LamersAss - The Vortex 2077rmx" and "lara - Flexi ate a magical broccoli" presets which had black screens.

## Shader Compilation Improvements

- **Add `D3DCOMPILE_PARTIAL_PRECISION` flag**: Hints to the SM5.0 compiler that lower precision is acceptable, nudging instruction selection closer to SM3.0 hardware behavior for complex raymarching presets.
- **Add `D3DCOMPILE_PREFER_FLOW_CONTROL` flag**: Hints compiler to prefer dynamic branching over predication, reinforcing `[loop]` injection for SM3.0-like codegen in branching shaders.
- **Fix D3DXCompileShader flag passthrough**: All caller flags now pass through to D3DCompile instead of only mapping DEBUG and SKIPVALIDATION.
- **Replace `i = I_MAX` break hack with native `break`**: SM3.0+ supports break natively; the transpiler hack caused incorrect loop behavior with some D3DCompile optimization paths.

## Diagnostics

- **Bytecode disassembly dump**: At verbose log level, writes SM5.0 instruction listing to `log/diag_asm_warp.txt` / `log/diag_asm_comp.txt` via `D3DDisassemble()` for diagnosing codegen differences.

## Installation

Download the portable zip below, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes across all releases.
