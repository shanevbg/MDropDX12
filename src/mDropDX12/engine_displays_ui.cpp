/*
  DisplaysWindow — Spout / Displays window (ToolWindow subclass).
  Two tabs: Display Outputs (Spout sender management, mirrors) and Video Input.
  Launched from "Spout / Displays..." button on the Settings General tab.
*/

#include "tool_window.h"
#include "engine.h"
#include "kv_store.h"
#include "pipe_server.h"  // MW_FS_ENTER / MW_FS_EXIT
#include "video_capture.h"
#include "engine_helpers.h"
#include "utility.h"
#include "format_to.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>   // SHBrowseForFolderW for the per-display preset dir
#include <shellapi.h> // SHFileOperationW, via Engine::DeleteDisplayProfile
#include "config_store.h"

namespace mdrop {

extern Engine g_engine;

// Width of a label's own text in its own font, plus a small margin.
//
// Replaces the fixed pixel widths this page used to use -- MulDiv scaled the
// line HEIGHT and the font with DPI but the label widths were literals, so
// "Sender Name:", "Width:" and "Height:" were sized for a string they were not
// rendering and clipped as soon as the font grew.
static int LabelW(HWND hw, HFONT hFont, const wchar_t* text) {
  HDC hdc = GetDC(hw);
  HFONT old = (HFONT)SelectObject(hdc, hFont);
  SIZE sz = {};
  GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
  SelectObject(hdc, old);
  ReleaseDC(hw, hdc);
  return sz.cx + MulDiv(8, sz.cy, 16);
}

// One of the two per-display preset fields: a mode, or a path.
//
// CBS_DROPDOWN rather than CBS_DROPDOWNLIST, for two reasons. A path can never
// be in the list, so a locked list could not express one at all; and a field
// somebody can paste into should be a field they can type into -- offering
// only the browse button makes the keyboard the slow way to do it.
//
// The three list entries are the kDispMode* sentinels verbatim, so picking one
// and typing one produce the identical stored value and there is no second
// representation to keep in step. `h` is the DROPPED-DOWN height for a combo,
// not the closed height.
static HWND CreateModeCombo(HWND hParent, int id, int x, int y, int w,
                            int lineH, HFONT hFont) {
  HWND h = CreateCombo(hParent, id, x, y, w, lineH * 8, NULL,
    WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL);
  if (!h) return h;
  if (hFont) SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
  SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)kDispModeDefault);
  SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)kDispModeRandom);
  SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)kDispModeCurrent);
  SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)kDispModeBrowse);
  SendMessage(h, CB_LIMITTEXT, (WPARAM)(MAX_PATH - 1), 0);

  // The DROPPED list is wider than the combo, and that is the whole point.
  //
  // By default a combo's list is exactly as wide as the closed control, so an
  // entry holding a full path is cut off in the one place it most needs to be
  // readable. The answer to that had been to widen the WINDOW until the path
  // fitted -- which is a race no window wins, because a path has no length
  // limit, and it is how this page ended up hundreds of pixels wider than any
  // control on it needed. A wide drop-down costs nothing while it is closed.
  SendMessage(h, CB_SETDROPPEDWIDTH, (WPARAM)MulDiv(560, lineH, 26), 0);
  return h;
}

// Show `value` in a mode combo, selecting the list row when it names a mode.
//
// SetWindowTextW alone would display it correctly but leave CB_GETCURSEL at
// -1, so the drop-down would open with nothing highlighted -- the field would
// read <Random> while the list claimed no mode was chosen.
void SetModeComboValue(HWND hCombo, const wchar_t* value) {
  if (!hCombo) return;
  if (!value) value = L"";
  // CB_SETCURSEL for the list highlight, and SetWindowTextW for the text.
  //
  // CB_SETCURSEL alone is enough for a user looking at the screen: it does copy
  // the item's text into a CBS_DROPDOWN's edit control, as documented. What it
  // does NOT update is the window's cached caption, and GetWindowText against
  // another process returns that caption rather than sending WM_GETTEXT -- by
  // design, so a hung app cannot block the caller. So a combo set only by
  // CB_SETCURSEL reads back as an empty string to anything outside this
  // process, including the probe harness. Setting the text as well keeps the
  // two in step and costs nothing. It is also what the one other editable combo
  // in this codebase does (ModalDialog, tool_window.cpp).
  const int idx = (int)SendMessageW(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                    (LPARAM)value);
  SendMessage(hCombo, CB_SETCURSEL, (WPARAM)(idx >= 0 ? idx : -1), 0);
  SetWindowTextW(hCombo, value);
}

// Is the selected display the one the main window is on?
//
// That tile is labelled "MAIN WINDOW" in the panel, and selecting it edits the
// MAIN RENDER's preset settings rather than a child's. The controls are the
// same controls; only the store behind them changes -- m_primaryProfile
// instead of the display's own config, and nothing written to settings.ini.
static bool IsRenderHostDisplay(Engine* p, int sel) {
  if (!p) return false;
  // m_displayOutputsMutex: this runs on the Displays ToolWindow thread,
  // reading a live element of Engine::m_displayOutputs, which only the
  // render thread structurally mutates (#27). Recursive, so a caller
  // that already holds it (e.g. UpdateDisplaysTabSelection) is unaffected.
  std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
  if (sel < 0 || (size_t)sel >= p->m_displayOutputs.size()) return false;
  const auto& cfg = p->m_displayOutputs[(size_t)sel].config;
  if (cfg.type != DisplayOutputType::Monitor) return false;
  wchar_t renderDev[32] = {};
  if (!p->GetRenderMonitorDevice(renderDev) || !renderDev[0]) return false;
  return wcscmp(renderDev, cfg.szDeviceName) == 0;
}

