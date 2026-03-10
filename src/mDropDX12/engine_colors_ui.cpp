/*
  ColorsWindow — standalone Colors tool window (ToolWindow subclass).
  Extracted from the Settings Colors tab.
  Controls: hue/saturation/brightness/gamma sliders, auto hue, reset.
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

ColorsWindow::ColorsWindow(Engine* pEngine)
  : ToolWindow(pEngine, 450, 400) {}

DWORD ColorsWindow::GetCommonControlFlags() const { return ICC_BAR_CLASSES; }

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void ColorsWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  int lw = MulDiv(160, lineH, 26);
  int sliderW = rw - lw - 60;
  wchar_t buf[64];

  Engine* p = m_pEngine;

  // Hue
  TrackControl(CreateLabel(hw, L"Hue:", x, y, lw, lineH, hFont));
  TrackControl(CreateSlider(hw, IDC_MW_COL_HUE, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(p->m_ColShiftHue * 100) + 100));
  swprintf(buf, 64, L"%.2f", p->m_ColShiftHue);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | WS_VISIBLE | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_HUE_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hLbl);
  }
  y += lineH + gap;

  // Saturation
  TrackControl(CreateLabel(hw, L"Saturation:", x, y, lw, lineH, hFont));
  TrackControl(CreateSlider(hw, IDC_MW_COL_SAT, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(p->m_ColShiftSaturation * 100) + 100));
  swprintf(buf, 64, L"%.2f", p->m_ColShiftSaturation);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | WS_VISIBLE | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_SAT_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hLbl);
  }
  y += lineH + gap;

  // Brightness
  TrackControl(CreateLabel(hw, L"Brightness:", x, y, lw, lineH, hFont));
  TrackControl(CreateSlider(hw, IDC_MW_COL_BRIGHT, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(p->m_ColShiftBrightness * 100) + 100));
  swprintf(buf, 64, L"%.2f", p->m_ColShiftBrightness);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | WS_VISIBLE | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_BRIGHT_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hLbl);
  }
  y += lineH + gap;

  // Gamma
  TrackControl(CreateLabel(hw, L"Gamma:", x, y, lw, lineH, hFont));
  TrackControl(CreateSlider(hw, IDC_MW_COL_GAMMA, x + lw + 4, y, sliderW, lineH, 0, 80, (int)(p->m_pState->m_fGammaAdj.eval(-1) * 10)));
  swprintf(buf, 64, L"%.1f", p->m_pState->m_fGammaAdj.eval(-1));
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | WS_VISIBLE | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_GAMMA_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hLbl);
  }
  y += lineH + gap + 4;

  // Auto Hue
  TrackControl(CreateCheck(hw, L"Auto Hue", IDC_MW_AUTO_HUE, x, y, rw / 2, lineH, hFont, p->m_AutoHue));
  TrackControl(CreateLabel(hw, L"Seconds:", x + rw / 2, y, 60, lineH, hFont));
  swprintf(buf, 64, L"%.3f", p->m_AutoHueSeconds);
  TrackControl(CreateEdit(hw, buf, IDC_MW_AUTO_HUE_SEC, x + rw / 2 + 64, y, 70, lineH, hFont));
  y += lineH + gap + 4;

  TrackControl(CreateBtn(hw, L"Reset", IDC_MW_RESET_COLORS, x, y, MulDiv(80, lineH, 26), lineH, hFont));
}

//----------------------------------------------------------------------
// Slider handler
//----------------------------------------------------------------------

LRESULT ColorsWindow::DoHScroll(HWND hWnd, int id, int pos) {
  switch (id) {
  case IDC_MW_COL_HUE: {
    m_pEngine->m_ColShiftHue = (pos - 100) / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%.2f", m_pEngine->m_ColShiftHue);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_HUE_LABEL), buf);
    break;
  }
  case IDC_MW_COL_SAT: {
    m_pEngine->m_ColShiftSaturation = (pos - 100) / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%.2f", m_pEngine->m_ColShiftSaturation);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_SAT_LABEL), buf);
    break;
  }
  case IDC_MW_COL_BRIGHT: {
    m_pEngine->m_ColShiftBrightness = (pos - 100) / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%.2f", m_pEngine->m_ColShiftBrightness);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT_LABEL), buf);
    break;
  }
  case IDC_MW_COL_GAMMA: {
    float gamma = pos / 10.0f;
    m_pEngine->m_pState->m_fGammaAdj = gamma;
    wchar_t buf[32]; swprintf(buf, 32, L"%.1f", gamma);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_GAMMA_LABEL), buf);
    break;
  }
  default:
    return -1;
  }
  return 0;
}

//----------------------------------------------------------------------
// Command handler
//----------------------------------------------------------------------

LRESULT ColorsWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;

  if (id == IDC_MW_RESET_COLORS && code == BN_CLICKED) {
    p->m_ColShiftHue = 0.0f;
    p->m_ColShiftSaturation = 0.0f;
    p->m_ColShiftBrightness = 0.0f;
    if (p->m_pState) p->m_pState->m_fGammaAdj = 2.0f;
    p->m_AutoHue = false;
    p->m_AutoHueSeconds = 0.02f;
    // Update sliders
    SendMessage(GetDlgItem(hWnd, IDC_MW_COL_HUE), TBM_SETPOS, TRUE, 100);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_HUE_LABEL), L"0.00");
    SendMessage(GetDlgItem(hWnd, IDC_MW_COL_SAT), TBM_SETPOS, TRUE, 100);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_SAT_LABEL), L"0.00");
    SendMessage(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT), TBM_SETPOS, TRUE, 100);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT_LABEL), L"0.00");
    SendMessage(GetDlgItem(hWnd, IDC_MW_COL_GAMMA), TBM_SETPOS, TRUE, 20);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_GAMMA_LABEL), L"2.0");
    // Update checkbox and edit
    SetChecked(IDC_MW_AUTO_HUE, false);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_AUTO_HUE_SEC), L"0.020");
    return 0;
  }

  // Checkbox
  if (id == IDC_MW_AUTO_HUE && code == BN_CLICKED) {
    p->m_AutoHue = IsChecked(id);
    return 0;
  }

  // EN_KILLFOCUS for Auto Hue Seconds
  if (id == IDC_MW_AUTO_HUE_SEC && code == EN_KILLFOCUS) {
    wchar_t buf[64];
    GetDlgItemTextW(hWnd, id, buf, 64);
    p->m_AutoHueSeconds = (float)_wtof(buf);
    if (p->m_AutoHueSeconds < 0.001f) p->m_AutoHueSeconds = 0.001f;
    return 0;
  }

  return -1;
}

} // namespace mdrop
