# MDropDX12 v1.4.1

## What's New

- **Scripting Guide** — Comprehensive `docs/Scripts.md` covering all script commands, `ACTION=` dispatch with 86 built-in actions, message parameters, and practical examples.
- **Workspace Layout Hotkey** — New `ApplyWorkspaceLayout` and `OpenWorkspaceLayout` hotkey actions. Bind a single key to instantly tile all your tool windows using your saved layout.
- **Bug Fixes** — Fixed owner-draw checkbox state in ToolWindows, light mode rendering, and hardcoded version on About tab.

## Scripting

Scripts are plain text files with BPM-driven command sequencing. New in v1.4.1:

- `ACTION=ApplyWorkspaceLayout` — apply your saved workspace layout from a script or hotkey
- `ACTION=OpenWorkspaceLayout` — open the Workspace Layout configuration window
- Full documentation: [docs/Scripts.md](https://github.com/shanevbg/MDropDX12/blob/v1.4/docs/Scripts.md)

## Bug Fixes

- Fixed owner-draw checkboxes always reading as unchecked (base class now auto-toggles)
- Fixed light mode: ToolWindow backgrounds and Button Board use system colors correctly
- Fixed About tab showing hardcoded version instead of reading from `version.h`

## Installation

Download `MDropDX12-1.4.1-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.4/docs/Changes.md) for the complete list of changes.
