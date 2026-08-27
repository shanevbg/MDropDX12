#include "engine.h"
#include "canvas_metric.h"
#include "utility.h"

using Microsoft::WRL::ComPtr;

namespace mdrop {

const double CanvasMetric::SAMPLE_INTERVAL_MS = 500.0;

static double NowMs()
{
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

bool CanvasMetric::Init(Engine* e)
{
  Release();
  if (!e || !e->m_lpDX || !e->m_lpDX->m_device) return false;

  // Readback rows are 256-byte aligned by D3D12.
  const UINT rowBytes = REDUCE_DIM * 4;
  m_rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
               ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width            = (UINT64)m_rowPitch * REDUCE_DIM;
  desc.Height           = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels        = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  for (UINT i = 0; i < RING_DEPTH; i++) {
    HRESULT hr = e->m_lpDX->m_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback[i]));
    if (FAILED(hr)) {
      DebugLogA("CanvasMetric: readback buffer create FAILED", LOG_ERROR);
      Release();
      return false;
    }
  }

  m_ringIndex    = 0;
  m_samplesTaken = 0;
  m_lastSampleMs = 0.0;
  m_latest       = CanvasSample{};
  m_ready        = true;
  DebugLogA("CanvasMetric: initialised", LOG_INFO);
  return true;
}

void CanvasMetric::Release()
{
  for (UINT i = 0; i < RING_DEPTH; i++) m_readback[i].Reset();
  m_ready        = false;
  m_ringIndex    = 0;
  m_samplesTaken = 0;
  m_lastSampleMs = 0.0;
  m_latest       = CanvasSample{};
  m_tracePreset[0] = 0;
  m_presetStartMs  = 0.0;
}

void CanvasMetric::RecordFrame(Engine* e)
{
  if (!m_ready || !e || !e->m_lpDX || !e->m_lpDX->m_ready) return;
  if (!e->m_pCanvasReducePSO || !e->m_canvasReduceTex.IsValid()) return;
  if (!e->m_canvasSrcTex.IsValid()) return;

  // Throttle the whole pass, not just the trace write: the reduction has to
  // copy the entire back buffer before it can sample it.
  const double now = NowMs();
  if (m_lastSampleMs > 0.0 && (now - m_lastSampleMs) < SAMPLE_INTERVAL_MS) return;
  m_lastSampleMs = now;

  // CopyResource needs identical dimensions; after a resize the src texture is
  // stale for a frame or two.
  if (e->m_canvasSrcTex.width  != (UINT)e->m_lpDX->m_backbuffer_width ||
      e->m_canvasSrcTex.height != (UINT)e->m_lpDX->m_backbuffer_height)
    return;

  Reduce(e);
  QueueReadback(e);
  // Only collect once the slot we are about to read has actually been written.
  if (m_samplesTaken >= RING_DEPTH) CollectReadback();

  m_samplesTaken++;
  m_ringIndex = (m_ringIndex + 1) % RING_DEPTH;

  if (m_traceEnabled) WriteTraceRow(e);
}

void CanvasMetric::SetTraceEnabled(bool on)
{
  if (on == m_traceEnabled) return;
  m_traceEnabled = on;
  if (on) {
    m_traceStartMs  = NowMs();
    m_tracePreset[0] = 0;
    m_presetStartMs = 0.0;
    DebugLogDiagTruncate(L"diag_canvas_trace.csv");
    DebugLogDiagAppend(L"diag_canvas_trace.csv",
                       "t_ms,preset_ms,tex_w,tex_h,mean,variance,preset\n");
    DebugLogA("CanvasMetric: trace started", LOG_INFO);
  } else {
    DebugLogA("CanvasMetric: trace stopped", LOG_INFO);
  }
}

void CanvasMetric::WriteTraceRow(Engine* e)
{
  if (!m_latest.valid || !e) return;

  // Sampling is already throttled to SAMPLE_INTERVAL_MS, so every collected
  // sample is traced and the trace needs no throttle of its own.
  const double t = NowMs() - m_traceStartMs;

  // The preset name is written last and stripped of commas: preset filenames
  // routinely contain them and would otherwise shift every later column.
  char presetUtf8[260] = {0};
  WideCharToMultiByte(CP_UTF8, 0, e->m_szCurrentPresetFile, -1,
                      presetUtf8, sizeof(presetUtf8) - 1, nullptr, nullptr);
  for (char* p = presetUtf8; *p; ++p)
    if (*p == ',' || *p == '\n' || *p == '\r') *p = ' ';

  if (strcmp(presetUtf8, m_tracePreset) != 0) {
    strcpy_s(m_tracePreset, presetUtf8);
    m_presetStartMs = t;
  }
  const double presetMs = t - m_presetStartMs;

  char row[512];
  sprintf_s(row, "%.0f,%.0f,%d,%d,%.6f,%.6f,%s\n",
            t, presetMs, e->m_nTexSizeX, e->m_nTexSizeY,
            m_latest.mean, m_latest.variance, presetUtf8);
  DebugLogDiagAppend(L"diag_canvas_trace.csv", row);
}

