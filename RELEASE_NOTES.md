# MDropDX12 v3.1.0

**v3.0 made every screen able to hold its own preset. v3.1 is about making that
survive a long night.**

Almost everything here is memory, frame rate or not-crashing. There are new
features, and they are at the bottom, because they are not the point of the
release.

A note on how this is written: where a change came out of a measurement, the
measurement is quoted. Several of the decisions below reversed an earlier
belief held confidently for weeks, and the numbers are the only reason they
moved. `docs/multidisplay_cost.md` is the long form of that for multi-display
rendering specifically.

Special thanks to [IkeC](https://github.com/IkeC) for
[Milkwave](https://github.com/IkeC/Milkwave) — the reference visualizer, and the
collaboration that keeps both projects honest. Thanks to
[Incubo_](https://github.com/OfficialIncubo) for
[BeatDrop](https://github.com/OfficialIncubo/BeatDrop-Music-Visualizer) testing
and comparison reports.

---

## Memory

Video memory grew for the life of the process. On a long unattended session it
ran the card out, and the low-VRAM guard closed the visualizer — which looked
like a clean exit and was really a resource leak arriving.

Four separate causes, found in order, each one only visible once the one before
it was gone.

**The texture cache never evicted anything.** Both of its limits — a byte
budget and an image count — were read from `settings.ini` at startup, written
back at shutdown, and compared against nothing in between. The eviction
function existed and had no callers at all. Measured: fifty presets from one
library took the cache from 2 MB to 3,228 MB against a configured 2,000 MB
limit, and a 1,270-preset sweep reached 13.4 GB before the app shut itself
down. At roughly 70 MB a distinct preset, an ordinary session on a 30-second
cycle exhausts a 15 GB card in under two hours.

The budget is now enforced on every texture load. Eviction is not simply
"drop the oldest": age turned out to be a poor proxy for "nobody is using
this", because a `.milk2` keeps two presets resident and a display running its
own preset is on its own timeline entirely. Instead every shader set that could
draw next frame is asked which textures it has bound, and a bound texture is
never dropped whatever its age.

**The cache was measuring textures at about 70% of their real size.** It
counted the base image only, and every texture carries a mip chain — which is
exactly 4/3 of the base level, plus 64 KB alignment per subresource. So
`MaxBytes=2000` was really holding about 2.8 GB. Sizes now come from the
graphics device rather than from width × height × 4. That alone took the
plateau down by 794 MB, and it explained a long-standing mystery: a
consistently unaccounted ~750 MB that had been attributed to several other
things over several weeks. The cache was 1,877 MB, and 1,877 × 0.40 is 750.

**Textures loaded from disk are now capped at 1024 pixels on the longest
edge**, downscaled on load with a box filter. This one is a deliberate default
rather than a bug fix. A modern photo used as a preset texture is 4000 pixels
wide and is being sampled onto a mesh; the resolution is not visible, and it
costs 16× the memory of a 1024 version. The cap is a setting and can be raised
from a remote if a particular library needs it. Measured on a real library: the
VRAM plateau moved from ~3,100 MB to ~1,455 MB, with 70 textures resident
costing 219 MB where the same 70 would previously have been about 4.3 GB.

**Shadertoy presets allocated their feedback buffers whether or not they were
used.** A `.milk3` preset can declare up to four feedback buffers; the engine
allocated all of them, in 32-bit float, for every rendering surface, from
startup, for the life of the process — 128 bytes per pixel per surface. Now
they are allocated when a `.milk3` actually loads, only the ones the preset
declares, and given back after a spell with no `.milk3` on screen. The release
is debounced so that stepping past one Shadertoy preset does not churn.

Measured at 1600×2600:

```
classic .milk only                547.0 MB
stepping another classic .milk    547.0 MB   (+0.0)
first .milk3                      670.9 MB   (+123.9, against 634.8 for a fixed ten)
back to classic, before delay     670.9 MB   (+0.0, debounce holding)
after the delay                   547.1 MB   (-123.8, released)
a .milk3 again                    670.9 MB   (+123.8, reallocates cleanly)
```

**Descriptor heap.** The shader-resource heap was too small to put an
independent mirror on every display — measured on a live instance, exactly two
fit and the third failed 76 times with "heap full", after which that display
drew the missing-texture placeholder while the main window looked fine. The
heap is larger, retired displays hand their reservations back, and a diagnostic
now reports when two displays' reservations overlap, which is a defect that
used to be invisible.

**Mirror swap chains** now size themselves from the VRAM budget rather than
from a fixed number.

---

## Stability

### Recovering from a lost graphics device

A GPU timeout — Windows resetting the display driver, usually because one frame
took too long — used to close the visualizer. It now rebuilds and carries on.
That is the intended design and the exit was a regression, not a policy.

Several of the specific ways the device was being lost are also gone, and they
were all the same mistake in different places: **freeing something the GPU was
still reading.**

- A sprite texture could be released mid-frame while a submitted command list
  still named it. Sprite textures are now retired on a fence.
- Loading a preset could free GPU state that a display's render list was still
  using.
- Turning mirror watermark off and on took the device down on some machines.
- A mirror's own snapshot could be freed mid-frame.

When a device removal does happen, the crash report now says which draw call
hung and which surface submitted it, rather than a generic device-removed
error.

### One preset must not be able to end the session

The heaviest preset in the test library costs 26–31.8 ms of GPU per frame,
against 75–130 µs for an ordinary one — a spread of 200–350×. There is now a
per-preset frame-rate ceiling, so a single expensive preset lowers its own rate
instead of running the machine into a timeout.

### Displays that render independently

Each display running its own preset has its own simulation, and several things
that were per-engine had to become per-display before that was safe: the
composite mesh, the warp and composite shader constants, the blur passes, the
texture bindings, and the `.milk2` blend field. While these were shared, one
display's preset could write into another's — the visible symptom being
"presets are rendering but they're incorrect", which is precisely what a shader
reading another preset's textures looks like.

A related fix this release: two displays each set to their own preset could end
up rendering the *same* preset, because their reservations in the descriptor
heap overlapped and whichever wrote last won. Each display's own bookkeeping was
entirely correct throughout — right preset, right shaders, own frame counter —
which is what made it hard to see. The per-display **messages** stayed correct
too, and that detail is what identified it.

### Threading

Window threads were reading engine state while the render thread rewrote it.
The Resource Viewer walked the texture list from the settings window's thread
while the render thread added to and removed from it; the diagnostic that
reports cache size did the same from the remote-control thread. Both now read a
snapshot the render thread publishes.

Worth being straight about this one: a soak test driving that race hard — 1,788
preset loads against 60,535 refreshes in 90 seconds — does **not** crash the
old build. These particular mutations corrupt without faulting, so the symptom
was wrong rows and an occasional crash rather than a reliable one. The fix is
justified structurally, not by a reproduction, and the guard against it coming
back is a source-level check rather than the soak.

---

## Frame rate

### Separate processes per display are gone, and that was the performance win

v3.0 already rendered every display from one process. v3.1 finished removing
the alternative and wrote down why, because the belief that heavy presets
should be isolated into their own process had survived for weeks on plausible
reasoning and no measurement.

Measured, it was backwards. Windows holds a window that is not focused to about
66 frames a second, so a display in its own process ran at a third of the rate
you asked for no matter how much hardware was idle. Three independent
simulations in one process cost **+100 MB of VRAM — 0.66% of a 15 GB budget —
and 75–130 µs of GPU per surface.** Moving the heaviest preset in the library
back in-process took the side displays from 15.4 and 15.8 fps to 26.1, with GPU
per surface down 3.6×.

Every argument that had been recorded in favour of multiple processes turned
out to describe a bug in the single-process path — bugs which are now fixed.
`docs/multidisplay_cost.md` has the full record, including the measurement
traps that produced the wrong answer the first three times.

### Adaptive frame-rate balancing

When several displays are competing and one is starving, the engine now steps
down through a short ladder — lower the cap, then fall back to the cheapest
copy that fits — rather than isolating anything. Two deliberate rules:

- **Starvation must be sustained, not sampled.** A single slow frame is not a
  problem; a second of them is.
- **It must not react to a rate Windows imposed.** A backgrounded window sitting
  at 22 fps with the GPU at 20% is not distressed, it is unfocused, and treating
  that as starvation made it worse. The ladder is gated accordingly.

The triggers scale with the frame rate you asked for, rather than being fixed
numbers that only made sense at 60.

### Smaller ones

- The engine no longer holds its mirror lock across presenting a frame.
- Mirrors are paced on measured GPU time rather than wall-clock time.
- Mirrored displays no longer show bands of an older frame after a resize or a
  profile load — the buffer wipe that clears them had not run since August.

---

## Settings that stay where you put them

A loaded display profile used to *lend* its main-window settings — preset
order, cycle interval — to the session, keeping your own values hidden
underneath to be restored at shutdown. That gave those settings two values, and
different parts of the program disagreed about which was real: saving a profile
recorded one, shutting down recorded the other, and only some of the ways you
can change the setting cleared the loan.

The result was a setting that would not stay put. It is now one value: loading a
profile changes the setting, the same as changing it anywhere else, and saving
records what is actually in use.

Also fixed: a per-display **Keep on top** that was stored but never applied
until you toggled it off and on; **Next preset** skipping displays holding their
own preset; and a display profile silently dropping several per-display settings
when it loaded.

---

## Presets that would not load before

- Sampler declaration lists, `sampler_rand` in all four spellings, `tex1D`, and
  preset-declared sampler addressing.
- Struct member defaults and preset-level globals.
- DDS and TGA files that Windows' own imaging component refuses to decode.
- Textures kept in a folder *beside* the preset folder, which older MilkDrop
  libraries rely on and which this engine had stopped looking in.
- Random textures with a prefix now reach your own collection rather than only
  the bundled set.

---

## Also in this release

- **The idle timer can set up the whole room.** A fourth action loads a display
  profile, so walking away can rearrange every screen rather than just putting
  one window fullscreen. A separate option takes every display to full opacity,
  including ones in watermark mode. It brings screens to the front and puts the
  z-order back on wake, and does not reload a profile that is already loaded.
  All of it is drivable from a remote.
- **Delete display profiles** from the Displays window; deleted ones go to the
  Recycle Bin.
- **Per-display watermark opacity** and **Keep on top** checkboxes.
- **Scripts can drive a test and report on it**, including sweeping a whole
  library and reporting which presets failed to compile.
- `GET_WINDOW` / `GET_WINDOWS` report where the program's windows are and tell
  the render windows apart.
- A blank startup screen with an elapsed-second counter, so a slow first load
  looks like work rather than a hang.
- The shader cache no longer follows the working directory.

---

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

**New in this release: the zip comes with something to look at.** 93 community
MilkDrop presets and 25 photographs, so it does something on first run instead
of asking you to go and find a preset library.

The presets are classic-lineage `.milk` files — Flexi, Geiss, Se7enSlasher,
martin, fiShbRaiN, Stahlregen, Rovastar, Cope, Zylot, Aderrasi, Krash and
others, credited in full in `resources/CREDITS.md`. None of them needs a
texture we cannot ship: they use either no image at all or the `randNN` slots,
which the bundled photographs fill. No Shadertoy or GLSL Sandbox conversions
are included — those carry their original authors' terms, and some of those
terms forbid redistribution outright.

The photographs are Shane's own, downscaled to 1024 pixels on the longest edge
— which is the engine's new default cap, and past which the detail is not
visible once an image is sampled onto a warp mesh.

Point it at your own library any time from **Settings → Files**; nothing in the
bundle is required, and the engine will still start on a preset compiled into
the executable if you delete the lot.

## Full changelog

See [docs/Changes.md](https://github.com/shanevbg/MDropDX12/blob/main/docs/Changes.md)
for the complete list across all releases.
