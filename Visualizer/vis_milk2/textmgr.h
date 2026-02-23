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

// CTextManager renders text via GDI font atlas textures + DX12 sprite quads.
// Replaces the previous D3D11on12 + Direct2D + DirectWrite pipeline for stability.

#include <d3d12.h>
#include <wrl/client.h>
#include "md_defines.h"
#include "AutoWide.h"
#include "shell_defines.h"  // NUM_BASIC_FONTS
#include "defines.h"        // NUM_EXTRA_FONTS
#include "dx12helpers.h"    // DX12Texture

using Microsoft::WRL::ComPtr;

#define MAX_MSGS 4096
#define MAX_TEXT_FONTS (NUM_BASIC_FONTS + NUM_EXTRA_FONTS)

typedef struct {
  wchar_t* msg;       // points into g_szMsgPool
  void*    pfont;     // encoded font index: (void*)(intptr_t)(fontIndex + 1), null = box
  RECT rect;
  DWORD flags;
  DWORD color;
  DWORD bgColor;
}
td_string;

// Per-glyph metrics in the font atlas
struct GlyphInfo {
    float u0, v0, u1, v1;  // UV coordinates in atlas texture
    float advanceX;         // horizontal advance in pixels
    float bearingX;         // left-side bearing from pen position
    float glyphWidth;       // visible glyph width in pixels
    float glyphHeight;      // visible glyph height in pixels
};

// Font atlas: pre-rendered bitmap of all glyphs for one font
struct FontAtlas {
    DX12Texture texture;       // atlas texture on GPU
    GlyphInfo glyphs[256];     // indexed by character code (0x00-0xFF)
    float lineHeight;          // total line height in pixels
    float ascent;              // baseline distance from top of cell
    int cellWidth;             // cell width in atlas (pixels)
    int cellHeight;            // cell height in atlas (pixels)
    bool valid = false;
};

class DXContext;  // forward declaration

class CTextManager {
public:
  CTextManager();
  ~CTextManager();

  // Legacy init — stores device pointer and initializes message queue.
  void Init(ID3D12Device* lpDevice, void* lpTextSurface, int bAdditive);
  void Finish();

  // Font atlas creation — call after DX12 device + fonts are ready.
  void InitDX12(DXContext* lpDX, HFONT* pFonts, int nFonts, void* pFontInfo);
  void CleanupDX12();
  void OnResize(int newW, int newH);

  // No-ops: font atlases persist across back-buffer resize (no D3D11on12 wrapping needed)
  void ReleaseBackBufferResources() {}
  void WrapBackBuffers() {}

  // Text drawing — queues entries; actual rendering happens in DrawNow().
  // DT_CALCRECT calls are handled immediately via glyph metrics measurement.
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
  void ClearAll(); // automatically called at end of DrawNow()

  // Decode font index from the pFont sentinel value.
  // Returns -1 if pFont is null or invalid.
  static int DecodeFontIndex(void* pFont);

  bool IsD2DReady() const { return m_ready; }

protected:
  ID3D12Device*  m_lpDevice;
  void*          m_lpTextSurface;
  int            m_blit_additively;

  int       m_nMsg;
  td_string m_msg[MAX_MSGS];
  wchar_t*  m_next_msg_start_ptr;

  // DX context reference
  DXContext*    m_lpDX;
  int           m_nFonts;
  void*         m_pFontInfo;    // borrowed pointer to CPluginShell::m_fontinfo[]

  // Font atlases (one per font slot)
  FontAtlas     m_atlases[MAX_TEXT_FONTS];
  bool          m_ready;

private:
  bool BuildFontAtlas(int fontIdx);
  int  MeasureText(int fontIdx, const wchar_t* text, RECT* pRect, DWORD flags);
  float MeasureStringWidth(int fontIdx, const wchar_t* text, int len = -1);
};

#endif
