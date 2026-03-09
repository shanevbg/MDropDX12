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
  // Post-process pass: applies the F11 inject effect and (for non-shader presets)
  // per-preset brighten/darken/solarize/invert on the composite back buffer.
  // Copies the back buffer to an intermediate texture, then draws it back with an effect shader.

  // Build per-preset effect bitmask from per-frame equation outputs.
  // In DX9, brighten/darken/solarize/invert are only applied by ShowToUser_NoShaders()
  // (milkdropfs.cpp line 6664-6744), which runs only for non-shader presets (PSVERSION_COMP=0).
  // When a comp shader is present, ShowToUser_Shaders() is called instead, which does NOT
  // apply these effects. Match that behavior: skip per-preset effects when comp PSO is active.
  UINT presetFxMask = 0;
  if (m_pState && !m_dx12CompPSO) {
    if (m_pState->var_pf_brighten && *m_pState->var_pf_brighten > 0.5) presetFxMask |= 1u;
    if (m_pState->var_pf_darken   && *m_pState->var_pf_darken   > 0.5) presetFxMask |= 2u;
    if (m_pState->var_pf_solarize && *m_pState->var_pf_solarize > 0.5) presetFxMask |= 4u;
    if (m_pState->var_pf_invert   && *m_pState->var_pf_invert   > 0.5) presetFxMask |= 8u;
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
  struct { UINT mode[4]; } cbData = { { (UINT)m_nInjectEffectMode, presetFxMask, srgbGamma, 0 } };
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
	m_text.DrawTextW(pFont, str, -1, &r, DT_NOPREFIX | ((corner == MTO_UPPER_RIGHT)?0:DT_SINGLELINE) | DT_WORD_ELLIPSIS | DT_CALCRECT | ((corner == MTO_UPPER_RIGHT) ? DT_RIGHT : 0), 0xFFFFFFFF, false, boxColor); \
    int w = r.right - r.left; \
    if      (corner == MTO_UPPER_LEFT ) SetRect(&r, xL, *upper_left_corner_y, xL+w, *upper_left_corner_y + h); \
    else if (corner == MTO_UPPER_RIGHT) SetRect(&r, xR-w, *upper_right_corner_y, xR, *upper_right_corner_y + h); \
    else if (corner == MTO_LOWER_LEFT ) SetRect(&r, xL, *lower_left_corner_y - h, xL+w, *lower_left_corner_y); \
    else if (corner == MTO_LOWER_RIGHT) SetRect(&r, xR-w, *lower_right_corner_y - h, xR, *lower_right_corner_y); \
	m_text.DrawTextW(pFont, str, -1, &r, DT_NOPREFIX | ((corner == MTO_UPPER_RIGHT)?0:DT_SINGLELINE) | DT_WORD_ELLIPSIS | ((corner == MTO_UPPER_RIGHT) ? DT_RIGHT: 0), 0xFFFFFFFF, bDarkBox, boxColor); \
    if      (corner == MTO_UPPER_LEFT ) *upper_left_corner_y  += h; \
    else if (corner == MTO_UPPER_RIGHT) *upper_right_corner_y += h; \
    else if (corner == MTO_LOWER_LEFT ) *lower_left_corner_y  -= h; \
    else if (corner == MTO_LOWER_RIGHT) *lower_right_corner_y -= h; \
}

#define MyTextOut_Color(str, corner, color) { \
    SetRect(&r, 0, 0, xR-xL, 2048); \
	m_text.DrawTextW(pFont, str, -1, &r, DT_NOPREFIX | ((corner == MTO_UPPER_RIGHT)?0:DT_SINGLELINE) | DT_WORD_ELLIPSIS | DT_CALCRECT | ((corner == MTO_UPPER_RIGHT) ? DT_RIGHT : 0), color, false, 0xFF000000); \
    int w = r.right - r.left; \
    if      (corner == MTO_UPPER_LEFT ) SetRect(&r, xL, *upper_left_corner_y, xL+w, *upper_left_corner_y + h); \
    else if (corner == MTO_UPPER_RIGHT) SetRect(&r, xR-w, *upper_right_corner_y, xR, *upper_right_corner_y + h); \
    else if (corner == MTO_LOWER_LEFT ) SetRect(&r, xL, *lower_left_corner_y - h, xL+w, *lower_left_corner_y); \
    else if (corner == MTO_LOWER_RIGHT) SetRect(&r, xR-w, *lower_right_corner_y - h, xR, *lower_right_corner_y); \
	m_text.DrawTextW(pFont, str, -1, &r, DT_NOPREFIX | ((corner == MTO_UPPER_RIGHT)?0:DT_SINGLELINE) | DT_WORD_ELLIPSIS | ((corner == MTO_UPPER_RIGHT) ? DT_RIGHT: 0), color, false, 0xFF000000); \
    if      (corner == MTO_UPPER_LEFT ) *upper_left_corner_y  += h; \
    else if (corner == MTO_UPPER_RIGHT) *upper_right_corner_y += h; \
    else if (corner == MTO_LOWER_LEFT ) *lower_left_corner_y  -= h; \
    else if (corner == MTO_LOWER_RIGHT) *lower_right_corner_y -= h; \
}

#define MyTextOut(str, corner, bDarkBox) MyTextOut_BGCOLOR(str, corner, bDarkBox, 0xFF000000)

