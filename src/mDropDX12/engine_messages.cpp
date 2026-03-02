/*
  Plugin module: Messages, Sprites, Remote Communication & Audio
  Extracted from engine.cpp for maintainability.
  Contains: Custom messages, supertexts, sprites, song title animations,
            remote communication, screenshots, audio analysis, misc utilities
*/

#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include "AutoCharFn.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "shell_defines.h"
#include "wasabi.h"
#include <assert.h>
#include <strsafe.h>
#include <Windows.h>
#include <cstdint>
#include <sstream>
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include "../audio/log.h"
#include "AMDDetection.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <set>

#define FRAND ((rand() % 7381)/7380.0f)

// RGB hue rotation helper — rotates RGB color by hue degrees (0–360)
static void HueRotateRGB(int& r, int& g, int& b, float hueDeg) {
  // Convert RGB to HSV
  float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
  float cmax = max(rf, max(gf, bf));
  float cmin = min(rf, min(gf, bf));
  float delta = cmax - cmin;
  float h = 0, s = 0, v = cmax;
  if (delta > 0.0001f) {
    s = delta / cmax;
    if (cmax == rf)      h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
    else if (cmax == gf) h = 60.0f * ((bf - rf) / delta + 2.0f);
    else                 h = 60.0f * ((rf - gf) / delta + 4.0f);
    if (h < 0) h += 360.0f;
  }
  // Rotate hue
  h = fmodf(h + hueDeg, 360.0f);
  if (h < 0) h += 360.0f;
  // Convert HSV back to RGB
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c;
  float r1, g1, b1;
  if (h < 60)       { r1 = c; g1 = x; b1 = 0; }
  else if (h < 120) { r1 = x; g1 = c; b1 = 0; }
  else if (h < 180) { r1 = 0; g1 = c; b1 = x; }
  else if (h < 240) { r1 = 0; g1 = x; b1 = c; }
  else if (h < 300) { r1 = x; g1 = 0; b1 = c; }
  else              { r1 = c; g1 = 0; b1 = x; }
  r = (int)((r1 + m) * 255.0f + 0.5f);
  g = (int)((g1 + m) * 255.0f + 0.5f);
  b = (int)((b1 + m) * 255.0f + 0.5f);
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
}

namespace mdrop {

extern Engine g_engine;

void Engine::PopulateMsgListBox(HWND hList) {
  if (!hList) return;
  SendMessage(hList, LB_RESETCONTENT, 0, 0);
  for (int i = 0; i < m_nMsgAutoplayCount; i++) {
    int idx = m_nMsgAutoplayOrder[i];
    if (idx >= 0 && idx < MAX_CUSTOM_MESSAGES && m_CustomMessage[idx].szText[0]) {
      wchar_t entry[300];
      swprintf(entry, 300, L"%02d: %s", idx, m_CustomMessage[idx].szText);
      SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)entry);
    }
  }
}

void Engine::BuildMsgPlaybackOrder() {
  m_nMsgAutoplayCount = 0;
  for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++) {
    if (m_CustomMessage[i].szText[0]) {
      m_nMsgAutoplayOrder[m_nMsgAutoplayCount++] = i;
    }
  }
}

void Engine::UpdateMsgPreview(HWND hSettingsWnd, int sel) {
  if (sel >= 0 && sel < m_nMsgAutoplayCount) {
    int idx = m_nMsgAutoplayOrder[sel];
    int fontID = m_CustomMessage[idx].nFont;
    const wchar_t* fontFace = m_CustomMessage[idx].bOverrideFace
      ? m_CustomMessage[idx].szFace
      : m_CustomMessageFont[fontID].szFace;
    int r = m_CustomMessage[idx].bOverrideColorR ? m_CustomMessage[idx].nColorR : m_CustomMessageFont[fontID].nColorR;
    int g = m_CustomMessage[idx].bOverrideColorG ? m_CustomMessage[idx].nColorG : m_CustomMessageFont[fontID].nColorG;
    int b = m_CustomMessage[idx].bOverrideColorB ? m_CustomMessage[idx].nColorB : m_CustomMessageFont[fontID].nColorB;
    wchar_t preview[512];
    swprintf(preview, 512, L"\"%s\"\nFont: %s  Size: %.0f  R:%d G:%d B:%d  Time: %.1fs",
      m_CustomMessage[idx].szText, fontFace, m_CustomMessage[idx].fSize, r, g, b, m_CustomMessage[idx].fTime);
    SetWindowTextW(GetDlgItem(hSettingsWnd, IDC_MW_MSG_PREVIEW), preview);
  } else {
    SetWindowTextW(GetDlgItem(hSettingsWnd, IDC_MW_MSG_PREVIEW), L"");
  }
}

void Engine::WriteCustomMessages() {
  // Write font definitions
  for (int n = 0; n < MAX_CUSTOM_MESSAGE_FONTS; n++) {
    wchar_t section[32];
    swprintf(section, 32, L"font%02d", n);
    WritePrivateProfileStringW(section, L"face", m_CustomMessageFont[n].szFace, m_szMsgIniFile);
    wchar_t val[32];
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].bBold ? 1 : 0);
    WritePrivateProfileStringW(section, L"bold", val, m_szMsgIniFile);
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].bItal ? 1 : 0);
    WritePrivateProfileStringW(section, L"ital", val, m_szMsgIniFile);
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].nColorR);
    WritePrivateProfileStringW(section, L"r", val, m_szMsgIniFile);
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].nColorG);
    WritePrivateProfileStringW(section, L"g", val, m_szMsgIniFile);
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].nColorB);
    WritePrivateProfileStringW(section, L"b", val, m_szMsgIniFile);
  }

  // Write message definitions
  for (int n = 0; n < MAX_CUSTOM_MESSAGES; n++) {
    wchar_t section[64];
    swprintf(section, 64, L"message%02d", n);

    if (m_CustomMessage[n].szText[0] == 0) {
      // Delete the section for empty messages
      WritePrivateProfileStringW(section, NULL, NULL, m_szMsgIniFile);
      continue;
    }

    WritePrivateProfileStringW(section, L"text", m_CustomMessage[n].szText, m_szMsgIniFile);
    wchar_t val[64];
    swprintf(val, 64, L"%d", m_CustomMessage[n].nFont);
    WritePrivateProfileStringW(section, L"font", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fSize);
    WritePrivateProfileStringW(section, L"size", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].x);
    WritePrivateProfileStringW(section, L"x", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].y);
    WritePrivateProfileStringW(section, L"y", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].randx);
    WritePrivateProfileStringW(section, L"randx", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].randy);
    WritePrivateProfileStringW(section, L"randy", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].growth);
    WritePrivateProfileStringW(section, L"growth", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fTime);
    WritePrivateProfileStringW(section, L"time", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fFade);
    WritePrivateProfileStringW(section, L"fade", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fFadeOut);
    WritePrivateProfileStringW(section, L"fadeout", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fBurnTime);
    WritePrivateProfileStringW(section, L"burntime", val, m_szMsgIniFile);

    // Color
    swprintf(val, 64, L"%d", m_CustomMessage[n].nColorR);
    WritePrivateProfileStringW(section, L"r", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nColorG);
    WritePrivateProfileStringW(section, L"g", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nColorB);
    WritePrivateProfileStringW(section, L"b", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nRandR);
    WritePrivateProfileStringW(section, L"randr", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nRandG);
    WritePrivateProfileStringW(section, L"randg", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nRandB);
    WritePrivateProfileStringW(section, L"randb", val, m_szMsgIniFile);

    // Overrides
    if (m_CustomMessage[n].bOverrideFace)
      WritePrivateProfileStringW(section, L"face", m_CustomMessage[n].szFace, m_szMsgIniFile);
    if (m_CustomMessage[n].bOverrideBold) {
      swprintf(val, 64, L"%d", m_CustomMessage[n].bBold ? 1 : 0);
      WritePrivateProfileStringW(section, L"bold", val, m_szMsgIniFile);
    }
    if (m_CustomMessage[n].bOverrideItal) {
      swprintf(val, 64, L"%d", m_CustomMessage[n].bItal ? 1 : 0);
      WritePrivateProfileStringW(section, L"ital", val, m_szMsgIniFile);
    }

    // Per-message randomize flags
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandPos);
    WritePrivateProfileStringW(section, L"rand_pos", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandSize);
    WritePrivateProfileStringW(section, L"rand_size", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandFont);
    WritePrivateProfileStringW(section, L"rand_font", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandColor);
    WritePrivateProfileStringW(section, L"rand_color", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandEffects);
    WritePrivateProfileStringW(section, L"rand_effects", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandGrowth);
    WritePrivateProfileStringW(section, L"rand_growth", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandDuration);
    WritePrivateProfileStringW(section, L"rand_duration", val, m_szMsgIniFile);
  }
}

void Engine::SaveMsgAutoplaySettings() {
  wchar_t* pIni = GetConfigIniFile();
  wchar_t val[32];

  swprintf(val, 32, L"%d", m_bMsgAutoplay ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgAutoplay", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgSequential ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgSequential", val, pIni);
  WritePrivateProfileFloatW(m_fMsgAutoplayInterval, (wchar_t*)L"MsgAutoplayInterval", pIni, (wchar_t*)L"Milkwave");
  WritePrivateProfileFloatW(m_fMsgAutoplayJitter, (wchar_t*)L"MsgAutoplayJitter", pIni, (wchar_t*)L"Milkwave");
  swprintf(val, 32, L"%d", m_bMessageAutoSize ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MessageAutoSize", val, pIni);

  // Save override settings
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomFont ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideRandomFont", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomColor ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideRandomColor", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomSize ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideRandomSize", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomEffects ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideRandomEffects", val, pIni);
  swprintf(val, 32, L"%.2f", m_fMsgOverrideSizeMin);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideSizeMin", val, pIni);
  swprintf(val, 32, L"%.2f", m_fMsgOverrideSizeMax);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideSizeMax", val, pIni);
  swprintf(val, 32, L"%d", m_nMsgMaxOnScreen);
  WritePrivateProfileStringW(L"Milkwave", L"MsgMaxOnScreen", val, pIni);
  // Animation overrides
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomPos ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideRandomPos", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomGrowth ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideRandomGrowth", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideSlideIn ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideSlideIn", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomDuration ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideRandomDuration", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideShadow ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideShadow", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideBox ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideBox", val, pIni);
  // Color shifting overrides
  swprintf(val, 32, L"%d", m_bMsgOverrideApplyHueShift ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideApplyHueShift", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomHue ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgOverrideRandomHue", val, pIni);
  swprintf(val, 32, L"%d", m_bMsgIgnorePerMsgRandom ? 1 : 0);
  WritePrivateProfileStringW(L"Milkwave", L"MsgIgnorePerMsgRandom", val, pIni);

  // Save playback order
  swprintf(val, 32, L"%d", m_nMsgAutoplayCount);
  WritePrivateProfileStringW(L"MsgOrder", L"Count", val, pIni);
  for (int i = 0; i < m_nMsgAutoplayCount; i++) {
    wchar_t key[32];
    swprintf(key, 32, L"Msg%d", i);
    swprintf(val, 32, L"%d", m_nMsgAutoplayOrder[i]);
    WritePrivateProfileStringW(L"MsgOrder", key, val, pIni);
  }
}

void Engine::LoadMsgAutoplaySettings() {
  wchar_t* pIni = GetConfigIniFile();

  m_bMsgAutoplay = GetPrivateProfileIntW(L"Milkwave", L"MsgAutoplay", 0, pIni) != 0;
  m_bMsgSequential = GetPrivateProfileIntW(L"Milkwave", L"MsgSequential", 0, pIni) != 0;
  m_fMsgAutoplayInterval = GetPrivateProfileFloatW(L"Milkwave", L"MsgAutoplayInterval", 30.0f, pIni);
  m_fMsgAutoplayJitter = GetPrivateProfileFloatW(L"Milkwave", L"MsgAutoplayJitter", 5.0f, pIni);
  m_bMessageAutoSize = GetPrivateProfileIntW(L"Milkwave", L"MessageAutoSize", 0, pIni) != 0;

  // Load override settings
  m_bMsgOverrideRandomFont = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideRandomFont", 0, pIni) != 0;
  m_bMsgOverrideRandomColor = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideRandomColor", 0, pIni) != 0;
  m_bMsgOverrideRandomSize = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideRandomSize", 0, pIni) != 0;
  m_bMsgOverrideRandomEffects = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideRandomEffects", 0, pIni) != 0;
  {
    wchar_t tmp[32];
    GetPrivateProfileStringW(L"Milkwave", L"MsgOverrideSizeMin", L"10", tmp, 32, pIni);
    m_fMsgOverrideSizeMin = (float)_wtof(tmp);
    GetPrivateProfileStringW(L"Milkwave", L"MsgOverrideSizeMax", L"40", tmp, 32, pIni);
    m_fMsgOverrideSizeMax = (float)_wtof(tmp);
  }
  m_nMsgMaxOnScreen = GetPrivateProfileIntW(L"Milkwave", L"MsgMaxOnScreen", 1, pIni);
  // Animation overrides
  m_bMsgOverrideRandomPos = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideRandomPos", 0, pIni) != 0;
  m_bMsgOverrideRandomGrowth = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideRandomGrowth", 0, pIni) != 0;
  m_bMsgOverrideSlideIn = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideSlideIn", 0, pIni) != 0;
  m_bMsgOverrideRandomDuration = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideRandomDuration", 0, pIni) != 0;
  m_bMsgOverrideShadow = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideShadow", 0, pIni) != 0;
  m_bMsgOverrideBox = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideBox", 0, pIni) != 0;
  // Color shifting overrides
  m_bMsgOverrideApplyHueShift = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideApplyHueShift", 0, pIni) != 0;
  m_bMsgOverrideRandomHue = GetPrivateProfileIntW(L"Milkwave", L"MsgOverrideRandomHue", 0, pIni) != 0;
  m_bMsgIgnorePerMsgRandom = GetPrivateProfileIntW(L"Milkwave", L"MsgIgnorePerMsgRandom", 0, pIni) != 0;
  if (m_fMsgOverrideSizeMin < 0.01f) m_fMsgOverrideSizeMin = 0.01f;
  if (m_fMsgOverrideSizeMax > 100.0f) m_fMsgOverrideSizeMax = 100.0f;
  if (m_fMsgOverrideSizeMin >= m_fMsgOverrideSizeMax) m_fMsgOverrideSizeMin = m_fMsgOverrideSizeMax * 0.5f;
  if (m_nMsgMaxOnScreen < 1) m_nMsgMaxOnScreen = 1;
  if (m_nMsgMaxOnScreen > NUM_SUPERTEXTS) m_nMsgMaxOnScreen = NUM_SUPERTEXTS;

  // Load playback order (if saved); otherwise use default order
  int count = GetPrivateProfileIntW(L"MsgOrder", L"Count", 0, pIni);
  if (count > 0 && count <= MAX_CUSTOM_MESSAGES) {
    m_nMsgAutoplayCount = 0;
    for (int i = 0; i < count; i++) {
      wchar_t key[32];
      swprintf(key, 32, L"Msg%d", i);
      int idx = GetPrivateProfileIntW(L"MsgOrder", key, -1, pIni);
      if (idx >= 0 && idx < MAX_CUSTOM_MESSAGES && m_CustomMessage[idx].szText[0]) {
        m_nMsgAutoplayOrder[m_nMsgAutoplayCount++] = idx;
      }
    }
  } else {
    BuildMsgPlaybackOrder();
  }
}

void Engine::ScheduleNextAutoMessage() {
  if (!m_bMsgAutoplay || m_nMsgAutoplayCount == 0) {
    m_fNextAutoMsgTime = -1.0f;
    return;
  }
  float jitter = m_fMsgAutoplayJitter * ((rand() % 2001 - 1000) / 1000.0f);
  float interval = m_fMsgAutoplayInterval + jitter;
  if (interval < 1.0f) interval = 1.0f;
  m_fNextAutoMsgTime = GetTime() + interval;
}

// Message edit dialog procedure
struct MsgEditDlgData {
  Engine*    plugin;
  int         msgIndex;
  bool        isNew;
  HWND        hDlgWnd;
  bool        bResult;
  bool        bDone;

  // Working copy of message fields
  wchar_t     szText[256];
  int         nFont;
  float       fSize, x, y, growth, fTime, fFade, fFadeOut;

  // Font override working copy
  bool        bOverrideFace, bOverrideBold, bOverrideItal;
  bool        bOverrideColorR, bOverrideColorG, bOverrideColorB;
  wchar_t     szFace[128];
  int         bBold, bItal;
  int         nColorR, nColorG, nColorB;

  // Per-message randomize working copies
  bool bRandPos, bRandSize, bRandFont, bRandColor;
  bool bRandEffects, bRandGrowth, bRandDuration;

  // Original message backup (for Send Now + Cancel)
  td_custom_msg originalMsg;

  static COLORREF s_acrCustColors[16];
};
COLORREF MsgEditDlgData::s_acrCustColors[16] = {};

static void UpdateMsgEditFontPreview(MsgEditDlgData* data) {
  if (!data || !data->hDlgWnd) return;
  Engine* p = data->plugin;
  int fontID = data->nFont;
  if (fontID < 0) fontID = 0;
  if (fontID >= MAX_CUSTOM_MESSAGE_FONTS) fontID = MAX_CUSTOM_MESSAGE_FONTS - 1;

  const wchar_t* face = data->bOverrideFace ? data->szFace : p->m_CustomMessageFont[fontID].szFace;
  bool bold = data->bOverrideBold ? (data->bBold != 0) : (p->m_CustomMessageFont[fontID].bBold != 0);
  bool ital = data->bOverrideItal ? (data->bItal != 0) : (p->m_CustomMessageFont[fontID].bItal != 0);
  int r = data->bOverrideColorR ? data->nColorR : p->m_CustomMessageFont[fontID].nColorR;
  int g = data->bOverrideColorG ? data->nColorG : p->m_CustomMessageFont[fontID].nColorG;
  int b = data->bOverrideColorB ? data->nColorB : p->m_CustomMessageFont[fontID].nColorB;

  wchar_t preview[256];
  swprintf(preview, 256, L"%s%s%s   RGB(%d, %d, %d)",
    face, bold ? L", Bold" : L"", ital ? L", Italic" : L"", r, g, b);
  SetWindowTextW(GetDlgItem(data->hDlgWnd, IDC_MSGEDIT_FONT_PREVIEW), preview);
  InvalidateRect(GetDlgItem(data->hDlgWnd, IDC_MSGEDIT_COLOR_SWATCH), NULL, TRUE);
}

