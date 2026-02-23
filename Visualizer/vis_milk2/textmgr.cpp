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

// CTextManager: GDI font atlas + DX12 sprite quad text rendering.
// At startup, renders all printable characters to bitmap atlases using GDI,
// then uploads them as DX12 textures. Each frame, builds SPRITEVERTEX quads
// for queued text and draws with the existing DX12 pipeline.

#include "textmgr.h"
#include "dxcontext.h"
#include "pluginshell.h"  // td_fontinfo
#include "support.h"      // SPRITEVERTEX, WFVERTEX
#include "dx12pipeline.h" // PSO enums
#include <cmath>
#include <vector>

#define MAX_MSG_CHARS (65536*2)
static wchar_t g_szMsgPool[MAX_MSG_CHARS];

// Atlas grid layout: 16 columns × 15 rows = 240 cells (covers 0x20-0xFF = 224 chars)
static const int ATLAS_COLS = 16;
static const int ATLAS_ROWS = 15;
static const int ATLAS_FIRST_CHAR = 0x20;
static const int ATLAS_LAST_CHAR  = 0xFF;

CTextManager::CTextManager()
  : m_lpDevice(nullptr)
  , m_lpTextSurface(nullptr)
  , m_blit_additively(0)
  , m_lpDX(nullptr)
  , m_nFonts(0)
  , m_pFontInfo(nullptr)
  , m_ready(false)
{
  m_nMsg = 0;
  m_next_msg_start_ptr = g_szMsgPool;
}

CTextManager::~CTextManager() {
  CleanupDX12();
}

void CTextManager::Init(ID3D12Device* lpDevice, void* lpTextSurface, int bAdditive) {
  m_lpDevice        = lpDevice;
  m_lpTextSurface   = lpTextSurface;
  m_blit_additively = bAdditive;

  m_nMsg = 0;
  m_next_msg_start_ptr = g_szMsgPool;
}

void CTextManager::Finish() {
  CleanupDX12();
}

int CTextManager::DecodeFontIndex(void* pFont) {
  if (!pFont) return -1;
  int idx = (int)(intptr_t)pFont - 1;
  return idx;
}

void CTextManager::InitDX12(DXContext* lpDX, HFONT* pFonts, int nFonts, void* pFontInfo) {
  m_lpDX      = lpDX;
  m_nFonts    = nFonts;
  m_pFontInfo = pFontInfo;

  if (!lpDX) return;

  // Build a font atlas for each configured font
  for (int i = 0; i < nFonts && i < MAX_TEXT_FONTS; i++) {
    if (!BuildFontAtlas(i)) {
      char buf[128];
      sprintf(buf, "CTextManager: BuildFontAtlas(%d) failed\n", i);
      OutputDebugStringA(buf);
    }
  }

  m_ready = true;
  OutputDebugStringA("CTextManager: Font atlas text rendering initialized\n");
}

