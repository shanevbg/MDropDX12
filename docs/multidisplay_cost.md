# What multi-display rendering actually costs

Everything here was measured on this machine, 2026-09-14, and every number
says where it came from. The point of the document is that several confident
beliefs about multi-display rendering in this app turned out to be wrong, and
the corrections are not obvious enough to survive as folklore.

The short version: **rendering several independent presets in one process is
cheap, and putting them in separate processes buys nothing.** Almost everything
below follows from that.

---

## 1. The numbers

### Three independent presets, one process

| | |
|---|---|
| VRAM over the same setup as mirrors | **+100 MB** — 0.66% of a 15,209 MB budget |
| GPU per surface, ordinary presets | **75–130 µs** |
| Combined throughput | 1211 fps across three surfaces at 55.8% GPU |

Corroborated three ways: toggling one display's own preset moves
VRAM by **~60 MB**; the code's long-standing figure in
`engine_displays.cpp` is **~45 MB per classic surface** at 1920x1088; and the
delta above works out at ~50 MB per extra simulation. Three independent
estimates, one answer.

### `.milk3` on every screen — measured, 2026-09-16

This document used to carry "three independent `.milk3` presets is ~800 MB",
estimated from a time when a surface could only render the *main window's*
Shadertoy preset. Three displays each holding their **own** `.milk3` became
possible in v3.2, so the arrangement can now be measured rather than inferred.
It is **less than half** the estimate.

`VMARK` (`QueryVideoMemoryInfo` local usage), same two displays and the same
primary throughout, changing only which kind of preset each holds:

| configuration | VRAM | vs floor |
|---|---|---|
| mirrors off, primary classic | 263 MB | — |
| classic ×3 (primary + 2 displays) | 1242 MB | +979 |
| `.milk3` on the **primary only** | 1495 MB | +1232 |
| `.milk3` ×3 | 1624 MB | +1361 |

| | |
|---|---|
| three `.milk3` over three `.milk` | **+382 MB** |
| …of which the **primary alone** accounts for | **+253 MB** |
| …leaving, for the two displays | +129 MB |
| **per own-preset `.milk3` display** | **+65 MB** |

**The primary is two thirds of the cost, and extra Shadertoy displays are
cheap.** A `.milk3` display costs about 65 MB more than a classic one — nothing
like the 270 MB a Shadertoy surface used to imply. The reason is #344: the
FLOAT32 feedback pairs are allocated only for the buffers a preset actually
declares, and most presets declare Image alone or Image + Buffer A, so the
"four pairs, always" arithmetic the old estimate came from no longer applies.

**Two caveats on these numbers, both of which make them an upper bound.**

*Resolution.* This rig is a 2160×3840 portrait primary with 2560×1440
secondaries clamped to a 1920×1088 canvas. Most setups are smaller, and every
figure here scales with canvas area — read them as the top of the range, not
the typical case.

*Z-order does not matter, which is worth recording because it is easy to assume
it does.* The whole table was taken twice, once with the surfaces parked at
`HWND_BOTTOM` as testing mode leaves them and once with every surface raised to
the top. The totals differ by **1.2 MB in 1361, or 0.09%**. Windows' QoS
classifier throttles an occluded window's *frame rate* — see §2, which is about
exactly that — but allocation is not gated the same way: a surface uses its
memory whether or not it is in the background. Do not carry the frame-rate rule
over to a memory measurement; it does not apply, and assuming it did is what
prompted the second run.

### What each display MODE costs — measured, 2026-09-16

The table above varies the kind of preset. This one varies the **mode**, which
is the question most users are actually on the other side of: a plain copy
mirror is the default, and it was widely assumed to be the expensive rung.

It is not. One fresh process per configuration, `VMARK` baseline after settling:

| configuration | VRAM | vs floor | surfaces |
|---|---|---|---|
| windowed, no mirrors | 263.0 MB | — | 0 |
| **fullscreen, no mirrors** | 1082.6 MB | **+819.6** | 0 |
| + two copy mirrors | 1159.9 MB | +896.9 | 1 |
| + independent mirrors | 1237.6 MB | +974.6 | 2 |
| + own preset (classic) | 1249.8 MB | +986.8 | 2 |
| + own preset (`.milk3`) | 1631.1 MB | +1368.1 | 2 |

