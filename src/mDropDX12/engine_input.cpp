/*
  Plugin module: Window Proc & Keyboard Input
  Extracted from engine.cpp for maintainability.
  Contains: MyWindowProc, HandleRegularKey, WaitString editing,
            window management (always-on-top, opacity, transparency)
*/

#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "shell_defines.h"
#include "wasabi.h"
#include <assert.h>
#include <strsafe.h>
#include <shellapi.h>
#include <Windows.h>
#include <cstdint>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include "App.h"

#define FRAND ((rand() % 7381)/7380.0f)
#define clamp(value, min, max) ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))

namespace mdrop {

extern Engine g_engine;
extern int ToggleFPSNumPressed;
extern int HardcutMode;
extern float timetick;
extern float timetick2;
extern float TimeToAutoLockPreset;
extern int beatcount;
extern bool TranspaMode;
extern bool AutoLockedPreset;
extern int g_nHelpLineCount;

void Engine::SetFPSCap(int fps) {
  m_max_fps_fs = fps;
  m_max_fps_dm = fps;
  m_max_fps_w = fps;
  wchar_t* ini = GetConfigIniFile();
  WritePrivateProfileIntW(fps, L"max_fps_fs", ini, L"Settings");
  WritePrivateProfileIntW(fps, L"max_fps_dm", ini, L"Settings");
  WritePrivateProfileIntW(fps, L"max_fps_w", ini, L"Settings");
  HWND hSettingsWnd = m_settingsWindow ? m_settingsWindow->GetHWND() : NULL;
  if (hSettingsWnd && IsWindow(hSettingsWnd)) {
    HWND hCombo = GetDlgItem(hSettingsWnd, IDC_MW_FPS_CAP);
    if (hCombo) {
      const int vals[] = { 30, 60, 90, 120, 144, 240, 360, 720, 0 };
      for (int i = 0; i < 9; i++)
        if (vals[i] == fps) { SendMessage(hCombo, CB_SETCURSEL, i, 0); break; }
    }
  }
}

void copyStringToClipboardA(const char* source) {
  int ok = OpenClipboard(NULL);
  if (!ok)
    return;

  HGLOBAL clipbuffer;
  EmptyClipboard();
  clipbuffer = GlobalAlloc(GMEM_DDESHARE, (lstrlenA(source) + 1) * sizeof(char));
  char* buffer = (char*)GlobalLock(clipbuffer);
  lstrcpyA(buffer, source);
  GlobalUnlock(clipbuffer);
  SetClipboardData(CF_TEXT, clipbuffer);
  CloseClipboard();
}

void copyStringToClipboardW(const wchar_t* source) {
  int ok = OpenClipboard(NULL);
  if (!ok)
    return;

  HGLOBAL clipbuffer;
  EmptyClipboard();
  clipbuffer = GlobalAlloc(GMEM_DDESHARE, (lstrlenW(source) + 1) * sizeof(wchar_t));
  wchar_t* buffer = (wchar_t*)GlobalLock(clipbuffer);
  lstrcpyW(buffer, source);
  GlobalUnlock(clipbuffer);
  SetClipboardData(CF_UNICODETEXT, clipbuffer);
  CloseClipboard();
}

/*
 * Suppose there is a string on the clipboard.
 * This function copies it FROM there.
 */
char* getStringFromClipboardA() {
  int ok = OpenClipboard(NULL);
  if (!ok)
    return NULL;

  HANDLE hData = GetClipboardData(CF_TEXT);
  char* buffer = (char*)GlobalLock(hData);
  GlobalUnlock(hData);
  CloseClipboard();
  return buffer;
}

wchar_t* getStringFromClipboardW() {
  int ok = OpenClipboard(NULL);
  if (!ok)
    return NULL;

  HANDLE hData = GetClipboardData(CF_UNICODETEXT);
  wchar_t* buffer = (wchar_t*)GlobalLock(hData);
  GlobalUnlock(hData);
  CloseClipboard();
  return buffer;
}

void ConvertCRsToLFCA(const char* src, char* dst) {
  while (*src) {
    char ch = *src;
    if (*src == 13 && *(src + 1) == 10) {
      *dst++ = LINEFEED_CONTROL_CHAR;
      src += 2;
    }
    else {
      *dst++ = *src++;
    }
  }
  *dst = 0;
}

void ConvertCRsToLFCW(const wchar_t* src, wchar_t* dst) {
  while (*src) {
    wchar_t ch = *src;
    if (*src == 13 && *(src + 1) == 10) {
      *dst++ = LINEFEED_CONTROL_CHAR;
      src += 2;
    }
    else {
      *dst++ = *src++;
    }
  }
  *dst = 0;
}

void ConvertLFCToCRsA(const char* src, char* dst) {
  while (*src) {
    char ch = *src;
    if (*src == LINEFEED_CONTROL_CHAR) {
      *dst++ = 13;
      *dst++ = 10;
      src++;
    }
    else {
      *dst++ = *src++;
    }
  }
  *dst = 0;
}

void ConvertLFCToCRsW(const wchar_t* src, wchar_t* dst) {
  while (*src) {
    wchar_t ch = *src;
    if (*src == LINEFEED_CONTROL_CHAR) {
      *dst++ = 13;
      *dst++ = 10;
      src++;
    }
    else {
      *dst++ = *src++;
    }
  }
  *dst = 0;
}

int mystrcmpiW(const wchar_t* s1, const wchar_t* s2) {
  // returns  1 if s1 comes before s2
  // returns  0 if equal
  // returns -1 if s1 comes after s2
  // treats all characters/symbols by their ASCII values,
  //    except that it DOES ignore case.

  int i = 0;

  while (LC2UC[s1[i]] == LC2UC[s2[i]] && s1[i] != 0)
    i++;

  //FIX THIS!

  if (s1[i] == 0 && s2[i] == 0)
    return 0;
  else if (s1[i] == 0)
    return -1;
  else if (s2[i] == 0)
    return 1;
  else
    return (LC2UC[s1[i]] < LC2UC[s2[i]]) ? -1 : 1;
}

void Engine::ToggleAlwaysOnTop(HWND hwnd) {

  RECT rect;
  GetWindowRect(hwnd, &rect);
  int x = rect.left;
  int y = rect.top;
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;

  if (m_bAlwaysOnTop) {
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_DRAWFRAME | SWP_FRAMECHANGED);
  }
  else {
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, width, height, SWP_DRAWFRAME | SWP_FRAMECHANGED);
  }
}

void ToggleTransparency(HWND hwnd) {
  RECT rect;
  GetWindowRect(hwnd, &rect);
  int x = rect.left;
  int y = rect.top;
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;

  //Checks if DWM (Aero) is enabled or disabled
  BOOL dwmEnabled = FALSE;
  DwmIsCompositionEnabled(&dwmEnabled);

  LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

  // Enable the layered window attribute without affecting other styles
  exStyle |= WS_EX_LAYERED;
  SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

  SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_DRAWFRAME | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED); // Redraws the window to fix the transparency mode issue for Windows 7, 8 and 8.1.
  if (TranspaMode) {
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_COLORKEY);
    g_engine.fOpacity = 1.0f;
    DragAcceptFiles(hwnd, TRUE);
  }
  else {
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    DragAcceptFiles(hwnd, TRUE);
  }
}

void Engine::SetOpacity(HWND hwnd) {
  if (IsBorderlessFullscreen(hwnd)) {
    g_engine.m_WindowWatermarkModeOpacity = fOpacity;
  }

  // Retrieve the current extended window style
  LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

  // Check if the window is currently in clickthrough mode
  bool isClickthrough = (exStyle & WS_EX_TRANSPARENT) != 0;

  // Ensure the window is layered (required for transparency)
  if (!(exStyle & WS_EX_LAYERED)) {
    exStyle |= WS_EX_LAYERED;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
  }

  // Set the new opacity
  BYTE alpha = static_cast<BYTE>(fOpacity * 255); // Convert opacity (0.0 to 1.0) to alpha (0 to 255)
  if (!SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA)) {
    DWORD error = GetLastError();
    printf("Failed to set window opacity. Error: %lu\n", error);
  }

  // Modify the clickthrough state
  if (isClickthrough) {
    exStyle |= WS_EX_TRANSPARENT;
  }
  else {
    exStyle &= ~WS_EX_TRANSPARENT;
  }

  // Reapply the extended window styles
  SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

  // Reapply the alpha value after modifying the extended styles
  if (!SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA)) {
    DWORD error = GetLastError();
    printf("Failed to reapply window opacity. Error: %lu\n", error);
  }

  int display = (int)std::ceil(100 * fOpacity);
  wchar_t buf[1024];
  swprintf(buf, 64, L"Opacity: %d%%", display); // Use %d for integers
  g_engine.AddNotification(buf);

  SendMessageToMDropDX12Remote((L"OPACITY=" + std::to_wstring(display)).c_str());
}

void ToggleWindowOpacity(HWND hwnd, bool bDown) {
  RECT rect;
  GetWindowRect(hwnd, &rect);
  int x = rect.left;
  int y = rect.top;
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;

  float changeVal = 0.1f;
  if (g_engine.fOpacity < 0.09 || (g_engine.fOpacity <= 0.1 && bDown)) {
    changeVal = 0.01f;
  }
  else {
    changeVal = 0.05f;
  }
  if (bDown) {
    g_engine.fOpacity -= changeVal;
  }
  else {
    g_engine.fOpacity += changeVal;
  }

  if (g_engine.fOpacity < 0.01f)
    g_engine.fOpacity = 0.01f;
  else if (g_engine.fOpacity > 1.0f)
    g_engine.fOpacity = 1.0f;

  // Set the opacity of the window
  g_engine.SetOpacity(hwnd);
}

bool Engine::IsBorderlessFullscreen(HWND hWnd) {
  // Check if the window is borderless fullscreen:
  // must fill the work area AND have no standard window chrome (WS_OVERLAPPEDWINDOW).
  // Without the style check, a normal maximized window also fills the work area
  // and would incorrectly trigger watermark mode on startup.
  LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
  if (style & (WS_CAPTION | WS_THICKFRAME))
    return false;  // has window chrome — it's just maximized, not borderless

  RECT workArea = {};
  MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
  HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
  if (GetMonitorInfo(hMonitor, &monitorInfo)) {
    workArea = monitorInfo.rcWork;
  }
  RECT currentRect;
  GetWindowRect(hWnd, &currentRect);
  return (currentRect.left == workArea.left &&
    currentRect.top == workArea.top &&
    currentRect.right == workArea.right &&
    currentRect.bottom == workArea.bottom);
}

static bool HasPresetExtension(const wchar_t* path) {
  size_t len = wcslen(path);
  if (len >= 5 && _wcsicmp(path + len - 5, L".milk") == 0) return true;
  if (len >= 6 && _wcsicmp(path + len - 6, L".milk2") == 0) return true;
  if (len >= 6 && _wcsicmp(path + len - 6, L".milk3") == 0) return true;
  return false;
}

