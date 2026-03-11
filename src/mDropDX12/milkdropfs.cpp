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


#include "engine.h"
#include "video_capture.h"
#include "resource.h"
#include "support.h"
//#include "evallib\eval.h"		// for math. expr. eval - thanks Francis! (in SourceOffSite, it's the 'vis_avs\evallib' project.)
//#include "evallib\compiler.h"
#include "../ns-eel2/ns-eel.h"
#include "utility.h"
#include <assert.h>
#include <math.h>
#include <algorithm>  // std::swap
using namespace mdrop;

#define D3DCOLOR_RGBA_01(r,g,b,a) D3DCOLOR_RGBA(((int)(r*255)),((int)(g*255)),((int)(b*255)),((int)(a*255)))
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
  swprintf(szTextToDraw, L" %s ", m_supertexts[supertextIndex].szTextW);

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

      RECT temp = rect;
      int h = ::DrawTextW(m_titleDC, szTextToDraw, -1, &temp, DT_SINGLELINE | DT_CALCRECT | DT_CENTER);

      long offset = h / 2;
      if (lineCount > 1) offset *= lineCount;

      temp.left = 0;
      temp.right = m_nTitleTexSizeX;
      temp.top = m_nTitleTexSizeY / 2 - offset;
      temp.bottom = m_nTitleTexSizeY / 2 + offset;

      DWORD flags = (lineCount == 1) ? (DT_SINGLELINE | DT_CENTER) : (DT_WORDBREAK | DT_CENTER);
      m_supertexts[supertextIndex].nFontSizeUsed = ::DrawTextW(m_titleDC, szTextToDraw, -1, &temp, flags);

      // Global autosize: compute fFontSize so text fills ~90% of screen width
      // Skip when slide-in animation is active (text enters from offscreen)
      bool bSlideIn = (m_supertexts[supertextIndex].fStartX != -100.0f &&
                       m_supertexts[supertextIndex].fStartX != m_supertexts[supertextIndex].fX) ||
                      (m_supertexts[supertextIndex].fStartY != -100.0f &&
                       m_supertexts[supertextIndex].fStartY != m_supertexts[supertextIndex].fY);
      if (m_bMessageAutoSize && !bSlideIn && m_supertexts[supertextIndex].nFontSizeUsed > 0) {
        const float kFill = 0.9f;
        float ratio = kFill * (float)m_supertexts[supertextIndex].nFontSizeUsed
                      / ((float)m_nTexSizeX / 1024.0f * 100.0f);
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
          lstrcpyW(&str[(int)(len * fPercentToKeep)], L"...");
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

void mdrop::Engine::LoadPerFrameEvallibVars(CState* pState) {
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

  *pState->var_pf_frame = (double)GetFrame();
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
    *pState->var_pf_aspectx = (double)m_fInvAspectX;
    *pState->var_pf_aspecty = (double)m_fInvAspectY;
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

  // Get the Direct3D device (may be null in DX12 mode — CPU code still runs)
  LPDIRECT3DDEVICE9 lpDevice = GetDevice();

  // Check if black mode is enabled
  if (m_blackmode) {
    if (lpDevice) {
      // Clear the screen to black (DX9 path)
      lpDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
      lpDevice->Present(NULL, NULL, NULL, NULL);
    }
    return;
  }
  {

    float fDeltaT = 1.0f / GetFps();

    if (bRedraw) {
      // pre-un-flip buffers, so we are redoing the same work as we did last frame...
      IDirect3DTexture9* pTemp = m_lpVS[0];
      m_lpVS[0] = m_lpVS[1];
      m_lpVS[1] = pTemp;
    }

    if (GetFrame() == 0) {
      m_fStartTime = GetTime();
      m_fPresetStartTime = GetTime();
      m_bPresetDiagLogged = false;
    }

    if (m_fNextPresetTime < 0) {
      float dt = m_fTimeBetweenPresetsRand * (rand() % 1000) * 0.001f;
      m_fNextPresetTime = GetTime() + m_fBlendTimeAuto + m_fTimeBetweenPresets + dt;
    }

    if (!bRedraw) {
      m_rand_frame = D3DXVECTOR4(FRAND, FRAND, FRAND, FRAND);

      // randomly change the preset, if it's time
      if (m_fNextPresetTime < GetTime()) {
        if (m_nLoadingPreset == 0) // don't start a load if one is already underway!
          LoadRandomPreset(m_fBlendTimeAuto);
      }

      if (MessagesEnabled()) {
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
      if (MessagesEnabled() && m_bMsgAutoplay && m_nMsgAutoplayCount > 0 &&
        m_fNextAutoMsgTime > 0 && GetTime() >= m_fNextAutoMsgTime) {
        int nActiveCustomMsgs = 0;
        for (int j = 0; j < NUM_SUPERTEXTS; j++) {
          if (!m_supertexts[j].bIsSongTitle &&
              m_supertexts[j].fStartTime >= 0 &&
              GetTime() < m_supertexts[j].fStartTime + m_supertexts[j].fDuration + m_supertexts[j].fFadeOutTime)
            nActiveCustomMsgs++;
        }
        if (nActiveCustomMsgs < m_nMsgMaxOnScreen) {
          int msgIdx;
          if (m_bMsgSequential) {
            if (m_nNextSequentialMsg >= m_nMsgAutoplayCount)
              m_nNextSequentialMsg = 0;
            msgIdx = m_nMsgAutoplayOrder[m_nNextSequentialMsg++];
          } else {
            msgIdx = m_nMsgAutoplayOrder[rand() % m_nMsgAutoplayCount];
          }
          LaunchCustomMessage(msgIdx);
          ScheduleNextAutoMessage();
        }
      }

      // update m_fBlendProgress;
      if (m_pState->m_bBlending) {
        m_pState->m_fBlendProgress = (GetTime() - m_pState->m_fBlendStartTime) / m_pState->m_fBlendDuration;
        if (m_pState->m_fBlendProgress > 1.0f) {
          m_pState->m_bBlending = false;
          // Release blend-only PSOs (no longer needed after blend completes)
          m_dx12OldWarpPSO.Reset();
          m_dx12WarpBlendPSO.Reset();
          m_dx12OldCompPSO.Reset();
          m_dx12CompBlendPSO.Reset();
        }
      }

      // handle hard cuts here (just after new sound analysis)
      static float m_fHardCutThresh;
      if (GetFrame() == 0)
        m_fHardCutThresh = m_fHardCutLoudnessThresh * 2.0f;
      if (GetFps() > 1.0f && !m_bHardCutsDisabled && !m_bPresetLockedByUser && !m_bPresetLockedByCode) {
        if (mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2] > m_fHardCutThresh * 3.0f) {
          if (m_nLoadingPreset == 0) // don't start a load if one is already underway!
            LoadRandomPreset(0.0f);
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
      float scale = m_pState->m_fWaveScale.eval(GetTime()) / 128.0f;
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

    // Per-vertex warp computation (CPU-only, no device dependency).
    // Moved here from inside the DX9 rendering block so DX12 path can use the results.
    ComputeGridAlphaValues();

    // ──── DX12 rendering path ────
    if (!lpDevice && m_lpDX && m_lpDX->m_device) {
      DX12_RenderWarpAndComposite();
      std::swap(m_dx12VS[0], m_dx12VS[1]);
      return;
    }

    // ──── DX9 rendering path (original code) ────
    if (!lpDevice)
      return;

    // Remember the original backbuffer and zbuffer
    LPDIRECT3DSURFACE9 pBackBuffer = NULL;//, pZBuffer=NULL;
    lpDevice->GetRenderTarget(0, &pBackBuffer);
    //lpDevice->GetDepthStencilSurface( &pZBuffer );

    // set up render state
    {
      DWORD texaddr = (*m_pState->var_pf_wrap > m_fSnapPoint) ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
      lpDevice->SetRenderState(D3DRS_WRAP0, 0);//D3DWRAPCOORD_0|D3DWRAPCOORD_1|D3DWRAPCOORD_2|D3DWRAPCOORD_3);
      //lpDevice->SetRenderState(D3DRS_WRAP0, (*m_pState->var_pf_wrap) ? D3DWRAP_U|D3DWRAP_V|D3DWRAP_W : 0);
      //lpDevice->SetRenderState(D3DRS_WRAP1, (*m_pState->var_pf_wrap) ? D3DWRAP_U|D3DWRAP_V|D3DWRAP_W : 0);
      lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);//texaddr);
      lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);//texaddr);
      lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);//texaddr);
      lpDevice->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      lpDevice->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      lpDevice->SetSamplerState(1, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);

      lpDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
      lpDevice->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
      lpDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      lpDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
      lpDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      lpDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
      lpDevice->SetRenderState(D3DRS_COLORVERTEX, TRUE);
      lpDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      lpDevice->SetRenderState(D3DRS_AMBIENT, 0xFFFFFFFF);  //?
      lpDevice->SetRenderState(D3DRS_CLIPPING, TRUE);

      // stages 0 and 1 always just use bilinear filtering.
      lpDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      lpDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
      lpDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
      lpDevice->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      lpDevice->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
      lpDevice->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

      // note: this texture stage state setup works for 0 or 1 texture.
      // if you set a texture, it will be modulated with the current diffuse color.
      // if you don't set a texture, it will just use the current diffuse color.
      lpDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      lpDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
      lpDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
      lpDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      lpDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

      // NOTE: don't forget to call SetTexture and SetVertexShader before drawing!
      // Examples:
      //      SPRITEVERTEX verts[4];          // has texcoords
      //   	lpDevice->SetTexture(0, m_sprite_tex);
      //      lpDevice->SetVertexShader( SPRITEVERTEX_FORMAT );
      //
      //      WFVERTEX verts[4];              // no texcoords
      //   	lpDevice->SetTexture(0, NULL);
      //      lpDevice->SetVertexShader( WFVERTEX_FORMAT );
    }

    // render string to m_lpDDSTitle, if necessary
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      if (m_supertexts[i].fStartTime != -1.0f && m_supertexts[i].bRedrawSuperText) {
        if (!RenderStringToTitleTexture(i))
          m_supertexts[i].fStartTime = -1.0f;
        m_supertexts[i].bRedrawSuperText = false;
      }
    }

    // set up to render [from NULL] to VS0 (for motion vectors).
    {
      lpDevice->SetTexture(0, NULL);

      IDirect3DSurface9* pNewTarget = NULL;
      if (m_lpVS == NULL || m_lpVS[0] == NULL || m_lpVS[0]->GetSurfaceLevel(0, &pNewTarget) != D3D_OK)
        return;
      lpDevice->SetRenderTarget(0, pNewTarget);
      //lpDevice->SetDepthStencilSurface( NULL );
      pNewTarget->Release();

      lpDevice->SetTexture(0, NULL);
    }

    // draw motion vectors to VS0
    DrawMotionVectors();

    lpDevice->SetTexture(0, NULL);
    lpDevice->SetTexture(1, NULL);

    // on first frame, clear OLD VS.
    if (m_nFramesSinceResize == 0) {
      IDirect3DSurface9* pNewTarget = NULL;
      if (m_lpVS[0]->GetSurfaceLevel(0, &pNewTarget) != D3D_OK)
        return;
      lpDevice->SetRenderTarget(0, pNewTarget);
      //lpDevice->SetDepthStencilSurface( NULL );
      pNewTarget->Release();

      lpDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    }


    // set up to render [from VS0] to VS1.
    {
      IDirect3DSurface9* pNewTarget = NULL;
      if (m_lpVS[1]->GetSurfaceLevel(0, &pNewTarget) != D3D_OK)
        return;
      lpDevice->SetRenderTarget(0, pNewTarget);
      //lpDevice->SetDepthStencilSurface( NULL );
      pNewTarget->Release();
    }

    if (m_bAutoGamma && GetFrame() == 0) {
      if (strstr(GetDriverDescription(), "nvidia") ||
        strstr(GetDriverDescription(), "nVidia") ||
        strstr(GetDriverDescription(), "NVidia") ||
        strstr(GetDriverDescription(), "NVIDIA"))
        m_n16BitGamma = 2;
      else if (strstr(GetDriverDescription(), "ATI RAGE MOBILITY M"))
        m_n16BitGamma = 2;
      else
        m_n16BitGamma = 0;
    }

    // ComputeGridAlphaValues() already called before DX12/DX9 branch above

    // do the warping for this frame [warp shader]
    if (!m_pState->m_bBlending) {
      // no blend
      if (bNewPresetUsesWarpShader)
        WarpedBlit_Shaders(1, false, false, false, false);
      else
        WarpedBlit_NoShaders(1, false, false, false, false);
    }
    else {
      // blending
      // WarpedBlit( nPass,  bAlphaBlend, bFlipAlpha, bCullTiles, bFlipCulling )
      // note: alpha values go from 0..1 during a blend.
      // note: bFlipCulling==false means tiles with alpha>0 will draw.
      //       bFlipCulling==true  means tiles with alpha<255 will draw.

      if (bOldPresetUsesWarpShader && bNewPresetUsesWarpShader) {
        WarpedBlit_Shaders(0, false, false, true, true);
        WarpedBlit_Shaders(1, true, false, true, false);
      }
      else if (!bOldPresetUsesWarpShader && bNewPresetUsesWarpShader) {
        WarpedBlit_NoShaders(0, false, false, true, true);
        WarpedBlit_Shaders(1, true, false, true, false);
      }
      else if (bOldPresetUsesWarpShader && !bNewPresetUsesWarpShader) {
        WarpedBlit_Shaders(0, false, false, true, true);
        WarpedBlit_NoShaders(1, true, false, true, false);
      }
      else if (!bOldPresetUsesWarpShader && !bNewPresetUsesWarpShader) {
        //WarpedBlit_NoShaders(0, false, false,   true, true);
        //WarpedBlit_NoShaders(1, true,  false,   true, false);

          // special case - all the blending just happens in the vertex UV's, so just pretend there's no blend.
        WarpedBlit_NoShaders(1, false, false, false, false);
      }
    }


    if (m_nMaxPSVersion > 0)
      BlurPasses();

    // draw audio data

    DrawCustomShapes(); // draw these first; better for feedback if the waves draw *over* them.
    DrawCustomWaves();
    DrawWave(mysound.fWave[0], mysound.fWave[1]);

    DrawSprites();

    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;

      // if song title animation just ended, burn it into the VS:
      if (m_supertexts[i].fStartTime >= 0 &&
        fProgress >= 1.0f &&
        !m_supertexts[i].bRedrawSuperText) {

        float fTimeAfterFullDuration = GetTime() - m_supertexts[i].fStartTime - m_supertexts[i].fDuration;
        ShowSongTitleAnim(m_nTexSizeX, m_nTexSizeY, fProgress, i);

        if (fTimeAfterFullDuration >= m_supertexts[i].fBurnTime) {
          m_supertexts[i].fStartTime = -1.0f;	// 'off' state
        }
      }
    }

    // Change the rendertarget back to the original setup
    lpDevice->SetTexture(0, NULL);
    lpDevice->SetRenderTarget(0, pBackBuffer);
    //lpDevice->SetDepthStencilSurface( pZBuffer );
    SafeRelease(pBackBuffer);
    //SafeRelease(pZBuffer);


      // show it to the user [composite shader]
    if (!m_pState->m_bBlending) {
      // no blend
      if (bNewPresetUsesCompShader)
        ShowToUser_Shaders(1, false, false, false, false);
      else
        ShowToUser_NoShaders();//1, false, false, false, false);
    }
    else {
      // blending
      // ShowToUser( nPass,  bAlphaBlend, bFlipAlpha, bCullTiles, bFlipCulling )
      // note: alpha values go from 0..1 during a blend.
      // note: bFlipCulling==false means tiles with alpha>0 will draw.
      //       bFlipCulling==true  means tiles with alpha<255 will draw.

      // NOTE: ShowToUser_NoShaders() must always come before ShowToUser_Shaders(),
      //        because it always draws the full quad (it can't do tile culling or alpha blending).
      //        [third case here]
      if (bOldPresetUsesCompShader && bNewPresetUsesCompShader) {
        ShowToUser_Shaders(0, false, false, true, true);
        ShowToUser_Shaders(1, true, false, true, false);
      }
      else if (!bOldPresetUsesCompShader && bNewPresetUsesCompShader) {
        ShowToUser_NoShaders();
        ShowToUser_Shaders(1, true, false, true, false);
      }
      else if (bOldPresetUsesCompShader && !bNewPresetUsesCompShader) {
        // THA FUNKY REVERSAL
      //ShowToUser_Shaders  (0);
      //ShowToUser_NoShaders(1);
        ShowToUser_NoShaders();
        ShowToUser_Shaders(0, true, true, true, true);
      }
      else if (!bOldPresetUsesCompShader && !bNewPresetUsesCompShader) {
        // special case - all the blending just happens in the blended state vars, so just pretend there's no blend.
        ShowToUser_NoShaders();//1, false, false, false, false);
      }
    }

    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
      // finally, render song title animation to back buffer
      if (m_supertexts[i].fStartTime >= 0 &&
        !m_supertexts[i].bRedrawSuperText
        && fProgress <= 1.0f
        ) {
        ShowSongTitleAnim(GetWidth(), GetHeight(),
          min(fProgress, 0.9999f),
          i);
        // TODO accentuate
        // if (fProgress >= 1.5f)
        //   m_supertexts[i].fStartTime = -1.0f;	// 'off' state
      }
    }
  }

  DrawUserSprites();

  // flip buffers
  IDirect3DTexture9* pTemp = m_lpVS[0];
  m_lpVS[0] = m_lpVS[1];
  m_lpVS[1] = pTemp;

  // Spout output is now handled in engineshell.cpp (between ExecuteCommandList and Present)
  // via SpoutDX12 D3D11On12 interop. See engineshell.cpp DoTime().

} // end RenderFrame

void mdrop::Engine::DrawMotionVectors() {
  // FLEXIBLE MOTION VECTOR FIELD
  if ((float)*m_pState->var_pf_mv_a >= 0.001f) {
    //-------------------------------------------------------
    LPDIRECT3DDEVICE9 lpDevice = GetDevice();
    if (!lpDevice)
      return;

    lpDevice->SetTexture(0, NULL);
    lpDevice->SetVertexShader(NULL);
    lpDevice->SetFVF(WFVERTEX_FORMAT);
    //-------------------------------------------------------

    int x, y;

    int nX = (int)(*m_pState->var_pf_mv_x);// + 0.999f);
    int nY = (int)(*m_pState->var_pf_mv_y);// + 0.999f);
    float dx = (float)*m_pState->var_pf_mv_x - nX;
    float dy = (float)*m_pState->var_pf_mv_y - nY;
    if (nX > 64) { nX = 64; dx = 0; }
    if (nY > 48) { nY = 48; dy = 0; }

    if (nX > 0 && nY > 0) {
      /*
      float dx2 = m_fMotionVectorsTempDx;//(*m_pState->var_pf_mv_dx) * 0.05f*GetTime();		// 0..1 range
      float dy2 = m_fMotionVectorsTempDy;//(*m_pState->var_pf_mv_dy) * 0.05f*GetTime();		// 0..1 range
      if (GetFps() > 2.0f && GetFps() < 300.0f)
      {
        dx2 += (float)(*m_pState->var_pf_mv_dx) * 0.05f / GetFps();
        dy2 += (float)(*m_pState->var_pf_mv_dy) * 0.05f / GetFps();
      }
      if (dx2 > 1.0f) dx2 -= (int)dx2;
      if (dy2 > 1.0f) dy2 -= (int)dy2;
      if (dx2 < 0.0f) dx2 = 1.0f - (-dx2 - (int)(-dx2));
      if (dy2 < 0.0f) dy2 = 1.0f - (-dy2 - (int)(-dy2));
      // hack: when there is only 1 motion vector on the screem, to keep it in
      //       the center, we gradually migrate it toward 0.5.
      dx2 = dx2*0.995f + 0.5f*0.005f;
      dy2 = dy2*0.995f + 0.5f*0.005f;
      // safety catch
      if (dx2 < 0 || dx2 > 1 || dy2 < 0 || dy2 > 1)
      {
        dx2 = 0.5f;
        dy2 = 0.5f;
      }
      m_fMotionVectorsTempDx = dx2;
      m_fMotionVectorsTempDy = dy2;*/
      float dx2 = (float)(*m_pState->var_pf_mv_dx);
      float dy2 = (float)(*m_pState->var_pf_mv_dy);

      float len_mult = (float)*m_pState->var_pf_mv_l;
      if (dx < 0) dx = 0;
      if (dy < 0) dy = 0;
      if (dx > 1) dx = 1;
      if (dy > 1) dy = 1;
      //dx = dx * 1.0f/(float)nX;
      //dy = dy * 1.0f/(float)nY;
      float inv_texsize = 1.0f / (float)m_nTexSizeX;
      float min_len = 1.0f * inv_texsize;

      WFVERTEX v[(64 + 1) * 2];
      ZeroMemory(v, sizeof(WFVERTEX) * (64 + 1) * 2);
      v[0].Diffuse = D3DCOLOR_RGBA_01((float)*m_pState->var_pf_mv_r, (float)*m_pState->var_pf_mv_g, (float)*m_pState->var_pf_mv_b, (float)*m_pState->var_pf_mv_a);
      for (x = 1; x < (nX + 1) * 2; x++)
        v[x].Diffuse = v[0].Diffuse;

      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

      for (y = 0; y < nY; y++) {
        float fy = (y + 0.25f) / (float)(nY + dy + 0.25f - 1.0f);

        // now move by offset
        fy -= dy2;

        if (fy > 0.0001f && fy < 0.9999f) {
          int n = 0;
          for (x = 0; x < nX; x++) {
            //float fx = (x + 0.25f)/(float)(nX + dx + 0.25f - 1.0f);
            float fx = (x + 0.25f) / (float)(nX + dx + 0.25f - 1.0f);

            // now move by offset
            fx += dx2;

            if (fx > 0.0001f && fx < 0.9999f) {
              float fx2, fy2;
              ReversePropagatePoint(fx, fy, &fx2, &fy2);	// NOTE: THIS IS REALLY A REVERSE-PROPAGATION
              //fx2 = fx*2 - fx2;
              //fy2 = fy*2 - fy2;
              //fx2 = fx + 1.0f/(float)m_nTexSize;
              //fy2 = 1-(fy + 1.0f/(float)m_nTexSize);

              // enforce minimum trail lengths:
              {
                float dx = (fx2 - fx);
                float dy = (fy2 - fy);
                dx *= len_mult;
                dy *= len_mult;
                float len = sqrtf(dx * dx + dy * dy);

                if (len > min_len) {

                }
                else if (len > 0.00000001f) {
                  len = min_len / len;
                  dx *= len;
                  dy *= len;
                }
                else {
                  dx = min_len;
                  dy = min_len;
                }

                fx2 = fx + dx;
                fy2 = fy + dy;
              }
              /**/

              v[n].x = fx * 2.0f - 1.0f;
              v[n].y = fy * 2.0f - 1.0f;
              v[n + 1].x = fx2 * 2.0f - 1.0f;
              v[n + 1].y = fy2 * 2.0f - 1.0f;

              // actually, project it in the reverse direction
              //v[n+1].x = v[n].x*2.0f - v[n+1].x;// + dx*2;
              //v[n+1].y = v[n].y*2.0f - v[n+1].y;// + dy*2;
              //v[n].x += dx*2;
              //v[n].y += dy*2;

              n += 2;
            }
          }

          // draw it
          lpDevice->DrawPrimitiveUP(D3DPT_LINELIST, n / 2, v, sizeof(WFVERTEX));
        }
      }

      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
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

bool mdrop::Engine::ReversePropagatePoint(float fx, float fy, float* fx2, float* fy2) {
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
  tu = m_verts[y0 * (m_nGridX + 1) + x0].tu * (1 - dx) * (1 - dy);
  tv = m_verts[y0 * (m_nGridX + 1) + x0].tv * (1 - dx) * (1 - dy);
  tu += m_verts[y0 * (m_nGridX + 1) + x1].tu * (dx) * (1 - dy);
  tv += m_verts[y0 * (m_nGridX + 1) + x1].tv * (dx) * (1 - dy);
  tu += m_verts[y1 * (m_nGridX + 1) + x0].tu * (1 - dx) * (dy);
  tv += m_verts[y1 * (m_nGridX + 1) + x0].tv * (1 - dx) * (dy);
  tu += m_verts[y1 * (m_nGridX + 1) + x1].tu * (dx) * (dy);
  tv += m_verts[y1 * (m_nGridX + 1) + x1].tv * (dx) * (dy);

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

void mdrop::Engine::BlurPasses() {
#if (NUM_BLUR_TEX>0)

  // Note: Blur is currently a little funky.  It blurs the *current* frame after warp;
  //         this way, it lines up well with the composite pass.  However, if you switch
  //         presets instantly, to one whose *warp* shader uses the blur texture,
  //         it will be outdated (just for one frame).  Oh well.
  //       This also means that when sampling the blurred textures in the warp shader,
  //         they are one frame old.  This isn't too big a deal.  Getting them to match
  //         up for the composite pass is probably more important.

  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  int passes = min(NUM_BLUR_TEX, m_nHighestBlurTexUsedThisFrame * 2);
  if (passes == 0)
    return;

  LPDIRECT3DSURFACE9 pBackBuffer = NULL;//, pZBuffer=NULL;
  lpDevice->GetRenderTarget(0, &pBackBuffer);

  //lpDevice->SetFVF( MYVERTEX_FORMAT );
  lpDevice->SetVertexShader(m_BlurShaders[0].vs.ptr);
  lpDevice->SetVertexDeclaration(m_pMyVertDecl);
  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  DWORD wrap = D3DTADDRESS_CLAMP;//D3DTADDRESS_WRAP;// : D3DTADDRESS_CLAMP;
  lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, wrap);
  lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, wrap);
  lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSW, wrap);
  lpDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1);

  IDirect3DSurface9* pNewTarget = NULL;

  // clear texture bindings
  for (int i = 0; i < 16; i++)
    lpDevice->SetTexture(i, NULL);

  // set up fullscreen quad
  MYVERTEX v[4];

  v[0].x = -1;
  v[0].y = -1;
  v[1].x = 1;
  v[1].y = -1;
  v[2].x = -1;
  v[2].y = 1;
  v[3].x = 1;
  v[3].y = 1;

  v[0].tu = 0;    //kiv: upside-down?
  v[0].tv = 0;
  v[1].tu = 1;
  v[1].tv = 0;
  v[2].tu = 0;
  v[2].tv = 1;
  v[3].tu = 1;
  v[3].tv = 1;

  const float w[8] = { 4.0f, 3.8f, 3.5f, 2.9f, 1.9f, 1.2f, 0.7f, 0.3f };  //<- user can specify these
  float edge_darken = (float)*m_pState->var_pf_blur1_edge_darken;
  float blur_min[3], blur_max[3];
  GetSafeBlurMinMax(m_pState, blur_min, blur_max);

  float fscale[3];
  float fbias[3];

  // figure out the progressive scale & bias needed, at each step,
  // to go from one [min..max] range to the next.
  float temp_min, temp_max;
  fscale[0] = 1.0f / (blur_max[0] - blur_min[0]);
  fbias[0] = -blur_min[0] * fscale[0];
  temp_min = (blur_min[1] - blur_min[0]) / (blur_max[0] - blur_min[0]);
  temp_max = (blur_max[1] - blur_min[0]) / (blur_max[0] - blur_min[0]);
  fscale[1] = 1.0f / (temp_max - temp_min);
  fbias[1] = -temp_min * fscale[1];
  temp_min = (blur_min[2] - blur_min[1]) / (blur_max[1] - blur_min[1]);
  temp_max = (blur_max[2] - blur_min[1]) / (blur_max[1] - blur_min[1]);
  fscale[2] = 1.0f / (temp_max - temp_min);
  fbias[2] = -temp_min * fscale[2];

  // note: warped blit just rendered from VS0 to VS1.
  for (int i = 0; i < passes; i++) {
    // hook up correct render target
    if (m_lpBlur[i]->GetSurfaceLevel(0, &pNewTarget) != D3D_OK)
      return;
    lpDevice->SetRenderTarget(0, pNewTarget);
    pNewTarget->Release();

    // hook up correct source texture - assume there is only one, at stage 0
    lpDevice->SetTexture(0, (i == 0) ? m_lpVS[0] : m_lpBlur[i - 1]);

    // set pixel shader
    lpDevice->SetPixelShader(m_BlurShaders[i % 2].ps.ptr);

    // set constants
    LPD3DXCONSTANTTABLE pCT = m_BlurShaders[i % 2].ps.CT;
    D3DXHANDLE* h = m_BlurShaders[i % 2].ps.params.const_handles;

    int srcw = (i == 0) ? GetWidth() : m_nBlurTexW[i - 1];
    int srch = (i == 0) ? GetHeight() : m_nBlurTexH[i - 1];
    D3DXVECTOR4 srctexsize = D3DXVECTOR4((float)srcw, (float)srch, 1.0f / (float)srcw, 1.0f / (float)srch);

    float fscale_now = fscale[i / 2];
    float fbias_now = fbias[i / 2];

    if (i % 2 == 0) {
      // pass 1 (long horizontal pass)
      //-------------------------------------
      const float w1 = w[0] + w[1];
      const float w2 = w[2] + w[3];
      const float w3 = w[4] + w[5];
      const float w4 = w[6] + w[7];
      const float d1 = 0 + 2 * w[1] / w1;
      const float d2 = 2 + 2 * w[3] / w2;
      const float d3 = 4 + 2 * w[5] / w3;
      const float d4 = 6 + 2 * w[7] / w4;
      const float w_div = 0.5f / (w1 + w2 + w3 + w4);
      //-------------------------------------
      //float4 _c0; // source texsize (.xy), and inverse (.zw)
      //float4 _c1; // w1..w4
      //float4 _c2; // d1..d4
      //float4 _c3; // scale, bias, w_div, 0
      //-------------------------------------
      if (h[0]) pCT->SetVector(lpDevice, h[0], &srctexsize);
      if (h[1]) pCT->SetVector(lpDevice, h[1], &D3DXVECTOR4(w1, w2, w3, w4));
      if (h[2]) pCT->SetVector(lpDevice, h[2], &D3DXVECTOR4(d1, d2, d3, d4));
      if (h[3]) pCT->SetVector(lpDevice, h[3], &D3DXVECTOR4(fscale_now, fbias_now, w_div, 0));
    }
    else {
      // pass 2 (short vertical pass)
      //-------------------------------------
      const float w1 = w[0] + w[1] + w[2] + w[3];
      const float w2 = w[4] + w[5] + w[6] + w[7];
      const float d1 = 0 + 2 * ((w[2] + w[3]) / w1);
      const float d2 = 2 + 2 * ((w[6] + w[7]) / w2);
      const float w_div = 1.0f / ((w1 + w2) * 2);
      //-------------------------------------
      //float4 _c0; // source texsize (.xy), and inverse (.zw)
      //float4 _c5; // w1,w2,d1,d2
      //float4 _c6; // w_div, edge_darken_c1, edge_darken_c2, edge_darken_c3
      //-------------------------------------
      if (h[0]) pCT->SetVector(lpDevice, h[0], &srctexsize);
      if (h[5]) pCT->SetVector(lpDevice, h[5], &D3DXVECTOR4(w1, w2, d1, d2));
      if (h[6]) {
        // note: only do this first time; if you do it many times,
        // then the super-blurred levels will have big black lines along the top & left sides.
        if (i == 1)
          pCT->SetVector(lpDevice, h[6], &D3DXVECTOR4(w_div, (1 - edge_darken), edge_darken, 5.0f)); //darken edges
        else
          pCT->SetVector(lpDevice, h[6], &D3DXVECTOR4(w_div, 1.0f, 0.0f, 5.0f)); // don't darken
      }
    }

    // draw fullscreen quad
    lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MYVERTEX));

    // clear texture bindings
    lpDevice->SetTexture(0, NULL);
  }

  lpDevice->SetRenderTarget(0, pBackBuffer);
  pBackBuffer->Release();
  lpDevice->SetPixelShader(NULL);
  lpDevice->SetVertexShader(NULL);
  lpDevice->SetTexture(0, NULL);
  lpDevice->SetFVF(MYVERTEX_FORMAT);
