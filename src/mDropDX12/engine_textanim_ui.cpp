// engine_textanim_ui.cpp — TextAnimWindow ToolWindow implementation
// Animation profile editor: ListView of profiles + detail editing area.
// Launched from Settings Tools tab "Text Animations..." button.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "utility.h"
#include <commctrl.h>
#include <commdlg.h>

namespace mdrop {

extern Engine g_engine;

COLORREF TextAnimWindow::s_acrCustColors[16] = {};

//======================================================================
// TextAnimWindow — ToolWindow subclass
//======================================================================

TextAnimWindow::TextAnimWindow(Engine* pEngine)
  : ToolWindow(pEngine, 560, 840) {}

//----------------------------------------------------------------------
// Engine bridge: Open / Close
//----------------------------------------------------------------------

void Engine::OpenTextAnimWindow() {
  if (!m_textAnimWindow)
    m_textAnimWindow = std::make_unique<TextAnimWindow>(this);
  m_textAnimWindow->Open();
}

void Engine::CloseTextAnimWindow() {
  if (m_textAnimWindow) {
    m_textAnimWindow->Close();
    m_textAnimWindow.reset();
  }
}

//----------------------------------------------------------------------
// Common control flags
//----------------------------------------------------------------------

DWORD TextAnimWindow::GetCommonControlFlags() const {
  return ICC_LISTVIEW_CLASSES;
}

//----------------------------------------------------------------------
// Helper: populate the Song Title / Preset Name profile combo box
//----------------------------------------------------------------------
static void PopulateProfileCombo(HWND hCombo, Engine* p, int selectedIdx,
                                 const wchar_t* firstEntry)
{
  if (!hCombo) return;
  SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
  SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)firstEntry);
  SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(Random)");
  for (int i = 0; i < p->m_nAnimProfileCount; i++)
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)p->m_AnimProfiles[i].szName);

  // Map index: -1 → 0 (Default/Disabled), -2 → 1 (Random), 0+ → idx+2
  int sel = 0;
  if (selectedIdx == -2) sel = 1;
  else if (selectedIdx >= 0) sel = selectedIdx + 2;
  SendMessage(hCombo, CB_SETCURSEL, sel, 0);
}

// Reverse: combo selection → profile index
static int ComboToProfileIndex(HWND hCombo) {
  int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
  if (sel <= 0) return -1;   // (Default) or (Disabled)
  if (sel == 1) return -2;   // (Random)
  return sel - 2;             // profile index
}

//----------------------------------------------------------------------
// PopulateListView — fill all rows from m_AnimProfiles
//----------------------------------------------------------------------

void TextAnimWindow::PopulateListView()
{
  if (!m_hList) return;
  ListView_DeleteAllItems(m_hList);

  for (int i = 0; i < m_pEngine->m_nAnimProfileCount; i++)
    UpdateListViewRow(i);
}

//----------------------------------------------------------------------
// UpdateListViewRow — insert or update a single row
//----------------------------------------------------------------------

