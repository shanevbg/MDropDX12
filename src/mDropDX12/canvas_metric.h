#ifndef MDROP_CANVAS_METRIC_H
#define MDROP_CANVAS_METRIC_H

#include <d3d12.h>
#include <wrl/client.h>

namespace mdrop {

class Engine;

// One reading of the presented frame.
struct CanvasSample {
  bool  valid    = false;
  float mean     = 0.0f;   // Rec.709 luminance, 0..1
  float variance = 0.0f;   // across the REDUCE_DIM^2 cells
};

// Measures the presented frame by reducing it to 8x8 and reading that back.
//
// Deliberately samples the PRESENTED frame, not the raw canvas (m_dx12VS[]):
// every threshold in the design doc was measured on presented frames, and it
// is also what a viewer perceives as broken. Sampling the canvas instead would
// invalidate all of those numbers.
//
// Reads are queued and collected RING_DEPTH samples later, so the render
// thread never blocks on the GPU.
//
// See docs/superpowers/specs/2026-08-23-canvas-runaway-detector-design.md.
class CanvasMetric {
public:
  static const UINT REDUCE_DIM = 8;
  static const UINT RING_DEPTH = 3;

  // The reduction has to copy the whole back buffer before it can sample it,
  // which at 2160x3840 is ~33 MB. Doing that every frame would burn ~1.3 GB/s
  // of bandwidth for a measurement only wanted twice a second, so the ENTIRE
  // reduce/readback is throttled to this interval -- not just the trace write.
  static const double SAMPLE_INTERVAL_MS;

  bool Init(Engine* e);
  void Release();

  // Once per frame, after the comp pass and before any UI/text is composited.
  // Self-guarding: does nothing until Init has succeeded, and throttles itself.
  void RecordFrame(Engine* e);

  CanvasSample Latest() const { return m_latest; }

  // Phase 1 trace collection. Off by default: this writes to disk, and the
  // point of Phase 1 is deliberate data-gathering runs, not always-on logging.
  void SetTraceEnabled(bool on);
  bool TraceEnabled() const { return m_traceEnabled; }

private:
  void Reduce(Engine* e);          // frame -> 8x8 RT
  void QueueReadback(Engine* e);   // 8x8 RT -> ring slot
  void CollectReadback();          // ring slot -> m_latest
  void WriteTraceRow(Engine* e);

  Microsoft::WRL::ComPtr<ID3D12Resource> m_readback[RING_DEPTH];
  UINT         m_ringIndex     = 0;
  UINT64       m_samplesTaken  = 0;
  UINT         m_rowPitch      = 0;
  bool         m_ready         = false;
  double       m_lastSampleMs  = 0.0;
  bool         m_traceEnabled  = false;
  double       m_traceStartMs  = 0.0;
  CanvasSample m_latest;

  // Preset age is tracked here rather than read from Engine::m_fPresetStartTime,
  // which was observed reading as "just started" on every sampled frame. Phase 2
  // aligns trajectories by preset age, so this column has to be trustworthy.
  char   m_tracePreset[260] = {0};
  double m_presetStartMs    = 0.0;
};

} // namespace mdrop

#endif // MDROP_CANVAS_METRIC_H
