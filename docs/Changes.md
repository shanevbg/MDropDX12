# MDropDX12 Changelog

## v1.4 (2026-03-03)

### ToolWindow System

- Extracted Settings, Displays, Song Info, and Hotkeys windows into standalone ToolWindow subclasses running on their own threads
- Each ToolWindow has independent always-on-top pin button, font synchronization, dark theme support, and sticky window positions
- ToolWindows remember their last active tab between sessions
- Displays tab split into Display Outputs and Video Input sub-tabs

### Configurable Hotkeys

- Added dedicated Hotkeys window (Ctrl+F7) with ListView showing all 6 configurable bindings
- Hotkey capture control with Set/Clear buttons and per-binding local/global scope toggle
- Local hotkeys work when the render window has focus; global hotkeys work system-wide via RegisterHotKey
- Conflict detection automatically clears duplicate bindings when assigning a new hotkey
- Reset to Defaults button restores original key assignments
- Default shortcuts: Ctrl+F8 (Displays), Shift+Ctrl+F8 (Song Info), Ctrl+F7 (Hotkeys)
- Replaces the old Global Hotkeys section on the Settings System tab with a "Hotkeys..." button

### Theme System

- Replaced boolean Dark Theme checkbox with tri-mode Theme selector: Dark, Light, Follow System
- Follow System mode reads Windows AppsUseLightTheme registry key and auto-switches when the user changes their Windows theme (via WM_SETTINGCHANGE)
- Light mode uses standard Windows system colors for all ToolWindows
- Dark mode unchanged (dark green theme matching MilkVision)
- Theme setting persists to INI; migrates old DarkTheme=0/1 to new ThemeMode format
- DWM caption, border, and text colors properly reset when switching to Light mode

### Video Input

- Added native webcam capture via Windows Media Foundation (no external tools needed)
- Added video file playback (MP4, AVI, WMV, MKV) with loop option
- Unified Spout, webcam, and video file into a single Video Input system with shared compositing controls
- All sources share the same pixel shader pipeline (luma key, opacity, background/overlay layer)
- Video input sources auto-restore from saved settings on startup (lazy-init on first render frame)

### Settings UI

- Added pin icon button (top-right of tab header) to toggle Settings window always-on-top
- Replaced "Enable" checkbox with source selector combo (None / Spout / Webcam / Video File)
- Added webcam device selector with refresh button
- Added video file browser with loop checkbox

### Bug Fixes

- Fixed Spout input not rendering on startup (receiver was never initialized from saved settings)
- Fixed video file playback failing for most MP4s (added stream selection, hardware transforms, proper stride handling via IMF2DBuffer)
- Fixed video input invisible despite successful decode (MFVideoFormat_RGB32 has alpha=0; shader now sets alpha directly from opacity)
- Fixed overlay radio button not responding (radio group handler swallowed non-log-level radio clicks)
- Fixed global hotkey focus and Always Show Track Info
- Fixed Always Show Track Info bypassing Overlay Notifications check
- Separated track info from error notification bucket so it survives resize/preset changes

### Native MIDI Input

- Added native MIDI input system with 50 mapping slots (Button/Knob actions)
- MIDI window accessible from Settings → System → MIDI button, runs on its own ToolWindow thread
- Device selector with Scan button, Enable checkbox, and configurable buffer delay
- Learn mode: select a row, click Learn, then press a MIDI button or turn a knob to auto-assign
- Button actions: NEXT, PREV, LOCK, RAND, HARDCUT, MASHUP, FULLSCREEN, STRETCH, SETTINGS, PRESETINFO, BLACKOUT, and all IPC commands
- Knob actions: Hue, Saturation, Brightness, Intensity, Shift, Speed, FPS Factor, Quality, Opacity, Amp Left, Amp Right
- JSON persistence (midi.json) with Save/Load/Defaults buttons
- INI persistence for device selection and enabled state

### Dynamic Script & Launch App Hotkeys

- Replaced fixed Script (10) and Launch App (4) slots with unlimited dynamic entries
- Click "+" button to add a Script Command or Launch App binding
- Script commands support Browse button for script files (.txt, .bat, .cmd, .ps1)
- Launch App entries launch or focus external programs (process enumeration + EnumWindows)
- All editing (key assignment, scope, command/path) consolidated into a single modal Edit dialog
- User entries can be deleted; built-in bindings cannot
- Reset to Defaults only resets built-in keys; user entries are preserved
- INI format upgraded to Version 3 (old Script/Launch keys cleaned out automatically)

### Input & Control

- Controller buttons can now bind any IPC command (e.g. `OPACITY=0.5`, `COL_HUE=0.3`, `PRESET=name.milk`)
- Added CAPTURE, SPOUT, BLACKOUT named controller commands
- Updated controller help popup with full command reference

### IPC / Remote Control

- Handled SPOUTINPUT= IPC command (Spout sender name, enable/disable via text protocol)
- Handled WM_ENABLESPOUTMIX, WM_SETSPOUTSENDER direct messages from Milkwave Remote
- Handled WM_SET_INPUTMIX_ONTOP, WM_SET_INPUTMIX_OPACITY, WM_SET_INPUTMIX_LUMAKEY direct messages
- Fixed WM_ENABLESPOUTMIX incorrectly toggling Spout output instead of Spout input

### Code

