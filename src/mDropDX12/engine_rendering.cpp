/*
  Plugin module: HUD/Overlay Rendering & Notifications
  Extracted from engine.cpp for maintainability.
  Contains: MyRenderUI, DrawTooltip, RenderInjectEffect, notification functions
*/

#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "shell_defines.h"
#include "wasabi.h"
#include <assert.h>
#include <strsafe.h>
#include <Windows.h>
#include <cstdint>
#include "config_store.h"
#include "format_to.h"

#define FRAND ((rand() % 7381)/7380.0f)

namespace mdrop {

extern Engine g_engine;
extern float timetick;

void Engine::CopyBackbufferToFeedback()
{
  // Single-pass feedback: comp now renders directly to FLOAT16 feedback buffer
  // and blits to the backbuffer, so no copy is needed.
  // Two-pass (Buffer A + Image): Buffer A writes directly to feedback.
  // This function is kept as a stub; the old backbuffer→feedback copy was removed
  // because the backbuffer is UNORM (clamps negatives) while feedback is FLOAT16.
  return;

  int writeIdx = 1 - m_nFeedbackIdx;
  DX12Texture& fbWrite = m_dx12Feedback[writeIdx];
  if (!fbWrite.IsValid()) return;
  if (!m_lpDX || !m_lpDX->m_ready) return;

  // Size guard: feedback texture must match backbuffer for CopyResource
  if (fbWrite.width  != (UINT)m_lpDX->m_backbuffer_width ||
      fbWrite.height != (UINT)m_lpDX->m_backbuffer_height)
    return;

  auto* cl = m_lpDX->m_commandList.Get();
  ID3D12Resource* pBackBuf = m_lpDX->m_renderTargets[m_lpDX->m_frameIndex].Get();

  // 1. Transition backbuffer RENDER_TARGET → COPY_SOURCE
  D3D12_RESOURCE_BARRIER toSrc = {};
  toSrc.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toSrc.Transition.pResource        = pBackBuf;
  toSrc.Transition.StateBefore      = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toSrc.Transition.StateAfter       = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toSrc.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &toSrc);

  // 2. Transition feedback write buffer → COPY_DEST
  m_lpDX->TransitionResource(fbWrite, D3D12_RESOURCE_STATE_COPY_DEST);

  // 3. Copy backbuffer → feedback write buffer
  cl->CopyResource(fbWrite.resource.Get(), pBackBuf);

  // 4. Transition feedback COPY_DEST → PIXEL_SHADER_RESOURCE
  m_lpDX->TransitionResource(fbWrite, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // 5. Transition backbuffer COPY_SOURCE → RENDER_TARGET
  D3D12_RESOURCE_BARRIER toRT = {};
  toRT.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toRT.Transition.pResource        = pBackBuf;
  toRT.Transition.StateBefore      = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toRT.Transition.StateAfter       = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toRT.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &toRT);
}

void Engine::RenderInjectEffect()
{
  if (m_lpDX) m_lpDX->BeginStage("injectfx");   // #222 stage map

  // Post-process pass: applies the F11 inject effect and (for non-shader presets)
  // per-preset brighten/darken/solarize/invert on the composite back buffer.
  // Copies the back buffer to an intermediate texture, then draws it back with an effect shader.

  // Build per-preset effect bitmask from per-frame equation outputs.
  // In MilkDrop/DX9, brighten/darken/solarize/invert are only applied by
  // ShowToUser_NoShaders(), which runs only for non-shader presets
  // (PSVERSION_COMP=0). When a comp shader is present, ShowToUser_Shaders() is
  // called instead, which does NOT apply these effects. Match that behavior:
  // skip per-preset effects when comp PSO is active.
  //
  // Those two names are MilkDrop's, not ours any more -- our dead DX9 copies of
  // them were deleted in issue 17, so do not go looking for them in
  // milkdropfs.cpp. This code IS the DX12 reimplementation of that rule.
  UINT presetFxMask = 0;
  // The comp pass degrades to a plain textured quad only when there is no comp
  // PSO at all (milkdropfs.cpp, PSO_TEXTURED_MYVERTEX): shaders switched off by
  // MaxPSVersion=0 or the old-GPU blacklist, or the blank startup state. That,
  // and only that, is this engine's ShowToUser_NoShaders.
  //
  // Every preset that loads has a comp PSO -- including one with no
  // [comp_shader] section, because Import auto-generates the shader and forces
  // MD2_PS_2_0 so it gets compiled (state.cpp), and GenCompPShaderText bakes
  // brighten/darken/solarize/invert straight into that generated body
  // (engine_shaders.cpp). Those presets get their effects from the shader.
  // Applying them here as well would be MD3's two different curves composed:
  // the generator uses sqrt and *4, this pass uses c(2-c) and *2.
  const bool bNoCompPSO = !m_dx12CompPSO;
  // While blending, the OLD preset gets a vote. MilkDrop only ever applies
  // these effects from ShowToUser_NoShaders, and its dispatch calls that
  // function only when a half lacks a comp shader (milkdropfs.cpp): with comp
  // shaders on both sides it runs ShowToUser_Shaders twice and applies no
  // classic post-FX at all. A .milk2 is a frozen blend, so both halves are
  // live every frame -- and the flags driving this pass are snapped from the
  // OLD state below m_fSnapPoint, which is how PRESET1's bInvert came to
  // repaint a composite that PRESET1 does not even own.
  //
  // "Has a comp shader" is spelled the way the rest of this codebase spells it
  // (milkdropfs.cpp:2277, 3346, 3379) and the way MilkDrop does
  // (MilkDrop3 milkdropfs.cpp:864, m_nCompPSVersion > 0). It cannot be a test
  // on m_szCompShadersText alone: auto-generation fills that in for every
  // preset, so the text is non-empty for all of them.
  const bool bOldHasCompShader =
      m_pState && m_pState->m_bBlending && m_pOldState &&
      m_pOldState->m_nCompPSVersion > 0 && !m_pOldState->m_bAutoGenCompShader;
  const bool classicPost = bNoCompPSO && !bOldHasCompShader;
  if (m_pState && classicPost) {
    if (m_pState->var_pf_brighten && *m_pState->var_pf_brighten > 0.5) presetFxMask |= 1u;
    if (m_pState->var_pf_darken   && *m_pState->var_pf_darken   > 0.5) presetFxMask |= 2u;
    if (m_pState->var_pf_solarize && *m_pState->var_pf_solarize > 0.5) presetFxMask |= 4u;
    if (m_pState->var_pf_invert   && *m_pState->var_pf_invert   > 0.5) presetFxMask |= 8u;
  }

  // Which clause opened the gate, and what it let through. Logged on CHANGE
  // only -- this runs every frame. Worth keeping: this pass silently rewrites
  // the whole composite, and when it fires wrongly the symptom (a preset that
  // renders inverted or washed out) points nowhere near here. The .milk2 case
  // is the trap: the flags are per-frame vars snapped from the OLD state
  // (milkdropfs.cpp, m_fSnapPoint), so m_pState->m_bInvert can read 0 while
  // *m_pState->var_pf_invert reads 1 -- do not diagnose this from STATE output.
  {
    static UINT s_lastMask = 0xFFFFFFFFu;
    static int  s_lastGate = -1;
    const int gate = (bNoCompPSO ? 1 : 0) | (bOldHasCompShader ? 2 : 0);
    if (presetFxMask != s_lastMask || gate != s_lastGate) {
      s_lastMask = presetFxMask; s_lastGate = gate;
      DLOG_WARN("InjectFX: mask=%u (brighten=%d darken=%d solarize=%d invert=%d) "
                "gate: noCompPSO=%d classicPost=%d oldHasComp=%d  preset=%ls",
                presetFxMask, (presetFxMask & 1u) ? 1 : 0, (presetFxMask & 2u) ? 1 : 0,
                (presetFxMask & 4u) ? 1 : 0, (presetFxMask & 8u) ? 1 : 0,
                bNoCompPSO ? 1 : 0, classicPost ? 1 : 0,
                bOldHasCompShader ? 1 : 0,
                m_pState ? m_pState->m_szDesc : L"(none)");
    }
  }

  // Shadertoy sRGB gamma: disabled. Shadertoy.com actually uses RGBA8 (NOT sRGB).
  // Shaders that need gamma encode it manually (e.g. pow(col, 0.45)).
  // Applying sRGB here would double-gamma those shaders.
  UINT srgbGamma = 0u;

  // Skip if nothing to do
  if (m_nInjectEffectMode == 0 && presetFxMask == 0 && srgbGamma == 0) return;
  if (!m_pInjectEffectPSO || !m_injectEffectTex.IsValid()) return;
  if (!m_lpDX || !m_lpDX->m_ready) return;

  // Size guard: intermediate texture must match back buffer for CopyResource
  if (m_injectEffectTex.width  != (UINT)m_lpDX->m_backbuffer_width ||
      m_injectEffectTex.height != (UINT)m_lpDX->m_backbuffer_height)
    return;

  auto* cl = m_lpDX->m_commandList.Get();
  ID3D12Resource* pBackBuf = m_lpDX->m_renderTargets[m_lpDX->m_frameIndex].Get();

  // 1. Transition back buffer RENDER_TARGET → COPY_SOURCE
  D3D12_RESOURCE_BARRIER toSrc = {};
  toSrc.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toSrc.Transition.pResource        = pBackBuf;
  toSrc.Transition.StateBefore      = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toSrc.Transition.StateAfter       = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toSrc.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &toSrc);

  // 2. Transition inject texture → COPY_DEST
  m_lpDX->TransitionResource(m_injectEffectTex, D3D12_RESOURCE_STATE_COPY_DEST);

  // 3. Copy back buffer to inject texture
  cl->CopyResource(m_injectEffectTex.resource.Get(), pBackBuf);

  // 4. Transition inject texture COPY_DEST → PIXEL_SHADER_RESOURCE
  m_lpDX->TransitionResource(m_injectEffectTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // 5. Transition back buffer COPY_SOURCE → RENDER_TARGET
  D3D12_RESOURCE_BARRIER toRT = {};
  toRT.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toRT.Transition.pResource        = pBackBuf;
  toRT.Transition.StateBefore      = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toRT.Transition.StateAfter       = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toRT.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &toRT);

  // 6. Re-bind back buffer as render target
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtvHandle.ptr += (SIZE_T)m_lpDX->m_frameIndex * m_lpDX->m_rtvDescriptorSize;
  cl->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

  // 7. Set viewport + scissor
  SetViewportAndScissor(cl, m_lpDX->m_backbuffer_width, m_lpDX->m_backbuffer_height);

  // 8. Set PSO + root signature
  cl->SetPipelineState(m_pInjectEffectPSO.Get());
  cl->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  // 9. Set descriptor heap
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cl->SetDescriptorHeaps(_countof(heaps), heaps);

  // 10. Upload mode CBV (b0 = uint4 mode)
  //     mode.x = F11 inject effect, mode.y = per-preset effect bitmask, mode.z = sRGB gamma
  struct { UINT mode[4]; } cbData = { { (UINT)m_nInjectEffectMode, presetFxMask, srgbGamma, 0u } };
  D3D12_GPU_VIRTUAL_ADDRESS cbva = m_lpDX->UploadConstantBuffer(&cbData, sizeof(cbData));
  if (cbva)
    cl->SetGraphicsRootConstantBufferView(0, cbva);

  // 11. Bind inject texture SRV (descriptor table t0..t15)
  cl->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBindingBlockGpuHandle(m_injectEffectTex));

  // 12. Draw fullscreen quad (BlurVS reads tu/tv from MYVERTEX as TEXCOORD0)
  MYVERTEX v[4];
  ZeroMemory(v, sizeof(v));
  v[0].x = -1.f; v[0].y =  1.f; v[0].z = 0.f; v[0].Diffuse = 0xFFFFFFFF; v[0].tu = 0.f; v[0].tv = 0.f;
  v[1].x =  1.f; v[1].y =  1.f; v[1].z = 0.f; v[1].Diffuse = 0xFFFFFFFF; v[1].tu = 1.f; v[1].tv = 0.f;
  v[2].x = -1.f; v[2].y = -1.f; v[2].z = 0.f; v[2].Diffuse = 0xFFFFFFFF; v[2].tu = 0.f; v[2].tv = 1.f;
  v[3].x =  1.f; v[3].y = -1.f; v[3].z = 0.f; v[3].Diffuse = 0xFFFFFFFF; v[3].tu = 1.f; v[3].tv = 1.f;
  m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX));
}

