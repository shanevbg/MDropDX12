#pragma once
/*
  pv_workers.h — parallel evaluation of a preset's per-vertex (`per_pixel`) code.

  WHY THIS EXISTS

  `ComputeGridAlphaValues()` calls `NSEEL_code_execute()` once per warp-mesh
  vertex. At the default nMeshSize=64 that is 65*49 = 3,185 calls per frame; at
  nMeshSize=192 it is 193*145 = 27,985, doubled again to 55,970 while two presets
  are blending (`num_reps = m_bBlending ? 2 : 1`).

  Measured 2026-08-20 at 1200x900, vsync off, FPS uncapped, nMeshSize=192, with
  samples taken only while the render window was foreground:

      preset                       wall      per-vertex EEL     gpu wait
      Web Blue Lines (32 pp lines) 5.85 ms   4.73 ms  (81%)     0.27 ms
      LazerX2                      6.20 ms   5.10 ms  (82%)     0.27 ms
      Reflective 4D Julia3         2.53 ms   1.53 ms  (61%)     0.20 ms

  The GPU is idle in every case. The per-vertex loop is the frame. It is also
  per-VERTEX, not per-pixel, so it does not shrink with resolution — it is a
  fixed CPU tax that caps FPS no matter how fast the GPU is.

  WHY IT IS SAFE TO PARALLELISE

  `per_pixel` is, in practice, a pure function of (x, y, rad, ang) plus values
  held constant for the whole frame. A scan of 286 shipped `.milk` presets found
  84 with per-pixel code and ZERO that assign to `reg00-99`, `megabuf[]` or
  `gmegabuf[]` there. `IsPerPixelParallelSafe()` re-checks per preset anyway and
  the caller falls back to the serial loop when it says no — a wrong answer here
  is silent corruption, not a crash, so it is deliberately conservative.

  Two ns-eel2 facts this relies on, both verified in the vendored source:

    * `reg00`-`reg99` are PROCESS-global (`__nseel_global_regs`,
      `nseel_globalreg_list` in nseel-compiler.c), shared by every VM context.
      Replica VMs therefore see exactly the registers the single VM saw.
    * `NSEEL_VM_SetGRAM()` is never called in this codebase, so `gmegabuf` is
      already per-VM. `per_frame` and `per_pixel` already run on SEPARATE VMs
      (`m_pf_eel` / `m_pv_eel`) and already do not share it. Adding replicas of
      the per-vertex VM changes no semantics.

  Each worker owns a private VM and a private compile of the same source, so no
  EEL state is shared. Workers write disjoint runs of `m_verts[]`, split by grid
  row, so the vertex array needs no locking either.
*/

#ifndef MDROP_PV_WORKERS_H
#define MDROP_PV_WORKERS_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ns-eel.h"

namespace mdrop {

// Frame-constant per-vertex variables: written once per frame, read by every
// vertex. Kept as names so a replica VM can resolve its own storage for each.
// The per-vertex I/O set (x, y, rad, ang, zoom, ... sy) is NOT here — those are
// written per vertex and live in named fields on PvWorker.
inline const char* const* PvConstNames(int* count) {
  static const char* const kNames[] = {
    "time", "fps", "frame", "progress",
    "bass", "mid", "treb",
    "bass_att", "mid_att", "treb_att",
    "bass_smooth", "mid_smooth", "treb_smooth",
    "mousex", "mousey", "mousedown", "mouseclick",
    "meshx", "meshy", "pixelsx", "pixelsy", "aspectx", "aspecty",
    "q1",  "q2",  "q3",  "q4",  "q5",  "q6",  "q7",  "q8",
    "q9",  "q10", "q11", "q12", "q13", "q14", "q15", "q16",
    "q17", "q18", "q19", "q20", "q21", "q22", "q23", "q24",
    "q25", "q26", "q27", "q28", "q29", "q30", "q31", "q32",
  };
  if (count) *count = (int)(sizeof(kNames) / sizeof(kNames[0]));
  return kNames;
}

// One replica of the per-vertex VM. Owns its context and its compiled code.
struct PvWorker {
  NSEEL_VMCTX      vm   = nullptr;
  NSEEL_CODEHANDLE code = nullptr;

