# Custom shader overrides

A **shader override** is a warp and/or comp pixel shader you wrote or imported,
used instead of a preset's own shader. Which preset gets which override is
decided by tags.

This is what MilkDrop 3 PRO does with its `MD31` key: a value in the preset
selects a shader from a store outside the file, and the preset's own shader text
is never compiled. Two of its presets show it plainly — `Rainbow Butterfly1` and
`Rainbow Butterfly2` are byte-identical apart from a comment and that key, and
they render as two different presets. The difference here is that the store is
yours, the shaders are editable text files, and **no preset file is ever
written to**.

Open the window with the **Open Custom Shaders** hotkey (unbound by default —
bind it in Hotkeys, `Ctrl+F7`).

---

## How a preset gets an override

1. Tag the preset. Tags live in `presets.json`; set them from the preset browser.
2. Add a rule: some tags → one override.
3. Load the preset. The window's status line names what it resolved to.

**Matching:** a rule matches when the preset carries **any** of the rule's tags.
When several rules match, the **first one in the list wins** — order is the
priority, which is what the Up/Down buttons change. One preset therefore
resolves to exactly one override, so two overrides can never fight over the same
shader slot.

An override may fill the warp slot, the comp slot, or both. An empty slot means
"keep the preset's own".

## One preset, different treatment

Tags are meant to stay generic. When a single preset wants something different,
give it its **own** override instead of inventing a tag that names one file —
the **Shader** and **VFX** dropdowns on the Annotations window do this for the
selected preset.

Each has three settings, and the middle one matters:

| setting | meaning |
|---|---|
| `(inherit from tags)` | whatever this preset's tags select — the default |
| `(none)` | **nothing**, even though a tag rule matches |
| a name | that override or profile, whatever the tags say |

`(none)` is how one preset opts out of a tag it otherwise belongs to.

## Video effects

A rule may also name a **VFX profile**, with or without a shader override — a
rule with only a profile changes video effects and leaves the preset's own
shaders alone. Set it from the **VFX profile** dropdown on the Custom Shaders
window.

The shader and the profile resolve **independently**, each in the same order:

1. the preset's own setting, from the Annotations window
2. the first matching tag rule
3. nothing

So a preset can take its shader from a generic rule while naming its own
profile.

**A profile applied this way is temporary.** Your video effect settings are
remembered before the preset loads and put back when you leave it, so a preset
cannot permanently move settings you did not change yourself.

If you change a video effect *while* such a preset is on screen, the change is
kept rather than silently undone, and leaving the preset asks whether to
**Keep** it in that profile or **Discard** it. `vfxprofiles.json` is written
only if you press Keep.

Turning shader overrides off disables all of this, per-preset settings
included, and restores your video effects.

---

## Where things live

```
resources/shaderoverrides.json    names, rules, master enable
resources/shaders/<name>.warp.hlsl
resources/shaders/<name>.comp.hlsl
```

```json
{
  "version": 1,
  "enabled": true,
  "overrides": {
    "Rainbow Glow": { "warp": "rainbowglow.warp.hlsl",
                      "comp": "rainbowglow.comp.hlsl", "notes": "" }
  },
  "rules": [
    { "tags": ["butterfly", "glow"], "override": "Rainbow Glow", "enabled": true }
  ]
}
```

Shader text is kept in real files, not inside the JSON, so it can be edited with
syntax highlighting, diffed, and kept in version control. **Edit warp** /
**Edit comp** open the file in whatever your system uses for `.hlsl`; **Reload**
re-reads everything and recompiles without restarting.

Members this build does not recognise are written back out untouched, so a
newer format is not erased by an older build.

## Getting a shader in

* **New...** — an empty override, to point at files later.
* **Import .hlsl...** — copies a `.hlsl`/`.fx` into `resources/shaders` and fills
  the warp or comp slot.
* **From preset...** — lifts the `warp_1=` / `comp_1=` blocks straight out of a
  `.milk`, which makes "use the shader off that preset I like" a two-click job.

## Applying one by hand

**Apply to preset** puts the selected override on whatever is running, with no
rule involved; **Revert** puts the preset's own shaders back. Neither is
remembered — a rule is how you make it stick.

---

## Things worth knowing

**Preset files are never modified.** The override's text is held beside the
preset state, never inside it, because saving a preset writes its shader text
back into the `.milk` — staging an override there would bake someone else's
shader into your file.

