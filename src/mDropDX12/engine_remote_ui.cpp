/*
  RemoteWindow — standalone Remote tool window (ToolWindow subclass).
  Extracted from the Settings Remote tab.
  Controls: window title config, IPC status, last message display.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include "pipe_server.h"
#include <commdlg.h>

// These globals are defined in App.cpp in the global namespace (after 'using namespace mdrop')
extern PipeServer g_pipeServer;
extern WCHAR g_szLastIPCMessage[2048];
extern WCHAR g_szLastIPCTime[16];
extern std::atomic<int> g_lastIPCMessageSeq;

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

RemoteWindow::RemoteWindow(Engine* pEngine)
  : ToolWindow(pEngine, 520, 600) {}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void RemoteWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  HFONT hFontBold = GetFontBold();
  int lw = MulDiv(120, lineH, 26);

  Engine* p = m_pEngine;

  // Section header
  TrackControl(CreateLabel(hw, L"Milkwave Remote Compatibility", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // Info line
  TrackControl(CreateLabel(hw, L"Configure window titles so Milkwave Remote (or other controllers) can discover this instance.", x, y, rw, lineH * 2, hFont));
  y += lineH * 2 + gap;

  // Window Title
  TrackControl(CreateLabel(hw, L"Window Title:", x, y, lw, lineH, hFont));
  TrackControl(CreateEdit(hw, p->m_szWindowTitle, IDC_MW_IPC_TITLE, x + lw + 4, y, rw - lw - 4, lineH, hFont));
  y += lineH + 2;

  // Hint
  TrackControl(CreateLabel(hw, L"(empty = \"MDropDX12 Visualizer\"  |  e.g. \"Milkwave Visualizer\")", x + lw + 4, y, rw - lw - 4, lineH, hFont));
  y += lineH + gap;

  // Apply button + Capture Screenshot button
  {
    int applyW = MulDiv(100, lineH, 26);
    TrackControl(CreateBtn(hw, L"Apply", IDC_MW_IPC_APPLY, x, y, applyW, lineH, hFont));
    int captureW = MulDiv(130, lineH, 26);
    TrackControl(CreateBtn(hw, L"Save Screenshot...", IDC_MW_IPC_CAPTURE, x + applyW + 8, y, captureW, lineH, hFont));
    y += lineH + gap + 8;
  }

  // Named Pipe IPC status
  TrackControl(CreateLabel(hw, L"Named Pipe IPC", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // IPC list box
  {
    int listH = lineH * 6;
    HWND hIPCList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
      x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_LIST, GetModuleHandle(NULL), NULL);
    if (hIPCList && hFont) SendMessage(hIPCList, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hIPCList);
    y += listH + gap;
  }

  // Last Message group box
  {
    int groupH = lineH * 5;
    HWND hGroup = CreateWindowExW(0, L"BUTTON", L"Last message:",
      WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
      x, y, rw, groupH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_GROUP,
      GetModuleHandle(NULL), NULL);
    if (hGroup && hFont) SendMessage(hGroup, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hGroup);

    int pad = 8;
    HWND hMsgText = CreateWindowExW(0, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
      x + pad, y + lineH + 2, rw - pad * 2, groupH - lineH - pad - 2,
      hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_TEXT,
      GetModuleHandle(NULL), NULL);
    if (hMsgText && hFont) SendMessage(hMsgText, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hMsgText);
  }

  // Populate IPC list with current state
  RefreshIPCList();

  // Start IPC message monitor timer (500ms polling)
  SetTimer(hw, IDT_IPC_MONITOR, 500, NULL);
}

//----------------------------------------------------------------------
// Command handler
//----------------------------------------------------------------------

LRESULT RemoteWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;

  // Apply & Restart IPC
  if (id == IDC_MW_IPC_APPLY && code == BN_CLICKED) {
    wchar_t buf[256];
    HWND hEdit = GetDlgItem(hWnd, IDC_MW_IPC_TITLE);
    if (hEdit) {
      GetWindowTextW(hEdit, buf, 256);
      lstrcpyW(p->m_szWindowTitle, buf);
    }
    const wchar_t* pIni = p->GetConfigIniFile();
    WritePrivateProfileStringW(L"Milkwave", L"WindowTitle", p->m_szWindowTitle, pIni);
    RefreshIPCList();
    return 0;
  }

  // Save Screenshot
  if (id == IDC_MW_IPC_CAPTURE && code == BN_CLICKED) {
    wchar_t presetName[MAX_PATH] = L"screenshot";
    if (p->m_szCurrentPresetFile[0]) {
      wchar_t* fn = wcsrchr(p->m_szCurrentPresetFile, L'\\');
      fn = fn ? fn + 1 : p->m_szCurrentPresetFile;
      wcsncpy_s(presetName, fn, _TRUNCATE);
      wchar_t* ext = wcsrchr(presetName, L'.');
      if (ext) *ext = L'\0';
      for (wchar_t* c = presetName; *c; c++)
        if (*c == L'/' || *c == L':' || *c == L'*' || *c == L'?' || *c == L'"' || *c == L'<' || *c == L'>' || *c == L'|')
          *c = L'_';
    }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t suggestedName[MAX_PATH];
    swprintf_s(suggestedName, L"%04d%02d%02d-%02d%02d%02d-%s.png",
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, presetName);

    static wchar_t sLastDir[MAX_PATH] = {};
    if (sLastDir[0] == L'\0') {
      swprintf_s(sLastDir, L"%scapture\\", p->m_szBaseDir);
      CreateDirectoryW(sLastDir, NULL);
    }

    wchar_t filePath[MAX_PATH];
    wcsncpy_s(filePath, suggestedName, _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = sLastDir;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Save Screenshot";

    if (GetSaveFileNameW(&ofn)) {
      wcsncpy_s(sLastDir, filePath, _TRUNCATE);
      wchar_t* lastSlash = wcsrchr(sLastDir, L'\\');
      if (lastSlash) lastSlash[1] = L'\0';

      wcsncpy_s(p->m_screenshotPath, filePath, _TRUNCATE);
      p->m_bScreenshotRequested = true;
    }
    return 0;
  }

  return -1;
}

//----------------------------------------------------------------------
// Message handler (timer)
//----------------------------------------------------------------------

LRESULT RemoteWindow::DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_TIMER && wParam == IDT_IPC_MONITOR) {
    int seq = g_lastIPCMessageSeq.load();
    if (seq != m_lastSeenIPCSeq) {
      m_lastSeenIPCSeq = seq;
      HWND hList = GetDlgItem(hWnd, IDC_MW_IPC_LIST);
      int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
      if (sel != LB_ERR) {
        wchar_t header[64];
        swprintf_s(header, L"Last message: %s", g_szLastIPCTime);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_IPC_MSG_GROUP), header);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_IPC_MSG_TEXT), g_szLastIPCMessage);
      }
    }
    return 0;
  }
  return -1;
}

//----------------------------------------------------------------------
// Destroy
//----------------------------------------------------------------------

void RemoteWindow::DoDestroy() {
  KillTimer(m_hWnd, IDT_IPC_MONITOR);
}

//----------------------------------------------------------------------
// RefreshIPCList
//----------------------------------------------------------------------

void RemoteWindow::RefreshIPCList() {
  HWND hList = GetDlgItem(m_hWnd, IDC_MW_IPC_LIST);
  if (!hList) return;

  SendMessage(hList, LB_RESETCONTENT, 0, 0);

  if (g_pipeServer.IsRunning()) {
    wchar_t entry[512];
    int nClients = g_pipeServer.GetClientCount();
    if (nClients > 0)
      swprintf_s(entry, L"%s  \u2014  %d client%s", g_pipeServer.GetPipeName(),
                 nClients, nClients == 1 ? L"" : L"s");
    else
      swprintf_s(entry, L"%s  \u2014  Listening", g_pipeServer.GetPipeName());
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)entry);
  } else {
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"(pipe server not running)");
  }
}

} // namespace mdrop
