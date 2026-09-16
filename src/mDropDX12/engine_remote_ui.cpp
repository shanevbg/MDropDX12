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
#include "format_to.h"
#include "pipe_server.h"
#include <commdlg.h>
#include <commctrl.h>
#include "config_store.h"
#include "pin_auth.h"
#include "AutoWide.h"

// These globals are defined in App.cpp in the global namespace (after 'using namespace mdrop')
extern PipeServer g_pipeServer;
extern TcpServer g_tcpServer;
extern WCHAR g_szLastIPCMessage[2048];
extern WCHAR g_szLastIPCTime[16];
extern std::atomic<int> g_lastIPCMessageSeq;

namespace mdrop {

extern Engine g_engine;

#define PAGE_CTRL(page, expr) TrackPageControl(page, (expr))
// Same, plus an anchor, for a control whose handle is not otherwise kept.
#define PAGE_CTRL_ANCHORED(page, edges, expr) do { HWND h_ = (expr); TrackPageControl(page, h_); AnchorControl(h_, edges); } while (0)

//----------------------------------------------------------------------
// Remote PIN verification
//
// Shared by both auth handlers -- this one and the one App.cpp installs when
// it starts the TCP server -- so the check exists once. See forgejo#44 for why
// it was a stub until now.
//----------------------------------------------------------------------
bool Engine::VerifyRemotePin(const std::string& submittedUtf8) {
  wchar_t stored[128] = {};
  Config().GetStringTo(L"Network", L"PinHash", L"", stored, 128);
  if (stored[0] == L'\0') return true;  // no PIN configured: nothing to check

  AutoWide wide(submittedUtf8.c_str());
  const wchar_t* p = wide;                     // null if the conversion failed
  const std::wstring submitted = p ? p : L"";
  if (!PinVerify(submitted, stored)) return false;

  // Correct PIN. If the stored value predates hashing, replace it now that we
  // know the cleartext is right. A child instance and a testing-mode session
  // both hold the config write shield up, so this quietly does nothing there
  // and simply migrates on the next ordinary run.
  if (PinHashIsLegacy(stored)) {
    const std::wstring hashed = PinHashCreate(submitted);
    if (!hashed.empty()) {
      Config().SetString(L"Network", L"PinHash", hashed.c_str());
      DebugLogW(L"Remote PIN migrated from plaintext to a salted hash", LOG_INFO);
    }
  }
  return true;
}

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

RemoteWindow::RemoteWindow(Engine* pEngine)
  // DEFAULT 520x600, MINIMUM 260x300 (in TOOLWINDOW_META). Those used to be the
  // same number, which is the whole reason this window could never be made
  // smaller -- the minimum is what the resize handler clamps to, so a default
  // that doubles as the minimum is a floor nobody can get under.
  //
  // 260x300 was tried as the default and is too small to work in; it stays as
  // the floor, so the window can be dragged down to it deliberately.
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
    {
      HWND hTitleEdit = CreateEdit(hw, p->m_szWindowTitle, IDC_MW_IPC_TITLE,
                                   tabX + lw + 4, py, tabRW - lw - 4, lineH, hFont);
      PAGE_CTRL(0, hTitleEdit);
      AnchorControl(hTitleEdit, kAnchorStretchWide);   // widen, do not grow taller
    }
    py += lineH + 2;

    // Hint
    PAGE_CTRL(0, CreateLabelTip(hw, L"(empty = \"MDropDX12 Visualizer\")",
        L"Leave this empty and the window is titled \"MDropDX12 Visualizer\". Set it to something a controller looks for, e.g. \"Milkwave Visualizer\".",
        tabX + lw + 4, py, tabRW - lw - 4, lineH, hFont));
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

