/*
  Plugin module: Settings Window, Resource Viewer & Configuration
  Extracted from engine.cpp for maintainability.
  Contains: Settings window management, controls, theme, resource viewer,
            settings config, user defaults, fallback paths
*/

#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "AutoCharFn.h"
#include "shell_defines.h"
#include "wasabi.h"
#include <assert.h>
#include <strsafe.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <Windows.h>
#include <cstdint>
#include <sstream>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include "../audio/log.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#include <set>
#include "Milkdrop2PcmVisualizer.h"

namespace mdrop {

extern Engine g_engine;
extern int ToggleFPSNumPressed;

const wchar_t* SETTINGS_WND_CLASS = L"MDropDX12SettingsWnd";
bool g_bSettingsWndClassRegistered = false;

void Engine::WriteRealtimeConfig() {
  // WritePrivateProfileIntW(m_bShowSongTitle, L"bShowSongTitle", GetConfigIniFile(), L"Settings");
  // WritePrivateProfileIntW(m_bShowSongTime, L"bShowSongTime", GetConfigIniFile(), L"Settings");
  // WritePrivateProfileIntW(m_bShowSongLen, L"bShowSongLen", GetConfigIniFile(), L"Settings");
}

// Get the current value of a setting as a display string
void Engine::GetSettingValueString(int id, wchar_t* buf, int bufLen) {
  switch (id) {
  case SET_PRESET_DIR:       lstrcpynW(buf, m_szPresetDir, bufLen); break;
  case SET_AUDIO_DEVICE:     lstrcpynW(buf, m_szAudioDevice, bufLen); break;
  case SET_AUDIO_SENSITIVITY: swprintf(buf, m_fAudioSensitivity == -1.0f ? L"Auto" : L"%.1f", m_fAudioSensitivity); break;
  case SET_BLEND_TIME:       swprintf(buf, L"%.1f s", m_fBlendTimeAuto); break;
  case SET_TIME_BETWEEN:     swprintf(buf, L"%.0f s", m_fTimeBetweenPresets); break;
  case SET_HARD_CUTS:        lstrcpyW(buf, m_bHardCutsDisabled ? L"yes" : L"no"); break;
  case SET_PRESET_LOCK:      lstrcpyW(buf, m_bPresetLockOnAtStartup ? L"on" : L"off"); break;
  case SET_SEQ_ORDER:        lstrcpyW(buf, m_bSequentialPresetOrder ? L"on" : L"off"); break;
  case SET_SONG_TITLE_ANIMS: lstrcpyW(buf, m_bSongTitleAnims ? L"on" : L"off"); break;
  case SET_CHANGE_WITH_SONG: lstrcpyW(buf, m_ChangePresetWithSong ? L"on" : L"off"); break;
  case SET_SHOW_FPS:         lstrcpyW(buf, m_bShowFPS ? L"on" : L"off"); break;
  case SET_ALWAYS_ON_TOP:    lstrcpyW(buf, m_bAlwaysOnTop ? L"on" : L"off"); break;
  case SET_BORDERLESS:       lstrcpyW(buf, m_WindowBorderless ? L"on" : L"off"); break;
  case SET_SPOUT:            lstrcpyW(buf, bSpoutOut ? L"on" : L"off"); break;
  case SET_SPRITES_MESSAGES: {
    const wchar_t* labels[] = { L"Off", L"Messages", L"Sprites", L"Messages & Sprites" };
    lstrcpyW(buf, labels[m_nSpriteMessagesMode & 3]);
    break;
  }
  default: buf[0] = 0; break;
  }
}

// Get the hint text for a setting
const wchar_t* Engine::GetSettingHint(int id) {
  SettingType t = g_settingsDesc[id].type;
  if (t == ST_PATH)     return L"ENTER: browse";
  if (t == ST_BOOL)     return L"ENTER: toggle";
  if (t == ST_FLOAT || t == ST_INT) return L"LEFT/RIGHT: adjust";
  return L"";
}

// Toggle or adjust a setting, save to INI
void Engine::ToggleSetting(int id) {
  bool* pBool = NULL;
  switch (id) {
  case SET_HARD_CUTS:        pBool = &m_bHardCutsDisabled; break;
  case SET_PRESET_LOCK:      pBool = &m_bPresetLockOnAtStartup; break;
  case SET_SEQ_ORDER:        pBool = &m_bSequentialPresetOrder; break;
  case SET_SONG_TITLE_ANIMS: pBool = &m_bSongTitleAnims; break;
  case SET_CHANGE_WITH_SONG: pBool = &m_ChangePresetWithSong; break;
  case SET_SHOW_FPS:         pBool = &m_bShowFPS; break;
  case SET_ALWAYS_ON_TOP:    pBool = &m_bAlwaysOnTop; break;
  case SET_BORDERLESS:       pBool = &m_WindowBorderless; break;
  case SET_SPOUT:            pBool = &bSpoutOut; break;
  case SET_SPRITES_MESSAGES:
    m_nSpriteMessagesMode = (m_nSpriteMessagesMode + 1) & 3;
    SaveSettingToINI(SET_SPRITES_MESSAGES);
    return;
  default: return;
  }
  *pBool = !(*pBool);
  SaveSettingToINI(id);

  // Side effects
  if (id == SET_ALWAYS_ON_TOP)
    ToggleAlwaysOnTop(GetPluginWindow());
}

void Engine::AdjustSetting(int id, int direction) {
  SettingDesc& s = g_settingsDesc[id];
  float* pFloat = NULL;
  switch (id) {
  case SET_AUDIO_SENSITIVITY: pFloat = &m_fAudioSensitivity; break;
  case SET_BLEND_TIME:        pFloat = &m_fBlendTimeAuto; break;
  case SET_TIME_BETWEEN:      pFloat = &m_fTimeBetweenPresets; break;
  default: return;
  }
  *pFloat += s.fStep * direction;
  if (*pFloat < s.fMin) *pFloat = s.fMin;
  if (*pFloat > s.fMax) *pFloat = s.fMax;
  if (id == SET_AUDIO_SENSITIVITY) {
    // Snap past the unusable range between -1 and 0.5
    if (m_fAudioSensitivity > -1.0f && m_fAudioSensitivity < 0.5f)
      m_fAudioSensitivity = (direction > 0) ? 0.5f : -1.0f;
    if (m_fAudioSensitivity == -1.0f) {
      mdropdx12_audio_adaptive = true;
    } else {
      mdropdx12_audio_adaptive = false;
      mdropdx12_audio_sensitivity = m_fAudioSensitivity;
    }
  }
  SaveSettingToINI(id);
}

void Engine::SaveSettingToINI(int id) {
  SettingDesc& s = g_settingsDesc[id];
  if (!s.iniSection || !s.iniKey) return;
  wchar_t val[MAX_PATH];
  GetSettingValueString(id, val, MAX_PATH);
  // For float values, write the raw number (not the display string with "s")
  switch (id) {
  case SET_AUDIO_SENSITIVITY: swprintf(val, L"%g", (double)m_fAudioSensitivity); break;
  case SET_BLEND_TIME:        swprintf(val, L"%f", m_fBlendTimeAuto); break;
  case SET_TIME_BETWEEN:      swprintf(val, L"%f", m_fTimeBetweenPresets); break;
  case SET_HARD_CUTS:
  case SET_PRESET_LOCK:
  case SET_SEQ_ORDER:
  case SET_SONG_TITLE_ANIMS:
  case SET_CHANGE_WITH_SONG:
  case SET_SHOW_FPS:
  case SET_ALWAYS_ON_TOP:
  case SET_BORDERLESS:
  case SET_SPRITES_MESSAGES:
    swprintf(val, L"%d", m_nSpriteMessagesMode);
    break;
  case SET_SPOUT: {
    bool bVal = false;
    switch (id) {
    case SET_HARD_CUTS:        bVal = m_bHardCutsDisabled; break;
    case SET_PRESET_LOCK:      bVal = m_bPresetLockOnAtStartup; break;
    case SET_SEQ_ORDER:        bVal = m_bSequentialPresetOrder; break;
    case SET_SONG_TITLE_ANIMS: bVal = m_bSongTitleAnims; break;
    case SET_CHANGE_WITH_SONG: bVal = m_ChangePresetWithSong; break;
    case SET_SHOW_FPS:         bVal = m_bShowFPS; break;
    case SET_ALWAYS_ON_TOP:    bVal = m_bAlwaysOnTop; break;
    case SET_BORDERLESS:       bVal = m_WindowBorderless; break;
    case SET_SPOUT:            bVal = bSpoutOut; break;
    }
    swprintf(val, L"%d", bVal ? 1 : 0);
    break;
  }
  }
  WritePrivateProfileStringW(s.iniSection, s.iniKey, val, GetConfigIniFile());
}

void Engine::OpenFolderPickerForPresetDir() {
  DebugLogW(L"OpenFolderPicker: entering", LOG_VERBOSE);

  // COM must be initialized on this thread for IFileDialog
  HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  {
    wchar_t dbg[128];
    swprintf(dbg, 128, L"OpenFolderPicker: CoInitializeEx hr=0x%08X", (unsigned)hrCom);
    DebugLogW(dbg, LOG_VERBOSE);
  }
  if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE) {
    AddError(L"Failed to initialize COM for folder picker.", 4.0f, ERR_MISC, true);
    return;
  }

  IFileDialog* pfd = NULL;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
  {
    wchar_t dbg[128];
    swprintf(dbg, 128, L"OpenFolderPicker: CoCreateInstance hr=0x%08X", (unsigned)hr);
    DebugLogW(dbg, LOG_VERBOSE);
  }
  if (SUCCEEDED(hr)) {
    DWORD dwOptions;
    pfd->GetOptions(&dwOptions);
    pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    pfd->SetTitle(L"Select Preset Directory (folder containing .milk files)");
    DebugLogW(L"OpenFolderPicker: options set", LOG_VERBOSE);

    IShellItem* psiFolder = NULL;
    if (SHCreateItemFromParsingName(m_szPresetDir, NULL, IID_PPV_ARGS(&psiFolder)) == S_OK) {
      pfd->SetFolder(psiFolder);
      psiFolder->Release();
      DebugLogW(L"OpenFolderPicker: initial folder set", LOG_VERBOSE);
    }

    DebugLogW(L"OpenFolderPicker: about to call Show(NULL)...", LOG_VERBOSE);
    hr = pfd->Show(NULL);  // NULL parent to avoid DX12 window interaction issues
    {
      wchar_t dbg[128];
      swprintf(dbg, 128, L"OpenFolderPicker: Show returned hr=0x%08X", (unsigned)hr);
      DebugLogW(dbg, LOG_VERBOSE);
    }
    if (SUCCEEDED(hr)) {
      IShellItem* psi = NULL;
      hr = pfd->GetResult(&psi);
      if (SUCCEEDED(hr)) {
        LPWSTR pszPath = NULL;
        hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
        if (SUCCEEDED(hr) && pszPath) {
          lstrcpyW(m_szPresetDir, pszPath);
          int len = lstrlenW(m_szPresetDir);
          if (len > 0 && m_szPresetDir[len - 1] != L'\\')
            lstrcatW(m_szPresetDir, L"\\");
          WritePrivateProfileStringW(L"Settings", L"szPresetDir", m_szPresetDir, GetConfigIniFile());
          CoTaskMemFree(pszPath);
          UpdatePresetList(false, true);
          m_bSettingsNeedAttention = false;
          wchar_t notif[512];
          swprintf(notif, L"Preset directory: %s", m_szPresetDir);
          AddNotification(notif);
          DebugLogW(L"OpenFolderPicker: preset dir updated", LOG_VERBOSE);
        }
        psi->Release();
      }
    }
    pfd->Release();
    DebugLogW(L"OpenFolderPicker: dialog released", LOG_VERBOSE);
  }

  if (SUCCEEDED(hrCom))
    CoUninitialize();
  DebugLogW(L"OpenFolderPicker: done", LOG_VERBOSE);
}

//----------------------------------------------------------------------
// Win32 Settings Window
//----------------------------------------------------------------------

static HWND CreateSlider(HWND hParent, int id, int x, int y, int w, int h,
                         int rangeMin, int rangeMax, int pos, bool visible = true) {
  DWORD style = WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, TRACKBAR_CLASSW, NULL, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw) {
    SendMessage(hw, TBM_SETRANGE, TRUE, MAKELPARAM(rangeMin, rangeMax));
    SendMessage(hw, TBM_SETPOS, TRUE, pos);
  }
  return hw;
}

