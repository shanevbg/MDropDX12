/*
  render_context.h — per-simulation state for independent mirror rendering.

  INTERNAL FRAGMENT: included by engine.h ONLY, inside `namespace mdrop`,
  after td_mysounddata / td_vertinfo / MYVERTEX are visible. Do not include
  from anywhere else.

  Spec: docs/superpowers/specs/2026-08-21-independent-mirror-sims-design.md
  Each independent mirror size/orientation group gets one MirrorSimContext:
  its own preset states (own EEL VMs with per-context reg00-99 + gmegabuf
  blocks), own warp mesh, own time base and audio snapshot — sharing only
  read-only resources (PSOs, textures, device) with the primary.
*/
#pragma once
// CState comes from state.h, already included by engine.h before this
// fragment (global namespace — do NOT forward-declare it here inside mdrop).

// Immutable copy of the primary's audio analysis, published once per primary
// frame (render thread) and copied by each sim thread at its frame start.
// Contains td_mysounddata wholesale — the exact struct every per-frame /
// shape / wave EEL feed reads — so sims consume identical audio.
struct AudioSnapshot {
  td_mysounddata snd = {};
  uint32_t       serial = 0;   // bumped per publish; 0 = never published
};

// What the render sequence needs to know about the surface it is drawing.
//
// The primary path (DX12_RenderWarpAndComposite) and the mirror worker's path
// (RenderClassicOrientPipeline) are two hand-copied bodies, and #15 exists
// because a mitigation written into one does not reach the other. That has
// shipped twice: canvasMax on 2026-08-23, and the feedback damp on 2026-08-27
// (`09dadf8`, "the feedback damp never reached mirrored displays").
//
// The two bodies are NOT two halves of one function. The primary runs 17
// stages; the mirror runs 4 of them -- warp, blur, inject, comp -- and skips
// the first-frame clear, supertexts, Shadertoy, video in/out, motion vectors,
// the Buffer A pass and the feedback blit entirely. So they are unified stage
// by stage rather than merged wholesale, and this is the carrier: the few
// things a stage needs that genuinely differ between the two.
//
// The canvas size is the interesting one. Both paths apply the same
// mitigations, but each at ITS OWN canvas -- the primary at m_nTexSizeX/Y and
// the mirror at its sim size -- which is exactly why the damp could not simply
// be hoisted into a shared function without one: `EffectiveFeedbackDamp()`
// reads the primary's size off the engine, so a mirror calling it would be
// damped for a canvas it is not rendering.
// The compiled pipeline objects for ONE preset. Raw pointers: the context
// borrows these for the duration of a record and never owns them, so nothing
// here AddRefs. Ownership stays with whatever holds the ComPtr -- today the
// Engine, and under #184 the context's own PShaderSet.
struct PresetPsoSet {
  ID3D12PipelineState* warp      = nullptr;
  ID3D12PipelineState* oldWarp   = nullptr;
  ID3D12PipelineState* warpBlend = nullptr;
  ID3D12PipelineState* comp      = nullptr;
  ID3D12PipelineState* oldComp   = nullptr;
  ID3D12PipelineState* compBlend = nullptr;
};

// The same eleven pipeline objects, OWNED. PresetPsoSet above borrows; this
// holds. A surface that compiles its own preset needs its own PSOs as well as
// its own shaders -- the bytecode and the pipeline object are built together
// and neither is shareable across two different presets.
struct PresetPsoOwned {
  Microsoft::WRL::ComPtr<ID3D12PipelineState> warp, oldWarp, warpBlend;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> comp, oldComp, compBlend, compFloat;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> bufferA, bufferB, bufferC, bufferD;
};

struct MirrorSimContext;   // defined below; RenderContext only names it

struct RenderContext {
  ID3D12GraphicsCommandList* cmdList = nullptr;

  // WHICH surface this context belongs to. Null on the primary.
  //
  // Everything else here describes the surface -- its canvas, its aspect, its
  // frame, its state pair -- without ever saying WHICH surface it is, and that
  // gap is what forced per-surface facts to be latched on engine members. A
  // latch shared by every consumer reports whichever ran last, so `ovrFrame`
  // in DIAG_MIRRORS reported the primary's frame beside a mirror's `pfFrame`
  // and the pair that exists to catch a disagreement could not (#198).
  //
  // Const on purpose: a record must stay a pure function of its context. The
  // single thing written through this is a diagnostic, and it is `mutable` at
  // its declaration so that intent is stated where the field lives rather than
  // cast away here.
  const MirrorSimContext* sim = nullptr;

