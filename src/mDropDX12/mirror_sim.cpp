// mirror_sim.cpp — independent mirror simulation contexts.
//
// Spec: docs/superpowers/specs/2026-08-21-independent-mirror-sims-design.md
// Each independent mirror size/orientation group runs its own simulation:
// own preset states (own EEL VMs, per-context reg/gmegabuf), own warp mesh,
// own frame counter and blend timeline, fed by an immutable audio snapshot
// from the primary. The wall clock (GetTime()) is shared — time flows the
// same everywhere; independence is in the per-FRAME integration.
//
// Threading contract (see engine.h at m_stateImportMutex):
//  - MirrorSimAdoptPreset / MirrorSimStepFrame run on the mirror thread with
//    NO engine mutex held. They touch only ctx-owned state, the preset
//    bundle (own mutex), the audio snapshot (own mutex), and read-only or
//    benign Engine scalars.
//  - MirrorSimApplyBlendPattern runs on the mirror thread UNDER the engine
//    mutex (record path): it briefly swaps m_vertinfo/m_fAspect to reuse the
//    1100-line blend-pattern generators at ctx aspect. Rare (preset loads).

#include "engine.h"
#include "utility.h"

extern void GetFast_CLEAR(); // state.cpp — GetFast line cache invalidation

namespace mdrop {

// ─── Audio snapshot ──────────────────────────────────────────────────────────
// The render thread publishes once per frame after sound analysis; sim
// threads copy at their frame start. Full-struct copy (~60 KB) is trivial at
// frame rates and keeps every EEL audio feed identical to the primary's.

void Engine::PublishAudioSnapshot()
{
    std::lock_guard<std::mutex> lk(m_audioSnapMutex);
    m_audioSnap.snd = mysound;
    m_audioSnap.serial++;
}

void Engine::CopyAudioSnapshot(AudioSnapshot& dst)
{
    std::lock_guard<std::mutex> lk(m_audioSnapMutex);
    dst = m_audioSnap;
}

// ─── Preset bundle ───────────────────────────────────────────────────────────

void Engine::PublishPresetBundle(bool isMilk2, float blendTime)
{
    std::lock_guard<std::mutex> lk(m_presetBundleMutex);
    m_presetBundle.path = m_szCurrentPresetFile;
    m_presetBundle.isMilk2 = isMilk2;
    m_presetBundle.milk2Body1 = std::move(m_pendingMilk2Body1);
    m_presetBundle.milk2Body2 = std::move(m_pendingMilk2Body2);
    m_presetBundle.milk2Progress = m_fMilk2FrozenProgress;
    m_presetBundle.milk2HasRandoms = m_bMilk2HasRandoms;
    for (int i = 0; i < 5; i++)
        m_presetBundle.milk2Random[i] = m_fMilk2Random[i];
    m_presetBundle.blendTime = blendTime;
    m_presetBundle.version = m_presetBundleVersion.fetch_add(1) + 1;
    DLOG_INFO("MirrorSim: bundle v%u published (%s)",
              m_presetBundle.version, isMilk2 ? "milk2" : "milk");
}

// ─── Grid geometry ───────────────────────────────────────────────────────────
// Mirrors Engine::AllocateMyDX9Stuff's grid init (engine.cpp:3162-3197) at
// the context's aspect. Texel offsets are zero — this path is DX12-only.

void Engine::MirrorSimEnsureGrid(MirrorSimContext& c, int w, int h,
                                 int aspectW, int aspectH)
{
    if (w <= 0 || h <= 0)
        return;
    // Aspect comes from the PANEL's raw dims when provided: the 16-aligned
    // render buffer (1920x1088) is 0.7% flatter than a 2560x1440 panel and
    // that read as slightly-oval circles on the mirrors.
    if (aspectW <= 0 || aspectH <= 0) {
        aspectW = w;
        aspectH = h;
    }
    const float ax = (aspectH > aspectW) ? aspectW / (float)aspectH : 1.0f;
    const float ay = (aspectW > aspectH) ? aspectH / (float)aspectW : 1.0f;
    const size_t need = (size_t)(m_nGridX + 1) * (size_t)(m_nGridY + 1);
    if (c.simW == w && c.simH == h && c.verts.size() == need &&
        c.fAspectX == ax && c.fAspectY == ay)
        return;

    c.simW = w;
    c.simH = h;
    c.portrait = (h > w);
    c.fAspectX = ax;
    c.fAspectY = ay;
    c.verts.assign(need, MYVERTEX{});
    c.vertinfo.assign(need, td_vertinfo{});

    int nVert = 0;
    for (int y = 0; y <= m_nGridY; y++) {
        for (int x = 0; x <= m_nGridX; x++) {
            MYVERTEX& v = c.verts[(size_t)nVert];
            td_vertinfo& vi = c.vertinfo[(size_t)nVert];
            v.x = x / (float)m_nGridX * 2.0f - 1.0f;
            v.y = y / (float)m_nGridY * 2.0f - 1.0f;
            v.z = 0.0f;

            if (m_bScreenDependentRenderMode)
                vi.rad = sqrtf(v.x * v.x + v.y * v.y);
            else
                vi.rad = sqrtf(v.x * v.x * c.fAspectX * c.fAspectX +
                               v.y * v.y * c.fAspectY * c.fAspectY);
            if (y == m_nGridY / 2 && x == m_nGridX / 2)
                vi.ang = 0.0f;
            else if (m_bScreenDependentRenderMode)
                vi.ang = atan2f(v.y, v.x);
            else
                vi.ang = atan2f(v.y * c.fAspectY, v.x * c.fAspectX);
            vi.a = 1;
            vi.c = 0;

            v.rad = vi.rad;
            v.ang = vi.ang;
            v.tu_orig = v.x * 0.5f + 0.5f;
            v.tv_orig = -v.y * 0.5f + 0.5f;
            v.Diffuse = 0xFFFFFFFF;
            nVert++;
        }
    }
    // Aspect changed → wipe field (if any) was for the old aspect.
    c.patternDirty = true;
}

// ─── Adoption ────────────────────────────────────────────────────────────────

static bool WriteTempPreset(const std::string& body, wchar_t out[MAX_PATH])
{
    wchar_t tempDir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempDir))
        return false;
    if (GetTempFileNameW(tempDir, L"msm", 0, out) == 0)
        return false;
    FILE* f = _wfopen(out, L"wb");
    if (!f) {
        DeleteFileW(out);
        return false;
    }
    const size_t wrote = body.empty() ? 0 : fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (wrote != body.size()) {
        DeleteFileW(out);
        return false;
    }
    return true;
}