#define MyTextOut_Shadow(str, corner) { \
    /* calc rect size */        \
    SetRect(&r, 0, 0, xR-xL, 2048); \
	m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_WORD_ELLIPSIS | DT_CALCRECT, 0xFFFFFFFF, false, 0xFF000000); \
    int w = r.right - r.left; \
    /* first the shadow */         \
    if      (corner == MTO_UPPER_LEFT ) SetRect(&r, xL, *upper_left_corner_y, xL+w, *upper_left_corner_y + h); \
    else if (corner == MTO_UPPER_RIGHT) SetRect(&r, xR-w, *upper_right_corner_y, xR, *upper_right_corner_y + h); \
    else if (corner == MTO_LOWER_LEFT ) SetRect(&r, xL, *lower_left_corner_y - h, xL+w, *lower_left_corner_y); \
    else if (corner == MTO_LOWER_RIGHT) SetRect(&r, xR-w, *lower_right_corner_y - h, xR, *lower_right_corner_y); \
    r.top += 1; r.left += 1;      \
    m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_WORD_ELLIPSIS, 0xFF000000, false, 0xFF000000); \
    /* now draw real text */            \
    r.top -= 1; r.left -= 1;       \
	m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_WORD_ELLIPSIS, 0xFFFFFFFF, false, 0xFF000000); \
    if      (corner == MTO_UPPER_LEFT ) *upper_left_corner_y  += h; \
    else if (corner == MTO_UPPER_RIGHT) *upper_right_corner_y += h; \
    else if (corner == MTO_LOWER_LEFT ) *lower_left_corner_y  -= h; \
    else if (corner == MTO_LOWER_RIGHT) *lower_right_corner_y -= h; \
}

#define MyTextOut_Shadow_Color(str, corner, color) { \
    /* calc rect size */        \
    SetRect(&r, 0, 0, xR-xL, 2048); \
	m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_WORD_ELLIPSIS | DT_CALCRECT, color, false, 0xFF000000); \
    int w = r.right - r.left; \
    /* first the shadow */         \
    if      (corner == MTO_UPPER_LEFT ) SetRect(&r, xL, *upper_left_corner_y, xL+w, *upper_left_corner_y + h); \
    else if (corner == MTO_UPPER_RIGHT) SetRect(&r, xR-w, *upper_right_corner_y, xR, *upper_right_corner_y + h); \
    else if (corner == MTO_LOWER_LEFT ) SetRect(&r, xL, *lower_left_corner_y - h, xL+w, *lower_left_corner_y); \
    else if (corner == MTO_LOWER_RIGHT) SetRect(&r, xR-w, *lower_right_corner_y - h, xR, *lower_right_corner_y); \
    r.top += 1; r.left += 1;      \
    m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_WORD_ELLIPSIS, 0xFF000000, false, 0xFF000000); \
    /* now draw real text */            \
    r.top -= 1; r.left -= 1;       \
	m_text.DrawTextW(pFont, (wchar_t*)str, -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_WORD_ELLIPSIS, color, false, 0xFF000000); \
    if      (corner == MTO_UPPER_LEFT ) *upper_left_corner_y  += h; \
    else if (corner == MTO_UPPER_RIGHT) *upper_right_corner_y += h; \
    else if (corner == MTO_LOWER_LEFT ) *lower_left_corner_y  -= h; \
    else if (corner == MTO_LOWER_RIGHT) *lower_right_corner_y -= h; \
}

void Engine::OnAltK() {
  AddNotification(wasabiApiLangString(IDS_PLEASE_EXIT_VIS_BEFORE_RUNNING_CONFIG_PANEL));
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
  if (category == ERR_NOTIFY)
    ClearErrors(category);

  assert(category != ERR_ALL);
  ErrorMsg x;
  x.msg = szMsg;
  x.birthTime = GetTime();
  x.expireTime = GetTime() + fDuration;
  x.category = category;
  x.bBold = bBold;
  x.bSentToRemote = false;
  x.color = 0; // default font color
  m_errors.push_back(x);
}

void Engine::AddNotificationColored(wchar_t* szMsg, float time, DWORD color) {
  DebugLogW(szMsg);
  ClearErrors(ERR_NOTIFY);

  ErrorMsg x;
  x.msg = szMsg;
  x.birthTime = GetTime();
  x.expireTime = GetTime() + time;
  x.category = ERR_NOTIFY;
  x.bBold = true;
  x.bSentToRemote = false;
  x.color = color;
  m_errors.push_back(x);
}

