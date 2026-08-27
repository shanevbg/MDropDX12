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

struct MirrorSimContext {
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

  // ── audio (copied from Engine::m_audioSnap at sim frame start) ──
  AudioSnapshot audio;
};