//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------

void Engine::DrawTooltip(wchar_t* str, int xR, int yB) {
  // draws a string in the lower-right corner of the screen.
  // note: ID3DXFont handles DT_RIGHT and DT_BOTTOM *very poorly*.
  //       it is best to calculate the size of the text first,
  //       then place it in the right spot.
  // note: use DT_WORDBREAK instead of DT_WORD_ELLIPSES, otherwise certain fonts'
  //       calcrect (for the dark box) will be wrong.

  RECT r, r2;
  SetRect(&r, 0, 0, xR - TEXT_MARGIN * 2, 2048);
  m_text.DrawTextW(GetFont(TOOLTIP_FONT), str, -1, &r, DT_CALCRECT, 0xFFFFFFFF, false);
  r2.bottom = yB - TEXT_MARGIN;
  r2.right = xR - TEXT_MARGIN;
  r2.left = r2.right - (r.right - r.left);
  r2.top = r2.bottom - (r.bottom - r.top);
  RECT r3 = r2; r3.left -= 4; r3.top -= 2; r3.right += 2; r3.bottom += 2;
  DrawDarkTranslucentBox(&r3);
  m_text.DrawTextW(GetFont(TOOLTIP_FONT), str, -1, &r2, 0, 0xFFFFFFFF, false);
}

#define MTO_UPPER_RIGHT 0
#define MTO_UPPER_LEFT  1
#define MTO_LOWER_RIGHT 2
#define MTO_LOWER_LEFT  3

#undef SelectFont  // avoid conflict with wingdi.h macro
#define SelectFont(n) { \
    pFont = GetFont(n); \
    h = GetFontHeight(n); \
}

#define MyTextOut_BGCOLOR(str, corner, bDarkBox, boxColor) { \
    SetRect(&r, 0, 0, xR-xL, 2048); \
	m_text.DrawTextW(pFont, str, -1, &r, DT_NOPREFIX | ((corner == MTO_UPPER_RIGHT)?0:DT_SINGLELINE) | DT_END_ELLIPSIS | DT_CALCRECT | ((corner == MTO_UPPER_RIGHT) ? DT_RIGHT : 0), 0xFFFFFFFF, false, boxColor); \
    int w = r.right - r.left; \
    if (w > xR-xL) w = xR-xL; \
    if      (corner == MTO_UPPER_LEFT ) SetRect(&r, xL, *upper_left_corner_y, xL+w, *upper_left_corner_y + h); \
    else if (corner == MTO_UPPER_RIGHT) SetRect(&r, xR-w, *upper_right_corner_y, xR, *upper_right_corner_y + h); \
    else if (corner == MTO_LOWER_LEFT ) SetRect(&r, xL, *lower_left_corner_y - h, xL+w, *lower_left_corner_y); \
    else if (corner == MTO_LOWER_RIGHT) SetRect(&r, xR-w, *lower_right_corner_y - h, xR, *lower_right_corner_y); \
	m_text.DrawTextW(pFont, str, -1, &r, DT_NOPREFIX | ((corner == MTO_UPPER_RIGHT)?0:DT_SINGLELINE) | DT_END_ELLIPSIS | ((corner == MTO_UPPER_RIGHT) ? DT_RIGHT: 0), 0xFFFFFFFF, bDarkBox, boxColor); \
    if      (corner == MTO_UPPER_LEFT ) *upper_left_corner_y  += h; \
    else if (corner == MTO_UPPER_RIGHT) *upper_right_corner_y += h; \
    else if (corner == MTO_LOWER_LEFT ) *lower_left_corner_y  -= h; \
    else if (corner == MTO_LOWER_RIGHT) *lower_right_corner_y -= h; \
}

#define MyTextOut_Color(str, corner, color) { \
    SetRect(&r, 0, 0, xR-xL, 2048); \
	m_text.DrawTextW(pFont, str, -1, &r, DT_NOPREFIX | ((corner == MTO_UPPER_RIGHT)?0:DT_SINGLELINE) | DT_END_ELLIPSIS | DT_CALCRECT | ((corner == MTO_UPPER_RIGHT) ? DT_RIGHT : 0), color, false, 0xFF000000); \
    int w = r.right - r.left; \
    if (w > xR-xL) w = xR-xL; \
    if      (corner == MTO_UPPER_LEFT ) SetRect(&r, xL, *upper_left_corner_y, xL+w, *upper_left_corner_y + h); \
    else if (corner == MTO_UPPER_RIGHT) SetRect(&r, xR-w, *upper_right_corner_y, xR, *upper_right_corner_y + h); \
    else if (corner == MTO_LOWER_LEFT ) SetRect(&r, xL, *lower_left_corner_y - h, xL+w, *lower_left_corner_y); \
    else if (corner == MTO_LOWER_RIGHT) SetRect(&r, xR-w, *lower_right_corner_y - h, xR, *lower_right_corner_y); \
	m_text.DrawTextW(pFont, str, -1, &r, DT_NOPREFIX | ((corner == MTO_UPPER_RIGHT)?0:DT_SINGLELINE) | DT_END_ELLIPSIS | ((corner == MTO_UPPER_RIGHT) ? DT_RIGHT: 0), color, false, 0xFF000000); \
    if      (corner == MTO_UPPER_LEFT ) *upper_left_corner_y  += h; \
    else if (corner == MTO_UPPER_RIGHT) *upper_right_corner_y += h; \
    else if (corner == MTO_LOWER_LEFT ) *lower_left_corner_y  -= h; \
    else if (corner == MTO_LOWER_RIGHT) *lower_right_corner_y -= h; \
}

#define MyTextOut(str, corner, bDarkBox) MyTextOut_BGCOLOR(str, corner, bDarkBox, 0xFF000000)

#define MyTextOut_Shadow(str, corner) { \
    /* calc rect size */        \
    SetRect(&r, 0, 0, xR-xL, 2048); \
	m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS | DT_CALCRECT, 0xFFFFFFFF, false, 0xFF000000); \
    int w = r.right - r.left; \
    if (w > xR-xL) w = xR-xL; \
    /* DT_RIGHT required for right corners: tight rect + left-align looks OK on primary, \
       but mirror scale uses sx for rect and sy for glyphs — without DT_RIGHT FPS drifts. */ \
    const DWORD alignFlags = ((corner == MTO_UPPER_RIGHT || corner == MTO_LOWER_RIGHT) ? DT_RIGHT : 0); \
    /* first the shadow */         \
    if      (corner == MTO_UPPER_LEFT ) SetRect(&r, xL, *upper_left_corner_y, xL+w, *upper_left_corner_y + h); \
    else if (corner == MTO_UPPER_RIGHT) SetRect(&r, xR-w, *upper_right_corner_y, xR, *upper_right_corner_y + h); \
    else if (corner == MTO_LOWER_LEFT ) SetRect(&r, xL, *lower_left_corner_y - h, xL+w, *lower_left_corner_y); \
    else if (corner == MTO_LOWER_RIGHT) SetRect(&r, xR-w, *lower_right_corner_y - h, xR, *lower_right_corner_y); \
    r.top += 1; r.left += 1;      \
    m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS | alignFlags, 0xFF000000, false, 0xFF000000); \
    /* now draw real text */            \
    r.top -= 1; r.left -= 1;       \
	m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS | alignFlags, 0xFFFFFFFF, false, 0xFF000000); \
    if      (corner == MTO_UPPER_LEFT ) *upper_left_corner_y  += h; \
    else if (corner == MTO_UPPER_RIGHT) *upper_right_corner_y += h; \
    else if (corner == MTO_LOWER_LEFT ) *lower_left_corner_y  -= h; \
    else if (corner == MTO_LOWER_RIGHT) *lower_right_corner_y -= h; \
}

#define MyTextOut_Shadow_Color(str, corner, color) { \
    /* calc rect size */        \
    SetRect(&r, 0, 0, xR-xL, 2048); \
	m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS | DT_CALCRECT, color, false, 0xFF000000); \
    int w = r.right - r.left; \
    if (w > xR-xL) w = xR-xL; \
    const DWORD alignFlags = ((corner == MTO_UPPER_RIGHT || corner == MTO_LOWER_RIGHT) ? DT_RIGHT : 0); \
    /* first the shadow */         \
    if      (corner == MTO_UPPER_LEFT ) SetRect(&r, xL, *upper_left_corner_y, xL+w, *upper_left_corner_y + h); \
    else if (corner == MTO_UPPER_RIGHT) SetRect(&r, xR-w, *upper_right_corner_y, xR, *upper_right_corner_y + h); \
    else if (corner == MTO_LOWER_LEFT ) SetRect(&r, xL, *lower_left_corner_y - h, xL+w, *lower_left_corner_y); \
    else if (corner == MTO_LOWER_RIGHT) SetRect(&r, xR-w, *lower_right_corner_y - h, xR, *lower_right_corner_y); \
    r.top += 1; r.left += 1;      \
    m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS | alignFlags, 0xFF000000, false, 0xFF000000); \
    /* now draw real text */            \
    r.top -= 1; r.left -= 1;       \
	m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS | alignFlags, color, false, 0xFF000000); \
    if      (corner == MTO_UPPER_LEFT ) *upper_left_corner_y  += h; \
    else if (corner == MTO_UPPER_RIGHT) *upper_right_corner_y += h; \
    else if (corner == MTO_LOWER_LEFT ) *lower_left_corner_y  -= h; \
    else if (corner == MTO_LOWER_RIGHT) *lower_right_corner_y -= h; \
}

