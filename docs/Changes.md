# MDropDX12 Changelog

## v3.1.0 (2026-09-16)

**Video memory stopped growing without bound.** A long unattended session used
to exhaust the card and close the visualiser — which looked like a clean exit
and was really a leak arriving. Four separate causes, each only visible once
the one before it was gone.

- **The texture cache now evicts.** Both of its limits were read at startup,
  written back at shutdown, and compared against nothing in between; the
  eviction function had no callers at all. Fifty presets took the cache from
  2 MB to 3,228 MB against a configured 2,000 MB, and a 1,270-preset sweep
  reached 13.4 GB before the low-VRAM guard closed the app.
- A texture is never evicted while a shader that could draw next frame still
  has it bound. Age alone is a poor proxy for "unused": a `.milk2` keeps two
  presets resident, and a display running its own preset is on its own
  timeline.
- **Texture sizes are measured from the graphics device**, not from
  width x height x 4. The old figure counted the base image and ignored the
  mip chain, so `MaxBytes=2000` was really holding about 2.8 GB. The plateau
  fell by 794 MB, and it explained a long-unaccounted ~750 MB that had been
  attributed to several other things.
- **Textures loaded from disk are capped at 1024 pixels on the longest edge**,
  downscaled on load. A 4000-pixel photo sampled onto a mesh costs 16x the
  memory of a 1024 version and looks the same. Raisable over IPC. A real
  library's plateau moved from ~3,100 MB to ~1,455 MB.
- **Shadertoy feedback buffers are allocated on demand and given back.** A
  `.milk3` can declare four; the engine allocated all four, in 32-bit float,
  per rendering surface, from startup, for the life of the process — 128 bytes
  a pixel per surface. Now only the ones the preset declares, released after a
  debounced delay with no `.milk3` on screen.
- **The descriptor heap holds an independent mirror on every display.** Exactly
  two fitted before; the third failed 76 times with "heap full" and drew the
  missing-texture placeholder. Retired displays hand their reservations back.
- Mirror swap chains size themselves from the VRAM budget rather than a fixed
  number.

**A lost graphics device is recovered from, not fatal.** A GPU timeout used to
close the visualiser. It now rebuilds and carries on — the intended design; the
exit was a regression.

- Sprite textures are retired on a fence instead of being freed mid-frame while
  a submitted command list still names them.
- Loading a preset no longer frees GPU state a display's render list is using.
- A mirror's own snapshot is no longer freed mid-frame.
- A device removal now reports which draw call hung and which surface submitted
  it, instead of a generic error.
- **A per-preset frame-rate ceiling**, so one expensive preset lowers its own
  rate rather than running the machine into a timeout. The heaviest preset in
  the test library costs 26-31.8 ms of GPU a frame against 75-130 us for an
  ordinary one.

**Displays rendering their own presets stopped writing into each other.** The
composite mesh, the warp and composite constants, the blur passes, the texture
bindings and the `.milk2` blend field were engine-wide and are now per display.
The visible symptom was "presets are rendering but they're incorrect", which is
what a shader reading another preset's textures looks like.

- Two displays each set to their own preset no longer render the *same* preset.
  Their reservations in the descriptor heap could overlap, and whichever wrote
  last won — while each display's own bookkeeping stayed entirely correct,
  which is what made it hard to see.
- The Resource Viewer and the cache diagnostic read a snapshot the render
  thread publishes, rather than walking engine state from a window thread while
  it is being rewritten.

**Separate processes per display are gone, and that was the performance win.**
The belief that heavy presets should be isolated into their own process had
survived weeks on plausible reasoning and no measurement. Measured, it was
backwards: Windows holds an unfocused window to about 66 fps, so an isolated
display ran at a third of the requested rate however much hardware was idle.

- Three independent simulations in one process cost +100 MB of VRAM (0.66% of a
  15 GB budget) and 75-130 us of GPU per surface. Moving the heaviest preset
  back in-process took the side displays from 15.4 and 15.8 fps to 26.1, GPU
  per surface down 3.6x. See `docs/multidisplay_cost.md`.
- **Adaptive frame-rate balancing** replaces automatic isolation: a short ladder
  that lowers the cap, then falls back to the cheapest copy that fits.
- Starvation must be sustained rather than sampled — one slow frame is not a
  problem.
- The ladder does not react to a rate Windows imposed. A backgrounded window at
  22 fps with the GPU at 20% is unfocused, not distressed, and treating it as
  starvation made it worse.
- Ladder triggers scale with the frame rate you asked for instead of assuming
  60.
- The engine no longer holds its mirror lock across presenting a frame, and
  mirrors are paced on measured GPU time rather than wall clock.

**A setting stays where you put it.** A loaded display profile used to *lend*
its main-window settings — preset order, cycle interval — to the session,
keeping your own values hidden underneath to be restored at shutdown. Different
parts of the program then disagreed about which value was real: saving a profile
recorded one, shutting down recorded the other, and only some of the ways you
can change the setting cleared the loan.

- Loading a profile now changes the setting like any other change, and saving
  records what is actually in use.
- **Keep on top** for a display is applied when set, rather than stored and
  ignored until toggled off and on.
- **Next preset** no longer skips displays holding their own preset.
- The display loader no longer drops per-display settings, destroying them.

**Presets that would not load before.** Sampler declaration lists,
`sampler_rand` in all four spellings, `tex1D`, preset-declared sampler
addressing, struct member defaults and preset-level globals.

- DDS and TGA files that Windows' own imaging component refuses to decode.
- Textures kept in a folder *beside* the preset folder, an older MilkDrop
  convention this engine had stopped looking in.
- Random textures with a prefix reach your own collection, not only the bundled
  set.
- The shader cache no longer follows the working directory.
- A blank startup screen with an elapsed-second counter, so a slow first load
  looks like work rather than a hang.
- Startup no longer reports an empty preset directory that has 933 files in it.

**The zip now comes with something to look at.** 93 community MilkDrop presets
and 25 photographs, so a fresh install does something on first run instead of
asking you to go and find a preset library.

- Classic-lineage `.milk` presets — Flexi, Geiss, Se7enSlasher, martin,
  fiShbRaiN, Stahlregen, Rovastar, Cope, Zylot, Aderrasi, Krash and others,
  credited in full in `resources/CREDITS.md`.
- No Shadertoy or GLSL Sandbox conversions. Those carry their original authors'
  terms and some forbid redistribution outright.
- No preset in the bundle needs a texture that cannot ship: each uses either no
  image or the `randNN` slots, which the bundled photographs fill.
- The photographs are the author's own, downscaled to 1024 pixels on the longest
  edge — the engine's new default cap, past which the detail is invisible once
  an image is sampled onto a warp mesh.
- Nothing in it is required. Point the app at your own library from
  Settings → Files, and it still starts on a preset compiled into the exe.

**Scripts can drive a test and report on it**, including sweeping a library and
reporting which presets failed to compile. `GET_WINDOW` / `GET_WINDOWS` report
where the program's windows are and tell the render windows apart.

**Mirror watermark survives being used.** It worked exactly once per session:
turning it off and back on left every panel frozen on the single frame the
first draw managed, and on some machines the toggle took the GPU down with it
and closed the visualiser. Both are fixed, along with the stale frames that
showed up as tearing on a mirrored display.

- Mirror watermark can be turned off and on as often as you like. A display
  whose simulation had been torn down never restarted it, so the panel froze
  on its first frame and stayed there.
- Toggling mirror watermark no longer removes the graphics device. The
  visualiser detected the loss and exited cleanly, which looked like it had
  simply vanished.
- Mirrored displays no longer show bands of an older frame after a resize or
  a display-profile load. The wipe that clears them had not run since August.
- **Delete a display profile** from the Displays window. Profiles could be
  saved but only removed by finding the folder by hand. Deleted profiles go to
  the Recycle Bin, and the startup profile is cleared if it was the one
  deleted.
- Loading a display profile no longer changes what Alt-S does. Setting Alt-S
  to "Load profile" and pressing it put the setting back to whatever the
  profile was saved with, so the choice could never survive being used.
- A display profile no longer brings back every own-preset display as a
  mirror.

**Walking away can set the whole room up.** The idle timer could put one window
fullscreen or turn the mirrors on, and that was the extent of it — so a setup
left in watermark mode stayed dim, and screens nobody was looking through any
more went on being see-through.

- **The idle timer can load a display profile.** A fourth action, with the
  profile chosen from a dropdown beside it, so the walk-away state can be a
  whole arrangement of screens — which display plays what, in which mode — and
  not just a window going fullscreen.
- **All displays to 100% opacity**, as a separate option that applies with any
  of the four actions. It reaches displays running in their own process too,
  which the existing mirror-opacity control never did: those kept whatever
  they were started with.
- Idle also brings the screens to the front, and puts the z-order back on wake.
- **The same profile is not reloaded.** If the profile idle wants is the one
  already loaded, only the opacity and the z-order change. Reloading would have
  torn down and respawned a display's own process purely to brighten screens
  that were already arranged correctly.
- **The idle timer is configurable from a remote**: timeout, action, profile
  and the opacity option, plus `RESET_IDLE_TIMER` to restart the countdown.
  Previously only the on/off switch was reachable.