**A broken shader falls back to the preset's own**, and the window says which
override failed. One typo in a shader attached by rule to 200 presets must not
take all 200 down.

**Overrides compile at `ps_3_0` or better**, whatever the preset asks for.
`ps_2_a` silently drops texture bindings in complex shaders.

**Shadertoy (`.milk3`) presets are not affected.** They render through a
different path with no warp or comp pass; the window says so rather than showing
a rule that could never fire.

**An override is the only way to fill an `MD31` preset's warp slot.** Those
presets have a stub warp shader — MilkDrop 3 PRO renders them from its own
shader cache — so what the file contains is a placeholder. This renderer used to
substitute two invented stand-ins for the missing shader, a glow halo and a set
of ribs. Both are gone: they were a poor guess at shaders nobody had, and MD3's
frame never contained either. An override is how you supply the real thing.

**Toggle Shader Overrides** (also unbound by default) flips the master enable
and re-resolves immediately.

---

## Preset identity

Tags, ratings, notes and play counts are keyed to a preset's **content**, not
its filename, so they follow it when you move, copy or rename it. The key is a
64-bit FNV-1a hash of the preset's normalized body:

1. Split the bytes on `\n`; drop a trailing `\r` from each line.
2. Drop any line whose key is `fRating` (case-insensitive, leading whitespace
   ignored).
3. Strip trailing whitespace from every line.
4. Drop leading and trailing blank lines; keep interior ones.
5. Join with `\n` and hash.

`fRating` is excluded because other MilkDrop-family programs write it back into
preset files, and rating a preset elsewhere must not change what it is here. It
is the only exclusion — `MD31`/`MD32` count, because their value picks which
cached shader MilkDrop 3 PRO renders, which makes two files with one body and
two `MD31` values two different presets.

**This rule cannot change.** The hash IS the identity, so altering the rule
would detach every `presets.json` entry from the preset it describes. It did
change once, on 2026-08-20, when `MD31`/`MD32` stopped being excluded — that was
affordable only because no release had shipped with the old rule.

Editing a preset does move its hash — that is unavoidable. The filename is kept
as a fallback key for exactly that case: content changed → hash misses →
filename hits → the entry is re-stamped with the new hash and nothing is lost.
The two keys cover each other, since the hash survives moving and renaming while
the filename survives editing.

## Ratings

A rating is a list of dated observations, one per version of the preset, and the
number shown is their average. Rate a preset 5, edit it, rate it 2, and both are
kept — so a preset that changed under you does not end up with one rating you
cannot explain. Ratings are stored in `presets.json` only; the `.milk` file's own
`fRating` is used only for presets you have never rated.

## Usage

`presets.json` records when a preset was last played, how many times, and how
long in total. A preset counts as played once it has been on screen for five
seconds, so cycling past forty presets does not log forty plays. **Reset Use** on
the Annotations window clears it, for one preset or all of them.

---

## IPC

| command | effect |
|---|---|
| `SHADER_OVERRIDE_APPLY=<name>` | apply an override to the running preset |
| `SHADER_OVERRIDE_REVERT` | restore the preset's own shaders |
| `SHADER_OVERRIDE_ENABLE=0\|1` | master enable |
| `SHADER_OVERRIDE_STATUS` | what resolved, from which rule, and any failure |
| `SHADER_OVERRIDE_RELOAD` | re-read the store and every shader file |
| `PRESET_OVERRIDE_STATUS` | what the running preset resolved to, and from where |
| `SET_PRESET_SHADER_OVERRIDE=<name>` | this preset's own override; empty means none |
| `SET_PRESET_VFX_PROFILE=<name>` | this preset's own VFX profile; empty means none |
| `CLEAR_PRESET_OVERRIDE=shader\|vfx` | drop the setting so the tags apply again |
| `VFX_SCOPED_STATUS` | the active scoped profile, and whether it has unsaved edits |
| `VFX_SCOPED_KEEP=0\|1` | answer the Keep/Discard prompt |
| `RELOAD_ANNOTATIONS` | re-read `presets.json` (tags decide overrides) |
| `GET_PRESET_HASH[=<path>]` | content identity of a preset |
| `SET_PRESET_RATING=<0-5>` | rate the running preset |
| `GET_PRESET_RATING` | effective rating, and whether it came from this install or the file |