  // THIS surface's canvas, not the engine's. See above.
  int canvasW = 0;
  int canvasH = 0;

  // THIS surface's aspect, derived from canvasW/H by FillCanvasInputs.
  //
  // These exist to delete SizeGuard. The mirror record used to OVERWRITE the
  // engine's m_fAspectX/Y and m_nTexSizeX/Y for the duration of a record and
  // restore them afterwards, which is the single reason two contexts cannot
  // render at once: they would each need the engine held at a different
  // aspect. Carrying them here makes a record a pure function of its context.
  //
  // The restore half was a live defect in its own right.
  // RestorePrimaryTexSizeFromVS() called before the guard raced the worker and
  // flattened mirror shapes to ellipses one frame in ~250 -- the reported
  // "intermittent flicker". DIAG_MIRRORS' aspBad/aspGood counters exist to
  // catch exactly that, and must stay at aspBad=0 across this change.
  float fAspectX = 1.0f;
  float fAspectY = 1.0f;
  float fInvAspectX = 1.0f;
  float fInvAspectY = 1.0f;

  // THIS surface's frame number (#111). Replaces FrameOverrideGuard, which set
  // an engine member for the duration of a record so EffectiveFrame() would
  // answer with the mirror's count instead of the primary's.
  //
  // -1 means "not a recorded surface -- ask the engine", the same convention
  // m_nFrameOverride itself used. It is NOT 0, because 0 is a real frame
  // number: a context that defaulted to 0 would silently claim to be the
  // first frame of a surface and hand every preset frame=0 forever. Only
  // FillCanvasInputs sets it, so the primary's warp and comp contexts -- which
  // carry time, fps and audio but describe no canvas -- correctly say -1.
  //
  // Test the VALUE, never merely that a context exists: that distinction is
  // what 43e807c7 had to fix for the canvas, where `rc ?` alone fed 0 in on
  // the primary path.
  int nFrame = -1;

  // iMouse, in pixels of THIS surface (.milk3 / Shadertoy only).
  //
  // Shaders read the mouse as mouse/texsize, so a surface of a different size
  // needs it rescaled by the same factor or the RATIO changes and every
  // mouse-steered preset aims somewhere else. glslsandbox_108342.0.milk3
  // rotates its camera by exactly that ratio and rendered as a few giant
  // diagonal bands against the primary's concentric tunnel (2026-08-23).
  //
  // This is what the milk3 SizeGuard scaled on the engine and put back. An
  // explicit flag rather than a sentinel value: 0 is a real mouse position,
  // and the top-left corner is exactly where an untouched mouse sits.
  bool  hasMouse = false;
  float stMouseX = 0.f, stMouseY = 0.f;
  float stClickX = 0.f, stClickY = 0.f;

  // The state pair this surface is rendering. For the primary these are the
  // engine's m_pState/m_pOldState; for a mirror they are its context's own.
  CState* pState = nullptr;
  CState* pOldState = nullptr;

  // This draw is happening inside a MIRROR WORKER'S ORIENT RECORD.
  //
  // Was the engine member m_bOrientOppositeAspect, set true around the whole
  // record and false after -- one of the two globals that forced the record to
  // hold m_mirrorEngineMutex exclusively, since two records could need it at
  // different values (#206). Same shape as SizeGuard and FrameOverrideGuard,
  // which #121 deleted the same way.
  //
  // The name it had described orientation, but it was set for EVERY worker
  // record regardless, so what it actually selected was mirror-record versus
  // primary. Named for that now. The constants it still chooses between --
  // kSoftMaxOpposite / kSoftMaxSameOrient -- keep their older names.
  bool bOrientRecord = false;

  // True for a mirror worker's surface. Kept because the paths do genuinely
  // differ and pretending otherwise is how the mirror pipeline acquired its
  // recorded crashes -- see .claude/memory/mirror_sim_architecture.md, which
  // lists three designs that deadlocked or TDR'd.
  bool isMirror = false;

