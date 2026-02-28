# Feature Comparison: MDropDX12 vs Milkwave vs MilkDrop3

Comparison of three MilkDrop-based music visualizer projects.

| Project | Description | Graphics API | Status |
| ------- | ----------- | ------------ | ------ |
| **MDropDX12** | Ground-up DX12 rebuild of MilkDrop2 engine | DirectX 12 | Active (v1.0-dev) |
| **Milkwave** | Remote control app + bundled MilkDrop2 visualizer | DirectX 9Ex | Active (v3.5-dev) |
| **MilkDrop3** | Enhanced MilkDrop2 fork (reference visualizer) | DirectX 9Ex | Active |

## Rendering

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| DirectX 12 rendering | ✅ | ❌ (DX9Ex) | ❌ (DX9Ex) |
| x64 build | ✅ | ❌ (x86) | ❌ (x86) |
| Fullscreen | ✅ | ✅ | ✅ |
| Borderless windowed | ✅ | ✅ | ✅ |
| Window transparency | ✅ | ✅ | ✅ |
| Clickthrough / watermark mode | ✅ | ✅ | ❌ |
| Always on top | ✅ | ✅ | ✅ |
| Adjustable render quality | ✅ | ✅ | ✅ |
| Auto quality (window-size adaptive) | ✅ | ✅ | ❌ |
| Fixed window dimensions (config) | ✅ | ✅ | ❌ |
| VSync toggle | ✅ | ✅ | ✅ |
| Black mode (hide rendering) | ✅ | ✅ | ❌ |

## Audio

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| WASAPI loopback capture | ✅ | ✅ | ✅ |
| Input device support (microphones) | ✅ | ✅ | ❌ |
| On-the-fly device selection | ✅ | ✅ | ❌ |
| Hi-res audio support | ✅ | ✅ | ❌ |
| Smooth audio variables (bass/mid/treb/vol_smooth) | ✅ | ✅ | ❌ |
| Signal amplification | ✅ | ✅ | ❌ |
| Disable audio capture option | ✅ | ✅ | ❌ |

## Presets

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| .milk preset loading | ✅ | ✅ | ✅ |
| .milk2 double-preset format | ❌ | ❌ | ✅ |
| Preset browser (in-app) | ✅ | ✅ | ✅ |
| Preset browser filtering (Ctrl+F) | ✅ | ✅ | ❌ |
| Preset tagging | ✅ | ✅ | ❌ |
| Tag-based playlists | ✅ | ✅ | ❌ |
| Age filter (modified within X days) | ✅ | ✅ | ❌ |
| Text filter (presets containing text) | ✅ | ✅ | ❌ |
| Quicksave (Ctrl+S) | ✅ | ✅ | ✅ |
| Quicksave2 folder (Ctrl+Shift+S) | ✅ | ✅ | ❌ |
| Preset auto-change timer | ✅ | ✅ | ✅ |
| Preset change on track change | ✅ | ✅ | ❌ |
| Preset mode buttons (assign/recall) | ❌ | ✅ | ❌ |
| Deep mashup (multi-layer) | ❌ | ❌ | ✅ |

## Preset Transitions

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Soft blend transitions | ✅ | ✅ | ✅ |
| Hard cut (H key) | ✅ | ✅ | ✅ |
| Beat-driven hard cuts | ✅ | ✅ | ✅ |
| 19+ blend patterns (.milk2) | ❌ | ❌ | ✅ |
| Async shader compilation (non-blocking) | ✅ | ❌ | ❌ |
| Compilation timeout with auto-skip | ✅ | ❌ | ❌ |
| Force soft transition type (Mixtype) | ✅ | ✅ | ❌ |

## Shaders

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Warp/comp HLSL shaders | ✅ | ✅ | ✅ |
| GLSL-to-HLSL conversion (live) | ✅ | ✅ | ❌ |
| Shader editor tab | ❌ | ✅ | ❌ |
| MilkPanel shader editor | ❌ | ❌ | ✅ |
| Shader precompiling and caching | ✅ | ✅ | ❌ |
| HLSL variable shadowing fix | ✅ | ❌ | ❌ |
| PSVersion=4 (AMD GPU support) | ✅ | ✅ | ❌ |
| Shader randomize (!/@ keys) | ✅ | ✅ | ✅ |

## Textures

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Disk texture loading | ✅ | ✅ | ✅ |
| Content Base Path (textures, sprites) | ✅ | ❌ | ❌ |
| Fallback texture search paths | ✅ | ❌ | ❌ |
| Random Textures Directory | ✅ | ❌ | ❌ |
| Fallback to 1x1 white texture | ✅ | ❌ | ❌ |
| 3D volume textures (noisevol) | ✅ | ✅ | ✅ |
| Resource viewer | ✅ | ❌ | ❌ |

## Display & HUD

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| GDI overlay HUD | ✅ | ❌ | ❌ |
| Preset name display | ✅ | ✅ | ✅ |
| FPS display | ✅ | ✅ | ✅ |
| Help screen | ✅ | ✅ | ✅ |
| Notifications | ✅ | ✅ | ✅ |
| CPU/GPU monitoring | ❌ | ✅ | ❌ |
| Save screenshot (Ctrl+X) | ♻️ | ✅ | ❌ |

