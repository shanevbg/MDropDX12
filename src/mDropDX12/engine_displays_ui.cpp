/*
  DisplaysWindow — Spout / Displays window (ToolWindow subclass).
  Contains: Display Outputs list, Spout sender management, Video Input controls.
  Launched from "Spout / Displays..." button on the Settings General tab.
*/

#include "tool_window.h"
#include "engine.h"
#include "video_capture.h"
#include "engine_helpers.h"
#include "utility.h"
#include <commctrl.h>
#include <commdlg.h>

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

DisplaysWindow::DisplaysWindow(Engine* pEngine)
  : ToolWindow(pEngine, 580, 800) {}

//----------------------------------------------------------------------
// Engine bridge: Open/Close via Engine members
//----------------------------------------------------------------------

void Engine::OpenDisplaysWindow() {
  if (!m_displaysWindow)
    m_displaysWindow = std::make_unique<DisplaysWindow>(this);
  m_displaysWindow->Open();
}

void Engine::CloseDisplaysWindow() {
  if (m_displaysWindow)
    m_displaysWindow->Close();
}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void DisplaysWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  // Create fonts from shared font size
  if (m_hFont) DeleteObject(m_hFont);
  m_hFont = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  if (m_hFontBold) DeleteObject(m_hFontBold);
  m_hFontBold = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  HFONT hFont = m_hFont;
  HFONT hFontBold = m_hFontBold;

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  int clientW = rcWnd.right;

  int lineH = GetLineHeight();
  int gap = 6, x = 16;
  int rw = clientW - x * 2;
  int y = 8;

  // Shorthand macro
  #define TC(expr) TrackControl(expr)

  // Font +/- buttons (top-left)
  {
    int btnW = lineH;
    TC(CreateBtn(hw, L"\u2212", IDC_MW_DISP_FONT_MINUS, x, y, btnW, lineH, hFont));
    TC(CreateBtn(hw, L"+", IDC_MW_DISP_FONT_PLUS, x + btnW + 4, y, btnW, lineH, hFont));
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
      (HMENU)(INT_PTR)IDC_MW_DISPLAYS_PIN, GetModuleHandle(NULL), NULL);
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
    TC(hPin);
  }

  y += lineH + gap + 4;

  Engine* p = m_pEngine;

  // ===== Display Outputs =====
  TC(CreateLabel(hw, L"Display Outputs", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // ListBox
  {
    int listH = lineH * 8;
    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
      x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_DISP_LIST, GetModuleHandle(NULL), NULL);
    if (hList && hFont) SendMessage(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
    TC(hList);
    y += listH + gap;
  }

  // Enable + Fullscreen checkboxes
  TC(CreateCheck(hw, L"Enabled", IDC_MW_DISP_ENABLE, x, y, rw / 2 - 4, lineH, hFont, false));
  TC(CreateCheck(hw, L"Fullscreen", IDC_MW_DISP_FULLSCREEN, x + rw / 2, y, rw / 2, lineH, hFont, false));
  y += lineH + gap;

  // Spout settings
  TC(CreateLabel(hw, L"Sender Name:", x, y, 100, lineH, hFont));
  TC(CreateEdit(hw, L"MDropDX12", IDC_MW_DISP_SPOUT_NAME, x + 104, y, rw - 104, lineH, hFont));
  y += lineH + 2;
  TC(CreateCheck(hw, L"Fixed Size", IDC_MW_DISP_SPOUT_FIXED, x, y, rw / 2 - 4, lineH, hFont, false));
  y += lineH + 2;
  TC(CreateLabel(hw, L"Width:", x, y, 50, lineH, hFont));
  TC(CreateEdit(hw, L"1920", IDC_MW_DISP_SPOUT_W, x + 54, y, 70, lineH, hFont));
  TC(CreateLabel(hw, L"Height:", x + 140, y, 50, lineH, hFont));
  TC(CreateEdit(hw, L"1080", IDC_MW_DISP_SPOUT_H, x + 194, y, 70, lineH, hFont));
  y += lineH + gap + 4;

  // Buttons: Add Spout, Remove, Refresh
  {
    int btnW = MulDiv(100, lineH, 26);
    int btnGap = 8;
    TC(CreateBtn(hw, L"Add Spout", IDC_MW_DISP_ADD_SPOUT, x, y, btnW, lineH, hFont));
    TC(CreateBtn(hw, L"Remove", IDC_MW_DISP_REMOVE, x + btnW + btnGap, y, btnW, lineH, hFont));
    TC(CreateBtn(hw, L"Refresh", IDC_MW_DISP_REFRESH, x + 2 * (btnW + btnGap), y, btnW, lineH, hFont));
    y += lineH + gap + 4;

    TC(CreateBtn(hw, p->m_bMirrorsActive ? L"Deactivate Mirrors" : L"Activate Mirrors",
      IDC_MW_DISP_ACTIVATE, x, y, rw, lineH, hFont));
    y += lineH + gap;

    // Click-through + opacity
    TC(CreateCheck(hw, L"Click-through", IDC_MW_DISP_CLICKTHRU, x, y, rw / 2 - 4, lineH, hFont, false));
    {
      int opLblW = MulDiv(60, lineH, 26);
      int opEditW = MulDiv(50, lineH, 26);
      int opPctW = MulDiv(20, lineH, 26);
      int opX = x + rw / 2;
      TC(CreateLabel(hw, L"Opacity:", opX, y, opLblW, lineH, hFont));
      HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"100",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
        opX + opLblW + 2, y, opEditW, lineH, hw,
        (HMENU)(INT_PTR)IDC_MW_DISP_OPACITY, GetModuleHandle(NULL), NULL);
      if (hEdit && hFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
      TC(hEdit);
      HWND hSpin = CreateWindowExW(0, UPDOWN_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
        0, 0, 0, 0, hw,
        (HMENU)(INT_PTR)IDC_MW_DISP_OPACITY_SPIN, GetModuleHandle(NULL), NULL);
      if (hSpin) {
        SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)hEdit, 0);
        SendMessage(hSpin, UDM_SETRANGE32, 1, 100);
        SendMessage(hSpin, UDM_SETPOS32, 0, 100);
      }
      TC(hSpin);
      TC(CreateLabel(hw, L"%", opX + opLblW + 2 + opEditW + 2, y, opPctW, lineH, hFont));
    }
  }
  y += lineH + gap;
  TC(CreateCheck(hw, L"Use mirrors for ALT-S (instead of stretch)",
    IDC_MW_DISP_MIRROR_ALTS, x, y, rw, lineH, hFont, p->m_bMirrorModeForAltS));
  y += lineH + gap;
  TC(CreateCheck(hw, L"Don't ask when no mirrors are enabled (enable all automatically)",
    IDC_MW_DISP_MIRROR_NOPROMPT, x, y, rw, lineH, hFont, p->m_bMirrorPromptDisabled));
  y += lineH + gap + 8;

  // ===== Video Input =====
  TC(CreateLabel(hw, L"Video Input", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  {
    bool active = (p->m_nVideoInputSource != p->VID_SOURCE_NONE);
    int sLbl = MulDiv(70, lineH, 26);

    // Source selector combo
    TC(CreateLabel(hw, L"Source:", x, y, sLbl, lineH, hFont));
    HWND hSrc = CreateWindowExW(0, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
      x + sLbl + 4, y, rw - sLbl - 4, lineH * 6, hw,
      (HMENU)(INT_PTR)IDC_MW_VIDINPUT_SOURCE, GetModuleHandle(NULL), NULL);
    if (hSrc && hFont) SendMessage(hSrc, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hSrc, CB_ADDSTRING, 0, (LPARAM)L"None");
    SendMessageW(hSrc, CB_ADDSTRING, 0, (LPARAM)L"Spout");
    SendMessageW(hSrc, CB_ADDSTRING, 0, (LPARAM)L"Webcam");
    SendMessageW(hSrc, CB_ADDSTRING, 0, (LPARAM)L"Video File");
    SendMessage(hSrc, CB_SETCURSEL, p->m_nVideoInputSource, 0);
    TC(hSrc);
    y += lineH + gap;

    // Spout sender combo + Refresh
    int refreshW = MulDiv(72, lineH, 26);
    TC(CreateLabel(hw, L"Sender:", x, y, sLbl, lineH, hFont));
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
      x + sLbl + 4, y, rw - sLbl - 4 - refreshW - 8, lineH * 8, hw,
      (HMENU)(INT_PTR)IDC_MW_SPINPUT_SENDER, GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(Auto - first available)");
    std::vector<std::string> senders;
    p->EnumerateSpoutSenders(senders);
    int selIdx = 0;
    for (int i = 0; i < (int)senders.size(); i++) {
      wchar_t wName[256];
      MultiByteToWideChar(CP_ACP, 0, senders[i].c_str(), -1, wName, 256);
      SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)wName);
      if (p->m_szSpoutInputSender[0] && _wcsicmp(wName, p->m_szSpoutInputSender) == 0)
        selIdx = i + 1;
    }
    SendMessage(hCombo, CB_SETCURSEL, selIdx, 0);
    if (p->m_nVideoInputSource != p->VID_SOURCE_SPOUT) EnableWindow(hCombo, FALSE);
    TC(hCombo);
    TC(CreateBtn(hw, L"Refresh", IDC_MW_SPINPUT_REFRESH, x + rw - refreshW, y, refreshW, lineH, hFont));
    if (p->m_nVideoInputSource != p->VID_SOURCE_SPOUT)
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_REFRESH), FALSE);
    y += lineH + gap;

    // Webcam device combo + Refresh
    TC(CreateLabel(hw, L"Webcam:", x, y, sLbl, lineH, hFont));
    HWND hWebcam = CreateWindowExW(0, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
      x + sLbl + 4, y, rw - sLbl - 4 - refreshW - 8, lineH * 8, hw,
      (HMENU)(INT_PTR)IDC_MW_VIDINPUT_WEBCAM, GetModuleHandle(NULL), NULL);
    if (hWebcam && hFont) SendMessage(hWebcam, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hWebcam, CB_ADDSTRING, 0, (LPARAM)L"(Default)");
    {
      auto webcams = VideoCaptureSource::EnumerateWebcams();
      int wcSel = 0;
      for (int i = 0; i < (int)webcams.size(); i++) {
        SendMessageW(hWebcam, CB_ADDSTRING, 0, (LPARAM)webcams[i].name.c_str());
        if (p->m_szWebcamDevice[0] && _wcsicmp(webcams[i].name.c_str(), p->m_szWebcamDevice) == 0)
          wcSel = i + 1;
      }
      SendMessage(hWebcam, CB_SETCURSEL, wcSel, 0);
    }
    if (p->m_nVideoInputSource != p->VID_SOURCE_WEBCAM) EnableWindow(hWebcam, FALSE);
    TC(hWebcam);
    TC(CreateBtn(hw, L"Refresh", IDC_MW_VIDINPUT_WEBCAM_REF, x + rw - refreshW, y, refreshW, lineH, hFont));
    if (p->m_nVideoInputSource != p->VID_SOURCE_WEBCAM)
      EnableWindow(GetDlgItem(hw, IDC_MW_VIDINPUT_WEBCAM_REF), FALSE);
    y += lineH + gap;

    // Video file path + Browse
    int browseW = MulDiv(72, lineH, 26);
    TC(CreateLabel(hw, L"File:", x, y, sLbl, lineH, hFont));
    HWND hFileEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", p->m_szVideoFile,
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
      x + sLbl + 4, y, rw - sLbl - 4 - browseW - 8, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_VIDINPUT_FILE_EDIT, GetModuleHandle(NULL), NULL);
    if (hFileEdit && hFont) SendMessage(hFileEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    if (p->m_nVideoInputSource != p->VID_SOURCE_FILE) EnableWindow(hFileEdit, FALSE);
    TC(hFileEdit);
    TC(CreateBtn(hw, L"Browse...", IDC_MW_VIDINPUT_FILE_BROWSE, x + rw - browseW, y, browseW, lineH, hFont));
    if (p->m_nVideoInputSource != p->VID_SOURCE_FILE)
      EnableWindow(GetDlgItem(hw, IDC_MW_VIDINPUT_FILE_BROWSE), FALSE);
    y += lineH + gap;

    // Loop checkbox
    TC(CreateCheck(hw, L"Loop video", IDC_MW_VIDINPUT_FILE_LOOP, x, y, rw / 3, lineH, hFont, p->m_bVideoLoop));
    if (p->m_nVideoInputSource != p->VID_SOURCE_FILE)
      EnableWindow(GetDlgItem(hw, IDC_MW_VIDINPUT_FILE_LOOP), FALSE);
    y += lineH + gap;

    // Layer radio: Background / Overlay
    int layLbl = MulDiv(50, lineH, 26);
    int radioW = MulDiv(110, lineH, 26);
    TC(CreateLabel(hw, L"Layer:", x, y, layLbl, lineH, hFont));
    TC(CreateRadio(hw, L"Background", IDC_MW_SPINPUT_LAYER_BG, x + layLbl + 4, y, radioW, lineH, hFont, !p->m_bSpoutInputOnTop, true));
    TC(CreateRadio(hw, L"Overlay", IDC_MW_SPINPUT_LAYER_OV, x + layLbl + 4 + radioW + 4, y, radioW, lineH, hFont, p->m_bSpoutInputOnTop));
    if (!active) {
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LAYER_BG), FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LAYER_OV), FALSE);
    }
    y += lineH + gap;

    // Opacity slider
    int slLbl = MulDiv(80, lineH, 26);
    int valW = MulDiv(50, lineH, 26);
    TC(CreateLabel(hw, L"Opacity:", x, y, slLbl, lineH, hFont));
    TC(CreateSlider(hw, IDC_MW_SPINPUT_OPACITY, x + slLbl + 4, y, rw - slLbl - 4 - valW, lineH, 0, 100, (int)(p->m_fSpoutInputOpacity * 100)));
    { wchar_t buf[32]; swprintf(buf, 32, L"%d%%", (int)(p->m_fSpoutInputOpacity * 100));
    TC(CreateLabel(hw, buf, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_SPINPUT_OPACITY_LBL);
    if (!active) EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_OPACITY), FALSE);
    y += lineH + gap;

    // Luma Key checkbox
    TC(CreateCheck(hw, L"Luma Key", IDC_MW_SPINPUT_LUMAKEY, x, y, rw / 3, lineH, hFont, p->m_bSpoutInputLumaKey));
    if (!active) EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LUMAKEY), FALSE);
    y += lineH + gap;

    // Threshold slider
    int indent = MulDiv(16, lineH, 26);
    int slLbl2 = MulDiv(90, lineH, 26);
    TC(CreateLabel(hw, L"Threshold:", x + indent, y, slLbl2, lineH, hFont));
    TC(CreateSlider(hw, IDC_MW_SPINPUT_LUMA_THR, x + indent + slLbl2 + 4, y, rw - indent - slLbl2 - 4 - valW, lineH, 0, 100, (int)(p->m_fSpoutInputLumaThreshold * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%d%%", (int)(p->m_fSpoutInputLumaThreshold * 100));
    TC(CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_SPINPUT_LUMA_THR_LBL);
    if (!active || !p->m_bSpoutInputLumaKey)
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LUMA_THR), FALSE);
    y += lineH + gap;

    // Softness slider
    TC(CreateLabel(hw, L"Softness:", x + indent, y, slLbl2, lineH, hFont));
    TC(CreateSlider(hw, IDC_MW_SPINPUT_LUMA_SOFT, x + indent + slLbl2 + 4, y, rw - indent - slLbl2 - 4 - valW, lineH, 0, 100, (int)(p->m_fSpoutInputLumaSoftness * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%d%%", (int)(p->m_fSpoutInputLumaSoftness * 100));
    TC(CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_SPINPUT_LUMA_SOFT_LBL);
    if (!active || !p->m_bSpoutInputLumaKey)
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LUMA_SOFT), FALSE);
  }

  #undef TC

  // Populate display list
  p->RefreshDisplaysTab();
}

