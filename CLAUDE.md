# MDropDX12 — DirectX 12 Music Visualizer

MDropDX12 is a ground-up DirectX 12 rebuild of the MilkDrop2 music visualizer engine. The rendering backend, text pipeline, settings UI, texture management, and shader compilation have all been rewritten from the original DX9Ex codebase while maintaining preset compatibility.

## Project Structure

- **Standard**: C++17, Windows (Win32 API), x64 only
- **Graphics**: DirectX 12 (migrated from DX9Ex; D3D11on12 for Direct2D text)
- **Audio**: WASAPI loopback capture
- **Spout** integration for texture sharing
- **Expression eval**: ns-eel2 (Cockos WDL, x64 JIT) — see `src/ns-eel2/`
- **Main source**: `src/mDropDX12/` (engine, app, text, menu, audio capture)
- **Build**: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
- **Build outputs**: `src/mDropDX12/Debug_x64/` (Debug), `src/mDropDX12/Release_x64/` (Release)
- Debug uses `Release/` as working dir; Release uses exe directory
- **IDE**: VSCodium with C/C++ extension (pre-configured in `.vscode/`)
- **Docs**: `docs/Install.md` (end-user), `docs/Development.md` (developer setup)

## Critical Warnings

- `HWND_NOTOPMOST` has ONE T — never use `HWND_NOTTOPMOST`
- All file paths use wide strings (`wchar_t`, `std::wstring`)
- The visualizer should never crash — always handle exceptions gracefully
- **New actions may be added to the hotkey system**, but they must be **unbound by default** (modifiers=0, key=0 in `HK_DEF`). Users bind keys themselves via the Hotkeys window (Ctrl+F7). Never hardcode key bindings in `WM_KEYDOWN`/`WM_CHAR` handlers — all key-triggered actions go through `DispatchHotkeyAction()` in `engine_hotkeys.cpp`.

## Naming Conventions

- Classes: `PascalCase` (e.g., `CPlugin`, `MDropDX12`)
- Member variables: `m_camelCase` (e.g., `m_WindowWidth`)
- Functions: `PascalCase` (e.g., `StartRenderThread`)
- Constants: `UPPER_CASE` (e.g., `SAMPLE_SIZE`)

## Threading Model

- **Render thread**: Main window and DirectX rendering
- **Setup thread**: Shader precompilation
- **Audio thread**: WASAPI loopback capture
- **IPC thread**: Hidden window for WM_COPYDATA from Milkwave Remote
- Use `std::atomic` for thread-safe flags

## Error Handling

- C++: `try/catch` for `std::exception`; SEH for low-level exceptions
- Logging via `DebugLogA()`/`DebugLogW()` from utility.h (level-gated: 0=Off, 1=Error, 2=Warn, 3=Info, 4=Verbose)
- All logs and diagnostic files go to `log/` subdirectory (created by `DebugLogInit`)
- Diagnostic file helpers: `DebugLogDiagWrite()`, `DebugLogDiagAppend()`, `DebugLogDiagOpen()`, `DebugLogDiagTruncate()`, `DebugLogDiagPath()`
- `DebugLogClearAll()` deletes all files in `log/` and re-opens debug.log fresh
- `settings.ini`: `LogLevel=2` for verbose logging

## Key Features (current: v2.3.0)

