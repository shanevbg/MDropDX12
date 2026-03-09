# MDropDX12 v1.7.2

## What's New

- **GLSL→HLSL matrix fixes** — Non-square matrices (`mat3x4`, etc.) now use unified mul-swap strategy, fixing `M[i]` column indexing. Matrix-returning function chains across newlines are correctly wrapped with nested `mul()`. Parenthesized expressions after matrix variables (`m * (expr)`) are now handled.
- **Channel auto-detection fix** — JSON `CHAN_FEEDBACK` assignments are no longer overridden by audio heuristic false positives (e.g. `texelFetch(iChannel, ivec2(0,0))`).
- **HLSL reserved keyword** — `point` is now auto-renamed when used as a variable name (geometry shader primitive type keyword).

## Bug Fixes

- Fixed non-square matrix `M[i]` indexing returning wrong vector type due to dimension swapping
- Fixed audio channel auto-detection overriding explicit JSON CHAN_FEEDBACK channel assignment
- Fixed matrix-returning function multiply chains not converting to `mul()` when `*` is on the next line
- Fixed `matVar * (parenthesized_expr)` not being converted to `mul()`
- Fixed `point` as variable name causing HLSL syntax error (reserved keyword)

## Installation

Download `MDropDX12-v1.7.2-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.8/docs/Changes.md) for the complete list of changes.