bool CTextManager::BuildFontAtlas(int fontIdx) {
  if (!m_lpDX || !m_pFontInfo || fontIdx < 0 || fontIdx >= m_nFonts)
    return false;

  td_fontinfo* fonts = (td_fontinfo*)m_pFontInfo;
  td_fontinfo& fi = fonts[fontIdx];

  // 1. Create GDI font matching the configured settings
  HFONT hFont = CreateFontW(
    fi.nSize, 0, 0, 0,
    fi.bBold ? FW_BOLD : FW_NORMAL,
    fi.bItalic, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
    fi.szFace);
  if (!hFont) return false;

  // 2. Create memory DC and get font metrics
  HDC memDC = CreateCompatibleDC(nullptr);
  HGDIOBJ oldFont = SelectObject(memDC, hFont);

  TEXTMETRICW tm;
  GetTextMetricsW(memDC, &tm);

  // Cell dimensions: enough for any character including overhang
  int cellW = tm.tmMaxCharWidth + 4;  // +4 padding for ClearType/overhang
  int cellH = tm.tmHeight;

  int atlasW = ATLAS_COLS * cellW;
  int atlasH = ATLAS_ROWS * cellH;

  // Round up to multiple of 4 for DX12 texture alignment
  atlasW = (atlasW + 3) & ~3;
  atlasH = (atlasH + 3) & ~3;

  // 3. Create 32-bit top-down DIB
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth       = atlasW;
  bmi.bmiHeader.biHeight      = -atlasH; // top-down
  bmi.bmiHeader.biPlanes      = 1;
  bmi.bmiHeader.biBitCount    = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* dibBits = nullptr;
  HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
  if (!hBmp || !dibBits) {
    SelectObject(memDC, oldFont);
    DeleteObject(hFont);
    DeleteDC(memDC);
    return false;
  }
  HGDIOBJ oldBmp = SelectObject(memDC, hBmp);

  // Clear to black (CreateDIBSection zeros memory)
  SetBkMode(memDC, TRANSPARENT);
  SetTextColor(memDC, RGB(255, 255, 255));

  // 4. Get ABC widths for each character
  ABC abcWidths[256] = {};
  if (!GetCharABCWidthsW(memDC, ATLAS_FIRST_CHAR, ATLAS_LAST_CHAR, &abcWidths[ATLAS_FIRST_CHAR])) {
    // Fallback: use GetCharWidth32 for non-TrueType fonts
    INT charWidths[256] = {};
    GetCharWidth32W(memDC, ATLAS_FIRST_CHAR, ATLAS_LAST_CHAR, &charWidths[ATLAS_FIRST_CHAR]);
    for (int c = ATLAS_FIRST_CHAR; c <= ATLAS_LAST_CHAR; c++) {
      abcWidths[c].abcA = 0;
      abcWidths[c].abcB = charWidths[c];
      abcWidths[c].abcC = 0;
    }
  }

  // 5. Render each character to the atlas grid
  FontAtlas& atlas = m_atlases[fontIdx];
  memset(atlas.glyphs, 0, sizeof(atlas.glyphs));
  atlas.lineHeight = (float)tm.tmHeight;
  atlas.ascent     = (float)tm.tmAscent;
  atlas.cellWidth  = cellW;
  atlas.cellHeight = cellH;

  for (int c = ATLAS_FIRST_CHAR; c <= ATLAS_LAST_CHAR; c++) {
    int idx = c - ATLAS_FIRST_CHAR;
    int col = idx % ATLAS_COLS;
    int row = idx / ATLAS_COLS;

    int x = col * cellW;
    int y = row * cellH;

    // Render character at cell origin (GDI handles bearings)
    wchar_t ch = (wchar_t)c;
    TextOutW(memDC, x, y, &ch, 1);

    // Calculate advance width
    float advance = (float)(abcWidths[c].abcA + (int)abcWidths[c].abcB + abcWidths[c].abcC);
    if (advance <= 0) advance = (float)cellW * 0.5f;

    // Store glyph metrics with UV coordinates
    GlyphInfo& g = atlas.glyphs[c];
    g.u0       = (float)x / (float)atlasW;
    g.v0       = (float)y / (float)atlasH;
    g.u1       = (float)(x + cellW) / (float)atlasW;
    g.v1       = (float)(y + cellH) / (float)atlasH;
    g.advanceX = advance;
    g.bearingX = (float)abcWidths[c].abcA;
    g.glyphWidth  = (float)cellW;
    g.glyphHeight = (float)cellH;
  }

  // Set up space character if not already valid
  if (atlas.glyphs[' '].advanceX <= 0)
    atlas.glyphs[' '].advanceX = (float)tm.tmAveCharWidth;

  SelectObject(memDC, oldFont);
  SelectObject(memDC, oldBmp);
  DeleteObject(hFont);
  DeleteDC(memDC);

  // 6. Post-process: convert GDI grayscale to premultiplied alpha
  // GDI renders white text (R=G=B=intensity, A=0) on black (all zeros).
  // Convert to premultiplied alpha: BGRA(intensity, intensity, intensity, intensity)
  BYTE* pixels = (BYTE*)dibBits;
  for (int py = 0; py < atlasH; py++) {
    for (int px = 0; px < atlasW; px++) {
      int i = (py * atlasW + px) * 4;
      BYTE b = pixels[i + 0];
      BYTE g = pixels[i + 1];
      BYTE r = pixels[i + 2];
      BYTE intensity = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
      pixels[i + 0] = intensity; // B (premultiplied)
      pixels[i + 1] = intensity; // G (premultiplied)
      pixels[i + 2] = intensity; // R (premultiplied)
      pixels[i + 3] = intensity; // A = coverage
    }
  }

  // 7. Upload atlas to DX12 texture
  atlas.texture = m_lpDX->CreateTextureFromPixels(
    dibBits, (UINT)atlasW, (UINT)atlasH,
    (UINT)(atlasW * 4), DXGI_FORMAT_B8G8R8A8_UNORM);

  DeleteObject(hBmp);

  if (!atlas.texture.IsValid()) {
    char buf[128];
    sprintf(buf, "CTextManager: CreateTextureFromPixels failed for font %d\n", fontIdx);
    OutputDebugStringA(buf);
    return false;
  }

  // 8. Create binding block (16 SRV slots: slot 0 = atlas, slots 1-15 = null)
  m_lpDX->CreateBindingBlockForTexture(atlas.texture);

  atlas.valid = true;

  char buf[256];
  sprintf(buf, "CTextManager: Font %d atlas built: %dx%d cells=%dx%d lineH=%.0f\n",
    fontIdx, atlasW, atlasH, cellW, cellH, atlas.lineHeight);
  OutputDebugStringA(buf);

  return true;
}