static void ScanFolderForPresets(const wchar_t* folderPath, std::vector<std::wstring>& out) {
  const wchar_t* patterns[] = { L"\\*.milk", L"\\*.milk2", L"\\*.milk3" };
  for (const wchar_t* pat : patterns) {
    wchar_t searchPath[MAX_PATH];
    lstrcpyW(searchPath, folderPath);
    lstrcatW(searchPath, pat);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
          std::wstring fullPath(folderPath);
          if (fullPath.back() != L'\\') fullPath += L'\\';
          fullPath += fd.cFileName;
          out.push_back(fullPath);
        }
      } while (FindNextFileW(hFind, &fd));
      FindClose(hFind);
    }
  }
}

void LoadPresetFilesViaDragAndDrop(WPARAM wParam) {
  HDROP hDrop = (HDROP)wParam;
  int count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
  if (count == 0) { DragFinish(hDrop); return; }

  // Collect dropped items
  std::vector<std::wstring> milkFiles;
  std::vector<std::wstring> folders;

  for (int i = 0; i < count; i++) {
    wchar_t path[MAX_PATH];
    DragQueryFileW(hDrop, i, path, MAX_PATH);
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) continue;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
      folders.push_back(path);
    else if (HasPresetExtension(path))
      milkFiles.push_back(path);
  }
  DragFinish(hDrop);

  int totalItems = (int)milkFiles.size() + (int)folders.size();
  if (totalItems == 0) {
    g_engine.AddError((wchar_t*)L"No preset files or folders found in drop", g_engine.m_ErrorDuration, ERR_NOTIFY, true);
    return;
  }

  // Case 1: Single preset file — load from original location
  if (totalItems == 1 && milkFiles.size() == 1) {
    g_engine.LoadPreset(milkFiles[0].c_str(), 0.0f);
    return;
  }

  // Case 2: Single folder — use as preset directory
  if (totalItems == 1 && folders.size() == 1) {
    std::wstring dir = folders[0];
    if (dir.back() != L'\\') dir += L'\\';
    lstrcpyW(g_engine.m_szPresetDir, dir.c_str());
    WritePrivateProfileStringW(L"Settings", L"szPresetDir", g_engine.m_szPresetDir, g_engine.GetConfigIniFile());
    g_engine.UpdatePresetList(false, true);
    wchar_t buf[512];
    swprintf(buf, 512, L"Preset directory: %s", g_engine.m_szPresetDir);
    g_engine.AddNotification(buf);
    return;
  }

  // Case 3: Multiple items — create Preset-DND directory, copy presets
  // Scan dropped folders for preset files
  for (const auto& folder : folders)
    ScanFolderForPresets(folder.c_str(), milkFiles);

  if (milkFiles.empty()) {
    g_engine.AddError((wchar_t*)L"No preset files found in dropped items", g_engine.m_ErrorDuration, ERR_NOTIFY, true);
    return;
  }

  // Create timestamped DND directory
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t timestamp[16];
  swprintf(timestamp, 16, L"%02d%02d%02d%02d%02d", st.wYear % 100, st.wMonth, st.wDay, st.wHour, st.wMinute);

  wchar_t parentDir[MAX_PATH];
  swprintf(parentDir, MAX_PATH, L"%sPreset-DND", g_engine.m_szBaseDir);
  CreateDirectoryW(parentDir, NULL);

  wchar_t dndDir[MAX_PATH];
  swprintf(dndDir, MAX_PATH, L"%sPreset-DND\\%s\\", g_engine.m_szBaseDir, timestamp);
  wchar_t dndDirNoSlash[MAX_PATH];
  swprintf(dndDirNoSlash, MAX_PATH, L"%sPreset-DND\\%s", g_engine.m_szBaseDir, timestamp);
  CreateDirectoryW(dndDirNoSlash, NULL);

  // Copy preset files into DND directory
  int copied = 0;
  std::wstring firstDest;
  for (const auto& src : milkFiles) {
    size_t pos = src.find_last_of(L'\\');
    std::wstring filename = (pos != std::wstring::npos) ? src.substr(pos + 1) : src;
    std::wstring dest = std::wstring(dndDir) + filename;
    if (CopyFileW(src.c_str(), dest.c_str(), FALSE)) {
      if (firstDest.empty()) firstDest = dest;
      copied++;
    }
  }

  if (copied == 0) {
    g_engine.AddError((wchar_t*)L"Failed to copy preset files", g_engine.m_ErrorDuration, ERR_NOTIFY, true);
    return;
  }

  // Set as preset directory
  lstrcpyW(g_engine.m_szPresetDir, dndDir);
  WritePrivateProfileStringW(L"Settings", L"szPresetDir", g_engine.m_szPresetDir, g_engine.GetConfigIniFile());
  g_engine.UpdatePresetList(false, true);

  // Load first preset
  if (!firstDest.empty())
    g_engine.LoadPreset(firstDest.c_str(), 0.0f);

  wchar_t buf[512];
  swprintf(buf, 512, L"Preset-DND/%s \u2014 %d presets copied", timestamp, copied);
  g_engine.AddNotification(buf);
}
//----------------------------------------------------------------------

