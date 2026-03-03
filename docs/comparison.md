# Feature Comparison: MDropDX12 vs Milkwave vs MilkDrop3 vs projectM

Comparison of four MilkDrop-based music visualizer projects on Windows 11 x64.

| Project | Description | Graphics API | Status |
| ------- | ----------- | ------------ | ------ |
| **MDropDX12** | Ground-up DX12 rebuild of MilkDrop2 engine | DirectX 12 | Active (v1.4) |
| **Milkwave** | Remote control app + bundled MilkDrop2 visualizer | DirectX 9Ex | Active (v3.5) |
| **MilkDrop3** | Enhanced MilkDrop2 fork (reference visualizer) | DirectX 9Ex | Active (v3.31) |
| **projectM** | Open-source MilkDrop reimplementation (SDL standalone) | OpenGL | Pre-release (lib v4.1.6) |

## Rendering

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| DirectX 12 rendering | ✅ | ❌ (DX9Ex) | ❌ (DX9Ex) | ❌ (OpenGL) |
| x64 build | ✅ | ❌ (x86) | ❌ (x86) | ✅ |
| Fullscreen | ✅ | ✅ | ✅ | ✅ |
| Borderless windowed | ✅ | ✅ | ✅ | ❌ |
| Window transparency | ✅ | ✅ | ✅ | ❌ |
| Clickthrough / watermark mode | ✅ | ✅ | ❌ | ❌ |
| Always on top | ✅ | ✅ | ✅ | ❌ |
| Adjustable render quality | ✅ | ✅ | ✅ | ❌ |
| Auto quality (window-size adaptive) | ✅ | ✅ | ❌ | ❌ |
| Fixed window dimensions (config) | ✅ | ✅ | ❌ | ✅ |
| VSync toggle | ✅ | ✅ | ✅ | ❌ |
| FPS cap dropdown in settings UI | ✅ | ❌ | ❌ | ❌ |
| FPS cap hotkey cycling | ✅ | ✅ | ✅ | ❌ |
| Black mode (hide rendering) | ✅ | ✅ | ❌ | ❌ |

## Audio

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| WASAPI loopback capture | ✅ | ✅ | ✅ | ❌ (SDL audio) |
| Input device support (microphones) | ✅ | ✅ | ❌ | ✅ |
| On-the-fly device selection | ✅ | ✅ | ❌ | ✅ (Ctrl+A) |
| Hi-res audio support | ✅ | ✅ | ✅ (v3.31) | ❌ |
| Smooth audio variables (bass/mid/treb/vol_smooth) | ✅ | ✅ | ❌ | ❌ |
| Signal amplification | ✅ | ✅ | ❌ | ❌ |
| Disable audio capture option | ✅ | ✅ | ❌ | ❌ |

## Presets

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| .milk preset loading | ✅ | ✅ | ✅ | ✅ |
| .milk2 double-preset format | ✅ | ❌ | ✅ | ❌ |
| Preset browser (in-app) | ✅ | ✅ | ✅ | ❌ (WIP) |
| Preset browser filtering (Ctrl+F) | ✅ | ✅ | ❌ | ❌ |
| Preset tagging | ✅ | ✅ | ❌ | ❌ |
| Tag-based playlists | ✅ | ✅ | ❌ | ❌ |
| Age filter (modified within X days) | ✅ | ✅ | ❌ | ❌ |
| Text filter (presets containing text) | ✅ | ✅ | ❌ | ❌ |
| Quicksave (Ctrl+S) | ✅ | ✅ | ✅ | ❌ |
| Quicksave2 folder (Ctrl+Shift+S) | ✅ | ✅ | ❌ | ❌ |
| Preset auto-change timer | ✅ | ✅ | ✅ | ✅ |
| Preset change on track change | ✅ | ✅ | ❌ | ❌ |
| Drag-and-drop preset loading | ✅ | ❌ | ❌ | ❌ |
| Command-line / Explorer double-click loading | ✅ | ❌ | ❌ | ❌ |
| Preset filter by type (.milk / .milk2) | ✅ | ❌ | ❌ | ❌ |
| Preset mode buttons (assign/recall) | ❌ | ✅ | ❌ | ❌ |
| Deep mashup (multi-layer) | ❌ | ❌ | ✅ | ❌ |
| Preset lock | ✅ | ✅ | ✅ | ✅ (Space) |

## Preset Transitions

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Soft blend transitions | ✅ | ✅ | ✅ | ✅ |
| Hard cut (H key) | ✅ | ✅ | ✅ | ✅ |
| Beat-driven hard cuts | ✅ | ✅ | ✅ | ✅ |
| 19+ blend patterns (.milk2) | ✅ | ❌ | ✅ | ❌ |
| Async shader compilation (non-blocking) | ✅ | ❌ | ❌ | ❌ |
| Compilation timeout with auto-skip | ✅ | ❌ | ❌ | ❌ |
| Force soft transition type (Mixtype) | ✅ | ✅ | ❌ | ❌ |