//----------------------------------------------------------------------
// Slider handling (WM_HSCROLL)
//----------------------------------------------------------------------

LRESULT DisplaysWindow::DoHScroll(HWND hWnd, int id, int pos) {
  Engine* p = m_pEngine;
  switch (id) {
  case IDC_MW_SPINPUT_OPACITY: {
    p->m_fSpoutInputOpacity = pos / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%d%%", pos);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_SPINPUT_OPACITY_LBL), buf);
    p->SaveSpoutInputSettings();
    return 0;
  }
  case IDC_MW_SPINPUT_LUMA_THR: {
    p->m_fSpoutInputLumaThreshold = pos / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%d%%", pos);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_SPINPUT_LUMA_THR_LBL), buf);
    p->SaveSpoutInputSettings();
    return 0;
  }
  case IDC_MW_SPINPUT_LUMA_SOFT: {
    p->m_fSpoutInputLumaSoftness = pos / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%d%%", pos);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_SPINPUT_LUMA_SOFT_LBL), buf);
    p->SaveSpoutInputSettings();
    return 0;
  }
  }
  return -1; // not handled
}

//----------------------------------------------------------------------
// Notifications (WM_NOTIFY)
//----------------------------------------------------------------------

LRESULT DisplaysWindow::DoNotify(HWND hWnd, NMHDR* pnm) {
  Engine* p = m_pEngine;
  if (pnm->idFrom == IDC_MW_DISP_OPACITY_SPIN && pnm->code == UDN_DELTAPOS) {
    NMUPDOWN* pud = (NMUPDOWN*)pnm;
    int newVal = pud->iPos + pud->iDelta;
    if (newVal < 1) newVal = 1;
    if (newVal > 100) newVal = 100;
    int sel = p->m_nDisplaysTabSel;
    if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
        p->m_displayOutputs[sel].config.type == DisplayOutputType::Monitor) {
      p->m_displayOutputs[sel].config.nOpacity = newVal;
      p->UpdateMirrorWindowStyles();
    }
    return 0;
  }
  return -1;
}