void CTextManager::CleanupDX12() {
  m_ready = false;
  for (int i = 0; i < MAX_TEXT_FONTS; i++) {
    m_atlases[i].texture.Reset();
    m_atlases[i].valid = false;
  }
  m_lpDX = nullptr;
  m_nFonts = 0;
  m_pFontInfo = nullptr;
}

void CTextManager::OnResize(int newW, int newH) {
  // Font atlases are independent of back buffer size — nothing to do.
  (void)newW; (void)newH;
}

void CTextManager::ClearAll() {
  m_nMsg = 0;
  m_next_msg_start_ptr = g_szMsgPool;
}

void CTextManager::DrawBox(LPRECT pRect, DWORD boxColor) {
  if (!pRect)
    return;

  if ((m_nMsg < MAX_MSGS) &&
    (size_t)((DWORD_PTR)m_next_msg_start_ptr - (DWORD_PTR)g_szMsgPool) + 0 + 1 < MAX_MSG_CHARS) {
    *m_next_msg_start_ptr = 0;

    m_msg[m_nMsg].msg   = m_next_msg_start_ptr;
    m_msg[m_nMsg].pfont = nullptr;
    m_msg[m_nMsg].rect  = *pRect;
    m_msg[m_nMsg].flags = 0;
    m_msg[m_nMsg].color = 0xFFFFFFFF;
    m_msg[m_nMsg].bgColor = boxColor;
    m_nMsg++;
    m_next_msg_start_ptr += 1;
  }
}

int CTextManager::DrawText(void* pFont, char* szText, RECT* pRect, DWORD flags, DWORD color, bool bBox, DWORD boxColor) {
  return DrawTextW(pFont, AutoWide(szText), pRect, flags, color, bBox, boxColor);
}

int CTextManager::DrawTextW(void* pFont, wchar_t* szText, RECT* pRect, DWORD flags, DWORD color, bool bBox, DWORD boxColor) {
  if (!pRect) return 0;

  // For DT_CALCRECT: measure text immediately and return.
  if (flags & DT_CALCRECT) {
    int fontIdx = DecodeFontIndex(pFont);
    return MeasureText(fontIdx, szText, pRect, flags);
  }

  // Queue the text entry for rendering in DrawNow()
  if (!szText || !szText[0]) return 0;
  int len = (int)wcslen(szText);

  if ((m_nMsg < MAX_MSGS) &&
      (size_t)((DWORD_PTR)m_next_msg_start_ptr - (DWORD_PTR)g_szMsgPool) + len + 1 < MAX_MSG_CHARS) {
    wcscpy(m_next_msg_start_ptr, szText);

    td_string& entry = m_msg[m_nMsg];
    entry.msg     = m_next_msg_start_ptr;
    entry.pfont   = pFont;
    entry.rect    = *pRect;
    entry.flags   = flags;
    entry.color   = color;
    entry.bgColor = boxColor;
    m_nMsg++;
    m_next_msg_start_ptr += len + 1;

    // If a dark box was requested, queue a box entry after this text
    if (bBox) {
      DrawBox(pRect, boxColor);
    }
  }

  // Measure actual text height so callers can advance layout.
  int fontIdx = DecodeFontIndex(pFont);
  RECT rc = *pRect;
  return MeasureText(fontIdx, szText, &rc, (flags | DT_CALCRECT) & ~DT_END_ELLIPSIS);
}

float CTextManager::MeasureStringWidth(int fontIdx, const wchar_t* text, int len) {
  if (!text || fontIdx < 0 || fontIdx >= m_nFonts || !m_atlases[fontIdx].valid)
    return 0;

  if (len < 0) len = (int)wcslen(text);

  const FontAtlas& atlas = m_atlases[fontIdx];
  float width = 0;
  for (int i = 0; i < len; i++) {
    int c = (int)text[i];
    if (c >= ATLAS_FIRST_CHAR && c <= ATLAS_LAST_CHAR)
      width += atlas.glyphs[c].advanceX;
    else if (c == '\t')
      width += atlas.glyphs[' '].advanceX * 8; // tab = 8 spaces
    else if (c > ATLAS_LAST_CHAR)
      width += atlas.glyphs['?'].advanceX; // unknown Unicode → '?'
    // skip control chars
  }
  return width;
}