void Engine::OnAltK() {
  wchar_t msg[64];
  AddNotification(wasabiApiLangString(IDS_PLEASE_EXIT_VIS_BEFORE_RUNNING_CONFIG_PANEL, msg));
}

void Engine::AddNotification(wchar_t* szMsg) {
  g_engine.AddError(szMsg, 3.0F, ERR_NOTIFY, m_fontinfo[SIMPLE_FONT].bBold);
}

void Engine::AddNotification(wchar_t* szMsg, float time) {
  g_engine.AddError(szMsg, time, ERR_NOTIFY, m_fontinfo[SIMPLE_FONT].bBold);
}

void Engine::AddNotificationAudioDevice() {
  std::wstring statusMessage;
  if (m_szAudioDeviceDisplayName[0] != L'\0') {
    statusMessage = m_szAudioDeviceDisplayName;
  }
  else if (g_engine.m_szAudioDeviceDisplayName[0] != L'\0') {
    statusMessage = g_engine.m_szAudioDeviceDisplayName;
  }
  else if (g_engine.m_szAudioDevice[0] != L'\0') {
    statusMessage = g_engine.m_szAudioDevice;
  }

  int effectiveType = m_nAudioDeviceActiveType;
  if (effectiveType == 0) {
    effectiveType = m_nAudioDeviceRequestType;
  }

  const wchar_t* tag = nullptr;
  if (effectiveType == 1) {
    tag = L" [In]";
  }
  else if (effectiveType == 2) {
    tag = L" [Out]";
  }

  if (!statusMessage.empty() && tag != nullptr) {
    if (statusMessage.find(tag) == std::wstring::npos) {
      statusMessage += tag;
    }
  }

  if (!statusMessage.empty()) {
    AddNotification(statusMessage.data());
  }
  else {
    AddNotification(g_engine.m_szAudioDeviceDisplayName);
  }
}

void Engine::AddError(wchar_t* szMsg, float fDuration, int category, bool bBold) {
  DebugLogW(szMsg, LOG_WARN);
  assert(category != ERR_ALL);

  // Built before the lock: GetTime() and the wstring copy have no business
  // inside it, and this runs from 77 call sites on every thread in the process.
  ErrorMsg x;
  x.msg = szMsg;
  x.birthTime = GetTime();
  x.expireTime = x.birthTime + fDuration;
  x.category = category;
  x.bBold = bBold;
  x.bSentToRemote = false;
  x.color = 0; // default font color

  std::lock_guard<std::mutex> lk(m_errorsMutex);
  if (category == ERR_NOTIFY)
    ClearErrorsLocked(category);   // NOT ClearErrors -- we already hold the lock
  x.id = m_nextErrorId++;
  m_errors.push_back(x);
}

void Engine::AddNotificationColored(wchar_t* szMsg, float time, DWORD color) {
  DebugLogW(szMsg);

  ErrorMsg x;
  x.msg = szMsg;
  x.birthTime = GetTime();
  x.expireTime = x.birthTime + time;
  x.category = ERR_NOTIFY;
  x.bBold = true;
  x.bSentToRemote = false;
  x.color = color;

  std::lock_guard<std::mutex> lk(m_errorsMutex);
  ClearErrorsLocked(ERR_NOTIFY);
  x.id = m_nextErrorId++;
  m_errors.push_back(x);
}

void Engine::ClearErrors(int category)  // 0=all categories
{
  std::lock_guard<std::mutex> lk(m_errorsMutex);
  ClearErrorsLocked(category);
}

void Engine::ClearErrorsLocked(int category)
{
  int N = (int)m_errors.size();
  for (int i = 0; i < N; i++) {
    int cat = m_errors[i].category;
    // Track info (BOTTOM_EXTRA) has its own bucket — only cleared explicitly
    if (category == ERR_ALL &&
        (cat == ERR_MSG_BOTTOM_EXTRA_1 || cat == ERR_MSG_BOTTOM_EXTRA_2 || cat == ERR_MSG_BOTTOM_EXTRA_3))
      continue;
    if (category == ERR_ALL || cat == category) {
      m_errors.erase(m_errors.begin() + i);
      i--;
      N--;
    }
  }
}

