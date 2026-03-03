# MDropDX12 Manual

MDropDX12 is a DirectX 12 music visualizer based on the MilkDrop2 engine. It renders real-time visual effects driven by audio input, using HLSL shaders and a per-frame expression evaluation system. MDropDX12 is a standalone x64 application for Windows.

## Getting Started

Run `MDropDX12.exe`. The visualizer starts in windowed mode, capturing system audio via WASAPI loopback. Play music through any application and visuals will respond automatically.

Press **F1** for the built-in help overlay (press again for page 2, again to close).

Press **F8** or **Ctrl+L** to open the Settings window.

## System Requirements

- Windows 11 64-bit or higher
- DirectX 12 compatible GPU

## Window Modes

| Mode | Activation | Description |
|------|-----------|-------------|
| Normal windowed | Default | Resizable window with title bar |
| Fullscreen | ALT+ENTER or double-click | Covers full monitor, cursor hidden |
| Borderless | F2 or right-click double-click | No title bar or borders |
| Windowed fullscreen | CTRL+F9 | Covers work area excluding taskbar |
| Watermark mode | CTRL+SHIFT+F9 | Borderless fullscreen + clickthrough + reduced opacity |
| Clickthrough | F9 | Mouse events pass through to windows below |
| Transparency | F12 | Black pixels become transparent |
| Black mode | CTRL+F12 | No preset rendering (black screen) |
| Always on top | F7 | Window stays above other windows |
| Multi-monitor stretch | ALT+S | Stretches across all monitors (default) |
| Mirror mode | ALT+S | Fullscreens primary + activates mirrors (when enabled in Displays tab) |

**Opacity**: SHIFT+UP/DOWN or SHIFT+Mousewheel adjusts window opacity.

**Reset window**: CTRL+F2 resets window size and position. CTRL+SHIFT+F2 sets window to fixed dimensions from config.

## Keyboard Shortcuts

### General

| Key | Action |
|-----|--------|
| F1 | Help overlay (page 1 / page 2 / off) |
| F2 | Toggle borderless window |
| CTRL+F2 | Disable all display outputs (kill switch) |
| F3 | Cycle FPS cap: 30/60/90/120/144/240/360/720/Unlimited |
| F4 | Show/hide preset name |
| F5 | Show/hide FPS counter |
| F6 | Show/hide preset rating |
| F7 | Toggle always on top |
| F8 or CTRL+L | Open Settings window |
| CTRL+F7 | Open Hotkeys window |
| CTRL+F8 | Open Displays window |
| SHIFT+CTRL+F8 | Open Song Info window |
| F9 | Toggle clickthrough mode |
| F10 or CTRL+Z | Toggle Spout output |
| SHIFT+F10 or SHIFT+CTRL+Z | Toggle fixed Spout resolution |
| F11 | Cycle effect: Off / Brighten / Darken / Solarize / Invert |
| SHIFT+F11 | Cycle hard cut mode |
| F12 | Toggle transparency mode |
| CTRL+F12 | Toggle black mode |
| ALT+ENTER | Toggle fullscreen |
| ALT+S | Toggle multi-monitor stretch (or mirror mode if enabled) |
| CTRL+F3 | Display current FPS setting |
| CTRL+F9 | Toggle windowed fullscreen |
| CTRL+SHIFT+F9 | Toggle watermark mode |
| SCROLL LOCK | Lock/unlock preset + toggle playlist |

### Presets

