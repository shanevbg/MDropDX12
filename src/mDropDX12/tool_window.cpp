/*
  ToolWindow — base class implementation for standalone tool windows on their own threads.
  Handles: thread lifecycle, message pump, dark theme painting, pin button,
  font +/- with cross-window sync, window size persistence, owner-draw controls.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shellapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor / Destructor
//----------------------------------------------------------------------

ToolWindow::ToolWindow(Engine* pEngine, int defaultW, int defaultH)
  : m_pEngine(pEngine), m_nDefaultW(defaultW), m_nDefaultH(defaultH),
    m_nWndW(defaultW), m_nWndH(defaultH) {}

ToolWindow::~ToolWindow() {
  Close();
}

//----------------------------------------------------------------------
// Open / Close
//----------------------------------------------------------------------

void ToolWindow::OnAlreadyOpen() {
  SetForegroundWindow(m_hWnd);
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
  if (m_settingsWindow && m_settingsWindow->IsOpen() && m_settingsWindow->GetHWND() != hSender)
    PostMessage(m_settingsWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_displaysWindow && m_displaysWindow->IsOpen() && m_displaysWindow->GetHWND() != hSender)
    PostMessage(m_displaysWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_songInfoWindow && m_songInfoWindow->IsOpen() && m_songInfoWindow->GetHWND() != hSender)
    PostMessage(m_songInfoWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_hotkeysWindow && m_hotkeysWindow->IsOpen() && m_hotkeysWindow->GetHWND() != hSender)
    PostMessage(m_hotkeysWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_midiWindow && m_midiWindow->IsOpen() && m_midiWindow->GetHWND() != hSender)
    PostMessage(m_midiWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_boardWindow && m_boardWindow->IsOpen() && m_boardWindow->GetHWND() != hSender)
    PostMessage(m_boardWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_presetsWindow && m_presetsWindow->IsOpen() && m_presetsWindow->GetHWND() != hSender)
    PostMessage(m_presetsWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_spritesWindow && m_spritesWindow->IsOpen() && m_spritesWindow->GetHWND() != hSender)
    PostMessage(m_spritesWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_messagesWindow && m_messagesWindow->IsOpen() && m_messagesWindow->GetHWND() != hSender)
    PostMessage(m_messagesWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_shaderImportWindow && m_shaderImportWindow->IsOpen() && m_shaderImportWindow->GetHWND() != hSender)
    PostMessage(m_shaderImportWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  if (m_workspaceLayoutWindow && m_workspaceLayoutWindow->IsOpen() && m_workspaceLayoutWindow->GetHWND() != hSender)
    PostMessage(m_workspaceLayoutWindow->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
}

//----------------------------------------------------------------------
// Thread + Window Creation
//----------------------------------------------------------------------

void ToolWindow::LoadWindowPosition() {
  const wchar_t* ini = m_pEngine->GetConfigIniFile();
  const wchar_t* sec = GetINISection();
  m_nWndW = GetPrivateProfileIntW(sec, L"WndW", m_nDefaultW, ini);
  m_nWndH = GetPrivateProfileIntW(sec, L"WndH", m_nDefaultH, ini);
  m_nPosX = GetPrivateProfileIntW(sec, L"PosX", -1, ini);
  m_nPosY = GetPrivateProfileIntW(sec, L"PosY", -1, ini);
  m_bOnTop = GetPrivateProfileIntW(sec, L"OnTop", 1, ini) != 0; // default sticky
  if (m_nWndW < GetMinWidth()) m_nWndW = GetMinWidth();
  if (m_nWndH < GetMinHeight()) m_nWndH = GetMinHeight();
}

void ToolWindow::SaveWindowPosition() {
  if (!m_hWnd) return;
  const wchar_t* ini = m_pEngine->GetConfigIniFile();
  const wchar_t* sec = GetINISection();
  wchar_t buf[16];
  RECT rc;
  GetWindowRect(m_hWnd, &rc);
  swprintf(buf, 16, L"%d", rc.right - rc.left);
  WritePrivateProfileStringW(sec, L"WndW", buf, ini);
  swprintf(buf, 16, L"%d", rc.bottom - rc.top);
  WritePrivateProfileStringW(sec, L"WndH", buf, ini);
  swprintf(buf, 16, L"%d", (int)rc.left);
  WritePrivateProfileStringW(sec, L"PosX", buf, ini);
  swprintf(buf, 16, L"%d", (int)rc.top);
  WritePrivateProfileStringW(sec, L"PosY", buf, ini);
  WritePrivateProfileStringW(sec, L"OnTop", m_bOnTop ? L"1" : L"0", ini);
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

  int posX, posY;
  if (m_nPosX >= 0 && m_nPosY >= 0) {
    posX = m_nPosX;
    posY = m_nPosY;
  } else {
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    posX = (screenW - m_nWndW) / 2;
    posY = (screenH - m_nWndH) / 2;
  }

  DWORD exStyle = WS_EX_TOOLWINDOW;
  if (m_bOnTop) exStyle |= WS_EX_TOPMOST;

  m_hWnd = CreateWindowExW(
    exStyle,
    GetWindowClass(), GetWindowTitle(),
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
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
  ApplyDarkTheme();

  ShowWindow(m_hWnd, SW_SHOW);
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
              if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"RichEdit20W") == 0)
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
      DrawOwnerButton(pDIS, p->IsDarkTheme(),
        p->m_colSettingsBtnFace, p->m_colSettingsBtnHi, p->m_colSettingsBtnShadow, p->m_colSettingsText);
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
  WritePrivateProfileStringW(GetINISection(), L"ActiveTab", buf, m_pEngine->GetConfigIniFile());
}

void ToolWindow::SelectInitialTab() {
  if (!m_hTab || m_pageCtrls.empty()) return;
  int numPages = (int)m_pageCtrls.size();
  int tab = GetPrivateProfileIntW(GetINISection(), L"ActiveTab", 0, m_pEngine->GetConfigIniFile());
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

void ToolWindow::RebuildFonts() {
  if (!m_hWnd) return;

  // Save active tab before destroying controls
  int savedTab = m_hTab ? TabCtrl_GetCurSel(m_hTab) : 0;

  // Destroy all child windows
  HWND hChild = GetWindow(m_hWnd, GW_CHILD);
  while (hChild) {
    HWND hNext = GetWindow(hChild, GW_HWNDNEXT);
    DestroyWindow(hChild);
    hChild = hNext;
  }
  m_childCtrls.clear();
  m_pageCtrls.clear();
  m_hTab = NULL;

  DoBuildControls();
  ApplyDarkTheme();

  // Restore tab selection (overrides the default from SelectInitialTab)
  if (m_hTab && savedTab > 0 && savedTab < (int)m_pageCtrls.size()) {
    TabCtrl_SetCurSel(m_hTab, savedTab);
    ShowPage(savedTab);
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

  case WM_SIZE:
    if (wParam != SIZE_MINIMIZED) {
      RECT rc;
      GetWindowRect(hWnd, &rc);
      tw->m_nWndW = rc.right - rc.left;
      tw->m_nWndH = rc.bottom - rc.top;
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
      if (p->m_nSettingsFontSize > -24) {
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

} // namespace mdrop
