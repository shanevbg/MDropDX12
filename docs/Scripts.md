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

### Driving a test, or a bug report

These five make a script file something you can hand to someone else: "load
this, press Play, send me `log/debug.log`". No Python, no named pipe, no
harness — the log is the result.

| Command | Description |
|---------|-------------|
| `STRICT` | From here on, a command this build does not recognise is an **error** — logged and counted — instead of being drawn on screen as a message. Also resets the error count. |
| `LOG=text` | Write `text` into `debug.log`, so a fault can be pinned to the step that caused it |
| `LOGSTATE` | Write the engine's current state (preset, fps, mesh size, loading, blending, mirrors, display count) into `debug.log` |
| `WAITREADY` | Hold here until the current preset has finished loading, instead of guessing at a number of beats. Times out after 10 seconds, and says so |

`STRICT` is the one that matters. By default **any unrecognised text is
displayed as a message** (see Syntax Rules), which is right for a VJ script and
wrong for a test: a mistyped command, or one that only exists in a newer build,
becomes scenery and the script reports success having done nothing. Under
`STRICT` it is reported instead, and the run ends with a line saying how many
errors there were.

`LOG=` writes at the Error level deliberately — not because it is an error, but
because that level survives every `LogLevel` above Off, so a script sent to
someone whose logging is at the default still produces a readable trace without
asking them to change a setting first.

### Sweeping a library and reporting on it

| Command | Description |
|---------|-------------|
| `CHECK` | Judge the preset currently up and tally it: shader compile errors, and textures it asked for and did not get |
| `REPEAT=count,line` | Jump back to `line`, at most `count` times, then fall through |
| `LOGSUMMARY` | Write the tally so far into `debug.log` |

`REPEAT` is the only bounded backward jump in the language, and it is what makes
a report possible at all: `LINE=` is unconditional, so a sweep written with it
runs until someone presses Stop — and a run that never ends never prints a
summary. `line` is counted from 0, ignoring comments and counting blank lines.

`CHECK` counts missing textures separately from shader errors because they fail
differently. A preset with a missing texture **compiles, renders, and is wrong**
— anything that only asks "did the shaders build" scores it a pass.

Both counters are per preset, reset when each one loads, so a sweep names the
preset at fault rather than reporting a running total.

`resources\scripts\sweep-report.txt` ships as a working sweep. It walks whatever
preset directory the app is currently using; turn Random off first if you want
it walked in order. The run ends with:

```
SCRIPT CHECK: FAIL shader_errors=1 textures_missing=0  C:\...\some preset.milk
SCRIPT SUMMARY: checked=200  shader_failures=1  texture_failures=0  unknown_commands=0
SCRIPT: finished -- 0 errors
```

Ready-made examples ship as `resources\scripts\selftest.txt` and
`resources\scripts\sweep-report.txt`.

```
STRICT
LOG=--- self-test begins ---
LOGSTATE

NEXT
WAITREADY
LOGSTATE

LOG=--- self-test ends ---
STOP
```

These are also reachable over the named pipe, so a harness can run a script
file: `SCRIPT_LOAD=<path>` (replies `SCRIPT_LOADED=<lines>|<path>`),
`SCRIPT_PLAY`, and `SCRIPT_STOP` (replies `SCRIPT_STOPPED|errors=<n>`).

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

#### Preset cycling

`RAND` and `LOCK` toggle. These set an absolute value and can be read back, which is what a client that has to *render* the current state needs — a toggle leaves two clients, or one client reconnecting, unable to agree on what the state is.

| Command | Description |
|---------|-------------|
| `SET_PRESET_ORDER=<0\|1>` | 0 = random, 1 = sequential |
| `SET_PRESET_LOCK=<0\|1>` | Lock or unlock the preset absolutely |
| `SET_TIME_BETWEEN_PRESETS=<sec>` | Seconds between automatic changes; 0 disables cycling |
| `GET_PRESET_CYCLE` | Replies `PRESET_CYCLE=<order>,<lock>,<interval>,<rand>` |

The same values also appear in the `SETTINGS\|` batch as `ORDER=`, `INTERVAL=` and `INTERVALRAND=`, so a client already reading that batch picks them up without a second round trip. Sending `SETTINGS` re-reads the file **and** replies with the batch.

#### Per-display instances and profiles

A display renders in one of **three** tiers, cheapest first:

| Tier | Preset | Where it renders |
|---|---|---|
| `mirror` | follows the primary | in this process |
| `preset` | its own | in this process |
| `child` | its own | its own `MDropDX12.exe -child` process |

`preset` is the middle tier and usually the one wanted: a display holding its own preset does **not** need a process of its own. `own` is accepted as a synonym for `preset`, because it is the obvious guess.