    // IPC list box -- takes whatever height is left.
    //
    // Sized from the tab's own bottom rather than as a fixed lineH * 6, which
    // is why this window used to show a band of empty space below its content
    // at any size above the minimum: the controls were laid out to a fixed
    // height and the rest of the client area was simply unused.
    //
    // The "Last message" block keeps its fixed height at the bottom, so the
    // list absorbs the difference. The anchors set below do the same thing on
    // RESIZE; this is the same layout decided at BUILD time, and the two agree
    // by construction because both reserve the same fixed block.
    {
      const int kMsgBlockH = lineH * 5;
      const int kBottomPad = 8;
      int listH = (rcTab.bottom - kBottomPad) - py - gap - kMsgBlockH;
      if (listH < lineH * 4) listH = lineH * 4;   // never collapse to nothing
      HWND hIPCList = CreateListBox(hw, IDC_MW_IPC_LIST, tabX, py, tabRW, listH, hFont,
        WS_VSCROLL | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT);
      PAGE_CTRL(0, hIPCList);
      // This is the section that grows. Anchoring preserves the DISTANCE to an
      // edge, so a bottom-anchored list keeps the gap that the "Last message"
      // block occupies below it -- the list gets every extra pixel of height
      // and the block below keeps its own size and slides down.
      AnchorControl(hIPCList, kAnchorFill);
      py += listH + gap;
    }

    // Last message -- fixed height, sits at the bottom. Same constant the list
    // above reserved; a second literal here is how the two would drift apart.
    {
      const int groupH = lineH * 5;

      // A heading, not a BS_GROUPBOX.
      //
      // A group box is a BUTTON, and the dark theme strips visual styles from
      // buttons so the parent can paint them -- but a style-less group box
      // falls back to the CLASSIC renderer, which draws its frame with
      // COLOR_3DHILIGHT and erases the strip behind its label with
      // COLOR_BTNFACE. Both are white. On this window that produced a white
      // band across the text and a white frame around the box below it, which
      // is what Shane saw: "the text on the window looks bad ... the text for
      // the last message".
      //
      // The other two sections here are plain bold headings and render
      // correctly, so this now matches them rather than being the one control
      // that needs a renderer nothing else on the window uses.
      // Created directly rather than through CreateLabel, which takes no
      // control id: this one needs IDC_MW_IPC_MSG_GROUP, because the timestamp
      // is written into it every time a message arrives.
      HWND hHeading = CreateWindowExW(0, L"STATIC", L"Last message:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        tabX, py, tabRW, lineH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_GROUP,
        GetModuleHandle(NULL), NULL);
      if (hHeading && hFontBold)
        SendMessage(hHeading, WM_SETFONT, (WPARAM)hFontBold, TRUE);
      PAGE_CTRL(0, hHeading);
      // Fixed height, stuck to the bottom edge and stretching wide: a detail
      // pane, not a growing one. The list above absorbs the extra height.
      AnchorControl(hHeading, kAnchorStretchBottom);

      int pad = 8;
      HWND hMsgText = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        tabX + pad, py + lineH + 2, tabRW - pad * 2, groupH - lineH - pad - 2,
        hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_TEXT,
        GetModuleHandle(NULL), NULL);
      if (hMsgText && hFont) SendMessage(hMsgText, WM_SETFONT, (WPARAM)hFont, TRUE);
      PAGE_CTRL(0, hMsgText);
      AnchorControl(hMsgText, kAnchorStretchBottom);
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
      bool enabled = Config().GetInt(L"Network", L"TcpEnabled", 1) != 0;
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
      FormatTo(portBuf, L"%d", Config().GetInt(L"Network", L"TcpPort", 9270));
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

    // Connected Clients
    PAGE_CTRL(1, CreateLabel(hw, L"Clients", tabX, py, tabRW, lineH, hFontBold, false));
    py += lineH + gap;

