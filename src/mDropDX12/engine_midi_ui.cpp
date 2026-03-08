/*
  MidiWindow — MIDI controller mapping window (ToolWindow subclass).
  Shows 50 mapping slots in a ListView, with device selection, Learn mode,
  and button/knob action editing. Launched from "MIDI..." button on General tab.

  Engine MIDI functions (JSON persistence, device lifecycle, dispatch) are in
  engine_midi.cpp.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <commctrl.h>
#include <algorithm>

namespace mdrop {

extern Engine g_engine;

//======================================================================
// MidiWindow — ToolWindow subclass
//======================================================================

MidiWindow::MidiWindow(Engine* pEngine)
  : ToolWindow(pEngine, 600, 700) {}

//----------------------------------------------------------------------
// Engine bridge: Open/Close
//----------------------------------------------------------------------

void Engine::OpenMidiWindow() {
  if (!m_midiWindow)
    m_midiWindow = std::make_unique<MidiWindow>(this);
  m_midiWindow->Open();
}

void Engine::CloseMidiWindow() {
  if (m_midiWindow)
    m_midiWindow->Close();
}

//----------------------------------------------------------------------
// Common control flags
//----------------------------------------------------------------------

DWORD MidiWindow::GetCommonControlFlags() const {
  return ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS;
}

//----------------------------------------------------------------------
// Knob action name lookup
//----------------------------------------------------------------------

const wchar_t* MidiWindow::KnobActionName(MidiKnobAction id) {
  switch (id) {
  case MIDI_KNOB_AMP_L:      return L"Amp L";
  case MIDI_KNOB_AMP_R:      return L"Amp R";
  case MIDI_KNOB_TIME:       return L"Time";
  case MIDI_KNOB_FPS:        return L"FPS";
  case MIDI_KNOB_INTENSITY:  return L"Intensity";
  case MIDI_KNOB_SHIFT:      return L"Shift";
  case MIDI_KNOB_QUALITY:    return L"Quality";
  case MIDI_KNOB_HUE:        return L"Hue";
  case MIDI_KNOB_SATURATION: return L"Saturation";
  case MIDI_KNOB_BRIGHTNESS: return L"Brightness";
  case MIDI_KNOB_OPACITY:    return L"Opacity";
  default:                   return L"(none)";
  }
}

//----------------------------------------------------------------------
// PopulateListView — fill all 50 rows from m_midiRows
//----------------------------------------------------------------------

void MidiWindow::PopulateListView()
{
  if (!m_hList) return;
  ListView_DeleteAllItems(m_hList);

  Engine* p = m_pEngine;
  if (p->m_midiRows.empty()) return;

  for (int i = 0; i < (int)p->m_midiRows.size(); i++) {
    UpdateListViewRow(i);
  }
}

//----------------------------------------------------------------------
// UpdateListViewRow — insert or update a single row
//----------------------------------------------------------------------

void MidiWindow::UpdateListViewRow(int idx)
{
  if (!m_hList) return;
  Engine* p = m_pEngine;
  if (idx < 0 || idx >= (int)p->m_midiRows.size()) return;
  const MidiRow& row = p->m_midiRows[idx];

  // Check if item exists (for update vs insert)
  int count = ListView_GetItemCount(m_hList);
  bool insert = (idx >= count);

  wchar_t buf[32];

  // Column 0: # (row number, 1-based)
  swprintf(buf, 32, L"%d", idx + 1);
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

  // Column 1: Active
  LVITEMW lvi = {};
  lvi.iSubItem = 1;
  lvi.pszText = (LPWSTR)(row.bActive ? L"\u2713" : L"");
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 2: Label
  wchar_t wLabel[64];
  MultiByteToWideChar(CP_UTF8, 0, row.szLabel, -1, wLabel, 64);
  lvi.iSubItem = 2;
  lvi.pszText = wLabel;
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 3: Ch
  swprintf(buf, 32, L"%d", row.nChannel);
  lvi.iSubItem = 3;
  lvi.pszText = (row.actionType != MIDI_TYPE_UNDEFINED) ? buf : (LPWSTR)L"";
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 4: Value/Note
  swprintf(buf, 32, L"%d", row.nValue);
  lvi.iSubItem = 4;
  lvi.pszText = (row.actionType == MIDI_TYPE_BUTTON) ? buf : (LPWSTR)L"";
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 5: CC
  swprintf(buf, 32, L"%d", row.nController);
  lvi.iSubItem = 5;
  lvi.pszText = (row.actionType == MIDI_TYPE_KNOB) ? buf : (LPWSTR)L"";
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 6: Type
  const wchar_t* typeStr = L"";
  if (row.actionType == MIDI_TYPE_BUTTON) typeStr = L"Button";
  else if (row.actionType == MIDI_TYPE_KNOB) typeStr = L"Knob";
  lvi.iSubItem = 6;
  lvi.pszText = (LPWSTR)typeStr;
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

  // Column 7: Action
  const wchar_t* actionStr = L"";
  wchar_t wAction[256];
  if (row.actionType == MIDI_TYPE_BUTTON) {
    MultiByteToWideChar(CP_UTF8, 0, row.szActionText, -1, wAction, 256);
    actionStr = wAction;
  } else if (row.actionType == MIDI_TYPE_KNOB) {
    actionStr = KnobActionName(row.knobAction);
  }
  lvi.iSubItem = 7;
  lvi.pszText = (LPWSTR)actionStr;
  SendMessageW(m_hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
}

//----------------------------------------------------------------------
// UpdateEditControls — populate detail area for selected row
//----------------------------------------------------------------------

void MidiWindow::UpdateEditControls(int sel)
{
  HWND hw = m_hWnd;
  if (!hw) return;
  Engine* p = m_pEngine;
  m_nSelectedRow = sel;

  bool valid = (sel >= 0 && sel < (int)p->m_midiRows.size());
  const MidiRow& row = valid ? p->m_midiRows[sel] : MidiRow{};

  // Label
  if (m_hLabelEdit) {
    wchar_t wLabel[64];
    MultiByteToWideChar(CP_UTF8, 0, row.szLabel, -1, wLabel, 64);
    SetWindowTextW(m_hLabelEdit, valid ? wLabel : L"");
    EnableWindow(m_hLabelEdit, valid);
  }

  // Type combo
  if (m_hTypeCombo) {
    int typeSel = 0;
    if (row.actionType == MIDI_TYPE_BUTTON) typeSel = 1;
    else if (row.actionType == MIDI_TYPE_KNOB) typeSel = 2;
    SendMessage(m_hTypeCombo, CB_SETCURSEL, typeSel, 0);
    EnableWindow(m_hTypeCombo, valid);
  }

  // Action combo
  if (valid)
    PopulateActionCombo(row.actionType);
  if (m_hActionCombo) {
    if (row.actionType == MIDI_TYPE_BUTTON) {
      wchar_t wAction[256];
      MultiByteToWideChar(CP_UTF8, 0, row.szActionText, -1, wAction, 256);
      SetWindowTextW(m_hActionCombo, wAction);
    } else if (row.actionType == MIDI_TYPE_KNOB) {
      // Find matching knob action in combo
      int count = (int)SendMessage(m_hActionCombo, CB_GETCOUNT, 0, 0);
      for (int i = 0; i < count; i++) {
        if ((int)SendMessage(m_hActionCombo, CB_GETITEMDATA, i, 0) == (int)row.knobAction) {
          SendMessage(m_hActionCombo, CB_SETCURSEL, i, 0);
          break;
        }
      }
    } else {
      SetWindowTextW(m_hActionCombo, L"");
    }
    EnableWindow(m_hActionCombo, valid && row.actionType != MIDI_TYPE_UNDEFINED);
  }

  // Increment
  if (m_hIncrementEdit) {
    bool showInc = valid && row.actionType == MIDI_TYPE_KNOB;
    wchar_t incBuf[32];
    swprintf(incBuf, 32, L"%.3f", row.fIncrement);
    SetWindowTextW(m_hIncrementEdit, showInc ? incBuf : L"");
    ShowWindow(m_hIncrementEdit, showInc ? SW_SHOW : SW_HIDE);
    // Also show/hide the label before it
    HWND hIncLabel = GetDlgItem(hw, IDC_MW_MIDI_INCREMENT + 100); // marker
    if (hIncLabel) ShowWindow(hIncLabel, showInc ? SW_SHOW : SW_HIDE);
  }
}

//----------------------------------------------------------------------
// SaveEditControls — save detail area back to the selected MidiRow
//----------------------------------------------------------------------

void MidiWindow::SaveEditControls()
{
  Engine* p = m_pEngine;
  int sel = m_nSelectedRow;
  if (sel < 0 || sel >= (int)p->m_midiRows.size()) return;
  MidiRow& row = p->m_midiRows[sel];

  // Label
  if (m_hLabelEdit) {
    wchar_t wLabel[64] = {};
    GetWindowTextW(m_hLabelEdit, wLabel, 64);
    WideCharToMultiByte(CP_UTF8, 0, wLabel, -1, row.szLabel, sizeof(row.szLabel), NULL, NULL);
  }

  // Type
  if (m_hTypeCombo) {
    int typeSel = (int)SendMessage(m_hTypeCombo, CB_GETCURSEL, 0, 0);
    if (typeSel == 1) row.actionType = MIDI_TYPE_BUTTON;
    else if (typeSel == 2) row.actionType = MIDI_TYPE_KNOB;
    else row.actionType = MIDI_TYPE_UNDEFINED;
  }

  // Action
  if (m_hActionCombo) {
    if (row.actionType == MIDI_TYPE_BUTTON) {
      wchar_t wAction[256] = {};
      GetWindowTextW(m_hActionCombo, wAction, 256);
      WideCharToMultiByte(CP_UTF8, 0, wAction, -1, row.szActionText, sizeof(row.szActionText), NULL, NULL);
    } else if (row.actionType == MIDI_TYPE_KNOB) {
      int cbSel = (int)SendMessage(m_hActionCombo, CB_GETCURSEL, 0, 0);
      if (cbSel >= 0)
        row.knobAction = (MidiKnobAction)(int)SendMessage(m_hActionCombo, CB_GETITEMDATA, cbSel, 0);
    }
  }

  // Increment
  if (m_hIncrementEdit && row.actionType == MIDI_TYPE_KNOB) {
    wchar_t incBuf[32] = {};
    GetWindowTextW(m_hIncrementEdit, incBuf, 32);
    float val = 0.02f;
    try { val = std::stof(incBuf); } catch (...) {}
    if (val < 0.001f) val = 0.001f;
    if (val > 1.0f) val = 1.0f;
    row.fIncrement = val;
  }

  UpdateListViewRow(sel);
}

//----------------------------------------------------------------------
// PopulateDeviceCombo
//----------------------------------------------------------------------

void MidiWindow::PopulateDeviceCombo()
{
  if (!m_hDeviceCombo) return;
  SendMessage(m_hDeviceCombo, CB_RESETCONTENT, 0, 0);

  int n = MidiInput::GetNumDevices();
  int selIdx = -1;
  for (int i = 0; i < n; i++) {
    wchar_t name[256] = {};
    MidiInput::GetDeviceName(i, name, 256);
    int idx = (int)SendMessageW(m_hDeviceCombo, CB_ADDSTRING, 0, (LPARAM)name);
    SendMessage(m_hDeviceCombo, CB_SETITEMDATA, idx, (LPARAM)i);
    if (i == m_pEngine->m_nMidiDeviceID)
      selIdx = idx;
  }

  if (selIdx >= 0)
    SendMessage(m_hDeviceCombo, CB_SETCURSEL, selIdx, 0);
  else if (n > 0)
    SendMessage(m_hDeviceCombo, CB_SETCURSEL, 0, 0);
}

//----------------------------------------------------------------------
// PopulateActionCombo — fill with button commands or knob actions
//----------------------------------------------------------------------

void MidiWindow::PopulateActionCombo(MidiActionType type)
{
  if (!m_hActionCombo) return;
  SendMessage(m_hActionCombo, CB_RESETCONTENT, 0, 0);

  if (type == MIDI_TYPE_BUTTON) {
    // Make it a dropdown with editable text (CBS_DROPDOWN)
    // Pre-populate with common commands
    const wchar_t* cmds[] = {
      L"NEXT", L"PREV", L"HARDCUT", L"LOCK", L"RAND", L"MASHUP",
      L"FULLSCREEN", L"SETTINGS", L"STRETCH", L"MIRROR", L"RESET",
      L"RESETWINDOW", L"CAPTURE", L"SPOUT", L"BLACKOUT", L"PRESETINFO", NULL
    };
    for (int i = 0; cmds[i]; i++)
      SendMessageW(m_hActionCombo, CB_ADDSTRING, 0, (LPARAM)cmds[i]);

    // Also load from midi-default.txt if it exists
    std::vector<std::string> userActions;
    m_pEngine->LoadMidiDefaultActions(userActions);
    for (auto& a : userActions) {
      wchar_t wa[256];
      MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, wa, 256);
      // Don't add duplicates
      if (SendMessageW(m_hActionCombo, CB_FINDSTRINGEXACT, -1, (LPARAM)wa) == CB_ERR)
        SendMessageW(m_hActionCombo, CB_ADDSTRING, 0, (LPARAM)wa);
    }
  }
  else if (type == MIDI_TYPE_KNOB) {
    // Fixed list of knob actions
    struct { MidiKnobAction id; const wchar_t* name; } knobs[] = {
      { MIDI_KNOB_HUE,        L"Hue" },
      { MIDI_KNOB_SATURATION, L"Saturation" },
      { MIDI_KNOB_BRIGHTNESS, L"Brightness" },
      { MIDI_KNOB_INTENSITY,  L"Intensity" },
      { MIDI_KNOB_SHIFT,      L"Shift" },
      { MIDI_KNOB_TIME,       L"Time" },
      { MIDI_KNOB_FPS,        L"FPS" },
      { MIDI_KNOB_AMP_L,      L"Amp L" },
      { MIDI_KNOB_AMP_R,      L"Amp R" },
      { MIDI_KNOB_QUALITY,    L"Quality" },
      { MIDI_KNOB_OPACITY,    L"Opacity" },
    };
    for (auto& k : knobs) {
      int idx = (int)SendMessageW(m_hActionCombo, CB_ADDSTRING, 0, (LPARAM)k.name);
      SendMessage(m_hActionCombo, CB_SETITEMDATA, idx, (LPARAM)(int)k.id);
    }
  }
}

//----------------------------------------------------------------------
// Learn mode
//----------------------------------------------------------------------

void MidiWindow::StartLearn()
{
  Engine* p = m_pEngine;
  if (m_nSelectedRow < 0) {
    p->AddNotification(L"Select a row first");
    return;
  }
  m_bLearning = true;
  m_nLearnRow = m_nSelectedRow;

  // Redirect MIDI callback to this window for learning
  if (p->m_midiInput.IsOpen())
    p->m_midiInput.SetNotifyWnd(m_hWnd);

  SetWindowTextW(GetDlgItem(m_hWnd, IDC_MW_MIDI_LEARN), L"Learning...");
  p->AddNotification(L"Press a MIDI button or turn a knob...");
}

void MidiWindow::StopLearn()
{
  Engine* p = m_pEngine;
  m_bLearning = false;
  m_nLearnRow = -1;

  // Restore MIDI callback to render window
  if (p->m_midiInput.IsOpen()) {
    HWND hRender = p->GetPluginWindow();
    if (hRender)
      p->m_midiInput.SetNotifyWnd(hRender);
  }

  SetWindowTextW(GetDlgItem(m_hWnd, IDC_MW_MIDI_LEARN), L"Learn");
}

//----------------------------------------------------------------------
// OnMidiData — handle incoming MIDI message (learn or dispatch)
//----------------------------------------------------------------------

void MidiWindow::OnMidiData(LPARAM lParam)
{
  Engine* p = m_pEngine;
  BYTE status = (BYTE)(lParam & 0xFF);
  BYTE data1 = (BYTE)((lParam >> 8) & 0xFF);
  BYTE data2 = (BYTE)((lParam >> 16) & 0xFF);
  BYTE cmd = status & 0xF0;
  int channel = (status & 0x0F) + 1;

  if (m_bLearning && m_nLearnRow >= 0 && m_nLearnRow < (int)p->m_midiRows.size()) {
    MidiRow& row = p->m_midiRows[m_nLearnRow];
    row.nChannel = channel;
    row.bActive = true;

    if (cmd == 0x90) {
      // NoteOn → Button
      row.actionType = MIDI_TYPE_BUTTON;
      row.nValue = data1;
      row.nController = 0;
    } else if (cmd == 0xB0) {
      // CC → Knob
      row.actionType = MIDI_TYPE_KNOB;
      row.nController = data1;
      row.nValue = 0;
      if (row.knobAction == MIDI_KNOB_NONE)
        row.knobAction = MIDI_KNOB_HUE;  // default knob target
    }

    StopLearn();
    UpdateListViewRow(m_nLearnRow);
    UpdateEditControls(m_nLearnRow);
    p->AddNotification(L"MIDI input learned");
    return;
  }

  // Not learning — dispatch to matching active rows
  for (auto& row : p->m_midiRows) {
    if (!row.bActive) continue;
    if (row.nChannel != 0 && row.nChannel != channel) continue;

    if (row.actionType == MIDI_TYPE_BUTTON && cmd == 0x90 && data1 == row.nValue && data2 > 0) {
      p->ExecuteMidiButton(row);
    }
    else if (row.actionType == MIDI_TYPE_KNOB && cmd == 0xB0 && data1 == row.nController) {
      p->ExecuteMidiKnob(row, data2);
    }
  }
}

//----------------------------------------------------------------------
// DoBuildControls
//----------------------------------------------------------------------

void MidiWindow::DoBuildControls()
{
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  HFONT hFontBold = GetFontBold();

  // Title
  TrackControl(CreateLabel(hw, L"MIDI", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // Device row: "Device:" label + combo + Scan + Enable
  int labelW = MulDiv(55, lineH, 26);
  TrackControl(CreateLabel(hw, L"Device:", x, y, labelW, lineH, hFont));

  int btnW = MulDiv(50, lineH, 26);
  int chkW = MulDiv(70, lineH, 26);
  int comboW = rw - labelW - btnW - chkW - 16;

  m_hDeviceCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
    x + labelW, y, comboW, lineH * 8, hw,
    (HMENU)(INT_PTR)IDC_MW_MIDI_DEVICE, GetModuleHandle(NULL), NULL);
  if (m_hDeviceCombo && hFont) SendMessage(m_hDeviceCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
  TrackControl(m_hDeviceCombo);

  TrackControl(CreateBtn(hw, L"Scan", IDC_MW_MIDI_SCAN,
    x + labelW + comboW + 4, y, btnW, lineH, hFont));
  TrackControl(CreateCheck(hw, L"Enable", IDC_MW_MIDI_ENABLE,
    x + labelW + comboW + btnW + 12, y, chkW, lineH, hFont, m_pEngine->m_bMidiEnabled));
  y += lineH + gap + 2;

  PopulateDeviceCombo();

  // ListView (report mode, 8 columns)
  RECT rc;
  GetClientRect(hw, &rc);
  int listH = rc.bottom - y - lineH * 5 - gap * 6 - 20;  // leave room for edit area
  if (listH < lineH * 5) listH = lineH * 5;

  m_hList = CreateThemedListView(IDC_MW_MIDI_LIST, x, y, rw, listH);
  TrackControl(m_hList);
  if (m_hList) {
    // Column widths proportional to rw
    int colW[] = {
      MulDiv(rw, 6, 100),   // #
      MulDiv(rw, 5, 100),   // Active
      MulDiv(rw, 16, 100),  // Label
      MulDiv(rw, 6, 100),   // Ch
      MulDiv(rw, 7, 100),   // Val
      MulDiv(rw, 7, 100),   // CC
      MulDiv(rw, 10, 100),  // Type
      0                      // Action (remainder)
    };
    colW[7] = rw - colW[0] - colW[1] - colW[2] - colW[3] - colW[4] - colW[5] - colW[6]
              - GetSystemMetrics(SM_CXVSCROLL) - 4;

    const wchar_t* colNames[] = { L"#", L"\u2713", L"Label", L"Ch", L"Val", L"CC", L"Type", L"Action" };
    for (int i = 0; i < 8; i++) {
      LVCOLUMNW col = {};
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = (LPWSTR)colNames[i];
      col.cx = colW[i];
      SendMessageW(m_hList, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }

    PopulateListView();
    if (!m_pEngine->m_midiRows.empty())
      ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
  y += listH + gap + 2;

  // Detail editing area
  // Row 1: Label + Type + Action
  int editLabelW = MulDiv(45, lineH, 26);
  int typeW = MulDiv(90, lineH, 26);
  int actionW = rw - editLabelW - typeW - MulDiv(50, lineH, 26) - MulDiv(55, lineH, 26) - 24;

  TrackControl(CreateLabel(hw, L"Label:", x, y, editLabelW, lineH, hFont));
  m_hLabelEdit = CreateEdit(hw, L"", IDC_MW_MIDI_LABEL, x + editLabelW + 4, y,
    rw - editLabelW - 4, lineH, hFont, ES_AUTOHSCROLL);
  TrackControl(m_hLabelEdit);
  y += lineH + gap;

  // Row 2: Type combo + Action combo
  TrackControl(CreateLabel(hw, L"Type:", x, y, editLabelW, lineH, hFont));
  m_hTypeCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
    x + editLabelW + 4, y, typeW, lineH * 4, hw,
    (HMENU)(INT_PTR)IDC_MW_MIDI_TYPE, GetModuleHandle(NULL), NULL);
  if (m_hTypeCombo && hFont) SendMessage(m_hTypeCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
  SendMessageW(m_hTypeCombo, CB_ADDSTRING, 0, (LPARAM)L"(none)");
  SendMessageW(m_hTypeCombo, CB_ADDSTRING, 0, (LPARAM)L"Button");
  SendMessageW(m_hTypeCombo, CB_ADDSTRING, 0, (LPARAM)L"Knob");
  TrackControl(m_hTypeCombo);

  int actionX = x + editLabelW + typeW + 12;
  int actionLabelW = MulDiv(50, lineH, 26);
  TrackControl(CreateLabel(hw, L"Action:", actionX, y, actionLabelW, lineH, hFont));
  m_hActionCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL,
    actionX + actionLabelW + 4, y, rw - (actionX - x) - actionLabelW - 4, lineH * 10, hw,
    (HMENU)(INT_PTR)IDC_MW_MIDI_ACTION, GetModuleHandle(NULL), NULL);
  if (m_hActionCombo && hFont) SendMessage(m_hActionCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
  TrackControl(m_hActionCombo);
  y += lineH + gap;

  // Row 3: Increment (for knobs, initially hidden) + buffer
  int incLabelW = MulDiv(70, lineH, 26);
  int incEditW = MulDiv(60, lineH, 26);
  TrackControl(CreateLabel(hw, L"Increment:", x, y, incLabelW, lineH, hFont, false));
  m_hIncrementEdit = CreateEdit(hw, L"0.020", IDC_MW_MIDI_INCREMENT,
    x + incLabelW + 4, y, incEditW, lineH, hFont, 0, false);
  TrackControl(m_hIncrementEdit);

  // Buffer delay
  int bufX = x + rw - MulDiv(130, lineH, 26);
  int bufLabelW = MulDiv(55, lineH, 26);
  int bufEditW = MulDiv(45, lineH, 26);
  TrackControl(CreateLabel(hw, L"Buffer:", bufX, y, bufLabelW, lineH, hFont));
  wchar_t bufVal[16];
  swprintf(bufVal, 16, L"%d", m_pEngine->m_nMidiBufferDelay);
  TrackControl(CreateEdit(hw, bufVal, IDC_MW_MIDI_BUFFER,
    bufX + bufLabelW + 4, y, bufEditW, lineH, hFont));
  TrackControl(CreateLabel(hw, L"ms", bufX + bufLabelW + bufEditW + 8, y,
    MulDiv(20, lineH, 26), lineH, hFont));
  y += lineH + gap + 4;

  // Row 4: Buttons — Learn, Clear, Delete | Save, Load, Defaults
  btnW = MulDiv(70, lineH, 26);
  int btnGap = 6;
  int bx = x;
  TrackControl(CreateBtn(hw, L"Learn", IDC_MW_MIDI_LEARN, bx, y, btnW, lineH, hFont));
  bx += btnW + btnGap;
  TrackControl(CreateBtn(hw, L"Clear", IDC_MW_MIDI_CLEAR, bx, y, btnW, lineH, hFont));
  bx += btnW + btnGap;
  TrackControl(CreateBtn(hw, L"Delete", IDC_MW_MIDI_DELETE, bx, y, btnW, lineH, hFont));

  bx = x + rw - 3 * btnW - 2 * btnGap;
  TrackControl(CreateBtn(hw, L"Save", IDC_MW_MIDI_SAVE, bx, y, btnW, lineH, hFont));
  bx += btnW + btnGap;
  TrackControl(CreateBtn(hw, L"Load", IDC_MW_MIDI_LOAD, bx, y, btnW, lineH, hFont));
  bx += btnW + btnGap;
  TrackControl(CreateBtn(hw, L"Defaults", IDC_MW_MIDI_DEFAULTS, bx, y, btnW, lineH, hFont));

  // Initial selection
  UpdateEditControls(0);
}

//----------------------------------------------------------------------
// DoCommand — handle button clicks and combo changes
//----------------------------------------------------------------------

LRESULT MidiWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam)
{
  Engine* p = m_pEngine;

  // Enable checkbox
  if (id == IDC_MW_MIDI_ENABLE && code == BN_CLICKED) {
    bool enabled = IsChecked(IDC_MW_MIDI_ENABLE);
    p->m_bMidiEnabled = enabled;
    if (enabled) {
      // Get selected device
      int cbSel = (int)SendMessage(m_hDeviceCombo, CB_GETCURSEL, 0, 0);
      if (cbSel >= 0) {
        p->m_nMidiDeviceID = (int)SendMessage(m_hDeviceCombo, CB_GETITEMDATA, cbSel, 0);
        wchar_t name[256] = {};
        SendMessageW(m_hDeviceCombo, CB_GETLBTEXT, cbSel, (LPARAM)name);
        wcsncpy_s(p->m_szMidiDeviceName, name, _TRUNCATE);
      }
      p->OpenMidiDevice();
      if (p->m_midiInput.IsOpen())
        p->AddNotification(L"MIDI enabled");
      else {
        p->AddError(L"Failed to open MIDI device", 3.0f, ERR_MISC, false);
        p->m_bMidiEnabled = false;
        SetChecked(IDC_MW_MIDI_ENABLE, false);
      }
    } else {
      p->CloseMidiDevice();
      p->AddNotification(L"MIDI disabled");
    }
    p->SaveMidiSettings();
    return 0;
  }

  // Scan button
  if (id == IDC_MW_MIDI_SCAN && code == BN_CLICKED) {
    PopulateDeviceCombo();
    int n = MidiInput::GetNumDevices();
    wchar_t msg[64];
    swprintf(msg, 64, L"Found %d MIDI device%s", n, n == 1 ? L"" : L"s");
    p->AddNotification(msg);
    return 0;
  }

  // Device combo change
  if (id == IDC_MW_MIDI_DEVICE && code == CBN_SELCHANGE) {
    int cbSel = (int)SendMessage(m_hDeviceCombo, CB_GETCURSEL, 0, 0);
    if (cbSel >= 0) {
      p->m_nMidiDeviceID = (int)SendMessage(m_hDeviceCombo, CB_GETITEMDATA, cbSel, 0);
      wchar_t name[256] = {};
      SendMessageW(m_hDeviceCombo, CB_GETLBTEXT, cbSel, (LPARAM)name);
      wcsncpy_s(p->m_szMidiDeviceName, name, _TRUNCATE);
      // Reopen if enabled
      if (p->m_bMidiEnabled) {
        p->CloseMidiDevice();
        p->OpenMidiDevice();
      }
      p->SaveMidiSettings();
    }
    return 0;
  }

  // Learn button
  if (id == IDC_MW_MIDI_LEARN && code == BN_CLICKED) {
    if (m_bLearning)
      StopLearn();
    else {
      if (!p->m_midiInput.IsOpen()) {
        p->AddNotification(L"Enable MIDI first");
        return 0;
      }
      StartLearn();
    }
    return 0;
  }

  // Clear button — clear selected row's MIDI assignment
  if (id == IDC_MW_MIDI_CLEAR && code == BN_CLICKED) {
    int sel = m_nSelectedRow;
    if (sel >= 0 && sel < (int)p->m_midiRows.size()) {
      MidiRow& row = p->m_midiRows[sel];
      row.nChannel = 0;
      row.nValue = 0;
      row.nController = 0;
      row.actionType = MIDI_TYPE_UNDEFINED;
      row.knobAction = MIDI_KNOB_NONE;
      row.szActionText[0] = '\0';
      row.szLabel[0] = '\0';
      row.bActive = false;
      row.fIncrement = 0.02f;
      UpdateListViewRow(sel);
      UpdateEditControls(sel);
    }
    return 0;
  }

  // Delete button — same as Clear (keeping all 50 rows, just clear the slot)
  if (id == IDC_MW_MIDI_DELETE && code == BN_CLICKED) {
    return DoCommand(hWnd, IDC_MW_MIDI_CLEAR, BN_CLICKED, 0);
  }

  // Type combo change — update action combo and save
  if (id == IDC_MW_MIDI_TYPE && code == CBN_SELCHANGE) {
    SaveEditControls();
    UpdateEditControls(m_nSelectedRow);
    return 0;
  }

  // Action combo — save on selection or killfocus
  if (id == IDC_MW_MIDI_ACTION && (code == CBN_SELCHANGE || code == CBN_KILLFOCUS)) {
    SaveEditControls();
    return 0;
  }

  // Label edit — save on killfocus
  if (id == IDC_MW_MIDI_LABEL && code == EN_KILLFOCUS) {
    SaveEditControls();
    return 0;
  }

  // Increment edit — save on killfocus
  if (id == IDC_MW_MIDI_INCREMENT && code == EN_KILLFOCUS) {
    SaveEditControls();
    return 0;
  }

  // Buffer delay edit — save on killfocus
  if (id == IDC_MW_MIDI_BUFFER && code == EN_KILLFOCUS) {
    wchar_t buf[16] = {};
    GetWindowTextW(GetDlgItem(hWnd, IDC_MW_MIDI_BUFFER), buf, 16);
    int val = _wtoi(buf);
    if (val < 0) val = 0;
    if (val > 500) val = 500;
    p->m_nMidiBufferDelay = val;
    p->SaveMidiSettings();
    return 0;
  }

  // Save button
  if (id == IDC_MW_MIDI_SAVE && code == BN_CLICKED) {
    SaveEditControls();
    p->SaveMidiJSON();
    p->SaveMidiSettings();
    p->AddNotification(L"MIDI mappings saved");
    return 0;
  }

  // Load button
  if (id == IDC_MW_MIDI_LOAD && code == BN_CLICKED) {
    p->LoadMidiJSON();
    PopulateListView();
    UpdateEditControls(0);
    if (m_hList)
      ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    p->AddNotification(L"MIDI mappings loaded");
    return 0;
  }

  // Defaults button
  if (id == IDC_MW_MIDI_DEFAULTS && code == BN_CLICKED) {
    p->m_midiRows.assign(MIDI_NUM_ROWS, MidiRow{});
    for (int i = 0; i < MIDI_NUM_ROWS; i++)
      p->m_midiRows[i].nRow = i + 1;
    PopulateListView();
    UpdateEditControls(0);
    if (m_hList)
      ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    p->AddNotification(L"MIDI mappings reset to defaults");
    return 0;
  }

  return -1;
}

//----------------------------------------------------------------------
// DoNotify — ListView selection change
//----------------------------------------------------------------------

LRESULT MidiWindow::DoNotify(HWND hWnd, NMHDR* pnm)
{
  if (pnm->idFrom == IDC_MW_MIDI_LIST && pnm->code == LVN_ITEMCHANGED) {
    NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
    if (pnmlv->uNewState & LVIS_SELECTED) {
      // Save previous row first
      if (m_nSelectedRow >= 0)
        SaveEditControls();
      UpdateEditControls(pnmlv->iItem);
    }
    return 0;
  }
  return -1;
}

//----------------------------------------------------------------------
// DoMessage — custom messages
//----------------------------------------------------------------------

LRESULT MidiWindow::DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_MW_MIDI_DATA) {
    OnMidiData(lParam);
    return 0;
  }
  return -1;
}

//----------------------------------------------------------------------
// DoDestroy
//----------------------------------------------------------------------

void MidiWindow::DoDestroy()
{
  if (m_bLearning)
    StopLearn();
  m_hList = NULL;
  m_hDeviceCombo = NULL;
  m_hTypeCombo = NULL;
  m_hActionCombo = NULL;
  m_hLabelEdit = NULL;
  m_hIncrementEdit = NULL;
}

} // namespace mdrop