  // ── warp pass inputs ──
  //
  // Blending is passed in rather than read off pState, because the two
  // surfaces genuinely disagree about what it means. The primary uses
  // m_pState->m_bBlending; a mirror uses
  //     c.pOldState && (c.pState->m_bBlending || c.bMilk2FrozenBlend)
  // because a .milk2 frozen blend leaves m_bBlending cleared, and deriving it
  // from pState there would draw only the new preset.
  bool bBlending = false;

  // Where this surface's warp bindings live. The primary uses the fixed
  // per-frame binding blocks (GetWarpBindingGpuHandle); a mirror uses its own
  // reserved SRV block range. Same shader params either way -- only the
  // descriptor table differs.
  D3D12_GPU_DESCRIPTOR_HANDLE warpTable = {};
  D3D12_GPU_DESCRIPTOR_HANDLE oldWarpTable = {};
  D3D12_GPU_DESCRIPTOR_HANDLE compTable = {};
  D3D12_GPU_DESCRIPTOR_HANDLE oldCompTable = {};

  // This surface's warp mesh: the engine's m_verts for the primary, the
  // context's own for a mirror (which runs at its own aspect and grid).
  const MYVERTEX* verts = nullptr;

  // ── blur pyramid inputs ──
  //
  // A mirror builds its OWN pyramid rather than sampling the primary's, and
  // that is deliberate: presets that lean on GetBlur1/2 look mushy and wrong
  // when the blur was generated at a different aspect.
  //
  // Sizes are carried explicitly rather than read off the textures, because
  // the two callers disagree about where the authoritative number lives -- the
  // primary keeps m_nBlurTexW/H alongside the textures and falls back to
  // GetWidth()/GetHeight() when a texture reports zero, while a mirror's pipe
  // carries its own blurW/blurH.
  DX12Texture* blurSrc = nullptr;      // what pass 0 reads
  int          blurSrcW = 0;
  int          blurSrcH = 0;
  DX12Texture* blurTex = nullptr;      // NUM_BLUR_TEX levels
  const int*   blurW = nullptr;        // per-level width, NUM_BLUR_TEX entries
  const int*   blurH = nullptr;
  int          highestBlurUsed = 0;    // levels the shaders actually sample

  // -- animation inputs --
  //
  // Time, rate, preset progress and audio for THIS surface, and it is the same
  // argument as canvasW/H above. A mirror runs its own clock, at its own rate,
  // on its own audio snapshot, so any stage reading GetTime() / GetFps() /
  // mysound off the engine animates it at the PRIMARY's tempo. That is exactly
  // what the HLSL uniforms did until these carried them: a mirror's `time`,
  // `fps`, `progress` and `bass`/`mid`/`treb` were the primary's, while the
  // same preset's per-frame EEL had already been handed the context's values
  // by LoadPerFrameEvallibVarsCtx.
  //
  // #111 fixed precisely that split for `frame`, and its note is the rule for
  // these: "The defect was that only part of the preset saw it." So populate
  // them to MATCH LoadPerFrameEvallibVarsCtx rather than merely to be
  // per-context -- the shader uniform and the preset's own code have to agree
  // about what time it is. Engine::FillAnimInputs is the single place that
  // knows both formulas; do not fill these by hand at a call site.
  //
  // `frame` and the canvas now live here too, above -- they used to reach a
  // mirror through FrameOverrideGuard and SizeGuard, which save/mutate/restore
  // engine members. Moving them onto this struct is what deleted those guards
  // (#186 phase 2); nothing a record touches mutates the engine any more.
  // The RAW clock, not an offset one: GetTime() for the primary,
  // MirrorSimContext::fTime for a mirror. Raw because two different offsets are
  // taken from it -- `time` subtracts m_fStartTime, `time_since_preset_start`
  // subtracts the STATE's preset start -- and a mirror state's preset start is
  // recorded on the mirror's clock (mirror_sim.cpp, `nowT = (float)c.fTime`).
  // Storing the offset form made the second subtraction mix the two clocks.
  double                fTime     = 0.0;
  float                 fFps      = 0.0f;     // measured rate; NOT derived from fTime (see MirrorSimContext::fFps)
  float                 fProgress = 0.0f;     // 0..1 through THIS surface's preset timeline
  const td_mysounddata* snd       = nullptr;  // THIS surface's audio; null falls back to the engine's live mysound