    // ListView for connected clients with checkboxes
    {
      // Same treatment as the IPC list: fill down to just above the
      // Disconnect button, which keeps its own height at the bottom.
      const int kBtnBlockH = lineH + 8;
      const int kBottomPad = 8;
      int listH = (rcTab.bottom - kBottomPad) - py - gap - kBtnBlockH;
      if (listH < lineH * 4) listH = lineH * 4;
      HWND hList = CreateThemedListView(IDC_MW_TCP_DEVICE_LIST,
        tabX, py, tabRW, listH, false, false);
      PAGE_CTRL(1, hList);
      // The informational section on this page. Same reasoning as the IPC list.
      AnchorControl(hList, kAnchorFill);

      // Enable checkboxes
      DWORD exStyle = (DWORD)SendMessageW(hList, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0);
      SendMessageW(hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, exStyle | LVS_EX_CHECKBOXES);

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

      col.pszText = (LPWSTR)L"Status";
      col.cx = MulDiv(tabRW * 25, 1, 100);
      SendMessageW(hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

      py += listH + gap;
    }

    // Disconnect button
    int removeW = MulDiv(100, lineH, 26);
    PAGE_CTRL_ANCHORED(1, kAnchorBottomLeft, CreateBtn(hw, L"Disconnect", IDC_MW_TCP_DEVICE_REMOVE,
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
    HWND hTitleEdit = GetDlgItem(hWnd, IDC_MW_IPC_TITLE);
    if (hTitleEdit) {
      GetWindowTextW(hTitleEdit, buf, 256);
      CopyTo(p->m_szWindowTitle, buf);
    }
    const wchar_t* pIni = p->GetConfigIniFile();
    Config().SetString(L"Milkwave", L"WindowTitle", p->m_szWindowTitle);
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
    FormatTo(suggestedName, L"%04d%02d%02d-%02d%02d%02d-%s.png",
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, presetName);

    static wchar_t sLastDir[MAX_PATH] = {};
    if (sLastDir[0] == L'\0') {
      FormatTo(sLastDir, L"%scapture\\", p->m_szBaseDir);
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
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
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
    Config().SetString(L"Network", L"TcpEnabled", enabled ? L"1" : L"0");

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
      Config().SetString(L"Network", L"TcpPort", portBuf);

      // Get the render window handle for PostMessage dispatch
      extern std::atomic<HWND> g_hRenderWindow;
      HWND hwndRender = g_hRenderWindow.load();
      g_tcpServer.LoadAuthorizedDevices(pIni);
      g_tcpServer.Start(port,
        [hwndRender](TcpClientConnection& client, const std::wstring& msg) {
          g_respondingTcpClient = &client;
          wchar_t* copy = (wchar_t*)malloc((msg.size() + 1) * sizeof(wchar_t));
          if (copy) {
            wcsncpy_s(copy, msg.size() + 1, msg.c_str(), _TRUNCATE);
            PostMessageW(hwndRender, WM_MW_IPC_MESSAGE, 1, (LPARAM)copy);
          }
        },
        [pIni](TcpClientConnection& client, const std::string& pin,
               const std::string& deviceId, const std::string& deviceName) {
          client.deviceId = deviceId;
          client.deviceName = deviceName;

          // A wrong PIN is rejected here, before the authorized-device check,
          // so a known device cannot skip it.
          if (!g_engine.VerifyRemotePin(pin)) {
            client.authState = TcpAuthState::Unauthenticated;
            g_tcpServer.SendTo(client, "AUTH_FAIL|BADPIN");
            return;
          }
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
      Config().SetString(L"Network", L"TcpEnabled", L"1");
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
      Config().SetString(L"Network", L"PinHash", L"");
    } else {
      const std::wstring hashed = PinHashCreate(pinBuf);
      if (hashed.empty()) {
        // The system RNG or the hash failed. Storing the PIN in the clear is
        // what this used to do and is worse than refusing, so leave whatever
        // was already configured alone and say so.
        DebugLogW(L"Could not hash the remote PIN; the setting was left unchanged", LOG_ERROR);
      } else {
        Config().SetString(L"Network", L"PinHash", hashed.c_str());
      }
    }

    // Clear the edit field after applying
    if (hPinEdit) SetWindowTextW(hPinEdit, L"");
    RefreshTcpStatus();
    return 0;
  }

  // Disconnect client
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
  // Handle checkbox click on client list
  if (pnm->idFrom == IDC_MW_TCP_DEVICE_LIST && pnm->code == LVN_ITEMCHANGED && !m_bRefreshingList) {
    NMLISTVIEW* pnmv = (NMLISTVIEW*)pnm;
    // Only handle check state changes (not selection changes)
    if ((pnmv->uChanged & LVIF_STATE) &&
        ((pnmv->uOldState ^ pnmv->uNewState) & LVIS_STATEIMAGEMASK)) {
      bool nowChecked = ((pnmv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
      int idx = pnmv->iItem;

      // Get device ID from column 1
      HWND hList = pnm->hwndFrom;
      wchar_t deviceIdW[256] = {};
      LVITEMW item = {};
      item.mask = LVIF_TEXT;
      item.iItem = idx;
      item.iSubItem = 1;
      item.pszText = deviceIdW;
      item.cchTextMax = 256;
      SendMessageW(hList, LVM_GETITEMTEXTW, idx, (LPARAM)&item);

      std::string deviceId = WideToUTF8(deviceIdW);
      if (deviceId.empty()) return 0;

      Engine* p = m_pEngine;
      if (nowChecked) {
        // Authorize: approve pending client and add to authorized list
        g_tcpServer.ApproveDevice(deviceId);
        // Get client name for persisted record
        auto clients = g_tcpServer.GetConnectedClients();
        std::string name;
        for (auto& c : clients) {
          if (c.deviceId == deviceId) { name = c.deviceName; break; }
        }
        g_tcpServer.AddAuthorizedDevice(deviceId, name);
        g_tcpServer.SaveAuthorizedDevices(p->GetConfigIniFile());
      } else {
        // Deauthorize: remove from authorized list, deny the client
        g_tcpServer.RemoveAuthorizedDevice(deviceId);
        g_tcpServer.DenyDevice(deviceId);
        g_tcpServer.SaveAuthorizedDevices(p->GetConfigIniFile());
      }
      RefreshDeviceList();
      return 0;
    }
  }
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
      FormatTo(header, L"Last message: %s", g_szLastIPCTime);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_IPC_MSG_GROUP), header);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_IPC_MSG_TEXT), g_szLastIPCMessage);
    }

    // Also refresh TCP status and client list on the Authorization tab
    RefreshTcpStatus();
    RefreshDeviceList();
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
      FormatTo(entry, L"%s  \u2014  %d client%s", g_pipeServer.GetPipeName(),
               nClients, nClients == 1 ? L"" : L"s");
    else
      FormatTo(entry, L"%s  \u2014  Listening", g_pipeServer.GetPipeName());
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
    FormatTo(buf, L"Running on port %d  \u2014  %d client%s",
             g_tcpServer.GetPort(), nClients, nClients == 1 ? L"" : L"s");
  } else {
    FormatTo(buf, L"Stopped");
  }
  SetWindowTextW(hStatus, buf);
}