| Key | Action |
|-----|--------|
| SPACE | Soft cut to next preset |
| H | Hard cut to next preset |
| BACKSPACE | Go back to previous preset |
| `` ` `` or `~` | Lock/unlock current preset |
| R | Toggle random/sequential order |
| L | Open preset browser |
| M | Show/hide preset-editing menu |
| N | Show per-frame monitor |
| S | Save new preset |
| CTRL+S | Quicksave to /presets/Quicksave |
| SHIFT+CTRL+S | Quicksave to /presets/Quicksave2 |
| Mousewheel | Scroll through presets |
| Drag-and-drop .milk file | Load preset |

### Audio and Quality

| Key | Action |
|-----|--------|
| CTRL+D | Set audio device to default |
| SHIFT+CTRL+D | Display current audio device |
| CTRL+Q | Double quality factor |
| SHIFT+CTRL+Q | Half quality factor |
| CTRL+H | Shift hue |
| CTRL+A | Toggle auto preset change on track change |
| CTRL+B | Toggle track info polling |
| CTRL+C | Toggle cover art display on track change |

### Media Controls

| Key | Action |
|-----|--------|
| LEFT ARROW | Previous track |
| RIGHT ARROW or V | Next track |
| DOWN ARROW or X | Play/pause |
| UP ARROW or C | Stop |
| CTRL+LEFT | Fast rewind |
| CTRL+RIGHT | Fast forward |
| B or Middle Mouse | Show track info |

### Messages and Sprites

| Key | Action |
|-----|--------|
| K | Toggle sprite/message mode |
| SHIFT+K | Enter sprite kill mode |
| 00-99 | Invoke sprite or message by number |
| * | Clear digit entry, reload messages.ini |
| DEL | Clear latest sprite/message |
| SHIFT+DEL | Clear oldest sprite/message |
| SHIFT+CTRL+DEL | Clear all |
| CTRL+K | Kill all sprites |
| CTRL+T | Kill all messages and song titles |
| CTRL+X | Save screenshot |

### Visual Tweaking

| Key | Action |
|-----|--------|
| Q / q | Wave echo zoom +/- |
| W / w | Wave mode +/- |
| E / e | Wave alpha +/- |
| U / u | Wave warp scale +/- |
| I / i | Wave zoom +/- |
| O / o | Wave warp amount +/- |
| P / p | Video echo alpha +/- |
| + / - | Brightness +/- |
| G / g | Gamma +/- |
| J / j | Wave scale +/- |
| [ / ] | X push +/- |
| { / } | Y push +/- |
| < / > | Rotation +/- |
| A | Random mini mash-up |
| D | Cycle shader lock states |
| F | Toggle echo orientation |
| ! | Randomize warp shader |
| @ | Randomize comp shader |

## Settings Window (F8)

Press **F8** or **Ctrl+L** to open the Settings window. It provides an 11-tab interface with tri-mode theme support (Dark / Light / Follow System). The pin icon in the top-right corner toggles always-on-top for the Settings window (on by default). Settings, Displays, Song Info, and Hotkeys windows are standalone ToolWindows that run on their own threads and remember their positions and active tabs between sessions.

### General Tab

- **Preset Directory**: Set the folder containing .milk and .milk2 presets
- **Preset Browser**: Navigate and load presets from the list with forward/back navigation buttons
- **Preset Filter**: Click the filter button (right side of nav row) to cycle through All / .milk / .milk2. Random and sequential preset selection respects the active filter
- **Audio Sensitivity**: -1 for auto-adaptive, or manual value (0.5-256)
- **Blend Time**: Duration of soft transitions between presets (seconds)
- **Time Between Presets**: Auto-advance interval (seconds)
- **Hard Cuts Disabled**: Prevent audio-triggered hard cuts
- **Lock Preset on Startup**: Start with preset locked
- **Sequential Preset Order**: Play presets in order instead of random
- **Messages/Sprites Mode**: Off / Messages / Sprites / Messages and Sprites
- **Song Info Source**: Select how track info is obtained — SMTC (Windows media sessions), IPC (from Milkwave Remote), or Window Title (regex-based parsing)
- **Profile / Edit**: When source is Window Title, select a named profile and open the Artist-Title Match Editor to configure window matching and parsing regex patterns (see Track Info section below)
- **Song Title Animations**: Animated display when track changes
- **Overlay Notifications**: Show track info as an overlay notification on track change
- **Show Cover Art**: Display album artwork on track change
- **Always Show Track Info**: Keep track info visible permanently instead of fading out
- **Display Corner**: Choose which screen corner shows track info (Top-Left, Top-Right, Bottom-Left, Bottom-Right)
- **Display Seconds**: How long track info remains visible (0.5-60 seconds)
- **Show Now**: Force-display current track info immediately
- **Change Preset with Song**: Auto-advance when a new track starts
- **Show FPS / Always on Top / Borderless**: Toggle checkboxes
- **Theme**: Select Dark, Light, or Follow System. Follow System auto-detects your Windows theme and switches when you change it in Windows Settings > Personalization > Colors
- **Font +/- buttons**: Adjust HUD overlay font size
- **Resources button**: Opens the Resource Viewer showing all textures loaded by the current preset with their load status, type, dimensions, and file paths

### Visual Tab

- **Opacity**: Window transparency (0-100%)
- **Render Quality**: Internal render buffer scale (0-100%). A low quality yields a pixellated retro look. Quality setting is ignored when fixed Spout resolution is used.
- **Auto Quality**: Automatically adjusts quality to maintain similar perceived quality on different window sizes
- **Time/Frame/FPS Factor**: Change internal time, FPS, and frame counters sent to presets. This may speed up, slow down, or otherwise change preset behavior depending on how the preset uses these variables.
- **Vis Intensity / Shift / Version**: Custom preset variable overrides (see Preset Variables below)
- **GPU Protection**: Max shape instances, skip heavy presets, heavy threshold
- **VSync / FPS Cap**: Control frame rate
- **Reload Preset / Restart Render**: Utility buttons

### Colors Tab

- **Hue**: Shift output hue (-1.0 to +1.0)
- **Saturation / Brightness**: Adjust output color
- **Gamma**: Gamma correction (0-8.0) with auto-reset for low-gamma presets
- **Auto Hue**: Automatically cycle hue over a configurable period
- **Reset**: Restore color defaults

### System Tab

- **Audio Device**: Select from system output and input devices. Input devices appear with `[Input]` suffix.
- **Hotkeys...**: Opens the Hotkeys window (also available via Ctrl+F7). See the Configurable Hotkeys section below.
- **Idle Timer**: Screensaver mode that activates after a configurable idle timeout (1-60 minutes). Action can be Fullscreen or Stretch/Mirror. Auto-restore returns to previous state when input is detected.
- **Game Controller**: Map game controller buttons to visualizer commands. Check **Enable** to start polling. Select a controller from the **Device** dropdown, click **Scan** to re-enumerate connected controllers. The **Button Mapping** text editor shows the JSON configuration mapping button numbers to commands. Click **Defaults** for the default DualSense mapping, **Save** to write to `controller.json`, **Load** to read it back. The **?** button shows an Xbox controller button reference. See Game Controller section below for details.

### Files Tab

- **Content Base Path**: Root directory for textures and sprites referenced by presets
- **Fallback Search Paths**: Additional directories to search when textures are missing. When a preset references a texture that is not found in the built-in directory, these paths are searched in order.
- **Random Textures Directory**: Separate directory specifically for random texture selection (rand00-rand13), which takes priority over fallback paths

### Messages Tab

- **Messages list**: Up to 100 custom message slots
- **Push Now / Add / Edit / Delete**: Manage messages
- **Reload from File / Paste / Open INI**: Utility functions
- **Overrides**: Global randomization and display overrides
- **Autoplay**: Timer-based automatic message cycling with interval and jitter
- **Sequential Order / Auto-Size**: Display options
- **Show Messages / Show Sprites**: Independent visibility toggles

### Sprites Tab

- **Sprites list**: Up to 100 sprite slots with image preview
- **Blend Mode**: Blend / Decal / Additive / SrcColor / ColorKey
- **Layer**: Behind Text / On Top
- **Position / Scale / Rotation**: Transform controls
- **Color (RGBA) / Colorkey**: Tint and transparency
- **Flip / Burn / Repeat**: Display flags
- **Init Code / Per-Frame Code**: EEL expression editors for animation

### Remote Tab

- **Window Title / Remote Title**: Configure titles for IPC discovery by Milkwave Remote
- **Apply and Restart IPC**: Restart the IPC server with new titles
- **Save Screenshot**: Save current frame with file dialog
- **Active IPC Windows**: Connected Remote clients
- **Last Message**: Most recent IPC message received

### Script Tab

- **Script File**: Browse for a BPM-timed preset sequence file
- **Play / Stop / Loop**: Playback controls
- **BPM / Beats**: Timing configuration
- **Script Lines**: List of script entries; double-click to jump to a line

### Displays Tab

The Displays tab is split into two sub-tabs: **Display Outputs** and **Video Input**.

#### Display Outputs Sub-Tab

- **Output List**: Shows all detected monitors and Spout senders with status (OFF / ON / ACTIVE)
- **Enable**: Toggle the selected output on or off
- **Fullscreen**: Toggle fullscreen mode for monitor mirrors
- **Add Spout**: Create a new Spout sender with a unique name
- **Remove**: Delete the selected Spout sender
- **Refresh**: Re-enumerate connected monitors
- **Activate Mirrors**: Activate or deactivate all enabled monitor mirrors. Mirrors are always inactive at startup for safety; press this button to create the mirror windows.
- **Click-through**: When checked, mouse events pass through mirror windows to applications behind them. Mirror windows are always topmost so they remain visible in click-through mode. Click-through is off by default at each launch (not persisted).
- **Opacity**: Mirror window opacity (1-100%). Applied to all active mirror windows in real time. Persisted to settings.ini.
- **Use mirrors for ALT-S**: When checked, ALT+S fullscreens the primary render window on its current monitor and activates display mirrors on all enabled outputs. ALT+S again deactivates mirrors and restores the primary window to its previous size and position. When unchecked, ALT+S uses the legacy stretch behavior (spanning one window across all monitors). If no mirrors are enabled but other displays are detected, a prompt asks whether to mirror to all displays. Persisted to settings.ini.
- **Don't ask when no mirrors are enabled**: When checked, ALT+S automatically enables all detected monitors for mirroring without showing the confirmation prompt. Useful for single-key activation workflows. Persisted to settings.ini.
- **Sender Name**: Name visible to Spout receivers (Spout outputs only)
- **Fixed Size**: Lock Spout output to a specific resolution (Spout outputs only)
- **Width / Height**: Fixed resolution dimensions (Spout outputs only)

#### Video Input Sub-Tab

- **Source**: Select video input source — **None**, **Spout** (receive from an external Spout sender), **Webcam** (capture from a connected camera via Media Foundation), or **Video File** (play an MP4 or other video file)
- **Layer**: Choose **Background** or **Overlay** compositing mode
- **Opacity**: Controls transparency of the video layer (0-100%)
- **Luma Key**: When enabled, dark areas of the video become transparent. Adjust **Threshold** and **Softness** controls for keying
- **Webcam Device**: Select a connected camera (with Refresh button)
- **Video File**: Browse for an MP4 or other video file (with Loop checkbox)
- **Spout Sender**: Select a Spout sender name (with Refresh button)

See the Display Outputs and Video Input sections below for details.

### About Tab

- Version and build information
- **Paths**: Base Dir, Settings INI, and Presets directory
- **Log Level**: Off / Error / Warn / Info / Verbose (writes to debug.log)
- **Register .milk / .milk2**: Associate .milk and .milk2 files with this exe so double-clicking them in Explorer opens the preset in MDropDX12 (writes to HKCU, no admin required)

## Audio

MDropDX12 captures audio via **WASAPI loopback**, which mirrors whatever is playing through the selected audio output device. No audio routing or virtual cable is needed. Play music through any application and the visualizer responds.

**Input devices** (microphones) are also supported. In Settings > Sound, input devices appear with an `[Input]` suffix.

**Audio Sensitivity** controls how strongly the visualizer reacts to audio levels. Set to **-1** for auto-adaptive mode, which automatically adjusts based on signal level. Manual values range from 0.5 (low sensitivity) to 256 (high sensitivity).

**Quick device switch**: Press **CTRL+D** to reset to the default audio device (useful after disconnecting Bluetooth headphones). Press **SHIFT+CTRL+D** to display the current device name.

## Presets

Presets are `.milk` or `.milk2` files that define visual behavior through per-frame equations, per-pixel equations, custom shapes, custom waves, and HLSL shaders. `.milk2` files contain two presets blended together. MDropDX12 is compatible with the MilkDrop2 preset ecosystem.

### Loading Presets

- Press **L** to open the preset browser
- Use UP/DOWN, PAGE UP/DOWN, HOME/END to navigate
- ENTER or SPACE to load the selected preset
- Type a letter to jump to presets starting with that letter
- Navigate into subdirectories; BACKSPACE to go up one level
- **Drag and drop** a .milk or .milk2 file from Explorer onto the window
- **Double-click** a .milk or .milk2 file in Explorer to load it directly (if MDropDX12 is already running, the preset is forwarded to the existing instance via IPC)
- **File association**: Use Settings → About → "Register .milk / .milk2" to enable double-click loading (no admin required)

### Preset Transitions

- **SPACE**: Soft blend to next preset (uses the blend time set in General tab)
- **H**: Hard cut to next preset (instant switch)
- **BACKSPACE**: Return to previously loaded preset
- Beat-driven hard cuts trigger automatically based on audio energy (configurable via SHIFT+F11)
- Force soft transition type using `Mixtype` in settings.ini

### Saving Presets

- **S**: Save a new preset (opens the preset-editing menu)
- **CTRL+S**: Quicksave current state to `resources/presets/Quicksave/`
- **SHIFT+CTRL+S**: Quicksave to `resources/presets/Quicksave2/`

### Preset Lock

Press **`` ` ``** or **`~`** (or SCROLL LOCK) to lock the current preset, preventing auto-advance.

