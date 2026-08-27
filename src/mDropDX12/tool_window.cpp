/*
  ToolWindow — base class implementation for standalone tool windows on their own threads.
  Handles: thread lifecycle, message pump, dark theme painting, pin button,
  font +/- with cross-window sync, window size persistence, owner-draw controls.
*/

#include "tool_window.h"
#include "engine.h"
#include <algorithm>
#include "engine_helpers.h"
#include "utility.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include "config_store.h"
#pragma comment(lib, "dwmapi.lib")

//----------------------------------------------------------------------
// Undocumented uxtheme dark mode APIs (Windows 10 1903+ / build 18362+)
// Used by Notepad++, Windows Terminal, VS Code, etc. for dark popup menus.
//----------------------------------------------------------------------
enum PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
using fnFlushMenuThemes = void(WINAPI*)();
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);

static fnSetPreferredAppMode  pSetPreferredAppMode = nullptr;
static fnFlushMenuThemes      pFlushMenuThemes = nullptr;
static fnAllowDarkModeForWindow pAllowDarkModeForWindow = nullptr;
static bool s_bDarkAPIsResolved = false;

static void ResolveDarkModeAPIs() {
    if (s_bDarkAPIsResolved) return;
    s_bDarkAPIsResolved = true;
    HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");
    if (!hUx) return;
    pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
    pFlushMenuThemes = (fnFlushMenuThemes)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
    pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
}

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor / Destructor
//----------------------------------------------------------------------

ToolWindow::ToolWindow(Engine* pEngine, int defaultW, int defaultH)
  : m_pEngine(pEngine), m_nDefaultW(defaultW), m_nDefaultH(defaultH),
    m_nWndW(defaultW), m_nWndH(defaultH)
{
  if (m_pEngine)
    m_pEngine->m_toolWindows.push_back(this);
}

ToolWindow::~ToolWindow() {
  Close();
  if (m_pEngine) {
    auto& v = m_pEngine->m_toolWindows;
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
  }
}

//----------------------------------------------------------------------
// Open / Close
//----------------------------------------------------------------------

void ToolWindow::OnAlreadyOpen() {
  // Post to the tool window's own thread so SetWindowPos runs on the owning thread.
  // Cross-thread SetWindowPos/SetForegroundWindow can silently fail.
  PostMessage(m_hWnd, WM_MW_BRING_TO_TOP, 0, 0);
}

DWORD ToolWindow::GetCommonControlFlags() const {
  return ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_TAB_CLASSES;
}

void ToolWindow::Open() {
  if (m_hWnd && IsWindow(m_hWnd)) {
    OnAlreadyOpen();
    return;
  }
  if (m_bThreadRunning.load()) return;

  if (m_thread.joinable())
    m_thread.join();

  m_thread = std::thread(&ToolWindow::CreateOnThread, this);
}

void ToolWindow::Close() {
  SignalClose();
  WaitClose();
}

void ToolWindow::RequestLoadPreset(int nPresetIndex, const wchar_t* szPath,
                                   float fBlendTime) {
  if (!m_pEngine) return;
  RenderCommand cmd;
  cmd.cmd = RenderCmd::LoadPresetPath;
  cmd.iParam1 = nPresetIndex;
  cmd.fParam = (fBlendTime >= 0.0f) ? fBlendTime : m_pEngine->m_fBlendTimeUser;
  if (szPath && szPath[0])
    cmd.sParam = szPath;
  m_pEngine->EnqueueRenderCmd(std::move(cmd));
}

void ToolWindow::RequestNavPreset(int nDirection) {
  if (!m_pEngine) return;
  RenderCommand cmd;
  cmd.cmd = RenderCmd::NavPreset;
  cmd.iParam1 = nDirection;
  cmd.fParam = m_pEngine->m_fBlendTimeUser;
  m_pEngine->EnqueueRenderCmd(std::move(cmd));
}

void ToolWindow::SignalClose() {
  if (m_hWnd && IsWindow(m_hWnd))
    PostMessage(m_hWnd, WM_CLOSE, 0, 0);
}

void ToolWindow::WaitClose() {
  if (m_thread.joinable())
    m_thread.join();
}

bool ToolWindow::IsOpen() const {
  return m_hWnd && IsWindow(m_hWnd);
}

//----------------------------------------------------------------------
// Font sync broadcast — notifies all windows except the sender
//----------------------------------------------------------------------