void TextAnimWindow::UpdateListViewRow(int idx)
{
  if (!m_hList) return;
  Engine* p = m_pEngine;
  if (idx < 0 || idx >= p->m_nAnimProfileCount) return;
  const td_anim_profile& prof = p->m_AnimProfiles[idx];

  int count = ListView_GetItemCount(m_hList);
  bool insert = (idx >= count);

  wchar_t buf[64];

  // Column 0: # (1-based)
  swprintf(buf, 64, L"%d", idx + 1);
  if (insert) {
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = idx;
    lvi.pszText = buf;
    SendMessageW(m_hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
  } else {
    LVITEMW lvi = {};
    lvi.iSubItem = 0;
    lvi.pszText = buf;
    SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
  }

  // Column 1: Name
  LVITEMW lvi = {};
  lvi.iSubItem = 1;
  lvi.pszText = (LPWSTR)prof.szName;
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 2: Enabled
  lvi.iSubItem = 2;
  lvi.pszText = (LPWSTR)(prof.bEnabled ? L"\u2713" : L"");
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 3: Position
  swprintf(buf, 64, L"%.2f, %.2f", prof.fX, prof.fY);
  lvi.iSubItem = 3;
  lvi.pszText = buf;
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 4: Font
  lvi.iSubItem = 4;
  lvi.pszText = (LPWSTR)(prof.szFontFace[0] ? prof.szFontFace : L"(default)");
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 5: Duration
  swprintf(buf, 64, L"%.1fs", prof.fDuration);
  lvi.iSubItem = 5;
  lvi.pszText = buf;
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
}

//----------------------------------------------------------------------
// UpdateEditControls — populate detail area from selected profile
//----------------------------------------------------------------------

void TextAnimWindow::UpdateEditControls(int sel)
{
  HWND hw = m_hWnd;
  Engine* p = m_pEngine;
  m_nSelectedRow = sel;

  bool valid = (sel >= 0 && sel < p->m_nAnimProfileCount);
  const td_anim_profile& prof = valid ? p->m_AnimProfiles[sel] : td_anim_profile{};

  wchar_t buf[128];

  auto SetFloat = [&](int id, float val) {
    swprintf(buf, 128, L"%.2f", val);
    SetDlgItemTextW(hw, id, buf);
    EnableWindow(GetDlgItem(hw, id), valid);
  };
  auto SetInt = [&](int id, int val) {
    swprintf(buf, 128, L"%d", val);
    SetDlgItemTextW(hw, id, buf);
    EnableWindow(GetDlgItem(hw, id), valid);
  };

  // Name
  SetDlgItemTextW(hw, IDC_MW_TEXTANIM_NAME, valid ? prof.szName : L"");
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_NAME), valid);

  // Enabled
  SetChecked(IDC_MW_TEXTANIM_ENABLED, valid && prof.bEnabled);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_ENABLED), valid);

  // Position
  SetFloat(IDC_MW_TEXTANIM_X, prof.fX);
  SetFloat(IDC_MW_TEXTANIM_Y, prof.fY);
  SetFloat(IDC_MW_TEXTANIM_RANDX, prof.fRandX);
  SetFloat(IDC_MW_TEXTANIM_RANDY, prof.fRandY);

  // Entry
  SetFloat(IDC_MW_TEXTANIM_STARTX, prof.fStartX);
  SetFloat(IDC_MW_TEXTANIM_STARTY, prof.fStartY);
  SetFloat(IDC_MW_TEXTANIM_MOVETIME, prof.fMoveTime);
  SetFloat(IDC_MW_TEXTANIM_EASE_FACTOR, prof.fEaseFactor);

  // Ease mode radios
  SetChecked(IDC_MW_TEXTANIM_EASE_LINEAR, valid && prof.nEaseMode == 0);
  SetChecked(IDC_MW_TEXTANIM_EASE_IN, valid && prof.nEaseMode == 1);
  SetChecked(IDC_MW_TEXTANIM_EASE_OUT, valid && prof.nEaseMode == 2);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_EASE_LINEAR), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_EASE_IN), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_EASE_OUT), valid);

  // Appearance
  SetDlgItemTextW(hw, IDC_MW_TEXTANIM_FONTFACE, valid ? prof.szFontFace : L"");
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_FONTFACE), valid);
  SetFloat(IDC_MW_TEXTANIM_FONTSIZE, prof.fFontSize);
  SetChecked(IDC_MW_TEXTANIM_BOLD, valid && prof.bBold);
  SetChecked(IDC_MW_TEXTANIM_ITALIC, valid && prof.bItal);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_BOLD), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_ITALIC), valid);

  // Color
  SetInt(IDC_MW_TEXTANIM_COLORR, prof.nColorR);
  SetInt(IDC_MW_TEXTANIM_COLORG, prof.nColorG);
  SetInt(IDC_MW_TEXTANIM_COLORB, prof.nColorB);
  SetInt(IDC_MW_TEXTANIM_RANDR, prof.nRandR);
  SetInt(IDC_MW_TEXTANIM_RANDG, prof.nRandG);
  SetInt(IDC_MW_TEXTANIM_RANDB, prof.nRandB);

  // Timing
  SetFloat(IDC_MW_TEXTANIM_DURATION, prof.fDuration);
  SetFloat(IDC_MW_TEXTANIM_FADEIN, prof.fFadeIn);
  SetFloat(IDC_MW_TEXTANIM_FADEOUT, prof.fFadeOut);
  SetFloat(IDC_MW_TEXTANIM_BURNTIME, prof.fBurnTime);

  // Effects
  SetFloat(IDC_MW_TEXTANIM_GROWTH, prof.fGrowth);
  SetFloat(IDC_MW_TEXTANIM_SHADOW, prof.fShadowOffset);
  SetFloat(IDC_MW_TEXTANIM_BOXALPHA, prof.fBoxAlpha);
  SetInt(IDC_MW_TEXTANIM_BOXCOLR, prof.nBoxColR);
  SetInt(IDC_MW_TEXTANIM_BOXCOLG, prof.nBoxColG);
  SetInt(IDC_MW_TEXTANIM_BOXCOLB, prof.nBoxColB);

  // Randomization flags
  SetChecked(IDC_MW_TEXTANIM_RAND_POS, valid && prof.bRandPos);
  SetChecked(IDC_MW_TEXTANIM_RAND_SIZE, valid && prof.bRandSize);
  SetChecked(IDC_MW_TEXTANIM_RAND_COLOR, valid && prof.bRandColor);
  SetChecked(IDC_MW_TEXTANIM_RAND_GROWTH, valid && prof.bRandGrowth);
  SetChecked(IDC_MW_TEXTANIM_RAND_DURATION, valid && prof.bRandDuration);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_RAND_POS), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_RAND_SIZE), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_RAND_COLOR), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_RAND_GROWTH), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_RAND_DURATION), valid);

  // Picker button enable state
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_CHOOSE_FONT), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_CHOOSE_COLOR), valid);
  EnableWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_CHOOSE_BOXCOL), valid);

  // Update visual previews
  UpdateColorSwatch(IDC_MW_TEXTANIM_COLOR_SWATCH, prof.nColorR, prof.nColorG, prof.nColorB);
  UpdateColorSwatch(IDC_MW_TEXTANIM_BOXCOL_SWATCH, prof.nBoxColR, prof.nBoxColG, prof.nBoxColB);
  UpdateFontPreview();
}

//----------------------------------------------------------------------
// SaveEditControls — save detail area back to selected profile
//----------------------------------------------------------------------

