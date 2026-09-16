/*
  Plugin module: Messages, Sprites, Remote Communication & Audio
  Extracted from engine.cpp for maintainability.
  Contains: Custom messages, supertexts, sprites, song title animations,
            remote communication, screenshots, audio analysis, misc utilities
*/

#include "tcp_server.h"  // Must be before engine.h — winsock2.h must precede windows.h
#include "engine.h"
#include "window_diag.h"
#include "preset_hash.h"
#include "shader_overrides.h"
#include "audio_profile_store.h"
#include "engine_helpers.h"
#include "tool_window.h"
#include "pipe_server.h"
#include "ipc_help.h"
#include "audio_capture.h"
#include <algorithm>
#include <thread>
#include "utility.h"
#include "AutoCharFn.h"
#include "format_to.h"
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
#include "config_store.h"
#include "config_backend.h"


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

//----------------------------------------------------------------------
// Video Effects parameters, addressable by name over IPC
//
// The Video Effects window covered every one of these; IPC covered none of
// them, so a video mix could be built by hand in the window but not driven
// from Milkwave Remote, a script or the MCP server. One table gives SET_VFX /
// GET_VFX the window's full coverage without thirty command names.
//
// Wire names match the [VideoFX] INI keys exactly, so a value seen in
// settings.ini can be set over the pipe with no translation in between --
// the same rule the render tunables follow.
//----------------------------------------------------------------------

namespace {

enum VfxKind { VFXK_FLOAT, VFXK_BOOL, VFXK_INT };

struct VfxParam {
  const wchar_t* name;
  VfxKind kind;
  size_t  off;        // byte offset into VideoEffectParams
  float   lo, hi;     // clamp range; ignored for VFXK_BOOL
};

typedef VideoEffectParams VP;
typedef AudioLink AL;

// AudioLink members are addressed as an offset pair so the audio-reactive rows
// need no special kind of their own.
#define VFX_AR_SRC(field) (offsetof(VP, field) + offsetof(AL, source))
#define VFX_AR_INT(field) (offsetof(VP, field) + offsetof(AL, intensity))

const VfxParam kVfxParams[] = {
  // Transform
  { L"PosX",       VFXK_FLOAT, offsetof(VP, posX),       -1.0f,  1.0f },
  { L"PosY",       VFXK_FLOAT, offsetof(VP, posY),       -1.0f,  1.0f },
  { L"Scale",      VFXK_FLOAT, offsetof(VP, scale),       0.1f,  5.0f },
  { L"Rotation",   VFXK_FLOAT, offsetof(VP, rotation),    0.0f, 360.0f },
  { L"MirrorH",    VFXK_BOOL,  offsetof(VP, mirrorH),     0.0f,  1.0f },
  { L"MirrorV",    VFXK_BOOL,  offsetof(VP, mirrorV),     0.0f,  1.0f },
  // Colour
  { L"TintR",      VFXK_FLOAT, offsetof(VP, tintR),       0.0f,  2.0f },
  { L"TintG",      VFXK_FLOAT, offsetof(VP, tintG),       0.0f,  2.0f },
  { L"TintB",      VFXK_FLOAT, offsetof(VP, tintB),       0.0f,  2.0f },
  { L"Brightness", VFXK_FLOAT, offsetof(VP, brightness), -1.0f,  1.0f },
  { L"Contrast",   VFXK_FLOAT, offsetof(VP, contrast),    0.0f,  3.0f },
  { L"Saturation", VFXK_FLOAT, offsetof(VP, saturation),  0.0f,  3.0f },
  { L"HueShift",   VFXK_FLOAT, offsetof(VP, hueShift),    0.0f, 360.0f },
  { L"Invert",     VFXK_BOOL,  offsetof(VP, invert),      0.0f,  1.0f },
  // Effects
  { L"Pixelation", VFXK_FLOAT, offsetof(VP, pixelation),  0.0f,  1.0f },
  { L"Chromatic",  VFXK_FLOAT, offsetof(VP, chromatic),   0.0f,  0.05f },
  { L"EdgeDetect", VFXK_BOOL,  offsetof(VP, edgeDetect),  0.0f,  1.0f },
  { L"BlendMode",  VFXK_INT,   offsetof(VP, blendMode),   0.0f,  5.0f },
  // Audio-reactive links. Source: 0=none 1=bass 2=mid 3=treb 4=vol
  { L"AR_PosX_Source",          VFXK_INT,   VFX_AR_SRC(arPosX),       0.0f, 4.0f },
  { L"AR_PosX_Intensity",       VFXK_FLOAT, VFX_AR_INT(arPosX),       0.0f, 2.0f },
  { L"AR_PosY_Source",          VFXK_INT,   VFX_AR_SRC(arPosY),       0.0f, 4.0f },
  { L"AR_PosY_Intensity",       VFXK_FLOAT, VFX_AR_INT(arPosY),       0.0f, 2.0f },
  { L"AR_Scale_Source",         VFXK_INT,   VFX_AR_SRC(arScale),      0.0f, 4.0f },
  { L"AR_Scale_Intensity",      VFXK_FLOAT, VFX_AR_INT(arScale),      0.0f, 2.0f },
  { L"AR_Rotation_Source",      VFXK_INT,   VFX_AR_SRC(arRotation),   0.0f, 4.0f },
  { L"AR_Rotation_Intensity",   VFXK_FLOAT, VFX_AR_INT(arRotation),   0.0f, 2.0f },
  { L"AR_Brightness_Source",    VFXK_INT,   VFX_AR_SRC(arBrightness), 0.0f, 4.0f },
  { L"AR_Brightness_Intensity", VFXK_FLOAT, VFX_AR_INT(arBrightness), 0.0f, 2.0f },
  { L"AR_Saturation_Source",    VFXK_INT,   VFX_AR_SRC(arSaturation), 0.0f, 4.0f },
  { L"AR_Saturation_Intensity", VFXK_FLOAT, VFX_AR_INT(arSaturation), 0.0f, 2.0f },
  { L"AR_Chromatic_Source",     VFXK_INT,   VFX_AR_SRC(arChromatic),  0.0f, 4.0f },
  { L"AR_Chromatic_Intensity",  VFXK_FLOAT, VFX_AR_INT(arChromatic),  0.0f, 2.0f },
};
const int kVfxParamCount = (int)(sizeof(kVfxParams) / sizeof(kVfxParams[0]));

const VfxParam* FindVfxParam(const wchar_t* name) {
  for (int i = 0; i < kVfxParamCount; i++)
    if (_wcsicmp(kVfxParams[i].name, name) == 0) return &kVfxParams[i];
  return nullptr;
}

float GetVfxValue(const VP& fx, const VfxParam& pm) {
  const char* base = (const char*)&fx;
  switch (pm.kind) {
    case VFXK_BOOL:  return *(const bool*)(base + pm.off) ? 1.0f : 0.0f;
    case VFXK_INT:   return (float)*(const int*)(base + pm.off);
    default:         return *(const float*)(base + pm.off);
  }
}

void SetVfxValue(VP& fx, const VfxParam& pm, float v) {
  char* base = (char*)&fx;
  switch (pm.kind) {
    case VFXK_BOOL:
      *(bool*)(base + pm.off) = (v != 0.0f);
      break;
    case VFXK_INT: {
      int iv = (int)(v + (v < 0 ? -0.5f : 0.5f));
      if (iv < (int)pm.lo) iv = (int)pm.lo;
      if (iv > (int)pm.hi) iv = (int)pm.hi;
      *(int*)(base + pm.off) = iv;
      break;
    }
    default:
      if (v < pm.lo) v = pm.lo;
      if (v > pm.hi) v = pm.hi;
      *(float*)(base + pm.off) = v;
      break;
  }
}

// Format one parameter for the wire: ints and bools as integers, floats with
// enough precision to round-trip through the INI (which writes %.4f).
std::wstring FormatVfxValue(const VP& fx, const VfxParam& pm) {
  wchar_t buf[64];
  const float v = GetVfxValue(fx, pm);
  if (pm.kind == VFXK_FLOAT) FormatTo(buf, L"%.4f", v);
  else                       FormatTo(buf, L"%d", (int)v);
  return buf;
}

// Split "Name=Value" into its two halves. Returns false when there is no '='.
bool SplitNameValue(const wchar_t* arg, std::wstring& name, std::wstring& value) {
  const wchar_t* eq = wcschr(arg, L'=');
  if (!eq) return false;
  name.assign(arg, eq - arg);
  value.assign(eq + 1);
  return true;
}

}  // anonymous namespace

// A tool window shows values IPC can now change underneath it. Each runs its
// own message pump on its own thread, so refresh by posting the rebuild message
// rather than touching its controls from the caller's thread.
static void RepaintToolWindow(ToolWindow* tw) {
  if (!tw) return;
  HWND h = tw->GetHWND();
  if (h && IsWindow(h)) PostMessageW(h, WM_MW_REBUILD_FONTS, 0, 0);
}

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
    ConfigFile(m_szMsgIniFile).SetString(section, L"face", m_CustomMessageFont[n].szFace);
    wchar_t val[32];
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].bBold ? 1 : 0);
    ConfigFile(m_szMsgIniFile).SetString(section, L"bold", val);
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].bItal ? 1 : 0);
    ConfigFile(m_szMsgIniFile).SetString(section, L"ital", val);
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].nColorR);
    ConfigFile(m_szMsgIniFile).SetString(section, L"r", val);
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].nColorG);
    ConfigFile(m_szMsgIniFile).SetString(section, L"g", val);
    swprintf(val, 32, L"%d", m_CustomMessageFont[n].nColorB);
    ConfigFile(m_szMsgIniFile).SetString(section, L"b", val);
  }

  // Write message definitions
  for (int n = 0; n < MAX_CUSTOM_MESSAGES; n++) {
    wchar_t section[64];
    swprintf(section, 64, L"message%02d", n);

    if (m_CustomMessage[n].szText[0] == 0) {
      // Delete the section for empty messages
      ConfigFile(m_szMsgIniFile).RemoveSection(section);
      continue;
    }

    ConfigFile(m_szMsgIniFile).SetString(section, L"text", m_CustomMessage[n].szText);
    wchar_t val[64];
    swprintf(val, 64, L"%d", m_CustomMessage[n].nFont);
    ConfigFile(m_szMsgIniFile).SetString(section, L"font", val);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fSize);
    ConfigFile(m_szMsgIniFile).SetString(section, L"size", val);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].x);
    ConfigFile(m_szMsgIniFile).SetString(section, L"x", val);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].y);
    ConfigFile(m_szMsgIniFile).SetString(section, L"y", val);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].randx);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randx", val);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].randy);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randy", val);
    swprintf(val, 64, L"%.2f", m_CustomMessage[n].growth);
    ConfigFile(m_szMsgIniFile).SetString(section, L"growth", val);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fTime);
    ConfigFile(m_szMsgIniFile).SetString(section, L"time", val);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fFade);
    ConfigFile(m_szMsgIniFile).SetString(section, L"fade", val);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fFadeOut);
    ConfigFile(m_szMsgIniFile).SetString(section, L"fadeout", val);
    swprintf(val, 64, L"%.1f", m_CustomMessage[n].fBurnTime);
    ConfigFile(m_szMsgIniFile).SetString(section, L"burntime", val);

    // Color
    swprintf(val, 64, L"%d", m_CustomMessage[n].nColorR);
    ConfigFile(m_szMsgIniFile).SetString(section, L"r", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nColorG);
    ConfigFile(m_szMsgIniFile).SetString(section, L"g", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nColorB);
    ConfigFile(m_szMsgIniFile).SetString(section, L"b", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nRandR);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randr", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nRandG);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randg", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].nRandB);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randb", val);

    // Overrides
    if (m_CustomMessage[n].bOverrideFace)
      ConfigFile(m_szMsgIniFile).SetString(section, L"face", m_CustomMessage[n].szFace);
    if (m_CustomMessage[n].bOverrideBold) {
      swprintf(val, 64, L"%d", m_CustomMessage[n].bBold ? 1 : 0);
      ConfigFile(m_szMsgIniFile).SetString(section, L"bold", val);
    }
    if (m_CustomMessage[n].bOverrideItal) {
      swprintf(val, 64, L"%d", m_CustomMessage[n].bItal ? 1 : 0);
      ConfigFile(m_szMsgIniFile).SetString(section, L"ital", val);
    }

    // Per-message randomize flags
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandPos);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_pos", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandSize);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_size", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandFont);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_font", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandColor);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_color", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandEffects);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_effects", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandGrowth);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_growth", val);
    swprintf(val, 64, L"%d", m_CustomMessage[n].bRandDuration);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_duration", val);

    // Animation profile reference
    swprintf(val, 64, L"%d", m_CustomMessage[n].nAnimProfile);
    ConfigFile(m_szMsgIniFile).SetString(section, L"animprofile", val);
  }

  // Write animation profiles
  WriteAnimProfiles();
}

// ======== Animation Profiles ========

void Engine::CreateDefaultAnimProfiles() {
  m_nAnimProfileCount = 5;

  // 1. Center Pop
  m_AnimProfiles[0] = td_anim_profile();
  CopyTo(m_AnimProfiles[0].szName, L"Center Pop");
  m_AnimProfiles[0].fX = 0.5f;
  m_AnimProfiles[0].fY = 0.5f;
  m_AnimProfiles[0].fGrowth = 1.2f;
  m_AnimProfiles[0].fDuration = 3.0f;
  CopyTo(m_AnimProfiles[0].szFontFace, L"Bahnschrift");
  m_AnimProfiles[0].bBold = 1;

  // 2. Slide from Left
  m_AnimProfiles[1] = td_anim_profile();
  CopyTo(m_AnimProfiles[1].szName, L"Slide from Left");
  m_AnimProfiles[1].fX = 0.5f;
  m_AnimProfiles[1].fY = 0.5f;
  m_AnimProfiles[1].fStartX = -0.3f;
  m_AnimProfiles[1].fStartY = 0.5f;
  m_AnimProfiles[1].fMoveTime = 0.8f;
  m_AnimProfiles[1].nEaseMode = 2;
  m_AnimProfiles[1].fDuration = 5.0f;
  CopyTo(m_AnimProfiles[1].szFontFace, L"Segoe UI");

  // 3. Slide from Right
  m_AnimProfiles[2] = td_anim_profile();
  CopyTo(m_AnimProfiles[2].szName, L"Slide from Right");
  m_AnimProfiles[2].fX = 0.5f;
  m_AnimProfiles[2].fY = 0.5f;
  m_AnimProfiles[2].fStartX = 1.3f;
  m_AnimProfiles[2].fStartY = 0.5f;
  m_AnimProfiles[2].fMoveTime = 0.8f;
  m_AnimProfiles[2].nEaseMode = 2;
  m_AnimProfiles[2].fDuration = 5.0f;
  CopyTo(m_AnimProfiles[2].szFontFace, L"Segoe UI");

  // 4. Bottom Crawl
  m_AnimProfiles[3] = td_anim_profile();
  CopyTo(m_AnimProfiles[3].szName, L"Bottom Crawl");
  m_AnimProfiles[3].fX = 0.5f;
  m_AnimProfiles[3].fY = 0.85f;
  m_AnimProfiles[3].fGrowth = 1.0f;
  m_AnimProfiles[3].fDuration = 8.0f;
  m_AnimProfiles[3].fFadeOut = 2.0f;
  CopyTo(m_AnimProfiles[3].szFontFace, L"Segoe UI");

  // 5. Top Flash
  m_AnimProfiles[4] = td_anim_profile();
  CopyTo(m_AnimProfiles[4].szName, L"Top Flash");
  m_AnimProfiles[4].fX = 0.5f;
  m_AnimProfiles[4].fY = 0.15f;
  m_AnimProfiles[4].fGrowth = 0.8f;
  m_AnimProfiles[4].fDuration = 2.0f;
  m_AnimProfiles[4].bBold = 1;
  m_AnimProfiles[4].fBurnTime = 0.3f;
  CopyTo(m_AnimProfiles[4].szFontFace, L"Bahnschrift");
}

void Engine::ReadAnimProfiles() {
  m_nAnimProfileCount = ConfigFile(m_szMsgIniFile).GetInt(L"AnimProfiles", L"Count", 0);

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

    ConfigFile(m_szMsgIniFile).GetStringTo(section, L"name", L"", m_AnimProfiles[n].szName, 64);
    m_AnimProfiles[n].bEnabled = ConfigFile(m_szMsgIniFile).GetBool(section, L"enabled", true);

    // Position
    m_AnimProfiles[n].fX = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"x", 0.5f);
    m_AnimProfiles[n].fY = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"y", 0.5f);
    m_AnimProfiles[n].fRandX = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"randx", 0.0f);
    m_AnimProfiles[n].fRandY = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"randy", 0.0f);

    // Entry animation
    m_AnimProfiles[n].fStartX = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"startx", -100.0f);
    m_AnimProfiles[n].fStartY = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"starty", -100.0f);
    m_AnimProfiles[n].fMoveTime = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"movetime", 0.0f);
    m_AnimProfiles[n].nEaseMode = ConfigFile(m_szMsgIniFile).GetInt(section, L"easemode", 2);
    m_AnimProfiles[n].fEaseFactor = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"easefactor", 2.0f);

    // Appearance
    ConfigFile(m_szMsgIniFile).GetStringTo(section, L"face", L"", m_AnimProfiles[n].szFontFace, 128);
    m_AnimProfiles[n].fFontSize = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"size", 50.0f);
    m_AnimProfiles[n].bBold = ConfigFile(m_szMsgIniFile).GetInt(section, L"bold", 0);
    m_AnimProfiles[n].bItal = ConfigFile(m_szMsgIniFile).GetInt(section, L"ital", 0);
    m_AnimProfiles[n].nColorR = ConfigFile(m_szMsgIniFile).GetInt(section, L"r", 255);
    m_AnimProfiles[n].nColorG = ConfigFile(m_szMsgIniFile).GetInt(section, L"g", 255);
    m_AnimProfiles[n].nColorB = ConfigFile(m_szMsgIniFile).GetInt(section, L"b", 255);
    m_AnimProfiles[n].nRandR = ConfigFile(m_szMsgIniFile).GetInt(section, L"randr", 0);
    m_AnimProfiles[n].nRandG = ConfigFile(m_szMsgIniFile).GetInt(section, L"randg", 0);
    m_AnimProfiles[n].nRandB = ConfigFile(m_szMsgIniFile).GetInt(section, L"randb", 0);

    // Timing
    m_AnimProfiles[n].fDuration = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"duration", 5.0f);
    m_AnimProfiles[n].fFadeIn = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"fadein", 0.2f);
    m_AnimProfiles[n].fFadeOut = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"fadeout", 0.5f);
    m_AnimProfiles[n].fBurnTime = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"burntime", 0.0f);

    // Effects
    m_AnimProfiles[n].fGrowth = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"growth", 1.0f);
    m_AnimProfiles[n].fShadowOffset = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"shadow", 0.0f);
    m_AnimProfiles[n].fBoxAlpha = ConfigFile(m_szMsgIniFile).GetFloat(section, (wchar_t*)L"boxalpha", 0.0f);
    m_AnimProfiles[n].nBoxColR = ConfigFile(m_szMsgIniFile).GetInt(section, L"boxr", 0);
    m_AnimProfiles[n].nBoxColG = ConfigFile(m_szMsgIniFile).GetInt(section, L"boxg", 0);
    m_AnimProfiles[n].nBoxColB = ConfigFile(m_szMsgIniFile).GetInt(section, L"boxb", 0);

    // Randomization flags
    m_AnimProfiles[n].bRandPos = ConfigFile(m_szMsgIniFile).GetInt(section, L"rand_pos", 0);
    m_AnimProfiles[n].bRandSize = ConfigFile(m_szMsgIniFile).GetInt(section, L"rand_size", 0);
    m_AnimProfiles[n].bRandColor = ConfigFile(m_szMsgIniFile).GetInt(section, L"rand_color", 0);
    m_AnimProfiles[n].bRandGrowth = ConfigFile(m_szMsgIniFile).GetInt(section, L"rand_growth", 0);
    m_AnimProfiles[n].bRandDuration = ConfigFile(m_szMsgIniFile).GetInt(section, L"rand_duration", 0);
  }
}

void Engine::WriteAnimProfiles() {
  wchar_t val[64];

  // Write count
  swprintf(val, 64, L"%d", m_nAnimProfileCount);
  ConfigFile(m_szMsgIniFile).SetString(L"AnimProfiles", L"Count", val);

  // Clear any old profiles beyond current count
  for (int n = m_nAnimProfileCount; n < MAX_ANIM_PROFILES; n++) {
    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);
    ConfigFile(m_szMsgIniFile).RemoveSection(section);
  }

  for (int n = 0; n < m_nAnimProfileCount; n++) {
    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);

    ConfigFile(m_szMsgIniFile).SetString(section, L"name", m_AnimProfiles[n].szName);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bEnabled ? 1 : 0);
    ConfigFile(m_szMsgIniFile).SetString(section, L"enabled", val);

    // Position
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fX);
    ConfigFile(m_szMsgIniFile).SetString(section, L"x", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fY);
    ConfigFile(m_szMsgIniFile).SetString(section, L"y", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fRandX);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randx", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fRandY);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randy", val);

    // Entry animation
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fStartX);
    ConfigFile(m_szMsgIniFile).SetString(section, L"startx", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fStartY);
    ConfigFile(m_szMsgIniFile).SetString(section, L"starty", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fMoveTime);
    ConfigFile(m_szMsgIniFile).SetString(section, L"movetime", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nEaseMode);
    ConfigFile(m_szMsgIniFile).SetString(section, L"easemode", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fEaseFactor);
    ConfigFile(m_szMsgIniFile).SetString(section, L"easefactor", val);

    // Appearance
    ConfigFile(m_szMsgIniFile).SetString(section, L"face", m_AnimProfiles[n].szFontFace);
    swprintf(val, 64, L"%.1f", m_AnimProfiles[n].fFontSize);
    ConfigFile(m_szMsgIniFile).SetString(section, L"size", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bBold);
    ConfigFile(m_szMsgIniFile).SetString(section, L"bold", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bItal);
    ConfigFile(m_szMsgIniFile).SetString(section, L"ital", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nColorR);
    ConfigFile(m_szMsgIniFile).SetString(section, L"r", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nColorG);
    ConfigFile(m_szMsgIniFile).SetString(section, L"g", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nColorB);
    ConfigFile(m_szMsgIniFile).SetString(section, L"b", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nRandR);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randr", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nRandG);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randg", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nRandB);
    ConfigFile(m_szMsgIniFile).SetString(section, L"randb", val);

    // Timing
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fDuration);
    ConfigFile(m_szMsgIniFile).SetString(section, L"duration", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fFadeIn);
    ConfigFile(m_szMsgIniFile).SetString(section, L"fadein", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fFadeOut);
    ConfigFile(m_szMsgIniFile).SetString(section, L"fadeout", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fBurnTime);
    ConfigFile(m_szMsgIniFile).SetString(section, L"burntime", val);

    // Effects
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fGrowth);
    ConfigFile(m_szMsgIniFile).SetString(section, L"growth", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fShadowOffset);
    ConfigFile(m_szMsgIniFile).SetString(section, L"shadow", val);
    swprintf(val, 64, L"%.2f", m_AnimProfiles[n].fBoxAlpha);
    ConfigFile(m_szMsgIniFile).SetString(section, L"boxalpha", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nBoxColR);
    ConfigFile(m_szMsgIniFile).SetString(section, L"boxr", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nBoxColG);
    ConfigFile(m_szMsgIniFile).SetString(section, L"boxg", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].nBoxColB);
    ConfigFile(m_szMsgIniFile).SetString(section, L"boxb", val);

    // Randomization flags
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandPos);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_pos", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandSize);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_size", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandColor);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_color", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandGrowth);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_growth", val);
    swprintf(val, 64, L"%d", m_AnimProfiles[n].bRandDuration);
    ConfigFile(m_szMsgIniFile).SetString(section, L"rand_duration", val);
  }
}

void Engine::ExportAnimProfiles(wchar_t* szPath) {
  wchar_t val[64];
  swprintf(val, 64, L"%d", m_nAnimProfileCount);
  ConfigFile(szPath).SetString(L"AnimProfiles", L"Count", val);

  for (int n = 0; n < m_nAnimProfileCount; n++) {
    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);
    const td_anim_profile& p = m_AnimProfiles[n];

    ConfigFile(szPath).SetString(section, L"name", p.szName);
    swprintf(val, 64, L"%d", p.bEnabled ? 1 : 0);
    ConfigFile(szPath).SetString(section, L"enabled", val);

    swprintf(val, 64, L"%.2f", p.fX); ConfigFile(szPath).SetString(section, L"x", val);
    swprintf(val, 64, L"%.2f", p.fY); ConfigFile(szPath).SetString(section, L"y", val);
    swprintf(val, 64, L"%.2f", p.fRandX); ConfigFile(szPath).SetString(section, L"randx", val);
    swprintf(val, 64, L"%.2f", p.fRandY); ConfigFile(szPath).SetString(section, L"randy", val);

    swprintf(val, 64, L"%.2f", p.fStartX); ConfigFile(szPath).SetString(section, L"startx", val);
    swprintf(val, 64, L"%.2f", p.fStartY); ConfigFile(szPath).SetString(section, L"starty", val);
    swprintf(val, 64, L"%.2f", p.fMoveTime); ConfigFile(szPath).SetString(section, L"movetime", val);
    swprintf(val, 64, L"%d", p.nEaseMode); ConfigFile(szPath).SetString(section, L"easemode", val);
    swprintf(val, 64, L"%.2f", p.fEaseFactor); ConfigFile(szPath).SetString(section, L"easefactor", val);

    ConfigFile(szPath).SetString(section, L"face", p.szFontFace);
    swprintf(val, 64, L"%.1f", p.fFontSize); ConfigFile(szPath).SetString(section, L"size", val);
    swprintf(val, 64, L"%d", p.bBold); ConfigFile(szPath).SetString(section, L"bold", val);
    swprintf(val, 64, L"%d", p.bItal); ConfigFile(szPath).SetString(section, L"ital", val);
    swprintf(val, 64, L"%d", p.nColorR); ConfigFile(szPath).SetString(section, L"r", val);
    swprintf(val, 64, L"%d", p.nColorG); ConfigFile(szPath).SetString(section, L"g", val);
    swprintf(val, 64, L"%d", p.nColorB); ConfigFile(szPath).SetString(section, L"b", val);
    swprintf(val, 64, L"%d", p.nRandR); ConfigFile(szPath).SetString(section, L"randr", val);
    swprintf(val, 64, L"%d", p.nRandG); ConfigFile(szPath).SetString(section, L"randg", val);
    swprintf(val, 64, L"%d", p.nRandB); ConfigFile(szPath).SetString(section, L"randb", val);

    swprintf(val, 64, L"%.2f", p.fDuration); ConfigFile(szPath).SetString(section, L"duration", val);
    swprintf(val, 64, L"%.2f", p.fFadeIn); ConfigFile(szPath).SetString(section, L"fadein", val);
    swprintf(val, 64, L"%.2f", p.fFadeOut); ConfigFile(szPath).SetString(section, L"fadeout", val);
    swprintf(val, 64, L"%.2f", p.fBurnTime); ConfigFile(szPath).SetString(section, L"burntime", val);

    swprintf(val, 64, L"%.2f", p.fGrowth); ConfigFile(szPath).SetString(section, L"growth", val);
    swprintf(val, 64, L"%.2f", p.fShadowOffset); ConfigFile(szPath).SetString(section, L"shadow", val);
    swprintf(val, 64, L"%.2f", p.fBoxAlpha); ConfigFile(szPath).SetString(section, L"boxalpha", val);
    swprintf(val, 64, L"%d", p.nBoxColR); ConfigFile(szPath).SetString(section, L"boxr", val);
    swprintf(val, 64, L"%d", p.nBoxColG); ConfigFile(szPath).SetString(section, L"boxg", val);
    swprintf(val, 64, L"%d", p.nBoxColB); ConfigFile(szPath).SetString(section, L"boxb", val);

    swprintf(val, 64, L"%d", p.bRandPos); ConfigFile(szPath).SetString(section, L"rand_pos", val);
    swprintf(val, 64, L"%d", p.bRandSize); ConfigFile(szPath).SetString(section, L"rand_size", val);
    swprintf(val, 64, L"%d", p.bRandColor); ConfigFile(szPath).SetString(section, L"rand_color", val);
    swprintf(val, 64, L"%d", p.bRandGrowth); ConfigFile(szPath).SetString(section, L"rand_growth", val);
    swprintf(val, 64, L"%d", p.bRandDuration); ConfigFile(szPath).SetString(section, L"rand_duration", val);
  }
}