// MD3 menu writes ZOOM 0-100 into fVideoEchoZoom (classic range is ~1-2).
// Mirrors the loader's remap (engine_presets.cpp, milk2 path).
static void RemapMd3EchoZoom(CState* s)
{
    if (!s) return;
    float e = s->m_fVideoEchoZoom.eval(-1);
    if (e >= 8.0f)
        s->m_fVideoEchoZoom = 1.0f + e * 0.01f;
}

bool Engine::MirrorSimAdoptPreset(MirrorSimContext& c)
{
    const uint32_t ver = m_presetBundleVersion.load(std::memory_order_acquire);
    if (ver == 0 || ver == c.presetVersion)
        return c.pState != nullptr;

    MirrorPresetBundle b;
    {
        std::lock_guard<std::mutex> lk(m_presetBundleMutex);
        b = m_presetBundle; // string copies; adoption is rare
    }

    const float nowT = GetTime();
    CState* newState = new CState();
    newState->SetEelStorage(c.regBlock, &c.gramBlock);
    CState* newOld = nullptr;
    bool ok = false;

    if (b.isMilk2 && !b.milk2Body1.empty() && !b.milk2Body2.empty()) {
        wchar_t t1[MAX_PATH] = {}, t2[MAX_PATH] = {};
        if (WriteTempPreset(b.milk2Body1, t1) && WriteTempPreset(b.milk2Body2, t2)) {
            newOld = new CState();
            newOld->SetEelStorage(c.regBlock, &c.gramBlock);
            {
                std::lock_guard<std::mutex> lk(m_stateImportMutex);
                ok = newOld->Import(t1, nowT, nullptr, STATE_ALL);
                GetFast_CLEAR();
                ok = newState->Import(t2, nowT, newOld, STATE_ALL) && ok;
                GetFast_CLEAR();
            }
        }
        if (t1[0]) DeleteFileW(t1);
        if (t2[0]) DeleteFileW(t2);
        if (ok) {
            RemapMd3EchoZoom(newOld);
            RemapMd3EchoZoom(newState);
            if (b.milk2HasRandoms) {
                D3DXVECTOR4 rp(b.milk2Random[0], b.milk2Random[1],
                               b.milk2Random[2], b.milk2Random[3]);
                newOld->m_rand_preset = rp;
                newState->m_rand_preset = rp;
            }
        }
    } else if (!b.isMilk2 && !b.path.empty()) {
        std::lock_guard<std::mutex> lk(m_stateImportMutex);
        ok = newState->Import(b.path.c_str(), nowT, nullptr, STATE_ALL);
        GetFast_CLEAR();
    }

    if (!ok) {
        DLOG_WARN("MirrorSim: adoption of bundle v%u FAILED (%ls) — keeping previous",
                  ver, b.path.c_str());
        delete newState;
        delete newOld;
        c.presetVersion = ver; // do not retry every frame
        return c.pState != nullptr;
    }

    // Rotate states. The mirror thread is the only user of these objects and
    // adoption runs before the record on the same thread, so plain deletes.
    if (b.isMilk2) {
        delete c.pOldState;
        delete c.pState;
        c.pOldState = newOld;
        c.pState = newState;
        c.pState->StartBlendFrom(c.pOldState, nowT, 1.0f); // frozen — duration moot
        c.pState->m_fBlendProgress = b.milk2Progress;
        c.bMilk2FrozenBlend = true;
        c.fMilk2FrozenProgress = b.milk2Progress;
    } else {
        delete c.pOldState;
        c.pOldState = c.pState; // may be null on first adoption
        c.pState = newState;
        c.bMilk2FrozenBlend = false;
        if (c.pOldState && b.blendTime >= 0.001f) {
            c.pState->StartBlendFrom(c.pOldState, nowT, b.blendTime);
        } else if (c.pOldState) {
            c.pState->StartBlendFrom(c.pOldState, nowT, 0);
            c.pState->m_bBlending = false;
        }
    }
    c.fPresetStartTime = nowT;
    c.presetVersion = ver;
    c.patternDirty = true;
    DLOG_INFO("MirrorSim: adopted bundle v%u (%ls)", ver, b.path.c_str());
    return true;
}