#endif

  m_nHighestBlurTexUsedThisFrame = 0;
}

// ---------------------------------------------------------------------------
// DX12 blur passes: gaussian blur pyramid from VS0 into m_dx12Blur[0..5].
// Each pair (0,1), (2,3), (4,5) is a horizontal + vertical pass at
// progressively halved resolution.  Blur textures are sampled in comp
// shaders via GetBlur1/2/3().
// ---------------------------------------------------------------------------
void mdrop::Engine::DX12_BlurPasses()
{
#if (NUM_BLUR_TEX > 0)
  if (!m_lpDX || !m_lpDX->m_device || !m_lpDX->m_commandList)
    return;

  int passes = min(NUM_BLUR_TEX, m_nHighestBlurTexUsedThisFrame * 2);

  // Diagnostic: log blur pass info once per preset load
  bool bLogDiag = !m_bPresetDiagLogged && GetTime() - m_fPresetStartTime >= 0.0f;
  if (bLogDiag) {
    DLOG_VERBOSE("DX12 BlurPasses: highest=%d, passes=%d, PSO[0]=%s, PSO[1]=%s, blur[1].srv=%u, blur[3].srv=%u",
            m_nHighestBlurTexUsedThisFrame, passes,
            m_dx12BlurPSO[0] ? "OK" : "NULL", m_dx12BlurPSO[1] ? "OK" : "NULL",
            m_dx12Blur[1].srvIndex, m_dx12Blur[3].srvIndex);
  }

  if (passes == 0)
    return;

  // Check blur PSOs are available
  if (!m_dx12BlurPSO[0] || !m_dx12BlurPSO[1])
    return;

  auto* cmdList = m_lpDX->m_commandList.Get();

  // Fullscreen quad vertices (MYVERTEX for input layout compatibility)
  MYVERTEX v[4];
  ZeroMemory(v, sizeof(v));
  v[0].x = -1; v[0].y =  1; v[0].z = 0; v[0].Diffuse = 0xFFFFFFFF; v[0].tu = 0; v[0].tv = 0;
  v[1].x =  1; v[1].y =  1; v[1].z = 0; v[1].Diffuse = 0xFFFFFFFF; v[1].tu = 1; v[1].tv = 0;
  v[2].x = -1; v[2].y = -1; v[2].z = 0; v[2].Diffuse = 0xFFFFFFFF; v[2].tu = 0; v[2].tv = 1;
  v[3].x =  1; v[3].y = -1; v[3].z = 0; v[3].Diffuse = 0xFFFFFFFF; v[3].tu = 1; v[3].tv = 1;

  // Blur weights and min/max
  const float w[8] = { 4.0f, 3.8f, 3.5f, 2.9f, 1.9f, 1.2f, 0.7f, 0.3f };
  float edge_darken = (float)*m_pState->var_pf_blur1_edge_darken;
  float blur_min[3], blur_max[3];
  GetSafeBlurMinMax(m_pState, blur_min, blur_max);

  // Progressive scale & bias
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

  // Ensure descriptor heaps are set
  // Use blur root signature (s0 = CLAMP) — SM5.0 assigns the blur shader's
  // single sampler to s0, and DX9 blur passes use CLAMP addressing.
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_blurRootSignature.Get());

  // DIAG: log blur details once per preset load (using flag set above)

  for (int i = 0; i < passes; i++) {
    // Source: pass 0 reads VS[0], subsequent passes read blur[i-1]
    DX12Texture& srcTex = (i == 0) ? m_dx12VS[0] : m_dx12Blur[i - 1];
    DX12Texture& dstTex = m_dx12Blur[i];

    // Transition resources
    m_lpDX->TransitionResource(srcTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_lpDX->TransitionResource(dstTex, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Set render target and viewport
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_lpDX->GetRtvCpuHandle(dstTex);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    SetViewportAndScissor(cmdList, dstTex.width, dstTex.height);

    // Set blur PSO: even passes = horizontal (blur1), odd = vertical (blur2)
    cmdList->SetPipelineState(m_dx12BlurPSO[i % 2].Get());

    // Update blur binding block with source texture at slot 12
    m_lpDX->UpdateBlurPassBinding(i, srcTex.srvIndex);
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBlurPassBindingGpuHandle(i));

    // Set constants via the blur CT
    LPD3DXCONSTANTTABLE pCT = m_BlurShaders[i % 2].ps.CT;
    D3DXHANDLE* h = m_BlurShaders[i % 2].ps.params.const_handles;

    int srcw = (i == 0) ? GetWidth() : m_nBlurTexW[i - 1];
    int srch = (i == 0) ? GetHeight() : m_nBlurTexH[i - 1];
    D3DXVECTOR4 srctexsize = D3DXVECTOR4((float)srcw, (float)srch, 1.0f / (float)srcw, 1.0f / (float)srch);

    float fscale_now = fscale[i / 2];
    float fbias_now = fbias[i / 2];

    if (bLogDiag) {
      DLOG_VERBOSE("DIAG BlurPass[%d]: src=%ux%u(srv=%u) dst=%ux%u(srv=%u,rtv=%u) "
              "CT=%p h[0]=%p h[1]=%p h[2]=%p h[3]=%p h[5]=%p h[6]=%p "
              "srcTexSize=(%.1f,%.1f,%.6f,%.6f) fscale=%.3f fbias=%.3f shadowSz=%u",
              i, srcTex.width, srcTex.height, srcTex.srvIndex,
              dstTex.width, dstTex.height, dstTex.srvIndex, dstTex.rtvIndex,
              (void*)pCT, (void*)h[0], (void*)h[1], (void*)h[2], (void*)h[3], (void*)h[5], (void*)h[6],
              srctexsize.x, srctexsize.y, srctexsize.z, srctexsize.w,
              fscale_now, fbias_now,
              pCT ? static_cast<DX12ConstantTable*>(pCT)->GetShadowSize() : 0);
    }

    if (i % 2 == 0) {
      // Horizontal pass (blur1_ps): _c0=srctexsize, _c1=weights, _c2=distances, _c3=scale/bias/w_div
      const float w1 = w[0] + w[1];
      const float w2 = w[2] + w[3];
      const float w3 = w[4] + w[5];
      const float w4 = w[6] + w[7];
      const float d1 = 0 + 2 * w[1] / w1;
      const float d2 = 2 + 2 * w[3] / w2;
      const float d3 = 4 + 2 * w[5] / w3;
      const float d4 = 6 + 2 * w[7] / w4;
      const float w_div = 0.5f / (w1 + w2 + w3 + w4);

      if (h[0]) pCT->SetVector(nullptr, h[0], &srctexsize);
      if (h[1]) pCT->SetVector(nullptr, h[1], &D3DXVECTOR4(w1, w2, w3, w4));
      if (h[2]) pCT->SetVector(nullptr, h[2], &D3DXVECTOR4(d1, d2, d3, d4));
      if (h[3]) pCT->SetVector(nullptr, h[3], &D3DXVECTOR4(fscale_now, fbias_now, w_div, 0));
    }
    else {
      // Vertical pass (blur2_ps): _c0=srctexsize, _c5=weights/distances, _c6=w_div/edge_darken
      const float w1 = w[0] + w[1] + w[2] + w[3];
      const float w2 = w[4] + w[5] + w[6] + w[7];
      const float d1 = 0 + 2 * ((w[2] + w[3]) / w1);
      const float d2 = 2 + 2 * ((w[6] + w[7]) / w2);
      const float w_div = 1.0f / ((w1 + w2) * 2);

      if (h[0]) pCT->SetVector(nullptr, h[0], &srctexsize);
      if (h[5]) pCT->SetVector(nullptr, h[5], &D3DXVECTOR4(w1, w2, d1, d2));
      if (h[6]) {
        if (i == 1)
          pCT->SetVector(nullptr, h[6], &D3DXVECTOR4(w_div, (1 - edge_darken), edge_darken, 5.0f));
        else
          pCT->SetVector(nullptr, h[6], &D3DXVECTOR4(w_div, 1.0f, 0.0f, 5.0f));
      }
    }

    // Upload CBV from the blur CT shadow buffer
    DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(pCT);
    if (ct && ct->GetShadowSize() > 0) {
      D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
          m_lpDX->UploadConstantBuffer(ct->GetShadowData(), ct->GetShadowSize());
      if (cbAddr)
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
    }

    // Draw fullscreen quad
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX));
  }

  // Transition blur textures to SRV state for sampling in comp shader
  for (int i = 0; i < passes; i++)
    m_lpDX->TransitionResource(m_dx12Blur[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  m_nHighestBlurTexUsedThisFrame = 0;
#endif
}

// ---------------------------------------------------------------------------
// DX12 rendering: warp pass (VS0 → VS1) + composite pass (VS1 → backbuffer)
// Called from RenderFrame after CPU-side computation (per-frame equations,
// per-vertex warp) has filled m_verts[] with final UVs.
// ---------------------------------------------------------------------------
// ── Shadertoy render path: Buffer A → Image, no warp/blur/shapes ──
void mdrop::Engine::RenderFrameShadertoy(ID3D12GraphicsCommandList* cmdList)
{
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
    wchar_t diagPath[MAX_PATH];
    swprintf(diagPath, MAX_PATH, L"%sdiag_bindings.txt", m_szBaseDir);
    FILE* fp = nullptr;
    _wfopen_s(&fp, diagPath, L"w");
    if (fp) fclose(fp);
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

    // HUD notification
    wchar_t note[256];
    swprintf(note, 256, L"Shadertoy: fb=%s bufA_PSO=%s comp_PSO=%s (%dx%d)",
      m_dx12Feedback[0].IsValid() ? L"OK" : L"FAIL",
      m_dx12BufferAPSO ? L"OK" : L"NULL",
      m_dx12CompPSO ? L"OK" : L"NULL",
      m_nTexSizeX, m_nTexSizeY);
    AddNotification(note, 5.0f);
  }

  if (stFrame < 2) {
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

  // Build binding slots for Buffer A, Buffer B, and comp/Image passes
  UINT warpSlots[32], bufferASlots[32], bufferBSlots[32], compSlots[32];
  memset(warpSlots, 0xFF, sizeof(warpSlots));  // warp unused in Shadertoy mode
  memset(bufferBSlots, 0xFF, sizeof(bufferBSlots));

  {
    // Image feedback read texture (previous Image output, if Image self-feedback is used)
    const DX12Texture* imgFbRead = m_bCompUsesImageFeedback ? &m_dx12ImageFeedback[fbRead] : nullptr;

    if (m_bHasBufferA) {
      // Buffer A reads FeedbackA[fbRead] (own previous output) + FeedbackB[fbRead]
      BuildBindingSlots(&m_shaders.bufferA.params, m_dx12VS[1], bufferASlots,
                        &m_dx12Feedback[fbRead], imgFbRead, &m_dx12FeedbackB[fbRead]);
    } else {
      memset(bufferASlots, 0xFF, sizeof(bufferASlots));
    }

    if (m_bHasBufferB) {
      // Buffer B reads FeedbackA[fbWrite] (current frame's Buffer A output, rendered before B)
      // + FeedbackB[fbRead] (own previous output, for self-feedback)
      // This matches Shadertoy.com's sequential execution: A→B→Image within each frame.
      BuildBindingSlots(&m_shaders.bufferB.params, m_dx12VS[1], bufferBSlots,
                        &m_dx12Feedback[fbWrite], imgFbRead, &m_dx12FeedbackB[fbRead]);
    }

    if (m_bHasBufferA || m_bHasBufferB) {
      // Image/comp reads FeedbackA[fbWrite] + FeedbackB[fbWrite] (current frame outputs)
      BuildBindingSlots(&m_shaders.comp.params, m_dx12VS[1], compSlots,
                        &m_dx12Feedback[fbWrite], imgFbRead, &m_dx12FeedbackB[fbWrite]);
    } else {
      BuildBindingSlots(&m_shaders.comp.params, m_dx12VS[1], compSlots,
                        &m_dx12Feedback[fbRead], imgFbRead, &m_dx12FeedbackB[fbRead]);
    }
  }
  {
    UINT oldWarpSlots[32], oldCompSlots[32];
    memset(oldWarpSlots, 0xFF, sizeof(oldWarpSlots));
    memset(oldCompSlots, 0xFF, sizeof(oldCompSlots));
    m_lpDX->UpdatePerFrameBindings(warpSlots, bufferASlots, bufferBSlots, compSlots,
                                   oldWarpSlots, oldCompSlots);
  }

  // ── Buffer A pass: render to feedback[fbWrite] ──
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

    // Fullscreen quad
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

    // Transition feedback[write] to SRV so Image pass can read it
    m_lpDX->TransitionResource(fbWriteTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // ── Buffer B pass: render to feedbackB[fbWrite] ──
  if (m_bHasBufferB && m_dx12BufferBPSO && m_dx12FeedbackB[fbWrite].IsValid()) {
    DX12Texture& fbBWriteTex = m_dx12FeedbackB[fbWrite];

    m_lpDX->TransitionResource(fbBWriteTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE fbBRtv = m_lpDX->GetRtvCpuHandle(fbBWriteTex);
    cmdList->OMSetRenderTargets(1, &fbBRtv, FALSE, nullptr);

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

    // Fullscreen quad (same UVs as Buffer A — DX convention, no Y-flip)
    MYVERTEX bufBQuad[4];
    ZeroMemory(bufBQuad, sizeof(bufBQuad));
    bufBQuad[0].x = -1.f; bufBQuad[0].y =  1.f; bufBQuad[0].z = 0.f; bufBQuad[0].Diffuse = 0xFFFFFFFF;
    bufBQuad[0].tu = 0.f; bufBQuad[0].tv = 0.f; bufBQuad[0].tu_orig = 0.f; bufBQuad[0].tv_orig = 0.f; bufBQuad[0].rad = 1.f; bufBQuad[0].ang = 3.14159f;
    bufBQuad[1].x =  1.f; bufBQuad[1].y =  1.f; bufBQuad[1].z = 0.f; bufBQuad[1].Diffuse = 0xFFFFFFFF;
    bufBQuad[1].tu = 1.f; bufBQuad[1].tv = 0.f; bufBQuad[1].tu_orig = 1.f; bufBQuad[1].tv_orig = 0.f; bufBQuad[1].rad = 1.f; bufBQuad[1].ang = 0.f;
    bufBQuad[2].x = -1.f; bufBQuad[2].y = -1.f; bufBQuad[2].z = 0.f; bufBQuad[2].Diffuse = 0xFFFFFFFF;
    bufBQuad[2].tu = 0.f; bufBQuad[2].tv = 1.f; bufBQuad[2].tu_orig = 0.f; bufBQuad[2].tv_orig = 1.f; bufBQuad[2].rad = 1.f; bufBQuad[2].ang = 3.14159f;
    bufBQuad[3].x =  1.f; bufBQuad[3].y = -1.f; bufBQuad[3].z = 0.f; bufBQuad[3].Diffuse = 0xFFFFFFFF;
    bufBQuad[3].tu = 1.f; bufBQuad[3].tv = 1.f; bufBQuad[3].tu_orig = 1.f; bufBQuad[3].tv_orig = 1.f; bufBQuad[3].rad = 1.f; bufBQuad[3].ang = 0.f;
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, bufBQuad, 4, sizeof(MYVERTEX));

    // Transition feedbackB[write] to SRV so Image pass can read it
    m_lpDX->TransitionResource(fbBWriteTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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

    if (m_dx12CompPSO) {
      cmdList->SetPipelineState(m_dx12CompPSO.Get());
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

    // Fullscreen quad — UVs flipped vertically (tv=1 at top, tv=0 at bottom) so that
    // the Image shader's fragCoord matches Shadertoy convention (y=0 at bottom, y=H at top)
    // while Buffer A's DX-convention feedback loop remains self-consistent.
    MYVERTEX quad[4];
    ZeroMemory(quad, sizeof(quad));
    quad[0].x = -1.f; quad[0].y =  1.f; quad[0].z = 0.f; quad[0].Diffuse = 0xFFFFFFFF;
    quad[0].tu = 0.f; quad[0].tv = 1.f; quad[0].tu_orig = 0.f; quad[0].tv_orig = 1.f; quad[0].rad = 1.f; quad[0].ang = 3.14159f;
    quad[1].x =  1.f; quad[1].y =  1.f; quad[1].z = 0.f; quad[1].Diffuse = 0xFFFFFFFF;
    quad[1].tu = 1.f; quad[1].tv = 1.f; quad[1].tu_orig = 1.f; quad[1].tv_orig = 1.f; quad[1].rad = 1.f; quad[1].ang = 0.f;
    quad[2].x = -1.f; quad[2].y = -1.f; quad[2].z = 0.f; quad[2].Diffuse = 0xFFFFFFFF;
    quad[2].tu = 0.f; quad[2].tv = 0.f; quad[2].tu_orig = 0.f; quad[2].tv_orig = 0.f; quad[2].rad = 1.f; quad[2].ang = 3.14159f;
    quad[3].x =  1.f; quad[3].y = -1.f; quad[3].z = 0.f; quad[3].Diffuse = 0xFFFFFFFF;
    quad[3].tu = 1.f; quad[3].tv = 0.f; quad[3].tu_orig = 1.f; quad[3].tv_orig = 0.f; quad[3].rad = 1.f; quad[3].ang = 0.f;
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, quad, 4, sizeof(MYVERTEX));

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

      // Fullscreen quad with standard UVs (no flip — feedback buffer is already in DX convention)
      MYVERTEX blitQ[4];
      ZeroMemory(blitQ, sizeof(blitQ));
      blitQ[0].x = -1.f; blitQ[0].y =  1.f; blitQ[0].z = 0.f; blitQ[0].Diffuse = 0xFFFFFFFF;
      blitQ[0].tu = 0.f; blitQ[0].tv = 0.f;
      blitQ[1].x =  1.f; blitQ[1].y =  1.f; blitQ[1].z = 0.f; blitQ[1].Diffuse = 0xFFFFFFFF;
      blitQ[1].tu = 1.f; blitQ[1].tv = 0.f;
      blitQ[2].x = -1.f; blitQ[2].y = -1.f; blitQ[2].z = 0.f; blitQ[2].Diffuse = 0xFFFFFFFF;
      blitQ[2].tu = 0.f; blitQ[2].tv = 1.f;
      blitQ[3].x =  1.f; blitQ[3].y = -1.f; blitQ[3].z = 0.f; blitQ[3].Diffuse = 0xFFFFFFFF;
      blitQ[3].tu = 1.f; blitQ[3].tv = 1.f;
      m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, blitQ, 4, sizeof(MYVERTEX));
    }
  }

  // ── Draw behind-text sprites (layer 0) on the backbuffer ──
  if (SpritesEnabled())
    DrawUserSprites(0);

  // ── Display active supertexts on the backbuffer ──
  if (MessagesEnabled()) {
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
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

  // ── Shadertoy pipeline: skip warp/blur/shapes entirely ──
  if (m_bShadertoyMode) {
    RenderFrameShadertoy(cmdList);
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
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
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
    //   No comp shader (passthrough) → TEX_VS binds VS[1] (matches ShowToUser_NoShaders)
    bool bNewUsesCompShader = (m_pState->m_nCompPSVersion > 0);
    const DX12Texture& compVsTex = bNewUsesCompShader ? m_dx12VS[0] : m_dx12VS[1];

    UINT warpSlots[32], bufferASlots[32], bufferBSlots[32], compSlots[32];
    UINT oldWarpSlots[32], oldCompSlots[32];
    memset(bufferBSlots, 0xFF, sizeof(bufferBSlots));  // Buffer B unused in non-Shadertoy mode
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
      bool bOldUsesCompShader = (m_pOldState && m_pOldState->m_nCompPSVersion > 0);
      const DX12Texture& oldCompVsTex = bOldUsesCompShader ? m_dx12VS[0] : m_dx12VS[1];
      BuildBindingSlots(&m_OldShaders.comp.params, oldCompVsTex, oldCompSlots);
    }

    m_lpDX->UpdatePerFrameBindings(warpSlots, bufferASlots, bufferBSlots, compSlots,
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
    // bFlipCulling: true = draw tiles where alpha<255 (old preset), false = draw tiles where alpha>0 (new preset).
    int primCount = m_nGridX * m_nGridY * 2;
    int totalVerts = primCount * 3;
    MYVERTEX tempv[1024 * 3];
    int max_per_batch = (sizeof(tempv) / sizeof(tempv[0])) / 3 - 4;

    auto drawWarpMesh = [&](D3DCOLOR cDecay, bool bCullTiles, bool bFlipCulling) {
      int src_idx = 0;
      while (src_idx < totalVerts) {
        int prims_queued = 0;
        int i = 0;
        while (prims_queued < max_per_batch && src_idx < totalVerts) {
          MYVERTEX v0 = m_verts[m_indices_list[src_idx]];
          MYVERTEX v1 = m_verts[m_indices_list[src_idx + 1]];
          MYVERTEX v2 = m_verts[m_indices_list[src_idx + 2]];
          src_idx += 3;

          if (bCullTiles) {
            BYTE a0 = (BYTE)(v0.Diffuse >> 24);
            BYTE a1 = (BYTE)(v1.Diffuse >> 24);
            BYTE a2 = (BYTE)(v2.Diffuse >> 24);
            if (bFlipCulling) {
              // Pass 0 (old preset): skip if all verts fully blended to new (alpha==0xFF)
              if (a0 == 0xFF && a1 == 0xFF && a2 == 0xFF) continue;
            } else {
              // Pass 1 (new preset): skip if all verts fully old (alpha==0x00)
              if (a0 == 0x00 && a1 == 0x00 && a2 == 0x00) continue;
            }
          }

          tempv[i] = v0;
          tempv[i].Diffuse = (cDecay & 0x00FFFFFF) | (v0.Diffuse & 0xFF000000);
          i++;
          tempv[i] = v1;
          tempv[i].Diffuse = (cDecay & 0x00FFFFFF) | (v1.Diffuse & 0xFF000000);
          i++;
          tempv[i] = v2;
          tempv[i].Diffuse = (cDecay & 0x00FFFFFF) | (v2.Diffuse & 0xFF000000);
          i++;
          prims_queued++;
        }
        if (prims_queued > 0)
          m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, tempv, prims_queued * 3, sizeof(MYVERTEX));
      }
    };

    auto bindWarpShader = [&](PShaderInfo* si, CState* pState, D3D12_GPU_DESCRIPTOR_HANDLE bindingHandle) {
      if (si->CT) {
        ApplyShaderParams(&si->params, si->CT, pState);
        DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(si->CT);
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
      cmdList->SetGraphicsRootDescriptorTable(1, bindingHandle);
    };

    bool bBlending = m_pState->m_bBlending;
    bool bOldUsesWarpShader = bBlending && (m_pOldState->m_nWarpPSVersion > 0);
    bool bNewUsesWarpShader = (m_pState->m_nWarpPSVersion > 0);

    if (bBlending && (bOldUsesWarpShader || bNewUsesWarpShader)) {
      // ── Two-pass warp blending (matches DX9 WarpedBlit logic) ──
      // Pass 0: old preset (opaque), cull tiles fully blended to new
      if (bOldUsesWarpShader && m_dx12OldWarpPSO) {
        cmdList->SetPipelineState(m_dx12OldWarpPSO.Get());
        float fDecayOld = (float)(*m_pOldState->var_pf_decay);
        D3DCOLOR cDecayOld = D3DCOLOR_RGBA_01(fDecayOld, fDecayOld, fDecayOld, 1);
        bindWarpShader(&m_OldShaders.warp, m_pOldState, m_lpDX->GetOldWarpBindingGpuHandle());
        drawWarpMesh(cDecayOld, true, true);
      } else if (!bOldUsesWarpShader) {
        // Old preset has no warp shader — use fallback PSO (texture * vertex color)
        cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
        float fDecay = (float)(*m_pOldState->var_pf_decay);
        D3DCOLOR cDecayOld = D3DCOLOR_RGBA_01(fDecay, fDecay, fDecay, 1);
        BYTE zeros[256] = {};
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
        if (cbAddr) cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetOldWarpBindingGpuHandle());
        drawWarpMesh(cDecayOld, true, true);
      }

      // Pass 1: new preset (alpha blend), cull tiles fully old
      if (bNewUsesWarpShader && m_dx12WarpBlendPSO) {
        cmdList->SetPipelineState(m_dx12WarpBlendPSO.Get());
        float fDecayNew = (float)(*m_pState->var_pf_decay);
        D3DCOLOR cDecayNew = D3DCOLOR_RGBA_01(fDecayNew, fDecayNew, fDecayNew, 1);
        bindWarpShader(&m_shaders.warp, m_pState, m_lpDX->GetWarpBindingGpuHandle());
        drawWarpMesh(cDecayNew, true, false);
      } else if (!bNewUsesWarpShader) {
        // New preset has no warp shader — use fallback PSO with alpha blend
        // Note: PSO_TEXTURED_MYVERTEX doesn't have alpha blend, but for the non-shader
        // case the UV blending is sufficient (DX9 line 1107 uses single pass).
        cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
        float fDecay = (float)(*m_pState->var_pf_decay);
        D3DCOLOR cDecayNew = D3DCOLOR_RGBA_01(fDecay, fDecay, fDecay, 1);
        BYTE zeros[256] = {};
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
        if (cbAddr) cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetWarpBindingGpuHandle());
        drawWarpMesh(cDecayNew, true, false);
      }
    } else if (bBlending && !bOldUsesWarpShader && !bNewUsesWarpShader) {
      // Special case: neither uses shader — UV blending is sufficient, single pass (DX9 line 1107)
      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
      float fDecay = (float)(*m_pState->var_pf_decay);
      D3DCOLOR cDecay = D3DCOLOR_RGBA_01(fDecay, fDecay, fDecay, 1);
      BYTE zeros[256] = {};
      D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
      if (cbAddr) cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
      cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetWarpBindingGpuHandle());
      drawWarpMesh(cDecay, false, false);
    } else {
      // ── No blend: single-pass warp (existing path) ──
      if (m_dx12WarpPSO) {
        cmdList->SetPipelineState(m_dx12WarpPSO.Get());
      } else {
        cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
      }

      bindWarpShader(&m_shaders.warp, m_pState, m_lpDX->GetWarpBindingGpuHandle());

      // Always pass per-frame decay via vertex color (like Milkwave DX9).
      // Generated warp shaders multiply ret by _vDiffuse.xyz for decay.
      float fDecay = (float)(*m_pState->var_pf_decay);
      D3DCOLOR cDecay = D3DCOLOR_RGBA_01(fDecay, fDecay, fDecay, 1);
      DLOG_VERBOSE("DIAG Warp decay: fDecay=%.4f cDecay=0x%08X hasPSO=%d", fDecay, cDecay, m_dx12WarpPSO ? 1 : 0);

      drawWarpMesh(cDecay, false, false);
    }

  }

  // ── Blur passes: build blur pyramid from VS0 (just-warped frame) ──
  // Must run after warp (which wrote VS1 from VS0) so that comp shaders
  // can sample blur textures via GetBlur1/2/3().
  // Pre-scan both warp and comp shader blur texture usage so blur passes
  // cover all needs (ApplyShaderParams for comp runs AFTER blur passes).
  // Some presets use GetBlur1() in the warp shader (e.g. organic12-3d-2.milk).
  {
    CShaderParams* shaderParams[] = { &m_shaders.warp.params, &m_shaders.comp.params };
    // During blend, also scan old shaders so blur textures they need are generated
    if (m_pState->m_bBlending) {
      CShaderParams* oldParams[] = { &m_OldShaders.warp.params, &m_OldShaders.comp.params };
      for (auto* sp : oldParams) {
        for (int i = 0; i < 32; i++) {
          if (sp->m_texcode[i] >= TEX_BLUR1 && sp->m_texcode[i] <= TEX_BLUR_LAST)
            m_nHighestBlurTexUsedThisFrame = max(m_nHighestBlurTexUsedThisFrame,
                ((int)sp->m_texcode[i] - (int)TEX_BLUR1) + 1);
        }
      }
    }
    for (auto* sp : shaderParams) {
      for (int i = 0; i < 32; i++) {
        if (sp->m_texcode[i] >= TEX_BLUR1 && sp->m_texcode[i] <= TEX_BLUR_LAST)
          m_nHighestBlurTexUsedThisFrame = max(m_nHighestBlurTexUsedThisFrame,
              ((int)sp->m_texcode[i] - (int)TEX_BLUR1) + 1);
      }
    }
  }
  DX12_BlurPasses();

  // ── Inject content into VS1 (drawn after warp, before composite) ──
  // VS1 is still the render target from the warp pass above.
  // Restore VS1 as render target (blur passes may have changed it).
  // Draw order matches original MilkDrop: shapes → custom waves → wave → sprites/borders
  {
    m_lpDX->TransitionResource(m_dx12VS[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_lpDX->GetRtvCpuHandle(m_dx12VS[1]);
    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    SetViewportAndScissor(cmdList, m_nTexSizeX, m_nTexSizeY);

    DX12_DrawCustomShapes();
    DX12_DrawCustomWaves();
    DX12_DrawWave(mysound.fWave[0], mysound.fWave[1]);
    DX12_DrawSprites();

    // Burn completed supertexts into VS1 (persistence through warp feedback)
    if (MessagesEnabled()) {
      for (int i = 0; i < NUM_SUPERTEXTS; i++) {
        if (m_supertexts[i].fStartTime >= 0 && !m_supertexts[i].bRedrawSuperText) {
          float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
          if (fProgress >= 1.0f) {
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

    // Compute hue_shader corner colors (matches ShowToUser_Shaders comp grid logic).
    // DX9 ShowToUser_Shaders hardcodes fShaderAmount=1 for shader comp presets,
    // so animated shade colors are always applied at full strength.
    float shade[4][3];
    for (int i = 0; i < 4; i++) {
      shade[i][0] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0143f + 3 + i * 21 + m_fRandStart[3]);
      shade[i][1] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0107f + 1 + i * 13 + m_fRandStart[1]);
      shade[i][2] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0129f + 6 + i * 9 + m_fRandStart[2]);
      float mx = ((shade[i][0] > shade[i][1]) ? shade[i][0] : shade[i][1]);
      if (shade[i][2] > mx) mx = shade[i][2];
      for (int k = 0; k < 3; k++) {
        shade[i][k] /= mx;
        shade[i][k] = 0.5f + 0.5f * shade[i][k];
      }
    }
    DWORD cShade[4] = {
      D3DCOLOR_RGBA_01(shade[1][0], shade[1][1], shade[1][2], 1),  // top-left
      D3DCOLOR_RGBA_01(shade[0][0], shade[0][1], shade[0][2], 1),  // top-right
      D3DCOLOR_RGBA_01(shade[3][0], shade[3][1], shade[3][2], 1),  // bottom-left
      D3DCOLOR_RGBA_01(shade[2][0], shade[2][1], shade[2][2], 1),  // bottom-right
    };

    // Helper: build and draw a fullscreen comp quad with given alpha
    auto drawCompQuad = [&](BYTE alpha) {
      auto applyAlpha = [](DWORD color, BYTE a) -> DWORD {
        return (color & 0x00FFFFFF) | ((DWORD)a << 24);
      };
      MYVERTEX quad[4];
      ZeroMemory(quad, sizeof(quad));
      quad[0].x = -1.f; quad[0].y =  1.f; quad[0].z = 0.f; quad[0].Diffuse = applyAlpha(cShade[0], alpha);
      quad[0].tu = 0.f; quad[0].tv = 0.f; quad[0].tu_orig = 0.f; quad[0].tv_orig = 0.f; quad[0].rad = 1.f; quad[0].ang = 3.14159f;
      quad[1].x =  1.f; quad[1].y =  1.f; quad[1].z = 0.f; quad[1].Diffuse = applyAlpha(cShade[1], alpha);
      quad[1].tu = 1.f; quad[1].tv = 0.f; quad[1].tu_orig = 1.f; quad[1].tv_orig = 0.f; quad[1].rad = 1.f; quad[1].ang = 0.f;
      quad[2].x = -1.f; quad[2].y = -1.f; quad[2].z = 0.f; quad[2].Diffuse = applyAlpha(cShade[2], alpha);
      quad[2].tu = 0.f; quad[2].tv = 1.f; quad[2].tu_orig = 0.f; quad[2].tv_orig = 1.f; quad[2].rad = 1.f; quad[2].ang = 3.14159f;
      quad[3].x =  1.f; quad[3].y = -1.f; quad[3].z = 0.f; quad[3].Diffuse = applyAlpha(cShade[3], alpha);
      quad[3].tu = 1.f; quad[3].tv = 1.f; quad[3].tu_orig = 1.f; quad[3].tv_orig = 1.f; quad[3].rad = 1.f; quad[3].ang = 0.f;
      m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, quad, 4, sizeof(MYVERTEX));
    };

    auto bindCompShader = [&](PShaderInfo* si, CState* pState, D3D12_GPU_DESCRIPTOR_HANDLE bindingHandle) {
      if (si->CT) {
        ApplyShaderParams(&si->params, si->CT, pState);
        DX12ConstantTable* ct = static_cast<DX12ConstantTable*>(si->CT);
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
      cmdList->SetGraphicsRootDescriptorTable(1, bindingHandle);
    };

    bool bCompBlending = m_pState->m_bBlending;
    bool bOldUsesCompShader = bCompBlending && (m_pOldState->m_nCompPSVersion > 0);
    bool bNewUsesCompShader = (m_pState->m_nCompPSVersion > 0);

    if (bCompBlending && (bOldUsesCompShader || bNewUsesCompShader)) {
      // ── Two-pass comp blending: uniform crossfade ──
      float fBlend = m_pState->m_fBlendProgress;
      BYTE blendAlpha = (BYTE)(min(max(fBlend, 0.0f), 1.0f) * 255.0f);

      // Pass 0: old comp shader (opaque)
      if (bOldUsesCompShader && m_dx12OldCompPSO) {
        cmdList->SetPipelineState(m_dx12OldCompPSO.Get());
        bindCompShader(&m_OldShaders.comp, m_pOldState, m_lpDX->GetOldCompBindingGpuHandle());
        drawCompQuad(0xFF);
      } else if (!bOldUsesCompShader) {
        cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
        BYTE zeros[256] = {};
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
        if (cbAddr) cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetOldCompBindingGpuHandle());
        drawCompQuad(0xFF);
      }

      // Pass 1: new comp shader (alpha blend)
      if (bNewUsesCompShader && m_dx12CompBlendPSO) {
        cmdList->SetPipelineState(m_dx12CompBlendPSO.Get());
        bindCompShader(&m_shaders.comp, m_pState, m_lpDX->GetCompBindingGpuHandle());
        drawCompQuad(blendAlpha);
      } else if (!bNewUsesCompShader) {
        // No new comp shader — fallback with alpha
        cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
        BYTE zeros[256] = {};
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(zeros, 256);
        if (cbAddr) cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
        cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetCompBindingGpuHandle());
        drawCompQuad(blendAlpha);
      }
    } else {
      // ── No blend: single-pass comp (existing path) ──
      if (m_dx12CompPSO) {
        cmdList->SetPipelineState(m_dx12CompPSO.Get());
      } else {
        cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_TEXTURED_MYVERTEX].Get());
      }
      bindCompShader(&m_shaders.comp, m_pState, m_lpDX->GetCompBindingGpuHandle());
      drawCompQuad(0xFF);
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
        m_lpDX->TransitionResource(m_spoutInput->dx12InputTex, D3D12_RESOURCE_STATE_COPY_DEST);
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

  // ── Draw behind-text sprites (layer 0) on the backbuffer ──
  if (SpritesEnabled())
    DrawUserSprites(0);

  // ── Display active supertexts on the backbuffer ──
  if (MessagesEnabled()) {
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
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

void mdrop::Engine::DX12_DrawWave(float* fL, float* fR) {
  if (!m_lpDX || !m_lpDX->m_commandList)
    return;

  WFVERTEX v1[576 + 1], v2[576 + 1];

  float cr = (float)(*m_pState->var_pf_wave_r);
  float cg = (float)(*m_pState->var_pf_wave_g);
  float cb = (float)(*m_pState->var_pf_wave_b);
  float cx = (float)(*m_pState->var_pf_wave_x);
  float cy = (float)(*m_pState->var_pf_wave_y);
  float fWaveParam = (float)(*m_pState->var_pf_wave_mystery);

  if (cr < 0) cr = 0;
  if (cg < 0) cg = 0;
  if (cb < 0) cb = 0;
  if (cr > 1) cr = 1;
  if (cg > 1) cg = 1;
  if (cb > 1) cb = 1;

  if (*m_pState->var_pf_wave_brighten) {
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

  float bass_rel = mysound.imm[0];
  float mid_rel = mysound.imm[1];
  float treble_rel = mysound.imm[2];

  int sample_offset = 0;
  int new_wavemode = (int)(*m_pState->var_pf_wave_mode) % NUM_WAVES;

  int its = (m_pState->m_bBlending && (new_wavemode != m_pState->m_nOldWaveMode)) ? 2 : 1;
  int nVerts1 = 0;
  int nVerts2 = 0;
  int nBreak1 = -1;
  int nBreak2 = -1;
  float alpha1, alpha2;

  for (int it = 0; it < its; it++) {
    int   wave = (it == 0) ? new_wavemode : m_pState->m_nOldWaveMode;
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

    float alpha = (float)(*m_pState->var_pf_wave_a);

    switch (wave) {
    case 0:
      // circular wave
      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
            v[i].x = rad * cosf(ang) * m_fAspectY + fWavePosX;
            v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY;
          }
        }
      }
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 1:
      // x-y osc. spiral
      alpha *= 1.25f;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      nVerts /= 2;
      for (int i = 0; i < nVerts; i++) {
        float rad = 0.53f + 0.43f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 1.57f + GetTime() * 2.3f;
        if (m_bScreenDependentRenderMode) {
          v[i].x = rad * cosf(ang) + fWavePosX;
          v[i].y = rad * sinf(ang) + fWavePosY;
        } else {
          v[i].x = rad * cosf(ang) * m_fAspectY + fWavePosX;
          v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY;
        }
      }
      break;

    case 2:
      // centered spiro (alpha constant)
      switch (m_nTexSizeX) {
      case 256:  alpha *= 0.07f; break;
      case 512:  alpha *= 0.09f; break;
      case 1024: alpha *= 0.11f; break;
      case 2048: alpha *= 0.13f; break;
      }
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      for (int i = 0; i < nVerts; i++) {
        if (m_bScreenDependentRenderMode) {
          v[i].x = fR[i] + fWavePosX;
          v[i].y = fL[i + 32] + fWavePosY;
        } else {
          v[i].x = fR[i] * m_fAspectY + fWavePosX;
          v[i].y = fL[i + 32] * m_fAspectX + fWavePosY;
        }
      }
      break;

    case 3:
      // centered spiro (alpha tied to volume)
      switch (m_nTexSizeX) {
      case 256:  alpha = 0.075f; break;
      case 512:  alpha = 0.150f; break;
      case 1024: alpha = 0.220f; break;
      case 2048: alpha = 0.330f; break;
      }
      alpha *= 1.3f;
      alpha *= powf(treble_rel, 2.0f);
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      for (int i = 0; i < nVerts; i++) {
        if (m_bScreenDependentRenderMode) {
          v[i].x = fR[i] + fWavePosX;
          v[i].y = fL[i + 32] + fWavePosY;
        } else {
          v[i].x = fR[i] * m_fAspectY + fWavePosX;
          v[i].y = fL[i + 32] * m_fAspectX + fWavePosY;
        }
      }
      break;

    case 4:
      // horizontal script
      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
      switch (m_nTexSizeX) {
      case 256:  alpha *= 0.07f; break;
      case 512:  alpha *= 0.09f; break;
      case 1024: alpha *= 0.11f; break;
      case 2048: alpha *= 0.13f; break;
      }
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
            v[i].x = (x0 * cos_rot - y0 * sin_rot) * m_fAspectY + fWavePosX;
            v[i].y = (x0 * sin_rot + y0 * cos_rot) * m_fAspectX + fWavePosY;
          }
        }
      }
      break;

    case 6:
    case 7:
    case 8:
      nVerts /= 2;
      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;
      if (wave == 8)
        nVerts = 256;
      else
        sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
            float f = 0.1f * logf(mysound.fSpecLeft[i * 2] + mysound.fSpecLeft[i * 2 + 1]);
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
      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;
      if (wave == 8)
        nVerts = 256;
      else
        sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      nVerts /= 2;
      for (int i = 0; i < nVerts; i++) {
        float rad = 0.63f + 0.23f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 0.9f + GetTime() * 3.3f;
        if (m_bScreenDependentRenderMode) {
          v[i].x = rad * cosf(ang + alpha) + fWavePosX;
          v[i].y = rad * sinf(ang) + fWavePosY;
        } else {
          v[i].x = rad * cosf(ang + alpha) * m_fAspectY + fWavePosX;
          v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY;
        }
      }
      break;

    case 13:
      // Star Wave
      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
            v[i].x = rad * cosf(ang) * m_fAspectY + fWavePosX;
            v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY;
          }
        }
      }
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 14:
      // Flower Wave
      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
            v[i].x = rad * cosf(ang * 3.1416f) * m_fAspectY / 1.5f + fWavePosX * cosf(3.1416f);
            v[i].y = rad * sinf(ang - GetTime() / 3) * m_fAspectX / 1.5f + fWavePosY * cosf(3.1416f);
          }
        }
      }
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 15:
      // Lasso Wave
      alpha *= 1.25f;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      nVerts /= 2;
      for (int i = 0; i < nVerts; i++) {
        float rad = 0.53f + 0.43f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 1.57f + GetTime() * 2.0f;
        float t = GetTime() / ang;
        v[i].x = (float)(cos(GetTime()) / 2 + cosf(ang * 2 + tanf(t)));
        v[i].y = (float)(sin(GetTime()) * 2 * sinf(ang * 3.14f) * m_fAspectX / 2.8f + fWavePosY);
      }
      break;

    case 16:
      // Triangle Wave
      nVerts = 256;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
            v[i].x = x_rot * m_fAspectY + fWavePosX;
            v[i].y = y_rot * m_fAspectX + fWavePosY;
          }
        }
      }
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 17:
      // Fireworks Waveform
      nVerts = 256;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
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
        float audio_boost = 1.0f + 2.0f * (mysound.imm_rel[0] + mysound.imm_rel[1]) * 0.5f;
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
            v[i].x = x * m_fAspectY + fWavePosX;
            v[i].y = y * m_fAspectX + fWavePosY;
          }
          alpha *= burst_fade;
        }
      }
      if (!m_pState->m_bBlending) {
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
  float mix = CosineInterp(m_pState->m_fBlendProgress);
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
  bool additive = (*m_pState->var_pf_wave_additive) != 0;
  bool useDots = (*m_pState->var_pf_wave_usedots) != 0;
  DX12PsoId psoId;
  D3D12_PRIMITIVE_TOPOLOGY topology;
  if (useDots) {
    psoId = additive ? PSO_POINT_ADDITIVE_WFVERTEX : PSO_POINT_ALPHABLEND_WFVERTEX;
    topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  } else {
    psoId = additive ? PSO_LINE_ADDITIVE_WFVERTEX : PSO_LINE_ALPHABLEND_WFVERTEX;
    topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
  }

  auto* cmdList = m_lpDX->m_commandList.Get();
  cmdList->SetPipelineState(m_lpDX->m_PSOs[psoId].Get());

  // Draw with thickness support (4 offset passes for thick/dot mode)
  float x_inc = 2.0f / (float)m_nTexSizeX;
  float y_inc = 2.0f / (float)m_nTexSizeY;
  int drawing_its = ((*m_pState->var_pf_wave_thick || useDots) && (m_nTexSizeX >= 512)) ? 4 : 1;

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

void mdrop::Engine::DX12_DrawSprites() {
  if (!m_lpDX || !m_lpDX->m_commandList)
    return;

  auto* cmdList = m_lpDX->m_commandList.Get();

  // Darken center
  if (*m_pState->var_pf_darken_center) {
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
      v3[1].x = 0.0f - fHalfSize * m_fAspectY;
    v3[2].x = 0.0f;
    if (m_bScreenDependentRenderMode)
      v3[3].x = 0.0f + fHalfSize;
    else
      v3[3].x = 0.0f + fHalfSize * m_fAspectY;
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
    float fOuterBorderSize = (float)*m_pState->var_pf_ob_size;
    float fInnerBorderSize = (float)*m_pState->var_pf_ib_size;

    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_ALPHABLEND_WFVERTEX].Get());

    for (int it = 0; it < 2; it++) {
      WFVERTEX v3[4];
      ZeroMemory(v3, sizeof(WFVERTEX) * 4);

      float r = (it == 0) ? (float)*m_pState->var_pf_ob_r : (float)*m_pState->var_pf_ib_r;
      float g = (it == 0) ? (float)*m_pState->var_pf_ob_g : (float)*m_pState->var_pf_ib_g;
      float b = (it == 0) ? (float)*m_pState->var_pf_ob_b : (float)*m_pState->var_pf_ib_b;
      float a = (it == 0) ? (float)*m_pState->var_pf_ob_a : (float)*m_pState->var_pf_ib_a;
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

void mdrop::Engine::DX12_DrawCustomShapes() {
  if (!m_lpDX || !m_lpDX->m_commandList)
    return;

  auto* cmdList = m_lpDX->m_commandList.Get();

  // --- Heavy preset detection: compute total instances across all shapes ---
  if (m_bSkipHeavyPresets) {
    int totalInstances = 0;
    for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
      if (m_pState->m_shape[i].enabled)
        totalInstances += m_pState->m_shape[i].instances;
      if (m_pState->m_bBlending && m_pOldState->m_shape[i].enabled)
        totalInstances += m_pOldState->m_shape[i].instances;
    }
    if (totalInstances > m_nHeavyPresetMaxInstances) {
      DLOG_INFO("GPU Protection: Skipping shapes — total instances %d exceeds threshold %d (preset: %ls)",
              totalInstances, m_nHeavyPresetMaxInstances,
              wcsrchr(m_szCurrentPresetFile, L'\\') ? wcsrchr(m_szCurrentPresetFile, L'\\') + 1 : m_szCurrentPresetFile);
      return;
    }
  }

  // --- Compute effective instance cap (resolution scaling + hard cap) ---
  int effectiveMaxInstances = 0; // 0 = unlimited
  if (m_bScaleInstancesByResolution && m_nTexSizeX > m_nInstanceScaleBaseWidth) {
    // Scale instances inversely with pixel count relative to base resolution
    // e.g. at 4K (3840) with base 1920: scale = (1920/3840)^2 = 0.25 → cap ~256
    // e.g. at 5K (5120) with base 1920: scale = (1920/5120)^2 = 0.14 → cap ~143
    float scale = (float)m_nInstanceScaleBaseWidth / (float)m_nTexSizeX;
    scale = scale * scale; // squared — proportional to pixel count ratio
    if (scale < 0.1f) scale = 0.1f;
    effectiveMaxInstances = (int)(1024.0f * scale);
    if (effectiveMaxInstances < 16) effectiveMaxInstances = 16;
  }
  if (m_nMaxShapeInstances > 0) {
    if (effectiveMaxInstances == 0 || m_nMaxShapeInstances < effectiveMaxInstances)
      effectiveMaxInstances = m_nMaxShapeInstances;
  }

  int diag_shapesDrawn = 0, diag_shapesVisible = 0;
  float diag_firstVisibleAlpha = 0;
  DWORD diag_firstVisibleColor = 0;

  int num_reps = (m_pState->m_bBlending) ? 2 : 1;
  for (int rep = 0; rep < num_reps; rep++) {
    CState* pState = (rep == 0) ? m_pState : m_pOldState;
    float alpha_mult = 1;
    if (num_reps == 2)
      alpha_mult = (rep == 0) ? m_pState->m_fBlendProgress : (1 - m_pState->m_fBlendProgress);

    for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
      if (pState->m_shape[i].enabled) {
        int instances = pState->m_shape[i].instances;

        // Apply instance cap
        if (effectiveMaxInstances > 0 && instances > effectiveMaxInstances) {
          // Log once per preset (within first half-second)
          if (GetTime() - m_fPresetStartTime < 0.5f) {
            DLOG_INFO("GPU Protection: Capping shape[%d] instances from %d to %d (res=%dx%d, preset: %ls)",
                    i, instances, effectiveMaxInstances, m_nTexSizeX, m_nTexSizeY,
                    wcsrchr(m_szCurrentPresetFile, L'\\') ? wcsrchr(m_szCurrentPresetFile, L'\\') + 1 : m_szCurrentPresetFile);
          }
          instances = effectiveMaxInstances;
        }

        for (int instance = 0; instance < instances; instance++) {
          LoadCustomShapePerFrameEvallibVars(pState, i, instance);

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
          v[0].y = (float)(*pState->m_shape[i].var_pf_y * 2 - 1);
          v[0].z = 0;
          v[0].tu = 0.5f;
          v[0].tv = 0.5f;
          // Early-exit: skip shapes where both center and outer alpha are zero.
          // With SrcAlpha/InvSrcAlpha blending, alpha=0 shapes contribute nothing
          // but still cost draw calls. Matches MilkDrop2 behavior where invisible
          // shapes don't affect the render target.
          float shapeA  = (float)*pState->m_shape[i].var_pf_a  * alpha_mult;
          float shapeA2 = (float)*pState->m_shape[i].var_pf_a2 * alpha_mult;
          if (shapeA <= 0.0f && shapeA2 <= 0.0f)
            continue;

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
              v[j].x = v[0].x + (float)*pState->m_shape[i].var_pf_rad * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_ang + 3.1415927f * 0.25f) * m_fAspectY;
            v[j].y = v[0].y - (float)*pState->m_shape[i].var_pf_rad * sinf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_ang + 3.1415927f * 0.25f);
            v[j].z = 0;
            if (m_bScreenDependentRenderMode)
              v[j].tu = 0.5f + 0.5f * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom);
            else
              v[j].tu = 0.5f + 0.5f * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom) * m_fAspectY;
            v[j].tv = 0.5f + 0.5f * sinf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom);
            v[j].Diffuse = v[1].Diffuse;
          }
          v[sides + 1] = v[1];

          // Draw fill: fan of sides+2 verts → expand to trilist
          if (textured) {
            // Bind VS0 texture at t0 for textured shapes
            cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetSrvGpuHandle(m_dx12VS[0]));
            bool bClamp = (*pState->var_pf_wrap <= m_fSnapPoint);
            DX12PsoId fillPso = additive
              ? (bClamp ? PSO_ADDITIVE_CLAMP_SPRITEVERTEX : PSO_ADDITIVE_SPRITEVERTEX)
              : (bClamp ? PSO_TEXTURED_CLAMP_SPRITEVERTEX : PSO_TEXTURED_SPRITEVERTEX);
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
            cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_LINE_ALPHABLEND_WFVERTEX].Get());

            int its = ((int)(*pState->m_shape[i].var_pf_thick) != 0) ? 4 : 1;
            float x_inc = 2.0f / (float)m_nTexSizeX;
            float y_inc = 2.0f / (float)m_nTexSizeY;
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
}