// A section heading, NOT a BS_GROUPBOX.
//
// The claim this used to carry -- "BS_GROUPBOX is a button, so the dark theme's
// owner-draw path already covers it" -- is false, and the window showed it. A
// group box IS a button, but the dark theme strips visual styles from buttons
// so the parent can paint them, and a style-less group box falls back to the
// CLASSIC renderer: frame in COLOR_3DHILIGHT, and the strip behind its caption
// erased with COLOR_BTNFACE. Both are white. WM_DRAWITEM cannot rescue it
// either -- BS_GROUPBOX and BS_OWNERDRAW are both values in BS_TYPEMASK, so a
// control cannot be one and get the other.
//
// engine_remote_ui.cpp hit exactly this and settled it the same way; its
// comment records the same white band and white frame. Grouping is still what
// makes "does this affect the selected display or every display" answerable
// without reading the code -- a bold heading says it and renders correctly.
static HWND CreateSectionHeading(HWND hParent, const wchar_t* text, int id,
                                 int x, int y, int w, int h, HFONT hFontBold) {
  HWND g = CreateWindowExW(0, L"STATIC", text,
    WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h, hParent,
    (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (g && hFontBold) SendMessage(g, WM_SETFONT, (WPARAM)hFontBold, TRUE);
  return g;
}

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

DisplaysWindow::DisplaysWindow(Engine* pEngine)
  : ToolWindow(pEngine, 760, 1020) {}   // wider/taller since the outputs page gained per-display preset controls (forgejo#22)

//----------------------------------------------------------------------
// Build Controls — creates tab control, then delegates to page builders
//----------------------------------------------------------------------

void DisplaysWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  // Common: fonts, font +/- buttons, pin button
  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, clientW = L.clientW;

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  int clientH = rcWnd.bottom;

  // ── Tab control (base handles creation, subclass, dark theme) ──
  const wchar_t* tabNames[] = { L"Display Outputs", L"Video Input" };
  RECT rcTab = BuildTabControl(IDC_MW_DISP_TAB, tabNames, DISPLAYS_NUM_PAGES,
                                0, y, clientW, clientH - y);
  int tabTop = rcTab.top + 4;
  int tabRW = rcTab.right - rcTab.left - x;

  // Build both pages (controls created hidden by SelectInitialTab)
  BuildOutputsPage(rcTab.left + x, tabTop, tabRW, lineH, gap);
  BuildVideoInputPage(rcTab.left + x, tabTop, tabRW, lineH, gap);
  SelectInitialTab();

  // Populate display list
  ApplyDisplayViewMode();
  m_pEngine->RefreshDisplaysTab();

  // Sync the "Selected display" group to the selection, which nothing did.
  //
  // m_nDisplaysTabSel is an Engine member assigned in exactly one place --
  // UpdateDisplaysTabSelection itself -- so building the window never touched
  // it and never touched the controls either. Every control in that group was
  // therefore left as CreateEdit and friends had constructed it: enabled, and
  // showing its literal default. That is why a freshly opened window showed
  // Sender "MDropDX12", Width 1920 and Height 1080 with nothing selected, and
  // why the preset combos and their "..." buttons looked live while every
  // handler behind them, gated on haveSel, did nothing at all.
  //
  // Re-applying the stored index restores the row from earlier in the session;
  // -1 (the initial value) disables the whole group, which is the honest
  // answer when no display is selected.
  {
    const int sel = m_pEngine->m_nDisplaysTabSel;
    bool haveSel;
    {
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(m_pEngine->m_displayOutputsMutex);
      haveSel = sel >= 0 && sel < (int)m_pEngine->m_displayOutputs.size();
    }
    if (haveSel) {
      if (HWND hList = GetDlgItem(hw, IDC_MW_DISP_LIST))
        SendMessage(hList, LB_SETCURSEL, (WPARAM)sel, 0);
    }
    m_pEngine->UpdateDisplaysTabSelection(sel);
  }
  // A starting child changes the list without anyone clicking anything, and
  // nothing else was going to tell it. One second: the state it reports moves
  // in whole seconds anyway, because the parent polls its children at that
  // rate.
  SetTimer(hw, IDT_DISPLAYS_CHILD_POLL, 1000, NULL);
}

//----------------------------------------------------------------------
// Page 0: Display Outputs
//----------------------------------------------------------------------

void DisplaysWindow::BuildOutputsPage(int x, int y, int rw, int lineH, int gap) {
  HWND hw = m_hWnd;
  HFONT hFont = m_hFont;
  HFONT hFontBold = m_hFontBold;
  Engine* p = m_pEngine;

  // Track in both base (for dark theme + destroy) and page (for show/hide)
  #define PAGE_TC(page, expr) TrackPageControl(page, (expr))

  // Clamp the working width to what the window can actually show, BEFORE
  // anything is placed.
  //
  // The `rw` handed to this page comes from the tab control's rect and has
  // measured WIDER than the client area, which is how the old layout ended up
  // with "Height:" hanging off the right edge. This clamp already existed --
  // but halfway down the function, so the list box and the buttons above it
  // were still laid out against the unclamped value. A right-aligned button
  // put on that row therefore hung off the edge, which is exactly what
  // happened to Reset. Deriving the width once, first, means a control placed
  // anywhere on this page is visible by construction.
  {
    RECT rcClient;
    GetClientRect(hw, &rcClient);
    const int usable = (rcClient.right - x - MulDiv(6, lineH, 26));
    if (rw > usable) rw = usable;
  }

  PAGE_TC(0, CreateLabel(hw, L"Display Outputs", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // Two views of the same list: the arrangement, and the detail lines.
  {
    HWND hLay = CreateWindowExW(0, L"BUTTON", L"Layout",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
      x, y, MulDiv(84, lineH, 26), lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_DISP_VIEW_LAYOUT, GetModuleHandle(NULL), NULL);
    HWND hDet = CreateWindowExW(0, L"BUTTON", L"Details",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
      x + MulDiv(88, lineH, 26), y, MulDiv(90, lineH, 26), lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_DISP_VIEW_DETAILS, GetModuleHandle(NULL), NULL);
    if (hLay && hFont) SendMessage(hLay, WM_SETFONT, (WPARAM)hFont, TRUE);
    if (hDet && hFont) SendMessage(hDet, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_TC(0, hLay);
    PAGE_TC(0, hDet);
    y += lineH + gap;
  }

  // Both occupy the same rectangle; ApplyDisplayViewMode shows one of them.
  // The LIST BOX is created either way and stays the home of the selection --
  // the panel drives it rather than replacing it, so nothing below needs to
  // know which view is up.
  {
    int listH = lineH * 8;
    // WS_HSCROLL, with the extent set from the longest row in
    // RefreshDisplaysTab. The detail lines carry a device name AND the preset
    // the display is showing, and a preset name has no length limit worth
    // designing to -- so the window was made wider and wider to fit them,
    // which is how it ended up ~475px wider than any control on it needed.
    // Scrolling the list reaches the long ones without any of that.
    HWND hList = CreateListBox(hw, IDC_MW_DISP_LIST, x, y, rw, listH, hFont,
      WS_VSCROLL | WS_HSCROLL | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT);
    PAGE_TC(0, hList);

    m_layoutPanel.Create(hw, p, IDC_MW_DISP_PANEL, x, y, rw, listH);
    if (HWND hPanel = m_layoutPanel.GetHWND())
      PAGE_TC(0, hPanel);

    y += listH + gap;
  }

  // -- Add / Remove / Refresh: they act on the LIST, so they stay with it --
  {
    int btnGap = MulDiv(8, lineH, 26);
    // Sized from the text they carry, like the two on the right already were.
    // A flat 100 units gave "Remove" and "Refresh" room for a longer word than
    // either of them has, and this row is the widest thing on the page -- so
    // that slack was setting the window's minimum useful width for everything.
    const int padW = MulDiv(24, lineH, 26);
    const int addW = LabelW(hw, hFont, L"Add Spout") + padW;
    const int remW = LabelW(hw, hFont, L"Remove")    + padW;
    const int refW = LabelW(hw, hFont, L"Refresh")   + padW;
    PAGE_TC(0, CreateBtn(hw, L"Add Spout", IDC_MW_DISP_ADD_SPOUT, x, y, addW, lineH, hFont));
    PAGE_TC(0, CreateBtn(hw, L"Remove", IDC_MW_DISP_REMOVE, x + addW + btnGap, y, remW, lineH, hFont));
    PAGE_TC(0, CreateBtn(hw, L"Refresh", IDC_MW_DISP_REFRESH, x + addW + remW + 2 * btnGap, y, refW, lineH, hFont));

    // Reset lives here, at the far end of the row that acts on the list, rather
    // than down in Profiles. It was the last control in the window, and the
    // rows added above it walked it off the bottom edge -- twice. This row has
    // had spare width since it was written, and "clear the whole list" belongs
    // beside Add and Remove more than it belongs beside Save Profile anyway.
    // Measured, not guessed: "Reset Displays to Defaults" at 170 units came
    // back clipped at both ends. Shorter label, and the width taken from the
    // text it actually has to hold.
    const int resetW = LabelW(hw, hFont, L"Reset to defaults") +
                       MulDiv(24, lineH, 26);
    const int frontW = LabelW(hw, hFont, L"Bring to front") +
                       MulDiv(24, lineH, 26);
    PAGE_TC(0, CreateBtn(hw, L"Bring to front", IDC_MW_DISP_BRING_FRONT,
                         x + rw - resetW - btnGap - frontW, y, frontW, lineH,
                         hFont));
    PAGE_TC(0, CreateBtn(hw, L"Reset to defaults", IDC_MW_DISP_RESET,
                         x + rw - resetW, y, resetW, lineH, hFont));
    y += lineH + gap + MulDiv(6, lineH, 26);
  }

  const int pad  = MulDiv(10, lineH, 26);   // inset inside a group box
  const int gTop = lineH + MulDiv(4, lineH, 26);  // a heading, plus a little air
  const int row  = lineH + MulDiv(4, lineH, 26);
  const int gx   = x + pad;
  const int gw   = rw - 2 * pad;

  // ===== Group 1: the selected display ==================================
  //
  // Everything in this box applies to the row highlighted in the list above
  // and to nothing else. That distinction is the whole point of the boxes:
  // the flat column this replaces gave no clue which controls were
  // per-display and which were global.
  {
    const int gyTop = y;
    PAGE_TC(0, CreateSectionHeading(hw, L"Selected display", IDC_MW_DISP_GRP_SELECTED,
                                    x, gyTop, rw, lineH, hFontBold));
    int gy = y + gTop;

    PAGE_TC(0, CreateCheck(hw, L"Enabled", IDC_MW_DISP_ENABLE,
                           gx, gy, gw / 2 - 4, lineH, hFont, false));
    PAGE_TC(0, CreateCheck(hw, L"Fullscreen", IDC_MW_DISP_FULLSCREEN,
                           gx + gw / 2, gy, gw / 2, lineH, hFont, false));
    gy += row;

    // Render mode. Mutually exclusive by construction: a display either
    // mirrors the primary frame or holds a preset of its own. WS_GROUP on the
    // first button makes the pair one radio group.
    //
    // "Own preset", not "Own process". It read "Own process" while that was
    // literally what it did -- a second MDropDX12.exe -child per display --
    // and #186 phase 6 deleted them. The INI key and the IPC keyword keep the
    // old spelling on purpose (renaming the key would un-configure every
    // display that has it set, and a renamed verb fails silently), but the
    // control a user reads should name what it does.
    {
      int lw = LabelW(hw, hFont, L"Render:");
      PAGE_TC(0, CreateLabel(hw, L"Render:", gx, gy, lw, lineH, hFont));
      int rx = gx + lw + MulDiv(6, lineH, 26);
      int rW = MulDiv(90, lineH, 26);
      HWND hMir = CreateWindowExW(0, L"BUTTON", L"Mirror",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        rx, gy, rW, lineH, hw, (HMENU)(INT_PTR)IDC_MW_DISP_MODE_MIRROR,
        GetModuleHandle(NULL), NULL);
      // Three tiers, cheapest first, so the row reads as increasing cost.
      // "Own preset" used to be the only way to get a different preset on a
      // display and silently cost a whole second instance; it is now the
      // in-process one, and the process is a separate choice beside it.
      const int rW2 = MulDiv(112, lineH, 26);
      HWND hOwn = CreateWindowExW(0, L"BUTTON", L"Own preset",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
        rx + rW + MulDiv(6, lineH, 26), gy, rW2, lineH, hw,
        (HMENU)(INT_PTR)IDC_MW_DISP_MODE_PRESET, GetModuleHandle(NULL), NULL);
      HWND hChi = CreateWindowExW(0, L"BUTTON", L"+ own process",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
        rx + rW + rW2 + MulDiv(12, lineH, 26), gy, MulDiv(150, lineH, 26),
        lineH, hw,
        (HMENU)(INT_PTR)IDC_MW_DISP_MODE_CHILD, GetModuleHandle(NULL), NULL);
      if (hMir && hFont) SendMessage(hMir, WM_SETFONT, (WPARAM)hFont, TRUE);
      if (hOwn && hFont) SendMessage(hOwn, WM_SETFONT, (WPARAM)hFont, TRUE);
      if (hChi && hFont) SendMessage(hChi, WM_SETFONT, (WPARAM)hFont, TRUE);
      PAGE_TC(0, hMir);
      PAGE_TC(0, hOwn);
      PAGE_TC(0, hChi);
      gy += row;
    }

    // Opacity + click-through (monitors)
    {
      int lw = LabelW(hw, hFont, L"Opacity:");
      int edW = MulDiv(60, lineH, 26);
      PAGE_TC(0, CreateLabel(hw, L"Opacity:", gx, gy, lw, lineH, hFont));
      HWND hOp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"100",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
        gx + lw + 2, gy, edW, lineH, hw,
        (HMENU)(INT_PTR)IDC_MW_DISP_OPACITY, GetModuleHandle(NULL), NULL);
      if (hOp && hFont) SendMessage(hOp, WM_SETFONT, (WPARAM)hFont, TRUE);
      PAGE_TC(0, hOp);
      HWND hSpin = CreateWindowExW(0, UPDOWN_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
        0, 0, 0, 0, hw, (HMENU)(INT_PTR)IDC_MW_DISP_OPACITY_SPIN,
        GetModuleHandle(NULL), NULL);
      if (hSpin) {
        SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)hOp, 0);
        SendMessage(hSpin, UDM_SETRANGE32, 1, 100);
        SendMessage(hSpin, UDM_SETPOS32, 0, 100);
      }
      PAGE_TC(0, hSpin);
      PAGE_TC(0, CreateLabel(hw, L"%", gx + lw + 2 + edW + 2, gy,
                             LabelW(hw, hFont, L"%"), lineH, hFont));
      PAGE_TC(0, CreateCheck(hw, L"Click-through", IDC_MW_DISP_CLICKTHRU,
                             gx + gw / 2, gy, gw / 2, lineH, hFont, false));
      gy += row;
      // Per-display watermark, in the "Selected display" group with the
      // settings it composes with. The "Mirror Watermark" button elsewhere on
      // this page is the every-display gesture; this is the state it sets, and
      // until now that state did not exist to be set on one display.
      PAGE_TC(0, CreateCheck(hw, L"Watermark this display", IDC_MW_DISP_WATERMARK,
                             gx, gy, gw / 2, lineH, hFont, false));
      // Beside watermark, because it is the piece of watermark someone often
      // wants on its own (#276). Click-through with nothing on top is close to
      // useless -- the window cannot be clicked, so nothing will ever raise it
      // and the next thing opened on that monitor buries it for good.
      PAGE_TC(0, CreateCheck(hw, L"Keep on top", IDC_MW_DISP_ONTOP,
                             gx + gw / 2, gy, gw / 2, lineH, hFont, false));
      gy += row;
    }

    // Spout sender name / fixed size (Spout outputs)
    {
      int lw = LabelW(hw, hFont, L"Sender:");
      PAGE_TC(0, CreateLabel(hw, L"Sender:", gx, gy, lw, lineH, hFont));
      PAGE_TC(0, CreateEdit(hw, L"MDropDX12", IDC_MW_DISP_SPOUT_NAME,
                            gx + lw + 4, gy, gw - lw - 4, lineH, hFont));
      gy += row;

      PAGE_TC(0, CreateCheck(hw, L"Fixed Size", IDC_MW_DISP_SPOUT_FIXED,
                             gx, gy, gw / 2 - 4, lineH, hFont, false));
      int wlw = LabelW(hw, hFont, L"Width:");
      int hlw = LabelW(hw, hFont, L"Height:");
      int edW = MulDiv(64, lineH, 26);
      int wx = gx + gw / 2;
      PAGE_TC(0, CreateLabel(hw, L"Width:", wx, gy, wlw, lineH, hFont));
      PAGE_TC(0, CreateEdit(hw, L"1920", IDC_MW_DISP_SPOUT_W,
                            wx + wlw + 2, gy, edW, lineH, hFont));
      int hx = wx + wlw + 2 + edW + MulDiv(10, lineH, 26);
      PAGE_TC(0, CreateLabel(hw, L"Height:", hx, gy, hlw, lineH, hFont));
      PAGE_TC(0, CreateEdit(hw, L"1080", IDC_MW_DISP_SPOUT_H,
                            hx + hlw + 2, gy, edW, lineH, hFont));
      gy += row;
    }

    // Own-process settings: preset directory, startup preset, cycle.
    {
      int lw = LabelW(hw, hFont, L"Preset dir:");
      int slw = LabelW(hw, hFont, L"Startup:");
      if (slw > lw) lw = slw;
      int brW = MulDiv(30, lineH, 26);

      PAGE_TC(0, CreateLabel(hw, L"Preset dir:", gx, gy, lw, lineH, hFont));
      PAGE_TC(0, CreateModeCombo(hw, IDC_MW_DISP_PRESETDIR,
                                 gx + lw + 4, gy, gw - lw - 8 - brW, lineH, hFont));
      PAGE_TC(0, CreateBtn(hw, L"...", IDC_MW_DISP_PRESETDIR_BR,
                           gx + gw - brW, gy, brW, lineH, hFont));
      gy += row;

      PAGE_TC(0, CreateLabel(hw, L"Startup:", gx, gy, lw, lineH, hFont));
      PAGE_TC(0, CreateModeCombo(hw, IDC_MW_DISP_STARTUP,
                                 gx + lw + 4, gy, gw - lw - 8 - brW, lineH, hFont));
      PAGE_TC(0, CreateBtn(hw, L"...", IDC_MW_DISP_STARTUP_BR,
                           gx + gw - brW, gy, brW, lineH, hFont));
      gy += row;

      int cw = LabelW(hw, hFont, L"Cycle every");
      int sw = LabelW(hw, hFont, L"seconds");
      int edW = MulDiv(50, lineH, 26);
      PAGE_TC(0, CreateCheck(hw, L"Cycle every", IDC_MW_DISP_CYCLE_ON,
                             gx, gy, cw + MulDiv(22, lineH, 26), lineH, hFont, false));
      int ex = gx + cw + MulDiv(26, lineH, 26);
      PAGE_TC(0, CreateEdit(hw, L"30", IDC_MW_DISP_CYCLE_SECS,
                            ex, gy, edW, lineH, hFont, ES_NUMBER | ES_RIGHT));
      PAGE_TC(0, CreateLabel(hw, L"seconds", ex + edW + 4, gy, sw, lineH, hFont));

      int ox = gx + gw / 2 + MulDiv(20, lineH, 26);
      HWND hRnd = CreateWindowExW(0, L"BUTTON", L"Random",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        ox, gy, MulDiv(90, lineH, 26), lineH, hw,
        (HMENU)(INT_PTR)IDC_MW_DISP_ORDER_RANDOM, GetModuleHandle(NULL), NULL);
      HWND hSeq = CreateWindowExW(0, L"BUTTON", L"Sequential",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
        ox, gy + row, MulDiv(110, lineH, 26), lineH, hw,
        (HMENU)(INT_PTR)IDC_MW_DISP_ORDER_SEQ, GetModuleHandle(NULL), NULL);
      if (hRnd && hFont) SendMessage(hRnd, WM_SETFONT, (WPARAM)hFont, TRUE);
      if (hSeq && hFont) SendMessage(hSeq, WM_SETFONT, (WPARAM)hFont, TRUE);
      PAGE_TC(0, hRnd);
      PAGE_TC(0, hSeq);
      gy += row * 2;

      // Preset lock, three-valued. The main window's lock is global and every
      // display follows it; these pin an answer for this display instead.
      // WS_GROUP on the first starts a new radio group, so these do not join
      // the Random/Sequential pair above.
      {
        int lw2 = LabelW(hw, hFont, L"Preset lock");
        PAGE_TC(0, CreateLabel(hw, L"Preset lock", gx, gy, lw2, lineH, hFont));
        struct { const wchar_t* text; int id; int w; } kLock[] = {
          { L"Inherit",  IDC_MW_DISP_LOCK_INHERIT, 80  },
          { L"Unlocked", IDC_MW_DISP_LOCK_OFF,     92  },
          { L"Locked",   IDC_MW_DISP_LOCK_ON,      82  },
        };
        int lx = gx + lw2 + MulDiv(8, lineH, 26);
        for (int i = 0; i < 3; i++) {
          const int cwid = MulDiv(kLock[i].w, lineH, 26);
          HWND h = CreateWindowExW(0, L"BUTTON", kLock[i].text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON |
            (i == 0 ? WS_GROUP : 0),
            lx, gy, cwid, lineH, hw,
            (HMENU)(INT_PTR)kLock[i].id, GetModuleHandle(NULL), NULL);
          if (h && hFont) SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
          PAGE_TC(0, h);
          lx += cwid;
        }
        gy += row;
      }

      // Step THIS display's preset, and nothing else's.
      //
      // The main window's Next broadcasts to every child, which is right for a
      // global next and exactly wrong here: the point of a per-display preset
      // is choosing what is on one screen. Same addressed path DISPLAY_NEXT
      // takes over IPC, which is what the Android remote already drives.
      {
        int lw4 = LabelW(hw, hFont, L"Preset:");
        PAGE_TC(0, CreateLabel(hw, L"Preset:", gx, gy, lw4, lineH, hFont));
        const int pbW = MulDiv(86, lineH, 26);
        const int pbGap = MulDiv(6, lineH, 26);
        int px = gx + lw4 + MulDiv(8, lineH, 26);
        PAGE_TC(0, CreateBtn(hw, L"< Prev", IDC_MW_DISP_PRESET_PREV,
                             px, gy, pbW, lineH, hFont));
        PAGE_TC(0, CreateBtn(hw, L"Next >", IDC_MW_DISP_PRESET_NEXT,
                             px + pbW + pbGap, gy, pbW, lineH, hFont));
        gy += row;
      }
    }

    y = gy + gap + MulDiv(4, lineH, 26);
  }

  // ===== Group 2: every mirror =========================================
  {
    const int gyTop = y;
    PAGE_TC(0, CreateSectionHeading(hw, L"All mirrors", IDC_MW_DISP_GRP_MIRRORS,
                                    x, gyTop, rw, lineH, hFontBold));
    int gy = y + gTop;

    PAGE_TC(0, CreateBtn(hw, p->m_bMirrorsActive ? L"Deactivate Mirror/Children"
                                                 : L"Activate Mirror/Children",
                         IDC_MW_DISP_ACTIVATE, gx, gy, gw / 2 - 4, lineH, hFont));
    PAGE_TC(0, CreateBtn(hw, L"Mirror Watermark", IDC_MW_DISP_MIRROR_WM,
                         gx + gw / 2, gy, gw / 2, lineH, hFont));
    gy += row;

    // A drop list, not a checkbox: ALT-S now has three answers, and "load the
    // default profile" is not a shade of "use mirrors instead of stretch".
    {
      int lw3 = LabelW(hw, hFont, L"ALT-S hotkey:");
      PAGE_TC(0, CreateLabel(hw, L"ALT-S hotkey:", gx, gy, lw3, lineH, hFont));
      HWND hAltS = CreateCombo(hw, IDC_MW_DISP_MIRROR_ALTS, gx + lw3 + MulDiv(8, lineH,
        26), gy, MulDiv(190, lineH, 26), lineH * 6, hFont);
      SendMessageW(hAltS, CB_ADDSTRING, 0, (LPARAM)L"Stretch");
      SendMessageW(hAltS, CB_ADDSTRING, 0, (LPARAM)L"Mirror");
      SendMessageW(hAltS, CB_ADDSTRING, 0, (LPARAM)L"Load profile");
      SendMessage(hAltS, CB_SETCURSEL, p->m_nAltSMode, 0);
      PAGE_TC(0, hAltS);
    }
    gy += row;
    PAGE_TC(0, CreateCheck(hw, L"Don't ask when no mirrors are enabled (enable all automatically)",
                           IDC_MW_DISP_MIRROR_NOPROMPT, gx, gy, gw, lineH, hFont,
                           p->m_bMirrorPromptDisabled));
    gy += row;
    PAGE_TC(0, CreateCheck(hw, L"Independent render by default (per-orientation re-draw)",
                           IDC_MW_DISP_INDEPENDENT, gx, gy, gw, lineH, hFont,
                           p->m_bMirrorIndependentDefault));
    gy += row;

    // Independent mirror FPS. "Match primary" steps the mirror sim once per
    // primary frame -- the mirror integrates its feedback per sim frame, so an
    // equal rate is what makes its motion run at the same SPEED as the window
    // (2026-08-23). A cap above the primary rate makes the mirrors move
    // faster, not smoother: the render thread owns the panel blits, so what a
    // panel SHOWS can never exceed the primary fps.
    {
      int ind = MulDiv(20, lineH, 26);
      int lw = LabelW(hw, hFont, L"FPS:");
      int cbW = MulDiv(150, lineH, 26);
      PAGE_TC(0, CreateLabel(hw, L"FPS:", gx + ind, gy, lw, lineH, hFont));
      HWND hFps = CreateCombo(hw, IDC_MW_DISP_MAXFPS, gx + ind + lw + 4, gy, cbW,
        lineH * 6, hFont, CBS_DROPDOWNLIST);
      SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"Match primary");
      SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"Max 120 fps");
      SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"Max 60 fps");
      SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"Max 30 fps");
      int fpsSel;
      switch (p->m_nMirrorMaxFps.load()) {
        case 120: fpsSel = 1; break;
        case 60:  fpsSel = 2; break;
        case 30:  fpsSel = 3; break;
        default:  fpsSel = 0; break;
      }
      SendMessage(hFps, CB_SETCURSEL, fpsSel, 0);
      PAGE_TC(0, hFps);
      gy += row;
    }

    // Same flag as Settings -> System -> "Show HUD only on primary display".
    PAGE_TC(0, CreateCheck(hw, L"Show HUD only on primary display", IDC_MW_DISP_HUD_DISABLE,
                           gx, gy, gw, lineH, hFont, p->m_bDisableMirrorHud));
    gy += row;

    y = gy + gap + MulDiv(4, lineH, 26);
  }

  // ===== Group 3: profiles =============================================
  {
    const int gyTop = y;
    PAGE_TC(0, CreateSectionHeading(hw, L"Profiles", IDC_MW_DISP_GRP_PROFILES,
                                    x, gyTop, rw, lineH, hFontBold));
    int gy = y + gTop;

    // Sized from their labels, like the list row above. A flat 120 units made
    // this the widest row on the page and so the thing setting the window's
    // minimum width -- measured, it was the reason Reset produced a window
    // wider than anything on it needed.
    const int profPad = MulDiv(24, lineH, 26);
    int btnGap = MulDiv(8, lineH, 26);

    {
      // All four profile buttons share the startup checkbox's row.
      //
      // They were on a row of their own, and the rows this window gained --
      // the preset lock, the per-display Prev/Next, the Layout/Details pair --
      // pushed that row off the bottom edge. The window's height is not the
      // thing to change: it is already tall, and every row added later would
      // push it again. These three are one group of related actions and read
      // fine side by side.
      //
      // "Set as Default" writes the shown profile out as THE profile -- what
      // ALT-S loads in profile mode, what the next/prev hotkeys start from, and
      // what loads at startup if the box beside it is ticked. Same shape as the
      // Presets window's Save button, and for the same reason: ConfigStore
      // buffers writes, so without a button that flushes and says so there is
      // no way to tell it took.
      //
      // Four buttons now, so the two long labels lost the word the heading
      // directly above them already says. Widening the band instead would set
      // this window's minimum width, which is the fault recorded above.
      // Shaped like the PRESET startup row, which is what Shane asked for:
      // "profiles should be similar to presets with options of Profile to load
      // at start / None / Current / FileName, with a save button instead of
      // 'Load this profile at startup'" (#278).
      //
      // The checkbox it replaces could only say on/off, so the third state --
      // "start from whatever the displays are set to right now" -- had nowhere
      // to live, and the stored file name was the only answer the UI could
      // give. Save writes through for the same reason the preset one does:
      // ConfigStore buffers, so a choice alone gives no sign it took.
      const int lblW  = LabelW(hw, hFont, L"Startup:") + 4;
      const int saveW = LabelW(hw, hFont, L"Save") + profPad;
      PAGE_TC(0, CreateLabel(hw, L"Startup:", gx, gy, lblW, lineH, hFont));
      HWND hMode = CreateCombo(hw, IDC_MW_DISP_PROFILE_MODE, gx + lblW, gy,
                               gw - lblW - saveW - btnGap, lineH * 5, hFont);
      PAGE_TC(0, hMode);
      SendMessageW(hMode, CB_ADDSTRING, 0, (LPARAM)L"None");
      SendMessageW(hMode, CB_ADDSTRING, 0, (LPARAM)L"Current");
      {
        const wchar_t* leaf = FileNameOnly(p->m_szStartupDisplayProfile);
        SendMessageW(hMode, CB_ADDSTRING, 0,
                     (LPARAM)((leaf && *leaf) ? leaf : L"(no saved profile)"));
      }
      SendMessage(hMode, CB_SETCURSEL, p->DisplayProfileStartupMode(), 0);
      PAGE_TC(0, CreateBtn(hw, L"Save", IDC_MW_DISP_PROFILE_MODESAVE,
                           gx + gw - saveW, gy, saveW, lineH, hFont));
    }
    gy += row;
    PAGE_TC(0, CreateCheck(hw, L"Autostart all displays when a profile loads",
                           IDC_MW_DISP_PROFILE_AUTOSTART, gx, gy, gw, lineH,
                           hFont, p->m_bProfileAutostartAll));
    gy += row;
    // The profile NAME, not the path it happens to live at. The path is always
    // resources/profiles/display/, which the row repeats for every profile and
    // tells nobody anything -- and it is long enough that the name, the only
    // part that differs, was the part pushed off the end.
    PAGE_TC(0, CreateReadonlyField(hw, FileNameOnly(p->m_szStartupDisplayProfile),
                          IDC_MW_DISP_PROFILE_PATH, gx, gy, gw, lineH, hFont));
    gy += row;

    // The four actions, at the BOTTOM of the group rather than crowded onto
    // the startup row -- Shane: "put the buttons at the bottom of the window".
    // They act on profiles as files; the row above says which one starts.
    {
      const int profPad2 = MulDiv(18, lineH, 26);
      const int btnGap2  = MulDiv(6, lineH, 26);
      const int sW = LabelW(hw, hFont, L"Save...")     + profPad2;
      const int lW = LabelW(hw, hFont, L"Load...")     + profPad2;
      const int dW = LabelW(hw, hFont, L"Delete...")   + profPad2;
      const int fW = LabelW(hw, hFont, L"Set Default") + profPad2;
      int bx = gx + gw - (sW + lW + dW + fW + btnGap2 * 3);
      PAGE_TC(0, CreateBtn(hw, L"Save...", IDC_MW_DISP_SAVE_PROFILE,
                           bx, gy, sW, lineH, hFont));
      bx += sW + btnGap2;
      PAGE_TC(0, CreateBtn(hw, L"Load...", IDC_MW_DISP_LOAD_PROFILE,
                           bx, gy, lW, lineH, hFont));
      bx += lW + btnGap2;
      PAGE_TC(0, CreateBtn(hw, L"Delete...", IDC_MW_DISP_PROFILE_DELETE,
                           bx, gy, dW, lineH, hFont));
      bx += dW + btnGap2;
      PAGE_TC(0, CreateBtn(hw, L"Set Default", IDC_MW_DISP_PROFILE_DEFAULT,
                           bx, gy, fW, lineH, hFont));
    }
    gy += row;

    y = gy + gap;
  }

  #undef PAGE_TC
}

