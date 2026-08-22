# MDropDX12 v2.7.0

Stability, rendering fixes, and quality-of-life improvements. Adds SEH crash recovery, baked lock icon, normalize whitespace fix, and TCP server default-off for clean first launch.

Special thanks to [IkeC](https://github.com/IkeC) for [Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer and tireless collaboration partner. Thanks to [Incubo_](https://github.com/OfficialIncubo) for [BeatDrop](https://github.com/OfficialIncubo/BeatDrop-Music-Visualizer) testing and comparison reports ([#30](https://github.com/shanevbg/MDropDX12/issues/30)).

## Stability

- **SEH crash recovery**: Render loop no longer hangs on GPU faults. After 3 consecutive exceptions, auto-skips to the next preset and flags the crashing preset in annotations. After 10 skipped presets, enters safe mode (black screen, commands only). Safe mode exits when user manually loads a preset.
- **TCP server off by default**: No Windows Defender firewall prompt on first launch. Users who want [MilkRemote](https://github.com/shanevbg/MilkRemote) Android control enable it in Settings or `settings.ini`.

## Rendering Fixes

- **Normalize whitespace fix**: Presets with `normalize (` (space before parenthesis) now correctly get the `_safe_normalize` replacement, preventing NaN from zero-length vectors on DX12.

## UI Improvements

- **Baked lock icon**: Preset lock indicator is now a pixel-art padlock drawn directly in the font atlas, replacing the barely-visible bullet dot. Appears before the preset name when locked.

## Related Apps

- **[Milkwave](https://github.com/IkeC/Milkwave)** — Windows companion app with Remote control, wave manipulation, messaging, and more
- **[MilkRemote](https://github.com/shanevbg/MilkRemote)** — Android remote control app for MDropDX12 ([download latest APK](https://github.com/shanevbg/MilkRemote/releases/latest))

## Installation

Download the portable zip below, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes across all releases.