void Engine::ImportAnimProfiles(wchar_t* szPath) {
  int count = ConfigFile(szPath).GetInt(L"AnimProfiles", L"Count", 0);
  if (count <= 0) return;
  if (count > MAX_ANIM_PROFILES) count = MAX_ANIM_PROFILES;

  m_nAnimProfileCount = count;
  for (int n = 0; n < count; n++) {
    m_AnimProfiles[n] = td_anim_profile();
    wchar_t section[32];
    swprintf(section, 32, L"AnimProfile%02d", n);

    ConfigFile(szPath).GetStringTo(section, L"name", L"", m_AnimProfiles[n].szName, 64);
    m_AnimProfiles[n].bEnabled = ConfigFile(szPath).GetBool(section, L"enabled", true);

    m_AnimProfiles[n].fX = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"x", 0.5f);
    m_AnimProfiles[n].fY = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"y", 0.5f);
    m_AnimProfiles[n].fRandX = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"randx", 0.0f);
    m_AnimProfiles[n].fRandY = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"randy", 0.0f);

    m_AnimProfiles[n].fStartX = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"startx", -100.0f);
    m_AnimProfiles[n].fStartY = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"starty", -100.0f);
    m_AnimProfiles[n].fMoveTime = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"movetime", 0.0f);
    m_AnimProfiles[n].nEaseMode = ConfigFile(szPath).GetInt(section, L"easemode", 2);
    m_AnimProfiles[n].fEaseFactor = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"easefactor", 2.0f);

    ConfigFile(szPath).GetStringTo(section, L"face", L"", m_AnimProfiles[n].szFontFace, 128);
    m_AnimProfiles[n].fFontSize = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"size", 50.0f);
    m_AnimProfiles[n].bBold = ConfigFile(szPath).GetInt(section, L"bold", 0);
    m_AnimProfiles[n].bItal = ConfigFile(szPath).GetInt(section, L"ital", 0);
    m_AnimProfiles[n].nColorR = ConfigFile(szPath).GetInt(section, L"r", 255);
    m_AnimProfiles[n].nColorG = ConfigFile(szPath).GetInt(section, L"g", 255);
    m_AnimProfiles[n].nColorB = ConfigFile(szPath).GetInt(section, L"b", 255);
    m_AnimProfiles[n].nRandR = ConfigFile(szPath).GetInt(section, L"randr", 0);
    m_AnimProfiles[n].nRandG = ConfigFile(szPath).GetInt(section, L"randg", 0);
    m_AnimProfiles[n].nRandB = ConfigFile(szPath).GetInt(section, L"randb", 0);

    m_AnimProfiles[n].fDuration = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"duration", 5.0f);
    m_AnimProfiles[n].fFadeIn = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"fadein", 0.2f);
    m_AnimProfiles[n].fFadeOut = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"fadeout", 0.5f);
    m_AnimProfiles[n].fBurnTime = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"burntime", 0.0f);

    m_AnimProfiles[n].fGrowth = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"growth", 1.0f);
    m_AnimProfiles[n].fShadowOffset = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"shadow", 0.0f);
    m_AnimProfiles[n].fBoxAlpha = ConfigFile(szPath).GetFloat(section, (wchar_t*)L"boxalpha", 0.0f);
    m_AnimProfiles[n].nBoxColR = ConfigFile(szPath).GetInt(section, L"boxr", 0);
    m_AnimProfiles[n].nBoxColG = ConfigFile(szPath).GetInt(section, L"boxg", 0);
    m_AnimProfiles[n].nBoxColB = ConfigFile(szPath).GetInt(section, L"boxb", 0);

    m_AnimProfiles[n].bRandPos = ConfigFile(szPath).GetInt(section, L"rand_pos", 0);
    m_AnimProfiles[n].bRandSize = ConfigFile(szPath).GetInt(section, L"rand_size", 0);
    m_AnimProfiles[n].bRandColor = ConfigFile(szPath).GetInt(section, L"rand_color", 0);
    m_AnimProfiles[n].bRandGrowth = ConfigFile(szPath).GetInt(section, L"rand_growth", 0);
    m_AnimProfiles[n].bRandDuration = ConfigFile(szPath).GetInt(section, L"rand_duration", 0);
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
    CopyTo(st.nFontFace, prof.szFontFace);
  st.fFontSize = prof.fFontSize;
  st.bExplicitSize = true;
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

  CopyTo(st.szTextW, m_szSongTitle);
  st.bIsSongTitle = false;
  st.bRedrawSuperText = true;

  // Determine profile
  int profIdx = m_nSongTitleAnimProfile;
  if (profIdx == -2) profIdx = PickRandomAnimProfile();

  if (profIdx >= 0 && profIdx < m_nAnimProfileCount) {
    ApplyAnimProfileToSupertext(st, m_AnimProfiles[profIdx]);
  } else {
    // Fallback defaults
    CopyTo(st.nFontFace, L"Segoe UI");
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
  wchar_t val[32];

  swprintf(val, 32, L"%d", m_bMsgAutoplay ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgAutoplay", val);
  swprintf(val, 32, L"%d", m_bMsgSequential ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgSequential", val);
  Config().SetFloat((wchar_t*)L"Milkwave", (wchar_t*)L"MsgAutoplayInterval", m_fMsgAutoplayInterval);
  Config().SetFloat((wchar_t*)L"Milkwave", (wchar_t*)L"MsgAutoplayJitter", m_fMsgAutoplayJitter);
  swprintf(val, 32, L"%d", m_bMessageAutoSize ? 1 : 0);
  Config().SetString(L"Milkwave", L"MessageAutoSize", val);

  // Save override settings
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomFont ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideRandomFont", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomColor ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideRandomColor", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomSize ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideRandomSize", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomEffects ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideRandomEffects", val);
  swprintf(val, 32, L"%.2f", m_fMsgOverrideSizeMin);
  Config().SetString(L"Milkwave", L"MsgOverrideSizeMin", val);
  swprintf(val, 32, L"%.2f", m_fMsgOverrideSizeMax);
  Config().SetString(L"Milkwave", L"MsgOverrideSizeMax", val);
  swprintf(val, 32, L"%d", m_nMsgMaxOnScreen);
  Config().SetString(L"Milkwave", L"MsgMaxOnScreen", val);
  // Animation overrides
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomPos ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideRandomPos", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomGrowth ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideRandomGrowth", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideSlideIn ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideSlideIn", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomDuration ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideRandomDuration", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideShadow ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideShadow", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideBox ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideBox", val);
  // Color shifting overrides
  swprintf(val, 32, L"%d", m_bMsgOverrideApplyHueShift ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideApplyHueShift", val);
  swprintf(val, 32, L"%d", m_bMsgOverrideRandomHue ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgOverrideRandomHue", val);
  swprintf(val, 32, L"%d", m_bMsgIgnorePerMsgRandom ? 1 : 0);
  Config().SetString(L"Milkwave", L"MsgIgnorePerMsgRandom", val);

  // Save playback order
  swprintf(val, 32, L"%d", m_nMsgAutoplayCount);
  Config().SetString(L"MsgOrder", L"Count", val);
  for (int i = 0; i < m_nMsgAutoplayCount; i++) {
    wchar_t key[32];
    swprintf(key, 32, L"Msg%d", i);
    swprintf(val, 32, L"%d", m_nMsgAutoplayOrder[i]);
    Config().SetString(L"MsgOrder", key, val);
  }
}

void Engine::LoadMsgAutoplaySettings() {

  m_bMsgAutoplay = Config().GetInt(L"Milkwave", L"MsgAutoplay", 0) != 0;
  m_bMsgSequential = Config().GetInt(L"Milkwave", L"MsgSequential", 0) != 0;
  m_fMsgAutoplayInterval = Config().GetFloat(L"Milkwave", L"MsgAutoplayInterval", 30.0f);
  m_fMsgAutoplayJitter = Config().GetFloat(L"Milkwave", L"MsgAutoplayJitter", 5.0f);
  m_bMessageAutoSize = Config().GetInt(L"Milkwave", L"MessageAutoSize", 1) != 0;

  // Load override settings
  m_bMsgOverrideRandomFont = Config().GetInt(L"Milkwave", L"MsgOverrideRandomFont", 0) != 0;
  m_bMsgOverrideRandomColor = Config().GetInt(L"Milkwave", L"MsgOverrideRandomColor", 0) != 0;
  m_bMsgOverrideRandomSize = Config().GetInt(L"Milkwave", L"MsgOverrideRandomSize", 0) != 0;
  m_bMsgOverrideRandomEffects = Config().GetInt(L"Milkwave", L"MsgOverrideRandomEffects", 0) != 0;
  {
    wchar_t tmp[32];
    Config().GetStringTo(L"Milkwave", L"MsgOverrideSizeMin", L"10", tmp, 32);
    m_fMsgOverrideSizeMin = (float)_wtof(tmp);
    Config().GetStringTo(L"Milkwave", L"MsgOverrideSizeMax", L"40", tmp, 32);
    m_fMsgOverrideSizeMax = (float)_wtof(tmp);
  }
  m_nMsgMaxOnScreen = Config().GetInt(L"Milkwave", L"MsgMaxOnScreen", 1);
  // Animation overrides
  m_bMsgOverrideRandomPos = Config().GetInt(L"Milkwave", L"MsgOverrideRandomPos", 0) != 0;
  m_bMsgOverrideRandomGrowth = Config().GetInt(L"Milkwave", L"MsgOverrideRandomGrowth", 0) != 0;
  m_bMsgOverrideSlideIn = Config().GetInt(L"Milkwave", L"MsgOverrideSlideIn", 0) != 0;
  m_bMsgOverrideRandomDuration = Config().GetInt(L"Milkwave", L"MsgOverrideRandomDuration", 0) != 0;
  m_bMsgOverrideShadow = Config().GetInt(L"Milkwave", L"MsgOverrideShadow", 0) != 0;
  m_bMsgOverrideBox = Config().GetInt(L"Milkwave", L"MsgOverrideBox", 0) != 0;
  // Color shifting overrides
  m_bMsgOverrideApplyHueShift = Config().GetInt(L"Milkwave", L"MsgOverrideApplyHueShift", 0) != 0;
  m_bMsgOverrideRandomHue = Config().GetInt(L"Milkwave", L"MsgOverrideRandomHue", 0) != 0;
  m_bMsgIgnorePerMsgRandom = Config().GetInt(L"Milkwave", L"MsgIgnorePerMsgRandom", 0) != 0;
  if (m_fMsgOverrideSizeMin < 0.01f) m_fMsgOverrideSizeMin = 0.01f;
  if (m_fMsgOverrideSizeMax > 100.0f) m_fMsgOverrideSizeMax = 100.0f;
  if (m_fMsgOverrideSizeMin >= m_fMsgOverrideSizeMax) m_fMsgOverrideSizeMin = m_fMsgOverrideSizeMax * 0.5f;
  if (m_nMsgMaxOnScreen < 1) m_nMsgMaxOnScreen = 1;
  if (m_nMsgMaxOnScreen > NUM_SUPERTEXTS) m_nMsgMaxOnScreen = NUM_SUPERTEXTS;

  // Load playback order (if saved); otherwise use default order
  int count = Config().GetInt(L"MsgOrder", L"Count", 0);
  if (count > 0 && count <= MAX_CUSTOM_MESSAGES) {
    m_nMsgAutoplayCount = 0;
    for (int i = 0; i < count; i++) {
      wchar_t key[32];
      swprintf(key, 32, L"Msg%d", i);
      int idx = Config().GetInt(L"MsgOrder", key, -1);
      if (idx >= 0 && idx < MAX_CUSTOM_MESSAGES && m_CustomMessage[idx].szText[0]) {
        m_nMsgAutoplayOrder[m_nMsgAutoplayCount++] = idx;
      }
    }
  } else {
    BuildMsgPlaybackOrder();
  }
}

// The animation clock moved by 'delta' seconds without any real time passing.
// Every absolute GetTime() value the app has stored has to move with it, or the
// work it schedules is stranded for as long as the clock jumped back.
//
// Written after a Ctrl+T poll in DoTime() (now gone) was found zeroing the clock
// from any application on the machine: autoplay messages stopped for the rest of
// the session, and so did automatic preset changes. The remaining caller is the
// 250000-second wrap, which is rare but does exactly the same damage.
//
// Values are shifted only when they are actually scheduling something; the
// sentinels (-1 = none, 0 = unset) have to survive unchanged.
void Engine::OnAnimationTimeRebased(float delta) {
  if (delta == 0.0f)
    return;

  m_fStartTime += delta;
  m_fPresetStartTime += delta;
  m_fPresetUsageStart += delta;
  m_prev_time += delta;
  m_AutoHueTimeLastChange += delta;
  m_fShowRatingUntilThisTime += delta;

  if (m_fNextAutoMsgTime > 0)        m_fNextAutoMsgTime += delta;
  if (m_fNextPresetTime > 0)         m_fNextPresetTime += delta;
  if (m_fPresetNameShowUntil >= 0)   m_fPresetNameShowUntil += delta;
  if (m_fLoadStartTime != 0)         m_fLoadStartTime += delta;
  if (m_fPendingStartupSaveTime > 0) m_fPendingStartupSaveTime += delta;

  m_script.lastLineTime += (double)delta;

  // A supertext stamped in the future neither draws (fProgress < 0) nor retires
  // (fProgress never reaches 1), and while it sits there it still counts against
  // m_nMsgMaxOnScreen -- so leaked slots block autoplay even once the clock has
  // caught up. -1 is the free slot and must stay -1.
  for (int i = 0; i < NUM_SUPERTEXTS; i++)
    if (m_supertexts[i].fStartTime >= 0)
      m_supertexts[i].fStartTime += delta;

  if (m_pState)    m_pState->m_fBlendStartTime += delta;
  if (m_pOldState) m_pOldState->m_fBlendStartTime += delta;
}

// The interval by itself, so every clock arms the same way (#255).
//
// There is more than one clock now: the primary's m_fNextAutoMsgTime, and one
// per display running its own preset. They share the store, the order and the
// jitter settings -- only WHEN each fires differs -- so the arithmetic lives
// here once rather than being copied per clock and drifting.
float Engine::NextAutoMessageInterval() {
  float jitter = m_fMsgAutoplayJitter * ((rand() % 2001 - 1000) / 1000.0f);
  float interval = m_fMsgAutoplayInterval + jitter;
  return interval < 1.0f ? 1.0f : interval;
}

void Engine::ScheduleNextAutoMessage() {
  if (!m_bMsgAutoplay || m_nMsgAutoplayCount == 0) {
    m_fNextAutoMsgTime = -1.0f;
    return;
  }
  m_fNextAutoMsgTime = GetTime() + NextAutoMessageInterval();
}

// One display's turn of the message autoplay loop (#255).
//
// Lifted verbatim from the block that used to sit inline in RenderFrame, and
// then given its state as parameters instead of reading the globals -- so the
// primary passes its own fields and each own-preset display passes its own.
// Same store, same order, same overrides; different clock.
//
// `ownerDisplay` is -1 for the primary or a 1-based DISPLAYn, and is stamped on
// whatever it spawns so the draw side knows whose it is.
void Engine::TickMessageAutoplay(int ownerDisplay, double& nextAt,
                                 int& nextSeq, int maxOnScreen) {
  if (!m_bMsgAutoplay || m_nMsgAutoplayCount <= 0)
    return;

  // Same backstop as the primary's: a next-message time further out than one
  // interval plus its jitter cannot have been scheduled from this clock, so
  // the clock jumped back. Re-arm rather than wait out the gap in silence.
  // Also arms a clock that has never been set (-1), which is how a display
  // joins in the first time it renders.
  if (nextAt <= 0.0 ||
      nextAt - GetTime() > m_fMsgAutoplayInterval + m_fMsgAutoplayJitter + 1.0f) {
    nextAt = GetTime() + NextAutoMessageInterval();
    return;
  }

  if (!MessagesEnabled() || Restrained(restraint::kMessages))
    return;
  if (GetTime() < nextAt)
    return;

  // Only THIS display's messages count against its limit. The slots are one
  // shared pool, so without the owner test a busy panel would hold every other
  // panel below its own limit.
  int nActive = 0;
  for (int j = 0; j < NUM_SUPERTEXTS; j++) {
    if (!m_supertexts[j].bIsSongTitle &&
        m_supertexts[j].nOwnerDisplay == ownerDisplay &&
        m_supertexts[j].fStartTime >= 0 &&
        GetTime() < m_supertexts[j].fStartTime + m_supertexts[j].fDuration +
                    m_supertexts[j].fFadeOutTime)
      nActive++;
  }
  if (nActive >= maxOnScreen)
    return;

  int msgIdx;
  if (m_bMsgSequential) {
    if (nextSeq >= m_nMsgAutoplayCount)
      nextSeq = 0;
    msgIdx = m_nMsgAutoplayOrder[nextSeq++];
  } else {
    msgIdx = m_nMsgAutoplayOrder[rand() % m_nMsgAutoplayCount];
  }
  LaunchCustomMessage(msgIdx, ownerDisplay);
  nextAt = GetTime() + NextAutoMessageInterval();
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
    m_hFontCombo = CreateCombo(m_hWnd, IDC_MSGEDIT_FONT_COMBO, margin + MulDiv(74, lineH,
      20), yPos, rw - MulDiv(74, lineH, 20), 300, hFont, CBS_DROPDOWNLIST | WS_VSCROLL, true, WS_EX_CLIENTEDGE);
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
      HWND hAPCombo = CreateCombo(m_hWnd, IDC_MSGEDIT_ANIM_PROFILE, xVal, yPos, apComboW,
        lineH * 10, hFont);
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
      CopyTo(lf.lfFaceName, curFace);
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
        CopyTo(szFace, lf.lfFaceName);
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
      CopyTo(m->szText, szText);
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
      CopyTo(m->szFace, szFace);
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
    CopyTo(dlg.szText, m->szText);
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
    CopyTo(dlg.szFace, m->szFace);
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
    CopyTo(m->szText, dlg.szText);
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
    CopyTo(m->szFace, dlg.szFace);
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
    CopyTo(m_CustomMessageFont[n].szFace, L"arial");
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
    CopyTo(m_CustomMessage[n].szFace, L"arial");
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
    FormatTo(szSectionName, L"font%02d", n);

    // get face, bold, italic, x, y for this custom message FONT
    ConfigFile(m_szMsgIniFile).GetStringTo(szSectionName, L"face", L"arial", m_CustomMessageFont[n].szFace);
    m_CustomMessageFont[n].bBold = ConfigFile(m_szMsgIniFile).GetBool(szSectionName, L"bold", m_CustomMessageFont[n].bBold);
    m_CustomMessageFont[n].bItal = ConfigFile(m_szMsgIniFile).GetBool(szSectionName, L"ital", m_CustomMessageFont[n].bItal);
    m_CustomMessageFont[n].nColorR = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"r", m_CustomMessageFont[n].nColorR);
    m_CustomMessageFont[n].nColorG = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"g", m_CustomMessageFont[n].nColorG);
    m_CustomMessageFont[n].nColorB = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"b", m_CustomMessageFont[n].nColorB);
  }

  for (n = 0; n < MAX_CUSTOM_MESSAGES; n++) {
    wchar_t szSectionName[64];
    FormatTo(szSectionName, L"message%02d", n);

    // get fontID, size, text, etc. for this custom message:
    ConfigFile(m_szMsgIniFile).GetStringTo(szSectionName, L"text", L"", m_CustomMessage[n].szText);
    if (m_CustomMessage[n].szText[0]) {
      m_CustomMessage[n].nFont = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"font", m_CustomMessage[n].nFont);
      m_CustomMessage[n].fSize = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"size", m_CustomMessage[n].fSize);
      m_CustomMessage[n].x = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"x", m_CustomMessage[n].x);
      m_CustomMessage[n].y = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"y", m_CustomMessage[n].y);
      m_CustomMessage[n].randx = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"randx", m_CustomMessage[n].randx);
      m_CustomMessage[n].randy = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"randy", m_CustomMessage[n].randy);

      m_CustomMessage[n].growth = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"growth", m_CustomMessage[n].growth);
      m_CustomMessage[n].fTime = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"time", m_CustomMessage[n].fTime);

      m_CustomMessage[n].fFade = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"fade", m_MessageDefaultFadeinTime);
      m_CustomMessage[n].fFadeOut = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"fadeout", m_MessageDefaultFadeoutTime);
      m_CustomMessage[n].fBurnTime = ConfigFile(m_szMsgIniFile).GetFloat(szSectionName, L"burntime", m_MessageDefaultBurnTime);

      m_CustomMessage[n].nColorR = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"r", m_CustomMessage[n].nColorR);
      m_CustomMessage[n].nColorG = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"g", m_CustomMessage[n].nColorG);
      m_CustomMessage[n].nColorB = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"b", m_CustomMessage[n].nColorB);
      m_CustomMessage[n].nRandR = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"randr", m_CustomMessage[n].nRandR);
      m_CustomMessage[n].nRandG = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"randg", m_CustomMessage[n].nRandG);
      m_CustomMessage[n].nRandB = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"randb", m_CustomMessage[n].nRandB);

      // overrides: r,g,b,face,bold,ital
      ConfigFile(m_szMsgIniFile).GetStringTo(szSectionName, L"face", L"", m_CustomMessage[n].szFace);
      m_CustomMessage[n].bBold = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"bold", -1);
      m_CustomMessage[n].bItal = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"ital", -1);
      m_CustomMessage[n].nColorR = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"r", -1);
      m_CustomMessage[n].nColorG = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"g", -1);
      m_CustomMessage[n].nColorB = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"b", -1);

      m_CustomMessage[n].bOverrideFace = (m_CustomMessage[n].szFace[0] != 0);
      m_CustomMessage[n].bOverrideBold = (m_CustomMessage[n].bBold != -1);
      m_CustomMessage[n].bOverrideItal = (m_CustomMessage[n].bItal != -1);
      m_CustomMessage[n].bOverrideColorR = (m_CustomMessage[n].nColorR != -1);
      m_CustomMessage[n].bOverrideColorG = (m_CustomMessage[n].nColorG != -1);
      m_CustomMessage[n].bOverrideColorB = (m_CustomMessage[n].nColorB != -1);

      // Per-message randomize flags
      m_CustomMessage[n].bRandPos = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"rand_pos", 0);
      m_CustomMessage[n].bRandSize = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"rand_size", 0);
      m_CustomMessage[n].bRandFont = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"rand_font", 0);
      m_CustomMessage[n].bRandColor = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"rand_color", 0);
      m_CustomMessage[n].bRandEffects = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"rand_effects", 0);
      m_CustomMessage[n].bRandGrowth = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"rand_growth", 0);
      m_CustomMessage[n].bRandDuration = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"rand_duration", 0);

      // Animation profile reference
      m_CustomMessage[n].nAnimProfile = ConfigFile(m_szMsgIniFile).GetInt(szSectionName, L"animprofile", -1);
    }
  }

  // Read animation profiles
  ReadAnimProfiles();
}