  // per-vertex inputs
  double *x = nullptr, *y = nullptr, *rad = nullptr, *ang = nullptr;
  // per-vertex in/out — seeded from the per-frame value, read back after execute
  double *zoom = nullptr, *zoomexp = nullptr, *rot = nullptr, *warp = nullptr;
  double *cx = nullptr, *cy = nullptr, *dx = nullptr, *dy = nullptr;
  double *sx = nullptr, *sy = nullptr;

  // frame constants, parallel to PvConstNames()
  std::vector<double*> konst;

  bool Valid() const { return vm && code; }
};

// What the per-vertex loop actually touches: one compiled handle and the
// fourteen variables written or read per vertex. Worker 0 binds the master VM,
// worker N>0 binds replica N-1, and the loop body is then identical for both.
struct PvBind {
  NSEEL_CODEHANDLE code = nullptr;
  double *x = nullptr, *y = nullptr, *rad = nullptr, *ang = nullptr;
  double *zoom = nullptr, *zoomexp = nullptr, *rot = nullptr, *warp = nullptr;
  double *cx = nullptr, *cy = nullptr, *dx = nullptr, *dy = nullptr;
  double *sx = nullptr, *sy = nullptr;
};

// `per_pixel` source that stores to state shared across vertices cannot be
// evaluated out of order. Returns false for any assignment to reg00-99,
// megabuf[...] or gmegabuf[...]. Compound assignment (+=, *=, ...) counts;
// comparison (==) does not.
//
// `src` is the stripped, comment-free source as handed to NSEEL_code_compile.
bool IsPerPixelParallelSafe(const char* src);

// A small persistent pool. The calling thread participates as worker 0, so
// `Workers()` counts it and a pool of size 1 never touches a thread at all.
//
// Threads are created once and parked on a condition variable; waking them per
// frame costs microseconds, where creating them would cost more than the work.
class PvThreadPool {
public:
  PvThreadPool() = default;
  ~PvThreadPool() { Stop(); }

  PvThreadPool(const PvThreadPool&) = delete;
  PvThreadPool& operator=(const PvThreadPool&) = delete;

  // `workers` counts the calling thread. <= 1 means "run inline, no threads".
  void Start(int workers);
  void Stop();

  int Workers() const { return m_workers; }

  // Invokes fn(i) for i in [0, Workers()), returning once all have finished.
  // fn must not throw; anything it throws is swallowed so the barrier cannot
  // deadlock, and is reported through Failed().
  void Run(const std::function<void(int)>& fn);

  bool Failed() const { return m_failed.load(std::memory_order_relaxed); }
  void ClearFailed() { m_failed.store(false, std::memory_order_relaxed); }

private:
  void WorkerLoop(int index);

  std::vector<std::thread>        m_threads;
  std::mutex                      m_mu;
  std::condition_variable         m_cv;
  const std::function<void(int)>* m_fn      = nullptr;
  unsigned long long              m_gen     = 0;
  std::atomic<int>                m_pending{0};
  std::atomic<bool>               m_failed{false};
  bool                            m_stop    = false;
  int                             m_workers = 1;
};

// How many workers this machine should use, counting the caller. Depends ONLY
// on core count, deliberately: the replica VMs are built when a preset compiles
// and the pool is sized separately, so if this varied with grid size the two
// could disagree after a mesh-size change and silently drop rows.
int PvChooseWorkerCount();

// Whether a grid of `vertexCount` vertices is big enough to be worth splitting.
// Below the threshold the wake-and-join costs more than it saves.
bool PvParallelWorthIt(int vertexCount);

} // namespace mdrop

#endif // MDROP_PV_WORKERS_H
