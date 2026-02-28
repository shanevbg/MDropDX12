# MDropDX12 Changelog

## v1.0 (2026-02-28)

Initial release. Forked from Milkwave v3.5-dev Visualizer and rebuilt as a standalone DirectX 12 application.

### DirectX 12 Rendering Engine

- Ground-up DirectX 12 rebuild replacing DX9Ex entirely
- x64 native build
- DX12 3D volume texture support (noisevol_lq/noisevol_hq)
- Async shader compilation with timeout and auto-skip (prevents GPU stalls)
- TDR recovery with focus restoration after device lost
- Shader precompiling and caching
- HLSL variable shadowing fix (auto-renames variables shadowing built-in functions)
- GLSL-to-HLSL shader conversion
- PSVersion=4 support for AMD GPUs
- VSync toggle and FPS cap (30/60/90/120/144/240/360/720/Unlimited)

### Expression Evaluation

- Replaced projectM-eval with native ns-eel2 (x64 JIT compiled)
- Full MilkDrop2 expression compatibility

### GDI Overlay HUD

- Separate layered window for text rendering (preset name, FPS, notifications, debug info)
- Independent of DX12 render pipeline

### Settings Window (F8)

- Dark theme UI with 10 tabs: General, Visual, Colors, Sound, Files, Messages, Sprites, Remote, Script, About
- Preset browser with directory navigation
- Resource viewer showing all preset textures with load status and paths
- Fallback texture search paths and Random Textures Directory
- Content Base Path for textures and sprites
- Log level control (Off/Error/Warn/Info/Verbose)

### Audio

- WASAPI loopback capture with input device support
- On-the-fly device selection
- Hi-res audio support
- Smooth audio variables (bass_smooth, mid_smooth, treb_smooth, vol_smooth)
- Signal amplification
- Audio sensitivity with auto-adaptive mode

### Presets

- .milk preset loading with full compatibility
- Preset browser with directory navigation
- Preset tagging system
- Quicksave (Ctrl+S) and Quicksave2 (Ctrl+Shift+S)
- Auto-change timer and preset change on track change
- Drag-and-drop preset loading
- Soft blend transitions with 19+ blend patterns
- Beat-driven hard cuts with 13 configurable modes
- Async shader compilation (non-blocking transitions)

### Window Modes

- Fullscreen, borderless windowed, windowed fullscreen
- Window transparency with opacity control
- Clickthrough mode and watermark mode
- Always on top
- Multiple monitor stretch
- Black mode (hide rendering)

### Messages and Sprites

- Up to 100 custom message slots with per-message properties
- Message autoplay with interval, jitter, and sequential/random order
- Per-message randomization (position, size, font, color, effects, growth, duration)
- Global message overrides dialog
- Send Now button for message preview
- Sprite management with blend modes, layers, EEL code, and positioning
- Independent show/hide toggles for messages and sprites

### Media Integration

- Now Playing track info from Spotify, YouTube, and system media
- Album artwork display
- Media transport hotkeys (play/pause/stop/next/prev/rewind/ff)
- Preset change on track change

### Spout

- Spout texture output for sharing frames with other applications
- Window-independent fixed output resolution (64x64 to 7680x4320)

### Remote Control

- Milkwave Remote IPC compatibility via WM_COPYDATA protocol
- 32 of 34 Milkwave commands handled
- Configurable window title for Remote discovery

### Color Effects

- Hue/Saturation/Brightness shifting with auto hue cycling
- Brighten/darken/solarize/invert effects (F11)
- Custom preset variables (vis_intensity, vis_shift, vis_version, colshift_hue)

### Screenshots

- Ctrl+X auto-saves to capture/ folder as timestamped PNG
- Save Screenshot button in Settings with file dialog

### Script System

- BPM-timed preset script playback
- Loop mode with beat counter