static LRESULT CALLBACK MsgEditWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  MsgEditDlgData* data = (MsgEditDlgData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

  switch (msg) {
  case WM_COMMAND: {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    if (id == IDC_MSGEDIT_OK && code == BN_CLICKED) {
      wchar_t buf[256];
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_TEXT), data->szText, 256);
      if (data->szText[0] == 0) {
        MessageBoxW(hWnd, L"Message text cannot be empty.", L"Messages", MB_OK);
        return 0;
      }
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_SIZE), buf, 64);
      data->fSize = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_XPOS), buf, 64);
      data->x = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_YPOS), buf, 64);
      data->y = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_GROWTH), buf, 64);
      data->growth = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_TIME), buf, 64);
      data->fTime = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_FADEIN), buf, 64);
      data->fFade = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_FADEOUT), buf, 64);
      data->fFadeOut = (float)_wtof(buf);

      int sel = (int)SendMessage(GetDlgItem(hWnd, IDC_MSGEDIT_FONT_COMBO), CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel < MAX_CUSTOM_MESSAGE_FONTS) data->nFont = sel;

      // Clamp
      if (data->nFont < 0) data->nFont = 0;
      if (data->nFont >= MAX_CUSTOM_MESSAGE_FONTS) data->nFont = MAX_CUSTOM_MESSAGE_FONTS - 1;
      if (data->fSize < 0) data->fSize = 0;
      if (data->fSize > 100) data->fSize = 100;
      if (data->fTime < 0.1f) data->fTime = 0.1f;

      // Read randomize checkbox states
      data->bRandPos = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_POS), L"Checked");
      data->bRandSize = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_SIZE), L"Checked");
      data->bRandFont = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_FONT), L"Checked");
      data->bRandColor = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_COLOR), L"Checked");
      data->bRandEffects = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_EFFECTS), L"Checked");
      data->bRandGrowth = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_GROWTH), L"Checked");
      data->bRandDuration = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_DURATION), L"Checked");

      data->bResult = true;
      data->bDone = true;
      return 0;
    }
    if (id == IDC_MSGEDIT_CANCEL && code == BN_CLICKED) {
      data->bResult = false;
      data->bDone = true;
      return 0;
    }
    if (id == IDC_MSGEDIT_FONT_COMBO && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel < MAX_CUSTOM_MESSAGE_FONTS)
        data->nFont = sel;
      UpdateMsgEditFontPreview(data);
      return 0;
    }
    if (id == IDC_MSGEDIT_CHOOSE_FONT && code == BN_CLICKED) {
      Engine* p = data->plugin;
      int fontID = data->nFont;
      if (fontID < 0) fontID = 0;
      if (fontID >= MAX_CUSTOM_MESSAGE_FONTS) fontID = MAX_CUSTOM_MESSAGE_FONTS - 1;

      // Resolve current effective values
      const wchar_t* curFace = data->bOverrideFace ? data->szFace : p->m_CustomMessageFont[fontID].szFace;
      bool curBold = data->bOverrideBold ? (data->bBold != 0) : (p->m_CustomMessageFont[fontID].bBold != 0);
      bool curItal = data->bOverrideItal ? (data->bItal != 0) : (p->m_CustomMessageFont[fontID].bItal != 0);
      int curR = data->bOverrideColorR ? data->nColorR : p->m_CustomMessageFont[fontID].nColorR;
      int curG = data->bOverrideColorG ? data->nColorG : p->m_CustomMessageFont[fontID].nColorG;
      int curB = data->bOverrideColorB ? data->nColorB : p->m_CustomMessageFont[fontID].nColorB;

      LOGFONTW lf = {};
      wcscpy_s(lf.lfFaceName, 32, curFace);
      lf.lfWeight = curBold ? FW_BOLD : FW_NORMAL;
      lf.lfItalic = curItal ? TRUE : FALSE;
      lf.lfHeight = -24;

      CHOOSEFONTW cf = { sizeof(cf) };
      cf.hwndOwner = hWnd;
      cf.lpLogFont = &lf;
      cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
      cf.rgbColors = RGB(curR < 0 ? 255 : curR, curG < 0 ? 255 : curG, curB < 0 ? 255 : curB);

      if (ChooseFontW(&cf)) {
        data->bOverrideFace = true;
        wcscpy_s(data->szFace, 128, lf.lfFaceName);
        data->bOverrideBold = true;
        data->bBold = (lf.lfWeight >= FW_BOLD) ? 1 : 0;
        data->bOverrideItal = true;
        data->bItal = lf.lfItalic ? 1 : 0;
        data->bOverrideColorR = true;
        data->bOverrideColorG = true;
        data->bOverrideColorB = true;
        data->nColorR = GetRValue(cf.rgbColors);
        data->nColorG = GetGValue(cf.rgbColors);
        data->nColorB = GetBValue(cf.rgbColors);
        UpdateMsgEditFontPreview(data);
      }
      return 0;
    }
    if (id == IDC_MSGEDIT_CHOOSE_COLOR && code == BN_CLICKED) {
      Engine* p = data->plugin;
      int fontID = data->nFont;
      if (fontID < 0) fontID = 0;
      if (fontID >= MAX_CUSTOM_MESSAGE_FONTS) fontID = MAX_CUSTOM_MESSAGE_FONTS - 1;

      int curR = data->bOverrideColorR ? data->nColorR : p->m_CustomMessageFont[fontID].nColorR;
      int curG = data->bOverrideColorG ? data->nColorG : p->m_CustomMessageFont[fontID].nColorG;
      int curB = data->bOverrideColorB ? data->nColorB : p->m_CustomMessageFont[fontID].nColorB;

      CHOOSECOLORW cc = { sizeof(cc) };
      cc.hwndOwner = hWnd;
      cc.rgbResult = RGB(curR < 0 ? 255 : curR, curG < 0 ? 255 : curG, curB < 0 ? 255 : curB);
      cc.lpCustColors = MsgEditDlgData::s_acrCustColors;
      cc.Flags = CC_FULLOPEN | CC_RGBINIT;

      if (ChooseColorW(&cc)) {
        data->bOverrideColorR = true;
        data->bOverrideColorG = true;
        data->bOverrideColorB = true;
        data->nColorR = GetRValue(cc.rgbResult);
        data->nColorG = GetGValue(cc.rgbResult);
        data->nColorB = GetBValue(cc.rgbResult);
        UpdateMsgEditFontPreview(data);
      }
      return 0;
    }
    // Randomize All — check all per-message randomize checkboxes
    if (id == IDC_MSGEDIT_RAND_ALL && code == BN_CLICKED) {
      int ids[] = { IDC_MSGEDIT_RAND_POS, IDC_MSGEDIT_RAND_SIZE, IDC_MSGEDIT_RAND_FONT,
                    IDC_MSGEDIT_RAND_COLOR, IDC_MSGEDIT_RAND_EFFECTS, IDC_MSGEDIT_RAND_GROWTH,
                    IDC_MSGEDIT_RAND_DURATION };
      for (int cid : ids) {
        HWND hCtrl = GetDlgItem(hWnd, cid);
        if (hCtrl) {
          SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)1);
          InvalidateRect(hCtrl, NULL, TRUE);
        }
      }
      return 0;
    }
    // Send Now — preview the message immediately
    if (id == IDC_MSGEDIT_SEND_NOW && code == BN_CLICKED && data) {
      // Read current control values into data
      wchar_t buf[256];
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_TEXT), data->szText, 256);
      if (data->szText[0] == 0) return 0;
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_SIZE), buf, 64);
      data->fSize = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_XPOS), buf, 64);
      data->x = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_YPOS), buf, 64);
      data->y = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_GROWTH), buf, 64);
      data->growth = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_TIME), buf, 64);
      data->fTime = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_FADEIN), buf, 64);
      data->fFade = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGEDIT_FADEOUT), buf, 64);
      data->fFadeOut = (float)_wtof(buf);
      int sel = (int)SendMessage(GetDlgItem(hWnd, IDC_MSGEDIT_FONT_COMBO), CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel < MAX_CUSTOM_MESSAGE_FONTS) data->nFont = sel;
      // Read randomize flags
      data->bRandPos = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_POS), L"Checked");
      data->bRandSize = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_SIZE), L"Checked");
      data->bRandFont = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_FONT), L"Checked");
      data->bRandColor = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_COLOR), L"Checked");
      data->bRandEffects = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_EFFECTS), L"Checked");
      data->bRandGrowth = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_GROWTH), L"Checked");
      data->bRandDuration = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGEDIT_RAND_DURATION), L"Checked");

      // Temporarily write to the message slot and push it
      Engine* p = data->plugin;
      td_custom_msg* m = &p->m_CustomMessage[data->msgIndex];
      wcscpy_s(m->szText, 256, data->szText);
      m->nFont = data->nFont;
      m->fSize = data->fSize;
      m->x = data->x;
      m->y = data->y;
      m->growth = data->growth;
      m->fTime = data->fTime;
      m->fFade = data->fFade;
      m->fFadeOut = data->fFadeOut;
      m->bOverrideFace = data->bOverrideFace ? 1 : 0;
      m->bOverrideBold = data->bOverrideBold ? 1 : 0;
      m->bOverrideItal = data->bOverrideItal ? 1 : 0;
      m->bOverrideColorR = data->bOverrideColorR ? 1 : 0;
      m->bOverrideColorG = data->bOverrideColorG ? 1 : 0;
      m->bOverrideColorB = data->bOverrideColorB ? 1 : 0;
      wcscpy_s(m->szFace, 128, data->szFace);
      m->bBold = data->bBold;
      m->bItal = data->bItal;
      m->nColorR = data->nColorR;
      m->nColorG = data->nColorG;
      m->nColorB = data->nColorB;
      m->bRandPos = data->bRandPos ? 1 : 0;
      m->bRandSize = data->bRandSize ? 1 : 0;
      m->bRandFont = data->bRandFont ? 1 : 0;
      m->bRandColor = data->bRandColor ? 1 : 0;
      m->bRandEffects = data->bRandEffects ? 1 : 0;
      m->bRandGrowth = data->bRandGrowth ? 1 : 0;
      m->bRandDuration = data->bRandDuration ? 1 : 0;

      HWND hw = p->GetPluginWindow();
      if (hw) PostMessage(hw, WM_MW_PUSH_MESSAGE, data->msgIndex, 0);
      return 0;
    }
    // Owner-drawn checkbox toggle for randomize checkboxes
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      bool bIsCheckbox = (bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox");
      if (bIsCheckbox) {
        bool wasChecked = (bool)(intptr_t)GetPropW(hCtrl, L"Checked");
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(wasChecked ? 0 : 1));
        InvalidateRect(hCtrl, NULL, TRUE);
      }
    }
    break;
  }

  case WM_DRAWITEM: {
    DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
    if (!pDIS) break;
    if (pDIS->CtlType == ODT_BUTTON) {
      // Owner-drawn checkbox rendering
      if (data && data->plugin) {
        Engine* p = data->plugin;
        bool bIsCheckbox = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsCheckbox");
        if (bIsCheckbox) {
          DrawOwnerCheckbox(pDIS, p->m_bSettingsDarkTheme,
            p->m_colSettingsBg, p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText);
          return TRUE;
        }
      }
      if ((int)pDIS->CtlID == IDC_MSGEDIT_COLOR_SWATCH && data) {
        // Draw color swatch
        Engine* p = data->plugin;
        int fontID = data->nFont;
        if (fontID < 0) fontID = 0;
        if (fontID >= MAX_CUSTOM_MESSAGE_FONTS) fontID = MAX_CUSTOM_MESSAGE_FONTS - 1;
        int r = data->bOverrideColorR ? data->nColorR : p->m_CustomMessageFont[fontID].nColorR;
        int g = data->bOverrideColorG ? data->nColorG : p->m_CustomMessageFont[fontID].nColorG;
        int b = data->bOverrideColorB ? data->nColorB : p->m_CustomMessageFont[fontID].nColorB;
        HBRUSH hBr = CreateSolidBrush(RGB(r < 0 ? 255 : r, g < 0 ? 255 : g, b < 0 ? 255 : b));
        FillRect(pDIS->hDC, &pDIS->rcItem, hBr);
        DeleteObject(hBr);
        FrameRect(pDIS->hDC, &pDIS->rcItem, (HBRUSH)GetStockObject(WHITE_BRUSH));
        return TRUE;
      }
      if (data && data->plugin) {
        Engine* p = data->plugin;
        DrawOwnerButton(pDIS, p->m_bSettingsDarkTheme,
          p->m_colSettingsBtnFace, p->m_colSettingsBtnHi, p->m_colSettingsBtnShadow, p->m_colSettingsText);
        return TRUE;
      }
    }
    break;
  }

  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    if (data && data->plugin && data->plugin->m_bSettingsDarkTheme && data->plugin->m_hBrSettingsCtrlBg) {
      SetTextColor((HDC)wParam, data->plugin->m_colSettingsText);
      SetBkColor((HDC)wParam, data->plugin->m_colSettingsCtrlBg);
      return (LRESULT)data->plugin->m_hBrSettingsCtrlBg;
    }
    break;

  case WM_CTLCOLORSTATIC:
    if (data && data->plugin && data->plugin->m_bSettingsDarkTheme && data->plugin->m_hBrSettingsBg) {
      SetTextColor((HDC)wParam, data->plugin->m_colSettingsText);
      SetBkColor((HDC)wParam, data->plugin->m_colSettingsBg);
      return (LRESULT)data->plugin->m_hBrSettingsBg;
    }
    break;

  case WM_ERASEBKGND:
    if (data && data->plugin && data->plugin->m_bSettingsDarkTheme) {
      RECT rc; GetClientRect(hWnd, &rc);
      HBRUSH hBr = CreateSolidBrush(data->plugin->m_colSettingsBg);
      FillRect((HDC)wParam, &rc, hBr);
      DeleteObject(hBr);
      return 1;
    }
    break;

  case WM_CLOSE:
    if (data) { data->bResult = false; data->bDone = true; }
    return 0;

  case WM_KEYDOWN:
    if (wParam == VK_ESCAPE && data) { data->bResult = false; data->bDone = true; return 0; }
    if (wParam == VK_RETURN && data) {
      // Simulate OK click
      SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDC_MSGEDIT_OK, BN_CLICKED), 0);
      return 0;
    }
    break;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool Engine::ShowMessageEditDialog(HWND hParent, int msgIndex, bool isNew) {
  // Register window class (once)
  static bool registered = false;
  static const wchar_t* WND_CLASS = L"MDropDX12MsgEdit";
  if (!registered) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MsgEditWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);
    registered = true;
  }

  // Initialize working copy from message data
  MsgEditDlgData data = {};
  data.plugin = this;
  data.msgIndex = msgIndex;
  data.isNew = isNew;
  data.bResult = false;
  data.bDone = false;

  td_custom_msg* m = &m_CustomMessage[msgIndex];
  data.originalMsg = *m; // Save original for Cancel after Send Now

  if (!isNew) {
    wcscpy_s(data.szText, 256, m->szText);
    data.nFont = m->nFont;
    data.fSize = m->fSize;
    data.x = m->x;
    data.y = m->y;
    data.growth = m->growth;
    data.fTime = m->fTime;
    data.fFade = m->fFade;
    data.fFadeOut = m->fFadeOut;
    data.bOverrideFace = m->bOverrideFace != 0;
    data.bOverrideBold = m->bOverrideBold != 0;
    data.bOverrideItal = m->bOverrideItal != 0;
    data.bOverrideColorR = m->bOverrideColorR != 0;
    data.bOverrideColorG = m->bOverrideColorG != 0;
    data.bOverrideColorB = m->bOverrideColorB != 0;
    wcscpy_s(data.szFace, 128, m->szFace);
    data.bBold = m->bBold;
    data.bItal = m->bItal;
    data.nColorR = m->nColorR;
    data.nColorG = m->nColorG;
    data.nColorB = m->nColorB;
    data.bRandPos = m->bRandPos != 0;
    data.bRandSize = m->bRandSize != 0;
    data.bRandFont = m->bRandFont != 0;
    data.bRandColor = m->bRandColor != 0;
    data.bRandEffects = m->bRandEffects != 0;
    data.bRandGrowth = m->bRandGrowth != 0;
    data.bRandDuration = m->bRandDuration != 0;
  } else {
    data.szText[0] = 0;
    data.nFont = 0;
    data.fSize = 50.0f;
    data.x = 0.5f;
    data.y = 0.5f;
    data.growth = 1.0f;
    data.fTime = 5.0f;
    data.fFade = 1.0f;
    data.fFadeOut = 1.0f;
    data.szFace[0] = 0;
    data.bBold = -1;
    data.bItal = -1;
    data.nColorR = -1;
    data.nColorG = -1;
    data.nColorB = -1;
  }

  // Create font for controls (use settings font size)
  HFONT hFont = CreateFontW(m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  // Compute line height from font metrics for proportional layout
  HDC hdcTmp = GetDC(hParent);
  HFONT hOldTmp = (HFONT)SelectObject(hdcTmp, hFont);
  TEXTMETRIC tmDlg = {};
  GetTextMetrics(hdcTmp, &tmDlg);
  SelectObject(hdcTmp, hOldTmp);
  ReleaseDC(hParent, hdcTmp);
  int dlgLineH = tmDlg.tmHeight + tmDlg.tmExternalLeading + 6;
  if (dlgLineH < 20) dlgLineH = 20;

  // Scale dialog dimensions from baseline (440x530 at lineH=20)
  int clientW = MulDiv(440, dlgLineH, 20);
  int clientH = MulDiv(530, dlgLineH, 20);
  DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  DWORD dwExStyle = WS_EX_DLGMODALFRAME;
  RECT rcSize = { 0, 0, clientW, clientH };
  AdjustWindowRectEx(&rcSize, dwStyle, FALSE, dwExStyle);
  int dlgW = rcSize.right - rcSize.left;
  int dlgH = rcSize.bottom - rcSize.top;

  // Center on the monitor that contains the parent window
  RECT rcParent;
  GetWindowRect(hParent, &rcParent);
  HMONITOR hMon = MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  GetMonitorInfo(hMon, &mi);
  int px = rcParent.left + (rcParent.right - rcParent.left - dlgW) / 2;
  int py = rcParent.top + (rcParent.bottom - rcParent.top - dlgH) / 2;
  // Clamp to monitor work area
  if (px < mi.rcWork.left) px = mi.rcWork.left;
  if (py < mi.rcWork.top) py = mi.rcWork.top;
  if (px + dlgW > mi.rcWork.right) px = mi.rcWork.right - dlgW;
  if (py + dlgH > mi.rcWork.bottom) py = mi.rcWork.bottom - dlgH;

  const wchar_t* title = isNew ? L"Add Message" : L"Edit Message";
  HWND hDlg = CreateWindowExW(dwExStyle,
    WND_CLASS, title, dwStyle,
    px, py, dlgW, dlgH, hParent, NULL, GetModuleHandle(NULL), NULL);
  if (!hDlg) { DeleteObject(hFont); return false; }

  data.hDlgWnd = hDlg;
  SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)&data);

  // Scale layout constants from baseline (designed at lineH=20)
  int margin = MulDiv(12, dlgLineH, 20);
  int rw = clientW - margin * 2;
  int lblW = MulDiv(90, dlgLineH, 20), editW = MulDiv(60, dlgLineH, 20);
  int y = MulDiv(10, dlgLineH, 20);
  int xVal = margin + lblW + 4;
  int smallH = dlgLineH - 4;      // label height
  int editH = dlgLineH;           // edit control height
  int btnH = dlgLineH + 4;        // button height
  int textEditH = dlgLineH * 2 + 8; // multiline text edit
  wchar_t buf[64];

  // Text label + edit
  CreateLabel(hDlg, L"Message Text:", margin, y, rw, smallH, hFont);
  y += smallH + 2;
  HWND hText = CreateEdit(hDlg, data.szText, IDC_MSGEDIT_TEXT, margin, y, rw, textEditH, hFont,
    ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
  y += textEditH + 6;

  // Font section
  CreateLabel(hDlg, L"Base Font:", margin, y + 2, MulDiv(70, dlgLineH, 20), smallH, hFont);
  HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
    margin + MulDiv(74, dlgLineH, 20), y, rw - MulDiv(74, dlgLineH, 20), 300, hDlg, (HMENU)(INT_PTR)IDC_MSGEDIT_FONT_COMBO,
    GetModuleHandle(NULL), NULL);
  if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
  for (int i = 0; i < MAX_CUSTOM_MESSAGE_FONTS; i++) {
    wchar_t entry[160];
    swprintf(entry, 160, L"Font %02d: %s%s%s", i,
      m_CustomMessageFont[i].szFace,
      m_CustomMessageFont[i].bBold ? L" [Bold]" : L"",
      m_CustomMessageFont[i].bItal ? L" [Italic]" : L"");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)entry);
  }
  SendMessage(hCombo, CB_SETCURSEL, data.nFont, 0);
  y += dlgLineH + 4;

  // Choose Font, Choose Color, Color Swatch
  int chooseBtnW = MulDiv(110, dlgLineH, 20);
  CreateBtn(hDlg, L"Choose Font...", IDC_MSGEDIT_CHOOSE_FONT, margin, y, chooseBtnW, btnH, hFont);
  CreateBtn(hDlg, L"Choose Color...", IDC_MSGEDIT_CHOOSE_COLOR, margin + chooseBtnW + 6, y, chooseBtnW, btnH, hFont);
  // Color swatch (owner-drawn button)
  int swatchSize = smallH;
  HWND hSwatch = CreateWindowExW(0, L"BUTTON", L"",
    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
    margin + chooseBtnW * 2 + 12, y + 2, swatchSize, swatchSize, hDlg,
    (HMENU)(INT_PTR)IDC_MSGEDIT_COLOR_SWATCH, GetModuleHandle(NULL), NULL);
  y += btnH + 6;

  // Font preview
  HWND hPreview = CreateLabel(hDlg, L"", margin, y, rw, smallH, hFont);
  SetWindowLongPtr(hPreview, GWL_ID, IDC_MSGEDIT_FONT_PREVIEW);
  y += dlgLineH + 4;

  // Separator
  y += 4;

  // Size, X, Y on same row
  CreateLabel(hDlg, L"Size (0-100):", margin, y + 2, lblW, smallH, hFont);
  swprintf(buf, 64, L"%.0f", data.fSize);
  CreateEdit(hDlg, buf, IDC_MSGEDIT_SIZE, xVal, y, editW, editH, hFont);
  int smallLblW = MulDiv(16, dlgLineH, 20);
  CreateLabel(hDlg, L"X:", xVal + editW + 10, y + 2, smallLblW, smallH, hFont);
  swprintf(buf, 64, L"%.2f", data.x);
  CreateEdit(hDlg, buf, IDC_MSGEDIT_XPOS, xVal + editW + 10 + smallLblW + 2, y, editW, editH, hFont);
  CreateLabel(hDlg, L"Y:", xVal + editW * 2 + 10 + smallLblW + 12, y + 2, smallLblW, smallH, hFont);
  swprintf(buf, 64, L"%.2f", data.y);
  CreateEdit(hDlg, buf, IDC_MSGEDIT_YPOS, xVal + editW * 2 + 10 + smallLblW * 2 + 14, y, editW, editH, hFont);
  y += dlgLineH + 4;

  // Growth
  CreateLabel(hDlg, L"Growth:", margin, y + 2, lblW, smallH, hFont);
  swprintf(buf, 64, L"%.2f", data.growth);
  CreateEdit(hDlg, buf, IDC_MSGEDIT_GROWTH, xVal, y, editW, editH, hFont);
  y += dlgLineH + 4;

  // Duration
  CreateLabel(hDlg, L"Duration (s):", margin, y + 2, lblW, smallH, hFont);
  swprintf(buf, 64, L"%.1f", data.fTime);
  CreateEdit(hDlg, buf, IDC_MSGEDIT_TIME, xVal, y, editW, editH, hFont);
  y += dlgLineH + 4;

  // Fade In, Fade Out on same row
  int fadeOutLblW = MulDiv(70, dlgLineH, 20);
  CreateLabel(hDlg, L"Fade In (s):", margin, y + 2, lblW, smallH, hFont);
  swprintf(buf, 64, L"%.1f", data.fFade);
  CreateEdit(hDlg, buf, IDC_MSGEDIT_FADEIN, xVal, y, editW, editH, hFont);
  CreateLabel(hDlg, L"Fade Out:", xVal + editW + 10, y + 2, fadeOutLblW, smallH, hFont);
  swprintf(buf, 64, L"%.1f", data.fFadeOut);
  CreateEdit(hDlg, buf, IDC_MSGEDIT_FADEOUT, xVal + editW + 10 + fadeOutLblW + 2, y, editW, editH, hFont);
  y += dlgLineH + 8;

  // --- Randomize section (2-column checkboxes) ---
  int halfW = (rw - 10) / 2;
  int randBtnW = MulDiv(110, dlgLineH, 20);
  CreateLabel(hDlg, L"Randomize:", margin, y + 2, MulDiv(80, dlgLineH, 20), smallH, hFont);
  CreateBtn(hDlg, L"Randomize All", IDC_MSGEDIT_RAND_ALL, margin + rw - randBtnW, y, randBtnW, btnH, hFont);
  y += btnH + 4;

  CreateCheck(hDlg, L"Position", IDC_MSGEDIT_RAND_POS, margin, y, halfW, editH, hFont, data.bRandPos, true);
  CreateCheck(hDlg, L"Font", IDC_MSGEDIT_RAND_FONT, margin + halfW + 10, y, halfW, editH, hFont, data.bRandFont, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Size", IDC_MSGEDIT_RAND_SIZE, margin, y, halfW, editH, hFont, data.bRandSize, true);
  CreateCheck(hDlg, L"Color", IDC_MSGEDIT_RAND_COLOR, margin + halfW + 10, y, halfW, editH, hFont, data.bRandColor, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Effects (bold/ital)", IDC_MSGEDIT_RAND_EFFECTS, margin, y, halfW, editH, hFont, data.bRandEffects, true);
  CreateCheck(hDlg, L"Growth", IDC_MSGEDIT_RAND_GROWTH, margin + halfW + 10, y, halfW, editH, hFont, data.bRandGrowth, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Duration", IDC_MSGEDIT_RAND_DURATION, margin, y, halfW, editH, hFont, data.bRandDuration, true);
  y += dlgLineH + 12;

  // Send Now / OK / Cancel buttons
  int okBtnW = MulDiv(80, dlgLineH, 20);
  int sendBtnW = MulDiv(90, dlgLineH, 20);
  CreateBtn(hDlg, L"Send Now", IDC_MSGEDIT_SEND_NOW, margin, y, sendBtnW, btnH, hFont);
  CreateBtn(hDlg, L"OK", IDC_MSGEDIT_OK, clientW / 2 - okBtnW + 20, y, okBtnW, btnH, hFont);
  CreateBtn(hDlg, L"Cancel", IDC_MSGEDIT_CANCEL, clientW / 2 + okBtnW + 20, y, okBtnW, btnH, hFont);

  // Update the font preview
  UpdateMsgEditFontPreview(&data);

  // Show dialog and make parent modal
  ShowWindow(hDlg, SW_SHOW);
  UpdateWindow(hDlg);
  EnableWindow(hParent, FALSE);

  // Local message loop
  MSG msg2;
  while (!data.bDone && GetMessage(&msg2, NULL, 0, 0)) {
    // Handle Tab key navigation
    if (msg2.message == WM_KEYDOWN && msg2.wParam == VK_TAB) {
      HWND hNext = GetNextDlgTabItem(hDlg, GetFocus(), GetKeyState(VK_SHIFT) < 0);
      if (hNext) SetFocus(hNext);
      continue;
    }
    // ESC closes dialog
    if (msg2.message == WM_KEYDOWN && msg2.wParam == VK_ESCAPE) {
      data.bResult = false;
      data.bDone = true;
      break;
    }
    TranslateMessage(&msg2);
    DispatchMessage(&msg2);
  }

  // Cleanup
  EnableWindow(hParent, TRUE);
  SetForegroundWindow(hParent);
  DestroyWindow(hDlg);
  if (hFont) DeleteObject(hFont);

  // Copy working data back if OK
  if (data.bResult) {
    wcscpy_s(m->szText, 256, data.szText);
    m->nFont = data.nFont;
    m->fSize = data.fSize;
    m->x = data.x;
    m->y = data.y;
    m->growth = data.growth;
    m->fTime = data.fTime;
    m->fFade = data.fFade;
    m->fFadeOut = data.fFadeOut;
    m->bOverrideFace = data.bOverrideFace ? 1 : 0;
    m->bOverrideBold = data.bOverrideBold ? 1 : 0;
    m->bOverrideItal = data.bOverrideItal ? 1 : 0;
    m->bOverrideColorR = data.bOverrideColorR ? 1 : 0;
    m->bOverrideColorG = data.bOverrideColorG ? 1 : 0;
    m->bOverrideColorB = data.bOverrideColorB ? 1 : 0;
    wcscpy_s(m->szFace, 128, data.szFace);
    m->bBold = data.bBold;
    m->bItal = data.bItal;
    m->nColorR = data.nColorR;
    m->nColorG = data.nColorG;
    m->nColorB = data.nColorB;
    m->bRandPos = data.bRandPos ? 1 : 0;
    m->bRandSize = data.bRandSize ? 1 : 0;
    m->bRandFont = data.bRandFont ? 1 : 0;
    m->bRandColor = data.bRandColor ? 1 : 0;
    m->bRandEffects = data.bRandEffects ? 1 : 0;
    m->bRandGrowth = data.bRandGrowth ? 1 : 0;
    m->bRandDuration = data.bRandDuration ? 1 : 0;
  } else {
    // Restore original message (Send Now may have modified it)
    *m = data.originalMsg;
  }

  return data.bResult;
}

// ======== Message Overrides Dialog ========

struct MsgOverridesDlgData {
  Engine*    plugin;
  HWND        hDlgWnd;
  bool        bResult;
  bool        bDone;

  // Working copies
  bool        bRandomFont;
  bool        bRandomColor;
  bool        bRandomSize;
  bool        bRandomEffects;
  float       fSizeMin;
  float       fSizeMax;
  int         nMaxOnScreen;
  // Animation overrides
  bool        bRandomPos;
  bool        bRandomGrowth;
  bool        bSlideIn;
  bool        bRandomDuration;
  bool        bShadow;
  bool        bBox;
  // Color shifting overrides
  bool        bApplyHueShift;
  bool        bRandomHue;
  // Per-message randomization
  bool        bIgnorePerMsg;
};

static LRESULT CALLBACK MsgOverridesWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  MsgOverridesDlgData* data = (MsgOverridesDlgData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

  switch (msg) {
  case WM_COMMAND: {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    // Owner-drawn checkbox toggle
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      bool bIsCheckbox = (bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox");
      if (bIsCheckbox) {
        bool wasChecked = (bool)(intptr_t)GetPropW(hCtrl, L"Checked");
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(wasChecked ? 0 : 1));
        InvalidateRect(hCtrl, NULL, TRUE);
      }
    }

    if (id == IDC_MSGOVERRIDE_OK && code == BN_CLICKED) {
      wchar_t buf[32];
      data->bRandomFont = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_RAND_FONT), L"Checked");
      data->bRandomColor = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_RAND_COLOR), L"Checked");
      data->bRandomSize = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_RAND_SIZE), L"Checked");
      data->bRandomEffects = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_RAND_EFFECTS), L"Checked");
      // Animation overrides
      data->bRandomPos = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_RAND_POS), L"Checked");
      data->bRandomGrowth = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_RAND_GROWTH), L"Checked");
      data->bSlideIn = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_SLIDE_IN), L"Checked");
      data->bRandomDuration = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_RAND_DURATION), L"Checked");
      data->bShadow = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_SHADOW), L"Checked");
      data->bBox = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_BOX), L"Checked");
      // Color shifting overrides
      data->bApplyHueShift = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_APPLY_HUE), L"Checked");
      data->bRandomHue = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_RAND_HUE), L"Checked");
      // Per-message randomization
      data->bIgnorePerMsg = (bool)(intptr_t)GetPropW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_IGNORE_PERMSG), L"Checked");

      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_SIZE_MIN), buf, 32);
      data->fSizeMin = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_SIZE_MAX), buf, 32);
      data->fSizeMax = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(hWnd, IDC_MSGOVERRIDE_MAX_ONSCREEN), buf, 32);
      data->nMaxOnScreen = _wtoi(buf);

      // Clamp values
      if (data->fSizeMin < 0.01f) data->fSizeMin = 0.01f;
      if (data->fSizeMax > 100.0f) data->fSizeMax = 100.0f;
      if (data->fSizeMin >= data->fSizeMax) data->fSizeMin = data->fSizeMax * 0.5f;
      if (data->nMaxOnScreen < 1) data->nMaxOnScreen = 1;
      if (data->nMaxOnScreen > NUM_SUPERTEXTS) data->nMaxOnScreen = NUM_SUPERTEXTS;

      data->bResult = true;
      data->bDone = true;
      return 0;
    }
    if (id == IDC_MSGOVERRIDE_CANCEL && code == BN_CLICKED) {
      data->bResult = false;
      data->bDone = true;
      return 0;
    }
    break;
  }

  case WM_DRAWITEM: {
    DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
    if (pDIS && pDIS->CtlType == ODT_BUTTON && data && data->plugin) {
      Engine* p = data->plugin;
      bool bIsCheckbox = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsCheckbox");
      if (bIsCheckbox) {
        DrawOwnerCheckbox(pDIS, p->m_bSettingsDarkTheme,
          p->m_colSettingsBg, p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText);
      } else {
        DrawOwnerButton(pDIS, p->m_bSettingsDarkTheme,
          p->m_colSettingsBtnFace, p->m_colSettingsBtnHi, p->m_colSettingsBtnShadow, p->m_colSettingsText);
      }
      return TRUE;
    }
    break;
  }

  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    if (data && data->plugin && data->plugin->m_bSettingsDarkTheme && data->plugin->m_hBrSettingsCtrlBg) {
      SetTextColor((HDC)wParam, data->plugin->m_colSettingsText);
      SetBkColor((HDC)wParam, data->plugin->m_colSettingsCtrlBg);
      return (LRESULT)data->plugin->m_hBrSettingsCtrlBg;
    }
    break;

  case WM_CTLCOLORSTATIC:
    if (data && data->plugin && data->plugin->m_bSettingsDarkTheme && data->plugin->m_hBrSettingsBg) {
      SetTextColor((HDC)wParam, data->plugin->m_colSettingsText);
      SetBkColor((HDC)wParam, data->plugin->m_colSettingsBg);
      return (LRESULT)data->plugin->m_hBrSettingsBg;
    }
    break;

  case WM_CTLCOLORBTN:
    if (data && data->plugin && data->plugin->m_bSettingsDarkTheme && data->plugin->m_hBrSettingsBg) {
      SetTextColor((HDC)wParam, data->plugin->m_colSettingsText);
      SetBkColor((HDC)wParam, data->plugin->m_colSettingsBg);
      return (LRESULT)data->plugin->m_hBrSettingsBg;
    }
    break;

  case WM_ERASEBKGND:
    if (data && data->plugin && data->plugin->m_bSettingsDarkTheme) {
      RECT rc; GetClientRect(hWnd, &rc);
      HBRUSH hBr = CreateSolidBrush(data->plugin->m_colSettingsBg);
      FillRect((HDC)wParam, &rc, hBr);
      DeleteObject(hBr);
      return 1;
    }
    break;

  case WM_CLOSE:
    if (data) { data->bResult = false; data->bDone = true; }
    return 0;

  case WM_KEYDOWN:
    if (wParam == VK_ESCAPE && data) { data->bResult = false; data->bDone = true; return 0; }
    if (wParam == VK_RETURN && data) {
      SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDC_MSGOVERRIDE_OK, BN_CLICKED), 0);
      return 0;
    }
    break;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool Engine::ShowMsgOverridesDialog(HWND hParent) {
  // Register window class (once)
  static bool registered = false;
  static const wchar_t* WND_CLASS = L"MDropDX12MsgOverrides";
  if (!registered) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MsgOverridesWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);
    registered = true;
  }

  // Initialize working copy from current settings
  MsgOverridesDlgData data = {};
  data.plugin = this;
  data.bResult = false;
  data.bDone = false;
  data.bRandomFont = m_bMsgOverrideRandomFont;
  data.bRandomColor = m_bMsgOverrideRandomColor;
  data.bRandomSize = m_bMsgOverrideRandomSize;
  data.bRandomEffects = m_bMsgOverrideRandomEffects;
  data.fSizeMin = m_fMsgOverrideSizeMin;
  data.fSizeMax = m_fMsgOverrideSizeMax;
  data.nMaxOnScreen = m_nMsgMaxOnScreen;
  data.bRandomPos = m_bMsgOverrideRandomPos;
  data.bRandomGrowth = m_bMsgOverrideRandomGrowth;
  data.bSlideIn = m_bMsgOverrideSlideIn;
  data.bRandomDuration = m_bMsgOverrideRandomDuration;
  data.bShadow = m_bMsgOverrideShadow;
  data.bBox = m_bMsgOverrideBox;
  data.bApplyHueShift = m_bMsgOverrideApplyHueShift;
  data.bRandomHue = m_bMsgOverrideRandomHue;
  data.bIgnorePerMsg = m_bMsgIgnorePerMsgRandom;

  // Create font for controls (use settings font size)
  HFONT hFont = CreateFontW(m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  // Compute line height from font metrics for proportional layout
  HDC hdcTmp = GetDC(hParent);
  HFONT hOldTmp = (HFONT)SelectObject(hdcTmp, hFont);
  TEXTMETRIC tmDlg = {};
  GetTextMetrics(hdcTmp, &tmDlg);
  SelectObject(hdcTmp, hOldTmp);
  ReleaseDC(hParent, hdcTmp);
  int dlgLineH = tmDlg.tmHeight + tmDlg.tmExternalLeading + 6;
  if (dlgLineH < 20) dlgLineH = 20;

  // Scale dialog dimensions from baseline (350x530 at lineH=20)
  int clientW = MulDiv(350, dlgLineH, 20);
  int clientH = MulDiv(530, dlgLineH, 20);
  DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  DWORD dwExStyle = WS_EX_DLGMODALFRAME;
  RECT rcSize = { 0, 0, clientW, clientH };
  AdjustWindowRectEx(&rcSize, dwStyle, FALSE, dwExStyle);
  int dlgW = rcSize.right - rcSize.left;
  int dlgH = rcSize.bottom - rcSize.top;

  // Center on parent
  RECT rcParent;
  GetWindowRect(hParent, &rcParent);
  HMONITOR hMon = MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  GetMonitorInfo(hMon, &mi);
  int px = rcParent.left + (rcParent.right - rcParent.left - dlgW) / 2;
  int py = rcParent.top + (rcParent.bottom - rcParent.top - dlgH) / 2;
  if (px < mi.rcWork.left) px = mi.rcWork.left;
  if (py < mi.rcWork.top) py = mi.rcWork.top;
  if (px + dlgW > mi.rcWork.right) px = mi.rcWork.right - dlgW;
  if (py + dlgH > mi.rcWork.bottom) py = mi.rcWork.bottom - dlgH;

  HWND hDlg = CreateWindowExW(dwExStyle,
    WND_CLASS, L"Message Overrides", dwStyle,
    px, py, dlgW, dlgH, hParent, NULL, GetModuleHandle(NULL), NULL);
  if (!hDlg) { DeleteObject(hFont); return false; }

  data.hDlgWnd = hDlg;
  SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)&data);

  // Scale layout constants from baseline (designed at lineH=20)
  int margin = MulDiv(16, dlgLineH, 20), rw = clientW - margin * 2;
  int editH = dlgLineH;
  int smallH = dlgLineH - 4;
  int btnH = dlgLineH + 4;
  int y = MulDiv(14, dlgLineH, 20);
  wchar_t buf[32];

  // Checkboxes
  CreateCheck(hDlg, L"Randomize font face", IDC_MSGOVERRIDE_RAND_FONT, margin, y, rw, editH, hFont, data.bRandomFont, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Randomize color", IDC_MSGOVERRIDE_RAND_COLOR, margin, y, rw, editH, hFont, data.bRandomColor, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Randomize effects (bold/italic)", IDC_MSGOVERRIDE_RAND_EFFECTS, margin, y, rw, editH, hFont, data.bRandomEffects, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Randomize size", IDC_MSGOVERRIDE_RAND_SIZE, margin, y, rw, editH, hFont, data.bRandomSize, true);
  y += dlgLineH + 4;

  // Size min/max
  int lblW2 = MulDiv(70, dlgLineH, 20), editW2 = MulDiv(50, dlgLineH, 20);
  int indent = MulDiv(20, dlgLineH, 20);
  CreateLabel(hDlg, L"Min size:", margin + indent, y + 2, lblW2, smallH, hFont);
  swprintf(buf, 32, L"%.2g", data.fSizeMin);
  CreateEdit(hDlg, buf, IDC_MSGOVERRIDE_SIZE_MIN, margin + indent + lblW2, y, editW2, editH, hFont, 0);
  CreateLabel(hDlg, L"Max size:", margin + indent + lblW2 + editW2 + 16, y + 2, lblW2, smallH, hFont);
  swprintf(buf, 32, L"%.2g", data.fSizeMax);
  CreateEdit(hDlg, buf, IDC_MSGOVERRIDE_SIZE_MAX, margin + indent + lblW2 * 2 + editW2 + 16, y, editW2, editH, hFont, 0);
  y += dlgLineH + 4;

  CreateLabel(hDlg, L"(min \x2265 0.01, max \x2264 100, 50 = normal)", margin + indent, y, rw, smallH, hFont);
  y += dlgLineH + 2;

  // Max on screen
  int maxLblW = MulDiv(170, dlgLineH, 20);
  CreateLabel(hDlg, L"Max messages on screen:", margin, y + 2, maxLblW, smallH, hFont);
  swprintf(buf, 32, L"%d", data.nMaxOnScreen);
  CreateEdit(hDlg, buf, IDC_MSGOVERRIDE_MAX_ONSCREEN, margin + maxLblW + 4, y, MulDiv(40, dlgLineH, 20), editH, hFont, 0);
  CreateLabel(hDlg, L"(1-10)", margin + maxLblW + MulDiv(50, dlgLineH, 20), y + 2, MulDiv(50, dlgLineH, 20), smallH, hFont);
  y += dlgLineH + 8;

  // --- Animations section ---
  CreateLabel(hDlg, L"Animations:", margin, y, rw, smallH, hFont);
  y += dlgLineH;
  CreateCheck(hDlg, L"Random position", IDC_MSGOVERRIDE_RAND_POS, margin, y, rw, editH, hFont, data.bRandomPos, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Random growth (text scales over time)", IDC_MSGOVERRIDE_RAND_GROWTH, margin, y, rw, editH, hFont, data.bRandomGrowth, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Slide in from edge", IDC_MSGOVERRIDE_SLIDE_IN, margin, y, rw, editH, hFont, data.bSlideIn, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Random duration (2\x2013" L"10 seconds)", IDC_MSGOVERRIDE_RAND_DURATION, margin, y, rw, editH, hFont, data.bRandomDuration, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Drop shadow", IDC_MSGOVERRIDE_SHADOW, margin, y, rw, editH, hFont, data.bShadow, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Background box", IDC_MSGOVERRIDE_BOX, margin, y, rw, editH, hFont, data.bBox, true);
  y += dlgLineH + 8;

  // --- Color Shifting section ---
  CreateLabel(hDlg, L"Color Shifting:", margin, y, rw, smallH, hFont);
  y += dlgLineH;
  CreateCheck(hDlg, L"Apply current hue shift", IDC_MSGOVERRIDE_APPLY_HUE, margin, y, rw, editH, hFont, data.bApplyHueShift, true);
  y += dlgLineH + 2;
  CreateCheck(hDlg, L"Random hue per message", IDC_MSGOVERRIDE_RAND_HUE, margin, y, rw, editH, hFont, data.bRandomHue, true);
  y += dlgLineH + 8;

  // --- Per-message section ---
  CreateLabel(hDlg, L"Per-Message:", margin, y, rw, smallH, hFont);
  y += dlgLineH;
  CreateCheck(hDlg, L"Ignore per-message randomization", IDC_MSGOVERRIDE_IGNORE_PERMSG, margin, y, rw, editH, hFont, data.bIgnorePerMsg, true);
  y += dlgLineH + 12;

  // OK / Cancel
  int okBtnW = MulDiv(80, dlgLineH, 20);
  CreateBtn(hDlg, L"OK", IDC_MSGOVERRIDE_OK, clientW / 2 - okBtnW - 10, y, okBtnW, btnH, hFont);
  CreateBtn(hDlg, L"Cancel", IDC_MSGOVERRIDE_CANCEL, clientW / 2 + 10, y, okBtnW, btnH, hFont);

  // Show dialog and make parent modal
  ShowWindow(hDlg, SW_SHOW);
  UpdateWindow(hDlg);
  EnableWindow(hParent, FALSE);

  // Local message loop
  MSG msg2;
  while (!data.bDone && GetMessage(&msg2, NULL, 0, 0)) {
    if (msg2.message == WM_KEYDOWN && msg2.wParam == VK_TAB) {
      HWND hNext = GetNextDlgTabItem(hDlg, GetFocus(), GetKeyState(VK_SHIFT) < 0);
      if (hNext) SetFocus(hNext);
      continue;
    }
    if (msg2.message == WM_KEYDOWN && msg2.wParam == VK_ESCAPE) {
      data.bResult = false;
      data.bDone = true;
      break;
    }
    TranslateMessage(&msg2);
    DispatchMessage(&msg2);
  }

  // Cleanup
  EnableWindow(hParent, TRUE);
  SetForegroundWindow(hParent);
  DestroyWindow(hDlg);
  if (hFont) DeleteObject(hFont);

  // Apply if OK
  if (data.bResult) {
    m_bMsgOverrideRandomFont = data.bRandomFont;
    m_bMsgOverrideRandomColor = data.bRandomColor;
    m_bMsgOverrideRandomSize = data.bRandomSize;
    m_bMsgOverrideRandomEffects = data.bRandomEffects;
    m_fMsgOverrideSizeMin = data.fSizeMin;
    m_fMsgOverrideSizeMax = data.fSizeMax;
    m_nMsgMaxOnScreen = data.nMaxOnScreen;
    m_bMsgOverrideRandomPos = data.bRandomPos;
    m_bMsgOverrideRandomGrowth = data.bRandomGrowth;
    m_bMsgOverrideSlideIn = data.bSlideIn;
    m_bMsgOverrideRandomDuration = data.bRandomDuration;
    m_bMsgOverrideShadow = data.bShadow;
    m_bMsgOverrideBox = data.bBox;
    m_bMsgOverrideApplyHueShift = data.bApplyHueShift;
    m_bMsgOverrideRandomHue = data.bRandomHue;
    m_bMsgIgnorePerMsgRandom = data.bIgnorePerMsg;
    SaveMsgAutoplaySettings();
  }

  return data.bResult;
}

void Engine::ReadCustomMessages() {
  int n;

  // First, clear all old data
  for (n = 0; n < MAX_CUSTOM_MESSAGE_FONTS; n++) {
    wcscpy(m_CustomMessageFont[n].szFace, L"arial");
    m_CustomMessageFont[n].bBold = false;
    m_CustomMessageFont[n].bItal = false;
    m_CustomMessageFont[n].nColorR = 255;
    m_CustomMessageFont[n].nColorG = 255;
    m_CustomMessageFont[n].nColorB = 255;
  }

  for (n = 0; n < MAX_CUSTOM_MESSAGES; n++) {
    m_CustomMessage[n].szText[0] = 0;
    m_CustomMessage[n].nFont = 0;
    m_CustomMessage[n].fSize = 50.0f;  // [0..100]  note that size is not absolute, but relative to the size of the window
    m_CustomMessage[n].x = 0.5f;
    m_CustomMessage[n].y = 0.5f;
    m_CustomMessage[n].randx = 0;
    m_CustomMessage[n].randy = 0;
    m_CustomMessage[n].growth = 1.0f;
    m_CustomMessage[n].fTime = 1.5f;
    m_CustomMessage[n].fFade = 0.2f;
    m_CustomMessage[n].fFadeOut = 0.0f;

    m_CustomMessage[n].bOverrideBold = false;
    m_CustomMessage[n].bOverrideItal = false;
    m_CustomMessage[n].bOverrideFace = false;
    m_CustomMessage[n].bOverrideColorR = false;
    m_CustomMessage[n].bOverrideColorG = false;
    m_CustomMessage[n].bOverrideColorB = false;
    m_CustomMessage[n].bBold = false;
    m_CustomMessage[n].bItal = false;
    wcscpy(m_CustomMessage[n].szFace, L"arial");
    m_CustomMessage[n].nColorR = 255;
    m_CustomMessage[n].nColorG = 255;
    m_CustomMessage[n].nColorB = 255;
    m_CustomMessage[n].nRandR = 0;
    m_CustomMessage[n].nRandG = 0;
    m_CustomMessage[n].nRandB = 0;
  }

  // Then read in the new file
  for (n = 0; n < MAX_CUSTOM_MESSAGE_FONTS; n++) {
    wchar_t szSectionName[32];
    swprintf(szSectionName, L"font%02d", n);

    // get face, bold, italic, x, y for this custom message FONT
    GetPrivateProfileStringW(szSectionName, L"face", L"arial", m_CustomMessageFont[n].szFace, sizeof(m_CustomMessageFont[n].szFace), m_szMsgIniFile);
    m_CustomMessageFont[n].bBold = GetPrivateProfileBoolW(szSectionName, L"bold", m_CustomMessageFont[n].bBold, m_szMsgIniFile);
    m_CustomMessageFont[n].bItal = GetPrivateProfileBoolW(szSectionName, L"ital", m_CustomMessageFont[n].bItal, m_szMsgIniFile);
    m_CustomMessageFont[n].nColorR = GetPrivateProfileIntW(szSectionName, L"r", m_CustomMessageFont[n].nColorR, m_szMsgIniFile);
    m_CustomMessageFont[n].nColorG = GetPrivateProfileIntW(szSectionName, L"g", m_CustomMessageFont[n].nColorG, m_szMsgIniFile);
    m_CustomMessageFont[n].nColorB = GetPrivateProfileIntW(szSectionName, L"b", m_CustomMessageFont[n].nColorB, m_szMsgIniFile);
  }

  for (n = 0; n < MAX_CUSTOM_MESSAGES; n++) {
    wchar_t szSectionName[64];
    swprintf(szSectionName, L"message%02d", n);

    // get fontID, size, text, etc. for this custom message:
    GetPrivateProfileStringW(szSectionName, L"text", L"", m_CustomMessage[n].szText, sizeof(m_CustomMessage[n].szText), m_szMsgIniFile);
    if (m_CustomMessage[n].szText[0]) {
      m_CustomMessage[n].nFont = GetPrivateProfileIntW(szSectionName, L"font", m_CustomMessage[n].nFont, m_szMsgIniFile);
      m_CustomMessage[n].fSize = GetPrivateProfileFloatW(szSectionName, L"size", m_CustomMessage[n].fSize, m_szMsgIniFile);
      m_CustomMessage[n].x = GetPrivateProfileFloatW(szSectionName, L"x", m_CustomMessage[n].x, m_szMsgIniFile);
      m_CustomMessage[n].y = GetPrivateProfileFloatW(szSectionName, L"y", m_CustomMessage[n].y, m_szMsgIniFile);
      m_CustomMessage[n].randx = GetPrivateProfileFloatW(szSectionName, L"randx", m_CustomMessage[n].randx, m_szMsgIniFile);
      m_CustomMessage[n].randy = GetPrivateProfileFloatW(szSectionName, L"randy", m_CustomMessage[n].randy, m_szMsgIniFile);

      m_CustomMessage[n].growth = GetPrivateProfileFloatW(szSectionName, L"growth", m_CustomMessage[n].growth, m_szMsgIniFile);
      m_CustomMessage[n].fTime = GetPrivateProfileFloatW(szSectionName, L"time", m_CustomMessage[n].fTime, m_szMsgIniFile);

      m_CustomMessage[n].fFade = GetPrivateProfileFloatW(szSectionName, L"fade", m_MessageDefaultFadeinTime, m_szMsgIniFile);
      m_CustomMessage[n].fFadeOut = GetPrivateProfileFloatW(szSectionName, L"fadeout", m_MessageDefaultFadeoutTime, m_szMsgIniFile);
      m_CustomMessage[n].fBurnTime = GetPrivateProfileFloatW(szSectionName, L"burntime", m_MessageDefaultBurnTime, m_szMsgIniFile);

      m_CustomMessage[n].nColorR = GetPrivateProfileIntW(szSectionName, L"r", m_CustomMessage[n].nColorR, m_szMsgIniFile);
      m_CustomMessage[n].nColorG = GetPrivateProfileIntW(szSectionName, L"g", m_CustomMessage[n].nColorG, m_szMsgIniFile);
      m_CustomMessage[n].nColorB = GetPrivateProfileIntW(szSectionName, L"b", m_CustomMessage[n].nColorB, m_szMsgIniFile);
      m_CustomMessage[n].nRandR = GetPrivateProfileIntW(szSectionName, L"randr", m_CustomMessage[n].nRandR, m_szMsgIniFile);
      m_CustomMessage[n].nRandG = GetPrivateProfileIntW(szSectionName, L"randg", m_CustomMessage[n].nRandG, m_szMsgIniFile);
      m_CustomMessage[n].nRandB = GetPrivateProfileIntW(szSectionName, L"randb", m_CustomMessage[n].nRandB, m_szMsgIniFile);

      // overrides: r,g,b,face,bold,ital
      GetPrivateProfileStringW(szSectionName, L"face", L"", m_CustomMessage[n].szFace, sizeof(m_CustomMessage[n].szFace), m_szMsgIniFile);
      m_CustomMessage[n].bBold = GetPrivateProfileIntW(szSectionName, L"bold", -1, m_szMsgIniFile);
      m_CustomMessage[n].bItal = GetPrivateProfileIntW(szSectionName, L"ital", -1, m_szMsgIniFile);
      m_CustomMessage[n].nColorR = GetPrivateProfileIntW(szSectionName, L"r", -1, m_szMsgIniFile);
      m_CustomMessage[n].nColorG = GetPrivateProfileIntW(szSectionName, L"g", -1, m_szMsgIniFile);
      m_CustomMessage[n].nColorB = GetPrivateProfileIntW(szSectionName, L"b", -1, m_szMsgIniFile);

      m_CustomMessage[n].bOverrideFace = (m_CustomMessage[n].szFace[0] != 0);
      m_CustomMessage[n].bOverrideBold = (m_CustomMessage[n].bBold != -1);
      m_CustomMessage[n].bOverrideItal = (m_CustomMessage[n].bItal != -1);
      m_CustomMessage[n].bOverrideColorR = (m_CustomMessage[n].nColorR != -1);
      m_CustomMessage[n].bOverrideColorG = (m_CustomMessage[n].nColorG != -1);
      m_CustomMessage[n].bOverrideColorB = (m_CustomMessage[n].nColorB != -1);

      // Per-message randomize flags
      m_CustomMessage[n].bRandPos = GetPrivateProfileIntW(szSectionName, L"rand_pos", 0, m_szMsgIniFile);
      m_CustomMessage[n].bRandSize = GetPrivateProfileIntW(szSectionName, L"rand_size", 0, m_szMsgIniFile);
      m_CustomMessage[n].bRandFont = GetPrivateProfileIntW(szSectionName, L"rand_font", 0, m_szMsgIniFile);
      m_CustomMessage[n].bRandColor = GetPrivateProfileIntW(szSectionName, L"rand_color", 0, m_szMsgIniFile);
      m_CustomMessage[n].bRandEffects = GetPrivateProfileIntW(szSectionName, L"rand_effects", 0, m_szMsgIniFile);
      m_CustomMessage[n].bRandGrowth = GetPrivateProfileIntW(szSectionName, L"rand_growth", 0, m_szMsgIniFile);
      m_CustomMessage[n].bRandDuration = GetPrivateProfileIntW(szSectionName, L"rand_duration", 0, m_szMsgIniFile);
    }
  }
}

void Engine::LaunchCustomMessage(int nMsgNum) {
  if (nMsgNum > 99)
    nMsgNum = 99;

  if (nMsgNum < 0) {
    int count = 0;
    // choose randomly
    for (nMsgNum = 0; nMsgNum < 100; nMsgNum++)
      if (m_CustomMessage[nMsgNum].szText[0])
        count++;

    int sel = (rand() % count) + 1;
    count = 0;
    for (nMsgNum = 0; nMsgNum < 100; nMsgNum++) {
      if (m_CustomMessage[nMsgNum].szText[0])
        count++;
      if (count == sel)
        break;
    }
  }

  if (nMsgNum < 0 ||
    nMsgNum >= MAX_CUSTOM_MESSAGES ||
    m_CustomMessage[nMsgNum].szText[0] == 0) {
    return;
  }

  int fontID = m_CustomMessage[nMsgNum].nFont;

  int nextFreeSupertextIndex = GetNextFreeSupertextIndex();
  if (nextFreeSupertextIndex > -1) {
    m_supertexts[nextFreeSupertextIndex].bRedrawSuperText = true;
    m_supertexts[nextFreeSupertextIndex].bIsSongTitle = false;
    lstrcpyW(m_supertexts[nextFreeSupertextIndex].szTextW, m_CustomMessage[nMsgNum].szText);

    // regular properties:
    m_supertexts[nextFreeSupertextIndex].fFontSize = m_CustomMessage[nMsgNum].fSize;
    m_supertexts[nextFreeSupertextIndex].fX = m_CustomMessage[nMsgNum].x + m_CustomMessage[nMsgNum].randx * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f);
    m_supertexts[nextFreeSupertextIndex].fY = m_CustomMessage[nMsgNum].y + m_CustomMessage[nMsgNum].randy * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f);
    m_supertexts[nextFreeSupertextIndex].fGrowth = m_CustomMessage[nMsgNum].growth;
    m_supertexts[nextFreeSupertextIndex].fDuration = m_CustomMessage[nMsgNum].fTime;
    m_supertexts[nextFreeSupertextIndex].fFadeInTime = m_CustomMessage[nMsgNum].fFade;
    m_supertexts[nextFreeSupertextIndex].fFadeOutTime = m_CustomMessage[nMsgNum].fFadeOut;
    m_supertexts[nextFreeSupertextIndex].fBurnTime = m_CustomMessage[nMsgNum].fBurnTime;

    // overrideables:
    if (m_CustomMessage[nMsgNum].bOverrideFace)
      lstrcpyW(m_supertexts[nextFreeSupertextIndex].nFontFace, m_CustomMessage[nMsgNum].szFace);
    else
      lstrcpyW(m_supertexts[nextFreeSupertextIndex].nFontFace, m_CustomMessageFont[fontID].szFace);
    m_supertexts[nextFreeSupertextIndex].bItal = (m_CustomMessage[nMsgNum].bOverrideItal) ? (m_CustomMessage[nMsgNum].bItal != 0) : (m_CustomMessageFont[fontID].bItal != 0);
    m_supertexts[nextFreeSupertextIndex].bBold = (m_CustomMessage[nMsgNum].bOverrideBold) ? (m_CustomMessage[nMsgNum].bBold != 0) : (m_CustomMessageFont[fontID].bBold != 0);
    m_supertexts[nextFreeSupertextIndex].nColorR = (m_CustomMessage[nMsgNum].bOverrideColorR) ? m_CustomMessage[nMsgNum].nColorR : m_CustomMessageFont[fontID].nColorR;
    m_supertexts[nextFreeSupertextIndex].nColorG = (m_CustomMessage[nMsgNum].bOverrideColorG) ? m_CustomMessage[nMsgNum].nColorG : m_CustomMessageFont[fontID].nColorG;
    m_supertexts[nextFreeSupertextIndex].nColorB = (m_CustomMessage[nMsgNum].bOverrideColorB) ? m_CustomMessage[nMsgNum].nColorB : m_CustomMessageFont[fontID].nColorB;

    // randomize color
    m_supertexts[nextFreeSupertextIndex].nColorR += (int)(m_CustomMessage[nMsgNum].nRandR * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
    m_supertexts[nextFreeSupertextIndex].nColorG += (int)(m_CustomMessage[nMsgNum].nRandG * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
    m_supertexts[nextFreeSupertextIndex].nColorB += (int)(m_CustomMessage[nMsgNum].nRandB * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
    if (m_supertexts[nextFreeSupertextIndex].nColorR < 0) m_supertexts[nextFreeSupertextIndex].nColorR = 0;
    if (m_supertexts[nextFreeSupertextIndex].nColorG < 0) m_supertexts[nextFreeSupertextIndex].nColorG = 0;
    if (m_supertexts[nextFreeSupertextIndex].nColorB < 0) m_supertexts[nextFreeSupertextIndex].nColorB = 0;
    if (m_supertexts[nextFreeSupertextIndex].nColorR > 255) m_supertexts[nextFreeSupertextIndex].nColorR = 255;
    if (m_supertexts[nextFreeSupertextIndex].nColorG > 255) m_supertexts[nextFreeSupertextIndex].nColorG = 255;
    if (m_supertexts[nextFreeSupertextIndex].nColorB > 255) m_supertexts[nextFreeSupertextIndex].nColorB = 255;

    // Apply global randomization overrides
    if (m_bMsgOverrideRandomFont) {
      int candidates[MAX_CUSTOM_MESSAGE_FONTS], nCandidates = 0;
      for (int f = 0; f < MAX_CUSTOM_MESSAGE_FONTS; f++)
        if (m_CustomMessageFont[f].szFace[0])
          candidates[nCandidates++] = f;
      if (nCandidates > 0) {
        int pick = candidates[rand() % nCandidates];
        lstrcpyW(m_supertexts[nextFreeSupertextIndex].nFontFace, m_CustomMessageFont[pick].szFace);
      }
    }
    if (m_bMsgOverrideRandomColor) {
      int pick = rand() % MAX_CUSTOM_MESSAGE_FONTS;
      m_supertexts[nextFreeSupertextIndex].nColorR = m_CustomMessageFont[pick].nColorR;
      m_supertexts[nextFreeSupertextIndex].nColorG = m_CustomMessageFont[pick].nColorG;
      m_supertexts[nextFreeSupertextIndex].nColorB = m_CustomMessageFont[pick].nColorB;
      if (m_supertexts[nextFreeSupertextIndex].nColorR < 0) m_supertexts[nextFreeSupertextIndex].nColorR = 255;
      if (m_supertexts[nextFreeSupertextIndex].nColorG < 0) m_supertexts[nextFreeSupertextIndex].nColorG = 255;
      if (m_supertexts[nextFreeSupertextIndex].nColorB < 0) m_supertexts[nextFreeSupertextIndex].nColorB = 255;
    }
    if (m_bMsgOverrideRandomEffects) {
      m_supertexts[nextFreeSupertextIndex].bBold = (rand() % 2) != 0;
      m_supertexts[nextFreeSupertextIndex].bItal = (rand() % 2) != 0;
    }
    if (m_bMsgOverrideRandomSize) {
      float range = m_fMsgOverrideSizeMax - m_fMsgOverrideSizeMin;
      m_supertexts[nextFreeSupertextIndex].fFontSize = m_fMsgOverrideSizeMin + range * ((rand() % 1000) / 1000.0f);
    }

    // Animation overrides
    td_supertext& st = m_supertexts[nextFreeSupertextIndex];
    if (m_bMsgOverrideRandomPos) {
      st.fX = 0.1f + (rand() % 800) / 1000.0f;
      st.fY = 0.1f + (rand() % 800) / 1000.0f;
    }
    if (m_bMsgOverrideRandomGrowth) {
      st.fGrowth = 0.5f + (rand() % 1500) / 1000.0f;
    }
    if (m_bMsgOverrideSlideIn) {
      int edge = rand() % 4;
      st.fStartX = (edge == 0) ? -0.3f : (edge == 1) ? 1.3f : st.fX;
      st.fStartY = (edge == 2) ? -0.3f : (edge == 3) ? 1.3f : st.fY;
      st.fMoveTime = 0.5f + (rand() % 500) / 1000.0f;
      st.nEaseMode = 2;
    }
    if (m_bMsgOverrideRandomDuration) {
      st.fDuration = 2.0f + (rand() % 8000) / 1000.0f;
    }
    if (m_bMsgOverrideShadow) {
      st.fShadowOffset = 2.0f;
    }
    if (m_bMsgOverrideBox) {
      st.fBoxAlpha = 0.5f;
      st.fBoxColR = 0; st.fBoxColG = 0; st.fBoxColB = 0;
    }
    // Color shifting overrides
    if (m_bMsgOverrideRandomHue) {
      float hue = (rand() % 3600) / 10.0f;
      HueRotateRGB(st.nColorR, st.nColorG, st.nColorB, hue);
    }
    if (m_bMsgOverrideApplyHueShift) {
      float hue = m_ColShiftHue * 360.0f;
      HueRotateRGB(st.nColorR, st.nColorG, st.nColorB, hue);
    }

    // Per-message randomization (unless globally ignored)
    if (!m_bMsgIgnorePerMsgRandom) {
      if (m_CustomMessage[nMsgNum].bRandFont) {
        int candidates[MAX_CUSTOM_MESSAGE_FONTS], nCandidates = 0;
        for (int f = 0; f < MAX_CUSTOM_MESSAGE_FONTS; f++)
          if (m_CustomMessageFont[f].szFace[0])
            candidates[nCandidates++] = f;
        if (nCandidates > 0) {
          int pick = candidates[rand() % nCandidates];
          lstrcpyW(st.nFontFace, m_CustomMessageFont[pick].szFace);
        }
      }
      if (m_CustomMessage[nMsgNum].bRandColor) {
        int pick = rand() % MAX_CUSTOM_MESSAGE_FONTS;
        st.nColorR = m_CustomMessageFont[pick].nColorR;
        st.nColorG = m_CustomMessageFont[pick].nColorG;
        st.nColorB = m_CustomMessageFont[pick].nColorB;
        if (st.nColorR < 0) st.nColorR = 255;
        if (st.nColorG < 0) st.nColorG = 255;
        if (st.nColorB < 0) st.nColorB = 255;
      }
      if (m_CustomMessage[nMsgNum].bRandEffects) {
        st.bBold = (rand() % 2) != 0;
        st.bItal = (rand() % 2) != 0;
      }
      if (m_CustomMessage[nMsgNum].bRandSize) {
        float range = m_fMsgOverrideSizeMax - m_fMsgOverrideSizeMin;
        st.fFontSize = m_fMsgOverrideSizeMin + range * ((rand() % 1000) / 1000.0f);
      }
      if (m_CustomMessage[nMsgNum].bRandPos) {
        st.fX = 0.1f + (rand() % 800) / 1000.0f;
        st.fY = 0.1f + (rand() % 800) / 1000.0f;
      }
      if (m_CustomMessage[nMsgNum].bRandGrowth) {
        st.fGrowth = 0.5f + (rand() % 1500) / 1000.0f;
      }
      if (m_CustomMessage[nMsgNum].bRandDuration) {
        st.fDuration = 2.0f + (rand() % 8000) / 1000.0f;
      }
    }

    m_supertexts[nextFreeSupertextIndex].fStartTime = GetTime();

  }
  // no free supertext slots available
  return;

}

void Engine::LaunchSongTitleAnim(int supertextIndex) {

  wchar_t debugMsg[128];
  swprintf(debugMsg, sizeof(debugMsg) / sizeof(debugMsg[0]), L"LaunchSongTitleAnim: supertextIndex=%d\n", supertextIndex);
  DebugLogW(debugMsg);

  if (supertextIndex == -1) {
    supertextIndex = GetNextFreeSupertextIndex();
  }
  m_supertexts[supertextIndex].bRedrawSuperText = true;
  m_supertexts[supertextIndex].bIsSongTitle = true;
  lstrcpyW(m_supertexts[supertextIndex].szTextW, m_szSongTitle);
  //lstrcpy(m_supertext[supertextIndex].szText, " ");
  lstrcpyW(m_supertexts[supertextIndex].nFontFace, m_fontinfo[SONGTITLE_FONT].szFace);
  m_supertexts[supertextIndex].fFontSize = (float)m_fontinfo[SONGTITLE_FONT].nSize;
  m_supertexts[supertextIndex].bBold = m_fontinfo[SONGTITLE_FONT].bBold;
  m_supertexts[supertextIndex].bItal = m_fontinfo[SONGTITLE_FONT].bItalic;
  m_supertexts[supertextIndex].fX = 0.5f;
  m_supertexts[supertextIndex].fY = 0.5f;
  m_supertexts[supertextIndex].fGrowth = 1.0f;
  m_supertexts[supertextIndex].fDuration = m_fSongTitleAnimDuration;
  m_supertexts[supertextIndex].nColorR = 255;
  m_supertexts[supertextIndex].nColorG = 255;
  m_supertexts[supertextIndex].nColorB = 255;

  m_supertexts[supertextIndex].fStartTime = GetTime();
}


// Convert std::wstring to LPCWSTR
LPCWSTR ConvertToLPCWSTR(const std::wstring& wstr) {
  return wstr.c_str();
}

void Engine::LaunchMessage(wchar_t* sMessage) {
  if (wcsncmp(sMessage, L"MSG|", 4) == 0) {

    std::wstring message(sMessage + 4); // Remove "MSG|"
    std::wstringstream ss(message);
    std::wstring token;
    std::map<std::wstring, std::wstring> params;

    // Parse key-value pairs
    while (std::getline(ss, token, L'|')) {
      size_t pos = token.find(L'=');
      if (pos != std::wstring::npos) {
        std::wstring key = token.substr(0, pos);
        std::wstring value = token.substr(pos + 1);
        params[key] = value;
      }
    }

    int nextFreeSupertextIndex = GetNextFreeSupertextIndex();
    // Set m_supertext properties
    if (params.find(L"text") != params.end()) {
      lstrcpyW(m_supertexts[nextFreeSupertextIndex].szTextW, ConvertToLPCWSTR(params[L"text"]));
    }
    else {
      return; // 'text' parameter is required
    }

    m_supertexts[nextFreeSupertextIndex].bRedrawSuperText = true;
    m_supertexts[nextFreeSupertextIndex].bIsSongTitle = false;

    if (params.find(L"font") != params.end()) {
      lstrcpyW(m_supertexts[nextFreeSupertextIndex].nFontFace, ConvertToLPCWSTR(params[L"font"]));
    }
    else {
      // Default font
      lstrcpyW(m_supertexts[nextFreeSupertextIndex].nFontFace, L"Segoe UI");
    }

    if (params.find(L"size") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fFontSize = std::stof(params[L"size"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].fFontSize = 30.0f; // Default size
    }

    if (params.find(L"x") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fX = std::stof(params[L"x"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].fX = 0.49f; // Default x position
    }

    if (params.find(L"y") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fY = std::stof(params[L"y"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].fY = 0.5f; // Default y position
    }

    if (params.find(L"randx") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fX += std::stof(params[L"randx"]) * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f);
    }

    if (params.find(L"randy") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fY += std::stof(params[L"randy"]) * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f);
    }

    if (params.find(L"growth") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fGrowth = std::stof(params[L"growth"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].fGrowth = 1.0f; // Default growth
    }

    if (params.find(L"time") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fDuration = std::stof(params[L"time"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].fDuration = 5.0f; // Default duration
    }

    if (params.find(L"fade") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fFadeInTime = std::stof(params[L"fade"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].fFadeInTime = m_MessageDefaultFadeinTime;
    }

    if (params.find(L"fadeout") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fFadeOutTime = std::stof(params[L"fadeout"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].fFadeOutTime = m_MessageDefaultFadeoutTime;
    }

    if (params.find(L"bold") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].bBold = std::stoi(params[L"bold"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].bBold = 0; // Default bold
    }

    if (params.find(L"ital") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].bItal = std::stoi(params[L"ital"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].bItal = 0; // Default italic
    }

    if (params.find(L"r") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorR = std::stoi(params[L"r"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].nColorR = 255; // Default red color
    }

    if (params.find(L"g") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorG = std::stoi(params[L"g"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].nColorG = 255; // Default green color
    }

    if (params.find(L"b") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorB = std::stoi(params[L"b"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].nColorB = 255; // Default blue color
    }

    if (params.find(L"randr") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorR += (int)(std::stof(params[L"randr"]) * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
    }

    if (params.find(L"randg") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorG += (int)(std::stof(params[L"randg"]) * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
    }

    if (params.find(L"randb") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorB += (int)(std::stof(params[L"randb"]) * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
    }

    if (m_supertexts[nextFreeSupertextIndex].nColorR < 0) m_supertexts[nextFreeSupertextIndex].nColorR = 0;
    if (m_supertexts[nextFreeSupertextIndex].nColorG < 0) m_supertexts[nextFreeSupertextIndex].nColorG = 0;
    if (m_supertexts[nextFreeSupertextIndex].nColorB < 0) m_supertexts[nextFreeSupertextIndex].nColorB = 0;
    if (m_supertexts[nextFreeSupertextIndex].nColorR > 255) m_supertexts[nextFreeSupertextIndex].nColorR = 255;
    if (m_supertexts[nextFreeSupertextIndex].nColorG > 255) m_supertexts[nextFreeSupertextIndex].nColorG = 255;
    if (m_supertexts[nextFreeSupertextIndex].nColorB > 255) m_supertexts[nextFreeSupertextIndex].nColorB = 255;


    if (params.find(L"startx") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fStartX = std::stof(params[L"startx"]);
    }

    if (params.find(L"starty") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fStartY = std::stof(params[L"starty"]);
    }

    if (params.find(L"movetime") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fMoveTime = std::stof(params[L"movetime"]);
    }

    if (params.find(L"easemode") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nEaseMode = std::stoi(params[L"easemode"]);
    }

    if (params.find(L"easefactor") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fEaseFactor = (float)std::stoi(params[L"easefactor"]);
    }

    if (params.find(L"shadowoffset") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fShadowOffset = std::stof(params[L"shadowoffset"]);
    }

    if (params.find(L"burntime") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fBurnTime = std::stof(params[L"burntime"]);
    }
    else {
      m_supertexts[nextFreeSupertextIndex].fBurnTime = m_MessageDefaultBurnTime;
    }

    if (params.find(L"box_alpha") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fBoxAlpha = std::stof(params[L"box_alpha"]);
    }
    if (params.find(L"box_col") != params.end()) {
      std::wstring colorStr = params[L"box_col"];
      std::wistringstream ss(colorStr);
      std::wstring token;
      std::vector<float> rgb;

      while (std::getline(ss, token, L',')) {
        try {
          rgb.push_back(std::stof(token));
        } catch (...) {
          rgb.push_back(0.0f); // fallback if parsing fails
        }
      }

      if (rgb.size() == 3) {
        m_supertexts[nextFreeSupertextIndex].fBoxColR = (int)rgb[0];
        m_supertexts[nextFreeSupertextIndex].fBoxColG = (int)rgb[1];
        m_supertexts[nextFreeSupertextIndex].fBoxColB = (int)rgb[2];
      }
    }

    if (params.find(L"box_left") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fBoxLeft = std::stof(params[L"box_left"]);
    }
    if (params.find(L"box_right") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fBoxRight = std::stof(params[L"box_right"]);
    }
    if (params.find(L"box_top") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fBoxTop = std::stof(params[L"box_top"]);
    }
    if (params.find(L"box_bottom") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fBoxBottom = std::stof(params[L"box_bottom"]);
    }

    m_supertexts[nextFreeSupertextIndex].fStartTime = GetTime();

    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      if (i != nextFreeSupertextIndex
        && m_supertexts[i].fStartTime != -1.0f
        && m_supertexts[i].fStartX == -100
        && m_supertexts[i].fStartY == -100
        && m_supertexts[i].fX == m_supertexts[nextFreeSupertextIndex].fX
        && m_supertexts[i].fY == m_supertexts[nextFreeSupertextIndex].fY) {
        // If the new supertext overlaps with an existing non-animated one, end it
        float fProgress = (GetTime() - m_supertexts[i].fStartTime) / m_supertexts[i].fDuration;
        // If text was growing, try keeping the current size
        if (m_supertexts[i].fGrowth != 1) {
          m_supertexts[i].fGrowth *= fProgress;
        }
        // set duration to the elapsed time, so burn-in is still applied
        m_supertexts[i].fDuration = GetTime() - m_supertexts[i].fStartTime;
      }
    }
  }
  else if (wcsncmp(sMessage, L"AMP|", 4) == 0) {
    // EQ message
    std::wstring message(sMessage + 4); // Remove "AMP|"
    std::wstringstream ss(message);
    std::wstring token;
    std::map<std::wstring, std::wstring> params;
    // Parse key-value pairs
    while (std::getline(ss, token, L'|')) {
      size_t pos = token.find(L'=');
      if (pos != std::wstring::npos) {
        std::wstring key = token.substr(0, pos);
        std::wstring value = token.substr(pos + 1);
        params[key] = value;
      }
    }
    if (params.find(L"l") != params.end() && params.find(L"r") != params.end()) {
      // Convert the std::wstring to a float using std::stof
      try {
        mdropdx12_amp_left = std::stof(params[L"l"]);
        mdropdx12_amp_right = std::stof(params[L"r"]);
      } catch (const std::exception&) {
        // Handle the error if the conversion fails
        mdropdx12_amp_left = 1.0f; // Default value
        mdropdx12_amp_right = 1.0f; // Default value
      }
    }
  }
  else if (wcsncmp(sMessage, L"PRESET=", 7) == 0) {
    // Find the position of ".milk" in the string
    // wchar_t* pos = wcsstr(sMessage, L".milk");
    // if (pos) {
    //   // Keep everything up to and including ".milk"
    //   pos[5] = L'\0'; // Truncate the string after ".milk"
    // }
    std::wstring message(sMessage + 7); // Remove "PRESET="

    size_t pos = message.find_last_of(L"\\/");
    std::wstring sPath;
    std::wstring sFilename;
    if (pos != std::wstring::npos) {
      // Extract the path up to and including the last separator
      sPath = message.substr(0, pos + 1);
      // Extract the filename after the last separator
      sFilename = message.substr(pos + 1);
    }
    else {
      // If no separator is found, assume the fullPath is just a filename
      sFilename = message;
    }

    if (sPath.length() > 0) {
      // Ensure 'sNewPath' is zero-terminated before using it in wcscmp
      wchar_t sNewPath[MAX_PATH];
      wcscpy_s(sNewPath, sPath.c_str());
      // ensure it is zero-terminated
      sNewPath[MAX_PATH - 1] = L'\0';
      if (wcscmp(sNewPath, g_engine.m_szPresetDir) != 0) {
        g_engine.ChangePresetDir(sNewPath, g_engine.m_szPresetDir);
      }
    }

    // try to set the current preset index
    for (size_t i = 0; i < m_presets.size(); i++) {
      if (wcscmp(m_presets[i].szFilename.c_str(), sFilename.c_str()) == 0) {
        m_nCurrentPreset = (int)i;
        break;
      }
    }

    LoadPreset(message.c_str(), 1);
    // Handle other message types here if needed
  }
  else if (wcsncmp(sMessage, L"WAVE|", 5) == 0) {
    std::wstring message(sMessage + 5);
    SetWaveParamsFromMessage(message);
  }
  else if (wcsncmp(sMessage, L"DEVICE=", 7) == 0) {
    std::wstring message(sMessage + 7);
    int newRequestType = 0;
    if (wcsncmp(message.c_str(), L"IN|", 3) == 0) {
      message = message.substr(3);
      newRequestType = 1;
    }
    else if (wcsncmp(message.c_str(), L"OUT|", 4) == 0) {
      message = message.substr(4);
      newRequestType = 2;
    }
    else {
      newRequestType = 0;
    }
    m_nAudioDeviceRequestType = newRequestType;
    wcscpy_s(g_engine.m_szAudioDevicePrevious, g_engine.m_szAudioDevice);
    g_engine.m_nAudioDevicePreviousType = g_engine.m_nAudioDeviceActiveType;
    wcscpy(g_engine.m_szAudioDevice, message.c_str());
    bool isRenderDevice = true;
    if (newRequestType == 1) {
      isRenderDevice = false;
    }
    else if (newRequestType == 2) {
      isRenderDevice = true;
    }
    g_engine.SetAudioDeviceDisplayName(message.c_str(), isRenderDevice);
    // Restart audio
    m_nAudioLoopState = 1;
  }
  else if (wcsncmp(sMessage, L"OPACITY=", 8) == 0) {
    std::wstring message(sMessage + 8);
    fOpacity = std::stof(message);
    SetOpacity(GetPluginWindow());
  }
  else if (wcsncmp(sMessage, L"STATE", 5) == 0) {
    int display = (int)std::ceil(100 * fOpacity);
    wchar_t buf[1024];
    swprintf(buf, 64, L"Opacity: %d%%", display); // Use %d for integers
    SendMessageToMDropDX12Remote((L"OPACITY=" + std::to_wstring(display)).c_str());
    SendPresetChangedInfoToMDropDX12Remote();
    SendSettingsInfoToMDropDX12Remote();
    if (m_nNumericInputMode == NUMERIC_INPUT_MODE_CUST_MSG) {
      PostMessageToMDropDX12Remote(WM_USER_MESSAGE_MODE);
    }
    else {
      PostMessageToMDropDX12Remote(WM_USER_SPRITE_MODE);
    }
  }
  else if (wcsncmp(sMessage, L"LINK=", 5) == 0) {
    std::wstring message(sMessage + 5);
    m_RemotePresetLink = std::stoi(message);
  }
  else if (wcsncmp(sMessage, L"QUICKSAVE", 9) == 0) {
    g_engine.SaveCurrentPresetToQuicksave(false);
  }
  else if (wcsncmp(sMessage, L"CONFIG", 6) == 0) {
    ReadConfig();
    // to update fonts
    AllocateDX9Stuff();
  }
  else if (wcsncmp(sMessage, L"SETTINGS", 8) == 0) {
    m_fTimeBetweenPresets = GetPrivateProfileFloatW(L"Settings", L"fTimeBetweenPresets", m_fTimeBetweenPresets, GetConfigIniFile());
    m_fPresetStartTime = GetTime();
    m_fNextPresetTime = -1.0f; // force recalculation
  }
  else if (wcsncmp(sMessage, L"TESTFONTS", 9) == 0) {
    ClearErrors(ERR_MSG_BOTTOM_EXTRA_1);
    ClearErrors(ERR_MSG_BOTTOM_EXTRA_2);
    ClearErrors(ERR_MSG_BOTTOM_EXTRA_3);
    // Send text to appear at the bottom first, assuming a bottom corner is used
    g_engine.AddError(L"Finally the Album", g_engine.m_SongInfoDisplaySeconds, ERR_MSG_BOTTOM_EXTRA_3, false);
    g_engine.AddError(L"Here goes the Title", g_engine.m_SongInfoDisplaySeconds, ERR_MSG_BOTTOM_EXTRA_2, false);
    g_engine.AddError(L"This is the Artist", g_engine.m_SongInfoDisplaySeconds, ERR_MSG_BOTTOM_EXTRA_1, false);
    if (!g_engine.m_bShowPresetInfo) g_engine.m_bShowPresetInfo = true;
    g_engine.AddNotification(L"This is a notification");
  }
  else if (wcsncmp(sMessage, L"TRACK|", 6) == 0) {
    // TRACK|artist=...|title=...|album=...  — track info from Milkwave Remote
    if (m_nTrackInfoSource == TRACK_SOURCE_IPC && mdropdx12) {
      std::wstring message(sMessage + 6);
      std::wstring artist, title, album;
      std::wistringstream ss(message);
      std::wstring token;
      while (std::getline(ss, token, L'|')) {
        size_t eq = token.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = token.substr(0, eq);
        std::wstring val = token.substr(eq + 1);
        if (key == L"artist") artist = val;
        else if (key == L"title") title = val;
        else if (key == L"album") album = val;
      }
      bool isChange = (artist != mdropdx12->currentArtist || title != mdropdx12->currentTitle || album != mdropdx12->currentAlbum);
      if (isChange) {
        mdropdx12->isSongChange = mdropdx12->currentArtist.length() || mdropdx12->currentTitle.length();
        mdropdx12->currentArtist = artist;
        mdropdx12->currentTitle = title;
        mdropdx12->currentAlbum = album;
        mdropdx12->updated = true;
      }
    }
  }
  else if (wcsncmp(sMessage, L"CLEARPRESET", 11) == 0) {
    ClearPreset();
  }
  else if (wcsncmp(sMessage, L"CLEARSPRITES", 12) == 0) {
    g_engine.KillAllSprites();
  }
  else if (wcsncmp(sMessage, L"CLEARTEXTS", 10) == 0) {
    g_engine.KillAllSupertexts();
  }
  else if (wcsncmp(sMessage, L"VAR_TIME=", 9) == 0) {
    std::wstring message(sMessage + 9);
    g_engine.m_timeFactor = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"VAR_FRAME=", 10) == 0) {
    std::wstring message(sMessage + 10);
    g_engine.m_frameFactor = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"VAR_FPS=", 8) == 0) {
    std::wstring message(sMessage + 8);
    g_engine.m_fpsFactor = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"VAR_INTENSITY=", 14) == 0) {
    std::wstring message(sMessage + 14);
    g_engine.m_VisIntensity = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"VAR_SHIFT=", 10) == 0) {
    std::wstring message(sMessage + 10);
    g_engine.m_VisShift = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"VAR_VERSION=", 12) == 0) {
    std::wstring message(sMessage + 12);
    g_engine.m_VisVersion = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"COL_HUE=", 8) == 0) {
    std::wstring message(sMessage + 8);
    g_engine.m_ColShiftHue = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"HUE_AUTO=", 9) == 0) {
    g_engine.m_AutoHue = (sMessage[9] == L'1');
  }
  else if (wcsncmp(sMessage, L"HUE_AUTO_SECONDS=", 17) == 0) {
    std::wstring message(sMessage + 17);
    g_engine.m_AutoHueSeconds = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"COL_SATURATION=", 15) == 0) {
    std::wstring message(sMessage + 15);
    g_engine.m_ColShiftSaturation = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"COL_BRIGHTNESS=", 15) == 0) {
    std::wstring message(sMessage + 15);
    g_engine.m_ColShiftBrightness = std::stof(message);
  }
  else if (wcsncmp(sMessage, L"VAR_QUALITY=", 12) == 0) {
    std::wstring message(sMessage + 12);
    g_engine.m_fRenderQuality = std::stof(message);
    ResetBufferAndFonts();
  }
  else if (wcsncmp(sMessage, L"VAR_AUTO=", 9) == 0) {
    g_engine.bQualityAuto = (sMessage[9] == L'1');
    ResetBufferAndFonts();
  }
  else if (wcsncmp(sMessage, L"SPOUT_ACTIVE=", 13) == 0) {
    wchar_t status = sMessage[13];
    if ((status == L'0' && bSpoutOut) || (status == L'1' && !bSpoutOut)) {
      ToggleSpout();
    }
  }
  else if (wcsncmp(sMessage, L"SPOUT_FIXEDSIZE=", 16) == 0) {
    wchar_t status = sMessage[16];
    if ((status == L'0' && bSpoutFixedSize) || (status == L'1' && !bSpoutFixedSize)) {
      SetSpoutFixedSize(true, true);
    }
  }
  else if (wcsncmp(sMessage, L"SPOUT_RESOLUTION=", 17) == 0) {
    std::wstring message(sMessage + 17);
    size_t pos = message.find(L'x');
    if (pos != std::wstring::npos) {
      std::wstring width = message.substr(0, pos);
      std::wstring height = message.substr(pos + 1);
      nSpoutFixedWidth = std::stoi(width);
      nSpoutFixedHeight = std::stoi(height);
      SetSpoutFixedSize(false, true);
    }
  }
  else if (wcsncmp(sMessage, L"CAPTURE", 7) == 0) {
    DebugLogW(L"[CAPTURE] Message received");
    mdropdx12->LogInfo(L"CAPTURE message received, calling CaptureScreenshot()");
    CaptureScreenshot();
    DebugLogW(L"[CAPTURE] CaptureScreenshot() returned");
  }
}

void Engine::SendPresetChangedInfoToMDropDX12Remote() {
  std::wstring msg = L"PRESET=" + std::wstring(m_szCurrentPresetFile);
  SendMessageToMDropDX12Remote(msg.c_str(), true);
  SendPresetWaveInfoToMDropDX12Remote();
}

void Engine::SendPresetWaveInfoToMDropDX12Remote() {
  std::wstring msg = L"WAVE|COLORR=" + std::to_wstring(static_cast<int>(std::ceil(g_engine.m_pState->m_fWaveR.eval(-1) * 255)))
    + L"|COLORG=" + std::to_wstring(static_cast<int>(std::ceil(g_engine.m_pState->m_fWaveG.eval(-1) * 255)))
    + L"|COLORB=" + std::to_wstring(static_cast<int>(std::ceil(g_engine.m_pState->m_fWaveB.eval(-1) * 255)))
    + L"|ALPHA=" + std::to_wstring(g_engine.m_pState->m_fWaveAlpha.eval(-1))
    + L"|MODE=" + std::to_wstring(static_cast<int>(g_engine.m_pState->m_nWaveMode))
    + L"|PUSHX=" + std::to_wstring(g_engine.m_pState->m_fXPush.eval(-1))
    + L"|PUSHY=" + std::to_wstring(g_engine.m_pState->m_fYPush.eval(-1))
    + L"|ZOOM=" + std::to_wstring(g_engine.m_pState->m_fZoom.eval(-1))
    + L"|WARP=" + std::to_wstring(g_engine.m_pState->m_fWarpAmount.eval(-1))
    + L"|ROTATION=" + std::to_wstring(g_engine.m_pState->m_fRot.eval(-1))
    + L"|DECAY=" + std::to_wstring(g_engine.m_pState->m_fDecay.eval(-1))
    + L"|SCALE=" + std::to_wstring(g_engine.m_pState->m_fWaveScale.eval(-1))
    + L"|ECHO=" + std::to_wstring(g_engine.m_pState->m_fVideoEchoZoom.eval(-1))
    + L"|BRIGHTEN=" + (g_engine.m_pState->m_bBrighten ? L"1" : L"0")
    + L"|DARKEN=" + (g_engine.m_pState->m_bDarken ? L"1" : L"0")
    + L"|SOLARIZE=" + (g_engine.m_pState->m_bSolarize ? L"1" : L"0")
    + L"|INVERT=" + (g_engine.m_pState->m_bInvert ? L"1" : L"0")
    + L"|ADDITIVE=" + (g_engine.m_pState->m_bAdditiveWaves ? L"1" : L"0")
    + L"|DOTTED=" + (g_engine.m_pState->m_bWaveDots ? L"1" : L"0")
    + L"|THICK=" + (g_engine.m_pState->m_bWaveThick ? L"1" : L"0")
    + L"|VOLALPHA=" + (g_engine.m_pState->m_bModWaveAlphaByVolume ? L"1" : L"0");
  SendMessageToMDropDX12Remote(msg.c_str(), true);
}

void Engine::SendSettingsInfoToMDropDX12Remote() {
  std::wstring msg = L"SETTINGS|ACTIVE=" + std::wstring(bSpoutOut ? L"1" : L"0")
    + L"|FIXEDSIZE=" + std::wstring(bSpoutFixedSize ? L"1" : L"0")
    + L"|FIXEDWIDTH=" + std::to_wstring(nSpoutFixedWidth)
    + L"|FIXEDHEIGHT=" + std::to_wstring(nSpoutFixedHeight)
    + L"|QUALITY=" + std::to_wstring(m_fRenderQuality)
    + L"|AUTO=" + std::wstring(bQualityAuto ? L"1" : L"0")
    + L"|HUE=" + std::to_wstring(m_ColShiftHue)
    + L"|LOCKED=" + std::wstring(m_bPresetLockedByUser ? L"1" : L"0");
  SendMessageToMDropDX12Remote(msg.c_str(), true);
}

void Engine::CaptureScreenshot() {
  wchar_t filename[MAX_PATH];
  CaptureScreenshotWithFilename(filename, MAX_PATH);
}

bool Engine::CaptureScreenshotWithFilename(wchar_t* outFilename, size_t outFilenameSize) {
  // Build filename from current preset name
  wchar_t presetName[MAX_PATH] = L"unknown";
  if (m_szCurrentPresetFile[0]) {
    wchar_t* filenameOnly = wcsrchr(m_szCurrentPresetFile, L'\\');
    if (filenameOnly) {
      filenameOnly++;
    } else {
      filenameOnly = m_szCurrentPresetFile;
    }

    wcsncpy_s(presetName, MAX_PATH, filenameOnly, _TRUNCATE);

    wchar_t* ext = wcsrchr(presetName, L'.');
    if (ext) *ext = L'\0';

    for (wchar_t* p = presetName; *p; p++) {
      if (*p == L'/' || *p == L':' || *p == L'*' ||
          *p == L'?' || *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|') {
        *p = L'_';
      }
    }
  }

  wchar_t captureDir[MAX_PATH];
  swprintf_s(captureDir, MAX_PATH, L"%scapture\\", m_szBaseDir);
  CreateDirectoryW(captureDir, NULL);

  SYSTEMTIME st;
  GetLocalTime(&st);

  wchar_t justFilename[MAX_PATH];
  swprintf_s(justFilename, MAX_PATH, L"%04d%02d%02d-%02d%02d%02d-%s.png",
    st.wYear, st.wMonth, st.wDay,
    st.wHour, st.wMinute, st.wSecond,
    presetName);

  // Store full path for deferred DX12 capture in DrawAndDisplay
  swprintf_s(m_screenshotPath, MAX_PATH, L"%s%s", captureDir, justFilename);
  m_bScreenshotRequested = true;

  if (outFilename && outFilenameSize > 0) {
    wcsncpy_s(outFilename, outFilenameSize, justFilename, _TRUNCATE);
  }
  return true;
}

void Engine::SetWaveParamsFromMessage(std::wstring& message) {
  std::wstringstream ss(message);
  std::wstring token;
  std::map<std::wstring, std::wstring> params;

  // Parse key-value pairs
  while (std::getline(ss, token, L'|')) {
    size_t pos = token.find(L'=');
    if (pos != std::wstring::npos) {
      std::wstring key = token.substr(0, pos);
      std::wstring value = token.substr(pos + 1);
      params[key] = value;
    }
  }

  if (params.find(L"MODE") != params.end()) {
    g_engine.m_pState->m_nWaveMode = std::stoi(params[L"MODE"]);
  }
  if (params.find(L"ALPHA") != params.end()) {
    g_engine.m_pState->m_fWaveAlpha = std::stof(params[L"ALPHA"]);
  }
  if (params.find(L"COLORR") != params.end()) {
    int colR = std::stoi(params[L"COLORR"]);
    float colRf = colR / 255.0f;
    g_engine.m_pState->m_fWaveR = colRf;
    g_engine.m_pState->m_fMvR = colRf;
  }
  if (params.find(L"COLORG") != params.end()) {
    int colG = std::stoi(params[L"COLORG"]);
    float colGf = colG / 255.0f;
    g_engine.m_pState->m_fWaveG = colGf;
    g_engine.m_pState->m_fMvG = colGf;
  }
  if (params.find(L"COLORB") != params.end()) {
    int colB = std::stoi(params[L"COLORB"]);
    float colBf = colB / 255.0f;
    g_engine.m_pState->m_fWaveB = colBf;
    g_engine.m_pState->m_fMvB = colBf;
  }
  if (params.find(L"PUSHX") != params.end()) {
    g_engine.m_pState->m_fXPush = std::stof(params[L"PUSHX"]);
  }
  if (params.find(L"PUSHY") != params.end()) {
    g_engine.m_pState->m_fYPush = std::stof(params[L"PUSHY"]);
  }
  if (params.find(L"ZOOM") != params.end()) {
    g_engine.m_pState->m_fZoom = std::stof(params[L"ZOOM"]);
  }
  if (params.find(L"WARP") != params.end()) {
    g_engine.m_pState->m_fWarpAmount = std::stof(params[L"WARP"]);
  }
  if (params.find(L"ROTATION") != params.end()) {
    g_engine.m_pState->m_fRot = std::stof(params[L"ROTATION"]);
  }
  if (params.find(L"DECAY") != params.end()) {
    g_engine.m_pState->m_fDecay = std::stof(params[L"DECAY"]);
  }
  if (params.find(L"SCALE") != params.end()) {
    g_engine.m_pState->m_fWaveScale = std::stof(params[L"SCALE"]);
  }
  if (params.find(L"ECHO") != params.end()) {
    g_engine.m_pState->m_fVideoEchoZoom = std::stof(params[L"ECHO"]);
  }
  if (params.find(L"BRIGHTEN") != params.end()) {
    g_engine.m_pState->m_bBrighten = params[L"BRIGHTEN"] == L"1";
  }
  if (params.find(L"DARKEN") != params.end()) {
    g_engine.m_pState->m_bDarken = params[L"DARKEN"] == L"1";
  }
  if (params.find(L"SOLARIZE") != params.end()) {
    g_engine.m_pState->m_bSolarize = params[L"SOLARIZE"] == L"1";
  }
  if (params.find(L"INVERT") != params.end()) {
    g_engine.m_pState->m_bInvert = params[L"INVERT"] == L"1";
  }
  if (params.find(L"ADDITIVE") != params.end()) {
    g_engine.m_pState->m_bAdditiveWaves = params[L"ADDITIVE"] == L"1";
  }
  if (params.find(L"DOTTED") != params.end()) {
    g_engine.m_pState->m_bWaveDots = params[L"DOTTED"] == L"1";
  }
  if (params.find(L"THICK") != params.end()) {
    g_engine.m_pState->m_bWaveThick = params[L"THICK"] == L"1";
  }
  if (params.find(L"VOLALPHA") != params.end()) {
    g_engine.m_pState->m_bModWaveAlphaByVolume = params[L"VOLALPHA"] == L"1";
  }
}

bool Engine::LaunchSprite(int nSpriteNum, int nSlot) {
  char initcode[8192], code[8192], sectionA[64];
  char szTemp[8192];
  wchar_t img[512], section[64];

  initcode[0] = 0;
  code[0] = 0;
  img[0] = 0;
  if (nSpriteNum < 100) {
    swprintf(section, L"img%02d", nSpriteNum);
    sprintf(sectionA, "img%02d", nSpriteNum);
  } else {
    swprintf(section, L"img%d", nSpriteNum);
    sprintf(sectionA, "img%d", nSpriteNum);
  }

  // 1. read in image filename
  GetPrivateProfileStringW(section, L"img", L"", img, sizeof(img) - 1, m_szImgIniFile);
  if (img[0] == 0) {
    wchar_t buf[1024];
    swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_ERROR_COULD_NOT_FIND_IMG_OR_NOT_DEFINED), nSpriteNum);
    AddError(buf, 7.0f, ERR_MISC, false);
    return false;
  }

  { wchar_t dbg[1024]; swprintf(dbg, 1024, L"LaunchSprite(%d): img=%s", nSpriteNum, img); DebugLogW(dbg, LOG_VERBOSE); }

  if (img[1] != L':')// || img[2] != '\\')
  {
    // it's not in the form "x:\blah\billy.jpg" so prepend base path.
    // Try content base path first, then fall back to plugin dir path.
    wchar_t temp[512];
    wcscpy(temp, img);
    if (m_szContentBasePath[0]) {
      swprintf(img, L"%s%s", m_szContentBasePath, temp);
      if (GetFileAttributesW(img) == INVALID_FILE_ATTRIBUTES)
        swprintf(img, L"%s%s", m_szMilkdrop2Path, temp);
    } else {
      swprintf(img, L"%s%s", m_szMilkdrop2Path, temp);
    }
  }
  { wchar_t dbg[1024]; swprintf(dbg, 1024, L"LaunchSprite(%d): resolved=%s exists=%d", nSpriteNum, img,
            GetFileAttributesW(img) != INVALID_FILE_ATTRIBUTES); DebugLogW(dbg, LOG_VERBOSE); }

  // 2. get color key
  //unsigned int ck_lo = (unsigned int)GetPrivateProfileInt(section, "colorkey_lo", 0x00000000, m_szImgIniFile);
  //unsigned int ck_hi = (unsigned int)GetPrivateProfileInt(section, "colorkey_hi", 0x00202020, m_szImgIniFile);
    // FIRST try 'colorkey_lo' (for backwards compatibility) and then try 'colorkey'
  unsigned int ck = (unsigned int)GetPrivateProfileIntW(section, L"colorkey_lo", 0x00000000, m_szImgIniFile/*GetConfigIniFile()*/);
  ck = (unsigned int)GetPrivateProfileIntW(section, L"colorkey", ck, m_szImgIniFile/*GetConfigIniFile()*/);

  // 3. read in init code & per-frame code
  for (int n = 0; n < 2; n++) {
    char* pStr = (n == 0) ? initcode : code;
    char szLineName[32];
    int len;

    int line = 1;
    int char_pos = 0;
    bool bDone = false;

    while (!bDone) {
      if (n == 0)
        sprintf(szLineName, "init_%d", line);
      else
        sprintf(szLineName, "code_%d", line);

      GetPrivateProfileString(sectionA, szLineName, "~!@#$", szTemp, 8192, AutoCharFn(m_szImgIniFile));	// fixme
      len = lstrlen(szTemp);

      if ((strcmp(szTemp, "~!@#$") == 0) ||		// if the key was missing,
        (len >= 8191 - char_pos - 1))			// or if we're out of space
      {
        bDone = true;
      }
      else {
        sprintf(&pStr[char_pos], "%s%c", szTemp, LINEFEED_CONTROL_CHAR);
      }

      char_pos += len + 1;
      line++;
    }
    pStr[char_pos++] = 0;	// null-terminate
  }

  if (nSlot == -1) {
    // find first empty slot; if none, chuck the oldest sprite & take its slot.
    int oldest_index = 0;
    int oldest_frame = m_texmgr.m_tex[0].nStartFrame;
    for (int x = 0; x < NUM_TEX; x++) {
      if (!m_texmgr.m_tex[x].pSurface) {
        nSlot = x;
        break;
      }
      else if (m_texmgr.m_tex[x].nStartFrame < oldest_frame) {
        oldest_index = x;
        oldest_frame = m_texmgr.m_tex[x].nStartFrame;
      }
    }

    if (nSlot == -1) {
      nSlot = oldest_index;
      m_texmgr.KillTex(nSlot);
    }
  }

  int ret = m_texmgr.LoadTex(img, nSlot, initcode, code, GetTime(), GetFrame(), ck);
  m_texmgr.m_tex[nSlot].nUserData = nSpriteNum;

  { wchar_t dbg[512]; swprintf(dbg, 512, L"LaunchSprite(%d): slot=%d ret=0x%X valid=%d pSurf=%p",
    nSpriteNum, nSlot, ret,
    m_texmgr.m_tex[nSlot].dx12Surface.IsValid() ? 1 : 0,
    m_texmgr.m_tex[nSlot].pSurface); DebugLogW(dbg, LOG_VERBOSE); }

  wchar_t buf[1024];
  switch (ret & TEXMGR_ERROR_MASK) {
  case TEXMGR_ERR_SUCCESS:
    switch (ret & TEXMGR_WARNING_MASK) {
    case TEXMGR_WARN_ERROR_IN_INIT_CODE:
      swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_WARNING_ERROR_IN_INIT_CODE), nSpriteNum);
      AddError(buf, 6.0f, ERR_MISC, true);
      break;
    case TEXMGR_WARN_ERROR_IN_REG_CODE:
      swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_WARNING_ERROR_IN_PER_FRAME_CODE), nSpriteNum);
      AddError(buf, 6.0f, ERR_MISC, true);
      break;
    default:
      // success; no errors OR warnings.
      break;
    }
    break;
  case TEXMGR_ERR_BAD_INDEX:
    swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_ERROR_BAD_SLOT_INDEX), nSpriteNum);
    AddError(buf, 6.0f, ERR_MISC, true);
    break;
    /*
      case TEXMGR_ERR_OPENING:                sprintf(m_szUserMessage, "sprite #%d error: unable to open imagefile", nSpriteNum); break;
    case TEXMGR_ERR_FORMAT:                 sprintf(m_szUserMessage, "sprite #%d error: file is corrupt or non-jpeg image", nSpriteNum); break;
    case TEXMGR_ERR_IMAGE_NOT_24_BIT:       sprintf(m_szUserMessage, "sprite #%d error: image does not have 3 color channels", nSpriteNum); break;
    case TEXMGR_ERR_IMAGE_TOO_LARGE:        sprintf(m_szUserMessage, "sprite #%d error: image is too large", nSpriteNum); break;
    case TEXMGR_ERR_CREATESURFACE_FAILED:   sprintf(m_szUserMessage, "sprite #%d error: createsurface() failed", nSpriteNum); break;
    case TEXMGR_ERR_LOCKSURFACE_FAILED:     sprintf(m_szUserMessage, "sprite #%d error: lock() failed", nSpriteNum); break;
    case TEXMGR_ERR_CORRUPT_JPEG:           sprintf(m_szUserMessage, "sprite #%d error: jpeg is corrupt", nSpriteNum); break;
      */
  case TEXMGR_ERR_BADFILE:
    swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_ERROR_IMAGE_FILE_MISSING_OR_CORRUPT), nSpriteNum);
    AddError(buf, 6.0f, ERR_MISC, true);
    break;
  case TEXMGR_ERR_OUTOFMEM:
    swprintf(buf, wasabiApiLangString(IDS_SPRITE_X_ERROR_OUT_OF_MEM), nSpriteNum);
    AddError(buf, 6.0f, ERR_MISC, true);
    break;
  }

  return (ret & TEXMGR_ERROR_MASK) ? false : true;
}