// Tab control subclass: paints dark background via WM_ERASEBKGND
static LRESULT CALLBACK SettingsTabSubclassProc(
  HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
  UINT_PTR /*subclassId*/, DWORD_PTR refData)
{
  switch (msg) {
  case WM_ERASEBKGND: {
    Engine* p = (Engine*)refData;
    if (p && p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      HDC hdc = (HDC)wParam;
      RECT rc;
      GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, p->m_hBrSettingsBg);
      return 1;
    }
    break;
  }
  case WM_NCDESTROY:
    RemoveWindowSubclass(hwnd, SettingsTabSubclassProc, 1);
    break;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Try to convert an absolute path to a relative path (relative to content base or milkdrop2 dir).
// If the file lives under one of those directories, returns the relative portion.
// Otherwise returns the absolute path unchanged.
static std::wstring MakeRelativeSpritePath(const wchar_t* szAbsPath,
                                            const wchar_t* szContentBasePath,
                                            const wchar_t* szMilkdrop2Path) {
  if (!szAbsPath || !szAbsPath[0]) return L"";
  // Already relative? Return as-is.
  if (szAbsPath[1] != L':') return szAbsPath;

  // Try content base path first, then milkdrop2 path
  const wchar_t* bases[] = { szContentBasePath, szMilkdrop2Path };
  for (auto base : bases) {
    if (!base || !base[0]) continue;
    size_t baseLen = wcslen(base);
    if (_wcsnicmp(szAbsPath, base, baseLen) == 0) {
      return szAbsPath + baseLen;
    }
  }
  // Not under either base path — return absolute
  return szAbsPath;
}

LRESULT CALLBACK Engine::SettingsWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  // Set GWLP_USERDATA on first message so dark theme painting works during creation
  if (uMsg == WM_NCCREATE) {
    CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
    if (pcs && pcs->lpCreateParams)
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pcs->lpCreateParams);
  }
  Engine* p = (Engine*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

  switch (uMsg) {
  case WM_CLOSE:
    DestroyWindow(hWnd);
    return 0;

  case WM_DESTROY:
    KillTimer(hWnd, IDT_IPC_MONITOR);
    if (p) {
      p->m_hSettingsWnd = NULL;
      p->m_hSettingsTab = NULL;
      for (int i = 0; i < SETTINGS_NUM_PAGES; i++) p->m_settingsPageCtrls[i].clear();
      if (p->m_hSpriteImageList) { ImageList_Destroy((HIMAGELIST)p->m_hSpriteImageList); p->m_hSpriteImageList = NULL; }
      p->m_hSpriteList = NULL;
      if (p->m_hSettingsFont) { DeleteObject(p->m_hSettingsFont); p->m_hSettingsFont = NULL; }
      if (p->m_hSettingsFontBold) { DeleteObject(p->m_hSettingsFontBold); p->m_hSettingsFontBold = NULL; }
      p->CleanupSettingsThemeBrushes();
    }
    PostQuitMessage(0);  // exit the settings thread's message loop
    return 0;

  case WM_MW_PRESET_CHANGED:
    // Render thread changed the active preset — sync the Settings listbox
    if (p) {
      HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
      if (hList && p->m_nCurrentPreset >= 0 && p->m_nCurrentPreset < p->m_nPresets)
        SendMessage(hList, LB_SETCURSEL, p->m_nCurrentPreset, 0);
    }
    return 0;

  case WM_NOTIFY:
  {
    NMHDR* pnm = (NMHDR*)lParam;
    if (pnm->idFrom == IDC_MW_TAB && pnm->code == TCN_SELCHANGE) {
      int sel = TabCtrl_GetCurSel(pnm->hwndFrom);
      if (p) p->ShowSettingsPage(sel);
    }
    // Sprite ListView selection change
    if (p && pnm->idFrom == IDC_MW_SPR_LIST && pnm->code == LVN_ITEMCHANGED) {
      NMLISTVIEW* pnmv = (NMLISTVIEW*)lParam;
      if ((pnmv->uNewState & LVIS_SELECTED) && !(pnmv->uOldState & LVIS_SELECTED)) {
        if (p->m_nSpriteSelected >= 0)
          p->SaveCurrentSpriteProperties();
        p->m_nSpriteSelected = pnmv->iItem;
        p->UpdateSpriteProperties(pnmv->iItem);
      }
    }
    return 0;
  }

  case WM_SIZE:
  {
    if (wParam == SIZE_MINIMIZED) break;
    if (p) {
      RECT rc;
      GetWindowRect(hWnd, &rc);
      p->m_nSettingsWndW = rc.right - rc.left;
      p->m_nSettingsWndH = rc.bottom - rc.top;
      p->LayoutSettingsControls();
    }
    return 0;
  }

  case WM_GETMINMAXINFO:
  {
    MINMAXINFO* mmi = (MINMAXINFO*)lParam;
    mmi->ptMinTrackSize.x = 500;
    mmi->ptMinTrackSize.y = 450;
    // Cap max to current monitor's work area to prevent oversized window
    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
      mmi->ptMaxTrackSize.x = mi.rcWork.right - mi.rcWork.left;
      mmi->ptMaxTrackSize.y = mi.rcWork.bottom - mi.rcWork.top;
    }
    return 0;
  }

  case WM_HSCROLL:
  {
    HWND hTrack = (HWND)lParam;
    int id = GetDlgCtrlID(hTrack);
    int pos = (int)SendMessage(hTrack, TBM_GETPOS, 0, 0);
    if (!p) break;

    switch (id) {
    case IDC_MW_OPACITY: {
      p->fOpacity = pos / 100.0f;
      wchar_t buf[32]; swprintf(buf, 32, L"%d%%", pos);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_OPACITY_LABEL), buf);
      HWND hw = p->GetPluginWindow();
      if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
      break;
    }
    case IDC_MW_RENDER_QUALITY: {
      p->m_fRenderQuality = pos / 100.0f;
      wchar_t buf[32]; swprintf(buf, 32, L"%.2f", p->m_fRenderQuality);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_QUALITY_LABEL), buf);
      HWND hw = p->GetPluginWindow();
      if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
      break;
    }
    case IDC_MW_COL_HUE: {
      p->m_ColShiftHue = (pos - 100) / 100.0f;
      wchar_t buf[32]; swprintf(buf, 32, L"%.2f", p->m_ColShiftHue);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_HUE_LABEL), buf);
      break;
    }
    case IDC_MW_COL_SAT: {
      p->m_ColShiftSaturation = (pos - 100) / 100.0f;
      wchar_t buf[32]; swprintf(buf, 32, L"%.2f", p->m_ColShiftSaturation);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_SAT_LABEL), buf);
      break;
    }
    case IDC_MW_COL_BRIGHT: {
      p->m_ColShiftBrightness = (pos - 100) / 100.0f;
      wchar_t buf[32]; swprintf(buf, 32, L"%.2f", p->m_ColShiftBrightness);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT_LABEL), buf);
      break;
    }
    case IDC_MW_COL_GAMMA: {
      float gamma = pos / 10.0f;
      p->m_pState->m_fGammaAdj = gamma;
      wchar_t buf[32]; swprintf(buf, 32, L"%.1f", gamma);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_GAMMA_LABEL), buf);
      break;
    }
    }
    return 0;
  }

  case WM_COMMAND:
  {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    if (!p) break;

    // Browse button
    if (id == IDC_MW_BROWSE_DIR && code == BN_CLICKED) {
      p->OpenFolderPickerForPresetDir();
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_PRESET_DIR), p->m_szPresetDir);
      // Repopulate preset listbox after directory change
      HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
      if (hList) {
        SendMessage(hList, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < p->m_nPresets; i++) {
          if (p->m_presets[i].szFilename.empty()) continue;
          SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)p->m_presets[i].szFilename.c_str());
        }
        if (p->m_nCurrentPreset >= 0 && p->m_nCurrentPreset < p->m_nPresets)
          SendMessage(hList, LB_SETCURSEL, p->m_nCurrentPreset, 0);
      }
      return 0;
    }

    // Preset listbox selection
    if (id == IDC_MW_PRESET_LIST && code == LBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel < p->m_nPresets) {
        // Skip directory entries (prefixed with '*') — don't try to load them
        if (p->m_presets[sel].szFilename.c_str()[0] == L'*')
          return 0;
        p->m_nCurrentPreset = sel;
        wchar_t szFile[MAX_PATH];
        swprintf(szFile, MAX_PATH, L"%s%s", p->m_szPresetDir, p->m_presets[sel].szFilename.c_str());
        p->LoadPreset(szFile, p->m_fBlendTimeUser);
      }
      return 0;
    }

    // Preset listbox double-click: navigate into directories or load preset
    if (id == IDC_MW_PRESET_LIST && code == LBN_DBLCLK) {
      int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel < p->m_nPresets) {
        if (p->m_presets[sel].szFilename.c_str()[0] == L'*') {
          // Directory entry
          if (wcscmp(p->m_presets[sel].szFilename.c_str(), L"*..") == 0)
            p->NavigatePresetDirUp(hWnd);
          else
            p->NavigatePresetDirInto(hWnd, sel);
        } else {
          // Regular preset — load it
          p->m_nCurrentPreset = sel;
          wchar_t szFile[MAX_PATH];
          swprintf(szFile, MAX_PATH, L"%s%s", p->m_szPresetDir, p->m_presets[sel].szFilename.c_str());
          p->LoadPreset(szFile, p->m_fBlendTimeUser);
        }
      }
      return 0;
    }

    // Preset nav: prev
    if (id == IDC_MW_PRESET_PREV && code == BN_CLICKED) {
      p->PrevPreset(p->m_fBlendTimeUser);
      HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
      if (hList && p->m_nCurrentPreset >= 0)
        SendMessage(hList, LB_SETCURSEL, p->m_nCurrentPreset, 0);
      return 0;
    }

    // Preset nav: next
    if (id == IDC_MW_PRESET_NEXT && code == BN_CLICKED) {
      p->NextPreset(p->m_fBlendTimeUser);
      HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
      if (hList && p->m_nCurrentPreset >= 0)
        SendMessage(hList, LB_SETCURSEL, p->m_nCurrentPreset, 0);
      return 0;
    }

    // Preset nav: copy path to clipboard
    if (id == IDC_MW_PRESET_COPY && code == BN_CLICKED) {
      HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
      int sel = hList ? (int)SendMessage(hList, LB_GETCURSEL, 0, 0) : -1;
      if (sel >= 0 && sel < p->m_nPresets) {
        wchar_t szFile[MAX_PATH];
        swprintf(szFile, MAX_PATH, L"%s%s", p->m_szPresetDir, p->m_presets[sel].szFilename.c_str());
        if (OpenClipboard(hWnd)) {
          EmptyClipboard();
          size_t len = (wcslen(szFile) + 1) * sizeof(wchar_t);
          HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
          if (hMem) {
            memcpy(GlobalLock(hMem), szFile, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
          }
          CloseClipboard();
        }
      }
      return 0;
    }

    // Directory nav: go up to parent directory
    if (id == IDC_MW_PRESET_UP && code == BN_CLICKED) {
      p->NavigatePresetDirUp(hWnd);
      return 0;
    }

    // Directory nav: enter selected subdirectory
    if (id == IDC_MW_PRESET_INTO && code == BN_CLICKED) {
      HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
      int sel = hList ? (int)SendMessage(hList, LB_GETCURSEL, 0, 0) : -1;
      p->NavigatePresetDirInto(hWnd, sel);
      return 0;
    }

    if (id == IDC_MW_RESOURCES && code == BN_CLICKED) {
      p->OpenResourceViewer();
      return 0;
    }

    if (id == IDC_MW_GPU_RELOAD_PRESET && code == BN_CLICKED) {
      // Re-render the current preset with updated GPU protection settings
      p->NextPreset(0.0f);
      return 0;
    }

    if (id == IDC_MW_RESTART_RENDER && code == BN_CLICKED) {
      HWND hw = p->GetPluginWindow();
      if (hw) PostMessage(hw, WM_MW_RESTART_DEVICE, 0, 0);
      return 0;
    }

    if (id == IDC_MW_RESET_VISUAL && code == BN_CLICKED) {
      p->fOpacity = 1.0f;
      p->m_fRenderQuality = 1.0f;
      p->bQualityAuto = false;
      p->m_timeFactor = 1.0f;
      p->m_frameFactor = 1.0f;
      p->m_fpsFactor = 1.0f;
      p->m_VisIntensity = 1.0f;
      p->m_VisShift = 0.0f;
      p->m_VisVersion = 1.0f;
      // Update sliders
      SendMessage(GetDlgItem(hWnd, IDC_MW_OPACITY), TBM_SETPOS, TRUE, 100);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_OPACITY_LABEL), L"100%");
      SendMessage(GetDlgItem(hWnd, IDC_MW_RENDER_QUALITY), TBM_SETPOS, TRUE, 100);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_QUALITY_LABEL), L"1.00");
      // Update checkbox
      SendMessage(GetDlgItem(hWnd, IDC_MW_QUALITY_AUTO), BM_SETCHECK, BST_UNCHECKED, 0);
      // Update edit boxes
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_TIME_FACTOR), L"1.00");
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_FRAME_FACTOR), L"1.00");
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_FPS_FACTOR), L"1.00");
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_INTENSITY), L"1.00");
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_SHIFT), L"0.00");
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_VERSION), L"1");
      // Reset GPU protection
      p->m_nMaxShapeInstances = 0;
      p->m_bScaleInstancesByResolution = false;
      p->m_nInstanceScaleBaseWidth = 1920;
      p->m_bSkipHeavyPresets = false;
      p->m_nHeavyPresetMaxInstances = 4096;
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_MAX_INST), L"0");
      SendMessage(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BY_RES), BM_SETCHECK, BST_UNCHECKED, 0);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BASE), L"1920");
      SendMessage(GetDlgItem(hWnd, IDC_MW_GPU_SKIP_HEAVY), BM_SETCHECK, BST_UNCHECKED, 0);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_HEAVY_THRESHOLD), L"4096");
      p->m_bEnableVSync = true;
      SendMessage(GetDlgItem(hWnd, IDC_MW_VSYNC_ENABLED), BM_SETCHECK, BST_CHECKED, 0);
      // Apply side-effects
      HWND hw = p->GetPluginWindow();
      if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
      if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
      return 0;
    }

    if (id == IDC_MW_RESET_COLORS && code == BN_CLICKED) {
      p->m_ColShiftHue = 0.0f;
      p->m_ColShiftSaturation = 0.0f;
      p->m_ColShiftBrightness = 0.0f;
      if (p->m_pState) p->m_pState->m_fGammaAdj = 2.0f;
      p->m_AutoHue = false;
      p->m_AutoHueSeconds = 0.02f;
      // Update sliders (H/S/B center=100, gamma 2.0 = pos 20)
      SendMessage(GetDlgItem(hWnd, IDC_MW_COL_HUE), TBM_SETPOS, TRUE, 100);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_HUE_LABEL), L"0.00");
      SendMessage(GetDlgItem(hWnd, IDC_MW_COL_SAT), TBM_SETPOS, TRUE, 100);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_SAT_LABEL), L"0.00");
      SendMessage(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT), TBM_SETPOS, TRUE, 100);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT_LABEL), L"0.00");
      SendMessage(GetDlgItem(hWnd, IDC_MW_COL_GAMMA), TBM_SETPOS, TRUE, 20);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_GAMMA_LABEL), L"2.0");
      // Update checkbox and edit
      SendMessage(GetDlgItem(hWnd, IDC_MW_AUTO_HUE), BM_SETCHECK, BST_UNCHECKED, 0);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_AUTO_HUE_SEC), L"0.020");
      return 0;
    }

    if (id == IDC_MW_RESET_ALL && code == BN_CLICKED) {
      p->ResetToFactory(hWnd);
      return 0;
    }

    if (id == IDC_MW_SAVE_DEFAULTS && code == BN_CLICKED) {
      p->SaveUserDefaults();
      return 0;
    }

    if (id == IDC_MW_USER_RESET && code == BN_CLICKED) {
      p->ResetToUserDefaults(hWnd);
      return 0;
    }

    if (id == IDC_MW_RESET_WINDOW && code == BN_CLICKED) {
      p->ResetSettingsWindow();
      return 0;
    }

    if (id == IDC_MW_FONT_PLUS && code == BN_CLICKED) {
      if (p->m_nSettingsFontSize > -24) {  // max font pixel height
        p->m_nSettingsFontSize -= 2;       // more negative = larger
        p->RebuildSettingsFonts();
      }
      return 0;
    }

    if (id == IDC_MW_FONT_MINUS && code == BN_CLICKED) {
      if (p->m_nSettingsFontSize < -12) {  // min font pixel height
        p->m_nSettingsFontSize += 2;       // less negative = smaller
        p->RebuildSettingsFonts();
      }
      return 0;
    }

    if (id == IDC_MW_FILE_ADD && code == BN_CLICKED) {
      // Open folder picker
      IFileDialog* pfd = NULL;
      HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
      if (SUCCEEDED(hr)) {
        DWORD opts;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->SetTitle(L"Add Fallback Search Path");
        if (SUCCEEDED(pfd->Show(hWnd))) {
          IShellItem* psi;
          if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR pszPath = NULL;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
              std::wstring path(pszPath);
              // Ensure trailing backslash
              if (!path.empty() && path.back() != L'\\') path += L'\\';
              p->m_fallbackPaths.push_back(path);
              HWND hList = GetDlgItem(hWnd, IDC_MW_FILE_LIST);
              if (hList) SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)path.c_str());
              p->SaveFallbackPaths();
              CoTaskMemFree(pszPath);
            }
            psi->Release();
          }
        }
        pfd->Release();
      }
      return 0;
    }

    if (id == IDC_MW_FILE_REMOVE && code == BN_CLICKED) {
      HWND hList = GetDlgItem(hWnd, IDC_MW_FILE_LIST);
      int sel = hList ? (int)SendMessage(hList, LB_GETCURSEL, 0, 0) : -1;
      if (sel >= 0 && sel < (int)p->m_fallbackPaths.size()) {
        p->m_fallbackPaths.erase(p->m_fallbackPaths.begin() + sel);
        SendMessage(hList, LB_DELETESTRING, sel, 0);
        p->SaveFallbackPaths();
      }
      return 0;
    }

    if (id == IDC_MW_CONTENT_BASE_BROWSE && code == BN_CLICKED) {
      IFileDialog* pfd = NULL;
      HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
      if (SUCCEEDED(hr)) {
        DWORD opts;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->SetTitle(L"Select Content Base Path (textures, sprites, etc.)");
        if (SUCCEEDED(pfd->Show(hWnd))) {
          IShellItem* psi = NULL;
          if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR pszPath = NULL;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
              lstrcpynW(p->m_szContentBasePath, pszPath, MAX_PATH);
              int len = lstrlenW(p->m_szContentBasePath);
              if (len > 0 && p->m_szContentBasePath[len - 1] != L'\\') {
                p->m_szContentBasePath[len] = L'\\';
                p->m_szContentBasePath[len + 1] = 0;
              }
              SetWindowTextW(GetDlgItem(hWnd, IDC_MW_CONTENT_BASE_EDIT), p->m_szContentBasePath);
              p->SaveFallbackPaths();
              p->m_bNeedRescanTexturesDir = true;
              CoTaskMemFree(pszPath);
            }
            psi->Release();
          }
        }
        pfd->Release();
      }
      return 0;
    }

    if (id == IDC_MW_CONTENT_BASE_CLEAR && code == BN_CLICKED) {
      p->m_szContentBasePath[0] = 0;
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_CONTENT_BASE_EDIT), L"");
      p->SaveFallbackPaths();
      p->m_bNeedRescanTexturesDir = true;
      return 0;
    }

    if (id == IDC_MW_RANDTEX_BROWSE && code == BN_CLICKED) {
      IFileDialog* pfd = NULL;
      HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
      if (SUCCEEDED(hr)) {
        DWORD opts;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->SetTitle(L"Select Random Textures Directory");
        if (SUCCEEDED(pfd->Show(hWnd))) {
          IShellItem* psi = NULL;
          if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR pszPath = NULL;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
              lstrcpynW(p->m_szRandomTexDir, pszPath, MAX_PATH);
              // Ensure trailing backslash
              int len = lstrlenW(p->m_szRandomTexDir);
              if (len > 0 && p->m_szRandomTexDir[len - 1] != L'\\') {
                p->m_szRandomTexDir[len] = L'\\';
                p->m_szRandomTexDir[len + 1] = 0;
              }
              SetWindowTextW(GetDlgItem(hWnd, IDC_MW_RANDTEX_EDIT), p->m_szRandomTexDir);
              p->SaveFallbackPaths();
              p->m_bNeedRescanTexturesDir = true;
              CoTaskMemFree(pszPath);
            }
            psi->Release();
          }
        }
        pfd->Release();
      }
      return 0;
    }

    if (id == IDC_MW_RANDTEX_CLEAR && code == BN_CLICKED) {
      p->m_szRandomTexDir[0] = 0;
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_RANDTEX_EDIT), L"");
      p->SaveFallbackPaths();
      p->m_bNeedRescanTexturesDir = true;
      return 0;
    }

    // Messages/Sprites combo box selection
    if (id == IDC_MW_SPRITES_MESSAGES && code == CBN_SELCHANGE) {
      HWND hCombo = (HWND)lParam;
      int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 3) {
        p->m_nSpriteMessagesMode = sel;
        p->SaveSettingToINI(SET_SPRITES_MESSAGES);
        CheckDlgButton(hWnd, IDC_MW_MSG_SHOW_MESSAGES, (sel & 1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hWnd, IDC_MW_MSG_SHOW_SPRITES, (sel & 2) ? BST_CHECKED : BST_UNCHECKED);
      }
      return 0;
    }

    // Audio device combo box selection
    if (id == IDC_MW_AUDIO_DEVICE && code == CBN_SELCHANGE) {
      HWND hCombo = (HWND)lParam;
      int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
      if (sel >= 0) {
        wchar_t deviceName[MAX_PATH] = {};
        SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)deviceName);

        if (sel == 0) {
          wcscpy_s(p->m_szAudioDevicePrevious, p->m_szAudioDevice);
          p->m_nAudioDevicePreviousType = p->m_nAudioDeviceActiveType;
          p->m_szAudioDevice[0] = L'\0';
          p->m_nAudioDeviceRequestType = 0;
          p->SetAudioDeviceDisplayName(NULL, true);
        }
        else {
          bool isInput = false;
          wchar_t cleanName[MAX_PATH];
          lstrcpyW(cleanName, deviceName);
          int len = lstrlenW(cleanName);
          const wchar_t* inputSuffix = L" [Input]";
          int suffixLen = lstrlenW(inputSuffix);
          if (len > suffixLen && _wcsicmp(cleanName + len - suffixLen, inputSuffix) == 0) {
            cleanName[len - suffixLen] = L'\0';
            isInput = true;
          }
          wcscpy_s(p->m_szAudioDevicePrevious, p->m_szAudioDevice);
          p->m_nAudioDevicePreviousType = p->m_nAudioDeviceActiveType;
          wcscpy_s(p->m_szAudioDevice, cleanName);
          p->m_nAudioDeviceRequestType = isInput ? 1 : 2;
          p->SetAudioDeviceDisplayName(cleanName, !isInput);
        }
        WritePrivateProfileStringW(L"Milkwave", L"AudioDevice", p->m_szAudioDevice, p->GetConfigIniFile());
        wchar_t reqBuf[16];
        swprintf(reqBuf, 16, L"%d", p->m_nAudioDeviceRequestType);
        WritePrivateProfileStringW(L"Milkwave", L"AudioDeviceRequestType", reqBuf, p->GetConfigIniFile());
        p->m_nAudioLoopState = 1;
        p->AddNotificationAudioDevice();
      }
      return 0;
    }

    // Close button
    if (id == IDC_MW_CLOSE && code == BN_CLICKED) {
      PostMessage(hWnd, WM_CLOSE, 0, 0);
      return 0;
    }

    // Checkbox toggles — save immediately
    // Owner-drawn checkboxes: toggle the "Checked" property on click
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      bool bIsCheckbox = (bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox");
      bool bChecked;
      if (bIsCheckbox) {
        bool wasChecked = (bool)(intptr_t)GetPropW(hCtrl, L"Checked");
        bChecked = !wasChecked;
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(bChecked ? 1 : 0));
        InvalidateRect(hCtrl, NULL, TRUE);
      } else {
        bChecked = false; // not a checkbox, but let BN_CLICKED handling proceed
      }
      HWND hw = p->GetPluginWindow();
      switch (id) {
      case IDC_MW_HARD_CUTS:
        p->m_bHardCutsDisabled = bChecked;
        p->SaveSettingToINI(SET_HARD_CUTS);
        return 0;
      case IDC_MW_PRESET_LOCK:
        p->m_bPresetLockOnAtStartup = bChecked;
        p->SaveSettingToINI(SET_PRESET_LOCK);
        return 0;
      case IDC_MW_SEQ_ORDER:
        p->m_bSequentialPresetOrder = bChecked;
        p->SaveSettingToINI(SET_SEQ_ORDER);
        return 0;
      case IDC_MW_SONG_TITLE:
        p->m_bSongTitleAnims = bChecked;
        p->SaveSettingToINI(SET_SONG_TITLE_ANIMS);
        return 0;
      case IDC_MW_CHANGE_SONG:
        p->m_ChangePresetWithSong = bChecked;
        p->SaveSettingToINI(SET_CHANGE_WITH_SONG);
        return 0;
      case IDC_MW_SHOW_FPS:
        p->m_bShowFPS = bChecked;
        p->SaveSettingToINI(SET_SHOW_FPS);
        return 0;
      case IDC_MW_ALWAYS_TOP:
        p->m_bAlwaysOnTop = bChecked;
        p->SaveSettingToINI(SET_ALWAYS_ON_TOP);
        if (hw) PostMessage(hw, WM_MW_SET_ALWAYS_ON_TOP, 0, 0);
        return 0;
      case IDC_MW_BORDERLESS:
        p->m_WindowBorderless = bChecked;
        p->SaveSettingToINI(SET_BORDERLESS);
        return 0;
      case IDC_MW_SPOUT:
        if (bChecked != p->bSpoutOut) {
          if (hw) PostMessage(hw, WM_MW_TOGGLE_SPOUT, 0, 0);
        }
        return 0;
      case IDC_MW_QUALITY_AUTO:
        p->bQualityAuto = bChecked;
        if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
        return 0;
      case IDC_MW_GPU_SCALE_BY_RES:
        p->m_bScaleInstancesByResolution = bChecked;
        return 0;
      case IDC_MW_GPU_SKIP_HEAVY:
        p->m_bSkipHeavyPresets = bChecked;
        return 0;
      case IDC_MW_VSYNC_ENABLED:
        p->m_bEnableVSync = bChecked;
        return 0;
      case IDC_MW_AUTO_HUE:
        p->m_AutoHue = bChecked;
        return 0;
      case IDC_MW_SPOUT_FIXED:
        p->bSpoutFixedSize = bChecked;
        if (hw) PostMessage(hw, WM_MW_SPOUT_FIXEDSIZE, 0, 0);
        return 0;
      case IDC_MW_DARK_THEME:
        p->m_bSettingsDarkTheme = bChecked;
        WritePrivateProfileStringW(L"SettingsTheme", L"DarkTheme",
            bChecked ? L"1" : L"0", p->GetConfigIniFile());
        p->LoadSettingsThemeFromINI();
        p->ApplySettingsDarkTheme();
        return 0;
      case IDC_MW_MSG_AUTOPLAY:
        p->m_bMsgAutoplay = bChecked;
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_MSG_PLAY), bChecked ? L"Stop" : L"Play");
        if (bChecked)
          p->ScheduleNextAutoMessage();
        else
          p->m_fNextAutoMsgTime = -1.0f;
        p->SaveMsgAutoplaySettings();
        return 0;
      case IDC_MW_MSG_SEQUENTIAL:
        p->m_bMsgSequential = bChecked;
        p->m_nNextSequentialMsg = 0;
        p->SaveMsgAutoplaySettings();
        return 0;
      case IDC_MW_MSG_AUTOSIZE:
        p->m_bMessageAutoSize = bChecked;
        p->SaveMsgAutoplaySettings();
        return 0;
      case IDC_MW_MSG_SHOW_MESSAGES:
        p->m_nSpriteMessagesMode = (p->m_nSpriteMessagesMode & ~1) | (bChecked ? 1 : 0);
        p->SaveSettingToINI(SET_SPRITES_MESSAGES);
        { HWND hCombo = GetDlgItem(hWnd, IDC_MW_SPRITES_MESSAGES);
          if (hCombo) SendMessage(hCombo, CB_SETCURSEL, p->m_nSpriteMessagesMode, 0); }
        return 0;
      case IDC_MW_MSG_SHOW_SPRITES:
        p->m_nSpriteMessagesMode = (p->m_nSpriteMessagesMode & ~2) | (bChecked ? 2 : 0);
        p->SaveSettingToINI(SET_SPRITES_MESSAGES);
        { HWND hCombo = GetDlgItem(hWnd, IDC_MW_SPRITES_MESSAGES);
          if (hCombo) SendMessage(hCombo, CB_SETCURSEL, p->m_nSpriteMessagesMode, 0); }
        return 0;

      // Messages tab button handlers (non-checkbox)
      case IDC_MW_MSG_PUSH: {
        HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
        int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
        if (sel >= 0 && sel < p->m_nMsgAutoplayCount) {
          int msgIdx = p->m_nMsgAutoplayOrder[sel];
          HWND hw = p->GetPluginWindow();
          if (hw) PostMessage(hw, WM_MW_PUSH_MESSAGE, msgIdx, 0);
        }
        return 0;
      }
      case IDC_MW_MSG_UP: {
        HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
        int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
        if (sel > 0 && sel < p->m_nMsgAutoplayCount) {
          std::swap(p->m_nMsgAutoplayOrder[sel], p->m_nMsgAutoplayOrder[sel - 1]);
          p->PopulateMsgListBox(hMsgList);
          SendMessage(hMsgList, LB_SETCURSEL, sel - 1, 0);
          p->SaveMsgAutoplaySettings();
        }
        return 0;
      }
      case IDC_MW_MSG_DOWN: {
        HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
        int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
        if (sel >= 0 && sel < p->m_nMsgAutoplayCount - 1) {
          std::swap(p->m_nMsgAutoplayOrder[sel], p->m_nMsgAutoplayOrder[sel + 1]);
          p->PopulateMsgListBox(hMsgList);
          SendMessage(hMsgList, LB_SETCURSEL, sel + 1, 0);
          p->SaveMsgAutoplaySettings();
        }
        return 0;
      }
      case IDC_MW_MSG_ADD: {
        int freeSlot = -1;
        for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++) {
          if (p->m_CustomMessage[i].szText[0] == 0) { freeSlot = i; break; }
        }
        if (freeSlot < 0) { MessageBoxW(hWnd, L"All 100 message slots are full.", L"Messages", MB_OK); return 0; }
        if (p->ShowMessageEditDialog(hWnd, freeSlot, true)) {
          p->BuildMsgPlaybackOrder();
          HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
          p->PopulateMsgListBox(hMsgList);
          p->WriteCustomMessages();
        }
        return 0;
      }
      case IDC_MW_MSG_EDIT: {
        HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
        int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
        if (sel >= 0 && sel < p->m_nMsgAutoplayCount) {
          int msgIdx = p->m_nMsgAutoplayOrder[sel];
          if (p->ShowMessageEditDialog(hWnd, msgIdx, false)) {
            p->PopulateMsgListBox(hMsgList);
            SendMessage(hMsgList, LB_SETCURSEL, sel, 0);
            p->UpdateMsgPreview(hWnd, sel);
            p->WriteCustomMessages();
          }
        }
        return 0;
      }
      case IDC_MW_MSG_DELETE: {
        HWND hMsgList = GetDlgItem(hWnd, IDC_MW_MSG_LIST);
        int sel = hMsgList ? (int)SendMessage(hMsgList, LB_GETCURSEL, 0, 0) : -1;
        if (sel >= 0 && sel < p->m_nMsgAutoplayCount) {
          int msgIdx = p->m_nMsgAutoplayOrder[sel];
          p->m_CustomMessage[msgIdx].szText[0] = 0;
          p->BuildMsgPlaybackOrder();
          p->PopulateMsgListBox(hMsgList);
          p->WriteCustomMessages();
          SetWindowTextW(GetDlgItem(hWnd, IDC_MW_MSG_PREVIEW), L"");
        }
        return 0;
      }
      case IDC_MW_MSG_RELOAD:
        p->ReadCustomMessages();
        p->BuildMsgPlaybackOrder();
        p->PopulateMsgListBox(GetDlgItem(hWnd, IDC_MW_MSG_LIST));
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_MSG_PREVIEW), L"");
        return 0;
      case IDC_MW_MSG_OPENINI: {
        // Temporarily drop TOPMOST so the editor window appears in front
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        HINSTANCE hr = ShellExecuteW(NULL, L"open", p->m_szMsgIniFile, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32)
          ShellExecuteW(NULL, L"open", L"notepad.exe", p->m_szMsgIniFile, NULL, SW_SHOWNORMAL);
        // Restore TOPMOST after a brief delay so editor gets focus first
        SetTimer(hWnd, 9999, 500, NULL);
        return 0;
      }
      case IDC_MW_MSG_PASTE: {
        if (!OpenClipboard(hWnd)) return 0;
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (!hData) { CloseClipboard(); return 0; }
        wchar_t* pText = (wchar_t*)GlobalLock(hData);
        if (!pText) { CloseClipboard(); return 0; }

        std::wstring clipText(pText);
        GlobalUnlock(hData);
        CloseClipboard();

        int added = 0;
        size_t pos = 0;
        while (pos < clipText.size()) {
          size_t end = clipText.find_first_of(L"\r\n", pos);
          if (end == std::wstring::npos) end = clipText.size();
          if (end > pos && (end - pos) < 256) {
            // Find free slot
            int freeSlot = -1;
            for (int i = 0; i < MAX_CUSTOM_MESSAGES; i++) {
              if (p->m_CustomMessage[i].szText[0] == 0) { freeSlot = i; break; }
            }
            if (freeSlot < 0) break;

            wcsncpy(p->m_CustomMessage[freeSlot].szText, clipText.c_str() + pos, end - pos);
            p->m_CustomMessage[freeSlot].szText[end - pos] = 0;
            p->m_CustomMessage[freeSlot].nFont = 0;
            p->m_CustomMessage[freeSlot].fSize = 50.0f;
            p->m_CustomMessage[freeSlot].x = 0.5f;
            p->m_CustomMessage[freeSlot].y = 0.5f;
            p->m_CustomMessage[freeSlot].randx = 0;
            p->m_CustomMessage[freeSlot].randy = 0;
            p->m_CustomMessage[freeSlot].growth = 1.0f;
            p->m_CustomMessage[freeSlot].fTime = 5.0f;
            p->m_CustomMessage[freeSlot].fFade = 1.0f;
            p->m_CustomMessage[freeSlot].fFadeOut = 1.0f;
            p->m_CustomMessage[freeSlot].fBurnTime = 0;
            p->m_CustomMessage[freeSlot].nColorR = -1;
            p->m_CustomMessage[freeSlot].nColorG = -1;
            p->m_CustomMessage[freeSlot].nColorB = -1;
            p->m_CustomMessage[freeSlot].nRandR = 0;
            p->m_CustomMessage[freeSlot].nRandG = 0;
            p->m_CustomMessage[freeSlot].nRandB = 0;
            p->m_CustomMessage[freeSlot].bOverrideFace = 0;
            p->m_CustomMessage[freeSlot].bOverrideBold = 0;
            p->m_CustomMessage[freeSlot].bOverrideItal = 0;
            p->m_CustomMessage[freeSlot].bOverrideColorR = 0;
            p->m_CustomMessage[freeSlot].bOverrideColorG = 0;
            p->m_CustomMessage[freeSlot].bOverrideColorB = 0;
            p->m_CustomMessage[freeSlot].bBold = -1;
            p->m_CustomMessage[freeSlot].bItal = -1;
            p->m_CustomMessage[freeSlot].szFace[0] = 0;
            added++;
          }
          pos = end;
          while (pos < clipText.size() && (clipText[pos] == L'\r' || clipText[pos] == L'\n')) pos++;
        }

        if (added > 0) {
          p->BuildMsgPlaybackOrder();
          p->PopulateMsgListBox(GetDlgItem(hWnd, IDC_MW_MSG_LIST));
          p->WriteCustomMessages();
          wchar_t msg[64];
          swprintf(msg, 64, L"Pasted %d message(s) from clipboard.", added);
          MessageBoxW(hWnd, msg, L"Messages", MB_OK | MB_ICONINFORMATION);
        } else {
          MessageBoxW(hWnd, L"No text found on clipboard (or all slots full).", L"Messages", MB_OK);
        }
        return 0;
      }
      case IDC_MW_MSG_PLAY: {
        p->m_bMsgAutoplay = !p->m_bMsgAutoplay;
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_MSG_PLAY), p->m_bMsgAutoplay ? L"Stop" : L"Play");
        CheckDlgButton(hWnd, IDC_MW_MSG_AUTOPLAY, p->m_bMsgAutoplay ? BST_CHECKED : BST_UNCHECKED);
        if (p->m_bMsgAutoplay)
          p->ScheduleNextAutoMessage();
        else
          p->m_fNextAutoMsgTime = -1.0f;
        p->SaveMsgAutoplaySettings();
        return 0;
      }
      case IDC_MW_MSG_OVERRIDES: {
        p->ShowMsgOverridesDialog(hWnd);
        return 0;
      }
      }
    }

    // Remote tab: Apply & Restart IPC
    if (id == IDC_MW_IPC_APPLY && code == BN_CLICKED) {
      // Read current edit field values
      wchar_t buf[256];
      HWND hEdit = GetDlgItem(hWnd, IDC_MW_IPC_TITLE);
      if (hEdit) {
        GetWindowTextW(hEdit, buf, 256);
        lstrcpyW(p->m_szWindowTitle, buf);
      }
      hEdit = GetDlgItem(hWnd, IDC_MW_IPC_REMOTE_TITLE);
      if (hEdit) {
        GetWindowTextW(hEdit, buf, 256);
        lstrcpyW(p->m_szRemoteWindowTitle, buf);
      }
      // Save to INI
      const wchar_t* pIni = p->GetConfigIniFile();
      WritePrivateProfileStringW(L"Milkwave", L"WindowTitle", p->m_szWindowTitle, pIni);
      WritePrivateProfileStringW(L"Milkwave", L"RemoteWindowTitle", p->m_szRemoteWindowTitle, pIni);
      // Restart IPC on render thread
      HWND hw = p->GetPluginWindow();
      if (hw) PostMessage(hw, WM_MW_RESTART_IPC, 0, 0);
      // Give IPC thread a moment to restart, then refresh list
      Sleep(200);
      p->RefreshIPCList(hWnd);
      return 0;
    }

    // Remote tab: Save Screenshot
    if (id == IDC_MW_IPC_CAPTURE && code == BN_CLICKED) {
      // Build a suggested filename from current preset + timestamp
      wchar_t presetName[MAX_PATH] = L"screenshot";
      if (p->m_szCurrentPresetFile[0]) {
        wchar_t* fn = wcsrchr(p->m_szCurrentPresetFile, L'\\');
        fn = fn ? fn + 1 : p->m_szCurrentPresetFile;
        wcsncpy_s(presetName, fn, _TRUNCATE);
        wchar_t* ext = wcsrchr(presetName, L'.');
        if (ext) *ext = L'\0';
        for (wchar_t* c = presetName; *c; c++)
          if (*c == L'/' || *c == L':' || *c == L'*' || *c == L'?' || *c == L'"' || *c == L'<' || *c == L'>' || *c == L'|')
            *c = L'_';
      }
      SYSTEMTIME st; GetLocalTime(&st);
      wchar_t suggestedName[MAX_PATH];
      swprintf_s(suggestedName, L"%04d%02d%02d-%02d%02d%02d-%s.png",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, presetName);

      // Remember last directory (static persists across calls)
      static wchar_t sLastDir[MAX_PATH] = {};
      if (sLastDir[0] == L'\0') {
        // Default to capture\ subdirectory
        swprintf_s(sLastDir, L"%scapture\\", p->m_szBaseDir);
        CreateDirectoryW(sLastDir, NULL);
      }

      wchar_t filePath[MAX_PATH];
      wcsncpy_s(filePath, suggestedName, _TRUNCATE);

      OPENFILENAMEW ofn = {};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hWnd;
      ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0";
      ofn.lpstrFile = filePath;
      ofn.nMaxFile = MAX_PATH;
      ofn.lpstrInitialDir = sLastDir;
      ofn.lpstrDefExt = L"png";
      ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
      ofn.lpstrTitle = L"Save Screenshot";

      if (GetSaveFileNameW(&ofn)) {
        // Remember directory for next time
        wcsncpy_s(sLastDir, filePath, _TRUNCATE);
        wchar_t* lastSlash = wcsrchr(sLastDir, L'\\');
        if (lastSlash) lastSlash[1] = L'\0';

        // Set screenshot path and request capture on render thread
        wcsncpy_s(p->m_screenshotPath, filePath, _TRUNCATE);
        p->m_bScreenshotRequested = true;
      }
      return 0;
    }

    // Messages tab listbox selection (different notification code)
    if (id == IDC_MW_MSG_LIST && code == LBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
      p->UpdateMsgPreview(hWnd, sel);
      return 0;
    }

    // Edit control changes (apply on focus lost)
    if (code == EN_KILLFOCUS) {
      wchar_t buf[64];
      GetWindowTextW((HWND)lParam, buf, 64);
      HWND hw = p->GetPluginWindow();
      switch (id) {
      case IDC_MW_AUDIO_SENS:
        p->m_fAudioSensitivity = (float)_wtof(buf);
        if (p->m_fAudioSensitivity < -1) p->m_fAudioSensitivity = -1;
        if (p->m_fAudioSensitivity > 256) p->m_fAudioSensitivity = 256;
        if (p->m_fAudioSensitivity == -1.0f) {
          mdropdx12_audio_adaptive = true;
        } else {
          mdropdx12_audio_adaptive = false;
          if (p->m_fAudioSensitivity < 0.5f) p->m_fAudioSensitivity = 0.5f;
          mdropdx12_audio_sensitivity = p->m_fAudioSensitivity;
        }
        p->SaveSettingToINI(SET_AUDIO_SENSITIVITY);
        return 0;
      case IDC_MW_BLEND_TIME:
        p->m_fBlendTimeAuto = (float)_wtof(buf);
        if (p->m_fBlendTimeAuto < 0.1f) p->m_fBlendTimeAuto = 0.1f;
        if (p->m_fBlendTimeAuto > 10) p->m_fBlendTimeAuto = 10;
        p->SaveSettingToINI(SET_BLEND_TIME);
        return 0;
      case IDC_MW_TIME_BETWEEN:
        p->m_fTimeBetweenPresets = (float)_wtof(buf);
        if (p->m_fTimeBetweenPresets < 1) p->m_fTimeBetweenPresets = 1;
        if (p->m_fTimeBetweenPresets > 300) p->m_fTimeBetweenPresets = 300;
        p->SaveSettingToINI(SET_TIME_BETWEEN);
        return 0;
      case IDC_MW_TIME_FACTOR:
        p->m_timeFactor = (float)_wtof(buf);
        return 0;
      case IDC_MW_FRAME_FACTOR:
        p->m_frameFactor = (float)_wtof(buf);
        return 0;
      case IDC_MW_FPS_FACTOR:
        p->m_fpsFactor = (float)_wtof(buf);
        return 0;
      case IDC_MW_VIS_INTENSITY:
        p->m_VisIntensity = (float)_wtof(buf);
        return 0;
      case IDC_MW_VIS_SHIFT:
        p->m_VisShift = (float)_wtof(buf);
        return 0;
      case IDC_MW_VIS_VERSION:
        p->m_VisVersion = (float)_wtof(buf);
        return 0;
      case IDC_MW_GPU_MAX_INST:
        p->m_nMaxShapeInstances = _wtoi(buf);
        if (p->m_nMaxShapeInstances < 0) p->m_nMaxShapeInstances = 0;
        return 0;
      case IDC_MW_GPU_SCALE_BASE:
        p->m_nInstanceScaleBaseWidth = _wtoi(buf);
        if (p->m_nInstanceScaleBaseWidth < 320) p->m_nInstanceScaleBaseWidth = 320;
        if (p->m_nInstanceScaleBaseWidth > 7680) p->m_nInstanceScaleBaseWidth = 7680;
        return 0;
      case IDC_MW_GPU_HEAVY_THRESHOLD:
        p->m_nHeavyPresetMaxInstances = _wtoi(buf);
        if (p->m_nHeavyPresetMaxInstances < 16) p->m_nHeavyPresetMaxInstances = 16;
        return 0;
      case IDC_MW_AUTO_HUE_SEC:
        p->m_AutoHueSeconds = (float)_wtof(buf);
        if (p->m_AutoHueSeconds < 0.001f) p->m_AutoHueSeconds = 0.001f;
        return 0;
      case IDC_MW_SPOUT_WIDTH:
        p->nSpoutFixedWidth = _wtoi(buf);
        if (p->nSpoutFixedWidth < 64) p->nSpoutFixedWidth = 64;
        if (p->nSpoutFixedWidth > 7680) p->nSpoutFixedWidth = 7680;
        if (hw) PostMessage(hw, WM_MW_SPOUT_FIXEDSIZE, 0, 0);
        return 0;
      case IDC_MW_SPOUT_HEIGHT:
        p->nSpoutFixedHeight = _wtoi(buf);
        if (p->nSpoutFixedHeight < 64) p->nSpoutFixedHeight = 64;
        if (p->nSpoutFixedHeight > 4320) p->nSpoutFixedHeight = 4320;
        if (hw) PostMessage(hw, WM_MW_SPOUT_FIXEDSIZE, 0, 0);
        return 0;
      case IDC_MW_MSG_INTERVAL: {
        float val = (float)_wtof(buf);
        if (val < 1.0f) val = 1.0f;
        if (val > 9999.0f) val = 9999.0f;
        p->m_fMsgAutoplayInterval = val;
        p->SaveMsgAutoplaySettings();
        return 0;
      }
      case IDC_MW_MSG_JITTER: {
        float val = (float)_wtof(buf);
        if (val < 0.0f) val = 0.0f;
        if (val > 9999.0f) val = 9999.0f;
        p->m_fMsgAutoplayJitter = val;
        p->SaveMsgAutoplaySettings();
        return 0;
      }
      case IDC_MW_IPC_TITLE: {
        wchar_t tbuf[256];
        GetWindowTextW((HWND)lParam, tbuf, 256);
        lstrcpyW(p->m_szWindowTitle, tbuf);
        return 0;
      }
      case IDC_MW_IPC_REMOTE_TITLE: {
        wchar_t tbuf[256];
        GetWindowTextW((HWND)lParam, tbuf, 256);
        lstrcpyW(p->m_szRemoteWindowTitle, tbuf);
        return 0;
      }
      case IDC_MW_DEBUG_LOG_LEVEL: {
        int val = _wtoi(buf);
        if (val < 0) val = 0;
        if (val > 3) val = 3;
        p->m_LogLevel = val;
        DebugLogSetLevel(val);
        WritePrivateProfileIntW(val, L"LogLevel", p->GetConfigIniFile(), L"Milkwave");
        return 0;
      }
      }
    }

    // ===== Sprites tab button handlers (BN_CLICKED) =====
    if (code == BN_CLICKED) {
      switch (id) {
      case IDC_MW_SPR_ADD: {
        // Drop TOPMOST so file dialog appears in front
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        IFileOpenDialog* pDlg = NULL;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
        if (SUCCEEDED(hr) && pDlg) {
          COMDLG_FILTERSPEC filters[] = {
            { L"Image Files", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds;*.gif" },
            { L"All Files", L"*.*" }
          };
          pDlg->SetFileTypes(2, filters);
          pDlg->SetTitle(L"Add Sprite Image");
          if (SUCCEEDED(pDlg->Show(hWnd))) {
            IShellItem* pItem = NULL;
            if (SUCCEEDED(pDlg->GetResult(&pItem))) {
              LPWSTR pPath = NULL;
              if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                std::set<int> used;
                for (auto& e : p->m_spriteEntries) used.insert(e.nIndex);
                int newIdx = 0;
                while (used.count(newIdx) && newIdx < 100) newIdx++;
                if (newIdx < 100) {
                  Engine::SpriteEntry entry = {};
                  entry.nIndex = newIdx;
                  // Convert to relative path when possible
                  std::wstring relPath = MakeRelativeSpritePath(pPath, p->m_szContentBasePath, p->m_szMilkdrop2Path);
                  wcscpy_s(entry.szImg, relPath.c_str());
                  entry.nColorkey = 0;
                  entry.szInitCode = "blendmode = 0;\r\nx = 0.5; y = 0.5;\r\nsx = 0.5; sy = 0.5; rot = 0;\r\nr = 1; g = 1; b = 1; a = 1;";
                  p->m_spriteEntries.push_back(entry);
                  p->PopulateSpriteListView();
                  int newSel = (int)p->m_spriteEntries.size() - 1;
                  ListView_SetItemState(p->m_hSpriteList, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                  ListView_EnsureVisible(p->m_hSpriteList, newSel, FALSE);
                }
                CoTaskMemFree(pPath);
              }
              pItem->Release();
            }
          }
          pDlg->Release();
        }
        // Restore TOPMOST after dialog closes
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
      }

      case IDC_MW_SPR_IMPORT: {
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        IFileOpenDialog* pDlg = NULL;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
        if (SUCCEEDED(hr) && pDlg) {
          DWORD options = 0;
          pDlg->GetOptions(&options);
          pDlg->SetOptions(options | FOS_PICKFOLDERS);
          pDlg->SetTitle(L"Import Sprite Images from Folder");
          if (SUCCEEDED(pDlg->Show(hWnd))) {
            IShellItem* pItem = NULL;
            if (SUCCEEDED(pDlg->GetResult(&pItem))) {
              LPWSTR pFolder = NULL;
              if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pFolder))) {
                std::set<int> used;
                for (auto& e : p->m_spriteEntries) used.insert(e.nIndex);

                const wchar_t* exts[] = { L"*.png", L"*.jpg", L"*.jpeg", L"*.bmp", L"*.tga", L"*.dds", L"*.gif" };
                int added = 0;
                for (const wchar_t* ext : exts) {
                  wchar_t searchPath[MAX_PATH];
                  swprintf(searchPath, MAX_PATH, L"%s\\%s", pFolder, ext);
                  WIN32_FIND_DATAW fd;
                  HANDLE hFind = FindFirstFileW(searchPath, &fd);
                  if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                      int newIdx = 0;
                      while (used.count(newIdx) && newIdx < 100) newIdx++;
                      if (newIdx >= 100) break;

                      Engine::SpriteEntry entry = {};
                      entry.nIndex = newIdx;
                      // Build absolute path, then convert to relative when possible
                      wchar_t absPath[512];
                      swprintf(absPath, 512, L"%s\\%s", pFolder, fd.cFileName);
                      std::wstring relPath = MakeRelativeSpritePath(absPath, p->m_szContentBasePath, p->m_szMilkdrop2Path);
                      wcscpy_s(entry.szImg, relPath.c_str());
                      entry.nColorkey = 0;
                      entry.szInitCode = "blendmode = 0;\r\nx = 0.5; y = 0.5;\r\nsx = 0.5; sy = 0.5; rot = 0;\r\nr = 1; g = 1; b = 1; a = 1;";
                      p->m_spriteEntries.push_back(entry);
                      used.insert(newIdx);
                      added++;
                    } while (FindNextFileW(hFind, &fd));
                    FindClose(hFind);
                  }
                }
                if (added > 0) p->PopulateSpriteListView();
                CoTaskMemFree(pFolder);
              }
              pItem->Release();
            }
          }
          pDlg->Release();
        }
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
      }

      case IDC_MW_SPR_DELETE: {
        if (p->m_nSpriteSelected >= 0 && p->m_nSpriteSelected < (int)p->m_spriteEntries.size()) {
          p->m_spriteEntries.erase(p->m_spriteEntries.begin() + p->m_nSpriteSelected);
          p->m_nSpriteSelected = -1;
          p->PopulateSpriteListView();
        }
        return 0;
      }

      case IDC_MW_SPR_PUSH: {
        if (p->m_nSpriteSelected >= 0 && p->m_nSpriteSelected < (int)p->m_spriteEntries.size()) {
          p->SaveCurrentSpriteProperties();
          p->SaveSpritesToINI();
          // Flush INI cache to ensure file is written before render thread reads it
          WritePrivateProfileStringW(NULL, NULL, NULL, p->m_szImgIniFile);

          auto& entry = p->m_spriteEntries[p->m_nSpriteSelected];
          int sprNum = entry.nIndex;
          { wchar_t dbg[1024]; swprintf(dbg, 1024, L"Sprites: Push [img%02d] img=%s", sprNum, entry.szImg); DebugLogW(dbg); }
          HWND hRender = p->GetPluginWindow();
          if (hRender) PostMessage(hRender, WM_MW_PUSH_SPRITE, sprNum, -1);
        }
        return 0;
      }

      case IDC_MW_SPR_KILL: {
        if (p->m_nSpriteSelected >= 0 && p->m_nSpriteSelected < (int)p->m_spriteEntries.size()) {
          int sprNum = p->m_spriteEntries[p->m_nSpriteSelected].nIndex;
          for (int s = 0; s < NUM_TEX; s++) {
            if (p->m_texmgr.m_tex[s].pSurface && p->m_texmgr.m_tex[s].nUserData == sprNum) {
              HWND hRender = p->GetPluginWindow();
              if (hRender) PostMessage(hRender, WM_MW_KILL_SPRITE, s, 0);
              break;
            }
          }
        }
        return 0;
      }

      case IDC_MW_SPR_KILLALL: {
        HWND hRender = p->GetPluginWindow();
        if (hRender) {
          for (int s = 0; s < NUM_TEX; s++) {
            if (p->m_texmgr.m_tex[s].pSurface)
              PostMessage(hRender, WM_MW_KILL_SPRITE, s, 0);
          }
        }
        return 0;
      }

      case IDC_MW_SPR_DEFAULTS: {
        if (p->m_nSpriteSelected >= 0 && p->m_nSpriteSelected < (int)p->m_spriteEntries.size()) {
          auto& e = p->m_spriteEntries[p->m_nSpriteSelected];
          e.szInitCode = "blendmode = 0;\r\nx = 0.5; y = 0.5;\r\nsx = 0.5; sy = 0.5; rot = 0;\r\nr = 1; g = 1; b = 1; a = 1;";
          e.szFrameCode.clear();
          e.nColorkey = 0;
          p->UpdateSpriteProperties(p->m_nSpriteSelected);
        }
        return 0;
      }

      case IDC_MW_SPR_SAVE: {
        if (p->m_nSpriteSelected >= 0)
          p->SaveCurrentSpriteProperties();
        p->SaveSpritesToINI();
        return 0;
      }

      case IDC_MW_SPR_RELOAD: {
        p->m_nSpriteSelected = -1;
        p->LoadSpritesFromINI();
        p->PopulateSpriteListView();
        return 0;
      }

      case IDC_MW_SPR_OPENINI: {
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        HINSTANCE hr = ShellExecuteW(NULL, L"open", p->m_szImgIniFile, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32)
          ShellExecuteW(NULL, L"open", L"notepad.exe", p->m_szImgIniFile, NULL, SW_SHOWNORMAL);
        SetTimer(hWnd, 9999, 500, NULL);
        return 0;
      }

      case IDC_MW_SPR_IMG_BROWSE: {
        if (p->m_nSpriteSelected < 0 || p->m_nSpriteSelected >= (int)p->m_spriteEntries.size()) return 0;
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        IFileOpenDialog* pDlg = NULL;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
        if (SUCCEEDED(hr) && pDlg) {
          COMDLG_FILTERSPEC filters[] = {
            { L"Image Files", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds;*.gif" },
            { L"All Files", L"*.*" }
          };
          pDlg->SetFileTypes(2, filters);
          pDlg->SetTitle(L"Browse for Sprite Image");
          if (SUCCEEDED(pDlg->Show(hWnd))) {
            IShellItem* pItem = NULL;
            if (SUCCEEDED(pDlg->GetResult(&pItem))) {
              LPWSTR pPath = NULL;
              if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                // Convert to relative path when possible
                std::wstring relPath = MakeRelativeSpritePath(pPath, p->m_szContentBasePath, p->m_szMilkdrop2Path);
                wcscpy_s(p->m_spriteEntries[p->m_nSpriteSelected].szImg, relPath.c_str());
                SetDlgItemTextW(hWnd, IDC_MW_SPR_IMG_PATH, relPath.c_str());
                p->PopulateSpriteListView();
                ListView_SetItemState(p->m_hSpriteList, p->m_nSpriteSelected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                CoTaskMemFree(pPath);
              }
              pItem->Release();
            }
          }
          pDlg->Release();
        }
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
      }
      } // end sprite BN_CLICKED switch
    }

    break;
  }

  case WM_TIMER:
    if (wParam == 9999) {
      KillTimer(hWnd, 9999);
      SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      return 0;
    }
    if (wParam == IDT_IPC_MONITOR && p) {
      int seq = g_lastIPCMessageSeq.load();
      if (seq != p->m_lastSeenIPCSeq) {
        p->m_lastSeenIPCSeq = seq;
        // Only update display if an IPC window is selected in list
        HWND hList = GetDlgItem(hWnd, IDC_MW_IPC_LIST);
        int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (sel != LB_ERR) {
          wchar_t header[64];
          swprintf_s(header, L"Last message: %s", g_szLastIPCTime);
          SetWindowTextW(GetDlgItem(hWnd, IDC_MW_IPC_MSG_GROUP), header);
          SetWindowTextW(GetDlgItem(hWnd, IDC_MW_IPC_MSG_TEXT), g_szLastIPCMessage);
        }
      }
      return 0;
    }
    break;

  // Dark theme color handling for settings window controls
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    if (p && p->m_bSettingsDarkTheme && p->m_hBrSettingsCtrlBg) {
      SetTextColor((HDC)wParam, p->m_colSettingsText);
      SetBkColor((HDC)wParam, p->m_colSettingsCtrlBg);
      return (LRESULT)p->m_hBrSettingsCtrlBg;
    }
    break;

  case WM_CTLCOLORSTATIC:
    if (p && p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      HDC hdc = (HDC)wParam;
      // Check if the static control is a disabled edit (ES_READONLY sends CTLCOLORSTATIC)
      HWND hCtrl = (HWND)lParam;
      wchar_t szClass[32];
      GetClassNameW(hCtrl, szClass, 32);
      if (_wcsicmp(szClass, L"Edit") == 0) {
        SetTextColor(hdc, p->m_colSettingsText);
        SetBkColor(hdc, p->m_colSettingsCtrlBg);
        return (LRESULT)p->m_hBrSettingsCtrlBg;
      }
      SetTextColor(hdc, p->m_colSettingsText);
      SetBkColor(hdc, p->m_colSettingsBg);
      SetBkMode(hdc, TRANSPARENT);
      return (LRESULT)p->m_hBrSettingsBg;
    }
    break;

  case WM_CTLCOLORBTN:
    if (p && p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      SetTextColor((HDC)wParam, p->m_colSettingsText);
      SetBkColor((HDC)wParam, p->m_colSettingsBg);
      return (LRESULT)p->m_hBrSettingsBg;
    }
    break;

  case WM_CTLCOLORDLG:
    if (p && p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      return (LRESULT)p->m_hBrSettingsBg;
    }
    break;

  case WM_DRAWITEM:
    if (p) {
      DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
      if (pDIS && pDIS->CtlType == ODT_TAB) {
        // Owner-draw tab header items with 3D beveled edges
        bool bSelected = (pDIS->itemState & ODS_SELECTED) != 0;
        HDC hdc = pDIS->hDC;
        RECT rc = pDIS->rcItem;
        if (p->m_bSettingsDarkTheme) {
          // Fill tab background
          COLORREF bg = bSelected ? p->m_colSettingsCtrlBg : p->m_colSettingsBtnFace;
          HBRUSH hBr = CreateSolidBrush(bg);
          FillRect(hdc, &rc, hBr);
          DeleteObject(hBr);

          if (bSelected) {
            // 3D raised edges on top and sides (no bottom — merges with content)
            HPEN hiPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnHi);
            HPEN shPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnShadow);
            HPEN oldPen = (HPEN)SelectObject(hdc, hiPen);
            // Top highlight
            MoveToEx(hdc, rc.left, rc.top, NULL);
            LineTo(hdc, rc.right - 1, rc.top);
            // Left highlight
            MoveToEx(hdc, rc.left, rc.top, NULL);
            LineTo(hdc, rc.left, rc.bottom);
            // Right shadow
            SelectObject(hdc, shPen);
            MoveToEx(hdc, rc.right - 1, rc.top, NULL);
            LineTo(hdc, rc.right - 1, rc.bottom);
            SelectObject(hdc, oldPen);
            DeleteObject(hiPen);
            DeleteObject(shPen);
          } else {
            // Unselected: subtle bottom edge only
            HPEN shPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnShadow);
            HPEN oldPen = (HPEN)SelectObject(hdc, shPen);
            MoveToEx(hdc, rc.left, rc.bottom - 1, NULL);
            LineTo(hdc, rc.right, rc.bottom - 1);
            SelectObject(hdc, oldPen);
            DeleteObject(shPen);
          }

          SetBkMode(hdc, TRANSPARENT);
          SetTextColor(hdc, bSelected ? p->m_colSettingsHighlightText : p->m_colSettingsText);
        } else {
          FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
          SetBkMode(hdc, TRANSPARENT);
          SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        }
        wchar_t szText[64] = {};
        TCITEMW tci = {};
        tci.mask = TCIF_TEXT;
        tci.pszText = szText;
        tci.cchTextMax = 64;
        SendMessageW(pDIS->hwndItem, TCM_GETITEMW, pDIS->itemID, (LPARAM)&tci);
        DrawTextW(hdc, szText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
      }
      if (pDIS && pDIS->CtlType == ODT_BUTTON) {
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
    }
    break;

  case WM_ERASEBKGND:
    if (p && p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      HDC hdc = (HDC)wParam;
      RECT rc;
      GetClientRect(hWnd, &rc);
      FillRect(hdc, &rc, p->m_hBrSettingsBg);
      return 1;
    }
    break;

  }
  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Enumerate audio devices into a combo box. Returns the index of the current device, or -1.