void Engine::BroadcastFontSync(HWND hSender) {
  for (auto* tw : m_toolWindows) {
    if (tw->IsOpen() && tw->GetHWND() != hSender)
      PostMessage(tw->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  }
}

//----------------------------------------------------------------------
// Thread + Window Creation
//----------------------------------------------------------------------

void ToolWindow::LoadWindowPosition() {
  const wchar_t* sec = GetINISection();
  m_nWndW = Config().GetInt(sec, L"WndW", m_nDefaultW);
  m_nWndH = Config().GetInt(sec, L"WndH", m_nDefaultH);
  // GetSignedInt, not GetInt: the plain one is bug-compatible with
  // GetPrivateProfileIntW, which reports zero for any negative value, and a
  // monitor placed left of or above the primary gives windows negative
  // coordinates. Saving x=-1800 and reading back 0 is how a window on the
  // left-hand screen jumps to the primary one on restart.
  //
  // INT_MIN is "never saved". Plain -1 cannot mean that here for the same
  // reason: it is a real coordinate.
  auto readPos = [&](const wchar_t* key) -> int {
    return Config().Has(sec, key) ? Config().GetSignedInt(sec, key, INT_MIN)
                                  : INT_MIN;
  };
  m_nPosX = readPos(L"PosX");
  m_nPosY = readPos(L"PosY");
  m_bOnTop = Config().GetInt(sec, L"OnTop", 1) != 0; // default sticky
  if (m_nWndW < GetMinWidth()) m_nWndW = GetMinWidth();
  if (m_nWndH < GetMinHeight()) m_nWndH = GetMinHeight();

  // The saved position may name a monitor that is no longer attached, or one
  // whose coordinates moved when the desktop was rearranged. Either way the
  // window would open somewhere the user cannot reach it.
  if (m_nPosX != INT_MIN && m_nPosY != INT_MIN)
    ClampToVisibleMonitor(m_nPosX, m_nPosY, m_nWndW, m_nWndH);
}

// Pull a saved window rect back onto a monitor that is actually attached.
//
// Two things put a window somewhere unusable: a display being unplugged (its
// coordinates stop belonging to any monitor at all) and the desktop being
// rearranged so a window that fitted now hangs over the edge of every screen.
// MONITOR_DEFAULTTONEAREST answers with a real monitor in both cases, so the
// window lands on the closest surviving display rather than always the primary.
void ToolWindow::ClampToVisibleMonitor(int& posX, int& posY, int w, int h)
{
  RECT rcWnd = { posX, posY, posX + w, posY + h };

  HMONITOR hMon = MonitorFromRect(&rcWnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  if (!hMon || !GetMonitorInfo(hMon, &mi)) {
    posX = INT_MIN;   // no monitor info at all: fall back to the centring path
    posY = INT_MIN;
    return;
  }

  const int workW = mi.rcWork.right - mi.rcWork.left;
  const int workH = mi.rcWork.bottom - mi.rcWork.top;

  // A window wider or taller than the work area cannot be contained; centre
  // that axis instead so at least the middle of it is reachable.
  const int newX = (w >= workW) ? mi.rcWork.left + (workW - w) / 2
                                : min(max(posX, mi.rcWork.left), mi.rcWork.right - w);
  const int newY = (h >= workH) ? mi.rcWork.top + (workH - h) / 2
                                : min(max(posY, mi.rcWork.top), mi.rcWork.bottom - h);

  if (newX != posX || newY != posY) {
    DLOG_INFO("ToolWindow: saved position %d,%d was off the attached displays, moved to %d,%d",
              posX, posY, newX, newY);
    posX = newX;
    posY = newY;
  }
}

void ToolWindow::SaveWindowPosition() {
  if (!m_hWnd) return;

  // A test run must not repossess the user's window layout.
  //
  // This is called on drag-end and on close, so a harness that opens a tool
  // window, moves it somewhere convenient and closes it was overwriting the
  // remembered position permanently -- the user came back to windows parked
  // wherever the last test left them, on whichever display it used, and had to
  // put every one of them back by hand.
  //
  // Testing mode positions tool windows on the render window's monitor (see
  // Open) precisely because that placement is disposable. Persisting it would
  // make it permanent, which is the opposite of the point.
  if (m_pEngine->m_bTestingMode) return;

  const wchar_t* sec = GetINISection();
  wchar_t buf[16];
  RECT rc;
  // A maximised window must not have its maximised geometry persisted as the
  // restored size -- reopening would then come up filling the monitor with no
  // way back. GetWindowPlacement reports the restored rect either way.
  WINDOWPLACEMENT wp = { sizeof(wp) };
  if (IsZoomed(m_hWnd) && GetWindowPlacement(m_hWnd, &wp))
    rc = wp.rcNormalPosition;
  else
    GetWindowRect(m_hWnd, &rc);
  swprintf(buf, 16, L"%d", rc.right - rc.left);
  Config().SetString(sec, L"WndW", buf);
  swprintf(buf, 16, L"%d", rc.bottom - rc.top);
  Config().SetString(sec, L"WndH", buf);
  swprintf(buf, 16, L"%d", (int)rc.left);
  Config().SetString(sec, L"PosX", buf);
  swprintf(buf, 16, L"%d", (int)rc.top);
  Config().SetString(sec, L"PosY", buf);
  Config().SetString(sec, L"OnTop", m_bOnTop ? L"1" : L"0");
}

void ToolWindow::CreateOnThread() {
  m_bThreadRunning.store(true);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  // Register window class (idempotent — RegisterClassEx fails silently if already registered)
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = BaseWndProc;
  wc.hInstance = GetModuleHandle(NULL);
  wc.lpszClassName = GetWindowClass();
  wc.hbrBackground = m_pEngine->IsDarkTheme()
    ? CreateSolidBrush(m_pEngine->m_colSettingsBg)
    : (HBRUSH)(COLOR_BTNFACE + 1);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  RegisterClassExW(&wc);

  // Init common controls
  INITCOMMONCONTROLSEX icex = { sizeof(icex), GetCommonControlFlags() };
  InitCommonControlsEx(&icex);

  // Ensure theme brushes are ready
  m_pEngine->LoadSettingsThemeFromINI();

  // Load persisted size/position
  LoadWindowPosition();

  // Centre on the monitor the RENDER window is on. SM_CXSCREEN is the PRIMARY
  // monitor's size, so it puts the window on the main display no matter where
  // the visualizer is.
  auto CenterOnRenderMonitor = [&](int& outX, int& outY) {
    HWND hRender = m_pEngine->GetPluginWindow();
    HMONITOR hMon = hRender ? MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST)
                            : MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
      outX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - m_nWndW) / 2;
      outY = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - m_nWndH) / 2;
    } else {
      outX = (GetSystemMetrics(SM_CXSCREEN) - m_nWndW) / 2;
      outY = (GetSystemMetrics(SM_CYSCREEN) - m_nWndH) / 2;
    }
  };

  // Where a tool window opens.
  //
  // Normally the remembered position wins -- windows stay where they were left,
  // which is the whole point of sticky positions.
  //
  // In TESTING MODE they open on the render window's monitor instead, and
  // SaveWindowPosition refuses to write, so a test run keeps its windows
  // together with the thing being measured and gives the layout back untouched
  // when it ends. A harness that had to drag windows around itself was the
  // reason they kept ending up on the wrong display and staying there.
  int posX, posY;
  if (!m_pEngine->m_bTestingMode && m_nPosX != INT_MIN && m_nPosY != INT_MIN) {
    posX = m_nPosX;
    posY = m_nPosY;
  } else {
    CenterOnRenderMonitor(posX, posY);
  }

  // If the render window is TOPMOST (fullscreen/borderless/spanning), create the
  // tool window TOPMOST too so it appears above the render surface.
  HWND hRender = m_pEngine->GetPluginWindow();
  bool renderIsTopmost = hRender &&
      (GetWindowLongW(hRender, GWL_EXSTYLE) & WS_EX_TOPMOST);

  DWORD exStyle = UsesToolWindowFrame() ? WS_EX_TOOLWINDOW : 0;
  if (m_bOnTop || renderIsTopmost) exStyle |= WS_EX_TOPMOST;

  m_hWnd = CreateWindowExW(
    exStyle,
    GetWindowClass(), GetWindowTitle(),
    // WS_CLIPCHILDREN: the frame stops painting the ground under its controls,
    // which is where resize flicker comes from -- the background is drawn, then
    // each control paints over it.
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN |
    GetExtraWindowStyle(),
    posX, posY, m_nWndW, m_nWndH,
    NULL, NULL, GetModuleHandle(NULL), (LPVOID)this);

  if (!m_hWnd) {
    CoUninitialize();
    m_bThreadRunning.store(false);
    return;
  }

  if (AcceptsDragDrop())
    DragAcceptFiles(m_hWnd, TRUE);

  DoBuildControls();
  m_bFirstBuild = false;
  ApplyDarkTheme();

  ShowWindow(m_hWnd, SW_SHOW);
  // Ensure we come to front even over topmost render window
  SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  if (!m_bOnTop && !renderIsTopmost)
    SetWindowPos(m_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  SetForegroundWindow(m_hWnd);
  UpdateWindow(m_hWnd);

  // Own message pump on this thread
  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    // --- Keyboard forwarding to render window ---
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
      UINT vk = (UINT)msg.wParam;
      bool bCtrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      bool bShift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
      bool bAlt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;

      // Escape always closes the tool window
      if (vk == VK_ESCAPE && !bCtrl && !bAlt) {
        PostMessage(m_hWnd, WM_CLOSE, 0, 0);
        continue;
      }

      // Ctrl+F2 resets tool window position/size to defaults
      if (vk == VK_F2 && bCtrl) {
        ResetPosition();
        continue;
      }

      {
        bool isFKey = (vk >= VK_F1 && vk <= VK_F24);
        // ForwardAllKeys(): forward everything (windows with no text edits)
        // Otherwise: only F-keys and Ctrl/Alt combos (bare alphanumerics go to edits)
        bool shouldForward = ForwardAllKeys() || isFKey || bCtrl || bAlt;

        // Don't forward standard edit shortcuts when an edit control has focus
        // (Ctrl+A=SelectAll, Ctrl+C=Copy, Ctrl+X=Cut, Ctrl+V=Paste, Ctrl+Z=Undo)
        if (shouldForward && bCtrl && !bAlt && !bShift) {
          if (vk == 'A' || vk == 'C' || vk == 'X' || vk == 'V' || vk == 'Z') {
            HWND hFocus = GetFocus();
            if (hFocus) {
              wchar_t cls[16];
              GetClassNameW(hFocus, cls, 16);
              // "Scintilla" is the Preset Editor's code control. Without it,
              // Ctrl+A/C/X/V/Z went to the render window and copy, paste and
              // undo simply did nothing in the editor.
              if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"RichEdit20W") == 0 ||
                  _wcsicmp(cls, L"Scintilla") == 0)
                shouldForward = false;
            }
          }
        }

        if (shouldForward) {
          HWND hRender = m_pEngine->GetPluginWindow();
          if (hRender) {
            PostMessage(hRender, msg.message, msg.wParam, msg.lParam);
            continue;
          }
        }
      }
    }
    if (!IsDialogMessage(m_hWnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  m_hWnd = NULL;
  CoUninitialize();
  m_bThreadRunning.store(false);
}

//----------------------------------------------------------------------
// Dark Theme
//----------------------------------------------------------------------

void ToolWindow::ApplyDarkTheme() {
  if (!m_hWnd) return;
  m_pEngine->LoadSettingsThemeFromINI();
  ApplyDarkThemeToWindow(m_pEngine, m_hWnd);
  ApplyDarkThemeToChildren(m_pEngine, m_childCtrls);
  RedrawWindow(m_hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

//----------------------------------------------------------------------
// ModalDialog — lightweight modal popup base class
//----------------------------------------------------------------------

bool ModalDialog::Show(HWND hParent, int clientW, int clientH) {
  m_hParent = hParent;

  // Register window class once
  WNDCLASSEXW wc = { sizeof(wc) };
  if (!GetClassInfoExW(GetModuleHandle(NULL), GetDialogClass(), &wc)) {
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ModalWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = m_pEngine->IsDarkTheme()
      ? CreateSolidBrush(m_pEngine->m_colSettingsBg)
      : (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = GetDialogClass();
    RegisterClassExW(&wc);
  }

  // Create font
  m_hFont = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  // Compute window rect from desired client area (DPI-aware)
  DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  DWORD dwExStyle = WS_EX_DLGMODALFRAME;
  UINT dpi = GetDpiForWindow(hParent);
  if (dpi == 0) dpi = 96;
  RECT rc = { 0, 0, clientW, clientH };
  AdjustWindowRectExForDpi(&rc, dwStyle, FALSE, dwExStyle, dpi);
  int wndW = rc.right - rc.left;
  int wndH = rc.bottom - rc.top;

  // Center on parent's monitor
  HMONITOR hMon = MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  GetMonitorInfo(hMon, &mi);
  int cx = (mi.rcWork.left + mi.rcWork.right - wndW) / 2;
  int cy = (mi.rcWork.top + mi.rcWork.bottom - wndH) / 2;

  m_hWnd = CreateWindowExW(dwExStyle, GetDialogClass(), GetDialogTitle(),
    dwStyle, cx, cy, wndW, wndH, hParent, NULL, GetModuleHandle(NULL), this);
  if (!m_hWnd) {
    if (m_hFont) { DeleteObject(m_hFont); m_hFont = NULL; }
    return false;
  }

  // Build controls
  DoBuildControls(clientW, clientH);

  // Apply dark theme
  m_pEngine->LoadSettingsThemeFromINI();
  ApplyDarkThemeToWindow(m_pEngine, m_hWnd);
  ApplyDarkThemeToChildren(m_pEngine, m_childCtrls);

  // Show and make modal
  ShowWindow(m_hWnd, SW_SHOW);
  RedrawWindow(m_hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
  EnableWindow(hParent, FALSE);

  // Local message loop
  MSG msg;
  while (!m_bDone && GetMessage(&msg, NULL, 0, 0)) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
      HWND hNext = GetNextDlgTabItem(m_hWnd, GetFocus(), GetKeyState(VK_SHIFT) < 0);
      if (hNext) SetFocus(hNext);
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
      m_bResult = false;
      m_bDone = true;
      break;
    }
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  // Cleanup
  EnableWindow(hParent, TRUE);
  SetForegroundWindow(hParent);
  DestroyWindow(m_hWnd);
  m_hWnd = NULL;
  m_childCtrls.clear();
  if (m_hFont) { DeleteObject(m_hFont); m_hFont = NULL; }

  return m_bResult;
}

bool ModalDialog::IsChecked(int id) const {
  HWND h = GetDlgItem(m_hWnd, id);
  return h ? (bool)(intptr_t)GetPropW(h, L"Checked") : false;
}

void ModalDialog::SetChecked(int id, bool checked) {
  HWND h = GetDlgItem(m_hWnd, id);
  if (h) {
    SetPropW(h, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
    InvalidateRect(h, NULL, TRUE);
  }
}

int ModalDialog::GetLineHeight() {
  if (!m_hFont) return 18;
  HDC hdc = GetDC(m_hWnd);
  HFONT hOld = (HFONT)SelectObject(hdc, m_hFont);
  TEXTMETRIC tm;
  GetTextMetrics(hdc, &tm);
  SelectObject(hdc, hOld);
  ReleaseDC(m_hWnd, hdc);
  int h = tm.tmHeight + tm.tmExternalLeading + 6;
  return max(h, 20); // match ToolWindow::GetLineHeight()
}

ModalDialog::BaseLayout ModalDialog::GetBaseLayout() {
  return { GetLineHeight(), 6, 16, 85 };
}

void ModalDialog::FitToContent(int clientW, int contentH) {
  HWND hDlg = m_hWnd;
  DWORD dwStyle = (DWORD)GetWindowLongPtrW(hDlg, GWL_STYLE);
  DWORD dwExStyle = (DWORD)GetWindowLongPtrW(hDlg, GWL_EXSTYLE);
  UINT dpi = GetDpiForWindow(hDlg);
  if (dpi == 0) dpi = 96;
  RECT rc = { 0, 0, clientW, contentH };
  AdjustWindowRectExForDpi(&rc, dwStyle, FALSE, dwExStyle, dpi);
  SetWindowPos(hDlg, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
    SWP_NOMOVE | SWP_NOZORDER);
}

LRESULT CALLBACK ModalDialog::ModalWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (uMsg == WM_NCCREATE) {
    CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
    if (pcs && pcs->lpCreateParams)
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pcs->lpCreateParams);
  }
  ModalDialog* dlg = (ModalDialog*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
  if (!dlg) return DefWindowProcW(hWnd, uMsg, wParam, lParam);

  Engine* p = dlg->m_pEngine;

  switch (uMsg) {
  case WM_CLOSE:
    dlg->EndDialog(false);
    return 0;

  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORDLG:
  {
    LRESULT lr = HandleDarkCtlColor(p, uMsg, wParam, lParam);
    if (lr) return lr;
    break;
  }

  case WM_DRAWITEM:
  {
    DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
    LRESULT lr = HandleDarkDrawItem(p, pDIS);
    if (lr) return lr;
    break;
  }

  case WM_ERASEBKGND:
    return HandleDarkEraseBkgnd(p, hWnd, (HDC)wParam);

  case WM_COMMAND:
  {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    // Auto-toggle owner-draw checkboxes/radios
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      if ((bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox")) {
        bool was = (bool)(intptr_t)GetPropW(hCtrl, L"Checked");
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(was ? 0 : 1));
        InvalidateRect(hCtrl, NULL, TRUE);
      }
      if ((bool)(intptr_t)GetPropW(hCtrl, L"IsRadio")) {
        int group = (int)(intptr_t)GetPropW(hCtrl, L"RadioGroup");
        if (group != 0) {
          for (HWND hChild : dlg->m_childCtrls) {
            if ((bool)(intptr_t)GetPropW(hChild, L"IsRadio") &&
                (int)(intptr_t)GetPropW(hChild, L"RadioGroup") == group) {
              SetPropW(hChild, L"Checked", (HANDLE)(intptr_t)(hChild == hCtrl ? 1 : 0));
              InvalidateRect(hChild, NULL, TRUE);
            }
          }
        }
      }
    }
    LRESULT r = dlg->DoCommand(id, code, lParam);
    if (r != -1) return r;
    break;
  }

  case WM_NOTIFY:
  {
    NMHDR* pnm = (NMHDR*)lParam;
    // ListView header dark theme custom draw
    if (p->IsDarkTheme() && pnm->code == NM_CUSTOMDRAW) {
      HWND hParent = GetParent(pnm->hwndFrom);
      if (hParent) {
        wchar_t szClass[32];
        GetClassNameW(hParent, szClass, 32);
        if (_wcsicmp(szClass, WC_LISTVIEWW) == 0) {
          bool handled = false;
          LRESULT result = PaintDarkListViewHeader(pnm, lParam, hParent,
            p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText, &handled);
          if (handled) return result;
        }
      }
    }
    LRESULT r = dlg->DoNotify(pnm);
    if (r != -1) return r;
    break;
  }

  case WM_SETTINGCHANGE:
    if (p->m_nThemeMode == Engine::THEME_SYSTEM && lParam &&
        _wcsicmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
      p->LoadSettingsThemeFromINI();
      ApplyDarkThemeToWindow(p, hWnd);
      ApplyDarkThemeToChildren(p, dlg->m_childCtrls);
      RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
    }
    break;

  default:
  {
    LRESULT r = dlg->DoMessage(uMsg, wParam, lParam);
    if (r != -1) return r;
    break;
  }
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

//----------------------------------------------------------------------
// Shared dark theme helpers (used by ToolWindow + ModalDialog + popups)
//----------------------------------------------------------------------

LRESULT HandleDarkCtlColor(Engine* p, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (!p->IsDarkTheme()) return 0;

  HDC hdc = (HDC)wParam;
  switch (msg) {
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    if (p->m_hBrSettingsCtrlBg) {
      SetTextColor(hdc, p->m_colSettingsText);
      SetBkColor(hdc, p->m_colSettingsCtrlBg);
      return (LRESULT)p->m_hBrSettingsCtrlBg;
    }
    break;

  case WM_CTLCOLORSTATIC:
    if (p->m_hBrSettingsBg) {
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
    if (p->m_hBrSettingsBg) {
      SetTextColor(hdc, p->m_colSettingsText);
      SetBkColor(hdc, p->m_colSettingsBg);
      return (LRESULT)p->m_hBrSettingsBg;
    }
    break;

  case WM_CTLCOLORDLG:
    if (p->m_hBrSettingsBg)
      return (LRESULT)p->m_hBrSettingsBg;
    break;
  }
  return 0;
}

LRESULT HandleDarkDrawItem(Engine* p, DRAWITEMSTRUCT* pDIS) {
  if (!pDIS) return FALSE;

  if (pDIS->CtlType == ODT_TAB) {
    bool bSelected = (pDIS->itemState & ODS_SELECTED) != 0;
    HDC hdc = pDIS->hDC;
    RECT rc = pDIS->rcItem;
    if (p->IsDarkTheme()) {
      COLORREF bg = bSelected ? p->m_colSettingsCtrlBg : p->m_colSettingsBtnFace;
      HBRUSH hBr = CreateSolidBrush(bg);
      FillRect(hdc, &rc, hBr);
      DeleteObject(hBr);
      if (bSelected) {
        HPEN hiPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnHi);
        HPEN shPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnShadow);
        HPEN oldPen = (HPEN)SelectObject(hdc, hiPen);
        MoveToEx(hdc, rc.left, rc.top, NULL);
        LineTo(hdc, rc.right - 1, rc.top);
        MoveToEx(hdc, rc.left, rc.top, NULL);
        LineTo(hdc, rc.left, rc.bottom);
        SelectObject(hdc, shPen);
        MoveToEx(hdc, rc.right - 1, rc.top, NULL);
        LineTo(hdc, rc.right - 1, rc.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(hiPen);
        DeleteObject(shPen);
      } else {
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
    DrawTextW(pDIS->hDC, szText, -1, &pDIS->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return TRUE;
  }

  if (pDIS->CtlType == ODT_BUTTON) {
    // Skip pin button — ToolWindow handles that itself
    if ((bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsPinBtn"))
      return FALSE;

    bool bIsCheckbox = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsCheckbox");
    bool bIsRadio = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsRadio");
    if (bIsCheckbox) {
      DrawOwnerCheckbox(pDIS, p->IsDarkTheme(),
        p->m_colSettingsBg, p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText);
    } else if (bIsRadio) {
      DrawOwnerRadio(pDIS, p->IsDarkTheme(),
        p->m_colSettingsBg, p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText);
    } else {
      // "AccentBtn" marks a button that needs to stand out -- currently the
      // Video Effects Save Profile button while there are unsaved changes.
      const bool bAccent = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"AccentBtn");
      DrawOwnerButton(pDIS, p->IsDarkTheme(),
        p->m_colSettingsBtnFace, p->m_colSettingsBtnHi, p->m_colSettingsBtnShadow,
        p->m_colSettingsText, bAccent);
    }
    return TRUE;
  }

  // Static swatch controls (SS_OWNERDRAW)
  if (pDIS->CtlType == ODT_STATIC) {
    COLORREF col = (COLORREF)(intptr_t)GetPropW(pDIS->hwndItem, L"SwatchColor");
    HDC hdc = pDIS->hDC;
    RECT rc = pDIS->rcItem;
    HBRUSH hBr = CreateSolidBrush(col);
    FillRect(hdc, &rc, hBr);
    DeleteObject(hBr);
    return TRUE;
  }

  return FALSE;
}

LRESULT HandleDarkEraseBkgnd(Engine* p, HWND hWnd, HDC hdc) {
  RECT rc;
  GetClientRect(hWnd, &rc);
  if (p->IsDarkTheme() && p->m_hBrSettingsBg)
    FillRect(hdc, &rc, p->m_hBrSettingsBg);
  else
    FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
  return 1;
}

void ApplyDarkThemeToWindow(Engine* p, HWND hWnd) {
  if (!hWnd) return;
  bool bDark = p->IsDarkTheme();
  BOOL bDarkDWM = bDark ? TRUE : FALSE;
  DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &bDarkDWM, sizeof(bDarkDWM));
  if (bDark) {
    DwmSetWindowAttribute(hWnd, 35, &p->m_colSettingsBg, sizeof(COLORREF));
    DwmSetWindowAttribute(hWnd, 34, &p->m_colSettingsBorder, sizeof(COLORREF));
    DwmSetWindowAttribute(hWnd, 36, &p->m_colSettingsText, sizeof(COLORREF));
  } else {
    COLORREF reset = 0xFFFFFFFF;
    DwmSetWindowAttribute(hWnd, 35, &reset, sizeof(reset));
    DwmSetWindowAttribute(hWnd, 34, &reset, sizeof(reset));
    DwmSetWindowAttribute(hWnd, 36, &reset, sizeof(reset));
  }

  // Enable dark popup menus via undocumented uxtheme APIs (Windows 10 1903+)
  ResolveDarkModeAPIs();
  if (pSetPreferredAppMode)
    pSetPreferredAppMode(bDark ? ForceDark : ForceLight);
  if (pAllowDarkModeForWindow)
    pAllowDarkModeForWindow(hWnd, bDark);
  if (pFlushMenuThemes)
    pFlushMenuThemes();
}

void ApplyDarkThemeToChildren(Engine* p, const std::vector<HWND>& ctrls) {
  bool bDark = p->IsDarkTheme();
  for (HWND hChild : ctrls) {
    if (!hChild || !IsWindow(hChild)) continue;
    wchar_t szClass[32];
    GetClassNameW(hChild, szClass, 32);
    if (_wcsicmp(szClass, WC_TABCONTROLW) == 0)
      SetWindowTheme(hChild, bDark ? L"" : NULL, bDark ? L"" : NULL);
    else if (_wcsicmp(szClass, HOTKEY_CLASSW) == 0)
      SetWindowTheme(hChild, bDark ? L"DarkMode_CFD" : NULL, NULL);
    else if (_wcsicmp(szClass, WC_LISTVIEWW) == 0) {
      SetWindowTheme(hChild, bDark ? L"DarkMode_Explorer" : NULL, NULL);
      if (bDark) {
        ListView_SetBkColor(hChild, p->m_colSettingsCtrlBg);
        ListView_SetTextBkColor(hChild, p->m_colSettingsCtrlBg);
        ListView_SetTextColor(hChild, p->m_colSettingsText);
      } else {
        ListView_SetBkColor(hChild, CLR_DEFAULT);
        ListView_SetTextBkColor(hChild, CLR_DEFAULT);
        ListView_SetTextColor(hChild, CLR_DEFAULT);
      }
    }
    else {
      // Strip visual styles — dark painting handled by WM_CTLCOLOR*,
      // WM_DRAWITEM, and WM_ERASEBKGND in the parent WndProc.
      // DarkMode_Explorer on EDIT controls overrides WM_CTLCOLOREDIT brush.
      SetWindowTheme(hChild, bDark ? L"" : NULL, bDark ? L"" : NULL);
    }
  }
}

//----------------------------------------------------------------------
// Dark ListView header helper (shared by ToolWindows + Resource Viewer)
//----------------------------------------------------------------------

LRESULT PaintDarkListViewHeader(NMHDR* pnm, LPARAM lParam, HWND hListView,
                                COLORREF colBg, COLORREF colBorder, COLORREF colText,
                                bool* pHandled)
{
  *pHandled = false;
  HWND hHeader = ListView_GetHeader(hListView);
  if (!hHeader || pnm->hwndFrom != hHeader) return 0;

  NMCUSTOMDRAW* pcd = (NMCUSTOMDRAW*)lParam;
  switch (pcd->dwDrawStage) {
  case CDDS_PREPAINT:
    *pHandled = true;
    return CDRF_NOTIFYITEMDRAW;
  case CDDS_ITEMPREPAINT: {
    HDC hdc = pcd->hdc;
    RECT rc = pcd->rc;
    HBRUSH hBr = CreateSolidBrush(colBg);
    FillRect(hdc, &rc, hBr);
    DeleteObject(hBr);
    HPEN hPen = CreatePen(PS_SOLID, 1, colBorder);
    HPEN hOld = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, rc.right - 1, rc.top, NULL);
    LineTo(hdc, rc.right - 1, rc.bottom);
    SelectObject(hdc, hOld);
    DeleteObject(hPen);
    wchar_t szText[128] = {};
    HDITEMW hdi = {};
    hdi.mask = HDI_TEXT;
    hdi.pszText = szText;
    hdi.cchTextMax = 128;
    Header_GetItem(hHeader, (int)pcd->dwItemSpec, &hdi);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, colText);
    HFONT hFont = (HFONT)SendMessage(hHeader, WM_GETFONT, 0, 0);
    HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
    rc.left += 6;
    DrawTextW(hdc, szText, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (hOldFont) SelectObject(hdc, hOldFont);
    *pHandled = true;
    return CDRF_SKIPDEFAULT;
  }
  }
  return 0;
}

//----------------------------------------------------------------------
// Themed ListView factory
//----------------------------------------------------------------------

HWND ToolWindow::CreateThemedListView(int id, int x, int y, int w, int h,
                                      bool visible, bool sortable)
{
  DWORD style = WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS;
  if (!sortable) style |= LVS_NOSORTHEADER;
  if (visible) style |= WS_VISIBLE | WS_TABSTOP;

  HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
    style, x, y, w, h, m_hWnd,
    (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hList) {
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (m_hFont)
      SendMessage(hList, WM_SETFONT, (WPARAM)m_hFont, TRUE);
  }
  return hList;
}

//----------------------------------------------------------------------
// Font helpers
//----------------------------------------------------------------------

int ToolWindow::GetLineHeight() {
  if (!m_hFont || !m_hWnd) return 26;
  HDC hdc = GetDC(m_hWnd);
  if (!hdc) return 26;
  HFONT hOld = (HFONT)SelectObject(hdc, m_hFont);
  TEXTMETRIC tm = {};
  GetTextMetrics(hdc, &tm);
  SelectObject(hdc, hOld);
  ReleaseDC(m_hWnd, hdc);
  int h = tm.tmHeight + tm.tmExternalLeading + 6;
  return max(h, 20);
}

ToolWindow::BaseLayout ToolWindow::BuildBaseControls() {
  HWND hw = m_hWnd;

  // Create fonts from shared font size
  if (m_hFont) DeleteObject(m_hFont);
  m_hFont = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  if (m_hFontBold) DeleteObject(m_hFontBold);
  m_hFontBold = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  int clientW = rcWnd.right;

  int lineH = GetLineHeight();
  int gap = 6, x = 16;
  int rw = clientW - x * 2;
  int y = 8;

  // Font +/- buttons (top-left)
  {
    int btnW = lineH;
    TrackControl(CreateBtn(hw, L"\u2212", GetFontMinusControlID(), x, y, btnW, lineH, m_hFont));
    TrackControl(CreateBtn(hw, L"+", GetFontPlusControlID(), x + btnW + 4, y, btnW, lineH, m_hFont));
  }

  // Pin button (top-right)
  {
    if (m_hPinFont) DeleteObject(m_hPinFont);
    int pinSize = lineH;
    m_hPinFont = CreateFontW(-pinSize + 4, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    int pinX = clientW - pinSize - x;
    HWND hPin = CreateWindowExW(0, L"BUTTON", L"\xE718",
      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
      pinX, y, pinSize, pinSize, hw,
      (HMENU)(INT_PTR)GetPinControlID(), GetModuleHandle(NULL), NULL);
    if (hPin) {
      if (m_hPinFont) SendMessage(hPin, WM_SETFONT, (WPARAM)m_hPinFont, TRUE);
      SetPropW(hPin, L"IsPinBtn", (HANDLE)(intptr_t)1);
      HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hw, NULL, GetModuleHandle(NULL), NULL);
      TrackTooltip(hTip);
      if (hTip) {
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        ti.hwnd = hw;
        ti.uId = (UINT_PTR)hPin;
        ti.lpszText = (LPWSTR)L"Always on top";
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
      }
    }
    TrackControl(hPin);
  }

  y += lineH + gap + 4;
  return { y, lineH, gap, x, rw, clientW };
}

// Re-anchor the base controls after a resize.
//
// The font +/- buttons are anchored to the left edge, so they are already right
// wherever the window goes. The PIN is anchored to the RIGHT edge, and nothing
// moved it: it was placed once at clientW - pinSize - x when the controls were
// built and then left there. Widen the window and it sat in the middle; narrow
// it and it went off the edge entirely and could not be clicked at all.
//
// This lives in the base class because the pin is a base control. Leaving it to
// each subclass's LayoutControls means twenty-odd windows each have to remember
// to move a control they never created -- and every one of them had forgotten.
void ToolWindow::LayoutBaseControls() {
  if (!m_hWnd) return;
  HWND hPin = GetDlgItem(m_hWnd, GetPinControlID());
  if (!hPin) return;

  RECT rc;
  GetClientRect(m_hWnd, &rc);

  // Must match BuildBaseControls: x = 16, y = 8, pinSize = lineH.
  const int lineH = GetLineHeight();
  const int x = 16, y = 8;
  MoveWindow(hPin, rc.right - lineH - x, y, lineH, lineH, TRUE);
}

//----------------------------------------------------------------------
// Tab control support
//----------------------------------------------------------------------

// Shared tab subclass for dark background — used by ToolWindow and ModalDialog
LRESULT CALLBACK DarkTabSubclassProc(
  HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
  UINT_PTR /*subclassId*/, DWORD_PTR refData)
{
  switch (msg) {
  case WM_ERASEBKGND: {
    Engine* p = (Engine*)refData;
    if (p && p->IsDarkTheme() && p->m_hBrSettingsBg) {
      HDC hdc = (HDC)wParam;
      RECT rc; GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, p->m_hBrSettingsBg);
      return 1;
    }
    break;
  }
  case WM_PAINT: {
    // Default tab WM_PAINT repaints the display area with system theme colors,
    // overriding our dark WM_ERASEBKGND. Let default paint, then repaint display area dark.
    Engine* p = (Engine*)refData;
    LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (p && p->IsDarkTheme() && p->m_hBrSettingsBg) {
      RECT rcDisplay;
      GetClientRect(hwnd, &rcDisplay);
      SendMessage(hwnd, TCM_ADJUSTRECT, FALSE, (LPARAM)&rcDisplay);
      HDC hdc = GetDC(hwnd);
      FillRect(hdc, &rcDisplay, p->m_hBrSettingsBg);
      ReleaseDC(hwnd, hdc);
    }
    return lr;
  }
  case WM_NCDESTROY:
    RemoveWindowSubclass(hwnd, DarkTabSubclassProc, 1);
    break;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ToolWindow::TabSubclassProc(
  HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
  UINT_PTR subclassId, DWORD_PTR refData)
{
  return DarkTabSubclassProc(hwnd, msg, wParam, lParam, subclassId, refData);
}

RECT ToolWindow::BuildTabControl(int tabCtrlID, const wchar_t* const* tabNames, int numPages,
                                  int x, int y, int w, int h)
{
  m_pageCtrls.clear();
  m_pageCtrls.resize(numPages);
  m_nActivePage = 0;

  m_hTab = CreateWindowExW(0, WC_TABCONTROLW, NULL,
    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED,
    x, y, w, h, m_hWnd, (HMENU)(INT_PTR)tabCtrlID,
    GetModuleHandle(NULL), NULL);
  SendMessage(m_hTab, WM_SETFONT, (WPARAM)m_hFont, TRUE);
  SetWindowSubclass(m_hTab, TabSubclassProc, 1, (DWORD_PTR)m_pEngine);
  TrackControl(m_hTab);

  for (int i = 0; i < numPages; i++) {
    TCITEMW ti = {};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)tabNames[i];
    SendMessageW(m_hTab, TCM_INSERTITEMW, i, (LPARAM)&ti);
  }

  RECT rcContent = { x, y, x + w, y + h };
  TabCtrl_AdjustRect(m_hTab, FALSE, &rcContent);
  return rcContent;
}

void ToolWindow::ShowPage(int page) {
  int numPages = (int)m_pageCtrls.size();
  if (page < 0 || page >= numPages) return;
  for (int i = 0; i < numPages; i++) {
    if (i == page) {
      for (HWND h : m_pageCtrls[i])
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    } else {
      for (HWND h : m_pageCtrls[i])
        ShowWindow(h, SW_HIDE);
    }
  }
  m_nActivePage = page;
  wchar_t buf[8]; swprintf(buf, 8, L"%d", page);
  Config().SetString(GetINISection(), L"ActiveTab", buf);
}

void ToolWindow::SelectInitialTab() {
  if (!m_hTab || m_pageCtrls.empty()) return;
  int numPages = (int)m_pageCtrls.size();
  int tab = Config().GetInt(GetINISection(), L"ActiveTab", 0);
  if (tab < 0 || tab >= numPages) tab = 0;
  TabCtrl_SetCurSel(m_hTab, tab);
  ShowPage(tab);
}

void ToolWindow::TrackPageControl(int page, HWND h) {
  if (!h) return;
  if (page >= 0 && page < (int)m_pageCtrls.size())
    m_pageCtrls[page].push_back(h);
  TrackControl(h);
}

bool ToolWindow::IsChecked(int controlID) const {
  HWND hCtrl = GetDlgItem(m_hWnd, controlID);
  return hCtrl ? (bool)(intptr_t)GetPropW(hCtrl, L"Checked") : false;
}

void ToolWindow::SetChecked(int controlID, bool checked) {
  HWND hCtrl = GetDlgItem(m_hWnd, controlID);
  if (hCtrl) {
    SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
    InvalidateRect(hCtrl, NULL, TRUE);
  }
}

void ToolWindow::ResetPosition() {
  if (!m_hWnd || !IsWindow(m_hWnd)) return;
  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  int posX = (screenW - m_nDefaultW) / 2;
  int posY = (screenH - m_nDefaultH) / 2;
  m_nWndW = m_nDefaultW;
  m_nWndH = m_nDefaultH;
  m_bOnTop = false;
  SetWindowPos(m_hWnd, HWND_NOTOPMOST, posX, posY, m_nDefaultW, m_nDefaultH, SWP_SHOWWINDOW);
  RebuildFonts();
}

// ---------------------------------------------------------------------------
// Keeping the user's work across a rebuild
// ---------------------------------------------------------------------------

std::vector<ToolWindow::ControlState> ToolWindow::CaptureControlState() const {
  std::vector<ControlState> saved;
  if (!m_hWnd) return saved;

  const HWND hFocus = GetFocus();
  for (HWND h = GetWindow(m_hWnd, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT)) {
    const int id = GetDlgCtrlID(h);
    if (id <= 0) continue;

    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);

    ControlState s;
    s.id = id;
    s.cls = cls;
    s.hadFocus = (h == hFocus);

    if (_wcsicmp(cls, L"Edit") == 0) {
      const int len = GetWindowTextLengthW(h);
      if (len > 0) {
        s.text.resize((size_t)len + 1);
        GetWindowTextW(h, &s.text[0], len + 1);
        s.text.resize((size_t)len);
        s.hasText = true;
      }
      // EM_GETMODIFY is the whole reason this is safe to do generically: it is
      // true only if the USER changed the text since it was last set
      // programmatically. An edit that DoBuildControls fills from live state
      // comes back false and is left alone, so a deliberate refresh is never
      // overwritten by a stale value.
      s.userModified = SendMessageW(h, EM_GETMODIFY, 0, 0) != 0;
      const DWORD sel = (DWORD)SendMessageW(h, EM_GETSEL, 0, 0);
      s.selStart = (int)LOWORD(sel);
      s.selEnd = (int)HIWORD(sel);
      s.topIndex = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    } else if (_wcsicmp(cls, L"ListBox") == 0) {
      s.selection = (int)SendMessageW(h, LB_GETCURSEL, 0, 0);
      s.topIndex = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0);
    } else if (_wcsicmp(cls, L"ComboBox") == 0) {
      s.selection = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    } else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
      s.selection = ListView_GetNextItem(h, -1, LVNI_SELECTED);
      s.topIndex = ListView_GetTopIndex(h);
    } else {
      // Buttons and labels carry no state a rebuild can lose: owner-draw check
      // and radio state is re-derived from the engine by DoBuildControls, which
      // is where it belongs.
      if (!s.hadFocus) continue;
    }
    saved.push_back(std::move(s));
  }
  return saved;
}

// The control this state belongs to, or NULL.
//
// A matching ID is not enough: a window is free to use one ID for a different
// control on a different page, and putting an edit's text into a list would be
// worse than losing it.
HWND ToolWindow::MatchingControl(const ControlState& s) const {
  HWND h = GetDlgItem(m_hWnd, s.id);
  if (!h) return NULL;
  wchar_t cls[64] = {};
  GetClassNameW(h, cls, 64);
  return _wcsicmp(cls, s.cls.c_str()) == 0 ? h : NULL;
}

// Step 1: where each list was, before the window gets a say.
void ToolWindow::RestoreControlSelections(const std::vector<ControlState>& saved) {
  if (!m_hWnd) return;
  for (const ControlState& s : saved) {
    HWND h = MatchingControl(s);
    if (!h) continue;
    const wchar_t* cls = s.cls.c_str();

    if (_wcsicmp(cls, L"ListBox") == 0) {
      if (s.selection >= 0 && s.selection < (int)SendMessageW(h, LB_GETCOUNT, 0, 0))
        SendMessageW(h, LB_SETCURSEL, (WPARAM)s.selection, 0);
      if (s.topIndex > 0) SendMessageW(h, LB_SETTOPINDEX, (WPARAM)s.topIndex, 0);
    } else if (_wcsicmp(cls, L"ComboBox") == 0) {
      if (s.selection >= 0 && s.selection < (int)SendMessageW(h, CB_GETCOUNT, 0, 0))
        SendMessageW(h, CB_SETCURSEL, (WPARAM)s.selection, 0);
    } else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
      const int count = ListView_GetItemCount(h);
      if (s.selection >= 0 && s.selection < count)
        ListView_SetItemState(h, s.selection, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
      // ListView has no SETTOPINDEX, so scrolling back means making the row
      // that used to be at the top visible again, then the selected row.
      if (s.topIndex > 0 && s.topIndex < count)
        ListView_EnsureVisible(h, s.topIndex, FALSE);
      if (s.selection >= 0 && s.selection < count)
        ListView_EnsureVisible(h, s.selection, TRUE);
    }
  }
}

// Step 3: what the user typed, and the caret they left in it.
void ToolWindow::RestoreControlText(const std::vector<ControlState>& saved) {
  if (!m_hWnd) return;
  HWND hFocusTarget = NULL;
  for (const ControlState& s : saved) {
    HWND h = MatchingControl(s);
    if (!h) continue;
    if (s.hadFocus) hFocusTarget = h;
    if (_wcsicmp(s.cls.c_str(), L"Edit") != 0) continue;

    // Restore what the USER typed, and also restore a pane the rebuild left
    // blank -- the shader import window creates its error box empty and fills
    // it from twenty other places, so a resize used to wipe the compile error
    // you were reading. What is never restored is an edit the window itself
    // refreshed from live state, which is what EM_GETMODIFY tells us apart.
    const bool cameBackEmpty = GetWindowTextLengthW(h) == 0;
    if (s.hasText && (s.userModified || cameBackEmpty)) {
      SetWindowTextW(h, s.text.c_str());
      SendMessageW(h, EM_SETSEL, (WPARAM)s.selStart, (LPARAM)s.selEnd);
      if (s.topIndex > 0) SendMessageW(h, EM_LINESCROLL, 0, (LPARAM)s.topIndex);
      // SetWindowText clears the modify flag; put it back, or the next rebuild
      // would take this for a programmatic value and drop it.
      if (s.userModified) SendMessageW(h, EM_SETMODIFY, TRUE, 0);
    }
  }
  if (hFocusTarget) SetFocus(hFocusTarget);
}

// ---------------------------------------------------------------------------
// Anchored layout
// ---------------------------------------------------------------------------

void ToolWindow::AnchorControl(HWND h, unsigned edges) {
  if (!h || !m_hWnd || !IsWindow(h)) return;

  RECT rc;
  GetWindowRect(h, &rc);
  MapWindowPoints(NULL, m_hWnd, (POINT*)&rc, 2);   // screen -> client

  RECT client;
  GetClientRect(m_hWnd, &client);
  m_anchorClient.cx = client.right;
  m_anchorClient.cy = client.bottom;

  AnchoredControl a;
  a.hwnd = h;
  a.edges = edges;
  a.rect = rc;
  m_anchors.push_back(a);
}

void ToolWindow::ApplyAnchors() {
  if (m_anchors.empty() || !m_hWnd) return;

  RECT client;
  GetClientRect(m_hWnd, &client);
  const int dx = client.right - m_anchorClient.cx;
  const int dy = client.bottom - m_anchorClient.cy;
  if (dx == 0 && dy == 0) return;

  // One batch, so the window redraws once instead of once per control. The
  // rebuild path this replaces did a full teardown per mouse-move.
  HDWP dwp = BeginDeferWindowPos((int)m_anchors.size());

  for (const AnchoredControl& a : m_anchors) {
    if (!IsWindow(a.hwnd)) continue;

    RECT r = a.rect;
    const bool left   = (a.edges & kAnchorLeft) != 0;
    const bool right  = (a.edges & kAnchorRight) != 0;
    const bool top    = (a.edges & kAnchorTop) != 0;
    const bool bottom = (a.edges & kAnchorBottom) != 0;

    // Right edge follows the window; if the left edge does not, the whole
    // control slides instead of stretching. Same for the vertical axis.
    if (right) {
      r.right += dx;
      if (!left) r.left += dx;
    }
    if (bottom) {
      r.bottom += dy;
      if (!top) r.top += dy;
    }
    // Anchored to neither side of an axis: stay centred on that axis.
    if (!left && !right) { r.left += dx / 2; r.right += dx / 2; }
    if (!top && !bottom) { r.top += dy / 2; r.bottom += dy / 2; }

    const int w = max(0L, r.right - r.left);
    const int h = max(0L, r.bottom - r.top);

    if (dwp)
      dwp = DeferWindowPos(dwp, a.hwnd, NULL, r.left, r.top, w, h,
                           SWP_NOZORDER | SWP_NOACTIVATE);
    else
      MoveWindow(a.hwnd, r.left, r.top, w, h, TRUE);
  }

  if (dwp) EndDeferWindowPos(dwp);
  InvalidateRect(m_hWnd, NULL, TRUE);
}

void ToolWindow::RebuildFonts() {
  if (!m_hWnd) return;

  // Every control is destroyed and re-created below, and each new one paints
  // itself as it appears -- unthemed first, then again once ApplyDarkTheme has
  // run. On a window that is already on screen (a resize, a font-size change)
  // that reads as the window redrawing itself piece by piece. Holding painting
  // off until the new controls are in place and themed makes it a single swap.
  const bool bVisible = IsWindowVisible(m_hWnd) != FALSE;
  if (bVisible) SendMessageW(m_hWnd, WM_SETREDRAW, FALSE, 0);

  // Save active tab before destroying controls
  int savedTab = m_hTab ? TabCtrl_GetCurSel(m_hTab) : 0;

  // ...and everything else the user was in the middle of.
  const std::vector<ControlState> savedState = CaptureControlState();

  // Destroy all child windows
  HWND hChild = GetWindow(m_hWnd, GW_CHILD);
  while (hChild) {
    HWND hNext = GetWindow(hChild, GW_HWNDNEXT);
    DestroyWindow(hChild);
    hChild = hNext;
  }
  // Tooltips are owned popups rather than children, so the loop above walks
  // straight past them. Left alone they accumulated one leaked window per
  // tooltip per rebuild, on every window, for the life of the process.
  for (HWND hTip : m_tooltips)
    if (IsWindow(hTip)) DestroyWindow(hTip);
  m_tooltips.clear();

  m_childCtrls.clear();
  m_pageCtrls.clear();
  m_anchors.clear();      // the HWNDs they name are gone
  m_hTab = NULL;

  DoBuildControls();
  m_bFirstBuild = false;
  ApplyDarkTheme();

  // Restore tab selection (overrides the default from SelectInitialTab)
  if (m_hTab && savedTab > 0 && savedTab < (int)m_pageCtrls.size()) {
    TabCtrl_SetCurSel(m_hTab, savedTab);
    ShowPage(savedTab);
  }

  // Put the user's work back. Selections first, then the window's own
  // reconciliation, then the typing -- see RestoreControlSelections.
  RestoreControlSelections(savedState);
  OnRebuilt();
  RestoreControlText(savedState);

  if (bVisible) {
    SendMessageW(m_hWnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(m_hWnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
  }
}

//----------------------------------------------------------------------
// Base WndProc — handles common messages, delegates to subclass virtuals
//----------------------------------------------------------------------

LRESULT CALLBACK ToolWindow::BaseWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  // Store 'this' pointer on creation
  if (uMsg == WM_NCCREATE) {
    CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
    if (pcs && pcs->lpCreateParams)
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pcs->lpCreateParams);
  }
  ToolWindow* tw = (ToolWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
  if (!tw) return DefWindowProcW(hWnd, uMsg, wParam, lParam);

  Engine* p = tw->m_pEngine;

  switch (uMsg) {
  case WM_CLOSE:
    DestroyWindow(hWnd);
    return 0;

  case WM_DESTROY:
    {
      tw->SaveWindowPosition();

      // Let subclass clean up
      tw->DoDestroy();

      // Clean up base resources
      tw->m_hWnd = NULL;
      tw->m_hTab = NULL;
      tw->m_pageCtrls.clear();
      tw->m_childCtrls.clear();
      if (tw->m_hFont) { DeleteObject(tw->m_hFont); tw->m_hFont = NULL; }
      if (tw->m_hFontBold) { DeleteObject(tw->m_hFontBold); tw->m_hFontBold = NULL; }
      if (tw->m_hPinFont) { DeleteObject(tw->m_hPinFont); tw->m_hPinFont = NULL; }
    }
    PostQuitMessage(0);
    return 0;

  case WM_EXITSIZEMOVE:
    // Save where the user just put it.
    //
    // WM_DESTROY also saves, but that only fires when this window is closed,
    // and nothing closes tool windows when the program exits -- so a window
    // left open when MDropDX12 quit never remembered its position, and neither
    // did one open when the program was killed or crashed. Writing on drag-end
    // costs one INI write per drag and survives all three.
    tw->SaveWindowPosition();
    return 0;

  case WM_SIZE:
    if (wParam != SIZE_MINIMIZED) {
      RECT rc;
      GetWindowRect(hWnd, &rc);
      const int w = rc.right - rc.left;
      const int h = rc.bottom - rc.top;
      // ShowWindow(SW_SHOW) sends a WM_SIZE carrying the size the window was
      // created at. Acting on it tore down the controls DoBuildControls had
      // just made and built them all over again -- this time on a window that
      // was now on screen, so opening the window looked like it was drawing
      // itself in pieces, one tab page after another. Only a real size change
      // needs a relayout.
      // Maximise/restore must always relayout even when the restored size is
      // unchanged, so the early-out has to know which state we were in.
      const bool bMax = (wParam == SIZE_MAXIMIZED);
      if (!bMax && bMax == tw->m_bWasMaximized &&
          w == tw->m_nWndW && h == tw->m_nWndH) return 0;
      tw->m_bWasMaximized = bMax;
      // Only the restored size is worth remembering.
      if (!bMax) {
        tw->m_nWndW = w;
        tw->m_nWndH = h;
      }
      // Before the subclass: the pin is a base control, and every subclass
      // would otherwise have to remember to move something it never created.
      tw->LayoutBaseControls();
      tw->OnResize();
    }
    return 0;

  case WM_GETMINMAXINFO:
  {
    MINMAXINFO* mmi = (MINMAXINFO*)lParam;
    mmi->ptMinTrackSize.x = tw->GetMinWidth();
    mmi->ptMinTrackSize.y = tw->GetMinHeight();
    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
      mmi->ptMaxTrackSize.x = mi.rcWork.right - mi.rcWork.left;
      mmi->ptMaxTrackSize.y = mi.rcWork.bottom - mi.rcWork.top;
    }
    return 0;
  }

  case WM_MW_REBUILD_FONTS:
    tw->RebuildFonts();
    return 0;

  case WM_MW_BRING_TO_TOP:
  {
    if (IsIconic(hWnd))
      ShowWindow(hWnd, SW_RESTORE);
    // Check if the render window is TOPMOST (fullscreen/borderless mode).
    // If so, keep the tool window TOPMOST too, otherwise it stays behind.
    HWND hRender = tw->m_pEngine->GetPluginWindow();
    bool renderIsTopmost = hRender &&
        (GetWindowLongW(hRender, GWL_EXSTYLE) & WS_EX_TOPMOST);
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (!tw->m_bOnTop && !renderIsTopmost)
      SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // Attach input to foreground thread so SetForegroundWindow succeeds
    // from this non-foreground ToolWindow thread
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD myThread = GetCurrentThreadId();
    if (fgThread != myThread)
      AttachThreadInput(myThread, fgThread, TRUE);
    SetForegroundWindow(hWnd);
    if (fgThread != myThread)
      AttachThreadInput(myThread, fgThread, FALSE);
    return 0;
  }

  case WM_MW_RESET_WINDOW:
    tw->ResetPosition();
    return 0;

  // ── Sliders ──
  case WM_HSCROLL:
  {
    HWND hTrack = (HWND)lParam;
    int id = GetDlgCtrlID(hTrack);
    int pos = (int)SendMessage(hTrack, TBM_GETPOS, 0, 0);
    LRESULT r = tw->DoHScroll(hWnd, id, pos);
    if (r != -1) return r;
    break;
  }

  // ── Notifications ──
  case WM_NOTIFY:
  {
    NMHDR* pnm = (NMHDR*)lParam;
    // Tab selection change (handled by base for all tabbed windows)
    if (tw->m_hTab && pnm->hwndFrom == tw->m_hTab && pnm->code == TCN_SELCHANGE) {
      tw->ShowPage(TabCtrl_GetCurSel(pnm->hwndFrom));
      return 0;
    }
    // ListView header dark theme custom draw (centralized for all ToolWindow ListViews)
    if (p->IsDarkTheme() && pnm->code == NM_CUSTOMDRAW) {
      HWND hParent = GetParent(pnm->hwndFrom);
      if (hParent) {
        wchar_t szClass[32];
        GetClassNameW(hParent, szClass, 32);
        if (_wcsicmp(szClass, WC_LISTVIEWW) == 0) {
          bool handled = false;
          LRESULT result = PaintDarkListViewHeader(pnm, lParam, hParent,
            p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText, &handled);
          if (handled) return result;
        }
      }
    }
    LRESULT r = tw->DoNotify(hWnd, pnm);
    if (r != -1) return r;
    break;
  }

  // ── Commands ──
  case WM_COMMAND:
  {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    // Pin button (common)
    if (id == tw->GetPinControlID() && code == BN_CLICKED) {
      tw->m_bOnTop = !tw->m_bOnTop;
      SetWindowPos(hWnd, tw->m_bOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      InvalidateRect((HWND)lParam, NULL, TRUE);
      return 0;
    }

    // Font + (common)
    if (id == tw->GetFontPlusControlID() && code == BN_CLICKED) {
      if (p->m_nSettingsFontSize > -32) {
        p->m_nSettingsFontSize -= 2;
        tw->RebuildFonts();
        p->BroadcastFontSync(hWnd);
      }
      return 0;
    }

    // Font - (common)
    if (id == tw->GetFontMinusControlID() && code == BN_CLICKED) {
      if (p->m_nSettingsFontSize < -12) {
        p->m_nSettingsFontSize += 2;
        tw->RebuildFonts();
        p->BroadcastFontSync(hWnd);
      }
      return 0;
    }

    // Owner-draw BN_CLICKED: auto-toggle checkbox and radio state.
    // Checkboxes and radio groups are toggled here so subclasses don't need to.
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      bool bIsCheckbox = (bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox");
      if (bIsCheckbox) {
        bool wasChecked = (bool)(intptr_t)GetPropW(hCtrl, L"Checked");
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(wasChecked ? 0 : 1));
        InvalidateRect(hCtrl, NULL, TRUE);
      }

      bool bIsRadio = (bool)(intptr_t)GetPropW(hCtrl, L"IsRadio");
      if (bIsRadio) {
        int group = (int)(intptr_t)GetPropW(hCtrl, L"RadioGroup");
        if (group != 0) {
          for (HWND hChild : tw->m_childCtrls) {
            if ((bool)(intptr_t)GetPropW(hChild, L"IsRadio") &&
                (int)(intptr_t)GetPropW(hChild, L"RadioGroup") == group) {
              SetPropW(hChild, L"Checked", (HANDLE)(intptr_t)(hChild == hCtrl ? 1 : 0));
              InvalidateRect(hChild, NULL, TRUE);
            }
          }
        }
      }
    }

    // Delegate to subclass
    LRESULT r = tw->DoCommand(hWnd, id, code, lParam);
    if (r != -1) return r;
    break;
  }

  // ── Dark theme painting (delegated to shared helpers) ──
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORDLG:
  {
    LRESULT lr = HandleDarkCtlColor(p, uMsg, wParam, lParam);
    if (lr) return lr;
    break;
  }

  case WM_DRAWITEM:
  {
    DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
    // Pin button — ToolWindow-specific (accesses tw->m_bOnTop, tw->m_hPinFont)
    if (pDIS && pDIS->CtlType == ODT_BUTTON && (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsPinBtn")) {
      HDC hdc = pDIS->hDC;
      RECT rc = pDIS->rcItem;
      bool pressed = (pDIS->itemState & ODS_SELECTED) != 0;
      bool pinned = tw->m_bOnTop;
      COLORREF bg = p->IsDarkTheme() ? p->m_colSettingsBg : GetSysColor(COLOR_BTNFACE);
      HBRUSH hBr = CreateSolidBrush(bg);
      FillRect(hdc, &rc, hBr);
      DeleteObject(hBr);
      SetBkMode(hdc, TRANSPARENT);
      COLORREF pinCol = pinned
        ? (p->IsDarkTheme() ? RGB(100, 180, 255) : RGB(0, 100, 200))
        : (p->IsDarkTheme() ? RGB(120, 120, 120) : RGB(160, 160, 160));
      SetTextColor(hdc, pinCol);
      HFONT hOld = tw->m_hPinFont ? (HFONT)SelectObject(hdc, tw->m_hPinFont) : NULL;
      RECT textRc = rc;
      if (pressed) OffsetRect(&textRc, 1, 1);
      DrawTextW(hdc, L"\xE718", 1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (hOld) SelectObject(hdc, hOld);
      return TRUE;
    }
    // Everything else: tabs, checkboxes, radios, buttons, swatches
    LRESULT lr = HandleDarkDrawItem(p, pDIS);
    if (lr) return lr;
    break;
  }

  case WM_ERASEBKGND:
    return HandleDarkEraseBkgnd(p, hWnd, (HDC)wParam);

  case WM_SETTINGCHANGE:
    if (p->m_nThemeMode == Engine::THEME_SYSTEM && lParam &&
        _wcsicmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
      p->LoadSettingsThemeFromINI();
      tw->ApplyDarkTheme();
    }
    break;

  case WM_CONTEXTMENU:
  {
    LRESULT r = tw->DoContextMenu(hWnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    if (r != -1) return r;
    break;
  }

  default:
  {
    LRESULT r = tw->DoMessage(hWnd, uMsg, wParam, lParam);
    if (r != -1) return r;
    break;
  }
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

//----------------------------------------------------------------------
// Shared Action Edit Dialog — used by both Button Board and Hotkeys windows.
// Subclass of ModalDialog; dark theme handled automatically by the base class.
//----------------------------------------------------------------------

class ActionEditDialog : public ModalDialog {
  ActionEditData& m_data;

  const wchar_t* GetDialogTitle() const override {
    return m_data.isBuiltInHotkey ? L"Edit Hotkey" : L"Edit Action";
  }
  const wchar_t* GetDialogClass() const override {
    return L"MDropActionEditDlg";
  }

  static bool IsMouseVK(UINT v) {
    return v == VK_LBUTTON || v == VK_RBUTTON || v == VK_MBUTTON ||
           v == VK_XBUTTON1 || v == VK_XBUTTON2;
  }
  static int MouseVKToIdx(UINT v) {
    if (v == VK_LBUTTON) return 1; if (v == VK_RBUTTON) return 2;
    if (v == VK_MBUTTON) return 3; if (v == VK_XBUTTON1) return 4;
    if (v == VK_XBUTTON2) return 5; return 0;
  }
  static void PopulateMouseCombo(HWND hCombo) {
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Left Mouse");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Right Mouse");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Middle Mouse");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"X1 Mouse");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"X2 Mouse");
  }
  static void SetHotkeyCtrl(HWND hHK, UINT mod, UINT vk) {
    UINT hkMod = 0;
    if (mod & MOD_ALT)     hkMod |= HOTKEYF_ALT;
    if (mod & MOD_CONTROL) hkMod |= HOTKEYF_CONTROL;
    if (mod & MOD_SHIFT)   hkMod |= HOTKEYF_SHIFT;
    SendMessageW(hHK, HKM_SETHOTKEY, MAKEWORD(vk, hkMod), 0);
  }

  void DoBuildControls(int clientW, int clientH) override {
    HFONT hFont = GetFont();
    HWND hDlg = GetHWND();
    auto L = GetBaseLayout();

    int margin = L.margin;
    int cw = clientW - 2 * margin;
    int y = margin;
    int labelW = L.labelW;
    int lineH = L.lineH;
    int gap = L.gap;
    int browseW = 70;

    if (m_data.isBuiltInHotkey) {
      // Built-in hotkey: read-only action name
      TrackControl(CreateLabel(hDlg, L"Action:", margin, y + 2, labelW, lineH, hFont));
      HWND hAction = CreateEdit(hDlg, m_data.actionName.c_str(), IDC_AE_ACTION_LABEL,
        margin + labelW, y, cw - labelW, lineH, hFont, ES_READONLY);
      TrackControl(hAction);
      y += lineH + gap;
    } else {
      // Action type dropdown
      TrackControl(CreateLabel(hDlg, L"Action:", margin, y + 2, labelW, lineH, hFont));
      HWND hType = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        margin + labelW, y, cw - labelW, 200, hDlg,
        (HMENU)(INT_PTR)IDC_AE_ACTION_TYPE, GetModuleHandle(NULL), NULL);
      SendMessageW(hType, WM_SETFONT, (WPARAM)hFont, TRUE);
      TrackControl(hType);
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"None");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Load Preset");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Push Sprite");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Script Command");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Launch Message");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Run Script File");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Launch App");
      SendMessageW(hType, CB_SETCURSEL, (int)m_data.actionType, 0);
      y += lineH + gap;

      // Label
      TrackControl(CreateLabel(hDlg, L"Label:", margin, y + 2, labelW, lineH, hFont));
      TrackControl(CreateEdit(hDlg, m_data.label.c_str(), IDC_AE_LABEL,
        margin + labelW, y, cw - labelW, lineH, hFont));
      y += lineH + gap;

      // Payload (multiline) + Browse
      TrackControl(CreateLabel(hDlg, L"Command:", margin, y + 2, labelW, lineH, hFont));

      // Convert pipes to newlines for display (script commands)
      std::wstring displayPayload = m_data.payload;
      if (m_data.actionType == ButtonAction::ScriptCommand ||
          m_data.actionType == ButtonAction::LaunchMessage) {
        for (size_t pos = 0; (pos = displayPayload.find(L'|', pos)) != std::wstring::npos; pos += 2)
          displayPayload.replace(pos, 1, L"\r\n");
      }

      int cmdH = lineH * 4;
      TrackControl(CreateEdit(hDlg, displayPayload.c_str(), IDC_AE_PAYLOAD,
        margin + labelW, y, cw - labelW - browseW - 6, cmdH, hFont,
        ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL));
      TrackControl(CreateBtn(hDlg, L"Browse...", IDC_AE_BROWSE,
        margin + cw - browseW, y, browseW, lineH, hFont));
      y += cmdH + gap;
    }

    // Key binding section
    if (m_data.showKeyBinding) {
      // Ensure HOTKEY_CLASS is available
      INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_HOTKEY_CLASS };
      InitCommonControlsEx(&icc);

      int clearW = 50;
      HINSTANCE hInst = GetModuleHandle(NULL);

      if (m_data.isBuiltInHotkey) {
        // ── Built-in hotkey: dual binding (Local + Global) ──
        y += gap;

        // Local key row
        TrackControl(CreateLabel(hDlg, L"Local Key:", margin, y + 2, labelW, lineH, hFont));
        HWND hHK = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
          margin + labelW, y, cw - labelW - clearW - 6, lineH, hDlg,
          (HMENU)(INT_PTR)IDC_AE_HOTKEY, hInst, NULL);
        SendMessageW(hHK, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hHK);
        TrackControl(CreateBtn(hDlg, L"Clear", IDC_AE_CLEAR_KEY,
          margin + cw - clearW, y, clearW, lineH, hFont));
        y += lineH + gap;

        if (hHK && m_data.vk != 0 && !IsMouseVK(m_data.vk))
          SetHotkeyCtrl(hHK, m_data.modifiers, m_data.vk);

        // Local mouse dropdown
        TrackControl(CreateLabel(hDlg, L"Mouse:", margin, y + 2, labelW, lineH, hFont));
        HWND hMouse = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
          margin + labelW, y, cw - labelW - clearW - 6, 200, hDlg,
          (HMENU)(INT_PTR)IDC_AE_MOUSE, hInst, NULL);
        SendMessageW(hMouse, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hMouse);
        PopulateMouseCombo(hMouse);
        SendMessageW(hMouse, CB_SETCURSEL, MouseVKToIdx(m_data.vk), 0);
        y += lineH + gap + gap;

        // Global key row
        TrackControl(CreateLabel(hDlg, L"Global Key:", margin, y + 2, labelW, lineH, hFont));
        HWND hGHK = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
          margin + labelW, y, cw - labelW - clearW - 6, lineH, hDlg,
          (HMENU)(INT_PTR)IDC_AE_GLOBAL_HOTKEY, hInst, NULL);
        SendMessageW(hGHK, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hGHK);
        TrackControl(CreateBtn(hDlg, L"Clear", IDC_AE_GLOBAL_CLEAR,
          margin + cw - clearW, y, clearW, lineH, hFont));
        y += lineH + gap;

        if (hGHK && m_data.globalVK != 0 && !IsMouseVK(m_data.globalVK))
          SetHotkeyCtrl(hGHK, m_data.globalMod, m_data.globalVK);

        // Global mouse dropdown
        TrackControl(CreateLabel(hDlg, L"Mouse:", margin, y + 2, labelW, lineH, hFont));
        HWND hGMouse = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
          margin + labelW, y, cw - labelW - clearW - 6, 200, hDlg,
          (HMENU)(INT_PTR)IDC_AE_GLOBAL_MOUSE, hInst, NULL);
        SendMessageW(hGMouse, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hGMouse);
        PopulateMouseCombo(hGMouse);
        SendMessageW(hGMouse, CB_SETCURSEL, MouseVKToIdx(m_data.globalVK), 0);
        y += lineH + gap;

      } else {
        // ── User hotkey: single binding with scope checkbox ──
        y += gap;
        TrackControl(CreateLabel(hDlg, L"Key:", margin, y + 2, labelW, lineH, hFont));
        HWND hHK = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
          margin + labelW, y, cw - labelW - clearW - 6, lineH, hDlg,
          (HMENU)(INT_PTR)IDC_AE_HOTKEY, hInst, NULL);
        SendMessageW(hHK, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hHK);
        TrackControl(CreateBtn(hDlg, L"Clear", IDC_AE_CLEAR_KEY,
          margin + cw - clearW, y, clearW, lineH, hFont));
        y += lineH + gap;

        if (hHK && m_data.vk != 0 && !IsMouseVK(m_data.vk))
          SetHotkeyCtrl(hHK, m_data.modifiers, m_data.vk);

        // Mouse button dropdown
        TrackControl(CreateLabel(hDlg, L"Mouse:", margin, y + 2, labelW, lineH, hFont));
        HWND hMouse = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
          margin + labelW, y, cw - labelW - clearW - 6, 200, hDlg,
          (HMENU)(INT_PTR)IDC_AE_MOUSE, hInst, NULL);
        SendMessageW(hMouse, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hMouse);
        PopulateMouseCombo(hMouse);
        SendMessageW(hMouse, CB_SETCURSEL, MouseVKToIdx(m_data.vk), 0);
        y += lineH + gap;

        // Scope checkbox (owner-draw via CreateCheck)
        TrackControl(CreateCheck(hDlg, L"Global (system-wide)", IDC_AE_SCOPE,
          margin + labelW, y, cw - labelW, lineH, hFont,
          m_data.scope == HKSCOPE_GLOBAL));
        y += lineH + gap;
      }
    }

    y += gap;

    // OK / Cancel
    int btnW = 80;
    int btnH = lineH + 4;
    int totalBtnW = btnW * 2 + 12;
    int btnX = margin + (cw - totalBtnW) / 2;
    TrackControl(CreateBtn(hDlg, L"OK", IDOK, btnX, y, btnW, btnH, hFont));
    TrackControl(CreateBtn(hDlg, L"Cancel", IDCANCEL, btnX + btnW + 12, y, btnW, btnH, hFont));
    y += btnH + margin;

    FitToContent(clientW, y);
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    HWND hDlg = GetHWND();

    // Clear local key
    if (id == IDC_AE_CLEAR_KEY && code == BN_CLICKED) {
      HWND hHK = GetDlgItem(hDlg, IDC_AE_HOTKEY);
      if (hHK) SendMessageW(hHK, HKM_SETHOTKEY, 0, 0);
      HWND hMouse = GetDlgItem(hDlg, IDC_AE_MOUSE);
      if (hMouse) SendMessageW(hMouse, CB_SETCURSEL, 0, 0);
      return 0;
    }

    // Clear global key
    if (id == IDC_AE_GLOBAL_CLEAR && code == BN_CLICKED) {
      HWND hGHK = GetDlgItem(hDlg, IDC_AE_GLOBAL_HOTKEY);
      if (hGHK) SendMessageW(hGHK, HKM_SETHOTKEY, 0, 0);
      HWND hGMouse = GetDlgItem(hDlg, IDC_AE_GLOBAL_MOUSE);
      if (hGMouse) SendMessageW(hGMouse, CB_SETCURSEL, 0, 0);
      return 0;
    }

    // Local keyboard key changed — clear local mouse dropdown
    if (id == IDC_AE_HOTKEY && code == EN_CHANGE) {
      HWND hHK = GetDlgItem(hDlg, IDC_AE_HOTKEY);
      DWORD hk = hHK ? (DWORD)SendMessageW(hHK, HKM_GETHOTKEY, 0, 0) : 0;
      if (LOBYTE(LOWORD(hk)) != 0) {
        HWND hMouse = GetDlgItem(hDlg, IDC_AE_MOUSE);
        if (hMouse) SendMessageW(hMouse, CB_SETCURSEL, 0, 0);
      }
      return 0;
    }

    // Global keyboard key changed — clear global mouse dropdown
    if (id == IDC_AE_GLOBAL_HOTKEY && code == EN_CHANGE) {
      HWND hGHK = GetDlgItem(hDlg, IDC_AE_GLOBAL_HOTKEY);
      DWORD hk = hGHK ? (DWORD)SendMessageW(hGHK, HKM_GETHOTKEY, 0, 0) : 0;
      if (LOBYTE(LOWORD(hk)) != 0) {
        HWND hGMouse = GetDlgItem(hDlg, IDC_AE_GLOBAL_MOUSE);
        if (hGMouse) SendMessageW(hGMouse, CB_SETCURSEL, 0, 0);
      }
      return 0;
    }

    // Local mouse dropdown changed — clear local keyboard key
    if (id == IDC_AE_MOUSE && code == CBN_SELCHANGE) {
      HWND hMouse = GetDlgItem(hDlg, IDC_AE_MOUSE);
      int sel = hMouse ? (int)SendMessageW(hMouse, CB_GETCURSEL, 0, 0) : 0;
      if (sel > 0) {
        HWND hHK = GetDlgItem(hDlg, IDC_AE_HOTKEY);
        if (hHK) SendMessageW(hHK, HKM_SETHOTKEY, 0, 0);
      }
      return 0;
    }

    // Global mouse dropdown changed — clear global keyboard key
    if (id == IDC_AE_GLOBAL_MOUSE && code == CBN_SELCHANGE) {
      HWND hGMouse = GetDlgItem(hDlg, IDC_AE_GLOBAL_MOUSE);
      int sel = hGMouse ? (int)SendMessageW(hGMouse, CB_GETCURSEL, 0, 0) : 0;
      if (sel > 0) {
        HWND hGHK = GetDlgItem(hDlg, IDC_AE_GLOBAL_HOTKEY);
        if (hGHK) SendMessageW(hGHK, HKM_SETHOTKEY, 0, 0);
      }
      return 0;
    }

    if (id == IDC_AE_BROWSE && code == BN_CLICKED) {
      HWND hType = GetDlgItem(hDlg, IDC_AE_ACTION_TYPE);
      int sel = hType ? (int)SendMessageW(hType, CB_GETCURSEL, 0, 0) : -1;
      ButtonAction act = (sel >= 0) ? (ButtonAction)sel : m_data.actionType;

      wchar_t szFile[MAX_PATH] = {};
      OPENFILENAMEW ofn = {};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hDlg;
      ofn.lpstrFile = szFile;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

      switch (act) {
      case ButtonAction::LoadPreset:
        ofn.lpstrFilter = L"Presets (*.milk;*.milk2;*.milk3)\0*.milk;*.milk2;*.milk3\0All Files\0*.*\0";
        ofn.lpstrTitle = L"Select Preset";
        break;
      case ButtonAction::RunScript:
        ofn.lpstrFilter = L"Script Files (*.txt;*.mws)\0*.txt;*.mws\0All Files\0*.*\0";
        ofn.lpstrTitle = L"Select Script File";
        break;
      case ButtonAction::LaunchApp:
        ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
        ofn.lpstrTitle = L"Select Application";
        break;
      default:
        ofn.lpstrFilter = L"Script Files (*.txt;*.mws)\0*.txt;*.mws\0All Files (*.*)\0*.*\0";
        ofn.lpstrTitle = L"Select File";
        break;
      }

      if (m_pEngine) {
        static std::wstring s_initDir;
        s_initDir = m_pEngine->m_szBaseDir;
        ofn.lpstrInitialDir = s_initDir.c_str();
      }

      if (GetOpenFileNameW(&ofn))
        SetWindowTextW(GetDlgItem(hDlg, IDC_AE_PAYLOAD), szFile);
      return 0;
    }

    if (id == IDOK && code == BN_CLICKED) {
      // Read action + payload (user/button mode)
      if (!m_data.isBuiltInHotkey) {
        HWND hType = GetDlgItem(hDlg, IDC_AE_ACTION_TYPE);
        if (hType) {
          int sel = (int)SendMessageW(hType, CB_GETCURSEL, 0, 0);
          m_data.actionType = (ButtonAction)sel;
        }

        wchar_t buf[512] = {};
        GetDlgItemTextW(hDlg, IDC_AE_LABEL, buf, 512);
        m_data.label = buf;

        // Read payload
        HWND hPayload = GetDlgItem(hDlg, IDC_AE_PAYLOAD);
        int payLen = GetWindowTextLengthW(hPayload);
        std::wstring payload(payLen + 1, L'\0');
        GetWindowTextW(hPayload, &payload[0], payLen + 1);
        payload.resize(payLen);

        // For script/message actions, convert newlines to pipes
        if (m_data.actionType == ButtonAction::ScriptCommand ||
            m_data.actionType == ButtonAction::LaunchMessage) {
          size_t pos = 0;
          while ((pos = payload.find(L"\r\n", pos)) != std::wstring::npos)
            payload.replace(pos, 2, L"|");
          pos = 0;
          while ((pos = payload.find(L'\n', pos)) != std::wstring::npos)
            payload.replace(pos, 1, L"|");
          while (!payload.empty() && payload.back() == L'|')
            payload.pop_back();
        }
        m_data.payload = payload;
      }

      // Read key binding
      if (m_data.showKeyBinding) {
        static const UINT mouseVKs[] = { 0, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };

        auto readBinding = [&](int hkID, int mouseID, UINT& outMod, UINT& outVK) {
          HWND hMouse = GetDlgItem(hDlg, mouseID);
          int mouseIdx = hMouse ? (int)SendMessageW(hMouse, CB_GETCURSEL, 0, 0) : 0;
          if (mouseIdx > 0 && mouseIdx < (int)_countof(mouseVKs)) {
            outVK = mouseVKs[mouseIdx];
            outMod = 0;
          } else {
            HWND hHK = GetDlgItem(hDlg, hkID);
            DWORD hk = hHK ? (DWORD)SendMessageW(hHK, HKM_GETHOTKEY, 0, 0) : 0;
            outVK = LOBYTE(LOWORD(hk));
            UINT hkMod = HIBYTE(LOWORD(hk));
            outMod = 0;
            if (hkMod & HOTKEYF_ALT)     outMod |= MOD_ALT;
            if (hkMod & HOTKEYF_CONTROL) outMod |= MOD_CONTROL;
            if (hkMod & HOTKEYF_SHIFT)   outMod |= MOD_SHIFT;
          }
        };

        // Read local binding
        readBinding(IDC_AE_HOTKEY, IDC_AE_MOUSE, m_data.modifiers, m_data.vk);

        if (m_data.isBuiltInHotkey) {
          // Read global binding
          readBinding(IDC_AE_GLOBAL_HOTKEY, IDC_AE_GLOBAL_MOUSE, m_data.globalMod, m_data.globalVK);
          // Mouse buttons can't be registered as global hotkeys
          HWND hGMouse = GetDlgItem(hDlg, IDC_AE_GLOBAL_MOUSE);
          int gMouseIdx = hGMouse ? (int)SendMessageW(hGMouse, CB_GETCURSEL, 0, 0) : 0;
          if (gMouseIdx > 0) { m_data.globalVK = 0; m_data.globalMod = 0; }
        } else {
          // User hotkeys: single binding with scope
          m_data.scope = IsChecked(IDC_AE_SCOPE) ? HKSCOPE_GLOBAL : HKSCOPE_LOCAL;
          HWND hMouse = GetDlgItem(hDlg, IDC_AE_MOUSE);
          int mouseIdx = hMouse ? (int)SendMessageW(hMouse, CB_GETCURSEL, 0, 0) : 0;
          if (mouseIdx > 0) m_data.scope = HKSCOPE_LOCAL;
        }
      }

      m_data.accepted = true;
      EndDialog(true);
      return 0;
    }

    if (id == IDCANCEL && code == BN_CLICKED) {
      EndDialog(false);
      return 0;
    }

    return -1;
  }