void Engine::KillSprite(int iSlot) {
  m_texmgr.KillTex(iSlot);
}

int SAMPLE_RATE = 44100; //Initialize sample rate globally, 44100hz is the default sample rate for MilkDrop

HRESULT DetectSampleRate() {
  HRESULT hr = S_OK;
  IMMDeviceEnumerator* pEnumerator = NULL;
  IMMDevice* pDevice = NULL;
  IPropertyStore* pProps = NULL;
  PROPVARIANT var;
  PropVariantInit(&var);

  // Initialize COM
  hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) return hr;

  // Create device enumerator
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
    CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
    (void**)&pEnumerator);
  if (FAILED(hr)) goto Cleanup;

  // Get default audio endpoint
  hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
  if (FAILED(hr)) goto Cleanup;

  // Open property store
  hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
  if (FAILED(hr)) goto Cleanup;

  // Get the format property
  hr = pProps->GetValue(PKEY_AudioEngine_DeviceFormat, &var);
  if (SUCCEEDED(hr) && var.vt == VT_BLOB) {
    WAVEFORMATEX* pwfx = (WAVEFORMATEX*)var.blob.pBlobData;
    if (pwfx != NULL) {
      SAMPLE_RATE = pwfx->nSamplesPerSec;
    }
  }

Cleanup:
  // Clean up
  PropVariantClear(&var);
  if (pProps) pProps->Release();
  if (pDevice) pDevice->Release();
  if (pEnumerator) pEnumerator->Release();
  CoUninitialize();

  return hr;
}

