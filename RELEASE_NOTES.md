# MDropDX12 v3.2.0

**v3.0 gave every screen its own preset. v3.1 made that survive a long night.
v3.2 makes it actually work.**

## Why this follows v3.1 so closely

Because using v3.1 is what found these.

v3.0's headline was that a second or third monitor could hold a preset of its
own rather than mirroring the main window, and v3.1 spent its effort on making
that survive hours of running — memory, frame rate, not crashing. What neither
release did was put that feature through a week of ordinary use on a real
preset library.

Doing that surfaced five separate ways it did not hold up, and four of them put
a **black or wrong picture on a screen**:

* a display never drew motion vectors at all, so a preset whose only light
  source is motion vectors was black there while the same file was correct on
  the main window;
* a display silently lost its own preset — and rendered the main window's —
  whenever the main window happened to load a `.milk3`;
* a display handed a `.milk3` went black and stayed black, reporting success,
  and ordinary preset cycling reached that by itself;
* a display holding a preset of a different *kind* from the main window's had
  its shaders built with the wrong scaffolding, so they failed to compile, the
  display dropped to a plain default shader and compile errors flashed across
  the screen;
* and there was no way to choose what a display played, only Next and Previous.

All five are in the same feature area. Four were found by using it; the fifth
turned up while measuring one of the other four. v3.2 is the correctness release
for what v3.0 and v3.1 introduced. If you are packaging MDropDX12 and skipped
v3.1, skip straight to this one.

One of the five outgrew being a fix. Rather than teach a display to refuse a
`.milk3`, a display can now hold and render one of its own — something no screen
but the main window could do before. The only other genuinely new thing is the
ability to pick which preset a display plays, listed last because it is the
smallest part of the release.

One section below sits outside that feature area: a GPU crash no longer ends the
session.

---

## A display keeps its own preset, whatever the main window is doing

`m_bShadertoyMode` — the flag that says the *main window's* preset is a
Shadertoy `.milk3` — was being read to decide what a **display** should do, at
seven separate places. So the moment the main window loaded any `.milk3`, every
display holding its own preset stopped simulating that preset and drew the main
window's Shadertoy instead. It came back when the main window returned to a
`.milk`.

Measured, one display left on the same preset for the whole run while **only**
the main window changed:

| main window on | the display showed | mean brightness |
|---|---|---|
| a `.milk` | its own preset | 31.9 |
| a `.milk3` | **the main window's Shadertoy** | 52.6 |
| a `.milk` again | its own preset | 26.3 |

Two things hid this. The display never stalled — its frame counter advanced at
the same rate throughout — and the diagnostics went on reporting the display's
own preset while something else was on the glass. The one tool that existed for
this said nothing was wrong.

Fixed by making the decision per display. The diagnostics gained a `renders=`
field naming the pipeline that actually ran, read from the same test the
renderer uses, so it cannot drift from what is drawn.

## A display can now hold a `.milk3` of its own

Before this, handing a display a Shadertoy preset failed in the worst available
way: the file opened, matched nothing, reported **success**, and the display
went black — then cached the path so it never tried again, while the
diagnostics named the `.milk3` as what was playing.

It was not an exotic thing to do. Preset **cycling** offered `.milk3` files, and
in a typical library they sit in the same folders as everything else, so a
display working through a folder reached one on its own and went dark at a
moment of its own choosing.

A display now parses, compiles and renders a `.milk3` for itself — its own Image
pass, its own Buffer A–D feedback, its own clock — independently of whatever the
main window is doing. Two displays can run two different Shadertoy presets while
the main window runs a third.

It is cheaper than you might expect. Measured on a 2160×3840 primary with two
1440p secondaries, going from three classic presets to three Shadertoy ones
costs **+382 MB of video memory — and two thirds of that is the main window
alone.** Each extra display running its own `.milk3` adds about **65 MB**,
because the FLOAT32 feedback buffers are allocated only for the passes a preset
actually declares. Those are upper-bound figures: everything scales with
resolution, and most setups are smaller than that one.

## Shaders are built for the preset in hand, not the main window's

Four places in the shader generator shape the HLSL it produces according to
whether the preset is a Shadertoy `.milk3` or a classic `.milk`: which Shadertoy
texture declarations survive, whether the `q1`–`q32` preset variables stay
defined, whether the output is a `float3` or a `float4`, and how the final line
is wrapped.

All four asked a **global** — *is the main window loading a Shadertoy?* — which
was the right question for exactly as long as the main window was the only thing
that ever compiled a preset. Since v3.0 a display compiles its own, on its own
thread. So a display compiling a classic preset at the moment the main window
happened to be loading a `.milk3` had its `q` variables undefined out from under
it and its output type changed underneath its code, and the shader did not
compile:

```
error X3004: undeclared identifier 'q10'
error X3017: cannot implicitly convert from 'float3' to 'float4'
```

The display then fell back to a plain default shader, so the picture was wrong
rather than absent, and the errors were briefly visible on screen as they went
past. The failure was also logged against the *main window's* file rather than
the preset that actually failed to compile — which is why it was first looked
for in entirely the wrong file.

It is a race, and that is why it outlived v3.1: changing presets one after
another never reproduces it. Only an overlap does, and an overlap is an ordinary
thing to cause — it needs nothing more than changing one screen while another is
still loading.

Each compile now carries its own answer instead of asking what the main window
is doing.

## Motion vectors are drawn on every display

`DX12_DrawMotionVectors` had exactly one call site, in the main window's render
path. A display holding its own preset rendered everything else — warp, shapes,
waves, sprites, borders — and not this.

For most presets that is invisible, because something else is drawing too. For a
preset whose *only* light source is motion vectors, the display was black.

Across a 7,678-preset library: **4 presets were completely black** on a display
and **545 more were quietly degraded**, drawing motion vectors on the main
window and not on the displays — about 7% of the library rendering differently
depending on which screen it was on.

## A GPU crash no longer ends the session

Recovering from a lost graphics device rather than exiting is a long-standing
principle here, and it had regressed to exiting.

What was measured after a device hang is this: the adapter refuses **the process
that caused it** for about two minutes, while a freshly started process gets a
working device in about twenty seconds. So MDropDX12 now restarts itself.

That is not the same as proving a hung device can never be recovered in place.
The process is still holding device objects at that point, and a device stays
alive as long as its children do — so nothing has yet tested what a genuinely
complete release would do. Restarting is the right answer for a better reason
than "nothing else works": a fresh process is the only teardown that is provably
complete, and it stays complete as the code grows.

The restart carries what it needs to not simply die the same way: it waits for
the old process to exit before asking for the adapter, it steps the mirror
resolution down (1920 → 1280 → 960 → mirrors off), and it counts attempts across
the whole chain rather than giving each new process a fresh three. After three
it exits, as before. An instance that has run healthily for five minutes and
then hits a crash gets the full allowance again, because what is being bounded
is a *loop*, not a rare event.

Device losses that are **not** hangs — a driver update, a reset, an adapter
change — are still recovered in place without restarting, exactly as before.

## Choosing what a display plays

The Displays window's **Selected display** panel gained a **Playing:** row: what
that display is rendering right now, and a **...** button that opens a file
picker to put a chosen preset on it immediately.

Until now a display could only be moved with **< Prev** and **Next >**, so
reaching one particular preset meant stepping through the folder until it came
up.

The chosen preset is shown, not pinned — it does not become the display's
startup preset and does not re-point the folder it cycles through. Picking
something to watch for a minute should not quietly change what plays for the
rest of the night.

The **Random / Sequential** pair of radio buttons is now a single **Sequential**
checkbox. It was one setting spending two rows to say it; unticked means random,
which is the default. That is the row **Playing:** now occupies.

---

## Also fixed

* **Three controls in the Displays window shared ID numbers with three others**
  — "Keep on top", and the startup-profile picker and its Save button, collided
  with the Layout/Details buttons and the arrangement panel. Windows can only
  find one control per number, so the startup-profile picker was being read
  through a handle to the "Details" button.
* **The `SET_DISPLAY_PRESET` script command and the new file picker now share
  one code path.** They previously spelled the same logic out separately, which
  is how the window's Prev/Next buttons and the equivalent script commands drifted
  apart in an earlier release.
* A display could be left **stuck on the wrong renderer** after switching preset
  types, and the sim grid could keep a stale shape — visible as a squashed
  picture on one screen.

## For script and tool authors

* `DIAG_MIRRORS` gains **`renders=`** on each surface row: `classic` or `milk3`,
  naming the pipeline that actually ran. `preset=` still names the display's own
  preset, which is a different question and was the only one answerable before.
* **`DIAG_SIMULATE_TDR`** drives the device-loss recovery path without a GPU, so
  the restart chain can be tested without resetting the display driver. Testing
  mode only — it makes the application exit and start a replacement.
* `SET_DISPLAY_PRESET=<n>,<path>` accepts `.milk3` for a display now.

Full detail in [docs/Changes.md](docs/Changes.md). Earlier releases are in
[docs/releases/](docs/releases/).