void Engine::LaunchCustomMessage(int nMsgNum, int ownerDisplay) {
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

  int nextFreeSupertextIndex = GetNextFreeSupertextIndex(ownerDisplay);
  if (nextFreeSupertextIndex > -1) {
    m_supertexts[nextFreeSupertextIndex].bRedrawSuperText = true;
    m_supertexts[nextFreeSupertextIndex].bIsSongTitle = false;
    CopyTo(m_supertexts[nextFreeSupertextIndex].szTextW, m_CustomMessage[nMsgNum].szText);

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
      CopyTo(m_supertexts[nextFreeSupertextIndex].nFontFace, m_CustomMessage[nMsgNum].szFace);
    else
      CopyTo(m_supertexts[nextFreeSupertextIndex].nFontFace, m_CustomMessageFont[fontID].szFace);
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
        CopyTo(m_supertexts[nextFreeSupertextIndex].nFontFace, m_CustomMessageFont[pick].szFace);
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
          CopyTo(st.nFontFace, m_CustomMessageFont[pick].szFace);
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
  CopyTo(m_supertexts[supertextIndex].szTextW, m_szSongTitle);

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
  CopyTo(m_supertexts[supertextIndex].nFontFace, m_fontinfo[SONGTITLE_FONT].szFace);
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


// SET_VSYNC / GET_VSYNC. A helper reached by an early return from
// LaunchMessage rather than two more links in the else-if chain: that chain is
// AT MSVC's block-nesting limit, and these two were the ones that tipped it --
// error C1061, measured, not anticipated. Same reason HandleCanvasIPC and
// HandlePresetCycleIPC exist.
//
// Returns false for anything it does not recognise, so the fallback that hands
// unmatched messages to the script engine is untouched.
bool Engine::HandleVSyncIPC(const wchar_t* sMessage) {
  if (MSG_IS(sMessage, L"SET_VSYNC=")) {
    // Present sync interval. 1 = wait for vblank, 0 = present immediately
    // (tearing allowed where supported). Reply: VSYNC=<0|1>
    //
    // Added for #121, where its absence was actively dangerous rather than
    // merely inconvenient. VSync defaults ON (engineshell.h:210) and
    // dxcontext.cpp:547 turns that into SyncInterval 1, which pins EVERY
    // surface to the panel's refresh regardless of SET_FPS, of which process
    // owns the window, and of whether Windows is throttling anything at all.
    // A frame-rate experiment that could not turn it off could therefore
    // measure 60 against 60 and "prove" whatever it set out to prove. Reaching
    // it meant hand-editing settings.ini before launch, which is exactly the
    // kind of out-of-band step a harness forgets.
    //
    // Deliberately NOT gated on testing mode: the write goes through
    // ConfigStore, so the shield already keeps a test's value out of the user's
    // file, and a rate the harness cannot change is worse than one it can.
    int v = _wtoi(sMessage + 10) ? 1 : 0;
    m_bEnableVSync = (v != 0);
    Config().SetInt(L"Milkwave", L"EnableVSync", m_bEnableVSync);
    // Keep the Visual window's checkbox honest if it happens to be open. Same
    // shape as SetFPSCap's combo sync (engine_input.cpp:52-57): look it up
    // defensively, because the window is on its own thread and may be closing.
    HWND hVisual = m_visualWindow ? m_visualWindow->GetHWND() : NULL;
    if (hVisual && IsWindow(hVisual)) {
      HWND hCheck = GetDlgItem(hVisual, IDC_MW_VSYNC_ENABLED);
      if (hCheck)
        SendMessage(hCheck, BM_SETCHECK, m_bEnableVSync ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    // Reach the children too. A -child is a separate process with its own
    // Engine and its own m_bEnableVSync, read from settings.ini at launch, so
    // without this the parent's displays and the children's would present under
    // different rules -- and a user who turns VSync off would see it take effect
    // on some of their monitors and not others.
    //
    // Safe against recursion by construction: BroadcastToChildren returns
    // immediately when m_bChildMode is set (child_instances.cpp:1461), and a
    // child has no children of its own.
    BroadcastToChildren(sMessage);
    extern PipeServer g_pipeServer;
    wchar_t buf[48];
    FormatTo(buf, L"VSYNC=%d", v);
    g_pipeServer.Send(buf);
    DLOG_INFO("VSync %s via IPC (broadcast to children)", v ? "enabled" : "disabled");
    return true;
  }
  if (MSG_IS(sMessage, L"GET_VSYNC")) {
    // Query. Replies with the scalar setting first, then one line per display
    // output, then VSYNC_END:
    //
    //   VSYNC=<0|1>
    //   VSYNC_OUT|display=<name>|type=<primary|mirror|child>|vsync=<0|1>
    //             |effective=<0|1>|source=<engine|fixed|broadcast>|state=<…>
    //   VSYNC_END
    //
    // The scalar line comes first and keeps its old meaning, so a client that
    // only wants "is VSync on" parses exactly what it did before.
    //
    // Per-output because the single bool is NOT the whole truth, and the gap is
    // the kind that makes a measurement lie:
    //
    //   primary  m_bEnableVSync, except that mirrors force it on --
    //            m_bSerializeWithMirrors (engine_displays.cpp:2627/:3037) is
    //            OR-ed into the sync interval at dxcontext.cpp:547. So with
    //            mirrors up the primary vsyncs however the setting reads, which
    //            is why effective= is reported separately from vsync=.
    //   mirror   always 0. The panels Present(0, ...) unconditionally
    //            (engine_displays.cpp:2438 and :3069); no setting reaches them.
    //   child    a separate process with its own copy of the flag. SET_VSYNC
    //            broadcasts, so an OWNED child tracks this value -- source=
    //            says broadcast rather than engine to keep "what we pushed"
    //            honestly distinct from "what we read back". A bare -child with
    //            no owner is not in m_children and is not covered; ask it on its
    //            own pipe.
    extern PipeServer g_pipeServer;
    wchar_t buf[256];
    const bool bSerialized = (m_lpDX && m_lpDX->m_bSerializeWithMirrors);
    FormatTo(buf, L"VSYNC=%d", m_bEnableVSync ? 1 : 0);
    g_pipeServer.Send(buf);

    FormatTo(buf, L"VSYNC_OUT|display=<primary>|type=primary|vsync=%d"
                    L"|effective=%d|source=engine|state=ready",
               m_bEnableVSync ? 1 : 0,
               (m_bEnableVSync || bSerialized) ? 1 : 0);
    g_pipeServer.Send(buf);

    for (auto& out : m_displayOutputs) {
      if (!out.config.bEnabled) continue;
      if (out.config.type != DisplayOutputType::Monitor) continue;
      std::wstring line = L"VSYNC_OUT|display=";
      line += out.config.szDeviceName;
      if (out.config.bOwnProcess) {
        ChildInstance ci;
        const bool have = GetChildInfo(out.config.szDeviceName, ci);
        line += L"|type=child|vsync=" + std::to_wstring(m_bEnableVSync ? 1 : 0);
        line += L"|effective=" + std::to_wstring(m_bEnableVSync ? 1 : 0);
        line += L"|source=broadcast|state=";
        line += have ? (ci.bFailed ? L"failed" : ci.bReady ? L"ready" : L"starting")
                     : L"absent";
      } else {
        line += L"|type=mirror|vsync=0|effective=0|source=fixed|state=";
        line += m_bMirrorsActive ? L"ready" : L"inactive";
      }
      g_pipeServer.Send(line.c_str());
    }
    g_pipeServer.Send(L"VSYNC_END");
    return true;
  }
  return false;
}


// Canvas-limit, canvas-metric and annotation-query commands.
//
// Split out of LaunchMessage because that function is one long else-if chain
// and adding these tipped it past MSVC's block nesting limit (C1061). Returns
// true when the message was handled.
// Preset cycling: order, lock and interval, settable absolutely and readable
// back (forgejo#22).
//
// A helper reached by an early return from LaunchMessage rather than four more
// links in the else-if chain, which is at MSVC's block-nesting limit -- one
// more `else if` down there is error C1061, not a style question. Same reason
// HandleCanvasIPC exists.
//
// Returns false for anything it does not recognise, so the fallback that hands
// unmatched messages to the script engine is untouched. RAND and LOCK still
// reach ExecuteScriptLine and still toggle; these are the absolute forms a
// client that has to RENDER the state needs, which a toggle cannot provide.
bool Engine::HandlePresetCycleIPC(const wchar_t* sMessage) {
  if (MSG_IS(sMessage, L"SET_PRESET_ORDER=")) {
    // Format: SET_PRESET_ORDER=<0|1>   0 = random, 1 = sequential
    m_bSequentialPresetOrder = (_wtoi(sMessage + 17) != 0);
    Config().SetInt(L"Settings", L"bSequentialPresetOrder", m_bSequentialPresetOrder);
    SendPresetCycleInfo();
    return true;
  }
  if (MSG_IS(sMessage, L"SET_PRESET_LOCK=")) {
    SetUserPresetLock(_wtoi(sMessage + 16) != 0);
    SendPresetCycleInfo();
    return true;
  }
  if (MSG_IS(sMessage, L"SET_TIME_BETWEEN_PRESETS=")) {
    // Seconds between automatic preset changes; 0 disables cycling, as today.
    float secs = (float)_wtof(sMessage + 25);
    if (secs < 0.0f) secs = 0.0f;
    m_fTimeBetweenPresets = secs;
    // -1 is this engine's "recompute on the next UpdateTime" flag, so a lowered
    // interval takes effect now rather than waiting out the deadline the
    // previous value set. Writing an absolute time here would fight UpdateTime
    // for ownership of the same field.
    m_fNextPresetTime = -1.0f;
    Config().SetFloat(L"Settings", L"fTimeBetweenPresets", m_fTimeBetweenPresets);
    SendPresetCycleInfo();
    return true;
  }
  if (MSG_IS(sMessage, L"GET_PRESET_CYCLE")) {
    SendPresetCycleInfo();
    return true;
  }
  return false;
}

// SET_WATERMARK / GET_WATERMARK: the absolute form WATERMARK never had.
//
// ALWAYS_ON_TOP and MIRROR_INDEPENDENT both already had SET_/GET_ pairs
// alongside their toggle -- WATERMARK did not, and neither did
// MIRROR_WATERMARK. Both have one now.
//
// MIRROR_WATERMARK took longer because its toggle is a multi-phase sequence
// -- move the render window to the largest display, go fullscreen, THEN
// activate mirrors a frame later -- that saves and restores several pieces
// of window state as it goes. The careful pass that warning asked for is
// done (#196): the comparison happens inside the window procedure, where
// the transition state lives, instead of in a caller that can only see a
// flag which stays false until the sequence finishes.
//
// Reported live: a running child's window ended up in watermark mode by
// accident (via WM_MW_MIRROR_WM's interaction with an own-process display),
// and there was no reliable way to ask for it on purpose -- DISPLAY|<N>|
// WATERMARK exists and reaches a child, but it can only ever toggle, so a
// client that does not already know the child's current state cannot use it
// to assert one. forgejo#85 comment thread.
void Engine::SetWatermark(bool want) {
  if (m_bWatermarkActive == want) return;
  HWND hw = GetPluginWindow();
  if (hw) PostMessage(hw, WM_MW_WATERMARK, 0, 0);
}

// Deliberately does NOT compare against m_bMirrorWatermarkActive here.
//
// Entering is two phases and that flag is not set until the second, so a
// comparison made on this side would see "off" mid-transition and ask for a
// second enter. WM_MW_MIRROR_WM resolves wParam 2/3 against the transition
// state it owns; this only forwards the request.
void Engine::SetMirrorWatermark(bool want) {
  HWND hw = GetPluginWindow();
  if (hw) PostMessage(hw, WM_MW_MIRROR_WM, want ? 2 : 3, 0);
}

bool Engine::HandleWindowModeIPC(const wchar_t* sMessage) {
  if (MSG_IS(sMessage, L"SET_WATERMARK=")) {
    // Format: SET_WATERMARK=<0|1>
    const bool want = (_wtoi(sMessage + 14) != 0);
    SetWatermark(want);
    // Reports the STATE ASKED FOR, not m_bWatermarkActive read back immediately
    // after posting: WM_MW_WATERMARK runs asynchronously on the window's own
    // thread, so the member would not have moved yet if this thread is not
    // that one. GET_WATERMARK exists for the client that wants to confirm it
    // actually landed.
    extern PipeServer g_pipeServer;
    wchar_t buf[32];
    FormatTo(buf, L"WATERMARK=%d", want ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }
  if (MSG_IS(sMessage, L"SET_MIRROR_WATERMARK=")) {
    // Format: SET_MIRROR_WATERMARK=<0|1>
    //
    // A switch sends "make it so", not "change it". With only a toggle a
    // client had to read, compare and then send -- and anything that moved
    // the state in between left the switch having done the opposite of what
    // was asked. A phone is the client most likely to be a second behind.
    const bool want = (_wtoi(sMessage + 21) != 0);
    SetMirrorWatermark(want);
    // The state ASKED FOR, like SET_WATERMARK= above: the message runs
    // asynchronously on the window's own thread, so the member has not moved
    // yet if this is not that thread. DIAG_MIRRORS' watermark= is the read
    // back, on both the header and every per-monitor record.
    extern PipeServer g_pipeServer;
    wchar_t buf[40];
    FormatTo(buf, L"MIRROR_WATERMARK=%d", want ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }
  if (MSG_IS(sMessage, L"GET_WATERMARK")) {
    extern PipeServer g_pipeServer;
    wchar_t buf[32];
    FormatTo(buf, L"WATERMARK=%d", m_bWatermarkActive ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }

  // ── HUD state, absolute, and the form the parent relays to its children ──
  //
  // ABSOLUTE, never a toggle. The parent sends its own resolved state, so a
  // child that missed an earlier message still converges instead of ending up
  // inverted -- the same reason SET_MIRROR_WATERMARK= exists beside the
  // WATERMARK toggle. `ACTION=ShowFPS` and `ACTION=DebugInfo` remain the
  // toggles a human binds to a key.
  //
  // These exist because a child could not be told any of this. Every one of
  // these four flags was set directly by a UI handler or a hotkey and read
  // from settings.ini only at process start, so a child kept whatever was on
  // disk when it spawned, for its whole life. Unticking "show HUD only on the
  // primary display" therefore did nothing to a running child: the parent's
  // flag went false and the child's stayed true, so it went on drawing no HUD
  // with no way to ask it to stop.
  struct HudFlag { const wchar_t* msg; size_t len; bool Engine::* field; };
  static const HudFlag kHudFlags[] = {
    { L"SET_HUD_PRIMARY_ONLY=", 21, &Engine::m_bDisableMirrorHud },
    { L"SET_SHOW_PRESET_INFO=", 21, &Engine::m_bShowPresetInfo   },
    { L"SET_SHOW_DEBUG=",       15, &Engine::m_bShowDebugInfo    },
    { L"SET_SHOW_FPS=",         13, &Engine::m_bShowFPS          },
  };
  for (const HudFlag& f : kHudFlags) {
    // len COUNTS the '=', so the compare includes it and the value starts one
    // past it. Comparing len-1 would match the verb without its '=' and then
    // read the value FROM the '=', and _wtoi("=1") is 0 -- every one of these
    // would have resolved to false while looking entirely reasonable.
    if (wcsncmp(sMessage, f.msg, f.len) != 0) continue;
    const bool want = (_wtoi(sMessage + f.len) != 0);
    this->*(f.field) = want;
    // Logged because "did the parent's change actually reach this child" is
    // the exact question this whole path exists to answer, and a child cannot
    // be asked from outside: its pipe accepts a connection from anyone but
    // replies only to the parent, so GET_HUD_STATE on a child's pipe times out
    // even though the child is healthy. Its own log/child-<pid>/debug.log is
    // where the answer has to be readable.
    DLOG_INFO("hud: %ls -> %d (child=%d)", f.msg, want ? 1 : 0,
              m_bChildMode ? 1 : 0);
    // A child is a leaf: it has no children of its own to tell, and relaying
    // from here would bounce the message back at the parent.
    if (!m_bChildMode) RelayHudStateToChildren();
    extern PipeServer g_pipeServer;
    std::wstring resp(f.msg, f.len - 1);   // the verb without its '='
    resp += L"=";
    resp += want ? L"1" : L"0";
    g_pipeServer.Send(resp.c_str());
    return true;
  }

  if (MSG_EQ(sMessage, L"GET_HUD_STATE")) {
    extern PipeServer g_pipeServer;
    wchar_t buf[128];
    FormatTo(buf, L"HUD_STATE|primaryOnly=%d|presetInfo=%d|fps=%d|debug=%d"
                  L"|child=%d",
             m_bDisableMirrorHud ? 1 : 0, m_bShowPresetInfo ? 1 : 0,
             m_bShowFPS ? 1 : 0, m_bShowDebugInfo ? 1 : 0,
             m_bChildMode ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }
  return false;
}

bool Engine::HandleInputMixIPC(const wchar_t* sMessage) {
  if (MSG_IS(sMessage, L"GET_INPUT_MIX")) {
    // source: 0 none, 1 Spout, 2 webcam, 3 file -- m_nVideoInputSource is the
    // single authoritative flag; m_bSpoutInputEnabled is kept only for
    // backward compat and is redundant with source != 0.
    //
    // lumaThr/lumaSoft are the stored 0.0-1.0 floats, not the 0-100 integer
    // SET_INPUTMIX_LUMAKEY takes -- the setter's scaling is a wire-format
    // choice, not the value's real unit.
    //
    // webcam names the device SETVIDEODEVICE= selects (empty means "system
    // default"), added alongside the rest rather than as its own GET_ --
    // #29's own point is fewer round trips, not one per field.
    extern PipeServer g_pipeServer;
    wchar_t buf[512];
    FormatTo(buf,
        L"INPUT_MIX=source=%d|onTop=%d|opacity=%.3f|lumaKey=%d|"
        L"lumaThr=%.3f|lumaSoft=%.3f|webcam=%s",
        m_nVideoInputSource, m_bSpoutInputOnTop ? 1 : 0, m_fSpoutInputOpacity,
        m_bSpoutInputLumaKey ? 1 : 0, m_fSpoutInputLumaThreshold,
        m_fSpoutInputLumaSoftness, m_szWebcamDevice);
    g_pipeServer.Send(buf);
    return true;
  }
  // GET_NUMERIC_MODE: the read-back SPRITE_MODE/MESSAGE_MODE never had
  // (#29). Those two signals now write m_nNumericInputMode for real
  // (engine_input.cpp's WM_MW_SPRITE_MODE/WM_MW_MESSAGE_MODE); this is the
  // GET half.
  if (MSG_IS(sMessage, L"GET_NUMERIC_MODE")) {
    extern PipeServer g_pipeServer;
    // SPRITE_KILL (Shift+K, local keyboard only -- no IPC signal reaches it)
    // is a sprite-mode variant, not a third top-level state either of the
    // two IPC signals can select, so it is reported as "sprite" rather than
    // added as a value no setter can produce.
    const wchar_t* mode = (m_nNumericInputMode == NUMERIC_INPUT_MODE_CUST_MSG)
                             ? L"message" : L"sprite";
    wchar_t buf[64];
    FormatTo(buf, L"NUMERIC_MODE=%s", mode);
    g_pipeServer.Send(buf);
    return true;
  }
  return false;
}

bool Engine::HandleCoverStatusIPC(const wchar_t* sMessage) {
  if (MSG_IS(sMessage, L"GET_COVER_STATUS")) {
    // Cover art is always launched as sprite 0 (LaunchSprite / WM_MW_SHOW_COVER
    // both special-case nSpriteNum==0), so a live texmgr slot carrying that
    // identity IS the cover being shown right now -- no separate bool to let
    // drift from the real render state. The slot clears itself when the
    // built-in fade-out finishes (milkdropfs.cpp kills it once its "done" EEL
    // var goes nonzero), so this call needs no polling logic of its own.
    bool bShown = false;
    for (int i = 0; i < NUM_TEX; i++) {
      // IsLive, not pSurface: a cover whose fade has finished is already off
      // the screen but keeps its slot until its fence clears (#225), and
      // reporting it as shown for those frames would make this answer lag the
      // render state it exists to describe.
      if (m_texmgr.IsLive(i) && m_texmgr.m_tex[i].nUserData == 0) {
        bShown = true;
        break;
      }
    }

    // Same source LaunchSprite itself reads for sprite 0: [img00]'s img= key,
    // falling back to the built-in cover.png path when unset. Reported even
    // when nothing is currently showing, since it answers "what would show",
    // matching how the other GET_ readers report configuration, not just state.
    wchar_t img[512] = {};
    ConfigFile(m_szImgIniFile).GetStringTo(L"img00", L"img", L"", img, 511);
    if (img[0] == 0) FormatTo(img, L"%ssprites\\cover.png", m_szMilkdrop2Path);

    extern PipeServer g_pipeServer;
    wchar_t buf[768];
    FormatTo(buf, L"COVER=%d|file=%s", bShown ? 1 : 0, img);
    g_pipeServer.Send(buf);
    return true;
  }
  return false;
}

namespace {

// The parent has never tracked a child's HWND -- it drives them entirely over
// the pipe -- so this is the one place that finds it, by pid. Used by
// GET_CHILDREN's rect=/visible=/minimized=/fs= and by SET_ALL_FULLSCREEN
// (forgejo#96), which needs the same lookup to snapshot and restore
// per-child window state, not just report it.
HWND FindTopWindowForPid(DWORD pid) {
  struct Find { DWORD pid; HWND hwnd; } f = { pid, nullptr };
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    auto* f = (Find*)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid == f->pid && GetWindow(h, GW_OWNER) == nullptr) {
      f->hwnd = h;
      return FALSE;
    }
    return TRUE;
  }, (LPARAM)&f);
  return f.hwnd;
}

}  // namespace

// What only this class knows about its own windows, for GET_WINDOW(S) (#259).
//
// Three roles that look identical from outside -- the primary render window,
// an in-process mirror, and a child's render window -- plus the display each
// one belongs to. A child is in ANOTHER PROCESS, so it cannot be found by an
// enumeration filtered to our pid and has to be contributed here or it is
// simply absent: a three-display setup with two children would otherwise
// report one window and look perfectly correct.
std::vector<mdrop::KnownWindow> Engine::BuildKnownWindows() {
  std::vector<mdrop::KnownWindow> out;

  mdrop::KnownWindow render;
  render.hWnd = GetPluginWindow();
  render.role = L"render";
  {
    wchar_t dev[32] = {};
    if (GetRenderMonitorDevice(dev)) render.device = dev;
  }
  out.push_back(render);

  // The display INDEX is the 1-based position among Monitor outputs, which is
  // what every other display verb means by "display N". Counted the same way
  // GET_DISPLAY_MODE counts it, so the two join.
  {
    std::lock_guard<std::recursive_mutex> lk(m_displayOutputsMutex);
    int idx = 0;
    for (auto& o : m_displayOutputs) {
      if (o.config.type != DisplayOutputType::Monitor) continue;
      ++idx;
      if (o.config.bOwnProcess) continue;   // a child; added below, with its pid
      if (!o.monitorState || !o.monitorState->hWnd) continue;
      mdrop::KnownWindow k;
      k.hWnd = o.monitorState->hWnd;
      k.role = L"mirror";
      k.display = idx;
      k.device = o.config.szDeviceName;
      out.push_back(k);
    }
  }

  {
    std::lock_guard<std::mutex> lk(m_childrenMutex);
    for (const ChildInstance& ci : m_children) {
      if (!ci.pid) continue;
      mdrop::KnownWindow k;
      // May be null: a child's pipe answers well before its window exists, and
      // that state is reported rather than hidden -- "spawned, no window yet"
      // and "not a child at all" are different answers to a caller waiting.
      k.hWnd = FindTopWindowForPid(ci.pid);
      k.role = L"child";
      k.device = ci.szDeviceName;
      k.pid = ci.pid;
      // Resolve the index from the device name rather than storing a second
      // copy of it on ChildInstance, so the two cannot drift apart.
      {
        std::lock_guard<std::recursive_mutex> lkd(m_displayOutputsMutex);
        int idx = 0;
        for (auto& o : m_displayOutputs) {
          if (o.config.type != DisplayOutputType::Monitor) continue;
          ++idx;
          if (_wcsicmp(o.config.szDeviceName, ci.szDeviceName) == 0) {
            k.display = idx;
            break;
          }
        }
      }
      out.push_back(k);
    }
  }
  return out;
}

// Step ONE display's own preset, in this process (#274).
//
// Sequential order steps one either way; random order ignores the direction,
// which is what "previous" means when there is no order to step back through.
//
// Shared, because the two callers had drifted. The IPC verbs were widened to
// the in-process `preset` tier and the Displays window's Prev/Next buttons were
// not, so a display set to "Own preset" was told it mirrored the main window
// and the button did nothing -- reported by Shane against his display 3.
bool Engine::StepDisplayOwnPreset(const DisplayOutputConfig& cfg, bool next,
                                  const wchar_t** reason)
{
  MirrorSurface* ps = MirrorSurfaceFor(cfg.szDeviceName);
  if (!ps) {
    if (reason) *reason = L"no_surface";
    return false;
  }
  const std::wstring dir = ResolveDisplayPresetDir(cfg);
  int idx = ps->seqIndex.load();
  if (cfg.bSequentialOrder && !next)
    idx -= 2;                   // PickDisplayPreset advances by one
  const std::wstring pick = PickDisplayPreset(dir, cfg.bSequentialOrder, idx);
  ps->seqIndex.store(idx);
  if (pick.empty()) {
    if (reason) *reason = L"no_presets";
    return false;
  }
  PostSurfacePreset(*ps, pick);
  return true;
}

std::wstring Engine::DisplayCurrentPresetPath(const DisplayOutputConfig& cfg)
{
  // The two tiers answer from different places, and a caller should not have
  // to know which tier a display is on to ask what it is showing.
  ChildInstance ci;
  if (cfg.bOwnProcess && GetChildInfo(cfg.szDeviceName, ci) &&
      ci.szCurrentPreset[0])
    return ci.szCurrentPreset;
  if (cfg.bOwnPreset) {
    MirrorSurface* ps = MirrorSurfaceFor(cfg.szDeviceName);
    if (ps && !ps->sim.ownPresetPath.empty())
      return ps->sim.ownPresetPath;
  }
  return std::wstring();
}

bool Engine::ShowPresetOnDisplay(const DisplayOutputConfig& cfg,
                                 const std::wstring& path,
                                 const wchar_t** reason)
{
  if (path.empty()) {
    if (reason) *reason = L"empty";
    return false;
  }

  // The render host takes the ordinary global load: for that display, "show
  // this preset" and "load this preset" are the same request.
  wchar_t renderDev[32] = {};
  if (GetRenderMonitorDevice(renderDev) && renderDev[0] &&
      wcscmp(renderDev, cfg.szDeviceName) == 0) {
    // No existence check: this may be called from the render thread, and
    // stat-ing a UNC path would stall the frame loop.
    RenderCommand rc;
    rc.cmd = RenderCmd::LoadPresetPath;
    rc.iParam1 = -1;
    rc.fParam = m_fBlendTimeUser;
    rc.sParam = path;
    EnqueueRenderCmd(rc);
    return true;
  }

  // A display showing the primary's frame has nothing of its own to put a
  // preset into, and silently promoting it would change what the display IS
  // rather than what it shows.
  if (!DisplayHoldsOwnPreset(cfg)) {
    if (reason) *reason = L"not_own_preset";
    return false;
  }

  // A .milk3 is accepted here now (#373): a surface parses one into its own
  // CState via ParseMilk3IntoState, compiles its own Image and Buffer A-D
  // passes, and renders them through RenderMilk3OrientPipeline with its own
  // shaders and PSOs. The interim refusal that lived here -- added when the
  // adopt path could only produce an empty classic state -- is gone with the
  // limitation it described.

  if (ChildOwnsDisplayForIpc(cfg.szDeviceName)) {
    RequestChildSend(cfg.szDeviceName, (L"PRESET=" + path).c_str());
    return true;
  }

  MirrorSurface* ps = MirrorSurfaceFor(cfg.szDeviceName);
  if (!ps) {
    if (reason) *reason = L"no_surface";
    return false;
  }
  // Posted rather than assigned: the worker owns sim.ownPresetPath, and this
  // is whichever thread the caller happens to be on.
  PostSurfacePreset(*ps, path);
  return true;
}

// Per-display render mode and the child registry (forgejo#22).
//
// Another early-return helper for the same reason as HandlePresetCycleIPC:
// LaunchMessage's else-if chain cannot take more links.
bool Engine::HandleDisplayModeIPC(const wchar_t* sMessage) {
  extern PipeServer g_pipeServer;

  if (MSG_IS(sMessage, L"SET_BORDERLESS_FS=")) {
    // Absolute borderless-fullscreen: SET_BORDERLESS_FS=<0|1>.
    //
    // SIGNAL|BORDERLESS_FS toggles, which is useless to any caller that needs a
    // known end state and cannot observe the current one -- the parent placing
    // a child onto a display, or a remote drawing a switch. Lives here rather
    // than in LaunchMessage's else-if chain because that chain has reached
    // MSVC's block nesting limit; these handlers are flat if/return.
    const bool want = (_wtoi(sMessage + 18) != 0);
    HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
    if (hRender)
      PostMessage(hRender, WM_MW_BORDERLESS_FS, want ? MW_FS_ENTER : MW_FS_EXIT, 0);
    wchar_t buf[48];
    FormatTo(buf, L"BORDERLESS_FS=%d", want ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"SET_MINIMIZED=")) {
    // Absolute minimise/restore: SET_MINIMIZED=<0|1>.
    //
    // Added for the Mirror/Children toggle, which clears the other screens
    // without retiring the instances that own them -- a child holds a preset,
    // a cycle timer and a warm shader cache, and killing it to blank a display
    // throws all three away for a keypress usually undone seconds later.
    //
    // Absolute rather than a toggle for the same reason SET_BORDERLESS_FS is:
    // the parent knows the state it wants and cannot see the state the child
    // is in. Restoring re-asserts borderless-fullscreen, because a minimised
    // borderless window comes back as a restored ordinary one.
    const bool want = (_wtoi(sMessage + 14) != 0);
    HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
    if (hRender)
      PostMessage(hRender, WM_MW_SET_MINIMIZED, want ? 1 : 0, 0);
    wchar_t buf[48];
    FormatTo(buf, L"MINIMIZED=%d", want ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"SET_ALL_FULLSCREEN=")) {
    // SET_ALL_FULLSCREEN=<0|1|2> (forgejo#96, forgejo#98): one absolute verb
    // for the walk-away control Shane asked for -- bring every ENABLED
    // display (primary, and every child-owned display with a ready child) to
    // the front, unminimized and fullscreen (=1), put each back exactly as it
    // was found (=0), or leave fullscreen unconditionally regardless of what
    // the snapshot says (=2). The alternative -- a burst of
    // DISPLAY|<N>|SET_MINIMIZED=0 / SET_BORDERLESS_FS= from the client --
    // cannot reach a mirror display at all, half-applies on any single relay
    // failure, and gives the client nothing to restore FROM, which is the
    // whole reason for a verb here rather than another client-side loop.
    //
    // =0 and =2 answer different questions (forgejo#98): =0 is "put it back
    // how it was", right after an automatic or temporary change; =2 is "give
    // me my desktop back", which is what pressing an "All screens showing"
    // control OFF means when the machine was already fullscreen before =1 was
    // ever sent -- =0's snapshot then says primaryWasFullscreen=true, so it
    // re-enters fullscreen instead of leaving it, and a toggle whose off half
    // silently does nothing is not a toggle. =2 does not touch minimize state
    // or mirrors (mirrors have no "fullscreen" of their own to leave, and
    // forcing every display un-minimized was never part of what "leave
    // fullscreen" asks for) and clears any pending snapshot afterward, so a
    // later =1 takes a fresh one rather than restoring to a now-stale
    // "before".
    //
    // Deliberately does NOT touch mirror-mode displays' bEnabled or
    // m_bMirrorsActive: activating mirrors from off goes through
    // m_nDeferMirrorActivate's multi-frame sequencing to avoid creating swap
    // chains the same turn as a fullscreen resize (measured TDR risk,
    // engine_displays.cpp), which is not this verb's to trigger. An already-
    // active mirror set is raised to the front; an inactive one is reported
    // skipped rather than force-activated -- SIGNAL|MIRROR or
    // DISPLAY_PROFILE_LOAD= (forgejo#95) are the deliberate ways to turn
    // mirrors on.
    //
    // Restoring (=0) means restoring to what was ACTUALLY there, not to a
    // saved profile or an assumed default -- exactly the piece #95's profiles
    // cannot cover, since nothing records "what was on screen" until asked.
    // A second =1 while a restore point already exists re-applies without
    // overwriting it, so a repeated tap cannot lose the original "before".
    const int mode = _wtoi(sMessage + 19);   // 0 restore-as-found, 1 enter, 2 force-exit
    const bool want = (mode == 1);
    HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
    int changedPrimary = 0, changedChildren = 0, skippedChildren = 0;
    bool mirrorsRaised = false, mirrorsSkipped = false;
    bool tookSnapshot = false, restored = false, forced = false;

    if (want) {
      if (!m_allFullscreenSnapshot.bActive) {
        m_allFullscreenSnapshot = AllFullscreenSnapshot{};
        m_allFullscreenSnapshot.bActive = true;
        tookSnapshot = true;
        if (hRender) {
          m_allFullscreenSnapshot.primaryWasMinimized = IsIconic(hRender) != FALSE;
          m_allFullscreenSnapshot.primaryWasFullscreen = IsBorderlessFullscreen(hRender);
        }
        for (auto& out : m_displayOutputs) {
          if (out.config.type != DisplayOutputType::Monitor) continue;
          if (!out.config.bEnabled || !out.config.bOwnProcess) continue;
          ChildInstance ci;
          if (!GetChildInfo(out.config.szDeviceName, ci) || !ci.bReady) continue;
          AllFullscreenSnapshot::ChildState cs;
          cs.device = out.config.szDeviceName;
          if (HWND hChild = FindTopWindowForPid(ci.pid)) {
            // STOWED, not merely iconic. A child is hidden rather than
            // minimised now (see WM_MW_SET_MINIMIZED), because minimising a
            // WS_EX_TOOLWINDOW leaves a caption stub on the desktop. Testing
            // only IsIconic would record a hidden child as "was on screen",
            // and the restore below would then send SET_BORDERLESS_FS to a
            // window that is not shown -- putting it back fullscreen but
            // invisible, with nothing to say why.
            cs.wasMinimized = (IsIconic(hChild) != FALSE) ||
                              (IsWindowVisible(hChild) == FALSE);
            cs.wasFullscreen = IsBorderlessFullscreen(hChild);
          }
          m_allFullscreenSnapshot.children.push_back(cs);
        }
      }

      if (hRender) {
        if (IsIconic(hRender)) PostMessage(hRender, WM_MW_SET_MINIMIZED, 0, 0);
        if (!IsBorderlessFullscreen(hRender))
          PostMessage(hRender, WM_MW_BORDERLESS_FS, MW_FS_ENTER, 0);
        PostMessage(hRender, WM_MW_RAISE_WINDOW, 0, 0);
        changedPrimary = 1;
      }

      for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        if (!out.config.bEnabled || !out.config.bOwnProcess) continue;
        ChildInstance ci;
        if (!GetChildInfo(out.config.szDeviceName, ci) || !ci.bReady) {
          skippedChildren++;
          continue;
        }
        RequestChildSend(out.config.szDeviceName, L"SET_MINIMIZED=0");
        RequestChildSend(out.config.szDeviceName, L"SET_BORDERLESS_FS=1");
        RequestChildSend(out.config.szDeviceName, L"RAISE_WINDOW");
        changedChildren++;
      }

      if (m_bMirrorsActive) {
        m_bRaiseMirrorsNextFrame.store(true);
        mirrorsRaised = true;
      } else {
        for (auto& out : m_displayOutputs) {
          if (out.config.type != DisplayOutputType::Monitor) continue;
          if (out.config.bEnabled && !out.config.bOwnProcess) { mirrorsSkipped = true; break; }
        }
      }
    } else if (mode == 2) {
      // Force exit regardless of any snapshot (forgejo#98) -- unconditional,
      // not "restore to not-fullscreen": WM_MW_BORDERLESS_FS/MW_FS_EXIT and
      // child SET_BORDERLESS_FS=0 are both no-ops when already not
      // fullscreen (App.cpp's WM_MW_BORDERLESS_FS handler compares current
      // state before toggling), so there is nothing to check here first.
      forced = true;
      if (hRender) {
        PostMessage(hRender, WM_MW_BORDERLESS_FS, MW_FS_EXIT, 0);
        changedPrimary = 1;
      }
      for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        if (!out.config.bEnabled || !out.config.bOwnProcess) continue;
        ChildInstance ci;
        if (!GetChildInfo(out.config.szDeviceName, ci) || !ci.bReady) {
          skippedChildren++;
          continue;
        }
        RequestChildSend(out.config.szDeviceName, L"SET_BORDERLESS_FS=0");
        changedChildren++;
      }
      // A forced exit invalidates whatever "before" a pending snapshot
      // remembered -- leaving it would let a later =0 restore back INTO
      // fullscreen, undoing the exit this branch exists to do.
      m_allFullscreenSnapshot = AllFullscreenSnapshot{};
    } else if (m_allFullscreenSnapshot.bActive) {
      // WM_MW_SET_MINIMIZED=0 (App.cpp) unconditionally re-enters borderless
      // fullscreen as part of restoring from minimized -- correct for an
      // actual minimize/restore pair, wrong here: sent when the target was
      // NEVER minimized, it silently wins a race against the MW_FS_EXIT
      // posted right after it, undoing the very restore this branch exists
      // to do. Measured live: the primary stayed fullscreen through a
      // restore until this was found. So SET_MINIMIZED is sent only to
      // re-minimize (wasMinimized==true); otherwise fullscreen state is set
      // directly and minimize is left alone.
      restored = true;
      if (hRender) {
        if (m_allFullscreenSnapshot.primaryWasMinimized) {
          PostMessage(hRender, WM_MW_SET_MINIMIZED, 1, 0);
        } else {
          PostMessage(hRender, WM_MW_BORDERLESS_FS,
                     m_allFullscreenSnapshot.primaryWasFullscreen ? MW_FS_ENTER : MW_FS_EXIT, 0);
        }
        changedPrimary = 1;
      }
      for (const auto& cs : m_allFullscreenSnapshot.children) {
        if (cs.wasMinimized) {
          RequestChildSend(cs.device.c_str(), L"SET_MINIMIZED=1");
        } else {
          wchar_t cmd[64];
          FormatTo(cmd, L"SET_BORDERLESS_FS=%d", cs.wasFullscreen ? 1 : 0);
          RequestChildSend(cs.device.c_str(), cmd);
        }
        changedChildren++;
      }
      m_allFullscreenSnapshot = AllFullscreenSnapshot{};
    }
    // mode==0 with no active snapshot: nothing to restore, everything below
    // stays 0/false -- distinguishable on the wire from "restored" by
    // restored=0, per the issue's own point that "nothing happened" and
    // "everything was already right" must not look identical from a phone.

    wchar_t buf[224];
    FormatTo(buf,
               L"ALL_FULLSCREEN=%d|snapshot_new=%d|restored=%d|forced=%d|primary=%d"
               L"|children=%d|children_skipped=%d|mirrors_raised=%d|mirrors_skipped=%d",
               want ? 1 : 0, tookSnapshot ? 1 : 0, restored ? 1 : 0, forced ? 1 : 0, changedPrimary,
               changedChildren, skippedChildren, mirrorsRaised ? 1 : 0, mirrorsSkipped ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"GET_ALL_FULLSCREEN")) {
    // True observation of current window state, not "was SET_ALL_FULLSCREEN=1
    // ever called" -- a display could have been un-fullscreened by hand
    // since, and a stale answer is worse than none (forgejo#96: "a toggle
    // verb from a surface that cannot read state is a coin flip" is the
    // exact reason this is absolute and has a query form at all).
    HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
    const bool primaryUp = hRender && !IsIconic(hRender) && IsBorderlessFullscreen(hRender);

    int childrenTotal = 0, childrenUp = 0;
    bool anyMirrorEnabled = false;
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      if (!out.config.bEnabled) continue;
      if (!out.config.bOwnProcess) { anyMirrorEnabled = true; continue; }
      childrenTotal++;
      ChildInstance ci;
      if (!GetChildInfo(out.config.szDeviceName, ci) || !ci.bReady) continue;
      HWND hChild = FindTopWindowForPid(ci.pid);
      if (hChild && !IsIconic(hChild) && IsBorderlessFullscreen(hChild))
        childrenUp++;
    }
    const bool mirrorsUp = !anyMirrorEnabled || m_bMirrorsActive;
    const bool all = primaryUp && mirrorsUp && (childrenUp == childrenTotal);

    wchar_t buf[192];
    FormatTo(buf,
               L"ALL_FULLSCREEN=%d|primary=%d|mirrors_active=%d|children_up=%d"
               L"|children_total=%d",
               all ? 1 : 0, primaryUp ? 1 : 0, m_bMirrorsActive ? 1 : 0,
               childrenUp, childrenTotal);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"SET_IDLE_ACTIVE=")) {
    // SET_IDLE_ACTIVE=<0|1>: make it look like the idle timer went off, right
    // now -- Shane's own words for what he wanted from a remote. Posts the
    // exact WM_MW_IDLE_ACTIVATE/WM_MW_IDLE_RESTORE messages the real timer
    // posts (App.cpp), so it gets the SAME behavior the timer already has:
    // whichever of the three configured actions (Fullscreen / Stretch-Mirror
    // / Mirror all) m_nIdleAction currently names, the same m_bIdleActivated
    // re-entry guard (a second =1 while already active is a harmless no-op,
    // matching every other absolute verb here), and the same restore that
    // knows what to put back because it is the SAME snapshot/restore code
    // the real timer relies on daily.
    //
    // Independent of SET_IDLE_TIMER=: whether the AUTOMATIC countdown is
    // enabled decides whether idle fires on its own, not whether it can be
    // asked for on demand.
    const bool want = (_wtoi(sMessage + 16) != 0);
    HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
    if (hRender)
      PostMessage(hRender, want ? WM_MW_IDLE_ACTIVATE : WM_MW_IDLE_RESTORE, 0, 0);
    wchar_t buf[48];
    FormatTo(buf, L"IDLE_ACTIVE=%d", want ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"GET_IDLE_ACTIVE")) {
    // m_bIdleActivated itself, not GET_IDLE_TIMER's config -- "is the idle
    // action on screen right now", which is true whether the real timer
    // triggered it or SET_IDLE_ACTIVE=1 did; nothing that follows can tell
    // the two apart, by design.
    wchar_t buf[48];
    FormatTo(buf, L"IDLE_ACTIVE=%d", m_bIdleActivated ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"SET_DISPLAY_MODE=")) {
    // SET_DISPLAY_MODE=<N>,<off|mirror|preset|child>
    // N names the monitor the way the SET_MIRROR_* family already does:
    // \\.\DISPLAY3 is 3.
    const wchar_t* args = sMessage + 17;
    const wchar_t* comma = wcschr(args, L',');
    if (!comma) {
      g_pipeServer.Send(
          L"DISPLAY_MODE=ERROR|expected <N>,<off|mirror|preset|child>");
      return true;
    }
    wchar_t target[32];
    FormatTo(target, L"\\\\.\\DISPLAY%d", _wtoi(args));

    // Trim the mode. It is the raw remainder of the message, and nothing
    // upstream strips anything: PipeServer::DispatchMessage copies verbatim and
    // a TCP client can legitimately deliver a trailing CR/LF.
    std::wstring mode(comma + 1);
    const wchar_t* kWs = L" \t\r\n";
    mode.erase(0, mode.find_first_not_of(kWs));
    const size_t modeEnd = mode.find_last_not_of(kWs);
    mode.erase(modeEnd == std::wstring::npos ? 0 : modeEnd + 1);

    // Validate BEFORE touching any display, and treat "off" as a real value
    // rather than the fallback. It used to be a bare else, so the documented
    // word and every typo took the same branch -- and that branch disabled the
    // display and killed its child. SET_DISPLAY_MODE=2,chidl turned display 2
    // off, killed its instance, and replied DISPLAY_MODE=2,chidl, which reads
    // as success (forgejo#34).
    const bool wantChild  = (_wcsicmp(mode.c_str(), L"child")  == 0);
    const bool wantMirror = (_wcsicmp(mode.c_str(), L"mirror") == 0);
    const bool wantOff    = (_wcsicmp(mode.c_str(), L"off")    == 0);
    // The middle tier: this display's own preset, rendered IN PROCESS. It is
    // what "child" always implied and had to pay a whole instance for.
    // "preset" is the word; "own" is accepted because it is the obvious guess.
    const bool wantPreset = (_wcsicmp(mode.c_str(), L"preset") == 0) ||
                            (_wcsicmp(mode.c_str(), L"own")    == 0);
    if (!wantChild && !wantMirror && !wantOff && !wantPreset) {
      wchar_t err[192];
      FormatTo(err,
                 L"DISPLAY_MODE=ERROR|%d|expected off|mirror|preset|child, "
                 L"got '%.40ls'",
                 _wtoi(args), mode.c_str());
      g_pipeServer.Send(err);
      return true;
    }

    // A child on the render window's own monitor is meaningless by definition:
    // that display already renders a preset, and a child there is actively
    // driven onto it with MOVE_TO_DISPLAY and SIGNAL|BORDERLESS_FS, so a second
    // full instance lands on top of the main window and the two fight over it.
    if (wantChild) {
      wchar_t renderDev[32];
      if (GetRenderMonitorDevice(renderDev) && wcscmp(renderDev, target) == 0) {
        wchar_t err[160];
        FormatTo(err, L"DISPLAY_MODE=ERROR|%d|%ls hosts the render window",
                   _wtoi(args), target);
        g_pipeServer.Send(err);
        return true;
      }
    }

    bool found = false;
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      if (wcscmp(out.config.szDeviceName, target) != 0) continue;
      found = true;

      if (wantChild) {
        out.config.bEnabled = true;
        out.config.bOwnPreset = true;
        out.config.bOwnProcess = true;
        // Mutually exclusive: bIndependentRender is the in-process mirror sim,
        // which renders the PRIMARY's preset. A display cannot be both.
        out.config.bIndependentRender = false;
        RequestChildSpawn(target);
      } else if (wantPreset) {
        // Its own preset, in process. No child: the surface worker holds the
        // preset itself, which is what #186 phase 4 built and what only a
        // child was previously allowed to do.
        out.config.bEnabled = true;
        out.config.bOwnPreset = true;
        out.config.bOwnProcess = false;
        out.config.bIndependentRender = false;
        RequestChildKill(target);
      } else if (wantMirror) {
        out.config.bEnabled = true;
        out.config.bOwnPreset = false;
        out.config.bOwnProcess = false;
        RequestChildKill(target);
      } else {   // wantOff -- now reached only by the word "off"
        out.config.bEnabled = false;
        out.config.bOwnPreset = false;
        out.config.bOwnProcess = false;
        RequestChildKill(target);
      }
      break;
    }

    if (!found) {
      wchar_t err[128];
      FormatTo(err, L"DISPLAY_MODE=ERROR|%d|no monitor %ls", _wtoi(args), target);
      g_pipeServer.Send(err);
      return true;
    }

    SaveDisplayOutputSettings();
    m_bRaiseMirrorsNextFrame.store(true);
    RefreshDisplaysTab();

    // The CANONICAL mode, not the caller's bytes, so the acknowledgement means
    // what it says and " Child " comes back as "child".
    wchar_t ok[96];
    FormatTo(ok, L"DISPLAY_MODE=%d,%ls", _wtoi(args),
               wantChild  ? L"child"
             : wantPreset ? L"preset"
             : wantMirror ? L"mirror" : L"off");
    g_pipeServer.Send(ok);
    return true;
  }

  if (MSG_IS(sMessage, L"SAVE_DISPLAY_PROFILE=")) {
    // SAVE_DISPLAY_PROFILE=<path>[|primary=1][|messaging=1]
    std::wstring arg(sMessage + 21);
    const bool primary   = wcsstr(sMessage, L"|primary=1") != nullptr;
    const bool messaging = wcsstr(sMessage, L"|messaging=1") != nullptr;
    const size_t bar = arg.find(L'|');
    if (bar != std::wstring::npos) arg = arg.substr(0, bar);
    const bool snapshot = wcsstr(sMessage, L"|snapshot=1") != nullptr;
    // forgejo#95: this used to repoint the startup profile at whatever was
    // just saved, unconditionally -- so a client saving a scratch profile
    // silently changed what loads at boot, with no way to ask for that
    // separately and no word of it in the reply. Saving and setting the
    // default are two different acts now; DISPLAY_PROFILE_STARTUP= is the
    // deliberate way to do the second one.
    if (SaveDisplayProfile(arg.c_str(), primary || snapshot, messaging, snapshot)) {
      g_pipeServer.Send((L"PROFILE_SAVED=" + arg).c_str());
    } else {
      g_pipeServer.Send(L"PROFILE_SAVED=ERROR");
    }
    return true;
  }

  if (MSG_IS(sMessage, L"SET_PIPE_NAME=")) {
    // SET_PIPE_NAME=<base>  serves  \\.\pipe\<base>_<pid>
    //
    // The PID is appended by the server and is never the caller's to choose:
    // two instances must not be able to claim one name, and the harness needs
    // to address a specific one.
    //
    // The reply goes out BEFORE the rename, because renaming stops the pipe
    // this arrived on and disconnects whoever sent it. They reconnect to the
    // name the reply just gave them.
    std::wstring base(sMessage + 14);
    while (!base.empty() && iswspace(base.front())) base.erase(base.begin());
    while (!base.empty() && iswspace(base.back()))  base.pop_back();

    // g_pipeServer is already declared in this scope by an enclosing block.
    wchar_t reply[160];
    FormatTo(reply, L"PIPE_NAME=%ls_%lu", base.c_str(),
               (unsigned long)GetCurrentProcessId());
    g_pipeServer.Send(reply);
    Sleep(120);                  // let the reply drain before the pipe closes

    if (!g_pipeServer.Rename(base.c_str()))
      DLOG_WARN("SET_PIPE_NAME refused '%ls' -- letters, digits and underscore "
                "only, under 40 characters", base.c_str());
    else
      DLOG_INFO("pipe renamed: %ls", g_pipeServer.GetPipeName());
    return true;
  }

  if (MSG_IS(sMessage, L"SAVE_DISPLAY_SNAPSHOT")) {
    // No path: the point is that a script -- or a hotkey -- can bank the
    // current arrangement without inventing a name for it.
    const std::wstring path = SaveDisplayProfileSnapshot();
    if (path.empty()) g_pipeServer.Send(L"PROFILE_SAVED=ERROR");
    else              g_pipeServer.Send((L"PROFILE_SAVED=" + path).c_str());
    return true;
  }

  if (MSG_IS(sMessage, L"LIST_DISPLAY_PROFILES")) {
    for (auto& name : ListDisplayProfiles())
      g_pipeServer.Send((L"DISPLAY_PROFILE|" + name).c_str());
    g_pipeServer.Send(L"DISPLAY_PROFILES_END");
    return true;
  }

  if (MSG_IS(sMessage, L"NEXT_DISPLAY_PROFILE") ||
      MSG_IS(sMessage, L"PREV_DISPLAY_PROFILE")) {
    const bool next = MSG_IS(sMessage, L"NEXT_DISPLAY_PROFILE");
    const bool ok = StepDisplayProfile(next ? 1 : -1);
    g_pipeServer.Send(ok ? L"DISPLAY_PROFILE_STEPPED=1"
                         : L"DISPLAY_PROFILE_STEPPED=0|no profiles saved");
    return true;
  }

  if (MSG_IS(sMessage, L"LOAD_DISPLAY_PROFILE=")) {
    // Through the render command queue: this mutates m_displayOutputs, spawns
    // and kills children, and rewrites settings -- none of which may run on the
    // IPC thread while the render thread is walking the same vector.
    RenderCommand rc;
    rc.cmd = RenderCmd::LoadDisplayProfile;
    rc.sParam = ResolveDisplayProfilePath(sMessage + 21);
    EnqueueRenderCmd(rc);
    return true;
  }

  // GET_DISPLAY_MODE[=<N>] -- which tier each display is on.
  //
  // Reports the same words SET_DISPLAY_MODE accepts, plus auto=1 when the
  // display is a child because the cost gate promoted it rather than because
  // the user asked. Without that the two are indistinguishable from outside,
  // which is exactly the confusion that had processes appearing unexplained.
  //
  // Before GET_DISPLAY_MODE, and before GET_DISPLAY_PROFILE: MSG_IS is a
  // prefix compare and all three begin "GET_DISPLAY_", so the longer verbs
  // have to be tested first.
  if (MSG_IS(sMessage, L"GET_DISPLAY_MODE")) {
    const wchar_t* eq = wcschr(sMessage, L'=');
    const int want = eq ? _wtoi(eq + 1) : -1;
    std::wstring resp = L"DISPLAY_MODE";
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
    int idx = 0;
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      ++idx;
      if (want >= 0 && idx != want) continue;
      const wchar_t* mode =
          !out.config.bEnabled                          ? L"off"
        : out.config.bOwnProcess                        ? L"child"
        : out.config.bOwnPreset                         ? L"preset"
                                                        : L"mirror";
      resp += L"|" + std::to_wstring(idx) + L"=" + mode +
              L",device=" + out.config.szDeviceName +
              // Always 0 since #261 removed the cost gate -- nothing promotes a
              // display any more, so every child is the user's. The field stays
              // rather than vanishing, because a client parsing it into a dict
              // and reading auto= would fault on its absence; it is scheduled
              // to go when the Remote no longer looks for it.
              L",auto=0" +
              L",independent=" + (out.config.bIndependentRender ? L"1" : L"0");
    }
    resp += L"|END";
    g_pipeServer.Send(resp.c_str());
    return true;
  }

  // GET_DISPLAY_WATERMARK[=<N>] -- one display's watermark state, or all of
  // them. The setter has existed for a while with nothing to read it back
  // with, so the only way to confirm a display's state was DIAG_MIRRORS, whose
  // watermark= echoed a global and said the same thing for every monitor.
  //
  // Checked before GET_DISPLAY_PROFILE because MSG_IS is a PREFIX compare and
  // both begin "GET_DISPLAY_"; the longer, more specific verb has to win.
  if (MSG_IS(sMessage, L"GET_DISPLAY_WATERMARK")) {
    const wchar_t* eq = wcschr(sMessage, L'=');
    const int want = eq ? _wtoi(eq + 1) : -1;
    std::wstring resp = L"DISPLAY_WATERMARK";
    std::lock_guard<std::recursive_mutex> lkDisp(m_displayOutputsMutex);
    int idx = 0;
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      ++idx;
      if (want >= 0 && idx != want) continue;
      resp += L"|" + std::to_wstring(idx) + L"=" +
              (out.config.bWatermark ? L"1" : L"0") +
              L",device=" + out.config.szDeviceName +
              L",ownProcess=" + (out.config.bOwnProcess ? L"1" : L"0");
    }
    resp += L"|END";
    g_pipeServer.Send(resp.c_str());
    return true;
  }

  if (MSG_IS(sMessage, L"GET_DISPLAY_PROFILE")) {
    g_pipeServer.Send((std::wstring(L"DISPLAY_PROFILE=") +
                       m_szStartupDisplayProfile).c_str());
    return true;
  }

  if (MSG_IS(sMessage, L"GET_CHILDREN")) {
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      if (!out.config.bOwnProcess) continue;
      ChildInstance ci;
      const bool have = GetChildInfo(out.config.szDeviceName, ci);

      // std::wstring, not a fixed buffer. This carried three MAX_PATH-capable
      // fields in wchar_t[MAX_PATH + 256]; dir= and startup= are two more, and
      // swprintf_s does NOT truncate on overflow -- it invokes the invalid
      // parameter handler, which in a release CRT with none installed ends the
      // process. A read-only query must not be able to do that.
      std::wstring line = L"CHILD|display=";
      line += out.config.szDeviceName;
      line += L"|pid=" + std::to_wstring(have ? (unsigned long)ci.pid : 0UL);
      line += L"|preset=";
      line += (have ? ci.szCurrentPreset : L"");

      // The rate this display is ACTUALLY rendering at (#246). Empty until the
      // child has answered once, so "not known yet" stays distinct from a
      // genuine 0 -- a child that has stopped rendering and one that has not
      // been asked yet are different answers.
      //
      // This is the only way to see it. DIAG_MIRRORS reports per-surface
      // frame times for IN-PROCESS surfaces only, so a display moved to its
      // own process vanished from every frame-rate figure the app could
      // produce -- which is precisely the arrangement a mirrors-vs-children
      // comparison exists to judge, leaving it able to report the primary and
      // nothing else.
      line += L"|fps=";
      if (have && ci.fFps >= 0.0f) {
        wchar_t f[24];
        FormatTo(f, L"%.1f", ci.fFps);
        line += f;
      }

      // The EFFECTIVE interval, so a child cycling happily at the parent's rate
      // no longer reports interval=-1.000 -- the sentinel meaning "inherit" was
      // being printed raw. interval_raw keeps "inherited" distinguishable from
      // "explicitly set to the same number".
      wchar_t num[64];
      FormatTo(num, L"%.3f", EffectiveChildInterval(out.config));
      line += L"|interval="; line += num;
      FormatTo(num, L"%.3f", out.config.fTimeBetweenPresets);
      line += L"|interval_raw="; line += num;

      line += L"|order=" + std::to_wstring(out.config.bSequentialOrder ? 1 : 0);
      // lock is the EFFECTIVE state, lock_raw keeps "inherited" tellable from
      // "pinned to the same value the main window happens to have".
      line += L"|lock=" + std::to_wstring(EffectiveChildPresetLock(out.config) ? 1 : 0);
      line += L"|lock_raw=" + std::to_wstring(out.config.nPresetLock);
      line += L"|dir=";     line += out.config.szPresetDir;
      line += L"|startup="; line += out.config.szStartupPreset;
      // state=absent used to mean three different things -- "a child is
      // coming", "the flag is set and nothing will act on it" and "we tried and
      // could not" -- so no client could choose between a spinner, a Start
      // button and an error. MDR_Android showed "Starting its own instance..."
      // forever (forgejo#57). The three are separate now:
      //
      //   pending   a spawn is queued, or the worker is acting on one
      //   starting  the process exists, its pipe has not answered yet
      //   ready     it is up and has answered
      //   failed    the last attempt failed; reason= says how
      //   absent    requested, nothing in flight, nothing tried -- ask again
      //
      // reason= is present on every line and empty unless state=failed, so a
      // client parses one shape.
      std::wstring reason;
      const bool failedSpawn = ChildSpawnFailure(out.config.szDeviceName, reason);
      line += L"|state=";
      if (have)
        line += (ci.bFailed ? L"failed" : ci.bReady ? L"ready" : L"starting");
      else if (ChildSpawnPending(out.config.szDeviceName))
        line += L"pending";
      else if (failedSpawn)
        line += L"failed";
      else
        line += L"absent";
      line += L"|reason=";
      if (failedSpawn && (!have || ci.bFailed)) line += reason;
      else if (have && ci.bFailed)             line += L"exited";

      // Where the child's window ACTUALLY is, found from its pid.
      //
      // The parent has never tracked a child's HWND -- it drives them entirely
      // over the pipe -- so before this there was no way to ask where one
      // landed, and "did the child assigned to display N end up on display N"
      // could only be answered by covering that display and looking at it. That
      // is the whole reason the display suites were intrusive.
      //
      // Empty for a child with no window yet, and for one built -hidewindow
      // that has none to find. visible= separates "no window" from "a window
      // deliberately not shown", which are different answers to different
      // questions.
      line += L"|rect=";
      if (have && ci.pid) {
        HWND hChild = FindTopWindowForPid(ci.pid);
        if (hChild) {
          RECT wr{};
          GetWindowRect(hChild, &wr);
          line += std::to_wstring(wr.left) + L"," + std::to_wstring(wr.top) + L"," +
                  std::to_wstring(wr.right - wr.left) + L"," +
                  std::to_wstring(wr.bottom - wr.top);
          line += L"|visible=";
          line += IsWindowVisible(hChild) ? L"1" : L"0";
          // IsWindowVisible checks only WS_VISIBLE, which a MINIMIZED window
          // still has -- so it cannot tell "shown" from "minimized" on its
          // own, and both fields are reported for that reason.
          //
          // The Mirror toggle no longer stands children down this way: it
          // RETIRES them, so a display it cleared has no child row at all
          // rather than a minimized one. SET_MINIMIZED= is still reachable
          // directly, and for a child it now HIDES rather than minimises --
          // so a stowed child reads visible=0, minimized=0, which is the
          // honest pair.
          line += L"|minimized=";
          line += IsIconic(hChild) ? L"1" : L"0";
          // fs=: the same test SET_ALL_FULLSCREEN's snapshot uses (forgejo#96),
          // so a client can already tell "fullscreen" from "windowed" per
          // child without waiting for that verb to exist.
          line += L"|fs=";
          line += IsBorderlessFullscreen(hChild) ? L"1" : L"0";
        } else {
          line += L"|visible=|minimized=|fs=";
        }
      } else {
        line += L"|visible=|minimized=|fs=";
      }
      g_pipeServer.Send(line.c_str());
    }
    g_pipeServer.Send(L"CHILDREN_END");
    return true;
  }

  if (MSG_IS(sMessage, L"SET_INJECT_EFFECT=")) {
    // 0 off, 1 brighten, 2 darken, 3 solarize, 4 invert -- the F11 cycle, but
    // absolute. F11 only steps, so nothing outside the app could put the
    // post-process back to off, and a session left on brighten shifted every
    // later luminance reading. test_canvas_metric asserts a flat 0.5 grey reads
    // ~0.5, and could not establish that precondition at all (forgejo#30): the
    // failure said "flat 0.5 grey should read ~0.5, got 0.71" and blamed the
    // metric under test for a leftover effect mode.
    int mode = _wtoi(sMessage + 18);
    if (mode < 0 || mode > 4) {
      g_pipeServer.Send(L"INJECT_EFFECT_ERR=expected 0 to 4");
      return true;
    }
    m_nInjectEffectMode = mode;
    Config().SetInt(L"Settings", L"nInjectEffectMode", m_nInjectEffectMode);
    wchar_t buf[48];
    FormatTo(buf, L"INJECT_EFFECT=%d", m_nInjectEffectMode);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"DIAG_UI_MODE")) {
    // Which full-frame overlay is up, and where the settings cursor sits.
    //
    // GET_PRESET_STATUS already reports the mode as `ui=<n>`, and that is not
    // duplicated here for its own sake. Two things it does not give:
    //
    //   * the NAME. `ui=` is the raw enum, and those numbers shift the moment
    //     a mode is added to the middle of ui_mode -- so a test asserting 15
    //     would afterwards pass or fail for a reason having nothing to do with
    //     what it is checking. The static_assert below keeps the names honest.
    //   * m_nSettingsCurSel, which nothing reported at all. Without it the
    //     settings overlay's UP/DOWN handling cannot be checked except by
    //     photographing the frame and reading the highlight.
    //
    // The second is the one that mattered: the overlay could not be ENTERED
    // either until #163, so its whole input path had never once been
    // exercised, and a way in without a way to look would only have moved the
    // problem.
    static const wchar_t* const kModeNames[] = {
      L"regular", L"menu", L"load", L"load_del", L"load_rename", L"saveas",
      L"save_overwrite", L"edit_menu_string", L"changedir", L"import_wave",
      L"export_wave", L"import_shape", L"export_shape",
      L"upgrade_pixel_shader", L"mashup", L"settings"
    };
    // Catches the drift at compile time rather than by printing "?" at runtime.
    // Fires if a mode is added anywhere, including after UI_SETTINGS.
    static_assert(_countof(kModeNames) == UI_SETTINGS + 1,
                  "kModeNames must name every ui_mode -- add the new one");
    const int m = (int)m_UI_mode;
    wchar_t buf[128];
    FormatTo(buf, L"UI_MODE|mode=%d|name=%s|sel=%d", m,
             (m >= 0 && m < (int)_countof(kModeNames)) ? kModeNames[m] : L"?",
             m_nSettingsCurSel);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"DIAG_GLOBAL_PRESET_CYCLE")) {
    // What the shutdown save WOULD write, not what the session is doing.
    //
    // A display profile's main-render settings apply for the session only and
    // must never become the saved globals. There is no way to check that from
    // outside without quitting the app and reading settings.ini, which no test
    // can do to a running instance -- so the value the save would use is
    // exposed here instead. See PrimaryOverride in engine.h.
    wchar_t buf[64];
    FormatTo(buf, L"DIAG_GLOBAL_PRESET_CYCLE=%.3f", GlobalTimeBetweenPresets());
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"DIAG_STARTUP_PRESET_LOCK")) {
    // The PREFERENCE -- "start the next run locked" -- not the live lock,
    // which GET_PRESET_CYCLE already reports. The two are separate members
    // that merely begin equal, and every way of toggling the lock at runtime
    // must leave this one alone.
    //
    // Exposed for the same reason as DIAG_GLOBAL_PRESET_CYCLE above: the only
    // other way to see what the shutdown save would write is to quit and read
    // settings.ini, which no test can do to a running instance. Two separate
    // paths had promoted the live lock into this preference -- the shutdown
    // save itself, and SET_DISPLAY_LOCK aimed at the primary -- so a session
    // that unlocked once came back unlocked forever after.
    wchar_t buf[64];
    FormatTo(buf, L"DIAG_STARTUP_PRESET_LOCK=%d",
               m_bPresetLockOnAtStartup ? 1 : 0);
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"CHILD_TEST_WINDOWS=")) {
    // How a child's window comes up: auto | off | tiny | hidden.
    //
    // "auto" follows testing mode, which is the default and the reason this
    // needs no setting up in the common case -- a suite that turns testing mode
    // on stops blacking out monitors without having to ask for anything.
    const wchar_t* v = sMessage + 19;
    int mode;
    if      (_wcsicmp(v, L"auto")   == 0) mode = kChildWindowAuto;
    else if (_wcsicmp(v, L"off")    == 0) mode = kChildWindowNormal;
    else if (_wcsicmp(v, L"normal") == 0) mode = kChildWindowNormal;
    else if (_wcsicmp(v, L"tiny")   == 0) mode = kChildWindowTiny;
    else if (_wcsicmp(v, L"hidden") == 0) mode = kChildWindowHidden;
    else {
      g_pipeServer.Send(L"CHILD_TEST_WINDOWS_ERR=expected auto, off, tiny or hidden");
      return true;
    }
    m_nChildTestWindows = mode;
    // Existing children are NOT restyled. The window is built from the command
    // line and cannot be rebuilt in place, so a mode set now applies to the
    // next spawn -- said in the reply rather than left to be discovered by a
    // test that changed the mode and saw nothing move.
    wchar_t buf[96];
    FormatTo(buf, L"CHILD_TEST_WINDOWS=%ls|effective=%d|applies=next_spawn",
               v, EffectiveChildWindowMode());
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"SET_DISPLAY_CHILD_RECT=")) {
    // SET_DISPLAY_CHILD_RECT=<N>,<x>,<y>,<w>,<h> -- where display N's child
    // window goes in tiny mode. w or h of 0 clears it, so the corner default
    // comes back.
    //
    // This is what makes placement testable at all: put a child at a chosen
    // spot, read GET_CHILDREN's rect= back, and check it landed there and
    // inside display N. Never persisted; see DisplayOutputConfig::rcTestWindow.
    int n = 0, x = 0, y = 0, w = 0, h = 0;
    if (swscanf_s(sMessage + 23, L"%d,%d,%d,%d,%d", &n, &x, &y, &w, &h) < 1 || n <= 0) {
      g_pipeServer.Send(L"DISPLAY_CHILD_RECT_ERR=expected <N>,<x>,<y>,<w>,<h>");
      return true;
    }
    wchar_t want[32];
    FormatTo(want, L"\\\\.\\DISPLAY%d", n);
    for (auto& o : m_displayOutputs) {
      if (o.config.type != DisplayOutputType::Monitor) continue;
      if (wcscmp(o.config.szDeviceName, want) != 0) continue;
      if (w > 0 && h > 0) {
        o.config.rcTestWindow.left   = x;
        o.config.rcTestWindow.top    = y;
        o.config.rcTestWindow.right  = x + w;
        o.config.rcTestWindow.bottom = y + h;
      } else {
        o.config.rcTestWindow = RECT{};
      }
      RECT eff{};
      TestChildRect(want, eff);
      wchar_t buf[160];
      FormatTo(buf, L"DISPLAY_CHILD_RECT=%d|rect=%d,%d,%d,%d|applies=next_spawn",
                 n, eff.left, eff.top, eff.right - eff.left, eff.bottom - eff.top);
      g_pipeServer.Send(buf);
      return true;
    }
    g_pipeServer.Send(L"DISPLAY_CHILD_RECT_ERR=no such display");
    return true;
  }

  if (MSG_IS(sMessage, L"SET_DISPLAY_ENABLED=")) {
    // SET_DISPLAY_ENABLED=<N>,<0|1> -- the IPC form of the Displays window's
    // "Enabled" checkbox, added alongside the fix for the checkbox itself
    // (reported live: "unchecking 'enabled'... doesn't disable or stop a
    // child"). Not folded into the shared kVerbs table below: that table's
    // precondition refuses anything that is not the primary or an
    // own-process child, and Enabled is exactly as meaningful for a plain
    // mirror or Spout output -- the gate that is correct for ORDER/LOCK/
    // WATERMARK would be wrong here.
    const wchar_t* args = sMessage + 20;   // wcslen(L"SET_DISPLAY_ENABLED=")
    const wchar_t* comma = wcschr(args, L',');
    if (!comma) {
      g_pipeServer.Send(L"DISPLAY_ENABLED_ERR=expected <N>,<0|1>");
      return true;
    }
    const int n = _wtoi(args);
    const bool want = (_wtoi(comma + 1) != 0);
    wchar_t target[32];
    FormatTo(target, L"\\\\.\\DISPLAY%d", n);
    wchar_t renderDev[32];
    if (GetRenderMonitorDevice(renderDev) && wcscmp(renderDev, target) == 0) {
      g_pipeServer.Send(L"DISPLAY_ENABLED_ERR=that display hosts the render window");
      return true;
    }
    for (auto& o : m_displayOutputs) {
      if (wcscmp(o.config.szDeviceName, target) != 0) continue;
      SetDisplayEnabled(o, want);
      SaveDisplayOutputSettings();
      RefreshDisplaysTab();
      wchar_t buf[64];
      FormatTo(buf, L"DISPLAY_ENABLED=%d,%d", n, want ? 1 : 0);
      g_pipeServer.Send(buf);
      return true;
    }
    g_pipeServer.Send(L"DISPLAY_ENABLED_ERR=no such display");
    return true;
  }

  if (MSG_IS(sMessage, L"DIAG_CHILDREN")) {
    // Worker health, not child state -- GET_CHILDREN already answers the
    // latter. This exists because forgejo#57 was a child worker that had been
    // stopped by a device recovery and could not be seen from outside: every
    // command was accepted, queued and silently never acted on, and the app
    // reported no children because there genuinely were none.
    //
    // worker=0, or a queued= that never falls, is that failure in one line.
    std::wstring line = L"CHILDREN_DIAG|worker=";
    line += ChildWorkerAlive() ? L"1" : L"0";
    line += L"|event=";
    line += m_hChildCmdEvent ? L"1" : L"0";
    line += L"|job=";
    line += m_hChildJob ? L"1" : L"0";
    line += L"|queued=" + std::to_wstring((unsigned long long)ChildQueueDepth());
    line += L"|children=" + std::to_wstring((long long)GetChildCount());
    line += L"|pending=";
    {
      std::lock_guard<std::mutex> lock(m_childCmdMutex);
      bool first = true;
      for (auto& d : m_childSpawnPending) {
        if (!first) line += L",";
        line += d;
        first = false;
      }
      line += L"|failed=";
      first = true;
      for (auto& f : m_childSpawnFailed) {
        if (!first) line += L",";
        line += f.first + L"=" + f.second;
        first = false;
      }
    }
    g_pipeServer.Send(line.c_str());
    return true;
  }

  return false;
}