void mdrop::Engine::DX12_DrawCustomWaves() {
  if (!m_lpDX || !m_lpDX->m_commandList)
    return;

  auto* cmdList = m_lpDX->m_commandList.Get();

  int num_reps = (m_pState->m_bBlending) ? 2 : 1;
  for (int rep = 0; rep < num_reps; rep++) {
    CState* pState = (rep == 0) ? m_pState : m_pOldState;
    float alpha_mult = 1;
    if (num_reps == 2)
      alpha_mult = (rep == 0) ? m_pState->m_fBlendProgress : (1 - m_pState->m_fBlendProgress);

    for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
      if (pState->m_wave[i].enabled) {
        int nSamples = pState->m_wave[i].samples;
        int max_samples = pState->m_wave[i].bSpectrum ? 512 : NUM_WAVEFORM_SAMPLES;
        if (nSamples > max_samples)
          nSamples = max_samples;
        nSamples -= pState->m_wave[i].sep;

        // 1. execute per-frame code
        LoadCustomWavePerFrameEvallibVars(pState, i);

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
          int j;
          float tempdata[2][512];
          float mult = ((pState->m_wave[i].bSpectrum) ? 0.15f : 0.004f) * pState->m_wave[i].scaling * pState->m_fWaveScale.eval(-1);
          float* pdata1 = (pState->m_wave[i].bSpectrum) ? m_sound.fSpectrum[0] : m_sound.fWaveform[0];
          float* pdata2 = (pState->m_wave[i].bSpectrum) ? m_sound.fSpectrum[1] : m_sound.fWaveform[1];

          int j0 = (pState->m_wave[i].bSpectrum) ? 0 : (max_samples - nSamples) / 2 - pState->m_wave[i].sep / 2;
          int j1 = (pState->m_wave[i].bSpectrum) ? 0 : (max_samples - nSamples) / 2 + pState->m_wave[i].sep / 2;
          float t = (pState->m_wave[i].bSpectrum) ? (max_samples - pState->m_wave[i].sep) / (float)nSamples : 1;
          float mix1 = powf(pState->m_wave[i].smoothing * 0.98f, 0.5f);
          float mix2 = 1 - mix1;

          tempdata[0][0] = pdata1[j0];
          tempdata[1][0] = pdata2[j1];
          for (int j = 1; j < nSamples; j++) {
            tempdata[0][j] = pdata1[(int)(j * t) + j0] * mix2 + tempdata[0][j - 1] * mix1;
            tempdata[1][j] = pdata2[(int)(j * t) + j1] * mix2 + tempdata[1][j - 1] * mix1;
          }
          for (j = nSamples - 2; j >= 0; j--) {
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
              v[j].x = (float)(*pState->m_wave[i].var_pp_x * 2 - 1);
              v[j].y = (float)(*pState->m_wave[i].var_pp_y * 2 - 1);
            } else {
              v[j].x = (float)(*pState->m_wave[i].var_pp_x * 2 - 1) * m_fInvAspectX;
              v[j].y = (float)(*pState->m_wave[i].var_pp_y * 2 - 1) * m_fInvAspectY;
            }

            v[j].z = 0;
            v[j].Diffuse =
              ((((int)(*pState->m_wave[i].var_pp_a * 255 * alpha_mult)) & 0xFF) << 24) |
              ((((int)(*pState->m_wave[i].var_pp_r * 255)) & 0xFF) << 16) |
              ((((int)(*pState->m_wave[i].var_pp_g * 255)) & 0xFF) << 8) |
              ((((int)(*pState->m_wave[i].var_pp_b * 255)) & 0xFF));
          }

          // 3. smooth it
          WFVERTEX v2[2048];
          WFVERTEX* pVerts = v;
          if (!pState->m_wave[i].bUseDots) {
            nSamples = SmoothWave(v, nSamples, v2);
            pVerts = v2;
          }

          // 4. draw it — select PSO based on additive + dots
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

          int its = (pState->m_wave[i].bDrawThick && !useDots) ? 4 : 1;
          float x_inc = 2.0f / (float)m_nTexSizeX;
          float y_inc = 2.0f / (float)m_nTexSizeY;
          for (int it = 0; it < its; it++) {
            switch (it) {
            case 0: break;
            case 1: for (int j = 0; j < nSamples; j++) pVerts[j].x += x_inc; break;
            case 2: for (int j = 0; j < nSamples; j++) pVerts[j].y += y_inc; break;
            case 3: for (int j = 0; j < nSamples; j++) pVerts[j].x -= x_inc; break;
            }
            m_lpDX->DrawVertices(topology, pVerts, nSamples, sizeof(WFVERTEX));
          }
        }
      }
    }
  }
}