// ─── Blend pattern (engine mutex held — record path) ─────────────────────────
// Reuses the primary's 1100-line pattern generators at ctx aspect by briefly
// swapping the members they write. The primary render thread is blocked on
// the engine mutex for the whole swap, and the members it parses these
// patterns from (m_szMilk2Pattern etc.) are stable while it is blocked.

void Engine::MirrorSimApplyBlendPattern(MirrorSimContext& c)
{
    if (!c.patternDirty || c.vertinfo.empty() || !c.pState)
        return;
    // Version guard: engine milk2 pattern members describe the CURRENT
    // bundle; if a newer one exists the mirror re-adopts next frame anyway.
    if (c.presetVersion != m_presetBundleVersion.load(std::memory_order_acquire))
        return;

    td_vertinfo* savedVi = m_vertinfo;
    const float savedAx = m_fAspectX, savedAy = m_fAspectY;
    m_vertinfo = c.vertinfo.data();
    m_fAspectX = c.fAspectX;
    m_fAspectY = c.fAspectY;

    if (c.bMilk2FrozenBlend)
        ApplyMilk2BlendPattern();
    else if (c.pState->m_bBlending)
        RandomizeBlendPattern();
    else {
        const size_t n = c.vertinfo.size();
        for (size_t i = 0; i < n; i++) {
            c.vertinfo[i].a = 1;
            c.vertinfo[i].c = 0;
        }
    }

    m_vertinfo = savedVi;
    m_fAspectX = savedAx;
    m_fAspectY = savedAy;
    c.patternDirty = false;
}

// ─── Per-frame step ──────────────────────────────────────────────────────────
// Ctx flavor of LoadPerFrameEvallibVars (milkdropfs.cpp:474-579): identical
// var feed, sourced from the context (audio snapshot, ctx frame counter,
// ctx aspect/size) instead of engine members.