- DirectX 12 rendering backend (migrated from DX9Ex)
- DX12 font atlas HUD text rendering (preset name, FPS, debug info, notifications)
- In-app Settings window (F8 / Ctrl+L) with tri-mode theme (Dark/Light/Follow System), 5-tab UI (General, Tools, System, Files, About), preset browser, resource viewer, path display on About tab
- ToolWindow system: 20+ standalone windows (Visual, Colors, Controller, Displays, Song Info, Hotkeys, MIDI, Presets, Sprites, Messages, Remote, Script, Shader Import, Video Effects, VFX Profiles, Text Animations, Button Board, Workspace Layout, Error Display, Annotations) run on their own threads with independent always-on-top, sticky positions, and tab memory
- Configurable hotkeys (Ctrl+F7) with per-binding local/global scope, mouse button bindings, conflict detection, dynamic Script/Launch entries, and Reset to Defaults
- Native MIDI input (50 mapping slots, Button/Knob actions, learn mode, JSON persistence)
- Standalone Song Info window (Shift+Ctrl+F8) and Displays window (Ctrl+F8)
- Self-bootstrapping exe with embedded shaders (no external .fx files required)
- Fallback texture search paths and dedicated Random Textures Directory
- HLSL variable shadowing fix (auto-renames variables shadowing built-in functions)
- DX12 3D volume texture support (noisevol_lq/noisevol_hq)
- Track info and artwork from Spotify/YouTube/media sources (SMTC, IPC, window title scraping)
- Animated song title rendering (DX12 warped text) with selectable track info sources
- Preset change on track change; preset tagging system
- Window transparency, borderless, clickthrough ("watermark mode")
- Window title regex parsing with named profiles for track info extraction
- Game controller support with JSON config and IPC command binding
- Hue/Saturation/Brightness color shifting
- Shader precompiling and caching
- Native webcam and video file input mixing (background/overlay compositing with luma key)
- Spout video input mixing (background/overlay compositing)
- Idle timer / screensaver mode
- Drag-and-drop presets, folders, and textures
- Command-line preset loading (double-click .milk/.milk2/.milk3 in Explorer; forwards to running instance via Named Pipe)
- Named Pipe IPC (`\\.\pipe\Milkwave_<PID>`) — PID-based discovery, duplex message-mode, no hidden window
- File association registration for .milk/.milk2 (Settings -> About, HKCU, no admin)
- Shadertoy import: GLSL->HLSL converter with .milk3 JSON format (SM5.0 / `ps_5_0`), separate render path (`RenderFrameShadertoy`), FLOAT32 ping-pong feedback buffers (see `docs/GLSL_importing.md`)
- Preset filter by type (All / .milk / .milk2 / .milk3) in preset browser; random/sequential selection respects filter
- Custom preset variables: `bass_smooth`, `mid_smooth`, `treb_smooth`, `vol_smooth`, `vis_intensity`, `vis_shift`, `vis_version`, `colshift_hue`
- FFT EQ smoothing with attack/decay and peak hold — `get_fft()`, `get_fft_hz()`, `get_fft_peak()`, `get_fft_peak_hz()` shader functions; audio texture 512x2 R32_FLOAT (row 0 = smoothed, row 1 = peak)
- Error Display Settings ToolWindow for configuring shader error notification appearance
- Preset annotation system: persistent per-preset ratings, flags (favorite/error/skip/broken), notes, and auto-captured shader errors in `presets.json`
- Annotations ToolWindow with filter, import from file, scan loaded presets for ratings, and detail dialogs
- Dark-themed popup context menus using undocumented uxtheme APIs

## DX12 Rendering Pipeline

The DX12 rendering pipeline uses two render targets (VS[0] and VS[1]) that ping-pong:

1. **Warp pass**: Reads VS[0] -> applies warp mesh distortion -> writes to VS[1]
2. **Shape/wave injection**: Custom shapes and waves drawn directly into VS[1]
3. **Comp pass**: Reads VS[1] -> applies comp mesh + comp shader -> writes to backbuffer

Key architecture differences from DX9:
- **No projection matrix**: DX12 vertex shaders output directly to clip space (`output.pos = float4(input.pos, 1.0)`) — no `D3DXMatrixOrthoLH` projection
- **No DX9 half-texel offset**: DX12 pixel centers are at integer+0.5 (DX9 at integers). All `0.5f / texSize` offsets must be zero in DX12.
- **No Y-flip compensation**: DX9 used `OrthoLH(2,-2)` which negated Y. Original code compensated with explicit Y-flips. DX12 passthrough VS doesn't flip Y, so these compensations must be removed.
- **Post-processing via shader**: `RenderInjectEffect()` in plugin.cpp handles brighten/darken/solarize/invert via pixel shader (not blend states)
- **Texture fallback**: Missing disk textures fall back to a 1x1 white texture (multiplicative identity) to prevent black-screen artifacts

See `docs/dx12-migration-status.md` for detailed migration state.

## Workflow Preferences

- After code changes, always attempt a build: `powershell -ExecutionPolicy Bypass -File build.ps1 Release x64`
- Building closes any running visualizer instance (the linker overwrites the exe)
- INI section is `[Milkwave]` for backward compatibility (not renamed)
- Reference visualizer for comparison: Milkwave Visualizer (window title "Milkwave Visualizer")
- User has custom texture files that presets reference (clipboard textures, etc.)
- Textures (`resources/textures/`) tracked in git; shader cache (`cache/`) is not
- Shader .fx files are embedded in the exe (self-bootstrapping); `resources/data/` no longer shipped
- Distribution is portable zip only (no installer)

## System Requirements

- Windows 10 64-bit or higher (Windows 11 recommended)
- DirectX 12 compatible GPU
- No additional runtime libraries required (VC runtime is statically linked)

## License

- CC-BY-NC 4.0 (Creative Commons Attribution-NonCommercial 4.0 International)
- Third-party components retain their original licenses (see THIRD-PARTY-LICENSES.txt)