## Shaders

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Warp/comp HLSL shaders | ✅ | ✅ | ✅ | ✅ (HLSL→GLSL transpilation) |
| Shader editor tab | ❌ | ✅ | ❌ | ❌ |
| MilkPanel shader editor | ❌ | ❌ | ✅ | ❌ |
| Shader precompiling and caching | ✅ | ✅ | ✅ (v3.31) | ❌ |
| HLSL variable shadowing fix | ✅ | ❌ | ❌ | n/a |
| PSVersion=4 (AMD GPU support) | ✅ | ✅ | ❌ | n/a |
| Shader randomize (!/@ keys) | ✅ | ✅ | ✅ | ❌ |

## Textures

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Disk texture loading | ✅ | ✅ | ✅ | ✅ |
| Content Base Path (textures, sprites) | ✅ | ❌ | ❌ | ❌ |
| Fallback texture search paths | ✅ | ❌ | ❌ | ❌ |
| Random Textures Directory | ✅ | ❌ | ❌ | ❌ |
| Fallback to 1x1 white texture | ✅ | ❌ | ❌ | ❌ |
| 3D volume textures (noisevol) | ✅ | ✅ | ✅ | ✅ |
| Resource viewer | ✅ | ❌ | ❌ | ❌ |

## Display & HUD

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| GDI overlay HUD | ✅ | ❌ | ❌ | ❌ |
| Preset name display | ✅ | ✅ | ✅ | ✅ |
| FPS display | ✅ | ✅ | ✅ | ❌ |
| Help screen | ✅ | ✅ | ✅ | ❌ |
| Notifications | ✅ | ✅ | ✅ | ❌ |
| CPU/GPU monitoring | ❌ | ✅ | ❌ | ❌ |
| Save screenshot (Ctrl+X) | ✅ | ✅ | ❌ | ❌ |

## Text & Messages

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Independent messages/sprites toggles | ✅ | ❌ | ❌ | ❌ |
| Message overrides (size, font, color, effects, animation) | ✅ | ✅ | ❌ | ❌ |
| Custom text messages (from Remote) | ♻️ | ✅ | ❌ | ❌ |
| Font/color customization | ♻️ | ✅ | ❌ | ❌ |
| Multiple simultaneous messages | ♻️ | ✅ | ❌ | ❌ |
| Message positioning (start/end coords) | ♻️ | ✅ | ❌ | ❌ |
| Background box for text | ♻️ | ✅ | ❌ | ❌ |
| Fade-out / burn-in timing | ♻️ | ✅ | ❌ | ❌ |
| Script file commands | ✅ | ✅ | ❌ | ❌ |

## Color & Visual Effects

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Hue/Saturation/Brightness shifting | ✅ | ✅ | ❌ | ❌ |
| Auto hue cycling | ♻️ | ✅ | ❌ | ❌ |
| Brighten/darken/solarize/invert | ✅ | ✅ | ✅ | ❌ |
| Custom preset vars (vis_intensity, vis_shift, vis_version) | ✅ | ✅ | ❌ | ❌ |
| colshift_hue preset variable | ✅ | ✅ | ❌ | ❌ |
| Screen-dependent render mode | ✅ | ✅ | ❌ | ❌ |
| Color/monochrome display toggle | ❌ | ✅ | ❌ | ❌ |

## Sprites

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Static sprites | ✅ | ✅ | ✅ | ❌ |
| Animated sprites | ✅ | ✅ | ✅ | ❌ |
| Sprite management UI (import, properties, defaults) | ✅ | ❌ | ❌ | ❌ |
| Sprite layer control (behind/on top of text) | ✅ | ❌ | ❌ | ❌ |
| Sprite selection from Remote | ✅ | ✅ | ❌ | ❌ |

## Shapes & Waves

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Custom shapes (per_frame/per_point) | ✅ | ✅ | ✅ | ✅ |
| Custom waves (per_frame/per_point) | ✅ | ✅ | ✅ | ✅ |
| Live wave manipulation | ♻️ | ✅ | ❌ | ❌ |
| Mouse interaction mode (Ctrl+M) | ❌ | ✅ | ❌ | ❌ |
| Expanded variable ranges | ❌ | ❌ | ✅ | ❌ |

## Input Mixing

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Webcam / video input mixing | ✅ | ✅ | ❌ | ❌ |
| Spout sender input mixing | ✅ | ✅ | ❌ | ❌ |
| Layer order and blending modes | ✅ (background/overlay) | ✅ | ❌ | ❌ |
| Luma key blending | ✅ | ✅ | ❌ | ❌ |

