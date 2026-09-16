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

Corroborated three ways: Shane's own toggle of one display's own preset moves
VRAM by **~60 MB**; the code's long-standing figure in
`engine_displays.cpp` is **~45 MB per classic surface** at 1920x1088; and the
delta above works out at ~50 MB per extra simulation. Three independent
estimates, one answer.

A `.milk3` (Shadertoy) surface is the exception at roughly **270 MB**, because
of its FLOAT32 ping-pong feedback buffers. Three independent `.milk3` presets
is ~800 MB rather than ~150 MB. Still not alarming on 15 GB, but it is the one
configuration where per-display presets cost something measurable.

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
engine's, and three separate mistakes in this session came from forgetting it.

It is also the real argument against child processes: only one process can own
the foreground, so a `-child` is throttled to about 66 fps *exactly when* it is
asked to carry an expensive preset.

---

## 3. What was removed, and why

### Every justification for multiple processes turned out to be a defect

This is the most useful thing in the document, and it was not visible from
inside the work — only from the far side of it. Shane, at the end:
*"it seems most of the issues that seemed to justify multiple processes were
because of coding issues."*

Each observation that argued for separate processes was real. None of them was
inherent. Every one had a bug underneath:

| What was observed | What it actually was | Fixed by |
|---|---|---|
| Clouds1b + two in-process mirrors: **device dead in 13 s** | mirrors paced on wall time, and the engine mutex held across `Present` | `ed02056d`, `14064459` |
| …and the death was fatal | no `DEVICE_REMOVED` recovery — a TDR exited the app | `71ebddd7` (#241) |
| **One preset in seven** needs isolation | the trigger read *shader text* for big loop bounds; `Saturday smoke0` outscores Clouds1b statically while costing 2–3 ms against its 26–29 | `d4b7d1f0` (#230) |
| **Every launch comes up with three processes** | an automatic isolation wrote `bOwnProcess` to disk, indistinguishable from the user's choice and never released | `2bfb29e2` (#230) |
| Isolation **flapping** on and off | the gate could not tell "cheap" from "not measured" — no surfaces live reads 0 | `63e696ed` (#245, #242) |
| Mirror chains must be capped at 1920 or the device TDRs | an uncorroborated claim; the layout pass was also sizing chains from the *window rect* | `d694ad81` (#252), `df88ef55` (#254) |
| In process ~16 fps vs children ~31 | the in-process path carried the pacing and locking defects above | the same fixes |

The original TDR's root cause was never isolated. Shane's reading is that it was
a memory leak since fixed, and nothing contradicts that; what is certain is that
the symptom was removed by repairing the in-process path, not by moving work
into another process.

**The generalisable lesson:** when a workaround is justified by observed
failures, the failures are evidence that *something* is wrong — not evidence
that the thing being worked around is inherently incapable. The workaround
(separate processes) was load-bearing for months on a foundation of six
separate defects, and each fix quietly removed part of its justification.

**What was missing was not scepticism.** Shane questioned the justification
repeatedly and throughout — "there is no real performance benefit for own
processes I think though", "the comment was probably because there was a memory
leak", "I don't think you ever recorded how much GPU memory it takes", and
finally "get rid of the own process". The doubt was there the whole time and it
was correct.

What overruled it was a body of confident prior rationale — measurements
recorded in code comments from configurations that no longer existed, written
up as settled fact — and, more than once, advice from this assistant to abandon
the one-process direction as impractical. That advice was wrong, and it was
wrong in the direction that preserved the status quo.

So the lesson is not "be more sceptical". It is that **scepticism loses to a
confident-sounding comment unless someone takes a measurement**, and the
measurement that settled this one was trivial: the ordinary case costs 75–130 µs
per surface against a 15,000 µs threshold. Half a percent of the trigger. It
could have been taken at any point in those months.

See also `prior-comments-are-claims-not-authority`: a rationale written into a
comment by an earlier session is a claim to be checked, not authority.

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
Shane read it that way within hours of it being written, which is the measure of
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
