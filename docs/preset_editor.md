# Preset Editor

Edit the running preset's code — with syntax highlighting, line numbers and
folding — and hear and see the change immediately.

## Opening it

| Where | How |
|---|---|
| Presets window | Select a preset, click **Edit** (tooltip: *Edit Preset*) |
| Presets window | Right-click a preset → **Edit Preset...** |
| Annotations window | Select a row, click **Edit** |
| Hotkeys (Ctrl+F7) | Bind **Open Preset Editor** — unbound by default |
| Settings → Tools | **Open Preset Editor** |

The editor always edits the **running** preset. Opening it from a list first
loads the preset you picked, then opens on it. The hotkey opens on whatever is
already on screen, without changing anything.

## The navigator

A tree down the left side, with the waves and shapes collapsed by default so
what you see first is the handful of sections most presets actually use:

```
Whole Preset          the entire preset, code grouped under [section] headers
Raw File              the .milk exactly as it is written
Preset
  Init / Per-Frame / Per-Pixel
Shaders
  Warp / Comp
Waves
  Wave 1 .. Wave 16   Init / Per-Frame / Per-Point
Shapes
  Shape 1 .. Shape 16 Init / Per-Frame
```

Custom shapes have no per-point code, so shapes have two entries each, not
three. MDropDX12 supports 16 custom waves and 16 custom shapes where MilkDrop 2
had 4; presets written for MilkDrop only ever fill the first four.

Switching sections keeps your unsaved edits in the sections you left.

## Whole Preset and Raw File

**Whole Preset** shows the entire preset in one document. The header lines and
`[preset00]` parameters appear as they are, and each run of numbered code keys
is lifted under a header with the prefix stripped — so this:

```
per_frame_init_1=n = 0;
per_frame_init_2=loop (4096, megabuf(n)=0; n=n+1);
```

reads as this:

```
[per_frame_init]
n = 0;
loop (4096, megabuf(n)=0; n=n+1);
```

Fold the headers (right-click → Options → Fold All) and the whole preset
collapses to its structure — every section on one screen.

**Raw File** shows the same content untransformed, prefixes and all, for when
you want to see exactly what is on disk.

Applying either replaces the whole preset, parameters included — so you can edit
`fDecay` or `nWaveMode` here, not just code. Embedded `[SPRITEn_BEGIN]` blocks
are carried through untouched.

## Apply vs. Save

**Apply** pushes what you are looking at into the running preset. From a code
section that recompiles just that section (plus any other section you have left
modified); from Whole Preset or Raw File it replaces the entire preset. The
visual changes at once, and nothing is written to disk.

**Revert** throws away your edits to the current section and reloads it from the
running preset.

**Save** applies everything, then writes the preset back to its own file.
**Save As...** writes it somewhere new.

Because Apply changes the *running* preset and not the file, closing the editor
without saving leaves your changes on screen — they last until the next preset
load. Save if you want to keep them.

While the editor is open, **preset auto-advance is suspended**, so a preset
change cannot throw away work in progress. Closing the editor resumes it.

## Templates

Right-click → **Templates** offers starter snippets for whatever section is
open — a beat-reactive zoom for per-frame, a circle for a custom wave, an empty
`shader_body` for a shader, and so on. Inserting one replaces the selection, or
drops it at the caret; it never wipes what is already there.

Whole Preset and Raw File get no templates: a snippet has no single right place
to land in a whole preset.

## Expand

Right-click → **Expand Statements** puts each statement on its own line. MilkDrop presets are usually
written with several statements per line (`zoom=1; rot=0; cx=0.5;`), which is
compact but hard to read and hard to spot an error in.

It only breaks at top-level semicolons — never inside parentheses, a string, or
a comment — so a `for` header or a function call is left alone. It is a plain
edit: **Revert** undoes it, and so does Ctrl+Z.

## Shader errors

When a warp or comp shader fails to compile, the message appears in the status
strip at the bottom and the failing line is **underlined in red**, with the
caret moved to it.

The line number is translated back to *your* source. The engine prepends its
whole include file and injects a few lines inside `shader_body` before handing
the text to the compiler, so the compiler's own line numbers are dozens of lines
off from what you typed; the editor undoes that shift.

If a shader fails, the preset keeps rendering — the engine falls back to its
built-in shader rather than going black.

## Right-click options

Right-click **anywhere in the window** — the code, the tree, the button row —
for Undo / Redo / Cut / Copy / Paste / Select All, **Expand Statements**,
**Templates**, **Save As...**, and an **Options** submenu:

- **Line Numbers** — show or hide the line-number margin
- **Code Folding** — show or hide the fold margin
- **Fold All** / **Unfold All**

Both toggles are remembered between sessions.

Folding is brace-based, so it earns its keep in the warp and comp shaders.
Equation sections rarely contain braces and so rarely fold.

## Highlighting

Two languages, picked automatically from the section:

- **Equations** (ns-eel2) — the preset variables MilkDrop defines (`bass`,
  `zoom`, `q1`–`q32`, `t1`–`t8`, `reg00`–`reg99`, …) are coloured differently
  from your own locals, which makes a typo in a magic name obvious.
- **Shaders** (MilkDrop-flavoured HLSL) — types, intrinsics, the MilkDrop
  samplers and uniforms (`uv`, `rad`, `sampler_main`, `texsize`, …).

## .milk2 double presets

A `.milk2` holds **two** complete presets plus a blend header, and renders them
both at once at a fixed blend position. Load one and the editor grows a
**Preset 1 / Preset 2** tab strip above the section tree.

Both are fully editable. Everything works the same on either tab — the tree,
Whole Preset, Apply, templates — and because the blend is frozen rather than
animating, an edit to *either* preset is visible immediately.

- **Preset 1** is the blend-from preset, **Preset 2** the blend-to. The status
  strip names which one you are in.
- **Save** writes a proper `.milk2`: both presets, the blend header
  (`blending_pattern`, `blending_progress`, `blending_direction`, any
  `random_N`), and any embedded sprite blocks. **Save As** offers `.milk2` by
  default; choosing a `.milk` name writes only the preset you are on.
- **Raw File** shows the whole wrapper for reading. It is the one thing Apply
  cannot take, because the preset loader reads one preset at a time — edit
  through Whole Preset or the sections instead.

Switching tabs discards edits you have not applied, since they were written
against the other preset's buffers; the status strip says so when it happens.

## Sprites

MilkDrop 3.25+ embeds sprites in the preset itself, as
`[SPRITEn_BEGIN]` … `[SPRITEn_END]` blocks holding the sprite's placement plus
its own `code_` lines. MDropDX12 reads them (`Engine::ParseEmbeddedSprites`).

They show up in **Whole Preset** and **Raw File** and survive a round trip
untouched, so you can edit them there. They are not in the tree as their own
sections — the tree covers what lives in the preset's render state.

The separate `sprites.ini` sprite set (`[imgNN]` sections) is a different
feature, edited in the Sprites window.

Either way the image itself is a **file reference** (`SpriteName=sprites\x.png`),
never embedded data. Textures are likewise referenced by sampler name and loaded
from the textures directory.