public:
  ActionEditDialog(Engine* pEngine, ActionEditData& data)
    : ModalDialog(pEngine), m_data(data) {}
};

bool ShowActionEditDialog(HWND hParent, ActionEditData& data)
{
  ActionEditDialog dlg(data.pEngine, data);
  // Initial client size — DoBuildControls resizes height to fit content
  bool ok = dlg.Show(hParent, 420, 500);
  return ok && data.accepted;
}

//----------------------------------------------------------------------
// PromptForName — one label, one editable dropdown, OK / Cancel
//
// Naming a VFX profile used to go through GetSaveFileNameW, because a profile
// was a file. It is a key in a store now, so a file picker would be offering
// to put it somewhere that no longer means anything -- but a bare edit box
// would be worse than the file picker in one way, because at least that
// listed what already existed. The dropdown carries the existing names, and
// stays typeable for a new one.
//----------------------------------------------------------------------

namespace {

#define IDC_PROMPT_NAME   1301
#define IDC_PROMPT_OK     1302
#define IDC_PROMPT_CANCEL 1303

class NamePromptDialog : public ModalDialog {
public:
  NamePromptDialog(Engine* pEngine, const wchar_t* title, const wchar_t* prompt,
                   std::wstring* text, size_t maxLen,
                   const std::vector<std::wstring>& choices)
    : ModalDialog(pEngine), m_title(title), m_prompt(prompt),
      m_pText(text), m_maxLen(maxLen), m_choices(choices) {}

protected:
  const wchar_t* GetDialogTitle() const override { return m_title.c_str(); }
  const wchar_t* GetDialogClass() const override { return L"MDropDX12NamePrompt"; }

