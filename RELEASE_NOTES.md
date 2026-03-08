# MDropDX12 v1.5.0

## What's New

- **Named Pipe IPC** — Replaced WM_COPYDATA / hidden window IPC with Named Pipes (`\\.\pipe\Milkwave_<PID>`). PID-based discovery eliminates fragile window-title matching and removes the hidden 1x1 IPC window entirely.
- **Animated song title rendering** — DX12 warped text animation for song titles with selectable track info sources, Windows color/font picker dialogs, export/import for animation profiles, and custom preview text.
- **Mouse button hotkeys** — Left, Right, Middle, X1, and X2 mouse buttons can now be assigned as hotkey bindings (local scope only). New "Mouse:" dropdown in the hotkey edit dialog.
- **Open Remote action** — New configurable hotkey action to find and activate Milkwave Remote, or launch it if not running. Remembers the last pipe-connected Remote exe path across sessions.
- **.milk3 preset support** — File dialogs and preset browser now support `.milk3` (Shadertoy JSON) presets alongside `.milk` and `.milk2`.
- **Bootstrap fix** — Self-bootstrap now prefers a `resources/` directory next to the exe instead of walking up parent directories.

## Bug Fixes

- Fixed upside-down sprites (DX12 Y-flip re-enabled)
- Fixed resize/fullscreen triggering an unwanted next-preset transition
- Fixed device recovery (TDR) reloading the same preset instead of skipping to next
- Fixed C5208 build warnings (anonymous struct with static member)

## IPC Changes

The IPC system has been completely rewritten. Milkwave Remote now discovers visualizers by enumerating `\\.\pipe\Milkwave_*` pipes and connecting by PID. All existing text message formats (`MSG|...`, `PRESET=...`, `WAVE|...`, `STATE`, etc.) are unchanged — only the transport layer has changed from WM_COPYDATA to Named Pipes.

Key benefits:

- No hidden IPC window (eliminates the 1x1 visible artifact)
- Deterministic PID-based discovery (no window title ambiguity)
- Duplex communication (visualizer can send messages back to Remote)
- Non-blocking writes (no SendMessage blocking the render pump)
- Pipe server tracks connected client exe path for auto-launch

**Requires Milkwave Remote 3.7+ for Named Pipe support.**

## Installation

Download `MDropDX12-1.5.0-Portable.zip`, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/v1.5/docs/Changes.md) for the complete list of changes.