LRESULT Engine::MyWindowProc(HWND hWnd, unsigned uMsg, WPARAM wParam, LPARAM lParam) {
  // You can handle Windows messages here while the plugin is running,
  //   such as mouse events (WM_MOUSEMOVE/WM_LBUTTONDOWN), keypresses
  //   (WK_KEYDOWN/WM_CHAR), and so on.
  // This function is threadsafe (thanks to Winamp's architecture),
  //   so you don't have to worry about using semaphores or critical
  //   sections to read/write your class member variables.
  // If you don't handle a message, let it continue on the usual path
  //   (to Winamp) by returning DefWindowProc(hWnd,uMsg,wParam,lParam).
  // If you do handle a message, prevent it from being handled again
  //   (by Winamp) by returning 0.

  // IMPORTANT: For the WM_KEYDOWN, WM_KEYUP, and WM_CHAR messages,
  //   you must return 0 if you process the message (key),
  //   and 1 if you do not.  DO NOT call DefWindowProc()
  //   for these particular messages!

  USHORT mask = 1 << (sizeof(SHORT) * 8 - 1);
  bool bShiftHeldDown = (GetKeyState(VK_SHIFT) & mask) != 0;
  bool bCtrlHeldDown = (GetKeyState(VK_CONTROL) & mask) != 0;

  int nRepeat = 1;  //updated as appropriate

  switch (uMsg) {
  // Settings window thread-safe side effects
  case WM_MW_SET_OPACITY:
    SetOpacity(GetPluginWindow());
    return 0;
  case WM_MW_SET_ALWAYS_ON_TOP:
    ToggleAlwaysOnTop(GetPluginWindow());
    return 0;
  case WM_MW_TOGGLE_SPOUT:
    EnqueueRenderCmd(RenderCmd::ToggleSpout);
    return 0;
  case WM_MW_RESET_BUFFERS:
    EnqueueRenderCmd(RenderCmd::ResetBuffers);
    return 0;
  case WM_MW_RESTART_DEVICE:
    EnqueueRenderCmd(RenderCmd::DeviceRecovery);
    return 0;
  case WM_MW_SPOUT_FIXEDSIZE:
    EnqueueRenderCmd(RenderCmd::SpoutFixedSize);
    return 0;
  case WM_MW_PUSH_MESSAGE:
    LaunchCustomMessage((int)wParam);
    return 0;

  case WM_MW_REFRESH_DISPLAYS:
    EnqueueRenderCmd(RenderCmd::RefreshDisplays);
    return 0;

  case WM_MW_REGISTER_HOTKEYS:
    UnregisterGlobalHotkeys(GetPluginWindow());
    RegisterGlobalHotkeys(GetPluginWindow());
    return 0;

  case WM_DISPLAYCHANGE:
    // Monitor connect/disconnect — re-enumerate
    PostMessage(GetPluginWindow(), WM_MW_REFRESH_DISPLAYS, 0, 0);
    return 0;

  case WM_MW_PUSH_SPRITE:
  {
    RenderCommand cmd;
    cmd.cmd = RenderCmd::PushSprite;
    cmd.iParam1 = (int)wParam;
    cmd.iParam2 = (int)lParam;
    EnqueueRenderCmd(std::move(cmd));
    return 0;
  }
  case WM_MW_KILL_SPRITE:
  {
    RenderCommand cmd;
    cmd.cmd = RenderCmd::KillSprite;
    cmd.iParam1 = (int)wParam;
    EnqueueRenderCmd(std::move(cmd));
    return 0;
  }

  // WM_MW_RESTART_IPC removed — pipe server uses PID-based naming, no restart needed

  case WM_MW_NO_PRESETS_PROMPT:
    OpenWelcomeWindow();
    return 0;

  // Milkwave Remote messages (forwarded from IPC window)
  case WM_MW_NEXT_PRESET:
  {
    RenderCommand cmd;
    cmd.cmd = RenderCmd::NextPreset;
    cmd.fParam = m_fBlendTimeUser;
    EnqueueRenderCmd(std::move(cmd));
    return 0;
  }
  case WM_MW_PREV_PRESET:
  {
    RenderCommand cmd;
    cmd.cmd = RenderCmd::PrevPreset;
    cmd.fParam = 0.0f;
    EnqueueRenderCmd(std::move(cmd));
    return 0;
  }
  case WM_MW_CAPTURE:
    EnqueueRenderCmd(RenderCmd::CaptureScreenshot);
    return 0;
  case WM_MW_ENABLESPOUTMIX:
  {
    bool bEnable = (wParam != 0);
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
    return 0;
  }
  case WM_MW_SET_INPUTMIX_ONTOP:
    m_bSpoutInputOnTop = (wParam != 0);
    SaveSpoutInputSettings();
    return 0;
  case WM_MW_SET_INPUTMIX_OPACITY:
  {
    float op = (float)(INT_PTR)wParam / 100.0f;
    if (op < 0.0f) op = 0.0f;
    if (op > 1.0f) op = 1.0f;
    m_fSpoutInputOpacity = op;
    SaveSpoutInputSettings();
    return 0;
  }
  case WM_MW_SET_INPUTMIX_LUMAKEY:
  {
    int threshold = (int)(INT_PTR)wParam;
    int softness = (int)(INT_PTR)lParam;
    if (threshold < 0) {
      m_bSpoutInputLumaKey = false;
    } else {
      m_bSpoutInputLumaKey = true;
      m_fSpoutInputLumaThreshold = (float)threshold / 100.0f;
      if (m_fSpoutInputLumaThreshold > 1.0f) m_fSpoutInputLumaThreshold = 1.0f;
      m_fSpoutInputLumaSoftness = (float)softness / 100.0f;
      if (m_fSpoutInputLumaSoftness > 1.0f) m_fSpoutInputLumaSoftness = 1.0f;
    }
    SaveSpoutInputSettings();
    return 0;
  }
  case WM_MW_COVER_CHANGED:
  case WM_MW_SPRITE_MODE:
  case WM_MW_MESSAGE_MODE:
  case WM_MW_SETVIDEODEVICE:
  case WM_MW_ENABLEVIDEOMIX:
  case WM_MW_SETSPOUTSENDER:
    // Not yet implemented — absorb to prevent DefWindowProc handling
    return 0;

  case WM_MW_SHOW_COVER:
  {
    RenderCommand cmd;
    cmd.cmd = RenderCmd::PushSprite;
    cmd.iParam1 = 0;  // sprite 0 = cover art
    cmd.iParam2 = -1;  // auto-pick slot
    EnqueueRenderCmd(std::move(cmd));
    return 0;
  }

  case WM_SIZE:
    // If render window went fullscreen, move settings window to another monitor
    if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED) {
      if (m_settingsWindow && m_settingsWindow->IsOpen())
        m_settingsWindow->EnsureVisible();
    }
    break; // let base class handle resize too

  case WM_MW_IPC_MESSAGE:
  {
    // Forwarded from IPC window thread — lParam is heap-allocated wchar_t*, wParam is dwData
    wchar_t* message = (wchar_t*)lParam;
    DWORD_PTR dwData = (DWORD_PTR)wParam;
    if (message) {
      // Capture for IPC monitor (lightweight, stays on message pump thread)
      SYSTEMTIME st; GetLocalTime(&st);
      swprintf_s(g_szLastIPCTime, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
      wcsncpy_s(g_szLastIPCMessage, message, _TRUNCATE);
      g_lastIPCMessageSeq.fetch_add(1);

      if (dwData == 1) {
        RenderCommand cmd;
        cmd.cmd = RenderCmd::IPCMessage;
        cmd.sParam = message;
        EnqueueRenderCmd(std::move(cmd));
      }
      else if (dwData == WM_MW_SETSPOUTSENDER) {
        // MR sends sender name via WM_COPYDATA with dwData=WM_MW_SETSPOUTSENDER
        // Store the name; MR sends WM_ENABLESPOUTMIX next to actually enable
        wcsncpy_s(m_szSpoutInputSender, message, _TRUNCATE);
      }
      free(message);
    }
    return 0;
  }

  case WM_COMMAND:

  case WM_CHAR:   // plain & simple alphanumeric keys
    nRepeat = LOWORD(lParam);
    if (m_waitstring.bActive)	// if user is in the middle of editing a string
    {
      if ((wParam >= ' ' && wParam <= 'z') || wParam == '{' || wParam == '}') {
        int len;
        if (m_waitstring.bDisplayAsCode)
          len = lstrlenA((char*)m_waitstring.szText);
        else
          len = lstrlenW(m_waitstring.szText);

        if (m_waitstring.bFilterBadChars &&
          (wParam == '\"' ||
            wParam == '\\' ||
            wParam == '/' ||
            wParam == ':' ||
            wParam == '*' ||
            wParam == '?' ||
            wParam == '|' ||
            wParam == '<' ||
            wParam == '>' ||
            wParam == '&'))	// NOTE: '&' is legal in filenames, but we try to avoid it since during GDI display it acts as a control code (it will not show up, but instead, underline the character following it).
        {
          // illegal char
          AddError(wasabiApiLangString(IDS_ILLEGAL_CHARACTER), m_ErrorDuration, ERR_MISC, true);
        }
        else if (len + nRepeat >= m_waitstring.nMaxLen) {
          // m_waitstring.szText has reached its limit
          AddError(wasabiApiLangString(IDS_STRING_TOO_LONG), m_ErrorDuration, ERR_MISC, true);
        }
        else {
          //m_fShowUserMessageUntilThisTime = GetTime();	// if there was an error message already, clear it

          if (m_waitstring.bDisplayAsCode) {
            char buf[16];
            sprintf(buf, "%c", (int)wParam);

            if (m_waitstring.nSelAnchorPos != -1)
              WaitString_NukeSelection();

            if (m_waitstring.bOvertypeMode) {
              // overtype mode
              for (int rep = 0; rep < nRepeat; rep++) {
                if (m_waitstring.nCursorPos == len) {
                  lstrcatA((char*)m_waitstring.szText, buf);
                  len++;
                }
                else {
                  char* ptr = (char*)m_waitstring.szText;
                  *(ptr + m_waitstring.nCursorPos) = buf[0];
                }
                m_waitstring.nCursorPos++;
              }
            }
            else {
              // insert mode:
              char* ptr = (char*)m_waitstring.szText;
              for (int rep = 0; rep < nRepeat; rep++) {
                for (int i = len; i >= m_waitstring.nCursorPos; i--)
                  *(ptr + i + 1) = *(ptr + i);
                *(ptr + m_waitstring.nCursorPos) = buf[0];
                m_waitstring.nCursorPos++;
                len++;
              }
            }
          }
          else {
            wchar_t buf[16];
            swprintf(buf, L"%c", (int)wParam);

            if (m_waitstring.nSelAnchorPos != -1)
              WaitString_NukeSelection();

            if (m_waitstring.bOvertypeMode) {
              // overtype mode
              for (int rep = 0; rep < nRepeat; rep++) {
                if (m_waitstring.nCursorPos == len) {
                  lstrcatW(m_waitstring.szText, buf);
                  len++;
                }
                else
                  m_waitstring.szText[m_waitstring.nCursorPos] = buf[0];
                m_waitstring.nCursorPos++;
              }
            }
            else {
              // insert mode:
              for (int rep = 0; rep < nRepeat; rep++) {
                for (int i = len; i >= m_waitstring.nCursorPos; i--)
                  m_waitstring.szText[i + 1] = m_waitstring.szText[i];
                m_waitstring.szText[m_waitstring.nCursorPos] = buf[0];
                m_waitstring.nCursorPos++;
                len++;
              }
            }
          }
        }
      }
      return 0; // we processed (or absorbed) the key
    }
    else if (m_UI_mode == UI_LOAD_DEL)	// waiting to confirm file delete
    {
      if (wParam == 'y' || wParam == 'Y')	// 'y' or 'Y'
      {
        // first add pathname to filename
        wchar_t szDelFile[512];
        swprintf(szDelFile, L"%s%s", GetPresetDir(), m_presets[m_nPresetListCurPos].szFilename.c_str());

        DeletePresetFile(szDelFile);
        //m_nCurrentPreset = -1;
      }

      m_UI_mode = UI_LOAD;

      return 0; // we processed (or absorbed) the key
    }
    else if (m_UI_mode == UI_UPGRADE_PIXEL_SHADER) {
      if (wParam == 'y' || wParam == 'Y')	// 'y' or 'Y'
      {
        if (m_pState->m_nMinPSVersion == m_pState->m_nMaxPSVersion) {
          switch (m_pState->m_nMinPSVersion) {
          case MD2_PS_NONE:
            m_pState->m_nWarpPSVersion = MD2_PS_2_0;
            m_pState->m_nCompPSVersion = MD2_PS_2_0;
            m_pState->GenDefaultWarpShader();
            m_pState->GenDefaultCompShader();
            break;
          case MD2_PS_2_0:
            m_pState->m_nWarpPSVersion = MD2_PS_2_X;
            m_pState->m_nCompPSVersion = MD2_PS_2_X;
            break;
          case MD2_PS_2_X:
            m_pState->m_nWarpPSVersion = MD2_PS_3_0;
            m_pState->m_nCompPSVersion = MD2_PS_3_0;
            break;
          default:
            assert(0);
            break;
          }
        }
        else {
          switch (m_pState->m_nMinPSVersion) {
          case MD2_PS_NONE:
            if (m_pState->m_nWarpPSVersion < MD2_PS_2_0) {
              m_pState->m_nWarpPSVersion = MD2_PS_2_0;
              m_pState->GenDefaultWarpShader();
            }
            if (m_pState->m_nCompPSVersion < MD2_PS_2_0) {
              m_pState->m_nCompPSVersion = MD2_PS_2_0;
              m_pState->GenDefaultCompShader();
            }
            break;
          case MD2_PS_2_0:
            m_pState->m_nWarpPSVersion = max(m_pState->m_nWarpPSVersion, MD2_PS_2_X);
            m_pState->m_nCompPSVersion = max(m_pState->m_nCompPSVersion, MD2_PS_2_X);
            break;
          case MD2_PS_2_X:
            m_pState->m_nWarpPSVersion = max(m_pState->m_nWarpPSVersion, MD2_PS_3_0);
            m_pState->m_nCompPSVersion = max(m_pState->m_nCompPSVersion, MD2_PS_3_0);
            break;
          default:
            assert(0);
            break;
          }
        }
        m_pState->m_nMinPSVersion = min(m_pState->m_nWarpPSVersion, m_pState->m_nCompPSVersion);
        m_pState->m_nMaxPSVersion = max(m_pState->m_nWarpPSVersion, m_pState->m_nCompPSVersion);

        LoadShaders(&m_shaders, m_pState, false, false);
        CreateDX12PresetPSOs();
        SetMenusForPresetVersion(m_pState->m_nWarpPSVersion, m_pState->m_nCompPSVersion);
      }
      if (wParam != 13)
        m_UI_mode = UI_MENU;
      return 0; // we processed (or absorbed) the key
    }
    else if (m_UI_mode == UI_SAVE_OVERWRITE)	// waiting to confirm overwrite file on save
    {
      if (wParam == 'y' || wParam == 'Y')	// 'y' or 'Y'
      {
        // first add pathname + extension to filename
        wchar_t szNewFile[512];
        swprintf(szNewFile, L"%s%s.milk", GetPresetDir(), m_waitstring.szText);

        SavePresetAs(szNewFile);

        // exit waitstring mode
        m_UI_mode = UI_REGULAR;
        m_waitstring.bActive = false;
        //m_bPresetLockedByCode = false;
      }
      else if ((wParam >= ' ' && wParam <= 'z') || wParam == 27)		// 27 is the ESCAPE key
      {
        // go back to SAVE AS mode
        m_UI_mode = UI_SAVEAS;
        m_waitstring.bActive = true;
      }

      return 0; // we processed (or absorbed) the key
    }
    else	// normal handling of a simple key (all non-virtual-key hotkeys end up here)
    {
      if (HandleRegularKey(wParam) == 0)
        return 0;
    }
    return 1; // end case WM_CHAR



    // Handle other messages here...


  case WM_MOUSEWHEEL:

    if (GET_WHEEL_DELTA_WPARAM(wParam) < 0 && !m_bPresetLockedByCode)
      if (bShiftHeldDown)
        ToggleWindowOpacity(hWnd, true);
      else
        NextPreset(0);

    else if (GET_WHEEL_DELTA_WPARAM(wParam) > 0 && !m_bPresetLockedByCode)
      if (bShiftHeldDown)
        ToggleWindowOpacity(hWnd, false);
      else
        PrevPreset(0);

    return 0;

  case WM_CREATE:
    DragAcceptFiles(hWnd, TRUE);
    return 0;

  case WM_DROPFILES:
    LoadPresetFilesViaDragAndDrop(wParam);
    // Refresh settings window if open
    {
      HWND hSW = m_settingsWindow ? m_settingsWindow->GetHWND() : NULL;
      if (hSW) {
        SetWindowTextW(GetDlgItem(hSW, IDC_MW_PRESET_DIR), m_szPresetDir);
        HWND hList = GetDlgItem(hSW, IDC_MW_PRESET_LIST);
        if (hList) {
          SendMessage(hList, LB_RESETCONTENT, 0, 0);
          for (int i = 0; i < m_nPresets; i++) {
            if (m_presets[i].szFilename.empty()) continue;
            SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)m_presets[i].szFilename.c_str());
          }
          if (m_nCurrentPreset >= 0 && m_nCurrentPreset < m_nPresets)
            SendMessage(hList, LB_SETCURSEL, m_nCurrentPreset, 0);
        }
      }
    }
    return 0;


    //case WM_LBUTTONDOWN:
  case WM_RBUTTONDOWN:
    m_mouseDown = 1;
    m_mouseClicked = 2; //no. of frames you set when you click (not to be confused with mouse held down)
    m_lastMouseX = m_mouseX;
    m_lastMouseY = -m_mouseY + 1;
    break;

    //case WM_LBUTTONUP:
  case WM_RBUTTONUP:
    m_mouseDown = 0;
    break;

  case WM_LBUTTONDOWN:
    // Shadertoy iMouse: compute pixel coords in render-target space (bottom-left origin)
    {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(GetPluginWindow(), &pt);
      RECT rc; GetClientRect(GetPluginWindow(), &rc);
      int cw = max((int)(rc.right - rc.left), 1);
      int ch = max((int)(rc.bottom - rc.top), 1);
      int tw = (m_lpDX && m_lpDX->m_backbuffer_width > 0) ? m_lpDX->m_backbuffer_width : cw;
      int th = (m_lpDX && m_lpDX->m_backbuffer_height > 0) ? m_lpDX->m_backbuffer_height : ch;
      float px = clamp((float)pt.x * tw / cw, 0.f, (float)(tw - 1));
      float py = clamp((float)(th - 1) - (float)pt.y * th / ch, 0.f, (float)(th - 1));
      m_stClickX = px;
      m_stClickY = py;
      m_stMouseX = px;
      m_stMouseY = py;
      m_stMouseDown = true;
      m_stMouseJustClicked = true;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);

  case WM_LBUTTONUP:
    m_stMouseDown = false;
    return DefWindowProc(hWnd, uMsg, wParam, lParam);

  case WM_KEYDOWN:    // virtual-key codes

    // Note that some keys will never reach this point, since they are
    //   intercepted by the plugin shell (see PluginShellWindowProc(),
    //   at the end of pluginshell.cpp for which ones).
    // For a complete list of virtual-key codes, look up the keyphrase
    //   "virtual-key codes [win32]" in the msdn help.
    nRepeat = LOWORD(lParam);

    // SPOUT DEBUG
    // Special case for F1 help display in pluginshell
    // to clear the vj screen of any existing text
    if (wParam == VK_F1) {
      // Bring up the VJ console if it has been minimised
      if (GetFocus() == GetPluginWindow()) {
        if (IsIconic(m_hTextWnd))
          ShowWindow(m_hTextWnd, SW_RESTORE);
      }
      // Change to regular
      m_UI_mode = UI_REGULAR;
      m_waitstring.bActive = false; // For F8
      // Toggle help display
      m_show_press_f1_msg = 0;
      ToggleHelp();
      return 0;
    }

    // Data-driven configurable hotkey lookup (local scope, UI_REGULAR only)
    if (m_UI_mode == UI_REGULAR) {
      UINT mods = 0;
      if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
      if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
      if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
      if (LookupLocalHotkey((UINT)wParam, mods)) return 0;
    }

    switch (wParam) {
    case VK_F2:
      if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
          (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        // Ctrl+Shift+F2: reset all hotkeys to defaults
        ResetHotkeyDefaults();
        SaveHotkeySettings();
        GenerateHelpText();
        g_nHelpLineCount = m_nHelpLineCount;
        HWND hRender = GetPluginWindow();
        if (hRender)
          PostMessage(hRender, WM_MW_REGISTER_HOTKEYS, 0, 0);
        AddNotification(L"All hotkeys reset to defaults");
        return 0;
      }
      if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        // Ctrl+F2: kill switch — disable all display outputs + reset open windows
        EnqueueRenderCmd(RenderCmd::DisableAllOutputs);
        AddNotification(L"All display outputs disabled");
        if (m_settingsWindow && m_settingsWindow->IsOpen())
          PostMessage(m_settingsWindow->GetHWND(), WM_MW_RESET_WINDOW, 0, 0);
        if (m_displaysWindow && m_displaysWindow->IsOpen())
          PostMessage(m_displaysWindow->GetHWND(), WM_MW_RESET_WINDOW, 0, 0);
        if (m_songInfoWindow && m_songInfoWindow->IsOpen())
          PostMessage(m_songInfoWindow->GetHWND(), WM_MW_RESET_WINDOW, 0, 0);
        if (m_hotkeysWindow && m_hotkeysWindow->IsOpen())
          PostMessage(m_hotkeysWindow->GetHWND(), WM_MW_RESET_WINDOW, 0, 0);
      }
      return 0;

    case 'L':
      // Ctrl+L: hardcoded fallback for opening Settings window
      if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        OpenSettingsWindow();
        return 0;
      }
      break;

    // All other F-keys, VK_SCROLL, Ctrl+Q/H are handled by the
    // data-driven binding table (LookupLocalHotkey above).

    } // end switch(wParam)
    //------------------------------------------


