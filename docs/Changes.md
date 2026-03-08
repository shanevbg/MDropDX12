# MDropDX12 Changelog

## v1.5.1 (2026-03-08)

### Shader Pipeline

- Rearchitected sampler registers: switched .milk/.milk2 pipeline from `sampler2D` (1 s-register per texture, 16 limit) to separated `Texture2D` + 4 shared `SamplerState` objects (LINEAR+WRAP, LINEAR+CLAMP, POINT+CLAMP, POINT+WRAP)
- Textures now use t-registers (~128 limit), eliminating X4510 "maximum sampler register exceeded" errors on texture-heavy presets
- Old preset code preserved via compatibility macros (`sampler2D`, `tex2D`, `tex2Dlod`, `tex2Dbias`)
- Special-mode samplers (fc_main, pc_main, pw_main, blur) handled via text substitution in LoadShaderFromMemory
- Root signature reduced from 16 to 4 static samplers; blur root signature shares main
- Expanded texture binding arrays from 16 to 32 slots, descriptor table from 16 to 32 SRVs
- SRV heap increased from 1280 to 2560 with overflow protection
- Fixed `sampler_rand` redefinition errors (~20 presets) by stripping include declarations when preset declares its own
- Fixed `line` keyword conflict (HLSL geometry shader keyword) by adding it to shadowed builtins list
- Stripped Shadertoy-specific texture declarations for non-Shadertoy presets to avoid register pressure
- Added `tex2Dbias` compatibility macro for presets using the DX9 intrinsic
- Improved shader error logging: failures logged at LOG_ERROR with preset filename

### ToolWindow System

- Added ModalDialog base class for themed popup dialogs with shared font/theme/DPI support
- Migrated all popup dialogs to ModalDialog (save preset, rename, delete, etc.)
- Fixed all owner-draw checkbox/radio controls across all ToolWindow tabs — `BM_GETCHECK`/`IsDlgButtonChecked`/`CheckDlgButton` do not work with BS_OWNERDRAW controls
- Affected controls: Sequential Preset Order, Hard Cuts Disabled, Preset Lock, Script Loop, Sprite flipx/flipy/burn, Video Effects mirror/invert/edge detect, VFX Profiles startup/save-on-close, MIDI Enable, Messages Autoplay
- Added text shrink-to-fit for messages, song titles, and preset names
- Added Animations tab to Message Overrides dialog
- Fixed ToolWindow resize bugs

### Debug Logging

- Debug log now reports log level name when starting a new log and when the level is changed
- Shader compilation failures now include the preset filename in the log message

## v1.5.0 (2026-03-07)

### Named Pipe IPC

- Replaced WM_COPYDATA / hidden window IPC with Named Pipes (`\\.\pipe\Milkwave_<PID>`)
- PID-based discovery eliminates fragile window-title matching — no more EnumWindows or FindWindow
- Removed hidden 1x1 IPC window (IPCWindowThread, IPCWindowProc, g_szIPCWindowTitle)
- Duplex message-mode pipe: visualizer can send messages back to Remote
- Non-blocking outgoing messages via pipe write thread (replaces fire-and-forget SendMessage worker)
- Second-instance forwarding rewritten: uses CreateToolhelp32Snapshot to find running instances, sends preset path over pipe
- New files: `pipe_server.h`, `pipe_server.cpp` — self-contained PipeServer class with listen/read/write threads
- Signal messages (`SIGNAL|NAME=VALUE`) replace PostMessage-based WM_USER+N signals
- All existing text message formats unchanged (MSG|, PRESET=, WAVE|, STATE, SPOUT_SENDER=, etc.)
- Configurable WM base offset via `PipeServer::Start(hwnd, wmIPCMessage, wmSignalBase)` for cross-project portability
- Settings UI (Remote tab) updated to show pipe name and connection status instead of window titles
- Pipe server tracks connected client exe path via `GetNamedPipeClientProcessId` for auto-launch
- Removed: IPC worker thread, IPC hidden window, WM_MW_RESTART_IPC, g_szIPCWindowTitle, StartIPCThread/StopIPCThread

