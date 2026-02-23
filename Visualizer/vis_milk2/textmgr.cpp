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

// CTextManager: D3D11on12 + Direct2D + DirectWrite text rendering.
// Replaces GDI→DIB→DX12 pipeline with GPU-accelerated text.

#include "textmgr.h"
#include "dxcontext.h"
#include "pluginshell.h"  // td_fontinfo
#include "support.h"
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#define MAX_MSG_CHARS (65536*2)
wchar_t g_szMsgPool[2][MAX_MSG_CHARS];

// Convert D3DCOLOR (0xAARRGGBB) to D2D1_COLOR_F
static D2D1_COLOR_F D3DColorToD2D(DWORD d3dColor) {
  float a = ((d3dColor >> 24) & 0xFF) / 255.0f;
  float r = ((d3dColor >> 16) & 0xFF) / 255.0f;
  float g = ((d3dColor >>  8) & 0xFF) / 255.0f;
  float b = ((d3dColor >>  0) & 0xFF) / 255.0f;
  return D2D1::ColorF(r, g, b, a);
}

// Map GDI DT_ flags to DirectWrite alignment on a text format
static void ApplyDWriteAlignment(IDWriteTextFormat* fmt, DWORD dtFlags) {
  // Horizontal alignment
  if (dtFlags & DT_CENTER)
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  else if (dtFlags & DT_RIGHT)
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
  else
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

  // Word wrapping
  if (dtFlags & DT_SINGLELINE)
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  else
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
}

CTextManager::CTextManager()
  : m_lpDevice(nullptr)
  , m_lpTextSurface(nullptr)
  , m_blit_additively(0)
  , m_lpDX(nullptr)
  , m_nFonts(0)
  , m_pFontInfo(nullptr)
  , m_d2dReady(false)
{
  m_b = 0;
  m_nMsg[0] = 0;
  m_nMsg[1] = 0;
  m_next_msg_start_ptr = g_szMsgPool[m_b];
}

CTextManager::~CTextManager() {
  CleanupDX12();
}

void CTextManager::Init(ID3D12Device* lpDevice, void* lpTextSurface, int bAdditive) {
  m_lpDevice        = lpDevice;
  m_lpTextSurface   = lpTextSurface;
  m_blit_additively = bAdditive;

  m_b = 0;
  m_nMsg[0] = 0;
  m_nMsg[1] = 0;
  m_next_msg_start_ptr = g_szMsgPool[m_b];
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
  InitD2D();
}

bool CTextManager::InitD2D() {
  if (!m_lpDX || !m_lpDX->m_device || !m_lpDX->m_commandQueue) return false;

  // 1. Create D3D11On12 device (shares DX12 device + command queue)
  UINT d3d11Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  d3d11Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  IUnknown* cmdQueues[] = { m_lpDX->m_commandQueue.Get() };
  ComPtr<ID3D11Device> d3d11Device;
  HRESULT hr = D3D11On12CreateDevice(
    m_lpDX->m_device.Get(),
    d3d11Flags,
    nullptr, 0,          // feature levels (default)
    cmdQueues, 1,        // command queue
    0,                   // node mask
    &d3d11Device,
    &m_d3d11Context,
    nullptr);
  if (FAILED(hr)) {
    char buf[128];
    sprintf(buf, "CTextManager: D3D11On12CreateDevice FAILED hr=0x%08X\n", (unsigned)hr);
    OutputDebugStringA(buf);
    return false;
  }

  hr = d3d11Device.As(&m_d3d11Device);
  hr = d3d11Device.As(&m_d3d11On12Device);
  if (!m_d3d11On12Device) {
    OutputDebugStringA("CTextManager: QueryInterface ID3D11On12Device FAILED\n");
    return false;
  }

  // 2. Create D2D factory
  D2D1_FACTORY_OPTIONS d2dOpts = {};
#ifdef _DEBUG
  d2dOpts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
    __uuidof(ID2D1Factory3), &d2dOpts,
    reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()));
  if (FAILED(hr)) {
    OutputDebugStringA("CTextManager: D2D1CreateFactory FAILED\n");
    return false;
  }

  // 3. Create D2D device + context from DXGI device
  ComPtr<IDXGIDevice> dxgiDevice;
  m_d3d11Device.As(&dxgiDevice);

  ComPtr<ID2D1Device> d2dDevice;
  hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
  if (FAILED(hr)) {
    OutputDebugStringA("CTextManager: D2D CreateDevice FAILED\n");
    return false;
  }
  d2dDevice.As(&m_d2dDevice);

  ComPtr<ID2D1DeviceContext> d2dCtx;
  hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx);
  if (FAILED(hr)) {
    OutputDebugStringA("CTextManager: D2D CreateDeviceContext FAILED\n");
    return false;
  }
  d2dCtx.As(&m_d2dContext);

  // 4. Create DirectWrite factory
  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
    __uuidof(IDWriteFactory5),
    reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
  if (FAILED(hr)) {
    // Try base factory version
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
      __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    if (FAILED(hr)) {
      OutputDebugStringA("CTextManager: DWriteCreateFactory FAILED\n");
      return false;
    }
  }

  // 5. Create reusable brush
  m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &m_brush);

  // 6. Create DirectWrite text formats from font info
  CreateDWriteFormats();

  // 7. Wrap back buffers for D2D rendering
  WrapBackBuffers();

  m_d2dReady = true;
  OutputDebugStringA("CTextManager: D2D initialized successfully\n");
  return true;
}