//----------------------------------------------------------------------
// Command handling (WM_COMMAND)
//----------------------------------------------------------------------

LRESULT DisplaysWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;
  HWND hw = p->GetPluginWindow();  // render window for PostMessage

  // ── Owner-draw BN_CLICKED handling ──
  if (code == BN_CLICKED) {
    HWND hCtrl = (HWND)lParam;
    bool bIsCheckbox = (bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox");
    bool bIsRadio = (bool)(intptr_t)GetPropW(hCtrl, L"IsRadio");
    bool bChecked = false;

    if (bIsRadio) {
      // Toggle radio group for layer radios
      static const int layerRadioIDs[] = { IDC_MW_SPINPUT_LAYER_BG, IDC_MW_SPINPUT_LAYER_OV };
      for (int rid : layerRadioIDs) {
        if (rid == id) {
          for (int gid : layerRadioIDs) {
            HWND hR = GetDlgItem(hWnd, gid);
            if (hR) {
              SetPropW(hR, L"Checked", (HANDLE)(intptr_t)(gid == id ? 1 : 0));
              InvalidateRect(hR, NULL, TRUE);
            }
          }
          break;
        }
      }
      bChecked = true;
    } else if (bIsCheckbox) {
      bool wasChecked = (bool)(intptr_t)GetPropW(hCtrl, L"Checked");
      bChecked = !wasChecked;
      SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(bChecked ? 1 : 0));
      InvalidateRect(hCtrl, NULL, TRUE);
    }

    switch (id) {
    // ── Display Outputs checkboxes ──
    case IDC_MW_DISP_ENABLE: {
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size()) {
        auto& out = p->m_displayOutputs[sel];
        out.config.bEnabled = bChecked;
        if (!bChecked && out.spoutState)
          p->DestroyDisplayOutput(out);
        if (out.config.type == DisplayOutputType::Spout) {
          bool isFirst = false;
          for (auto& o : p->m_displayOutputs) {
            if (o.config.type == DisplayOutputType::Spout) { isFirst = (&o == &out); break; }
          }
          if (isFirst) p->bSpoutOut = bChecked;
        }
        p->bSpoutChanged = true;
        if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
        p->RefreshDisplaysTab();
        HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
        if (hList) SendMessage(hList, LB_SETCURSEL, sel, 0);
      }
      return 0;
    }
    case IDC_MW_DISP_FULLSCREEN: {
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size()) {
        p->m_displayOutputs[sel].config.bFullscreen = bChecked;
        p->bSpoutChanged = true;
      }
      return 0;
    }
    case IDC_MW_DISP_SPOUT_FIXED: {
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Spout) {
        p->m_displayOutputs[sel].config.bFixedSize = bChecked;
        for (auto& o : p->m_displayOutputs) {
          if (o.config.type == DisplayOutputType::Spout) { p->bSpoutFixedSize = o.config.bFixedSize; break; }
        }
        p->bSpoutChanged = true;
        if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
      }
      return 0;
    }
    case IDC_MW_DISP_CLICKTHRU: {
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Monitor) {
        p->m_displayOutputs[sel].config.bClickThrough = bChecked;
        p->UpdateMirrorWindowStyles();
        p->SaveDisplayOutputSettings();
      }
      return 0;
    }
    case IDC_MW_DISP_MIRROR_ALTS:
      p->m_bMirrorModeForAltS = bChecked;
      p->SaveDisplayOutputSettings();
      return 0;
    case IDC_MW_DISP_MIRROR_NOPROMPT:
      p->m_bMirrorPromptDisabled = bChecked;
      p->SaveDisplayOutputSettings();
      return 0;
    // ── Video Input checkboxes ──
    case IDC_MW_SPINPUT_LUMAKEY: {
      p->m_bSpoutInputLumaKey = bChecked;
      bool lumaOn = (p->m_nVideoInputSource != p->VID_SOURCE_NONE) && bChecked;
      EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_LUMA_THR), lumaOn);
      EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_LUMA_SOFT), lumaOn);
      p->SaveSpoutInputSettings();
      return 0;
    }
    case IDC_MW_SPINPUT_LAYER_BG:
      if (bChecked) { p->m_bSpoutInputOnTop = false; p->SaveSpoutInputSettings(); }
      return 0;
    case IDC_MW_SPINPUT_LAYER_OV:
      if (bChecked) { p->m_bSpoutInputOnTop = true; p->SaveSpoutInputSettings(); }
      return 0;
    case IDC_MW_VIDINPUT_FILE_LOOP:
      p->m_bVideoLoop = bChecked;
      if (p->m_videoCapture) p->m_videoCapture->m_bLoop = bChecked;
      p->SaveSpoutInputSettings();
      return 0;
    }
  }

  // ── Displays listbox selection ──
  if (id == IDC_MW_DISP_LIST && code == LBN_SELCHANGE) {
    int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
    p->UpdateDisplaysTabSelection(sel);
    return 0;
  }

  // ── Edit control changes (apply on focus lost) ──
  if (code == EN_KILLFOCUS) {
    wchar_t buf[64];
    GetWindowTextW((HWND)lParam, buf, 64);
    switch (id) {
    case IDC_MW_DISP_SPOUT_NAME: {
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Spout) {
        wchar_t nbuf[128];
        GetWindowTextW((HWND)lParam, nbuf, 128);
        wcsncpy_s(p->m_displayOutputs[sel].config.szName, nbuf, _TRUNCATE);
        p->bSpoutChanged = true;
        p->RefreshDisplaysTab();
        HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
        if (hList) SendMessage(hList, LB_SETCURSEL, sel, 0);
      }
      return 0;
    }
    case IDC_MW_DISP_SPOUT_W: {
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Spout) {
        int w = _wtoi(buf);
        if (w < 64) w = 64; if (w > 7680) w = 7680;
        p->m_displayOutputs[sel].config.nWidth = w;
        for (auto& o : p->m_displayOutputs) {
          if (o.config.type == DisplayOutputType::Spout) { p->nSpoutFixedWidth = o.config.nWidth; break; }
        }
        p->bSpoutChanged = true;
      }
      return 0;
    }
    case IDC_MW_DISP_SPOUT_H: {
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Spout) {
        int h = _wtoi(buf);
        if (h < 64) h = 64; if (h > 4320) h = 4320;
        p->m_displayOutputs[sel].config.nHeight = h;
        for (auto& o : p->m_displayOutputs) {
          if (o.config.type == DisplayOutputType::Spout) { p->nSpoutFixedHeight = o.config.nHeight; break; }
          }
        p->bSpoutChanged = true;
      }
      return 0;
    }
    case IDC_MW_DISP_OPACITY: {
      int val = _wtoi(buf);
      if (val < 1) val = 1;
      if (val > 100) val = 100;
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Monitor) {
        p->m_displayOutputs[sel].config.nOpacity = val;
        p->UpdateMirrorWindowStyles();
        p->SaveDisplayOutputSettings();
      }
      return 0;
    }
    }
  }

  // ===== Activate Mirrors =====
  if (code == BN_CLICKED && id == IDC_MW_DISP_ACTIVATE) {
    p->m_bMirrorsActive = !p->m_bMirrorsActive;
    HWND hBtn = GetDlgItem(hWnd, IDC_MW_DISP_ACTIVATE);
    if (hBtn) SetWindowTextW(hBtn, p->m_bMirrorsActive ? L"Deactivate Mirrors" : L"Activate Mirrors");
    if (p->m_bMirrorsActive) {
      int nMirrors = 0;
      for (auto& o : p->m_displayOutputs)
        if (o.config.type == DisplayOutputType::Monitor && o.config.bEnabled)
          nMirrors++;
      if (nMirrors > 0) {
        wchar_t buf[128];
        swprintf(buf, 128, L"Mirror outputs active (%d)", nMirrors);
        p->AddNotification(buf);
      } else
        p->AddNotification(L"Mirror outputs active (no monitors configured)");
    } else {
      p->AddNotification(L"Mirror outputs disabled");
    }
    p->RefreshDisplaysTab();
    HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
    if (hList) SetFocus(hList);
    return 0;
  }

  // ===== Add Spout / Remove / Refresh =====
  if (code == BN_CLICKED && (id == IDC_MW_DISP_REFRESH || id == IDC_MW_DISP_ADD_SPOUT || id == IDC_MW_DISP_REMOVE)) {
    switch (id) {
    case IDC_MW_DISP_REFRESH:
      p->EnumerateDisplayOutputs();
      p->RefreshDisplaysTab();
      p->UpdateDisplaysTabSelection(-1);
      return 0;
    case IDC_MW_DISP_ADD_SPOUT: {
      DisplayOutput newSpout;
      newSpout.config.type = DisplayOutputType::Spout;
      newSpout.config.bEnabled = false;
      int idx = 1;
      for (auto& o : p->m_displayOutputs)
        if (o.config.type == DisplayOutputType::Spout) idx++;
      if (idx == 1)
        wcscpy_s(newSpout.config.szName, L"MDropDX12");
      else
        swprintf(newSpout.config.szName, 128, L"MDropDX12_%d", idx);
      p->m_displayOutputs.insert(p->m_displayOutputs.begin(), std::move(newSpout));
      p->bSpoutChanged = true;
      p->RefreshDisplaysTab();
      HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
      if (hList) SendMessage(hList, LB_SETCURSEL, 0, 0);
      p->UpdateDisplaysTabSelection(0);
      return 0;
    }
    case IDC_MW_DISP_REMOVE: {
      int sel = p->m_nDisplaysTabSel;
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size()) {
        if (p->m_displayOutputs[sel].config.type == DisplayOutputType::Spout) {
          p->DestroyDisplayOutput(p->m_displayOutputs[sel]);
          p->m_displayOutputs.erase(p->m_displayOutputs.begin() + sel);
          p->bSpoutChanged = true;
          if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
          p->RefreshDisplaysTab();
          p->UpdateDisplaysTabSelection(-1);
        }
      }
      return 0;
    }
    }
  }

  // ===== Video Input handlers =====

  if (code == CBN_SELCHANGE && id == IDC_MW_VIDINPUT_SOURCE) {
    int newSrc = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
    if (newSrc < 0) newSrc = 0;
    int oldSrc = p->m_nVideoInputSource;
    if (newSrc == oldSrc) return 0;

    if (oldSrc == p->VID_SOURCE_SPOUT) p->DestroySpoutInput();
    else if (oldSrc == p->VID_SOURCE_WEBCAM || oldSrc == p->VID_SOURCE_FILE) p->DestroyVideoCapture();

    p->m_nVideoInputSource = newSrc;
    p->m_bSpoutInputEnabled = (newSrc != p->VID_SOURCE_NONE);

    if (newSrc == p->VID_SOURCE_SPOUT) p->InitSpoutInput();
    else if (newSrc == p->VID_SOURCE_WEBCAM || newSrc == p->VID_SOURCE_FILE) p->InitVideoCapture();

    bool active = (newSrc != p->VID_SOURCE_NONE);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_SENDER), newSrc == p->VID_SOURCE_SPOUT);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_REFRESH), newSrc == p->VID_SOURCE_SPOUT);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_VIDINPUT_WEBCAM), newSrc == p->VID_SOURCE_WEBCAM);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_VIDINPUT_WEBCAM_REF), newSrc == p->VID_SOURCE_WEBCAM);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_VIDINPUT_FILE_EDIT), newSrc == p->VID_SOURCE_FILE);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_VIDINPUT_FILE_BROWSE), newSrc == p->VID_SOURCE_FILE);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_VIDINPUT_FILE_LOOP), newSrc == p->VID_SOURCE_FILE);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_LAYER_BG), active);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_LAYER_OV), active);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_OPACITY), active);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_LUMAKEY), active);
    bool lumaOn = active && p->m_bSpoutInputLumaKey;
    EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_LUMA_THR), lumaOn);
    EnableWindow(GetDlgItem(hWnd, IDC_MW_SPINPUT_LUMA_SOFT), lumaOn);

    p->SaveSpoutInputSettings();
    return 0;
  }

  if (code == CBN_SELCHANGE && id == IDC_MW_VIDINPUT_WEBCAM) {
    HWND hCombo = (HWND)lParam;
    int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (sel <= 0)
      p->m_szWebcamDevice[0] = L'\0';
    else
      SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)p->m_szWebcamDevice);
    if (p->m_nVideoInputSource == p->VID_SOURCE_WEBCAM) {
      p->DestroyVideoCapture();
      p->InitVideoCapture();
    }
    p->SaveSpoutInputSettings();
    return 0;
  }

  if (code == BN_CLICKED && id == IDC_MW_VIDINPUT_WEBCAM_REF) {
    HWND hCombo = GetDlgItem(hWnd, IDC_MW_VIDINPUT_WEBCAM);
    if (hCombo) {
      wchar_t curSel[256] = {};
      int idx = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
      if (idx > 0) SendMessageW(hCombo, CB_GETLBTEXT, idx, (LPARAM)curSel);
      SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
      SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(Default)");
      auto webcams = VideoCaptureSource::EnumerateWebcams();
      int newSel = 0;
      for (int i = 0; i < (int)webcams.size(); i++) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)webcams[i].name.c_str());
        if (curSel[0] && _wcsicmp(webcams[i].name.c_str(), curSel) == 0) newSel = i + 1;
      }
      SendMessage(hCombo, CB_SETCURSEL, newSel, 0);
    }
    return 0;
  }

  if (code == BN_CLICKED && id == IDC_MW_VIDINPUT_FILE_BROWSE) {
    wchar_t szFile[MAX_PATH] = {};
    wcscpy_s(szFile, p->m_szVideoFile);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Video Files (*.mp4;*.avi;*.wmv;*.mkv;*.mov)\0*.mp4;*.avi;*.wmv;*.mkv;*.mov\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
      wcscpy_s(p->m_szVideoFile, szFile);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIDINPUT_FILE_EDIT), szFile);
      if (p->m_nVideoInputSource == p->VID_SOURCE_FILE) {
        p->DestroyVideoCapture();
        p->InitVideoCapture();
      }
      p->SaveSpoutInputSettings();
    }
    return 0;
  }

  if (code == BN_CLICKED && id == IDC_MW_SPINPUT_REFRESH) {
    HWND hCombo = GetDlgItem(hWnd, IDC_MW_SPINPUT_SENDER);
    if (hCombo) {
      wchar_t curSel[256] = {};
      int idx = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
      if (idx > 0) SendMessageW(hCombo, CB_GETLBTEXT, idx, (LPARAM)curSel);
      SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
      SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(Auto - first available)");
      std::vector<std::string> senders;
      p->EnumerateSpoutSenders(senders);
      int newSel = 0;
      for (int i = 0; i < (int)senders.size(); i++) {
        wchar_t wName[256];
        MultiByteToWideChar(CP_ACP, 0, senders[i].c_str(), -1, wName, 256);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)wName);
        if (curSel[0] && _wcsicmp(wName, curSel) == 0) newSel = i + 1;
      }
      SendMessage(hCombo, CB_SETCURSEL, newSel, 0);
    }
    return 0;
  }

  if (code == CBN_SELCHANGE && id == IDC_MW_SPINPUT_SENDER) {
    HWND hCombo = (HWND)lParam;
    int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (sel <= 0)
      p->m_szSpoutInputSender[0] = L'\0';
    else
      SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)p->m_szSpoutInputSender);
    if (p->m_nVideoInputSource == p->VID_SOURCE_SPOUT) {
      p->DestroySpoutInput();
      p->InitSpoutInput();
    }
    p->SaveSpoutInputSettings();
    return 0;
  }

  return -1; // not handled
}

} // namespace mdrop