//----------------------------------------------------------------------
// Page 1: Video Input
//----------------------------------------------------------------------

void DisplaysWindow::BuildVideoInputPage(int x, int y, int rw, int lineH, int gap) {
  HWND hw = m_hWnd;
  HFONT hFont = m_hFont;
  HFONT hFontBold = m_hFontBold;
  Engine* p = m_pEngine;

  #define PAGE_TC(page, expr) TrackPageControl(page, (expr))

  PAGE_TC(1, CreateLabel(hw, L"Video Input", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  {
    bool active = (p->m_nVideoInputSource != p->VID_SOURCE_NONE);
    int sLbl = MulDiv(70, lineH, 26);

    // Source selector combo
    PAGE_TC(1, CreateLabel(hw, L"Source:", x, y, sLbl, lineH, hFont));
    HWND hSrc = CreateCombo(hw, IDC_MW_VIDINPUT_SOURCE, x + sLbl + 4, y, rw - sLbl - 4,
      lineH * 6, hFont);
    SendMessageW(hSrc, CB_ADDSTRING, 0, (LPARAM)L"None");
    SendMessageW(hSrc, CB_ADDSTRING, 0, (LPARAM)L"Spout");
    SendMessageW(hSrc, CB_ADDSTRING, 0, (LPARAM)L"Webcam");
    SendMessageW(hSrc, CB_ADDSTRING, 0, (LPARAM)L"Video File");
    SendMessage(hSrc, CB_SETCURSEL, p->m_nVideoInputSource, 0);
    PAGE_TC(1, hSrc);
    y += lineH + gap;

    // Spout sender combo + Refresh
    int refreshW = MulDiv(72, lineH, 26);
    PAGE_TC(1, CreateLabel(hw, L"Sender:", x, y, sLbl, lineH, hFont));
    HWND hSenderCombo = CreateCombo(hw, IDC_MW_SPINPUT_SENDER, x + sLbl + 4, y,
      rw - sLbl - 4 - refreshW - 8, lineH * 8, hFont);
    SendMessageW(hSenderCombo, CB_ADDSTRING, 0, (LPARAM)L"(Auto - first available)");
    std::vector<std::string> senders;
    p->EnumerateSpoutSenders(senders);
    int selIdx = 0;
    for (int i = 0; i < (int)senders.size(); i++) {
      wchar_t wName[256];
      MultiByteToWideChar(CP_ACP, 0, senders[i].c_str(), -1, wName, 256);
      SendMessageW(hSenderCombo, CB_ADDSTRING, 0, (LPARAM)wName);
      if (p->m_szSpoutInputSender[0] && _wcsicmp(wName, p->m_szSpoutInputSender) == 0)
        selIdx = i + 1;
    }
    SendMessage(hSenderCombo, CB_SETCURSEL, selIdx, 0);
    if (p->m_nVideoInputSource != p->VID_SOURCE_SPOUT) EnableWindow(hSenderCombo, FALSE);
    PAGE_TC(1, hSenderCombo);
    PAGE_TC(1, CreateBtn(hw, L"Refresh", IDC_MW_SPINPUT_REFRESH, x + rw - refreshW, y, refreshW, lineH, hFont));
    if (p->m_nVideoInputSource != p->VID_SOURCE_SPOUT)
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_REFRESH), FALSE);
    y += lineH + gap;

    // Webcam device combo + Refresh
    PAGE_TC(1, CreateLabel(hw, L"Webcam:", x, y, sLbl, lineH, hFont));
    HWND hWebcam = CreateCombo(hw, IDC_MW_VIDINPUT_WEBCAM, x + sLbl + 4, y,
      rw - sLbl - 4 - refreshW - 8, lineH * 8, hFont);
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
    PAGE_TC(1, hWebcam);
    PAGE_TC(1, CreateBtn(hw, L"Refresh", IDC_MW_VIDINPUT_WEBCAM_REF, x + rw - refreshW, y, refreshW, lineH, hFont));
    if (p->m_nVideoInputSource != p->VID_SOURCE_WEBCAM)
      EnableWindow(GetDlgItem(hw, IDC_MW_VIDINPUT_WEBCAM_REF), FALSE);
    y += lineH + gap;

    // Video file path + Browse
    int browseW = MulDiv(72, lineH, 26);
    PAGE_TC(1, CreateLabel(hw, L"File:", x, y, sLbl, lineH, hFont));
    HWND hFileEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", p->m_szVideoFile,
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
      x + sLbl + 4, y, rw - sLbl - 4 - browseW - 8, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_VIDINPUT_FILE_EDIT, GetModuleHandle(NULL), NULL);
    if (hFileEdit && hFont) SendMessage(hFileEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    if (p->m_nVideoInputSource != p->VID_SOURCE_FILE) EnableWindow(hFileEdit, FALSE);
    PAGE_TC(1, hFileEdit);
    PAGE_TC(1, CreateBtn(hw, L"Browse...", IDC_MW_VIDINPUT_FILE_BROWSE, x + rw - browseW, y, browseW, lineH, hFont));
    if (p->m_nVideoInputSource != p->VID_SOURCE_FILE)
      EnableWindow(GetDlgItem(hw, IDC_MW_VIDINPUT_FILE_BROWSE), FALSE);
    y += lineH + gap;

    // Loop checkbox
    PAGE_TC(1, CreateCheck(hw, L"Loop video", IDC_MW_VIDINPUT_FILE_LOOP, x, y, rw / 3, lineH, hFont, p->m_bVideoLoop));
    if (p->m_nVideoInputSource != p->VID_SOURCE_FILE)
      EnableWindow(GetDlgItem(hw, IDC_MW_VIDINPUT_FILE_LOOP), FALSE);
    y += lineH + gap;

    // Layer radio: Background / Overlay
    int layLbl = MulDiv(50, lineH, 26);
    int radioW = MulDiv(110, lineH, 26);
    PAGE_TC(1, CreateLabel(hw, L"Layer:", x, y, layLbl, lineH, hFont));
    PAGE_TC(1, CreateRadio(hw, L"Background", IDC_MW_SPINPUT_LAYER_BG, x + layLbl + 4, y, radioW, lineH, hFont, !p->m_bSpoutInputOnTop, true, true, IDC_MW_SPINPUT_LAYER_BG));
    PAGE_TC(1, CreateRadio(hw, L"Overlay", IDC_MW_SPINPUT_LAYER_OV, x + layLbl + 4 + radioW + 4, y, radioW, lineH, hFont, p->m_bSpoutInputOnTop, false, true, IDC_MW_SPINPUT_LAYER_BG));
    if (!active) {
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LAYER_BG), FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LAYER_OV), FALSE);
    }
    y += lineH + gap;

    // Opacity slider
    int slLbl = MulDiv(80, lineH, 26);
    int valW = MulDiv(50, lineH, 26);
    PAGE_TC(1, CreateLabel(hw, L"Opacity:", x, y, slLbl, lineH, hFont));
    PAGE_TC(1, CreateSlider(hw, IDC_MW_SPINPUT_OPACITY, x + slLbl + 4, y, rw - slLbl - 4 - valW, lineH, 0, 100, (int)(p->m_fSpoutInputOpacity * 100)));
    { wchar_t buf[32]; swprintf(buf, 32, L"%d%%", (int)(p->m_fSpoutInputOpacity * 100));
    PAGE_TC(1, CreateLabel(hw, buf, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_SPINPUT_OPACITY_LBL);
    if (!active) EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_OPACITY), FALSE);
    y += lineH + gap;

    // Luma Key checkbox
    PAGE_TC(1, CreateCheck(hw, L"Luma Key", IDC_MW_SPINPUT_LUMAKEY, x, y, rw / 3, lineH, hFont, p->m_bSpoutInputLumaKey));
    if (!active) EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LUMAKEY), FALSE);
    y += lineH + gap;

    // Threshold slider
    int indent = MulDiv(16, lineH, 26);
    int slLbl2 = MulDiv(90, lineH, 26);
    PAGE_TC(1, CreateLabel(hw, L"Threshold:", x + indent, y, slLbl2, lineH, hFont));
    PAGE_TC(1, CreateSlider(hw, IDC_MW_SPINPUT_LUMA_THR, x + indent + slLbl2 + 4, y, rw - indent - slLbl2 - 4 - valW, lineH, 0, 100, (int)(p->m_fSpoutInputLumaThreshold * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%d%%", (int)(p->m_fSpoutInputLumaThreshold * 100));
    PAGE_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_SPINPUT_LUMA_THR_LBL);
    if (!active || !p->m_bSpoutInputLumaKey)
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LUMA_THR), FALSE);
    y += lineH + gap;

    // Softness slider
    PAGE_TC(1, CreateLabel(hw, L"Softness:", x + indent, y, slLbl2, lineH, hFont));
    PAGE_TC(1, CreateSlider(hw, IDC_MW_SPINPUT_LUMA_SOFT, x + indent + slLbl2 + 4, y, rw - indent - slLbl2 - 4 - valW, lineH, 0, 100, (int)(p->m_fSpoutInputLumaSoftness * 100)));
    { wchar_t b[32]; swprintf(b, 32, L"%d%%", (int)(p->m_fSpoutInputLumaSoftness * 100));
    PAGE_TC(1, CreateLabel(hw, b, x + rw - valW, y, valW, lineH, hFont)); }
    SetWindowLongPtrW(m_childCtrls.back(), GWLP_ID, IDC_MW_SPINPUT_LUMA_SOFT_LBL);
    if (!active || !p->m_bSpoutInputLumaKey)
      EnableWindow(GetDlgItem(hw, IDC_MW_SPINPUT_LUMA_SOFT), FALSE);
    y += lineH + gap;

    // Effects... button
    int btnW = MulDiv(80, lineH, 26);
    PAGE_TC(1, CreateBtn(hw, L"Effects...", IDC_MW_OPEN_VFX, x, y, btnW, lineH, hFont));
    if (!active) EnableWindow(GetDlgItem(hw, IDC_MW_OPEN_VFX), FALSE);
  }

  #undef PAGE_TC
}