void TextAnimWindow::SaveEditControls()
{
  Engine* p = m_pEngine;
  int sel = m_nSelectedRow;
  if (sel < 0 || sel >= p->m_nAnimProfileCount) return;
  td_anim_profile& prof = p->m_AnimProfiles[sel];
  HWND hw = m_hWnd;

  wchar_t buf[128];

  auto GetFloat = [&](int id, float def) -> float {
    GetDlgItemTextW(hw, id, buf, 128);
    try { return std::stof(buf); } catch (...) { return def; }
  };
  auto GetInt = [&](int id, int def) -> int {
    GetDlgItemTextW(hw, id, buf, 128);
    try { return std::stoi(buf); } catch (...) { return def; }
  };

  // Name
  GetDlgItemTextW(hw, IDC_MW_TEXTANIM_NAME, prof.szName, 64);

  // Enabled
  prof.bEnabled = IsChecked(IDC_MW_TEXTANIM_ENABLED);

  // Position
  prof.fX = GetFloat(IDC_MW_TEXTANIM_X, 0.5f);
  prof.fY = GetFloat(IDC_MW_TEXTANIM_Y, 0.5f);
  prof.fRandX = GetFloat(IDC_MW_TEXTANIM_RANDX, 0.0f);
  prof.fRandY = GetFloat(IDC_MW_TEXTANIM_RANDY, 0.0f);

  // Entry
  prof.fStartX = GetFloat(IDC_MW_TEXTANIM_STARTX, -100.0f);
  prof.fStartY = GetFloat(IDC_MW_TEXTANIM_STARTY, -100.0f);
  prof.fMoveTime = GetFloat(IDC_MW_TEXTANIM_MOVETIME, 0.0f);
  prof.fEaseFactor = GetFloat(IDC_MW_TEXTANIM_EASE_FACTOR, 2.0f);

  // Ease mode
  if (IsChecked(IDC_MW_TEXTANIM_EASE_LINEAR)) prof.nEaseMode = 0;
  else if (IsChecked(IDC_MW_TEXTANIM_EASE_IN)) prof.nEaseMode = 1;
  else prof.nEaseMode = 2;

  // Appearance
  GetDlgItemTextW(hw, IDC_MW_TEXTANIM_FONTFACE, prof.szFontFace, 128);
  prof.fFontSize = GetFloat(IDC_MW_TEXTANIM_FONTSIZE, 50.0f);
  prof.bBold = IsChecked(IDC_MW_TEXTANIM_BOLD) ? 1 : 0;
  prof.bItal = IsChecked(IDC_MW_TEXTANIM_ITALIC) ? 1 : 0;

  // Color
  prof.nColorR = max(0, min(255, GetInt(IDC_MW_TEXTANIM_COLORR, 255)));
  prof.nColorG = max(0, min(255, GetInt(IDC_MW_TEXTANIM_COLORG, 255)));
  prof.nColorB = max(0, min(255, GetInt(IDC_MW_TEXTANIM_COLORB, 255)));
  prof.nRandR = max(0, min(255, GetInt(IDC_MW_TEXTANIM_RANDR, 0)));
  prof.nRandG = max(0, min(255, GetInt(IDC_MW_TEXTANIM_RANDG, 0)));
  prof.nRandB = max(0, min(255, GetInt(IDC_MW_TEXTANIM_RANDB, 0)));

  // Timing
  prof.fDuration = GetFloat(IDC_MW_TEXTANIM_DURATION, 5.0f);
  prof.fFadeIn = GetFloat(IDC_MW_TEXTANIM_FADEIN, 0.2f);
  prof.fFadeOut = GetFloat(IDC_MW_TEXTANIM_FADEOUT, 0.5f);
  prof.fBurnTime = GetFloat(IDC_MW_TEXTANIM_BURNTIME, 0.0f);

  // Effects
  prof.fGrowth = GetFloat(IDC_MW_TEXTANIM_GROWTH, 1.0f);
  prof.fShadowOffset = GetFloat(IDC_MW_TEXTANIM_SHADOW, 0.0f);
  prof.fBoxAlpha = GetFloat(IDC_MW_TEXTANIM_BOXALPHA, 0.0f);
  prof.nBoxColR = max(0, min(255, GetInt(IDC_MW_TEXTANIM_BOXCOLR, 0)));
  prof.nBoxColG = max(0, min(255, GetInt(IDC_MW_TEXTANIM_BOXCOLG, 0)));
  prof.nBoxColB = max(0, min(255, GetInt(IDC_MW_TEXTANIM_BOXCOLB, 0)));

  // Randomization flags
  prof.bRandPos = IsChecked(IDC_MW_TEXTANIM_RAND_POS) ? 1 : 0;
  prof.bRandSize = IsChecked(IDC_MW_TEXTANIM_RAND_SIZE) ? 1 : 0;
  prof.bRandColor = IsChecked(IDC_MW_TEXTANIM_RAND_COLOR) ? 1 : 0;
  prof.bRandGrowth = IsChecked(IDC_MW_TEXTANIM_RAND_GROWTH) ? 1 : 0;
  prof.bRandDuration = IsChecked(IDC_MW_TEXTANIM_RAND_DURATION) ? 1 : 0;

  UpdateListViewRow(sel);
}

//----------------------------------------------------------------------
// SelectProfile — select a row in the ListView and update edit controls
//----------------------------------------------------------------------