void mdrop::Engine::ComputeGridAlphaValues() {
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
    float fZoom = (float)(*pState->var_pf_zoom);
    float fZoomExp = (float)(*pState->var_pf_zoomexp);
    float fRot = (float)(*pState->var_pf_rot);
    float fWarp = (float)(*pState->var_pf_warp);
    float fCX = (float)(*pState->var_pf_cx);
    float fCY = (float)(*pState->var_pf_cy);
    float fDX = (float)(*pState->var_pf_dx);
    float fDY = (float)(*pState->var_pf_dy);
    float fSX = (float)(*pState->var_pf_sx);
    float fSY = (float)(*pState->var_pf_sy);

    int n = 0;

    for (int y = 0; y <= m_nGridY; y++) {
      for (int x = 0; x <= m_nGridX; x++) {
        // Note: x, y, z are now set at init. time - no need to mess with them!
        //m_verts[n].x = i/(float)m_nGridX*2.0f - 1.0f;
        //m_verts[n].y = j/(float)m_nGridY*2.0f - 1.0f;
        //m_verts[n].z = 0.0f;

        if (pState->m_pp_codehandle) {
          // restore all the variables to their original states,
          //  run the user-defined equations,
          //  then move the results into local vars for computation as floats

          if (m_bScreenDependentRenderMode) {
            *pState->var_pv_x = (double)(m_verts[n].x * 0.5f + 0.5f);
            *pState->var_pv_y = (double)(m_verts[n].y * -0.5f + 0.5f);
          }
          else {
            *pState->var_pv_x = (double)(m_verts[n].x * 0.5f * m_fAspectX + 0.5f);
            *pState->var_pv_y = (double)(m_verts[n].y * -0.5f * m_fAspectY + 0.5f);
          }

          *pState->var_pv_rad = (double)m_vertinfo[n].rad;
          *pState->var_pv_ang = (double)m_vertinfo[n].ang;
          *pState->var_pv_zoom = *pState->var_pf_zoom;
          *pState->var_pv_zoomexp = *pState->var_pf_zoomexp;
          *pState->var_pv_rot = *pState->var_pf_rot;
          *pState->var_pv_warp = *pState->var_pf_warp;
          *pState->var_pv_cx = *pState->var_pf_cx;
          *pState->var_pv_cy = *pState->var_pf_cy;
          *pState->var_pv_dx = *pState->var_pf_dx;
          *pState->var_pv_dy = *pState->var_pf_dy;
          *pState->var_pv_sx = *pState->var_pf_sx;
          *pState->var_pv_sy = *pState->var_pf_sy;
          //*pState->var_pv_time		= *pState->var_pv_time;		// (these are all now initialized
          //*pState->var_pv_bass		= *pState->var_pv_bass;		//  just once per frame)
          //*pState->var_pv_mid		= *pState->var_pv_mid;
          //*pState->var_pv_treb		= *pState->var_pv_treb;
          //*pState->var_pv_bass_att	= *pState->var_pv_bass_att;
          //*pState->var_pv_mid_att	= *pState->var_pv_mid_att;
          //*pState->var_pv_treb_att	= *pState->var_pv_treb_att;

#ifndef _NO_EXPR_
          NSEEL_code_execute(pState->m_pp_codehandle);
#endif

          fZoom = (float)(*pState->var_pv_zoom);
          fZoomExp = (float)(*pState->var_pv_zoomexp);
          fRot = (float)(*pState->var_pv_rot);
          fWarp = (float)(*pState->var_pv_warp);
          fCX = (float)(*pState->var_pv_cx);
          fCY = (float)(*pState->var_pv_cy);
          fDX = (float)(*pState->var_pv_dx);
          fDY = (float)(*pState->var_pv_dy);
          fSX = (float)(*pState->var_pv_sx);
          fSY = (float)(*pState->var_pv_sy);
        }

        float fZoom2 = powf(fZoom, powf(fZoomExp, m_vertinfo[n].rad * 2.0f - 1.0f));

        // initial texcoords, w/built-in zoom factor
        float fZoom2Inv = 1.0f / fZoom2;

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

  }
}

void mdrop::Engine::WarpedBlit_NoShaders(int nPass, bool bAlphaBlend, bool bFlipAlpha, bool bCullTiles, bool bFlipCulling) {
  MungeFPCW(NULL);	// puts us in single-precision mode & disables exceptions

  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  if (!wcscmp(m_pState->m_szDesc, INVALID_PRESET_DESC)) {
    // if no valid preset loaded, clear the target to black, and return
    lpDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    return;
  }

  lpDevice->SetTexture(0, m_lpVS[0]);
  lpDevice->SetVertexShader(NULL);
  lpDevice->SetPixelShader(NULL);
  lpDevice->SetFVF(MYVERTEX_FORMAT);
  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

  // stages 0 and 1 always just use bilinear filtering.
  lpDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

  // note: this texture stage state setup works for 0 or 1 texture.
  // if you set a texture, it will be modulated with the current diffuse color.
  // if you don't set a texture, it will just use the current diffuse color.
  lpDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  lpDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  lpDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
  lpDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  lpDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  lpDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  DWORD texaddr = (*m_pState->var_pf_wrap > m_fSnapPoint) ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
  lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, texaddr);
  lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, texaddr);
  lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSW, texaddr);

  // decay
  float fDecay = (float)(*m_pState->var_pf_decay);

  //if (m_pState->m_bBlending)
  //	fDecay = fDecay*(fCosineBlend) + (1.0f-fCosineBlend)*((float)(*m_pOldState->var_pf_decay));

  if (m_n16BitGamma > 0 &&
    (GetBackBufFormat() == D3DFMT_R5G6B5 || GetBackBufFormat() == D3DFMT_X1R5G5B5 || GetBackBufFormat() == D3DFMT_A1R5G5B5 || GetBackBufFormat() == D3DFMT_A4R4G4B4) &&
    fDecay < 0.9999f) {
    fDecay = min(fDecay, (32.0f - m_n16BitGamma) / 32.0f);
  }

  D3DCOLOR cDecay = D3DCOLOR_RGBA_01(fDecay, fDecay, fDecay, 1);

  // hurl the triangle strips at the video card
  int poly;
  for (poly = 0; poly < (m_nGridX + 1) * 2; poly++)
    m_verts_temp[poly].Diffuse = cDecay;

  if (bAlphaBlend) {
    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    if (bFlipAlpha) {
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVSRCALPHA);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
    }
    else {
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
  }
  else
    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

  int nAlphaTestValue = 0;
  if (bFlipCulling)
    nAlphaTestValue = 1 - nAlphaTestValue;

  // Hurl the triangles at the video card.
  // We're going to un-index it, so that we don't stress any crappy (AHEM intel g33)
  //  drivers out there.
  // If we're blending, we'll skip any polygon that is all alpha-blended out.
  // This also respects the MaxPrimCount limit of the video card.
  MYVERTEX tempv[1024 * 3];
  int max_prims_per_batch = min(GetCaps()->MaxPrimitiveCount, (sizeof(tempv) / sizeof(tempv[0])) / 3) - 4;
  int primCount = m_nGridX * m_nGridY * 2;
  int src_idx = 0;
  int prims_sent = 0;
  while (src_idx < primCount * 3) {
    int prims_queued = 0;
    int i = 0;
    while (prims_queued < max_prims_per_batch && src_idx < primCount * 3) {
      // copy 3 verts
      for (int j = 0; j < 3; j++) {
        tempv[i++] = m_verts[m_indices_list[src_idx++]];
        // don't forget to flip sign on Y and factor in the decay color!:
        tempv[i - 1].y *= -1;
        tempv[i - 1].Diffuse = (cDecay & 0x00FFFFFF) | (tempv[i - 1].Diffuse & 0xFF000000);
      }
      if (bCullTiles) {
        DWORD d1 = (tempv[i - 3].Diffuse >> 24);
        DWORD d2 = (tempv[i - 2].Diffuse >> 24);
        DWORD d3 = (tempv[i - 1].Diffuse >> 24);
        bool bIsNeeded;
        if (nAlphaTestValue)
          bIsNeeded = ((d1 & d2 & d3) < 255);//(d1 < 255) || (d2 < 255) || (d3 < 255);
        else
          bIsNeeded = ((d1 | d2 | d3) > 0);//(d1 > 0) || (d2 > 0) || (d3 > 0);
        if (!bIsNeeded)
          i -= 3;
        else
          prims_queued++;
      }
      else
        prims_queued++;
    }
    if (prims_queued > 0)
      lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, prims_queued, tempv, sizeof(MYVERTEX));
  }

  /*
  if (!bCullTiles)
  {
      assert(!bAlphaBlend); //not handled yet

      // draw normally - just a full triangle strip for each half-row of cells
      // (even if we are blending, it is between two pre-pixel-shader presets,
      //  so the blend all happens exclusively in the per-vertex equations.)
    for (int strip=0; strip<m_nGridY*2; strip++)
    {
      int index = strip * (m_nGridX+2);

      for (poly=0; poly<m_nGridX+2; poly++)
      {
        int ref_vert = m_indices_strip[index];
        m_verts_temp[poly].x = m_verts[ref_vert].x;
        m_verts_temp[poly].y = -m_verts[ref_vert].y;
        m_verts_temp[poly].z = m_verts[ref_vert].z;
        m_verts_temp[poly].tu = m_verts[ref_vert].tu;
        m_verts_temp[poly].tv = m_verts[ref_vert].tv;
          //m_verts_temp[poly].Diffuse = cDecay;      this is done just once - see jsut above
        index++;
      }
          lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, m_nGridX, (void*)m_verts_temp, sizeof(MYVERTEX));
    }
  }
  else
  {
      //   we're blending to/from a new pixel-shader enabled preset;
      //   only draw the cells needed!  (an optimization)
      int nAlphaTestValue = 0;
      if (bFlipCulling)
          nAlphaTestValue = 1-nAlphaTestValue;

      int idx[2048];
    for (int y=0; y<m_nGridY; y++)
    {
          // copy verts & flip sign on Y
          int ref_vert = y*(m_nGridX+1);
      for (int i=0; i<(m_nGridX+1)*2; i++)
      {
        m_verts_temp[i].x  =  m_verts[ref_vert].x;
        m_verts_temp[i].y  = -m_verts[ref_vert].y;
        m_verts_temp[i].z  =  m_verts[ref_vert].z;
        m_verts_temp[i].tu =  m_verts[ref_vert].tu;
        m_verts_temp[i].tv =  m_verts[ref_vert].tv;
          m_verts_temp[i].Diffuse = (cDecay & 0x00FFFFFF) | (m_verts[ref_vert].Diffuse & 0xFF000000);
              ref_vert++;
          }

          // create (smart) indices
          int count = 0;
          int nVert = 0;
          bool bWasNeeded;
          ref_vert = (y)*(m_nGridX+1);
          DWORD d1 = (m_verts[ref_vert           ].Diffuse >> 24);
          DWORD d2 = (m_verts[ref_vert+m_nGridX+1].Diffuse >> 24);
          if (nAlphaTestValue)
              bWasNeeded = (d1 < 255) || (d2 < 255);
          else
              bWasNeeded = (d1 > 0) || (d2 > 0);
          for (i=0; i<m_nGridX; i++)
          {
              bool bIsNeeded;
              DWORD d1 = (m_verts[ref_vert+1           ].Diffuse >> 24);
              DWORD d2 = (m_verts[ref_vert+1+m_nGridX+1].Diffuse >> 24);
              if (nAlphaTestValue)
                  bIsNeeded = (d1 < 255) || (d2 < 255);
              else
                  bIsNeeded = (d1 > 0) || (d2 > 0);

              if (bIsNeeded || bWasNeeded)
              {
                  idx[count++] = nVert;
                  idx[count++] = nVert+1;
                  idx[count++] = nVert+m_nGridX+1;
                  idx[count++] = nVert+m_nGridX+1;
                  idx[count++] = nVert+1;
                  idx[count++] = nVert+m_nGridX+2;
              }
              bWasNeeded = bIsNeeded;

              nVert++;
              ref_vert++;
          }
          lpDevice->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (m_nGridX+1)*2, count/3, (void*)idx, D3DFMT_INDEX32, (void*)m_verts_temp, sizeof(MYVERTEX));
    }
  }/**/

  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void mdrop::Engine::WarpedBlit_Shaders(int nPass, bool bAlphaBlend, bool bFlipAlpha, bool bCullTiles, bool bFlipCulling) {
  // if nPass==0, it draws old preset (blending 1 of 2).
  // if nPass==1, it draws new preset (blending 2 of 2, OR done blending)

  MungeFPCW(NULL);	// puts us in single-precision mode & disables exceptions

  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  if (!wcscmp(m_pState->m_szDesc, INVALID_PRESET_DESC)) {
    // if no valid preset loaded, clear the target to black, and return
    lpDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    return;
  }

  //float fBlend = m_pState->m_fBlendProgress;//max(0,min(1,(m_pState->m_fBlendProgress*1.6f - 0.3f)));
  //if (nPassOverride==0)
  //    fBlend = 1-fBlend;  // <-- THIS IS THE KEY - FLIPS THE ALPHAS AND EVERYTHING ELSE JUST WORKS.
  //bool  bBlending = m_pState->m_bBlending;//(fBlend >= 0.0001f && fBlend <= 0.9999f);

//lpDevice->SetTexture(0, m_lpVS[0]);
  lpDevice->SetVertexShader(NULL);
  lpDevice->SetFVF(MYVERTEX_FORMAT);

  // texel alignment
  float texel_offset_x = 0.5f / (float)m_nTexSizeX;
  float texel_offset_y = 0.5f / (float)m_nTexSizeY;

  int nAlphaTestValue = 0;
  if (bFlipCulling)
    nAlphaTestValue = 1 - nAlphaTestValue;

  if (bAlphaBlend) {
    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    if (bFlipAlpha) {
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVSRCALPHA);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
    }
    else {
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
  }
  else
    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

  int pass = nPass;
  {
    // PASS 0: draw using *blended per-vertex motion vectors*, but with the OLD warp shader.
    // PASS 1: draw using *blended per-vertex motion vectors*, but with the NEW warp shader.
    PShaderInfo* si = (pass == 0) ? &m_OldShaders.warp : &m_shaders.warp;
    CState* state = (pass == 0) ? m_pOldState : m_pState;

    lpDevice->SetVertexDeclaration(m_pMyVertDecl);
    lpDevice->SetVertexShader(m_fallbackShaders_vs.warp.ptr);
    lpDevice->SetPixelShader(si->ptr);

    ApplyShaderParams(&(si->params), si->CT, state);

    // Hurl the triangles at the video card.
    // We're going to un-index it, so that we don't stress any crappy (AHEM intel g33)
    //  drivers out there.
    // We divide it into the two halves of the screen (top/bottom) so we can hack
    //  the 'ang' values along the angle-wrap seam, halfway through the draw.
    // If we're blending, we'll skip any polygon that is all alpha-blended out.
    // This also respects the MaxPrimCount limit of the video card.
    MYVERTEX tempv[1024 * 3];
    int max_prims_per_batch = min(GetCaps()->MaxPrimitiveCount, (sizeof(tempv) / sizeof(tempv[0])) / 3) - 4;
    for (int half = 0; half < 2; half++) {
      // hack / restore the ang values along the angle-wrap [0 <-> 2pi] seam...
      float new_ang = half ? 3.1415926535897932384626433832795f : -3.1415926535897932384626433832795f;
      int y_offset = (m_nGridY / 2) * (m_nGridX + 1);
      for (int x = 0; x < m_nGridX / 2; x++)
        m_verts[y_offset + x].ang = new_ang;

      // send half of the polys
      int primCount = m_nGridX * m_nGridY * 2 / 2;  // in this case, to draw HALF the polys
      int src_idx = 0;
      int src_idx_offset = half * primCount * 3;
      int prims_sent = 0;
      while (src_idx < primCount * 3) {
        int prims_queued = 0;
        int i = 0;
        while (prims_queued < max_prims_per_batch && src_idx < primCount * 3) {
          // copy 3 verts
          for (int j = 0; j < 3; j++)
            tempv[i++] = m_verts[m_indices_list[src_idx_offset + src_idx++]];
          if (bCullTiles) {
            DWORD d1 = (tempv[i - 3].Diffuse >> 24);
            DWORD d2 = (tempv[i - 2].Diffuse >> 24);
            DWORD d3 = (tempv[i - 1].Diffuse >> 24);
            bool bIsNeeded;
            if (nAlphaTestValue)
              bIsNeeded = ((d1 & d2 & d3) < 255);//(d1 < 255) || (d2 < 255) || (d3 < 255);
            else
              bIsNeeded = ((d1 | d2 | d3) > 0);//(d1 > 0) || (d2 > 0) || (d3 > 0);
            if (!bIsNeeded)
              i -= 3;
            else
              prims_queued++;
          }
          else
            prims_queued++;
        }
        if (prims_queued > 0)
          lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, prims_queued, tempv, sizeof(MYVERTEX));
      }
    }
  }

  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

  RestoreShaderParams();
}