void CTextManager::CreateDWriteFormats() {
  if (!m_dwriteFactory || !m_pFontInfo) return;

  td_fontinfo* fontInfo = (td_fontinfo*)m_pFontInfo;

  for (int i = 0; i < m_nFonts && i < MAX_TEXT_FONTS; i++) {
    float fontSize = (float)abs(fontInfo[i].nSize);
    if (fontSize <= 0) fontSize = 16.0f;

    DWRITE_FONT_WEIGHT weight = fontInfo[i].bBold
      ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = fontInfo[i].bItalic
      ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;

    HRESULT hr = m_dwriteFactory->CreateTextFormat(
      fontInfo[i].szFace,
      nullptr,
      weight,
      style,
      DWRITE_FONT_STRETCH_NORMAL,
      fontSize,
      L"en-us",
      &m_dwFormats[i]);

    if (FAILED(hr)) {
      char buf[256];
      sprintf(buf, "CTextManager: CreateTextFormat failed for font %d hr=0x%08X\n", i, (unsigned)hr);
      OutputDebugStringA(buf);
    }
  }
}

void CTextManager::WrapBackBuffers() {
  ReleaseBackBufferResources();
  if (!m_d3d11On12Device || !m_lpDX) return;

  for (UINT i = 0; i < 2; i++) {
    if (!m_lpDX->m_renderTargets[i]) continue;

    D3D11_RESOURCE_FLAGS d3d11Flags = {};
    d3d11Flags.BindFlags = D3D11_BIND_RENDER_TARGET;

    HRESULT hr = m_d3d11On12Device->CreateWrappedResource(
      m_lpDX->m_renderTargets[i].Get(),
      &d3d11Flags,
      D3D12_RESOURCE_STATE_RENDER_TARGET,  // state on acquire
      D3D12_RESOURCE_STATE_PRESENT,        // state on release
      IID_PPV_ARGS(&m_wrappedBackBuffers[i]));

    if (FAILED(hr)) {
      char buf[128];
      sprintf(buf, "CTextManager: CreateWrappedResource[%u] FAILED hr=0x%08X\n", i, (unsigned)hr);
      OutputDebugStringA(buf);
      continue;
    }

    ComPtr<IDXGISurface> surface;
    m_wrappedBackBuffers[i].As(&surface);
    if (!surface) continue;

    D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    // Set DPI to 96 so DIPs = pixels (font sizes match CreateFontW pixel heights)
    bmpProps.dpiX = 96.0f;
    bmpProps.dpiY = 96.0f;

    hr = m_d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &bmpProps,
      &m_d2dRenderTargets[i]);
    if (FAILED(hr)) {
      char buf[128];
      sprintf(buf, "CTextManager: CreateBitmapFromDxgiSurface[%u] FAILED hr=0x%08X\n", i, (unsigned)hr);
      OutputDebugStringA(buf);
    }
  }
}

void CTextManager::ReleaseBackBufferResources() {
  if (m_d2dContext) m_d2dContext->SetTarget(nullptr);
  for (UINT i = 0; i < 2; i++) {
    m_d2dRenderTargets[i].Reset();
    m_wrappedBackBuffers[i].Reset();
  }
  if (m_d3d11Context) m_d3d11Context->Flush();
}