int Engine::GetNextFreeSupertextIndex() {
  int index = 0;
  for (int i = 0; i < NUM_SUPERTEXTS; i++) {
    if (m_supertexts[i].fStartTime == -1.0f) {
      index = i;
      break;
    }
  }
  // if no text is free, we'll reset and use index=0
  m_supertexts[index] = td_supertext(); // Reset the supertext at this index
  return index;
}

void Engine::DoCustomSoundAnalysis() {
  //Now uses configurations via beatdrop.ini, don't modify here.
    //Bass
  int BASS_MIN = m_nBassStart;
  int BASS_MAX = m_nBassEnd;

  //Middle
  int MID_MIN = m_nMidStart;
  int MID_MAX = m_nMidEnd;

  //Treble
  int TREBLE_MIN = m_nTrebStart;
  int TREBLE_MAX = m_nTrebEnd;

  // This uses the sample rate dependent on your speaker device.
  // Beat Detection Configuration
  // Look at the start of line 10566 for the new beat detection splitting algorithm.

  memcpy(mysound.fWave[0], m_sound.fWaveform[0], sizeof(float) * 576);
  memcpy(mysound.fWave[1], m_sound.fWaveform[1], sizeof(float) * 576);

  // do our own [UN-NORMALIZED] fft
  float fWaveLeft[576];
  float fWaveRight[576];
  for (int i = 0; i < 576; i++) {
    fWaveLeft[i] = m_sound.fWaveform[0][i]; //left channel
    fWaveRight[i] = m_sound.fWaveform[1][i]; //right channel
  }

  memset(mysound.fSpecLeft, 0, sizeof(float) * MY_FFT_SAMPLES);
  memset(mysound.fSpecRight, 0, sizeof(float) * MY_FFT_SAMPLES);

  myfft.time_to_frequency_domain(fWaveLeft, mysound.fSpecLeft);
  myfft.time_to_frequency_domain(fWaveRight, mysound.fSpecRight);
  //for (i=0; i<MY_FFT_SAMPLES; i++) fSpecLeft[i] = sqrtf(fSpecLeft[i]*fSpecLeft[i] + fSpecTemp[i]*fSpecTemp[i]);

  // DeepSeek - Update the sample rate (we don't need to check HRESULT every frame)
  static DWORD lastCheck = 0;
  DWORD currentTime = GetTickCount();
  if (currentTime - lastCheck > 5000) // Check once per second
  {
    DetectSampleRate();
    lastCheck = currentTime;
  }

  // sum spectrum up into 3 bands
  //DeepSeek - Updated Beat Detection Splitting Algorithm
  for (int i = 0; i < 3; i++) {
    // Calculate which FFT bins correspond to our frequency ranges
    int start_bin, end_bin;

    switch (i) {
    case 0: // Bass (0-250Hz)
      start_bin = (int)(BASS_MIN * MY_FFT_SAMPLES / (SAMPLE_RATE / 2));
      end_bin = (int)(BASS_MAX * MY_FFT_SAMPLES / (SAMPLE_RATE / 2));
      break;
    case 1: // Mid (250-4000Hz)
      start_bin = (int)(MID_MIN * MY_FFT_SAMPLES / (SAMPLE_RATE / 2));
      end_bin = (int)(MID_MAX * MY_FFT_SAMPLES / (SAMPLE_RATE / 2));
      break;
    case 2: // Treble (4000-20000Hz)
      start_bin = (int)(TREBLE_MIN * MY_FFT_SAMPLES / (SAMPLE_RATE / 2));
      end_bin = (int)(TREBLE_MAX * MY_FFT_SAMPLES / (SAMPLE_RATE / 2));
      break;
    }

    // Clamp values to valid range
    start_bin = max(0, min(start_bin, MY_FFT_SAMPLES - 1));
    end_bin = max(0, min(end_bin, MY_FFT_SAMPLES - 1));

    mysound.imm[i] = 0; //To prevent the waveform's spikyness and performance lag

    // Sum the energy in the frequency range
    for (int j = start_bin; j <= end_bin; j++) {
      mysound.imm[i] += (mysound.fSpecLeft[j] + mysound.fSpecRight[j]);
    }
  }

  int recentBufferSize = (int)GetFps();

  // do temporal blending to create attenuated and super-attenuated versions
  for (int i = 0; i < 3; i++) {
    float rate;

    if (mysound.imm[i] > mysound.avg[i])
      rate = 0.2f;
    else
      rate = 0.5f;
    rate = AdjustRateToFPS(rate, 30.0f, GetFps());
    mysound.avg[i] = mysound.avg[i] * rate + mysound.imm[i] * (1 - rate);

    if (GetFrame() < 50)
      rate = 0.9f;
    else
      rate = 0.992f;
    rate = AdjustRateToFPS(rate, 30.0f, GetFps());
    mysound.long_avg[i] = mysound.long_avg[i] * rate + mysound.imm[i] * (1 - rate);

    // also get bass/mid/treble levels *relative to the past*
    //changed all the values to 0 instead of 1 when it's no music
    if (fabsf(mysound.long_avg[i]) < 0.001f)
      mysound.imm_rel[i] = 0.0f;
    else
      mysound.imm_rel[i] = mysound.imm[i] / mysound.long_avg[i];

    if (fabsf(mysound.long_avg[i]) < 0.001f)
      mysound.avg_rel[i] = 0.0f;
    else
      mysound.avg_rel[i] = mysound.avg[i] / mysound.long_avg[i];

    if (mysound.recent[i].size() == 0) {
      mysound.recent[i] = std::vector<float>();
    }

    // smooth
    mysound.recent[i].push_back(mysound.imm_rel[i]);
    if ((int)mysound.recent[i].size() > recentBufferSize) {
      mysound.recent[i].erase(mysound.recent[i].begin());
    }
    mysound.smooth[i] = 0;
    int k = 0;
    for (; k < (int)mysound.recent[i].size(); k++) {
      mysound.smooth[i] += mysound.recent[i][k];
    }
    if (k > 0) {
      mysound.smooth[i] /= k;
    }

    if (fabsf(mysound.long_avg[i]) < 0.001f)
      mysound.smooth_rel[i] = 0.0f;
    else
      mysound.smooth_rel[i] = mysound.smooth[i] / mysound.long_avg[i];


    //wchar_t buffer[256];
    //swprintf(buffer, 256, L"[%i] %5.2f %5.2f %5.2f %5.2f\n", i, mysound.imm[i], mysound.imm_rel[i], mysound.avg_rel[i], mysound.smooth[i]);
    //OutputDebugStringW(buffer);
  }
}


