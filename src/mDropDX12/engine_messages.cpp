/*
  Plugin module: Messages, Sprites, Remote Communication & Audio
  Extracted from engine.cpp for maintainability.
  Contains: Custom messages, supertexts, sprites, song title animations,
            remote communication, screenshots, audio analysis, misc utilities
*/

#include "tcp_server.h"  // Must be before engine.h — winsock2.h must precede windows.h
#include "engine.h"
#include "engine_helpers.h"
#include "tool_window.h"
#include "pipe_server.h"
#include "audio_capture.h"
#include <thread>
#include "utility.h"
#include "AutoCharFn.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "shell_defines.h"
#include "wasabi.h"
#include "../ns-eel2/ns-eel.h"
extern "C" EEL_F * volatile nseel_gmembuf_default; // ns-eel2 global gmegabuf
#include <assert.h>
#include <strsafe.h>
#include <Windows.h>
#include <cstdint>
#include <sstream>
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
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
extern int SAMPLE_RATE;  // defined later in this file (DoCustomSoundAnalysis section)

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

    // Animation profile reference
    swprintf(val, 64, L"%d", m_CustomMessage[n].nAnimProfile);
    WritePrivateProfileStringW(section, L"animprofile", val, m_szMsgIniFile);
  }

  // Write animation profiles
  WriteAnimProfiles();
}

// ======== Animation Profiles ========

void Engine::CreateDefaultAnimProfiles() {
  m_nAnimProfileCount = 5;

  // 1. Center Pop
  m_AnimProfiles[0] = td_anim_profile();
  wcscpy(m_AnimProfiles[0].szName, L"Center Pop");
  m_AnimProfiles[0].fX = 0.5f;
  m_AnimProfiles[0].fY = 0.5f;
  m_AnimProfiles[0].fGrowth = 1.2f;
  m_AnimProfiles[0].fDuration = 3.0f;
  wcscpy(m_AnimProfiles[0].szFontFace, L"Bahnschrift");
  m_AnimProfiles[0].bBold = 1;

  // 2. Slide from Left
  m_AnimProfiles[1] = td_anim_profile();
  wcscpy(m_AnimProfiles[1].szName, L"Slide from Left");
  m_AnimProfiles[1].fX = 0.5f;
  m_AnimProfiles[1].fY = 0.5f;
  m_AnimProfiles[1].fStartX = -0.3f;
  m_AnimProfiles[1].fStartY = 0.5f;
  m_AnimProfiles[1].fMoveTime = 0.8f;
  m_AnimProfiles[1].nEaseMode = 2;
  m_AnimProfiles[1].fDuration = 5.0f;
  wcscpy(m_AnimProfiles[1].szFontFace, L"Segoe UI");

  // 3. Slide from Right
  m_AnimProfiles[2] = td_anim_profile();
  wcscpy(m_AnimProfiles[2].szName, L"Slide from Right");
  m_AnimProfiles[2].fX = 0.5f;
  m_AnimProfiles[2].fY = 0.5f;
  m_AnimProfiles[2].fStartX = 1.3f;
  m_AnimProfiles[2].fStartY = 0.5f;
  m_AnimProfiles[2].fMoveTime = 0.8f;
  m_AnimProfiles[2].nEaseMode = 2;
  m_AnimProfiles[2].fDuration = 5.0f;
  wcscpy(m_AnimProfiles[2].szFontFace, L"Segoe UI");

  // 4. Bottom Crawl
  m_AnimProfiles[3] = td_anim_profile();
  wcscpy(m_AnimProfiles[3].szName, L"Bottom Crawl");
  m_AnimProfiles[3].fX = 0.5f;
  m_AnimProfiles[3].fY = 0.85f;
  m_AnimProfiles[3].fGrowth = 1.0f;
  m_AnimProfiles[3].fDuration = 8.0f;
  m_AnimProfiles[3].fFadeOut = 2.0f;
  wcscpy(m_AnimProfiles[3].szFontFace, L"Segoe UI");

  // 5. Top Flash
  m_AnimProfiles[4] = td_anim_profile();
  wcscpy(m_AnimProfiles[4].szName, L"Top Flash");
  m_AnimProfiles[4].fX = 0.5f;
  m_AnimProfiles[4].fY = 0.15f;
  m_AnimProfiles[4].fGrowth = 0.8f;
  m_AnimProfiles[4].fDuration = 2.0f;
  m_AnimProfiles[4].bBold = 1;
  m_AnimProfiles[4].fBurnTime = 0.3f;
  wcscpy(m_AnimProfiles[4].szFontFace, L"Bahnschrift");
}

void Engine::ReadAnimProfiles() {
  m_nAnimProfileCount = GetPrivateProfileIntW(L"AnimProfiles", L"Count", 0, m_szMsgIniFile);

  if (m_nAnimProfileCount <= 0) {
    CreateDefaultAnimProfiles();
    WriteAnimProfiles();
    return;
  }

  if (m_nAnimProfileCount > MAX_ANIM_PROFILES)
    m_nAnimProfileCount = MAX_ANIM_PROFILES;

  for (int n = 0; n < m_nAnimProfileCount; n++) {
    m_AnimProfiles[n] = td_anim_profile();  // reset to defaults

    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);

    GetPrivateProfileStringW(section, L"name", L"", m_AnimProfiles[n].szName, 64, m_szMsgIniFile);
    m_AnimProfiles[n].bEnabled = GetPrivateProfileBoolW(section, L"enabled", true, m_szMsgIniFile);

    // Position
    m_AnimProfiles[n].fX = GetPrivateProfileFloatW(section, (wchar_t*)L"x", 0.5f, m_szMsgIniFile);
    m_AnimProfiles[n].fY = GetPrivateProfileFloatW(section, (wchar_t*)L"y", 0.5f, m_szMsgIniFile);
    m_AnimProfiles[n].fRandX = GetPrivateProfileFloatW(section, (wchar_t*)L"randx", 0.0f, m_szMsgIniFile);
    m_AnimProfiles[n].fRandY = GetPrivateProfileFloatW(section, (wchar_t*)L"randy", 0.0f, m_szMsgIniFile);

    // Entry animation
    m_AnimProfiles[n].fStartX = GetPrivateProfileFloatW(section, (wchar_t*)L"startx", -100.0f, m_szMsgIniFile);
    m_AnimProfiles[n].fStartY = GetPrivateProfileFloatW(section, (wchar_t*)L"starty", -100.0f, m_szMsgIniFile);
    m_AnimProfiles[n].fMoveTime = GetPrivateProfileFloatW(section, (wchar_t*)L"movetime", 0.0f, m_szMsgIniFile);
    m_AnimProfiles[n].nEaseMode = GetPrivateProfileIntW(section, L"easemode", 2, m_szMsgIniFile);
    m_AnimProfiles[n].fEaseFactor = GetPrivateProfileFloatW(section, (wchar_t*)L"easefactor", 2.0f, m_szMsgIniFile);

    // Appearance
    GetPrivateProfileStringW(section, L"face", L"", m_AnimProfiles[n].szFontFace, 128, m_szMsgIniFile);
    m_AnimProfiles[n].fFontSize = GetPrivateProfileFloatW(section, (wchar_t*)L"size", 50.0f, m_szMsgIniFile);
    m_AnimProfiles[n].bBold = GetPrivateProfileIntW(section, L"bold", 0, m_szMsgIniFile);
    m_AnimProfiles[n].bItal = GetPrivateProfileIntW(section, L"ital", 0, m_szMsgIniFile);
    m_AnimProfiles[n].nColorR = GetPrivateProfileIntW(section, L"r", 255, m_szMsgIniFile);
    m_AnimProfiles[n].nColorG = GetPrivateProfileIntW(section, L"g", 255, m_szMsgIniFile);
    m_AnimProfiles[n].nColorB = GetPrivateProfileIntW(section, L"b", 255, m_szMsgIniFile);
    m_AnimProfiles[n].nRandR = GetPrivateProfileIntW(section, L"randr", 0, m_szMsgIniFile);
    m_AnimProfiles[n].nRandG = GetPrivateProfileIntW(section, L"randg", 0, m_szMsgIniFile);
    m_AnimProfiles[n].nRandB = GetPrivateProfileIntW(section, L"randb", 0, m_szMsgIniFile);

    // Timing
    m_AnimProfiles[n].fDuration = GetPrivateProfileFloatW(section, (wchar_t*)L"duration", 5.0f, m_szMsgIniFile);
    m_AnimProfiles[n].fFadeIn = GetPrivateProfileFloatW(section, (wchar_t*)L"fadein", 0.2f, m_szMsgIniFile);
    m_AnimProfiles[n].fFadeOut = GetPrivateProfileFloatW(section, (wchar_t*)L"fadeout", 0.5f, m_szMsgIniFile);
    m_AnimProfiles[n].fBurnTime = GetPrivateProfileFloatW(section, (wchar_t*)L"burntime", 0.0f, m_szMsgIniFile);

    // Effects
    m_AnimProfiles[n].fGrowth = GetPrivateProfileFloatW(section, (wchar_t*)L"growth", 1.0f, m_szMsgIniFile);
    m_AnimProfiles[n].fShadowOffset = GetPrivateProfileFloatW(section, (wchar_t*)L"shadow", 0.0f, m_szMsgIniFile);
    m_AnimProfiles[n].fBoxAlpha = GetPrivateProfileFloatW(section, (wchar_t*)L"boxalpha", 0.0f, m_szMsgIniFile);
    m_AnimProfiles[n].nBoxColR = GetPrivateProfileIntW(section, L"boxr", 0, m_szMsgIniFile);
    m_AnimProfiles[n].nBoxColG = GetPrivateProfileIntW(section, L"boxg", 0, m_szMsgIniFile);
    m_AnimProfiles[n].nBoxColB = GetPrivateProfileIntW(section, L"boxb", 0, m_szMsgIniFile);

    // Randomization flags
    m_AnimProfiles[n].bRandPos = GetPrivateProfileIntW(section, L"rand_pos", 0, m_szMsgIniFile);
    m_AnimProfiles[n].bRandSize = GetPrivateProfileIntW(section, L"rand_size", 0, m_szMsgIniFile);
    m_AnimProfiles[n].bRandColor = GetPrivateProfileIntW(section, L"rand_color", 0, m_szMsgIniFile);
    m_AnimProfiles[n].bRandGrowth = GetPrivateProfileIntW(section, L"rand_growth", 0, m_szMsgIniFile);
    m_AnimProfiles[n].bRandDuration = GetPrivateProfileIntW(section, L"rand_duration", 0, m_szMsgIniFile);
  }
}

void Engine::WriteAnimProfiles() {
  wchar_t val[64];

  // Write count
  swprintf(val, 64, L"%d", m_nAnimProfileCount);
  WritePrivateProfileStringW(L"AnimProfiles", L"Count", val, m_szMsgIniFile);

  // Clear any old profiles beyond current count
  for (int n = m_nAnimProfileCount; n < MAX_ANIM_PROFILES; n++) {
    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);
    WritePrivateProfileStringW(section, NULL, NULL, m_szMsgIniFile);
  }

  for (int n = 0; n < m_nAnimProfileCount; n++) {
    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);

    WritePrivateProfileStringW(section, L"name", m_AnimProfiles[n].szName, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bEnabled ? 1 : 0);
    WritePrivateProfileStringW(section, L"enabled", val, m_szMsgIniFile);

    // Position
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fX);
    WritePrivateProfileStringW(section, L"x", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fY);
    WritePrivateProfileStringW(section, L"y", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fRandX);
    WritePrivateProfileStringW(section, L"randx", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fRandY);
    WritePrivateProfileStringW(section, L"randy", val, m_szMsgIniFile);

    // Entry animation
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fStartX);
    WritePrivateProfileStringW(section, L"startx", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fStartY);
    WritePrivateProfileStringW(section, L"starty", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fMoveTime);
    WritePrivateProfileStringW(section, L"movetime", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nEaseMode);
    WritePrivateProfileStringW(section, L"easemode", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fEaseFactor);
    WritePrivateProfileStringW(section, L"easefactor", val, m_szMsgIniFile);

    // Appearance
    WritePrivateProfileStringW(section, L"face", m_AnimProfiles[n].szFontFace, m_szMsgIniFile);
    swprintf(val, 64, L"%.1f", m_AnimProfiles[n].fFontSize);
    WritePrivateProfileStringW(section, L"size", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bBold);
    WritePrivateProfileStringW(section, L"bold", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bItal);
    WritePrivateProfileStringW(section, L"ital", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nColorR);
    WritePrivateProfileStringW(section, L"r", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nColorG);
    WritePrivateProfileStringW(section, L"g", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nColorB);
    WritePrivateProfileStringW(section, L"b", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nRandR);
    WritePrivateProfileStringW(section, L"randr", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nRandG);
    WritePrivateProfileStringW(section, L"randg", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nRandB);
    WritePrivateProfileStringW(section, L"randb", val, m_szMsgIniFile);

    // Timing
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fDuration);
    WritePrivateProfileStringW(section, L"duration", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fFadeIn);
    WritePrivateProfileStringW(section, L"fadein", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fFadeOut);
    WritePrivateProfileStringW(section, L"fadeout", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fBurnTime);
    WritePrivateProfileStringW(section, L"burntime", val, m_szMsgIniFile);

    // Effects
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fGrowth);
    WritePrivateProfileStringW(section, L"growth", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fShadowOffset);
    WritePrivateProfileStringW(section, L"shadow", val, m_szMsgIniFile);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fBoxAlpha);
    WritePrivateProfileStringW(section, L"boxalpha", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nBoxColR);
    WritePrivateProfileStringW(section, L"boxr", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nBoxColG);
    WritePrivateProfileStringW(section, L"boxg", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nBoxColB);
    WritePrivateProfileStringW(section, L"boxb", val, m_szMsgIniFile);

    // Randomization flags
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandPos);
    WritePrivateProfileStringW(section, L"rand_pos", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandSize);
    WritePrivateProfileStringW(section, L"rand_size", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandColor);
    WritePrivateProfileStringW(section, L"rand_color", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandGrowth);
    WritePrivateProfileStringW(section, L"rand_growth", val, m_szMsgIniFile);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandDuration);
    WritePrivateProfileStringW(section, L"rand_duration", val, m_szMsgIniFile);
  }
}