- Turning the idle timer off from the Settings window no longer strands what it
  had put on screen. Unticking the box forgot that idle was active without
  undoing it, so the fullscreen and the mirrors stayed up with nothing left
  that knew how to take them down.
- Changing the Action while idle is on screen no longer restores through the
  wrong branch. Restore now follows the action that actually fired.
- With the action set to Fullscreen and the window already fullscreen, the idle
  timer no longer re-fires every second for as long as you are away.
- A crash while the idle timer had a profile applied no longer leaves that
  profile as your normal setup: the previous state is banked first and restored
  at the next start.

## v3.0.0 (2026-09-09)

**Every screen gets its own preset.** That is the release in one line, and
almost everything else here follows from it or was found on the way.

Special thanks to [IkeC](https://github.com/IkeC) for
[Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer, and
the collaboration that keeps both projects honest. Thanks to
[Incubo_](https://github.com/OfficialIncubo) for
[BeatDrop](https://github.com/OfficialIncubo/BeatDrop-Music-Visualizer) testing
and comparison reports.

### Every screen its own preset

- **Each display can run its own preset, on its own clock.** Set a display to
  **Own preset** on the Displays tab and it gets its own preset folder, its own
  startup preset, its own cycle interval and its own order — advancing
  independently of the main window and of every other screen. Pressing Next
  moves each display on to *its* next preset rather than putting the same one
  on all of them. Previously an extra display could only mirror the main
  window; a wall of screens showed one picture.

- **It all happens in one process.** Earlier builds did this by launching a
  second copy of MDropDX12 per display. That worked, and it cost: Windows holds
  a window that is not focused to about 66 frames a second, so every screen but
  the one you were looking at ran at a third of the rate you asked for — and
  frame rate in this program is a creative control, not a detail. Each display
  is now rendered by a thread inside the one process, which Windows treats as
  part of the foreground application. **Displays run at the rate you set.**

  It is also about 140 MB lighter per extra screen, starts instantly instead of
  taking seconds to spawn, and cannot leave an orphaned window behind if
  something goes wrong.

- **A display never takes your keyboard.** Clicking a screen that was running
  its own preset used to hand it the keyboard, after which nothing responded —
  no shortcuts, no next preset, no way back but clicking the main window. There
  is no separate window to click into any more. The mouse still reaches the
  display, so presets that follow the pointer work there.

- **Switching a display is instant.** Turning a screen from mirroring to its own
  preset used to mean waiting for a process to start, with a fade to cover the
  gap. There is no gap.

- **The preset lock applies to every display.** With the lock on, the main
  window stayed put while any display running its own preset carried on
  changing. Locking now reaches every screen immediately, however you do it —
  the hotkey, Scroll Lock, a game controller, a script, the remote, or loading a
  display profile. A display that should ignore the main window can say so
  (Preset lock: Inherit / Unlocked / Locked), and the choice is saved with the
  display and stored in profiles.

- **A display with no startup preset starts on a random one.** Setting a screen
  to its own preset without pinning one brought it up on the same preset as the
  main window, which is the opposite of the point. Each screen now picks from
  its own folder, so they start somewhere different.

- **Each display's opacity is its own.** The Opacity box did nothing for a
  display running its own preset — it used the main window's value, so a screen
  set to 100% could come up almost invisible.

- **Save every screen's presets and come back to them.** A new (unbound)
  hotkey, **Save Display Snapshot**, writes what every display is playing right
  now to a timestamped profile, and loading it puts the same presets back on the
  same screens. Two more unbound hotkeys step to the next and previous profile,
  so a set of arrangements can be walked with one key or from a script. Ordinary
  profiles are unchanged — they store the settings that decide what a display
  plays, not what it happens to be playing.

- **Display profiles save and restore a whole setup.** A profile describes the
  complete display state, so switching no longer leaves Spout senders or enabled
  displays behind from the one before. Profiles can carry each display's preset
  settings, and optionally the main window's and your custom messages. One can
  be loaded automatically at startup.

- **ALT-S can load a profile.** The "Use mirrors for ALT-S" tick box is now a
  drop list: Stretch, Mirror, or Load profile. An existing setting is carried
  over.

- **The Displays tab is reorganised, and shows your monitors as they sit.**
  Controls are grouped into the selected display, settings for all mirrors, and
  profiles. The tab now draws a scale map of the desktop rather than listing
  displays, with each tile coloured by what it is doing and captioned with what
  it is playing.

### Audio Mixer

- **A mixer window for every audio device on the machine.** New in this
  release, opened from Settings → Tools. It lists every playback and capture
  endpoint with a fader and a mute, renames them to whatever you want to call
  them, hides the ones you never touch, and remembers all of it. Eight unbound
  hotkey actions drive two named groups, so a volume key can move a chosen set
  of faders at once.

- **SteelSeries Sonar channels, with both of their levels.** Each Sonar channel
  appears with what *you* hear and what a *stream* hears, adjustable
  independently — which Windows cannot do at all, because Sonar's virtual
  devices ignore the system volume entirely. Sonar is discovered automatically
  and everything to do with it is on a hard timeout, so a wedged install slows
  nothing down.

- **Optional audio failover.** A route can be told to move to another device
  when the one it is using disappears — a headset going flat mid-session, most
  usefully. It is off unless you turn it on, only ever picks a device you listed,
  waits for the replacement to settle before switching, and does nothing at all
  if the original comes back in the meantime. The allowlist is a real list with
  columns, including when each device was last seen, so two identical headsets
  can be told apart and rotated.

- **Your headset's battery, beside the FPS.** A **Show Battery** toggle (a check
  box on Settings → General, and an unbound `ShowBattery` hotkey) draws
  `batt: 68%` in the corner — green above half, amber to a quarter, red below.
  It reads the headset you are actually listening on rather than whatever
  Windows calls the default output: with audio middleware in the way the default
  *is* the middleware and knows no battery. Nothing reports a charge, no line.

- **Volume control beyond the capture device.** MDropDX12 could only change the
  volume of the device it was listening to; it can now set any device's volume
  and mute over IPC. Off until you switch it on.

### .milk2 blend patterns

- **Blend patterns render correctly.** A `.milk2` file is two presets frozen at
  a blend, and the named pattern decides the shape of the boundary — `cercle`,
  `plasma`, `donuts`, `side`, `arrow` and twenty-five more. Most were previously
  approximations, which is why some files looked close and others did not.
  All thirty are now correct.

- **A frozen `.milk2` renders the same way every time it loads.** Patterns
  without a settled form used to draw from the C random number generator, so
  the same file produced a different boundary on every load. They are now seeded
  from the file's own name and saved values: different presets still look
  different, each one is reproducible.

- **MDropDX12 warns when a wipe collapses.** Some `.milk2` files produce a blend
  field that is entirely one preset — the other half is loaded and never shown.
  That now says so rather than looking like a preset that simply does not do
  much.

- **Presets that read the FFT get the frequency scaling they expect**, applied
  in the presets rather than bolted onto the preamble.

### Importing shaders

- **Two command-line converters ship in `tools/`.** Both are Python standard
  library only — nothing to install — and both write `.milk3` files you can drop
  into your presets folder.

  `glslsandbox_to_milk3.py` fetches a [GLSL Sandbox](https://glslsandbox.com)
  effect from its URL or id and converts it:

  ```
  python tools/glslsandbox_to_milk3.py https://glslsandbox.com/e#109677.0
  ```

  `milk3_shadered.py` converts a preset out to a [SHADERed](https://shadered.org)
  project and back, so you can work on a shader in a real IDE with a debugger
  and bring the result home. Multi-pass presets survive the round trip with
  their channel wiring intact, and an unedited export comes back byte-identical:

  ```
  python tools/milk3_shadered.py to-shadered resources/presets/mine.milk3 --zip
  python tools/milk3_shadered.py to-milk3   project.zip -o mine.milk3
  ```

  See [docs/GLSL_importing.md](GLSL_importing.md).

### The HUD and the windows

- **A black screen is never the answer.** A fresh install with no preset library
  configured showed a plain black window forever — indistinguishable from a
  crash. It now comes up on a built-in preset that says where to add your own.

- **GPU load on the HUD.** The debug overlay reports this process's own GPU
  utilisation, and it is readable over IPC.

- **A window that lists which global hotkey combinations are free.** Global
  hotkeys are registered with Windows and any other program can hold one first.
  Collisions used to fail silently; they are now reported, and there is a window
  showing what is actually available before you try to bind it.

- **Hotkeys with Alt in them work.** Any binding using Alt — Alt+B, Ctrl+Alt+P,
  anything — did nothing when the visualizer had the focus, because Windows
  delivers Alt combinations as a different message and that message never
  reached the hotkey table. F10 was unreachable for the same reason.

### Settings, profiles and files

- **Settings move out of `settings.ini` into JSON files.** Display outputs,
  window positions, hotkeys, the audio mixer and every kind of profile now live
  in their own file under `resources/`. One large ini meant a single bad write
  could cost everything; each file is now small, readable, and independently
  recoverable. Existing settings are migrated on first run — nothing to do.

- **Every profile is one file.** Display, audio, VFX and mixer profiles each get
  a file under `resources/profiles/<type>/`. A single-file store rewrote every
  profile on every save, so one bad write cost the whole set.

- **"Preset Lock on Startup" stays set.** The setting was being overwritten with
  whatever the lock happened to be, so unlocking a preset once during a session
  quietly turned the setting off and the next run started unlocked — and locking
  with the setting off switched it on. Two separate things did this. It now
  changes only when you change it, on the Settings window.

- **The Presets window saves the preset you are actually on.** With startup set
  to "Current Preset", Save wrote whichever preset was playing when you *picked
  that mode*, so choosing the mode, browsing to the preset you wanted and
  pressing Save pinned the wrong one.

- **The Presets tab shows which preset is the startup preset**, with a Browse
  button to choose one. The setting existed but nothing displayed it.

- **The profile file dialogs open in the profile folder**, instead of wherever
  the last file dialog left off.

### Remote control and scripting

- **A running copy describes its own command set.** `HELP` lists every command
  the build understands, grouped, with usage and a one-line description;
  `HELP=<command>` explains one; `HELP=JSON` returns it machine-readably.
  Scripts and remotes no longer have to guess.

- **A remote can tell what a PC can do before it asks.** `GET_VERSION` reports a
  `protocol=` number and a `features=` list, both of which move when the IPC
  surface gains something a client can rely on. Until now a phone had to send a
  command and see whether an answer came back, so a user on an older PC got a
  quietly reduced app with no hint that updating the PC was the fix.

- **The remote PIN is actually checked, and is no longer stored in the clear.**
  The PIN guarding the TCP remote was collected and saved but never compared
  against what a client sent, so any PIN was accepted — and it was written to
  `settings.ini` verbatim. It is now salted and hashed, and a wrong PIN is
  refused. An existing PIN keeps working and is converted the first time it is
  used correctly. A phone with the wrong PIN saved will now be told so instead
  of connecting anyway.

- **Remotes and scripts are no longer intermittently refused.** The command pipe
  accepted one connection at a time and was briefly unreachable while setting
  each one up, so several commands in quick succession could silently fail to
  connect. It keeps a pool ready.

- **The Fullscreen button on a remote works.** `SIGNAL|FULLSCREEN` — what
  Milkwave Remote, the Android remote, the Button Board and the `FULLSCREEN`
  script command all send — could only ever *leave* fullscreen, so from a
  windowed visualizer the button appeared dead.

- **Displays are addressable by number.** Preset, folder, order, lock, cycle
  interval and next/previous can be set and read per display, so a remote can
  show their real state instead of only toggling and hoping. Every screen's
  state is readable in one call.

- **Walk-away controls.** `SET_ALL_FULLSCREEN` puts every screen up (and `=2`
  takes them down regardless of what was remembered), and the idle timer can be
  triggered on demand rather than only by waiting.

### Other fixes

- **Messages and automatic preset changes no longer stop partway through a
  session.** Pressing Ctrl+T anywhere on the machine — opening a browser tab,
  most commonly — reset the visualizer's internal clock, and everything waiting
  on that clock was left waiting as far ahead as the session was already long.
  On a three-hour session autoplay messages simply stopped, automatic preset
  changes stopped with them, and text overlays could occupy slots they never
  gave back. Restarting was the only cure.

- **The Feedback Damp applies to mirrored displays.** It shipped in v2.11.0
  covering only the primary window, so damping a preset calmed the main screen
  while every mirror kept running away.

- **Song titles are positioned from their actual coordinates.** The centring
  offset was computed from an uninitialised value, so where a title landed
  depended on whatever happened to be in memory. Positions may shift slightly
  from v2.11.0.

- **The hard-cut suppression after a manual preset change works.** Changing
  preset by hand is supposed to raise the beat threshold briefly so an automatic
  hard cut does not stack on top of it. The value being raised was not the one
  being read.

- **A preset that is only half a file no longer takes the screen with it.** A
  truncated or malformed preset now falls back to something that works.

## v2.11.0 (2026-08-27)

- **A second way to fix a preset that washes out on a big screen.** A handful of
  presets run a feedback loop whose brake weakens as the canvas grows, so they
  look right in a window and flatten to a bright glare at full screen. Until now
  the only answer was **Canvas Limit**, which fixes it by refusing to let the
  canvas grow -- at the cost of sharpness. The new **Feedback Damp** leaves the
  canvas at full size and takes a little energy out of the loop each frame
  instead. The two are independent: use either, both, or neither, per preset,
  from the Presets context menu or the Annotations window.

  The damp is scaled by the canvas rather than by a number tuned to one screen,
  so it does nothing at all at or below a 1024-pixel canvas and grows from there.
  Five settings, Off through Maximum.

  Try it and look, because on some presets it is backwards: if the preset's
  composite shader inverts or folds the feedback it samples, taking energy out
  of the loop hands it straight back and the picture gets brighter, not dimmer.
  The Damp menu now says so when it can tell -- it reads "Feedback Damp (inverts
  - will brighten)" -- but that is a hint, not a guarantee, and neither
  mitigation is ever applied on its own. If Damp does not help, clear it and use
  Canvas Limit, which works whatever the shader does.

- **A preset copied into a TEST folder is now genuinely a fresh copy.** Preset
  identity follows content, so a copy made in order to try a preset *without*
  its saved settings was byte-identical to the original and quietly inherited
  them anyway -- its canvas cap, shader override, VFX profile and audio profile.
  In testing mode, a preset under a TEST folder (or any folder beneath one) now
  gets no identity at all, so it behaves as a stranger. Outside testing mode
  nothing changes.

- **Presets no longer file themselves under each other.** A preset's play count
  was recorded against whichever preset's hash happened to be loaded at that
  moment rather than the one being timed, so an entry could collect other
  presets' locations and pool their play counts -- `Flexi - jellyfish jam` had
  picked up nine unrelated Shadertoy imports. Fixed, and `DIAG_ANNOT_REPAIR`
  cleans up what was already recorded: it removes a stored location only when
  the file is readable and provably a different preset, so genuine copies of one
  preset in several folders are left alone.

- **Keep your settings in the Windows registry instead of a file.** Put
  `UseRegistry=1` in `useregistry.ini` beside the executable and MDropDX12 stores
  everything under `HKEY_CURRENT_USER\Software\MDropDX12` instead of in
  `settings.ini`, `messages.ini` and `sprites.ini`. Settings then belong to you
  rather than to the folder, so replacing the folder with a new build no longer
  takes them with it. The first switch copies your existing files across, so
  nothing is lost, and the files are left untouched as a snapshot; `.ini` files
  remain the default and the portable choice. See
  [docs/settings.md](settings.md).

- **Resizing a window no longer throws away what you were doing.** Dragging the
  edge of a tool window used to rebuild everything inside it, and that took your
  work with it: pasted GLSL in the Shader Editor reverted to whatever the pass
  held when the window opened, edited Controller JSON reverted to the last save,
  a typed BPM went back to the old one, the compile error you were reading
  vanished, and every list jumped back to the top with row 1 selected. All of
  that is kept now. The Sprites window also stopped re-reading `sprites.ini`
  mid-drag, which had been discarding unsaved sprite edits.

- **Sliders fill the window.** The Colors and Visual windows now stretch their
  sliders when you widen them, instead of leaving empty space on the right.

- **A test run no longer changes your settings.** Anything written while testing
  mode is on is held in memory and dropped when it ends. Most importantly, the
  frame-rate cap: testing mode pins the frame rate to 60 and used to write that
  into your configuration, so a measurement run that ended by killing the app
  left you capped at 60 for good.

- **Settings writes no longer stutter.** Changes are collected in memory and
  written about once a second, so dragging a slider or moving a window costs one
  write instead of one whole-file rewrite per pixel of movement.

- **Find duplicate presets, and delete the copies.** **Find Copies** on the
  Annotations window reads every preset under your preset folder and groups the
  ones that are the same preset, by content rather than by name — so a copy that
  was renamed on the way, or that differs only in its `fRating`, is still
  recognised. The report lists each group with folder, date and size; tick the
  copies you want gone and they go to the **Recycle Bin**, never permanently.
  - **Tick: All but newest** and **Tick: Outside preset folder** do the obvious
    selection for you, and the window refuses to delete every copy of a preset —
    at least one always survives. Ratings, tags and notes are stored against the
    preset's content rather than its path, so they follow whichever copy is left.
  - A new **Copies** column and a **Duplicates** filter show the result in the
    main list, so you can sort your library by how redundant it is.
  - See [docs/annotations.md](annotations.md).

- **Play history is visible at last.** The Annotations list gains **Last Used**,
  **Plays** and **Time** columns for the usage data MDropDX12 has been recording
  all along but never showed. All are sortable — sort **Last Used** ascending and
  everything you have never watched comes to the top.

- **The Annotations search is fixed and gets a clear button.** Typing a search,
  closing the window and reopening it used to leave the list still filtered while
  the box appeared empty, with no way back — backspace in an empty box does
  nothing. The box now shows what it is filtering by, the filter dropdown
  likewise remembers its selection, and a **✕** beside the box clears the search
  outright.

- **Purge Missing** is now a button rather than a buried right-click item, and
  says how many entries it is about to drop before you agree. It only ever edits
  `presets.json`; deleting files is the duplicates report's job, deliberately
  kept somewhere else.

- **Details tells you what a preset actually is.** Double-clicking a row now
  shows its content hash, every location the preset is known to live at, the
  other filenames the same content is stored under, all four override slots with
  their real inherit / none / named state, its tags and its usage — not just the
  note and the shader error. It is no longer greyed out for presets that have no
  error.

- **Scratch presets stay out of `presets.json`.** Presets in a folder named
  `TEST` no longer get an entry, and nothing is recorded at all while testing
  mode is on, so a debugging session stops salting the database with throwaway
  files and inflated play counts. Set `AnnotationIgnoreDirs` in the `[Milkwave]`
  section of `settings.ini` to change the folder names, or empty it to record
  everything.

- **Tool windows lay out properly when resized.** The Annotations window shifted
  8px and grew 16px wider the first time it was resized, because its resize code
  and its build code disagreed about the margins; its buttons ran off the right
  edge at narrow widths and its list columns never followed the window at all.
  Rows now wrap when they do not fit, the override slots stack into one column
  when the window is narrow, and the columns size themselves to the window and
  the HUD font. The **always-on-top pin** was also placed once and never moved
  again — on every tool window — so widening the window stranded it in the middle
  and narrowing it put it off the edge where it could not be clicked.

- **New Preset Editor** for editing the running preset's code with syntax
  highlighting, line numbers and folding. Open it from the Presets window
  (**Edit** button or right-click → **Edit Preset...**), from the Annotations
  window, from Settings → Tools, or with the new **Open Preset Editor** hotkey
  (unbound by default). Covers preset init, per-frame, per-pixel, the warp and
  comp shaders, and all 16 custom waves and shapes.
  - A **collapsible tree** navigates the sections, with waves and shapes shut by
    default, plus two whole-file views: **Whole Preset**, which shows the entire
    preset with the numbered code keys (`per_frame_init_1=`, …) grouped under
    `[per_frame_init]`-style headers and the prefixes stripped, and **Raw File**,
    which shows the `.milk` exactly as written. Fold the headers and the whole
    preset collapses to its structure on one screen. Applying either replaces
    the whole preset, so parameters like `fDecay` can be edited too, and
    embedded `[SPRITEn]` sprite blocks are carried through untouched.
  - **Apply** recompiles just the section you changed and you see it
    immediately; **Save** writes the preset back to its file.
  - **Templates** in the right-click menu drop starter snippets suited to the
    open section.
  - **.milk2 double presets** get a **Preset 1 / Preset 2** tab strip, and both
    presets are fully editable. A `.milk2` renders both at a frozen blend
    position, so an edit to either is visible immediately. Save writes a proper
    `.milk2` — both presets, the blend header and any embedded sprite blocks —
    and Save As offers `.milk2` by default.
  - Shader compile errors appear in the status strip with the failing line
    underlined — line numbers are translated back to your own source rather
    than the engine's generated text.
  - **Expand** puts each statement on its own line, so the usual
    `zoom=1; rot=0; cx=0.5;` packing becomes readable. It never breaks inside
    parentheses, strings or comments.
  - Right-click for edit commands and an **Options** submenu with line-number
    and code-folding toggles, both remembered between sessions.
  - Preset auto-advance pauses while the editor is open, so a preset change
    cannot discard work in progress.
  - The window has minimise / maximise / restore.
  - See [docs/preset_editor.md](preset_editor.md).

- **Audio profiles.** A preset can now be given its own audio response curve
  from the new **Audio** dropdown in the Annotations window, beside Shader and
  VFX. Presets written for a different spectrum shape can look unreactive here —
  a preset that draws a ring straight from the spectrum may barely move — and
  switching its profile brings it back to life. One of the supplied profiles is
  matched to **Milkwave Visualizer**, so a preset tuned there reacts here the way
  it does at home; it was matched against the Milkwave build current at the time
  and has not been rechecked since. Nothing changes for any other preset: the
  default profile is this build's existing behaviour exactly.
  - Set a default for everything in Settings (`AudioProfile` in
    `settings.ini`), or attach a profile to a **tag**, so a whole collection
    follows one rule without editing a single preset.
  - Profiles live in `resources/audioprofiles.json` and are plain text, so you
    can copy one, adjust it and share it.

- **Shader errors can be copied.** Right-click → **View Error** in the Presets
  window now opens a proper window instead of a message box, with a **Copy**
  button; the Annotations window's detail dialog has a matching **Copy Error**.
  The copied text is the preset's full path on the first line and then one
  error per line, with the compiler's meaningless
  `…\Shader@0x00000243F354A910` prefix stripped so the line and column lead.
- **Shader errors are recorded against the preset that produced them.**
  Compilation runs on the load thread while the previous preset is still on
  screen, so the "error" flag and the error text were being written to whatever
  preset happened to precede the broken one — permanently, in `presets.json`.
  Dozens of perfectly good presets carried another preset's error. Existing
  wrong flags are not cleared automatically; clear one with right-click →
  **Clear All Flags**.
- **Long shader errors are no longer truncated to garbage.** An error list over
  1024 characters overflowed a fixed buffer and was reported as uninitialised
  memory.

- **Removed two synthetic effects — a glow halo and a set of string ribs** —
  along with the Video Effects window's Rendering tab, the `Md3GlowStrength` /
  `MartinRibCore` / `MartinRibHalo` settings and the `SET_TUNABLE` /
  `GET_TUNABLES` commands. Both were stand-ins for a shader some presets do not
  carry, and they drew over the preset rather than under it — on Rainbow
  Butterfly1 they added five bright ribs and a halo that were not wanted. If you
  liked what they did, a custom shader override supplies a real warp shader
  instead. A `rendering` block in an existing VFX profile is left in place
  rather than stripped.

## v2.9.0 (2026-08-20)

Independent multi-monitor mirrors, user-owned shader overrides, content-hash
preset identity, and rendering and stability fixes.

### Multi-monitor mirrors

- **Independent per-display render**: each monitor renders its own frame rather than copying the primary, with a stable toggle, per-mirror FPS accounting, and no black screens on switching.
- **Portrait mirrors**: a second `.milk3` orientation pipeline with its own feedback buffers, so portrait displays are not letterboxed from a landscape render. Independent `.milk3` mirrors are coalesced by orientation.
- **Per-display HUD**: text is scaled and right-aligned per display, with ellipsis handling on portrait, and an option to hide the HUD on mirrors entirely.
- **Always On Top** is honoured in mirror mode without cross-thread HWND freezes, and primary fullscreen is reliable with mirrors active.
- Fixed a mirror RTV leak, mirror ghosting and freezing, and hotkey focus while a mirror has the foreground.

### Preset identity and annotations

- **Content-hash identity**: `presets.json` is keyed by a hash of the preset's normalized body, so tags, ratings, notes and play counts follow a preset when it is moved or renamed. Exposed over IPC as `GET_PRESET_HASH`. The normalization rule is a file format — see `src/mDropDX12/preset_hash.h`.
- **MDX12 ratings** are dated per-version observations averaged into one number. The app no longer writes `fRating` into `.milk` files.
- **Per-preset usage stats** — last played, play count, seconds shown — gated on a 5-second view threshold so skipping past a preset does not count as playing it.
- `MD31`/`MD32` count toward preset identity: their value selects which cached shader MilkDrop 3 PRO renders, so one body with two `MD31` values is two presets.

### Custom shader overrides

- **Tag-selected warp/comp shader overrides**, substituted at compile time from `resources/shaderoverrides.json` and `resources/shaders/*.hlsl`, with a Custom Shaders tool window. This is MilkDrop 3 PRO's `MD31` mechanism made user-owned. See `docs/custom_shaders.md`.
- **MD31/MD32 presets are detected and flagged** at load, warning that MD3 PRO renders them through a different shader path, so comparisons are not silently running two different pipelines.

### Video effects and settings storage

- **Video effects are drivable over IPC**, and their settings moved out of `settings.ini` into JSON with a single `vfxprofiles.json` profile store.
- **Importer included** for VFX settings written by older builds, so upgrading does not lose them.
- Video effects no longer restore at startup unless "Load on startup" is set.

### Rendering and stability

- **FPS timing**: capped-FPS motion is smooth, unlimited FPS stays real-time with a true frame-rate readout, and the cap no longer locks to ~32 fps on coarse Windows timers. Timing resyncs after sleep.
- **Device recovery**: suspend/resume handling, a VRAM budget exit guard, clean exit on TDR, and no post-hibernate stutter from audio reset or double recovery.
- Blur now runs between warp and inject, as MilkDrop 3 does. MilkDrop 3's thick custom-wave draw is restored, and DX12 custom waves are aspect-corrected.
- Fixed `.milk3` cascading triangle artifacts, the `selfie.milk3` Image V-flip, and a texsize leak.
- **MD31/MD32 glow** is approximated rather than leaving those presets flat. Martin rib ribbon widths are tunable.

### Idle and window behaviour

- Idle timer can mirror to all displays, with a HUD countdown, cursor restore, and fullscreen restore on wake.
- **Tool windows remember where they were** — positions save on `WM_EXITSIZEMOVE`, not only at destroy, and off-screen windows are rescued back on screen. Partial-redraw flicker fixed.

### Shadertoy import

- **GLSL Sandbox to `.milk3` converter**, alongside the existing Shadertoy path.
- Redistributable `sb_tunnel` `.milk3` demos ship in the portable zip.

### IPC / Remote

- **`SIGNAL|MIRROR_INDEPENDENT`** / **`MIRROR_INDEPENDENT`** / **`SET_MIRROR_INDEPENDENT=0|1`** — remote toggle/set independent per-display mirror render (also `ACTION=MirrorIndependent`)
- **`SIGNAL|ALWAYS_ON_TOP`** / **`ALWAYS_ON_TOP`** / **`SET_ALWAYS_ON_TOP=0|1`** — remote toggle/set always-on-top for primary + mirrors
- **`SET_MIRROR_ENABLED=0|1`** (or `N,val` for DISPLAY N) — enable/disable monitor mirror outputs
- **`DIAG_MIRRORS`** now reports `independent`, `aot`, `watermark`, and per-monitor `independent`
- Script and controller accept `MIRROR`, `STRETCH`, `MIRROR_WM`, `WATERMARK`, `MIRROR_INDEPENDENT`, `ALWAYS_ON_TOP`
- Button Board Window menu: Mirror Independent, Always On Top (in addition to Watermark)

## v2.8.0 (2026-06-15)

MilkRemote connectivity, Shadertoy import fixes, and FPS cap cleanup.

### MilkRemote / TCP

- **TCP reconnect handling**: Evicts stale sockets from the same device ID and client IP on reconnect; sends `AUTH_REQUIRED` when commands arrive before re-auth.
- **Split connection timeouts**: 15s for unauthenticated clients, 120s for authenticated sessions.

### Shadertoy Import

- **Cross-line matrix multiply**: GLSL→HLSL converter no longer emits `mul(matN(...), )` with an empty operand when `*` spans lines.
- **Matrix macro detection**: `#define R(a) mat2(...)` style macros are converted to `mul()` correctly.
- **`.json` preset loading**: `LoadPreset()` routes `.json` files through `ShaderImportWindow::ImportFromFile()`.

### UI / Controls

- **FPS cap table**: All FPS options (Visual combo, F3 hotkey, `SetFPSCap` sync) use a single `kFpsCapOptions` table in `fps_caps.h`.
- **40 fps cap**: Added to the FPS cap list; F3 cycles in the same order as the Visual window combo.

### Other

- **Manifest trustInfo**: Explicit `asInvoker` execution level in `manifest.xml`.

## v2.7.0 (2026-03-20)

Stability, rendering fixes, and quality-of-life improvements.

### Stability

- **SEH crash recovery**: Auto-skip preset after 3 consecutive render exceptions; safe mode after 10 skipped presets.
- **TCP server off by default**: No firewall prompt on first launch. Enable in Settings or `settings.ini` for MilkRemote.

### Rendering Fixes

- **Normalize whitespace fix**: `normalize (` with space now correctly replaced with `_safe_normalize(`.

### UI Improvements

- **Baked lock icon**: Pixel-art padlock in font atlas replaces bullet dot as preset lock indicator.

## v2.6.0 (2026-03-18)

Shader compatibility, rendering quality, and .milk2 blend improvements.

### New Features

- **Anisotropic Filtering** (Visual window): 16x anisotropic filtering toggle for linear DX12 samplers. Off by default.

### Preset Rendering Fixes

- **Safe division for DX9 compatibility**: `_safe_denom()` wrapper prevents NaN from `0/0` in shader bodies (DX9 returns 0, DX12 returns NaN).
- **Initialize comp quad Diffuse**: Fixed uninitialized vertex diffuse color on fullscreen comp quad.

### .milk2 Blend Improvements

- **Fixed wipe patterns**: `blending_pattern=horizontal` and `blending_pattern=vertical` in `.milk2` files now use dedicated fixed-position wipe shaders.

### MCP Server

- Auto-launch MDropDX12 and black frame retry in compare tool
- END_BATCH sentinel for instant multi-message pipe responses
- MilkDrop 3 pipe classification fix (exe name with space)
- Compare window positioning via pipe STATE

## v2.5.0 (2026-03-17)

Rendering accuracy, quality controls, and stability release.

### Preset Rendering Fixes

- **NaN-safe atan2**: `atan2(0, 0)` returns NaN on DX12 (DX9 NVIDIA returns 0). Added `_safe_atan2(y, x)` wrapper that prevents NaN at the origin — fixes persistent black holes in tunnel/radial presets.
- **Safe denominator intrinsics**: Added `_safe_normalize()` overloads that guard against zero-length vectors, preventing NaN propagation in raymarching and particle presets.
- **Fix message SIZE parameter**: Messages with explicit `SIZE=N` parameter now respect the specified size instead of always autosizing.

### New Features

- **Mesh Size control** (Visual window): Slider to adjust warp/shape mesh vertex density (8–192, step 8). Previously only changeable via `settings.ini`.
- **Texture Precision control** (Visual window): Combo box to select internal render target bit depth — 8-bit, 16-bit float, or 32-bit float. Previously only changeable via `settings.ini`.
- **Media key routing**: Media keys route through `keybd_event` for system-level handling.
- **Windows volume control via IPC**: `SET_VOLUME` / `GET_VOLUME` / `SET_MUTE` / `GET_MUTE` IPC commands.
- **Audio gain attenuation**: Audio sensitivity can now be set below 1.0 (previously clamped to 1.0 minimum).

### Stability Fixes

- **Fix resize crash** (PR #32): Restore DX12 command infrastructure after `ResizeBuffers` failure.
- **Fix display output cleanup**: Release display mirror Spout wrapped backbuffers during device cleanup.
- **Fix preset startup default** (PR #31): `m_bEnablePresetStartup` defaults to `true` for fresh installs.

### MCP Server

- BeatDrop pipe discovery support
- Capture path included in response
- Null-terminator framing fix for reliable message parsing

## v2.4.0 (2026-03-15)

Preset compatibility and visual accuracy release. Fixes two rendering bugs that caused presets to render differently from the reference Milkwave Visualizer, and adds a visual comparison document with side-by-side screenshots.

### Preset Rendering Fixes

- **Fix alpha blend feedback amplification in textured shapes**: SPRITEVERTEX PSOs used `SrcBlendAlpha=ONE` instead of `SRC_ALPHA`, causing textured shapes to write excess alpha that compounded through the feedback loop. DX9 has no separate alpha blend (`D3DRS_SEPARATEALPHABLENDENABLE=FALSE`), so alpha uses the same factors as color. Fixes "BrainStain - re entry" and other presets with textured shapes appearing much brighter than reference.
- **Fix HLSL variable shadowing user-defined functions**: Added `FixShadowedUserFunctions()` to rename local variables that reuse user-defined function names (valid in GLSL, rejected by HLSL with error X3005). The existing `FixShadowedBuiltins` only handled intrinsic functions. Fixes "Marex + IkeC - Shadow Party Shader Jam 2025" rendering as black screen.

### Documentation

- **Visual comparison document**: New `docs/comparison.md` with side-by-side screenshots of 11 presets rendered on both MDropDX12 and Milkwave Visualizer. All 11 presets now render equivalently.

## v2.3.0 (2026-03-14)

Shader compatibility and rendering completeness release.

### Preset Rendering Fixes

- **Fix `_safe_sqrt` to return always-positive values**: Changed from `sign(x)*sqrt(abs(x))` to `sqrt(abs(x))`, matching DX9 SM3.0 native sqrt behavior. The sign-preserving form created singularities in presets where `sqrt(negative)+offset` crossed zero, producing infinity through the feedback loop. Fixes "martin - axon3" (blown out white) and similar presets.
- **Implement DX12 motion vector rendering**: New `DX12_DrawMotionVectors()` draws motion vector lines into VS[0] before the warp pass using `PSO_LINE_ALPHABLEND_WFVERTEX`. Fixes presets like "Illusion & Rovastar - Clouded Bottle" which appeared too dark because their motion vector lines were missing.
- **Fix `[loop]` attribute injection**: Only inject `[loop]` on `while` loops (raymarching constructs), not `for` loops. Small fixed-count `for` loops caused `error X3531` when marked `[loop]`. Fixes black-screen failures in several MilkDrop2077 presets.

### Shader Compilation Improvements

- Add `D3DCOMPILE_PARTIAL_PRECISION` and `D3DCOMPILE_PREFER_FLOW_CONTROL` flags for SM3.0-like codegen
- Fix D3DXCompileShader flag passthrough (all caller flags now reach D3DCompile)
- Replace `i = I_MAX` break hack with native `break` statement

### Diagnostics

- Bytecode disassembly dump at verbose log level (`log/diag_asm_*.txt`) via `D3DDisassemble()`

## v2.2.0 (2026-03-14)

Preset rendering accuracy release. Fixes long-standing math and shader issues that caused many presets to render darker, incorrectly filtered, or visually different compared to the reference Milkwave Visualizer.

Special thanks to [IkeC](https://github.com/IkeC) for all his brilliant work on [Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer, testing feedback, and tireless collaboration that continues to drive MDropDX12 forward.

### Preset Rendering Fixes

- **Fix sqrt() emulation to match DX9 hardware behavior**: DX9 compiles `sqrt(x)` as `x * rsq(x)`, which returns `sign(x) * sqrt(|x|)` for negative inputs — a negative value that UNORM render targets clamp to zero. MDropDX12's previous `_safe_sqrt` used `sqrt(abs(x))`, returning positive values for negative inputs and shifting the feedback equilibrium brighter in some presets and darker in others. Now uses `sign(x) * sqrt(abs(x))` to match DX9 exactly. Fixes darkness/brightness issues across many presets with complex feedback loops.
- **Fix sampler addressing for prefixed noise/random textures**: Presets using `sampler_pw_noise_lq`, `sampler_fc_noise_hq`, `sampler_pc_rand00`, and other `pw_`/`fc_`/`pc_` prefixed samplers were falling through to the default LINEAR+WRAP sampler instead of their intended POINT+WRAP, LINEAR+CLAMP, or POINT+CLAMP modes. Added 36 `replaceTex2D` entries covering all prefixed variants of noise, noisevol, and random textures. Fixes excessive glow, incorrect texture filtering, and visual artifacts in presets like "martin - push ax".
- **Fix vertex decay color applied to custom warp shader presets**: Milkwave's `WarpedBlit_Shaders` passes white (0xFFFFFFFF) vertices when a custom warp shader is active — the shader handles decay internally. MDropDX12 was incorrectly always applying the `cDecay` vertex color, causing premature darkening. Custom warp shader presets now receive white vertices; non-shader presets continue to use cDecay for fallback decay modulation.
- **Add NaN-safe shader intrinsics for DX12 IEEE 754 compliance**: DX12 follows strict IEEE 754 where `sqrt(negative) = NaN`, `tan(pole) = inf`, `inf * 0 = NaN`, and `pow(negative, fraction) = NaN`. DX9 NVIDIA hardware deviates — `sqrt(neg) = sqrt(|x|)`, `inf * 0 → 0`, etc. Added safe wrappers (`_safe_sqrt`, `_safe_tan`, `_safe_pow`, `_safe_asin`, `_safe_acos`, `_safe_normalize`) that are `#define`-redirected before preset shader code. Prevents NaN propagation through the feedback loop which caused persistent black holes.
- **Fix comp vertex shader uv_orig binding**: The comp and warp vertex shaders now use separate TEXCOORD0/1/2 inputs matching the `g_MyVertexLayout` input element descriptor, correctly passing `uv`, `uv_orig`, and `rad_ang` as independent float2 values.

### Other Fixes

- **Fix screenshot red/blue channel swap**: WIC PNG encoder's `SetPixelFormat` silently changes RGBA to BGRA, then `WritePixels` misinterprets the RGBA backbuffer data. Screenshots now swap R↔B in the readback buffer before writing as BGRA.
- **Increase default ToolWindow font size**: Default increased from 16px to 20px, maximum from 24px to 32px for better readability on high-DPI displays.

### Diagnostics

- Added `DIAG_DISPLAY_MODE` IPC command for raw render target inspection (0=normal, 1=VS[0], 2=VS[1])
- Shader text dump diagnostics for warp and comp shaders

## v2.1.0 (2026-03-13)

Stability and IPC release focused on multi-display mirror modes, watermark mode robustness, and expanded Named Pipe commands.

### Bug Fixes

- **Fix watermark mirror -> regular mirror transition**: Switching from mirror watermark to regular mirror mode via hotkey left mirrors at 30% opacity with click-through enabled, causing visual hang. New `ExitMirrorWatermark()` helper properly restores render window state (opacity, click-through, always-on-top, position) and resets mirror configs from all toggle paths (hotkeys, IPC signals, Alt+S, Alt+Enter, window reset)
- **Fix WindowOpacity persisting watermark value to INI**: Watermark mode's 0.30 opacity leaked into `settings.ini` on save/exit, persisting across restarts. Watermark handlers now write pre-watermark opacity to INI immediately on entry; the general INI save routine skips `fOpacity` when in any watermark mode
- **Fix window position persisting to wrong monitor during watermark**: `SaveWindowSizeAndPosition()` now skips when in watermark mode (the render window is temporarily on a different monitor at full size)
- **Fix render monitor excluded from display list**: The render window's monitor was completely removed from `m_displayOutputs` during enumeration. Now included with `bSkippedSameMonitor` flag for dynamic runtime filtering in `SendToDisplayOutputs()` — all monitors appear in DIAG and Displays window
- **Fix display config lost on re-enumeration**: Monitor configs (enabled, fullscreen, opacity, click-through) are now saved and restored across `EnumDisplayMonitors` calls, matched by display rect with device name fallback
- **Fix mirror activation prompt blocking message loop**: Removed the blocking `MessageBox` prompt from `TryActivateMirrors()` — auto-enables all monitors when none are configured (the "Don't ask" checkbox made this the default anyway)
- **Fix mirror swap chain init race**: Added `WaitForGpu()` before `CreateSwapChainForHwnd` to prevent driver failures from in-flight render commands
- **Fix SRV descriptor heap overflow on window resize and font rebuild**
- **Fix border-only shape rendering and improve adaptive audio gain**
- **Fix always-on-top setting not persisting through restarts**
- **Fix preset lists, message autoshrink, and deferred startup save**
- **Fix IPC signal handlers for FULLSCREEN, WATERMARK, BORDERLESS_FS, STRETCH, MIRROR**

### New Features

- **Mirror Watermark mode**: New button in Displays window + `MirrorWatermark` hotkey action + `SIGNAL|MIRROR_WM` IPC command. Puts the visualizer on all displays simultaneously with low opacity and click-through for desktop overlay use
- **Hotkey reset moved to Hotkeys window only**: Reset to Defaults now requires confirmation dialog and is only available in the Hotkeys window (no longer in Settings)
- **TRACK| outgoing IPC message**: Broadcasts current track info (artist, title, album, artwork path) to all connected pipe clients when track changes
- **New IPC commands**: `DIAG_MIRRORS` (comprehensive mirror state dump), `SET_MIRROR_OPACITY=N`, `SET_MIRROR_CLICKTHRU=0|1`, `MOVE_TO_DISPLAY=N`, `SET_WINDOW=x,y,w,h`, `SIGNAL|MIRROR_WM`
- **Display profile save/load**: Save and restore complete display output configurations as JSON files
- **Improved mirror logging**: Detailed init/skip/error logging for mirror window creation with device names

### IPC / Pipe Changes

- `SIGNAL|FULLSCREEN` — deactivates mirrors first, then toggles fullscreen
- `SIGNAL|MIRROR` — toggle mirror mode with proper watermark cleanup
- `SIGNAL|MIRROR_WM` — toggle mirror watermark mode (all displays, low opacity, click-through)
- `SIGNAL|WATERMARK` — toggle single-window watermark mode
- `SIGNAL|BORDERLESS_FS` — toggle borderless fullscreen
- `SIGNAL|STRETCH` — toggle multi-monitor stretch
- `DIAG_MIRRORS` — dump full mirror state (active, render monitor, per-monitor config, swap chain sizes, window rects)
- `SET_MIRROR_OPACITY=<1-100>` — set opacity for all monitor mirrors
- `SET_MIRROR_CLICKTHRU=<0|1>` — set click-through for all monitor mirrors
- `MOVE_TO_DISPLAY=<N>` — move render window to center of display N (1-based)
- `SET_WINDOW=<x>,<y>,<w>,<h>` — set render window position and size
- `TRACK|artist=...|title=...|album=...|artwork=...` — outgoing broadcast on track change

## v2.0.0 (2026-03-12)

Major milestone release. MDropDX12 2.0 marks the point where the engine has been fully rebuilt on DirectX 12 with a comprehensive feature set that goes well beyond the original MilkDrop2 visualizer.

Special thanks to [IkeC](https://github.com/IkeC) for all his hard work on [Milkwave](https://github.com/IkeC/Milkwave) — the inspiration, testing, feedback, and collaboration that made this project possible.

### Shadertoy Import & GLSL->HLSL Converter

- Full Shadertoy rendering pipeline with `.milk3` JSON preset format
- Multi-pass rendering: Buffer A -> Buffer B -> Buffer C -> Buffer D -> Image, with Common shared code
- SM5.0 (`ps_5_0`) shader compilation for all Shadertoy presets
- FLOAT32 ping-pong feedback buffers for temporal accumulation effects
- Comprehensive GLSL->HLSL converter handling matrices, structs, vector comparisons, array params, over-specified constructors, and more
- Shader Import window with two-panel editor, per-pass channel combos, Convert & Apply, and Save .milk3 export
- Channel auto-detection: self-feedback, audio, noise textures, and JSON channel names
- Shadertoy-compatible `iMouse`, `iDate`, `iResolution`, `iTime`, `iChannel0–3` uniforms
- sRGB gamma correction for Shadertoy-accurate color output
- 17+ converter fixes since initial release (variable shadowing, matrix chains, precision strips, texture bias, and more)

### Named Pipe IPC

- Replaced WM_COPYDATA / hidden window IPC with Named Pipes (`\\.\pipe\Milkwave_<PID>`)
- PID-based discovery eliminates fragile window-title matching
- Multi-instance pipe server: concurrent client connections (Milkwave Remote + MCP simultaneously)
- Duplex message-mode communication with non-blocking outgoing messages
- 32 of 34 Milkwave commands handled natively
- Signal messages (`SIGNAL|NAME=VALUE`) for extensible event notification
- MCP server (`tools/mdrop-mcp/`) for AI-assisted visualizer control

### ToolWindow System

- 20+ standalone windows running on their own threads with independent always-on-top, sticky positions, and tab memory
- Windows: Visual, Colors, Controller, Displays, Song Info, Hotkeys, MIDI, Presets, Sprites, Messages, Remote, Script, Shader Import, Video Effects, VFX Profiles, Text Animations, Button Board, Workspace Layout, Error Display, Annotations
- Dark-themed popup context menus using uxtheme dark mode APIs
- ModalDialog base class for themed popup dialogs with shared font/theme/DPI support

### Configurable Hotkeys & Input

- Dedicated Hotkeys window (Ctrl+F7) with per-binding local/global scope
- Mouse button bindings (Left, Right, Middle, X1, X2)
- Dynamic Script and Launch App hotkey slots with unlimited entries
- Conflict detection and Reset to Defaults
- Native MIDI input with 50 mapping slots, learn mode, button/knob actions
- Game controller support with JSON config and IPC command binding

### Preset Annotations

- Persistent per-preset ratings (0-5), flags (favorite/error/skip/broken), notes
- Auto-captured shader error text in `presets.json`
- Annotations ToolWindow with filter, import, scan, and detail dialogs
- Right-click context menu on Presets window

### Video & Display

- Native webcam and video file input mixing (background/overlay compositing with luma key)
- Spout video input mixing via D3D11On12
- Video Effects window with transform, color, and audio-reactive controls
- VFX JSON profiles for saving/loading effect presets
- Monitor mirroring with per-display opacity, click-through, and safety controls
- Workspace Layout window for tiling tool windows across the screen
- Two-pass shader blending for preset transitions

### Audio & FFT

- FFT EQ smoothing with configurable attack/decay and peak hold
- Audio texture 512x2 R32_FLOAT (smoothed spectrum + peak values)
- Shader functions: `get_fft()`, `get_fft_hz()`, `get_fft_peak()`, `get_fft_peak_hz()`
- Separate clean FFT for shader audio texture (Hann³ window, no EQ)

### UI & Settings

- In-app Settings window (F8) with tri-mode theme (Dark/Light/Follow System)
- 5-tab UI (General, Tools, System, Files, About) with preset browser and resource viewer
- Button Board with slot images, hotkeys, JSON layouts, drag-drop
- Text Animations window with DX12 warped text, color/font pickers, animation profiles
- Cover art sprite system with IPC signal
- File association registration for .milk/.milk2 (Settings -> About, no admin required)
- Command-line preset loading (double-click .milk/.milk2/.milk3 in Explorer)
- Welcome window with first-run setup options

### Fixed Issues (since v1.0)

- Fixed fullscreen black rendering for dot-based presets (DX12 point size emulation)
- Fixed dark/incorrect blur presets (removed DX9 half-texel UV offsets)
- Fixed non-shader preset rendering (comp shader binding, shape alpha blend, warp decay)
- Fixed pre-MilkDrop2 preset rendering (clamp sampler, Y-flip)
- Fixed ns-eel2 `regNN * regNN` multiply bug (optimizer treated different regs as identical)
- Fixed `ps_2_a` silently dropping texture bindings on complex shaders (raised to ps_3_0 minimum)
- Fixed comp shader reading post-warp darkened texture instead of pre-warp input
- Fixed TDR crash when disabling mirror outputs (GPU flush before resource release)
- Fixed HUD text overflow on portrait/large displays
- Fixed render hang on display mode switching (swap chain recovery)
- Fixed mirror window deadlock (cross-thread message pumping)
- Fixed global hotkeys not dispatching most actions
- Fixed `sampler_rand` black screen and random texture directory search
- Fixed GLSL converter: variable shadowing, matrix chains, precision strips, texture bias, out param zero-init, over-specified constructors, reserved keywords, and many more
- Fixed channel auto-detection: temporal reprojection false positives, audio heuristic overrides
- Fixed screenshot filename using previous preset name after shader import
- Fixed notification overlay persisting across preset changes

## v1.7.7 (2026-03-11)

### Rendering Fixes

- Fixed fullscreen black rendering for dot-based presets — DX12 custom wave dots were always 1px instead of emulating DX9 `D3DRS_POINTSIZE` (2-3px squares at high resolution). Sparse additive dots couldn't accumulate enough brightness in the feedback loop to overcome comp shader darken effects.
- Fixed dark/incorrect rendering for blur-dependent presets (e.g., Flexi) — removed leftover DX9 half-texel UV offsets from blur shaders that shifted blur by 1 texel, compounding through the feedback loop
- Fixed non-shader preset rendering — auto-gen comp shader now correctly binds VS[1] (post-warp+shapes) instead of VS[0], and custom shapes use proper alpha blending PSO
- Fixed comp shader reading post-warp darkened texture instead of pre-warp input for user-written comp shaders
- Fixed pre-MilkDrop2 preset rendering — clamp sampler for non-wrapping textures, correct Y-flip, and proper warp decay via vertex diffuse color

### Hotkeys & Input

- Added separate Mirror and Stretch hotkey actions (unbound by default)
- Fixed global hotkeys not dispatching most actions
- Guard mirror prompt against re-entry with auto-accept after 5 seconds

### Internal

- Reduced ToolWindow boilerplate with macros, registry, and template helpers

## v1.7.6 (2026-03-10)

### Display Mode Stability

- Fixed render hang when switching between mirroring and fullscreen — Present `E_FAIL` (0x80004005) now triggers automatic swap chain recovery after consecutive failures
- Fixed `DXGI_PRESENT_ALLOW_TEARING` causing persistent `E_FAIL` during window state transitions — Present now retries without the tearing flag on failure
- Fixed device recovery (TDR) never triggering after `DXGI_ERROR_DEVICE_REMOVED` — `m_lastErr` check was blocked by `m_ready` check running first
- Fixed render window losing focus when switching between fullscreen, spanning, and mirroring display modes
- Fixed mirror window deadlock — render thread now pumps messages for render-thread-owned windows (mirror windows) to prevent cross-thread `SendMessage` deadlock during `SetWindowPos` z-order changes
- Fixed Alt+Enter while mirroring now disables mirrors and stays in single-monitor fullscreen instead of exiting to windowed mode

### ToolWindows

- ToolWindows opened via hotkey now appear above fullscreen/topmost render window using `AttachThreadInput` for reliable cross-thread `SetForegroundWindow`
- ToolWindows created while render is topmost now inherit `WS_EX_TOPMOST` so they aren't hidden behind fullscreen render

### Documentation

- Restructured Manual.md: Settings window now documents 5 tabs (General, Tools, System, Files, About) with ToolWindows as a separate section
- Added documentation for all ToolWindows including Controller, Error Display, Annotations, Workspace Layout, Video Effects, VFX Profiles, Button Board
- Updated IPC protocol references from WM_COPYDATA to Named Pipes throughout docs
- Added IPC commands reference section to Scripts.md
- Fixed blur level count in Textures.md (6 -> 3)
- Added presets.json to Install.md configuration files list

## v1.7.5 (2026-03-10)

### Preset Annotations

- New persistent preset annotation system — per-preset ratings (0-5), flags (favorite/error/skip/broken), notes, and auto-captured shader error text stored in `presets.json`
- Annotations ToolWindow with filterable ListView, detail dialogs, import from file, and scan loaded presets for `.milk` `fRatingThis` values
- Import dialog shows side-by-side comparison of source vs current ratings/flags with selective import, merge, or bulk import options
- Auto-flag presets with `PFLAG_ERROR` on shader compilation failure, capturing error text for later review
- Random preset selection skips presets flagged as `PFLAG_SKIP` or `PFLAG_BROKEN`
- Right-click context menu on Presets window with Toggle Favorite, Toggle Skip, Flag as Broken, Add Note, View Error, Clear Flags, and Rating submenu
- Annotations accessible from Presets context menu ("Open Annotations...") and Tools tab in Settings

### UI Infrastructure

- Added `DoContextMenu` virtual method to ToolWindow base class — enables right-click context menus in any ToolWindow subclass
- Dark-themed popup context menus using undocumented uxtheme dark mode APIs (`SetPreferredAppMode`, `AllowDarkModeForWindow`, `FlushMenuThemes`)
- Cover art sprite system with Show Now button and IPC signal

### Preset Transitions

- Two-pass shader blending for DX12 preset transitions

## v1.7.4 (2026-03-10)

Special thanks to [IkeC](https://github.com/IkeC) and [_Incubo](https://github.com/OfficialIncubo) for testing and reporting issues.

### Rendering

- Fixed `sampler_rand` textures causing black screen — random texture was not bound when preset references `sampler_rand` without a corresponding texture file
- Fixed random texture directory not searched when loading `sampler_rand` textures (only the preset's own directory was checked)
- Fixed `tex2D` sampler mismatch when a space follows the opening parenthesis (e.g. `tex2D( sampler, uv)`) — sampler name was not extracted correctly
- Fixed non-shader preset rendering: video echo, gamma adjustment, and hue rotation now work correctly for presets without comp shaders
- Fixed `hue_shader` corner colors not applied to DX12 comp fullscreen quad (colors were black instead of interpolated)
- Fixed warp mesh decay and blur texture scan (warp feedback was too aggressive, blur levels miscalculated)

### Shader Infrastructure

- Raised minimum pixel shader version to `ps_3_0` — fixes `ps_2_a` silently dropping texture bindings on complex shaders (e.g. Organic12-3d-2)
- Normalized `tex2D` whitespace handling in shader text processing

### Expression Evaluation

- Fixed ns-eel2 `regNN * regNN` multiply bug — optimizer incorrectly treated `reg10*reg20` as `sqr(reg10)` due to cleared symbol names (bug exists in upstream WDL)

### Preset Loading

- `.milk2` preset loading is now asynchronous (non-blocking, same as `.milk` presets)

### Diagnostics

- Added EEL crash diagnostics with thread-local compile context tracking (`diag_eel_error.txt`)
- Added shader text dump diagnostics (`diag_comp_shader.txt`, `diag_warp_shader.txt`)

### Bootstrap

- Removed automatic directory creation during bootstrap (prevents creating empty directories in unexpected locations)

## v1.7.3 (2026-03-09)

### Rendering

- Fixed comp shader `rad`/`ang` per-pixel computation — presets using `(1-rad)` in comp shaders no longer render as black screen (fullscreen quad had `rad=1.0` hardcoded at all vertices; now computed per-pixel from UV coordinates with aspect correction)

### Shader Infrastructure

- Embedded (compiled) shaders are now always the primary source — disk `.fx` files no longer override the DX12-correct versions
- Disk `.fx` user overrides are merged: `#define` directives override compiled defaults, and custom functions not in compiled are appended
- Fixes `_samp_lc` / DX12 sampler errors when running from a directory with legacy DX9 `.fx` files (e.g. Milkwave)

### Crash Diagnostics

- SEH crash handler now writes full post-mortem diagnostics to `diag_seh_crash.txt` — register state (RAX-R15, RIP, EFLAGS), stack trace (StackWalk64 + SymFromAddr), exception details, and module resolution
- EEL compile context tracking: thread-local RAII scope records which EEL expression phase/source/preset is being compiled, written to `diag_eel_error.txt` on crash

## v1.7.2 (2026-03-09)

### GLSL->HLSL Converter

- Unified non-square matrix conversion to use mul-swap strategy (same as square matrices) — fixes `M[i]` indexing returning wrong vector type/size
- Fixed audio channel auto-detection overriding JSON `CHAN_FEEDBACK` assignment — `texelFetch(iChannel, ivec2(x,0))` false positive
- Fixed matrix-returning function chains across newlines (`M1(...)\n* M2(...)\n* v` now correctly wraps with nested `mul()`)
- Fixed `matVar * (expr)` not converting parenthesized expressions to `mul()`
- Added `point` to HLSL reserved keyword renaming (geometry shader primitive type)

## v1.7.1 (2026-03-09)

### FFT Accuracy

- Added separate clean FFT (`m_fftShader`) with Hann³ window and no equalization for shader audio texture — fixes high frequencies being over-accentuated in EQ visualization presets
- FFT for shader `get_fft()` / `get_fft_hz()` now matches Milkwave's scaling (0.00035f, noise gate 5e-5, visible floor 2.5e-4)
- Beat detection FFT (`myfft`) unchanged — still uses equalization for accurate bass/mid/treb analysis

### Shader Features

- Added `iDate` support (`_c19`): `float4(year, month 0-11, day, seconds_since_midnight)` for wall clock time in shaders
- Added `tex3Dlod` macro for 3D texture LOD sampling
- GLSL converter now maps `iDate` to `_c19` instead of commenting it out as unsupported

### Preset Name Display

- Added "(Simple)" mode for preset name display — uses decorative font settings (adjustable size and color only, no animation effects)
- Simple mode is now the default (`PresetNameAnimProfile=-3`)
- Combo box in Text Animations window shows (Disabled) / (Simple) / (Random) / named profiles

### Shadertoy Rendering

- Supertexts (preset name, song title) and user sprites now render in Shadertoy (.milk3) render path

### Preset Browser

- Single-click on directory entries now navigates into/out of directories (previously required double-click)
- `sampler_rand` stripping now only matches actual `Texture2D` declarations (prevents false positives when presets reference sampler by name only)

### HUD Overlay

- Song title on overlay auto-shrinks font when text exceeds available width

### Stability

- Suppressed "no presets found" error when a preset is already playing — fixes nag during preset list rebuild race
- Animation profiles now persist to disk when Text Animations window is closed

## v1.7.0 (2026-03-08)

### FFT EQ Smoothing & Peak Hold

- Added FFT smoothing with configurable attack/decay interpolation for shader `get_fft()` / `get_fft_peak()` functions
- Audio texture upgraded to 512x2 R32_FLOAT: row 0 = smoothed FFT spectrum, row 1 = peak hold values
- Peak hold algorithm: 30-frame hold then 0.97x per-frame decay
- New shader helper functions in include.fx: `get_fft(pos)`, `get_fft_hz(freq)`, `get_fft_peak(pos)`, `get_fft_peak_hz(freq)`
- `HAS_FFT_PEAK` define for conditional peak access in presets
- `sampler_fft` alias for `sampler_audio` compatibility
- Milkwave EQ visualization presets now work (e.g. IkeC - Equalizer.milk)

### Milkwave Remote IPC

- Handle `FFT_ATTACK=` and `FFT_DECAY=` IPC commands (0.0–1.0 range, clamped)
- Report `FFTATTACK` and `FFTDECAY` values in status response to Remote
- INI persistence for FFTAttack/FFTDecay defaults under `[Milkwave]` section

### Bootstrap & First Run

- Upgraded first-run dialog from MessageBox to TaskDialogIndirect with 4 command-link buttons
- New "Copy settings from existing install" option: folder picker imports all .ini files from another MDropDX12 directory
- Proper Common Controls v6 manifest activation (fixes ordinal 345 / TaskDialogIndirect crash)
- PerMonitorV2 DPI awareness and Windows 10/11 compatibility declarations in manifest
- Manifest now embedded via resource compiler (`1 RT_MANIFEST "manifest.xml"`)

### Error Display Settings

- New Error Display Settings ToolWindow for configuring shader error notification appearance
- Normal and LOUD display modes with separate font size, duration, and color settings
- Configurable error duration used across the codebase (replaces hardcoded values)
- DPI-aware modal dialog sizing with BaseLayout for consistent metrics

### Bug Fixes

- Fixed DX9 caps garbage causing "PSVersion=0x2" shader model error on DX12 (hardcoded MD2_PS_3_0)
- Fixed `blankDecl` stripping `sampler_audio` declaration needed by `get_fft()` shader functions
- Fixed `UpdateAudioTexture()` not called in regular `RenderFrame()` path (only in Shadertoy path)
- Fixed FFT scaling factor producing near-zero values (was 0.00035f from Milkwave Visualizer, now 2.0f for MDropDX12's FFT range)

## v1.6.0 (2026-03-08)

### Audio Reactivity

- Fixed frequency band ranges in AnalyzeNewSound — restored original 200–11025 Hz range (was incorrectly changed to 20–20000 Hz, spreading energy across 10 octaves instead of 5.8 and using wrong FFT bin divisor)
- Bass/mid/treb reactivity now matches Milkwave reference

### Code Reorganization

- Created `engine_midi.cpp` — MIDI persistence, device lifecycle, knob/button dispatch (extracted from engine_midi_ui.cpp)
- Created `engine_sprites.cpp` — sprite lifecycle, INI I/O, property management (extracted from engine_messages.cpp and engine_settings_ui.cpp)
- Created `engine_config.cpp` — settings persistence, theme, user defaults, folder picker, screenshots, Remote launch (extracted from engine_settings_ui.cpp and engine_messages.cpp)
- Moved Spout output functions (OpenSender, SpoutReleaseWraps, ToggleSpout, SetSpoutFixedSize) from engine_messages.cpp to engine_displays.cpp
- Promoted duplicated sprite helpers (FormatSpriteSection, FormatSpriteSectionA, MakeRelativeSpritePath) to inline functions in engine_helpers.h
- Removed ~1,680 lines of duplicated/misplaced code from engine_messages.cpp (~515 lines), engine_settings_ui.cpp (~811 lines), engine_midi_ui.cpp (~332 lines)

### Documentation

- Added Coding Patterns section to docs/Development.md (file organization convention, where to put new code, shared helpers)
- Added Common Pitfalls section (BS_OWNERDRAW controls, HWND_NOTOPMOST, wide strings, descriptor heap ordering, thread safety, sampler architecture, Shadertoy mode flags, IPC window titles)
- Added DX12 Rendering Pipeline section (render target ping-pong, binding layout, key differences from DX9)

### Bug Fixes

- Fixed folder picker dialog opening behind PresetsWindow (now passes caller's HWND as owner)

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
- Convert & Apply button: GLSL->HLSL conversion + live preview in one click
- Save .milk3 button: export converted shaders as portable JSON presets
- Automatic .milk3 name suggestion from Shadertoy project name
- Comprehensive GLSL->HLSL converter handling matrices, structs, vector comparisons, array params, and more
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
- Accessible from Welcome window and Settings -> About tab

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
- MIDI window accessible from Settings -> System -> MIDI button, runs on its own ToolWindow thread
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

- Clarified project attribution and derivation chain (MilkDrop2 -> BeatDrop -> Milkwave -> MDropDX12)
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

- Refactored source directory: `vis_milk2/` -> `mDropDX12/`, `Milkdrop2PcmVisualizer` -> `App`
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