## MIDI & Controllers

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Native MIDI input (50 mapping slots, learn mode) | ✅ | ❌ | ❌ | ❌ |
| MIDI automation via Remote (50 controls) | ❌ | ✅ | ❌ | ❌ |
| MIDI tab in Remote | ❌ | ✅ | ❌ | ❌ |
| Launch App hotkeys (launch/focus external programs) | ✅ | ❌ | ❌ | ❌ |
| Game controller support | ✅ | ✅ | ❌ | ❌ |

## Media Integration

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Now Playing track info | ✅ | ✅ | ❌ | ❌ |
| Album artwork display | ✅ | ✅ | ❌ | ❌ |
| Song info (Spotify/YouTube/media) | ✅ | ✅ | ❌ | ❌ |
| Window title regex parsing (named profiles) | ✅ | ❌ | ❌ | ❌ |
| Windows Media Play/Pause/Stop keys | ✅ | ✅ | ❌ | ❌ |

## Remote Control

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Milkwave Remote compatibility | ♻️ | ✅ (built-in) | ❌ | ❌ |
| WM_COPYDATA IPC protocol | ♻️ | ✅ | ❌ | ❌ |
| Tabbed Remote UI | 🤝 | ✅ | ❌ | ❌ |
| Button panel (Remote buttons) | ♻️ | ✅ | ❌ | ❌ |

## Display Outputs

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| Spout texture output | ✅ (DX12 via D3D11On12) | ✅ (DX9) | ❌ | ❌ |
| Multiple Spout senders | ✅ | ❌ | ❌ | ❌ |
| Fixed-size Spout resolution | ✅ | ✅ | ❌ | ❌ |
| Monitor mirroring (DX12 copy) | ✅ | ❌ | ❌ | ❌ |
| ALT-S mirror mode (fullscreen + mirrors) | ✅ | ❌ | ❌ | ❌ |
| Unified Displays tab | ✅ | ❌ | ❌ | ❌ |
| Display output kill switch (Ctrl+F2) | ✅ | ❌ | ❌ | ❌ |

## GPU Protection

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| TDR recovery (device lost handling) | ✅ | ❌ | ❌ | ❌ |
| Focus restoration after TDC rebuild | ✅ | ❌ | ❌ | ❌ |
| Device restart from settings UI | ✅ | ❌ | ❌ | ❌ |
| Async compilation (prevents GPU stall) | ✅ | ❌ | ❌ | ❌ |
| Shader compile timeout | ✅ | ❌ | ❌ | ❌ |

## Settings & Configuration

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| In-app settings window (F8) | ✅ | ✅ | ✅ | ✅ (WIP) |
| Tri-mode theme (Dark / Light / Follow System) | ✅ | ❌ | ❌ | ❌ |
| Multi-tab settings (11 tabs) | ✅ | ✅ | ✅ | ❌ |
| Per-display opacity and click-through | ✅ | ❌ | ❌ | ❌ |
| Configurable hotkeys (local + global scope) | ✅ | ❌ | ❌ | ❌ |
| Idle timer / screensaver mode | ✅ | ❌ | ❌ | ❌ |
| ToolWindow system (own threads, sticky positions) | ✅ | ❌ | ❌ | ❌ |
| Settings tab memory | ✅ | ❌ | ❌ | ❌ |
| Game controller config UI | ✅ | ❌ (Remote-side) | ❌ | ❌ |
| File association registration (.milk/.milk2) | ✅ | ❌ | ❌ | ❌ |
| Verbose logging (LogLevel=2) | ✅ | ✅ | ❌ | ❌ |

## Expression Evaluation

| Feature | MDropDX12 | Milkwave | MilkDrop3 | projectM |
| ------- | --------- | -------- | --------- | -------- |
| ns-eel2 (native, x64 JIT) | ✅ | ❌ | ❌ | ❌ |
| ns-eel2 (native, x86) | ❌ | ❌ | ✅ | ❌ |
| projectM-eval (via ns-eel2 shim) | ❌ | ✅ | ❌ | ❌ |
| projectM-eval (native) | ❌ | ❌ | ❌ | ✅ |

---

## Notes

### Milkwave Remote Compatibility

Milkwave Remote finds the Visualizer window using `EnumWindows()` + `GetWindowText()` to match the window title. It communicates via `WM_COPYDATA` with Unicode pipe-delimited `key=value` messages. MDropDX12 runs a dedicated hidden IPC window that receives these commands, supporting 32 of 34 Milkwave commands. The window title is configurable in Settings > Remote tab.

### .milk2 Double-Preset Format

MilkDrop3 introduced `.milk2` files which contain two presets blended together with 19+ blend patterns. MDropDX12 also supports `.milk2` loading and blend transitions. Milkwave and projectM do not support this format.

### projectM on Windows

