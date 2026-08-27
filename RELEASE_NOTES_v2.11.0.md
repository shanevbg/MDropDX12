# MDropDX12 v2.11.0

A preset-management and settings release. Presets now keep their own settings
reliably, tool windows survive being resized, and there are two ways to rescue a
preset that washes out on a large screen.

Portable zip, x64, Windows 10 or later. No installer, no runtime to fetch —
unzip and run.

---

## Presets that wash out on a big screen

A handful of presets look right in a window and flatten to a bright glare at full
screen. Their feedback loop is braked by something measured in pixels while its
gain is not, so a bigger canvas weakens the brake and the picture runs away. A
sweep of 1266 presets found 24 of them, so it is rare — but it is not something
you can fix by adjusting the preset.

There are now two per-preset fixes, from the Presets context menu or the
Annotations window. They are independent: use either, both, or neither.

- **Canvas Limit** refuses to let the feedback canvas grow past a size you pick.
  Reliable, and it costs some sharpness.
- **Feedback Damp** *(new)* keeps full resolution and bleeds a little energy out
  of the loop each frame instead. It is scaled by the canvas, so it does nothing
  at all at or below 1024 pixels and grows from there.

Try Damp and look at the result, because on some presets it works backwards: if a
preset's shader inverts what it samples, taking energy out hands it straight
back and the picture gets *brighter*. The menu warns you when it can tell —
it reads *"Feedback Damp (inverts - will brighten)"* — but that is a hint, not a
guarantee. If Damp does not help, clear it and use Canvas Limit, which works
whatever the shader does.

Affected presets are flagged so you can find them again without hunting.

## Your preset library

- **Find duplicate presets, and delete the copies.** **Find Copies** scans by
  content, so it catches the same preset saved under different names. Deletions
  go to the Recycle Bin.
- **Play history is visible at last** — **Last Used**, **Plays** and **Time**
  columns, all sortable.
- **Details** tells you what a preset actually is: every location it is known to
  live at, the other filenames the same content is stored under, its override
  slots and tags, and any captured shader error.
- **Purge Missing** is a button now rather than a buried right-click item.
- **The Annotations search is fixed** and gains a clear button; the search and
  filter survive closing and reopening the window.
- **Presets no longer file themselves under each other.** Play counts were being
  recorded against whichever preset happened to be loaded rather than the one
  being timed, so entries collected other presets' locations and pooled their
  play counts.
- **Scratch presets stay out of the database.** Anything under a folder named
  `TEST` gets no entry, and a copy made there behaves as a genuinely fresh
  preset rather than quietly inheriting the original's settings.

## New Preset Editor

Edit the running preset's code with syntax highlighting — init, per-frame and
per-pixel, the warp and comp shaders, and all 16 waves and shapes. Apply changes
live without reloading. Shader errors map back to your own source line rather
than the generated one. Preset auto-advance pauses while the editor is open, so
a preset change cannot discard work in progress.

## Audio profiles

A preset can be given its own audio response curve, from the **Audio** dropdown
beside Shader and VFX. Presets written for a different spectrum shape can look
unreactive here — one that draws a ring straight from the spectrum may barely
move — and switching its profile brings it back to life.

One of the supplied profiles is matched to **Milkwave Visualizer**, so a preset
tuned there reacts here the way it does at home. It was matched against the
Milkwave build current at the time and has not been rechecked since.

Set a default for everything in Settings, or attach a profile to a **tag** so a
whole collection follows one rule without editing a single preset. Nothing
changes for any other preset: the default profile is this build's existing
behaviour exactly.

## Windows that behave when you resize them

- **Resizing no longer throws away what you were doing.** Dragging the edge of a
  tool window used to rebuild its controls, discarding typed text, list
  selection, scroll position and focus.
- **Sliders fill the window.** The Colors and Visual windows stretch their
  controls to the width you gave them instead of leaving them stranded.
- **Tool windows lay out properly** rather than leaving labels behind at their
  build-time positions.

## Settings

- **Settings writes no longer stutter.** Changes are collected in memory and
  written about once a second, so a value that changes rapidly costs one write
  instead of hundreds.
- **A test run no longer changes your settings.** Anything written while testing
  mode is on goes to memory and is discarded when it ends.
- **Keep your settings in the registry instead of a file**, if you prefer — put
  `UseRegistry=1` in `useregistry.ini` beside the executable.

## Shader errors

- **They can be copied.** A proper window with a **Copy** button instead of a
  message box, with the compiler's noisy address prefix stripped so the line and
  column lead.
- **They are recorded against the preset that produced them.** Compilation runs
  while the previous preset is still on screen, so errors were being attributed
  to whatever preset preceded the broken one — permanently. Existing wrong flags
  are not cleared automatically; clear one with right-click → **Clear All Flags**.
- **Long errors are no longer truncated to garbage.** An error list over 1024
  characters overflowed a buffer and was reported as uninitialised memory.

## Removed

Two synthetic effects — a glow halo and a set of string ribs — along with the
Video Effects window's Rendering tab, the `Md3GlowStrength` / `MartinRibCore` /
`MartinRibHalo` settings and the `SET_TUNABLE` / `GET_TUNABLES` commands. Both
were stand-ins for a shader some presets do not carry, and they drew over the
preset rather than under it. If you liked what they did, a custom shader
override supplies a real warp shader instead.

---

Full detail in [docs/Changes.md](docs/Changes.md).