void TextAnimWindow::SelectProfile(int idx)
{
  if (!m_hList) return;
  if (idx >= 0 && idx < m_pEngine->m_nAnimProfileCount) {
    ListView_SetItemState(m_hList, idx, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(m_hList, idx, FALSE);
  }
  UpdateEditControls(idx);
}

//----------------------------------------------------------------------
// OnResize — recalculate layout
//----------------------------------------------------------------------

void TextAnimWindow::OnResize()
{
  RebuildFonts();
}

//----------------------------------------------------------------------
// DoBuildControls — create all child controls
//----------------------------------------------------------------------

void TextAnimWindow::DoBuildControls()
{
  HWND hw = m_hWnd;
  if (!hw) return;

  Engine* p = m_pEngine;
  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  HFONT hFontBold = GetFontBold();
  m_nTopY = y;

  // ── Header: Song Title Profile + Preset Name Profile combos ──
  int labelW = MulDiv(130, lineH, 26);
  int comboW = rw - labelW - MulDiv(80, lineH, 26) - 8;
  int pushW = MulDiv(80, lineH, 26);

  TrackControl(CreateLabel(hw, L"Song Title Profile:", x, y, labelW, lineH, hFont));
  HWND hSongCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
    x + labelW, y, comboW, lineH * 10, hw,
    (HMENU)(INT_PTR)IDC_MW_TEXTANIM_SONG_COMBO, GetModuleHandle(NULL), NULL);
  if (hSongCombo && hFont) SendMessage(hSongCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
  TrackControl(hSongCombo);
  PopulateProfileCombo(hSongCombo, p, p->m_nSongTitleAnimProfile, L"(Default)");

  TrackControl(CreateBtn(hw, L"Push Title", IDC_MW_TEXTANIM_PUSH_TITLE,
    x + labelW + comboW + 4, y, pushW, lineH, hFont));
  y += lineH + gap;

  TrackControl(CreateLabel(hw, L"Preset Name Profile:", x, y, labelW, lineH, hFont));
  HWND hPresetCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
    x + labelW, y, comboW + pushW + 4, lineH * 10, hw,
    (HMENU)(INT_PTR)IDC_MW_TEXTANIM_PRESET_COMBO, GetModuleHandle(NULL), NULL);
  if (hPresetCombo && hFont) SendMessage(hPresetCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
  TrackControl(hPresetCombo);
  PopulateProfileCombo(hPresetCombo, p, p->m_nPresetNameAnimProfile, L"(Disabled)");
  y += lineH + gap + 4;

  // ── ListView ──
  RECT rc;
  GetClientRect(hw, &rc);
  int listH = MulDiv(120, lineH, 26);  // ~5 rows
  if (listH > rc.bottom - y - lineH * 18) listH = rc.bottom - y - lineH * 18;
  if (listH < lineH * 3) listH = lineH * 3;

  m_hList = CreateThemedListView(IDC_MW_TEXTANIM_LIST, x, y, rw, listH);
  TrackControl(m_hList);
  if (m_hList) {
    int colW[] = {
      MulDiv(rw, 6, 100),   // #
      MulDiv(rw, 26, 100),  // Name
      MulDiv(rw, 6, 100),   // On
      MulDiv(rw, 18, 100),  // Position
      MulDiv(rw, 22, 100),  // Font
      0                      // Duration (remainder)
    };
    colW[5] = rw - colW[0] - colW[1] - colW[2] - colW[3] - colW[4]
              - GetSystemMetrics(SM_CXVSCROLL) - 4;

    const wchar_t* colNames[] = { L"#", L"Name", L"On", L"Position", L"Font", L"Duration" };
    for (int i = 0; i < 6; i++) {
      LVCOLUMNW col = {};
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = (LPWSTR)colNames[i];
      col.cx = colW[i];
      SendMessageW(m_hList, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }

    PopulateListView();
    if (p->m_nAnimProfileCount > 0)
      ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
  }
  y += listH + gap;

  // ── Button row: Add, Duplicate, Delete, Move Up, Move Down, Templates ──
  {
    int btnW = MulDiv(58, lineH, 26);
    int btnGap = 4;
    int bx = x;
    TrackControl(CreateBtn(hw, L"Add", IDC_MW_TEXTANIM_ADD, bx, y, btnW, lineH, hFont));
    bx += btnW + btnGap;
    TrackControl(CreateBtn(hw, L"Dupe", IDC_MW_TEXTANIM_DUPLICATE, bx, y, btnW, lineH, hFont));
    bx += btnW + btnGap;
    TrackControl(CreateBtn(hw, L"Delete", IDC_MW_TEXTANIM_DELETE, bx, y, btnW, lineH, hFont));
    bx += btnW + btnGap;
    TrackControl(CreateBtn(hw, L"\x25B2", IDC_MW_TEXTANIM_MOVEUP, bx, y, MulDiv(30, lineH, 26), lineH, hFont));
    bx += MulDiv(30, lineH, 26) + btnGap;
    TrackControl(CreateBtn(hw, L"\x25BC", IDC_MW_TEXTANIM_MOVEDOWN, bx, y, MulDiv(30, lineH, 26), lineH, hFont));

    // Right-aligned: Templates
    int templW = MulDiv(80, lineH, 26);
    TrackControl(CreateBtn(hw, L"Templates", IDC_MW_TEXTANIM_TEMPLATES,
      x + rw - templW, y, templW, lineH, hFont));
  }
  y += lineH + gap + 4;

  // ── Detail editing area ──
  int editLabelW = MulDiv(55, lineH, 26);
  int editW = MulDiv(50, lineH, 26);
  int editWide = MulDiv(140, lineH, 26);

  // Name + Enabled
  TrackControl(CreateLabel(hw, L"Name:", x, y, editLabelW, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_NAME,
    x + editLabelW + 4, y, rw - editLabelW - MulDiv(80, lineH, 26) - 12, lineH, hFont, ES_AUTOHSCROLL));
  TrackControl(CreateCheck(hw, L"Enabled", IDC_MW_TEXTANIM_ENABLED,
    x + rw - MulDiv(80, lineH, 26), y, MulDiv(80, lineH, 26), lineH, hFont, true));
  y += lineH + gap;

  // ── Position section ──
  TrackControl(CreateLabel(hw, L"Position", x, y, rw, lineH, hFontBold));
  y += lineH + 2;

  {
    int cx = x;
    int smallLbl = MulDiv(20, lineH, 26);
    int medLbl = MulDiv(50, lineH, 26);

    TrackControl(CreateLabel(hw, L"X:", cx, y, smallLbl, lineH, hFont));
    cx += smallLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_X, cx, y, editW, lineH, hFont));
    cx += editW + 4;
    TrackControl(CreateLabel(hw, L"Y:", cx, y, smallLbl, lineH, hFont));
    cx += smallLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_Y, cx, y, editW, lineH, hFont));
    cx += editW + 8;
    TrackControl(CreateLabel(hw, L"Rand X:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_RANDX, cx, y, editW, lineH, hFont));
    cx += editW + 4;
    TrackControl(CreateLabel(hw, L"Y:", cx, y, smallLbl, lineH, hFont));
    cx += smallLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_RANDY, cx, y, editW, lineH, hFont));
  }
  y += lineH + gap;

  // Entry position
  {
    int cx = x;
    int smallLbl = MulDiv(48, lineH, 26);
    int medLbl = MulDiv(70, lineH, 26);

    TrackControl(CreateLabel(hw, L"Entry X:", cx, y, smallLbl, lineH, hFont));
    cx += smallLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_STARTX, cx, y, editW, lineH, hFont));
    cx += editW + 4;
    TrackControl(CreateLabel(hw, L"Y:", cx, y, MulDiv(20, lineH, 26), lineH, hFont));
    cx += MulDiv(20, lineH, 26);
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_STARTY, cx, y, editW, lineH, hFont));
    cx += editW + 8;
    TrackControl(CreateLabel(hw, L"Move Time:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_MOVETIME, cx, y, editW, lineH, hFont));
  }
  y += lineH + gap;

  // Ease mode
  {
    int cx = x;
    int radioW = MulDiv(60, lineH, 26);
    int medLbl = MulDiv(50, lineH, 26);

    TrackControl(CreateLabel(hw, L"Ease:", cx, y, MulDiv(40, lineH, 26), lineH, hFont));
    cx += MulDiv(40, lineH, 26);
    TrackControl(CreateRadio(hw, L"Linear", IDC_MW_TEXTANIM_EASE_LINEAR, cx, y, radioW, lineH, hFont, false, true, true, 1));
    cx += radioW + 2;
    TrackControl(CreateRadio(hw, L"In", IDC_MW_TEXTANIM_EASE_IN, cx, y, MulDiv(35, lineH, 26), lineH, hFont, false, false, true, 1));
    cx += MulDiv(35, lineH, 26) + 2;
    TrackControl(CreateRadio(hw, L"Out", IDC_MW_TEXTANIM_EASE_OUT, cx, y, MulDiv(40, lineH, 26), lineH, hFont, true, false, true, 1));
    cx += MulDiv(40, lineH, 26) + 8;
    TrackControl(CreateLabel(hw, L"Factor:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_EASE_FACTOR, cx, y, editW, lineH, hFont));
  }
  y += lineH + gap + 4;

  // ── Appearance section ──
  TrackControl(CreateLabel(hw, L"Appearance", x, y, rw, lineH, hFontBold));
  y += lineH + 2;

  // Font picker button + preview + size
  {
    int cx = x;
    int btnW = MulDiv(60, lineH, 26);
    int medLbl = MulDiv(40, lineH, 26);

    TrackControl(CreateBtn(hw, L"Font...", IDC_MW_TEXTANIM_CHOOSE_FONT, cx, y, btnW, lineH, hFont));
    cx += btnW + 4;

    // Font preview label (shows "Segoe UI, Bold")
    int previewW = rw - btnW - medLbl - editW - 12;
    HWND hPreview = CreateWindowExW(0, L"STATIC", L"",
      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
      cx, y, previewW, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_TEXTANIM_FONT_PREVIEW, GetModuleHandle(NULL), NULL);
    if (hPreview && hFont) SendMessage(hPreview, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hPreview);
    cx += previewW + 4;

    TrackControl(CreateLabel(hw, L"Size:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_FONTSIZE, cx, y, editW, lineH, hFont));

    // Hidden controls for font data (read by SaveEditControls)
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_FONTFACE, 0, 0, 0, 0, hFont, ES_AUTOHSCROLL));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_FONTFACE), SW_HIDE);
    TrackControl(CreateCheck(hw, L"", IDC_MW_TEXTANIM_BOLD, 0, 0, 0, 0, hFont, false));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_BOLD), SW_HIDE);
    TrackControl(CreateCheck(hw, L"", IDC_MW_TEXTANIM_ITALIC, 0, 0, 0, 0, hFont, false));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_ITALIC), SW_HIDE);
  }
  y += lineH + gap;

  // Color picker button + swatch + rand
  {
    int cx = x;
    int btnW = MulDiv(60, lineH, 26);
    int swatchW = MulDiv(30, lineH, 26);
    int medLbl = MulDiv(42, lineH, 26);
    int colEditW = MulDiv(35, lineH, 26);
    int smallLbl = MulDiv(20, lineH, 26);

    TrackControl(CreateBtn(hw, L"Color...", IDC_MW_TEXTANIM_CHOOSE_COLOR, cx, y, btnW, lineH, hFont));
    cx += btnW + 4;

    // Color swatch (owner-drawn static)
    HWND hSwatch = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"",
      WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
      cx, y, swatchW, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_TEXTANIM_COLOR_SWATCH, GetModuleHandle(NULL), NULL);
    TrackControl(hSwatch);
    cx += swatchW + 8;

    TrackControl(CreateLabel(hw, L"Rand:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateLabel(hw, L"R:", cx, y, smallLbl, lineH, hFont));
    cx += smallLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_RANDR, cx, y, colEditW, lineH, hFont));
    cx += colEditW + 2;
    TrackControl(CreateLabel(hw, L"G:", cx, y, smallLbl, lineH, hFont));
    cx += smallLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_RANDG, cx, y, colEditW, lineH, hFont));
    cx += colEditW + 2;
    TrackControl(CreateLabel(hw, L"B:", cx, y, smallLbl, lineH, hFont));
    cx += smallLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_RANDB, cx, y, colEditW, lineH, hFont));

    // Hidden R/G/B edit controls for data storage
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_COLORR, 0, 0, 0, 0, hFont));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_COLORR), SW_HIDE);
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_COLORG, 0, 0, 0, 0, hFont));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_COLORG), SW_HIDE);
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_COLORB, 0, 0, 0, 0, hFont));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_COLORB), SW_HIDE);
  }
  y += lineH + gap + 4;

  // ── Timing section ──
  TrackControl(CreateLabel(hw, L"Timing", x, y, rw, lineH, hFontBold));
  y += lineH + 2;

  {
    int cx = x;
    int medLbl = MulDiv(60, lineH, 26);

    TrackControl(CreateLabel(hw, L"Duration:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_DURATION, cx, y, editW, lineH, hFont));
    cx += editW + 4;
    TrackControl(CreateLabel(hw, L"Fade In:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_FADEIN, cx, y, editW, lineH, hFont));
    cx += editW + 4;
    TrackControl(CreateLabel(hw, L"Fade Out:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_FADEOUT, cx, y, editW, lineH, hFont));
    cx += editW + 4;
    TrackControl(CreateLabel(hw, L"Burn:", cx, y, MulDiv(40, lineH, 26), lineH, hFont));
    cx += MulDiv(40, lineH, 26);
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_BURNTIME, cx, y, editW, lineH, hFont));
  }
  y += lineH + gap + 4;

  // ── Effects section ──
  TrackControl(CreateLabel(hw, L"Effects", x, y, rw, lineH, hFontBold));
  y += lineH + 2;

  {
    int cx = x;
    int medLbl = MulDiv(55, lineH, 26);

    TrackControl(CreateLabel(hw, L"Growth:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_GROWTH, cx, y, editW, lineH, hFont));
    cx += editW + 4;
    TrackControl(CreateLabel(hw, L"Shadow:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_SHADOW, cx, y, editW, lineH, hFont));
    cx += editW + 4;
    TrackControl(CreateLabel(hw, L"Box \u03B1:", cx, y, medLbl, lineH, hFont));
    cx += medLbl;
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_BOXALPHA, cx, y, editW, lineH, hFont));
  }
  y += lineH + gap;

  // Box color picker + swatch
  {
    int cx = x;
    int btnW = MulDiv(80, lineH, 26);
    int swatchW = MulDiv(30, lineH, 26);

    TrackControl(CreateBtn(hw, L"Box Color...", IDC_MW_TEXTANIM_CHOOSE_BOXCOL, cx, y, btnW, lineH, hFont));
    cx += btnW + 4;

    HWND hBoxSwatch = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"",
      WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
      cx, y, swatchW, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_TEXTANIM_BOXCOL_SWATCH, GetModuleHandle(NULL), NULL);
    TrackControl(hBoxSwatch);

    // Hidden R/G/B edit controls for data storage
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_BOXCOLR, 0, 0, 0, 0, hFont));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_BOXCOLR), SW_HIDE);
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_BOXCOLG, 0, 0, 0, 0, hFont));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_BOXCOLG), SW_HIDE);
    TrackControl(CreateEdit(hw, L"", IDC_MW_TEXTANIM_BOXCOLB, 0, 0, 0, 0, hFont));
    ShowWindow(GetDlgItem(hw, IDC_MW_TEXTANIM_BOXCOLB), SW_HIDE);
  }
  y += lineH + gap;

  // Randomization flags
  {
    int chkW = MulDiv(65, lineH, 26);
    int cx = x;
    TrackControl(CreateCheck(hw, L"Pos", IDC_MW_TEXTANIM_RAND_POS, cx, y, chkW, lineH, hFont, false));
    cx += chkW + 2;
    TrackControl(CreateCheck(hw, L"Size", IDC_MW_TEXTANIM_RAND_SIZE, cx, y, chkW, lineH, hFont, false));
    cx += chkW + 2;
    TrackControl(CreateCheck(hw, L"Color", IDC_MW_TEXTANIM_RAND_COLOR, cx, y, chkW, lineH, hFont, false));
    cx += chkW + 2;
    TrackControl(CreateCheck(hw, L"Growth", IDC_MW_TEXTANIM_RAND_GROWTH, cx, y, chkW, lineH, hFont, false));
    cx += chkW + 2;
    TrackControl(CreateCheck(hw, L"Duration", IDC_MW_TEXTANIM_RAND_DURATION, cx, y, MulDiv(80, lineH, 26), lineH, hFont, false));
  }
  y += lineH + gap + 4;

  // ── Bottom buttons: Preview, Save ──
  {
    int btnW = MulDiv(70, lineH, 26);
    int btnGap = 6;
    TrackControl(CreateBtn(hw, L"Preview", IDC_MW_TEXTANIM_PREVIEW, x, y, btnW, lineH, hFont));
    TrackControl(CreateBtn(hw, L"Save", IDC_MW_TEXTANIM_SAVE, x + btnW + btnGap, y, btnW, lineH, hFont));
  }

  // Initial selection
  UpdateEditControls(p->m_nAnimProfileCount > 0 ? 0 : -1);
}