void Engine::ExportAnimProfiles(wchar_t* szPath) {
  wchar_t val[64];
  swprintf(val, 64, L"%d", m_nAnimProfileCount);
  WritePrivateProfileStringW(L"AnimProfiles", L"Count", val, szPath);

  for (int n = 0; n < m_nAnimProfileCount; n++) {
    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);
    const td_anim_profile& p = m_AnimProfiles[n];

    WritePrivateProfileStringW(section, L"name", p.szName, szPath);
    swprintf(val, 64, L"%d", p.bEnabled ? 1 : 0);
    WritePrivateProfileStringW(section, L"enabled", val, szPath);

    swprintf(val, 64, L"%.2f", p.fX); WritePrivateProfileStringW(section, L"x", val, szPath);
    swprintf(val, 64, L"%.2f", p.fY); WritePrivateProfileStringW(section, L"y", val, szPath);
    swprintf(val, 64, L"%.2f", p.fRandX); WritePrivateProfileStringW(section, L"randx", val, szPath);
    swprintf(val, 64, L"%.2f", p.fRandY); WritePrivateProfileStringW(section, L"randy", val, szPath);

    swprintf(val, 64, L"%.2f", p.fStartX); WritePrivateProfileStringW(section, L"startx", val, szPath);
    swprintf(val, 64, L"%.2f", p.fStartY); WritePrivateProfileStringW(section, L"starty", val, szPath);
    swprintf(val, 64, L"%.2f", p.fMoveTime); WritePrivateProfileStringW(section, L"movetime", val, szPath);
    swprintf(val, 64, L"%d", p.nEaseMode); WritePrivateProfileStringW(section, L"easemode", val, szPath);
    swprintf(val, 64, L"%.2f", p.fEaseFactor); WritePrivateProfileStringW(section, L"easefactor", val, szPath);

    WritePrivateProfileStringW(section, L"face", p.szFontFace, szPath);
    swprintf(val, 64, L"%.1f", p.fFontSize); WritePrivateProfileStringW(section, L"size", val, szPath);
    swprintf(val, 64, L"%d", p.bBold); WritePrivateProfileStringW(section, L"bold", val, szPath);
    swprintf(val, 64, L"%d", p.bItal); WritePrivateProfileStringW(section, L"ital", val, szPath);
    swprintf(val, 64, L"%d", p.nColorR); WritePrivateProfileStringW(section, L"r", val, szPath);
    swprintf(val, 64, L"%d", p.nColorG); WritePrivateProfileStringW(section, L"g", val, szPath);
    swprintf(val, 64, L"%d", p.nColorB); WritePrivateProfileStringW(section, L"b", val, szPath);
    swprintf(val, 64, L"%d", p.nRandR); WritePrivateProfileStringW(section, L"randr", val, szPath);
    swprintf(val, 64, L"%d", p.nRandG); WritePrivateProfileStringW(section, L"randg", val, szPath);
    swprintf(val, 64, L"%d", p.nRandB); WritePrivateProfileStringW(section, L"randb", val, szPath);

    swprintf(val, 64, L"%.2f", p.fDuration); WritePrivateProfileStringW(section, L"duration", val, szPath);
    swprintf(val, 64, L"%.2f", p.fFadeIn); WritePrivateProfileStringW(section, L"fadein", val, szPath);
    swprintf(val, 64, L"%.2f", p.fFadeOut); WritePrivateProfileStringW(section, L"fadeout", val, szPath);
    swprintf(val, 64, L"%.2f", p.fBurnTime); WritePrivateProfileStringW(section, L"burntime", val, szPath);

    swprintf(val, 64, L"%.2f", p.fGrowth); WritePrivateProfileStringW(section, L"growth", val, szPath);
    swprintf(val, 64, L"%.2f", p.fShadowOffset); WritePrivateProfileStringW(section, L"shadow", val, szPath);
    swprintf(val, 64, L"%.2f", p.fBoxAlpha); WritePrivateProfileStringW(section, L"boxalpha", val, szPath);
    swprintf(val, 64, L"%d", p.nBoxColR); WritePrivateProfileStringW(section, L"boxr", val, szPath);
    swprintf(val, 64, L"%d", p.nBoxColG); WritePrivateProfileStringW(section, L"boxg", val, szPath);
    swprintf(val, 64, L"%d", p.nBoxColB); WritePrivateProfileStringW(section, L"boxb", val, szPath);

    swprintf(val, 64, L"%d", p.bRandPos); WritePrivateProfileStringW(section, L"rand_pos", val, szPath);
    swprintf(val, 64, L"%d", p.bRandSize); WritePrivateProfileStringW(section, L"rand_size", val, szPath);
    swprintf(val, 64, L"%d", p.bRandColor); WritePrivateProfileStringW(section, L"rand_color", val, szPath);
    swprintf(val, 64, L"%d", p.bRandGrowth); WritePrivateProfileStringW(section, L"rand_growth", val, szPath);
    swprintf(val, 64, L"%d", p.bRandDuration); WritePrivateProfileStringW(section, L"rand_duration", val, szPath);
  }
}

void Engine::ImportAnimProfiles(wchar_t* szPath) {
  int count = GetPrivateProfileIntW(L"AnimProfiles", L"Count", 0, szPath);
  if (count <= 0) return;
  if (count > MAX_ANIM_PROFILES) count = MAX_ANIM_PROFILES;

  m_nAnimProfileCount = count;
  for (int n = 0; n < count; n++) {
    m_AnimProfiles[n] = td_anim_profile();
    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);

    GetPrivateProfileStringW(section, L"name", L"", m_AnimProfiles[n].szName, 64, szPath);
    m_AnimProfiles[n].bEnabled = GetPrivateProfileBoolW(section, L"enabled", true, szPath);

    m_AnimProfiles[n].fX = GetPrivateProfileFloatW(section, (wchar_t*)L"x", 0.5f, szPath);
    m_AnimProfiles[n].fY = GetPrivateProfileFloatW(section, (wchar_t*)L"y", 0.5f, szPath);
    m_AnimProfiles[n].fRandX = GetPrivateProfileFloatW(section, (wchar_t*)L"randx", 0.0f, szPath);
    m_AnimProfiles[n].fRandY = GetPrivateProfileFloatW(section, (wchar_t*)L"randy", 0.0f, szPath);

    m_AnimProfiles[n].fStartX = GetPrivateProfileFloatW(section, (wchar_t*)L"startx", -100.0f, szPath);
    m_AnimProfiles[n].fStartY = GetPrivateProfileFloatW(section, (wchar_t*)L"starty", -100.0f, szPath);
    m_AnimProfiles[n].fMoveTime = GetPrivateProfileFloatW(section, (wchar_t*)L"movetime", 0.0f, szPath);
    m_AnimProfiles[n].nEaseMode = GetPrivateProfileIntW(section, L"easemode", 2, szPath);
    m_AnimProfiles[n].fEaseFactor = GetPrivateProfileFloatW(section, (wchar_t*)L"easefactor", 2.0f, szPath);

    GetPrivateProfileStringW(section, L"face", L"", m_AnimProfiles[n].szFontFace, 128, szPath);
    m_AnimProfiles[n].fFontSize = GetPrivateProfileFloatW(section, (wchar_t*)L"size", 50.0f, szPath);
    m_AnimProfiles[n].bBold = GetPrivateProfileIntW(section, L"bold", 0, szPath);
    m_AnimProfiles[n].bItal = GetPrivateProfileIntW(section, L"ital", 0, szPath);
    m_AnimProfiles[n].nColorR = GetPrivateProfileIntW(section, L"r", 255, szPath);
    m_AnimProfiles[n].nColorG = GetPrivateProfileIntW(section, L"g", 255, szPath);
    m_AnimProfiles[n].nColorB = GetPrivateProfileIntW(section, L"b", 255, szPath);
    m_AnimProfiles[n].nRandR = GetPrivateProfileIntW(section, L"randr", 0, szPath);
    m_AnimProfiles[n].nRandG = GetPrivateProfileIntW(section, L"randg", 0, szPath);
    m_AnimProfiles[n].nRandB = GetPrivateProfileIntW(section, L"randb", 0, szPath);

    m_AnimProfiles[n].fDuration = GetPrivateProfileFloatW(section, (wchar_t*)L"duration", 5.0f, szPath);
    m_AnimProfiles[n].fFadeIn = GetPrivateProfileFloatW(section, (wchar_t*)L"fadein", 0.2f, szPath);
    m_AnimProfiles[n].fFadeOut = GetPrivateProfileFloatW(section, (wchar_t*)L"fadeout", 0.5f, szPath);
    m_AnimProfiles[n].fBurnTime = GetPrivateProfileFloatW(section, (wchar_t*)L"burntime", 0.0f, szPath);

    m_AnimProfiles[n].fGrowth = GetPrivateProfileFloatW(section, (wchar_t*)L"growth", 1.0f, szPath);
    m_AnimProfiles[n].fShadowOffset = GetPrivateProfileFloatW(section, (wchar_t*)L"shadow", 0.0f, szPath);
    m_AnimProfiles[n].fBoxAlpha = GetPrivateProfileFloatW(section, (wchar_t*)L"boxalpha", 0.0f, szPath);
    m_AnimProfiles[n].nBoxColR = GetPrivateProfileIntW(section, L"boxr", 0, szPath);
    m_AnimProfiles[n].nBoxColG = GetPrivateProfileIntW(section, L"boxg", 0, szPath);
    m_AnimProfiles[n].nBoxColB = GetPrivateProfileIntW(section, L"boxb", 0, szPath);

    m_AnimProfiles[n].bRandPos = GetPrivateProfileIntW(section, L"rand_pos", 0, szPath);
    m_AnimProfiles[n].bRandSize = GetPrivateProfileIntW(section, L"rand_size", 0, szPath);
    m_AnimProfiles[n].bRandColor = GetPrivateProfileIntW(section, L"rand_color", 0, szPath);
    m_AnimProfiles[n].bRandGrowth = GetPrivateProfileIntW(section, L"rand_growth", 0, szPath);
    m_AnimProfiles[n].bRandDuration = GetPrivateProfileIntW(section, L"rand_duration", 0, szPath);
  }
}

int Engine::PickRandomAnimProfile() {
  int pool[MAX_ANIM_PROFILES];
  int poolSize = 0;
  for (int i = 0; i < m_nAnimProfileCount; i++) {
    if (m_AnimProfiles[i].bEnabled)
      pool[poolSize++] = i;
  }
  if (poolSize == 0) return -1;
  return pool[rand() % poolSize];
}