static int EnumAudioDevicesIntoCombo(HWND hCombo, const wchar_t* szCurrentDevice) {
  int curIdx = -1;

  // Add "(Default)" as first entry
  SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(Default)");

  HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  IMMDeviceEnumerator* pEnum = NULL;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
  if (SUCCEEDED(hr) && pEnum) {
    // Enumerate render (output) devices
    IMMDeviceCollection* pCollection = NULL;
    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr) && pCollection) {
      UINT count = 0;
      pCollection->GetCount(&count);
      for (UINT i = 0; i < count; i++) {
        IMMDevice* pDev = NULL;
        if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
          IPropertyStore* pProps = NULL;
          if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
              int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)pv.pwszVal);
              if (_wcsicmp(pv.pwszVal, szCurrentDevice) == 0)
                curIdx = idx;
            }
            PropVariantClear(&pv);
            pProps->Release();
          }
          pDev->Release();
        }
      }
      pCollection->Release();
    }
    // Also enumerate capture (input) devices
    pCollection = NULL;
    hr = pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr) && pCollection) {
      UINT count = 0;
      pCollection->GetCount(&count);
      for (UINT i = 0; i < count; i++) {
        IMMDevice* pDev = NULL;
        if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
          IPropertyStore* pProps = NULL;
          if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
              // Mark input devices with [Input] suffix
              wchar_t label[MAX_PATH];
              swprintf(label, MAX_PATH, L"%s [Input]", pv.pwszVal);
              int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)label);
              if (_wcsicmp(pv.pwszVal, szCurrentDevice) == 0)
                curIdx = idx;
            }
            PropVariantClear(&pv);
            pProps->Release();
          }
          pDev->Release();
        }
      }
      pCollection->Release();
    }
    pEnum->Release();
  }
  if (SUCCEEDED(hrCom))
    CoUninitialize();

  // Select current device, or default
  SendMessageW(hCombo, CB_SETCURSEL, curIdx >= 0 ? curIdx : 0, 0);
  return curIdx;
}

