# GitHub Copilot Instructions for MDropDX12

## Critical: Windows API Constants

**IMPORTANT**: The correct constant is `HWND_NOTOPMOST` (one T), NOT `HWND_NOTTOPMOST` (two T's)

## Code Standards

- **Standard**: C++17
- **Platform**: Windows (Win32 API)
- **Graphics**: DirectX 12 (D3D11on12 for Direct2D text)
- **Audio**: WASAPI loopback capture
- **Main source**: `src/mDropDX12/`

## Common Patterns

### Windows Constants
- `HWND_NOTOPMOST` - Window positioning (one T!)
- `HWND_TOPMOST` - Always on top window
- Use Windows SDK constants, never define custom values

### Error Handling
- C++: Use try/catch for std::exception
- SEH (Structured Exception Handling) is used for low-level exceptions
- All logging goes through the MDropDX12 logging system

### Threading
- Render thread: Main window and DirectX rendering
- Setup thread: Shader precompilation
- Audio thread: WASAPI loopback capture
- Overlay thread: GDI layered window for HUD text
- Use `std::atomic` for thread-safe flags

## Naming Conventions

- Classes: `PascalCase` (e.g., `CPlugin`, `MDropDX12`)
- Member variables: `m_camelCase` (e.g., `m_WindowWidth`)
- Functions: `PascalCase` (e.g., `StartRenderThread`)
- Constants: `UPPER_CASE` (e.g., `SAMPLE_SIZE`)

## Important Notes

- The project uses **DirectX 12** with D3D11on12 interop for Direct2D text rendering
- All file paths use wide strings (`wchar_t`, `std::wstring`)
- Logging is done through `mdropdx12.LogInfo()`, `mdropdx12.LogException()`, etc.
- Always handle exceptions gracefully - the visualizer should never crash

## Build Configuration

- Build: `powershell -ExecutionPolicy Bypass -File build.ps1 Release`
- Debug: Uses `../../Release` as working directory
- Release: Uses executable directory as base path
- Always append backslash to base directory paths