int CTextManager::MeasureText(int fontIdx, const wchar_t* text, RECT* pRect, DWORD flags) {
  if (!pRect || fontIdx < 0 || fontIdx >= m_nFonts || !m_atlases[fontIdx].valid)
    return 0;

  const FontAtlas& atlas = m_atlases[fontIdx];
  const wchar_t* str = text ? text : L"";
  int strLen = (int)wcslen(str);

  if (flags & DT_SINGLELINE) {
    // Single line: width = sum of advances, height = lineHeight
    float w = MeasureStringWidth(fontIdx, str, strLen);
    pRect->right  = pRect->left + (LONG)ceilf(w);
    pRect->bottom = pRect->top  + (LONG)ceilf(atlas.lineHeight);
    return (int)ceilf(atlas.lineHeight);
  }

  // Multi-line: split on '\n', find max width and total height
  float maxWidth = 0;
  float totalHeight = 0;
  float maxLayoutWidth = (float)(pRect->right - pRect->left);
  if (maxLayoutWidth <= 0) maxLayoutWidth = 100000.f;

  const wchar_t* lineStart = str;
  for (int i = 0; i <= strLen; i++) {
    if (i == strLen || str[i] == '\n') {
      int lineLen = (int)(str + i - lineStart);
      float lineW = MeasureStringWidth(fontIdx, lineStart, lineLen);

      // Word wrap: if line exceeds layout width, approximate wrapped height
      if (lineW > maxLayoutWidth && maxLayoutWidth > 0) {
        int numWraps = (int)ceilf(lineW / maxLayoutWidth);
        totalHeight += atlas.lineHeight * numWraps;
      } else {
        totalHeight += atlas.lineHeight;
      }
      if (lineW > maxWidth) maxWidth = lineW;
      lineStart = str + i + 1;
    }
  }

  pRect->right  = pRect->left + (LONG)ceilf(maxWidth);
  pRect->bottom = pRect->top  + (LONG)ceilf(totalHeight);
  return (int)ceilf(totalHeight);
}