void Engine::ApplyAnimProfileToSupertext(td_supertext& st, const td_anim_profile& prof) {
  // Position
  st.fX = prof.fX;
  st.fY = prof.fY;
  if (prof.fRandX != 0.0f) st.fX += prof.fRandX * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f);
  if (prof.fRandY != 0.0f) st.fY += prof.fRandY * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f);

  // Entry animation
  st.fStartX = prof.fStartX;
  st.fStartY = prof.fStartY;
  st.fMoveTime = prof.fMoveTime;
  st.nEaseMode = prof.nEaseMode;
  st.fEaseFactor = prof.fEaseFactor;

  // Appearance
  if (prof.szFontFace[0])
    wcscpy(st.nFontFace, prof.szFontFace);
  st.fFontSize = prof.fFontSize;
  st.bBold = prof.bBold;
  st.bItal = prof.bItal;
  st.nColorR = prof.nColorR;
  st.nColorG = prof.nColorG;
  st.nColorB = prof.nColorB;
  if (prof.nRandR) st.nColorR += (int)(prof.nRandR * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
  if (prof.nRandG) st.nColorG += (int)(prof.nRandG * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
  if (prof.nRandB) st.nColorB += (int)(prof.nRandB * ((rand() % 1037) / 1037.0f * 2.0f - 1.0f));
  st.nColorR = max(0, min(255, st.nColorR));
  st.nColorG = max(0, min(255, st.nColorG));
  st.nColorB = max(0, min(255, st.nColorB));

  // Timing
  st.fDuration = prof.fDuration;
  st.fFadeInTime = prof.fFadeIn;
  st.fFadeOutTime = prof.fFadeOut;
  st.fBurnTime = prof.fBurnTime;

  // Effects
  st.fGrowth = prof.fGrowth;
  st.fShadowOffset = prof.fShadowOffset;
  st.fBoxAlpha = prof.fBoxAlpha;
  st.fBoxColR = prof.nBoxColR;
  st.fBoxColG = prof.nBoxColG;
  st.fBoxColB = prof.nBoxColB;

  // Per-trigger randomization
  if (prof.bRandPos) {
    st.fX = (rand() % 1037) / 1037.0f * 0.6f + 0.2f;
    st.fY = (rand() % 1037) / 1037.0f * 0.6f + 0.2f;
  }
  if (prof.bRandSize) {
    st.fFontSize = 20.0f + (rand() % 1037) / 1037.0f * 60.0f;
  }
  if (prof.bRandColor) {
    st.nColorR = rand() % 256;
    st.nColorG = rand() % 256;
    st.nColorB = rand() % 256;
  }
  if (prof.bRandGrowth) {
    st.fGrowth = 0.5f + (rand() % 1037) / 1037.0f * 1.5f;
  }
  if (prof.bRandDuration) {
    st.fDuration = 1.0f + (rand() % 1037) / 1037.0f * 9.0f;
  }
}

void Engine::PushSongTitleAsMessage() {
  if (m_szSongTitle[0] == 0) return;

  int idx = GetNextFreeSupertextIndex();
  td_supertext& st = m_supertexts[idx];
  st = td_supertext();  // reset

  wcscpy(st.szTextW, m_szSongTitle);
  st.bIsSongTitle = false;
  st.bRedrawSuperText = true;

  // Determine profile
  int profIdx = m_nSongTitleAnimProfile;
  if (profIdx == -2) profIdx = PickRandomAnimProfile();

  if (profIdx >= 0 && profIdx < m_nAnimProfileCount) {
    ApplyAnimProfileToSupertext(st, m_AnimProfiles[profIdx]);
  } else {
    // Fallback defaults
    wcscpy(st.nFontFace, L"Segoe UI");
    st.fFontSize = 50.0f;
    st.fX = 0.5f;
    st.fY = 0.5f;
    st.fDuration = 5.0f;
    st.fFadeInTime = 0.2f;
    st.fGrowth = 1.0f;
    st.nColorR = 255;
    st.nColorG = 255;
    st.nColorB = 255;
  }

  st.fStartTime = GetTime();
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
  m_bMessageAutoSize = GetPrivateProfileIntW(L"Milkwave", L"MessageAutoSize", 1, pIni) != 0;

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

// ======== Message Edit Dialog (ModalDialog subclass) ========

class MsgEditDialog : public mdrop::ModalDialog {
public:
  MsgEditDialog(Engine* pEngine) : ModalDialog(pEngine) {}

  // Context
  int  msgIndex = 0;
  bool isNew = false;

  // Working copy of message fields
  wchar_t szText[256] = {};
  int  nFont = 0;
  float fSize = 50.0f, x = 0.5f, y = 0.5f, growth = 1.0f;
  float fTime = 5.0f, fFade = 1.0f, fFadeOut = 1.0f;

  // Font override working copy
  bool bOverrideFace = false, bOverrideBold = false, bOverrideItal = false;
  bool bOverrideColorR = false, bOverrideColorG = false, bOverrideColorB = false;
  wchar_t szFace[128] = {};
  int  bBold = -1, bItal = -1;
  int  nColorR = -1, nColorG = -1, nColorB = -1;

  // Animation profile
  int nAnimProfile = -1;

  // Per-message randomize working copies
  bool bRandPos = false, bRandSize = false, bRandFont = false, bRandColor = false;
  bool bRandEffects = false, bRandGrowth = false, bRandDuration = false;

  // Original message backup (for Send Now + Cancel)
  td_custom_msg originalMsg = {};

  static COLORREF s_acrCustColors[16];

protected:
  const wchar_t* GetDialogTitle() const override {
    return isNew ? L"Add Message" : L"Edit Message";
  }
  const wchar_t* GetDialogClass() const override { return L"MDropDX12MsgEdit"; }

  void DoBuildControls(int clientW, int clientH) override {
    HFONT hFont = GetFont();
    HINSTANCE hInst = GetModuleHandle(NULL);
    int lineH = GetLineHeight();
    int margin = MulDiv(12, lineH, 20);
    int rw = clientW - margin * 2;
    int lblW = MulDiv(90, lineH, 20), editW = MulDiv(60, lineH, 20);
    int yPos = MulDiv(10, lineH, 20);
    int xVal = margin + lblW + 4;
    int smallH = lineH - 4;
    int editH = lineH;
    int btnH = lineH + 4;
    int textEditH = lineH * 2 + 8;
    wchar_t buf[64];

    // Text label + edit
    TrackControl(CreateLabel(m_hWnd, L"Message Text:", margin, yPos, rw, smallH, hFont));
    yPos += smallH + 2;
    TrackControl(CreateEdit(m_hWnd, szText, IDC_MSGEDIT_TEXT, margin, yPos, rw, textEditH, hFont,
      ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL));
    yPos += textEditH + 6;

    // Font section
    TrackControl(CreateLabel(m_hWnd, L"Base Font:", margin, yPos + 2, MulDiv(70, lineH, 20), smallH, hFont));
    m_hFontCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
      margin + MulDiv(74, lineH, 20), yPos, rw - MulDiv(74, lineH, 20), 300, m_hWnd,
      (HMENU)(INT_PTR)IDC_MSGEDIT_FONT_COMBO, hInst, NULL);
    if (m_hFontCombo) SendMessage(m_hFontCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(m_hFontCombo);
    for (int i = 0; i < MAX_CUSTOM_MESSAGE_FONTS; i++) {
      wchar_t entry[160];
      swprintf(entry, 160, L"Font %02d: %s%s%s", i,
        m_pEngine->m_CustomMessageFont[i].szFace,
        m_pEngine->m_CustomMessageFont[i].bBold ? L" [Bold]" : L"",
        m_pEngine->m_CustomMessageFont[i].bItal ? L" [Italic]" : L"");
      SendMessageW(m_hFontCombo, CB_ADDSTRING, 0, (LPARAM)entry);
    }
    SendMessage(m_hFontCombo, CB_SETCURSEL, nFont, 0);
    yPos += lineH + 4;

    // Choose Font, Choose Color, Color Swatch
    int chooseBtnW = MulDiv(110, lineH, 20);
    TrackControl(CreateBtn(m_hWnd, L"Choose Font...", IDC_MSGEDIT_CHOOSE_FONT, margin, yPos, chooseBtnW, btnH, hFont));
    TrackControl(CreateBtn(m_hWnd, L"Choose Color...", IDC_MSGEDIT_CHOOSE_COLOR, margin + chooseBtnW + 6, yPos, chooseBtnW, btnH, hFont));
    // Color swatch (owner-drawn static, uses SwatchColor prop for HandleDarkDrawItem)
    int swatchSize = smallH;
    HWND hSwatch = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"",
      WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY,
      margin + chooseBtnW * 2 + 12, yPos + 2, swatchSize, swatchSize, m_hWnd,
      (HMENU)(INT_PTR)IDC_MSGEDIT_COLOR_SWATCH, hInst, NULL);
    TrackControl(hSwatch);
    yPos += btnH + 6;

    // Font preview
    HWND hPreview = CreateLabel(m_hWnd, L"", margin, yPos, rw, smallH, hFont);
    SetWindowLongPtr(hPreview, GWL_ID, IDC_MSGEDIT_FONT_PREVIEW);
    TrackControl(hPreview);
    yPos += lineH + 4;

    // Separator
    yPos += 4;

    // Size, X, Y on same row
    TrackControl(CreateLabel(m_hWnd, L"Size (0-100):", margin, yPos + 2, lblW, smallH, hFont));
    swprintf(buf, 64, L"%.0f", fSize);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_MSGEDIT_SIZE, xVal, yPos, editW, editH, hFont));
    int smallLblW = MulDiv(16, lineH, 20);
    TrackControl(CreateLabel(m_hWnd, L"X:", xVal + editW + 10, yPos + 2, smallLblW, smallH, hFont));
    swprintf(buf, 64, L"%.2f", x);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_MSGEDIT_XPOS, xVal + editW + 10 + smallLblW + 2, yPos, editW, editH, hFont));
    TrackControl(CreateLabel(m_hWnd, L"Y:", xVal + editW * 2 + 10 + smallLblW + 12, yPos + 2, smallLblW, smallH, hFont));
    swprintf(buf, 64, L"%.2f", y);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_MSGEDIT_YPOS, xVal + editW * 2 + 10 + smallLblW * 2 + 14, yPos, editW, editH, hFont));
    yPos += lineH + 4;

    // Growth
    TrackControl(CreateLabel(m_hWnd, L"Growth:", margin, yPos + 2, lblW, smallH, hFont));
    swprintf(buf, 64, L"%.2f", growth);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_MSGEDIT_GROWTH, xVal, yPos, editW, editH, hFont));
    yPos += lineH + 4;

    // Duration
    TrackControl(CreateLabel(m_hWnd, L"Duration (s):", margin, yPos + 2, lblW, smallH, hFont));
    swprintf(buf, 64, L"%.1f", fTime);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_MSGEDIT_TIME, xVal, yPos, editW, editH, hFont));
    yPos += lineH + 4;

    // Fade In, Fade Out on same row
    int fadeOutLblW = MulDiv(70, lineH, 20);
    TrackControl(CreateLabel(m_hWnd, L"Fade In (s):", margin, yPos + 2, lblW, smallH, hFont));
    swprintf(buf, 64, L"%.1f", fFade);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_MSGEDIT_FADEIN, xVal, yPos, editW, editH, hFont));
    TrackControl(CreateLabel(m_hWnd, L"Fade Out:", xVal + editW + 10, yPos + 2, fadeOutLblW, smallH, hFont));
    swprintf(buf, 64, L"%.1f", fFadeOut);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_MSGEDIT_FADEOUT, xVal + editW + 10 + fadeOutLblW + 2, yPos, editW, editH, hFont));
    yPos += lineH + 6;

    // Animation Profile combo
    TrackControl(CreateLabel(m_hWnd, L"Anim Profile:", margin, yPos + 2, lblW, smallH, hFont));
    {
      int apComboW = rw - lblW - 4;
      HWND hAPCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        xVal, yPos, apComboW, lineH * 10, m_hWnd,
        (HMENU)(INT_PTR)IDC_MSGEDIT_ANIM_PROFILE, hInst, NULL);
      if (hAPCombo) SendMessage(hAPCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
      TrackControl(hAPCombo);
      SendMessageW(hAPCombo, CB_ADDSTRING, 0, (LPARAM)L"(Use message settings)");
      SendMessageW(hAPCombo, CB_ADDSTRING, 0, (LPARAM)L"(Random profile)");
      for (int i = 0; i < m_pEngine->m_nAnimProfileCount; i++)
        SendMessageW(hAPCombo, CB_ADDSTRING, 0, (LPARAM)m_pEngine->m_AnimProfiles[i].szName);
      int apSel = 0;
      if (nAnimProfile == -2) apSel = 1;
      else if (nAnimProfile >= 0) apSel = nAnimProfile + 2;
      SendMessage(hAPCombo, CB_SETCURSEL, apSel, 0);
    }
    yPos += lineH + 6;

    // --- Randomize section (2-column checkboxes) ---
    int halfW = (rw - 10) / 2;
    int randBtnW = MulDiv(110, lineH, 20);
    TrackControl(CreateLabel(m_hWnd, L"Randomize:", margin, yPos + 2, MulDiv(80, lineH, 20), smallH, hFont));
    TrackControl(CreateBtn(m_hWnd, L"Randomize All", IDC_MSGEDIT_RAND_ALL, margin + rw - randBtnW, yPos, randBtnW, btnH, hFont));
    yPos += btnH + 4;

    TrackControl(CreateCheck(m_hWnd, L"Position", IDC_MSGEDIT_RAND_POS, margin, yPos, halfW, editH, hFont, bRandPos, true));
    TrackControl(CreateCheck(m_hWnd, L"Font", IDC_MSGEDIT_RAND_FONT, margin + halfW + 10, yPos, halfW, editH, hFont, bRandFont, true));
    yPos += lineH + 2;
    TrackControl(CreateCheck(m_hWnd, L"Size", IDC_MSGEDIT_RAND_SIZE, margin, yPos, halfW, editH, hFont, bRandSize, true));
    TrackControl(CreateCheck(m_hWnd, L"Color", IDC_MSGEDIT_RAND_COLOR, margin + halfW + 10, yPos, halfW, editH, hFont, bRandColor, true));
    yPos += lineH + 2;
    TrackControl(CreateCheck(m_hWnd, L"Effects (bold/ital)", IDC_MSGEDIT_RAND_EFFECTS, margin, yPos, halfW, editH, hFont, bRandEffects, true));
    TrackControl(CreateCheck(m_hWnd, L"Growth", IDC_MSGEDIT_RAND_GROWTH, margin + halfW + 10, yPos, halfW, editH, hFont, bRandGrowth, true));
    yPos += lineH + 2;
    TrackControl(CreateCheck(m_hWnd, L"Duration", IDC_MSGEDIT_RAND_DURATION, margin, yPos, halfW, editH, hFont, bRandDuration, true));
    yPos += lineH + 12;

    // Send Now / OK / Cancel buttons
    int okBtnW = MulDiv(80, lineH, 20);
    int sendBtnW = MulDiv(90, lineH, 20);
    TrackControl(CreateBtn(m_hWnd, L"Send Now", IDC_MSGEDIT_SEND_NOW, margin, yPos, sendBtnW, btnH, hFont));
    TrackControl(CreateBtn(m_hWnd, L"OK", IDC_MSGEDIT_OK, clientW / 2 - okBtnW + 20, yPos, okBtnW, btnH, hFont));
    TrackControl(CreateBtn(m_hWnd, L"Cancel", IDC_MSGEDIT_CANCEL, clientW / 2 + okBtnW + 20, yPos, okBtnW, btnH, hFont));

    // Update the font preview + color swatch
    UpdateFontPreview();
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    if (id == IDC_MSGEDIT_OK && code == BN_CLICKED) {
      wchar_t buf[256];
      GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_TEXT), szText, 256);
      if (szText[0] == 0) {
        MessageBoxW(m_hWnd, L"Message text cannot be empty.", L"Messages", MB_OK);
        return 0;
      }
      GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_SIZE), buf, 64);
      fSize = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_XPOS), buf, 64);
      x = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_YPOS), buf, 64);
      y = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_GROWTH), buf, 64);
      growth = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_TIME), buf, 64);
      fTime = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_FADEIN), buf, 64);
      fFade = (float)_wtof(buf);
      GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_FADEOUT), buf, 64);
      fFadeOut = (float)_wtof(buf);

      int sel = (int)SendMessage(GetDlgItem(m_hWnd, IDC_MSGEDIT_FONT_COMBO), CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel < MAX_CUSTOM_MESSAGE_FONTS) nFont = sel;

      // Clamp
      if (nFont < 0) nFont = 0;
      if (nFont >= MAX_CUSTOM_MESSAGE_FONTS) nFont = MAX_CUSTOM_MESSAGE_FONTS - 1;
      if (fSize < 0) fSize = 0;
      if (fSize > 100) fSize = 100;
      if (fTime < 0.1f) fTime = 0.1f;

      // Read randomize checkbox states
      bRandPos = IsChecked(IDC_MSGEDIT_RAND_POS);
      bRandSize = IsChecked(IDC_MSGEDIT_RAND_SIZE);
      bRandFont = IsChecked(IDC_MSGEDIT_RAND_FONT);
      bRandColor = IsChecked(IDC_MSGEDIT_RAND_COLOR);
      bRandEffects = IsChecked(IDC_MSGEDIT_RAND_EFFECTS);
      bRandGrowth = IsChecked(IDC_MSGEDIT_RAND_GROWTH);
      bRandDuration = IsChecked(IDC_MSGEDIT_RAND_DURATION);

      // Animation profile combo: 0=(own), 1=(random), 2+=profile index
      {
        int apSel = (int)SendMessage(GetDlgItem(m_hWnd, IDC_MSGEDIT_ANIM_PROFILE), CB_GETCURSEL, 0, 0);
        if (apSel <= 0) nAnimProfile = -1;
        else if (apSel == 1) nAnimProfile = -2;
        else nAnimProfile = apSel - 2;
      }

      EndDialog(true);
      return 0;
    }
    if (id == IDC_MSGEDIT_CANCEL && code == BN_CLICKED) {
      EndDialog(false);
      return 0;
    }
    if (id == IDC_MSGEDIT_FONT_COMBO && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel < MAX_CUSTOM_MESSAGE_FONTS)
        nFont = sel;
      UpdateFontPreview();
      return 0;
    }
    if (id == IDC_MSGEDIT_CHOOSE_FONT && code == BN_CLICKED) {
      int fontID = nFont;
      if (fontID < 0) fontID = 0;
      if (fontID >= MAX_CUSTOM_MESSAGE_FONTS) fontID = MAX_CUSTOM_MESSAGE_FONTS - 1;

      const wchar_t* curFace = bOverrideFace ? szFace : m_pEngine->m_CustomMessageFont[fontID].szFace;
      bool curBold = bOverrideBold ? (bBold != 0) : (m_pEngine->m_CustomMessageFont[fontID].bBold != 0);
      bool curItal = bOverrideItal ? (bItal != 0) : (m_pEngine->m_CustomMessageFont[fontID].bItal != 0);
      int curR = bOverrideColorR ? nColorR : m_pEngine->m_CustomMessageFont[fontID].nColorR;
      int curG = bOverrideColorG ? nColorG : m_pEngine->m_CustomMessageFont[fontID].nColorG;
      int curB = bOverrideColorB ? nColorB : m_pEngine->m_CustomMessageFont[fontID].nColorB;

      LOGFONTW lf = {};
      wcscpy_s(lf.lfFaceName, 32, curFace);
      lf.lfWeight = curBold ? FW_BOLD : FW_NORMAL;
      lf.lfItalic = curItal ? TRUE : FALSE;
      lf.lfHeight = -24;

      CHOOSEFONTW cf = { sizeof(cf) };
      cf.hwndOwner = m_hWnd;
      cf.lpLogFont = &lf;
      cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
      cf.rgbColors = RGB(curR < 0 ? 255 : curR, curG < 0 ? 255 : curG, curB < 0 ? 255 : curB);

      if (ChooseFontW(&cf)) {
        bOverrideFace = true;
        wcscpy_s(szFace, 128, lf.lfFaceName);
        bOverrideBold = true;
        bBold = (lf.lfWeight >= FW_BOLD) ? 1 : 0;
        bOverrideItal = true;
        bItal = lf.lfItalic ? 1 : 0;
        bOverrideColorR = true;
        bOverrideColorG = true;
        bOverrideColorB = true;
        nColorR = GetRValue(cf.rgbColors);
        nColorG = GetGValue(cf.rgbColors);
        nColorB = GetBValue(cf.rgbColors);
        UpdateFontPreview();
      }
      return 0;
    }
    if (id == IDC_MSGEDIT_CHOOSE_COLOR && code == BN_CLICKED) {
      int fontID = nFont;
      if (fontID < 0) fontID = 0;
      if (fontID >= MAX_CUSTOM_MESSAGE_FONTS) fontID = MAX_CUSTOM_MESSAGE_FONTS - 1;

      int curR = bOverrideColorR ? nColorR : m_pEngine->m_CustomMessageFont[fontID].nColorR;
      int curG = bOverrideColorG ? nColorG : m_pEngine->m_CustomMessageFont[fontID].nColorG;
      int curB = bOverrideColorB ? nColorB : m_pEngine->m_CustomMessageFont[fontID].nColorB;

      CHOOSECOLORW cc = { sizeof(cc) };
      cc.hwndOwner = m_hWnd;
      cc.rgbResult = RGB(curR < 0 ? 255 : curR, curG < 0 ? 255 : curG, curB < 0 ? 255 : curB);
      cc.lpCustColors = s_acrCustColors;
      cc.Flags = CC_FULLOPEN | CC_RGBINIT;

      if (ChooseColorW(&cc)) {
        bOverrideColorR = true;
        bOverrideColorG = true;
        bOverrideColorB = true;
        nColorR = GetRValue(cc.rgbResult);
        nColorG = GetGValue(cc.rgbResult);
        nColorB = GetBValue(cc.rgbResult);
        UpdateFontPreview();
      }
      return 0;
    }
    // Randomize All
    if (id == IDC_MSGEDIT_RAND_ALL && code == BN_CLICKED) {
      int ids[] = { IDC_MSGEDIT_RAND_POS, IDC_MSGEDIT_RAND_SIZE, IDC_MSGEDIT_RAND_FONT,
                    IDC_MSGEDIT_RAND_COLOR, IDC_MSGEDIT_RAND_EFFECTS, IDC_MSGEDIT_RAND_GROWTH,
                    IDC_MSGEDIT_RAND_DURATION };
      for (int cid : ids)
        SetChecked(cid, true);
      return 0;
    }
    // Send Now
    if (id == IDC_MSGEDIT_SEND_NOW && code == BN_CLICKED) {
      ReadControlValues();
      if (szText[0] == 0) return 0;

      // Temporarily write to the message slot and push it
      td_custom_msg* m = &m_pEngine->m_CustomMessage[msgIndex];
      wcscpy_s(m->szText, 256, szText);
      m->nFont = nFont;
      m->fSize = fSize;
      m->x = x;
      m->y = y;
      m->growth = growth;
      m->fTime = fTime;
      m->fFade = fFade;
      m->fFadeOut = fFadeOut;
      m->bOverrideFace = bOverrideFace ? 1 : 0;
      m->bOverrideBold = bOverrideBold ? 1 : 0;
      m->bOverrideItal = bOverrideItal ? 1 : 0;
      m->bOverrideColorR = bOverrideColorR ? 1 : 0;
      m->bOverrideColorG = bOverrideColorG ? 1 : 0;
      m->bOverrideColorB = bOverrideColorB ? 1 : 0;
      wcscpy_s(m->szFace, 128, szFace);
      m->bBold = bBold;
      m->bItal = bItal;
      m->nColorR = nColorR;
      m->nColorG = nColorG;
      m->nColorB = nColorB;
      m->bRandPos = bRandPos ? 1 : 0;
      m->bRandSize = bRandSize ? 1 : 0;
      m->bRandFont = bRandFont ? 1 : 0;
      m->bRandColor = bRandColor ? 1 : 0;
      m->bRandEffects = bRandEffects ? 1 : 0;
      m->bRandGrowth = bRandGrowth ? 1 : 0;
      m->bRandDuration = bRandDuration ? 1 : 0;

      HWND hw = m_pEngine->GetPluginWindow();
      if (hw) PostMessage(hw, WM_MW_PUSH_MESSAGE, msgIndex, 0);
      return 0;
    }
    return -1;
  }

