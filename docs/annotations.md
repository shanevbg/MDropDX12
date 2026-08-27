# Annotations

Everything MDropDX12 remembers about a preset — rating, flags, notes, tags, play
history, per-preset overrides and any shader error it hit — lives in
`presets.json`, and the Annotations window is where you read and edit it.

It is also the one place that can see your whole library at once rather than one
folder at a time, which is what makes it the right place to find duplicates and
dead entries.

## Opening it

| from | how |
|---|---|
| Settings | **Tools** tab → **Annotations** |
| Presets window | right-click a preset → **Annotations...** |
| Hotkey | **Open Annotations** (unbound by default — bind it in Ctrl+F7) |

## The list

| column | what it shows |
|---|---|
| Preset | the preset's filename |
| Copies | how many files on disk share this preset's content — blank until **Find Copies** has run |
| Rating | your rating, averaged across versions (see [Ratings](custom_shaders.md#ratings)) |
| Flags | ★ favorite, ⚠ error, ⊘ skip, ✖ broken |
| Last Used | when it was last played |
| Plays | how many times it has been played |
| Time | how long it has been on screen in total |
| Notes | your note, with `(file missing)` in front when the preset's file is gone |

Click any column heading to sort by it; click again to reverse. Sorting uses the
underlying values, not the displayed text, so Rating orders by number rather than
by star glyph and Last Used orders chronologically. Presets that have never been
played sort as the oldest, so sorting **Last Used** ascending brings everything
you have never watched to the top.

### Finding a preset

**Find** filters the list as you type, matching the filename *or* the note, so a
preset can be found by something you wrote about it. Type `*` or `?` anywhere in
the box and it switches to a wildcard match instead of a substring one.

The **✕** button beside the box clears the search. The search survives closing
and reopening the window — the box shows what it is still filtering by.

**Filter** narrows the list to Favorites, Errors, Skipped, Broken, or
**Duplicates** — presets that exist as more than one file, which is empty until a
scan has run.

## Finding duplicate copies

Preset collections accumulate copies: the same preset downloaded twice, renamed,
or copied into a themed folder. **Find Copies** reads every `.milk`, `.milk2` and
`.milk3` under your preset folder and its subfolders, and groups them by
**content**, not by name — so `Aurora.milk`, `Aurora (2).milk` and
`aa_renamed.milk` are recognised as one preset in three places.

Identity is the same content hash `presets.json` is keyed on, which means a copy
whose only difference is its `fRating` still counts as the same preset — other
MilkDrop-family programs rewrite that field, and it deliberately does not affect
identity.

The scan only reads; it changes nothing. On a large library it takes a while, and
the progress dialog can be cancelled.

### The report

Each group is numbered, one row per file, showing the folder, the modified date
and the size. Tick the copies you want gone and press **Delete Ticked Files...**.

- **Tick: All but newest** keeps the most recently modified copy in each group.
  The file bodies are identical by definition, so the only thing separating them
  is which one you have been touching.
- **Tick: Outside preset folder** ticks copies that are not in the folder you are
  currently browsing — and never touches a group that has no copy there.
- **Copy Report** puts the whole grouping on the clipboard as text, with full
  paths, if you would rather act on it yourself.

**Deleted files go to the Recycle Bin, not permanently.** The window will not let
you delete every copy of a preset: if you tick a whole group it refuses and says
which one. Ratings, tags and notes are attached to the preset's content rather
than to a path, so they follow whichever copy survives.

## Tidying the database

**Purge Missing** drops entries whose preset file no longer exists. It edits
`presets.json` only and never touches a file on disk — deleting files is the
duplicates report's job, deliberately kept somewhere else.

An entry is only "missing" when it records one or more locations and the file is
at none of them. Entries that were never seen at a recorded path are left alone:
having no recorded location is not evidence that a file is gone. The prompt tells
you how many will go before you agree, because ratings, notes and play history go
with them and cannot be recovered.

## Per-preset overrides

The five dropdowns below the buttons apply to the selected preset:

| slot | what it selects |
|---|---|
| Shader | a custom warp/comp shader override — see [custom_shaders.md](custom_shaders.md) |
| VFX | a Video Effects profile |
| Audio | which engine's audio behaviour feeds the preset |
| Canvas | a long-edge cap on the feedback canvas, in pixels |
| Damp | how hard to bleed energy out of the feedback loop |

Each has **three** states, and the middle one matters:

- **(inherit from tags)** — nothing is set here; whatever the preset's tags
  select applies.
- **(none)** — explicitly nothing, which *suppresses* a rule the preset's tags
  would otherwise trigger. This is how one preset opts out of a tag it legitimately
  carries.
- a name — that specific override, regardless of tags.

Canvas and Damp are the exception: they are plain values, not three-state slots,
and they are **two answers to the same question**, described next.

## Canvas and Damp: two fixes for a runaway feedback loop

A small number of presets look right in a window and wash out to a flat glare
when the canvas grows. The cause is a feedback loop whose brake is spatial
transport measured in *texels* while its gain is not: each frame the image
spreads by roughly a fixed number of texels, and on a bigger canvas that spread
covers a smaller fraction of the picture, so the brake weakens and the loop
diverges. A measured sweep of 1266 presets on 2026-08-26 found 24 of them.

Two settings address it and they are independent — use either, both, or neither:

- **Canvas** refuses to let the feedback canvas grow past a long edge, putting
  the preset back where its author balanced the two rates. Reliable, and it
  costs sharpness: the picture is upscaled to the window.
- **Damp** leaves the canvas at full size and multiplies the feedback buffer
  down by a small amount every frame, replacing the brake the larger canvas took
  away. Full resolution, and whether it looks right is a judgement about the
  particular preset.

Damp is scaled by the canvas, not by a number tuned to one screen:

    lost = 1 - 1024/longEdge      how much of the authored per-frame transport
                                  this canvas has taken away
    damp = 1 - strength * 0.15 * lost

At or below a 1024 px long edge the multiplier is exactly 1.0 and the setting
does nothing at all. The five choices — Off, Gentle, Medium, Strong, Maximum —
are `strength` from 0 to 1.

### Damp can make a preset worse, and it is not a bug

Try it and look. On some presets the damp is not merely ineffective, it is
backwards: the picture gets *brighter* the harder it is damped, right up to a
white screen. That happens whenever the preset's composite shader is a
**decreasing** function of the feedback it samples, and two forms of that are
common:

- a plain inversion, `ret = 1 - ret` as the last line of the comp shader;
- a solarize fold such as `ret = 1 - ret*(1-ret)*4`, which is `(1-2·ret)²` — its
  slope is negative for anything below mid grey.

Take energy out of the loop and both of those hand it straight back. Measured on
`rarian rakista - Eyes through the ether`, the mean brightness went 0.49 → 0.87 →
0.96 → 0.98 as the multiplier was walked 1.00 → 0.90 → 0.80 → 0.70.

A third case looks like noise and is not. A loop with no multiplicative loss and
a gain just above 1 is **bistable**: the damp is an ignition switch rather than a
brightness knob, so the same preset with the same setting can look different from
one run to the next, and a measurement that samples it once will disagree with
itself.

The Damp menu warns when the running preset's composite shader ends in an
inversion or a fold — it reads as **Feedback Damp (inverts - will brighten)**,
and `GET_CANVAS_MAX` reports the same thing as `compInverts=1`. That catches the
common cases, but it is a text scan over arbitrary HLSL, so treat it as a hint
and still look at the frame. Neither mitigation is ever applied automatically.

Judge the damp on whether the picture stops *climbing* over a minute, not on how
bright it is in the first few seconds. These loops take one to three minutes to
reach their resting state, and a preset with no multiplicative loss can look
completely different from one run to the next until it settles.

There is deliberately no "boost" setting. A preset that brightens under damping
is not short of energy — its shader inverts what it samples, and multiplying an
already-diverging accumulator by more than one is a detonator, not a mitigation.
Use **Canvas Limit** for those: it works regardless of what the shader does with
its feedback, at the cost of sharpness.

Both settings are also reachable from the Presets browser's context menu, and
the **canvas** flag marks a preset as having the problem whichever fix is
chosen.

### Over IPC

| command | effect |
|---|---|
| `SET_CANVAS_MAX=<px>` | global long-edge ceiling; 0 = none |
| `SET_PRESET_CANVAS_MAX=<px>` | canvas cap for the current preset; 0 clears |
| `PRESET_CANVAS_MAX_CLEAR` | clears it |
| `SET_PRESET_DAMP=<0..1>` | damp strength for the current preset; 0 clears |
| `PRESET_DAMP_CLEAR` | clears it |
| `SET_DAMP_OVERRIDE=<mult>` | a raw per-frame multiplier for **this session only** — see below |
| `GET_CANVAS_MAX` | everything at once: global, `texW`/`texH`, applied limit, `presetMax`, `damp`, `dampApplied`, `dampOverride`, `flagged` |
| `SET_PRESET_FLAG=canvas,<0\|1>` | sets or clears the canvas flag |

`SET_DAMP_OVERRIDE` is the odd one out and deliberately so. It is not a strength
on the dial — it *is* the multiplier, it beats both the annotation and the
scratch-preset rule, it is never written to `presets.json`, and it does not
survive a restart. It exists so a mitigation can be tried on the running frame
without editing a preset or relaunching, and so a measurement can walk past both
ends of the dial to find out what a preset actually needs. `SET_DAMP_OVERRIDE=clear`
removes it; leaving one set quietly damps everything afterwards.

## Details

Double-click a row, or press **Details**, for everything known about one preset:
its flags, rating, notes, usage, content hash, every location it is known to
live at, the other filenames the same content is stored under, all five override
slots, its tags, and any captured shader error with the date it was captured and
a **Copy Error** button.

## Keeping test presets out

Presets in a folder named `TEST` never get an entry in `presets.json`, so scratch
presets made while debugging do not accumulate there. Nothing is recorded while
[testing mode](Scripts.md) is on either — a measurement run parks one preset on
screen for minutes at a time, and counting that as listening would make "most
played" a report on the test rig.

Both gates only block *new* entries and *new* recording. An existing entry still
resolves and still reads normally.

Set `AnnotationIgnoreDirs` in the `[Milkwave]` section of `settings.ini` to change
the folder names (semicolon-separated, matched against a whole path segment, case
insensitive). Set it empty to record everything.

## Importing

**Import** reads another `presets.json` and shows it side by side with yours so
you can take ratings and flags across. **Scan** reads `fRatingThis` out of the
`.milk` files in the loaded list, for a collection that was rated in another
MilkDrop-family program.

## IPC

| command | effect |
|---|---|
| `DIAG_ANNOT_QUERY=<pattern>` | how many annotations the search box would match |
| `DIAG_ANNOT_RESOLVE=<filename>` | where that annotation's preset actually lives |
| `DIAG_ANNOT_MISSING` | how many entries name a location where the file is gone |
| `ANNOT_REMOVE_MISSING` | purge those entries |
| `DIAG_ANNOT_DUPES[=<root>]` | hash every preset under a root; reports groups, redundant copies and distinct presets |
| `DIAG_ANNOT_IGNORED=<path>` | whether a preset at that path would be recorded, and which gate stops it |
| `RELOAD_ANNOTATIONS` | re-read `presets.json` from disk |
| `SET_PRESET_NOTE=<text>` | set the running preset's note |
| `GET_PRESET_NOTE` | read it back |