void Engine::GetSongTitle(wchar_t* szSongTitle, int nSize) {
  lstrcpynW(szSongTitle, m_szSongTitle, nSize);
}

// =========================================================
// NOTE: SpoutSendFrame() has been replaced by SendToDisplayOutputs() in engine_displays.cpp
// The per-output Spout lifecycle is now managed there.
// =========================================================

// SPOUT initialization function
// Initializes SpoutDX12 sender via D3D11On12 interop
//
bool Engine::OpenSender(unsigned int width, unsigned int height) {
  SpoutLogNotice("Engine::OpenSender(%d, %d)", width, height);

  // Close existing sender
  SpoutReleaseWraps();
  if (bInitialized) {
    spoutsender.CloseDirectX12();
    bInitialized = false;
  }

  if (!m_lpDX || !m_lpDX->m_device || !m_lpDX->m_commandQueue) {
    DebugLogA("Spout: OpenSender failed - no DX12 device/queue", LOG_ERROR);
    return false;
  }

  // Give the sender a name
  spoutsender.SetSenderName(WinampSenderName);

  // Initialize SpoutDX12 with our DX12 device + command queue
  if (!spoutsender.OpenDirectX12(m_lpDX->m_device.Get(),
          reinterpret_cast<IUnknown**>(m_lpDX->m_commandQueue.GetAddressOf()))) {
    DebugLogA("Spout: OpenDirectX12 failed", LOG_ERROR);
    return false;
  }

  // Wrap each swap chain backbuffer for DX11 access
  for (int n = 0; n < DXC_FRAME_COUNT; n++) {
    if (!spoutsender.WrapDX12Resource(
            m_lpDX->m_renderTargets[n].Get(),
            &m_pWrappedBackBuffers[n],
            D3D12_RESOURCE_STATE_RENDER_TARGET)) {
      DebugLogA("Spout: WrapDX12Resource failed for backbuffer", LOG_ERROR);
      SpoutReleaseWraps();
      spoutsender.CloseDirectX12();
      return false;
    }
  }

  g_Width = width;
  g_Height = height;
  bSpoutOut = true;
  bInitialized = true;
  m_bSpoutDX12Ready = true;

  DebugLogA("Spout: DX12 sender initialized successfully");

  return true;

} // end OpenSender