void Engine::MyRenderUI(
  int* upper_left_corner_y,  // increment me!
  int* upper_right_corner_y, // increment me!
  int* lower_left_corner_y,  // decrement me!
  int* lower_right_corner_y, // decrement me!
  int xL,
  int xR
) {
  // draw text messages directly to the back buffer.
  // when you draw text into one of the four corners,
  //   draw the text at the current 'y' value for that corner
  //   (one of the first 4 params to this function),
  //   and then adjust that y value so that the next time
  //   text is drawn in that corner, it gets drawn above/below
  //   the previous text (instead of overtop of it).
  // when drawing into the upper or lower LEFT corners,
  //   left-align your text to 'xL'.
  // when drawing into the upper or lower RIGHT corners,
  //   right-align your text to 'xR'.

  // note: try to keep the bounding rectangles on the text small;
  //   the smaller the area that has to be locked (to draw the text),
  //   the faster it will be.  (on some cards, drawing text is
  //   ferociously slow, so even if it works okay on yours, it might
  //   not work on another video card.)
  // note: if you want some text to be on the screen often, and the text
  //   won't be changing every frame, please consider the poor folks
  //   whose video cards hate that; in that case you should probably
  //   draw the text just once, to a texture, and then display the
  //   texture each frame.  This is how the help screen is done; see
  //   pluginshell.cpp for example code.

  RECT r = { 0 };
  LPD3DXFONT pFont = GetFont(DECORATIVE_FONT);
  int h = GetFontHeight(DECORATIVE_FONT);

  // ── Startup loading screen (#291) ──
  //
  // RenderFrame returns before drawing while the first preset is still
  // compiling, so the canvas behind this is black; all that is left to do is
  // say how long it has been. Tenths, at Shane's asking -- the wait is around a
  // second on this machine, so whole seconds would show 0 and then 1 and look
  // stuck, and a machine slow enough to need the number is the one that most
  // needs to see it moving.
  //
  // FIRST in this function and it returns: a preset name, an FPS figure or a
  // battery percentage over an empty screen is noise, and half of them would be
  // reporting on a preset that is not up yet.
  //
  // LOADING_FONT, a slot of its own at 160pt. There is no per-draw glyph scale
  // in this text path -- size comes from the atlas -- so "large" has to be a
  // font that is already large, and the biggest existing slot (SONGTITLE_FONT,
  // 40pt) is a speck on a 2160x3840 panel.
  if (LoadingScreenActive()) {
    SelectFont(LOADING_FONT);
    wchar_t szWait[32];
    FormatTo(szWait, L"%.1f", LoadingScreenSeconds());

    // MEASURED, then placed -- the same two-step every MyTextOut macro uses.
    //
    // DT_VCENTER is not honoured by this text path (glyphs are queued against
    // the rect's top edge), so asking for it put the counter hard against the
    // top of the screen while DT_CENTER placed it correctly across. Measured
    // rather than assumed: it was photographed up there.
    const DWORD fmt = DT_NOPREFIX | DT_SINGLELINE | DT_CENTER;
    SetRect(&r, xL, 0, xR, 4096);
    m_text.DrawTextW(pFont, szWait, -1, &r, fmt | DT_CALCRECT,
                     0xFFFFFFFF, false, 0xFF000000);
    const int tw = r.right - r.left;
    const int th = r.bottom - r.top;
    // The corner cursors at their starting values bound the drawable area,
    // which is why this block runs before anything moves them.
    const int cx = (xL + xR) / 2;
    const int cy = (*upper_left_corner_y + *lower_left_corner_y) / 2;
    SetRect(&r, cx - tw / 2, cy - th / 2, cx + tw / 2, cy + th / 2);
    m_text.DrawTextW(pFont, szWait, -1, &r, fmt,
                     0xFFFFFFFF, false, 0xFF000000);
    return;
  }

  // 1. render text in upper-right corner - EXCEPT USER MESSAGE - it goes last b/c it draws a box under itself
  //                                        and it should be visible over everything else (usually an error msg)
  {
    wchar_t buf[512] = { 0 };

    // a) preset name with lock icon (U+E000 = baked lock glyph in font atlas)
    if (m_bShowPresetInfo && !m_blackmode) {
      SelectFont(DECORATIVE_FONT);
      FormatTo(
        buf,
        L"%s%s ",
        (m_bPresetLockedByUser || m_bPresetLockedByCode) && m_ShowLockSymbol ? L"\uE000 " : L"",
        (m_nLoadingPreset != 0) ? m_pNewState->m_szDesc : m_pState->m_szDesc);

      DWORD alpha = 255;
      DWORD cr = m_fontinfo[DECORATIVE_FONT].R;
      DWORD cg = m_fontinfo[DECORATIVE_FONT].G;
      DWORD cb = m_fontinfo[DECORATIVE_FONT].B;
      DWORD color = (alpha << 24) | (cr << 16) | (cg << 8) | cb;
      MyTextOut_Color(buf, MTO_UPPER_RIGHT, color);
    }

    // b) preset rating
    if (m_bShowRating || GetTime() < m_fShowRatingUntilThisTime) {
      // see also: SetCurrentPresetRating() in milkdrop.cpp
      SelectFont(SIMPLE_FONT);
      wchar_t label[64];
      FormatTo(buf, L" %s: %d ", wasabiApiLangString(IDS_RATING, label), (int)m_pState->m_fRating);
      if (!m_bEnableRating) AppendTo(buf, wasabiApiLangString(IDS_DISABLED, label));
      MyTextOut_Shadow(buf, MTO_UPPER_RIGHT);
    }

    // c) fps display
    if (m_bShowFPS) {
      SelectFont(SIMPLE_FONT);
      wchar_t label[64];
      FormatTo(buf, L"%s: %4.2f ", wasabiApiLangString(IDS_FPS, label), GetFps()); // leave extra space @ end, so italicized fonts don't get clipped
      MyTextOut_Shadow(buf, MTO_UPPER_RIGHT);
      // Independent mirror sims run their own frame rate — show it under the
      // primary's. (Cross-thread float read of the worker's counter; display only.)
      // m_fOrientFps, not OnlyMirrorSurface().sim.fFps: the latter is written only by
      // MirrorSimStepFrame, which is skipped entirely in shadertoy mode, so
      // .milk3 presets showed no mirror line at all (Shane, 2026-08-23).
      // m_fOrientFps counts worker submits on the render thread and is valid in
      // both modes — and is the same number DIAG_MIRRORS reports as orientFps,
      // so the HUD and the diagnostics now agree.
      if (m_bMirrorsActive && m_mirrorQueue && m_fOrientFps > 0.5f) {
        FormatTo(buf, L"%s: %4.2f ", L"mirror", m_fOrientFps);
        MyTextOut_Shadow(buf, MTO_UPPER_RIGHT);
      }
    }

    // c2) headset battery
    //
    // Its own toggle, not part of the FPS block: they answer different
    // questions and Shane asked for the battery unbound and separate. No
    // figure means no line -- the same rule the mixer window follows, where
    // an absent battery is a blank cell and never a zero.
    if (m_bShowBattery) {
      const int pct = MixerBatteryPercent();
      if (pct >= 0) {
        SelectFont(SIMPLE_FONT);
        FormatTo(buf, L"batt: %d%% ", pct);  // trailing space: italics clip
        // Green above half, amber down to a quarter, red at a quarter and
        // below. Muted rather than pure so they stay legible over a bright
        // preset with only the one-pixel shadow behind them.
        const DWORD color = (pct > 50) ? 0xFF3CE07A
                          : (pct > 25) ? 0xFFFFC83C
                                       : 0xFFFF5050;
        MyTextOut_Shadow_Color(buf, MTO_UPPER_RIGHT, color);
      }
    }

    // d) debug information
    if (m_bShowDebugInfo) {
      SelectFont(SIMPLE_FONT);
      DWORD color = GetFontColor(SIMPLE_FONT);

      wchar_t label[64];
      FormatTo(buf, L"  %6.2f %s", (float)(*m_pState->var_pf_monitor), wasabiApiLangString(IDS_PF_MONITOR, label));
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      // GPU: this process's own 3D engine share, and its share of every GPU
      // engine. Blank for the first second or two after the overlay comes up --
      // the counter is a rate and needs two samples. See gpu_usage.h.
      if (m_gpuUsage.Available()) {
        FormatTo(buf, L"  %5.1f%% gpu3d  %5.1f%% gpu",
                 m_gpuUsage.Percent3D(), m_gpuUsage.PercentAll());
      } else {
        FormatTo(buf, L"     -   gpu3d");
      }
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      if (!m_bPresetLockedByUser && !m_bPresetLockedByCode) {
        FormatTo(buf, L"  %6.2f %s", (float)(GetTime() - m_fPresetStartTime), L"time");
        MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      }

      {
        const wchar_t* action = (m_nIdleAction == 3) ? L"profile"
          : (m_nIdleAction == 2) ? L"mirror-all"
          : (m_nIdleAction == 1) ? L"stretch" : L"fullscreen";
        if (!m_bIdleTimerEnabled) {
          FormatTo(buf, L"idle off");
        } else if (m_bIdleActivated) {
          FormatTo(buf, L"idle active  %s%s", action, m_bIdleAutoRestore ? L"  auto" : L"");
        } else {
          // The SAME helper the countdown in App.cpp uses. Reading
          // GetLastInputInfo here independently is how the HUD would come to
          // disagree with the timer about how long is left after a
          // RESET_IDLE_TIMER, and the HUD is the only place that figure shows.
          const DWORD idleMs = IdleElapsedMs();
          const DWORD timeoutMs = (DWORD)m_nIdleTimeoutMinutes * 60u * 1000u;
          const DWORD remainMs = (idleMs >= timeoutMs) ? 0 : (timeoutMs - idleMs);
          unsigned sec = remainMs / 1000u;
          FormatTo(buf, L"idle %u:%02u  %s", sec / 60u, sec % 60u, action);
        }
        MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      }

      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.imm_rel[0] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_bass), L"bass");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.avg_rel[0] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_bass_att), L"bass_att");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.smooth_rel[0] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_bass_smooth), L"bass_smooth");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.imm_rel[1] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_mid), L"mid");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.avg_rel[1] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_mid_att), L"mid_att");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.smooth_rel[1] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_mid_smooth), L"mid_smooth");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.imm_rel[2] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_treb), L"treb");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.avg_rel[2] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_treb_att), L"treb_att");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      FormatTo(buf, L"%s %6.2f %s", ((double)mysound.smooth_rel[2] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_treb_smooth), L"treb_smooth");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      FormatTo(buf, L"q=%.2f hue=%.2f sat=%.2f bri=%.2f", m_fRenderQuality, m_ColShiftHue, m_ColShiftSaturation, m_ColShiftBrightness);
      MyTextOut_Color(buf, MTO_LOWER_RIGHT, color);

      // VRAM budget sample (DXGI local). Always shown in debug; highlighted when exit triggered.
      if (m_vramBudgetBytes > 0 || m_bVramExitTriggered) {
        double budgetMB = (double)m_vramBudgetBytes / (1024.0 * 1024.0);
        double usageMB = (double)m_vramUsageBytes / (1024.0 * 1024.0);
        FormatTo(buf, L"vram avail %.1f%%  usage %.0f / budget %.0f MB%s",
          m_vramAvailablePercent, usageMB, budgetMB,
          m_bVramExitTriggered ? L"  EXIT" : L"");
        MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      }

      if (m_bEnableMouseInteraction) {
        FormatTo(buf, L"%s x=%0.2f y=%0.2f z=%s ", L"mouse", m_mouseX, m_mouseY, m_mouseDown ? L"1" : L"0");
        MyTextOut_Color(buf, MTO_LOWER_RIGHT, color);
      }
    }
    // NOTE: custom timed msg comes at the end!!
  }

  // 1b. render timed preset name display (Simple mode) — right-aligned at top
  if (m_bShowNotifications && m_szPresetNameDisplay[0] && GetTime() < m_fPresetNameShowUntil) {
    SelectFont(DECORATIVE_FONT);
    float fAge = GetTime() - (m_fPresetNameShowUntil - 3.5f);
    float fAlpha = 1.0f;
    if (fAge < 0.3f) fAlpha = fAge / 0.3f;                          // fade in
    else if (fAge > 3.5f - 0.3f) fAlpha = (3.5f - fAge) / 0.3f;    // fade out
    fAlpha = max(0.0f, min(1.0f, fAlpha));
    DWORD alpha = (DWORD)(fAlpha * 255.0f);
    DWORD cr = m_fontinfo[DECORATIVE_FONT].R;
    DWORD cg = m_fontinfo[DECORATIVE_FONT].G;
    DWORD cb = m_fontinfo[DECORATIVE_FONT].B;
    DWORD color = (alpha << 24) | (cr << 16) | (cg << 8) | cb;
    SetRect(&r, xL, h / 2, xR, h / 2 + h * 2);
    m_text.DrawTextW(pFont, m_szPresetNameDisplay, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_RIGHT | DT_END_ELLIPSIS, color, false, 0xFF000000);
  }

  // 2. render text in lower-right corner
  {
    // waitstring tooltip:
    if (m_waitstring.bActive && m_bShowMenuToolTips && m_waitstring.szToolTip[0]) {
      DrawTooltip(m_waitstring.szToolTip, xR, *lower_right_corner_y);
    }
  }

  // 3. render text in lower-left corner
  {
    wchar_t buf2[512] = { 0 };
    wchar_t buf3[512 + 1] = { 0 }; // add two extra spaces to end, so italicized fonts don't get clipped

    // render song title in lower-left corner
    if (m_bShowSongTitle) {
      wchar_t buf4[512] = { 0 };
      SelectFont(DECORATIVE_FONT);
      GetSongTitle(buf4, sizeof(buf4)); // defined in utility.h/cpp

      MyTextOut_Shadow(buf4, MTO_LOWER_LEFT);
    }

    // render song time & len above that:
    if (m_bShowSongTime || m_bShowSongLen) {
      /*if (playbackService) {
          FormatSongTime(playbackService->GetPosition(), buf); // defined in utility.h/cpp
          FormatSongTime(playbackService->GetDuration(), buf2); // defined in utility.h/cpp
          if (m_bShowSongTime && m_bShowSongLen)
          {
              // only show playing position and track length if it is playing (buffer is valid)
              if (buf[0])
                  FormatTo(buf3, L"%s / %s ", buf, buf2);
              else
                  lstrcpynW(buf3, buf2, 512);
          }
          else if (m_bShowSongTime)
              lstrcpynW(buf3, buf, 512);
          else
              lstrcpynW(buf3, buf2, 512);

          SelectFont(DECORATIVE_FONT);
          MyTextOut_Shadow(buf3, MTO_LOWER_LEFT);
      }*/
    }
  }

  // 4. render text in upper-left corner
  {
    wchar_t buf[64000] = { 0 };  // must fit the longest strings (code strings are 32768 chars)
    // AND leave extra space for &->&&, and [,[,& insertion
    char bufA[64000] = { 0 };

    SelectFont(SIMPLE_FONT);

    // stuff for loading presets, menus, etc:

    if (m_waitstring.bActive) {
      // 1. draw the prompt string
      MyTextOut(m_waitstring.szPrompt, MTO_UPPER_LEFT, true);

      // extra instructions:
      bool bIsWarp = m_waitstring.bDisplayAsCode && (m_pCurMenu == &m_menuPreset) && !wcscmp(m_menuPreset.GetCurItem()->m_szName, L"[ edit warp shader ]");
      bool bIsComp = m_waitstring.bDisplayAsCode && (m_pCurMenu == &m_menuPreset) && !wcscmp(m_menuPreset.GetCurItem()->m_szName, L"[ edit composite shader ]");
      if (bIsWarp || bIsComp) {
        wchar_t line[128];
        if (m_bShowShaderHelp) {
          wasabiApiLangString(IDS_PRESS_F9_TO_HIDE_SHADER_QREF, line);
          MyTextOut(line, MTO_UPPER_LEFT, true);
        }
        else {
          wasabiApiLangString(IDS_PRESS_F9_TO_SHOW_SHADER_QREF, line);
          MyTextOut(line, MTO_UPPER_LEFT, true);
        }
        *upper_left_corner_y += h * 2 / 3;

        if (m_bShowShaderHelp) {
          // draw dark box - based on longest line & # lines...
          SetRect(&r, 0, 0, 2048, 2048);
          m_text.DrawTextW(pFont, wasabiApiLangString(IDS_STRING615, line), -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_WORD_ELLIPSIS | DT_CALCRECT, 0xFFFFFFFF, false, 0xFF000000);
          RECT darkbox;
          SetRect(&darkbox, xL, *upper_left_corner_y - 2, xL + r.right - r.left, *upper_left_corner_y + (r.bottom - r.top) * 13 + 2);
          DrawDarkTranslucentBox(&darkbox);

          wasabiApiLangString(IDS_STRING616, line);
          MyTextOut(line, MTO_UPPER_LEFT, false);
          wasabiApiLangString(IDS_STRING617, line);
          MyTextOut(line, MTO_UPPER_LEFT, false);
          wasabiApiLangString(IDS_STRING618, line);
          MyTextOut(line, MTO_UPPER_LEFT, false);
          wasabiApiLangString(IDS_STRING619, line);
          MyTextOut(line, MTO_UPPER_LEFT, false);
          wasabiApiLangString(IDS_STRING620, line);
          MyTextOut(line, MTO_UPPER_LEFT, false);
          wasabiApiLangString(IDS_STRING621, line);
          MyTextOut(line, MTO_UPPER_LEFT, false);
          if (bIsWarp) {
            wasabiApiLangString(IDS_STRING622, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING623, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING624, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING625, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING626, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING627, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING628, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
          }
          else if (bIsComp) {
            wasabiApiLangString(IDS_STRING629, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING630, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING631, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING632, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING633, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING634, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
            wasabiApiLangString(IDS_STRING635, line);
            MyTextOut(line, MTO_UPPER_LEFT, false);
          }
          *upper_left_corner_y += h * 2 / 3;
        }
      }
      else if (m_UI_mode == UI_SAVEAS && (m_bWarpShaderLock || m_bCompShaderLock)) {
        wchar_t lockMsg[256] = { 0 };
        int shader_msg_id = IDS_COMPOSITE_SHADER_LOCKED;
        if (m_bWarpShaderLock && m_bCompShaderLock)
          shader_msg_id = IDS_WARP_AND_COMPOSITE_SHADERS_LOCKED;
        else if (m_bWarpShaderLock && !m_bCompShaderLock)
          shader_msg_id = IDS_WARP_SHADER_LOCKED;
        else
          shader_msg_id = IDS_COMPOSITE_SHADER_LOCKED;

        wasabiApiLangString(shader_msg_id, lockMsg);
        MyTextOut_BGCOLOR(lockMsg, MTO_UPPER_LEFT, true, 0xFF000000);
        *upper_left_corner_y += h * 2 / 3;
      }
      else
        *upper_left_corner_y += h * 2 / 3;


      // 2. reformat the waitstring text for display
      int bBrackets = m_waitstring.nSelAnchorPos != -1 && m_waitstring.nSelAnchorPos != m_waitstring.nCursorPos;
      int bCursorBlink = (!bBrackets &&
        ((int)(GetTime() * 270.0f) % 100 > 50)
        //((GetFrame() % 3) >= 2)
        );

      CopyTo(buf, m_waitstring.szText);
      CopyToA(bufA, m_waitstring.szCode);

      int temp_cursor_pos = m_waitstring.nCursorPos;
      int temp_anchor_pos = m_waitstring.nSelAnchorPos;

      if (bBrackets) {
        if (m_waitstring.bDisplayAsCode) {
          // insert [] around the selection
          int start = (temp_cursor_pos < temp_anchor_pos) ? temp_cursor_pos : temp_anchor_pos;
          int end = (temp_cursor_pos > temp_anchor_pos) ? temp_cursor_pos - 1 : temp_anchor_pos - 1;
          int len = lstrlenA(bufA);
          int i;

          for (i = len; i > end; i--)
            bufA[i + 1] = bufA[i];
          bufA[end + 1] = ']';
          len++;

          for (i = len; i >= start; i--)
            bufA[i + 1] = bufA[i];
          bufA[start] = '[';
          len++;
        }
        else {
          // insert [] around the selection
          int start = (temp_cursor_pos < temp_anchor_pos) ? temp_cursor_pos : temp_anchor_pos;
          int end = (temp_cursor_pos > temp_anchor_pos) ? temp_cursor_pos - 1 : temp_anchor_pos - 1;
          int len = lstrlenW(buf);
          int i;

          for (i = len; i > end; i--)
            buf[i + 1] = buf[i];
          buf[end + 1] = L']';
          len++;

          for (i = len; i >= start; i--)
            buf[i + 1] = buf[i];
          buf[start] = L'[';
          len++;
        }
      }
      else {
        // underline the current cursor position by rapidly toggling the character with an underscore
        if (m_waitstring.bDisplayAsCode) {
          if (bCursorBlink) {
            if (bufA[temp_cursor_pos] == 0) {
              bufA[temp_cursor_pos] = '_';
              bufA[temp_cursor_pos + 1] = 0;
            }
            else if (bufA[temp_cursor_pos] == LINEFEED_CONTROL_CHAR) {
              for (int i = (int)strlen(bufA); i >= temp_cursor_pos; i--)
                bufA[i + 1] = bufA[i];
              bufA[temp_cursor_pos] = '_';
            }
            else if (bufA[temp_cursor_pos] == '_')
              bufA[temp_cursor_pos] = ' ';
            else // it's a space or symbol or alphanumeric.
              bufA[temp_cursor_pos] = '_';
          }
          else {
            if (bufA[temp_cursor_pos] == 0) {
              bufA[temp_cursor_pos] = ' ';
              bufA[temp_cursor_pos + 1] = 0;
            }
            else if (bufA[temp_cursor_pos] == LINEFEED_CONTROL_CHAR) {
              for (int i = (int)strlen(bufA); i >= temp_cursor_pos; i--)
                bufA[i + 1] = bufA[i];
              bufA[temp_cursor_pos] = ' ';
            }
            //else if (buf[temp_cursor_pos] == '_')
              // do nothing
            //else // it's a space or symbol or alphanumeric.
              // do nothing
          }
        }
        else {
          if (bCursorBlink) {
            if (buf[temp_cursor_pos] == 0) {
              buf[temp_cursor_pos] = L'_';
              buf[temp_cursor_pos + 1] = 0;
            }
            else if (buf[temp_cursor_pos] == LINEFEED_CONTROL_CHAR) {
              for (int i = (int)wcslen(buf); i >= temp_cursor_pos; i--)
                buf[i + 1] = buf[i];
              buf[temp_cursor_pos] = L'_';
            }
            else if (buf[temp_cursor_pos] == L'_')
              buf[temp_cursor_pos] = L' ';
            else // it's a space or symbol or alphanumeric.
              buf[temp_cursor_pos] = L'_';
          }
          else {
            if (buf[temp_cursor_pos] == 0) {
              buf[temp_cursor_pos] = L' ';
              buf[temp_cursor_pos + 1] = 0;
            }
            else if (buf[temp_cursor_pos] == LINEFEED_CONTROL_CHAR) {
              for (int i = (int)wcslen(buf); i >= temp_cursor_pos; i--)
                buf[i + 1] = buf[i];
              buf[temp_cursor_pos] = L' ';
            }
            //else if (buf[temp_cursor_pos] == '_')
              // do nothing
            //else // it's a space or symbol or alphanumeric.
              // do nothing
          }
        }
      }

      RECT rect = { 0 };
      SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);
      rect.top += PLAYLIST_INNER_MARGIN;
      rect.left += PLAYLIST_INNER_MARGIN;
      rect.right -= PLAYLIST_INNER_MARGIN;
      rect.bottom -= PLAYLIST_INNER_MARGIN;

      // then draw the edit string
      if (m_waitstring.bDisplayAsCode) {
        char buf2[8192] = { 0 };
        int top_of_page_pos = 0;

        // compute top_of_page_pos so that the line the cursor is on will show.
                // also compute dims of the black rectangle while we're at it.
        {
          int start = 0;
          int pos = 0;
          int ypixels = 0;
          int page = 1;
          int exit_on_next_page = 0;

          RECT box = rect;
          box.right = box.left;
          box.bottom = box.top;

          while (bufA[pos] != 0)  // for each line of text... (note that it might wrap)
          {
            start = pos;
            while (bufA[pos] != LINEFEED_CONTROL_CHAR && bufA[pos] != 0)
              pos++;

            char ch = bufA[pos];
            bufA[pos] = 0;
            sprintf(buf2, "   %sX", &bufA[start]); // put a final 'X' instead of ' ' b/c CALCRECT returns w==0 if string is entirely whitespace!
            RECT r2 = rect;
            r2.bottom = 4096;
            m_text.DrawText(GetFont(SIMPLE_FONT), buf2, -1, &r2, DT_CALCRECT /*| DT_WORDBREAK*/, 0xFFFFFFFF, false);
            int lineH = r2.bottom - r2.top;
            ypixels += lineH;
            bufA[pos] = ch;

            if (start > m_waitstring.nCursorPos) // make sure 'box' gets updated for each line on this page
              exit_on_next_page = 1;

            if (ypixels > rect.bottom - rect.top) // this line belongs on the next page
            {
              if (exit_on_next_page) {
                bufA[start] = 0; // so text stops where the box stops, when we draw the text
                break;
              }

              ypixels = lineH;
              top_of_page_pos = start;
              page++;

              box = rect;
              box.right = box.left;
              box.bottom = box.top;
            }
            box.bottom += lineH;
            box.right = max(box.right, box.left + r2.right - r2.left);

            if (bufA[pos] == 0)
              break;
            pos++;
          }

          // use r2 to draw a dark box:
          box.top -= PLAYLIST_INNER_MARGIN;
          box.left -= PLAYLIST_INNER_MARGIN;
          box.right += PLAYLIST_INNER_MARGIN;
          box.bottom += PLAYLIST_INNER_MARGIN;
          DrawDarkTranslucentBox(&box);
          *upper_left_corner_y += box.bottom - box.top + PLAYLIST_INNER_MARGIN * 3;
          FormatResTo(m_waitstring.szToolTip, IDS_PAGE_X, page);
        }

        // display multiline (replace all character 13's with a CR)
        {
          int start = top_of_page_pos;
          int pos = top_of_page_pos;

          while (bufA[pos] != 0) {
            while (bufA[pos] != LINEFEED_CONTROL_CHAR && bufA[pos] != 0)
              pos++;

            char ch = bufA[pos];
            bufA[pos] = 0;
            sprintf(buf2, "   %s ", &bufA[start]);
            DWORD color = MENU_COLOR;
            if (m_waitstring.nCursorPos >= start && m_waitstring.nCursorPos <= pos)
              color = MENU_HILITE_COLOR;
            rect.top += m_text.DrawText(GetFont(SIMPLE_FONT), buf2, -1, &rect, 0/*DT_WORDBREAK*/, color, false);
            bufA[pos] = ch;

            if (rect.top > rect.bottom)
              break;

            if (bufA[pos] != 0) pos++;
            start = pos;
          }
        }
        // note: *upper_left_corner_y is updated above, when the dark box is drawn.
      }
      else {
        wchar_t buf2[8192] = { 0 };

        // display on one line
        RECT box = rect;
        box.bottom = 4096;
        FormatTo(buf2, L"    %sX", buf);  // put a final 'X' instead of ' ' b/c CALCRECT returns w==0 if string is entirely whitespace!
        m_text.DrawTextW(GetFont(SIMPLE_FONT), buf2, -1, &box, DT_CALCRECT, MENU_COLOR, false);

        // use r2 to draw a dark box:
        box.top -= PLAYLIST_INNER_MARGIN;
        box.left -= PLAYLIST_INNER_MARGIN;
        box.right += PLAYLIST_INNER_MARGIN;
        box.bottom += PLAYLIST_INNER_MARGIN;
        DrawDarkTranslucentBox(&box);
        *upper_left_corner_y += box.bottom - box.top + PLAYLIST_INNER_MARGIN * 3;

        FormatTo(buf2, L"    %s ", buf);
        m_text.DrawTextW(GetFont(SIMPLE_FONT), buf2, -1, &rect, 0, MENU_COLOR, false);
      }
    }
    else if (m_UI_mode == UI_MENU) {
      if (m_pCurMenu) {
        RECT rect;
        SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);

        // First pass: calculate bounding box for dark background
        RECT box;
        m_pCurMenu->DrawMenu(rect, xR, *lower_right_corner_y, 1, &box);
        box.top -= PLAYLIST_INNER_MARGIN;
        box.left -= PLAYLIST_INNER_MARGIN;
        box.right += PLAYLIST_INNER_MARGIN;
        box.bottom += PLAYLIST_INNER_MARGIN;
        DrawDarkTranslucentBox(&box);
        *upper_left_corner_y += box.bottom - box.top + PLAYLIST_INNER_MARGIN * 3;

        // Second pass: draw menu items
        m_pCurMenu->DrawMenu(rect, xR, *lower_right_corner_y);
      }
    }
    else if (m_UI_mode == UI_UPGRADE_PIXEL_SHADER) {
      RECT rect = { 0 };
      SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);
      wchar_t msg[128];

      if (m_pState->m_nWarpPSVersion >= m_nMaxPSVersion &&
        m_pState->m_nCompPSVersion >= m_nMaxPSVersion) {
        assert(m_pState->m_nMaxPSVersion == m_nMaxPSVersion);
        wchar_t psVerMsg[1024] = { 0 };
        FormatResTo(psVerMsg, IDS_PRESET_USES_HIGHEST_PIXEL_SHADER_VERSION, m_nMaxPSVersion);
        rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), psVerMsg, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
        rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESS_ESC_TO_RETURN, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      }
      else {
        if (m_pState->m_nMinPSVersion != m_pState->m_nMaxPSVersion) {
          switch (m_pState->m_nMinPSVersion) {
          case MD2_PS_NONE:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_HAS_MIXED_VERSIONS_OF_SHADERS, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_SHADERS_TO_USE_PS2, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_2_0:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_HAS_MIXED_VERSIONS_OF_SHADERS, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_SHADERS_TO_USE_PS2X, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_2_X:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_HAS_MIXED_VERSIONS_OF_SHADERS, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_SHADERS_TO_USE_PS3, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_3_0:
            assert(false);
            break;
          default:
            assert(0);
            break;
          }
        }
        else {
          switch (m_pState->m_nMinPSVersion) {
          case MD2_PS_NONE:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_DOES_NOT_USE_PIXEL_SHADERS, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_TO_USE_PS2, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_OLD_GPU_MIGHT_NOT_WORK_WITH_PRESET, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_2_0:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_CURRENTLY_USES_PS2, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_TO_USE_PS2X, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_OLD_GPU_MIGHT_NOT_WORK_WITH_PRESET, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_2_X:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_CURRENTLY_USES_PS2X, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_TO_USE_PS3, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_OLD_GPU_MIGHT_NOT_WORK_WITH_PRESET, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_3_0:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_CURRENTLY_USES_PS3, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_TO_USE_PS4, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_OLD_GPU_MIGHT_NOT_WORK_WITH_PRESET, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          default:
            assert(0);
            break;
          }
        }
      }
      *upper_left_corner_y = rect.top;
    }
    else if (m_UI_mode == UI_LOAD_DEL) {
      RECT rect;
      SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);
      wchar_t msg[64];
      rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_ARE_YOU_SURE_YOU_WANT_TO_DELETE_PRESET, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      FormatResTo(buf, IDS_PRESET_TO_DELETE, PresetNameAt(m_nPresetListCurPos).c_str());
      rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), buf, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      *upper_left_corner_y = rect.top;
    }
    else if (m_UI_mode == UI_SAVE_OVERWRITE) {
      RECT rect;
      SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);
      wchar_t msg[64];
      rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_FILE_ALREADY_EXISTS_OVERWRITE_IT, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      FormatResTo(buf, IDS_FILE_IN_QUESTION_X_MILK, m_waitstring.szText);
      rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), buf, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      if (m_bWarpShaderLock)
        rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_DO_NOT_FORGET_WARP_SHADER_WAS_LOCKED, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, 0xFFFFFFFF, true, 0xFFCC0000);
      if (m_bCompShaderLock)
        rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_DO_NOT_FORGET_COMPOSITE_SHADER_WAS_LOCKED, msg), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, 0xFFFFFFFF, true, 0xFFCC0000);
      *upper_left_corner_y = rect.top;
    }
    else if (m_UI_mode == UI_MASHUP) {
      if (m_nPresets - m_nDirs == 0) {
        if (wcscmp(m_pState->m_szDesc, INVALID_PRESET_DESC) == 0) {
          wchar_t errMsg[1024];
          FormatResTo(errMsg, IDS_ERROR_NO_PRESET_FILE_FOUND_IN_X_MILK, m_szPresetDir);
          AddError(errMsg, 6.0f, ERR_MISC, true);
        }
        m_UI_mode = UI_REGULAR;
      }
      else {
        UpdatePresetList(true); // make sure list is completely ready

        // quick checks
        for (int mash = 0; mash < MASH_SLOTS; mash++) {
          // check validity
          if (m_nMashPreset[mash] < m_nDirs)
            m_nMashPreset[mash] = m_nDirs;
          if (m_nMashPreset[mash] >= m_nPresets)
            m_nMashPreset[mash] = m_nPresets - 1;

          // apply changes, if it's time
          if (m_nLastMashChangeFrame[mash] + MASH_APPLY_DELAY_FRAMES + 1 == GetFrame()) {
            // import just a fragment of a preset!!
            DWORD ApplyFlags = 0;
            switch (mash) {
            case 0: ApplyFlags = STATE_GENERAL; break;
            case 1: ApplyFlags = STATE_MOTION; break;
            case 2: ApplyFlags = STATE_WAVE; break;
            case 3: ApplyFlags = STATE_WARP; break;
            case 4: ApplyFlags = STATE_COMP; break;
            }

            wchar_t szFile[MAX_PATH];
            BuildPresetPath(m_nMashPreset[mash], szFile, MAX_PATH);

            m_pState->Import(szFile, GetTime(), m_pState, ApplyFlags);

            if (ApplyFlags & STATE_WARP)
              SafeRelease(m_shaders.warp.ptr);
            if (ApplyFlags & STATE_COMP)
              SafeRelease(m_shaders.comp.ptr);
            LoadShaders(&m_shaders, m_pState, false, false);
            CreateDX12PresetPSOs();

            SetMenusForPresetVersion(m_pState->m_nWarpPSVersion, m_pState->m_nCompPSVersion);
          }
        }

        wchar_t line[128];
        wasabiApiLangString(IDS_PRESET_MASH_UP_TEXT1, line);
        MyTextOut(line, MTO_UPPER_LEFT, true);
        wasabiApiLangString(IDS_PRESET_MASH_UP_TEXT2, line);
        MyTextOut(line, MTO_UPPER_LEFT, true);
        wasabiApiLangString(IDS_PRESET_MASH_UP_TEXT3, line);
        MyTextOut(line, MTO_UPPER_LEFT, true);
        wasabiApiLangString(IDS_PRESET_MASH_UP_TEXT4, line);
        MyTextOut(line, MTO_UPPER_LEFT, true);
        *upper_left_corner_y += PLAYLIST_INNER_MARGIN;

        RECT rect;
        SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);
        rect.top += PLAYLIST_INNER_MARGIN;
        rect.left += PLAYLIST_INNER_MARGIN;
        rect.right -= PLAYLIST_INNER_MARGIN;
        rect.bottom -= PLAYLIST_INNER_MARGIN;

        int lines_available = (rect.bottom - rect.top - PLAYLIST_INNER_MARGIN * 2) / GetFontHeight(SIMPLE_FONT);
        lines_available -= MASH_SLOTS;

        if (lines_available < 10) {
          // force it
          rect.bottom = rect.top + GetFontHeight(SIMPLE_FONT) * 10 + 1;
          lines_available = 10;
        }
        if (lines_available > 16)
          lines_available = 16;

        if (m_bUserPagedDown) {
          m_nMashPreset[m_nMashSlot] += lines_available;
          if (m_nMashPreset[m_nMashSlot] >= m_nPresets)
            m_nMashPreset[m_nMashSlot] = m_nPresets - 1;
          m_bUserPagedDown = false;
        }
        if (m_bUserPagedUp) {
          m_nMashPreset[m_nMashSlot] -= lines_available;
          if (m_nMashPreset[m_nMashSlot] < m_nDirs)
            m_nMashPreset[m_nMashSlot] = m_nDirs;
          m_bUserPagedUp = false;
        }

        int first_line = m_nMashPreset[m_nMashSlot] - (m_nMashPreset[m_nMashSlot] % lines_available);
        int last_line = first_line + lines_available;
        wchar_t str[512], str2[512], label[64];

        if (last_line > m_nPresets)
          last_line = m_nPresets;

        // tooltip:
        if (m_bShowMenuToolTips) {
          wchar_t pageMsg[256];
          FormatResTo(pageMsg, IDS_PAGE_X_OF_X, m_nMashPreset[m_nMashSlot] / lines_available + 1, (m_nPresets + lines_available - 1) / lines_available);
          DrawTooltip(pageMsg, xR, *lower_right_corner_y);
        }

        RECT orig_rect = rect;

        RECT box;
        box.top = rect.top;
        box.left = rect.left;
        box.right = rect.left;
        box.bottom = rect.top;

        int mashNames[MASH_SLOTS] = { IDS_MASHUP_GENERAL_POSTPROC,
                        IDS_MASHUP_MOTION_EQUATIONS,
                                              IDS_MASHUP_WAVEFORMS_SHAPES,
                                              IDS_MASHUP_WARP_SHADER,
                        IDS_MASHUP_COMP_SHADER,
        };


        for (int pass = 0; pass < 2; pass++) {
          box = orig_rect;
          int w = 0;
          int stackH = 0;   // running pixel height of the slot list; not the font height 'h'

          int start_y = orig_rect.top;
          for (int mash = 0; mash < MASH_SLOTS; mash++) {
            int idx = m_nMashPreset[mash];

            wchar_t slotLine[1024];
            wchar_t name[64];
            // SPOUT
                        // FormatTo(slotLine, L"%s%s", wasabiApiLangString(mashNames[mash], name), PresetNameAt(idx));
            FormatTo(slotLine, L"%s%s", wasabiApiLangString(mashNames[mash], name), PresetNameAt(idx).c_str());
            RECT r2 = orig_rect;
            r2.top += stackH;
            stackH += m_text.DrawTextW(GetFont(SIMPLE_FONT), slotLine, -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | (pass == 0 ? DT_CALCRECT : 0), (mash == m_nMashSlot) ? PLAYLIST_COLOR_HILITE_TRACK : PLAYLIST_COLOR_NORMAL, false);
            w = max(w, r2.right - r2.left);
          }
          if (pass == 0) {
            box.right = box.left + w;
            box.bottom = box.top + stackH;
            DrawDarkTranslucentBox(&box);
          }
          else
            orig_rect.top += stackH;
        }

        orig_rect.top += GetFontHeight(SIMPLE_FONT) + PLAYLIST_INNER_MARGIN;

        box = orig_rect;
        box.right = box.left;
        box.bottom = box.top;

        // draw a directory listing box right after...
        for (int pass = 0; pass < 2; pass++) {
          //if (pass==1)
          //    GetFont(SIMPLE_FONT)->Begin();

          rect = orig_rect;
          for (int i = first_line; i < last_line && PresetNameAt(i).c_str(); i++) {
            // remove the extension before displaying the filename.  also pad w/spaces.
            //lstrcpy(str, m_pPresetAddr[i]);
            bool bIsDir = (PresetNameAt(i).c_str()[0] == '*');
            bool bIsRunning = false;
            bool bIsSelected = (i == m_nMashPreset[m_nMashSlot]);

            if (bIsDir) {
              // directory
              if (wcscmp(PresetNameAt(i).c_str() + 1, L"..") == 0)
                FormatTo(str2, L" [ %s ] (%s) ", PresetNameAt(i).c_str() + 1, wasabiApiLangString(IDS_PARENT_DIRECTORY, label));
              else
                FormatTo(str2, L" [ %s ] ", PresetNameAt(i).c_str() + 1);
            }
            else {
              // preset file — show relative name for readability
              const std::wstring name_i = PresetNameAt(i);
              const wchar_t* displayName = name_i.c_str();
              if (displayName[0] && displayName[1] == L':') {
                // Absolute path: strip preset dir prefix, or find "\presets\" portion
                int dirLen = lstrlenW(m_szPresetDir);
                if (dirLen > 0 && _wcsnicmp(displayName, m_szPresetDir, dirLen) == 0)
                  displayName += dirLen;
                else { const wchar_t* p = wcsstr(displayName, L"\\presets\\"); if (p) displayName = p + 1; }
              }
              CopyTo(str, displayName);
              RemoveExtension(str);
              FormatTo(str2, L" %s ", str);

              if (wcscmp(PresetNameAt(m_nMashPreset[m_nMashSlot]).c_str(), str) == 0)
                bIsRunning = true;
            }

            if (bIsRunning && m_bPresetLockedByUser)
              AppendTo(str2, wasabiApiLangString(IDS_LOCKED, label));

            DWORD color = bIsDir ? DIR_COLOR : PLAYLIST_COLOR_NORMAL;
            if (bIsRunning)
              color = bIsSelected ? PLAYLIST_COLOR_BOTH : PLAYLIST_COLOR_PLAYING_TRACK;
            else if (bIsSelected)
              color = PLAYLIST_COLOR_HILITE_TRACK;

            RECT r2 = rect;
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), str2, -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | (pass == 0 ? DT_CALCRECT : 0), color, false);

            if (pass == 0)  // calculating dark box
            {
              box.right = max(box.right, box.left + r2.right - r2.left);
              box.bottom += r2.bottom - r2.top;
            }
          }

          //if (pass==1)
          //    GetFont(SIMPLE_FONT)->End();

          if (pass == 0)  // calculating dark box
          {
            box.top -= PLAYLIST_INNER_MARGIN;
            box.left -= PLAYLIST_INNER_MARGIN;
            box.right += PLAYLIST_INNER_MARGIN;
            box.bottom += PLAYLIST_INNER_MARGIN;
            DrawDarkTranslucentBox(&box);
            *upper_left_corner_y = box.bottom + PLAYLIST_INNER_MARGIN;
          }
          else
            orig_rect.top += box.bottom - box.top;
        }

        orig_rect.top += PLAYLIST_INNER_MARGIN;

      }
    }
    else if (m_UI_mode == UI_LOAD) {
      if (m_nPresets - m_nDirs == 0) {
        // No preset files (only directories) — post dialog to UI thread
        m_UI_mode = UI_REGULAR;
        HWND hw = GetPluginWindow();
        if (hw) PostMessage(hw, WM_MW_NO_PRESETS_PROMPT, 0, 0);
      }
      else {
        wchar_t line[128];
        wasabiApiLangString(IDS_LOAD_WHICH_PRESET_PLUS_COMMANDS, line);
        MyTextOut(line, MTO_UPPER_LEFT, true);

        wchar_t dirLine[MAX_PATH + 64];
        FormatResTo(dirLine, IDS_DIRECTORY_OF_X, m_szPresetDir);
        MyTextOut(dirLine, MTO_UPPER_LEFT, true);

        *upper_left_corner_y += h / 2;

        RECT rect;
        SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);
        rect.top += PLAYLIST_INNER_MARGIN;
        rect.left += PLAYLIST_INNER_MARGIN;
        rect.right -= PLAYLIST_INNER_MARGIN;
        rect.bottom -= PLAYLIST_INNER_MARGIN;

        int lines_available = (rect.bottom - rect.top - PLAYLIST_INNER_MARGIN * 2) / GetFontHeight(SIMPLE_FONT);

        if (lines_available < 1) {
          // force it
          rect.bottom = rect.top + GetFontHeight(SIMPLE_FONT) + 1;
          lines_available = 1;
        }
        if (lines_available > MAX_PRESETS_PER_PAGE)
          lines_available = MAX_PRESETS_PER_PAGE;

        if (m_bUserPagedDown) {
          m_nPresetListCurPos += lines_available;
          if (m_nPresetListCurPos >= m_nPresets)
            m_nPresetListCurPos = m_nPresets - 1;

          // remember this preset's name so the next time they hit 'L' it jumps straight to it
          //lstrcpy(m_szLastPresetSelected, PresetNameAt(m_nPresetListCurPos).c_str());

          m_bUserPagedDown = false;
        }

        if (m_bUserPagedUp) {
          m_nPresetListCurPos -= lines_available;
          if (m_nPresetListCurPos < 0)
            m_nPresetListCurPos = 0;

          // remember this preset's name so the next time they hit 'L' it jumps straight to it
          //lstrcpy(m_szLastPresetSelected, PresetNameAt(m_nPresetListCurPos).c_str());

          m_bUserPagedUp = false;
        }

        int first_line = m_nPresetListCurPos - (m_nPresetListCurPos % lines_available);
        int last_line = first_line + lines_available;
        wchar_t str[512], str2[512], label[64];

        if (last_line > m_nPresets)
          last_line = m_nPresets;

        // tooltip:
        if (m_bShowMenuToolTips) {
          wchar_t pageMsg[256];
          FormatResTo(pageMsg, IDS_PAGE_X_OF_X, m_nPresetListCurPos / lines_available + 1, (m_nPresets + lines_available - 1) / lines_available);
          DrawTooltip(pageMsg, xR, *lower_right_corner_y);
        }

        RECT orig_rect = rect;

        RECT box;
        box.top = rect.top;
        box.left = rect.left;
        box.right = rect.left;
        box.bottom = rect.top;

        for (int pass = 0; pass < 2; pass++) {
          //if (pass==1)
          //    GetFont(SIMPLE_FONT)->Begin();

          rect = orig_rect;
          for (int i = first_line; i < last_line && PresetNameAt(i).c_str(); i++) {
            // remove the extension before displaying the filename.  also pad w/spaces.
            //lstrcpy(str, m_pPresetAddr[i]);
            bool bIsDir = (PresetNameAt(i).c_str()[0] == '*');
            bool bIsRunning = (i == m_nCurrentPreset);//false;
            bool bIsSelected = (i == m_nPresetListCurPos);

            if (bIsDir) {
              // directory
              if (wcscmp(PresetNameAt(i).c_str() + 1, L"..") == 0)
                FormatTo(str2, L" [ %s ] (%s) ", PresetNameAt(i).c_str() + 1, wasabiApiLangString(IDS_PARENT_DIRECTORY, label));
              else
                FormatTo(str2, L" [ %s ] ", PresetNameAt(i).c_str() + 1);
            }
            else {
              // preset file — show relative name for readability
              const std::wstring name_i = PresetNameAt(i);
              const wchar_t* displayName = name_i.c_str();
              if (displayName[0] && displayName[1] == L':') {
                // Absolute path: strip preset dir prefix, or find "\presets\" portion
                int dirLen = lstrlenW(m_szPresetDir);
                if (dirLen > 0 && _wcsnicmp(displayName, m_szPresetDir, dirLen) == 0)
                  displayName += dirLen;
                else { const wchar_t* p = wcsstr(displayName, L"\\presets\\"); if (p) displayName = p + 1; }
              }
              CopyTo(str, displayName);
              RemoveExtension(str);
              FormatTo(str2, L" %s ", str);

              //if (lstrcmp(m_pState->m_szDesc, str)==0)
            //    bIsRunning = true;
            }

            if (bIsRunning && m_bPresetLockedByUser)
              AppendTo(str2, wasabiApiLangString(IDS_LOCKED, label));

            DWORD color = bIsDir ? DIR_COLOR : PLAYLIST_COLOR_NORMAL;
            if (bIsRunning)
              color = bIsSelected ? PLAYLIST_COLOR_BOTH : PLAYLIST_COLOR_PLAYING_TRACK;
            else if (bIsSelected)
              color = PLAYLIST_COLOR_HILITE_TRACK;

            RECT r2 = rect;
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), str2, -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | (pass == 0 ? DT_CALCRECT : 0), color, false);

            if (pass == 0)  // calculating dark box
            {
              box.right = max(box.right, box.left + r2.right - r2.left);
              box.bottom += r2.bottom - r2.top;
            }
          }

          //if (pass==1)
          //    GetFont(SIMPLE_FONT)->End();

          if (pass == 0)  // calculating dark box
          {
            box.top -= PLAYLIST_INNER_MARGIN;
            box.left -= PLAYLIST_INNER_MARGIN;
            box.right += PLAYLIST_INNER_MARGIN;
            box.bottom += PLAYLIST_INNER_MARGIN;
            DrawDarkTranslucentBox(&box);
            *upper_left_corner_y = box.bottom + PLAYLIST_INNER_MARGIN;
          }
        }
      }
    }
    else if (m_UI_mode == UI_SETTINGS) {
      // Settings screen header
      // ESC, not F2. F2 has never opened or closed this screen -- the text was
      // written when the overlay had no way in at all, so nothing ever
      // contradicted it (#163). Escape genuinely closes it; the way in is now
      // the "Settings Overlay" action, which ships unbound like every other
      // new action, so naming a key for it here would be wrong again.
      MyTextOut(L"MDROPDX12 SETTINGS  (ESC to close, UP/DOWN to navigate, LEFT/RIGHT or ENTER to change)",
                MTO_UPPER_LEFT, true);

      wchar_t iniPath[MAX_PATH + 64];
      FormatTo(iniPath, L"Config: %s", GetConfigIniFile());
      MyTextOut(iniPath, MTO_UPPER_LEFT, true);

      if (GetFileAttributesW(m_szPresetDir) == INVALID_FILE_ATTRIBUTES)
        MyTextOut(L"WARNING: Preset directory not found! Please set a valid path.", MTO_UPPER_LEFT, true);

      *upper_left_corner_y += h / 2;

      RECT rect;
      SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);
      rect.top += PLAYLIST_INNER_MARGIN;
      rect.left += PLAYLIST_INNER_MARGIN;
      rect.right -= PLAYLIST_INNER_MARGIN;
      rect.bottom -= PLAYLIST_INNER_MARGIN;

      RECT orig_rect = rect;

      // Columns are placed by MEASUREMENT, not by space padding.
      //
      // Each row used to be one string, " %s%-24s %-40s  [%s]". Space padding
      // aligns only in a monospace font and the HUD font is proportional, so
      // the value and hint columns landed wherever the preceding text happened
      // to stop -- visibly ragged, with the hints in a zigzag down the screen.
      // Nobody had ever seen it: nothing could open this screen until #163.
      //
      // So each row is drawn as four runs at fixed x, and the widths come from
      // DT_CALCRECT over every row, which is the same measurement the drawing
      // itself uses.
      // const_cast because CTextManager::DrawTextW takes wchar_t* -- it does
      // not write to the string, and every other caller here happens to hold a
      // non-const buffer, so the signature was never noticed.
      auto measure = [&](const wchar_t* s) -> SIZE {
        RECT rm = orig_rect;
        m_text.DrawTextW(GetFont(SIMPLE_FONT), const_cast<wchar_t*>(s), -1, &rm,
                         DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX,
                         PLAYLIST_COLOR_NORMAL, false);
        SIZE sz = { rm.right - rm.left, rm.bottom - rm.top };
        return sz;
      };

      // The marker sits in a column of its own rather than being prepended to
      // the name. Prepending shifts the name of whichever row is selected, so
      // the highlight appears to nudge the text sideways as it moves.
      const int wMark = measure(L"> ").cx;
      const int gap   = measure(L"  ").cx;
      const int rowH  = measure(L"Ag").cy;

      int wName = 0, wValue = 0, wHint = 0;
      for (int i = 0; i < SET_COUNT; i++) {
        wchar_t v[MAX_PATH];
        GetSettingValueString(g_settingsDesc[i].id, v, MAX_PATH);
        wName  = max(wName,  measure(g_settingsDesc[i].name).cx);
        wValue = max(wValue, measure(v).cx);
        if (g_settingsDesc[i].type != ST_READONLY)
          wHint = max(wHint, measure(GetSettingHint(g_settingsDesc[i].id)).cx);
      }

      // The preset directory is a full path and is routinely wider than
      // everything else put together. Letting it set the column would push the
      // hints off the frame, so the value column gives way first and its cell
      // ellipsises -- the hint is a fixed short phrase and the name identifies
      // the row, so the path is the one that can afford to be cut.
      const int avail = orig_rect.right - orig_rect.left;
      wValue = min(wValue, max(gap, avail - wMark - wName - wHint - 3 * gap));

      const int xMark  = orig_rect.left;
      const int xName  = xMark + wMark;
      const int xValue = xName + wName + gap;
      const int xHint  = xValue + wValue + gap;

      RECT box;
      box.top    = rect.top    - PLAYLIST_INNER_MARGIN;
      box.left   = rect.left   - PLAYLIST_INNER_MARGIN;
      box.right  = min(orig_rect.right, xHint + wHint) + PLAYLIST_INNER_MARGIN;
      box.bottom = rect.top + rowH * SET_COUNT + PLAYLIST_INNER_MARGIN;
      DrawDarkTranslucentBox(&box);
      *upper_left_corner_y = box.bottom + PLAYLIST_INNER_MARGIN;

      for (int i = 0; i < SET_COUNT; i++) {
        const bool bSelected = (i == m_nSettingsCurSel);
        const bool bReadOnly = (g_settingsDesc[i].type == ST_READONLY);

        wchar_t valBuf[MAX_PATH];
        GetSettingValueString(g_settingsDesc[i].id, valBuf, MAX_PATH);

        DWORD color = PLAYLIST_COLOR_NORMAL;
        if (bSelected)  color = PLAYLIST_COLOR_HILITE_TRACK;
        if (bReadOnly)  color = bSelected ? PLAYLIST_COLOR_HILITE_TRACK : 0x80808080;

        auto cell = [&](const wchar_t* s, int x, int w) {
          if (!s || !*s || w <= 0) return;
          RECT r2 = rect;
          r2.left  = x;
          r2.right = min(x + w, orig_rect.right);
          m_text.DrawTextW(GetFont(SIMPLE_FONT), const_cast<wchar_t*>(s), -1, &r2,
                           DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX,
                           color, false);
        };

        cell(bSelected ? L">" : L" ", xMark,  wMark);
        cell(g_settingsDesc[i].name,  xName,  wName);
        cell(valBuf,                  xValue, wValue);
        if (!bReadOnly)
          cell(GetSettingHint(g_settingsDesc[i].id), xHint, wHint);

        rect.top += rowH;
      }
    }
  }

  // 5. render *remaining* text to upper-right corner
  {
    // e) custom timed message — overlay thread handles rendering; keep erase + remote-send logic always
    if (!m_bWarningsDisabled2) {
      wchar_t buf[512] = { 0 };
      float t = GetTime();

      // Snapshot under the lock; draw outside it.
      //
      // Neither MyTextOut_Color nor SendMessageToMDropDX12Remote may run with
      // m_errorsMutex held -- the remote send is a blocking IPC call, and a
      // ToolWindow thread sitting in AddError waiting for the same lock would
      // stall the render thread behind it. Copying is cheap: the list holds a
      // handful of short-lived notifications.
      //
      // Expiry is applied in the same pass, which is where the old code did it.
      std::vector<ErrorMsg> live;
      {
        std::lock_guard<std::mutex> lk(m_errorsMutex);
        for (size_t i = 0; i < m_errors.size(); ) {
          if (t >= m_errors[i].birthTime && t < m_errors[i].expireTime) {
            live.push_back(m_errors[i]);
            i++;
          } else {
            m_errors.erase(m_errors.begin() + i);
          }
        }
      }

      // Ids whose remote send succeeded this frame. Written back afterwards,
      // because `live` is a copy and the flag has to survive it.
      std::vector<uint64_t> sentIds;

      for (const ErrorMsg& e : live) {
        if (e.category == ERR_MSG_BOTTOM_EXTRA_1 || e.category == ERR_MSG_BOTTOM_EXTRA_2 || e.category == ERR_MSG_BOTTOM_EXTRA_3) {
          int fontIndex = NUM_BASIC_FONTS + e.category - ERR_MSG_BOTTOM_EXTRA_1;
          SelectFont(static_cast<eFontIndex>(fontIndex));

          FormatTo(buf, L"%s ", e.msg.c_str());

          // 0..1
          float totalDuration = e.expireTime - e.birthTime;
          float age_rel;
          if (totalDuration > 3600.0f) {
            // Always-show: 0.5s fade in, then full alpha permanently
            float age = t - e.birthTime;
            age_rel = (age < 0.5f) ? (age / 0.5f) * 0.05f : 0.5f;
          } else {
            age_rel = (t - e.birthTime) / totalDuration;
          }
          DWORD cr = m_fontinfo[fontIndex].R;
          DWORD cg = m_fontinfo[fontIndex].G;
          DWORD cb = m_fontinfo[fontIndex].B;
          DWORD alpha = 0;
          if (age_rel >= 0.0f && age_rel < 0.05f) {
            alpha = (DWORD)(255 * (age_rel / 0.05f));
          }
          else if (age_rel > 0.8f && age_rel <= 1.0f) {
            alpha = (DWORD)(255 * ((1.0f - age_rel) / 0.2f));
          }
          else if (age_rel >= 0.05f && age_rel <= 0.8f) {
            alpha = 255;
          }
          DWORD z = (alpha << 24) | (cr << 16) | (cg << 8) | cb;
          if (m_SongInfoDisplayCorner == 1) {
            MyTextOut_Color(buf, MTO_UPPER_LEFT, z);
          }
          else if (m_SongInfoDisplayCorner == 2) {
            MyTextOut_Color(buf, MTO_UPPER_RIGHT, z);
          }
          else if (m_SongInfoDisplayCorner == 4) {
            MyTextOut_Color(buf, MTO_LOWER_RIGHT, z);
          }
          else {
            MyTextOut_Color(buf, MTO_LOWER_LEFT, z);
          }
        }
        else {
          // Always send to remote (regardless of overlay state)
          bool sent = e.bSentToRemote;
          if (!sent) {
            int res = SendMessageToMDropDX12Remote((L"STATUS=" + e.msg).c_str());
            sent = res != 0;
            if (sent) sentIds.push_back(e.id);
          }
          bool hideForRemote = sent && m_HideNotificationsWhenRemoteActive;
          if (m_bShowNotifications && !hideForRemote) {
            SelectFont(e.color ? TOOLTIP_FONT : SIMPLE_FONT);
            FormatTo(buf, L"%s ", e.msg.c_str());
            DWORD col = e.color ? e.color : GetFontColor(SIMPLE_FONT);
            MyTextOut_Color(buf, MTO_UPPER_RIGHT, col);
          }
        }
      }

      if (!sentIds.empty()) {
        std::lock_guard<std::mutex> lk(m_errorsMutex);
        for (ErrorMsg& msgEntry : m_errors)
          for (uint64_t id : sentIds)
            if (msgEntry.id == id) { msgEntry.bSentToRemote = true; break; }
      }
    }
  }
}


} // namespace mdrop