  void DoBuildControls(int clientW, int clientH) override {
    auto L = GetBaseLayout();
    int x = L.margin, y = L.margin;
    int rw = clientW - L.margin * 2;

    TrackControl(CreateLabel(m_hWnd, m_prompt.c_str(), x, y, rw, L.lineH, m_hFont));
    y += L.lineH + L.gap;

    // CBS_DROPDOWN, not DROPDOWNLIST: the point is to allow a name that is not
    // in the list yet. The height passed is the dropped-down height.
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
      x, y, rw, L.lineH * 9, m_hWnd,
      (HMENU)(INT_PTR)IDC_PROMPT_NAME, GetModuleHandle(NULL), NULL);
    TrackControl(hCombo);
    if (hCombo) {
      if (m_hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)m_hFont, TRUE);
      for (const auto& c : m_choices)
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)c.c_str());
      SetWindowTextW(hCombo, m_pText->c_str());
      SendMessage(hCombo, CB_LIMITTEXT, (WPARAM)(m_maxLen - 1), 0);
    }
    y += L.lineH + L.gap * 2;

    int btnW = MulDiv(80, L.lineH, 26);
    TrackControl(CreateBtn(m_hWnd, L"OK", IDC_PROMPT_OK,
                           x + rw - btnW * 2 - L.gap, y, btnW, L.lineH, m_hFont));
    TrackControl(CreateBtn(m_hWnd, L"Cancel", IDC_PROMPT_CANCEL,
                           x + rw - btnW, y, btnW, L.lineH, m_hFont));
    y += L.lineH + L.margin;

    FitToContent(clientW, y);
    if (hCombo) SetFocus(hCombo);
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    if (id == IDC_PROMPT_OK) {
      wchar_t buf[512] = {};
      GetDlgItemTextW(m_hWnd, IDC_PROMPT_NAME, buf, 512);
      // Trim: a name that is only spaces reads as a blank row in the list.
      std::wstring v(buf);
      const size_t a = v.find_first_not_of(L" \t");
      const size_t b = v.find_last_not_of(L" \t");
      v = (a == std::wstring::npos) ? L"" : v.substr(a, b - a + 1);
      if (v.empty()) {
        MessageBoxW(m_hWnd, L"Please enter a name.", m_title.c_str(), MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      *m_pText = v;
      EndDialog(true);
      return 0;
    }
    if (id == IDC_PROMPT_CANCEL) {
      EndDialog(false);
      return 0;
    }
    return -1;
  }

private:
  std::wstring  m_title, m_prompt;
  std::wstring* m_pText;
  size_t        m_maxLen;
  std::vector<std::wstring> m_choices;
};

} // namespace