private:
  HWND m_hFontCombo = NULL;

  void ReadControlValues() {
    wchar_t buf[256];
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_TEXT), szText, 256);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_SIZE), buf, 64);
    fSize = (float)_wtof(buf);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_XPOS), buf, 64);
    x = (float)_wtof(buf);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_YPOS), buf, 64);
    y = (float)_wtof(buf);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_GROWTH), buf, 64);
    growth = (float)_wtof(buf);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_TIME), buf, 64);
    fTime = (float)_wtof(buf);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_FADEIN), buf, 64);
    fFade = (float)_wtof(buf);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_FADEOUT), buf, 64);
    fFadeOut = (float)_wtof(buf);
    int sel = (int)SendMessage(GetDlgItem(m_hWnd, IDC_MSGEDIT_FONT_COMBO), CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < MAX_CUSTOM_MESSAGE_FONTS) nFont = sel;
    bRandPos = IsChecked(IDC_MSGEDIT_RAND_POS);
    bRandSize = IsChecked(IDC_MSGEDIT_RAND_SIZE);
    bRandFont = IsChecked(IDC_MSGEDIT_RAND_FONT);
    bRandColor = IsChecked(IDC_MSGEDIT_RAND_COLOR);
    bRandEffects = IsChecked(IDC_MSGEDIT_RAND_EFFECTS);
    bRandGrowth = IsChecked(IDC_MSGEDIT_RAND_GROWTH);
    bRandDuration = IsChecked(IDC_MSGEDIT_RAND_DURATION);
  }

  void UpdateFontPreview() {
    int fontID = nFont;
    if (fontID < 0) fontID = 0;
    if (fontID >= MAX_CUSTOM_MESSAGE_FONTS) fontID = MAX_CUSTOM_MESSAGE_FONTS - 1;

    const wchar_t* face = bOverrideFace ? szFace : m_pEngine->m_CustomMessageFont[fontID].szFace;
    bool bold = bOverrideBold ? (bBold != 0) : (m_pEngine->m_CustomMessageFont[fontID].bBold != 0);
    bool ital = bOverrideItal ? (bItal != 0) : (m_pEngine->m_CustomMessageFont[fontID].bItal != 0);
    int r = bOverrideColorR ? nColorR : m_pEngine->m_CustomMessageFont[fontID].nColorR;
    int g = bOverrideColorG ? nColorG : m_pEngine->m_CustomMessageFont[fontID].nColorG;
    int b = bOverrideColorB ? nColorB : m_pEngine->m_CustomMessageFont[fontID].nColorB;

    wchar_t preview[256];
    swprintf(preview, 256, L"%s%s%s   RGB(%d, %d, %d)",
      face, bold ? L", Bold" : L"", ital ? L", Italic" : L"", r, g, b);
    SetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGEDIT_FONT_PREVIEW), preview);

    // Update color swatch via SwatchColor property (handled by HandleDarkDrawItem)
    HWND hSwatch = GetDlgItem(m_hWnd, IDC_MSGEDIT_COLOR_SWATCH);
    if (hSwatch) {
      SetPropW(hSwatch, L"SwatchColor", (HANDLE)(intptr_t)RGB(r < 0 ? 255 : r, g < 0 ? 255 : g, b < 0 ? 255 : b));
      InvalidateRect(hSwatch, NULL, TRUE);
    }
  }
};
COLORREF MsgEditDialog::s_acrCustColors[16] = {};