**`child` has no measured performance benefit, and is kept as a choice rather than a recommendation.** It used to be described as buying safety for "a preset heavy enough to endanger the device", and the app used to promote displays to it automatically on that basis. Both are gone (#261), because the measurements did not support either:

- **Memory is not the reason.** Toggling a display's own preset in-process moves VRAM by about 60 MB; three independent presets in one process cost +100 MB over the same setup as mirrors, which is 0.66% of a 15 GB budget.
- **Nor is speed.** Three independent renders of the heaviest preset here sustain 23/25/31 fps at 82% GPU *in one process*. And a child is worse for exactly the case it was supposed to help: Windows classifies QoS from the **focused** window and only one process can own it, so a child is throttled to about 66 fps precisely when it is asked to carry a heavy preset (#121).

What a child still genuinely buys is **fault isolation** — its device loss is its own, so a display that dies does not take the session with it — at the cost of a whole second instance. Choose it for that, not for frame rate.

Full measurements, the governors that replaced automatic isolation, and the measurement traps involved are in [`multidisplay_cost.md`](multidisplay_cost.md).

A display holding its own preset advances on its own timer, independently of the primary and of the other displays, and `SIGNAL\|NEXT_PRESET` on the parent nudges each of them to pick its *own* next preset. Both own-preset tiers are mutually exclusive with `mirror`, which re-draws the *primary's* preset at another orientation and cannot hold a preset of its own.

| Command | Description |
|---------|-------------|
| `SET_DISPLAY_MODE=<N>,<off\|mirror\|preset\|child>` | How DISPLAY N renders. `N` is the device-name index, so `\\.\DISPLAY3` is `3` |
| `GET_DISPLAY_MODE[=<N>]` | `DISPLAY_MODE\|<N>=<off\|mirror\|preset\|child>,device=…,auto=…,independent=…` per monitor, then `\|END`. `auto=1` means the tier was raised to `child` automatically because the preset tripped the GPU-cost gate. Omit `=<N>` for every monitor |
| `GET_CHILDREN` | One `CHILD\|display=…\|pid=…\|preset=…\|fps=…\|interval=…\|interval_raw=…\|order=…\|lock=…\|lock_raw=…\|dir=…\|startup=…\|state=…\|reason=…\|rect=…\|visible=…\|minimized=…\|fs=…` line per child, then `CHILDREN_END`. `fps=` is the rate that child is **actually rendering at**, collected from the poll that already fetches its preset — empty until it has answered once, so "not known yet" stays distinct from a real 0. It is the only way to see it: `DIAG_MIRRORS` reports per-surface figures for **in-process** surfaces only, so a display moved to its own process is invisible to every other frame-rate measurement. **One message per child**, so a client must read until the terminator rather than matching the first reply — a matcher that the terminator also satisfies returns `CHILDREN_END` and silently drops every row. `preset=` is what the child last reported it has loaded and `startup=` what it was configured with, so the two disagreeing is the signal that a child drifted off its preset. `interval_raw`/`lock_raw` keep the `-1` "inherit" sentinel visible where `interval`/`lock` show the resolved value |
| `DIAG_CHILDREN` | Child-worker health: `worker=`, `event=`, `job=`, `queued=`, `children=`, `pending=`, `failed=` |
| `SET_DISPLAY_WATERMARK=<N>,<0\|1>` | Watermark mode for **one** display. `N` is the device-name index throughout, and naming the display the render window is on addresses *this* instance — the primary is an ordinary row, not a special index. A child is relayed `SET_WATERMARK=`, since it owns its window outright and the style pass cannot reach it; an in-process display records the intent and the style pass applies it |
| `GET_DISPLAY_WATERMARK[=<N>]` | `DISPLAY_WATERMARK\|<N>=<0\|1>,device=…,ownProcess=…` per monitor, then `\|END` |
| `SET_DISPLAY_ENABLED=<N>,<0\|1>` | Turn display N on or off. Starts or stops its child if it owns one |
| `SET_DISPLAY_PRESET=<N>,<path>` | Show a preset on display N **now**. Transient — it does not become that display's startup preset, and does not re-point the folder it cycles through. `.milk`, `.milk2` and `.milk3` are all accepted: since v3.2 a display parses and renders a `.milk3` with its own Image and Buffer A–D passes, independently of what the main window is on. Shares one code path with the Displays window's preset picker, so the two cannot answer differently |
| `SET_DISPLAY_STARTUP_PRESET=<N>,<path>` | The preset display N returns to when its instance restarts |
| `SET_DISPLAY_PRESET_DIR=<N>,<path>` | The folder display N cycles through; empty inherits the parent's |
| `SET_DISPLAY_ORDER=<N>,<0\|1>` | Random (0) or sequential (1) order on display N |
| `SET_DISPLAY_TIME_BETWEEN=<N>,<sec>` | Cycle interval for display N. `0` never cycles, `-1` inherits the parent's |
| `SET_DISPLAY_MSGMAX=<N>,<-1\|0..n>` | How many messages display N shows at once; `-1` inherits the main window's, which cannot itself inherit. The supertext slots are **one shared pool**, so without this a busy panel takes them all and the other screens fall silent |
| `DISPLAY_NEXT=<N>` / `DISPLAY_PREV=<N>` | Advance or step back **display N only**, leaving every other display where it is |
| `CHILD_TEST_WINDOWS=<auto\|off\|tiny\|hidden>` | **Testing.** How a child's window comes up; `auto` follows testing mode. Applies to the next spawn |
| `SET_DISPLAY_CHILD_RECT=<N>,<x>,<y>,<w>,<h>` | **Testing.** Where display N's child window goes in `tiny` mode; `w=0` restores the corner default |
| `SAVE_DISPLAY_SNAPSHOT` | Bank every screen's **current** preset as a timestamped profile |
| `LIST_DISPLAY_PROFILES` | One `DISPLAY_PROFILE\|<name>` line per saved profile, then `DISPLAY_PROFILES_END` |
| `NEXT_DISPLAY_PROFILE` / `PREV_DISPLAY_PROFILE` | Load the next/previous saved profile, wrapping |
| `SAVE_DISPLAY_PROFILE=<path>[\|primary=1][\|messaging=1]` | Write the whole display state to a JSON profile |
| `LOAD_DISPLAY_PROFILE=<path>` | **Replace** the display state with a profile — anything it does not name is turned off |
| `GET_DISPLAY_PROFILE` | Path of the profile last saved or loaded |

#### Profiles by name

The verbs above take a **path**. There is a name-addressed set as well, and it is
the one to prefer: a client should not have to know where the folder is, and a
profile saved by name lands in the right place automatically. Gate on the
`displayprofiles` feature token.

| Command | Reply | Description |
|---------|-------|-------------|
| `DISPLAY_PROFILE_LIST` | `DISPLAY_PROFILE\|file=…\|name=…` per profile, or `DISPLAY_PROFILE_NONE` | Saved profiles, as leaf and display name. **One message** with the rows newline-joined, not a stream with a terminator |
| `DISPLAY_PROFILE_SAVE=<name>[\|primary=1][\|messaging=1]` | `DISPLAY_PROFILE_SAVED\|file=…` | Save by name. An empty name banks a timestamp. **Does not** change the startup default — a save should not silently decide what loads at boot |
| `DISPLAY_PROFILE_LOAD=<name>` | async `PROFILE_LOADED=<path>` | Load by name. Errors come back as `DISPLAY_PROFILE_ERR\|reason=unknown_profile` or `…\|reason=malformed` |
| `DISPLAY_PROFILE_DELETE=<name>` | `DISPLAY_PROFILE_DELETED\|file=…` | Delete a saved profile (to the Recycle Bin) |
| `DISPLAY_PROFILE_STARTUP[=<name>]` | `DISPLAY_PROFILE_STARTUP\|file=…\|enabled=<0\|1>` | Query, or set which profile loads at startup. An empty name disables it |

Files whose name begins with `_` never appear in these listings — they are
bookkeeping, not profiles anybody chose. `_idle_restore.json` is the one in use
today (see [Idle Timer](#idle-timer-ipc)).

`state` answers "why is there no child on this display", which used to be
unanswerable — `absent` meant three different things and a remote could only
show a spinner:

| `state` | Meaning | What a client should offer |
|---------|---------|----------------------------|
| `pending` | A spawn is queued, or the worker is acting on one | "Starting…" |
| `starting` | The process exists; its pipe has not answered yet | "Starting…" |
| `ready` | Up and answering | Its preset and controls |
| `failed` | The last attempt failed; `reason=` says how | The reason, and a retry |
| `absent` | Requested, nothing in flight, nothing tried | A Start button |

`lock` is the effective preset lock for that display and `lock_raw` is what it
was set to: `-1` inherit the main window's, `0` unlocked, `1` locked. The lock is
global — every display follows the main window unless it has pinned its own
value with `SET_DISPLAY_LOCK`, on the Displays tab, or in a display profile.

`reason=` is on every line and empty unless `state=failed`. Its tokens are
`hosts_render_window`, `create_process_failed`, `exited_before_pipe`,
`pipe_timeout` and `exited`.

A display with no startup preset set gets a **random one from its preset
directory** when its child starts. A child reads the shared `settings.ini`, so
without this it comes up on the preset the main window last saved — the one the
main window is already showing, which is not what "own preset" means.

Profiles live in `resources/displayprofiles/`. `LOAD_DISPLAY_PROFILE=` takes a
bare name from there or a full path, so older absolute paths keep working.

`SAVE_DISPLAY_PROFILE=` normally stores the settings that decide what each
display plays. Add `|snapshot=1` — which is what `SAVE_DISPLAY_SNAPSHOT` does —
to store what each display is **playing right now** instead, so loading it puts
that exact set of presets back on those exact screens. Snapshots are named
`yyyy-MM-dd_HH-mm-ss.json`, so listing them by name lists them by age, and a
script can walk the set with `NEXT_DISPLAY_PROFILE`.

A profile is a complete state, not a patch: loading one disables any output it does not mention and removes Spout senders it does not name, so switching between profiles round-trips exactly.

`|primary=1` additionally stores the main window's preset directory, startup preset, cycle interval, order and lock. `|messaging=1` stores the messaging setup **with the message set inlined** — not a reference to `messages.ini`, so a captured setup keeps meaning the same thing after that file is edited. Loading replaces the in-memory messages and writes nothing; an empty message array is treated as "leave the existing messages alone", so a truncated profile cannot silently empty the list.

Set `szStartupDisplayProfile` and `bLoadDisplayProfileOnStart` in `settings.ini` to apply a profile at startup. It **wins over** the per-display values in `settings.ini` — getting that exact state is the point of asking for it.

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
| `STYLE=<name>` | Select a message **animation profile** by name — the entrance, movement and fade set up in the Text Animations window. Applies to messages that follow, like the other defaults here |
| `CLEARPARAMS` | Reset font, size, and color to defaults (Arial, 20, white) |

### Sprites and Messages

| Command | Description |
|---------|-------------|
| `CLEARSPRITES` | Remove all active sprites |
| `CLEARTEXTS` | Remove all active text messages |
| `SEND=00` | Send a string to the visualizer window (e.g., trigger sprite 00) |
| `SEND=0x73` | Send a virtual key code (hex) to the visualizer (e.g., 0x73 = F4) |
| `BTN=<n>` | Press Button Board button `n` — whatever that button is bound to, without needing to know what it is |
| `SONGINFO` | Open the Song Info window |
| `SOUNDINFO` | Toggle the sound debug display |

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

#### Frame pacing

| Command | Description | Range |
|---------|-------------|-------|
| `SET_FPS=N` | Primary window frame cap. Reply `FPSCAP=<fps>` | 0 = unlimited, else 5-720 |
| `GET_FPS` | Read the primary cap. Reply `FPSCAP=<fps>` | — |
| `SET_MIRROR_MAXFPS=N` | Independent-mirror worker cap. Reply `MIRROR_MAXFPS=<fps>` | 0 = parity with primary, else 5-240 |
| `GET_MIRROR_MAXFPS` | Read the mirror cap. Reply `MIRROR_MAXFPS=<fps>` | — |
| `SET_MIRRORCAP=N` | Persisted mirror swap-chain long-edge cap. Reply `MIRRORCAP=<applied>\|setting=<N>` | -1 derive from the VRAM budget (default), 0 no cap, else a fixed long edge |
| `DIAG_MIRRORCAP=N` | The same cap, **unpersisted** — an experiment, not a decision. Reply `MIRRORCAP=<applied>\|source=<derived\|setting\|diag>\|setting=<n>\|derived=<n>\|budgetMB=<n>\|share=<pct>` | -1 hands control back to the derivation |
| `SET_LADDER_TRIGGER=<-1\|fps>` | `LADDER_TRIGGER=<applied>\|setting=\|derived=\|target=` | The rate below which the distress ladder falls back to copy mirrors. `-1` derives it from the frame cap — 34 when that is high or unset, two thirds of it at or below 35 — and a value pins it. **Persisted.** It is a setting rather than a diagnostic because whether a given multi-display rate is acceptable is a judgement about the person watching: measured on a 3-display rig, three independent renders of the heaviest preset here reach ~35 fps achievable, one frame the right side of the derived 34, so someone who wants that case to fall back has to say so |
| `GET_LADDER_TRIGGER` | as above | Read it back, including what the derivation *would* choose, so an override can be compared against the rule it replaced |
| `SET_VSYNC=N` | Wait for vblank when presenting. Reply `VSYNC=<0\|1>` | 0 = off, 1 = on |
| `GET_VSYNC` | Read VSync for every display output (batch, see below) | — |

`MIRRORCAP` is a **size** cap, not a rate cap: it limits the long edge of each
mirror swap chain. It defaults to a value derived from the reported VRAM budget
— a tenth of the budget, split between the enabled monitor outputs, converted to
a 16:9 long edge and rounded down to a multiple of 256 so ordinary drift in the
budget does not resize anything. On any ordinary GPU that lands at the 8192
ceiling, which is past every panel it will meet, so the derived cap is
effectively "no cap"; a small card gets a real one. `derived=` in the
`DIAG_MIRRORCAP` reply always reports that computation even when a setting or a
diagnostic override is what is actually applied, so the two can be compared
without clearing either (#254).

The cap is any integer in range, not a menu choice — a deliberately unusual
value such as `SET_FPS=137` is the cleanest way to tell "this surface is
delivering what it was asked for" apart from "this surface is sitting at
something near the panel's refresh rate for some other reason".

`GET_VSYNC` answers for **every display output**, because one process-wide bool
is not the whole truth:

    VSYNC=0
    VSYNC_OUT|display=<primary>|type=primary|vsync=0|effective=1|source=engine|state=ready
    VSYNC_OUT|display=\.\DISPLAY2|type=mirror|vsync=0|effective=0|source=fixed|state=ready
    VSYNC_OUT|display=\.\DISPLAY3|type=child|vsync=0|effective=0|source=broadcast|state=ready
    VSYNC_END

The scalar `VSYNC=` line comes first and keeps its old meaning, so a client that
only wants "is VSync on" parses what it always did.

`vsync=` is the setting; **`effective=` is what the surface actually does**, and
they differ. The primary is forced to vsync whenever mirrors are active, so it
can report `vsync=0|effective=1`. Mirror panels present unsynced by
construction and are always `0`, whatever the setting says — `source=fixed`
marks a value no setting reaches. `source=broadcast` marks a child, which is a
separate process with its own copy of the flag that `SET_VSYNC` pushes to it;
a bare `-child` with no owner receives no broadcast and must be asked on its own
pipe.

**`SET_VSYNC=0` is a prerequisite for any frame-rate measurement.** VSync
defaults **on**, and with it on every surface presents at the panel's refresh
regardless of the caps above, so a measurement taken without turning it off is
reading the monitor rather than the engine. Note also that mirrors force VSync
on the primary while they are active, and that a mirror left at the default
`MIRROR_MAXFPS=0` runs in parity mode — locked to the primary's rate by design
rather than free-running.

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

#### With arguments

Quote the path, and everything after it is passed as arguments:

```
LAUNCH="C:\Python313\python.exe" "resources\scripts\compare_milkwave.py" --presets 20
LAUNCH="C:\Windows\System32\cmd.exe" /c echo hello
```

Two things change when a path is quoted:

- Everything after the closing quote becomes the command line arguments.
- It always starts a **new** process instead of focusing one that is already
  running. Arguments are a request to *do something*, and handing focus to a
  copy started earlier with different arguments would silently do nothing.

The unquoted form is unchanged, including for paths with spaces in them —
`LAUNCH=C:\Program Files\App\app.exe` still treats the whole value as the path.
Only a leading quote turns arguments on.

> `|` cannot be used as a separator here. The script tokenizer splits lines on
> `|` before a command is dispatched, so `LAUNCH=app.exe|--flag` arrives as two
> separate commands.

`EXEC=<command>` runs a command line instead of opening an application. Use
`LAUNCH=` when you want the window — it focuses an instance that is already
running rather than starting a second one — and `EXEC=` when you only want the
command to happen. The same `|` restriction applies.

### Comparing against Milkwave

Milkwave 4 ships MDropDX12 v3, so both are installed together and can be
compared side by side. Two files ship for this:

| File | What it is |
|---|---|
| `resources/scripts/compare-milkwave.txt` | the script you run |
| `resources/scripts/compare_milkwave.py` | the helper it launches |

Edit the two paths at the top of the `.txt` — your Python, and your Milkwave —
then run it from the Script window, or over IPC with
`SCRIPT_LOAD=resources\scripts\compare-milkwave.txt` followed by `SCRIPT_PLAY`
(the path is taken literally, relative to the working directory).

The helper plays the same preset in both engines, photographs each window,
scores how far apart they look and writes `comparison/report.txt` ranked worst
first, with a side-by-side PNG per preset.

It makes the two comparable before measuring anything:

- **Same window size.** Both engines render to the shape they are given, so
  comparing a tall window against a wide one measures the window rather than the
  engine. Both are set to the smaller of the two client sizes and put back
  afterwards.
- **Same audio device.** `DEVICE=` is sent to both, so they are not visualising
  different sounds. Without this, one engine listening to a silent endpoint
  looks exactly like a rendering difference.

It never moves, raises or focuses either window — `PrintWindow` photographs a
window that is completely covered — and it refuses to run if more than one copy
of either program is up, rather than guessing which to reconfigure.

Needs Pillow, and offers to install it if it is missing. Milkwave must not be
minimised: a minimised window has nothing to photograph.

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

### Where to start

The surface is large — around 290 commands — and most of it is one of a dozen
subsystems. If you know what you are trying to do, start here rather than
reading down:

| I want to… | Go to |
|---|---|
| know what *this* build accepts | [`HELP`](#ask-the-running-build-help) — authoritative, unlike this file |
| gate a client on a capability | [Knowing what a build supports](#knowing-what-a-build-supports) |
| give each screen its own preset | [Per-display instances and profiles](#per-display-instances-and-profiles) |
| save and restore a whole screen arrangement | [Profiles by name](#profiles-by-name) |
| set up a walk-away screensaver | [Idle Timer](#idle-timer-ipc) |
| put every screen up, then put them back | [Walk-away control](#walk-away-control) |
| fix a preset that washes out to white | [Runaway feedback](#runaway-feedback-canvas-damp-and-frame-caps) |
| rate, tag or annotate presets | [Preset annotations](#preset-annotations-ratings-flags-tags-and-notes) |
| pin a shader or VFX profile to one preset | [Per-preset overrides](#per-preset-overrides) |
| drive or photograph a tool window | [Driving the tool windows](#driving-the-tool-windows) |
| work out why something looks wrong | [Diagnostics](#diagnostics) |
| script a repeatable test | [Driving a test, or a bug report](#driving-a-test-or-a-bug-report) |
| control volume and audio devices | [Audio](#audio), then [`mixer.md`](mixer.md) |

**Two conventions worth knowing before you write a client.**

*Prefer the absolute form to the toggle.* Where both exist —
`SET_MIRROR_WATERMARK=<0|1>` beside `SIGNAL|MIRROR_WM`, `SET_BORDERLESS_FS=`
beside `SIGNAL|BORDERLESS_FS` — the absolute one is there because driving a
switch from a toggle means read, compare, and send only if they differ, and that
is a race against anything else changing it.

*Read until the terminator.* A command that answers with a list sends **one
message per row** and then a terminator — `CHILDREN_END`, `WINDOWS_END`,
`PRESET_ROWS_END`, `MIXER_END`, `END_BATCH`. A client that returns the first
reply it sees will sometimes get the terminator and silently report nothing.

### Protocol

- **Pipe name**: `\\.\pipe\Milkwave_<PID>` (discover via `CreateToolhelp32Snapshot` or the MCP server)
- **Child instances use a different namespace**: a display running as its own
  process serves `\\.\pipe\mDxChild_<PID>` and **accepts connections only from
  its parent**. `Milkwave_` is shared -- Milkwave Visualizer publishes it too,
  and every discovery tool enumerates it looking for something to drive -- so a
  child must not appear in it. The parent owns every external interface, which
  is already why a child serves no TCP port and no mDNS advert. Drive a child
  through the parent: `DISPLAY|<N>|<command>`, or a `SET_DISPLAY_*` verb.
- **Message encoding**: UTF-16LE null-terminated wide strings
- **Field delimiter**: `|` separates fields within a command
- **Duplex**: The pipe supports bidirectional communication (e.g., `STATE` returns data)
- **Busy pipe**: the server keeps a pool of instances with a connect pending on
  each, so a reconnecting client normally finds one free. A client that
  reconnects faster than the server can re-arm will still see `ERROR_PIPE_BUSY`
  (measured: none below ~1,800 connects/second, saturation at ~270,000).
  Clients should retry, as `WaitNamedPipe` does.

**Over the network.** The same command vocabulary is served on TCP for LAN
clients, which authenticate once per connection and are then remembered:

| Command | Description |
|---------|-------------|
| `LIST_DEVICES` | Devices currently authorised for the TCP remote |
| `DEAUTH_DEVICE\|<id>` | Revoke one device's authorisation and disconnect it |

### Ask the running build: `HELP`

This document is written by hand and cannot be trusted to match the binary you
are talking to. **The running instance can list its own commands**, which is
authoritative for that build:

| Command | Reply |
|---------|-------|
| `HELP` | The full index: one message per group, each line `usage  description`. Ends with `END_BATCH` |
| `HELP=<command>` | One entry - usage, group, description |
| `HELP=<group>` | Every command in one group (`preset`, `shader`, `vfx`, `audio`, `display`, ...) |
| `HELP=JSON` | `HELP_JSON_BEGIN\|parts=<n>`, then `HELP_JSON\|<i>\|<chunk>` messages, then `END_BATCH` |
| `COMMANDS` / `LIST_COMMANDS` | Aliases for `HELP` |
| `GET_VERSION` (or exactly `VERSION`) | `VERSION=<x.y.z>\|build=...\|config=...\|arch=...\|protocol=<n>\|pid=<n>\|child=<0\|1>\|commands=<n>\|script=<n>\|signals=<n>` |

Reassemble `HELP=JSON` by filtering messages that start with `HELP_JSON|`,
sorting by the index, and concatenating the payloads - the chunks are numbered
because a client's queue can also hold messages from an earlier batch.

`protocol` increments on **any** change a client can depend on — a new command
or an added reply field counts, not only a breaking one — so a client can
feature-detect without comparing version strings. See
[Knowing what a build supports](#knowing-what-a-build-supports) for the numbered
history and the `features` list that goes with it.

Note that `VERSION` is matched **exactly**: `VERSION=<n>` remains the script
command that sets `vis_version`.

**A command missing from `HELP` is not invalid.** Anything the IPC handler does
not recognise is passed to the script engine (see Fallback, below), so the index
lists what is dispatched explicitly plus the script vocabulary reachable that
way - not an exhaustive list of what will be accepted.

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
| `DIAG_MIRRORS` | `MIRRORS\|…` | Rich mirror dump. Global: `main=WxH,mainPort,needSrv,canSample,anyOpp,anyIndepMilk3,shadertoy,compPso,slots,frame,skipFrames,allocHr,listHr,auxUsed,orient=WxH,orientReady,orientFrames,orientFb`. Per `monN`: enabled/opacity/clickthru/independent/skipped/portrait, swapsize/bufs, layered/visible, **path** (`orient`/`stretchMain`/`letterbox`/`copy`/`black`/`orientFail`), drawFI/presFI/presHr, ok/fail/skip/draws, mustBlock/oppIndep/wipe/paintMask, bb0/bb1/bb2 states. Use when debugging portrait landscape ghost / every-other-frame. Per `surf=` (one row per surface that renders for itself — an own-preset display or an independent mirror): `own` (does it hold a preset of its own), `preset` (that preset's filename), **`renders`** (`classic` or `milk3` — the pipeline that ACTUALLY ran), `size`, `ready`, `frames`, `pf`/`ovr` (its own per-frame clock), `simFps`, `simTime`, `bindBase`, `workUs`, `sub`/`done` (its own fence), `prims`/`psInv`/`overdraw`, `gpuUs`. **`preset` and `renders` answer different questions and you usually want both**: `preset` is what the display is configured to play, `renders` is which renderer drew the frame. Before v3.2 only the first existed, and a display drawing the main window's Shadertoy went on reporting its own preset the whole time — the one diagnostic for that fault asserted nothing was wrong. |
| `DIAG_WINDOW=<title>` | `WINDOW_CTRLS\|…`, one `WINDOW_CTRL\|…` per control, `WINDOW_CTRLS_END` | Every control on one of our own tool windows: `class`, `id`, `x`/`y`/`w`/`h` (client coords), `vis`, `enab`, `clip` (1 when the client edge cuts the control's RECTANGLE off), `tw` (the width the CONTROL would need to hold its text on one line, its own chrome included, so `textfits` is 0 exactly when `tw` > `w`), `textfits` (0 when the text does not fit the control it is in, 1 when it does, -1 where the question is not asked — an edit or a combo holds the user's data and scrolls it), `u=` the text's CODEPOINTS, and `text=` the text itself (`\|` escaped as `\|`, backslash as `\\`, CR/LF as `\r`/`\n`; decode left to right in ONE pass — the pipe is deliberately **not** `\p`, because almost every string here is a Windows path and a two-pass decoder turns `C:\resources\presets\` into `C:\resources\|resets\`, which looks exactly like a truncated buffer). Title matched case-insensitively, whole title before prefix. `WINDOW_CTRLS=NOTFOUND` if no visible window matches. **The codepoints are the point:** a glyph button whose escape was mangled into the system codepage still prints as something, and only `u=` shows `U+E8E5` arriving as `00C3 00A8 0045 0035`. `clip` and `textfits` are different failures and neither implies the other: a button in the right place can still be drawn as `Ou` instead of `Out`. `textfits` is measured against this app's OWN drawing code — every button, check box and radio here is `BS_OWNERDRAW`, so the check-glyph reserve comes from `DrawOwnerCheckbox`, not from a system metric — which is one more reason it can only be answered in-process. |
| `CAPTURE_WINDOW=<title>` | `WINDOW_CAPTURE_PATH=<png>` | A PNG of one tool window, written under `capture\`. Taken INSIDE the process with `PW_RENDERFULLCONTENT` — from another process `PrintWindow` does not render owner-drawn buttons at all, so the pin, the refresh and every glyph button come back as flat background and a broken one looks exactly like a working one. Use `CAPTURE` for the render output; this is for the windows. |
| `DIAG_MESSAGES` | `DIAG_MESSAGES\|…` | Every term of the autoplay guard — `enabled`, `testing`, `autoplay`, `count`, `nextAt`, `now`, `interval`, `jitter`, `maxOnScreen`, `sequential`, `nextSeq`, `animProfiles` — then one `stN=` block per live supertext (`start`, `dur`, `fadeOut`, `burn`, `redraw`, `song`, `text`); free slots are omitted. Use when messages do not appear: `nextAt` far ahead of `now` means the clock moved backwards, and a `stN` with `start` in the future is a leaked slot. |
| `DIAG_MIRRORPACE=<0\|1>` | `MIRRORPACE=<0\|1>,skipped=<n>` | The closed-loop mirror pacer (#230), which holds mirror GPU work below the TDR watchdog by dropping frames when the measured cost climbs. **Off exists only to demonstrate the device removal it prevents** — turn it off to show the failure is still reachable, then back on. `skipped=` is the running count of frames it dropped. |
| `DIAG_BALANCE[=<0\|1>]` | `BALANCE=<0\|1>\|state=\|main=\|slowest=\|displays=\|ratio=\|cap=\|userCap=\|baseline=` | Adaptive frame-rate balancing (#261), which replaced the automatic isolation gate `DIAG_GPURISK` switched. When the main render exceeds **1.25×** the slowest independent display, the main is capped to that ratio and the displays re-measured. If they improved the cap is kept and tracks the current slowest, so the main rises again as they do; if they did **not** improve, the cap is released and the preset is `ruledout`, because some presets are simply slow and capping the main buys nothing. `state=` is `idle`/`probing`/`holding`/`ruledout`; `userCap=` is the frame cap being held on the user's behalf (`-1` when none). The verdict resets on a preset change. The cap is never written to `settings.ini`. Children are excluded from `slowest=`: they render in another process, so capping this one frees them nothing. |
| `DIAG_SIMULATE_TDR` | `SIMULATE_TDR\|ok=<0\|1>` | **Testing mode only**, and it makes the application EXIT and start a replacement — which is exactly what nobody wants arriving down a pipe by accident, hence the gate. Drives the GPU-fatal path with no GPU involved, so the whole device-loss response can be exercised without resetting the display driver for the machine: the mirror-resolution backoff (1920 → 1280 → 960 → off), the relaunch, the successor waiting for its predecessor to exit, and the three-attempt session budget. A fourth call in the same chain exits instead of relaunching. Refuses with `err=not_testing` unless `TESTING_MODE=1` is set first. The one thing it does *not* exercise is the detection ahead of it — that a real `DXGI_ERROR_DEVICE_HUNG` arrives — which needs a real TDR. |
| `SET_MIRROR_WATERMARK=<0\|1>` | `MIRROR_WATERMARK=<0\|1>` | Watermark mode for **every** mirror at once — absolute, not a toggle. Per-display is `SET_DISPLAY_WATERMARK=<N>,<0\|1>`; the primary's own is `SET_WATERMARK=<0\|1>` / `GET_WATERMARK`. |
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
| `GET_WINDOW[=<title>]` | `WINDOW\|role=\|display=<N>\|device=\|pid=\|title=\|x=\|y=\|w=\|h=\|client=WxH\|monitor=\|visible=\|minimized=\|maximized=\|topmost=\|dpi=` | Where a window IS. Bare gives the render window; a title matches one window (whole title before prefix, case-insensitive), `WINDOW=NOTFOUND` if none. Screen coordinates, because that is the frame a client arranging itself around us has to reason in. `client=` is **not** redundant with `w`/`h`: the render cost, the swap-chain size and every capture are client-sized, while a layout must reserve the FRAME — on a borderless window the two are equal, on a tool window they differ by caption and border, and that difference is exactly the margin a layout gets wrong when it only has one of them. `role=` is `render`, `mirror`, `child` or `tool` — with several displays these look identical from outside, and only the app can tell them apart. `display=` is the 1-based index `GET_DISPLAY_MODE` uses and `device=` its device name, so the two verbs join without inferring a monitor from coordinates; `pid=` appears only for a child, which runs in its own process. `monitor=` is where Windows says the window actually is, as against `device=` which is where the engine assigned it — normally identical, and worth being able to see when they are not. Device names are emitted raw, unescaped, matching `GET_DISPLAY_MODE` |
| `GET_WINDOWS` | `WINDOWS\|count=<n>`, one `WINDOW\|…` per window, then `WINDOWS_END` | Every window this app owns — render window, in-process mirrors, tool windows, **and children**, each with the fields above plus `class=`. A child renders in its **own process**, so it cannot be found by an enumeration filtered to our pid and is contributed from the child registry instead; without that a three-display setup with two children reports one window and looks perfectly correct. A child spawned whose window has not appeared yet is reported as `state=nowindow` rather than omitted, since "waiting for it" and "not a child" are different answers. Hidden ones are listed with `visible=0`, since a child in hidden mode is a real window with a real rect; zero-area windows are dropped, because the IME plumbing Windows attaches to every process carries a title and would otherwise be three rows of noise. A **closed** tool window does not appear at all: `ToolWindow::Close` destroys it, so its remembered position lives in the ini rather than in an HWND. Terminated, unlike `UI_LIST` |

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
| `GET_AUDIO_DEVICES` | Every capture and render device this build can see |
| `SET_AUDIO_GAIN=<n>` / `GET_AUDIO_GAIN` | Input gain applied before the FFT, independent of the Windows device volume |
| `GET_AUDIO_DIAG` / `DIAG_AUDIO` | What the capture is actually receiving — the first thing to read when a preset looks deaf |
| `AUDIO_PROFILE=<name>` | Apply a saved audio profile (the response curves that make this engine sound like another) |
| `AUDIO_PROFILE_LIST` / `AUDIO_PROFILE_CLEAR` | List them; drop back to the built-in response |

#### Mixer

The mixer is a subsystem of its own and **off by default**. The verbs below are
the ones worth knowing from a script; the whole surface — routes, failover,
groups, profiles and the view model — is in [`mixer.md`](mixer.md).

| Command | Description |
|---------|-------------|
| `MIXER_ENABLE=0\|1` | Turn the subsystem on or off. Responds `MIXER_ENABLED=0\|1` |
| `MIXER_SUBSCRIBE=0\|1` | Ask for live updates. Gates **all** provider polling, so leaving it off costs nothing |
| `MIXER_STATE` | Every channel, fader, route and device. Answers in chunks bracketed by `MIXER_BEGIN` / `MIXER_END` — read until `MIXER_END` |
| `MIXER_SET=<ch>\|<fader>\|<0.0-1.0>` | Set one fader's volume. Responds with that `MIXER_FADER` record, or `MIXER_ERR\|reason=…` |
| `MIXER_MUTE=<ch>\|<fader>\|<0\|1\|toggle>` | Mute, unmute or toggle one fader |
| `MIXER_REFRESH` | Re-enumerate devices now |
| `MIXER_BATTERY` | Headset battery, where the device reports one |
| `MIXER_PROFILE_LIST`, `MIXER_PROFILE_SAVE=<name>`, `MIXER_PROFILE_LOAD=<name>`, `MIXER_PROFILE_DELETE=<name>` | Saved mixer profiles — levels and layout together |
| `MIXER_FADER_NAME=<ch>\|<fader>\|<name>` | Rename one fader |
| `MIXER_RENAME_DEVICE=<id>\|<name>` | Rename a device everywhere it appears |
| `MIXER_HIDE=<ch>\|<fader>\|<0\|1>` / `MIXER_HIDDEN` | Hide a fader from the drawn list; query what is hidden |
| `MIXER_WINDOW=<0\|1>` | Show or hide the Mixer window |
| `MIXER_CFG_FILE=<path>` / `MIXER_CFG_PATH` | Point the mixer at another config file, and read back which one is in use. **A test must set this** rather than write the real one |
| `MIXER_ALLOW_MOVE=<route>\|<deviceId>\|<delta>` | Reorder a route's allowlist — the order failover tries devices in |
| `MIXER_CONFIRM_STATUS`, `MIXER_CONFIRM_BEGIN=`, `MIXER_CONFIRM_SET=`, `MIXER_CONFIRM_ANSWER=` | The "did that work?" prompt after a failover switch, driven without clicking it |
| `MIXER_SONAR_RESTART` | Restart SteelSeries Sonar, which hangs often enough to need it |
| `DIAG_MIXER` | Mixer subsystem state, for the probe harness |
| `MIXER_ORDER` | The STORED fader order, one `MIXER_ORDER` record per key, then `MIXER_ORDER_END\|count=…\|rev=…`. Not what the window draws — see `MIXER_VIEW` |
| `MIXER_VIEW` | The fader list as DRAWN, with the view rules applied. One record per fader carrying `pos` (drawn row, `-1` if hidden), `order` (its index in `MIXER_ORDER` space), `hidden`, `pinned`, then `MIXER_VIEW_END` with the counts, the `rev` and the three view flags |
| `MIXER_ORDER_REV` | Just the arrangement token, for a client checking whether the list it holds is still current |
| `MIXER_ORDER_MOVE=<ch>\|<fader>\|<delta>[\|<rev>]` | Move one fader in stored order. With `rev`, refused as `MIXER_ERR\|reason=stale\|rev=…` if the arrangement changed since |
| `MIXER_ORDER_SET=<rev>\|<key>,<key>,…` | Push a whole arrangement. `rev` is required; omitted keys keep their order and are appended |
| `MIXER_GROUPS` | Every hotkey group, one `MIXER_GROUP` record each: `group` (1-based), `name`, `members`, `present`, then one `member=<ch>\|<fader>` per fader. `present` is how many can be moved right now, so it drops below `members` when a device is switched off |
| `MIXER_GROUP_SET=<n>\|<ch>\|<fader>\|…` | Replace group `n`'s membership wholesale. Pairs, so an odd tail is `MIXER_ERR\|reason=malformed`. Not validated against the current channel list — a member may name hardware that is switched off |
| `MIXER_GROUP_NUDGE=<n>\|<delta>` | Move every present member by `delta`, clamped 0..1 per fader so the offsets between them survive. Replies with the group record plus `moved=<count>` |
| `MIXER_GROUP_MUTE=<n>` | One decision for the group: any member unmuted means mute them all, else unmute them all. Replies with the group record plus `changed=<count>` |
| `MIXER_ROUTE_ARM=<route>\|<0\|1>` | Opt a route in or out of failover. Off by default |
| `MIXER_ROUTE_SET=<route>\|<deviceId>` | Point a route at a device now; cancels any pending failover |
| `MIXER_ALLOW_ADD=<route>\|<deviceId>` | Append a device to that route's ordered allowlist |
| `MIXER_ALLOW_REMOVE=<route>\|<deviceId>` | Remove a device from the allowlist |
| `DIAG_FAILOVER` | Failover state for the default-render route |
| `MIXER_SIM_DEVICE=<id>\|<present\|gone\|current\|forget>` | Testing mode only: drive the watcher without hardware |

### Knowing what a build supports

`GET_VERSION` answers with the build's identity:

    VERSION=3.2.0|build=…|config=Release|arch=x64|protocol=11|pid=…|child=0
    |commands=292|script=56|signals=21|features=mixerview,mixerrev,mixerpush,…,idleconfig

**Gate on `protocol` and `features`, never on the version string.** A version is
what a release is called; these two describe the shape of the wire, which is
what a client actually needs to know.

`protocol` is a monotonic integer and moves for **any** change a client can
depend on — a new command or an added field counts, not only a breaking one. It
stood at 1 through the whole of the v3 work under the old "only when an existing
command's contract changes" rule, which is why the rule changed (#74).

    1   the original surface, through the v2.11.0 release
    2   the mixer view, the arrangement token, and both fader fields
    3   the hotkey diagnostics: what is bound globally, and what is HELD
    4   groups= on MIXER_FADER
    5   named display-profile IPC, and DISPLAY_PROFILE_STARTUP
    6   SET_ALL_FULLSCREEN=/GET_ALL_FULLSCREEN, fs= on GET_CHILDREN
    7   SET_IDLE_ACTIVE=/GET_IDLE_ACTIVE — trigger the idle action on demand
    8   SET_VSYNC=/GET_VSYNC
    9   SET_MIRROR_WATERMARK= — the absolute form, alongside the toggles
    10  the idle timer is configurable and resettable from a remote
    11  SET_TEXCAP=/GET_TEXCAP — the longest edge a texture loaded from disk keeps

`features` names capabilities individually, because a client usually degrades
per feature rather than per build — MilkDrop Remote has a separate fallback for
the drawn order, the token, the battery field and the push, and will happily use
three of the four.

| name | what it promises |
|---|---|
| `mixerview` | `MIXER_VIEW` / `MIXER_VIEW_END`, and `MIXER_ORDER_END` on `MIXER_ORDER` |
| `mixerrev` | `MIXER_ORDER_REV`, the optional `rev=` guard on `MIXER_ORDER_MOVE`, and `MIXER_ORDER_SET` |
| `mixerpush` | `MIXER_VIEW_CHANGED` to subscribers when the arrangement moves on its own |
| `mixerbattery` | `battery=` on `MIXER_FADER` |
| `mixernames` | `chwindowsName=` on `MIXER_FADER` |
| `mixergroups` | `MIXER_GROUPS`, `MIXER_GROUP_SET`, `MIXER_GROUP_NUDGE`, `MIXER_GROUP_MUTE`. **Replaces the `MIXER_SLOT*` verbs**, which are gone — a client seeing this token must stop sending them |
| `hotkeydiag` | `DIAG_HOTKEYS` and friends: what is bound globally, and whether this process actually **holds** the combination or another application owns it |
| `mixerfadergroups` | `groups=` on `MIXER_FADER` — which volume-hotkey groups a fader belongs to |
| `displayprofiles` | The name-addressed display profiles: `DISPLAY_PROFILE_LIST` / `_SAVE=` / `_LOAD=` / `_DELETE=` / `_STARTUP[=]` |
| `allfullscreen` | `SET_ALL_FULLSCREEN=` / `GET_ALL_FULLSCREEN`, and `fs=` on `GET_CHILDREN` — the walk-away control and its restore point |
| `idleactive` | `SET_IDLE_ACTIVE=` / `GET_IDLE_ACTIVE` — fire the idle action on demand, independent of the countdown |
| `mirrorwatermark` | `SET_MIRROR_WATERMARK=<0\|1>`, the absolute form. A client gating on this can drive a switch instead of reading, comparing and toggling — which is a race |
| `windowzorder` | `LOWER_WINDOW` exists alongside `RAISE_WINDOW`, so "bring to front" and "send to back" can be offered as a pair |
| `displaywatermark` | `SET_DISPLAY_WATERMARK=` addresses **one** display, and `GET_DISPLAY_WATERMARK` reads it back |
| `displaytiers` | `SET_DISPLAY_MODE=<N>,preset` — a display holds its own preset **in this process**, the middle tier between mirror and child |
| `idleconfig` | The idle timer is configurable from a remote, not just switchable: `SET_IDLE_TIMEOUT=`, `SET_IDLE_ACTION=`, `SET_IDLE_PROFILE=`, `SET_IDLE_FULL_OPACITY=`, `RESET_IDLE_TIMER`, and two more fields on `IDLE_TIMER=` |
| `texcap` | `SET_TEXCAP=` / `GET_TEXCAP` — the longest edge a texture loaded from disk keeps, so a client can offer the control and show the current value |

Three rules the list follows, so that it can be trusted:

- **Append-only.** A shipped name keeps its meaning forever, because clients in
  the wild compare that exact string. Retiring a capability means shipping a new
  name, never redefining an old one.
- A name says this build **answers those verbs and fields**. It says nothing
  about whether the subsystem is switched on or the hardware is present — those
  are runtime questions with runtime answers (`MIXER_ENABLE`, `DIAG_MIXER`).
- **Absence means not declared, not absent.** Only capabilities worth gating on
  are listed; the rest of the surface is discovered from `HELP` as before.

`HELP=JSON` carries the same `protocol` and `features`, for a client already
parsing the index.

`commands=` / `script=` / `signals=` are diagnostics, not a compatibility
signal: a count cannot say *which* commands exist, it moves on any unrelated
table edit, and two different builds can share a total. A test pins them so that
a grown surface fails the build until someone decides whether the protocol
should move with it.

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
| `SET_MESH_SIZE=<gridX>` | Warp mesh width; the height follows at 3/4. Applied on the render thread | 8-192 |
| `SET_INJECT_EFFECT=<0-4>` | Post-process effect: 0 off, 1 brighten, 2 darken, 3 solarize, 4 invert. `F11` cycles the same thing | 0-4 |

### Runaway feedback: canvas, damp and frame caps

A small number of presets look right in a window and wash out to a flat glare on
a big screen. The cause is a feedback loop whose brake is spatial transport
measured in **texels** while its gain is not, so a larger canvas weakens the
brake and the loop diverges. A sweep of 1266 presets found 24 of them.

Two independent settings address it — use either, both or neither — and a third
caps the frame rate for presets that are simply expensive. The full explanation,
including why damp can make a preset *worse* and when to prefer the canvas
limit, is in [`annotations.md`](annotations.md#canvas-and-damp-two-fixes-for-a-runaway-feedback-loop).
These are the verbs that drive it.

**Global ceiling**

| Command | Description |
|---------|-------------|
| `SET_CANVAS_MAX=<px>` | Global feedback-canvas ceiling, long edge in pixels. `0` means no limit |
| `GET_CANVAS_MAX` | The global ceiling, the canvas actually in use, and what this preset asks for |

**Per preset**, keyed by content hash, so the setting follows a preset that is
renamed or moved. Each applies to the preset that is **running now**:

| Command | Description |
|---------|-------------|
| `SET_PRESET_CANVAS_MAX=<px>` | Canvas limit for this preset. Capped by the global ceiling, so a value above it has no effect |
| `PRESET_CANVAS_MAX_CLEAR` | Clear it |
| `SET_PRESET_DAMP=<0..1>` | Feedback damp strength. `0` clears it. At or below a 1024 px long edge the multiplier is exactly 1.0 and this does nothing at all |
| `PRESET_DAMP_CLEAR` | Clear it |
| `SET_PRESET_FPS_MAX=<fps>` | Frame cap for this preset. It only ever **lowers** the active cap, so a value above the user's own cap does nothing; `0` clears it |
| `PRESET_FPS_MAX_CLEAR` | Clear it |
| `GET_PRESET_FPS_MAX` | `PRESET_FPS_MAX=<n>\|active=<n>` — this preset's own cap, and the cap actually in force |

**Measuring and tuning**

| Command | Description |
|---------|-------------|
| `DIAG_CANVAS_METRIC` | Mean luminance of an 8×8 reduction of the presented frame. This is how a runaway is *detected*: a preset heading for a flat glare climbs towards 1.0 |
| `SET_CANVAS_TRACE=<0\|1>` | Write per-frame canvas metrics to `log/diag_canvas_trace.csv` |
| `SET_DAMP_OVERRIDE=<mult\|clear>` | Raw per-frame feedback multiplier, **this session only** — nothing is persisted. For finding a value before committing it with `SET_PRESET_DAMP=` |

A sweep that looks for runaways therefore reads: load the preset, let it settle,
poll `DIAG_CANVAS_METRIC`, and record the ones that climb.

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
| `VFX_SCOPED_STATUS` | `VFX_SCOPED\|active=…\|profile=…\|dirty=…` | Whether a **scoped** edit is in progress, which profile it belongs to, and whether it has unsaved changes |
| `VFX_SCOPED_KEEP=<0\|1>` | — | Answer the Keep/Discard prompt without clicking it. `1` writes the edit into the scoped profile |

A *scoped* edit is one made while a profile is attached to the running preset:
changing a parameter then asks whether to keep it in that profile or throw it
away. `VFX_SCOPED_STATUS` and `VFX_SCOPED_KEEP=` answer that prompt from a
script, which a test needs because the prompt otherwise blocks on a click.

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
| `SPOUT_SENDER=<name>` | Which sender to take input **from**. Stores the name only — `SPOUTINPUT=` or `SIGNAL\|ENABLESPOUTMIX` is what switches the input on, which is the order Milkwave Remote sends them in |

### Video input mixing (IPC)

A webcam, a video file or a Spout sender can be composited under or over the
visualisation. The `SIGNAL|` verbs above switch it on and set it up; this reads
it back:

| Command | Description |
|---------|-------------|
| `GET_INPUT_MIX` | Source (0 none, 1 Spout, 2 webcam, 3 file), webcam device, opacity, whether it is on top, and the luma key |

`source` is the single authoritative flag — a client should read that rather
than inferring from whether a Spout sender name happens to be set. The full
feature is in [`VideoInput.md`](VideoInput.md).

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

This instance's own window. For a *display's* window use the `SET_DISPLAY_*`
verbs, and for a child use the relay — `DISPLAY|<N>|<command>`.

| Command | Description |
|---------|-------------|
| `OPACITY=<0.0-1.0>` | Window opacity |
| `SET_WINDOW=<x>,<y>,<w>,<h>` | Position and size. `w=0,h=0` moves without resizing |
| `SET_BORDERLESS_FS=<0\|1>` | Borderless fullscreen, **absolutely**. `SIGNAL\|BORDERLESS_FS` toggles instead |
| `SET_MINIMIZED=<0\|1>` | Minimise or restore. Clears a screen **without retiring the child that owns it**, so coming back is instant |
| `RAISE_WINDOW` | Bring to the top of the z-order, without taking focus |
| `LOWER_WINDOW` | Send to the bottom, without taking focus. Drops always-on-top first — otherwise the move stays inside the topmost band and nothing visibly happens. Relayable, so `DISPLAY\|<N>\|LOWER_WINDOW` reaches a child |
| `GET_HUD_STATE` | `HUD_STATE\|primaryOnly=…\|presetInfo=…\|fps=…\|debug=…\|child=…` — what the HUD is set to show, so a remote can draw its toggles in the right position instead of guessing |

Prefer the absolute forms (`SET_…=<0|1>`) over the toggles for anything
automated. A toggle forces a client to read the state, compare, and send only if
they differ — which is a race, and the reason the absolute forms exist.

#### Walk-away control

| Command | Description |
|---------|-------------|
| `SET_ALL_FULLSCREEN=<0\|1\|2>` | `1` foregrounds and fullscreens the primary **and every ready child**; `0` puts each back exactly as it was found; `2` leaves them fullscreen regardless of what `0` would have restored |
| `GET_ALL_FULLSCREEN` | Whether the primary, the mirrors and every enabled child are up — read from the **actual windows**, not from what was last requested |

`0` restoring "exactly as found" is the point: it is a restore, not a toggle, so
a screen that was already fullscreen before the call stays that way afterwards.
`2` is the escape hatch for when that memory is wrong or unwanted.

#### Driving the tool windows

The Settings, Displays, Mixer and other tool windows can be opened and operated
over the pipe. This exists so a test can exercise the UI without a human, and so
a remote can put a window where the user is looking.

| Command | Description |
|---------|-------------|
| `UI_SHOW=<window>[\|tab=N][\|ctrl=ID]` | Open a tool window by its **hotkey action name**, optionally at a page and on a control |
| `UI_CLICK=<window>\|ctrl=ID` | Press a control in a tool window |
| `UI_MOVE=<window>\|render` | Move a tool window; `render` centres it on the visualiser's display |
| `UI_CLOSE=<window>` | Close a tool window |
| `UI_LIST` | The tool windows this build has, and which are open |
| `DIAG_WINDOW=<title>` | Every control on one of our tool windows: class, id, rect, visibility, **clipping**, whether the text fits, and its codepoints |
| `CAPTURE_WINDOW=<title>` | A PNG of one tool window, under `capture\` |

**The `UI_*` verbs and the `*_WINDOW` verbs name windows differently**, which is
the thing that trips people up. `UI_*` takes the **hotkey action name** —
`OpenSettings`, `OpenDisplays`, `OpenAnnotations`, `OpenAudioMixer`,
`OpenPresetEditor` and so on; `UI_LIST` prints the full set. `DIAG_WINDOW=` and
`CAPTURE_WINDOW=` take the window **title**, matched case-insensitively on the
whole title before any prefix — `MDropDX12 Settings`. An unknown name comes back
as `UI_ERR|reason=unknown_window`, and an unmatched title as
`WINDOW_CTRLS=NOTFOUND`.

`DIAG_WINDOW` and `CAPTURE_WINDOW` are how a layout is checked without eyes on
the screen. Two failures they separate that look identical in a screenshot: a
control can be in the right place and still have its text cut off (`textfits=0`
with `clip=0`), and the codepoints show a glyph button whose escape was mangled
into the system codepage — `U+E8E5` arriving as four characters — which
otherwise just renders as *something*.

`CAPTURE_WINDOW` is taken **inside** the process with `PW_RENDERFULLCONTENT`.
From another process `PrintWindow` does not render owner-drawn buttons at all,
so every glyph button comes back as flat background and a broken one looks
exactly like a working one. Use `CAPTURE` for the render output and this for the
windows.

### Idle Timer (IPC)

The walk-away screensaver: after a few minutes with no input the visualizer puts
itself up, and on input it puts everything back.

| Command | Response | Description |
|---------|----------|-------------|
| `GET_IDLE_TIMER` | `IDLE_TIMER=<enabled>\|<timeoutMin>\|<action>\|<profile>\|<fullOpacity>` | The whole configuration in one call |
| `SET_IDLE_TIMER=<0\|1>` | `IDLE_TIMER=…` | Enable or disable the automatic countdown. Disabling while idle is on screen restores first, so nothing is stranded |
| `SET_IDLE_TIMEOUT=<1-60>` | `IDLE_TIMER=…` | Minutes of no input before the action fires |
| `SET_IDLE_ACTION=<0-3>` | `IDLE_TIMER=…` | `0` Fullscreen, `1` Stretch/Mirror, `2` Mirror all, `3` Load display profile |
| `SET_IDLE_PROFILE=<name>` | `IDLE_TIMER=…`, or `IDLE_PROFILE=ERROR\|<name>` | The display profile action 3 loads. An empty value clears it. A name that does not exist is **refused now** rather than failing silently hours later with nobody watching |
| `SET_IDLE_FULL_OPACITY=<0\|1>` | `IDLE_TIMER=…` | Take **every** display to 100% opacity while idle, whichever action is set |
| `RESET_IDLE_TIMER` | `IDLE_TIMER_RESET=1` | Restart the countdown |
| `SET_IDLE_ACTIVE=<0\|1>` | `IDLE_ACTIVE=<0\|1>` | Fire the idle action **now**, or restore. Independent of `SET_IDLE_TIMER=` — whether the countdown runs on its own says nothing about whether idle can be asked for |
| `GET_IDLE_ACTIVE` | `IDLE_ACTIVE=<0\|1>` | Whether the idle action is on screen right now |

The last two fields of `IDLE_TIMER=` were **appended**, so a client that reads
only the first three keeps working unchanged.

`RESET_IDLE_TIMER` is "pretend the user just did something", not a separate
concept. The countdown is normally the system-wide last-input age, which no
application can move, so this supplies a second baseline and the shorter of the
two wins. Because real input would also *wake* an active idle session, so does
this — when `AutoRestore` is on.

**Action 3 does not reload a profile that is already loaded.** If the configured
profile is the one `GET_DISPLAY_PROFILE` reports, idle changes only the opacity
and the z-order. That matters: loading a profile disables every output,
reapplies the whole state and reconciles children, so a needless reload would
tear down and respawn a child process purely to brighten screens that were
already arranged correctly.

Opacity and z-order are transient and never reach `displays.json`. The display
arrangement is not transient, so before loading a profile idle banks the current
state to `_idle_restore.json` in the profile folder and reloads it on wake — and
if the app is killed while idle is up, that file is loaded at the next start, in
preference to the startup profile. Files whose name begins with `_` never appear
in the profile lists; they are bookkeeping, not profiles anybody chose.

### Messages (IPC)

The `MSG|` command sends a styled text overlay. Fields are pipe-delimited key=value pairs:

```
MSG|text=Hello World|font=Segoe UI|size=30|r=255|g=255|b=255|time=5|x=0.5|y=0.5
```

See the [Messages](#messages) section above for the full list of MSG parameters (text, font, size, r, g, b, time, growth, x, y, startx, starty, movetime, easemode, easefactor, shadowoffset, burntime, fade, fadeout, bold, ital, randx, randy, randr, randg, randb, box_alpha, box_col, box_left, box_right, box_top, box_bottom, profile).

`MSG|` shows one message. These control the **autoplay** set — the messages the
app shows on its own from `messages.ini`:

| Command | Description |
|---------|-------------|
| `SET_MESSAGES=<0\|1>` | Enable or disable automatic message playback |
| `GET_MESSAGES` | Whether it is enabled |
| `TOGGLE_MESSAGES` | Toggle it |
| `DIAG_MESSAGES` | Every term of the autoplay guard — `enabled`, `testing`, `autoplay`, `count`, `nextAt`, `now`, `interval`, `jitter`, `maxOnScreen`, `sequential` — then one `stN=` block per live message |

Use `DIAG_MESSAGES` when messages stop appearing: `nextAt` far ahead of `now`
means the clock moved backwards, and an `stN` with a `start` in the future is a
leaked slot.

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
| `GET_PRESET_STATUS` | One message with the preset and load status: file, description, loading, ready, Shadertoy buffers |
| `GET_COVER_STATUS` | Whether the track-artwork sprite is on screen, and which image file it is |
| `GET_NUMERIC_MODE` | Whether typing a digit selects a **sprite** or a **custom message** — the same digits mean different things in the two modes, so a remote drawing a keypad has to know which |

#### Process and session control

| Command | Description |
|---------|-------------|
| `SHUTDOWN` | Clean shutdown: saves settings, stops the render thread, exits |
| `RESTART_DEVICE` | Recreate the D3D12 device **in place** — the path a driver update, a device reset or an adapter change takes. A real TDR does **not** come this way: since v3.2 a `DXGI_ERROR_DEVICE_HUNG` relaunches the process instead, because the adapter refuses the process that hung it for about two minutes afterwards. `PerformDeviceRecovery` routes a hang away before any rebuild is attempted. To exercise *that* path, see `DIAG_SIMULATE_TDR` |
| `TESTING_MODE[=<0\|1>]` | Freeze automatic preset changes and the idle timer, and ignore the keyboard. Bare form queries. **Never persisted** |
| `SET_LOGLEVEL=<0-4>` / `GET_LOGLEVEL` | 0 Off, 1 Error, 2 Warn, 3 Info, 4 Verbose. The getter also reports the log directory |
| `CLEAR_LOGS` | Delete everything under `log/` and reopen `debug.log` |
| `CONFIG_EXPORT_INI` | Copy registry-held settings back into `settings.ini` and friends |
| `SET_PIPE_NAME=<base>` | **Testing only.** Serve the pipe under another namespace so a harness instance is distinguishable from the real one |

**`TESTING_MODE=1` is the switch to raise before driving this app in anger.** It
stops the frame changing underneath a measurement, keeps a run out of
`presets.json`, and stops tool windows saving the positions a test left them in.
Raise it once at the start of a session and lower it at the end — cycling it
around each individual step leaves the app in the normal state in between, which
is exactly where a stray write gets in.

#### Missing textures

| Command | Description |
|---------|-------------|
| `SET_FALLBACK_TEX=<0-5>[\|<path>]` | What a preset gets when a texture it names is not found: `0` hue, `1` white, `2` black, `3` and `4` random, `5` a custom file |
| `GET_FALLBACK_TEX` | The current style, file and SRV index |

White is the multiplicative identity, which is why it is the safe default: a
missing texture that falls back to black turns whatever it multiplies into a
black screen, and that reads as a broken preset rather than a missing file.

#### Texture size cap

| Command | Description |
|---------|-------------|
| `SET_TEXCAP=<0\|64-16384>` | Longest edge a texture loaded from disk keeps; `0` removes the cap. Default 1024. Reply `TEXCAP=<n>`. Applies to the **next** load, not to textures already resident — step a preset, or let the cache evict. Persisted as `MaxTextureDim` |
| `GET_TEXCAP` | Read the cap back. Reply `TEXCAP=<n>` |

A value between 1 and 63 is raised to 64 and anything above 16384 is clamped;
the reply carries the value actually in force.

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

### Preset annotations: ratings, flags, tags and notes

`presets.json` records what you have learned about a preset — a rating, flags,
tags, a note, how often it has played and for how long, plus any shader errors
captured automatically. It is keyed by a **hash of the preset's normalised
body**, not by filename, so everything here follows a preset that is renamed or
moved. The database, the flags and the duplicate handling are described in
[`annotations.md`](annotations.md).

Two addressing forms. The plain verbs act on the preset that is **running now**;
the `_FOR=` verbs take a content hash and act on any preset in the database,
running or not.

| Command | Description |
|---------|-------------|
| `SET_PRESET_RATING=<0-5>` / `SET_PRESET_RATING_FOR=<hash>,<0-5>` | Rate a preset. `0` clears the rating |
| `SET_PRESET_FLAG=<name>,<0\|1>` / `SET_PRESET_FLAG_FOR=<hash>,…` | Set or clear one flag: `favorite`, `error`, `skip`, `broken`, `canvas` |
| `SET_PRESET_TAG_FOR=<hash>,<name>,<0\|1>` | Add or remove a tag. **Hash-addressed only** — there is no plain `SET_PRESET_TAG=` form |
| `SET_PRESET_NOTE=<text>` / `SET_PRESET_NOTE_FOR=<hash>,<text>` | Set the note |
| `GET_PRESET_NOTE` / `GET_PRESET_NOTE_FOR=<hash>` | Read the note; empty if it has none |
| `GET_PRESET_TAGS` | Every tag in use, with counts |
| `GET_PRESET_FOLDERS` | Folders presets live in, with counts. A preset in two places counts in both |

Nothing is recorded while **testing mode** is on, and presets under a folder
named `TEST` never get an entry at all. Both gates block *writes* only — reads
always work — so a measurement run cannot make itself the "most played".

#### Browsing the database

| Command | Description |
|---------|-------------|
| `BROWSE_PRESETS=<offset>,<count>[,q=][,folder=][,tag=][,flag=][,minrating=][,sort=name\|rating\|used\|time\|lastused]` | Page the database. One `PRESET_ROW\|` per entry, then `PRESET_ROWS_END` carrying the **filtered** total |

`BROWSE_PRESETS` is what a remote's preset browser is built on: it filters and
sorts server-side, so a client never holds the whole library. The terminator
carries the filtered total rather than the database size, which is what a pager
needs to draw its scrollbar.

#### Per-preset overrides

A preset can pin a shader override or a VFX profile to itself, overriding
whatever its tags would otherwise select. See
[`custom_shaders.md`](custom_shaders.md) for the override system itself.

| Command | Description |
|---------|-------------|
| `PRESET_OVERRIDE_STATUS` | What each slot resolved to, and **whether a rule or the preset won** |
| `SET_PRESET_SHADER_OVERRIDE=<name>` | Pin a shader override to this preset. An empty value means *explicitly none* |
| `SET_PRESET_VFX_PROFILE=<name>` | Pin a VFX profile to this preset. An empty value means *explicitly none* |
| `CLEAR_PRESET_OVERRIDE=<shader\|vfx>` | Remove the slot **entirely**, so tag rules apply again |

Note the three-state distinction, which is the thing that catches people out:
setting a slot **empty** means "this preset uses none", while `CLEAR_` removes
the slot so the tag rules decide. They are not the same.

#### Keeping the database honest

| Command | Description |
|---------|-------------|
| `DIAG_ANNOT_MISSING` | Count entries whose preset file cannot be found |
| `ANNOT_REMOVE_MISSING` | Purge those entries |
| `DIAG_ANNOT_RESOLVE=<file>` | Where an entry's preset actually lives; an empty path means gone |
| `DIAG_ANNOT_QUERY=<pattern>` | Count entries matching the Annotations window's own search predicate — so a test can check the search without driving the UI |
| `DIAG_ANNOT_REPAIR` | Drop alias paths that hash to a **different** preset than the entry holding them |

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

### Diagnostics

The `DIAG_*` family reports what the engine is **actually** doing, as against
what it was asked to do. They are the difference between a bug report that says
"it looked wrong" and one somebody can act on, and most of them exist because a
specific wrong belief cost a debugging session.

| Command | What it answers |
|---------|-----------------|
| `DIAG_MIRRORS` | The rich per-display dump: size, opacity, click-through, watermark, always-on-top, which **render path** each display took (`orient` / `stretchMain` / `letterbox` / `copy` / `black`), draw and present counts, and back-buffer states. Start here for anything about displays |
| `DIAG_CHILDREN` | Child-worker health: `worker=`, `event=`, `job=`, `queued=`, `children=`, `pending=`, `failed=` |
| `DIAG_LADDER[=<0\|1>]` | The distress ladder and its rung: 1 adaptive frame rate, 2 cheapest copy mirrors. Reports whether the frame is GPU-bound and the rate rung 1 is tracking |
| `DIAG_GPU` | This process's GPU and VRAM cost, keyed by pid. Asking turns the sampler on for about 8s, and because the counter is a rate the **first** reply after a quiet period carries `gpu3d=-1` — poll twice about a second apart and read the second |
| `DIAG_GPUTIME=<0\|1>[,<ms>]` | Per-stage GPU timing; logs frames where a stage exceeds `<ms>` (default 50). Averages over a frame hide the single draw that kills a device |
| `DIAG_VRAMMARK=<0\|1>` | Local VRAM at each allocating checkpoint plus the delta since the previous mark, so a jump can be attributed to the call that caused it. The trail goes to `log/debug.log` as `VMARK` lines, not down the pipe; the reply is only `VRAMMARK=<0\|1>`. Off by default — `QueryVideoMemoryInfo` is a real call and some marks sit in per-surface paths. Enabling re-baselines, so the first mark of a run reports a level rather than a delta |
| `DIAG_CONFIG[=flush]` | Settings-layer counters; `=flush` writes the cache out first |
| `DIAG_UI_MODE` | Which full-frame overlay is up (regular / menu / load / settings …) and the settings cursor |
| `DIAG_MESSAGES` | Every term of the autoplay message guard — see [Messages](#messages-ipc) |
| `GET_RENDER_DIAG` | Rendering values (blur scales and biases), for cross-engine comparison |
| `DIAG_BINDINGS` | Side-by-side SRV binding slots for the primary and the mirror |
| `DIAG_DISPLAY_MODE=<0\|1\|2>` | Show the raw `VS[0]` or `VS[1]` render target instead of the composed frame |
| `DIAG_CANVAS_METRIC` | Mean luminance of the presented frame — see [Runaway feedback](#runaway-feedback-canvas-damp-and-frame-caps) |
| `GET_EEL_STATE [megabuf=S,C] [gmegabuf=S,C] [reg=S,C]` | Dump EEL `megabuf`, `gmegabuf` and `reg` values |
| `TEST_EEL_ISOLATION` | Self-test that per-context EEL storage really is isolated |
| `DUMP_SHADER` | Write the running shader sources to files under `log/` |

**Hotkeys.** A binding can be configured and still not work, because another
application owns the combination system-wide. These tell the two apart:

| Command | What it answers |
|---------|-----------------|
| `DIAG_HOTKEYS` | One `HOTKEY\|` line per **global** binding, then `HOTKEYS_END`. `held=0` means another application owns that combination — the binding is correct and inert |
| `DIAG_HOTKEY_SCAN` | One line per candidate combination, then `HOTKEY_SCAN_END`; status is `free` / `taken` / `ours`. For finding somewhere to put a new binding |
| `DIAG_HOTKEY_BIND=<action>,<mod>,<vk>[,<scope>]` | Set a binding — scope `1` (default) global and re-registered, `0` local. **In memory only**, never written to `hotkeys.json`. `vk=0` unbinds. This is how a test drives a new action without pressing a key |
| `DIAG_HOTKEY_CONFLICTS=<action>,<mod>,<vk>,<scope>` | What assigning that combination *would* unbind, before doing it |

**Settings that two paths can disagree about.** Each of these exists because a
preference and a live value were once the same variable, and changing one
silently changed the other:

| Command | What it answers |
|---------|-----------------|
| `DIAG_GLOBAL_PRESET_CYCLE` | The cycle interval the shutdown save *would* write — which loading a display profile must not change |
| `DIAG_STARTUP_PRESET_LOCK` | The "Preset Lock on Startup" preference — which toggling the live lock must not change |
| `DIAG_BATTERY` | HUD headset battery: toggle, percent, and which device it came from |
| `DIAG_BATTERY_SIM=<0-100\|off>` | Force the HUD battery figure so its colour bands can be seen. In memory only |

> **Two device-loss simulations, and they exercise different paths.**
>
> `DIAG_SIMULATE_DEVICE_REMOVED` calls `ID3D12Device5::RemoveDevice`, which
> raises `DXGI_ERROR_DEVICE_REMOVED` — the **in-place** recovery: the device is
> destroyed and rebuilt without the application exiting, the same path a driver
> update or an adapter change takes. It removes only **this process's** device
> object, so nothing else on the machine is affected.
>
> `DIAG_SIMULATE_TDR` drives the other path — the one a `DEVICE_HUNG` takes,
> where the process relaunches itself. Testing mode only.
>
> Neither is a real TDR, and that is the point of having them. **A real TDR is
> machine-wide**: it resets the display driver and takes down every other
> visualizer, game and GPU-accelerated window running at the time, so inducing
> one is expensive and is not the way to iterate on the recovery logic. This
> warning used to sit on `DIAG_SIMULATE_DEVICE_REMOVED`, which is not
> machine-wide and does not produce a TDR; it belongs here.

### Recipes

Short end-to-end sequences for the things the newer commands were built for.
Each line is one pipe message.

**Set up a walk-away screensaver.** After ten idle minutes, load a saved screen
arrangement and take every display to full brightness:

```text
SET_IDLE_ACTION=3
SET_IDLE_PROFILE=evening
SET_IDLE_FULL_OPACITY=1
SET_IDLE_TIMEOUT=10
SET_IDLE_TIMER=1
GET_IDLE_TIMER
```

Try it without waiting ten minutes — `SET_IDLE_ACTIVE=1` fires the action now
and `SET_IDLE_ACTIVE=0` puts everything back. `RESET_IDLE_TIMER` restarts the
countdown, and wakes an active session exactly as a keypress would.

If the profile you name is the one already loaded, idle skips the load and only
changes opacity and z-order — a reload would tear down and respawn any display
running in its own process, for screens that were already arranged correctly.

**Give one screen its own preset, then bank the arrangement.** The middle tier
(`preset`) holds its own preset without a second process — prefer it to `child`:

```text
SET_DISPLAY_MODE=2,preset
SET_DISPLAY_PRESET_DIR=2,C:\Presets\ambient
SET_DISPLAY_TIME_BETWEEN=2,45
SET_DISPLAY_ORDER=2,1
GET_DISPLAY_MODE
DISPLAY_PROFILE_SAVE=three-screens
```

`DISPLAY_PROFILE_LOAD=three-screens` puts it back later. A profile is a
**complete** state, so loading one turns off any display it does not mention.

**Find presets that wash out to white.** A runaway climbs towards a flat glare,
which `DIAG_CANVAS_METRIC` measures directly:

```text
TESTING_MODE=1
PRESET=C:\Presets\suspect.milk
GET_PRESET_STATUS           … poll until ready=1
DIAG_CANVAS_METRIC          … wait a few seconds, then again
DIAG_CANVAS_METRIC
SET_PRESET_CANVAS_MAX=1024  … if it climbed
TESTING_MODE=0
```

Poll `GET_PRESET_STATUS` rather than sleeping a fixed time: the machine a
runaway matters on is the slow one, where a guessed delay is wrong. (Inside a
**script file** the same wait is one word, `WAITREADY` — but that only holds a
*playing* script, so over the pipe it does nothing and the poll is the way.)

`TESTING_MODE=1` matters here: without it the preset advances underneath the
measurement, and the run writes itself into `presets.json` as the most-played
preset in the library.

**Check a window's layout without looking at it.** The reply says which controls
are clipped, and which have text that does not fit — two different faults that a
screenshot cannot tell apart:

```text
UI_SHOW=OpenDisplays
DIAG_WINDOW=MDropDX12 Displays
CAPTURE_WINDOW=MDropDX12 Displays
UI_CLOSE=OpenDisplays
```

**Put every screen up for a performance, then restore.** `0` restores each
window exactly as it was found, so a screen that was already fullscreen stays
that way:

```text
SET_ALL_FULLSCREEN=1
… later …
SET_ALL_FULLSCREEN=0
```

### Fallback

Any command not recognized by the IPC handler is passed to the script engine (`ExecuteScriptLine`). This means all script commands (NEXT, PREV, LOCK, RAND, ACTION=, LAUNCH=, etc.) work seamlessly over IPC.

This is deliberate and Milkwave Remote depends on it, so an unrecognised keyword
is never reported as an error. It also means a new IPC keyword must not
prefix-match an existing script command, or that script command becomes
unreachable - `HELP` claims `VERSION` only as an exact match for this reason.

#### STRICT

A token that **nothing** recognises - neither the IPC handler nor the script
engine - is drawn on the visualizer as message text. That is right for a
person typing at Milkwave Remote and wrong for a program: a probe that sent
`LOGLEVEL=4` (the keyword is `SET_LOGLEVEL=`) painted its own typo across the
frame it was about to capture.

`STRICT` makes such a token an error instead:

| command | reply | meaning |
|---|---|---|
| `STRICT` | `STRICT=<0\|1>` | what this connection is set to |
| `STRICT=1` \| `ON` \| `TRUE` \| `YES` | `STRICT=1` | report unknown tokens |
| `STRICT=0` \| `OFF` \| `FALSE` \| `NO` | `STRICT=0` | draw them, as before |
| anything else | `ERR\|STRICT\|<value>\|expected …` | refused, state unchanged |

An unknown token on a strict connection answers `ERR|UNKNOWN|<token>` and is
not displayed.

Three properties, and they are what make this safe to have at all:

- **Per connection.** It belongs to one pipe client for the life of its
  connection. A client that never mentions `STRICT` is never affected, which
  is why Milkwave Remote cannot notice this exists.
- **Never permanent.** It dies with the connection - there is no setting, no
  ini key, and no way for a crashed test to leave the app rejecting a real
  client's commands. Note that a client which reconnects per message (the probe
  harness does) cannot use `STRICT=` at all; for those, only the default below
  applies.
- **Sticky while the connection lives.** `STRICT=OFF` stays off until that same
  client turns it back on.

**`-child` and `-test` instances start strict.** Their only callers are a parent
process and a test harness, neither of which is served by having its mistake
rendered. Everything else starts lenient. The exe-name convention counts: a copy
called `MDropDX12_test.exe` is a test instance whether or not `-test` was given.

Strictness applies only to the *final* fallback. Every script command - `SEND=`,
`NEXT`, `LOCK` and the rest - still works on a strict connection, because those
are recognised; "the IPC handler did not match it" is not the same as "nothing
recognises it".