bool PromptForName(Engine* pEngine, HWND hParent, const wchar_t* title,
                   const wchar_t* prompt, std::wstring& text, size_t maxLen,
                   const std::vector<std::wstring>& choices)
{
  NamePromptDialog dlg(pEngine, title, prompt, &text, maxLen, choices);
  return dlg.Show(hParent, 380, 200);
}

//----------------------------------------------------------------------
// Clipboard
//----------------------------------------------------------------------
// Another window may own the clipboard for a few milliseconds at a time, so a
// single OpenClipboard is not enough -- it fails outright rather than waiting.

bool CopyTextToClipboard(HWND owner, const wchar_t* text) {
  if (!text || !text[0]) return false;
  for (int attempt = 0; attempt < 8; attempt++) {
    if (OpenClipboard(owner)) {
      EmptyClipboard();
      size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
      HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (!hMem) { CloseClipboard(); return false; }
      void* p = GlobalLock(hMem);
      if (!p) { GlobalFree(hMem); CloseClipboard(); return false; }
      memcpy(p, text, bytes);
      GlobalUnlock(hMem);
      if (!SetClipboardData(CF_UNICODETEXT, hMem))
        GlobalFree(hMem);   // ownership only transfers on success
      CloseClipboard();
      return true;
    }
    Sleep(10);
  }
  return false;
}

