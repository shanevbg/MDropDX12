# MDropDX12 Scripting Guide

MDropDX12 includes a built-in scripting engine for automating preset sequences, displaying messages, adjusting visual parameters, and triggering actions on a beat-driven timer.

## Quick Start

1. Open Settings (F8) and go to the **Script** tab.
2. Click **Load** and choose a `.txt` script file (or use the included `script-default.txt`).
3. Click **Play** to start the script. Lines execute on a BPM-driven timer.
4. Click **Stop** to halt execution at any time.

## File Format

Script files are plain text (`.txt`) with one command line per line.

```
# Lines starting with # are comments (ignored)
# Blank lines act as beat delays (the timer advances but nothing happens)

BPM=120|BEATS=8
PRESET=resources\presets\MDropDX12\01 - Martin - blue haze.milk|Welcome!
NEXT
NEXT|COLOR=255,0,0|Red text on next preset

STOP
```

### Syntax Rules

- **Comments**: Lines starting with `#` are skipped entirely.
- **Pipe separator**: Use `|` to put multiple commands on one line. All commands on a line execute simultaneously.
- **Blank lines**: Count as beat delays — the timer advances one interval but nothing executes.
- **Bare text**: Any text that isn't a recognized command is displayed as a message using the current default font, size, and color.
- **Case-insensitive**: Command names like `NEXT`, `next`, and `Next` all work.

### Timing

The script advances one line per timer interval. The interval is calculated as:

```
interval = (60 / BPM) * BEATS seconds
```

With the defaults (`BPM=120`, `BEATS=8`), each line lasts 4 seconds. Change these at any point in the script to speed up or slow down.

## Command Reference

### Sequencing

