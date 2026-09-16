// engine_displays.cpp — Display output management (monitor mirrors + Spout senders)
//
// Part of the MDropDX12 unified display output system.
// Manages enumeration, INI persistence, init/destroy, and per-frame send.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "json_utils.h"
#include "profile_paths.h"
#include "kv_store.h"
#include "utility.h"
#include "format_to.h"
#include <algorithm>
#include <dxgi1_4.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM for mirror mouse input
#include <process.h>
#include <shellapi.h>  // SHFileOperationW -- a deleted profile goes to the bin
#include <mutex>
#include <chrono>
#include "config_store.h"
#include "pipe_server.h"

namespace mdrop {

// Used by InitDisplayOutput and ApplyMirrorWindowStyles (defined later)
static void ComputeMirrorLayout(const DisplayOutputConfig& cfg,
                                int& outX, int& outY, int& outW, int& outH);

// Aspect-preserving long-edge cap, 16-aligned. Shared by the mirror swap-chain
// cap and the feedback-canvas limit: they want identical geometry rules, and
// having two copies would let them drift.
void CapDimToLongEdge(int& w, int& h, int maxDim)
{
    if (maxDim > 0 && (w > maxDim || h > maxDim)) {
        float sc = (float)maxDim / (float)((w > h) ? w : h);
        w = max(1, (int)(w * sc + 0.5f));
        h = max(1, (int)(h * sc + 0.5f));
    }
    w = ((w + 15) / 16) * 16;
    h = ((h + 15) / 16) * 16;
}

// The mirror swap-chain long-edge cap.
//
// The original comment here read: "Never allocate a mirror SC larger than 1920
// on a side. 2160x3840 primary plus matching mirror SCs TDRs (idle-timer FS at
// 12:22)." That is NOT established. Shane: "the comment was probably because
// there was a memory leak" -- the device loss being VRAM exhaustion from a leak
// since fixed rather than anything inherent to large chains. The commit that
// introduced the cap (796fba30) is a large mixed change whose message never
// mentions it, so nothing corroborates the stated reason, and an apparent SRV
// leak elsewhere in this work turned out to be heap CAPACITY rather than a leak.
//
// So the value became adjustable at runtime (DIAG_MIRRORCAP), for the same
// reason DIAG_MIRRORPACE can be switched off: the only way to find out whether
// a guard is still earning its place is to be able to remove it and look. The
// comparison was run -- uncapped mirrors at the native panel size cost +82 MB
// and produced no device loss -- so the fixed 1920 is gone and the default is
// now DERIVED from the VRAM budget (#254). 0 still means no cap.
//
// This variable holds the APPLIED value, not the setting. It starts at 1920
// because that is the right guess for the one moment the budget is not known
// yet: the first frame, before the VRAM poll has run.
//
// Read through an atomic because the render thread sizes chains while the IPC
// thread may be changing it.
static std::atomic<int> g_mirrorScCap{1920};

// The share of the VRAM budget mirror swap chains may occupy (#254).
//
// Generous on purpose, because they are not the consumer that matters: two
// 2560x1440 chains are 88 MB at MIRROR_BUFFER_COUNT=3 (measured +82 MB), which
// is 0.6% of a 15 GB budget. Presets and textures dominate, and
// MinAvailableVramPercent is the real low-memory backstop. This exists so the
// cap is derived from what the machine HAS rather than from a fixed 1920 that
// was chosen on one rig for a reason nothing corroborates.
static const double kMirrorVramShare = 0.10;

void SetMirrorSwapChainCap(int maxDim)
{
    g_mirrorScCap.store(maxDim < 0 ? 0 : maxDim, std::memory_order_relaxed);
}

// Recompute the effective cap from the setting and the VRAM budget (#254).
//
// Three states, the same -1/0/n shape the per-display settings use:
//
//     -1   derive it from the budget (the default)
//      0   no cap -- the native panel size, whatever it is
//     >0   that long edge, for a machine under real contention
//
// A diagnostic override from DIAG_MIRRORCAP wins over all of them and is not
// persisted, so an evaluation cannot be mistaken for a decision.
// What the VRAM budget alone implies, ignoring the setting and any override.
//
// Deliberately answers the machine's question and not the effective one, so the
// diagnostic can show a user what their hardware would have chosen while their
// own setting is in force. RefreshMirrorSwapChainCap is what decides between
// them.
int Engine::DeriveMirrorSwapChainCap() const
{
    const UINT64 budget = m_vramBudgetBytes;
    if (budget == 0) {
        // No budget reported yet (the poll has not run, or the query failed).
        // Fall back to the historical 1920 rather than to "unlimited": an
        // unknown budget is the one case where the old conservative number is
        // the better guess.
        return 1920;
    }

    int mirrors = 0;
    for (const auto& out : m_displayOutputs)
        if (out.config.type == DisplayOutputType::Monitor && out.config.bEnabled)
            mirrors++;
    if (mirrors < 1) mirrors = 1;

    // Bytes a chain costs: RGBA8 times the buffer count. Split the allowance
    // between the chains that will exist, so adding displays tightens it
    // rather than each display claiming the whole share.
    const double perChain = (double)budget * kMirrorVramShare / (double)mirrors;
    const double maxPixels = perChain / (double)(4 * MIRROR_BUFFER_COUNT);
    if (maxPixels <= 0.0)
        return 1920;

    // Pixels -> a LONG EDGE, which is what CapDimToLongEdge wants. Assumes
    // 16:9, the widest common panel, so a squarer one gets a little headroom
    // rather than being clipped.
    int longEdge = (int)sqrt(maxPixels * 16.0 / 9.0);
    // Round DOWN to a multiple of 256 so an ordinary drift in the reported
    // budget -- other applications allocating and freeing -- does not move the
    // answer. The cap is a coarse safety limit, not a measurement, and a cap
    // that jitters would rebuild every swap chain each time it did.
    longEdge = (longEdge / 256) * 256;
    // Floors and ceilings so a nonsense budget cannot produce a nonsense cap.
    // 8192 is past any panel this will meet, so on any ordinary GPU the derived
    // cap is effectively "no cap" -- which is what the measurement said it
    // should be.
    if (longEdge < 640)  longEdge = 640;
    if (longEdge > 8192) longEdge = 8192;
    return longEdge;
}

void Engine::RefreshMirrorSwapChainCap()
{
    if (m_bMirrorScCapOverridden)
        return;                       // DIAG_MIRRORCAP is driving

    // Precedence: diag override (above) > ladder > setting > derived.
    //
    // The ladder outranks the user's setting because it only speaks at rung 3,
    // where the alternative is a display nobody can watch; the diag override
    // still outranks everything, because an experiment must be able to pin the
    // value whatever else is going on.
    const int want = (m_nLadderMirrorCap > 0)     ? m_nLadderMirrorCap
                   : (m_nMirrorScCapSetting >= 0) ? m_nMirrorScCapSetting
                                                  : DeriveMirrorSwapChainCap();
    if (want == GetMirrorSwapChainCap())
        return;

    SetMirrorSwapChainCap(want);
    // The chains are sized from this, so a change has to reach them. Only on an
    // actual change: this is called from the VRAM poll, and rebuilding every
    // mirror swap chain twice a second would be its own defect.
    m_bMirrorStylesDirty.store(true);
    m_bMirrorIndepSizeDirty.store(true);
    DLOGW_INFO(L"[mirror] swap-chain cap -> %d (%ls)", want,
               m_nMirrorScCapSetting >= 0 ? L"setting" : L"derived from VRAM budget");
}

int GetMirrorSwapChainCap()
{
    return g_mirrorScCap.load(std::memory_order_relaxed);
}

double MirrorVramShare()
{
    return kMirrorVramShare;
}

// ---------------------------------------------------------------------------
// Adaptive frame-rate balancing (#261)
//
// Replaces automatic process isolation. Shane, 2026-09-14: "get rid of the own
// process and enable adaptive frame rate balancing if main render >1.25 times a
// child", then "cap main and give freed gpu to the displays if the fps doesn't
// improve, unlock main again", and the sentence that shapes the whole design:
// "you can remeasure and increase but some renders might just have slow frame
// rates".
//
// That is why this is a PROBE and not a rule. A display running below the main
// is slow for one of two reasons, and they look identical from outside:
//
//     STARVED   the main is taking GPU it could have used   -> capping helps
//     HEAVY     its own preset simply costs that much       -> capping is loss
//
// No single reading separates them, so the balancer finds out by trying: cap
// the main, wait, measure again. If the displays picked up, the cap earned its
// place and is kept. If they did not, the main was never the problem, the cap
// is released, and this preset is ruled out so the probe does not repeat --
// a probe that keeps re-running is just a visible stutter.
//
// Two-way while held: the ceiling is recomputed from the CURRENT slowest
// display, so the main rises as the displays do. The verdict belongs to the
// preset and is discarded on a preset change (ResetFrameRateBalancer).
//
// Displays are never paced against EACH OTHER -- they keep their own clocks,
// and only the main render is ever adjusted.

// Above this ratio the main is taking more than its share. Shane's number.
static const float kBalanceRatio = 1.25f;
// A probe has to move the slowest display by this much to count as helping.
// Below it the change is indistinguishable from ordinary frame-rate wobble.
static const float kBalanceGainNeeded = 1.10f;
static const DWORD kBalanceProbeMs = 4000;   // let the cap take effect
static const DWORD kBalanceHoldMs = 2000;    // re-evaluate cadence
static const int   kBalanceMinCap = 10;      // never pace the main below this

static int BalanceCapFor(float slowest)
{
    int cap = (int)(slowest * kBalanceRatio + 0.5f);
    if (cap < kBalanceMinCap) cap = kBalanceMinCap;
    if (cap > 720) cap = 720;
    return cap;
}

// The cap the balancer imposes is NOT the user's setting and must never be
// written as one. SetFPSCap persists to settings.ini and moves the Settings
// window's combo; this only changes the live limit, the same separation #254
// established between a diagnostic override and a stored value.
void Engine::ApplyBalanceCap(int fps)
{
    m_max_fps_fs = fps;
    m_max_fps_dm = fps;
    m_max_fps_w = fps;
    ResyncFrameTiming(false);
}

void Engine::ResetFrameRateBalancer()
{
    if (m_balanceUserCap >= 0) {
        ApplyBalanceCap(m_balanceUserCap);   // hand the user's rate back
        m_balanceUserCap = -1;
    }
    m_balanceState = BalanceState::Idle;
    m_balanceAppliedCap = 0;
    m_balanceBaseline = 0.0f;
    m_balanceNextMs = 0;
}

// Slowest INDEPENDENT in-process display surface, and how many there are.
//
// Children are excluded deliberately: they render in another process with
// their own device and queue, so capping this process's main render frees them
// nothing. Balancing against one would throttle the main for no benefit -- and
// would never resolve, because the probe could not detect an improvement it
// cannot cause.
float Engine::SlowestDisplayFps(int* countOut) const
{
    float slowest = 0.0f;
    int n = 0;
    for (auto& sp : SnapshotMirrorSurfaces()) {
        if (!sp->imageReady.load()) continue;
        const float f = sp->sim.fFps;
        if (f <= 0.0f) continue;
        n++;
        if (slowest == 0.0f || f < slowest) slowest = f;
    }
    if (countOut) *countOut = n;
    return slowest;
}

// ---------------------------------------------------------------------------
// The distress ladder (#258)
//
// Two rungs, cheapest first, the second engaging only when the first has not
// recovered the rate:
//
//     below 60 fps, GPU-starved   adapt the main cap to ~1 below the stable rate
//     below 20 fps                switch to the cheapest copy mirrors that fit
//
// It was three. A middle rung clamped the canvas, and Shane removed it: "I
// think removing rung 2 and only having rungs 1 and 3 makes more sense." The
// measurement agreed -- on the one preset heavy enough to reach it the clamp
// took the achievable rate from 12.2 to 10.9, which is nothing, because it only
// helps a preset whose cost is per-pixel. It also cost visible resolution for
// that nothing, and the edge it clamped to was a number picked out of the air.
// Collapsing N simulations to one does far more and gives up something the user
// can choose to give up.
//
// The mirror rung therefore takes the below-20 trigger. That was already its
// trigger whenever the canvas clamp had not helped, and with the clamp gone
// that condition is simply always true.
//
// GPU-STARVED is the gate on all of it, and it is what keeps this from firing
// on rates that are nobody's fault. Measured this session: a backgrounded
// window sat at 22 fps with the GPU at 47.5%, while the same preset GPU-bound
// ran 43.8 fps at 92%. So a low rate with the GPU idle is Windows pacing an
// unfocused window -- correct behaviour we do not code around (#121) -- and a
// low rate with the GPU saturated is the thing this ladder is for. The frame's
// own timestamp separates them without needing to know about focus at all.
//
// Everything unwinds on a preset change. Shane: "it will go away and try again
// when preset changes."
static const float kRung1Fps = 60.0f;   // adapt the rate
// The mirror rung's trigger when the target is high or unset.
//
// 39 is chosen from the measurement rather than picked. Three independent
// Clouds1b renders on a 3-display rig reach about 35-37 fps achievable, and at
// the previous 34 the rung never fired -- so the case that motivated the whole
// mechanism sat one frame the wrong side of its own trigger. Forcing the rung
// at 40 showed what it was worth: the side displays went from 15.4 and 15.8 fps
// to 26.1, GPU cost per surface down 3.6x, primary unchanged at 37.0. So the
// default now catches that case.
static const float kRung2FpsHigh = 39.0f;
// Above this target the trigger is kRung2FpsHigh; at or below it, a window of
// kRung2TargetWindow around the target -- someone who asked for 30 and is
// getting 30 is not in distress, and must not have their displays collapsed.
static const float kRung2TargetKnee = 30.0f;
static const float kRung2TargetWindow = 0.85f;
// The whole ladder's ceiling while this app is NOT the foreground.
//
// Windows paces an unfocused window, so a rate in the 20s-60s there is the
// compositor's doing and says nothing about the preset -- acting on it would
// collapse the displays every time the user alt-tabbed away. But nothing paces
// a window down to 11 fps; below this it is genuinely failing, and declining to
// act merely because it is not in front leaves it failing.
static const float kUnfocusedGateFps = 15.0f;
// The floor the adaptive cap will not go below. A cap under this is not pacing
// any more, it is a slideshow.
static const float kCapFloorFps = 10.0f;
// A frame is GPU-bound when the GPU was busy for most of it. Below this the
// limiter is somewhere else and none of the three rungs can help.
static const float kStarvedFraction = 0.80f;
// Consecutive GPU-bound ticks required before the ladder will climb. Three at
// kLadderStepMs is about nine seconds of sustained starvation -- long enough
// that the flicker measured at 82% GPU cannot carry it, short enough that a
// genuinely stuck preset is not left alone for a minute.
static const int   kStarvedRunNeeded = 3;
static const DWORD kLadderStepMs = 3000;
// How long a newly-engaged rung is given to work before the next is considered.
// Shane: "the gpu starved should try to use the adaptive frame instead" -- the
// adaptive rate is the remedy, and the rungs below it are fallbacks for when it
// is not enough. Stepping on the fps threshold alone took a preset at 8 fps
// through all three rungs in nine seconds, shrinking the canvas and collapsing
// to mirrors without ever learning whether the cap alone had fixed it.
//
// So: one rung at a time, from the bottom, and each gets a real dwell before
// the next is paid for. Longer than kLadderStepMs because a cap change has to
// propagate through the frame timing and settle before GetFps() reflects it.
static const DWORD kLadderSettleMs = 8000;

// The rate the USER asked for: their own cap, or 0 for uncapped. Read past
// anything the ladder or balancer is holding, since those are our values and
// not a statement of intent.
int Engine::UserFpsTarget() const
{
    return (m_balanceUserCap >= 0) ? m_balanceUserCap : m_max_fps_w;
}

// Where the mirror rung fires, relative to what was asked for.
//
// Shane: "how about cheap mirror kicks in at <34 if max fps >35 and 20 if max
// fps was 30." A fixed 20 is wrong in both directions. Someone running at 60
// and getting 30 has lost half their motion and is in distress well before 20;
// someone who deliberately capped at 30 and is getting 30 is not in distress at
// all, and a fixed 34 would collapse their displays for no reason.
//
// So the trigger tracks the target. Above the knee -- or uncapped, where the
// intent is "as fast as possible" -- it is 34. At or below it, two thirds of
// the target, which is his 30 -> 20.
//
// The step at the knee is deliberate and comes from his two numbers rather than
// from a curve fitted to them:
//
//     uncapped, 120, 60, 40, 36 -> 34
//     30 -> 20      24 -> 16      15 -> 10 (the floor)
// What the target-relative rule alone would choose, ignoring any setting.
// Reported by the diagnostic so a user can see what they are overriding.
float Engine::DerivedMirrorRungTriggerFps() const
{
    // Shane: "default it to 39 if the max frame rate >30, if max frame rate is
    // 30 than use an 85% window."
    //
    // Above the knee -- or uncapped, where the intent is "as fast as possible"
    // -- the trigger is a flat 39, set from the measurement above. At or below
    // it the trigger tracks the target instead, because someone who asked for
    // 30 and is getting 30 has what they wanted and collapsing their displays
    // would be a bug, not a rescue.
    //
    // Note the two rungs' triggers can invert between 31 and 43 -- a target of
    // 40 gives rung 1 at 36 and rung 2 at 39 -- which is harmless because the
    // climb is strictly one rung at a time, so rung 1 is always tried and given
    // its dwell before rung 2 is considered. The inversion only changes which
    // rung the FIRST evaluation names, not the order they are paid for.
    const int target = UserFpsTarget();
    if (target <= 0 || (float)target > kRung2TargetKnee)
        return kRung2FpsHigh;
    float t = (float)target * kRung2TargetWindow;
    if (t < kCapFloorFps) t = kCapFloorFps;
    return t;
}

float Engine::MirrorRungTriggerFps() const
{
    // An explicit setting outranks the rule (#267). Shane: "can you add an ipc
    // command to set the limit at which it triggers, it might be something that
    // should be user settable."
    //
    // It should be. The derived 34 is right for a machine where a heavy preset
    // across three displays lands well under it, and wrong for one where it
    // lands just above: measured here, three independent Clouds1b renders reach
    // ~35 achievable -- one frame above the trigger -- so the preset that
    // motivated this whole mechanism does NOT collapse. Whether that is correct
    // depends on whether 23/25/31 fps across three displays is acceptable, and
    // that is a judgement about the person watching, not about the hardware.
    if (m_nLadderTriggerSetting > 0)
        return (float)m_nLadderTriggerSetting;
    return DerivedMirrorRungTriggerFps();
}

bool Engine::GpuStarved()
{
    if (!m_lpDX) return false;
    const unsigned gpuUs = m_lpDX->LastFrameGpuUs();
    if (gpuUs == 0) return false;              // nothing measured yet
    const float fps = GetFps();
    if (fps <= 0.0f) return false;
    const float budgetUs = 1000000.0f / fps;
    return (float)gpuUs >= budgetUs * kStarvedFraction;
}

void Engine::ResetDistressLadder()
{
    if (m_ladderRung == 0) return;
    if (m_ladderTookMirrorOnly) {
        // Put back exactly what was taken, per display -- not "all mirrors".
        std::lock_guard<std::recursive_mutex> lk(m_displayOutputsMutex);
        for (auto& o : m_displayOutputs) {
            if (o.config.type != DisplayOutputType::Monitor) continue;
            auto it = m_ladderSavedTiers.find(o.config.szDeviceName);
            if (it == m_ladderSavedTiers.end()) continue;
            o.config.bOwnPreset = it->second.ownPreset;
            o.config.bIndependentRender = it->second.independent;
        }
        m_ladderSavedTiers.clear();
        m_ladderTookMirrorOnly = false;
        m_bMirrorIndepSizeDirty.store(true);
    }
    if (m_nLadderMirrorCap) {
        m_nLadderMirrorCap = 0;
        RefreshMirrorSwapChainCap();
        m_bMirrorStylesDirty.store(true);
        m_bMirrorIndepSizeDirty.store(true);
    }
    // Hand the FRAME cap back too.
    //
    // Rung 1 takes the user's cap and replaces it, and this used to restore the
    // mirrors and the mirror cap while quietly leaving that one in force -- so
    // a preset change released everything the ladder had done EXCEPT the
    // throttle, which then outlived the preset it was diagnosed for. It also
    // left m_balanceUserCap set, which is the balancer's "I am holding
    // something on the user's behalf" flag, so the two governors disagreed
    // about who owned the cap. Measured as a user cap of 60 coming back as 0.
    if (m_balanceUserCap >= 0) {
        ApplyBalanceCap(m_balanceUserCap);
        m_balanceUserCap = -1;
    }
    m_balanceAppliedCap = 0;

    m_ladderRung = 0;
    m_ladderStableFps = 0.0f;
    m_ladderStarvedRun = 0;
    // Do NOT evaluate immediately. A preset change is the usual caller, and for
    // several seconds afterwards the rate reflects shader compilation and the
    // first cold frames rather than what the preset costs. Measured: a preset
    // that runs at 300+ fps was put on rung 1 with a cap of 10, judged entirely
    // on its own load transient.
    m_ladderNextMs = GetTickCount() + kLadderSettleMs;
    DLOGW_INFO(L"[ladder] released -- back to rung 0");
}

// Is the render window the one the user is actually looking at?
//
// The ladder must not act on a rate that Windows imposed rather than the GPU.
// A window that is not in the foreground is PACED -- see #121 -- and that is
// correct behaviour we neither code around nor react to.
//
// GpuStarved() alone cannot tell the two apart, which is the hole Shane found:
// "it will trigger when I put clouds in background while doing something else."
// An expensive preset in the background is genuinely GPU-busy AND rate-limited,
// so both halves of the starvation test pass, the rung fires, and because it
// LATCHES he comes back to collapsed displays he never asked for. Focus is the
// missing term: paced-and-busy looks identical to starved-and-busy except that
// one of them is not the foreground.
bool Engine::RenderWindowHasFocus()
{
    const HWND hRender = GetPluginWindow();
    if (!hRender) return false;
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == hRender) return true;
    // A tool window of ours counts: the user is still working in this app, and
    // opening the Visual window should not freeze the ladder.
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void Engine::TickDistressLadder()
{
    if (m_bChildMode) return;
    if (!m_bDistressLadderEnabled) return;

    const DWORD now = GetTickCount();
    if (m_ladderNextMs && now < m_ladderNextMs) return;
    m_ladderNextMs = now + kLadderStepMs;

    const float fps = GetFps();
    if (fps <= 0.0f) return;

    // Starvation has to be SUSTAINED, not sampled.
    //
    // GpuStarved() compares one frame's GPU time against one frame's budget,
    // and near the threshold that flickers. Measured on three independent
    // Clouds1b simulations at 82% GPU: starved read 1,0,0,1,1,0,1,0 across
    // consecutive ticks. Climbing on a single reading would make the decision
    // to collapse every display to a copy depend on which tick happened to
    // sample -- a coin flip, not a judgement.
    //
    // A run of them is required instead, and a single quiet tick does not
    // reset the run; only a clear stretch does. The threshold stays where it
    // is, since the reading is right on average and it is the sampling that
    // was wrong.
    if (GpuStarved()) {
        if (m_ladderStarvedRun < 8) m_ladderStarvedRun++;
    } else if (m_ladderStarvedRun > 0) {
        m_ladderStarvedRun--;
    }
    if (m_ladderStarvedRun < kStarvedRunNeeded) return;

    // Re-measure after a rung changed the configuration. This is the ONLY
    // place the achievable rate is taken, and it runs with no ladder cap in
    // force, which is what keeps the cap from feeding back into the estimate.
    // Rung 1 only: rung 2 is terminal and runs uncapped by design.
    if (m_ladderRung == 1 && m_ladderStableFps <= 0.0f) {
        m_ladderStableFps = fps;
        int cap = (int)(fps - 1.0f);
        if (cap < (int)kCapFloorFps) cap = (int)kCapFloorFps;
        if (m_balanceUserCap < 0) m_balanceUserCap = m_max_fps_w;
        ApplyBalanceCap(cap);
        m_balanceAppliedCap = cap;
        DLOGW_INFO(L"[ladder] re-measured at rung %d: %.1f achievable -- "
                   L"main cap %d", m_ladderRung, fps, cap);
        m_ladderNextMs = now + kLadderSettleMs;
        return;
    }

    // Judge on the ACHIEVABLE rate, never on the capped one.
    //
    // Two failures sit either side of this, and both were measured. Judging on
    // raw fps once rung 1 holds a cap makes the ladder cascade on a rate it
    // imposed on itself: rung 1 capped a heavy preset at 10, fps duly sat at
    // 10, and the next rung fired within seconds on that number. Judging
    // instead on "is it meeting its cap" fixes the cascade and breaks the
    // opposite way -- a preset stuck at 11 fps meets its cap of 11 perfectly
    // and never escalates, when collapsing to mirrors is exactly what it needs.
    //
    // m_ladderStableFps is the answer to both: what the renderer can sustain
    // UNCAPPED. It is measured only while no ladder cap is in force, so the cap
    // can never feed back into it and walk it down.
    const float judgeFps =
        (m_ladderRung >= 1 && m_ladderStableFps > 0.0f) ? m_ladderStableFps : fps;

    // Both triggers are relative to what was asked for.
    //
    // Rung 1's 60 is the same mistake as rung 2's old fixed 20, one rung up: a
    // user who capped at 30 and is getting 30 has exactly what they asked for,
    // and judging them against 60 puts them on rung 1 permanently, capped at 29
    // for no reason. Against their own target there is nothing wrong.
    const int target = UserFpsTarget();
    float rung1At = (target > 0 && (float)target < kRung1Fps)
                        ? (float)target * 0.9f : kRung1Fps;
    float rung2At = MirrorRungTriggerFps();

    // UNFOCUSED: drop the whole ladder to a 15 fps gate rather than blocking
    // it. Shane: "drop the gate to 15 if unfocused."
    //
    // Blocking outright was the previous answer and it was too absolute. An
    // unfocused window IS paced by Windows, so a rate in the 20s-60s there says
    // nothing about the preset and must not collapse anything -- that is the
    // case that would have caught him alt-tabbing away from Clouds. But a
    // background instance at 11 fps is not being paced to 11; nothing in the
    // compositor asks for that. It is genuinely failing, and refusing to act
    // because the window happens not to be in front leaves it failing.
    //
    // 15 is low enough to sit well under anything pacing produces and high
    // enough to catch real collapse. A user setting below it still wins, so a
    // deliberately conservative choice is never overridden upward.
    if (!RenderWindowHasFocus()) {
        if (rung1At > kUnfocusedGateFps) rung1At = kUnfocusedGateFps;
        if (rung2At > kUnfocusedGateFps) rung2At = kUnfocusedGateFps;
    }

    int want = 0;
    if (judgeFps < rung2At)      want = 2;
    else if (judgeFps < rung1At) want = 1;

    // RUNGS 2 AND 3 LATCH for the life of the preset. Shane: "once on mirror or
    // canvas is lowered don't go back up just because fps increases."
    //
    // That is the whole stability argument. The rate improved BECAUSE of the
    // rung, so reading the improvement as permission to undo it sends the rate
    // straight back down -- shrink, recover, restore, collapse, shrink -- and
    // the visible result is a preset that flickers between two resolutions
    // forever. A recovered rate is evidence the rung is working, not evidence
    // it is unnecessary.
    //
    // Rung 1 is exempt and stays adaptive: its cap is derived from the
    // measured achievable rate rather than from a threshold crossing, so
    // tracking it upward is the feature rather than a relapse.
    if (want <= m_ladderRung) {
        // Nothing to do. The cap is NOT re-derived from the capped rate here:
        // fps is then approximately the cap, so smoothing it into the stable
        // estimate drags that estimate down a fraction each step and the cap
        // ratchets to the floor over a few minutes. The stable rate is
        // re-measured only where it can be measured honestly -- below, after a
        // rung has changed the configuration.
        return;
    }

    // Climbing. One rung per step, so each is given a chance to work before the
    // next is paid for.
    const int next = m_ladderRung + 1;
    switch (next) {
    case 1: {
        // Rung 1 IS the adaptive frame rate. Shane: "the gpu starved should try
        // to use the adaptive frame instead", then "so the first rule was fixed
        // by adaptive frame" -- it is not a second mechanism beside the
        // balancer, it is the same lever with an absolute trigger instead of a
        // relative one, and it goes through the same ApplyBalanceCap and the
        // same m_balanceUserCap so only one owner ever holds the cap.
        //
        // ~1 fps below the STABLE achievable rate, not below the current one.
        // Chasing the current rate means every reduction lowers the next
        // target and the cap walks itself to the floor.
        if (m_ladderStableFps <= 0.0f) m_ladderStableFps = fps;
        else m_ladderStableFps = m_ladderStableFps * 0.7f + fps * 0.3f;
        int cap = (int)(m_ladderStableFps - 1.0f);
        if (cap < (int)kCapFloorFps) cap = (int)kCapFloorFps;
        if (m_balanceUserCap < 0) m_balanceUserCap = m_max_fps_w;
        ApplyBalanceCap(cap);
        m_balanceAppliedCap = cap;
        DLOGW_INFO(L"[ladder] rung 1: %.1f fps and GPU-bound -- main cap %d "
                   L"(~1 below the stable %.1f)", fps, cap, m_ladderStableFps);
        break;
    }
    case 2: {
        std::lock_guard<std::recursive_mutex> lk(m_displayOutputsMutex);
        int changed = 0;
        for (auto& o : m_displayOutputs) {
            if (o.config.type != DisplayOutputType::Monitor) continue;
            if (!o.config.bEnabled) continue;
            if (o.config.bOwnProcess) continue;   // the user's child, not ours
            if (!o.config.bOwnPreset && !o.config.bIndependentRender) continue;
            LadderTier saved;
            saved.ownPreset = o.config.bOwnPreset;
            saved.independent = o.config.bIndependentRender;
            m_ladderSavedTiers[o.config.szDeviceName] = saved;
            // All the way to COPY mirrors. An independent mirror still runs a
            // full simulation of the same preset, so stopping at the mirror
            // tier would keep the preset variety and almost none of the cost.
            // Per-display timing is what this gives up.
            o.config.bOwnPreset = false;
            o.config.bIndependentRender = false;
            changed++;
        }
        m_ladderTookMirrorOnly = (changed > 0);
        // "Cheapest copy to fit" -- Shane. Copy mode alone is only half of it:
        // a copy into a chain sized from the derived cap pays native-resolution
        // presentation cost for an image whose source is the primary's back
        // buffer. Size the chains to that source instead and let the display
        // scale; 1:1 with the source is as small as a copy can be without
        // losing anything the source had.
        {
            const int srcEdge = m_lpDX
                ? (int)max(m_lpDX->m_client_width, m_lpDX->m_client_height) : 0;
            m_nLadderMirrorCap = srcEdge > 0 ? srcEdge : 1024;
        }
        RefreshMirrorSwapChainCap();
        m_bMirrorStylesDirty.store(true);
        m_bMirrorIndepSizeDirty.store(true);

        // UNCAP the main. Shane: "when switching to cheapest copy let fps be
        // uncapped again as won't switch again until preset change."
        //
        // Rung 1's cap existed to stabilise a rate that was collapsing; this
        // rung has just removed the reason it was collapsing, by taking N
        // simulations down to one. Holding the cap now throttles the main for
        // nothing. And because this rung is TERMINAL -- latched until the
        // preset changes -- there is no further escalation the cap could be
        // protecting a measurement for.
        if (m_balanceAppliedCap > 0 || m_balanceUserCap >= 0) {
            ApplyBalanceCap(m_balanceUserCap >= 0 ? m_balanceUserCap : 0);
            m_balanceAppliedCap = 0;
            m_balanceUserCap = -1;    // no longer holding anything on their behalf
        }
        // Left non-zero deliberately, so the re-measure block above does not
        // fire and re-impose a cap at a rung that is meant to run free.
        m_ladderStableFps = fps;

        DLOGW_INFO(L"[ladder] rung 2: %.1f fps -- %d display(s) to copy mirrors "
                   L"at %d; main uncapped%ls",
                   fps, changed, m_nLadderMirrorCap,
                   changed ? L"" : L" (already mirror-only, nothing left to give)");
        break;
    }
    default:
        break;
    }
    m_ladderRung = next;
    // The newly-engaged rung gets a real dwell before the next is considered.
    // Without it the fps threshold alone walks a badly-off preset through every
    // rung in a few seconds, paying for all three before learning whether the
    // first was enough.
    m_ladderNextMs = now + kLadderSettleMs;
}

void Engine::BalanceDisplayFrameRates()
{
    if (m_bChildMode) return;                  // a child balances nothing

    // The ladder runs first and outranks the balancer on the shared lever.
    // Both adjust the main's cap, and two governors writing one value is how
    // an oscillation is built -- the ladder answers absolute distress, the
    // balancer a relative imbalance, so distress wins.
    TickDistressLadder();
    if (m_ladderRung > 0) return;

    if (!m_bFrameBalanceEnabled) return;
    if (m_balanceState == BalanceState::RuledOut) return;

    const DWORD now = GetTickCount();
    if (m_balanceNextMs && now < m_balanceNextMs) return;

    int count = 0;
    const float slowest = SlowestDisplayFps(&count);
    if (count == 0 || slowest <= 0.0f) {
        // Nothing to balance against. Release anything held: a cap must never
        // outlive the reason for it.
        if (m_balanceState != BalanceState::Idle) ResetFrameRateBalancer();
        return;
    }
    const float mainFps = GetFps();
    if (mainFps <= 0.0f) return;

    switch (m_balanceState) {
    case BalanceState::Idle:
        if (mainFps <= slowest * kBalanceRatio) return;   // already in balance
        m_balanceBaseline = slowest;
        m_balanceUserCap = m_max_fps_w;                   // to restore later
        m_balanceAppliedCap = BalanceCapFor(slowest);
        ApplyBalanceCap(m_balanceAppliedCap);
        m_balanceState = BalanceState::Probing;
        m_balanceNextMs = now + kBalanceProbeMs;
        DLOGW_INFO(L"[balance] main %.1f > %.2fx slowest display %.1f -- "
                   L"capping main at %d and re-measuring",
                   mainFps, kBalanceRatio, slowest, m_balanceAppliedCap);
        break;

    case BalanceState::Probing: {
        const float base = m_balanceBaseline > 0.0f ? m_balanceBaseline : 1.0f;
        const float gain = slowest / base;
        if (gain >= kBalanceGainNeeded) {
            m_balanceState = BalanceState::Holding;
            m_balanceNextMs = now + kBalanceHoldMs;
            DLOGW_INFO(L"[balance] displays improved %.0f%% (%.1f -> %.1f); "
                       L"keeping the cap",
                       (gain - 1.0f) * 100.0f, m_balanceBaseline, slowest);
        } else {
            // "some renders might just have slow frame rates" -- this is one.
            DLOGW_INFO(L"[balance] displays did not improve (%.1f -> %.1f); "
                       L"releasing the cap, this preset is simply slow",
                       m_balanceBaseline, slowest);
            ResetFrameRateBalancer();
            m_balanceState = BalanceState::RuledOut;   // after the reset
        }
        break;
    }

    case BalanceState::Holding: {
        // Two-way: recomputed from the CURRENT slowest, so the main rises with
        // the displays and falls again if they slow down.
        const int want = BalanceCapFor(slowest);
        if (want != m_balanceAppliedCap) {
            ApplyBalanceCap(want);
            m_balanceAppliedCap = want;
            DLOGW_INFO(L"[balance] slowest display now %.1f -- main cap -> %d",
                       slowest, want);
        }
        // The user's own cap already being at or below ours means we are adding
        // nothing; get out of the way rather than hold a redundant override.
        if (m_balanceUserCap > 0 && m_balanceUserCap <= want) {
            DLOGW_INFO(L"[balance] user cap %d already at or below ours %d -- "
                       L"standing down", m_balanceUserCap, want);
            ResetFrameRateBalancer();
        } else {
            m_balanceNextMs = now + kBalanceHoldMs;
        }
        break;
    }

    default:
        break;
    }
}

static void CapMirrorSwapChainDim(int& w, int& h)
{
    CapDimToLongEdge(w, h, g_mirrorScCap.load(std::memory_order_relaxed));
}

// Copy-mode: primary BB size (capped) -- the panel shows the primary's image,
// so the primary's shape is the right shape.
// Opposite-aspect independent, or a display holding its OWN PRESET: the
// monitor's own aspect, long side <= 1920.
static void MirrorSwapChainSize(const DisplayOutput& out, int primW, int primH,
                                int& outW, int& outH)
{
    int x = 0, y = 0, layW = 0, layH = 0;
    ComputeMirrorLayout(out.config, x, y, layW, layH);
    const bool primPortrait = primH > primW;
    const bool panelPortrait = layH > layW;

    // bOwnProcess means "this display holds its own preset" (#186 phase 4), and
    // it is NOT paired with bIndependentRender -- the two were mutually
    // exclusive when one meant "spawn a child". So an own-preset display fell
    // into the else below and was given the PRIMARY's dimensions.
    //
    // Measured, with a portrait primary and two landscape own-preset panels:
    // the swap chain came out 1088x1920 (portrait) while the surface rendered
    // 1920x1088, so BlitOrientOutputToMirror letterboxed by
    // ny = dstAr/srcAr = 0.5667/1.7647 = 0.3211 -- the reported "too wide"
    // band, content in 32% of the panel height. A following mirror on the same
    // run got 1920x1088 and ny = 1.0.
    //
    // Orientation is not part of the own-preset test: that test asks whether
    // the primary's image can be stretched onto this panel, and for a display
    // rendering a DIFFERENT PRESET the answer is no at any orientation.
    // bOwnPreset too, not just bOwnProcess. This asked only about the process
    // and so answered "no" for the in-process own-preset tier, which then
    // rendered at the PRIMARY's shape and was stretched onto a panel of a
    // different one -- Shane: "own preset was squashed". Whether the image can
    // be stretched from the primary depends on whether this display shows the
    // primary's preset, which is exactly what bOwnPreset says.
    const bool ownPreset = out.config.bOwnPreset || out.config.bOwnProcess;
    const bool wantsOwnShape =
        ownPreset || (out.config.bIndependentRender && panelPortrait != primPortrait);

    if (wantsOwnShape && layW > 0 && layH > 0) {
        outW = layW;
        outH = layH;
    } else {
        outW = primW;
        outH = primH;
    }
    CapMirrorSwapChainDim(outW, outH);
}

// ─── Mirror Window Proc ──────────────────────────────────────────────────────
// GWLP_USERDATA holds the primary render HWND (set at CreateWindow).
// Mirrors use WS_EX_NOACTIVATE so they never become the key window; without
// explicit focus hand-off, clicks on a mirror leave local hotkeys dead.

static void RequestPrimaryFocus(HWND hMirror)
{
    HWND primary = (HWND)GetWindowLongPtrW(hMirror, GWLP_USERDATA);
    if (!primary || !IsWindow(primary))
        return;
    // Cross-thread: primary is owned by the UI thread — never SetFocus here.
    PostMessageW(primary, WM_MW_FOCUS_PRIMARY, 0, 0);
}

extern Engine g_engine;   // declared in App.cpp

// Map a click on a mirror panel into the shadertoy iMouse space.
//
// iMouse lives in PRIMARY-target pixels, and the orient pass rescales it by
// texsize when it renders the mirror — so feeding this one value keeps a single
// source of truth and makes the centre of a mirror land on the centre of the
// primary's view, with no second code path. The panel blit is a straight
// stretch of the orient render, so normalising across the client rect is the
// whole mapping; there are no letterbox bars to compensate for.
// Returns false if the sizes are not usable yet.
static bool MirrorClientToShadertoyMouse(HWND hMirror, POINT pt, float& outX, float& outY)
{
    RECT rc;
    if (!GetClientRect(hMirror, &rc))
        return false;
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw <= 1 || ch <= 1)
        return false;

    int targetW = 1, targetH = 1;
    if (g_engine.m_lpDX && g_engine.m_lpDX->m_backbuffer_width > 0 &&
        g_engine.m_lpDX->m_backbuffer_height > 0) {
        targetW = g_engine.m_lpDX->m_backbuffer_width;
        targetH = g_engine.m_lpDX->m_backbuffer_height;
    } else if (g_engine.m_WindowWidth > 0 && g_engine.m_WindowHeight > 0) {
        targetW = g_engine.m_WindowWidth;
        targetH = g_engine.m_WindowHeight;
    }
    if (targetW <= 1 || targetH <= 1)
        return false;

    float fx = (float)pt.x / (float)(cw - 1);
    float fy = (float)pt.y / (float)(ch - 1);
    fx = fx < 0.f ? 0.f : (fx > 1.f ? 1.f : fx);
    fy = fy < 0.f ? 0.f : (fy > 1.f ? 1.f : fy);

    outX = fx * (float)(targetW - 1);
    outY = (1.0f - fy) * (float)(targetH - 1);  // iMouse origin is bottom-left
    return true;
}

static LRESULT CALLBACK MirrorWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CLOSE:
        return 0; // Prevent user from closing the mirror window
    case WM_ERASEBKGND:
        return 1; // DX12 handles rendering; skip GDI erase

    case WM_MOUSEACTIVATE:
        // Do not activate the mirror; ask primary to take keyboard focus.
        RequestPrimaryFocus(hWnd);
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN:
    {
        // Drive the shadertoy mouse from the panel so mouse-steered presets
        // respond to a drag on a mirror. Shared on purpose (Shane, 2026-08-23):
        // one iMouse, so the primary and every mirror swing together.
        // Focus still goes to the primary and MA_NOACTIVATE still stands, so
        // clicking a mirror steers without stealing activation or keystrokes.
        RequestPrimaryFocus(hWnd);
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        float mx, my;
        if (MirrorClientToShadertoyMouse(hWnd, pt, mx, my)) {
            g_engine.m_stMouseX = mx;
            g_engine.m_stMouseY = my;
            g_engine.m_stClickX = mx;
            g_engine.m_stClickY = my;
            g_engine.m_stMouseDown = true;
            g_engine.m_stMouseJustClicked = true;
            SetCapture(hWnd);   // keep tracking if the drag leaves the panel
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        // Position updates only while held — same drag-only rule as the primary.
        if (g_engine.m_stMouseDown && GetCapture() == hWnd) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            float mx, my;
            if (MirrorClientToShadertoyMouse(hWnd, pt, mx, my)) {
                g_engine.m_stMouseX = mx;
                g_engine.m_stMouseY = my;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (GetCapture() == hWnd)
            ReleaseCapture();
        g_engine.m_stMouseDown = false;
        return 0;

    case WM_CAPTURECHANGED:
        // Capture yanked away mid-drag (alt-tab, another window): end the drag
        // rather than leaving iMouse.z stuck down forever.
        g_engine.m_stMouseDown = false;
        return 0;

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        RequestPrimaryFocus(hWnd);
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    {
        // If keys ever land here, forward to primary so hotkeys still work.
        HWND primary = (HWND)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (primary && IsWindow(primary))
            PostMessageW(primary, uMsg, wParam, lParam);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Helper: find a monitor by device name and return its current rect
struct FindMonitorCtx {
    const wchar_t* szDeviceName;
    RECT rcResult;
    bool bFound;
};

static BOOL CALLBACK FindMonitorCB(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindMonitorCtx*>(lp);
    MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (wcscmp(mi.szDevice, ctx->szDeviceName) == 0) {
            ctx->rcResult = mi.rcMonitor;
            ctx->bFound = true;
            return FALSE; // stop enumeration
        }
    }
    return TRUE;
}

// ─── Monitor Enumeration ──────────────────────────────────────────────────────

struct EnumMonitorCtx {
    Engine* engine;
    HMONITOR hRenderMonitor; // the monitor hosting the render window (for bSkippedSameMonitor)
};

static BOOL CALLBACK EnumMonitorCB(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<EnumMonitorCtx*>(lp);

    MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
    if (!GetMonitorInfoW(hMon, &mi))
        return TRUE;

    DisplayOutput out;
    out.config.type = DisplayOutputType::Monitor;
    out.config.bEnabled = false;
    out.config.bFullscreen = true;
    out.config.rcMonitor = mi.rcMonitor;
    wcsncpy_s(out.config.szDeviceName, mi.szDevice, _TRUNCATE);

    // Mark the render window's monitor as skipped (dynamic check in SendToDisplayOutputs
    // will update this at runtime when the render window moves between monitors)
    if (hMon == ctx->hRenderMonitor)
        out.bSkippedSameMonitor = true;

    // Get friendly display name from DISPLAY_DEVICEW
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0))
        wcsncpy_s(out.config.szName, dd.DeviceString, _TRUNCATE);
    else
        wcsncpy_s(out.config.szName, mi.szDevice, _TRUNCATE);

    // Append device name for disambiguation
    wchar_t label[128];
    swprintf(label, 128, L"%s (%s)", out.config.szName, mi.szDevice);
    wcsncpy_s(out.config.szName, label, _TRUNCATE);

    ctx->engine->m_displayOutputs.push_back(std::move(out));
    return TRUE;
}

void Engine::EnumerateDisplayOutputs()
{
    // m_displayOutputsMutex, not just render-thread confinement: this function
    // erases every monitor entry and repopulates it in several steps (erase,
    // re-enumerate, restore saved config), and ChildConfigureNow reads the
    // vector from the child-worker thread. Without the lock a read landing
    // mid-rebuild could see a reallocated buffer, or a monitor entry that
    // exists but has not yet had its saved settings restored. #27.
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);

    // Save existing monitor configs so we can preserve enabled state after re-enumeration
    // Mirrors are session-lived: carry the live MonitorMirrorState across
    // re-enumeration instead of letting the erase below destroy it. A plain
    // unique_ptr reset never runs DestroyMonitorMirror, so every enumeration
    // used to leak that mirror's reserved RTV block (16 slots total → mirrors
    // stopped coming back after ~4 idle "Mirror all" cycles) and its HWND.
    //
    // The whole config is carried, not a hand-picked list of fields. It used to
    // name five -- bEnabled, bFullscreen, nOpacity, bClickThrough,
    // bIndependentRender -- and none of the five child fields added later, so a
    // hotplug, a resolution change or a rearrangement silently reset
    // bOwnProcess, szPresetDir, szStartupPreset, fTimeBetweenPresets and
    // bSequentialOrder for every display (forgejo#35). Nothing killed the child
    // it had just disowned, so a full second instance kept rendering while
    // GET_CHILDREN -- which filters on bOwnProcess -- stopped listing it, and
    // no record was left associating it with a display. Copying the struct
    // means the next field added to DisplayOutputConfig is carried by default
    // instead of being lost until someone notices.
    struct SavedMonitorConfig {
        DisplayOutputConfig config;
        std::unique_ptr<MonitorMirrorState> monitorState;
        bool bMatched = false;   // a monitor that never came back
    };
    std::vector<SavedMonitorConfig> saved;
    for (auto& o : m_displayOutputs) {
        if (o.config.type == DisplayOutputType::Monitor) {
            SavedMonitorConfig s;
            s.config = o.config;
            s.monitorState = std::move(o.monitorState);
            saved.push_back(std::move(s));
        }
    }

    // Remove existing monitor entries (keep Spout outputs)
    m_displayOutputs.erase(
        std::remove_if(m_displayOutputs.begin(), m_displayOutputs.end(),
            [](const DisplayOutput& o) { return o.config.type == DisplayOutputType::Monitor; }),
        m_displayOutputs.end());

    // Determine which monitor hosts the render window
    HMONITOR hRenderMon = nullptr;
    if (m_lpDX && m_lpDX->GetHwnd())
        hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);

    EnumMonitorCtx ctx = { this, hRenderMon };
    EnumDisplayMonitors(NULL, NULL, EnumMonitorCB, reinterpret_cast<LPARAM>(&ctx));

    // Restore saved config for monitors that still exist (match by display rect first,
    // fall back to device name — Windows can reassign device names across enumerations)
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        for (auto& s : saved) {
            bool rectMatch = (out.config.rcMonitor.left == s.config.rcMonitor.left &&
                              out.config.rcMonitor.top == s.config.rcMonitor.top &&
                              out.config.rcMonitor.right == s.config.rcMonitor.right &&
                              out.config.rcMonitor.bottom == s.config.rcMonitor.bottom);
            if (rectMatch || wcscmp(out.config.szDeviceName, s.config.szDeviceName) == 0) {
                // Everything the USER chose comes back; the four fields the
                // MACHINE owns stay as this enumeration just discovered them,
                // because that is the entire point of re-enumerating.
                const DisplayOutputType type = out.config.type;
                const RECT rc = out.config.rcMonitor;
                wchar_t dev[32], name[128];
                wcsncpy_s(dev, out.config.szDeviceName, _TRUNCATE);
                wcsncpy_s(name, out.config.szName, _TRUNCATE);

                out.config = s.config;

                out.config.type = type;
                out.config.rcMonitor = rc;
                wcsncpy_s(out.config.szDeviceName, dev, _TRUNCATE);
                wcsncpy_s(out.config.szName, name, _TRUNCATE);
                s.bMatched = true;
                // Same physical rect → reuse the live window + swap chain.
                // Rect changed (resolution / rearrange) → let it be rebuilt at
                // the new size; the old one is destroyed on the render thread.
                if (s.monitorState && rectMatch)
                    out.monitorState = std::move(s.monitorState);
                break;
            }
        }
    }

    // Anything not carried over (monitor unplugged, rect changed) still owns a
    // swap chain, an RTV block and an HWND. WaitForGpu/DestroyWindow are
    // render-thread-only, so hand them off instead of destroying them here.
    {
        std::lock_guard<std::mutex> lk(m_orphanMirrorMutex);
        for (auto& s : saved) {
            if (s.monitorState)
                m_orphanMirrors.push_back(std::move(s.monitorState));
        }
    }

    // A monitor that did not come back takes its child process with it. Losing
    // the record and leaving the process running are two separate bugs, and
    // carrying the fields above only fixes the first: without this, unplugging
    // a display leaves a full instance rendering to nothing, unlisted by
    // GET_CHILDREN and addressable by nothing (forgejo#35).
    for (auto& s : saved) {
        if (!s.bMatched && s.config.bOwnProcess && s.config.szDeviceName[0]) {
            DebugLogWFmt(LOG_WARN, L"display %ls is gone; killing its child instance",
                         s.config.szDeviceName);
            RequestChildKill(s.config.szDeviceName);
        }
    }
}

// ─── Mirror Activation Failsafe ──────────────────────────────────────────────

Engine::MirrorActivateResult Engine::TryActivateMirrors(HWND hRenderWnd)
{
    // Which monitor hosts the render window (mirrors of that one are skipped)
    wchar_t renderDevice[32] = {};
    if (hRenderWnd) {
        HMONITOR hRenderMon = MonitorFromWindow(hRenderWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = { sizeof(mi) };
        if (hRenderMon && GetMonitorInfoW(hRenderMon, &mi))
            CopyTo(renderDevice, mi.szDevice);
    }

    int totalOther = 0;   // monitors that are not the render surface
    int enabledOther = 0;
    for (auto& o : m_displayOutputs) {
        if (o.config.type != DisplayOutputType::Monitor)
            continue;
        const bool isPrimary = renderDevice[0] &&
            wcscmp(o.config.szDeviceName, renderDevice) == 0;
        if (isPrimary)
            continue;
        totalOther++;
        if (o.config.bEnabled)
            enabledOther++;
    }

    // Case 1: User already enabled one or more non-primary monitors — keep that selection
    if (enabledOther > 0)
        return MirrorActivated;

    // Case 2: No other monitors at all — fullscreen only
    if (totalOther == 0)
        return MirrorFullscreenOnly;

    // Case 3: Other monitors exist but none enabled — auto-enable only non-primary
    // (do NOT force-enable the render monitor; that never gets a mirror window)
    for (auto& o : m_displayOutputs) {
        if (o.config.type != DisplayOutputType::Monitor)
            continue;
        const bool isPrimary = renderDevice[0] &&
            wcscmp(o.config.szDeviceName, renderDevice) == 0;
        o.config.bEnabled = !isPrimary;
    }
    SaveDisplayOutputSettings();
    return MirrorActivated;
}

// ─── INI Persistence ──────────────────────────────────────────────────────────

// [DisplayOutputs] and [DisplayOutput_*] -> resources/displays.json, once.
//
// 62 keys, and the last big block in settings.ini. The reader and writer above
// were not rewritten to speak json -- they map some sixty fields across two
// output types and decide whether a child process takes over a real monitor,
// and a field misread there puts a window on a screen the user is using. Only
// the STORE changed; see kv_store.h.
//
// Every key is found by enumeration, never from a list written here. The
// section is deleted straight afterwards, so a forgotten key would mean a lost
// one -- which is why ConfigBackend grew KeyNames().
void Engine::MigrateDisplaySettings()
{
    std::vector<std::wstring> secs;
    for (const std::wstring& sec : Config().SectionNames()) {
        if (_wcsicmp(sec.c_str(), L"DisplayOutputs") == 0 ||
            _wcsnicmp(sec.c_str(), L"DisplayOutput_", 14) == 0)
            secs.push_back(sec);
    }
    if (secs.empty()) return;               // already moved, or a fresh install

    size_t copied = 0;
    for (const std::wstring& sec : secs) {
        // A key already in the json wins: it is newer than anything still
        // sitting in the INI, and this runs before the loader.
        for (const std::wstring& key : Config().KeyNames(sec.c_str())) {
            if (DisplayCfg().Has(sec.c_str(), key.c_str())) continue;
            wchar_t val[512] = {};
            Config().GetStringTo(sec.c_str(), key.c_str(), L"", val, 512);
            DisplayCfg().SetString(sec.c_str(), key.c_str(), val);
            copied++;
        }
    }

    // Read every key back and compare before removing anything.
    for (const std::wstring& sec : secs) {
        for (const std::wstring& key : Config().KeyNames(sec.c_str())) {
            wchar_t was[512] = {}, now[512] = {};
            Config().GetStringTo(sec.c_str(), key.c_str(), L"", was, 512);
            DisplayCfg().GetStringTo(sec.c_str(), key.c_str(), L"\x01", now, 512);
            if (wcscmp(was, now) == 0) continue;
            DLOG_ERROR("displays: %ls/%ls did not survive ('%ls' vs '%ls'); "
                       "keeping the INI sections", sec.c_str(), key.c_str(), was, now);
            return;
        }
    }

    for (const std::wstring& sec : secs) Config().RemoveSection(sec.c_str());
    ConfigFlushAll();
    DLOG_INFO("displays: moved %zu key(s) in %zu section(s) to %ls",
              copied, secs.size(), DisplayCfg().Path().c_str());
}

void Engine::LoadDisplayOutputSettings()
{
    // Startup-only (no other thread touches m_displayOutputs yet at this
    // point), but taking the lock here too means every structural mutator
    // holds it with no carved-out exception to remember. #27.
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);

    int count = DisplayCfg().GetInt(L"DisplayOutputs", L"Count", -1);
    int legacyOpacity = DisplayCfg().GetInt(L"DisplayOutputs", L"MirrorOpacity", 100);
    if (legacyOpacity < 1) legacyOpacity = 1;
    if (legacyOpacity > 100) legacyOpacity = 100;
    // AltSMode replaced the MirrorModeForAltS bool. Read the old key as the
    // default for the new one so an existing "use mirrors for ALT-S" keeps
    // working without the user setting it again.
    m_nDisplayViewMode = DisplayCfg().GetInt(L"DisplayOutputs", L"ViewMode", 0);
    if (m_nDisplayViewMode < 0 || m_nDisplayViewMode > 1) m_nDisplayViewMode = 0;
    m_nAltSMode = DisplayCfg().GetInt(L"DisplayOutputs", L"AltSMode",
        DisplayCfg().GetBool(L"DisplayOutputs", L"MirrorModeForAltS", false)
            ? ALTS_MIRROR : ALTS_STRETCH);
    if (m_nAltSMode < ALTS_STRETCH || m_nAltSMode > ALTS_PROFILE)
        m_nAltSMode = ALTS_STRETCH;
    m_bMirrorPromptDisabled = DisplayCfg().GetBool(L"DisplayOutputs", L"MirrorPromptDisabled", false);
    m_bMirrorIndependentDefault = DisplayCfg().GetBool(L"DisplayOutputs", L"MirrorIndependentDefault", false);
    {
        int fps = DisplayCfg().GetInt(L"DisplayOutputs", L"MirrorMaxFps", 0);
        if (fps < 0) fps = 0;
        if (fps > 0 && fps < 5) fps = 5;
        if (fps > 240) fps = 240;
        m_nMirrorMaxFps.store(fps);
    }
    {
        // -1 derive from the VRAM budget, 0 uncapped, >0 a fixed long edge (#254).
        int cap = DisplayCfg().GetInt(L"DisplayOutputs", L"MirrorSwapChainCap", -1);
        if (cap < -1) cap = -1;
        if (cap > 16384) cap = 16384;
        m_nMirrorScCapSetting = cap;
        RefreshMirrorSwapChainCap();
    }
    {
        // -1 keeps the target-relative rule; >0 pins the fps at which the
        // distress ladder falls back to copy mirrors (#267).
        int trig = DisplayCfg().GetInt(L"DisplayOutputs", L"LadderMirrorTriggerFps", -1);
        if (trig < -1) trig = -1;
        if (trig > 720) trig = 720;
        m_nLadderTriggerSetting = trig;
    }

    if (count < 0) {
        // Legacy migration: no [DisplayOutputs] section yet.
        // Create a default Spout output from the old settings.
        DisplayOutput spout;
        spout.config.type = DisplayOutputType::Spout;
        spout.config.bEnabled = bSpoutOut;
        spout.config.bFixedSize = bSpoutFixedSize;
        spout.config.nWidth = nSpoutFixedWidth;
        spout.config.nHeight = nSpoutFixedHeight;
        CopyTo(spout.config.szName, L"MDropDX12");
        m_displayOutputs.insert(m_displayOutputs.begin(), std::move(spout));
        return;
    }

    for (int i = 0; i < count; i++) {
        wchar_t section[64];
        swprintf(section, 64, L"DisplayOutput_%d", i);

        wchar_t typeBuf[32] = {};
        DisplayCfg().GetStringTo(section, L"Type", L"Spout", typeBuf, 32);

        DisplayOutput out;
        if (wcscmp(typeBuf, L"Monitor") == 0)
            out.config.type = DisplayOutputType::Monitor;
        else
            out.config.type = DisplayOutputType::Spout;

        out.config.bEnabled = DisplayCfg().GetBool(section, L"Enabled", false);

        wchar_t nameBuf[128] = {};
        DisplayCfg().GetStringTo(section, L"Name", L"MDropDX12", nameBuf, 128);
        wcsncpy_s(out.config.szName, nameBuf, _TRUNCATE);

        if (out.config.type == DisplayOutputType::Monitor) {
            wchar_t devBuf[32] = {};
            DisplayCfg().GetStringTo(section, L"DeviceName", L"", devBuf, 32);
            wcsncpy_s(out.config.szDeviceName, devBuf, _TRUNCATE);
            out.config.bFullscreen = DisplayCfg().GetBool(section, L"Fullscreen", true);
            out.config.nOpacity = DisplayCfg().GetInt(section, L"Opacity", 100);
            if (out.config.nOpacity < 1) out.config.nOpacity = 1;
            if (out.config.nOpacity > 100) out.config.nOpacity = 100;
            out.config.bClickThrough = DisplayCfg().GetBool(section, L"ClickThrough", false);
            out.config.bWatermark = DisplayCfg().GetBool(section, L"Watermark", false);
            out.config.bAlwaysOnTop = DisplayCfg().GetBool(section, L"AlwaysOnTop", false);
            out.config.nWatermarkOpacity =
                DisplayCfg().GetInt(section, L"WatermarkOpacity", -1);
            out.config.bIndependentRender = DisplayCfg().GetBool(section, L"IndependentRender", m_bMirrorIndependentDefault);
            // Per-display child settings (forgejo#22)
            out.config.bOwnProcess = DisplayCfg().GetBool(section, L"OwnProcess", false);
            // Defaults to OwnProcess, which migrates every display configured
            // before the two were separate: back then asking for a display's
            // own preset was the only reason to tick OwnProcess, so an old
            // config saying OwnProcess=1 means "own preset, in its own
            // process" and must keep doing exactly that.
            out.config.bOwnPreset =
                DisplayCfg().GetBool(section, L"OwnPreset", out.config.bOwnProcess);

            // OwnProcess=1 now comes back as an in-process own-preset
            // display, and is therefore persisted like any other setting.
            //
            // It used to be DOWNGRADED here to an independent mirror, because
            // it meant "spawn a second MDropDX12.exe" and a stale flag nobody
            // remembered setting brought up borderless-fullscreen instances on
            // displays somebody was working on. That reason is gone with the
            // spawn: since #186 phase 4 the flag means the display holds its
            // own preset, rendered by this process, so restoring it starts no
            // process and covers no monitor that was not already an output.
            //
            // The key keeps its now-inaccurate name deliberately -- see
            // DisplayHoldsOwnPreset.
            {
                wchar_t pathBuf[MAX_PATH] = {};
                DisplayCfg().GetStringTo(section, L"PresetDir", L"", pathBuf, MAX_PATH);
                wcsncpy_s(out.config.szPresetDir, pathBuf, _TRUNCATE);
                pathBuf[0] = L'\0';
                DisplayCfg().GetStringTo(section, L"StartupPreset", L"", pathBuf, MAX_PATH);
                wcsncpy_s(out.config.szStartupPreset, pathBuf, _TRUNCATE);
            }
            out.config.fTimeBetweenPresets = DisplayCfg().GetFloat(section, L"TimeBetweenPresets", -1.0f);
            out.config.bSequentialOrder = DisplayCfg().GetBool(section, L"SequentialOrder", false);
            out.config.nPresetLock = DisplayCfg().GetInt(section, L"PresetLock", -1);
            if (out.config.nPresetLock < -1) out.config.nPresetLock = -1;
            // -1 inherits the global Messages setting (#255). Clamped the same
            // way, so a hand-edited file cannot put a display below "none".
            out.config.nMsgMaxOnScreen =
                DisplayCfg().GetInt(section, L"MsgMaxOnScreen", -1);
            if (out.config.nMsgMaxOnScreen < -1) out.config.nMsgMaxOnScreen = -1;
            if (out.config.nMsgMaxOnScreen > NUM_SUPERTEXTS)
                out.config.nMsgMaxOnScreen = NUM_SUPERTEXTS;
            if (out.config.nPresetLock > 1)  out.config.nPresetLock = 1;
        }
        else {
            out.config.bFixedSize = DisplayCfg().GetBool(section, L"FixedSize", false);
            out.config.nWidth = DisplayCfg().GetInt(section, L"Width", 1920);
            out.config.nHeight = DisplayCfg().GetInt(section, L"Height", 1080);
        }

        // For monitors, try to match to an already-enumerated monitor by DeviceName
        if (out.config.type == DisplayOutputType::Monitor) {
            bool matched = false;
            for (auto& existing : m_displayOutputs) {
                if (existing.config.type == DisplayOutputType::Monitor &&
                    wcscmp(existing.config.szDeviceName, out.config.szDeviceName) == 0) {
                    // WHOLESALE, not field by field (#295).
                    //
                    // This copied fourteen fields by hand, and three settings
                    // added since had never been added to the list:
                    // bAlwaysOnTop and nWatermarkOpacity (#282) and
                    // nMsgMaxOnScreen (#255). All three were read out of the
                    // file into `out` a few lines above and then dropped on the
                    // floor here -- so the enumerated entry kept its default,
                    // and the next SaveDisplayOutputSettings wrote that default
                    // back OVER the file.
                    //
                    // That is worse than not loading: the setting is destroyed,
                    // not merely ignored, and it looks like the save failing
                    // rather than the load. Reported as "always on top is not
                    // persisting for a single display ... the always on top was
                    // on startup", and reproduced as a pure config round trip --
                    // AlwaysOnTop=1 written with the app down came back 0 after
                    // one launch and one save.
                    //
                    // Assigning the struct means the next field added is
                    // carried without anyone having to remember this line,
                    // which is the actual failure here: three people added a
                    // setting and none of them knew to come and edit a copy
                    // list in the loader.
                    //
                    // THREE fields are the ENUMERATION's, not the file's, and
                    // have to be put back afterwards. Every one of them is
                    // marked "not persisted" at its declaration in
                    // display_output.h, and that list is the whole contract --
                    // anything else in the struct comes from the file.
                    //
                    //   rcMonitor     the monitor's rect, from the OS. The file
                    //                 does not carry it, so `out` has it zeroed
                    //                 -- and assigning that zero is what made
                    //                 display 2 vanish from the layout panel
                    //                 the first time this was written wholesale.
                    //   szName        the friendly monitor name Windows
                    //                 reported for the device present now.
                    //   rcTestWindow  deliberately never persisted.
                    //
                    // szDeviceName is identical by construction -- it is what
                    // matched.
                    wchar_t keepName[128];
                    CopyTo(keepName, existing.config.szName);
                    const RECT keepMonitor  = existing.config.rcMonitor;
                    const RECT keepTestRect = existing.config.rcTestWindow;
                    existing.config = out.config;
                    CopyTo(existing.config.szName, keepName);
                    existing.config.rcMonitor    = keepMonitor;
                    existing.config.rcTestWindow = keepTestRect;
                    matched = true;
                    break;
                }
            }
            // If monitor not currently connected, skip it
            if (!matched) continue;
        }
        else {
            // Spout outputs: insert at the beginning (before monitors)
            m_displayOutputs.insert(m_displayOutputs.begin(), std::move(out));
        }
    }

    // Sync legacy variables from first Spout output (backward compat)
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Spout) {
            bSpoutOut = out.config.bEnabled;
            bSpoutFixedSize = out.config.bFixedSize;
            nSpoutFixedWidth = out.config.nWidth;
            nSpoutFixedHeight = out.config.nHeight;
            break;
        }
    }
}

void Engine::SaveDisplayOutputSettings()
{

    int count = (int)m_displayOutputs.size();
    wchar_t buf[64];
    swprintf(buf, 64, L"%d", count);
    DisplayCfg().SetString(L"DisplayOutputs", L"Count", buf);
    swprintf(buf, 64, L"%d", m_nAltSMode);
    DisplayCfg().SetString(L"DisplayOutputs", L"AltSMode", buf);
    // The old key stays written and truthful, so downgrading a build does not
    // silently turn ALT-S back into stretch for someone using mirrors.
    swprintf(buf, 64, L"%d", m_nAltSMode == ALTS_MIRROR ? 1 : 0);
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorModeForAltS", buf);
    swprintf(buf, 64, L"%d", m_bMirrorPromptDisabled ? 1 : 0);
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorPromptDisabled", buf);
    swprintf(buf, 64, L"%d", m_bMirrorIndependentDefault ? 1 : 0);
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorIndependentDefault", buf);
    swprintf(buf, 64, L"%d", m_nMirrorMaxFps.load());
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorMaxFps", buf);
    swprintf(buf, 64, L"%d", m_nMirrorScCapSetting);
    DisplayCfg().SetString(L"DisplayOutputs", L"MirrorSwapChainCap", buf);
    swprintf(buf, 64, L"%d", m_nLadderTriggerSetting);
    DisplayCfg().SetString(L"DisplayOutputs", L"LadderMirrorTriggerFps", buf);

    for (int i = 0; i < count; i++) {
        auto& cfg = m_displayOutputs[i].config;
        wchar_t section[64];
        swprintf(section, 64, L"DisplayOutput_%d", i);

        DisplayCfg().SetString(section, L"Type", cfg.type == DisplayOutputType::Monitor ? L"Monitor" : L"Spout");
        swprintf(buf, 64, L"%d", cfg.bEnabled ? 1 : 0);
        DisplayCfg().SetString(section, L"Enabled", buf);
        DisplayCfg().SetString(section, L"Name", cfg.szName);

        if (cfg.type == DisplayOutputType::Monitor) {
            DisplayCfg().SetString(section, L"DeviceName", cfg.szDeviceName);
            swprintf(buf, 64, L"%d", cfg.bFullscreen ? 1 : 0);
            DisplayCfg().SetString(section, L"Fullscreen", buf);
            swprintf(buf, 64, L"%d", cfg.nOpacity);
            DisplayCfg().SetString(section, L"Opacity", buf);
            swprintf(buf, 64, L"%d", cfg.bClickThrough ? 1 : 0);
            DisplayCfg().SetString(section, L"ClickThrough", buf);
            swprintf(buf, 64, L"%d", cfg.bWatermark ? 1 : 0);
            DisplayCfg().SetString(section, L"Watermark", buf);
            swprintf(buf, 64, L"%d", cfg.bAlwaysOnTop ? 1 : 0);
            DisplayCfg().SetString(section, L"AlwaysOnTop", buf);
            swprintf(buf, 64, L"%d", cfg.nWatermarkOpacity);
            DisplayCfg().SetString(section, L"WatermarkOpacity", buf);
            swprintf(buf, 64, L"%d", cfg.bIndependentRender ? 1 : 0);
            DisplayCfg().SetString(section, L"IndependentRender", buf);
            // Per-display child settings (forgejo#22)
            //
            // bOwnProcess is the user's choice and nothing else's now (#261),
            // so it is saved plainly. It used to need filtering: an automatic
            // isolation set the same flag, and persisting that turned a
            // temporary response to one expensive preset into a permanent
            // setting no release path could take back -- which is how every
            // launch came up with three processes whatever the preset ("I just
            // launched the build via mdx12 ... and it has all three
            // processes"). With the cost gate gone there is no automatic
            // writer left to filter out.
            swprintf(buf, 64, L"%d", cfg.bOwnProcess ? 1 : 0);
            DisplayCfg().SetString(section, L"OwnProcess", buf);
            // Saved unconditionally, unlike OwnProcess: an automatic promotion
            // moves a display between the two OWN-PRESET tiers and must not
            // change whether it holds its own preset at all.
            swprintf(buf, 64, L"%d", cfg.bOwnPreset ? 1 : 0);
            DisplayCfg().SetString(section, L"OwnPreset", buf);
            DisplayCfg().SetString(section, L"PresetDir", cfg.szPresetDir);
            DisplayCfg().SetString(section, L"StartupPreset", cfg.szStartupPreset);
            swprintf(buf, 64, L"%.3f", cfg.fTimeBetweenPresets);
            DisplayCfg().SetString(section, L"TimeBetweenPresets", buf);
            swprintf(buf, 64, L"%d", cfg.bSequentialOrder ? 1 : 0);
            DisplayCfg().SetString(section, L"SequentialOrder", buf);
            swprintf(buf, 64, L"%d", cfg.nPresetLock);
            DisplayCfg().SetString(section, L"PresetLock", buf);
            swprintf(buf, 64, L"%d", cfg.nMsgMaxOnScreen);
            DisplayCfg().SetString(section, L"MsgMaxOnScreen", buf);
        }
        else {
            swprintf(buf, 64, L"%d", cfg.bFixedSize ? 1 : 0);
            DisplayCfg().SetString(section, L"FixedSize", buf);
            swprintf(buf, 64, L"%d", cfg.nWidth);
            DisplayCfg().SetString(section, L"Width", buf);
            swprintf(buf, 64, L"%d", cfg.nHeight);
            DisplayCfg().SetString(section, L"Height", buf);
        }
    }

    // Also sync legacy INI keys from first Spout output
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Spout) {
            bSpoutOut = out.config.bEnabled;
            bSpoutFixedSize = out.config.bFixedSize;
            nSpoutFixedWidth = out.config.nWidth;
            nSpoutFixedHeight = out.config.nHeight;
            break;
        }
    }
}

// ─── Display Output Init / Destroy ────────────────────────────────────────────

void Engine::InitDisplayOutput(DisplayOutput& out)
{
    if (out.config.type == DisplayOutputType::Spout) {
        // A -child instance publishes no Spout sender. Sender names live in a
        // namespace shared across the whole machine, and a child reading the
        // same settings.ini would advertise a second sender under the parent's
        // name -- receivers would then bind to whichever registered last. The
        // parent owns the outward-facing sender; children only render.
        if (m_bChildMode)
            return;
        if (out.spoutState && out.spoutState->bReady)
            return; // already live — never thrash Spout wraps mid-session
        out.spoutState = std::make_unique<SpoutOutputState>();
        auto& ss = *out.spoutState;

        if (!m_lpDX || !m_lpDX->m_device.Get())
            return;

        // Convert wide name to ANSI for Spout API
        char senderNameA[256] = {};
        WideCharToMultiByte(CP_ACP, 0, out.config.szName, -1, senderNameA, 256, NULL, NULL);
        ss.sender.SetName(senderNameA);

        if (!ss.sender.Open(m_lpDX->m_device.Get(),
                            m_lpDX->m_commandQueue.Get())) {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: Spout Open failed for '%s'\n", senderNameA);
            DebugLogA(logBuf, LOG_ERROR);
            return;
        }

        for (int n = 0; n < DXC_FRAME_COUNT; n++) {
            if (!ss.sender.WrapResource(
                    m_lpDX->m_renderTargets[n].Get(),
                    &ss.wrappedBackBuffers[n],
                    D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                char logBuf[512];
                sprintf(logBuf, "InitDisplayOutput: Spout WrapResource failed [%d]\n", n);
                DebugLogA(logBuf, LOG_ERROR);
                // Cleanup partial wraps
                for (int j = 0; j < n; j++) {
                    if (ss.wrappedBackBuffers[j]) {
                        ss.wrappedBackBuffers[j]->Release();
                        ss.wrappedBackBuffers[j] = nullptr;
                    }
                }
                ss.sender.Close();
                return;
            }
        }
        ss.bReady = true;
        { char logBuf[512]; sprintf(logBuf, "InitDisplayOutput: Spout sender '%s' ready\n", senderNameA); DebugLogA(logBuf); }
    }
    else if (out.config.type == DisplayOutputType::Monitor) {
        if (!m_lpDX || !m_lpDX->m_device.Get() || !m_lpDX->m_swapChain.Get())
            return;

        // Idempotent: if a session-lived mirror already exists, never recreate.
        // Destroying + CreateSwapChain while the GPU still references back buffers
        // is what TDRs the device after a couple of enable/disable toggles.
        if (out.monitorState && out.monitorState->bReady && out.monitorState->swapChain) {
            out.monitorState->bSoftDisabled = false;
            out.monitorState->nFramesUntilHardDestroy = 0;
            if (out.monitorState->hWnd && !IsWindowVisible(out.monitorState->hWnd))
                ShowWindow(out.monitorState->hWnd, SW_SHOWNOACTIVATE);
            return;
        }
        // Stale half-init (failed earlier) — drop without WaitForGpu (no live SC)
        if (out.monitorState)
            out.monitorState.reset();

        VMARK("InitDisplayOutput enter");
        {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: Starting init for %ls (%ls)\n",
                out.config.szName, out.config.szDeviceName);
            DebugLogA(logBuf, LOG_WARN);
        }

        // Safety: don't mirror the monitor hosting the render window.
        // In watermark mode, use the stored target device name for deterministic detection.
        {
            wchar_t renderDevice[32] = {};
            if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
                CopyTo(renderDevice, m_szWatermarkRenderDevice);
            } else if (m_lpDX->GetHwnd()) {
                HMONITOR hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
                if (hRenderMon) {
                    MONITORINFOEXW mi = { sizeof(mi) };
                    if (GetMonitorInfoW(hRenderMon, &mi))
                        CopyTo(renderDevice, mi.szDevice);
                }
            }
            if (renderDevice[0] && wcscmp(renderDevice, out.config.szDeviceName) == 0) {
                char logBuf[256];
                sprintf(logBuf, "InitDisplayOutput: Skipping %ls — render window's monitor\n",
                    out.config.szDeviceName);
                DebugLogA(logBuf, LOG_WARN);
                out.bSkippedSameMonitor = true;
                return;
            }
        }

        // Do NOT WaitForGpu here — it freezes the whole machine when called often
        // (enable/disable cycles). Soft-destroy already waits multiple frames before
        // releasing swap chains. PeekMessage/Dispatch is also avoided: re-entering
        // the message pump from the render thread can deadlock DXGI.

        out.monitorState = std::make_unique<MonitorMirrorState>();
        auto& ms = *out.monitorState;
        ms.bSoftDisabled = false;
        ms.nFramesUntilHardDestroy = 0;

        // Register mirror window class (once)
        if (!m_bMirrorClassRegistered) {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = MirrorWndProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = L"MDropDX12_Mirror";
            if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
                m_bMirrorClassRegistered = true;
        }

        // Re-query the monitor rect fresh by device name (cached rect may be stale)
        FindMonitorCtx fmc = { out.config.szDeviceName, {}, false };
        EnumDisplayMonitors(NULL, NULL, FindMonitorCB, reinterpret_cast<LPARAM>(&fmc));
        if (fmc.bFound) {
            out.config.rcMonitor = fmc.rcResult;
        } else {
            char logBuf[256];
            sprintf(logBuf, "InitDisplayOutput: WARNING — monitor %ls not found by EnumDisplayMonitors!\n",
                out.config.szDeviceName);
            DebugLogA(logBuf, LOG_WARN);
        }

        RECT rc = out.config.rcMonitor;
        int monW = rc.right - rc.left;
        int monH = rc.bottom - rc.top;
        {
            char logBuf[512];
            sprintf(logBuf, "InitDisplayOutput: %ls rect = (%d,%d)-(%d,%d) size %dx%d\n",
                out.config.szDeviceName,
                (int)rc.left, (int)rc.top, (int)rc.right, (int)rc.bottom, monW, monH);
            DebugLogA(logBuf, LOG_WARN);
        }

        if (monW <= 0 || monH <= 0) {
            DebugLogA("InitDisplayOutput: Invalid monitor rect, skipping\n", LOG_WARN);
            out.monitorState.reset();
            return;
        }

        // Create borderless popup covering the target monitor.
        // IMPORTANT: Do NOT use WS_EX_LAYERED when opacity is 100%. Flip-model
        // swap chains + layered windows let DWM composite other monitors' content
        // through the mirror (landscape "ghost" strip on portrait, often flickering).
        // Layered is only for partial opacity or click-through.
        //
        // Through the shared resolver, so a watermarked display arrives
        // watermarked (#286). This read nOpacity and bClickThrough straight
        // out of the config, and nothing set m_bMirrorStylesDirty afterwards,
        // so ApplyMirrorWindowStyles -- which does know about the watermark --
        // never ran for a window it had just created. A display watermarked in
        // displays.json or in a profile therefore came up fully opaque,
        // clickable and behind everything, while opacity and click-through set
        // on their own worked, because those two are what this line read.
        bool wmClickThru = false, wmOnTop = false;
        int wmOpacity = 100;
        ResolveMirrorWindowStyle(out.config, wmClickThru, wmOpacity, wmOnTop);

        const bool needLayered = (wmOpacity < 100) || wmClickThru;
        DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        if (needLayered)
            exStyle |= WS_EX_LAYERED;
        if (wmClickThru)
            exStyle |= WS_EX_TRANSPARENT;
        // Fullscreen checkbox: full monitor vs centered windowed layout
        int layX = rc.left, layY = rc.top, layW = monW, layH = monH;
        ComputeMirrorLayout(out.config, layX, layY, layW, layH);

        ms.hWnd = CreateWindowExW(
            exStyle,
            L"MDropDX12_Mirror",
            L"MDropDX12 Mirror",
            WS_POPUP,
            layX, layY, layW, layH,
            nullptr, nullptr, GetModuleHandle(NULL), nullptr);
        if (!ms.hWnd) {
            DebugLogA("InitDisplayOutput: CreateWindowExW failed for mirror\n", LOG_ERROR);
            out.monitorState.reset();
            return;
        }
        // Primary HWND for focus hand-off on click (local hotkeys need primary focus).
        HWND hPrimary = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
        SetWindowLongPtrW(ms.hWnd, GWLP_USERDATA, (LONG_PTR)hPrimary);
        if (needLayered) {
            BYTE alpha = (BYTE)(wmOpacity * 255 / 100);
            if (alpha < 3) alpha = 3;
            SetLayeredWindowAttributes(ms.hWnd, 0, alpha, LWA_ALPHA);
        }
        // Mirrors must stay above other windows on their monitor; otherwise the
        // landscape primary (or desktop) can show through as a ghost. The hop
        // to TOPMOST and straight back is how that is done without leaving the
        // window stickily on top.
        //
        // Not in testing mode. That hop is momentary but it is still a real
        // raise over whatever the user is doing, and it fires again on every
        // hide->show. Shane, 2026-09-10: "you consistently want to take over
        // every display on my computer to test when you don't need to... you
        // can do all this testing about the displays and the old preset
        // without having to actually be the topmost window."
        //
        // Same principle testing mode already applies to focus and to saved
        // window positions: anything done for the benefit of a human who is
        // looking is switched off when the human is not looking at this one.
        // A test reads the frame with PrintWindow, which captures a fully
        // obscured window correctly, so nothing here needs to be in front.
        SetWindowPos(ms.hWnd, MirrorZOrder(true),
                     layX, layY, layW, layH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // wmOnTop, not m_bAlwaysOnTop: a watermarked display -- or one with its
        // own Keep on top -- must STAY up rather than be dropped straight back
        // down by the hop (#286). The global flag cannot express "this one",
        // which is exactly what both of those settings are.
        if (MirrorZOrderHop() && !wmOnTop)
            SetWindowPos(ms.hWnd, HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);

        VMARK("InitDisplayOutput window created");

        // Get DXGI factory from existing swap chain
        ComPtr<IDXGIFactory4> factory;
        HRESULT hr = m_lpDX->m_swapChain->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            DebugLogA("InitDisplayOutput: GetParent(IDXGIFactory4) failed\n", LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }

        // Copy mode: SC = primary BB (CopyResource + DXGI stretch).
        // Independent opposite-aspect: SC = monitor aspect, long side capped at
        // 1920 (full 2160x3840 + a second classic pass TDRs). Same-aspect
        // independent still uses primary size and blits.
        const int primW = (m_lpDX->m_client_width > 0) ? m_lpDX->m_client_width : layW;
        const int primH = (m_lpDX->m_client_height > 0) ? m_lpDX->m_client_height : layH;
        MirrorSwapChainSize(out, primW, primH, ms.width, ms.height);
        ms.bIndependentSized = (ms.width != primW || ms.height != primH);

        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Width = (UINT)ms.width;
        scDesc.Height = (UINT)ms.height;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = DXC_FRAME_COUNT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        scDesc.Flags = 0;
        scDesc.Scaling = DXGI_SCALING_STRETCH;

        VMARK("InitDisplayOutput before swapchain");
        ComPtr<IDXGISwapChain1> sc1;
        hr = factory->CreateSwapChainForHwnd(
            m_lpDX->m_commandQueue.Get(), ms.hWnd, &scDesc, nullptr, nullptr, &sc1);
        ms.bufferCount = scDesc.BufferCount;
        VMARK("InitDisplayOutput after swapchain");
        if (FAILED(hr)) {
            char logBuf[256]; sprintf(logBuf, "InitDisplayOutput: CreateSwapChainForHwnd failed (0x%08X)\n", (unsigned)hr);
            DebugLogA(logBuf, LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }
        factory->MakeWindowAssociation(ms.hWnd, DXGI_MWA_NO_ALT_ENTER);
        hr = sc1.As(&ms.swapChain);
        if (FAILED(hr)) {
            DebugLogA("InitDisplayOutput: QueryInterface IDXGISwapChain4 failed\n", LOG_ERROR);
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }

        // Get back buffers (+ permanent-reserve RTVs that survive resize rewinds)
        ms.bHasRtv = false;
        ms.rtvSlotBase = m_lpDX->AllocateMirrorRtvBlock(ms.bufferCount);
        if (ms.rtvSlotBase == UINT_MAX) {
            DebugLogA("InitDisplayOutput: no reserved RTV slots for mirror\n", LOG_ERROR);
            ms.swapChain.Reset();
            DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
            out.monitorState.reset();
            return;
        }
        for (UINT i = 0; i < ms.bufferCount && i < MIRROR_BUFFER_COUNT; i++) {
            hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
            if (FAILED(hr)) {
                char logBuf[256]; sprintf(logBuf, "InitDisplayOutput: GetBuffer(%d) failed\n", i);
                DebugLogA(logBuf, LOG_ERROR);
                m_lpDX->FreeMirrorRtvBlock(ms.rtvSlotBase, ms.bufferCount);
                ms.rtvSlotBase = UINT_MAX;
                ms.swapChain.Reset();
                DestroyWindow(ms.hWnd); ms.hWnd = nullptr;
                out.monitorState.reset();
                return;
            }
            ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
            m_lpDX->m_device->CreateRenderTargetView(ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
        }
        ms.bHasRtv = true;
        ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
        ms.bNeedsFullChainClear = true;
        ms.paintedBufferMask = 0;
        for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
            ms.bbState[bi] = D3D12_RESOURCE_STATE_COMMON;

        ms.bReady = true;
        {
            char logBuf[256];
            sprintf(logBuf, "InitDisplayOutput: Mirror %ls READY (hwnd=%p, swapchain=%p, %dx%d copy-size)\n",
                out.config.szDeviceName, (void*)ms.hWnd, (void*)ms.swapChain.Get(),
                ms.width, ms.height);
            DebugLogA(logBuf, LOG_WARN);
        }
    }
}

void Engine::DestroyDisplayOutput(DisplayOutput& out)
{
    if (out.spoutState) {
        auto& ss = *out.spoutState;
        for (int n = 0; n < DXC_FRAME_COUNT; n++) {
            if (ss.wrappedBackBuffers[n]) {
                ss.wrappedBackBuffers[n]->Release();
                ss.wrappedBackBuffers[n] = nullptr;
            }
        }
        if (ss.bReady) {
            ss.sender.Close();
            ss.bReady = false;
        }
        out.spoutState.reset();
    }
    if (out.monitorState) {
        DestroyMonitorMirror(*out.monitorState);
        out.monitorState.reset();
    }
}

// Single teardown path for a mirror's GPU + window resources. Callers must be
// on the render thread: WaitForGpu freezes the UI thread, and DestroyWindow
// only works from the thread that created the window.
void Engine::DestroyMonitorMirror(MonitorMirrorState& ms)
{
    ms.bReady = false;
    ms.bSoftDisabled = true;
    // Must idle the GPU before releasing swap-chain buffers. Without this,
    // in-flight mirror command lists TDR the device (seen as lockup after
    // 1–2 toggles when destroy ran mid-session). Only called on profile
    // load / refresh / app shutdown — never on enable/disable toggle.
    if (m_lpDX && ms.swapChain)
        m_lpDX->WaitForGpu();
    if (m_lpDX && ms.rtvSlotBase != UINT_MAX) {
        m_lpDX->FreeMirrorRtvBlock(ms.rtvSlotBase, ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT);
        ms.rtvSlotBase = UINT_MAX;
    }
    ms.bHasRtv = false;
    for (UINT i = 0; i < MIRROR_BUFFER_COUNT; i++)
        ms.backBuffers[i].Reset();
    ms.swapChain.Reset();
    if (ms.hWnd) {
        DestroyWindow(ms.hWnd);
        ms.hWnd = nullptr;
    }
}

// Render-thread drain for mirrors detached by EnumerateDisplayOutputs.
void Engine::DrainOrphanedMirrors()
{
    std::vector<std::unique_ptr<MonitorMirrorState>> orphans;
    {
        std::lock_guard<std::mutex> lk(m_orphanMirrorMutex);
        if (m_orphanMirrors.empty())
            return;
        orphans.swap(m_orphanMirrors);
    }
    for (auto& ms : orphans) {
        if (ms)
            DestroyMonitorMirror(*ms);
    }
    char logBuf[128];
    sprintf(logBuf, "DrainOrphanedMirrors: destroyed %d re-enumerated mirror(s)\n", (int)orphans.size());
    DebugLogA(logBuf, LOG_WARN);
}

void Engine::ReleaseDisplayOutputWraps()
{
    for (auto& out : m_displayOutputs) {
        if (out.spoutState) {
            auto& ss = *out.spoutState;
            for (int n = 0; n < DXC_FRAME_COUNT; n++) {
                if (ss.wrappedBackBuffers[n]) {
                    ss.wrappedBackBuffers[n]->Release();
                    ss.wrappedBackBuffers[n] = nullptr;
                }
            }
            ss.sender.Close();
            ss.bReady = false;
            out.spoutState.reset();
        }
    }
}

void Engine::DestroyAllDisplayOutputs()
{
    StopMirrorThread();
    DrainOrphanedMirrors();
    for (auto& out : m_displayOutputs)
        DestroyDisplayOutput(out);

    // Release mirror command objects
    m_mirrorCmdList.Reset();
    for (int i = 0; i < DXC_FRAME_COUNT; i++)
        m_mirrorCmdAllocators[i].Reset();
    if (m_lpDX && !LagIndepFenceIdle())
        m_lpDX->WaitForGpu();
    ReleaseLagIndepObjects();
}

bool Engine::LagIndepFenceIdle() const
{
    if (!m_lagIndepFence)
        return true;
    return m_lagIndepFence->GetCompletedValue() >= m_lagIndepSubmitted;
}

bool Engine::EnsureLagIndepObjects()
{
    if (!m_lpDX || !m_lpDX->m_device)
        return false;
    if (!m_lagIndepFence) {
        HRESULT hr = m_lpDX->m_device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_lagIndepFence));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateFence failed\n", LOG_ERROR);
            return false;
        }
        m_lagIndepSubmitted = 0;
        m_lagIndepSignal = 0;
    }
    if (!m_lagIndepAlloc) {
        HRESULT hr = m_lpDX->m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_lagIndepAlloc));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateCommandAllocator failed\n", LOG_ERROR);
            return false;
        }
    }
    if (!m_lagIndepList) {
        HRESULT hr = m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_lagIndepAlloc.Get(), nullptr, IID_PPV_ARGS(&m_lagIndepList));
        if (FAILED(hr)) {
            DebugLogA("EnsureLagIndepObjects: CreateCommandList failed\n", LOG_ERROR);
            return false;
        }
        m_lagIndepList->Close();
    }
    return true;
}

void Engine::ReleaseLagIndepObjects()
{
    m_lagIndepList.Reset();
    m_lagIndepAlloc.Reset();
    m_lagIndepFence.Reset();
    m_lagIndepSubmitted = 0;
    m_lagIndepSignal = 0;
    m_lagIndepAuxFrame = UINT_MAX;
}

void Engine::LockMirrorEngine() {
    // No yield. A hand-rolled one stood here for two years in two flavours,
    // and both were wrong in opposite directions -- see the note on
    // m_mirrorEngineMutex. The workers block on the mutex properly now, so
    // they are in its wait list and the OS hands it to them.
    m_mirrorEngineMutex.lock();
}
void Engine::UnlockMirrorEngine() { m_mirrorEngineMutex.unlock(); }

namespace {
// One per worker: the engine plus the surface it serves, owned by the thread.
struct MirrorWorkerArg {
    mdrop::Engine*                                engine;
    std::shared_ptr<mdrop::Engine::MirrorSurface> surf;
};
}

static unsigned __stdcall MirrorThreadThunk(void* arg)
{
    // The shared_ptr is what keeps the surface alive for the thread's whole
    // life. A retire can drop the engine's reference while this thread is
    // mid-frame; it must not free the sim and the pipe under it.
    std::unique_ptr<MirrorWorkerArg> a(reinterpret_cast<MirrorWorkerArg*>(arg));
    a->engine->MirrorThreadMain(a->surf);
    return 0;
}

bool Engine::StartSurfaceWorker(const std::shared_ptr<MirrorSurface>& surf)
{
    if (!surf)
        return false;
    if (surf->hThread)
        return true;
    if (!m_lpDX || !m_lpDX->m_device || !m_mirrorQueue)
        return false;

    surf->quit.store(false);
    if (!surf->hWake)
        surf->hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!surf->hWake)
        return false;

    // SUSPENDED, because a worker MUST own an upload ring before it runs a
    // single frame and a ring is claimed per THREAD ID -- which does not exist
    // until the thread does. Sharing the aux ring let a worker reset the
    // render thread's offsets mid-frame (black presets, then TDR); that is why
    // phase 5a made the ring per thread and why this cannot simply start.
    auto* arg = new MirrorWorkerArg{ this, surf };
    unsigned tid = 0;
    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, MirrorThreadThunk, arg,
                                      CREATE_SUSPENDED, &tid);
    if (!h) {
        delete arg;
        DebugLogA("StartSurfaceWorker: _beginthreadex failed\n", LOG_ERROR);
        return false;
    }
    // No ring, no worker. Running one anyway would put it on the shared aux
    // ring, which is the exact failure the per-thread rings exist to prevent --
    // and IsMirrorWorkerThread() reads the ring registry, so a ringless worker
    // would also not be recognised as one by EnsureOrientPipeline's guard.
    if (!m_lpDX->SetMirrorUploadThreadId((DWORD)tid) ||
        !m_lpDX->CreateMirrorUploadBuffer((DWORD)tid)) {
        DebugLogA("StartSurfaceWorker: no upload ring available - surface stays idle\n",
                  LOG_ERROR);
        surf->quit.store(true);
        ResumeThread(h);            // let it observe the quit flag and exit
        WaitForSingleObject(h, 2000);
        CloseHandle(h);
        m_lpDX->ReleaseMirrorUploadBuffer((DWORD)tid);
        if (DXContext::MirrorUploadRing* r = m_lpDX->MirrorRingForThread((DWORD)tid))
            r->threadId.store(0, std::memory_order_release);
        return false;
    }
    VMARK("StartSurfaceWorker after upload ring");
    surf->hThread = h;
    surf->threadId = tid;
    ResumeThread(h);
    char buf[192];
    sprintf(buf, "mirror worker started for %ls (tid %u)\n",
            surf->device.empty() ? L"(none)" : surf->device.c_str(), tid);
    DebugLogA(buf, LOG_INFO);
    return true;
}

// See the note on the declaration. WaitForGpu covers the main queue; this
// covers the mirror queue, and anything freeing a GPU-referenced object needs
// both (#206).
void Engine::WaitForSurfaceWork(DWORD timeoutMs)
{
    // This runs from every WaitForGpu, which is a couple of hundred times a
    // second -- measured at 8567 entries across 30 preset loads. So the
    // frequency is read once, not per call; it does not change.
    static LARGE_INTEGER s_freq = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    m_diagSurfaceWaitCalls.fetch_add(1, std::memory_order_relaxed);
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    bool waited = false;

    for (auto& sp : m_mirrorSurfaces) {
        MirrorSurface& s = *sp;
        if (!s.workFence || s.workSubmitted == 0 || s.WorkIdle())
            continue;
        waited = true;
        // Borrow the surface's own event: the worker only waits on it from
        // inside its own iteration, and this runs on the render thread while
        // that worker is either blocked on the engine mutex or between frames.
        if (!s.fenceEvt)
            continue;
        if (SUCCEEDED(s.workFence->SetEventOnCompletion(s.workSubmitted, s.fenceEvt)))
            WaitForSingleObject(s.fenceEvt, timeoutMs);
    }

    if (!waited)
        return;   // every surface was already idle -- not a stall, don't record it
    QueryPerformanceCounter(&t1);
    const int us = (int)((t1.QuadPart - t0.QuadPart) * 1000000 / s_freq.QuadPart);
    m_diagSurfaceWaitUs.store(us, std::memory_order_relaxed);
    m_diagSurfaceWaitCount.fetch_add(1, std::memory_order_relaxed);
    int prev = m_diagSurfaceWaitMaxUs.load(std::memory_order_relaxed);
    while (us > prev &&
           !m_diagSurfaceWaitMaxUs.compare_exchange_weak(prev, us,
                                                         std::memory_order_relaxed))
        ;
}

void Engine::StopSurfaceWorker(MirrorSurface& surf)
{
    bool joined = true;   // #214: false means the worker may still be running
    if (surf.hThread) {
        surf.quit.store(true);
        if (surf.hWake)
            SetEvent(surf.hWake);
        // The join TIMES OUT on purpose. This runs on the render thread, which
        // may be holding the engine mutex the worker is spinning for; the
        // worker's try_lock loop checks the quit flag every spin, so it gets
        // out -- but if anything else has it parked, waiting forever here would
        // hang the render thread instead.
        joined = (WaitForSingleObject(surf.hThread, 4000) == WAIT_OBJECT_0);
        CloseHandle(surf.hThread);
        surf.hThread = nullptr;
    }
    if (surf.hWake) {
        CloseHandle(surf.hWake);
        surf.hWake = nullptr;
    }
    if (m_lpDX && surf.threadId) {
        // The join above says the CPU side is finished. It does NOT say the
        // GPU is: the worker's last submitted list still reads its vertices
        // out of this ring, and unmapping and freeing 32 MB underneath a
        // running list is a use-after-free (#214). Wait for that surface's own
        // fence -- the main-queue fence is a different queue and proves
        // nothing here.
        if (joined && surf.workFence && surf.workSubmitted &&
            surf.workFence->GetCompletedValue() < surf.workSubmitted && surf.fenceEvt) {
            // The worker has exited, so its event is nobody else's now.
            if (SUCCEEDED(surf.workFence->SetEventOnCompletion(surf.workSubmitted,
                                                               surf.fenceEvt)))
                WaitForSingleObject(surf.fenceEvt, 4000);
        }

        if (joined) {
            // Buffer first, then the slot -- releasing the slot would make the ring
            // unfindable and leak the 32 MB it holds.
            m_lpDX->ReleaseMirrorUploadBuffer((DWORD)surf.threadId);
            if (DXContext::MirrorUploadRing* r = m_lpDX->MirrorRingForThread((DWORD)surf.threadId))
                r->threadId.store(0, std::memory_order_release);
        } else {
            // The join timed out, so that thread is still running and may be
            // writing into this very ring. Leak it, deliberately: 32 MB and a
            // permanently claimed ring slot are a far better outcome than
            // unmapping memory a live thread is writing to. Keeping threadId
            // set is what makes the leak complete -- the slot stays claimed so
            // nothing else adopts a ring the old worker still holds.
            DebugLogA("StopSurfaceWorker: join timed out -- leaking the upload "
                      "ring rather than freeing it under a live worker (#214)\n",
                      LOG_ERROR);
            return;
        }
    }
    surf.threadId = 0;
}

void Engine::StartMirrorThread()
{
    if (!m_lpDX || !m_lpDX->m_device)
        return;

    VMARK("StartMirrorThread enter");

    // The queue is shared by every worker: ID3D12CommandQueue is free-threaded,
    // so concurrent ExecuteCommandLists and Signal on it are legal.
    if (!m_mirrorQueue) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(m_lpDX->m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_mirrorQueue)))) {
            DebugLogA("StartMirrorThread: CreateCommandQueue failed\n", LOG_ERROR);
            return;
        }
    }

    // From here on, "wait for the GPU" has to include this queue. Installing it
    // on DXContext rather than editing 20 WaitForGpu call sites is deliberate:
    // the next person to free a GPU object gets the mirror queue covered
    // without knowing it exists, which is exactly what went wrong with the
    // preset PSOs (#206).
    //
    // The thread guard matters. WaitForSurfaceWork walks m_mirrorSurfaces
    // unlocked, which is safe only for the thread that also adds to and removes
    // from it -- the render thread, the one running here now. A WaitForGpu from
    // anywhere else gets the main-queue wait it always had and nothing more.
    const DWORD renderTid = GetCurrentThreadId();
    m_lpDX->m_waitForAuxQueues = [this, renderTid]() {
        if (GetCurrentThreadId() == renderTid)
            WaitForSurfaceWork();
    };

    // Same thread guard, same reason: m_mirrorSurfaces is walked unlocked and
    // only the render thread adds to or removes from it (#214). Off that
    // thread this reports "not idle", which merely defers a release by a
    // frame -- the safe direction, and the deferred-release queue is drained
    // from the render thread anyway.
    m_lpDX->m_auxQueuesIdle = [this, renderTid]() -> bool {
        if (GetCurrentThreadId() != renderTid)
            return false;
        for (auto& sp : m_mirrorSurfaces)
            if (!sp->WorkIdle())
                return false;
        return true;
    };

    VMARK("StartMirrorThread queue+callbacks");

    m_bMirrorThreadQuit.store(false);
    for (auto& sp : SnapshotMirrorSurfaces()) {
        sp->publishedIdx.store(-1);
        sp->lastWrite = -1;
        sp->pendingWrite = -1;
        StartSurfaceWorker(sp);
        VMARK("StartMirrorThread after one worker");
    }
    VMARK("StartMirrorThread exit");
}

// Release EVERY GPU object the mirror subsystem holds (#230).
//
// For TDR recovery specifically. After a device loss, D3D12CreateDevice keeps
// returning DXGI_ERROR_DEVICE_HUNG with no adapter selectable at all for as
// long as this process still references the removed device -- measured:
// `D3D12CreateDevice failed hr=0x887A0006 adapter=NONE`, instantly, on all six
// retry attempts, which is why waiting patiently for the driver achieved
// nothing.
//
// StopMirrorThread alone is not enough. It stops the workers and drops the
// mirror queue, but each surface keeps its fence, command list and allocator,
// and every one of those holds the dead device.
void Engine::ReleaseAllMirrorGpuObjects()
{
    StopMirrorThread();
    for (auto& sp : SnapshotMirrorSurfaces()) {
        ReleaseOrientPipeline(*sp);
        ReleaseSurfaceWorkObjects(*sp);
    }
}

// NOT adding Spout / video-capture teardown here, deliberately.
//
// I assumed Spout was holding the removed device and started writing this.
// Shane pointed out it should not be initialised unless actually used, and the
// failing run's log confirmed it: ZERO Spout lines anywhere in it. It was never
// running, so releasing it would have been decoration, not a fix.
//
// If a Spout sender IS open when a device is lost it genuinely does hold the
// device and the queue through its D3D11On12 wrapper, and this is where that
// teardown belongs -- but only once a failure has actually shown it.

void Engine::StopMirrorThread()
{
    m_bMirrorThreadQuit.store(true);
    for (auto& sp : SnapshotMirrorSurfaces()) {
        StopSurfaceWorker(*sp);
        sp->publishedIdx.store(-1);
        sp->lastWrite = -1;
        sp->pendingWrite = -1;
        // Sim context: states + per-context EEL storage die with the thread.
        MirrorSimFree(sp->sim);
    }
    // Retire the waiter with the queue it waits on. Leaving it installed would
    // have WaitForGpu walk a surface list this teardown is dismantling (#206).
    if (m_lpDX) {
        m_lpDX->m_waitForAuxQueues = nullptr;
        // Retired with the waiter, for the same reason (#214): left installed
        // it would walk a surface list this teardown is dismantling. With it
        // gone the main-queue fence is the whole test again, which is correct
        // once there are no mirror queues.
        m_lpDX->m_auxQueuesIdle = nullptr;
    }
    m_mirrorQueue.Reset();
}

void Engine::MirrorThreadMain(const std::shared_ptr<MirrorSurface>& surf)
{
    MirrorSurface& s = *surf;
    {
        char buf[160];
        sprintf(buf, "MirrorThreadMain: enter (%ls)\n",
                s.device.empty() ? L"(none)" : s.device.c_str());
        DebugLogA(buf, LOG_INFO);
    }
    while (!m_bMirrorThreadQuit.load() && !s.quit.load()) {
        // 2 ms is the FALLBACK, not the pacer. The primary signals hWake
        // directly once per frame, right after publishing the parity token
        // (see m_nPrimaryFrameSeq's advance), so in normal running this wait
        // returns on the event and the timeout never expires.
        //
        // It used to say "nothing signals the wake event, so this timeout IS
        // the worker's loop pacer", which was true when it was written and
        // stopped being true when the direct wake was added. Left as it was it
        // sends the next person measuring the worker's rate after a 2 ms poll
        // that is not what limits it.
        //
        // The timeout still matters for the cases the event does not cover:
        // mirrors active with the primary stalled, and the first iteration
        // before any token has been published.
        WaitForSingleObject(s.hWake, 2);
        if (m_bMirrorThreadQuit.load() || s.quit.load())
            break;
        if (!m_bMirrorsActive)
            continue;
        try {
            MirrorThreadDrawAndPresent(s);
        } catch (...) {
            DebugLogA("MirrorThreadMain: exception - skip frame\n", LOG_ERROR);
        }
    }
    DebugLogA("MirrorThreadMain: exit\n", LOG_INFO);
}

void Engine::MirrorThreadDrawAndPresent(MirrorSurface& surf)
{
    // ONE surface, on its own thread (#186 phase 5c). What used to be a loop
    // over every surface inside one worker is now this function running N
    // times concurrently, which is the whole change: the simulation below is
    // ctx-pure and lock-free, so N of them genuinely overlap. The RECORD still
    // takes m_mirrorEngineMutex exclusively -- see the note on MirrorSurface
    // for why that is the right split and not a shortcut.
    //
    // Quit checks: StopSurfaceWorker can run on the render thread WHILE that
    // thread holds the engine mutex - its join times out if we are blocked on
    // that mutex, and it then frees the sim context and D3D objects. Every
    // resume point below must bail on quit before touching anything.
    if (m_bMirrorThreadQuit.load() || surf.quit.load()) {
        surf.lastGate.store(1); return;
    }
    if (!m_lpDX || !m_lpDX->m_device || !m_mirrorQueue) {
        surf.lastGate.store(2); return;
    }

    // Nothing to draw for? Then do not simulate.
    //
    // Each worker runs a FULL independent MilkDrop simulation -- its own CState,
    // its own 50 EEL VMs, per-vertex EEL over the same warp grid as the primary
    // -- and it had no test for whether any consumer existed. When every mirror
    // target is taken over by a -child, SendToDisplayOutputs correctly stops
    // copying the source and the mirror swap chains stop presenting, but this
    // kept running flat out into a surface nobody read.
    //
    // Measured on a 3-display setup with both mirror targets child-owned:
    // orientFrames advanced 1283 in five seconds (~256fps) while per-monitor
    // draws advanced 0, having accumulated 165,000 discarded frames. At
    // sim 1.8ms + lock 1.5ms + record 0.2ms per frame that is most of a core,
    // and it took m_mirrorEngineMutex ~256 times a second in contention with
    // the render thread -- which is the more expensive half, because the
    // primary blocks on that same mutex.
    //
    // Safe against the handover it might look like it breaks: ChildOwnsDisplay
    // is gated on the child being READY, not merely requested, so the mirror is
    // held until there is something to hand the display over to. And the sim's
    // animation clock is the primary's (mirror_sim.cpp), so a mirror that
    // starts again after a child is retired resumes at the correct time rather
    // than replaying stale simulation.
    if (!m_bAnyMirrorTargetLive.load(std::memory_order_acquire)) {
        surf.lastGate.store(3); return;
    }

    // Jerkiness instrumentation. thread_local since 5c: each worker measures
    // ITS OWN surface, which is what these numbers were always trying to
    // describe. The published values are therefore one surface's -- whichever
    // closed its window last -- and no longer a total over all of them.
    static LONGLONG s_qpf = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
    }();
    struct DiagWin {
        LONGLONG lastDone = 0, winStart = 0;
        int n = 0, fence = 0;
        LONGLONG dtSum = 0, lockSum = 0, recSum = 0, simSum = 0, fenceWaitSum = 0;
        int dtMin = INT_MAX, dtMax = 0, lockMax = 0, recMax = 0, simMax = 0;
    };
    thread_local DiagWin s_dw;
    auto usSince = [&](LONGLONG a, LONGLONG b) {
        return (int)((b - a) * 1000000 / s_qpf);
    };

    // This surface's own previous frame must be off the GPU. Wait briefly
    // rather than returning: a hard return cost a full primary frame per miss
    // and put backgrounded mirrors at 11 fps. Nothing here waits on ANOTHER
    // surface any more -- which is precisely what 5b's per-surface fences were
    // for, and what lets these threads run independently.
    if (!surf.WorkIdle()) {
        s_dw.fence++;
        bool waited = false;
        // How LONG, not just how often (#189). The pipeline is one frame deep
        // here -- this surface must see its own previous submission complete
        // before recording the next -- so if the worker's rate is bounded by
        // the GPU rather than by the engine lock, the evidence is the time
        // spent on this line. A count cannot tell those apart.
        LARGE_INTEGER fA, fB;
        QueryPerformanceCounter(&fA);
        if (surf.workFence && surf.fenceEvt &&
            SUCCEEDED(surf.workFence->SetEventOnCompletion(surf.workSubmitted,
                                                           surf.fenceEvt)))
            waited = WaitForSingleObject(surf.fenceEvt, 20) == WAIT_OBJECT_0;
        QueryPerformanceCounter(&fB);
        s_dw.fenceWaitSum += usSince(fA.QuadPart, fB.QuadPart);
        if (!waited) {
            surf.lastGate.store(4); return;
        }
    }

    const int needW = surf.needW.load(), needH = surf.needH.load();
    if (needW <= 0 || needH <= 0 || !surf.pipe.ready) {
        surf.lastGate.store(5); return;
    }

    // -- Pacing ---------------------------------------------------------------
    // The orient pipeline integrates its feedback once per orient frame, so the
    // worker's rate IS the mirror's animation SPEED, not just its smoothness.
    // Free-running it (the old parity mode) only looked right while engine-lock
    // contention happened to throttle the worker to the primary's rate: an
    // fps-capped primary sleeps between frames and releases the lock, so the
    // worker hit 160 fps against a 40 fps primary and its warp/zoom advanced
    // four times per primary frame - mirrors visibly faster than the window
    // they mirror (Shane, 2026-08-23, "martin - Gin Tonic on Ice 004.milk2").
    //
    //   maxFps == 0 (parity, default): one sim frame per PRIMARY frame. Same
    //     integration rate as the primary is what makes the motion match.
    //   maxFps  > 0: explicit ceiling instead (Displays dropdown /
    //     SET_MIRROR_MAXFPS). Set it high for the old unbounded free-run.
    //
    // Both gates run BEFORE the sim step: the cap used to step the ctx EEL and
    // then discard the frame, advancing the simulation with no render behind it.
    //
    // Per SURFACE since 5c. These were engine members when one worker paced
    // every surface; shared, whichever thread ran first would satisfy the gate
    // on behalf of all of them and the rest would starve.
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);

    // -- Closed-loop back-pressure (#230) -------------------------------------
    // The fence gate above has already proved this surface's previous frame
    // complete, so here is the one place that knows what the GPU actually
    // charged for it. Everything below picks a rate; this decides whether the
    // hardware has earned another frame yet.
    // Measure ALWAYS, gate only when enabled. Turning the pacer off used to
    // stop the measurement too, which reported workUs=0 for exactly the runs
    // where the cost is the thing being investigated.
    // The fence gate above proved the previous frame complete, so the resolve
    // that wrote these counters has landed and the readback is safe to map.
    if (surf.statsPending) {
        surf.statsPending = false;
        void* p = nullptr;
        D3D12_RANGE rr = { 0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) };
        if (surf.statReadback &&
            SUCCEEDED(surf.statReadback->Map(0, &rr, &p)) && p) {
            surf.lastStats = *(const D3D12_QUERY_DATA_PIPELINE_STATISTICS*)p;
            D3D12_RANGE none = { 0, 0 };
            surf.statReadback->Unmap(0, &none);
        }
        if (surf.timeReadback && surf.timeFreq > 0) {
            void* tp = nullptr;
            D3D12_RANGE tr = { 0, sizeof(UINT64) * 2 };
            if (SUCCEEDED(surf.timeReadback->Map(0, &tr, &tp)) && tp) {
                const UINT64* ts = (const UINT64*)tp;
                if (ts[1] > ts[0]) {
                    surf.lastGpuUs = (unsigned)((ts[1] - ts[0]) * 1000000ull /
                                                surf.timeFreq);
                    // Publish the worst any surface has seen for this preset.
                    // This is what decides whether the extra displays belong
                    // in their own processes -- a measurement, where the old
                    // trigger read the shader text and flagged one preset in
                    // seven.
                    unsigned peak = m_nMirrorPeakGpuUs.load(
                        std::memory_order_relaxed);
                    while (surf.lastGpuUs > peak &&
                           !m_nMirrorPeakGpuUs.compare_exchange_weak(
                               peak, surf.lastGpuUs, std::memory_order_relaxed))
                        ;
                }
                D3D12_RANGE none = { 0, 0 };
                surf.timeReadback->Unmap(0, &none);
            }
        }
    }

    if (surf.workSubmitQpc != 0) {
        surf.lastWorkUs = (unsigned)usSince(surf.workSubmitQpc, qpcNow.QuadPart);
        surf.workSubmitQpc = 0;
        // Pace on gpuUs -- what the HARDWARE spent on this surface's frame --
        // not on workUs, which is wall time from submit to fence and therefore
        // includes however long the work sat in the queue.
        //
        // That distinction is the whole reason earlier attempts failed. workUs
        // is a lagging measure of a problem it is also a symptom of: while the
        // GPU keeps up it reads low, and by the time it reads high the queue
        // has already grown past saving. Measured with two independent mirrors
        // at 1920x1088 and the pacer off:
        //
        //                    psInv    overdraw   workUs early   GPU time
        //   Saturday smoke0  4.18M      2.00      7.7-12.4 ms   2.0-3.6 ms
        //   Clouds1b         4.19M      2.00      1.2 ms        26-27 ms
        //
        // Before it explodes the FATAL preset measures CHEAPER in workUs than
        // the healthy one, so no threshold on workUs can separate them -- and
        // none did. Two floors were tried and both failed, in opposite
        // directions: 8 ms caught Clouds1b but cost smoke0 36,795 skipped
        // frames and pushed it from 22.2 ms to 33.8 ms for nothing, while
        // 30 ms left smoke0 alone but engaged far too late and Clouds1b died
        // at 0 s. A ramp detector on workUs failed too: the jump is 1 ms to
        // 3.2 SECONDS in a single frame, so there is no ramp to catch.
        //
        // gpuUs separates them on the first completed frame and keeps
        // separating them: 26 ms against 2-3 ms, an 8x gap that is a property
        // of the shader rather than of the contention it causes. Identical
        // fragment counts and identical overdraw (2.00) on both, so this is
        // genuinely cost per fragment and not geometry.
        //
        // The rule needs no threshold at all. A surface that spent G on the
        // GPU waits G * liveSurfaces / share before submitting again, so
        // aggregate mirror demand converges on `share` of the hardware
        // whatever the preset costs. When the hold is shorter than the rate
        // the gate below would pick anyway, nothing is skipped and the pacer
        // is invisible -- which is what happens for every cheap preset:
        //
        //   smoke0    3 ms * 2 / 0.5 =  12 ms hold, under a 45 fps parity
        //             period of 22 ms -> never throttled
        //   Clouds1b 27 ms * 2 / 0.5 = 108 ms hold -> ~9 fps per mirror,
        //             about half the GPU for the pair, leaving the rest for
        //             the primary the user is actually watching
        //
        // Until the first frame completes there is no measurement, and an
        // unmeasured surface is not held back.
        const int live = m_nLiveMirrorSurfaces.load(std::memory_order_relaxed);
        const double share = 0.5;            // mirrors may have half the GPU
        if (surf.lastGpuUs == 0) {
            surf.nextSubmitQpc = 0;
        } else {
            const double holdUs = (double)surf.lastGpuUs *
                                  (double)(live > 0 ? live : 1) / share;
            surf.nextSubmitQpc = qpcNow.QuadPart +
                                 (LONGLONG)(holdUs * (double)s_qpf / 1000000.0);
        }
    }
    if (!m_bMirrorPacer.load(std::memory_order_relaxed))
        surf.nextSubmitQpc = 0;

    // The gate sits BEFORE the rate gates, and the counter is qualified.
    //
    // Both halves are measured, not preference. Returning here leaves the
    // parity token unconsumed, so a hold is absorbed by the poll loop and no
    // frame is lost: smoke0 holds orientDt at 22.2 ms, exact parity with a
    // 45 fps primary. Moving the gate after the rate gates was tried and is
    // worse -- parity consumes the token first, so declining then DROPS the
    // frame and smoke0 fell to 28.4 ms, about 35 fps.
    //
    // But the worker wakes every ~2 ms, so counting every declined wake made
    // smoke0 report 24,885 skipFrames across 50 seconds while losing nothing
    // at all. That is the same kind of lie this counter told when nothing
    // incremented it, and would be read the same wrong way. So it only counts
    // when a primary frame was actually waiting to be rendered -- a deferred
    // frame -- rather than every poll spent inside the hold window.
    if (surf.nextSubmitQpc != 0 && qpcNow.QuadPart < surf.nextSubmitQpc) {
        if (m_nMirrorMaxFps.load(std::memory_order_relaxed) > 0 ||
            m_nPrimaryFrameSeq.load(std::memory_order_acquire) !=
                surf.seenPrimaryFrame)
            m_mirrorDiagSkipFrames.fetch_add(1, std::memory_order_relaxed);
        surf.lastGate.store(11);   // throttled: GPU has not earned this frame
        return;
    }

    const int maxFps = m_nMirrorMaxFps.load(std::memory_order_relaxed);
    if (maxFps > 0) {
        // -1.5 ms so the 2 ms wake quantization lands ON the target period
        // instead of one poll past it (a 60 fps cap otherwise runs ~53).
        const double minMs = 1000.0 / (double)maxFps - 1.5;
        if (surf.lastOrientQpc != 0 &&
            (double)(qpcNow.QuadPart - surf.lastOrientQpc) * 1000.0 / (double)s_qpf < minMs) {
            surf.lastGate.store(6); return;
        }
    } else {
        const unsigned pf = m_nPrimaryFrameSeq.load(std::memory_order_acquire);
        if (pf == surf.seenPrimaryFrame)
            return;   // primary has not advanced. NOT recorded as a gate: the
                      // worker polls at 2ms against a ~60fps primary, so this
                      // is the overwhelmingly common return and recording it
                      // buries whatever the interesting frames did.
        // Everything between the last frame we serviced and this one is a
        // primary frame this surface will never render (#189). Stepping once
        // is right -- a missed frame cannot be rendered late -- but the count
        // is the whole story of whether parity is being held, so it is kept
        // rather than thrown away with the assignment below.
        if (surf.seenPrimaryFrame != 0) {
            const unsigned skipped = pf - surf.seenPrimaryFrame - 1;
            if (skipped)
                surf.parityMissed.fetch_add(skipped, std::memory_order_relaxed);
        }
        surf.seenPrimaryFrame = pf;
    }


    // Independent simulation: adopt the latest preset bundle and advance the
    // context OUTSIDE the engine lock - ctx-pure work (own states/VMs/mesh),
    // imports serialize on m_presetLoadMutex only. Never nest those locks
    // (see engine.h at m_presetLoadMutex).
    //
    // THIS is the part 5c parallelises, and the measurement that chose it:
    // two surfaces free-running spend sim 1.6 ms, record 0.4 ms and lock-wait
    // 0.2 ms of a 3.8 ms iteration.
    int simUs = 0;
    // THIS surface's mode, not the primary's (#351). While this asked the
    // global, the primary loading any .milk3 skipped the whole block for every
    // own-preset display -- no grid, no preset drain, no adopt, no sim step --
    // so the display silently stopped running its own preset.
    // UNCONDITIONAL, whatever pipeline this surface renders with (#373).
    //
    // These three were inside the mode guard, and that made the guard
    // self-latching for the second time in this file: a surface in milk3 mode
    // skipped DrainSurfacePreset and MirrorSimAdoptPreset, which are the only
    // things that could ever take it back OUT of milk3 mode. Handing such a
    // display a .milk left it stuck on its old Shadertoy preset forever.
    //
    // MirrorSimEnsureGrid belongs here for a second reason: it is what carries
    // the PANEL's aspect into the sim grid. Skipped, the grid keeps whatever
    // shape it was last given, which is what "the blue haze on display 1 is
    // squished" looks like -- a classic preset drawn on a grid built for
    // something else.
    {
        const int gw = surf.pipe.ready ? surf.pipe.w : needW;
        const int gh = surf.pipe.ready ? surf.pipe.h : needH;
        MirrorSimEnsureGrid(surf.sim, gw, gh,
                            surf.panelW.load(), surf.panelH.load());
        // Take any preset the render or IPC thread asked for. The worker owns
        // sim.ownPresetPath; nothing else may assign to it.
        DrainSurfacePreset(surf);
        // No bundle yet (startup): keep the last presented image.
        if (!MirrorSimAdoptPreset(surf.sim))
            return;
    }

    // The per-frame SIMULATION is the part a Shadertoy preset does not use --
    // it has no per-frame equations and does not draw the warp mesh. Its CLOCK
    // still has to advance, though, because c.fTime and c.nFrame are what
    // become iTime and iFrame, so the step runs either way and the milk3 cost
    // is an unused mesh rather than a frozen preset.
    {
        LARGE_INTEGER simA, simB;
        QueryPerformanceCounter(&simA);
        MirrorSimStepFrame(surf.sim);
        QueryPerformanceCounter(&simB);
        simUs = usSince(simA.QuadPart, simB.QuadPart);
    }

    // Block on the lock, with a timeout that exists ONLY to re-check the quit
    // flag. An untimed lock could park us here across StopSurfaceWorker (which
    // runs on the render thread WHILE it holds this mutex): its join timed out
    // and it freed everything under us (0xC0000409 on mirror disable,
    // 2026-08-21). The predecessor of this went the other way and used
    // try_lock, which never enqueues -- see the note on m_mirrorEngineMutex for
    // why that starved the worker and what the yield it needed then cost.
    LARGE_INTEGER lockA;
    QueryPerformanceCounter(&lockA);
    std::unique_lock<std::timed_mutex> lk(m_mirrorEngineMutex, std::defer_lock);
    while (!lk.try_lock_for(std::chrono::milliseconds(50))) {
        if (m_bMirrorThreadQuit.load() || surf.quit.load())
            return;
    }
    LARGE_INTEGER lockB;
    QueryPerformanceCounter(&lockB);
    const int lockUs = usSince(lockA.QuadPart, lockB.QuadPart);
    // Re-check after acquisition: a Stop may have freed members while we spun.
    if (m_bMirrorThreadQuit.load() || surf.quit.load())
        return;
    surf.lastOrientQpc = qpcNow.QuadPart;

    // The fence gate above proved last iteration's orient frame complete on
    // the GPU - publish its disp[] index for the render thread's blit and
    // advance the triple rotation. All panel blits/presents stay on the
    // render thread (worker-side main-queue submits deadlocked D3D12Core's
    // queue mutex; mirror-queue writes to flip buffers TDR'd - 2026-08-21).
    if (surf.lastWrite >= 0 && surf.WorkIdle()) {
        surf.publishedIdx.store(surf.lastWrite, std::memory_order_release);
        // Slot first, then the counter: a reader that sees the new count is
        // guaranteed to read at least this slot (and a slot one newer is just
        // as valid - the triple rotation keeps both fence-complete).
        surf.publishedSeq.fetch_add(1, std::memory_order_release);
        surf.pipe.dispWrite = (surf.lastWrite + 1) % 3;
    }

    m_lpDX->BeginAuxUpload();
    // This surface records into ITS OWN list and signals ITS OWN fence
    // (#186 phase 5b) -- which is what makes a thread per surface possible at
    // all: two threads cannot record into one list, and one shared fence
    // cannot say whose frame is done.
    surf.stepAttempts.fetch_add(1);
    surf.pendingWrite = -1;
    bool recorded = false;
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    if (!surf.workList || !surf.workAlloc || !surf.workFence)
        surf.lastGate.store(8);   // work objects missing (#197 instrumentation)
    if (surf.workList && surf.workAlloc && surf.workFence &&
        SUCCEEDED(surf.workAlloc->Reset()) &&
        SUCCEEDED(surf.workList->Reset(surf.workAlloc.Get(), nullptr))) {
        ID3D12GraphicsCommandList* cmd = surf.workList.Get();
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

        // Count what this frame actually asks the GPU for, around the WHOLE
        // orient pipeline -- every warp, wave, shape and blit this surface
        // issues -- so PSInvocations can be compared against the surface's
        // pixel count and say whether the fragments are the problem.
        const bool counting = (surf.statHeap && surf.statReadback);
        // The tick rate belongs to the QUEUE the work runs on, not the device,
        // and m_mirrorQueue does not exist yet when a surface's work objects
        // are first created -- surfaces come before mirrors are activated.
        // Asking only at creation left timeFreq at 0 forever, and since timing
        // requires a non-zero frequency the timestamps silently never
        // recorded: every gpuUs read 0, including on frames that provably
        // shaded 4.18M fragments. Asked here instead, where the queue is
        // necessarily live, and only until it answers.
        if (surf.timeHeap && surf.timeFreq == 0 && m_mirrorQueue &&
            FAILED(m_mirrorQueue->GetTimestampFrequency(&surf.timeFreq)))
            surf.timeFreq = 0;
        const bool timing = (surf.timeHeap && surf.timeReadback &&
                             surf.timeFreq > 0);
        if (counting)
            cmd->BeginQuery(surf.statHeap.Get(),
                            D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        if (timing)
            cmd->EndQuery(surf.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        // THIS surface's mode (#351). Asking the global here is what put the
        // primary's Shadertoy on a display that had a preset of its own.
        const bool ok = SurfaceUsesMilk3(surf)
            ? RenderMilk3OrientPipeline(surf, cmd, needW, needH)
            : RenderClassicOrientPipeline(surf, cmd, needW, needH);

        if (counting) {
            cmd->EndQuery(surf.statHeap.Get(),
                          D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            cmd->ResolveQueryData(surf.statHeap.Get(),
                                  D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
                                  surf.statReadback.Get(), 0);
        }
        if (timing) {
            cmd->EndQuery(surf.timeHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            cmd->ResolveQueryData(surf.timeHeap.Get(),
                                  D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                  surf.timeReadback.Get(), 0);
        }
        if (FAILED(cmd->Close()) || !ok) {
            surf.lastGate.store(ok ? 9 : 10);  // 9 = Close failed, 10 = pipeline said no
            surf.imageReady.store(false);
        } else {
            ID3D12CommandList* lists[] = { cmd };
            m_mirrorQueue->ExecuteCommandLists(1, lists);
            surf.workSubmitted++;
            m_mirrorQueue->Signal(surf.workFence.Get(), surf.workSubmitted);
            // Stamp for the closed-loop pacer (#230). Read once the fence
            // proves this frame done; the gap IS what the GPU charged, queue
            // wait included, which is the number the watchdog cares about.
            {
                LARGE_INTEGER subQpc;
                QueryPerformanceCounter(&subQpc);
                surf.workSubmitQpc = subQpc.QuadPart;
            }
            // Either query pending is enough to schedule the readback: gpuUs
            // now drives the pacer, so a failed statistics heap must not be
            // able to switch pacing off as a side effect.
            if (counting || timing)
                surf.statsPending = true;
            // Which disp[] face this frame wrote; published at the NEXT
            // iteration, once this surface's own fence proves it done.
            surf.pendingWrite = surf.pipe.dispWrite % 3;
            surf.lastWrite = surf.pendingWrite;
            if (surf.pipe.frames >= 2)
                surf.imageReady.store(true);
            surf.lastGate.store(0);   // recorded and submitted
            surf.stepRecords.fetch_add(1);
            recorded = true;
        }
    }
    m_lpDX->EndAuxUpload();

    if (!recorded)
        return;
    m_nOrientFrameAccum.fetch_add(1, std::memory_order_relaxed);

    // -- Window accounting: dt = interval between completed worker frames --
    LARGE_INTEGER doneQpc;
    QueryPerformanceCounter(&doneQpc);
    const int recUs = usSince(lockB.QuadPart, doneQpc.QuadPart);
    if (s_dw.lastDone != 0) {
        const int dtUs = usSince(s_dw.lastDone, doneQpc.QuadPart);
        s_dw.n++;
        s_dw.dtSum += dtUs;
        if (dtUs < s_dw.dtMin) s_dw.dtMin = dtUs;
        if (dtUs > s_dw.dtMax) s_dw.dtMax = dtUs;
        s_dw.lockSum += lockUs;
        if (lockUs > s_dw.lockMax) s_dw.lockMax = lockUs;
        s_dw.recSum += recUs;
        if (recUs > s_dw.recMax) s_dw.recMax = recUs;
        s_dw.simSum += simUs;
        if (simUs > s_dw.simMax) s_dw.simMax = simUs;
    }
    s_dw.lastDone = doneQpc.QuadPart;
    if (s_dw.winStart == 0)
        s_dw.winStart = doneQpc.QuadPart;
    if (usSince(s_dw.winStart, doneQpc.QuadPart) >= 1000000) {
        const int n = s_dw.n > 0 ? s_dw.n : 1;
        m_diagOrientDtMinUs.store(s_dw.dtMin == INT_MAX ? 0 : s_dw.dtMin, std::memory_order_relaxed);
        m_diagOrientDtMaxUs.store(s_dw.dtMax, std::memory_order_relaxed);
        m_diagOrientDtAvgUs.store((int)(s_dw.dtSum / n), std::memory_order_relaxed);
        m_diagOrientLockMaxUs.store(s_dw.lockMax, std::memory_order_relaxed);
        m_diagOrientLockAvgUs.store((int)(s_dw.lockSum / n), std::memory_order_relaxed);
        m_diagOrientRecMaxUs.store(s_dw.recMax, std::memory_order_relaxed);
        m_diagOrientRecAvgUs.store((int)(s_dw.recSum / n), std::memory_order_relaxed);
        m_diagOrientSimMaxUs.store(s_dw.simMax, std::memory_order_relaxed);
        m_diagOrientSimAvgUs.store((int)(s_dw.simSum / n), std::memory_order_relaxed);
        m_diagOrientFenceWaits.store(s_dw.fence, std::memory_order_relaxed);
        m_diagOrientFenceMsAvg.store(
            (int)(s_dw.fenceWaitSum / (s_dw.n > 0 ? s_dw.n : 1)),
            std::memory_order_relaxed);
        s_dw = DiagWin{};
        s_dw.lastDone = doneQpc.QuadPart;
        s_dw.winStart = doneQpc.QuadPart;
    }
}

void Engine::CopyPrimaryToMirrorSrc()
{
    m_bMirrorSrcCopiedThisFrame = false;
    if (!m_bMirrorsActive || !m_lpDX || !m_lpDX->m_commandList || !m_lpDX->m_device)
        return;

    bool anyMirror = false;
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.config.bEnabled)
            continue;
        if (out.bSkippedSameMonitor)
            continue;
        if (out.monitorState && out.monitorState->bReady && !out.monitorState->bSoftDisabled)
            anyMirror = true;
        else if (!out.monitorState)
            anyMirror = true; // will Init this frame
    }
    if (!anyMirror)
        return;

    auto* cmd = m_lpDX->m_commandList.Get();
    ID3D12Resource* bb = m_lpDX->m_renderTargets[m_lpDX->m_frameIndex].Get();
    if (!cmd || !bb)
        return;

    // CopyResource requires exact size match. Client size can disagree with the
    // flip BB for a frame after fullscreen (that mismatch TDRs the GPU).
    const D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
    const UINT bw = (UINT)bbDesc.Width;
    const UINT bh = bbDesc.Height;
    if (bw == 0 || bh == 0)
        return;

    static UINT s_mirrorSrcEpoch = UINT_MAX;
    // m_bMirrorSrcRecreateNextFrame carries a rebuild asked for by the
    // primary-geom handler, which cannot free the texture itself: it runs
    // after this function in the same frame, with a copy into this very
    // resource already recorded (#195). A geometry change that leaves the
    // size alone would not trip the checks below, so it says so explicitly.
    const bool srcStale = !m_mirrorSrcTex.IsValid() ||
        m_bMirrorSrcRecreateNextFrame ||
        m_mirrorSrcTex.width != bw ||
        m_mirrorSrcTex.height != bh ||
        s_mirrorSrcEpoch != m_lpDX->m_descriptorEpoch;
    m_bMirrorSrcRecreateNextFrame = false;
    if (srcStale) {
        m_mirrorSrcTex.Reset();
        m_mirrorSrcTex = m_lpDX->CreateRenderTargetTexture(
            bw, bh, DXGI_FORMAT_R8G8B8A8_UNORM);
        s_mirrorSrcEpoch = m_lpDX->m_descriptorEpoch;
        if (!m_mirrorSrcTex.IsValid())
            return;
    }
    if (!m_mirrorSrcTex.resource ||
        m_mirrorSrcTex.width != bw || m_mirrorSrcTex.height != bh)
        return;

    D3D12_RESOURCE_BARRIER bars[2] = {};
    bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[0].Transition.pResource = bb;
    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[1].Transition.pResource = m_mirrorSrcTex.resource.Get();
    bars[1].Transition.StateBefore = m_mirrorSrcTex.currentState;
    bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (bars[1].Transition.StateBefore == bars[1].Transition.StateAfter)
        cmd->ResourceBarrier(1, &bars[0]);
    else
        cmd->ResourceBarrier(2, bars);

    cmd->CopyResource(m_mirrorSrcTex.resource.Get(), bb);
    m_mirrorSrcTex.currentState = D3D12_RESOURCE_STATE_COPY_DEST;

    bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(2, bars);
    m_mirrorSrcTex.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_bMirrorSrcCopiedThisFrame = true;
}

void Engine::ResizeMirrorSwapChain(MonitorMirrorState& ms, int newW, int newH)
{
    // Prefer soft-recreate (caller sets bSoftDisabled) over ResizeBuffers+WaitForGpu.
    // Kept for rare callers; never block the GPU for 5s here.
    if (!ms.swapChain || !m_lpDX || newW <= 0 || newH <= 0) {
        ms.bReady = false;
        return;
    }
    if (ms.width == newW && ms.height == newH && ms.bHasRtv)
        return;

    if (m_lpDX->m_device && m_lpDX->m_device->GetDeviceRemovedReason() != S_OK) {
        m_lpDX->DumpDredToLog("MirrorResize/pre");
        ms.bReady = false;
        return;
    }

    // GPU must be idle before releasing back buffers
    m_lpDX->WaitForGpu();
    if (m_lpDX->m_device && m_lpDX->m_device->GetDeviceRemovedReason() != S_OK) {
        m_lpDX->DumpDredToLog("MirrorResize/postWait");
        ms.bReady = false;
        return;
    }

    for (UINT i = 0; i < MIRROR_BUFFER_COUNT; i++)
        ms.backBuffers[i].Reset();
    ms.bHasRtv = false;

    // Must match CreateSwapChain flags (copy-mode SCs are created with Flags=0).
    UINT scFlags = 0;
    UINT nBuf = ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT;
    HRESULT hr = ms.swapChain->ResizeBuffers(
        nBuf, (UINT)newW, (UINT)newH,
        DXGI_FORMAT_R8G8B8A8_UNORM, scFlags);
    if (FAILED(hr)) {
        char logBuf[256]; sprintf(logBuf, "ResizeMirrorSwapChain: ResizeBuffers failed (0x%08X)\n", (unsigned)hr);
        DebugLogA(logBuf, LOG_ERROR);
        ms.bReady = false;
        return;
    }

    for (UINT i = 0; i < nBuf && i < MIRROR_BUFFER_COUNT; i++) {
        hr = ms.swapChain->GetBuffer(i, IID_PPV_ARGS(&ms.backBuffers[i]));
        if (FAILED(hr)) {
            ms.bReady = false;
            return;
        }
        if (ms.rtvSlotBase != UINT_MAX) {
            ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
            m_lpDX->m_device->CreateRenderTargetView(
                ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
        }
    }
    ms.bHasRtv = (ms.rtvSlotBase != UINT_MAX);
    ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
    ms.width = newW;
    ms.height = newH;
    ms.bEverPresented = false;
    ms.bNeedsFullChainClear = true;
    ms.paintedBufferMask = 0;
    for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
        ms.bbState[bi] = D3D12_RESOURCE_STATE_COMMON;
}

// ─── Per-Frame Send ───────────────────────────────────────────────────────────

// One preset from a display's directory.
//
// A plain directory walk rather than a call into the engine's preset list, for
// the same reason PickRandomPresetFile is one: m_presets belongs to the render
// thread and describes the PARENT's directory, while a display may be pointed
// at any other. Top level only, matching what a child was given.
std::wstring Engine::PickDisplayPreset(const std::wstring& dir, bool sequential,
                                       int& seqIndex) const
{
    if (dir.empty())
        return std::wstring();
    std::wstring base = dir;
    if (base.back() != L'\\' && base.back() != L'/')
        base += L'\\';

    // One pass with an explicit extension test rather than a mask per
    // extension: FindFirstFile matches 8.3 short names too, so three masks
    // would need dedup afterwards.
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return std::wstring();
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
        if (!dot)
            continue;
        // .milk3 is back (#373). It was excluded for one release because a
        // display's own preset went through CState::Import, which has no .milk3
        // branch and returns success on a default state -- so cycling onto one
        // took the display to 0.2% lit and left it there while IPC reported
        // ok=1. A surface now parses .milk3 into its own CState and renders it
        // with its own compiled Image and Buffer A-D passes, so cycling can
        // offer them again.
        if (_wcsicmp(dot, L".milk") == 0 || _wcsicmp(dot, L".milk2") == 0 ||
            _wcsicmp(dot, L".milk3") == 0)
            files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (files.empty())
        return std::wstring();

    // Sorted, so "sequential" means the same order every run and on every
    // display -- an unsorted FindFirstFile order is stable in practice but is
    // not promised, and a display that reordered itself between runs would
    // look like it had skipped presets.
    std::sort(files.begin(), files.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return _wcsicmp(a.c_str(), b.c_str()) < 0;
              });

    size_t idx;
    if (sequential) {
        seqIndex = (seqIndex < 0) ? 0 : seqIndex + 1;
        if ((size_t)seqIndex >= files.size())
            seqIndex = 0;
        idx = (size_t)seqIndex;
    } else {
        idx = (size_t)(rand() % (int)files.size());
        seqIndex = (int)idx;   // so a later switch to sequential carries on here
    }
    return base + files[idx];
}

// One surface's recording objects. Idempotent, render thread.
//
// DRED attribution names the list after the display, so a device-removed dump
// says which surface was recording rather than "MirrorWorkList".
bool Engine::EnsureSurfaceWorkObjects(MirrorSurface& surf)
{
    if (!m_lpDX || !m_lpDX->m_device)
        return false;
    if (surf.workList && surf.workAlloc && surf.workFence)
        return true;

    if (!surf.workFence &&
        FAILED(m_lpDX->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                             IID_PPV_ARGS(&surf.workFence)))) {
        DebugLogA("mirror surface: CreateFence failed\n", LOG_ERROR);
        return false;
    }
    if (!surf.workAlloc &&
        FAILED(m_lpDX->m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&surf.workAlloc)))) {
        DebugLogA("mirror surface: CreateCommandAllocator failed\n", LOG_ERROR);
        return false;
    }
    if (!surf.workList &&
        FAILED(m_lpDX->m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            surf.workAlloc.Get(), nullptr, IID_PPV_ARGS(&surf.workList)))) {
        DebugLogA("mirror surface: CreateCommandList failed\n", LOG_ERROR);
        return false;
    }
    std::wstring name = L"MirrorWork:" + (surf.device.empty() ? L"(none)" : surf.device);
    surf.workList->SetName(name.c_str());
    surf.workList->Close();
    if (!surf.fenceEvt)
        surf.fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    surf.workSubmitted = 0;

    // Pipeline statistics for this surface's frame. Failing to create them is
    // not fatal: they are a diagnostic, and a mirror that renders without
    // counters is far better than a mirror that refuses to render.
    if (!surf.statHeap) {
        D3D12_QUERY_HEAP_DESC qd = {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        qd.Count = 1;
        if (FAILED(m_lpDX->m_device->CreateQueryHeap(
                &qd, IID_PPV_ARGS(&surf.statHeap)))) {
            surf.statHeap.Reset();
            DebugLogA("mirror surface: pipeline-statistics heap unavailable\n",
                      LOG_WARN);
        }
    }
    if (surf.statHeap && !surf.statReadback) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_lpDX->m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&surf.statReadback)))) {
            surf.statReadback.Reset();
            surf.statHeap.Reset();
            DebugLogA("mirror surface: pipeline-statistics readback failed\n",
                      LOG_WARN);
        }
    }

    // GPU timestamps, to put a GPU-time figure beside workUs' wall time.
    if (!surf.timeHeap) {
        D3D12_QUERY_HEAP_DESC td = {};
        td.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        td.Count = 2;                       // one at entry, one at exit
        if (SUCCEEDED(m_lpDX->m_device->CreateQueryHeap(
                &td, IID_PPV_ARGS(&surf.timeHeap)))) {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = sizeof(UINT64) * 2;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_UNKNOWN;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(m_lpDX->m_device->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&surf.timeReadback))))
                surf.timeReadback.Reset();
        }
        if (!surf.timeReadback)
            surf.timeHeap.Reset();
    }
    return true;
}

void Engine::ReleaseSurfaceWorkObjects(MirrorSurface& surf)
{
    surf.workList.Reset();
    surf.workAlloc.Reset();
    surf.workFence.Reset();
    surf.statHeap.Reset();
    surf.statReadback.Reset();
    surf.statsPending = false;
    surf.lastStats = {};
    surf.timeHeap.Reset();
    surf.timeReadback.Reset();
    surf.timeFreq = 0;
    surf.lastGpuUs = 0;
    surf.workSubmitted = 0;
    if (surf.fenceEvt) {
        CloseHandle(surf.fenceEvt);
        surf.fenceEvt = nullptr;
    }
}

// Two live surfaces whose reserved SRV ranges intersect (#354). Always a
// defect: both fill the same descriptors every frame, so both displays draw
// whichever wrote last, and the surfaces themselves look perfectly healthy --
// right preset, right shaders, own sim, own frame counter.
//
// Counted rather than merely flagged so DIAG_MIRRORS can say how bad it is,
// and cheap enough to compute per query: this is a handful of surfaces.
int Engine::OrientBindOverlapCount() const
{
  const UINT slots = OrientBindSlots();
  const UINT epoch = m_lpDX ? m_lpDX->m_descriptorEpoch : 0;
  auto surfs = SnapshotMirrorSurfaces();
  int pairs = 0;
  for (size_t i = 0; i < surfs.size(); i++) {
    if (!surfs[i] || surfs[i]->pipe.bindBase == UINT_MAX) continue;
    if (surfs[i]->pipe.bindEpoch != epoch) continue;
    for (size_t j = i + 1; j < surfs.size(); j++) {
      if (!surfs[j] || surfs[j]->pipe.bindBase == UINT_MAX) continue;
      if (surfs[j]->pipe.bindEpoch != epoch) continue;
      const UINT a = surfs[i]->pipe.bindBase, b = surfs[j]->pipe.bindBase;
      if (a < b + slots && b < a + slots)
        pairs++;
    }
  }
  return pairs;
}

std::shared_ptr<Engine::MirrorSurface> Engine::EnsureMirrorSurface(const wchar_t* device)
{
    // ONE locked section, and deliberately not via MirrorSurfaceFor: that takes
    // the same non-recursive mutex, so calling it from in here would deadlock.
    std::lock_guard<std::mutex> lock(m_mirrorSurfacesMutex);
    for (auto& sp : m_mirrorSurfaces)
        if (_wcsicmp(sp->device.c_str(), device ? device : L"") == 0)
            return sp;
    // An unkeyed surface may already exist: OnlyMirrorSurface() materialises
    // one lazily for the diagnostics, and it is the same object this display
    // should adopt rather than a second one beside it.
    for (auto& sp : m_mirrorSurfaces) {
        if (sp->device.empty()) {
            sp->device = device ? device : L"";
            return sp;
        }
    }
    m_mirrorSurfaces.emplace_back(std::make_shared<MirrorSurface>());
    m_mirrorSurfaces.back()->device = device ? device : L"";
    // Lock-free copy of the count for the pacer (#230). MirrorSurfaceCount()
    // takes m_mirrorSurfacesMutex, and the closed-loop gate runs on every
    // surface worker on every wake -- exactly the contention the per-surface
    // threads exist to avoid.
    m_nLiveMirrorSurfaces.store((int)m_mirrorSurfaces.size(),
                                std::memory_order_relaxed);
    return m_mirrorSurfaces.back();
}

void Engine::RetireMirrorSurfacesExcept(const std::vector<std::wstring>& keep)
{
    // Detach under the lock, release outside it. WaitForGpu can block for a
    // whole frame, and holding the surfaces mutex across it would stall the
    // worker's snapshot -- which is taken every wake.
    std::vector<std::shared_ptr<MirrorSurface>> retired;
    {
        std::lock_guard<std::mutex> lock(m_mirrorSurfacesMutex);
        for (size_t i = m_mirrorSurfaces.size(); i-- > 0 && m_mirrorSurfaces.size() > 1; ) {
            auto& sp = m_mirrorSurfaces[i];
            // Never retire the last one, and never an unkeyed one: the
            // diagnostics read a surface unconditionally through
            // OnlyMirrorSurface(), so an empty list would mean an empty-check
            // at every one of those sites for no gain. An idle surface holds no
            // pipe, and therefore no VRAM.
            bool wanted = sp->device.empty();
            for (const auto& k : keep)
                if (_wcsicmp(k.c_str(), sp->device.c_str()) == 0) { wanted = true; break; }
            if (wanted)
                continue;
            // Hand the surface's SRV bind range back before dropping it,
            // otherwise those 768 descriptors are gone for the life of the
            // process and a few mirror mode switches fill a 4096-slot heap.
            if (sp->pipe.bindBase != UINT_MAX &&
                sp->pipe.bindEpoch == m_lpDX->m_descriptorEpoch)
                m_freeOrientBindBases.push_back(sp->pipe.bindBase);
            retired.push_back(sp);
            m_mirrorSurfaces.erase(m_mirrorSurfaces.begin() + i);
        }
        m_nLiveMirrorSurfaces.store((int)m_mirrorSurfaces.size(),
                                    std::memory_order_relaxed);
    }
    if (retired.empty())
        return;
    // Stop the threads FIRST, before the WaitForGpu and before anything is
    // freed. A worker mid-record would otherwise have its pipe released under
    // it -- and it is holding a shared_ptr, so the surface itself survives,
    // which makes the failure a use-after-free of the D3D objects rather than
    // an obvious null.
    for (auto& sp : retired)
        StopSurfaceWorker(*sp);
    // The shared_ptrs above keep these alive even if a worker snapshotted
    // them a moment ago; their pipes hold D3D objects the GPU may still read.
    m_lpDX->WaitForGpu();
    for (auto& sp : retired) {
        ReleaseOrientPipeline(*sp);
        ReleaseSurfaceWorkObjects(*sp);
    }
}

void Engine::SendToDisplayOutputs()
{
    if (!m_lpDX) return;

    // Free any mirror handed over by a re-enumeration before doing anything
    // else — their RTV blocks are needed by the InitDisplayOutput below.
    DrainOrphanedMirrors();

    // Mirror throughput, sampled once a second. orient is the opposite-orient
    // worker (its own thread, own rate); present is mirror swap-chain flips.
    // A large gap between them means panels are re-showing the same orient frame.
    {
        // Primary frame interval (render thread) — the jitter baseline the
        // mirror is compared against.
        static LONGLONG s_pqf = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
        }();
        static LONGLONG s_pLast = 0, s_pWinStart = 0, s_pSum = 0;
        static int s_pN = 0, s_pMin = INT_MAX, s_pMax = 0;
        // Parity pacing token for the mirror worker (see m_nPrimaryFrameSeq),
        // plus a direct wake. Left to its 2 ms poll the worker found the new
        // count anywhere inside the primary's period, so its step interval
        // wobbled 9.5-40 ms around a 25 ms mean — the right number of frames,
        // at the wrong moments. This lands it right after the primary frame,
        // while the fps limiter is sleeping and the engine lock is free.
        m_nPrimaryFrameSeq.fetch_add(1, std::memory_order_release);
        if (m_bMirrorsActive) {
            // One event per surface since 5c: every worker is waiting on its
            // own, and all of them are pacing off the count just published.
            for (auto& sp : SnapshotMirrorSurfaces())
                if (sp->hWake)
                    SetEvent(sp->hWake);
        }
        LARGE_INTEGER pNow;
        QueryPerformanceCounter(&pNow);
        if (s_pLast != 0) {
            const int dtUs = (int)((pNow.QuadPart - s_pLast) * 1000000 / s_pqf);
            s_pN++; s_pSum += dtUs;
            if (dtUs < s_pMin) s_pMin = dtUs;
            if (dtUs > s_pMax) s_pMax = dtUs;
        }
        s_pLast = pNow.QuadPart;
        if (s_pWinStart == 0) s_pWinStart = pNow.QuadPart;
        if ((pNow.QuadPart - s_pWinStart) * 1000000 / s_pqf >= 1000000) {
            const int n = s_pN > 0 ? s_pN : 1;
            m_diagPrimDtMinUs.store(s_pMin == INT_MAX ? 0 : s_pMin, std::memory_order_relaxed);
            m_diagPrimDtMaxUs.store(s_pMax, std::memory_order_relaxed);
            m_diagPrimDtAvgUs.store((int)(s_pSum / n), std::memory_order_relaxed);
            s_pSum = 0; s_pN = 0; s_pMin = INT_MAX; s_pMax = 0;
            s_pWinStart = pNow.QuadPart;
        }

        const DWORD nowMs = GetTickCount();
        if (m_dwMirrorFpsTick == 0)
            m_dwMirrorFpsTick = nowMs;
        const DWORD elapsed = nowMs - m_dwMirrorFpsTick;
        if (elapsed >= 1000) {
            const float secs = (float)elapsed / 1000.0f;
            const unsigned o = m_nOrientFrameAccum.exchange(0, std::memory_order_relaxed);
            const unsigned pr = m_nMirrorPresentAccum.exchange(0, std::memory_order_relaxed);
            // PER SURFACE, which is what this always meant. One worker used to
            // record every surface and bump the counter once, so the raw total
            // WAS the per-surface rate; with a thread per surface (#186 phase
            // 5c) each bumps it for itself, and undivided the same two mirrors
            // would read as a doubled frame rate. That exact mistake is already
            // recorded against mirrorFps in bench_children_vs_mirrors, where a
            // summed counter "proved" a 2x speedup.
            const unsigned surfaces = (unsigned)MirrorSurfaceCount();
            m_fOrientFps = (float)o / secs / (float)(surfaces > 0 ? surfaces : 1);
            m_fMirrorPresentFps = (float)pr / secs;
            m_dwMirrorFpsTick = nowMs;
            if (m_bMirrorsActive) {
                char logBuf[160];
                sprintf(logBuf, "MirrorFPS: orient=%.1f present=%.1f\n",
                        m_fOrientFps, m_fMirrorPresentFps);
                DebugLogA(logBuf, LOG_INFO);
            }
        }
    }

    // Primary geom changed (resize / Windows primary-monitor switch): never leave
    // orient SizeGuard aspect or opposite-orient warp mesh on the primary path.
    if (m_bPrimaryGeomDirty.exchange(false)) {
        ClearOutputSizeOverride();
        RestorePrimaryTexSizeFromVS();
        // Soft orient reset — keep RTs if size still matches; force warmup clear.
        for (auto& sp : m_mirrorSurfaces) {
            if (sp->pipe.ready) {
                sp->pipe.frames = 0;
                sp->pipe.fbIdx = 0;
            }
        }
        // Rebuild the primary-BB copy next frame at the new size -- a stale
        // m_mirrorSrcTex after landscape<->portrait caused wrong stretch
        // samples. Ask for the rebuild; do NOT free the texture here (#195).
        //
        // This line was m_mirrorSrcTex.Reset(), and that is a use-after-free
        // of a resource the GPU has not finished with. CopyPrimaryToMirrorSrc
        // runs EARLIER IN THE SAME FRAME (engineshell.cpp, before
        // SendToDisplayOutputs) and records CopyResource(m_mirrorSrcTex, bb)
        // into the main command list. Releasing the destination here dropped
        // the last reference before that list was ever executed, so the GPU
        // read a freed resource: DXGI_ERROR_DEVICE_HUNG 0x887A0006 with DRED
        // reporting page fault VA = 0 and COPYRESOURCE as the last completed
        // op. There is no deferred-release queue in this codebase, so the only
        // safe answer is not to free mid-frame at all.
        //
        // Nothing needs freeing anyway: the flag below makes the next frame's
        // staleness check recreate it, at the top of that frame, which is
        // where every other recreate of this texture already happens.
        m_bMirrorSrcRecreateNextFrame = true;
        m_bMirrorSrcCopiedThisFrame = false;
        m_bMirrorResetOrientNextFrame.store(true);
        m_nDeferMirrorResize.store(8);
        DebugLogA("SendToDisplayOutputs: primary geom dirty — skip mirror init/draw this frame\n", LOG_INFO);
        return;
    }

    // Finish deferred activate only on a stable-size frame (not the FS resize).
    {
        int defer = m_nDeferMirrorActivate.load();
        if (defer > 0) {
            if (m_nDeferMirrorActivate.fetch_sub(1) == 1 && !m_bMirrorsActive) {
                m_bMirrorsActive = true;
                m_bMirrorStylesDirty.store(true);
                m_bRaiseMirrorsNextFrame.store(true);
                DebugLogA("SendToDisplayOutputs: deferred mirror activate\n", LOG_INFO);
            } else {
                return;
            }
        }
    }

    // Automatic isolation used to be re-decided here, moving displays into
    // their own processes when the measured GPU cost crossed kIsolateGpuUs.
    // It is gone (#261). Measured: three independent presets in one process
    // cost +100 MB of VRAM -- 0.66% of the budget -- and 75-130 us of GPU each
    // against a 15000 us threshold, so the ordinary case never needed it; and
    // for the expensive case the answer is now rate balancing rather than a
    // second process, because Windows classifies QoS from the focused window
    // and only one process can own it.
    //
    // A display the USER puts in its own process is untouched by this: that
    // tier stays, and nothing here decides it any more.
    BalanceDisplayFrameRates();

    // Apply deferred mirror style changes (set by UI thread via m_bMirrorStylesDirty)
    if (m_bMirrorStylesDirty.exchange(false))
        ApplyMirrorWindowStyles();

    // Apply pending fullscreen/windowed layout (size may require SC resize)
    {
        bool anyLayout = false;
        for (auto& out : m_displayOutputs) {
            if (out.monitorState && out.monitorState->bPendingLayout) {
                anyLayout = true;
                break;
            }
        }
        if (anyLayout) {
            for (auto& out : m_displayOutputs) {
                if (!out.monitorState || !out.monitorState->bPendingLayout)
                    continue;
                auto& ms = *out.monitorState;
                // Clear it only once the request can actually be SERVED (#338).
                // Clearing first threw the request away for a mirror that had
                // no window or no swap chain yet -- which is precisely a mirror
                // still coming up, the case most likely to have a pending
                // layout. The z-order travels with this request, so a dropped
                // one is a display left in the wrong band with nothing queued
                // to correct it.
                if (!ms.hWnd || !ms.swapChain) continue;
                ms.bPendingLayout = false;
                const int x = ms.pendingX, y = ms.pendingY;
                const int w = ms.pendingW, h = ms.pendingH;
                if (w <= 0 || h <= 0) continue;
                // Through the shared resolver, like the style pass and the
                // creation path. This site read only the GLOBAL flag, so a
                // watermarked or keep-on-top display lost its z-order the
                // moment anything resized it -- and a resize is not rare: the
                // fullscreen checkbox, a monitor change and the layout pass all
                // arrive here.
                bool zClickThru = false, zOnTop = false;
                int zOpacity = 100;
                ResolveMirrorWindowStyle(out.config, zClickThru, zOpacity, zOnTop);
                SetWindowPos(ms.hWnd, MirrorZOrder(zOnTop),
                             x, y, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                // The WINDOW takes the layout rect. The SWAP CHAIN does not.
                //
                // This used to resize the chain to w,h -- the monitor rect --
                // which is a different quantity and skips the cap entirely.
                // MirrorSwapChainSize exists to decide it, and it ends in
                // CapMirrorSwapChainDim, whose comment says why: "Never
                // allocate a mirror SC larger than 1920 on a side. 2160x3840
                // primary plus matching mirror SCs TDRs."
                //
                // Measured on Shane's instance: two copy-mode mirrors on
                // 2560x1440 monitors reported swapsize=2560x1440, over that
                // cap, while a harness run that never took this path reported
                // the capped 1088x1920 and rendered cleanly. In copy mode the
                // chain is deliberately the PRIMARY's shape, so the window and
                // the chain differ by design and resizing one to the other's
                // size is always wrong.
                //
                // Note the enclosing test above compares the window rect
                // against ms.width/height, which is the CHAIN's size -- so in
                // copy mode it is true on essentially every pass. That makes
                // the SetWindowPos redundant rather than harmful, but it is
                // what made this resize fire repeatedly instead of once.
                int wantW = 0, wantH = 0;
                MirrorSwapChainSize(out, m_lpDX->m_client_width,
                                    m_lpDX->m_client_height, wantW, wantH);
                if (wantW > 0 && wantH > 0 &&
                    (ms.width != wantW || ms.height != wantH))
                    ResizeMirrorSwapChain(ms, wantW, wantH);
            }
        }
    }

    // Soft reset orient history when re-entering independent (do NOT free RTs —
    // CreateRenderTargetTexture RTV slots are never reclaimed; free+recreate
    // exhausted the dynamic RTV heap → permanent black until full app restart.
    if (m_bMirrorResetOrientNextFrame.exchange(false)) {
        for (auto& sp : m_mirrorSurfaces) {
            if (sp->pipe.ready) {
                sp->pipe.frames = 0;
                sp->pipe.fbIdx = 0;
            }
        }
    }

    // After mirror on (and first Init of HWNDs), bring surfaces to front once.
    const bool raiseThisFrame = m_bRaiseMirrorsNextFrame.exchange(false);

    // Force reinit: tear down session-lived monitor mirrors so SC/window style
    // changes (layered, flip model, buffer count) actually take effect.
    // Without this, InitDisplayOutput early-returns and leaves the old SC forever.
    // NOTE: Independent toggle must NOT set this flag — only opacity/layer style.
    if (m_bMirrorForceReinit.exchange(false)) {
        for (auto& out : m_displayOutputs) {
            if (out.config.type == DisplayOutputType::Monitor && out.monitorState)
                DestroyDisplayOutput(out);
        }
        ReleaseAllOrientPipelines();
        m_bRaiseMirrorsNextFrame.store(true);
        DebugLogA("SendToDisplayOutputs: force-reinit destroyed monitor mirrors for recreate\n", LOG_WARN);
    }

    // After resize / ResetDynamicDescriptors the SRV bump rewinds — letterbox block
    // must be reallocated (RTVs use a permanent reserve and only need re-CreateRTV).
    if (m_mirrorLetterboxSrvEpoch != m_lpDX->m_descriptorEpoch) {
        m_mirrorLetterboxSrvBase = UINT_MAX;
        m_mirrorLetterboxSrvEpoch = m_lpDX->m_descriptorEpoch;
    }

    int mainW = m_lpDX->m_client_width;
    int mainH = m_lpDX->m_client_height;
    UINT fi = m_lpDX->m_frameIndex;
    bool hasActiveMonitors = false;

    // Primary monitor detection (never mirror onto the render window's display)
    wchar_t renderDevice[32] = {};
    if (m_lpDX->GetHwnd()) {
        if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
            CopyTo(renderDevice, m_szWatermarkRenderDevice);
        } else {
            HMONITOR hRenderMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW rmi = { sizeof(rmi) };
            if (hRenderMon && GetMonitorInfoW(hRenderMon, &rmi))
                CopyTo(renderDevice, rmi.szDevice);
        }
    }
    const bool gotDevice = renderDevice[0] != L'\0';

    // ── Session-lived mirrors: show/hide only — never Destroy on per-monitor toggle ──
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor)
            continue;

        const bool isPrimary = gotDevice &&
            wcscmp(out.config.szDeviceName, renderDevice) == 0;
        if (isPrimary)
            out.bSkippedSameMonitor = true;
        else if (m_bMirrorsActive && out.config.bEnabled)
            out.bSkippedSameMonitor = false;

        // A display driven by its own instance owns those pixels. Mirroring
        // onto it puts the PRIMARY's frame on top of a child rendering its own
        // preset, so the display appears to ignore everything asked of it while
        // the child is in fact doing exactly as told (forgejo#38).
        //
        // Gated on the child being READY, not merely requested: spawning takes
        // seconds, and dropping the mirror at request time would blank the
        // monitor until the child appeared. The mirror is held until there is
        // something to hand the display over to, which is the "replace, not
        // remove" behaviour this wants.
        const bool childOwns = out.config.bOwnProcess &&
            ChildOwnsDisplay(out.config.szDeviceName);

        const bool wantVisible = m_bMirrorsActive && out.config.bEnabled &&
            !out.bSkippedSameMonitor && !childOwns;

        if (!out.monitorState) {
            // Create once when first needed
            if (wantVisible)
                InitDisplayOutput(out);
        }

        if (!out.monitorState || !out.monitorState->bReady)
            continue;

        auto& ms = *out.monitorState;
        if (wantVisible) {
            ms.bSoftDisabled = false;
            ms.nFramesUntilHardDestroy = 0;
            // Re-bind RTVs after any descriptor epoch change (window resize rebuilds
            // dynamic RTVs and used to overwrite bump-allocated mirror slots → SEH crash).
            if (ms.bHasRtv && ms.rtvSlotBase != UINT_MAX &&
                ms.rtvEpoch != m_lpDX->m_descriptorEpoch) {
                for (UINT i = 0; i < ms.bufferCount && i < MIRROR_BUFFER_COUNT; i++) {
                    if (!ms.backBuffers[i])
                        continue;
                    ms.rtvHandles[i] = m_lpDX->GetRtvCpuHandleAt(ms.rtvSlotBase + i);
                    m_lpDX->m_device->CreateRenderTargetView(
                        ms.backBuffers[i].Get(), nullptr, ms.rtvHandles[i]);
                }
                ms.rtvEpoch = m_lpDX->m_descriptorEpoch;
                DebugLogA("SendToDisplayOutputs: refreshed mirror RTVs after descriptor rewind\n", LOG_WARN);
            }
            if (ms.hWnd) {
                // Raise only on hide→show (monitor just enabled). Never every frame
                // (DWM thrashing floors FPS at ~30). Same TOPMOST→NOTOPMOST hop as
                // RaiseMirrorSurfaces so the surface is frontmost without sticky AOT
                // unless Always On Top is on.
                const bool wasHidden = !IsWindowVisible(ms.hWnd);
                if (wasHidden) {
                    ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
                    const UINT zflags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
                    // The hop drops back only when this display does not want
                    // to STAY up (#288). It tested the global m_bAlwaysOnTop,
                    // so a watermarked display -- or one with its own Keep on
                    // top -- was pushed back down every hide->show.
                    //
                    // Which is not a rare event: LoadDisplayProfile hides every
                    // mirror window before applying, so loading ANY profile
                    // knocked a watermarked display off top. Measured on the
                    // live instance: DISPLAY3 went from alwaysontop=1 to 0
                    // across one profile load, watermark=1 throughout.
                    bool zClickThru = false, zOnTop = false;
                    int zOpacity = 100;
                    ResolveMirrorWindowStyle(out.config, zClickThru, zOpacity, zOnTop);
                    SetWindowPos(ms.hWnd, MirrorZOrder(true), 0, 0, 0, 0, zflags);
                    if (MirrorZOrderHop() && !zOnTop)
                        SetWindowPos(ms.hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, zflags);
                }
            }
            hasActiveMonitors = true;
        } else {
            // Park the mirror: hide + skip draws. Keep SC alive for the session.
            ms.bSoftDisabled = true;
            if (ms.hWnd && IsWindowVisible(ms.hWnd))
                ShowWindow(ms.hWnd, SW_HIDE);
        }
    }

    // Independent opposite-aspect SCs are capped native; copy-mode is primary
    // size. Resize on the render thread only (UI must not WaitForGpu).
    if (m_nDeferMirrorResize.load() > 0)
        m_nDeferMirrorResize.fetch_sub(1);
    if (m_nDeferMirrorResize.load() <= 0 && m_bMirrorIndepSizeDirty.exchange(false)) {
        // m_mirrorWorkFence stood here, and phase 5b stopped signalling it
        // when each surface got its own -- so this gate had quietly become
        // "always idle" and resizes went ahead over in-flight orient work.
        if (!AllSurfacesIdle()) {
            m_bMirrorIndepSizeDirty.store(true); // a worker is still presenting
        } else {
        const int primW = m_lpDX->m_client_width;
        const int primH = m_lpDX->m_client_height;
        const bool lagIdleForResize = LagIndepFenceIdle();
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor || !out.monitorState)
                continue;
            if (!out.monitorState->bReady || !out.monitorState->swapChain)
                continue;
            int wantW = primW, wantH = primH;
            MirrorSwapChainSize(out, primW, primH, wantW, wantH);
            if (wantW > 0 && wantH > 0 &&
                (out.monitorState->width != wantW || out.monitorState->height != wantH)) {
                if (!lagIdleForResize && out.config.bIndependentRender) {
                    m_bMirrorIndepSizeDirty.store(true); // retry when lag fence is idle
                    continue;
                }
                ResizeMirrorSwapChain(*out.monitorState, wantW, wantH);
            }
        }
        }
    }

    // Mirror on: raise mirror HWNDs here (render-owned). Ask UI thread to raise primary
    // — never SetWindowPos the primary from this thread (cross-thread deadlock).
    if (raiseThisFrame) {
        RaiseMirrorSurfaces(nullptr); // mirrors only
        HWND hPrimary = m_lpDX->GetHwnd();
        if (hPrimary)
            PostMessage(hPrimary, WM_MW_RAISE_PRIMARY, 0, 0);
    }

    // Never hard-destroy monitor mirrors while the app is running — CreateSwapChain
    // thrash is what locked the PC after two toggles. Park = hide only.

    // Spout cleanup when disabled
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor &&
            !out.config.bEnabled && out.spoutState)
            DestroyDisplayOutput(out);
    }

    // Spout send
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Spout)
            continue;
        if (!out.spoutState || !out.spoutState->bReady) {
            if (!out.spoutState)
                InitDisplayOutput(out);
            if (!out.spoutState || !out.spoutState->bReady)
                continue;
        }
        out.spoutState->sender.Send(out.spoutState->wrappedBackBuffers[fi]);
    }

    // Tell the orient worker whether anything still wants its frames. It has no
    // consumer test of its own, and without one it keeps simulating into a
    // surface nobody reads -- see MirrorThreadDrawAndPresent.
    m_bAnyMirrorTargetLive.store(hasActiveMonitors, std::memory_order_release);

    if (!hasActiveMonitors) {
        // Restore default DXGI latency after a multi-mirror session (stuck high
        // latency was observed as a permanent ~30–33 FPS ceiling).
        m_lpDX->m_bSerializeWithMirrors = false;
        m_lpDX->EnsureMultiSwapChainFrameLatency(1, false);
        return;
    }

    // Stretch/copy on the *same* queue (pre-July-29 path). CopyResource when
    // the mirror SC matches the flip BB; stretch-blit otherwise. Present is
    // non-blocking. Independent re-render is not in this path.
    ID3D12Resource* flipBB = m_lpDX->m_renderTargets[fi].Get();
    if (!flipBB)
        return;
    const D3D12_RESOURCE_DESC flipDesc = flipBB->GetDesc();
    const int bbW = (int)flipDesc.Width;
    const int bbH = (int)flipDesc.Height;
    if (bbW <= 0 || bbH <= 0)
        return;

    if (!m_mirrorCmdAllocators[0]) {
        for (int i = 0; i < DXC_FRAME_COUNT; i++) {
            if (FAILED(m_lpDX->m_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_mirrorCmdAllocators[i]))))
                return;
        }
        if (FAILED(m_lpDX->m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_mirrorCmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_mirrorCmdList))))
            return;
        m_mirrorCmdList->Close();
    }

    HRESULT hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
    if (FAILED(hrAlloc)) {
        m_lpDX->WaitForGpu();
        hrAlloc = m_mirrorCmdAllocators[fi]->Reset();
    }
    if (FAILED(hrAlloc))
        return;
    HRESULT hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
    if (FAILED(hrList)) {
        m_lpDX->WaitForGpu();
        hrList = m_mirrorCmdList->Reset(m_mirrorCmdAllocators[fi].Get(), nullptr);
    }
    if (FAILED(hrList))
        return;

    ID3D12GraphicsCommandList* cmd = m_mirrorCmdList.Get();
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
    m_lpDX->BeginAuxUpload();

    // Prefer the pre-Execute snapshot (same size as flip BB). Else copy the flip BB.
    ID3D12Resource* src = flipBB;
    D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_PRESENT;
    int srcW = bbW, srcH = bbH;
    const bool haveSnap = m_bMirrorSrcCopiedThisFrame && m_mirrorSrcTex.IsValid() &&
        m_mirrorSrcTex.resource &&
        (int)m_mirrorSrcTex.width == bbW && (int)m_mirrorSrcTex.height == bbH;
    if (haveSnap) {
        src = m_mirrorSrcTex.resource.Get();
        srcState = m_mirrorSrcTex.currentState;
        srcW = (int)m_mirrorSrcTex.width;
        srcH = (int)m_mirrorSrcTex.height;
    }

    auto isPortrait = [](int w, int h) { return h > w; };
    const bool mainPortrait = isPortrait(bbW, bbH);

    // ── One surface per display that needs one (#186 phase 3) ────────────
    //
    // This loop used to pick a single WINNER -- the largest opposite-orient
    // panel -- size the one shared pipe to it, and let every other panel blit
    // that same image stretched to fit. Two consequences it is worth naming,
    // because they are what the collection fixes:
    //
    //  * a portrait and a landscape mirror alternately failed the pipe's w/h
    //    test, so EnsureOrientPipeline tore the pipe down and rebuilt it on
    //    every frame -- WaitForGpu included;
    //  * a panel of a different shape from the winner showed a stretched
    //    image, never one rendered at its own aspect.
    //
    // Each qualifying display now gets its own surface, sized to itself. They
    // all still follow the primary, so they render the same preset and the
    // picture is unchanged -- phase 4 is what gives them presets of their own.
    // #186 phase 4: a display that holds its OWN PRESET needs a surface
    // whatever its orientation. The opposite-orientation test above is about
    // whether the primary's image can simply be stretched onto the panel; a
    // display running a different preset cannot use the primary's image at any
    // orientation, so it renders for itself even when the shapes match.
    bool anyOppositeIndep = false;
    std::vector<std::wstring> wanted;
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        const bool ownPreset = DisplayHoldsOwnPreset(out.config);
        if (!out.config.bIndependentRender && !ownPreset)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool port = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                            : isPortrait(out.monitorState->width, out.monitorState->height);
        if (port == mainPortrait && !ownPreset)
            continue;
        anyOppositeIndep = true;
        wanted.push_back(out.config.szDeviceName);

        std::shared_ptr<MirrorSurface> msp = EnsureMirrorSurface(out.config.szDeviceName);
        MirrorSurface& ms = *msp;
        EnsureSurfaceWorkObjects(ms);
        // A display that starts mirroring mid-session gets its thread here --
        // StartMirrorThread only covers the surfaces that existed when mirrors
        // were switched on. Idempotent, so this is a no-op on every later pass.
        if (m_bMirrorsActive && !m_bMirrorThreadQuit.load())
            StartSurfaceWorker(msp);
        // Latch what the CONFIG says, every frame, so the worker's pipeline
        // choice never depends on state the worker has not produced yet (#351).
        // Unconditional and before the ownPreset branch below: a display that
        // has just STOPPED holding its own preset has to be told too.
        ms.holdsOwnPreset.store(ownPreset, std::memory_order_relaxed);
        // Resolve the display's preset ONCE, not every frame: <Random> picks a
        // new file each time it is asked, so re-resolving per frame would
        // reload a different preset sixty times a second. After this the
        // surface owns the answer, and cycling replaces it deliberately.
        if (ownPreset) {
            const std::wstring dir = ResolveDisplayPresetDir(out.config);
            const float interval = EffectiveChildInterval(out.config);
            const bool  locked   = EffectiveChildPresetLock(out.config);
            const double now = GetTime();

            if (ms.nextPresetAt == 0.0 && ms.sim.ownPresetPath.empty() &&
                !ms.requestPending) {
                // First preset for this display. Resolved ONCE -- <Random>
                // answers differently every time it is asked, so resolving per
                // frame would reload a new preset sixty times a second.
                const std::wstring pick =
                    ResolveDisplayStartupPreset(out.config, dir.c_str());
                if (!pick.empty()) {
                    PostSurfacePreset(ms, pick);
                    ms.nextPresetAt = (interval > 0.0f) ? now + interval : -1.0;
                    DLOG_INFO("display %ls holds its own preset: %ls",
                              out.config.szDeviceName, pick.c_str());
                }
            } else if (!locked && interval > 0.0f && ms.nextPresetAt > 0.0 &&
                       now >= ms.nextPresetAt && !ms.requestPending) {
                // Its own timer, its own order. The lock is the RESOLVED one:
                // this display's if it has pinned a value, otherwise the main
                // window's, so the global lock still stops every display.
                int idx = ms.seqIndex.load();
                const std::wstring pick =
                    PickDisplayPreset(dir, out.config.bSequentialOrder, idx);
                ms.seqIndex.store(idx);
                if (!pick.empty()) {
                    PostSurfacePreset(ms, pick);
                    ms.nextPresetAt = now + interval;
                }
            } else if (interval > 0.0f && ms.nextPresetAt < 0.0) {
                // Cycling was off when this display started and has been turned
                // on since: give it a deadline rather than waiting for a
                // restart.
                ms.nextPresetAt = now + interval;
            } else if (interval <= 0.0f && ms.nextPresetAt > 0.0) {
                ms.nextPresetAt = -1.0;    // cycling switched off
            }
        } else if (!ms.sim.ownPresetPath.empty() || ms.requestPending) {
            // Turned back into a follower: an empty request puts it back on the
            // primary's bundle. Posted, not assigned -- the worker owns the sim.
            PostSurfacePreset(ms, std::wstring());
            ms.nextPresetAt = 0.0;
            ms.seqIndex.store(-1);
            // Its message clock goes with the preset it no longer owns: a
            // follower shows the primary's messages again, so leaving a clock
            // armed here would keep spawning ones nothing draws (#255).
            ms.nextMsgAt = -1.0;
            ms.nextMsgSeq = 0;
        }

        // THIS DISPLAY'S TURN of the message loop (#255).
        //
        // Gated on ownPreset for the same reason the preset clock above is: a
        // display showing the primary's preset is showing the primary's frame,
        // messages included, and giving it a second clock would double them up.
        // A display holding its own preset is its own visual world, which is
        // the behaviour the own-PROCESS path had for free -- a child being a
        // whole engine with its own message pump.
        //
        // Same store, same order, same overrides; its own timing, its own
        // placement, its own effects, and its own maxOnScreen so one busy panel
        // cannot take every slot in the shared pool.
        if (ownPreset) {
            const int disp = DisplayNumberFromDevice(out.config.szDeviceName);
            if (disp > 0) {
                const int cap = (out.config.nMsgMaxOnScreen >= 0)
                              ? out.config.nMsgMaxOnScreen
                              : m_nMsgMaxOnScreen;
                TickMessageAutoplay(disp, ms.nextMsgAt, ms.nextMsgSeq, cap);
            }
        }
        int rawW = (lw > 0) ? lw : out.monitorState->width;
        int rawH = (lh > 0) ? lh : out.monitorState->height;
        int tw = rawW, th = rawH;
        CapMirrorSwapChainDim(tw, th);
        ms.panelW.store(rawW);
        ms.panelH.store(rawH);

        // Latch the preset's canvas limit ONLY while this surface has no pipe,
        // then clamp with it. Adopting a new limit under a live pipe changes
        // the computed size and triggers a recreate, which hangs the GPU.
        if (!ms.pipe.ready)
            ms.canvasLimit = m_nCanvasLimitApplied;
        ClampOrientCanvas(ms, tw, th);
        ms.needW.store(tw);
        ms.needH.store(th);
    }
    // Displays that stopped qualifying give their VRAM back -- about 45 MB per
    // classic surface at 1920x1088, and roughly 270 MB for a .milk3 one.
    RetireMirrorSurfacesExcept(wanted);
    const bool workerIdle = AllSurfacesIdle();
    if (anyOppositeIndep && workerIdle) {
        // ONE WaitForGpu for the whole pass, however many surfaces need a pipe.
        // Per-surface it would be N stalls in a frame, and the wait is the
        // expensive half of a recreate.
        bool anyNeedsPipe = false;
        for (auto& sp : m_mirrorSurfaces) {
            const int nw = sp->needW.load(), nh = sp->needH.load();
            if (nw <= 0 || nh <= 0)
                continue;
            // Mode is part of "need": the worker is barred from rebuilding, so
            // if this test misses a reason the pipe is unusable, nobody
            // rebuilds it and the mirrors freeze. OrientPipeReadyForMode covers
            // ready-ness and the per-mode texture set together.
            if (!OrientPipeReadyForMode(*sp) ||
                sp->pipe.w != nw || sp->pipe.h != nh ||
                sp->pipe.bindEpoch != m_lpDX->m_descriptorEpoch) {
                anyNeedsPipe = true;
                break;
            }
        }
        if (anyNeedsPipe) {
            m_lpDX->WaitForGpu();
            for (auto& sp : m_mirrorSurfaces) {
                const int nw = sp->needW.load(), nh = sp->needH.load();
                if (nw <= 0 || nh <= 0)
                    continue;
                if (!OrientPipeReadyForMode(*sp) ||
                    sp->pipe.w != nw || sp->pipe.h != nh ||
                    sp->pipe.bindEpoch != m_lpDX->m_descriptorEpoch) {
                    EnsureOrientPipeline(*sp, nw, nh);
                    sp->imageReady.store(false);
                }
            }
        }
        // The worker owns a dedicated upload ring (StartMirrorThread), so it
        // cannot stomp the render thread's offsets. Start it once an
        // opposite-orient independent panel exists; it free-runs from here and
        // is deliberately not frame-locked to the primary.
        if (!m_mirrorQueue)
            StartMirrorThread();
    }

    // Readiness is per surface now -- see the panel loop, which asks the
    // surface serving THAT display. A single flag would have let one panel's
    // warmup hold every other panel on the stretched fallback.
    //
    // No workerIdle requirement: the blit reads the PUBLISHED (double-buffered,
    // fence-proven) display face, never the one in flight. Requiring idle here
    // made the path fall back to a stretched primary whenever the worker was
    // busy — the panels alternated two different images (~20 Hz flicker).
    auto surfaceReady = [](const MirrorSurface* sp) {
        return sp && sp->imageReady.load() && sp->pipe.ready &&
               sp->pipe.frames >= 2 &&
               sp->publishedIdx.load(std::memory_order_acquire) >= 0;
    };

    if (anyOppositeIndep && m_nDeferMirrorResize.load() <= 0 && workerIdle) {
        for (auto& out : m_displayOutputs) {
            if (!out.config.bIndependentRender || out.config.type != DisplayOutputType::Monitor)
                continue;
            if (!out.monitorState || !out.monitorState->bReady || !out.monitorState->swapChain)
                continue;
            int wantW = bbW, wantH = bbH;
            MirrorSwapChainSize(out, bbW, bbH, wantW, wantH);
            if (wantW > 0 && wantH > 0 &&
                (out.monitorState->width != wantW || out.monitorState->height != wantH))
                ResizeMirrorSwapChain(*out.monitorState, wantW, wantH);
        }
    }

    bool anyCopy = false;
    bool anyBlit = false;
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool panelPort = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                                 : isPortrait(ms.width, ms.height);
        const bool oppositeIndep = out.config.bIndependentRender &&
            (panelPort != mainPortrait);
        if (oppositeIndep || ms.width != srcW || ms.height != srcH)
            anyBlit = true;
        else
            anyCopy = true;
    }

    // ONE barrier for both kinds of consumer, because src can only be in one
    // state at a time and this frame may have both (#195).
    //
    // A same-size panel takes a CopyResource off src; a different-size one is
    // blitted through a shader. These were two sequential barriers, so a frame
    // holding one of each ran them in order and left src in
    // PIXEL_SHADER_RESOURCE -- and then the copy branch below ran
    // CopyResource(mirrorBB, src) anyway, because it only ever transitions the
    // DESTINATION and trusts the source to have been prepared here.
    //
    // Copying out of a resource that is not in COPY_SOURCE is invalid, and the
    // driver's answer is to take the device: DXGI_ERROR_DEVICE_REMOVED
    // 0x887A0006, page fault VA 0, with COPYRESOURCE as DRED's last completed
    // op. Reproduced by MIRROR_WATERMARK, which enables every display at once
    // right after moving the render window to a different-shaped monitor --
    // so for a few frames one panel still matches the old primary size and
    // copies while another already differs and blits.
    //
    // COPY_SOURCE and PIXEL_SHADER_RESOURCE are both READ states, and D3D12
    // allows read states to be combined, so the frame asks for exactly the ones
    // it needs and every consumer is satisfied. Nothing downstream has to
    // change: the closing transition reads the same srcState and unwinds
    // whichever combination was used.
    D3D12_RESOURCE_STATES wantSrc = D3D12_RESOURCE_STATE_COMMON;
    if (anyCopy)
        wantSrc = (D3D12_RESOURCE_STATES)(wantSrc | D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (anyBlit)
        wantSrc = (D3D12_RESOURCE_STATES)(wantSrc | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if ((anyCopy || anyBlit) && srcState != wantSrc) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.StateBefore = srcState;
        b.Transition.StateAfter = wantSrc;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        srcState = wantSrc;
    }

    // Whether messages and HUD were held OUT of the snapshot this frame.
    //
    // The decision is made once for the whole set: RenderFrame skips drawing
    // supertexts into the backbuffer when ANY mirror is independent, so each
    // panel can draw them at its own size instead of receiving them stretched.
    // Redrawing them, though, used to be keyed to the panel's OWN
    // bIndependentRender -- so a copy/stretch mirror standing next to an
    // independent one got a snapshot the messages had been taken out of and
    // nothing to put them back. That is how a mirror went silent while the
    // render window kept its text, and it needs only ONE independent entry to
    // happen: in the reported case DISPLAY2, the render window's own monitor,
    // which is skipped as a mirror and so was not even drawing.
    //
    // The flag that removes the overlays has to be the flag that restores them.
    const bool overlaysDeferred = AnyIndependentMirrorEnabled();

    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        const bool wantOverlays = out.config.bIndependentRender || overlaysDeferred;
        UINT mirrorFI = ms.swapChain->GetCurrentBackBufferIndex();
        if (mirrorFI >= ms.bufferCount || mirrorFI >= MIRROR_BUFFER_COUNT ||
            !ms.backBuffers[mirrorFI])
            continue;
        ID3D12Resource* mirrorBB = ms.backBuffers[mirrorFI].Get();
        const D3D12_RESOURCE_STATES before = ms.bEverPresented
            ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;

        int lx = 0, ly = 0, lw = 0, lh = 0;
        ComputeMirrorLayout(out.config, lx, ly, lw, lh);
        const bool panelPort = (lw > 0 && lh > 0) ? isPortrait(lw, lh)
                                                 : isPortrait(ms.width, ms.height);
        // A display holding its own preset renders for itself whatever its
        // shape: the primary's image is the WRONG PRESET, so stretching it is
        // not an option the way it is for a same-orient follower (#186 ph 4).
        const bool ownPreset = DisplayHoldsOwnPreset(out.config);
        const bool oppositeIndep = (out.config.bIndependentRender &&
            (panelPort != mainPortrait)) || ownPreset;
        // THIS display's surface, not "the" surface: each one renders at its
        // own size into its own pipe.
        MirrorSurface* psurf = oppositeIndep
            ? MirrorSurfaceFor(out.config.szDeviceName) : nullptr;
        const bool useOrient = oppositeIndep && surfaceReady(psurf);
        // oppIndep= in DIAG_MIRRORS was declared, reported, and never written,
        // so it read 0 forever -- including on a panel that WAS drawing from
        // its own orient surface. That is worse than absent: 0 reads as "this
        // panel is not using its own surface", which is exactly the wrong
        // hypothesis to hand someone chasing #354.
        ms.lastOppositeIndep = oppositeIndep;
        ms.bFreshDraw = true;
        // Present gating: no new sim frame published since this panel's last
        // draw → keep the presented image (skip draw AND present). Re-blitting
        // and re-presenting identical content every render frame was ~2x the
        // sim rate in pure waste.
        if (useOrient && ms.bEverPresented &&
            ms.lastPubSeq == psurf->publishedSeq.load(std::memory_order_acquire)) {
            ms.bFreshDraw = false;
            continue;
        }
        // Never 1:1-copy a portrait primary into a landscape SC (or vice versa).
        const bool doCopy = !oppositeIndep && (ms.width == srcW && ms.height == srcH);
        D3D12_RESOURCE_BARRIER mb = {};
        mb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        mb.Transition.pResource = mirrorBB;
        mb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        if (doCopy) {
            if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
                mb.Transition.StateBefore = before;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                cmd->ResourceBarrier(1, &mb);
            }
            cmd->CopyResource(mirrorBB, src);
            if (wantOverlays && ms.bHasRtv) {
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cmd->ResourceBarrier(1, &mb);
                // Bind the panel's RTV. Nothing else on this branch does: the
                // blit paths bind it inside BlitMainToMirror /
                // BlitOrientOutputToMirror, and a straight CopyResource needs
                // no render target at all -- so the overlay draw here inherited
                // whatever target was last set on the list.
                D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv = ms.rtvHandles[mirrorFI];
                cmd->OMSetRenderTargets(1, &mirrorRtv, FALSE, nullptr);
                DrawOverlaysToMirror(cmd, ms.width, ms.height, false,
                                     MessageOwnerFor(out.config));
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                cmd->ResourceBarrier(1, &mb);
            } else {
                mb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                cmd->ResourceBarrier(1, &mb);
            }
            ms.lastPath = 4;
        } else {
            if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                mb.Transition.StateBefore = before;
                mb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cmd->ResourceBarrier(1, &mb);
            }
            // Perform the full-chain wipe that six other sites keep asking for.
            //
            // A blit that does not cover the whole face leaves the rest of it
            // holding whatever it held before -- and right after a resize, an
            // orient-pipe rebuild or a profile load, that is a differently
            // shaped frame. The chain then alternates fresh face, stale face,
            // which reads as bands of the old image tearing through the new.
            //
            // Only the CURRENT face may be touched: transitioning the others
            // while DWM still owns them TDRs the GPU. So the wipe necessarily
            // spans as many frames as the chain has faces, and is retired in
            // the Present loop below once every face has been repainted.
            //
            // Two guards beyond bHasRtv, and neither is defensive padding.
            //
            // rtvEpoch: a descriptor-heap rewind invalidates every handle in
            // ms.rtvHandles, and this loop does NOT revalidate them -- that is
            // done in the visibility pass, whose comment records what a stale
            // one costs ("used to overwrite bump-allocated mirror slots -> SEH
            // crash"). The blits below use the same handle unguarded, so this
            // does not make the frame safe on its own; it only keeps the write
            // ADDED here from being the one that reaches a rewound handle.
            //
            // It is NOT the fix for anything currently open. It was written
            // while chasing the MIRROR_WATERMARK device removal, and that
            // turned out to be unrelated: disabling this clear outright does
            // not prevent it, measured on three consecutive runs. Kept because
            // it is correct, not because it cured something.
            //
            // willHold: the branch below returns without touching the RTV when
            // an opposite-orientation panel has no fresh orient image, and an
            // orient-pipe recreate is one of the things that sets the wipe
            // flag. Clearing there reached a handle the frame had already
            // decided not to use, and wrote black into a face that is then
            // held rather than presented.
            const bool willHold = !useOrient && oppositeIndep && ms.bEverPresented;
            if (ms.bNeedsFullChainClear && ms.bHasRtv && !willHold &&
                ms.rtvEpoch == m_lpDX->m_descriptorEpoch) {
                const float black[4] = { 0.f, 0.f, 0.f, 1.f };
                cmd->ClearRenderTargetView(ms.rtvHandles[mirrorFI], black, 0, nullptr);
            }
            if (useOrient) {
                if (!BlitOrientOutputToMirror(*psurf, cmd,
                                              ms.rtvHandles[mirrorFI],
                                              ms.width, ms.height))
                    BlitMainToMirror(cmd, src, srcW, srcH,
                                     ms.rtvHandles[mirrorFI], ms.width, ms.height, 0);
                // Orient pass is warp+comp only — draw messages/sprites at panel size.
                DrawOverlaysToMirror(cmd, ms.width, ms.height, true,
                                     MessageOwnerFor(out.config));
                ms.lastPath = 1;
                ms.lastPubSeq = psurf->publishedSeq.load(std::memory_order_acquire);
            } else {
                // Opposite independent without a ready orient image: if this
                // panel EVER showed a sim frame, hold it — a stretch-filled
                // portrait frame here is exactly the "infrequent whole-screen
                // stretched flash" Shane pinned down (fires for 1-2 worker
                // periods on every orient-pipe recreate: descriptor-heap
                // rewind, preset mode switch, resize). Stretch only serves the
                // very first light-up, before any sim frame exists.
                if (oppositeIndep && ms.bEverPresented) {
                    ms.bFreshDraw = false;
                    ms.lastPath = 6;
                    // Undo the RT transition recorded above for this face.
                    mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                    cmd->ResourceBarrier(1, &mb);
                    if (mirrorFI < MIRROR_BUFFER_COUNT)
                        ms.bbState[mirrorFI] = D3D12_RESOURCE_STATE_PRESENT;
                    continue;
                }
                // Stretch-fill (letterbox looked like an unstretched portrait copy).
                const int scale = oppositeIndep ? 0 : (out.config.bIndependentRender ? 1 : 0);
                BlitMainToMirror(cmd, src, srcW, srcH,
                                 ms.rtvHandles[mirrorFI], ms.width, ms.height, scale);
                if (wantOverlays)
                    DrawOverlaysToMirror(cmd, ms.width, ms.height, false,
                                         MessageOwnerFor(out.config));
                ms.lastPath = 2;
            }
            mb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            mb.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            cmd->ResourceBarrier(1, &mb);
        }
        if (mirrorFI < MIRROR_BUFFER_COUNT)
            ms.bbState[mirrorFI] = D3D12_RESOURCE_STATE_PRESENT;
        ms.lastDrawFI = mirrorFI;
        ms.drawCount++;
        if (ms.hWnd && !IsWindowVisible(ms.hWnd))
            ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
    }

    if (haveSnap)
        m_mirrorSrcTex.currentState = srcState;
    else if (srcState != D3D12_RESOURCE_STATE_PRESENT) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.StateBefore = srcState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    }

    cmd->Close();
    m_lpDX->EndAuxUpload();
    ID3D12CommandList* lists[] = { cmd };
    m_lpDX->m_commandQueue->ExecuteCommandLists(1, lists);

    // DXGI frame latency is DEVICE-wide. At the default (2-3), multi-swapchain
    // flip queues can re-scan a STALE face every other refresh — visible as a
    // two-image flicker on the panels that composition-level captures
    // (PrintWindow/ImageGrab) can NOT see, since DWM composes the newest
    // frame while scanout flips the queue. The latency=1 call lived only in
    // the dead slot-path since the July rework; restore it here (2026-08-21).
    {
        int nMirrorPresents = 0;
        for (auto& out : m_displayOutputs)
            if (out.config.bEnabled && out.config.type == DisplayOutputType::Monitor &&
                out.monitorState && out.monitorState->bReady && !out.monitorState->bSoftDisabled)
                nMirrorPresents++;
        m_lpDX->EnsureMultiSwapChainFrameLatency(1 + nMirrorPresents, true);
    }

    // SC is created with Flags=0 — ALLOW_TEARING is invalid and Present fails
    // (invisible HWND). First present is blocking so flip-model actually shows.
    for (auto& out : m_displayOutputs) {
        if (!out.config.bEnabled || out.config.type != DisplayOutputType::Monitor)
            continue;
        if (!out.monitorState || !out.monitorState->bReady || out.monitorState->bSoftDisabled)
            continue;
        auto& ms = *out.monitorState;
        if (!ms.bFreshDraw)
            continue; // nothing new drawn this frame — keep the shown image
        if (ms.hWnd && !IsWindowVisible(ms.hWnd))
            ShowWindow(ms.hWnd, SW_SHOWNOACTIVATE);
        const UINT flags = ms.bEverPresented ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
        const UINT curIdx = ms.swapChain->GetCurrentBackBufferIndex();
        ms.lastPresentFI = curIdx;
        HRESULT hr = ms.swapChain->Present(0, flags);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            hr = ms.swapChain->Present(0, 0);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                ms.presentSkipCount++;
                ms.lastPresentHr = hr;
                continue;
            }
        }
        ms.lastPresentHr = hr;
        if (SUCCEEDED(hr)) {
            ms.bEverPresented = true;
            ms.presentOkCount++;
            m_nMirrorPresentAccum.fetch_add(1, std::memory_order_relaxed);
            // Retire the wipe once every face has been repainted. The only
            // code that did this went behind an #if 0 with the old present
            // path in 796fba30, so bNeedsFullChainClear stuck at 1 forever,
            // paintedBufferMask never took a single bit, and the clear above
            // -- which reads that flag -- had nothing left to switch it off.
            if (curIdx < 32)
                ms.paintedBufferMask |= (1u << curIdx);
            const UINT nBuf = ms.bufferCount ? ms.bufferCount : MIRROR_BUFFER_COUNT;
            const UINT allBits = (nBuf >= 32) ? 0xFFFFFFFFu : ((1u << nBuf) - 1u);
            if ((ms.paintedBufferMask & allBits) == allBits)
                ms.bNeedsFullChainClear = false;
        } else {
            char logBuf[256];
            sprintf(logBuf, "Mirror Present failed (0x%08X) on %ls\n",
                    (unsigned)hr, out.config.szDeviceName);
            DebugLogA(logBuf, LOG_ERROR);
            ms.presentFailCount++;
        }
    }
}

// ─── Independent mirror rendering ────────────────────────────────────────────

bool Engine::AnyIndependentMirrorEnabled() const
{
    if (!m_bMirrorsActive)
        return false;
    for (const auto& out : m_displayOutputs) {
        if (out.config.bEnabled &&
            out.config.type == DisplayOutputType::Monitor &&
            out.config.bIndependentRender)
            return true;
    }
    return false;
}

void Engine::DrawDeferredMessages()
{
    if (!MessagesEnabled() || !m_lpDX)
        return;
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
        // THE PRIMARY'S OWN, only (#255).
        //
        // This is the primary's message path whenever an independent mirror is
        // up -- the two loops in milkdropfs.cpp are both gated on
        // !AnyIndependentMirrorEnabled(), so filtering only those left this one
        // drawing everything. Reported immediately: "messages from all
        // displays were all showing on the main render", which is precisely
        // what an unfiltered pool looks like once several loops spawn into it.
        if (m_supertexts[i].nOwnerDisplay != -1)
            continue;
        if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
            float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
            if (fProgress <= 1.0f)
                ShowSongTitleAnim(GetWidth(), GetHeight(), min(fProgress, 0.9999f), i);
        }
    }
}

void Engine::DrawOverlaysToMirror(ID3D12GraphicsCommandList* cmdList, int monW, int monH,
                                  bool drawSprites, int ownerFilter)
{
    if (!cmdList || !m_lpDX || monW <= 0 || monH <= 0)
        return;

    // Viewport must match mirror RT (milk3 path already set it; re-assert for safety)
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

    // messages.ini / song-title anims stay on mirrors even when HUD is off.
    if (MessagesEnabled()) {
        for (int i = 0; i < NUM_SUPERTEXTS; i++) {
            // Whose message is this? (#255)
            //
            // A display running its own preset draws only what its own loop
            // spawned; a FOLLOWER draws the primary's, because it is showing
            // the primary's frame and its messages along with it. Without this
            // every panel drew every message, which is the lockstep the
            // per-display loops exist to end.
            if (m_supertexts[i].nOwnerDisplay != ownerFilter)
                continue;
            if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
                float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
                if (fProgress <= 1.0f)
                    ShowSongTitleAnim(monW, monH, min(fProgress, 0.9999f), i, cmdList);
            }
        }
    }

    if (drawSprites && SpritesEnabled()) {
        DrawUserSprites(0, cmdList);
        DrawUserSprites(1, cmdList);
    }

    if (m_bDisableMirrorHud)
        return;

    // HUD / preset / notifications: queue was laid out in primary client pixels.
    // Pass primary as layout size so positions and font scale map to monW×monH.
    const int layoutW = m_lpDX->m_client_width > 0 ? m_lpDX->m_client_width : monW;
    const int layoutH = m_lpDX->m_client_height > 0 ? m_lpDX->m_client_height : monH;
    m_text.DrawNow(cmdList, monW, monH, false, layoutW, layoutH);
}

void Engine::RenderMilk3ImageToMirror(ID3D12GraphicsCommandList* cmdList,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv, int monW, int monH)
{
    if (!cmdList || !m_lpDX || monW <= 0 || monH <= 0)
        return;

    // RAII: restore primary VS texsize — portrait dims must not stick into next primary frame.
    struct SizeOverrideGuard {
        Engine* e;
        explicit SizeOverrideGuard(Engine* eng, int w, int h) : e(eng) {
            e->m_nTexSizeX = w;
            e->m_nTexSizeY = h;
            e->SetOutputSizeOverride(w, h);
            e->m_fAspectX = (h > w) ? w / (float)h : 1.0f;
            e->m_fAspectY = (w > h) ? h / (float)w : 1.0f;
            e->m_fInvAspectX = 1.0f / e->m_fAspectX;
            e->m_fInvAspectY = 1.0f / e->m_fAspectY;
        }
        ~SizeOverrideGuard() { e->RestorePrimaryTexSizeFromVS(); }
    } sizeGuard(this, monW, monH);

    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    if (!m_dx12CompPSO)
        return;
    cmdList->SetPipelineState(m_dx12CompPSO.Get());

    PShaderInfo* compSI = &m_shaders.comp;
    if (compSI->CT) {
        ApplyShaderParams(&compSI->params, compSI->CT, m_pState);
        DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(compSI->CT);
        if (ct->GetShadowSize() > 0) {
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
                m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
            if (cbAddr)
                cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        }
    } else {
        BYTE zeros[256] = {};
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
        if (cbAddr)
            cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
    }

    // Bindings = primary Image pass (audio shared). Do not write feedback here —
    // OM is the mirror RT only.
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetCompBindingGpuHandle());

    MYVERTEX v[4];
    ZeroMemory(v, sizeof(v));
    const float cx[4] = { -1.f, 1.f, -1.f, 1.f };
    const float cy[4] = { 1.f, 1.f, -1.f, -1.f };
    const float cu[4] = { 0.f, 1.f, 0.f, 1.f };
    const float cv[4] = { 0.f, 0.f, 1.f, 1.f };
    for (int i = 0; i < 4; i++) {
        v[i].x = cx[i]; v[i].y = cy[i]; v[i].z = 0.f;
        v[i].Diffuse = 0xFFFFFFFFu;
        v[i].tu = cu[i]; v[i].tv = cv[i];
        v[i].tu_orig = cu[i]; v[i].tv_orig = cv[i];
    }
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
}

void Engine::BlitMainToMirror(ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* mainBB, int mainW, int mainH,
                              D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv, int monW, int monH,
                              int scaleMode)
{
    if (!cmdList || !m_lpDX || !mainBB || monW <= 0 || monH <= 0 || mainW <= 0 || mainH <= 0)
        return;

    // Root table is 32 SRVs — reuse one permanent block (allocate once).
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    if (m_mirrorLetterboxSrvBase == UINT_MAX) {
        D3D12_CPU_DESCRIPTOR_HANDLE first = m_lpDX->AllocateSrvCpu();
        m_mirrorLetterboxSrvBase = m_lpDX->m_nextFreeSrvSlot;
        m_lpDX->AllocateSrvGpu();
        for (UINT i = 1; i < DXContext::BINDING_BLOCK_SIZE; i++) {
            m_lpDX->AllocateSrvCpu();
            m_lpDX->AllocateSrvGpu();
        }
        (void)first;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE blockCpu = m_lpDX->GetSrvCpuHandleAt(m_mirrorLetterboxSrvBase);
    for (UINT i = 0; i < DXContext::BINDING_BLOCK_SIZE; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE slot = blockCpu;
        slot.ptr += (SIZE_T)i * m_lpDX->m_srvDescriptorSize;
        m_lpDX->m_device->CreateShaderResourceView(mainBB, &srvDesc, slot);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE blockGpu =
        m_lpDX->GetBindingBlockGpuHandleByIndex(m_mirrorLetterboxSrvBase);

    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->OMSetRenderTargets(1, &mirrorRtv, FALSE, nullptr);
    cmdList->ClearRenderTargetView(mirrorRtv, black, 0, nullptr);
    SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);

    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    cmdList->SetGraphicsRootDescriptorTable(1, blockGpu);
    BYTE zeros[256] = {};
    // A dedicated 256 KB buffer stood here for the one worker thread, chosen by
    // thread id. Phase 5a gave every worker its own upload ring and taught
    // SelectUploadRing to find it, so the ordinary path is already per-thread
    // correct -- and a thread-id test against ONE worker would now be wrong.
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
    if (cbAddr)
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);

    // scaleMode 0 = stretch (full UV, full NDC)
    // scaleMode 1 = letterbox/fit (shrink NDC, full UV — bars)
    // scaleMode 2 = cover/crop (full NDC, center-crop UV — fills screen)
    float nx = 1.f, ny = 1.f;
    float u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
    const float srcAr = (float)mainW / (float)mainH;
    const float dstAr = (float)monW / (float)monH;
    if (scaleMode == 1) {
        if (srcAr > dstAr)
            ny = dstAr / srcAr; // source wider → letterbox top/bottom
        else
            nx = srcAr / dstAr; // source taller → pillarbox left/right
    } else if (scaleMode == 2) {
        if (srcAr > dstAr) {
            // Source wider than dest — crop left/right, fill height
            const float visible = dstAr / srcAr;
            u0 = (1.f - visible) * 0.5f;
            u1 = 1.f - u0;
        } else {
            // Source taller than dest — crop top/bottom, fill width
            const float visible = srcAr / dstAr;
            v0 = (1.f - visible) * 0.5f;
            v1 = 1.f - v0;
        }
    }

    MYVERTEX v[4];
    ZeroMemory(v, sizeof(v));
    const float px[4] = { -nx, nx, -nx, nx };
    const float py[4] = { ny, ny, -ny, -ny };
    // TRIANGLESTRIP order: TL, TR, BL, BR
    const float pu[4] = { u0, u1, u0, u1 };
    const float pv[4] = { v0, v0, v1, v1 };
    for (int i = 0; i < 4; i++) {
        v[i].x = px[i]; v[i].y = py[i]; v[i].z = 0.f;
        v[i].Diffuse = 0xFFFFFFFFu;
        v[i].tu = pu[i]; v[i].tv = pv[i];
        v[i].tu_orig = pu[i]; v[i].tv_orig = pv[i];
    }
    // DrawVertices suballocates from the CALLING THREAD's ring since phase 5a,
    // so the hand-rolled vertex upload that used to sit here for the worker is
    // simply what this already does.
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
}

void Engine::SetMirrorIndependentRender(bool enable)
{
    // The early-out asks whether anything would CHANGE, which is not the same
    // question as whether the global flag already matches.
    //
    // It used to compare only m_bMirrorIndependentDefault, and the two can
    // disagree with the displays: the flag is loaded from
    // DisplayOutputs/MirrorIndependentDefault, while each display's
    // IndependentRender is loaded from its OWN section and only DEFAULTS to
    // the global one -- so a display carrying an explicit 0 keeps it. Asking
    // for "independent by default" then hit the early-out, because the global
    // was already 1, and the displays were never reconciled.
    //
    // Measured on Shane's instance: global independent=1 with DISPLAY1 and
    // DISPLAY3 both independent=0 and stuck on path=stretchMain, which blits
    // the PORTRAIT primary into a landscape mirror and leaves a live strip of
    // wrong content down the right-hand edge. Sending 1 -> 0 -> 1 to force two
    // real transitions fixed it, which is the shape of a reconciliation bug
    // rather than a rendering one. Reported as "I chose mirror with
    // independent render by default and display3 is showing some ghosting".
    bool anyDisplayDiffers = false;
    for (const auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Monitor &&
            out.config.bIndependentRender != enable) {
            anyDisplayDiffers = true;
            break;
        }
    }
    if (m_bMirrorIndependentDefault == enable && !anyDisplayDiffers) {
        // Genuinely nothing to do -- still returns so remote clients get a
        // response path.
        return;
    }
    m_bMirrorIndependentDefault = enable;
    for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Monitor)
            out.config.bIndependentRender = m_bMirrorIndependentDefault;
    }
    SaveDisplayOutputSettings();

    // Independent is a DRAW PATH only. Do not destroy swap chains (that froze
    // the UI). Copy mode resizes any leftover native-sized SC to primary size
    // on the render thread (see SendToDisplayOutputs).
    for (auto& out : m_displayOutputs) {
        if (!out.monitorState) continue;
        auto& ms = *out.monitorState;
        ms.bNeedsFullChainClear = true;
        ms.paintedBufferMask = 0;
        ms.lastPath = 0;
        ms.lastDrawFI = UINT_MAX;
        ms.drawCount = 0;
        ms.presentOkCount = 0;
        ms.presentFailCount = 0;
        ms.presentSkipCount = 0;
        for (UINT bi = 0; bi < MIRROR_BUFFER_COUNT; bi++)
            ms.bbState[bi] = ms.bEverPresented
                ? D3D12_RESOURCE_STATE_PRESENT
                : D3D12_RESOURCE_STATE_COMMON;
    }

    if (m_bMirrorIndependentDefault)
        m_bMirrorResetOrientNextFrame.store(true);
    else
        ClearOutputSizeOverride();
    m_bMirrorIndepSizeDirty.store(true);

    // Ensure windows stay visible after mode flip (no hide/recreate)
    m_bRaiseMirrorsNextFrame.store(true);
    m_bMirrorStylesDirty.store(true);
    RefreshDisplaysTab();
    AddNotification(m_bMirrorIndependentDefault
        ? L"Mirrors: independent (own feedback per orientation)"
        : L"Mirrors: copy mode (stretch)");
}

void Engine::ToggleMirrorIndependentRender()
{
    SetMirrorIndependentRender(!m_bMirrorIndependentDefault);
}

// ─── Mirror Window Style Updates ─────────────────────────────────────────────

// Compute desired mirror HWND rect from config (fullscreen = full monitor,
// windowed = centered ~80% of work area, min 640x360).
static void ComputeMirrorLayout(const DisplayOutputConfig& cfg,
                                int& outX, int& outY, int& outW, int& outH)
{
    RECT mon = cfg.rcMonitor;
    // Refresh monitor rect by device name when possible
    struct FindCtx { const wchar_t* name; RECT rc; bool found; };
    FindCtx ctx = { cfg.szDeviceName, mon, false };
    EnumDisplayMonitors(NULL, NULL,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<FindCtx*>(lp);
            MONITORINFOEXW mi = { sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi) &&
                wcscmp(mi.szDevice, c->name) == 0) {
                c->rc = mi.rcMonitor;
                c->found = true;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found)
        mon = ctx.rc;

    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;
    if (cfg.bFullscreen || monW <= 0 || monH <= 0) {
        outX = mon.left;
        outY = mon.top;
        outW = max(1, monW);
        outH = max(1, monH);
        return;
    }
    // Windowed: use work area if available, else 80% of monitor
    RECT work = mon;
    {
        HMONITOR hMon = MonitorFromRect(&mon, MONITOR_DEFAULTTONULL);
        if (hMon) {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi))
                work = mi.rcWork;
        }
    }
    int workW = work.right - work.left;
    int workH = work.bottom - work.top;
    outW = max(640, (workW * 4) / 5);
    outH = max(360, (workH * 4) / 5);
    if (outW > workW) outW = workW;
    if (outH > workH) outH = workH;
    outX = work.left + (workW - outW) / 2;
    outY = work.top + (workH - outH) / 2;
}

// What a display's window should actually look like, watermark resolved.
//
// ONE resolver, because there are two places that dress a mirror window and
// they had drifted (#286). ApplyMirrorWindowStyles knew about the watermark;
// InitDisplayOutput, which creates the window, read nOpacity and bClickThrough
// straight out of the config and nothing ever set m_bMirrorStylesDirty
// afterwards -- so a display that came up watermarked from displays.json or a
// profile got none of it. Measured on the live instance: DISPLAY3 reporting
// watermark=1 with clickthru=0, layered=0, alwaysontop=0, opacity=100.
//
// Watermark is an OVERLAY on the display's own settings, not a rewrite of
// them: it forces click-through, the watermark opacity and top-most for as
// long as it is on, while bClickThrough, nOpacity and bAlwaysOnTop go on
// recording what the user actually chose. Turning it off therefore restores
// those, which the old bulk gesture could not do because it had overwritten
// them.
void Engine::ResolveMirrorWindowStyle(const DisplayOutputConfig& cfg,
                                      bool& clickThru, int& opacity,
                                      bool& onTop) const
{
    const bool wm = cfg.bWatermark;
    clickThru = wm || cfg.bClickThrough;
    // A watermarked display is on top whatever the global setting says: being
    // above the windows it is watermarking is most of what the mode is for,
    // and the global flag cannot express "this one".
    onTop = m_bAlwaysOnTop || wm || cfg.bAlwaysOnTop;
    opacity = cfg.nOpacity;
    if (wm) {
        // This display's own watermark opacity if it has one, else the global.
        // nOpacity is left alone either way -- it is what the display returns
        // to when the watermark comes off.
        opacity = (cfg.nWatermarkOpacity > 0)
                      ? cfg.nWatermarkOpacity
                      : (int)(m_WindowWatermarkModeOpacity * 100.0f);
        if (opacity < 1) opacity = 1;
        if (opacity > 100) opacity = 100;
    }
}

void Engine::ApplyMirrorWindowStyles()
{
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor || !out.monitorState)
            continue;
        auto& ms = *out.monitorState;
        HWND hWnd = ms.hWnd;
        if (!hWnd) continue;

        bool clickThru = false, onTop = false;
        int opacity = 100;
        ResolveMirrorWindowStyle(out.config, clickThru, opacity, onTop);

        // Layered only when needed — flip + WS_EX_LAYERED ghosts other monitors' pixels.
        const bool needLayered = (opacity < 100) || clickThru;
        LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
        if (needLayered)
            ex |= WS_EX_LAYERED;
        else
            ex &= ~WS_EX_LAYERED;
        if (clickThru)
            ex |= WS_EX_TRANSPARENT;
        else
            ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, ex);
        if (needLayered) {
            BYTE alpha = (BYTE)(opacity * 255 / 100);
            if (alpha < 3) alpha = 3;
            SetLayeredWindowAttributes(hWnd, 0, alpha, LWA_ALPHA);
        }

        // Fullscreen checkbox: full monitor vs windowed layout
        int x = 0, y = 0, w = 0, h = 0;
        ComputeMirrorLayout(out.config, x, y, w, h);
        if (w != ms.width || h != ms.height) {
            ms.pendingX = x;
            ms.pendingY = y;
            ms.pendingW = w;
            ms.pendingH = h;
            ms.bPendingLayout = true;
        } else {
            const UINT zflags = SWP_NOACTIVATE | SWP_FRAMECHANGED;
            SetWindowPos(hWnd, MirrorZOrder(onTop), x, y, w, h, zflags);
        }
    }
}

// ─── Idle timer: opacity and z-order ──────────────────────────────────────────
//
// "Every screen" is THREE legs, not one, and the existing SET_MIRROR_OPACITY=
// path only ever did two. A display running in its own process has no
// monitorState, so ApplyMirrorWindowStyles skips it entirely and it keeps
// whatever alpha it was spawned with -- walking away from a watermarked setup
// and finding one screen still dim is exactly that gap.
//
// Nothing here calls SaveDisplayOutputSettings(). Idle opacity and z-order are
// transient by definition: they last until the user touches the mouse. Writing
// them to displays.json would turn a crash mid-idle into a permanent change to
// the user's arrangement.

void Engine::IdleSnapshotOpacity()
{
    m_idleOpacitySnap.clear();
    m_fIdlePrevOpacity = fOpacity;
    m_bIdlePrevAlwaysOnTop = m_bAlwaysOnTop;
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        IdleOpacitySnap s;
        s.device = out.config.szDeviceName;
        s.nOpacity = out.config.nOpacity;
        s.nWatermarkOpacity = out.config.nWatermarkOpacity;
        m_idleOpacitySnap.push_back(std::move(s));
    }
    m_bIdleOpacitySnapValid = true;
}

void Engine::IdleApplyFullOpacity()
{
    // Leg 1 -- this instance's own window. SetOpacity reads fOpacity and must
    // run on the thread that owns the render window, so post rather than call:
    // this runs on the render thread.
    fOpacity = 1.0f;
    HWND hw = GetPluginWindow();
    if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);

    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        out.config.nOpacity = 100;
        // bWatermark is left ALONE -- click-through and always-on-top are the
        // user's arrangement and idle has no business rewriting them. But
        // ResolveMirrorWindowStyle lets the watermark opacity outrank nOpacity,
        // so without this a watermarked display stays dim and the 100% never
        // lands: a live setting with no visible effect.
        if (out.config.bWatermark)
            out.config.nWatermarkOpacity = 100;
        // Leg 3 -- a child owns its window outright, in another process, so no
        // style pass here can reach it.
        if (out.config.bOwnProcess)
            RequestChildSend(out.config.szDeviceName, L"OPACITY=1.000");
    }
    // Leg 2 -- in-process mirrors and own-preset surfaces, applied by the style
    // pass at the top of the next frame.
    m_bMirrorStylesDirty.store(true);
}

void Engine::IdleRaiseToTop()
{
    // Not under testing mode. The raise is done for the benefit of a human who
    // is looking at the screen, and testing mode exists to say nobody is: a
    // harness instance driven to always-on-top sits over the user's work, and
    // combined with a fullscreen idle action it is the exact gesture that once
    // locked him out of his own machine. Testing mode already suppresses
    // tool-window focus for this reason -- anything it makes disposable, it
    // should suppress.
    if (m_bTestingMode) return;

    // A screensaver behind the browser window is not a screensaver. On the
    // same-profile short circuit this and the opacity are the ONLY things that
    // change, which is the point of that path: the profile reload it skips
    // would have achieved the raise by rebuilding every window.
    m_bAlwaysOnTop = true;
    // The render window belongs to the MESSAGE thread. SetWindowPos on another
    // thread's window can SendMessage to its owner and deadlock -- the same
    // reason RaiseMirrorSurfaces refuses to touch HWNDs it does not own.
    HWND hw = GetPluginWindow();
    if (hw) PostMessage(hw, WM_MW_SET_ALWAYS_ON_TOP, 0, 0);
    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        if (out.config.bOwnProcess)
            RequestChildSend(out.config.szDeviceName, L"SET_ALWAYS_ON_TOP=1");
    }
    // In-process mirrors read onTop from m_bAlwaysOnTop in
    // ResolveMirrorWindowStyle, so raising the dirty flag is the whole job.
    m_bMirrorStylesDirty.store(true);
}

void Engine::IdleRestoreOpacity()
{
    if (!m_bIdleOpacitySnapValid) return;

    fOpacity = m_fIdlePrevOpacity;
    m_bAlwaysOnTop = m_bIdlePrevAlwaysOnTop;
    HWND hw = GetPluginWindow();
    if (hw) {
        PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
        PostMessage(hw, WM_MW_SET_ALWAYS_ON_TOP, 0, 0);
    }

    for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        for (const auto& s : m_idleOpacitySnap) {
            if (s.device != out.config.szDeviceName) continue;
            out.config.nOpacity = s.nOpacity;
            out.config.nWatermarkOpacity = s.nWatermarkOpacity;
            break;
        }
        // A display that appeared DURING the idle session is not in the
        // snapshot and is left as it is -- it was never changed by idle, so
        // there is nothing of it to put back.
        if (out.config.bOwnProcess) {
            wchar_t buf[48];
            FormatTo(buf, L"OPACITY=%.3f", (float)out.config.nOpacity / 100.0f);
            RequestChildSend(out.config.szDeviceName, buf);
            // That DISPLAY's own flag, not the global one idle borrowed (#340).
            // Restoring the global here told a child whose Keep on top is ON to
            // turn it off, purely because the main window's Always On Top
            // happened to be off -- the same mistake #338 found in
            // ToggleAlwaysOnTop, one relay further out. cfg.bAlwaysOnTop is what
            // ChildConfigureNow sends a child at spawn, so this says the same
            // thing that would be said if the child restarted.
            RequestChildSend(out.config.szDeviceName,
                             out.config.bAlwaysOnTop ? L"SET_ALWAYS_ON_TOP=1"
                                                     : L"SET_ALWAYS_ON_TOP=0");
        }
    }
    m_bMirrorStylesDirty.store(true);
    m_idleOpacitySnap.clear();
    m_bIdleOpacitySnapValid = false;
}

bool Engine::IdleBankDisplayState()
{
    // SaveDisplayProfile does NOT create its own directory -- every caller
    // does it, SaveDisplayProfileSnapshot included. Without this the bank
    // fails on an install that has never saved a profile, and because the
    // bank failing is what refuses the load, the idle profile would simply
    // never apply, with only a line in debug.log to say why.
    profiles::EnsureDirPath(DisplayProfileDir());
    return SaveDisplayProfile(IdleRestoreProfilePath().c_str());
}

std::wstring Engine::IdleRestoreProfilePath() const
{
    // The leading underscore is what keeps this out of ListDisplayProfiles: it
    // is bookkeeping, not a profile anybody chose, and it has no business in
    // the Settings combo, the Displays window list or StepDisplayProfile's walk.
    return DisplayProfileDir() + L"_idle_restore.json";
}

// ─── Displays Tab Refresh ─────────────────────────────────────────────────────

void Engine::RefreshDisplaysTab()
{
    HWND hWnd = m_displaysWindow ? m_displaysWindow->GetHWND() : NULL;
    if (!hWnd) return;

    HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
    if (!hList) return;

    // Remember the selection across the rebuild. LB_RESETCONTENT drops it, and
    // all twenty-six callers of this function therefore cleared whichever
    // display the user had picked. That was survivable while the only refreshes
    // were user actions; it stops being survivable the moment this is also
    // called on a timer, which is what makes a starting child's status update.
    const LRESULT keepSel = SendMessage(hList, LB_GETCURSEL, 0, 0);

    SendMessage(hList, LB_RESETCONTENT, 0, 0);

    // Resolve primary (render) monitor for clearer list status
    wchar_t renderDevice[32] = {};
    if (m_lpDX && m_lpDX->GetHwnd()) {
        HMONITOR hMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = { sizeof(mi) };
        if (hMon && GetMonitorInfoW(hMon, &mi))
            CopyTo(renderDevice, mi.szDevice);
    }

    // Build every row's text FIRST, under the lock, into a local buffer --
    // then release the lock before touching hList at all. #27's lock must
    // never be held across a SendMessage(W) to a control owned by the
    // Displays ToolWindow thread: when this runs on the render thread (most
    // RenderCmd handlers call it), that SendMessage blocks waiting for the
    // ToolWindow thread's own message pump to answer, and if that thread is
    // itself blocked trying to take this same (recursive, but only within
    // ONE thread) lock -- which any of its own field-edit handlers can be,
    // this rapidly -- the two threads deadlock each other. Measured: this
    // exact shape hung the process solid inside a couple of seconds of
    // concurrent Add Spout/Remove clicks and IPC traffic, caught by
    // test_display_outputs_thread_safety.py.
    std::vector<std::wstring> rows;
    {
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        rows.reserve(m_displayOutputs.size());
        for (size_t i = 0; i < m_displayOutputs.size(); i++) {
            auto& out = m_displayOutputs[i];
            auto& cfg = out.config;
            wchar_t label[256];
            const wchar_t* prefix = (cfg.type == DisplayOutputType::Monitor) ? L"[Monitor]" : L"[Spout]";
            const wchar_t* status;
            if (cfg.type == DisplayOutputType::Monitor) {
                const bool isPrimary = renderDevice[0] &&
                    wcscmp(cfg.szDeviceName, renderDevice) == 0;
                if (isPrimary)
                    status = L"PRIMARY (no mirror)";
                else if (!cfg.bEnabled)
                    status = L"OFF";
                else if (!m_bMirrorsActive)
                    status = L"ON (not active)";
                else if (out.bSkippedSameMonitor)
                    status = L"SKIPPED";
                else
                    status = L"MIRRORING";
            }
            else {
                status = cfg.bEnabled ? L"ON" : L"OFF";
            }
            // A display running its own instance says so, and says what it is
            // playing. Not being able to tell which preset was on which screen was
            // half of what made this feature hard to use (forgejo#22).
            if (cfg.type == DisplayOutputType::Monitor && cfg.bOwnProcess) {
                ChildInstance ci;
                const bool have = GetChildInfo(cfg.szDeviceName, ci);
                const wchar_t* state = !have    ? L"starting"
                                     : ci.bFailed ? L"FAILED"
                                     : ci.bReady  ? L"CHILD"
                                                  : L"starting";
                const wchar_t* preset = L"";
                if (have && ci.szCurrentPreset[0]) {
                    const wchar_t* slash = wcsrchr(ci.szCurrentPreset, L'\\');
                    preset = slash ? slash + 1 : ci.szCurrentPreset;
                }
                if (preset[0])
                    swprintf(label, 256, L"%s %s  (%s)  %s", prefix, cfg.szName,
                             state, preset);
                else
                    swprintf(label, 256, L"%s %s  (%s)", prefix, cfg.szName, state);
            } else {
                swprintf(label, 256, L"%s %s  (%s)", prefix, cfg.szName, status);
            }
            rows.emplace_back(label);
        }
    }
    for (const auto& row : rows)
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)row.c_str());

    if (keepSel != LB_ERR &&
        keepSel < (LRESULT)SendMessage(hList, LB_GETCOUNT, 0, 0))
        SendMessage(hList, LB_SETCURSEL, (WPARAM)keepSel, 0);

    // How far the horizontal scrollbar reaches: the widest row actually in the
    // list, measured in the list's own font.
    //
    // Without this the bar exists but does nothing, and a detail line naming a
    // long preset is simply cut off at the right edge -- which is what made the
    // window keep growing to fit names that have no length limit.
    {
        HDC hdc = GetDC(hList);
        HFONT hf = (HFONT)SendMessage(hList, WM_GETFONT, 0, 0);
        HFONT old = hf ? (HFONT)SelectObject(hdc, hf) : NULL;
        int widest = 0;
        const int count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; i++) {
            wchar_t row[256] = {};
            if (SendMessageW(hList, LB_GETTEXTLEN, i, 0) >= 256) continue;
            SendMessageW(hList, LB_GETTEXT, i, (LPARAM)row);
            SIZE sz = {};
            GetTextExtentPoint32W(hdc, row, (int)wcslen(row), &sz);
            if (sz.cx > widest) widest = sz.cx;
        }
        if (old) SelectObject(hdc, old);
        ReleaseDC(hList, hdc);
        SendMessage(hList, LB_SETHORIZONTALEXTENT, (WPARAM)(widest + 12), 0);
    }

    // The arrangement is the same data drawn differently, so it is stale the
    // moment the list is not.
    if (HWND hPanel = GetDlgItem(hWnd, IDC_MW_DISP_PANEL))
        InvalidateRect(hPanel, NULL, FALSE);

    // Sync Activate Mirrors button text
    HWND hBtn = GetDlgItem(hWnd, IDC_MW_DISP_ACTIVATE);
    if (hBtn) SetWindowTextW(hBtn, m_bMirrorsActive ? L"Deactivate Mirror/Children"
                                                    : L"Activate Mirror/Children");

}

// What the list would SAY about the children right now, cheaply.
//
// The Displays list is only redrawn when something asks it to, so a display
// switched to its own process sat on "(starting)" until the user happened to
// click something else -- the child was up in well under a second and the list
// had no way to know. A timer polls this instead of rebuilding every second:
// the rebuild flickers and costs a LB_RESETCONTENT, and nothing changes between
// ticks for almost all of them.
std::wstring Engine::ChildListSignature() const
{
    std::wstring sig;
    for (auto& out : m_displayOutputs) {
        const auto& cfg = out.config;
        if (cfg.type != DisplayOutputType::Monitor || !cfg.bOwnProcess) continue;
        ChildInstance ci;
        const bool have = GetChildInfo(cfg.szDeviceName, ci);
        sig += cfg.szDeviceName;
        sig += have ? (ci.bFailed ? L"|F" : ci.bReady ? L"|R" : L"|S") : L"|-";
        if (have) sig += ci.szCurrentPreset;
        sig += L';';
    }
    return sig;
}

// Clear and disable every control in the "Selected display" group that only
// means anything for a display running its own process.
//
// Split out because the "nothing selected" branch of UpdateDisplaysTabSelection
// below used to RETURN before reaching them. Nine controls were disabled there
// and eleven were left exactly as DoBuildControls had constructed them: enabled,
// and showing their construction defaults. So with no row selected the group
// read as a live, editable panel -- and every handler behind it is gated on
// having a selection, so the "..." browse buttons and the preset combos looked
// usable and silently did nothing when used. The buttons are owner-drawn and DO
// honour ODS_DISABLED, so once they are actually disabled they look it.
static void ClearDisplayChildControls(HWND hWnd)
{
    auto uncheck = [](HWND h) {
        if (!h) return;
        SetPropW(h, L"Checked", (HANDLE)(intptr_t)0);
        InvalidateRect(h, NULL, TRUE);
    };
    const int kRadios[] = {
        IDC_MW_DISP_MODE_MIRROR, IDC_MW_DISP_MODE_PRESET, IDC_MW_DISP_MODE_CHILD,
        IDC_MW_DISP_ORDER_SEQ,
        IDC_MW_DISP_LOCK_INHERIT, IDC_MW_DISP_LOCK_OFF, IDC_MW_DISP_LOCK_ON,
        IDC_MW_DISP_CYCLE_ON,
    };
    for (int id : kRadios) {
        HWND h = GetDlgItem(hWnd, id);
        if (!h) continue;
        uncheck(h);
        EnableWindow(h, FALSE);
    }
    for (int id : { IDC_MW_DISP_PRESETDIR, IDC_MW_DISP_STARTUP }) {
        HWND h = GetDlgItem(hWnd, id);
        if (!h) continue;
        SetModeComboValue(h, L"");
        EnableWindow(h, FALSE);
    }
    if (HWND h = GetDlgItem(hWnd, IDC_MW_DISP_CYCLE_SECS)) {
        SetWindowTextW(h, L"");
        EnableWindow(h, FALSE);
    }
    if (HWND h = GetDlgItem(hWnd, IDC_MW_DISP_NOW_PRESET)) {
        SetTextIfChanged(h, L"");
        EnableWindow(h, FALSE);
    }
    for (int id : { IDC_MW_DISP_PRESETDIR_BR, IDC_MW_DISP_STARTUP_BR,
                    IDC_MW_DISP_NOW_BROWSE }) {
        if (HWND h = GetDlgItem(hWnd, id)) EnableWindow(h, FALSE);
    }
}

void Engine::RefreshDisplayNowPlaying()
{
    HWND hWnd = m_displaysWindow ? m_displaysWindow->GetHWND() : NULL;
    if (!hWnd) return;
    HWND hNowPre = GetDlgItem(hWnd, IDC_MW_DISP_NOW_PRESET);
    // Disabled means the selection has no preset of its own, which the
    // selection sync has already worked out -- no need to decide it twice.
    if (!hNowPre || !IsWindowEnabled(hNowPre)) return;

    const int sel = m_nDisplaysTabSel;
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
    if (sel < 0 || sel >= (int)m_displayOutputs.size()) return;
    const auto& cfg = m_displayOutputs[sel].config;

    wchar_t renderDev[32] = {};
    const bool isRenderHost =
        GetRenderMonitorDevice(renderDev) && renderDev[0] &&
        wcscmp(renderDev, cfg.szDeviceName) == 0;
    const std::wstring now = isRenderHost ? std::wstring(m_szCurrentPresetFile)
                                          : DisplayCurrentPresetPath(cfg);
    SetTextIfChanged(hNowPre, FileNameOnly(now.c_str()));
}

void Engine::UpdateDisplaysTabSelection(int sel)
{
    HWND hWnd = m_displaysWindow ? m_displaysWindow->GetHWND() : NULL;
    if (!hWnd) return;
    m_nDisplaysTabSel = sel;

    HWND hEnable    = GetDlgItem(hWnd, IDC_MW_DISP_ENABLE);
    HWND hFullscr   = GetDlgItem(hWnd, IDC_MW_DISP_FULLSCREEN);
    HWND hClickThru = GetDlgItem(hWnd, IDC_MW_DISP_CLICKTHRU);
    HWND hOpacity   = GetDlgItem(hWnd, IDC_MW_DISP_OPACITY);
    HWND hOpSpin    = GetDlgItem(hWnd, IDC_MW_DISP_OPACITY_SPIN);
    HWND hName      = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_NAME);
    HWND hFixed     = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_FIXED);
    HWND hW         = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_W);
    HWND hH         = GetDlgItem(hWnd, IDC_MW_DISP_SPOUT_H);

    // Helper: sync custom owner-drawn checkbox property + visual state.
    //
    // BOTH mechanisms, because this group contains both kinds of control. The
    // checkboxes come from CreateCheck and are owner-drawn from a "Checked"
    // window property; the Random/Sequential and Inherit/Unlocked/Locked
    // radios are created straight from CreateWindowExW as plain
    // BS_AUTORADIOBUTTONs and paint from the real button state.
    //
    // Setting only the property meant those five radios never showed what was
    // selected -- measured, not assumed: setting the property on "Unlocked"
    // left it hollow, while clearing the button state on "Inherit" emptied it,
    // so the paint follows the state and not the property. That affected every
    // display, not merely the main render, which is why it is fixed here
    // rather than worked around at the call site.
    //
    // BM_SETCHECK on an owner-drawn checkbox is harmless: it never reads the
    // real state when painting.
    auto SetCheckbox = [](HWND hCtrl, bool checked) {
        if (!hCtrl) return;
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
        SendMessageW(hCtrl, BM_SETCHECK,
                     (WPARAM)(checked ? BST_CHECKED : BST_UNCHECKED), 0);
        InvalidateRect(hCtrl, NULL, TRUE);
    };

    bool haveSel;
    {
        // m_displayOutputsMutex: just the size check, released immediately
        // after -- nothing below needs the lock held. #27.
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        haveSel = sel >= 0 && sel < (int)m_displayOutputs.size();
    }
    if (!haveSel) {
        // Nothing selected — clear/disable controls
        if (hEnable)    { SetCheckbox(hEnable, false); EnableWindow(hEnable, FALSE); }
        if (hFullscr)   { SetCheckbox(hFullscr, false); EnableWindow(hFullscr, FALSE); }
        if (hClickThru) { SetCheckbox(hClickThru, false); EnableWindow(hClickThru, FALSE); }
        if (hOpacity)   { SetWindowTextW(hOpacity, L""); EnableWindow(hOpacity, FALSE); }
        if (hOpSpin)    { EnableWindow(hOpSpin, FALSE); }
        if (hName)      { SetWindowTextW(hName, L""); EnableWindow(hName, FALSE); }
        if (hFixed)     { SetCheckbox(hFixed, false); EnableWindow(hFixed, FALSE); }
        if (hW)         { SetWindowTextW(hW, L""); EnableWindow(hW, FALSE); }
        if (hH)         { SetWindowTextW(hH, L""); EnableWindow(hH, FALSE); }
        // The rest of the group, which this branch used to walk straight past.
        ClearDisplayChildControls(hWnd);
        return;
    }

    // A COPY, not the live reference this used to bind here -- taken under
    // the lock and used for the rest of the function with the lock released.
    // #27's lock must never be held across a SendMessage(W)/EnableWindow/
    // SetWindowTextW to a control owned by the Displays ToolWindow thread,
    // which is everything below this point: when this function runs on the
    // render thread (most RenderCmd handlers call it), those calls block
    // waiting for the ToolWindow thread's own message pump, and if that
    // thread is itself blocked trying to take this same lock -- which any of
    // its own field-edit handlers can be -- the two threads deadlock each
    // other. Measured: this exact shape hung the process solid within
    // seconds of concurrent Add Spout/Remove clicks and IPC traffic, caught
    // by test_display_outputs_thread_safety.py. DisplayOutputConfig is
    // POD-ish (fixed-size members, no owned pointers), so the copy is cheap.
    DisplayOutputConfig cfg;
    {
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
        cfg = m_displayOutputs[sel].config;
    }
    bool isSpout = (cfg.type == DisplayOutputType::Spout);
    bool isMon = !isSpout;

    // Enable checkbox — always available
    if (hEnable)  { SetCheckbox(hEnable, cfg.bEnabled); EnableWindow(hEnable, TRUE); }

    // Fullscreen — only for monitors
    if (hFullscr) { SetCheckbox(hFullscr, cfg.bFullscreen); EnableWindow(hFullscr, isMon); }

    // Click-through and opacity — only for monitors
    if (hClickThru) { SetCheckbox(hClickThru, cfg.bClickThrough); EnableWindow(hClickThru, isMon); }
    // Watermark — per display, so it follows the selection like the rest of
    // this group rather than reading a global.
    if (HWND hWm = GetDlgItem(hWnd, IDC_MW_DISP_WATERMARK)) {
        SetCheckbox(hWm, cfg.bWatermark);
        EnableWindow(hWm, isMon);
    }
    // Keep on top, unnested from the watermark control and enabled like every
    // other member of this group (#338). It was the one box in here that was
    // repainted only if the watermark box happened to exist and never got an
    // EnableWindow of its own, so it stayed clickable on a Spout row whose
    // handler drops the write -- click, paints checked, nothing happens.
    if (HWND hTop = GetDlgItem(hWnd, IDC_MW_DISP_ONTOP)) {
        // The PRIMARY row shows the GLOBAL flag, because that is what it now
        // drives: that display is the render window and has no mirror of its
        // own, so cfg.bAlwaysOnTop would be a value nothing reads.
        wchar_t renderDev[32] = {};
        const bool isPrimary = isMon && GetRenderMonitorDevice(renderDev) &&
                               renderDev[0] &&
                               wcscmp(cfg.szDeviceName, renderDev) == 0;
        SetCheckbox(hTop, isPrimary ? m_bAlwaysOnTop : cfg.bAlwaysOnTop);
        EnableWindow(hTop, isMon);
    }
    // Show the value the style pass is ACTUALLY using: the watermark opacity
    // while watermarked, the normal one otherwise (#276). Showing nOpacity
    // during watermark was half of why the control looked broken -- it
    // displayed a number that was not on screen and accepted edits that went
    // nowhere.
    const int shownOpacity =
        cfg.bWatermark
            ? (cfg.nWatermarkOpacity > 0
                   ? cfg.nWatermarkOpacity
                   : (int)(m_WindowWatermarkModeOpacity * 100.0f))
            : cfg.nOpacity;
    if (hOpacity) {
        wchar_t buf[8];
        swprintf(buf, 8, L"%d", shownOpacity);
        SetWindowTextW(hOpacity, isMon ? buf : L"");
        EnableWindow(hOpacity, isMon);
    }
    if (hOpSpin) {
        if (isMon) SendMessage(hOpSpin, UDM_SETPOS32, 0, shownOpacity);
        EnableWindow(hOpSpin, isMon);
    }

    // Spout-specific fields
    if (hName) { SetWindowTextW(hName, isSpout ? cfg.szName : L""); EnableWindow(hName, isSpout); }
    if (hFixed) { SetCheckbox(hFixed, cfg.bFixedSize); EnableWindow(hFixed, isSpout); }
    wchar_t buf[32];
    if (hW) { swprintf(buf, 32, L"%d", cfg.nWidth); SetWindowTextW(hW, isSpout ? buf : L""); EnableWindow(hW, isSpout); }
    if (hH) { swprintf(buf, 32, L"%d", cfg.nHeight); SetWindowTextW(hH, isSpout ? buf : L""); EnableWindow(hH, isSpout); }

    // ── Per-display child settings (forgejo#22) ──────────────────────────
    //
    // Only a monitor can be a child, and only a child has a preset directory
    // of its own -- a mirror renders the primary's preset. Grey the rest out
    // rather than leave controls that silently do nothing.
    {
        HWND hModeMir  = GetDlgItem(hWnd, IDC_MW_DISP_MODE_MIRROR);
        HWND hModeChi  = GetDlgItem(hWnd, IDC_MW_DISP_MODE_CHILD);
        HWND hDir      = GetDlgItem(hWnd, IDC_MW_DISP_PRESETDIR);
        HWND hDirBr    = GetDlgItem(hWnd, IDC_MW_DISP_PRESETDIR_BR);
        HWND hStartup  = GetDlgItem(hWnd, IDC_MW_DISP_STARTUP);
        HWND hStartBr  = GetDlgItem(hWnd, IDC_MW_DISP_STARTUP_BR);
        HWND hCycleOn  = GetDlgItem(hWnd, IDC_MW_DISP_CYCLE_ON);
        HWND hCycleSec = GetDlgItem(hWnd, IDC_MW_DISP_CYCLE_SECS);
        HWND hOrdSeq   = GetDlgItem(hWnd, IDC_MW_DISP_ORDER_SEQ);
        HWND hNowPre   = GetDlgItem(hWnd, IDC_MW_DISP_NOW_PRESET);
        HWND hNowBr    = GetDlgItem(hWnd, IDC_MW_DISP_NOW_BROWSE);

        const bool isChild = isMon && cfg.bOwnProcess;

        // The monitor the main window is on is a display too, and selecting it
        // now edits the MAIN RENDER's preset settings -- for the profile being
        // saved, not for the app.
        //
        // It is the same tile the panel already labels "MAIN WINDOW", so there
        // is nothing new to find: pick the screen the visualiser is on and the
        // preset controls describe it. Before this they greyed out on that one
        // tile, because only an own-process child had a preset of its own --
        // so a profile could restore every screen except the largest, and the
        // main window's preset could only be changed from the Presets window,
        // where a change is global rather than part of a setup.
        //
        // Two important differences from a child, both of which follow from
        // the main render not being a child:
        //
        //   * these values live in m_primaryProfile, not in out.config, and
        //     nothing here writes settings.ini -- so a profile that is never
        //     loaded changes nothing;
        //   * "(default)" means inherit the global rather than inherit the
        //     main window, because for this row the main window IS the thing
        //     being described.
        wchar_t renderDev[32] = {};
        const bool isRenderHost =
            isMon && GetRenderMonitorDevice(renderDev) && renderDev[0] &&
            wcscmp(renderDev, cfg.szDeviceName) == 0;
        // Either kind can carry preset settings; which STORE they come from is
        // what differs below.
        // A display holding its own preset IN PROCESS has all the same preset
        // settings a child does -- directory, startup, cycling, order, lock --
        // so the controls follow bOwnPreset, not the process.
        const bool hasPresets =
            (isMon && cfg.bOwnPreset) || isChild || isRenderHost;
        const PrimaryProfileCfg& pp = m_primaryProfile;

        if (hModeMir) {
            SetCheckbox(hModeMir, isMon && !cfg.bOwnPreset && !cfg.bOwnProcess);
            EnableWindow(hModeMir, isMon);
        }
        // The middle tier: its own preset, in process. Ticked only when the
        // display is NOT a child, so an automatic promotion shows as the third
        // radio while it lasts and drops back here when the preset changes.
        if (HWND hModePre = GetDlgItem(hWnd, IDC_MW_DISP_MODE_PRESET)) {
            SetCheckbox(hModePre, isMon && cfg.bOwnPreset && !cfg.bOwnProcess);
            EnableWindow(hModePre, isMon);
        }
        if (hModeChi) {
            SetCheckbox(hModeChi, isChild);
            EnableWindow(hModeChi, isMon);
        }
        // Both are mode combos now (CreateModeCombo), so an empty stored value
        // is shown as the mode it has always meant rather than as a blank the
        // user would have to guess at.
        const wchar_t* showDir = L"";
        const wchar_t* showPreset = L"";
        if (isRenderHost) {
            showDir    = pp.szPresetDir[0] ? pp.szPresetDir : kDispModeDefault;
            // <Default> rather than <Random>: an unset child picks a random
            // preset because that is what a child with no pin does, but an
            // unset MAIN render means "whatever the profile captures", which
            // is the running preset. Showing <Random> here would claim the
            // profile had asked for a random one.
            showPreset = pp.szStartupPreset[0] ? pp.szStartupPreset
                                               : kDispModeDefault;
        } else if (hasPresets) {
            // hasPresets, not isChild (#330). The ENABLE path above was taught
            // that a display holding its own preset in process carries the same
            // settings a child does; this one was not, so for that tier -- the
            // ordinary way to give a display its own preset -- the combos came
            // up enabled and empty.
            //
            // Everything the value was set to still worked: the preset loaded,
            // the profile saved it, the config kept it. Only the window never
            // showed it, which reads as the setting having been lost. "saved
            // presets will load but the preset name won't be in the drop list",
            // and with both fields blank the directory has to be picked again
            // before the preset, every time.
            showDir    = cfg.szPresetDir[0] ? cfg.szPresetDir : kDispModeDefault;
            showPreset = cfg.szStartupPreset[0] ? cfg.szStartupPreset
                                                : kDispModeRandom;
        }
        if (hDir)     { SetModeComboValue(hDir, showDir);
                        EnableWindow(hDir, hasPresets); }
        if (hDirBr)   EnableWindow(hDirBr, hasPresets);
        if (hStartup) { SetModeComboValue(hStartup, showPreset);
                        EnableWindow(hStartup, hasPresets); }
        if (hStartBr) EnableWindow(hStartBr, hasPresets);

        // fTimeBetweenPresets: -1 inherits the parent's, 0 means no cycling.
        // The checkbox is "does this display cycle at all", so an inherited
        // value shows as ticked with the parent's number in the box.
        //
        // For the main render, -1 inherits the GLOBAL interval, so the box
        // shows what the app is actually cycling at when the profile has not
        // pinned one -- the same thing a child's inherited value shows.
        float eff;
        if (isRenderHost)
            eff = pp.fTimeBetweenPresets >= 0.0f ? pp.fTimeBetweenPresets
                                                 : m_fTimeBetweenPresets;
        else
            eff = EffectiveChildInterval(cfg);
        if (hCycleOn)  { SetCheckbox(hCycleOn, hasPresets && eff > 0.0f);
                         EnableWindow(hCycleOn, hasPresets); }
        if (hCycleSec) {
            swprintf(buf, 32, L"%d", (int)(eff > 0.0f ? eff : 30.0f));
            SetWindowTextW(hCycleSec, hasPresets ? buf : L"");
            EnableWindow(hCycleSec, hasPresets && eff > 0.0f);
        }
        const bool seq = isRenderHost
            ? (pp.nSequentialOrder >= 0 ? pp.nSequentialOrder != 0
                                        : m_bSequentialPresetOrder)
            : cfg.bSequentialOrder;
        // One checkbox now: unticked is random. Random was never a separate
        // setting, only the other half of this boolean.
        if (hOrdSeq) { SetCheckbox(hOrdSeq, hasPresets && seq);
                       EnableWindow(hOrdSeq, hasPresets); }

        // What is on this display right now (#364), which is not the same
        // question as what it starts on and until now had no answer anywhere
        // in the UI. The 1 s child-poll timer refreshes it, so it tracks a
        // display cycling on its own as well as one just changed from here.
        //
        // Only the file name: the full path is what the field holds for
        // copying, but a whole path in a field this wide shows its useless
        // half. The main render is included -- it is a display too, and the
        // row already describes its preset settings.
        if (hNowPre) {
            std::wstring now = isRenderHost ? std::wstring(m_szCurrentPresetFile)
                                            : DisplayCurrentPresetPath(cfg);
            const wchar_t* show = hasPresets ? FileNameOnly(now.c_str()) : L"";
            SetTextIfChanged(hNowPre, show);
            // Enabled so the text can be selected and copied; it is read-only
            // either way, and a greyed-out field is unreadable rather than
            // merely uneditable.
            EnableWindow(hNowPre, hasPresets);
        }
        if (hNowBr) EnableWindow(hNowBr, hasPresets);

        // Preset lock: the RAW value picks the radio, because which of the
        // three is selected is exactly the thing the user set. Showing the
        // resolved value here would make Inherit unselectable the moment the
        // main window happened to agree with it.
        HWND hLkInh = GetDlgItem(hWnd, IDC_MW_DISP_LOCK_INHERIT);
        HWND hLkOff = GetDlgItem(hWnd, IDC_MW_DISP_LOCK_OFF);
        HWND hLkOn  = GetDlgItem(hWnd, IDC_MW_DISP_LOCK_ON);
        // The main render shows its OWN lock, not a pin, and cannot inherit --
        // it is what every other display inherits FROM.
        const int lockRaw = isRenderHost ? (m_bPresetLockedByUser ? 1 : 0)
                                         : cfg.nPresetLock;
        if (hLkInh) { SetCheckbox(hLkInh, hasPresets && lockRaw < 0);
                      EnableWindow(hLkInh, hasPresets && !isRenderHost); }
        if (hLkOff) { SetCheckbox(hLkOff, hasPresets && lockRaw == 0);
                      EnableWindow(hLkOff, hasPresets); }
        if (hLkOn)  { SetCheckbox(hLkOn,  hasPresets && lockRaw == 1);
                      EnableWindow(hLkOn,  hasPresets); }
    }
}

// ─── Display Profile Save / Load ─────────────────────────────────────────────

// ── Main-render settings a loaded profile is standing in front of ────────
//
// See PrimaryOverride in engine.h for why these exist. In short: a profile's
// main-render settings apply to the running session only, and the settings
// save has to keep writing the globals underneath them.

void Engine::ApplyPrimaryProfilePreset(const std::wstring& path) {
    if (path.empty()) return;
    // Queued, not called. LoadDisplayProfile runs on the UI thread and the
    // preset must change on the render thread -- the same route
    // DISPLAY_PRESET takes for the primary window.
    //
    // No existence check: this can be a UNC path, and stat-ing one from here
    // would stall the frame loop. A path that is not there fails in LoadPreset
    // exactly as a typed one does, and says so on screen.
    RenderCommand rc;
    rc.cmd = RenderCmd::LoadPresetPath;
    rc.iParam1 = -1;
    rc.fParam = m_fBlendTimeUser;
    rc.sParam = path;
    EnqueueRenderCmd(rc);
}

// ── One value, not two ───────────────────────────────────────────────────
//
// These four used to answer "the user's own value, ignoring what a loaded
// display profile has lent the session". There is no loan any more (#358), so
// they are the members, and they are kept as names rather than inlined because
// eleven call sites read better saying what they mean.
const wchar_t* Engine::GlobalPresetDir() const {
    return m_szPresetDir;
}

void Engine::SetMainRenderInterval(float secs) {
    if (secs < 0.0f) secs = 0.0f;
    m_fTimeBetweenPresets = secs;
    // -1 is this engine's "recompute on the next UpdateTime" flag, so a lowered
    // interval takes effect now rather than waiting out the old deadline. Same
    // reason SET_TIME_BETWEEN_PRESETS does it.
    m_fNextPresetTime = -1.0f;
    m_primaryProfile.fTimeBetweenPresets = -1.0f;
    Config().SetFloat(L"Settings", L"fTimeBetweenPresets", m_fTimeBetweenPresets);
}

void Engine::SetMainRenderOrder(bool sequential) {
    m_bSequentialPresetOrder = sequential;
    m_primaryProfile.nSequentialOrder = -1;
    Config().SetInt(L"Settings", L"bSequentialPresetOrder", sequential ? 1 : 0);
}

void Engine::SetMainRenderLock(bool locked) {
    // The live lock, and only that. bPresetLockOnAtStartup is a PREFERENCE --
    // how the NEXT run begins -- and is written by the Settings window alone.
    SetUserPresetLock(locked);
    m_primaryProfile.nPresetLock = -1;
}

// The "Enabled" checkbox and SET_DISPLAY_ENABLED share this, so there is one
// place that knows disabling an own-process display has to stop its child --
// not two copies that could disagree, the same shape as SetMainRenderLock
// above and the reason it exists.
//
// Reported live: "unchecking 'enabled' while on the display screen doesn't
// disable or stop a child." The checkbox handler only ever set bEnabled and
// never touched the child at all -- not on disable, and, by the same
// omission, not on re-enable either. Confirmed by reading the handler: no
// RequestChildKill/RequestChildSpawn anywhere in it, unlike every other place
// a display's own-process state changes (SET_DISPLAY_MODE's wantMirror and
// wantOff branches, the Displays tab's Mirror/Child radio buttons).
//
// bOwnProcess itself is left untouched, deliberately: "Enabled" is a power
// switch, not a mode selector. Unlike SET_DISPLAY_MODE=<N>,off (a three-way
// exclusive choice where "off" really does mean neither mirror nor child),
// this checkbox sits ABOVE the Mirror/Child radio pair in the UI, so turning
// a display off and back on should return it to whatever it was set to, not
// forget it.
void Engine::SetDisplayEnabled(DisplayOutput& out, bool enabled) {
    out.config.bEnabled = enabled;
    if (out.config.type != DisplayOutputType::Monitor) return;
    if (!out.config.bOwnProcess) return;
    if (enabled) RequestChildSpawn(out.config.szDeviceName);
    else         RequestChildKill(out.config.szDeviceName);
}

float Engine::GlobalTimeBetweenPresets() const {
    return m_fTimeBetweenPresets;
}

bool Engine::GlobalSequentialOrder() const {
    return m_bSequentialPresetOrder;
}

bool Engine::GlobalPresetLocked() const {
    return m_bPresetLockedByUser;
}

bool Engine::SaveDisplayProfile(const wchar_t* filePath,
                                bool bIncludePrimary, bool bIncludeMessaging,
                                bool bSnapshot)
{
    JsonWriter w;
    w.BeginObject();
    // v2 adds per-display child settings and the optional primary/messaging
    // sections. v1 files still load; see LoadDisplayProfile.
    w.Int(L"version", 2);
    w.Float(L"mainWindowOpacity", fOpacity);
    w.Bool(L"mirrorsActive", m_bMirrorsActive);
    w.Int(L"altSMode", m_nAltSMode);
    w.Bool(L"autostartAllDisplays", m_bProfileAutostartAll);
    // v2 readers only know the bool. Keep it truthful for them.
    w.Bool(L"mirrorModeForAltS", m_nAltSMode == ALTS_MIRROR);

    w.BeginArray(L"displays");
    for (auto& out : m_displayOutputs) {
        auto& cfg = out.config;
        w.BeginObject();
        w.String(L"type", cfg.type == DisplayOutputType::Monitor ? L"Monitor" : L"Spout");
        w.String(L"name", cfg.szName);
        w.Bool(L"enabled", cfg.bEnabled);

        if (cfg.type == DisplayOutputType::Monitor) {
            w.String(L"deviceName", cfg.szDeviceName);
            w.Bool(L"fullscreen", cfg.bFullscreen);
            w.Int(L"opacity", cfg.nOpacity);
            w.Bool(L"clickThrough", cfg.bClickThrough);
            // Watermark travels with the profile, like every other per-display
            // style. Leaving it out is not neutral: loading a profile rewrites
            // the whole display config, so an absent key reads as false and
            // silently clears a watermark the user had set.
            w.Bool(L"watermark", cfg.bWatermark);
            // The same argument, for the two keys that were added after it and
            // never got the same treatment (#338). Keep on top and the
            // per-display watermark opacity are per-display styles exactly like
            // the three above, and a profile that cannot carry them is not the
            // "complete display state" it claims to be: set Keep on top, save a
            // profile, load it back, and the setting is gone.
            w.Bool(L"alwaysOnTop", cfg.bAlwaysOnTop);
            w.Int(L"watermarkOpacity", cfg.nWatermarkOpacity);
            w.Bool(L"independentRender", cfg.bIndependentRender);
            // Per-display child settings (forgejo#22)
            // The user's choice, not an automatic isolation -- same reasoning
            // as the OwnProcess key in SaveDisplayOutputSettings. A profile
            // captured while an expensive preset was running would otherwise
            // recreate those children every time it is loaded.
            w.Bool(L"ownProcess", cfg.bOwnProcess);
            w.Bool(L"ownPreset", cfg.bOwnPreset);
            w.String(L"presetDir", cfg.szPresetDir);
            // A snapshot records what this display is PLAYING, so loading the
            // profile puts that preset back rather than whatever was pinned as
            // its startup preset -- usually nothing, in which case the display
            // would come back on a random one. Falls back to the pinned value
            // for a display with no live child to ask.
            {
                std::wstring startup = cfg.szStartupPreset;

                // What this display is PLAYING, whichever tier holds it.
                //
                // One lookup for both callers below (#330). It used to be
                // written out twice, and the snapshot copy asked only
                // bOwnProcess -- the child-process tier. A display on the
                // IN-PROCESS own-preset tier, which is the ordinary way to give
                // a display its own preset since #186 phase 4, matched neither
                // branch, so a snapshot banked cfg.szStartupPreset instead:
                // empty, for any display whose preset was chosen rather than
                // pinned. Loading that profile then wrote the empty value back
                // and every display came up with no preset name at all.
                // DisplayCurrentPresetPath, not another copy of the tier walk:
                // the Displays window needs the same answer to show what is
                // playing (#364), and two hand-written copies of "which tier
                // holds this display's preset" is how this one went wrong.
                if (bSnapshot) {
                    const std::wstring live = DisplayCurrentPresetPath(cfg);
                    if (!live.empty()) startup = live;
                }
                // <Current> is resolved HERE, not left for the load (#284).
                //
                // The other two sentinels are genuine deferred modes: <Random>
                // and <Default> mean something at load time, and something a
                // little different every time, which is the point of them.
                // <Current> does not -- it names the preset playing at the
                // moment it is READ, and a profile is read either at startup,
                // when nothing is playing at all, or later, when "current" is
                // whatever the profile is about to replace. Neither is the
                // preset the user was looking at when they pressed Save.
                if (_wcsicmp(startup.c_str(), kDispModeCurrent) == 0) {
                    std::wstring live = DisplayCurrentPresetPath(cfg);
                    // A display following the main window has no preset of its
                    // own, so <Current> there means the main window's.
                    if (live.empty()) {
                        wchar_t cur[MAX_PATH] = {};
                        CurrentPresetPath(cur, MAX_PATH);
                        live = cur;
                    }
                    if (!live.empty()) startup = live;
                }
                w.String(L"startupPreset", startup.c_str());
            }
            w.Float(L"timeBetweenPresets", cfg.fTimeBetweenPresets);
            w.Bool(L"sequentialOrder", cfg.bSequentialOrder);
            // -1 inherit / 0 unlocked / 1 locked, so a profile can carry "this
            // display never advances" independently of the main window.
            w.Int(L"presetLock", cfg.nPresetLock);
            // How many messages this display shows at once, -1 to inherit the
            // global (#255). Carried so a profile describing a wall of
            // displays carries their message pacing too.
            w.Int(L"msgMaxOnScreen", cfg.nMsgMaxOnScreen);
        } else {
            w.Bool(L"fixedSize", cfg.bFixedSize);
            w.Int(L"width", cfg.nWidth);
            w.Int(L"height", cfg.nHeight);
        }
        w.EndObject();
    }
    w.EndArray();

    // Optional: the primary window's own preset settings. Added so a profile
    // can capture a whole test setup rather than only the display topology.
    //
    // Normally NOT the running preset -- these are the settings that decide
    // what it plays, not an instruction to jump somewhere now. A SNAPSHOT is
    // the deliberate exception: its whole purpose is "put these exact presets
    // back on these exact screens", so there it records what is playing.
    if (bIncludePrimary) {
        w.BeginObject(L"primary");
        // The Main render group OVERRIDES what would otherwise be captured; it
        // does not replace the capture.
        //
        // Both halves matter. A profile saved from this window is a snapshot --
        // "put these presets back on these screens" -- so leaving a field at
        // "(default)" has to go on recording what the main window is actually
        // doing, exactly as it always did and as every display entry above
        // does. Writing only the pinned values would quietly turn every
        // existing profile's main-render section into "no opinion".
        //
        // What the group adds is the ability to say something ELSE: pin a
        // preset the main window is not playing, and that is what the profile
        // carries. Which is the whole request -- those settings were reachable
        // only from the Presets window, where changing one changes the global.
        const auto& pp = m_primaryProfile;

        w.String(L"presetDir",
                 pp.szPresetDir[0] ? pp.szPresetDir : m_szPresetDir);

        // Pinned first, then the running preset for a snapshot, then whatever
        // is merely pinned globally. A display entry resolves in the same
        // order for the same reason.
        std::wstring preset = pp.szStartupPreset;
        if (preset.empty())
            preset = (bSnapshot && m_szCurrentPresetFile[0])
                         ? m_szCurrentPresetFile : m_szPresetStartup;

        // <Current> resolved here rather than at load, exactly as the display
        // entries above now do, and for a reason that bites HARDEST on the
        // main window (#284).
        //
        // The main-render combo offers <Current>, and choosing it stored the
        // sentinel. A profile loaded at STARTUP resolves it before anything is
        // playing, so it fell through to a random pick -- and with
        // bEnablePresetStartup off, the random preset then landed on the one
        // screen the user could see clearly. Reported as "the settings.ini
        // preset takes over on the primary while the non primary displays
        // follow the profile settings and I can't seem to load a profile with
        // the startup primary I want".
        if (_wcsicmp(preset.c_str(), kDispModeCurrent) == 0) {
            wchar_t cur[MAX_PATH] = {};
            CurrentPresetPath(cur, MAX_PATH);
            if (cur[0]) preset = cur;
        }
        w.String(L"startupPreset", preset.c_str());

        // The user's OWN interval and order, not the one a loaded profile has
        // lent this session (#356).
        //
        // These two are the pair engine.cpp's shutdown save singles out: "the
        // persisted setting and the running value ARE one variable, so a
        // loaded display profile really can replace what the shutdown save
        // writes, and GlobalTimeBetweenPresets / GlobalSequentialOrder exist
        // to strip that session-only override back off." Saving a profile is
        // a persist too, and it was reading the raw globals -- so the two
        // paths disagreed about what the setting even was.
        //
        // That disagreement RATCHETS, and cannot be escaped by choosing the
        // value you want:
        //
        //   a profile records sequential -> it loads at startup and lends
        //   sequential to the session -> any snapshot taken afterwards records
        //   sequential again, because it read the loan -> that profile loads
        //   at the next startup ...
        //
        // ...while settings.ini goes on correctly saying random the whole
        // time, because shutdown writes the stash. Measured: live=1,
        // settings.ini=0, saved profile=1, with the order never touched.
        //
        // Reported as "my presets keep switching from random to sequential
        // order when I go to save them on the display window", and then the
        // detail that pins it -- "My live global is random though". It was:
        // the global he was reading is the stash, and the profile was
        // recording the loan.
        //
        // A deliberate choice still records: setting the order clears the
        // override, so the stash becomes the new value and this writes it.
        w.Float(L"timeBetweenPresets",
                pp.fTimeBetweenPresets >= 0.0f ? pp.fTimeBetweenPresets
                                               : GlobalTimeBetweenPresets());
        // -1 inherit / 0 random / 1 sequential. This was a plain JSON bool,
        // which has no room for inherit; asInt reads an old true/false back as
        // 1/0, so a v2 profile keeps meaning what it meant.
        w.Int(L"sequentialOrder",
              pp.nSequentialOrder >= 0 ? pp.nSequentialOrder
                                       : (GlobalSequentialOrder() ? 1 : 0));
        w.Int(L"presetLock",
              pp.nPresetLock >= 0 ? pp.nPresetLock
                                  : (m_bPresetLockedByUser ? 1 : 0));
        w.EndObject();
    }

    // Optional: the messaging setup, message set INLINED.
    //
    // Not a path to messages.ini. The point of this section is that a captured
    // setup keeps meaning the same thing later, and a reference to a file that
    // gets edited next week silently stops doing that. Loading replaces the
    // in-memory message set and writes nothing -- the Messages window persists
    // exactly as it always has.
    if (bIncludeMessaging) {
        w.BeginObject(L"messaging");
        w.Bool(L"autoplay", m_bMsgAutoplay);
        w.Bool(L"sequential", m_bMsgSequential);
        w.Float(L"interval", m_fMsgAutoplayInterval);
        w.Float(L"jitter", m_fMsgAutoplayJitter);
        w.Int(L"maxOnScreen", m_nMsgMaxOnScreen);
        w.Bool(L"overrideRandomFont", m_bMsgOverrideRandomFont);
        w.Bool(L"overrideRandomColor", m_bMsgOverrideRandomColor);
        w.Bool(L"overrideRandomSize", m_bMsgOverrideRandomSize);
        w.Bool(L"overrideRandomEffects", m_bMsgOverrideRandomEffects);
        w.Bool(L"overrideRandomPos", m_bMsgOverrideRandomPos);
        w.Bool(L"overrideRandomGrowth", m_bMsgOverrideRandomGrowth);
        w.Bool(L"overrideSlideIn", m_bMsgOverrideSlideIn);
        w.Bool(L"overrideRandomDuration", m_bMsgOverrideRandomDuration);
        w.Bool(L"overrideShadow", m_bMsgOverrideShadow);
        w.Bool(L"overrideBox", m_bMsgOverrideBox);
        w.Bool(L"overrideApplyHueShift", m_bMsgOverrideApplyHueShift);
        w.Bool(L"overrideRandomHue", m_bMsgOverrideRandomHue);
        w.Bool(L"ignorePerMsgRandom", m_bMsgIgnorePerMsgRandom);

        w.BeginArray(L"messages");
        for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++) {
            const td_custom_msg& m = m_CustomMessage[i];
            if (!m.szText[0]) continue;      // empty slot
            w.BeginObject();
            w.Int(L"slot", i);
            w.String(L"text", m.szText);
            w.Int(L"font", m.nFont);
            w.Float(L"size", m.fSize);
            w.Float(L"x", m.x);
            w.Float(L"y", m.y);
            w.Float(L"randx", m.randx);
            w.Float(L"randy", m.randy);
            w.Float(L"growth", m.growth);
            w.Float(L"time", m.fTime);
            w.Float(L"fade", m.fFade);
            w.Float(L"fadeOut", m.fFadeOut);
            w.Int(L"animProfile", m.nAnimProfile);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }

    w.EndObject();

    return w.SaveToFile(filePath);
}

// ─── Testing-mode snapshot / restore ─────────────────────────────────────────

void Engine::TakeDisplaySnapshot()
{
    m_displaySnapshot.clear();
    for (const auto& o : m_displayOutputs)
        m_displaySnapshot.push_back(o.config);
    m_snapAltSMode                 = m_nAltSMode;
    m_snapMirrorPromptDisabled     = m_bMirrorPromptDisabled;
    m_snapMirrorIndependentDefault = m_bMirrorIndependentDefault;
    m_snapDisableMirrorHud         = m_bDisableMirrorHud;
    m_snapMirrorMaxFps             = m_nMirrorMaxFps.load();
    m_bHaveDisplaySnapshot = true;
    DLOG_INFO("testing mode: snapshotted %zu display output(s) to restore on exit",
              m_displaySnapshot.size());
}

void Engine::RestoreDisplaysAfterTesting()
{
    if (!m_bHaveDisplaySnapshot) {
        mdrop::SetConfigWriteShield(false);
        return;
    }
    m_bHaveDisplaySnapshot = false;

    int changed = 0;
    for (auto& out : m_displayOutputs) {
        const DisplayOutputConfig* was = nullptr;
        for (const auto& snap : m_displaySnapshot) {
            if (snap.type != out.config.type) continue;
            // Monitors are identified by device name; a Spout output by its
            // sender name. Both are owned by the machine or the user, never by
            // the settings this restores.
            const wchar_t* a = (out.config.type == DisplayOutputType::Monitor)
                             ? out.config.szDeviceName : out.config.szName;
            const wchar_t* b = (snap.type == DisplayOutputType::Monitor)
                             ? snap.szDeviceName : snap.szName;
            if (wcscmp(a, b) == 0) { was = &snap; break; }
        }
        // A display that appeared DURING the run has no snapshot, so there is
        // nothing of the user's to put back and it is left exactly as it is.
        if (!was) continue;

        // WHOLESALE, not field by field -- the same fix, for the same reason,
        // as the loader above (#295, #338).
        //
        // This was a hand-written list of sixteen fields, and four settings
        // added since had never been added to it: bAlwaysOnTop and
        // nWatermarkOpacity (#282), nMsgMaxOnScreen (#255), and bWatermark.
        // A test run that changed any of those left it changed, and `changed`
        // stayed 0 so the restore did not even raise the style flags. #330
        // fixed bOwnPreset here one field at a time; this is the third time the
        // list has been the bug, so the list goes.
        //
        // The snapshot is a whole DisplayOutputConfig taken from this same
        // vector, so everything in it is this display's -- except the THREE
        // fields that belong to the ENUMERATION rather than to the user, which
        // are put back afterwards exactly as the loader does. The monitor may
        // have been rearranged or renamed during the run, and the machine's
        // current answer wins over a stale one.
        const bool ownWas = out.config.bOwnProcess;
        const bool enWas  = out.config.bEnabled;

        wchar_t keepName[128];
        CopyTo(keepName, out.config.szName);
        const RECT keepMonitor  = out.config.rcMonitor;
        const RECT keepTestRect = out.config.rcTestWindow;
        out.config = *was;
        CopyTo(out.config.szName, keepName);
        out.config.rcMonitor    = keepMonitor;
        out.config.rcTestWindow = keepTestRect;
        changed++;

        // Restoring the FIELD is not restoring the WORLD. A child process is
        // spawned and retired only by an explicit request -- nothing re-derives
        // it from config -- so a run that promoted a display would otherwise
        // leave a full second instance rendering with no record tying it to a
        // display, which is forgejo#35 by another door.
        if (out.config.type == DisplayOutputType::Monitor) {
            const bool wantChild = out.config.bOwnProcess && out.config.bEnabled;
            const bool hadChild  = ownWas && enWas;
            if (wantChild && !hadChild) RequestChildSpawn(out.config.szDeviceName);
            else if (!wantChild && hadChild) RequestChildKill(out.config.szDeviceName);
        }
    }

    m_nAltSMode                 = m_snapAltSMode;
    m_bMirrorPromptDisabled     = m_snapMirrorPromptDisabled;
    m_bMirrorIndependentDefault = m_snapMirrorIndependentDefault;
    m_bDisableMirrorHud         = m_snapDisableMirrorHud;
    m_nMirrorMaxFps.store(m_snapMirrorMaxFps);

    // Opacity, click-through and fullscreen only reach a live window through
    // ApplyMirrorWindowStyles, which the render thread runs off these flags.
    if (changed) {
        m_bMirrorStylesDirty.store(true);
        m_bMirrorIndepSizeDirty.store(true);
        m_bRaiseMirrorsNextFrame.store(true);
    }

    // LAST. Until this line the file is still protected, so a process killed
    // before this command runs -- which is how most sweeps end -- leaves the
    // user's settings untouched rather than half-restored.
    mdrop::SetConfigWriteShield(false);

    DLOG_WARN("Testing mode OFF: %d display output(s) restored to their "
              "pre-test state, shield lowered", changed);
    if (changed) {
        // Displays, not settings. `changed` used to count the displays whose
        // settings differed, which meant counting differences -- and counting
        // differences is what needed the field list that #338 removed.
        wchar_t msg[128];
        FormatTo(msg, L"Testing mode off - %d display(s) restored", changed);
        AddNotification(msg);
    }
}

bool Engine::GetRenderMonitorDevice(wchar_t (&dev)[32]) const
{
    dev[0] = 0;
    if (m_bMirrorWatermarkActive && m_szWatermarkRenderDevice[0]) {
        CopyTo(dev, m_szWatermarkRenderDevice);
        return true;
    }
    if (!m_lpDX || !m_lpDX->GetHwnd()) return false;
    HMONITOR h = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi = { sizeof(mi) };
    if (!h || !GetMonitorInfoW(h, &mi)) return false;
    CopyTo(dev, mi.szDevice);
    return true;
}

void Engine::ResetDisplayOutputsToDefaults()
{
    // Soft-park rather than destroy, for the same reason LoadDisplayProfile
    // does: WaitForGpu + DestroyWindow from the UI thread races Present and
    // has TDR'd. The render thread hides them from the new flags.
    for (auto& out : m_displayOutputs) {
        if (out.monitorState) {
            out.monitorState->bSoftDisabled = true;
            if (out.monitorState->hWnd)
                ShowWindow(out.monitorState->hWnd, SW_HIDE);
        }
    }

    // Globals, back to the values LoadDisplayOutputSettings would pick when
    // the keys are absent.
    m_bMirrorsActive = false;
    m_nAltSMode = ALTS_STRETCH;
    m_bMirrorPromptDisabled = false;
    m_bMirrorIndependentDefault = false;
    m_bDisableMirrorHud = false;
    m_nMirrorMaxFps.store(0);

    // m_displayOutputsMutex guards the erase/insert below against
    // ChildConfigureNow's read on the child-worker thread. Scoped to just
    // this block, not the whole function, because EnumerateDisplayOutputs()
    // below takes the same (non-recursive) lock itself. #27.
    int oldCount;
    {
        std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);

        // Every Spout output is user-added, so they all go; a fresh install has
        // exactly one, disabled, named MDropDX12. Destroying a Spout sender from
        // this thread is what the Remove button already does.
        oldCount = (int)m_displayOutputs.size();
        for (auto it = m_displayOutputs.begin(); it != m_displayOutputs.end(); ) {
            if (it->config.type == DisplayOutputType::Spout) {
                DestroyDisplayOutput(*it);
                it = m_displayOutputs.erase(it);
            } else {
                ++it;
            }
        }

        // Monitors keep their identity -- they are the machine's, not a setting --
        // but every choice made about one goes back to the struct default. That
        // includes the per-display child fields, so no child is left configured to
        // spawn on the next start.
        for (auto& out : m_displayOutputs) {
            DisplayOutputConfig& c = out.config;
            const DisplayOutputConfig def;
            c.bEnabled = false;
            c.bFullscreen = def.bFullscreen;
            c.nOpacity = def.nOpacity;
            c.bClickThrough = def.bClickThrough;
            c.bIndependentRender = def.bIndependentRender;
            c.bOwnProcess = def.bOwnProcess;
            c.szPresetDir[0] = 0;
            c.szStartupPreset[0] = 0;
            c.fTimeBetweenPresets = def.fTimeBetweenPresets;
            c.bSequentialOrder = def.bSequentialOrder;
            c.nPresetLock = def.nPresetLock;
        }

        DisplayOutput spout;
        spout.config.type = DisplayOutputType::Spout;
        spout.config.bEnabled = false;
        CopyTo(spout.config.szName, L"MDropDX12");
        m_displayOutputs.insert(m_displayOutputs.begin(), std::move(spout));
    }

    bSpoutOut = false;
    bSpoutChanged = true;
    HWND hw = GetPluginWindow();
    if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);

    // SaveDisplayOutputSettings writes Count and then sections 0..Count-1; it
    // never removes the ones above that. Shrinking the list without this leaves
    // DisplayOutput_N behind to be picked up if the count ever grows back.
    for (int i = (int)m_displayOutputs.size(); i < oldCount + 8; i++) {
        wchar_t section[64];
        swprintf(section, 64, L"DisplayOutput_%d", i);
        DisplayCfg().RemoveSection(section);
    }

    // Re-enumerate so the monitor list matches the machine as it is now, then
    // persist, so the reset survives a crash rather than only a clean exit.
    EnumerateDisplayOutputs();
    SaveDisplayOutputSettings();
    ConfigFlushAll();
}

bool Engine::LoadDisplayProfile(const wchar_t* filePath)
{
    JsonValue root = JsonLoadFile(filePath);
    if (!root.isObject()) return false;

    int version = root[L"version"].asInt(0);
    if (version < 1) return false;

    // Soft-park only — never Destroy from the UI thread (WaitForGpu + DestroyWindow
    // races with Present and TDRs). Render thread will show/hide from new flags.
    for (auto& out : m_displayOutputs) {
        if (out.monitorState) {
            out.monitorState->bSoftDisabled = true;
            if (out.monitorState->hWnd)
                ShowWindow(out.monitorState->hWnd, SW_HIDE);
        }
    }

    // Main window opacity
    if (root.has(L"mainWindowOpacity")) {
        fOpacity = root[L"mainWindowOpacity"].asFloat(1.0f);
        if (fOpacity < 0.0f) fOpacity = 0.0f;
        if (fOpacity > 1.0f) fOpacity = 1.0f;
        // Apply via message to render window (owns the HWND)
        HWND hw = GetPluginWindow();
        if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
    }

    // Global flags
    m_bMirrorsActive = root[L"mirrorsActive"].asBool(false);
    // m_nAltSMode is DELIBERATELY NOT restored from the profile.
    //
    // It says what the ALT-S key DOES: stretch, mirror, or load a profile. A
    // profile is one of the things it does. Letting a loaded profile rewrite it
    // is a loop with no way out: set ALT-S to "Load profile", press it, and the
    // profile that loads sets the mode back to whatever it was saved with --
    // so the setting can never survive being used.
    //
    // Reported live: "The setting for Alt-S seems to keep reseting to mirror",
    // and "all the profiles I saved lately I selected load profile". Both are
    // this loop. All 26 saved profiles record altSMode=1 (Mirror) even though
    // the mode was set to Load profile before saving, because the load that
    // preceded each save had already reset it.
    //
    // Same rule as "Preset Lock on Startup": a setting changes when the user
    // changes it, on the window that owns it, and nothing else writes it.
    // The key is still WRITTEN to profiles -- old and new ones stay readable
    // and nothing has to be migrated -- it is simply not applied.

    // A profile is a complete display state, not a patch.
    //
    // This used to only ever ADD Spout outputs and leave any monitor the
    // profile did not name exactly as the previous profile had set it, so
    // A -> B -> A did not round-trip and senders accumulated across every
    // profile ever loaded. Turn everything off first, drop Spout outputs the
    // profile does not name, and let the loop below turn back on only what it
    // asks for. Applies to v1 files too -- this is the bug fix, not a v2
    // feature.
    const auto& displays = root[L"displays"];

    // m_displayOutputsMutex guards this whole rewrite -- the erase just below
    // plus the per-display match/insert loop that follows -- against
    // ChildConfigureNow's read on the child-worker thread. A unique_lock,
    // unlocked explicitly once the loop finishes (rather than a lock_guard
    // scoped to a new brace block), so the diff against the existing
    // structure stays small. Not held across EnumerateDisplayOutputs() above,
    // which takes the same (non-recursive) lock itself. #27.
    std::unique_lock<std::recursive_mutex> lkDisp(m_displayOutputsMutex);

    // Which monitors this profile actually talks about, by device name.
    // "Autostart all displays" (below) must stay scoped to these -- a profile
    // is a complete state, not a patch (forgejo#22), so a monitor the profile
    // is silent about has to be left off by the reset above, not swept up by
    // a blanket enable meant for the displays the profile names.
    std::vector<std::wstring> namedMonitors;
    {
        std::vector<std::wstring> namedSpouts;
        for (size_t i = 0; i < displays.size(); i++) {
            const auto& d = displays.at(i);
            if (d[L"type"].asString(L"") == L"Spout")
                namedSpouts.push_back(d[L"name"].asString(L""));
        }
        for (auto& out : m_displayOutputs) {
            out.config.bEnabled = false;
            out.config.bOwnProcess = false;
        }
        m_displayOutputs.erase(
            std::remove_if(m_displayOutputs.begin(), m_displayOutputs.end(),
                [&](const DisplayOutput& o) {
                    return o.config.type == DisplayOutputType::Spout &&
                           std::find(namedSpouts.begin(), namedSpouts.end(),
                                     std::wstring(o.config.szName)) == namedSpouts.end();
                }),
            m_displayOutputs.end());
    }

    // Apply per-display settings
    for (size_t i = 0; i < displays.size(); i++) {
        const auto& d = displays.at(i);
        std::wstring type = d[L"type"].asString(L"");

        if (type == L"Monitor") {
            std::wstring devName = d[L"deviceName"].asString(L"");
            namedMonitors.push_back(devName);
            bool attached = false;
            // Match to currently enumerated monitor
            for (auto& out : m_displayOutputs) {
                if (out.config.type != DisplayOutputType::Monitor) continue;
                if (devName != out.config.szDeviceName) continue;
                out.config.bEnabled     = d[L"enabled"].asBool(false);
                out.config.bFullscreen  = d[L"fullscreen"].asBool(true);
                out.config.nOpacity     = d[L"opacity"].asInt(100);
                if (out.config.nOpacity < 1) out.config.nOpacity = 1;
                if (out.config.nOpacity > 100) out.config.nOpacity = 100;
                out.config.bClickThrough = d[L"clickThrough"].asBool(false);
                // Defaults to what the display already has, not to false: a
                // profile written before watermark existed has no key, and
                // reading that as "off" would clear a watermark the profile
                // never meant to speak about.
                out.config.bWatermark =
                    d[L"watermark"].asBool(out.config.bWatermark);
                // Same defence, same reason (#338). Every profile saved before
                // this change is silent about these two, and every one of those
                // files is still in use -- so an absent key has to mean "this
                // profile does not speak about it", never "off".
                out.config.bAlwaysOnTop =
                    d[L"alwaysOnTop"].asBool(out.config.bAlwaysOnTop);
                out.config.nWatermarkOpacity =
                    d[L"watermarkOpacity"].asInt(out.config.nWatermarkOpacity);
                out.config.bIndependentRender = d[L"independentRender"].asBool(false);
                // Per-display child settings (forgejo#22). Absent in v1, and
                // the defaults below are what v1 always meant.
                //
                // ownProcess is restored as saved, the same way the
                // settings/displays.json loader above restores it.
                //
                // It used to be DOWNGRADED here to an independent mirror,
                // because the flag meant "spawn a second MDropDX12.exe" and a
                // stale one nobody remembered setting brought up
                // borderless-fullscreen instances every launch. That reason is
                // gone: #186 phase 4 moved own-preset rendering into this
                // process and phase 6 deleted the spawn, so restoring the flag
                // starts no process and covers no monitor that was not already
                // an output.
                //
                // The downgrade meanwhile cost the user the thing profiles are
                // for. bIndependentRender is the mirror sim and CANNOT hold a
                // preset of its own, so every display saved holding its own
                // preset came back rendering the primary's -- two profiles
                // saved with different presets both reloaded as mirrors, while
                // the szStartupPreset read just below sat there inert.
                // 401cb914 removed the twin of this block from the other
                // loader and missed this copy.
                //
                // The key keeps its now-inaccurate name deliberately -- see
                // DisplayHoldsOwnPreset.
                out.config.bOwnProcess = d[L"ownProcess"].asBool(false);
                // Same migration as the settings loader: a profile written
                // before the tiers were separate says ownProcess and means
                // "its own preset, in its own process".
                out.config.bOwnPreset =
                    d[L"ownPreset"].asBool(out.config.bOwnProcess);
                wcsncpy_s(out.config.szPresetDir,
                          d[L"presetDir"].asString(L"").c_str(), _TRUNCATE);
                wcsncpy_s(out.config.szStartupPreset,
                          d[L"startupPreset"].asString(L"").c_str(), _TRUNCATE);
                out.config.fTimeBetweenPresets =
                          d[L"timeBetweenPresets"].asFloat(-1.0f);
                out.config.bSequentialOrder = d[L"sequentialOrder"].asBool(false);
                // -1 inherits the global (#255); absent in profiles written
                // before this existed, which is the same as inheriting.
                out.config.nMsgMaxOnScreen = d[L"msgMaxOnScreen"].asInt(-1);
                if (out.config.nMsgMaxOnScreen < -1)
                    out.config.nMsgMaxOnScreen = -1;
                if (out.config.nMsgMaxOnScreen > NUM_SUPERTEXTS)
                    out.config.nMsgMaxOnScreen = NUM_SUPERTEXTS;
                out.config.nPresetLock = d[L"presetLock"].asInt(-1);
                if (out.config.nPresetLock < -1) out.config.nPresetLock = -1;
                if (out.config.nPresetLock > 1)  out.config.nPresetLock = 1;
                attached = true;
                break;
            }
            // Monitors come and go -- a laptop undocked, a TV switched off. A
            // profile naming one that is not here must not be all-or-nothing:
            // skip that entry, say so, and apply the rest.
            if (!attached)
                DLOG_WARN("display profile: %ls is not attached; skipped",
                          devName.c_str());
        } else if (type == L"Spout") {
            std::wstring name = d[L"name"].asString(L"");
            // Try to match existing Spout output by name
            bool matched = false;
            for (auto& out : m_displayOutputs) {
                if (out.config.type != DisplayOutputType::Spout) continue;
                if (name != out.config.szName) continue;
                out.config.bEnabled   = d[L"enabled"].asBool(false);
                out.config.bFixedSize = d[L"fixedSize"].asBool(false);
                out.config.nWidth     = d[L"width"].asInt(1920);
                out.config.nHeight    = d[L"height"].asInt(1080);
                matched = true;
                break;
            }
            // If no existing Spout output matched, add a new one
            if (!matched && !name.empty()) {
                DisplayOutput newOut;
                newOut.config.type      = DisplayOutputType::Spout;
                newOut.config.bEnabled  = d[L"enabled"].asBool(false);
                newOut.config.bFixedSize = d[L"fixedSize"].asBool(false);
                newOut.config.nWidth    = d[L"width"].asInt(1920);
                newOut.config.nHeight   = d[L"height"].asInt(1080);
                wcsncpy_s(newOut.config.szName, name.c_str(), _TRUNCATE);
                m_displayOutputs.insert(m_displayOutputs.begin(), std::move(newOut));
            }
        }
    }
    lkDisp.unlock();

    // Optional: the primary's own preset settings (v2). Absent from v1 files
    // and from any profile saved without the checkbox.
    if (root.has(L"primary")) {
        const auto& pr = root[L"primary"];

        // The globals underneath, captured before anything is replaced. These
        // settings apply to the RUNNING session only -- Shane's rule is "if
        // the profile doesn't load then the global settings still work", and
        // without this the shutdown save would promote them permanently.

        // Nothing below writes m_primaryProfile.
        //
        // That struct is what the NEXT saved profile should carry -- the pins
        // typed into the "Main render" group -- not a record of the profile
        // last loaded. Populating it here made every later save carry the
        // loaded profile's values instead of the live state, so a snapshot
        // taken after loading a profile and then changing the preset by hand
        // stored the preset that had been replaced.

        // Both fields can hold a MODE rather than a path -- <Random>,
        // <Current>, <Default> -- because the main render's combos offer the
        // same list every other display's do. They are resolved here through
        // the very same functions a child spawn uses, so there is one rule
        // for what a mode means and not a second one that can drift.
        //
        // They used to be written through verbatim. Angle brackets are illegal
        // in a Windows filename, so a profile saved with the combo left on
        // <Current> asked the render thread to load a file called literally
        // "<Current>", which of course failed -- the setting simply never took
        // (reported 2026-09-02: "I have saved profiles and the render preset
        // setting doesn't seem to take ... it's not loading them on profile
        // load").
        DisplayOutputConfig askedFor;   // a carrier for the resolvers, not a display
        wcsncpy_s(askedFor.szPresetDir,     pr[L"presetDir"].asString(L"").c_str(),     _TRUNCATE);
        wcsncpy_s(askedFor.szStartupPreset, pr[L"startupPreset"].asString(L"").c_str(), _TRUNCATE);

        // What is on screen, as a full path, worked out the same way the
        // <Current> resolver does. m_szCurrentPresetFile's comment claims it
        // carries no path and that is not reliably true, so it is tested
        // rather than trusted -- and this must be taken BEFORE m_szPresetDir
        // moves, or a bare name would be joined to the incoming directory
        // instead of the one it actually came from.
        std::wstring currentFull;
        if (m_szCurrentPresetFile[0]) {
            if (wcschr(m_szCurrentPresetFile, L'\\') || wcschr(m_szCurrentPresetFile, L'/')) {
                currentFull = m_szCurrentPresetFile;
            } else {
                currentFull = m_szPresetDir;
                if (!currentFull.empty() && currentFull.back() != L'\\' &&
                    currentFull.back() != L'/')
                    currentFull += L'\\';
                currentFull += m_szCurrentPresetFile;
            }
        }

        // Resolved BEFORE m_szPresetDir moves: <Current> and <Default> are
        // relative to what is playing now, which is the arrangement the
        // profile is replacing.
        const bool wantDir = askedFor.szPresetDir[0] != L'\0';
        const std::wstring dir = wantDir ? ResolveDisplayPresetDir(askedFor)
                                         : std::wstring();
        const bool wantPreset = askedFor.szStartupPreset[0] != L'\0';
        const std::wstring preset =
            wantPreset ? ResolveDisplayStartupPreset(askedFor,
                             wantDir ? dir.c_str() : m_szPresetDir)
                       : std::wstring();

        if (!dir.empty()) {
            wcsncpy_s(m_szPresetDir, dir.c_str(), _TRUNCATE);
            Config().SetString(L"Settings", L"szPresetDir", m_szPresetDir);
        }

        // An empty startupPreset means INHERIT, not "clear the pinned one".
        // It used to be written through unconditionally, so loading any
        // profile that carried no preset wiped szPresetStartup.
        if (!preset.empty()) {
            // The RESOLVED path is pinned, never the mode. m_szPresetStartup
            // is what the next launch opens, and a sentinel sitting in it
            // would fail there exactly as it failed here.
            wcsncpy_s(m_szPresetStartup, preset.c_str(), _TRUNCATE);
            // Tell the startup path this counts (#284). Without it,
            // AllocateMyDX9Stuff tested m_bEnablePresetStartup -- a
            // settings.ini flag a profile has no say over -- and with that off
            // it loaded a random preset over the one just pinned here.
            m_bProfileSuppliedStartupPreset = true;
            // And PLAY it, rather than only pinning it for next launch.
            //
            // Every child display already has its preset put back on load;
            // the main window was the one screen that did not, so a profile
            // restored three quarters of a setup and left the biggest surface
            // on whatever happened to be running.
            //
            // Unless it is already on screen. <Current> resolves to the
            // running preset by definition, and re-loading that would restart
            // it -- a visible glitch, and a reset of the animation clock --
            // to arrive back where it started. Comparing paths rather than
            // special-casing the mode also spares the reload when a profile
            // pins the preset that happens to be playing.
            if (currentFull.empty() || _wcsicmp(preset.c_str(), currentFull.c_str()) != 0)
                ApplyPrimaryProfilePreset(preset);
        }

        // A profile's main-render settings ARE the settings, and are persisted
        // here as any deliberate change would be (#358).
        //
        // They used to be a SESSION-ONLY LOAN: loading a profile stashed the
        // user's own value in m_primaryOverride and lent the profile's to the
        // running session, so shutdown could put the original back. Shane's
        // verdict on that, after it produced the same class of bug twice in
        // one afternoon: "the profile and the gload setting should be the
        // same having them be out of sync is too confusing and I never said
        // to do that."
        //
        // He is right, and not only about the confusion. Two values for one
        // setting means every reader has to pick one, and they did not agree:
        // the shutdown save persisted the stash, SaveDisplayProfile recorded
        // the live value, the Displays radios showed the live value, and only
        // some of the writers cleared the loan. There is no assignment of
        // "which value wins" that makes a two-value setting behave -- #356
        // fixed it in one direction and promptly broke it in the other, which
        // is what finally made the point.
        const float secs = pr[L"timeBetweenPresets"].asFloat(-1.0f);
        if (secs >= 0.0f) {
            m_fTimeBetweenPresets = secs;
            Config().SetFloat(L"Settings", L"fTimeBetweenPresets", m_fTimeBetweenPresets);
        }

        // Read as an int so -1 can mean inherit. A v2 file wrote a JSON bool
        // here; asInt on true/false gives 1/0, which are the two real values,
        // so an old profile keeps meaning what it meant.
        const int order = pr[L"sequentialOrder"].asInt(-1);
        if (order >= 0) {
            m_bSequentialPresetOrder = (order != 0);
            Config().SetInt(L"Settings", L"bSequentialPresetOrder",
                            m_bSequentialPresetOrder ? 1 : 0);
        }

        // "presetLocked" was the v2 name and a bool; "presetLock" is the
        // tri-state. Fall back so an older profile still locks what it locked.
        //
        // The lock stays as it was: a profile moves only the RUNTIME lock and
        // never bPresetLockOnAtStartup, which is a separate preference about
        // how the next run begins. That was already true and is not the
        // two-value problem above -- see the note in engine.cpp's shutdown
        // save, which explains why writing the resolved lock there "promoted
        // every runtime unlock into 'start unlocked'".
        int lock = pr[L"presetLock"].asInt(-1);
        if (lock < 0 && pr.has(L"presetLocked"))
            lock = pr[L"presetLocked"].asBool(false) ? 1 : 0;
        if (lock >= 0)
            SetUserPresetLock(lock != 0);

        m_fNextPresetTime = -1.0f;   // recompute; see SET_TIME_BETWEEN_PRESETS
    }

    // Optional: messaging (v2). Applies to the IN-MEMORY set only; nothing here
    // writes messages.ini.
    if (root.has(L"messaging")) {
        const auto& ms = root[L"messaging"];
        m_bMsgAutoplay         = ms[L"autoplay"].asBool(m_bMsgAutoplay);
        m_bMsgSequential       = ms[L"sequential"].asBool(m_bMsgSequential);
        m_fMsgAutoplayInterval = ms[L"interval"].asFloat(m_fMsgAutoplayInterval);
        m_fMsgAutoplayJitter   = ms[L"jitter"].asFloat(m_fMsgAutoplayJitter);
        m_nMsgMaxOnScreen      = ms[L"maxOnScreen"].asInt(m_nMsgMaxOnScreen);
        m_bMsgOverrideRandomFont     = ms[L"overrideRandomFont"].asBool(m_bMsgOverrideRandomFont);
        m_bMsgOverrideRandomColor    = ms[L"overrideRandomColor"].asBool(m_bMsgOverrideRandomColor);
        m_bMsgOverrideRandomSize     = ms[L"overrideRandomSize"].asBool(m_bMsgOverrideRandomSize);
        m_bMsgOverrideRandomEffects  = ms[L"overrideRandomEffects"].asBool(m_bMsgOverrideRandomEffects);
        m_bMsgOverrideRandomPos      = ms[L"overrideRandomPos"].asBool(m_bMsgOverrideRandomPos);
        m_bMsgOverrideRandomGrowth   = ms[L"overrideRandomGrowth"].asBool(m_bMsgOverrideRandomGrowth);
        m_bMsgOverrideSlideIn        = ms[L"overrideSlideIn"].asBool(m_bMsgOverrideSlideIn);
        m_bMsgOverrideRandomDuration = ms[L"overrideRandomDuration"].asBool(m_bMsgOverrideRandomDuration);
        m_bMsgOverrideShadow         = ms[L"overrideShadow"].asBool(m_bMsgOverrideShadow);
        m_bMsgOverrideBox            = ms[L"overrideBox"].asBool(m_bMsgOverrideBox);
        m_bMsgOverrideApplyHueShift  = ms[L"overrideApplyHueShift"].asBool(m_bMsgOverrideApplyHueShift);
        m_bMsgOverrideRandomHue      = ms[L"overrideRandomHue"].asBool(m_bMsgOverrideRandomHue);
        m_bMsgIgnorePerMsgRandom     = ms[L"ignorePerMsgRandom"].asBool(m_bMsgIgnorePerMsgRandom);

        // An empty array is indistinguishable from a truncated file, and a
        // profile must not be able to silently empty the user's message list.
        const auto& msgs = ms[L"messages"];
        if (msgs.size() > 0) {
            for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++)
                m_CustomMessage[i].szText[0] = L'\0';
            for (size_t i = 0; i < msgs.size(); i++) {
                const auto& m = msgs.at(i);
                const int slot = m[L"slot"].asInt(-1);
                if (slot < 0 || slot >= MAX_CUSTOM_MESSAGES) continue;
                td_custom_msg& dst = m_CustomMessage[slot];
                wcsncpy_s(dst.szText, m[L"text"].asString(L"").c_str(), _TRUNCATE);
                dst.nFont    = m[L"font"].asInt(dst.nFont);
                dst.fSize    = m[L"size"].asFloat(dst.fSize);
                dst.x        = m[L"x"].asFloat(dst.x);
                dst.y        = m[L"y"].asFloat(dst.y);
                dst.randx    = m[L"randx"].asFloat(dst.randx);
                dst.randy    = m[L"randy"].asFloat(dst.randy);
                dst.growth   = m[L"growth"].asFloat(dst.growth);
                dst.fTime    = m[L"time"].asFloat(dst.fTime);
                dst.fFade    = m[L"fade"].asFloat(dst.fFade);
                dst.fFadeOut = m[L"fadeOut"].asFloat(dst.fFadeOut);
                dst.nAnimProfile = m[L"animProfile"].asInt(dst.nAnimProfile);
            }
        } else {
            DLOG_WARN("display profile: messaging section has no messages; "
                      "the existing message set was left alone");
        }
    }

    // "Autostart all displays": every display the profile names comes on,
    // not only the ones that were running when it was saved. Applied here,
    // before the pass below reads bEnabled to decide what to spawn and what to
    // kill, so one flag governs both.
    //
    // Monitors only. A Spout sender is not a screen and turning one on because
    // a profile mentioned it would start publishing a texture nobody asked for.
    //
    // Scoped to namedMonitors (forgejo#86): this used to loop over every
    // enumerated monitor, so a monitor the profile never mentioned -- one it
    // deliberately omits, meaning "leave it off" per the reset above -- got
    // swept up and force-enabled anyway the moment the checkbox was on for
    // ANY reason, including a stale true carried into this profile file from
    // an earlier save the user never meant to affect this one. "The profile
    // names it" is the actual contract the field's own comment describes;
    // the loop just wasn't enforcing it.
    m_bProfileAutostartAll = root[L"autostartAllDisplays"].asBool(false);
    if (m_bProfileAutostartAll) {
        int on = 0;
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor) continue;
            if (out.config.bEnabled) continue;
            if (std::find(namedMonitors.begin(), namedMonitors.end(),
                          std::wstring(out.config.szDeviceName)) == namedMonitors.end())
                continue;
            out.config.bEnabled = true;
            on++;
        }
        if (on) DLOG_INFO("display profile: autostart turned on %d display(s)", on);
    }

    // Bring the running children into line with what the profile asked for.
    // Without this a profile can describe children but not produce them.
    if (!m_bChildMode) {
        for (auto& out : m_displayOutputs) {
            if (out.config.type != DisplayOutputType::Monitor) continue;
            if (!(out.config.bOwnProcess && out.config.bEnabled)) {
                RequestChildKill(out.config.szDeviceName);
                continue;
            }

            // A display that ALREADY has a child gets nothing from a spawn
            // request: ChildSpawnNow dedupes and returns, so the profile's
            // preset directory, interval, order, lock and startup preset were
            // all silently dropped for exactly the displays that were already
            // running. Loading a profile is an explicit "make it this state",
            // so reconfigure the existing child instead of asking for one.
            ChildInstance existing;
            const bool haveChild = GetChildInfo(out.config.szDeviceName, existing);
            if (!haveChild) {
                RequestChildSpawn(out.config.szDeviceName);
                continue;   // a fresh spawn configures itself, preset included
            }

            RequestChildReconfigure(out.config.szDeviceName);
            // Reconfigure deliberately does NOT re-send the startup preset --
            // five Displays-UI controls call it, and yanking a running display
            // back to its startup preset because a checkbox was ticked is the
            // bug that rule exists to prevent. A profile load is the opposite
            // case: putting that preset back is the whole point, and it is what
            // makes a snapshot restore the set of presets it captured.
            //
            // Resolved, not sent raw: the field may hold <Random> or <Current>,
            // and a child handed one of those would try to open a file called
            // "<Random>". Same two calls, in the same order, as a fresh spawn.
            {
                const std::wstring d = ResolveDisplayPresetDir(out.config);
                const std::wstring s =
                    ResolveDisplayStartupPreset(out.config, d.c_str());
                if (!s.empty())
                    RequestChildSend(out.config.szDeviceName,
                                     (std::wstring(L"PRESET=") + s).c_str());
            }
        }
    }

    // Request render-thread mirror style refresh and save to INI
    m_bMirrorStylesDirty.store(true);
    SaveDisplayOutputSettings();
    RefreshDisplaysTab();
    // A loaded profile has to MATCH what the window shows, or the Displays
    // window becomes a liar: RefreshDisplaysTab rebuilds the list rows only,
    // so every control in the "Selected display" group -- Enabled, Render
    // mode, Preset dir, Startup, Cycle, Lock -- went on showing the values
    // from before the load. Re-applying the same selection repopulates them
    // from the config the profile just wrote.
    //
    // Here and not inside RefreshDisplaysTab, which also runs on a 1-second
    // child-status timer: repopulating edit and combo controls once a second
    // would fight the user typing a path into them.
    UpdateDisplaysTabSelection(m_nDisplaysTabSel);
    return true;
}

//======================================================================
// Spout output — sender lifecycle and control
//======================================================================

bool Engine::OpenSender(unsigned int width, unsigned int height) {
  DLOG_INFO("Spout: OpenSender(%d, %d)", width, height);

  // Close existing sender
  SpoutReleaseWraps();
  if (bInitialized) {
    spoutsender.Close();
    bInitialized = false;
  }

  if (!m_lpDX || !m_lpDX->m_device || !m_lpDX->m_commandQueue) {
    DebugLogA("Spout: OpenSender failed - no DX12 device/queue", LOG_ERROR);
    return false;
  }

  // Give the sender a name
  spoutsender.SetName(WinampSenderName);

  // Initialize SpoutDX12 with our DX12 device + command queue
  if (!spoutsender.Open(m_lpDX->m_device.Get(),
                        m_lpDX->m_commandQueue.Get())) {
    DebugLogA("Spout: Open failed", LOG_ERROR);
    return false;
  }

  // Wrap each swap chain backbuffer for DX11 access
  for (int n = 0; n < DXC_FRAME_COUNT; n++) {
    if (!spoutsender.WrapResource(
            m_lpDX->m_renderTargets[n].Get(),
            &m_pWrappedBackBuffers[n],
            D3D12_RESOURCE_STATE_RENDER_TARGET)) {
      DebugLogA("Spout: WrapResource failed for backbuffer", LOG_ERROR);
      SpoutReleaseWraps();
      spoutsender.Close();
      return false;
    }
  }

  g_Width = width;
  g_Height = height;
  bSpoutOut = true;
  bInitialized = true;
  m_bSpoutDX12Ready = true;

  DebugLogA("Spout: DX12 sender initialized successfully");

  return true;

} // end OpenSender

// Release wrapped DX12 backbuffers
void Engine::SpoutReleaseWraps() {
  for (auto& w : m_pWrappedBackBuffers) {
    if (w) { w->Release(); w = nullptr; }
  }
  m_bSpoutDX12Ready = false;
}

int Engine::ToggleSpout() {
  bSpoutChanged = true; // write config on exit
  bSpoutOut = !bSpoutOut;
  if (bSpoutOut) {
    AddNotification(L"Spout output enabled");
  }
  else {
    AddNotification(L"Spout output disabled");
  }

  // Sync first Spout output in m_displayOutputs
  for (auto& o : m_displayOutputs) {
    if (o.config.type == DisplayOutputType::Spout) {
      o.config.bEnabled = bSpoutOut;
      if (!bSpoutOut && o.spoutState) {
        DestroyDisplayOutput(o);
      }
      break;
    }
  }

  SetSpoutFixedSize(false, false);

  if (bInitialized || m_bSpoutDX12Ready) {
    SpoutReleaseWraps();
    spoutsender.Close();
    bInitialized = false;
  }

  ResetBufferAndFonts();
  SendSettingsInfoToMDropDX12Remote();
  return 0;
}

int Engine::SetSpoutFixedSize(bool toggleSwitch, bool showNotifications) {
  bSpoutChanged = true; // write config on exit
  if (toggleSwitch) {
    bSpoutFixedSize = !bSpoutFixedSize;
  }
  // Sync first Spout output in m_displayOutputs
  for (auto& o : m_displayOutputs) {
    if (o.config.type == DisplayOutputType::Spout) {
      o.config.bFixedSize = bSpoutFixedSize;
      o.config.nWidth = nSpoutFixedWidth;
      o.config.nHeight = nSpoutFixedHeight;
      break;
    }
  }
  if (IsSpoutActiveAndFixed()) {
    if (toggleSwitch && showNotifications) {
      std::wstring msg = L"Fixed Spout output size enabled ("
        + std::to_wstring(nSpoutFixedWidth) + L"x"
        + std::to_wstring(nSpoutFixedHeight) + L")";
      AddNotification(msg.data());
    }
    else if (showNotifications) {
      std::wstring msg = L"Spout output size set to "
        + std::to_wstring(nSpoutFixedWidth) + L"x"
        + std::to_wstring(nSpoutFixedHeight);
      AddNotification(msg.data());
    }
    // DX12 TODO: Fixed-size Spout requires a separate render target + copy/scale.
    // For now, Spout sends at window resolution regardless of fixed-size setting.
    ResetBufferAndFonts();
  }
  else {
    // bSpoutFixedSize OR bSpoutOut is false
    if (toggleSwitch && showNotifications && bSpoutOut) {
      AddNotification(L"Fixed Spout output size disabled");
    }
    ResetBufferAndFonts();
  }
  SendSettingsInfoToMDropDX12Remote();
  return 0;
}

// ─── The display-profile folder ────────────────────────────────────────────────

std::wstring Engine::DisplayProfileDir() const {
    return profiles::DirFor(m_szBaseDir, profiles::Kind::Display);
}

// resources/displayprofiles/ -> resources/profiles/display/, once.
//
// Four kinds of profile stored themselves four different ways; the layout is
// stated once in profile_paths.h now, and this is display's move onto it. The
// files were already one per profile with timestamped names, so this is a
// change of address and nothing else: no format conversion, no rewrite.
//
// MOVED, and only where the move is confirmed. MoveFileExW answers true or the
// original stays exactly where it was; anything that will not move is left
// behind and logged rather than quietly lost, and the old directory is removed
// only when it is EMPTY -- so whatever did not move keeps the folder alive to
// be noticed instead of vanishing with it.
void Engine::MigrateDisplayProfiles() {
    std::wstring oldDir = m_szBaseDir;
    if (!oldDir.empty() && oldDir.back() != L'\\' && oldDir.back() != L'/')
        oldDir += L'\\';
    oldDir += L"resources\\displayprofiles\\";

    const DWORD attr = GetFileAttributesW(oldDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return;                   // already moved, or there never was one

    const std::wstring newDir = DisplayProfileDir();
    if (!profiles::EnsureDirPath(newDir)) {
        DLOG_ERROR("display profiles: could not create %ls", newDir.c_str());
        return;
    }

    int moved = 0, kept = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((oldDir + L"*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring from = oldDir + fd.cFileName;
            const std::wstring to = newDir + fd.cFileName;
            // Never overwrite: a name already there was not put there by this
            // migration, and it wins.
            if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DLOG_WARN("display profiles: %ls exists; leaving the old copy",
                          to.c_str());
                kept++;
                continue;
            }
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED)) {
                moved++;
            } else {
                kept++;
                DLOG_ERROR("display profiles: could not move %ls", from.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (kept == 0 && RemoveDirectoryW(oldDir.c_str()))
        DLOG_INFO("display profiles: moved %d into %ls, old folder removed",
                  moved, newDir.c_str());
    else if (moved || kept)
        DLOG_INFO("display profiles: moved %d, left %d in %ls",
                  moved, kept, oldDir.c_str());

    // A startup profile stored as an absolute path into the old folder would
    // now point at nothing. Re-point it at the same leaf in the new one.
    if (m_szStartupDisplayProfile[0] &&
        wcsstr(m_szStartupDisplayProfile, L"displayprofiles")) {
        const wchar_t* leaf = wcsrchr(m_szStartupDisplayProfile, L'\\');
        leaf = leaf ? leaf + 1 : m_szStartupDisplayProfile;
        const std::wstring repointed = newDir + leaf;
        wcsncpy_s(m_szStartupDisplayProfile, repointed.c_str(), _TRUNCATE);
        DLOG_INFO("display profiles: startup profile re-pointed to %ls",
                  m_szStartupDisplayProfile);
    }
}

std::wstring Engine::ResolveDisplayProfilePath(const wchar_t* nameOrPath) const {
    if (!nameOrPath || !*nameOrPath) return std::wstring();
    std::wstring s = nameOrPath;
    // Anything that looks like a path is one. Profiles saved before the folder
    // existed are absolute, and they must keep loading.
    if (s.find(L'\\') != std::wstring::npos || s.find(L'/') != std::wstring::npos ||
        (s.size() > 1 && s[1] == L':'))
        return s;
    if (s.size() < 5 || _wcsicmp(s.c_str() + s.size() - 5, L".json") != 0)
        s += L".json";
    return DisplayProfileDir() + s;
}

std::vector<std::wstring> Engine::ListDisplayProfiles() const {
    std::vector<std::wstring> out;
    const std::wstring dir = DisplayProfileDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // A leading underscore marks bookkeeping rather than a profile anybody
        // chose -- profiles::ListDir has said so since the folder layout was
        // written down, and this listing, which rolls its own enumeration, was
        // the one place that did not honour it. _idle_restore.json is the first
        // file to depend on that: it is the pre-idle state banked so waking up
        // can reload it, and it has no business appearing in the Settings combo
        // or in StepDisplayProfile's walk.
        if (fd.cFileName[0] == L'_') continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // By name, which is by age: SaveDisplayProfileSnapshot timestamps them
    // yyyy-MM-dd_HH-mm-ss precisely so the two orders are the same one.
    std::sort(out.begin(), out.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return _wcsicmp(a.c_str(), b.c_str()) < 0;
              });
    return out;
}

std::wstring Engine::SuggestedDisplayProfileName() const {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    FormatTo(name, L"%04d-%02d-%02d_%02d-%02d-%02d.json",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return name;
}

std::wstring Engine::SaveDisplayProfileSnapshot() {
    const std::wstring dir = DisplayProfileDir();
    // One level at a time: resources/ may exist while displayprofiles/ does not.
    {
        std::wstring res = m_szBaseDir;
        if (!res.empty() && res.back() != L'\\' && res.back() != L'/') res += L'\\';
        res += L"resources";
        CreateDirectoryW(res.c_str(), NULL);
        CreateDirectoryW(dir.c_str(), NULL);
    }

    const std::wstring path = dir + SuggestedDisplayProfileName();
    // primary and messaging both included: the point of a snapshot is to put
    // the whole thing back, and leaving the main window out of "all my screens"
    // would be a strange place to draw the line.
    if (!SaveDisplayProfile(path.c_str(), true, true, true))
        return std::wstring();
    wcsncpy_s(m_szStartupDisplayProfile, path.c_str(), _TRUNCATE);
    DLOG_INFO("display profile snapshot saved: %ls", path.c_str());
    return path;
}

bool Engine::LoadDefaultDisplayProfile() {
    if (!m_szStartupDisplayProfile[0]) {
        AddNotification((wchar_t*)L"No default display profile set");
        return false;
    }
    const std::wstring path = ResolveDisplayProfilePath(m_szStartupDisplayProfile);
    // Through the render command queue, for the same reason the IPC verb is:
    // loading mutates m_displayOutputs and spawns and kills children, and the
    // render thread walks that vector every frame.
    RenderCommand rc;
    rc.cmd = RenderCmd::LoadDisplayProfile;
    rc.sParam = path;
    EnqueueRenderCmd(rc);
    return true;
}

bool Engine::StepDisplayProfile(int delta) {
    std::vector<std::wstring> all = ListDisplayProfiles();
    if (all.empty()) {
        AddNotification((wchar_t*)L"No display profiles saved");
        return false;
    }

    // Where we are now, by file name. Unknown -- a profile loaded from outside
    // the folder, or none yet -- starts the walk at the first entry, so the
    // first press always lands somewhere rather than doing nothing.
    int idx = -1;
    if (m_szStartupDisplayProfile[0]) {
        const wchar_t* leaf = wcsrchr(m_szStartupDisplayProfile, L'\\');
        leaf = leaf ? leaf + 1 : m_szStartupDisplayProfile;
        for (size_t i = 0; i < all.size(); i++)
            if (_wcsicmp(all[i].c_str(), leaf) == 0) { idx = (int)i; break; }
    }
    const int n = (int)all.size();
    idx = (idx < 0) ? 0 : ((idx + delta) % n + n) % n;

    const std::wstring path = DisplayProfileDir() + all[idx];
    wcsncpy_s(m_szStartupDisplayProfile, path.c_str(), _TRUNCATE);
    RenderCommand rc;
    rc.cmd = RenderCmd::LoadDisplayProfile;
    rc.sParam = path;
    EnqueueRenderCmd(rc);
    AddNotification((wchar_t*)all[idx].c_str());
    return true;
}

bool Engine::DeleteDisplayProfile(const std::wstring& nameOrPath) {
    const std::wstring path = ResolveDisplayProfilePath(nameOrPath.c_str());
    if (path.empty()) return false;

    // The Recycle Bin, not DeleteFileW. A display profile is minutes of
    // arranging screens by hand and there is no other copy of it; the
    // annotations window already deletes preset files this way, for the same
    // reason. SHFileOperationW wants a double-null-terminated list.
    std::vector<wchar_t> buf(path.begin(), path.end());
    buf.push_back(0);
    buf.push_back(0);

    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = buf.data();
    // SILENT/NOERRORUI because this runs on the render thread for the IPC verb
    // and on a tool window's thread for the button -- neither may block on a
    // shell dialog. The caller asks the question.
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted)
        return false;

    // Never leave the startup pointer aimed at a file that is gone: startup
    // would try to load it, fail, and come up with no profile at all with
    // nothing said about why.
    if (_wcsicmp(m_szStartupDisplayProfile, path.c_str()) == 0) {
        m_szStartupDisplayProfile[0] = 0;
        m_bLoadDisplayProfileOnStart = false;
        ConfigWriteOverride allow;  // a write the user explicitly asked for
        Config().SetString(L"Settings", L"szStartupDisplayProfile", L"");
        Config().SetInt(L"Settings", L"bLoadDisplayProfileOnStart", 0);
        ConfigFlushAll();
    }
    return true;
}

// ── Named display-profile IPC, addressed like MIXER_PROFILE_*/VFX_PROFILE_* ──
//
// forgejo#95. The older SAVE_DISPLAY_PROFILE=/LOAD_DISPLAY_PROFILE= take a
// full path, which the harness and scripts already know how to build. A
// remote does not: it has no way to learn the profiles folder, so it cannot
// construct one, and nothing tells it what already exists.
bool Engine::HandleDisplayProfileIPC(const wchar_t* sMessage) {
    extern PipeServer g_pipeServer;

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_LIST")) {
        // resources/displayprofiles/ (DisplayProfileDir()) only -- the same
        // folder ListDisplayProfiles() already scans, nowhere else.
        std::wstring reply;
        for (const std::wstring& leaf : ListDisplayProfiles()) {
            if (!reply.empty()) reply += L"\n";
            // Display profiles carry no separate stored "name" the way mixer
            // profiles do (MixerProfileName reads a "name" field); the leaf
            // IS the name, timestamped, so "name" here is just the leaf with
            // its extension trimmed for display.
            std::wstring name = leaf;
            if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".json") == 0)
                name.resize(name.size() - 5);
            wchar_t line[MAX_PATH * 2];
            FormatTo(line, L"DISPLAY_PROFILE|file=%s|name=%s", leaf.c_str(), name.c_str());
            reply += line;
        }
        if (reply.empty()) reply = L"DISPLAY_PROFILE_NONE";
        g_pipeServer.Send(reply.c_str());
        return true;
    }

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_SAVE=")) {
        // DISPLAY_PROFILE_SAVE=<name>[|primary=1][|messaging=1]
        // An empty name banks a timestamp, matching MIXER_PROFILE_SAVE=.
        //
        // Deliberately does NOT touch the startup profile. The older
        // SAVE_DISPLAY_PROFILE= still does -- see the fix just below it in
        // HandleDisplayModeIPC -- and doing the same here would repeat
        // exactly the surprise this issue asked to have fixed: a save must
        // not silently decide what loads at boot.
        std::wstring arg(sMessage + 21);
        const bool primary   = arg.find(L"|primary=1") != std::wstring::npos;
        const bool messaging = arg.find(L"|messaging=1") != std::wstring::npos;
        const size_t bar = arg.find(L'|');
        if (bar != std::wstring::npos) arg = arg.substr(0, bar);
        const std::wstring name = arg.empty() ? SuggestedDisplayProfileName() : arg;

        profiles::EnsureDirPath(DisplayProfileDir());
        const std::wstring path = ResolveDisplayProfilePath(name.c_str());
        if (path.empty() || !SaveDisplayProfile(path.c_str(), primary, messaging, false)) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=save_failed");
            return true;
        }
        g_pipeServer.Send((std::wstring(L"DISPLAY_PROFILE_SAVED|file=") +
                           FileNameOnly(path.c_str())).c_str());
        return true;
    }

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_LOAD=")) {
        const std::wstring name(sMessage + 21);
        if (name.empty()) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=malformed");
            return true;
        }
        const std::wstring path = ResolveDisplayProfilePath(name.c_str());
        if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=unknown_profile");
            return true;
        }
        // Through the render command queue, same as LOAD_DISPLAY_PROFILE= and
        // for the same reason: loading mutates m_displayOutputs and spawns
        // and kills children, which must not happen off the render thread.
        // The eventual PROFILE_LOADED=<path> / PROFILE_LOADED=ERROR reply
        // comes from that same shared handler (engine_input.cpp), including
        // its existing "loading also becomes the new startup default"
        // behavior -- unchanged here. Only DISPLAY_PROFILE_SAVE='s side
        // effect was the one this issue asked to remove; decoupling LOAD
        // from the default too would change StepDisplayProfile's and
        // LoadDefaultDisplayProfile's shared plumbing well beyond what was
        // asked. DISPLAY_PROFILE_STARTUP= below is the deliberate way to
        // change the default without loading anything.
        RenderCommand rc;
        rc.cmd = RenderCmd::LoadDisplayProfile;
        rc.sParam = path;
        EnqueueRenderCmd(rc);
        return true;
    }

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_DELETE=")) {
        const std::wstring name(sMessage + 23);
        if (name.empty()) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=malformed");
            return true;
        }
        if (!DeleteDisplayProfile(name)) {
            g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=unknown_profile");
            return true;
        }
        g_pipeServer.Send((std::wstring(L"DISPLAY_PROFILE_DELETED|file=") + name).c_str());
        return true;
    }

    // DISPLAY_PROFILE_STARTUP=<name>  sets both the name and enables it.
    // DISPLAY_PROFILE_STARTUP=        (empty) disables it; the name is kept,
    //                                 so re-enabling later needs no name again.
    // DISPLAY_PROFILE_STARTUP         (no '=') queries both without changing
    //                                 either -- checked FIRST would be wrong,
    //                                 since MSG_IS is a prefix match and the
    //                                 bare form is a prefix of the '=' form.
    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_STARTUP=")) {
        // One verb for both pieces of state (Shane's comment on forgejo#95):
        // WHICH profile and WHETHER one loads at all are only meaningful
        // together, and "set this to load at start" is one intention.
        // Neither loads nor saves anything -- a remote can point the default
        // at an existing profile without disturbing what is on screen now.
        const std::wstring name(sMessage + 24);
        if (name.empty()) {
            m_bLoadDisplayProfileOnStart = false;
        } else {
            const std::wstring path = ResolveDisplayProfilePath(name.c_str());
            if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                g_pipeServer.Send(L"DISPLAY_PROFILE_ERR|reason=unknown_profile");
                return true;
            }
            wcsncpy_s(m_szStartupDisplayProfile, path.c_str(), _TRUNCATE);
            m_bLoadDisplayProfileOnStart = true;
        }
        // Same section/keys and the same SetInt (not SetBool) the checkbox
        // itself uses (engine_displays_ui.cpp), so either surface reads back
        // what the other just wrote.
        Config().SetString(L"Settings", L"szStartupDisplayProfile", m_szStartupDisplayProfile);
        Config().SetInt(L"Settings", L"bLoadDisplayProfileOnStart",
                        m_bLoadDisplayProfileOnStart ? 1 : 0);
        wchar_t buf[MAX_PATH + 32];
        FormatTo(buf, L"DISPLAY_PROFILE_STARTUP|file=%s|enabled=%d",
                 FileNameOnly(m_szStartupDisplayProfile), m_bLoadDisplayProfileOnStart ? 1 : 0);
        g_pipeServer.Send(buf);
        return true;
    }

    if (MSG_IS(sMessage, L"DISPLAY_PROFILE_STARTUP")) {
        wchar_t buf[MAX_PATH + 32];
        FormatTo(buf, L"DISPLAY_PROFILE_STARTUP|file=%s|enabled=%d",
                 FileNameOnly(m_szStartupDisplayProfile), m_bLoadDisplayProfileOnStart ? 1 : 0);
        g_pipeServer.Send(buf);
        return true;
    }

    return false;
}

} // namespace mdrop