void CanvasMetric::Reduce(Engine* e)
{
  auto* cl = e->m_lpDX->m_commandList.Get();
  ID3D12Resource* pBackBuf = e->m_lpDX->m_renderTargets[e->m_lpDX->m_frameIndex].Get();

  // Back buffer RENDER_TARGET -> COPY_SOURCE, copy into the sampling texture,
  // then back. Same dance as RenderInjectEffect: a swap-chain buffer cannot be
  // bound as an SRV directly.
  D3D12_RESOURCE_BARRIER toSrc = {};
  toSrc.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toSrc.Transition.pResource   = pBackBuf;
  toSrc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toSrc.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &toSrc);

  e->m_lpDX->TransitionResource(e->m_canvasSrcTex, D3D12_RESOURCE_STATE_COPY_DEST);
  cl->CopyResource(e->m_canvasSrcTex.resource.Get(), pBackBuf);
  e->m_lpDX->TransitionResource(e->m_canvasSrcTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  D3D12_RESOURCE_BARRIER toRT = toSrc;
  toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
  cl->ResourceBarrier(1, &toRT);

  // Draw the reduction into the 8x8 RT.
  e->m_lpDX->TransitionResource(e->m_canvasReduceTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv =
      e->m_lpDX->GetRtvCpuHandleAt(e->m_canvasReduceTex.rtvIndex);
  cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  SetViewportAndScissor(cl, REDUCE_DIM, REDUCE_DIM);

  cl->SetPipelineState(e->m_pCanvasReducePSO.Get());
  cl->SetGraphicsRootSignature(e->m_lpDX->m_rootSignature.Get());
  ID3D12DescriptorHeap* heaps[] = { e->m_lpDX->m_srvHeap.Get() };
  cl->SetDescriptorHeaps(_countof(heaps), heaps);
  cl->SetGraphicsRootDescriptorTable(1, e->m_lpDX->GetBindingBlockGpuHandle(e->m_canvasSrcTex));

  MYVERTEX v[4];
  ZeroMemory(v, sizeof(v));
  v[0].x = -1.f; v[0].y =  1.f; v[0].z = 0.f; v[0].Diffuse = 0xFFFFFFFF; v[0].tu = 0.f; v[0].tv = 0.f;
  v[1].x =  1.f; v[1].y =  1.f; v[1].z = 0.f; v[1].Diffuse = 0xFFFFFFFF; v[1].tu = 1.f; v[1].tv = 0.f;
  v[2].x = -1.f; v[2].y = -1.f; v[2].z = 0.f; v[2].Diffuse = 0xFFFFFFFF; v[2].tu = 0.f; v[2].tv = 1.f;
  v[3].x =  1.f; v[3].y = -1.f; v[3].z = 0.f; v[3].Diffuse = 0xFFFFFFFF; v[3].tu = 1.f; v[3].tv = 1.f;
  // Note the argument order: vertices before count.
  e->m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX));

  // Restore the back buffer as the render target so the UI pass that follows
  // draws where it expects to. Forgetting this sends all HUD text to the 8x8.
  D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = e->m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  bbRtv.ptr += (SIZE_T)e->m_lpDX->m_frameIndex * e->m_lpDX->m_rtvDescriptorSize;
  cl->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
  SetViewportAndScissor(cl, (UINT)e->m_lpDX->m_backbuffer_width,
                            (UINT)e->m_lpDX->m_backbuffer_height);
}

void CanvasMetric::QueueReadback(Engine* e)
{
  auto* cl = e->m_lpDX->m_commandList.Get();
  e->m_lpDX->TransitionResource(e->m_canvasReduceTex, D3D12_RESOURCE_STATE_COPY_SOURCE);

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource        = e->m_canvasReduceTex.resource.Get();
  src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource                          = m_readback[m_ringIndex].Get();
  dst.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset             = 0;
  dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
  dst.PlacedFootprint.Footprint.Width    = REDUCE_DIM;
  dst.PlacedFootprint.Footprint.Height   = REDUCE_DIM;
  dst.PlacedFootprint.Footprint.Depth    = 1;
  dst.PlacedFootprint.Footprint.RowPitch = m_rowPitch;

  cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

void CanvasMetric::CollectReadback()
{
  // The slot one ahead in the ring is the oldest -- its copy was submitted
  // RING_DEPTH samples ago (>= 1.5 s) and has long since retired.
  const UINT slot = (m_ringIndex + 1) % RING_DEPTH;
  ID3D12Resource* res = m_readback[slot].Get();
  if (!res) return;

  void* mapped = nullptr;
  D3D12_RANGE readRange{ 0, (SIZE_T)m_rowPitch * REDUCE_DIM };
  if (FAILED(res->Map(0, &readRange, &mapped)) || !mapped) return;

  const auto* bytes = static_cast<const unsigned char*>(mapped);
  float lum[REDUCE_DIM * REDUCE_DIM];
  int n = 0;
  for (UINT y = 0; y < REDUCE_DIM; y++) {
    const unsigned char* row = bytes + (size_t)y * m_rowPitch;
    for (UINT x = 0; x < REDUCE_DIM; x++) {
      const float r = row[x * 4 + 0] / 255.0f;
      const float g = row[x * 4 + 1] / 255.0f;
      const float b = row[x * 4 + 2] / 255.0f;
      lum[n++] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }
  }

  D3D12_RANGE noWrite{ 0, 0 };
  res->Unmap(0, &noWrite);

  float sum = 0.0f;
  for (int i = 0; i < n; i++) sum += lum[i];
  const float mean = sum / (float)n;

  float acc = 0.0f;
  for (int i = 0; i < n; i++) { const float d = lum[i] - mean; acc += d * d; }

  m_latest.mean     = mean;
  m_latest.variance = acc / (float)n;
  m_latest.valid    = true;
}

} // namespace mdrop