  // Four random floats for the pixel shaders, or null to use the engine's.
  // Shane, 2026-09-09: "the rand_frame can be different if the rendering is
  // different" -- so a surface that simulates the preset for itself gets its
  // own noise, on the same principle as its own clock. A stretch/copy mirror
  // is NOT rendering differently, and never reaches this path.
  //
  // Four floats rather than a D3DXVECTOR4 so this header need not pull in the
  // D3DX types; the consumer builds the vector.
  const float*          randFrame = nullptr;

  // -- the preset being drawn (#184) --
  //
  // Which compiled shaders and pipeline objects this surface draws with. The
  // stages used to reach for Engine::m_shaders / m_OldShaders and the m_dx12*PSO
  // set directly, which is exactly why a context "cannot hold a preset of its
  // own" (display_output.h): there was only ever one set to reach for.
  //
  // Carrying them here does NOT by itself give a context its own preset -- both
  // callers still fill these from the Engine, so nothing changes yet. It is the
  // step that makes owning one expressible: the record path no longer names the
  // engine's preset state, so pointing a context at its own compiled set becomes
  // a fill-site change rather than a rewrite of every draw.
  //
  // Non-const because ApplyShaderParams writes through to the constant table.
  // Borrowed, never owned -- see PresetPsoSet.
  // Which feedback texture textured custom shapes sample, when it is not the
  // engine's VS0. Was the engine member m_pShapeVsOverride, set around part of
  // a mirror record and cleared after -- so two workers cross-contaminated and
  // a worker's write leaked into the primary's shapes (#206). Same shape as
  // m_cmdListOverride, retired the same way.
  // Base of THIS surface's blur descriptor blocks, already offset for the
  // surface's own frame rotation. UINT_MAX means "the engine's", which is the
  // primary.
  //
  // The blur pass was the ONE binding path with no per-surface home: its block
  // was m_blurBindingBase + m_frameIndex*... + passIndex*..., with no surface
  // and no thread term, so the primary and every worker wrote the same block
  // (#209). Every other path already had one -- pipe.bindBase blocks 0-5.
  //
  // The mutex did not cover it. A descriptor is read by the GPU when the list
  // executes, which outlives the CPU-side lock, so serialising the writes only
  // narrowed the window.
  unsigned int blurBindBase = 0xFFFFFFFFu;

  const DX12Texture* shapeVs = nullptr;

  PShaderSet*  shaders    = nullptr;
  PShaderSet*  oldShaders = nullptr;
  PresetPsoSet psos;

  int LongEdge() const { return canvasW > canvasH ? canvasW : canvasH; }
};

struct MirrorSimContext {
  // What ApplyShaderParams last gave THIS surface's `frame` uniform.
  //
  // Mutable because a record is otherwise a pure function of its context and
  // should stay that way: this is written for diagnostics only and is never
  // read back by rendering. It exists so DIAG_MIRRORS can report the uniform
  // and the per-frame variable for the SAME surface (#198).
  mutable int diagShaderFrame = -1;

  // ── preset state (own EEL VMs; compiled AFTER SetRegBase/SetGRAM) ──
  CState*  pState = nullptr;      // current preset
  CState*  pOldState = nullptr;   // blend-from preset (own blend timeline)
  bool     bBlending = false;     // mirrors pState->m_bBlending (set per step)
  bool     bMilk2FrozenBlend = false;
  float    fMilk2FrozenProgress = 0.5f;
  bool     patternDirty = false;  // adoption happened: regenerate ctx blend
                                  // mesh under the engine mutex (record path)
  double   fPresetStartTime = 0.0;
  uint32_t presetVersion = 0;     // last adopted Engine::m_presetBundleVersion

  // #186: where this context's preset comes from.
  //
  // Empty means FOLLOW THE PRIMARY -- adopt whatever PublishPresetBundle
  // broadcast, which is what every mirror did before this and remains the
  // default for a display nobody has configured. Non-empty means this context
  // holds a preset of its own and ignores the bundle entirely.
  //
  // loadedOwnPath is what it actually has, so a resolve that returns the same
  // answer every frame does not reload every frame.
  std::wstring ownPresetPath;
  std::wstring loadedOwnPath;

  // #186: the record path must wipe this context's feedback before the next
  // frame, because the preset that drew what is in it is gone.
  //
  // Raised by adoption, acted on by the record -- adoption runs on the sim
  // thread and has no command list, so it cannot clear a texture itself.
  //
  // Only for an OWN-preset adoption. Following the primary is a TRANSITION and
  // carrying feedback across it is deliberate: that continuity is what makes a
  // preset change look like a blend rather than a cut. A context switching to a
  // preset of its own is a DISCONTINUITY, and the pixels it would carry belong
  // to a preset it is no longer rendering.
  bool clearFeedbackPending = false;