## Track Info

MDropDX12 can display current track information (artist, title, album) and album artwork as an overlay. Track info is obtained from one of three sources, configured in Settings > General > Song Info Source.

### Sources

- **SMTC** (default): Windows System Media Transport Controls. Automatically detects track info from Spotify, YouTube (in browsers), Windows Media Player, and any app that reports to Windows media sessions. No configuration needed.
- **IPC**: Receives track info from Milkwave Remote via the WM_COPYDATA protocol.
- **Window Title**: Parses track info from any application's window title using configurable regex patterns. Useful for media players that don't report to SMTC (internet radio apps, niche players, etc.).

### Window Title Profiles

When the source is set to Window Title, MDropDX12 uses named profiles to match windows and extract track info. Each profile contains:

- **Profile Name**: A label for the profile (e.g., "Spotify", "RarmaRadio", "foobar2000")
- **Window Match regex**: A regular expression to find the target window via EnumWindows
- **Parse Regex**: A regex with named capture groups `(?<artist>...)`, `(?<title>...)`, and `(?<album>...)` to extract fields from the matched window title
- **Poll Interval**: How often to check the window title (1-10 seconds)

Multiple profiles can be created for different media players. Select the active profile from the dropdown on the General tab.

### Artist-Title Match Editor

Click **Edit** next to the profile dropdown to open the Artist-Title Match Editor. The editor provides:

- **Profile selector** with New/Delete buttons for managing multiple profiles
- **Windows dropdown**: Lists all visible windows on your system. Selecting a window auto-fills the Window Match field with an escaped regex pattern.
- **Window Match**: Regex to find the target window. Use `.*keyword.*` for broad matching.
- **Matched**: Shows the first window title that matches (live preview).
- **Parse Regex**: Regex with named capture groups to extract artist/title/album.
- **Parsed**: Shows the extracted fields (live preview).
- **Poll Interval**: Check interval in seconds (1-10).
- **Regex Help**: Opens MDN regex documentation in your browser.

Changes take effect when you click **OK**.

### Parse Regex Examples

The parse regex uses ECMAScript regex syntax with named capture groups: `(?<artist>...)`, `(?<title>...)`, `(?<album>...)`. Groups are matched by name and mapped to the corresponding track info field.

**RarmaRadio** — title format: `Di.fm Vocal Trance - Artist Name - Song Title - 2.77.7 - RarmaRadio`

