/*
  LICENSE
  -------
Copyright 2005-2013 Nullsoft, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the name of Nullsoft nor the names of its contributors may be used to
    endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//
// SPOUT
// 
//	15.05.23 - Change from SpoutLibrary to SpoutDX support class
//


#include <map>
#include <set>
#include "engine.h"
#include "video_capture.h"
#include "resource.h"
#include "format_to.h"
#include "support.h"
#include <d3dcompiler.h>
//#include "evallib\eval.h"		// for math. expr. eval - thanks Francis! (in SourceOffSite, it's the 'vis_avs\evallib' project.)
//#include "evallib\compiler.h"
#include "../ns-eel2/ns-eel.h"
#include "utility.h"
#include <assert.h>
#include <math.h>
#include <algorithm>  // std::swap
#include <thread>
#include "config_store.h"

using mdrop::Config;
using mdrop::ConfigFile;
using namespace mdrop;

// Each parameter is parenthesised. Without that, `a*255` binds tighter than
// whatever the caller wrote, so an argument like `1.0f - damp` expands to
// `(int)(1.0f - damp*255)` -- a silently wrong colour, not a compile error.
// That cost hours on 2026-08-27: the feedback damp's black quad came out with
// an alpha byte of 0x01 instead of 0xFF, so every blended draw was a no-op and
// the blame landed on the blend state, the PSO, the render target and the
// command list in turn before the packed DWORD was finally logged. Every
// existing call site passes a bare variable or a literal and is unaffected.
#define D3DCOLOR_RGBA_01(r,g,b,a)   D3DCOLOR_RGBA(((int)((r)*255)),((int)((g)*255)),((int)((b)*255)),((int)((a)*255)))
#define FRAND ((rand() % 7381)/7380.0f)

#define VERT_CLIP 0.75f		// warning: top/bottom can get clipped if you go < 0.65!

int g_title_font_sizes[] =
{
  // NOTE: DO NOT EXCEED 64 FONTS HERE.
6,  8,  10, 12, 14, 16,
20, 26, 32, 38, 44, 50, 56,
64, 72, 80, 88, 96, 104, 112, 120, 128, 136, 144,
160, 192, 224, 256, 288, 320, 352, 384, 416, 448,
480, 512	/**/
};

//#define COMPILE_MULTIMON_STUBS 1
//#include <multimon.h>

// This function evaluates whether the floating-point
// control Word is set to single precision/round to nearest/
// exceptions disabled. If not, the
// function changes the control Word to set them and returns
// TRUE, putting the old control Word value in the passback
// location pointed to by pwOldCW.
static void MungeFPCW(WORD* pwOldCW) {
#if 0
  BOOL ret = FALSE;
  WORD wTemp, wSave;

  __asm fstcw wSave
  if (wSave & 0x300 ||            // Not single mode
    0x3f != (wSave & 0x3f) ||   // Exceptions enabled
    wSave & 0xC00)              // Not round to nearest mode
  {
    __asm
    {
      mov ax, wSave
      and ax, not 300h;; single mode
      or ax, 3fh;; disable all exceptions
      and ax, not 0xC00;; round to nearest mode
      mov wTemp, ax
      fldcw   wTemp
    }
    ret = TRUE;
  }
  if (pwOldCW) *pwOldCW = wSave;
  //  return ret;
#else
#ifndef _WIN64
  _controlfp(_PC_24, _MCW_PC); // single precision (x86 only; no-op on x64)
#endif
  _controlfp(_RC_NEAR, _MCW_RC); // round to nearest mode
  _controlfp(_EM_ZERODIVIDE, _EM_ZERODIVIDE);  // disable divide-by-zero
#endif
}

void RestoreFPCW(WORD wSave) {
#ifndef _WIN64
  __asm fldcw wSave
#endif
}

int GetNumToSpawn(float fTime, float fDeltaT, float fRate, float fRegularity, int iNumSpawnedSoFar) {
  // PARAMETERS
  // ------------
  // fTime:          sum of all fDeltaT's so far (excluding this one)
  // fDeltaT:        time window for this frame
  // fRate:          avg. rate (spawns per second) of generation
  // fRegularity:    regularity of generation
//					0.0: totally chaotic
//					0.2: getting chaotic / very jittered
//					0.4: nicely jittered
//					0.6: slightly jittered
//					0.8: almost perfectly regular
//					1.0: perfectly regular
  // iNumSpawnedSoFar: the total number of spawnings so far
  //
  // RETURN VALUE
  // ------------
  // The number to spawn for this frame (add this to your net count!).
  //
// COMMENTS
// ------------
// The spawn values returned will, over time, match
// (within 1%) the theoretical totals expected based on the
// amount of time passed and the average generation rate.
//
// UNRESOLVED ISSUES
// -----------------
// actual results of mixed gen. (0 < reg < 1) are about 1% too low
  // in the long run (vs. analytical expectations).  Decided not
// to bother fixing it since it's only 1% (and VERY consistent).

  float fNumToSpawnReg;
  float fNumToSpawnIrreg;
  float fNumToSpawn;

  // compute # spawned based on regular generation
  fNumToSpawnReg = ((fTime + fDeltaT) * fRate) - iNumSpawnedSoFar;

  // compute # spawned based on irregular (random) generation
  if (fDeltaT <= 1.0f / fRate) {
    // case 1: avg. less than 1 spawn per frame
    if ((rand() % 16384) / 16384.0f < fDeltaT * fRate)
      fNumToSpawnIrreg = 1.0f;
    else
      fNumToSpawnIrreg = 0.0f;
  }
  else {
    // case 2: avg. more than 1 spawn per frame
    fNumToSpawnIrreg = fDeltaT * fRate;
    fNumToSpawnIrreg *= 2.0f * (rand() % 16384) / 16384.0f;
  }

  // get linear combo. of regular & irregular
  fNumToSpawn = fNumToSpawnReg * fRegularity + fNumToSpawnIrreg * (1.0f - fRegularity);

  // round to nearest integer for result
  return (int)(fNumToSpawn + 0.49f);
}

bool mdrop::Engine::RenderStringToTitleTexture(int supertextIndex)
{
  int texIndex = supertextIndex;

  if (!m_dx12Title[texIndex].IsValid())
    return false;

  if (m_supertexts[supertextIndex].szTextW[0] == 0)
    return false;

  if (!m_titleDC || !m_titleDIBBits || !m_dx12TitleUploadBuf[texIndex] || !m_lpDX)
    return false;

  wchar_t szTextToDraw[512];
  FormatTo(szTextToDraw, L" %s ", m_supertexts[supertextIndex].szTextW);

  UINT tw = (UINT)m_nTitleTexSizeX;
  UINT th = (UINT)m_nTitleTexSizeY;

  // Clear DIB to black
  memset(m_titleDIBBits, 0, (size_t)tw * th * 4);

  // Set text color to white on transparent background
  SetTextColor(m_titleDC, RGB(255, 255, 255));
  SetBkMode(m_titleDC, TRANSPARENT);

  RECT rect;
  rect.left = 0;
  rect.right = m_nTitleTexSizeX;
  rect.top = m_nTitleTexSizeY * 1 / 21;
  rect.bottom = m_nTitleTexSizeY * 17 / 21;

  bool ret = true;

  if (!m_supertexts[supertextIndex].bIsSongTitle) {
    // --- Font cache: avoid expensive binary search + CreateFontW when
    // the font face/style and text length are unchanged. ---
    static HFONT  s_cachedFont = nullptr;
    static wchar_t s_cachedFace[128] = {};
    static int    s_cachedBold = 0;
    static int    s_cachedItal = 0;
    static int    s_cachedTexW = 0;
    static int    s_cachedTextLen = 0;
    static int    s_cachedSizeIdx = -1; // index into g_title_font_sizes

    int textLen = (int)wcslen(szTextToDraw);
    bool cacheHit = (s_cachedFont != nullptr &&
                     s_cachedTexW == m_nTitleTexSizeX &&
                     s_cachedBold == m_supertexts[supertextIndex].bBold &&
                     s_cachedItal == m_supertexts[supertextIndex].bItal &&
                     wcscmp(s_cachedFace, m_supertexts[supertextIndex].nFontFace) == 0);

    int lo = 0;

    if (cacheHit && s_cachedTextLen > 0) {
      // Text length similar enough — verify cached size still fits with one measurement
      HGDIOBJ oldFont = SelectObject(m_titleDC, s_cachedFont);
      RECT temp = rect;
      int h = ::DrawTextW(m_titleDC, szTextToDraw, -1, &temp, DT_SINGLELINE | DT_CALCRECT | DT_CENTER);
      SelectObject(m_titleDC, oldFont);

      if (temp.right - temp.left < rect.right - rect.left && h <= rect.bottom - rect.top) {
        // Cached font still fits — skip binary search entirely
        lo = s_cachedSizeIdx;
      } else {
        // Text too long for cached size — invalidate and redo search
        cacheHit = false;
      }
    }

    if (!cacheHit) {
      // Full binary search for best font size
      int hi = sizeof(g_title_font_sizes) / sizeof(int) - 1;

      RECT temp = rect;
      while (lo < hi - 1) {
        int mid = (lo + hi) / 2;

        HFONT testFont = CreateFontW(g_title_font_sizes[mid], 0, 0, 0,
          m_supertexts[supertextIndex].bBold ? 900 : 400,
          m_supertexts[supertextIndex].bItal, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          m_fontinfo[SONGTITLE_FONT].bAntiAliased ? ANTIALIASED_QUALITY : DEFAULT_QUALITY,
          DEFAULT_PITCH, m_supertexts[supertextIndex].nFontFace);
        if (!testFont) { hi = mid; continue; }

        HGDIOBJ oldFont = SelectObject(m_titleDC, testFont);
        temp = rect;
        int h = ::DrawTextW(m_titleDC, szTextToDraw, -1, &temp, DT_SINGLELINE | DT_CALCRECT | DT_CENTER);

        if (temp.right - temp.left >= rect.right - rect.left || h > rect.bottom - rect.top)
          hi = mid;
        else
          lo = mid;

        SelectObject(m_titleDC, oldFont);
        DeleteObject(testFont);
      }

      // Update font cache: destroy old, create and cache new
      if (s_cachedFont) { DeleteObject(s_cachedFont); s_cachedFont = nullptr; }

      s_cachedFont = CreateFontW(g_title_font_sizes[lo], 0, 0, 0,
        m_supertexts[supertextIndex].bBold ? 900 : 400,
        m_supertexts[supertextIndex].bItal, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH, m_supertexts[supertextIndex].nFontFace);

      wcsncpy_s(s_cachedFace, m_supertexts[supertextIndex].nFontFace, _TRUNCATE);
      s_cachedBold = m_supertexts[supertextIndex].bBold;
      s_cachedItal = m_supertexts[supertextIndex].bItal;
      s_cachedTexW = m_nTitleTexSizeX;
      s_cachedTextLen = textLen;
      s_cachedSizeIdx = lo;
    }

    if (s_cachedFont) {
      HGDIOBJ oldFont = SelectObject(m_titleDC, s_cachedFont);

      int lineCount = 1;
      for (const wchar_t* p = szTextToDraw; *p; ++p) {
        if (*p == L'\n') ++lineCount;
      }

      // Measure text width for autosize calculation
      RECT temp = rect;
      int h = ::DrawTextW(m_titleDC, szTextToDraw, -1, &temp, DT_SINGLELINE | DT_CALCRECT | DT_CENTER);
      m_supertexts[supertextIndex].nTextWidthUsed = (int)(temp.right - temp.left);

      long offset = h / 2;
      if (lineCount > 1) offset *= lineCount;

      temp.left = 0;
      temp.right = m_nTitleTexSizeX;
      temp.top = m_nTitleTexSizeY / 2 - offset;
      temp.bottom = m_nTitleTexSizeY / 2 + offset;

      DWORD flags = (lineCount == 1) ? (DT_SINGLELINE | DT_CENTER) : (DT_WORDBREAK | DT_CENTER);
      m_supertexts[supertextIndex].nFontSizeUsed = ::DrawTextW(m_titleDC, szTextToDraw, -1, &temp, flags);

      // Global autosize: compute fFontSize so text fits on screen at maximum growth,
      // accounting for aspect ratio correction and off-center positioning.
      // The visible half-width on screen is: fSizeX * growth * textFillRatio / aspectScale
      // This must fit within: 1.0 - abs(dx), where dx = fX*2-1 (offset from center in clip space).
      if (m_bMessageAutoSize && !m_supertexts[supertextIndex].bExplicitSize
                              && m_supertexts[supertextIndex].nFontSizeUsed > 0
                              && m_supertexts[supertextIndex].nTextWidthUsed > 0) {
        float maxGrowth = max(1.0f, m_supertexts[supertextIndex].fGrowth);
        float aspectCorr = (float)m_nTexSizeX / ((float)m_nTexSizeY * 4.0f / 3.0f) * 1.4f;
        float aspectScale = (aspectCorr < 1.0f) ? aspectCorr : 1.0f;
        // Account for off-center positioning: reduce fill to prevent edge clipping
        float dx = fabsf(m_supertexts[supertextIndex].fX * 2.0f - 1.0f);
        float kFill = max(0.3f, min(0.88f, 1.0f - dx - 0.05f));  // 0.05 margin
        float textW = (float)m_supertexts[supertextIndex].nTextWidthUsed;
        float texW  = (float)m_nTitleTexSizeX;
        float fontH = (float)m_supertexts[supertextIndex].nFontSizeUsed;
        float screenScale = (float)m_nTexSizeX / 1024.0f * 100.0f;
        float ratio = kFill * aspectScale * texW * fontH / (screenScale * textW * maxGrowth);
        float computed = 50.0f + logf(ratio) / logf(1.033f);
        m_supertexts[supertextIndex].fFontSize = max(0.0f, min(100.0f, computed));
      }

      SelectObject(m_titleDC, oldFont);
      // Don't delete — font is cached for reuse
    } else {
      ret = false;
    }
  }
  else {
    // Song title: shrink font to fit, fall back to "..." truncation at smallest size
    wchar_t* str = m_supertexts[supertextIndex].szTextW;

    if (m_gdi_title_font_doublesize) {
      // First try the pre-created font at normal size
      HGDIOBJ oldFont = SelectObject(m_titleDC, m_gdi_title_font_doublesize);
      RECT temp = rect;
      int h = ::DrawTextW(m_titleDC, str, -1, &temp, DT_SINGLELINE | DT_CALCRECT);
      SelectObject(m_titleDC, oldFont);

      HFONT hShrunkFont = NULL;
      if (temp.right - temp.left > m_nTitleTexSizeX) {
        // Text too wide — binary search for a smaller font size
        int nominalSize = m_fontinfo[SONGTITLE_FONT].nSize * m_nTitleTexSizeX / 256;
        if (nominalSize < 6) nominalSize = 6;
        int lo = 6, hi = nominalSize;
        int bestSize = lo;

        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          HFONT testFont = CreateFontW(mid, 0, 0, 0,
            m_fontinfo[SONGTITLE_FONT].bBold ? 900 : 400,
            m_fontinfo[SONGTITLE_FONT].bItalic, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            m_fontinfo[SONGTITLE_FONT].bAntiAliased ? ANTIALIASED_QUALITY : DEFAULT_QUALITY,
            DEFAULT_PITCH, m_fontinfo[SONGTITLE_FONT].szFace);
          if (!testFont) { hi = mid - 1; continue; }

          HGDIOBJ prev = SelectObject(m_titleDC, testFont);
          temp = rect;
          ::DrawTextW(m_titleDC, str, -1, &temp, DT_SINGLELINE | DT_CALCRECT);
          SelectObject(m_titleDC, prev);

          if (temp.right - temp.left <= m_nTitleTexSizeX) {
            bestSize = mid;
            lo = mid + 1;
          } else {
            hi = mid - 1;
          }
          DeleteObject(testFont);
        }

        hShrunkFont = CreateFontW(bestSize, 0, 0, 0,
          m_fontinfo[SONGTITLE_FONT].bBold ? 900 : 400,
          m_fontinfo[SONGTITLE_FONT].bItalic, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          m_fontinfo[SONGTITLE_FONT].bAntiAliased ? ANTIALIASED_QUALITY : DEFAULT_QUALITY,
          DEFAULT_PITCH, m_fontinfo[SONGTITLE_FONT].szFace);
      }

      HFONT hUseFont = hShrunkFont ? hShrunkFont : m_gdi_title_font_doublesize;
      oldFont = SelectObject(m_titleDC, hUseFont);

      // Measure with the chosen font
      temp = rect;
      h = ::DrawTextW(m_titleDC, str, -1, &temp, DT_SINGLELINE | DT_CALCRECT);

      // Last resort: truncate with "..." if still too wide at smallest size
      if (temp.right - temp.left > m_nTitleTexSizeX) {
        int len = (int)wcslen(str);
        float fPercentToKeep = 0.91f * m_nTitleTexSizeX / (float)(temp.right - temp.left);
        if (len > 8)
          lstrcpynW(&str[(int)(len * fPercentToKeep)], L"...", 4);
        temp = rect;
        h = ::DrawTextW(m_titleDC, str, -1, &temp, DT_SINGLELINE | DT_CALCRECT);
      }

      temp.left = 0;
      temp.right = m_nTitleTexSizeX;
      temp.top = m_nTitleTexSizeY / 2 - h / 2;
      temp.bottom = m_nTitleTexSizeY / 2 + h / 2;

      m_supertexts[supertextIndex].nFontSizeUsed = ::DrawTextW(m_titleDC, str, -1, &temp, DT_SINGLELINE | DT_CENTER);

      SelectObject(m_titleDC, oldFont);
      if (hShrunkFont) DeleteObject(hShrunkFont);
    } else {
      ret = false;
    }
  }

  if (!ret) return false;

  // Upload DIB to DX12 title texture
  UINT rowPitch = (tw * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                  & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

  BYTE* uploadPtr = nullptr;
  m_dx12TitleUploadBuf[texIndex]->Map(0, nullptr, (void**)&uploadPtr);
  if (uploadPtr) {
    for (UINT y = 0; y < th; y++) {
      memcpy(uploadPtr + y * rowPitch, m_titleDIBBits + y * tw * 4, tw * 4);
    }
    m_dx12TitleUploadBuf[texIndex]->Unmap(0, nullptr);
  }

  auto* cmdList = m_lpDX->m_commandList.Get();

  m_lpDX->TransitionResource(m_dx12Title[texIndex], D3D12_RESOURCE_STATE_COPY_DEST);

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = m_dx12TitleUploadBuf[texIndex].Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset = 0;
  src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_B8G8R8A8_UNORM;
  src.PlacedFootprint.Footprint.Width    = tw;
  src.PlacedFootprint.Footprint.Height   = th;
  src.PlacedFootprint.Footprint.Depth    = 1;
  src.PlacedFootprint.Footprint.RowPitch = rowPitch;

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = m_dx12Title[texIndex].resource.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;

  cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  m_lpDX->TransitionResource(m_dx12Title[texIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  return true;
}

void mdrop::Engine::LoadPerFrameEvallibVars(CState* pState, const RenderContext* rc) {
  // load the 'var_pf_*' variables in this CState object with the correct values.
  // for vars that affect pixel motion, that means evaluating them at time==-1,
  //    (i.e. no blending w/blendto value); the blending of the file dx/dy
  //    will be done *after* execution of the per-vertex code.
  // for vars that do NOT affect pixel motion, evaluate them at the current time,
  //    so that if they're blending, both states see the blended value.

  // 1. vars that affect pixel motion: (eval at time==-1)
  *pState->var_pf_zoom = (double)pState->m_fZoom.eval(-1);//GetTime());
  *pState->var_pf_zoomexp = (double)pState->m_fZoomExponent.eval(-1);//GetTime());
  *pState->var_pf_rot = (double)pState->m_fRot.eval(-1);//GetTime());
  *pState->var_pf_warp = (double)pState->m_fWarpAmount.eval(-1);//GetTime());
  *pState->var_pf_cx = (double)pState->m_fRotCX.eval(-1);//GetTime());
  *pState->var_pf_cy = (double)pState->m_fRotCY.eval(-1);//GetTime());
  *pState->var_pf_dx = (double)pState->m_fXPush.eval(-1);//GetTime());
  *pState->var_pf_dy = (double)pState->m_fYPush.eval(-1);//GetTime());
  *pState->var_pf_sx = (double)pState->m_fStretchX.eval(-1);//GetTime());
  *pState->var_pf_sy = (double)pState->m_fStretchY.eval(-1);//GetTime());
  // read-only:
  *pState->var_pf_time = (double)(GetTime() - m_fStartTime);
  *pState->var_pf_fps = (double)GetFps();

  *pState->var_pf_bass = (double)mysound.imm_rel[0];
  *pState->var_pf_mid = (double)mysound.imm_rel[1];
  *pState->var_pf_treb = (double)mysound.imm_rel[2];

  *pState->var_pf_bass_att = (double)mysound.avg_rel[0];
  *pState->var_pf_mid_att = (double)mysound.avg_rel[1];
  *pState->var_pf_treb_att = (double)mysound.avg_rel[2];

  *pState->var_pf_bass_smooth = (double)mysound.smooth[0];
  *pState->var_pf_mid_smooth = (double)mysound.smooth[1];
  *pState->var_pf_treb_smooth = (double)mysound.smooth[2];

  *pState->var_pf_frame = (double)FrameOf(rc);
  //*pState->var_pf_monitor     = 0;   -leave this as it was set in the per-frame INIT code!
  for (int vi = 0; vi < NUM_Q_VAR; vi++)
    *pState->var_pf_q[vi] = pState->q_values_after_init_code[vi];//0.0f;
  *pState->var_pf_monitor = pState->monitor_after_init_code;
  *pState->var_pf_progress = (GetTime() - m_fPresetStartTime) / (m_fNextPresetTime - m_fPresetStartTime);

  // 2. vars that do NOT affect pixel motion: (eval at time==now)
  *pState->var_pf_decay = (double)pState->m_fDecay.eval(GetTime());
  *pState->var_pf_wave_a = (double)pState->m_fWaveAlpha.eval(GetTime());
  *pState->var_pf_wave_r = (double)pState->m_fWaveR.eval(GetTime());
  *pState->var_pf_wave_g = (double)pState->m_fWaveG.eval(GetTime());
  *pState->var_pf_wave_b = (double)pState->m_fWaveB.eval(GetTime());
  *pState->var_pf_wave_x = (double)pState->m_fWaveX.eval(GetTime());
  *pState->var_pf_wave_y = (double)pState->m_fWaveY.eval(GetTime());
  *pState->var_pf_wave_mystery = (double)pState->m_fWaveParam.eval(GetTime());
  *pState->var_pf_wave_mode = (double)pState->m_nWaveMode;	//?!?! -why won't it work if set to pState->m_nWaveMode???
  *pState->var_pf_ob_size = (double)pState->m_fOuterBorderSize.eval(GetTime());
  *pState->var_pf_ob_r = (double)pState->m_fOuterBorderR.eval(GetTime());
  *pState->var_pf_ob_g = (double)pState->m_fOuterBorderG.eval(GetTime());
  *pState->var_pf_ob_b = (double)pState->m_fOuterBorderB.eval(GetTime());
  *pState->var_pf_ob_a = (double)pState->m_fOuterBorderA.eval(GetTime());
  *pState->var_pf_ib_size = (double)pState->m_fInnerBorderSize.eval(GetTime());
  *pState->var_pf_ib_r = (double)pState->m_fInnerBorderR.eval(GetTime());
  *pState->var_pf_ib_g = (double)pState->m_fInnerBorderG.eval(GetTime());
  *pState->var_pf_ib_b = (double)pState->m_fInnerBorderB.eval(GetTime());
  *pState->var_pf_ib_a = (double)pState->m_fInnerBorderA.eval(GetTime());
  *pState->var_pf_mv_x = (double)pState->m_fMvX.eval(GetTime());
  *pState->var_pf_mv_y = (double)pState->m_fMvY.eval(GetTime());
  *pState->var_pf_mv_dx = (double)pState->m_fMvDX.eval(GetTime());
  *pState->var_pf_mv_dy = (double)pState->m_fMvDY.eval(GetTime());
  *pState->var_pf_mv_l = (double)pState->m_fMvL.eval(GetTime());
  *pState->var_pf_mv_r = (double)pState->m_fMvR.eval(GetTime());
  *pState->var_pf_mv_g = (double)pState->m_fMvG.eval(GetTime());
  *pState->var_pf_mv_b = (double)pState->m_fMvB.eval(GetTime());
  *pState->var_pf_mv_a = (double)pState->m_fMvA.eval(GetTime());
  *pState->var_pf_echo_zoom = (double)pState->m_fVideoEchoZoom.eval(GetTime());
  *pState->var_pf_echo_alpha = (double)pState->m_fVideoEchoAlpha.eval(GetTime());
  *pState->var_pf_echo_orient = (double)pState->m_nVideoEchoOrientation;
  // new in v1.04:
  *pState->var_pf_wave_usedots = (double)pState->m_bWaveDots;
  *pState->var_pf_wave_thick = (double)pState->m_bWaveThick;
  *pState->var_pf_wave_additive = (double)pState->m_bAdditiveWaves;
  *pState->var_pf_wave_brighten = (double)pState->m_bMaximizeWaveColor;
  *pState->var_pf_darken_center = (double)pState->m_bDarkenCenter;
  *pState->var_pf_gamma = (double)pState->m_fGammaAdj.eval(GetTime());
  *pState->var_pf_wrap = (double)pState->m_bTexWrap;
  *pState->var_pf_invert = (double)pState->m_bInvert;
  *pState->var_pf_brighten = (double)pState->m_bBrighten;
  *pState->var_pf_darken = (double)pState->m_bDarken;
  *pState->var_pf_solarize = (double)pState->m_bSolarize;
  *pState->var_pf_meshx = (double)m_nGridX;
  *pState->var_pf_meshy = (double)m_nGridY;
  *pState->var_pf_pixelsx = (double)GetWidth();
  *pState->var_pf_pixelsy = (double)GetHeight();

  if (m_bScreenDependentRenderMode) {
    *pState->var_pf_aspectx = 1;
    *pState->var_pf_aspecty = 1;
  }
  else {
    *pState->var_pf_aspectx = (double)InvAspectXOf(rc);
    *pState->var_pf_aspecty = (double)InvAspectYOf(rc);
  }

  // new in v2.0:
  *pState->var_pf_blur1min = (double)pState->m_fBlur1Min.eval(GetTime());
  *pState->var_pf_blur2min = (double)pState->m_fBlur2Min.eval(GetTime());
  *pState->var_pf_blur3min = (double)pState->m_fBlur3Min.eval(GetTime());
  *pState->var_pf_blur1max = (double)pState->m_fBlur1Max.eval(GetTime());
  *pState->var_pf_blur2max = (double)pState->m_fBlur2Max.eval(GetTime());
  *pState->var_pf_blur3max = (double)pState->m_fBlur3Max.eval(GetTime());
  *pState->var_pf_blur1_edge_darken = (double)pState->m_fBlur1EdgeDarken.eval(GetTime());

  // BMV/MDropDX12
  *pState->var_pf_mousex = (double)m_mouseX;
  *pState->var_pf_mousey = (double)m_mouseY;
  *pState->var_pf_mousedown = m_mouseDown ? 1.0 : 0.0;
  *pState->var_pf_mouseclick = m_mouseClicked > 0 ? 1.0 : 0.0;
}

void mdrop::Engine::RunPerFrameEquations(int code) {
  // run per-frame calculations

    /*
      code is only valid when blending.
          OLDcomp ~ blend-from preset has a composite shader;
          NEWwarp ~ blend-to preset has a warp shader; etc.

      code OLDcomp NEWcomp OLDwarp NEWwarp
        0
        1            1
        2                            1
        3            1               1
        4     1
        5     1      1
        6     1                      1
        7     1      1               1
        8                    1
        9            1       1
        10                   1       1
        11           1       1       1
        12    1              1
        13    1      1       1
        14    1              1       1
        15    1      1       1       1
    */

    // when blending booleans (like darken, invert, etc) for pre-shader presets,
    // if blending to/from a pixel-shader preset, we can tune the snap point
    // (when it changes during the blend) for a less jumpy transition:
  m_fSnapPoint = 0.5f;
  if (m_pState->m_bBlending) {
    switch (code) {
    case 4:
    case 6:
    case 12:
    case 14:
      // old preset (only) had a comp shader
      m_fSnapPoint = -0.01f;
      break;
    case 1:
    case 3:
    case 9:
    case 11:
      // new preset (only) has a comp shader
      m_fSnapPoint = 1.01f;
      break;
    case 0:
    case 2:
    case 8:
    case 10:
      // neither old or new preset had a comp shader
      m_fSnapPoint = 0.5f;
      break;
    case 5:
    case 7:
    case 13:
    case 15:
      // both old and new presets use a comp shader - so it won't matter
      m_fSnapPoint = 0.5f;
      break;
    }
  }

  int num_reps = (m_pState->m_bBlending) ? 2 : 1;
  for (int rep = 0; rep < num_reps; rep++) {
    CState* pState;

    if (rep == 0)
      pState = m_pState;
    else
      pState = m_pOldState;

    // values that will affect the pixel motion (and will be automatically blended
    //	LATER, when the results of 2 sets of these params creates 2 different U/V
    //  meshes that get blended together.)
    LoadPerFrameEvallibVars(pState);

    // also do just a once-per-frame init for the *per-**VERTEX*** *READ-ONLY* variables
    // (the non-read-only ones will be reset/restored at the start of each vertex)
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
    *pState->var_pv_pixelsx = (double)GetWidth();
    *pState->var_pv_pixelsy = (double)GetHeight();
    *pState->var_pv_aspectx = (double)m_fInvAspectX;
    *pState->var_pv_aspecty = (double)m_fInvAspectY;

    if (m_bScreenDependentRenderMode) {
      *pState->var_pv_aspectx = 1;
      *pState->var_pv_aspecty = 1;
    }
    else {
      *pState->var_pv_aspectx = (double)m_fInvAspectX;
      *pState->var_pv_aspecty = (double)m_fInvAspectY;
    }
    //*pState->var_pv_monitor     = *pState->var_pf_monitor;

// execute once-per-frame expressions:
#ifndef _NO_EXPR_
    if (pState->m_pf_codehandle) {
        NSEEL_code_execute(pState->m_pf_codehandle);
    }
#endif

    // save some things for next frame:
    pState->monitor_after_init_code = *pState->var_pf_monitor;

    // save some things for per-vertex code:
    for (int vi = 0; vi < NUM_Q_VAR; vi++)
      *pState->var_pv_q[vi] = *pState->var_pf_q[vi];

    // (a few range checks:)
    *pState->var_pf_gamma = max(0, min(8, *pState->var_pf_gamma));
    *pState->var_pf_echo_zoom = max(0.001, min(1000, *pState->var_pf_echo_zoom));

    /*
        if (m_pState->m_bRedBlueStereo || m_bAlways3D)
    {
      // override wave colors
      *pState->var_pf_wave_r = 0.35f*(*pState->var_pf_wave_r) + 0.65f;
      *pState->var_pf_wave_g = 0.35f*(*pState->var_pf_wave_g) + 0.65f;
      *pState->var_pf_wave_b = 0.35f*(*pState->var_pf_wave_b) + 0.65f;
    }
        */
  }

  if (m_pState->m_bBlending) {
    // For all variables that do NOT affect pixel motion, blend them NOW,
        // so later the user can just access m_pState->m_pf_whatever.
    double mix = (double)CosineInterp(m_pState->m_fBlendProgress);
    double mix2 = 1.0 - mix;
    *m_pState->var_pf_decay = mix * (*m_pState->var_pf_decay) + mix2 * (*m_pOldState->var_pf_decay);
    *m_pState->var_pf_wave_a = mix * (*m_pState->var_pf_wave_a) + mix2 * (*m_pOldState->var_pf_wave_a);
    *m_pState->var_pf_wave_r = mix * (*m_pState->var_pf_wave_r) + mix2 * (*m_pOldState->var_pf_wave_r);
    *m_pState->var_pf_wave_g = mix * (*m_pState->var_pf_wave_g) + mix2 * (*m_pOldState->var_pf_wave_g);
    *m_pState->var_pf_wave_b = mix * (*m_pState->var_pf_wave_b) + mix2 * (*m_pOldState->var_pf_wave_b);
    *m_pState->var_pf_wave_x = mix * (*m_pState->var_pf_wave_x) + mix2 * (*m_pOldState->var_pf_wave_x);
    *m_pState->var_pf_wave_y = mix * (*m_pState->var_pf_wave_y) + mix2 * (*m_pOldState->var_pf_wave_y);
    *m_pState->var_pf_wave_mystery = mix * (*m_pState->var_pf_wave_mystery) + mix2 * (*m_pOldState->var_pf_wave_mystery);
    // wave_mode: exempt (integer)
    // Border parameters are NOT blended for a frozen .milk2.
    //
    // Interpolating them can synthesise a border neither preset has. Mandala2
    // is the case in point: PRESET1 is invisible because ob_size = 0 while
    // ob_a = 1 and the colour is magenta; PRESET2 is invisible because
    // ob_a = 0. Blending at the file's frozen progress gives size ~0.023 with
    // alpha ~0.78 in magenta -- a bright pink frame around the whole screen,
    // built out of two invisible borders. MD3 PRO draws no border here.
    //
    // A transition may still interpolate them: it is on screen for two seconds
    // and ends at a clean endpoint, whereas a .milk2 is permanent, so any
    // artefact is permanent too.
    const bool bBlendBorders = !m_bMilk2FrozenBlend;
    if (bBlendBorders) {
    *m_pState->var_pf_ob_size = mix * (*m_pState->var_pf_ob_size) + mix2 * (*m_pOldState->var_pf_ob_size);
    *m_pState->var_pf_ob_r = mix * (*m_pState->var_pf_ob_r) + mix2 * (*m_pOldState->var_pf_ob_r);
    *m_pState->var_pf_ob_g = mix * (*m_pState->var_pf_ob_g) + mix2 * (*m_pOldState->var_pf_ob_g);
    *m_pState->var_pf_ob_b = mix * (*m_pState->var_pf_ob_b) + mix2 * (*m_pOldState->var_pf_ob_b);
    *m_pState->var_pf_ob_a = mix * (*m_pState->var_pf_ob_a) + mix2 * (*m_pOldState->var_pf_ob_a);
    *m_pState->var_pf_ib_size = mix * (*m_pState->var_pf_ib_size) + mix2 * (*m_pOldState->var_pf_ib_size);
    *m_pState->var_pf_ib_r = mix * (*m_pState->var_pf_ib_r) + mix2 * (*m_pOldState->var_pf_ib_r);
    *m_pState->var_pf_ib_g = mix * (*m_pState->var_pf_ib_g) + mix2 * (*m_pOldState->var_pf_ib_g);
    *m_pState->var_pf_ib_b = mix * (*m_pState->var_pf_ib_b) + mix2 * (*m_pOldState->var_pf_ib_b);
    *m_pState->var_pf_ib_a = mix * (*m_pState->var_pf_ib_a) + mix2 * (*m_pOldState->var_pf_ib_a);
    }
    *m_pState->var_pf_mv_x = mix * (*m_pState->var_pf_mv_x) + mix2 * (*m_pOldState->var_pf_mv_x);
    *m_pState->var_pf_mv_y = mix * (*m_pState->var_pf_mv_y) + mix2 * (*m_pOldState->var_pf_mv_y);
    *m_pState->var_pf_mv_dx = mix * (*m_pState->var_pf_mv_dx) + mix2 * (*m_pOldState->var_pf_mv_dx);
    *m_pState->var_pf_mv_dy = mix * (*m_pState->var_pf_mv_dy) + mix2 * (*m_pOldState->var_pf_mv_dy);
    *m_pState->var_pf_mv_l = mix * (*m_pState->var_pf_mv_l) + mix2 * (*m_pOldState->var_pf_mv_l);
    *m_pState->var_pf_mv_r = mix * (*m_pState->var_pf_mv_r) + mix2 * (*m_pOldState->var_pf_mv_r);
    *m_pState->var_pf_mv_g = mix * (*m_pState->var_pf_mv_g) + mix2 * (*m_pOldState->var_pf_mv_g);
    *m_pState->var_pf_mv_b = mix * (*m_pState->var_pf_mv_b) + mix2 * (*m_pOldState->var_pf_mv_b);
    *m_pState->var_pf_mv_a = mix * (*m_pState->var_pf_mv_a) + mix2 * (*m_pOldState->var_pf_mv_a);
    *m_pState->var_pf_echo_zoom = mix * (*m_pState->var_pf_echo_zoom) + mix2 * (*m_pOldState->var_pf_echo_zoom);
    *m_pState->var_pf_echo_alpha = mix * (*m_pState->var_pf_echo_alpha) + mix2 * (*m_pOldState->var_pf_echo_alpha);
    *m_pState->var_pf_echo_orient = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_echo_orient : *m_pState->var_pf_echo_orient;
    // added in v1.04:
    *m_pState->var_pf_wave_usedots = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_wave_usedots : *m_pState->var_pf_wave_usedots;
    *m_pState->var_pf_wave_thick = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_wave_thick : *m_pState->var_pf_wave_thick;
    *m_pState->var_pf_wave_additive = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_wave_additive : *m_pState->var_pf_wave_additive;
    *m_pState->var_pf_wave_brighten = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_wave_brighten : *m_pState->var_pf_wave_brighten;
    *m_pState->var_pf_darken_center = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_darken_center : *m_pState->var_pf_darken_center;
    *m_pState->var_pf_gamma = mix * (*m_pState->var_pf_gamma) + mix2 * (*m_pOldState->var_pf_gamma);
    *m_pState->var_pf_wrap = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_wrap : *m_pState->var_pf_wrap;
    *m_pState->var_pf_invert = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_invert : *m_pState->var_pf_invert;
    *m_pState->var_pf_brighten = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_brighten : *m_pState->var_pf_brighten;
    *m_pState->var_pf_darken = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_darken : *m_pState->var_pf_darken;
    *m_pState->var_pf_solarize = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_solarize : *m_pState->var_pf_solarize;
    // added in v2.0:
    *m_pState->var_pf_blur1min = mix * (*m_pState->var_pf_blur1min) + mix2 * (*m_pOldState->var_pf_blur1min);
    *m_pState->var_pf_blur2min = mix * (*m_pState->var_pf_blur2min) + mix2 * (*m_pOldState->var_pf_blur2min);
    *m_pState->var_pf_blur3min = mix * (*m_pState->var_pf_blur3min) + mix2 * (*m_pOldState->var_pf_blur3min);
    *m_pState->var_pf_blur1max = mix * (*m_pState->var_pf_blur1max) + mix2 * (*m_pOldState->var_pf_blur1max);
    *m_pState->var_pf_blur2max = mix * (*m_pState->var_pf_blur2max) + mix2 * (*m_pOldState->var_pf_blur2max);
    *m_pState->var_pf_blur3max = mix * (*m_pState->var_pf_blur3max) + mix2 * (*m_pOldState->var_pf_blur3max);
    *m_pState->var_pf_blur1_edge_darken = mix * (*m_pState->var_pf_blur1_edge_darken) + mix2 * (*m_pOldState->var_pf_blur1_edge_darken);

    // BMV/MDropDX12 mouse variables
    *m_pState->var_pf_mousex = mix * (*m_pState->var_pf_mousex) + mix2 * (*m_pOldState->var_pf_mousex);
    *m_pState->var_pf_mousey = mix * (*m_pState->var_pf_mousey) + mix2 * (*m_pOldState->var_pf_mousey);
    *m_pState->var_pf_mousedown = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_mousedown : *m_pState->var_pf_mousedown;
    *m_pState->var_pf_mouseclick = (mix < m_fSnapPoint) ? *m_pOldState->var_pf_mouseclick : *m_pState->var_pf_mouseclick;
  }
}

void mdrop::Engine::RenderFrame(int bRedraw) {

  // Black mode: return without drawing. The early return IS the implementation
  // under DX12 -- nothing renders, so the canvas is left black. The DX9 body
  // that used to Clear and Present here is gone (issue 17).
  if (m_blackmode)
    return;

  // Startup loading screen (#291): nothing is drawn until the first preset has
  // a comp shader to draw with.
  //
  // Same mechanism as black mode above -- the early return IS the blank canvas.
  // Without it the warp pass fell through to PSO_TEXTURED_MYVERTEX and drew the
  // bare warp mesh, so startup showed a screenful of triangles while the first
  // preset compiled. Shane, 2026-09-14: "instead of showing a 'broken' render
  // of triangles while loading can you just have a blank screen with an overlay
  // of seconds centered in a large font" -- "it was on the primary render", "it
  // was only for a second", "it will be worse for other people I think".
  //
  // The test is the comp BYTECODE, not the PSOs. PSOs are built at the top of
  // DX12_RenderWarpAndComposite, which this return skips -- so gating on
  // m_bDX12PSOsDirty would wait for something that can only happen once the
  // gate opens, and the loading screen would never end.
  //
  // MyRenderUI draws the counter over the black; the shell calls it after this
  // returns, exactly as it does for black mode.
  if (!m_bFirstPresetEverReady) {
    if (m_shaders.comp.bytecodeBlob != nullptr) {
      m_bFirstPresetEverReady = true;
    } else {
      if (!m_dwLoadingScreenStart)
        m_dwLoadingScreenStart = GetTickCount();
      return;
    }
  }
  {

    float fDeltaT = 1.0f / GetFps();

    // The DX9 "pre-un-flip" for a redraw stood here (issue 17). It swapped
    // m_lpVS[0]/[1] so that redoing a frame repeated last frame's work rather
    // than compounding it. There is no DX12 equivalent and this is NOT one
    // waiting to be written: the swap of m_dx12VS below is post-render and
    // unconditional, so a bRedraw swap added here would double-swap.

    if (GetFrame() == 0) {
      m_fStartTime = GetTime();
      m_fPresetStartTime = GetTime();
      m_bPresetDiagLogged = false;
    }

    // A schedule further out than the longest interval that could have produced
    // it means the clock moved backwards (OnAnimationTimeRebased covers the
    // resets we know about; this catches the ones we don't). Left alone it never
    // arrives, and automatic preset changes stop for the rest of the session.
    if (m_fNextPresetTime > 0 && m_fTimeBetweenPresets > 0 &&
        m_fNextPresetTime - GetTime() >
          m_fBlendTimeAuto + m_fTimeBetweenPresets + m_fTimeBetweenPresetsRand + 1.0f)
      m_fNextPresetTime = -1.0f;   // fall into the reschedule just below

    if (m_fNextPresetTime < 0 && m_fTimeBetweenPresets > 0) {
      float dt = m_fTimeBetweenPresetsRand * (rand() % 1000) * 0.001f;
      m_fNextPresetTime = GetTime() + m_fBlendTimeAuto + m_fTimeBetweenPresets + dt;
    }

    if (!bRedraw) {
      m_rand_frame = D3DXVECTOR4(FRAND, FRAND, FRAND, FRAND);

      // randomly change the preset, if it's time (disabled when m_fTimeBetweenPresets == 0)
      if (m_fTimeBetweenPresets > 0 && m_fNextPresetTime < GetTime() &&
          AutoPresetChangesAllowed()) {
        if (m_nLoadingPreset == 0) { // don't start a load if one is already underway!
          // Tag this load as auto-initiated so LoadPresetTick can discard it
          // if testing mode / preset lock arrives while it is still compiling.
          m_bNextLoadIsAuto = true;
          m_pszNextLoadReason = "timer";
          LoadRandomPreset(m_fBlendTimeAuto);
        }
      }

      // Count this preset as played once it has been up long enough.
      TickPresetUsage();

      // Deferred startup preset save — persist to INI after 5s of uninterrupted render
      if (m_fPendingStartupSaveTime > 0 && GetTime() - m_fPendingStartupSaveTime >= 5.0f) {
        std::wstring savePath(m_szPendingStartupSave);
        std::wstring saveIni(GetConfigIniFile());
        m_fPendingStartupSaveTime = 0;
        m_szPendingStartupSave[0] = 0;
        // Fire-and-forget background write to avoid blocking the render thread
        std::thread([savePath, saveIni]() {
          ConfigFile(saveIni.c_str()).SetString(L"Settings", L"szPresetStartup", savePath.c_str());
        }).detach();
      }

      // Restrained(kMessages): both spawns below are timers, and a timer that
      // puts text on screen is exactly what testing mode is for. The random
      // song title in particular made even a deliberate ACTION=SongTitle
      // capture unreliable, because another one could appear between the
      // request and the screenshot.
      //
      // Through the restraints table rather than m_bTestingMode directly,
      // because a child holds testing mode permanently and still has to show
      // the user's messages -- it renders a real display for a real viewer.
      // See kChildVetoes in engine.h.
      if (MessagesEnabled() && !Restrained(restraint::kMessages)) {
        for (int i = 0; i < NUM_SUPERTEXTS; i++) {
          // randomly spawn Song Title, if time
          if (m_fTimeBetweenRandomSongTitles > 0 &&
            !m_supertexts[i].bRedrawSuperText &&
            GetTime() >= m_supertexts[i].fStartTime + m_supertexts[i].fDuration + 1.0f / GetFps()) {
            int n = GetNumToSpawn(GetTime(), fDeltaT, 1.0f / m_fTimeBetweenRandomSongTitles, 0.5f, m_nSongTitlesSpawned);
            if (n > 0) {
              LaunchSongTitleAnim(i);
              m_nSongTitlesSpawned += n;
            }
          }

          // Legacy random spawn Custom Message (when autoplay off)
          if (!m_bMsgAutoplay && m_fTimeBetweenRandomCustomMsgs > 0 &&
            !m_supertexts[i].bRedrawSuperText &&
            GetTime() >= m_supertexts[i].fStartTime + m_supertexts[i].fDuration + 1.0f / GetFps()) {
            int n = GetNumToSpawn(GetTime(), fDeltaT, 1.0f / m_fTimeBetweenRandomCustomMsgs, 0.5f, m_nCustMsgsSpawned);
            if (n > 0) {
              LaunchCustomMessage(-1);
              m_nCustMsgsSpawned += n;
            }
          }
        }
      }

      // Autoplay custom messages (managed via Messages tab)
      // Moved outside per-slot loop to support concurrent messages via m_nMsgMaxOnScreen
      //
      // Restrained(kMessages): this is a timer changing the frame without being
      // asked,
      // which is the whole category testing mode exists to freeze (engine.h --
      // "the timed advance, the audio hard cuts, the preset change on song
      // change, the idle timer"). Autoplay was simply missed, and it is not a
      // cosmetic omission: a capture harness photographs whatever message
      // happened to be on screen, and two runs of the same test then differ by
      // a rotating string. That cost an A/B comparison on 2026-08-28, where a
      // before/after pair was read as a song-title difference and was actually
      // two different autoplay messages.
      //
      // An explicit MSG= over IPC still works. The gate blocks what the app
      // decides on its own, never what the harness asks for.
      // Same backstop as the preset schedule above: a next-message time further
      // out than one interval plus its jitter cannot have been scheduled from
      // this clock, so the clock jumped back. Re-arm instead of waiting out the
      // whole gap in silence -- that silence was the reported bug.
      // The PRIMARY's turn of the loop. Identical logic to what stood here
      // inline; it moved into TickMessageAutoplay so a display running its own
      // preset can take a turn of its own, with its own clock and its own
      // maxOnScreen, off the same store (#255). The per-display turns are
      // driven from ComputeMirrorLayout, beside their preset clock.
      //
      // The primary keeps its persisted fields as its clock, so nothing about
      // the Messages tab or settings.ini changes shape.
      {
        double nextAt = m_fNextAutoMsgTime;
        TickMessageAutoplay(-1, nextAt, m_nNextSequentialMsg,
                            m_nMsgMaxOnScreen);
        m_fNextAutoMsgTime = (float)nextAt;
      }

      // update m_fBlendProgress;
      if (m_pState->m_bBlending) {
        if (m_bMilk2FrozenBlend) {
          // .milk2: blend stays frozen at the progress value from the file metadata
          m_pState->m_fBlendProgress = m_fMilk2FrozenProgress;
        } else {
          m_pState->m_fBlendProgress = (GetTime() - m_pState->m_fBlendStartTime) / m_pState->m_fBlendDuration;
          if (m_pState->m_fBlendProgress > 1.0f) {
            m_pState->m_bBlending = false;
            // Release blend-only PSOs (no longer needed after blend completes).
            //
            // RETIRE, do not Reset (#214). This runs inside the frame update,
            // and a mirror worker's submitted command list can still name
            // these PSOs -- a PSO must outlive every list that references it.
            // Dropping the last reference here is the same use-after-free
            // shape as #222 and #225, and it fires on every preset transition
            // rather than in some corner.
            //
            // A WaitForGpu would also be correct and is what the preset commit
            // does, but this site is far more frequent and a stall per blend
            // is not worth paying when holding four references for a frame
            // costs nothing.
            if (m_lpDX) {
              m_lpDX->RetireAndReset(m_dx12OldWarpPSO);
              m_lpDX->RetireAndReset(m_dx12WarpBlendPSO);
              m_lpDX->RetireAndReset(m_dx12OldCompPSO);
              m_lpDX->RetireAndReset(m_dx12CompBlendPSO);
            } else {
              m_dx12OldWarpPSO.Reset();
              m_dx12WarpBlendPSO.Reset();
              m_dx12OldCompPSO.Reset();
              m_dx12CompBlendPSO.Reset();
            }
          }
        }
      }

      // handle hard cuts here (just after new sound analysis)
      //
      // The member, not a function-local static. It was a static shadowing
      // Engine::m_fHardCutThresh (engine.h), which meant the two places that
      // raise the threshold after a MANUAL preset change --
      // engine_hotkeys.cpp:536 and engine_input.cpp:2263 -- were writing a
      // member nobody read. The suppression they exist to provide never
      // happened: change preset by hand and a loud beat could hard-cut again
      // immediately, stacking on the cut you just made. Found by C4458 once
      // shadowing warnings were turned on.
      if (GetFrame() == 0)
        m_fHardCutThresh = m_fHardCutLoudnessThresh * 2.0f;
      // AutoPresetChangesAllowed, not the two locks alone: this was the ninth
      // copy-pasted lock check, missed when the other eight became the helper,
      // so beat-driven hard cuts sailed straight through TESTING_MODE -- with
      // music playing, "freeze automatic preset changes" changed presets on
      // every loud beat. Found live during an A/B eyeball session.
      if (GetFps() > 1.0f && !m_bHardCutsDisabled && AutoPresetChangesAllowed()) {
        if (mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2] > m_fHardCutThresh * 3.0f) {
          if (m_nLoadingPreset == 0) { // don't start a load if one is already underway!
            m_bNextLoadIsAuto = true;  // discardable if a freeze lands mid-compile
            m_pszNextLoadReason = "hardcut-loudness";
            LoadRandomPreset(0.0f);
          }
          m_fHardCutThresh *= 2.0f;
        }
        else {
          /*
          float halflife_modified = m_fHardCutHalflife*0.5f;
          //thresh = (thresh - 1.5f)*0.99f + 1.5f;
          float k = -0.69315f / halflife_modified;*/
          float k = -1.3863f / (m_fHardCutHalflife * GetFps());
          //float single_frame_multiplier = powf(2.7183f, k / GetFps());
          float single_frame_multiplier = expf(k);
          m_fHardCutThresh = (m_fHardCutThresh - m_fHardCutLoudnessThresh) * single_frame_multiplier + m_fHardCutLoudnessThresh;
        }
      }

      // smooth & scale the audio data, according to m_state, for display purposes
      // pcmGain rides alongside the preset's own fWaveScale: the preset says
    // how big its wave should be, the profile says how loud the engine's
    // PCM runs.
    float scale = m_pState->m_fWaveScale.eval(GetTime())
                  * m_audioProfile.pcmGain / 128.0f;
      mysound.fWave[0][0] *= scale;
      mysound.fWave[1][0] *= scale;
      float mix2 = m_pState->m_fWaveSmoothing.eval(GetTime());
      float mix1 = scale * (1.0f - mix2);
      for (int i = 1; i < 576; i++) {
        mysound.fWave[0][i] = mysound.fWave[0][i] * mix1 + mysound.fWave[0][i - 1] * mix2;
        mysound.fWave[1][i] = mysound.fWave[1][i] * mix1 + mysound.fWave[1][i - 1] * mix2;
      }
    }

    bool bOldPresetUsesWarpShader = (m_pOldState->m_nWarpPSVersion > 0);
    bool bNewPresetUsesWarpShader = (m_pState->m_nWarpPSVersion > 0);
    bool bOldPresetUsesCompShader = (m_pOldState->m_nCompPSVersion > 0);
    bool bNewPresetUsesCompShader = (m_pState->m_nCompPSVersion > 0);

    // note: 'code' is only meaningful if we are BLENDING.
    int code = (bOldPresetUsesWarpShader ? 8 : 0) |
      (bOldPresetUsesCompShader ? 4 : 0) |
      (bNewPresetUsesWarpShader ? 2 : 0) |
      (bNewPresetUsesCompShader ? 1 : 0);

    RunPerFrameEquations(code);

    // Update audio texture (FFT + waveform/peak) for sampler_audio / get_fft() access
    UpdateAudioTexture();

    // Publish this frame's analysis for mirror sim threads (post-analysis,
    // post-smoothing — the exact values the primary's EEL feeds just read).
    PublishAudioSnapshot();

    // Per-vertex warp computation (CPU-only, no device dependency).
    // Moved here from inside the DX9 rendering block so DX12 path can use the results.
    //
    // Skipped for .milk3. RenderFrameShadertoy() draws fullscreen passes, and the
    // early return in DX12_RenderWarpAndComposite() means nothing downstream ever
    // reads m_verts[] on that path. Measured 2026-08-20 at nMeshSize=192: this loop
    // was 0.79 ms of a 1.19 ms sb_tunnel.milk3 frame -- the majority of the frame
    // spent building a warp mesh that was never sampled.
    // RenderClassicOrientPipeline() calls this itself, so the mirror path is
    // unaffected by the gate.
    if (!m_bShadertoyMode)
      ComputeGridAlphaValues();

    // ──── DX12 rendering path ────
    if (m_lpDX && m_lpDX->m_device) {
      DX12_RenderWarpAndComposite();
      std::swap(m_dx12VS[0], m_dx12VS[1]);
    }
    // Nothing follows. The 265-line DX9 renderer that stood here is gone
    // (issue 17), together with every call site of the nine dead DX9 draw
    // functions -- the branch above is the only renderer there is.
  }
} // end RenderFrame


void mdrop::Engine::DX12_DrawMotionVectors(CState* pStateOverride,
                                           RenderContext* pCtx,
                                           DX12Texture* pTarget) {
  // DX12 port of DrawMotionVectors() — draws motion vector lines into the face
  // the warp pass is about to read, so they enter the feedback loop.
  //
  // Everything the draw needs now comes from the caller's context rather than
  // from engine members (#366). While the primary was the only caller the two
  // were the same thing; for an own-preset display they are four different
  // things — its preset, its canvas, its warp mesh and its command list — and
  // reading any of them off the engine would draw the PRIMARY's motion field
  // onto another display, which is the failure #186 fixed for the wave and
  // border stage.
  CState* pState = pStateOverride ? pStateOverride : m_pState;
  if (!pState)
    return;
  if ((float)*pState->var_pf_mv_a < 0.001f)
    return;
  if (!m_lpDX)
    return;

  auto* cmdList = (pCtx && pCtx->cmdList) ? pCtx->cmdList
                                          : m_lpDX->m_commandList.Get();
  if (!cmdList)
    return;

  int nX = (int)(*pState->var_pf_mv_x);
  int nY = (int)(*pState->var_pf_mv_y);
  float dx = (float)*pState->var_pf_mv_x - nX;
  float dy = (float)*pState->var_pf_mv_y - nY;
  if (nX > 64) { nX = 64; dx = 0; }
  if (nY > 48) { nY = 48; dy = 0; }
  if (nX <= 0 || nY <= 0)
    return;

  // This surface's canvas and mesh, not the engine's.
  const int texW = (pCtx && pCtx->canvasW > 0) ? pCtx->canvasW : m_nTexSizeX;
  const int texH = (pCtx && pCtx->canvasH > 0) ? pCtx->canvasH : m_nTexSizeY;
  const MYVERTEX* meshVerts = (pCtx && pCtx->verts) ? pCtx->verts : m_verts;
  if (texW <= 0 || texH <= 0 || !meshVerts)
    return;

  float dx2 = (float)(*pState->var_pf_mv_dx);
  float dy2 = (float)(*pState->var_pf_mv_dy);
  float len_mult = (float)*pState->var_pf_mv_l;
  if (dx < 0) dx = 0;
  if (dy < 0) dy = 0;
  if (dx > 1) dx = 1;
  if (dy > 1) dy = 1;
  float inv_texsize = 1.0f / (float)texW;
  float min_len = 1.0f * inv_texsize;

  WFVERTEX v[(64 + 1) * 2];
  ZeroMemory(v, sizeof(WFVERTEX) * (64 + 1) * 2);
  v[0].Diffuse = D3DCOLOR_RGBA_01(
    (float)*pState->var_pf_mv_r, (float)*pState->var_pf_mv_g,
    (float)*pState->var_pf_mv_b, (float)*pState->var_pf_mv_a);
  for (int x = 1; x < (nX + 1) * 2; x++)
    v[x].Diffuse = v[0].Diffuse;

  // The face the warp will read. The primary's is VS[0]; an own-preset
  // display's is whichever half of its own ping-pong is current this frame.
  DX12Texture& target = pTarget ? *pTarget : m_dx12VS[0];
  // Null for the primary, deliberately: TransitionResource resolves a null list
  // through the thread/context overrides, and handing it an explicit one
  // bypasses that. The primary must keep resolving exactly as it did before
  // this function took a context at all.
  m_lpDX->TransitionResource(target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                             pCtx ? cmdList : nullptr);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_lpDX->GetRtvCpuHandle(target);
  cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  SetViewportAndScissor(cmdList, texW, texH);

  // Root signature + PSO for alpha-blended line drawing
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
  cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_LINE_ALPHABLEND_WFVERTEX].Get());

  for (int y = 0; y < nY; y++) {
    float fy = (y + 0.25f) / (float)(nY + dy + 0.25f - 1.0f);
    fy -= dy2;

    if (fy > 0.0001f && fy < 0.9999f) {
      int n = 0;
      for (int x = 0; x < nX; x++) {
        float fx = (x + 0.25f) / (float)(nX + dx + 0.25f - 1.0f);
        fx += dx2;

        if (fx > 0.0001f && fx < 0.9999f) {
          float fx2, fy2;
          ReversePropagatePoint(fx, fy, &fx2, &fy2, meshVerts);

          // Enforce minimum trail lengths
          {
            float ddx = (fx2 - fx);
            float ddy = (fy2 - fy);
            ddx *= len_mult;
            ddy *= len_mult;
            float len = sqrtf(ddx * ddx + ddy * ddy);

            if (len > min_len) {
              // keep as-is
            } else if (len > 0.00000001f) {
              len = min_len / len;
              ddx *= len;
              ddy *= len;
            } else {
              ddx = min_len;
              ddy = min_len;
            }

            fx2 = fx + ddx;
            fy2 = fy + ddy;
          }

          v[n].x = fx * 2.0f - 1.0f;
          v[n].y = fy * 2.0f - 1.0f;
          v[n + 1].x = fx2 * 2.0f - 1.0f;
          v[n + 1].y = fy2 * 2.0f - 1.0f;

          n += 2;
        }
      }

      if (n > 0)
        m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_LINELIST, v, n, sizeof(WFVERTEX));
    }
  }
}

/*
void mdrop::Engine::UpdateSongInfo()
{
  if (m_bShowSongTitle || m_bSongTitleAnims)
  {
    char szOldSongMessage[512];
    lstrcpy(szOldSongMessage, m_szSongMessage);

    if (::GetWindowText(m_hWndParent, m_szSongMessage, sizeof(m_szSongMessage)))
    {
      // remove ' - Winamp' at end
      if (strlen(m_szSongMessage) > 9)
      {
        int check_pos = strlen(m_szSongMessage) - 9;
        if (lstrcmp(" - Winamp", (char *)(m_szSongMessage + check_pos)) == 0)
          m_szSongMessage[check_pos] = 0;
      }

      // remove ' - Winamp [Paused]' at end
      if (strlen(m_szSongMessage) > 18)
      {
        int check_pos = strlen(m_szSongMessage) - 18;
        if (lstrcmp(" - Winamp [Paused]", (char *)(m_szSongMessage + check_pos)) == 0)
          m_szSongMessage[check_pos] = 0;
      }

      // remove song # and period from beginning
      char *p = m_szSongMessage;
      while (*p >= '0' && *p <= '9') p++;
      if (*p == '.' && *(p+1) == ' ')
      {
        p += 2;
        int pos = 0;
        while (*p != 0)
        {
          m_szSongMessage[pos++] = *p;
          p++;
        }
        m_szSongMessage[pos++] = 0;
      }

      // fix &'s for display
      /*
      {
        int pos = 0;
        int len = strlen(m_szSongMessage);
        while (m_szSongMessage[pos])
        {
          if (m_szSongMessage[pos] == '&')
          {
            for (int x=len; x>=pos; x--)
              m_szSongMessage[x+1] = m_szSongMessage[x];
            len++;
            pos++;
          }
          pos++;
        }
      }*/
      /*
      if (m_bSongTitleAnims &&
        ((lstrcmp(szOldSongMessage, m_szSongMessage) != 0) || (GetFrame()==0)))
      {
        // launch song title animation
        LaunchSongTitleAnim();

        /*
        m_supertext.bRedrawSuperText = true;
        m_supertext.bIsSongTitle = true;
        lstrcpy(m_supertext.szText, m_szSongMessage);
        lstrcpy(m_supertext.nFontFace, m_szTitleFontFace);
        m_supertext.fFontSize   = (float)m_nTitleFontSize;
        m_supertext.bBold       = m_bTitleFontBold;
        m_supertext.bItal       = m_bTitleFontItalic;
        m_supertext.fX          = 0.5f;
        m_supertext.fY          = 0.5f;
        m_supertext.fGrowth     = 1.0f;
        m_supertext.fDuration   = m_fSongTitleAnimDuration;
        m_supertext.nColorR     = 255;
        m_supertext.nColorG     = 255;
        m_supertext.nColorB     = 255;

        m_supertext.fStartTime  = GetTime();
        */
        /*			}
            }
            else
            {
              sprintf(m_szSongMessage, "<couldn't get song title>");
            }
          }

          m_nTrackPlaying = SendMessage(m_hWndParent,WM_USER, 0, 125);

          // append song time
          if (m_bShowSongTime && m_nSongPosMS >= 0)
          {
            float time_s = m_nSongPosMS*0.001f;

            int minutes = (int)(time_s/60);
            time_s -= minutes*60;
            int seconds = (int)time_s;
            time_s -= seconds;
            int dsec = (int)(time_s*100);

            sprintf(m_szSongTime, "%d:%02d.%02d", minutes, seconds, dsec);
          }

          // append song length
          if (m_bShowSongLen && m_nSongLenMS > 0)
          {
            int len_s = m_nSongLenMS/1000;
            int minutes = len_s/60;
            int seconds = len_s - minutes*60;

            char buf[512];
            sprintf(buf, " / %d:%02d", minutes, seconds);
            lstrcat(m_szSongTime, buf);
          }
        }
        */

bool mdrop::Engine::ReversePropagatePoint(float fx, float fy, float* fx2, float* fy2,
                                          const MYVERTEX* verts) {
  // The mesh to trace back through. An own-preset display warps by ITS preset,
  // so tracing the engine's mesh would draw the primary's motion field on it
  // (#366). The grid DIMENSIONS are still engine-wide -- a surface's mesh is
  // sized (m_nGridX+1)*(m_nGridY+1) and RenderClassicOrientPipeline rejects a
  // context whose verts disagree (milkdropfs.cpp:2205).
  if (!verts) verts = m_verts;
  if (!verts) return false;
  //float fy = y/(float)nMotionVectorsY;
  int   y0 = (int)(fy * m_nGridY);
  float dy = fy * m_nGridY - y0;

  //float fx = x/(float)nMotionVectorsX;
  int   x0 = (int)(fx * m_nGridX);
  float dx = fx * m_nGridX - x0;

  int x1 = x0 + 1;
  int y1 = y0 + 1;

  if (x0 < 0) return false;
  if (y0 < 0) return false;
  //if (x1 < 0) return false;
  //if (y1 < 0) return false;
  //if (x0 > m_nGridX) return false;
  //if (y0 > m_nGridY) return false;
  if (x1 > m_nGridX) return false;
  if (y1 > m_nGridY) return false;

  float tu, tv;
  tu = verts[y0 * (m_nGridX + 1) + x0].tu * (1 - dx) * (1 - dy);
  tv = verts[y0 * (m_nGridX + 1) + x0].tv * (1 - dx) * (1 - dy);
  tu += verts[y0 * (m_nGridX + 1) + x1].tu * (dx) * (1 - dy);
  tv += verts[y0 * (m_nGridX + 1) + x1].tv * (dx) * (1 - dy);
  tu += verts[y1 * (m_nGridX + 1) + x0].tu * (1 - dx) * (dy);
  tv += verts[y1 * (m_nGridX + 1) + x0].tv * (1 - dx) * (dy);
  tu += verts[y1 * (m_nGridX + 1) + x1].tu * (dx) * (dy);
  tv += verts[y1 * (m_nGridX + 1) + x1].tv * (dx) * (dy);

  *fx2 = tu;
  *fy2 = 1.0f - tv;
  return true;
}

void mdrop::Engine::GetSafeBlurMinMax(CState* pState, float* blur_min, float* blur_max) {
  blur_min[0] = (float)*pState->var_pf_blur1min;
  blur_min[1] = (float)*pState->var_pf_blur2min;
  blur_min[2] = (float)*pState->var_pf_blur3min;
  blur_max[0] = (float)*pState->var_pf_blur1max;
  blur_max[1] = (float)*pState->var_pf_blur2max;
  blur_max[2] = (float)*pState->var_pf_blur3max;

  // check that precision isn't wasted in later blur passes [...min-max gap can't grow!]
  // also, if min-max are close to each other, push them apart:
  const float fMinDist = 0.1f;
  if (blur_max[0] - blur_min[0] < fMinDist) {
    float avg = (blur_min[0] + blur_max[0]) * 0.5f;
    blur_min[0] = avg - fMinDist * 0.5f;
    blur_max[0] = avg + fMinDist * 0.5f;
  }
  blur_max[1] = min(blur_max[0], blur_max[1]);
  blur_min[1] = max(blur_min[0], blur_min[1]);
  if (blur_max[1] - blur_min[1] < fMinDist) {
    float avg = (blur_min[1] + blur_max[1]) * 0.5f;
    blur_min[1] = avg - fMinDist * 0.5f;
    blur_max[1] = avg + fMinDist * 0.5f;
  }
  blur_max[2] = min(blur_max[1], blur_max[2]);
  blur_min[2] = max(blur_min[1], blur_min[2]);
  if (blur_max[2] - blur_min[2] < fMinDist) {
    float avg = (blur_min[2] + blur_max[2]) * 0.5f;
    blur_min[2] = avg - fMinDist * 0.5f;
    blur_max[2] = avg + fMinDist * 0.5f;
  }
}


// Fullscreen NDC quad as TRIANGLESTRIP (TL, TR, BL, BR).
// Buffer A–D: tv=0 at top (DX). Written pixels and texelFetch(fragCoord) stay
// self-consistent for temporal feedback.
// Image: flipV=true so tv=1 at top. fragCoord then matches Shadertoy (y=0 at
// bottom). Combined with Buffer A/B's DX storage, texelFetch of those buffers
// from Image also lines up. Do not flip the backbuffer blit — Image already
// wrote Shadertoy-upright content into the RT.
// CRITICAL: never put ang=π on left and ang=0 on right (old code); that makes
// a permanent center triangle that feedback stacks into cascading layers.
// rad/ang are consistent placeholders; COMP PS recomputes them per-pixel.
static void FillShadertoyFullscreenStrip(MYVERTEX v[4], bool flipV = false)
{
  ZeroMemory(v, sizeof(MYVERTEX) * 4);
  // TL, TR, BL, BR
  const float cx[4] = { -1.f, 1.f, -1.f, 1.f };
  const float cy[4] = { 1.f, 1.f, -1.f, -1.f };
  const float cu[4] = { 0.f, 1.f, 0.f, 1.f };
  const float cvUnflip[4] = { 0.f, 0.f, 1.f, 1.f };
  const float cvFlip[4]   = { 1.f, 1.f, 0.f, 0.f };
  const float* cv = flipV ? cvFlip : cvUnflip;
  for (int i = 0; i < 4; i++) {
    v[i].x = cx[i];
    v[i].y = cy[i];
    v[i].z = 0.f;
    v[i].Diffuse = 0xFFFFFFFFu;
    v[i].tu = cu[i];
    v[i].tv = cv[i];
    v[i].tu_orig = cu[i];
    v[i].tv_orig = cv[i];
    const float dx = cu[i] * 2.f - 1.f;
    const float dy = cv[i] * 2.f - 1.f;
    v[i].rad = sqrtf(dx * dx + dy * dy);
    v[i].ang = 0.f; // must be the same on all verts — PS overwrites per-pixel
  }
}

void mdrop::Engine::RestorePrimaryTexSizeFromVS()
{
  ClearOutputSizeOverride();
  if (m_dx12VS[0].IsValid() && m_dx12VS[0].width > 0 && m_dx12VS[0].height > 0) {
    const int vx = (int)m_dx12VS[0].width;
    const int vy = (int)m_dx12VS[0].height;
    if (m_nTexSizeX != vx || m_nTexSizeY != vy) {
      // Expected once per mirror orient pass (SizeGuard). Verbose only — was
      // flooding debug.log every frame at LOG_WARN when portrait mirrors active.
      DLOG_VERBOSE("RestorePrimaryTexSize: %dx%d -> VS %dx%d (orient SizeGuard)",
                   m_nTexSizeX, m_nTexSizeY, vx, vy);
      m_nTexSizeX = vx;
      m_nTexSizeY = vy;
    }
  }
  // Keep aspect in sync with texsize (AllocateDX9Stuff formula)
  if (m_nTexSizeX > 0 && m_nTexSizeY > 0) {
    m_fAspectX = (m_nTexSizeY > m_nTexSizeX) ? m_nTexSizeX / (float)m_nTexSizeY : 1.0f;
    m_fAspectY = (m_nTexSizeX > m_nTexSizeY) ? m_nTexSizeY / (float)m_nTexSizeX : 1.0f;
    m_fInvAspectX = 1.0f / m_fAspectX;
    m_fInvAspectY = 1.0f / m_fAspectY;
  }
}

// Every surface's pipe. Device teardown and shutdown do not know or care how
// many displays are mirrored -- they only know the D3D objects must all go.
void mdrop::Engine::ReleaseAllOrientPipelines()
{
  for (auto& sp : m_mirrorSurfaces)
    ReleaseOrientPipeline(*sp);
}

void mdrop::Engine::ReleaseOrientPipeline(MirrorSurface& surf)
{
  // Stale published faces must never be blitted against a recreated pipe.
  surf.publishedIdx.store(-1);
  surf.lastWrite = -1;
  surf.pipe.disp[0].Reset(); surf.pipe.disp[1].Reset(); surf.pipe.disp[2].Reset();
  surf.pipe.dispWrite = 0;
  surf.pipe.fbA[0].Reset(); surf.pipe.fbA[1].Reset();
  surf.pipe.fbB[0].Reset(); surf.pipe.fbB[1].Reset();
  surf.pipe.fbC[0].Reset(); surf.pipe.fbC[1].Reset();
  surf.pipe.fbD[0].Reset(); surf.pipe.fbD[1].Reset();
  surf.pipe.imgFb[0].Reset(); surf.pipe.imgFb[1].Reset();
  for (int i = 0; i < 6; i++) {
    surf.pipe.blur[i].Reset();
    surf.pipe.blurW[i] = 0;
    surf.pipe.blurH[i] = 0;
  }
  surf.pipe.w = surf.pipe.h = 0;
  surf.pipe.fbIdx = 0;
  surf.pipe.frames = 0;
  surf.pipe.ready = false;
  // bindBase left allocated (descriptor heap slots not reclaimed mid-session)
}

// Long-edge ceiling for the mirror's feedback canvas.
//
//  - 1920 is the hard floor-of-last-resort: 2560 with a 4K portrait primary
//    caused a GPU TDR (device hung) on dual full classic re-renders.
//  - The preset's effective canvas limit has to reach here too. Texel-scaled
//    presets dissipate per texel, so a bigger canvas weakens the brake and the
//    feedback saturates (see the canvas-runaway notes). Capping only the
//    primary fixed the window and left the panels blowing out.
//
// Uses the LATCHED limit (OnlyMirrorSurface().canvasLimit), never the live one: this must
// return the same answer for the life of a pipe or the size appears to change
// and something tries to recreate it. See OnlyMirrorSurface().canvasLimit in engine.h.
void mdrop::Engine::ClampOrientCanvas(const MirrorSurface& surf, int& w, int& h) const
{
  int maxDim = 1920;
  if (surf.canvasLimit > 0 && surf.canvasLimit < maxDim)
    maxDim = surf.canvasLimit;

  // TDR BACKOFF (#230). Lowered a step by each successful device recovery.
  //
  // The GPU cost that kills this app arrives with no ramp -- measured over and
  // over: milliseconds to 2900 ms in ONE frame -- so nothing can see it coming
  // and every mitigation that measures-then-reacts is too late by construction.
  // Three were tried and all three failed that way.
  //
  // The TDR is therefore the only honest signal there is, and this is what
  // listens to it: having actually died once, render the mirrors smaller so the
  // retry is not a rerun of the death. Mirrors are softened only on a machine
  // that has proven it cannot cope, rather than everywhere pre-emptively.
  const int tdrCeil = m_nTdrMirrorCeiling.load(std::memory_order_relaxed);
  if (tdrCeil > 0 && tdrCeil < maxDim)
    maxDim = tdrCeil;
  if (w > maxDim || h > maxDim) {
    const float s = (float)maxDim / (float)((w > h) ? w : h);
    w = max(1, (int)(w * s + 0.5f));
    h = max(1, (int)(h * s + 0.5f));
  }
  w = ((w + 15) / 16) * 16;
  h = ((h + 15) / 16) * 16;
}

// The single answer to "can the current preset mode draw with this pipe?".
// Used by EnsureOrientPipeline's early-out AND by the render thread's rebuild
// test — see the note in engine.h before changing either caller.
bool mdrop::Engine::OrientPipeReadyForMode(const MirrorSurface& surf) const
{
  if (!surf.pipe.ready)
    return false;
  const bool dispOk = surf.pipe.disp[0].IsValid() &&
      surf.pipe.disp[1].IsValid() && surf.pipe.disp[2].IsValid();
  const bool imgOk = surf.pipe.imgFb[0].IsValid() && surf.pipe.imgFb[1].IsValid();
  // The DECLARED buffers, matching what EnsureOrientPipeline now allocates
  // (#344). This must agree with it exactly: it is the render thread's "do I
  // need to rebuild" test, and when only the builder knew about the mode a
  // classic -> .milk3 switch left this saying "fine" while the worker skipped
  // every frame and the mirrors froze. The same trap applies per buffer -- a
  // preset that wants Buffer B on a pipe built without one would bind the VS
  // fallback and render quietly wrong, forever, because nothing asked for a
  // rebuild.
  // THIS surface's buffer set, not the engine's (#373). A display holding its
  // own .milk3 declares its own Buffer A-D, and asking the primary's flags here
  // would call its pipe ready for a buffer list belonging to another preset.
  bool sA = false, sB = false, sC = false, sD = false;
  SurfaceBufferFlags(surf, sA, sB, sC, sD);
  const bool milk3Ok = imgOk &&
      surf.pipe.fbA[0].IsValid() && surf.pipe.fbA[1].IsValid() &&
      (!sB || surf.pipe.fbB[0].IsValid()) &&
      (!sC || surf.pipe.fbC[0].IsValid()) &&
      (!sD || surf.pipe.fbD[0].IsValid());
  // THIS surface's mode (#351). Asking the global made a classic own-preset
  // surface demand the full A-D milk3 buffer set the moment the PRIMARY loaded
  // a .milk3 -- so its pipe read "not ready" and was rebuilt for a mode it was
  // never going to render in.
  return dispOk && (SurfaceUsesMilk3(surf) ? milk3Ok : imgOk);
}

bool mdrop::Engine::EnsureOrientPipeline(MirrorSurface& surf, int w, int h)
{
  if (!m_lpDX || !m_lpDX->m_device || w <= 0 || h <= 0)
    return false;

  // No mark here: this runs every frame and early-outs when the pipe already
  // matches, so a mark at the top is one QueryVideoMemoryInfo per surface per
  // frame and buries the run in zero-delta lines. The exit mark below is only
  // reached when a pipe is actually built, which is the event worth timing.
  ClampOrientCanvas(surf, w, h);

  // Descriptor heap rewind can overwrite SRV slots still held by orient textures
  // (landscape strip on portrait when blit samples the wrong resource).
  // NEVER WaitForGpu here — callers may be mid command-list record (hang/SEH).
  // If GPU may still reference old textures, caller must idle before Ensure.
  // 6 blocks for warp/comp/milk3/blit as before, plus this surface's OWN blur
  // blocks -- DXC_FRAME_COUNT rotations of MAX_BLUR_PASSES (#209). The blur was
  // the last binding path still sharing one global block set.
  const UINT kBindSlots = OrientBindSlots();
  const bool epochMismatch = (surf.pipe.bindEpoch != m_lpDX->m_descriptorEpoch);

  // Mode-aware: classic only has imgFb pair + fbA[0]; milk3 has full A–D + imgFb pairs.
  if (surf.pipe.w == w && surf.pipe.h == h && !epochMismatch &&
      OrientPipeReadyForMode(surf))
    return true;

  // ONLY THE RENDER THREAD MAY BUILD OR REBUILD THIS PIPE.
  //
  // It does so from SendToDisplayOutputs, which runs under the mirror engine
  // mutex, gates on workerIdle and calls WaitForGpu first. The mirror worker
  // reaches here from RenderClassic/Milk3OrientPipeline in the MIDDLE of
  // recording its command list, where ReleaseOrientPipeline would free
  // textures the GPU is still reading — the "caller should have WaitForGpu"
  // note above is not satisfiable from there.
  //
  // This stayed latent while the size was a constant 1920 clamp: the worker's
  // call always matched the existing pipe and returned at the fast path above.
  // Once ClampOrientCanvas folded the per-preset canvas limit in, leaving the
  // one preset that carries a canvasMax resized the orient pipe 768x448 ->
  // 1920x1088, and the first thread to notice was the worker. Device removed,
  // 0x887A0006 (2026-08-23).
  //
  // Skipping the frame is the right answer, not a fallback: the panels hold
  // their last image, the worker submits nothing (so workerIdle goes true) and
  // the render thread rebuilds on its very next pass.
  //
  // Was a comparison against the single worker's thread id. With a thread per
  // surface (#186 phase 5c) that test would pass for exactly one of them and
  // let the others rebuild a pipe from inside their record -- the same device
  // removal, just harder to reproduce.
  if (IsMirrorWorkerThread())
    return false;

  // Drop old pipe (caller should have WaitForGpu if GPU could still be using it).
  if (epochMismatch && surf.pipe.ready)
    DebugLogA("OrientPipe: descriptor epoch changed — full recreate (stale SRV fix)\n", LOG_WARN);

  // The epoch counter alone cannot prove the old reservation survived. A full
  // DXContext re-init constructs a NEW context whose m_descriptorEpoch restarts
  // at 0 AND whose SRV bump allocator restarts at 0, so a bindBase saved under
  // the previous device can compare "same epoch" while the heap under it has
  // already been handed out again. Reuse the saved range only when the
  // allocator has actually passed its end.
  //
  // Without this the per-frame block fills write over whatever now lives in
  // [bindBase, bindBase+kBindSlots) — and what lives there is the orient pipe's
  // OWN textures, since they are allocated from the same rewound pointer. The
  // blur SRVs get overwritten with the slots the block carries (mostly the
  // missing-texture fallback), so the mirror renders the fallback texture while
  // the primary is fine. Measured 2026-08-22: re-init at 03:08 left bindBase
  // 2501 reused with the orient blur SRVs at 2505/2507 — inside block 0 — and
  // the whole mirror came up as the fallback image.
  const bool stillReserved = (surf.pipe.bindBase != UINT_MAX) &&
      (m_lpDX->m_nextFreeSrvSlot >= surf.pipe.bindBase + kBindSlots);
  if (!epochMismatch && surf.pipe.bindBase != UINT_MAX && !stillReserved) {
    char buf[160];
    sprintf(buf, "OrientPipe: bind range %u..%u no longer reserved (free=%u) -- reallocating\n",
            surf.pipe.bindBase, surf.pipe.bindBase + kBindSlots,
            m_lpDX->m_nextFreeSrvSlot);
    DebugLogA(buf, LOG_WARN);
  }
  const UINT savedBase = (!epochMismatch && stillReserved)
      ? surf.pipe.bindBase : UINT_MAX;
  ReleaseOrientPipeline(surf);

  if (savedBase != UINT_MAX) {
    surf.pipe.bindBase = savedBase;
    surf.pipe.bindEpoch = m_lpDX->m_descriptorEpoch;
  } else {
    // A range handed back by a retired surface, before taking a new one.
    //
    // Without this the allocator only ever goes up: a surface retired by
    // RetireMirrorSurfacesExcept abandons its block, so a long session of
    // display changes climbs even though a surface rebuilding its OWN pipe
    // reuses its range via stillReserved above.
    //
    // This is NOT what exhausted the heap on 2026-09-12. That log has only two
    // bindBase allocations and no "no longer reserved" warnings, so nothing
    // had leaked -- 4096 descriptors simply could not hold a third orient
    // pipe. See DXC_MAX_SRV for that fix and its numbers.
    //
    // The epoch is checked because a device re-init rewinds m_nextFreeSrvSlot
    // to its baseline and hands the same descriptors out again; a range saved
    // under the old epoch names slots that now belong to somebody else.
    if (m_freeOrientBindEpoch != m_lpDX->m_descriptorEpoch) {
      m_freeOrientBindBases.clear();
      m_freeOrientBindEpoch = m_lpDX->m_descriptorEpoch;
    }
    // Does [base, base+kBindSlots) run into a range another LIVE surface is
    // still using? (#354)
    //
    // The free list and m_nextFreeSrvSlot are two allocators over one
    // descriptor space, and only one of them knows about a reused range. This
    // is what reconciles them at the point the answer is knowable.
    auto overlapsLiveSurface = [&](UINT base) -> const MirrorSurface* {
      for (const auto& sp : SnapshotMirrorSurfaces()) {
        if (!sp || sp.get() == &surf) continue;
        if (sp->pipe.bindBase == UINT_MAX) continue;
        if (sp->pipe.bindEpoch != m_lpDX->m_descriptorEpoch) continue;
        if (base < sp->pipe.bindBase + kBindSlots &&
            sp->pipe.bindBase < base + kBindSlots)
          return sp.get();
      }
      return nullptr;
    };

    // Drop any freed base that a live surface has since taken over. Nothing
    // else notices: a range goes on the list when its surface retires, and it
    // can be handed out again from the bump pointer in between.
    while (!m_freeOrientBindBases.empty()) {
      const UINT cand = m_freeOrientBindBases.back();
      const MirrorSurface* clash = overlapsLiveSurface(cand);
      if (!clash) break;
      m_freeOrientBindBases.pop_back();
      char cbuf[224];
      sprintf(cbuf, "OrientPipe: discarding freed bindBase=%u -- overlaps live "
                    "surface %ls at %u..%u\n",
              cand, clash->device.empty() ? L"(none)" : clash->device.c_str(),
              clash->pipe.bindBase, clash->pipe.bindBase + kBindSlots);
      DebugLogA(cbuf, LOG_WARN);
    }

    if (!m_freeOrientBindBases.empty()) {
      surf.pipe.bindBase = m_freeOrientBindBases.back();
      m_freeOrientBindBases.pop_back();
      char rbuf[144];
      sprintf(rbuf, "OrientPipe: reusing freed SRV bindBase=%u (%u still free)\n",
              surf.pipe.bindBase, (unsigned)m_freeOrientBindBases.size());
      DebugLogA(rbuf, LOG_INFO);
    } else {
      if (m_lpDX->m_nextFreeSrvSlot + kBindSlots > DXC_MAX_SRV) {
        DebugLogA("OrientPipe: SRV heap full — cannot reserve bind blocks\n", LOG_ERROR);
        return false;
      }
      surf.pipe.bindBase = m_lpDX->m_nextFreeSrvSlot;
      m_lpDX->m_nextFreeSrvSlot += kBindSlots;
      char buf[144];
      sprintf(buf, "OrientPipe: SRV bindBase=%u (%u slots, free=%u/%u)\n",
              surf.pipe.bindBase, kBindSlots,
              m_lpDX->m_nextFreeSrvSlot, (UINT)DXC_MAX_SRV);
      DebugLogA(buf, LOG_INFO);
    }

    // THE INVARIANT, and the bug this fixes (#354): the bump pointer must
    // always sit ABOVE every live reservation.
    //
    // Hold that, and every other allocation in the process is safe by
    // construction -- textures, font atlases and the letterbox SRV all bump
    // the same counter, and none of them knows an orient pipe exists.
    //
    // The fresh branch above keeps it for free. The REUSE branch did not: it
    // takes a base off the free list without touching the pointer, which is
    // right in the ordinary case (the range is already accounted for) and
    // wrong whenever the pointer has been rewound to at or below that base.
    // Then the next bump -- of anything -- walks straight into a live range.
    //
    // Measured on Shane's instance, from its own log:
    //
    //   09:26:10  bindBase=2237 (free=2813)   two surfaces, 576 apart, clean
    //   09:26:10  bindBase=2824 (free=3400)
    //   14:05:33  Idle timer activated -> DISPLAY3 re-init
    //   14:05:33  bind range 2824..3400 no longer reserved (free=2237)
    //   14:05:33  reusing freed SRV bindBase=2237      <- pointer left at 2237
    //   15:31:45  bindBase=2248 (free=2824)            <- 11 slots later
    //
    // 2237..2813 against 2248..2824: 565 of 576 descriptors in common. Both
    // surfaces then filled their binding blocks into the same slots every
    // frame, so both displays drew whichever wrote last -- reported as "my
    // display1 and 3 have gotten linked and are running the same preset",
    // with the per-display MESSAGES still correctly different, because only
    // the descriptors were shared and not the surfaces.
    //
    // The eleven slots between 2237 and 2248 went to ordinary textures, which
    // is the same collision with a wider blast radius and needs no second
    // own-preset display to happen.
    if (m_lpDX->m_nextFreeSrvSlot < surf.pipe.bindBase + kBindSlots) {
      char abuf[176];
      sprintf(abuf, "OrientPipe: bump pointer %u was inside reservation %u..%u "
                    "-- advancing (#354)\n",
              m_lpDX->m_nextFreeSrvSlot, surf.pipe.bindBase,
              surf.pipe.bindBase + kBindSlots);
      DebugLogA(abuf, LOG_WARN);
      m_lpDX->m_nextFreeSrvSlot = surf.pipe.bindBase + kBindSlots;
    }
    surf.pipe.bindEpoch = m_lpDX->m_descriptorEpoch;
  }

  // Formats must match the PSO each target is drawn with — a render target whose
  // format differs from the bound PSO's RTVFormats[0] is invalid D3D12, and with
  // no debug layer running it does not error, it corrupts or hangs.
  //   imgFb / disp / blur : UNORM   (comp PSO in milk3 mode targets the UNORM
  //                                  backbuffer format; classic comp likewise)
  //   fbA–fbD             : FLOAT32 (Buffer A–D PSOs are built with
  //                                  feedbackRtvFormat = R32G32B32A32_FLOAT)
  // fbA–D were UNORM until 2026-08-23, so every mirror buffer pass ran
  // format-mismatched: selfie/volcanic (raymarchers that park far-plane
  // sentinels and non-colour data in a buffer) drew wrong on the panels, and
  // elevated took the driver out (0x887A0005, DRED page fault VA=0x0).
  // Allocate only what the active mode needs so we stay inside the dynamic RTV range:
  //   classic: imgFb×2 (VS) + fbA[0] (comp display) + blur×6  ≈ 9 RTVs
  //   milk3:   fbA–D×2 + imgFb×2                             ≈ 10 RTVs
  // Full classic+milk3 all-pairs+blur was 16 and exhausted DXC_MAX_RTV.
  // clearAlpha: what THIS target is cleared with further down, so its optimized
  // clear value matches and the driver keeps its fast path (forgejo#104).
  // RenderMilk3OrientPipeline clears fbA-D and imgFb to (0,0,0,0) because those
  // carry Shadertoy accumulation data in alpha; RenderClassicOrientPipeline
  // clears the VS faces and display to opaque black. The pipe is rebuilt on a
  // mode change (OrientPipeReadyForMode), so imgFb is allocated for whichever
  // role it is about to serve.
  auto makeOneFmt = [&](DX12Texture& tex, const char* name, DXGI_FORMAT fmt,
                        float clearAlpha = 1.f) -> bool {
    tex = m_lpDX->CreateRenderTargetTexture((UINT)w, (UINT)h, fmt, clearAlpha);
    if (!tex.IsValid()) {
      char buf[128];
      sprintf(buf, "OrientPipe: FAILED creating %s %dx%d (rtv free=%u/%u)\n",
              name, w, h, m_lpDX->m_nextFreeRtvSlot, (UINT)DXC_MIRROR_RTV_BASE);
      DebugLogA(buf, LOG_ERROR);
      return false;
    }
    return true;
  };
  auto makeOne = [&](DX12Texture& tex, const char* name) -> bool {
    return makeOneFmt(tex, name, DXGI_FORMAT_R8G8B8A8_UNORM);
  };
  auto makePairFmt = [&](DX12Texture pair[2], const char* name, DXGI_FORMAT fmt,
                         float clearAlpha = 1.f) -> bool {
    char n0[32], n1[32];
    sprintf(n0, "%s[0]", name);
    sprintf(n1, "%s[1]", name);
    return makeOneFmt(pair[0], n0, fmt, clearAlpha) &&
           makeOneFmt(pair[1], n1, fmt, clearAlpha);
  };
  auto makePair = [&](DX12Texture pair[2], const char* name,
                      float clearAlpha = 1.f) -> bool {
    return makePairFmt(pair, name, DXGI_FORMAT_R8G8B8A8_UNORM, clearAlpha);
  };

  // Built for THIS surface's mode (#351). With the global here, a display
  // holding a classic preset of its own was given the .milk3 texture set --
  // eight FLOAT32 targets it could never use -- whenever the primary happened
  // to be on a .milk3, and had its pipe rebuilt again on the way back.
  const bool forMilk3 = SurfaceUsesMilk3(surf);
  const bool forClassic = !forMilk3;

  if (forMilk3) {
    const DXGI_FORMAT kFb = DXGI_FORMAT_R32G32B32A32_FLOAT; // Buffer A–D PSO format
    // Only the buffers the preset DECLARES (#344).
    //
    // All four pairs were allocated unconditionally: eight FLOAT32 targets at
    // 16 bytes a pixel is 128 B/px PER SURFACE, and the overwhelming majority
    // of .milk3 presets use Buffer A alone -- the GLSL importer cannot even
    // produce Buffer C or D (docs/GLSL_importing.md), so on an imported preset
    // those two pairs were pure waste. Measured at +533 MB across two 1920x1088
    // surfaces for one .milk3 on the primary.
    //
    // fbA is unconditional: RenderFrameShadertoy binds it every frame for the
    // Image pass whether or not a Buffer A shader exists.
    //
    // GROW-ONLY, never shrink: `|| already valid` keeps a pair that an earlier
    // preset needed. Dropping one would make the next preset that wants it a
    // pipe REBUILD, and rebuilds are the thing to avoid here -- AllocateRtv is
    // a bump allocator with no free path, so each recreate burns ~10 RTV slots
    // until the allocator rewinds and the new RTVs alias live ones. That is the
    // deterministic TDR (0x887A0006) recorded at ClampOrientCanvas. Monotonic
    // growth bounds it at three extra rebuilds for the whole session.
    // THIS surface's declared buffers (#373), matching the readiness test at
    // OrientPipeReadyForMode. Those two must agree about the buffer list or the
    // builder and the rebuild test disagree, which is the shape that froze
    // every mirror on 2026-08-23 -- hence one helper feeding both.
    bool bA = false, bB = false, bC = false, bD = false;
    SurfaceBufferFlags(surf, bA, bB, bC, bD);
    const bool wantB = bB || surf.pipe.fbB[0].IsValid();
    const bool wantC = bC || surf.pipe.fbC[0].IsValid();
    const bool wantD = bD || surf.pipe.fbD[0].IsValid();
    if (!makePairFmt(surf.pipe.fbA, "fbA", kFb, 0.f) ||
        (wantB && !makePairFmt(surf.pipe.fbB, "fbB", kFb, 0.f)) ||
        (wantC && !makePairFmt(surf.pipe.fbC, "fbC", kFb, 0.f)) ||
        (wantD && !makePairFmt(surf.pipe.fbD, "fbD", kFb, 0.f)) ||
        !makePair(surf.pipe.imgFb, "imgFb", 0.f)) {
      ReleaseOrientPipeline(surf);
      return false;
    }
  } else {
    // Classic: VS ping-pong only (display goes to the disp[] rotation below).
    if (!makePair(surf.pipe.imgFb, "imgFb")) {
      ReleaseOrientPipeline(surf);
      return false;
    }
  }

  // Triple-buffered display rotation (both modes) — see engine.h.
  if (!makeOne(surf.pipe.disp[0], "disp[0]") ||
      !makeOne(surf.pipe.disp[1], "disp[1]") ||
      !makeOne(surf.pipe.disp[2], "disp[2]")) {
    ReleaseOrientPipeline(surf);
    return false;
  }
  surf.pipe.dispWrite = 0;

  // Classic blur pyramid at orient size (GetBlur1/2/3 for blue haze etc.)
  if (forClassic) {
    int bw = w, bh = h;
    for (int i = 0; i < 6; i++) {
      if (!(i & 1) || (i < 2)) {
        bw = max(16, bw / 2);
        bh = max(16, bh / 2);
      }
      surf.pipe.blurW[i] = ((bw + 3) / 16) * 16;
      surf.pipe.blurH[i] = ((bh + 3) / 4) * 4;
      surf.pipe.blur[i] = m_lpDX->CreateRenderTargetTexture(
          (UINT)surf.pipe.blurW[i], (UINT)surf.pipe.blurH[i],
          DXGI_FORMAT_R8G8B8A8_UNORM);
      if (!surf.pipe.blur[i].IsValid()) {
        char buf[160];
        sprintf(buf, "OrientPipe: FAILED blur[%d] %dx%d (rtv free=%u/%u) — continuing without blur\n",
                i, surf.pipe.blurW[i], surf.pipe.blurH[i],
                m_lpDX->m_nextFreeRtvSlot, (UINT)DXC_MIRROR_RTV_BASE);
        DebugLogA(buf, LOG_ERROR);
        // Soft-fail: warp/comp still run; blur slots fall back in BuildBindingSlots
        for (int j = i; j < 6; j++) {
          surf.pipe.blur[j].Reset();
          surf.pipe.blurW[j] = 0;
          surf.pipe.blurH[j] = 0;
        }
        break;
      }
    }
  }

  // What this pipe actually cost, per surface (#344). Attributing orient-pipe
  // VRAM by arithmetic from outside got it wrong -- the measured total came to
  // roughly three times what the declared buffers predicted -- and there was no
  // way to ask which pairs were built for which surface. With this, the
  // residual was identified as the mode-change REBUILD rather than the
  // feedback buffers.
  if (forMilk3) {
    const int fbPairs = 1 + (surf.pipe.fbB[0].IsValid() ? 1 : 0)
                          + (surf.pipe.fbC[0].IsValid() ? 1 : 0)
                          + (surf.pipe.fbD[0].IsValid() ? 1 : 0);
    DLOG_INFO("OrientPipe[%ls] milk3 %dx%d: %d FLOAT32 pair(s) "
              "(B=%d C=%d D=%d declared) = %.1f MB",
              surf.device.empty() ? L"(none)" : surf.device.c_str(), w, h,
              fbPairs, m_bHasBufferB ? 1 : 0, m_bHasBufferC ? 1 : 0,
              m_bHasBufferD ? 1 : 0,
              (double)fbPairs * 2.0 * w * h * 16.0 / (1024.0 * 1024.0));
  }

  // The VRAM checkpoint stays alongside it: the log line above says what THIS
  // pipe asked for, the mark says what the GPU actually charged for it, and the
  // gap between those two is what made this issue hard to attribute.
  //
  // m_bShadertoyMode is still GLOBAL, so the primary loading one .milk3 rebuilds
  // every surface's pipe -- that is the remaining cost here, not the feedback
  // buffers, which are now only the pairs the preset declares.
  VMARK(forMilk3 ? "EnsureOrientPipeline exit (milk3)"
                 : "EnsureOrientPipeline exit (classic)");

  surf.pipe.w = w;
  surf.pipe.h = h;
  surf.pipe.fbIdx = 0;
  surf.pipe.frames = 0;
  surf.pipe.ready = true;
  // Mirror flip-chains may still hold letterboxed landscape frames — wipe until
  // every buffer index has been redrawn (see paintedBufferMask in Present path).
  for (auto& out : m_displayOutputs) {
    if (out.monitorState) {
      out.monitorState->bNeedsFullChainClear = true;
      out.monitorState->paintedBufferMask = 0;
    }
  }
  {
    char buf[160];
    sprintf(buf, "OrientPipe: ready %dx%d mode=%s rtvNext=%u/%u\n",
            w, h, forMilk3 ? "milk3" : "classic",
            m_lpDX->m_nextFreeRtvSlot, (UINT)DXC_MIRROR_RTV_BASE);
    DebugLogA(buf, LOG_INFO);
  }
  return true;
}

bool mdrop::Engine::RenderMilk3OrientPipeline(MirrorSurface& surf,
                                              ID3D12GraphicsCommandList* cmdList, int outW, int outH)
{
  // SurfaceUsesMilk3, not the global (#351): the caller now dispatches on this
  // surface's own mode, so the entry guard has to agree with it or the two
  // disagree and the surface renders nothing at all.
  if (!cmdList || !m_lpDX || !SurfaceUsesMilk3(surf))
    return false;

  // THIS surface's compiled set, exactly as RenderClassicOrientPipeline picks
  // one at milkdropfs.cpp:2269 (#373). A surface holding its OWN .milk3 has its
  // own Image and Buffer A-D shaders and its own PSOs -- PresetPsoOwned has
  // carried bufferA-D and compFloat all along, and the surface's adopt path has
  // been building them for every preset it takes. Reading the ENGINE's set here
  // is what made a display unable to hold a .milk3: it would have bound the
  // primary's Shadertoy shaders against its own buffers.
  //
  // A surface with no preset of its own is rendering the primary's, so for that
  // one the engine's set IS the right set.
  MirrorSimContext& c = surf.sim;
  const bool ctxOwnSet = c.HasOwnPipeline();
  PShaderSet& sh = ctxOwnSet ? c.shaders : m_shaders;
  ID3D12PipelineState* psoComp    = ctxOwnSet ? c.psos.comp.Get()    : m_dx12CompPSO.Get();
  ID3D12PipelineState* psoBufferA = ctxOwnSet ? c.psos.bufferA.Get() : m_dx12BufferAPSO.Get();
  ID3D12PipelineState* psoBufferB = ctxOwnSet ? c.psos.bufferB.Get() : m_dx12BufferBPSO.Get();
  ID3D12PipelineState* psoBufferC = ctxOwnSet ? c.psos.bufferC.Get() : m_dx12BufferCPSO.Get();
  ID3D12PipelineState* psoBufferD = ctxOwnSet ? c.psos.bufferD.Get() : m_dx12BufferDPSO.Get();
  if (!psoComp)
    return false;

  // Which buffers exist is a property of the SET, not of the engine. The
  // primary derives its own flags the same way at engine_presets.cpp:4030
  // (`m_bHasBufferA = (m_shaders.bufferA.bytecodeBlob != NULL)`), so deriving
  // them here cannot drift from the shaders actually compiled.
  const bool hasA = (sh.bufferA.bytecodeBlob != nullptr) && psoBufferA;
  const bool hasB = (sh.bufferB.bytecodeBlob != nullptr) && psoBufferB;
  const bool hasC = (sh.bufferC.bytecodeBlob != nullptr) && psoBufferC;
  const bool hasD = (sh.bufferD.bytecodeBlob != nullptr) && psoBufferD;
  CState* pPresetState = ctxOwnSet && c.pState ? c.pState : m_pState;

  if (!EnsureOrientPipeline(surf, outW, outH) || !surf.pipe.ready)
    return false;

  Milk3OrientPipeline& P = surf.pipe;
  const int fbRead = P.fbIdx;
  const int fbWrite = 1 - P.fbIdx;
  const int W = P.w, H = P.h;

  // THIS surface, described rather than imposed.
  //
  // What stood here was SizeGuard: it overwrote the engine's texsize, aspect,
  // output-size override and mouse for the duration of the record and put them
  // all back afterwards. That restore raced the render thread --
  // RestorePrimaryTexSizeFromVS running before the guard flattened mirror
  // shapes to ellipses one frame in ~250, the reported intermittent flicker --
  // and it is why a record could never run beside a primary frame.
  //
  // THE SURFACE'S OWN CLOCK when it holds its own preset (#373).
  //
  // The frame used to be -1 unconditionally -- "ask the engine" -- and the note
  // here said the milk3 record never had a frame override. That was correct
  // while this record could only ever draw the PRIMARY's .milk3: the engine's
  // frame and time WERE this surface's, because it was rendering the engine's
  // preset.
  //
  // A display holding its own .milk3 has its own iTime and iFrame, and reading
  // the engine's froze it. Measured: frame-to-frame image change 0.01 against
  // 2.4 for the same surface on a classic preset, while its own pf counter went
  // on advancing -- so the clock was running and the picture was not.
  //
  // A surface with no preset of its own still asks the engine, because it is
  // still rendering the engine's preset.
  RenderContext ic;
  if (ctxOwnSet)
    FillAnimInputs(ic, c);
  FillCanvasInputs(ic, W, H, ctxOwnSet ? c.nFrame : -1);
  FillMouseInputs(ic, W, H);
  // This IS a mirror orient record, and the context has to say so (#208).
  //
  // It said neither, so ApplyShaderParams' primary-only guard
  // (`if (!(rc && rc->bOrientRecord))`) let this path through and a MIRROR
  // WORKER wrote m_nHighestBlurTexUsedThisFrame -- a plain int the render
  // thread is writing at the same time. The magnitude was harmless, because
  // this path scans the engine's own shader set and so could only reach a
  // value the primary would reach anyway; the unsynchronised write was not.
  //
  // Safe to set both: this function calls none of the other branches that read
  // them -- no RenderWarpPass, RenderCompPass, RenderBlurPasses,
  // DX12_DrawCustomShapes or DrawUserSprites -- so the only behaviour that
  // changes is the guard that should have been catching it.
  ic.isMirror      = true;
  ic.bOrientRecord = true;

  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  // Barriers MUST go on cmdList (mirror list). Main list is already closed.
  if (P.frames < 8) {
    float black[] = { 0.f, 0.f, 0.f, 0.f };
    auto clearPair = [&](DX12Texture pair[2]) {
      for (int i = 0; i < 2; i++) {
        if (!pair[i].IsValid()) continue;
        m_lpDX->TransitionResource(pair[i], D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
        cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(pair[i]), black, 0, nullptr);
        m_lpDX->TransitionResource(pair[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
      }
    };
    clearPair(P.fbA); clearPair(P.fbB); clearPair(P.fbC); clearPair(P.fbD); clearPair(P.imgFb);
  }

  // Warmup: clear imgFb only — caller blits black/empty to mirrors.
  if (P.frames < 2) {
    if (P.imgFb[fbWrite].IsValid()) {
      m_lpDX->TransitionResource(P.imgFb[fbWrite], D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
      float black[] = { 0.f, 0.f, 0.f, 0.f };
      cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(P.imgFb[fbWrite]), black, 0, nullptr);
      m_lpDX->TransitionResource(P.imgFb[fbWrite], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
    }
    P.fbIdx = 1 - P.fbIdx; // publish cleared face as "latest" for blit
    P.frames++;
    return true;
  }

  const DX12Texture& vsTex = m_lpDX->m_nullTexture;
  const DX12Texture* imgFbRead = &P.imgFb[fbRead];
  const bool hasAnyBuffer = hasA || hasB || hasC || hasD;
  const bool kStrict = true; // never fall back to primary feedback

  UINT slotsA[32], slotsB[32], slotsC[32], slotsD[32], slotsComp[32];
  memset(slotsA, 0xFF, sizeof(slotsA));
  memset(slotsB, 0xFF, sizeof(slotsB));
  memset(slotsC, 0xFF, sizeof(slotsC));
  memset(slotsD, 0xFF, sizeof(slotsD));
  memset(slotsComp, 0xFF, sizeof(slotsComp));

  if (hasA)
    BuildBindingSlots(&sh.bufferA.params, vsTex, slotsA,
                      &P.fbA[fbRead], imgFbRead, &P.fbB[fbRead], &P.fbC[fbRead], &P.fbD[fbRead], kStrict);
  if (hasB)
    BuildBindingSlots(&sh.bufferB.params, vsTex, slotsB,
                      &P.fbA[fbWrite], imgFbRead, &P.fbB[fbRead], &P.fbC[fbRead], &P.fbD[fbRead], kStrict);
  if (hasC)
    BuildBindingSlots(&sh.bufferC.params, vsTex, slotsC,
                      &P.fbA[fbWrite], imgFbRead, &P.fbB[fbWrite], &P.fbC[fbRead], &P.fbD[fbRead], kStrict);
  if (hasD)
    BuildBindingSlots(&sh.bufferD.params, vsTex, slotsD,
                      &P.fbA[fbWrite], imgFbRead, &P.fbB[fbWrite], &P.fbC[fbWrite], &P.fbD[fbRead], kStrict);
  if (hasAnyBuffer)
    BuildBindingSlots(&sh.comp.params, vsTex, slotsComp,
                      &P.fbA[fbWrite], imgFbRead, &P.fbB[fbWrite], &P.fbC[fbWrite], &P.fbD[fbWrite], kStrict);
  else
    BuildBindingSlots(&sh.comp.params, vsTex, slotsComp,
                      &P.fbA[fbRead], imgFbRead, &P.fbB[fbRead], &P.fbC[fbRead], &P.fbD[fbRead], kStrict);

  const UINT b0 = P.bindBase;
  const UINT bs = DXContext::BINDING_BLOCK_SIZE;
  m_lpDX->FillSrvBindingBlock(b0 + 0 * bs, slotsA);
  m_lpDX->FillSrvBindingBlock(b0 + 1 * bs, slotsB);
  m_lpDX->FillSrvBindingBlock(b0 + 2 * bs, slotsC);
  m_lpDX->FillSrvBindingBlock(b0 + 3 * bs, slotsD);
  m_lpDX->FillSrvBindingBlock(b0 + 4 * bs, slotsComp);

  auto drawBufferPass = [&](bool enabled, ID3D12PipelineState* pso, DX12Texture& writeTex,
                            PShaderInfo* si, UINT bindOffset) {
    if (!enabled || !pso || !writeTex.IsValid() || !si)
      return;
    m_lpDX->TransitionResource(writeTex, D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_lpDX->GetRtvCpuHandle(writeTex);
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float black[] = { 0.f, 0.f, 0.f, 0.f };
    cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);
    SetViewportAndScissor(cmdList, (UINT)W, (UINT)H);
    cmdList->SetPipelineState(pso);
    if (si->CT) {
      ApplyShaderParams(&si->params, si->CT, pPresetState, &ic);
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(si->CT);
      if (ct->GetShadowSize() > 0) {
        D3D12_GPU_VIRTUAL_ADDRESS cb =
            m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
        if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);
      }
    }
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetSrvBlockGpuHandle(b0 + bindOffset));
    MYVERTEX strip[4];
    FillShadertoyFullscreenStrip(strip);
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, strip, 4, sizeof(MYVERTEX), cmdList);
    m_lpDX->TransitionResource(writeTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
  };

  drawBufferPass(hasA, psoBufferA, P.fbA[fbWrite], &sh.bufferA, 0 * bs);
  drawBufferPass(hasB, psoBufferB, P.fbB[fbWrite], &sh.bufferB, 1 * bs);
  drawBufferPass(hasC, psoBufferC, P.fbC[fbWrite], &sh.bufferC, 2 * bs);
  drawBufferPass(hasD, psoBufferD, P.fbD[fbWrite], &sh.bufferD, 3 * bs);

  // Image → internal imgFb only. Callers blit to every mirror SC face (leader + peers)
  // via BlitOrientOutputToMirror — never multi-pass into a flip-model back buffer and
  // never sample a SC buffer as SRV (that caused leader-only ghost bands over time).
  if (!P.imgFb[fbWrite].IsValid())
    return false;

  m_lpDX->TransitionResource(P.imgFb[fbWrite], D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
  {
    D3D12_CPU_DESCRIPTOR_HANDLE imgRtv = m_lpDX->GetRtvCpuHandle(P.imgFb[fbWrite]);
    cmdList->OMSetRenderTargets(1, &imgRtv, FALSE, nullptr);
    float black[] = { 0.f, 0.f, 0.f, 0.f };
    cmdList->ClearRenderTargetView(imgRtv, black, 0, nullptr);
  }
  SetViewportAndScissor(cmdList, (UINT)W, (UINT)H);
  cmdList->SetPipelineState(psoComp);
  PShaderInfo* compSI = &sh.comp;
  if (compSI->CT) {
    ApplyShaderParams(&compSI->params, compSI->CT, pPresetState, &ic);
    DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(compSI->CT);
    if (ct->GetShadowSize() > 0) {
      D3D12_GPU_VIRTUAL_ADDRESS cb =
          m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
      if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);
    }
  }
  cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetSrvBlockGpuHandle(b0 + 4 * bs));
  {
    MYVERTEX strip[4];
    FillShadertoyFullscreenStrip(strip, true); // Image: Shadertoy y-up
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, strip, 4, sizeof(MYVERTEX), cmdList);
  }
  m_lpDX->TransitionResource(P.imgFb[fbWrite], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);

  // Copy the final Image into the display rotation: imgFb faces are FEEDBACK
  // (rewritten every other frame), so the render thread must never blit them
  // directly — it blits the published disp[] face instead (see engine.h).
  if (P.disp[P.dispWrite % 3].IsValid()) {
    DX12Texture& d = P.disp[P.dispWrite % 3];
    m_lpDX->TransitionResource(P.imgFb[fbWrite], D3D12_RESOURCE_STATE_COPY_SOURCE, cmdList);
    m_lpDX->TransitionResource(d, D3D12_RESOURCE_STATE_COPY_DEST, cmdList);
    cmdList->CopyResource(d.resource.Get(), P.imgFb[fbWrite].resource.Get());
    m_lpDX->TransitionResource(d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
    m_lpDX->TransitionResource(P.imgFb[fbWrite], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
  }

  P.fbIdx = 1 - P.fbIdx; // latest result is imgFb[P.fbIdx]
  P.frames++;
  return true;
}

bool mdrop::Engine::RenderClassicOrientPipeline(MirrorSurface& surf,
                                                ID3D12GraphicsCommandList* cmdList,
                                                int outW, int outH)
{
  // Classic independent re-render mirrors the primary DX12 path:
  //   imgFb[0/1] = VS ping-pong (warp feedback = previous post-warp+shapes)
  //   fbA[0]     = display target (comp output) — NEVER feed this into warp
  // Previous broken path wrote comp into the feedback face and bound every SRV
  // slot to that face (noise/disk/blur all wrong) — looked nothing like primary.
  if (!cmdList || !m_lpDX || outW <= 0 || outH <= 0)
    return false;
  // The mirror of the guard in RenderMilk3OrientPipeline (#351). This one is
  // why an own-preset display went dark rather than merely stale when the
  // primary loaded a .milk3: the dispatcher sent it here and this refused.
  if (SurfaceUsesMilk3(surf))
    return false;
  if (!EnsureOrientPipeline(surf, outW, outH) || !surf.pipe.ready)
    return false;
  if (!m_indices_list || m_nGridX < 1 || m_nGridY < 1)
    return false;

  // Startup loading screen, on an own-preset surface (#291).
  //
  // The primary's gate in RenderFrame does not cover these: a surface records
  // through this path, and FillPresetPipeline(rc, c) BORROWS the engine's
  // pipeline whenever the context has not adopted its own yet. At startup the
  // engine's set is empty too, so the borrow yielded a null warp PSO and the
  // shared warp dispatch fell through to PSO_TEXTURED_MYVERTEX -- the same
  // screenful of triangles, once per display.
  //
  // Which is exactly the arrangement Shane had to describe twice before it
  // reproduced: "it has to be clouds1b but own preset on each display I think
  // for you to see the issue". One display compiling one preset is over too
  // quickly to notice; three surfaces each compiling their own is not.
  //
  // Returning false leaves the surface unpainted for this frame, which its
  // own bNeedsFullChainClear has already made black.
  if (LoadingScreenActive() && !surf.sim.HasOwnPipeline())
    return false;

  // Independent mirror simulation context: own states (own EEL storage), own
  // warp mesh at this aspect, own blend timeline. Adoption + per-frame step
  // ran on the mirror thread BEFORE the engine lock; here (under the lock)
  // we only regenerate the blend wipe field when adoption flagged it.
  MirrorSimContext& c = surf.sim;

  // #186: build bindings from the SAME set the draws bind.
  //
  // These used to name m_shaders / m_OldShaders unconditionally while the
  // stages bound whatever RenderContext carried. That was harmless while the
  // context rendered the primary's preset -- the two sets described the same
  // shader, so the descriptor table matched. The moment the context holds a
  // DIFFERENT preset it stops matching: BuildBindingSlots resolves the
  // ENGINE preset's texture list into the table, and the CONTEXT preset's
  // shader then samples whatever happens to be in those slots.
  //
  // Reported as 'presets are rendering but they're incorrect', which is
  // exactly what a shader reading another preset's textures looks like.
  const bool ctxOwnSet = c.HasOwnPipeline();
  PShaderSet& sh    = ctxOwnSet ? c.shaders    : m_shaders;
  PShaderSet& oldSh = ctxOwnSet ? c.oldShaders : m_OldShaders;
  if (!c.pState || c.verts.size() != (size_t)(m_nGridX + 1) * (m_nGridY + 1))
    return false;

  // Everything this record publishes as `frame` is THIS context's count, not
  // the engine's (#111). An independent mirror simulates the preset for itself
  // and keeps its own counter -- that is the point of the mode, and a user who
  // wants an exact copy of the primary picks a stretch/copy path instead. The
  // defect was that only part of the preset saw it: per-frame EEL got the
  // context's counter while custom shapes, custom waves, sprite scripts and the
  // HLSL `frame` uniform all read GetFrame() in the same frame.
  MirrorSimApplyBlendPattern(c);

  // What shape is this surface actually drawing, and on which blend path?
  //
  // Logged once per surface per preset, so it costs nothing and answers the
  // question that took four confounded A/B runs to pin down: a squashed
  // own-preset mirror had an IDENTICAL mesh, canvas and aspect to a healthy
  // following one, and differed only in milk2frozen/blending. Keep it.
  {
    const std::wstring key = surf.device + L"|" + c.loadedOwnPath;
    if (surf.lastShapeLogKey != key) {
      surf.lastShapeLogKey = key;
      float ymin = 1e9f, ymax = -1e9f, xmin = 1e9f, xmax = -1e9f;
      for (const auto& v : c.verts) {
        if (v.y < ymin) ymin = v.y;
        if (v.y > ymax) ymax = v.y;
        if (v.x < xmin) xmin = v.x;
        if (v.x > xmax) xmax = v.x;
      }
      DLOG_INFO("orient surface %ls own=%d pipe=%dx%d sim=%dx%d asp=%.4f/%.4f "
                "verts x[%.3f..%.3f] y[%.3f..%.3f] milk2frozen=%d blending=%d",
                surf.device.c_str(), c.ownPresetPath.empty() ? 0 : 1,
                surf.pipe.w, surf.pipe.h, c.simW, c.simH,
                c.fAspectX, c.fAspectY,
                xmin, xmax, ymin, ymax,
                c.bMilk2FrozenBlend ? 1 : 0,
                (c.pState && c.pState->m_bBlending) ? 1 : 0);
    }
  }

  Milk3OrientPipeline& P = surf.pipe;

  // Warp mesh UVs/alphas were computed on the mirror thread by
  // MirrorSimStepFrame → ComputeGridAlphaValuesCtx into c.verts (the
  // MeshAspectGuard save/restore dance over the PRIMARY's mesh is gone).

  DX12Texture& vsRead = P.imgFb[P.fbIdx];
  DX12Texture& vsWrite = P.imgFb[1 - P.fbIdx];
  // Comp output rotates through the disp[] triple: the render thread blits
  // the PUBLISHED face while we render another, so it never waits for (or
  // falls back from) an in-flight orient frame, and a published face is
  // never rewritten while late blits may still sample it.
  DX12Texture& display = P.disp[P.dispWrite % 3];
  if (!vsRead.IsValid() || !vsWrite.IsValid() || !display.IsValid())
    return false;

  // THIS surface, described ONCE for the whole record.
  //
  // It used to be built two thirds of the way down, which left every stage
  // above it -- the video-input composite among them -- reading the primary's
  // canvas off the engine while drawing onto the mirror's. One description,
  // available to every stage, is also what lets SizeGuard go: nothing has to
  // consult the engine for a size any more.
  RenderContext ic;
  FillAnimInputs(ic, c);
  FillCanvasInputs(ic, P.w, P.h, c.nFrame);
  // THIS surface's command list, too. The context described the surface
  // completely except for where its work goes, so every draw handed `ic` still
  // fell back to the engine's active list and needed m_cmdListOverride to make
  // that answer right (#206).
  ic.cmdList = cmdList;
  // And that it IS a mirror (#230). This context described the surface in
  // every other respect and left this flag false, so every `pCtx->isMirror`
  // test on the injection path -- shapes, waves, sprites -- answered "primary"
  // for a mirror worker. It read as harmless while the only consumer was a
  // diagnostic counter; it stops being harmless the moment a per-surface
  // BUDGET asks the same question, which is how the mirror load shed came to
  // measure 0 in 3 twice while never once running.
  ic.isMirror = true;

  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  // Warmup: clear VS faces + display so feedback starts clean.
  //
  // Also on an own-preset adoption (#186). The classic path ping-pongs the VS
  // faces -- every frame warps the previous one forward -- so without a clear
  // here the image drawn by the PREVIOUS preset is carried indefinitely, and
  // whether it ever disappears depends on the new preset's decay and coverage.
  // Reported as "the initial preset seems to be loaded in memory and few seem
  // to evict it".
  //
  // .milk3 never showed this, which is what identified it: that path renders
  // Buffer A-D and Image fresh each frame and has no carry-forward to retain.
  const bool freshOwnPreset = c.clearFeedbackPending;
  c.clearFeedbackPending = false;
  if (P.frames < 2 || freshOwnPreset) {
    float black[] = { 0.f, 0.f, 0.f, 1.f };
    auto clearTex = [&](DX12Texture& t) {
      if (!t.IsValid()) return;
      m_lpDX->TransitionResource(t, D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
      cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(t), black, 0, nullptr);
      m_lpDX->TransitionResource(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
    };
    clearTex(vsRead);
    clearTex(vsWrite);
    clearTex(display);
    // Only the WARMUP consumes a warmup frame. An adoption clear must not
    // advance that counter, or a context that changes preset twice early on
    // would skip the real warmup.
    if (P.frames < 2)
      P.frames++;
    return true;
  }

  // Video background into vsRead before warp — same as the primary path.
  // Without this, independent classic shows an empty/wrong VS and the disk
  // sampler (a "texture") plus DrawOverlays text instead of the real preset.
  if (m_nVideoInputSource != VID_SOURCE_NONE && !m_bSpoutInputOnTop) {
    DX12Texture* pVid = nullptr;
    UINT vidW = 0, vidH = 0;
    if (m_nVideoInputSource == VID_SOURCE_SPOUT) {
      // CONSUME ONLY, same as the webcam/file branch below (#213).
      //
      // This used to call UpdateSpoutInputTexture() here, on a MIRROR WORKER.
      // That function receives a Spout frame, releases and re-adopts
      // si.pReceivedTexture, rewrites dx12InputTex's resource, width, height
      // and currentState, and can reallocate its SRV -- all on state the render
      // thread owns and is using in the same frame. The webcam half of this was
      // fixed in d490234e and the Spout half was left; it is the same defect.
      if (m_spoutInput && m_spoutInput->bConnected && m_spoutInput->dx12InputTex.IsValid()) {
        pVid = &m_spoutInput->dx12InputTex;
        vidW = m_spoutInput->nSenderWidth;
        vidH = m_spoutInput->nSenderHeight;
      }
    } else if (m_nVideoInputSource == VID_SOURCE_WEBCAM || m_nVideoInputSource == VID_SOURCE_FILE) {
      // CONSUME ONLY on a mirror worker (#206).
      //
      // This used to call InitVideoCapture() and UpdateVideoCaptureTexture()
      // here. Both belong to the render thread, which already does them every
      // frame on the primary path. UpdateVideoCaptureTexture ends in
      // VideoCaptureSource::UpdateTexture, which records CopyTextureRegion
      // into dx->m_commandList -- the RENDER THREAD's open list, named
      // directly rather than through GetActiveCmdList, so no command-list
      // override could redirect it. Two threads recording into one
      // ID3D12GraphicsCommandList is undefined behaviour. InitVideoCapture
      // additionally constructs the shared m_videoCapture unique_ptr out from
      // under the render thread.
      //
      // The texture is shared, so consuming whatever the render thread last
      // uploaded is both correct and what the mirror wanted anyway.
      if (m_videoCapture && m_videoCapture->IsConnected() &&
          m_videoCapture->m_dx12Tex.IsValid()) {
        pVid = &m_videoCapture->m_dx12Tex;
        vidW = m_videoCapture->GetWidth();
        vidH = m_videoCapture->GetHeight();
      }
    }
    if (pVid && vidW && vidH) {
      m_lpDX->SetThreadCmdListOverride(cmdList);
      m_lpDX->TransitionResource(vsRead, D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
      D3D12_CPU_DESCRIPTOR_HANDLE vrtv = m_lpDX->GetRtvCpuHandle(vsRead);
      cmdList->OMSetRenderTargets(1, &vrtv, FALSE, nullptr);
      SetViewportAndScissor(cmdList, (UINT)P.w, (UINT)P.h);
      CompositeVideoInputFX(true, *pVid, vidW, vidH, &ic);
      // The shared input texture is NOT transitioned back here (#213).
      //
      // It used to go to COPY_DEST after every composite, from every recorder.
      // TransitionResource read-modify-writes DX12Texture::currentState, so N
      // mirror surfaces plus the primary all wrote one shadow variable for one
      // resource, and a barrier built from a stale read disagrees with what the
      // GPU thinks the resource is. In Release that is silent: a correct frame
      // by luck, a corrupted one, or a device removal minutes later with
      // nothing pointing back here.
      //
      // The render thread now parks it in PIXEL_SHADER_RESOURCE for the whole
      // window in which mirrors run and only moves it to COPY_DEST around its
      // own upload. Every transition a recorder still asks for is therefore a
      // no-op -- TransitionResource returns early when currentState already
      // equals the target -- so what was a write from many threads is a read.
      m_lpDX->TransitionResource(vsRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
      m_lpDX->SetThreadCmdListOverride(nullptr);
      cmdList->SetDescriptorHeaps(1, heaps);
      cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
    }
  }

  const UINT b0 = P.bindBase;
  const UINT bs = DXContext::BINDING_BLOCK_SIZE;

  // .milk2 keeps both presets on at a frozen progress. Treat that as blending
  // even if m_bBlending was cleared — otherwise only the new preset draws
  // (looks like a single MilkDrop-1 layer). All flags from the CTX states.
  const bool bBlending = c.pOldState &&
      (c.pState->m_bBlending || c.bMilk2FrozenBlend);
  const bool bOldUsesWarp = bBlending && (c.pOldState->m_nWarpPSVersion > 0);
  const bool bNewUsesWarp = (c.pState->m_nWarpPSVersion > 0);
  const bool bOldUsesComp = bBlending && (c.pOldState->m_nCompPSVersion > 0);
  const bool bNewUsesComp = (c.pState->m_nCompPSVersion > 0);
  const bool bCustomComp =
      (c.pState->m_nCompPSVersion > 0) && !c.pState->m_bAutoGenCompShader;
  const bool bOldCustomComp = bBlending && (c.pOldState->m_nCompPSVersion > 0) &&
      !c.pOldState->m_bAutoGenCompShader;

  // Bindings: block0=new warp, block1=new comp, block2=old warp, block3=old comp
  // Blur slots must use orient pyramid (primary blur is wrong aspect → soft garbage).
  auto patchOrientBlurSlots = [&](CShaderParams* params, UINT slots[32]) {
    if (!params) return;
    for (int i = 0; i < 32; i++) {
#if (NUM_BLUR_TEX >= 2)
      if (params->m_texcode[i] == TEX_BLUR1 && P.blur[1].IsValid())
        slots[i] = P.blur[1].srvIndex;
#endif
#if (NUM_BLUR_TEX >= 4)
      else if (params->m_texcode[i] == TEX_BLUR2 && P.blur[3].IsValid())
        slots[i] = P.blur[3].srvIndex;
#endif
#if (NUM_BLUR_TEX >= 6)
      else if (params->m_texcode[i] == TEX_BLUR3 && P.blur[5].IsValid())
        slots[i] = P.blur[5].srvIndex;
#endif
    }
  };

  UINT warpSlots[32], compSlots[32], oldWarpSlots[32], oldCompSlots[32];
  memset(warpSlots, 0xFF, sizeof(warpSlots));
  memset(compSlots, 0xFF, sizeof(compSlots));
  memset(oldWarpSlots, 0xFF, sizeof(oldWarpSlots));
  memset(oldCompSlots, 0xFF, sizeof(oldCompSlots));
  BuildBindingSlots(&sh.warp.params, vsRead, warpSlots,
                    nullptr, nullptr, nullptr, nullptr, nullptr, true);
  patchOrientBlurSlots(&sh.warp.params, warpSlots);
  if (bBlending && oldSh.warp.bytecodeBlob) {
    BuildBindingSlots(&oldSh.warp.params, vsRead, oldWarpSlots,
                      nullptr, nullptr, nullptr, nullptr, nullptr, true);
    patchOrientBlurSlots(&oldSh.warp.params, oldWarpSlots);
  }
  // DIAG_BINDINGS: what the MIRROR resolved from the same params.
  CaptureBindSnapshot(surf.bindSnap[BINDSNAP_WARP], &sh.warp.params, warpSlots);
  CaptureBindSnapshot(surf.bindSnap[BINDSNAP_OLDWARP], &oldSh.warp.params, oldWarpSlots);

  m_lpDX->FillSrvBindingBlock(b0 + 0 * bs, warpSlots);
  m_lpDX->FillSrvBindingBlock(b0 + 2 * bs, oldWarpSlots);

  // #121: this record's clock, rate, schedule and audio. The stages below run
  // through RenderWarpPass/RenderCompPass and get theirs from their own
  // RenderContext; this lambda binds shaders directly, so it needs its own.
  RenderContext animCtx;
  FillAnimInputs(animCtx, c);

  auto bindShader = [&](PShaderInfo* si, CState* st, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    if (si && si->CT && st) {
      ApplyShaderParams(&si->params, si->CT, st, &animCtx);
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(si->CT);
      if (ct->GetShadowSize() > 0) {
        D3D12_GPU_VIRTUAL_ADDRESS cb =
            m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
        if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);
      }
    } else {
      BYTE zeros[256] = {};
      D3D12_GPU_VIRTUAL_ADDRESS cb = m_lpDX->UploadConstantBuffer(zeros, 256);
      if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);
    }
    cmdList->SetGraphicsRootDescriptorTable(1, table);
  };

  auto drawWarpMesh = [&](D3DCOLOR cDecay, bool bCullTiles, bool bFlipCulling) {
    DrawWarpMeshIndexed(c.verts.data(), cDecay, bCullTiles, bFlipCulling, cmdList);
  };

  // ── Motion vectors: into vsRead BEFORE warp (enters feedback loop) ──
  //
  // The primary does this at milkdropfs.cpp:3303, drawing into VS[0] before the
  // warp reads it. This path did not do it at all (#366): every other injector
  // — shapes, waves, sprites, and the wave/border/darken-centre stage — was
  // given a state override by #186 and wired into the injection block below,
  // and DX12_DrawMotionVectors had no override to give it, so it was left out.
  //
  // Invisible for most presets, because most have something else that draws.
  // For a preset whose ONLY light source is motion vectors the display stayed
  // black forever while the same file rendered correctly on the primary.
  //
  // Before the warp and not in the block below, because these belong IN the
  // feedback: drawn after the warp they would be a foreground overlay that the
  // next frame never carries forward, which is a different effect.
  {
    RenderContext mv;
    FillAnimInputs(mv, c);
    FillCanvasInputs(mv, P.w, P.h, c.nFrame);
    mv.cmdList  = cmdList;
    mv.pState   = c.pState;
    mv.verts    = c.verts.data();
    mv.isMirror = true;
    m_lpDX->SetThreadCmdListOverride(cmdList);
    DX12_DrawMotionVectors(c.pState, &mv, &vsRead);
    m_lpDX->SetThreadCmdListOverride(nullptr);
  }

  // ── Warp: vsRead → vsWrite (two-pass when blending / .milk2) ──
  m_lpDX->TransitionResource(vsRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
  m_lpDX->TransitionResource(vsWrite, D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_lpDX->GetRtvCpuHandle(vsWrite);
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);
  }
  SetViewportAndScissor(cmdList, (UINT)P.w, (UINT)P.h);

  // The warp draw dispatch is shared with the primary (#15). Only the
  // descriptor tables and the mesh differ; the branch logic is a property of
  // the preset, not of the surface.
  {
    RenderContext wc;
    FillAnimInputs(wc, c);
    FillPresetPipeline(wc, c);
    FillCanvasInputs(wc, P.w, P.h, c.nFrame);
    wc.cmdList      = cmdList;
    wc.pState       = c.pState;
    wc.pOldState    = c.pOldState;
    wc.bBlending    = bBlending;
    wc.warpTable    = m_lpDX->GetSrvBlockGpuHandle(b0 + 0 * bs);
    wc.oldWarpTable = m_lpDX->GetSrvBlockGpuHandle(b0 + 2 * bs);
    wc.verts        = c.verts.data();
    wc.isMirror     = true;
    RenderWarpPass(wc);
  }

  // Every in-sequence mitigation, shared with the primary (#15). This used to
  // be a hand-copied ApplyFeedbackDamp call plus a comment asking whoever adds
  // the third mitigation to remember to add it here as well -- which is what
  // failed twice. The canvas is this context's sim size, not the engine's.
  {
    RenderContext mit;
    FillAnimInputs(mit, c);
    FillPresetPipeline(mit, c);
    FillCanvasInputs(mit, P.w, P.h, c.nFrame);
    mit.cmdList = cmdList;
    mit.canvasW = c.simW;
    mit.canvasH = c.simH;
    mit.pState = c.pState;
    mit.pOldState = c.pOldState;
    mit.isMirror = true;
    ApplyRenderMitigations(mit);
  }

  // The blur pyramid is shared with the primary (#15). This context builds its
  // OWN pyramid rather than sampling the primary's -- a blur generated at a
  // different aspect makes presets that lean on GetBlur1/2 look mushy.
  {
    RenderContext bc;
    FillAnimInputs(bc, c);
    FillPresetPipeline(bc, c);
    FillCanvasInputs(bc, P.w, P.h, c.nFrame);
    bc.cmdList         = cmdList;
    bc.pState          = c.pState;
    bc.pOldState       = c.pOldState;
    bc.bBlending       = bBlending;
    bc.blurSrc         = &vsRead;
    bc.blurSrcW        = P.w;
    bc.blurSrcH        = P.h;
    bc.blurTex         = P.blur;
    bc.blurW           = P.blurW;
    bc.blurH           = P.blurH;
    bc.highestBlurUsed = ScanHighestBlurUsed(bBlending, &bc);
    bc.isMirror        = true;
    // THIS surface's blur blocks, not the engine-wide set (#209). They sit
    // directly after the 6 blocks EnsureOrientPipeline already reserves, and
    // the rotation term is the SURFACE's own frame count -- never
    // DXContext::m_frameIndex, which belongs to the render thread.
    if (P.bindBase != UINT_MAX) {
      const UINT kBlock = DXContext::BINDING_BLOCK_SIZE;
      const UINT rot = (UINT)(P.frames % DXC_FRAME_COUNT);
      bc.blurBindBase = P.bindBase + 6 * kBlock +
                        rot * DXContext::MAX_BLUR_PASSES * kBlock;
    }
    RenderBlurPasses(bc);
  }

  // ── Inject shapes / waves / sprites (already dual-preset aware) ──
  // Shapes/waves run on the CTX states: their per-frame EEL and t-vars live
  // in the context's own VMs with per-context reg/gmegabuf storage, so the
  // old save/restore hack around the process-global regs is gone.
  {
    m_lpDX->SetThreadCmdListOverride(cmdList);
    ic.shapeVs = &vsRead;

    SetViewportAndScissor(cmdList, (UINT)P.w, (UINT)P.h);
    {
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_lpDX->GetRtvCpuHandle(vsWrite);
      cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

    // This surface's canvas, not the engine's (#121). One context, built once
    // at the top of the record and shared by every stage -- there is no longer
    // a SizeGuard behind it supplying the same values to stragglers, so a
    // stage that failed to take the context would read the PRIMARY's size.
    DX12_DrawCustomShapes(c.pState, c.pOldState, &ic);
    DX12_DrawCustomWaves(c.pState, c.pOldState, &ic);
    // The context's state, not the engine's. These two took no override
    // until #186, so with the mirror on a preset of its own they drew the
    // PRIMARY preset's wave, darken-center and borders into vsWrite -- the
    // face the next frame warps forward. Reported as a ghost of the primary
    // bleeding through, pulsing with the music, in some layers but not all:
    // exactly the stages listed here, and no others.
    DX12_DrawWave(c.audio.snd.fWave[0], c.audio.snd.fWave[1], c.pState,
                  &c.audio.snd, &ic);
    DX12_DrawSprites(c.pState, &ic);
    // Milk2 merge layer (12) burns into the feedback exactly like the primary
    // (milkdropfs.cpp:4039) — golden mirror robot's chrome structure IS this
    // sprite persisting through warp. Sprite anim state is not re-evaluated on
    // non-primary lists (DrawUserSprites' bPrimaryList gate), so the mirror
    // stamps the sprites at their current state — no double-stepping.
    if (SpritesEnabled())
      DrawUserSprites(12, cmdList, &ic);

    ic.shapeVs = nullptr;
    m_lpDX->SetThreadCmdListOverride(nullptr);

    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
  }

  m_lpDX->TransitionResource(vsWrite, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);

  // Comp TEX_VS: custom comp -> pre-warp VS, auto-gen -> current. There is no
  // GetPixel exception: in MilkDrop3, GetPixel is only a macro for
  // tex2D(sampler_main,...) and ApplyShaderParams binds m_lpVS[0]
  // unconditionally. Mirrors the primary path.
  const bool bindPreWarp = bCustomComp;
  const DX12Texture& compVsTex = bindPreWarp ? vsRead : vsWrite;
  BuildBindingSlots(&sh.comp.params, compVsTex, compSlots,
                    nullptr, nullptr, nullptr, nullptr, nullptr, true);
  patchOrientBlurSlots(&sh.comp.params, compSlots);
  m_lpDX->FillSrvBindingBlock(b0 + 1 * bs, compSlots);
  if (bBlending && oldSh.comp.bytecodeBlob) {
    const bool oldBindPreWarp = bOldCustomComp;   // no GetPixel exception; see above
    const DX12Texture& oldCompVs = oldBindPreWarp ? vsRead : vsWrite;
    BuildBindingSlots(&oldSh.comp.params, oldCompVs, oldCompSlots,
                      nullptr, nullptr, nullptr, nullptr, nullptr, true);
    patchOrientBlurSlots(&oldSh.comp.params, oldCompSlots);
    m_lpDX->FillSrvBindingBlock(b0 + 3 * bs, oldCompSlots);
  }

  // DIAG_BINDINGS: mirror comp pair, captured after the blur patch so the
  // snapshot is what actually reaches the descriptor block.
  CaptureBindSnapshot(surf.bindSnap[BINDSNAP_COMP], &sh.comp.params, compSlots);
  CaptureBindSnapshot(surf.bindSnap[BINDSNAP_OLDCOMP], &oldSh.comp.params, oldCompSlots);

  // ── Comp → display (fbA[0]); two-pass crossfade when blending / .milk2 ──
  m_lpDX->TransitionResource(display, D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_lpDX->GetRtvCpuHandle(display);
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->ClearRenderTargetView(rtv, black, 0, nullptr);
  }
  SetViewportAndScissor(cmdList, (UINT)P.w, (UINT)P.h);

  // The comp draw dispatch is shared with the primary (#15).
  {
    RenderContext cc;
    FillAnimInputs(cc, c);
    FillPresetPipeline(cc, c);
    FillCanvasInputs(cc, P.w, P.h, c.nFrame);
    cc.cmdList      = cmdList;
    cc.pState       = c.pState;
    cc.pOldState    = c.pOldState;
    cc.bBlending    = bBlending;
    cc.compTable    = m_lpDX->GetSrvBlockGpuHandle(b0 + 1 * bs);
    cc.oldCompTable = m_lpDX->GetSrvBlockGpuHandle(b0 + 3 * bs);
    cc.verts        = c.verts.data();
    cc.isMirror     = true;
    RenderCompPass(cc);
  }

  // Overlay video on the composed display (matches primary post-comp path).
  if (m_nVideoInputSource != VID_SOURCE_NONE && m_bSpoutInputOnTop) {
    DX12Texture* pVid = nullptr;
    UINT vidW = 0, vidH = 0;
    if (m_nVideoInputSource == VID_SOURCE_SPOUT) {
      if (m_spoutInput && m_spoutInput->bConnected && m_spoutInput->dx12InputTex.IsValid()) {
        pVid = &m_spoutInput->dx12InputTex;
        vidW = m_spoutInput->nSenderWidth;
        vidH = m_spoutInput->nSenderHeight;
      }
    } else if ((m_nVideoInputSource == VID_SOURCE_WEBCAM || m_nVideoInputSource == VID_SOURCE_FILE) &&
               m_videoCapture && m_videoCapture->IsConnected() && m_videoCapture->m_dx12Tex.IsValid()) {
      pVid = &m_videoCapture->m_dx12Tex;
      vidW = m_videoCapture->GetWidth();
      vidH = m_videoCapture->GetHeight();
    }
    if (pVid && vidW && vidH) {
      m_lpDX->SetThreadCmdListOverride(cmdList);
      CompositeVideoInputFX(false, *pVid, vidW, vidH, &ic);
      if (m_nVideoInputSource == VID_SOURCE_SPOUT)
        m_lpDX->TransitionResource(*pVid, D3D12_RESOURCE_STATE_COPY_DEST, cmdList);
      m_lpDX->SetThreadCmdListOverride(nullptr);
      cmdList->SetDescriptorHeaps(1, heaps);
      cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
    }
  }

  m_lpDX->TransitionResource(display, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);

  // Milk2 back layer (10) into the feedback for next frame — mirrors the
  // primary's post-comp stamp (milkdropfs.cpp:4323-4328). vsWrite is next
  // frame's warp source; without this the sprite never enters the loop.
  if (SpritesEnabled()) {
    m_lpDX->SetThreadCmdListOverride(cmdList);
    m_lpDX->TransitionResource(vsWrite, D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);
    D3D12_CPU_DESCRIPTOR_HANDLE wRtv = m_lpDX->GetRtvCpuHandle(vsWrite);
    cmdList->OMSetRenderTargets(1, &wRtv, FALSE, nullptr);
    SetViewportAndScissor(cmdList, (UINT)P.w, (UINT)P.h);
    DrawUserSprites(10, cmdList, &ic);
    m_lpDX->TransitionResource(vsWrite, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
    m_lpDX->SetThreadCmdListOverride(nullptr);
  }

  P.fbIdx = 1 - P.fbIdx;
  P.frames++;
  return true;
}

bool mdrop::Engine::BlitOrientOutputToMirror(MirrorSurface& surf,
                                             ID3D12GraphicsCommandList* cmdList,
                                             D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv,
                                             int monW, int monH)
{
  if (!cmdList || !m_lpDX || monW <= 0 || monH <= 0 || !surf.pipe.ready)
    return false;
  // Read the PUBLISHED disp[] face (fence-proven complete by the worker and
  // not rewritten for two full worker periods — triple rotation).
  const int pub = surf.publishedIdx.load(std::memory_order_acquire);
  if (pub < 0)
    return false;
  DX12Texture& src = surf.pipe.disp[pub % 3];
  if (!src.IsValid() || src.srvIndex == UINT_MAX)
    return false;

  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  m_lpDX->TransitionResource(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);

  cmdList->OMSetRenderTargets(1, &mirrorRtv, FALSE, nullptr);
  {
    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->ClearRenderTargetView(mirrorRtv, black, 0, nullptr);
  }
  SetViewportAndScissor(cmdList, (UINT)monW, (UINT)monH);
  cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());

  const UINT blitBase = surf.pipe.bindBase + 5 * DXContext::BINDING_BLOCK_SIZE;
  UINT blitSlots[32];
  for (UINT i = 0; i < 32; i++)
    blitSlots[i] = src.srvIndex;
  m_lpDX->FillSrvBindingBlock(blitBase, blitSlots);
  cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetSrvBlockGpuHandle(blitBase));

  BYTE zeros[256] = {};
  D3D12_GPU_VIRTUAL_ADDRESS cb = m_lpDX->UploadConstantBuffer(zeros, 256);
  if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);

  // Letterbox/pillarbox to preserve aspect (pipe is often 16-aligned/capped;
  // force-fill stretch made independent look anamorphically stretched).
  float nx = 1.f, ny = 1.f;
  const float srcAr = (float)src.width / (float)src.height;
  const float dstAr = (float)monW / (float)monH;
  if (srcAr > dstAr)
    ny = dstAr / srcAr;
  else if (dstAr > srcAr)
    nx = srcAr / dstAr;

  // What this panel is actually handed. A counter, not a capture -- with two
  // own-preset displays the report is "too wide", and this says whether the
  // source, the destination or the letterbox is where the width comes from.
  {
    static std::set<std::string> s_seenBlit;
    char key[192];
    sprintf(key, "%ls src=%ux%u dst=%dx%d srcAr=%.4f dstAr=%.4f nx=%.4f ny=%.4f",
            surf.device.empty() ? L"(none)" : surf.device.c_str(),
            src.width, src.height, monW, monH, srcAr, dstAr, nx, ny);
    if (s_seenBlit.insert(key).second)
      DebugLogA((std::string("blit ") + key + "\n").c_str(), LOG_INFO);
  }

  MYVERTEX blit[4];
  ZeroMemory(blit, sizeof(blit));
  const float px[4] = { -nx, nx, -nx, nx };
  const float py[4] = { ny, ny, -ny, -ny };
  const float pu[4] = { 0.f, 1.f, 0.f, 1.f };
  const float pv[4] = { 0.f, 0.f, 1.f, 1.f };
  for (int i = 0; i < 4; i++) {
    blit[i].x = px[i]; blit[i].y = py[i]; blit[i].z = 0.f;
    blit[i].Diffuse = 0xFFFFFFFFu;
    blit[i].tu = pu[i]; blit[i].tv = pv[i];
    blit[i].tu_orig = pu[i]; blit[i].tv_orig = pv[i];
  }
  m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, blit, 4, sizeof(MYVERTEX), cmdList);
  return true;
}

// ── Shadertoy render path: Buffer A → Image, no warp/blur/shapes ──
void mdrop::Engine::RenderFrameShadertoy(ID3D12GraphicsCommandList* cmdList)
{
  // Primary must never inherit portrait texsize left over from mirror orient pass.
  RestorePrimaryTexSizeFromVS();

  int fbRead = m_nFeedbackIdx;
  int fbWrite = 1 - m_nFeedbackIdx;

  // Clear feedback buffers on the first 2 frames of a Shadertoy preset.
  // This ensures no garbage VRAM data leaks into temporal accumulation.
  // Many shaders also self-clear via "if (iFrame < 2) fragColor = 0;" but
  // we do it here as a safety net for shaders that don't.
  int stFrame = GetFrame() - m_nShadertoyStartFrame;

  // Update audio texture (FFT + waveform) for this frame
  UpdateAudioTexture();

  // Truncate binding diagnostics file on first Shadertoy frame (Verbose only)
  if (DLOG_DIAG_ENABLED() && stFrame == 0) {
    DebugLogDiagTruncate(L"diag_bindings.txt");
  }

  // One-time diagnostics on first Shadertoy frame
  if (stFrame == 0) {
    DLOG_INFO("Shadertoy: fb[0]=%s (%ux%u fmt=%u) fb[1]=%s (%ux%u fmt=%u)",
      m_dx12Feedback[0].IsValid() ? "OK" : "INVALID",
      m_dx12Feedback[0].width, m_dx12Feedback[0].height, (UINT)m_dx12Feedback[0].format,
      m_dx12Feedback[1].IsValid() ? "OK" : "INVALID",
      m_dx12Feedback[1].width, m_dx12Feedback[1].height, (UINT)m_dx12Feedback[1].format);
    DLOG_INFO("Shadertoy: bufferA_PSO=%s comp_PSO=%s hasBufferA=%d compUsesFeedback=%d compUsesImageFeedback=%d",
      m_dx12BufferAPSO ? "OK" : "NULL", m_dx12CompPSO ? "OK" : "NULL",
      (int)m_bHasBufferA, (int)m_bCompUsesFeedback, (int)m_bCompUsesImageFeedback);
    DLOG_INFO("Shadertoy: texSize=%dx%d fbRead=%d fbWrite=%d",
      m_nTexSizeX, m_nTexSizeY, fbRead, fbWrite);

    // Log shader status (no HUD overlay — VJ use case)
    DLOG_INFO("Shadertoy: A=%s B=%s C=%s D=%s Img=%s (%dx%d)",
      m_dx12BufferAPSO ? "OK" : "--",
      m_dx12BufferBPSO ? "OK" : "--",
      m_dx12BufferCPSO ? "OK" : "--",
      m_dx12BufferDPSO ? "OK" : "--",
      m_dx12CompPSO ? "OK" : "--",
      m_nTexSizeX, m_nTexSizeY);
  }

  // Clear for several frames after preset load so any prior garbage / NaN wedges
  // cannot cascade through feedback as layered triangles.
  if (stFrame < 6) {
    float black[] = { 0.f, 0.f, 0.f, 0.f };
    for (int i = 0; i < 2; i++) {
      if (m_dx12Feedback[i].IsValid()) {
        m_lpDX->TransitionResource(m_dx12Feedback[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(m_dx12Feedback[i]), black, 0, nullptr);
        m_lpDX->TransitionResource(m_dx12Feedback[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      if (m_dx12FeedbackB[i].IsValid()) {
        m_lpDX->TransitionResource(m_dx12FeedbackB[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(m_dx12FeedbackB[i]), black, 0, nullptr);
        m_lpDX->TransitionResource(m_dx12FeedbackB[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      if (m_dx12FeedbackC[i].IsValid()) {
        m_lpDX->TransitionResource(m_dx12FeedbackC[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(m_dx12FeedbackC[i]), black, 0, nullptr);
        m_lpDX->TransitionResource(m_dx12FeedbackC[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      if (m_dx12FeedbackD[i].IsValid()) {
        m_lpDX->TransitionResource(m_dx12FeedbackD[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(m_dx12FeedbackD[i]), black, 0, nullptr);
        m_lpDX->TransitionResource(m_dx12FeedbackD[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      if (m_dx12ImageFeedback[i].IsValid()) {
        m_lpDX->TransitionResource(m_dx12ImageFeedback[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(m_dx12ImageFeedback[i]), black, 0, nullptr);
        m_lpDX->TransitionResource(m_dx12ImageFeedback[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
  }

  // Set up descriptor heaps and root signature (shared by all passes)
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  // Build binding slots for Buffer A, B, C, D and comp/Image passes
  // Shadertoy sequential execution: A→B→C→D→Image
  // Each buffer reads: already-rendered buffers from THIS frame (fbWrite),
  //                    not-yet-rendered buffers from PREVIOUS frame (fbRead),
  //                    own previous output from PREVIOUS frame (fbRead for self-feedback).
  UINT warpSlots[32], bufferASlots[32], bufferBSlots[32], bufferCSlots[32], bufferDSlots[32], compSlots[32];
  memset(warpSlots, 0xFF, sizeof(warpSlots));  // warp unused in Shadertoy mode
  memset(bufferASlots, 0xFF, sizeof(bufferASlots));
  memset(bufferBSlots, 0xFF, sizeof(bufferBSlots));
  memset(bufferCSlots, 0xFF, sizeof(bufferCSlots));
  memset(bufferDSlots, 0xFF, sizeof(bufferDSlots));

  {
    const DX12Texture* imgFbRead = m_bCompUsesImageFeedback ? &m_dx12ImageFeedback[fbRead] : nullptr;
    bool hasAnyBuffer = m_bHasBufferA || m_bHasBufferB || m_bHasBufferC || m_bHasBufferD;

    // Buffer A: reads all from previous frame
    if (m_bHasBufferA) {
      BuildBindingSlots(&m_shaders.bufferA.params, m_dx12VS[1], bufferASlots,
                        &m_dx12Feedback[fbRead], imgFbRead,
                        &m_dx12FeedbackB[fbRead], &m_dx12FeedbackC[fbRead], &m_dx12FeedbackD[fbRead]);
    }

    // Buffer B: reads A from this frame (fbWrite), own + C/D from previous frame (fbRead)
    if (m_bHasBufferB) {
      BuildBindingSlots(&m_shaders.bufferB.params, m_dx12VS[1], bufferBSlots,
                        &m_dx12Feedback[fbWrite], imgFbRead,
                        &m_dx12FeedbackB[fbRead], &m_dx12FeedbackC[fbRead], &m_dx12FeedbackD[fbRead]);
    }

    // Buffer C: reads A+B from this frame (fbWrite), own + D from previous frame (fbRead)
    if (m_bHasBufferC) {
      BuildBindingSlots(&m_shaders.bufferC.params, m_dx12VS[1], bufferCSlots,
                        &m_dx12Feedback[fbWrite], imgFbRead,
                        &m_dx12FeedbackB[fbWrite], &m_dx12FeedbackC[fbRead], &m_dx12FeedbackD[fbRead]);
    }

    // Buffer D: reads A+B+C from this frame (fbWrite), own from previous frame (fbRead)
    if (m_bHasBufferD) {
      BuildBindingSlots(&m_shaders.bufferD.params, m_dx12VS[1], bufferDSlots,
                        &m_dx12Feedback[fbWrite], imgFbRead,
                        &m_dx12FeedbackB[fbWrite], &m_dx12FeedbackC[fbWrite], &m_dx12FeedbackD[fbRead]);
    }

    // Image: reads all from this frame (fbWrite) when any buffer exists
    if (hasAnyBuffer) {
      BuildBindingSlots(&m_shaders.comp.params, m_dx12VS[1], compSlots,
                        &m_dx12Feedback[fbWrite], imgFbRead,
                        &m_dx12FeedbackB[fbWrite], &m_dx12FeedbackC[fbWrite], &m_dx12FeedbackD[fbWrite]);
    } else {
      BuildBindingSlots(&m_shaders.comp.params, m_dx12VS[1], compSlots,
                        &m_dx12Feedback[fbRead], imgFbRead,
                        &m_dx12FeedbackB[fbRead], &m_dx12FeedbackC[fbRead], &m_dx12FeedbackD[fbRead]);
    }
  }
  {
    UINT oldWarpSlots[32], oldCompSlots[32];
    memset(oldWarpSlots, 0xFF, sizeof(oldWarpSlots));
    memset(oldCompSlots, 0xFF, sizeof(oldCompSlots));
    m_lpDX->UpdatePerFrameBindings(warpSlots, bufferASlots, bufferBSlots, bufferCSlots, bufferDSlots, compSlots,
                                   oldWarpSlots, oldCompSlots);
  }

  // ── Buffer A pass: render to feedback[fbWrite] ──
  if (m_bHasBufferA && m_dx12BufferAPSO && m_dx12Feedback[fbWrite].IsValid()) {
    DX12Texture& fbWriteTex = m_dx12Feedback[fbWrite];

    m_lpDX->TransitionResource(fbWriteTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE fbRtv = m_lpDX->GetRtvCpuHandle(fbWriteTex);
    cmdList->OMSetRenderTargets(1, &fbRtv, FALSE, nullptr);

    // ALWAYS clear write target before draw. Ping-pong RTs keep stale pixels if
    // any region is not covered; those stack with feedback into cascading triangles.
    {
      float black[] = { 0.f, 0.f, 0.f, 0.f };
      cmdList->ClearRenderTargetView(fbRtv, black, 0, nullptr);
    }

    SetViewportAndScissor(cmdList, fbWriteTex.width, fbWriteTex.height);
    cmdList->SetPipelineState(m_dx12BufferAPSO.Get());

    // Apply Buffer A shader params and upload constant buffer
    PShaderInfo* bufASI = &m_shaders.bufferA;
    if (bufASI->CT) {
      ApplyShaderParams(&bufASI->params, bufASI->CT, m_pState);
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(bufASI->CT);
      if (ct->GetShadowSize() > 0) {
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
            m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
        if (cbAddr)
          cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
      }
    }

    // Bind Buffer A descriptor table (feedback[read] for self-referencing)
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBufferABindingGpuHandle());

    MYVERTEX bufAStrip[4];
    FillShadertoyFullscreenStrip(bufAStrip);
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, bufAStrip, 4, sizeof(MYVERTEX));

    // Transition feedback[write] to SRV so Image pass can read it
    m_lpDX->TransitionResource(fbWriteTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // ── Buffer B pass: render to feedbackB[fbWrite] ──
  if (m_bHasBufferB && m_dx12BufferBPSO && m_dx12FeedbackB[fbWrite].IsValid()) {
    DX12Texture& fbBWriteTex = m_dx12FeedbackB[fbWrite];

    m_lpDX->TransitionResource(fbBWriteTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE fbBRtv = m_lpDX->GetRtvCpuHandle(fbBWriteTex);
    cmdList->OMSetRenderTargets(1, &fbBRtv, FALSE, nullptr);
    {
      float black[] = { 0.f, 0.f, 0.f, 0.f };
      cmdList->ClearRenderTargetView(fbBRtv, black, 0, nullptr);
    }

    SetViewportAndScissor(cmdList, fbBWriteTex.width, fbBWriteTex.height);
    cmdList->SetPipelineState(m_dx12BufferBPSO.Get());

    // Apply Buffer B shader params and upload constant buffer
    PShaderInfo* bufBSI = &m_shaders.bufferB;
    if (bufBSI->CT) {
      ApplyShaderParams(&bufBSI->params, bufBSI->CT, m_pState);
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(bufBSI->CT);
      if (ct->GetShadowSize() > 0) {
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
            m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
        if (cbAddr)
          cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
      }
    }

    // Bind Buffer B descriptor table
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBufferBBindingGpuHandle());

    MYVERTEX bufBStrip[4];
    FillShadertoyFullscreenStrip(bufBStrip);
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, bufBStrip, 4, sizeof(MYVERTEX));

    // Transition feedbackB[write] to SRV so Image pass can read it
    m_lpDX->TransitionResource(fbBWriteTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // ── Buffer C pass: render to feedbackC[fbWrite] ──
  if (m_bHasBufferC && m_dx12BufferCPSO && m_dx12FeedbackC[fbWrite].IsValid()) {
    DX12Texture& fbCWriteTex = m_dx12FeedbackC[fbWrite];

    m_lpDX->TransitionResource(fbCWriteTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE fbCRtv = m_lpDX->GetRtvCpuHandle(fbCWriteTex);
    cmdList->OMSetRenderTargets(1, &fbCRtv, FALSE, nullptr);
    {
      float black[] = { 0.f, 0.f, 0.f, 0.f };
      cmdList->ClearRenderTargetView(fbCRtv, black, 0, nullptr);
    }

    SetViewportAndScissor(cmdList, fbCWriteTex.width, fbCWriteTex.height);
    cmdList->SetPipelineState(m_dx12BufferCPSO.Get());

    PShaderInfo* bufCSI = &m_shaders.bufferC;
    if (bufCSI->CT) {
      ApplyShaderParams(&bufCSI->params, bufCSI->CT, m_pState);
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(bufCSI->CT);
      if (ct->GetShadowSize() > 0) {
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
            m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
        if (cbAddr)
          cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
      }
    }

    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBufferCBindingGpuHandle());

    MYVERTEX bufCStrip[4];
    FillShadertoyFullscreenStrip(bufCStrip);
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, bufCStrip, 4, sizeof(MYVERTEX));

    m_lpDX->TransitionResource(fbCWriteTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // ── Buffer D pass: render to feedbackD[fbWrite] ──
  if (m_bHasBufferD && m_dx12BufferDPSO && m_dx12FeedbackD[fbWrite].IsValid()) {
    DX12Texture& fbDWriteTex = m_dx12FeedbackD[fbWrite];

    m_lpDX->TransitionResource(fbDWriteTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE fbDRtv = m_lpDX->GetRtvCpuHandle(fbDWriteTex);
    cmdList->OMSetRenderTargets(1, &fbDRtv, FALSE, nullptr);
    {
      float black[] = { 0.f, 0.f, 0.f, 0.f };
      cmdList->ClearRenderTargetView(fbDRtv, black, 0, nullptr);
    }

    SetViewportAndScissor(cmdList, fbDWriteTex.width, fbDWriteTex.height);
    cmdList->SetPipelineState(m_dx12BufferDPSO.Get());

    PShaderInfo* bufDSI = &m_shaders.bufferD;
    if (bufDSI->CT) {
      ApplyShaderParams(&bufDSI->params, bufDSI->CT, m_pState);
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(bufDSI->CT);
      if (ct->GetShadowSize() > 0) {
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
            m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
        if (cbAddr)
          cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
      }
    }

    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBufferDBindingGpuHandle());

    MYVERTEX bufDStrip[4];
    FillShadertoyFullscreenStrip(bufDStrip);
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, bufDStrip, 4, sizeof(MYVERTEX));

    m_lpDX->TransitionResource(fbDWriteTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // ── Image/Comp pass ──
  // When Image self-feedback is active, render to m_dx12ImageFeedback[fbWrite] (FLOAT32),
  // then blit to backbuffer. Otherwise render directly to backbuffer.
  {
    m_lpDX->TransitionResource(m_dx12VS[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    bool bImageToFeedback = m_bCompUsesImageFeedback && m_dx12ImageFeedback[fbWrite].IsValid();

    if (bImageToFeedback) {
      // Render Image pass to FLOAT32 feedback buffer (preserves HDR precision for self-feedback)
      DX12Texture& imgFbWrite = m_dx12ImageFeedback[fbWrite];
      m_lpDX->TransitionResource(imgFbWrite, D3D12_RESOURCE_STATE_RENDER_TARGET);
      D3D12_CPU_DESCRIPTOR_HANDLE imgRtv = m_lpDX->GetRtvCpuHandle(imgFbWrite);
      cmdList->OMSetRenderTargets(1, &imgRtv, FALSE, nullptr);
      SetViewportAndScissor(cmdList, imgFbWrite.width, imgFbWrite.height);
    } else {
      // Render directly to backbuffer (UNORM)
      D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
      bbRtv.ptr += (SIZE_T)m_lpDX->m_frameIndex * m_lpDX->m_rtvDescriptorSize;
      cmdList->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
      SetViewportAndScissor(cmdList, m_lpDX->m_client_width, m_lpDX->m_client_height);
    }

    // The PSO must be the one built for the format we just bound: FLOAT32 when
    // Image goes to its own feedback, UNORM when it goes straight to the
    // backbuffer. Binding the UNORM PSO to the FLOAT32 target is invalid D3D12.
    ID3D12PipelineState* imgPso =
        (bImageToFeedback && m_dx12CompFloatPSO) ? m_dx12CompFloatPSO.Get()
                                                 : m_dx12CompPSO.Get();
    if (imgPso) {
      cmdList->SetPipelineState(imgPso);
    } else {
      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    }

    // Apply comp/Image shader params and upload constant buffer
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

    // Bind comp descriptor table (feedback[write] = Buffer A's output as iChannel0)
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetCompBindingGpuHandle());

    MYVERTEX imgStrip[4];
    FillShadertoyFullscreenStrip(imgStrip, true); // Image: Shadertoy y-up
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, imgStrip, 4, sizeof(MYVERTEX));

    // ── Blit Image feedback to backbuffer ──
    // When Image was rendered to FLOAT32 feedback buffer, copy it to the UNORM backbuffer
    // using a textured quad draw (can't CopyResource across different formats).
    if (bImageToFeedback) {
      DX12Texture& imgFbWrite = m_dx12ImageFeedback[fbWrite];
      m_lpDX->TransitionResource(imgFbWrite, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

      // Switch render target to backbuffer
      D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
      bbRtv.ptr += (SIZE_T)m_lpDX->m_frameIndex * m_lpDX->m_rtvDescriptorSize;
      cmdList->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
      SetViewportAndScissor(cmdList, m_lpDX->m_client_width, m_lpDX->m_client_height);

      // Use simple textured quad PSO (passthrough — no shader effects)
      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());

      // Bind Image feedback texture as t0
      cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBindingBlockGpuHandle(imgFbWrite));

      // Zero CBV (no shader params needed for simple blit)
      BYTE zeros[256] = {};
      D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
      if (cbAddr)
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);

      MYVERTEX blitStrip[4];
      FillShadertoyFullscreenStrip(blitStrip);
      m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, blitStrip, 4, sizeof(MYVERTEX));
    }
  }

  // ── Draw behind-text sprites (layer 0) on the backbuffer ──
  if (SpritesEnabled())
    DrawUserSprites(0);

  // ── Display active supertexts on the backbuffer ──
  // Independent mirrors snapshot this BB and re-draw messages at panel size.
  if (MessagesEnabled() && !AnyIndependentMirrorEnabled()) {
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      // The primary draws only its OWN (#255). The slots are one pool shared
      // with every display running its own message loop, so without this the
      // main window would show theirs as well as its own.
      if (m_supertexts[i].nOwnerDisplay != -1)
        continue;
      if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
        float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
        if (fProgress <= 1.0f) {
          ShowSongTitleAnim(GetWidth(), GetHeight(), min(fProgress, 0.9999f), i);
        }
      }
    }
  }

  // ── Draw front sprites (layer 1) on the backbuffer ──
  if (SpritesEnabled())
    DrawUserSprites(1);

  // Mark diagnostics as logged
  if (!m_bPresetDiagLogged && GetTime() - m_fPresetStartTime >= 0.0f)
    m_bPresetDiagLogged = true;
}

void mdrop::Engine::DX12_RenderWarpAndComposite()
{
  if (!m_lpDX || !m_lpDX->m_device || !m_lpDX->m_commandList)
    return;

  auto* cmdList = m_lpDX->m_commandList.Get();

  // Deferred PSO creation: safe here because previous frame's command list
  // has already been submitted via ExecuteCommandLists in EndFrame.
  if (m_bDX12PSOsDirty) {
    CreateDX12PresetPSOs();
    m_bDX12PSOsDirty = false;
  }

  // ── First-frame: clear VS0 to black ──
  if (m_nFramesSinceResize == 0) {
    m_lpDX->TransitionResource(m_dx12VS[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    float black[] = { 0.f, 0.f, 0.f, 1.f };
    cmdList->ClearRenderTargetView(m_lpDX->GetRtvCpuHandle(m_dx12VS[0]), black, 0, nullptr);
    m_lpDX->TransitionResource(m_dx12VS[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // ── Render pending supertext strings to DX12 title textures ──
  for (int i = 0; i < NUM_SUPERTEXTS; i++) {
    if (m_supertexts[i].fStartTime != -1.0f && m_supertexts[i].bRedrawSuperText) {
      if (!RenderStringToTitleTexture(i))
        m_supertexts[i].fStartTime = -1.0f;
      m_supertexts[i].bRedrawSuperText = false;
    }
  }

  // Per-frame, in EVERY mode (#208).
  //
  // The classic path re-assigns the real depth from ScanHighestBlurUsed below,
  // so for it this is redundant. It is not redundant for the shadertoy branch
  // immediately after, which RETURNS before that scan while
  // RenderFrameShadertoy's ApplyShaderParams calls carry no RenderContext --
  // so they clear the primary-only guard and accumulate through max() with
  // nothing ever putting the value back. That left the member session-scoped
  // on a .milk3 preset, which is the exact shape #208 was filed about.
  m_nHighestBlurTexUsedThisFrame = 0;

  // ── Shadertoy pipeline: skip warp/blur/shapes entirely ──
  if (m_bShadertoyMode) {
    RenderFrameShadertoy(cmdList);
    // Must set here: early return skips the normal end-of-frame diag latch.
    // Without this, Verbose mode rewrote diag_bindings.txt every frame forever.
    if (!m_bPresetDiagLogged && GetTime() - m_fPresetStartTime >= 0.0f)
      m_bPresetDiagLogged = true;
    return;
  }

  // ── Video Input: BACKGROUND layer ──
  // Draw onto VS[0] before warp so video feeds through the preset's warp distortion
  if (m_nVideoInputSource != VID_SOURCE_NONE && !m_bSpoutInputOnTop) {
    bool hasFrame = false;
    DX12Texture* pTex = nullptr;
    UINT srcW = 0, srcH = 0;

    if (m_nVideoInputSource == VID_SOURCE_SPOUT) {
      UpdateSpoutInputTexture();
      if (m_spoutInput && m_spoutInput->bConnected && m_spoutInput->dx12InputTex.IsValid()) {
        pTex = &m_spoutInput->dx12InputTex;
        srcW = m_spoutInput->nSenderWidth;
        srcH = m_spoutInput->nSenderHeight;
        hasFrame = true;
      }
    } else if (m_nVideoInputSource == VID_SOURCE_WEBCAM || m_nVideoInputSource == VID_SOURCE_FILE) {
      // Lazy-init: create capture source on first render (startup from saved settings)
      if (!m_videoCapture)
        InitVideoCapture();
      if (m_videoCapture && m_videoCapture->IsConnected()) {
        UpdateVideoCaptureTexture();
        if (m_videoCapture->m_dx12Tex.IsValid()) {
          pTex = &m_videoCapture->m_dx12Tex;
          srcW = m_videoCapture->GetWidth();
          srcH = m_videoCapture->GetHeight();
          hasFrame = true;
        }
      }
    }

    if (hasFrame && pTex) {
      m_lpDX->TransitionResource(m_dx12VS[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_lpDX->GetRtvCpuHandle(m_dx12VS[0]);
      cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      SetViewportAndScissor(cmdList, m_nTexSizeX, m_nTexSizeY);
      CompositeVideoInputFX(true, *pTex, srcW, srcH);
      if (m_nVideoInputSource == VID_SOURCE_SPOUT)
        m_lpDX->TransitionResource(*pTex, D3D12_RESOURCE_STATE_COPY_DEST);
      m_lpDX->TransitionResource(m_dx12VS[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
  }

  // ── Motion vectors: draw into VS0 BEFORE warp (enters feedback loop) ──
  // DX9 draws these at milkdropfs.cpp line 1059, before warp reads VS0.
  DX12_DrawMotionVectors();

  // Feedback ping-pong indices (used by warp bindings, Buffer A pass, and comp pass)
  int fbRead = m_nFeedbackIdx;
  int fbWrite = 1 - m_nFeedbackIdx;

  // ── Warp pass: draw mesh from VS0 into VS1 ──
  {
    m_lpDX->TransitionResource(m_dx12VS[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_lpDX->TransitionResource(m_dx12VS[1], D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_lpDX->GetRtvCpuHandle(m_dx12VS[1]);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Clear VS[1] to black before warp mesh draws.
    // Prevents stale pixels from the ping-pong swap persisting if the mesh
    // doesn't cover every texel (rounding, edge cases).
    // Alpha=1.0: DX9 used X8R8G8B8 (no alpha). Keep alpha at 1.0 so shapes/waves
    // that sample VS don't pick up transparent pixels.
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    SetViewportAndScissor(cmdList, m_nTexSizeX, m_nTexSizeY);

    // Set descriptor heaps, root sig, PSO
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

    // Build full 16-slot binding arrays (VS + blur + noise + disk textures)
    // Warp reads VS[0] (previous frame's warp+shapes, after end-of-frame swap).
    //
    // Comp shader TEX_VS binding: DX9 ApplyShaderParams (line 4735) ALWAYS binds
    // m_lpVS[0] for TEX_VS. This means custom comp shaders read the WARP INPUT
    // (previous frame), not the warp output. The warp output (VS[1]) contains
    // current-frame decay/darkening that should NOT feed into the comp shader —
    // it only feeds back through the next frame's warp pass via the end-of-frame swap.
    // DX9 ShowToUser_NoShaders (no comp shader) explicitly reads m_lpVS[1] (line 4907).
    //
    // DX12 mapping:
    //   Custom comp shader → TEX_VS binds VS[0] (matches ApplyShaderParams)
    //   Auto-gen comp shader (no [comp_shader] section) → TEX_VS binds VS[1] (matches ShowToUser_NoShaders)
    // Note: m_nCompPSVersion > 0 is true for BOTH user-written and auto-generated comp shaders
    // (auto-gen sets it to MD2_PS_2_0 so the shader gets compiled). Use m_bAutoGenCompShader
    // to distinguish: auto-gen comp shaders read VS[1] (post-warp + shapes), user-written
    // comp shaders read VS[0] (pre-warp previous frame, matching DX9 ApplyShaderParams).
    bool bNewUsesCompShader = (m_pState->m_nCompPSVersion > 0) && !m_pState->m_bAutoGenCompShader;
    // A GetPixel/GetMain-conditional redirect to VS[1] used to sit here. It is
    // wrong, per MilkDrop3's own source: GetPixel is only
    //     #define GetPixel(uv) (tex2D(sampler_main,uv).xyz)
    // so it carries no binding semantics, and ApplyShaderParams binds
    // m_lpVS[0] unconditionally -- there is not one GetPixel-conditional VS
    // switch in the reference. The rule really is custom comp -> VS[0],
    // auto-gen comp -> VS[1], exactly as the lines above express it.
    // Affected 8890 .milk presets whose comp shader mentions GetPixel.
    const DX12Texture& compVsTex = bNewUsesCompShader ? m_dx12VS[0] : m_dx12VS[1];

    UINT warpSlots[32], bufferASlots[32], bufferBSlots[32], bufferCSlots[32], bufferDSlots[32], compSlots[32];
    UINT oldWarpSlots[32], oldCompSlots[32];
    memset(bufferBSlots, 0xFF, sizeof(bufferBSlots));  // Buffer B/C/D unused in non-Shadertoy mode
    memset(bufferCSlots, 0xFF, sizeof(bufferCSlots));
    memset(bufferDSlots, 0xFF, sizeof(bufferDSlots));
    memset(oldWarpSlots, 0xFF, sizeof(oldWarpSlots));
    memset(oldCompSlots, 0xFF, sizeof(oldCompSlots));
    BuildBindingSlots(&m_shaders.warp.params, m_dx12VS[0], warpSlots);

    // Buffer A reads feedback[read] (own previous output), comp reads feedback[write] (Buffer A's current output)
    if (m_bHasBufferA) {
      BuildBindingSlots(&m_shaders.bufferA.params, m_dx12VS[1], bufferASlots, &m_dx12Feedback[fbRead]);
      BuildBindingSlots(&m_shaders.comp.params, compVsTex, compSlots, &m_dx12Feedback[fbWrite]);
    } else {
      memset(bufferASlots, 0xFF, sizeof(bufferASlots));  // all UINT_MAX (unused)
      BuildBindingSlots(&m_shaders.comp.params, compVsTex, compSlots, &m_dx12Feedback[fbRead]);
    }

    // Build old shader bindings during blend transitions
    if (m_pState->m_bBlending && m_OldShaders.warp.bytecodeBlob)
      BuildBindingSlots(&m_OldShaders.warp.params, m_dx12VS[0], oldWarpSlots);
    if (m_pState->m_bBlending && m_OldShaders.comp.bytecodeBlob) {
      bool bOldUsesCompShader = (m_pOldState && m_pOldState->m_nCompPSVersion > 0 && !m_pOldState->m_bAutoGenCompShader);
      // Do NOT apply the GetPixel/GetMain VS[1] redirect to the OLD comp.
      // Preset 1 of a .milk2 commonly calls GetPixel; rebinding it to VS[1]
      // made the whole PRESET1 region of a frozen blend render black
      // (measured: solid-colour probe at blending_progress=0 was black instead
      // of preset 1, and Mandala2 at 0 went black instead of dark red).
      // The redirect still applies to the NEW comp above.
      const DX12Texture& oldCompVsTex = bOldUsesCompShader ? m_dx12VS[0] : m_dx12VS[1];
      BuildBindingSlots(&m_OldShaders.comp.params, oldCompVsTex, oldCompSlots);
    }

    // DIAG_BINDINGS: what the PRIMARY resolved, for diffing against the mirror.
    CaptureBindSnapshot(m_bindSnapPrimary[BINDSNAP_WARP], &m_shaders.warp.params, warpSlots);
    CaptureBindSnapshot(m_bindSnapPrimary[BINDSNAP_COMP], &m_shaders.comp.params, compSlots);
    CaptureBindSnapshot(m_bindSnapPrimary[BINDSNAP_OLDWARP], &m_OldShaders.warp.params, oldWarpSlots);
    CaptureBindSnapshot(m_bindSnapPrimary[BINDSNAP_OLDCOMP], &m_OldShaders.comp.params, oldCompSlots);

    m_lpDX->UpdatePerFrameBindings(warpSlots, bufferASlots, bufferBSlots, bufferCSlots, bufferDSlots, compSlots,
                                   oldWarpSlots, oldCompSlots);

    // Diagnostic: log binding slots once per preset load
    if (!m_bPresetDiagLogged && GetTime() - m_fPresetStartTime >= 0.0f) {
      // Log comp shader's m_texcode and resulting binding slots
      {
        CShaderParams* cp = &m_shaders.comp.params;
        DLOG_VERBOSE("DIAG CompBindings texcode: [%d,%d,%d,%d, %d,%d,%d,%d, %d,%d,%d,%d, %d,%d,%d,%d]",
                cp->m_texcode[0], cp->m_texcode[1], cp->m_texcode[2], cp->m_texcode[3],
                cp->m_texcode[4], cp->m_texcode[5], cp->m_texcode[6], cp->m_texcode[7],
                cp->m_texcode[8], cp->m_texcode[9], cp->m_texcode[10], cp->m_texcode[11],
                cp->m_texcode[12], cp->m_texcode[13], cp->m_texcode[14], cp->m_texcode[15]);
        DLOG_VERBOSE("DIAG CompBindings slots:   [%u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u]",
                compSlots[0], compSlots[1], compSlots[2], compSlots[3],
                compSlots[4], compSlots[5], compSlots[6], compSlots[7],
                compSlots[8], compSlots[9], compSlots[10], compSlots[11],
                compSlots[12], compSlots[13], compSlots[14], compSlots[15]);
        DLOG_VERBOSE("DIAG CompBindings blur SRVs: blur[1].srv=%u blur[3].srv=%u blur[5].srv=%u VS[1].srv=%u",
                m_dx12Blur[1].srvIndex, m_dx12Blur[3].srvIndex, m_dx12Blur[5].srvIndex, m_dx12VS[1].srvIndex);
      }
      // Log warp shader's m_texcode
      {
        CShaderParams* wp = &m_shaders.warp.params;
        DLOG_VERBOSE("DIAG WarpBindings texcode: [%d,%d,%d,%d, %d,%d,%d,%d, %d,%d,%d,%d, %d,%d,%d,%d]",
                wp->m_texcode[0], wp->m_texcode[1], wp->m_texcode[2], wp->m_texcode[3],
                wp->m_texcode[4], wp->m_texcode[5], wp->m_texcode[6], wp->m_texcode[7],
                wp->m_texcode[8], wp->m_texcode[9], wp->m_texcode[10], wp->m_texcode[11],
                wp->m_texcode[12], wp->m_texcode[13], wp->m_texcode[14], wp->m_texcode[15]);
        DLOG_VERBOSE("DIAG WarpBindings slots:   [%u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u]",
                warpSlots[0], warpSlots[1], warpSlots[2], warpSlots[3],
                warpSlots[4], warpSlots[5], warpSlots[6], warpSlots[7],
                warpSlots[8], warpSlots[9], warpSlots[10], warpSlots[11],
                warpSlots[12], warpSlots[13], warpSlots[14], warpSlots[15]);
      }
    }

    // Diagnostic: log rotation matrix evolution over multiple frames
    {
      static int diagFrameCount = 0;
      static bool diagWasLogged = true;
      if (m_bPresetDiagLogged) { diagWasLogged = true; }
      if (!m_bPresetDiagLogged && diagWasLogged) { diagFrameCount = 0; diagWasLogged = false; }
      int presetFrame = diagFrameCount++;
      if (presetFrame == 0 || presetFrame == 1 || presetFrame == 5 ||
          presetFrame == 10 || presetFrame == 30 || presetFrame == 60 ||
          presetFrame == 120) {
        double* regs = NSEEL_getglobalregs();
        DLOG_VERBOSE("DIAG frame=%d q7=%.4f q8=%.4f q14=%.6f q16=%.4f",
                presetFrame, (float)*m_pState->var_pf_q[6], (float)*m_pState->var_pf_q[7],
                (float)*m_pState->var_pf_q[13], (float)*m_pState->var_pf_q[15]);
        DLOG_VERBOSE("DIAG frame=%d q20-28(rot): %.6f %.6f %.6f | %.6f %.6f %.6f | %.6f %.6f %.6f",
                presetFrame,
                (float)*m_pState->var_pf_q[19], (float)*m_pState->var_pf_q[20], (float)*m_pState->var_pf_q[21],
                (float)*m_pState->var_pf_q[22], (float)*m_pState->var_pf_q[23], (float)*m_pState->var_pf_q[24],
                (float)*m_pState->var_pf_q[25], (float)*m_pState->var_pf_q[26], (float)*m_pState->var_pf_q[27]);
        DLOG_VERBOSE("DIAG frame=%d reg20-28: %.6f %.6f %.6f | %.6f %.6f %.6f | %.6f %.6f %.6f",
                presetFrame,
                (float)regs[20], (float)regs[21], (float)regs[22],
                (float)regs[23], (float)regs[24], (float)regs[25],
                (float)regs[26], (float)regs[27], (float)regs[28]);
        DLOG_VERBOSE("DIAG frame=%d q4-6(pos): %.4f %.4f %.4f q10=%.6f",
                presetFrame,
                (float)*m_pState->var_pf_q[3], (float)*m_pState->var_pf_q[4], (float)*m_pState->var_pf_q[5],
                (float)*m_pState->var_pf_q[9]);
      }
    }

    // Helper: bind shader constant buffer and descriptor table, then draw warp mesh.
    // bCullTiles: skip fully-transparent or fully-opaque tiles during blend.
    // The warp draw dispatch is shared with the mirror worker (#15). Only the
    // descriptor tables and the mesh differ; the branch logic is a property of
    // the preset, not of the surface.
    //
    // This also closes a gap that existed here and not in the mirror: the old
    // second pass drew nothing at all when the preset used a warp shader and
    // m_dx12WarpBlendPSO had not been built, so the incoming preset vanished
    // mid-blend. See RenderWarpPass.
    {
      RenderContext wc;
      FillAnimInputs(wc);
      FillPresetPipeline(wc);
      wc.cmdList      = cmdList;
      wc.pState       = m_pState;
      wc.pOldState    = m_pOldState;
      wc.bBlending    = m_pState->m_bBlending;
      wc.warpTable    = m_lpDX->GetWarpBindingGpuHandle();
      wc.oldWarpTable = m_lpDX->GetOldWarpBindingGpuHandle();
      wc.verts        = m_verts;
      wc.isMirror     = false;
      RenderWarpPass(wc);
    }

    // Every in-sequence mitigation, shared with the mirror worker (#15).
    // Here and not later: VS1 currently holds ONLY the warped history, and
    // that is the only thing that should be attenuated.
    {
      RenderContext mit;
      FillAnimInputs(mit);
      FillPresetPipeline(mit);
      mit.cmdList = cmdList;
      mit.canvasW = m_nTexSizeX;
      mit.canvasH = m_nTexSizeY;
      mit.pState = m_pState;
      mit.pOldState = m_pOldState;
      mit.isMirror = false;
      ApplyRenderMitigations(mit);
    }
  }

  // ── Blur passes: build blur pyramid from VS0 (just-warped frame) ──
  //
  // Must run after warp (which wrote VS1 from VS0) so comp shaders can sample
  // GetBlur1/2/3, and BEFORE shapes/waves/sprites, matching MilkDrop3's order.
  //
  // The SOURCE is VS[0] -- the previous frame's final image, including that
  // frame's injected shapes. Every reference engine reads m_lpVS[0] here, and
  // VS[0] is also what a custom comp binds as sampler_main, so GetPixel and
  // GetBlur1 describe the same image. Reading VS[1] (a pre-2.10 bug) gave a
  // blur with no injected shapes in it, so sparks never cancelled and blew out.
  //
  // Shared with the mirror worker (#15). The scan has to include the OLD
  // shaders during a blend, or levels they sample are never generated.
  // ASSIGN, not max. This is the primary's once-a-frame scan and it is what
  // makes the member per-frame again (#208); the binds below may still raise it
  // within the frame.
  m_nHighestBlurTexUsedThisFrame = ScanHighestBlurUsed(m_pState->m_bBlending);
  {
    RenderContext bc;
    FillAnimInputs(bc);
    FillPresetPipeline(bc);
    bc.cmdList         = cmdList;
    bc.pState          = m_pState;
    bc.pOldState       = m_pOldState;
    bc.bBlending       = m_pState->m_bBlending;
    bc.blurSrc         = &m_dx12VS[0];
    bc.blurSrcW        = (int)m_dx12VS[0].width;
    bc.blurSrcH        = (int)m_dx12VS[0].height;
    bc.blurTex         = m_dx12Blur;
    bc.blurW           = m_nBlurTexW;
    bc.blurH           = m_nBlurTexH;
    bc.highestBlurUsed = m_nHighestBlurTexUsedThisFrame;
    bc.isMirror        = false;
    RenderBlurPasses(bc);
  }

  // ── Inject content into VS1 (drawn after warp, before composite) ──
  // VS1 is still the render target from the warp pass above.
  // Restore VS1 as render target (blur passes may have changed it).
  // Draw order matches original MilkDrop: shapes → custom waves → wave → sprites/borders
  {
    m_lpDX->TransitionResource(m_dx12VS[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_lpDX->GetRtvCpuHandle(m_dx12VS[1]);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    SetViewportAndScissor(cmdList, m_nTexSizeX, m_nTexSizeY);

    // Milk2 layer 0 is stamped after comp (see below). Injecting it here
    // made GetPixel*float3(2,2,…) blow the current sprite out to white.
    DX12_DrawCustomShapes();
    DX12_DrawCustomWaves();
    DX12_DrawWave(mysound.fWave[0], mysound.fWave[1]);
    DX12_DrawSprites();
    if (SpritesEnabled())
      DrawUserSprites(12);

    // Burn completed supertexts into VS1 (persistence through warp feedback)
    if (MessagesEnabled()) {
      for (int i = 0; i < NUM_SUPERTEXTS; i++) {
        if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
          float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
          if (fProgress >= 1.0f) {
            // Burn only the PRIMARY's own into the primary's feedback (#255).
            //
            // Deliberately NOT a `continue` over the whole block: this loop is
            // also where a finished supertext is RETIRED, so skipping it
            // entirely for a display-owned message would hold its slot for
            // ever and starve the shared pool. Skip the draw, keep the
            // retirement.
            if (m_supertexts[i].nOwnerDisplay == -1)
              ShowSongTitleAnim(m_nTexSizeX, m_nTexSizeY, fProgress, i);
            float fTimeAfterFullDuration = GetTime() - m_supertexts[i].fStartTime - m_supertexts[i].fDuration;
            if (fTimeAfterFullDuration >= m_supertexts[i].fBurnTime) {
              m_supertexts[i].fStartTime = -1.0f;  // 'off' state
            }
          }
        }
      }
    }
  }


  // ── Buffer A pass: render to feedback[fbWrite] (Shadertoy temporal reprojection) ──
  if (m_bHasBufferA && m_dx12BufferAPSO && m_dx12Feedback[fbWrite].IsValid()) {
    DX12Texture& fbWriteTex = m_dx12Feedback[fbWrite];

    m_lpDX->TransitionResource(fbWriteTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE fbRtv = m_lpDX->GetRtvCpuHandle(fbWriteTex);
    cmdList->OMSetRenderTargets(1, &fbRtv, FALSE, nullptr);

    SetViewportAndScissor(cmdList, fbWriteTex.width, fbWriteTex.height);

    cmdList->SetPipelineState(m_dx12BufferAPSO.Get());

    // Apply Buffer A shader params and upload constant buffer
    PShaderInfo* bufASI = &m_shaders.bufferA;
    if (bufASI->CT) {
      ApplyShaderParams(&bufASI->params, bufASI->CT, m_pState);
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(bufASI->CT);
      if (ct->GetShadowSize() > 0) {
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
            m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
        if (cbAddr)
          cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
      }
    }

    // Bind Buffer A descriptor table (feedback[read] for self-referencing)
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBufferABindingGpuHandle());

    // Fullscreen quad (same MYVERTEX layout as comp)
    MYVERTEX bufAQuad[4];
    ZeroMemory(bufAQuad, sizeof(bufAQuad));
    bufAQuad[0].x = -1.f; bufAQuad[0].y =  1.f; bufAQuad[0].z = 0.f; bufAQuad[0].Diffuse = 0xFFFFFFFF;
    bufAQuad[0].tu = 0.f; bufAQuad[0].tv = 0.f; bufAQuad[0].tu_orig = 0.f; bufAQuad[0].tv_orig = 0.f; bufAQuad[0].rad = 1.f; bufAQuad[0].ang = 3.14159f;
    bufAQuad[1].x =  1.f; bufAQuad[1].y =  1.f; bufAQuad[1].z = 0.f; bufAQuad[1].Diffuse = 0xFFFFFFFF;
    bufAQuad[1].tu = 1.f; bufAQuad[1].tv = 0.f; bufAQuad[1].tu_orig = 1.f; bufAQuad[1].tv_orig = 0.f; bufAQuad[1].rad = 1.f; bufAQuad[1].ang = 0.f;
    bufAQuad[2].x = -1.f; bufAQuad[2].y = -1.f; bufAQuad[2].z = 0.f; bufAQuad[2].Diffuse = 0xFFFFFFFF;
    bufAQuad[2].tu = 0.f; bufAQuad[2].tv = 1.f; bufAQuad[2].tu_orig = 0.f; bufAQuad[2].tv_orig = 1.f; bufAQuad[2].rad = 1.f; bufAQuad[2].ang = 3.14159f;
    bufAQuad[3].x =  1.f; bufAQuad[3].y = -1.f; bufAQuad[3].z = 0.f; bufAQuad[3].Diffuse = 0xFFFFFFFF;
    bufAQuad[3].tu = 1.f; bufAQuad[3].tv = 1.f; bufAQuad[3].tu_orig = 1.f; bufAQuad[3].tv_orig = 1.f; bufAQuad[3].rad = 1.f; bufAQuad[3].ang = 0.f;
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, bufAQuad, 4, sizeof(MYVERTEX));

    // Transition feedback[write] back to SRV so comp/Image can read it
    m_lpDX->TransitionResource(fbWriteTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // ── Composite pass: draw VS1 ──
  // When single-pass feedback is active, render to the FLOAT feedback buffer first
  // (preserves camera data outside [0,1]), then blit to the UNORM backbuffer for display.
  bool bCompToFeedback = m_bCompUsesFeedback && !m_bHasBufferA
                         && m_dx12Feedback[fbWrite].IsValid();

  {
    m_lpDX->TransitionResource(m_dx12VS[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (bCompToFeedback) {
      // Render comp to FLOAT feedback[write] instead of backbuffer
      m_lpDX->TransitionResource(m_dx12Feedback[fbWrite], D3D12_RESOURCE_STATE_RENDER_TARGET);
      D3D12_CPU_DESCRIPTOR_HANDLE fbRtv = m_lpDX->GetRtvCpuHandle(m_dx12Feedback[fbWrite]);
      cmdList->OMSetRenderTargets(1, &fbRtv, FALSE, nullptr);
    } else {
      // Normal path: render directly to backbuffer
      D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
      bbRtv.ptr += (SIZE_T)m_lpDX->m_frameIndex * m_lpDX->m_rtvDescriptorSize;
      cmdList->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
    }

    // When rendering to feedback, use VS resolution (matches texsize in the shader).
    // When rendering to backbuffer, use client/backbuffer resolution.
    if (bCompToFeedback)
      SetViewportAndScissor(cmdList, m_nTexSizeX, m_nTexSizeY);
    else
      SetViewportAndScissor(cmdList, m_lpDX->m_client_width, m_lpDX->m_client_height);

    // The comp draw dispatch is shared with the mirror worker (#15). This also
    // closes the same gap the warp pass had: the old second blend pass drew
    // nothing when the preset used a comp shader and m_dx12CompBlendPSO had not
    // been built. See RenderCompPass.
    {
      RenderContext cc;
      FillAnimInputs(cc);
      FillPresetPipeline(cc);
      cc.cmdList      = cmdList;
      cc.pState       = m_pState;
      cc.pOldState    = m_pOldState;
      cc.bBlending    = m_pState->m_bBlending;
      cc.compTable    = m_lpDX->GetCompBindingGpuHandle();
      cc.oldCompTable = m_lpDX->GetOldCompBindingGpuHandle();
      cc.verts        = m_verts;
      cc.isMirror     = false;
      RenderCompPass(cc);
    }
  }

  // ── Feedback blit: copy FLOAT feedback[write] → UNORM backbuffer for display ──
  if (bCompToFeedback) {
    m_lpDX->TransitionResource(m_dx12Feedback[fbWrite], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Set backbuffer as render target
    D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    bbRtv.ptr += (SIZE_T)m_lpDX->m_frameIndex * m_lpDX->m_rtvDescriptorSize;
    cmdList->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
    SetViewportAndScissor(cmdList, m_lpDX->m_client_width, m_lpDX->m_client_height);

    // Simple passthrough PSO + bind feedback texture
    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    BYTE zeros[256] = {};
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
    if (cbAddr)
      cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBindingBlockGpuHandle(m_dx12Feedback[fbWrite]));

    // Blit fullscreen quad
    MYVERTEX blit[4];
    ZeroMemory(blit, sizeof(blit));
    blit[0].x = -1.f; blit[0].y =  1.f; blit[0].z = 0.f; blit[0].Diffuse = 0xFFFFFFFF; blit[0].tu = 0.f; blit[0].tv = 0.f;
    blit[1].x =  1.f; blit[1].y =  1.f; blit[1].z = 0.f; blit[1].Diffuse = 0xFFFFFFFF; blit[1].tu = 1.f; blit[1].tv = 0.f;
    blit[2].x = -1.f; blit[2].y = -1.f; blit[2].z = 0.f; blit[2].Diffuse = 0xFFFFFFFF; blit[2].tu = 0.f; blit[2].tv = 1.f;
    blit[3].x =  1.f; blit[3].y = -1.f; blit[3].z = 0.f; blit[3].Diffuse = 0xFFFFFFFF; blit[3].tu = 1.f; blit[3].tv = 1.f;
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, blit, 4, sizeof(MYVERTEX));
  }

  // ── Video Input: OVERLAY layer ──
  // Draw onto backbuffer after comp pass (video on top of preset)
  if (m_nVideoInputSource != VID_SOURCE_NONE && m_bSpoutInputOnTop) {
    if (m_nVideoInputSource == VID_SOURCE_SPOUT) {
      UpdateSpoutInputTexture();
      if (m_spoutInput && m_spoutInput->bConnected && m_spoutInput->dx12InputTex.IsValid()) {
        CompositeVideoInputFX(false, m_spoutInput->dx12InputTex, m_spoutInput->nSenderWidth, m_spoutInput->nSenderHeight);
        // No transition back -- UpdateSpoutInputTexture owns the COPY_DEST
        // window now, immediately around the Receive() that needs it (#213).
      }
    } else if (m_nVideoInputSource == VID_SOURCE_WEBCAM || m_nVideoInputSource == VID_SOURCE_FILE) {
      if (!m_videoCapture)
        InitVideoCapture();
      if (m_videoCapture && m_videoCapture->IsConnected()) {
        UpdateVideoCaptureTexture();
        if (m_videoCapture->m_dx12Tex.IsValid()) {
          CompositeVideoInputFX(false, m_videoCapture->m_dx12Tex, m_videoCapture->GetWidth(), m_videoCapture->GetHeight());
        }
      }
    }
  }

  // Faint live sprite on top (Mandala1 is grayscale). Gold comes from
  // GetPixel of *previous* stamps, not from boosting this frame's copy.
  if (SpritesEnabled())
    DrawUserSprites(0);

  if (SpritesEnabled() && m_dx12VS[1].IsValid()) {
    m_lpDX->TransitionResource(m_dx12VS[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE vsRtv = m_lpDX->GetRtvCpuHandle(m_dx12VS[1]);
    cmdList->OMSetRenderTargets(1, &vsRtv, FALSE, nullptr);
    SetViewportAndScissor(cmdList, m_nTexSizeX, m_nTexSizeY);
    DrawUserSprites(10);
    D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    bbRtv.ptr += (SIZE_T)m_lpDX->m_frameIndex * m_lpDX->m_rtvDescriptorSize;
    cmdList->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
    SetViewportAndScissor(cmdList, m_lpDX->m_client_width, m_lpDX->m_client_height);
  }

  // ── Display active supertexts on the backbuffer ──
  if (MessagesEnabled() && !AnyIndependentMirrorEnabled()) {
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      // The primary draws only its OWN (#255). The slots are one pool shared
      // with every display running its own message loop, so without this the
      // main window would show theirs as well as its own.
      if (m_supertexts[i].nOwnerDisplay != -1)
        continue;
      if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
        float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
        if (fProgress <= 1.0f) {
          ShowSongTitleAnim(GetWidth(), GetHeight(), min(fProgress, 0.9999f), i);
        }
      }
    }
  }

  // Mark preset diagnostics as logged after all sub-functions have had their chance
  if (!m_bPresetDiagLogged && GetTime() - m_fPresetStartTime >= 0.0f)
    m_bPresetDiagLogged = true;
}

// Forward declaration — defined later in this file
int SmoothWave(WFVERTEX* vi, int nVertsIn, WFVERTEX* vo);

// start -> alpha 0, end -> alpha 1, linearly, clamped. Stock MilkDrop.
//
// An inverted range (start > end) is legal and meaningful: it simply makes the
// ramp run the other way, so the wave is strongest at LOW volume and fades out
// as the track gets louder.
//
// A special case added in 961c18ba turned that into a hard step -- visible only
// once volume reached `start` -- which is the exact opposite of stock. Measured
// on Mandala2 (start 1.5, end 0.95): with music playing, MD3 PRO hides the wave
// while MDropDX12 drew it across the frame. Stock gives t = 0 at high volume;
// the special case gave t = 1. Removed.
static float WaveAlphaAfterVolumeMod(float alpha, bool enable, float vol, float start, float end)
{
  if (enable) {
    float t;
    if (fabsf(end - start) < 1.0e-6f) {
      t = (vol >= end) ? 1.0f : 0.0f;
    } else {
      t = (vol - start) / (end - start);
      if (t < 0.0f) t = 0.0f;
      if (t > 1.0f) t = 1.0f;
    }
    alpha *= t;
  }
  if (alpha < 0.0f) alpha = 0.0f;
  if (alpha > 1.0f) alpha = 1.0f;
  return alpha;
}

void mdrop::Engine::DX12_DrawWave(float* fL, float* fR, CState* pStateOverride,
                                  const td_mysounddata* pSndOverride,
                                  const RenderContext* pCtx) {
  if (!m_lpDX || !CmdListFor(pCtx))
    return;
  m_lpDX->BeginStage("wave");   // #222 stage map

  // Whose wave is this? Every parameter below -- colour, position, mode, alpha,
  // mystery, brighten, and the old-wave blend -- used to come from m_pState
  // unconditionally, so a mirror rendering its own preset drew the PRIMARY's
  // wave into its feedback face and warped it forward every frame (#186).
  CState* pState = pStateOverride ? pStateOverride : m_pState;
  if (!pState)
    return;
  // ...and whose audio. The caller already passed this surface's wave samples;
  // imm/imm_rel/fSpec came from the engine's live analysis, so the same wave
  // was half one surface's audio and half another's.
  const td_mysounddata& snd = pSndOverride ? *pSndOverride : mysound;

  // Whose canvas. Same argument as the state above: these were the ENGINE's
  // aspect and texsize, which SizeGuard had to overwrite for the duration of a
  // mirror record precisely because they are not this surface's (#121).
  // haveCanvas for ALL of them, not just the sizes. A context that carries no
  // canvas has aspect 1.0 by default, so testing `pCtx ?` here would hand a
  // canvas-less context square aspect instead of the engine's -- the same
  // defect 43e807c7 fixed one line lower for texsize, where `rc ?` fed 0 in and
  // bound (0,0,inf,inf). Both callers happen to be unambiguous today; this
  // stops the next one from having to notice.
  const bool  haveCanvas = pCtx && pCtx->canvasW > 0 && pCtx->canvasH > 0;
  const float aspectX    = haveCanvas ? pCtx->fAspectX    : m_fAspectX;
  const float aspectY    = haveCanvas ? pCtx->fAspectY    : m_fAspectY;
  const float invAspectX = haveCanvas ? pCtx->fInvAspectX : m_fInvAspectX;
  const float invAspectY = haveCanvas ? pCtx->fInvAspectY : m_fInvAspectY;
  const int   texSizeX   = haveCanvas ? pCtx->canvasW : m_nTexSizeX;
  const int   texSizeY   = haveCanvas ? pCtx->canvasH : m_nTexSizeY;

  WFVERTEX v1[576 + 1], v2[576 + 1];

  float cr = (float)(*pState->var_pf_wave_r);
  float cg = (float)(*pState->var_pf_wave_g);
  float cb = (float)(*pState->var_pf_wave_b);
  float cx = (float)(*pState->var_pf_wave_x);
  float cy = (float)(*pState->var_pf_wave_y);
  float fWaveParam = (float)(*pState->var_pf_wave_mystery);

  if (cr < 0) cr = 0;
  if (cg < 0) cg = 0;
  if (cb < 0) cb = 0;
  if (cr > 1) cr = 1;
  if (cg > 1) cg = 1;
  if (cb > 1) cb = 1;

  if (*pState->var_pf_wave_brighten) {
    float fMaximizeWaveColorAmount = 1.0f;
    float max = cr;
    if (max < cg) max = cg;
    if (max < cb) max = cb;
    if (max > 0.01f) {
      cr = cr / max * fMaximizeWaveColorAmount + cr * (1.0f - fMaximizeWaveColorAmount);
      cg = cg / max * fMaximizeWaveColorAmount + cg * (1.0f - fMaximizeWaveColorAmount);
      cb = cb / max * fMaximizeWaveColorAmount + cb * (1.0f - fMaximizeWaveColorAmount);
    }
  }

  float fWavePosX = cx * 2.0f - 1.0f;
  float fWavePosY = cy * 2.0f - 1.0f;

  float bass_rel = snd.imm[0];
  float mid_rel = snd.imm[1];
  float treble_rel = snd.imm[2];

  int sample_offset = 0;
  int new_wavemode = (int)(*pState->var_pf_wave_mode) % NUM_WAVES;

  int its = (pState->m_bBlending && (new_wavemode != pState->m_nOldWaveMode)) ? 2 : 1;
  int nVerts1 = 0;
  int nVerts2 = 0;
  int nBreak1 = -1;
  int nBreak2 = -1;
  float alpha1, alpha2;

  for (int it = 0; it < its; it++) {
    int   wave = (it == 0) ? new_wavemode : pState->m_nOldWaveMode;
    int   nVerts = NUM_WAVEFORM_SAMPLES;
    int   nBreak = -1;

    float fWaveParam2 = fWaveParam;
    if ((wave == 0 || wave == 1 || wave == 4) && (fWaveParam2 < -1 || fWaveParam2 > 1)) {
      fWaveParam2 = fWaveParam2 * 0.5f + 0.5f;
      fWaveParam2 -= floorf(fWaveParam2);
      fWaveParam2 = fabsf(fWaveParam2);
      fWaveParam2 = fWaveParam2 * 2 - 1;
    }

    WFVERTEX* v = (it == 0) ? v1 : v2;
    ZeroMemory(v, sizeof(WFVERTEX) * nVerts);

    float alpha = (float)(*pState->var_pf_wave_a);
    alpha = WaveAlphaAfterVolumeMod(alpha, pState->m_bModWaveAlphaByVolume,
        (snd.imm_rel[0] + snd.imm_rel[1] + snd.imm_rel[2]) * 0.333f,
        pState->m_fModWaveAlphaStart.eval(GetTime()),
        pState->m_fModWaveAlphaEnd.eval(GetTime()));

    switch (wave) {
    case 0:
      // circular wave
      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;

      {
        float inv_nverts_minus_one = 1.0f / (float)(nVerts - 1);
        for (int i = 0; i < nVerts; i++) {
          float rad = 0.5f + 0.4f * fR[i + sample_offset] + fWaveParam2;
          float ang = (i)*inv_nverts_minus_one * 6.28f + GetTime() * 0.2f;
          if (i < nVerts / 10) {
            float mix = i / (nVerts * 0.1f);
            mix = 0.5f - 0.5f * cosf(mix * 3.1416f);
            float rad_2 = 0.5f + 0.4f * fR[i + nVerts + sample_offset] + fWaveParam2;
            rad = rad_2 * (1.0f - mix) + rad * (mix);
          }
          if (m_bScreenDependentRenderMode) {
            v[i].x = rad * cosf(ang) + fWavePosX;
            v[i].y = rad * sinf(ang) + fWavePosY;
          } else {
            v[i].x = rad * cosf(ang) * aspectY + fWavePosX;
            v[i].y = rad * sinf(ang) * aspectX + fWavePosY;
          }
        }
      }
      if (!pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 1:
      // x-y osc. spiral
      alpha *= 1.25f;

      nVerts /= 2;
      for (int i = 0; i < nVerts; i++) {
        float rad = 0.53f + 0.43f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 1.57f + GetTime() * 2.3f;
        if (m_bScreenDependentRenderMode) {
          v[i].x = rad * cosf(ang) + fWavePosX;
          v[i].y = rad * sinf(ang) + fWavePosY;
        } else {
          v[i].x = rad * cosf(ang) * aspectY + fWavePosX;
          v[i].y = rad * sinf(ang) * aspectX + fWavePosY;
        }
      }
      break;

    case 2:
      // centered spiro (alpha constant)
      switch (texSizeX) {
      case 256:  alpha *= 0.07f; break;
      case 512:  alpha *= 0.09f; break;
      case 1024: alpha *= 0.11f; break;
      case 2048: alpha *= 0.13f; break;
      }

      for (int i = 0; i < nVerts; i++) {
        if (m_bScreenDependentRenderMode) {
          v[i].x = fR[i] + fWavePosX;
          v[i].y = fL[i + 32] + fWavePosY;
        } else {
          v[i].x = fR[i] * aspectY + fWavePosX;
          v[i].y = fL[i + 32] * aspectX + fWavePosY;
        }
      }
      break;

    case 3:
      // centered spiro (alpha tied to volume)
      switch (texSizeX) {
      case 256:  alpha = 0.075f; break;
      case 512:  alpha = 0.150f; break;
      case 1024: alpha = 0.220f; break;
      case 2048: alpha = 0.330f; break;
      }
      alpha *= 1.3f;
      alpha *= powf(treble_rel, 2.0f);

      for (int i = 0; i < nVerts; i++) {
        if (m_bScreenDependentRenderMode) {
          v[i].x = fR[i] + fWavePosX;
          v[i].y = fL[i + 32] + fWavePosY;
        } else {
          v[i].x = fR[i] * aspectY + fWavePosX;
          v[i].y = fL[i + 32] * aspectX + fWavePosY;
        }
      }
      break;

    case 4:
      // horizontal script
      if (nVerts > texSizeX / 3)
        nVerts = texSizeX / 3;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;

      {
        float w1 = 0.45f + 0.5f * (fWaveParam2 * 0.5f + 0.5f);
        float w2 = 1.0f - w1;
        float inv_nverts = 1.0f / (float)(nVerts);
        for (int i = 0; i < nVerts; i++) {
          v[i].x = -1.0f + 2.0f * (i * inv_nverts) + fWavePosX;
          v[i].y = fL[i + sample_offset] * 0.47f + fWavePosY;
          v[i].x += fR[i + 25 + sample_offset] * 0.44f;
          if (i > 1) {
            v[i].x = v[i].x * w2 + w1 * (v[i - 1].x * 2.0f - v[i - 2].x);
            v[i].y = v[i].y * w2 + w1 * (v[i - 1].y * 2.0f - v[i - 2].y);
          }
        }
      }
      break;

    case 5:
      // explosive complex
      switch (texSizeX) {
      case 256:  alpha *= 0.07f; break;
      case 512:  alpha *= 0.09f; break;
      case 1024: alpha *= 0.11f; break;
      case 2048: alpha *= 0.13f; break;
      }

      {
        float cos_rot = cosf(GetTime() * 0.3f);
        float sin_rot = sinf(GetTime() * 0.3f);
        for (int i = 0; i < nVerts; i++) {
          float x0 = (fR[i] * fL[i + 32] + fL[i] * fR[i + 32]);
          float y0 = (fR[i] * fR[i] - fL[i + 32] * fL[i + 32]);
          if (m_bScreenDependentRenderMode) {
            v[i].x = (x0 * cos_rot - y0 * sin_rot) + fWavePosX;
            v[i].y = (x0 * sin_rot + y0 * cos_rot) + fWavePosY;
          } else {
            v[i].x = (x0 * cos_rot - y0 * sin_rot) * aspectY + fWavePosX;
            v[i].y = (x0 * sin_rot + y0 * cos_rot) * aspectX + fWavePosY;
          }
        }
      }
      break;

    case 6:
    case 7:
    case 8:
      nVerts /= 2;
      if (nVerts > texSizeX / 3)
        nVerts = texSizeX / 3;
      if (wave == 8)
        nVerts = 256;
      else
        sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;

      {
        float ang = 1.57f * fWaveParam2;
        float dx = cosf(ang);
        float dy = sinf(ang);
        float edge_x[2], edge_y[2];
        edge_x[0] = fWavePosX * cosf(ang + 1.57f) - dx * 3.0f;
        edge_y[0] = fWavePosX * sinf(ang + 1.57f) - dy * 3.0f;
        edge_x[1] = fWavePosX * cosf(ang + 1.57f) + dx * 3.0f;
        edge_y[1] = fWavePosX * sinf(ang + 1.57f) + dy * 3.0f;
        for (int i = 0; i < 2; i++) {
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;
            switch (j) {
            case 0: if (edge_x[i] > 1.1f)  { t = (1.1f - edge_x[1-i]) / (edge_x[i] - edge_x[1-i]); bClip = true; } break;
            case 1: if (edge_x[i] < -1.1f) { t = (-1.1f - edge_x[1-i]) / (edge_x[i] - edge_x[1-i]); bClip = true; } break;
            case 2: if (edge_y[i] > 1.1f)  { t = (1.1f - edge_y[1-i]) / (edge_y[i] - edge_y[1-i]); bClip = true; } break;
            case 3: if (edge_y[i] < -1.1f) { t = (-1.1f - edge_y[1-i]) / (edge_y[i] - edge_y[1-i]); bClip = true; } break;
            }
            if (bClip) {
              float dx2 = edge_x[i] - edge_x[1-i];
              float dy2 = edge_y[i] - edge_y[1-i];
              edge_x[i] = edge_x[1-i] + dx2 * t;
              edge_y[i] = edge_y[1-i] + dy2 * t;
            }
          }
        }
        dx = (edge_x[1] - edge_x[0]) / (float)nVerts;
        dy = (edge_y[1] - edge_y[0]) / (float)nVerts;
        float ang2 = atan2f(dy, dx);
        float perp_dx = cosf(ang2 + 1.57f);
        float perp_dy = sinf(ang2 + 1.57f);
        if (wave == 6)
          for (int i = 0; i < nVerts; i++) {
            v[i].x = edge_x[0] + dx * i + perp_dx * 0.25f * fL[i + sample_offset];
            v[i].y = edge_y[0] + dy * i + perp_dy * 0.25f * fL[i + sample_offset];
          }
        else if (wave == 8)
          for (int i = 0; i < nVerts; i++) {
            float f = 0.1f * logf(snd.fSpecLeft[i * 2] + snd.fSpecLeft[i * 2 + 1]);
            v[i].x = edge_x[0] + dx * i + perp_dx * f;
            v[i].y = edge_y[0] + dy * i + perp_dy * f;
          }
        else {
          float sep = powf(fWavePosY * 0.5f + 0.5f, 2.0f);
          for (int i = 0; i < nVerts; i++) {
            v[i].x = edge_x[0] + dx * i + perp_dx * (0.25f * fL[i + sample_offset] + sep);
            v[i].y = edge_y[0] + dy * i + perp_dy * (0.25f * fL[i + sample_offset] + sep);
          }
          for (int i = 0; i < nVerts; i++) {
            v[i + nVerts].x = edge_x[0] + dx * i + perp_dx * (0.25f * fR[i + sample_offset] - sep);
            v[i + nVerts].y = edge_y[0] + dy * i + perp_dy * (0.25f * fR[i + sample_offset] - sep);
          }
          nBreak = nVerts;
          nVerts *= 2;
        }
      }
      break;

    case 9:
      // large wave
      nVerts /= 2;
      if (nVerts > texSizeX / 3)
        nVerts = texSizeX / 3;
      if (wave == 8)
        nVerts = 256;
      else
        sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;

      {
        float ang = 1.57f * fWaveParam2;
        float dx = cosf(ang);
        float dy = sinf(ang);
        float edge_x[2], edge_y[2];
        edge_x[0] = fWavePosX * cosf(ang + 1.57f) - dx * 3.0f;
        edge_y[0] = fWavePosX * sinf(ang + 1.57f) - dy * 3.0f;
        edge_x[1] = fWavePosX * cosf(ang + 1.57f) + dx * 3.0f;
        edge_y[1] = fWavePosX * sinf(ang + 1.57f) + dy * 3.0f;
        for (int i = 0; i < 2; i++) {
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;
            switch (j) {
            case 0: if (edge_x[i] > 1.1f)  { t = (1.1f - edge_x[1-i]) / (edge_x[i] - edge_x[1-i]); bClip = true; } break;
            case 1: if (edge_x[i] < -1.1f) { t = (-1.1f - edge_x[1-i]) / (edge_x[i] - edge_x[1-i]); bClip = true; } break;
            case 2: if (edge_y[i] > 1.1f)  { t = (1.1f - edge_y[1-i]) / (edge_y[i] - edge_y[1-i]); bClip = true; } break;
            case 3: if (edge_y[i] < -1.1f) { t = (-1.1f - edge_y[1-i]) / (edge_y[i] - edge_y[1-i]); bClip = true; } break;
            }
            if (bClip) {
              float dx2 = edge_x[i] - edge_x[1-i];
              float dy2 = edge_y[i] - edge_y[1-i];
              edge_x[i] = edge_x[1-i] + dx2 * t;
              edge_y[i] = edge_y[1-i] + dy2 * t;
            }
          }
        }
        dx = (edge_x[1] - edge_x[0]) / (float)nVerts;
        dy = (edge_y[1] - edge_y[0]) / (float)nVerts;
        float ang2 = atan2f(dy, dx);
        float perp_dx = cosf(ang2 + 1.57f);
        float perp_dy = sinf(ang2 + 1.57f);
        for (int i = 0; i < nVerts; i++) {
          v[i].x = edge_x[0] + dx * i + perp_dx * 1.00f * fL[i + sample_offset];
          v[i].y = edge_y[0] + dy * i + perp_dy * 1.00f * fL[i + sample_offset];
        }
        nBreak = nVerts;
        nVerts *= 2;
      }
      break;

    case 10:
      // X marks the spot
      nVerts /= 2;
      if (nVerts > texSizeX / 3)
        nVerts = texSizeX / 3;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;

      {
        float ang = -0.75f + fWaveParam2 * 3.15f;
        float dx = cosf(ang);
        float dy = sinf(ang);
        float edge_x[2], edge_y[2];
        edge_x[0] = fWavePosX * cosf(ang + 1.57f) - dx * 3.0f;
        edge_y[0] = fWavePosX * sinf(ang + 1.57f) - dy * 3.0f;
        edge_x[1] = fWavePosX * cosf(ang + 1.57f) + dx * 3.0f;
        edge_y[1] = fWavePosX * sinf(ang + 1.57f) + dy * 3.0f;
        for (int i = 0; i < 2; i++) {
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;
            switch (j) {
            case 0: if (edge_x[i] > 1.1f)  { t = (1.1f - edge_x[1-i]) / (edge_x[i] - edge_x[1-i]); bClip = true; } break;
            case 1: if (edge_x[i] < -1.1f) { t = (-1.1f - edge_x[1-i]) / (edge_x[i] - edge_x[1-i]); bClip = true; } break;
            case 2: if (edge_y[i] > 1.1f)  { t = (1.1f - edge_y[1-i]) / (edge_y[i] - edge_y[1-i]); bClip = true; } break;
            case 3: if (edge_y[i] < -1.1f) { t = (-1.1f - edge_y[1-i]) / (edge_y[i] - edge_y[1-i]); bClip = true; } break;
            }
            if (bClip) {
              float dx2 = edge_x[i] - edge_x[1-i];
              float dy2 = edge_y[i] - edge_y[1-i];
              edge_x[i] = edge_x[1-i] + dx2 * t;
              edge_y[i] = edge_y[1-i] + dy2 * t;
            }
          }
        }
        dx = (edge_x[1] - edge_x[0]) / (float)nVerts;
        dy = (edge_y[1] - edge_y[0]) / (float)nVerts;
        float ang2 = atan2f(dy, dx);
        float perp_dx = cosf(ang2 + 1.57f);
        float perp_dy = sinf(ang2 + 1.57f);
        for (int i = 0; i < nVerts; i++) {
          v[i].x = edge_x[0] + dx * i + perp_dx * 0.35f * fL[i + sample_offset];
          v[i].y = edge_y[0] + dy * i + perp_dy * 0.35f * fL[i + sample_offset];
        }
        // second arm of the X
        float ang3 = 0.75f + fWaveParam2 * 3.15f;
        float dx3 = cosf(ang3);
        float dy3 = sinf(ang3);
        float edge_x3[2], edge_y3[2];
        edge_x3[0] = fWavePosX * cosf(ang3 + 1.57f) - dx3 * 3.0f;
        edge_y3[0] = fWavePosX * sinf(ang3 + 1.57f) - dy3 * 3.0f;
        edge_x3[1] = fWavePosX * cosf(ang3 + 1.57f) + dx3 * 3.0f;
        edge_y3[1] = fWavePosX * sinf(ang3 + 1.57f) + dy3 * 3.0f;
        for (int i = 0; i < 2; i++) {
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;
            switch (j) {
            case 0: if (edge_x3[i] > 1.1f)  { t = (1.1f - edge_x3[1-i]) / (edge_x3[i] - edge_x3[1-i]); bClip = true; } break;
            case 1: if (edge_x3[i] < -1.1f) { t = (-1.1f - edge_x3[1-i]) / (edge_x3[i] - edge_x3[1-i]); bClip = true; } break;
            case 2: if (edge_y3[i] > 1.1f)  { t = (1.1f - edge_y3[1-i]) / (edge_y3[i] - edge_y3[1-i]); bClip = true; } break;
            case 3: if (edge_y3[i] < -1.1f) { t = (-1.1f - edge_y3[1-i]) / (edge_y3[i] - edge_y3[1-i]); bClip = true; } break;
            }
            if (bClip) {
              float dx4 = edge_x3[i] - edge_x3[1-i];
              float dy4 = edge_y3[i] - edge_y3[1-i];
              edge_x3[i] = edge_x3[1-i] + dx4 * t;
              edge_y3[i] = edge_y3[1-i] + dy4 * t;
            }
          }
        }
        dx3 = (edge_x3[1] - edge_x3[0]) / (float)nVerts;
        dy3 = (edge_y3[1] - edge_y3[0]) / (float)nVerts;
        float ang4 = atan2f(dy3, dx3);
        float perp_dx3 = cosf(ang4 + 1.57f);
        float perp_dy3 = sinf(ang4 + 1.57f);
        for (int i = 0; i < nVerts; i++) {
          v[i + nVerts].x = edge_x3[0] + dx3 * i + perp_dx3 * (0.35f * fR[i + sample_offset]);
          v[i + nVerts].y = edge_y3[0] + dy3 * i + perp_dy3 * (0.35f * fR[i + sample_offset]);
        }
        nBreak = nVerts;
        nVerts *= 2;
      }
      break;

    case 11:
      // vertical dual wave
      nVerts /= 2;
      if (nVerts > texSizeX / 3)
        nVerts = texSizeX / 3;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;

      {
        float ang = 1.57f;
        float dx = cosf(ang);
        float dy = sinf(ang);
        float edge_x[2], edge_y[2];
        edge_x[0] = fWavePosX * cosf(ang + 1.57f) - dx * 3.0f;
        edge_y[0] = fWavePosX * sinf(ang + 1.57f) - dy * 3.0f;
        edge_x[1] = fWavePosX * cosf(ang + 1.57f) + dx * 3.0f;
        edge_y[1] = fWavePosX * sinf(ang + 1.57f) + dy * 3.0f;
        for (int i = 0; i < 2; i++) {
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;
            switch (j) {
            case 0: if (edge_x[i] > 1.1f)  { t = (1.1f - edge_x[1-i]) / (edge_x[i] - edge_x[1-i]); bClip = true; } break;
            case 1: if (edge_x[i] < -1.1f) { t = (-1.1f - edge_x[1-i]) / (edge_x[i] - edge_x[1-i]); bClip = true; } break;
            case 2: if (edge_y[i] > 1.1f)  { t = (1.1f - edge_y[1-i]) / (edge_y[i] - edge_y[1-i]); bClip = true; } break;
            case 3: if (edge_y[i] < -1.1f) { t = (-1.1f - edge_y[1-i]) / (edge_y[i] - edge_y[1-i]); bClip = true; } break;
            }
            if (bClip) {
              float dx2 = edge_x[i] - edge_x[1-i];
              float dy2 = edge_y[i] - edge_y[1-i];
              edge_x[i] = edge_x[1-i] + dx2 * t;
              edge_y[i] = edge_y[1-i] + dy2 * t;
            }
          }
        }
        dx = (edge_x[1] - edge_x[0]) / (float)nVerts;
        dy = (edge_y[1] - edge_y[0]) / (float)nVerts;
        float ang2 = atan2f(dy, dx);
        float perp_dx = cosf(ang2 + 1.57f);
        float perp_dy = sinf(ang2 + 1.57f);
        for (int i = 0; i < nVerts; i++) {
          v[i].x = edge_x[0] - 0.45f + dx * i + perp_dx * 0.35f * fL[i + sample_offset];
          v[i].y = edge_y[0] + dy * i + perp_dy * 0.35f * fL[i + sample_offset];
        }
        // second vertical wave
        float ang3 = 1.57f;
        float dx3 = cosf(ang3);
        float dy3 = sinf(ang3);
        float edge_x3[2], edge_y3[2];
        edge_x3[0] = fWavePosX * cosf(ang3 + 1.57f) - dx3 * 3.0f;
        edge_y3[0] = fWavePosX * sinf(ang3 + 1.57f) - dy3 * 3.0f;
        edge_x3[1] = fWavePosX * cosf(ang3 + 1.57f) + dx3 * 3.0f;
        edge_y3[1] = fWavePosX * sinf(ang3 + 1.57f) + dy3 * 3.0f;
        for (int i = 0; i < 2; i++) {
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;
            switch (j) {
            case 0: if (edge_x3[i] > 1.1f)  { t = (1.1f - edge_x3[1-i]) / (edge_x3[i] - edge_x3[1-i]); bClip = true; } break;
            case 1: if (edge_x3[i] < -1.1f) { t = (-1.1f - edge_x3[1-i]) / (edge_x3[i] - edge_x3[1-i]); bClip = true; } break;
            case 2: if (edge_y3[i] > 1.1f)  { t = (1.1f - edge_y3[1-i]) / (edge_y3[i] - edge_y3[1-i]); bClip = true; } break;
            case 3: if (edge_y3[i] < -1.1f) { t = (-1.1f - edge_y3[1-i]) / (edge_y3[i] - edge_y3[1-i]); bClip = true; } break;
            }
            if (bClip) {
              float dx4 = edge_x3[i] - edge_x3[1-i];
              float dy4 = edge_y3[i] - edge_y3[1-i];
              edge_x3[i] = edge_x3[1-i] + dx4 * t;
              edge_y3[i] = edge_y3[1-i] + dy4 * t;
            }
          }
        }
        dx3 = (edge_x3[1] - edge_x3[0]) / (float)nVerts;
        dy3 = (edge_y3[1] - edge_y3[0]) / (float)nVerts;
        float ang4 = atan2f(dy3, dx3);
        float perp_dx3 = cosf(ang4 + 1.57f);
        float perp_dy3 = sinf(ang4 + 1.57f);
        for (int i = 0; i < nVerts; i++) {
          v[i + nVerts].x = edge_x3[0] + 0.45f + dx3 * i + perp_dx3 * (0.35f * fR[i + sample_offset]);
          v[i + nVerts].y = edge_y3[0] + dy3 * i + perp_dy3 * (0.35f * fR[i + sample_offset]);
        }
        nBreak = nVerts;
        nVerts *= 2;
      }
      break;

    case 12:
      // x-y osc. spiral, skewed
      alpha *= 1.25f;

      nVerts /= 2;
      for (int i = 0; i < nVerts; i++) {
        float rad = 0.63f + 0.23f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 0.9f + GetTime() * 3.3f;
        if (m_bScreenDependentRenderMode) {
          v[i].x = rad * cosf(ang + alpha) + fWavePosX;
          v[i].y = rad * sinf(ang) + fWavePosY;
        } else {
          v[i].x = rad * cosf(ang + alpha) * aspectY + fWavePosX;
          v[i].y = rad * sinf(ang) * aspectX + fWavePosY;
        }
      }
      break;

    case 13:
      // Star Wave
      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;

      {
        float inv_nverts_minus_one = 1.0f / (float)(nVerts - 1);
        for (int i = 0; i < nVerts; i++) {
          float rad = 0.7f + 0.4f * fR[i + sample_offset] + fWaveParam2;
          float ang = (i)*inv_nverts_minus_one * 6.28f + GetTime() * 0.2f;
          if (i < nVerts / rad) {
            float mix = i / (nVerts * 0.1f);
            mix = 0.5f - 0.5f * cosf(mix * 3.1416f);
            float rad_2 = 0.5f + 0.4f * fR[i + nVerts + sample_offset] + fWaveParam2;
            rad = rad_2 * (1.0f - mix) + rad * (mix);
          }
          if (m_bScreenDependentRenderMode) {
            v[i].x = rad * cosf(ang) + fWavePosX;
            v[i].y = rad * sinf(ang) + fWavePosY;
          } else {
            v[i].x = rad * cosf(ang) * aspectY + fWavePosX;
            v[i].y = rad * sinf(ang) * aspectX + fWavePosY;
          }
        }
      }
      if (!pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 14:
      // Flower Wave
      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;

      {
        float inv_nverts_minus_one = 1.0f / (float)(nVerts - 1);
        for (int i = 0; i < nVerts; i++) {
          float rad = 0.7f + 0.7f * fR[i + sample_offset] + fWaveParam2;
          float ang = (i)*inv_nverts_minus_one * 6.28f + GetTime() * 0.2f;
          ang = ang / 2;
          rad = rad / 2;
          if (i < nVerts / rad) {
            float mix = i / (nVerts * 0.1f);
            mix = 0.7f - 0.7f * cosf(mix * 3.1416f);
            float rad_2 = 0.7f + 0.7f * fR[i + nVerts + sample_offset] + fWaveParam2;
            rad = rad_2 * (1.0f - mix) + rad * (mix * 2) / 8;
          }
          if (m_bScreenDependentRenderMode) {
            v[i].x = rad * cosf(ang * 3.1416f) / 1.5f + fWavePosX * cosf(3.1416f);
            v[i].y = rad * sinf(ang - GetTime() / 3) / 1.5f + fWavePosY * cosf(3.1416f);
          } else {
            v[i].x = rad * cosf(ang * 3.1416f) * aspectY / 1.5f + fWavePosX * cosf(3.1416f);
            v[i].y = rad * sinf(ang - GetTime() / 3) * aspectX / 1.5f + fWavePosY * cosf(3.1416f);
          }
        }
      }
      if (!pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 15:
      // Lasso Wave
      alpha *= 1.25f;

      nVerts /= 2;
      for (int i = 0; i < nVerts; i++) {
        float rad = 0.53f + 0.43f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 1.57f + GetTime() * 2.0f;
        float t = GetTime() / ang;
        v[i].x = (float)(cos(GetTime()) / 2 + cosf(ang * 2 + tanf(t)));
        v[i].y = (float)(sin(GetTime()) * 2 * sinf(ang * 3.14f) * aspectX / 2.8f + fWavePosY);
      }
      break;

    case 16:
      // Triangle Wave
      nVerts = 256;

      {
        float size = 0.575f;
        float rotation = (fWaveParam2) * 3.141593f;
        float cos_rot = cosf(rotation);
        float sin_rot = sinf(rotation);
        float inv_nverts = 1.0f / (float)(nVerts - 1);
        for (int i = 0; i < nVerts; i++) {
          float phase = i * inv_nverts;
          float x, y;
          if (phase < 0.3333f) {
            float t = phase * 3.0f;
            x = -size + t * size;
            y = -size + t * 2.0f * size;
          } else if (phase < 0.6666f) {
            float t = (phase - 0.3333f) * 3.0f;
            x = 0.0f + t * size;
            y = size - t * 2.0f * size;
          } else {
            float t = (phase - 0.6666f) * 3.0f;
            x = size - t * 2.0f * size;
            y = -size;
          }
          float audio_mod = 1.0f + 0.3f * fL[(i * 2) % NUM_WAVEFORM_SAMPLES];
          x *= audio_mod;
          y *= audio_mod;
          float x_rot = x * cos_rot - y * sin_rot;
          float y_rot = x * sin_rot + y * cos_rot;
          if (m_bScreenDependentRenderMode) {
            v[i].x = x_rot + fWavePosX;
            v[i].y = y_rot + fWavePosY;
          } else {
            v[i].x = x_rot * aspectY + fWavePosX;
            v[i].y = y_rot * aspectX + fWavePosY;
          }
        }
      }
      if (!pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 17:
      // Fireworks Waveform
      nVerts = 256;

      {
        float time = GetTime();
        float burst_frequency = 1.0f - fWaveParam2 + .001f;
        float burst_phase = fmodf(time, burst_frequency) / burst_frequency;
        int burst_num = (int)(time / burst_frequency);
        float rand_seed = (burst_num * 10.0f);
        float base_x = (rand_seed * 0.1345f - floorf(rand_seed * 0.1345f)) * 2.0f - 1.0f;
        float base_y = (rand_seed * 0.2783f - floorf(rand_seed * 0.2783f)) * 2.0f - 1.0f;
        if (fmodf(rand_seed, 1.0f) > 0.3f) {
          base_x *= 0.3f;
          base_y *= 0.3f;
        }
        float burst_size = min(1.0f, burst_phase * 4.0f);
        float burst_fade = 1.0f - powf(burst_phase, 3.0f);
        float audio_boost = 1.0f + 2.0f * (snd.imm_rel[0] + snd.imm_rel[1]) * 0.5f;
        for (int i = 0; i < nVerts; i++) {
          float ang = (i / (float)nVerts) * 6.283185f;
          float dist_var = 0.7f + 0.3f * (fmodf(rand_seed + i * 0.1f, 1.0f));
          float dist = burst_size * dist_var * (0.5f + 0.5f * fR[(i * 3) % NUM_WAVEFORM_SAMPLES]) * audio_boost;
          float x = base_x + cosf(ang) * dist;
          float y = base_y + sinf(ang) * dist;
          float swirl = time * 3.0f + ang;
          x += cosf(swirl) * burst_size * 0.1f;
          y += sinf(swirl) * burst_size * 0.1f;
          if (m_bScreenDependentRenderMode) {
            v[i].x = x + fWavePosX;
            v[i].y = y + fWavePosY;
          } else {
            v[i].x = x * aspectY + fWavePosX;
            v[i].y = y * aspectX + fWavePosY;
          }
          alpha *= burst_fade;
        }
      }
      if (!pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;
    }

    if (it == 0) {
      nVerts1 = nVerts;
      nBreak1 = nBreak;
      alpha1 = alpha;
    } else {
      nVerts2 = nVerts;
      nBreak2 = nBreak;
      alpha2 = alpha;
    }
  }

  // Blend two waveforms during preset transition
  float mix = CosineInterp(pState->m_fBlendProgress);
  float mix2 = 1.0f - mix;
  if (nVerts2 > 0) {
    float m = (nVerts2 - 1) / (float)nVerts1;
    float x, y;
    for (int i = 0; i < nVerts1; i++) {
      float fIdx = i * m;
      int   nIdx = (int)fIdx;
      float t = fIdx - nIdx;
      if (nIdx == nBreak2 - 1) {
        x = v2[nIdx].x;
        y = v2[nIdx].y;
        nBreak1 = i + 1;
      } else {
        x = v2[nIdx].x * (1 - t) + v2[nIdx + 1].x * (t);
        y = v2[nIdx].y * (1 - t) + v2[nIdx + 1].y * (t);
      }
      v1[i].x = v1[i].x * (mix)+x * (mix2);
      v1[i].y = v1[i].y * (mix)+y * (mix2);
    }
  }
  if (nVerts2 > 0) {
    alpha1 = alpha1 * (mix)+alpha2 * (1.0f - mix);
  }

  // Apply color & alpha
  // Note: DX9 flips Y here to compensate for the OrthoLH(2,-2) projection.
  // DX12 vertex shaders bypass projection, so Y flip is NOT needed.
  v1[0].Diffuse = D3DCOLOR_RGBA_01(cr, cg, cb, alpha1);
  for (int i = 0; i < nVerts1; i++) {
    v1[i].Diffuse = v1[0].Diffuse;
  }

  if (alpha1 < 0.004f)
    return;

  // Tessellate (smooth the wave)
  WFVERTEX* pVerts = v1;
  WFVERTEX vTess[(576 + 3) * 2];
  if (nBreak1 == -1) {
    nVerts1 = SmoothWave(v1, nVerts1, vTess);
  } else {
    int oldBreak = nBreak1;
    nBreak1 = SmoothWave(v1, nBreak1, vTess);
    nVerts1 = SmoothWave(&v1[oldBreak], nVerts1 - oldBreak, &vTess[nBreak1]) + nBreak1;
  }
  pVerts = vTess;

  // Select PSO based on additive blend and dots mode
  bool additive = (*pState->var_pf_wave_additive) != 0;
  bool useDots = (*pState->var_pf_wave_usedots) != 0;
  DX12PsoId psoId;
  D3D12_PRIMITIVE_TOPOLOGY topology;
  if (useDots) {
    psoId = additive ? PSO_POINT_ADDITIVE_WFVERTEX : PSO_POINT_ALPHABLEND_WFVERTEX;
    topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  } else {
    psoId = additive ? PSO_LINE_ADDITIVE_WFVERTEX : PSO_LINE_ALPHABLEND_WFVERTEX;
    topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
  }

  auto* cmdList = CmdListFor(pCtx);
  if (!cmdList) return;
  cmdList->SetPipelineState(m_lpDX->m_PSOs[psoId].Get());

  // Draw with thickness support (4 offset passes for thick/dot mode)
  float x_inc = 2.0f / (float)texSizeX;
  float y_inc = 2.0f / (float)texSizeY;
  int drawing_its = ((*pState->var_pf_wave_thick || useDots) && (texSizeX >= 512)) ? 4 : 1;

  for (int it = 0; it < drawing_its; it++) {
    switch (it) {
    case 0: break;
    case 1: for (int j = 0; j < nVerts1; j++) pVerts[j].x += x_inc; break;
    case 2: for (int j = 0; j < nVerts1; j++) pVerts[j].y += y_inc; break;
    case 3: for (int j = 0; j < nVerts1; j++) pVerts[j].x -= x_inc; break;
    }

    if (nBreak1 == -1) {
      m_lpDX->DrawVertices(topology, pVerts, nVerts1, sizeof(WFVERTEX));
    } else {
      m_lpDX->DrawVertices(topology, pVerts, nBreak1, sizeof(WFVERTEX));
      m_lpDX->DrawVertices(topology, &pVerts[nBreak1], nVerts1 - nBreak1, sizeof(WFVERTEX));
    }
  }
}

// Expand TRIANGLEFAN to TRIANGLELIST (DX12 has no fan primitive)
template<typename V>
static int ExpandFanToTriList(const V* src, int nFanVerts, V* dest) {
  int out = 0;
  for (int i = 1; i <= nFanVerts - 2; i++) {
    dest[out++] = src[0];
    dest[out++] = src[i];
    dest[out++] = src[i + 1];
  }
  return out;
}

//----------------------------------------------------------------------
// ApplyFeedbackDamp - scale the feedback buffer down by a constant each frame
//
// The alternative to capping the canvas for a preset whose feedback loop
// diverges as the canvas grows. Drawn into VS1 immediately after the warp mesh
// and BEFORE the shapes and waves injected a few lines further down -- those
// are this frame's new signal and must arrive at full strength, or the preset
// just looks dim.
//
// It does NOT follow that only VS1 is affected. DX12_RenderWarpAndComposite
// returns to a std::swap(m_dx12VS[0], m_dx12VS[1]), so this frame's damped VS1
// IS next frame's VS0 -- the blur pyramid and a custom comp's sampler_main both
// read damped content, one frame late. That is correct and intended; it is
// written down because an earlier version of this comment claimed the blur was
// untouched, and anyone reasoning about the SIGN of the damp needs to know
// every feedback tap sees it.
//
// One quad over the whole canvas -- 6 vertices, no texture, no constant
// buffer. `damp` is the multiplier, so 1.0 means "leave it alone" and the draw
// is skipped entirely.
//----------------------------------------------------------------------

void mdrop::Engine::ApplyFeedbackDamp(ID3D12GraphicsCommandList* cmdList, float damp) {
  if (!cmdList || damp >= 0.9999f) return;
  if (damp < 0.0f) damp = 0.0f;

  cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_DAMP_WFVERTEX].Get());

  WFVERTEX v[6];
  ZeroMemory(v, sizeof(v));
  // Black at alpha 1-damp leaves dst*damp behind. The alpha is computed into a
  // local first: D3DCOLOR_RGBA_01 now parenthesises its parameters, but the
  // expression is clearer read once than inlined into a four-argument macro.
  const float alpha = 1.0f - damp;
  const D3DCOLOR c = D3DCOLOR_RGBA_01(0.0f, 0.0f, 0.0f, alpha);
  // Clip space, so the quad is the whole render target whatever its size --
  // there is no projection matrix in the DX12 path.
  const float x[6] = { -1.f,  1.f, -1.f,  1.f,  1.f, -1.f };
  const float y[6] = { -1.f, -1.f,  1.f, -1.f,  1.f,  1.f };
  for (int i = 0; i < 6; i++) {
    v[i].x = x[i];
    v[i].y = y[i];
    v[i].Diffuse = c;
  }
  m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, v, 6, sizeof(WFVERTEX), cmdList);
}

// RenderWarpPass - the warp draw dispatch, for any surface (#15)
//
// Four branches decide what gets drawn: old-and-new during a blend, the
// neither-uses-a-shader special case, and the ordinary single pass. That
// decision is a property of the PRESET, not of the surface, so it belongs in
// one place -- and it existed twice.
//
// The two copies had already drifted, which is the argument for this made
// concrete. The primary's second blend pass read
//
//     if (bNewUsesWarpShader && m_dx12WarpBlendPSO)        { draw }
//     else if (!bNewUsesWarpShader)                        { draw fallback }
//
// leaving `bNewUsesWarpShader && !m_dx12WarpBlendPSO` covered by NEITHER
// branch, so the incoming preset was not drawn at all. That is reachable two
// ways: the blend PSO is Reset() when a blend completes (milkdropfs.cpp:983)
// while m_dx12WarpPSO is not, and its creation is guarded on
// m_shaders.warp.bytecodeBlob while bNewUsesWarpShader comes from
// m_nWarpPSVersion -- two independent ways to have one true and the other null.
//
// The mirror already guarded it, with `(m_dx12WarpBlendPSO || m_dx12WarpPSO)`.
// The primary's FIRST pass carries a comment describing exactly this shape and
// crediting the mirror for the fix -- so pass 0 was repaired and pass 1 was
// not, in the same function, which is the copy-paste class this issue exists
// to end. Unifying on the mirror's guard fixes it as a side effect.
//
// What stays with the callers: transitions, render target, viewport, and
// building the binding slots. Those differ legitimately -- the primary builds
// bufferA-D bindings and diagnostics the mirror has no use for, and the two
// address different descriptor ranges.
void mdrop::Engine::RenderWarpPass(const RenderContext& ctx) {
  if (!ctx.cmdList || !ctx.pState || !ctx.verts || !m_lpDX) return;
  ID3D12GraphicsCommandList* cmdList = ctx.cmdList;

  auto bindShader = [&](PShaderInfo* si, CState* st,
                        D3D12_GPU_DESCRIPTOR_HANDLE table) {
    if (si && si->CT && st) {
      // Stage into a buffer THIS record owns, not into the shader's shadow
      // (#206) -- see the note on ApplyShaderParams.
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(si->CT);
      std::vector<BYTE> cb12;
      ct->CopyShadowInto(cb12);
      ApplyShaderParams(&si->params, si->CT, st, &ctx, &cb12);
      if (!cb12.empty()) {
        D3D12_GPU_VIRTUAL_ADDRESS cb =
            m_lpDX->UploadConstantBuffer(cb12.data(), (UINT)cb12.size());
        if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);
      }
    } else {
      BYTE zeros[256] = {};
      D3D12_GPU_VIRTUAL_ADDRESS cb = m_lpDX->UploadConstantBuffer(zeros, 256);
      if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);
    }
    cmdList->SetGraphicsRootDescriptorTable(1, table);
  };

  // ctx.cmdList explicitly, never nullptr. DrawIndexedVertices resolves a null
  // list through GetActiveCmdList(), which returns m_cmdListOverride when one
  // is set -- so a null here could send the draw to a different list than the
  // transitions and render-target binding this caller just issued.
  auto drawWarpMesh = [&](D3DCOLOR cDecay, bool bCullTiles, bool bFlipCulling) {
    DrawWarpMeshIndexed(ctx.verts, cDecay, bCullTiles, bFlipCulling, cmdList);
  };

  // Decay travels in the vertex colour only when the shader does not handle it
  // itself. A custom warp shader gets white vertices, because DX9's
  // fixed-function modulate is bypassed when a pixel shader is active and such
  // shaders encode decay in their own maths; an auto-generated one, or the
  // no-shader fallback, needs it as vDiffuse.
  auto decayColor = [](CState* st) -> D3DCOLOR {
    const float d = (float)(*st->var_pf_decay);
    return D3DCOLOR_RGBA_01(d, d, d, 1);
  };

  const bool bBlending = ctx.bBlending;
  const bool bOldUsesWarp = bBlending && ctx.pOldState &&
                            (ctx.pOldState->m_nWarpPSVersion > 0);
  const bool bNewUsesWarp = (ctx.pState->m_nWarpPSVersion > 0);

  if (bBlending && (bOldUsesWarp || bNewUsesWarp)) {
    // EVERY TILE MUST BE DRAWN BY EXACTLY ONE OF THESE TWO PASSES (#188).
    //
    // The two culls are complementary by construction -- pass 0 drops a tile
    // only when every vertex alpha is 255, pass 1 only when every one is 0 --
    // so between them they cover the mesh. That guarantee is worth nothing if
    // a pass declines to run at all, and both used to:
    //
    //     if (bOldUsesWarp && (psos.oldWarp || psos.warp))  { draw }
    //     else if (!bOldUsesWarp && pOldState)              { draw fallback }
    //
    // A side that WANTS a warp shader but has neither PSO matched neither
    // branch, so its tiles were never written and the feedback buffer kept
    // whatever was there -- black. Visible as hard-edged black regions along
    // the .milk2 cutout, which is what #188 was reported as, and confirmed by
    // capturing VS[1] directly: the black is already in the warp destination,
    // before any comp shader runs.
    //
    // The no-blend branch below always had this right -- `psos.warp ?
    // psos.warp : PSO_TEXTURED_MYVERTEX` -- so this is the same rule applied
    // to the two passes that did not have it. An earlier fix narrowed the hole
    // by adding `|| psos.warp` to the conditions; it did not close it, because
    // both PSOs can be null together.
    //
    // Pass 0: old preset, opaque, culling tiles fully blended to the new one.
    if (ctx.pOldState) {
      ID3D12PipelineState* oldPso =
          bOldUsesWarp ? (ctx.psos.oldWarp ? ctx.psos.oldWarp : ctx.psos.warp)
                       : nullptr;
      if (oldPso) {
        cmdList->SetPipelineState(oldPso);
        bindShader(&ctx.oldShaders->warp, ctx.pOldState, ctx.oldWarpTable);
        drawWarpMesh(ctx.pOldState->m_bAutoGenWarpShader ? decayColor(ctx.pOldState)
                                                         : 0xFFFFFFFFu,
                     true, true);
      } else {
        cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
        bindShader(nullptr, nullptr, ctx.oldWarpTable);
        drawWarpMesh(decayColor(ctx.pOldState), true, true);
      }
    }

    // Pass 1: new preset, alpha blended, culling tiles that are fully old.
    {
      ID3D12PipelineState* newPso =
          bNewUsesWarp ? (ctx.psos.warpBlend ? ctx.psos.warpBlend : ctx.psos.warp)
                       : nullptr;
      if (newPso) {
        cmdList->SetPipelineState(newPso);
        bindShader(&ctx.shaders->warp, ctx.pState, ctx.warpTable);
        drawWarpMesh(ctx.pState->m_bAutoGenWarpShader ? decayColor(ctx.pState)
                                                      : 0xFFFFFFFFu,
                     true, false);
      } else {
        // No warp shader, or one whose PSO is missing: the fallback PSO has no
        // alpha blend, but for this case the UV blending is sufficient -- DX9
        // uses a single pass here too.
        cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
        bindShader(nullptr, nullptr, ctx.warpTable);
        drawWarpMesh(decayColor(ctx.pState), true, false);
      }
    }
  } else if (bBlending && !bOldUsesWarp && !bNewUsesWarp) {
    // Neither preset uses a warp shader: UV blending alone is enough.
    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    bindShader(nullptr, nullptr, ctx.warpTable);
    drawWarpMesh(decayColor(ctx.pState), false, false);
  } else {
    // No blend: one pass.
    cmdList->SetPipelineState(ctx.psos.warp
                                  ? ctx.psos.warp
                                  : m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    bindShader(&ctx.shaders->warp, ctx.pState, ctx.warpTable);
    drawWarpMesh((ctx.psos.warp && !ctx.pState->m_bAutoGenWarpShader)
                     ? 0xFFFFFFFFu
                     : decayColor(ctx.pState),
                 false, false);
  }
}

// ScanHighestBlurUsed - how many blur levels this frame's shaders sample (#15)
//
// Both render bodies scanned the same four CShaderParams for TEX_BLUR* codes;
// the primary accumulated into the member m_nHighestBlurTexUsedThisFrame and
// the mirror into a local. Same loop, written twice.
int mdrop::Engine::ScanHighestBlurUsed(bool bBlending,
                                       const RenderContext* rc) const {
  int highest = 0;
  auto scan = [&](const CShaderParams* sp) {
    if (!sp) return;
    for (int i = 0; i < 32; i++) {
      if (sp->m_texcode[i] >= TEX_BLUR1 && sp->m_texcode[i] <= TEX_BLUR_LAST)
        highest = max(highest, ((int)sp->m_texcode[i] - (int)TEX_BLUR1) + 1);
    }
  };
  // THIS surface's set when it has one. Reading m_shaders here meant an
  // own-preset mirror sized its pyramid from the primary's preset (#208).
  const PShaderSet* cur = (rc && rc->shaders) ? rc->shaders : &m_shaders;
  scan(&cur->warp.params);
  scan(&cur->comp.params);
  // A blend samples the outgoing preset's shaders too, so their blur levels
  // have to be generated as well or they read whatever was last left there.
  if (bBlending) {
    const PShaderSet* old = (rc && rc->oldShaders) ? rc->oldShaders : &m_OldShaders;
    scan(&old->warp.params);
    scan(&old->comp.params);
  }
  return highest;
}

// RenderBlurPasses - the separable blur pyramid, for any surface (#15)
//
// The same loop existed twice: once as the primary's DX12_BlurPasses() and
// once inlined into the mirror body; both are gone, replaced by this. Identical weights, identical progressive
// scale/bias derivation, identical two-shader ping-pong and constant layout --
// only the source texture, the destination pyramid and the sizes differed.
//
// Unlike warp and comp there is no preset-state branching here, so this did not
// hide a drifted guard. What it did hide is worse in a quieter way: a change to
// the blur weights or the scale/bias maths applied to one surface and not the
// other would be invisible on the primary and wrong on every mirror.
//
// A mirror builds its own pyramid rather than sampling the primary's, and that
// is deliberate -- a blur generated at a different aspect makes presets that
// lean on GetBlur1/2 look mushy.
void mdrop::Engine::RenderBlurPasses(const RenderContext& ctx) {
#if (NUM_BLUR_TEX > 0)
  if (!m_lpDX || !m_lpDX->m_device || !ctx.cmdList) return;
  if (!ctx.blurSrc || !ctx.blurTex || !ctx.pState) return;
  m_lpDX->BeginStage("blur");   // #222 stage map

  const int passes = min(NUM_BLUR_TEX, ctx.highestBlurUsed * 2);
  if (passes <= 0) return;
  if (!m_dx12BlurPSO[0] || !m_dx12BlurPSO[1]) return;
  if (!ctx.blurTex[0].IsValid()) return;

  ID3D12GraphicsCommandList* cmdList = ctx.cmdList;

  const float w[8] = { 4.0f, 3.8f, 3.5f, 2.9f, 1.9f, 1.2f, 0.7f, 0.3f };
  const float edge_darken = (float)*ctx.pState->var_pf_blur1_edge_darken;
  float blur_min[3], blur_max[3];
  GetSafeBlurMinMax(ctx.pState, blur_min, blur_max);

  // Progressive scale & bias: each level is renormalised against the range the
  // level below it already compressed into.
  float fscale[3], fbias[3];
  fscale[0] = 1.0f / (blur_max[0] - blur_min[0]);
  fbias[0] = -blur_min[0] * fscale[0];
  float temp_min = (blur_min[1] - blur_min[0]) / (blur_max[0] - blur_min[0]);
  float temp_max = (blur_max[1] - blur_min[0]) / (blur_max[0] - blur_min[0]);
  fscale[1] = 1.0f / (temp_max - temp_min);
  fbias[1] = -temp_min * fscale[1];
  temp_min = (blur_min[2] - blur_min[1]) / (blur_max[1] - blur_min[1]);
  temp_max = (blur_max[2] - blur_min[1]) / (blur_max[1] - blur_min[1]);
  fscale[2] = 1.0f / (temp_max - temp_min);
  fbias[2] = -temp_min * fscale[2];

  MYVERTEX v[4];
  ZeroMemory(v, sizeof(v));
  v[0].x = -1; v[0].y =  1; v[0].z = 0; v[0].Diffuse = 0xFFFFFFFF; v[0].tu = 0; v[0].tv = 0;
  v[1].x =  1; v[1].y =  1; v[1].z = 0; v[1].Diffuse = 0xFFFFFFFF; v[1].tu = 1; v[1].tv = 0;
  v[2].x = -1; v[2].y = -1; v[2].z = 0; v[2].Diffuse = 0xFFFFFFFF; v[2].tu = 0; v[2].tv = 1;
  v[3].x =  1; v[3].y = -1; v[3].z = 0; v[3].Diffuse = 0xFFFFFFFF; v[3].tu = 1; v[3].tv = 1;

  // The blur shaders use their own root signature: SM5.0 puts their single
  // sampler at s0, and DX9's blur passes address with CLAMP.
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_blurRootSignature.Get());

  // One staging buffer per blur direction, owned by THIS record rather than by
  // the shader (#206). m_BlurShaders[0..1] are engine-wide, so writing the
  // constants into their own shadow buffers meant two surfaces blurring at the
  // same time each uploaded a blend of both surfaces' values.
  std::vector<BYTE> cbLocal[2];
  bool cbLocalReady[2] = { false, false };

  for (int i = 0; i < passes; i++) {
    DX12Texture& srcTex = (i == 0) ? *ctx.blurSrc : ctx.blurTex[i - 1];
    DX12Texture& dstTex = ctx.blurTex[i];

    m_lpDX->TransitionResource(srcTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);
    m_lpDX->TransitionResource(dstTex, D3D12_RESOURCE_STATE_RENDER_TARGET, cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_lpDX->GetRtvCpuHandle(dstTex);
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    SetViewportAndScissor(cmdList, dstTex.width, dstTex.height);

    // Even passes blur horizontally, odd passes vertically.
    cmdList->SetPipelineState(m_dx12BlurPSO[i % 2].Get());
    m_lpDX->UpdateBlurPassBinding(i, srcTex.srvIndex, ctx.blurBindBase);
    cmdList->SetGraphicsRootDescriptorTable(
        1, m_lpDX->GetBlurPassBindingGpuHandle(i, ctx.blurBindBase));

    DX12ConstantTable* ct =
        static_cast<DX12ConstantTable*>(m_BlurShaders[i % 2].ps.CT);
    D3DXHANDLE* h = m_BlurShaders[i % 2].ps.params.const_handles;
    if (!ct) continue;
    std::vector<BYTE>& cb = cbLocal[i % 2];
    if (!cbLocalReady[i % 2]) {
      ct->CopyShadowInto(cb);
      cbLocalReady[i % 2] = true;
    }

    int srcw = (i == 0) ? ctx.blurSrcW : (ctx.blurW ? ctx.blurW[i - 1] : (int)srcTex.width);
    int srch = (i == 0) ? ctx.blurSrcH : (ctx.blurH ? ctx.blurH[i - 1] : (int)srcTex.height);
    if (srcw < 1) srcw = GetWidth();
    if (srch < 1) srch = GetHeight();
    D3DXVECTOR4 srctexsize((float)srcw, (float)srch, 1.0f / (float)srcw, 1.0f / (float)srch);

    const float fscale_now = fscale[i / 2];
    const float fbias_now = fbias[i / 2];

    if (i % 2 == 0) {
      const float w1 = w[0] + w[1];
      const float w2 = w[2] + w[3];
      const float w3 = w[4] + w[5];
      const float w4 = w[6] + w[7];
      const float d1 = 0 + 2 * w[1] / w1;
      const float d2 = 2 + 2 * w[3] / w2;
      const float d3 = 4 + 2 * w[5] / w3;
      const float d4 = 6 + 2 * w[7] / w4;
      const float w_div = 0.5f / (w1 + w2 + w3 + w4);
      if (h[0]) ct->WriteVectorInto(cb, h[0], srctexsize);
      if (h[1]) ct->WriteVectorInto(cb, h[1], D3DXVECTOR4(w1, w2, w3, w4));
      if (h[2]) ct->WriteVectorInto(cb, h[2], D3DXVECTOR4(d1, d2, d3, d4));
      if (h[3]) ct->WriteVectorInto(cb, h[3], D3DXVECTOR4(fscale_now, fbias_now, w_div, 0));
    } else {
      const float w1 = w[0] + w[1] + w[2] + w[3];
      const float w2 = w[4] + w[5] + w[6] + w[7];
      const float d1 = 0 + 2 * ((w[2] + w[3]) / w1);
      const float d2 = 2 + 2 * ((w[6] + w[7]) / w2);
      const float w_div = 1.0f / ((w1 + w2) * 2);
      if (h[0]) ct->WriteVectorInto(cb, h[0], srctexsize);
      if (h[5]) ct->WriteVectorInto(cb, h[5], D3DXVECTOR4(w1, w2, d1, d2));
      if (h[6]) {
        // Only the first vertical pass carries the edge darkening.
        if (i == 1)
          ct->WriteVectorInto(cb, h[6], D3DXVECTOR4(w_div, (1 - edge_darken), edge_darken, 5.0f));
        else
          ct->WriteVectorInto(cb, h[6], D3DXVECTOR4(w_div, 1.0f, 0.0f, 5.0f));
      }
    }

    if (!cb.empty()) {
      D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
          m_lpDX->UploadConstantBuffer(cb.data(), (UINT)cb.size());
      if (cbAddr) cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
    }
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
  }

  for (int i = 0; i < passes; i++)
    m_lpDX->TransitionResource(ctx.blurTex[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, cmdList);

  // Hand the main root signature back. The mirror body always did this; the
  // primary relied on whatever drew next setting its own, which is true today
  // but leaves the blur root signature bound in between for no reason.
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
#endif
}

// RenderCompPass - the comp draw dispatch, for any surface (#15)
//
// The second half of the same story as RenderWarpPass, and it carried the same
// defect in the same position. The primary's second blend pass read
//
//     if (bNewUsesCompShader && m_dx12CompBlendPSO) { draw }
//     else if (!bNewUsesCompShader)                 { draw fallback }
//
// leaving `bNewUsesCompShader && !m_dx12CompBlendPSO` covered by neither
// branch, and m_dx12CompBlendPSO is Reset() when a blend completes
// (milkdropfs.cpp:983) exactly like the warp one. Its pass 0 even carries the
// comment "Same silent-no-draw shape as the warp guard above; mirrored from
// :3038" -- so for the SECOND time, pass 0 was repaired from the mirror and
// pass 1 was left alone.
//
// Two copies, two stages, the same half-applied fix. That is the argument for
// this issue better than any description of it.
//
// The hue-shader corner colours are computed here rather than by the callers
// because they are pure preset maths -- fShader, GetTime() and m_fRandStart --
// and both bodies had a verbatim copy, one of which had the two inner loops
// fused. Same result, but two copies of a thing nobody would notice diverging.
void mdrop::Engine::RenderCompPass(const RenderContext& ctx) {
  if (!ctx.cmdList || !ctx.pState || !m_lpDX) return;
  ID3D12GraphicsCommandList* cmdList = ctx.cmdList;

  // Custom comp shaders always shade at full strength (DX9 ShowToUser_Shaders
  // hardcodes 1.0); an auto-generated one uses the preset's own fShader and
  // blends toward white (ShowToUser_NoShaders). m_nCompPSVersion > 0 is true
  // for both, so m_bAutoGenCompShader is what separates them.
  const bool bCustomComp = (ctx.pState->m_nCompPSVersion > 0) &&
                           !ctx.pState->m_bAutoGenCompShader;
  float shade[4][3] = {
    { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }
  };
  const float fShaderAmount =
      bCustomComp ? 1.0f : ctx.pState->m_fShader.eval(GetTime());
  if (fShaderAmount > 0.001f) {
    for (int i = 0; i < 4; i++) {
      shade[i][0] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0143f + 3 + i * 21 + m_fRandStart[3]);
      shade[i][1] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0107f + 1 + i * 13 + m_fRandStart[1]);
      shade[i][2] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0129f + 6 + i * 9 + m_fRandStart[2]);
      float mx = ((shade[i][0] > shade[i][1]) ? shade[i][0] : shade[i][1]);
      if (shade[i][2] > mx) mx = shade[i][2];
      for (int k = 0; k < 3; k++) {
        shade[i][k] /= mx;
        shade[i][k] = 0.5f + 0.5f * shade[i][k];
        shade[i][k] = shade[i][k] * fShaderAmount + 1.0f * (1.0f - fShaderAmount);
      }
    }
  }
  const DWORD cShade[4] = {
    D3DCOLOR_RGBA_01(shade[1][0], shade[1][1], shade[1][2], 1),  // top-left
    D3DCOLOR_RGBA_01(shade[0][0], shade[0][1], shade[0][2], 1),  // top-right
    D3DCOLOR_RGBA_01(shade[3][0], shade[3][1], shade[3][2], 1),  // bottom-left
    D3DCOLOR_RGBA_01(shade[2][0], shade[2][1], shade[2][2], 1),  // bottom-right
  };

  auto bindShader = [&](PShaderInfo* si, CState* st,
                        D3D12_GPU_DESCRIPTOR_HANDLE table) {
    if (si && si->CT && st) {
      // Stage into a buffer THIS record owns, not into the shader's shadow
      // (#206) -- see the note on ApplyShaderParams.
      DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(si->CT);
      std::vector<BYTE> cb12;
      ct->CopyShadowInto(cb12);
      ApplyShaderParams(&si->params, si->CT, st, &ctx, &cb12);
      if (!cb12.empty()) {
        D3D12_GPU_VIRTUAL_ADDRESS cb =
            m_lpDX->UploadConstantBuffer(cb12.data(), (UINT)cb12.size());
        if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);
      }
    } else {
      BYTE zeros[256] = {};
      D3D12_GPU_VIRTUAL_ADDRESS cb = m_lpDX->UploadConstantBuffer(zeros, 256);
      if (cb) cmdList->SetGraphicsRootConstantBufferView(0, cb);
    }
    cmdList->SetGraphicsRootDescriptorTable(1, table);
  };

  // ctx.cmdList explicitly -- the primary used to let this default to
  // GetActiveCmdList(). See the note in RenderWarpPass.
  auto drawCompQuad = [&](BYTE alpha) {
    auto applyAlpha = [](DWORD color, BYTE a) -> DWORD {
      return (color & 0x00FFFFFF) | ((DWORD)a << 24);
    };
    MYVERTEX quad[4];
    ZeroMemory(quad, sizeof(quad));
    quad[0].x = -1.f; quad[0].y =  1.f; quad[0].z = 0.f; quad[0].Diffuse = applyAlpha(cShade[0], alpha);
    quad[0].tu = 0.f; quad[0].tv = 0.f; quad[0].tu_orig = 0.f; quad[0].tv_orig = 0.f;
    quad[0].rad = 1.f; quad[0].ang = 3.14159f;
    quad[1].x =  1.f; quad[1].y =  1.f; quad[1].z = 0.f; quad[1].Diffuse = applyAlpha(cShade[1], alpha);
    quad[1].tu = 1.f; quad[1].tv = 0.f; quad[1].tu_orig = 1.f; quad[1].tv_orig = 0.f;
    quad[1].rad = 1.f; quad[1].ang = 0.f;
    quad[2].x = -1.f; quad[2].y = -1.f; quad[2].z = 0.f; quad[2].Diffuse = applyAlpha(cShade[2], alpha);
    quad[2].tu = 0.f; quad[2].tv = 1.f; quad[2].tu_orig = 0.f; quad[2].tv_orig = 1.f;
    quad[2].rad = 1.f; quad[2].ang = 3.14159f;
    quad[3].x =  1.f; quad[3].y = -1.f; quad[3].z = 0.f; quad[3].Diffuse = applyAlpha(cShade[3], alpha);
    quad[3].tu = 1.f; quad[3].tv = 1.f; quad[3].tu_orig = 1.f; quad[3].tv_orig = 1.f;
    quad[3].rad = 1.f; quad[3].ang = 0.f;
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, quad, 4,
                         sizeof(MYVERTEX), cmdList);
  };

  const bool bBlending = ctx.bBlending;
  const bool bOldUsesComp = bBlending && ctx.pOldState &&
                            (ctx.pOldState->m_nCompPSVersion > 0);
  const bool bNewUsesComp = (ctx.pState->m_nCompPSVersion > 0);

  // FIRST in the chain, not third (#188). This used to sit after the blending
  // branch, so on any preset that was blending it never ran -- and a frozen
  // .milk2 blends for as long as it is loaded, which makes "show me the raw
  // feedback buffer" silently do nothing on precisely the presets whose
  // feedback is in question. A diagnostic override that quietly declines to
  // override is worse than not having one.
  if (!ctx.isMirror && m_nDiagDisplayMode > 0) {
    // Raw VS[0] or VS[1] with no comp shader.
    DX12Texture& diagTex = (m_nDiagDisplayMode == 1) ? m_dx12VS[0] : m_dx12VS[1];
    m_lpDX->TransitionResource(diagTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    bindShader(nullptr, nullptr, m_lpDX->GetBindingBlockGpuHandle(diagTex));
    drawCompQuad(0xFF);
  } else if (bBlending && (bOldUsesComp || bNewUsesComp)) {
    // Per-vertex alpha from the warp wipe mesh, at THIS surface's aspect --
    // a uniform quad alpha ignores the .milk2 blending pattern.
    // Both passes draw the SAME staged mesh, which is why the buffer is
    // taken once here rather than reached for inside each draw: the two blend
    // passes cull against each other's alphas, so they must see identical
    // vertices (#206).
    const MYVERTEX* compVerts =
        UpdateCompMeshBlendColors(cShade, ctx.pState, ctx.verts);

    if (bOldUsesComp && (ctx.psos.oldComp || ctx.psos.comp)) {
      cmdList->SetPipelineState(ctx.psos.oldComp ? ctx.psos.oldComp
                                                 : ctx.psos.comp);
      bindShader(&ctx.oldShaders->comp, ctx.pOldState, ctx.oldCompTable);
      DrawCompMesh(compVerts, true, true, cmdList);
    } else if (!bOldUsesComp) {
      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
      bindShader(nullptr, nullptr, ctx.oldCompTable);
      DrawCompMesh(compVerts, true, true, cmdList);
    }

    // `|| m_dx12CompPSO` is the guard the primary was missing -- see above.
    if (bNewUsesComp && (ctx.psos.compBlend || ctx.psos.comp)) {
      cmdList->SetPipelineState(ctx.psos.compBlend ? ctx.psos.compBlend
                                                   : ctx.psos.comp);
      bindShader(&ctx.shaders->comp, ctx.pState, ctx.compTable);
      DrawCompMesh(compVerts, true, false, cmdList);
    } else if (!bNewUsesComp) {
      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
      bindShader(nullptr, nullptr, ctx.compTable);
      DrawCompMesh(compVerts, true, false, cmdList);
    }
  } else {
    // No blend: one pass. The m_nCompPSVersion check is the mirror's, and is
    // the more careful form -- m_dx12CompPSO can outlive the preset that built
    // it, and without it a preset with no comp shader would be drawn through
    // the previous preset's.
    if (ctx.psos.comp && ctx.pState->m_nCompPSVersion > 0)
      cmdList->SetPipelineState(ctx.psos.comp);
    else
      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
    bindShader(&ctx.shaders->comp, ctx.pState, ctx.compTable);
    drawCompQuad(0xFF);
  }
}

// ApplyRenderMitigations - every in-sequence mitigation, for any surface (#15)
//
// The primary and the mirror worker are two hand-copied render bodies, and a
// mitigation written into one does not reach the other. That is not a
// hypothetical: canvasMax shipped that way on 2026-08-23 and needed
// ClampOrientCanvas added to reach the mirror, and the feedback damp shipped
// the same way three days later in v2.11.0 (`09dadf8`). Two defects, four
// days, one shape.
//
// So the mitigations move here and both bodies call this instead. A third one
// is added inside this function, once, and reaches every surface by default --
// which is the whole of what #15 asks for. The comment this replaces in the
// mirror body ended "If a third mitigation is added, add it here too", and an
// instruction to remember something is exactly the thing being removed.
//
// Why a context rather than a damp value: each surface is damped for ITS OWN
// canvas, not the engine's. `EffectiveFeedbackDamp()` reads m_nTexSizeX/Y off
// the engine, so a mirror calling it would be damped for the primary's size
// instead of its sim size. ctx.LongEdge() is the per-surface answer, and it is
// the only reason this could not simply have been hoisted as-is.
void mdrop::Engine::ApplyRenderMitigations(const RenderContext& ctx) {
  if (!ctx.cmdList) return;

  // Feedback damp. Called at the point where the write target holds ONLY the
  // warped history, which is the only thing that should be attenuated -- both
  // callers invoke this immediately after their warp pass for that reason.
  ApplyFeedbackDamp(ctx.cmdList, FeedbackDampForEdge(ctx.LongEdge()));
}

void mdrop::Engine::DX12_DrawSprites(CState* pStateOverride,
                                     const RenderContext* pCtx) {
  if (!m_lpDX || !CmdListFor(pCtx))
    return;
  m_lpDX->BeginStage("darkencenter+borders");   // #222 stage map

  // darken_center and the inner/outer borders are PRESET settings, so they
  // follow the surface's own preset rather than the engine's (#186).
  CState* pState = pStateOverride ? pStateOverride : m_pState;
  if (!pState)
    return;

  // Whose canvas. Same argument as the state above: these were the ENGINE's
  // aspect and texsize, which SizeGuard had to overwrite for the duration of a
  // mirror record precisely because they are not this surface's (#121).
  // haveCanvas for ALL of them, not just the sizes. A context that carries no
  // canvas has aspect 1.0 by default, so testing `pCtx ?` here would hand a
  // canvas-less context square aspect instead of the engine's -- the same
  // defect 43e807c7 fixed one line lower for texsize, where `rc ?` fed 0 in and
  // bound (0,0,inf,inf). Both callers happen to be unambiguous today; this
  // stops the next one from having to notice.
  const bool  haveCanvas = pCtx && pCtx->canvasW > 0 && pCtx->canvasH > 0;
  const float aspectX    = haveCanvas ? pCtx->fAspectX    : m_fAspectX;
  const float aspectY    = haveCanvas ? pCtx->fAspectY    : m_fAspectY;
  const float invAspectX = haveCanvas ? pCtx->fInvAspectX : m_fInvAspectX;
  const float invAspectY = haveCanvas ? pCtx->fInvAspectY : m_fInvAspectY;
  const int   texSizeX   = haveCanvas ? pCtx->canvasW : m_nTexSizeX;
  const int   texSizeY   = haveCanvas ? pCtx->canvasH : m_nTexSizeY;

  auto* cmdList = CmdListFor(pCtx);

  // Darken center
  if (*pState->var_pf_darken_center) {
    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_ALPHABLEND_WFVERTEX].Get());

    WFVERTEX v3[6];
    ZeroMemory(v3, sizeof(WFVERTEX) * 6);

    v3[0].Diffuse = D3DCOLOR_RGBA_01(0, 0, 0, 3.0f / 32.0f);
    v3[1].Diffuse = D3DCOLOR_RGBA_01(0, 0, 0, 0.0f / 32.0f);
    v3[2].Diffuse = v3[1].Diffuse;
    v3[3].Diffuse = v3[1].Diffuse;
    v3[4].Diffuse = v3[1].Diffuse;
    v3[5].Diffuse = v3[1].Diffuse;

    float fHalfSize = 0.05f;
    v3[0].x = 0.0f;
    if (m_bScreenDependentRenderMode)
      v3[1].x = 0.0f - fHalfSize;
    else
      v3[1].x = 0.0f - fHalfSize * aspectY;
    v3[2].x = 0.0f;
    if (m_bScreenDependentRenderMode)
      v3[3].x = 0.0f + fHalfSize;
    else
      v3[3].x = 0.0f + fHalfSize * aspectY;
    v3[4].x = 0.0f;
    v3[5].x = v3[1].x;
    v3[0].y = 0.0f;
    v3[1].y = 0.0f;
    v3[2].y = 0.0f - fHalfSize;
    v3[3].y = 0.0f;
    v3[4].y = 0.0f + fHalfSize;
    v3[5].y = v3[1].y;

    // 6-vert fan → 4 triangles → 12 verts
    WFVERTEX triVerts[12];
    int nTriVerts = ExpandFanToTriList(v3, 6, triVerts);
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, nTriVerts, sizeof(WFVERTEX));
  }

  // Borders (outer + inner)
  {
    float fOuterBorderSize = (float)*pState->var_pf_ob_size;
    float fInnerBorderSize = (float)*pState->var_pf_ib_size;

    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_ALPHABLEND_WFVERTEX].Get());

    for (int it = 0; it < 2; it++) {
      WFVERTEX v3[4];
      ZeroMemory(v3, sizeof(WFVERTEX) * 4);

      float r = (it == 0) ? (float)*pState->var_pf_ob_r : (float)*pState->var_pf_ib_r;
      float g = (it == 0) ? (float)*pState->var_pf_ob_g : (float)*pState->var_pf_ib_g;
      float b = (it == 0) ? (float)*pState->var_pf_ob_b : (float)*pState->var_pf_ib_b;
      float a = (it == 0) ? (float)*pState->var_pf_ob_a : (float)*pState->var_pf_ib_a;
      if (a > 0.001f) {
        v3[0].Diffuse = D3DCOLOR_RGBA_01(r, g, b, a);
        v3[1].Diffuse = v3[0].Diffuse;
        v3[2].Diffuse = v3[0].Diffuse;
        v3[3].Diffuse = v3[0].Diffuse;

        float fInnerRad = (it == 0) ? 1.0f - fOuterBorderSize : 1.0f - fOuterBorderSize - fInnerBorderSize;
        float fOuterRad = (it == 0) ? 1.0f : 1.0f - fOuterBorderSize;
        v3[0].x = fInnerRad;
        v3[1].x = fOuterRad;
        v3[2].x = fOuterRad;
        v3[3].x = fInnerRad;
        v3[0].y = fInnerRad;
        v3[1].y = fOuterRad;
        v3[2].y = -fOuterRad;
        v3[3].y = -fInnerRad;

        for (int rot = 0; rot < 4; rot++) {
          // 4-vert fan → 2 triangles → 6 verts
          WFVERTEX triVerts[6];
          int nTriVerts = ExpandFanToTriList(v3, 4, triVerts);
          m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, nTriVerts, sizeof(WFVERTEX));

          // rotate by 90 degrees
          for (int vi = 0; vi < 4; vi++) {
            float t = 1.570796327f;
            float x = v3[vi].x;
            float y = v3[vi].y;
            v3[vi].x = x * cosf(t) - y * sinf(t);
            v3[vi].y = x * sinf(t) + y * cosf(t);
          }
        }
      }
    }
  }
}

void mdrop::Engine::DX12_DrawCustomShapes(CState* pNewOverride, CState* pOldOverride,
                                          const RenderContext* pCtx) {
  if (!m_lpDX || !CmdListFor(pCtx))
    return;
  m_lpDX->BeginStage("shapes");   // #222 stage map

  // Mirror sim contexts pass their own states; null = primary (unchanged).
  CState* pNewState = pNewOverride ? pNewOverride : m_pState;
  CState* pOldStateL = pNewOverride ? pOldOverride : m_pOldState;

  // Whose canvas. Same argument as the state above: these were the ENGINE's
  // aspect and texsize, which SizeGuard had to overwrite for the duration of a
  // mirror record precisely because they are not this surface's (#121).
  // haveCanvas for ALL of them, not just the sizes. A context that carries no
  // canvas has aspect 1.0 by default, so testing `pCtx ?` here would hand a
  // canvas-less context square aspect instead of the engine's -- the same
  // defect 43e807c7 fixed one line lower for texsize, where `rc ?` fed 0 in and
  // bound (0,0,inf,inf). Both callers happen to be unambiguous today; this
  // stops the next one from having to notice.
  const bool  haveCanvas = pCtx && pCtx->canvasW > 0 && pCtx->canvasH > 0;
  const float aspectX    = haveCanvas ? pCtx->fAspectX    : m_fAspectX;
  const float aspectY    = haveCanvas ? pCtx->fAspectY    : m_fAspectY;
  const float invAspectX = haveCanvas ? pCtx->fInvAspectX : m_fInvAspectX;
  const float invAspectY = haveCanvas ? pCtx->fInvAspectY : m_fInvAspectY;
  const int   texSizeX   = haveCanvas ? pCtx->canvasW : m_nTexSizeX;
  const int   texSizeY   = haveCanvas ? pCtx->canvasH : m_nTexSizeY;
  const bool bStateBlending = pNewState->m_bBlending && pOldStateL;

  auto* cmdList = CmdListFor(pCtx);

  // DIAG (flicker hunt 2026-08-21): shapes flatten to ellipses on mirrors when
  // aspectY deviates from the orient value during a ctx record. Count it.
  // THIS surface's expected aspect. It compared against the FIRST surface's
  // pipe, so with several displays aspBad/aspGood were scored against the wrong
  // one -- which also means earlier aspBad=0 runs proved less than they looked.
  // Test the VALUE, not merely that a context exists -- the rule RenderContext
  // documents, and the one 43e807c7 had to fix for the canvas.
  const bool orientRecord = (pCtx && pCtx->bOrientRecord);
  if (orientRecord && haveCanvas && texSizeX > 0 && texSizeY > 0) {
    const float expect = (texSizeX > texSizeY)
        ? texSizeY / (float)texSizeX
        : 1.0f;
    if (fabsf(aspectY - expect) > 0.01f) {
      m_diagOrientAspectBad.fetch_add(1, std::memory_order_relaxed);
      DLOG_WARN("OrientAspect BAD during ctx shape draw: aspY=%.4f expect=%.4f (count=%u)",
                aspectY, expect, m_diagOrientAspectBad.load(std::memory_order_relaxed));
    } else {
      m_diagOrientAspectGood.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // --- Heavy preset detection: compute total instances across all shapes ---
  //
  // Hoisted out of the m_bSkipHeavyPresets branch (#230): the mirror GPU-cost
  // cap below needs this count whether or not that opt-in setting is on, and
  // it is sixteen adds.
  int totalInstances = 0;
  for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
    if (pNewState->m_shape[i].enabled)
      totalInstances += pNewState->m_shape[i].instances;
    if (bStateBlending && pOldStateL->m_shape[i].enabled)
      totalInstances += pOldStateL->m_shape[i].instances;
  }

  if (m_bSkipHeavyPresets) {
    if (totalInstances > m_nHeavyPresetMaxInstances) {
      DLOG_INFO("GPU Protection: Skipping shapes — total instances %d exceeds threshold %d (preset: %ls)",
              totalInstances, m_nHeavyPresetMaxInstances,
              CurrentPresetLeaf());
      return;
    }
  }

  // --- Compute effective instance cap (resolution scaling + hard cap) ---
  int effectiveMaxInstances = 0; // 0 = unlimited
  if (m_bScaleInstancesByResolution && texSizeX > m_nInstanceScaleBaseWidth) {
    // Scale instances inversely with pixel count relative to base resolution
    // e.g. at 4K (3840) with base 1920: scale = (1920/3840)^2 = 0.25 → cap ~256
    // e.g. at 5K (5120) with base 1920: scale = (1920/5120)^2 = 0.14 → cap ~143
    float scale = (float)m_nInstanceScaleBaseWidth / (float)texSizeX;
    scale = scale * scale; // squared — proportional to pixel count ratio
    if (scale < 0.1f) scale = 0.1f;
    effectiveMaxInstances = (int)(1024.0f * scale);
    if (effectiveMaxInstances < 16) effectiveMaxInstances = 16;
  }
  if (m_nMaxShapeInstances > 0) {
    if (effectiveMaxInstances == 0 || m_nMaxShapeInstances < effectiveMaxInstances)
      effectiveMaxInstances = m_nMaxShapeInstances;
  }
  // Independent mirror re-render (cmdListOverride): cap from remaining aux upload so
  // milk2 dual-pass never silent-drops draws (black mirrors). Same-orient re-render
  // gets a higher soft ceiling; opposite-aspect already spent more of the buffer on warp.
  // pCtx->isMirror, NOT m_lpDX->m_cmdListOverride (#230).
  //
  // That member has ZERO assignments anywhere in the tree -- it is always
  // nullptr -- so everything in this block has been dead code: the aux-upload
  // budget, kSoftMaxOpposite, kSoftMaxSameOrient, the whole per-mirror
  // instance ceiling. Mirror workers set the THREAD-LOCAL override instead
  // (SetThreadCmdListOverride), and the header says plainly that the member
  // "exists to paper over exactly these sites and is being removed one at a
  // time". This site was left behind when it went.
  //
  // So a mirror safety budget that reads as carefully tuned has never once
  // run. The RenderContext knows the answer and is the thing every other
  // per-surface decision in this function already asks.
  if (pCtx && pCtx->isMirror) {
    const UINT kReserveBytes = 2u * 1024u * 1024u;   // waves + HUD + comp CBs
    const UINT kBytesPerInst = 12u * 1024u;          // ~100-gon fill + border (conservative)
    const int kSoftMaxSameOrient = 1536;             // letterbox path rarely hits this
    const int kSoftMaxOpposite   = 1024;             // dual classic pipeline + shapes
    const int kSoftMin           = 32;

    UINT rem = m_lpDX->GetUploadBytesRemaining();
    int budget = 0;
    if (rem > kReserveBytes)
      budget = (int)((rem - kReserveBytes) / kBytesPerInst);
    // milk2 frozen blend draws shapes twice (new + old) — leave half for the other rep
    if (bStateBlending)
      budget = max(kSoftMin, budget / 2);

    int softMax = orientRecord ? kSoftMaxOpposite : kSoftMaxSameOrient;
    int orientCap = budget;
    if (orientCap < kSoftMin) orientCap = kSoftMin;
    if (orientCap > softMax) orientCap = softMax;

    if (effectiveMaxInstances == 0 || orientCap < effectiveMaxInstances)
      effectiveMaxInstances = orientCap;

    // GPU-COST CAP, mirrors only (#230).
    //
    // The caps above bound the UPLOAD RING. They say nothing about how long
    // the GPU takes, and that is what actually kills the device: every mirror
    // does a full classic re-render, so its command list carries about as many
    // draws as the primary's. Three surfaces is therefore roughly three times
    // the geometry work of one, and a preset with 1300+ shape instances turns
    // its first frame into a watchdog kill.
    //
    // Measured, uncapped, on one fixed preset sequence:
    //
    //     primary alone, 2160x3840      0 device removals in 3 runs
    //     primary + two mirrors         3 in 3, every one of them on
    //                                   'martin - crystal palace 2077.3c.milk'
    //                                   at 1312 instances, ~110 ms after it
    //                                   was applied
    //
    // So the primary's own cost is survivable and the mirrors are the
    // difference. They are the ones capped, and the primary is deliberately
    // left alone: it is what the user is looking at, and degrading that to
    // protect a secondary panel is the wrong trade.
    //
    // Engages only above the instance count the engine already calls heavy,
    // so an ordinary preset's mirrors are untouched and identical to the
    // primary. This is the "protect automatically" half of the rule that says
    // a user must never have to annotate a preset to stop the app dying -- and
    // it replaces a GPU Warning that has been printing this exact number for
    // ages while doing nothing about it.
    const int kMirrorCapAbove = 512;   // == the existing GPU Warning threshold
    if (totalInstances > kMirrorCapAbove) {
      // The budget comes from a measurement rather than a taste: the primary
      // ALONE renders this preset without killing the device, so its own cost
      // is the known-survivable unit. The mirrors together are allowed to add
      // at most HALF of that again, split between them.
      //
      // Scaling by the mirror COUNT is what makes this hold for one panel and
      // for four; a fixed fraction would be too tight for one and too loose
      // for four. Not scaled by mirror CANVAS on purpose -- a mirror renders
      // at a quarter of the primary's pixels and still issues the same number
      // of draws, and it is the draw and vertex work that dominates here, not
      // fill. Dividing by pixels would let the cap go slack exactly where it
      // must not.
      //
      // Dividing by three (one share each) was tried first and measured: it
      // fixed the 1312-instance preset but still died once in six runs on the
      // 1625-instance one, so the budget was too generous at the top end.
      const int nMirrors = (int)MirrorSurfaceCount();
      int gpuCap = totalInstances / (2 * (nMirrors > 0 ? nMirrors : 1));
      if (gpuCap < 64) gpuCap = 64;   // a mirror that draws nothing is worse
      // Report engagement, rate-limited to once per preset. Without this the
      // only evidence a cap is working is the absence of a crash, and a cap
      // that silently never runs looks exactly the same -- which is precisely
      // how the dead m_cmdListOverride branch above survived for so long.
      {
        // Once per distinct instance count, i.e. once per preset that trips
        // it -- NOT once per frame. Keyed on the frame counter it fired three
        // times in six milliseconds, because each surface reaches here inside
        // the same engine frame.
        static int s_lastLoggedTotal = -1;
        if (s_lastLoggedTotal != totalInstances) {
          s_lastLoggedTotal = totalInstances;
          DLOG_WARN("Mirror load shed (#230): total=%d mirrors=%d cap=%d/mirror "
                    "res=%dx%d preset=%ls",
                    totalInstances, nMirrors, gpuCap, texSizeX, texSizeY,
                    CurrentPresetLeaf());
        }
      }
      if (effectiveMaxInstances == 0 || gpuCap < effectiveMaxInstances)
        effectiveMaxInstances = gpuCap;
    }
  }

  int diag_shapesDrawn = 0, diag_shapesVisible = 0;
  float diag_firstVisibleAlpha = 0;
  DWORD diag_firstVisibleColor = 0;

  // SHAPE GEOMETRY EXTREMES (#222).
  //
  // The startup TDR is a genuine timeout on a DrawInstanced, and its shape is
  // the tell: frames cost 0.5 ms, then ONE frame costs 2525 ms. A factor of
  // 5000 is not a preset that is gradually too heavy for the canvas -- that
  // would creep through 50 ms and 200 ms first. It is one frame whose geometry
  // is degenerate.
  //
  // A shape is a fan of `sides` triangles around (x,y) with radius `rad` in
  // clip space, where the whole screen is 2 units across. So rad = 1000 is a
  // polygon covering the screen five hundred times over, and a preset with 850
  // instances of it at 2160x3840 asks the rasteriser for something on the
  // order of 10^13 pixels. That is a watchdog kill, from arithmetic alone.
  // Non-finite values are worse and are never intentional.
  //
  // These counters cost a compare per instance and are logged only when they
  // are actually extreme, so this stays in for the next person who sees a hang.
  double diag_maxRad = 0.0;
  int diag_nonFinite = 0;

  int num_reps = bStateBlending ? 2 : 1;
  for (int rep = 0; rep < num_reps; rep++) {
    CState* pState = (rep == 0) ? pNewState : pOldStateL;
    float alpha_mult = 1;
    if (num_reps == 2)
      alpha_mult = (rep == 0) ? pNewState->m_fBlendProgress : (1 - pNewState->m_fBlendProgress);

    for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
      if (pState->m_shape[i].enabled) {
        int instances = pState->m_shape[i].instances;

        // Apply instance cap
        if (effectiveMaxInstances > 0 && instances > effectiveMaxInstances) {
          // Log once per preset (within first half-second)
          if (GetTime() - m_fPresetStartTime < 0.5f) {
            DLOG_INFO("GPU Protection: Capping shape[%d] instances from %d to %d (res=%dx%d, preset: %ls)",
                    i, instances, effectiveMaxInstances, texSizeX, texSizeY,
                    CurrentPresetLeaf());
          }
          instances = effectiveMaxInstances;
        }

        // Stop before DrawVertices silent-fails (black incomplete frame). Shapes are
        // optional work; the comp mesh drawn after them is not, so reserve enough of the
        // ring that a heavy shape dump can never starve the pass that actually reaches
        // the backbuffer. Mirror aux keeps its tuned 48 KB; the main ring reserves the
        // dual-preset comp mesh (2 * (FCGSX-2)*(FCGSY-2)*2 tris * 3 * 40 B = 310 KB)
        // plus custom waves, sprites and HUD text.
        // isMirror for the same reason as the block above (#230): the member
        // is never assigned, so this has always taken the primary's 768 KB
        // branch -- including on mirror surfaces, whose aux ring is far
        // smaller and for whom the 48 KB figure was chosen.
        const UINT kShapeUploadReserve =
            (pCtx && pCtx->isMirror) ? 48u * 1024u : 768u * 1024u;

        for (int instance = 0; instance < instances; instance++) {
          if (m_lpDX->GetUploadBytesRemaining() < kShapeUploadReserve) {
            instance = instances; // break outer shape loop cleanly
            break;
          }

          LoadCustomShapePerFrameEvallibVars(pState, i, instance, pCtx);

#ifndef _NO_EXPR_
          if (pState->m_shape[i].m_pf_codehandle) {
            NSEEL_code_execute(pState->m_shape[i].m_pf_codehandle);
          }
#endif

          int sides = (int)(*pState->m_shape[i].var_pf_sides);
          if (sides < 3) sides = 3;
          if (sides > 100) sides = 100;

          bool additive = ((int)(*pState->m_shape[i].var_pf_additive) != 0);
          bool textured = ((int)(*pState->m_shape[i].var_pf_textured) != 0);

          // Compute vertices (SPRITEVERTEX for texcoords, even if untextured)
          SPRITEVERTEX v[512];
          v[0].x = (float)(*pState->m_shape[i].var_pf_x * 2 - 1);
          // DX12 clip is passthrough. DX9 used y*-2+1 AND OrthoLH(2,-2);
          // that pair is the same visual as y*2-1 here. Do not re-apply the
          // Ortho compensation or every custom shape inverts.
          v[0].y = (float)(*pState->m_shape[i].var_pf_y * 2 - 1);
          v[0].z = 0;
          v[0].tu = 0.5f;
          v[0].tv = 0.5f;
          // Early-exit: skip shapes where fill, outer, AND border alpha are all zero.
          float shapeA  = (float)*pState->m_shape[i].var_pf_a  * alpha_mult;
          float shapeA2 = (float)*pState->m_shape[i].var_pf_a2 * alpha_mult;
          float borderA = (float)*pState->m_shape[i].var_pf_border_a;
          if (shapeA <= 0.0f && shapeA2 <= 0.0f && borderA <= 0.0f)
            continue;

          // Measured AFTER the alpha early-exit, so it describes geometry that
          // is actually submitted -- an invisible shape costs the GPU nothing
          // however absurd its radius.
          {
            const double gx   = *pState->m_shape[i].var_pf_x;
            const double gy   = *pState->m_shape[i].var_pf_y;
            const double grad = *pState->m_shape[i].var_pf_rad;
            const double gang = *pState->m_shape[i].var_pf_ang;
            if (!isfinite(gx) || !isfinite(gy) || !isfinite(grad) || !isfinite(gang))
              diag_nonFinite++;
            else if (fabs(grad) > diag_maxRad)
              diag_maxRad = fabs(grad);
          }

          v[0].Diffuse =
            ((((int)(*pState->m_shape[i].var_pf_a * 255 * alpha_mult)) & 0xFF) << 24) |
            ((((int)(*pState->m_shape[i].var_pf_r * 255)) & 0xFF) << 16) |
            ((((int)(*pState->m_shape[i].var_pf_g * 255)) & 0xFF) << 8) |
            ((((int)(*pState->m_shape[i].var_pf_b * 255)) & 0xFF));

          // Shape diagnostics: track draw count and visibility
          diag_shapesDrawn++;
          float shapeAlpha = (float)*pState->m_shape[i].var_pf_a * alpha_mult;
          if (shapeAlpha > 0.001f) {
            if (diag_shapesVisible == 0) {
              diag_firstVisibleAlpha = shapeAlpha;
              diag_firstVisibleColor = v[0].Diffuse;
            }
            diag_shapesVisible++;
          }

          v[1].Diffuse =
            ((((int)(*pState->m_shape[i].var_pf_a2 * 255 * alpha_mult)) & 0xFF) << 24) |
            ((((int)(*pState->m_shape[i].var_pf_r2 * 255)) & 0xFF) << 16) |
            ((((int)(*pState->m_shape[i].var_pf_g2 * 255)) & 0xFF) << 8) |
            ((((int)(*pState->m_shape[i].var_pf_b2 * 255)) & 0xFF));

          for (int j = 1; j < sides + 1; j++) {
            float t = (j - 1) / (float)sides;
            if (m_bScreenDependentRenderMode)
              v[j].x = v[0].x + (float)*pState->m_shape[i].var_pf_rad * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_ang + 3.1415927f * 0.25f);
            else
              v[j].x = v[0].x + (float)*pState->m_shape[i].var_pf_rad * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_ang + 3.1415927f * 0.25f) * aspectY;
            v[j].y = v[0].y - (float)*pState->m_shape[i].var_pf_rad * sinf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_ang + 3.1415927f * 0.25f);
            v[j].z = 0;
            if (m_bScreenDependentRenderMode)
              v[j].tu = 0.5f + 0.5f * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom);
            else
              v[j].tu = 0.5f + 0.5f * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom) * aspectY;
            v[j].tv = 0.5f + 0.5f * sinf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom);
            v[j].Diffuse = v[1].Diffuse;
          }
          v[sides + 1] = v[1];

          // Draw fill: fan of sides+2 verts → expand to trilist
          if (textured) {
            // Bind VS0 (or orient override) at t0 for textured shapes
            const DX12Texture& shapeVs =
                (pCtx && pCtx->shapeVs) ? *pCtx->shapeVs : m_dx12VS[0];
            cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetSrvGpuHandle(shapeVs));
            bool bClamp = (*pState->var_pf_wrap <= m_fSnapPoint);
            DX12PsoId fillPso = additive
              ? (bClamp ? PSO_ADDITIVE_CLAMP_SPRITEVERTEX : PSO_ADDITIVE_SPRITEVERTEX)
              : (bClamp ? PSO_TEXTURED_CLAMP_SPRITEVERTEX : PSO_ALPHABLEND_SPRITEVERTEX);
            cmdList->SetPipelineState(m_lpDX->m_PSOs[fillPso].Get());
            SPRITEVERTEX triVerts[300]; // max 100 sides → 100 tris → 300 verts
            int nTriVerts = ExpandFanToTriList(v, sides + 2, triVerts);
            m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, nTriVerts, sizeof(SPRITEVERTEX));
          } else {
            // Untextured: copy to WFVERTEX
            WFVERTEX v2[512];
            for (int j = 0; j < sides + 2; j++) {
              v2[j].x = v[j].x;
              v2[j].y = v[j].y;
              v2[j].z = v[j].z;
              v2[j].Diffuse = v[j].Diffuse;
            }
            DX12PsoId fillPso = additive ? PSO_ADDITIVE_WFVERTEX : PSO_ALPHABLEND_WFVERTEX;
            cmdList->SetPipelineState(m_lpDX->m_PSOs[fillPso].Get());
            WFVERTEX triVerts[300];
            int nTriVerts = ExpandFanToTriList(v2, sides + 2, triVerts);
            m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, nTriVerts, sizeof(WFVERTEX));
          }

          // Draw border
          if (*pState->m_shape[i].var_pf_border_a > 0) {
            WFVERTEX v2[512];
            v2[0].Diffuse =
              ((((int)(*pState->m_shape[i].var_pf_border_a * 255 * alpha_mult)) & 0xFF) << 24) |
              ((((int)(*pState->m_shape[i].var_pf_border_r * 255)) & 0xFF) << 16) |
              ((((int)(*pState->m_shape[i].var_pf_border_g * 255)) & 0xFF) << 8) |
              ((((int)(*pState->m_shape[i].var_pf_border_b * 255)) & 0xFF));
            for (int j = 0; j < sides + 2; j++) {
              v2[j].x = v[j].x;
              v2[j].y = v[j].y;
              v2[j].z = v[j].z;
              v2[j].Diffuse = v2[0].Diffuse;
            }

            // Border uses line alpha blend PSO
            if (!m_lpDX->m_PSOs[PSO_LINE_ALPHABLEND_WFVERTEX]) {
              if (instance == 0) DLOG_INFO("SHAPE BORDER: PSO_LINE_ALPHABLEND_WFVERTEX is NULL!");
            }
            cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_LINE_ALPHABLEND_WFVERTEX].Get());

            int its = ((int)(*pState->m_shape[i].var_pf_thick) != 0) ? 4 : 1;
            float x_inc = 2.0f / (float)texSizeX;
            float y_inc = 2.0f / (float)texSizeY;
            for (int it = 0; it < its; it++) {
              switch (it) {
              case 0: break;
              case 1: for (int j = 0; j < sides + 2; j++) v2[j].x += x_inc; break;
              case 2: for (int j = 0; j < sides + 2; j++) v2[j].y += y_inc; break;
              case 3: for (int j = 0; j < sides + 2; j++) v2[j].x -= x_inc; break;
              }
              // Border starts at v2[1] (skip center), sides+1 verts for closed loop
              m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, &v2[1], sides + 1, sizeof(WFVERTEX));
            }
          }
        }
      }
    }
  }

  // Diagnostic: log shape draw stats once per preset load
  if (!m_bPresetDiagLogged && GetTime() - m_fPresetStartTime >= 0.0f) {
    DLOG_VERBOSE("DX12 Shapes: drawn=%d visible=%d firstAlpha=%.3f firstColor=0x%08X",
            diag_shapesDrawn, diag_shapesVisible, diag_firstVisibleAlpha, diag_firstVisibleColor);
  }

  // Report only genuinely extreme geometry (#222). rad > 8 already covers the
  // screen sixteen times over, so nothing that renders sensibly trips this;
  // it fires on the frame that is about to hang the GPU, and because it is
  // written while the CPU builds the vertices it reaches the log BEFORE the
  // watchdog kills the device. That ordering is the whole point -- a stat
  // gathered after the frame would never be written at all.
  if (diag_nonFinite > 0 || diag_maxRad > 8.0) {
    DLOG_ERROR("SHAPE EXTREME (#222): drawn=%d maxRad=%.1f nonFinite=%d res=%dx%d preset=%ls",
               diag_shapesDrawn, diag_maxRad, diag_nonFinite, texSizeX, texSizeY,
               CurrentPresetLeaf());
  }
}

void mdrop::Engine::DX12_DrawCustomWaves(CState* pNewOverride, CState* pOldOverride,
                                         const RenderContext* pCtx) {
  if (!m_lpDX || !CmdListFor(pCtx))
    return;
  m_lpDX->BeginStage("customwaves");   // #222 stage map

  // Mirror sim contexts pass their own states; null = primary (unchanged).
  CState* pNewState = pNewOverride ? pNewOverride : m_pState;
  CState* pOldStateL = pNewOverride ? pOldOverride : m_pOldState;

  // Whose canvas. Same argument as the state above: these were the ENGINE's
  // aspect and texsize, which SizeGuard had to overwrite for the duration of a
  // mirror record precisely because they are not this surface's (#121).
  // haveCanvas for ALL of them, not just the sizes. A context that carries no
  // canvas has aspect 1.0 by default, so testing `pCtx ?` here would hand a
  // canvas-less context square aspect instead of the engine's -- the same
  // defect 43e807c7 fixed one line lower for texsize, where `rc ?` fed 0 in and
  // bound (0,0,inf,inf). Both callers happen to be unambiguous today; this
  // stops the next one from having to notice.
  const bool  haveCanvas = pCtx && pCtx->canvasW > 0 && pCtx->canvasH > 0;
  const float aspectX    = haveCanvas ? pCtx->fAspectX    : m_fAspectX;
  const float aspectY    = haveCanvas ? pCtx->fAspectY    : m_fAspectY;
  const float invAspectX = haveCanvas ? pCtx->fInvAspectX : m_fInvAspectX;
  const float invAspectY = haveCanvas ? pCtx->fInvAspectY : m_fInvAspectY;
  const int   texSizeX   = haveCanvas ? pCtx->canvasW : m_nTexSizeX;
  const int   texSizeY   = haveCanvas ? pCtx->canvasH : m_nTexSizeY;
  const bool bStateBlending = pNewState->m_bBlending && pOldStateL;

  auto* cmdList = CmdListFor(pCtx);

  int num_reps = bStateBlending ? 2 : 1;
  for (int rep = 0; rep < num_reps; rep++) {
    CState* pState = (rep == 0) ? pNewState : pOldStateL;
    float alpha_mult = 1;
    if (num_reps == 2)
      alpha_mult = (rep == 0) ? pNewState->m_fBlendProgress : (1 - pNewState->m_fBlendProgress);

    for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
      if (pState->m_wave[i].enabled) {
        int nSamples = pState->m_wave[i].samples;
        int max_samples = pState->m_wave[i].bSpectrum ? 512 : NUM_WAVEFORM_SAMPLES;
        if (nSamples > max_samples)
          nSamples = max_samples;
        nSamples -= pState->m_wave[i].sep;

        // 1. execute per-frame code
        LoadCustomWavePerFrameEvallibVars(pState, i, pCtx);

        *pState->m_wave[i].var_pp_time = *pState->m_wave[i].var_pf_time;
        *pState->m_wave[i].var_pp_fps = *pState->m_wave[i].var_pf_fps;
        *pState->m_wave[i].var_pp_frame = *pState->m_wave[i].var_pf_frame;
        *pState->m_wave[i].var_pp_progress = *pState->m_wave[i].var_pf_progress;
        *pState->m_wave[i].var_pp_bass = *pState->m_wave[i].var_pf_bass;
        *pState->m_wave[i].var_pp_mid = *pState->m_wave[i].var_pf_mid;
        *pState->m_wave[i].var_pp_treb = *pState->m_wave[i].var_pf_treb;
        *pState->m_wave[i].var_pp_bass_att = *pState->m_wave[i].var_pf_bass_att;
        *pState->m_wave[i].var_pp_mid_att = *pState->m_wave[i].var_pf_mid_att;
        *pState->m_wave[i].var_pp_treb_att = *pState->m_wave[i].var_pf_treb_att;

        if (pState->m_wave[i].m_pf_codehandle)
          NSEEL_code_execute(pState->m_wave[i].m_pf_codehandle);

        for (int vi = 0; vi < NUM_Q_VAR; vi++)
          *pState->m_wave[i].var_pp_q[vi] = *pState->m_wave[i].var_pf_q[vi];
        for (int vi = 0; vi < NUM_T_VAR; vi++)
          *pState->m_wave[i].var_pp_t[vi] = *pState->m_wave[i].var_pf_t[vi];

        nSamples = (int)*pState->m_wave[i].var_pf_samples;
        nSamples = min(512, nSamples);

        if ((nSamples >= 2) || (pState->m_wave[i].bUseDots && nSamples >= 1)) {
          float tempdata[2][512];
          float mult = ((pState->m_wave[i].bSpectrum) ? 0.15f : 0.004f) * pState->m_wave[i].scaling * pState->m_fWaveScale.eval(-1);
          float* pdata1 = (pState->m_wave[i].bSpectrum) ? m_sound.fSpectrum[0] : m_sound.fWaveform[0];
          float* pdata2 = (pState->m_wave[i].bSpectrum) ? m_sound.fSpectrum[1] : m_sound.fWaveform[1];

          int j0 = (pState->m_wave[i].bSpectrum) ? 0 : (max_samples - nSamples) / 2 - pState->m_wave[i].sep / 2;
          int j1 = (pState->m_wave[i].bSpectrum) ? 0 : (max_samples - nSamples) / 2 + pState->m_wave[i].sep / 2;
          float sample_stride = (pState->m_wave[i].bSpectrum) ? (max_samples - pState->m_wave[i].sep) / (float)nSamples : 1;
          float mix1 = powf(pState->m_wave[i].smoothing * 0.98f, 0.5f);
          float mix2 = 1 - mix1;

          tempdata[0][0] = pdata1[j0];
          tempdata[1][0] = pdata2[j1];
          for (int j = 1; j < nSamples; j++) {
            tempdata[0][j] = pdata1[(int)(j * sample_stride) + j0] * mix2 + tempdata[0][j - 1] * mix1;
            tempdata[1][j] = pdata2[(int)(j * sample_stride) + j1] * mix2 + tempdata[1][j - 1] * mix1;
          }
          for (int j = nSamples - 2; j >= 0; j--) {
            tempdata[0][j] = tempdata[0][j] * mix2 + tempdata[0][j + 1] * mix1;
            tempdata[1][j] = tempdata[1][j] * mix2 + tempdata[1][j + 1] * mix1;
          }
          for (int j = 0; j < nSamples; j++) {
            tempdata[0][j] *= mult;
            tempdata[1][j] *= mult;
          }

          // 2. per-point code execution
          WFVERTEX v[1024];
          float j_mult = 1.0f / (float)(nSamples - 1);
          for (int j = 0; j < nSamples; j++) {
            float t = j * j_mult;
            float value1 = tempdata[0][j];
            float value2 = tempdata[1][j];
            *pState->m_wave[i].var_pp_sample = t;
            *pState->m_wave[i].var_pp_value1 = value1;
            *pState->m_wave[i].var_pp_value2 = value2;
            *pState->m_wave[i].var_pp_x = 0.5f + value1;
            *pState->m_wave[i].var_pp_y = 0.5f + value2;
            *pState->m_wave[i].var_pp_r = *pState->m_wave[i].var_pf_r;
            *pState->m_wave[i].var_pp_g = *pState->m_wave[i].var_pf_g;
            *pState->m_wave[i].var_pp_b = *pState->m_wave[i].var_pf_b;
            *pState->m_wave[i].var_pp_a = *pState->m_wave[i].var_pf_a;

#ifndef _NO_EXPR_
            if (pState->m_wave[i].m_pp_codehandle)
              NSEEL_code_execute(pState->m_wave[i].m_pp_codehandle);
#endif

            if (m_bScreenDependentRenderMode) {
              // Aspect correction, matching MilkDrop3 (milkdropfs.cpp:2612) and the
              // legacy path below. This was missing here: the DX12 path was
              // hardcoded to the screen-dependent branch regardless of the flag,
              // so custom waves came out compressed by 1/invAspectY (about
              // 33% on a landscape frame) and could not reach the frame edges.
              //
              // The Y sign stays as-is: DX9 flipped via OrthoLH(2,-2) and
              // compensated with *-2+1, which DX12's passthrough VS must not
              // repeat. Only the aspect scale was absent.
              if (m_bScreenDependentRenderMode) {
                v[j].x = (float)(*pState->m_wave[i].var_pp_x * 2 - 1);
                v[j].y = (float)(*pState->m_wave[i].var_pp_y * 2 - 1);
              } else {
                v[j].x = (float)(*pState->m_wave[i].var_pp_x * 2 - 1) * invAspectX;
                v[j].y = (float)(*pState->m_wave[i].var_pp_y * 2 - 1) * invAspectY;
              }
            } else {
              v[j].x = (float)(*pState->m_wave[i].var_pp_x * 2 - 1) * invAspectX;
              v[j].y = (float)(*pState->m_wave[i].var_pp_y * 2 - 1) * invAspectY;
            }

            v[j].z = 0;
            v[j].Diffuse =
              ((((int)(*pState->m_wave[i].var_pp_a * 255 * alpha_mult)) & 0xFF) << 24) |
              ((((int)(*pState->m_wave[i].var_pp_r * 255)) & 0xFF) << 16) |
              ((((int)(*pState->m_wave[i].var_pp_g * 255)) & 0xFF) << 8) |
              ((((int)(*pState->m_wave[i].var_pp_b * 255)) & 0xFF));
          }

          // 4. draw it — select PSO based on additive + dots
          // NOTE: forcing lines for MD31 presets was tried here and REVERTED.
          //
          // MD3 renders `MD31=` presets down its 3.1 path, where the rainbow
          // arcs come out as smooth continuous curves rather than the dotted
          // streaks this renderer produces. Overriding bUseDots for those
          // presets reproduced the aggregate structure closely -- blobs 19 vs
          // MD3's 24, elongation 1.395 vs 1.229, lit 0.951 vs 0.850, against
          // 355 / 1.831 / 0.279 when honouring dots -- but it looks wrong: MD3
          // draws smooth arcs AND keeps the starfield as dots, whereas forcing
          // lines turns the starfield into lines everywhere, and the frame came
          // out about twice as bright as MD3 (lum 0.643 vs 0.326).
          //
          // So the blob/elongation measures were not sufficient to tell "arcs
          // plus dots" from "lines everywhere". Whatever the 3.1 path does, it
          // is not simply ignoring bUseDots. m_bMD3CachedShaderPreset is still parsed, for
          // whoever picks this up next.
          bool useDots = pState->m_wave[i].bUseDots;
          bool additive = pState->m_wave[i].bAdditive;
          DX12PsoId psoId;
          D3D12_PRIMITIVE_TOPOLOGY topology;
          if (useDots) {
            psoId = additive ? PSO_POINT_ADDITIVE_WFVERTEX : PSO_POINT_ALPHABLEND_WFVERTEX;
            topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
          } else {
            psoId = additive ? PSO_LINE_ADDITIVE_WFVERTEX : PSO_LINE_ALPHABLEND_WFVERTEX;
            topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
          }
          cmdList->SetPipelineState(m_lpDX->m_PSOs[psoId].Get());

          float x_inc = 2.0f / (float)texSizeX;
          float y_inc = 2.0f / (float)texSizeY;

          if (useDots) {
            // Emulate DX9 D3DRS_POINTSIZE for dots (DX12 points are always 1px).
            // DX9: ptsize = (texWidth >= 1024 ? 2 : 1) + (bDrawThick ? 1 : 0)
            int ptsize = (texSizeX >= 1024 ? 2 : 1) + (pState->m_wave[i].bDrawThick ? 1 : 0);
            int half = ptsize / 2;
            for (int dy = 0; dy < ptsize; dy++) {
              for (int dx = 0; dx < ptsize; dx++) {
                float xoff = (float)(dx - half) * x_inc;
                float yoff = (float)(dy - half) * y_inc;
                if (xoff != 0.0f || yoff != 0.0f) {
                  for (int j = 0; j < nSamples; j++) { v[j].x += xoff; v[j].y += yoff; }
                }
                m_lpDX->DrawVertices(topology, v, nSamples, sizeof(WFVERTEX));
                if (xoff != 0.0f || yoff != 0.0f) {
                  for (int j = 0; j < nSamples; j++) { v[j].x -= xoff; v[j].y -= yoff; }
                }
              }
            }
          } else {
            // Smooth, then draw. Matches MilkDrop3 DrawCustomWaves: smoothing
            // applies to the line (non-dots) branch, and a thick line is FOUR
            // passes at CUMULATIVE offsets forming a 2x2 square --
            // (0,0), (+x,0), (+x,+y), (0,+y).
            //
            // 961c18ba replaced that with a symmetric (2*halfPx+1)^2 box, so a
            // thick wave was drawn 9 times about its own centre rather than 4
            // times to one side -- thicker, brighter under additive blending,
            // and centred differently. The code being replaced was a faithful
            // port, identical to the reference down to its "draw fat dots"
            // comments.
            WFVERTEX tmp[2048];
            int ns = SmoothWave(v, nSamples, tmp);
            if (ns >= 2) {
              const int its = pState->m_wave[i].bDrawThick ? 4 : 1;
              for (int it = 0; it < its; it++) {
                switch (it) {
                case 0: break;
                case 1: for (int j = 0; j < ns; j++) tmp[j].x += x_inc; break;  // draw fat dots
                case 2: for (int j = 0; j < ns; j++) tmp[j].y += y_inc; break;  // draw fat dots
                case 3: for (int j = 0; j < ns; j++) tmp[j].x -= x_inc; break;  // draw fat dots
                }
                m_lpDX->DrawVertices(topology, tmp, ns, sizeof(WFVERTEX));
              }
            }
          }
        }
      }
    }
  }
}

void mdrop::Engine::ComputeGridAlphaValues() {
  // Start the per-vertex worker pool on first use. The count depends only on
  // core count, so after the first frame this is a comparison and nothing more.
  {
    const int want = GetPerVertexWorkerCount();
    if (m_pvPool.Workers() != want)
      m_pvPool.Start(want);
  }

  float fBlend = m_pState->m_fBlendProgress;//max(0,min(1,(m_pState->m_fBlendProgress*1.6f - 0.3f)));
  /*switch(code) //if (nPassOverride==0)
  {
  //case 8:
  //case 9:
  //case 12:
  //case 13:
      // note - these are the 4 cases where the old preset uses a warp shader, but new preset doesn't.
      fBlend = 1-fBlend;  // <-- THIS IS THE KEY - FLIPS THE ALPHAS AND EVERYTHING ELSE JUST WORKS.
      break;
  }*/
  //fBlend = 1-fBlend;  // <-- THIS IS THE KEY - FLIPS THE ALPHAS AND EVERYTHING ELSE JUST WORKS.
  bool bBlending = m_pState->m_bBlending;//(fBlend >= 0.0001f && fBlend <= 0.9999f);


  // warp stuff
  float fWarpTime = GetTime() * m_pState->m_fWarpAnimSpeed;
  float fWarpScaleInv = 1.0f / m_pState->m_fWarpScale.eval(GetTime());
  float f[4];
  f[0] = 11.68f + 4.0f * cosf(fWarpTime * 1.413f + 10);
  f[1] = 8.77f + 3.0f * cosf(fWarpTime * 1.113f + 7);
  f[2] = 10.54f + 3.0f * cosf(fWarpTime * 1.233f + 3);
  f[3] = 11.49f + 4.0f * cosf(fWarpTime * 0.933f + 5);

  // DX9 half-texel offset for UV alignment; not needed in DX12 (pixel centers at +0.5).
  float texel_offset_x = (m_lpDX && m_lpDX->m_device) ? 0.0f : 0.5f / (float)m_nTexSizeX;
  float texel_offset_y = (m_lpDX && m_lpDX->m_device) ? 0.0f : 0.5f / (float)m_nTexSizeY;

  int num_reps = (m_pState->m_bBlending) ? 2 : 1;
  int start_rep = 0;

  // FIRST WE HAVE 1-2 PASSES FOR CRUNCHING THE PER-VERTEX EQUATIONS
  for (int rep = start_rep; rep < num_reps; rep++) {
    // to blend the two PV equations together, we simulate both to get the final UV coords,
    // then we blend those final UV coords.  We also write out an alpha value so that
    // the second DRAW pass below (which might use a different shader) can do blending.
    CState* pState;

    if (rep == 0)
      pState = m_pState;
    else
      pState = m_pOldState;

    // cache the doubles as floats so that computations are a bit faster
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

    // Hand this frame's constants to the replica VMs before any of them run.
    pState->SyncPerVertexWorkerConsts();

    const int gridW = m_nGridX + 1;

    // One horizontal band of the vertex grid, rows [yBegin, yEnd). `bind`
    // selects which EEL context to evaluate on, so this body is identical
    // whether it runs on the render thread or on a worker.
    //
    // The ten warp parameters MUST be locals. With per-pixel code present every
    // vertex rewrites them, so hoisting them out (as this loop used to) would be
    // a data race the moment more than one band runs at a time.
    //
    // The body below is deliberately left at its original indentation: it is
    // ~100 unchanged lines, and re-indenting them would bury the actual change.
    auto evalRows = [&](int yBegin, int yEnd, const mdrop::PvBind& bind) {
    float fZoom = fZoomPF, fZoomExp = fZoomExpPF, fRot = fRotPF, fWarp = fWarpPF;
    float fCX = fCXPF, fCY = fCYPF, fDX = fDXPF, fDY = fDYPF;
    float fSX = fSXPF, fSY = fSYPF;

    int n = yBegin * gridW;

    for (int y = yBegin; y < yEnd; y++) {
      for (int x = 0; x <= m_nGridX; x++) {
        // Note: x, y, z are now set at init. time - no need to mess with them!
        //m_verts[n].x = i/(float)m_nGridX*2.0f - 1.0f;
        //m_verts[n].y = j/(float)m_nGridY*2.0f - 1.0f;
        //m_verts[n].z = 0.0f;

        if (bind.code) {
          // restore all the variables to their original states,
          //  run the user-defined equations,
          //  then move the results into local vars for computation as floats

          if (m_bScreenDependentRenderMode) {
            *bind.x = (double)(m_verts[n].x * 0.5f + 0.5f);
            *bind.y = (double)(m_verts[n].y * -0.5f + 0.5f);
          }
          else {
            *bind.x = (double)(m_verts[n].x * 0.5f * m_fAspectX + 0.5f);
            *bind.y = (double)(m_verts[n].y * -0.5f * m_fAspectY + 0.5f);
          }

          // NOTE: the seeds are copied as DOUBLES from the per-frame vars, not
          // from the float cache above -- narrowing here would change results.
          // Reading pState->var_pf_* from a worker is a shared read of values
          // that are fixed for the whole frame.
          *bind.rad = (double)m_vertinfo[n].rad;
          *bind.ang = (double)m_vertinfo[n].ang;
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
          //*pState->var_pv_time		= *pState->var_pv_time;		// (these are all now initialized
          //*pState->var_pv_bass		= *pState->var_pv_bass;		//  just once per frame)
          //*pState->var_pv_mid		= *pState->var_pv_mid;
          //*pState->var_pv_treb		= *pState->var_pv_treb;
          //*pState->var_pv_bass_att	= *pState->var_pv_bass_att;
          //*pState->var_pv_mid_att	= *pState->var_pv_mid_att;
          //*pState->var_pv_treb_att	= *pState->var_pv_treb_att;

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

        float fZoom2 = powf(fZoom, powf(fZoomExp, m_vertinfo[n].rad * 2.0f - 1.0f));

        // initial texcoords, w/built-in zoom factor
        float fZoom2Inv = 1.0f / fZoom2;

        // DIAG: log zoom/UV values for first 3 vertices on first frame of preset
        if (n < 3 && !m_bPresetDiagLogged) {
          DLOG_VERBOSE("DIAG WarpUV[%d]: fZoom=%.6f fZoomExp=%.4f fZoom2=%.6f fZoom2Inv=%.6f rad=%.4f",
                  n, fZoom, fZoomExp, fZoom2, fZoom2Inv, m_vertinfo[n].rad);
        }

        float u, v;
        if (m_bScreenDependentRenderMode) {
          u = m_verts[n].x * 0.5f * fZoom2Inv + 0.5f;
          v = -m_verts[n].y * 0.5f * fZoom2Inv + 0.5f;
        }
        else {
          u = m_verts[n].x * m_fAspectX * 0.5f * fZoom2Inv + 0.5f;
          v = -m_verts[n].y * m_fAspectY * 0.5f * fZoom2Inv + 0.5f;
        }
        //float u_orig = u;
        //float v_orig = v;
        //m_verts[n].tr = u_orig + texel_offset_x;
        //m_verts[n].ts = v_orig + texel_offset_y;

// stretch on X, Y:
        u = (u - fCX) / fSX + fCX;
        v = (v - fCY) / fSY + fCY;

        // warping:
        //if (fWarp > 0.001f || fWarp < -0.001f)
        //{
        u += fWarp * 0.0035f * sinf(fWarpTime * 0.333f + fWarpScaleInv * (m_verts[n].x * f[0] - m_verts[n].y * f[3]));
        v += fWarp * 0.0035f * cosf(fWarpTime * 0.375f - fWarpScaleInv * (m_verts[n].x * f[2] + m_verts[n].y * f[1]));
        u += fWarp * 0.0035f * cosf(fWarpTime * 0.753f - fWarpScaleInv * (m_verts[n].x * f[1] - m_verts[n].y * f[2]));
        v += fWarp * 0.0035f * sinf(fWarpTime * 0.825f + fWarpScaleInv * (m_verts[n].x * f[0] + m_verts[n].y * f[3]));
        //}

        // rotation:
        float u2 = u - fCX;
        float v2 = v - fCY;

        float cos_rot = cosf(fRot);
        float sin_rot = sinf(fRot);
        u = u2 * cos_rot - v2 * sin_rot + fCX;
        v = u2 * sin_rot + v2 * cos_rot + fCY;

        // translation:
        u -= fDX;
        v -= fDY;

        // undo aspect ratio fix:
        if (!m_bScreenDependentRenderMode) {
          u = (u - 0.5f) * m_fInvAspectX + 0.5f;
          v = (v - 0.5f) * m_fInvAspectY + 0.5f;
        }

        // final half-texel-offset translation:
        u += texel_offset_x;
        v += texel_offset_y;

        if (rep == 0) {
          // UV's for m_pState
          m_verts[n].tu = u;
          m_verts[n].tv = v;
          m_verts[n].Diffuse = 0xFFFFFFFF;
        }
        else {
          // blend to UV's for m_pOldState
          float mix2 = m_vertinfo[n].a * fBlend + m_vertinfo[n].c;//fCosineBlend2;
          mix2 = max(0, min(1, mix2));
          //     if fBlend un-flipped, then mix2 is 0 at the beginning of a blend, 1 at the end...
          //                           and alphas are 0 at the beginning, 1 at the end.
          m_verts[n].tu = m_verts[n].tu * (mix2)+u * (1 - mix2);
          m_verts[n].tv = m_verts[n].tv * (mix2)+v * (1 - mix2);
          // this sets the alpha values for blending between two presets:
          m_verts[n].Diffuse = 0x00FFFFFF | (((DWORD)(mix2 * 255)) << 24);
        }

        n++;
      }
    }
    };  // evalRows

    // Split the grid by rows when this preset has replica VMs to run on.
    // Bands are contiguous, so each worker writes a disjoint run of m_verts[]
    // and the array needs no locking.
    const int rows = m_nGridY + 1;

    // With per-pixel code, the split is limited to the replica VMs that were
    // built, and the pool must match: binding a band to a VM that does not
    // exist would silently render those rows without per-pixel code. Both
    // counts come from PvChooseWorkerCount() (core count only), so they agree
    // unless worker construction partly failed -- hence the equality test.
    //
    // Without per-pixel code there is no EEL involved at all: the body is pure
    // float math over m_verts/m_vertinfo, so any number of bands is safe and
    // PerVertexBind() hands back an empty bind that skips the EEL block.
    const bool havePP  = (pState->m_pp_codehandle != NULL);
    const int  nWorkers = havePP ? pState->PerVertexWorkerTotal() : m_pvPool.Workers();
    const bool canSplit = havePP ? (m_pvPool.Workers() == nWorkers) : true;

    if (nWorkers > 1 && canSplit && mdrop::PvParallelWorthIt(gridW * rows)) {
      m_pvPool.Run([&](int t) {
        const int y0 = (int)(((long long)rows * t) / nWorkers);
        const int y1 = (int)(((long long)rows * (t + 1)) / nWorkers);
        if (y1 > y0)
          evalRows(y0, y1, pState->PerVertexBind(t));
      });
      if (m_pvPool.Failed()) {
        // A worker threw. The band it owned may be half-written, so redo the
        // whole grid serially rather than ship a torn frame.
        m_pvPool.ClearFailed();
        DLOG_WARN("per-vertex worker threw; recomputing grid serially");
        evalRows(0, rows, pState->PerVertexBind(0));
      }
    }
    else {
      evalRows(0, rows, pState->PerVertexBind(0));
    }

  }
}

// Workers for the per-vertex loop, counting the render thread itself.
// 1 means "serial" and no thread is ever created.
int mdrop::Engine::GetPerVertexWorkerCount() {
  return mdrop::PvChooseWorkerCount();
}

void mdrop::Engine::ShutdownPerVertexPool() {
  m_pvPool.Stop();
}




void mdrop::Engine::LoadCustomShapePerFrameEvallibVars(CState* pState, int i, int instance,
                                                       const RenderContext* rc) {
  *pState->m_shape[i].var_pf_time = (double)(GetTime() - m_fStartTime);
  // EffectiveFrame, not GetFrame: on a mirror record this is that context's
  // own counter, so custom shapes agree with the per-frame code (#111).
  *pState->m_shape[i].var_pf_frame = (double)FrameOf(rc);
  *pState->m_shape[i].var_pf_fps = (double)GetFps();
  *pState->m_shape[i].var_pf_progress = (GetTime() - m_fPresetStartTime) / (m_fNextPresetTime - m_fPresetStartTime);

  *pState->m_shape[i].var_pf_bass = (double)mysound.imm_rel[0];
  *pState->m_shape[i].var_pf_mid = (double)mysound.imm_rel[1];
  *pState->m_shape[i].var_pf_treb = (double)mysound.imm_rel[2];

  *pState->m_shape[i].var_pf_bass_att = (double)mysound.avg_rel[0];
  *pState->m_shape[i].var_pf_mid_att = (double)mysound.avg_rel[1];
  *pState->m_shape[i].var_pf_treb_att = (double)mysound.avg_rel[2];

  for (int vi = 0; vi < NUM_Q_VAR; vi++)
    *pState->m_shape[i].var_pf_q[vi] = *pState->var_pf_q[vi];
  for (int vi = 0; vi < NUM_T_VAR; vi++)
    *pState->m_shape[i].var_pf_t[vi] = pState->m_shape[i].t_values_after_init_code[vi];
  *pState->m_shape[i].var_pf_x = pState->m_shape[i].x;
  *pState->m_shape[i].var_pf_y = pState->m_shape[i].y;
  *pState->m_shape[i].var_pf_rad = pState->m_shape[i].rad;
  *pState->m_shape[i].var_pf_ang = pState->m_shape[i].ang;
  *pState->m_shape[i].var_pf_tex_zoom = pState->m_shape[i].tex_zoom;
  *pState->m_shape[i].var_pf_tex_ang = pState->m_shape[i].tex_ang;
  *pState->m_shape[i].var_pf_sides = pState->m_shape[i].sides;
  *pState->m_shape[i].var_pf_additive = pState->m_shape[i].additive;
  *pState->m_shape[i].var_pf_textured = pState->m_shape[i].textured;
  *pState->m_shape[i].var_pf_instances = pState->m_shape[i].instances;
  *pState->m_shape[i].var_pf_instance = instance;
  *pState->m_shape[i].var_pf_thick = pState->m_shape[i].thickOutline;
  *pState->m_shape[i].var_pf_r = pState->m_shape[i].r;
  *pState->m_shape[i].var_pf_g = pState->m_shape[i].g;
  *pState->m_shape[i].var_pf_b = pState->m_shape[i].b;
  *pState->m_shape[i].var_pf_a = pState->m_shape[i].a;
  *pState->m_shape[i].var_pf_r2 = pState->m_shape[i].r2;
  *pState->m_shape[i].var_pf_g2 = pState->m_shape[i].g2;
  *pState->m_shape[i].var_pf_b2 = pState->m_shape[i].b2;
  *pState->m_shape[i].var_pf_a2 = pState->m_shape[i].a2;
  *pState->m_shape[i].var_pf_border_r = pState->m_shape[i].border_r;
  *pState->m_shape[i].var_pf_border_g = pState->m_shape[i].border_g;
  *pState->m_shape[i].var_pf_border_b = pState->m_shape[i].border_b;
  *pState->m_shape[i].var_pf_border_a = pState->m_shape[i].border_a;
}

void mdrop::Engine::LoadCustomWavePerFrameEvallibVars(CState* pState, int i,
                                                      const RenderContext* rc) {
  *pState->m_wave[i].var_pf_time = (double)(GetTime() - m_fStartTime);
  // EffectiveFrame: see LoadCustomShapePerFrameEvallibVars (#111).
  *pState->m_wave[i].var_pf_frame = (double)FrameOf(rc);
  *pState->m_wave[i].var_pf_fps = (double)GetFps();
  *pState->m_wave[i].var_pf_progress = (GetTime() - m_fPresetStartTime) / (m_fNextPresetTime - m_fPresetStartTime);

  *pState->m_wave[i].var_pf_bass = (double)mysound.imm_rel[0];
  *pState->m_wave[i].var_pf_mid = (double)mysound.imm_rel[1];
  *pState->m_wave[i].var_pf_treb = (double)mysound.imm_rel[2];

  *pState->m_wave[i].var_pf_bass_att = (double)mysound.avg_rel[0];
  *pState->m_wave[i].var_pf_mid_att = (double)mysound.avg_rel[1];
  *pState->m_wave[i].var_pf_treb_att = (double)mysound.avg_rel[2];

  for (int vi = 0; vi < NUM_Q_VAR; vi++)
    *pState->m_wave[i].var_pf_q[vi] = *pState->var_pf_q[vi];
  for (int vi = 0; vi < NUM_T_VAR; vi++)
    *pState->m_wave[i].var_pf_t[vi] = pState->m_wave[i].t_values_after_init_code[vi];
  *pState->m_wave[i].var_pf_r = pState->m_wave[i].r;
  *pState->m_wave[i].var_pf_g = pState->m_wave[i].g;
  *pState->m_wave[i].var_pf_b = pState->m_wave[i].b;
  *pState->m_wave[i].var_pf_a = pState->m_wave[i].a;
  *pState->m_wave[i].var_pf_samples = pState->m_wave[i].samples;
}

// does a better-than-linear smooth on a wave.  Roughly doubles the # of points.
int SmoothWave(WFVERTEX* vi, int nVertsIn, WFVERTEX* vo) {
  const float c1 = -0.15f;
  const float c2 = 1.15f;
  const float c3 = 1.15f;
  const float c4 = -0.15f;
  const float inv_sum = 1.0f / (c1 + c2 + c3 + c4);

  int j = 0;

  int i_below = 0;
  int i_above;
  int i_above2 = 1;
  for (int i = 0; i < nVertsIn - 1; i++) {
    i_above = i_above2;
    i_above2 = min(nVertsIn - 1, i + 2);
    vo[j] = vi[i];
    vo[j + 1].x = (c1 * vi[i_below].x + c2 * vi[i].x + c3 * vi[i_above].x + c4 * vi[i_above2].x) * inv_sum;
    vo[j + 1].y = (c1 * vi[i_below].y + c2 * vi[i].y + c3 * vi[i_above].y + c4 * vi[i_above2].y) * inv_sum;
    vo[j + 1].z = 0;
    vo[j + 1].Diffuse = vi[i].Diffuse;//0xFFFF0080;
    i_below = i;
    j += 2;
  }
  vo[j++] = vi[nVertsIn - 1];

  return j;
}




/*
bool mdrop::Engine::SetMilkdropRenderTarget(LPDIRECTDRAWSURFACE7 lpSurf, int w, int h, char *szErrorMsg)
{
  HRESULT hr = m_lpD3DDev->SetRenderTarget(0, lpSurf, 0);
  if (hr != D3D_OK)
  {
    //if (szErrorMsg && szErrorMsg[0]) dumpmsg(szErrorMsg);
    //IdentifyD3DError(hr);
    return false;
  }

  //DDSURFACEDESC2 ddsd;
  //ddsd.dwSize = sizeof(ddsd);
  //lpSurf->GetSurfaceDesc(&ddsd);

  D3DVIEWPORT7 viewData;
  ZeroMemory(&viewData, sizeof(D3DVIEWPORT7));
  viewData.dwWidth  = w;	// not: in windowed mode, when lpSurf is the back buffer, chances are good that w,h are smaller than the full surface size (since real size is fullscreen, but we're only using a portion of it as big as the window).
  viewData.dwHeight = h;
  hr = m_lpD3DDev->SetViewport(&viewData);

  return true;
}
*/

// The comp mesh THIS thread stages into (#206).
//
// m_comp_verts was one engine-wide array that the blend path rewrote in full
// immediately before DrawCompMesh read it back, so two mirror workers -- or a
// worker and the render thread's own comp pass, which reaches the identical
// code -- interleaved their writes and each drew a mesh whose per-vertex alpha
// was a mix of two surfaces' wipe patterns at two different aspects. That is a
// torn .milk2 blending_pattern, not a subtle colour shift, and for a .milk2
// library it is every mirror frame forever: bMilk2FrozenBlend stays true.
//
// Same shape as DrawWarpMeshIndexed's tlVerts/tlIdx. m_comp_indices stays
// shared -- built once in AllocateMyDX9Stuff, only read afterwards.
static MYVERTEX* CompStageForThisThread()
{
  thread_local std::vector<MYVERTEX> tl;
  if (tl.size() != (size_t)(FCGSX * FCGSY))
    tl.assign((size_t)FCGSX * FCGSY, MYVERTEX{});
  return tl.data();
}

const MYVERTEX* mdrop::Engine::UpdateCompMeshBlendColors(const DWORD cShade[4],
                                              CState* pStateOverride,
                                              const MYVERTEX* warpVertsOverride)
{
  // Match DX9 ShowToUser_Shaders: interpolate corner hue-shader colors onto
  // the comp mesh and, while blending, pull per-vertex alpha from the warp mesh
  // (the snail / plasma / wipe pattern). That is what makes .milk2 photos
  // follow the saved blending_pattern instead of a uniform 50% mix.
  CState* pState = pStateOverride ? pStateOverride : m_pState;
  const MYVERTEX* warpVerts = warpVertsOverride ? warpVertsOverride : m_verts;
  const bool bBlending = pState && pState->m_bBlending && warpVerts;
  MYVERTEX* stage = CompStageForThisThread();
  for (int j = 0; j < FCGSY; j++) {
    for (int i = 0; i < FCGSX; i++) {
      // Geometry from the shared template, colour into this thread's copy.
      MYVERTEX* p = &stage[i + j * FCGSX];
      *p = m_comp_verts[i + j * FCGSX];
      float x = p->x * 0.5f + 0.5f;
      float y = p->y * 0.5f + 0.5f;
      auto unpack = [](DWORD c, int shift) -> float {
        return ((c >> shift) & 0xFF) / 255.0f;
      };
      float cr = unpack(cShade[3], 16) * (x) * (y) +
                 unpack(cShade[2], 16) * (1 - x) * (y) +
                 unpack(cShade[1], 16) * (x) * (1 - y) +
                 unpack(cShade[0], 16) * (1 - x) * (1 - y);
      float cg = unpack(cShade[3], 8) * (x) * (y) +
                 unpack(cShade[2], 8) * (1 - x) * (y) +
                 unpack(cShade[1], 8) * (x) * (1 - y) +
                 unpack(cShade[0], 8) * (1 - x) * (1 - y);
      float cb = unpack(cShade[3], 0) * (x) * (y) +
                 unpack(cShade[2], 0) * (1 - x) * (y) +
                 unpack(cShade[1], 0) * (x) * (1 - y) +
                 unpack(cShade[0], 0) * (1 - x) * (1 - y);
      double alpha = 1.0;
      if (bBlending) {
        float gx = x * (m_nGridX + 1);
        float gy = y * (m_nGridY + 1);
        gx = max(min(gx, (float)m_nGridX - 1), 0.0f);
        gy = max(min(gy, (float)m_nGridY - 1), 0.0f);
        int nx = (int)gx;
        int ny = (int)gy;
        double dx = gx - nx;
        double dy = gy - ny;
        double a00 = (warpVerts[(ny) * (m_nGridX + 1) + (nx)].Diffuse >> 24);
        double a01 = (warpVerts[(ny) * (m_nGridX + 1) + (nx + 1)].Diffuse >> 24);
        double a10 = (warpVerts[(ny + 1) * (m_nGridX + 1) + (nx)].Diffuse >> 24);
        double a11 = (warpVerts[(ny + 1) * (m_nGridX + 1) + (nx + 1)].Diffuse >> 24);
        alpha = (a00 * (1 - dx) * (1 - dy) +
                 a01 * (dx) * (1 - dy) +
                 a10 * (1 - dx) * (dy) +
                 a11 * (dx) * (dy)) / 255.0;
      }
      p->Diffuse = D3DCOLOR_RGBA_01(cr, cg, cb, (float)alpha);
    }
  }
  return stage;
}

// Submits the warp mesh as an indexed draw. The mesh reuses each vertex ~6x, so the old
// expanded triangle list copied 6.33 MB per draw at nMeshSize 192 (and twice that on a
// blending .milk2) where the mesh itself is only 1.12 MB.
//
// cDecay replaces each vertex's RGB while its alpha is preserved, exactly as the expanded
// path did — staged once over the vertex array rather than once per triangle corner.
// Tile culling drops triangles by rewriting the index list instead of skipping vertex
// copies, so the visible result is identical.
void mdrop::Engine::DrawWarpMeshIndexed(const MYVERTEX* srcVerts, DWORD cDecay,
                                        bool bCullTiles, bool bFlipCulling,
                                        ID3D12GraphicsCommandList* cmdList)
{
  if (!m_lpDX || !srcVerts || !m_indices_list) return;
  if (!m_warpIdx16All) return;
  m_lpDX->BeginStage("warp");   // #222 stage map

  const int vertCount = (m_nGridX + 1) * (m_nGridY + 1);
  const int totalIdx  = m_warpIdx16AllCount;
  if (vertCount <= 0 || totalIdx <= 0) return;

  // Staging is PER THREAD (#206). The tinted vertex copy and the culled index
  // list used to be two engine-wide arrays, so the primary and a mirror worker
  // staging at the same time interleaved their vertices into one buffer and
  // both drew the result -- corrupt geometry, not a torn scalar. m_warpIdx16All
  // stays shared because it is built once and thereafter only read.
  thread_local std::vector<MYVERTEX> tlVerts;
  thread_local std::vector<UINT16>   tlIdx;
  if ((int)tlVerts.size() < vertCount) tlVerts.resize(vertCount);
  if ((int)tlIdx.size() < totalIdx)    tlIdx.resize(totalIdx);

  const DWORD rgb = cDecay & 0x00FFFFFF;
  for (int i = 0; i < vertCount; i++) {
    tlVerts[i] = srcVerts[i];
    tlVerts[i].Diffuse = rgb | (srcVerts[i].Diffuse & 0xFF000000);
  }

  const UINT16* indices = m_warpIdx16All;
  UINT indexCount = (UINT)totalIdx;

  if (bCullTiles) {
    UINT out = 0;
    for (int i = 0; i + 2 < totalIdx; i += 3) {
      const int i0 = m_indices_list[i];
      const int i1 = m_indices_list[i + 1];
      const int i2 = m_indices_list[i + 2];
      const BYTE a0 = (BYTE)(srcVerts[i0].Diffuse >> 24);
      const BYTE a1 = (BYTE)(srcVerts[i1].Diffuse >> 24);
      const BYTE a2 = (BYTE)(srcVerts[i2].Diffuse >> 24);
      if (bFlipCulling) {
        // Pass 0 (old preset): skip if all verts fully blended to new (alpha==0xFF)
        if (a0 == 0xFF && a1 == 0xFF && a2 == 0xFF) continue;
      } else {
        // Pass 1 (new preset): skip if all verts fully old (alpha==0x00)
        if (a0 == 0x00 && a1 == 0x00 && a2 == 0x00) continue;
      }
      tlIdx[out++] = (UINT16)i0;
      tlIdx[out++] = (UINT16)i1;
      tlIdx[out++] = (UINT16)i2;
    }
    if (out == 0) return; // every tile culled — nothing to submit
    indices = tlIdx.data();
    indexCount = out;
  }

  m_lpDX->DrawIndexedVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                              tlVerts.data(), (UINT)vertCount, sizeof(MYVERTEX),
                              indices, indexCount, cmdList);
}

void mdrop::Engine::DrawCompMesh(const MYVERTEX* compVerts, bool bCullTiles,
                                 bool bFlipCulling,
                                 ID3D12GraphicsCommandList* cmdList)
{
  if (!m_lpDX || !compVerts)
    return;
  m_lpDX->BeginStage("comp");   // #222 stage map
  if (!cmdList)
    cmdList = m_lpDX->m_commandList.Get();
  if (!cmdList)
    return;

  MYVERTEX tempv[1024 * 3];
  const int primCount = (FCGSX - 2) * (FCGSY - 2) * 2;
  const int max_prims_per_batch = (int)(sizeof(tempv) / sizeof(tempv[0]) / 3) - 4;
  int src_idx = 0;
  while (src_idx < primCount * 3) {
    int prims_queued = 0;
    int i = 0;
    while (prims_queued < max_prims_per_batch && src_idx < primCount * 3) {
      for (int j = 0; j < 3; j++)
        tempv[i++] = compVerts[m_comp_indices[src_idx++]];
      if (bCullTiles) {
        DWORD d1 = (tempv[i - 3].Diffuse >> 24);
        DWORD d2 = (tempv[i - 2].Diffuse >> 24);
        DWORD d3 = (tempv[i - 1].Diffuse >> 24);
        bool bIsNeeded = bFlipCulling
            ? ((d1 & d2 & d3) < 255)
            : ((d1 | d2 | d3) > 0);
        if (!bIsNeeded)
          i -= 3;
        else
          prims_queued++;
      }
      else
        prims_queued++;
    }
    if (prims_queued > 0)
      m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, tempv, prims_queued * 3, sizeof(MYVERTEX), cmdList);
  }
}

void mdrop::Engine::DrawUserSprites(int targetLayer, ID3D12GraphicsCommandList* cmdList,
                                    const RenderContext* rc)
{
  if (!m_lpDX)
    return;
  m_lpDX->BeginStage("usersprites");   // #222 stage map
  if (!cmdList)
    cmdList = m_lpDX->m_commandList.Get();
  if (!cmdList)
    return;
  const bool bPrimaryList = (cmdList == m_lpDX->m_commandList.Get());

  // Evaluate once per frame on the earliest sprite pass (milk2 back is 10,
  // then merge 12, then classic 0). Layer 0 used to be the first pass; if we
  // only eval on <=0 the wings already drew with stale sx/a.
  static int s_spriteEvalFrame = -1;
  const bool mayEval = bPrimaryList &&
      (targetLayer <= 0 || targetLayer == 10 || targetLayer == 12);
  // GetFrame(), NOT FrameOf(rc), and deliberately so. This latch runs the
  // user's sprite EEL once per ENGINE frame; a mirror draws the sprites but
  // must not re-evaluate their code. s_spriteEvalFrame is one static shared by
  // every surface, so keying it on a per-surface counter would make it differ
  // on every call -- primary 101, mirror 57, primary 102 -- and evaluate every
  // pass instead of once. The canvas reads above DO follow the context: a
  // sprite is POSITIONED against the surface it is drawn on.
  const bool evalThisPass = mayEval && (s_spriteEvalFrame != GetFrame());
  if (evalThisPass)
    s_spriteEvalFrame = GetFrame();

  // Set up DX12 rendering state
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  for (int iSlot = 0; iSlot < NUM_TEX; iSlot++) {
    // A retiring sprite keeps its slot until its fence clears (#225), so the
    // pending flag -- not the surface -- is what says whether to draw it.
    // Without this it would go on drawing, and go on being killed, for the
    // frame or two between the two.
    if (m_texmgr.m_tex[iSlot].dx12Surface.IsValid() &&
        !m_texmgr.m_tex[iSlot].bPendingKill) {
      if (evalThisPass) {
        // set values of input variables:
        *(m_texmgr.m_tex[iSlot].var_time) = (double)(GetTime() - m_texmgr.m_tex[iSlot].fStartTime);
        // EffectiveFrame so a sprite script on a mirror advances on that
        // surface's own clock rather than the primary's (#111).
        *(m_texmgr.m_tex[iSlot].var_frame) = (double)(FrameOf(rc) - m_texmgr.m_tex[iSlot].nStartFrame);
        *(m_texmgr.m_tex[iSlot].var_fps) = (double)GetFps();
        *(m_texmgr.m_tex[iSlot].var_progress) = (double)m_pState->m_fBlendProgress;
        *(m_texmgr.m_tex[iSlot].var_bass) = m_pState->var_pf_bass ? *m_pState->var_pf_bass : (double)mysound.imm_rel[0];
        *(m_texmgr.m_tex[iSlot].var_mid) = m_pState->var_pf_mid ? *m_pState->var_pf_mid : (double)mysound.imm_rel[1];
        *(m_texmgr.m_tex[iSlot].var_treb) = m_pState->var_pf_treb ? *m_pState->var_pf_treb : (double)mysound.imm_rel[2];
        *(m_texmgr.m_tex[iSlot].var_bass_att) = m_pState->var_pf_bass_att ? *m_pState->var_pf_bass_att : (double)mysound.avg_rel[0];
        *(m_texmgr.m_tex[iSlot].var_mid_att) = m_pState->var_pf_mid_att ? *m_pState->var_pf_mid_att : (double)mysound.avg_rel[1];
        *(m_texmgr.m_tex[iSlot].var_treb_att) = m_pState->var_pf_treb_att ? *m_pState->var_pf_treb_att : (double)mysound.avg_rel[2];

        // evaluate expressions
#ifndef _NO_EXPR_
        if (m_texmgr.m_tex[iSlot].m_codehandle) {
          NSEEL_code_execute(m_texmgr.m_tex[iSlot].m_codehandle);
        }
#endif
      }

      // Filter by target layer:
      //  -1 = all, 0 = classic behind-text, 1 = front,
      //  10 = milk2 in-back (layer 0), 12 = milk2 merge (layers 2–4)
      {
        const bool isMilk2 =
            (m_texmgr.m_tex[iSlot].nUserData & 0xFFFFFF00) == MILK2_SPRITE_USERDATA;
        int spriteLayer = (int)(*m_texmgr.m_tex[iSlot].var_layer + 0.5);
        if (spriteLayer < 0) spriteLayer = 0;
        if (spriteLayer > 4) spriteLayer = isMilk2 ? 4 : 1;
        if (targetLayer >= 0) {
          bool keep = false;
          if (targetLayer == 0)
            keep = spriteLayer == 0;
          else if (targetLayer == 1)
            keep = spriteLayer == 1 && !isMilk2;
          else if (targetLayer == 10)
            // Stamp layer 0 into VS *after* comp (pond trail), not before.
            keep = isMilk2 && spriteLayer == 0;
          else if (targetLayer == 12)
            // milk2 layers 1–4 enter VS before comp so Split Mode
            // (abs(uv.x-0.5)) and invert/echo can mirror them.
            keep = isMilk2 && spriteLayer >= 1;
          if (!keep)
            continue;
        }
      }

      bool bKillSprite = (*m_texmgr.m_tex[iSlot].var_done != 0.0);
      bool bBurnIn = (*m_texmgr.m_tex[iSlot].var_burn != 0.0);

      // Bind sprite texture
      D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = m_lpDX->GetBindingBlockGpuHandle(m_texmgr.m_tex[iSlot].dx12Surface);
      cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);

      SPRITEVERTEX v3[4];
      ZeroMemory(v3, sizeof(SPRITEVERTEX) * 4);

      /*
      int dest_w, dest_h;
      {
          LPDIRECT3DSURFACE9 pRT;
          lpDevice->GetRenderTarget( 0, &pRT );

          D3DSURFACE_DESC desc;
          pRT->GetDesc(&desc);
          dest_w = desc.Width;
          dest_h = desc.Height;
          pRT->Release();
      }*/

      // The file's SpriteX/SpriteY are an OFFSET added to the per-frame x/y,
      // not a position. They were parsed and then dropped, so a sprite the
      // preset had displaced rendered dead centre.
      //
      // MD3 clamps 2*var_x - 1 to +/-1000 and then adds SpriteX to it, in this
      // same NDC domain and at weight 1.0: the offset is applied after the
      // clamp, not before it.
      //
      // The corpus agrees: the values are SIGNED (-0.2125 .. 0.7975) and
      // cluster hard on 0.000000. A [0,1] position could not be negative, and
      // 0 would mean the left edge, yet those sprites plainly render centred.
      const float baseX = m_texmgr.m_tex[iSlot].base_x;
      const float baseY = m_texmgr.m_tex[iSlot].base_y;
      float x = min(1000.0f, max(-1000.0f, (float)(*m_texmgr.m_tex[iSlot].var_x) * 2.0f - 1.0f + baseX));
      float y = min(1000.0f, max(-1000.0f, (float)(*m_texmgr.m_tex[iSlot].var_y) * 2.0f - 1.0f + baseY));
      float sx = (float)(*m_texmgr.m_tex[iSlot].var_sx);
      float sy = (float)(*m_texmgr.m_tex[iSlot].var_sy);
      // MD3 adds the file's SpriteSX/SY to the per-frame sx/sy, then the
      // rendered width is NOT simply that sum.
      //
      // A plain add and nothing else would be:
      //
      //     y  = y  + SpriteY;      x  = x  + SpriteX;
      //     sx = sx + SpriteSX;     sy = sy + SpriteSY;
      //
      // but the frame it produces does not follow. At sum = 0.16 the plain add
      // predicts 0.16 of frame width and MD3 renders about 0.26; at sum = 0.70
      // it predicts 0.70 and MD3 renders 0.773. Something between the vertex
      // math and the presented frame scales sprites by roughly 1.52 at small
      // sizes, easing off as they grow. That factor is NOT understood.
      //
      // The piecewise curve below was fitted to MD3's rendered widths and
      // reproduces them -- it predicted 0.774 against a measured 0.773 in an
      // independent check. So it stays: the goal is matching what MD3 renders,
      // and the plain addition provably does not. This stayed an open
      // question: the same unexplained ~1.52 also appears in a position
      // sweep, where MD3's x mapping measures 3.04*x - 1.004 where the
      // straightforward reading is 2*var_x - 1.
      //
      //     sum <= 0.319 : frac = 1.522 * sum
      //     sum >  0.319 : frac = 0.755 * sum + 0.245
      //
      // fitting all nine measured points to within 0.002.
      if ((m_texmgr.m_tex[iSlot].nUserData & 0xFFFFFF00) == MILK2_SPRITE_USERDATA) {
        // MD3 simply ADDS the header scale to the per-frame one, keeping the
        // sign:
        //
        //     sx = sx + SpriteSX;   sy = sy + SpriteSY;
        //
        // A fitted piecewise curve used to sit here (1.522*sum below a knee at
        // 0.3194, 0.755*sum + 0.245 above) because captured MD3 sprites measured
        // about 1.52x wider than the plain sum predicts. That factor was never
        // MD3's: its window displays roughly the top-left 65.5% of its render
        // surface at 1:1, a ~1.5x zoomed crop, so every size measured off a
        // capture was inflated by 1/0.655 = 1.527. The curve was fitting that
        // artifact, and it showed: 'Rainbow Butterfly1/2' embed burst2.png at
        // SpriteSX=-1.34 with per-frame sx ~1.0, so the sum is -0.34 and the
        // curve returned 0.502 -- half the frame, a diffuse wash instead of the
        // crisp rotating burst MD3 draws. 0.502/0.34 = 1.48, the artifact again.
        //
        // The sign is kept: a negative scale mirrors the quad, which is what
        // MilkDrop does and is invisible on a symmetric burst. An earlier
        // version clamped negatives to zero, which made the burst vanish.
        sx = m_texmgr.m_tex[iSlot].base_sx + sx;
        sy = m_texmgr.m_tex[iSlot].base_sy + sy;
      }
      sx = min(1000.0f, max(-1000.0f, sx));
      sy = min(1000.0f, max(-1000.0f, sy));
      float rot = (float)(*m_texmgr.m_tex[iSlot].var_rot);
      int flipx = (*m_texmgr.m_tex[iSlot].var_flipx == 0.0) ? 0 : 1;
      int flipy = (*m_texmgr.m_tex[iSlot].var_flipy == 0.0) ? 0 : 1;
      float repeatx = min(100.0f, max(0.01f, (float)(*m_texmgr.m_tex[iSlot].var_repeatx)));
      float repeaty = min(100.0f, max(0.01f, (float)(*m_texmgr.m_tex[iSlot].var_repeaty)));

      // MD3 PRO exposes eleven blend modes, 0..10. This clamped to 7, which
      // silently rendered Cut-off, Darken and Vivid as Invert. The parse-time
      // clamp in engine_presets.cpp had to be lifted with it -- either alone
      // leaves the others unreachable.
      int blendmode = min(10, max(0, ((int)(*m_texmgr.m_tex[iSlot].var_blendmode))));
      float r = min(1.0f, max(0.0f, ((float)(*m_texmgr.m_tex[iSlot].var_r))));
      float g = min(1.0f, max(0.0f, ((float)(*m_texmgr.m_tex[iSlot].var_g))));
      float b = min(1.0f, max(0.0f, ((float)(*m_texmgr.m_tex[iSlot].var_b))));
      float a = min(1.0f, max(0.0f, ((float)(*m_texmgr.m_tex[iSlot].var_a))));

      // set x,y coords
      v3[0 + flipx].x = -sx;
      v3[1 - flipx].x = sx;
      v3[2 + flipx].x = -sx;
      v3[3 - flipx].x = sx;
      v3[0 + flipy * 2].y = -sy;
      v3[1 + flipy * 2].y = -sy;
      v3[2 - flipy * 2].y = sy;
      v3[3 - flipy * 2].y = sy;

      // Keep image + screen aspect so a square burst stays a plus/star.
      // Stretching milk2 sprites to the window made burst2 a tall/wide mess.
      const bool stretchMilk2 = false;

      // first aspect ratio: adjust for non-1:1 images
      if (!stretchMilk2)
      {
        float aspect = m_texmgr.m_tex[iSlot].img_h / (float)m_texmgr.m_tex[iSlot].img_w;

        if (aspect < 1)
          for (int k = 0; k < 4; k++) v3[k].y *= aspect;		// wide image
        else
          for (int k = 0; k < 4; k++) v3[k].x /= aspect;		// tall image
      }

      // 2D rotation
      {
        float cos_rot = cosf(rot);
        float sin_rot = sinf(rot);
        for (int k = 0; k < 4; k++) {
          float x2 = v3[k].x * cos_rot - v3[k].y * sin_rot;
          float y2 = v3[k].x * sin_rot + v3[k].y * cos_rot;
          v3[k].x = x2;
          v3[k].y = y2;
        }
      }

      // translation
      for (int k = 0; k < 4; k++) {
        v3[k].x += x;
        v3[k].y += y;
      }

      // Aspect of the surface we are drawing to (window vs warp VS).
      const bool toVS = (targetLayer == 10 || targetLayer == 12);
      const float destW = toVS ? (float)TexSizeXOf(rc) : (float)GetWidth();
      const float destH = toVS ? (float)TexSizeYOf(rc) : (float)GetHeight();

      // second aspect ratio: normalize to width of destination
      if (!stretchMilk2)
      {
        float aspect = destW / destH;

        if (aspect > 1)
          for (int k = 0; k < 4; k++) v3[k].y *= aspect;
        else
          for (int k = 0; k < 4; k++) v3[k].x /= aspect;
      }

      // third aspect ratio: adjust for burn-in
      if (bKillSprite && bBurnIn)	// final render-to-VS1
      {
        float aspect = destW / (destH * 4.0f / 3.0f);
        if (!m_bScreenDependentRenderMode)
          if (aspect < 1.0f)
            for (int k = 0; k < 4; k++) v3[k].x *= aspect;
          else
            for (int k = 0; k < 4; k++) v3[k].y /= aspect;
      }

      // Backbuffer: Ortho is gone, flip clip Y (Feb sprite fix).
      if (!toVS) {
        for (int k = 0; k < 4; k++) v3[k].y *= -1.0f;
      } else {
        // Warp VS: sit on the horizontal midline. SpriteY plus the Ortho
        // compensation kept sliding the pair above or below it.
        float cy = 0.25f * (v3[0].y + v3[1].y + v3[2].y + v3[3].y);
        for (int k = 0; k < 4; k++) v3[k].y -= cy;
      }

      // set u,v coords
      {
        float dtu = 0.5f;// / (float)m_texmgr.m_tex[iSlot].tex_w;
        float dtv = 0.5f;// / (float)m_texmgr.m_tex[iSlot].tex_h;
        v3[0].tu = -dtu;
        v3[1].tu = dtu;///*m_texmgr.m_tex[iSlot].img_w / (float)m_texmgr.m_tex[iSlot].tex_w*/ - dtu;
        v3[2].tu = -dtu;
        v3[3].tu = dtu;///*m_texmgr.m_tex[iSlot].img_w / (float)m_texmgr.m_tex[iSlot].tex_w*/ - dtu;
        v3[0].tv = -dtv;
        v3[1].tv = -dtv;
        v3[2].tv = dtv;///*m_texmgr.m_tex[iSlot].img_h / (float)m_texmgr.m_tex[iSlot].tex_h*/ - dtv;
        v3[3].tv = dtv;///*m_texmgr.m_tex[iSlot].img_h / (float)m_texmgr.m_tex[iSlot].tex_h*/ - dtv;

        // repeat on x,y
        for (int k = 0; k < 4; k++) {
          v3[k].tu = (v3[k].tu - 0.0f) * repeatx + 0.5f;
          v3[k].tv = (v3[k].tv - 0.0f) * repeaty + 0.5f;
        }

        if (toVS) {
          for (int k = 0; k < 4; k++)
            v3[k].tv = 1.0f - v3[k].tv;
        }
      }

      // DX12: Select PSO based on blend mode and set vertex colors
      DX12PsoId spritePso;
      switch (blendmode) {
      case 1:
        // decal (no blend)
        spritePso = PSO_TEXTURED_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r * a, g * a, b * a, 1);
        break;
      case 2:
        // additive (One/One with pre-multiplied colors)
        spritePso = PSO_ONEONE_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r * a, g * a, b * a, 1);
        break;
      case 3:
        // srccolor: dest = src*src + dest*(1-src). Dark texels leave the viz.
        spritePso = PSO_SRCCOLOR_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(1, 1, 1, 1);
        break;
      case 5:
        // MD3 multiplicative: src * dest
        spritePso = PSO_MULTIPLY_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r, g, b, a);
        break;
      case 6:
        // MD3 "Subtractive" is dest * (1 - src), NOT dest - src. Measured
        // against MilkDrop 3 PRO over 31862 samples per source colour with the
        // destination spanning 0..1: d*(1-s) fits at MAE 0.0010, while d-s is
        // off by 0.08-0.17 for a coloured source. Zero/InvSrcColor is exactly
        // that product.
        spritePso = PSO_DARKEN_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r * a, g * a, b * a, 1);
        break;
      case 7:
        // MD3 "invert" is 1 - src, independent of the destination -- measured
        // at MAE 0.0000 across the full destination range. Needs the shader
        // variant; a blend state cannot invert the source.
        spritePso = PSO_INVERT_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r, g, b, a);
        break;
      case 9:
        // MD3 "darken" is dest * min(1, 0.5 + src), MAE 0.0009. The shader
        // supplies min(1, 0.5 + src) and Zero/SrcColor multiplies it in.
        spritePso = PSO_DARKEN9_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r, g, b, a);
        break;
      case 8:
        // MD3 "cut-off": measured to drive every non-keyed source to 0
        // regardless of destination, so it blacks the region out. Colour-keyed
        // texels stay transparent, which is what makes it useful.
        spritePso = PSO_ZERO_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r, g, b, a);
        break;
      case 10:
        // MD3 "vivid" is dest + 2*src, measured at MAE 0.0000. One additive
        // pass gives dest + src, so the quad is drawn twice; see the second
        // draw below. A mid-grey source saturating to white at every
        // destination is what identified the factor of two.
        spritePso = PSO_ONEONE_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r * a, g * a, b * a, 1);
        break;
      case 0:
      case 4:
      default:
        // alpha blend (SrcAlpha/InvSrcAlpha)
        spritePso = PSO_ALPHABLEND_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(r, g, b, a);
        break;
      }

      cmdList->SetPipelineState(m_lpDX->m_PSOs[spritePso].Get());

      // Convert tri-strip (4 verts) to tri-list (6 verts)
      SPRITEVERTEX triVerts[6] = {
        v3[0], v3[1], v3[2],
        v3[2], v3[1], v3[3],
      };
      m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, 6, sizeof(SPRITEVERTEX), cmdList);

      // MD3 "vivid" is dest + 2*src. One additive pass contributes dest + src,
      // so the same quad is drawn a second time to double the source term.
      if (blendmode == 10)
        m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, 6, sizeof(SPRITEVERTEX), cmdList);

      // Burn-in: also render to VS1 so the sprite persists in the feedback loop
      if (bKillSprite && bBurnIn && bPrimaryList && m_dx12VS[1].resource) {
        m_lpDX->TransitionResource(m_dx12VS[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE vs1Rtv = m_lpDX->GetRtvCpuHandle(m_dx12VS[1]);
        cmdList->OMSetRenderTargets(1, &vs1Rtv, FALSE, nullptr);

        SetViewportAndScissor(cmdList, TexSizeXOf(rc), TexSizeYOf(rc));

        SPRITEVERTEX burnVerts[6] = { triVerts[0], triVerts[1], triVerts[2],
                                      triVerts[3], triVerts[4], triVerts[5] };
        m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, burnVerts, 6, sizeof(SPRITEVERTEX));

        // Restore backbuffer as render target
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        bbRtv.ptr += (SIZE_T)m_lpDX->m_frameIndex * m_lpDX->m_rtvDescriptorSize;
        cmdList->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);

        SetViewportAndScissor(cmdList, m_lpDX->m_client_width, m_lpDX->m_client_height);
      }

      // PRIMARY ONLY, like the burn-in branch just above it (#206).
      //
      // bKillSprite reads var_done, which the sprite EEL sets -- and that EEL
      // runs only on the primary, by the evalThisPass latch at the top of this
      // function. So every surface drawing this sprite sees the same "done"
      // and each one used to call KillSprite. texmgr::KillTex releases a
      // shared texture, writes a null descriptor into the live shader-visible
      // heap and frees an NSEEL handle the primary is still executing; doing
      // that from a mirror worker is a use-after-free waiting for the timing
      // to line up. The engine lock has been making it nearly unreachable
      // rather than the gate doing it.
      if (bKillSprite && bPrimaryList) {
        KillSprite(iSlot);
      }
    }
  }
}

void mdrop::Engine::UvToMathSpace(float u, float v, float* rad, float* ang) {
  // (screen space = -1..1 on both axes; corresponds to UV space)
  // uv space = [0..1] on both axes
  // "math" space = what the preset authors are used to:
  //      upper left = [0,0]
  //      bottom right = [1,1]
  //      rad == 1 at corners of screen
  //      ang == 0 at three o'clock, and increases counter-clockwise (to 6.28).

  float px, py;
  if (m_bScreenDependentRenderMode) {
    px = (u * 2 - 1);  // probably 1.0
    py = (v * 2 - 1);  // probably <1
    *rad = sqrtf(px * px + py * py);
  }
  else {
    px = (u * 2 - 1) * m_fAspectX;  // probably 1.0
    py = (v * 2 - 1) * m_fAspectY;  // probably <1

    *rad = sqrtf(px * px + py * py) / sqrtf(m_fAspectX * m_fAspectX + m_fAspectY * m_fAspectY);
  }

  *ang = atan2f(py, px);
  if (*ang < 0)
    *ang += 6.2831853071796f;
}


// Record what a BuildBindingSlots call actually produced, so the primary's and
// the mirror's view of the same CShaderParams can be diffed over the pipe.
// See Engine::BindSnapshot in engine.h.
void mdrop::Engine::CaptureBindSnapshot(BindSnapshot& dst, const CShaderParams* params,
                                        const UINT slots[32]) {
  if (!params || !slots) {
    dst.valid = false;
    return;
  }
  for (int i = 0; i < 32; i++) {
    dst.slots[i] = slots[i];
    dst.bindingSrv[i] = params->m_texture_bindings[i].dx12SrvIndex;
    dst.texcode[i] = (int)params->m_texcode[i];
  }
  dst.valid = true;
}

void mdrop::Engine::BuildBindingSlots(CShaderParams* params, const DX12Texture& vsTex, UINT outSlots[32], const DX12Texture* feedbackTex, const DX12Texture* imageFeedbackTex, const DX12Texture* bufferBTex, const DX12Texture* bufferCTex, const DX12Texture* bufferDTex, bool bNoPrimaryFallback) {
  for (int i = 0; i < 32; i++) {
    outSlots[i] = UINT_MAX;
    switch (params->m_texcode[i]) {
    case TEX_VS:
      outSlots[i] = vsTex.srvIndex;
      break;
    case TEX_FEEDBACK:
      if (feedbackTex && feedbackTex->IsValid())
        outSlots[i] = feedbackTex->srvIndex;
      else if (!bNoPrimaryFallback && m_dx12Feedback[0].IsValid())
        outSlots[i] = m_dx12Feedback[0].srvIndex;  // default: read buffer
      else
        outSlots[i] = vsTex.srvIndex;  // fallback to VS / null (never primary when strict)
      break;
    case TEX_IMAGE_FEEDBACK:
      if (imageFeedbackTex && imageFeedbackTex->IsValid())
        outSlots[i] = imageFeedbackTex->srvIndex;
      else if (!bNoPrimaryFallback && m_dx12ImageFeedback[0].IsValid())
        outSlots[i] = m_dx12ImageFeedback[0].srvIndex;
      else
        outSlots[i] = vsTex.srvIndex;  // fallback
      break;
    case TEX_BUFFER_B:
      if (bufferBTex && bufferBTex->IsValid())
        outSlots[i] = bufferBTex->srvIndex;
      else if (!bNoPrimaryFallback && m_dx12FeedbackB[0].IsValid())
        outSlots[i] = m_dx12FeedbackB[0].srvIndex;
      break;
    case TEX_BUFFER_C:
      if (bufferCTex && bufferCTex->IsValid())
        outSlots[i] = bufferCTex->srvIndex;
      else if (!bNoPrimaryFallback && m_dx12FeedbackC[0].IsValid())
        outSlots[i] = m_dx12FeedbackC[0].srvIndex;
      break;
    case TEX_BUFFER_D:
      if (bufferDTex && bufferDTex->IsValid())
        outSlots[i] = bufferDTex->srvIndex;
      else if (!bNoPrimaryFallback && m_dx12FeedbackD[0].IsValid())
        outSlots[i] = m_dx12FeedbackD[0].srvIndex;
      break;
    case TEX_AUDIO:
      if (m_dx12AudioTex.IsValid())
        outSlots[i] = m_dx12AudioTex.srvIndex;
      break;
#if (NUM_BLUR_TEX >= 2)
    case TEX_BLUR1:
      // Primary blur pyramid is landscape — never bind on orient pipe
      if (!bNoPrimaryFallback && m_dx12Blur[1].srvIndex != UINT_MAX)
        outSlots[i] = m_dx12Blur[1].srvIndex;
      else
        outSlots[i] = vsTex.srvIndex;
      break;
#endif
#if (NUM_BLUR_TEX >= 4)
    case TEX_BLUR2:
      if (!bNoPrimaryFallback && m_dx12Blur[3].srvIndex != UINT_MAX)
        outSlots[i] = m_dx12Blur[3].srvIndex;
      else
        outSlots[i] = vsTex.srvIndex;
      break;
#endif
#if (NUM_BLUR_TEX >= 6)
    case TEX_BLUR3:
      if (!bNoPrimaryFallback && m_dx12Blur[5].srvIndex != UINT_MAX)
        outSlots[i] = m_dx12Blur[5].srvIndex;
      else
        outSlots[i] = vsTex.srvIndex;
      break;
#endif
    case TEX_DISK:
      if (params->m_texture_bindings[i].dx12SrvIndex != UINT_MAX)
        outSlots[i] = params->m_texture_bindings[i].dx12SrvIndex;
      else if (i == 0)
        outSlots[i] = vsTex.srvIndex;  // MD1 fallback: slot 0 = VS texture
      else if (m_lpDX->m_fallbackTexture.srvIndex != UINT_MAX)
        outSlots[i] = m_lpDX->m_fallbackTexture.srvIndex;  // missing tex: fallback (hue gradient/white/black)
      break;
    default:
      break;
    }
  }
  // Diagnostic dump for Shadertoy binding verification (Verbose only).
  // Once-per-preset latch is set after RenderFrameShadertoy returns (early-return path).
  if (DLOG_DIAG_ENABLED() && m_bShadertoyMode && !m_bPresetDiagLogged) {
    FILE* fp = DebugLogDiagOpen(L"diag_bindings.txt", L"a");
    if (fp) {
      fprintf(fp, "Binding: fb=%s(%u) bufB=%s(%u) bufC=%s(%u) bufD=%s(%u)\n",
              feedbackTex && feedbackTex->IsValid() ? "OK" : "no",
              feedbackTex ? feedbackTex->srvIndex : UINT_MAX,
              bufferBTex && bufferBTex->IsValid() ? "OK" : "no",
              bufferBTex ? bufferBTex->srvIndex : UINT_MAX,
              bufferCTex && bufferCTex->IsValid() ? "OK" : "no",
              bufferCTex ? bufferCTex->srvIndex : UINT_MAX,
              bufferDTex && bufferDTex->IsValid() ? "OK" : "no",
              bufferDTex ? bufferDTex->srvIndex : UINT_MAX);
      for (int i = 0; i < 32; i++) {
        if (params->m_texcode[i] != 0 || outSlots[i] != UINT_MAX) {
          fprintf(fp, "  slot[%d]: texcode=%d srv=%u binding_srv=%u\n", i, params->m_texcode[i], outSlots[i],
                  params->m_texture_bindings[i].dx12SrvIndex);
        }
      }
      fprintf(fp, "---\n");
      fclose(fp);
    }
  }
}

// The preset pipeline a surface draws with.
//
// Every field is the engine's, which is what the record path reached for
// directly until now, so this is behaviour-preserving by construction. The
// point is that the stages no longer NAME Engine::m_shaders and the m_dx12*PSO
// set: pointing a context at its own compiled preset becomes a change here
// rather than a rewrite of every draw. See RenderContext's note.
void mdrop::Engine::FillPresetPipeline(RenderContext& rc) {
  rc.shaders        = &m_shaders;
  rc.oldShaders     = &m_OldShaders;
  rc.psos.warp      = m_dx12WarpPSO.Get();
  rc.psos.oldWarp   = m_dx12OldWarpPSO.Get();
  rc.psos.warpBlend = m_dx12WarpBlendPSO.Get();
  rc.psos.comp      = m_dx12CompPSO.Get();
  rc.psos.oldComp   = m_dx12OldCompPSO.Get();
  rc.psos.compBlend = m_dx12CompBlendPSO.Get();
}

// A mirror surface: its own compiled preset, once it has one.
//
// The fallback is not a nicety. A context has no shaders until it has adopted a
// bundle, and a frame recorded before that must still draw something -- so it
// borrows the engine's set exactly as it did before #184. Once adoption has
// compiled, this is what makes the mirror stop drawing with the primary's
// pipeline, which is the whole point of the issue.
void mdrop::Engine::FillPresetPipeline(RenderContext& rc, const MirrorSimContext& c) {
  if (!c.HasOwnPipeline()) {
    FillPresetPipeline(rc);
    return;
  }
  rc.shaders        = const_cast<PShaderSet*>(&c.shaders);
  rc.oldShaders     = const_cast<PShaderSet*>(&c.oldShaders);
  rc.psos.warp      = c.psos.warp.Get();
  rc.psos.oldWarp   = c.psos.oldWarp.Get();
  rc.psos.warpBlend = c.psos.warpBlend.Get();
  rc.psos.comp      = c.psos.comp.Get();
  rc.psos.oldComp   = c.psos.oldComp.Get();
  rc.psos.compBlend = c.psos.compBlend.Get();
}

// The primary surface: the engine's own clock, rate, schedule and live audio.
// These are the expressions ApplyShaderParams used unconditionally before the
// context carried them, so a primary-path RenderContext filled here reproduces
// the previous behaviour exactly.
void mdrop::Engine::FillAnimInputs(RenderContext& rc) {
  rc.fTime     = GetTime();
  rc.fFps      = (float)GetFps();
  // Deliberately UNGUARDED, matching what this expression has always been on
  // the primary. A zero-length window makes it inf/nan and always has; adding
  // the mirror's 30-second fallback here would be a silent behaviour change on
  // every preset that reads `progress`, smuggled in under a refactor. Worth
  // fixing on its own, with its own before/after.
  rc.fProgress = (GetTime() - m_fPresetStartTime) / (m_fNextPresetTime - m_fPresetStartTime);
  rc.snd       = &mysound;
}

// A mirror surface. Every expression here MUST match LoadPerFrameEvallibVarsCtx
// (mirror_sim.cpp), because the shader uniform and the preset's own per-frame
// code have to agree about what time it is -- that is #111's rule, applied to
// the rest of its list. The fps floor and the 30-second duration fallback are
// copied from there deliberately, not approximated.
void mdrop::Engine::FillAnimInputs(RenderContext& rc, const MirrorSimContext& c) {
  // Every mirror context is built through this overload, so naming the surface
  // here covers all of them at once and cannot be forgotten at a new call site
  // -- which is exactly how the canvas mitigations were missed twice.
  rc.sim = &c;
  // Every mirror context is built through this overload and only mirror
  // contexts are, so this is exactly the old engine flag's lifetime: true for
  // the duration of a worker record, false on the primary, which uses the
  // other overload (#206).
  rc.bOrientRecord = true;
  rc.fTime = c.fTime;
  rc.fFps  = (c.fFps > 1.f ? c.fFps : 60.f);
  float dur = m_fNextPresetTime - m_fPresetStartTime;
  if (dur <= 0.001f) dur = 30.0f;
  rc.fProgress = (float)((c.fTime - c.fPresetStartTime) / dur);
  rc.snd = &c.audio.snd;
  rc.randFrame = c.randFrame;
}

// Canvas, aspect and frame for ONE surface, from its size.
//
// The formulas are lifted VERBATIM from SizeGuard, which this replaces. Keep
// them identical while both exist: any drift shows up as a mirror rendered at
// the wrong aspect, not as a compile error, and the aspect defect this area
// already carries was one frame in ~250 -- far too rare to notice by eye
// during a refactor.
void mdrop::Engine::FillCanvasInputs(RenderContext& rc, int w, int h, int frame) {
  rc.canvasW     = w;
  rc.canvasH     = h;
  rc.fAspectX    = (h > w) ? w / (float)h : 1.0f;
  rc.fAspectY    = (w > h) ? h / (float)w : 1.0f;
  rc.fInvAspectX = 1.0f / rc.fAspectX;
  rc.fInvAspectY = 1.0f / rc.fAspectY;
  rc.nFrame      = frame;
}

void mdrop::Engine::FillMouseInputs(RenderContext& rc, int w, int h) {
  // Scale by the same factor the canvas changed by, which is what the milk3
  // SizeGuard did by multiplying the engine's members in place. Same maths,
  // no mutation: the primary's mouse is left alone and this surface gets its
  // own. When the primary has no size yet there is nothing to scale from, so
  // the mouse passes through unchanged.
  rc.hasMouse = true;
  float sx = 1.0f, sy = 1.0f;
  if (m_nTexSizeX > 0 && m_nTexSizeY > 0) {
    sx = (float)w / (float)m_nTexSizeX;
    sy = (float)h / (float)m_nTexSizeY;
  }
  rc.stMouseX = m_stMouseX * sx;
  rc.stMouseY = m_stMouseY * sy;
  rc.stClickX = m_stClickX * sx;   // z/w carry click pixels
  rc.stClickY = m_stClickY * sy;
}

void mdrop::Engine::ApplyShaderParams(CShaderParams* p, LPD3DXCONSTANTTABLE pCT, CState* pState,
                                      const RenderContext* rc,
                                      std::vector<BYTE>* stage) {
  // Every constant below goes through one of these, so there is a single place
  // that decides where the bytes land (#206). With a stage the caller owns the
  // buffer and nothing is shared; without one this is exactly the call that
  // was here before, including its null-handle guard.
  DX12ConstantTable* ct12 = static_cast<DX12ConstantTable*>(pCT);
  auto put = [&](D3DXHANDLE h, const D3DXVECTOR4& v) {
    if (!h) return;
    if (stage && ct12) ct12->WriteVectorInto(*stage, h, v);
    else if (pCT)      pCT->SetVector(nullptr, h, &v);
  };
  auto putMat = [&](D3DXHANDLE h, const D3DXMATRIX& m) {
    if (!h) return;
    if (stage && ct12) ct12->WriteMatrixInto(*stage, h, m);
    else if (pCT)      pCT->SetMatrix(nullptr, h, &m);
  };

  // Note which blur levels this preset samples, so the blur passes know how
  // many to actually run -- see the note at DX12_RenderWarpAndComposite, which
  // runs them before the comp shader's ApplyShaderParams.
  //
  // The DX9 texture and sampler binding that stood here is gone (issue 17). Its
  // DX12 replacements are elsewhere and predate this removal: BuildBindingSlots
  // resolves the same m_texcode switch into SRV descriptor indices, and the
  // sampler half is four static samplers declared in the root signature.
  // Only the PRIMARY accumulates into the engine member. A mirror runs this
  // same function, and with no guard its preset's blur3 permanently inflated
  // the primary's pyramid depth (#208). A mirror's own depth comes from the
  // context-aware scan above, into bc.highestBlurUsed.
  if (!(rc && rc->bOrientRecord)) {
    for (int i = 0; i < sizeof(p->m_texture_bindings) / sizeof(p->m_texture_bindings[0]); i++) {
      if (p->m_texcode[i] >= TEX_BLUR1 && p->m_texcode[i] <= TEX_BLUR_LAST)
        m_nHighestBlurTexUsedThisFrame = max(m_nHighestBlurTexUsedThisFrame, ((int)p->m_texcode[i] - (int)TEX_BLUR1) + 1);
    }
  }

  // bind "texsize_XYZ" params
  int N = (int)p->texsize_params.size();
  for (int i = 0; i < N; i++) {
    TexSizeParamInfo* q = &(p->texsize_params[i]);
    put(q->texsize_param, D3DXVECTOR4((float)q->w, (float)q->h, 1.0f / q->w, 1.0f / q->h));
  }

  // Both offsets come off the SAME clock as the state whose start time is
  // being subtracted -- see RenderContext::fTime.
  const double surfaceTime = rc ? rc->fTime : GetTime();
  float time_since_preset_start = (float)surfaceTime - pState->GetPresetStartTime();
  float time_since_preset_start_wrapped = time_since_preset_start - (int)(time_since_preset_start / 10000) * 10000;
  // #121: this surface's clock, rate, schedule and audio -- the engine's when
  // no context is given, which is every primary-path caller and keeps their
  // behaviour byte-identical. A mirror passes its context and stops animating
  // at the primary's tempo. See RenderContext's animation-inputs note.
  double time = surfaceTime - m_fStartTime;
  float progress = rc ? rc->fProgress
                      : (GetTime() - m_fPresetStartTime) / (m_fNextPresetTime - m_fPresetStartTime);
  const float fpsVal = rc ? rc->fFps : (float)GetFps();
  const td_mysounddata& snd = (rc && rc->snd) ? *rc->snd : mysound;
  // This surface's canvas (#121). outW/outH consult
  // m_nOutputSizeOverrideW/H, which is the OTHER half of what SizeGuard sets --
  // so this function was reading a mirror's size through the engine's back door
  // rather than from the context it was already being handed.
  // Guard on the VALUE, not merely on rc being non-null. canvasW/H default to
  // 0 and only a surface that called FillCanvasInputs has them set -- the
  // primary's warp and comp contexts carry time, fps and audio but no canvas.
  // Testing `rc ?` alone therefore fed 0 in on the PRIMARY path, making
  // logf(outW) = -inf and binding texsize as (0,0,inf,inf) to every shader.
  const bool haveCanvas = rc && rc->canvasW > 0 && rc->canvasH > 0;
  const int outW     = haveCanvas ? rc->canvasW : GetWidth();
  const int outH     = haveCanvas ? rc->canvasH : GetHeight();
  const int texSizeX = haveCanvas ? rc->canvasW : m_nTexSizeX;
  const int texSizeY = haveCanvas ? rc->canvasH : m_nTexSizeY;
  float mip_x = logf((float)outW) / logf(2.0f);
  float mip_y = logf((float)outW) / logf(2.0f);
  float mip_avg = 0.5f * (mip_x + mip_y);
  float aspect_x = 1;
  float aspect_y = 1;

  if (!m_bScreenDependentRenderMode)
    if (outW > outH)
      aspect_y = outH / (float)outW;
    else
      aspect_x = outW / (float)outH;

  // What a MIRROR's shaders are actually handed. Counter, not a capture: a
  // screen grab of these displays photographs whatever is in front (twice it
  // caught a game), so the numbers have to come from the engine.
  //
  // haveCanvas=0 here means the context carried no canvas and everything below
  // silently fell back to the PRIMARY's size and aspect -- which is what
  // SizeGuard used to paper over before #186 phase 2 removed it.
  if (rc && rc->isMirror) {
    // thread_local, not static (#206). Only mirror workers reach this, and
    // with more than one surface an unsynchronised std::set::insert from N
    // threads is not merely a lost log line -- it is concurrent mutation of a
    // red-black tree. Per-thread is also the more useful diagnostic: one line
    // per SURFACE per distinct shape, rather than whichever surface happened
    // to get there first speaking for all of them.
    thread_local std::set<std::string> s_seen;
    char key[192];
    sprintf(key, "have=%d ctx=%dx%d out=%dx%d tex=%dx%d asp=%.4f/%.4f eng=%dx%d/%.4f/%.4f",
            haveCanvas ? 1 : 0, rc->canvasW, rc->canvasH, outW, outH,
            texSizeX, texSizeY, aspect_x, aspect_y,
            m_nTexSizeX, m_nTexSizeY, m_fAspectX, m_fAspectY);
    if (s_seen.insert(key).second)
      DebugLogA((std::string("shaderparams mirror ") + key + "\n").c_str(),
                LOG_INFO);
  }

  float blur_min[3], blur_max[3];
  GetSafeBlurMinMax(pState, blur_min, blur_max);

  // bind float4's (C-Variables!)
  // Per context where the surface has its own, the engine's otherwise. The
  // behaviour question this raises -- a mirror's noise no longer matching the
  // primary's -- was put to the owner and answered: "the rand_frame can be
  // different if the rendering is different." An independently-rendering
  // surface already differs in clock, phase, aspect and canvas, so its noise
  // differing too is consistent rather than a new kind of divergence. A
  // stretch/copy mirror is not rendering differently and never gets here.
  if (p->rand_frame) {
    if (rc && rc->randFrame) {
      D3DXVECTOR4 rf(rc->randFrame[0], rc->randFrame[1], rc->randFrame[2], rc->randFrame[3]);
      put(p->rand_frame, rf);
    } else {
      put(p->rand_frame, m_rand_frame);
    }
  }
  put(p->rand_preset, pState->m_rand_preset);
  D3DXHANDLE* h = p->const_handles;
  put(h[0], D3DXVECTOR4(aspect_x, aspect_y, 1.0f / aspect_x, 1.0f / aspect_y));
  put(h[1], D3DXVECTOR4(0, 0, 0, 0));
  {
    // In Shadertoy mode, iFrame must start at 0 when the preset loads (not global frame count).
    // Many Shadertoy shaders use "if (iFrame < 2) { reset; }" to initialize the feedback buffer.
    // FrameOf(rc): the HLSL `frame` uniform must match what the preset's own
    // per-frame code was given on this surface (#111). Both now read the frame
    // off the context, so the pair cannot drift apart -- which was the whole
    // point of the override this replaces.
    float frameVal = m_bShadertoyMode ? (float)(FrameOf(rc) - m_nShadertoyStartFrame)
                                      : (float)FrameOf(rc);
    // Latched for DIAG_MIRRORS: what the SHADER uniform was actually given.
    //
    // Assigned FROM frameVal, never re-derived. A re-derivation measures the
    // expression written in the diagnostic rather than the one the shader got,
    // so the two can disagree and the diagnostic reports the wrong one -- which
    // is not hypothetical: latching a second EffectiveFrame() call here made a
    // deliberately reverted frameVal pass the test that exists to catch it.
    //
    // One NAMED consumer, too. A latch shared by every consumer reports
    // whichever ran last, so three sites still doing the right thing hide a
    // fourth doing the wrong one -- also measured.
    // Onto the SURFACE when the context names one, so the value belongs to
    // whoever recorded it. m_nDiagShaderFrame stays for the primary, which has
    // no surface context (#198).
    if (rc && rc->sim)
      rc->sim->diagShaderFrame = (int)frameVal;
    else
      m_nDiagShaderFrame = (int)frameVal;
    put(h[2], D3DXVECTOR4(time_since_preset_start_wrapped, fpsVal, frameVal, progress));
  }
  put(h[3], D3DXVECTOR4(snd.imm_rel[0], snd.imm_rel[1], snd.imm_rel[2], 0.3333f * (snd.imm_rel[0], snd.imm_rel[1], snd.imm_rel[2])));
  put(h[4], D3DXVECTOR4(snd.avg_rel[0], snd.avg_rel[1], snd.avg_rel[2], 0.3333f * (snd.avg_rel[0], snd.avg_rel[1], snd.avg_rel[2])));
  put(h[5], D3DXVECTOR4(blur_max[0] - blur_min[0], blur_min[0], blur_max[1] - blur_min[1], blur_min[1]));
  put(h[6], D3DXVECTOR4(blur_max[2] - blur_min[2], blur_min[2], blur_min[0], blur_max[0]));
  put(h[7], D3DXVECTOR4((float)texSizeX, (float)texSizeY, 1.0f / (float)texSizeX, 1.0f / (float)texSizeY));
  put(h[8], D3DXVECTOR4(0.5f + 0.5f * cosf((float)time * 0.329f + 1.2f),
    0.5f + 0.5f * cosf((float)time * 1.293f + 3.9f),
    0.5f + 0.5f * cosf((float)time * 5.070f + 2.5f),
    0.5f + 0.5f * cosf((float)time * 20.051f + 5.4f)
  ));
  put(h[9], D3DXVECTOR4(0.5f + 0.5f * sinf((float)time * 0.329f + 1.2f),
    0.5f + 0.5f * sinf((float)time * 1.293f + 3.9f),
    0.5f + 0.5f * sinf((float)time * 5.070f + 2.5f),
    0.5f + 0.5f * sinf((float)time * 20.051f + 5.4f)
  ));
  put(h[10], D3DXVECTOR4(0.5f + 0.5f * cosf((float)time * 0.0050f + 2.7f),
    0.5f + 0.5f * cosf((float)time * 0.0085f + 5.3f),
    0.5f + 0.5f * cosf((float)time * 0.0133f + 4.5f),
    0.5f + 0.5f * cosf((float)time * 0.0217f + 3.8f)
  ));
  put(h[11], D3DXVECTOR4(0.5f + 0.5f * sinf((float)time * 0.0050f + 2.7f),
    0.5f + 0.5f * sinf((float)time * 0.0085f + 5.3f),
    0.5f + 0.5f * sinf((float)time * 0.0133f + 4.5f),
    0.5f + 0.5f * sinf((float)time * 0.0217f + 3.8f)
  ));
  put(h[12], D3DXVECTOR4(mip_x, mip_y, mip_avg, 0));
  put(h[13], D3DXVECTOR4(blur_min[1], blur_max[1], blur_min[2], blur_max[2]));
  
  // BMV/MDropDX12
  if (h[14]) {
    if (m_bShadertoyMode) {
      // Shadertoy iMouse: pixel coords, z/w encode click position with sign for button state
      //
      // THIS surface's mouse. A mirror renders a different canvas, and a
      // shader reads the mouse as mouse/texsize -- so the pixels have to be
      // rescaled with the canvas or the ratio changes under the preset. That
      // rescale used to be done by multiplying the engine's members inside
      // SizeGuard; it now arrives on the context.
      const float mx = (rc && rc->hasMouse) ? rc->stMouseX : m_stMouseX;
      const float my = (rc && rc->hasMouse) ? rc->stMouseY : m_stMouseY;
      const float cx = (rc && rc->hasMouse) ? rc->stClickX : m_stClickX;
      const float cy = (rc && rc->hasMouse) ? rc->stClickY : m_stClickY;
      bool neverClicked = (cx == 0.f && cy == 0.f);
      float z = neverClicked ? 0.f : (m_stMouseDown ?  cx : -cx);
      float w = neverClicked ? 0.f : (m_stMouseJustClicked ? cy : -cy);
      put(h[14], D3DXVECTOR4(mx, my, z, w));
    } else {
      put(h[14], D3DXVECTOR4(m_mouseX,
        m_mouseY != -1 ? -m_mouseY + 1 : -1,
        m_mouseDown ? 1.0f : 0.0f,
        m_mouseClicked > 0 ? m_lastMouseY : -m_lastMouseY));
    }
  }
  put(h[15], D3DXVECTOR4(snd.smooth[0], snd.smooth[1], snd.smooth[2], 0.3333f * (snd.smooth[0], snd.smooth[1], snd.smooth[2])));
  put(h[16], D3DXVECTOR4(m_VisIntensity, m_VisShift, m_VisVersion, 0));
  put(h[17], D3DXVECTOR4(m_ColShiftHue, m_ColShiftSaturation, m_ColShiftBrightness, 0));
  if (h[18]) {
    float ve_alpha = (float)(*pState->var_pf_echo_alpha);
    float ve_zoom  = (float)(*pState->var_pf_echo_zoom);
    int   ve_orient = (int)(*pState->var_pf_echo_orient) % 4;
    float inv_zoom = (ve_zoom > 0.001f) ? (1.0f / ve_zoom) : 1.0f;
    put(h[18], D3DXVECTOR4(
        (float)(*pState->var_pf_gamma),
        ve_alpha,
        inv_zoom,
        (float)ve_orient
    ));
  }
  if (h[19]) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    float secsSinceMidnight = (float)st.wHour * 3600.0f + (float)st.wMinute * 60.0f + (float)st.wSecond + (float)st.wMilliseconds * 0.001f;
    put(h[19], D3DXVECTOR4((float)st.wYear, (float)(st.wMonth - 1), (float)st.wDay, secsSinceMidnight));
  }

  // get_fft's shape, from the active audio profile
  if (p->fft_params)
    put(p->fft_params, D3DXVECTOR4(
      m_audioProfile.fftSqrt ? 0.5f : 1.0f, m_audioProfile.fftHzRef,
      // .z = this frame's largest FFT bin, for md3_fft(). .w still free.
      m_fFFTFramePeak, 0.0f));

  // write q vars
  int num_q_float4s = sizeof(p->q_const_handles) / sizeof(p->q_const_handles[0]);
  for (int i = 0; i < num_q_float4s; i++) {
    if (p->q_const_handles[i])
      put(p->q_const_handles[i], D3DXVECTOR4(
        (float)*pState->var_pf_q[i * 4 + 0],
        (float)*pState->var_pf_q[i * 4 + 1],
        (float)*pState->var_pf_q[i * 4 + 2],
        (float)*pState->var_pf_q[i * 4 + 3]));
  }

  // write matrices
  for (int i = 0; i < 20; i++) {
    if (p->rot_mat[i]) {
      D3DXMATRIX mx, my, mz, mxlate, temp;

      D3DXMatrixRotationX(&mx, pState->m_rot_base[i].x + pState->m_rot_speed[i].x * (float)time);
      D3DXMatrixRotationY(&my, pState->m_rot_base[i].y + pState->m_rot_speed[i].y * (float)time);
      D3DXMatrixRotationZ(&mz, pState->m_rot_base[i].z + pState->m_rot_speed[i].z * (float)time);
      D3DXMatrixTranslation(&mxlate, pState->m_xlate[i].x, pState->m_xlate[i].y, pState->m_xlate[i].z);

      D3DXMatrixMultiply(&temp, &mx, &mxlate);
      D3DXMatrixMultiply(&temp, &temp, &mz);
      D3DXMatrixMultiply(&temp, &temp, &my);

      putMat(p->rot_mat[i], temp);
    }
  }
  // the last 4 are totally random, each frame
  for (int i = 20; i < 24; i++) {
    if (p->rot_mat[i]) {
      D3DXMATRIX mx, my, mz, mxlate, temp;

      D3DXMatrixRotationX(&mx, FRAND * 6.28f);
      D3DXMatrixRotationY(&my, FRAND * 6.28f);
      D3DXMatrixRotationZ(&mz, FRAND * 6.28f);
      D3DXMatrixTranslation(&mxlate, FRAND, FRAND, FRAND);

      D3DXMatrixMultiply(&temp, &mx, &mxlate);
      D3DXMatrixMultiply(&temp, &temp, &mz);
      D3DXMatrixMultiply(&temp, &temp, &my);

      putMat(p->rot_mat[i], temp);
    }
  }
}



void mdrop::Engine::ShowSongTitleAnim(int w, int h, float fProgress, int supertextIndex,
                                      ID3D12GraphicsCommandList* cmdListIn) {
  int i, x, y;

  int texIndex = supertextIndex;
  if (!m_dx12Title[texIndex].IsValid())
    return;

  if (!m_lpDX || !m_lpDX->m_commandList)
    return;

  auto* cmdList = cmdListIn ? cmdListIn : m_lpDX->m_commandList.Get();
  if (!cmdList)
    return;

  // Set up DX12 rendering state
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  // Bind title texture
  D3D12_GPU_DESCRIPTOR_HANDLE titleSrvHandle = m_lpDX->GetBindingBlockGpuHandle(m_dx12Title[texIndex]);
  cmdList->SetGraphicsRootDescriptorTable(1, titleSrvHandle);

  SPRITEVERTEX v3[128];
  ZeroMemory(v3, sizeof(SPRITEVERTEX) * 128);

  float currentX = m_supertexts[supertextIndex].fX;
  float currentY = m_supertexts[supertextIndex].fY;

  // Initialised here, not left to the else branch below.
  //
  // These were declared uninitialised and assigned ONLY on the not-a-song-title
  // path, but both paths fall through to `wantedCenter = dx/dy` further down,
  // which feeds `offset` and displaces every one of the 128 vertices. A song
  // title therefore centred itself on whatever was in that stack slot.
  //
  // This is the same expression the else branch uses; it just runs before the
  // easing adjustments rather than after, so the eased path still overwrites
  // it. For a song title there is no easing, so fX/fY is the wanted centre --
  // which is what the "make fX and fY work as expected" comment below intends.
  float dx = (currentX * 2 - 1);
  float dy = (currentY * 2 - 1);

  if (m_supertexts[supertextIndex].bIsSongTitle) {
    // positioning:
    float fSizeX = 50.0f / (float)m_supertexts[supertextIndex].nFontSizeUsed * powf(1.5f, m_supertexts[supertextIndex].fFontSize - 2.0f);
    float fSizeY = fSizeX * m_nTitleTexSizeY / (float)m_nTitleTexSizeX;// * m_nWidth/(float)m_nHeight;

    // Clamp for aspect ratio (portrait mode stretches X via aspect correction later)
    {
      float aspectCorr = w / (float)(h * 4.0f / 3.0f) * 1.4f;
      float aspectScale = (aspectCorr < 1.0f) ? aspectCorr : 1.0f;
      float textFillRatio = (m_supertexts[supertextIndex].nTextWidthUsed > 0)
        ? (float)m_supertexts[supertextIndex].nTextWidthUsed / (float)m_nTitleTexSizeX
        : 1.0f;
      float maxAllowed = 0.88f * aspectScale / textFillRatio;
      if (fSizeX > maxAllowed) {
        fSizeY *= maxAllowed / fSizeX;
        fSizeX = maxAllowed;
      }
    }

    i = 0;
    float vert_clip = VERT_CLIP;//1.0f;//0.45f;	// warning: visible clipping has been observed at 0.4!
    for (y = 0; y < 8; y++) {
      for (x = 0; x < 16; x++) {
        v3[i].tu = x / 15.0f;
        v3[i].tv = (y / 7.0f - 0.5f) * vert_clip + 0.5f;
        v3[i].x = (v3[i].tu * 2.0f - 1.0f) * fSizeX;
        v3[i].y = (v3[i].tv * 2.0f - 1.0f) * fSizeY;
        if (fProgress >= 1.0f)
          v3[i].y += 1.0f / (float)m_nTexSizeY;  //this is a pretty hacky guess @ getting it to align...
        i++;
      }
    }

    // warping
    float ramped_progress = max(0.0f, 1 - fProgress * 1.5f);
    float t2 = powf(ramped_progress, 1.8f) * 1.3f;
    for (y = 0; y < 8; y++) {
      for (x = 0; x < 16; x++) {
        i = y * 16 + x;
        v3[i].x += t2 * 0.070f * sinf(GetTime() * 0.31f + v3[i].x * 0.39f - v3[i].y * 1.94f);
        v3[i].x += t2 * 0.044f * sinf(GetTime() * 0.81f - v3[i].x * 1.91f + v3[i].y * 0.27f);
        v3[i].x += t2 * 0.061f * sinf(GetTime() * 1.31f + v3[i].x * 0.61f + v3[i].y * 0.74f);
        v3[i].y += t2 * 0.061f * sinf(GetTime() * 0.37f + v3[i].x * 1.83f + v3[i].y * 0.69f);
        v3[i].y += t2 * 0.070f * sinf(GetTime() * 0.67f + v3[i].x * 0.42f - v3[i].y * 1.39f);
        v3[i].y += t2 * 0.087f * sinf(GetTime() * 1.07f + v3[i].x * 3.55f + v3[i].y * 0.89f);
      }
    }

    // scale down over time
    float scale = 1.01f / (powf(fProgress, 0.21f) + 0.01f);
    for (i = 0; i < 128; i++) {
      v3[i].x *= scale;
      v3[i].y *= scale;
    }
  }
  else { // not song title

    // positioning:
    float fSizeX = (float)m_nTexSizeX / 1024.0f * 100.0f / (float)m_supertexts[supertextIndex].nFontSizeUsed * powf(1.033f, m_supertexts[supertextIndex].fFontSize - 50.0f);
    float fSizeY = fSizeX * m_nTitleTexSizeY / (float)m_nTitleTexSizeX;

    // Clamp so text fits on screen, accounting for growth, aspect, text fill ratio,
    // and off-center positioning. The visible half-width must not exceed 1.0 - abs(dx).
    float maxGrowth = max(1.0f, m_supertexts[supertextIndex].fGrowth);
    float aspectCorr = w / (float)(h * 4.0f / 3.0f) * 1.4f;
    float aspectScale = (aspectCorr < 1.0f) ? aspectCorr : 1.0f;
    float textFillRatio = (m_supertexts[supertextIndex].nTextWidthUsed > 0)
      ? (float)m_supertexts[supertextIndex].nTextWidthUsed / (float)m_nTitleTexSizeX
      : 1.0f;
    float posDx = fabsf(m_supertexts[supertextIndex].fX * 2.0f - 1.0f);
    float kFill = max(0.3f, min(0.88f, 1.0f - posDx - 0.05f));
    float maxAllowed = kFill * aspectScale / (maxGrowth * textFillRatio);
    if (fSizeX > maxAllowed) {
      fSizeY *= maxAllowed / fSizeX;
      fSizeX = maxAllowed;
    }

    i = 0;
    float vert_clip = VERT_CLIP;//0.67f;	// warning: visible clipping has been observed at 0.5 (for very short strings) and even 0.6 (for wingdings)!
    for (y = 0; y < 8; y++) {
      for (x = 0; x < 16; x++) {
        v3[i].tu = x / 15.0f;
        v3[i].tv = (y / 7.0f - 0.5f) * vert_clip + 0.5f;
        v3[i].x = (v3[i].tu * 2.0f - 1.0f) * fSizeX;
        v3[i].y = (v3[i].tv * 2.0f - 1.0f) * fSizeY;
        if (fProgress >= 1.0f)
          v3[i].y += 1.0f / (float)m_nTexSizeY;  //this is a pretty hacky guess @ getting it to align...
        i++;
      }
    }

    // apply 'growth' factor and move to user-specified (x,y)
    {
      float t = (1.0f) * (1 - fProgress) + (fProgress) * (m_supertexts[supertextIndex].fGrowth);
      if (m_supertexts[supertextIndex].fMoveTime == -1) {
        m_supertexts[supertextIndex].fMoveTime = m_supertexts[supertextIndex].fDuration;
      }

      float startTimeProgress = 1;
      if (m_supertexts[supertextIndex].fMoveTime > 0) {
        startTimeProgress = (GetTime() - m_supertexts[supertextIndex].fStartTime) / m_supertexts[supertextIndex].fMoveTime;
      }

      float tFactor = startTimeProgress;
      if (m_supertexts[supertextIndex].nEaseMode == 1) {
        // Ease in: start slow, speed up
        tFactor = powf(tFactor, m_supertexts[supertextIndex].fEaseFactor);
      }
      else if (m_supertexts[supertextIndex].nEaseMode == 2) {
        // Ease out: slow down towards the end
        tFactor = 1.0f - powf(1.0f - tFactor, m_supertexts[supertextIndex].fEaseFactor);
      }

      if (startTimeProgress < 1 && m_supertexts[supertextIndex].fStartX != -100 && m_supertexts[supertextIndex].fStartX != m_supertexts[supertextIndex].fX) {
        currentX -= (m_supertexts[supertextIndex].fX - m_supertexts[supertextIndex].fStartX) * (1 - tFactor);
      }
      dx = (currentX * 2 - 1);

      if (startTimeProgress < 1 && m_supertexts[supertextIndex].fStartY != -100 && m_supertexts[supertextIndex].fStartY != m_supertexts[supertextIndex].fY) {
        currentY -= (m_supertexts[supertextIndex].fY - m_supertexts[supertextIndex].fStartY) * (1 - tFactor);
      }
      dy = (currentY * 2 - 1);

      for (i = 0; i < 128; i++) {
        // note: (x,y) are in (-1,1) range, but m_supertext[supertextIndex].f{X|Y} are in (0..1) range
        v3[i].x = (v3[i].x) * t + dx;
        v3[i].y = (v3[i].y) * t + dy;
      }

      // swprintf(debugMsg, sizeof(debugMsg) / sizeof(debugMsg[0]), L"ShowSongTitleAnim: dx=%.2f dy=%.2f\n", dx, dy);
      // OutputDebugStringW(debugMsg);
    }
  }

  float aspect = w / (float)(h * 4.0f / 3.0f);

  // A kinda hacky solutionto make fX and fY work as expected, eg. so fY=0 always means
  // top of screen, no matter the aspect ratio of the window. Not great but works.
  float posStart, minVal, maxVal, wantedCenter;

  // Adjust to change the proportional scaling of the font. This VALUE seems to match the
  // original font well enough in my tests using Segoe UI.
  aspect *= 1.4f;

  if (aspect < 1) {
    posStart = v3[0].x;
    minVal = posStart + (v3[0].x - posStart) / aspect;
    maxVal = posStart + (v3[127].x - posStart) / aspect;
    wantedCenter = dx;
  }
  else {
    posStart = v3[0].y;
    minVal = posStart + (v3[0].y - posStart) * aspect;
    maxVal = posStart + (v3[127].y - posStart) * aspect;
    wantedCenter = dy;
  }
  float actualCenter = (minVal + maxVal) / 2;
  float offset = actualCenter - wantedCenter;

  for (i = 0; i < 128; i++) {
    if (aspect < 1) {

      //swprintf(debugMsg, sizeof(debugMsg) / sizeof(debugMsg[0]), L"ShowSongTitleAnim: v3[%i].x=%.2f\n", i, v3[i].x);
      //OutputDebugStringW(debugMsg);

      v3[i].x = posStart + (v3[i].x - posStart) / aspect;
      v3[i].x -= offset; // center the text on the wanted position

      //swprintf(debugMsg, sizeof(debugMsg) / sizeof(debugMsg[0]), L"ShowSongTitleAnim: v3[%i].x=%.2f (after)\n", i, v3[i].x);
      //OutputDebugStringW(debugMsg);
    }
    else {

      // swprintf(debugMsg, sizeof(debugMsg) / sizeof(debugMsg[0]), L"ShowSongTitleAnim: v3[%i].y=%.2f\n", i, v3[i].y);
      // OutputDebugStringW(debugMsg);

      v3[i].y = posStart + (v3[i].y - posStart) * aspect;
      v3[i].y -= offset; // center the text on the wanted position

      // swprintf(debugMsg, sizeof(debugMsg) / sizeof(debugMsg[0]), L"ShowSongTitleAnim: v3[%i].y=%.2f (after)\n", i, v3[i].y);
      // OutputDebugStringW(debugMsg);
    }
  }

  // DX12: flip Y axis — DX12 clip space has +Y = top, but the vertex grid
  // maps increasing tv (texture top→bottom) to increasing y, which puts
  // the top of the text at the bottom of the screen without this flip.
  for (i = 0; i < 128; i++)
    v3[i].y = -v3[i].y;

  float t = 1.0f;
  float currentTime = GetTime();

  float fadeInProgress = 1.0f;
  if (m_supertexts[supertextIndex].fFadeInTime > 0) {
    fadeInProgress = (currentTime - m_supertexts[supertextIndex].fStartTime) / m_supertexts[supertextIndex].fFadeInTime;
  }
  float fadeOutStartTime = m_supertexts[supertextIndex].fStartTime + m_supertexts[supertextIndex].fDuration - m_supertexts[supertextIndex].fFadeOutTime;

  float fadeOutProgress = 0.0f;
  if (m_supertexts[supertextIndex].fFadeOutTime > 0) {
    fadeOutProgress = (currentTime - fadeOutStartTime) / m_supertexts[supertextIndex].fFadeOutTime;
  }

  if (m_supertexts[supertextIndex].bIsSongTitle) {
    t = powf(fProgress, 0.3f) * 1.0f;
  }
  else if (fadeInProgress < 1.0f) {
    // Fade-in phase
    t = CosineInterp(max(0.0f, min(1.0f, fadeInProgress)));
  }
  else if (fadeOutProgress >= 0.0f) {
    // Fade-out phase
    t = 1.0f - CosineInterp(max(0.0f, min(1.0f, fadeOutProgress)));
  }

  if (t < 0) {
    t = 0;
  }

  int boxAlpha = (int)(m_supertexts[supertextIndex].fBoxAlpha * 255);
  boxAlpha = std::clamp(boxAlpha, 0, 255);
  boxAlpha = (int)(boxAlpha * t);

  if (boxAlpha > 0) {

    float minX = +1e9f, minY = +1e9f;
    float maxX = -1e9f, maxY = -1e9f;
    for (i = 0; i < 128; ++i) {
      minX = min(minX, v3[i].x);
      minY = min(minY, v3[i].y);
      maxX = max(maxX, v3[i].x);
      maxY = max(maxY, v3[i].y);
    }

    // some reasonable default values
    float centerX = (minX + maxX) * 0.5f;
    float centerY = (minY + maxY) * 0.5f;
    
    float halfWidth = (maxX - minX) * 0.5f;
    float halfHeight = (maxY - minY) * 0.5f;

    minX = centerX - halfWidth * 1.05f * m_supertexts[supertextIndex].fBoxLeft;
    maxX = centerX + halfWidth * 1.1f * m_supertexts[supertextIndex].fBoxRight;

    minY = centerY - halfHeight * 0.8f * m_supertexts[supertextIndex].fBoxTop;
    maxY = centerY + halfHeight * 0.8f * m_supertexts[supertextIndex].fBoxBottom;

    int boxColR = std::clamp(m_supertexts[supertextIndex].fBoxColR, 0, 255);
    int boxColG = std::clamp(m_supertexts[supertextIndex].fBoxColG, 0, 255);
    int boxColB = std::clamp(m_supertexts[supertextIndex].fBoxColB, 0, 255);

    D3DCOLOR boxCol = D3DCOLOR_ARGB(boxAlpha, boxColR, boxColG, boxColB);

    // DX12: Draw box as untextured alpha-blended triangles
    WFVERTEX boxVerts[6] = {
      { minX, minY, 1.0f, boxCol },
      { maxX, minY, 1.0f, boxCol },
      { minX, maxY, 1.0f, boxCol },
      { minX, maxY, 1.0f, boxCol },
      { maxX, minY, 1.0f, boxCol },
      { maxX, maxY, 1.0f, boxCol },
    };

    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_ALPHABLEND_WFVERTEX].Get());
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, boxVerts, 6, sizeof(WFVERTEX), cmdList);
  }

  // nudge down & right for shadow, up & left for solid text
  float offset_x = 0, offset_y = 0;
  float baseOffsetX = m_supertexts[supertextIndex].fShadowOffset / m_nTitleTexSizeX * (m_supertexts[supertextIndex].fFontSize / 40);
  float baseOffsetY = -m_supertexts[supertextIndex].fShadowOffset / m_nTitleTexSizeY * (m_supertexts[supertextIndex].fFontSize / 40);

  int start_it = 0;
  if (m_supertexts[supertextIndex].fBoxAlpha > 0) {
    start_it = 1;
    baseOffsetX = 0;
    baseOffsetY = 0;
  }

  for (int it = start_it; it < 2; it++) {
    // colors
    {
      if (it == 0)
        v3[0].Diffuse = D3DCOLOR_RGBA_01(t, t, t, t);
      else
        v3[0].Diffuse = D3DCOLOR_RGBA_01(t * m_supertexts[supertextIndex].nColorR / 255.0f, t * m_supertexts[supertextIndex].nColorG / 255.0f, t * m_supertexts[supertextIndex].nColorB / 255.0f, t);

      for (i = 1; i < 128; i++)
        v3[i].Diffuse = v3[0].Diffuse;
    }

    switch (it) {
    case 0:
      offset_x = baseOffsetX;
      offset_y = baseOffsetY;
      break;
    case 1:
      offset_x = -2 * baseOffsetX;
      offset_y = -2 * baseOffsetY;
      break;
    }

    for (i = 0; i < 128; i++) {
      v3[i].x += offset_x;
      v3[i].y += offset_y;
    }

    // DX12: Select PSO for this pass
    if (it == 0)
      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_DARKEN_SPRITEVERTEX].Get());
    else
      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_ONEONE_SPRITEVERTEX].Get());

    // Expand indexed mesh to non-indexed triangle list and draw
    SPRITEVERTEX triVerts[7 * 15 * 6]; // 630 vertices (210 triangles)
    int triIdx = 0;
    for (int ty = 0; ty < 7; ty++) {
      for (int tx = 0; tx < 15; tx++) {
        triVerts[triIdx++] = v3[ty * 16 + tx];
        triVerts[triIdx++] = v3[ty * 16 + tx + 1];
        triVerts[triIdx++] = v3[ty * 16 + tx + 16];
        triVerts[triIdx++] = v3[ty * 16 + tx + 1];
        triVerts[triIdx++] = v3[ty * 16 + tx + 16];
        triVerts[triIdx++] = v3[ty * 16 + tx + 17];
      }
    }

    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, triIdx, sizeof(SPRITEVERTEX), cmdList);
  }
}
