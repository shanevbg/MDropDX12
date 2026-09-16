/*
  ScriptWindow — standalone Script tool window (ToolWindow subclass).
  Extracted from the Settings Script tab.
  Controls: file browser, play/stop/loop, BPM/beats, script listbox.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include "format_to.h"
#include <commdlg.h>

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

ScriptWindow::ScriptWindow(Engine* pEngine)
  : ToolWindow(pEngine, 480, 600) {}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void ScriptWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  int lw = MulDiv(100, lineH, 26);

  Engine* p = m_pEngine;

  // Script file path + Browse
  TrackControl(CreateLabel(hw, L"Script File:", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, L"", IDC_MW_SCRIPT_FILE, x + lw + 4, y, rw - lw - 4 - 80 - 4, lineH, hFont, ES_READONLY));
  {
    // In 26ths: a flat 80 fitted "Browse..." at a line height of 26 and cut
    // it at anything larger. It grows leftwards from the right edge, so the
    // row is unchanged (issue 159).
    const int brwW = MulDiv(80, lineH, 26);
    TrackControl(CreateBtn(hw, L"Browse...", IDC_MW_SCRIPT_BROWSE, x + rw - brwW, y, brwW, lineH, hFont));
  }
  y += lineH + gap;

  // Play / Stop / Loop
  {
    int btnW = MulDiv(80, lineH, 26);
    TrackControl(CreateBtn(hw, L"Play", IDC_MW_SCRIPT_PLAY, x, y, btnW, lineH, hFont));
    TrackControl(CreateBtn(hw, L"Stop", IDC_MW_SCRIPT_STOP, x + btnW + 4, y, btnW, lineH, hFont));
    TrackControl(CreateCheck(hw, L"Loop", IDC_MW_SCRIPT_LOOP, x + btnW * 2 + 12, y, 80, lineH, hFont, p->m_script.loop));
    y += lineH + gap;
  }

  // BPM + Beats
  {
    wchar_t bpmBuf[32], beatsBuf[32];
    FormatTo(bpmBuf, L"%.1f", p->m_script.bpm);
    FormatTo(beatsBuf, L"%d", p->m_script.beats);
    // In 26ths. These were flat pixel counts, so the row kept its width
    // while the font grew and "BPM:" and "Beats:" were cut (issue 159).
    const int S = lineH;
    TrackControl(CreateLabel(hw, L"BPM:", x, y, MulDiv(40, S, 26), lineH, hFont));
    TrackControl(CreateEdit(hw, bpmBuf, IDC_MW_SCRIPT_BPM, x + MulDiv(44, S, 26), y, MulDiv(70, S, 26), lineH, hFont));
    TrackControl(CreateLabel(hw, L"Beats:", x + MulDiv(130, S, 26), y, MulDiv(50, S, 26), lineH, hFont));
    TrackControl(CreateEdit(hw, beatsBuf, IDC_MW_SCRIPT_BEATS, x + MulDiv(184, S, 26), y, MulDiv(50, S, 26), lineH, hFont));
    y += lineH + gap;
  }

  // Line status label
  {
    HWND hLineLabel = CreateWindowExW(0, L"STATIC", L"No script loaded",
      WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, rw, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_SCRIPT_LINE, GetModuleHandle(NULL), NULL);
    if (hLineLabel && hFont) SendMessage(hLineLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hLineLabel);
  }
  y += lineH + gap;

  // Script lines listbox
  {
    RECT rc;
    GetClientRect(hw, &rc);
    int listH = rc.bottom - y - 16;
    if (listH < lineH * 5) listH = lineH * 5;
    HWND hScriptList = CreateListBox(hw, IDC_MW_SCRIPT_LIST, x, y, rw, listH, hFont,
      WS_VSCROLL | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT);
    TrackControl(hScriptList);

    // Populate if script already loaded
    if (!p->m_script.lines.empty()) {
      for (int i = 0; i < (int)p->m_script.lines.size(); i++)
        SendMessageW(hScriptList, LB_ADDSTRING, 0, (LPARAM)p->m_script.lines[i].c_str());
    }
  }

  // Sync current state
  p->SyncScriptUI();
}

//----------------------------------------------------------------------
// Command handler
//----------------------------------------------------------------------

LRESULT ScriptWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;

  // Browse
  if (id == IDC_MW_SCRIPT_BROWSE && code == BN_CLICKED) {
    wchar_t filePath[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Script Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = p->m_szBaseDir;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Open Script File";
    if (GetOpenFileNameW(&ofn)) {
      p->LoadScript(filePath);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_SCRIPT_FILE), filePath);
      HWND hList = GetDlgItem(hWnd, IDC_MW_SCRIPT_LIST);
      if (hList) {
        SendMessage(hList, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < (int)p->m_script.lines.size(); i++)
          SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)p->m_script.lines[i].c_str());
      }
    }
    return 0;
  }

  // Play
  if (id == IDC_MW_SCRIPT_PLAY && code == BN_CLICKED) {
    wchar_t buf[64];
    HWND hBpm = GetDlgItem(hWnd, IDC_MW_SCRIPT_BPM);
    if (hBpm) { GetWindowTextW(hBpm, buf, 64); double v = _wtof(buf); if (v > 0) p->m_script.bpm = v; }
    HWND hBeats = GetDlgItem(hWnd, IDC_MW_SCRIPT_BEATS);
    if (hBeats) { GetWindowTextW(hBeats, buf, 64); int v = _wtoi(buf); if (v > 0) p->m_script.beats = v; }
    p->StartScript();
    return 0;
  }

  // Stop
  if (id == IDC_MW_SCRIPT_STOP && code == BN_CLICKED) {
    p->StopScript();
    return 0;
  }

  // Loop checkbox
  if (id == IDC_MW_SCRIPT_LOOP && code == BN_CLICKED) {
    p->m_script.loop = IsChecked(id);
    return 0;
  }

  // Listbox double-click to jump
  if (id == IDC_MW_SCRIPT_LIST && code == LBN_DBLCLK) {
    int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)p->m_script.lines.size()) {
      p->m_script.currentLine = sel;
      p->m_script.lastLineTime = p->GetTime();
      p->ExecuteScriptLine(sel);
      p->SyncScriptUI();
    }
    return 0;
  }

  return -1;
}

} // namespace mdrop