| Command | Description |
|---------|-------------|
| `NEXT` | Soft cut to next preset (like pressing Space) |
| `PREV` | Go back to previous preset (like pressing Backspace) |
| `STOP` | Stop the script |
| `RESET` | Reset the beat timer (re-sync timing) |
| `BPM=120` | Set beats per minute |
| `BEATS=8` | Set beats per line (how many beats before advancing) |
| `LINE=5` | Jump to line 5 and execute it (0-based, comments excluded) |
| `LINE=CURR` | Re-execute the current line |
| `LINE=NEXT` | Jump to next line and execute it |
| `LINE=PREV` | Jump to previous line and execute it |
| `FILE=another.txt` | Load and start a different script file (relative paths are relative to the current script's directory) |

### Preset Control

| Command | Description |
|---------|-------------|
| `PRESET=path\to\preset.milk` | Load a specific preset. Path can be absolute or relative to the MDropDX12 base directory |
| `LOCK` | Toggle preset lock on/off |
| `RAND` | Toggle random/sequential preset order |
| `PRESETINFO` | Toggle preset info display |
| `FULLSCREEN` | Toggle fullscreen mode |
| `MIRROR` | Toggle multi-monitor mirror mode |
| `STRETCH` | Toggle multi-monitor stretch mode |
| `MIRROR_WM` / `MIRROR_WATERMARK` | Toggle mirror watermark mode |
| `WATERMARK` | Toggle single-window watermark mode |
| `MIRROR_INDEPENDENT` / `INDEPENDENT` | Toggle independent per-display render |
| `ALWAYS_ON_TOP` / `AOT` | Toggle always-on-top |

### Shader Overrides and Preset Identity

See `docs/custom_shaders.md`.

| Command | Description |
|---------|-------------|
| `SHADER_OVERRIDE_APPLY=<name>` | Apply a shader override to the running preset (no rule needed) |
| `SHADER_OVERRIDE_REVERT` | Restore the preset's own shaders |
| `SHADER_OVERRIDE_ENABLE=0\|1` | Master enable for tag-selected overrides |
| `SHADER_OVERRIDE_STATUS` | What resolved, from which rule/tag, and any compile failure |
| `SHADER_OVERRIDE_RELOAD` | Re-read shaderoverrides.json and every shader file |
| `RELOAD_ANNOTATIONS` | Re-read presets.json (tags decide which override applies) |
| `DIAG_ANNOT_DUPES` | Hash every preset under the current preset dir; replies `groups`, `redundant`, `hashes` |
| `DIAG_ANNOT_DUPES=<root>` | Same, over any folder tree |
| `DIAG_ANNOT_IGNORED=<path>` | Would a preset there be recorded? Replies `ignored`, `dirmatch`, `testing` |
| `GET_PRESET_HASH` | Content identity of the running preset |
| `GET_PRESET_HASH=<path>` | Content identity of any preset file |
| `SET_PRESET_RATING=<0-5>` | Rate the running preset (written to presets.json, never to the .milk) |
| `GET_PRESET_RATING` | Effective rating, and whether it came from this install or the file's fRating |

### Messages

Any unrecognized text on a line is displayed as a message overlay using the current default style:

```
Hello World!
This text appears as a message overlay
Use // for a line break//like this
```

For full control over message parameters, use `MSG=`:

```
MSG=text=Hello;size=30;time=5.0;growth=1.5
```

MSG parameters (separated by `;`):

| Parameter | Description | Default |
|-----------|-------------|---------|
| `text=...` | Message text (`//` for newlines) | (required) |
| `font=Arial` | Font face name | Current default |
| `size=20` | Font size in points | Current default |
| `color_r=255` | Red (0-255) | Current default |
| `color_g=255` | Green (0-255) | Current default |
| `color_b=255` | Blue (0-255) | Current default |
| `time=3.0` | Display duration in seconds | 3.0 |
| `growth=1.0` | Text growth rate | 1.0 |
| `x=0.5` | Horizontal position (0.0=left, 1.0=right) | 0.5 |
| `y=0.5` | Vertical position (0.0=top, 1.0=bottom) | 0.5 |
| `startx=0.5` | Starting X for animation | Same as x |
| `starty=0.5` | Starting Y for animation | Same as y |
| `movetime=0.5` | Time to animate from start to final position | 0.5 |
| `burntime=2.0` | Fade-out duration | 2.0 |
| `easemode=0` | Easing: 0=linear, 1=ease-in | 0 |
| `shadowoffset=2` | Drop shadow pixel offset (0=no shadow) | 2 |

### Message Style Defaults

These commands set defaults for subsequent bare-text messages:

| Command | Description |
|---------|-------------|
| `FONT=Arial` | Set default font face |
| `SIZE=20` | Set default font size |
| `COLOR=255,0,0` | Set default color (R,G,B) |
| `CLEARPARAMS` | Reset font, size, and color to defaults (Arial, 20, white) |

### Sprites and Messages

| Command | Description |
|---------|-------------|
| `CLEARSPRITES` | Remove all active sprites |
| `CLEARTEXTS` | Remove all active text messages |
| `SEND=00` | Send a string to the visualizer window (e.g., trigger sprite 00) |
| `SEND=0x73` | Send a virtual key code (hex) to the visualizer (e.g., 0x73 = F4) |

### Visual Parameters

These adjust real-time rendering parameters:

| Command | Description | Range |
|---------|-------------|-------|
| `TIME=1.2` | Time speed factor | 0.0+ |
| `FRAME=1.5` | Frame factor | 0.0+ |
| `FPS=0.1` | FPS factor | 0.0+ |
| `INTENSITY=1.2` | Visual intensity | 0.0+ |
| `SHIFT=-0.2` | Visual shift | any |
| `VERSION=2` | Vis version override | integer |
| `QUALITY=0.5` | Render quality (0.5 = half resolution) | 0.01-1.0 |
| `HUE=-0.8` | Hue shift | -1.0 to 1.0 |
| `SATURATION=0.77` | Saturation shift | -1.0 to 1.0 |
| `BRIGHTNESS=0.1` | Brightness shift | -1.0 to 1.0 |

### Media Control

| Command | Description |
|---------|-------------|
| `MEDIA_PLAY` | Press the system Play/Pause media key |
| `MEDIA_STOP` | Press the system Stop media key |

### Actions (Hotkey Dispatch)

`ACTION=TagName` triggers any built-in hotkey action by its tag name. This is the most powerful scripting command — it gives scripts access to everything the keyboard can do.

```
ACTION=OpenSettings
ACTION=NextPreset
ACTION=ApplyWorkspaceLayout
```

#### Navigation

| Tag | Description |
|-----|-------------|
| `NextPreset` | Next preset (soft cut) |
| `PrevPreset` | Previous preset |
| `HardCut` | Hard cut to next preset |
| `RandomMashup` | Random mini-mashup |
| `LockPreset` | Toggle preset lock |
| `ToggleRandom` | Toggle random/sequential |
| `OpenPresetList` | Open preset browser |
| `SavePreset` | Save preset as... |
| `OpenMenu` | Toggle preset-editing menu |

#### Visual Parameters

| Tag | Description |
|-----|-------------|
| `OpacityUp` / `OpacityDown` | Window opacity +/- |
| `Opacity25` / `Opacity50` / `Opacity75` / `Opacity100` | Set opacity to fixed value |
| `WaveModeNext` / `WaveModePrev` | Cycle wave mode |
| `WaveAlphaUp` / `WaveAlphaDown` | Wave alpha +/- |
| `WaveScaleUp` / `WaveScaleDown` | Wave scale +/- |
| `ZoomIn` / `ZoomOut` | Zoom +/- |
| `WarpAmtUp` / `WarpAmtDown` | Warp amount +/- |
| `WarpScaleUp` / `WarpScaleDown` | Warp scale +/- |
| `EchoAlphaUp` / `EchoAlphaDown` | Echo alpha +/- |
| `EchoZoomUp` / `EchoZoomDown` | Echo zoom +/- |
| `EchoOrient` | Cycle echo orientation |
| `GammaUp` / `GammaDown` | Gamma +/- |
| `BrightnessUp` / `BrightnessDown` | Brightness +/- |
| `PushXPos` / `PushXNeg` | Push X +/- |
| `PushYPos` / `PushYNeg` | Push Y +/- |
| `RotateLeft` / `RotateRight` | Rotation +/- |
| `HueForward` / `HueBackward` | Hue shift +/- |

#### Media

| Tag | Description |
|-----|-------------|
| `MediaPlayPause` | Play/Pause |
| `MediaStop` | Stop |
| `MediaPrevTrack` / `MediaNextTrack` | Previous/Next track |
| `MediaRewind` / `MediaFastFwd` | Rewind/Fast forward |

#### Window

| Tag | Description |
|-----|-------------|
| `ToggleFullscreen` | Toggle fullscreen |
| `ToggleStretch` | Toggle multi-monitor stretch/mirror |
| `MirrorWatermark` | Toggle mirror watermark mode (all displays, low opacity, click-through) |
| `AlwaysOnTop` | Toggle always on top |
| `TransparencyMode` | Toggle transparency (F12) |
| `BlackMode` | Toggle black mode |
| `FPSCycle` | Cycle FPS limit |
| `ShowPresetInfo` | Toggle preset info |
| `ShowFPS` | Toggle FPS display |
| `ShowRating` | Toggle rating display |
| `ShowShaderHelp` | Toggle shader help |

#### Tool Windows

| Tag | Description |
|-----|-------------|
| `OpenSettings` | Open Settings window |
| `OpenDisplays` | Open Displays window |
| `OpenSongInfo` | Open Song Info window |
| `OpenHotkeys` | Open Hotkeys window |
| `OpenMidi` | Open MIDI window |
| `OpenBoard` | Open Button Board |
| `OpenPresets` | Open Presets window |
| `OpenSprites` | Open Sprites window |
| `OpenMessages` | Open Messages window |
| `OpenShaderImport` | Open Shader Import window |
| `OpenVideoFX` | Open Video Effects window |
| `OpenVFXProfiles` | Open VFX Profile Picker |
| `OpenWorkspaceLayout` | Open Workspace Layout window |
| `ApplyWorkspaceLayout` | Apply saved workspace layout (opens, tiles, and positions all selected windows) |

#### Shader / Effects

| Tag | Description |
|-----|-------------|
| `InjectEffectCycle` | Cycle inject effect (off/brighten/darken/solarize/invert) |
| `HardcutModeCycle` | Cycle hard cut mode |
| `QualityDown` / `QualityUp` | Halve/double render quality |
| `SpoutToggle` | Toggle Spout output |
| `SpoutFixedSize` | Set Spout to fixed resolution |
| `Screenshot` | Save screenshot |
| `ShaderLockCycle` | Cycle shader lock (comp/warp/both/none) |
| `SongTitle` | Trigger song title animation |
| `KillSprites` | Kill all sprites |
| `KillSupertexts` | Kill all text overlays |
| `AutoPresetChange` | Toggle auto-preset-change on track change |
| `ScrambleWarp` | Randomize warp shader |
| `ScrambleComp` | Randomize comp shader |
| `Quicksave` | Quicksave current preset |
| `ScrollLock` | Toggle scroll lock (lock + playlist) |
| `ReloadMessages` | Reload messages.ini |

#### Misc

| Tag | Description |
|-----|-------------|
| `DebugInfo` | Toggle debug info overlay |
| `SpriteMode` | Toggle sprite/message input mode |

### Launch External Application

```
LAUNCH=C:\Tools\MyApp.exe
LAUNCH=notepad.exe
```

Opens (or focuses) an external application. If the application is already running, it brings it to the foreground.

## Examples

### Basic Preset Sequence

```
# Cycle through presets every 8 beats at 120 BPM
BPM=120|BEATS=8
PRESET=resources\presets\MDropDX12\01 - Martin - blue haze.milk
NEXT
NEXT
NEXT
STOP
```

### Message Showcase

```
# Display styled messages
BPM=120|BEATS=4
COLOR=255,255,0|SIZE=30|Welcome to the show!
COLOR=0,255,255|A cyan message
MSG=text=Custom positioned;size=40;x=0.2;y=0.3;time=5.0;growth=1.5
CLEARPARAMS|Back to defaults
STOP
```

### Animated Text Entrance

```
# Text slides in from the left
BPM=120|BEATS=4
MSG=text=Sliding In;size=30;startx=0;starty=0.5;x=0.5;y=0.5;movetime=1.0;easemode=1;time=4.0
```

### Fast Preset Switching

```
# Rapid-fire presets at 180 BPM, 2 beats each
BPM=180|BEATS=2
NEXT
NEXT
NEXT
NEXT|BPM=120|BEATS=8|Slowing down now
NEXT
STOP
```

### Visual Parameter Automation

```
# Shift colors and adjust parameters over time
BPM=120|BEATS=4
HUE=0.3|SATURATION=0.5
HUE=0.6|BRIGHTNESS=0.2
HUE=-0.5|SATURATION=-0.3
HUE=0|SATURATION=0|BRIGHTNESS=0
STOP
```

### Open Workspace Layout with a Single Hotkey

You can apply your saved workspace layout (tiling tool windows across the screen with a render preview in a corner) using a single hotkey. There are two ways to set this up:

#### Method 1: Bind a Hotkey Directly

1. Open the Hotkeys window (Ctrl+F7).
2. Find **"Apply Workspace Layout"** in the Tools category.
3. Click its key column and press your desired key combination (e.g., Ctrl+Shift+W).
4. The hotkey now opens and applies your saved workspace layout instantly.

#### Method 2: Use a Script Hotkey

1. Open the Hotkeys window (Ctrl+F7).
2. Add a new **Script** entry at the bottom of the list.
3. Set the command to: `ACTION=ApplyWorkspaceLayout`
4. Bind it to your preferred key.

You can combine it with other actions on a single script line:

```
# Open workspace layout and lock the current preset
ACTION=ApplyWorkspaceLayout|LOCK
```

### Chaining Script Files

```
# main.txt — play intro, then switch to loop
BPM=120|BEATS=8
Welcome to the show!
NEXT

FILE=loop.txt
```

```
# loop.txt — loops forever
BPM=140|BEATS=4
NEXT
NEXT
NEXT
NEXT
```

### Triggering Sprites

```
# Trigger sprite 05, wait, then clear
BPM=120|BEATS=4
SEND=05
SEND=05

CLEARSPRITES
STOP
```

## Tips

- **Use RESET** after changing BPM or BEATS to avoid timing drift.
- **Blank lines are delays**, not no-ops. Each blank line consumes one beat interval.
- **Relative paths** in `FILE=` and `PRESET=` are relative to the current script's directory (for FILE) or the MDropDX12 base directory (for PRESET).
- **The Script window** shows the current line, BPM, and beats. You can also adjust BPM/beats from the UI while a script is running.
- **Loop mode**: Enable looping in the Script window to have the script restart from the beginning after the last line.
- Scripts loaded via Milkwave Remote use the same command format.

## IPC Commands (Named Pipe)

MDropDX12 listens for commands on a Named Pipe at `\\.\pipe\Milkwave_<PID>` where `<PID>` is the visualizer's process ID. All script commands listed above also work over IPC. The commands below are additional IPC-only commands not available in script files.

### Protocol

- **Pipe name**: `\\.\pipe\Milkwave_<PID>` (discover via `CreateToolhelp32Snapshot` or the MCP server)
- **Message encoding**: UTF-16LE null-terminated wide strings
- **Field delimiter**: `|` separates fields within a command
- **Duplex**: The pipe supports bidirectional communication (e.g., `STATE` returns data)

### Signal Commands

Signal commands use the `SIGNAL|` prefix and are processed directly by the pipe server. They trigger immediate actions without going through the message queue.

| Command | Description |
|---------|-------------|
| `SIGNAL\|NEXT_PRESET` | Switch to next preset (soft cut) |
| `SIGNAL\|PREV_PRESET` | Switch to previous preset |
| `SIGNAL\|CAPTURE` | Capture screenshot |
| `SIGNAL\|SHOW_COVER` | Display album cover art sprite |
| `SIGNAL\|COVER_CHANGED` | Notify that album cover image has changed |
| `SIGNAL\|SPRITE_MODE` | Switch to sprite input mode |
| `SIGNAL\|MESSAGE_MODE` | Switch to message input mode |
| `SIGNAL\|SETVIDEODEVICE=N` | Set video input device (N = device index) |
| `SIGNAL\|ENABLEVIDEOMIX=0\|1` | Enable (1) or disable (0) video input mixing |
| `SIGNAL\|ENABLESPOUTMIX=0\|1` | Enable (1) or disable (0) Spout input mixing |
| `SIGNAL\|SET_INPUTMIX_OPACITY=N` | Set input mix opacity (0-100) |
| `SIGNAL\|SET_INPUTMIX_LUMAKEY=threshold\|softness` | Set luma key (0-255 each, or -1 to disable) |
| `SIGNAL\|SET_INPUTMIX_ONTOP=0\|1` | Set input mix layer order (1 = overlay, 0 = background) |
| `SIGNAL\|FULLSCREEN` | Toggle fullscreen (deactivates mirrors first if active) |
| `SIGNAL\|WATERMARK` | Toggle single-window watermark mode (borderless FS + click-through + low opacity) |
| `SIGNAL\|BORDERLESS_FS` | Toggle borderless fullscreen |
| `SIGNAL\|STRETCH` | Toggle multi-monitor stretch |
| `SIGNAL\|MIRROR` | Toggle mirror mode (fullscreen + mirrors on all displays) |
| `SIGNAL\|MIRROR_WM` | Toggle mirror watermark mode (all displays, low opacity, click-through) |
| `SIGNAL\|MIRROR_INDEPENDENT` | Toggle independent per-display mirror render (correct aspect vs stretch-copy) |
| `SIGNAL\|ALWAYS_ON_TOP` | Toggle always-on-top for primary + live mirrors |

### Mirror / Display Control

| Command | Response | Description |
|---------|----------|-------------|
| `DIAG_MIRRORS` | `MIRRORS\|…` | Rich mirror dump. Global: `main=WxH,mainPort,needSrv,canSample,anyOpp,anyIndepMilk3,shadertoy,compPso,slots,frame,skipFrames,allocHr,listHr,auxUsed,orient=WxH,orientReady,orientFrames,orientFb`. Per `monN`: enabled/opacity/clickthru/independent/skipped/portrait, swapsize/bufs, layered/visible, **path** (`orient`/`stretchMain`/`letterbox`/`copy`/`black`/`orientFail`), drawFI/presFI/presHr, ok/fail/skip/draws, mustBlock/oppIndep/wipe/paintMask, bb0/bb1/bb2 states. Use when debugging portrait landscape ghost / every-other-frame. |
| `SET_MIRROR_WIPE` | `MIRROR_WIPE=1` | Re-arm full flip-chain wipe (clears paint masks). `SET_MIRROR_WIPE=reinit` also force-recreates mirror swap chains. |
| `SET_MIRROR_OPACITY=<1-100>` | `MIRROR_OPACITY=<N>` | Set opacity for all monitor mirrors (or `N,val` for DISPLAY N) |
| `SET_MIRROR_CLICKTHRU=<0\|1>` | `MIRROR_CLICKTHRU=<N>` | Set click-through for all monitor mirrors |
| `SET_MIRROR_INDEPENDENT=<0\|1>` | `MIRROR_INDEPENDENT=<N>` | Set independent render (global default + all monitors). Optional `N,val` for DISPLAY N only |
| `GET_MIRROR_INDEPENDENT` | `MIRROR_INDEPENDENT=<N>` | Query independent render state |
| `MIRROR_INDEPENDENT` | `MIRROR_INDEPENDENT=<N>` | Toggle independent render (same as signal / hotkey) |
| `SET_MIRROR_ENABLED=<0\|1>` | `MIRROR_ENABLED=<N>` | Enable/disable all non-primary monitor outputs (or `N,val` for DISPLAY N) |
| `SET_ALWAYS_ON_TOP=<0\|1>` | `ALWAYS_ON_TOP=<N>` | Set always-on-top (primary + mirrors) |
| `GET_ALWAYS_ON_TOP` | `ALWAYS_ON_TOP=<N>` | Query always-on-top state |
| `ALWAYS_ON_TOP` | `ALWAYS_ON_TOP=<N>` | Toggle always-on-top |
| `MOVE_TO_DISPLAY=<N>` | `MOVED_TO=<device>` | Move render window to center of display N (1-based index into monitor list) |
| `SET_WINDOW=<x>,<y>,<w>,<h>` | `WINDOW=(<x>,<y>)-(<x2>,<y2>) <w>x<h>` | Set render window position and size. Use w=0,h=0 to move without resizing |

### Audio

| Command | Description |
|---------|-------------|
| `AMP\|l=N\|r=N` | Set audio amplification for left and right channels (float) |
| `DEVICE=name` | Switch audio device (loopback output) |
| `DEVICE=IN\|name` | Switch to input device (microphone) |
| `DEVICE=OUT\|name` | Switch to output device (loopback) |
| `FFT_ATTACK=N` | FFT attack smoothing (0.0-1.0) |
| `FFT_DECAY=N` | FFT decay smoothing (0.0-1.0) |
| `SET_DEVICE_VOLUME=N` | Set Windows device volume (0.0-1.0). Responds with `DEVICE_VOLUME=N\|muted=0\|1` |
| `GET_DEVICE_VOLUME` | Query current device volume and mute state. Responds with `DEVICE_VOLUME=N\|muted=0\|1` |
| `SET_DEVICE_MUTE=0\|1` | Mute (1) or unmute (0) the audio device. Responds with `DEVICE_MUTE=0\|1` |
| `TOGGLE_DEVICE_MUTE` | Toggle device mute state. Responds with `DEVICE_MUTE=0\|1` |

### Visual Parameters

| Command | Description | Range |
|---------|-------------|-------|
| `VAR_TIME=N` | Time speed factor | 0.0+ |
| `VAR_FRAME=N` | Frame factor | 0.0+ |
| `VAR_FPS=N` | FPS factor | 0.0+ |
| `VAR_INTENSITY=N` | Visual intensity multiplier | 0.0+ |
| `VAR_SHIFT=N` | Visual shift value | any float |
| `VAR_VERSION=N` | Vis version override | integer |
| `VAR_QUALITY=N` | Render quality scale | 0.01-1.0 |
| `VAR_AUTO=0\|1` | Auto quality adjustment | 0 or 1 |

### Color

| Command | Description | Range |
|---------|-------------|-------|
| `COL_HUE=N` | Hue shift | -1.0 to 1.0 |
| `COL_SATURATION=N` | Saturation adjustment | -1.0 to 1.0 |
| `COL_BRIGHTNESS=N` | Brightness adjustment | -1.0 to 1.0 |
| `HUE_AUTO=0\|1` | Enable auto hue cycling | 0 or 1 |
| `HUE_AUTO_SECONDS=N` | Auto hue cycle period (seconds) | float |

### Video Effects (IPC)

Every parameter the Video Effects window exposes, addressable by name. Names
match the `[VideoFX]` INI keys exactly. Values are clamped to each parameter's
range and the response reports what was stored.

| Command | Response | Description |
|---------|----------|-------------|
| `GET_VFX` | `VFX=<Name>=<value>\|…` | Dump all parameters |
| `GET_VFX_STATUS` | `VFX_STATUS=profile=<name>\|dirty=<0\|1>` | Which profile is loaded, and whether it has unsaved changes |
| `SET_VFX=<Name>=<value>` | `VFX_SET=<Name>=<value>` | Set one parameter. Unknown name returns `VFX_SET=ERROR\|unknown\|<Name>` |
| `VFX_RESET=<group>` | `VFX_RESET=<group>` | Reset `transform`, `effects`, `audio` or `all` to defaults |
| `VFX_PROFILE_LIST` | `VFX_PROFILES=<name>\|…` | List saved profile names |
| `VFX_PROFILE_SAVE=<name>` | `VFX_PROFILE_SAVED=<name>` | Save current parameters under that name, replacing it if it exists |
| `VFX_PROFILE_LOAD=<name>` | `VFX_PROFILE_LOADED=<name>` | Load a profile. Unknown name returns `VFX_PROFILE_LOADED=ERROR\|<name>` |
| `VFX_PROFILE_IMPORT=<path>[\|<name>]` | `VFX_PROFILE_IMPORTED=<count>` | Import from a file this build did not write (see below). `<name>` names a single unnamed set |

The three reset groups are exactly the window's three Reset buttons, which means
`blendMode` counts as **transform** — its combo box sits on the Transform tab.

**Parameters**

| Group | Names | Range |
|-------|-------|-------|
| Transform | `PosX` `PosY` | -1.0 to 1.0 |
| | `Scale` | 0.1 to 5.0 |
| | `Rotation` | 0 to 360 |
| | `MirrorH` `MirrorV` | 0 or 1 |
| | `BlendMode` | 0=Alpha 1=Additive 2=Multiply 3=Screen 4=Overlay 5=Difference |
| Effects | `TintR` `TintG` `TintB` | 0.0 to 2.0 |
| | `Brightness` | -1.0 to 1.0 |
| | `Contrast` `Saturation` | 0.0 to 3.0 |
| | `HueShift` | 0 to 360 |
| | `Invert` `EdgeDetect` | 0 or 1 |
| | `Pixelation` | 0.0 to 1.0 |
| | `Chromatic` | 0.0 to 0.05 |
| Audio | `AR_<Param>_Source` | 0=none 1=bass 2=mid 3=treb 4=vol |
| | `AR_<Param>_Intensity` | 0.0 to 2.0 |

`<Param>` for the audio links is one of `PosX` `PosY` `Scale` `Rotation`
`Brightness` `Saturation` `Chromatic` — e.g. `SET_VFX=AR_Scale_Source=1`.

**Where these values live**

The parameters are stored in JSON, not in `settings.ini`. Two kinds of file:

| File | Written | Role |
|---|---|---|
| `videofx/current.json` | on every change | Live state. Loaded at startup, so a value set over the pipe survives a restart. |
| `videofx/<name>.json` | only on an explicit save | A named snapshot. Never changes on its own. |

`[VideoFX] MovedToJson=1` in `settings.ini` records that the move has happened.
While it is set, **every key in that section is ignored** except the ones naming
which JSON to use — `szCurrentVFXProfile`, `szVFXStartup` and their two flags,
which have to stay in the INI because they say *which* file to read. The
parameter copies are read exactly once, to migrate an install coming from an
older build, and are then left in place only so that older build can still read
them.

Profiles used to carry a `rendering` object of render tunables — the MD31 glow
and rib widths. Those knobs are gone, so a profile is video effects only. A
`rendering` object written by an older build is left untouched rather than
stripped, so downgrading does not lose it.

### Where profiles live

Every profile is a named entry in a single file, `resources/vfxprofiles.json`:

```json
{ "version": 1, "profiles": { "Warm": { "transform": {…}, "color": {…}, … } } }
```

A profile is a **name**, not a path — there is no file per profile to point at.
Any `videofx/<name>.json` files from an older build are folded into the store
on first run; the folder is read, never deleted.

### Importing older files

**Import...** on the VFX Profiles window, or `VFX_PROFILE_IMPORT`, reads
settings written by builds that stored them differently. Files are identified
by content, not by name, so a renamed or hand-edited one still comes in:

| Shape | Becomes |
|---|---|
| `settings.ini` with `[VideoFX]` parameter keys | one profile, named by you |
| `videofx/<name>.json`, `current.json` — one profile per file | one profile, suggested name from the filename |
| a whole `vfxprofiles.json` from another install | every profile in it, keeping their names |
| any of the above wrapped in a `"videoFX"` object | as above |

Imported names never overwrite what you already have — a clash is numbered
`Name (2)`. The exception is a single unnamed set imported under a name you
chose, which replaces that name after asking.

> **One trap this handles for you.** `[VideoFX]` is *also* the Video Effects
> tool window's INI section, and window position is saved there as `PosX` /
> `PosY` — the same key names the position parameters used. An old
> `settings.ini` can therefore hold a window coordinate where a `-1..1`
> position belongs. When the section also carries window geometry the importer
> ignores those two keys and logs a warning. Everything imported is clamped to
> the range its slider offers.

---

> **Nothing is written unless you save, and nothing is restored unless you ask.**
> `SET_VFX` and `VFX_RESET` change live state and write nothing
> at all. Only `VFX_PROFILE_SAVE` writes. At startup the parameters begin at
> their defaults unless **Load on startup** names a profile — there is no
> live-state file, so effects do not follow you across a restart by themselves.
> While live values differ from the loaded profile, `GET_VFX_STATUS` reports
> `dirty=1` and the Save Profile button on the Video Effects window turns red
> and reads `Save Profile *`.
>
> Earlier builds auto-saved the current profile from every code path that
> touched settings — not just on close — so a saved profile silently tracked
> every later edit and was never a snapshot. That is fixed; if you relied on
> the old behaviour, save explicitly.

### Spout Output

| Command | Description |
|---------|-------------|
| `SPOUT_ACTIVE=0\|1` | Enable (1) or disable (0) Spout output |
| `SPOUT_FIXEDSIZE=0\|1` | Toggle fixed Spout output resolution |
| `SPOUT_RESOLUTION=WxH` | Set Spout output resolution (e.g., `1920x1080`) |

### Spout Input

| Command | Description |
|---------|-------------|
| `SPOUTINPUT=0\|1\|senderName` | Enable/disable Spout input from a specific sender |
| `SPOUT_SENDER=name` | Set Spout input sender name |

### Wave Parameters

`WAVE|` sets live wave rendering parameters. All fields are optional and pipe-delimited:

```
WAVE|COLORR=255|COLORG=128|COLORB=0|ALPHA=0.8|MODE=2|PUSHX=0.1|PUSHY=-0.1|ZOOM=1.5|WARP=0.5|ROTATION=0.3|DECAY=0.98|SCALE=1.2|ECHO=1.0|BRIGHTEN=1|DARKEN=0|SOLARIZE=0|INVERT=0|ADDITIVE=1|DOTTED=0|THICK=1|VOLALPHA=0
```

| Field | Description | Type |
|-------|-------------|------|
| `COLORR`, `COLORG`, `COLORB` | Wave color (0-255) | int |
| `ALPHA` | Wave alpha | float |
| `MODE` | Wave mode index | int |
| `PUSHX`, `PUSHY` | X/Y push offset | float |
| `ZOOM` | Zoom level | float |
| `WARP` | Warp amount | float |
| `ROTATION` | Rotation amount | float |
| `DECAY` | Decay factor | float |
| `SCALE` | Wave scale | float |
| `ECHO` | Video echo zoom | float |
| `BRIGHTEN` | Brighten effect | 0 or 1 |
| `DARKEN` | Darken effect | 0 or 1 |
| `SOLARIZE` | Solarize effect | 0 or 1 |
| `INVERT` | Invert effect | 0 or 1 |
| `ADDITIVE` | Additive wave blending | 0 or 1 |
| `DOTTED` | Dotted wave style | 0 or 1 |
| `THICK` | Thick wave lines | 0 or 1 |
| `VOLALPHA` | Modulate alpha by volume | 0 or 1 |

### Track Info (IPC)

| Command | Description |
|---------|-------------|
| `TRACK\|artist=X\|title=Y\|album=Z` | Update displayed track info (from Milkwave Remote) |

**Outgoing**: When the current track changes, MDropDX12 broadcasts a `TRACK|` message to all connected pipe clients:

```text
TRACK|artist=Artist Name|title=Song Title|album=Album Name|artwork=C:\path\to\cover.jpg
```

This allows external tools to receive track info updates in real time.

### Window (IPC)

| Command | Description |
|---------|-------------|
| `OPACITY=N` | Set window opacity (0.0-1.0) |

### Messages (IPC)

The `MSG|` command sends a styled text overlay. Fields are pipe-delimited key=value pairs:

```
MSG|text=Hello World|font=Segoe UI|size=30|r=255|g=255|b=255|time=5|x=0.5|y=0.5
```

See the [Messages](#messages) section above for the full list of MSG parameters (text, font, size, r, g, b, time, growth, x, y, startx, starty, movetime, easemode, easefactor, shadowoffset, burntime, fade, fadeout, bold, ital, randx, randy, randr, randg, randb, box_alpha, box_col, box_left, box_right, box_top, box_bottom, profile).

### State and Configuration

| Command | Description |
|---------|-------------|
| `STATE` | Query current state (returns opacity, preset info, settings via pipe response) |
| `CONFIG` | Reload configuration and rebuild fonts |
| `SETTINGS` | Reload timing settings from INI |
| `TESTFONTS` | Display font/animation test messages (debug) |
| `QUICKSAVE` | Save current preset to Quicksave folder |
| `CAPTURE` | Capture screenshot to file |
| `CLEARPRESET` | Clear current preset state |
| `CLEARSPRITES` | Remove all active sprites |
| `CLEARTEXTS` | Remove all active text overlays |
| `LINK=N` | Link Remote to preset index N |

### Preset Lists and Directory (IPC)

These commands manage saved preset lists and the active preset directory.

| Command | Response | Description |
|---------|----------|-------------|
| `LOAD_LIST=<name>` | `LOAD_LIST_RESULT=OK\|<name>` or `ERROR\|...` | Load a saved preset list by name (without `.txt` extension) |
| `CLEAR_LIST` | `CLEAR_LIST_RESULT=OK` | Clear the active preset list and revert to directory scanning |
| `ENUM_LISTS` | `ENUM_LISTS_RESULT=name1\|name2\|...` | Enumerate all available saved preset lists |
| `SET_DIR=<path>` | `SET_DIR_RESULT=OK\|<path>` or `ERROR\|...` | Change the preset directory to `<path>` |
| `SET_DIR=<path>\|recursive` | `SET_DIR_RESULT=OK\|<path>` or `ERROR\|...` | Change directory with recursive subdirectory scanning enabled |

**Examples:**

```text
ENUM_LISTS
LOAD_LIST=favorites
SET_DIR=C:\MyPresets\collection1|recursive
CLEAR_LIST
```

### Shader Import (IPC)

These commands provide headless GLSL->HLSL shader import and conversion via the pipe. All are bidirectional — the visualizer sends a response back through the pipe. The `ShaderImportWindow` is created lazily if needed and does not need to be open.

| Command | Response | Description |
|---------|----------|-------------|
| `SHADER_IMPORT=<path>` | `SHADER_IMPORT_RESULT=OK\|...` or `ERROR\|...` | Load a `shader_import` JSON file, convert all passes GLSL->HLSL, and apply to the engine |
| `SHADER_GLSL=<glsl>` | `SHADER_GLSL_RESULT=OK\|...` or `ERROR\|...` | Send raw GLSL source for a single Image pass, convert and apply |
| `SHADER_CONVERT=<glsl>` | `SHADER_CONVERT_RESULT=OK\|<hlsl>` or `ERROR\|...` | Convert GLSL to HLSL without applying — returns the HLSL output |
| `SHADER_SAVE=<path>` | `SHADER_SAVE_RESULT=OK\|...` or `ERROR\|...` | Save the current shader passes as a `.milk3` or `.milk` preset file |

**SHADER_IMPORT** loads a `shader_import` format JSON file (the same format used by the Shader Import window's Load/Save Project feature). The JSON must have `"type": "shader_import"` and a `"passes"` array. Example:

```json
{
  "type": "shader_import",
  "version": 1,
  "passes": [
    {
      "name": "Image",
      "glsl": "void mainImage(out vec4 o, in vec2 fc) { o = vec4(fc/iResolution.xy, 0, 1); }",
      "channels": { "ch0": 0, "ch1": 0, "ch2": 1, "ch3": 2 }
    }
  ]
}
```

**SHADER_GLSL** takes inline GLSL source (UTF-16LE encoded like all pipe messages). Useful for quick single-pass shader testing. Channel assignments are auto-detected from the GLSL source.

**SHADER_CONVERT** is the same as `SHADER_GLSL` but does not apply the shader — it only converts and returns the HLSL. Useful for inspecting the GLSL->HLSL conversion output.

**SHADER_SAVE** saves whatever was last imported/converted. Use `.milk3` extension for the JSON Shadertoy format or `.milk` for legacy MilkDrop format. Requires a prior `SHADER_IMPORT`, `SHADER_GLSL`, or `SHADER_CONVERT` call to populate the shader passes.

### Fallback

Any command not recognized by the IPC handler is passed to the script engine (`ExecuteScriptLine`). This means all script commands (NEXT, PREV, LOCK, RAND, ACTION=, LAUNCH=, etc.) work seamlessly over IPC.
