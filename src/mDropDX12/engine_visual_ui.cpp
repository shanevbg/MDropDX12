/*
  VisualWindow — standalone Visual tool window (ToolWindow subclass).
  Extracted from the Settings Visual tab.
  Controls: opacity, render quality, time/frame/fps factors, vis settings,
            GPU protection, VSync, FPS cap, reload/restart buttons.
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

VisualWindow::VisualWindow(Engine* pEngine)
  : ToolWindow(pEngine, 480, 750) {}

DWORD VisualWindow::GetCommonControlFlags() const { return ICC_BAR_CLASSES; }

//----------------------------------------------------------------------
// Engine bridge: Open/Close
//----------------------------------------------------------------------

void Engine::OpenVisualWindow() {
  if (!m_visualWindow)
    m_visualWindow = std::make_unique<VisualWindow>(this);
  m_visualWindow->Open();
}

void Engine::CloseVisualWindow() {
  if (m_visualWindow)
    m_visualWindow->Close();
}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void VisualWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  int lw = MulDiv(160, lineH, 26);
  int sliderW = rw - lw - 60;
  wchar_t buf[64];

  Engine* p = m_pEngine;

  // Opacity
  TrackControl(CreateLabel(hw, L"Opacity:", x, y, lw, lineH, hFont));
  TrackControl(CreateSlider(hw, IDC_MW_OPACITY, x + lw + 4, y, sliderW, lineH, 0, 100, (int)(p->fOpacity * 100)));
  swprintf(buf, 64, L"%d%%", (int)(p->fOpacity * 100));
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | WS_VISIBLE | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_OPACITY_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hLbl);
  }
  y += lineH + gap;

  // Render Quality
  TrackControl(CreateLabel(hw, L"Render Quality:", x, y, lw, lineH, hFont));
  TrackControl(CreateSlider(hw, IDC_MW_RENDER_QUALITY, x + lw + 4, y, sliderW, lineH, 0, 100, (int)(p->m_fRenderQuality * 100)));
  swprintf(buf, 64, L"%.2f", p->m_fRenderQuality);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | WS_VISIBLE | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_QUALITY_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hLbl);
  }
  y += lineH + gap;

  TrackControl(CreateCheck(hw, L"Auto Quality", IDC_MW_QUALITY_AUTO, x, y, rw, lineH, hFont, p->bQualityAuto));
  y += lineH + gap + 4;

  // Time/Frame/FPS Factors
  TrackControl(CreateLabel(hw, L"Time Factor:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.2f", p->m_timeFactor);
  TrackControl(CreateEdit(hw, buf, IDC_MW_TIME_FACTOR, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap;

  TrackControl(CreateLabel(hw, L"Frame Factor:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.2f", p->m_frameFactor);
  TrackControl(CreateEdit(hw, buf, IDC_MW_FRAME_FACTOR, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap;

  TrackControl(CreateLabel(hw, L"FPS Factor:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.2f", p->m_fpsFactor);
  TrackControl(CreateEdit(hw, buf, IDC_MW_FPS_FACTOR, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap;

  // Vis Intensity/Shift/Version
  TrackControl(CreateLabel(hw, L"Vis Intensity:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.2f", p->m_VisIntensity);
  TrackControl(CreateEdit(hw, buf, IDC_MW_VIS_INTENSITY, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap;

  TrackControl(CreateLabel(hw, L"Vis Shift:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.2f", p->m_VisShift);
  TrackControl(CreateEdit(hw, buf, IDC_MW_VIS_SHIFT, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap;

  TrackControl(CreateLabel(hw, L"Vis Version:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.0f", p->m_VisVersion);
  TrackControl(CreateEdit(hw, buf, IDC_MW_VIS_VERSION, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap + 4;

  TrackControl(CreateBtn(hw, L"Reset", IDC_MW_RESET_VISUAL, x, y, MulDiv(80, lineH, 26), lineH, hFont));
  y += lineH + gap + 8;

  // GPU Protection section
  HFONT hFontBold = GetFontBold();
  TrackControl(CreateLabel(hw, L"GPU Protection", x, y, rw, lineH, hFontBold));
  y += lineH + 2;

  TrackControl(CreateLabel(hw, L"Max Instances:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%d", p->m_nMaxShapeInstances);
  TrackControl(CreateEdit(hw, buf, IDC_MW_GPU_MAX_INST, x + lw + 4, y, 60, lineH, hFont));
  TrackControl(CreateLabel(hw, L"(0=unlimited)", x + lw + 70, y, 100, lineH, hFont));
  y += lineH + gap;

  TrackControl(CreateCheck(hw, L"Scale Instances by Resolution", IDC_MW_GPU_SCALE_BY_RES, x, y, rw, lineH, hFont, p->m_bScaleInstancesByResolution));
  y += lineH + 2;

  TrackControl(CreateLabel(hw, L"Scale Base Width:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%d", p->m_nInstanceScaleBaseWidth);
  TrackControl(CreateEdit(hw, buf, IDC_MW_GPU_SCALE_BASE, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap;

  TrackControl(CreateCheck(hw, L"Skip Heavy Presets", IDC_MW_GPU_SKIP_HEAVY, x, y, rw, lineH, hFont, p->m_bSkipHeavyPresets));
  y += lineH + 2;

  TrackControl(CreateLabel(hw, L"Heavy Threshold:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%d", p->m_nHeavyPresetMaxInstances);
  TrackControl(CreateEdit(hw, buf, IDC_MW_GPU_HEAVY_THRESHOLD, x + lw + 4, y, 60, lineH, hFont));
  y += lineH + gap + 4;

  TrackControl(CreateCheck(hw, L"Enable VSync", IDC_MW_VSYNC_ENABLED, x, y, rw, lineH, hFont, p->m_bEnableVSync));
  y += lineH + gap;

  // FPS Cap combo
  TrackControl(CreateLabel(hw, L"FPS Cap:", x, y, lw, lineH, hFont));
  {
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
      x + lw + 4, y, 120, lineH * 8, hw,
      (HMENU)(INT_PTR)IDC_MW_FPS_CAP, GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    const wchar_t* fpsLabels[] = { L"30", L"60", L"90", L"120", L"144", L"240", L"360", L"720", L"Unlimited" };
    const int fpsValues[] = { 30, 60, 90, 120, 144, 240, 360, 720, 0 };
    int selIdx = 8;
    for (int i = 0; i < 9; i++) {
      SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)fpsLabels[i]);
      if (fpsValues[i] == p->m_max_fps_fs) selIdx = i;
    }
    SendMessage(hCombo, CB_SETCURSEL, selIdx, 0);
    TrackControl(hCombo);
  }
  y += lineH + gap + 4;

  TrackControl(CreateBtn(hw, L"Reload Preset", IDC_MW_GPU_RELOAD_PRESET, x, y, MulDiv(110, lineH, 26), lineH, hFont));
  TrackControl(CreateBtn(hw, L"Restart Render", IDC_MW_RESTART_RENDER, x + MulDiv(120, lineH, 26), y, MulDiv(120, lineH, 26), lineH, hFont));
}

//----------------------------------------------------------------------
// Slider handler
//----------------------------------------------------------------------

LRESULT VisualWindow::DoHScroll(HWND hWnd, int id, int pos) {
  switch (id) {
  case IDC_MW_OPACITY: {
    m_pEngine->fOpacity = pos / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%d%%", pos);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_OPACITY_LABEL), buf);
    HWND hw = m_pEngine->GetPluginWindow();
    if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
    break;
  }
  case IDC_MW_RENDER_QUALITY: {
    m_pEngine->m_fRenderQuality = pos / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%.2f", m_pEngine->m_fRenderQuality);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_QUALITY_LABEL), buf);
    HWND hw = m_pEngine->GetPluginWindow();
    if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
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

LRESULT VisualWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;

  if (id == IDC_MW_GPU_RELOAD_PRESET && code == BN_CLICKED) {
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
    SetChecked(IDC_MW_QUALITY_AUTO, false);
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
    SetChecked(IDC_MW_GPU_SCALE_BY_RES, false);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BASE), L"1920");
    SetChecked(IDC_MW_GPU_SKIP_HEAVY, false);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_HEAVY_THRESHOLD), L"4096");
    p->m_bEnableVSync = true;
    SetChecked(IDC_MW_VSYNC_ENABLED, true);
    // Apply side-effects
    HWND hw = p->GetPluginWindow();
    if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
    if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
    return 0;
  }

  // FPS Cap combo box
  if (id == IDC_MW_FPS_CAP && code == CBN_SELCHANGE) {
    int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
    const int fpsValues[] = { 30, 60, 90, 120, 144, 240, 360, 720, 0 };
    if (sel >= 0 && sel < 9)
      p->SetFPSCap(fpsValues[sel]);
    return 0;
  }

  // Checkbox handlers
  if (code == BN_CLICKED) {
    bool bChecked = IsChecked(id);
    switch (id) {
    case IDC_MW_QUALITY_AUTO:
      p->bQualityAuto = bChecked;
      { HWND hw = p->GetPluginWindow();
        if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0); }
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
    }
  }

  // EN_KILLFOCUS handlers for edit fields
  if (code == EN_KILLFOCUS) {
    wchar_t buf[64];
    GetDlgItemTextW(hWnd, id, buf, 64);
    switch (id) {
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
    }
  }

  return -1;
}

} // namespace mdrop
