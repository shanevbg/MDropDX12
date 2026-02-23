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

#ifndef GEISS_TEXT_DRAWING_MANAGER
#define GEISS_TEXT_DRAWING_MANAGER 1

// CTextManager renders text via D3D11on12 + Direct2D + DirectWrite.
// GPU-accelerated text rendering directly to the back buffer.

#include <d3d12.h>
#include <d3d11on12.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include "md_defines.h"
#include "AutoWide.h"
#include "shell_defines.h"  // NUM_BASIC_FONTS
#include "defines.h"        // NUM_EXTRA_FONTS

using Microsoft::WRL::ComPtr;

#define MAX_MSGS 4096
#define MAX_TEXT_FONTS (NUM_BASIC_FONTS + NUM_EXTRA_FONTS)

typedef struct {
  wchar_t* msg;       // points to some character in g_szMsgPool[2][].
  void*    pfont;     // encoded font index: (void*)(intptr_t)(fontIndex + 1)
  RECT rect;
  DWORD flags;
  DWORD color;
  DWORD bgColor;
  int added, deleted;        // temporary; used during DrawNow()
  void* prev_dark_box_ptr;   // temporary; used during DrawNow()
}
td_string;

class DXContext;  // forward declaration

class CTextManager {
public:
  CTextManager();
  ~CTextManager();

  // Legacy init — stores device pointer and initializes message queue.
  void Init(ID3D12Device* lpDevice, void* lpTextSurface, int bAdditive);
  void Finish();

  // D2D text rendering — call after DX12 device + fonts are ready.
  // pFontInfo: pointer to td_fontinfo array (from CPluginShell::m_fontinfo[])
  // nFonts: number of fonts
  void InitDX12(DXContext* lpDX, HFONT* pFonts, int nFonts, void* pFontInfo);
  void CleanupDX12();
  void OnResize(int newW, int newH);

  // Back buffer wrapping lifecycle — must bracket ResizeSwapChain
  void ReleaseBackBufferResources();
  void WrapBackBuffers();

  // Text drawing — queues entries; actual rendering happens in DrawNow().
  // DT_CALCRECT calls are handled immediately via DirectWrite measurement.
  int  DrawText(void* pFont, char* szText, RECT* pRect, DWORD flags, DWORD color, bool bBlackBox, DWORD boxColor = 0xFF000000);
  int  DrawText(void* pFont, char* szText, int len, RECT* pRect, DWORD flags, DWORD color, bool bBox, DWORD boxColor = 0xFF000000) {
    return DrawTextW(pFont, AutoWide(szText), pRect, flags, color, bBox, boxColor);
  };
  int  DrawTextW(void* pFont, wchar_t* szText, RECT* pRect, DWORD flags, DWORD color, bool bBlackBox, DWORD boxColor = 0xFF000000);
  int  DrawTextW(void* pFont, wchar_t* szText, int len, RECT* pRect, DWORD flags, DWORD color, bool bBox, DWORD boxColor = 0xFF000000) {
    return DrawTextW(pFont, szText, pRect, flags, color, bBox, boxColor);
  };
  void DrawBox(LPRECT pRect, DWORD boxColor);
  void DrawDarkBox(LPRECT pRect) { DrawBox(pRect, 0xFF000000); }
  void DrawNow();
  void ClearAll(); // automatically called @ end of DrawNow()

  // Decode font index from the pFont sentinel value.
  // Returns -1 if pFont is null or invalid.
  static int DecodeFontIndex(void* pFont);

protected:
  ID3D12Device*  m_lpDevice;
  void*          m_lpTextSurface;
  int            m_blit_additively;

  int       m_nMsg[2];
  td_string m_msg[2][MAX_MSGS];
  wchar_t* m_next_msg_start_ptr;
  int       m_b;

  // DX context reference
  DXContext*    m_lpDX;
  int           m_nFonts;
  void*         m_pFontInfo;    // borrowed pointer to CPluginShell::m_fontinfo[]

  // D3D11on12 interop
  ComPtr<ID3D11Device>         m_d3d11Device;
  ComPtr<ID3D11DeviceContext>  m_d3d11Context;
  ComPtr<ID3D11On12Device>     m_d3d11On12Device;

  // Direct2D
  ComPtr<ID2D1Factory3>        m_d2dFactory;
  ComPtr<ID2D1Device2>         m_d2dDevice;
  ComPtr<ID2D1DeviceContext2>  m_d2dContext;

  // DirectWrite
  ComPtr<IDWriteFactory5>      m_dwriteFactory;
  ComPtr<IDWriteTextFormat>    m_dwFormats[MAX_TEXT_FONTS];

  // Per-back-buffer wrapped resources
  ComPtr<ID3D11Resource>       m_wrappedBackBuffers[2]; // DXC_FRAME_COUNT
  ComPtr<ID2D1Bitmap1>         m_d2dRenderTargets[2];

  // Reusable brush (color set per-draw)
  ComPtr<ID2D1SolidColorBrush> m_brush;

  bool m_d2dReady;

public:
  bool IsD2DReady() const { return m_d2dReady; }
private:
  bool InitD2D();
  void CleanupD2D();
  void CreateDWriteFormats();
  int  MeasureTextDW(int fontIdx, const wchar_t* text, RECT* pRect, DWORD flags);
};

#endif