void mdrop::Engine::DrawCustomShapes() {
  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  //lpDevice->SetTexture(0, m_lpVS[0]);//NULL);
  //lpDevice->SetVertexShader( SPRITEVERTEX_FORMAT );

  int num_reps = (m_pState->m_bBlending) ? 2 : 1;
  for (int rep = 0; rep < num_reps; rep++) {
    CState* pState = (rep == 0) ? m_pState : m_pOldState;
    float alpha_mult = 1;
    if (num_reps == 2)
      alpha_mult = (rep == 0) ? m_pState->m_fBlendProgress : (1 - m_pState->m_fBlendProgress);

    for (int i = 0; i < MAX_CUSTOM_SHAPES; i++) {
      if (pState->m_shape[i].enabled) {
        /*
        int bAdditive = 0;
        int nSides = 3;//3 + ((int)GetTime() % 8);
        int bThickOutline = 0;
        float x = 0.5f + 0.1f*cosf(GetTime()*0.8f+1);
        float y = 0.5f + 0.1f*sinf(GetTime()*0.8f+1);
        float rad = 0.15f + 0.07f*sinf(GetTime()*1.1f+3);
        float ang = GetTime()*1.5f;

        // inside colors
        float r = 1;
        float g = 0;
        float b = 0;
        float a = 0.4f;//0.1f + 0.1f*sinf(GetTime()*0.31f);

        // outside colors
        float r2 = 0;
        float g2 = 1;
        float b2 = 0;
        float a2 = 0;

        // border colors
        float border_r = 1;
        float border_g = 1;
        float border_b = 1;
        float border_a = 0.5f;
        */

        for (int instance = 0; instance < pState->m_shape[i].instances; instance++) {
          // 1. execute per-frame code
          LoadCustomShapePerFrameEvallibVars(pState, i, instance);

#ifndef _NO_EXPR_
          if (pState->m_shape[i].m_pf_codehandle) {
            NSEEL_code_execute(pState->m_shape[i].m_pf_codehandle);
          }
#endif

          // save changes to t1-t8 this frame
          /*
      pState->m_shape[i].t_values_after_init_code[0] = *pState->m_shape[i].var_pf_t1;
      pState->m_shape[i].t_values_after_init_code[1] = *pState->m_shape[i].var_pf_t2;
      pState->m_shape[i].t_values_after_init_code[2] = *pState->m_shape[i].var_pf_t3;
      pState->m_shape[i].t_values_after_init_code[3] = *pState->m_shape[i].var_pf_t4;
      pState->m_shape[i].t_values_after_init_code[4] = *pState->m_shape[i].var_pf_t5;
      pState->m_shape[i].t_values_after_init_code[5] = *pState->m_shape[i].var_pf_t6;
      pState->m_shape[i].t_values_after_init_code[6] = *pState->m_shape[i].var_pf_t7;
      pState->m_shape[i].t_values_after_init_code[7] = *pState->m_shape[i].var_pf_t8;
          */

          int sides = (int)(*pState->m_shape[i].var_pf_sides);
          if (sides < 3) sides = 3;
          if (sides > 100) sides = 100;

          lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
          lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
          lpDevice->SetRenderState(D3DRS_DESTBLEND, ((int)(*pState->m_shape[i].var_pf_additive) != 0) ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);

          SPRITEVERTEX v[512];  // for textured shapes (has texcoords)
          WFVERTEX v2[512];     // for untextured shapes + borders

          v[0].x = (float)(*pState->m_shape[i].var_pf_x * 2 - 1);// * ASPECT;
          v[0].y = (float)(*pState->m_shape[i].var_pf_y * 2 - 1);
          v[0].z = 0;
          v[0].tu = 0.5f;
          v[0].tv = 0.5f;
          v[0].Diffuse =
            ((((int)(*pState->m_shape[i].var_pf_a * 255 * alpha_mult)) & 0xFF) << 24) |
            ((((int)(*pState->m_shape[i].var_pf_r * 255)) & 0xFF) << 16) |
            ((((int)(*pState->m_shape[i].var_pf_g * 255)) & 0xFF) << 8) |
            ((((int)(*pState->m_shape[i].var_pf_b * 255)) & 0xFF));
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
              v[j].x = v[0].x + (float)*pState->m_shape[i].var_pf_rad * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_ang + 3.1415927f * 0.25f) * m_fAspectY;  // DON'T TOUCH!

            v[j].y = v[0].y - (float)*pState->m_shape[i].var_pf_rad * sinf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_ang + 3.1415927f * 0.25f);           // DON'T TOUCH!
            v[j].z = 0;

            if (m_bScreenDependentRenderMode)
              v[j].tu = 0.5f + 0.5f * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom);
            else
              v[j].tu = 0.5f + 0.5f * cosf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom) * m_fAspectY;  // DON'T TOUCH!
            v[j].tv = 0.5f + 0.5f * sinf(t * 3.1415927f * 2 + (float)*pState->m_shape[i].var_pf_tex_ang + 3.1415927f * 0.25f) / ((float)*pState->m_shape[i].var_pf_tex_zoom);  // DON'T TOUCH!

            v[j].Diffuse = v[1].Diffuse;
          }
          v[sides + 1] = v[1];

          if ((int)(*pState->m_shape[i].var_pf_textured) != 0) {
            // draw textured version
            lpDevice->SetTexture(0, m_lpVS[0]);
            lpDevice->SetVertexShader(NULL);
            lpDevice->SetFVF(SPRITEVERTEX_FORMAT);
            lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, sides, (void*)v, sizeof(SPRITEVERTEX));
          }
          else {
            // no texture
            for (int j = 0; j < sides + 2; j++) {
              v2[j].x = v[j].x;
              v2[j].y = v[j].y;
              v2[j].z = v[j].z;
              v2[j].Diffuse = v[j].Diffuse;
            }
            lpDevice->SetTexture(0, NULL);
            lpDevice->SetVertexShader(NULL);
            lpDevice->SetFVF(WFVERTEX_FORMAT);
            lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, sides, (void*)v2, sizeof(WFVERTEX));
          }

          // DRAW BORDER
          if (*pState->m_shape[i].var_pf_border_a > 0) {
            lpDevice->SetTexture(0, NULL);
            lpDevice->SetVertexShader(NULL);
            lpDevice->SetFVF(WFVERTEX_FORMAT);

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

            int its = ((int)(*pState->m_shape[i].var_pf_thick) != 0) ? 4 : 1;
            float x_inc = 2.0f / (float)m_nTexSizeX;
            float y_inc = 2.0f / (float)m_nTexSizeY;
            for (int it = 0; it < its; it++) {
              switch (it) {
              case 0: break;
              case 1: for (int j = 0; j < sides + 2; j++) v2[j].x += x_inc; break;		// draw fat dots
              case 2: for (int j = 0; j < sides + 2; j++) v2[j].y += y_inc; break;		// draw fat dots
              case 3: for (int j = 0; j < sides + 2; j++) v2[j].x -= x_inc; break;		// draw fat dots
              }
              lpDevice->DrawPrimitiveUP(D3DPT_LINESTRIP, sides, (void*)&v2[1], sizeof(WFVERTEX));
            }
          }

          lpDevice->SetTexture(0, m_lpVS[0]);
          lpDevice->SetVertexShader(NULL);
          lpDevice->SetFVF(SPRITEVERTEX_FORMAT);
        }
      }
    }
  }

  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