//----------------------------------------------------------------------
// Shader error viewer
//----------------------------------------------------------------------
// D3DCompile prefixes every diagnostic with the "source name" it was handed,
// which for us is `<some directory>\Shader@0x00000243F354A910` -- a directory
// that has nothing to do with the preset and a pointer that is different every
// run. Pasting that into a bug report or an editor is noise, and it pushes the
// part that matters (line, column, message) off the right of the box. Strip
// the prefix back to the parenthesised position.

std::wstring StripShaderErrorPrefix(const std::wstring& line) {
  const size_t at = line.find(L"Shader@0x");
  if (at == std::wstring::npos) return line;
  const size_t paren = line.find(L'(', at);
  if (paren == std::wstring::npos) return line;
  return line.substr(paren);
}

std::wstring FormatShaderErrorForClipboard(const std::wstring& presetPath,
                                           const std::wstring& errorText,
                                           const std::wstring& capturedAt) {
  std::wstring out = presetPath;
  out += L"\r\n";
  if (!capturedAt.empty()) {
    out += L"captured ";
    out += capturedAt;
    out += L"\r\n";
  }
  size_t pos = 0;
  while (pos <= errorText.size()) {
    size_t nl = errorText.find(L'\n', pos);
    std::wstring line = errorText.substr(pos, nl == std::wstring::npos
                                                ? std::wstring::npos : nl - pos);
    while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' ||
                             line.back() == L'\t'))
      line.pop_back();
    if (!line.empty()) {
      out += StripShaderErrorPrefix(line);
      out += L"\r\n";
    }
    if (nl == std::wstring::npos) break;
    pos = nl + 1;
  }
  return out;
}