void Engine::OpenSettingsWindow() {
  // If already open, bring to front (and move off fullscreen monitor if needed)
  if (m_hSettingsWnd && IsWindow(m_hSettingsWnd)) {
    EnsureSettingsVisible();
    return;
  }
  if (m_bSettingsThreadRunning.load()) return;

  // Join any previous thread
  if (m_settingsThread.joinable())
    m_settingsThread.join();

  m_settingsThread = std::thread(&Engine::CreateSettingsWindowOnThread, this);
}

void Engine::CreateSettingsWindowOnThread() {
  m_bSettingsThreadRunning.store(true);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  // Register window class (idempotent)
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SettingsWndProc;
  wc.hInstance = GetModuleHandle(NULL);
  wc.lpszClassName = SETTINGS_WND_CLASS;
  // Use dark background if dark theme enabled; WM_ERASEBKGND handles the rest
  wc.hbrBackground = m_bSettingsDarkTheme ? CreateSolidBrush(m_colSettingsBg) : (HBRUSH)(COLOR_BTNFACE + 1);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  RegisterClassExW(&wc);

  // Init common controls for trackbar and tab support
  INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
  InitCommonControlsEx(&icex);

  // Create theme brushes BEFORE window creation so WM_ERASEBKGND works during CreateWindowEx
  LoadSettingsThemeFromINI();

  int wndW = m_nSettingsWndW, wndH = m_nSettingsWndH;
  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  int posX = (screenW - wndW) / 2;
  int posY = (screenH - wndH) / 2;

  m_hSettingsWnd = CreateWindowExW(
    WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
    SETTINGS_WND_CLASS, L"MDropDX12 Settings",
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
    posX, posY, wndW, wndH,
    NULL, NULL, GetModuleHandle(NULL), (LPVOID)this);

  if (!m_hSettingsWnd) {
    CoUninitialize();
    m_bSettingsThreadRunning.store(false);
    return;
  }
  BuildSettingsControls();
  ApplySettingsDarkTheme();

  ShowWindow(m_hSettingsWnd, SW_SHOW);
  UpdateWindow(m_hSettingsWnd);

  // Own message pump on this thread
  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    if (!IsDialogMessage(m_hSettingsWnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  m_hSettingsWnd = NULL;
  CoUninitialize();
  m_bSettingsThreadRunning.store(false);
}

void Engine::CleanupSettingsThemeBrushes() {
  if (m_hBrSettingsBg)     { DeleteObject(m_hBrSettingsBg);     m_hBrSettingsBg = NULL; }
  if (m_hBrSettingsCtrlBg) { DeleteObject(m_hBrSettingsCtrlBg); m_hBrSettingsCtrlBg = NULL; }
}

void Engine::LoadSettingsThemeFromINI() {
  // Brushes are (re)created from the current color values
  CleanupSettingsThemeBrushes();
  if (m_bSettingsDarkTheme) {
    m_hBrSettingsBg     = CreateSolidBrush(m_colSettingsBg);
    m_hBrSettingsCtrlBg = CreateSolidBrush(m_colSettingsCtrlBg);
  }
}

void Engine::ApplySettingsDarkTheme() {
  HWND hw = m_hSettingsWnd;
  if (!hw) return;

  LoadSettingsThemeFromINI();

  BOOL bDark = m_bSettingsDarkTheme ? TRUE : FALSE;

  // Title bar via DWM (works reliably on Win11+)
  DwmSetWindowAttribute(hw, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &bDark, sizeof(bDark));
  if (m_bSettingsDarkTheme) {
    DwmSetWindowAttribute(hw, 35 /* DWMWA_CAPTION_COLOR */, &m_colSettingsBg, sizeof(m_colSettingsBg));
    DwmSetWindowAttribute(hw, 34 /* DWMWA_BORDER_COLOR */, &m_colSettingsBorder, sizeof(m_colSettingsBorder));
    DwmSetWindowAttribute(hw, 36 /* DWMWA_TEXT_COLOR */, &m_colSettingsText, sizeof(m_colSettingsText));
  } else {
    // Reset to system defaults by removing custom colors
    COLORREF defNone = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
    DwmSetWindowAttribute(hw, 35, &defNone, sizeof(defNone));
    DwmSetWindowAttribute(hw, 34, &defNone, sizeof(defNone));
    DwmSetWindowAttribute(hw, 36, &defNone, sizeof(defNone));
  }

  // Tab control: strip all visual styles (owner-drawn via TCS_OWNERDRAWFIXED)
  if (m_hSettingsTab) {
    SetWindowTheme(m_hSettingsTab, m_bSettingsDarkTheme ? L"" : NULL, m_bSettingsDarkTheme ? L"" : NULL);
  }
  // Child controls: use DarkMode_Explorer for native dark scrollbars on listboxes/combos
  for (int page = 0; page < SETTINGS_NUM_PAGES; page++) {
    for (HWND hChild : m_settingsPageCtrls[page]) {
      if (!hChild || !IsWindow(hChild)) continue;
      SetWindowTheme(hChild, m_bSettingsDarkTheme ? L"DarkMode_Explorer" : NULL, NULL);
    }
  }

  // Force full redraw
  RedrawWindow(hw, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

void Engine::BuildSettingsControls() {
  HWND hw = m_hSettingsWnd;
  if (!hw) return;

  // Clear previous page control lists
  for (int i = 0; i < SETTINGS_NUM_PAGES; i++) m_settingsPageCtrls[i].clear();
  m_hSpriteList = NULL;
  if (m_hSpriteImageList) { ImageList_Destroy((HIMAGELIST)m_hSpriteImageList); m_hSpriteImageList = NULL; }

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  int clientW = rcWnd.right;
  int clientH = rcWnd.bottom;

  // Create fonts (cached for LayoutSettingsControls)
  if (m_hSettingsFont) DeleteObject(m_hSettingsFont);
  m_hSettingsFont = CreateFontW(m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  if (m_hSettingsFontBold) DeleteObject(m_hSettingsFontBold);
  m_hSettingsFontBold = CreateFontW(m_nSettingsFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  HFONT hFont = m_hSettingsFont;
  HFONT hFontBold = m_hSettingsFontBold;

  // Create tab control (TCS_OWNERDRAWFIXED lets us paint tab headers in dark theme)
  m_hSettingsTab = CreateWindowExW(0, WC_TABCONTROLW, NULL,
    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED,
    0, 0, clientW, clientH, hw, (HMENU)(INT_PTR)IDC_MW_TAB,
    GetModuleHandle(NULL), NULL);
  SendMessage(m_hSettingsTab, WM_SETFONT, (WPARAM)hFont, TRUE);
  SetWindowSubclass(m_hSettingsTab, SettingsTabSubclassProc, 1, (DWORD_PTR)this);

  // Insert tab pages (use TCM_INSERTITEMW explicitly — project is _MBCS, not UNICODE)
  const wchar_t* tabNames[] = { L"General", L"Visual", L"Colors", L"Sound", L"Files", L"Messages", L"Sprites", L"Remote", L"About" };
  for (int i = 0; i < SETTINGS_NUM_PAGES; i++) {
    TCITEMW ti = {};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)tabNames[i];
    SendMessageW(m_hSettingsTab, TCM_INSERTITEMW, i, (LPARAM)&ti);
  }

  // Get the display area below tab headers
  RECT rcDisplay = { 0, 0, clientW, clientH };
  TabCtrl_AdjustRect(m_hSettingsTab, FALSE, &rcDisplay);
  int tabTop = rcDisplay.top;

  int lineH = GetSettingsLineHeight();
  int gap = 6, x = 16;
  int lw = MulDiv(160, lineH, 26);
  int rw = clientW - 36;
  int sliderW = rw - lw - 60;
  wchar_t buf[64];
  int y;

  // Helper: track control for a page. All controls are children of hw (main window).
  // Pages 1-3 are created hidden; ShowSettingsPage(0) is called at the end.
  #define PAGE_CTRL(page, expr) do { HWND _h = (expr); if (_h) m_settingsPageCtrls[page].push_back(_h); } while(0)

  // ====== PAGE 0: General ======
  y = tabTop + 10;

  // Preset directory + browse
  PAGE_CTRL(0, CreateLabel(hw, L"Preset Directory:", x, y, rw, lineH, hFont));
  y += lineH;
  PAGE_CTRL(0, CreateEdit(hw, m_szPresetDir, IDC_MW_PRESET_DIR, x, y, rw - 70, lineH, hFont, ES_READONLY));
  PAGE_CTRL(0, CreateBtn(hw, L"Browse", IDC_MW_BROWSE_DIR, x + rw - 65, y, 65, lineH, hFont));
  y += lineH + gap;

  // Preset listbox
  {
    int listH = 8 * lineH;  // ~192px
    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
      x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_PRESET_LIST, GetModuleHandle(NULL), NULL);
    if (hList && hFont) SendMessage(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
    // Populate with preset filenames
    for (int i = 0; i < m_nPresets; i++) {
      if (m_presets[i].szFilename.empty()) continue;
      SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)m_presets[i].szFilename.c_str());
    }
    // Select current preset
    if (m_nCurrentPreset >= 0 && m_nCurrentPreset < m_nPresets)
      SendMessage(hList, LB_SETCURSEL, m_nCurrentPreset, 0);
    PAGE_CTRL(0, hList);
    y += listH + gap;
  }

  // Nav buttons: ◄  ►  ✂ (copy path)  |  ▲ Up  ▼ Into
  {
    int btnW = MulDiv(40, lineH, 26);
    int btnGap = 6;
    PAGE_CTRL(0, CreateBtn(hw, L"\x25C4", IDC_MW_PRESET_PREV, x, y, btnW, lineH + 4, hFont));
    PAGE_CTRL(0, CreateBtn(hw, L"\x25BA", IDC_MW_PRESET_NEXT, x + btnW + btnGap, y, btnW, lineH + 4, hFont));
    PAGE_CTRL(0, CreateBtn(hw, L"\x2702", IDC_MW_PRESET_COPY, x + 2 * (btnW + btnGap), y, btnW, lineH + 4, hFont));
    // Directory navigation: Up (parent) and Into (selected subfolder)
    int dirBtnX = x + 3 * (btnW + btnGap) + 10;
    int dirBtnW = MulDiv(55, lineH, 26);
    PAGE_CTRL(0, CreateBtn(hw, L"\x25B2 Up", IDC_MW_PRESET_UP, dirBtnX, y, dirBtnW, lineH + 4, hFont));
    PAGE_CTRL(0, CreateBtn(hw, L"\x25BC Into", IDC_MW_PRESET_INTO, dirBtnX + dirBtnW + btnGap, y, dirBtnW, lineH + 4, hFont));
    y += lineH + 4 + gap + 4;
  }

  // Settings
  PAGE_CTRL(0, CreateLabel(hw, L"Audio Sensitivity (-1=Auto):", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%g", (double)m_fAudioSensitivity);
  PAGE_CTRL(0, CreateEdit(hw, buf, IDC_MW_AUDIO_SENS, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap;

  PAGE_CTRL(0, CreateLabel(hw, L"Blend Time (s):", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.1f", m_fBlendTimeAuto);
  PAGE_CTRL(0, CreateEdit(hw, buf, IDC_MW_BLEND_TIME, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap;

  PAGE_CTRL(0, CreateLabel(hw, L"Time Between (s):", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.0f", m_fTimeBetweenPresets);
  PAGE_CTRL(0, CreateEdit(hw, buf, IDC_MW_TIME_BETWEEN, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap + 4;

  PAGE_CTRL(0, CreateCheck(hw, L"Hard Cuts Disabled",      IDC_MW_HARD_CUTS,    x, y, rw, lineH, hFont, m_bHardCutsDisabled)); y += lineH + 2;
  PAGE_CTRL(0, CreateCheck(hw, L"Preset Lock on Startup",  IDC_MW_PRESET_LOCK,  x, y, rw, lineH, hFont, m_bPresetLockOnAtStartup)); y += lineH + 2;
  PAGE_CTRL(0, CreateCheck(hw, L"Sequential Preset Order", IDC_MW_SEQ_ORDER,    x, y, rw, lineH, hFont, m_bSequentialPresetOrder)); y += lineH + 2;
  PAGE_CTRL(0, CreateLabel(hw, L"Messages/Sprites:", x, y, lw, lineH, hFont));
  {
    HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      x + lw + 4, y, rw - lw - 4, lineH + 4 * lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_SPRITES_MESSAGES,
      GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Off");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Messages");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Sprites");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Messages & Sprites");
    SendMessageW(hCombo, CB_SETCURSEL, m_nSpriteMessagesMode, 0);
    PAGE_CTRL(0, hCombo);
  }
  y += lineH + 2;
  PAGE_CTRL(0, CreateCheck(hw, L"Song Title Animations",   IDC_MW_SONG_TITLE,   x, y, rw, lineH, hFont, m_bSongTitleAnims)); y += lineH + 2;
  PAGE_CTRL(0, CreateCheck(hw, L"Change Preset w/ Song",   IDC_MW_CHANGE_SONG,  x, y, rw, lineH, hFont, m_ChangePresetWithSong)); y += lineH + 2;
  PAGE_CTRL(0, CreateCheck(hw, L"Show FPS",                IDC_MW_SHOW_FPS,     x, y, rw, lineH, hFont, m_bShowFPS)); y += lineH + 2;
  PAGE_CTRL(0, CreateCheck(hw, L"Always On Top",           IDC_MW_ALWAYS_TOP,   x, y, rw, lineH, hFont, m_bAlwaysOnTop)); y += lineH + 2;
  PAGE_CTRL(0, CreateCheck(hw, L"Borderless Window",       IDC_MW_BORDERLESS,   x, y, rw, lineH, hFont, m_WindowBorderless)); y += lineH + 2;
  PAGE_CTRL(0, CreateCheck(hw, L"Dark Theme",              IDC_MW_DARK_THEME,   x, y, rw, lineH, hFont, m_bSettingsDarkTheme));
  y += lineH + gap + 4;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(95, lineH, 26), bw2 = MulDiv(65, lineH, 26), bw3 = MulDiv(80, lineH, 26);
    PAGE_CTRL(0, CreateBtn(hw, L"Resources...", IDC_MW_RESOURCES, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(0, CreateBtn(hw, L"Reset", IDC_MW_RESET_ALL, bx, y, bw2, lineH, hFont)); bx += bw2 + bg;
    PAGE_CTRL(0, CreateBtn(hw, L"Save Safe", IDC_MW_SAVE_DEFAULTS, bx, y, bw3, lineH, hFont)); bx += bw3 + bg;
    PAGE_CTRL(0, CreateBtn(hw, L"Safe Reset", IDC_MW_USER_RESET, bx, y, bw3, lineH, hFont));
  }
  y += lineH + gap;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(100, lineH, 26), bw2 = MulDiv(55, lineH, 26);
    PAGE_CTRL(0, CreateBtn(hw, L"Reset Window", IDC_MW_RESET_WINDOW, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(0, CreateBtn(hw, L"Font +", IDC_MW_FONT_PLUS, bx, y, bw2, lineH, hFont)); bx += bw2 + bg;
    PAGE_CTRL(0, CreateBtn(hw, L"Font \x2013", IDC_MW_FONT_MINUS, bx, y, bw2, lineH, hFont));
  }

  // ====== PAGE 1: Visual (created hidden) ======
  y = tabTop + 10;

  PAGE_CTRL(1, CreateLabel(hw, L"Opacity:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(1, CreateSlider(hw, IDC_MW_OPACITY, x + lw + 4, y, sliderW, lineH, 0, 100, (int)(fOpacity * 100), false));
  swprintf(buf, 64, L"%d%%", (int)(fOpacity * 100));
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_OPACITY_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(1, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(1, CreateLabel(hw, L"Render Quality:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(1, CreateSlider(hw, IDC_MW_RENDER_QUALITY, x + lw + 4, y, sliderW, lineH, 0, 100, (int)(m_fRenderQuality * 100), false));
  swprintf(buf, 64, L"%.2f", m_fRenderQuality);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_QUALITY_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(1, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(1, CreateCheck(hw, L"Auto Quality", IDC_MW_QUALITY_AUTO, x, y, rw, lineH, hFont, bQualityAuto, false));
  y += lineH + gap + 4;

  PAGE_CTRL(1, CreateLabel(hw, L"Time Factor:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_timeFactor);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_TIME_FACTOR, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(1, CreateLabel(hw, L"Frame Factor:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_frameFactor);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_FRAME_FACTOR, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(1, CreateLabel(hw, L"FPS Factor:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_fpsFactor);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_FPS_FACTOR, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(1, CreateLabel(hw, L"Vis Intensity:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_VisIntensity);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_VIS_INTENSITY, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(1, CreateLabel(hw, L"Vis Shift:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_VisShift);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_VIS_SHIFT, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(1, CreateLabel(hw, L"Vis Version:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.0f", m_VisVersion);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_VIS_VERSION, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap + 4;
  PAGE_CTRL(1, CreateBtn(hw, L"Reset", IDC_MW_RESET_VISUAL, x, y, MulDiv(80, lineH, 26), lineH, hFont));
  y += lineH + gap + 8;

  // -- GPU Protection section --
  PAGE_CTRL(1, CreateLabel(hw, L"GPU Protection", x, y, rw, lineH, hFont, false));
  y += lineH + 2;

  PAGE_CTRL(1, CreateLabel(hw, L"Max Instances:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%d", m_nMaxShapeInstances);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_GPU_MAX_INST, x + lw + 4, y, 60, lineH, hFont, 0, false));
  PAGE_CTRL(1, CreateLabel(hw, L"(0=unlimited)", x + lw + 70, y, 100, lineH, hFont, false));
  y += lineH + gap;

  PAGE_CTRL(1, CreateCheck(hw, L"Scale Instances by Resolution", IDC_MW_GPU_SCALE_BY_RES, x, y, rw, lineH, hFont, m_bScaleInstancesByResolution, false));
  y += lineH + 2;

  PAGE_CTRL(1, CreateLabel(hw, L"Scale Base Width:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%d", m_nInstanceScaleBaseWidth);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_GPU_SCALE_BASE, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(1, CreateCheck(hw, L"Skip Heavy Presets", IDC_MW_GPU_SKIP_HEAVY, x, y, rw, lineH, hFont, m_bSkipHeavyPresets, false));
  y += lineH + 2;

  PAGE_CTRL(1, CreateLabel(hw, L"Heavy Threshold:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%d", m_nHeavyPresetMaxInstances);
  PAGE_CTRL(1, CreateEdit(hw, buf, IDC_MW_GPU_HEAVY_THRESHOLD, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap + 4;

  PAGE_CTRL(1, CreateCheck(hw, L"Enable VSync", IDC_MW_VSYNC_ENABLED, x, y, rw, lineH, hFont, m_bEnableVSync, false));
  y += lineH + gap + 4;

  PAGE_CTRL(1, CreateBtn(hw, L"Reload Preset", IDC_MW_GPU_RELOAD_PRESET, x, y, MulDiv(110, lineH, 26), lineH, hFont));
  PAGE_CTRL(1, CreateBtn(hw, L"Restart Render", IDC_MW_RESTART_RENDER, x + MulDiv(120, lineH, 26), y, MulDiv(120, lineH, 26), lineH, hFont));

  // ====== PAGE 2: Colors (created hidden) ======
  y = tabTop + 10;

  PAGE_CTRL(2, CreateLabel(hw, L"Hue:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(2, CreateSlider(hw, IDC_MW_COL_HUE, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(m_ColShiftHue * 100) + 100, false));
  swprintf(buf, 64, L"%.2f", m_ColShiftHue);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_HUE_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(2, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(2, CreateLabel(hw, L"Saturation:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(2, CreateSlider(hw, IDC_MW_COL_SAT, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(m_ColShiftSaturation * 100) + 100, false));
  swprintf(buf, 64, L"%.2f", m_ColShiftSaturation);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_SAT_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(2, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(2, CreateLabel(hw, L"Brightness:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(2, CreateSlider(hw, IDC_MW_COL_BRIGHT, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(m_ColShiftBrightness * 100) + 100, false));
  swprintf(buf, 64, L"%.2f", m_ColShiftBrightness);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_BRIGHT_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(2, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(2, CreateLabel(hw, L"Gamma:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(2, CreateSlider(hw, IDC_MW_COL_GAMMA, x + lw + 4, y, sliderW, lineH, 0, 80, (int)(m_pState->m_fGammaAdj.eval(-1) * 10), false));
  swprintf(buf, 64, L"%.1f", m_pState->m_fGammaAdj.eval(-1));
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_GAMMA_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(2, hLbl);
  }
  y += lineH + gap + 4;

  PAGE_CTRL(2, CreateCheck(hw, L"Auto Hue", IDC_MW_AUTO_HUE, x, y, rw / 2, lineH, hFont, m_AutoHue, false));
  PAGE_CTRL(2, CreateLabel(hw, L"Seconds:", x + rw / 2, y, 60, lineH, hFont, false));
  swprintf(buf, 64, L"%.3f", m_AutoHueSeconds);
  PAGE_CTRL(2, CreateEdit(hw, buf, IDC_MW_AUTO_HUE_SEC, x + rw / 2 + 64, y, 70, lineH, hFont, 0, false));
  y += lineH + gap + 4;
  PAGE_CTRL(2, CreateBtn(hw, L"Reset", IDC_MW_RESET_COLORS, x, y, MulDiv(80, lineH, 26), lineH, hFont));

  // ====== PAGE 3: Sound (created hidden) ======
  y = tabTop + 10;

  PAGE_CTRL(3, CreateCheck(hw, L"Spout Output",    IDC_MW_SPOUT,       x, y, rw, lineH, hFont, bSpoutOut, false));
  y += lineH + 2;
  PAGE_CTRL(3, CreateCheck(hw, L"Fixed Size",      IDC_MW_SPOUT_FIXED, x, y, rw, lineH, hFont, bSpoutFixedSize, false));
  y += lineH + gap;

  PAGE_CTRL(3, CreateLabel(hw, L"Width:", x, y, 50, lineH, hFont, false));
  swprintf(buf, 64, L"%d", nSpoutFixedWidth);
  PAGE_CTRL(3, CreateEdit(hw, buf, IDC_MW_SPOUT_WIDTH, x + 54, y, 70, lineH, hFont, 0, false));
  PAGE_CTRL(3, CreateLabel(hw, L"Height:", x + 140, y, 50, lineH, hFont, false));
  swprintf(buf, 64, L"%d", nSpoutFixedHeight);
  PAGE_CTRL(3, CreateEdit(hw, buf, IDC_MW_SPOUT_HEIGHT, x + 194, y, 70, lineH, hFont, 0, false));
  y += lineH + gap + 8;

  // Audio Device (moved here from General tab)
  PAGE_CTRL(3, CreateLabel(hw, L"Audio Device:", x, y, rw, lineH, hFont, false));
  y += lineH;
  {
    HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
      x, y, rw, 200, hw, (HMENU)(INT_PTR)IDC_MW_AUDIO_DEVICE, GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    EnumAudioDevicesIntoCombo(hCombo, m_szAudioDevice);
    PAGE_CTRL(3, hCombo);
  }

  // ====== PAGE 4: Files (created hidden) ======
  y = tabTop + 10;

  // Content Base Path (for textures, sprites, etc.)
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"Content Base Path (textures, sprites, etc.):",
      WS_CHILD | SS_LEFT, x, y, rw, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_CONTENT_BASE_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(4, hLbl);
  }
  y += lineH + 2;
  {
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_szContentBasePath,
      WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_CONTENT_BASE_EDIT,
      GetModuleHandle(NULL), NULL);
    if (hEdit && hFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(4, hEdit);
  }
  y += lineH + 6;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(80, lineH, 26), bw2 = MulDiv(60, lineH, 26);
    PAGE_CTRL(4, CreateBtn(hw, L"Browse...", IDC_MW_CONTENT_BASE_BROWSE, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(4, CreateBtn(hw, L"Clear", IDC_MW_CONTENT_BASE_CLEAR, bx, y, bw2, lineH, hFont));
  }
  y += lineH + gap + 4;

  PAGE_CTRL(4, CreateLabel(hw, L"Fallback Search Paths:", x, y, rw, lineH, hFont, false));
  y += lineH + 2;
  {
    HWND hList = CreateWindowExW(0, L"LISTBOX", L"",
      WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
      x, y, rw, 120, hw, (HMENU)(INT_PTR)IDC_MW_FILE_LIST,
      GetModuleHandle(NULL), NULL);
    if (hList && hFont) SendMessage(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
    // Populate from m_fallbackPaths
    for (auto& p : m_fallbackPaths)
      SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)p.c_str());
    PAGE_CTRL(4, hList);
  }
  y += 124;
  {
    int bx = x, bg = 4;
    int fbw = MulDiv(70, lineH, 26);
    PAGE_CTRL(4, CreateBtn(hw, L"Add...", IDC_MW_FILE_ADD, bx, y, fbw, lineH, hFont)); bx += fbw + bg;
    PAGE_CTRL(4, CreateBtn(hw, L"Remove", IDC_MW_FILE_REMOVE, bx, y, fbw, lineH, hFont));
  }
  y += lineH + gap;
  {
    HWND hDesc = CreateWindowExW(0, L"STATIC",
      L"These paths are searched for textures and presets\nin addition to the built-in directories.",
      WS_CHILD | SS_LEFT, x, y, rw, lineH * 2, hw,
      (HMENU)(INT_PTR)IDC_MW_FILE_DESC, GetModuleHandle(NULL), NULL);
    if (hDesc && hFont) SendMessage(hDesc, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(4, hDesc);
  }
  y += lineH * 2 + gap;

  // Random Textures Directory
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"Random Textures Directory:",
      WS_CHILD | SS_LEFT, x, y, rw, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_RANDTEX_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(4, hLbl);
  }
  y += lineH + 2;
  {
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_szRandomTexDir,
      WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_RANDTEX_EDIT,
      GetModuleHandle(NULL), NULL);
    if (hEdit && hFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(4, hEdit);
  }
  y += lineH + 6;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(80, lineH, 26), bw2 = MulDiv(60, lineH, 26);
    PAGE_CTRL(4, CreateBtn(hw, L"Browse...", IDC_MW_RANDTEX_BROWSE, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(4, CreateBtn(hw, L"Clear", IDC_MW_RANDTEX_CLEAR, bx, y, bw2, lineH, hFont));
  }

  // ===== Messages tab (page 5) =====
  y = tabTop + 10;

  // Show Messages / Show Sprites toggles
  {
    int halfW = rw / 2 - 2;
    PAGE_CTRL(5, CreateCheck(hw, L"Show Messages", IDC_MW_MSG_SHOW_MESSAGES, x, y, halfW, lineH, hFont, (m_nSpriteMessagesMode & 1) != 0, false));
    PAGE_CTRL(5, CreateCheck(hw, L"Show Sprites", IDC_MW_MSG_SHOW_SPRITES, x + halfW + 4, y, halfW, lineH, hFont, (m_nSpriteMessagesMode & 2) != 0, false));
  }
  y += lineH + gap;

  PAGE_CTRL(5, CreateLabel(hw, L"Custom Messages:", x, y, rw, lineH, hFont, false));
  y += lineH + 2;
  {
    int listH = 10 * lineH;
    HWND hMsgList = CreateWindowExW(0, L"LISTBOX", L"",
      WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
      x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_MSG_LIST,
      GetModuleHandle(NULL), NULL);
    if (hMsgList && hFont) SendMessage(hMsgList, WM_SETFONT, (WPARAM)hFont, TRUE);
    PopulateMsgListBox(hMsgList);
    PAGE_CTRL(5, hMsgList);
  }
  y += 10 * lineH + 4;

  // Button row 1: Push Now, Up, Down, Add, Edit, Delete, Play
  {
    int bx = x, btnGap = 4;
    int pushW = MulDiv(75, lineH, 26), arrowW = MulDiv(30, lineH, 26);
    int smallW = MulDiv(40, lineH, 26), medW = MulDiv(50, lineH, 26);
    PAGE_CTRL(5, CreateBtn(hw, L"Push Now", IDC_MW_MSG_PUSH, bx, y, pushW, lineH, hFont));
    bx += pushW + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, L"\x25B2", IDC_MW_MSG_UP, bx, y, arrowW, lineH, hFont));
    bx += arrowW + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, L"\x25BC", IDC_MW_MSG_DOWN, bx, y, arrowW, lineH, hFont));
    bx += arrowW + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, L"Add", IDC_MW_MSG_ADD, bx, y, smallW, lineH, hFont));
    bx += smallW + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, L"Edit", IDC_MW_MSG_EDIT, bx, y, smallW, lineH, hFont));
    bx += smallW + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, L"Delete", IDC_MW_MSG_DELETE, bx, y, medW, lineH, hFont));
    bx += medW + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, m_bMsgAutoplay ? L"Stop" : L"Play", IDC_MW_MSG_PLAY, bx, y, medW, lineH, hFont));
  }
  y += lineH + gap;

  {
    int bx = x, btnGap = 4;
    int bw1 = MulDiv(130, lineH, 26), bw2 = MulDiv(55, lineH, 26);
    int bw3 = MulDiv(70, lineH, 26), bw4 = MulDiv(75, lineH, 26);
    PAGE_CTRL(5, CreateBtn(hw, L"Reload from File", IDC_MW_MSG_RELOAD, bx, y, bw1, lineH, hFont)); bx += bw1 + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, L"Paste", IDC_MW_MSG_PASTE, bx, y, bw2, lineH, hFont)); bx += bw2 + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, L"Open INI", IDC_MW_MSG_OPENINI, bx, y, bw3, lineH, hFont)); bx += bw3 + btnGap;
    PAGE_CTRL(5, CreateBtn(hw, L"Overrides", IDC_MW_MSG_OVERRIDES, bx, y, bw4, lineH, hFont));
  }
  y += lineH + gap + 4;

  PAGE_CTRL(5, CreateLabel(hw, L"Font size: 50 = normal, <50 = smaller, >50 = larger (0.01\x2013" L"100)", x, y, rw, lineH, hFont, false));
  y += lineH + gap;

  // Autoplay controls
  PAGE_CTRL(5, CreateCheck(hw, L"Autoplay Messages", IDC_MW_MSG_AUTOPLAY, x, y, rw, lineH, hFont, m_bMsgAutoplay, false));
  y += lineH + 2;
  PAGE_CTRL(5, CreateCheck(hw, L"Sequential Order", IDC_MW_MSG_SEQUENTIAL, x, y, rw, lineH, hFont, m_bMsgSequential, false));
  y += lineH + 2;
  PAGE_CTRL(5, CreateCheck(hw, L"Auto-size messages to fit screen width", IDC_MW_MSG_AUTOSIZE, x, y, rw, lineH, hFont, m_bMessageAutoSize, false));
  y += lineH + gap;

  // Interval + Jitter on same row
  {
    HWND hLbl = CreateLabel(hw, L"Interval (s):", x, y, 90, lineH, hFont, false);
    if (hLbl) SetWindowLongPtr(hLbl, GWL_ID, IDC_MW_MSG_INTERVAL_LBL);
    if (hLbl) m_settingsPageCtrls[5].push_back(hLbl);
  }
  swprintf(buf, 64, L"%.1f", m_fMsgAutoplayInterval);
  PAGE_CTRL(5, CreateEdit(hw, buf, IDC_MW_MSG_INTERVAL, x + 94, y, 60, lineH, hFont, 0));
  {
    HWND hLbl = CreateLabel(hw, L"+/- (s):", x + 170, y, 60, lineH, hFont, false);
    if (hLbl) SetWindowLongPtr(hLbl, GWL_ID, IDC_MW_MSG_JITTER_LBL);
    if (hLbl) m_settingsPageCtrls[5].push_back(hLbl);
  }
  swprintf(buf, 64, L"%.1f", m_fMsgAutoplayJitter);
  PAGE_CTRL(5, CreateEdit(hw, buf, IDC_MW_MSG_JITTER, x + 234, y, 60, lineH, hFont, 0));
  y += lineH + gap;

  // Preview area
  {
    HWND hPrev = CreateWindowExW(0, L"STATIC", L"(select a message to preview)",
      WS_CHILD | SS_LEFT, x, y, rw, lineH * 3, hw,
      (HMENU)(INT_PTR)IDC_MW_MSG_PREVIEW, GetModuleHandle(NULL), NULL);
    if (hPrev && hFont) SendMessage(hPrev, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(5, hPrev);
  }

  // ===== Sprites tab (page 6) =====
  // (built below after About/Remote to keep insertion order, but PAGE_CTRL(6,...) groups them)
  y = tabTop + 10;

  PAGE_CTRL(6, CreateLabel(hw, L"Sprites (sprites.ini):", x, y, rw, lineH, hFontBold, false));
  y += lineH + 2;

  // ListView for sprite entries
  {
    int listH = 8 * lineH;
    m_hSpriteList = CreateWindowExW(0, WC_LISTVIEWW, L"",
      WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
      x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_SPR_LIST,
      GetModuleHandle(NULL), NULL);
    ListView_SetExtendedListViewStyle(m_hSpriteList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    // Create ImageList for thumbnails (32x32)
    m_hSpriteImageList = (void*)ImageList_Create(32, 32, ILC_COLOR32, 100, 10);
    ListView_SetImageList(m_hSpriteList, (HIMAGELIST)m_hSpriteImageList, LVSIL_SMALL);

    // Columns: #, Image, Path
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT; col.cx = 60; col.pszText = (LPWSTR)L"#";
    SendMessageW(m_hSpriteList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = 160; col.pszText = (LPWSTR)L"Image";
    SendMessageW(m_hSpriteList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = rw - 240; col.pszText = (LPWSTR)L"Path";
    SendMessageW(m_hSpriteList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

    if (hFont) SendMessage(m_hSpriteList, WM_SETFONT, (WPARAM)hFont, TRUE);

    if (m_bSettingsDarkTheme) {
      SetWindowTheme(m_hSpriteList, L"", L"");
      ListView_SetBkColor(m_hSpriteList, m_colSettingsCtrlBg);
      ListView_SetTextBkColor(m_hSpriteList, m_colSettingsCtrlBg);
      ListView_SetTextColor(m_hSpriteList, m_colSettingsText);
    }

    LoadSpritesFromINI();
    PopulateSpriteListView();

    PAGE_CTRL(6, m_hSpriteList);
  }
  y += 8 * lineH + 4;

  // Button row 1: Push, Kill, Kill All, Defaults
  {
    int bx = x, bg = 4;
    int bw = MulDiv(55, lineH, 26), bwL = MulDiv(65, lineH, 26);
    PAGE_CTRL(6, CreateBtn(hw, L"Push", IDC_MW_SPR_PUSH, bx, y, bw, lineH, hFont)); bx += bw + bg;
    PAGE_CTRL(6, CreateBtn(hw, L"Kill", IDC_MW_SPR_KILL, bx, y, bw, lineH, hFont)); bx += bw + bg;
    PAGE_CTRL(6, CreateBtn(hw, L"Kill All", IDC_MW_SPR_KILLALL, bx, y, bwL, lineH, hFont)); bx += bwL + bg;
    PAGE_CTRL(6, CreateBtn(hw, L"Defaults", IDC_MW_SPR_DEFAULTS, bx, y, bwL, lineH, hFont));
  }
  y += lineH + gap;

  // Button row 2: Add, Import Folder, Delete, Save, Reload, Open INI
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(50, lineH, 26), bw2 = MulDiv(100, lineH, 26);
    int bw3 = MulDiv(55, lineH, 26), bw4 = MulDiv(60, lineH, 26), bw5 = MulDiv(75, lineH, 26);
    PAGE_CTRL(6, CreateBtn(hw, L"Add", IDC_MW_SPR_ADD, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(6, CreateBtn(hw, L"Import Folder", IDC_MW_SPR_IMPORT, bx, y, bw2, lineH, hFont)); bx += bw2 + bg;
    PAGE_CTRL(6, CreateBtn(hw, L"Delete", IDC_MW_SPR_DELETE, bx, y, bw3, lineH, hFont)); bx += bw3 + bg;
    PAGE_CTRL(6, CreateBtn(hw, L"Save", IDC_MW_SPR_SAVE, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(6, CreateBtn(hw, L"Reload", IDC_MW_SPR_RELOAD, bx, y, bw4, lineH, hFont)); bx += bw4 + bg;
    PAGE_CTRL(6, CreateBtn(hw, L"Open INI", IDC_MW_SPR_OPENINI, bx, y, bw5, lineH, hFont));
  }
  y += lineH + gap + 4;

  // Image path row
  PAGE_CTRL(6, CreateLabel(hw, L"Image:", x, y, 50, lineH, hFont, false));
  PAGE_CTRL(6, CreateEdit(hw, L"", IDC_MW_SPR_IMG_PATH, x + 54, y, rw - 124, lineH, hFont, ES_READONLY, false));
  PAGE_CTRL(6, CreateBtn(hw, L"Browse", IDC_MW_SPR_IMG_BROWSE, x + rw - 65, y, 65, lineH, hFont));
  y += lineH + 2;

  // Properties row 1: Blend, X, Y
  {
    int col1 = x, col2 = x + rw / 3, col3 = x + 2 * rw / 3;
    int editW = 50, labelW = 30;

    PAGE_CTRL(6, CreateLabel(hw, L"Blend:", col1, y, 40, lineH, hFont, false));
    HWND hBlend = CreateWindowExW(0, L"COMBOBOX", L"",
      WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
      col1 + 44, y, 110, 200, hw, (HMENU)(INT_PTR)IDC_MW_SPR_BLENDMODE,
      GetModuleHandle(NULL), NULL);
    if (hBlend && hFont) SendMessage(hBlend, WM_SETFONT, (WPARAM)hFont, TRUE);
    const wchar_t* blendNames[] = { L"0: Blend", L"1: Decal", L"2: Additive", L"3: SrcColor", L"4: ColorKey" };
    for (int i = 0; i < 5; i++) SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)blendNames[i]);
    if (m_bSettingsDarkTheme) SetWindowTheme(hBlend, L"DarkMode_Explorer", NULL);
    PAGE_CTRL(6, hBlend);

    PAGE_CTRL(6, CreateLabel(hw, L"X:", col2, y, labelW, lineH, hFont, false));
    PAGE_CTRL(6, CreateEdit(hw, L"0.5", IDC_MW_SPR_X, col2 + labelW, y, editW, lineH, hFont, 0, false));

    PAGE_CTRL(6, CreateLabel(hw, L"Y:", col3, y, labelW, lineH, hFont, false));
    PAGE_CTRL(6, CreateEdit(hw, L"0.5", IDC_MW_SPR_Y, col3 + labelW, y, editW, lineH, hFont, 0, false));
  }
  y += lineH + 2;

  // Properties row 2: SX, SY, Rot
  {
    int col1 = x, col2 = x + rw / 3, col3 = x + 2 * rw / 3;
    int editW = 50, labelW = 30;

    PAGE_CTRL(6, CreateLabel(hw, L"SX:", col1, y, labelW, lineH, hFont, false));
    PAGE_CTRL(6, CreateEdit(hw, L"1.0", IDC_MW_SPR_SX, col1 + labelW, y, editW, lineH, hFont, 0, false));

    PAGE_CTRL(6, CreateLabel(hw, L"SY:", col2, y, labelW, lineH, hFont, false));
    PAGE_CTRL(6, CreateEdit(hw, L"1.0", IDC_MW_SPR_SY, col2 + labelW, y, editW, lineH, hFont, 0, false));

    PAGE_CTRL(6, CreateLabel(hw, L"Rot:", col3, y, labelW + 4, lineH, hFont, false));
    PAGE_CTRL(6, CreateEdit(hw, L"0", IDC_MW_SPR_ROT, col3 + labelW + 4, y, editW, lineH, hFont, 0, false));
  }
  y += lineH + 2;

  // Properties row 3: R, G, B, A
  {
    int gap2 = 4, labelW2 = 16, editW = 40;
    int cx = x;
    PAGE_CTRL(6, CreateLabel(hw, L"R:", cx, y, labelW2, lineH, hFont, false)); cx += labelW2;
    PAGE_CTRL(6, CreateEdit(hw, L"1", IDC_MW_SPR_R, cx, y, editW, lineH, hFont, 0, false)); cx += editW + gap2;
    PAGE_CTRL(6, CreateLabel(hw, L"G:", cx, y, labelW2, lineH, hFont, false)); cx += labelW2;
    PAGE_CTRL(6, CreateEdit(hw, L"1", IDC_MW_SPR_G, cx, y, editW, lineH, hFont, 0, false)); cx += editW + gap2;
    PAGE_CTRL(6, CreateLabel(hw, L"B:", cx, y, labelW2, lineH, hFont, false)); cx += labelW2;
    PAGE_CTRL(6, CreateEdit(hw, L"1", IDC_MW_SPR_B, cx, y, editW, lineH, hFont, 0, false)); cx += editW + gap2;
    PAGE_CTRL(6, CreateLabel(hw, L"A:", cx, y, labelW2, lineH, hFont, false)); cx += labelW2;
    PAGE_CTRL(6, CreateEdit(hw, L"1", IDC_MW_SPR_A, cx, y, editW, lineH, hFont, 0, false));
  }
  y += lineH + 2;

  // Properties row 4: FlipX, FlipY, Burn, RepeatX, RepeatY, Colorkey
  {
    int cx = x;
    PAGE_CTRL(6, CreateCheck(hw, L"FlipX", IDC_MW_SPR_FLIPX, cx, y, 55, lineH, hFont, false, false)); cx += 59;
    PAGE_CTRL(6, CreateCheck(hw, L"FlipY", IDC_MW_SPR_FLIPY, cx, y, 55, lineH, hFont, false, false)); cx += 59;
    PAGE_CTRL(6, CreateCheck(hw, L"Burn", IDC_MW_SPR_BURN, cx, y, 50, lineH, hFont, false, false)); cx += 54;
    PAGE_CTRL(6, CreateLabel(hw, L"RepX:", cx, y, 40, lineH, hFont, false)); cx += 40;
    PAGE_CTRL(6, CreateEdit(hw, L"1", IDC_MW_SPR_REPEATX, cx, y, 35, lineH, hFont, 0, false)); cx += 39;
    PAGE_CTRL(6, CreateLabel(hw, L"RepY:", cx, y, 40, lineH, hFont, false)); cx += 40;
    PAGE_CTRL(6, CreateEdit(hw, L"1", IDC_MW_SPR_REPEATY, cx, y, 35, lineH, hFont, 0, false)); cx += 39;
    PAGE_CTRL(6, CreateLabel(hw, L"CK:", cx, y, 25, lineH, hFont, false)); cx += 25;
    PAGE_CTRL(6, CreateEdit(hw, L"0x000000", IDC_MW_SPR_COLORKEY, cx, y, 70, lineH, hFont, 0, false));
  }
  y += lineH + gap;

  // Init Code editor
  PAGE_CTRL(6, CreateLabel(hw, L"Init", x, y, 30, lineH, hFont, false));
  y += lineH;
  {
    int codeH = 4 * lineH;
    PAGE_CTRL(6, CreateEdit(hw, L"", IDC_MW_SPR_INIT_CODE, x, y, rw, codeH, hFont,
      ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, false));
  }
  y += 4 * lineH + 4;

  // Per-Frame Code editor
  PAGE_CTRL(6, CreateLabel(hw, L"Per-Frame", x, y, 80, lineH, hFont, false));
  y += lineH;
  {
    int codeH = 4 * lineH;
    PAGE_CTRL(6, CreateEdit(hw, L"", IDC_MW_SPR_FRAME_CODE, x, y, rw, codeH, hFont,
      ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, false));
  }

  // ===== About tab (page 8) =====
  y = tabTop + 10;

  PAGE_CTRL(8, CreateLabel(hw, L"MDropDX12", x, y, rw, 24, hFontBold, false));
  y += 28;

  {
    wchar_t szVersion[128];
    swprintf(szVersion, 128, L"Version %d.%d-dev", INT_VERSION / 100, INT_SUBVERSION);
    PAGE_CTRL(8, CreateLabel(hw, szVersion, x, y, rw, lineH, hFont, false));
    y += lineH + 4;
  }

  {
    wchar_t szBuild[128];
    wchar_t wDate[32], wTime[32];
    MultiByteToWideChar(CP_ACP, 0, __DATE__, -1, wDate, 32);
    MultiByteToWideChar(CP_ACP, 0, __TIME__, -1, wTime, 32);
    swprintf(szBuild, 128, L"Built: %s  %s", wDate, wTime);
    PAGE_CTRL(8, CreateLabel(hw, szBuild, x, y, rw, lineH, hFont, false));
    y += lineH + 4;
  }

  PAGE_CTRL(8, CreateLabel(hw, L"MilkDrop2-based music visualizer", x, y, rw, lineH, hFont, false));
  y += lineH + 4;
  PAGE_CTRL(8, CreateLabel(hw, L"DirectX 12 / Windows 11 64-bit", x, y, rw, lineH, hFont, false));
  y += lineH + 12;

  // Debug Log Level spin box
  PAGE_CTRL(8, CreateLabel(hw, L"Debug Log Level:", x, y, lw, lineH, hFont, false));
  {
    wchar_t logBuf[16];
    swprintf(logBuf, 16, L"%d", m_LogLevel);
    HWND hEdit = CreateEdit(hw, logBuf, IDC_MW_DEBUG_LOG_LEVEL, x + lw + 4, y, 40, lineH, hFont, ES_NUMBER, false);
    PAGE_CTRL(8, hEdit);
    HWND hSpin = CreateWindowExW(0, UPDOWN_CLASSW, NULL,
      WS_CHILD | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
      0, 0, 0, 0, hw, (HMENU)(INT_PTR)IDC_MW_DEBUG_LOG_LEVEL_SPIN,
      GetModuleHandle(NULL), NULL);
    if (hSpin) {
      SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)hEdit, 0);
      SendMessage(hSpin, UDM_SETRANGE32, 0, 3); // 0=Off, 1=Error, 2=Info, 3=Verbose
      SendMessage(hSpin, UDM_SETPOS32, 0, m_LogLevel);
    }
    PAGE_CTRL(8, hSpin);
  }
  PAGE_CTRL(8, CreateLabel(hw, L"0=Off  1=Error  2=Info  3=Verbose", x + lw + 50, y, rw - lw - 50, lineH, hFont, false));

  // ===== Remote tab (page 7) =====
  y = tabTop + 10;

  // Section header
  PAGE_CTRL(7, CreateLabel(hw, L"Milkwave Remote Compatibility", x, y, rw, lineH, hFontBold, false));
  y += lineH + gap;

  // Info line
  PAGE_CTRL(7, CreateLabel(hw, L"Configure window titles so Milkwave Remote (or other controllers) can discover this instance.", x, y, rw, lineH * 2, hFont, false));
  y += lineH * 2 + gap;

  // Window Title
  PAGE_CTRL(7, CreateLabel(hw, L"Window Title:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(7, CreateEdit(hw, m_szWindowTitle, IDC_MW_IPC_TITLE, x + lw + 4, y, rw - lw - 4, lineH, hFont, 0, false));
  y += lineH + 2;

  // Hint
  PAGE_CTRL(7, CreateLabel(hw, L"(empty = \"MDropDX12 Visualizer\"  |  e.g. \"Milkwave Visualizer\")", x + lw + 4, y, rw - lw - 4, lineH, hFont, false));
  y += lineH + gap;

  // Remote Window Title
  PAGE_CTRL(7, CreateLabel(hw, L"Remote Title:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(7, CreateEdit(hw, m_szRemoteWindowTitle, IDC_MW_IPC_REMOTE_TITLE, x + lw + 4, y, rw - lw - 4, lineH, hFont, 0, false));
  y += lineH + 2;

  // Hint
  PAGE_CTRL(7, CreateLabel(hw, L"(empty = \"MDropDX12 Remote\"  |  e.g. \"Milkwave Remote\")", x + lw + 4, y, rw - lw - 4, lineH, hFont, false));
  y += lineH + gap;

  // Apply button + Capture Screenshot button
  {
    int applyW = MulDiv(150, lineH, 26);
    PAGE_CTRL(7, CreateBtn(hw, L"Apply && Restart IPC", IDC_MW_IPC_APPLY, x, y, applyW, lineH, hFont, false));
    int captureW = MulDiv(130, lineH, 26);
    PAGE_CTRL(7, CreateBtn(hw, L"Save Screenshot...", IDC_MW_IPC_CAPTURE, x + applyW + 8, y, captureW, lineH, hFont, false));
    y += lineH + gap + 8;
  }

  // Active IPC Windows section
  PAGE_CTRL(7, CreateLabel(hw, L"Active IPC Windows", x, y, rw, lineH, hFontBold, false));
  y += lineH + gap;

  // IPC list box
  {
    int listH = lineH * 6;
    HWND hIPCList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
      WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
      x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_LIST, GetModuleHandle(NULL), NULL);
    if (hIPCList && hFont) SendMessage(hIPCList, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(7, hIPCList);
    y += listH + gap;
  }

  // Last Message group box
  {
    int groupH = lineH * 5;
    HWND hGroup = CreateWindowExW(0, L"BUTTON", L"Last message:",
      WS_CHILD | BS_GROUPBOX,
      x, y, rw, groupH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_GROUP,
      GetModuleHandle(NULL), NULL);
    if (hGroup && hFont) SendMessage(hGroup, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(7, hGroup);

    int pad = 8;
    HWND hMsgText = CreateWindowExW(0, L"EDIT", L"",
      WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
      x + pad, y + lineH + 2, rw - pad * 2, groupH - lineH - pad - 2,
      hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_TEXT,
      GetModuleHandle(NULL), NULL);
    if (hMsgText && hFont) SendMessage(hMsgText, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(7, hMsgText);
  }

  // Populate IPC list with current state
  RefreshIPCList(hw);

  // Start IPC message monitor timer (500ms polling)
  SetTimer(hw, IDT_IPC_MONITOR, 500, NULL);

  #undef PAGE_CTRL

  // Show only page 0 initially, hide all others
  ShowSettingsPage(0);
}

void Engine::RefreshIPCList(HWND hSettingsWnd) {
  HWND hList = GetDlgItem(hSettingsWnd, IDC_MW_IPC_LIST);
  if (!hList) return;

  SendMessage(hList, LB_RESETCONTENT, 0, 0);

  if (g_bIPCRunning.load()) {
    wchar_t entry[512];
    swprintf_s(entry, L"%s  \u2014  Running", g_szIPCWindowTitle);
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)entry);
  } else {
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"(no IPC window active)");
  }
}

void Engine::ShowSettingsPage(int page) {
  for (int i = 0; i < SETTINGS_NUM_PAGES; i++) {
    if (i == page) {
      // Show + bring to top z-order so controls above resized siblings receive clicks
      for (HWND h : m_settingsPageCtrls[i])
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    } else {
      for (HWND h : m_settingsPageCtrls[i])
        ShowWindow(h, SW_HIDE);
    }
  }
  m_nSettingsActivePage = page;
}

int Engine::GetSettingsLineHeight() {
  if (!m_hSettingsFont || !m_hSettingsWnd) return 26;
  HDC hdc = GetDC(m_hSettingsWnd);
  if (!hdc) return 26;
  HFONT hOld = (HFONT)SelectObject(hdc, m_hSettingsFont);
  TEXTMETRIC tm = {};
  GetTextMetrics(hdc, &tm);
  SelectObject(hdc, hOld);
  ReleaseDC(m_hSettingsWnd, hdc);
  int h = tm.tmHeight + tm.tmExternalLeading + 6;
  return max(h, 20);
}

void Engine::LayoutSettingsControls() {
  if (!m_hSettingsWnd || !m_hSettingsTab) return;

  RECT rc;
  GetClientRect(m_hSettingsWnd, &rc);
  MoveWindow(m_hSettingsTab, 0, 0, rc.right, rc.bottom, TRUE);

  // Get the content area inside the tab control
  RECT rcDisplay = { 0, 0, rc.right, rc.bottom };
  TabCtrl_AdjustRect(m_hSettingsTab, FALSE, &rcDisplay);
  int lineH = GetSettingsLineHeight();
  int rw = rc.right - 36;  // 16px left + 20px right margin
  int lw = MulDiv(160, lineH, 26);
  int newSliderW = rw - lw - 60;
  if (newSliderW < 80) newSliderW = 80;

  // Stretch preset dir edit + reposition Browse button
  HWND hDir = GetDlgItem(m_hSettingsWnd, IDC_MW_PRESET_DIR);
  if (hDir) {
    RECT r; GetWindowRect(hDir, &r);
    MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
    MoveWindow(hDir, r.left, r.top, rw - 70, r.bottom - r.top, TRUE);
    HWND hBrw = GetDlgItem(m_hSettingsWnd, IDC_MW_BROWSE_DIR);
    if (hBrw) MoveWindow(hBrw, r.left + rw - 65, r.top, 65, r.bottom - r.top, TRUE);
  }

  // Stretch preset listbox
  HWND hList = GetDlgItem(m_hSettingsWnd, IDC_MW_PRESET_LIST);
  if (hList) {
    RECT r; GetWindowRect(hList, &r);
    MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
    MoveWindow(hList, r.left, r.top, rw, r.bottom - r.top, TRUE);
  }

  // Stretch audio combo
  HWND hAudio = GetDlgItem(m_hSettingsWnd, IDC_MW_AUDIO_DEVICE);
  if (hAudio) {
    RECT r; GetWindowRect(hAudio, &r);
    MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
    MoveWindow(hAudio, r.left, r.top, rw, 200, TRUE);
  }

  // Stretch sliders + reposition value labels
  int sliderIDs[] = { IDC_MW_OPACITY, IDC_MW_RENDER_QUALITY, IDC_MW_COL_HUE, IDC_MW_COL_SAT, IDC_MW_COL_BRIGHT };
  int labelIDs[] = { IDC_MW_OPACITY_LABEL, IDC_MW_QUALITY_LABEL, IDC_MW_COL_HUE_LABEL, IDC_MW_COL_SAT_LABEL, IDC_MW_COL_BRIGHT_LABEL };
  for (int i = 0; i < 5; i++) {
    HWND hSlider = GetDlgItem(m_hSettingsWnd, sliderIDs[i]);
    HWND hLabel = GetDlgItem(m_hSettingsWnd, labelIDs[i]);
    if (hSlider) {
      RECT r; GetWindowRect(hSlider, &r);
      MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
      MoveWindow(hSlider, r.left, r.top, newSliderW, r.bottom - r.top, TRUE);
      if (hLabel) MoveWindow(hLabel, r.left + newSliderW + 4, r.top, 50, r.bottom - r.top, TRUE);
    }
  }

  // Stretch Files tab ListBox and reposition buttons + description below it
  HWND hFileList = GetDlgItem(m_hSettingsWnd, IDC_MW_FILE_LIST);
  if (hFileList) {
    RECT r; GetWindowRect(hFileList, &r);
    MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
    int gap = 6;
    int reserveBelow = 4 + lineH + gap + lineH * 2 + gap + lineH + 2 + (lineH + 4) + 6 + lineH;  // buttons + desc + randtex label + edit + browse/clear
    int listBottom = rcDisplay.bottom - reserveBelow;
    if (listBottom < r.top + 40) listBottom = r.top + 40;
    MoveWindow(hFileList, r.left, r.top, rw, listBottom - r.top, TRUE);

    int btnY = listBottom + 4;
    HWND hAdd = GetDlgItem(m_hSettingsWnd, IDC_MW_FILE_ADD);
    HWND hRem = GetDlgItem(m_hSettingsWnd, IDC_MW_FILE_REMOVE);
    int fbw = MulDiv(70, lineH, 26), fbg = 4;
    if (hAdd) MoveWindow(hAdd, r.left, btnY, fbw, lineH, TRUE);
    if (hRem) MoveWindow(hRem, r.left + fbw + fbg, btnY, fbw, lineH, TRUE);

    HWND hDesc = GetDlgItem(m_hSettingsWnd, IDC_MW_FILE_DESC);
    if (hDesc) MoveWindow(hDesc, r.left, btnY + lineH + gap, rw, lineH * 2, TRUE);

    // Random textures directory controls - keep grouped tightly
    int randY = btnY + lineH + gap + lineH * 2 + gap;
    HWND hRandLabel = GetDlgItem(m_hSettingsWnd, IDC_MW_RANDTEX_LABEL);
    HWND hRandEdit = GetDlgItem(m_hSettingsWnd, IDC_MW_RANDTEX_EDIT);
    HWND hRandBrowse = GetDlgItem(m_hSettingsWnd, IDC_MW_RANDTEX_BROWSE);
    HWND hRandClear = GetDlgItem(m_hSettingsWnd, IDC_MW_RANDTEX_CLEAR);
    if (hRandLabel) MoveWindow(hRandLabel, r.left, randY, rw, lineH, TRUE);
    if (hRandEdit) MoveWindow(hRandEdit, r.left, randY + lineH + 2, rw, lineH + 4, TRUE);
    int rbw1 = MulDiv(80, lineH, 26), rbw2 = MulDiv(60, lineH, 26);
    if (hRandBrowse) MoveWindow(hRandBrowse, r.left, randY + lineH + 2 + lineH + 6, rbw1, lineH, TRUE);
    if (hRandClear) MoveWindow(hRandClear, r.left + rbw1 + 4, randY + lineH + 2 + lineH + 6, rbw2, lineH, TRUE);
  }

  // Stretch Messages tab ListBox and reposition all controls below it
  HWND hMsgList = GetDlgItem(m_hSettingsWnd, IDC_MW_MSG_LIST);
  if (hMsgList) {
    RECT r; GetWindowRect(hMsgList, &r);
    MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
    int gap = 6;
    // Reserve: gap + buttons + gap + reload + gap+4 + autoplay + 2 + sequential + gap + interval + gap + preview
    int reserveBelow = 4 + (lineH + gap) + (lineH + gap + 4) + (lineH + 2) + (lineH + gap) + (lineH + gap) + lineH * 3;
    int listBottom = rcDisplay.bottom - reserveBelow;
    if (listBottom < r.top + 60) listBottom = r.top + 60;
    MoveWindow(hMsgList, r.left, r.top, rw, listBottom - r.top, TRUE);

    // Reposition all controls below the resized listbox
    int y = listBottom + 4;
    int x = r.left;
    // Button row
    int bx = x, btnGap = 4;
    int pushW = MulDiv(75, lineH, 26), arrowW = MulDiv(30, lineH, 26);
    int smallW = MulDiv(40, lineH, 26), medW = MulDiv(50, lineH, 26);
    auto moveCtrl = [&](int id, int cx, int cy, int cw, int ch) {
      HWND h = GetDlgItem(m_hSettingsWnd, id);
      if (h) MoveWindow(h, cx, cy, cw, ch, TRUE);
    };
    moveCtrl(IDC_MW_MSG_PUSH, bx, y, pushW, lineH); bx += pushW + btnGap;
    moveCtrl(IDC_MW_MSG_UP, bx, y, arrowW, lineH); bx += arrowW + btnGap;
    moveCtrl(IDC_MW_MSG_DOWN, bx, y, arrowW, lineH); bx += arrowW + btnGap;
    moveCtrl(IDC_MW_MSG_ADD, bx, y, smallW, lineH); bx += smallW + btnGap;
    moveCtrl(IDC_MW_MSG_EDIT, bx, y, smallW, lineH); bx += smallW + btnGap;
    moveCtrl(IDC_MW_MSG_DELETE, bx, y, medW, lineH); bx += medW + btnGap;
    moveCtrl(IDC_MW_MSG_PLAY, bx, y, medW, lineH);
    y += lineH + gap;
    // Reload + Paste + Open INI + Overrides
    {
      int rbx = x;
      int bw1 = MulDiv(130, lineH, 26), bw2 = MulDiv(55, lineH, 26);
      int bw3 = MulDiv(70, lineH, 26), bw4 = MulDiv(75, lineH, 26);
      moveCtrl(IDC_MW_MSG_RELOAD, rbx, y, bw1, lineH); rbx += bw1 + btnGap;
      moveCtrl(IDC_MW_MSG_PASTE, rbx, y, bw2, lineH); rbx += bw2 + btnGap;
      moveCtrl(IDC_MW_MSG_OPENINI, rbx, y, bw3, lineH); rbx += bw3 + btnGap;
      moveCtrl(IDC_MW_MSG_OVERRIDES, rbx, y, bw4, lineH);
    }
    y += lineH + gap + 4;
    // Checkboxes
    moveCtrl(IDC_MW_MSG_AUTOPLAY, x, y, rw, lineH);
    y += lineH + 2;
    moveCtrl(IDC_MW_MSG_SEQUENTIAL, x, y, rw, lineH);
    y += lineH + 2;
    moveCtrl(IDC_MW_MSG_AUTOSIZE, x, y, rw, lineH);
    y += lineH + gap;
    // Interval + Jitter labels and edits
    moveCtrl(IDC_MW_MSG_INTERVAL_LBL, x, y, 90, lineH);
    moveCtrl(IDC_MW_MSG_INTERVAL, x + 94, y, 60, lineH);
    moveCtrl(IDC_MW_MSG_JITTER_LBL, x + 170, y, 60, lineH);
    moveCtrl(IDC_MW_MSG_JITTER, x + 234, y, 60, lineH);
    y += lineH + gap;
    // Preview
    moveCtrl(IDC_MW_MSG_PREVIEW, x, y, rw, lineH * 3);
  }

  // Stretch Sprites tab ListView to fill width
  if (m_hSpriteList) {
    RECT r; GetWindowRect(m_hSpriteList, &r);
    MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
    MoveWindow(m_hSpriteList, r.left, r.top, rw, r.bottom - r.top, TRUE);
    // Resize the Path column to fill remaining width
    int col0w = (int)SendMessageW(m_hSpriteList, LVM_GETCOLUMNWIDTH, 0, 0);
    int col1w = (int)SendMessageW(m_hSpriteList, LVM_GETCOLUMNWIDTH, 1, 0);
    int col2w = rw - col0w - col1w - 4;
    if (col2w > 50)
      SendMessageW(m_hSpriteList, LVM_SETCOLUMNWIDTH, 2, col2w);
  }
  // Stretch sprite image path edit to fill width
  {
    HWND hPath = GetDlgItem(m_hSettingsWnd, IDC_MW_SPR_IMG_PATH);
    HWND hBrowse = GetDlgItem(m_hSettingsWnd, IDC_MW_SPR_IMG_BROWSE);
    if (hPath && hBrowse) {
      RECT rp; GetWindowRect(hPath, &rp);
      MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&rp, 2);
      int browseW = 65;
      int pathW = rw - (rp.left - rcDisplay.left) - browseW - 4;
      if (pathW > 50) {
        MoveWindow(hPath, rp.left, rp.top, pathW, rp.bottom - rp.top, TRUE);
        RECT rb; GetWindowRect(hBrowse, &rb);
        MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&rb, 2);
        MoveWindow(hBrowse, rp.left + pathW + 4, rb.top, browseW, rb.bottom - rb.top, TRUE);
      }
    }
  }
  // Stretch sprite code editors to fill width
  {
    auto stretchEdit = [&](int id) {
      HWND h = GetDlgItem(m_hSettingsWnd, id);
      if (!h) return;
      RECT r; GetWindowRect(h, &r);
      MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
      MoveWindow(h, r.left, r.top, rw, r.bottom - r.top, TRUE);
    };
    stretchEdit(IDC_MW_SPR_INIT_CODE);
    stretchEdit(IDC_MW_SPR_FRAME_CODE);
  }

  // Stretch Remote tab edit fields and list box to fill width
  {
    int lw = MulDiv(160, lineH, 26);
    auto moveCtrl = [&](int id, int cx, int cy, int cw, int ch) {
      HWND h = GetDlgItem(m_hSettingsWnd, id);
      if (!h) return;
      RECT r; GetWindowRect(h, &r);
      MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&r, 2);
      if (cx < 0) cx = r.left;
      if (cy < 0) cy = r.top;
      if (cw < 0) cw = r.right - r.left;
      if (ch < 0) ch = r.bottom - r.top;
      MoveWindow(h, cx, cy, cw, ch, TRUE);
    };
    int editX = rcDisplay.left + 16 + lw + 4;
    int editW = rw - lw - 4;
    moveCtrl(IDC_MW_IPC_TITLE, editX, -1, editW, -1);
    moveCtrl(IDC_MW_IPC_REMOTE_TITLE, editX, -1, editW, -1);
    moveCtrl(IDC_MW_IPC_LIST, rcDisplay.left + 16, -1, rw, -1);
    moveCtrl(IDC_MW_IPC_MSG_GROUP, rcDisplay.left + 16, -1, rw, -1);
    // Stretch message text inside group box
    HWND hGroup = GetDlgItem(m_hSettingsWnd, IDC_MW_IPC_MSG_GROUP);
    if (hGroup) {
      RECT rg; GetWindowRect(hGroup, &rg);
      MapWindowPoints(NULL, m_hSettingsWnd, (POINT*)&rg, 2);
      int pad = 8;
      moveCtrl(IDC_MW_IPC_MSG_TEXT, rg.left + pad, rg.top + lineH + 2,
               rg.right - rg.left - pad * 2, rg.bottom - rg.top - lineH - pad - 2);
    }
  }

  InvalidateRect(m_hSettingsWnd, NULL, TRUE);
}

void Engine::CloseSettingsWindow() {
  if (m_hSettingsWnd && IsWindow(m_hSettingsWnd)) {
    PostMessage(m_hSettingsWnd, WM_CLOSE, 0, 0);
  }
  if (m_settingsThread.joinable())
    m_settingsThread.join();
}

// ====== User Defaults & Fallback Paths ======

void Engine::UpdateVisualUI(HWND hWnd) {
  wchar_t buf[32];
  SendMessage(GetDlgItem(hWnd, IDC_MW_OPACITY), TBM_SETPOS, TRUE, (int)(fOpacity * 100));
  swprintf(buf, 32, L"%d%%", (int)(fOpacity * 100));
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_OPACITY_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_RENDER_QUALITY), TBM_SETPOS, TRUE, (int)(m_fRenderQuality * 100));
  swprintf(buf, 32, L"%.2f", m_fRenderQuality);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_QUALITY_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_QUALITY_AUTO), BM_SETCHECK, bQualityAuto ? BST_CHECKED : BST_UNCHECKED, 0);
  swprintf(buf, 32, L"%.2f", m_timeFactor);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_TIME_FACTOR), buf);
  swprintf(buf, 32, L"%.2f", m_frameFactor);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_FRAME_FACTOR), buf);
  swprintf(buf, 32, L"%.2f", m_fpsFactor);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_FPS_FACTOR), buf);
  swprintf(buf, 32, L"%.2f", m_VisIntensity);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_INTENSITY), buf);
  swprintf(buf, 32, L"%.2f", m_VisShift);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_SHIFT), buf);
  swprintf(buf, 32, L"%.0f", m_VisVersion);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_VERSION), buf);
  // GPU Protection controls
  swprintf(buf, 32, L"%d", m_nMaxShapeInstances);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_MAX_INST), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BY_RES), BM_SETCHECK, m_bScaleInstancesByResolution ? BST_CHECKED : BST_UNCHECKED, 0);
  swprintf(buf, 32, L"%d", m_nInstanceScaleBaseWidth);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BASE), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_GPU_SKIP_HEAVY), BM_SETCHECK, m_bSkipHeavyPresets ? BST_CHECKED : BST_UNCHECKED, 0);
  swprintf(buf, 32, L"%d", m_nHeavyPresetMaxInstances);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_HEAVY_THRESHOLD), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_VSYNC_ENABLED), BM_SETCHECK, m_bEnableVSync ? BST_CHECKED : BST_UNCHECKED, 0);
  HWND hw = GetPluginWindow();
  if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
  if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
}

void Engine::UpdateColorsUI(HWND hWnd) {
  wchar_t buf[32];
  SendMessage(GetDlgItem(hWnd, IDC_MW_COL_HUE), TBM_SETPOS, TRUE, (int)(m_ColShiftHue * 100) + 100);
  swprintf(buf, 32, L"%.2f", m_ColShiftHue);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_HUE_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_COL_SAT), TBM_SETPOS, TRUE, (int)(m_ColShiftSaturation * 100) + 100);
  swprintf(buf, 32, L"%.2f", m_ColShiftSaturation);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_SAT_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT), TBM_SETPOS, TRUE, (int)(m_ColShiftBrightness * 100) + 100);
  swprintf(buf, 32, L"%.2f", m_ColShiftBrightness);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT_LABEL), buf);
  float gamma = m_pState ? m_pState->m_fGammaAdj.eval(-1) : 2.0f;
  SendMessage(GetDlgItem(hWnd, IDC_MW_COL_GAMMA), TBM_SETPOS, TRUE, (int)(gamma * 10));
  swprintf(buf, 32, L"%.1f", gamma);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_GAMMA_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_AUTO_HUE), BM_SETCHECK, m_AutoHue ? BST_CHECKED : BST_UNCHECKED, 0);
  swprintf(buf, 32, L"%.3f", m_AutoHueSeconds);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_AUTO_HUE_SEC), buf);
}

