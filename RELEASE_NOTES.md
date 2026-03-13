# MDropDX12 v2.1.0

Major milestone release. MDropDX12 2.1 marks the point where the engine has been fully rebuilt on DirectX 12 with a comprehensive feature set that goes well beyond the original MilkDrop2 visualizer.

Special thanks to [IkeC](https://github.com/IkeC) for all his hard work on [Milkwave](https://github.com/IkeC/Milkwave) — the inspiration, testing, feedback, and collaboration that made this project possible.

## Highlights

### Shadertoy Import & GLSL-to-HLSL Converter
- Full Shadertoy rendering pipeline with `.milk3` JSON preset format
- Multi-pass rendering (Buffer A/B/C/D + Image) with Common shared code and SM5.0 shaders
- Comprehensive GLSL-to-HLSL converter handling matrices, structs, vector comparisons, over-specified constructors, and more
- Shader Import window with two-panel editor, per-pass channel combos, Convert & Apply, and Save .milk3 export
- Shadertoy-compatible uniforms: `iMouse`, `iDate`, `iResolution`, `iTime`, `iChannel0-3`
- 17+ converter fixes since initial release

### Named Pipe IPC
- Replaced WM_COPYDATA with Named Pipes (`\\.\pipe\Milkwave_<PID>`) — PID-based discovery, multi-instance, duplex messaging
- Concurrent client connections (Milkwave Remote + MCP server simultaneously)
- MCP server (`tools/mdrop-mcp/`) for AI-assisted visualizer control

### 20+ ToolWindows
- Visual, Colors, Controller, Displays, Song Info, Hotkeys, MIDI, Presets, Sprites, Messages, Remote, Script, Shader Import, Video Effects, VFX Profiles, Text Animations, Button Board, Workspace Layout, Error Display, Annotations
- Each runs on its own thread with independent always-on-top, sticky positions, and tab memory

### Configurable Hotkeys & Input
- Per-binding local/global scope, mouse button bindings, dynamic Script/Launch entries
- Native MIDI input (50 mapping slots, learn mode, button/knob actions)
- Game controller support with JSON config and IPC command binding

### Preset Annotations
- Persistent per-preset ratings, flags (favorite/error/skip/broken), notes, and auto-captured shader errors

### Video & Display
- Native webcam and video file input mixing with luma key compositing
- Monitor mirroring, Workspace Layout, Video Effects with VFX profiles
- Two-pass shader blending for preset transitions

### Audio & FFT
- FFT EQ smoothing with attack/decay and peak hold
- Shader functions: `get_fft()`, `get_fft_hz()`, `get_fft_peak()`, `get_fft_peak_hz()`

### UI & Settings
- Tri-mode theme (Dark/Light/Follow System), Button Board, Text Animations
- File association for .milk/.milk2, command-line preset loading, drag-and-drop

## Fixed Issues (since v1.0)

- Fixed fullscreen black rendering for dot-based presets (DX12 point size emulation)
- Fixed dark/incorrect blur presets (removed DX9 half-texel UV offsets)
- Fixed non-shader preset rendering (comp shader binding, shape alpha blend, warp decay)
- Fixed pre-MilkDrop2 preset rendering (clamp sampler, Y-flip)
- Fixed ns-eel2 `regNN * regNN` multiply bug (optimizer treated different regs as identical)
- Fixed `ps_2_a` silently dropping texture bindings (raised to ps_3_0 minimum)
- Fixed comp shader reading wrong input texture
- Fixed TDR crash when disabling mirror outputs
- Fixed render hang on display mode switching
- Fixed mirror window deadlock
- Fixed global hotkeys not dispatching most actions
- Fixed `sampler_rand` black screen and texture directory search
- Fixed 17+ GLSL converter issues (variable shadowing, matrix chains, precision strips, texture bias, and more)
- Fixed channel auto-detection false positives
- Fixed screenshot filename after shader import
- Fixed notification overlay persisting across preset changes
- Fixed HUD text overflow on portrait/large displays

## Installation

Download the portable zip below, extract to any folder with write access, and run `MDropDX12.exe`. No installer or admin privileges required. No VC++ Redistributable needed.

Press **F8** to open Settings. Press **F1** for keyboard shortcuts.

## Full Changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md) for the complete list of changes across all releases.