void CTextManager::CleanupD2D() {
  m_d2dReady = false;
  ReleaseBackBufferResources();
  m_brush.Reset();
  for (int i = 0; i < MAX_TEXT_FONTS; i++)
    m_dwFormats[i].Reset();
  m_dwriteFactory.Reset();
  m_d2dContext.Reset();
  m_d2dDevice.Reset();
  m_d2dFactory.Reset();
  m_d3d11On12Device.Reset();
  m_d3d11Context.Reset();
  m_d3d11Device.Reset();
}

void CTextManager::CleanupDX12() {
  CleanupD2D();
  m_lpDX = nullptr;
  m_nFonts = 0;
  m_pFontInfo = nullptr;
}

void CTextManager::OnResize(int newW, int newH) {
  if (newW <= 0 || newH <= 0) return;
  // Back buffers are about to be resized — release wrapped resources.
  // Caller must call WrapBackBuffers() after ResizeSwapChain completes.
  ReleaseBackBufferResources();
}

void CTextManager::ClearAll() {
  m_nMsg[m_b] = 0;
  m_next_msg_start_ptr = g_szMsgPool[m_b];
}

void CTextManager::DrawBox(LPRECT pRect, DWORD boxColor) {
  if (!pRect)
    return;

  if ((m_nMsg[m_b] < MAX_MSGS) &&
    (size_t)((DWORD_PTR)m_next_msg_start_ptr - (DWORD_PTR)g_szMsgPool[m_b]) + 0 + 1 < MAX_MSG_CHARS) {
    *m_next_msg_start_ptr = 0;

    m_msg[m_b][m_nMsg[m_b]].msg   = m_next_msg_start_ptr;
    m_msg[m_b][m_nMsg[m_b]].pfont = nullptr;
    m_msg[m_b][m_nMsg[m_b]].rect  = *pRect;
    m_msg[m_b][m_nMsg[m_b]].flags = 0;
    m_msg[m_b][m_nMsg[m_b]].color = 0xFFFFFFFF;
    m_msg[m_b][m_nMsg[m_b]].bgColor = boxColor;
    m_nMsg[m_b]++;
    m_next_msg_start_ptr += 1;
  }
}

int CTextManager::DrawText(void* pFont, char* szText, RECT* pRect, DWORD flags, DWORD color, bool bBox, DWORD boxColor) {
  return DrawTextW(pFont, AutoWide(szText), pRect, flags, color, bBox, boxColor);
}

int CTextManager::DrawTextW(void* pFont, wchar_t* szText, RECT* pRect, DWORD flags, DWORD color, bool bBox, DWORD boxColor) {
  if (!pRect) return 0;

  // For DT_CALCRECT: measure text immediately via DirectWrite and return.
  if (flags & DT_CALCRECT) {
    int fontIdx = DecodeFontIndex(pFont);
    return MeasureTextDW(fontIdx, szText, pRect, flags);
  }

  // Queue the text entry for rendering in DrawNow()
  if (!szText || !szText[0]) return 0;
  int len = (int)wcslen(szText);

  if ((m_nMsg[m_b] < MAX_MSGS) &&
      (size_t)((DWORD_PTR)m_next_msg_start_ptr - (DWORD_PTR)g_szMsgPool[m_b]) + len + 1 < MAX_MSG_CHARS) {
    wcscpy(m_next_msg_start_ptr, szText);

    td_string& entry = m_msg[m_b][m_nMsg[m_b]];
    entry.msg     = m_next_msg_start_ptr;
    entry.pfont   = pFont;
    entry.rect    = *pRect;
    entry.flags   = flags;
    entry.color   = color;
    entry.bgColor = boxColor;
    m_nMsg[m_b]++;
    m_next_msg_start_ptr += len + 1;

    // If a dark box was requested, queue a box entry before this text
    if (bBox) {
      DrawBox(pRect, boxColor);
    }
  }

  // Measure actual text height via DirectWrite so callers can advance layout.
  int fontIdx = DecodeFontIndex(pFont);
  RECT rc = *pRect;
  return MeasureTextDW(fontIdx, szText, &rc, (flags | DT_CALCRECT) & ~DT_END_ELLIPSIS);
}

