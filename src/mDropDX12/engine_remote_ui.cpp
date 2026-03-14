/*
  RemoteWindow — standalone Remote tool window (ToolWindow subclass).
  Extracted from the Settings Remote tab.
  Tab 0 "IPC": window title config, pipe status, last message display.
  Tab 1 "Authorization": TCP server status, PIN config, authorized devices list.
*/

#include "tcp_server.h"  // Must be before engine.h — winsock2.h must precede windows.h
#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include "pipe_server.h"
#include <commdlg.h>
#include <commctrl.h>

// These globals are defined in App.cpp in the global namespace (after 'using namespace mdrop')
extern PipeServer g_pipeServer;
extern TcpServer g_tcpServer;
extern WCHAR g_szLastIPCMessage[2048];
extern WCHAR g_szLastIPCTime[16];
extern std::atomic<int> g_lastIPCMessageSeq;

namespace mdrop {

extern Engine g_engine;

#define PAGE_CTRL(page, expr) TrackPageControl(page, (expr))

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

RemoteWindow::RemoteWindow(Engine* pEngine)
  : ToolWindow(pEngine, 520, 600) {}

//----------------------------------------------------------------------
// Common control flags (need tab + listview)
//----------------------------------------------------------------------

DWORD RemoteWindow::GetCommonControlFlags() const {
  return ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void RemoteWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw, clientW = L.clientW;
  HFONT hFont = GetFont();
  HFONT hFontBold = GetFontBold();
  int lw = MulDiv(120, lineH, 26);

  Engine* p = m_pEngine;

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  int clientH = rcWnd.bottom;

  // ── Tab control ──
  const wchar_t* tabNames[] = { L"IPC", L"Authorization" };
  RECT rcTab = BuildTabControl(IDC_MW_REMOTE_TAB, tabNames, 2,
                                0, y, clientW, clientH - y);
  int tabX = rcTab.left + x;
  int tabY = rcTab.top + 4;
  int tabRW = rcTab.right - rcTab.left - x;

  // ════════════════════════════════════════════════════════════════════
  // Page 0: IPC
  // ════════════════════════════════════════════════════════════════════
  {
    int py = tabY;

    // Section header
    PAGE_CTRL(0, CreateLabel(hw, L"Milkwave Remote Compatibility", tabX, py, tabRW, lineH, hFontBold));
    py += lineH + gap;

    // Info line
    PAGE_CTRL(0, CreateLabel(hw, L"Configure window titles so Milkwave Remote (or other controllers) can discover this instance.", tabX, py, tabRW, lineH * 2, hFont));
    py += lineH * 2 + gap;

    // Window Title
    PAGE_CTRL(0, CreateLabel(hw, L"Window Title:", tabX, py, lw, lineH, hFont));
    PAGE_CTRL(0, CreateEdit(hw, p->m_szWindowTitle, IDC_MW_IPC_TITLE, tabX + lw + 4, py, tabRW - lw - 4, lineH, hFont));
    py += lineH + 2;

    // Hint
    PAGE_CTRL(0, CreateLabel(hw, L"(empty = \"MDropDX12 Visualizer\"  |  e.g. \"Milkwave Visualizer\")", tabX + lw + 4, py, tabRW - lw - 4, lineH, hFont));
    py += lineH + gap;

    // Apply button + Capture Screenshot button
    {
      int applyW = MulDiv(100, lineH, 26);
      PAGE_CTRL(0, CreateBtn(hw, L"Apply", IDC_MW_IPC_APPLY, tabX, py, applyW, lineH, hFont));
      int captureW = MulDiv(130, lineH, 26);
      PAGE_CTRL(0, CreateBtn(hw, L"Save Screenshot...", IDC_MW_IPC_CAPTURE, tabX + applyW + 8, py, captureW, lineH, hFont));
      py += lineH + gap + 8;
    }

    // Named Pipe IPC status
    PAGE_CTRL(0, CreateLabel(hw, L"Named Pipe IPC", tabX, py, tabRW, lineH, hFontBold));
    py += lineH + gap;

    // IPC list box
    {
      int listH = lineH * 6;
      HWND hIPCList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        tabX, py, tabRW, listH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_LIST, GetModuleHandle(NULL), NULL);
      if (hIPCList && hFont) SendMessage(hIPCList, WM_SETFONT, (WPARAM)hFont, TRUE);
      PAGE_CTRL(0, hIPCList);
      py += listH + gap;
    }

    // Last Message group box
    {
      int groupH = lineH * 5;
      HWND hGroup = CreateWindowExW(0, L"BUTTON", L"Last message:",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        tabX, py, tabRW, groupH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_GROUP,
        GetModuleHandle(NULL), NULL);
      if (hGroup && hFont) SendMessage(hGroup, WM_SETFONT, (WPARAM)hFont, TRUE);
      PAGE_CTRL(0, hGroup);

      int pad = 8;
      HWND hMsgText = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        tabX + pad, py + lineH + 2, tabRW - pad * 2, groupH - lineH - pad - 2,
        hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_TEXT,
        GetModuleHandle(NULL), NULL);
      if (hMsgText && hFont) SendMessage(hMsgText, WM_SETFONT, (WPARAM)hFont, TRUE);
      PAGE_CTRL(0, hMsgText);
    }
  }

  // ════════════════════════════════════════════════════════════════════
  // Page 1: Authorization
  // ════════════════════════════════════════════════════════════════════
  {
    int py = tabY;
    const wchar_t* pIni = p->GetConfigIniFile();

    // TCP Server header
    PAGE_CTRL(1, CreateLabel(hw, L"TCP Server", tabX, py, tabRW, lineH, hFontBold, false));
    py += lineH + gap;

    // Enable checkbox
    {
      bool enabled = GetPrivateProfileIntW(L"Network", L"TcpEnabled", 1, pIni) != 0;
      HWND hChk = CreateWindowExW(0, L"BUTTON", L"Enable TCP Server",
        WS_CHILD | BS_AUTOCHECKBOX,
        tabX, py, MulDiv(160, lineH, 26), lineH, hw, (HMENU)(INT_PTR)IDC_MW_TCP_ENABLED,
        GetModuleHandle(NULL), NULL);
      if (hChk && hFont) SendMessage(hChk, WM_SETFONT, (WPARAM)hFont, TRUE);
      SendMessage(hChk, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
      PAGE_CTRL(1, hChk);
    }
    py += lineH + gap;

    // Port + Start/Stop on same line
    {
      int portLW = MulDiv(35, lineH, 26);
      PAGE_CTRL(1, CreateLabel(hw, L"Port:", tabX, py, portLW, lineH, hFont, false));

      int portEditW = MulDiv(70, lineH, 26);
      wchar_t portBuf[16];
      swprintf_s(portBuf, L"%d", GetPrivateProfileIntW(L"Network", L"TcpPort", 9270, pIni));
      PAGE_CTRL(1, CreateEdit(hw, portBuf, IDC_MW_TCP_PORT_EDIT,
        tabX + portLW + 4, py, portEditW, lineH, hFont, ES_NUMBER, false));

      int btnW = MulDiv(80, lineH, 26);
      PAGE_CTRL(1, CreateBtn(hw, g_tcpServer.IsRunning() ? L"Stop" : L"Start", IDC_MW_TCP_STARTSTOP,
        tabX + portLW + portEditW + 12, py, btnW, lineH, hFont, false));
    }
    py += lineH + gap;

    // Status label
    {
      HWND hStatus = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT,
        tabX, py, tabRW, lineH, hw, (HMENU)(INT_PTR)IDC_MW_TCP_STATUS,
        GetModuleHandle(NULL), NULL);
      if (hStatus && hFont) SendMessage(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
      PAGE_CTRL(1, hStatus);
    }
    py += lineH + gap + 4;

    // PIN configuration
    PAGE_CTRL(1, CreateLabel(hw, L"PIN", tabX, py, tabRW, lineH, hFontBold, false));
    py += lineH + gap;

    {
      int pinLW = MulDiv(35, lineH, 26);
      PAGE_CTRL(1, CreateLabel(hw, L"PIN:", tabX, py, pinLW, lineH, hFont, false));
      int pinEditW = MulDiv(150, lineH, 26);
      PAGE_CTRL(1, CreateEdit(hw, L"", IDC_MW_TCP_PIN_EDIT,
        tabX + pinLW + 4, py, pinEditW, lineH, hFont, ES_PASSWORD, false));

      int applyW = MulDiv(80, lineH, 26);
      PAGE_CTRL(1, CreateBtn(hw, L"Apply", IDC_MW_TCP_PIN_APPLY,
        tabX + pinLW + pinEditW + 12, py, applyW, lineH, hFont, false));
    }
    py += lineH + gap + 8;

    // Authorized Devices
    PAGE_CTRL(1, CreateLabel(hw, L"Authorized Devices", tabX, py, tabRW, lineH, hFontBold, false));
    py += lineH + gap;

    // ListView for devices
    {
      int listH = lineH * 8;
      HWND hList = CreateThemedListView(IDC_MW_TCP_DEVICE_LIST,
        tabX, py, tabRW, listH, false, false);
      PAGE_CTRL(1, hList);

      // Add columns
      LVCOLUMNW col = {};
      col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
      col.fmt = LVCFMT_LEFT;

      col.pszText = (LPWSTR)L"Device Name";
      col.cx = MulDiv(tabRW * 40, 1, 100);
      SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);

      col.pszText = (LPWSTR)L"Device ID";
      col.cx = MulDiv(tabRW * 35, 1, 100);
      SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

      col.pszText = (LPWSTR)L"Date Added";
      col.cx = MulDiv(tabRW * 25, 1, 100);
      SendMessageW(hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

      py += listH + gap;
    }

    // Remove button
    int removeW = MulDiv(100, lineH, 26);
    PAGE_CTRL(1, CreateBtn(hw, L"Remove", IDC_MW_TCP_DEVICE_REMOVE,
      tabX, py, removeW, lineH, hFont, false));
  }

  // Restore persisted tab selection
  SelectInitialTab();

  // Populate IPC list with current state
  RefreshIPCList();

  // Populate Authorization tab
  RefreshTcpStatus();
  RefreshDeviceList();

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

  // Enable/Disable TCP
  if (id == IDC_MW_TCP_ENABLED && code == BN_CLICKED) {
    HWND hChk = GetDlgItem(hWnd, IDC_MW_TCP_ENABLED);
    bool enabled = SendMessage(hChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const wchar_t* pIni = p->GetConfigIniFile();
    WritePrivateProfileStringW(L"Network", L"TcpEnabled", enabled ? L"1" : L"0", pIni);

    if (!enabled && g_tcpServer.IsRunning()) {
      g_tcpServer.Stop();
    }
    RefreshTcpStatus();
    // Update Start/Stop button text
    HWND hBtn = GetDlgItem(hWnd, IDC_MW_TCP_STARTSTOP);
    if (hBtn) SetWindowTextW(hBtn, g_tcpServer.IsRunning() ? L"Stop" : L"Start");
    return 0;
  }

  // Start/Stop TCP server
  if (id == IDC_MW_TCP_STARTSTOP && code == BN_CLICKED) {
    const wchar_t* pIni = p->GetConfigIniFile();
    if (g_tcpServer.IsRunning()) {
      g_tcpServer.Stop();
    } else {
      // Read port from edit field
      wchar_t portBuf[16] = {};
      HWND hPortEdit = GetDlgItem(hWnd, IDC_MW_TCP_PORT_EDIT);
      if (hPortEdit) GetWindowTextW(hPortEdit, portBuf, 16);
      int port = _wtoi(portBuf);
      if (port < 1 || port > 65535) port = 9270;

      // Save port to INI
      WritePrivateProfileStringW(L"Network", L"TcpPort", portBuf, pIni);

      // Get the render window handle for PostMessage dispatch
      extern std::atomic<HWND> g_hRenderWindow;
      HWND hwndRender = g_hRenderWindow.load();
      g_tcpServer.LoadAuthorizedDevices(pIni);
      g_tcpServer.Start(port,
        [hwndRender](TcpClientConnection& client, const std::wstring& msg) {
          g_respondingTcpClient = &client;
          wchar_t* copy = (wchar_t*)malloc((msg.size() + 1) * sizeof(wchar_t));
          if (copy) {
            wcscpy_s(copy, msg.size() + 1, msg.c_str());
            PostMessageW(hwndRender, WM_MW_IPC_MESSAGE, 1, (LPARAM)copy);
          }
        },
        [pIni](TcpClientConnection& client, const std::string& pin,
               const std::string& deviceId, const std::string& deviceName) {
          wchar_t storedHash[128] = {};
          GetPrivateProfileStringW(L"Network", L"PinHash", L"", storedHash, 128, pIni);
          bool pinConfigured = storedHash[0] != L'\0';
          if (pinConfigured) {
            // TODO: SHA256 verification
          }
          client.deviceId = deviceId;
          client.deviceName = deviceName;
          if (g_tcpServer.IsDeviceAuthorized(deviceId)) {
            client.authState = TcpAuthState::Authenticated;
            g_tcpServer.SendTo(client, "AUTH_OK");
          } else {
            client.authState = TcpAuthState::Pending;
            g_tcpServer.SendTo(client, "AUTH_PENDING");
          }
        }
      );

      // Update enable checkbox
      HWND hChk = GetDlgItem(hWnd, IDC_MW_TCP_ENABLED);
      if (hChk) SendMessage(hChk, BM_SETCHECK, BST_CHECKED, 0);
      WritePrivateProfileStringW(L"Network", L"TcpEnabled", L"1", pIni);
    }
    RefreshTcpStatus();
    HWND hBtn = GetDlgItem(hWnd, IDC_MW_TCP_STARTSTOP);
    if (hBtn) SetWindowTextW(hBtn, g_tcpServer.IsRunning() ? L"Stop" : L"Start");
    return 0;
  }

  // Apply PIN
  if (id == IDC_MW_TCP_PIN_APPLY && code == BN_CLICKED) {
    wchar_t pinBuf[128] = {};
    HWND hPinEdit = GetDlgItem(hWnd, IDC_MW_TCP_PIN_EDIT);
    if (hPinEdit) {
      GetWindowTextW(hPinEdit, pinBuf, 128);
    }

    const wchar_t* pIni = p->GetConfigIniFile();

    if (pinBuf[0] == L'\0') {
      // Clear PIN
      WritePrivateProfileStringW(L"Network", L"PinHash", L"", pIni);
    } else {
      // TODO: Compute SHA256 hash of PIN using BCryptHashData
      // For now, store the PIN directly as a placeholder
      WritePrivateProfileStringW(L"Network", L"PinHash", pinBuf, pIni);
    }

    // Clear the edit field after applying
    if (hPinEdit) SetWindowTextW(hPinEdit, L"");
    RefreshTcpStatus();
    return 0;
  }

  // Remove authorized device
  if (id == IDC_MW_TCP_DEVICE_REMOVE && code == BN_CLICKED) {
    HWND hList = GetDlgItem(hWnd, IDC_MW_TCP_DEVICE_LIST);
    if (!hList) return 0;

    int sel = (int)SendMessageW(hList, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
    if (sel < 0) return 0;

    // Get the device ID from column 1
    wchar_t deviceIdW[256] = {};
    LVITEMW item = {};
    item.mask = LVIF_TEXT;
    item.iItem = sel;
    item.iSubItem = 1;
    item.pszText = deviceIdW;
    item.cchTextMax = 256;
    SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&item);

    // Convert wide to narrow for the API
    std::string deviceId = WideToUTF8(deviceIdW);

    g_tcpServer.RemoveAuthorizedDevice(deviceId);
    g_tcpServer.DisconnectDevice(deviceId);
    g_tcpServer.SaveAuthorizedDevices(p->GetConfigIniFile());

    RefreshDeviceList();
    RefreshTcpStatus();
    return 0;
  }

  return -1;
}

//----------------------------------------------------------------------
// Notify handler (listview)
//----------------------------------------------------------------------

LRESULT RemoteWindow::DoNotify(HWND hWnd, NMHDR* pnm) {
  // Tab switching is handled by base class (TCN_SELCHANGE)
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
      RefreshIPCList();
      wchar_t header[64];
      swprintf_s(header, L"Last message: %s", g_szLastIPCTime);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_IPC_MSG_GROUP), header);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_IPC_MSG_TEXT), g_szLastIPCMessage);
    }

    // Also refresh TCP status on the Authorization tab
    RefreshTcpStatus();
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