// next handle the waitstring case (for string-editing),
//	then the menu navigation case,
//  then handle normal case (handle the message normally or pass on to winamp)

// case 1: waitstring mode
    if (m_waitstring.bActive) {
      // handle arrow keys, home, end, etc.

      USHORT mask = 1 << (sizeof(SHORT) * 8 - 1);	// we want the highest-order bit
      bool bShiftHeldDown = (GetKeyState(VK_SHIFT) & mask) != 0;
      bool bCtrlHeldDown = (GetKeyState(VK_CONTROL) & mask) != 0;

      if (wParam == VK_LEFT || wParam == VK_RIGHT ||
        wParam == VK_HOME || wParam == VK_END ||
        wParam == VK_UP || wParam == VK_DOWN) {
        if (bShiftHeldDown) {
          if (m_waitstring.nSelAnchorPos == -1)
            m_waitstring.nSelAnchorPos = m_waitstring.nCursorPos;
        }
        else {
          m_waitstring.nSelAnchorPos = -1;
        }
      }

      if (bCtrlHeldDown)  // copy/cut/paste
      {
        switch (wParam) {
        case 'c':
        case 'C':
        case VK_INSERT:
          WaitString_Copy();
          return 0; // we processed (or absorbed) the key
        case 'x':
        case 'X':
          WaitString_Cut();
          return 0; // we processed (or absorbed) the key
        case 'v':
        case 'V':
          WaitString_Paste();
          return 0; // we processed (or absorbed) the key
        case VK_LEFT:	WaitString_SeekLeftWord();	return 0; // we processed (or absorbed) the key
        case VK_RIGHT:	WaitString_SeekRightWord();	return 0; // we processed (or absorbed) the key
        case VK_HOME:	m_waitstring.nCursorPos = 0;	return 0; // we processed (or absorbed) the key
        case VK_END:
          if (m_waitstring.bDisplayAsCode) {
            m_waitstring.nCursorPos = lstrlenA((char*)m_waitstring.szText);
          }
          else {
            m_waitstring.nCursorPos = lstrlenW(m_waitstring.szText);
          }
          return 0; // we processed (or absorbed) the key
        case VK_RETURN:
          if (m_waitstring.bDisplayAsCode) {
            // CTRL+ENTER accepts the string -> finished editing
            //assert(m_pCurMenu);
            m_pCurMenu->OnWaitStringAccept(m_waitstring.szText);
            // OnWaitStringAccept calls the callback function.  See the
            // calls to CMenu::AddItem from milkdrop.cpp to find the
            // callback functions for different "waitstrings".
            m_waitstring.bActive = false;
            m_UI_mode = UI_MENU;
          }
          return 0; // we processed (or absorbed) the key
        }
      }
      else	// waitstring mode key pressed, and ctrl NOT held down
      {
        switch (wParam) {
        case VK_INSERT:
          m_waitstring.bOvertypeMode = !m_waitstring.bOvertypeMode;
          return 0; // we processed (or absorbed) the key

        case VK_LEFT:
          for (int rep = 0; rep < nRepeat; rep++)
            if (m_waitstring.nCursorPos > 0)
              m_waitstring.nCursorPos--;
          return 0; // we processed (or absorbed) the key

        case VK_RIGHT:
          for (int rep = 0; rep < nRepeat; rep++) {
            if (m_waitstring.bDisplayAsCode) {
              if (m_waitstring.nCursorPos < (int)lstrlenA((char*)m_waitstring.szText))
                m_waitstring.nCursorPos++;
            }
            else {
              if (m_waitstring.nCursorPos < (int)lstrlenW(m_waitstring.szText))
                m_waitstring.nCursorPos++;
            }
          }
          return 0; // we processed (or absorbed) the key

        case VK_HOME:
          m_waitstring.nCursorPos -= WaitString_GetCursorColumn();
          return 0; // we processed (or absorbed) the key

        case VK_END:
          m_waitstring.nCursorPos += WaitString_GetLineLength() - WaitString_GetCursorColumn();
          return 0; // we processed (or absorbed) the key

        case VK_UP:
          for (int rep = 0; rep < nRepeat; rep++)
            WaitString_SeekUpOneLine();
          return 0; // we processed (or absorbed) the key

        case VK_DOWN:
          for (int rep = 0; rep < nRepeat; rep++)
            WaitString_SeekDownOneLine();
          return 0; // we processed (or absorbed) the key

        case VK_BACK:
          if (m_waitstring.nSelAnchorPos != -1) {
            WaitString_NukeSelection();
          }
          else if (m_waitstring.nCursorPos > 0) {
            int len;
            if (m_waitstring.bDisplayAsCode) {
              len = lstrlenA((char*)m_waitstring.szText);
            }
            else {
              len = lstrlenW(m_waitstring.szText);
            }
            int src_pos = m_waitstring.nCursorPos;
            int dst_pos = m_waitstring.nCursorPos - nRepeat;
            int gap = nRepeat;
            int copy_chars = len - m_waitstring.nCursorPos + 1;  // includes NULL @ end
            if (dst_pos < 0) {
              gap += dst_pos;
              //copy_chars += dst_pos;
              dst_pos = 0;
            }

            if (m_waitstring.bDisplayAsCode) {
              char* ptr = (char*)m_waitstring.szText;
              for (int i = 0; i < copy_chars; i++)
                *(ptr + dst_pos + i) = *(ptr + src_pos + i);
            }
            else {
              for (int i = 0; i < copy_chars; i++)
                m_waitstring.szText[dst_pos + i] = m_waitstring.szText[src_pos + i];
            }
            m_waitstring.nCursorPos -= gap;
          }
          return 0; // we processed (or absorbed) the key

        case VK_DELETE:
          if (m_waitstring.nSelAnchorPos != -1) {
            WaitString_NukeSelection();
          }
          else {
            if (m_waitstring.bDisplayAsCode) {
              int len = lstrlenA((char*)m_waitstring.szText);
              char* ptr = (char*)m_waitstring.szText;
              for (int i = m_waitstring.nCursorPos; i <= len - nRepeat; i++)
                *(ptr + i) = *(ptr + i + nRepeat);
            }
            else {
              int len = lstrlenW(m_waitstring.szText);
              for (int i = m_waitstring.nCursorPos; i <= len - nRepeat; i++)
                m_waitstring.szText[i] = m_waitstring.szText[i + nRepeat];
            }
          }
          return 0; // we processed (or absorbed) the key

        case VK_RETURN:
          if (m_UI_mode == UI_LOAD_RENAME)	// rename (move) the file
          {
            // first add pathnames to filenames
            wchar_t szOldFile[512];
            wchar_t szNewFile[512];
            lstrcpyW(szOldFile, GetPresetDir());
            lstrcpyW(szNewFile, GetPresetDir());
            lstrcatW(szOldFile, m_presets[m_nPresetListCurPos].szFilename.c_str());
            lstrcatW(szNewFile, m_waitstring.szText);
            lstrcatW(szNewFile, L".milk");

            RenamePresetFile(szOldFile, szNewFile);
          }
          else if (m_UI_mode == UI_IMPORT_WAVE ||
            m_UI_mode == UI_EXPORT_WAVE ||
            m_UI_mode == UI_IMPORT_SHAPE ||
            m_UI_mode == UI_EXPORT_SHAPE) {
            int bWave = (m_UI_mode == UI_IMPORT_WAVE || m_UI_mode == UI_EXPORT_WAVE);
            int bImport = (m_UI_mode == UI_IMPORT_WAVE || m_UI_mode == UI_IMPORT_SHAPE);

            int i = m_pCurMenu->GetCurItem()->m_lParam;
            int ret;
            switch (m_UI_mode) {
            case UI_IMPORT_WAVE: ret = m_pState->m_wave[i].Import(NULL, m_waitstring.szText, 0); break;
            case UI_EXPORT_WAVE: ret = m_pState->m_wave[i].Export(NULL, m_waitstring.szText, 0); break;
            case UI_IMPORT_SHAPE: ret = m_pState->m_shape[i].Import(NULL, m_waitstring.szText, 0); break;
            case UI_EXPORT_SHAPE: ret = m_pState->m_shape[i].Export(NULL, m_waitstring.szText, 0); break;
            }

            if (bImport)
              m_pState->RecompileExpressions(1);

            //m_fShowUserMessageUntilThisTime = GetTime() - 1.0f;	// if there was an error message already, clear it
            if (!ret) {
              wchar_t buf[1024];
              if (m_UI_mode == UI_IMPORT_WAVE || m_UI_mode == UI_IMPORT_SHAPE)
                wasabiApiLangString(IDS_ERROR_IMPORTING_BAD_FILENAME, buf, 1024);
              else
                wasabiApiLangString(IDS_ERROR_IMPORTING_BAD_FILENAME_OR_NOT_OVERWRITEABLE, buf, 1024);
              AddError(wasabiApiLangString(IDS_STRING_TOO_LONG), m_ErrorDuration, ERR_MISC, true);
            }

            m_waitstring.bActive = false;
            m_UI_mode = UI_MENU;
            //m_bPresetLockedByCode = false;
          }
          else if (m_UI_mode == UI_SAVEAS) {
            // first add pathname + extension to filename
            wchar_t szNewFile[512];
            swprintf(szNewFile, L"%s%s.milk", GetPresetDir(), m_waitstring.szText);

            if (GetFileAttributesW(szNewFile) != -1)		// check if file already exists
            {
              // file already exists -> overwrite it?
              m_waitstring.bActive = false;
              m_UI_mode = UI_SAVE_OVERWRITE;
            }
            else {
              SavePresetAs(szNewFile);

              // exit waitstring mode
              m_UI_mode = UI_REGULAR;
              m_waitstring.bActive = false;
              //m_bPresetLockedByCode = false;
            }
          }
          else if (m_UI_mode == UI_EDIT_MENU_STRING) {
            if (m_waitstring.bDisplayAsCode) {
              if (m_waitstring.nSelAnchorPos != -1)
                WaitString_NukeSelection();

              int len = lstrlenA((char*)m_waitstring.szText);
              char* ptr = (char*)m_waitstring.szText;
              if (len + 1 < m_waitstring.nMaxLen) {
                // insert a linefeed.  Use CTRL+return to accept changes in this case.
                for (int pos = len + 1; pos > m_waitstring.nCursorPos; pos--)
                  *(ptr + pos) = *(ptr + pos - 1);
                *(ptr + m_waitstring.nCursorPos++) = LINEFEED_CONTROL_CHAR;

                //m_fShowUserMessageUntilThisTime = GetTime() - 1.0f;	// if there was an error message already, clear it
              }
              else {
                // m_waitstring.szText has reached its limit
                AddError(wasabiApiLangString(IDS_STRING_TOO_LONG), m_ErrorDuration, ERR_MISC, true);
              }
            }
            else {
              // finished editing
              //assert(m_pCurMenu);
              m_pCurMenu->OnWaitStringAccept(m_waitstring.szText);
              // OnWaitStringAccept calls the callback function.  See the
              // calls to CMenu::AddItem from milkdrop.cpp to find the
              // callback functions for different "waitstrings".
              m_waitstring.bActive = false;
              m_UI_mode = UI_MENU;
            }
          }
          else if (m_UI_mode == UI_CHANGEDIR) {
            //m_fShowUserMessageUntilThisTime = GetTime();	// if there was an error message already, clear it

            bool bSuccess = ChangePresetDir(m_waitstring.szText, g_engine.m_szPresetDir);
            if (bSuccess) {

              // set current preset index to -1 because current preset is no longer in the list
              m_nCurrentPreset = -1;

              // go to file load menu
              m_waitstring.bActive = false;
              m_UI_mode = UI_LOAD;

              ClearErrors(ERR_MISC);
            }
          }
          return 0; // we processed (or absorbed) the key

        case VK_ESCAPE:
          if (m_UI_mode == UI_LOAD_RENAME) {
            m_waitstring.bActive = false;
            m_UI_mode = UI_LOAD;
          }
          else if (
            m_UI_mode == UI_SAVEAS ||
            m_UI_mode == UI_SAVE_OVERWRITE ||
            m_UI_mode == UI_EXPORT_SHAPE ||
            m_UI_mode == UI_IMPORT_SHAPE ||
            m_UI_mode == UI_EXPORT_WAVE ||
            m_UI_mode == UI_IMPORT_WAVE) {
            //m_bPresetLockedByCode = false;
            m_waitstring.bActive = false;
            m_UI_mode = UI_REGULAR;
          }
          else if (m_UI_mode == UI_EDIT_MENU_STRING) {
            m_waitstring.bActive = false;
            if (m_waitstring.bDisplayAsCode)    // if were editing code...
              m_UI_mode = UI_MENU;    // return to menu
            else
              m_UI_mode = UI_REGULAR; // otherwise don't (we might have been editing a filename, for example)
          }
          else /*if (m_UI_mode == UI_EDIT_MENU_STRING || m_UI_mode == UI_CHANGEDIR || 1)*/
          {
            m_waitstring.bActive = false;
            m_UI_mode = UI_REGULAR;
          }
          return 0; // we processed (or absorbed) the key
        }
      }

      // don't let keys go anywhere else
      return 0; // we processed (or absorbed) the key
    }

    // case 2: menu is up & gets the keyboard input
    if (m_UI_mode == UI_MENU) {
      //assert(m_pCurMenu);
      if (m_pCurMenu->HandleKeydown(hWnd, uMsg, wParam, lParam) == 0)
        return 0; // we processed (or absorbed) the key
    }

    // case 2b: settings screen keyboard input
    if (m_UI_mode == UI_SETTINGS) {
      switch (wParam) {
      case VK_UP:
        m_nSettingsCurSel--;
        if (m_nSettingsCurSel < 0) m_nSettingsCurSel = SET_COUNT - 1;
        return 0;
      case VK_DOWN:
        m_nSettingsCurSel++;
        if (m_nSettingsCurSel >= SET_COUNT) m_nSettingsCurSel = 0;
        return 0;
      case VK_RETURN:
        if (g_settingsDesc[m_nSettingsCurSel].type == ST_PATH) {
          OpenFolderPickerForPresetDir();
        }
        else if (g_settingsDesc[m_nSettingsCurSel].type == ST_BOOL) {
          ToggleSetting(g_settingsDesc[m_nSettingsCurSel].id);
        }
        return 0;
      case VK_LEFT:
        if (g_settingsDesc[m_nSettingsCurSel].type == ST_FLOAT || g_settingsDesc[m_nSettingsCurSel].type == ST_INT)
          AdjustSetting(g_settingsDesc[m_nSettingsCurSel].id, -1);
        return 0;
      case VK_RIGHT:
        if (g_settingsDesc[m_nSettingsCurSel].type == ST_FLOAT || g_settingsDesc[m_nSettingsCurSel].type == ST_INT)
          AdjustSetting(g_settingsDesc[m_nSettingsCurSel].id, 1);
        return 0;
      case VK_ESCAPE:
        m_UI_mode = UI_REGULAR;
        return 0;
      }
      // absorb all other keys while in settings
      return 0;
    }

    // case 3: handle non-character keys (virtual keys) and return 0.
        //         if we don't handle them, return 1, and the shell will
        //         (passing some to the shell's key bindings, some to Winamp,
        //          and some to DefWindowProc)
    //		note: regular hotkeys should be handled in HandleRegularKey.
    switch (wParam) {
    // VK_LEFT: UI_REGULAR media control handled by binding table
    case VK_RIGHT:
      if (m_UI_mode == UI_LOAD) {
        return 0; // absorb right arrow in preset browser
      }
      else if (m_UI_mode == UI_UPGRADE_PIXEL_SHADER) {
        m_UI_mode = UI_MENU;
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_MASHUP) {
        m_nMashSlot = min(MASH_SLOTS - 1, m_nMashSlot + 1);
        return 0; // we processed (or absorbed) the key
      }
      // UI_REGULAR media control handled by binding table
      break;

    case VK_ESCAPE:
      if (m_UI_mode == UI_LOAD || m_UI_mode == UI_MENU || m_UI_mode == UI_MASHUP || m_UI_mode == UI_SETTINGS) {
        m_UI_mode = UI_REGULAR;
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_LOAD_DEL) {
        m_UI_mode = UI_LOAD;
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_UPGRADE_PIXEL_SHADER) {
        m_UI_mode = UI_MENU;
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_SAVE_OVERWRITE) {
        m_UI_mode = UI_SAVEAS;
        // return to waitstring mode, leaving all the parameters as they were before:
        m_waitstring.bActive = true;
        return 0; // we processed (or absorbed) the key
      }
      // Close help overlay if showing
      else if (m_show_help) {
        ToggleHelp();
        return 0;
      }
      // SPOUT - put back in for vj mode.
      else {
        // Don't close if esc pressed when vj window has focus
        if (GetFocus() == GetPluginWindow()) {
          if (!IsBorderlessFullscreen(GetPluginWindow())) {
            bool isShiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (isShiftPressed || MessageBoxA(GetPluginWindow(), "Close MDropDX12 Visualizer?\n\n(You may also use SHIFT+ESC or RIGHT+LEFT MOUSE BUTTON\nto close without confirmation)", "MDropDX12 Visualizer", MB_YESNO | MB_TOPMOST) == IDYES) {
              PostMessage(hWnd, WM_CLOSE, 0, 0);
            }
            return 0;
          }
        }
      }
      /*else if (hwnd == GetPluginWindow())		// (don't close on ESC for text window)
      {
        dumpmsg("User pressed ESCAPE");
        //m_bExiting = true;
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0; // we processed (or absorbed) the key
      }*/
      break;

    case VK_UP:
      if (m_UI_mode == UI_MASHUP) {
        for (int rep = 0; rep < nRepeat; rep++)
          m_nMashPreset[m_nMashSlot] = max(m_nMashPreset[m_nMashSlot] - 1, m_nDirs);
        m_nLastMashChangeFrame[m_nMashSlot] = GetFrame();  // causes delayed apply
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_LOAD) {
        for (int rep = 0; rep < nRepeat; rep++)
          if (m_nPresetListCurPos > 0)
            m_nPresetListCurPos--;
        return 0; // we processed (or absorbed) the key
      }
      // UI_REGULAR opacity/media handled by binding table
      break;

    case VK_DOWN:
      if (m_UI_mode == UI_MASHUP) {
        for (int rep = 0; rep < nRepeat; rep++)
          m_nMashPreset[m_nMashSlot] = min(m_nMashPreset[m_nMashSlot] + 1, m_nPresets - 1);
        m_nLastMashChangeFrame[m_nMashSlot] = GetFrame();  // causes delayed apply
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_LOAD) {
        for (int rep = 0; rep < nRepeat; rep++)
          if (m_nPresetListCurPos < m_nPresets - 1)
            m_nPresetListCurPos++;
        return 0; // we processed (or absorbed) the key
      }
      // UI_REGULAR opacity/media handled by binding table
      break;

    // X/C/V/A media keys and Ctrl combos handled by binding table
    case VK_SPACE:
      if (m_UI_mode == UI_LOAD)
        goto HitEnterFromLoadMenu;
      // UI_REGULAR next-preset handled by binding table
      break;

    case VK_PRIOR:
      if (m_UI_mode == UI_LOAD || m_UI_mode == UI_MASHUP) {
        m_bUserPagedUp = true;
        if (m_UI_mode == UI_MASHUP)
          m_nLastMashChangeFrame[m_nMashSlot] = GetFrame();  // causes delayed apply
        return 0; // we processed (or absorbed) the key
      }
      break;
    case VK_NEXT:
      if (m_UI_mode == UI_LOAD || m_UI_mode == UI_MASHUP) {
        m_bUserPagedDown = true;
        if (m_UI_mode == UI_MASHUP)
          m_nLastMashChangeFrame[m_nMashSlot] = GetFrame();  // causes delayed apply
        return 0; // we processed (or absorbed) the key
      }
      break;
    case VK_HOME:
      if (m_UI_mode == UI_LOAD) {
        m_nPresetListCurPos = 0;
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_MASHUP) {
        m_nMashPreset[m_nMashSlot] = m_nDirs;
        m_nLastMashChangeFrame[m_nMashSlot] = GetFrame();  // causes delayed apply
        return 0; // we processed (or absorbed) the key
      }
      break;
    case VK_END:
      // printf("VK_END (%d)\n", m_UI_mode);
      if (m_UI_mode == UI_LOAD) // 2
      {
        m_nPresetListCurPos = m_nPresets - 1;
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_MASHUP) // 14
      {
        m_nMashPreset[m_nMashSlot] = m_nPresets - 1;
        m_nLastMashChangeFrame[m_nMashSlot] = GetFrame();  // causes delayed apply
        return 0; // we processed (or absorbed) the key
      }
      break;

    case VK_DELETE:
      if (m_UI_mode == UI_LOAD) {
        if (m_presets[m_nPresetListCurPos].szFilename.c_str()[0] != '*')	// can't delete directories
          m_UI_mode = UI_LOAD_DEL;
        return 0; // we processed (or absorbed) the key
      }
      else //if (m_nNumericInputDigits == 0)
      {
        if (m_nNumericInputMode == NUMERIC_INPUT_MODE_CUST_MSG) {
          m_nNumericInputDigits = 0;
          m_nNumericInputNum = 0;

          // stop display of text messages
          KillAllSupertexts();

          return 0; // we processed (or absorbed) the key
        }
        else if (m_nNumericInputMode == NUMERIC_INPUT_MODE_SPRITE) {
          // kill newest sprite (regular DELETE key)
          // oldest sprite (SHIFT + DELETE),
          // or all sprites (CTRL + SHIFT + DELETE).

          m_nNumericInputDigits = 0;
          m_nNumericInputNum = 0;

          USHORT mask = 1 << (sizeof(SHORT) * 8 - 1);	// we want the highest-order bit
          bool bShiftHeldDown = (GetKeyState(VK_SHIFT) & mask) != 0;
          bool bCtrlHeldDown = (GetKeyState(VK_CONTROL) & mask) != 0;

          if (bShiftHeldDown && bCtrlHeldDown) {
            for (int x = 0; x < NUM_TEX; x++)
              m_texmgr.KillTex(x);
          }
          else {
            int newest = -1;
            int frame;
            for (int x = 0; x < NUM_TEX; x++) {
              if (m_texmgr.m_tex[x].pSurface) {
                if ((newest == -1) ||
                  (!bShiftHeldDown && m_texmgr.m_tex[x].nStartFrame > frame) ||
                  (bShiftHeldDown && m_texmgr.m_tex[x].nStartFrame < frame)) {
                  newest = x;
                  frame = m_texmgr.m_tex[x].nStartFrame;
                }
              }
            }

            if (newest != -1)
              m_texmgr.KillTex(newest);
          }
          return 0; // we processed (or absorbed) the key
        }
      }
      break;

    case VK_INSERT:		// RENAME
      if (m_UI_mode == UI_LOAD) {
        if (m_presets[m_nPresetListCurPos].szFilename.c_str()[0] != '*')	// can't rename directories
        {
          // go into RENAME mode
          m_UI_mode = UI_LOAD_RENAME;
          m_waitstring.bActive = true;
          m_waitstring.bFilterBadChars = true;
          m_waitstring.bDisplayAsCode = false;
          m_waitstring.nSelAnchorPos = -1;
          m_waitstring.nMaxLen = min(sizeof(m_waitstring.szText) - 1, MAX_PATH - lstrlenW(GetPresetDir()) - 6);	// 6 for the extension + null char.  We set this because win32 LoadFile, MoveFile, etc. barf if the path+filename+ext are > MAX_PATH chars.

          // initial string is the filename, minus the extension
          lstrcpyW(m_waitstring.szText, m_presets[m_nPresetListCurPos].szFilename.c_str());
          RemoveExtension(m_waitstring.szText);

          // set the prompt & 'tooltip'
          swprintf(m_waitstring.szPrompt, wasabiApiLangString(IDS_ENTER_THE_NEW_NAME_FOR_X), m_waitstring.szText);
          m_waitstring.szToolTip[0] = 0;

          // set the starting edit position
          m_waitstring.nCursorPos = lstrlenW(m_waitstring.szText);
        }
        return 0; // we processed (or absorbed) the key
      }
      break;

    case VK_RETURN:

      if (m_UI_mode == UI_MASHUP) {
        m_nLastMashChangeFrame[m_nMashSlot] = GetFrame() + MASH_APPLY_DELAY_FRAMES;  // causes instant apply
        return 0; // we processed (or absorbed) the key
      }
      else if (m_UI_mode == UI_LOAD) {
      HitEnterFromLoadMenu:

        if (m_presets[m_nPresetListCurPos].szFilename.c_str()[0] == '*') {
          // CHANGE DIRECTORY
          wchar_t* p = GetPresetDir();

          if (wcscmp(m_presets[m_nPresetListCurPos].szFilename.c_str(), L"*..") == 0) {
            // back up one dir
            wchar_t* p2 = wcsrchr(p, L'\\');
            if (p2 && p2 > p) {
              *p2 = 0;
              p2 = wcsrchr(p, L'\\');
              if (p2) *(p2 + 1) = 0;
              else lstrcatW(p, L"\\");  // keep drive root as "X:\"
            }
          }
          else {
            // open subdir
            lstrcatW(p, &m_presets[m_nPresetListCurPos].szFilename.c_str()[1]);
            lstrcatW(p, L"\\");
          }

          WritePrivateProfileStringW(L"Settings", L"szPresetDir", GetPresetDir(), GetConfigIniFile());

          UpdatePresetList(true, true, false);

          // set current preset index to -1 because current preset is no longer in the list
          m_nCurrentPreset = -1;
        }
        else {
          // LOAD NEW PRESET
          m_nCurrentPreset = m_nPresetListCurPos;

          // first take the filename and prepend the path.  (already has extension)
          wchar_t s[MAX_PATH];
          lstrcpyW(s, GetPresetDir());	// note: m_szPresetDir always ends with '\'
          lstrcatW(s, m_presets[m_nCurrentPreset].szFilename.c_str());

          // now load (and blend to) the new preset
          m_presetHistoryPos = (m_presetHistoryPos + 1) % PRESET_HIST_LEN;
          LoadPreset(s, (wParam == VK_SPACE) ? m_fBlendTimeUser : 0);
        }
        return 0; // we processed (or absorbed) the key
      }
      break;

    case VK_BACK:
      if (m_UI_mode == UI_LOAD) {
        // Navigate up one directory in the preset browser
        wchar_t* p = GetPresetDir();
        wchar_t* p2 = wcsrchr(p, L'\\');
        if (p2 && p2 > p) {
          *p2 = 0;
          p2 = wcsrchr(p, L'\\');
          if (p2) *(p2 + 1) = 0;
          else lstrcatW(p, L"\\");  // keep drive root as "X:\"
          WritePrivateProfileStringW(L"Settings", L"szPresetDir", GetPresetDir(), GetConfigIniFile());
          UpdatePresetList(true, true, false);
          m_nCurrentPreset = -1;
        }
        return 0;
      }
      // UI_REGULAR prev-preset handled by binding table
      // Ctrl+Z/S/T/K handled by binding table
      break;
    }
    if (wParam == keyMappings[2])	// 'Y'
    {
      if (bCtrlHeldDown)      // stop display of custom message or song title.
      {
        KillAllSupertexts();
        return 0;
      }
    }
    // Handle character keys sent as WM_KEYDOWN by Milkwave Remote
    // (PostMessage WM_KEYDOWN with lParam=0 won't generate WM_CHAR via TranslateMessage)
    if (m_UI_mode == UI_REGULAR && !bCtrlHeldDown && !bShiftHeldDown) {
      switch (wParam) {
      case 'N':  // Sound Info toggle
        m_bShowDebugInfo = !m_bShowDebugInfo;
        return 0;
      case 'K':  // Sprite/Message input mode
      {
        USHORT mask2 = 1 << (sizeof(SHORT) * 8 - 1);
        bool bShift = (GetKeyState(VK_SHIFT) & mask2) != 0;
        if (bShift) {
          m_nNumericInputMode = NUMERIC_INPUT_MODE_SPRITE_KILL;
        } else {
          m_nNumericInputMode = (m_nNumericInputMode == NUMERIC_INPUT_MODE_SPRITE)
            ? NUMERIC_INPUT_MODE_CUST_MSG : NUMERIC_INPUT_MODE_SPRITE;
        }
        m_nNumericInputDigits = 0;
        m_nNumericInputNum = 0;
        return 0;
      }
      }
    }
    return 1; // end case WM_KEYDOWN

  case WM_KEYUP:
    return 1;
    break;

  default:
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    break;
  }

  return 0;
}