// ─── Per-display preset control (forgejo#32) ─────────────────────────────────
//
// A display running as its own child process could be created and destroyed but
// never driven: no addressed form of PRESET=, of the cycle settings, or of
// next/prev existed, so GET_CHILDREN reported an interval that nothing could
// set. The surface below is the other half of that round trip.
//
// The split between what is validated-and-persisted and what is relayed is not
// a matter of taste. A -child pins the config write shield for its whole life
// (App.cpp), so a child persists NOTHING. That makes the four child fields in
// DisplayOutputConfig the complete set of per-display state that can be
// durable, and everything else -- which preset is up this second, history
// position -- has nowhere to live even if we wanted it to. So:
//
//   parent-owned, validated, written to settings.ini, replayed on every spawn:
//       SET_DISPLAY_PRESET_DIR / _STARTUP_PRESET / _ORDER / _TIME_BETWEEN
//   child-owned, transient, relayed and forgotten:
//       SET_DISPLAY_PRESET (browse), DISPLAY_NEXT, DISPLAY_PREV, DISPLAY|N|...
//
// "Survives a restart of the child" then falls out for free: ChildConfigureNow
// replaying the config on spawn already IS the restore path.
//
// The primary is addressable by everything except child mode -- a remote drawing
// one row per monitor gets working buttons on every row -- and an addressed verb
// aimed at it applies to THIS instance without broadcasting, which is why
// NextPresetLocal / PrevPresetLocal exist.

namespace {

// These BUILD the reply; the member function below sends it. g_pipeServer is a
// global, and this file lives in namespace mdrop without including App.h, so a
// declaration at namespace scope here would declare a new mdrop::g_pipeServer
// and one in the anonymous namespace would get internal linkage -- both fail to
// link. Every other sender in this file uses a block-scope extern inside an
// Engine member, and so does HandleDisplayPresetIPC.

// <VERB>=<N>|ok=0|err=<stable token>|detail=<prose>
//
// The display number is on every reply INCLUDING the failures. SET_DISPLAY_MODE
// used to answer "DISPLAY_MODE=ERROR|expected ..." with no N at all, so a remote
// with two displays in flight could not tell which one had failed. err= is what
// a client switches on; detail= is what it shows a human.
std::wstring ReplyErr(const wchar_t* verb, int n, const wchar_t* code,
                      const wchar_t* detail) {
  std::wstring r = verb;
  r += L"=" + std::to_wstring(n);
  r += L"|ok=0|err="; r += code;
  r += L"|detail=";   r += detail;
  return r;
}

std::wstring ReplyOk(const wchar_t* verb, int n, const wchar_t* what,
                     const wchar_t* value) {
  std::wstring r = verb;
  r += L"=" + std::to_wstring(n);
  r += L"|ok=1|applied="; r += what;
  if (value && *value) { r += L"|value="; r += value; }
  return r;
}

// Trim in place. Every one of these arguments is the tail of a pipe message and
// a TCP client can deliver a trailing CR/LF that nothing upstream strips --
// forgejo#34 was exactly this, one command over.
std::wstring TrimWs(const wchar_t* p) {
  std::wstring s(p ? p : L"");
  const wchar_t* kWs = L" \t\r\n";
  s.erase(0, s.find_first_not_of(kWs));
  const size_t e = s.find_last_not_of(kWs);
  s.erase(e == std::wstring::npos ? 0 : e + 1);
  return s;
}

}  // namespace

// IDLE_TIMER=<enabled>|<timeoutMinutes>|<action>|<profile>|<fullOpacity>
//
// The last two fields were APPENDED rather than inserted, so every existing
// reader keeps working: they split on '|' and take the first three, and a
// client that has never heard of an idle profile simply ignores what follows.
void Engine::SendIdleTimerReply() {
  extern PipeServer g_pipeServer;
  wchar_t buf[MAX_PATH + 96];
  FormatTo(buf, L"IDLE_TIMER=%d|%d|%d|%s|%d",
           m_bIdleTimerEnabled ? 1 : 0, m_nIdleTimeoutMinutes, m_nIdleAction,
           m_szIdleProfile, m_bIdleFullOpacity ? 1 : 0);
  g_pipeServer.Send(buf);
}

bool Engine::HandleDisplayPresetIPC(const wchar_t* sMessage) {
  extern PipeServer g_pipeServer;
  #define DisplayReplyErr(...) g_pipeServer.Send(ReplyErr(__VA_ARGS__).c_str())
  #define DisplayReplyOk(...)  g_pipeServer.Send(ReplyOk(__VA_ARGS__).c_str())

  // ── the relay ───────────────────────────────────────────────────────────
  //
  // DISPLAY|<N>|<command>, forwarded verbatim to that display's child, which is
  // the same binary and already speaks the whole vocabulary. One verb buys the
  // present surface and every future one.
  //
  // EVERY path below returns true once the prefix matches. That is not tidiness
  // -- LaunchMessage's final else hands an unmatched message to
  // ExecuteScriptLine, which SPLITS ON '|' and runs each token as a script
  // command, and NEXT, PREV, LOCK, RAND, FULLSCREEN, MIRROR and WATERMARK are
  // all live there. So a DISPLAY| request that fell through would drive the
  // PARENT: "DISPLAY|9|NEXT" would advance the primary's preset and
  // "DISPLAY|9|FULLSCREEN" would toggle its fullscreen. Returning false here is
  // a destructive action, not a no-op.
  if (MSG_IS(sMessage, L"DISPLAY|")) {
    const wchar_t* rest = sMessage + 8;
    const wchar_t* bar = wcschr(rest, L'|');
    if (!bar) {
      DisplayReplyErr(L"DISPLAY_RELAY", 0, L"malformed",
                      L"expected DISPLAY|<N>|<command>");
      return true;
    }
    const int n = _wtoi(rest);
    const std::wstring inner = TrimWs(bar + 1);
    if (inner.empty()) {
      DisplayReplyErr(L"DISPLAY_RELAY", n, L"malformed", L"empty command");
      return true;
    }

    // Denied verbs. Deliberately short: a child owns no external interfaces, so
    // most of what could be relayed is already inert there. These are the ones
    // that would either take the child somewhere the parent could not follow,
    // or recurse.
    static const wchar_t* kDenied[] = {
      L"SHUTDOWN", L"TESTING_MODE", L"SET_DISPLAY_MODE", L"DISPLAY|",
      L"SAVE_DISPLAY_PROFILE", L"LOAD_DISPLAY_PROFILE", L"GET_CHILDREN",
      L"SET_DISPLAY_PRESET_DIR", L"SET_DISPLAY_STARTUP_PRESET",
      L"SET_DISPLAY_ORDER", L"SET_DISPLAY_TIME_BETWEEN", L"SET_DISPLAY_LOCK",
      L"SET_DISPLAY_WATERMARK", L"SET_DISPLAY_ENABLED",
    };
    for (const wchar_t* d : kDenied) {
      const size_t dl = wcslen(d);
      if (_wcsnicmp(inner.c_str(), d, dl) == 0) {
        DisplayReplyErr(L"DISPLAY_RELAY", n, L"denied", d);
        return true;
      }
    }

    wchar_t target[32];
    FormatTo(target, L"\\\\.\\DISPLAY%d", n);
    ChildInstance ci;
    if (!GetChildInfo(target, ci) || !ci.bReady) {
      DisplayReplyErr(L"DISPLAY_RELAY", n, L"not_a_child",
                      L"no ready child on this display; "
                      L"SET_DISPLAY_MODE=<N>,child gives it one");
      return true;
    }
    RequestChildSend(target, inner.c_str());
    DisplayReplyOk(L"DISPLAY_RELAY", n, L"relayed", inner.c_str());
    return true;
  }

  // ── the addressed verbs ─────────────────────────────────────────────────
  struct Verb { const wchar_t* msg; const wchar_t* reply; size_t len; };
  static const Verb kVerbs[] = {
    { L"SET_DISPLAY_PRESET_DIR=",      L"DISPLAY_PRESET_DIR",      23 },
    { L"SET_DISPLAY_STARTUP_PRESET=",  L"DISPLAY_STARTUP_PRESET",  27 },
    { L"SET_DISPLAY_TIME_BETWEEN=",    L"DISPLAY_TIME_BETWEEN",    25 },
    { L"SET_DISPLAY_ORDER=",           L"DISPLAY_ORDER",           18 },
    { L"SET_DISPLAY_LOCK=",            L"DISPLAY_LOCK",            17 },
    { L"SET_DISPLAY_WATERMARK=",       L"DISPLAY_WATERMARK",       22 },
    // Longest-first, as the comment on this table requires: this must precede
    // SET_DISPLAY_MSG= were one ever added, and it is longer than the entries
    // it sits among, so it is safe here (#255).
    { L"SET_DISPLAY_MSGMAX=",          L"DISPLAY_MSGMAX",          19 },
    { L"SET_DISPLAY_PRESET=",          L"DISPLAY_PRESET",          19 },
    { L"DISPLAY_NEXT=",                L"DISPLAY_NEXT",            13 },
    { L"DISPLAY_PREV=",                L"DISPLAY_PREV",            13 },
  };
  const Verb* v = nullptr;
  for (const Verb& cand : kVerbs) {
    if (_wcsnicmp(sMessage, cand.msg, cand.len) == 0) { v = &cand; break; }
  }
  if (!v) return false;

  // SET_DISPLAY_PRESET_DIR= must not be matched by SET_DISPLAY_PRESET=. The
  // table is ordered longest-first for exactly that reason; assert it stays so.
  const wchar_t* args = sMessage + v->len;
  const wchar_t* comma = wcschr(args, L',');
  const int n = _wtoi(args);
  const bool needsValue = (v->msg[0] == L'S');   // the SET_ forms take a value
  if (needsValue && !comma) {
    DisplayReplyErr(v->reply, n, L"malformed", L"expected <N>,<value>");
    return true;
  }
  const std::wstring value = needsValue ? TrimWs(comma + 1) : std::wstring();

  wchar_t target[32];
  FormatTo(target, L"\\\\.\\DISPLAY%d", n);

  // Is this the display the render window is on? Then the verb applies to THIS
  // instance -- the primary is a first-class target for everything except child
  // mode, so a remote's per-display row works on every row.
  wchar_t renderDev[32];
  const bool isPrimary = GetRenderMonitorDevice(renderDev) &&
                         wcscmp(renderDev, target) == 0;

  DisplayOutput* out = nullptr;
  for (auto& o : m_displayOutputs) {
    if (o.config.type != DisplayOutputType::Monitor) continue;
    if (wcscmp(o.config.szDeviceName, target) != 0) continue;
    out = &o;
    break;
  }
  if (!out) {
    DisplayReplyErr(v->reply, n, L"no_monitor", target);
    return true;
  }

  // A display that is neither the primary nor a child has nowhere to put this.
  //
  // #186 phase 1 restores this refusal for a MIRROR. It was relaxed in 9d6b783c
  // on the reasoning that #184 and #186 had given the mirror sim a preset of its
  // own, so the setting would no longer be dead config. The machinery part was
  // right; the placement was not. A mirror is defined by rendering the primary's
  // preset, so a per-display preset stored against one is not merely dead -- if
  // it were honoured it would stop the mirror being a mirror.
  //
  // Per-screen presets belong to the own-preset path, and phase 4 puts them
  // there. At that point this guard widens again -- to displays that hold their
  // own preset, which is a different test from bIndependentRender.
  // Widened, as that paragraph said it would be: the test is whether the
  // display HOLDS ITS OWN PRESET, not whether it runs in its own process. With
  // the middle tier those are different questions, and asking the old one told
  // a display in `preset` mode -- which has a preset of its own and a surface
  // to step it on -- that it was a mirror.
  if (!isPrimary && !DisplayHoldsOwnPreset(out->config)) {
    DisplayReplyErr(v->reply, n, L"not_own_preset",
                    L"display follows the primary; SET_DISPLAY_MODE=<N>,preset "
                    L"gives it its own preset in this process, or ,child in its "
                    L"own");
    return true;
  }

  // ── transient: browse, next, prev ───────────────────────────────────────
  if (_wcsicmp(v->reply, L"DISPLAY_PRESET") == 0) {
    if (value.empty()) {
      DisplayReplyErr(v->reply, n, L"malformed", L"empty path");
      return true;
    }
    // ShowPresetOnDisplay carries the three-way this arm used to spell out:
    // the primary takes a render command, a child takes a pipe message (#236 --
    // reaching for a surface here answered no_surface for exactly the displays
    // that DO have a preset of their own), and an in-process display takes a
    // post to its surface (#186 phase 4).
    //
    // Shared with the Displays window's file dialog (#364) rather than written
    // out twice. Prev/Next is the standing warning: the IPC path was widened to
    // the in-process tier and the button was not, so the button spent a release
    // telling a display with its own preset that it was a mirror.
    const wchar_t* why = nullptr;
    if (!ShowPresetOnDisplay(out->config, value, &why)) {
      const wchar_t* detail = L"";
      if (wcscmp(why, L"no_surface") == 0)
        detail = L"display has no render surface yet; enable it first";
      else if (wcscmp(why, L"milk3_needs_primary") == 0)
        detail = L"a .milk3 can only play on the main render window; a display "
                 L"renders its own preset through the classic pipeline (#373)";
      DisplayReplyErr(v->reply, n, why, detail);
      return true;
    }
    DisplayReplyOk(v->reply, n,
                   isPrimary ? L"primary"
                             : (ChildOwnsDisplayForIpc(target) ? L"child"
                                                               : L"display"),
                   value.c_str());
    return true;
  }

  if (_wcsicmp(v->reply, L"DISPLAY_NEXT") == 0 ||
      _wcsicmp(v->reply, L"DISPLAY_PREV") == 0) {
    const bool next = (_wcsicmp(v->reply, L"DISPLAY_NEXT") == 0);
    if (isPrimary) {
      // The LOCAL forms. NextPreset/PrevPreset broadcast to every child, which
      // is right for a global Next and exactly wrong for an addressed one.
      RenderCommand rc;
      rc.cmd = next ? RenderCmd::NextPresetLocal : RenderCmd::PrevPresetLocal;
      rc.fParam = m_fBlendTimeUser;
      EnqueueRenderCmd(rc);
    } else if (ChildOwnsDisplayForIpc(target)) {
      // The child owns this display's preset and steps it itself. Same hole as
      // SET_DISPLAY_PRESET above: the guard that lets a child reach this arm
      // sent it to a branch that could only serve an in-process surface, so
      // MDR_Android's Previous/Next were dead on precisely the displays
      // running their own instance (#236).
      //
      // SIGNAL|NEXT_PRESET rather than a resolved path: the child owns its own
      // directory, order and sequence position, and picking a file here would
      // step a sequence the child is not keeping.
      RequestChildSend(target, next ? L"SIGNAL|NEXT_PRESET"
                                    : L"SIGNAL|PREV_PRESET");
      DisplayReplyOk(v->reply, n, L"child", L"");
      return true;
    } else {
      const wchar_t* why = nullptr;
      if (!StepDisplayOwnPreset(out->config, next, &why)) {
        if (wcscmp(why, L"no_surface") == 0)
          DisplayReplyErr(v->reply, n, why,
                          L"display has no render surface yet; enable it first");
        else
          DisplayReplyErr(v->reply, n, why,
                          ResolveDisplayPresetDir(out->config).c_str());
        return true;
      }
    }
    DisplayReplyOk(v->reply, n, isPrimary ? L"primary" : L"display", L"");
    return true;
  }

  // ── persisted: the four parent-owned fields ─────────────────────────────
  if (_wcsicmp(v->reply, L"DISPLAY_PRESET_DIR") == 0) {
    if (isPrimary) {
      DisplayReplyErr(v->reply, n, L"primary_uses_global",
                      L"use SET_DIR= for the main instance");
      return true;
    }
    wcsncpy_s(out->config.szPresetDir, value.c_str(), _TRUNCATE);
    SaveDisplayOutputSettings();
    RequestChildSend(target, (std::wstring(L"SET_DIR=") + value).c_str());
    DisplayReplyOk(v->reply, n, L"child", out->config.szPresetDir);
    return true;
  }

  if (_wcsicmp(v->reply, L"DISPLAY_STARTUP_PRESET") == 0) {
    if (isPrimary) {
      // The main render's PROFILE pin, which is a different thing from the
      // global startup preset and used to be refused here as though it were
      // the same one (#284).
      //
      // m_primaryProfile is what the Displays window's "Main render" group
      // writes and what the next saved profile carries for the main window.
      // It was reachable from that window and from nowhere else -- so a remote
      // could pin a startup preset for every display except the biggest one,
      // and no test could reach the field at all.
      //
      // Deliberately NOT SaveDisplayOutputSettings: this is staging for the
      // next profile save, held in memory exactly as the window holds it, and
      // writing it to settings.ini would make it a second global competing
      // with szPresetStartup.
      wcsncpy_s(m_primaryProfile.szStartupPreset, value.c_str(), _TRUNCATE);
      DisplayReplyOk(v->reply, n, L"primary", m_primaryProfile.szStartupPreset);
      return true;
    }
    // Pin, as opposed to browse. SET_DISPLAY_PRESET shows a preset now and is
    // forgotten; this decides what the display comes back to. Keeping them
    // apart means a remote flicking through presets does not rewrite
    // settings.ini per step, nor silently redefine "startup preset" as "the
    // last thing somebody browsed to".
    wcsncpy_s(out->config.szStartupPreset, value.c_str(), _TRUNCATE);
    SaveDisplayOutputSettings();
    DisplayReplyOk(v->reply, n, L"child", out->config.szStartupPreset);
    return true;
  }

  if (_wcsicmp(v->reply, L"DISPLAY_MSGMAX") == 0) {
    // SET_DISPLAY_MSGMAX=<N>,<n> -- how many messages this display shows at
    // once; -1 inherits the global Messages setting (#255).
    //
    // Per display because a display running its own preset runs its own
    // message loop, and the supertext slots are ONE shared pool: without a
    // per-display limit a busy panel takes every slot and the others fall
    // silent. Aimed at the primary it is the global setting, so a remote
    // drawing one row per monitor gets a working control on every row -- the
    // same shape DISPLAY_LOCK follows below.
    const int want = _wtoi(value.c_str());
    if (want < -1 || want > NUM_SUPERTEXTS) {
      DisplayReplyErr(v->reply, n, L"out_of_range",
                      L"expected -1 to inherit, or 0..NUM_SUPERTEXTS");
      return true;
    }
    if (isPrimary) {
      if (want < 0) {
        DisplayReplyErr(v->reply, n, L"primary_cannot_inherit",
                        L"the main window is what the others inherit from");
        return true;
      }
      m_nMsgMaxOnScreen = want;
      SaveSettingToINI(SET_SPRITES_MESSAGES);
    } else {
      out->config.nMsgMaxOnScreen = want;
      SaveDisplayOutputSettings();
      RefreshDisplaysTab();
    }
    wchar_t val[16];
    FormatTo(val, L"%d", want);
    DisplayReplyOk(v->reply, n, isPrimary ? L"primary" : L"display", val);
    return true;
  }

  if (_wcsicmp(v->reply, L"DISPLAY_LOCK") == 0) {
    // -1 inherit the main window's lock, 0 unlocked, 1 locked.
    //
    // The lock is global: a child follows the main window unless this display
    // has pinned a value, which is what makes -1 meaningfully different from 0.
    // Aimed at the primary it is just SET_PRESET_LOCK, so a remote drawing one
    // row per monitor gets a working control on every row.
    const int want = _wtoi(value.c_str());
    if (want < -1 || want > 1) {
      DisplayReplyErr(v->reply, n, L"out_of_range",
                      L"expected -1 to inherit, 0 unlocked, or 1 locked");
      return true;
    }
    if (isPrimary) {
      if (want < 0) {
        DisplayReplyErr(v->reply, n, L"primary_cannot_inherit",
                        L"the main window is what the others inherit from");
        return true;
      }
      // Exactly SET_PRESET_LOCK, as the comment above says -- the live lock
      // and nothing else. This used to also write bPresetLockOnAtStartup,
      // which is the STARTUP PREFERENCE and a different thing: it decides how
      // the next run begins, not what the lock is now. A remote with a lock
      // control on the main row therefore rewrote the user's startup setting
      // every time the lock was toggled from it, while the hotkey, the
      // controller and SET_PRESET_LOCK -- the same gesture by other routes --
      // left it alone.
      // Through the shared setter, so this verb, the Displays window's
      // main-render row and the Presets window are one implementation. Three
      // copies of "set the main render's lock" is three chances to disagree,
      // and they did.
      SetMainRenderLock(want != 0);
      SendPresetCycleInfo();
    } else {
      out->config.nPresetLock = want;
      SaveDisplayOutputSettings();
      // Push the RESOLVED value: going back to inherit must move the child to
      // whatever the main window currently is, not leave it on its last pin.
      RequestChildSend(target, EffectiveChildPresetLock(out->config)
                                   ? L"SET_PRESET_LOCK=1" : L"SET_PRESET_LOCK=0");
    }
    wchar_t num[16];
    FormatTo(num, L"%d", want);
    DisplayReplyOk(v->reply, n, isPrimary ? L"primary" : L"display", num);
    return true;
  }

  if (_wcsicmp(v->reply, L"DISPLAY_WATERMARK") == 0) {
    // Format: SET_DISPLAY_WATERMARK=<N>,<0|1>.
    //
    // Watermark is per-PROCESS, not per-mirror-surface, which is why this
    // shares the primary-or-child gate above rather than the comma-addressed
    // shape SET_MIRROR_INDEPENDENT uses: a mirror display renders in-process
    // and has no window of its own to watermark, but a child is a whole
    // separate instance that already understands SET_WATERMARK= -- the same
    // relay SET_DISPLAY_ORDER uses for SET_PRESET_ORDER=. forgejo#85 comment
    // thread: reported live as a display that could only be put into
    // watermark mode by accident, with no reliable way to ask for it.
    const bool want = (_wtoi(value.c_str()) != 0);
    if (isPrimary) {
      SetWatermark(want);
    } else {
      // Per display now, for an in-process mirror as well as a child. The
      // comment above used to say a mirror "has no window of its own to
      // watermark" and refuse; it does have one -- ApplyMirrorWindowStyles
      // styles it every time m_bMirrorStylesDirty is raised -- what was
      // missing was somewhere to record the intent, which is
      // DisplayOutputConfig::bWatermark.
      out->config.bWatermark = want;
      m_bMirrorStylesDirty.store(true);
      SaveDisplayOutputSettings();
      // A child owns its window outright, so the style pass cannot reach it.
      if (out->config.bOwnProcess)
        RequestChildSend(target, want ? L"SET_WATERMARK=1" : L"SET_WATERMARK=0");
      RefreshDisplaysTab();
    }
    DisplayReplyOk(v->reply, n, isPrimary ? L"primary" : L"display",
                   want ? L"1" : L"0");
    return true;
  }

  if (_wcsicmp(v->reply, L"DISPLAY_ORDER") == 0) {
    const bool seq = (_wtoi(value.c_str()) != 0);
    if (isPrimary) {
      SetMainRenderOrder(seq);
    } else {
      out->config.bSequentialOrder = seq;
      SaveDisplayOutputSettings();
      RequestChildSend(target, seq ? L"SET_PRESET_ORDER=1"
                                   : L"SET_PRESET_ORDER=0");
    }
    DisplayReplyOk(v->reply, n, isPrimary ? L"primary" : L"display",
                   seq ? L"1" : L"0");
    return true;
  }

  if (_wcsicmp(v->reply, L"DISPLAY_TIME_BETWEEN") == 0) {
    const float secs = (float)_wtof(value.c_str());
    if (secs < -1.0f) {
      DisplayReplyErr(v->reply, n, L"out_of_range",
                      L"seconds must be >= 0, or -1 to inherit");
      return true;
    }
    if (isPrimary) {
      // One implementation, shared with the Displays window's main-render row
      // and the Presets window. Three copies of "set the main render's cycle"
      // is three chances for them to disagree, which is the bug this came from.
      SetMainRenderInterval(max(0.0f, secs));
    } else {
      out->config.fTimeBetweenPresets = secs;
      SaveDisplayOutputSettings();
      wchar_t msg[64];
      FormatTo(msg, L"SET_TIME_BETWEEN_PRESETS=%.3f",
                 EffectiveChildInterval(out->config));
      RequestChildSend(target, msg);
    }
    wchar_t shown[32];
    FormatTo(shown, L"%.3f", isPrimary ? m_fTimeBetweenPresets : secs);
    DisplayReplyOk(v->reply, n, isPrimary ? L"primary" : L"display", shown);
    return true;
  }

  return false;
  #undef DisplayReplyErr
  #undef DisplayReplyOk
}