Read incrementally, for two displays:

| step | cost | per display |
|---|---|---|
| **primary windowed → fullscreen** | **+819.6 MB** | — (no displays involved) |
| two copy mirrors | +77.3 MB | +38.7 MB — a lower bound, see below |
| making them independent | +77.7 MB | +38.8 MB |
| giving them their own preset | +12.2 MB | +6.1 MB |
| all three going `.milk3` | +381.3 MB | see the breakdown above |

**A copy mirror costs tens of megabytes, not hundreds.** Independence roughly
doubles it. A preset of its own adds almost nothing on top, because the orient
pipeline the independent rung already paid for is the expensive part.

#### A copy mirror is sized to the PRIMARY, not to its own monitor

Worth stating precisely, because it is the one place a copy mirror costs more
than it needs to, and because it makes the per-display figure *variable*.
`MirrorSwapChainSize`
([engine_displays.cpp:856-866](../src/mDropDX12/engine_displays.cpp#L856-L866)):

```cpp
const bool ownPreset = out.config.bOwnPreset || out.config.bOwnProcess;
const bool wantsOwnShape =
    ownPreset || (out.config.bIndependentRender && panelPortrait != primPortrait);
if (wantsOwnShape && layW > 0 && layH > 0) { outW = layW; outH = layH; }
else                                       { outW = primW; outH = primH; }
```

A copy mirror is false on all three terms, so it takes the primary's client
size; `DXGI_SCALING_STRETCH` then squashes that into a window created at the
monitor rect. The code says so deliberately at
[:3600-3603](../src/mDropDX12/engine_displays.cpp#L3600-L3603) — "in copy mode
the chain is deliberately the PRIMARY's shape, so the window and the chain
differ by design". The format is `R8G8B8A8_UNORM` and the count is
`DXC_FRAME_COUNT` = 2.

Against this rig's 2160×3840 primary:

| | per display |
|---|---|
| as built, primary-sized | 2160 × 3840 × 4 B × 2 = **63.3 MB** |
| if it were monitor-sized | 2560 × 1440 × 4 B × 2 = 28.1 MB |
| available saving | **35.2 MB, a factor of 2.25** |

The 63.3 MB is confirmed against the `VMARK` trail to 0.12 MB — the checkpoints
either side of `CreateSwapChainForHwnd` read `+63.4 MB` twice.

**The per-display figure in the table above is lower than 63.3 because the cost
depends on when the mirror is created relative to the primary's fullscreen
transition**, the primary's client size being the input. Measured values ranged
38.7 to 63.4 MB per display across runs that differed only in ordering. Treat
63.3 as the figure for a settled fullscreen primary and the ladder's number as
a lower bound; do not quote a single value without saying what the primary was
doing.

Whether to change it is a real question and not obviously yes: a monitor-sized
chain would need the blit rescaled rather than stretched by DXGI at present,
and on a mirror whose aspect differs from the primary's the stretch is currently
what produces the letterboxing users see. It is recorded here as a measured
opportunity, not a pending fix.

The last row is the same +382 MB the `.milk3` table above decomposes; it moves
the primary as well as the two displays, so do not read its "per display" as
65 MB — that decomposition is in the previous section and this measurement
agrees with it to 0.7 MB.

**The real headline is the row with no displays in it at all.** Going fullscreen
on this rig costs more than every multi-display feature combined, several times
over, and it is nothing to do with multi-display: `ResizeSwapChain: swap=678x1044
-> 2160x3840` is an 11.7× increase in pixel count, and the render chain scales
with it about linearly. That is the expected behaviour of a bigger render
target, not a leak — but it means **resolution dominates this entire document**.
A user worried about VRAM should look at their canvas size long before they look
at how many displays they are driving.

#### The trap that hid this for a day

The first version of this ladder ran all six rungs **in one process**, and
reported `two copy mirrors: +819.6 MB`. That number is real, reproducible, and
attributed to entirely the wrong thing. Two separate faults, either of which is
enough to ruin the measurement:

*Activating mirrors calls `EnsureFullScreen` on the primary*
([App.cpp:2740](../src/mDropDX12/App.cpp#L2740)). So the floor was taken with a
small windowed primary and every later rung with a fullscreen one, and the
primary's own render-chain rebuild was charged to the first rung that happened
to follow it. The fresh-process ladder reproduces `+819.6` exactly with
**`surfs=0` and both displays disabled** — which is what settles it.

*Disabling a display does not release its swap chain.* Turning two copy mirrors
off and back on again measures **+0.0 MB**, because they were never given back.
So no in-process sequence can measure a configuration's cost: every rung carries
every allocation every earlier rung made, and the ladder silently reports
high-water marks rather than costs.

The tell was not in the totals, which looked plausible. It was in the `VMARK`
trail, which showed the swap chains costing 63.4 MB each while **661 MB had
already been spent before `InitDisplayOutput` was even entered**. Totals hide
this; checkpoints do not. Read the trail.

### The expensive case

Clouds1b — the heaviest preset in this library — costs **26–31.8 ms of GPU per
frame** (`gpuUs`), against 75–130 µs for an ordinary preset. That is a spread
of 200–350×, which is why any threshold between them is easy to place and why
reading the shader text never worked (see §3).

Three independent Clouds1b renders, on real monitors, primary fullscreen at
2160x3840:

| surface | size | fps | gpuUs |
|---|---|---|---|
| DISPLAY1 | 1920x1088 | 23.2 | 7,300 |
| DISPLAY3 | 1920x1088 | 24.6 | 8,898 |
| primary | 2160x3840 | 31.1 | — |

**82% GPU, 79 fps total.** That is a usable picture on all three displays, from
the preset that motivated years of work on process isolation.

### Collapsing to copy mirrors

Forcing the distress ladder's mirror rung on the same configuration:

| | before (3 independent) | after (collapsed) |
|---|---|---|
| DISPLAY1 | 15.4 fps, 27,844 µs | **26.1 fps, 7,757 µs** |
| DISPLAY3 | 15.8 fps, 26,829 µs | no longer an independent surface |
| primary | 37.0 fps | 37.0 fps |
| GPU | 92.5% | 92.2% |

**The primary does not move.** What the fallback rescues is the *secondary*
displays — 15 fps is unwatchable, 26 is not, and GPU cost per surface drops
3.6×. Worth knowing before deciding it is worth having: it is not a throughput
optimisation, it is a way to make side screens watchable.

---

## 2. Windows decides the frame rate more than the engine does

Quality of Service is classified **per process, from the window state of the
windows that process owns**. In focus → High; visible but unfocused → Medium;
minimised or occluded → Low. The `EXECUTION_SPEED` opt-out only flips the *API*
source and cannot override the visibility source, so it does not hold a
graphical app at High once something else owns the foreground.

Measured here, same preset:

| | fps | GPU |
|---|---|---|
| foreground, GPU-bound | 43.8 | 92% |
| background, paced | 22.0 | 47.5% |
| background ceiling, light preset | 66.0 | — |

**This is the single most important thing to know before measuring anything.**
A rate taken from an unfocused window is the compositor's number, not the
engine's, and three separate wrong conclusions here came from forgetting it.

It is also the real argument against child processes: only one process can own
the foreground, so a `-child` is throttled to about 66 fps *exactly when* it is
asked to carry an expensive preset.

---

## 3. What was removed, and why

### Every justification for multiple processes turned out to be a defect

This is the most useful thing in the document, and it was not visible from
inside the work — only from the far side of it. Put plainly at the end of it:
most of the issues that seemed to justify multiple processes turned out to be
coding defects.

Each observation that argued for separate processes was real. None of them was
inherent. Every one had a bug underneath:

| What was observed | What it actually was | Fixed by |
|---|---|---|
| Clouds1b + two in-process mirrors: **device dead in 13 s** | mirrors paced on wall time, and the engine mutex held across `Present` | `ed02056d`, `14064459` |
| …and the death was fatal | four HRESULTs folded together, so a **driver update** killed the app exactly as a hang did. A hang itself still exited, by design | `71ebddd7` (#241) |
| **One preset in seven** needs isolation | the trigger read *shader text* for big loop bounds; `Saturday smoke0` outscores Clouds1b statically while costing 2–3 ms against its 26–29 | `d4b7d1f0` (#230) |
| **Every launch comes up with three processes** | an automatic isolation wrote `bOwnProcess` to disk, indistinguishable from the user's choice and never released | `2bfb29e2` (#230) |
| Isolation **flapping** on and off | the gate could not tell "cheap" from "not measured" — no surfaces live reads 0 | `63e696ed` (#245, #242) |
| Mirror chains must be capped at 1920 or the device TDRs | an uncorroborated claim; the layout pass was also sizing chains from the *window rect* | `d694ad81` (#252), `df88ef55` (#254) |
| In process ~16 fps vs children ~31 | the in-process path carried the pacing and locking defects above | the same fixes |

The original TDR's root cause was never isolated. The likeliest reading is that
it was a memory leak since fixed, and nothing contradicts that; what is certain
is that the symptom was removed by repairing the in-process path, not by moving
work into another process.

### One narrow exception, added in v3.2 — and it is not a crack in the above

v3.2 measured something that does send work to another process: after
`DXGI_ERROR_DEVICE_HUNG` the adapter refuses **the process that caused the
hang** for around 120 seconds, while a freshly started process is served a
device in about 22. So TDR recovery is now a relaunch rather than an in-place
rebuild.

Read carefully, because it is easy to over-generalise:

* It is about **getting a device back after a hang**, not about where rendering
  lives. Nothing here recommends putting a display in its own process, and
  everything above about that still holds.
* It is **not** "the adapter is cursed", and the code says so explicitly
  (`App.cpp`, where the in-process path used to be): *"'DEVICE_HUNG is not
  recoverable in process' is NOT established."* The measurement was taken on a
  process that was still holding live device children — `ReleaseGpuResources`
  has exactly one caller, WinMain exit, and prints `20 sampled resource(s) had
  survived every other teardown path` on every run. A per-process, per-adapter
  D3D12 device singleton kept alive by those children fits the signature
  exactly.
* The reason to relaunch anyway is the better one: process exit is the only
  teardown that is **provably complete**, and stays complete as the code grows
  without anyone maintaining a release list. This tree has already shipped three
  places that silently held a device child.

So the row above — *"…and the death was fatal: no `DEVICE_REMOVED` recovery"* —
is still an in-process defect that was fixed in process. `DEVICE_REMOVED` and
`DEVICE_RESET` are recovered in place today. Only `DEVICE_HUNG` relaunches, and
only for want of a teardown nobody has yet proven complete.

**The generalisable lesson:** when a workaround is justified by observed
failures, the failures are evidence that *something* is wrong — not evidence
that the thing being worked around is inherently incapable. The workaround
(separate processes) was load-bearing for months on a foundation of six
separate defects, and each fix quietly removed part of its justification.

**What was missing was not scepticism.** The justification was questioned
repeatedly and throughout: that there was no real performance benefit to own
processes; that the original comment was probably written around a memory leak;
that the GPU memory cost had never actually been recorded; and finally the
instruction to get rid of the own-process path altogether. The doubt was there
the whole time and it was correct.

What overruled it was a body of confident prior rationale — measurements
recorded in code comments from configurations that no longer existed, written
up as settled fact — and, more than once, a recommendation to abandon the
one-process direction as impractical. That recommendation was wrong, and it was
wrong in the direction that preserved the status quo.

So the lesson is not "be more sceptical". It is that **scepticism loses to a
confident-sounding comment unless someone takes a measurement**, and the
measurement that settled this one was trivial: the ordinary case costs 75–130 µs
per surface against a 15,000 µs threshold. Half a percent of the trigger. It
could have been taken at any point in those months.

The general form: a rationale written into a code comment is a claim to be
checked, not authority, however settled it sounds.

A second-order cost worth noting: the workaround itself accumulated bugs that
made it look necessary. A child's fullscreen landing on the wrong display
(#240), watermark mode throwing a child onto the primary (#244), children never
hearing about HUD state (#243) — all of that was maintenance spent on a
mechanism that the measurements say was not needed.

### Automatic process isolation (#261)

A gate promoted extra displays to their own process when the measured GPU cost
crossed `kIsolateGpuUs = 15000`. It is gone. The ordinary case never needed it —
75–130 µs against a 15,000 µs threshold is half a percent of the trigger — and
for the expensive case a child is the wrong instrument, per §2.

An earlier version of the same gate read the *shader text* for large loop
bounds. That fired on 216 of 1,532 presets — one in seven — and ranked wrongly:
`Saturday smoke0` outscores Clouds1b on static loop cost while costing 2–3 ms
against Clouds1b's 26–29 ms. Static cost cannot rank runtime cost.

**The user-chosen `child` tier stays.** What went is the app choosing it
unbidden. See `Scripts.md` for what that tier does and does not buy.

### A canvas-shrink rung (#258)

The distress ladder briefly had a middle rung that clamped the canvas to 1024.
Measured on the one preset heavy enough to reach it, the achievable rate went
from 12.2 to 10.9 — nothing — because it only helps a preset whose cost is
per-pixel. It cost visible resolution for that nothing, and 1024 was arbitrary.
Removed.

**That verdict is about FRAME RATE only, and does not transfer.** The sentence
above is easy to read as "clamping the canvas is useless", and it is not:
It was read that way within hours of being written, which is the measure of
how the wording lands rather than a criticism of the reading.

Clamping the canvas has a second, unrelated use — **visual correctness** for a
preset whose maths is written in TEXELS. A displacement of *n* texels is a large
fraction of a 540 px canvas and a tiny one of a 2160 px canvas, so such a
preset's structure shrinks relative to the screen as the canvas grows and the
image is simply wrong at fullscreen. #188 is the worked example, confirmed by
its own author: *"it only really works at this tiny little resolution and then
it can be stretched to fit."* There, clamping is the whole remedy and the
frame-rate measurement says nothing about it.

Keep the two apart:

| clamping the canvas to... | verdict |
|---|---|
| recover frame rate | measured useless (12.2 → 10.9); the rung is gone |
| make a texel-scaled preset render correctly | the correct remedy; see #188 |

---

## 4. What replaced them

Two governors, sharing one lever (the primary's frame cap) with one owner at a
time. The ladder outranks the balancer.

### The balancer (#261) — relative imbalance

When the main render exceeds **1.25×** the slowest independent display, cap the
main to that ratio and re-measure. If the displays improved by ≥10%, keep it. If
not, release it and rule the preset out — no retry until the preset changes.

It is a **probe, not a rule**, because a display slower than the main is either
*starved* (capping frees GPU it can use) or simply *heavy* (capping is pure
loss), and no single reading separates them. Clouds1b is the second kind:

    [balance] main 39.6 > 1.25x slowest display 30.1 -- capping main at 38 and re-measuring
    [balance] displays did not improve (30.1 -> 19.5); releasing the cap, this preset is simply slow

### The distress ladder (#258) — absolute distress

| rung | trigger | action |
|---|---|---|
| 1 | below 60 (or 90% of a sub-60 target), GPU-starved | main cap to ~1 below the **achievable** rate |
| 2 | below 39 (or an 85% window at/below a 30 target) | cheapest copy mirrors, main uncapped |

Rung 2 **latches** until the preset changes: the rate improved *because* of the
rung, so reading that as permission to undo it collapses the rate again and the
preset flickers between two states forever.

While the app is **not the foreground**, both thresholds clamp to **15 fps** —
low enough to sit under anything pacing produces, high enough to catch real
failure. Blocking outright was tried first and was too absolute: it protected
the alt-tab case but left a genuinely failing background instance stuck.

`SET_LADDER_TRIGGER=<-1|fps>` pins rung 2's threshold; `-1` derives it.

### The mirror swap-chain cap (#254)

Was a fixed 1920, justified by a TDR that nothing corroborated. The A/B came
back clean — uncapped mirrors at native panel size cost +82 MB and lost no
device — so the cap is now derived from the VRAM budget: a tenth of it, split
between the enabled outputs, as a 16:9 long edge. On an ordinary GPU that
reaches the 8192 ceiling, i.e. effectively no cap; a small card still gets one.

**Do not confuse this with the canvas clamp, which is a second and unrelated
ceiling.** This one sizes the mirror **swap chain** from the VRAM budget.
`ClampOrientCanvas` limits the mirror's feedback **canvas**, from a hard base of
1920 — and from v3.2 a GPU hang steps that base down for the rest of the session
(1280 → 960 → mirrors off) so the relaunched instance does not restore the
arrangement that just killed the device. A display that comes back smaller after
a crash is that, not this.

---

## 5. How to measure this without getting it wrong

Every item here cost a wrong conclusion first.

**`gpuUs`, not `workUs`.** `gpuUs` is the GPU timestamp delta — the real cost.
`workUs` is submit-to-fence-complete wall time and includes queueing, so it
**inflates about 4× under pacing**. The same two surfaces read `workUs`
1616–1818 µs unfocused and 435–538 µs focused, while `gpuUs` barely moved
(75–130 µs). Only `gpuUs` is focus-proof.

**Report the frame rate beside any cost figure.** It is what makes an invalid
sample visible. A reading at `simFps=22.1` against a cap of 40 is the tell that
the window was not focused and the numbers are worthless.

**Measure the achievable rate uncapped.** Any governor's cap feeds straight
back into a smoothed estimate derived from it, walking the estimate down a
fraction per step until it hits the floor. The ladder measures its stable rate
only while no ladder cap is in force, for exactly this reason.

**Do not sum `simFps` across a configuration change.** A collapsed display stops
being an independent surface and leaves `DIAG_MIRRORS`, so a before/after total
adds up different things. One run printed `total 68 -> 63 (0.93x)` — an apparent
regression — while the surviving display had gone from 15.4 to 26.1 fps.

**Wait out the load transient.** For several seconds after a preset change the
rate reflects shader compilation and cold frames. A preset that runs at 300+ fps
was once judged at 10 on its own transient.

**One fresh process per VRAM configuration.** GPU allocations are not given back
when the configuration that made them goes away — disabling two copy mirrors and
re-enabling them measures **+0.0 MB**, because the swap chains were never
released. An in-process ladder therefore reports high-water marks, not costs, and
every rung silently carries every rung before it. Restart between configurations.
This is specific to memory: the frame-rate ladders above are fine in one process,
because a frame rate has no history.

**Hold the primary's window state fixed across a VRAM ladder.** Activating
mirrors calls `EnsureFullScreen` on the primary, and on a 2160×3840 portrait
display that single transition costs **+819.6 MB** — more than every
multi-display feature put together. A ladder whose floor is windowed and whose
rungs are fullscreen charges all of it to whichever rung came first. It looked
exactly like "copy mirrors are astonishingly expensive" for a day.

**Read the `VMARK` trail, not the totals.** Both faults above were invisible in
the per-rung totals, which looked entirely plausible, and obvious in the
checkpoints: the mirror swap chains cost 63.4 MB each while 661 MB had already
been spent before `InitDisplayOutput` was entered. `DIAG_VRAMMARK=1` turns the
trail on.

**Check the compile log before believing a `.milk3` memory figure.** Under #344
the FLOAT32 feedback buffers are allocated on demand, so a preset whose shaders
fail to compile falls back to the default shader and allocates nothing — and the
measurement comes back low rather than failing. The first `.milk3` ladder ran
against the shader bug in #380 and had failures in its log. Count
`RecompilePShader FAILED` over the run and say it is zero.

**Synthetic heavy presets have a compile cost.** Scaling Clouds1b's raymarch
loop is linear and well-behaved in *render* cost — `outer × fps ≈ 33,000` across
a 65× range — but the heaviest fixtures take minutes to compile their shaders
and block the preset loader while they do. Use the smallest fixture that reaches
the behaviour under test.

---

## 6. What is still unknown

Stated plainly so nobody assumes otherwise:

- **After the mirror rung fires, DISPLAY3 has no surface entry at all** while
  DISPLAY1 still has one at reduced cost. That may be correct — a pure copy has
  no simulation to report — or that display may not be presenting. Not verified.
- **Nobody has watched a collapse happen.** How the transition looks in motion,
  and whether losing per-display timing is noticeable, is untested.
- **Rung 2's trigger is placed from one machine's numbers.** 39 was chosen
  because three independent Clouds1b renders reach ~35–37 achievable here. On
  different hardware that relationship will differ, which is why the threshold
  is settable.