int Engine::HandleRegularKey(WPARAM wParam) {
  // here we handle all the normal keys for milkdrop-
  // these are the hotkeys that are used when you're not
  // in the middle of editing a string, navigating a menu, etc.

  // do not make references to virtual keys here; only
  // straight WM_CHAR messages should be sent in.

    // return 0 if you process/absorb the key; otherwise return 1.

  // SPOUT DEBUG for BeatDrop vj mode
  // For "L, "M', "S" and "VK_F8"
  // if pluginshell VK_F1 help has been pressed
  // reset help and clear the window

  if (m_UI_mode == UI_LOAD && ((wParam >= 'A' && wParam <= 'Z') || (wParam >= 'a' && wParam <= 'z'))) {
    SeekToPreset((char)wParam);
    return 0; // we processed (or absorbed) the key
  }
  else if (m_UI_mode == UI_MASHUP && wParam >= '1' && wParam <= ('0' + MASH_SLOTS)) {
    m_nMashSlot = (int)(wParam - '1');
  }
  else {
    // For UI_REGULAR mode, suppress character keys that are bound in the hotkey table.
    // The action was already dispatched by WM_KEYDOWN's LookupLocalHotkey().
    if (m_UI_mode == UI_REGULAR) {
      SHORT vkScan = VkKeyScanW((wchar_t)wParam);
      if (vkScan != -1) {
        UINT vk = LOBYTE(vkScan);
        UINT scanMods = HIBYTE(vkScan);
        UINT mods = 0;
        if (scanMods & 1) mods |= MOD_SHIFT;
        if (scanMods & 2) mods |= MOD_CONTROL;
        if (scanMods & 4) mods |= MOD_ALT;
        for (int i = 0; i < NUM_HOTKEYS; i++) {
          if (m_hotkeys[i].vk == vk && m_hotkeys[i].modifiers == mods &&
              m_hotkeys[i].scope == HKSCOPE_LOCAL) {
            return 0; // already dispatched by WM_KEYDOWN
          }
        }
      }
    }

    switch (wParam) {
    // ── Numeric input (sprites, custom messages) ──
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    {
      int digit = (int)(wParam - '0');
      m_nNumericInputNum = (m_nNumericInputNum * 10) + digit;
      m_nNumericInputDigits++;
      if (m_nNumericInputDigits >= 2) {
        if (m_nNumericInputMode == NUMERIC_INPUT_MODE_CUST_MSG)
          LaunchCustomMessage(m_nNumericInputNum);
        else if (m_nNumericInputMode == NUMERIC_INPUT_MODE_SPRITE)
          m_pendingSpriteLoads.push_back({m_nNumericInputNum, -1});
        else if (m_nNumericInputMode == NUMERIC_INPUT_MODE_SPRITE_KILL) {
          for (int x = 0; x < NUM_TEX; x++)
            if (m_texmgr.m_tex[x].nUserData == m_nNumericInputNum)
              m_texmgr.KillTex(x);
        }
        m_nNumericInputDigits = 0;
        m_nNumericInputNum = 0;
      }
    }
    return 0;

    // ── Modal keys: only non-REGULAR behavior remains ──

    case 'h':
    case 'H':
      // Mashup mode: randomize current slot (h) or all slots (H)
      if (m_UI_mode == UI_MASHUP) {
        if (wParam == 'h') {
          m_nMashPreset[m_nMashSlot] = m_nDirs + (rand() % (m_nPresets - m_nDirs));
          m_nLastMashChangeFrame[m_nMashSlot] = GetFrame() + MASH_APPLY_DELAY_FRAMES;
        }
        else {
          for (int mash = 0; mash < MASH_SLOTS; mash++) {
            m_nMashPreset[mash] = m_nDirs + (rand() % (m_nPresets - m_nDirs));
            m_nLastMashChangeFrame[mash] = GetFrame() + MASH_APPLY_DELAY_FRAMES;
          }
        }
      }
      // UI_REGULAR hard cut handled by binding table
      return 0;

    case 'k':
    case 'K':
    {
      // Shift+K: enter sprite-kill input mode (not in binding table as separate action)
      USHORT mask = 1 << (sizeof(SHORT) * 8 - 1);
      bool bShiftHeldDown = (GetKeyState(VK_SHIFT) & mask) != 0;
      if (bShiftHeldDown) {
        m_nNumericInputMode = NUMERIC_INPUT_MODE_SPRITE_KILL;
        SendMessageToMDropDX12Remote(L"STATUS=Sprite Mode set");
        PostMessageToMDropDX12Remote(WM_USER_SPRITE_MODE);
        m_nNumericInputNum = 0;
        m_nNumericInputDigits = 0;
      }
      // Non-shift k/K sprite mode toggle handled by binding table
    }
    return 0;

    case 'l':
    case 'L':
      // Menu → Load transition (UI_REGULAR→UI_LOAD handled by binding table)
      m_show_help = 0;
      if (m_UI_mode == UI_MENU) {
        if (!DirHasMilkFilesHelper(m_szPresetDir)) {
          swprintf(m_szPresetDir, L"%spresets\\", m_szMilkdrop2Path);
          TryDescendIntoPresetSubdirHelper(m_szPresetDir);
          WritePrivateProfileStringW(L"Settings", L"szPresetDir", m_szPresetDir, GetConfigIniFile());
        }
        UpdatePresetList(false, true);
        m_UI_mode = UI_LOAD;
        m_bUserPagedUp = false;
        m_bUserPagedDown = false;
        return 0;
      }
      break;

    case 'm':
    case 'M':
      // Menu/Load toggle (UI_REGULAR→UI_MENU handled by binding table)
      m_show_help = 0;
      if (m_UI_mode == UI_MENU)
        m_UI_mode = UI_REGULAR;
      else if (m_UI_mode == UI_LOAD)
        m_UI_mode = UI_MENU;
      return 0;

    case 'y':
    case 'Y':
      return 0; // absorbed
    }
  }

  return 1;
}