bool Engine::HandleCanvasIPC(const wchar_t* sMessage) {
    if (MSG_IS(sMessage, L"SET_CANVAS_MAX=")) {
      // Global feedback-canvas ceiling, long edge in px. 0 = no limit.
      extern PipeServer g_pipeServer;
      m_nGlobalCanvasMax = max(0, _wtoi(sMessage + 15));
      MyWriteConfig();
      // MUST go through the render command queue: IPC handlers run on the message
      // pump thread, and rebuilding DX12 resources there while the render thread
      // is mid-frame is a crash (render_commands.h).
      EnqueueRenderCmd(RenderCmd::ReallocCanvas);
      wchar_t out[64];
      FormatTo(out, L"CANVAS_MAX=%d", m_nGlobalCanvasMax);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"SET_PRESET_CANVAS_MAX=")) {
      // Per-preset limit for the CURRENT preset. Capped by the global ceiling at
      // resolve time (EffectiveCanvasLimit), so a value above it has no effect.
      extern PipeServer g_pipeServer;
      const int px = max(0, _wtoi(sMessage + 22));
      const wchar_t* fn = CurrentPresetLeaf();
      // Same write path the Presets context menu and Annotations combo use, so
      // the three surfaces cannot diverge and these IPC tests cover all of them.
      SetPresetCanvasMaxByFile(fn, px);
      wchar_t out[64];
      FormatTo(out, L"PRESET_CANVAS_MAX=%d", px);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"SET_PRESET_FPS_MAX=")) {
      // Per-preset frame-rate ceiling for the CURRENT preset (#222). It only
      // ever LOWERS the active cap -- EnforceMaxFPS takes the min -- so a value
      // above the user's own cap has no effect, and 0 clears it.
      extern PipeServer g_pipeServer;
      const int fps = max(0, _wtoi(sMessage + 19));
      SetPresetFpsMaxByFile(CurrentPresetLeaf(), fps);
      wchar_t out[64];
      FormatTo(out, L"PRESET_FPS_MAX=%d", fps);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"PRESET_FPS_MAX_CLEAR")) {
      extern PipeServer g_pipeServer;
      SetPresetFpsMaxByFile(CurrentPresetLeaf(), 0);
      g_pipeServer.Send(L"PRESET_FPS_MAX=cleared");
    }
    else if (MSG_IS(sMessage, L"GET_PRESET_FPS_MAX")) {
      extern PipeServer g_pipeServer;
      wchar_t out[64];
      FormatTo(out, L"PRESET_FPS_MAX=%d|active=%d", m_nPresetFpsCap, m_max_fps_w);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"PRESET_CANVAS_MAX_CLEAR")) {
      extern PipeServer g_pipeServer;
      const wchar_t* fn = CurrentPresetLeaf();
      SetPresetCanvasMaxByFile(fn, 0);   // 0 clears the flag, never stores a zero
      g_pipeServer.Send(L"PRESET_CANVAS_MAX=cleared");
    }
    else if (MSG_IS(sMessage, L"SET_PRESET_DAMP=")) {
      // Feedback damp strength for the CURRENT preset, 0..1. 0 clears it.
      // The second of the two runaway mitigations; unlike SET_PRESET_CANVAS_MAX
      // this one does not resize anything, so it can be swept live at 45s a
      // step without a canvas rebuild between measurements.
      extern PipeServer g_pipeServer;
      const float s = (float)_wtof(sMessage + 16);
      const wchar_t* fn = CurrentPresetLeaf();
      SetPresetFeedbackDampByFile(fn, s);
      wchar_t out[96];
      FormatTo(out, L"PRESET_DAMP=%.4f|applied=%.6f", s, EffectiveFeedbackDamp());
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"SET_DAMP_OVERRIDE=")) {
      // A raw per-frame feedback multiplier for THIS SESSION: 1.0 is off,
      // smaller bites harder, anything negative (or "clear") removes it.
      // Nothing is written to presets.json and nothing survives a restart.
      //
      // Not the same knob as SET_PRESET_DAMP. That one takes a strength on a
      // dial and lets the canvas decide the multiplier; this one IS the
      // multiplier, so a measurement can walk past the ends of the dial and
      // find out what a preset actually needs before the dial's range is
      // chosen.
      extern PipeServer g_pipeServer;
      const wchar_t* arg = sMessage + 18;
      m_fDampOverride = (_wcsicmp(arg, L"clear") == 0) ? -1.0f : (float)_wtof(arg);
      wchar_t out[96];
      if (m_fDampOverride < 0.0f)
        FormatTo(out, L"DAMP_OVERRIDE=off|applied=%.6f", EffectiveFeedbackDamp());
      else
        FormatTo(out, L"DAMP_OVERRIDE=%.6f|applied=%.6f",
                   m_fDampOverride, EffectiveFeedbackDamp());
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"PRESET_DAMP_CLEAR")) {
      extern PipeServer g_pipeServer;
      const wchar_t* fn = CurrentPresetLeaf();
      SetPresetFeedbackDampByFile(fn, 0.0f);
      g_pipeServer.Send(L"PRESET_DAMP=cleared");
    }
    else if (MSG_IS(sMessage, L"GET_CANVAS_MAX")) {
      // The whole canvas picture in one reply: the global ceiling, what the
      // canvas actually came out as, and what THIS preset asks for -- a script
      // deciding whether a mitigation is needed should not have to correlate
      // three commands to find out.
      extern PipeServer g_pipeServer;
      const wchar_t* fn = CurrentPresetLeaf();
      // Copied out under the guard; the pipe send below blocks, and blocking
      // IPC inside a with() parks every ToolWindow thread (#11).
      int presetMax = 0;
      float presetDamp = 0.0f;
      uint32_t presetFlags = 0;
      WithAnnotation(fn, /*create=*/false, [&](PresetAnnotation& a) {
        if (a.hasCanvasMax)    presetMax  = a.canvasMax;
        if (a.hasFeedbackDamp) presetDamp = a.feedbackDamp;
        presetFlags = a.flags;
      });
      wchar_t out[384];
      FormatTo(out,
                 L"CANVAS_MAX|global=%d|texW=%d|texH=%d|applied=%d"
                 L"|presetMax=%d|damp=%.4f|dampApplied=%.6f|dampOverride=%.4f"
                 L"|compInverts=%d|flagged=%d|preset=%.120ls",
                 m_nGlobalCanvasMax, m_nTexSizeX, m_nTexSizeY,
                 m_nCanvasLimitApplied,
                 presetMax,
                 presetDamp,
                 EffectiveFeedbackDamp(),
                 m_fDampOverride,
                 CompShaderInvertsFeedbackNow() ? 1 : 0,
                 (presetFlags & PFLAG_CANVAS) ? 1 : 0,
                 fn);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"SET_PRESET_FLAG=")) {
      // Set or clear a flag on the CURRENT preset.
      //   SET_PRESET_FLAG=canvas,1
      // Names match what presets.json stores, so what you type is what you see
      // in the file. Explicit, so it goes through even in testing mode -- the
      // gates exist to stop the app deciding on its own, not to stop the user.
      extern PipeServer g_pipeServer;
      const wchar_t* arg = sMessage + 16;
      const wchar_t* comma = wcschr(arg, L',');
      std::wstring name(arg, comma ? comma : arg + wcslen(arg));
      const bool set = comma ? (_wtoi(comma + 1) != 0) : true;

      uint32_t bit = 0;
      if (name == L"favorite")    bit = PFLAG_FAVORITE;
      else if (name == L"error")  bit = PFLAG_ERROR;
      else if (name == L"skip")   bit = PFLAG_SKIP;
      else if (name == L"broken") bit = PFLAG_BROKEN;
      else if (name == L"canvas") bit = PFLAG_CANVAS;

      wchar_t out[256];
      if (!bit) {
        FormatTo(out, L"PRESET_FLAG=ERROR|unknown flag '%.40ls' "
                        L"(favorite|error|skip|broken|canvas)", name.c_str());
      } else if (!m_szCurrentPresetFile[0]) {
        FormatTo(out, L"PRESET_FLAG=ERROR|no preset loaded");
      } else {
        const wchar_t* fn = CurrentPresetLeaf();
        SetPresetFlag(fn, bit, set);
        uint32_t flags = 0;
        WithAnnotation(fn, /*create=*/false,
                       [&](PresetAnnotation& a) { flags = a.flags; });
        FormatTo(out, L"PRESET_FLAG=OK|%.40ls=%d|flags=0x%02X|preset=%.120ls",
                   name.c_str(), set ? 1 : 0, flags, fn);
      }
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"DIAG_ANNOT_RESOLVE=")) {
      // Where does this annotation's preset actually live? Empty path = gone.
      extern PipeServer g_pipeServer;
      const wchar_t* fn = sMessage + 19;
      // ResolveAnnotationPath does PathFileExistsW calls, so it runs on a COPY
      // with the guard released rather than inside the callback (#11).
      PresetAnnotation copy;
      const bool found = WithAnnotation(fn, /*create=*/false,
                                        [&](PresetAnnotation& a) { copy = a; });
      const std::wstring path = found ? ResolveAnnotationPath(copy) : std::wstring();
      wchar_t out[1024];
      FormatTo(out, L"ANNOT_RESOLVE|found=%d|path=%.700ls",
                 found ? 1 : 0, path.c_str());
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"DIAG_ANNOT_MISSING")) {
      extern PipeServer g_pipeServer;
      int missing = 0, total = 0;
      m_annot.with([&](AnnotStore& s) {
        for (auto& kv : s.byKey)
          if (IsAnnotationKnownMissing(kv.second)) missing++;
        total = (int)s.byKey.size();
      });
      wchar_t out[128];
      FormatTo(out, L"ANNOT_MISSING|missing=%d|total=%d", missing, total);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"DIAG_ANNOT_IGNORED=")) {
      // Would a preset at this path be allowed into presets.json?
      //
      // Exercises the SAME predicate the annotation writer uses, so the tests
      // cover the shipped gate rather than a copy of its logic. `testing`
      // is reported separately because it makes every path ignored, and a
      // test that could not tell the two apart would pass for the wrong reason.
      extern PipeServer g_pipeServer;
      const wchar_t* path = sMessage + 19;
      wchar_t out[1024];
      FormatTo(out, L"ANNOT_IGNORED|ignored=%d|dirmatch=%d|testing=%d|path=%.700ls",
                 ShouldSkipAnnotationWrite(path) ? 1 : 0,
                 IsAnnotationIgnoredPath(path) ? 1 : 0,
                 m_bTestingMode ? 1 : 0,
                 path);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"DIAG_ANNOT_REPAIR")) {
      // Remove recorded alias paths that hash to a different preset than the
      // entry holding them -- the damage left by TickPresetUsage having paired
      // a filename with the wrong hash. Explicit, because it edits
      // presets.json; nothing runs it on its own.
      extern PipeServer g_pipeServer;
      int checked = 0, unjudged = 0;
      const int removed = RepairAnnotationPaths(&checked, &unjudged);
      wchar_t out[160];
      FormatTo(out, L"DIAG_ANNOT_REPAIR|removed=%d|checked=%d|unjudged=%d",
                 removed, checked, unjudged);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"DIAG_ANNOT_DUPES")) {
      // Hash every preset under a root and report how the groups fall out.
      // Format: DIAG_ANNOT_DUPES=<root>, or DIAG_ANNOT_DUPES for the current
      // preset directory.  Synchronous: an IPC handler already runs off the
      // render thread, and a test wants the answer, not a progress feed.
      extern PipeServer g_pipeServer;
      const wchar_t* eq = wcschr(sMessage, L'=');
      const std::wstring root = (eq && eq[1]) ? std::wstring(eq + 1)
                                              : std::wstring(m_szPresetDir);
      std::set<std::wstring> hashes;
      const auto groups = ScanForDuplicatePresets(root.c_str(), nullptr, &hashes);
      int redundant = 0;
      for (const auto& g : groups) redundant += (int)g.files.size() - 1;
      wchar_t out[1024];
      FormatTo(out,
                 L"ANNOT_DUPES|groups=%d|redundant=%d|hashes=%d|root=%.700ls",
                 (int)groups.size(), redundant, (int)hashes.size(), root.c_str());
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"ANNOT_REMOVE_MISSING")) {
      extern PipeServer g_pipeServer;
      const int n = RemoveMissingAnnotations();
      const int total = m_annot.with([](AnnotStore& s) { return (int)s.byKey.size(); });
      wchar_t out[96];
      FormatTo(out, L"ANNOT_REMOVED|count=%d|total=%d", n, total);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"DIAG_ANNOT_QUERY=")) {
      // Exercises the same predicate the Annotations search box uses, so the
      // tests cover the real matcher rather than a copy of its logic.
      extern PipeServer g_pipeServer;
      const wchar_t* pattern = sMessage + 17;
      int count = 0, total = 0;
      m_annot.with([&](AnnotStore& s) {
        for (auto& kv : s.byKey)
          if (AnnotationMatches(kv.second, pattern)) count++;
        total = (int)s.byKey.size();
      });
      wchar_t out[512];
      FormatTo(out, L"ANNOT_QUERY|pattern=%.200ls|count=%d|total=%d",
                 pattern, count, total);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"DIAG_CANVAS_METRIC")) {
      // Phase 1 instrumentation. Mean luminance of an 8x8 reduction of the
      // presented frame; variance is reported but is NOT a discriminator (it
      // overlaps between healthy and runaway -- see the design doc).
      extern PipeServer g_pipeServer;
      const CanvasSample s = m_canvasMetric.Latest();
      wchar_t out[160];
      FormatTo(out, L"CANVAS_METRIC|valid=%d|mean=%.6f|var=%.6f",
                 s.valid ? 1 : 0, s.mean, s.variance);
      g_pipeServer.Send(out);
    }
    else if (MSG_IS(sMessage, L"SET_CANVAS_TRACE=")) {
      // Phase 1 trace collection to log/diag_canvas_trace.csv. Off by default.
      extern PipeServer g_pipeServer;
      const bool on = (_wtoi(sMessage + 17) != 0);
      m_canvasMetric.SetTraceEnabled(on);
      wchar_t out[48];
      FormatTo(out, L"CANVAS_TRACE=%d", on ? 1 : 0);
      g_pipeServer.Send(out);
    }
  else {
    return false;
  }
  return true;
}

// Says what an audio-profile request actually did.
//
// Three outcomes worth telling apart, because they need different fixes:
// the profile does not exist, the preset cannot carry one, or it worked.
void Engine::AnswerAudioProfile(const wchar_t* name, bool ok, bool known)
{
  extern PipeServer g_pipeServer;
  const wchar_t* shown = (name && *name) ? name : L"(default)";
  wchar_t buf[384];
  if (ok) {
    FormatTo(buf, L"AUDIO_PROFILE=%s|ok=1|resolved=%s",
             shown, m_resolvedAudioProfile.c_str());
  } else {
    const wchar_t* why =
        !known ? L"no such profile"
      : IsScratchPreset(m_szCurrentPresetFile)
            ? L"scratch preset: testing mode plus an AnnotationIgnoreDirs folder"
            : L"no annotation could be created for the current preset";
    FormatTo(buf, L"AUDIO_PROFILE=%s|ok=0|reason=%s", shown, why);
  }
  g_pipeServer.Send(buf);
}