//----------------------------------------------------------------------
// UpdateColorSwatch — repaint a color swatch control
//----------------------------------------------------------------------

void TextAnimWindow::UpdateColorSwatch(int ctrlID, int r, int g, int b)
{
  HWND hSwatch = GetDlgItem(m_hWnd, ctrlID);
  if (hSwatch) {
    // Store the color as window property for WM_DRAWITEM
    SetPropW(hSwatch, L"SwatchColor", (HANDLE)(intptr_t)RGB(r, g, b));
    InvalidateRect(hSwatch, NULL, TRUE);
  }
}

//----------------------------------------------------------------------
// UpdateFontPreview — show "FontFace, Bold Italic" in the preview label
//----------------------------------------------------------------------

void TextAnimWindow::UpdateFontPreview()
{
  HWND hw = m_hWnd;
  if (!hw) return;
  int sel = m_nSelectedRow;
  if (sel < 0 || sel >= m_pEngine->m_nAnimProfileCount) {
    SetDlgItemTextW(hw, IDC_MW_TEXTANIM_FONT_PREVIEW, L"");
    return;
  }
  const td_anim_profile& prof = m_pEngine->m_AnimProfiles[sel];
  wchar_t preview[256];
  const wchar_t* face = prof.szFontFace[0] ? prof.szFontFace : L"(default)";
  if (prof.bBold && prof.bItal)
    swprintf(preview, 256, L"%s, Bold Italic", face);
  else if (prof.bBold)
    swprintf(preview, 256, L"%s, Bold", face);
  else if (prof.bItal)
    swprintf(preview, 256, L"%s, Italic", face);
  else
    swprintf(preview, 256, L"%s", face);
  SetDlgItemTextW(hw, IDC_MW_TEXTANIM_FONT_PREVIEW, preview);
}