void Engine::ResetToFactory(HWND hWnd) {
  // Visual defaults
  fOpacity = 1.0f;
  m_fRenderQuality = 1.0f;
  bQualityAuto = false;
  m_timeFactor = 1.0f;
  m_frameFactor = 1.0f;
  m_fpsFactor = 1.0f;
  m_VisIntensity = 1.0f;
  m_VisShift = 0.0f;
  m_VisVersion = 1.0f;
  // Color defaults
  m_ColShiftHue = 0.0f;
  m_ColShiftSaturation = 0.0f;
  m_ColShiftBrightness = 0.0f;
  if (m_pState) m_pState->m_fGammaAdj = 2.0f;
  m_AutoHue = false;
  m_AutoHueSeconds = 0.02f;
  // Update UI
  UpdateVisualUI(hWnd);
  UpdateColorsUI(hWnd);
}

void Engine::SaveUserDefaults() {
  // Copy current values with safety clamps
  m_udOpacity = max(fOpacity, 0.5f);
  m_udRenderQuality = m_fRenderQuality;
  m_udTimeFactor = m_timeFactor;
  m_udFrameFactor = m_frameFactor;
  m_udFpsFactor = m_fpsFactor;
  m_udVisIntensity = m_VisIntensity;
  m_udVisShift = m_VisShift;
  m_udVisVersion = m_VisVersion;
  m_udHue = m_ColShiftHue;
  m_udSaturation = m_ColShiftSaturation;
  m_udBrightness = max(m_ColShiftBrightness, -1.0f);
  m_udGamma = max(m_pState ? m_pState->m_fGammaAdj.eval(-1) : 2.0f, 0.5f);
  m_bUserDefaultsSaved = true;

  // Write to INI
  wchar_t* pIni = GetConfigIniFile();
  wchar_t buf[32];
  WritePrivateProfileStringW(L"UserDefaults", L"Saved", L"1", pIni);
  #define WRITE_UD_FLOAT(key, val) swprintf(buf, 32, L"%.4f", val); WritePrivateProfileStringW(L"UserDefaults", key, buf, pIni)
  WRITE_UD_FLOAT(L"Opacity", m_udOpacity);
  WRITE_UD_FLOAT(L"RenderQuality", m_udRenderQuality);
  WRITE_UD_FLOAT(L"TimeFactor", m_udTimeFactor);
  WRITE_UD_FLOAT(L"FrameFactor", m_udFrameFactor);
  WRITE_UD_FLOAT(L"FpsFactor", m_udFpsFactor);
  WRITE_UD_FLOAT(L"VisIntensity", m_udVisIntensity);
  WRITE_UD_FLOAT(L"VisShift", m_udVisShift);
  WRITE_UD_FLOAT(L"VisVersion", m_udVisVersion);
  WRITE_UD_FLOAT(L"Hue", m_udHue);
  WRITE_UD_FLOAT(L"Saturation", m_udSaturation);
  WRITE_UD_FLOAT(L"Brightness", m_udBrightness);
  WRITE_UD_FLOAT(L"Gamma", m_udGamma);
  #undef WRITE_UD_FLOAT
}