void Engine::LoadPerFrameEvallibVarsCtx(MirrorSimContext& c, CState* pState)
{
    const double t = c.fTime;
    const td_mysounddata& snd = c.audio.snd;

    *pState->var_pf_zoom = (double)pState->m_fZoom.eval(-1);
    *pState->var_pf_zoomexp = (double)pState->m_fZoomExponent.eval(-1);
    *pState->var_pf_rot = (double)pState->m_fRot.eval(-1);
    *pState->var_pf_warp = (double)pState->m_fWarpAmount.eval(-1);
    *pState->var_pf_cx = (double)pState->m_fRotCX.eval(-1);
    *pState->var_pf_cy = (double)pState->m_fRotCY.eval(-1);
    *pState->var_pf_dx = (double)pState->m_fXPush.eval(-1);
    *pState->var_pf_dy = (double)pState->m_fYPush.eval(-1);
    *pState->var_pf_sx = (double)pState->m_fStretchX.eval(-1);
    *pState->var_pf_sy = (double)pState->m_fStretchY.eval(-1);

    *pState->var_pf_time = t - (double)m_fStartTime;
    *pState->var_pf_fps = (double)(c.fFps > 1.f ? c.fFps : 60.f);

    *pState->var_pf_bass = (double)snd.imm_rel[0];
    *pState->var_pf_mid = (double)snd.imm_rel[1];
    *pState->var_pf_treb = (double)snd.imm_rel[2];
    *pState->var_pf_bass_att = (double)snd.avg_rel[0];
    *pState->var_pf_mid_att = (double)snd.avg_rel[1];
    *pState->var_pf_treb_att = (double)snd.avg_rel[2];
    *pState->var_pf_bass_smooth = (double)snd.smooth[0];
    *pState->var_pf_mid_smooth = (double)snd.smooth[1];
    *pState->var_pf_treb_smooth = (double)snd.smooth[2];

    *pState->var_pf_frame = (double)c.nFrame;
    for (int vi = 0; vi < NUM_Q_VAR; vi++)
        *pState->var_pf_q[vi] = pState->q_values_after_init_code[vi];
    *pState->var_pf_monitor = pState->monitor_after_init_code;
    {
        // Same schedule shape as the primary: elapsed over the auto-advance
        // window. The window length is a benign scalar read.
        float dur = m_fNextPresetTime - m_fPresetStartTime;
        if (dur <= 0.001f) dur = 30.0f;
        *pState->var_pf_progress = (t - c.fPresetStartTime) / dur;
    }

    const float tf = (float)t;
    *pState->var_pf_decay = (double)pState->m_fDecay.eval(tf);
    *pState->var_pf_wave_a = (double)pState->m_fWaveAlpha.eval(tf);
    *pState->var_pf_wave_r = (double)pState->m_fWaveR.eval(tf);
    *pState->var_pf_wave_g = (double)pState->m_fWaveG.eval(tf);
    *pState->var_pf_wave_b = (double)pState->m_fWaveB.eval(tf);
    *pState->var_pf_wave_x = (double)pState->m_fWaveX.eval(tf);
    *pState->var_pf_wave_y = (double)pState->m_fWaveY.eval(tf);
    *pState->var_pf_wave_mystery = (double)pState->m_fWaveParam.eval(tf);
    *pState->var_pf_wave_mode = (double)pState->m_nWaveMode;
    *pState->var_pf_ob_size = (double)pState->m_fOuterBorderSize.eval(tf);
    *pState->var_pf_ob_r = (double)pState->m_fOuterBorderR.eval(tf);
    *pState->var_pf_ob_g = (double)pState->m_fOuterBorderG.eval(tf);
    *pState->var_pf_ob_b = (double)pState->m_fOuterBorderB.eval(tf);
    *pState->var_pf_ob_a = (double)pState->m_fOuterBorderA.eval(tf);
    *pState->var_pf_ib_size = (double)pState->m_fInnerBorderSize.eval(tf);
    *pState->var_pf_ib_r = (double)pState->m_fInnerBorderR.eval(tf);
    *pState->var_pf_ib_g = (double)pState->m_fInnerBorderG.eval(tf);
    *pState->var_pf_ib_b = (double)pState->m_fInnerBorderB.eval(tf);
    *pState->var_pf_ib_a = (double)pState->m_fInnerBorderA.eval(tf);
    *pState->var_pf_mv_x = (double)pState->m_fMvX.eval(tf);
    *pState->var_pf_mv_y = (double)pState->m_fMvY.eval(tf);
    *pState->var_pf_mv_dx = (double)pState->m_fMvDX.eval(tf);
    *pState->var_pf_mv_dy = (double)pState->m_fMvDY.eval(tf);
    *pState->var_pf_mv_l = (double)pState->m_fMvL.eval(tf);
    *pState->var_pf_mv_r = (double)pState->m_fMvR.eval(tf);
    *pState->var_pf_mv_g = (double)pState->m_fMvG.eval(tf);
    *pState->var_pf_mv_b = (double)pState->m_fMvB.eval(tf);
    *pState->var_pf_mv_a = (double)pState->m_fMvA.eval(tf);
    *pState->var_pf_echo_zoom = (double)pState->m_fVideoEchoZoom.eval(tf);
    *pState->var_pf_echo_alpha = (double)pState->m_fVideoEchoAlpha.eval(tf);
    *pState->var_pf_echo_orient = (double)pState->m_nVideoEchoOrientation;
    *pState->var_pf_wave_usedots = (double)pState->m_bWaveDots;
    *pState->var_pf_wave_thick = (double)pState->m_bWaveThick;
    *pState->var_pf_wave_additive = (double)pState->m_bAdditiveWaves;
    *pState->var_pf_wave_brighten = (double)pState->m_bMaximizeWaveColor;
    *pState->var_pf_darken_center = (double)pState->m_bDarkenCenter;
    *pState->var_pf_gamma = (double)pState->m_fGammaAdj.eval(tf);
    *pState->var_pf_wrap = (double)pState->m_bTexWrap;
    *pState->var_pf_invert = (double)pState->m_bInvert;
    *pState->var_pf_brighten = (double)pState->m_bBrighten;
    *pState->var_pf_darken = (double)pState->m_bDarken;
    *pState->var_pf_solarize = (double)pState->m_bSolarize;
    *pState->var_pf_meshx = (double)m_nGridX;
    *pState->var_pf_meshy = (double)m_nGridY;
    *pState->var_pf_pixelsx = (double)c.simW;
    *pState->var_pf_pixelsy = (double)c.simH;

    if (m_bScreenDependentRenderMode) {
        *pState->var_pf_aspectx = 1;
        *pState->var_pf_aspecty = 1;
    } else {
        *pState->var_pf_aspectx = 1.0 / (double)c.fAspectX;
        *pState->var_pf_aspecty = 1.0 / (double)c.fAspectY;
    }

    *pState->var_pf_blur1min = (double)pState->m_fBlur1Min.eval(tf);
    *pState->var_pf_blur2min = (double)pState->m_fBlur2Min.eval(tf);
    *pState->var_pf_blur3min = (double)pState->m_fBlur3Min.eval(tf);
    *pState->var_pf_blur1max = (double)pState->m_fBlur1Max.eval(tf);
    *pState->var_pf_blur2max = (double)pState->m_fBlur2Max.eval(tf);
    *pState->var_pf_blur3max = (double)pState->m_fBlur3Max.eval(tf);
    *pState->var_pf_blur1_edge_darken = (double)pState->m_fBlur1EdgeDarken.eval(tf);

    *pState->var_pf_mousex = (double)m_mouseX;
    *pState->var_pf_mousey = (double)m_mouseY;
    *pState->var_pf_mousedown = m_mouseDown ? 1.0 : 0.0;
    *pState->var_pf_mouseclick = m_mouseClicked > 0 ? 1.0 : 0.0;
}

