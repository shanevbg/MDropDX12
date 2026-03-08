// Engine configuration — settings persistence, theme, user defaults, folder picker, screenshots, and misc utilities.
// Separated from engine_settings_ui.cpp and engine_messages.cpp.

#include "engine.h"
#include "engine_helpers.h"
#include "tool_window.h"
#include "utility.h"
#include "pipe_server.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

namespace mdrop {

// ---------------------------------------------------------------------------
// Settings value I/O
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Folder picker
// ---------------------------------------------------------------------------

void Engine::OpenFolderPickerForPresetDir(HWND hOwnerOverride) {
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

    DebugLogW(L"OpenFolderPicker: about to call Show()...", LOG_VERBOSE);
    HWND hOwner = hOwnerOverride ? hOwnerOverride : (m_settingsWindow ? m_settingsWindow->GetHWND() : NULL);
    hr = pfd->Show(hOwner);
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

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

void Engine::CleanupSettingsThemeBrushes() {
  if (m_hBrSettingsBg)     { DeleteObject(m_hBrSettingsBg);     m_hBrSettingsBg = NULL; }
  if (m_hBrSettingsCtrlBg) { DeleteObject(m_hBrSettingsCtrlBg); m_hBrSettingsCtrlBg = NULL; }
}

bool Engine::IsDarkTheme() const {
  if (m_nThemeMode == THEME_DARK)  return true;
  if (m_nThemeMode == THEME_LIGHT) return false;
  // THEME_SYSTEM: read Windows personalization registry
  DWORD value = 1;  // default to light if key missing
  DWORD size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER,
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
    L"AppsUseLightTheme", RRF_RT_DWORD, NULL, &value, &size);
  return value == 0;  // 0 = dark mode
}

void Engine::LoadSettingsThemeFromINI() {
  // Brushes are (re)created from the current color values
  CleanupSettingsThemeBrushes();
  if (IsDarkTheme()) {
    m_hBrSettingsBg     = CreateSolidBrush(m_colSettingsBg);
    m_hBrSettingsCtrlBg = CreateSolidBrush(m_colSettingsCtrlBg);
  }
}

// ---------------------------------------------------------------------------
// User defaults
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Fallback paths
// ---------------------------------------------------------------------------

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
  // Fallback texture file (custom file mode)
  WritePrivateProfileStringW(L"FallbackPaths", L"FallbackTexFile",
    m_szFallbackTexFile[0] ? m_szFallbackTexFile : NULL, pIni);
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
  // Fallback texture file (custom file mode)
  GetPrivateProfileStringW(L"FallbackPaths", L"FallbackTexFile", L"", m_szFallbackTexFile, MAX_PATH, pIni);
}

// ---------------------------------------------------------------------------
// Audio device
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// DX checks
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Remote
// ---------------------------------------------------------------------------

void Engine::OpenMDropDX12Remote() {
  // First try to find an existing Remote window and bring it to front
  HWND hwnd = FindWindowW(NULL, L"MDropDX12 Remote");
  if (!hwnd)
    hwnd = FindWindowW(NULL, L"Milkwave Remote");
  if (hwnd) {
    if (IsIconic(hwnd))
      ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    return;
  }

  // Update stored path from pipe server if a client has connected this session
  extern PipeServer g_pipeServer;
  const wchar_t* clientExe = g_pipeServer.GetLastClientExePath();
  if (clientExe[0] != L'\0' && wcscmp(clientExe, m_szLastRemoteExePath) != 0) {
    wcscpy_s(m_szLastRemoteExePath, clientExe);
    WritePrivateProfileStringW(L"Milkwave", L"LastRemoteExePath", m_szLastRemoteExePath, GetConfigIniFile());
  }

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  // Helper: extract directory from a full exe path
  auto getExeDir = [](const wchar_t* exePath, wchar_t* dirOut, size_t dirSize) {
    wcscpy_s(dirOut, dirSize, exePath);
    wchar_t* pSlash = wcsrchr(dirOut, L'\\');
    if (pSlash) *pSlash = L'\0';
    else dirOut[0] = L'\0';
  };

  // Try the last known Remote exe path first (remembered across sessions)
  if (m_szLastRemoteExePath[0] != L'\0') {
    wchar_t szDir[MAX_PATH] = {};
    getExeDir(m_szLastRemoteExePath, szDir, MAX_PATH);
    if (CreateProcessW(m_szLastRemoteExePath, NULL, NULL, NULL, FALSE, 0, NULL, szDir[0] ? szDir : NULL, &si, &pi)) {
      wchar_t szName[MAX_PATH];
      wchar_t* pName = wcsrchr(m_szLastRemoteExePath, L'\\');
      swprintf_s(szName, L"Starting %s", pName ? pName + 1 : m_szLastRemoteExePath);
      AddNotification(szName);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      return;
    }
  }

  // Fallback: try MDropDX12Remote.exe in exe directory, then PATH
  wchar_t szPath[MAX_PATH] = {};
  swprintf(szPath, MAX_PATH, L"%sMDropDX12Remote.exe", m_szBaseDir);
  if (!CreateProcessW(szPath, NULL, NULL, NULL, FALSE, 0, NULL, m_szBaseDir, &si, &pi)) {
    if (!CreateProcessW(L"MDropDX12Remote.exe", NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
      AddError(L"Could not start Remote app", 3.0f, ERR_MISC, false);
      return;
    }
  }
  AddNotification(L"Starting MDropDX12 Remote");
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

int Engine::GetPresetCount() { return m_nPresets; }
int Engine::GetCurrentPresetIndex() { return m_nCurrentPreset; }

} // namespace mdrop
