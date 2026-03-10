/*
  ControllerWindow — standalone Game Controller tool window (ToolWindow subclass).
  Extracted from the Settings System tab.
  Controls: enable checkbox, device combo, scan, JSON edit, defaults/save/load, help.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <commctrl.h>

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

ControllerWindow::ControllerWindow(Engine* pEngine)
  : ToolWindow(pEngine, 400, 500) {}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void ControllerWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();

  Engine* p = m_pEngine;

  // Title + Help
  {
    int titleW = MulDiv(130, lineH, 26);
    int helpW = lineH;
    TrackControl(CreateLabel(hw, L"Game Controller", x, y, titleW, lineH, hFont));
    TrackControl(CreateBtn(hw, L"\u2753", IDC_MW_CTRL_HELP, x + titleW + 4, y, helpW, lineH, hFont));
  }
  TrackControl(CreateCheck(hw, L"Enable", IDC_MW_CTRL_ENABLE, x + rw / 2, y, rw / 2, lineH, hFont, p->m_bControllerEnabled));
  y += lineH + gap;

  {
    int lblW = MulDiv(55, lineH, 26);
    int scanW = MulDiv(60, lineH, 26);
    int comboW = rw - lblW - scanW - 8;

    // Device combo + Scan button
    TrackControl(CreateLabel(hw, L"Device:", x, y, lblW, lineH, hFont));
    HWND hCtrlCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
      x + lblW, y, comboW, lineH * 8, hw,
      (HMENU)(INT_PTR)IDC_MW_CTRL_DEVICE, GetModuleHandle(NULL), NULL);
    if (hCtrlCombo && hFont) SendMessage(hCtrlCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hCtrlCombo);
    p->EnumerateControllers(hCtrlCombo);

    TrackControl(CreateBtn(hw, L"Scan", IDC_MW_CTRL_SCAN, x + lblW + comboW + 4, y, scanW, lineH, hFont));
    y += lineH + gap;

    // Button Mapping label
    TrackControl(CreateLabel(hw, L"Button Mapping:", x, y, rw, lineH, hFont));
    y += lineH;

    // Multiline JSON edit control
    int editH = lineH * 8;
    HWND hJsonEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
      x, y, rw, editH, hw,
      (HMENU)(INT_PTR)IDC_MW_CTRL_JSON_EDIT, GetModuleHandle(NULL), NULL);
    if (hJsonEdit && hFont) SendMessage(hJsonEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hJsonEdit);

    // Populate with current JSON text
    {
      int wLen = MultiByteToWideChar(CP_ACP, 0, p->m_szControllerJSONText.c_str(), (int)p->m_szControllerJSONText.size(), NULL, 0);
      std::wstring wJson(wLen, L'\0');
      MultiByteToWideChar(CP_ACP, 0, p->m_szControllerJSONText.c_str(), (int)p->m_szControllerJSONText.size(), &wJson[0], wLen);
      SetWindowTextW(hJsonEdit, wJson.c_str());
    }
    y += editH + gap;

    // Defaults / Save / Load buttons
    int btnW = MulDiv(70, lineH, 26);
    int btnGap = 6;
    TrackControl(CreateBtn(hw, L"Defaults", IDC_MW_CTRL_DEFAULTS, x, y, btnW, lineH, hFont));
    TrackControl(CreateBtn(hw, L"Save", IDC_MW_CTRL_SAVE, x + btnW + btnGap, y, btnW, lineH, hFont));
    TrackControl(CreateBtn(hw, L"Load", IDC_MW_CTRL_LOAD, x + 2 * (btnW + btnGap), y, btnW, lineH, hFont));

    // Disable sub-controls if not enabled
    if (!p->m_bControllerEnabled) {
      EnableWindow(hCtrlCombo, FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_CTRL_SCAN), FALSE);
      EnableWindow(hJsonEdit, FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_CTRL_DEFAULTS), FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_CTRL_SAVE), FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_CTRL_LOAD), FALSE);
    }
  }
}

//----------------------------------------------------------------------
// Command handler
//----------------------------------------------------------------------

LRESULT ControllerWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;

  // Enable checkbox
  if (id == IDC_MW_CTRL_ENABLE && code == BN_CLICKED) {
    bool bChecked = IsChecked(id);
    p->m_bControllerEnabled = bChecked;
    p->m_dwLastControllerButtons = 0;
    EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_DEVICE), bChecked);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_SCAN), bChecked);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT), bChecked);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_DEFAULTS), bChecked);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_SAVE), bChecked);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_LOAD), bChecked);
    if (bChecked) {
      HWND hEdit = GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT);
      if (hEdit) {
        int len = GetWindowTextLengthW(hEdit);
        std::wstring wText(len + 1, L'\0');
        GetWindowTextW(hEdit, &wText[0], len + 1);
        int cbNeeded = WideCharToMultiByte(CP_ACP, 0, wText.c_str(), len, NULL, 0, NULL, NULL);
        p->m_szControllerJSONText.resize(cbNeeded);
        WideCharToMultiByte(CP_ACP, 0, wText.c_str(), len, &p->m_szControllerJSONText[0], cbNeeded, NULL, NULL);
        p->ParseControllerJSON(p->m_szControllerJSONText);
      }
    }
    p->SaveControllerSettings();
    return 0;
  }

  // Help
  if (id == IDC_MW_CTRL_HELP && code == BN_CLICKED) {
    p->ShowControllerHelpPopup(hWnd);
    return 0;
  }

  // Scan
  if (id == IDC_MW_CTRL_SCAN && code == BN_CLICKED) {
    HWND hCombo = GetDlgItem(hWnd, IDC_MW_CTRL_DEVICE);
    if (hCombo) p->EnumerateControllers(hCombo);
    return 0;
  }

  // Defaults
  if (id == IDC_MW_CTRL_DEFAULTS && code == BN_CLICKED) {
    HWND hEdit = GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT);
    if (hEdit) {
      std::string def = p->GetDefaultControllerJSON();
      int wLen = MultiByteToWideChar(CP_ACP, 0, def.c_str(), (int)def.size(), NULL, 0);
      std::wstring wDef(wLen, L'\0');
      MultiByteToWideChar(CP_ACP, 0, def.c_str(), (int)def.size(), &wDef[0], wLen);
      SetWindowTextW(hEdit, wDef.c_str());
    }
    return 0;
  }

  // Save
  if (id == IDC_MW_CTRL_SAVE && code == BN_CLICKED) {
    HWND hEdit = GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT);
    if (hEdit) {
      int len = GetWindowTextLengthW(hEdit);
      std::wstring wText(len + 1, L'\0');
      GetWindowTextW(hEdit, &wText[0], len + 1);
      int cbNeeded = WideCharToMultiByte(CP_ACP, 0, wText.c_str(), len, NULL, 0, NULL, NULL);
      std::string text(cbNeeded, '\0');
      WideCharToMultiByte(CP_ACP, 0, wText.c_str(), len, &text[0], cbNeeded, NULL, NULL);
      p->SaveControllerJSON(text);
    }
    return 0;
  }

  // Load
  if (id == IDC_MW_CTRL_LOAD && code == BN_CLICKED) {
    p->LoadControllerJSON();
    HWND hEdit = GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT);
    if (hEdit) {
      const std::string& s = p->m_szControllerJSONText;
      int wLen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), NULL, 0);
      std::wstring wJson(wLen, L'\0');
      MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &wJson[0], wLen);
      SetWindowTextW(hEdit, wJson.c_str());
    }
    return 0;
  }

  // Device combo
  if (id == IDC_MW_CTRL_DEVICE && code == CBN_SELCHANGE) {
    HWND hCombo = (HWND)lParam;
    int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0) {
      int joyID = (int)SendMessage(hCombo, CB_GETITEMDATA, sel, 0);
      p->m_nControllerDeviceID = joyID;
      p->m_dwLastControllerButtons = 0;
      SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)p->m_szControllerName);
      p->SaveControllerSettings();
    }
    return 0;
  }

  return -1;
}

} // namespace mdrop