//----------------------------------------------------------------------
// DoCommand — handle button clicks
//----------------------------------------------------------------------

LRESULT TextAnimWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam)
{
  Engine* p = m_pEngine;

  switch (id) {
  case IDC_MW_TEXTANIM_ADD:
    if (p->m_nAnimProfileCount < MAX_ANIM_PROFILES) {
      td_anim_profile& prof = p->m_AnimProfiles[p->m_nAnimProfileCount];
      prof = td_anim_profile{};
      swprintf(prof.szName, 64, L"Profile %d", p->m_nAnimProfileCount + 1);
      p->m_nAnimProfileCount++;
      PopulateListView();
      SelectProfile(p->m_nAnimProfileCount - 1);
    }
    return 0;

  case IDC_MW_TEXTANIM_DUPLICATE:
    if (m_nSelectedRow >= 0 && m_nSelectedRow < p->m_nAnimProfileCount
        && p->m_nAnimProfileCount < MAX_ANIM_PROFILES) {
      SaveEditControls();
      p->m_AnimProfiles[p->m_nAnimProfileCount] = p->m_AnimProfiles[m_nSelectedRow];
      wchar_t name[64];
      swprintf(name, 64, L"%s (copy)", p->m_AnimProfiles[m_nSelectedRow].szName);
      lstrcpynW(p->m_AnimProfiles[p->m_nAnimProfileCount].szName, name, 64);
      p->m_nAnimProfileCount++;
      PopulateListView();
      SelectProfile(p->m_nAnimProfileCount - 1);
    }
    return 0;

  case IDC_MW_TEXTANIM_DELETE:
    if (m_nSelectedRow >= 0 && m_nSelectedRow < p->m_nAnimProfileCount) {
      for (int i = m_nSelectedRow; i < p->m_nAnimProfileCount - 1; i++)
        p->m_AnimProfiles[i] = p->m_AnimProfiles[i + 1];
      p->m_nAnimProfileCount--;
      PopulateListView();
      int newSel = min(m_nSelectedRow, p->m_nAnimProfileCount - 1);
      SelectProfile(newSel);
    }
    return 0;

  case IDC_MW_TEXTANIM_MOVEUP:
    if (m_nSelectedRow > 0 && m_nSelectedRow < p->m_nAnimProfileCount) {
      SaveEditControls();
      std::swap(p->m_AnimProfiles[m_nSelectedRow], p->m_AnimProfiles[m_nSelectedRow - 1]);
      PopulateListView();
      SelectProfile(m_nSelectedRow - 1);
    }
    return 0;

  case IDC_MW_TEXTANIM_MOVEDOWN:
    if (m_nSelectedRow >= 0 && m_nSelectedRow < p->m_nAnimProfileCount - 1) {
      SaveEditControls();
      std::swap(p->m_AnimProfiles[m_nSelectedRow], p->m_AnimProfiles[m_nSelectedRow + 1]);
      PopulateListView();
      SelectProfile(m_nSelectedRow + 1);
    }
    return 0;

  case IDC_MW_TEXTANIM_TEMPLATES:
    if (MessageBoxW(hWnd, L"Reset profiles to built-in defaults?\nThis will replace all current profiles.",
                    L"Load Templates", MB_YESNO | MB_ICONQUESTION) == IDYES) {
      p->CreateDefaultAnimProfiles();
      PopulateListView();
      SelectProfile(0);
      // Refresh combos
      PopulateProfileCombo(GetDlgItem(hWnd, IDC_MW_TEXTANIM_SONG_COMBO),
                           p, p->m_nSongTitleAnimProfile, L"(Default)");
      PopulateProfileCombo(GetDlgItem(hWnd, IDC_MW_TEXTANIM_PRESET_COMBO),
                           p, p->m_nPresetNameAnimProfile, L"(Disabled)");
    }
    return 0;

  case IDC_MW_TEXTANIM_PREVIEW:
    if (m_nSelectedRow >= 0 && m_nSelectedRow < p->m_nAnimProfileCount) {
      SaveEditControls();
      // Launch a preview supertext with the selected profile
      int slot = p->GetNextFreeSupertextIndex();
      lstrcpyW(p->m_supertexts[slot].szTextW, L"Preview Animation");
      p->m_supertexts[slot].bRedrawSuperText = true;
      p->m_supertexts[slot].bIsSongTitle = false;
      p->ApplyAnimProfileToSupertext(p->m_supertexts[slot], p->m_AnimProfiles[m_nSelectedRow]);
      p->m_supertexts[slot].fStartTime = p->GetTime();
    }
    return 0;

  case IDC_MW_TEXTANIM_SAVE:
    SaveEditControls();
    p->WriteAnimProfiles();
    p->AddNotification(L"Animation profiles saved.");
    return 0;

  case IDC_MW_TEXTANIM_PUSH_TITLE:
    p->PushSongTitleAsMessage();
    return 0;

  case IDC_MW_TEXTANIM_SONG_COMBO:
    if (code == CBN_SELCHANGE) {
      p->m_nSongTitleAnimProfile = ComboToProfileIndex(GetDlgItem(hWnd, IDC_MW_TEXTANIM_SONG_COMBO));
    }
    return 0;

  case IDC_MW_TEXTANIM_PRESET_COMBO:
    if (code == CBN_SELCHANGE) {
      p->m_nPresetNameAnimProfile = ComboToProfileIndex(GetDlgItem(hWnd, IDC_MW_TEXTANIM_PRESET_COMBO));
    }
    return 0;

  case IDC_MW_TEXTANIM_CHOOSE_FONT:
    if (m_nSelectedRow >= 0 && m_nSelectedRow < p->m_nAnimProfileCount) {
      td_anim_profile& prof = p->m_AnimProfiles[m_nSelectedRow];
      LOGFONTW lf = {};
      wcscpy_s(lf.lfFaceName, 32, prof.szFontFace[0] ? prof.szFontFace : L"Segoe UI");
      lf.lfWeight = prof.bBold ? FW_BOLD : FW_NORMAL;
      lf.lfItalic = prof.bItal ? TRUE : FALSE;
      lf.lfHeight = -24;

      CHOOSEFONTW cf = { sizeof(cf) };
      cf.hwndOwner = hWnd;
      cf.lpLogFont = &lf;
      cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
      cf.rgbColors = RGB(prof.nColorR, prof.nColorG, prof.nColorB);

      if (ChooseFontW(&cf)) {
        wcscpy_s(prof.szFontFace, 128, lf.lfFaceName);
        prof.bBold = (lf.lfWeight >= FW_BOLD) ? 1 : 0;
        prof.bItal = lf.lfItalic ? 1 : 0;
        prof.nColorR = GetRValue(cf.rgbColors);
        prof.nColorG = GetGValue(cf.rgbColors);
        prof.nColorB = GetBValue(cf.rgbColors);
        UpdateEditControls(m_nSelectedRow);
        UpdateListViewRow(m_nSelectedRow);
      }
    }
    return 0;

  case IDC_MW_TEXTANIM_CHOOSE_COLOR:
    if (m_nSelectedRow >= 0 && m_nSelectedRow < p->m_nAnimProfileCount) {
      td_anim_profile& prof = p->m_AnimProfiles[m_nSelectedRow];
      CHOOSECOLORW cc = { sizeof(cc) };
      cc.hwndOwner = hWnd;
      cc.rgbResult = RGB(prof.nColorR, prof.nColorG, prof.nColorB);
      cc.lpCustColors = s_acrCustColors;
      cc.Flags = CC_FULLOPEN | CC_RGBINIT;

      if (ChooseColorW(&cc)) {
        prof.nColorR = GetRValue(cc.rgbResult);
        prof.nColorG = GetGValue(cc.rgbResult);
        prof.nColorB = GetBValue(cc.rgbResult);
        UpdateEditControls(m_nSelectedRow);
      }
    }
    return 0;

  case IDC_MW_TEXTANIM_CHOOSE_BOXCOL:
    if (m_nSelectedRow >= 0 && m_nSelectedRow < p->m_nAnimProfileCount) {
      td_anim_profile& prof = p->m_AnimProfiles[m_nSelectedRow];
      CHOOSECOLORW cc = { sizeof(cc) };
      cc.hwndOwner = hWnd;
      cc.rgbResult = RGB(prof.nBoxColR, prof.nBoxColG, prof.nBoxColB);
      cc.lpCustColors = s_acrCustColors;
      cc.Flags = CC_FULLOPEN | CC_RGBINIT;

      if (ChooseColorW(&cc)) {
        prof.nBoxColR = GetRValue(cc.rgbResult);
        prof.nBoxColG = GetGValue(cc.rgbResult);
        prof.nBoxColB = GetBValue(cc.rgbResult);
        UpdateEditControls(m_nSelectedRow);
      }
    }
    return 0;

  // Ease mode radio buttons — toggle manually (base class doesn't know groups)
  case IDC_MW_TEXTANIM_EASE_LINEAR:
    SetChecked(IDC_MW_TEXTANIM_EASE_LINEAR, true);
    SetChecked(IDC_MW_TEXTANIM_EASE_IN, false);
    SetChecked(IDC_MW_TEXTANIM_EASE_OUT, false);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_LINEAR), NULL, TRUE);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_IN), NULL, TRUE);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_OUT), NULL, TRUE);
    return 0;

  case IDC_MW_TEXTANIM_EASE_IN:
    SetChecked(IDC_MW_TEXTANIM_EASE_LINEAR, false);
    SetChecked(IDC_MW_TEXTANIM_EASE_IN, true);
    SetChecked(IDC_MW_TEXTANIM_EASE_OUT, false);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_LINEAR), NULL, TRUE);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_IN), NULL, TRUE);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_OUT), NULL, TRUE);
    return 0;

  case IDC_MW_TEXTANIM_EASE_OUT:
    SetChecked(IDC_MW_TEXTANIM_EASE_LINEAR, false);
    SetChecked(IDC_MW_TEXTANIM_EASE_IN, false);
    SetChecked(IDC_MW_TEXTANIM_EASE_OUT, true);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_LINEAR), NULL, TRUE);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_IN), NULL, TRUE);
    InvalidateRect(GetDlgItem(hWnd, IDC_MW_TEXTANIM_EASE_OUT), NULL, TRUE);
    return 0;

  case IDC_MW_TEXTANIM_ENABLED:
    // Checkbox was auto-toggled by base class — no extra handling needed
    return 0;
  case IDC_MW_TEXTANIM_BOLD:
  case IDC_MW_TEXTANIM_ITALIC:
  case IDC_MW_TEXTANIM_RAND_POS:
  case IDC_MW_TEXTANIM_RAND_SIZE:
  case IDC_MW_TEXTANIM_RAND_COLOR:
  case IDC_MW_TEXTANIM_RAND_GROWTH:
  case IDC_MW_TEXTANIM_RAND_DURATION:
    return 0;
  }

  return -1;
}