bool Engine::ShowMessageEditDialog(HWND hParent, int msgIndex, bool isNew) {
  MsgEditDialog dlg(this);
  dlg.msgIndex = msgIndex;
  dlg.isNew = isNew;

  td_custom_msg* m = &m_CustomMessage[msgIndex];
  dlg.originalMsg = *m;

  if (!isNew) {
    wcscpy_s(dlg.szText, 256, m->szText);
    dlg.nFont = m->nFont;
    dlg.fSize = m->fSize;
    dlg.x = m->x;
    dlg.y = m->y;
    dlg.growth = m->growth;
    dlg.fTime = m->fTime;
    dlg.fFade = m->fFade;
    dlg.fFadeOut = m->fFadeOut;
    dlg.bOverrideFace = m->bOverrideFace != 0;
    dlg.bOverrideBold = m->bOverrideBold != 0;
    dlg.bOverrideItal = m->bOverrideItal != 0;
    dlg.bOverrideColorR = m->bOverrideColorR != 0;
    dlg.bOverrideColorG = m->bOverrideColorG != 0;
    dlg.bOverrideColorB = m->bOverrideColorB != 0;
    wcscpy_s(dlg.szFace, 128, m->szFace);
    dlg.bBold = m->bBold;
    dlg.bItal = m->bItal;
    dlg.nColorR = m->nColorR;
    dlg.nColorG = m->nColorG;
    dlg.nColorB = m->nColorB;
    dlg.bRandPos = m->bRandPos != 0;
    dlg.bRandSize = m->bRandSize != 0;
    dlg.bRandFont = m->bRandFont != 0;
    dlg.bRandColor = m->bRandColor != 0;
    dlg.bRandEffects = m->bRandEffects != 0;
    dlg.bRandGrowth = m->bRandGrowth != 0;
    dlg.bRandDuration = m->bRandDuration != 0;
    dlg.nAnimProfile = m->nAnimProfile;
  }

  // Compute line height from font size for proportional layout
  HFONT hTmp = CreateFontW(m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HDC hdcTmp = GetDC(hParent);
  HFONT hOld = (HFONT)SelectObject(hdcTmp, hTmp);
  TEXTMETRIC tm = {};
  GetTextMetrics(hdcTmp, &tm);
  SelectObject(hdcTmp, hOld);
  ReleaseDC(hParent, hdcTmp);
  DeleteObject(hTmp);
  int lineH = tm.tmHeight + tm.tmExternalLeading + 6;
  if (lineH < 20) lineH = 20;

  int clientW = MulDiv(440, lineH, 20);
  int clientH = MulDiv(560, lineH, 20);

  bool accepted = dlg.Show(hParent, clientW, clientH);

  if (accepted) {
    wcscpy_s(m->szText, 256, dlg.szText);
    m->nFont = dlg.nFont;
    m->fSize = dlg.fSize;
    m->x = dlg.x;
    m->y = dlg.y;
    m->growth = dlg.growth;
    m->fTime = dlg.fTime;
    m->fFade = dlg.fFade;
    m->fFadeOut = dlg.fFadeOut;
    m->bOverrideFace = dlg.bOverrideFace ? 1 : 0;
    m->bOverrideBold = dlg.bOverrideBold ? 1 : 0;
    m->bOverrideItal = dlg.bOverrideItal ? 1 : 0;
    m->bOverrideColorR = dlg.bOverrideColorR ? 1 : 0;
    m->bOverrideColorG = dlg.bOverrideColorG ? 1 : 0;
    m->bOverrideColorB = dlg.bOverrideColorB ? 1 : 0;
    wcscpy_s(m->szFace, 128, dlg.szFace);
    m->bBold = dlg.bBold;
    m->bItal = dlg.bItal;
    m->nColorR = dlg.nColorR;
    m->nColorG = dlg.nColorG;
    m->nColorB = dlg.nColorB;
    m->bRandPos = dlg.bRandPos ? 1 : 0;
    m->bRandSize = dlg.bRandSize ? 1 : 0;
    m->bRandFont = dlg.bRandFont ? 1 : 0;
    m->bRandColor = dlg.bRandColor ? 1 : 0;
    m->bRandEffects = dlg.bRandEffects ? 1 : 0;
    m->bRandGrowth = dlg.bRandGrowth ? 1 : 0;
    m->bRandDuration = dlg.bRandDuration ? 1 : 0;
    m->nAnimProfile = dlg.nAnimProfile;
  } else {
    // Restore original message (Send Now may have modified it)
    *m = dlg.originalMsg;
  }

  return accepted;
}

// ======== Message Overrides Dialog (ModalDialog subclass) ========

enum { ANIMCOL_NAME = 0, ANIMCOL_DURATION, ANIMCOL_EFFECTS };

class MsgOverridesDialog : public mdrop::ModalDialog {
public:
  MsgOverridesDialog(Engine* pEngine) : ModalDialog(pEngine) {}

  // Working copies (populated before Show, read back after)
  bool bRandomFont = false, bRandomColor = false, bRandomSize = false, bRandomEffects = false;
  float fSizeMin = 1.0f, fSizeMax = 100.0f;
  int  nMaxOnScreen = 3;
  bool bRandomPos = false, bRandomGrowth = false, bSlideIn = false, bRandomDuration = false;
  bool bShadow = false, bBox = false;
  bool bApplyHueShift = false, bRandomHue = false;
  bool bIgnorePerMsg = false;
  bool animEnabled[MAX_ANIM_PROFILES] = {};
  int  nAnimCount = 0;

protected:
  const wchar_t* GetDialogTitle() const override { return L"Message Overrides"; }
  const wchar_t* GetDialogClass() const override { return L"MDropDX12MsgOverrides"; }

  void DoBuildControls(int clientW, int clientH) override {
    HFONT hFont = GetFont();
    HINSTANCE hInst = GetModuleHandle(NULL);
    int lineH = GetLineHeight();
    int margin = MulDiv(16, lineH, 20), rw = clientW - margin * 2;
    int editH = lineH, smallH = lineH - 4, btnH = lineH + 4;
    wchar_t buf[32];

    // ── Tab Control ──
    int tabY = MulDiv(6, lineH, 20);
    int tabH = clientH - tabY - btnH - MulDiv(20, lineH, 20);
    m_hTab = CreateWindowExW(0, WC_TABCONTROLW, NULL,
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS | TCS_OWNERDRAWFIXED,
      margin - 4, tabY, rw + 8, tabH, m_hWnd,
      (HMENU)(INT_PTR)IDC_MSGOVERRIDE_TAB, hInst, NULL);
    if (m_hTab) {
      SendMessage(m_hTab, WM_SETFONT, (WPARAM)hFont, TRUE);
      SetWindowSubclass(m_hTab, mdrop::DarkTabSubclassProc, 1, (DWORD_PTR)m_pEngine);
    }
    TrackControl(m_hTab);

    TCITEMW tci = {};
    tci.mask = TCIF_TEXT;
    tci.pszText = (LPWSTR)L"Randomize";
    SendMessageW(m_hTab, TCM_INSERTITEMW, 0, (LPARAM)&tci);
    tci.pszText = (LPWSTR)L"Animations";
    SendMessageW(m_hTab, TCM_INSERTITEMW, 1, (LPARAM)&tci);

    // Get tab content area
    RECT rcTab;
    GetClientRect(m_hTab, &rcTab);
    SendMessage(m_hTab, TCM_ADJUSTRECT, FALSE, (LPARAM)&rcTab);
    POINT ptTab = { rcTab.left, rcTab.top };
    MapWindowPoints(m_hTab, m_hWnd, &ptTab, 1);
    int cx = ptTab.x + 4, cy = ptTab.y + 4;
    int cw = rcTab.right - rcTab.left - 8;

    // ═══ Randomize Tab Controls ═══
    int y = cy;
    auto trackRand = [&](HWND h) { if (h) { m_randomizeControls.push_back(h); TrackControl(h); } };

    trackRand(CreateCheck(m_hWnd, L"Randomize font face", IDC_MSGOVERRIDE_RAND_FONT, cx, y, cw, editH, hFont, bRandomFont, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Randomize color", IDC_MSGOVERRIDE_RAND_COLOR, cx, y, cw, editH, hFont, bRandomColor, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Randomize effects (bold/italic)", IDC_MSGOVERRIDE_RAND_EFFECTS, cx, y, cw, editH, hFont, bRandomEffects, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Randomize size", IDC_MSGOVERRIDE_RAND_SIZE, cx, y, cw, editH, hFont, bRandomSize, true));
    y += lineH + 4;

    // Size min/max
    int lblW2 = MulDiv(70, lineH, 20), editW2 = MulDiv(50, lineH, 20);
    int indent = MulDiv(20, lineH, 20);
    trackRand(CreateLabel(m_hWnd, L"Min size:", cx + indent, y + 2, lblW2, smallH, hFont));
    swprintf(buf, 32, L"%.2g", fSizeMin);
    trackRand(CreateEdit(m_hWnd, buf, IDC_MSGOVERRIDE_SIZE_MIN, cx + indent + lblW2, y, editW2, editH, hFont, 0));
    trackRand(CreateLabel(m_hWnd, L"Max size:", cx + indent + lblW2 + editW2 + 16, y + 2, lblW2, smallH, hFont));
    swprintf(buf, 32, L"%.2g", fSizeMax);
    trackRand(CreateEdit(m_hWnd, buf, IDC_MSGOVERRIDE_SIZE_MAX, cx + indent + lblW2 * 2 + editW2 + 16, y, editW2, editH, hFont, 0));
    y += lineH + 4;

    trackRand(CreateLabel(m_hWnd, L"(min \x2265 0.01, max \x2264 100, 50 = normal)", cx + indent, y, cw, smallH, hFont));
    y += lineH + 2;

    // Max on screen
    int maxLblW = MulDiv(170, lineH, 20);
    trackRand(CreateLabel(m_hWnd, L"Max messages on screen:", cx, y + 2, maxLblW, smallH, hFont));
    swprintf(buf, 32, L"%d", nMaxOnScreen);
    trackRand(CreateEdit(m_hWnd, buf, IDC_MSGOVERRIDE_MAX_ONSCREEN, cx + maxLblW + 4, y, MulDiv(40, lineH, 20), editH, hFont, 0));
    trackRand(CreateLabel(m_hWnd, L"(1-10)", cx + maxLblW + MulDiv(50, lineH, 20), y + 2, MulDiv(50, lineH, 20), smallH, hFont));
    y += lineH + 8;

    trackRand(CreateLabel(m_hWnd, L"Animations:", cx, y, cw, smallH, hFont));
    y += lineH;
    trackRand(CreateCheck(m_hWnd, L"Random position", IDC_MSGOVERRIDE_RAND_POS, cx, y, cw, editH, hFont, bRandomPos, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Random growth (text scales over time)", IDC_MSGOVERRIDE_RAND_GROWTH, cx, y, cw, editH, hFont, bRandomGrowth, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Slide in from edge", IDC_MSGOVERRIDE_SLIDE_IN, cx, y, cw, editH, hFont, bSlideIn, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Random duration (2\x2013" L"10 seconds)", IDC_MSGOVERRIDE_RAND_DURATION, cx, y, cw, editH, hFont, bRandomDuration, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Drop shadow", IDC_MSGOVERRIDE_SHADOW, cx, y, cw, editH, hFont, bShadow, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Background box", IDC_MSGOVERRIDE_BOX, cx, y, cw, editH, hFont, bBox, true));
    y += lineH + 8;

    trackRand(CreateLabel(m_hWnd, L"Color Shifting:", cx, y, cw, smallH, hFont));
    y += lineH;
    trackRand(CreateCheck(m_hWnd, L"Apply current hue shift", IDC_MSGOVERRIDE_APPLY_HUE, cx, y, cw, editH, hFont, bApplyHueShift, true));
    y += lineH + 2;
    trackRand(CreateCheck(m_hWnd, L"Random hue per message", IDC_MSGOVERRIDE_RAND_HUE, cx, y, cw, editH, hFont, bRandomHue, true));
    y += lineH + 8;

    trackRand(CreateLabel(m_hWnd, L"Per-Message:", cx, y, cw, smallH, hFont));
    y += lineH;
    trackRand(CreateCheck(m_hWnd, L"Ignore per-message randomization", IDC_MSGOVERRIDE_IGNORE_PERMSG, cx, y, cw, editH, hFont, bIgnorePerMsg, true));

    // ═══ Animations Tab Controls ═══
    {
      int listY = cy;
      int listH = tabH - (cy - tabY) - btnH - MulDiv(16, lineH, 20);

      m_hAnimList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        cx, listY, cw, listH, m_hWnd,
        (HMENU)(INT_PTR)IDC_MSGOVERRIDE_ANIM_LIST, hInst, NULL);
      if (m_hAnimList) {
        SendMessage(m_hAnimList, WM_SETFONT, (WPARAM)hFont, TRUE);
        ListView_SetExtendedListViewStyle(m_hAnimList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        TrackControl(m_hAnimList);

        int scrollW = GetSystemMetrics(SM_CXVSCROLL) + 4;
        int colName = MulDiv(cw, 40, 100);
        int colDur = MulDiv(cw, 18, 100);
        int colFx = cw - colName - colDur - scrollW;

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)L"Name";
        col.cx = colName;
        SendMessageW(m_hAnimList, LVM_INSERTCOLUMNW, ANIMCOL_NAME, (LPARAM)&col);
        col.pszText = (LPWSTR)L"Duration";
        col.cx = colDur;
        SendMessageW(m_hAnimList, LVM_INSERTCOLUMNW, ANIMCOL_DURATION, (LPARAM)&col);
        col.pszText = (LPWSTR)L"Effects";
        col.cx = colFx;
        SendMessageW(m_hAnimList, LVM_INSERTCOLUMNW, ANIMCOL_EFFECTS, (LPARAM)&col);

        RefreshAnimList();
      }

      int abtnY = listY + listH + 4;
      int abtnW = MulDiv(80, lineH, 20);
      m_hAnimAll = CreateBtn(m_hWnd, L"Select All", IDC_MSGOVERRIDE_ANIM_ALL, cx, abtnY, abtnW, btnH, hFont);
      m_hAnimNone = CreateBtn(m_hWnd, L"Select None", IDC_MSGOVERRIDE_ANIM_NONE, cx + abtnW + 8, abtnY, abtnW, btnH, hFont);
      TrackControl(m_hAnimAll);
      TrackControl(m_hAnimNone);
    }

    // Show Randomize tab by default, hide Animations tab
    ShowTab(0);

    // ── OK / Cancel ──
    int okY = tabY + tabH + MulDiv(8, lineH, 20);
    int okBtnW = MulDiv(80, lineH, 20);
    TrackControl(CreateBtn(m_hWnd, L"OK", IDC_MSGOVERRIDE_OK, clientW / 2 - okBtnW - 10, okY, okBtnW, btnH, hFont));
    TrackControl(CreateBtn(m_hWnd, L"Cancel", IDC_MSGOVERRIDE_CANCEL, clientW / 2 + 10, okY, okBtnW, btnH, hFont));
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    if (id == IDC_MSGOVERRIDE_ANIM_ALL && code == BN_CLICKED) {
      for (int i = 0; i < nAnimCount; i++) animEnabled[i] = true;
      RefreshAnimList();
      return 0;
    }
    if (id == IDC_MSGOVERRIDE_ANIM_NONE && code == BN_CLICKED) {
      for (int i = 0; i < nAnimCount; i++) animEnabled[i] = false;
      RefreshAnimList();
      return 0;
    }
    if (id == IDC_MSGOVERRIDE_OK && code == BN_CLICKED) {
      ReadBackCheckboxes();
      EndDialog(true);
      return 0;
    }
    if (id == IDC_MSGOVERRIDE_CANCEL && code == BN_CLICKED) {
      EndDialog(false);
      return 0;
    }
    return -1;
  }

  LRESULT DoNotify(NMHDR* pnm) override {
    // Tab selection changed
    if (pnm->idFrom == IDC_MSGOVERRIDE_TAB && pnm->code == TCN_SELCHANGE) {
      ShowTab((int)SendMessage(m_hTab, TCM_GETCURSEL, 0, 0));
      return 0;
    }
    // ListView column header click — sort
    if (pnm->idFrom == IDC_MSGOVERRIDE_ANIM_LIST && pnm->code == LVN_COLUMNCLICK) {
      NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
      if (pnmlv->iSubItem == m_nSortColumn)
        m_bSortAscending = !m_bSortAscending;
      else {
        m_nSortColumn = pnmlv->iSubItem;
        m_bSortAscending = true;
      }
      SendMessage(m_hAnimList, LVM_SORTITEMS, (WPARAM)this, (LPARAM)AnimListCompare);
      return 0;
    }
    // ListView checkbox toggle
    if (pnm->idFrom == IDC_MSGOVERRIDE_ANIM_LIST && pnm->code == LVN_ITEMCHANGED) {
      NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
      if ((pnmlv->uChanged & LVIF_STATE) && ((pnmlv->uNewState ^ pnmlv->uOldState) & LVIS_STATEIMAGEMASK)) {
        LVITEMW lvi = {};
        lvi.mask = LVIF_PARAM;
        lvi.iItem = pnmlv->iItem;
        SendMessage(m_hAnimList, LVM_GETITEMW, 0, (LPARAM)&lvi);
        int profIdx = (int)lvi.lParam;
        if (profIdx >= 0 && profIdx < nAnimCount) {
          bool checked = ListView_GetCheckState(m_hAnimList, pnmlv->iItem) != 0;
          animEnabled[profIdx] = checked;
        }
      }
      return 0;
    }
    return -1;
  }

  LRESULT DoMessage(UINT msg, WPARAM wParam, LPARAM lParam) override {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
      SendMessage(m_hWnd, WM_COMMAND, MAKEWPARAM(IDC_MSGOVERRIDE_OK, BN_CLICKED), 0);
      return 0;
    }
    return -1;
  }

private:
  HWND m_hTab = NULL;
  HWND m_hAnimList = NULL, m_hAnimAll = NULL, m_hAnimNone = NULL;
  std::vector<HWND> m_randomizeControls;
  int  m_nSortColumn = ANIMCOL_NAME;
  bool m_bSortAscending = true;

  void ShowTab(int tab) {
    int showRand = (tab == 0) ? SW_SHOW : SW_HIDE;
    int showAnim = (tab == 1) ? SW_SHOW : SW_HIDE;
    for (HWND h : m_randomizeControls) ShowWindow(h, showRand);
    if (m_hAnimList) ShowWindow(m_hAnimList, showAnim);
    if (m_hAnimAll) ShowWindow(m_hAnimAll, showAnim);
    if (m_hAnimNone) ShowWindow(m_hAnimNone, showAnim);
  }

  void RefreshAnimList() {
    if (!m_hAnimList) return;
    SendMessage(m_hAnimList, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < nAnimCount; i++) {
      const auto& prof = m_pEngine->m_AnimProfiles[i];
      LVITEMW lvi = {};
      lvi.mask = LVIF_TEXT | LVIF_PARAM;
      lvi.iItem = i;
      lvi.lParam = i;
      lvi.pszText = (LPWSTR)prof.szName;
      int idx = (int)SendMessageW(m_hAnimList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
      ListView_SetCheckState(m_hAnimList, idx, animEnabled[i]);

      wchar_t buf[32];
      swprintf(buf, 32, L"%.1fs", prof.fDuration);
      lvi.mask = LVIF_TEXT;
      lvi.iItem = idx;
      lvi.iSubItem = 1;
      lvi.pszText = buf;
      SendMessageW(m_hAnimList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

      std::wstring fx;
      if (prof.fMoveTime > 0) fx += L"Slide ";
      if (prof.fGrowth != 1.0f) fx += L"Growth ";
      if (prof.fShadowOffset > 0) fx += L"Shadow ";
      if (prof.fBoxAlpha > 0) fx += L"Box ";
      if (prof.bRandPos) fx += L"RPos ";
      if (prof.bRandColor) fx += L"RCol ";
      if (fx.empty()) fx = L"Default";
      lvi.iSubItem = 2;
      lvi.pszText = (LPWSTR)fx.c_str();
      SendMessageW(m_hAnimList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
    }
  }

  void ReadBackCheckboxes() {
    bRandomFont = IsChecked(IDC_MSGOVERRIDE_RAND_FONT);
    bRandomColor = IsChecked(IDC_MSGOVERRIDE_RAND_COLOR);
    bRandomSize = IsChecked(IDC_MSGOVERRIDE_RAND_SIZE);
    bRandomEffects = IsChecked(IDC_MSGOVERRIDE_RAND_EFFECTS);
    bRandomPos = IsChecked(IDC_MSGOVERRIDE_RAND_POS);
    bRandomGrowth = IsChecked(IDC_MSGOVERRIDE_RAND_GROWTH);
    bSlideIn = IsChecked(IDC_MSGOVERRIDE_SLIDE_IN);
    bRandomDuration = IsChecked(IDC_MSGOVERRIDE_RAND_DURATION);
    bShadow = IsChecked(IDC_MSGOVERRIDE_SHADOW);
    bBox = IsChecked(IDC_MSGOVERRIDE_BOX);
    bApplyHueShift = IsChecked(IDC_MSGOVERRIDE_APPLY_HUE);
    bRandomHue = IsChecked(IDC_MSGOVERRIDE_RAND_HUE);
    bIgnorePerMsg = IsChecked(IDC_MSGOVERRIDE_IGNORE_PERMSG);

    wchar_t buf[32];
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGOVERRIDE_SIZE_MIN), buf, 32);
    fSizeMin = (float)_wtof(buf);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGOVERRIDE_SIZE_MAX), buf, 32);
    fSizeMax = (float)_wtof(buf);
    GetWindowTextW(GetDlgItem(m_hWnd, IDC_MSGOVERRIDE_MAX_ONSCREEN), buf, 32);
    nMaxOnScreen = _wtoi(buf);

    if (fSizeMin < 0.01f) fSizeMin = 0.01f;
    if (fSizeMax > 100.0f) fSizeMax = 100.0f;
    if (fSizeMin >= fSizeMax) fSizeMin = fSizeMax * 0.5f;
    if (nMaxOnScreen < 1) nMaxOnScreen = 1;
    if (nMaxOnScreen > NUM_SUPERTEXTS) nMaxOnScreen = NUM_SUPERTEXTS;
  }

  static int CALLBACK AnimListCompare(LPARAM lp1, LPARAM lp2, LPARAM lParamSort) {
    MsgOverridesDialog* dlg = (MsgOverridesDialog*)lParamSort;
    int i1 = (int)lp1, i2 = (int)lp2;
    if (i1 < 0 || i1 >= dlg->nAnimCount || i2 < 0 || i2 >= dlg->nAnimCount) return 0;
    const auto& a = dlg->m_pEngine->m_AnimProfiles[i1];
    const auto& b = dlg->m_pEngine->m_AnimProfiles[i2];
    int cmp = 0;
    switch (dlg->m_nSortColumn) {
    case ANIMCOL_NAME: cmp = _wcsicmp(a.szName, b.szName); break;
    case ANIMCOL_DURATION:
      if (a.fDuration < b.fDuration) cmp = -1;
      else if (a.fDuration > b.fDuration) cmp = 1;
      break;
    case ANIMCOL_EFFECTS: break;
    }
    return dlg->m_bSortAscending ? cmp : -cmp;
  }
};

bool Engine::ShowMsgOverridesDialog(HWND hParent) {
  MsgOverridesDialog dlg(this);

  // Populate working copies
  dlg.bRandomFont = m_bMsgOverrideRandomFont;
  dlg.bRandomColor = m_bMsgOverrideRandomColor;
  dlg.bRandomSize = m_bMsgOverrideRandomSize;
  dlg.bRandomEffects = m_bMsgOverrideRandomEffects;
  dlg.fSizeMin = m_fMsgOverrideSizeMin;
  dlg.fSizeMax = m_fMsgOverrideSizeMax;
  dlg.nMaxOnScreen = m_nMsgMaxOnScreen;
  dlg.bRandomPos = m_bMsgOverrideRandomPos;
  dlg.bRandomGrowth = m_bMsgOverrideRandomGrowth;
  dlg.bSlideIn = m_bMsgOverrideSlideIn;
  dlg.bRandomDuration = m_bMsgOverrideRandomDuration;
  dlg.bShadow = m_bMsgOverrideShadow;
  dlg.bBox = m_bMsgOverrideBox;
  dlg.bApplyHueShift = m_bMsgOverrideApplyHueShift;
  dlg.bRandomHue = m_bMsgOverrideRandomHue;
  dlg.bIgnorePerMsg = m_bMsgIgnorePerMsgRandom;
  dlg.nAnimCount = m_nAnimProfileCount;
  for (int i = 0; i < dlg.nAnimCount; i++)
    dlg.animEnabled[i] = m_AnimProfiles[i].bEnabled;

  // Compute line height from font size for proportional layout
  // (font not yet created — use a temporary font to measure)
  HFONT hTmp = CreateFontW(m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HDC hdcTmp = GetDC(hParent);
  HFONT hOld = (HFONT)SelectObject(hdcTmp, hTmp);
  TEXTMETRIC tm = {};
  GetTextMetrics(hdcTmp, &tm);
  SelectObject(hdcTmp, hOld);
  ReleaseDC(hParent, hdcTmp);
  DeleteObject(hTmp);
  int lineH = tm.tmHeight + tm.tmExternalLeading + 6;
  if (lineH < 20) lineH = 20;

  int clientW = MulDiv(420, lineH, 20);
  int clientH = MulDiv(590, lineH, 20);

  if (!dlg.Show(hParent, clientW, clientH))
    return false;

  // Apply results
  m_bMsgOverrideRandomFont = dlg.bRandomFont;
  m_bMsgOverrideRandomColor = dlg.bRandomColor;
  m_bMsgOverrideRandomSize = dlg.bRandomSize;
  m_bMsgOverrideRandomEffects = dlg.bRandomEffects;
  m_fMsgOverrideSizeMin = dlg.fSizeMin;
  m_fMsgOverrideSizeMax = dlg.fSizeMax;
  m_nMsgMaxOnScreen = dlg.nMaxOnScreen;
  m_bMsgOverrideRandomPos = dlg.bRandomPos;
  m_bMsgOverrideRandomGrowth = dlg.bRandomGrowth;
  m_bMsgOverrideSlideIn = dlg.bSlideIn;
  m_bMsgOverrideRandomDuration = dlg.bRandomDuration;
  m_bMsgOverrideShadow = dlg.bShadow;
  m_bMsgOverrideBox = dlg.bBox;
  m_bMsgOverrideApplyHueShift = dlg.bApplyHueShift;
  m_bMsgOverrideRandomHue = dlg.bRandomHue;
  m_bMsgIgnorePerMsgRandom = dlg.bIgnorePerMsg;

  for (int i = 0; i < dlg.nAnimCount; i++)
    m_AnimProfiles[i].bEnabled = dlg.animEnabled[i];
  WriteAnimProfiles();
  SaveMsgAutoplaySettings();

  return true;
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
    m_CustomMessage[n].nAnimProfile = -1;
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

      // Animation profile reference
      m_CustomMessage[n].nAnimProfile = GetPrivateProfileIntW(szSectionName, L"animprofile", -1, m_szMsgIniFile);
    }
  }

  // Read animation profiles
  ReadAnimProfiles();
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

    // Check for animation profile
    int profIdx = m_CustomMessage[nMsgNum].nAnimProfile;
    if (profIdx == -2) profIdx = PickRandomAnimProfile();
    if (profIdx >= 0 && profIdx < m_nAnimProfileCount) {
      ApplyAnimProfileToSupertext(m_supertexts[nextFreeSupertextIndex], m_AnimProfiles[profIdx]);
      m_supertexts[nextFreeSupertextIndex].fStartTime = GetTime();
      return;
    }

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

  // Check for animation profile
  int profIdx = m_nSongTitleAnimProfile;
  if (profIdx == -2) profIdx = PickRandomAnimProfile();
  if (profIdx >= 0 && profIdx < m_nAnimProfileCount) {
    m_supertexts[supertextIndex].bIsSongTitle = false;  // render as custom message style
    ApplyAnimProfileToSupertext(m_supertexts[supertextIndex], m_AnimProfiles[profIdx]);
    m_supertexts[supertextIndex].fStartTime = GetTime();
    return;
  }

  // Default hardcoded song title animation
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
  // Route SIGNAL| commands through pipe server dispatch (PostMessage to render window)
  if (wcsncmp(sMessage, L"SIGNAL|", 7) == 0) {
    extern PipeServer g_pipeServer;
    g_pipeServer.DispatchSignal(sMessage + 7);
    return;
  }

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

    // Apply animation profile as base (explicit params override below)
    bool hasProfile = false;
    if (params.find(L"profile") != params.end()) {
      int profIdx = std::stoi(params[L"profile"]);
      if (profIdx == -2) profIdx = PickRandomAnimProfile();
      if (profIdx >= 0 && profIdx < m_nAnimProfileCount) {
        ApplyAnimProfileToSupertext(m_supertexts[nextFreeSupertextIndex], m_AnimProfiles[profIdx]);
        hasProfile = true;
      }
    }

    if (params.find(L"font") != params.end()) {
      lstrcpyW(m_supertexts[nextFreeSupertextIndex].nFontFace, ConvertToLPCWSTR(params[L"font"]));
    }
    else if (!hasProfile) {
      lstrcpyW(m_supertexts[nextFreeSupertextIndex].nFontFace, L"Segoe UI");
    }

    if (params.find(L"size") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fFontSize = std::stof(params[L"size"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].fFontSize = 30.0f;
    }

    if (params.find(L"x") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fX = std::stof(params[L"x"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].fX = 0.49f;
    }

    if (params.find(L"y") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fY = std::stof(params[L"y"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].fY = 0.5f;
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
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].fGrowth = 1.0f;
    }

    if (params.find(L"time") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fDuration = std::stof(params[L"time"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].fDuration = 5.0f;
    }

    if (params.find(L"fade") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fFadeInTime = std::stof(params[L"fade"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].fFadeInTime = m_MessageDefaultFadeinTime;
    }

    if (params.find(L"fadeout") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fFadeOutTime = std::stof(params[L"fadeout"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].fFadeOutTime = m_MessageDefaultFadeoutTime;
    }

    if (params.find(L"bold") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].bBold = std::stoi(params[L"bold"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].bBold = 0;
    }

    if (params.find(L"ital") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].bItal = std::stoi(params[L"ital"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].bItal = 0;
    }

    if (params.find(L"r") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorR = std::stoi(params[L"r"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].nColorR = 255;
    }

    if (params.find(L"g") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorG = std::stoi(params[L"g"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].nColorG = 255;
    }

    if (params.find(L"b") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].nColorB = std::stoi(params[L"b"]);
    }
    else if (!hasProfile) {
      m_supertexts[nextFreeSupertextIndex].nColorB = 255;
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
    else if (!hasProfile) {
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
    SendTrackInfoToMDropDX12Remote();
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
    // to update fonts — use ResetBufferAndFonts for proper SRV cleanup
    ResetBufferAndFonts();
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
  else if (wcsncmp(sMessage, L"SPOUTINPUT=", 11) == 0) {
    // Format: SPOUTINPUT=enabled|senderName  (e.g. "SPOUTINPUT=1|OBS Spout Filter")
    std::wstring msg(sMessage + 11);
    size_t sep = msg.find(L'|');
    bool bEnable = (!msg.empty() && msg[0] == L'1');
    if (sep != std::wstring::npos && sep + 1 < msg.size())
      wcsncpy_s(m_szSpoutInputSender, msg.substr(sep + 1).c_str(), _TRUNCATE);
    if (bEnable) {
      int oldSrc = m_nVideoInputSource;
      if (oldSrc == VID_SOURCE_WEBCAM || oldSrc == VID_SOURCE_FILE)
        DestroyVideoCapture();
      m_nVideoInputSource = VID_SOURCE_SPOUT;
      m_bSpoutInputEnabled = true;
      InitSpoutInput();
    } else {
      if (m_nVideoInputSource == VID_SOURCE_SPOUT)
        DestroySpoutInput();
      m_nVideoInputSource = VID_SOURCE_NONE;
      m_bSpoutInputEnabled = false;
    }
    SaveSpoutInputSettings();
  }
  else if (wcsncmp(sMessage, L"CAPTURE", 7) == 0) {
    DebugLogW(L"[CAPTURE] Message received");
    mdropdx12->LogInfo(L"CAPTURE message received, calling CaptureScreenshot()");
    CaptureScreenshot();
    DebugLogW(L"[CAPTURE] CaptureScreenshot() returned");
  }
  else if (wcsncmp(sMessage, L"FFT_ATTACK=", 11) == 0) {
    m_fFFTAttackGlobal = (float)_wtof(sMessage + 11);
    m_fFFTAttackGlobal = max(0.0f, min(1.0f, m_fFFTAttackGlobal));
    m_bFFTSmoothingActive = true;
  }
  else if (wcsncmp(sMessage, L"FFT_DECAY=", 10) == 0) {
    m_fFFTDecayGlobal = (float)_wtof(sMessage + 10);
    m_fFFTDecayGlobal = max(0.0f, min(1.0f, m_fFFTDecayGlobal));
    m_bFFTSmoothingActive = true;
  }
  else if (wcsncmp(sMessage, L"LOAD_LIST=", 10) == 0) {
    // Offloaded from render thread — file I/O + CancelThread can block up to 500ms
    std::wstring listName(sMessage + 10);
    std::thread([this, listName]() {
      extern PipeServer g_pipeServer;
      if (listName.find_first_of(L"\\/:") != std::wstring::npos ||
          listName.find(L"..") != std::wstring::npos) {
        g_pipeServer.Send(L"LOAD_LIST_RESULT=ERROR|invalid list name");
        return;
      }
      wchar_t szDir[MAX_PATH];
      GetPresetListDir(szDir, MAX_PATH);
      wchar_t szPath[MAX_PATH];
      swprintf(szPath, MAX_PATH, L"%s%s.txt", szDir, listName.c_str());
      if (LoadPresetList(szPath)) {
        WritePrivateProfileStringW(L"Settings", L"szActivePresetList", m_szActivePresetList.c_str(), GetConfigIniFile());
        g_pipeServer.Send(L"LOAD_LIST_RESULT=OK|" + listName);
      } else {
        g_pipeServer.Send(L"LOAD_LIST_RESULT=ERROR|list not found: " + listName);
      }
    }).detach();
  }
  else if (wcsncmp(sMessage, L"CLEAR_LIST", 10) == 0) {
    // Offloaded from render thread — INI write + UpdatePresetList
    std::thread([this]() {
      m_szActivePresetList.clear();
      WritePrivateProfileStringW(L"Settings", L"szActivePresetList", L"", GetConfigIniFile());
      UpdatePresetList(true, true);
      extern PipeServer g_pipeServer;
      g_pipeServer.Send(L"CLEAR_LIST_RESULT=OK");
    }).detach();
  }
  else if (wcsncmp(sMessage, L"ENUM_LISTS", 10) == 0) {
    // Offloaded from render thread — directory scan
    std::thread([this]() {
      std::vector<std::wstring> names;
      EnumPresetLists(names);
      std::wstring result = L"ENUM_LISTS_RESULT=";
      for (size_t i = 0; i < names.size(); i++) {
        if (i > 0) result += L"|";
        result += names[i];
      }
      extern PipeServer g_pipeServer;
      g_pipeServer.Send(result);
    }).detach();
  }
  else if (wcsncmp(sMessage, L"SET_DIR=", 8) == 0) {
    // Offloaded from render thread — dir validation + ChangePresetDir (INI write + UpdatePresetList)
    std::wstring value(sMessage + 8);
    bool bRecursive = false;
    size_t pipePos = value.find(L'|');
    if (pipePos != std::wstring::npos) {
      std::wstring opts = value.substr(pipePos + 1);
      if (_wcsicmp(opts.c_str(), L"recursive") == 0)
        bRecursive = true;
      value = value.substr(0, pipePos);
    }
    if (!value.empty() && value.back() != L'\\')
      value += L'\\';
    std::thread([this, value, bRecursive]() {
      wchar_t szNewDir[MAX_PATH];
      lstrcpynW(szNewDir, value.c_str(), MAX_PATH);
      extern PipeServer g_pipeServer;
      if (GetFileAttributesW(szNewDir) != INVALID_FILE_ATTRIBUTES) {
        m_szActivePresetList.clear();
        WritePrivateProfileStringW(L"Settings", L"szActivePresetList", L"", GetConfigIniFile());
        m_nSubdirMode = bRecursive ? 1 : 0;
        m_nCurrentPreset = -1;
        ChangePresetDir(szNewDir, m_szPresetDir);
        g_pipeServer.Send(L"SET_DIR_RESULT=OK|" + value);
      } else {
        g_pipeServer.Send(L"SET_DIR_RESULT=ERROR|directory not found: " + value);
      }
    }).detach();
  }
  else if (wcsncmp(sMessage, L"SHADER_IMPORT=", 14) == 0) {
    // Load JSON, convert GLSL→HLSL, apply.  Format: SHADER_IMPORT=<path>
    std::wstring filePath(sMessage + 14);
    DebugLogW((L"[SHADER_IMPORT] Loading: " + filePath).c_str());
    if (!m_shaderImportWindow)
        m_shaderImportWindow = std::make_unique<mdrop::ShaderImportWindow>(this);
    std::wstring result = m_shaderImportWindow->ImportFromFile(filePath.c_str());
    DebugLogW((L"[SHADER_IMPORT] Result: " + result).c_str());
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"SHADER_IMPORT_RESULT=" + result);
  }
  else if (wcsncmp(sMessage, L"SHADER_GLSL=", 12) == 0) {
    // Raw GLSL → convert + apply.  Format: SHADER_GLSL=<glsl_code>
    std::wstring wGlsl(sMessage + 12);
    std::string glsl;
    glsl.reserve(wGlsl.size());
    for (wchar_t ch : wGlsl)
      glsl += (ch < 128) ? (char)ch : '?';
    DebugLogA(("SHADER_GLSL: received " + std::to_string(glsl.size()) + " chars").c_str());
    if (!m_shaderImportWindow)
        m_shaderImportWindow = std::make_unique<mdrop::ShaderImportWindow>(this);
    std::wstring result = m_shaderImportWindow->ImportFromGLSL(glsl, true);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"SHADER_GLSL_RESULT=" + result);
  }
  else if (wcsncmp(sMessage, L"SHADER_CONVERT=", 15) == 0) {
    // Convert only, don't apply — returns HLSL.  Format: SHADER_CONVERT=<glsl_code>
    std::wstring wGlsl(sMessage + 15);
    std::string glsl;
    glsl.reserve(wGlsl.size());
    for (wchar_t ch : wGlsl)
      glsl += (ch < 128) ? (char)ch : '?';
    DebugLogA(("SHADER_CONVERT: received " + std::to_string(glsl.size()) + " chars").c_str());
    if (!m_shaderImportWindow)
        m_shaderImportWindow = std::make_unique<mdrop::ShaderImportWindow>(this);
    std::wstring result = m_shaderImportWindow->ImportFromGLSL(glsl, false);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"SHADER_CONVERT_RESULT=" + result);
  }
  else if (wcsncmp(sMessage, L"SHADER_SAVE=", 12) == 0) {
    // Save current shader passes as preset.  Format: SHADER_SAVE=<path.milk3>
    std::wstring savePath(sMessage + 12);
    DebugLogW((L"[SHADER_SAVE] " + savePath).c_str());
    if (!m_shaderImportWindow)
        m_shaderImportWindow = std::make_unique<mdrop::ShaderImportWindow>(this);
    std::wstring result = m_shaderImportWindow->SavePresetToFile(savePath.c_str());
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"SHADER_SAVE_RESULT=" + result);
  }
  else if (wcsncmp(sMessage, L"SET_LOGLEVEL=", 13) == 0) {
    // Change log level at runtime.  Format: SET_LOGLEVEL=<0-4>
    int newLevel = _wtoi(sMessage + 13);
    if (newLevel < 0) newLevel = 0;
    if (newLevel > 4) newLevel = 4;
    m_LogLevel = newLevel;
    DebugLogSetLevel(newLevel);
    if (mdropdx12) mdropdx12->logLevel = newLevel;
    WritePrivateProfileIntW(newLevel, L"LogLevel", GetConfigIniFile(), L"Milkwave");
    const wchar_t* names[] = { L"Off", L"Error", L"Warn", L"Info", L"Verbose" };
    wchar_t buf[64];
    swprintf_s(buf, L"LOGLEVEL=%d|%s", newLevel, names[newLevel]);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
    DLOG_INFO("Log level changed to %d via IPC", newLevel);
  }
  else if (wcsncmp(sMessage, L"DIAG_MIRRORS", 12) == 0) {
    // Dump mirror state for diagnostics
    extern PipeServer g_pipeServer;
    std::wstring response = L"MIRRORS|active=";
    response += m_bMirrorsActive ? L"1" : L"0";
    // Report which monitor the render window is on
    if (m_lpDX && m_lpDX->GetHwnd()) {
      HMONITOR hMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
      if (hMon) {
        MONITORINFOEXW mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
          response += L"|render_on=";
          response += mi.szDevice;
          RECT wr;
          GetWindowRect(m_lpDX->GetHwnd(), &wr);
          wchar_t buf[128];
          swprintf_s(buf, L",renderwin=(%d,%d)-(%d,%d) %dx%d",
            wr.left, wr.top, wr.right, wr.bottom,
            wr.right - wr.left, wr.bottom - wr.top);
          response += buf;
          // Render window opacity and style
          swprintf_s(buf, L",opacity=%.2f,fs=%d",
            fOpacity, IsBorderlessFullscreen(m_lpDX->GetHwnd()) ? 1 : 0);
          response += buf;
          LONG_PTR exStyle = GetWindowLongPtr(m_lpDX->GetHwnd(), GWL_EXSTYLE);
          response += (exStyle & WS_EX_TRANSPARENT) ? L",clickthru=1" : L",clickthru=0";
        }
      }
    }
    int idx = 0;
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      response += L"|mon";
      response += std::to_wstring(idx);
      response += L"=";
      response += out.config.szDeviceName;
      response += L",enabled=";
      response += out.config.bEnabled ? L"1" : L"0";
      response += L",opacity=";
      response += std::to_wstring(out.config.nOpacity);
      response += L",clickthru=";
      response += out.config.bClickThrough ? L"1" : L"0";
      response += L",skipped=";
      response += out.bSkippedSameMonitor ? L"1" : L"0";
      // Display area from enumeration
      auto& rc = out.config.rcMonitor;
      wchar_t rcBuf[128];
      swprintf_s(rcBuf, L",display=(%d,%d)-(%d,%d) %dx%d",
        rc.left, rc.top, rc.right, rc.bottom,
        rc.right - rc.left, rc.bottom - rc.top);
      response += rcBuf;
      if (out.monitorState) {
        auto& ms = *out.monitorState;
        response += L",ready=";
        response += ms.bReady ? L"1" : L"0";
        response += L",hwnd=";
        wchar_t hwndBuf[32];
        swprintf_s(hwndBuf, L"%p", (void*)ms.hWnd);
        response += hwndBuf;
        response += L",swapsize=";
        response += std::to_wstring(ms.width);
        response += L"x";
        response += std::to_wstring(ms.height);
        // Check if window is actually visible
        if (ms.hWnd) {
          RECT wr;
          GetWindowRect(ms.hWnd, &wr);
          swprintf_s(rcBuf, L",winrect=(%d,%d)-(%d,%d) %dx%d",
            wr.left, wr.top, wr.right, wr.bottom,
            wr.right - wr.left, wr.bottom - wr.top);
          response += rcBuf;
          response += IsWindowVisible(ms.hWnd) ? L",visible=1" : L",visible=0";
        }
      } else {
        response += L",state=none";
      }
      idx++;
    }
    g_pipeServer.Send(response.c_str());
  }
  else if (wcsncmp(sMessage, L"SET_MIRROR_OPACITY=", 19) == 0) {
    // Set opacity for monitor mirrors.
    // Format: SET_MIRROR_OPACITY=<1-100>         (all monitors)
    //         SET_MIRROR_OPACITY=<N>,<1-100>      (DISPLAY N by device name, e.g. \\.\DISPLAY1 = 1)
    const wchar_t* args = sMessage + 19;
    const wchar_t* comma = wcschr(args, L',');
    int displayNum = -1; // -1 = all
    int val;
    if (comma) {
      displayNum = _wtoi(args);  // DISPLAY number from device name
      val = _wtoi(comma + 1);
    } else {
      val = _wtoi(args);
    }
    if (val < 1) val = 1;
    if (val > 100) val = 100;
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      if (displayNum > 0) {
        // Match by DISPLAY number in device name (e.g. \\.\DISPLAY2 → 2)
        wchar_t target[32];
        swprintf_s(target, L"\\\\.\\DISPLAY%d", displayNum);
        if (wcscmp(out.config.szDeviceName, target) != 0) continue;
      }
      out.config.nOpacity = val;
    }
    m_bMirrorStylesDirty.store(true);
    SaveDisplayOutputSettings();
    RefreshDisplaysTab();
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    if (displayNum > 0)
      swprintf_s(buf, L"MIRROR_OPACITY=%d,%d", displayNum, val);
    else
      swprintf_s(buf, L"MIRROR_OPACITY=%d", val);
    g_pipeServer.Send(buf);
  }
  else if (wcsncmp(sMessage, L"SET_MIRROR_CLICKTHRU=", 21) == 0) {
    // Set click-through for all monitor mirrors.  Format: SET_MIRROR_CLICKTHRU=<0|1>
    bool val = (_wtoi(sMessage + 21) != 0);
    for (auto& out : m_displayOutputs)
      if (out.config.type == DisplayOutputType::Monitor)
        out.config.bClickThrough = val;
    m_bMirrorStylesDirty.store(true);
    SaveDisplayOutputSettings();
    RefreshDisplaysTab();
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    swprintf_s(buf, L"MIRROR_CLICKTHRU=%d", val ? 1 : 0);
    g_pipeServer.Send(buf);
  }
  else if (wcsncmp(sMessage, L"MOVE_TO_DISPLAY=", 16) == 0) {
    // Move render window to center of display N (1-based).  Format: MOVE_TO_DISPLAY=<N>
    int idx = _wtoi(sMessage + 16) - 1; // convert 1-based to 0-based
    int monIdx = 0;
    extern PipeServer g_pipeServer;
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      if (monIdx == idx) {
        RECT rc = out.config.rcMonitor;
        int cx = (rc.left + rc.right) / 2;
        int cy = (rc.top + rc.bottom) / 2;
        HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
        if (hRender) {
          RECT wr;
          GetWindowRect(hRender, &wr);
          int ww = wr.right - wr.left;
          int wh = wr.bottom - wr.top;
          SetWindowPos(hRender, nullptr, cx - ww/2, cy - wh/2, 0, 0,
              SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
          wchar_t buf[128];
          swprintf_s(buf, L"MOVED_TO=%ls", out.config.szDeviceName);
          g_pipeServer.Send(buf);
        }
        break;
      }
      monIdx++;
    }
  }
  else if (wcsncmp(sMessage, L"SET_WINDOW=", 11) == 0) {
    // Set render window position and size.  Format: SET_WINDOW=<x>,<y>,<w>,<h>
    int x, y, w, h;
    if (swscanf_s(sMessage + 11, L"%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
      HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
      if (hRender) {
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
        if (w <= 0 || h <= 0) flags |= SWP_NOSIZE;
        SetWindowPos(hRender, nullptr, x, y,
            (w > 0 ? w : 0), (h > 0 ? h : 0), flags);
        RECT wr;
        GetWindowRect(hRender, &wr);
        extern PipeServer g_pipeServer;
        wchar_t buf[128];
        swprintf_s(buf, L"WINDOW=(%d,%d)-(%d,%d) %dx%d",
            wr.left, wr.top, wr.right, wr.bottom,
            wr.right - wr.left, wr.bottom - wr.top);
        g_pipeServer.Send(buf);
      }
    }
  }
  else if (wcsncmp(sMessage, L"GET_LOGLEVEL", 12) == 0) {
    // Query current log level.
    const wchar_t* names[] = { L"Off", L"Error", L"Warn", L"Info", L"Verbose" };
    int lvl = g_debugLogLevel;
    if (lvl < 0) lvl = 0; if (lvl > 4) lvl = 4;
    wchar_t buf[64];
    swprintf_s(buf, L"LOGLEVEL=%d|%s", lvl, names[lvl]);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
  }
  else if (wcsncmp(sMessage, L"CLEAR_LOGS", 10) == 0) {
    // Delete all files in the log/ directory and re-open debug.log
    DebugLogClearAll();
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"LOGS_CLEARED");
  }
  else if (wcsncmp(sMessage, L"SHUTDOWN", 8) == 0) {
    // Clean shutdown via WM_CLOSE — saves settings, stops render thread, exits
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"SHUTTING_DOWN");
    PostMessage(GetPluginWindow(), WM_CLOSE, 0, 0);
  }
  else if (wcsncmp(sMessage, L"DIAG_DISPLAY_MODE=", 18) == 0) {
    // Toggle diagnostic display mode: 0=normal, 1=show VS[0] raw, 2=show VS[1] raw
    int mode = _wtoi(sMessage + 18);
    m_nDiagDisplayMode = (mode >= 0 && mode <= 2) ? mode : 0;
    extern PipeServer g_pipeServer;
    wchar_t resp[64];
    swprintf_s(resp, L"DIAG_DISPLAY_MODE=%d", m_nDiagDisplayMode);
    g_pipeServer.Send(resp);
    DLOG_INFO("DIAG_DISPLAY_MODE set to %d", m_nDiagDisplayMode);
  }
  else if (wcsncmp(sMessage, L"SET_AUDIO_GAIN=", 15) == 0) {
    float val = (float)_wtof(sMessage + 15);
    if (val <= 0.0f) val = 1.0f;
    if (val > 256.0f) val = 256.0f;
    m_fAudioSensitivity = val;
    extern float mdropdx12_audio_sensitivity;
    mdropdx12_audio_sensitivity = (val <= 1.0f) ? 1.0f : val;
    SaveSettingToINI(SET_AUDIO_SENSITIVITY);
    wchar_t buf[128];
    swprintf_s(buf, L"AUDIO_GAIN=%.2f|effective=%.2f", m_fAudioSensitivity, mdropdx12_audio_sensitivity);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
  }
  else if (wcsncmp(sMessage, L"GET_AUDIO_GAIN", 14) == 0) {
    extern float mdropdx12_audio_sensitivity;
    wchar_t buf[128];
    swprintf_s(buf, L"AUDIO_GAIN=%.2f|effective=%.2f", m_fAudioSensitivity, mdropdx12_audio_sensitivity);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
  }
  else if (wcsncmp(sMessage, L"GET_RENDER_DIAG", 15) == 0) {
    // Dump rendering diagnostic values for comparing with Milkwave
    float blur_min[3], blur_max[3];
    GetSafeBlurMinMax(m_pState, blur_min, blur_max);
    float fscale0 = 1.0f / (blur_max[0] - blur_min[0]);
    float fbias0 = -blur_min[0] * fscale0;
    float fscale1 = 0, fbias1 = 0, fscale2 = 0, fbias2 = 0;
    if (blur_max[0] - blur_min[0] > 0.0001f) {
      float t_min1 = (blur_min[1] - blur_min[0]) / (blur_max[0] - blur_min[0]);
      float t_max1 = (blur_max[1] - blur_min[0]) / (blur_max[0] - blur_min[0]);
      if (t_max1 - t_min1 > 0.0001f) {
        fscale1 = 1.0f / (t_max1 - t_min1);
        fbias1 = -t_min1 * fscale1;
      }
    }
    if (blur_max[1] - blur_min[1] > 0.0001f) {
      float t_min2 = (blur_min[2] - blur_min[1]) / (blur_max[1] - blur_min[1]);
      float t_max2 = (blur_max[2] - blur_min[1]) / (blur_max[1] - blur_min[1]);
      if (t_max2 - t_min2 > 0.0001f) {
        fscale2 = 1.0f / (t_max2 - t_min2);
        fbias2 = -t_min2 * fscale2;
      }
    }
    float decay = m_pState->var_pf_decay ? (float)*m_pState->var_pf_decay : 0;
    float gamma = m_pState->var_pf_gamma ? (float)*m_pState->var_pf_gamma : 0;
    float echo_alpha = m_pState->var_pf_echo_alpha ? (float)*m_pState->var_pf_echo_alpha : 0;
    float echo_zoom = m_pState->var_pf_echo_zoom ? (float)*m_pState->var_pf_echo_zoom : 0;
    // Compute aspect ratio matching ApplyShaderParams
    float diag_aspect_x = 1, diag_aspect_y = 1;
    if (!m_bScreenDependentRenderMode) {
      if (GetWidth() > GetHeight())
        diag_aspect_y = GetHeight() / (float)GetWidth();
      else
        diag_aspect_x = GetWidth() / (float)GetHeight();
    }
    float diag_time = GetTime() - m_pState->GetPresetStartTime();
    int nShapesVis = 0, nWavesVis = 0;
    for (int si = 0; si < MAX_CUSTOM_SHAPES; si++)
      if (m_pState->m_shape[si].enabled) nShapesVis++;
    for (int wi = 0; wi < MAX_CUSTOM_WAVES; wi++)
      if (m_pState->m_wave[wi].enabled) nWavesVis++;

    // Per-frame equation outputs for warp
    float pf_zoom = m_pState->var_pf_zoom ? (float)*m_pState->var_pf_zoom : 0;
    float pf_rot = m_pState->var_pf_rot ? (float)*m_pState->var_pf_rot : 0;
    float pf_warp = m_pState->var_pf_warp ? (float)*m_pState->var_pf_warp : 0;
    float pf_cx = m_pState->var_pf_cx ? (float)*m_pState->var_pf_cx : 0;
    float pf_cy = m_pState->var_pf_cy ? (float)*m_pState->var_pf_cy : 0;
    float pf_dx = m_pState->var_pf_dx ? (float)*m_pState->var_pf_dx : 0;
    float pf_dy = m_pState->var_pf_dy ? (float)*m_pState->var_pf_dy : 0;
    float pf_sx = m_pState->var_pf_sx ? (float)*m_pState->var_pf_sx : 0;
    float pf_sy = m_pState->var_pf_sy ? (float)*m_pState->var_pf_sy : 0;
    float pf_zoomexp = m_pState->var_pf_zoomexp ? (float)*m_pState->var_pf_zoomexp : 0;

    // Sample warp mesh UVs at corners and center
    int gw = m_nGridX, gh = m_nGridY;
    int ctrIdx = (gh / 2) * (gw + 1) + gw / 2;
    int tlIdx = 0;
    int trIdx = gw;
    int blIdx = gh * (gw + 1);
    int brIdx = gh * (gw + 1) + gw;
    float mesh_ctr_u = 0, mesh_ctr_v = 0;
    float mesh_tl_u = 0, mesh_tl_v = 0, mesh_br_u = 0, mesh_br_v = 0;
    if (m_verts) {
      mesh_ctr_u = m_verts[ctrIdx].tu; mesh_ctr_v = m_verts[ctrIdx].tv;
      mesh_tl_u = m_verts[tlIdx].tu; mesh_tl_v = m_verts[tlIdx].tv;
      mesh_br_u = m_verts[brIdx].tu; mesh_br_v = m_verts[brIdx].tv;
    }

    wchar_t buf[4096];
    swprintf_s(buf, 4096,
      L"RENDER_DIAG"
      L"|blur_min0=%.6f|blur_max0=%.6f|blur_min1=%.6f|blur_max1=%.6f|blur_min2=%.6f|blur_max2=%.6f"
      L"|fscale0=%.4f|fbias0=%.4f|fscale1=%.4f|fbias1=%.4f|fscale2=%.4f|fbias2=%.4f"
      L"|nHighestBlur=%d|decay=%.6f|gamma=%.4f|echo_alpha=%.4f|echo_zoom=%.4f"
      L"|zoom=%.6f|rot=%.6f|warp=%.6f|cx=%.6f|cy=%.6f|dx=%.6f|dy=%.6f|sx=%.6f|sy=%.6f|zoomexp=%.6f"
      L"|q1=%.6f|q2=%.6f|q3=%.6f|q4=%.6f|q5=%.6f|q6=%.6f|q7=%.6f|q8=%.6f"
      L"|q9=%.6f|q10=%.6f|q11=%.6f|q12=%.6f|q13=%.6f|q14=%.6f|q15=%.6f|q16=%.6f"
      L"|q17=%.6f|q18=%.6f|q19=%.6f|q20=%.6f|q21=%.6f|q22=%.6f|q23=%.6f|q24=%.6f"
      L"|q25=%.6f|q26=%.6f|q27=%.6f|q28=%.6f|q29=%.6f|q30=%.6f|q31=%.6f|q32=%.6f"
      L"|bass_rel=%.6f|mid_rel=%.6f|treb_rel=%.6f"
      L"|bass_imm=%.6f|mid_imm=%.6f|treb_imm=%.6f"
      L"|bass_avg=%.6f|mid_avg=%.6f|treb_avg=%.6f"
      L"|bass_lavg=%.6f|mid_lavg=%.6f|treb_lavg=%.6f"
      L"|warpPSVer=%d|compPSVer=%d"
      L"|texW=%d|texH=%d|aspect_x=%.6f|aspect_y=%.6f|gridW=%d|gridH=%d"
      L"|mesh_ctr=%.6f,%.6f|mesh_tl=%.6f,%.6f|mesh_br=%.6f,%.6f"
      L"|time=%.4f|frame=%d|shapes=%d|waves=%d|srate=%d|fps=%.1f|wave_peak=%.1f",
      blur_min[0], blur_max[0], blur_min[1], blur_max[1], blur_min[2], blur_max[2],
      fscale0, fbias0, fscale1, fbias1, fscale2, fbias2,
      m_nHighestBlurTexUsedThisFrame, decay, gamma, echo_alpha, echo_zoom,
      pf_zoom, pf_rot, pf_warp, pf_cx, pf_cy, pf_dx, pf_dy, pf_sx, pf_sy, pf_zoomexp,
      m_pState->var_pf_q[0] ? (float)*m_pState->var_pf_q[0] : 0.f,
      m_pState->var_pf_q[1] ? (float)*m_pState->var_pf_q[1] : 0.f,
      m_pState->var_pf_q[2] ? (float)*m_pState->var_pf_q[2] : 0.f,
      m_pState->var_pf_q[3] ? (float)*m_pState->var_pf_q[3] : 0.f,
      m_pState->var_pf_q[4] ? (float)*m_pState->var_pf_q[4] : 0.f,
      m_pState->var_pf_q[5] ? (float)*m_pState->var_pf_q[5] : 0.f,
      m_pState->var_pf_q[6] ? (float)*m_pState->var_pf_q[6] : 0.f,
      m_pState->var_pf_q[7] ? (float)*m_pState->var_pf_q[7] : 0.f,
      m_pState->var_pf_q[8] ? (float)*m_pState->var_pf_q[8] : 0.f,
      m_pState->var_pf_q[9] ? (float)*m_pState->var_pf_q[9] : 0.f,
      m_pState->var_pf_q[10] ? (float)*m_pState->var_pf_q[10] : 0.f,
      m_pState->var_pf_q[11] ? (float)*m_pState->var_pf_q[11] : 0.f,
      m_pState->var_pf_q[12] ? (float)*m_pState->var_pf_q[12] : 0.f,
      m_pState->var_pf_q[13] ? (float)*m_pState->var_pf_q[13] : 0.f,
      m_pState->var_pf_q[14] ? (float)*m_pState->var_pf_q[14] : 0.f,
      m_pState->var_pf_q[15] ? (float)*m_pState->var_pf_q[15] : 0.f,
      m_pState->var_pf_q[16] ? (float)*m_pState->var_pf_q[16] : 0.f,
      m_pState->var_pf_q[17] ? (float)*m_pState->var_pf_q[17] : 0.f,
      m_pState->var_pf_q[18] ? (float)*m_pState->var_pf_q[18] : 0.f,
      m_pState->var_pf_q[19] ? (float)*m_pState->var_pf_q[19] : 0.f,
      m_pState->var_pf_q[20] ? (float)*m_pState->var_pf_q[20] : 0.f,
      m_pState->var_pf_q[21] ? (float)*m_pState->var_pf_q[21] : 0.f,
      m_pState->var_pf_q[22] ? (float)*m_pState->var_pf_q[22] : 0.f,
      m_pState->var_pf_q[23] ? (float)*m_pState->var_pf_q[23] : 0.f,
      m_pState->var_pf_q[24] ? (float)*m_pState->var_pf_q[24] : 0.f,
      m_pState->var_pf_q[25] ? (float)*m_pState->var_pf_q[25] : 0.f,
      m_pState->var_pf_q[26] ? (float)*m_pState->var_pf_q[26] : 0.f,
      m_pState->var_pf_q[27] ? (float)*m_pState->var_pf_q[27] : 0.f,
      m_pState->var_pf_q[28] ? (float)*m_pState->var_pf_q[28] : 0.f,
      m_pState->var_pf_q[29] ? (float)*m_pState->var_pf_q[29] : 0.f,
      m_pState->var_pf_q[30] ? (float)*m_pState->var_pf_q[30] : 0.f,
      m_pState->var_pf_q[31] ? (float)*m_pState->var_pf_q[31] : 0.f,
      mysound.imm_rel[0], mysound.imm_rel[1], mysound.imm_rel[2],
      mysound.imm[0], mysound.imm[1], mysound.imm[2],
      mysound.avg[0], mysound.avg[1], mysound.avg[2],
      mysound.long_avg[0], mysound.long_avg[1], mysound.long_avg[2],
      m_pState->m_nWarpPSVersion, m_pState->m_nCompPSVersion,
      m_nTexSizeX, m_nTexSizeY, diag_aspect_x, diag_aspect_y, gw, gh,
      mesh_ctr_u, mesh_ctr_v, mesh_tl_u, mesh_tl_v, mesh_br_u, mesh_br_v,
      diag_time, (int)GetFrame(), nShapesVis, nWavesVis,
      SAMPLE_RATE, GetFps(),
      [&]() { float peak = 0; for (int k = 0; k < 576; k++) { float v = fabsf(mysound.fWave[0][k]); if (v > peak) peak = v; } return peak; }());
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
  }
  else if (wcsncmp(sMessage, L"GET_EEL_STATE", 13) == 0) {
    // Dump EEL megabuf/gmegabuf values and per-frame variable state
    // Format: GET_EEL_STATE [megabuf=START,COUNT] [gmegabuf=START,COUNT] [reg=START,COUNT]
    // Defaults: megabuf=0,32  gmegabuf=0,32  reg=0,100
    int mb_start = 0, mb_count = 32;
    int gmb_start = 0, gmb_count = 32;
    int reg_start = 0, reg_count = 100;

    // Parse optional parameters
    const wchar_t* p = sMessage + 13;
    while (*p) {
      while (*p == L' ') p++;
      if (wcsncmp(p, L"megabuf=", 8) == 0) {
        swscanf_s(p + 8, L"%d,%d", &mb_start, &mb_count);
      } else if (wcsncmp(p, L"gmegabuf=", 9) == 0) {
        swscanf_s(p + 9, L"%d,%d", &gmb_start, &gmb_count);
      } else if (wcsncmp(p, L"reg=", 4) == 0) {
        swscanf_s(p + 4, L"%d,%d", &reg_start, &reg_count);
      }
      while (*p && *p != L' ') p++;
    }

    // Clamp
    if (mb_count > 256) mb_count = 256;
    if (gmb_count > 256) gmb_count = 256;
    if (reg_count > 256) reg_count = 256;
    if (mb_count < 0) mb_count = 0;
    if (gmb_count < 0) gmb_count = 0;
    if (reg_count < 0) reg_count = 0;

    std::wstring result = L"EEL_STATE";

    // 1. Global registers (reg00-reg99)
    double* globalRegs = NSEEL_getglobalregs();
    if (globalRegs && reg_count > 0) {
      result += L"|REGS=";
      for (int i = 0; i < reg_count; i++) {
        if (i > 0) result += L",";
        wchar_t tmp[32];
        swprintf_s(tmp, L"%.6g", globalRegs[reg_start + i]);
        result += tmp;
      }
    }

    // 2. Megabuf (per-frame VM's local RAM)
    if (m_pState && m_pState->m_pf_eel && mb_count > 0) {
      result += L"|MEGABUF=";
      for (int i = 0; i < mb_count; i++) {
        if (i > 0) result += L",";
        int validCount = 0;
        EEL_F* ptr = NSEEL_VM_getramptr_noalloc(m_pState->m_pf_eel, mb_start + i, &validCount);
        wchar_t tmp[32];
        swprintf_s(tmp, L"%.6g", ptr ? (double)*ptr : 0.0);
        result += tmp;
      }
    }

    // 3. Global megabuf (gmegabuf)
    if (gmb_count > 0) {
      // Access the default gmegabuf (declared at file scope with extern "C")
      result += L"|GMEGABUF=";
      for (int i = 0; i < gmb_count; i++) {
        if (i > 0) result += L",";
        wchar_t tmp[32];
        double val = 0.0;
        if (nseel_gmembuf_default) {
          unsigned int offs = (unsigned int)(gmb_start + i) & ((1 << 20) - 1);
          val = (double)nseel_gmembuf_default[offs];
        }
        swprintf_s(tmp, L"%.6g", val);
        result += tmp;
      }
    }

    // 4. Monitor variable
    if (m_pState && m_pState->var_pf_monitor)
      result += L"|monitor=" + std::to_wstring((float)*m_pState->var_pf_monitor);

    // 5. Key per-frame output vars for blue haze (regNN values used by the preset)
    result += L"|frame=" + std::to_wstring((int)GetFrame());

    extern PipeServer g_pipeServer;
    g_pipeServer.Send(result.c_str());
  }
  else if (wcsncmp(sMessage, L"GET_AUDIO_DIAG", 14) == 0) {
    extern float mdropdx12_audio_sensitivity;
    extern float mdropdx12_amp_left;
    extern float mdropdx12_amp_right;
    extern unsigned char pcmLeftLpb[576];
    extern signed int pcmPos;
    // Sample a few PCM values from the buffer to check levels
    int p = pcmPos;
    int pcm0 = pcmLeftLpb[(p + 0) % 576];
    int pcm1 = pcmLeftLpb[(p + 100) % 576];
    int pcm2 = pcmLeftLpb[(p + 200) % 576];
    int pcm3 = pcmLeftLpb[(p + 300) % 576];
    int pcm4 = pcmLeftLpb[(p + 400) % 576];
    // Min/max scan
    int pcmMin = 255, pcmMax = 0;
    for (int i = 0; i < 576; i++) {
      int v = pcmLeftLpb[i];
      if (v < pcmMin) pcmMin = v;
      if (v > pcmMax) pcmMax = v;
    }
    wchar_t buf[512];
    swprintf_s(buf, 512,
      L"AUDIO_DIAG|gain=%.2f|effective=%.2f|ampL=%.2f|ampR=%.2f"
      L"|bass=%.3f|mid=%.3f|treb=%.3f"
      L"|bass_att=%.3f|mid_att=%.3f|treb_att=%.3f"
      L"|bass_imm=%.6f|mid_imm=%.6f|treb_imm=%.6f"
      L"|bass_avg=%.6f|mid_avg=%.6f|treb_avg=%.6f"
      L"|pcm_min=%d|pcm_max=%d|pcm_samples=%d,%d,%d,%d,%d",
      m_fAudioSensitivity, mdropdx12_audio_sensitivity,
      mdropdx12_amp_left, mdropdx12_amp_right,
      mysound.imm_rel[0], mysound.imm_rel[1], mysound.imm_rel[2],
      m_sound.avg[0][0], m_sound.avg[0][1], m_sound.avg[0][2],
      m_sound.imm[0][0], m_sound.imm[0][1], m_sound.imm[0][2],
      m_sound.avg[0][0], m_sound.avg[0][1], m_sound.avg[0][2],
      pcmMin, pcmMax, pcm0, pcm1, pcm2, pcm3, pcm4);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
  }
  else if (wcsncmp(sMessage, L"DEAUTH_DEVICE|", 14) == 0) {
    extern TcpServer g_tcpServer;
    extern PipeServer g_pipeServer;
    std::string deviceId = WideToUTF8(sMessage + 14);
    g_tcpServer.RemoveAuthorizedDevice(deviceId);
    g_tcpServer.DisconnectDevice(deviceId);
    g_tcpServer.SaveAuthorizedDevices(GetConfigIniFile());
    g_pipeServer.Send(L"DEAUTH_OK");
  }
  else if (wcscmp(sMessage, L"LIST_DEVICES") == 0) {
    extern TcpServer g_tcpServer;
    extern PipeServer g_pipeServer;
    auto devices = g_tcpServer.GetAuthorizedDevices();
    std::wstring response = L"DEVICES";
    for (auto& d : devices) {
      response += L"|id=";
      response += UTF8ToWide(d.id);
      response += L",name=";
      response += UTF8ToWide(d.name);
      response += L",added=";
      response += UTF8ToWide(d.dateAdded);
    }
    g_pipeServer.Send(response);
  }
  else {
    // Fallback: treat as pipe-chained script command (NEXT, PREV, LOCK,
    // SEND=0x.., etc.)  This unifies IPC and button board dispatch.
    ExecuteScriptLine(sMessage);
  }
}

void Engine::SendPresetChangedInfoToMDropDX12Remote() {
  std::wstring msg = L"PRESET=" + std::wstring(m_szCurrentPresetFile);
  SendMessageToMDropDX12Remote(msg.c_str(), true);
  SendPresetWaveInfoToMDropDX12Remote();
}

void Engine::SendTrackInfoToMDropDX12Remote() {
  extern MDropDX12 mdropdx12;
  std::wstring msg = L"TRACK|artist=" + mdropdx12.currentArtist
                   + L"|title=" + mdropdx12.currentTitle
                   + L"|album=" + mdropdx12.currentAlbum;
  SendMessageToMDropDX12Remote(msg.c_str(), true);
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
    + L"|LOCKED=" + std::wstring(m_bPresetLockedByUser ? L"1" : L"0")
    + L"|FFTATTACK=" + std::to_wstring(m_fFFTAttackGlobal)
    + L"|FFTDECAY=" + std::to_wstring(m_fFFTDecayGlobal);
  SendMessageToMDropDX12Remote(msg.c_str(), true);
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

  // do our own [UN-NORMALIZED] fft — use float circular buffer (matches Milkwave)
  float fWaveLeft[576];
  float fWaveRight[576];
  GetAudioBufFloat(fWaveLeft, fWaveRight, 576);

  memset(mysound.fSpecLeft, 0, sizeof(float) * MY_FFT_SAMPLES);
  memset(mysound.fSpecRight, 0, sizeof(float) * MY_FFT_SAMPLES);

  myfft.time_to_frequency_domain(fWaveLeft, mysound.fSpecLeft);
  myfft.time_to_frequency_domain(fWaveRight, mysound.fSpecRight);
  //for (i=0; i<MY_FFT_SAMPLES; i++) fSpecLeft[i] = sqrtf(fSpecLeft[i]*fSpecLeft[i] + fSpecTemp[i]*fSpecTemp[i]);

  // Compute clean (un-equalized) FFT for get_fft()/get_fft_hz() shader functions
  memset(mysound.fShaderSpecLeft, 0, sizeof(float) * MY_FFT_SAMPLES);
  memset(mysound.fShaderSpecRight, 0, sizeof(float) * MY_FFT_SAMPLES);
  m_fftShader.time_to_frequency_domain(fWaveLeft, mysound.fShaderSpecLeft);
  m_fftShader.time_to_frequency_domain(fWaveRight, mysound.fShaderSpecRight);

  // Update the sample rate (we don't need to check HRESULT every frame)
  using namespace std::chrono;
  static steady_clock::time_point lastCheck = steady_clock::now();
  auto now = steady_clock::now();
  if (duration_cast<seconds>(now - lastCheck).count() >= 5) // Check every 5 seconds
  {
    DetectSampleRate();
    lastCheck = now;
  }

  // sum spectrum up into 3 bands
  //DeepSeek - Updated Beat Detection Splitting Algorithm
  // Use effective post-downsample rate for bin mapping, not device native rate.
  // Audio is downsampled: effectiveRate = SAMPLE_RATE / floor(SAMPLE_RATE / TARGET_SAMPLE_RATE).
  // At 96kHz device: ratio=2, effective=48000. At 44.1kHz: ratio=1, effective=44100.
  // Using SAMPLE_RATE directly would halve the bins per band at 96kHz (Nyquist 48kHz vs actual 24kHz).
  int effectiveRate = SAMPLE_RATE;
  if (SAMPLE_RATE > TARGET_SAMPLE_RATE) {
    int downsampleRatio = SAMPLE_RATE / TARGET_SAMPLE_RATE;
    effectiveRate = SAMPLE_RATE / downsampleRatio;
  }

  for (int i = 0; i < 3; i++) {
    // Calculate which FFT bins correspond to our frequency ranges
    int start_bin, end_bin;

    switch (i) {
    case 0: // Bass (0-250Hz)
      start_bin = (int)(BASS_MIN * MY_FFT_SAMPLES / (effectiveRate / 2));
      end_bin = (int)(BASS_MAX * MY_FFT_SAMPLES / (effectiveRate / 2));
      break;
    case 1: // Mid (250-4000Hz)
      start_bin = (int)(MID_MIN * MY_FFT_SAMPLES / (effectiveRate / 2));
      end_bin = (int)(MID_MAX * MY_FFT_SAMPLES / (effectiveRate / 2));
      break;
    case 2: // Treble (4000-20000Hz)
      start_bin = (int)(TREBLE_MIN * MY_FFT_SAMPLES / (effectiveRate / 2));
      end_bin = (int)(TREBLE_MAX * MY_FFT_SAMPLES / (effectiveRate / 2));
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

    // smooth — O(1) ring buffer insert, then average the most recent entries
    {
      int bufMax = td_mysounddata::RECENT_BUF_MAX;
      mysound.recent_buf[i][mysound.recent_pos[i]] = mysound.imm_rel[i];
      mysound.recent_pos[i] = (mysound.recent_pos[i] + 1) % bufMax;
      if (mysound.recent_len[i] < bufMax) mysound.recent_len[i]++;

      int nAvg = min(mysound.recent_len[i], recentBufferSize);
      float sum = 0;
      for (int k = 0; k < nAvg; k++) {
        int idx = (mysound.recent_pos[i] - 1 - k + bufMax) % bufMax;
        sum += mysound.recent_buf[i][idx];
      }
      mysound.smooth[i] = (nAvg > 0) ? sum / nAvg : 0;
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

const wchar_t* Engine::GetPresetName(int idx) {
  if (idx >= 0 && idx < m_nPresets)
    return m_presets[idx].szFilename.c_str();
  return L"";
}

//----------------------------------------------------------------------


} // namespace mdrop