//----------------------------------------------------------------------
// RefreshTcpStatus
//----------------------------------------------------------------------

void RemoteWindow::RefreshTcpStatus() {
  HWND hStatus = GetDlgItem(m_hWnd, IDC_MW_TCP_STATUS);
  if (!hStatus) return;

  wchar_t buf[256];
  if (g_tcpServer.IsRunning()) {
    int nClients = g_tcpServer.GetClientCount();
    swprintf_s(buf, L"Running on port %d  \u2014  %d client%s",
               g_tcpServer.GetPort(), nClients, nClients == 1 ? L"" : L"s");
  } else {
    swprintf_s(buf, L"Stopped");
  }
  SetWindowTextW(hStatus, buf);
}

//----------------------------------------------------------------------
// RefreshDeviceList
//----------------------------------------------------------------------

void RemoteWindow::RefreshDeviceList() {
  HWND hList = GetDlgItem(m_hWnd, IDC_MW_TCP_DEVICE_LIST);
  if (!hList) return;

  SendMessageW(hList, LVM_DELETEALLITEMS, 0, 0);

  auto devices = g_tcpServer.GetAuthorizedDevices();
  for (int i = 0; i < (int)devices.size(); i++) {
    // Convert narrow strings to wide
    std::wstring nameW = UTF8ToWide(devices[i].name);
    std::wstring idW = UTF8ToWide(devices[i].id);
    std::wstring dateW = UTF8ToWide(devices[i].dateAdded);

    LVITEMW item = {};
    item.mask = LVIF_TEXT;
    item.iItem = i;
    item.iSubItem = 0;
    item.pszText = (LPWSTR)nameW.c_str();
    SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&item);

    LVITEMW sub = {};
    sub.mask = LVIF_TEXT;
    sub.iItem = i;
    sub.iSubItem = 1;
    sub.pszText = (LPWSTR)idW.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, i, (LPARAM)&sub);
    sub.iSubItem = 2;
    sub.pszText = (LPWSTR)dateW.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, i, (LPARAM)&sub);
  }
}

} // namespace mdrop
