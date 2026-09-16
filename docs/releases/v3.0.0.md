# MDropDX12 v3.0.0

**Every screen gets its own preset.**

That is the release. A second monitor used to be a mirror — the same picture as
your main window, at another size. Now each display can hold its own preset,
its own folder, its own cycle, on its own clock, and advance independently of
every other screen.

Special thanks to [IkeC](https://github.com/IkeC) for
[Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer, and the
collaboration that keeps both projects honest. Thanks to
[Incubo_](https://github.com/OfficialIncubo) for
[BeatDrop](https://github.com/OfficialIncubo/BeatDrop-Music-Visualizer) testing
and comparison reports.

## Per-display presets

Set a display to **Own preset** on the Displays tab. It gets its own preset
folder, startup preset, cycle interval and order. Pressing Next moves each
screen on to *its* next preset rather than putting the same one everywhere.

**And it runs at the frame rate you asked for.** Earlier development builds did
this by launching a second copy of MDropDX12 per display, which worked and cost
dearly: Windows holds a window that is not focused to about 66 frames a second,
so every screen except the one you were looking at ran at a third of the rate
you set. Frame rate in this program is a creative control, not a detail. Each
display is now rendered by a thread inside the one process — which Windows
treats as part of the foreground application — so they all run at your rate.

It is also about 140 MB lighter per extra screen and switches instantly.

**Save Display Snapshot** (an unbound hotkey) banks what every screen is playing
as a timestamped profile; loading it puts the same presets back on the same
displays. Two more hotkeys step through saved profiles.

## Audio Mixer

A new window, from Settings → Tools. Every playback and capture device on the
machine with a fader and a mute, renameable, hideable, and remembered. Eight
unbound hotkey actions drive two named groups so one key can move a chosen set
of faders.

**SteelSeries Sonar channels get both of their levels** — what you hear and what
a stream hears, adjustable independently. Windows cannot do this at all, because
Sonar's virtual devices ignore the system volume entirely.

**Optional audio failover**: a route can move to another device when the one it
is on disappears — a headset going flat mid-session. Off unless you turn it on,
only ever picks a device you listed, and backs out if the original returns.

**Your headset's battery beside the FPS**, read from the device you are actually
listening on rather than whatever Windows calls the default output.

## .milk2 blend patterns

A `.milk2` file is two presets frozen at a blend, and its named pattern decides
the shape of the boundary. Most of those patterns were previously
approximations, which is why some files looked right and others did not.

**All thirty are now correct.** Frozen `.milk2` files also render identically on
every load — several patterns used to draw a different boundary each time.

## Importing shaders from the command line

Two converters ship in `tools/`. Python standard library only — nothing to
install.

```
python tools/glslsandbox_to_milk3.py https://glslsandbox.com/e#109677.0

python tools/milk3_shadered.py to-shadered resources/presets/mine.milk3 --zip
python tools/milk3_shadered.py to-milk3   project.zip -o mine.milk3
```

The first fetches a [GLSL Sandbox](https://glslsandbox.com) effect and writes a
`.milk3`. The second converts a preset out to a [SHADERed](https://shadered.org)
project and back, so you can work on a shader in an IDE with a real debugger and
bring the result home — multi-pass presets survive the trip with their channel
wiring intact.

See [docs/GLSL_importing.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/GLSL_importing.md).

## Also in this release

- **A black screen is never the answer.** A fresh install with no preset library
  showed a black window forever, indistinguishable from a crash. It now comes up
  on a built-in preset that says where to add your own.
- **Hotkeys with Alt in them work.** Alt+B, Ctrl+Alt+P, F10 — none of them fired
  when the visualizer had the focus.
- **A window listing which global hotkey combinations are actually free**, so a
  collision with another program is visible before you bind it.
- **Settings move out of `settings.ini`** into small JSON files under
  `resources/` — displays, windows, hotkeys, the mixer, and one file per
  profile. Migrated on first run; nothing to do.
- **The remote PIN is now actually checked**, and is salted and hashed rather
  than stored in the clear. A phone with the wrong PIN saved will now be told so
  instead of connecting anyway.
- **A running copy describes its own IPC command set** — `HELP`, `HELP=<cmd>`,
  `HELP=JSON` — and `GET_VERSION` reports a protocol number and feature list so
  a remote can tell what a PC supports before asking.
- **Messages and automatic preset changes no longer stop partway through a long
  session.** Pressing Ctrl+T anywhere on the machine reset the internal clock and
  stranded everything waiting on it.

## Related apps

- **[Milkwave](https://github.com/IkeC/Milkwave)** — Windows companion app with
  Remote control, wave manipulation, messaging and more
- **[MilkRemote](https://github.com/shanevbg/MilkRemote)** — Android remote for
  MDropDX12 ([latest APK](https://github.com/shanevbg/MilkRemote/releases/latest))

## Installation

Download the portable zip below, extract to any folder with write access, and
run `MDropDX12.exe`. No installer, no admin rights, no VC++ Redistributable.

Press **F8** for Settings, **F1** for keyboard shortcuts, **Ctrl+F8** for
Displays.

The zip ships the engine only. Point it at your preset library from
Settings → Files.

## Full changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md)
for the complete list across all releases.