void Engine::WaitString_NukeSelection() {
  if (m_waitstring.bActive &&
    m_waitstring.nSelAnchorPos != -1) {
    // nuke selection.  note: start & end are INCLUSIVE.
    int start = (m_waitstring.nCursorPos < m_waitstring.nSelAnchorPos) ? m_waitstring.nCursorPos : m_waitstring.nSelAnchorPos;
    int end = (m_waitstring.nCursorPos > m_waitstring.nSelAnchorPos) ? m_waitstring.nCursorPos - 1 : m_waitstring.nSelAnchorPos - 1;
    int len = (m_waitstring.bDisplayAsCode ? lstrlenA((char*)m_waitstring.szText) : lstrlenW(m_waitstring.szText));
    int how_far_to_shift = end - start + 1;
    int num_chars_to_shift = len - end;		// includes NULL char

    if (m_waitstring.bDisplayAsCode) {
      char* ptr = (char*)m_waitstring.szText;
      for (int i = 0; i < num_chars_to_shift; i++)
        *(ptr + start + i) = *(ptr + start + i + how_far_to_shift);
    }
    else {
      for (int i = 0; i < num_chars_to_shift; i++)
        m_waitstring.szText[start + i] = m_waitstring.szText[start + i + how_far_to_shift];
    }

    // clear selection
    m_waitstring.nCursorPos = start;
    m_waitstring.nSelAnchorPos = -1;
  }
}

