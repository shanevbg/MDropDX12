# MDropDX12 v1.4.3

## What's New

- **Messages & MIDI Fix** — Fixed checkboxes in the Messages and MIDI windows silently disabling features on click.
- **Radio Group Auto-Toggle** — ToolWindow base class now auto-toggles radio button groups, eliminating subclass boilerplate.

## Bug Fixes

- Fixed Messages window checkboxes (Show Messages, Autoplay, Sequential, Autosize) always reading as unchecked — clicking "Show Messages" would silently disable messages
- Fixed MIDI window Enable checkbox always reading as unchecked
- Root cause: `IsDlgButtonChecked()` returns 0 for BS_OWNERDRAW controls; replaced with `IsChecked()`

## Improvements

- Radio button groups auto-toggled by base class via `radioGroup` parameter on `CreateRadio()`
- Removed ~50 lines of duplicated radio toggle boilerplate from DisplaysWindow, SettingsWindow, and WorkspaceLayoutWindow

## Installation

Download `MDropDX12-1.4.3-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.4.3/docs/Changes.md) for the complete list of changes.