```text
Window Match: .*RarmaRadio.*
Parse Regex:  ^.+? - (?<artist>.+?) - (?<title>.+?) - .+? - RarmaRadio
Result:       Artist: Artist Name | Title: Song Title
```

**Spotify** — title format: `Song Title - Artist Name - Spotify`

```text
Window Match: .*Spotify.*
Parse Regex:  ^(?<title>.+?) - (?<artist>.+?) - Spotify
Result:       Artist: Artist Name | Title: Song Title
```

**foobar2000** — title format: `Artist Name - Song Title [foobar2000]`

```text
Window Match: .*foobar2000.*
Parse Regex:  ^(?<artist>.+?) - (?<title>.+?) \[foobar2000\]
Result:       Artist: Artist Name | Title: Song Title
```

**VLC media player** — title format: `Song Title - VLC media player`

```text
Window Match: .*VLC media player.*
Parse Regex:  ^(?<title>.+?) - VLC media player
Result:       Title: Song Title
```

**AIMP** — title format: `Artist Name - Song Title - AIMP`

```text
Window Match: .*AIMP.*
Parse Regex:  ^(?<artist>.+?) - (?<title>.+?) - AIMP
Result:       Artist: Artist Name | Title: Song Title
```

**Winamp** — title format: `123. Artist Name - Song Title - Winamp`

```text
Window Match: .*Winamp.*
Parse Regex:  ^\d+\.\s+(?<artist>.+?) - (?<title>.+?) - Winamp
Result:       Artist: Artist Name | Title: Song Title
```

**YouTube in browser** — title format: `Song Title - YouTube - Google Chrome`

```text
Window Match: .*YouTube.*Chrome.*
Parse Regex:  ^(?<title>.+?) - YouTube
Result:       Title: Song Title
```

**Generic "Artist - Title" fallback** — works with any `Artist - Title` format:

```text
Window Match: .*YourPlayer.*
Parse Regex:  (?<artist>.+?) - (?<title>.+)
Result:       Artist: Artist Name | Title: Song Title
```

### Tips

- Use `.*keyword.*` for window matching — it's simpler and more reliable than escaping the full title.
- The Window Match regex is case-insensitive. The Parse Regex is case-sensitive.
- Named groups are optional. If you use plain groups like `(.+?) - (.+)`, group 1 maps to artist, group 2 to title, group 3 to album.
- Special regex characters (`\.`, `\(`, `\)`, `\[`, `\]`, `\+`, `\*`) need backslash escaping when matching literal text.
- The live preview in the editor updates as you type, so you can test patterns interactively.
- SMTC is recommended for most users since it works automatically with Spotify, YouTube, and modern media apps. Use Window Title profiles only for apps that don't report to Windows media sessions.

## Messages

The message system displays user-defined text overlays on screen. Messages are stored in `messages.ini` with up to 100 slots (00-99).

### Using Messages

1. Set mode to Messages (Settings > General > Messages/Sprites Mode, or press **K**)
2. Type a two-digit number (00-99) to display that message
3. Press ***** to reload messages.ini from disk

### Managing Messages

Use Settings > Messages tab to add, edit, and delete messages. Each message has:

- **Text**: The message content
- **Font / Size**: Font face and point size
- **Position X / Y**: Normalized coordinates (0.0-1.0)
- **Growth**: Text scale animation rate
- **Duration**: How long the message stays on screen (seconds)
- **Fade In / Fade Out**: Transition durations
- **Color**: RGB color values

### Per-Message Randomization

Each message can have individual randomize flags for position, size, font, color, effects, growth, and duration. When enabled, the corresponding property is randomized each time the message is displayed. Use **Randomize All** to check all flags at once.

### Send Now

The **Send Now** button in the edit dialog immediately displays the message on screen without closing the dialog, allowing quick preview and iteration. If you cancel after using Send Now, the original message values are restored.

### Autoplay

Enable **Autoplay Messages** in the Messages tab to automatically cycle through messages on a timer. Configure the interval (seconds between messages) and jitter (random variation). Choose sequential or random order.

### Fade-in, Fade-Out, and Burn Time

You can define default values for fade-in (`fade`, default 0.2), fade-out (`fadeout`, default 0.0) and burn time (`burntime`, default 0.1) in settings.ini, and override them per message as a parameter.

If a burntime > 0 is defined, the message will be "baked" into the background and slowly fade away for the defined duration. Note that this does not work with all presets. If you define a fadeout > 0, the burntime will be irrelevant because the message fades out before there is anything to "burn in".

### Global Overrides

The **Overrides** dialog applies global randomization rules to all messages:

- Random font, color, size, effects
- Size range (min/max)
- Maximum simultaneous messages on screen
- Random position, growth, slide-in animation, random duration
- Drop shadow, background box
- Hue shift and random hue
- **Ignore per-message randomization**: Disables all per-message random flags

## Sprites

Sprites are image overlays managed via Settings > Sprites tab. Each sprite has an image file, blend mode, layer order, position, scale, rotation, colorkey, RGBA tint, and animation code.

Sprites support **EEL expressions** in Init Code (runs once on load) and Per-Frame Code (runs every frame) for animation.

## Display Outputs

The Displays tab in Settings (F8) provides a unified interface for managing all render outputs: monitor mirrors and Spout senders.

### Monitor Mirroring

MDropDX12 can mirror its visualization output to additional monitors connected to your system. The Displays tab lists all detected monitors (excluding the one running the main visualizer window). Enable a monitor and press **Activate Mirrors** to create borderless fullscreen mirror windows on those displays.

**Safety**: Mirror windows are always inactive at startup. You must explicitly press the **Activate Mirrors** button on the Displays tab to create them. This prevents accidental full-screen coverage of monitors.

**ALT+S failsafe**: When "Use mirrors for ALT-S" is enabled and you press ALT+S with no monitors enabled, MDropDX12 detects available displays and prompts: "No mirrors enabled. Found N display(s). Mirror to all?" Choose **Yes** to enable all detected monitors and activate mirrors, **No** to go fullscreen without mirrors, or **Cancel** to abort. Check "Don't ask when no mirrors are enabled" on the Displays tab to skip the prompt and automatically enable all monitors.

**Z-order**: Mirror windows are always topmost so they stay above normal desktop windows. The Settings window (also topmost) sits above mirrors naturally when focused.

**Click-through**: By default, mirror windows are opaque and block mouse input. Check **Click-through** on the Displays tab to allow mouse events to pass through to applications behind the mirror. Click-through state is not persisted — it resets to off each launch.

**Opacity**: Use the **Opacity** spin box (1-100%) to control mirror window transparency. This is useful in combination with click-through to see and interact with windows behind the visualization. Opacity is persisted to settings.ini.

