# MDropDX12 Project

MDropDX12 is a ground-up DirectX 12 rebuild of the MilkDrop2 music visualizer engine, with a companion remote control app. The rendering backend, text pipeline, settings UI, texture management, and shader compilation have all been rewritten from the original DX9Ex codebase while maintaining preset compatibility.

## Project Structure

### Visualizer (C++)
- Main visualizer engine based on MilkDrop2/BeatDrop
- **Standard**: C++17, Windows (Win32 API)
- **Graphics**: DirectX 12 (migrated from DX9Ex; D3D11on12 for Direct2D text)
- **Audio**: WASAPI loopback capture
- **Spout** integration for texture sharing
- **Expression eval**: projectM-eval via ns-eel2 shim (see `Visualizer/ns-eel2-shim/`)
- Build outputs: Debug uses `../../Release` as working dir; Release uses exe directory

### Remote (C#/.NET 8)
- WPF-based remote control application
- Communicates with Visualizer via named pipes and window messages

## Critical Warnings

- `HWND_NOTOPMOST` has ONE T — never use `HWND_NOTTOPMOST`
- All file paths use wide strings (`wchar_t`, `std::wstring`)
- The visualizer should never crash — always handle exceptions gracefully

## Naming Conventions

### C++
- Classes: `PascalCase` (e.g., `CPlugin`, `MDropDX12`)
- Member variables: `m_camelCase` (e.g., `m_WindowWidth`)
- Functions: `PascalCase` (e.g., `StartRenderThread`)
- Constants: `UPPER_CASE` (e.g., `SAMPLE_SIZE`)

### C#
- Standard C# conventions (PascalCase for public members)

## Threading Model

- **Render thread**: Main window and DirectX rendering
- **Setup thread**: Shader precompilation
- **Audio thread**: WASAPI loopback capture
- Use `std::atomic` for thread-safe flags

## Error Handling

- C++: `try/catch` for `std::exception`; SEH for low-level exceptions
- Logging via `mdropdx12.LogInfo()`, `mdropdx12.LogException()`, etc.
- `settings.ini`: `LogLevel=2` for verbose logging

## Key Features (current: v3.5-dev)

- DirectX 12 rendering backend (migrated from DX9Ex)
- GPU-accelerated text overlay via D3D11on12 + Direct2D + DirectWrite
- In-app Settings window (F2) with dark theme, 5-tab UI, preset browser, resource viewer
- Fallback texture search paths and dedicated Random Textures Directory
- HLSL variable shadowing fix (auto-renames variables shadowing built-in functions)
- DX12 3D volume texture support (noisevol_lq/noisevol_hq)
- x64 build support
- Track info and artwork from Spotify/YouTube/media sources
- Preset change on track change; preset tagging system
- Window transparency, borderless, clickthrough ("watermark mode")
- GLSL-to-HLSL shader conversion with live preview
- MIDI automation (up to 50 controls)
- Hue/Saturation/Brightness color shifting
- Shader precompiling and caching
- Custom preset variables: `bass_smooth`, `mid_smooth`, `treb_smooth`, `vol_smooth`, `vis_intensity`, `vis_shift`, `vis_version`, `colshift_hue`

## System Requirements

- Windows 11 64-bit or higher
- DirectX 12 compatible GPU
- .NET Desktop Runtime 8 (for Remote)

## Licenses

- **Visualizer**: 3-Clause BSD License (as BeatDrop fork)
- **Remote**: CC BY-NC 4.0 (non-commercial)
