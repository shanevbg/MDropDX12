// Error Display Settings window — configures Normal/LOUD error display modes.

#include "engine.h"

namespace mdrop {

extern Engine g_engine;

// ── Open / Close bridges ──

void Engine::OpenErrorDisplayWindow() {
  if (!m_errorDisplayWindow)
    m_errorDisplayWindow = std::make_unique<ErrorDisplayWindow>(this);
  m_errorDisplayWindow->Open();
}

void Engine::CloseErrorDisplayWindow() {
  if (m_errorDisplayWindow)
    m_errorDisplayWindow->Close();
}

// ── ErrorDisplayWindow ──

ErrorDisplayWindow::ErrorDisplayWindow(Engine* pEngine)
  : ToolWindow(pEngine, 460, 680) {}

static void SetEditInt(HWND hWnd, int id, int val) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%d", val);
  SetDlgItemTextW(hWnd, id, buf);
}

static void SetEditFloat(HWND hWnd, int id, float val) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%.1f", val);
  SetDlgItemTextW(hWnd, id, buf);
}

static int GetEditInt(HWND hWnd, int id) {
  wchar_t buf[32];
  GetDlgItemTextW(hWnd, id, buf, 32);
  return _wtoi(buf);
}

static float GetEditFloat(HWND hWnd, int id) {
  wchar_t buf[32];
  GetDlgItemTextW(hWnd, id, buf, 32);
  return (float)_wtof(buf);
}

void ErrorDisplayWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  Engine* p = m_pEngine;
  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = m_hFont;
  HFONT hFontBold = m_hFontBold;

  int lw = 110;  // label width
  int ew = 60;   // edit width
  int cw = 40;   // color edit width

  // ═══ Normal Mode ═══
  TrackControl(CreateLabel(hw, L"Normal Mode", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // Duration
  TrackControl(CreateLabel(hw, L"Duration (s):", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_NORM_DURATION, x + lw + 4, y, ew, lineH, hFont));
  SetEditFloat(hw, IDC_ERRDLG_NORM_DURATION, p->m_ErrorDuration);
  y += lineH + 2;

  // Font Face
  TrackControl(CreateLabel(hw, L"Font Face:", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_NORM_FONTFACE, x + lw + 4, y, rw - lw - 4, lineH, hFont));
  SetDlgItemTextW(hw, IDC_ERRDLG_NORM_FONTFACE, p->m_szErrorFontFace);
  y += lineH + 2;

  // Font Size
  TrackControl(CreateLabel(hw, L"Font Size:", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_NORM_FONTSIZE, x + lw + 4, y, ew, lineH, hFont));
  SetEditInt(hw, IDC_ERRDLG_NORM_FONTSIZE, p->m_ErrorFontSize);
  y += lineH + 2;

  // Corner
  TrackControl(CreateLabel(hw, L"Corner:", x, y, lw, lineH, hFont));
  {
    HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      x + lw + 4, y, rw - lw - 4, lineH + 4 * lineH, hw, (HMENU)(INT_PTR)IDC_ERRDLG_NORM_CORNER,
      GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Upper-Right");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Upper-Left");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Lower-Right");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Lower-Left");
    SendMessageW(hCombo, CB_SETCURSEL, p->m_ErrorCorner, 0);
    TrackControl(hCombo);
  }
  y += lineH + 2;

  // Color R G B
  TrackControl(CreateLabel(hw, L"Color (R G B):", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_NORM_R, x + lw + 4, y, cw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_NORM_G, x + lw + 4 + cw + 4, y, cw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_NORM_B, x + lw + 4 + (cw + 4) * 2, y, cw, lineH, hFont));
  SetEditInt(hw, IDC_ERRDLG_NORM_R, p->m_ErrorColorR);
  SetEditInt(hw, IDC_ERRDLG_NORM_G, p->m_ErrorColorG);
  SetEditInt(hw, IDC_ERRDLG_NORM_B, p->m_ErrorColorB);
  y += lineH + 2;

  // Test Normal button
  TrackControl(CreateBtn(hw, L"Test Normal", IDC_ERRDLG_NORM_TEST, x + lw + 4, y, 120, lineH, hFont));
  y += lineH + gap * 2;

  // ═══ LOUD Mode ═══
  TrackControl(CreateLabel(hw, L"LOUD Mode", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // Duration
  TrackControl(CreateLabel(hw, L"Duration (s):", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_DURATION, x + lw + 4, y, ew, lineH, hFont));
  SetEditFloat(hw, IDC_ERRDLG_LOUD_DURATION, p->m_LoudDuration);
  y += lineH + 2;

  // Font Size
  TrackControl(CreateLabel(hw, L"Font Size (0=auto):", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_FONTSIZE, x + lw + 4, y, ew, lineH, hFont));
  SetEditInt(hw, IDC_ERRDLG_LOUD_FONTSIZE, p->m_LoudFontSize);
  y += lineH + 2;

  // Color 1
  TrackControl(CreateLabel(hw, L"Color 1 (R G B):", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_R1, x + lw + 4, y, cw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_G1, x + lw + 4 + cw + 4, y, cw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_B1, x + lw + 4 + (cw + 4) * 2, y, cw, lineH, hFont));
  SetEditInt(hw, IDC_ERRDLG_LOUD_R1, p->m_LoudColorR1);
  SetEditInt(hw, IDC_ERRDLG_LOUD_G1, p->m_LoudColorG1);
  SetEditInt(hw, IDC_ERRDLG_LOUD_B1, p->m_LoudColorB1);
  y += lineH + 2;

  // Color 2
  TrackControl(CreateLabel(hw, L"Color 2 (R G B):", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_R2, x + lw + 4, y, cw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_G2, x + lw + 4 + cw + 4, y, cw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_B2, x + lw + 4 + (cw + 4) * 2, y, cw, lineH, hFont));
  SetEditInt(hw, IDC_ERRDLG_LOUD_R2, p->m_LoudColorR2);
  SetEditInt(hw, IDC_ERRDLG_LOUD_G2, p->m_LoudColorG2);
  SetEditInt(hw, IDC_ERRDLG_LOUD_B2, p->m_LoudColorB2);
  y += lineH + 2;

  // Pulse Speed
  TrackControl(CreateLabel(hw, L"Pulse Speed:", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_ERRDLG_LOUD_PULSE, x + lw + 4, y, ew, lineH, hFont));
  SetEditInt(hw, IDC_ERRDLG_LOUD_PULSE, p->m_LoudPulseSpeed);
  y += lineH + 2;

  // Test LOUD button
  TrackControl(CreateBtn(hw, L"Test LOUD", IDC_ERRDLG_LOUD_TEST, x + lw + 4, y, 120, lineH, hFont));
  y += lineH + gap * 2;

  // ═══ Footer ═══
  int btnW = 130;
  TrackControl(CreateBtn(hw, L"Reset to Defaults", IDC_ERRDLG_RESET, x, y, btnW, lineH, hFont));
}

void ErrorDisplayWindow::ApplySettings() {
  Engine* p = m_pEngine;
  HWND hw = m_hWnd;

  // Normal
  p->m_ErrorDuration = GetEditFloat(hw, IDC_ERRDLG_NORM_DURATION);
  if (p->m_ErrorDuration < 0.5f) p->m_ErrorDuration = 0.5f;
  if (p->m_ErrorDuration > 120.0f) p->m_ErrorDuration = 120.0f;

  GetDlgItemTextW(hw, IDC_ERRDLG_NORM_FONTFACE, p->m_szErrorFontFace, _countof(p->m_szErrorFontFace));
  p->m_ErrorFontSize = GetEditInt(hw, IDC_ERRDLG_NORM_FONTSIZE);
  if (p->m_ErrorFontSize < 0) p->m_ErrorFontSize = 0;
  if (p->m_ErrorFontSize > 200) p->m_ErrorFontSize = 200;

  HWND hCombo = GetDlgItem(hw, IDC_ERRDLG_NORM_CORNER);
  if (hCombo) p->m_ErrorCorner = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);

  p->m_ErrorColorR = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_NORM_R)));
  p->m_ErrorColorG = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_NORM_G)));
  p->m_ErrorColorB = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_NORM_B)));

  // LOUD
  p->m_LoudDuration = GetEditFloat(hw, IDC_ERRDLG_LOUD_DURATION);
  if (p->m_LoudDuration < 1.0f) p->m_LoudDuration = 1.0f;
  if (p->m_LoudDuration > 300.0f) p->m_LoudDuration = 300.0f;

  p->m_LoudFontSize = GetEditInt(hw, IDC_ERRDLG_LOUD_FONTSIZE);
  if (p->m_LoudFontSize < 0) p->m_LoudFontSize = 0;
  if (p->m_LoudFontSize > 500) p->m_LoudFontSize = 500;

  p->m_LoudColorR1 = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_LOUD_R1)));
  p->m_LoudColorG1 = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_LOUD_G1)));
  p->m_LoudColorB1 = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_LOUD_B1)));
  p->m_LoudColorR2 = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_LOUD_R2)));
  p->m_LoudColorG2 = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_LOUD_G2)));
  p->m_LoudColorB2 = max(0, min(255, GetEditInt(hw, IDC_ERRDLG_LOUD_B2)));

  p->m_LoudPulseSpeed = max(0, min(20, GetEditInt(hw, IDC_ERRDLG_LOUD_PULSE)));

  // Save to INI
  p->MyWriteConfig();
}