//----------------------------------------------------------------------
// Slider handling (WM_HSCROLL)
//----------------------------------------------------------------------

// Show whichever view is chosen, and remember the choice.
void DisplaysWindow::ApplyDisplayViewMode() {
    HWND hWnd = GetHWND();
    if (!hWnd) return;
    const bool layout = (m_pEngine->m_nDisplayViewMode == 0);
    if (HWND h = GetDlgItem(hWnd, IDC_MW_DISP_LIST))
        ShowWindow(h, layout ? SW_HIDE : SW_SHOW);
    if (HWND h = m_layoutPanel.GetHWND())
        ShowWindow(h, layout ? SW_SHOW : SW_HIDE);
    SetChecked(IDC_MW_DISP_VIEW_LAYOUT, layout);
    SetChecked(IDC_MW_DISP_VIEW_DETAILS, !layout);
}

void DisplaysWindow::OnPageShown(int page) {
    // Page 0 is the outputs page, where the layout panel and the detail list
    // share a rectangle. ShowPage has just shown both; this puts back the
    // choice between them, and the radio buttons that report it.
    if (page == 0) ApplyDisplayViewMode();
}

LRESULT DisplaysWindow::DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == IDT_DISPLAYS_CHILD_POLL) {
        // Only when it would say something different: RefreshDisplaysTab is a
        // full LB_RESETCONTENT and rebuild, and doing that every second flickers
        // for no reason on a window where nothing is usually changing.
        std::wstring sig = m_pEngine->ChildListSignature();
        if (sig != m_lastChildSig) {
            m_lastChildSig = sig;
            m_pEngine->RefreshDisplaysTab();
            m_layoutPanel.Invalidate();
        }
        return 0;
    }
    return -1;
}

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

  // Opacity spin control
  if (pnm->idFrom == IDC_MW_DISP_OPACITY_SPIN && pnm->code == UDN_DELTAPOS) {
    NMUPDOWN* pud = (NMUPDOWN*)pnm;
    int newVal = pud->iPos + pud->iDelta;
    if (newVal < 1) newVal = 1;
    if (newVal > 100) newVal = 100;
    int sel = p->m_nDisplaysTabSel;
    // m_displayOutputsMutex: #27.
    std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
    if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
        p->m_displayOutputs[sel].config.type == DisplayOutputType::Monitor) {
      // The SAME two things the edit box's handler does (#293).
      //
      // This wrote nOpacity and stopped: no watermark branch, so spinning the
      // arrows on a watermarked display moved a value the style pass ignores;
      // and no save, so the number lived in memory until the next thing to
      // write displays.json happened to carry it, or -- far more often -- was
      // simply replaced when the panel was repainted from config.
      //
      // Reported as "the opacity doesn't seem to update when spinner changes,
      // e.g. selected display1 changed opacity from 75 to 90 went to display2
      // came back to display1 and opacity was 75". 75 is what displays.json
      // held, which is the tell: the 90 never reached it.
      auto& cfg = p->m_displayOutputs[sel].config;
      if (cfg.bWatermark) cfg.nWatermarkOpacity = newVal;
      else                cfg.nOpacity = newVal;
      p->m_bMirrorStylesDirty.store(true);
      p->SaveDisplayOutputSettings();
    }
    return 0;
  }
  return -1;
}

//----------------------------------------------------------------------
// Command handling (WM_COMMAND)
//----------------------------------------------------------------------

// Commit whatever is being TYPED before the selected display changes (#301).
//
// Every edit box on this tab applies on EN_KILLFOCUS -- deliberately, so a
// half-typed number is not written on each keystroke. But clicking a display
// box in the layout panel does not move keyboard focus: the panel synthesises
// an LBN_SELCHANGE (display_panel.cpp) and the edit keeps the caret. So no
// EN_KILLFOCUS arrives, the typed value is never committed, and
// UpdateDisplaysTabSelection then repaints the box from config -- putting the
// OLD number back in front of the user.
//
// Reported for opacity: "click on display 1 change the opacity by hand to a
// number click on display 2 and come back it's the same opacity as before I
// changed it ... if I click off the window and back it seems to get fixed".
// Clicking off the window IS a focus change, which is why that worked, and why
// the spin control was unaffected -- it commits in its own handler (#293).
//
// Moving focus to the list rather than re-reading the text here: that fires the
// existing EN_KILLFOCUS handler, so every field commits through the one tested
// path and nothing has to know which fields exist. It runs while
// m_nDisplaysTabSel still names the OLD display, which is the display the typed
// value belongs to.
static void CommitFocusedEdit(HWND hWnd) {
  HWND hFocus = GetFocus();
  if (!hFocus || !IsChild(hWnd, hFocus)) return;
  wchar_t cls[32] = {};
  GetClassNameW(hFocus, cls, 32);
  if (_wcsicmp(cls, L"EDIT") != 0) return;
  HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
  if (hList && hList != hFocus) SetFocus(hList);
}