  // ── per-context EEL storage (see NSEEL_VM_SetRegBase / NSEEL_VM_SetGRAM) ──
  double   regBlock[100] = {};
  void*    gramBlock = nullptr;   // pass &gramBlock to NSEEL_VM_SetGRAM;
                                  // NSEEL_VM_FreeGRAM(&gramBlock) on destroy

  // ── sim identity ──
  int      simW = 0, simH = 0;    // capped sim buffer size
  bool     portrait = false;
  float    fAspectX = 1.f, fAspectY = 1.f;
  double   fTime = 0.0;           // own time base (QPC-advanced on sim thread)
  int      nFrame = 0;
  float    fFps = 0.f;            // sim thread's own measured fps (QPC-derived)
  // fFps MUST NOT be measured off fTime: that is the PRIMARY's animation clock
  // (EngineShell::m_time), which only advances once per primary frame. Steps
  // taken between two primary frames see no delta at all, so the average
  // converged on the primary's rate no matter how fast this worker ran — the
  // HUD's "mirror" line read 40 against a 160 fps sim (Shane, 2026-08-23).
  long long qpcLastStep = 0;      // wall clock of the previous sim step

  // ── own warp mesh, sized (gridX+1)*(gridY+1) like the primary's m_verts ──
  std::vector<MYVERTEX>    verts;
  std::vector<td_vertinfo> vertinfo;

  // Does this context render from its OWN compiled preset, or borrow the
  // engine's? (#206)
  //
  // ONE place, because it used to be spelled twice -- once in
  // RenderClassicOrientPipeline for the shader set, once in
  // FillPresetPipeline for the PSOs -- and two copies of a predicate is how
  // the shader set and the pipeline it is drawn with drift apart.
  //
  // The test is about the CONTEXT, not about what its preset happened to
  // contain: a preset with no custom warp or comp shader compiles to no
  // bytecode, and that is a fact about the preset, not evidence that this
  // context has nothing of its own.
  // ADOPTION, not bytecode. The old test was
  //
  //     shaders.warp.bytecodeBlob || shaders.comp.bytecodeBlob
  //
  // which asks what the preset contained, and answers "borrow the engine's"
  // for any preset with no custom warp or comp shader -- a fact about the
  // preset, not about this context. The comment on the shader-set site says
  // what that costs when the two sets hold different presets:
  // BuildBindingSlots resolves the ENGINE preset's texture list into the
  // table and the CONTEXT preset's shader then samples whatever is in those
  // slots. Reported as "presets are rendering but they're incorrect".
  //
  // Once adoption has run this context owns a compiled set, empty or not, and
  // an empty one is the right answer for a preset that has no shaders -- the
  // engine's set for that same preset is equally empty. The borrow survives
  // only for the pre-adoption frame it was written for.
  bool HasOwnPipeline() const {
    return pState != nullptr && (presetVersion != 0 || !loadedOwnPath.empty());
  }

  // ── audio (copied from Engine::m_audioSnap at sim frame start) ──
  AudioSnapshot audio;

  // -- this context's own compiled preset (#184) --
  //
  // The context already owns its CState pair: the adoption path builds them
  // with `new CState()` and gives them its own EEL storage, on the sim thread,
  // nowhere near Engine::LoadPreset. Shaders and pipeline objects were the only
  // thing still borrowed, and the only reason display_output.h can say a mirror
  // "cannot hold a preset of its own".
  //
  // These have to come from a real compile, not from CloneFrom: CloneFrom
  // AddRefs the SAME DX12ConstantTable, so its shadow buffer would still be
  // shared and the concurrency hazard #184 exists to remove would survive
  // untouched. An independent compile is what gives an independent shadow.
  PShaderSet     shaders;
  PShaderSet     oldShaders;
  PresetPsoOwned psos;

  // This context's own per-frame randomness, advanced once per sim step
  // alongside nFrame. The engine's m_rand_frame advances once per PRIMARY
  // frame, so sharing it stepped a mirror's noise at the primary's rate
  // however fast this worker ran.
  float randFrame[4] = { 0.f, 0.f, 0.f, 0.f };
};
