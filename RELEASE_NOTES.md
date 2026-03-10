# MDropDX12 v1.7.6

## What's New

- **Display mode stability** — Fixed render hangs when switching between mirroring and fullscreen. Present `E_FAIL` now triggers automatic swap chain recovery. Device recovery (TDR) now properly triggers after GPU timeout.
- **Focus and z-order fixes** — Render window maintains focus during display mode transitions. ToolWindows opened via hotkey appear above fullscreen render.
- **Mirror deadlock fix** — Render thread now pumps messages for mirror windows, preventing cross-thread `SendMessage` deadlock.
- **Alt+Enter in mirroring** — Disables mirrors and stays fullscreen instead of exiting to windowed mode.
- **Documentation overhaul** — Settings/ToolWindows restructured, IPC commands reference added, all docs updated to match current UI.

## Bug Fixes

- Fixed persistent `E_FAIL` from `DXGI_PRESENT_ALLOW_TEARING` during window state transitions
- Fixed device recovery never firing after `DXGI_ERROR_DEVICE_REMOVED` (check order bug)
- Fixed render window losing focus on fullscreen/spanning/mirroring transitions
- Fixed mirror window deadlock on z-order changes
- Fixed Alt+Enter while mirroring going to windowed instead of single fullscreen

## Installation

Download the latest portable zip from the [latest release](https://github.com/shanevbg/MDropDX12/releases/latest), extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes.