### Animated Song Titles

- DX12 warped text animation for song title rendering with selectable track info sources
- Windows color picker and font picker dialogs (replaced raw R/G/B fields)
- Color swatch click-to-open color picker
- Export/Import for animation profiles
- Custom preview text field in Text Animations window

### Mouse Button Hotkeys

- Left, Right, Middle, X1, and X2 mouse buttons can now be assigned as hotkey bindings
- New "Mouse:" dropdown combo in the hotkey edit dialog (mutual exclusion with keyboard key)
- Mouse button bindings are forced to local scope (RegisterHotKey does not support mouse buttons)
- Added WM_XBUTTONDOWN handler for X1/X2 mouse button dispatch

### Open Remote Action

- New HK_OPEN_REMOTE hotkey action: finds and activates Milkwave Remote window, or launches it if not running
- Searches for both "MDropDX12 Remote" and "Milkwave Remote" window titles
- Remembers last pipe-connected Remote exe path across sessions (persisted to INI)
- New HK_POLL_TRACK_INFO hotkey action: force immediate track info poll

### .milk3 Preset Support

- File dialogs (Open, Save As) now include `.milk3` filter alongside `.milk` and `.milk2`
- Preset browser type filter updated: All / .milk / .milk2 / .milk3

### Bootstrap

- Self-bootstrap now prefers a `resources/` directory next to the exe over walking up parent directories
- Prevents finding a stray `resources/data/` in an unrelated ancestor directory

### Bug Fixes