void Engine::LaunchMessage(wchar_t* sMessage, bool bStrict) {
  // forgejo#21 -- HELP / COMMANDS / GET_VERSION, the self-describing index.
  // First, and an early return, for the same reason RESTART_DEVICE below is
  // one: the else-if chain further down is at MSVC's block-nesting limit.
  //
  // ipc_help::Handle returns false for everything else, so the fallback at the
  // end of this function -- which hands unmatched messages to the script
  // engine for Milkwave Remote -- is untouched.
  {
    std::vector<std::wstring> help;
    if (ipc_help::Handle(sMessage, m_bChildMode, help)) {
      extern PipeServer g_pipeServer;
      for (const std::wstring& line : help) g_pipeServer.Send(line);
      g_pipeServer.Send(L"END_BATCH");
      return;
    }
  }

  if (HandleCanvasIPC(sMessage)) return;

  if (HandlePresetCycleIPC(sMessage)) return;

  if (HandleVSyncIPC(sMessage)) return;

  if (HandleWindowModeIPC(sMessage)) return;

  if (HandleInputMixIPC(sMessage)) return;
  if (HandleCoverStatusIPC(sMessage)) return;
  if (HandleDisplayProfileIPC(sMessage)) return;

  if (HandleDisplayModeIPC(sMessage)) return;
  if (HandleDisplayPresetIPC(sMessage)) return;
  if (HandlePresetBrowseIPC(sMessage)) return;
  if (HandleHotkeyDiagIPC(sMessage)) return;
  if (HandleWindowDiagIPC(sMessage, m_szBaseDir, BuildKnownWindows())) return;

  // Warp mesh dimensions. An early return rather than another link in the
  // else-if chain below, which is already at MSVC's block-nesting limit -- one
  // more `else if` down there is error C1061, not a style question.
  //
  // Goes through the render command queue rather than assigning here:
  // m_nGridX/m_nGridY both SIZE the m_verts allocation and INDEX it, so only
  // the render thread may write them (forgejo#8). Clamping happens in the
  // handler, next to the code that does the indexing.
  // Recreate the D3D12 device, the same path a TDR takes. Exposed over IPC
  // because it previously had no trigger except a button in the Visual
  // ToolWindow, which meant device recovery -- the thing that has to work when
  // everything else has already gone wrong -- could not be exercised by the
  // harness at all.
  if (MSG_IS(sMessage, L"RESTART_DEVICE")) {
    EnqueueRenderCmd(RenderCmd::DeviceRecovery);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"RESTART_DEVICE=queued");
    return;
  }

  if (MSG_IS(sMessage, L"SET_MESH_SIZE=")) {
    // SET_MESH_SIZE=<x>        -> x by x*3/4, the historical 4:3 derivation
    // SET_MESH_SIZE=<x>,<y>    -> exactly x by y
    //
    // The optional Y exists because the 4:3 derivation is itself under
    // question (#188): MilkDrop 3 uses a SQUARE 64x64 mesh (plugin.cpp
    // 1256-1257, its only assignment), so every preset here samples its warp
    // on a differently-shaped grid than the engine it is written for, and
    // .milk2 blend cutouts inherit that distortion. Measuring the difference
    // needs a square mesh at runtime; forcing one by changing the derivation
    // would change how every preset renders in order to run the experiment.
    const int gridX = _wtoi(sMessage + 14);
    const wchar_t* comma = wcschr(sMessage + 14, L',');
    const int gridY = comma ? _wtoi(comma + 1) : (gridX * 3 / 4);
    m_reqGridX.store(gridX);
    m_reqGridY.store(gridY);
    EnqueueRenderCmd(RenderCmd::SetMeshSize);
    return;
  }
  // Route SIGNAL| commands through pipe server dispatch (PostMessage to render window)
  if (MSG_IS(sMessage, L"SIGNAL|")) {
    extern PipeServer g_pipeServer;
    g_pipeServer.DispatchSignal(sMessage + 7);
    return;
  }

  // Audio mixer. Checked BEFORE the else-if chain below rather than inside it:
  // that chain already sits at MSVC's block-nesting limit, and one more link
  // fails to compile with C1061. MixerHandleAndReply returns false for
  // anything it does not own, so an unrecognised keyword still reaches the
  // chain and, past it, the script engine.
  if (MixerHandleAndReply(sMessage)) return;

  if (MSG_IS(sMessage, L"MSG|")) {

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
      CopyTo(m_supertexts[nextFreeSupertextIndex].szTextW, ConvertToLPCWSTR(params[L"text"]));
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
      CopyTo(m_supertexts[nextFreeSupertextIndex].nFontFace, ConvertToLPCWSTR(params[L"font"]));
    }
    else if (!hasProfile) {
      CopyTo(m_supertexts[nextFreeSupertextIndex].nFontFace, L"Segoe UI");
    }

    if (params.find(L"size") != params.end()) {
      m_supertexts[nextFreeSupertextIndex].fFontSize = std::stof(params[L"size"]);
      m_supertexts[nextFreeSupertextIndex].bExplicitSize = true;
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
      std::wistringstream colorSS(colorStr);
      std::wstring colorToken;
      std::vector<float> rgb;

      while (std::getline(colorSS, colorToken, L',')) {
        try {
          rgb.push_back(std::stof(colorToken));
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
    return;
  }
  if (MSG_IS(sMessage, L"AMP|")) {
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
    return;
  }
  if (MSG_IS(sMessage, L"PRESET=")) {
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

    // Always visible at default LogLevel=2 — pipe loads were hard to diagnose
    DebugLogWFmt(LOG_WARN, L"IPC PRESET= request: %s", message.c_str());
    {
      char line[1024];
      FormatToA(line, "IPC PRESET= %ls\n", message.c_str());
      DebugLogDiagAppend(L"diag_preset_load.txt", line);
    }

    if (sPath.length() > 0) {
      // Ensure 'sNewPath' is zero-terminated before using it in wcscmp
      wchar_t sNewPath[MAX_PATH];
      CopyTo(sNewPath, sPath.c_str());
      // ensure it is zero-terminated
      sNewPath[MAX_PATH - 1] = L'\0';
      if (wcscmp(sNewPath, g_engine.m_szPresetDir) != 0) {
        g_engine.ChangePresetDir(sNewPath, g_engine.m_szPresetDir);
      }
    }

    // try to set the current preset index
    for (int i = 0; i < (int)PresetCount(); i++) {
      if (wcscmp(PresetNameAt(i).c_str(), sFilename.c_str()) == 0) {
        m_nCurrentPreset = i;
        break;
      }
    }

    // Hard-cut on IPC so load applies immediately (blend was masking failed/stuck loads)
    LoadPreset(message.c_str(), 0.0f);
    return;
  }
  if (MSG_IS(sMessage, L"WAVE|")) {
    std::wstring message(sMessage + 5);
    SetWaveParamsFromMessage(message);
    return;
  }
  if (MSG_IS(sMessage, L"DEVICE=")) {
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
    CopyTo(g_engine.m_szAudioDevicePrevious, g_engine.m_szAudioDevice);
    g_engine.m_nAudioDevicePreviousType = g_engine.m_nAudioDeviceActiveType;
    CopyTo(g_engine.m_szAudioDevice, message.c_str());
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
    return;
  }
  if (MSG_IS(sMessage, L"OPACITY=")) {
    std::wstring message(sMessage + 8);
    fOpacity = std::stof(message);
    SetOpacity(GetPluginWindow());
    return;
  }
  if (MSG_IS(sMessage, L"STATE")) {
    int display = (int)std::ceil(100 * fOpacity);
    // (a "Opacity: %d%%" buffer used to be formatted here and never read — the
    //  opacity actually reported to the remote is the OPACITY= line below)
    SendMessageToMDropDX12Remote((L"OPACITY=" + std::to_wstring(display)).c_str());
    SendPresetChangedInfoToMDropDX12Remote();
    SendSettingsInfoToMDropDX12Remote();
    SendTrackInfoToMDropDX12Remote();
    // Compact preset/load status for agents (read all pipe messages until END_BATCH)
    {
      extern PipeServer g_pipeServer;
      wchar_t status[1536];
      const wchar_t* desc = (m_pState && m_pState->m_szDesc[0]) ? m_pState->m_szDesc : L"";
      const wchar_t* loading = m_szLoadingPreset[0] ? m_szLoadingPreset : L"";
      FormatTo(status,
        L"PRESET_STATUS|file=%s|desc=%s|loading=%s|loadingFlag=%d|ready=%d"
        L"|shadertoy=%d|hasA=%d|hasB=%d|hasC=%d|hasD=%d"
        // fps= is the ACHIEVED rate, not the cap -- GET_FPS answers FPSCAP and
        // there was nothing that answered this. It rides here because the
        // parent already fetches this reply from every child once per poll, so
        // a display's real rate costs no extra round trip; a child cannot be
        // asked directly, since its pipe replies only to its parent (#246).
        L"|compPS=%d|bufferAPS=%d|logLevel=%d|fps=%.1f",
        m_szCurrentPresetFile, desc, loading,
        m_nLoadingPreset,
        m_bPresetLoadReady.load() ? 1 : 0,
        m_bShadertoyMode ? 1 : 0,
        m_bHasBufferA ? 1 : 0, m_bHasBufferB ? 1 : 0,
        m_bHasBufferC ? 1 : 0, m_bHasBufferD ? 1 : 0,
        m_pState ? m_pState->m_nCompPSVersion : 0,
        m_pState ? m_pState->m_nBufferAPSVersion : 0,
        g_debugLogLevel,
        GetFps());
      g_pipeServer.Send(status);
    }
    if (m_nNumericInputMode == NUMERIC_INPUT_MODE_CUST_MSG) {
      PostMessageToMDropDX12Remote(WM_USER_MESSAGE_MODE);
    }
    else {
      PostMessageToMDropDX12Remote(WM_USER_SPRITE_MODE);
    }
    // Send device volume state
    {
      float curVol = 0; BOOL muted = FALSE;
      if (SUCCEEDED(GetDeviceVolume(m_szAudioDevice, m_nAudioDeviceRequestType, &curVol, &muted))) {
        wchar_t buf[128];
        FormatTo(buf, L"DEVICE_VOLUME=%.2f|muted=%d", curVol, (int)muted);
        SendMessageToMDropDX12Remote(buf, true);
      }
    }
    // Send render window rect for MCP compare positioning (via pipe, not WM_COPYDATA)
    {
      HWND hRender = m_lpDX ? m_lpDX->GetHwnd() : nullptr;
      if (hRender) {
        RECT wr;
        GetWindowRect(hRender, &wr);
        wchar_t buf[128];
        FormatTo(buf, L"renderwin=(%d,%d)-(%d,%d)",
            wr.left, wr.top, wr.right, wr.bottom);
        extern PipeServer g_pipeServer;
        g_pipeServer.Send(buf);
      }
    }
    // Send end-of-batch sentinel — MCP detects this to resolve immediately
    {
      extern PipeServer g_pipeServer;
      g_pipeServer.Send(L"END_BATCH");
    }
    return;
  }
  if (MSG_IS(sMessage, L"RELOAD_ANNOTATIONS")) {
    // Re-read presets.json. Tags are normally edited through the browser;
    // this lets an external editor (or a test) change them and have the
    // running instance pick them up, which matters because tags decide which
    // shader override a preset gets.
    extern PipeServer g_pipeServer;
    LoadPresetAnnotations();
    ResolveShaderOverrideForPreset(m_pState);
    const int total = m_annot.with([](AnnotStore& s) { return (int)s.byKey.size(); });
    wchar_t reply[64];
    FormatTo(reply, L"ANNOTATIONS_RELOADED=%d", total);
    g_pipeServer.Send(reply);
    return;
  }
  else if (MSG_IS(sMessage, L"SET_PRESET_SHADER_OVERRIDE=") ||
           MSG_IS(sMessage, L"SET_PRESET_VFX_PROFILE=") ||
           MSG_IS(sMessage, L"CLEAR_PRESET_OVERRIDE=")) {
    // The per-preset slots, on whatever preset is running.
    //
    // SET_ with a name selects it; SET_ with an EMPTY value means "explicitly
    // none", which suppresses whatever this preset's tags would have chosen.
    // CLEAR_ removes the member entirely so the tags apply again. Those last
    // two are different states and the reply reports which one happened.
    const bool bClear = (MSG_IS(sMessage, L"CLEAR_PRESET_OVERRIDE="));
    const wchar_t* eq = wcschr(sMessage, L'=');
    const std::wstring val = eq ? std::wstring(eq + 1) : std::wstring();

    bool bShader;
    if (bClear)
      bShader = (_wcsicmp(val.c_str(), L"shader") == 0);
    else
      bShader = (MSG_IS(sMessage, L"SET_PRESET_SHADER_OVERRIDE="));

    const std::wstring name = bClear ? std::wstring() : val;
    const bool bPresent = !bClear;

    if (bShader) SetPresetShaderOverride(m_szCurrentPresetFile, name, bPresent);
    else         SetPresetVFXProfile(m_szCurrentPresetFile, name, bPresent);

    ResolveShaderOverrideForPreset(m_pState);
    if (bShader) RequestShaderRecompile();

    extern PipeServer g_pipeServer;
    std::wstring reply = std::wstring(L"PRESET_OVERRIDE_SET=")
      + (bShader ? L"shader" : L"vfx") + L"|" + name
      + L"|" + (bPresent ? L"1" : L"0");
    g_pipeServer.Send(reply.c_str());
  }
  if (MSG_IS(sMessage, L"VFX_SCOPED_KEEP=")) {
    // Answer a pending scoped edit without clicking the prompt. The prompt's
    // buttons call the same function, so the tested path IS the clicked path.
    const bool bKeep = _wtoi(sMessage + 16) != 0;
    AnswerScopedVFX(bKeep);
    extern PipeServer g_pipeServer;
    wchar_t reply[48];
    FormatTo(reply, L"VFX_SCOPED_KEEP=%d", bKeep ? 1 : 0);
    g_pipeServer.Send(reply);
    return;
  }
  if (MSG_IS(sMessage, L"VFX_SCOPED_STATUS")) {
    extern PipeServer g_pipeServer;
    std::wstring out = L"VFX_SCOPED|active=" + std::wstring(m_bVFXScopeActive ? L"1" : L"0")
      + L"|profile=" + std::wstring(m_szVFXScopeProfile)
      + L"|dirty=" + std::wstring(m_bVFXScopeDirty ? L"1" : L"0");
    g_pipeServer.Send(out.c_str());
    return;
  }
  if (MSG_IS(sMessage, L"PRESET_OVERRIDE_STATUS")) {
    // What each slot resolved to for the running preset, AND from where.
    // The source is the point: the result alone cannot distinguish a tag rule
    // having matched from a per-preset entry having won, and those are the two
    // halves of the feature.
    auto srcName = [](OverrideSource s) -> const wchar_t* {
      return s == OverrideSource::Preset ? L"preset"
           : s == OverrideSource::Rule   ? L"rule"
                                         : L"none";
    };
    extern PipeServer g_pipeServer;
    std::wstring msg = L"PRESET_OVERRIDE|shader=" + m_activeOverride.name
      + L"|shaderSrc=" + srcName(m_activeOverride.source)
      + L"|vfx=" + m_resolvedVFXProfile
      + L"|vfxSrc=" + srcName(m_resolvedVFXSource)
      + L"|tag=" + m_activeOverride.matchedTag;
    g_pipeServer.Send(msg.c_str());
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_OVERRIDE_STATUS")) {
    // What the running preset resolved to, and why.
    extern PipeServer g_pipeServer;
    ShaderOverrideStore& store = ShaderOverrides();
    const wchar_t* source = !m_activeOverride.IsActive() ? L"none"
                          : (m_activeOverride.fromRule ? L"rule" : L"manual");
    std::wstring err;
    if (m_activeOverride.warpFailed) err += L"warp;";
    if (m_activeOverride.compFailed) err += L"comp;";

    wchar_t reply[1024];
    FormatTo(reply,
      L"SHADER_OVERRIDE_STATUS|enabled=%d|resolved=%s|source=%s|tag=%s"
      L"|warp=%d|comp=%d|failed=%s|overrides=%d|rules=%d|shadertoy=%d"
      L"|audio=%s|audioSource=%s",
      store.IsEnabled() ? 1 : 0,
      m_activeOverride.name.c_str(),
      source,
      m_activeOverride.matchedTag.c_str(),
      (int)m_activeOverride.warpText.size(),
      (int)m_activeOverride.compText.size(),
      err.empty() ? L"" : err.c_str(),
      (int)store.Overrides().size(),
      (int)store.Rules().size(),
      m_bShadertoyMode ? 1 : 0,
      m_resolvedAudioProfile.c_str(),
      m_resolvedAudioSource == OverrideSource::Preset ? L"preset"
        : m_resolvedAudioSource == OverrideSource::Rule ? L"tag" : L"global");
    g_pipeServer.Send(reply);
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_OVERRIDE_RELOAD")) {
    // Re-read shaderoverrides.json and every shader file it points at, then
    // re-resolve for the running preset. Lets a shader be edited in an external
    // editor and picked up without a restart.
    extern PipeServer g_pipeServer;
    ShaderOverrideStore& store = ShaderOverrides();
    store.Load(m_szMilkdrop2Path);
    ResolveShaderOverrideForPreset(m_pState);
    RequestShaderRecompile();
    wchar_t reply[128];
    FormatTo(reply, L"SHADER_OVERRIDE_RELOADED=%d", (int)store.Overrides().size());
    g_pipeServer.Send(reply);
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_OVERRIDE_REVERT")) {
    extern PipeServer g_pipeServer;
    RevertOverrideOnCurrentPreset();
    g_pipeServer.Send(L"SHADER_OVERRIDE_APPLIED=");
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_OVERRIDE_ENABLE=")) {
    extern PipeServer g_pipeServer;
    const bool on = (_wtoi(sMessage + 23) != 0);
    ShaderOverrides().SetEnabled(on);
    ShaderOverrides().Save();
    // Re-resolve: switching the master off must restore the preset's own
    // shaders immediately, not at the next preset change.
    ResolveShaderOverrideForPreset(m_pState);
    RequestShaderRecompile();
    wchar_t reply[64];
    FormatTo(reply, L"SHADER_OVERRIDE_ENABLED=%d", on ? 1 : 0);
    g_pipeServer.Send(reply);
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_OVERRIDE_APPLY=")) {
    extern PipeServer g_pipeServer;
    std::wstring name(sMessage + 22);
    if (name.empty()) {
      g_pipeServer.Send(L"SHADER_OVERRIDE_APPLIED=ERROR|no name");
    } else if (!ShaderOverrides().Find(name)) {
      g_pipeServer.Send(L"SHADER_OVERRIDE_APPLIED=ERROR|no such override|" + name);
    } else if (!ApplyOverrideToCurrentPreset(name)) {
      g_pipeServer.Send(L"SHADER_OVERRIDE_APPLIED=ERROR|not applicable|" + name);
    } else {
      g_pipeServer.Send(L"SHADER_OVERRIDE_APPLIED=" + name);
    }
    return;
  }
  if (MSG_IS(sMessage, L"SET_PRESET_NOTE=")) {
    // Set the running preset's note text. Recorded in presets.json against the
    // preset's content hash, so the note follows the file if it is renamed or
    // moved; the .milk file is not touched.
    //
    // Exists because SetPresetNote had exactly one caller -- the preset browser
    // -- so a note could only be written by hand, one preset at a time. A
    // triage pass over thousands of presets needs to record WHY a preset was
    // judged, and editing presets.json from outside is unsafe while the app is
    // running: the engine rewrites that file on exit and would discard it.
    //
    // Format: SET_PRESET_NOTE=<text>   ('\n' is accepted for line breaks)
    // Reply:  PRESET_NOTE=OK|<chars>   or PRESET_NOTE=ERROR|<why>
    extern PipeServer g_pipeServer;
    const wchar_t* note = sMessage + 16;
    if (!m_szCurrentPresetFile[0]) {
      g_pipeServer.Send(L"PRESET_NOTE=ERROR|no preset loaded");
    } else {
      SetPresetNote(m_szCurrentPresetFile, std::wstring(note));
      wchar_t reply[64];
      FormatTo(reply, L"PRESET_NOTE=OK|%d", (int)wcslen(note));
      g_pipeServer.Send(reply);
      DLOG_INFO("Preset note set via IPC (%d chars) for %ls",
                (int)wcslen(note), m_szCurrentPresetFile);
    }
    return;
  }
  if (MSG_IS(sMessage, L"GET_PRESET_NOTE")) {
    // Query only. Reply: PRESET_NOTE=<text> (empty if none / no preset).
    extern PipeServer g_pipeServer;
    std::wstring out = L"PRESET_NOTE=";
    if (m_szCurrentPresetFile[0]) {
      std::wstring note;
      WithAnnotation(m_szCurrentPresetFile, false,
                     [&](PresetAnnotation& a) { note = a.notes; });
      out += note;
    }
    g_pipeServer.Send(out.c_str());
    return;
  }
  if (MSG_IS(sMessage, L"SET_PRESET_RATING=")) {
    // Rate the running preset.  Recorded in presets.json against the preset's
    // current content hash; the .milk file is not touched.
    extern PipeServer g_pipeServer;
    int value = _wtoi(sMessage + 18);
    if (value < 0) value = 0;
    if (value > 5) value = 5;
    if (!m_szCurrentPresetFile[0]) {
      g_pipeServer.Send(L"PRESET_RATING=ERROR|no preset loaded");
    } else {
      SetPresetRatingMDX(m_szCurrentPresetFile, value);
      if (m_pState) m_pState->m_fRating = (float)value;
      wchar_t reply[128];
      FormatTo(reply, L"PRESET_RATING=%d|source=mdx", value);
      g_pipeServer.Send(reply);
    }
    return;
  }
  if (MSG_IS(sMessage, L"GET_PRESET_RATING")) {
    // Reports the effective rating and where it came from: "mdx" when this
    // install has rated the preset, "file" when falling back to the preset's
    // own fRating, "none" when neither exists.
    extern PipeServer g_pipeServer;
    const float fFileRating = m_pState ? m_pState->m_fRating : 0.f;
    int obs = 0;
    if (m_szCurrentPresetFile[0])
      WithAnnotation(m_szCurrentPresetFile, false,
                     [&](PresetAnnotation& a) { obs = (int)a.ratings.size(); });
    const int effective = EffectiveRating(m_szCurrentPresetFile, fFileRating);
    const wchar_t* source = (obs > 0) ? L"mdx" : (fFileRating > 0.f ? L"file" : L"none");
    wchar_t reply[192];
    FormatTo(reply, L"PRESET_RATING=%d|source=%s|obs=%d|file=%.3f",
               effective, source, obs, fFileRating);
    g_pipeServer.Send(reply);
    return;
  }
  if (MSG_IS(sMessage, L"GET_PRESET_HASH")) {
    // Content identity of a preset file (see preset_hash.h).  With "=<path>"
    // hashes that file; bare, reports the running preset's hash.
    //
    // The path form exists so the test harness can pin the normalization rule
    // against this implementation rather than a second copy of it in Python.
    extern PipeServer g_pipeServer;
    std::wstring reply;
    if (sMessage[15] == L'=' && sMessage[16]) {
      std::string hash = ComputePresetHashFile(sMessage + 16);
      if (hash.empty())
        reply = L"PRESET_HASH=ERROR|cannot read file";
      else
        reply = L"PRESET_HASH=" + std::wstring(hash.begin(), hash.end());
    }
    else if (m_pState && m_pState->m_szPresetHash[0]) {
      const char* h = m_pState->m_szPresetHash;
      reply = L"PRESET_HASH=" + std::wstring(h, h + strlen(h));
    }
    else {
      reply = L"PRESET_HASH=ERROR|no preset loaded";
    }
    g_pipeServer.Send(reply);
    return;
  }
  if (MSG_IS(sMessage, L"GET_PRESET_STATUS")) {
    // Single-message query for agents (no batch drain needed)
    extern PipeServer g_pipeServer;
    wchar_t status[1536];
    const wchar_t* desc = (m_pState && m_pState->m_szDesc[0]) ? m_pState->m_szDesc : L"";
    const wchar_t* loading = m_szLoadingPreset[0] ? m_szLoadingPreset : L"";
    FormatTo(status,
      L"PRESET_STATUS|file=%s|desc=%s|loading=%s|loadingFlag=%d|ready=%d"
      L"|shadertoy=%d|hasA=%d|hasB=%d|hasC=%d|hasD=%d"
      L"|compPS=%d|bufferAPS=%d|compBlob=%d|bufferABlob=%d"
      // ui= is the UI_* mode: 0 is UI_REGULAR, anything else means the preset
      // browser, menu or a save dialog is over the render window. Reported
      // because nothing else could tell a caller, and a harness that cannot ask
      // has to guess -- the probe harness guessed by sending ESC after every
      // preset load, which with the window focused is a CLOSE request, not a
      // no-op, and left a modal "Close MDropDX12 Visualizer?" on the user's
      // desktop with the app disabled behind it.
      // fps= as above: the ACHIEVED rate. Both PRESET_STATUS emitters carry it,
      // or which one answered would decide whether the field exists.
      L"|warpLock=%d|compLock=%d|meshX=%d|meshY=%d|ui=%d|fps=%.1f",
      m_szCurrentPresetFile, desc, loading,
      m_nLoadingPreset,
      m_bPresetLoadReady.load() ? 1 : 0,
      m_bShadertoyMode ? 1 : 0,
      m_bHasBufferA ? 1 : 0, m_bHasBufferB ? 1 : 0,
      m_bHasBufferC ? 1 : 0, m_bHasBufferD ? 1 : 0,
      m_pState ? m_pState->m_nCompPSVersion : 0,
      m_pState ? m_pState->m_nBufferAPSVersion : 0,
      (m_shaders.comp.bytecodeBlob != NULL) ? 1 : 0,
      (m_shaders.bufferA.bytecodeBlob != NULL) ? 1 : 0,
      // A shader lock changes which STATE_ flags reach CState::Import, and so
      // which code path a preset load takes. Reported because a harness that
      // cannot see the lock cannot tell "the locked path is clean" from "the
      // lock never came on".
      m_bWarpShaderLock ? 1 : 0,
      m_bCompShaderLock ? 1 : 0,
      m_nGridX, m_nGridY,
      (int)m_UI_mode,
      GetFps());
    g_pipeServer.Send(status);
    return;
  }
  if (MSG_IS(sMessage, L"LINK=")) {
    std::wstring message(sMessage + 5);
    m_RemotePresetLink = std::stoi(message);
    return;
  }
  if (MSG_IS(sMessage, L"QUICKSAVE")) {
    g_engine.SaveCurrentPresetToQuicksave(false);
    return;
  }
  if (MSG_IS(sMessage, L"CONFIG_EXPORT_INI")) {
    // Copy what the registry holds back into settings.ini / messages.ini /
    // sprites.ini. For going back to a portable install after running on the
    // registry, and for looking at what is actually stored.
    extern PipeServer g_pipeServer;
    std::wstring summary;
    const bool ok = mdrop::ExportRegistryToIniFiles(&summary);
    wchar_t out[256];
    FormatTo(out, L"CONFIG_EXPORT_INI=%d|%ls", ok ? 1 : 0,
               ok ? summary.c_str() : L"nothing stored in the registry");
    g_pipeServer.Send(out);
    return;
  }
  if (MSG_IS(sMessage, L"CONFIG")) {
    ReadConfig();
    // to update fonts — use ResetBufferAndFonts for proper SRV cleanup
    ResetBufferAndFonts();
    return;
  }
  if (MSG_IS(sMessage, L"SETTINGS")) {
    m_fTimeBetweenPresets = Config().GetFloat(L"Settings", L"fTimeBetweenPresets", m_fTimeBetweenPresets);
    m_fPresetStartTime = GetTime();
    m_fNextPresetTime = -1.0f; // force recalculation
    // ...and answer. This used to re-read the file and reply with nothing at
    // all, so the one command named after the settings was the one way to ask
    // for them that produced no settings.
    SendSettingsInfoToMDropDX12Remote();
    return;
  }
  if (MSG_IS(sMessage, L"TESTFONTS")) {
    ClearErrors(ERR_MSG_BOTTOM_EXTRA_1);
    ClearErrors(ERR_MSG_BOTTOM_EXTRA_2);
    ClearErrors(ERR_MSG_BOTTOM_EXTRA_3);
    // Send text to appear at the bottom first, assuming a bottom corner is used
    g_engine.AddError(L"Finally the Album", g_engine.m_SongInfoDisplaySeconds, ERR_MSG_BOTTOM_EXTRA_3, false);
    g_engine.AddError(L"Here goes the Title", g_engine.m_SongInfoDisplaySeconds, ERR_MSG_BOTTOM_EXTRA_2, false);
    g_engine.AddError(L"This is the Artist", g_engine.m_SongInfoDisplaySeconds, ERR_MSG_BOTTOM_EXTRA_1, false);
    if (!g_engine.m_bShowPresetInfo) g_engine.m_bShowPresetInfo = true;
    g_engine.AddNotification(L"This is a notification");
    return;
  }
  if (MSG_IS(sMessage, L"TRACK|")) {
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
    return;
  }
  if (MSG_IS(sMessage, L"CLEARPRESET")) {
    ClearPreset();
    return;
  }
  if (MSG_IS(sMessage, L"CLEARSPRITES")) {
    g_engine.KillAllSprites();
    return;
  }
  if (MSG_IS(sMessage, L"CLEARTEXTS")) {
    g_engine.KillAllSupertexts();
    return;
  }
  if (MSG_IS(sMessage, L"VAR_TIME=")) {
    std::wstring message(sMessage + 9);
    g_engine.m_timeFactor = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"VAR_FRAME=")) {
    std::wstring message(sMessage + 10);
    g_engine.m_frameFactor = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"VAR_FPS=")) {
    std::wstring message(sMessage + 8);
    g_engine.m_fpsFactor = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"VAR_INTENSITY=")) {
    std::wstring message(sMessage + 14);
    g_engine.m_VisIntensity = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"VAR_SHIFT=")) {
    std::wstring message(sMessage + 10);
    g_engine.m_VisShift = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"VAR_VERSION=")) {
    std::wstring message(sMessage + 12);
    g_engine.m_VisVersion = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"COL_HUE=")) {
    std::wstring message(sMessage + 8);
    g_engine.m_ColShiftHue = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"HUE_AUTO=")) {
    g_engine.m_AutoHue = (sMessage[9] == L'1');
    return;
  }
  if (MSG_IS(sMessage, L"HUE_AUTO_SECONDS=")) {
    std::wstring message(sMessage + 17);
    g_engine.m_AutoHueSeconds = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"COL_SATURATION=")) {
    std::wstring message(sMessage + 15);
    g_engine.m_ColShiftSaturation = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"COL_BRIGHTNESS=")) {
    std::wstring message(sMessage + 15);
    g_engine.m_ColShiftBrightness = std::stof(message);
    return;
  }
  if (MSG_IS(sMessage, L"VAR_QUALITY=")) {
    std::wstring message(sMessage + 12);
    g_engine.m_fRenderQuality = std::stof(message);
    ResetBufferAndFonts();
    return;
  }
  if (MSG_IS(sMessage, L"VAR_AUTO=")) {
    g_engine.bQualityAuto = (sMessage[9] == L'1');
    ResetBufferAndFonts();
    return;
  }
  if (MSG_IS(sMessage, L"SPOUT_ACTIVE=")) {
    wchar_t status = sMessage[13];
    if ((status == L'0' && bSpoutOut) || (status == L'1' && !bSpoutOut)) {
      ToggleSpout();
    }
    return;
  }
  if (MSG_IS(sMessage, L"SPOUT_FIXEDSIZE=")) {
    wchar_t status = sMessage[16];
    if ((status == L'0' && bSpoutFixedSize) || (status == L'1' && !bSpoutFixedSize)) {
      SetSpoutFixedSize(true, true);
    }
    return;
  }
  if (MSG_IS(sMessage, L"SPOUT_RESOLUTION=")) {
    std::wstring message(sMessage + 17);
    size_t pos = message.find(L'x');
    if (pos != std::wstring::npos) {
      std::wstring width = message.substr(0, pos);
      std::wstring height = message.substr(pos + 1);
      nSpoutFixedWidth = std::stoi(width);
      nSpoutFixedHeight = std::stoi(height);
      SetSpoutFixedSize(false, true);
    }
    return;
  }
  if (MSG_IS(sMessage, L"SPOUTINPUT=")) {
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
    return;
  }
  if (MSG_IS(sMessage, L"CAPTURE")) {
    DebugLogW(L"[CAPTURE] Message received");
    mdropdx12->LogInfo(L"CAPTURE message received, calling CaptureScreenshot()");
    wchar_t filename[MAX_PATH];
    if (CaptureScreenshotWithFilename(filename, MAX_PATH)) {
      // Respond with full path so MCP can read the file directly
      wchar_t response[MAX_PATH + 32];
      FormatTo(response, L"CAPTURE_PATH=%s", m_screenshotPath);
      extern PipeServer g_pipeServer;
      g_pipeServer.Send(response);
      DLOGW_INFO(L"[CAPTURE] Queued screenshot: %s", m_screenshotPath);
    } else {
      extern PipeServer g_pipeServer;
      g_pipeServer.Send(L"CAPTURE_PATH=ERROR");
      DebugLogW(L"[CAPTURE] CaptureScreenshotWithFilename failed");
    }
    return;
  }
  if (MSG_IS(sMessage, L"FFT_ATTACK=")) {
    m_fFFTAttackGlobal = (float)_wtof(sMessage + 11);
    m_fFFTAttackGlobal = max(0.0f, min(1.0f, m_fFFTAttackGlobal));
    m_bFFTSmoothingActive = true;
    return;
  }
  if (MSG_IS(sMessage, L"FFT_DECAY=")) {
    m_fFFTDecayGlobal = (float)_wtof(sMessage + 10);
    m_fFFTDecayGlobal = max(0.0f, min(1.0f, m_fFFTDecayGlobal));
    m_bFFTSmoothingActive = true;
    return;
  }
  if (MSG_IS(sMessage, L"LOAD_LIST=")) {
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
        Config().SetString(L"Settings", L"szActivePresetList", m_szActivePresetList.c_str());
        g_pipeServer.Send(L"LOAD_LIST_RESULT=OK|" + listName);
      } else {
        g_pipeServer.Send(L"LOAD_LIST_RESULT=ERROR|list not found: " + listName);
      }
    }).detach();
    return;
  }
  if (MSG_IS(sMessage, L"CLEAR_LIST")) {
    // Offloaded from render thread — INI write + UpdatePresetList
    std::thread([this]() {
      m_szActivePresetList.clear();
      Config().SetString(L"Settings", L"szActivePresetList", L"");
      UpdatePresetList(true, true);
      extern PipeServer g_pipeServer;
      g_pipeServer.Send(L"CLEAR_LIST_RESULT=OK");
    }).detach();
    return;
  }
  if (MSG_IS(sMessage, L"ENUM_LISTS")) {
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
    return;
  }
  if (MSG_IS(sMessage, L"SET_DIR=")) {
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
        Config().SetString(L"Settings", L"szActivePresetList", L"");
        m_nSubdirMode = bRecursive ? 1 : 0;
        m_nCurrentPreset = -1;
        ChangePresetDir(szNewDir, m_szPresetDir);
        g_pipeServer.Send(L"SET_DIR_RESULT=OK|" + value);
      } else {
        g_pipeServer.Send(L"SET_DIR_RESULT=ERROR|directory not found: " + value);
      }
    }).detach();
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_IMPORT=")) {
    // Load JSON, convert GLSL→HLSL, apply.  Format: SHADER_IMPORT=<path>
    std::wstring filePath(sMessage + 14);
    DebugLogW((L"[SHADER_IMPORT] Loading: " + filePath).c_str());
    if (!m_shaderImportWindow)
        m_shaderImportWindow = std::make_unique<mdrop::ShaderImportWindow>(this);
    std::wstring result = m_shaderImportWindow->ImportFromFile(filePath.c_str());
    DebugLogW((L"[SHADER_IMPORT] Result: " + result).c_str());
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"SHADER_IMPORT_RESULT=" + result);
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_GLSL=")) {
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
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_CONVERT=")) {
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
    return;
  }
  if (MSG_IS(sMessage, L"SHADER_SAVE=")) {
    // Save current shader passes as preset.  Format: SHADER_SAVE=<path.milk3>
    std::wstring savePath(sMessage + 12);
    DebugLogW((L"[SHADER_SAVE] " + savePath).c_str());
    if (!m_shaderImportWindow)
        m_shaderImportWindow = std::make_unique<mdrop::ShaderImportWindow>(this);
    std::wstring result = m_shaderImportWindow->SavePresetToFile(savePath.c_str());
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"SHADER_SAVE_RESULT=" + result);
    return;
  }
  if (MSG_IS(sMessage, L"SET_LOGLEVEL=")) {
    // Change log level at runtime.  Format: SET_LOGLEVEL=<0-4>
    int newLevel = _wtoi(sMessage + 13);
    if (newLevel < 0) newLevel = 0;
    if (newLevel > 4) newLevel = 4;
    m_LogLevel = newLevel;
    DebugLogSetLevel(newLevel);
    if (mdropdx12) mdropdx12->logLevel = newLevel;
    Config().SetInt(L"Milkwave", L"LogLevel", newLevel);
    const wchar_t* names[] = { L"Off", L"Error", L"Warn", L"Info", L"Verbose" };
    wchar_t buf[64];
    FormatTo(buf, L"LOGLEVEL=%d|%s", newLevel, names[newLevel]);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
    DLOG_INFO("Log level changed to %d via IPC", newLevel);
    return;
  }
  if (MSG_IS(sMessage, L"SET_VFX=")) {
    // Set one Video Effects parameter.  Format: SET_VFX=<Name>=<value>
    extern PipeServer g_pipeServer;
    std::wstring name, value;
    if (!SplitNameValue(sMessage + 8, name, value)) {
      g_pipeServer.Send(L"VFX_SET=ERROR|expected SET_VFX=<Name>=<value>");
    } else {
      const VfxParam* pm = FindVfxParam(name.c_str());
      if (!pm) {
        g_pipeServer.Send(L"VFX_SET=ERROR|unknown|" + name);
      } else {
        SetVfxValue(m_videoFX, *pm, (float)_wtof(value.c_str()));
        OnVideoFXChanged();
        g_pipeServer.Send(L"VFX_SET=" + std::wstring(pm->name) + L"=" + FormatVfxValue(m_videoFX, *pm));
        RepaintToolWindow(m_pVideoEffectsWindow);
        DLOG_INFO("VideoFX set via IPC: %S=%S", pm->name, value.c_str());
      }
    }
    return;
  }
  if (MSG_IS(sMessage, L"GET_VFX_STATUS")) {
    // Which profile is loaded, and does it have unsaved changes?
    // Saving is explicit now, so a caller driving parameters over IPC needs a
    // way to see the same state the red Save button shows.
    const wchar_t* name = m_szCurrentVFXProfile[0] ? PathFindFileNameW(m_szCurrentVFXProfile) : L"";
    std::wstring stem(name);
    const size_t dot = stem.rfind(L'.');
    if (dot != std::wstring::npos) stem.erase(dot);
    std::wstring out = L"VFX_STATUS=profile=" + stem +
                       L"|dirty=" + (IsVideoFXDirty() ? L"1" : L"0");
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(out);
    return;
  }
  if (MSG_IS(sMessage, L"GET_VFX")) {
    // Report every Video Effects parameter as <Name>=<value>|...
    std::wstring out = L"VFX=";
    for (int i = 0; i < kVfxParamCount; i++) {
      if (i) out += L"|";
      out += kVfxParams[i].name;
      out += L"=";
      out += FormatVfxValue(m_videoFX, kVfxParams[i]);
    }
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(out);
    return;
  }
  if (MSG_IS(sMessage, L"VFX_RESET=")) {
    // Reset a group of Video Effects parameters.
    // Format: VFX_RESET=<transform|effects|audio|all>
    //
    // The three groups are exactly the window's three Reset buttons, including
    // the detail that blendMode counts as Transform (its combo box lives on the
    // Transform tab). A group that reset a different set of fields over IPC
    // than the button of the same name would be its own trap.
    //
    // Values come from a fresh VideoEffectParams so they cannot drift from the
    // member initialisers.
    const std::wstring which(sMessage + 10);
    const VideoEffectParams def{};
    const bool all = (_wcsicmp(which.c_str(), L"all") == 0);
    bool known = all;

    if (all || _wcsicmp(which.c_str(), L"transform") == 0) {
      m_videoFX.posX = def.posX; m_videoFX.posY = def.posY;
      m_videoFX.scale = def.scale; m_videoFX.rotation = def.rotation;
      m_videoFX.mirrorH = def.mirrorH; m_videoFX.mirrorV = def.mirrorV;
      m_videoFX.blendMode = def.blendMode;
      known = true;
    }
    if (all || _wcsicmp(which.c_str(), L"effects") == 0) {
      m_videoFX.tintR = def.tintR; m_videoFX.tintG = def.tintG; m_videoFX.tintB = def.tintB;
      m_videoFX.brightness = def.brightness; m_videoFX.contrast = def.contrast;
      m_videoFX.saturation = def.saturation; m_videoFX.hueShift = def.hueShift;
      m_videoFX.invert = def.invert;
      m_videoFX.pixelation = def.pixelation; m_videoFX.chromatic = def.chromatic;
      m_videoFX.edgeDetect = def.edgeDetect;
      known = true;
    }
    if (all || _wcsicmp(which.c_str(), L"audio") == 0) {
      m_videoFX.arPosX = def.arPosX; m_videoFX.arPosY = def.arPosY;
      m_videoFX.arScale = def.arScale; m_videoFX.arRotation = def.arRotation;
      m_videoFX.arBrightness = def.arBrightness; m_videoFX.arSaturation = def.arSaturation;
      m_videoFX.arChromatic = def.arChromatic;
      known = true;
    }

    extern PipeServer g_pipeServer;
    if (!known) {
      g_pipeServer.Send(L"VFX_RESET=ERROR|unknown group|" + which);
    } else {
      OnVideoFXChanged();
      g_pipeServer.Send(L"VFX_RESET=" + which);
      RepaintToolWindow(m_pVideoEffectsWindow);
    }
    return;
  }
  if (MSG_IS(sMessage, L"VFX_PROFILE_LIST")) {
    // List the saved VFX profiles by name.
    std::vector<std::wstring> names;
    m_vfxProfiles.Names(names);

    std::wstring out = L"VFX_PROFILES=";
    for (size_t i = 0; i < names.size(); i++) {
      if (i) out += L"|";
      out += names[i];
    }
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(out);
    return;
  }
  // MSG_EQ, not MSG_IS: DIAG_GPURISK= and DIAG_GPUTIME= both begin with this
  // and are handled below, so a prefix match here made them unreachable.
  if (MSG_EQ(sMessage, L"DIAG_GPU")) {
    // What this PROCESS is costing the GPU, plus the VRAM figures the engine
    // already samples. One instance per reply and keyed by pid, because the
    // question it exists for -- what does a parent plus N children cost --
    // needs each process counted separately and then summed by the caller.
    //
    // Asking turns the sampler on for a few seconds. The counter is a rate, so
    // the FIRST reply after a quiet period carries gpu3d=-1: poll twice, about
    // a second apart, and read the second. That is not a fault to work around;
    // it is what a rate counter is.
    extern PipeServer g_pipeServer;
    m_gpuWantedUntilTick = GetTickCount64() + 8000;
    // What the TEXTURE CACHE is holding, alongside the VRAM figure (#312).
    //
    // Chasing ~70 MB of VRAM per distinct preset, the first question is whether
    // the cache is the thing growing -- and nothing reported its size, so the
    // only way to ask was to read the log and count "loaded texture" lines,
    // which says what was loaded and not what is still held.
    //
    // evictable is the part EvictSomeTexture is allowed to drop. It used to
    // say here that nothing called that function; #312/#314 gave it callers,
    // and EnforceTextureCacheBudget now evicts on most preset loads once the
    // cache is at its cap.
    //
    // READ, not walked (#352). This runs on the IPC thread, and m_textures is
    // a plain vector the render thread push_backs into -- reallocating -- and
    // erases from. The render thread publishes these three numbers at every
    // mutation instead; a diagnostic that reports a torn figure is worse than
    // one that reports none, because the figure is what a measurement run
    // then reasons from.
    const size_t texCount     = (size_t)m_texCacheCount.load(std::memory_order_relaxed);
    const size_t texBytes     = (size_t)m_texCacheBytes.load(std::memory_order_relaxed);
    const size_t texEvictable = (size_t)m_texCacheEvictableBytes.load(std::memory_order_relaxed);
    wchar_t out[640];
    FormatTo(out,
      L"DIAG_GPU|pid=%lu|child=%d|gpu3d=%.2f|gpuall=%.2f"
      L"|vramBudgetMB=%.1f|vramUsageMB=%.1f|vramAvailPct=%.1f"
      L"|texCount=%d|texMB=%.1f|texEvictableMB=%.1f|presetsLoaded=%d",
      (unsigned long)GetCurrentProcessId(),
      m_bChildMode ? 1 : 0,
      m_gpuUsage.Percent3D(), m_gpuUsage.PercentAll(),
      (double)m_vramBudgetBytes / (1024.0 * 1024.0),
      (double)m_vramUsageBytes / (1024.0 * 1024.0),
      m_vramAvailablePercent,
      (int)texCount,
      (double)texBytes / (1024.0 * 1024.0),
      (double)texEvictable / (1024.0 * 1024.0),
      m_nPresetsLoadedTotal);
    g_pipeServer.Send(out);
    return;
  }
  if (MSG_IS(sMessage, L"DIAG_CONFIG")) {
    // What the settings layer has been doing. A test asserts on `shielded` and
    // `writes` to prove a run left the user's settings.ini alone: with testing
    // mode on, every Set* should land in `shielded` and none in `writes`.
    //
    // DIAG_CONFIG=flush forces the write-back cache out first, for a test that
    // wants to read the file rather than trust the counters.
    extern PipeServer g_pipeServer;
    if (wcsstr(sMessage, L"=flush")) mdrop::ConfigFlushAll();
    const mdrop::ConfigStats s = mdrop::ConfigDiagnostics();
    wchar_t out[512];
    FormatTo(out,
      L"DIAG_CONFIG|shield=%d|override=%d|persist=%d|testingUsed=%d"
      L"|sets=%lld|writes=%lld|elided=%lld|shielded=%lld|reads=%lld|flushes=%lld"
      L"|dirtyMain=%d",
      mdrop::IsConfigWriteShielded() ? 1 : 0,
      mdrop::IsConfigWriteOverridden() ? 1 : 0,
      m_bTestingModeWritesSettings ? 1 : 0,
      m_bTestingModeUsedThisSession ? 1 : 0,
      s.sets, s.writes, s.elided, s.shielded, s.reads, s.flushes,
      mdrop::Config().IsDirty() ? 1 : 0);
    g_pipeServer.Send(out);
    return;
  }
  if (MSG_IS(sMessage, L"DIAG_AUDIO")) {
    // What the audio path is actually doing right now. bass/mid/treb are
    // relative (imm / long_avg) so they hover near 1.0 in every mode and are
    // useless for telling one apart -- the mode itself is the thing worth
    // reporting, and it is what a test can assert without depending on
    // whatever is playing.
    extern PipeServer g_pipeServer;
    wchar_t out[512];
    FormatTo(out,
      L"DIAG_AUDIO|profile=%ls|bandMode=%ls|fftScale=%.8f|fftSqrt=%d"
      L"|fftHzRef=%.1f|pcmGain=%.4f"
      // MD3's FFT treatment. Reported because these three were unobservable
      // from outside, and being unobservable is how they stayed dead from
      // 30b26ff until 2026-09-05: they were set by BuiltInMilkDrop3() and then
      // reset to false/0/0 by every profile load, and nothing could tell.
      L"|fftPeakNormalise=%d|fftRelGate=%.4f|fftPostGateExp=%.3f|fftSpreadTaps=%d"
      L"|texCalls=%d|holdFrames=%d|peakDecay=%.5f"
      L"|bass=%.4f|mid=%.4f|treb=%.4f",
      m_resolvedAudioProfile.c_str(),
      m_audioProfile.bandMode == BandMode::MilkDrop ? L"milkdrop" : L"custom",
      m_audioProfile.fftScale,
      m_audioProfile.fftSqrt ? 1 : 0,
      m_audioProfile.fftHzRef,
      m_audioProfile.pcmGain,
      m_audioProfile.fftPeakNormalise ? 1 : 0,
      m_audioProfile.fftRelGate,
      m_audioProfile.fftPostGateExp,
      m_audioProfile.fftSpreadTaps,
      // How many times UpdateAudioTexture was CALLED in the frame it
      // last advanced on. 1 is correct; 2 is the .milk3 double-call
      // (#110), which the guard now absorbs -- the count still shows it.
      m_nAudioTexCallsThisFrame,
      m_nDiagPeakHoldFrames,
      m_fDiagPeakDecay,
      mysound.imm_rel[0], mysound.imm_rel[1], mysound.imm_rel[2]);
    g_pipeServer.Send(out);
    return;
  }
  if (MSG_IS(sMessage, L"DIAG_MESSAGES")) {
    // Every term of the autoplay guard in milkdropfs.cpp, plus the supertext
    // slots it spawns into. Written because "autoplay messages never appear"
    // could not be narrowed from outside: five of the six guard terms were
    // observable over the pipe and m_fNextAutoMsgTime was not, so there was no
    // way to tell a timer that never fires from a slot that never draws.
    extern PipeServer g_pipeServer;
    std::wstring out;
    wchar_t buf[512];
    FormatTo(buf,
      // restrained= is the term the spawn timer actually reads. testing= is
      // kept beside it because it is no longer the same answer: a child holds
      // testing mode for life and still spawns messages (kChildVetoes), so
      // testing=1 alone stopped meaning "no messages will appear".
      L"DIAG_MESSAGES|enabled=%d|testing=%d|restrained=%d|autoplay=%d|count=%d"
      L"|nextAt=%.3f|now=%.3f|interval=%.2f|jitter=%.2f|maxOnScreen=%d"
      L"|sequential=%d|nextSeq=%d|animProfiles=%d",
      MessagesEnabled() ? 1 : 0, m_bTestingMode ? 1 : 0,
      Restrained(restraint::kMessages) ? 1 : 0,
      m_bMsgAutoplay ? 1 : 0, m_nMsgAutoplayCount,
      m_fNextAutoMsgTime, GetTime(),
      m_fMsgAutoplayInterval, m_fMsgAutoplayJitter, m_nMsgMaxOnScreen,
      m_bMsgSequential ? 1 : 0, m_nNextSequentialMsg, m_nAnimProfileCount);
    out = buf;
    for (int i = 0; i < NUM_SUPERTEXTS; i++) {
      const td_supertext& st = m_supertexts[i];
      if (st.fStartTime == -1.0f)
        continue;   // free slot; only the live ones are interesting
      // owner= is which display's message loop spawned this one: -1 the
      // primary, otherwise a 1-based DISPLAYn (#255). Reported because it is
      // the only way to see from outside that the loops are actually separate
      // -- the slots are one shared pool, so without it a run cannot tell one
      // clock driving every display from several clocks agreeing by chance.
      FormatTo(buf,
        L"|st%d=start=%.3f,dur=%.3f,fadeOut=%.3f,burn=%.3f,redraw=%d,song=%d,"
        L"owner=%d,text=%.24ls",
        i, st.fStartTime, st.fDuration, st.fFadeOutTime, st.fBurnTime,
        st.bRedrawSuperText ? 1 : 0, st.bIsSongTitle ? 1 : 0,
        st.nOwnerDisplay, st.szTextW);
      out += buf;
    }
    g_pipeServer.Send(out.c_str());
    return;
  }
  if (MSG_IS(sMessage, L"DIAG_SIMULATE_DEVICE_REMOVED")) {
    // Test hook for the GPU-fatal path. ID3D12Device5::RemoveDevice exists for
    // exactly this: it forces DXGI_ERROR_DEVICE_REMOVED so the TDR handling can
    // be exercised without provoking a real driver hang.
    //
    // This DESTROYS the device and the visualizer will exit. That is the point
    // of the test: it verifies the process actually terminates rather than
    // stranding a live, permanently blank window (see the 2026-08-23 hang).
    extern PipeServer g_pipeServer;
    if (!m_lpDX || !m_lpDX->m_device) {
      g_pipeServer.Send(L"SIMULATE_DEVICE_REMOVED|no device");
    } else {
      Microsoft::WRL::ComPtr<ID3D12Device5> dev5;
      HRESULT hr = m_lpDX->m_device.As(&dev5);
      if (SUCCEEDED(hr) && dev5) {
        DebugLogA("DIAG_SIMULATE_DEVICE_REMOVED: forcing device removal\n", LOG_ERROR);
        // Reply BEFORE removing: the pipe will not outlive the device by long.
        g_pipeServer.Send(L"SIMULATE_DEVICE_REMOVED|ok");
        dev5->RemoveDevice();
      } else {
        wchar_t out[96];
        FormatTo(out, L"SIMULATE_DEVICE_REMOVED|no ID3D12Device5 (hr=0x%08X)", (unsigned)hr);
        g_pipeServer.Send(out);
      }
    }
    return;
  }
  if (MSG_IS(sMessage, L"AUDIO_PROFILE_CLEAR")) {
    // Remove the member entirely, so the preset goes back to inheriting.
    // Distinct from AUDIO_PROFILE= with an empty name, which is "explicitly
    // the default" and suppresses whatever a tag rule would have selected.
    const bool cleared = SetPresetAudioProfile(m_szCurrentPresetFile,
                                               std::wstring(), false);
    if (cleared) ResolveAudioProfileForPreset(m_pState);
    AnswerAudioProfile(L"", cleared, true);
    return;
  }
  if (MSG_IS(sMessage, L"AUDIO_PROFILE=")) {
    // ANSWERED, always. This used to set and return in silence, so a caller
    // could not tell "applied" from "did nothing" -- and it does nothing in a
    // case that is easy to be in without realising: a SCRATCH preset, meaning
    // testing mode plus a folder named in AnnotationIgnoreDirs, binds no
    // per-preset override at all.
    //
    // TestFFTProfileApplied sat squarely in that case. It turns testing mode on
    // to stop auto-advance swapping its probe out, and its probe lives under
    // resources/presets/TEST/ -- so every AUDIO_PROFILE= it sent was discarded.
    // It then asserted on a rendered pixel that could not move, and the result
    // was filed as flakiness in the audio pipeline and theorised about for
    // months (#88). One reply would have said so in the first minute.
    const std::wstring name(sMessage + 14);

    std::vector<std::wstring> known;
    AudioProfiles().Names(known);
    if (!name.empty() &&
        std::find(known.begin(), known.end(), name) == known.end()) {
      AnswerAudioProfile(name.c_str(), false, false);
      return;
    }

    const bool bound = SetPresetAudioProfile(m_szCurrentPresetFile, name, true);
    if (bound) ResolveAudioProfileForPreset(m_pState);
    AnswerAudioProfile(name.c_str(), bound, true);
    return;
  }
  if (MSG_IS(sMessage, L"AUDIO_PROFILE_LIST")) {
    // List the audio profiles by name. Named to match VFX_PROFILE_LIST rather
    // than the GET_* family, since this is the same kind of thing.
    std::vector<std::wstring> names;
    AudioProfiles().Names(names);

    std::wstring out = L"AUDIO_PROFILES=";
    for (size_t i = 0; i < names.size(); i++) {
      if (i) out += L"|";
      out += names[i];
    }
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(out);
    return;
  }
  else if (MSG_IS(sMessage, L"VFX_PROFILE_SAVE=") ||
           MSG_IS(sMessage, L"VFX_PROFILE_LOAD=")) {
    // Save or load a VFX profile.  Format: VFX_PROFILE_SAVE=<name>
    // A name, not a path -- every profile lives in vfxprofiles.json now, so
    // there is nowhere else for one to be.
    const bool bSave = (sMessage[12] == L'S' || sMessage[12] == L's');
    const std::wstring name(sMessage + 17);

    extern PipeServer g_pipeServer;
    if (name.empty()) {
      g_pipeServer.Send(bSave ? L"VFX_PROFILE_SAVED=ERROR|no name"
                              : L"VFX_PROFILE_LOADED=ERROR|no name");
    } else if (bSave) {
      const bool ok = SaveVideoFXProfile(name.c_str());
      if (ok) {
        CopyTo(m_szCurrentVFXProfile, name.c_str());
        MarkVideoFXSaved();            // just written -> clean
        SaveSpoutInputSettings();
        OnVideoFXChanged();
      }
      g_pipeServer.Send(ok ? L"VFX_PROFILE_SAVED=" + name
                           : L"VFX_PROFILE_SAVED=ERROR|" + name);
      DLOG_INFO("VFX profile save via IPC: %S -> %s", name.c_str(), ok ? "ok" : "FAILED");
    } else {
      const bool ok = LoadVideoFXProfile(name.c_str());
      if (ok) {
        MarkVideoFXSaved();            // just loaded -> clean
        SaveSpoutInputSettings();
        OnVideoFXChanged();
        RepaintToolWindow(m_pVideoEffectsWindow);
      }
      g_pipeServer.Send(ok ? L"VFX_PROFILE_LOADED=" + name
                           : L"VFX_PROFILE_LOADED=ERROR|" + name);
      DLOG_INFO("VFX profile load via IPC: %S -> %s", name.c_str(), ok ? "ok" : "FAILED");
    }
  }
  if (MSG_IS(sMessage, L"VFX_PROFILE_IMPORT=")) {
    // Import profiles from a file this build did not write.
    // Format: VFX_PROFILE_IMPORT=<path>[|<name>]
    //
    // <name> is used when the file holds a single unnamed parameter set -- an
    // old settings.ini, or a videofx/<name>.json -- and replaces an existing
    // profile of that name. Files that carry their own names keep them and are
    // numbered rather than overwriting anything.
    const std::wstring arg(sMessage + 19);
    const size_t bar = arg.find(L'|');
    const std::wstring path = (bar == std::wstring::npos) ? arg : arg.substr(0, bar);
    const std::wstring name = (bar == std::wstring::npos) ? L"" : arg.substr(bar + 1);

    extern PipeServer g_pipeServer;
    if (path.empty()) {
      g_pipeServer.Send(L"VFX_PROFILE_IMPORTED=ERROR|no path");
    } else {
      const int n = m_vfxProfiles.Import(path.c_str(), name.c_str());
      if (n < 0) {
        g_pipeServer.Send(L"VFX_PROFILE_IMPORTED=ERROR|" + path);
      } else {
        g_pipeServer.Send(L"VFX_PROFILE_IMPORTED=" + std::to_wstring(n));
        RepaintToolWindow(m_pVideoEffectsWindow);
      }
      DLOG_INFO("VFX import via IPC: %S -> %d profile(s)", path.c_str(), n);
    }
    return;
  }
  if (MSG_IS(sMessage, L"TESTING_MODE")) {
    // Freeze everything that changes the frame without being asked, and ignore
    // the keyboard.  Format: TESTING_MODE=<0|1>, or TESTING_MODE to query.
    //
    // A SET rather than a toggle, deliberately: a harness cannot drive a toggle
    // without already knowing the state, which is why ACTION=LockPreset was not
    // good enough.  Never persisted -- see m_bTestingMode in engine.h.
    //
    // Testing mode also raises the settings write shield, so nothing the run
    // changes reaches settings.ini.  A test that needs its changes to persist
    // says so: TESTING_MODE=1,persist.
    const wchar_t* eq = wcschr(sMessage, L'=');
    if (eq) {
      // The suffix can only turn the override ON. Making it settable both ways
      // over the pipe would let one test leave the next one unshielded; the way
      // to make persistence the default is the INI key, which a person sets.
      if (wcsstr(eq, L",persist")) m_bTestingModeWritesSettings = true;
      SetTestingMode(_wtoi(eq + 1) != 0);
    }
    extern PipeServer g_pipeServer;
    wchar_t buf[48];
    FormatTo(buf, L"TESTING_MODE=%d", m_bTestingMode ? 1 : 0);
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"SET_IDLE_TIMER=")) {
    // Enable/disable the idle timer at runtime.  Format: SET_IDLE_TIMER=<0|1>
    // Automated capture/compare runs must not be interrupted by the idle action
    // (fullscreen / stretch / mirror-all) firing after TimeoutMinutes.
    const bool bEnable = _wtoi(sMessage + 15) != 0;
    const bool bWasActivated = m_bIdleActivated;
    m_bIdleTimerEnabled = bEnable;
    SaveIdleTimerSettings();
    // Disabling while the idle action is already on screen would strand it:
    // nothing else undoes the fullscreen/mirrors it applied. Restore first.
    if (!bEnable && bWasActivated)
      PostMessage(GetPluginWindow(), WM_MW_IDLE_RESTORE, 0, 0);
    SendIdleTimerReply();
    DLOG_INFO("Idle timer %s via IPC", bEnable ? "enabled" : "disabled");
    return;
  }
  // Keep the whole SET_IDLE_* family together. MSG_IS is a PREFIX compare, so
  // neighbours matter: SET_IDLE_TIMER= / SET_IDLE_TIMEOUT= and SET_IDLE_ACTIVE=
  // / SET_IDLE_ACTION= each diverge before the '=', so neither shadows the
  // other -- but that is only obvious while they sit next to each other.
  if (MSG_IS(sMessage, L"SET_IDLE_TIMEOUT=")) {
    int mins = _wtoi(sMessage + 17);
    if (mins < 1) mins = 1;
    if (mins > 60) mins = 60;
    m_nIdleTimeoutMinutes = mins;
    SaveIdleTimerSettings();
    SendIdleTimerReply();
    return;
  }
  if (MSG_IS(sMessage, L"SET_IDLE_ACTION=")) {
    int act = _wtoi(sMessage + 16);
    if (act < 0 || act > 3) act = 0;
    m_nIdleAction = act;
    SaveIdleTimerSettings();
    SendIdleTimerReply();
    return;
  }
  if (MSG_IS(sMessage, L"SET_IDLE_PROFILE=")) {
    // An empty value clears it, the way SET_PRESET_SHADER_OVERRIDE= does.
    const wchar_t* name = sMessage + 17;
    while (*name == L' ') name++;
    extern PipeServer g_pipeServer;
    if (!*name) {
      m_szIdleProfile[0] = 0;
      SaveIdleTimerSettings();
      SendIdleTimerReply();
      return;
    }
    // Refused NOW rather than silently at idle time, hours later, with nobody
    // watching: a profile that does not exist is the one failure mode where a
    // walk-away feature gives no feedback at all.
    const std::wstring path = ResolveDisplayProfilePath(name);
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      wchar_t err[MAX_PATH + 32];
      FormatTo(err, L"IDLE_PROFILE=ERROR|%s", name);
      g_pipeServer.Send(err);
      return;
    }
    wcsncpy_s(m_szIdleProfile, name, _TRUNCATE);
    SaveIdleTimerSettings();
    SendIdleTimerReply();
    return;
  }
  if (MSG_IS(sMessage, L"SET_IDLE_FULL_OPACITY=")) {
    m_bIdleFullOpacity = (_wtoi(sMessage + 22) != 0);
    SaveIdleTimerSettings();
    SendIdleTimerReply();
    return;
  }
  if (MSG_IS(sMessage, L"RESET_IDLE_TIMER")) {
    // Exactly what a keypress would do, minus the keypress. The countdown is
    // otherwise read from the SYSTEM-wide last-input tick, which no application
    // can move -- so "restart the countdown" needs a second, local baseline,
    // and IdleElapsedMs takes whichever of the two is shorter.
    //
    // Restoring an ACTIVE idle session is not a side effect, it is the same
    // rule: real input would wake it, so pretending input happened wakes it
    // too. Only when auto-restore is on, for the same reason.
    m_nIdleResetTick = GetTickCount();
    if (m_bIdleAutoRestore && m_bIdleActivated)
      PostMessage(GetPluginWindow(), WM_MW_IDLE_RESTORE, 0, 0);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"IDLE_TIMER_RESET=1");
    return;
  }
  if (MSG_IS(sMessage, L"GET_IDLE_TIMER")) {
    SendIdleTimerReply();
    return;
  }
  // DIAG_GPUTIME=<0|1>[,<ms>] -- per-stage GPU timing (#230).
  //
  // Every other instrument here averages over a frame, and the thing that kills
  // the device is one draw inside one frame: primDt sat flat at 22 ms for 135
  // seconds and then the device was gone. This times each stage on the GPU's
  // own clock and logs only the frames where a stage exceeds the threshold, so
  // it can be left on while waiting for a failure.
  // DIAG_VRAMMARK=<0|1> -- VRAM checkpoints through the allocating paths (#344).
  //
  // Same shape and the same reason as DIAG_GPUTIME above: a diagnostic that
  // costs nothing until asked for, because QueryVideoMemoryInfo is a real call
  // and some of these marks sit in paths that run per surface. The engine's own
  // VRAM sample is refreshed about every two seconds, so it can say the level
  // but never which call moved it.
  // SET_TEXCAP=<0|N> / GET_TEXCAP -- longest edge a disk texture keeps (#349).
  //
  // Takes effect on the NEXT load, not retrospectively: textures already
  // resident were built at whatever cap was in force, and rebuilding them would
  // mean releasing resources a command list may still name. Step a preset to
  // see it, or let the cache evict.
  if (MSG_IS(sMessage, L"SET_TEXCAP=")) {
    extern PipeServer g_pipeServer;
    SetMaxTextureDim(_wtoi(sMessage + 11));
    Config().SetInt(L"Settings", L"MaxTextureDim", GetMaxTextureDim());
    wchar_t out[64];
    FormatTo(out, L"TEXCAP=%d", GetMaxTextureDim());
    g_pipeServer.Send(out);
    DLOG_WARN("texture cap set to %d (0 = none); applies to the next load",
              GetMaxTextureDim());
    return;
  }
  if (MSG_EQ(sMessage, L"GET_TEXCAP")) {
    extern PipeServer g_pipeServer;
    wchar_t out[64];
    FormatTo(out, L"TEXCAP=%d", GetMaxTextureDim());
    g_pipeServer.Send(out);
    return;
  }

  if (MSG_IS(sMessage, L"DIAG_VRAMMARK=")) {
    extern PipeServer g_pipeServer;
    const bool on = (_wtoi(sMessage + 14) != 0);
    VramMarkSetEnabled(on);
    VMARK("DIAG_VRAMMARK (baseline)");
    wchar_t out[64];
    FormatTo(out, L"VRAMMARK=%d", VramMarkEnabled() ? 1 : 0);
    g_pipeServer.Send(out);
    return;
  }

  if (MSG_IS(sMessage, L"DIAG_GPUTIME=")) {
    extern PipeServer g_pipeServer;
    const wchar_t* args = sMessage + 13;
    const bool on = (_wtoi(args) != 0);
    if (m_lpDX) {
      if (const wchar_t* comma = wcschr(args, L',')) {
        const double ms = _wtof(comma + 1);
        if (ms > 0.0) m_lpDX->m_gpuTimingReportMs = ms;
      }
      m_lpDX->EnableGpuStageTiming(on);
      wchar_t out[96];
      FormatTo(out, L"GPUTIME=%d,%.0f",
               m_lpDX->GpuStageTimingOn() ? 1 : 0, m_lpDX->m_gpuTimingReportMs);
      g_pipeServer.Send(out);
    } else {
      g_pipeServer.Send(L"GPUTIME=ERROR|no device");
    }
    return;
  }

  // DIAG_BALANCE[=<0|1>] -- the adaptive frame-rate balancer (#261).
  //
  // Replaces DIAG_GPURISK, which switched the automatic child isolation this
  // superseded. Off exists for the same reason DIAG_MIRRORPACE's does: the only
  // way to show a governor is doing something is to be able to stop it and
  // look. Reports the whole state, because a probe that silently ruled a preset
  // out is otherwise indistinguishable from one that never ran.
  if (MSG_IS(sMessage, L"DIAG_BALANCE")) {
    extern PipeServer g_pipeServer;
    const wchar_t* eq = wcschr(sMessage, L'=');
    if (eq) {
      const bool on = (_wtoi(eq + 1) != 0);
      if (!on && m_bFrameBalanceEnabled)
        ResetFrameRateBalancer();     // give the user's cap back on the way out
      m_bFrameBalanceEnabled = on;
    }
    static const wchar_t* kNames[] = { L"idle", L"probing", L"holding",
                                       L"ruledout" };
    int slowCount = 0;
    const float slowest = SlowestDisplayFps(&slowCount);
    wchar_t out[320];
    FormatTo(out,
             L"BALANCE=%d|state=%ls|main=%.1f|slowest=%.1f|displays=%d"
             L"|ratio=%.2f|cap=%d|userCap=%d|baseline=%.1f"
             // The ladder shares this reply because it shares the cap: one
             // owner, one place to read who has it (#258).
             L"|ladder=%d|rung=%d|starved=%d|stable=%.1f|mirrorCap=%d|mirrorOnly=%d",
             m_bFrameBalanceEnabled ? 1 : 0,
             kNames[(int)m_balanceState],
             GetFps(), slowest, slowCount,
             slowest > 0.0f ? GetFps() / slowest : 0.0f,
             m_balanceAppliedCap, m_balanceUserCap, m_balanceBaseline,
             m_bDistressLadderEnabled ? 1 : 0, m_ladderRung,
             GpuStarved() ? 1 : 0, m_ladderStableFps,
             m_nLadderMirrorCap, m_ladderTookMirrorOnly ? 1 : 0);
    g_pipeServer.Send(out);
    return;
  }

  // SET_LADDER_TRIGGER=<-1|fps> -- where the ladder falls back to copy
  // mirrors (#267).
  //
  // Persisted, and deliberately a SETTING rather than a diagnostic: whether
  // 23/25/31 fps across three displays is acceptable, or ought to collapse to
  // one simulation, is a judgement about the person watching. The derived
  // default is 34 for a high or unset frame cap and two thirds of it below
  // that, and on this machine three independent Clouds1b renders land at ~35
  // achievable -- one frame the right side of it -- so someone who wants that
  // case to fall back needs to say so.
  if (MSG_IS(sMessage, L"SET_LADDER_TRIGGER=")) {
    extern PipeServer g_pipeServer;
    int want = _wtoi(sMessage + 19);
    if (want < -1) want = -1;
    if (want > 720) want = 720;
    m_nLadderTriggerSetting = want;
    SaveDisplayOutputSettings();
    wchar_t out[160];
    FormatTo(out, L"LADDER_TRIGGER=%.1f|setting=%d|derived=%.1f|target=%d",
             MirrorRungTriggerFps(), m_nLadderTriggerSetting,
             DerivedMirrorRungTriggerFps(), UserFpsTarget());
    g_pipeServer.Send(out);
    return;
  }
  if (MSG_IS(sMessage, L"GET_LADDER_TRIGGER")) {
    extern PipeServer g_pipeServer;
    wchar_t out[160];
    FormatTo(out, L"LADDER_TRIGGER=%.1f|setting=%d|derived=%.1f|target=%d",
             MirrorRungTriggerFps(), m_nLadderTriggerSetting,
             DerivedMirrorRungTriggerFps(), UserFpsTarget());
    g_pipeServer.Send(out);
    return;
  }

  // DIAG_LADDER[=<0|1>] -- the distress ladder (#258), separately switchable
  // from the balancer because they answer different questions and a test needs
  // to hold one still while exercising the other.
  if (MSG_IS(sMessage, L"DIAG_LADDER")) {
    extern PipeServer g_pipeServer;
    const wchar_t* eq = wcschr(sMessage, L'=');
    if (eq) {
      const bool on = (_wtoi(eq + 1) != 0);
      if (!on && m_bDistressLadderEnabled) ResetDistressLadder();
      m_bDistressLadderEnabled = on;
    }
    wchar_t out[224];
    FormatTo(out,
             L"LADDER=%d|rung=%d|fps=%.1f|starved=%d|stable=%.1f"
             L"|mirrorCap=%d|mirrorOnly=%d|cap=%d"
             // The triggers are computed from the user's target, so they have
             // to be readable -- otherwise "why did it not fire" is unanswerable
             // from outside.
             L"|target=%d|rung2At=%.1f|rung2Set=%d|fg=%d",
             m_bDistressLadderEnabled ? 1 : 0, m_ladderRung, GetFps(),
             GpuStarved() ? 1 : 0, m_ladderStableFps,
             m_nLadderMirrorCap, m_ladderTookMirrorOnly ? 1 : 0,
             m_balanceAppliedCap,
             UserFpsTarget(), MirrorRungTriggerFps(), m_nLadderTriggerSetting,
             RenderWindowHasFocus() ? 1 : 0);
    g_pipeServer.Send(out);
    return;
  }

  // DIAG_MIRRORPACE=<0|1> -- the closed-loop mirror pacer (#230). Off exists
  // only to demonstrate the device removal it prevents: once it works there is
  // no other way to show the failure is still reachable, and a fix nobody can
  // see failing is a fix nobody can check.
  if (MSG_IS(sMessage, L"DIAG_MIRRORPACE=")) {
    extern PipeServer g_pipeServer;
    m_bMirrorPacer.store(_wtoi(sMessage + 16) != 0, std::memory_order_relaxed);
    wchar_t out[64];
    FormatTo(out, L"MIRRORPACE=%d,skipped=%u",
             m_bMirrorPacer.load(std::memory_order_relaxed) ? 1 : 0,
             m_mirrorDiagSkipFrames.load(std::memory_order_relaxed));
    g_pipeServer.Send(out);
    return;
  }

  // DIAG_MIRRORCAP=<n> -- the mirror swap-chain long-edge cap; 0 removes it.
  //
  // Adjustable because its justification is in doubt (forgejo#252). The comment
  // on CapMirrorSwapChainDim attributes it to a TDR; Shane's reading is that
  // the device loss was a memory leak since fixed. Neither can be settled by
  // argument, and the only way to find out whether a guard still earns its
  // place is to remove it and watch -- the same reason DIAG_MIRRORPACE can be
  // switched off.
  //
  // Marks both resize paths dirty so the change takes effect on live chains
  // rather than only on the next one created.
  if (MSG_IS(sMessage, L"DIAG_MIRRORCAP=")) {
    extern PipeServer g_pipeServer;
    const int want = _wtoi(sMessage + 15);
    // -1 hands control back to the derivation; anything else is a diagnostic
    // override that outranks it until then. Not persisted either way -- an
    // evaluation must not turn into a setting by accident (#254).
    if (want < 0) {
      m_bMirrorScCapOverridden = false;
      RefreshMirrorSwapChainCap();
    } else {
      m_bMirrorScCapOverridden = true;
      SetMirrorSwapChainCap(want);
    }
    m_bMirrorStylesDirty.store(true);
    m_bMirrorIndepSizeDirty.store(true);
    wchar_t out[200];
    FormatTo(out,
             L"MIRRORCAP=%d|source=%ls|setting=%d|derived=%d|budgetMB=%.0f|share=%.0f%%",
             GetMirrorSwapChainCap(),
             m_bMirrorScCapOverridden ? L"diag"
               : (m_nMirrorScCapSetting >= 0 ? L"setting" : L"derived"),
             m_nMirrorScCapSetting,
             DeriveMirrorSwapChainCap(),
             (double)m_vramBudgetBytes / (1024.0 * 1024.0),
             MirrorVramShare() * 100.0);
    g_pipeServer.Send(out);
    return;
  }

  // SET_MIRRORCAP=<n> -- the PERSISTED setting (#254).
  //
  // -1 derive from the VRAM budget (the default), 0 no cap, >0 a fixed long
  // edge for a machine under real contention. Separate verb from
  // DIAG_MIRRORCAP because they answer different questions: this one is a
  // choice that survives a restart, that one is an experiment that does not.
  if (MSG_IS(sMessage, L"SET_MIRRORCAP=")) {
    extern PipeServer g_pipeServer;
    int want = _wtoi(sMessage + 14);
    if (want < -1) want = -1;
    if (want > 16384) want = 16384;
    m_nMirrorScCapSetting = want;
    m_bMirrorScCapOverridden = false;   // an explicit choice clears the probe
    RefreshMirrorSwapChainCap();
    SaveDisplayOutputSettings();
    m_bMirrorStylesDirty.store(true);
    m_bMirrorIndepSizeDirty.store(true);
    wchar_t out[96];
    FormatTo(out, L"MIRRORCAP=%d|setting=%d", GetMirrorSwapChainCap(), want);
    g_pipeServer.Send(out);
    return;
  }

  if (MSG_IS(sMessage, L"DIAG_SIMULATE_TDR")) {
    // Drive the TDR relaunch path on demand (#368), so the backoff, the
    // relaunch, the successor's wait and the three-relaunch bound can be
    // exercised without resetting the display driver for the whole machine.
    //
    // TESTING MODE ONLY. This verb makes the app exit and start a replacement,
    // which is exactly what nobody wants arriving down a pipe by accident.
    extern PipeServer g_pipeServer;
    if (!m_bTestingMode) {
      g_pipeServer.Send(L"SIMULATE_TDR|ok=0|err=not_testing|detail=TESTING_MODE=1 first");
      return;
    }
    g_pipeServer.Send(L"SIMULATE_TDR|ok=1");
    extern void MDropSimulateGpuFatal();
    MDropSimulateGpuFatal();
    return;
  }

  if (MSG_IS(sMessage, L"DIAG_MIRRORS")) {
    // Rich mirror diagnostics for IPC / MCP (portrait ghost, every-other-frame, path).
    // path: 0=none 1=orient 2=stretchMain 3=letterbox 4=copy 5=black 6=orientFail
    extern PipeServer g_pipeServer;
    std::wstring response = L"MIRRORS|active=";
    response += m_bMirrorsActive ? L"1" : L"0";
    response += L"|independent=";
    response += m_bMirrorIndependentDefault ? L"1" : L"0";
    response += L"|aot=";
    response += m_bAlwaysOnTop ? L"1" : L"0";
    response += L"|watermark=";
    response += m_bMirrorWatermarkActive ? L"1" : L"0";
    // #184: does the sim context hold its OWN compiled preset, or is it still
    // borrowing the engine's?
    //
    // Without this the two are indistinguishable from outside. The mirror
    // renders the SAME preset either way, so a compile that silently failed --
    // or a fallback that never stopped being taken -- produces a picture
    // identical to the working case, and every render comparison passes while
    // the feature does nothing. That is not hypothetical: the fallback exists
    // for the frames before first adoption and looks exactly like failure.
    // #186: which preset SOURCE the context is on, and what it chose.
    //
    // Same argument as ctxShaders below: a mirror that silently ignored its
    // configuration and followed the primary looks exactly like one that was
    // never configured, so "it showed the primary's preset" cannot distinguish
    // working-as-asked from not-reading-the-config.
    // One row per surface (#186 phase 4), and now the ONLY place per-surface
    // facts are reported. ctxSource, ctxPreset and ctxShaders used to sit here
    // describing m_mirrorSurfaces.front() -- kept that way "because the
    // harness parses them", which is how a diagnostic ends up lying to the
    // thing that reads it. They are own=, preset= and shaders= on each row
    // below (#202).
    for (auto& sp : SnapshotMirrorSurfaces()) {
      std::wstring name = sp->sim.loadedOwnPath;
      const size_t sep = name.find_last_of(L"\\/");
      if (sep != std::wstring::npos)
        name = name.substr(sep + 1);
      const double pfv =
          (sp->sim.pState && sp->sim.pState->var_pf_frame)
              ? *sp->sim.pState->var_pf_frame : -1.0;
      // WHICH PRESET IS ACTUALLY ON THE GLASS, which until now nothing reported
      // (#351). preset= names the surface's OWN preset, and that stays true of
      // its configuration even while something else is being drawn -- so during
      // the #351 bug this row asserted "01 - Martin - blue haze.milk" for three
      // straight phases while the primary's Shadertoy was on screen, and the
      // frame counter advanced the whole time. The one diagnostic that exists
      // for this said nothing was wrong.
      //
      // Reads the same predicate the worker dispatches on, so it cannot drift
      // from what is actually drawn. Now that #351 is fixed an own-preset
      // surface reports "own" whatever the primary is doing, and a mirror
      // reports "primary_milk3" while the primary is on a .milk3 -- which for a
      // mirror is correct, since it IS rendering the primary's preset.
      //
      // Names the PIPELINE, not the ownership -- own= already says whether the
      // surface has a preset of its own, and "own"/"primary_milk3" read as a
      // second opinion on that. An independent mirror with no preset of its own
      // legitimately renders classic while the primary is on a .milk, and
      // "renders=own" for it was actively misleading.
      const wchar_t* renders = SurfaceUsesMilk3(*sp) ? L"milk3" : L"classic";
      wchar_t row[832];
      FormatTo(row,
               L"|surf=%ls,own=%d,preset=%ls,renders=%ls"
               L",size=%dx%d,ready=%d,frames=%d,pub=%d"
               L",gate=%d,pipeRdy=%d,need=%dx%d,seenPf=%u,try=%d,rec=%d"
               L",shaders=%d,fb=%d,simFps=%.1f,pf=%.0f,ovr=%d,simTime=%.3f"
               // workUs is what the GPU last charged for ONE frame of this
               // surface, submit to fence complete (#230). It is the input the
               // closed-loop pacer throttles on, so without it the pacer's
               // decisions cannot be checked from outside -- which is the
               // mistake skipFrames= made for so long.
               // psInv / prims / overdraw answer the question workUs cannot:
               // whether an expensive frame was expensive per PIXEL or per
               // FRAGMENT. overdraw is PSInvocations divided by this surface's
               // own pixel count, so ~1 means each pixel was shaded about once
               // and the shader is the cost, while a large number means the
               // warp mesh is covering the canvas repeatedly and the geometry
               // is what went wrong. Those need opposite fixes and nothing
               // else here distinguishes them (#230).
               // sub= and done= are this surface's OWN fence: what it has
               // submitted, and what the GPU has signalled complete. The
               // fence gate (gate=4) refuses to record a new frame until
               // done >= sub, and a surface frozen there is the shape seen in
               // #230 -- so these two numbers turn "its fence never signals"
               // from an inference into a reading. done < sub, both static,
               // means the submitted work is not completing.
               L",bindBase=%u,parityMissed=%u,workUs=%u,sub=%llu,done=%llu"
               L",prims=%llu,psInv=%llu,overdraw=%.1f,gpuUs=%u",
               sp->device.empty() ? L"(none)" : sp->device.c_str(),
               sp->sim.ownPresetPath.empty() ? 0 : 1,
               name.empty() ? L"-" : name.c_str(),
               renders,
               sp->pipe.w, sp->pipe.h,
               sp->imageReady.load() ? 1 : 0,
               sp->pipe.frames,
               sp->publishedIdx.load(),
               sp->lastGate.load(),
               sp->pipe.ready ? 1 : 0,
               sp->needW.load(), sp->needH.load(),
               sp->seenPrimaryFrame,
               sp->stepAttempts.load(), sp->stepRecords.load(),
               (sp->sim.shaders.warp.bytecodeBlob ||
                sp->sim.shaders.comp.bytecodeBlob) ? 1 : 0,
               sp->pipe.fbIdx,
               sp->sim.fFps,
               pfv,
               sp->sim.diagShaderFrame,
               sp->sim.fTime,
               sp->pipe.bindBase,
               sp->parityMissed.load(std::memory_order_relaxed),
               sp->lastWorkUs,
               (unsigned long long)sp->workSubmitted,
               (unsigned long long)(sp->workFence
                                        ? sp->workFence->GetCompletedValue()
                                        : 0ull),
               (unsigned long long)sp->lastStats.CPrimitives,
               (unsigned long long)sp->lastStats.PSInvocations,
               (sp->pipe.w > 0 && sp->pipe.h > 0)
                   ? (double)sp->lastStats.PSInvocations /
                     ((double)sp->pipe.w * (double)sp->pipe.h)
                   : 0.0,
               sp->lastGpuUs);
      response += row;
    }
    {
      wchar_t gbuf[1024];
      FormatTo(gbuf,
        L"|main=%dx%d,mainPort=%d,needSrv=%d,canSample=%d,anyOpp=%d,anyIndepMilk3=%d,"
        L"shadertoy=%d,compPso=%d,slots=%d,frame=%u,skipFrames=%u,"
        L"allocHr=0x%08X,listHr=0x%08X,auxUsed=%u,"
        // surfaces= is a count; orientFps/mirrorFps are aggregates over every
        // surface. The per-surface orient=/orientReady=/orientFrames=/orientFb=
        // that used to be here are size=, pipeRdy=, frames= and fb= on each
        // surf= row (#202).
        L"surfaces=%d,"
        // workerCmdList counts GetActiveCmdList() calls made ON A MIRROR
        // WORKER -- every one is a site that had a RenderContext and asked a
        // global anyway. It is the progress measure for #206: it should fall
        // to 0 as the drawing helpers are moved onto their context, and any
        // rise afterwards is a new site doing the old thing.
        // peakGpuUs is the value the isolation decision actually reads --
        // the worst GPU cost any surface has reported for the CURRENT preset.
        // Without it the only visible figure was each surface's LAST frame,
        // which is a different quantity and reads far lower, so the threshold
        // could not be checked from outside.
        L"orientFps=%.1f,mirrorFps=%.1f,maxFps=%d,peakGpuUs=%u,aspBad=%u,aspGood=%u"
        L",workerCmdList=%u,bindOverlap=%d"
        L"|pace_ms=orientDt(min=%.1f,avg=%.1f,max=%.1f),lock(avg=%.1f,max=%.1f),"
        L"rec(avg=%.1f,max=%.1f),sim(avg=%.1f,max=%.1f),fenceMiss=%d,"
        L"fenceWait(avg=%.2f),"
        L"primDt(min=%.1f,avg=%.1f,max=%.1f)"
        // simFps is the ctx's OWN measured rate — the number the HUD's "mirror"
        // line shows and the one presets read as var_pf_fps. It was derived from
        // the primary's animation clock and so always echoed the primary's fps;
        // exposing it here is what makes that verifiable instead of eyeballed.
        // Shadertoy iMouse as the shaders see it (primary target pixels).
        // What WaitForSurfaceWork cost on the render thread, in microseconds:
        // last call, worst since start, and how many calls actually waited.
        L"|surfWait=last=%d,max=%d,n=%u,calls=%u"
        L"|stMouse=%.0f,%.0f,down=%d",
        m_mirrorDiagMainW, m_mirrorDiagMainH, m_mirrorDiagMainPortrait,
        m_mirrorDiagNeedMainSrv, m_mirrorDiagCanSampleMain,
        m_mirrorDiagAnyOpposite, m_mirrorDiagAnyIndepMilk3,
        m_mirrorDiagShadertoy, m_mirrorDiagCompPso, m_mirrorDiagSlotCount,
        m_mirrorDiagFrameCounter,
        m_mirrorDiagSkipFrames.load(std::memory_order_relaxed),
        m_mirrorDiagAllocHr, m_mirrorDiagListHr, m_mirrorDiagAuxUsed,
        // surfaces= is the count of per-display mirror surfaces (#186 phase 3).
        // The orient* fields that follow describe the FIRST of them; phase 4
        // adds a row per surface, when they stop rendering the same preset.
        (int)MirrorSurfaceCount(),
        m_fOrientFps, m_fMirrorPresentFps, m_nMirrorMaxFps.load(),
        // MeasuredPeakGpuUs, not the raw surface high-water mark: the gate
        // reads the primary's frame too, and a diagnostic that showed only
        // half of that would make the decision unverifiable from outside --
        // which is the whole reason this field was added (#245).
        MeasuredPeakGpuUs(),
        m_diagOrientAspectBad.load(), m_diagOrientAspectGood.load(),
        m_lpDX ? m_lpDX->ActiveCmdListOnWorkerCount() : 0u,
        // bindOverlap: pairs of live surfaces whose reserved SRV ranges
        // intersect. Non-zero is ALWAYS a bug and is the thing nothing
        // reported during #354 -- the two bindBase values were both printed,
        // 11 apart, and that only reads as wrong if you know the reservation
        // is 576 wide. This does the comparison so a reader does not have to.
        OrientBindOverlapCount(),
        m_diagOrientDtMinUs.load() / 1000.0, m_diagOrientDtAvgUs.load() / 1000.0,
        m_diagOrientDtMaxUs.load() / 1000.0,
        m_diagOrientLockAvgUs.load() / 1000.0, m_diagOrientLockMaxUs.load() / 1000.0,
        m_diagOrientRecAvgUs.load() / 1000.0, m_diagOrientRecMaxUs.load() / 1000.0,
        m_diagOrientSimAvgUs.load() / 1000.0, m_diagOrientSimMaxUs.load() / 1000.0,
        m_diagOrientFenceWaits.load(),
        m_diagOrientFenceMsAvg.load() / 1000.0,
        m_diagPrimDtMinUs.load() / 1000.0, m_diagPrimDtAvgUs.load() / 1000.0,
        m_diagPrimDtMaxUs.load() / 1000.0,
        m_diagSurfaceWaitUs.load(), m_diagSurfaceWaitMaxUs.load(),
        m_diagSurfaceWaitCount.load(), m_diagSurfaceWaitCalls.load(),
        m_stMouseX, m_stMouseY, m_stMouseDown ? 1 : 0);
      response += gbuf;
    }
    {
      // What `frame` each surface actually published, so #111 stays measurable.
      //
      // These are EXPECTED to differ. An independent mirror simulates the
      // preset for itself and keeps its own counter -- that is the mode, and a
      // user wanting an exact copy of the primary picks a stretch/copy path
      // instead. So this is not a parity check.
      //
      // What must agree is pfFrame and ovrFrame: the first is read back from
      // the mirror context's own per-frame EEL variable, the second is what
      // ApplyShaderParams gave the HLSL `frame` uniform. Those used to be the
      // context's counter and the PRIMARY's respectively -- one preset seeing
      // two different values for `frame` in a single frame.
      //
      // They can differ by one: pfFrame is read live while ovrFrame was latched
      // during the last record, and the context steps in between. A reader
      // should allow that skew; the defect this catches is a difference of
      // thousands, not of one.
      // The PRIMARY's frame and clock only. Each surface's own pf=, ovr= and
      // simTime= are on its surf= row, where the comparison above is actually
      // made per surface -- reading them off m_mirrorSurfaces.front() while
      // sitting next to a primFrame that describes the instance is exactly the
      // mismatch that let #198 hide (#202).
      wchar_t fbuf[128];
      FormatTo(fbuf, L"|primFrame=%d|primTime=%.3f",
                 GetFrame(), (double)GetTime());
      response += fbuf;
    }
    // Report which monitor the render window is on
    if (m_lpDX && m_lpDX->GetHwnd()) {
      HMONITOR hMon = MonitorFromWindow(m_lpDX->GetHwnd(), MONITOR_DEFAULTTONEAREST);
      if (hMon) {
        MONITORINFOEXW mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
          response += L"|render_on=";
          response += mi.szDevice;
          // Whether the render window is actually on screen, and whether it
          // has finished arriving. A child built hidden on its own display is
          // on the right monitor from the first instant, so "is it on target"
          // stopped being enough for the parent to decide the mirror could
          // stand down -- it would hand the display over to a window nobody
          // can see (forgejo#57).
          //
          // revealed= is the one the parent waits on: a child fades in over the
          // mirror, so visible= goes true at the START of the fade and retiring
          // the mirror then would show the desktop through it. Always 1 for
          // anything that is not a fading child.
          response += IsWindowVisible(m_lpDX->GetHwnd()) ? L",visible=1"
                                                         : L",visible=0";
          response += m_bChildRevealComplete.load() ? L",revealed=1"
                                                    : L",revealed=0";
          RECT wr;
          GetWindowRect(m_lpDX->GetHwnd(), &wr);
          wchar_t buf[128];
          FormatTo(buf, L",renderwin=(%d,%d)-(%d,%d) %dx%d",
            wr.left, wr.top, wr.right, wr.bottom,
            wr.right - wr.left, wr.bottom - wr.top);
          response += buf;
          FormatTo(buf, L",opacity=%.2f,fs=%d",
            fOpacity, IsBorderlessFullscreen(m_lpDX->GetHwnd()) ? 1 : 0);
          response += buf;
          LONG_PTR exStyle = GetWindowLongPtr(m_lpDX->GetHwnd(), GWL_EXSTYLE);
          response += (exStyle & WS_EX_TRANSPARENT) ? L",clickthru=1" : L",clickthru=0";
        }
      }
    }
    auto stateName = [](D3D12_RESOURCE_STATES st) -> const wchar_t* {
      if (st == D3D12_RESOURCE_STATE_COMMON) return L"COMMON";
      if (st == D3D12_RESOURCE_STATE_PRESENT) return L"PRESENT";
      if (st == D3D12_RESOURCE_STATE_RENDER_TARGET) return L"RT";
      if (st == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return L"SRV";
      if (st == D3D12_RESOURCE_STATE_COPY_DEST) return L"COPY_DST";
      if (st == D3D12_RESOURCE_STATE_COPY_SOURCE) return L"COPY_SRC";
      return L"OTHER";
    };
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
      // #193. A remote wants a SWITCH for these two, and a switch has to show
      // a position. Neither was readable per display, so a client had three
      // bad options: infer watermark from clickthru && opacity<100, which also
      // reports true for anyone who set those two by hand; remember what it
      // last sent, which lies the moment anything changes at the PC; or start
      // the switch in the wrong place. They arrive with the state the screen
      // already polls, so a client needs no second round trip and cannot show
      // a switch that disagrees with the opacity slider beside it.
      //
      // Scope is deliberately not disguised. Watermark covers every mirror at
      // once and always-on-top is instance-wide, so both repeat the same value
      // on every record. That is still the truthful answer to "is THIS
      // display's window watermarked / on top", which is what a switch shows.
      // What the SETTER covers is a different question, and its own reply
      // already answers it: SET_DISPLAY_WATERMARK refuses a non-primary
      // display with no_per_display_watermark.
      response += L",watermark=";
      // THIS display's own state, not the global gesture's. It echoed
      // m_bMirrorWatermarkActive, so every monitor reported the same value and
      // the field could not answer "is display 3 watermarked" -- which is the
      // only question anyone asks it.
      response += out.config.bWatermark ? L"1" : L"0";
      // Read off the WINDOW where there is one, rather than echoing the flag
      // that requested it. A flag says what was asked for; the ex-style says
      // what is on screen, and those parting company without anyone noticing
      // is the failure this field exists to prevent. A display with no window
      // has no z-order of its own, so it reports the instance's intent.
      response += L",alwaysontop=";
      {
        bool aot = m_bAlwaysOnTop;
        if (out.monitorState && out.monitorState->hWnd)
          aot = (GetWindowLongPtrW(out.monitorState->hWnd, GWL_EXSTYLE)
                 & WS_EX_TOPMOST) != 0;
        response += aot ? L"1" : L"0";
      }
      response += L",independent=";
      response += out.config.bIndependentRender ? L"1" : L"0";
      response += L",skipped=";
      response += out.bSkippedSameMonitor ? L"1" : L"0";
      // ownProcess is what was asked for; childOwns is whether the child is
      // actually up and therefore whether the mirror has stood down. The two
      // differ for the seconds a child takes to spawn, and telling them apart
      // is the difference between "starting" and "stuck" for any client.
      response += L",ownProcess=";
      response += out.config.bOwnProcess ? L"1" : L"0";
      response += L",childOwns=";
      response += ChildOwnsDisplay(out.config.szDeviceName) ? L"1" : L"0";
      auto& rc = out.config.rcMonitor;
      const int monW = rc.right - rc.left;
      const int monH = rc.bottom - rc.top;
      const bool monPortrait = monH > monW;
      wchar_t rcBuf[256];
      FormatTo(rcBuf, L",display=(%d,%d)-(%d,%d) %dx%d,portrait=%d",
        rc.left, rc.top, rc.right, rc.bottom, monW, monH, monPortrait ? 1 : 0);
      response += rcBuf;
      if (out.monitorState) {
        auto& ms = *out.monitorState;
        response += L",ready=";
        response += ms.bReady ? L"1" : L"0";
        response += L",softDis=";
        response += ms.bSoftDisabled ? L"1" : L"0";
        wchar_t hwndBuf[32];
        FormatTo(hwndBuf, L",hwnd=%p", (void*)ms.hWnd);
        response += hwndBuf;
        FormatTo(rcBuf, L",swapsize=%dx%d,bufs=%u,rtvBase=%u",
          ms.width, ms.height, ms.bufferCount, ms.rtvSlotBase);
        response += rcBuf;
        if (ms.hWnd) {
          RECT wr;
          GetWindowRect(ms.hWnd, &wr);
          FormatTo(rcBuf, L",winrect=(%d,%d)-(%d,%d) %dx%d",
            wr.left, wr.top, wr.right, wr.bottom,
            wr.right - wr.left, wr.bottom - wr.top);
          response += rcBuf;
          response += IsWindowVisible(ms.hWnd) ? L",visible=1" : L",visible=0";
          LONG_PTR ex = GetWindowLongPtrW(ms.hWnd, GWL_EXSTYLE);
          response += (ex & WS_EX_LAYERED) ? L",layered=1" : L",layered=0";
        }
        // Draw/present path (updated every mirror frame)
        const wchar_t* pathNames[] = {
          L"none", L"orient", L"stretchMain", L"letterbox", L"copy", L"black", L"orientFail"
        };
        const int pathIdx = (ms.lastPath >= 0 && ms.lastPath <= 6) ? ms.lastPath : 0;
        FormatTo(rcBuf,
          L",path=%s,drawFI=%u,presFI=%u,presHr=0x%08X,"
          L"ok=%u,fail=%u,skip=%u,draws=%u,"
          L"mustBlock=%d,oppIndep=%d,wipe=%d,paintMask=0x%X,"
          L"everPres=%d,bb0=%s,bb1=%s,bb2=%s",
          pathNames[pathIdx],
          ms.lastDrawFI, ms.lastPresentFI, (unsigned)ms.lastPresentHr,
          ms.presentOkCount, ms.presentFailCount, ms.presentSkipCount, ms.drawCount,
          ms.lastMustBlock ? 1 : 0, ms.lastOppositeIndep ? 1 : 0,
          ms.bNeedsFullChainClear ? 1 : 0, ms.paintedBufferMask,
          ms.bEverPresented ? 1 : 0,
          stateName(ms.bbState[0]), stateName(ms.bbState[1]), stateName(ms.bbState[2]));
        response += rcBuf;
      } else {
        response += L",state=none";
      }
      idx++;
    }
    g_pipeServer.Send(response.c_str());
    return;
  }
  if (MSG_IS(sMessage, L"DIAG_BINDINGS")) {
    extern PipeServer g_pipeServer;
    // Side-by-side dump of the SRV binding slots the PRIMARY and the MIRROR
    // each built from the same CShaderParams this frame. A disk-texture
    // register showing bind=4294967295 (UINT_MAX) resolved to nothing and was
    // silently pointed at m_fallbackTexture — invisible in every other log.
    // texcode: 0=DISK 1=VS 2=FEEDBACK 3=IMGFB 4=AUDIO 5..7=BUF_B/C/D 8..=BLUR
    std::wstring response = L"BINDINGS";
    wchar_t buf[512];
    if (m_lpDX) {
      // orientBind..orientBindEnd is the mirror's reserved block range: if any
      // slot= value below falls inside it, the per-frame fills are overwriting
      // that texture's own descriptor (see EnsureOrientPipeline's stillReserved).
      // Was 6 * BINDING_BLOCK_SIZE = 192, which under-reported the reserved
      // range by 384 slots: the reservation has been (6 + blur) blocks since
      // #209. So orientBindEnd said a texture at bindBase+300 was OUTSIDE the
      // block when it was squarely inside it -- the precise question this
      // field exists to answer (#354).
      const UINT bindSlots = OrientBindSlots();
      FormatTo(buf, L"|fallbackSrv=%u,nullSrv=%u,style=%d,srvUsed=%u,"
                      L"orientBind=%u,orientBindEnd=%u",
                 m_lpDX->m_fallbackTexture.srvIndex,
                 m_lpDX->m_nullTexture.srvIndex,
                 m_lpDX->m_nFallbackTexStyle,
                 m_lpDX->m_nextFreeSrvSlot,
                 OnlyMirrorSurface().pipe.bindBase,
                 OnlyMirrorSurface().pipe.bindBase == UINT_MAX ? UINT_MAX
                                                   : OnlyMirrorSurface().pipe.bindBase + bindSlots);
      response += buf;
      response += L",file=";
      response += m_lpDX->m_szFallbackCustomFile[0] ? m_lpDX->m_szFallbackCustomFile : L"(none)";
    }
    static const wchar_t* kPassNames[] = { L"warp", L"comp", L"oldWarp", L"oldComp" };
    auto dumpSet = [&](const wchar_t* who, const BindSnapshot* snaps) {
      for (int p = 0; p < BINDSNAP_COUNT; p++) {
        const BindSnapshot& s = snaps[p];
        response += L"|";
        response += who;
        response += L".";
        response += kPassNames[p];
        if (!s.valid) {
          response += L"=none";
          continue;
        }
        response += L"=";
        // Registers 0-7 always (shaders bind low t-registers); past that only
        // those carrying a real texture, to keep the line readable.
        for (int i = 0; i < 32; i++) {
          if (i >= 8 && s.texcode[i] == 0 && s.bindingSrv[i] == UINT_MAX)
            continue;
          FormatTo(buf, L"t%d:c=%d,slot=%u,bind=%u;",
                     i, s.texcode[i], s.slots[i], s.bindingSrv[i]);
          response += buf;
        }
      }
    };
    dumpSet(L"primary", m_bindSnapPrimary);
    // Every surface, named by its device. The old single "mirror" set was one
    // array all of them wrote, so with more than one surface this report was a
    // shuffle of several -- which made it useless for exactly the multi-surface
    // case it exists to diagnose (#206).
    for (const auto& sp : SnapshotMirrorSurfaces()) {
      std::wstring who = L"mirror";
      if (!sp->device.empty()) {
        who += L"[";
        who += sp->device;
        who += L"]";
      }
      dumpSet(who.c_str(), sp->bindSnap);
    }
    g_pipeServer.Send(response.c_str());
    return;
  }
  if (MSG_IS(sMessage, L"GET_FALLBACK_TEX")) {
    extern PipeServer g_pipeServer;
    wchar_t buf[MAX_PATH + 128];
    FormatTo(buf, L"FALLBACK_TEX=%d|%s|srv=%u",
               m_nFallbackTexStyle,
               m_szFallbackTexFile[0] ? m_szFallbackTexFile : L"(none)",
               m_lpDX ? m_lpDX->m_fallbackTexture.srvIndex : UINT_MAX);
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"SET_FALLBACK_TEX=")) {
    extern PipeServer g_pipeServer;
    // SET_FALLBACK_TEX=<style>[|<path>]
    //   0=Hue Gradient  1=White  2=Black
    //   3=Random from RandomTexDir  4=Random from resources\textures
    //   5=Custom file (path required, or the previously saved one is reused)
    std::wstring value(sMessage + 17);
    std::wstring path;
    size_t bar = value.find(L'|');
    if (bar != std::wstring::npos) {
      path = value.substr(bar + 1);
      value = value.substr(0, bar);
    }
    const int style = _wtoi(value.c_str());
    if (style < 0 || style > 5) {
      g_pipeServer.Send(L"SET_FALLBACK_TEX_RESULT=ERR|style must be 0-5");
    } else {
      m_nFallbackTexStyle = style;
      Config().SetInt(L"Milkwave", L"FallbackTexStyle", style);
      if (!path.empty()) {
        CopyTo(m_szFallbackTexFile, path.c_str());
        SaveFallbackPaths();
      }
      m_nPendingFallbackStyle = style;
      CopyTo(m_szPendingFallbackFile, m_szFallbackTexFile);
      m_bFallbackTexRefreshPending.store(true, std::memory_order_release);
      wchar_t buf[MAX_PATH + 128];
      FormatTo(buf, L"SET_FALLBACK_TEX_RESULT=OK|style=%d|%s",
                 style, m_szFallbackTexFile[0] ? m_szFallbackTexFile : L"(none)");
      g_pipeServer.Send(buf);
    }
    return;
  }
  if (MSG_IS(sMessage, L"SET_MIRROR_WIPE")) {
    // Re-arm full flip-chain wipe on all mirrors (+ optional force-reinit).
    // SET_MIRROR_WIPE       — paint-mask clear only (next frames redraw every face)
    // SET_MIRROR_WIPE=1     — same
    // SET_MIRROR_WIPE=reinit — destroy/recreate SCs (heavy)
    // Indices 15/16, not 14/15. "SET_MIRROR_WIPE" is FIFTEEN characters, so
    // sMessage[14] is the trailing 'E' and never '=' -- the reinit form has
    // therefore never once fired, and sMessage+15 handed "=reinit" to a
    // comparison against "reinit". The prefix compare above was 14 too, which
    // is what hid it; MSG_IS now takes the length from the literal.
    const bool reinit = (sMessage[15] == L'=' &&
      (_wcsicmp(sMessage + 16, L"reinit") == 0 || _wtoi(sMessage + 16) == 2));
    for (auto& out : m_displayOutputs) {
      if (!out.monitorState) continue;
      out.monitorState->bNeedsFullChainClear = true;
      out.monitorState->paintedBufferMask = 0;
      out.monitorState->presentOkCount = 0;
      out.monitorState->presentFailCount = 0;
      out.monitorState->presentSkipCount = 0;
    }
    if (reinit)
      m_bMirrorForceReinit.store(true);
    m_bRaiseMirrorsNextFrame.store(true);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(reinit ? L"MIRROR_WIPE=reinit" : L"MIRROR_WIPE=1");
    DLOG_INFO("SET_MIRROR_WIPE reinit=%d", reinit ? 1 : 0);
    return;
  }
  if (MSG_IS(sMessage, L"SET_MIRROR_OPACITY=")) {
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
        FormatTo(target, L"\\\\.\\DISPLAY%d", displayNum);
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
      FormatTo(buf, L"MIRROR_OPACITY=%d,%d", displayNum, val);
    else
      FormatTo(buf, L"MIRROR_OPACITY=%d", val);
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"SET_MIRROR_CLICKTHRU=")) {
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
    FormatTo(buf, L"MIRROR_CLICKTHRU=%d", val ? 1 : 0);
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"SET_MIRROR_INDEPENDENT=")) {
    // Absolute set independent per-display render. Format: SET_MIRROR_INDEPENDENT=<0|1>
    // Optional per-display: SET_MIRROR_INDEPENDENT=<N>,<0|1>  (DISPLAY N by device name)
    const wchar_t* args = sMessage + 23;
    const wchar_t* comma = wcschr(args, L',');
    int displayNum = -1;
    int val;
    if (comma) {
      displayNum = _wtoi(args);
      val = _wtoi(comma + 1);
    } else {
      val = _wtoi(args);
    }
    const bool enable = (val != 0);
    if (displayNum > 0) {
      wchar_t target[32];
      FormatTo(target, L"\\\\.\\DISPLAY%d", displayNum);
      for (auto& out : m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        if (wcscmp(out.config.szDeviceName, target) != 0) continue;
        out.config.bIndependentRender = enable;
      }
      SaveDisplayOutputSettings();
      // Soft wipe only — do not destroy swap chains (black mirrors until Ctrl+S)
      for (auto& out : m_displayOutputs) {
        if (!out.monitorState) continue;
        out.monitorState->bNeedsFullChainClear = true;
        out.monitorState->paintedBufferMask = 0;
      }
      m_bRaiseMirrorsNextFrame.store(true);
      RefreshDisplaysTab();
    } else {
      SetMirrorIndependentRender(enable);
    }
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    if (displayNum > 0)
      FormatTo(buf, L"MIRROR_INDEPENDENT=%d,%d", displayNum, enable ? 1 : 0);
    else
      FormatTo(buf, L"MIRROR_INDEPENDENT=%d", m_bMirrorIndependentDefault ? 1 : 0);
    g_pipeServer.Send(buf);
    return;
  }
  else if (MSG_IS(sMessage, L"GET_MIRROR_INDEPENDENT") ||
           wcscmp(sMessage, L"MIRROR_INDEPENDENT") == 0) {
    // GET_MIRROR_INDEPENDENT → report; bare MIRROR_INDEPENDENT → toggle then report
    if (wcscmp(sMessage, L"MIRROR_INDEPENDENT") == 0)
      ToggleMirrorIndependentRender();
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    FormatTo(buf, L"MIRROR_INDEPENDENT=%d", m_bMirrorIndependentDefault ? 1 : 0);
    g_pipeServer.Send(buf);
  }
  if (MSG_IS(sMessage, L"SET_MIRROR_MAXFPS=")) {
    // Independent-mirror worker rate cap. Format: SET_MIRROR_MAXFPS=<fps>
    // 0 = parity (free-run); else clamped to 5-240. Reply: MIRROR_MAXFPS=<fps>
    int fps = _wtoi(sMessage + 18);
    if (fps < 0) fps = 0;
    if (fps > 0 && fps < 5) fps = 5;
    if (fps > 240) fps = 240;
    m_nMirrorMaxFps.store(fps);
    SaveDisplayOutputSettings();
    RefreshDisplaysTab(); // sync dropdown if the Displays window is open
    extern PipeServer g_pipeServer;
    wchar_t buf[48];
    FormatTo(buf, L"MIRROR_MAXFPS=%d", fps);
    g_pipeServer.Send(buf);
    DLOG_INFO("Mirror max FPS set to %d via IPC", fps);
    return;
  }
  if (MSG_IS(sMessage, L"SET_FPS=")) {
    // The PRIMARY window's frame cap. 0 = unlimited; otherwise clamped to
    // 5-720 to match the fps_caps.h table's range. Reply: FPSCAP=<fps>
    //
    // 49dd80e1 gave the MIRRORS a settable+readable cap and stopped there, so
    // the primary's cap stayed reachable only from F3 and the Visual window.
    // That asymmetry is not cosmetic: a measurement harness could pin MD3 PRO
    // at 60 (its ini has MaxFPS) but not this app, so every cross-engine A/B
    // ran MDropDX12 uncapped at ~66fps against MD3's 60 -- and a feedback loop
    // with decay<1 accumulates more energy per second at the higher rate, so
    // the comparison read as "MDropDX12 is brighter" when it was "MDropDX12
    // drew more frames".
    int fps = _wtoi(sMessage + 8);
    if (fps < 0) fps = 0;
    if (fps > 0 && fps < 5) fps = 5;
    if (fps > 720) fps = 720;
    // Posted, not called: SetFPSCap touches the ini and the Visual window.
    HWND hRender = GetPluginWindow();
    if (hRender) PostMessage(hRender, WM_MW_SET_FPS_CAP, (WPARAM)fps, 0);
    extern PipeServer g_pipeServer;
    wchar_t buf[48];
    FormatTo(buf, L"FPSCAP=%d", fps);
    g_pipeServer.Send(buf);
    DLOG_INFO("Primary FPS cap set to %d via IPC", fps);
    return;
  }
  if (MSG_IS(sMessage, L"GET_FPS")) {
    // Query only. Reply: FPSCAP=<fps> (0 = unlimited)
    extern PipeServer g_pipeServer;
    wchar_t buf[48];
    FormatTo(buf, L"FPSCAP=%d", m_max_fps_w);
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"GET_MIRROR_MAXFPS")) {
    // Query only. Reply: MIRROR_MAXFPS=<fps> (0 = parity)
    extern PipeServer g_pipeServer;
    wchar_t buf[48];
    FormatTo(buf, L"MIRROR_MAXFPS=%d", m_nMirrorMaxFps.load());
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"SET_ALWAYS_ON_TOP=")) {
    // Format: SET_ALWAYS_ON_TOP=<0|1>
    bool want = (_wtoi(sMessage + 18) != 0);
    if (m_bAlwaysOnTop != want) {
      m_bAlwaysOnTop = want;
      ToggleAlwaysOnTop(GetPluginWindow());
      SaveSettingToINI(SET_ALWAYS_ON_TOP);
    }
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    FormatTo(buf, L"ALWAYS_ON_TOP=%d", m_bAlwaysOnTop ? 1 : 0);
    g_pipeServer.Send(buf);
    return;
  }
  else if (MSG_IS(sMessage, L"GET_ALWAYS_ON_TOP") ||
           wcscmp(sMessage, L"ALWAYS_ON_TOP") == 0) {
    if (wcscmp(sMessage, L"ALWAYS_ON_TOP") == 0) {
      m_bAlwaysOnTop = !m_bAlwaysOnTop;
      ToggleAlwaysOnTop(GetPluginWindow());
      SaveSettingToINI(SET_ALWAYS_ON_TOP);
      AddNotification(m_bAlwaysOnTop ? L"Always On Top: ON" : L"Always On Top: OFF");
    }
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    FormatTo(buf, L"ALWAYS_ON_TOP=%d", m_bAlwaysOnTop ? 1 : 0);
    g_pipeServer.Send(buf);
  }
  if (MSG_IS(sMessage, L"SET_MIRROR_ENABLED=")) {
    // Enable/disable a monitor output. Format:
    //   SET_MIRROR_ENABLED=<0|1>         (all non-primary monitors)
    //   SET_MIRROR_ENABLED=<N>,<0|1>     (DISPLAY N)
    const wchar_t* args = sMessage + 19;
    const wchar_t* comma = wcschr(args, L',');
    int displayNum = -1;
    int val;
    if (comma) {
      displayNum = _wtoi(args);
      val = _wtoi(comma + 1);
    } else {
      val = _wtoi(args);
    }
    const bool enable = (val != 0);
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      if (displayNum > 0) {
        wchar_t target[32];
        FormatTo(target, L"\\\\.\\DISPLAY%d", displayNum);
        if (wcscmp(out.config.szDeviceName, target) != 0) continue;
      }
      out.config.bEnabled = enable;
    }
    SaveDisplayOutputSettings();
    RefreshDisplaysTab();
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    if (displayNum > 0)
      FormatTo(buf, L"MIRROR_ENABLED=%d,%d", displayNum, enable ? 1 : 0);
    else
      FormatTo(buf, L"MIRROR_ENABLED=%d", enable ? 1 : 0);
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"MOVE_TO_DISPLAY=")) {
    // Move render window to the centre of a display, addressed by DEVICE
    // NUMBER -- the N in \\.\DISPLAYN -- matching every other display
    // command (SET_MIRROR_*, SET_DISPLAY_*, DISPLAY_NEXT=/PREV=, all of
    // which build \\.\DISPLAY%d and match szDeviceName against it).
    //
    // Used to take a 1-based ENUMERATION POSITION instead, which agrees
    // with the device number only when every \\.\DISPLAYN is a contiguous
    // 1..n -- diverges the moment a monitor is missing or Windows has not
    // renumbered after a rearrange, and the mismatch failed with no reply
    // at all: a client had no way to tell "moved" from "nothing happened"
    // (forgejo#37).
    int n = _wtoi(sMessage + 16);
    wchar_t target[32];
    FormatTo(target, L"\\\\.\\DISPLAY%d", n);
    extern PipeServer g_pipeServer;
    bool found = false;
    for (auto& out : m_displayOutputs) {
      if (out.config.type != DisplayOutputType::Monitor) continue;
      if (wcscmp(out.config.szDeviceName, target) != 0) continue;
      found = true;
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
        FormatTo(buf, L"MOVED_TO=%ls", out.config.szDeviceName);
        g_pipeServer.Send(buf);
      }
      break;
    }
    if (!found) {
      wchar_t buf[64];
      FormatTo(buf, L"MOVE_TO_DISPLAY_ERR=no such display %d", n);
      g_pipeServer.Send(buf);
    }
    return;
  }
  if (MSG_IS(sMessage, L"SET_WINDOW=")) {
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
        FormatTo(buf, L"WINDOW=(%d,%d)-(%d,%d) %dx%d",
            wr.left, wr.top, wr.right, wr.bottom,
            wr.right - wr.left, wr.bottom - wr.top);
        g_pipeServer.Send(buf);
      }
    }
    return;
  }
  if (MSG_IS(sMessage, L"GET_LOGLEVEL")) {
    // Query current log level.
    const wchar_t* names[] = { L"Off", L"Error", L"Warn", L"Info", L"Verbose" };
    int lvl = g_debugLogLevel;
    if (lvl < 0) lvl = 0; if (lvl > 4) lvl = 4;
    wchar_t buf[512];
    FormatTo(buf, L"LOGLEVEL=%d|%s|LOGDIR=%s", lvl, names[lvl], DebugLogGetDir());
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"CLEAR_LOGS")) {
    // Delete all files in the log/ directory and re-open debug.log
    DebugLogClearAll();
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"LOGS_CLEARED");
    return;
  }
  if (MSG_IS(sMessage, L"DUMP_SHADER")) {
    // Dump current shader source texts to diag files for debugging
    extern PipeServer g_pipeServer;
    int nDumped = 0;
    auto dumpOne = [&](const char* text, const wchar_t* diagName, const char* label) {
      if (!text || !text[0]) return;
      FILE* f = DebugLogDiagOpen(diagName, L"w");
      if (!f) return;
      fprintf(f, "// DIAG: DUMP_SHADER %s preset=%ls shadertoy=%d hasA=%d\n",
              label, m_pState ? m_pState->m_szDesc : L"?",
              m_bShadertoyMode ? 1 : 0, m_bHasBufferA ? 1 : 0);
      fputs(text, f);
      fclose(f);
      nDumped++;
    };
    if (m_pState) {
      dumpOne(m_pState->m_szCompShadersText, L"diag_comp_shader.txt", "comp/image");
      dumpOne(m_pState->m_szWarpShadersText, L"diag_warp_shader.txt", "warp");
      dumpOne(m_pState->m_szBufferAShadersText, L"diag_bufferA_shader.txt", "bufferA");
      dumpOne(m_pState->m_szBufferBShadersText, L"diag_bufferB_shader.txt", "bufferB");
      dumpOne(m_pState->m_szBufferCShadersText, L"diag_bufferC_shader.txt", "bufferC");
      dumpOne(m_pState->m_szBufferDShadersText, L"diag_bufferD_shader.txt", "bufferD");
    }
    wchar_t resp[256];
    FormatTo(resp, L"DUMP_SHADER|files=%d|dir=%s", nDumped, DebugLogGetDir());
    g_pipeServer.Send(resp);
    return;
  }
  if (MSG_IS(sMessage, L"SHUTDOWN")) {
    // Clean shutdown via WM_CLOSE — saves settings, stops render thread, exits
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(L"SHUTTING_DOWN");
    PostMessage(GetPluginWindow(), WM_CLOSE, 0, 0);
    return;
  }
  if (MSG_IS(sMessage, L"DIAG_DISPLAY_MODE=")) {
    // Toggle diagnostic display mode: 0=normal, 1=show VS[0] raw, 2=show VS[1] raw
    int mode = _wtoi(sMessage + 18);
    m_nDiagDisplayMode = (mode >= 0 && mode <= 2) ? mode : 0;
    extern PipeServer g_pipeServer;
    wchar_t resp[64];
    FormatTo(resp, L"DIAG_DISPLAY_MODE=%d", m_nDiagDisplayMode);
    g_pipeServer.Send(resp);
    DLOG_INFO("DIAG_DISPLAY_MODE set to %d", m_nDiagDisplayMode);
    return;
  }
  if (MSG_IS(sMessage, L"SET_AUDIO_GAIN=")) {
    float val = (float)_wtof(sMessage + 15);
    if (val <= 0.0f) val = 1.0f;
    if (val > 256.0f) val = 256.0f;
    m_fAudioSensitivity = val;
    mdropdx12_audio_sensitivity = val;
    SaveSettingToINI(SET_AUDIO_SENSITIVITY);
    wchar_t buf[128];
    FormatTo(buf, L"AUDIO_GAIN=%.2f|effective=%.2f", m_fAudioSensitivity, mdropdx12_audio_sensitivity);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"GET_AUDIO_GAIN")) {
    wchar_t buf[128];
    FormatTo(buf, L"AUDIO_GAIN=%.2f|effective=%.2f", m_fAudioSensitivity, mdropdx12_audio_sensitivity);
    extern PipeServer g_pipeServer;
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"SET_DEVICE_VOLUME=")) {
    float vol = (float)_wtof(sMessage + 18);
    HRESULT hr = SetDeviceVolume(m_szAudioDevice, m_nAudioDeviceRequestType, vol);
    wchar_t buf[128];
    if (SUCCEEDED(hr)) {
      float curVol = 0; BOOL muted = FALSE;
      GetDeviceVolume(m_szAudioDevice, m_nAudioDeviceRequestType, &curVol, &muted);
      FormatTo(buf, L"DEVICE_VOLUME=%.2f|muted=%d", curVol, (int)muted);
    } else {
      FormatTo(buf, L"DEVICE_VOLUME_ERROR=0x%08x", (unsigned)hr);
    }
    SendMessageToMDropDX12Remote(buf, true);
    return;
  }
  if (MSG_IS(sMessage, L"GET_DEVICE_VOLUME")) {
    float curVol = 0; BOOL muted = FALSE;
    HRESULT hr = GetDeviceVolume(m_szAudioDevice, m_nAudioDeviceRequestType, &curVol, &muted);
    wchar_t buf[128];
    if (SUCCEEDED(hr))
      FormatTo(buf, L"DEVICE_VOLUME=%.2f|muted=%d", curVol, (int)muted);
    else
      FormatTo(buf, L"DEVICE_VOLUME_ERROR=0x%08x", (unsigned)hr);
    SendMessageToMDropDX12Remote(buf, true);
    return;
  }
  if (MSG_IS(sMessage, L"SET_DEVICE_MUTE=")) {
    int mute = _wtoi(sMessage + 16);
    HRESULT hr = SetDeviceMute(m_szAudioDevice, m_nAudioDeviceRequestType, mute ? TRUE : FALSE);
    wchar_t buf[128];
    if (SUCCEEDED(hr))
      FormatTo(buf, L"DEVICE_MUTE=%d", mute ? 1 : 0);
    else
      FormatTo(buf, L"DEVICE_MUTE_ERROR=0x%08x", (unsigned)hr);
    SendMessageToMDropDX12Remote(buf, true);
    return;
  }
  if (MSG_IS(sMessage, L"TOGGLE_DEVICE_MUTE")) {
    float curVol = 0; BOOL muted = FALSE;
    HRESULT hr = GetDeviceVolume(m_szAudioDevice, m_nAudioDeviceRequestType, &curVol, &muted);
    if (SUCCEEDED(hr))
      hr = SetDeviceMute(m_szAudioDevice, m_nAudioDeviceRequestType, muted ? FALSE : TRUE);
    wchar_t buf[128];
    if (SUCCEEDED(hr))
      FormatTo(buf, L"DEVICE_MUTE=%d", muted ? 0 : 1);
    else
      FormatTo(buf, L"DEVICE_MUTE_ERROR=0x%08x", (unsigned)hr);
    SendMessageToMDropDX12Remote(buf, true);
    return;
  }
  if (MSG_IS(sMessage, L"GET_RENDER_DIAG")) {
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
    FormatTo(buf,
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
    return;
  }
  if (MSG_IS(sMessage, L"TEST_EEL_ISOLATION")) {
    // Self-test for per-context EEL storage (independent mirror sims).
    // VMs A and B get their own reg00-99 + GRAM blocks; a write through one
    // must not appear in the other or in the legacy process-global regs.
    // Reply: EEL_ISOLATION=PASS or EEL_ISOLATION=FAIL,<detail>
    extern PipeServer g_pipeServer;
    std::wstring fail;
    double regA[100] = {}, regB[100] = {};
    void* gramA = nullptr; void* gramB = nullptr;
    NSEEL_VMCTX vmA = NSEEL_VM_alloc();
    NSEEL_VMCTX vmB = NSEEL_VM_alloc();
    NSEEL_CODEHANDLE hA = nullptr, hB = nullptr, hA2 = nullptr, hB2 = nullptr, hC = nullptr;
    const double globalReg55Before = NSEEL_getglobalregs()[55];
    const double globalReg01Before = NSEEL_getglobalregs()[1];
    if (!vmA || !vmB) {
      fail = L"vm alloc failed";
    } else {
      NSEEL_VM_SetRegBase(vmA, regA);
      NSEEL_VM_SetGRAM(vmA, &gramA);
      NSEEL_VM_SetRegBase(vmB, regB);
      NSEEL_VM_SetGRAM(vmB, &gramB);
      double* aOut1 = NSEEL_VM_regvar(vmA, "y1");
      double* aOut2 = NSEEL_VM_regvar(vmA, "y2");
      double* bOut1 = NSEEL_VM_regvar(vmB, "y1");
      double* bOut2 = NSEEL_VM_regvar(vmB, "y2");
      hA = NSEEL_code_compile(vmA, (char*)"reg01=7;gmegabuf(3)=11;", 0);
      hB = NSEEL_code_compile(vmB, (char*)"reg01=20;gmegabuf(3)=40;", 0);
      hA2 = NSEEL_code_compile(vmA, (char*)"y1=reg01;y2=gmegabuf(3);", 0);
      hB2 = NSEEL_code_compile(vmB, (char*)"y1=reg01;y2=gmegabuf(3);", 0);
      if (!hA || !hB || !hA2 || !hB2) {
        fail = L"compile failed";
      } else {
        NSEEL_code_execute(hA);
        NSEEL_code_execute(hB);
        NSEEL_code_execute(hA2);
        NSEEL_code_execute(hB2);
        wchar_t d[160];
        if (!aOut1 || !aOut2 || !bOut1 || !bOut2) {
          fail = L"regvar failed";
        } else if (*aOut1 != 7.0 || *aOut2 != 11.0) {
          FormatTo(d, L"A read %.3g/%.3g want 7/11", *aOut1, *aOut2);
          fail = d;
        } else if (*bOut1 != 20.0 || *bOut2 != 40.0) {
          FormatTo(d, L"B read %.3g/%.3g want 20/40", *bOut1, *bOut2);
          fail = d;
        } else if (regA[1] != 7.0 || regB[1] != 20.0) {
          FormatTo(d, L"blocks regA[1]=%.3g regB[1]=%.3g", regA[1], regB[1]);
          fail = d;
        } else if (NSEEL_getglobalregs()[1] != globalReg01Before) {
          fail = L"leak into legacy reg01";
        }
      }
      // Legacy path still works: a base-less VM writes the shared regs.
      if (fail.empty()) {
        NSEEL_VMCTX vmC = NSEEL_VM_alloc();
        if (vmC) {
          hC = NSEEL_code_compile(vmC, (char*)"reg55=123;", 0);
          if (hC) NSEEL_code_execute(hC);
          if (!hC || NSEEL_getglobalregs()[55] != 123.0)
            fail = L"legacy reg path broken";
          NSEEL_getglobalregs()[55] = globalReg55Before; // restore
          if (hC) NSEEL_code_free(hC);
          NSEEL_VM_free(vmC);
        }
      }
    }
    if (hA) NSEEL_code_free(hA);
    if (hB) NSEEL_code_free(hB);
    if (hA2) NSEEL_code_free(hA2);
    if (hB2) NSEEL_code_free(hB2);
    if (vmA) NSEEL_VM_free(vmA);
    if (vmB) NSEEL_VM_free(vmB);
    NSEEL_VM_FreeGRAM(&gramA);
    NSEEL_VM_FreeGRAM(&gramB);
    std::wstring reply = fail.empty() ? L"EEL_ISOLATION=PASS"
                                      : (L"EEL_ISOLATION=FAIL," + fail);
    g_pipeServer.Send(reply.c_str());
    return;
  }
  if (MSG_IS(sMessage, L"GET_EEL_STATE")) {
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
        FormatTo(tmp, L"%.6g", globalRegs[reg_start + i]);
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
        FormatTo(tmp, L"%.6g", ptr ? (double)*ptr : 0.0);
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
        FormatTo(tmp, L"%.6g", val);
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
    return;
  }
  if (MSG_IS(sMessage, L"GET_AUDIO_DIAG")) {
    // gain/amp globals come from audio_capture.h; only the PCM buffer needs a
    // local extern (it is not declared in any header)
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
    FormatTo(buf,
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
    return;
  }
  if (MSG_IS(sMessage, L"DEAUTH_DEVICE|")) {
    extern TcpServer g_tcpServer;
    extern PipeServer g_pipeServer;
    std::string deviceId = WideToUTF8(sMessage + 14);
    g_tcpServer.RemoveAuthorizedDevice(deviceId);
    g_tcpServer.DisconnectDevice(deviceId);
    g_tcpServer.SaveAuthorizedDevices(GetConfigIniFile());
    g_pipeServer.Send(L"DEAUTH_OK");
    return;
  }
  if (wcscmp(sMessage, L"GET_AUDIO_DEVICES") == 0) {
    // Enumerate active audio render devices using WASAPI
    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (SUCCEEDED(hr) && pEnumerator) {
      IMMDeviceCollection* pCollection = nullptr;
      hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
      if (SUCCEEDED(hr) && pCollection) {
        UINT count = 0;
        pCollection->GetCount(&count);

        // Get active device name for comparison
        std::wstring activeName;
        if (m_szAudioDevice[0]) {
          activeName = m_szAudioDevice;
        } else {
          // Default device — resolve its friendly name
          IMMDevice* pDefault = nullptr;
          if (SUCCEEDED(GetDefaultLoopbackDevice(&pDefault, activeName)) && pDefault)
            pDefault->Release();
        }

        std::wstring response = L"AUDIO_DEVICES|count=" + std::to_wstring(count);
        for (UINT i = 0; i < count; i++) {
          IMMDevice* pDevice = nullptr;
          if (SUCCEEDED(pCollection->Item(i, &pDevice)) && pDevice) {
            IPropertyStore* pProps = nullptr;
            if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
              PROPVARIANT varName;
              PropVariantInit(&varName);
              if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                response += L"|dev" + std::to_wstring(i) + L"=" + varName.pwszVal;
              }
              PropVariantClear(&varName);
              pProps->Release();
            }
            pDevice->Release();
          }
        }
        response += L"|active=" + activeName;
        SendMessageToMDropDX12Remote(response.c_str(), true);
        pCollection->Release();
      }
      pEnumerator->Release();
    }
    if (!pEnumerator || FAILED(hr)) {
      // Send empty response so client knows the request was processed
      SendMessageToMDropDX12Remote(L"AUDIO_DEVICES|count=0", true);
    }
    return;
  }
  if (wcscmp(sMessage, L"LIST_DEVICES") == 0) {
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
    return;
  }
  if (MSG_IS(sMessage, L"TOGGLE_MESSAGES")) {
    SetSpriteMessagesMode(m_nSpriteMessagesMode ^ 1); // toggle messages bit
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    FormatTo(buf, L"MESSAGES=%d", MessagesEnabled() ? 1 : 0);
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"SET_MESSAGES=")) {
    int val = _wtoi(sMessage + 13);
    SetSpriteMessagesMode(val ? (m_nSpriteMessagesMode | 1)   // enable messages bit
                              : (m_nSpriteMessagesMode & ~1)); // disable messages bit
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    FormatTo(buf, L"MESSAGES=%d", MessagesEnabled() ? 1 : 0);
    g_pipeServer.Send(buf);
    return;
  }
  if (MSG_IS(sMessage, L"GET_MESSAGES")) {
    extern PipeServer g_pipeServer;
    wchar_t buf[64];
    FormatTo(buf, L"MESSAGES=%d", MessagesEnabled() ? 1 : 0);
    g_pipeServer.Send(buf);
    return;
  }
  // ── Driving a SCRIPT FILE over the pipe (#303) ──
  //
  // The script engine could be driven only from its tool window, so a script
  // could not be started by a harness -- which meant the script-as-test feature
  // could not be regression tested, and a support script could not be run
  // remotely for someone.
  //
  // Deliberately three verbs and not one: loading and playing are separate acts
  // in the UI too, and a caller wanting to inspect what parsed before anything
  // executes needs the gap.
  if (MSG_IS(sMessage, L"SCRIPT_LOAD=") || MSG_EQ(sMessage, L"SCRIPT_PLAY") ||
      MSG_EQ(sMessage, L"SCRIPT_STOP")) {
    extern PipeServer g_pipeServer;
  if (MSG_IS(sMessage, L"SCRIPT_LOAD=")) {
    std::wstring path(sMessage + 12);
    LoadScript(path.c_str());
    wchar_t reply[MAX_PATH + 64];
    FormatTo(reply, L"SCRIPT_LOADED=%d|%ls",
             (int)m_script.lines.size(), path.c_str());
    g_pipeServer.Send(reply);
    return;
  }
  if (MSG_EQ(sMessage, L"SCRIPT_PLAY")) {
    StartScript();
    g_pipeServer.Send(L"SCRIPT_PLAYING");
    return;
  }
  if (MSG_EQ(sMessage, L"SCRIPT_STOP")) {
    StopScript();
    wchar_t reply[64];
    FormatTo(reply, L"SCRIPT_STOPPED|errors=%d", m_script.errors);
    g_pipeServer.Send(reply);
    return;
  }
  }

  // Fallback -- reached only when nothing above claimed the message.
  //
  // Treated as a pipe-chained script command (NEXT, PREV, LOCK, SEND=0x.., ...),
  // which unifies IPC and button board dispatch and is what Milkwave Remote
  // depends on. It is NOT a failure path: most of the vocabulary arrives here.
  //
  // Unless this connection asked for STRICT (issue 102). Then an unmatched
  // keyword is a caller error and is reported as one, because the alternative
  // is worse than silence: an unrecognised script line is DRAWN ON THE
  // VISUALIZER, so a typo from a harness contaminates the very frame it was
  // about to capture. -child and -test start strict for that reason -- their
  // only callers are a parent process and a test harness, neither of which is
  // served by having its mistake rendered.
  // Strictness is passed DOWN rather than acted on here: most of the working
  // vocabulary lives in the script engine, so "the dispatcher did not match it"
  // is not the same as "nothing recognises it". See ExecuteScriptCommand.
  ExecuteScriptLine(sMessage, bStrict);
}