The projectM standalone visualizer ([frontend-sdl-cpp](https://github.com/projectM-visualizer/frontend-sdl-cpp)) is an SDL2-based application using the libprojectM rendering library (v4.1.6). On Windows it uses SDL audio capture rather than native WASAPI loopback, so it captures input devices (microphones) but does not capture desktop/system audio natively. The standalone app and its settings UI are still under active development (pre-release). projectM renders via OpenGL — it transpiles HLSL shader code from .milk presets to GLSL at runtime using an internal HLSL→GLSL transpiler (formerly Cg, now hlsltranslator). All .milk presets store HLSL shader bodies; no .milk presets in the wild use native GLSL. projectM does not implement sprites, the MilkDrop text message system, or Spout integration.

### Architectural Differences

- **MDropDX12 v1.4**: DirectX 12, x64, native ns-eel2 (x64 JIT), GDI overlay for text, no DX9 half-texel offset, no projection matrix (clip-space passthrough); ToolWindow system (Settings, Displays, Song Info, Hotkeys, MIDI run on own threads), configurable hotkeys with local/global scope and Launch App slots, native MIDI input (50 mapping slots with learn mode), tri-mode theme (Dark/Light/Follow System with WM_SETTINGCHANGE auto-detection), native webcam/video file capture via Media Foundation, Spout input mixing via D3D11On12, monitor mirroring, game controller support, idle timer, window title regex parsing
- **Milkwave v3.5**: Bundles a modified MilkDrop2 (DX9Ex) visualizer with a separate .NET 8 Remote control app; adds input mixing, game controller support, MIDI automation, projectM-eval expression engine
- **MilkDrop3 v3.31**: DirectX 9Ex, x86, native ns-eel2 (x86), adds .milk2 format, MilkPanel shader editor, deep mashup system, expanded variable ranges, shader caching (v3.31), hi-res audio (v3.31), 5 sprite layers with blend modes
- **projectM v4.1.6**: OpenGL, x64, cross-platform (Windows/macOS/Linux), projectM-eval expression engine, GLSL shader pipeline, SDL2 audio; reimplements MilkDrop2 rendering from scratch without using original MilkDrop2 code

### Feature Overlap

Many features listed under Milkwave (text messages, MIDI, media integration, wave manipulation, scripts) are **Remote-side features** that send commands to the Visualizer via IPC. Once MDropDX12 supports the WM_COPYDATA protocol, these features would become available through Milkwave Remote without needing to be reimplemented in MDropDX12 itself.

### 🤝 Features Available via Milkwave Remote 3.5+

Features marked 🤝 are not built into MDropDX12 directly but will work when used with Milkwave Remote 3.5 or later. The Remote provides its own UI for these features and sends commands to MDropDX12 via the WM_COPYDATA IPC protocol.

### ♻️ Features Implemented via IPC (Milkwave Remote)

Features marked ♻️ are implemented through the WM_COPYDATA IPC protocol and believed to be working with Milkwave Remote. MDropDX12 runs a dedicated hidden IPC window on a separate thread that receives commands non-blockingly from Milkwave Remote (or any WM_COPYDATA sender). The window title is configurable in Settings → Remote tab.

**Supported IPC commands** (32 of 34 Milkwave commands handled):

- `MSG|` — Full text message system (text, font, size, position, color, fade, burn-in, background box, animation)
- `AMP|` — Audio amplification (left/right channels)
- `PRESET=` — Load preset by path/filename
- `WAVE|` — Live wave parameter manipulation
- `DEVICE=` — Audio device switching (input/output)
- `OPACITY=` — Window transparency
- `STATE` — State query (reports opacity, preset, settings back to Remote)
- `LINK=` — Remote preset link toggle
- `QUICKSAVE` — Save current preset to Quicksave folder
- `CONFIG` / `SETTINGS` — Reload configuration
- `VAR_TIME=` / `VAR_FRAME=` / `VAR_FPS=` — Time/frame/FPS factors
- `VAR_INTENSITY=` / `VAR_SHIFT=` / `VAR_VERSION=` — Visual parameters
- `COL_HUE=` / `HUE_AUTO=` / `HUE_AUTO_SECONDS=` — Hue shifting + auto cycling
- `COL_SATURATION=` / `COL_BRIGHTNESS=` — Color adjustments
- `VAR_QUALITY=` / `VAR_AUTO=` — Render quality control
- `SPOUT_ACTIVE=` / `SPOUT_FIXEDSIZE=` / `SPOUT_RESOLUTION=` — Spout output control
- `CAPTURE` — Screenshot capture
- `CLEARPRESET` / `CLEARSPRITES` / `CLEARTEXTS` / `TESTFONTS` — Utility commands
- `SPOUTINPUT=` — Spout input mixing (sender, enable/disable, layer, opacity, luma key)