- Fixed upside-down sprites by re-enabling DX12 Y-flip (DX12 passthrough VS doesn't negate Y like DX9 OrthoLH)
- Fixed resize/fullscreen triggering an unwanted next-preset transition
- Fixed device recovery (TDR) reloading the same preset instead of skipping to next
- Fixed C5208 build warnings (anonymous typedef struct with static const member)

### Code

- New files: `pipe_server.h`, `pipe_server.cpp` (Named Pipe IPC server)
- New file: `engine_textanim_ui.cpp` (Text Animations window)
- Removed ~200 lines of IPC window thread and worker thread code from App.cpp and engine_presets.cpp
- Zero code warnings in Release build

## v1.4.3 (2026-03-07)

### ToolWindow Improvements

- Radio button groups are now auto-toggled by the base class via `radioGroup` parameter on `CreateRadio()` — subclasses no longer need manual toggle boilerplate
- Updated `docs/tool_window.md` with radio group auto-toggle documentation

### Bug Fixes

- Fixed Messages window checkboxes (Show Messages, Autoplay, Sequential, Autosize) always reading as unchecked — clicking "Show Messages" would silently disable messages
- Fixed MIDI window Enable checkbox always reading as unchecked
- Root cause: `IsDlgButtonChecked()` silently returns 0 for BS_OWNERDRAW controls; replaced with `IsChecked()`

## v1.4.2 (2026-03-06)

### Packaging

- Portable zip now contains only the exe and README - the exe self-bootstraps all config files, directories, and defaults on first run
- Removed bundled presets and textures from repository
- Added `docs/Resources.md` with links to recommended preset collections
- Featured IkeC's [Milkwave preset collection](https://github.com/IkeC/Milkwave/tree/main/Visualizer/resources) as the recommended starting point

### Scripting

- Added comprehensive scripting guide (`docs/Scripts.md`) covering all script commands, ACTION= dispatch, and examples
- Added `ACTION=OpenWorkspaceLayout` hotkey action to open the Workspace Layout window
- Added `ACTION=ApplyWorkspaceLayout` hotkey action to apply saved workspace layout with a single keypress
- Workspace layout can now be triggered via script, hotkey binding, or Button Board action

### Bug Fixes

- Fixed owner-draw checkboxes always reading as unchecked in ToolWindows (base class auto-toggle)
- Fixed light mode rendering: ToolWindow backgrounds and Button Board now use system colors
- Fixed About tab showing hardcoded version instead of reading from `version.h`

## v1.4 (2026-03-06)

### Shadertoy Import (.milk3)

- Added Shadertoy-compatible rendering pipeline with GLSL-to-HLSL converter
- New `.milk3` JSON preset format supporting multi-pass shaders (Buffer A, Buffer B, Image, Common)
- SM5.0 (`ps_5_0`) shader compilation for all Shadertoy presets
- FLOAT32 ping-pong feedback buffers for temporal accumulation effects
- sRGB gamma correction post-process for Shadertoy-accurate color output
- Shadertoy-compatible `iMouse` input (pixel coords, left-click drag, button state via sign encoding)
- Channel auto-detection: `iChannel0` maps to self-feedback or noise depending on shader context
- Buffer A self-feedback detection via screen-space textureLod pattern matching
- JSON channel import supports string names (`"self"`, `"bufferA"`, `"noiseLQ"`, `"audio"`, etc.)
- Audio texture (512x2, R32_FLOAT) with FFT spectrum and PCM waveform rows
- Noise textures (LQ/MQ/HQ) and 3D volume noise textures for Shadertoy compatibility

### Shader Import Window

- Two-panel Shader Import UI: pass listbox (left) with shader editor (right)
- Multi-pass support: add/remove Buffer A, Buffer B, Common, and Image passes
- Per-pass channel input combos (ch0–ch3) with auto-detection from GLSL source
- Convert & Apply button: GLSL→HLSL conversion + live preview in one click
- Save .milk3 button: export converted shaders as portable JSON presets
- Automatic .milk3 name suggestion from Shadertoy project name
- Comprehensive GLSL→HLSL converter handling matrices, structs, vector comparisons, array params, and more
- Error display with scrollable output showing conversion and compilation diagnostics

### Video Effects Window

- Standalone Video Effects window with transform, color, and audio-reactive controls
- Transform controls: scale, rotation, X/Y offset with real-time sliders
- Color controls: brightness, contrast, saturation, hue shift
- Audio-reactive mode: link visual parameters to bass/mid/treb audio bands
- VFX JSON profiles: save/load effect presets as named JSON files
- Startup preset mode selector: choose which VFX profile loads on startup

### Workspace Layout Window

- New Workspace Layout window for tiling tool windows across the screen
- Two modes: corner (render in screen corner) or fullscreen on separate display
- Corner picker (TL/TR/BL/BR) with render size slider (5–50%)
- Display picker combo for multi-monitor fullscreen mode
- Checkbox grid for selecting which tool windows to open and tile
- Apply Layout button opens selected windows and arranges them in a grid
- Reset to Defaults button restores default layout
- Accessible from Welcome window and Settings → About tab

### ToolWindow System

- Extracted Settings, Displays, Song Info, and Hotkeys windows into standalone ToolWindow subclasses running on their own threads
- Each ToolWindow has independent always-on-top pin button, font synchronization, dark theme support, and sticky window positions
- ToolWindows remember their last active tab between sessions
- Displays tab split into Display Outputs and Video Input sub-tabs
- Added Presets window, Sprites window, and Messages window as standalone ToolWindows
- Added `IsChecked()` / `SetChecked()` helper methods to ToolWindow base class for owner-draw checkbox/radio state
- Base class auto-toggles checkbox state on click (subclasses no longer need toggle boilerplate)
- Comprehensive ToolWindow documentation at `docs/tool_window.md`

### Welcome Window

- First-run Welcome window with quick-start options
- Browse for Resources folder, Open Shader Import, Open Settings, Setup Workspace Layout buttons
- Appears automatically when no presets are found; can be dismissed

### Configurable Hotkeys

- Added dedicated Hotkeys window (Ctrl+F7) with ListView showing all configurable bindings
- Hotkey capture control with Set/Clear buttons and per-binding local/global scope toggle
- Local hotkeys work when the render window has focus; global hotkeys work system-wide via RegisterHotKey
- Conflict detection automatically clears duplicate bindings when assigning a new hotkey
- Reset to Defaults button restores original key assignments
- Default shortcuts: Ctrl+F8 (Displays), Shift+Ctrl+F8 (Song Info), Ctrl+F7 (Hotkeys)

### Theme System

- Replaced boolean Dark Theme checkbox with tri-mode Theme selector: Dark, Light, Follow System
- Follow System mode reads Windows AppsUseLightTheme registry key and auto-switches when the user changes their Windows theme (via WM_SETTINGCHANGE)
- Light mode uses standard Windows system colors for all ToolWindows
- Dark mode unchanged (dark green theme matching MilkVision)
- Fixed light mode rendering: ToolWindow backgrounds, Button Board panel, and owner-draw controls now render correctly in light mode
- DWM caption, border, and text colors properly reset when switching to Light mode

### Video Input

- Added native webcam capture via Windows Media Foundation (no external tools needed)
- Added video file playback (MP4, AVI, WMV, MKV) with loop option
- Unified Spout, webcam, and video file into a single Video Input system with shared compositing controls
- All sources share the same pixel shader pipeline (luma key, opacity, background/overlay layer)
- Video input sources auto-restore from saved settings on startup (lazy-init on first render frame)

### Native MIDI Input

- Added native MIDI input system with 50 mapping slots (Button/Knob actions)
- MIDI window accessible from Settings → System → MIDI button, runs on its own ToolWindow thread
- Device selector with Scan button, Enable checkbox, and configurable buffer delay
- Learn mode: select a row, click Learn, then press a MIDI button or turn a knob to auto-assign
- Button actions: NEXT, PREV, LOCK, RAND, HARDCUT, MASHUP, FULLSCREEN, STRETCH, SETTINGS, PRESETINFO, BLACKOUT, and all IPC commands
- Knob actions: Hue, Saturation, Brightness, Intensity, Shift, Speed, FPS Factor, Quality, Opacity, Amp Left, Amp Right
- JSON persistence (midi.json) with Save/Load/Defaults buttons

### Button Board

- Added per-slot image thumbnails (Set Image / Clear Image context menu, drag-drop image files onto slots)
- Added per-slot hotkey bindings (modifiers, key, local/global scope) persisted in INI
- Added RunScript and LaunchApp action types for button slots
- Added JSON layout export/import (Save Layout / Load Layout in config menu)
- Added Reset to Defaults with Milkwave Remote-style default layout (auto-populates on first run)
- Added "Assign Action..." cascading submenu with all built-in actions organized by category
- Shared ActionEditDialog used by both Button Board and Hotkeys windows
- Button Board forwards all keyboard input to render window so VJ hotkeys work while Board has focus
- Fixed Button Board using dark colors in light mode

### Dynamic Script & Launch App Hotkeys

- Replaced fixed Script (10) and Launch App (4) slots with unlimited dynamic entries
- Click "+" button to add a Script Command or Launch App binding
- Script commands support Browse button for script files (.txt, .bat, .cmd, .ps1)
- Launch App entries launch or focus external programs (process enumeration + EnumWindows)
- All editing consolidated into a single modal Edit dialog
- INI format upgraded to Version 3

### Settings UI

- Added Tools tab with launcher buttons for all tool windows
- Added pin icon button (top-right of tab header) to toggle Settings window always-on-top
- Replaced "Enable" checkbox with source selector combo (None / Spout / Webcam / Video File)
- Added webcam device selector with refresh button
- Added video file browser with loop checkbox
- About tab now shows version from single source of truth (`version.h`)
- Added Workspace Layout button on About tab
- Preset filter by type (All / .milk / .milk2) in preset browser

### Version Management

- Added `version.h` as single source of truth for app version number
- Version defines used by About tab, `engine.rc` (FileVersion/ProductVersion), and all version references
- Updated version from 1.3 to 1.4.0

### Input & Control

- Controller buttons can now bind any IPC command (e.g. `OPACITY=0.5`, `COL_HUE=0.3`, `PRESET=name.milk`)
- Added CAPTURE, SPOUT, BLACKOUT named controller commands

### IPC / Remote Control

- Handled SPOUTINPUT= IPC command (Spout sender name, enable/disable via text protocol)
- Handled WM_ENABLESPOUTMIX, WM_SETSPOUTSENDER direct messages from Milkwave Remote
- Handled WM_SET_INPUTMIX_ONTOP, WM_SET_INPUTMIX_OPACITY, WM_SET_INPUTMIX_LUMAKEY direct messages
- Fixed WM_ENABLESPOUTMIX incorrectly toggling Spout output instead of Spout input

### Bug Fixes

- Fixed owner-draw checkboxes always reading as unchecked (IsDlgButtonChecked doesn't work with BS_OWNERDRAW)
- Fixed light mode: ToolWindow WM_ERASEBKGND now explicitly paints light background instead of relying on stale class brush
- Fixed light mode: ButtonPanel defaults to system colors when not in dark mode
- Fixed GLSL precision declaration causing shader compile failure
- Fixed GLSL converter: *= matVar backward scan order for square matrix multiply
- Fixed GLSL converter: Buffer A self-feedback detection, bare return, hardcoded resolution
- Fixed GLSL converter: matrix funcs, array params, struct ctors, vector comparisons
- Fixed Shadertoy import: alpha output, per-pass diagnostics, Image texture V-flip
- Fixed Shadertoy time variable shadowing for animation looping
- Fixed Shadertoy converter: UV calculation, matrix functions, inout structs
- Fixed Shadertoy converter: over-specified constructors, iChannel0 mapping, sRGB gamma
- Fixed ResizeBuffers E_INVALIDARG by passing tearing flag
- Fixed Shadertoy shader error dialog on resolution change
- Fixed mirror opacity hang, default to 100%, and add display profiles
- Fixed invisible controls in Messages/Sprites windows
- Fixed Ctrl+A/C/V/X/Z not working in tool window edit boxes
- Fixed LaunchOrFocusApp focus stealing and control ID collision
- Fixed Spout input not rendering on startup
- Fixed video file playback for most MP4s (stream selection, hardware transforms, proper stride handling)
- Fixed video input invisible despite successful decode (alpha=0 in MFVideoFormat_RGB32)
- Fixed overlay radio button not responding
- Fixed global hotkey focus and Always Show Track Info
- Separated track info from error notification bucket so it survives resize/preset changes

### Code

- New file: `version.h` (single source of truth for version number)
- New file: `engine_workspace_layout_ui.cpp` (Workspace Layout window)
- New file: `engine_video_effects_ui.cpp` (Video Effects window)
- New file: `engine_vfx_profiles_ui.cpp` (VFX JSON profiles)
- New files: `engine_hotkeys_ui.cpp`, `engine_songinfo_ui.cpp` (ToolWindow subclasses)
- New files: `midi_input.h`, `midi_input.cpp` (winmm MIDI input wrapper)
- New file: `engine_midi_ui.cpp` (MidiWindow ToolWindow + Engine MIDI functions)
- New files: `video_capture.h`, `video_capture.cpp` (Media Foundation capture)
- New files: `json_utils.cpp/h` (lightweight JSON writer/reader)
- New file: `docs/tool_window.md` (comprehensive ToolWindow developer reference)
- Linked Media Foundation libraries (mfplat.lib, mfreadwrite.lib, mfuuid.lib, mf.lib)
- Static CRT linking (no VC++ Redistributable required)
- Consolidated all OutputDebugString calls to DebugLogA/W
- Level-gated debug macros with output destination control

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
