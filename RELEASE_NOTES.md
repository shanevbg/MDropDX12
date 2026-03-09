# MDropDX12 v1.7.3

## What's New

- **Comp shader rad/ang fix** — Presets using `rad` (distance from center) in comp shaders now render correctly. Previously the fullscreen quad had `rad=1.0` at all vertices, causing presets with `(1-rad)` to display as black screen. Now computed per-pixel from UV coordinates with aspect correction.
- **Embedded shader priority** — Compiled (embedded) shaders are always the primary source. Disk `.fx` files are merged as overrides: user `#define`s replace compiled defaults, custom functions are appended. Fixes DX12 sampler errors when running from directories with legacy DX9 `.fx` files.
- **SEH crash diagnostics** — Full post-mortem dumps to `diag_seh_crash.txt`: register state, stack trace, exception details, and module resolution for JIT/EEL crash analysis without a debugger.

## Bug Fixes

- Fixed comp shader `rad` always being 1.0 (black screen on presets using `(1-rad)`, e.g. "martin - shader pimped caleidoscope 2077")
- Fixed `_samp_lc` undeclared identifier when running from Milkwave directory with DX9-era `.fx` files
- Added EEL compile context tracking for crash diagnostics (`diag_eel_error.txt`)

## Installation

Download `MDropDX12-v1.7.3-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes.