// Release wrapped DX12 backbuffers
void Engine::SpoutReleaseWraps() {
  for (auto& w : m_pWrappedBackBuffers) {
    if (w) { w->Release(); w = nullptr; }
  }
  m_bSpoutDX12Ready = false;
}

void Engine::OpenMDropDX12Remote() {
  HWND hwnd = FindWindowW(NULL, L"MDropDX12 Remote");
  if (hwnd) {
    // Bring the window to the front  
    SetForegroundWindow(hwnd);
    ShowWindow(hwnd, SW_RESTORE);
  }
  else {
    // Start the program "MDropDX12Remote.exe"  
    // Ensure STARTUPINFOW is used for CreateProcessW
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(L"MDropDX12Remote.exe", NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
      g_engine.AddError(L"Could not start MDropDX12 Remote", 3.0f, ERR_MISC, false);
    }
    else {
      g_engine.AddNotification(L"Starting MDropDX12 Remote");
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
  }
}

void Engine::SetAudioDeviceDisplayName(const wchar_t* displayName, bool isRenderDevice) {
  m_nAudioDeviceActiveType = isRenderDevice ? 2 : 1;

  if (displayName == nullptr) {
    m_szAudioDeviceDisplayName[0] = L'\0';
    return;
  }

  std::wstring sanitized(displayName);

  auto removeDuplicateTag = [&sanitized](const wchar_t* tag) {
    size_t first = sanitized.find(tag);
    if (first == std::wstring::npos) {
      return;
    }

    size_t searchPos = first + wcslen(tag);
    while (true) {
      size_t next = sanitized.find(tag, searchPos);
      if (next == std::wstring::npos) {
        break;
      }
      sanitized.erase(next, wcslen(tag));
      if (next > 0 && sanitized[next - 1] == L' ') {
        sanitized.erase(next - 1, 1);
      }
      searchPos = next;
    }
    };

  removeDuplicateTag(L" [In]");
  removeDuplicateTag(L" [Out]");

  // collapse duplicate spaces
  size_t dupSpace;
  while ((dupSpace = sanitized.find(L"  ")) != std::wstring::npos) {
    sanitized.erase(dupSpace, 1);
  }

  wcsncpy_s(m_szAudioDeviceDisplayName, MAX_PATH, sanitized.c_str(), _TRUNCATE);
}

void Engine::SetAMDFlag() {
  if (m_AMDDetectionMode == 0) {
    m_IsAMD = is_amd_ati();
  }
  else if (m_AMDDetectionMode == 1) {
    m_IsAMD = true;
  }
  else {
    m_IsAMD = false;
  }
}

int Engine::GetPresetCount() { return m_nPresets; }
int Engine::GetCurrentPresetIndex() { return m_nCurrentPreset; }
const wchar_t* Engine::GetPresetName(int idx) {
  if (idx >= 0 && idx < m_nPresets)
    return m_presets[idx].szFilename.c_str();
  return L"";
}

bool Engine::CheckDX9DLL() {
  // Try to load the DLL manually
  HMODULE hD3DX = LoadLibrary(TEXT("D3DX9_43.dll"));

  if (!hD3DX) {
    ShowDirectXMissingMessage();
    return false;
  }

  // If successful, free the DLL (optional if you're linking statically)
  FreeLibrary(hD3DX);

  return true;
}

// Test for DirectX installation and warn if not installed
//
// Registry method only works for DirectX 9 and lower but that is OK
bool Engine::CheckForDirectX9c() {

  // HKLM\Software\Microsoft\DirectX\Version should be 4.09.00.0904
  // handy information : http://en.wikipedia.org/wiki/DirectX
  HKEY  hRegKey;
  LONG  regres;
  DWORD  dwSize, major, minor, revision, notused;
  char value[256];
  dwSize = 256;

  // Does the key exist
  regres = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\DirectX", NULL, KEY_READ, &hRegKey);
  if (regres == ERROR_SUCCESS) {
    // Read the key
    regres = RegQueryValueExA(hRegKey, "Version", 0, NULL, (LPBYTE)value, &dwSize);
    // Decode the string : 4.09.00.0904
    sscanf_s(value, "%d.%d.%d.%d", &major, &minor, &notused, &revision);
    // printf("DirectX registry : [%s] (%d.%d.%d.%d)\n", value, major, minor, notused, revision);
    RegCloseKey(hRegKey);
    if (major == 4 && minor == 9 && revision == 904)
      return true;
  }
  // If we get here, DirectX 9c is not installed
  ShowDirectXMissingMessage();
  return false;
}

void Engine::ShowDirectXMissingMessage() {
  if (MessageBoxA(NULL,
    "Could not initialize DirectX 9.\n\nPlease install the DirectX End-User Legacy Runtimes.\n\nOpen Download-Website now?",
    "MDropDX12 Visualizer", MB_YESNO | MB_SETFOREGROUND | MB_TOPMOST) == IDYES) {
    // open website in browser
    ShellExecuteA(NULL, "open", "https://www.microsoft.com/en-us/download/details.aspx?id=35", NULL, NULL, SW_SHOWNORMAL);
  }
}

// Spout functions - originally interleaved in engine.cpp
int Engine::ToggleSpout() {
  bSpoutChanged = true; // write config on exit
  bSpoutOut = !bSpoutOut;
  if (bSpoutOut) {
    AddNotification(L"Spout output enabled");
  }
  else {
    AddNotification(L"Spout output disabled");
  }

  // Sync first Spout output in m_displayOutputs
  for (auto& o : m_displayOutputs) {
    if (o.config.type == DisplayOutputType::Spout) {
      o.config.bEnabled = bSpoutOut;
      if (!bSpoutOut && o.spoutState) {
        DestroyDisplayOutput(o);
      }
      break;
    }
  }

  SetSpoutFixedSize(false, false);

  if (bInitialized || m_bSpoutDX12Ready) {
    SpoutReleaseWraps();
    spoutsender.CloseDirectX12();
    bInitialized = false;
  }

  ResetBufferAndFonts();
  SendSettingsInfoToMDropDX12Remote();
  return 0;
}

int Engine::SetSpoutFixedSize(bool toggleSwitch, bool showNotifications) {
  bSpoutChanged = true; // write config on exit
  if (toggleSwitch) {
    bSpoutFixedSize = !bSpoutFixedSize;
  }
  // Sync first Spout output in m_displayOutputs
  for (auto& o : m_displayOutputs) {
    if (o.config.type == DisplayOutputType::Spout) {
      o.config.bFixedSize = bSpoutFixedSize;
      o.config.nWidth = nSpoutFixedWidth;
      o.config.nHeight = nSpoutFixedHeight;
      break;
    }
  }
  if (IsSpoutActiveAndFixed()) {
    if (toggleSwitch && showNotifications) {
      std::wstring msg = L"Fixed Spout output size enabled ("
        + std::to_wstring(nSpoutFixedWidth) + L"x"
        + std::to_wstring(nSpoutFixedHeight) + L")";
      AddNotification(msg.data());
    }
    else if (showNotifications) {
      std::wstring msg = L"Spout output size set to "
        + std::to_wstring(nSpoutFixedWidth) + L"x"
        + std::to_wstring(nSpoutFixedHeight);
      AddNotification(msg.data());
    }
    // DX12 TODO: Fixed-size Spout requires a separate render target + copy/scale.
    // For now, Spout sends at window resolution regardless of fixed-size setting.
    ResetBufferAndFonts();
  }
  else {
    // bSpoutFixedSize OR bSpoutOut is false
    if (toggleSwitch && showNotifications && bSpoutOut) {
      AddNotification(L"Fixed Spout output size disabled");
    }
    ResetBufferAndFonts();
  }
  SendSettingsInfoToMDropDX12Remote();
  return 0;
}

//----------------------------------------------------------------------


} // namespace mdrop