void Engine::LoadUserDefaults() {
  wchar_t* pIni = GetConfigIniFile();
  wchar_t buf[32];
  m_bUserDefaultsSaved = GetPrivateProfileIntW(L"UserDefaults", L"Saved", 0, pIni) != 0;
  if (!m_bUserDefaultsSaved) return;

  #define READ_UD_FLOAT(key, dest, def) GetPrivateProfileStringW(L"UserDefaults", key, L"", buf, 32, pIni); dest = buf[0] ? (float)_wtof(buf) : def
  READ_UD_FLOAT(L"Opacity", m_udOpacity, 1.0f);
  READ_UD_FLOAT(L"RenderQuality", m_udRenderQuality, 1.0f);
  READ_UD_FLOAT(L"TimeFactor", m_udTimeFactor, 1.0f);
  READ_UD_FLOAT(L"FrameFactor", m_udFrameFactor, 1.0f);
  READ_UD_FLOAT(L"FpsFactor", m_udFpsFactor, 1.0f);
  READ_UD_FLOAT(L"VisIntensity", m_udVisIntensity, 1.0f);
  READ_UD_FLOAT(L"VisShift", m_udVisShift, 0.0f);
  READ_UD_FLOAT(L"VisVersion", m_udVisVersion, 1.0f);
  READ_UD_FLOAT(L"Hue", m_udHue, 0.0f);
  READ_UD_FLOAT(L"Saturation", m_udSaturation, 0.0f);
  READ_UD_FLOAT(L"Brightness", m_udBrightness, 0.0f);
  READ_UD_FLOAT(L"Gamma", m_udGamma, 2.0f);
  #undef READ_UD_FLOAT
}