void Engine::WaitString_Cut() {
  if (m_waitstring.bActive &&
    m_waitstring.nSelAnchorPos != -1) {
    WaitString_Copy();
    WaitString_NukeSelection();
  }
}

void Engine::WaitString_Copy() {
  if (m_waitstring.bActive &&
    m_waitstring.nSelAnchorPos != -1) {
    // note: start & end are INCLUSIVE.
    int start = (m_waitstring.nCursorPos < m_waitstring.nSelAnchorPos) ? m_waitstring.nCursorPos : m_waitstring.nSelAnchorPos;
    int end = (m_waitstring.nCursorPos > m_waitstring.nSelAnchorPos) ? m_waitstring.nCursorPos - 1 : m_waitstring.nSelAnchorPos - 1;
    int chars_to_copy = end - start + 1;

    if (m_waitstring.bDisplayAsCode) {
      char* ptr = (char*)m_waitstring.szText;
      for (int i = 0; i < chars_to_copy; i++)
        m_waitstring.szClipboard[i] = *(ptr + start + i);
      m_waitstring.szClipboard[chars_to_copy] = 0;

      char tmp[64000];
      ConvertLFCToCRsA(m_waitstring.szClipboard, tmp);
      copyStringToClipboardA(tmp);
    }
    else {
      for (int i = 0; i < chars_to_copy; i++)
        m_waitstring.szClipboardW[i] = m_waitstring.szText[start + i];
      m_waitstring.szClipboardW[chars_to_copy] = 0;

      wchar_t tmp[64000];
      ConvertLFCToCRsW(m_waitstring.szClipboardW, tmp);
      copyStringToClipboardW(tmp);
    }
  }
}