void mdrop::Engine::LoadCustomShapePerFrameEvallibVars(CState* pState, int i, int instance) {
  *pState->m_shape[i].var_pf_time = (double)(GetTime() - m_fStartTime);
  *pState->m_shape[i].var_pf_frame = (double)GetFrame();
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

void mdrop::Engine::LoadCustomWavePerFrameEvallibVars(CState* pState, int i) {
  *pState->m_wave[i].var_pf_time = (double)(GetTime() - m_fStartTime);
  *pState->m_wave[i].var_pf_frame = (double)GetFrame();
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

void mdrop::Engine::DrawCustomWaves() {
  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  lpDevice->SetTexture(0, NULL);
  lpDevice->SetVertexShader(NULL);
  lpDevice->SetFVF(WFVERTEX_FORMAT);

  // note: read in all sound data from mdrop::EngineShell's m_sound
  int num_reps = (m_pState->m_bBlending) ? 2 : 1;
  for (int rep = 0; rep < num_reps; rep++) {
    CState* pState = (rep == 0) ? m_pState : m_pOldState;
    float alpha_mult = 1;
    if (num_reps == 2)
      alpha_mult = (rep == 0) ? m_pState->m_fBlendProgress : (1 - m_pState->m_fBlendProgress);

    for (int i = 0; i < MAX_CUSTOM_WAVES; i++) {
      if (pState->m_wave[i].enabled) {
        int nSamples = pState->m_wave[i].samples;
        int max_samples = pState->m_wave[i].bSpectrum ? 512 : NUM_WAVEFORM_SAMPLES;
        if (nSamples > max_samples)
          nSamples = max_samples;
        nSamples -= pState->m_wave[i].sep;

        // 1. execute per-frame code
        LoadCustomWavePerFrameEvallibVars(pState, i);

        // 2.a. do just a once-per-frame init for the *per-point* *READ-ONLY* variables
        //     (the non-read-only ones will be reset/restored at the start of each vertex)
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

        NSEEL_code_execute(pState->m_wave[i].m_pf_codehandle);

        for (int vi = 0; vi < NUM_Q_VAR; vi++)
          *pState->m_wave[i].var_pp_q[vi] = *pState->m_wave[i].var_pf_q[vi];
        for (int vi = 0; vi < NUM_T_VAR; vi++)
          *pState->m_wave[i].var_pp_t[vi] = *pState->m_wave[i].var_pf_t[vi];

        nSamples = (int)*pState->m_wave[i].var_pf_samples;
        nSamples = min(512, nSamples);

        if ((nSamples >= 2) || (pState->m_wave[i].bUseDots && nSamples >= 1)) {
          int j;
          float tempdata[2][512];
          float mult = ((pState->m_wave[i].bSpectrum) ? 0.15f : 0.004f) * pState->m_wave[i].scaling * pState->m_fWaveScale.eval(-1);
          float* pdata1 = (pState->m_wave[i].bSpectrum) ? m_sound.fSpectrum[0] : m_sound.fWaveform[0];
          float* pdata2 = (pState->m_wave[i].bSpectrum) ? m_sound.fSpectrum[1] : m_sound.fWaveform[1];

          // initialize tempdata[2][512]
          int j0 = (pState->m_wave[i].bSpectrum) ? 0 : (max_samples - nSamples) / 2/**(1-pState->m_wave[i].bSpectrum)*/ - pState->m_wave[i].sep / 2;
          int j1 = (pState->m_wave[i].bSpectrum) ? 0 : (max_samples - nSamples) / 2/**(1-pState->m_wave[i].bSpectrum)*/ + pState->m_wave[i].sep / 2;
          float t = (pState->m_wave[i].bSpectrum) ? (max_samples - pState->m_wave[i].sep) / (float)nSamples : 1;
          float mix1 = powf(pState->m_wave[i].smoothing * 0.98f, 0.5f);  // lower exponent -> more default smoothing
          float mix2 = 1 - mix1;
          // SMOOTHING:
          tempdata[0][0] = pdata1[j0];
          tempdata[1][0] = pdata2[j1];
          for (int j = 1; j < nSamples; j++) {
            tempdata[0][j] = pdata1[(int)(j * t) + j0] * mix2 + tempdata[0][j - 1] * mix1;
            tempdata[1][j] = pdata2[(int)(j * t) + j1] * mix2 + tempdata[1][j - 1] * mix1;
          }
          // smooth again, backwards: [this fixes the asymmetry of the beginning & end..]
          for (j = nSamples - 2; j >= 0; j--) {
            tempdata[0][j] = tempdata[0][j] * mix2 + tempdata[0][j + 1] * mix1;
            tempdata[1][j] = tempdata[1][j] * mix2 + tempdata[1][j + 1] * mix1;
          }
          // finally, scale to final size:
          for (int j = 0; j < nSamples; j++) {
            tempdata[0][j] *= mult;
            tempdata[1][j] *= mult;
          }

          // 2. for each point, execute per-point code


          // to do:
          //  -add any of the m_wave[i].xxx menu-accessible vars to the code?
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
            NSEEL_code_execute(pState->m_wave[i].m_pp_codehandle);
#endif

            if (m_bScreenDependentRenderMode) {
              v[j].x = (float)(*pState->m_wave[i].var_pp_x * 2 - 1);
              v[j].y = (float)(*pState->m_wave[i].var_pp_y * -2 + 1);
            }
            else {
              v[j].x = (float)(*pState->m_wave[i].var_pp_x * 2 - 1) * m_fInvAspectX;
              v[j].y = (float)(*pState->m_wave[i].var_pp_y * -2 + 1) * m_fInvAspectY;
            }

            v[j].z = 0;
            v[j].Diffuse =
              ((((int)(*pState->m_wave[i].var_pp_a * 255 * alpha_mult)) & 0xFF) << 24) |
              ((((int)(*pState->m_wave[i].var_pp_r * 255)) & 0xFF) << 16) |
              ((((int)(*pState->m_wave[i].var_pp_g * 255)) & 0xFF) << 8) |
              ((((int)(*pState->m_wave[i].var_pp_b * 255)) & 0xFF));
          }



          // save changes to t1-t8 this frame
          /*
      pState->m_wave[i].t_values_after_init_code[0] = *pState->m_wave[i].var_pp_t1;
      pState->m_wave[i].t_values_after_init_code[1] = *pState->m_wave[i].var_pp_t2;
      pState->m_wave[i].t_values_after_init_code[2] = *pState->m_wave[i].var_pp_t3;
      pState->m_wave[i].t_values_after_init_code[3] = *pState->m_wave[i].var_pp_t4;
      pState->m_wave[i].t_values_after_init_code[4] = *pState->m_wave[i].var_pp_t5;
      pState->m_wave[i].t_values_after_init_code[5] = *pState->m_wave[i].var_pp_t6;
      pState->m_wave[i].t_values_after_init_code[6] = *pState->m_wave[i].var_pp_t7;
      pState->m_wave[i].t_values_after_init_code[7] = *pState->m_wave[i].var_pp_t8;
          */

          // 3. smooth it
          WFVERTEX v2[2048];
          WFVERTEX* pVerts = v;
          if (!pState->m_wave[i].bUseDots) {
            nSamples = SmoothWave(v, nSamples, v2);
            pVerts = v2;
          }

          // 4. draw it
          lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
          lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
          lpDevice->SetRenderState(D3DRS_DESTBLEND, pState->m_wave[i].bAdditive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);

          float ptsize = (float)((m_nTexSizeX >= 1024) ? 2 : 1) + (pState->m_wave[i].bDrawThick ? 1 : 0);
          if (pState->m_wave[i].bUseDots)
            lpDevice->SetRenderState(D3DRS_POINTSIZE, *((DWORD*)&ptsize));

          int its = (pState->m_wave[i].bDrawThick && !pState->m_wave[i].bUseDots) ? 4 : 1;
          float x_inc = 2.0f / (float)m_nTexSizeX;
          float y_inc = 2.0f / (float)m_nTexSizeY;
          for (int it = 0; it < its; it++) {
            switch (it) {
            case 0: break;
            case 1: for (int j = 0; j < nSamples; j++) pVerts[j].x += x_inc; break;		// draw fat dots
            case 2: for (int j = 0; j < nSamples; j++) pVerts[j].y += y_inc; break;		// draw fat dots
            case 3: for (int j = 0; j < nSamples; j++) pVerts[j].x -= x_inc; break;		// draw fat dots
            }
            lpDevice->DrawPrimitiveUP(pState->m_wave[i].bUseDots ? D3DPT_POINTLIST : D3DPT_LINESTRIP, nSamples - (pState->m_wave[i].bUseDots ? 0 : 1), (void*)pVerts, sizeof(WFVERTEX));
          }

          ptsize = 1.0f;
          if (pState->m_wave[i].bUseDots)
            lpDevice->SetRenderState(D3DRS_POINTSIZE, *((DWORD*)&ptsize));
        }
      }
    }
  }

  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

void mdrop::Engine::DrawWave(float* fL, float* fR) {
  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  lpDevice->SetTexture(0, NULL);
  lpDevice->SetVertexShader(NULL);
  lpDevice->SetFVF(WFVERTEX_FORMAT);

  WFVERTEX v1[576 + 1], v2[576 + 1];

  /*
m_lpD3DDev->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD); //D3DSHADE_FLAT
m_lpD3DDev->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, FALSE);
m_lpD3DDev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
if (m_D3DDevDesc.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_DITHER)
  m_lpD3DDev->SetRenderState(D3DRENDERSTATE_DITHERENABLE, TRUE);
m_lpD3DDev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
m_lpD3DDev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
m_lpD3DDev->SetRenderState(D3DRENDERSTATE_COLORVERTEX, TRUE);
m_lpD3DDev->SetRenderState(D3DRENDERSTATE_FILLMODE, D3DFILL_WIREFRAME);  // vs. SOLID
m_lpD3DDev->SetRenderState(D3DRENDERSTATE_AMBIENT, D3DCOLOR_RGBA_01(1,1,1,1));

hr = m_lpD3DDev->SetTexture(0, NULL);
if (hr != D3D_OK)
{
  //dumpmsg("Draw(): ERROR: SetTexture");
  //IdentifyD3DError(hr);
}
  */

  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  lpDevice->SetRenderState(D3DRS_DESTBLEND, (*m_pState->var_pf_wave_additive) ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);

  //float cr = m_pState->m_waveR.eval(GetTime());
  //float cg = m_pState->m_waveG.eval(GetTime());
  //float cb = m_pState->m_waveB.eval(GetTime());
  float cr = (float)(*m_pState->var_pf_wave_r);
  float cg = (float)(*m_pState->var_pf_wave_g);
  float cb = (float)(*m_pState->var_pf_wave_b);
  float cx = (float)(*m_pState->var_pf_wave_x);
  float cy = (float)(*m_pState->var_pf_wave_y); // note: it was backwards (top==1) in the original milkdrop, so we keep it that way!
  float fWaveParam = (float)(*m_pState->var_pf_wave_mystery);

  /*if (m_pState->m_bBlending)
  {
    cr = cr*(m_pState->m_fBlendProgress) + (1.0f-m_pState->m_fBlendProgress)*((float)(*m_pOldState->var_pf_wave_r));
    cg = cg*(m_pState->m_fBlendProgress) + (1.0f-m_pState->m_fBlendProgress)*((float)(*m_pOldState->var_pf_wave_g));
    cb = cb*(m_pState->m_fBlendProgress) + (1.0f-m_pState->m_fBlendProgress)*((float)(*m_pOldState->var_pf_wave_b));
    cx = cx*(m_pState->m_fBlendProgress) + (1.0f-m_pState->m_fBlendProgress)*((float)(*m_pOldState->var_pf_wave_x));
    cy = cy*(m_pState->m_fBlendProgress) + (1.0f-m_pState->m_fBlendProgress)*((float)(*m_pOldState->var_pf_wave_y));
    fWaveParam = fWaveParam*(m_pState->m_fBlendProgress) + (1.0f-m_pState->m_fBlendProgress)*((float)(*m_pOldState->var_pf_wave_mystery));
  }*/

  if (cr < 0) cr = 0;
  if (cg < 0) cg = 0;
  if (cb < 0) cb = 0;
  if (cr > 1) cr = 1;
  if (cg > 1) cg = 1;
  if (cb > 1) cb = 1;

  // maximize color:
  if (*m_pState->var_pf_wave_brighten) {
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

  float fWavePosX = cx * 2.0f - 1.0f; // go from 0..1 user-range to -1..1 D3D range
  float fWavePosY = cy * 2.0f - 1.0f;

  float bass_rel = mysound.imm[0];
  float mid_rel = mysound.imm[1];
  float treble_rel = mysound.imm[2];

  int sample_offset = 0;
  int new_wavemode = (int)(*m_pState->var_pf_wave_mode) % NUM_WAVES;  // since it can be changed from per-frame code!

  int its = (m_pState->m_bBlending && (new_wavemode != m_pState->m_nOldWaveMode)) ? 2 : 1;
  int nVerts1 = 0;
  int nVerts2 = 0;
  int nBreak1 = -1;
  int nBreak2 = -1;
  float alpha1, alpha2;

  for (int it = 0; it < its; it++) {
    int   wave = (it == 0) ? new_wavemode : m_pState->m_nOldWaveMode;
    int   nVerts = NUM_WAVEFORM_SAMPLES;		// allowed to peek ahead 64 (i.e. left is [i], right is [i+64])
    int   nBreak = -1;

    float fWaveParam2 = fWaveParam;
    //std::string fWaveParam; // kill its scope
    if ((wave == 0 || wave == 1 || wave == 4) && (fWaveParam2 < -1 || fWaveParam2 > 1)) {
      //fWaveParam2 = max(fWaveParam2, -1.0f);
      //fWaveParam2 = min(fWaveParam2,  1.0f);
      fWaveParam2 = fWaveParam2 * 0.5f + 0.5f;
      fWaveParam2 -= floorf(fWaveParam2);
      fWaveParam2 = fabsf(fWaveParam2);
      fWaveParam2 = fWaveParam2 * 2 - 1;
    }

    WFVERTEX* v = (it == 0) ? v1 : v2;
    ZeroMemory(v, sizeof(WFVERTEX) * nVerts);

    float alpha = (float)(*m_pState->var_pf_wave_a);//m_pState->m_fWaveAlpha.eval(GetTime());

    switch (wave) {
    case 0:
      // circular wave

      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts * 12/10);	// only call this once nVerts is final!

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

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
          }
          else {
            v[i].x = rad * cosf(ang) * m_fAspectY + fWavePosX;		// 0.75 = adj. for aspect ratio
            v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY;
          }
          //v[i].Diffuse = color;
        }
      }

      // dupe last vertex to connect the lines; skip if blending
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }

      break;

    case 1:
      // x-y osc. that goes around in a spiral, in time

      alpha *= 1.25f;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      nVerts /= 2;

      for (int i = 0; i < nVerts; i++) {
        float rad = 0.53f + 0.43f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 1.57f + GetTime() * 2.3f;

        if (m_bScreenDependentRenderMode) {
          v[i].x = rad * cosf(ang) + fWavePosX;
          v[i].y = rad * sinf(ang) + fWavePosY;
        }
        else {
          v[i].x = rad * cosf(ang) * m_fAspectY + fWavePosX;		// 0.75 = adj. for aspect ratio
          v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY;
        }
        //v[i].Diffuse = color;//(D3DCOLOR_RGBA_01(cr, cg, cb, alpha*min(1, max(0, fL[i])));
      }

      break;

    case 2:
      // centered spiro (alpha constant)
      //	 aimed at not being so sound-responsive, but being very "nebula-like"
      //   difference is that alpha is constant (and faint), and waves a scaled way up

      switch (m_nTexSizeX) {
      case 256:  alpha *= 0.07f; break;
      case 512:  alpha *= 0.09f; break;
      case 1024: alpha *= 0.11f; break;
      case 2048: alpha *= 0.13f; break;
      }

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      for (int i = 0; i < nVerts; i++) {
        if (m_bScreenDependentRenderMode) {
          v[i].x = fR[i] + fWavePosX;//((pR[i] ^ 128) - 128)/90.0f * ASPECT; // 0.75 = adj. for aspect ratio
          v[i].y = fL[i + 32] + fWavePosY;//((pL[i+32] ^ 128) - 128)/90.0f;
        }
        else {
          v[i].x = fR[i] * m_fAspectY + fWavePosX;//((pR[i] ^ 128) - 128)/90.0f * ASPECT; // 0.75 = adj. for aspect ratio
          v[i].y = fL[i + 32] * m_fAspectX + fWavePosY;//((pL[i+32] ^ 128) - 128)/90.0f;Add commentMore actions
        }
        //v[i].Diffuse = color;
      }

      break;
    case 3:
      // centered spiro (alpha tied to volume)
      //	 aimed at having a strong audio-visual tie-in
      //   colors are always bright (no darks)

      switch (m_nTexSizeX) {
      case 256:  alpha = 0.075f; break;
      case 512:  alpha = 0.150f; break;
      case 1024: alpha = 0.220f; break;
      case 2048: alpha = 0.330f; break;
      }

      alpha *= 1.3f;
      alpha *= powf(treble_rel, 2.0f);
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      for (int i = 0; i < nVerts; i++) {
        if (m_bScreenDependentRenderMode) {
          v[i].x = fR[i] + fWavePosX;//((pR[i] ^ 128) - 128)/90.0f * ASPECT; // 0.75 = adj. for aspect ratio
          v[i].y = fL[i + 32] + fWavePosY;//((pL[i+32] ^ 128) - 128)/90.0f;
        }
        else {
          v[i].x = fR[i] * m_fAspectY + fWavePosX;//((pR[i] ^ 128) - 128)/90.0f * ASPECT; // 0.75 = adj. for aspect ratio
          v[i].y = fL[i + 32] * m_fAspectX + fWavePosY;//((pL[i+32] ^ 128) - 128)/90.0f;Add commentMore actions
        }
        //v[i].Diffuse = color;
      }
      break;
    case 4:
      // horizontal "script", left channel

      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;

      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts + 25);	// only call this once nVerts is final!

      /*
      if (treble_rel > treb_thresh_for_wave6)
      {
        //alpha = 1.0f;
        treb_thresh_for_wave6 = treble_rel * 1.025f;
      }
      else
      {
        alpha *= 0.2f;
        treb_thresh_for_wave6 *= 0.996f;		// fixme: make this fps-independent
      }
      */

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      {
        float w1 = 0.45f + 0.5f * (fWaveParam2 * 0.5f + 0.5f);		// 0.1 - 0.9
        float w2 = 1.0f - w1;

        float inv_nverts = 1.0f / (float)(nVerts);

        for (int i = 0; i < nVerts; i++) {
          v[i].x = -1.0f + 2.0f * (i * inv_nverts) + fWavePosX;
          v[i].y = fL[i + sample_offset] * 0.47f + fWavePosY;//((pL[i] ^ 128) - 128)/270.0f;
          v[i].x += fR[i + 25 + sample_offset] * 0.44f;//((pR[i+25] ^ 128) - 128)/290.0f;
          //v[i].Diffuse = color;

          // momentum
          if (i > 1) {
            v[i].x = v[i].x * w2 + w1 * (v[i - 1].x * 2.0f - v[i - 2].x);
            v[i].y = v[i].y * w2 + w1 * (v[i - 1].y * 2.0f - v[i - 2].y);
          }
        }

        /*
        // center on Y
        float avg_y = 0;
        for (i=0; i<nVerts; i++)
          avg_y += v[i].y;
        avg_y /= (float)nVerts;
        avg_y *= 0.5f;		// damp the movement
        for (i=0; i<nVerts; i++)
          v[i].y -= avg_y;
        */
      }

      break;

    case 5:
      // weird explosive complex # thingy

      switch (m_nTexSizeX) {
      case 256:  alpha *= 0.07f; break;
      case 512:  alpha *= 0.09f; break;
      case 1024: alpha *= 0.11f; break;
      case 2048: alpha *= 0.13f; break;
      }

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      {
        float cos_rot = cosf(GetTime() * 0.3f);
        float sin_rot = sinf(GetTime() * 0.3f);

        for (int i = 0; i < nVerts; i++) {
          float x0 = (fR[i] * fL[i + 32] + fL[i] * fR[i + 32]);
          float y0 = (fR[i] * fR[i] - fL[i + 32] * fL[i + 32]);

          if (m_bScreenDependentRenderMode) {
            v[i].x = (x0 * cos_rot - y0 * sin_rot) + fWavePosX;
            v[i].y = (x0 * sin_rot + y0 * cos_rot) + fWavePosY;
          }
          else {
            v[i].x = (x0 * cos_rot - y0 * sin_rot) * m_fAspectY + fWavePosX;
            v[i].y = (x0 * sin_rot + y0 * cos_rot) * m_fAspectX + fWavePosY;
          }
          //v[i].Diffuse = color;
        }
      }

      break;

    case 6:
    case 7:
    case 8:
      // 6: angle-adjustable left channel, with temporal wave alignment;
      //   fWaveParam2 controls the angle at which it's drawn
      //	 fWavePosX slides the wave away from the center, transversely.
      //   fWavePosY does nothing
      //
      // 7: same, except there are two channels shown, and
      //   fWavePosY determines the separation distance.
      //
      // 8: same as 6, except using the spectrum analyzer (UNFINISHED)
      //
      nVerts /= 2;

      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;

      if (wave == 8)
        nVerts = 256;
      else
        sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts);	// only call this once nVerts is final!

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      {
        float ang = 1.57f * fWaveParam2;	// from -PI/2 to PI/2
        float dx = cosf(ang);
        float dy = sinf(ang);

        float edge_x[2], edge_y[2];

        //edge_x[0] = fWavePosX - dx*3.0f;
        //edge_y[0] = fWavePosY - dy*3.0f;
        //edge_x[1] = fWavePosX + dx*3.0f;
        //edge_y[1] = fWavePosY + dy*3.0f;
        edge_x[0] = fWavePosX * cosf(ang + 1.57f) - dx * 3.0f;
        edge_y[0] = fWavePosX * sinf(ang + 1.57f) - dy * 3.0f;
        edge_x[1] = fWavePosX * cosf(ang + 1.57f) + dx * 3.0f;
        edge_y[1] = fWavePosX * sinf(ang + 1.57f) + dy * 3.0f;

        for (int i = 0; i < 2; i++)	// for each point defining the line
        {
          // clip the point against 4 edges of screen
          // be a bit lenient (use +/-1.1 instead of +/-1.0)
          //	 so the dual-wave doesn't end too soon, after the channels are moved apart
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;

            switch (j) {
            case 0:
              if (edge_x[i] > 1.1f) {
                t = (1.1f - edge_x[1 - i]) / (edge_x[i] - edge_x[1 - i]);
                bClip = true;
              }
              break;
            case 1:
              if (edge_x[i] < -1.1f) {
                t = (-1.1f - edge_x[1 - i]) / (edge_x[i] - edge_x[1 - i]);
                bClip = true;
              }
              break;
            case 2:
              if (edge_y[i] > 1.1f) {
                t = (1.1f - edge_y[1 - i]) / (edge_y[i] - edge_y[1 - i]);
                bClip = true;
              }
              break;
            case 3:
              if (edge_y[i] < -1.1f) {
                t = (-1.1f - edge_y[1 - i]) / (edge_y[i] - edge_y[1 - i]);
                bClip = true;
              }
              break;
            }

            if (bClip) {
              float dx = edge_x[i] - edge_x[1 - i];
              float dy = edge_y[i] - edge_y[1 - i];
              edge_x[i] = edge_x[1 - i] + dx * t;
              edge_y[i] = edge_y[1 - i] + dy * t;
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
            //v[i].Diffuse = color;
          }
        else if (wave == 8)
          //256 verts
          for (int i = 0; i < nVerts; i++) {
            float f = 0.1f * logf(mysound.fSpecLeft[i * 2] + mysound.fSpecLeft[i * 2 + 1]);
            v[i].x = edge_x[0] + dx * i + perp_dx * f;
            v[i].y = edge_y[0] + dy * i + perp_dy * f;
            //v[i].Diffuse = color;
          }
        else {
          float sep = powf(fWavePosY * 0.5f + 0.5f, 2.0f);
          for (int i = 0; i < nVerts; i++) {
            v[i].x = edge_x[0] + dx * i + perp_dx * (0.25f * fL[i + sample_offset] + sep);
            v[i].y = edge_y[0] + dy * i + perp_dy * (0.25f * fL[i + sample_offset] + sep);
            //v[i].Diffuse = color;
          }

          //D3DPRIMITIVETYPE primtype = (*m_pState->var_pf_wave_usedots) ? D3DPT_POINTLIST : D3DPT_LINESTRIP;
          //m_lpD3DDev->DrawPrimitive(primtype, D3DFVF_LVERTEX, (LPVOID)v, nVerts, NULL);

          for (int i = 0; i < nVerts; i++) {
            v[i + nVerts].x = edge_x[0] + dx * i + perp_dx * (0.25f * fR[i + sample_offset] - sep);
            v[i + nVerts].y = edge_y[0] + dy * i + perp_dy * (0.25f * fR[i + sample_offset] - sep);
            //v[i+nVerts].Diffuse = color;
          }

          nBreak = nVerts;
          nVerts *= 2;
        }
      }

      break;
    case 9:
      // large wave
      nVerts /= 2;

      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;

      if (wave == 8)
        nVerts = 256;
      else
        sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts);	// only call this once nVerts is final!

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      {
        float ang = 1.57f * fWaveParam2;	// from -PI/2 to PI/2
        float dx = cosf(ang);
        float dy = sinf(ang);

        float edge_x[2], edge_y[2];

        //edge_x[0] = fWavePosX - dx*3.0f;
        //edge_y[0] = fWavePosY - dy*3.0f;
        //edge_x[1] = fWavePosX + dx*3.0f;
        //edge_y[1] = fWavePosY + dy*3.0f;
        edge_x[0] = fWavePosX * cosf(ang + 1.57f) - dx * 3.0f;
        edge_y[0] = fWavePosX * sinf(ang + 1.57f) - dy * 3.0f;
        edge_x[1] = fWavePosX * cosf(ang + 1.57f) + dx * 3.0f;
        edge_y[1] = fWavePosX * sinf(ang + 1.57f) + dy * 3.0f;

        for (int i = 0; i < 2; i++)	// for each point defining the line
        {
          // clip the point against 4 edges of screen
          // be a bit lenient (use +/-1.1 instead of +/-1.0)
          //	 so the dual-wave doesn't end too soon, after the channels are moved apart
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;

            switch (j) {
            case 0:
              if (edge_x[i] > 1.1f) {
                t = (1.1f - edge_x[1 - i]) / (edge_x[i] - edge_x[1 - i]);
                bClip = true;
              }
              break;
            case 1:
              if (edge_x[i] < -1.1f) {
                t = (-1.1f - edge_x[1 - i]) / (edge_x[i] - edge_x[1 - i]);
                bClip = true;
              }
              break;
            case 2:
              if (edge_y[i] > 1.1f) {
                t = (1.1f - edge_y[1 - i]) / (edge_y[i] - edge_y[1 - i]);
                bClip = true;
              }
              break;
            case 3:
              if (edge_y[i] < -1.1f) {
                t = (-1.1f - edge_y[1 - i]) / (edge_y[i] - edge_y[1 - i]);
                bClip = true;
              }
              break;
            }

            if (bClip) {
              float dx = edge_x[i] - edge_x[1 - i];
              float dy = edge_y[i] - edge_y[1 - i];
              edge_x[i] = edge_x[1 - i] + dx * t;
              edge_y[i] = edge_y[1 - i] + dy * t;
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
          //v[i].Diffuse = color;
        }



        nBreak = nVerts;
        nVerts *= 2;
      }


      break;



    case 10:
      // x wave (It should be called X marks the spot, isn't it?)
      nVerts /= 2;

      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;

      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts);	// only call this once nVerts is final!

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      {
        float ang = -0.75f + fWaveParam2 * 3.15f;	// from -PI/2 to PI/2
        float dx = cosf(ang);
        float dy = sinf(ang);

        float edge_x[2], edge_y[2];

        //edge_x[0] = fWavePosX - dx*3.0f;
        //edge_y[0] = fWavePosY - dy*3.0f;
        //edge_x[1] = fWavePosX + dx*3.0f;
        //edge_y[1] = fWavePosY + dy*3.0f;
        edge_x[0] = fWavePosX * cosf(ang + 1.57f) - dx * 3.0f;
        edge_y[0] = fWavePosX * sinf(ang + 1.57f) - dy * 3.0f;
        edge_x[1] = fWavePosX * cosf(ang + 1.57f) + dx * 3.0f;
        edge_y[1] = fWavePosX * sinf(ang + 1.57f) + dy * 3.0f;

        for (int i = 0; i < 2; i++)	// for each point defining the line
        {
          // clip the point against 4 edges of screen
          // be a bit lenient (use +/-1.1 instead of +/-1.0)
          //	 so the dual-wave doesn't end too soon, after the channels are moved apart
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;

            switch (j) {
            case 0:
              if (edge_x[i] > 1.1f) {
                t = (1.1f - edge_x[1 - i]) / (edge_x[i] - edge_x[1 - i]);
                bClip = true;
              }
              break;
            case 1:
              if (edge_x[i] < -1.1f) {
                t = (-1.1f - edge_x[1 - i]) / (edge_x[i] - edge_x[1 - i]);
                bClip = true;
              }
              break;
            case 2:
              if (edge_y[i] > 1.1f) {
                t = (1.1f - edge_y[1 - i]) / (edge_y[i] - edge_y[1 - i]);
                bClip = true;
              }
              break;
            case 3:
              if (edge_y[i] < -1.1f) {
                t = (-1.1f - edge_y[1 - i]) / (edge_y[i] - edge_y[1 - i]);
                bClip = true;
              }
              break;
            }

            if (bClip) {
              float dx = edge_x[i] - edge_x[1 - i];
              float dy = edge_y[i] - edge_y[1 - i];
              edge_x[i] = edge_x[1 - i] + dx * t;
              edge_y[i] = edge_y[1 - i] + dy * t;
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
          //v[i].Diffuse = color;
        }


        ////////////////\\\\\\\\\\\\\\\\\\\\\

        float ang3 = 0.75f + fWaveParam2 * 3.15f;	// from -PI/2 to PI/2
        float dx3 = cosf(ang3);
        float dy3 = sinf(ang3);

        float edge_x3[2], edge_y3[2];

        edge_x3[0] = fWavePosX * cosf(ang3 + 1.57f) - dx3 * 3.0f;
        edge_y3[0] = fWavePosX * sinf(ang3 + 1.57f) - dy3 * 3.0f;
        edge_x3[1] = fWavePosX * cosf(ang3 + 1.57f) + dx3 * 3.0f;
        edge_y3[1] = fWavePosX * sinf(ang3 + 1.57f) + dy3 * 3.0f;

        for (int i = 0; i < 2; i++)	// for each point defining the line
        {
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;

            switch (j) {
            case 0:
              if (edge_x3[i] > 1.1f) {
                t = (1.1f - edge_x3[1 - i]) / (edge_x3[i] - edge_x3[1 - i]);
                bClip = true;
              }
              break;
            case 1:
              if (edge_x3[i] < -1.1f) {
                t = (-1.1f - edge_x3[1 - i]) / (edge_x3[i] - edge_x3[1 - i]);
                bClip = true;
              }
              break;
            case 2:
              if (edge_y3[i] > 1.1f) {
                t = (1.1f - edge_y3[1 - i]) / (edge_y3[i] - edge_y3[1 - i]);
                bClip = true;
              }
              break;
            case 3:
              if (edge_y3[i] < -1.1f) {
                t = (-1.1f - edge_y3[1 - i]) / (edge_y3[i] - edge_y3[1 - i]);
                bClip = true;
              }
              break;
            }

            if (bClip) {
              float dx3 = edge_x3[i] - edge_x3[1 - i];
              float dy3 = edge_y3[i] - edge_y3[1 - i];
              edge_x3[i] = edge_x3[1 - i] + dx3 * t;
              edge_y3[i] = edge_y3[1 - i] + dy3 * t;
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
          //v[i+nVerts].Diffuse = color;
        }



        nBreak = nVerts;
        nVerts *= 2;
      }


      break;


    case 11:
      // vertical dual wave
      nVerts /= 2;

      if (nVerts > m_nTexSizeX / 3)
        nVerts = m_nTexSizeX / 3;

      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts);	// only call this once nVerts is final!

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      {
        float ang = 1.57f;	// from -PI/2 to PI/2
        float dx = cosf(ang);
        float dy = sinf(ang);

        float edge_x[2], edge_y[2];

        //edge_x[0] = fWavePosX - dx*3.0f;
        //edge_y[0] = fWavePosY - dy*3.0f;
        //edge_x[1] = fWavePosX + dx*3.0f;
        //edge_y[1] = fWavePosY + dy*3.0f;
        edge_x[0] = fWavePosX * cosf(ang + 1.57f) - dx * 3.0f;
        edge_y[0] = fWavePosX * sinf(ang + 1.57f) - dy * 3.0f;
        edge_x[1] = fWavePosX * cosf(ang + 1.57f) + dx * 3.0f;
        edge_y[1] = fWavePosX * sinf(ang + 1.57f) + dy * 3.0f;

        for (int i = 0; i < 2; i++)	// for each point defining the line
        {
          // clip the point against 4 edges of screen
          // be a bit lenient (use +/-1.1 instead of +/-1.0)
          //	 so the dual-wave doesn't end too soon, after the channels are moved apart
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;

            switch (j) {
            case 0:
              if (edge_x[i] > 1.1f) {
                t = (1.1f - edge_x[1 - i]) / (edge_x[i] - edge_x[1 - i]);
                bClip = true;
              }
              break;
            case 1:
              if (edge_x[i] < -1.1f) {
                t = (-1.1f - edge_x[1 - i]) / (edge_x[i] - edge_x[1 - i]);
                bClip = true;
              }
              break;
            case 2:
              if (edge_y[i] > 1.1f) {
                t = (1.1f - edge_y[1 - i]) / (edge_y[i] - edge_y[1 - i]);
                bClip = true;
              }
              break;
            case 3:
              if (edge_y[i] < -1.1f) {
                t = (-1.1f - edge_y[1 - i]) / (edge_y[i] - edge_y[1 - i]);
                bClip = true;
              }
              break;
            }

            if (bClip) {
              float dx = edge_x[i] - edge_x[1 - i];
              float dy = edge_y[i] - edge_y[1 - i];
              edge_x[i] = edge_x[1 - i] + dx * t;
              edge_y[i] = edge_y[1 - i] + dy * t;
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
          //v[i].Diffuse = color;
        }


        ////////////////\\\\\\\\\\\\\\\\\\\\\


        float ang3 = 1.57f;	// from -PI/2 to PI/2
        float dx3 = cosf(ang3);
        float dy3 = sinf(ang3);

        float edge_x3[2], edge_y3[2];

        edge_x3[0] = fWavePosX * cosf(ang3 + 1.57f) - dx3 * 3.0f;
        edge_y3[0] = fWavePosX * sinf(ang3 + 1.57f) - dy3 * 3.0f;
        edge_x3[1] = fWavePosX * cosf(ang3 + 1.57f) + dx3 * 3.0f;
        edge_y3[1] = fWavePosX * sinf(ang3 + 1.57f) + dy3 * 3.0f;

        for (int i = 0; i < 2; i++)	// for each point defining the line
        {
          for (int j = 0; j < 4; j++) {
            float t;
            bool bClip = false;

            switch (j) {
            case 0:
              if (edge_x3[i] > 1.1f) {
                t = (1.1f - edge_x3[1 - i]) / (edge_x3[i] - edge_x3[1 - i]);
                bClip = true;
              }
              break;
            case 1:
              if (edge_x3[i] < -1.1f) {
                t = (-1.1f - edge_x3[1 - i]) / (edge_x3[i] - edge_x3[1 - i]);
                bClip = true;
              }
              break;
            case 2:
              if (edge_y3[i] > 1.1f) {
                t = (1.1f - edge_y3[1 - i]) / (edge_y3[i] - edge_y3[1 - i]);
                bClip = true;
              }
              break;
            case 3:
              if (edge_y3[i] < -1.1f) {
                t = (-1.1f - edge_y3[1 - i]) / (edge_y3[i] - edge_y3[1 - i]);
                bClip = true;
              }
              break;
            }

            if (bClip) {
              float dx3 = edge_x3[i] - edge_x3[1 - i];
              float dy3 = edge_y3[i] - edge_y3[1 - i];
              edge_x3[i] = edge_x3[1 - i] + dx3 * t;
              edge_y3[i] = edge_y3[1 - i] + dy3 * t;
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
          //v[i+nVerts].Diffuse = color;
        }

        ////////////////\\\\\\\\\\\\\\\\\\\\\


        nBreak = nVerts;
        nVerts *= 2;
      }


      break;


    case 12:
      // x-y osc. that goes around in a spiral, in time, but skewed

      alpha *= 1.25f;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      nVerts /= 2;
      for (int i = 0; i < nVerts; i++) {
        //float rad = 0.13f + 0.43f * fR[i] + fWaveParam2 + perp_dy3/2 ;
        //float ang = fL[i + 32] * 3.57f + GetTime() * perp_dx3/2;
        //v[i].x = rad * cosf(ang+ alpha) * m_fAspectY + fWavePosX ;		// 0.75 = adj. for aspect ratio
        //v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY ;
        //v[i].Diffuse = color;//(D3DCOLOR_RGBA_01(cr, cg, cb, alpha*min(1, max(0, fL[i])));
        float rad = 0.63f + 0.23f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 0.9f + GetTime() * 3.3f;

        if (m_bScreenDependentRenderMode) {
          v[i].x = rad * cosf(ang + alpha) + fWavePosX;
          v[i].y = rad * sinf(ang) + fWavePosY;
        }
        else {
          v[i].x = rad * cosf(ang + alpha) * m_fAspectY + fWavePosX;		// 0.75 = adj. for aspect ratioAdd commentMore actions
          v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY;
        }
      }


      break;

      //NEW WAVEFORMS 2023
    case 13: // Star Wave, MilkDrop2077 -----------------------------------------------------------------------------------------------------------

      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts * 12/10);// only call this once nVerts is final!

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

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
          }
          else {
            v[i].x = rad * cosf(ang) * m_fAspectY + fWavePosX;// 0.75 = adj. for aspect ratio
            v[i].y = rad * sinf(ang) * m_fAspectX + fWavePosY;
          }

          //v[i].Diffuse = color;
        }
      }

      // dupe last vertex to connect the lines; skip if blending
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }

      break;

    case 14: // Flower Wave, MilkDrop2077 -------------------------------------------------------------------------------------------------------------

      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts * 12/10);// only call this once nVerts is final!

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      {
        float inv_nverts_minus_one = 1.0f / (float)(nVerts - 1);

        for (int i = 0; i < nVerts; i++) {
          float rad = 0.7f + 0.7f * fR[i + sample_offset] + fWaveParam2;
          float ang = (i)*inv_nverts_minus_one * 6.28f + GetTime() * 0.2f;
          ang = ang / 2;
          rad = rad / 2;
          if (i < nVerts / rad) {
            float mix = i / (nVerts * 0.1f);
            //mix = 0.7f - 0.7f * cosf(mix * 3.1416f) - sinf((GetTime()/10));
            //mix = 0.7f - 0.7f * cosf(mix * 1.1416f) - sinf((GetTime())/8);
            //mix = 0.7f - 0.7f * cosf(mix*2 * 3.1416f); //flower, more leaves
            mix = 0.7f - 0.7f * cosf(mix * 3.1416f);   //flower

            float rad_2 = 0.7f + 0.7f * fR[i + nVerts + sample_offset] + fWaveParam2;
            //rad = rad_2 * (1.0f - mix) + rad * (mix);
              //rad = rad_2 * (1.0f - mix) + rad * (mix) /5; // div5 optional
            rad = rad_2 * (1.0f - mix) + rad * (mix * 2) / 8;
          }
          //v[i].x = rad * (cosf(GetTime()*ang)/3) * m_fAspectY + fWavePosX;// 0.75 = adj. for aspect ratio
          //v[i].y = rad * sinf(GetTime()*ang) * m_fAspectX + fWavePosY;
          //v[i].Diffuse = color;
          //v[i].x = rad * cosf(ang* 2) * m_fAspectY + fWavePosX;//

          if (m_bScreenDependentRenderMode) {
            v[i].x = rad * cosf(ang * 3.1416f) / 1.5f + fWavePosX * cosf(3.1416f);
            v[i].y = rad * sinf(ang - GetTime() / 3) / 1.5f + fWavePosY * cosf(3.1416f);
          }
          else {
            v[i].x = rad * cosf(ang * 3.1416f) * m_fAspectY / 1.5f + fWavePosX * cosf(3.1416f);// 0.75 = adj. for aspect ratio
            v[i].y = rad * sinf(ang - GetTime() / 3) * m_fAspectX / 1.5f + fWavePosY * cosf(3.1416f);
          }
        }
      }

      // dupe last vertex to connect the lines; skip if blending
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }

      break;

      /*
      ALTERNATIVE:
      nVerts /= 2;
      sample_offset = (NUM_WAVEFORM_SAMPLES - nVerts) / 2;//mysound.GoGoAlignatron(nVerts * 12/10);// only call this once nVerts is final!

      if (m_pState->m_bModWaveAlphaByVolume)
      alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      {
      float inv_nverts_minus_one = 1.0f / (float)(nVerts - 1);

      for (int i = 0; i < nVerts; i++)
      {
      float rad = 0.7f + 0.4f * fR[i + sample_offset] + fWaveParam2;
      float ang = (i)* inv_nverts_minus_one * 6.28f + GetTime() * 0.2f;
      if (i < nVerts / rad)
      {
      float mix = i / (nVerts * 0.1f);
      mix = 0.5f - 0.5f * cosf(mix * 3.1416f);
      float rad_2 = 0.5f + 0.4f * fR[i + nVerts + sample_offset] + fWaveParam2/(mix*3);
      rad = rad_2 * (1.0f - mix) + rad * (mix);
        //rad = rad_2 * (1.0f - mix) + rad * (GetTime() / 3); // BIG
      }
      v[i].x = rad * cosf(ang*2) * m_fAspectY + fWavePosX;// 0.75 = adj. for aspect ratio
      v[i].y = rad * sinf(ang - GetTime() / 3) * m_fAspectX + fWavePosY;
      //v[i].Diffuse = color;
      }
      }

      // dupe last vertex to connect the lines; skip if blending
      if (!m_pState->m_bBlending)
      {
      nVerts++;
      memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }

      break;*/

    case 15: // Lasso Wave, MilkDrop2077 --------------------------------------------------------------------------------------------------------------

      //m_pState->m_wave[i].Import(NULL, m_waitstring.szText, 0);
      //break;
      alpha *= 1.25f;
      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;
      //color = D3DCOLOR_RGBA_01(cr, cg, cb, alpha);

      nVerts /= 2;

      for (int i = 0; i < nVerts; i++) {
        float rad = 0.53f + 0.43f * fR[i] + fWaveParam2;
        float ang = fL[i + 32] * 1.57f + GetTime() * 2.0f;
        float t = GetTime() / ang;
        //ball
        //v[i].x = cosf(ang* GetTime())*3 * m_fAspectY/3 + fWavePosX;// 0.75 = adj. for aspect ratio
        //v[i].y = sin(rad* GetTime()) *3 * sinf(ang) * m_fAspectX/3 + fWavePosY;
        //v[i].x = cos(GetTime()) / 2 + cosf(ang * 2 +tanf(t))*m_fAspectY / 2.8 + fWavePosX
        v[i].x = cosf(GetTime()) / 2 + cosf(ang * 2 + tanf(t));
        v[i].y = sinf(GetTime()) * 2 * sinf(ang * 3.14f) * m_fAspectX / 2.8f + fWavePosY;
        //v[i].Diffuse = color;//(D3DCOLOR_RGBA_01(cr, cg, cb, alpha*min(1, max(0, fL[i])));
      }

      break;

    case 16:
      // DeepSeek - Triangle Wave
      nVerts = 256; // More vertices for smoother audio modulation

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;

      {
        float size = 0.575f;
        float rotation = (fWaveParam2) * 3.141593f;
        float cos_rot = cosf(rotation);
        float sin_rot = sinf(rotation);

        float inv_nverts = 1.0f / (float)(nVerts - 1);

        for (int i = 0; i < nVerts; i++) {
          // Create perfect triangle shape
          float phase = i * inv_nverts;
          float x, y;

          if (phase < 0.3333f) {
            // First segment (bottom left to top)
            float t = phase * 3.0f;
            x = -size + t * size;
            y = -size + t * 2.0f * size;
          }
          else if (phase < 0.6666f) {
            // Second segment (top to bottom right)
            float t = (phase - 0.3333f) * 3.0f;
            x = 0.0f + t * size;
            y = size - t * 2.0f * size;
          }
          else {
            // Third segment (bottom right to bottom left)
            float t = (phase - 0.6666f) * 3.0f;
            x = size - t * 2.0f * size;
            y = -size;
          }

          // Apply audio modulation (using circular buffer position)
          float audio_mod = 1.0f + 0.3f * fL[(i * 2) % NUM_WAVEFORM_SAMPLES];
          x *= audio_mod;
          y *= audio_mod;

          // Apply rotation
          float x_rot = x * cos_rot - y * sin_rot;
          float y_rot = x * sin_rot + y * cos_rot;

          // Apply position and aspect ratio
          if (m_bScreenDependentRenderMode) {
            v[i].x = x_rot + fWavePosX;
            v[i].y = y_rot + fWavePosY;
          }
          else {
            v[i].x = x_rot * m_fAspectY + fWavePosX;
            v[i].y = y_rot * m_fAspectX + fWavePosY;
          }
        }
      }

      // Close the triangle
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;

    case 17:
      // DeepSeek - Fireworks Waveform
      nVerts = 256; // Use fewer vertices for better performance with particles

      if (m_pState->m_bModWaveAlphaByVolume)
        alpha *= ((mysound.imm_rel[0] + mysound.imm_rel[1] + mysound.imm_rel[2]) * 0.333f - m_pState->m_fModWaveAlphaStart.eval(GetTime())) / (m_pState->m_fModWaveAlphaEnd.eval(GetTime()) - m_pState->m_fModWaveAlphaStart.eval(GetTime()));
      if (alpha < 0) alpha = 0;
      if (alpha > 1) alpha = 1;

      {
        float time = GetTime();
        float burst_frequency = 1.0f - fWaveParam2 + .001f; // How often new bursts occur
        float burst_phase = fmodf(time, burst_frequency) / burst_frequency;

        // Random seed based on which burst we're on
        int burst_num = (int)(time / burst_frequency);
        float rand_seed = (burst_num * 10.0f);

        // Base position for this firework
        float base_x = (rand_seed * 0.1345f - floor(rand_seed * 0.1345f)) * 2.0f - 1.0f;
        float base_y = (rand_seed * 0.2783f - floor(rand_seed * 0.2783f)) * 2.0f - 1.0f;

        // Make bursts start from random positions but stay centered more often
        if (fmodf(rand_seed, 1.0f) > 0.3f) {
          base_x *= 0.3f;
          base_y *= 0.3f;
        }

        // Size and fade of current burst
        float burst_size = min(1.0f, burst_phase * 4.0f); // Quick expansion
        float burst_fade = 1.0f - powf(burst_phase, 3.0f); // Slow fade

        // Audio reactivity
        float audio_boost = 1.0f + 2.0f * (mysound.imm_rel[0] + mysound.imm_rel[1]) * 0.5f;

        for (int i = 0; i < nVerts; i++) {
          // Particle angle
          float ang = (i / (float)nVerts) * 6.283185f;

          // Particle distance from center (with some randomness)
          float dist_var = 0.7f + 0.3f * (fmodf(rand_seed + i * 0.1f, 1.0f));
          float dist = burst_size * dist_var * (0.5f + 0.5f * fR[(i * 3) % NUM_WAVEFORM_SAMPLES]) * audio_boost;

          // Calculate position
          float x = base_x + cosf(ang) * dist;
          float y = base_y + sinf(ang) * dist;

          // Add some secondary motion
          float swirl = time * 3.0f + ang;
          x += cosf(swirl) * burst_size * 0.1f;
          y += sinf(swirl) * burst_size * 0.1f;

          // Apply aspect ratio
          if (m_bScreenDependentRenderMode) {
            v[i].x = x + fWavePosX;
            v[i].y = y + fWavePosY;
          }
          else {
            v[i].x = x * m_fAspectY + fWavePosX;
            v[i].y = y * m_fAspectX + fWavePosY;
          }

          // Fade particles as burst ages
          alpha *= burst_fade;
        }
      }

      // For point rendering mode (recommended for this waveform)
      if (!m_pState->m_bBlending) {
        nVerts++;
        memcpy(&v[nVerts - 1], &v[0], sizeof(WFVERTEX));
      }
      break;
    }

    if (it == 0) {
      nVerts1 = nVerts;
      nBreak1 = nBreak;
      alpha1 = alpha;
    }
    else {
      nVerts2 = nVerts;
      nBreak2 = nBreak;
      alpha2 = alpha;
    }
  }

  // v1[] is for the current waveform
  // v2[] is for the old waveform (from prev. preset - only used if blending)
  // nVerts1 is the # of vertices in v1
  // nVerts2 is the # of vertices in v2
  // nBreak1 is the index of the point at which to break the solid line in v1[] (-1 if no break)
  // nBreak2 is the index of the point at which to break the solid line in v2[] (-1 if no break)

  float mix = CosineInterp(m_pState->m_fBlendProgress);
  float mix2 = 1.0f - mix;

  // blend 2 waveforms
  if (nVerts2 > 0) {
    // note: this won't yet handle the case where (nBreak1 > 0 && nBreak2 > 0)
    //       in this case, code must break wave into THREE segments
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
      }
      else {
        x = v2[nIdx].x * (1 - t) + v2[nIdx + 1].x * (t);
        y = v2[nIdx].y * (1 - t) + v2[nIdx + 1].y * (t);
      }
      v1[i].x = v1[i].x * (mix)+x * (mix2);
      v1[i].y = v1[i].y * (mix)+y * (mix2);
    }
  }

  // determine alpha
  if (nVerts2 > 0) {
    alpha1 = alpha1 * (mix)+alpha2 * (1.0f - mix);
  }

  // apply color & alpha
    // ALSO reverse all y values, to stay consistent with the pre-VMS milkdrop,
    //  which DIDN'T:
  v1[0].Diffuse = D3DCOLOR_RGBA_01(cr, cg, cb, alpha1);
  for (int i = 0; i < nVerts1; i++) {
    v1[i].Diffuse = v1[0].Diffuse;
    v1[i].y = -v1[i].y;
  }

  // don't draw wave if (possibly blended) alpha is less than zero.
  if (alpha1 < 0.004f)
    goto SKIP_DRAW_WAVE;

  // TESSELLATE - smooth the wave, one time.
  WFVERTEX* pVerts = v1;
  WFVERTEX vTess[(576 + 3) * 2];
  if (1) {
    if (nBreak1 == -1) {
      nVerts1 = SmoothWave(v1, nVerts1, vTess);
    }
    else {
      int oldBreak = nBreak1;
      nBreak1 = SmoothWave(v1, nBreak1, vTess);
      nVerts1 = SmoothWave(&v1[oldBreak], nVerts1 - oldBreak, &vTess[nBreak1]) + nBreak1;
    }
    pVerts = vTess;
  }

  // draw primitives
  {
    //D3DPRIMITIVETYPE primtype = (*m_pState->var_pf_wave_usedots) ? D3DPT_POINTLIST : D3DPT_LINESTRIP;
    float x_inc = 2.0f / (float)m_nTexSizeX;
    float y_inc = 2.0f / (float)m_nTexSizeY;
    int drawing_its = ((*m_pState->var_pf_wave_thick || *m_pState->var_pf_wave_usedots) && (m_nTexSizeX >= 512)) ? 4 : 1;

    for (int it = 0; it < drawing_its; it++) {
      switch (it) {
      case 0: break;
      case 1: for (int j = 0; j < nVerts1; j++) pVerts[j].x += x_inc; break;		// draw fat dots
      case 2: for (int j = 0; j < nVerts1; j++) pVerts[j].y += y_inc; break;		// draw fat dots
      case 3: for (int j = 0; j < nVerts1; j++) pVerts[j].x -= x_inc; break;		// draw fat dots
      }

      if (nBreak1 == -1) {
        if (*m_pState->var_pf_wave_usedots)
          lpDevice->DrawPrimitiveUP(D3DPT_POINTLIST, nVerts1, (void*)pVerts, sizeof(WFVERTEX));
        else
          lpDevice->DrawPrimitiveUP(D3DPT_LINESTRIP, nVerts1 - 1, (void*)pVerts, sizeof(WFVERTEX));
      }
      else {
        if (*m_pState->var_pf_wave_usedots) {
          lpDevice->DrawPrimitiveUP(D3DPT_POINTLIST, nBreak1, (void*)pVerts, sizeof(WFVERTEX));
          lpDevice->DrawPrimitiveUP(D3DPT_POINTLIST, nVerts1 - nBreak1, (void*)&pVerts[nBreak1], sizeof(WFVERTEX));
        }
        else {
          lpDevice->DrawPrimitiveUP(D3DPT_LINESTRIP, nBreak1 - 1, (void*)pVerts, sizeof(WFVERTEX));
          lpDevice->DrawPrimitiveUP(D3DPT_LINESTRIP, nVerts1 - nBreak1 - 1, (void*)&pVerts[nBreak1], sizeof(WFVERTEX));
        }
      }
    }
  }

