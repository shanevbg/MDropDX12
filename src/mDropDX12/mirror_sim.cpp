// mirror_sim.cpp — independent mirror simulation contexts.
//
// Spec: docs/superpowers/specs/2026-08-21-independent-mirror-sims-design.md
// Each independent mirror size/orientation group runs its own simulation:
// own preset states (own EEL VMs, per-context reg/gmegabuf), own warp mesh,
// own frame counter and blend timeline, fed by an immutable audio snapshot
// from the primary. The wall clock (GetTime()) is shared — time flows the
// same everywhere; independence is in the per-FRAME integration.
//
// Threading contract (see engine.h at m_presetLoadMutex):
//  - MirrorSimAdoptPreset / MirrorSimStepFrame run on the mirror thread with
//    NO engine mutex held. They touch only ctx-owned state, the preset
//    bundle (own mutex), the audio snapshot (own mutex), and read-only or
//    benign Engine scalars.
//  - MirrorSimApplyBlendPattern runs on the mirror thread UNDER the engine
//    mutex (record path): it briefly swaps m_vertinfo/m_fAspect to reuse the
//    1100-line blend-pattern generators at ctx aspect. Rare (preset loads).

#include "engine.h"
#include "utility.h"


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
    // #186 phase 1: a mirror FOLLOWS the primary. Nothing sets
    // ownPresetPath today, so `own` is always false and the own-preset
    // branch below is dormant.
    //
    // It is kept rather than deleted, and that is the point of this change.
    // The machinery that lets a context hold a preset of its own is correct
    // and was the expensive half of #184/#186. What was wrong was the POLICY
    // that handed one to a MIRROR -- "the first enabled independent display
    // with a literal startup preset wins", arbitrary by its own admission,
    // and a mirror that renders a different preset is not a mirror.
    //
    // Phase 4 sets ownPresetPath from the display that owns THIS context,
    // and this branch wakes up unchanged.
    const bool own = !c.ownPresetPath.empty();

    const uint32_t ver = m_presetBundleVersion.load(std::memory_order_acquire);
    // "Already showing it" must mean the COMPILE is still there, not merely
    // that the path matches.
    //
    // These tests were `return c.pState != nullptr`, which conflates two
    // different answers: "nothing to do" and "the thing I was asked for is
    // gone". A teardown -- CleanUpMyDX9Stuff on the resize that mirror
    // watermark performs -- frees pState while leaving this context, its
    // ownPresetPath and its loadedOwnPath all intact and equal. From then on
    // this returned false forever: the path said the preset was loaded so it
    // never re-imported, and pState stayed null so it never reported success.
    // MirrorThreadDrawAndPresent bails on a false return, so the surface never
    // simulated again and the panel froze on whatever single frame the first
    // draw managed (#197).
    //
    // Falling through instead re-imports, which is exactly what a context whose
    // compiled state was freed underneath it needs. Plain MIRROR off/on never
    // hit this because it does not resize the window and so never tears the
    // mirror stack down -- which is why this looked watermark-specific.
    if (own) {
        if (c.ownPresetPath == c.loadedOwnPath && c.pState != nullptr)
            return true;
    } else {
        // ver == 0 means nothing has been broadcast yet, so there is genuinely
        // nothing to adopt; that answer is unchanged.
        if (ver == 0)
            return c.pState != nullptr;
        if (ver == c.presetVersion && c.pState != nullptr)
            return true;
    }

    MirrorPresetBundle b;
    if (own) {
        // Synthesised rather than broadcast, so everything below -- import,
        // compile, rotate -- is the same code either way.
        b.path = c.ownPresetPath;
        b.isMilk2 = false;
        b.blendTime = 0.f;

        // A .milk2 is TWO presets at a fixed blend, not one preset. The first
        // version of this set isMilk2=false unconditionally and imported the
        // file whole, which threw that structure away -- the reported "one
        // frozen preset on top of another" and "advancing leaves a mess".
        //
        // Split it the way the primary's loader does. ParseMilk2File writes its
        // results into ENGINE members (m_szMilk2Pattern, m_bMilk2HasRandoms,
        // m_fMilk2Random, m_nMilk2Direction), which the primary is using for
        // ITS preset -- so this saves and restores them around the call, under
        // the same lock the compile below takes.
        //
        // Save/mutate/restore is the pattern #121 wants deleted, and this is a
        // stopgap knowingly in that shape: the parser has to return these
        // rather than deposit them on the engine. Doing that properly is its
        // own change; doing it here would mean editing the primary's load path
        // from a fix to the mirror's.
        const size_t dot = c.ownPresetPath.find_last_of(L'.');
        const bool looksMilk2 =
            dot != std::wstring::npos &&
            _wcsicmp(c.ownPresetPath.c_str() + dot, L".milk2") == 0;
        if (looksMilk2) {
            std::lock_guard<std::mutex> lk(m_presetLoadMutex);
            char savedPattern[32];
            strncpy_s(savedPattern, sizeof(savedPattern), m_szMilk2Pattern, _TRUNCATE);
            const bool savedHasRnd = m_bMilk2HasRandoms;
            float savedRnd[5];
            for (int i = 0; i < 5; i++) savedRnd[i] = m_fMilk2Random[i];
            const int savedDir = m_nMilk2Direction;

            wchar_t t1[MAX_PATH] = {}, t2[MAX_PATH] = {};
            int mixType = -1, direction = 1;
            float progress = 0.5f;
            if (ParseMilk2File(c.ownPresetPath.c_str(), t1, t2, mixType, progress, direction)) {
                auto slurp = [](const wchar_t* path, std::string& out) {
                    out.clear();
                    FILE* tf = _wfopen(path, L"rb");
                    if (!tf) return;
                    fseek(tf, 0, SEEK_END);
                    long sz = ftell(tf);
                    fseek(tf, 0, SEEK_SET);
                    if (sz > 0) { out.resize((size_t)sz); out.resize(fread(&out[0], 1, (size_t)sz, tf)); }
                    fclose(tf);
                };
                slurp(t1, b.milk2Body1);
                slurp(t2, b.milk2Body2);
                if (!b.milk2Body1.empty() && !b.milk2Body2.empty()) {
                    b.isMilk2 = true;
                    b.milk2Progress = progress;
                    b.milk2HasRandoms = m_bMilk2HasRandoms;
                    for (int i = 0; i < 5; i++)
                        b.milk2Random[i] = m_fMilk2Random[i];
                }
            }
            if (t1[0]) DeleteFileW(t1);
            if (t2[0]) DeleteFileW(t2);

            strncpy_s(m_szMilk2Pattern, sizeof(m_szMilk2Pattern), savedPattern, _TRUNCATE);
            m_bMilk2HasRandoms = savedHasRnd;
            for (int i = 0; i < 5; i++) m_fMilk2Random[i] = savedRnd[i];
            m_nMilk2Direction = savedDir;
        }
    } else {
        std::lock_guard<std::mutex> lk(m_presetBundleMutex);
        b = m_presetBundle; // string copies; adoption is rare
    }

    // The CONTEXT's clock, not GetTime(). Everything anchored here is later
    // compared against c.fTime -- blend progress at
    // `(c.fTime - m_fBlendStartTime) / m_fBlendDuration`, preset progress
    // against c.fPresetStartTime -- so anchoring on the primary's clock while
    // measuring on the context's would make both meaningless the moment the two
    // clocks are independent.
    const float nowT = (float)c.fTime;
    CState* newState = new CState();
    newState->SetEelStorage(c.regBlock, &c.gramBlock);
    CState* newOld = nullptr;
    bool ok = false;

    if (b.isMilk2 && !b.milk2Body1.empty() && !b.milk2Body2.empty()) {
        wchar_t t1[MAX_PATH] = {}, t2[MAX_PATH] = {};
        if (WriteTempPreset(b.milk2Body1, t1) && WriteTempPreset(b.milk2Body2, t2)) {
            newOld = new CState();
            newOld->SetEelStorage(c.regBlock, &c.gramBlock);
            // No lock and no cache clear: each Import owns its own parse
            // cursor now (issue 13), so two of them cannot see each other.
            ok = newOld->Import(t1, nowT, nullptr, STATE_ALL);
            ok = newState->Import(t2, nowT, newOld, STATE_ALL) && ok;
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
        // A .milk3 takes the Shadertoy parse, not CState::Import (#373).
        //
        // Import has no .milk3 branch: the JSON opens, matches no keys and
        // returns SUCCESS on a default state, so a surface used to adopt an
        // empty preset, render black, cache the path so it never retried, and
        // report the .milk3 over IPC as what was playing -- with nothing
        // logged. ParseMilk3IntoState fills the same CState fields the primary
        // uses, after which this surface's own LoadShaders and
        // CreateDX12PresetPSOs below compile the Image and Buffer A-D passes
        // and build their PSOs, exactly as they already do for a classic
        // preset. PresetPsoOwned has carried bufferA-D and compFloat all along.
        const size_t dot = b.path.find_last_of(L'.');
        const bool wantMilk3 =
            dot != std::wstring::npos &&
            _wcsicmp(b.path.c_str() + dot, L".milk3") == 0;
        if (wantMilk3) {
            ok = ParseMilk3IntoState(b.path.c_str(), newState);
            if (!ok)
                DLOG_WARN("MirrorSim: %ls is not a usable .milk3 — keeping "
                          "previous (#373)", b.path.c_str());
        } else {
            ok = newState->Import(b.path.c_str(), nowT, nullptr, STATE_ALL);
        }
    }

    if (!ok) {
        DLOG_WARN("MirrorSim: adoption of bundle v%u FAILED (%ls) — keeping previous",
                  ver, b.path.c_str());
        delete newState;
        delete newOld;
        c.presetVersion = ver; // do not retry every frame
        // Same reason, for the own-preset source: without this a preset that
        // will not import is re-attempted on every sim step forever.
        c.loadedOwnPath = own ? c.ownPresetPath : std::wstring();
        return c.pState != nullptr;
    }

    // WHICH KIND this surface now holds (#373). Derived from the path and set
    // HERE, past the failure return, so that:
    //
    //   * every branch above is covered. Setting it inside the .milk3 arm left
    //     a surface going .milk3 -> .milk2 with the flag still raised, since a
    //     .milk2 takes the other branch entirely and would then have rendered
    //     a classic state through the Shadertoy pipeline;
    //   * a file that failed to parse cannot leave the surface claiming a mode
    //     it has no compiled shaders for -- on failure the previous preset is
    //     kept, and so is the flag that describes it.
    //
    // It is set BEFORE the compile below, which reads it to pick the comp
    // template's `ret` type.
    {
        const size_t d = b.path.find_last_of(L'.');
        c.isMilk3.store(d != std::wstring::npos &&
                            _wcsicmp(b.path.c_str() + d, L".milk3") == 0,
                        std::memory_order_relaxed);
    }

    // Compile THIS context's own shaders and pipeline objects (#184).
    //
    // Until now a mirror drew with the PRIMARY's compiled set, which is the one
    // thing that made display_output.h's "cannot hold a preset of its own"
    // true. The context already owned its CState pair; this gives it the other
    // half. It compiles the same preset text the bundle carried, so the picture
    // is unchanged -- what changes is that the set is ITS OWN, which is also
    // what gives it its own DX12ConstantTable and therefore its own shadow
    // buffer.
    //
    // Under m_presetLoadMutex: LoadShaders writes shared Engine members on its
    // failure paths (m_activeOverride.warpFailed/compFailed,
    // ShaderOverrides().SetLastError, and m_pShaderCompileErrors inside
    // LoadShaderFromMemory), so it must not run concurrently with the engine's
    // own loader. That costs nothing today -- adoption is rare -- and shrinking
    // the lock is #121's item, not this one.
    //
    // PSOs are built here too, which is safe off the render thread only because
    // the builder no longer calls WaitForGpu: ID3D12Device is free-threaded for
    // CreatePipelineState, but a worker stalling the whole device is what
    // deadlocked D3D12Core before (engine.h).
    PShaderSet stagedNew, stagedOld;
    PresetPsoOwned stagedPsos;
    {
        std::lock_guard<std::mutex> lk(m_presetLoadMutex);
        // Errors here belong to the preset being adopted, on this thread -- and
        // so does its KIND (#373). The comp template injects `float4 ret` for a
        // Shadertoy preset and `float3` for a classic one, and it used to read a
        // global: a classic preset adopted while the ENGINE was on a .milk3 got
        // the Shadertoy prelude and failed to compile with X3017.
        //
        // A scope rather than two bare assignments, so an early return or a
        // throw inside LoadShaders cannot leave this thread claiming to be
        // compiling something it is not.
        ShaderCompileScope scope(b.path.empty() ? nullptr : b.path.c_str(),
                                 c.isMilk3.load(std::memory_order_relaxed));
        LoadShaders(&stagedNew, newState, false, false);
        if (newOld)
            LoadShaders(&stagedOld, newOld, false, false);
        CreateDX12PresetPSOs(stagedNew, stagedOld, stagedPsos);
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
    // Same rotation for the shaders, and RotatePShaderSet rather than
    // assignment: copy is deleted on these types (#5) precisely so that a
    // hand-written copy-then-null cannot double-release later.
    RotatePShaderSet(c.oldShaders, c.shaders);
    RotatePShaderSet(c.shaders, stagedNew);
    if (newOld)
        RotatePShaderSet(c.oldShaders, stagedOld);
    c.psos = stagedPsos;

    c.fPresetStartTime = nowT;
    c.presetVersion = ver;
    c.loadedOwnPath = own ? c.ownPresetPath : std::wstring();
    // See RenderContext::clearFeedbackPending. Own-preset adoptions only.
    if (own)
        c.clearFeedbackPending = true;
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

    // The generators take the canvas they are filling. This used to swap
    // m_vertinfo, m_fAspectX and m_fAspectY onto the engine and put them back
    // -- the third of #186 phase 2's mutation sites, and the only one that
    // moved a POINTER, so a primary frame recording concurrently could follow
    // it into the mirror's vertex array.
    const BlendPatternTarget tgt{ c.vertinfo.data(), c.fAspectX, c.fAspectY };

    if (c.bMilk2FrozenBlend)
        ApplyMilk2BlendPattern(tgt);
    else if (c.pState->m_bBlending)
        RandomizeBlendPattern(tgt);
    else {
        const size_t n = c.vertinfo.size();
        for (size_t i = 0; i < n; i++) {
            c.vertinfo[i].a = 1;
            c.vertinfo[i].c = 0;
        }
    }

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

    c.nFrame++;

    // Own randomness, stepped with the frame (#121). rand()'s UCRT state is
    // per thread, so this worker draws from its own sequence and cannot
    // perturb the render thread's -- which is what makes generating it here
    // safe rather than merely convenient.
    //
    // That same per-thread state means this sequence is UNSEEDED and therefore
    // repeats run to run, while the primary's does not. Harmless for shader
    // noise, and worth knowing before it is reported as "the mirror's noise is
    // the same every launch": it is, by construction, and seeding it would need
    // a seed that is not the render thread's.
    for (int i = 0; i < 4; i++)
        c.randFrame[i] = (rand() % 7381) / 7380.0f;

    // Animation clock: this context's OWN, advanced from its own step timing.
    //
    // It used to be `c.fTime = (double)GetTime()` -- the primary's clock. An
    // independent mirror simulates the preset for itself, and a user who wants
    // a surface locked to the primary chooses a stretch or copy path instead,
    // so borrowing the primary's clock made the two modes less different than
    // the user's choice between them implies.
    //
    // The advance rule mirrors the primary's (engineshell.cpp:2099-2112) using
    // this context's own cap rather than the window's:
    //   - capped and keeping up  -> exactly 1/cap, so a capped mirror animates
    //                               at the rate it was asked for
    //   - capped but running late -> real elapsed, so a slow mirror does not
    //                               crawl at real_rate/cap
    //   - uncapped (parity)      -> real elapsed
    // Parity pacing is unaffected: how OFTEN this steps is still one step per
    // primary frame (m_nPrimaryFrameSeq), and only what each step adds to the
    // clock is decided here. That distinction is what keeps this from
    // reintroducing the 2026-08-23 regression where a free-running worker made
    // mirrors visibly faster than an fps-capped primary -- that was about step
    // RATE, which this does not touch.
    //
    // Rate is still measured on the WALL clock and never off fTime (see
    // render_context.h): fFps feeds var_pf_fps, which presets use to normalise
    // per-frame motion, so it has to be this worker's real step rate.
    {
        static const long long qpf = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
        }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = 0.0;
        if (c.qpcLastStep != 0 && now.QuadPart > c.qpcLastStep)
            dt = (double)(now.QuadPart - c.qpcLastStep) / (double)qpf;
        const bool bSaneDt = (dt > 1e-5 && dt < 0.25);

        if (bSaneDt) {
            const float inst = (float)(1.0 / dt);
            c.fFps = (c.fFps <= 1.f) ? inst : c.fFps * 0.95f + inst * 0.05f;
        }

        const int cap = m_nMirrorMaxFps.load(std::memory_order_relaxed);
        if (cap > 0) {
            const double period = 1.0 / (double)cap;
            c.fTime += (bSaneDt && dt > period * 1.25) ? dt : period;
        } else if (bSaneDt) {
            c.fTime += dt;
        } else {
            // First step, or a stall long enough that the delta is not
            // trustworthy: advance by a nominal frame rather than jumping.
            c.fTime += 1.0 / 60.0;
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