LRESULT DisplaysWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;
  HWND hw = p->GetPluginWindow();  // render window for PostMessage

  // ── Owner-draw BN_CLICKED handling ──
  // Checkbox and radio state is auto-toggled by the base class before DoCommand.
  if (code == BN_CLICKED) {
    bool bChecked = IsChecked(id);

    switch (id) {
    // ── Display Outputs checkboxes ──
    case IDC_MW_DISP_ENABLE: {
      int sel = p->m_nDisplaysTabSel;
      // m_displayOutputsMutex held for the whole block: `out` below is a
      // reference into the live vector, used across several calls including
      // ones (RefreshDisplaysTab, UpdateDisplaysTabSelection) that take the
      // same recursive lock themselves. #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size()) {
        auto& out = p->m_displayOutputs[sel];
        // Also stops/starts a live child, not just the flag -- see
        // SetDisplayEnabled's own comment.
        p->SetDisplayEnabled(out, bChecked);
        // Do NOT ShowWindow/Destroy from the UI thread — DXGI Present races with
        // ShowWindow on another thread and has TDRd the device after ~2 toggles.
        // Render thread parks/shows mirrors each frame from bEnabled alone.
        if (out.config.type == DisplayOutputType::Spout) {
          bool isFirst = false;
          for (auto& o : p->m_displayOutputs) {
            if (o.config.type == DisplayOutputType::Spout) { isFirst = (&o == &out); break; }
          }
          if (isFirst) p->bSpoutOut = bChecked;
          p->bSpoutChanged = true;
          if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
        } else if (out.config.type == DisplayOutputType::Monitor) {
          // Count how many non-primary monitors will still mirror
          int nMirror = 0;
          for (auto& o : p->m_displayOutputs) {
            if (o.config.type == DisplayOutputType::Monitor && o.config.bEnabled)
              nMirror++;
          }
          wchar_t buf[160];
          if (!bChecked)
            // ASCII only: HUD font atlas is 0x20-0xFF (em dash U+2014 is not covered)
            swprintf(buf, 160, L"Monitor off - %d enabled for mirrors", nMirror);
          else
            swprintf(buf, 160, L"Monitor on - %d enabled for mirrors", nMirror);
          p->AddNotification(buf);
          // Bring the newly shown mirror (and primary) to the front of Z-order so
          // enable is not confusing when windows sit behind other apps.
          if (bChecked && p->m_bMirrorsActive)
            p->m_bRaiseMirrorsNextFrame.store(true);
        }
        p->SaveDisplayOutputSettings();
        p->RefreshDisplaysTab();
        HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
        if (hList) {
          SendMessage(hList, LB_SETCURSEL, sel, 0);
          p->UpdateDisplaysTabSelection(sel);
        }
      }
      return 0;
    }
    case IDC_MW_DISP_FULLSCREEN: {
      int sel = p->m_nDisplaysTabSel;
      // m_displayOutputsMutex: `out` below is a reference into the live
      // vector, used across this whole block. #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
      if (sel < 0 || sel >= (int)p->m_displayOutputs.size())
        return 0;
      auto& out = p->m_displayOutputs[sel];
      if (out.config.type != DisplayOutputType::Monitor)
        return 0;

      out.config.bFullscreen = bChecked;
      p->SaveDisplayOutputSettings();

      // Is this the render window's monitor (primary)?
      bool isPrimary = false;
      if (hw) {
        HMONITOR hMon = MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = { sizeof(mi) };
        if (hMon && GetMonitorInfoW(hMon, &mi) &&
            wcscmp(mi.szDevice, out.config.szDeviceName) == 0)
          isPrimary = true;
      }

      if (isPrimary) {
        // Primary: enter/exit true fullscreen on the render HWND (not a no-op flag).
        // Absolute enter/exit, never a toggle: the checkbox knows the state
        // it wants (constants in pipe_server.h).
        if (hw)
          PostMessage(hw, WM_MW_FULLSCREEN,
                      bChecked ? MW_FS_ENTER : MW_FS_EXIT, 0);
      } else {
        // Mirror: apply full-monitor vs windowed layout on render thread.
        p->m_bMirrorStylesDirty.store(true);
        p->m_bRaiseMirrorsNextFrame.store(true);
      }
      return 0;
    }
    case IDC_MW_DISP_SPOUT_FIXED: {
      int sel = p->m_nDisplaysTabSel;
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
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
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Monitor) {
        p->m_displayOutputs[sel].config.bClickThrough = bChecked;
        p->m_bMirrorStylesDirty.store(true);
        p->SaveDisplayOutputSettings();
      }
      return 0;
    }
    case IDC_MW_DISP_ONTOP: {
      int sel = p->m_nDisplaysTabSel;
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Monitor) {
        auto& cfg = p->m_displayOutputs[sel].config;
        // The PRIMARY row means the render window, so it drives the global
        // (#338). That display is refused a mirror -- it IS the render window
        // -- so cfg.bAlwaysOnTop had nothing to act on: the box stored a value,
        // drew itself checked forever and no SetWindowPos ever ran.
        //
        // The global flag is the same request for that row, so it drives the
        // same value the Settings window's "Always On Top" drives rather than a
        // second copy that can disagree with it.
        wchar_t renderDev[32] = {};
        const bool isPrimary = p->GetRenderMonitorDevice(renderDev) &&
                               renderDev[0] &&
                               wcscmp(cfg.szDeviceName, renderDev) == 0;
        if (isPrimary) {
          // Exactly what the Settings window's own checkbox does, so the two
          // cannot drift: set the flag, persist it, and let the render window's
          // thread apply it. Never SetWindowPos on that HWND from here.
          p->m_bAlwaysOnTop = bChecked;
          p->SaveSettingToINI(SET_ALWAYS_ON_TOP);
          if (hw) PostMessage(hw, WM_MW_SET_ALWAYS_ON_TOP, 0, 0);
          return 0;
        }
        cfg.bAlwaysOnTop = bChecked;
        p->m_bMirrorStylesDirty.store(true);
        p->SaveDisplayOutputSettings();
        // A display running its own process has its OWN window, which the style
        // pass cannot reach -- it only walks this process's mirror windows. The
        // watermark checkbox below already relays for exactly this reason; this
        // one never did, so Keep on top was inert on a child-owned display and
        // toggling it off and on did not even fix it (#338).
        if (cfg.bOwnProcess)
          p->RequestChildSend(cfg.szDeviceName,
                              bChecked ? L"SET_ALWAYS_ON_TOP=1"
                                       : L"SET_ALWAYS_ON_TOP=0");
      }
      return 0;
    }
    case IDC_MW_DISP_WATERMARK: {
      int sel = p->m_nDisplaysTabSel;
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Monitor) {
        auto& cfg = p->m_displayOutputs[sel].config;
        cfg.bWatermark = bChecked;
        p->m_bMirrorStylesDirty.store(true);
        p->SaveDisplayOutputSettings();
        // A display running its own process has its OWN window, which the
        // style pass above cannot reach -- it only walks the parent's mirror
        // windows. Without this the checkbox would tick and nothing on that
        // monitor would change, which is how the watermark got stuck on a
        // child-owned display in the first place.
        if (cfg.bOwnProcess)
          p->RequestChildSend(cfg.szDeviceName,
                              bChecked ? L"SET_WATERMARK=1" : L"SET_WATERMARK=0");
        // The Opacity control now means a different value either side of this
        // toggle -- the watermark opacity while on, the normal one while off
        // (#276) -- so it has to be repainted or it keeps showing the one that
        // is no longer in force.
        p->UpdateDisplaysTabSelection(sel);
      }
      return 0;
    }
    case IDC_MW_DISP_MIRROR_ALTS:
      // A combo, so the interesting notification is CBN_SELCHANGE; the
      // checkbox auto-toggle the base class does for buttons does not apply.
      if (code == CBN_SELCHANGE) {
        const int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
        if (sel >= Engine::ALTS_STRETCH && sel <= Engine::ALTS_PROFILE) {
          p->m_nAltSMode = sel;
          p->SaveDisplayOutputSettings();
        }
      }
      return 0;
    case IDC_MW_DISP_MIRROR_NOPROMPT:
      p->m_bMirrorPromptDisabled = bChecked;
      p->SaveDisplayOutputSettings();
      return 0;
    case IDC_MW_DISP_HUD_DISABLE:
      p->m_bDisableMirrorHud = bChecked;
      Config().SetInt(L"Settings", L"bDisableMirrorHud", bChecked ? 1 : 0);
      // The twin of the System-tab box: same flag, so the same relay. Missing
      // it here would make the fix depend on which window it was toggled from.
      p->RelayHudStateToChildren();
      return 0;
    case IDC_MW_DISP_INDEPENDENT: {
      // Soft path switch only — never force-reinit SCs (that left mirrors black
      // until Ctrl+S / save UI recovered GPU state).
      p->SetMirrorIndependentRender(bChecked);
      return 0;
    }
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
    case IDC_MW_OPEN_VFX:
      p->OpenVideoEffectsWindow();
      return 0;
    }
  }

  // ── Displays listbox selection ──
  if (id == IDC_MW_DISP_LIST && code == LBN_SELCHANGE) {
    // BEFORE the selection moves: anything half-typed belongs to the display
    // being left, not the one being opened (#301). The layout panel routes its
    // clicks through here too, and those never move focus on their own.
    CommitFocusedEdit(hWnd);
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
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
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
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
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
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
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
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
      if (sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
          p->m_displayOutputs[sel].config.type == DisplayOutputType::Monitor) {
        auto& cfg = p->m_displayOutputs[sel].config;
        // While watermarked, this edits the WATERMARK opacity (#276). It used
        // to write nOpacity unconditionally, which the style pass then ignored
        // for as long as the watermark was on -- a control that moved and did
        // nothing. nOpacity still has to keep the normal-mode value, because
        // that is what the display returns to when the watermark comes off.
        if (cfg.bWatermark) cfg.nWatermarkOpacity = val;
        else                cfg.nOpacity = val;
        p->m_bMirrorStylesDirty.store(true);
        p->SaveDisplayOutputSettings();
      }
      return 0;
    }
    }
  }

  // ===== Mirror Watermark =====
  if (code == BN_CLICKED && id == IDC_MW_DISP_MIRROR_WM) {
    if (hw) PostMessage(hw, WM_MW_MIRROR_WM, 0, 0);
    return 0;
  }

  // ===== Activate Mirrors =====
  // Must run on the render-window path (WM_MW_TOGGLE_MIRROR_MODE): that also
  // fullscreens the primary. Flipping m_bMirrorsActive alone left the primary windowed.
  if (code == BN_CLICKED && id == IDC_MW_DISP_ACTIVATE) {
    if (hw)
      PostMessage(hw, WM_MW_TOGGLE_MIRROR_MODE, 0, 0);
    // Button label / list status refresh after App.cpp applies the toggle
    HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
    if (hList) SetFocus(hList);
    return 0;
  }

  // ===== Add Spout / Remove / Refresh =====
  // Through the render command queue, NOT directly -- all three structurally
  // mutate m_displayOutputs (insert/erase/full re-enumeration) while the
  // render thread walks the same vector every frame (#27). Any UI
  // feedback that only reflects a value already known here (not one read
  // from the vector) can still happen immediately below; the parts that
  // read the post-mutation list happen in the render-thread handler.
  if (code == BN_CLICKED && (id == IDC_MW_DISP_REFRESH || id == IDC_MW_DISP_ADD_SPOUT || id == IDC_MW_DISP_REMOVE)) {
    switch (id) {
    case IDC_MW_DISP_REFRESH:
      p->EnqueueRenderCmd(RenderCmd::RefreshDisplays);
      return 0;
    case IDC_MW_DISP_ADD_SPOUT:
      p->EnqueueRenderCmd(RenderCmd::AddSpoutOutput);
      return 0;
    case IDC_MW_DISP_REMOVE: {
      int sel = p->m_nDisplaysTabSel;
      bool removable;
      {
        // m_displayOutputsMutex: this is only a pre-check -- the render
        // thread re-validates the same condition under its own lock before
        // actually erasing, since the vector can change in the frame
        // between this enqueue and that command draining. #27.
        std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
        removable = sel >= 0 && sel < (int)p->m_displayOutputs.size() &&
                    p->m_displayOutputs[sel].config.type == DisplayOutputType::Spout;
      }
      if (removable) {
        RenderCommand rc;
        rc.cmd = RenderCmd::RemoveDisplayOutput;
        rc.iParam1 = sel;
        p->EnqueueRenderCmd(rc);
      }
      return 0;
    }
    }
  }

  // ===== Reset to Defaults =====
  if (code == BN_CLICKED && id == IDC_MW_DISP_RESET) {
    if (MessageBoxW(hWnd,
          L"Turn every display off, clear the per-display preset folders, "
          L"startup presets and cycle timers, remove all Spout senders, and "
          L"clear the mirror options?\n\n"
          L"The main window is not affected. Save a profile first if you want "
          L"this setup back.",
          L"Reset Displays to Defaults",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
      // Through the render command queue: ResetDisplayOutputsToDefaults()
      // rewrites m_displayOutputs and destroys every monitor mirror, which
      // must not run on this thread while the render thread walks the same
      // vector (#27). The group-box controls below reflect fixed
      // default values this button always resets TO, not anything read from
      // the vector, so they can still be set immediately.
      p->EnqueueRenderCmd(RenderCmd::ResetDisplayOutputs);
      // RefreshDisplaysTab rebuilds the list, not the group boxes below it, so
      // the mirror options would still show their old ticks over the reset
      // values -- the one thing a reset button must not do.
      // These are BS_OWNERDRAW buttons whose state lives in a "Checked" window
      // property, not in BM_SETCHECK -- CheckDlgButton on one changes nothing
      // it draws from. SetChecked writes the property and invalidates.
      for (int cid : { IDC_MW_DISP_MIRROR_NOPROMPT,
                       IDC_MW_DISP_INDEPENDENT, IDC_MW_DISP_HUD_DISABLE })
        SetChecked(cid, false);
      if (HWND hAltS = GetDlgItem(hWnd, IDC_MW_DISP_MIRROR_ALTS))
        SendMessage(hAltS, CB_SETCURSEL, Engine::ALTS_STRETCH, 0);
      if (HWND hFps = GetDlgItem(hWnd, IDC_MW_DISP_MAXFPS))
        SendMessage(hFps, CB_SETCURSEL, 0, 0);   // "Match primary"
      if (HWND hAct = GetDlgItem(hWnd, IDC_MW_DISP_ACTIVATE))
        SetWindowTextW(hAct, L"Activate Mirror/Children");
    }
    return 0;
  }

  if (code == BN_CLICKED && (id == IDC_MW_DISP_VIEW_LAYOUT ||
                             id == IDC_MW_DISP_VIEW_DETAILS)) {
    p->m_nDisplayViewMode = (id == IDC_MW_DISP_VIEW_LAYOUT) ? 0 : 1;
    DisplayCfg().SetInt(L"DisplayOutputs", L"ViewMode", p->m_nDisplayViewMode);
    ApplyDisplayViewMode();
    return 0;
  }

  if (code == BN_CLICKED && (id == IDC_MW_DISP_PRESET_PREV ||
                             id == IDC_MW_DISP_PRESET_NEXT)) {
    const int sel = (int)SendMessage(GetDlgItem(hWnd, IDC_MW_DISP_LIST),
                                     LB_GETCURSEL, 0, 0);
    // m_displayOutputsMutex: cfg below is a reference into the live vector,
    // used across this whole block. #27.
    std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
    if (sel < 0 || sel >= (int)p->m_displayOutputs.size()) {
      p->AddNotification((wchar_t*)L"Select a display first");
      return 0;
    }
    auto& cfg = p->m_displayOutputs[sel].config;
    if (cfg.type != DisplayOutputType::Monitor) return 0;
    const bool next = (id == IDC_MW_DISP_PRESET_NEXT);

    wchar_t renderDev[32];
    const bool isPrimary = p->GetRenderMonitorDevice(renderDev) &&
                           wcscmp(renderDev, cfg.szDeviceName) == 0;
    if (isPrimary) {
      // The LOCAL forms: NextPreset/PrevPreset broadcast to every child, which
      // is exactly wrong for a button that names one display.
      RenderCommand rc;
      rc.cmd = next ? RenderCmd::NextPresetLocal : RenderCmd::PrevPresetLocal;
      rc.fParam = p->m_fBlendTimeUser;
      p->EnqueueRenderCmd(rc);
    } else if (cfg.bOwnProcess) {
      p->RequestChildSend(cfg.szDeviceName,
                          next ? L"SIGNAL|NEXT_PRESET" : L"SIGNAL|PREV_PRESET");
    } else if (cfg.bOwnPreset) {
      // The IN-PROCESS own-preset tier. This arm was missing (#274): a display
      // set to "Own preset" but not "+ own process" fell through to the mirror
      // message below, so the button said it mirrored the main window and did
      // nothing at all.
      //
      // The IPC verbs had already been widened for exactly this -- their own
      // comment says asking the old question "told a display in `preset` mode
      // -- which has a preset of its own and a surface to step it on -- that it
      // was a mirror" -- and the button was left behind. Both now go through
      // StepDisplayOwnPreset so they cannot drift apart again.
      const wchar_t* why = nullptr;
      if (!p->StepDisplayOwnPreset(cfg, next, &why)) {
        p->AddNotification((wchar_t*)(
            wcscmp(why, L"no_surface") == 0
                ? L"That display has no render surface yet"
                : L"No presets in that display's preset folder"));
      }
    } else {
      // A mirror shows the main window's preset; there is nothing of its own
      // to step, and silently stepping the main window would be a surprise.
      p->AddNotification((wchar_t*)L"That display mirrors the main window");
    }
    return 0;
  }

  if (code == BN_CLICKED && id == IDC_MW_DISP_BRING_FRONT) {
    // Everything the app is drawing, back in front of whatever has buried it.
    //
    // A mirror is shown without activating and a child is raised exactly once,
    // both deliberately -- neither may steal the foreground, and pinning them
    // topmost was tried and was worse than the bug. The consequence is that
    // anything opened afterwards covers them, on a monitor the user expects to
    // be showing the visualizer, with no way to get them back. This is that
    // way: asked for, once, rather than taken.
    //
    // forgejo#97: this used to unconditionally set m_bRaiseMirrorsNextFrame and
    // report "Brought N displays to the front" -- which does nothing for a
    // mirror when m_bMirrorsActive is off (a SEPARATE flag from a display's own
    // Enabled checkbox, and the render loop's raise gate requires both), and
    // said so anyway. Confirmed live: mirrors on two real monitors had gone
    // inactive, toggling Enabled and pressing this button repeatedly did
    // nothing to them, with no error anywhere naming why. If mirrors are
    // configured (some non-primary, non-child display is enabled) but not
    // active, activate them instead of claiming a raise that cannot happen --
    // WM_MW_TOGGLE_MIRROR_MODE is the exact path SIGNAL|MIRROR and the
    // Activate button already use, posted rather than called so this UI
    // thread never touches the render thread's HWNDs directly.
    wchar_t renderDev[32] = {};
    const bool haveRenderDev = p->GetRenderMonitorDevice(renderDev);
    bool mirrorConfigured = false;
    int raised = 0;
    {
      // m_displayOutputsMutex: #27.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
      for (auto& out : p->m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        if (out.config.bOwnProcess || !out.config.bEnabled) continue;
        if (haveRenderDev && wcscmp(out.config.szDeviceName, renderDev) == 0) continue;
        mirrorConfigured = true;
        break;
      }

      for (auto& out : p->m_displayOutputs) {
        if (out.config.type != DisplayOutputType::Monitor) continue;
        if (!out.config.bOwnProcess) continue;
        // RAISE_WINDOW, not a SetWindowPos from here: the child's window belongs
        // to another process and only that process may reorder it safely.
        p->RequestChildSend(out.config.szDeviceName, L"RAISE_WINDOW");
        raised++;
      }
    }
    HWND hRender = p->GetPluginWindow();
    if (hRender)
      PostMessage(hRender, WM_MW_RAISE_WINDOW, 0, 0);

    if (mirrorConfigured && !p->m_bMirrorsActive) {
      if (hRender)
        PostMessage(hRender, WM_MW_TOGGLE_MIRROR_MODE, 0, 0);
      // Its own notification ("Mirror outputs starting..." / "...disabled" if
      // the user cancels a monitor-arrangement prompt mid-activation) replaces
      // this button's -- reporting "brought to front" here would still be the
      // same overclaim this fix exists to remove.
    } else {
      p->m_bRaiseMirrorsNextFrame.store(true);
      wchar_t note[96];
      FormatTo(note, L"Brought %d display%s to the front", raised + 1,
               raised == 0 ? L"" : L"s");
      p->AddNotification(note);
    }
    return 0;
  }

  if (code == BN_CLICKED && id == IDC_MW_DISP_PROFILE_DEFAULT) {
    if (!p->m_szStartupDisplayProfile[0]) {
      p->AddNotification((wchar_t*)L"No profile to set as default");
      return 0;
    }
    // ConfigWriteOverride: a write the user explicitly asked for, which testing
    // mode's shield would otherwise swallow while reporting success.
    {
      ConfigWriteOverride allow;
      Config().SetString(L"Settings", L"szStartupDisplayProfile",
                         p->m_szStartupDisplayProfile);
      Config().SetInt(L"Settings", L"bLoadDisplayProfileOnStart",
                      p->m_bLoadDisplayProfileOnStart);
      ConfigFlushAll();
    }
    p->AddNotification((wchar_t*)L"Default display profile set");
    return 0;
  }

  // ===== Delete Profile =====
  //
  // Picks the file rather than acting on the shown one: the shown profile is
  // whichever was last saved or loaded, so a "delete the current profile"
  // button deletes something the user did not choose, and cannot see the name
  // of until it is already gone. The engine does the deleting -- to the
  // Recycle Bin, and clearing the startup pointer if this was it.
  if (code == BN_CLICKED && id == IDC_MW_DISP_PROFILE_DELETE) {
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Display Profile (*.json)\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Delete Display Profile";
    const std::wstring profileDir = p->DisplayProfileDir();
    ofn.lpstrInitialDir = profileDir.c_str();
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
      return 0;

    // "normally", because FOF_ALLOWUNDO is a request and not a guarantee --
    // Windows deletes outright when the bin is off or full for that drive.
    // Same wording as the annotations window, for the same reason: the app
    // must not promise something it cannot know is true.
    const bool wasDefault =
        (_wcsicmp(p->m_szStartupDisplayProfile, filePath) == 0);
    wchar_t prompt[512];
    FormatTo(prompt,
        L"Delete the display profile \"%s\"?\n\n"
        L"It normally goes to the Recycle Bin, but Windows removes it "
        L"permanently if it is on a network drive or the bin is off or full "
        L"for that drive.%s",
        FileNameOnly(filePath),
        wasDefault ? L"\n\nThis is the profile set to load at startup, so "
                     L"nothing will load at startup until another is set."
                   : L"");
    if (MessageBoxW(hWnd, prompt, L"Delete Display Profile",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
      return 0;

    if (!p->DeleteDisplayProfile(filePath)) {
      MessageBoxW(hWnd, L"Failed to delete the display profile.", L"Error",
                  MB_OK | MB_ICONERROR);
      return 0;
    }
    if (wasDefault) {
      HWND hPath = GetDlgItem(hWnd, IDC_MW_DISP_PROFILE_PATH);
      if (hPath) SetTextIfChanged(hPath, L"");
      HWND hChk = GetDlgItem(hWnd, IDC_MW_DISP_PROFILE_ONSTART);
      if (hChk) SendMessage(hChk, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    wchar_t note[MAX_PATH + 32];
    FormatTo(note, L"Deleted profile %s", FileNameOnly(filePath));
    p->AddNotification(note);
    return 0;
  }

  // ===== Save / Load Profile =====
  if (code == BN_CLICKED && (id == IDC_MW_DISP_SAVE_PROFILE || id == IDC_MW_DISP_LOAD_PROFILE)) {
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Display Profile (*.json)\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    // Open where the profiles live. Both dialogs used to start wherever the
    // shell last left them, so a set of profiles meant for stepping through was
    // saved and reopened somewhere different each time -- one ended up in a
    // temp folder. Created first, or the dialog silently ignores a path that
    // does not exist yet and falls back to the last-used one.
    const std::wstring profileDir = p->DisplayProfileDir();
    {
      std::wstring res = profileDir.substr(0, profileDir.find_last_of(L"\\/",
                                                profileDir.size() - 2) + 1);
      CreateDirectoryW(res.c_str(), NULL);
      CreateDirectoryW(profileDir.c_str(), NULL);
    }
    ofn.lpstrInitialDir = profileDir.c_str();

    if (id == IDC_MW_DISP_SAVE_PROFILE) {
      ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
      ofn.lpstrDefExt = L"json";
      // Open with the name a snapshot would have used. The generator existed
      // already but only the snapshot action reached it, so this dialog opened
      // blank and a profile saved through it got whatever was quick to type --
      // "0901.json". ListDisplayProfiles sorts by name BECAUSE the name is a
      // timestamp, and StepDisplayProfile walks that order, so such a name does
      // not merely look untidy: it steps out of sequence with its neighbours.
      //
      // A suggestion, not a rule: it is selected in the dialog and typing
      // replaces it.
      wcsncpy_s(filePath, p->SuggestedDisplayProfileName().c_str(), _TRUNCATE);
      if (GetSaveFileNameW(&ofn)) {
        // Messages only. This used to govern the main window's preset settings
        // as well, and cannot any more: those are now controls in the "Main
        // render" group that the user has filled in deliberately, so answering
        // No would silently discard what they just typed. A setting with a
        // control of its own is saved because that control says so.
        //
        // The main-render block is written every time and costs nothing when
        // it is all "(default)": the inherit sentinels mean the profile
        // carries no opinion, and loading it leaves the globals in charge.
        // No prompt. Messages are stored, always (#279).
        //
        // It used to ask "Also store the custom messages in this profile?" and
        // Shane's verdict on it was "I forgot what this window really means
        // when I save a display profile" -- then, once the wording was fixed,
        // "just get rid of modal and assume yes". He is right that the better
        // fix was removing the question rather than explaining it: a profile is
        // a snapshot of how things are set up, messages are part of that, and
        // the choice was being put to the user on every single save for a
        // decision almost nobody makes differently twice.
        //
        // Nothing is lost by dropping Cancel with it -- the file dialog above
        // is already the place this is called off, and reaching this line means
        // the user has picked a name and pressed Save.
        {
          const bool withExtras = true;
          // Snapshot, always.
          //
          // This passed bSnapshot=false, so a profile stored each display's
          // PINNED startup preset rather than the one it was playing -- and
          // since nothing normally pins one, every load put back whatever had
          // been pinned when the profile was first made. Shane: "the save
          // profile isn't saving my presets for the displays ... instead it
          // seems to reload the presets from the original save I did as every
          // time, when I load a profile it should try to reload the presets on
          // all the displays."
          //
          // Saving a display profile from this window IS the "put these exact
          // presets back on these exact screens" case; the timestamped
          // snapshot hotkey already passed true for the same reason. The
          // question above only governs the main window and the messages.
          if (!p->SaveDisplayProfile(filePath, true, withExtras, true))
            MessageBoxW(hWnd, L"Failed to save display profile.", L"Error",
                        MB_OK | MB_ICONERROR);
          else {
            wcsncpy_s(p->m_szStartupDisplayProfile, filePath, _TRUNCATE);
            HWND hPath = GetDlgItem(hWnd, IDC_MW_DISP_PROFILE_PATH);
            if (hPath) SetTextIfChanged(hPath, FileNameOnly(filePath));
            Config().SetString(L"Settings", L"szStartupDisplayProfile",
                               p->m_szStartupDisplayProfile);
          }
        }
      }
    } else {
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
      if (GetOpenFileNameW(&ofn)) {
        // Through the render command queue, NOT directly. Loading rewrites
        // m_displayOutputs and spawns/retires child processes, and this runs on
        // the ToolWindow's own thread while the render thread walks the same
        // vector every frame (#27).
        RenderCommand rc;
        rc.cmd = RenderCmd::LoadDisplayProfile;
        rc.sParam = filePath;
        p->EnqueueRenderCmd(rc);
        wcsncpy_s(p->m_szStartupDisplayProfile, filePath, _TRUNCATE);
        HWND hPath = GetDlgItem(hWnd, IDC_MW_DISP_PROFILE_PATH);
        if (hPath) SetTextIfChanged(hPath, FileNameOnly(filePath));
        Config().SetString(L"Settings", L"szStartupDisplayProfile",
                           p->m_szStartupDisplayProfile);
        HWND hList = GetDlgItem(hWnd, IDC_MW_DISP_LIST);
        if (hList) SendMessage(hList, LB_SETCURSEL, -1, 0);
        // Tell the engine too. Clearing the list box alone left the "Selected
        // display" group enabled and showing the previous row's values, with
        // every handler behind it gated on a selection that no longer exists.
        p->UpdateDisplaysTabSelection(-1);
        p->UpdateDisplaysTabSelection(-1);
      }
    }
    return 0;
  }

  // ===== Per-display render mode, preset settings and cycle (forgejo#22) ====
  {
    const int sel = p->m_nDisplaysTabSel;
    bool haveSel;
    {
      // m_displayOutputsMutex: #27. Each branch below that actually
      // indexes m_displayOutputs[sel] takes its own (recursive) lock again
      // rather than trusting this alone, except the two Browse buttons,
      // which re-check sel explicitly after their modal dialog returns.
      std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
      haveSel = sel >= 0 && sel < (int)p->m_displayOutputs.size();
    }

    if (code == BN_CLICKED &&
        (id == IDC_MW_DISP_MODE_MIRROR || id == IDC_MW_DISP_MODE_PRESET ||
         id == IDC_MW_DISP_MODE_CHILD)) {
      if (haveSel) {
        // m_displayOutputsMutex: #27.
        std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
        auto& cfg = p->m_displayOutputs[sel].config;
        if (cfg.type == DisplayOutputType::Monitor) {
          const bool wantChild  = (id == IDC_MW_DISP_MODE_CHILD);
          const bool wantPreset = (id == IDC_MW_DISP_MODE_PRESET) || wantChild;
          cfg.bOwnPreset  = wantPreset;
          cfg.bOwnProcess = wantChild;
          if (wantPreset) {
            cfg.bEnabled = true;
            // A display cannot be both: bIndependentRender is the in-process
            // mirror sim, which renders the PRIMARY's preset.
            cfg.bIndependentRender = false;
          }
          if (wantChild)
            p->RequestChildSpawn(cfg.szDeviceName);
          else
            p->RequestChildKill(cfg.szDeviceName);
          p->SaveDisplayOutputSettings();
          p->UpdateDisplaysTabSelection(sel);
          p->RefreshDisplaysTab();
        }
      }
      return 0;
    }

    // ── The two mode combos: Preset dir and Startup ──
    //
    // Nothing committed these before. They were plain edits with no
    // EN_KILLFOCUS case, so everything typed or pasted into them was silently
    // dropped and only the "..." buttons ever changed anything.
    //
    // SELCHANGE is read with CB_GETLBTEXT rather than GetWindowText: while the
    // list is still open the edit field has not been updated yet, so the window
    // text is the PREVIOUS value.
    if ((code == CBN_SELCHANGE || code == CBN_KILLFOCUS) &&
        (id == IDC_MW_DISP_PRESETDIR || id == IDC_MW_DISP_STARTUP)) {
      if (haveSel) {
        // m_displayOutputsMutex, held for the whole block: cfg is a
        // reference used throughout, and nothing here opens a dialog or
        // blocks (the <Browse...> case below only posts a message).
        // #27.
        std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
        auto& cfg = p->m_displayOutputs[sel].config;
        HWND hCombo = (HWND)lParam;
        wchar_t buf[MAX_PATH] = {};
        if (code == CBN_SELCHANGE) {
          const int i = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
          if (i >= 0 &&
              SendMessageW(hCombo, CB_GETLBTEXTLEN, (WPARAM)i, 0) < MAX_PATH)
            SendMessageW(hCombo, CB_GETLBTEXT, (WPARAM)i, (LPARAM)buf);
        } else {
          GetWindowTextW(hCombo, buf, MAX_PATH);
        }

        // Trim: a path pasted from Explorer arrives wrapped in quotes, and a
        // trailing space is invisible in the field but not to CreateFile.
        std::wstring v(buf);
        const wchar_t* kWs = L" \t\r\n\"";
        v.erase(0, v.find_first_not_of(kWs));
        const size_t end = v.find_last_not_of(kWs);
        v.erase(end == std::wstring::npos ? 0 : end + 1);

        // The MAIN RENDER's settings when its own screen is selected, and the
        // display's otherwise. Same controls, different store.
        const bool mainRender = IsRenderHostDisplay(p, sel);
        wchar_t* dst;
        if (mainRender)
          dst = (id == IDC_MW_DISP_PRESETDIR) ? p->m_primaryProfile.szPresetDir
                                              : p->m_primaryProfile.szStartupPreset;
        else
          dst = (id == IDC_MW_DISP_PRESETDIR) ? cfg.szPresetDir
                                              : cfg.szStartupPreset;

        // An empty field and its implicit mode are the same answer -- the
        // resolvers treat them identically -- so storing one over the other
        // changes nothing and must not look like an edit. The repaint puts
        // "<Default>" into a combo whose config is still empty, and the next
        // focus change then delivered that as a fresh value: a settings write
        // and a child reconfigure for a setting nobody touched.
        //
        // The main render's implicit preset is <Default>, not <Random>: an
        // unpinned child picks a random preset, but an unpinned main render
        // means "whatever the profile captures", which is what is playing.
        const wchar_t* implicit =
            (id == IDC_MW_DISP_PRESETDIR || mainRender) ? kDispModeDefault
                                                        : kDispModeRandom;
        if (!dst[0] && _wcsicmp(v.c_str(), implicit) == 0) return 0;

        // <Browse...> is an action, not a value. Hand off to the browse button
        // and let it put the field right.
        //
        // POSTED rather than called, for two reasons. This runs from
        // CBN_SELCHANGE with the drop-down still closing, and opening a modal
        // dialog inside that leaves the list painted over the top of it. And
        // the combo writes the newly selected item into its edit field AFTER
        // the notification returns -- so restoring the old text here is undone
        // a moment later, which is why "<Browse...>" stayed visible in the
        // field. The browse handler calls UpdateDisplaysTabSelection on both
        // paths, which repaints the combo from the config once the dialog is
        // gone, whether the user chose something or cancelled.
        if (_wcsicmp(v.c_str(), kDispModeBrowse) == 0) {
          // SELCHANGE only, and never stored.
          //
          // Opening the dialog takes the focus off the combo, so the combo
          // sends CBN_KILLFOCUS -- and the field still reads "<Browse...>",
          // because the combo rewrites its edit from the current selection
          // AFTER this notification returns and there is no way to win that
          // race from in here. Accepting it on KILLFOCUS therefore posted a
          // SECOND browse request, which a modal dialog's own message loop
          // dispatches: one pick, two dialogs stacked on each other. Ignoring
          // every code but SELCHANGE also guarantees the sentinel can never be
          // written to the config as if it were a path.
          if (code != CBN_SELCHANGE) return 0;
          const int browseId = (id == IDC_MW_DISP_PRESETDIR)
                                 ? IDC_MW_DISP_PRESETDIR_BR
                                 : IDC_MW_DISP_STARTUP_BR;
          PostMessageW(hWnd, WM_COMMAND,
                       MAKEWPARAM(browseId, BN_CLICKED),
                       (LPARAM)GetDlgItem(hWnd, browseId));
          return 0;
        }
        if (wcscmp(dst, v.c_str()) != 0) {
          // "<Default>" is stored as EMPTY, so the profile carries no opinion
          // rather than the literal word -- an inheriting profile and one that
          // pinned the same value behave differently the moment the global
          // changes, and only the empty form can express the first.
          if (_wcsicmp(v.c_str(), kDispModeDefault) == 0) dst[0] = L'\0';
          else wcsncpy_s(dst, MAX_PATH, v.c_str(), _TRUNCATE);
          if (mainRender) {
            // Nothing saved, nothing reconfigured, and the main window keeps
            // playing what it was playing. This describes the profile that
            // WILL be saved; it is not a request to change the app now, and it
            // must never reach settings.ini.
            p->UpdateDisplaysTabSelection(sel);
          } else {
            p->SaveDisplayOutputSettings();
            // Reconfigure deliberately does NOT re-send the startup preset, so
            // editing this field does not yank a running display back to it --
            // the new value takes effect the next time the display starts,
            // which is what "Startup" means.
            p->RequestChildReconfigure(cfg.szDeviceName);
            p->UpdateDisplaysTabSelection(sel);
          }
        }
      }
      return 0;
    }

    if (code == BN_CLICKED && id == IDC_MW_DISP_PRESETDIR_BR) {
      if (haveSel) {
        // No m_displayOutputs reference held across SHBrowseForFolderW --
        // that dialog can stay open for as long as the user likes, and a
        // reference into the vector held across it would dangle if the
        // render thread reallocated the vector while it was open. cfg is
        // bound fresh, under the lock, only after the dialog returns, and
        // sel is re-validated in case the vector changed shape meanwhile.
        // #27.
        BROWSEINFOW bi = {};
        bi.hwndOwner = hWnd;
        bi.lpszTitle = L"Preset folder for this display";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
          wchar_t path[MAX_PATH] = {};
          if (SHGetPathFromIDListW(pidl, path)) {
            if (IsRenderHostDisplay(p, sel)) {
              wcsncpy_s(p->m_primaryProfile.szPresetDir, path, _TRUNCATE);
            } else {
              std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
              if (sel < (int)p->m_displayOutputs.size()) {
                auto& cfg = p->m_displayOutputs[sel].config;
                wcsncpy_s(cfg.szPresetDir, path, _TRUNCATE);
                p->SaveDisplayOutputSettings();
                p->RequestChildReconfigure(cfg.szDeviceName);
              }
            }
          }
          CoTaskMemFree(pidl);
        }
        // Unconditional: reached by the <Browse...> list entry as well as the
        // button, and that entry leaves the combo showing "<Browse...>" until
        // something repaints it from the config. Cancelling has to repaint too.
        p->UpdateDisplaysTabSelection(sel);
      }
      return 0;
    }

    if (code == BN_CLICKED && id == IDC_MW_DISP_STARTUP_BR) {
      if (haveSel) {
        // Everything GetOpenFileNameW needs is copied out to locals FIRST,
        // under a lock scoped to just that copy -- no reference into
        // m_displayOutputs is held across the dialog, which can stay open
        // indefinitely and would otherwise dangle if the render thread
        // reallocated the vector while it was up. cfg is bound again, fresh
        // and under its own lock, only after the dialog returns, with sel
        // re-validated. #27.
        const bool mainRender = IsRenderHostDisplay(p, sel);
        // For the main render the profile's own directory, then the one in
        // use: a profile is usually built from presets you are looking at.
        std::wstring initDir;
        wchar_t file[MAX_PATH] = {};
        {
          std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
          if (sel >= (int)p->m_displayOutputs.size()) return 0;
          auto& cfg = p->m_displayOutputs[sel].config;
          // The field may hold a MODE, which is not a filename -- handing
          // "<Random>" to GetOpenFileName shows it in the name box as though
          // the user had typed it. Start from a real directory instead, and
          // with an empty name box unless a literal file is already set.
          if (mainRender) {
            initDir = p->m_primaryProfile.szPresetDir;
            if (initDir.empty()) initDir = p->m_szPresetDir;
          } else {
            initDir = p->ResolveDisplayPresetDir(cfg);
          }
          const wchar_t* cur = mainRender ? p->m_primaryProfile.szStartupPreset
                                          : cfg.szStartupPreset;
          if (cur[0] && cur[0] != L'<')
            wcsncpy_s(file, cur, _TRUNCATE);
        }
        OPENFILENAMEW o = {};
        o.lStructSize = sizeof(o);
        o.hwndOwner = hWnd;
        o.lpstrFilter = L"Presets\0*.milk;*.milk2;*.milk3\0All Files\0*.*\0";
        o.lpstrFile = file;
        o.nMaxFile = MAX_PATH;
        o.lpstrInitialDir = initDir.c_str();
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&o)) {
          // The folder the preset came from becomes this display's preset
          // directory (#330).
          //
          // Picking a preset and setting the directory were two separate
          // chores, and the directory was the one easy to forget: the display
          // would start on the chosen preset and then cycle through a different
          // library entirely, because the two settings are resolved separately.
          // Shane asked for one selection to do both.
          //
          // Only the folder is taken, never a guess at anything else, and the
          // preset keeps its full path -- so a preset that lives outside the
          // folder it is stored against still opens.
          std::wstring folder(file);
          const size_t slash = folder.find_last_of(L"\\/");
          folder = (slash == std::wstring::npos) ? std::wstring()
                                                 : folder.substr(0, slash);

          if (mainRender) {
            wcsncpy_s(p->m_primaryProfile.szStartupPreset, file, _TRUNCATE);
            if (!folder.empty())
              wcsncpy_s(p->m_primaryProfile.szPresetDir, folder.c_str(), _TRUNCATE);
          } else {
            std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
            if (sel < (int)p->m_displayOutputs.size()) {
              auto& cfg = p->m_displayOutputs[sel].config;
              wcsncpy_s(cfg.szStartupPreset, file, _TRUNCATE);
              if (!folder.empty())
                wcsncpy_s(cfg.szPresetDir, folder.c_str(), _TRUNCATE);
              p->SaveDisplayOutputSettings();
              p->RequestChildReconfigure(cfg.szDeviceName);
            }
          }
        }
        // Unconditional -- see the preset-directory browse above.
        p->UpdateDisplaysTabSelection(sel);
      }
      return 0;
    }

    if (code == BN_CLICKED && id == IDC_MW_DISP_CYCLE_ON) {
      if (haveSel) {
        // m_displayOutputsMutex: #27.
        std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
        auto& cfg = p->m_displayOutputs[sel].config;
        // bChecked from the switch above is out of scope here; ask directly.
        float secsF;
        if (IsChecked(id)) {
          wchar_t buf[32] = {};
          HWND hSecs = GetDlgItem(hWnd, IDC_MW_DISP_CYCLE_SECS);
          if (hSecs) GetWindowTextW(hSecs, buf, 32);
          int secs = _wtoi(buf);
          if (secs <= 0) secs = 30;
          secsF = (float)secs;
        } else {
          secsF = 0.0f;   // 0 disables cycling, as elsewhere
        }
        if (IsRenderHostDisplay(p, sel)) {
          // The GLOBAL, not a shadow of it. This row and the Presets window
          // describe the same main render, so they drive the same value --
          // otherwise unticking this box left the visualizer still cycling,
          // the setting did not survive a restart, and a saved profile
          // recorded whatever the global had drifted to instead.
          p->SetMainRenderInterval(secsF);
          p->UpdateDisplaysTabSelection(sel);
        } else {
          cfg.fTimeBetweenPresets = secsF;
          p->SaveDisplayOutputSettings();
          p->RequestChildReconfigure(cfg.szDeviceName);
          p->UpdateDisplaysTabSelection(sel);
        }
      }
      return 0;
    }

    // haveSel, like every sibling in this block. It was the one handler without
    // it, and the omission was not cosmetic: sel is -1 whenever no row is
    // selected, vector::operator[] takes an unsigned index, so m_displayOutputs[-1]
    // resolves one element BEFORE the buffer. It then wrote nPresetLock through
    // that reference and passed the same wild memory to RequestChildSend as a
    // device name. Reaching it took only a click on Inherit/Unlocked/Locked with
    // nothing selected -- which, until the enable fix above, the radios happily
    // allowed because they were left enabled.
    if (code == BN_CLICKED &&
        (id == IDC_MW_DISP_LOCK_INHERIT || id == IDC_MW_DISP_LOCK_OFF ||
         id == IDC_MW_DISP_LOCK_ON)) {
      if (haveSel) {
        // m_displayOutputsMutex: #27.
        std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
        auto& cfg = p->m_displayOutputs[sel].config;
        const int want = (id == IDC_MW_DISP_LOCK_INHERIT) ? -1
                       : (id == IDC_MW_DISP_LOCK_ON)      ?  1 : 0;
        if (IsRenderHostDisplay(p, sel)) {
          // The main window's own lock, which is what every other display
          // inherits -- so Inherit is not a choice it can make, and the radio
          // is disabled for this row rather than silently doing nothing.
          if (want >= 0) p->SetMainRenderLock(want != 0);
          p->RefreshDisplaysTab();
        } else {
          cfg.nPresetLock = want;
          p->SaveDisplayOutputSettings();
          // Resolved, not raw: choosing Inherit must move the child to whatever
          // the main window is now rather than leaving it on its last pinned
          // value.
          p->RequestChildSend(cfg.szDeviceName,
                              p->EffectiveChildPresetLock(cfg)
                                  ? L"SET_PRESET_LOCK=1" : L"SET_PRESET_LOCK=0");
          p->RefreshDisplaysTab();
        }
      }
      return 0;
    }
    // Shaped like its siblings. It used to fold the range test into the match
    // condition, so with nothing selected it did not match at all and the
    // message fell THROUGH to the handlers below instead of being consumed --
    // and it matched on the id alone, whatever the notification code.
    if (code == BN_CLICKED &&
        (id == IDC_MW_DISP_ORDER_RANDOM || id == IDC_MW_DISP_ORDER_SEQ)) {
      if (haveSel) {
        // m_displayOutputsMutex: #27.
        std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
        auto& cfg = p->m_displayOutputs[sel].config;
        const bool seq = (id == IDC_MW_DISP_ORDER_SEQ);
        if (IsRenderHostDisplay(p, sel)) {
          p->SetMainRenderOrder(seq);
          p->UpdateDisplaysTabSelection(sel);
        } else {
          cfg.bSequentialOrder = seq;
          p->SaveDisplayOutputSettings();
          p->RequestChildReconfigure(cfg.szDeviceName);
          p->UpdateDisplaysTabSelection(sel);
        }
      }
      return 0;
    }

    if (code == EN_KILLFOCUS && id == IDC_MW_DISP_CYCLE_SECS) {
      if (haveSel) {
        // m_displayOutputsMutex: #27.
        std::lock_guard<std::recursive_mutex> lkDisp(p->m_displayOutputsMutex);
        auto& cfg = p->m_displayOutputs[sel].config;
        wchar_t buf[32] = {};
        GetWindowTextW((HWND)lParam, buf, 32);
        int secs = _wtoi(buf);
        if (secs < 0) secs = 0;
        if (IsRenderHostDisplay(p, sel)) {
          p->SetMainRenderInterval((float)secs);
        } else {
          cfg.fTimeBetweenPresets = (float)secs;
          p->SaveDisplayOutputSettings();
          p->RequestChildReconfigure(cfg.szDeviceName);
        }
      }
      return 0;
    }
  }

  if (code == BN_CLICKED && id == IDC_MW_DISP_PROFILE_AUTOSTART) {
    p->m_bProfileAutostartAll = IsChecked(id);
    // Saved with the next profile, and applied by the next load. Nothing to
    // write to settings.ini: it belongs to the profile, not to the app.
    return 0;
  }

  if (code == BN_CLICKED && id == IDC_MW_DISP_PROFILE_ONSTART) {
    const bool onStart = IsChecked(id);
    p->m_bLoadDisplayProfileOnStart = onStart;
    Config().SetInt(L"Settings", L"bLoadDisplayProfileOnStart", onStart ? 1 : 0);
    return 0;
  }

  // Profiles startup: None / Current / <file>, applied by Save (#278).
  //
  // The choice does nothing until Save, deliberately -- the same as the preset
  // startup row it is modelled on. Selecting "Current" is a REQUEST to snapshot,
  // and a combo that snapshotted on selection would write a profile to disk
  // every time someone browsed the list.
  if (code == BN_CLICKED && id == IDC_MW_DISP_PROFILE_MODESAVE) {
    HWND hMode = GetDlgItem(hWnd, IDC_MW_DISP_PROFILE_MODE);
    const int mode = hMode ? (int)SendMessage(hMode, CB_GETCURSEL, 0, 0) : -1;
    if (mode < 0) return 0;

    if (mode == 0) {                       // None
      p->m_bLoadDisplayProfileOnStart = false;
      Config().SetInt(L"Settings", L"bLoadDisplayProfileOnStart", 0);
      p->AddNotification((wchar_t*)L"No profile will load at startup");
    } else {
      if (mode == 1) {
        // Shane: "if they select current and save it will save the current
        // settings as a new profile and use that as the default." So this is
        // two actions in one press -- write a new timestamped profile from the
        // live display state, then adopt it -- and the combo stops saying
        // "Current" afterwards because it has become a file.
        const std::wstring path = p->SaveDisplayProfileSnapshot();
        if (path.empty()) {
          p->AddNotification((wchar_t*)L"Could not save the current settings");
          return 0;
        }
        wcsncpy_s(p->m_szStartupDisplayProfile, path.c_str(), _TRUNCATE);
        Config().SetString(L"Settings", L"szStartupDisplayProfile",
                           p->m_szStartupDisplayProfile);
      }
      if (!p->m_szStartupDisplayProfile[0]) {
        p->AddNotification((wchar_t*)L"No profile saved yet -- pick Current, or Save...");
        return 0;
      }
      p->m_bLoadDisplayProfileOnStart = true;
      Config().SetInt(L"Settings", L"bLoadDisplayProfileOnStart", 1);
      p->AddNotification((wchar_t*)L"Startup profile saved");
    }
    // Written THROUGH: ConfigStore buffers for about a second, so without this
    // the button gives no sign it took -- which is the whole reason the preset
    // startup row grew a Save in the first place.
    ConfigFlushAll();

    // Repaint the row rather than rebuild the page: the third combo entry and
    // the name field both show the stored profile, and after a Current-snapshot
    // both are stale. Rebuilding the whole tab would also lose the scroll
    // position and the focus.
    const wchar_t* leaf = FileNameOnly(p->m_szStartupDisplayProfile);
    if (HWND hPath = GetDlgItem(hWnd, IDC_MW_DISP_PROFILE_PATH))
      SetWindowTextW(hPath, leaf ? leaf : L"");
    if (hMode) {
      SendMessage(hMode, CB_DELETESTRING, 2, 0);
      SendMessageW(hMode, CB_INSERTSTRING, 2,
                   (LPARAM)((leaf && *leaf) ? leaf : L"(no saved profile)"));
      SendMessage(hMode, CB_SETCURSEL, p->DisplayProfileStartupMode(), 0);
    }
    return 0;
  }

  // Independent mirror FPS cap dropdown
  if (code == CBN_SELCHANGE && id == IDC_MW_DISP_MAXFPS) {
    int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
    static const int kMaxFps[] = { 0, 120, 60, 30 }; // matches combo item order
    if (sel >= 0 && sel < 4) {
      p->m_nMirrorMaxFps.store(kMaxFps[sel]);
      p->SaveDisplayOutputSettings();
    }
    return 0;
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
    EnableWindow(GetDlgItem(hWnd, IDC_MW_OPEN_VFX), active);

    p->SaveSpoutInputSettings();
    return 0;
  }

  if (code == CBN_SELCHANGE && id == IDC_MW_VIDINPUT_WEBCAM) {
    HWND hWebcamCombo = (HWND)lParam;
    int sel = (int)SendMessage(hWebcamCombo, CB_GETCURSEL, 0, 0);
    if (sel <= 0)
      p->m_szWebcamDevice[0] = L'\0';
    else
      SendMessageW(hWebcamCombo, CB_GETLBTEXT, sel, (LPARAM)p->m_szWebcamDevice);
    if (p->m_nVideoInputSource == p->VID_SOURCE_WEBCAM) {
      p->DestroyVideoCapture();
      p->InitVideoCapture();
    }
    p->SaveSpoutInputSettings();
    return 0;
  }

  if (code == BN_CLICKED && id == IDC_MW_VIDINPUT_WEBCAM_REF) {
    HWND hWebcamCombo = GetDlgItem(hWnd, IDC_MW_VIDINPUT_WEBCAM);
    if (hWebcamCombo) {
      wchar_t curSel[256] = {};
      int idx = (int)SendMessage(hWebcamCombo, CB_GETCURSEL, 0, 0);
      if (idx > 0) SendMessageW(hWebcamCombo, CB_GETLBTEXT, idx, (LPARAM)curSel);
      SendMessage(hWebcamCombo, CB_RESETCONTENT, 0, 0);
      SendMessageW(hWebcamCombo, CB_ADDSTRING, 0, (LPARAM)L"(Default)");
      auto webcams = VideoCaptureSource::EnumerateWebcams();
      int newSel = 0;
      for (int i = 0; i < (int)webcams.size(); i++) {
        SendMessageW(hWebcamCombo, CB_ADDSTRING, 0, (LPARAM)webcams[i].name.c_str());
        if (curSel[0] && _wcsicmp(webcams[i].name.c_str(), curSel) == 0) newSel = i + 1;
      }
      SendMessage(hWebcamCombo, CB_SETCURSEL, newSel, 0);
    }
    return 0;
  }

  if (code == BN_CLICKED && id == IDC_MW_VIDINPUT_FILE_BROWSE) {
    wchar_t szFile[MAX_PATH] = {};
    CopyTo(szFile, p->m_szVideoFile);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Video Files (*.mp4;*.avi;*.wmv;*.mkv;*.mov)\0*.mp4;*.avi;*.wmv;*.mkv;*.mov\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
      CopyTo(p->m_szVideoFile, szFile);
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
    HWND hSenderCombo = GetDlgItem(hWnd, IDC_MW_SPINPUT_SENDER);
    if (hSenderCombo) {
      wchar_t curSel[256] = {};
      int idx = (int)SendMessage(hSenderCombo, CB_GETCURSEL, 0, 0);
      if (idx > 0) SendMessageW(hSenderCombo, CB_GETLBTEXT, idx, (LPARAM)curSel);
      SendMessage(hSenderCombo, CB_RESETCONTENT, 0, 0);
      SendMessageW(hSenderCombo, CB_ADDSTRING, 0, (LPARAM)L"(Auto - first available)");
      std::vector<std::string> senders;
      p->EnumerateSpoutSenders(senders);
      int newSel = 0;
      for (int i = 0; i < (int)senders.size(); i++) {
        wchar_t wName[256];
        MultiByteToWideChar(CP_ACP, 0, senders[i].c_str(), -1, wName, 256);
        SendMessageW(hSenderCombo, CB_ADDSTRING, 0, (LPARAM)wName);
        if (curSel[0] && _wcsicmp(wName, curSel) == 0) newSel = i + 1;
      }
      SendMessage(hSenderCombo, CB_SETCURSEL, newSel, 0);
    }
    return 0;
  }

  if (code == CBN_SELCHANGE && id == IDC_MW_SPINPUT_SENDER) {
    HWND hSenderCombo = (HWND)lParam;
    int sel = (int)SendMessage(hSenderCombo, CB_GETCURSEL, 0, 0);
    if (sel <= 0)
      p->m_szSpoutInputSender[0] = L'\0';
    else
      SendMessageW(hSenderCombo, CB_GETLBTEXT, sel, (LPARAM)p->m_szSpoutInputSender);
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