SKIP_DRAW_WAVE:
  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void mdrop::Engine::DrawSprites() {
  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  lpDevice->SetTexture(0, NULL);
  lpDevice->SetVertexShader(NULL);
  lpDevice->SetFVF(WFVERTEX_FORMAT);

  if (*m_pState->var_pf_darken_center) {
    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);//SRCALPHA);
    lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    WFVERTEX v3[6];
    ZeroMemory(v3, sizeof(WFVERTEX) * 6);

    // colors:
    v3[0].Diffuse = D3DCOLOR_RGBA_01(0, 0, 0, 3.0f / 32.0f);
    v3[1].Diffuse = D3DCOLOR_RGBA_01(0, 0, 0, 0.0f / 32.0f);
    v3[2].Diffuse = v3[1].Diffuse;
    v3[3].Diffuse = v3[1].Diffuse;
    v3[4].Diffuse = v3[1].Diffuse;
    v3[5].Diffuse = v3[1].Diffuse;

    // positioning:
    float fHalfSize = 0.05f;
    v3[0].x = 0.0f;

    if (m_bScreenDependentRenderMode)
      v3[1].x = 0.0f - fHalfSize;
    else
      v3[1].x = 0.0f - fHalfSize * m_fAspectY;
    v3[2].x = 0.0f;

    if (m_bScreenDependentRenderMode)
      v3[3].x = 0.0f + fHalfSize;
    else
      v3[3].x = 0.0f + fHalfSize * m_fAspectY;

    v3[4].x = 0.0f;
    v3[5].x = v3[1].x;
    v3[0].y = 0.0f;
    v3[1].y = 0.0f;
    v3[2].y = 0.0f - fHalfSize;
    v3[3].y = 0.0f;
    v3[4].y = 0.0f + fHalfSize;
    v3[5].y = v3[1].y;
    //v3[0].tu = 0;	v3[1].tu = 1;	v3[2].tu = 0;	v3[3].tu = 1;
    //v3[0].tv = 1;	v3[1].tv = 1;	v3[2].tv = 0;	v3[3].tv = 0;

    lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 4, (LPVOID)v3, sizeof(WFVERTEX));

    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  }

  // do borders
  {
    float fOuterBorderSize = (float)*m_pState->var_pf_ob_size;
    float fInnerBorderSize = (float)*m_pState->var_pf_ib_size;

    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    for (int it = 0; it < 2; it++) {
      WFVERTEX v3[4];
      ZeroMemory(v3, sizeof(WFVERTEX) * 4);

      // colors:
      float r = (it == 0) ? (float)*m_pState->var_pf_ob_r : (float)*m_pState->var_pf_ib_r;
      float g = (it == 0) ? (float)*m_pState->var_pf_ob_g : (float)*m_pState->var_pf_ib_g;
      float b = (it == 0) ? (float)*m_pState->var_pf_ob_b : (float)*m_pState->var_pf_ib_b;
      float a = (it == 0) ? (float)*m_pState->var_pf_ob_a : (float)*m_pState->var_pf_ib_a;
      if (a > 0.001f) {
        v3[0].Diffuse = D3DCOLOR_RGBA_01(r, g, b, a);
        v3[1].Diffuse = v3[0].Diffuse;
        v3[2].Diffuse = v3[0].Diffuse;
        v3[3].Diffuse = v3[0].Diffuse;

        // positioning:
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
          lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, (LPVOID)v3, sizeof(WFVERTEX));

          // rotate by 90 degrees
          for (int v = 0; v < 4; v++) {
            float t = 1.570796327f;
            float x = v3[v].x;
            float y = v3[v].y;
            v3[v].x = x * cosf(t) - y * sinf(t);
            v3[v].y = x * sinf(t) + y * cosf(t);
          }
        }
      }
    }
    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  }
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