namespace {

#define IDC_SHERR_TEXT   1311
#define IDC_SHERR_COPY   1312
#define IDC_SHERR_CLOSE  1313

class ShaderErrorDialog : public ModalDialog {
public:
  ShaderErrorDialog(Engine* pEngine, const std::wstring& presetPath,
                    const std::wstring& errorText, const std::wstring& capturedAt)
    : ModalDialog(pEngine), m_path(presetPath), m_error(errorText),
      m_captured(capturedAt) {}

protected:
  const wchar_t* GetDialogTitle() const override { return L"Shader Error"; }
  const wchar_t* GetDialogClass() const override { return L"MDropDX12ShaderError"; }

  void DoBuildControls(int clientW, int clientH) override {
    auto L = GetBaseLayout();
    int x = L.margin, y = L.margin;
    int rw = clientW - L.margin * 2;

    // The path is what identifies the preset, so it is shown as well as
    // copied -- an error alone does not say which preset produced it.
    HWND hPath = CreateWindowExW(0, L"EDIT", m_path.c_str(),
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
    if (hPath && m_hFont) SendMessage(hPath, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    TrackControl(hPath);
    y += L.lineH + L.gap;

    // When this was recorded, shown above the text rather than buried in it:
    // a stored error survives the build that produced it, so an undated one
    // reads as a verdict on the running build when it may be nothing of the
    // kind.  Entries written before the date existed say so.
    if (!m_error.empty()) {
      std::wstring when = L"Captured: ";
      when += m_captured.empty() ? std::wstring(L"(before this was recorded)")
                                 : m_captured;
      HWND hWhen = CreateWindowExW(0, L"STATIC", when.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
      if (hWhen && m_hFont) SendMessage(hWhen, WM_SETFONT, (WPARAM)m_hFont, TRUE);
      TrackControl(hWhen);
      y += L.lineH + L.gap;
    }

    const std::wstring body = m_error.empty()
      ? std::wstring(L"(no error recorded)")
      : FormatShaderErrorForClipboard(m_path, m_error).substr(m_path.size() + 2);

    int editH = L.lineH * 9;
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", body.c_str(),
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
      ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
      x, y, rw, editH, m_hWnd, (HMENU)(INT_PTR)IDC_SHERR_TEXT,
      GetModuleHandle(NULL), NULL);
    if (hEdit && m_hFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    TrackControl(hEdit);
    y += editH + L.gap * 2;

    int btnW = MulDiv(80, L.lineH, 26);
    TrackControl(CreateBtn(m_hWnd, L"Copy", IDC_SHERR_COPY,
                           x + rw - btnW * 2 - L.gap, y, btnW, L.lineH, m_hFont));
    TrackControl(CreateBtn(m_hWnd, L"Close", IDC_SHERR_CLOSE,
                           x + rw - btnW, y, btnW, L.lineH, m_hFont));
    y += L.lineH + L.margin;

    FitToContent(clientW, y);
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    if (id == IDC_SHERR_COPY) {
      const std::wstring text = FormatShaderErrorForClipboard(m_path, m_error, m_captured);
      if (CopyTextToClipboard(m_hWnd, text.c_str()) && m_pEngine)
        m_pEngine->AddNotification(L"Shader error copied to clipboard");
      return 0;
    }
    if (id == IDC_SHERR_CLOSE || id == IDCANCEL) { EndDialog(false); return 0; }
    return -1;
  }

private:
  std::wstring m_path, m_error, m_captured;
};

} // namespace

void ShowShaderErrorDialog(Engine* pEngine, HWND hParent,
                           const std::wstring& presetPath,
                           const std::wstring& errorText,
                           const std::wstring& capturedAt) {
  ShaderErrorDialog dlg(pEngine, presetPath, errorText, capturedAt);
  dlg.Show(hParent, 620, 320);
}

} // namespace mdrop