- New files: video_capture.h, video_capture.cpp (Media Foundation capture with dedicated thread)
- New files: engine_hotkeys_ui.cpp, engine_songinfo_ui.cpp (ToolWindow subclasses)
- New files: midi_input.h, midi_input.cpp (winmm MIDI input wrapper)
- New file: engine_midi_ui.cpp (MidiWindow ToolWindow + Engine MIDI functions)
- Linked Media Foundation libraries (mfplat.lib, mfreadwrite.lib, mfuuid.lib, mf.lib)
- Extracted shared CompositeVideoInput() from Spout-specific CompositeSpoutInput()
- Extracted DX12 helper functions to reduce duplication across engine files

## v1.3 (2026-03-02)

### Performance

- Removed per-frame debug overhead from render loop (unconditional sprintf/DebugLog calls)
- Gated diagnostic logging with once-per-preset flag instead of per-frame time checks
- Replaced O(n) vector::erase(begin()) with O(1) ring buffer for audio band smoothing (bass_smooth, mid_smooth, treb_smooth)
- Cached QPC frequency; standardized all throttles to std::chrono::steady_clock

### Audio

- Consolidated 11-file src/audio/ into 2 files (audio_capture.h/cpp) inside src/mDropDX12/
- Removed dead weight: CPrefs command-line parser, WAV file recording, duplicate logging system

### Engine

- Self-bootstrapping exe with embedded shaders — no external .fx files required
- Disk .fx files in resources/data/ now serve as user overrides (embedded shaders are primary)

### Settings UI

- About tab now shows active paths (Base Dir, Settings INI, Presets directory)

### Resources

- Pared down bundled resources — ships curated texture-mix presets and textures only
- Removed legacy buttons, icons, docs, shapes, waves, sprites, and BeatDrop preset pack
- Removed resources/data/ shader files (now embedded in exe)

## v1.2 (2026-03-01)

### Media Integration

- Added song title rendering with selectable track info sources (Spotify, System Media, Window Title)
- Added Window Title regex profiles with configurable parse patterns and named capture groups
- Added Artist-Title Match Editor popup with live preview, enumerated windows dropdown, and profile management
- Added per-profile poll interval (1-10 seconds) for window title polling

### Display & Window

- Added Alt+S mirror mode failsafe: prompts to enable all monitors when none are enabled
- Added "Don't ask when no mirrors are enabled" checkbox on Displays tab to auto-enable all monitors

### Documentation & Project

- Clarified project attribution and derivation chain (MilkDrop2 → BeatDrop → Milkwave → MDropDX12)
- Rewrote bundled README.txt for MDropDX12 with correct maintainer and project info
- Removed ambiguous first-person references inherited from upstream projects

## v1.1 (2026-03-01)

### Rendering & Performance

- Replaced SpoutDX9 with SpoutDX12 via D3D11On12 interop for OBS compatibility
- Replaced GDI overlay with DX12 font atlas for all HUD text rendering
- Replaced Sleep()-based FPS limiter with high-resolution waitable timer
- Separated render thread from message pump with fire-and-forget IPC
- Added VSync toggle and tearing support
- Added FPS cap dropdown to Visual tab (30/60/90/120/144/240/360/720/Unlimited) and Ctrl+Shift+F3 reset hotkey
- Fixed all 45 build warnings (now 0 warnings)

### Expression Evaluation

- Replaced projectM-eval with native WDL ns-eel2 for x64 JIT compiled expression evaluation

### Display & Window

- Added unified Display Outputs system with monitor mirroring and multiple Spout senders
- Added mirror mode (ALT+S) with safety controls (activation button, click-through, opacity, topmost)
- Added per-display settings and configurable global hotkeys
- Added idle timer / screensaver mode
- Restored window focus after TDR device rebuild

### Settings UI

- Added Sprites tab with full sprite management UI (blend modes, layers, EEL code, import dialog)
- Added Script tab with Milkwave-compatible BPM-timed script file playback
- Added Content Base Path setting to Files tab for textures and sprites
- Added Show Messages and Show Sprites checkboxes to Messages tab
- Added log level radio buttons (Off/Error/Warn/Info/Verbose)
- Added sprite layer rendering support and redesigned sprite properties panel

### Input & Control

- Added game controller support with JSON configuration, settings UI, 14-button mapping (Xbox reference)
- Added media key hotkeys (play/pause/stop/next/prev/rewind/ff) and song info polling
- Added Milkwave Remote button support
- Fixed global hotkeys: register on render thread, disable controls when off, error feedback

### Messages & Sprites

- Added Send Now button and per-message randomization (position, size, font, color, effects, growth, duration)
- Added animation and color shifting overrides to Message Overrides dialog
- Added Spout video input mixing with background/overlay compositing

### Presets

- Added drag-and-drop preset loading with folder support and multi-file DND directory
- Added Quicksave2 (Ctrl+Shift+S) slot

### Project

- Refactored source directory: `vis_milk2/` → `mDropDX12/`, `Milkdrop2PcmVisualizer` → `App`
- Unified license under CC-BY-NC 4.0 (third-party components retain original licenses)
- Renamed project from MDropDX12Visualizer to MDropDX12

### Documentation

- Added comprehensive Manual.md, Architecture.md, and comparison chart
- Consolidated documentation into docs/ directory

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

- Dark theme UI with 11 tabs: General, Visual, Colors, System, Files, Messages, Sprites, Remote, Script, Displays, About
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