void Engine::SendPresetChangedInfoToMDropDX12Remote() {
  std::wstring msg = L"PRESET=" + std::wstring(m_szCurrentPresetFile);
  SendMessageToMDropDX12Remote(msg.c_str(), true);
  SendPresetWaveInfoToMDropDX12Remote();
}

void Engine::SendTrackInfoToMDropDX12Remote() {
  // The member pointer, as the rest of this file uses (see :3272). It is set to
  // &mdropdx12 in App.cpp, so the function-local `extern MDropDX12 mdropdx12;`
  // that used to be here named the same object -- but it shadowed the member,
  // so this one function reached the global directly while its neighbours went
  // through the pointer. Guarded because the assignment happens during startup.
  // Guarded because the pointer is assigned during startup (App.cpp), and
  // this used to reach the global object directly and so could never fail.
  // Logged rather than silent: turning "always sends" into "sometimes sends
  // nothing" is exactly the kind of change that is invisible until someone
  // wonders why the remote has no track info.
  if (!mdropdx12) {
    DLOG_WARN("SendTrackInfoToMDropDX12Remote: mdropdx12 not yet bound, track info not sent");
    return;
  }
  std::wstring msg = L"TRACK|artist=" + mdropdx12->currentArtist
                   + L"|title=" + mdropdx12->currentTitle
                   + L"|album=" + mdropdx12->currentAlbum;
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
    + L"|TESTING=" + std::wstring(m_bTestingMode ? L"1" : L"0")
    // 0 = unlimited. Without this a caller can set the cap but never read it,
    // which is what forced the A/B harness to edit settings.ini instead.
    + L"|FPSCAP=" + std::to_wstring(m_max_fps_w)
    + L"|FFTATTACK=" + std::to_wstring(m_fFFTAttackGlobal)
    + L"|FFTDECAY=" + std::to_wstring(m_fFFTDecayGlobal)
    // A client that can set the preset cycle but never read it can only flip
    // and hope, and two clients desynchronise on the first flip (forgejo#22).
    + L"|ORDER=" + std::wstring(m_bSequentialPresetOrder ? L"1" : L"0")
    + L"|INTERVAL=" + std::to_wstring(m_fTimeBetweenPresets)
    + L"|INTERVALRAND=" + std::to_wstring(m_fTimeBetweenPresetsRand);
  SendMessageToMDropDX12Remote(msg.c_str(), true);
}