void mdrop::Engine::DrawUserSprites(int targetLayer)	// from system memory, to back buffer.
{
  if (!m_lpDX || !m_lpDX->m_commandList)
    return;

  auto* cmdList = m_lpDX->m_commandList.Get();

  // Set up DX12 rendering state
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  for (int iSlot = 0; iSlot < NUM_TEX; iSlot++) {
    if (m_texmgr.m_tex[iSlot].dx12Surface.IsValid()) {
      // Evaluate expressions only on the first pass (targetLayer <= 0)
      // to avoid double-advancing time-based animations
      if (targetLayer <= 0) {
        // set values of input variables:
        *(m_texmgr.m_tex[iSlot].var_time) = (double)(GetTime() - m_texmgr.m_tex[iSlot].fStartTime);
        *(m_texmgr.m_tex[iSlot].var_frame) = (double)(GetFrame() - m_texmgr.m_tex[iSlot].nStartFrame);
        *(m_texmgr.m_tex[iSlot].var_fps) = (double)GetFps();
        *(m_texmgr.m_tex[iSlot].var_progress) = (double)m_pState->m_fBlendProgress;
        *(m_texmgr.m_tex[iSlot].var_bass) = (double)mysound.imm_rel[0];
        *(m_texmgr.m_tex[iSlot].var_mid) = (double)mysound.imm_rel[1];
        *(m_texmgr.m_tex[iSlot].var_treb) = (double)mysound.imm_rel[2];
        *(m_texmgr.m_tex[iSlot].var_bass_att) = (double)mysound.avg_rel[0];
        *(m_texmgr.m_tex[iSlot].var_mid_att) = (double)mysound.avg_rel[1];
        *(m_texmgr.m_tex[iSlot].var_treb_att) = (double)mysound.avg_rel[2];

        // evaluate expressions
#ifndef _NO_EXPR_
        if (m_texmgr.m_tex[iSlot].m_codehandle) {
          NSEEL_code_execute(m_texmgr.m_tex[iSlot].m_codehandle);
        }
#endif
      }

      // Filter by target layer (-1 = all, 0 = behind text, 1 = on top)
      if (targetLayer >= 0) {
        int spriteLayer = (*m_texmgr.m_tex[iSlot].var_layer != 0.0) ? 1 : 0;
        if (spriteLayer != targetLayer) continue;
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

      float x = min(1000.0f, max(-1000.0f, (float)(*m_texmgr.m_tex[iSlot].var_x) * 2.0f - 1.0f));
      float y = min(1000.0f, max(-1000.0f, (float)(*m_texmgr.m_tex[iSlot].var_y) * 2.0f - 1.0f));
      float sx = min(1000.0f, max(-1000.0f, (float)(*m_texmgr.m_tex[iSlot].var_sx)));
      float sy = min(1000.0f, max(-1000.0f, (float)(*m_texmgr.m_tex[iSlot].var_sy)));
      float rot = (float)(*m_texmgr.m_tex[iSlot].var_rot);
      int flipx = (*m_texmgr.m_tex[iSlot].var_flipx == 0.0) ? 0 : 1;
      int flipy = (*m_texmgr.m_tex[iSlot].var_flipy == 0.0) ? 0 : 1;
      float repeatx = min(100.0f, max(0.01f, (float)(*m_texmgr.m_tex[iSlot].var_repeatx)));
      float repeaty = min(100.0f, max(0.01f, (float)(*m_texmgr.m_tex[iSlot].var_repeaty)));

      int blendmode = min(4, max(0, ((int)(*m_texmgr.m_tex[iSlot].var_blendmode))));
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

      // first aspect ratio: adjust for non-1:1 images
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

      // second aspect ratio: normalize to width of screen
      {
        float aspect = GetWidth() / (float)(GetHeight());

        if (aspect > 1)
          for (int k = 0; k < 4; k++) v3[k].y *= aspect;
        else
          for (int k = 0; k < 4; k++) v3[k].x /= aspect;
      }

      // third aspect ratio: adjust for burn-in
      if (bKillSprite && bBurnIn)	// final render-to-VS1
      {
        float aspect = GetWidth() / (float)(GetHeight() * 4.0f / 3.0f);
        if (!m_bScreenDependentRenderMode)
          if (aspect < 1.0f)
            for (int k = 0; k < 4; k++) v3[k].x *= aspect;
          else
            for (int k = 0; k < 4; k++) v3[k].y /= aspect;
      }

      // DX12: flip Y — DX9 OrthoLH(2,-2) negated Y implicitly; DX12 passthrough VS does not
      for (int k = 0; k < 4; k++) v3[k].y *= -1.0f;

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
        // srccolor — approximate with alpha blend
        spritePso = PSO_ALPHABLEND_SPRITEVERTEX;
        for (int k = 0; k < 4; k++) v3[k].Diffuse = D3DCOLOR_RGBA_01(1, 1, 1, 1);
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
      m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, 6, sizeof(SPRITEVERTEX));

      // Burn-in: also render to VS1 so the sprite persists in the feedback loop
      if (bKillSprite && bBurnIn && m_dx12VS[1].resource) {
        m_lpDX->TransitionResource(m_dx12VS[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE vs1Rtv = m_lpDX->GetRtvCpuHandle(m_dx12VS[1]);
        cmdList->OMSetRenderTargets(1, &vs1Rtv, FALSE, nullptr);

        SetViewportAndScissor(cmdList, m_nTexSizeX, m_nTexSizeY);

        SPRITEVERTEX burnVerts[6] = { triVerts[0], triVerts[1], triVerts[2],
                                      triVerts[3], triVerts[4], triVerts[5] };
        m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, burnVerts, 6, sizeof(SPRITEVERTEX));

        // Restore backbuffer as render target
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = m_lpDX->m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        bbRtv.ptr += (SIZE_T)m_lpDX->m_frameIndex * m_lpDX->m_rtvDescriptorSize;
        cmdList->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);

        SetViewportAndScissor(cmdList, m_lpDX->m_client_width, m_lpDX->m_client_height);
      }

      if (bKillSprite) {
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

void mdrop::Engine::RestoreShaderParams() {
  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  for (int i = 0; i < 2; i++) {
    lpDevice->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);//texaddr);
    lpDevice->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);//texaddr);
    lpDevice->SetSamplerState(i, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);//texaddr);
    lpDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    lpDevice->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    lpDevice->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  }

  for (int i = 0; i < 4; i++)
    lpDevice->SetTexture(i, NULL);

  lpDevice->SetVertexShader(NULL);
  //lpDevice->SetVertexDeclaration(NULL);  -directx debug runtime complains heavily about this
  lpDevice->SetPixelShader(NULL);

}

void mdrop::Engine::BuildBindingSlots(CShaderParams* params, const DX12Texture& vsTex, UINT outSlots[32], const DX12Texture* feedbackTex, const DX12Texture* imageFeedbackTex, const DX12Texture* bufferBTex) {
  for (int i = 0; i < 32; i++) {
    outSlots[i] = UINT_MAX;
    switch (params->m_texcode[i]) {
    case TEX_VS:
      outSlots[i] = vsTex.srvIndex;
      break;
    case TEX_FEEDBACK:
      if (feedbackTex && feedbackTex->IsValid())
        outSlots[i] = feedbackTex->srvIndex;
      else if (m_dx12Feedback[0].IsValid())
        outSlots[i] = m_dx12Feedback[0].srvIndex;  // default: read buffer
      else
        outSlots[i] = vsTex.srvIndex;  // fallback to VS if feedback not ready
      break;
    case TEX_IMAGE_FEEDBACK:
      if (imageFeedbackTex && imageFeedbackTex->IsValid())
        outSlots[i] = imageFeedbackTex->srvIndex;
      else if (m_dx12ImageFeedback[0].IsValid())
        outSlots[i] = m_dx12ImageFeedback[0].srvIndex;
      else
        outSlots[i] = vsTex.srvIndex;  // fallback
      break;
    case TEX_BUFFER_B:
      if (bufferBTex && bufferBTex->IsValid())
        outSlots[i] = bufferBTex->srvIndex;
      else if (m_dx12FeedbackB[0].IsValid())
        outSlots[i] = m_dx12FeedbackB[0].srvIndex;
      break;
    case TEX_AUDIO:
      if (m_dx12AudioTex.IsValid())
        outSlots[i] = m_dx12AudioTex.srvIndex;
      break;
#if (NUM_BLUR_TEX >= 2)
    case TEX_BLUR1:
      if (m_dx12Blur[1].srvIndex != UINT_MAX) outSlots[i] = m_dx12Blur[1].srvIndex;
      break;
#endif
#if (NUM_BLUR_TEX >= 4)
    case TEX_BLUR2:
      if (m_dx12Blur[3].srvIndex != UINT_MAX) outSlots[i] = m_dx12Blur[3].srvIndex;
      break;
#endif
#if (NUM_BLUR_TEX >= 6)
    case TEX_BLUR3:
      if (m_dx12Blur[5].srvIndex != UINT_MAX) outSlots[i] = m_dx12Blur[5].srvIndex;
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
  // Diagnostic dump for Shadertoy binding verification (Verbose only)
  if (DLOG_DIAG_ENABLED() && m_bShadertoyMode && !m_bPresetDiagLogged) {
    wchar_t diagPath[MAX_PATH];
    swprintf(diagPath, MAX_PATH, L"%sdiag_bindings.txt", m_szBaseDir);
    FILE* fp = nullptr;
    _wfopen_s(&fp, diagPath, L"a");
    if (fp) {
      fprintf(fp, "Binding: fb=%s(%u) bufB=%s(%u)\n",
              feedbackTex && feedbackTex->IsValid() ? "OK" : "no",
              feedbackTex ? feedbackTex->srvIndex : UINT_MAX,
              bufferBTex && bufferBTex->IsValid() ? "OK" : "no",
              bufferBTex ? bufferBTex->srvIndex : UINT_MAX);
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

void mdrop::Engine::ApplyShaderParams(CShaderParams* p, LPD3DXCONSTANTTABLE pCT, CState* pState) {
  LPDIRECT3DDEVICE9 lpDevice = GetDevice();

  // bind textures + sampler states (DX9 only — skip if no DX9 device)
  for (int i = 0; i < sizeof(p->m_texture_bindings) / sizeof(p->m_texture_bindings[0]); i++) {
    if (lpDevice) {
      if (p->m_texcode[i] == TEX_VS)
        lpDevice->SetTexture(i, m_lpVS[0]);
      else
        lpDevice->SetTexture(i, p->m_texture_bindings[i].texptr);

      if (p->m_texcode[i] == TEX_VS || p->m_texture_bindings[i].texptr) {
        bool bAniso = false;
        DWORD HQFilter = bAniso ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
        DWORD wrap = p->m_texture_bindings[i].bWrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
        DWORD filter = p->m_texture_bindings[i].bBilinear ? HQFilter : D3DTEXF_POINT;
        lpDevice->SetSamplerState(i, D3DSAMP_ADDRESSU, wrap);
        lpDevice->SetSamplerState(i, D3DSAMP_ADDRESSV, wrap);
        lpDevice->SetSamplerState(i, D3DSAMP_ADDRESSW, wrap);
        lpDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, filter);
        lpDevice->SetSamplerState(i, D3DSAMP_MINFILTER, filter);
        lpDevice->SetSamplerState(i, D3DSAMP_MIPFILTER, filter);
      }
    }

    // still track blur texture usage regardless of DX9 device
    if (p->m_texcode[i] >= TEX_BLUR1 && p->m_texcode[i] <= TEX_BLUR_LAST)
      m_nHighestBlurTexUsedThisFrame = max(m_nHighestBlurTexUsedThisFrame, ((int)p->m_texcode[i] - (int)TEX_BLUR1) + 1);
  }

  // bind "texsize_XYZ" params
  int N = (int)p->texsize_params.size();
  for (int i = 0; i < N; i++) {
    TexSizeParamInfo* q = &(p->texsize_params[i]);
    pCT->SetVector(lpDevice, q->texsize_param, &D3DXVECTOR4((float)q->w, (float)q->h, 1.0f / q->w, 1.0f / q->h));
  }

  float time_since_preset_start = GetTime() - pState->GetPresetStartTime();
  float time_since_preset_start_wrapped = time_since_preset_start - (int)(time_since_preset_start / 10000) * 10000;
  double time = GetTime() - m_fStartTime;
  float progress = (GetTime() - m_fPresetStartTime) / (m_fNextPresetTime - m_fPresetStartTime);
  float mip_x = logf((float)GetWidth()) / logf(2.0f);
  float mip_y = logf((float)GetWidth()) / logf(2.0f);
  float mip_avg = 0.5f * (mip_x + mip_y);
  float aspect_x = 1;
  float aspect_y = 1;

  if (!m_bScreenDependentRenderMode)
    if (GetWidth() > GetHeight())
      aspect_y = GetHeight() / (float)GetWidth();
    else
      aspect_x = GetWidth() / (float)GetHeight();

  float blur_min[3], blur_max[3];
  GetSafeBlurMinMax(pState, blur_min, blur_max);

  // bind float4's (C-Variables!)
  if (p->rand_frame) pCT->SetVector(lpDevice, p->rand_frame, &m_rand_frame);
  if (p->rand_preset) pCT->SetVector(lpDevice, p->rand_preset, &pState->m_rand_preset);
  D3DXHANDLE* h = p->const_handles;
  if (h[0]) pCT->SetVector(lpDevice, h[0], &D3DXVECTOR4(aspect_x, aspect_y, 1.0f / aspect_x, 1.0f / aspect_y));
  if (h[1]) pCT->SetVector(lpDevice, h[1], &D3DXVECTOR4(0, 0, 0, 0));
  {
    // In Shadertoy mode, iFrame must start at 0 when the preset loads (not global frame count).
    // Many Shadertoy shaders use "if (iFrame < 2) { reset; }" to initialize the feedback buffer.
    float frameVal = m_bShadertoyMode ? (float)(GetFrame() - m_nShadertoyStartFrame) : (float)GetFrame();
    if (h[2]) pCT->SetVector(lpDevice, h[2], &D3DXVECTOR4(time_since_preset_start_wrapped, GetFps(), frameVal, progress));
  }
  if (h[3]) pCT->SetVector(lpDevice, h[3], &D3DXVECTOR4(mysound.imm_rel[0], mysound.imm_rel[1], mysound.imm_rel[2], 0.3333f * (mysound.imm_rel[0], mysound.imm_rel[1], mysound.imm_rel[2])));
  if (h[4]) pCT->SetVector(lpDevice, h[4], &D3DXVECTOR4(mysound.avg_rel[0], mysound.avg_rel[1], mysound.avg_rel[2], 0.3333f * (mysound.avg_rel[0], mysound.avg_rel[1], mysound.avg_rel[2])));
  if (h[5]) pCT->SetVector(lpDevice, h[5], &D3DXVECTOR4(blur_max[0] - blur_min[0], blur_min[0], blur_max[1] - blur_min[1], blur_min[1]));
  if (h[6]) pCT->SetVector(lpDevice, h[6], &D3DXVECTOR4(blur_max[2] - blur_min[2], blur_min[2], blur_min[0], blur_max[0]));
  if (h[7]) pCT->SetVector(lpDevice, h[7], &D3DXVECTOR4((float)m_nTexSizeX, (float)m_nTexSizeY, 1.0f / (float)m_nTexSizeX, 1.0f / (float)m_nTexSizeY));
  if (h[8]) pCT->SetVector(lpDevice, h[8], &D3DXVECTOR4(0.5f + 0.5f * cosf((float)time * 0.329f + 1.2f),
    0.5f + 0.5f * cosf((float)time * 1.293f + 3.9f),
    0.5f + 0.5f * cosf((float)time * 5.070f + 2.5f),
    0.5f + 0.5f * cosf((float)time * 20.051f + 5.4f)
  ));
  if (h[9]) pCT->SetVector(lpDevice, h[9], &D3DXVECTOR4(0.5f + 0.5f * sinf((float)time * 0.329f + 1.2f),
    0.5f + 0.5f * sinf((float)time * 1.293f + 3.9f),
    0.5f + 0.5f * sinf((float)time * 5.070f + 2.5f),
    0.5f + 0.5f * sinf((float)time * 20.051f + 5.4f)
  ));
  if (h[10]) pCT->SetVector(lpDevice, h[10], &D3DXVECTOR4(0.5f + 0.5f * cosf((float)time * 0.0050f + 2.7f),
    0.5f + 0.5f * cosf((float)time * 0.0085f + 5.3f),
    0.5f + 0.5f * cosf((float)time * 0.0133f + 4.5f),
    0.5f + 0.5f * cosf((float)time * 0.0217f + 3.8f)
  ));
  if (h[11]) pCT->SetVector(lpDevice, h[11], &D3DXVECTOR4(0.5f + 0.5f * sinf((float)time * 0.0050f + 2.7f),
    0.5f + 0.5f * sinf((float)time * 0.0085f + 5.3f),
    0.5f + 0.5f * sinf((float)time * 0.0133f + 4.5f),
    0.5f + 0.5f * sinf((float)time * 0.0217f + 3.8f)
  ));
  if (h[12]) pCT->SetVector(lpDevice, h[12], &D3DXVECTOR4(mip_x, mip_y, mip_avg, 0));
  if (h[13]) pCT->SetVector(lpDevice, h[13], &D3DXVECTOR4(blur_min[1], blur_max[1], blur_min[2], blur_max[2]));
  
  // BMV/MDropDX12
  if (h[14]) {
    if (m_bShadertoyMode) {
      // Shadertoy iMouse: pixel coords, z/w encode click position with sign for button state
      bool neverClicked = (m_stClickX == 0.f && m_stClickY == 0.f);
      float z = neverClicked ? 0.f : (m_stMouseDown ?  m_stClickX : -m_stClickX);
      float w = neverClicked ? 0.f : (m_stMouseJustClicked ? m_stClickY : -m_stClickY);
      pCT->SetVector(lpDevice, h[14], &D3DXVECTOR4(m_stMouseX, m_stMouseY, z, w));
    } else {
      pCT->SetVector(lpDevice, h[14], &D3DXVECTOR4(m_mouseX,
        m_mouseY != -1 ? -m_mouseY + 1 : -1,
        m_mouseDown ? 1.0f : 0.0f,
        m_mouseClicked > 0 ? m_lastMouseY : -m_lastMouseY));
    }
  }
  if (h[15]) pCT->SetVector(lpDevice, h[15], &D3DXVECTOR4(mysound.smooth[0], mysound.smooth[1], mysound.smooth[2], 0.3333f * (mysound.smooth[0], mysound.smooth[1], mysound.smooth[2])));
  if (h[16]) pCT->SetVector(lpDevice, h[16], &D3DXVECTOR4(m_VisIntensity, m_VisShift, m_VisVersion, 0));
  if (h[17]) pCT->SetVector(lpDevice, h[17], &D3DXVECTOR4(m_ColShiftHue, m_ColShiftSaturation, m_ColShiftBrightness, 0));
  if (h[18]) pCT->SetVector(lpDevice, h[18], &D3DXVECTOR4((float)(*pState->var_pf_gamma), 0, 0, 0));
  if (h[19]) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    float secsSinceMidnight = (float)st.wHour * 3600.0f + (float)st.wMinute * 60.0f + (float)st.wSecond + (float)st.wMilliseconds * 0.001f;
    pCT->SetVector(lpDevice, h[19], &D3DXVECTOR4((float)st.wYear, (float)(st.wMonth - 1), (float)st.wDay, secsSinceMidnight));
  }

  // write q vars
  int num_q_float4s = sizeof(p->q_const_handles) / sizeof(p->q_const_handles[0]);
  for (int i = 0; i < num_q_float4s; i++) {
    if (p->q_const_handles[i])
      pCT->SetVector(lpDevice, p->q_const_handles[i], &D3DXVECTOR4(
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

      pCT->SetMatrix(lpDevice, p->rot_mat[i], &temp);
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

      pCT->SetMatrix(lpDevice, p->rot_mat[i], &temp);
    }
  }
}

void mdrop::Engine::ShowToUser_NoShaders()//int bRedraw, int nPassOverride)
{
  // note: this one has to draw the whole screen!  (one big quad)

  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  lpDevice->SetTexture(0, m_lpVS[1]);
  lpDevice->SetVertexShader(NULL);
  lpDevice->SetPixelShader(NULL);
  lpDevice->SetFVF(SPRITEVERTEX_FORMAT);

  // stages 0 and 1 always just use bilinear filtering.
  lpDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  lpDevice->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

  // note: this texture stage state setup works for 0 or 1 texture.
  // if you set a texture, it will be modulated with the current diffuse color.
  // if you don't set a texture, it will just use the current diffuse color.
  lpDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  lpDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  lpDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
  lpDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  lpDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  lpDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  lpDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  float fZoom = 1.0f;
  SPRITEVERTEX v3[4];
  ZeroMemory(v3, sizeof(SPRITEVERTEX) * 4);

  // extend the poly we draw by 1 pixel around the viewable image area,
  //  in case the video card wraps u/v coords with a +0.5-texel offset
  //  (otherwise, a 1-pixel-wide line of the image would wrap at the top and left edges).
  float fOnePlusInvWidth = 1.0f + 1.0f / (float)GetWidth();
  float fOnePlusInvHeight = 1.0f + 1.0f / (float)GetHeight();
  v3[0].x = -fOnePlusInvWidth;
  v3[1].x = fOnePlusInvWidth;
  v3[2].x = -fOnePlusInvWidth;
  v3[3].x = fOnePlusInvWidth;
  v3[0].y = fOnePlusInvHeight;
  v3[1].y = fOnePlusInvHeight;
  v3[2].y = -fOnePlusInvHeight;
  v3[3].y = -fOnePlusInvHeight;

  //float aspect = GetWidth() / (float)(GetHeight()/(ASPECT)/**4.0f/3.0f*/);
  float aspect = GetWidth() / (float)(GetHeight() * m_fInvAspectY/**4.0f/3.0f*/);
  float x_aspect_mult = 1.0f;
  float y_aspect_mult = 1.0f;

  if (!m_bScreenDependentRenderMode)
    if (aspect > 1)
      y_aspect_mult = aspect;
    else
      x_aspect_mult = 1.0f / aspect;

  for (int n = 0; n < 4; n++) {
    v3[n].x *= x_aspect_mult;
    v3[n].y *= y_aspect_mult;
  }

  {
    float shade[4][3] = {
      { 1.0f, 1.0f, 1.0f },
      { 1.0f, 1.0f, 1.0f },
      { 1.0f, 1.0f, 1.0f },
      { 1.0f, 1.0f, 1.0f } };  // for each vertex, then each comp.

    float fShaderAmount = m_pState->m_fShader.eval(GetTime());

    if (fShaderAmount > 0.001f) {
      for (int i = 0; i < 4; i++) {
        shade[i][0] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0143f + 3 + i * 21 + m_fRandStart[3]);
        shade[i][1] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0107f + 1 + i * 13 + m_fRandStart[1]);
        shade[i][2] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0129f + 6 + i * 9 + m_fRandStart[2]);
        float max = ((shade[i][0] > shade[i][1]) ? shade[i][0] : shade[i][1]);
        if (shade[i][2] > max) max = shade[i][2];
        for (int k = 0; k < 3; k++) {
          shade[i][k] /= max;
          shade[i][k] = 0.5f + 0.5f * shade[i][k];
        }
        for (int k = 0; k < 3; k++) {
          shade[i][k] = shade[i][k] * (fShaderAmount)+1.0f * (1.0f - fShaderAmount);
        }
        v3[i].Diffuse = D3DCOLOR_RGBA_01(shade[i][0], shade[i][1], shade[i][2], 1);
      }
    }

    float fVideoEchoZoom = (float)(*m_pState->var_pf_echo_zoom);//m_pState->m_fVideoEchoZoom.eval(GetTime());
    float fVideoEchoAlpha = (float)(*m_pState->var_pf_echo_alpha);//m_pState->m_fVideoEchoAlpha.eval(GetTime());
    int   nVideoEchoOrientation = (int)(*m_pState->var_pf_echo_orient) % 4;//m_pState->m_nVideoEchoOrientation;
    float fGammaAdj = (float)(*m_pState->var_pf_gamma);//m_pState->m_fGammaAdj.eval(GetTime());

    if (m_pState->m_bBlending &&
      m_pState->m_fVideoEchoAlpha.eval(GetTime()) > 0.01f &&
      m_pState->m_fVideoEchoAlphaOld > 0.01f &&
      m_pState->m_nVideoEchoOrientation != m_pState->m_nVideoEchoOrientationOld) {
      if (m_pState->m_fBlendProgress < m_fSnapPoint) {
        nVideoEchoOrientation = m_pState->m_nVideoEchoOrientationOld;
        fVideoEchoAlpha *= 1.0f - 2.0f * CosineInterp(m_pState->m_fBlendProgress);
      }
      else {
        fVideoEchoAlpha *= 2.0f * CosineInterp(m_pState->m_fBlendProgress) - 1.0f;
      }
    }

    if (fVideoEchoAlpha > 0.001f) {
      // video echo
      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);

      for (int i = 0; i < 2; i++) {
        fZoom = (i == 0) ? 1.0f : fVideoEchoZoom;

        float temp_lo = 0.5f - 0.5f / fZoom;
        float temp_hi = 0.5f + 0.5f / fZoom;
        v3[0].tu = temp_lo;
        v3[0].tv = temp_hi;
        v3[1].tu = temp_hi;
        v3[1].tv = temp_hi;
        v3[2].tu = temp_lo;
        v3[2].tv = temp_lo;
        v3[3].tu = temp_hi;
        v3[3].tv = temp_lo;

        // flipping
        if (i == 1) {
          for (int j = 0; j < 4; j++) {
            if (nVideoEchoOrientation % 2)
              v3[j].tu = 1.0f - v3[j].tu;
            if (nVideoEchoOrientation >= 2)
              v3[j].tv = 1.0f - v3[j].tv;
          }
        }

        float mix = (i == 1) ? fVideoEchoAlpha : 1.0f - fVideoEchoAlpha;
        for (int k = 0; k < 4; k++)
          v3[k].Diffuse = D3DCOLOR_RGBA_01(mix * shade[k][0], mix * shade[k][1], mix * shade[k][2], 1);

        lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));

        if (i == 0) {
          lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
          lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        }

        if (fGammaAdj > 0.001f) {
          // draw layer 'i' a 2nd (or 3rd, or 4th...) time, additively
          int nRedraws = (int)(fGammaAdj - 0.0001f);
          float gamma;

          for (int nRedraw = 0; nRedraw < nRedraws; nRedraw++) {
            if (nRedraw == nRedraws - 1)
              gamma = fGammaAdj - (int)(fGammaAdj - 0.0001f);
            else
              gamma = 1.0f;

            for (int k = 0; k < 4; k++)
              v3[k].Diffuse = D3DCOLOR_RGBA_01(gamma * mix * shade[k][0], gamma * mix * shade[k][1], gamma * mix * shade[k][2], 1);
            lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));
          }
        }
      }
    }
    else {
      // no video echo
      v3[0].tu = 0;	v3[1].tu = 1;	v3[2].tu = 0;	v3[3].tu = 1;
      v3[0].tv = 1;	v3[1].tv = 1;	v3[2].tv = 0;	v3[3].tv = 0;

      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);

      // draw it iteratively, solid the first time, and additively after that
      int nPasses = (int)(fGammaAdj - 0.001f) + 1;
      float gamma;

      for (int nPass = 0; nPass < nPasses; nPass++) {
        if (nPass == nPasses - 1)
          gamma = fGammaAdj - (float)nPass;
        else
          gamma = 1.0f;

        for (int k = 0; k < 4; k++)
          v3[k].Diffuse = D3DCOLOR_RGBA_01(gamma * shade[k][0], gamma * shade[k][1], gamma * shade[k][2], 1);
        lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));

        if (nPass == 0) {
          lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
          lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
          lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        }
      }
    }

    SPRITEVERTEX v3[4];
    ZeroMemory(v3, sizeof(SPRITEVERTEX) * 4);
    float fOnePlusInvWidth = 1.0f + 1.0f / (float)GetWidth();
    float fOnePlusInvHeight = 1.0f + 1.0f / (float)GetHeight();
    v3[0].x = -fOnePlusInvWidth;
    v3[1].x = fOnePlusInvWidth;
    v3[2].x = -fOnePlusInvWidth;
    v3[3].x = fOnePlusInvWidth;
    v3[0].y = fOnePlusInvHeight;
    v3[1].y = fOnePlusInvHeight;
    v3[2].y = -fOnePlusInvHeight;
    v3[3].y = -fOnePlusInvHeight;
    for (int i = 0; i < 4; i++) v3[i].Diffuse = D3DCOLOR_RGBA_01(1, 1, 1, 1);

    if (*m_pState->var_pf_brighten &&
      (GetCaps()->SrcBlendCaps & D3DPBLENDCAPS_INVDESTCOLOR) &&
      (GetCaps()->DestBlendCaps & D3DPBLENDCAPS_DESTCOLOR)
      ) {
      // square root filter

      //lpDevice->SetRenderState(D3DRS_COLORVERTEX, FALSE);       //?
      //lpDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT); //?

      lpDevice->SetTexture(0, NULL);
      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

      // first, a perfect invert
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
      lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));

      // then modulate by self (square it)
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_DESTCOLOR);
      lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));

      // then another perfect invert
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
      lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));
    }

    if (*m_pState->var_pf_darken &&
      (GetCaps()->DestBlendCaps & D3DPBLENDCAPS_DESTCOLOR)
      ) {
      // squaring filter

      //lpDevice->SetRenderState(D3DRS_COLORVERTEX, FALSE);          //?
      //lpDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);    //?

      lpDevice->SetTexture(0, NULL);
      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_DESTCOLOR);
      lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));

      //lpDevice->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_DESTCOLOR);
      //lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
      //lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));

    }

    if (*m_pState->var_pf_solarize &&
      (GetCaps()->SrcBlendCaps & D3DPBLENDCAPS_DESTCOLOR) &&
      (GetCaps()->DestBlendCaps & D3DPBLENDCAPS_INVDESTCOLOR)
      ) {
      //lpDevice->SetRenderState(D3DRS_COLORVERTEX, FALSE);        //?
      //lpDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);  //?

      lpDevice->SetTexture(0, NULL);
      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVDESTCOLOR);
      lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));

      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
      lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));
    }

    if (*m_pState->var_pf_invert &&
      (GetCaps()->SrcBlendCaps & D3DPBLENDCAPS_INVDESTCOLOR)
      ) {
      //lpDevice->SetRenderState(D3DRS_COLORVERTEX, FALSE);        //?
      //lpDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);  //?

      lpDevice->SetTexture(0, NULL);
      lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);

      lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, (void*)v3, sizeof(SPRITEVERTEX));
    }

    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  }
}

void mdrop::Engine::ShowToUser_Shaders(int nPass, bool bAlphaBlend, bool bFlipAlpha, bool bCullTiles, bool bFlipCulling)//int bRedraw, int nPassOverride, bool bFlipAlpha)
{
  LPDIRECT3DDEVICE9 lpDevice = GetDevice();
  if (!lpDevice)
    return;

  //lpDevice->SetTexture(0, m_lpVS[1]);
  lpDevice->SetVertexShader(NULL);
  lpDevice->SetFVF(MYVERTEX_FORMAT);
  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

  float fZoom = 1.0f;

  float aspect = GetWidth() / (float)(GetHeight() * m_fInvAspectY/**4.0f/3.0f*/);
  float x_aspect_mult = 1.0f;
  float y_aspect_mult = 1.0f;

  if (!m_bScreenDependentRenderMode)
    if (aspect > 1)
      y_aspect_mult = aspect;
    else
      x_aspect_mult = 1.0f / aspect;

  // hue shader
  float shade[4][3] = {
    { 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f } };  // for each vertex, then each comp.

  float fShaderAmount = 1;//since we don't know if shader uses it or not!  m_pState->m_fShader.eval(GetTime());

  if (fShaderAmount > 0.001f || m_pState->m_bBlending) {
    // pick 4 colors for the 4 corners
    for (int i = 0; i < 4; i++) {
      shade[i][0] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0143f + 3 + i * 21 + m_fRandStart[3]);
      shade[i][1] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0107f + 1 + i * 13 + m_fRandStart[1]);
      shade[i][2] = 0.6f + 0.3f * sinf(GetTime() * 30.0f * 0.0129f + 6 + i * 9 + m_fRandStart[2]);
      float max = ((shade[i][0] > shade[i][1]) ? shade[i][0] : shade[i][1]);
      if (shade[i][2] > max) max = shade[i][2];
      for (int k = 0; k < 3; k++) {
        shade[i][k] /= max;
        shade[i][k] = 0.5f + 0.5f * shade[i][k];
      }
      // note: we now pass the raw hue shader colors down; the shader can only use a certain % if it wants.
//for (k=0; k<3; k++)
//	shade[i][k] = shade[i][k]*(fShaderAmount) + 1.0f*(1.0f - fShaderAmount);
//m_comp_verts[i].Diffuse = D3DCOLOR_RGBA_01(shade[i][0],shade[i][1],shade[i][2],1);
    }

    // interpolate the 4 colors & apply to all the verts
    for (int j = 0; j < FCGSY; j++) {
      for (int i = 0; i < FCGSX; i++) {
        MYVERTEX* p = &m_comp_verts[i + j * FCGSX];
        float x = p->x * 0.5f + 0.5f;
        float y = p->y * 0.5f + 0.5f;

        float col[3] = { 1, 1, 1 };
        if (fShaderAmount > 0.001f) {
          for (int c = 0; c < 3; c++)
            col[c] = shade[0][c] * (x) * (y)+
            shade[1][c] * (1 - x) * (y)+
            shade[2][c] * (x) * (1 - y) +
            shade[3][c] * (1 - x) * (1 - y);
        }

        // TO DO: improve interp here?
        // TO DO: during blend, only send the triangles needed

        // if blending, also set up the alpha values - pull them from the alphas used for the Warped Blit
        double alpha = 1;
        if (m_pState->m_bBlending) {
          x *= (m_nGridX + 1);
          y *= (m_nGridY + 1);
          x = max(min(x, m_nGridX - 1), 0);
          y = max(min(y, m_nGridY - 1), 0);
          int nx = (int)x;
          int ny = (int)y;
          double dx = x - nx;
          double dy = y - ny;
          double alpha00 = (m_verts[(ny) * (m_nGridX + 1) + (nx)].Diffuse >> 24);
          double alpha01 = (m_verts[(ny) * (m_nGridX + 1) + (nx + 1)].Diffuse >> 24);
          double alpha10 = (m_verts[(ny + 1) * (m_nGridX + 1) + (nx)].Diffuse >> 24);
          double alpha11 = (m_verts[(ny + 1) * (m_nGridX + 1) + (nx + 1)].Diffuse >> 24);
          alpha = alpha00 * (1 - dx) * (1 - dy) +
            alpha01 * (dx) * (1 - dy) +
            alpha10 * (1 - dx) * (dy)+
            alpha11 * (dx) * (dy);
          alpha /= 255.0f;
          //if (bFlipAlpha)
          //    alpha = 1-alpha;

          //alpha = (m_verts[y*(m_nGridX+1) + x].Diffuse >> 24) / 255.0f;
        }
        p->Diffuse = D3DCOLOR_RGBA_01(col[0], col[1], col[2], alpha);
      }
    }
  }

  int nAlphaTestValue = 0;
  if (bFlipCulling)
    nAlphaTestValue = 1 - nAlphaTestValue;

  if (bAlphaBlend) {
    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    if (bFlipAlpha) {
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVSRCALPHA);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
    }
    else {
      lpDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      lpDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
  }
  else
    lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

  // Now do the final composite blit, fullscreen;
  //  or do it twice, alpha-blending, if we're blending between two sets of shaders.

  int pass = nPass;
  {
    // PASS 0: draw using *blended per-vertex motion vectors*, but with the OLD comp shader.
    // PASS 1: draw using *blended per-vertex motion vectors*, but with the NEW comp shader.
    PShaderInfo* si = (pass == 0) ? &m_OldShaders.comp : &m_shaders.comp;
    CState* state = (pass == 0) ? m_pOldState : m_pState;

    lpDevice->SetVertexDeclaration(m_pMyVertDecl);
    lpDevice->SetVertexShader(m_fallbackShaders_vs.comp.ptr);
    lpDevice->SetPixelShader(si->ptr);

    ApplyShaderParams(&(si->params), si->CT, state);

    // Hurl the triangles at the video card.
    // We're going to un-index it, so that we don't stress any crappy (AHEM intel g33)
    //  drivers out there.  Not a big deal - only ~800 polys / 24kb of data.
    // If we're blending, we'll skip any polygon that is all alpha-blended out.
    // This also respects the MaxPrimCount limit of the video card.
    MYVERTEX tempv[1024 * 3];
    int primCount = (FCGSX - 2) * (FCGSY - 2) * 2;  // although, some might not be drawn!
    int max_prims_per_batch = min(GetCaps()->MaxPrimitiveCount, (sizeof(tempv) / sizeof(tempv[0])) / 3) - 4;
    int src_idx = 0;
    while (src_idx < primCount * 3) {
      int prims_queued = 0;
      int i = 0;
      while (prims_queued < max_prims_per_batch && src_idx < primCount * 3) {
        // copy 3 verts
        for (int j = 0; j < 3; j++)
          tempv[i++] = m_comp_verts[m_comp_indices[src_idx++]];
        if (bCullTiles) {
          DWORD d1 = (tempv[i - 3].Diffuse >> 24);
          DWORD d2 = (tempv[i - 2].Diffuse >> 24);
          DWORD d3 = (tempv[i - 1].Diffuse >> 24);
          bool bIsNeeded;
          if (nAlphaTestValue)
            bIsNeeded = ((d1 & d2 & d3) < 255);//(d1 < 255) || (d2 < 255) || (d3 < 255);
          else
            bIsNeeded = ((d1 | d2 | d3) > 0);//(d1 > 0) || (d2 > 0) || (d3 > 0);
          if (!bIsNeeded)
            i -= 3;
          else
            prims_queued++;
        }
        else
          prims_queued++;
      }
      if (prims_queued > 0)
        lpDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, prims_queued, tempv, sizeof(MYVERTEX));
    }
  }

  lpDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

  RestoreShaderParams();
}

void mdrop::Engine::ShowSongTitleAnim(int w, int h, float fProgress, int supertextIndex) {
  int i, x, y;

  int texIndex = supertextIndex;
  if (!m_dx12Title[texIndex].IsValid())
    return;

  if (!m_lpDX || !m_lpDX->m_commandList)
    return;

  auto* cmdList = m_lpDX->m_commandList.Get();

  // Set up DX12 rendering state
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  // Bind title texture
  D3D12_GPU_DESCRIPTOR_HANDLE titleSrvHandle = m_lpDX->GetBindingBlockGpuHandle(m_dx12Title[texIndex]);
  cmdList->SetGraphicsRootDescriptorTable(1, titleSrvHandle);

  SPRITEVERTEX v3[128];
  ZeroMemory(v3, sizeof(SPRITEVERTEX) * 128);

  float dx, dy;
  float currentX = m_supertexts[supertextIndex].fX;
  float currentY = m_supertexts[supertextIndex].fY;

  if (m_supertexts[supertextIndex].bIsSongTitle) {
    // positioning:
    float fSizeX = 50.0f / (float)m_supertexts[supertextIndex].nFontSizeUsed * powf(1.5f, m_supertexts[supertextIndex].fFontSize - 2.0f);
    float fSizeY = fSizeX * m_nTitleTexSizeY / (float)m_nTitleTexSizeX;// * m_nWidth/(float)m_nHeight;

    if (fSizeX > 0.88f) {
      fSizeY *= 0.88f / fSizeX;
      fSizeX = 0.88f;
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
    for (int i = 0; i < 128; i++) {
      v3[i].x *= scale;
      v3[i].y *= scale;
    }
  }
  else { // not song title

    // positioning:
    float fSizeX = (float)m_nTexSizeX / 1024.0f * 100.0f / (float)m_supertexts[supertextIndex].nFontSizeUsed * powf(1.033f, m_supertexts[supertextIndex].fFontSize - 50.0f);
    float fSizeY = fSizeX * m_nTitleTexSizeY / (float)m_nTitleTexSizeX;

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

      for (int i = 0; i < 128; i++) {
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

  for (int i = 0; i < 128; i++) {
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
  for (int i = 0; i < 128; i++)
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
    for (int i = 0; i < 128; ++i) {
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
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, boxVerts, 6, sizeof(WFVERTEX));
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

    for (int i = 0; i < 128; i++) {
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

    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, triVerts, triIdx, sizeof(SPRITEVERTEX));
  }
}
