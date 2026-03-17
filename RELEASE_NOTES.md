# MDropDX12 v2.5.0

Rendering accuracy, quality controls, and stability release. Adds NaN-safe atan2, UI-accessible mesh size and texture precision controls, media key support, Windows volume control via IPC, and resize crash fixes from community PRs.

Special thanks to [IkeC](https://github.com/IkeC) for all his brilliant work on [Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer, testing feedback, and tireless collaboration that continues to drive MDropDX12 forward.

## Preset Rendering Fixes

- **NaN-safe atan2**: `atan2(0, 0)` returns NaN on DX12 (DX9 NVIDIA returns 0). Added `_safe_atan2(y, x)` wrapper that prevents NaN at the origin — fixes persistent black holes in tunnel/radial presets.
- **Safe denominator intrinsics**: Added `_safe_normalize()` overloads that guard against zero-length vectors, preventing NaN propagation in raymarching and particle presets.
- **Fix message SIZE parameter**: Messages with explicit `SIZE=N` parameter now respect the specified size instead of always autosizing.

## New Features

- **Mesh Size control** (Visual window): Slider to adjust warp/shape mesh vertex density (8–192, step 8). Higher values produce smoother curves and distortion effects. Previously only changeable via `settings.ini`.
- **Texture Precision control** (Visual window): Combo box to select internal render target bit depth — 8-bit (default), 16-bit float, or 32-bit float. Higher precision reduces color banding and improves feedback loop accuracy.
- **Media key routing**: Media keys (play/pause, next, previous, stop) now route through `keybd_event` for system-level handling, fixing media control when the visualizer has focus.
- **Windows volume control via IPC**: New `SET_VOLUME` / `GET_VOLUME` / `SET_MUTE` / `GET_MUTE` IPC commands for controlling system audio device volume and mute state.
- **Audio gain attenuation**: Audio sensitivity can now be set below 1.0 for attenuation (previously clamped to 1.0 minimum).

## Stability Fixes

- **Fix resize crash** (PR #32): Restore DX12 command infrastructure after `ResizeBuffers` failure so the render loop doesn't crash accessing null objects during recovery.
- **Fix display output cleanup**: Release display mirror Spout wrapped backbuffers during device cleanup to prevent crashes on resize.
- **Fix preset startup default** (PR #31): `m_bEnablePresetStartup` now defaults to `true` for expected out-of-box behavior with fresh `settings.ini`.

## MCP Server

- BeatDrop pipe discovery support for comparison tooling
- Capture path included in response for automation workflows
- Null-terminator framing fix for reliable message parsing

## Installation

Download the portable zip below, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes across all releases.