void Engine::WaitString_Paste() {
  // NOTE: if there is a selection, it is wiped out, and replaced with the clipboard contents.

  if (m_waitstring.bActive) {
    WaitString_NukeSelection();

    if (m_waitstring.bDisplayAsCode) {
      char tmp[64000];
      lstrcpyA(tmp, getStringFromClipboardA());
      ConvertCRsToLFCA(tmp, m_waitstring.szClipboard);
    }
    else {
      wchar_t tmp[64000];
      lstrcpyW(tmp, getStringFromClipboardW());
      ConvertCRsToLFCW(tmp, m_waitstring.szClipboardW);
    }

    int len;
    int chars_to_insert;

    if (m_waitstring.bDisplayAsCode) {
      len = lstrlenA((char*)m_waitstring.szText);
      chars_to_insert = lstrlenA(m_waitstring.szClipboard);
    }
    else {
      len = lstrlenW(m_waitstring.szText);
      chars_to_insert = lstrlenW(m_waitstring.szClipboardW);
    }

    if (len + chars_to_insert + 1 >= m_waitstring.nMaxLen) {
      chars_to_insert = m_waitstring.nMaxLen - len - 1;

      // inform user
      AddError(wasabiApiLangString(IDS_STRING_TOO_LONG), m_ErrorDuration, ERR_MISC, true);
    }
    else {
      //m_fShowUserMessageUntilThisTime = GetTime();	// if there was an error message already, clear it
    }

    int i;
    if (m_waitstring.bDisplayAsCode) {
      char* ptr = (char*)m_waitstring.szText;
      for (i = len; i >= m_waitstring.nCursorPos; i--)
        *(ptr + i + chars_to_insert) = *(ptr + i);
      for (i = 0; i < chars_to_insert; i++)
        *(ptr + i + m_waitstring.nCursorPos) = m_waitstring.szClipboard[i];
    }
    else {
      for (i = len; i >= m_waitstring.nCursorPos; i--)
        m_waitstring.szText[i + chars_to_insert] = m_waitstring.szText[i];
      for (i = 0; i < chars_to_insert; i++)
        m_waitstring.szText[i + m_waitstring.nCursorPos] = m_waitstring.szClipboardW[i];
    }
    m_waitstring.nCursorPos += chars_to_insert;
  }
}

void Engine::WaitString_SeekLeftWord() {
  // move to beginning of prior word
  if (m_waitstring.bDisplayAsCode) {
    char* ptr = (char*)m_waitstring.szText;
    while (m_waitstring.nCursorPos > 0 &&
      !IsAlphanumericChar(*(ptr + m_waitstring.nCursorPos - 1)))
      m_waitstring.nCursorPos--;

    while (m_waitstring.nCursorPos > 0 &&
      IsAlphanumericChar(*(ptr + m_waitstring.nCursorPos - 1)))
      m_waitstring.nCursorPos--;
  }
  else {
    while (m_waitstring.nCursorPos > 0 &&
      !IsAlphanumericChar(m_waitstring.szText[m_waitstring.nCursorPos - 1]))
      m_waitstring.nCursorPos--;

    while (m_waitstring.nCursorPos > 0 &&
      IsAlphanumericChar(m_waitstring.szText[m_waitstring.nCursorPos - 1]))
      m_waitstring.nCursorPos--;
  }
}

void Engine::WaitString_SeekRightWord() {
  // move to beginning of next word

  //testing  lotsa   stuff

  if (m_waitstring.bDisplayAsCode) {
    int len = lstrlenA((char*)m_waitstring.szText);

    char* ptr = (char*)m_waitstring.szText;
    while (m_waitstring.nCursorPos < len &&
      IsAlphanumericChar(*(ptr + m_waitstring.nCursorPos)))
      m_waitstring.nCursorPos++;

    while (m_waitstring.nCursorPos < len &&
      !IsAlphanumericChar(*(ptr + m_waitstring.nCursorPos)))
      m_waitstring.nCursorPos++;
  }
  else {
    int len = lstrlenW(m_waitstring.szText);

    while (m_waitstring.nCursorPos < len &&
      IsAlphanumericChar(m_waitstring.szText[m_waitstring.nCursorPos]))
      m_waitstring.nCursorPos++;

    while (m_waitstring.nCursorPos < len &&
      !IsAlphanumericChar(m_waitstring.szText[m_waitstring.nCursorPos]))
      m_waitstring.nCursorPos++;
  }
}

int Engine::WaitString_GetCursorColumn() {
  if (m_waitstring.bDisplayAsCode) {
    int column = 0;
    char* ptr = (char*)m_waitstring.szText;
    while (m_waitstring.nCursorPos - column - 1 >= 0 &&
      *(ptr + m_waitstring.nCursorPos - column - 1) != LINEFEED_CONTROL_CHAR)
      column++;

    return column;
  }
  else {
    return m_waitstring.nCursorPos;
  }
}

int	Engine::WaitString_GetLineLength() {
  int line_start = m_waitstring.nCursorPos - WaitString_GetCursorColumn();
  int line_length = 0;

  if (m_waitstring.bDisplayAsCode) {
    char* ptr = (char*)m_waitstring.szText;
    while (*(ptr + line_start + line_length) != 0 &&
      *(ptr + line_start + line_length) != LINEFEED_CONTROL_CHAR)
      line_length++;
  }
  else {
    while (m_waitstring.szText[line_start + line_length] != 0 &&
      m_waitstring.szText[line_start + line_length] != LINEFEED_CONTROL_CHAR)
      line_length++;
  }

  return line_length;
}

void Engine::WaitString_SeekUpOneLine() {
  int column = g_engine.WaitString_GetCursorColumn();

  if (column != m_waitstring.nCursorPos) {
    // seek to very end of previous line (cursor will be at the semicolon)
    m_waitstring.nCursorPos -= column + 1;

    int new_column = g_engine.WaitString_GetCursorColumn();

    if (new_column > column)
      m_waitstring.nCursorPos -= (new_column - column);
  }
}

void Engine::WaitString_SeekDownOneLine() {
  int column = g_engine.WaitString_GetCursorColumn();
  int newpos = m_waitstring.nCursorPos;

  char* ptr = (char*)m_waitstring.szText;
  while (*(ptr + newpos) != 0 && *(ptr + newpos) != LINEFEED_CONTROL_CHAR)
    newpos++;

  if (*(ptr + newpos) != 0) {
    m_waitstring.nCursorPos = newpos + 1;

    while (column > 0 &&
      *(ptr + m_waitstring.nCursorPos) != LINEFEED_CONTROL_CHAR &&
      *(ptr + m_waitstring.nCursorPos) != 0) {
      m_waitstring.nCursorPos++;
      column--;
    }
  }
}


// ─── Render Command Dispatch ─────────────────────────────────────────────────

void Engine::ExecuteRenderCommand(const RenderCommand& cmd) {
  switch (cmd.cmd) {
    case RenderCmd::ResetBuffers:
      ResetBufferAndFonts();
      break;
    case RenderCmd::ResizeWindow:
      OnUserResizeWindow();
      break;
    case RenderCmd::DeviceRecovery:
      m_bDeviceRecoveryPending = true;
      break;
    case RenderCmd::ToggleSpout:
      ToggleSpout();
      break;
    case RenderCmd::SpoutFixedSize:
      SetSpoutFixedSize(false, true);
      break;
    case RenderCmd::RefreshDisplays:
      for (auto& out : m_displayOutputs) {
        if (out.config.type == DisplayOutputType::Monitor && out.monitorState)
          DestroyDisplayOutput(out);
      }
      EnumerateDisplayOutputs();
      RefreshDisplaysTab();
      break;
    case RenderCmd::NextPreset:
      if (!m_bPresetLockedByCode)
        LoadRandomPreset(cmd.fParam > 0 ? cmd.fParam : m_fBlendTimeUser);
      break;
    case RenderCmd::PrevPreset:
      PrevPreset(cmd.fParam);
      m_fHardCutThresh *= 2.0f;
      break;
    case RenderCmd::LoadPreset:
      if (!m_bPresetLockedByCode)
        LoadRandomPreset(cmd.fParam > 0 ? cmd.fParam : m_fBlendTimeUser);
      break;
    case RenderCmd::CaptureScreenshot:
    {
      wchar_t filename[MAX_PATH];
      if (CaptureScreenshotWithFilename(filename, MAX_PATH)) {
        wchar_t msg[MAX_PATH + 32];
        swprintf_s(msg, MAX_PATH + 32, L"capture/%s saved", filename);
        AddNotification(msg);
      }
      break;
    }
    case RenderCmd::IPCMessage:
      if (!cmd.sParam.empty()) {
        std::wstring msgCopy = cmd.sParam;
        LaunchMessage(msgCopy.data());
      }
      break;
    case RenderCmd::PushSprite:
      m_pendingSpriteLoads.push_back({cmd.iParam1, cmd.iParam2});
      break;
    case RenderCmd::KillSprite:
      KillSprite(cmd.iParam1);
      break;
    case RenderCmd::LoadShaders:
      // Handled during preset load; placeholder for future use
      break;
    case RenderCmd::RecompileCompShader:
    {
      ClearErrors(ERR_PRESET);
      bool allOK = true;
      if (m_nMaxPSVersion > 0) {
        // Recompile Buffer A if present
        m_bHasBufferA = false;
        m_shaders.bufferA.Clear();
        if (m_pState->m_nBufferAPSVersion > 0 && m_pState->m_szBufferAShadersText[0]) {
          if (RecompilePShader(m_pState->m_szBufferAShadersText, &m_shaders.bufferA,
                               SHADER_COMP, false, m_pState->m_nBufferAPSVersion, false, "bufferA")) {
            m_bHasBufferA = true;
            m_bCompUsesFeedback = true;
          } else {
            allOK = false;
          }
        }
        // Recompile Buffer B if present
        m_bHasBufferB = false;
        m_shaders.bufferB.Clear();
        if (m_pState->m_nBufferBPSVersion > 0 && m_pState->m_szBufferBShadersText[0]) {
          if (RecompilePShader(m_pState->m_szBufferBShadersText, &m_shaders.bufferB,
                               SHADER_COMP, false, m_pState->m_nBufferBPSVersion, false, "bufferB")) {
            m_bHasBufferB = true;
          } else {
            allOK = false;
          }
        }
        // Recompile Image/comp shader last (so diag_comp_* reflects Image pass)
        m_shaders.comp.Clear();
        if (!RecompilePShader(m_pState->m_szCompShadersText, &m_shaders.comp,
                              SHADER_COMP, false, m_pState->m_nCompPSVersion, false)) {
          m_shaders.comp = m_fallbackShaders_ps.comp;
          allOK = false;
        }
        // Derive feedback flags from compiled shaders (same as LoadPresetTick)
        m_bCompUsesFeedback = m_bHasBufferA;
        m_bCompUsesImageFeedback = false;
        for (int i = 0; i < 16; i++) {
          if (m_shaders.comp.params.m_texcode[i] == TEX_FEEDBACK)
            m_bCompUsesFeedback = true;
          if (m_shaders.comp.params.m_texcode[i] == TEX_IMAGE_FEEDBACK)
            m_bCompUsesImageFeedback = true;
        }
        // Reset Shadertoy start frame so feedback buffers get cleared
        m_nShadertoyStartFrame = GetFrame();
        CreateDX12PresetPSOs();
      }
      m_nRecompileResult.store(allOK ? 2 : 3);  // 2=ok, 3=fail
      break;
    }
    case RenderCmd::DisableAllOutputs:
      for (auto& out : m_displayOutputs) {
        out.config.bEnabled = false;
        if (out.monitorState)
          DestroyDisplayOutput(out);
      }
      m_bMirrorsActive = false;
      SaveDisplayOutputSettings();
      RefreshDisplaysTab();
      break;
    case RenderCmd::Quit:
      // Handled by the render loop directly
      break;
  }
}

} // namespace mdrop
