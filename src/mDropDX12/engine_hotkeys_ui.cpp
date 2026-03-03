/*
  HotkeysWindow — Keyboard Shortcuts window (ToolWindow subclass).
  Shows all configurable hotkey bindings in a ListView, with Add/Edit/Delete
  buttons for dynamic user entries (Script Commands and Launch Apps).
  Key assignment and editing happen in a modal dialog.
  Launched from "Hotkeys..." button on the Settings System tab, or Ctrl+F7.
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

HotkeysWindow::HotkeysWindow(Engine* pEngine)
  : ToolWindow(pEngine, 680, 580) {}

//----------------------------------------------------------------------
// Engine bridge: Open/Close via Engine members
//----------------------------------------------------------------------

void Engine::OpenHotkeysWindow() {
  if (!m_hotkeysWindow)
    m_hotkeysWindow = std::make_unique<HotkeysWindow>(this);
  m_hotkeysWindow->Open();
}

void Engine::CloseHotkeysWindow() {
  if (m_hotkeysWindow)
    m_hotkeysWindow->Close();
}

//----------------------------------------------------------------------
// Common control flags
//----------------------------------------------------------------------

DWORD HotkeysWindow::GetCommonControlFlags() const {
  return ICC_HOTKEY_CLASS | ICC_LISTVIEW_CLASSES;
}

//----------------------------------------------------------------------
// lParam encoding: bit 31 distinguishes built-in from user entries
//   built-in: lParam = index into m_hotkeys[]   (bit 31 = 0)
//   user:     lParam = 0x80000000 | index into m_userHotkeys[]
//----------------------------------------------------------------------

static constexpr LPARAM USER_LPARAM_BIT = 0x80000000;

static bool IsUserLParam(LPARAM lp) { return (lp & USER_LPARAM_BIT) != 0; }
static int  UserIndex(LPARAM lp)    { return (int)(lp & 0x7FFFFFFF); }

//----------------------------------------------------------------------
// Helper: refresh ListView contents from m_hotkeys[] + m_userHotkeys
//----------------------------------------------------------------------

// Column indices for the ListView
enum { COL_CATEGORY = 0, COL_ACTION, COL_SHORTCUT, COL_SCOPE };

// Sort state
static int  s_sortColumn = COL_CATEGORY;  // default: sort by category
static bool s_sortAscending = true;

static void RefreshHotkeyList(HWND hList, Engine* p)
{
  if (!hList) return;
  ListView_DeleteAllItems(hList);

  int row = 0;

  // Built-in hotkeys
  for (int i = 0; i < NUM_HOTKEYS; i++) {
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = row;
    lvi.iSubItem = 0;
    lvi.lParam = (LPARAM)i;  // bit 31 = 0 → built-in
    const wchar_t* catName = (p->m_hotkeys[i].category < HKCAT_COUNT)
        ? kCategoryNames[p->m_hotkeys[i].category] : L"?";
    lvi.pszText = (LPWSTR)catName;
    int idx = (int)SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

    // Action column
    lvi.mask = LVIF_TEXT;
    lvi.iItem = idx;
    lvi.iSubItem = COL_ACTION;
    lvi.pszText = (LPWSTR)p->m_hotkeys[i].szAction;
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

    // Shortcut column
    std::wstring shortcut = p->FormatHotkeyDisplay(p->m_hotkeys[i].modifiers, p->m_hotkeys[i].vk);
    lvi.iSubItem = COL_SHORTCUT;
    lvi.pszText = (LPWSTR)shortcut.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

    // Scope column
    const wchar_t* scope = (p->m_hotkeys[i].vk == 0) ? L"-" :
      (p->m_hotkeys[i].scope == HKSCOPE_GLOBAL ? L"Global" : L"Local");
    lvi.iSubItem = COL_SCOPE;
    lvi.pszText = (LPWSTR)scope;
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
    row++;
  }

  // User hotkeys (Script Commands + Launch Apps)
  for (int i = 0; i < (int)p->m_userHotkeys.size(); i++) {
    const auto& uh = p->m_userHotkeys[i];

    const wchar_t* catName = (uh.type == USER_HK_SCRIPT)
        ? kCategoryNames[HKCAT_SCRIPT] : kCategoryNames[HKCAT_LAUNCH];

    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = row;
    lvi.iSubItem = 0;
    lvi.lParam = (LPARAM)(USER_LPARAM_BIT | i);
    lvi.pszText = (LPWSTR)catName;
    int idx = (int)SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

    // Action: label + command preview in parens
    std::wstring actionName = uh.label;
    if (!uh.command.empty()) {
      if (uh.type == USER_HK_SCRIPT) {
        actionName += L" (" + uh.command + L")";
      } else {
        const wchar_t* exeName = wcsrchr(uh.command.c_str(), L'\\');
        if (!exeName) exeName = wcsrchr(uh.command.c_str(), L'/');
        exeName = exeName ? exeName + 1 : uh.command.c_str();
        actionName += L" (";
        actionName += exeName;
        actionName += L")";
      }
    }
    lvi.mask = LVIF_TEXT;
    lvi.iItem = idx;
    lvi.iSubItem = COL_ACTION;
    lvi.pszText = (LPWSTR)actionName.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

    std::wstring shortcut = p->FormatHotkeyDisplay(uh.modifiers, uh.vk);
    lvi.iSubItem = COL_SHORTCUT;
    lvi.pszText = (LPWSTR)shortcut.c_str();
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);

    const wchar_t* scope = (uh.vk == 0) ? L"-" :
      (uh.scope == HKSCOPE_GLOBAL ? L"Global" : L"Local");
    lvi.iSubItem = COL_SCOPE;
    lvi.pszText = (LPWSTR)scope;
    SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
    row++;
  }
}

//----------------------------------------------------------------------
// Sort comparison callback
//----------------------------------------------------------------------

struct SortContext {
  HWND hList;
  Engine* pEngine;
};

// Helpers to extract sort fields from either built-in or user entry
struct HKSortData {
  int category;
  const wchar_t* action;
  UINT vk, mod;
  int scope;
};

static HKSortData GetSortData(LPARAM lp, Engine* p)
{
  HKSortData d = {};
  if (IsUserLParam(lp)) {
    int idx = UserIndex(lp);
    if (idx >= 0 && idx < (int)p->m_userHotkeys.size()) {
      const auto& uh = p->m_userHotkeys[idx];
      d.category = (uh.type == USER_HK_SCRIPT) ? HKCAT_SCRIPT : HKCAT_LAUNCH;
      d.action = uh.label.c_str();
      d.vk = uh.vk;
      d.mod = uh.modifiers;
      d.scope = (int)uh.scope;
    }
  } else {
    int idx = (int)lp;
    if (idx >= 0 && idx < NUM_HOTKEYS) {
      d.category = (int)p->m_hotkeys[idx].category;
      d.action = p->m_hotkeys[idx].szAction;
      d.vk = p->m_hotkeys[idx].vk;
      d.mod = p->m_hotkeys[idx].modifiers;
      d.scope = (int)p->m_hotkeys[idx].scope;
    }
  }
  return d;
}

static int CALLBACK HotkeyListCompare(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
  SortContext* ctx = (SortContext*)lParamSort;
  HKSortData a = GetSortData(lParam1, ctx->pEngine);
  HKSortData b = GetSortData(lParam2, ctx->pEngine);

  int cmp = 0;
  switch (s_sortColumn) {
  case COL_CATEGORY:
    cmp = a.category - b.category;
    if (cmp == 0) cmp = _wcsicmp(a.action, b.action);
    break;
  case COL_ACTION:
    cmp = _wcsicmp(a.action, b.action);
    break;
  case COL_SHORTCUT:
    cmp = (int)a.vk - (int)b.vk;
    if (cmp == 0) cmp = (int)a.mod - (int)b.mod;
    break;
  case COL_SCOPE:
    cmp = a.scope - b.scope;
    break;
  }
  return s_sortAscending ? cmp : -cmp;
}

//----------------------------------------------------------------------
// Helper: save bindings and re-register global hotkeys
//----------------------------------------------------------------------

static void SaveAndReRegister(Engine* p)
{
  p->SaveHotkeySettings();
  p->GenerateHelpText();
  HWND hRender = p->GetPluginWindow();
  if (hRender)
    PostMessage(hRender, WM_MW_REGISTER_HOTKEYS, 0, 0);
}

//----------------------------------------------------------------------
// Helper: get lParam from ListView item
//----------------------------------------------------------------------

static LPARAM GetItemLParam(HWND hList, int iItem)
{
  LVITEMW lvi = {};
  lvi.mask = LVIF_PARAM;
  lvi.iItem = iItem;
  if (SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&lvi))
    return lvi.lParam;
  return -1;
}

static LPARAM GetSelectedLParam(HWND hList)
{
  int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
  if (sel < 0) return -1;
  return GetItemLParam(hList, sel);
}

//----------------------------------------------------------------------
// Edit Hotkey modal dialog
//----------------------------------------------------------------------

struct EditHotkeyData {
  bool isBuiltIn;
  int  index;              // into m_hotkeys[] or m_userHotkeys[]
  std::wstring actionName; // read-only display for built-in
  std::wstring label;      // editable for user entries
  UINT modifiers, vk;
  HotkeyScope scope;
  UserHotkeyType userType; // only for user entries
  std::wstring command;    // script cmd or app path
  Engine* pEngine;
  bool accepted;
};

// Dark theme brush for the dialog
static HBRUSH s_hDlgBrush = NULL;

static INT_PTR CALLBACK EditHotkeyDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
  EditHotkeyData* pData = (EditHotkeyData*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);

  switch (msg) {
  case WM_INITDIALOG:
  {
    pData = (EditHotkeyData*)lParam;
    SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)pData);

    bool isScript = !pData->isBuiltIn && pData->userType == USER_HK_SCRIPT;
    bool isLaunch = !pData->isBuiltIn && pData->userType == USER_HK_LAUNCH;

    // Title
    if (pData->isBuiltIn)
      SetWindowTextW(hDlg, L"Edit Hotkey");
    else if (isScript)
      SetWindowTextW(hDlg, L"Edit Script Command");
    else
      SetWindowTextW(hDlg, L"Edit Launch App");

    // Create child controls manually (bare dialog template has cdit=0)
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HINSTANCE hInst = GetModuleHandle(NULL);

    // Convert dialog units to pixels
    RECT duRect = { 0, 0, 4, 8 };
    MapDialogRect(hDlg, &duRect);
    int duX = duRect.right;   // pixels per 4 DLUs horizontal
    int duY = duRect.bottom;  // pixels per 8 DLUs vertical

    int margin = 12;
    RECT dlgRc;
    GetClientRect(hDlg, &dlgRc);
    int cw = dlgRc.right - 2 * margin;  // content width
    int lineH = MulDiv(14, duY, 8);
    int editH = lineH + 4;
    int gap = 6;
    int y = margin;
    int labelW = MulDiv(70, duX, 4);

    // Action label (read-only for built-in)
    CreateWindowExW(0, L"STATIC", L"Action:", WS_CHILD | WS_VISIBLE,
      margin, y + 2, labelW, lineH, hDlg, NULL, hInst, NULL);
    HWND hAction = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pData->actionName.c_str(),
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (pData->isBuiltIn ? ES_READONLY : 0),
      margin + labelW, y, cw - labelW, editH, hDlg,
      (HMENU)(INT_PTR)IDC_HK_EDIT_ACTION, hInst, NULL);
    SendMessageW(hAction, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += editH + gap;

    // Label (user entries only)
    if (!pData->isBuiltIn) {
      CreateWindowExW(0, L"STATIC", L"Label:", WS_CHILD | WS_VISIBLE,
        margin, y + 2, labelW, lineH, hDlg, NULL, hInst, NULL);
      HWND hLabel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pData->label.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        margin + labelW, y, cw - labelW, editH, hDlg,
        (HMENU)(INT_PTR)IDC_HK_EDIT_LABEL, hInst, NULL);
      SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
      y += editH + gap;
    }

    // Command field (Script type) — multiline, converts | to newlines for editing
    if (isScript) {
      CreateWindowExW(0, L"STATIC", L"Command:", WS_CHILD | WS_VISIBLE,
        margin, y + 2, labelW, lineH, hDlg, NULL, hInst, NULL);
      int browseW = MulDiv(60, duX, 4);
      int cmdH = editH * 5;  // ~5 lines for multiline

      // Convert pipes to newlines for display
      std::wstring displayCmd = pData->command;
      for (size_t pos = 0; (pos = displayCmd.find(L'|', pos)) != std::wstring::npos; pos += 2)
        displayCmd.replace(pos, 1, L"\r\n");

      HWND hCmd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", displayCmd.c_str(),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL,
        margin + labelW, y, cw - labelW - browseW - 6, cmdH, hDlg,
        (HMENU)(INT_PTR)IDC_HK_EDIT_COMMAND, hInst, NULL);
      SendMessageW(hCmd, WM_SETFONT, (WPARAM)hFont, TRUE);
      HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        margin + cw - browseW, y, browseW, editH, hDlg,
        (HMENU)(INT_PTR)IDC_HK_EDIT_BROWSE, hInst, NULL);
      SendMessageW(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
      y += cmdH + gap;
    }

    // Path field (Launch type)
    if (isLaunch) {
      CreateWindowExW(0, L"STATIC", L"App path:", WS_CHILD | WS_VISIBLE,
        margin, y + 2, labelW, lineH, hDlg, NULL, hInst, NULL);
      int browseW = MulDiv(60, duX, 4);
      HWND hPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pData->command.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        margin + labelW, y, cw - labelW - browseW - 6, editH, hDlg,
        (HMENU)(INT_PTR)IDC_HK_EDIT_PATH, hInst, NULL);
      SendMessageW(hPath, WM_SETFONT, (WPARAM)hFont, TRUE);
      HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        margin + cw - browseW, y, browseW, editH, hDlg,
        (HMENU)(INT_PTR)IDC_HK_EDIT_BROWSE, hInst, NULL);
      SendMessageW(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
      y += editH + gap;
    }

    // Separator
    y += 4;

    // Key combo label + HOTKEY_CLASS + Clear button
    CreateWindowExW(0, L"STATIC", L"Key:", WS_CHILD | WS_VISIBLE,
      margin, y + 2, labelW, lineH, hDlg, NULL, hInst, NULL);
    int clearW = MulDiv(50, duX, 4);
    HWND hHK = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      margin + labelW, y, cw - labelW - clearW - 6, editH, hDlg,
      (HMENU)(INT_PTR)IDC_HK_EDIT_HOTKEY, hInst, NULL);
    SendMessageW(hHK, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hClear = CreateWindowExW(0, L"BUTTON", L"Clear",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      margin + cw - clearW, y, clearW, editH, hDlg,
      (HMENU)(INT_PTR)IDC_HK_EDIT_CLEAR, hInst, NULL);
    SendMessageW(hClear, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += editH + gap;

    // Scope checkbox
    HWND hScope = CreateWindowExW(0, L"BUTTON", L"Global (system-wide)",
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
      margin, y, cw, lineH, hDlg,
      (HMENU)(INT_PTR)IDC_HK_EDIT_SCOPE, hInst, NULL);
    SendMessageW(hScope, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += lineH + gap + 8;

    // OK / Cancel buttons
    int btnW = MulDiv(70, duX, 4);
    int btnH = editH + 2;
    int totalBtnW = btnW * 2 + 12;
    int btnX = margin + (cw - totalBtnW) / 2;
    HWND hOK = CreateWindowExW(0, L"BUTTON", L"OK",
      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
      btnX, y, btnW, btnH, hDlg,
      (HMENU)(INT_PTR)IDOK, hInst, NULL);
    SendMessageW(hOK, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      btnX + btnW + 12, y, btnW, btnH, hDlg,
      (HMENU)(INT_PTR)IDCANCEL, hInst, NULL);
    SendMessageW(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += btnH + margin;

    // Resize dialog to fit content
    RECT wr;
    GetWindowRect(hDlg, &wr);
    int borderW = (wr.right - wr.left) - dlgRc.right;
    int borderH = (wr.bottom - wr.top) - dlgRc.bottom;
    SetWindowPos(hDlg, NULL, 0, 0,
      dlgRc.right + borderW, y + borderH,
      SWP_NOMOVE | SWP_NOZORDER);

    // Now populate the hotkey control
    if (hHK && pData->vk != 0) {
      UINT hkMod = 0;
      if (pData->modifiers & MOD_ALT)     hkMod |= HOTKEYF_ALT;
      if (pData->modifiers & MOD_CONTROL) hkMod |= HOTKEYF_CONTROL;
      if (pData->modifiers & MOD_SHIFT)   hkMod |= HOTKEYF_SHIFT;
      SendMessageW(hHK, HKM_SETHOTKEY, MAKEWORD(pData->vk, hkMod), 0);
    }

    // Scope checkbox
    CheckDlgButton(hDlg, IDC_HK_EDIT_SCOPE,
      pData->scope == HKSCOPE_GLOBAL ? BST_CHECKED : BST_UNCHECKED);

    // Apply dark theme
    s_hDlgBrush = CreateSolidBrush(RGB(30, 30, 30));

    // DWM dark title bar
    HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
    if (hDwm) {
      typedef HRESULT(WINAPI* DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
      auto fn = (DwmSetWindowAttribute_t)GetProcAddress(hDwm, "DwmSetWindowAttribute");
      if (fn) {
        BOOL useDark = TRUE;
        fn(hDlg, 20, &useDark, sizeof(useDark));
      }
    }

    // Center on parent
    HWND hParent = GetParent(hDlg);
    if (hParent) {
      RECT rp, rd;
      GetWindowRect(hParent, &rp);
      GetWindowRect(hDlg, &rd);
      int cx = rp.left + ((rp.right - rp.left) - (rd.right - rd.left)) / 2;
      int cy = rp.top + ((rp.bottom - rp.top) - (rd.bottom - rd.top)) / 2;
      SetWindowPos(hDlg, NULL, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    // Set font on static labels
    EnumChildWindows(hDlg, [](HWND hChild, LPARAM lp) -> BOOL {
      wchar_t cls[64];
      GetClassNameW(hChild, cls, 64);
      if (_wcsicmp(cls, L"Static") == 0)
        SendMessageW(hChild, WM_SETFONT, (WPARAM)lp, TRUE);
      return TRUE;
    }, (LPARAM)hFont);

    return TRUE;
  }

  case WM_CTLCOLORDLG:
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORBTN:
    SetTextColor((HDC)wParam, RGB(220, 220, 220));
    SetBkColor((HDC)wParam, RGB(30, 30, 30));
    return (INT_PTR)s_hDlgBrush;

  case WM_COMMAND:
  {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    if (id == IDC_HK_EDIT_CLEAR && code == BN_CLICKED) {
      HWND hHK = GetDlgItem(hDlg, IDC_HK_EDIT_HOTKEY);
      if (hHK) SendMessageW(hHK, HKM_SETHOTKEY, 0, 0);
      return TRUE;
    }

    if (id == IDC_HK_EDIT_BROWSE && code == BN_CLICKED) {
      wchar_t szFile[MAX_PATH] = {};
      if (pData && !pData->command.empty())
        wcsncpy_s(szFile, pData->command.c_str(), _TRUNCATE);

      OPENFILENAMEW ofn = {};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hDlg;

      if (pData && pData->userType == USER_HK_SCRIPT)
        ofn.lpstrFilter = L"Script Files (*.txt;*.bat;*.cmd;*.ps1)\0*.txt;*.bat;*.cmd;*.ps1\0All Files (*.*)\0*.*\0";
      else
        ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";

      ofn.lpstrFile = szFile;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
      ofn.lpstrTitle = (pData && pData->userType == USER_HK_SCRIPT)
          ? L"Select Script File" : L"Select Application";

      if (GetOpenFileNameW(&ofn)) {
        int editId = (pData && pData->userType == USER_HK_SCRIPT)
            ? IDC_HK_EDIT_COMMAND : IDC_HK_EDIT_PATH;
        SetDlgItemTextW(hDlg, editId, szFile);
      }
      return TRUE;
    }

    if (id == IDOK && code == BN_CLICKED) {
      if (!pData) { EndDialog(hDlg, IDCANCEL); return TRUE; }

      // Read hotkey
      HWND hHK = GetDlgItem(hDlg, IDC_HK_EDIT_HOTKEY);
      DWORD hk = hHK ? (DWORD)SendMessageW(hHK, HKM_GETHOTKEY, 0, 0) : 0;
      pData->vk = LOBYTE(LOWORD(hk));
      UINT hkMod = HIBYTE(LOWORD(hk));
      pData->modifiers = 0;
      if (hkMod & HOTKEYF_ALT)     pData->modifiers |= MOD_ALT;
      if (hkMod & HOTKEYF_CONTROL) pData->modifiers |= MOD_CONTROL;
      if (hkMod & HOTKEYF_SHIFT)   pData->modifiers |= MOD_SHIFT;

      // Read scope
      pData->scope = (IsDlgButtonChecked(hDlg, IDC_HK_EDIT_SCOPE) == BST_CHECKED)
          ? HKSCOPE_GLOBAL : HKSCOPE_LOCAL;

      // Read user-specific fields
      if (!pData->isBuiltIn) {
        wchar_t buf[512] = {};
        GetDlgItemTextW(hDlg, IDC_HK_EDIT_LABEL, buf, 512);
        pData->label = buf;
        if (pData->label.empty())
          pData->label = (pData->userType == USER_HK_SCRIPT) ? L"Script Command" : L"Launch App";

        if (pData->userType == USER_HK_SCRIPT) {
          // Read multiline text — use dynamic buffer for long scripts
          HWND hCmd = GetDlgItem(hDlg, IDC_HK_EDIT_COMMAND);
          int len = GetWindowTextLengthW(hCmd);
          std::wstring cmdText(len + 1, L'\0');
          GetWindowTextW(hCmd, &cmdText[0], len + 1);
          cmdText.resize(len);
          // Convert \r\n and \n back to pipes
          std::wstring result;
          result.reserve(cmdText.size());
          for (size_t i = 0; i < cmdText.size(); i++) {
            if (cmdText[i] == L'\r' && i + 1 < cmdText.size() && cmdText[i + 1] == L'\n') {
              result += L'|';
              i++; // skip \n
            } else if (cmdText[i] == L'\n') {
              result += L'|';
            } else {
              result += cmdText[i];
            }
          }
          // Trim trailing pipes
          while (!result.empty() && result.back() == L'|')
            result.pop_back();
          pData->command = result;
        } else {
          GetDlgItemTextW(hDlg, IDC_HK_EDIT_PATH, buf, 512);
          pData->command = buf;
        }
      }

      pData->accepted = true;
      EndDialog(hDlg, IDOK);
      return TRUE;
    }

    if (id == IDCANCEL && code == BN_CLICKED) {
      EndDialog(hDlg, IDCANCEL);
      return TRUE;
    }
    break;
  }

  case WM_CLOSE:
    EndDialog(hDlg, IDCANCEL);
    return TRUE;

  case WM_DESTROY:
    if (s_hDlgBrush) { DeleteObject(s_hDlgBrush); s_hDlgBrush = NULL; }
    return TRUE;
  }

  return FALSE;
}

//----------------------------------------------------------------------
// Build and show the edit dialog using in-memory DLGTEMPLATE
//----------------------------------------------------------------------

void HotkeysWindow::OpenEditDialog(int lvItem)
{
  HWND hList = GetDlgItem(m_hWnd, IDC_MW_HOTKEYS_LIST);
  if (!hList || lvItem < 0) return;
  LPARAM lp = GetItemLParam(hList, lvItem);
  if (lp == (LPARAM)-1) return;

  Engine* p = m_pEngine;
  EditHotkeyData data = {};
  data.pEngine = p;
  data.accepted = false;

  if (IsUserLParam(lp)) {
    int idx = UserIndex(lp);
    if (idx < 0 || idx >= (int)p->m_userHotkeys.size()) return;
    const auto& uh = p->m_userHotkeys[idx];
    data.isBuiltIn = false;
    data.index = idx;
    data.actionName = uh.label;
    data.label = uh.label;
    data.modifiers = uh.modifiers;
    data.vk = uh.vk;
    data.scope = uh.scope;
    data.userType = uh.type;
    data.command = uh.command;
  } else {
    int idx = (int)lp;
    if (idx < 0 || idx >= NUM_HOTKEYS) return;
    data.isBuiltIn = true;
    data.index = idx;
    data.actionName = p->m_hotkeys[idx].szAction;
    data.modifiers = p->m_hotkeys[idx].modifiers;
    data.vk = p->m_hotkeys[idx].vk;
    data.scope = p->m_hotkeys[idx].scope;
  }

  // Build in-memory dialog template
  // Layout: approximately 320x260 DLUs for built-in, taller for user entries
  bool isScript = !data.isBuiltIn && data.userType == USER_HK_SCRIPT;
  bool isLaunch = !data.isBuiltIn && data.userType == USER_HK_LAUNCH;

  int dlgW = 320;
  int dlgH = data.isBuiltIn ? 140 : (isScript ? 280 : 210);

  // Allocate buffer for the in-memory dialog template + controls
  // We need room for DLGTEMPLATE + items; 4KB is plenty
  BYTE buf[4096] = {};
  DLGTEMPLATE* pDlg = (DLGTEMPLATE*)buf;
  pDlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT;
  pDlg->cx = (short)dlgW;
  pDlg->cy = (short)dlgH;
  pDlg->cdit = 0;  // we create controls manually in WM_INITDIALOG

  // Font name follows the DLGTEMPLATE + menu + class strings
  WORD* pw = (WORD*)(pDlg + 1);
  *pw++ = 0; // no menu
  *pw++ = 0; // default class
  *pw++ = 0; // empty title (set in WM_INITDIALOG)
  // DS_SETFONT requires point size + font name
  *pw++ = 9; // point size
  // Font name "Segoe UI"
  wcscpy((wchar_t*)pw, L"Segoe UI");
  pw += 9; // 8 chars + null

  // Show dialog — but first, we need to create controls manually since
  // the in-memory template approach for controls is cumbersome.
  // Instead, use a minimal template and create controls in WM_INITDIALOG
  // via CreateWindowExW directly.

  // Actually, a simpler approach: use a real in-memory template for
  // just the container, then create child controls in WM_INITDIALOG.
  // This is the AudioDeviceDlgProc pattern.

  // Convert DLU to pixels for child control creation
  LONG dlgBaseUnits = GetDialogBaseUnits();
  int duX = LOWORD(dlgBaseUnits);
  int duY = HIWORD(dlgBaseUnits);

  // We'll create a bare dialog and add controls in WM_INITDIALOG via
  // a custom proc that wraps around our data.

  // Store control creation info in the data struct so the proc can use it
  // For now, let's use a simpler approach: create all controls in the proc

  // Use DialogBoxIndirectParamW with our proc and data
  // The proc will create controls via CreateWindowExW

  // First, let's use the simpler approach: just create a popup window
  // as a modal dialog with CreateWindowExW

  // Actually, the cleanest approach is a real DLGTEMPLATE with no items
  // (cdit=0) and then create all controls in WM_INITDIALOG.

  DialogBoxIndirectParamW(GetModuleHandle(NULL),
    pDlg, m_hWnd, EditHotkeyDlgProc, (LPARAM)&data);

  if (!data.accepted) return;

  // Apply changes
  if (data.isBuiltIn) {
    // Conflict detection: clear any other binding with the same key+mod
    if (data.vk != 0) {
      for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (i != data.index && p->m_hotkeys[i].vk == data.vk && p->m_hotkeys[i].modifiers == data.modifiers) {
          p->m_hotkeys[i].vk = 0;
          p->m_hotkeys[i].modifiers = 0;
        }
      }
      for (auto& uh : p->m_userHotkeys) {
        if (uh.vk == data.vk && uh.modifiers == data.modifiers) {
          uh.vk = 0;
          uh.modifiers = 0;
        }
      }
    }
    p->m_hotkeys[data.index].modifiers = data.modifiers;
    p->m_hotkeys[data.index].vk = data.vk;
    p->m_hotkeys[data.index].scope = data.scope;
  } else {
    int idx = data.index;
    if (idx >= 0 && idx < (int)p->m_userHotkeys.size()) {
      // Conflict detection
      if (data.vk != 0) {
        for (int i = 0; i < NUM_HOTKEYS; i++) {
          if (p->m_hotkeys[i].vk == data.vk && p->m_hotkeys[i].modifiers == data.modifiers) {
            p->m_hotkeys[i].vk = 0;
            p->m_hotkeys[i].modifiers = 0;
          }
        }
        for (int i = 0; i < (int)p->m_userHotkeys.size(); i++) {
          if (i != idx && p->m_userHotkeys[i].vk == data.vk && p->m_userHotkeys[i].modifiers == data.modifiers) {
            p->m_userHotkeys[i].vk = 0;
            p->m_userHotkeys[i].modifiers = 0;
          }
        }
      }
      auto& uh = p->m_userHotkeys[idx];
      uh.modifiers = data.modifiers;
      uh.vk = data.vk;
      uh.scope = data.scope;
      uh.label = data.label;
      uh.command = data.command;
    }
  }

  SaveAndReRegister(p);
  RefreshHotkeyList(hList, p);
  // Re-select the edited item
  for (int i = 0; i < ListView_GetItemCount(hList); i++) {
    if (GetItemLParam(hList, i) == lp) {
      ListView_SetItemState(hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      ListView_EnsureVisible(hList, i, FALSE);
      break;
    }
  }
}

//----------------------------------------------------------------------
// Update Delete button enable state
//----------------------------------------------------------------------

void HotkeysWindow::UpdateDeleteButton()
{
  if (!m_hList) return;
  LPARAM lp = GetSelectedLParam(m_hList);
  if (m_hBtnDelete)
    EnableWindow(m_hBtnDelete, (lp != (LPARAM)-1 && IsUserLParam(lp)) ? TRUE : FALSE);

  // Clear Key enabled when any entry is selected and has a key bound
  if (m_hBtnClearKey) {
    bool hasKey = false;
    if (lp != (LPARAM)-1) {
      if (IsUserLParam(lp)) {
        int idx = UserIndex(lp);
        if (idx >= 0 && idx < (int)m_pEngine->m_userHotkeys.size())
          hasKey = m_pEngine->m_userHotkeys[idx].vk != 0;
      } else {
        int idx = (int)lp;
        if (idx >= 0 && idx < NUM_HOTKEYS)
          hasKey = m_pEngine->m_hotkeys[idx].vk != 0;
      }
    }
    EnableWindow(m_hBtnClearKey, hasKey ? TRUE : FALSE);
  }
}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void HotkeysWindow::DoBuildControls()
{
  HWND hw = m_hWnd;
  if (!hw) return;

  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = GetFont();
  HFONT hFontBold = GetFontBold();

  // Title label
  TrackControl(CreateLabel(hw, L"Keyboard Shortcuts", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  m_headerH = y;  // save for LayoutControls

  // Button bar height (at bottom)
  m_buttonBarH = lineH + gap + 8;

  // ListView fills remaining vertical space
  RECT rc;
  GetClientRect(hw, &rc);
  int listH = (rc.bottom - y - m_buttonBarH - gap);
  if (listH < lineH * 5) listH = lineH * 5;

  m_hList = CreateThemedListView(IDC_MW_HOTKEYS_LIST, x, y, rw, listH,
                                  /*visible=*/true, /*sortable=*/true);
  TrackControl(m_hList);
  if (m_hList) {
    int scrollW = GetSystemMetrics(SM_CXVSCROLL) + 4;
    int colCategory = MulDiv(rw, 18, 100);
    int colAction   = MulDiv(rw, 35, 100);
    int colScope    = MulDiv(rw, 15, 100);
    int colShortcut = rw - colCategory - colAction - colScope - scrollW;

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)L"Category";
    col.cx = colCategory;
    SendMessageW(m_hList, LVM_INSERTCOLUMNW, COL_CATEGORY, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Action";
    col.cx = colAction;
    SendMessageW(m_hList, LVM_INSERTCOLUMNW, COL_ACTION, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Shortcut";
    col.cx = colShortcut;
    SendMessageW(m_hList, LVM_INSERTCOLUMNW, COL_SHORTCUT, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Scope";
    col.cx = colScope;
    SendMessageW(m_hList, LVM_INSERTCOLUMNW, COL_SCOPE, (LPARAM)&col);

    RefreshHotkeyList(m_hList, m_pEngine);
    ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
  y += listH + gap;

  // Button row: [+] [Edit] [Delete] on left, [Reset to Defaults] on right
  int btnW = MulDiv(60, lineH, 26);
  int addW = MulDiv(30, lineH, 26);
  int btnGap = 8;

  m_hBtnAdd = CreateBtn(hw, L"+", IDC_MW_HOTKEYS_ADD, x, y, addW, lineH, hFont);
  TrackControl(m_hBtnAdd);
  int bx = x + addW + btnGap;

  m_hBtnEdit = CreateBtn(hw, L"Edit", IDC_MW_HOTKEYS_EDITBTN, bx, y, btnW, lineH, hFont);
  TrackControl(m_hBtnEdit);
  bx += btnW + btnGap;

  m_hBtnDelete = CreateBtn(hw, L"Delete", IDC_MW_HOTKEYS_DELETE, bx, y, btnW, lineH, hFont);
  TrackControl(m_hBtnDelete);
  bx += btnW + btnGap;

  int clearKeyW = MulDiv(80, lineH, 26);
  m_hBtnClearKey = CreateBtn(hw, L"Clear Key", IDC_MW_HOTKEYS_CLEARKEY, bx, y, clearKeyW, lineH, hFont);
  TrackControl(m_hBtnClearKey);

  int resetW = MulDiv(160, lineH, 26);
  m_hBtnReset = CreateBtn(hw, L"Reset to Defaults", IDC_MW_HOTKEYS_RESET,
    x + rw - resetW, y, resetW, lineH, hFont);
  TrackControl(m_hBtnReset);

  // Set initial delete button state
  UpdateDeleteButton();
}

//----------------------------------------------------------------------
// OnResize — reposition controls without full rebuild
//----------------------------------------------------------------------

void HotkeysWindow::OnResize()
{
  LayoutControls();
}

void HotkeysWindow::LayoutControls()
{
  if (!m_hWnd || !m_hList) return;

  RECT rc;
  GetClientRect(m_hWnd, &rc);

  int lineH = GetLineHeight();
  int gap = MulDiv(6, lineH, 26);
  int margin = 12;
  int x = margin;
  int rw = rc.right - 2 * margin;

  // Recalculate list and button positions
  int listY = m_headerH;
  int buttonY = rc.bottom - m_buttonBarH;
  int listH = buttonY - listY - gap;
  if (listH < lineH * 5) listH = lineH * 5;

  MoveWindow(m_hList, x, listY, rw, listH, TRUE);

  // Reposition button row
  int btnW = MulDiv(60, lineH, 26);
  int addW = MulDiv(30, lineH, 26);
  int btnGap = 8;

  if (m_hBtnAdd)    MoveWindow(m_hBtnAdd, x, buttonY, addW, lineH, TRUE);
  int bx = x + addW + btnGap;
  if (m_hBtnEdit)   MoveWindow(m_hBtnEdit, bx, buttonY, btnW, lineH, TRUE);
  bx += btnW + btnGap;
  if (m_hBtnDelete) MoveWindow(m_hBtnDelete, bx, buttonY, btnW, lineH, TRUE);
  bx += btnW + btnGap;
  int clearKeyW = MulDiv(80, lineH, 26);
  if (m_hBtnClearKey) MoveWindow(m_hBtnClearKey, bx, buttonY, clearKeyW, lineH, TRUE);

  int resetW = MulDiv(160, lineH, 26);
  if (m_hBtnReset) MoveWindow(m_hBtnReset, x + rw - resetW, buttonY, resetW, lineH, TRUE);
}

//----------------------------------------------------------------------
// DoCommand — button clicks
//----------------------------------------------------------------------

LRESULT HotkeysWindow::DoCommand(HWND hWnd, int id, int code, LPARAM /*lParam*/)
{
  Engine* p = m_pEngine;

  if (id == IDC_MW_HOTKEYS_ADD && code == BN_CLICKED) {
    // Show popup menu: Script Command / Launch App
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, L"Script Command");
    AppendMenuW(hMenu, MF_STRING, 2, L"Launch App");
    RECT rc;
    GetWindowRect(m_hBtnAdd, &rc);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                              rc.left, rc.bottom, 0, hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == 1 || cmd == 2) {
      UserHotkeyType type = (cmd == 1) ? USER_HK_SCRIPT : USER_HK_LAUNCH;
      int idx = p->AddUserHotkey(type);
      SaveAndReRegister(p);
      RefreshHotkeyList(m_hList, p);

      // Select the new entry and open edit dialog
      LPARAM newLp = USER_LPARAM_BIT | idx;
      for (int i = 0; i < ListView_GetItemCount(m_hList); i++) {
        if (GetItemLParam(m_hList, i) == newLp) {
          ListView_SetItemState(m_hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
          ListView_EnsureVisible(m_hList, i, FALSE);
          OpenEditDialog(i);
          break;
        }
      }
      UpdateDeleteButton();
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_EDITBTN && code == BN_CLICKED) {
    int sel = ListView_GetNextItem(m_hList, -1, LVNI_SELECTED);
    if (sel >= 0) OpenEditDialog(sel);
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_DELETE && code == BN_CLICKED) {
    LPARAM lp = GetSelectedLParam(m_hList);
    if (lp != (LPARAM)-1 && IsUserLParam(lp)) {
      int idx = UserIndex(lp);
      p->RemoveUserHotkey(idx);
      SaveAndReRegister(p);
      RefreshHotkeyList(m_hList, p);
      // Select first item if any
      if (ListView_GetItemCount(m_hList) > 0)
        ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      UpdateDeleteButton();
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_CLEARKEY && code == BN_CLICKED) {
    LPARAM lp = GetSelectedLParam(m_hList);
    if (lp == (LPARAM)-1) return 0;

    if (IsUserLParam(lp)) {
      int idx = UserIndex(lp);
      if (idx >= 0 && idx < (int)p->m_userHotkeys.size()) {
        p->m_userHotkeys[idx].vk = 0;
        p->m_userHotkeys[idx].modifiers = 0;
      }
    } else {
      int idx = (int)lp;
      if (idx >= 0 && idx < NUM_HOTKEYS) {
        p->m_hotkeys[idx].vk = 0;
        p->m_hotkeys[idx].modifiers = 0;
      }
    }
    SaveAndReRegister(p);
    RefreshHotkeyList(m_hList, p);
    // Re-select the item
    for (int i = 0; i < ListView_GetItemCount(m_hList); i++) {
      if (GetItemLParam(m_hList, i) == lp) {
        ListView_SetItemState(m_hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(m_hList, i, FALSE);
        break;
      }
    }
    return 0;
  }

  if (id == IDC_MW_HOTKEYS_RESET && code == BN_CLICKED) {
    p->ResetHotkeyDefaults();
    SaveAndReRegister(p);
    RefreshHotkeyList(m_hList, p);
    ListView_SetItemState(m_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    p->AddNotification(L"Built-in hotkeys reset to defaults (user entries kept)");
    return 0;
  }

  return -1;
}

//----------------------------------------------------------------------
// DoNotify — ListView selection change, column click sorting, double-click
//----------------------------------------------------------------------

LRESULT HotkeysWindow::DoNotify(HWND hWnd, NMHDR* pnm)
{
  if (pnm->idFrom != IDC_MW_HOTKEYS_LIST) return -1;

  if (pnm->code == LVN_ITEMCHANGED) {
    NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
    if (pnmlv->uNewState & LVIS_SELECTED)
      UpdateDeleteButton();
    return 0;
  }

  if (pnm->code == NM_DBLCLK) {
    NMITEMACTIVATE* pnma = (NMITEMACTIVATE*)pnm;
    if (pnma->iItem >= 0)
      OpenEditDialog(pnma->iItem);
    return 0;
  }

  if (pnm->code == LVN_COLUMNCLICK) {
    NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
    if (pnmlv->iSubItem == s_sortColumn)
      s_sortAscending = !s_sortAscending;
    else {
      s_sortColumn = pnmlv->iSubItem;
      s_sortAscending = true;
    }
    SortContext ctx = { m_hList, m_pEngine };
    ListView_SortItems(m_hList, HotkeyListCompare, (LPARAM)&ctx);
    return 0;
  }

  return -1;
}

} // namespace mdrop