void Engine::ClearErrors(int category)  // 0=all categories
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
  wchar_t buf[512] = { 0 };
  LPD3DXFONT pFont = GetFont(DECORATIVE_FONT);
  int h = GetFontHeight(DECORATIVE_FONT);

  // 1. render text in upper-right corner - EXCEPT USER MESSAGE - it goes last b/c it draws a box under itself
  //                                        and it should be visible over everything else (usually an error msg)
  {
    // a) preset name — rendered by overlay thread; CTextManager used only as fallback
    if (m_bShowPresetInfo && !m_blackmode && !m_overlay.IsAlive()) {
      SelectFont(DECORATIVE_FONT);
      swprintf(
        buf,
        L"%s %s ",
        (m_bPresetLockedByUser || m_bPresetLockedByCode) && m_ShowLockSymbol ? L"\xD83D\xDD12" : L"",
        (m_nLoadingPreset != 0) ? m_pNewState->m_szDesc : m_pState->m_szDesc);

      DWORD alpha = 255;
      DWORD cr = m_fontinfo[DECORATIVE_FONT].R;
      DWORD cg = m_fontinfo[DECORATIVE_FONT].G;
      DWORD cb = m_fontinfo[DECORATIVE_FONT].B;
      DWORD color = (alpha << 24) | (cr << 16) | (cg << 8) | cb;
      MyTextOut_Color(buf, MTO_UPPER_RIGHT, color);
    }

    // b) preset rating — rendered by overlay thread; CTextManager used only as fallback
    if ((m_bShowRating || GetTime() < m_fShowRatingUntilThisTime) && !m_overlay.IsAlive()) {
      // see also: SetCurrentPresetRating() in milkdrop.cpp
      SelectFont(SIMPLE_FONT);
      swprintf(buf, L" %s: %d ", wasabiApiLangString(IDS_RATING), (int)m_pState->m_fRating);
      if (!m_bEnableRating) lstrcatW(buf, wasabiApiLangString(IDS_DISABLED));
      MyTextOut_Shadow(buf, MTO_UPPER_RIGHT);
    }

    // Feed data to overlay thread (handles FPS, debug info, and menu rendering).
    // Always send when alive so overlay clears to transparent when nothing is shown.
    if (m_overlay.IsAlive()) {
      OverlayData od = {};
      od.bShowFPS         = m_bShowFPS;
      od.bShowDebugInfo   = m_bShowDebugInfo;
      od.bPresetLocked    = m_bPresetLockedByUser || m_bPresetLockedByCode;
      od.bEnableMouseInteraction = m_bEnableMouseInteraction;
      od.clientWidth      = m_lpDX->m_client_width;
      od.clientHeight     = m_lpDX->m_client_height;
      od.fps              = GetFps();
      od.fRenderQuality   = m_fRenderQuality;
      od.fColShiftHue     = m_ColShiftHue;
      od.fColShiftSaturation = m_ColShiftSaturation;
      od.fColShiftBrightness = m_ColShiftBrightness;
      if (m_pState) {
        od.bass         = (float)(*m_pState->var_pf_bass);
        od.bass_att     = (float)(*m_pState->var_pf_bass_att);
        od.bass_smooth  = (float)(*m_pState->var_pf_bass_smooth);
        od.mid          = (float)(*m_pState->var_pf_mid);
        od.mid_att      = (float)(*m_pState->var_pf_mid_att);
        od.mid_smooth   = (float)(*m_pState->var_pf_mid_smooth);
        od.treb         = (float)(*m_pState->var_pf_treb);
        od.treb_att     = (float)(*m_pState->var_pf_treb_att);
        od.treb_smooth  = (float)(*m_pState->var_pf_treb_smooth);
        od.pfMonitor    = (float)(*m_pState->var_pf_monitor);
      }
      od.bass_imm_rel    = mysound.imm_rel[0];
      od.bass_avg_rel    = mysound.avg_rel[0];
      od.bass_smooth_rel = mysound.smooth_rel[0];
      od.mid_imm_rel     = mysound.imm_rel[1];
      od.mid_avg_rel     = mysound.avg_rel[1];
      od.mid_smooth_rel  = mysound.smooth_rel[1];
      od.treb_imm_rel    = mysound.imm_rel[2];
      od.treb_avg_rel    = mysound.avg_rel[2];
      od.treb_smooth_rel = mysound.smooth_rel[2];
      od.presetTime      = GetTime() - m_fPresetStartTime;
      od.mouseX          = m_mouseX;
      od.mouseY          = m_mouseY;
      od.mouseDown       = m_mouseDown;

      // HUD: preset name
      od.bShowPresetName = m_bShowPresetInfo && !m_blackmode;
      if (od.bShowPresetName) {
        swprintf(od.szPresetName, 256, L"%s%s ",
          (m_bPresetLockedByUser || m_bPresetLockedByCode) && m_ShowLockSymbol ? L"\xD83D\xDD12 " : L"",
          (m_nLoadingPreset != 0) ? m_pNewState->m_szDesc : m_pState->m_szDesc);
        od.presetNameColor = ((DWORD)m_fontinfo[DECORATIVE_FONT].R << 16)
                           | ((DWORD)m_fontinfo[DECORATIVE_FONT].G << 8)
                           |  (DWORD)m_fontinfo[DECORATIVE_FONT].B;
      }

      // HUD: rating
      od.bShowRating = m_bShowRating || GetTime() < m_fShowRatingUntilThisTime;
      if (od.bShowRating && m_pState) {
        swprintf(od.szRating, 64, L" %s: %d ", wasabiApiLangString(IDS_RATING), (int)m_pState->m_fRating);
        if (!m_bEnableRating) lstrcatW(od.szRating, wasabiApiLangString(IDS_DISABLED));
      }

      // HUD: song title
      od.bShowSongTitle = m_bShowSongTitle;
      if (od.bShowSongTitle)
        GetSongTitle(od.szSongTitle, sizeof(od.szSongTitle));

      // HUD: notifications
      od.nNotifications = 0;
      if (!m_bWarningsDisabled2) {
        float tN = GetTime();
        int nErr = (int)m_errors.size();
        for (int i = 0; i < nErr && od.nNotifications < OverlayData::OVERLAY_MAX_NOTIFICATIONS; i++) {
          if (tN < m_errors[i].birthTime || tN >= m_errors[i].expireTime) continue;
          int cat = m_errors[i].category;
          if (cat == ERR_MSG_BOTTOM_EXTRA_1 || cat == ERR_MSG_BOTTOM_EXTRA_2 || cat == ERR_MSG_BOTTOM_EXTRA_3) {
            auto& n = od.notifications[od.nNotifications++];
            swprintf(n.text, 256, L"%s ", m_errors[i].msg.c_str());
            int fi = NUM_BASIC_FONTS + cat - ERR_MSG_BOTTOM_EXTRA_1;
            n.color = ((DWORD)m_fontinfo[fi].R << 16) | ((DWORD)m_fontinfo[fi].G << 8) | (DWORD)m_fontinfo[fi].B;
            int sc = m_SongInfoDisplayCorner;
            n.corner = (sc == 1) ? MTO_UPPER_LEFT : (sc == 2) ? MTO_UPPER_RIGHT : (sc == 4) ? MTO_LOWER_RIGHT : MTO_LOWER_LEFT;
          }
          else if (!m_errors[i].bSentToRemote || !m_HideNotificationsWhenRemoteActive) {
            auto& n = od.notifications[od.nNotifications++];
            swprintf(n.text, 256, L"%s ", m_errors[i].msg.c_str());
            n.color = m_errors[i].color ? (m_errors[i].color & 0x00FFFFFF) : 0x00FFFFFF;
            n.corner = 0; // upper-right
          }
        }
      }

      // Menu data extraction — replicate DrawMenu layout for overlay rendering
      if (m_UI_mode == UI_MENU && m_pCurMenu) {
        od.bShowMenu = true;
        int fontH = GetFontHeight(SIMPLE_FONT);
        if (fontH <= 0) fontH = 20;
        od.menuFontHeight = fontH;

        int menuTop  = *upper_left_corner_y;
        int menuLeft = xL;
        int availH   = *lower_left_corner_y - menuTop;

        int nLines = (availH - PLAYLIST_INNER_MARGIN * 2) / fontH - 1;  // -1 for tooltip
        if (nLines < 1) nLines = 1;

        int curSel = m_pCurMenu->GetCurSel();
        int nStart = (curSel / nLines) * nLines;

        int nMenuLines = 0;
        int lineY = menuTop + PLAYLIST_INNER_MARGIN;
        int lineX = menuLeft + PLAYLIST_INNER_MARGIN;

        if (!m_pCurMenu->IsEditingCurSel()) {
          int nLinesDrawn = 0;
          int idx = 0;

          // Child menus
          for (int cm = 0; cm < m_pCurMenu->GetNumChildMenus(); cm++) {
            CMilkMenu* pChild = m_pCurMenu->GetChildMenu(cm);
            if (idx >= nStart && idx < nStart + nLines) {
              if (pChild && pChild->IsEnabled() && nMenuLines < OVERLAY_MAX_MENU_LINES) {
                OverlayMenuLine& line = od.menuLines[nMenuLines];
                swprintf(line.text, 256, L"%s", pChild->GetName());
                line.color = (idx == curSel) ? MENU_HILITE_COLOR : MENU_COLOR;
                line.x = lineX;
                line.y = lineY;
                lineY += fontH;
                nMenuLines++;
                nLinesDrawn++;
              }
              if (m_bShowMenuToolTips && idx == curSel) {
                od.bShowTooltip = true;
                wcsncpy(od.tooltip, wasabiApiLangString(IDS_SZ_MENU_NAV_TOOLTIP), 1023);
                od.tooltip[1023] = 0;
              }
            }
            idx++;
          }

          // Child items
          CMilkMenuItem* pItem = m_pCurMenu->GetFirstChildItem();
          while (pItem && nLinesDrawn < nStart + nLines) {
            if (!pItem->m_bEnabled) { pItem = pItem->m_pNext; idx++; continue; }
            if (idx >= nStart && nMenuLines < OVERLAY_MAX_MENU_LINES) {
              OverlayMenuLine& line = od.menuLines[nMenuLines];
              size_t addr = pItem->m_var_offset + (size_t)m_pState;
              switch (pItem->m_type) {
              case MENUITEMTYPE_BOOL:
                swprintf(line.text, 256, L"%s [%s]", pItem->m_szName,
                  *((bool*)addr) ? L"ON" : L"OFF");
                break;
              default:
                wcsncpy(line.text, pItem->m_szName, 255);
                line.text[255] = 0;
                break;
              }
              line.color = (idx == curSel) ? MENU_HILITE_COLOR : MENU_COLOR;
              line.x = lineX;
              line.y = lineY;
              lineY += fontH;
              nMenuLines++;
              nLinesDrawn++;

              if (m_bShowMenuToolTips && idx == curSel && pItem->m_szToolTip[0]) {
                od.bShowTooltip = true;
                wcsncpy(od.tooltip, pItem->m_szToolTip, 1023);
                od.tooltip[1023] = 0;
              }
            }
            pItem = pItem->m_pNext;
            idx++;
          }
        } else {
          // Editing current selection — show instructions + current value
          CMilkMenuItem* pItem = m_pCurMenu->GetFirstChildItem();
          for (int sk = m_pCurMenu->GetNumChildMenus(); sk < curSel; sk++)
            pItem = pItem->m_pNext;
          size_t addr = pItem->m_var_offset + (size_t)m_pState;

          if (nMenuLines < OVERLAY_MAX_MENU_LINES) {
            OverlayMenuLine& line = od.menuLines[nMenuLines];
            wcsncpy(line.text, wasabiApiLangString(IDS_USE_UP_DOWN_ARROW_KEYS), 255);
            line.text[255] = 0;
            line.color = MENU_COLOR;
            line.x = lineX; line.y = lineY; lineY += fontH;
            nMenuLines++;
          }
          if (nMenuLines < OVERLAY_MAX_MENU_LINES) {
            OverlayMenuLine& line = od.menuLines[nMenuLines];
            swprintf(line.text, 256, wasabiApiLangString(IDS_CURRENT_VALUE_OF_X), pItem->m_szName);
            line.color = MENU_COLOR;
            line.x = lineX; line.y = lineY; lineY += fontH;
            nMenuLines++;
          }
          if (nMenuLines < OVERLAY_MAX_MENU_LINES) {
            OverlayMenuLine& line = od.menuLines[nMenuLines];
            switch (pItem->m_type) {
            case MENUITEMTYPE_INT:
              swprintf(line.text, 256, L" %d ", *((int*)addr));
              break;
            case MENUITEMTYPE_FLOAT:
            case MENUITEMTYPE_LOGFLOAT:
              swprintf(line.text, 256, L" %5.3f ", *((float*)addr));
              break;
            case MENUITEMTYPE_BLENDABLE:
            case MENUITEMTYPE_LOGBLENDABLE:
              swprintf(line.text, 256, L" %5.3f ", ((CBlendableFloat*)addr)->eval(-1));
              break;
            default:
              wcscpy(line.text, L" ? ");
              break;
            }
            line.color = MENU_HILITE_COLOR;
            line.x = lineX; line.y = lineY; lineY += fontH;
            nMenuLines++;
          }
          if (m_bShowMenuToolTips && pItem->m_szToolTip[0]) {
            od.bShowTooltip = true;
            wcsncpy(od.tooltip, pItem->m_szToolTip, 1023);
            od.tooltip[1023] = 0;
          }
        }

        od.nMenuLines = nMenuLines;

        // Dark background box rect
        od.menuBox.left   = menuLeft;
        od.menuBox.top    = menuTop;
        od.menuBox.right  = xR;
        od.menuBox.bottom = lineY + PLAYLIST_INNER_MARGIN;

        // Tooltip position (lower-right corner)
        if (od.bShowTooltip) {
          od.tooltipX = xR - 500;
          od.tooltipY = *lower_right_corner_y - fontH - TEXT_MARGIN;
        }
      }

      m_overlay.UpdateData(od);
    }

    // c) fps display (fallback to CTextManager if overlay thread is dead)
    if (m_bShowFPS && !m_overlay.IsAlive()) {
      SelectFont(SIMPLE_FONT);
      swprintf(buf, L"%s: %4.2f ", wasabiApiLangString(IDS_FPS), GetFps()); // leave extra space @ end, so italicized fonts don't get clipped
      MyTextOut_Shadow(buf, MTO_UPPER_RIGHT);
    }

    // d) debug information (fallback to CTextManager if overlay thread is dead)
    if (m_bShowDebugInfo && !m_overlay.IsAlive()) {
      SelectFont(SIMPLE_FONT);
      DWORD color = GetFontColor(SIMPLE_FONT);

      swprintf(buf, L"  %6.2f %s", (float)(*m_pState->var_pf_monitor), wasabiApiLangString(IDS_PF_MONITOR));
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      if (!m_bPresetLockedByUser && !m_bPresetLockedByCode) {
        swprintf(buf, L"  %6.2f %s", (float)(GetTime() - m_fPresetStartTime), L"time");
        MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      }

      swprintf(buf, L"%s %6.2f %s", ((double)mysound.imm_rel[0] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_bass), L"bass");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      swprintf(buf, L"%s %6.2f %s", ((double)mysound.avg_rel[0] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_bass_att), L"bass_att");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      swprintf(buf, L"%s %6.2f %s", ((double)mysound.smooth_rel[0] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_bass_smooth), L"bass_smooth");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      swprintf(buf, L"%s %6.2f %s", ((double)mysound.imm_rel[1] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_mid), L"mid");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      swprintf(buf, L"%s %6.2f %s", ((double)mysound.avg_rel[1] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_mid_att), L"mid_att");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      swprintf(buf, L"%s %6.2f %s", ((double)mysound.smooth_rel[1] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_mid_smooth), L"mid_smooth");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      swprintf(buf, L"%s %6.2f %s", ((double)mysound.imm_rel[2] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_treb), L"treb");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      swprintf(buf, L"%s %6.2f %s", ((double)mysound.avg_rel[2] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_treb_att), L"treb_att");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);
      swprintf(buf, L"%s %6.2f %s", ((double)mysound.smooth_rel[2] >= 1.3) ? L"+" : L" ", (float)(*m_pState->var_pf_treb_smooth), L"treb_smooth");
      MyTextOut_Color(buf, MTO_UPPER_LEFT, color);

      swprintf(buf, L"q=%.2f hue=%.2f sat=%.2f bri=%.2f", m_fRenderQuality, m_ColShiftHue, m_ColShiftSaturation, m_ColShiftBrightness);
      MyTextOut_Color(buf, MTO_LOWER_RIGHT, color);

      if (m_bEnableMouseInteraction) {
        swprintf(buf, L"%s x=%0.2f y=%0.2f z=%s ", L"mouse", m_mouseX, m_mouseY, m_mouseDown ? L"1" : L"0");
        MyTextOut_Color(buf, MTO_LOWER_RIGHT, color);
      }
    }
    // NOTE: custom timed msg comes at the end!!
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

    // render song title in lower-left corner — overlay thread handles it; CTextManager is fallback
    if (m_bShowSongTitle && !m_overlay.IsAlive()) {
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
                  swprintf(buf3, L"%s / %s ", buf, buf2);
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
        if (m_bShowShaderHelp) {
          MyTextOut(wasabiApiLangString(IDS_PRESS_F9_TO_HIDE_SHADER_QREF), MTO_UPPER_LEFT, true);
        }
        else {
          MyTextOut(wasabiApiLangString(IDS_PRESS_F9_TO_SHOW_SHADER_QREF), MTO_UPPER_LEFT, true);
        }
        *upper_left_corner_y += h * 2 / 3;

        if (m_bShowShaderHelp) {
          // draw dark box - based on longest line & # lines...
          SetRect(&r, 0, 0, 2048, 2048);
          m_text.DrawTextW(pFont, wasabiApiLangString(IDS_STRING615), -1, &r, DT_NOPREFIX | DT_SINGLELINE | DT_WORD_ELLIPSIS | DT_CALCRECT, 0xFFFFFFFF, false, 0xFF000000);
          RECT darkbox;
          SetRect(&darkbox, xL, *upper_left_corner_y - 2, xL + r.right - r.left, *upper_left_corner_y + (r.bottom - r.top) * 13 + 2);
          DrawDarkTranslucentBox(&darkbox);

          MyTextOut(wasabiApiLangString(IDS_STRING616), MTO_UPPER_LEFT, false);
          MyTextOut(wasabiApiLangString(IDS_STRING617), MTO_UPPER_LEFT, false);
          MyTextOut(wasabiApiLangString(IDS_STRING618), MTO_UPPER_LEFT, false);
          MyTextOut(wasabiApiLangString(IDS_STRING619), MTO_UPPER_LEFT, false);
          MyTextOut(wasabiApiLangString(IDS_STRING620), MTO_UPPER_LEFT, false);
          MyTextOut(wasabiApiLangString(IDS_STRING621), MTO_UPPER_LEFT, false);
          if (bIsWarp) {
            MyTextOut(wasabiApiLangString(IDS_STRING622), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING623), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING624), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING625), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING626), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING627), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING628), MTO_UPPER_LEFT, false);
          }
          else if (bIsComp) {
            MyTextOut(wasabiApiLangString(IDS_STRING629), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING630), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING631), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING632), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING633), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING634), MTO_UPPER_LEFT, false);
            MyTextOut(wasabiApiLangString(IDS_STRING635), MTO_UPPER_LEFT, false);
          }
          *upper_left_corner_y += h * 2 / 3;
        }
      }
      else if (m_UI_mode == UI_SAVEAS && (m_bWarpShaderLock || m_bCompShaderLock)) {
        wchar_t buf[256] = { 0 };
        int shader_msg_id = IDS_COMPOSITE_SHADER_LOCKED;
        if (m_bWarpShaderLock && m_bCompShaderLock)
          shader_msg_id = IDS_WARP_AND_COMPOSITE_SHADERS_LOCKED;
        else if (m_bWarpShaderLock && !m_bCompShaderLock)
          shader_msg_id = IDS_WARP_SHADER_LOCKED;
        else
          shader_msg_id = IDS_COMPOSITE_SHADER_LOCKED;

        wasabiApiLangString(shader_msg_id, buf, 256);
        MyTextOut_BGCOLOR(buf, MTO_UPPER_LEFT, true, 0xFF000000);
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

      lstrcpyW(buf, m_waitstring.szText);
      lstrcpyA(bufA, (char*)m_waitstring.szText);

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
            m_text.DrawTextA(GetFont(SIMPLE_FONT), buf2, -1, &r2, DT_CALCRECT /*| DT_WORDBREAK*/, 0xFFFFFFFF, false);
            int h = r2.bottom - r2.top;
            ypixels += h;
            bufA[pos] = ch;

            if (start > m_waitstring.nCursorPos) // make sure 'box' gets updated for each line on this page
              exit_on_next_page = 1;

            if (ypixels > rect.bottom - rect.top) // this line belongs on the next page
            {
              if (exit_on_next_page) {
                bufA[start] = 0; // so text stops where the box stops, when we draw the text
                break;
              }

              ypixels = h;
              top_of_page_pos = start;
              page++;

              box = rect;
              box.right = box.left;
              box.bottom = box.top;
            }
            box.bottom += h;
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
          swprintf(m_waitstring.szToolTip, wasabiApiLangString(IDS_PAGE_X), page);
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
            rect.top += m_text.DrawTextA(GetFont(SIMPLE_FONT), buf2, -1, &rect, 0/*DT_WORDBREAK*/, color, false);
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
        swprintf(buf2, L"    %sX", buf);  // put a final 'X' instead of ' ' b/c CALCRECT returns w==0 if string is entirely whitespace!
        m_text.DrawTextW(GetFont(SIMPLE_FONT), buf2, -1, &box, DT_CALCRECT, MENU_COLOR, false);

        // use r2 to draw a dark box:
        box.top -= PLAYLIST_INNER_MARGIN;
        box.left -= PLAYLIST_INNER_MARGIN;
        box.right += PLAYLIST_INNER_MARGIN;
        box.bottom += PLAYLIST_INNER_MARGIN;
        DrawDarkTranslucentBox(&box);
        *upper_left_corner_y += box.bottom - box.top + PLAYLIST_INNER_MARGIN * 3;

        swprintf(buf2, L"    %s ", buf);
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

      if (m_pState->m_nWarpPSVersion >= m_nMaxPSVersion &&
        m_pState->m_nCompPSVersion >= m_nMaxPSVersion) {
        assert(m_pState->m_nMaxPSVersion == m_nMaxPSVersion);
        wchar_t buf[1024] = { 0 };
        swprintf(buf, wasabiApiLangString(IDS_PRESET_USES_HIGHEST_PIXEL_SHADER_VERSION), m_nMaxPSVersion);
        rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), buf, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
        rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESS_ESC_TO_RETURN), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      }
      else {
        if (m_pState->m_nMinPSVersion != m_pState->m_nMaxPSVersion) {
          switch (m_pState->m_nMinPSVersion) {
          case MD2_PS_NONE:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_HAS_MIXED_VERSIONS_OF_SHADERS), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_SHADERS_TO_USE_PS2), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_2_0:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_HAS_MIXED_VERSIONS_OF_SHADERS), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_SHADERS_TO_USE_PS2X), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_2_X:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_HAS_MIXED_VERSIONS_OF_SHADERS), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_SHADERS_TO_USE_PS3), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
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
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_DOES_NOT_USE_PIXEL_SHADERS), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_TO_USE_PS2), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_OLD_GPU_MIGHT_NOT_WORK_WITH_PRESET), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_2_0:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_CURRENTLY_USES_PS2), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_TO_USE_PS2X), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_OLD_GPU_MIGHT_NOT_WORK_WITH_PRESET), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_2_X:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_CURRENTLY_USES_PS2X), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_TO_USE_PS3), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_OLD_GPU_MIGHT_NOT_WORK_WITH_PRESET), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            break;
          case MD2_PS_3_0:
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_PRESET_CURRENTLY_USES_PS3), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_UPGRADE_TO_USE_PS4), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
            rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_OLD_GPU_MIGHT_NOT_WORK_WITH_PRESET), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
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
      rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_ARE_YOU_SURE_YOU_WANT_TO_DELETE_PRESET), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      swprintf(buf, wasabiApiLangString(IDS_PRESET_TO_DELETE), m_presets[m_nPresetListCurPos].szFilename.c_str());
      rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), buf, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      *upper_left_corner_y = rect.top;
    }
    else if (m_UI_mode == UI_SAVE_OVERWRITE) {
      RECT rect;
      SetRect(&rect, xL, *upper_left_corner_y, xR, *lower_left_corner_y);
      rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_FILE_ALREADY_EXISTS_OVERWRITE_IT), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      swprintf(buf, wasabiApiLangString(IDS_FILE_IN_QUESTION_X_MILK), m_waitstring.szText);
      rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), buf, -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, MENU_COLOR, true);
      if (m_bWarpShaderLock)
        rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_DO_NOT_FORGET_WARP_SHADER_WAS_LOCKED), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, 0xFFFFFFFF, true, 0xFFCC0000);
      if (m_bCompShaderLock)
        rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), wasabiApiLangString(IDS_WARNING_DO_NOT_FORGET_COMPOSITE_SHADER_WAS_LOCKED), -1, &rect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, 0xFFFFFFFF, true, 0xFFCC0000);
      *upper_left_corner_y = rect.top;
    }
    else if (m_UI_mode == UI_MASHUP) {
      if (m_nPresets - m_nDirs == 0) {
        if (wcscmp(m_pState->m_szDesc, INVALID_PRESET_DESC) == 0) {
          wchar_t buf[1024];
          swprintf(buf, wasabiApiLangString(IDS_ERROR_NO_PRESET_FILE_FOUND_IN_X_MILK), m_szPresetDir);
          AddError(buf, 6.0f, ERR_MISC, true);
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
            swprintf(szFile, L"%s%s", m_szPresetDir, m_presets[m_nMashPreset[mash]].szFilename.c_str());

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

        MyTextOut(wasabiApiLangString(IDS_PRESET_MASH_UP_TEXT1), MTO_UPPER_LEFT, true);
        MyTextOut(wasabiApiLangString(IDS_PRESET_MASH_UP_TEXT2), MTO_UPPER_LEFT, true);
        MyTextOut(wasabiApiLangString(IDS_PRESET_MASH_UP_TEXT3), MTO_UPPER_LEFT, true);
        MyTextOut(wasabiApiLangString(IDS_PRESET_MASH_UP_TEXT4), MTO_UPPER_LEFT, true);
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
        wchar_t str[512], str2[512];

        if (last_line > m_nPresets)
          last_line = m_nPresets;

        // tooltip:
        if (m_bShowMenuToolTips) {
          wchar_t buf[256];
          swprintf(buf, wasabiApiLangString(IDS_PAGE_X_OF_X), m_nMashPreset[m_nMashSlot] / lines_available + 1, (m_nPresets + lines_available - 1) / lines_available);
          DrawTooltip(buf, xR, *lower_right_corner_y);
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
          int h = 0;

          int start_y = orig_rect.top;
          for (int mash = 0; mash < MASH_SLOTS; mash++) {
            int idx = m_nMashPreset[mash];

            wchar_t buf[1024];
            // SPOUT
                        // swprintf(buf, L"%s%s", wasabiApiLangString(mashNames[mash]), m_presets[idx].szFilename);
            swprintf(buf, L"%s%s", wasabiApiLangString(mashNames[mash]), m_presets[idx].szFilename.c_str());
            RECT r2 = orig_rect;
            r2.top += h;
            h += m_text.DrawTextW(GetFont(SIMPLE_FONT), buf, -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | (pass == 0 ? DT_CALCRECT : 0), (mash == m_nMashSlot) ? PLAYLIST_COLOR_HILITE_TRACK : PLAYLIST_COLOR_NORMAL, false);
            w = max(w, r2.right - r2.left);
          }
          if (pass == 0) {
            box.right = box.left + w;
            box.bottom = box.top + h;
            DrawDarkTranslucentBox(&box);
          }
          else
            orig_rect.top += h;
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
          for (int i = first_line; i < last_line && m_presets[i].szFilename.c_str(); i++) {
            // remove the extension before displaying the filename.  also pad w/spaces.
            //lstrcpy(str, m_pPresetAddr[i]);
            bool bIsDir = (m_presets[i].szFilename.c_str()[0] == '*');
            bool bIsRunning = false;
            bool bIsSelected = (i == m_nMashPreset[m_nMashSlot]);

            if (bIsDir) {
              // directory
              if (wcscmp(m_presets[i].szFilename.c_str() + 1, L"..") == 0)
                swprintf(str2, L" [ %s ] (%s) ", m_presets[i].szFilename.c_str() + 1, wasabiApiLangString(IDS_PARENT_DIRECTORY));
              else
                swprintf(str2, L" [ %s ] ", m_presets[i].szFilename.c_str() + 1);
            }
            else {
              // preset file
              lstrcpyW(str, m_presets[i].szFilename.c_str());
              RemoveExtension(str);
              swprintf(str2, L" %s ", str);

              if (wcscmp(m_presets[m_nMashPreset[m_nMashSlot]].szFilename.c_str(), str) == 0)
                bIsRunning = true;
            }

            if (bIsRunning && m_bPresetLockedByUser)
              lstrcatW(str2, wasabiApiLangString(IDS_LOCKED));

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
        MyTextOut(wasabiApiLangString(IDS_LOAD_WHICH_PRESET_PLUS_COMMANDS), MTO_UPPER_LEFT, true);

        wchar_t buf[MAX_PATH + 64];
        swprintf(buf, wasabiApiLangString(IDS_DIRECTORY_OF_X), m_szPresetDir);
        MyTextOut(buf, MTO_UPPER_LEFT, true);

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
          //lstrcpy(m_szLastPresetSelected, m_presets[m_nPresetListCurPos].szFilename.c_str());

          m_bUserPagedDown = false;
        }

        if (m_bUserPagedUp) {
          m_nPresetListCurPos -= lines_available;
          if (m_nPresetListCurPos < 0)
            m_nPresetListCurPos = 0;

          // remember this preset's name so the next time they hit 'L' it jumps straight to it
          //lstrcpy(m_szLastPresetSelected, m_presets[m_nPresetListCurPos].szFilename.c_str());

          m_bUserPagedUp = false;
        }

        int first_line = m_nPresetListCurPos - (m_nPresetListCurPos % lines_available);
        int last_line = first_line + lines_available;
        wchar_t str[512], str2[512];

        if (last_line > m_nPresets)
          last_line = m_nPresets;

        // tooltip:
        if (m_bShowMenuToolTips) {
          wchar_t buf[256];
          swprintf(buf, wasabiApiLangString(IDS_PAGE_X_OF_X), m_nPresetListCurPos / lines_available + 1, (m_nPresets + lines_available - 1) / lines_available);
          DrawTooltip(buf, xR, *lower_right_corner_y);
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
          for (int i = first_line; i < last_line && m_presets[i].szFilename.c_str(); i++) {
            // remove the extension before displaying the filename.  also pad w/spaces.
            //lstrcpy(str, m_pPresetAddr[i]);
            bool bIsDir = (m_presets[i].szFilename.c_str()[0] == '*');
            bool bIsRunning = (i == m_nCurrentPreset);//false;
            bool bIsSelected = (i == m_nPresetListCurPos);

            if (bIsDir) {
              // directory
              if (wcscmp(m_presets[i].szFilename.c_str() + 1, L"..") == 0)
                swprintf(str2, L" [ %s ] (%s) ", m_presets[i].szFilename.c_str() + 1, wasabiApiLangString(IDS_PARENT_DIRECTORY));
              else
                swprintf(str2, L" [ %s ] ", m_presets[i].szFilename.c_str() + 1);
            }
            else {
              // preset file
              lstrcpyW(str, m_presets[i].szFilename.c_str());
              RemoveExtension(str);
              swprintf(str2, L" %s ", str);

              //if (lstrcmp(m_pState->m_szDesc, str)==0)
            //    bIsRunning = true;
            }

            if (bIsRunning && m_bPresetLockedByUser)
              lstrcatW(str2, wasabiApiLangString(IDS_LOCKED));

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
      MyTextOut(L"MDROPDX12 SETTINGS  (F2 to close, UP/DOWN to navigate)", MTO_UPPER_LEFT, true);

      wchar_t iniPath[MAX_PATH + 64];
      swprintf(iniPath, L"Config: %s", GetConfigIniFile());
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
      RECT box;
      box.top = rect.top;
      box.left = rect.left;
      box.right = rect.left;
      box.bottom = rect.top;

      for (int pass = 0; pass < 2; pass++) {
        rect = orig_rect;
        for (int i = 0; i < SET_COUNT; i++) {
          bool bSelected = (i == m_nSettingsCurSel);

          wchar_t valBuf[MAX_PATH];
          GetSettingValueString(g_settingsDesc[i].id, valBuf, MAX_PATH);
          const wchar_t* hint = GetSettingHint(g_settingsDesc[i].id);

          wchar_t line[1024];
          if (g_settingsDesc[i].type == ST_READONLY)
            swprintf(line, L" %s%-24s %s", bSelected ? L"> " : L"  ", g_settingsDesc[i].name, valBuf);
          else
            swprintf(line, L" %s%-24s %-40s  [%s]", bSelected ? L"> " : L"  ", g_settingsDesc[i].name, valBuf, hint);

          DWORD color = PLAYLIST_COLOR_NORMAL;
          if (bSelected)
            color = PLAYLIST_COLOR_HILITE_TRACK;
          if (g_settingsDesc[i].type == ST_READONLY)
            color = bSelected ? PLAYLIST_COLOR_HILITE_TRACK : 0x80808080;

          RECT r2 = rect;
          rect.top += m_text.DrawTextW(GetFont(SIMPLE_FONT), line, -1, &r2,
            DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | (pass == 0 ? DT_CALCRECT : 0),
            color, false);

          if (pass == 0) {
            box.right = max(box.right, box.left + r2.right - r2.left);
            box.bottom += r2.bottom - r2.top;
          }
        }
        if (pass == 0) {
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

  // 5. render *remaining* text to upper-right corner
  {
    // e) custom timed message — overlay thread handles rendering; keep erase + remote-send logic always
    if (!m_bWarningsDisabled2) {
      wchar_t buf[512] = { 0 };
      float t = GetTime();
      int N = (int)m_errors.size();
      for (int i = 0; i < N; i++) {
        if (t >= m_errors[i].birthTime && t < m_errors[i].expireTime) {
          if (m_errors[i].category == ERR_MSG_BOTTOM_EXTRA_1 || m_errors[i].category == ERR_MSG_BOTTOM_EXTRA_2 || m_errors[i].category == ERR_MSG_BOTTOM_EXTRA_3) {
            // Overlay thread renders these; CTextManager used only as fallback
            if (!m_overlay.IsAlive()) {
              int fontIndex = NUM_BASIC_FONTS + m_errors[i].category - ERR_MSG_BOTTOM_EXTRA_1;
              SelectFont(static_cast<eFontIndex>(fontIndex));

              swprintf(buf, L"%s ", m_errors[i].msg.c_str());

              // 0..1
              float totalDuration = m_errors[i].expireTime - m_errors[i].birthTime;
              float age_rel;
              if (totalDuration > 3600.0f) {
                // Always-show: 0.5s fade in, then full alpha permanently
                float age = t - m_errors[i].birthTime;
                age_rel = (age < 0.5f) ? (age / 0.5f) * 0.05f : 0.5f;
              } else {
                age_rel = (t - m_errors[i].birthTime) / totalDuration;
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
          }
          else {
            // Always send to remote (regardless of overlay state)
            if (!m_errors[i].bSentToRemote) {
              int res = SendMessageToMDropDX12Remote((L"STATUS=" + m_errors[i].msg).c_str());
              m_errors[i].bSentToRemote = res != 0;
            }
            // Overlay thread renders these; CTextManager used only as fallback
            if (!m_overlay.IsAlive()) {
              if (!m_errors[i].bSentToRemote || !m_HideNotificationsWhenRemoteActive) {
                SelectFont(m_errors[i].color ? TOOLTIP_FONT : SIMPLE_FONT);
                swprintf(buf, L"%s ", m_errors[i].msg.c_str());
                DWORD col = m_errors[i].color ? m_errors[i].color : GetFontColor(SIMPLE_FONT);
                MyTextOut_Color(buf, MTO_UPPER_RIGHT, col);
              }
            }
          }
        }
        else {
          m_errors.erase(m_errors.begin() + i);
          i--;
          N--;
        }
      }
    }
  }
}


} // namespace mdrop