void CTextManager::DrawNow() {
  if (!m_ready || !m_lpDX || m_nMsg == 0) {
    ClearAll();
    return;
  }

  auto* cmdList = m_lpDX->m_commandList.Get();
  if (!cmdList) { ClearAll(); return; }

  int cw = m_lpDX->m_client_width;
  int ch = m_lpDX->m_client_height;
  if (cw <= 0 || ch <= 0) { ClearAll(); return; }

  // Ensure root signature and descriptor heaps are set for built-in PSOs
  ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
  cmdList->SetDescriptorHeaps(1, heaps);
  cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());

  float invW = 1.0f / (float)cw;
  float invH = 1.0f / (float)ch;

  // Pass 1: Draw all dark boxes (untextured, alpha-blended)
  {
    // Count boxes
    int boxCount = 0;
    for (int i = 0; i < m_nMsg; i++) {
      if (!m_msg[i].pfont) boxCount++;
    }

    if (boxCount > 0) {
      std::vector<WFVERTEX> boxVerts;
      boxVerts.reserve(boxCount * 6);

      for (int i = 0; i < m_nMsg; i++) {
        if (m_msg[i].pfont) continue;

        RECT& r = m_msg[i].rect;
        DWORD col = m_msg[i].bgColor;

        // Convert pixel rect to NDC (-1..+1)
        float x0 = (float)r.left   * invW * 2.0f - 1.0f;
        float x1 = (float)r.right  * invW * 2.0f - 1.0f;
        float y0 = 1.0f - (float)r.top    * invH * 2.0f;
        float y1 = 1.0f - (float)r.bottom * invH * 2.0f;

        // Two triangles for the box quad
        boxVerts.push_back({ x0, y0, 0, col });
        boxVerts.push_back({ x1, y0, 0, col });
        boxVerts.push_back({ x0, y1, 0, col });
        boxVerts.push_back({ x0, y1, 0, col });
        boxVerts.push_back({ x1, y0, 0, col });
        boxVerts.push_back({ x1, y1, 0, col });
      }

      cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_ALPHABLEND_WFVERTEX].Get());
      m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                           boxVerts.data(), (UINT)boxVerts.size(), sizeof(WFVERTEX));
    }
  }

  // Pass 2: Draw text, batched by font atlas
  for (int fontIdx = 0; fontIdx < m_nFonts; fontIdx++) {
    if (!m_atlases[fontIdx].valid) continue;

    // Check if any text uses this font
    bool hasText = false;
    for (int i = 0; i < m_nMsg; i++) {
      if (!m_msg[i].pfont) continue;
      if (DecodeFontIndex(m_msg[i].pfont) == fontIdx) { hasText = true; break; }
    }
    if (!hasText) continue;

    FontAtlas& atlas = m_atlases[fontIdx];

    // Bind atlas texture and set PSO (premultiplied alpha blend)
    cmdList->SetPipelineState(m_lpDX->m_PSOs[PSO_PREMULALPHA_SPRITEVERTEX].Get());
    cmdList->SetGraphicsRootDescriptorTable(1,
      m_lpDX->GetBindingBlockGpuHandle(atlas.texture));

    std::vector<SPRITEVERTEX> textVerts;
    textVerts.reserve(2048);

    for (int i = 0; i < m_nMsg; i++) {
      if (!m_msg[i].pfont) continue;
      if (DecodeFontIndex(m_msg[i].pfont) != fontIdx) continue;

      td_string& entry = m_msg[i];
      DWORD color = entry.color;
      const wchar_t* text = entry.msg;
      int textLen = (int)wcslen(text);
      if (textLen == 0) continue;

      float rectW = (float)(entry.rect.right - entry.rect.left);

      // Calculate horizontal alignment offset
      float textW = MeasureStringWidth(fontIdx, text, textLen);
      float startX = (float)entry.rect.left;
      if (entry.flags & DT_CENTER)
        startX = entry.rect.left + (rectW - textW) * 0.5f;
      else if (entry.flags & DT_RIGHT)
        startX = (float)entry.rect.right - textW;

      float penX = startX;
      float penY = (float)entry.rect.top;
      float clipRight = (float)entry.rect.right;

      // Handle DT_END_ELLIPSIS: truncate and append "..." if text exceeds rect
      int visibleChars = textLen;
      bool useEllipsis = false;
      if ((entry.flags & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS)) && textW > rectW && rectW > 0) {
        float ellipsisW = MeasureStringWidth(fontIdx, L"...", 3);
        float accum = 0;
        visibleChars = 0;
        for (int j = 0; j < textLen; j++) {
          int c = (int)text[j];
          float charW = 0;
          if (c >= ATLAS_FIRST_CHAR && c <= ATLAS_LAST_CHAR)
            charW = atlas.glyphs[c].advanceX;
          else if (c > ATLAS_LAST_CHAR)
            charW = atlas.glyphs['?'].advanceX;
          if (accum + charW + ellipsisW > rectW) break;
          accum += charW;
          visibleChars++;
        }
        useEllipsis = true;
      }

      // Lambda to emit a character quad
      auto emitChar = [&](int c) {
        if (c < ATLAS_FIRST_CHAR || c > ATLAS_LAST_CHAR) {
          if (c > ATLAS_LAST_CHAR) c = '?';
          else return; // skip control chars
        }
        const GlyphInfo& g = atlas.glyphs[c];

        // Character quad in pixel space
        float x0px = penX;
        float y0px = penY;
        float x1px = penX + g.glyphWidth;
        float y1px = penY + g.glyphHeight;

        // Skip if entirely outside the clipping rect
        if ((entry.flags & DT_SINGLELINE) && x0px >= clipRight) {
          penX += g.advanceX;
          return;
        }

        // Convert to NDC
        float x0 = x0px * invW * 2.0f - 1.0f;
        float x1 = x1px * invW * 2.0f - 1.0f;
        float y0 = 1.0f - y0px * invH * 2.0f;
        float y1 = 1.0f - y1px * invH * 2.0f;

        textVerts.push_back({ x0, y0, 0, color, g.u0, g.v0 });
        textVerts.push_back({ x1, y0, 0, color, g.u1, g.v0 });
        textVerts.push_back({ x0, y1, 0, color, g.u0, g.v1 });
        textVerts.push_back({ x0, y1, 0, color, g.u0, g.v1 });
        textVerts.push_back({ x1, y0, 0, color, g.u1, g.v0 });
        textVerts.push_back({ x1, y1, 0, color, g.u1, g.v1 });

        penX += g.advanceX;
      };

      // Render visible characters
      for (int j = 0; j < visibleChars; j++) {
        int c = (int)text[j];
        if (c == '\t') {
          penX += atlas.glyphs[' '].advanceX * 8;
          continue;
        }
        emitChar(c);
      }

      // Append ellipsis if truncated
      if (useEllipsis) {
        emitChar('.');
        emitChar('.');
        emitChar('.');
      }
    }

    // Issue single batched draw for all text using this font
    if (!textVerts.empty()) {
      m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                           textVerts.data(), (UINT)textVerts.size(), sizeof(SPRITEVERTEX));
    }
  }

  ClearAll();
}