void ErrorDisplayWindow::ResetToDefaults() {
  HWND hw = m_hWnd;

  // Normal defaults
  SetEditFloat(hw, IDC_ERRDLG_NORM_DURATION, 8.0f);
  SetDlgItemTextW(hw, IDC_ERRDLG_NORM_FONTFACE, L"Segoe UI");
  SetEditInt(hw, IDC_ERRDLG_NORM_FONTSIZE, 20);
  HWND hCombo = GetDlgItem(hw, IDC_ERRDLG_NORM_CORNER);
  if (hCombo) SendMessage(hCombo, CB_SETCURSEL, 0, 0);
  SetEditInt(hw, IDC_ERRDLG_NORM_R, 255);
  SetEditInt(hw, IDC_ERRDLG_NORM_G, 255);
  SetEditInt(hw, IDC_ERRDLG_NORM_B, 255);

  // LOUD defaults
  SetEditFloat(hw, IDC_ERRDLG_LOUD_DURATION, 30.0f);
  SetEditInt(hw, IDC_ERRDLG_LOUD_FONTSIZE, 0);
  SetEditInt(hw, IDC_ERRDLG_LOUD_R1, 255);
  SetEditInt(hw, IDC_ERRDLG_LOUD_G1, 50);
  SetEditInt(hw, IDC_ERRDLG_LOUD_B1, 50);
  SetEditInt(hw, IDC_ERRDLG_LOUD_R2, 255);
  SetEditInt(hw, IDC_ERRDLG_LOUD_G2, 255);
  SetEditInt(hw, IDC_ERRDLG_LOUD_B2, 50);
  SetEditInt(hw, IDC_ERRDLG_LOUD_PULSE, 2);

  ApplySettings();
}

LRESULT ErrorDisplayWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;

  if (code == BN_CLICKED) {
    switch (id) {
    case IDC_ERRDLG_NORM_TEST:
      ApplySettings();
      p->ClearErrors(ERR_NOTIFY);
      p->AddError((wchar_t*)L"This is a test error message.", p->m_ErrorDuration, ERR_NOTIFY, true);
      return 0;

    case IDC_ERRDLG_LOUD_TEST:
      ApplySettings();
      p->ClearErrors(ERR_MISC);
      p->AddLoudError((wchar_t*)L"LOUD TEST MESSAGE");
      return 0;

    case IDC_ERRDLG_RESET:
      ResetToDefaults();
      return 0;
    }
  }

  // Auto-save on edit changes (EN_KILLFOCUS = when user tabs away from an edit)
  if (code == EN_KILLFOCUS) {
    ApplySettings();
    return 0;
  }

  // Combo box selection
  if (id == IDC_ERRDLG_NORM_CORNER && code == CBN_SELCHANGE) {
    ApplySettings();
    return 0;
  }

  return -1; // not handled
}

} // namespace mdrop
