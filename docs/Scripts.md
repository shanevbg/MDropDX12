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

### Audio

| Command | Description |
|---------|-------------|
| `AMP\|l=N\|r=N` | Set audio amplification for left and right channels (float) |
| `DEVICE=name` | Switch audio device (loopback output) |
| `DEVICE=IN\|name` | Switch to input device (microphone) |
| `DEVICE=OUT\|name` | Switch to output device (loopback) |
| `FFT_ATTACK=N` | FFT attack smoothing (0.0-1.0) |
| `FFT_DECAY=N` | FFT decay smoothing (0.0-1.0) |

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

These commands provide headless GLSL→HLSL shader import and conversion via the pipe. All are bidirectional — the visualizer sends a response back through the pipe. The `ShaderImportWindow` is created lazily if needed and does not need to be open.

| Command | Response | Description |
|---------|----------|-------------|
| `SHADER_IMPORT=<path>` | `SHADER_IMPORT_RESULT=OK\|...` or `ERROR\|...` | Load a `shader_import` JSON file, convert all passes GLSL→HLSL, and apply to the engine |
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

**SHADER_CONVERT** is the same as `SHADER_GLSL` but does not apply the shader — it only converts and returns the HLSL. Useful for inspecting the GLSL→HLSL conversion output.

**SHADER_SAVE** saves whatever was last imported/converted. Use `.milk3` extension for the JSON Shadertoy format or `.milk` for legacy MilkDrop format. Requires a prior `SHADER_IMPORT`, `SHADER_GLSL`, or `SHADER_CONVERT` call to populate the shader passes.

### Fallback

Any command not recognized by the IPC handler is passed to the script engine (`ExecuteScriptLine`). This means all script commands (NEXT, PREV, LOCK, RAND, ACTION=, LAUNCH=, etc.) work seamlessly over IPC.