Monitors are enumerated automatically at startup and when displays are connected or disconnected. Use the "Refresh" button to manually re-scan.

### Multiple Spout Senders

You can configure multiple Spout senders from the Displays tab, each with its own name and optional fixed resolution. Use "Add Spout" to create additional senders beyond the default one. Each sender appears in Spout-compatible receivers (OBS, Resolume, etc.) as a separate source.

For each Spout output you can configure:

- **Sender Name**: The name visible to Spout receivers (must be unique)
- **Fixed Size**: Lock the Spout output to a specific resolution instead of matching the visualizer window size
- **Width / Height**: The fixed resolution dimensions (only used when Fixed Size is enabled)

### Display Output Kill Switch

Press **Ctrl+F2** to instantly disable all display outputs (monitors and Spout senders) and deactivate mirrors. This is useful if a mirror window covers your screen or you need to quickly free GPU resources. The main visualizer window is not affected.

## Spout Output

### What is Spout

[Spout](https://spout.zeal.co/) is a real-time video sharing framework for Windows that allows applications to share GPU textures. With Spout enabled, other applications on the same machine can receive the MDropDX12 visualizer output as a live video texture with minimal latency.

Common Spout receivers include OBS Studio, Resolume Arena/Avenue, TouchDesigner, VDMX, and Processing.

### How Spout Works in MDropDX12

MDropDX12 acts as a **Spout sender** with the name `"MDropDX12"`. After each frame completes the full rendering pipeline (warp pass, shape/wave injection, comp pass, and post-processing effects), the final composited frame is shared through Spout's texture sharing system.

The sharing mechanism works by creating a **shared GPU texture** that is registered in a system-wide shared memory namespace managed by Spout. Receiver applications discover available senders by name and read directly from the shared texture. Because the texture stays on the GPU throughout the entire sharing pipeline, there is no CPU-side pixel copying and latency is minimal (typically under one frame).

The flow for each frame is:

1. MDropDX12 renders the frame through the DX12 pipeline (warp, shapes/waves, comp, effects)
2. The final backbuffer contents are copied to a shared texture
3. Spout registers the texture with sender name `"MDropDX12"` and current dimensions
4. Any Spout receiver application can discover and read this texture in real-time
5. If the output resolution changes (window resize or fixed size change), the shared texture is automatically recreated at the new dimensions

### Enabling Spout

There are three ways to toggle Spout output:

- **Keyboard**: Press **F10** or **CTRL+Z**
- **Settings UI**: Check **Spout Output** in Settings > System tab
- **IPC command**: Send `SPOUT_ACTIVE=1` or `SPOUT_ACTIVE=0` via WM_COPYDATA

A notification appears on screen confirming the state change.

### Output Resolution

By default, the Spout output resolution **matches the visualizer window size**. When you resize the window, the Spout output resolution changes to match. This is convenient for quick use but means the receiver's resolution depends on the window size.

**Fixed resolution mode** decouples the Spout output from the window size. This is the recommended mode for production use, because it provides a consistent output resolution regardless of how the visualizer window is sized or positioned.

For example, you can send a full 1920x1080 stream to OBS while keeping the visualizer in a small corner preview window.

To enable fixed resolution:

- **Keyboard**: Press **SHIFT+F10** or **SHIFT+CTRL+Z** to toggle
- **Settings UI**: Check **Fixed Size** in Settings > System tab, then enter Width and Height
- **IPC command**: Send `SPOUT_FIXEDSIZE=1` and `SPOUT_RESOLUTION=1920x1080`

The supported resolution range is **64x64 to 7680x4320**.

When fixed resolution is active, the entire rendering pipeline operates at the fixed dimensions internally. This means text layout, mesh resolution, and all visual elements are rendered at native quality for the Spout output rather than being scaled. The render quality setting is ignored when fixed Spout resolution is used. The visualizer window displays a scaled preview of the actual output.

### Spout Settings Reference

| Setting | Location | Default | Description |
|---------|----------|---------|-------------|
| Spout Output | System tab / F10 / CTRL+Z | Enabled | Master on/off toggle |
| Fixed Size | System tab / SHIFT+F10 | Disabled | Decouple output from window size |
| Width | System tab | 1280 | Fixed output width (64-7680) |
| Height | System tab | 720 | Fixed output height (64-4320) |

### INI Configuration

Spout settings are stored in `settings.ini` under the `[Settings]` section:

```ini
bSpoutOut=1
bSpoutFixedSize=0
nSpoutFixedWidth=1280
nSpoutFixedHeight=720
```

### IPC Commands for Spout

When controlled via Milkwave Remote or other WM_COPYDATA senders:

| Command | Description |
|---------|-------------|
| `SPOUT_ACTIVE=1` | Enable Spout output |
| `SPOUT_ACTIVE=0` | Disable Spout output |
| `SPOUT_FIXEDSIZE=1` | Enable fixed resolution mode |
| `SPOUT_FIXEDSIZE=0` | Disable fixed resolution mode |
| `SPOUT_RESOLUTION=1920x1080` | Set fixed resolution (WxH format) |

### Using Spout with OBS Studio

1. Install the [OBS Spout2 Plugin](https://github.com/Off-World-Live/obs-spout2-plugin)
2. In OBS, add a new **Spout2 Capture** source
3. Select `"MDropDX12"` from the sender dropdown
4. The visualizer output appears in your OBS scene

For best results with streaming or recording, enable fixed Spout resolution at your desired output size (e.g., 1920x1080) so the resolution stays consistent regardless of how you size the visualizer window on your desktop.

### Using Spout with Other Applications

Any application that supports Spout receiving can capture the MDropDX12 output:

- **Resolume Arena/Avenue**: Add a Spout source and select MDropDX12
- **TouchDesigner**: Use a Spout In TOP
- **Processing**: Use the Spout library for Processing
- **VDMX**: Add a Spout source

### Troubleshooting Spout

**Receiver does not see MDropDX12**: Ensure Spout output is enabled (F10). Check that the receiver application supports Spout (not NDI or other protocols). Both applications must be on the same machine.

**Intermittent frames or flickering**: Lower the visualizer FPS cap to match your receiver's frame rate. If using fixed resolution, ensure the resolution is reasonable for your GPU.

**High GPU usage with fixed resolution**: Large fixed resolutions (e.g., 3840x2160) require more GPU work since the entire pipeline renders at that resolution. Reduce the fixed resolution or lower render quality if needed.

## Video Input

MDropDX12 can composite a video source onto the visualization as a background or overlay layer. This is configured on the **Displays** tab in Settings (F8).

### Source Types

| Source | Description |
|--------|-------------|
| None | Video input disabled |
| Spout | Receive frames from an external Spout sender (e.g., OBS, Resolume) |
| Webcam | Capture from a connected camera via Windows Media Foundation |
| Video File | Play an MP4 or other video file |

### Layer Modes

- **Background**: Video is drawn onto the render target before the warp pass, so the preset's warp distortion applies to the video. The visualization renders on top.
- **Overlay**: Video is drawn onto the backbuffer after the comp pass, so it appears on top of the visualization.

### Shared Controls

All source types share the same compositing controls:

- **Opacity**: Controls the transparency of the video layer (0-100%)
- **Luma Key**: When enabled, dark areas of the video become transparent based on luminance. Use **Threshold** to set the cutoff point and **Softness** to control the transition gradient.

### Webcam

Select a webcam from the device dropdown. Use **Refresh** to re-enumerate devices if you connect a camera after opening Settings. The webcam captures at its native resolution using Media Foundation and delivers BGRA frames to the GPU.

### Video File

Click **Browse** to select a video file. Most formats supported by Windows Media Foundation work (MP4, AVI, WMV, MKV with appropriate codecs). Check **Loop** to repeat the video continuously.

### Spout

Select a Spout sender from the dropdown or leave it on Auto to connect to the first available sender. Use **Refresh** to update the sender list. This requires an external application sending frames via Spout (e.g., OBS with the Spout filter, Resolume, or any Spout-compatible sender).

### Settings Persistence

Video input settings are saved to the `[SpoutInput]` section of `settings.ini`. The `Source` key stores the active source type (0=None, 1=Spout, 2=Webcam, 3=File). All sources restore automatically on restart.

## Color Effects

### Hue / Saturation / Brightness

Adjust the overall color of the visualizer output via Settings > Colors tab. The hue shift rotates all colors around the color wheel. These can also be controlled via the Milkwave Remote IPC protocol.

### Auto Hue Cycling

Enable **Auto Hue** to continuously rotate the hue over a configurable period (in seconds). This creates a slowly shifting color palette effect.

### Inject Effects (F11)

Cycle through post-processing effects applied after the comp shader:

- **Off**: No effect
- **Brighten**: Amplifies all colors
- **Darken**: Reduces all colors
- **Solarize**: Inverts mid-range tones
- **Invert**: Inverts all colors

## Screenshots

- **CTRL+X**: Auto-saves a PNG screenshot to the `capture/` subdirectory with filename format `YYYYMMDD-HHMMSS-presetname.png`
- **Settings > Remote tab > Save Screenshot**: Opens a file dialog to choose save location
- **IPC**: The `CAPTURE` command triggers a screenshot from Milkwave Remote

## Preset Variables

MDropDX12 introduces additional variables for preset authors. These are designed to be backward compatible with other MilkDrop-based visualizers when wrapped in `#ifdef` guards.

### Smooth Audio Variables

`bass_smooth`, `mid_smooth`, `treb_smooth`, `vol_smooth`

These provide much more smoothed versions than the standard `*_att` variables, meaning the value change is much more subtle between each frame. For backward compatibility with other MilkDrop-based visualizers:

```hlsl
#ifndef bass_smooth
#define bass_smooth bass_att
#endif
```

Or wrap your code in a conditional block:

```hlsl
#ifdef bass_smooth
float3 myColor = float3(sin(bass_smooth)+1, 0, 0);
#endif
```

### Intensity, Shift, and Version

`vis_intensity` (float, default 1.0), `vis_shift` (float, default 0.0), `vis_version` (int, default 1)

Presets can use these variables to modify appearance aspects. Users can adjust these values live from Settings > Visual tab while the preset is running. They can also be assigned to MIDI controls via Milkwave Remote.

For backward compatibility:

```hlsl
#ifndef vis_intensity
#define vis_intensity 1
#endif
```

See the Shader presets included with MDropDX12 for examples that make use of these parameters.

### Color Shift Variables

`colshift_hue`, `colshift_saturation`, `colshift_brightness`

These all default to 0 and can be changed in Settings > Colors tab or via Milkwave Remote. The values are used in the `shiftHSV()` function which is included in MDropDX12's `include.fx` file.

To save a color shift value to a preset, add something like this at the end of your comp shader definition:

```hlsl
#ifdef colshift_saturation
  colshift_saturation += 0.5;
#endif
```

Note that this is an MDropDX12-specific change that does not port to other MilkDrop variants since they use a different `include.fx`. For a portable version, include your own `shiftHSV()` implementation in your preset.

## GLSL-to-HLSL Shader Conversion

MDropDX12 supports converting GLSL shader code (e.g., from Shadertoy) to HLSL for use in presets. The conversion can be done live via the Milkwave Remote Shader tab.

### Shadertoy Examples

Some Shadertoy shaders that can be converted to MDropDX12 presets:

| Title | ID | Corrections |
|-------|----|-------------|
| Shader Art Coding Introduction | [mtyGWy](https://www.shadertoy.com/view/mtyGWy) | None |
| Cyber Fuji 2020 | [Wt33Wf](https://www.shadertoy.com/view/Wt33Wf) | None |
| Tunnel of Lights | [w3KGRK](https://www.shadertoy.com/view/w3KGRK) | None |
| String Theory 2 | [33sSzf](https://www.shadertoy.com/view/33sSzf) | None |
| Fractal Pyramid | [tsXBzS](https://www.shadertoy.com/view/tsXBzS) | Replace `break;` with `i=64.;` |
| CineShader Lava | [3sySRK](https://www.shadertoy.com/view/3sySRK) | Replace `break;` with `i=64;`, replace aspect correction with `uv.x *= aspect.x;`, remove flipping |

### Known GLSL-to-HLSL Limitations

Common terms that cannot be converted automatically and need manual editing:

| Term | Fix |
|------|-----|
| `break` | Replace with a statement setting a condition to end the loop |
| `myFloat3 *= myMatrix` | `myFloat3 = mul(myFloat3, transpose(myMatrix))` |
| `float3(1)` | Explicitly set all components: `float3(1,1,1)` |
| `atan(a,b)` | `atan2(a,b)` |
| `float[3] arr` | `float arr[3]` |
| `radians(a)` | Multiply by pi/180 directly: `a * (M_PI/180)` |
| `int[3] arr = int[](1,2,3)` | `int arr[3] = {1,2,3}` |
| `int ix = i & 1` (bitwise) | `int ix = i % 2` |
| `int yx = y >> 1` (bitwise) | `int yx = y / 2` |
| Asymmetric returns | Put subsequent code in else branch |

### Resources

- [An introduction to Shader Art Coding](https://www.youtube.com/watch?v=f4s1h2YETNY) (Video)
- [How to create Presets from OpenGL Shaders](https://www.youtube.com/watch?v=Ur2gPa996Aw) (Video)
- [GLSL-to-HLSL reference](https://learn.microsoft.com/en-us/windows/uwp/gaming/glsl-to-hlsl-reference)
- [MilkDrop Preset Authoring Guide](https://www.geisswerks.com/milkdrop/milkdrop_preset_authoring.html#3f)
- [Shadertoy How-To](https://www.shadertoy.com/howto)

## Remote Control (Milkwave Remote)

MDropDX12 is compatible with [Milkwave Remote](https://github.com/IkeC/Milkwave), a separate control application that communicates via the Windows `WM_COPYDATA` IPC protocol.

The Remote discovers MDropDX12 by matching the window title (configurable in Settings > Remote tab). Once connected, the Remote can control presets, messages, sprites, audio settings, Spout output, color shifting, and more.

32 of 34 Milkwave IPC commands are supported, including:

- `MSG|` — Text messages with full formatting
- `PRESET=` — Load preset by path
- `WAVE|` — Live wave manipulation
- `DEVICE=` — Audio device switching
- `OPACITY=` — Window transparency
- `SPOUT_ACTIVE=` / `SPOUT_FIXEDSIZE=` / `SPOUT_RESOLUTION=` — Spout control
- `COL_HUE=` / `COL_SATURATION=` / `COL_BRIGHTNESS=` — Color adjustments
- `CAPTURE` — Screenshot

Not yet handled: `VIDEOINPUT=`

## Script System

The script system plays timed preset sequences synchronized to a BPM (beats per minute). Script files define which presets to load and when, enabling choreographed visual shows.

Manage scripts via Settings > Script tab: browse for a script file, set BPM, and use Play/Stop/Loop controls. See `script-default.txt` for a detailed description of available commands.

## Game Controller

MDropDX12 supports game controllers (gamepads) for hands-free control of the visualizer. Controller buttons are mapped to visualizer commands via a JSON configuration file.

### Setup

1. Open Settings (F8) > System tab
2. Check **Enable** under Game Controller
3. Click **Scan** to detect connected controllers
4. Select your controller from the **Device** dropdown
5. Click **Defaults** to load the default button mapping, then **Save**

The **?** button next to the Game Controller label opens a reference popup showing Xbox Wireless Controller button numbers.

### Button Mapping (controller.json)

Button mappings are stored in `controller.json` in the application directory. The format is a flat JSON object mapping button numbers (1-based) to command strings:

```json
{
  "1": "NEXT",
  "2": "PREV",
  "3": "LOCK",
  "4": "RAND",
  "5": "PREV",
  "6": "NEXT",
  "7": "",
  "8": "",
  "9": "PRESETINFO",
  "10": "SETTINGS",
  "11": "FULLSCREEN",
  "12": "HARDCUT",
  "13": "MASHUP",
  "14": ""
}
```

Lines starting with `//` are treated as comments and ignored. Empty strings disable that button. Commands are case-insensitive and leading/trailing whitespace is trimmed.

### Available Commands

| Command | Action |
|---------|--------|
| `NEXT` | Soft cut to next preset |
| `PREV` | Go back to previous preset |
| `HARDCUT` | Hard cut to next preset (instant) |
| `LOCK` | Toggle preset lock |
| `RAND` | Toggle random/sequential order |
| `MASHUP` | Random mini mash-up |
| `FULLSCREEN` | Toggle fullscreen (ALT+ENTER) |
| `STRETCH` | Toggle multi-monitor stretch mode |
| `MIRROR` | Toggle mirror mode |
| `RESET` | Reset window to safe state (exits stretch/mirror/fullscreen) |
| `PRESETINFO` | Toggle preset info display |
| `SETTINGS` | Open Settings window (F8) |
| `SEND=<n>` | Send virtual keypress (decimal VK code) |

### Xbox Wireless Controller Button Numbers

The Windows Multimedia API (winmm) exposes up to 32 buttons per controller. For an Xbox Wireless Controller (Model 1797), the mapping is:

| Button | Xbox Control |
|--------|-------------|
| 1 | A |
| 2 | B |
| 3 | X |
| 4 | Y |
| 5 | Left Bumper (LB) |
| 6 | Right Bumper (RB) |
| 7 | Back / View |
| 8 | Start / Menu |
| 9 | Left Stick Press (L3) |
| 10 | Right Stick Press (R3) |

**Not available via winmm**: D-pad (reported as POV hat, not buttons), left/right triggers (reported as axes), thumbstick axes, and the Xbox button (reserved by the system).

### Behavior

- **Polling rate**: 20 Hz (50 ms timer on the message pump thread)
- **Rising-edge detection**: Commands fire only on button press, not while held
- **Hot-plug**: If a controller is disconnected, polling silently skips until reconnected
- **Overhead**: Near-zero when disabled (early return before any winmm call)

### Managing Mappings

Use the **Button Mapping** text editor on the System tab to edit mappings directly:

- **Defaults**: Fills the editor with the default DualSense/Xbox mapping
- **Save**: Writes the editor contents to `controller.json` and reloads the config
- **Load**: Reads `controller.json` from disk and fills the editor

## Configurable Hotkeys

MDropDX12 supports configurable hotkeys for frequently used actions. Open the Hotkeys window via **Ctrl+F7** or Settings > System > **Hotkeys...** button.

### Hotkeys Window

The Hotkeys window displays a list of configurable bindings with columns for Name, Key, and Scope. Each binding has:

- **Name**: The action the hotkey triggers (e.g., "Displays Window", "Song Info Window", "Hotkeys Window")
- **Key**: The assigned key combination
- **Scope**: **Local** (works only when the render window has focus) or **Global** (works system-wide via RegisterHotKey)

### Assigning Hotkeys

1. Select a binding in the list
2. Click **Set** to enter capture mode
3. Press the desired key combination (modifiers + key)
4. The new binding is saved automatically

Click **Clear** to remove the current binding. Click **Reset to Defaults** to restore all original assignments.

### Conflict Detection

If you assign a key combination that is already used by another binding, the conflicting binding is automatically cleared to prevent duplicate registrations.

### Default Bindings

| Action | Default Key | Default Scope |
|--------|-------------|---------------|
| Displays Window | Ctrl+F8 | Local |
| Song Info Window | Shift+Ctrl+F8 | Local |
| Hotkeys Window | Ctrl+F7 | Local |
| Launch App 1-4 | (none) | Global |

### Scope Behavior

- **Local** hotkeys only work when the MDropDX12 render window has focus
- **Global** hotkeys work system-wide regardless of which application is in the foreground, using the Windows RegisterHotKey API

### Launch App Slots

The Hotkeys window includes 4 **Launch App** slots for launching or focusing external programs via global hotkeys. This is useful for quickly switching to companion tools like Milkwave Remote.

When you select a Launch App row, a path edit and **Browse...** button appear below the list:

1. Click **Browse...** and select an executable (e.g., `MilkwaveRemote.exe`)
2. Assign a global hotkey (e.g., Ctrl+Alt+R)
3. Press the hotkey to launch the app, or bring it to the foreground if already running

**Behavior**:

- If the configured program is already running, its main window is brought to the foreground (restored from minimized if needed)
- If not running, the program is launched via ShellExecute
- The configured exe name is shown in parentheses in the hotkey list (e.g., "Launch App 1 (Remote.exe)")
- App paths are saved to `settings.ini` under `[Hotkeys]`

## MIDI Input

MDropDX12 has native MIDI input support with 50 configurable mapping slots. Open the MIDI window via Settings > System > **MIDI...** button.

### MIDI Setup

1. Open Settings (F8) > System tab
2. Click **MIDI...**
3. Click **Scan** to detect connected MIDI devices
4. Select your device from the **Device** dropdown
5. Check **Enable** to start receiving MIDI data

### Mapping Slots

The MIDI window displays a ListView with 50 mapping slots (matching Milkwave Remote's 5 rows × 10 banks layout). Each slot has:

| Column | Description |
|--------|-------------|
| # | Slot number (1-50) |
| Active | Whether the mapping is enabled |
| Label | User-defined name for this control |
| Ch | MIDI channel (1-16) |
| Val | Note number (buttons) or CC number (knobs) |
| CC | CC number for knobs |
| Type | Button or Knob |
| Action | The command or parameter to control |

### Learn Mode

The easiest way to assign MIDI controls:

1. Select a slot in the list
2. Click **Learn**
3. Press a button or turn a knob on your MIDI controller
4. The channel, value, CC, and type are auto-detected and filled in
5. Click **Learn** again (or select another slot) to stop learning

### Button Actions

Button mappings trigger on MIDI Note On messages (velocity > 0). Available actions:

| Action | Description |
|--------|-------------|
| NEXT | Soft cut to next preset |
| PREV | Go back to previous preset |
| HARDCUT | Hard cut to next preset |
| LOCK | Toggle preset lock |
| RAND | Toggle random/sequential order |
| MASHUP | Random mini mash-up |
| FULLSCREEN | Toggle fullscreen |
| STRETCH | Toggle multi-monitor stretch |
| SETTINGS | Open Settings window |
| PRESETINFO | Toggle preset info display |
| BLACKOUT | Toggle black mode |
| Any IPC command | e.g., `PRESET=name.milk`, `OPACITY=0.5` |

### Knob Actions

Knob mappings respond to MIDI Control Change (CC) messages. The MIDI value (0-127) is mapped to the parameter range. Available targets:

| Knob Action | Parameter | Range |
|-------------|-----------|-------|
| Hue | Color shift hue | 0.0 – 1.0 |
| Saturation | Color shift saturation | 0.0 – 1.0 |
| Brightness | Color shift brightness | 0.0 – 1.0 |
| Intensity | Visual intensity | 0.0 – 2.0 |
| Shift | Visual shift | -1.0 – 1.0 |
| Speed | Time factor | 0.0 – 5.0 |
| FPS Factor | FPS multiplier | 0.0 – 2.0 |
| Quality | Render quality | 0.25 – 4.0 |
| Opacity | Window opacity | 0.0 – 1.0 |
| Amp Left | Audio amp (left) | 0.0 – 5.0 |
| Amp Right | Audio amp (right) | 0.0 – 5.0 |

Each knob can have an **Increment** value for relative control (set to 0 for absolute mapping).

### Persistence

- **midi.json**: Stores all 50 mapping slot definitions (channel, value, CC, type, action, label, increment)
- **settings.ini [MIDI]**: Stores device selection, enabled state, and buffer delay
- **Save/Load**: Manually save or load `midi.json` from the MIDI window
- **Defaults**: Loads mappings from `midi-default.txt` if present in the application directory

## Song Info Window

Press **Shift+Ctrl+F8** to open a standalone Song Info window showing the current track information (artist, title, album). This is a ToolWindow that runs on its own thread with independent always-on-top, theme support, and sticky position.

The Song Info window provides the same track info source selector and display options as the General tab in Settings, allowing quick access to track info configuration without opening the full Settings window.

## GPU Protection

MDropDX12 includes safeguards against GPU overload:

- **TDR Recovery**: Automatic recovery from GPU timeout detection and recovery events, with window focus restoration
- **Async Shader Compilation**: Shaders compile on a background thread to prevent GPU stalls during preset transitions
- **Shader Compile Timeout**: Shaders that take too long to compile are automatically skipped
- **Max Shape Instances**: Limit the number of custom shape instances per frame (Settings > Visual)
- **Skip Heavy Presets**: Automatically skip presets that exceed a configurable GPU load threshold
- **Device Restart**: Restart the render device from Settings > Visual tab if needed

## Files and Directories

| Path | Description |
|------|-------------|
| `settings.ini` | Main configuration file (section: `[Milkwave]`) |
| `messages.ini` | Custom message definitions |
| `sprites.ini` | Sprite definitions |
| `controller.json` | Game controller button-to-command mappings |
| `midi.json` | MIDI controller mapping definitions (50 slots) |
| `debug.log` | Application log (verbosity set in About tab) |
| `resources/presets/` | Preset directory |
| `resources/presets/Quicksave/` | Quicksave destination (CTRL+S) |
| `resources/presets/Quicksave2/` | Alternate quicksave (SHIFT+CTRL+S) |
| `capture/` | Screenshot output directory |
| `resources/textures/` | Texture files referenced by presets |

## Troubleshooting

**No audio response**: Check Settings > System tab to ensure the correct audio device is selected. Press CTRL+D to reset to default. Verify music is playing through that device.

**Presets render black**: This may be caused by missing textures. Check Settings > Files tab to configure fallback search paths. Missing textures fall back to a 1x1 white texture to prevent black-screen artifacts.

**Shader compilation errors**: Some presets may fail to compile on certain GPUs. MDropDX12 auto-skips failed shaders and moves to the next preset. Enable verbose logging (About tab > Log Level: Verbose) and check `debug.log` for details.

**High GPU usage**: Reduce render quality in Settings > Visual tab, enable Auto Quality, or lower the FPS cap. If using Spout fixed size, reduce the output resolution.

**Window appears frozen**: The visualizer may be in black mode (CTRL+F12) or the preset may be locked on a static visual. Press SPACE to advance to the next preset.

## Getting Help

If you need further help, [open an issue on GitHub](https://github.com/shanevbg/MDropDX12/issues).