void Engine::MirrorSimStepFrame(MirrorSimContext& c)
{
    if (!c.pState)
        return;

    // Animation clock: the primary's, so a parity-paced mirror stays in time
    // sync with the window it mirrors.
    c.fTime = (double)GetTime();
    c.nFrame++;

    // Rate: measured on the WALL clock, never off fTime (see render_context.h).
    // This value is the HUD's "mirror" line and feeds var_pf_fps in the ctx's
    // own EEL, which presets use to normalise per-frame motion — it has to be
    // this worker's real step rate.
    {
        static const long long qpf = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
        }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (c.qpcLastStep != 0 && now.QuadPart > c.qpcLastStep) {
            const double dt = (double)(now.QuadPart - c.qpcLastStep) / (double)qpf;
            if (dt > 1e-5 && dt < 0.25) {
                const float inst = (float)(1.0 / dt);
                c.fFps = (c.fFps <= 1.f) ? inst : c.fFps * 0.95f + inst * 0.05f;
            }
        }
        c.qpcLastStep = now.QuadPart;
    }

    CopyAudioSnapshot(c.audio);

    // Blend timeline (mirrors milkdropfs.cpp:925-941, ctx clock).
    if (c.pState->m_bBlending) {
        if (c.bMilk2FrozenBlend) {
            c.pState->m_fBlendProgress = c.fMilk2FrozenProgress;
        } else {
            c.pState->m_fBlendProgress =
                ((float)c.fTime - c.pState->m_fBlendStartTime) / c.pState->m_fBlendDuration;
            if (c.pState->m_fBlendProgress > 1.0f)
                c.pState->m_bBlending = false;
        }
    } else if (c.bMilk2FrozenBlend && c.pOldState) {
        // Frozen milk2 blend never ends (engine.cpp:3526-3528).
        c.pState->m_bBlending = true;
        c.pState->m_fBlendProgress = c.fMilk2FrozenProgress;
    }
    c.bBlending = c.pState->m_bBlending && c.pOldState;

    const int reps = c.bBlending ? 2 : 1;
    for (int rep = 0; rep < reps; rep++) {
        CState* pState = (rep == 0) ? c.pState : c.pOldState;
        LoadPerFrameEvallibVarsCtx(c, pState);

        // Per-vertex read-only seeding (milkdropfs.cpp:661-697).
        *pState->var_pv_time = *pState->var_pf_time;
        *pState->var_pv_fps = *pState->var_pf_fps;
        *pState->var_pv_frame = *pState->var_pf_frame;
        *pState->var_pv_progress = *pState->var_pf_progress;
        *pState->var_pv_bass = *pState->var_pf_bass;
        *pState->var_pv_mid = *pState->var_pf_mid;
        *pState->var_pv_treb = *pState->var_pf_treb;
        *pState->var_pv_bass_att = *pState->var_pf_bass_att;
        *pState->var_pv_mid_att = *pState->var_pf_mid_att;
        *pState->var_pv_treb_att = *pState->var_pf_treb_att;
        *pState->var_pv_bass_smooth = *pState->var_pf_bass_smooth;
        *pState->var_pv_mid_smooth = *pState->var_pf_mid_smooth;
        *pState->var_pv_treb_smooth = *pState->var_pf_treb_smooth;
        *pState->var_pv_mousex = *pState->var_pf_mousex;
        *pState->var_pv_mousey = *pState->var_pf_mousey;
        *pState->var_pv_mousedown = *pState->var_pf_mousedown;
        *pState->var_pv_mouseclick = *pState->var_pf_mouseclick;
        *pState->var_pv_meshx = (double)m_nGridX;
        *pState->var_pv_meshy = (double)m_nGridY;
        *pState->var_pv_pixelsx = (double)c.simW;
        *pState->var_pv_pixelsy = (double)c.simH;
        if (m_bScreenDependentRenderMode) {
            *pState->var_pv_aspectx = 1;
            *pState->var_pv_aspecty = 1;
        } else {
            *pState->var_pv_aspectx = 1.0 / (double)c.fAspectX;
            *pState->var_pv_aspecty = 1.0 / (double)c.fAspectY;
        }

#ifndef _NO_EXPR_
        if (pState->m_pf_codehandle)
            NSEEL_code_execute(pState->m_pf_codehandle);
#endif
        pState->monitor_after_init_code = *pState->var_pf_monitor;
        for (int vi = 0; vi < NUM_Q_VAR; vi++)
            *pState->var_pv_q[vi] = *pState->var_pf_q[vi];
        *pState->var_pf_gamma = max(0, min(8, *pState->var_pf_gamma));
        *pState->var_pf_echo_zoom = max(0.001, min(1000, *pState->var_pf_echo_zoom));
    }

    // Blend the non-motion vars now (milkdropfs.cpp:729-806; snap fixed at
    // 0.5 — the shader-aware snap tuning is a transition cosmetic).
    if (c.bBlending) {
        CState* S = c.pState;
        CState* O = c.pOldState;
        const double mix = (double)CosineInterp(S->m_fBlendProgress);
        const double mix2 = 1.0 - mix;
        const float snap = 0.5f;
        auto lerp = [&](double* a, double* b) { *a = mix * (*a) + mix2 * (*b); };
        auto pick = [&](double* a, double* b) { if (mix < snap) *a = *b; };
        lerp(S->var_pf_decay, O->var_pf_decay);
        lerp(S->var_pf_wave_a, O->var_pf_wave_a);
        lerp(S->var_pf_wave_r, O->var_pf_wave_r);
        lerp(S->var_pf_wave_g, O->var_pf_wave_g);
        lerp(S->var_pf_wave_b, O->var_pf_wave_b);
        lerp(S->var_pf_wave_x, O->var_pf_wave_x);
        lerp(S->var_pf_wave_y, O->var_pf_wave_y);
        lerp(S->var_pf_wave_mystery, O->var_pf_wave_mystery);
        // Border parameters are NOT blended for a frozen .milk2 (Mandala2 —
        // interpolating two invisible borders synthesised a visible one).
        if (!c.bMilk2FrozenBlend) {
            lerp(S->var_pf_ob_size, O->var_pf_ob_size);
            lerp(S->var_pf_ob_r, O->var_pf_ob_r);
            lerp(S->var_pf_ob_g, O->var_pf_ob_g);
            lerp(S->var_pf_ob_b, O->var_pf_ob_b);
            lerp(S->var_pf_ob_a, O->var_pf_ob_a);
            lerp(S->var_pf_ib_size, O->var_pf_ib_size);
            lerp(S->var_pf_ib_r, O->var_pf_ib_r);
            lerp(S->var_pf_ib_g, O->var_pf_ib_g);
            lerp(S->var_pf_ib_b, O->var_pf_ib_b);
            lerp(S->var_pf_ib_a, O->var_pf_ib_a);
        }
        lerp(S->var_pf_mv_x, O->var_pf_mv_x);
        lerp(S->var_pf_mv_y, O->var_pf_mv_y);
        lerp(S->var_pf_mv_dx, O->var_pf_mv_dx);
        lerp(S->var_pf_mv_dy, O->var_pf_mv_dy);
        lerp(S->var_pf_mv_l, O->var_pf_mv_l);
        lerp(S->var_pf_mv_r, O->var_pf_mv_r);
        lerp(S->var_pf_mv_g, O->var_pf_mv_g);
        lerp(S->var_pf_mv_b, O->var_pf_mv_b);
        lerp(S->var_pf_mv_a, O->var_pf_mv_a);
        lerp(S->var_pf_echo_zoom, O->var_pf_echo_zoom);
        lerp(S->var_pf_echo_alpha, O->var_pf_echo_alpha);
        pick(S->var_pf_echo_orient, O->var_pf_echo_orient);
        pick(S->var_pf_wave_usedots, O->var_pf_wave_usedots);
        pick(S->var_pf_wave_thick, O->var_pf_wave_thick);
        pick(S->var_pf_wave_additive, O->var_pf_wave_additive);
        pick(S->var_pf_wave_brighten, O->var_pf_wave_brighten);
        pick(S->var_pf_darken_center, O->var_pf_darken_center);
        lerp(S->var_pf_gamma, O->var_pf_gamma);
        pick(S->var_pf_wrap, O->var_pf_wrap);
        pick(S->var_pf_invert, O->var_pf_invert);
        pick(S->var_pf_brighten, O->var_pf_brighten);
        pick(S->var_pf_darken, O->var_pf_darken);
        pick(S->var_pf_solarize, O->var_pf_solarize);
        lerp(S->var_pf_blur1min, O->var_pf_blur1min);
        lerp(S->var_pf_blur2min, O->var_pf_blur2min);
        lerp(S->var_pf_blur3min, O->var_pf_blur3min);
        lerp(S->var_pf_blur1max, O->var_pf_blur1max);
        lerp(S->var_pf_blur2max, O->var_pf_blur2max);
        lerp(S->var_pf_blur3max, O->var_pf_blur3max);
        lerp(S->var_pf_blur1_edge_darken, O->var_pf_blur1_edge_darken);
        lerp(S->var_pf_mousex, O->var_pf_mousex);
        lerp(S->var_pf_mousey, O->var_pf_mousey);
        pick(S->var_pf_mousedown, O->var_pf_mousedown);
        pick(S->var_pf_mouseclick, O->var_pf_mouseclick);
    }

    ComputeGridAlphaValuesCtx(c);
}