## Text & Messages

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Independent messages/sprites toggles | ✅ | ❌ | ❌ |
| Message overrides (size, font, color, effects, animation) | ✅ | ✅ | ❌ |
| Custom text messages (from Remote) | ♻️ | ✅ | ❌ |
| Font/color customization | ♻️ | ✅ | ❌ |
| Multiple simultaneous messages | ♻️ | ✅ | ❌ |
| Message positioning (start/end coords) | ♻️ | ✅ | ❌ |
| Background box for text | ♻️ | ✅ | ❌ |
| Fade-out / burn-in timing | ♻️ | ✅ | ❌ |
| Script file commands | ❌ | ✅ | ❌ |

## Color & Visual Effects

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Hue/Saturation/Brightness shifting | ✅ | ✅ | ❌ |
| Auto hue cycling | ♻️ | ✅ | ❌ |
| Brighten/darken/solarize/invert | ✅ | ✅ | ✅ |
| Custom preset vars (vis_intensity, vis_shift, vis_version) | ✅ | ✅ | ❌ |
| colshift_hue preset variable | ✅ | ✅ | ❌ |
| Screen-dependent render mode | ✅ | ✅ | ❌ |
| Color/monochrome display toggle | ❌ | ✅ | ❌ |

## Sprites

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Static sprites | ♻️ | ✅ | ✅ |
| Animated sprites | ♻️ | ✅ | ✅ |
| Sprite selection from Remote | ♻️ | ✅ | ❌ |

## Shapes & Waves

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Custom shapes (per_frame/per_point) | ✅ | ✅ | ✅ |
| Custom waves (per_frame/per_point) | ✅ | ✅ | ✅ |
| Live wave manipulation | ♻️ | ✅ | ❌ |
| Mouse interaction mode (Ctrl+M) | ❌ | ✅ | ❌ |
| Expanded variable ranges | ❌ | ❌ | ✅ |

## MIDI

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| MIDI automation (up to 50 controls) | ❌ | ✅ | ❌ |
| MIDI tab in Remote | ❌ | ✅ | ❌ |

## Media Integration

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Now Playing track info | ❌ | ✅ | ❌ |
| Album artwork display | ❌ | ✅ | ❌ |
| Song info (Spotify/YouTube/media) | ❌ | ✅ | ❌ |
| Windows Media Play/Pause/Stop keys | ❌ | ✅ | ❌ |

## Remote Control

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Milkwave Remote compatibility | ♻️ | ✅ (built-in) | ❌ |
| WM_COPYDATA IPC protocol | ♻️ | ✅ | ❌ |
| Tabbed Remote UI | ❌ | ✅ | ❌ |
| Button panel (Remote buttons) | ♻️ | ✅ | ❌ |

## Spout

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| Spout texture output | ✅ | ✅ | ❌ |
| Window-independent Spout resolution | ✅ | ✅ | ❌ |

## GPU Protection

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| TDR recovery (device lost handling) | ✅ | ❌ | ❌ |
| Focus restoration after TDC rebuild | ✅ | ❌ | ❌ |
| Device restart from settings UI | ✅ | ❌ | ❌ |
| Async compilation (prevents GPU stall) | ✅ | ❌ | ❌ |
| Shader compile timeout | ✅ | ❌ | ❌ |

## Settings & Configuration

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| In-app settings window (F2) | ✅ | ✅ | ✅ |
| Dark theme settings UI | ✅ | ❌ | ❌ |
| Multi-tab settings | ✅ | ✅ | ✅ |
| Verbose logging (LogLevel=2) | ✅ | ✅ | ❌ |

## Expression Evaluation

| Feature | MDropDX12 | Milkwave | MilkDrop3 |
| ------- | --------- | -------- | --------- |
| ns-eel2 (native, x64 JIT) | ✅ | ❌ | ✅ |
| projectM-eval (via ns-eel2 shim) | ❌ | ✅ | ❌ |

---

## Notes

### Milkwave Remote Compatibility

Milkwave Remote finds the Visualizer window using `EnumWindows()` + `GetWindowText()` to match the window title. It communicates via `WM_COPYDATA` with Unicode pipe-delimited `key=value` messages. MDropDX12 could potentially receive commands from Milkwave Remote by matching the expected window title format — this is a planned integration path.

### .milk2 Double-Preset Format

MilkDrop3 supports `.milk2` files which contain two presets blended together with 19+ blend patterns. Neither MDropDX12 nor Milkwave currently support this format. Adding .milk2 support to MDropDX12 is a planned feature.

### Architectural Differences

- **MDropDX12**: DirectX 12, x64, GDI overlay for text, no DX9 half-texel offset, no projection matrix (clip-space passthrough)
- **Milkwave**: Bundles a modified MilkDrop2 (DX9Ex) visualizer with a separate WinForms Remote control app
- **MilkDrop3**: DirectX 9Ex, x86, adds .milk2 format, MilkPanel shader editor, deep mashup system, expanded variable ranges

### Feature Overlap

Many features listed under Milkwave (text messages, MIDI, media integration, wave manipulation, scripts) are **Remote-side features** that send commands to the Visualizer via IPC. Once MDropDX12 supports the WM_COPYDATA protocol, these features would become available through Milkwave Remote without needing to be reimplemented in MDropDX12 itself.

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

**Not yet handled**: `VIDEOINPUT=`, `SPOUTINPUT=`