int CTextManager::MeasureTextDW(int fontIdx, const wchar_t* text, RECT* pRect, DWORD flags) {
  if (!m_dwriteFactory || fontIdx < 0 || fontIdx >= m_nFonts || !m_dwFormats[fontIdx] || !pRect)
    return 0;

  float maxWidth = (float)(pRect->right - pRect->left);
  if (maxWidth <= 0) maxWidth = 100000.f;
  float maxHeight = (float)(pRect->bottom - pRect->top);
  if (maxHeight <= 0) maxHeight = 100000.f;

  const wchar_t* str = text ? text : L"";
  UINT32 strLen = (UINT32)wcslen(str);

  IDWriteTextLayout* layoutRaw = nullptr;
  HRESULT hr = m_dwriteFactory->CreateTextLayout(
    str, strLen, m_dwFormats[fontIdx].Get(), maxWidth, maxHeight, &layoutRaw);
  ComPtr<IDWriteTextLayout> layout;
  layout.Attach(layoutRaw);
  if (FAILED(hr) || !layout) return 0;

  // Apply word wrapping based on DT_ flags
  if (flags & DT_SINGLELINE)
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  else
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

  DWRITE_TEXT_METRICS metrics;
  layout->GetMetrics(&metrics);

  pRect->right = pRect->left + (LONG)ceilf(metrics.widthIncludingTrailingWhitespace);
  pRect->bottom = pRect->top + (LONG)ceilf(metrics.height);

  return (int)ceilf(metrics.height);
}

void CTextManager::DrawNow() {
  int readBuf = 1 - m_b;
  int nMsgs = m_nMsg[readBuf];

  if (!m_d2dReady || !m_lpDX) {
    m_b = 1 - m_b;
    ClearAll();
    return;
  }

  UINT frameIdx = m_lpDX->m_frameIndex;
  if (frameIdx >= 2 || !m_wrappedBackBuffers[frameIdx] || !m_d2dRenderTargets[frameIdx]) {
    m_b = 1 - m_b;
    ClearAll();
    return;
  }

  // Acquire the wrapped back buffer for D2D access
  ID3D11Resource* resources[] = { m_wrappedBackBuffers[frameIdx].Get() };
  m_d3d11On12Device->AcquireWrappedResources(resources, 1);

  if (nMsgs > 0) {
    m_d2dContext->SetTarget(m_d2dRenderTargets[frameIdx].Get());
    m_d2dContext->BeginDraw();

    for (int i = 0; i < nMsgs; i++) {
      td_string& entry = m_msg[readBuf][i];

      if (!entry.pfont) {
        // Box entry (no font = filled rectangle)
        D2D1_COLOR_F boxColor = D3DColorToD2D(entry.bgColor);
        m_brush->SetColor(boxColor);
        D2D1_RECT_F rect = {
          (float)entry.rect.left,  (float)entry.rect.top,
          (float)entry.rect.right, (float)entry.rect.bottom };
        m_d2dContext->FillRectangle(rect, m_brush.Get());
        continue;
      }

      // Text entry
      int fontIdx = DecodeFontIndex(entry.pfont);
      if (fontIdx < 0 || fontIdx >= m_nFonts || !m_dwFormats[fontIdx])
        continue;

      D2D1_COLOR_F color = D3DColorToD2D(entry.color);
      m_brush->SetColor(color);

      DWORD drawFlags = entry.flags & ~DT_CALCRECT;

      D2D1_RECT_F rect = {
        (float)entry.rect.left,  (float)entry.rect.top,
        (float)entry.rect.right, (float)entry.rect.bottom };

      // Map DT_ flags to DirectWrite alignment
      ApplyDWriteAlignment(m_dwFormats[fontIdx].Get(), drawFlags);

      D2D1_DRAW_TEXT_OPTIONS opts = D2D1_DRAW_TEXT_OPTIONS_CLIP;
      m_d2dContext->DrawText(
        entry.msg, (UINT32)wcslen(entry.msg),
        m_dwFormats[fontIdx].Get(), rect, m_brush.Get(), opts);
    }

    m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);
  }

  // Release wrapped resource (transitions back buffer RT → PRESENT)
  m_d3d11On12Device->ReleaseWrappedResources(resources, 1);
  m_d3d11Context->Flush();

  // Flip double buffer and clear for next frame
  m_b = 1 - m_b;
  ClearAll();
}