// ─── Per-vertex mesh (ctx, serial) ───────────────────────────────────────────
// The serial body of ComputeGridAlphaValues (milkdropfs.cpp:5797-6066) over
// ctx arrays and aspect. Runs on the sim thread against the ctx state's own
// pv VM (PerVertexBind(0)); the parallel worker pool stays primary-only.

void Engine::ComputeGridAlphaValuesCtx(MirrorSimContext& c)
{
    if (!c.pState || c.verts.empty())
        return;

    const float fBlend = c.pState->m_fBlendProgress;
    const bool bBlending = c.bBlending;

    const float fWarpTime = (float)c.fTime * c.pState->m_fWarpAnimSpeed;
    const float fWarpScaleInv = 1.0f / c.pState->m_fWarpScale.eval((float)c.fTime);
    float f[4];
    f[0] = 11.68f + 4.0f * cosf(fWarpTime * 1.413f + 10);
    f[1] = 8.77f + 3.0f * cosf(fWarpTime * 1.113f + 7);
    f[2] = 10.54f + 3.0f * cosf(fWarpTime * 1.233f + 3);
    f[3] = 11.49f + 4.0f * cosf(fWarpTime * 0.933f + 5);

    const int num_reps = bBlending ? 2 : 1;
    for (int rep = 0; rep < num_reps; rep++) {
        CState* pState = (rep == 0) ? c.pState : c.pOldState;
        if (!pState)
            continue;

        const float fZoomPF = (float)(*pState->var_pf_zoom);
        const float fZoomExpPF = (float)(*pState->var_pf_zoomexp);
        const float fRotPF = (float)(*pState->var_pf_rot);
        const float fWarpPF = (float)(*pState->var_pf_warp);
        const float fCXPF = (float)(*pState->var_pf_cx);
        const float fCYPF = (float)(*pState->var_pf_cy);
        const float fDXPF = (float)(*pState->var_pf_dx);
        const float fDYPF = (float)(*pState->var_pf_dy);
        const float fSXPF = (float)(*pState->var_pf_sx);
        const float fSYPF = (float)(*pState->var_pf_sy);

        const mdrop::PvBind bind = pState->PerVertexBind(0);

        float fZoom = fZoomPF, fZoomExp = fZoomExpPF, fRot = fRotPF, fWarp = fWarpPF;
        float fCX = fCXPF, fCY = fCYPF, fDX = fDXPF, fDY = fDYPF;
        float fSX = fSXPF, fSY = fSYPF;

        int n = 0;
        for (int y = 0; y <= m_nGridY; y++) {
            for (int x = 0; x <= m_nGridX; x++) {
                MYVERTEX& v = c.verts[(size_t)n];
                const td_vertinfo& vi = c.vertinfo[(size_t)n];

                if (bind.code) {
                    if (m_bScreenDependentRenderMode) {
                        *bind.x = (double)(v.x * 0.5f + 0.5f);
                        *bind.y = (double)(v.y * -0.5f + 0.5f);
                    } else {
                        *bind.x = (double)(v.x * 0.5f * c.fAspectX + 0.5f);
                        *bind.y = (double)(v.y * -0.5f * c.fAspectY + 0.5f);
                    }
                    *bind.rad = (double)vi.rad;
                    *bind.ang = (double)vi.ang;
                    *bind.zoom = *pState->var_pf_zoom;
                    *bind.zoomexp = *pState->var_pf_zoomexp;
                    *bind.rot = *pState->var_pf_rot;
                    *bind.warp = *pState->var_pf_warp;
                    *bind.cx = *pState->var_pf_cx;
                    *bind.cy = *pState->var_pf_cy;
                    *bind.dx = *pState->var_pf_dx;
                    *bind.dy = *pState->var_pf_dy;
                    *bind.sx = *pState->var_pf_sx;
                    *bind.sy = *pState->var_pf_sy;
#ifndef _NO_EXPR_
                    NSEEL_code_execute(bind.code);
#endif
                    fZoom = (float)(*bind.zoom);
                    fZoomExp = (float)(*bind.zoomexp);
                    fRot = (float)(*bind.rot);
                    fWarp = (float)(*bind.warp);
                    fCX = (float)(*bind.cx);
                    fCY = (float)(*bind.cy);
                    fDX = (float)(*bind.dx);
                    fDY = (float)(*bind.dy);
                    fSX = (float)(*bind.sx);
                    fSY = (float)(*bind.sy);
                }

                const float fZoom2 = powf(fZoom, powf(fZoomExp, vi.rad * 2.0f - 1.0f));
                const float fZoom2Inv = 1.0f / fZoom2;

                float u, v2;
                if (m_bScreenDependentRenderMode) {
                    u = v.x * 0.5f * fZoom2Inv + 0.5f;
                    v2 = -v.y * 0.5f * fZoom2Inv + 0.5f;
                } else {
                    u = v.x * c.fAspectX * 0.5f * fZoom2Inv + 0.5f;
                    v2 = -v.y * c.fAspectY * 0.5f * fZoom2Inv + 0.5f;
                }

                u = (u - fCX) / fSX + fCX;
                v2 = (v2 - fCY) / fSY + fCY;

                u += fWarp * 0.0035f * sinf(fWarpTime * 0.333f + fWarpScaleInv * (v.x * f[0] - v.y * f[3]));
                v2 += fWarp * 0.0035f * cosf(fWarpTime * 0.375f - fWarpScaleInv * (v.x * f[2] + v.y * f[1]));
                u += fWarp * 0.0035f * cosf(fWarpTime * 0.753f - fWarpScaleInv * (v.x * f[1] - v.y * f[2]));
                v2 += fWarp * 0.0035f * sinf(fWarpTime * 0.825f + fWarpScaleInv * (v.x * f[0] + v.y * f[3]));

                const float u2 = u - fCX;
                const float vv2 = v2 - fCY;
                const float cos_rot = cosf(fRot);
                const float sin_rot = sinf(fRot);
                u = u2 * cos_rot - vv2 * sin_rot + fCX;
                v2 = u2 * sin_rot + vv2 * cos_rot + fCY;

                u -= fDX;
                v2 -= fDY;

                if (!m_bScreenDependentRenderMode) {
                    u = (u - 0.5f) / c.fAspectX + 0.5f;
                    v2 = (v2 - 0.5f) / c.fAspectY + 0.5f;
                }

                if (rep == 0) {
                    v.tu = u;
                    v.tv = v2;
                    v.Diffuse = 0xFFFFFFFF;
                } else {
                    float mix2 = vi.a * fBlend + vi.c;
                    mix2 = max(0, min(1, mix2));
                    v.tu = v.tu * mix2 + u * (1 - mix2);
                    v.tv = v.tv * mix2 + v2 * (1 - mix2);
                    v.Diffuse = 0x00FFFFFF | (((DWORD)(mix2 * 255)) << 24);
                }
                n++;
            }
        }
    }
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void Engine::MirrorSimFree(MirrorSimContext& c)
{
    delete c.pState;
    delete c.pOldState;
    c.pState = nullptr;
    c.pOldState = nullptr;
    // GRAM must outlive every VM that referenced it (freed above).
    NSEEL_VM_FreeGRAM(&c.gramBlock);
    c.gramBlock = nullptr;
    c.verts.clear();
    c.vertinfo.clear();
    c.presetVersion = 0;
    c.simW = c.simH = 0;
    c.nFrame = 0;
    c.fFps = 0.f;
    c.bMilk2FrozenBlend = false;
    c.patternDirty = false;
}

} // namespace mdrop