void Engine::SendPresetCycleInfo() {
  extern PipeServer g_pipeServer;
  wchar_t buf[128];
  FormatTo(buf, L"PRESET_CYCLE=%d,%d,%.3f,%.3f",
             m_bSequentialPresetOrder ? 1 : 0,
             m_bPresetLockedByUser ? 1 : 0,
             m_fTimeBetweenPresets,
             m_fTimeBetweenPresetsRand);
  g_pipeServer.Send(buf);
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

int Engine::GetNextFreeSupertextIndex(int ownerDisplay) {
  int index = 0;
  for (int i = 0; i < NUM_SUPERTEXTS; i++) {
    if (m_supertexts[i].fStartTime == -1.0f) {
      index = i;
      break;
    }
  }
  // if no text is free, we'll reset and use index=0
  m_supertexts[index] = td_supertext(); // Reset the supertext at this index
  // Stamped HERE, right after the reset that would otherwise clear it, so
  // every spawn path gets it without each having to remember (#255).
  m_supertexts[index].nOwnerDisplay = ownerDisplay;
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
    if (m_audioProfile.bandEnergy == BandEnergy::Mean && end_bin > start_bin)
      mysound.imm[i] /= (float)(end_bin - start_bin + 1);
    mysound.imm[i] /= m_audioProfile.bandNormalise[i];
  }

  // MilkDrop 3's analysis is already computed, every frame, into m_sound by
  // EngineShell::AnalyzeNewSound -- octave-spaced bands, mean energy, the
  // empirical divisors, a 14 fps rate reference. Nothing to recompute here:
  // fold its two channels to the mono this struct carries and let the shared
  // imm_rel work below run on it. m_sound is per-channel, mysound is not.
  const bool bMilkDropBands = (m_audioProfile.bandMode == BandMode::MilkDrop);
  if (bMilkDropBands) {
    for (int i = 0; i < 3; i++) {
      mysound.imm[i]      = 0.5f * (m_sound.imm[0][i]      + m_sound.imm[1][i]);
      mysound.avg[i]      = 0.5f * (m_sound.avg[0][i]      + m_sound.avg[1][i]);
      mysound.long_avg[i] = 0.5f * (m_sound.long_avg[0][i] + m_sound.long_avg[1][i]);
    }
  }

  int recentBufferSize = (int)GetFps();

  // do temporal blending to create attenuated and super-attenuated versions
  for (int i = 0; i < 3; i++) {
    // Skipped in MilkDrop mode: AnalyzeNewSound already blended at MilkDrop's
    // own rates, and blending again here would put two filters in series.
    if (!bMilkDropBands) {
      float rate;

      if (mysound.imm[i] > mysound.avg[i])
        rate = m_audioProfile.avgAttack;
      else
        rate = m_audioProfile.avgDecay;
      rate = AdjustRateToFPS(rate, m_audioProfile.fpsRef, GetFps());
      mysound.avg[i] = mysound.avg[i] * rate + mysound.imm[i] * (1 - rate);

      if (GetFrame() < 50)
        rate = 0.9f;
      else
        rate = m_audioProfile.longMix;
      rate = AdjustRateToFPS(rate, m_audioProfile.fpsRef, GetFps());
      mysound.long_avg[i] = mysound.long_avg[i] * rate + mysound.imm[i] * (1 - rate);
    }

    // also get bass/mid/treble levels *relative to the past*
    //changed all the values to 0 instead of 1 when it's no music
    //
    // NOTE: MilkDrop substitutes 1.0 here (plugin.cpp:8821), and swapping this
    // back to 1.0 was tried on the theory that a bass-driven sprite alpha
    // (`a = 0.25*bass` in Rainbow Butterfly1/2) was collapsing to zero. It was
    // NOT the cause: a render-level A/B showed the sprite contributing either
    // way, and measuring bass/mid/treb through a comp shader gives 0.518 /
    // 1.131 / 1.005 here against MD3's 0.368 / 0.383 / 0.398 -- neither engine
    // reads zero, so this guard is not even firing. Reverted rather than change
    // audio behaviour on a disproven hypothesis.
    if (fabsf(mysound.long_avg[i]) < 0.001f)
      mysound.imm_rel[i] = m_audioProfile.silenceValue;
    else
      mysound.imm_rel[i] = mysound.imm[i] / mysound.long_avg[i];

    if (fabsf(mysound.long_avg[i]) < 0.001f)
      mysound.avg_rel[i] = m_audioProfile.silenceValue;
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
    return PresetNameAt(idx).c_str();
  return L"";
}

//----------------------------------------------------------------------


} // namespace mdrop
