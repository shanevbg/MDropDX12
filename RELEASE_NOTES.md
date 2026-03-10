# MDropDX12 v1.7.5

Special thanks to [IkeC](https://github.com/IkeC) and [_Incubo](https://github.com/OfficialIncubo) for testing and reporting issues.

## What's New

- **Preset annotation system** — Persistent per-preset ratings (0-5), flags (favorite/error/skip/broken), notes, and auto-captured shader error text stored in `presets.json`. Right-click context menu on the Presets window for quick access.
- **Annotations ToolWindow** — Filterable ListView with detail dialogs, import from file, and scan loaded presets for `.milk` ratings. Import dialog shows side-by-side comparison with selective import.
- **Dark-themed popup menus** — Context menus follow the app's dark theme.
- **Cover art sprite system** — Show Now button and IPC signal for cover art display.
- **Two-pass shader blending** — DX12 preset transitions use two-pass shader blending for smoother crossfades.

## Bug Fixes

- Random preset selection now skips presets flagged as skip/broken
- Auto-flag presets with shader compilation errors

## Installation

Download the latest portable zip from the [latest release](https://github.com/shanevbg/MDropDX12/releases/latest), extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes.