void Engine::ResetToUserDefaults(HWND hWnd) {
  if (!m_bUserDefaultsSaved) {
    ResetToFactory(hWnd);
    return;
  }
  // Visual
  fOpacity = m_udOpacity;
  m_fRenderQuality = m_udRenderQuality;
  bQualityAuto = false;
  m_timeFactor = m_udTimeFactor;
  m_frameFactor = m_udFrameFactor;
  m_fpsFactor = m_udFpsFactor;
  m_VisIntensity = m_udVisIntensity;
  m_VisShift = m_udVisShift;
  m_VisVersion = m_udVisVersion;
  // Colors
  m_ColShiftHue = m_udHue;
  m_ColShiftSaturation = m_udSaturation;
  m_ColShiftBrightness = m_udBrightness;
  if (m_pState) m_pState->m_fGammaAdj = m_udGamma;
  m_AutoHue = false;
  m_AutoHueSeconds = 0.02f;
  // Update UI
  UpdateVisualUI(hWnd);
  UpdateColorsUI(hWnd);
}

void Engine::SaveFallbackPaths() {
  wchar_t* pIni = GetConfigIniFile();
  wchar_t buf[32];
  swprintf(buf, 32, L"%d", (int)m_fallbackPaths.size());
  WritePrivateProfileStringW(L"FallbackPaths", L"Count", buf, pIni);
  for (int i = 0; i < (int)m_fallbackPaths.size(); i++) {
    wchar_t key[32];
    swprintf(key, 32, L"Path%d", i);
    WritePrivateProfileStringW(L"FallbackPaths", key, m_fallbackPaths[i].c_str(), pIni);
  }
  // Clean up old entries beyond current count
  for (int i = (int)m_fallbackPaths.size(); i < 20; i++) {
    wchar_t key[32];
    swprintf(key, 32, L"Path%d", i);
    WritePrivateProfileStringW(L"FallbackPaths", key, NULL, pIni);
  }
  // Random textures directory
  WritePrivateProfileStringW(L"FallbackPaths", L"RandomTexDir",
    m_szRandomTexDir[0] ? m_szRandomTexDir : NULL, pIni);
  // Content base path
  WritePrivateProfileStringW(L"FallbackPaths", L"ContentBasePath",
    m_szContentBasePath[0] ? m_szContentBasePath : NULL, pIni);
}

void Engine::LoadFallbackPaths() {
  wchar_t* pIni = GetConfigIniFile();
  int count = GetPrivateProfileIntW(L"FallbackPaths", L"Count", 0, pIni);
  m_fallbackPaths.clear();
  for (int i = 0; i < count && i < 20; i++) {
    wchar_t key[32], val[MAX_PATH] = {};
    swprintf(key, 32, L"Path%d", i);
    GetPrivateProfileStringW(L"FallbackPaths", key, L"", val, MAX_PATH, pIni);
    if (val[0])
      m_fallbackPaths.push_back(val);
  }
  // Random textures directory
  GetPrivateProfileStringW(L"FallbackPaths", L"RandomTexDir", L"", m_szRandomTexDir, MAX_PATH, pIni);
  // Content base path
  GetPrivateProfileStringW(L"FallbackPaths", L"ContentBasePath", L"", m_szContentBasePath, MAX_PATH, pIni);
}

// ====== Settings Fullscreen Awareness ======

struct EnumMonitorData {
  HMONITOR hExclude;   // monitor to skip (the one render is on)
  RECT     rcResult;   // work area of first alternate monitor found
  bool     bFound;
};

static BOOL CALLBACK FindAltMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
  EnumMonitorData* d = (EnumMonitorData*)lp;
  if (hMon == d->hExclude) return TRUE; // skip render monitor
  MONITORINFO mi = { sizeof(mi) };
  if (GetMonitorInfo(hMon, &mi)) {
    d->rcResult = mi.rcWork;
    d->bFound = true;
    return FALSE; // stop enumerating
  }
  return TRUE;
}

void Engine::ResetSettingsWindow() {
  if (!m_hSettingsWnd || !IsWindow(m_hSettingsWnd)) return;

  m_nSettingsWndW = 620;
  m_nSettingsWndH = 700;

  // Center on the monitor the settings window is currently on
  HMONITOR hMon = MonitorFromWindow(m_hSettingsWnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  if (GetMonitorInfo(hMon, &mi)) {
    int monW = mi.rcWork.right - mi.rcWork.left;
    int monH = mi.rcWork.bottom - mi.rcWork.top;
    int posX = mi.rcWork.left + (monW - m_nSettingsWndW) / 2;
    int posY = mi.rcWork.top + (monH - m_nSettingsWndH) / 2;
    SetWindowPos(m_hSettingsWnd, HWND_TOPMOST, posX, posY, m_nSettingsWndW, m_nSettingsWndH, SWP_SHOWWINDOW);
  } else {
    SetWindowPos(m_hSettingsWnd, HWND_TOPMOST, 0, 0, m_nSettingsWndW, m_nSettingsWndH, SWP_NOMOVE | SWP_SHOWWINDOW);
  }
  LayoutSettingsControls();
}

static BOOL CALLBACK SetFontProc(HWND hChild, LPARAM lParam) {
  SendMessage(hChild, WM_SETFONT, (WPARAM)lParam, TRUE);
  return TRUE;
}

void Engine::RebuildSettingsFonts() {
  if (!m_hSettingsWnd) return;

  // Save current tab selection
  int curTab = m_hSettingsTab ? TabCtrl_GetCurSel(m_hSettingsTab) : 0;

  // Destroy all child windows (tab, controls, etc.)
  HWND hChild = GetWindow(m_hSettingsWnd, GW_CHILD);
  while (hChild) {
    HWND hNext = GetWindow(hChild, GW_HWNDNEXT);
    DestroyWindow(hChild);
    hChild = hNext;
  }
  m_hSettingsTab = NULL;
  for (int i = 0; i < SETTINGS_NUM_PAGES; i++) m_settingsPageCtrls[i].clear();
  m_hSpriteList = NULL;
  if (m_hSpriteImageList) { ImageList_Destroy((HIMAGELIST)m_hSpriteImageList); m_hSpriteImageList = NULL; }

  // Rebuild all controls at the new font size
  // (BuildSettingsControls recreates fonts, tab, and all controls)
  BuildSettingsControls();
  ApplySettingsDarkTheme();

  // Restore tab selection
  if (m_hSettingsTab && curTab > 0) {
    TabCtrl_SetCurSel(m_hSettingsTab, curTab);
    ShowSettingsPage(curTab);
  }
}

void Engine::NavigatePresetDirUp(HWND hSettingsWnd) {
  wchar_t* pDir = GetPresetDir();
  int dirLen = lstrlenW(pDir);

  // Strip trailing backslash, find previous one, truncate after it
  wchar_t* p2 = wcsrchr(pDir, L'\\');
  if (p2 && p2 > pDir) {
    *p2 = 0;
    p2 = wcsrchr(pDir, L'\\');
    if (p2) *(p2 + 1) = 0;
    else lstrcatW(pDir, L"\\");  // keep drive root as "X:\"
  } else {
    return;  // nowhere to go
  }
  WritePrivateProfileStringW(L"Settings", L"szPresetDir", pDir, GetConfigIniFile());
  UpdatePresetList(false, true, false);
  m_nCurrentPreset = -1;

  if (hSettingsWnd) {
    SetWindowTextW(GetDlgItem(hSettingsWnd, IDC_MW_PRESET_DIR), pDir);
    HWND hList = GetDlgItem(hSettingsWnd, IDC_MW_PRESET_LIST);
    if (hList) {
      SendMessage(hList, LB_RESETCONTENT, 0, 0);
      for (int i = 0; i < m_nPresets; i++) {
        if (m_presets[i].szFilename.empty()) continue;
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)m_presets[i].szFilename.c_str());
      }
    }
  }
}

void Engine::NavigatePresetDirInto(HWND hSettingsWnd, int sel) {
  if (sel < 0 || sel >= m_nPresets) return;
  if (m_presets[sel].szFilename.c_str()[0] != L'*') return;

  wchar_t* pDir = GetPresetDir();
  lstrcatW(pDir, &m_presets[sel].szFilename.c_str()[1]);
  lstrcatW(pDir, L"\\");
  WritePrivateProfileStringW(L"Settings", L"szPresetDir", pDir, GetConfigIniFile());
  UpdatePresetList(false, true, false);
  m_nCurrentPreset = -1;

  if (hSettingsWnd) {
    SetWindowTextW(GetDlgItem(hSettingsWnd, IDC_MW_PRESET_DIR), pDir);
    HWND hList = GetDlgItem(hSettingsWnd, IDC_MW_PRESET_LIST);
    if (hList) {
      SendMessage(hList, LB_RESETCONTENT, 0, 0);
      for (int i = 0; i < m_nPresets; i++) {
        if (m_presets[i].szFilename.empty()) continue;
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)m_presets[i].szFilename.c_str());
      }
    }
  }
}

void Engine::EnsureSettingsVisible() {
  if (!m_hSettingsWnd || !IsWindow(m_hSettingsWnd) || !IsWindowVisible(m_hSettingsWnd))
    return;

  HWND hRender = GetPluginWindow();
  if (!hRender) return;

  HMONITOR hRenderMon = MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST);
  HMONITOR hSettingsMon = MonitorFromWindow(m_hSettingsWnd, MONITOR_DEFAULTTONEAREST);

  // Only act if both windows are on the same monitor AND render is fullscreen
  if (hRenderMon != hSettingsMon || !IsBorderlessFullscreen(hRender)) {
    SetForegroundWindow(m_hSettingsWnd);
    return;
  }

  // Try to find an alternate monitor
  EnumMonitorData emd = {};
  emd.hExclude = hRenderMon;
  emd.bFound = false;
  EnumDisplayMonitors(NULL, NULL, FindAltMonitorProc, (LPARAM)&emd);

  if (emd.bFound) {
    // Move settings window to center of the alternate monitor's work area
    int monW = emd.rcResult.right - emd.rcResult.left;
    int monH = emd.rcResult.bottom - emd.rcResult.top;
    int wx = emd.rcResult.left + (monW - m_nSettingsWndW) / 2;
    int wy = emd.rcResult.top + (monH - m_nSettingsWndH) / 2;
    SetWindowPos(m_hSettingsWnd, HWND_TOPMOST, wx, wy, m_nSettingsWndW, m_nSettingsWndH, SWP_SHOWWINDOW);
  } else {
    // Single monitor — just bring to foreground
    SetForegroundWindow(m_hSettingsWnd);
  }
}

// ====== Resource Viewer ======

static bool g_bResourceViewerClassRegistered = false;

void Engine::OpenResourceViewer() {
  if (m_hResourceWnd && IsWindow(m_hResourceWnd)) {
    ShowWindow(m_hResourceWnd, SW_SHOW);
    SetForegroundWindow(m_hResourceWnd);
    PopulateResourceViewer();
    return;
  }

  if (!g_bResourceViewerClassRegistered) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ResourceViewerWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = m_bSettingsDarkTheme ? CreateSolidBrush(m_colSettingsBg) : (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = L"MDropDX12ResourceViewer";
    RegisterClassExW(&wc);
    g_bResourceViewerClassRegistered = true;
  }

  m_hResourceWnd = CreateWindowExW(
    WS_EX_TOOLWINDOW,
    L"MDropDX12ResourceViewer",
    L"Resource Viewer",
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
    CW_USEDEFAULT, CW_USEDEFAULT, 750, 420,
    m_hSettingsWnd,
    NULL,
    GetModuleHandle(NULL),
    NULL);

  SetWindowLongPtrW(m_hResourceWnd, GWLP_USERDATA, (LONG_PTR)this);

  m_hResourceList = CreateWindowExW(
    0,
    WC_LISTVIEWW,
    L"",
    WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
    0, 0, 100, 100,
    m_hResourceWnd,
    (HMENU)(INT_PTR)IDC_RV_LISTVIEW,
    GetModuleHandle(NULL),
    NULL);

  ListView_SetExtendedListViewStyle(m_hResourceList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

  // Add columns
  LVCOLUMNW col = {};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
  col.fmt = LVCFMT_CENTER;
  col.cx = 32;
  col.pszText = (LPWSTR)L"";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);

  col.fmt = LVCFMT_LEFT;
  col.cx = 90;
  col.pszText = (LPWSTR)L"Type";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

  col.cx = 150;
  col.pszText = (LPWSTR)L"Name";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

  col.cx = 320;
  col.pszText = (LPWSTR)L"Path";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

  col.cx = 90;
  col.pszText = (LPWSTR)L"Details";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);

  // Create buttons (owner-draw for dark theme painting)
  CreateWindowExW(0, L"BUTTON", L"\u2702", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
    0, 0, 36, 28, m_hResourceWnd, (HMENU)(INT_PTR)IDC_RV_COPY_PATH, GetModuleHandle(NULL), NULL);
  CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
    0, 0, 70, 28, m_hResourceWnd, (HMENU)(INT_PTR)IDC_RV_REFRESH, GetModuleHandle(NULL), NULL);

  // Set font on ListView and buttons
  if (m_hSettingsFont) {
    SendMessage(m_hResourceList, WM_SETFONT, (WPARAM)m_hSettingsFont, TRUE);
    SendMessage(GetDlgItem(m_hResourceWnd, IDC_RV_COPY_PATH), WM_SETFONT, (WPARAM)m_hSettingsFont, TRUE);
    SendMessage(GetDlgItem(m_hResourceWnd, IDC_RV_REFRESH), WM_SETFONT, (WPARAM)m_hSettingsFont, TRUE);
  }

  // Apply dark theme to resource viewer
  if (m_bSettingsDarkTheme) {
    BOOL bDark = TRUE;
    DwmSetWindowAttribute(m_hResourceWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &bDark, sizeof(bDark));
    DwmSetWindowAttribute(m_hResourceWnd, 35 /* DWMWA_CAPTION_COLOR */, &m_colSettingsBg, sizeof(m_colSettingsBg));
    DwmSetWindowAttribute(m_hResourceWnd, 34 /* DWMWA_BORDER_COLOR */, &m_colSettingsBorder, sizeof(m_colSettingsBorder));
    DwmSetWindowAttribute(m_hResourceWnd, 36 /* DWMWA_TEXT_COLOR */, &m_colSettingsText, sizeof(m_colSettingsText));

    // Strip visual styles so custom painting works reliably
    SetWindowTheme(m_hResourceList, L"", L"");
    HWND hCopy = GetDlgItem(m_hResourceWnd, IDC_RV_COPY_PATH);
    HWND hRefresh = GetDlgItem(m_hResourceWnd, IDC_RV_REFRESH);
    if (hCopy) SetWindowTheme(hCopy, L"", L"");
    if (hRefresh) SetWindowTheme(hRefresh, L"", L"");

    ListView_SetBkColor(m_hResourceList, m_colSettingsBg);
    ListView_SetTextBkColor(m_hResourceList, m_colSettingsBg);
    ListView_SetTextColor(m_hResourceList, m_colSettingsText);
  }

  LayoutResourceViewer();
  PopulateResourceViewer();
}

LRESULT CALLBACK Engine::ResourceViewerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  Engine* p = (Engine*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

  switch (uMsg) {
  case WM_CLOSE:
    ShowWindow(hWnd, SW_HIDE);
    return 0;

  case WM_SIZE:
    if (p) p->LayoutResourceViewer();
    return 0;

  case WM_GETMINMAXINFO: {
    MINMAXINFO* mmi = (MINMAXINFO*)lParam;
    mmi->ptMinTrackSize.x = 500;
    mmi->ptMinTrackSize.y = 250;
    return 0;
  }

  case WM_NOTIFY: {
    NMHDR* pnm = (NMHDR*)lParam;
    // Custom-draw the ListView header (column headers) for dark theme
    if (p && p->m_bSettingsDarkTheme && pnm->code == NM_CUSTOMDRAW) {
      // The header control is a child of the ListView
      HWND hHeader = ListView_GetHeader(p->m_hResourceList);
      if (pnm->hwndFrom == hHeader) {
        NMCUSTOMDRAW* pcd = (NMCUSTOMDRAW*)lParam;
        switch (pcd->dwDrawStage) {
        case CDDS_PREPAINT:
          return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
          HDC hdc = pcd->hdc;
          RECT rc = pcd->rc;
          // Fill header item background
          HBRUSH hBr = CreateSolidBrush(p->m_colSettingsCtrlBg);
          FillRect(hdc, &rc, hBr);
          DeleteObject(hBr);
          // Draw separator line at right edge
          HPEN hPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBorder);
          HPEN hOld = (HPEN)SelectObject(hdc, hPen);
          MoveToEx(hdc, rc.right - 1, rc.top, NULL);
          LineTo(hdc, rc.right - 1, rc.bottom);
          SelectObject(hdc, hOld);
          DeleteObject(hPen);
          // Draw header text
          wchar_t szText[128] = {};
          HDITEMW hdi = {};
          hdi.mask = HDI_TEXT;
          hdi.pszText = szText;
          hdi.cchTextMax = 128;
          Header_GetItem(hHeader, (int)pcd->dwItemSpec, &hdi);
          SetBkMode(hdc, TRANSPARENT);
          SetTextColor(hdc, p->m_colSettingsText);
          HFONT hFont = (HFONT)SendMessage(hHeader, WM_GETFONT, 0, 0);
          HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
          rc.left += 6; // padding
          DrawTextW(hdc, szText, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
          if (hOldFont) SelectObject(hdc, hOldFont);
          return CDRF_SKIPDEFAULT;
        }
        }
      }
    }
    break;
  }

  case WM_ERASEBKGND:
    if (p && p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      HDC hdc = (HDC)wParam;
      RECT rc;
      GetClientRect(hWnd, &rc);
      FillRect(hdc, &rc, p->m_hBrSettingsBg);
      return 1;
    }
    break;

  case WM_CTLCOLORBTN:
    if (p && p->m_bSettingsDarkTheme && p->m_hBrSettingsBg) {
      SetTextColor((HDC)wParam, p->m_colSettingsText);
      SetBkColor((HDC)wParam, p->m_colSettingsBg);
      return (LRESULT)p->m_hBrSettingsBg;
    }
    break;

  case WM_DRAWITEM:
    if (p) {
      DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
      if (pDIS && pDIS->CtlType == ODT_BUTTON) {
        DrawOwnerButton(pDIS, p->m_bSettingsDarkTheme,
          p->m_colSettingsBtnFace, p->m_colSettingsBtnHi, p->m_colSettingsBtnShadow, p->m_colSettingsText);
        return TRUE;
      }
    }
    break;

  case WM_COMMAND: {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);
    if (id == IDC_RV_COPY_PATH && code == BN_CLICKED && p && p->m_hResourceList) {
      int sel = ListView_GetNextItem(p->m_hResourceList, -1, LVNI_SELECTED);
      if (sel >= 0) {
        wchar_t szPath[1024] = {};
        LVITEMW item = {};
        item.iItem = sel;
        item.iSubItem = 3;  // Path column
        item.mask = LVIF_TEXT;
        item.pszText = szPath;
        item.cchTextMax = 1024;
        SendMessageW(p->m_hResourceList, LVM_GETITEMTEXTW, sel, (LPARAM)&item);

        // For procedural resources, copy Name + Type + Details instead of path
        wchar_t szClip[2048] = {};
        if (!wcscmp(szPath, L"(procedural)") || !wcscmp(szPath, L"(render target)")) {
          wchar_t szType[128] = {}, szName[256] = {}, szDetails[128] = {};
          item.iSubItem = 1; item.pszText = szType; item.cchTextMax = 128;
          SendMessageW(p->m_hResourceList, LVM_GETITEMTEXTW, sel, (LPARAM)&item);
          item.iSubItem = 2; item.pszText = szName; item.cchTextMax = 256;
          SendMessageW(p->m_hResourceList, LVM_GETITEMTEXTW, sel, (LPARAM)&item);
          item.iSubItem = 4; item.pszText = szDetails; item.cchTextMax = 128;
          SendMessageW(p->m_hResourceList, LVM_GETITEMTEXTW, sel, (LPARAM)&item);
          swprintf(szClip, 2048, L"%s\t%s\t%s", szName, szType, szDetails);
        } else {
          lstrcpyW(szClip, szPath);
        }

        if (szClip[0] && OpenClipboard(hWnd)) {
          EmptyClipboard();
          size_t len = (wcslen(szClip) + 1) * sizeof(wchar_t);
          HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
          if (hMem) {
            memcpy(GlobalLock(hMem), szClip, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
          }
          CloseClipboard();
        }
      }
      return 0;
    }
    if (id == IDC_RV_REFRESH && code == BN_CLICKED && p) {
      p->PopulateResourceViewer();
      return 0;
    }
    break;
  }
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void Engine::LayoutResourceViewer() {
  if (!m_hResourceWnd || !m_hResourceList) return;
  RECT rc;
  GetClientRect(m_hResourceWnd, &rc);
  int btnH = 28;
  int margin = 6;
  int listBottom = rc.bottom - btnH - margin * 2;

  MoveWindow(m_hResourceList, 0, 0, rc.right, listBottom, TRUE);

  HWND hCopy = GetDlgItem(m_hResourceWnd, IDC_RV_COPY_PATH);
  HWND hRefresh = GetDlgItem(m_hResourceWnd, IDC_RV_REFRESH);
  if (hCopy) MoveWindow(hCopy, rc.right - 36 - margin - 70 - margin, listBottom + margin, 36, btnH, TRUE);
  if (hRefresh) MoveWindow(hRefresh, rc.right - 70 - margin, listBottom + margin, 70, btnH, TRUE);
}

// Sort callback for resource viewer: failed items first, then by type, name, path
static int CALLBACK RV_SortCompare(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
  HWND hList = (HWND)lParamSort;
  wchar_t buf1[512], buf2[512];
  LVITEMW item = {};
  item.mask = LVIF_TEXT;
  item.cchTextMax = 512;

  // Compare status column (col 0): ✗ before ✓
  item.iSubItem = 0;
  item.pszText = buf1; item.iItem = (int)lParam1;
  SendMessageW(hList, LVM_GETITEMTEXTW, lParam1, (LPARAM)&item);
  item.pszText = buf2; item.iItem = (int)lParam2;
  SendMessageW(hList, LVM_GETITEMTEXTW, lParam2, (LPARAM)&item);
  bool fail1 = (buf1[0] == L'\u2717'), fail2 = (buf2[0] == L'\u2717');
  if (fail1 != fail2) return fail1 ? -1 : 1;

  // Compare type (col 1), then name (col 2), then path (col 3)
  for (int col = 1; col <= 3; col++) {
    item.iSubItem = col;
    item.pszText = buf1; item.iItem = (int)lParam1;
    SendMessageW(hList, LVM_GETITEMTEXTW, lParam1, (LPARAM)&item);
    item.pszText = buf2; item.iItem = (int)lParam2;
    SendMessageW(hList, LVM_GETITEMTEXTW, lParam2, (LPARAM)&item);
    int cmp = _wcsicmp(buf1, buf2);
    if (cmp != 0) return cmp;
  }
  return 0;
}

static void RV_AddRow(HWND hList, int idx, const wchar_t* status, const wchar_t* type,
                      const wchar_t* name, const wchar_t* path, const wchar_t* details) {
  LVITEMW item = {};
  item.mask = LVIF_TEXT;
  item.iItem = idx;
  item.iSubItem = 0;
  item.pszText = (LPWSTR)status;
  SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&item);

  item.iSubItem = 1;
  item.pszText = (LPWSTR)type;
  SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&item);

  item.iSubItem = 2;
  item.pszText = (LPWSTR)name;
  SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&item);

  item.iSubItem = 3;
  item.pszText = (LPWSTR)path;
  SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&item);

  item.iSubItem = 4;
  item.pszText = (LPWSTR)details;
  SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&item);
}