//----------------------------------------------------------------------
// RefreshDeviceList
//----------------------------------------------------------------------

void RemoteWindow::RefreshDeviceList() {
  HWND hList = GetDlgItem(m_hWnd, IDC_MW_TCP_DEVICE_LIST);
  if (!hList) return;

  m_bRefreshingList = true; // suppress LVN_ITEMCHANGED during rebuild

  // Remember which item was selected
  int prevSel = (int)SendMessageW(hList, LVM_GETNEXTITEM, -1, LVNI_SELECTED);

  SendMessageW(hList, LVM_DELETEALLITEMS, 0, 0);

  auto clients = g_tcpServer.GetConnectedClients();
  for (int i = 0; i < (int)clients.size(); i++) {
    std::wstring nameW = clients[i].deviceName.empty()
      ? L"(unknown)" : UTF8ToWide(clients[i].deviceName);
    std::wstring idW = clients[i].deviceId.empty()
      ? L"" : UTF8ToWide(clients[i].deviceId);

    const wchar_t* statusStr = L"Unauthenticated";
    if (clients[i].authState == TcpAuthState::Pending)
      statusStr = L"Pending";
    else if (clients[i].authState == TcpAuthState::Authenticated)
      statusStr = L"Authorized";

    LVITEMW item = {};
    item.mask = LVIF_TEXT;
    item.iItem = i;
    item.iSubItem = 0;
    item.pszText = (LPWSTR)nameW.c_str();
    SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&item);

    // Set checkbox: checked = Authorized
    ListView_SetCheckState(hList, i,
      clients[i].authState == TcpAuthState::Authenticated);

    LVITEMW sub = {};
    sub.mask = LVIF_TEXT;
    sub.iItem = i;
    sub.iSubItem = 1;
    sub.pszText = (LPWSTR)idW.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, i, (LPARAM)&sub);
    sub.iSubItem = 2;
    sub.pszText = (LPWSTR)statusStr;
    SendMessageW(hList, LVM_SETITEMTEXTW, i, (LPARAM)&sub);
  }

  // Restore selection
  if (prevSel >= 0 && prevSel < (int)clients.size()) {
    ListView_SetItemState(hList, prevSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }

  m_bRefreshingList = false;
}

} // namespace mdrop
