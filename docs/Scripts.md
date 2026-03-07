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
- **The Script tab** in Settings shows the current line, BPM, and beats. You can also adjust BPM/beats from the UI while a script is running.
- **Loop mode**: Enable looping in the Script tab to have the script restart from the beginning after the last line.
- Scripts loaded via Milkwave Remote use the same command format.