void Engine::PopulateResourceViewer() {
  if (!m_hResourceList) return;

  SendMessageW(m_hResourceList, LVM_DELETEALLITEMS, 0, 0);
  int row = 0;
  wchar_t szDetails[128];

  // 1. Render Targets
  for (int i = 0; i < 2; i++) {
    wchar_t szName[32];
    swprintf(szName, 32, L"VS[%d]", i);
    bool valid = m_dx12VS[i].IsValid();
    swprintf(szDetails, 128, L"%dx%d", m_dx12VS[i].width, m_dx12VS[i].height);
    RV_AddRow(m_hResourceList, row++, valid ? L"\u2713" : L"\u2717",
              L"Render Target", szName, L"(render target)", valid ? szDetails : L"");
  }

  // 2. Noise Textures (non-evictable)
  for (int i = 0; i < (int)m_textures.size(); i++) {
    if (m_textures[i].bEvictable) continue;
    bool valid = (m_textures[i].dx12Tex.srvIndex != UINT_MAX) || (m_textures[i].texptr != NULL);
    if (m_textures[i].d > 1)
      swprintf(szDetails, 128, L"%dx%dx%d", m_textures[i].w, m_textures[i].h, m_textures[i].d);
    else
      swprintf(szDetails, 128, L"%dx%d", m_textures[i].w, m_textures[i].h);
    RV_AddRow(m_hResourceList, row++, valid ? L"\u2713" : L"\u2717",
              L"Noise", m_textures[i].texname, L"(procedural)", valid ? szDetails : L"");
  }

  // 3. Blur Targets
  for (int i = 0; i < NUM_BLUR_TEX; i++) {
    wchar_t szName[32];
    swprintf(szName, 32, L"blur[%d]", i);
    bool valid = m_dx12Blur[i].IsValid();
    swprintf(szDetails, 128, L"%dx%d", m_nBlurTexW[i], m_nBlurTexH[i]);
    RV_AddRow(m_hResourceList, row++, valid ? L"\u2713" : L"\u2717",
              L"Blur Target", szName, L"(render target)", valid ? szDetails : L"");
  }

  // 4. Shaders
  {
    bool warpHasCode = m_pState && m_pState->m_nWarpPSVersion > 0 && m_pState->m_szWarpShadersText[0] != 0;
    bool warpOk = m_shaders.warp.bytecodeBlob != NULL;
    wchar_t szVer[32] = L"";
    if (m_pState) swprintf(szVer, 32, L"ps_%d_0", m_pState->m_nWarpPSVersion);
    RV_AddRow(m_hResourceList, row++, warpOk ? L"\u2713" : L"\u2717",
              L"Warp Shader", L"warp", warpHasCode ? L"(custom)" : L"(default)", szVer);

    bool compHasCode = m_pState && m_pState->m_nCompPSVersion > 0 && m_pState->m_szCompShadersText[0] != 0;
    bool compOk = m_shaders.comp.bytecodeBlob != NULL;
    szVer[0] = 0;
    if (m_pState) swprintf(szVer, 32, L"ps_%d_0", m_pState->m_nCompPSVersion);
    RV_AddRow(m_hResourceList, row++, compOk ? L"\u2713" : L"\u2717",
              L"Comp Shader", L"comp", compHasCode ? L"(custom)" : L"(default)", szVer);
  }

  // 5. Custom Textures — reflect sampler names from both warp and comp shader CTs
  {
    std::set<std::wstring> addedNames;  // deduplicate across warp/comp
    LPD3DXCONSTANTTABLE CTs[2] = { m_shaders.warp.CT, m_shaders.comp.CT };
    const wchar_t* shaderLabel[2] = { L"warp", L"comp" };

    for (int s = 0; s < 2; s++) {
      LPD3DXCONSTANTTABLE pCT = CTs[s];
      if (!pCT) continue;

      D3DXCONSTANTTABLE_DESC desc;
      pCT->GetDesc(&desc);

      for (UINT ci = 0; ci < desc.Constants; ci++) {
        D3DXHANDLE h = pCT->GetConstant(NULL, ci);
        D3DXCONSTANT_DESC cd;
        unsigned int count = 1;
        pCT->GetConstantDesc(h, &cd, &count);

        if (cd.RegisterSet != D3DXRS_SAMPLER) continue;

        // Get sampler name and strip "sampler_" prefix
        wchar_t szSamplerName[MAX_PATH];
        lstrcpyW(szSamplerName, AutoWide(cd.Name));

        wchar_t szRootName[MAX_PATH];
        if (!strncmp(cd.Name, "sampler_", 8))
          lstrcpyW(szRootName, AutoWide(&cd.Name[8]));
        else
          lstrcpyW(szRootName, AutoWide(cd.Name));

        // Strip XY_ filter/wrap prefix
        if (lstrlenW(szRootName) > 3 && szRootName[2] == L'_') {
          wchar_t c0 = szRootName[0], c1 = szRootName[1];
          if (c0 >= L'a' && c0 <= L'z') c0 -= L'a' - L'A';
          if (c1 >= L'a' && c1 <= L'z') c1 -= L'a' - L'A';
          bool isPrefix = (c0 == L'F' || c0 == L'P' || c0 == L'W' || c0 == L'C') &&
                          (c1 == L'F' || c1 == L'P' || c1 == L'W' || c1 == L'C');
          if (isPrefix) {
            int j = 0;
            while (szRootName[j + 3]) { szRootName[j] = szRootName[j + 3]; j++; }
            szRootName[j] = 0;
          }
        }

        // Skip built-in resources
        if (!wcscmp(szRootName, L"main")) continue;
        if (!wcscmp(szRootName, L"blur1") || !wcscmp(szRootName, L"blur2") || !wcscmp(szRootName, L"blur3")) continue;
        if (!wcscmp(szRootName, L"blur4") || !wcscmp(szRootName, L"blur5") || !wcscmp(szRootName, L"blur6")) continue;
        if (!wcsncmp(szRootName, L"noise_", 6) || !wcsncmp(szRootName, L"noisevol_", 9)) continue;

        // Deduplicate
        std::wstring key(szRootName);
        if (addedNames.count(key)) continue;
        addedNames.insert(key);

        // Look up in m_textures by name
        bool found = false;
        int texIdx = -1;
        for (int t = 0; t < (int)m_textures.size(); t++) {
          if (!wcscmp(m_textures[t].texname, szRootName)) {
            found = true;
            texIdx = t;
            break;
          }
        }
        // If not found by name (e.g. rand## textures get resolved to a different name),
        // check the actual shader binding to see if a texture was loaded for this slot.
        if (!found && cd.RegisterIndex < 16) {
          CShaderParams& sp = (s == 0) ? m_shaders.warp.params : m_shaders.comp.params;
          UINT srvIdx = sp.m_texture_bindings[cd.RegisterIndex].dx12SrvIndex;
          if (srvIdx != UINT_MAX) {
            for (int t = 0; t < (int)m_textures.size(); t++) {
              if (m_textures[t].dx12Tex.srvIndex == srvIdx) {
                found = true;
                texIdx = t;
                break;
              }
            }
          }
        }

        // For rand textures resolved via SRV index, use the actual loaded texture name
        const wchar_t* szLookupName = (texIdx >= 0) ? m_textures[texIdx].texname : szRootName;

        // Build full file path by searching the same way CacheParams does
        wchar_t szFullPath[MAX_PATH * 2] = {};
        {
          bool pathFound = false;
          for (int z = 0; z < 8; z++) {  // 8 extensions in texture_exts
            wchar_t szTry[MAX_PATH];
            swprintf(szTry, MAX_PATH, L"%stextures\\%s.%s", m_szMilkdrop2Path, szLookupName, texture_exts[z].c_str());
            if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
              lstrcpyW(szFullPath, szTry);
              pathFound = true;
              break;
            }
            swprintf(szTry, MAX_PATH, L"%s%s.%s", m_szPresetDir, szLookupName, texture_exts[z].c_str());
            if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
              lstrcpyW(szFullPath, szTry);
              pathFound = true;
              break;
            }
            // Search random textures directory
            if (m_szRandomTexDir[0]) {
              swprintf(szTry, MAX_PATH, L"%s%s.%s", m_szRandomTexDir, szLookupName, texture_exts[z].c_str());
              if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
                lstrcpyW(szFullPath, szTry);
                pathFound = true;
                break;
              }
            }
            // Search content base path
            if (m_szContentBasePath[0]) {
              swprintf(szTry, MAX_PATH, L"%s%s.%s", m_szContentBasePath, szLookupName, texture_exts[z].c_str());
              if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
                lstrcpyW(szFullPath, szTry);
                pathFound = true;
                break;
              }
            }
            // Search fallback paths
            for (auto& fbPath : m_fallbackPaths) {
              swprintf(szTry, MAX_PATH, L"%s%s.%s", fbPath.c_str(), szLookupName, texture_exts[z].c_str());
              if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
                lstrcpyW(szFullPath, szTry);
                pathFound = true;
                break;
              }
            }
            if (pathFound) break;
          }
          if (!pathFound) {
            // Show expected primary search path for missing textures
            swprintf(szFullPath, MAX_PATH * 2, L"%stextures\\%s", m_szMilkdrop2Path, szLookupName);
          }
        }

        if (found) {
          swprintf(szDetails, 128, L"%dx%d", m_textures[texIdx].w, m_textures[texIdx].h);
          RV_AddRow(m_hResourceList, row++, L"\u2713", L"Custom Tex", szSamplerName, szFullPath, szDetails);
        } else {
          RV_AddRow(m_hResourceList, row++, L"\u2717", L"Custom Tex", szSamplerName, szFullPath, L"(missing)");
        }
      }
    }
  }

  // Sort: failed items first, then by type, name, path
  ListView_SortItemsEx(m_hResourceList, RV_SortCompare, (LPARAM)m_hResourceList);
}

//----------------------------------------------------------------------
// Sprites Tab Functions
//----------------------------------------------------------------------

void Engine::LoadSpritesFromINI() {
  m_spriteEntries.clear();
  m_nSpriteSelected = -1;

  for (int i = 0; i < 100; i++) {
    wchar_t section[64];
    swprintf(section, 64, L"img%02d", i);

    wchar_t img[512] = {};
    GetPrivateProfileStringW(section, L"img", L"", img, 511, m_szImgIniFile);
    if (img[0] == 0) continue;

    SpriteEntry entry = {};
    entry.nIndex = i;
    wcscpy_s(entry.szImg, img);

    // Read colorkey (try colorkey_lo first for backwards compat, then colorkey)
    entry.nColorkey = (unsigned int)GetPrivateProfileIntW(section, L"colorkey_lo", 0, m_szImgIniFile);
    entry.nColorkey = (unsigned int)GetPrivateProfileIntW(section, L"colorkey", entry.nColorkey, m_szImgIniFile);

    // Read init_N and code_N lines (same pattern as LaunchSprite)
    char sectionA[64];
    sprintf(sectionA, "img%02d", i);
    char szTemp[8192];

    for (int pass = 0; pass < 2; pass++) {
      std::string& dest = (pass == 0) ? entry.szInitCode : entry.szFrameCode;
      dest.clear();
      for (int line = 1; ; line++) {
        char key[32];
        sprintf(key, pass == 0 ? "init_%d" : "code_%d", line);
        GetPrivateProfileStringA(sectionA, key, "~!@#$", szTemp, 8192, AutoCharFn(m_szImgIniFile));
        if (strcmp(szTemp, "~!@#$") == 0) break;
        if (!dest.empty()) dest += "\r\n";
        dest += szTemp;
      }
    }
    m_spriteEntries.push_back(entry);
  }
}

void Engine::SaveSpritesToINI() {
  // Delete all [imgNN] sections first
  for (int i = 0; i < 100; i++) {
    wchar_t section[64];
    swprintf(section, 64, L"img%02d", i);
    WritePrivateProfileStringW(section, NULL, NULL, m_szImgIniFile);
  }

  // Write current entries
  for (auto& e : m_spriteEntries) {
    wchar_t section[64];
    swprintf(section, 64, L"img%02d", e.nIndex);

    WritePrivateProfileStringW(section, L"img", e.szImg, m_szImgIniFile);

    if (e.nColorkey != 0) {
      wchar_t ckBuf[32];
      swprintf(ckBuf, 32, L"0x%06X", e.nColorkey);
      WritePrivateProfileStringW(section, L"colorkey", ckBuf, m_szImgIniFile);
    }

    // Write init_N / code_N lines
    char sectionA[64];
    sprintf(sectionA, "img%02d", e.nIndex);

    auto writeLines = [&](const std::string& code, const char* prefix) {
      std::istringstream ss(code);
      std::string line;
      int n = 1;
      while (std::getline(ss, line)) {
        // Strip trailing \r if present (from \r\n in edit control)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        char key[32];
        sprintf(key, "%s_%d", prefix, n++);
        WritePrivateProfileStringA(sectionA, key, line.c_str(), AutoCharFn(m_szImgIniFile));
      }
    };
    writeLines(e.szInitCode, "init");
    writeLines(e.szFrameCode, "code");
  }

  // Flush INI cache to disk
  WritePrivateProfileStringW(NULL, NULL, NULL, m_szImgIniFile);
}

HBITMAP Engine::LoadThumbnailWIC(const wchar_t* szPath, int cx, int cy) {
  // Resolve relative path
  wchar_t fullPath[MAX_PATH] = {};
  if (szPath[0] && szPath[1] != L':') {
    if (m_szContentBasePath[0]) {
      swprintf(fullPath, MAX_PATH, L"%s%s", m_szContentBasePath, szPath);
      if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
        swprintf(fullPath, MAX_PATH, L"%s%s", m_szMilkdrop2Path, szPath);
    } else {
      swprintf(fullPath, MAX_PATH, L"%s%s", m_szMilkdrop2Path, szPath);
    }
  } else {
    wcscpy_s(fullPath, szPath);
  }

  if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
    return NULL;

  IWICImagingFactory* pFactory = NULL;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
  if (FAILED(hr) || !pFactory) return NULL;

  IWICBitmapDecoder* pDecoder = NULL;
  hr = pFactory->CreateDecoderFromFilename(fullPath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
  if (FAILED(hr) || !pDecoder) { pFactory->Release(); return NULL; }

  IWICBitmapFrameDecode* pFrame = NULL;
  pDecoder->GetFrame(0, &pFrame);
  if (!pFrame) { pDecoder->Release(); pFactory->Release(); return NULL; }

  IWICBitmapScaler* pScaler = NULL;
  pFactory->CreateBitmapScaler(&pScaler);
  if (!pScaler) { pFrame->Release(); pDecoder->Release(); pFactory->Release(); return NULL; }
  pScaler->Initialize(pFrame, cx, cy, WICBitmapInterpolationModeLinear);

  IWICFormatConverter* pConverter = NULL;
  pFactory->CreateFormatConverter(&pConverter);
  if (!pConverter) { pScaler->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return NULL; }
  pConverter->Initialize(pScaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = cx;
  bmi.bmiHeader.biHeight = -cy;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  void* pvBits = NULL;
  HBITMAP hBmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
  if (hBmp && pvBits)
    pConverter->CopyPixels(NULL, cx * 4, cx * cy * 4, (BYTE*)pvBits);

  pConverter->Release();
  pScaler->Release();
  pFrame->Release();
  pDecoder->Release();
  pFactory->Release();
  return hBmp;
}

void Engine::PopulateSpriteListView() {
  if (!m_hSpriteList) return;
  ListView_DeleteAllItems(m_hSpriteList);
  if (m_hSpriteImageList) ImageList_RemoveAll((HIMAGELIST)m_hSpriteImageList);

  for (int i = 0; i < (int)m_spriteEntries.size(); i++) {
    auto& e = m_spriteEntries[i];

    // Load thumbnail
    HBITMAP hBmp = LoadThumbnailWIC(e.szImg, 32, 32);
    int imgIdx = -1;
    if (hBmp) {
      imgIdx = ImageList_Add((HIMAGELIST)m_hSpriteImageList, hBmp, NULL);
      DeleteObject(hBmp);
    }

    // Insert item
    wchar_t numBuf[16];
    swprintf(numBuf, 16, L"img%02d", e.nIndex);
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_IMAGE;
    lvi.iItem = i;
    lvi.iImage = imgIdx;
    lvi.pszText = numBuf;
    SendMessageW(m_hSpriteList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

    // Filename column
    const wchar_t* pName = wcsrchr(e.szImg, L'\\');
    if (!pName) pName = wcsrchr(e.szImg, L'/');
    pName = pName ? pName + 1 : e.szImg;
    lvi.mask = LVIF_TEXT;
    lvi.iSubItem = 1;
    lvi.pszText = (LPWSTR)pName;
    SendMessageW(m_hSpriteList, LVM_SETITEMW, 0, (LPARAM)&lvi);

    // Full path column
    lvi.iSubItem = 2;
    lvi.pszText = e.szImg;
    SendMessageW(m_hSpriteList, LVM_SETITEMW, 0, (LPARAM)&lvi);
  }
}

void Engine::UpdateSpriteProperties(int sel) {
  HWND hw = m_hSettingsWnd;
  if (!hw || sel < 0 || sel >= (int)m_spriteEntries.size()) return;

  auto& e = m_spriteEntries[sel];

  SetDlgItemTextW(hw, IDC_MW_SPR_IMG_PATH, e.szImg);

  // Parse init code for default variable values
  // Defaults
  double x = 0.5, y2 = 0.5, sx = 1.0, sy = 1.0, rot = 0.0;
  double r = 1.0, g = 1.0, b = 1.0, a = 1.0;
  int blendmode = 0;
  bool flipx = false, flipy = false, burn = false;
  double repeatx = 1.0, repeaty = 1.0;

  // Simple parser: look for "varname = value" or "varname=value" in init code
  auto parseVar = [&](const std::string& code, const char* name, double& val) {
    size_t pos = 0;
    std::string search = std::string(name) + "=";
    std::string search2 = std::string(name) + " =";
    while (pos < code.size()) {
      size_t found = code.find(search, pos);
      size_t found2 = code.find(search2, pos);
      size_t f = std::string::npos;
      if (found != std::string::npos && (found2 == std::string::npos || found <= found2)) f = found;
      else f = found2;
      if (f == std::string::npos) break;
      // Find the value after =
      size_t eq = code.find('=', f);
      if (eq != std::string::npos) {
        val = atof(code.c_str() + eq + 1);
        return;
      }
      pos = f + 1;
    }
  };

  parseVar(e.szInitCode, "x", x);
  parseVar(e.szInitCode, "y", y2);
  parseVar(e.szInitCode, "sx", sx);
  parseVar(e.szInitCode, "sy", sy);
  parseVar(e.szInitCode, "rot", rot);
  parseVar(e.szInitCode, "r", r);
  parseVar(e.szInitCode, "g", g);
  parseVar(e.szInitCode, "b", b);
  parseVar(e.szInitCode, "a", a);
  parseVar(e.szInitCode, "repeatx", repeatx);
  parseVar(e.szInitCode, "repeaty", repeaty);
  double dBlend = 0; parseVar(e.szInitCode, "blendmode", dBlend); blendmode = (int)dBlend;
  double dFlipx = 0; parseVar(e.szInitCode, "flipx", dFlipx); flipx = dFlipx != 0;
  double dFlipy = 0; parseVar(e.szInitCode, "flipy", dFlipy); flipy = dFlipy != 0;
  double dBurn = 0; parseVar(e.szInitCode, "burn", dBurn); burn = dBurn != 0;

  wchar_t buf[64];
  swprintf(buf, 64, L"%.3g", x); SetDlgItemTextW(hw, IDC_MW_SPR_X, buf);
  swprintf(buf, 64, L"%.3g", y2); SetDlgItemTextW(hw, IDC_MW_SPR_Y, buf);
  swprintf(buf, 64, L"%.3g", sx); SetDlgItemTextW(hw, IDC_MW_SPR_SX, buf);
  swprintf(buf, 64, L"%.3g", sy); SetDlgItemTextW(hw, IDC_MW_SPR_SY, buf);
  swprintf(buf, 64, L"%.3g", rot); SetDlgItemTextW(hw, IDC_MW_SPR_ROT, buf);
  swprintf(buf, 64, L"%.3g", r); SetDlgItemTextW(hw, IDC_MW_SPR_R, buf);
  swprintf(buf, 64, L"%.3g", g); SetDlgItemTextW(hw, IDC_MW_SPR_G, buf);
  swprintf(buf, 64, L"%.3g", b); SetDlgItemTextW(hw, IDC_MW_SPR_B, buf);
  swprintf(buf, 64, L"%.3g", a); SetDlgItemTextW(hw, IDC_MW_SPR_A, buf);
  swprintf(buf, 64, L"%.3g", repeatx); SetDlgItemTextW(hw, IDC_MW_SPR_REPEATX, buf);
  swprintf(buf, 64, L"%.3g", repeaty); SetDlgItemTextW(hw, IDC_MW_SPR_REPEATY, buf);
  swprintf(buf, 64, L"0x%06X", e.nColorkey); SetDlgItemTextW(hw, IDC_MW_SPR_COLORKEY, buf);

  SendDlgItemMessageW(hw, IDC_MW_SPR_BLENDMODE, CB_SETCURSEL, blendmode, 0);
  CheckDlgButton(hw, IDC_MW_SPR_FLIPX, flipx ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hw, IDC_MW_SPR_FLIPY, flipy ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(hw, IDC_MW_SPR_BURN, burn ? BST_CHECKED : BST_UNCHECKED);

  // Set code editors - convert \n to \r\n for edit control
  {
    std::wstring wInit(e.szInitCode.begin(), e.szInitCode.end());
    SetDlgItemTextW(hw, IDC_MW_SPR_INIT_CODE, wInit.c_str());
  }
  {
    std::wstring wFrame(e.szFrameCode.begin(), e.szFrameCode.end());
    SetDlgItemTextW(hw, IDC_MW_SPR_FRAME_CODE, wFrame.c_str());
  }
}

void Engine::SaveCurrentSpriteProperties() {
  HWND hw = m_hSettingsWnd;
  if (!hw || m_nSpriteSelected < 0 || m_nSpriteSelected >= (int)m_spriteEntries.size()) return;

  auto& e = m_spriteEntries[m_nSpriteSelected];

  // Read image path (preserves relative or absolute as displayed)
  GetDlgItemTextW(hw, IDC_MW_SPR_IMG_PATH, e.szImg, 511);

  // Read colorkey
  wchar_t buf[64];
  GetDlgItemTextW(hw, IDC_MW_SPR_COLORKEY, buf, 64);
  e.nColorkey = (unsigned int)wcstoul(buf, NULL, 16);

  // Read init code from editor (preserve original code with custom variables/expressions)
  {
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SPR_INIT_CODE));
    if (len > 0) {
      std::vector<wchar_t> wbuf(len + 1);
      GetDlgItemTextW(hw, IDC_MW_SPR_INIT_CODE, wbuf.data(), len + 1);
      std::string narrow;
      for (wchar_t wc : wbuf) {
        if (wc == 0) break;
        narrow += (char)wc;
      }
      e.szInitCode = narrow;
    } else {
      e.szInitCode.clear();
    }
  }

  // Read per-frame code from editor
  {
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SPR_FRAME_CODE));
    if (len > 0) {
      std::vector<wchar_t> wbuf(len + 1);
      GetDlgItemTextW(hw, IDC_MW_SPR_FRAME_CODE, wbuf.data(), len + 1);
      std::string narrow;
      for (wchar_t wc : wbuf) {
        if (wc == 0) break;
        narrow += (char)wc;
      }
      e.szFrameCode = narrow;
    } else {
      e.szFrameCode.clear();
    }
  }
}

//----------------------------------------------------------------------


} // namespace mdrop