//----------------------------------------------------------------------
// DoNotify — handle ListView selection changes
//----------------------------------------------------------------------

LRESULT TextAnimWindow::DoNotify(HWND hWnd, NMHDR* pnm)
{
  if (pnm->idFrom == IDC_MW_TEXTANIM_LIST) {
    if (pnm->code == LVN_ITEMCHANGED) {
      NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
      if ((pnmlv->uChanged & LVIF_STATE) &&
          (pnmlv->uNewState & LVIS_SELECTED) &&
          !(pnmlv->uOldState & LVIS_SELECTED)) {
        // New selection — save old, load new
        if (m_nSelectedRow >= 0 && m_nSelectedRow < m_pEngine->m_nAnimProfileCount)
          SaveEditControls();
        UpdateEditControls(pnmlv->iItem);
      }
      return 0;
    }
    // Dark header painting
    if (pnm->code == NM_CUSTOMDRAW) {
      bool handled = false;
      LRESULT lr = PaintDarkListViewHeader(pnm, 0, m_hList,
        RGB(45, 45, 48), RGB(70, 70, 74), RGB(220, 220, 220), &handled);
      if (handled) return lr;
    }
  }
  return -1;
}

//----------------------------------------------------------------------
// DoDestroy
//----------------------------------------------------------------------

void TextAnimWindow::DoDestroy()
{
  // Save current edits before closing
  if (m_nSelectedRow >= 0 && m_nSelectedRow < m_pEngine->m_nAnimProfileCount)
    SaveEditControls();
  m_hList = NULL;
  m_nSelectedRow = -1;
}

} // namespace mdrop
