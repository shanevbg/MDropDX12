/*
  Plugin module: Settings Window, Resource Viewer & Configuration
  Extracted from engine.cpp for maintainability.
  Contains: Settings window management, controls, theme, resource viewer,
            settings config, user defaults, fallback paths
*/

#include "engine.h"
#include "tool_window.h"
#include "video_capture.h"
#include "engine_helpers.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "AutoCharFn.h"
#include "shell_defines.h"
#include "wasabi.h"
#include <assert.h>
#include <strsafe.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <Windows.h>
#include <cstdint>
#include <sstream>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#include <set>
#include "App.h"

namespace mdrop {

extern Engine g_engine;
extern int ToggleFPSNumPressed;

// SettingsWindow constructor (ToolWindow subclass)
SettingsWindow::SettingsWindow(Engine* pEngine) : ToolWindow(pEngine, 620, 850) {}

DWORD SettingsWindow::GetCommonControlFlags() const {
  return ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_HOTKEY_CLASS;
}

void SettingsWindow::OnAlreadyOpen() { EnsureVisible(); }
void SettingsWindow::OnResize() { LayoutControls(); }

// Shorten a directory path to "...\parent\leaf\" for compact display.
// If the path has fewer than 3 components, it's returned as-is.
static void ShortenDirectoryPath(const wchar_t* szFullPath, wchar_t* szOut, int nMaxChars) {
  if (!szFullPath || !szFullPath[0]) {
    szOut[0] = 0;
    return;
  }
  // Find the last two backslash-delimited segments
  int len = (int)wcslen(szFullPath);
  // Strip trailing backslash for scanning
  int end = len;
  if (end > 0 && szFullPath[end - 1] == L'\\') end--;
  // Find second-to-last backslash
  int slash1 = -1, slash2 = -1;
  for (int i = end - 1; i >= 0; i--) {
    if (szFullPath[i] == L'\\') {
      if (slash1 < 0) slash1 = i;
      else { slash2 = i; break; }
    }
  }
  // If path is short enough or has <3 segments, show as-is
  if (slash2 < 0 || len < 35) {
    lstrcpynW(szOut, szFullPath, nMaxChars);
    return;
  }
  // Format as "...\parent\leaf\"
  swprintf(szOut, nMaxChars, L"...%s", szFullPath + slash2);
}

// EnumWindows callback: collects visible window titles for the Window Title combo
static BOOL CALLBACK EnumVisibleWindowTitlesProc(HWND hwnd, LPARAM lParam) {
  if (!IsWindowVisible(hwnd)) return TRUE;
  wchar_t title[256] = {};
  int len = GetWindowTextW(hwnd, title, _countof(title));
  if (len < 3) return TRUE;
  if (wcsstr(title, L"MDropDX12") != nullptr) return TRUE;
  auto* titles = reinterpret_cast<std::vector<std::wstring>*>(lParam);
  titles->push_back(title);
  return TRUE;
}

// Fills the Window Title CBS_DROPDOWN combo with all visible window titles
static void PopulateWindowTitleCombo(HWND hCombo, const wchar_t* currentTitle) {
  wchar_t editText[256] = {};
  if (currentTitle && currentTitle[0])
    lstrcpynW(editText, currentTitle, _countof(editText));
  else
    GetWindowTextW(hCombo, editText, _countof(editText));
  SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
  std::vector<std::wstring> titles;
  EnumWindows(EnumVisibleWindowTitlesProc, (LPARAM)&titles);
  std::sort(titles.begin(), titles.end(), [](const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  });
  for (const auto& t : titles)
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)t.c_str());
  SetWindowTextW(hCombo, editText);
}

// EnumWindows callback for regex-based window matching in settings UI
struct SettingsWTMatchContext {
  const std::wregex* pRegex;
  std::wstring matchedTitle;
};
static BOOL CALLBACK EnumWindowsSettingsMatch(HWND hWnd, LPARAM lParam) {
  auto* ctx = reinterpret_cast<SettingsWTMatchContext*>(lParam);
  if (!IsWindowVisible(hWnd)) return TRUE;
  wchar_t buf[512] = {};
  int len = GetWindowTextW(hWnd, buf, _countof(buf));
  if (len <= 0) return TRUE;
  if (wcsstr(buf, L"MDropDX12") != nullptr) return TRUE;
  try {
    if (std::regex_search(buf, *ctx->pRegex)) {
      ctx->matchedTitle = buf;
      return FALSE;
    }
  } catch (...) {}
  return TRUE;
}

// Updates the preview label: matches window via regex, then parses with active profile's parse regex
static void UpdateWindowTitlePreview(HWND hSettingsWnd, const wchar_t* windowRegex) {
  HWND hPreview = GetDlgItem(hSettingsWnd, IDC_MW_SONG_WT_PREVIEW);
  if (!hPreview) return;
  if (!windowRegex || !windowRegex[0]) { SetWindowTextW(hPreview, L""); return; }

  // Compile window match regex
  std::wregex compiledRegex;
  try {
    compiledRegex = std::wregex(windowRegex, std::regex_constants::ECMAScript | std::regex_constants::icase);
  } catch (...) {
    SetWindowTextW(hPreview, L"(invalid regex)");
    return;
  }

  // Find matching window
  SettingsWTMatchContext ctx = { &compiledRegex, {} };
  EnumWindows(EnumWindowsSettingsMatch, reinterpret_cast<LPARAM>(&ctx));
  if (ctx.matchedTitle.empty()) { SetWindowTextW(hPreview, L"(no match)"); return; }

  // Try to parse with active profile's parse regex
  std::wstring preview;
  int idx = g_engine.m_nActiveWindowTitleProfile;
  if (idx >= 0 && idx < (int)g_engine.m_windowTitleProfiles.size() && g_engine.m_windowTitleProfiles[idx].szParseRegex[0]) {
    try {
      std::wregex parseRegex(StripNamedGroups(g_engine.m_windowTitleProfiles[idx].szParseRegex), std::regex_constants::ECMAScript);
      std::wsmatch match;
      if (std::regex_search(ctx.matchedTitle, match, parseRegex)) {
        // Build name→index map for named groups
        std::wstring parseStr(g_engine.m_windowTitleProfiles[idx].szParseRegex);
        std::map<std::wstring, int> nameMap;
        int groupIdx = 0;
        for (size_t i = 0; i < parseStr.size(); i++) {
          if (parseStr[i] == L'\\') { i++; continue; }
          if (parseStr[i] == L'(') {
            if (i + 1 < parseStr.size() && parseStr[i + 1] == L'?') {
              if (i + 2 < parseStr.size() && (parseStr[i + 2] == L'<' || parseStr[i + 2] == L'\'')) {
                wchar_t delim = (parseStr[i + 2] == L'<') ? L'>' : L'\'';
                size_t nameStart = i + 3;
                size_t nameEnd = parseStr.find(delim, nameStart);
                if (nameEnd != std::wstring::npos) {
                  groupIdx++;
                  nameMap[parseStr.substr(nameStart, nameEnd - nameStart)] = groupIdx;
                }
              }
            } else { groupIdx++; }
          }
        }
        auto getGroup = [&](const wchar_t* name, int fallback) -> std::wstring {
          auto it = nameMap.find(name);
          int gi = (it != nameMap.end()) ? it->second : fallback;
          return (gi > 0 && gi < (int)match.size() && match[gi].matched) ? match[gi].str() : L"";
        };
        std::wstring artist = getGroup(L"artist", 1);
        std::wstring title = getGroup(L"title", 2);
        std::wstring album = getGroup(L"album", 3);
        if (!artist.empty()) preview += L"Artist: " + artist;
        if (!title.empty()) { if (!preview.empty()) preview += L"  |  "; preview += L"Title: " + title; }
        if (!album.empty()) { if (!preview.empty()) preview += L"  |  "; preview += L"Album: " + album; }
      } else {
        preview = L"(regex didn't match: " + ctx.matchedTitle + L")";
      }
    } catch (...) {
      preview = L"(invalid parse regex)";
    }
  } else {
    // No parse regex — show raw matched title
    preview = L"Matched: " + ctx.matchedTitle;
  }
  SetWindowTextW(hPreview, preview.c_str());
}

//----------------------------------------------------------------------
// Sprite Import Settings Dialog (ModalDialog subclass)
//----------------------------------------------------------------------
class SpriteImportDialog : public mdrop::ModalDialog {
public:
  SpriteImportDialog(Engine* pEngine) : ModalDialog(pEngine) {}
  ~SpriteImportDialog() override { if (m_hFontBold) DeleteObject(m_hFontBold); }

  // Working copies (populated before Show, read back after)
  bool   bReplace = false;     // true = replace all, false = add to existing
  int    nMaxSprites = 100;    // max to import
  int    nBlendMode = 0;
  int    nLayer = 0;           // 0 = behind text, 1 = on top
  double x = 0.5, y = 0.5, sx = 0.5, sy = 0.5, rot = 0;
  double r = 1, g = 1, b = 1, a = 1;

protected:
  const wchar_t* GetDialogTitle() const override { return L"Import Sprites from Folder"; }
  const wchar_t* GetDialogClass() const override { return L"MDropDX12SprImp"; }

  void DoBuildControls(int clientW, int clientH) override {
    HFONT hFont = GetFont();
    int lineH = GetLineHeight();

    // Create bold font for section headers
    m_hFontBold = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    int margin = MulDiv(12, lineH, 20);
    int rw = clientW - margin * 2;
    int cy = MulDiv(10, lineH, 20);
    int gap = 4;
    int lblW = MulDiv(80, lineH, 20);
    int editW = MulDiv(60, lineH, 20);
    wchar_t buf[64];

    // --- Import Mode ---
    TrackControl(CreateLabel(m_hWnd, L"Import Mode:", margin, cy, rw, lineH, m_hFontBold));
    cy += lineH + gap;
    TrackControl(CreateRadio(m_hWnd, L"Add to existing sprites", IDC_SPRIMP_MODE_ADD, margin + 10, cy, rw - 10, lineH, hFont, !bReplace, true, true, 1));
    cy += lineH + 2;
    TrackControl(CreateRadio(m_hWnd, L"Replace all sprites", IDC_SPRIMP_MODE_REPLACE, margin + 10, cy, rw - 10, lineH, hFont, bReplace, false, true, 1));
    cy += lineH + gap + 4;

    // --- Max sprites ---
    TrackControl(CreateLabel(m_hWnd, L"Max sprites to import:", margin, cy, MulDiv(150, lineH, 20), lineH, hFont));
    swprintf(buf, 64, L"%d", nMaxSprites);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_MAX_EDIT, margin + MulDiv(155, lineH, 20), cy, editW, lineH, hFont, ES_NUMBER));
    cy += lineH + gap + 6;

    // --- Default Properties ---
    TrackControl(CreateLabel(m_hWnd, L"Default Properties for Imported Sprites:", margin, cy, rw, lineH, m_hFontBold));
    cy += lineH + gap + 2;

    // Blend mode
    int col1 = margin;
    TrackControl(CreateLabel(m_hWnd, L"Blend:", col1, cy, lblW, lineH, hFont));
    HWND hBlend = CreateWindowExW(0, L"COMBOBOX", L"",
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
      col1 + lblW, cy, MulDiv(120, lineH, 20), 200, m_hWnd, (HMENU)(INT_PTR)IDC_SPRIMP_BLEND,
      GetModuleHandle(NULL), NULL);
    if (hBlend && hFont) SendMessage(hBlend, WM_SETFONT, (WPARAM)hFont, TRUE);
    const wchar_t* blendNames[] = { L"0: Blend", L"1: Decal", L"2: Additive", L"3: SrcColor", L"4: ColorKey" };
    for (int i = 0; i < 5; i++) SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)blendNames[i]);
    SendMessageW(hBlend, CB_SETCURSEL, nBlendMode, 0);
    TrackControl(hBlend);
    cy += lineH + gap;

    // Layer
    TrackControl(CreateLabel(m_hWnd, L"Layer:", col1, cy, lblW, lineH, hFont));
    HWND hLayer = CreateWindowExW(0, L"COMBOBOX", L"",
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
      col1 + lblW, cy, MulDiv(140, lineH, 20), 200, m_hWnd, (HMENU)(INT_PTR)IDC_SPRIMP_LAYER,
      GetModuleHandle(NULL), NULL);
    if (hLayer && hFont) SendMessage(hLayer, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hLayer, CB_ADDSTRING, 0, (LPARAM)L"0: Behind Text");
    SendMessageW(hLayer, CB_ADDSTRING, 0, (LPARAM)L"1: On Top of Text");
    SendMessageW(hLayer, CB_SETCURSEL, nLayer, 0);
    TrackControl(hLayer);
    cy += lineH + gap;

    // Position: X, Y
    int col2 = col1 + lblW + editW + MulDiv(20, lineH, 20);
    TrackControl(CreateLabel(m_hWnd, L"X:", col1, cy, MulDiv(24, lineH, 20), lineH, hFont));
    swprintf(buf, 64, L"%.3g", x);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_X, col1 + MulDiv(24, lineH, 20), cy, editW, lineH, hFont));
    TrackControl(CreateLabel(m_hWnd, L"Y:", col2, cy, MulDiv(24, lineH, 20), lineH, hFont));
    swprintf(buf, 64, L"%.3g", y);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_Y, col2 + MulDiv(24, lineH, 20), cy, editW, lineH, hFont));
    cy += lineH + gap;

    // Scale: SX, SY
    TrackControl(CreateLabel(m_hWnd, L"SX:", col1, cy, MulDiv(24, lineH, 20), lineH, hFont));
    swprintf(buf, 64, L"%.3g", sx);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_SX, col1 + MulDiv(24, lineH, 20), cy, editW, lineH, hFont));
    TrackControl(CreateLabel(m_hWnd, L"SY:", col2, cy, MulDiv(24, lineH, 20), lineH, hFont));
    swprintf(buf, 64, L"%.3g", sy);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_SY, col2 + MulDiv(24, lineH, 20), cy, editW, lineH, hFont));
    cy += lineH + gap;

    // Rotation
    TrackControl(CreateLabel(m_hWnd, L"Rot:", col1, cy, MulDiv(30, lineH, 20), lineH, hFont));
    swprintf(buf, 64, L"%.3g", rot);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_ROT, col1 + MulDiv(30, lineH, 20), cy, editW, lineH, hFont));
    cy += lineH + gap;

    // Color: R, G, B, A
    int col3 = col1 + (MulDiv(24, lineH, 20) + editW + MulDiv(10, lineH, 20));
    int col4 = col3 + (MulDiv(24, lineH, 20) + editW + MulDiv(10, lineH, 20));
    int col5 = col4 + (MulDiv(24, lineH, 20) + editW + MulDiv(10, lineH, 20));
    int smallLblW = MulDiv(18, lineH, 20);
    TrackControl(CreateLabel(m_hWnd, L"R:", col1, cy, smallLblW, lineH, hFont));
    swprintf(buf, 64, L"%.3g", r);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_R, col1 + smallLblW, cy, editW, lineH, hFont));
    TrackControl(CreateLabel(m_hWnd, L"G:", col3, cy, smallLblW, lineH, hFont));
    swprintf(buf, 64, L"%.3g", g);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_G, col3 + smallLblW, cy, editW, lineH, hFont));
    TrackControl(CreateLabel(m_hWnd, L"B:", col4, cy, smallLblW, lineH, hFont));
    swprintf(buf, 64, L"%.3g", b);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_B, col4 + smallLblW, cy, editW, lineH, hFont));
    TrackControl(CreateLabel(m_hWnd, L"A:", col5, cy, smallLblW, lineH, hFont));
    swprintf(buf, 64, L"%.3g", a);
    TrackControl(CreateEdit(m_hWnd, buf, IDC_SPRIMP_A, col5 + smallLblW, cy, editW, lineH, hFont));
    cy += lineH + gap + 8;

    // --- OK / Cancel ---
    int btnW = MulDiv(80, lineH, 20);
    int btnH = lineH + 4;
    int btnGap = MulDiv(10, lineH, 20);
    int btnX = clientW / 2 - btnW - btnGap / 2;
    TrackControl(CreateBtn(m_hWnd, L"OK", IDC_SPRIMP_OK, btnX, cy, btnW, btnH, hFont));
    TrackControl(CreateBtn(m_hWnd, L"Cancel", IDC_SPRIMP_CANCEL, btnX + btnW + btnGap, cy, btnW, btnH, hFont));
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    if (code == BN_CLICKED) {
      if (id == IDC_SPRIMP_OK) {
        ReadBackFields();
        EndDialog(true);
        return 0;
      }
      if (id == IDC_SPRIMP_CANCEL) {
        EndDialog(false);
        return 0;
      }
    }
    return -1;
  }

  LRESULT DoMessage(UINT msg, WPARAM wParam, LPARAM lParam) override {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
      SendMessage(m_hWnd, WM_COMMAND, MAKEWPARAM(IDC_SPRIMP_OK, BN_CLICKED), 0);
      return 0;
    }
    return -1;
  }

private:
  HFONT m_hFontBold = NULL;

  void ReadBackFields() {
    wchar_t buf[64];
    // Read max sprites
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_MAX_EDIT, buf, 64);
    nMaxSprites = _wtoi(buf);
    if (nMaxSprites < 1) nMaxSprites = 1;
    // Read blend mode
    int sel = (int)SendDlgItemMessageW(m_hWnd, IDC_SPRIMP_BLEND, CB_GETCURSEL, 0, 0);
    nBlendMode = (sel == CB_ERR) ? 0 : sel;
    // Read layer
    int lsel = (int)SendDlgItemMessageW(m_hWnd, IDC_SPRIMP_LAYER, CB_GETCURSEL, 0, 0);
    nLayer = (lsel == CB_ERR) ? 0 : lsel;
    // Read property values
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_X, buf, 64);   x   = _wtof(buf);
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_Y, buf, 64);   y   = _wtof(buf);
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_SX, buf, 64);  sx  = _wtof(buf);
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_SY, buf, 64);  sy  = _wtof(buf);
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_ROT, buf, 64); rot = _wtof(buf);
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_R, buf, 64);   r   = _wtof(buf);
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_G, buf, 64);   g   = _wtof(buf);
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_B, buf, 64);   b   = _wtof(buf);
    GetDlgItemTextW(m_hWnd, IDC_SPRIMP_A, buf, 64);   a   = _wtof(buf);
    // Read radio state
    bReplace = IsChecked(IDC_SPRIMP_MODE_REPLACE);
  }
};

static bool ShowSpriteImportDialog(HWND hParent, Engine* plugin, SpriteImportDialog& dlg) {
  // Compute line height from font size for proportional layout
  // (font not yet created — use a temporary font to measure)
  HFONT hTmp = CreateFontW(plugin->m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HDC hdcTmp = GetDC(hParent);
  HFONT hOld = (HFONT)SelectObject(hdcTmp, hTmp);
  TEXTMETRIC tm = {};
  GetTextMetrics(hdcTmp, &tm);
  SelectObject(hdcTmp, hOld);
  ReleaseDC(hParent, hdcTmp);
  DeleteObject(hTmp);
  int lineH = tm.tmHeight + tm.tmExternalLeading + 6;
  if (lineH < 20) lineH = 20;

  int clientW = MulDiv(420, lineH, 20);
  int clientH = MulDiv(380, lineH, 20);

  return dlg.Show(hParent, clientW, clientH);
}

void SettingsWindow::DoDestroy() {
  KillTimer(m_hWnd, IDT_IPC_MONITOR);
  m_pEngine->CleanupSettingsThemeBrushes();
}

void SettingsWindow::RebuildFonts() {
  if (!m_hWnd) return;
  ToolWindow::RebuildFonts();
}

// ─── Tools ListView sort ────────────────────────────────────────────────────
static int  s_toolSortCol = 0;
static bool s_toolSortAsc = true;

static int CALLBACK ToolListCompare(LPARAM lp1, LPARAM lp2, LPARAM lpSort)
{
  HWND hList = (HWND)lpSort;
  wchar_t a[128] = {}, b[128] = {};
  LVITEMW lvi = {};
  lvi.mask = LVIF_TEXT;
  lvi.iSubItem = s_toolSortCol;
  lvi.pszText = a; lvi.cchTextMax = 128;
  // Find items by lParam
  LVFINDINFOW fi = {};
  fi.flags = LVFI_PARAM;
  fi.lParam = lp1;
  int i1 = (int)SendMessageW(hList, LVM_FINDITEMW, -1, (LPARAM)&fi);
  fi.lParam = lp2;
  int i2 = (int)SendMessageW(hList, LVM_FINDITEMW, -1, (LPARAM)&fi);
  if (i1 >= 0) { lvi.iItem = i1; SendMessageW(hList, LVM_GETITEMTEXTW, i1, (LPARAM)&lvi); }
  lvi.pszText = b; lvi.cchTextMax = 128;
  if (i2 >= 0) { lvi.iItem = i2; SendMessageW(hList, LVM_GETITEMTEXTW, i2, (LPARAM)&lvi); }
  int cmp = _wcsicmp(a, b);
  return s_toolSortAsc ? cmp : -cmp;
}

LRESULT SettingsWindow::DoNotify(HWND hWnd, NMHDR* pnm) {
  // Idle timer timeout spin control
  if (pnm->idFrom == IDC_MW_IDLE_TIMEOUT_SPIN && pnm->code == UDN_DELTAPOS) {
    NMUPDOWN* pud = (NMUPDOWN*)pnm;
    int newVal = pud->iPos + pud->iDelta;
    if (newVal < 1) newVal = 1;
    if (newVal > 60) newVal = 60;
    m_pEngine->m_nIdleTimeoutMinutes = newVal;
    m_pEngine->SaveIdleTimerSettings();
  }

  // Tools ListView: double-click to open
  if (pnm->idFrom == IDC_MW_TOOLS_LIST && pnm->code == NM_DBLCLK) {
    HWND hList = pnm->hwndFrom;
    int sel = (int)SendMessageW(hList, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
    if (sel >= 0) {
      LVITEMW lvi = {};
      lvi.mask = LVIF_PARAM;
      lvi.iItem = sel;
      SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&lvi);
      int hkIdx = (int)lvi.lParam;
      if (hkIdx >= 0 && hkIdx < NUM_HOTKEYS)
        m_pEngine->DispatchHotkeyAction(m_pEngine->m_hotkeys[hkIdx].id);
    }
    return 0;
  }

  // Tools ListView: column click to sort
  if (pnm->idFrom == IDC_MW_TOOLS_LIST && pnm->code == LVN_COLUMNCLICK) {
    NMLISTVIEW* pnmlv = (NMLISTVIEW*)pnm;
    if (pnmlv->iSubItem == s_toolSortCol)
      s_toolSortAsc = !s_toolSortAsc;
    else {
      s_toolSortCol = pnmlv->iSubItem;
      s_toolSortAsc = true;
    }
    ListView_SortItems(pnm->hwndFrom, ToolListCompare, (LPARAM)pnm->hwndFrom);
    return 0;
  }

  return 0;
}

LRESULT SettingsWindow::DoHScroll(HWND hWnd, int id, int pos) {
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
  case IDC_MW_COL_HUE: {
    m_pEngine->m_ColShiftHue = (pos - 100) / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%.2f", m_pEngine->m_ColShiftHue);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_HUE_LABEL), buf);
    break;
  }
  case IDC_MW_COL_SAT: {
    m_pEngine->m_ColShiftSaturation = (pos - 100) / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%.2f", m_pEngine->m_ColShiftSaturation);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_SAT_LABEL), buf);
    break;
  }
  case IDC_MW_COL_BRIGHT: {
    m_pEngine->m_ColShiftBrightness = (pos - 100) / 100.0f;
    wchar_t buf[32]; swprintf(buf, 32, L"%.2f", m_pEngine->m_ColShiftBrightness);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT_LABEL), buf);
    break;
  }
  case IDC_MW_COL_GAMMA: {
    float gamma = pos / 10.0f;
    m_pEngine->m_pState->m_fGammaAdj = gamma;
    wchar_t buf[32]; swprintf(buf, 32, L"%.1f", gamma);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_GAMMA_LABEL), buf);
    break;
  }
  default:
    return -1;
  }
  return 0;
}

LRESULT SettingsWindow::DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
  case WM_MW_PRESET_CHANGED: {
    // Render thread changed the active preset — sync the Settings listbox
    HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
    if (hList && m_pEngine->m_nCurrentPreset >= 0 && m_pEngine->m_nCurrentPreset < m_pEngine->m_nPresets)
      SendMessage(hList, LB_SETCURSEL, m_pEngine->m_nCurrentPreset, 0);
    return 0;
  }
  case WM_TIMER:
    if (wParam == 9999) {
      KillTimer(hWnd, 9999);
      SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      return 0;
    }
    if (wParam == IDT_IPC_MONITOR) {
      int seq = g_lastIPCMessageSeq.load();
      if (seq != m_lastSeenIPCSeq) {
        m_lastSeenIPCSeq = seq;
        // Only update display if an IPC window is selected in list
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
    break;
  case WM_DROPFILES:
    LoadPresetFilesViaDragAndDrop(wParam);
    // Refresh settings UI after potential directory change
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_PRESET_DIR), m_pEngine->m_szPresetDir);
    {
      HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
      if (hList) {
        SendMessage(hList, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < m_pEngine->m_nPresets; i++) {
          if (m_pEngine->m_presets[i].szFilename.empty()) continue;
          SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)m_pEngine->m_presets[i].szFilename.c_str());
        }
        if (m_pEngine->m_nCurrentPreset >= 0 && m_pEngine->m_nCurrentPreset < m_pEngine->m_nPresets)
          SendMessage(hList, LB_SETCURSEL, m_pEngine->m_nCurrentPreset, 0);
      }
    }
    return 0;
  }
  return -1;
}

LRESULT SettingsWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;

    // Browse Preset file
    if (id == IDC_MW_BROWSE_PRESET && code == BN_CLICKED) {
      wchar_t szFile[MAX_PATH] = {};
      OPENFILENAMEW ofn = {};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hWnd;
      ofn.lpstrFilter = L"Preset Files (*.milk;*.milk2;*.milk3)\0*.milk;*.milk2;*.milk3\0All Files (*.*)\0*.*\0";
      ofn.lpstrFile = szFile;
      ofn.nMaxFile = MAX_PATH;
      ofn.lpstrInitialDir = p->m_szPresetDir;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
      if (GetOpenFileNameW(&ofn)) {
        p->LoadPreset(szFile, p->m_fBlendTimeUser);
        // Update current preset display
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_CURRENT_PRESET), p->m_szCurrentPresetFile);
      }
      return 0;
    }

    if (id == IDC_MW_RESOURCES && code == BN_CLICKED) {
      p->OpenResourceViewer();
      return 0;
    }

    if (id == IDC_MW_GPU_RELOAD_PRESET && code == BN_CLICKED) {
      // Re-render the current preset with updated GPU protection settings
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
      SendMessage(GetDlgItem(hWnd, IDC_MW_QUALITY_AUTO), BM_SETCHECK, BST_UNCHECKED, 0);
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
      SendMessage(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BY_RES), BM_SETCHECK, BST_UNCHECKED, 0);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BASE), L"1920");
      SendMessage(GetDlgItem(hWnd, IDC_MW_GPU_SKIP_HEAVY), BM_SETCHECK, BST_UNCHECKED, 0);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_HEAVY_THRESHOLD), L"4096");
      p->m_bEnableVSync = true;
      SendMessage(GetDlgItem(hWnd, IDC_MW_VSYNC_ENABLED), BM_SETCHECK, BST_CHECKED, 0);
      // Apply side-effects
      HWND hw = p->GetPluginWindow();
      if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
      if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
      return 0;
    }

    if (id == IDC_MW_RESET_COLORS && code == BN_CLICKED) {
      p->m_ColShiftHue = 0.0f;
      p->m_ColShiftSaturation = 0.0f;
      p->m_ColShiftBrightness = 0.0f;
      if (p->m_pState) p->m_pState->m_fGammaAdj = 2.0f;
      p->m_AutoHue = false;
      p->m_AutoHueSeconds = 0.02f;
      // Update sliders (H/S/B center=100, gamma 2.0 = pos 20)
      SendMessage(GetDlgItem(hWnd, IDC_MW_COL_HUE), TBM_SETPOS, TRUE, 100);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_HUE_LABEL), L"0.00");
      SendMessage(GetDlgItem(hWnd, IDC_MW_COL_SAT), TBM_SETPOS, TRUE, 100);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_SAT_LABEL), L"0.00");
      SendMessage(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT), TBM_SETPOS, TRUE, 100);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT_LABEL), L"0.00");
      SendMessage(GetDlgItem(hWnd, IDC_MW_COL_GAMMA), TBM_SETPOS, TRUE, 20);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_GAMMA_LABEL), L"2.0");
      // Update checkbox and edit
      SendMessage(GetDlgItem(hWnd, IDC_MW_AUTO_HUE), BM_SETCHECK, BST_UNCHECKED, 0);
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_AUTO_HUE_SEC), L"0.020");
      return 0;
    }

    if (id == IDC_MW_RESET_ALL && code == BN_CLICKED) {
      ResetToFactory();
      return 0;
    }

    if (id == IDC_MW_SAVE_DEFAULTS && code == BN_CLICKED) {
      p->SaveUserDefaults();
      return 0;
    }

    if (id == IDC_MW_USER_RESET && code == BN_CLICKED) {
      ResetToUserDefaults();
      return 0;
    }

    // (IDC_MW_RESET_WINDOW, IDC_MW_FONT_PLUS, IDC_MW_FONT_MINUS handled by BaseWndProc)

    if (id == IDC_MW_OPEN_DISPLAYS && code == BN_CLICKED) {
      p->OpenDisplaysWindow();
      return 0;
    }

    if (id == IDC_MW_OPEN_SONGINFO && code == BN_CLICKED) {
      p->OpenSongInfoWindow();
      return 0;
    }

    if (id == IDC_MW_OPEN_BOARD && code == BN_CLICKED) {
      p->OpenBoardWindow();
      return 0;
    }

    if (id == IDC_MW_OPEN_SHIMPORT && code == BN_CLICKED) {
      p->OpenShaderImportWindow();
      return 0;
    }

    if (id == IDC_MW_OPEN_PRESETS && code == BN_CLICKED) {
      p->OpenPresetsWindow();
      return 0;
    }

    if (id == IDC_MW_OPEN_SPRITES && code == BN_CLICKED) {
      p->OpenSpritesWindow();
      return 0;
    }

    if (id == IDC_MW_OPEN_MESSAGES && code == BN_CLICKED) {
      p->OpenMessagesWindow();
      return 0;
    }

    if (id == IDC_MW_FILE_ADD && code == BN_CLICKED) {
      // Open folder picker
      IFileDialog* pfd = NULL;
      HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
      if (SUCCEEDED(hr)) {
        DWORD opts;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->SetTitle(L"Add Fallback Search Path");
        if (SUCCEEDED(pfd->Show(hWnd))) {
          IShellItem* psi;
          if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR pszPath = NULL;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
              std::wstring path(pszPath);
              // Ensure trailing backslash
              if (!path.empty() && path.back() != L'\\') path += L'\\';
              p->m_fallbackPaths.push_back(path);
              HWND hList = GetDlgItem(hWnd, IDC_MW_FILE_LIST);
              if (hList) SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)path.c_str());
              p->SaveFallbackPaths();
              CoTaskMemFree(pszPath);
            }
            psi->Release();
          }
        }
        pfd->Release();
      }
      return 0;
    }

    if (id == IDC_MW_FILE_REMOVE && code == BN_CLICKED) {
      HWND hList = GetDlgItem(hWnd, IDC_MW_FILE_LIST);
      int sel = hList ? (int)SendMessage(hList, LB_GETCURSEL, 0, 0) : -1;
      if (sel >= 0 && sel < (int)p->m_fallbackPaths.size()) {
        p->m_fallbackPaths.erase(p->m_fallbackPaths.begin() + sel);
        SendMessage(hList, LB_DELETESTRING, sel, 0);
        p->SaveFallbackPaths();
      }
      return 0;
    }

    if (id == IDC_MW_CONTENT_BASE_BROWSE && code == BN_CLICKED) {
      IFileDialog* pfd = NULL;
      HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
      if (SUCCEEDED(hr)) {
        DWORD opts;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->SetTitle(L"Select Content Base Path (textures, sprites, etc.)");
        if (SUCCEEDED(pfd->Show(hWnd))) {
          IShellItem* psi = NULL;
          if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR pszPath = NULL;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
              lstrcpynW(p->m_szContentBasePath, pszPath, MAX_PATH);
              int len = lstrlenW(p->m_szContentBasePath);
              if (len > 0 && p->m_szContentBasePath[len - 1] != L'\\') {
                p->m_szContentBasePath[len] = L'\\';
                p->m_szContentBasePath[len + 1] = 0;
              }
              SetWindowTextW(GetDlgItem(hWnd, IDC_MW_CONTENT_BASE_EDIT), p->m_szContentBasePath);
              p->SaveFallbackPaths();
              p->m_bNeedRescanTexturesDir = true;
              CoTaskMemFree(pszPath);
            }
            psi->Release();
          }
        }
        pfd->Release();
      }
      return 0;
    }

    if (id == IDC_MW_CONTENT_BASE_CLEAR && code == BN_CLICKED) {
      p->m_szContentBasePath[0] = 0;
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_CONTENT_BASE_EDIT), L"");
      p->SaveFallbackPaths();
      p->m_bNeedRescanTexturesDir = true;
      return 0;
    }

    if (id == IDC_MW_RANDTEX_BROWSE && code == BN_CLICKED) {
      IFileDialog* pfd = NULL;
      HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
      if (SUCCEEDED(hr)) {
        DWORD opts;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->SetTitle(L"Select Random Textures Directory");
        if (SUCCEEDED(pfd->Show(hWnd))) {
          IShellItem* psi = NULL;
          if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR pszPath = NULL;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
              lstrcpynW(p->m_szRandomTexDir, pszPath, MAX_PATH);
              // Ensure trailing backslash
              int len = lstrlenW(p->m_szRandomTexDir);
              if (len > 0 && p->m_szRandomTexDir[len - 1] != L'\\') {
                p->m_szRandomTexDir[len] = L'\\';
                p->m_szRandomTexDir[len + 1] = 0;
              }
              SetWindowTextW(GetDlgItem(hWnd, IDC_MW_RANDTEX_EDIT), p->m_szRandomTexDir);
              p->SaveFallbackPaths();
              p->m_bNeedRescanTexturesDir = true;
              CoTaskMemFree(pszPath);
            }
            psi->Release();
          }
        }
        pfd->Release();
      }
      return 0;
    }

    if (id == IDC_MW_RANDTEX_CLEAR && code == BN_CLICKED) {
      p->m_szRandomTexDir[0] = 0;
      SetWindowTextW(GetDlgItem(hWnd, IDC_MW_RANDTEX_EDIT), L"");
      p->SaveFallbackPaths();
      p->m_bNeedRescanTexturesDir = true;
      return 0;
    }

    // Messages/Sprites combo box selection
    if (id == IDC_MW_SPRITES_MESSAGES && code == CBN_SELCHANGE) {
      HWND hCombo = (HWND)lParam;
      int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 3) {
        p->m_nSpriteMessagesMode = sel;
        p->SaveSettingToINI(SET_SPRITES_MESSAGES);
      }
      return 0;
    }

    // Theme mode combo box (Dark / Light / Follow System)
    if (id == IDC_MW_DARK_THEME && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 2) {
        p->m_nThemeMode = (Engine::ThemeMode)sel;
        wchar_t buf[8]; swprintf(buf, 8, L"%d", sel);
        WritePrivateProfileStringW(L"SettingsTheme", L"ThemeMode", buf, p->GetConfigIniFile());
        p->LoadSettingsThemeFromINI();
        ApplyDarkTheme();
        p->BroadcastFontSync(m_hWnd);
      }
      return 0;
    }

    // Idle timer action combo box
    if (id == IDC_MW_IDLE_ACTION && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 1) {
        p->m_nIdleAction = sel;
        p->SaveIdleTimerSettings();
      }
      return 0;
    }

    // Fallback texture style combo box
    if (id == IDC_MW_FALLBACK_TEX && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 5) {
        p->m_nFallbackTexStyle = sel;
        WritePrivateProfileIntW(sel, L"FallbackTexStyle", p->GetConfigIniFile(), L"Milkwave");
        // Show/hide custom file controls
        HWND hFileEdit = GetDlgItem(hWnd, IDC_MW_FALLBACK_FILE_EDIT);
        HWND hFileBrowse = GetDlgItem(hWnd, IDC_MW_FALLBACK_FILE_BROWSE);
        HWND hFileClear = GetDlgItem(hWnd, IDC_MW_FALLBACK_FILE_CLEAR);
        int show = (sel == 5) ? SW_SHOW : SW_HIDE;
        if (hFileEdit) ShowWindow(hFileEdit, show);
        if (hFileBrowse) ShowWindow(hFileBrowse, show);
        if (hFileClear) ShowWindow(hFileClear, show);
      }
      return 0;
    }

    // Fallback texture custom file browse
    if (id == IDC_MW_FALLBACK_FILE_BROWSE && code == BN_CLICKED) {
      IFileOpenDialog* pFileOpen = NULL;
      HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_IFileOpenDialog, (void**)&pFileOpen);
      if (SUCCEEDED(hr)) {
        COMDLG_FILTERSPEC filters[] = {
          { L"Image Files", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds" },
          { L"All Files", L"*.*" }
        };
        pFileOpen->SetFileTypes(2, filters);
        pFileOpen->SetTitle(L"Select Fallback Texture");
        if (p->m_szFallbackTexFile[0]) {
          wchar_t szDir[MAX_PATH];
          wcscpy_s(szDir, MAX_PATH, p->m_szFallbackTexFile);
          wchar_t* sl = wcsrchr(szDir, L'\\');
          if (sl) { sl[1] = L'\0'; }
          IShellItem* pFolder = NULL;
          if (SUCCEEDED(SHCreateItemFromParsingName(szDir, NULL, IID_PPV_ARGS(&pFolder)))) {
            pFileOpen->SetFolder(pFolder);
            pFolder->Release();
          }
        }
        hr = pFileOpen->Show(hWnd);
        if (SUCCEEDED(hr)) {
          IShellItem* pItem = NULL;
          hr = pFileOpen->GetResult(&pItem);
          if (SUCCEEDED(hr)) {
            PWSTR pPath = NULL;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath);
            if (SUCCEEDED(hr)) {
              wcscpy_s(p->m_szFallbackTexFile, MAX_PATH, pPath);
              HWND hEdit = GetDlgItem(hWnd, IDC_MW_FALLBACK_FILE_EDIT);
              if (hEdit) SetWindowTextW(hEdit, pPath);
              p->SaveFallbackPaths();
              CoTaskMemFree(pPath);
            }
            pItem->Release();
          }
        }
        pFileOpen->Release();
      }
      return 0;
    }

    // Fallback texture custom file clear
    if (id == IDC_MW_FALLBACK_FILE_CLEAR && code == BN_CLICKED) {
      p->m_szFallbackTexFile[0] = 0;
      HWND hEdit = GetDlgItem(hWnd, IDC_MW_FALLBACK_FILE_EDIT);
      if (hEdit) SetWindowTextW(hEdit, L"");
      p->SaveFallbackPaths();
      return 0;
    }

    // FPS Cap combo box selection
    if (id == IDC_MW_FPS_CAP && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      const int fpsValues[] = { 30, 60, 90, 120, 144, 240, 360, 720, 0 };
      if (sel >= 0 && sel < 9)
        p->SetFPSCap(fpsValues[sel]);
      return 0;
    }

    // Audio device combo box selection
    if (id == IDC_MW_AUDIO_DEVICE && code == CBN_SELCHANGE) {
      HWND hCombo = (HWND)lParam;
      int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
      if (sel >= 0) {
        wchar_t deviceName[MAX_PATH] = {};
        SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)deviceName);

        if (sel == 0) {
          wcscpy_s(p->m_szAudioDevicePrevious, p->m_szAudioDevice);
          p->m_nAudioDevicePreviousType = p->m_nAudioDeviceActiveType;
          p->m_szAudioDevice[0] = L'\0';
          p->m_nAudioDeviceRequestType = 0;
          p->SetAudioDeviceDisplayName(NULL, true);
        }
        else {
          bool isInput = false;
          wchar_t cleanName[MAX_PATH];
          lstrcpyW(cleanName, deviceName);
          int len = lstrlenW(cleanName);
          const wchar_t* inputSuffix = L" [Input]";
          int suffixLen = lstrlenW(inputSuffix);
          if (len > suffixLen && _wcsicmp(cleanName + len - suffixLen, inputSuffix) == 0) {
            cleanName[len - suffixLen] = L'\0';
            isInput = true;
          }
          wcscpy_s(p->m_szAudioDevicePrevious, p->m_szAudioDevice);
          p->m_nAudioDevicePreviousType = p->m_nAudioDeviceActiveType;
          wcscpy_s(p->m_szAudioDevice, cleanName);
          p->m_nAudioDeviceRequestType = isInput ? 1 : 2;
          p->SetAudioDeviceDisplayName(cleanName, !isInput);
        }
        WritePrivateProfileStringW(L"Milkwave", L"AudioDevice", p->m_szAudioDevice, p->GetConfigIniFile());
        wchar_t reqBuf[16];
        swprintf(reqBuf, 16, L"%d", p->m_nAudioDeviceRequestType);
        WritePrivateProfileStringW(L"Milkwave", L"AudioDeviceRequestType", reqBuf, p->GetConfigIniFile());
        p->m_nAudioLoopState = 1;
        p->AddNotificationAudioDevice();
      }
      return 0;
    }

    // Open Hotkeys window
    if (id == IDC_MW_OPEN_HOTKEYS && code == BN_CLICKED) {
      p->OpenHotkeysWindow();
      return 0;
    }

    // Open MIDI window
    if (id == IDC_MW_OPEN_MIDI && code == BN_CLICKED) {
      p->OpenMidiWindow();
      return 0;
    }

    // Close button
    if (id == IDC_MW_CLOSE && code == BN_CLICKED) {
      PostMessage(hWnd, WM_CLOSE, 0, 0);
      return 0;
    }

    // (IDC_MW_SETTINGS_PIN handled by BaseWndProc)

    // Checkbox and radio state is auto-toggled by the base class before DoCommand.
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      bool bChecked = IsChecked(id);
      HWND hw = p->GetPluginWindow();
      switch (id) {
      case IDC_MW_LOGLEVEL_OFF:
      case IDC_MW_LOGLEVEL_ERROR:
      case IDC_MW_LOGLEVEL_WARN:
      case IDC_MW_LOGLEVEL_INFO:
      case IDC_MW_LOGLEVEL_VERBOSE:
      {
        int newLevel = 0;
        switch (id) {
          case IDC_MW_LOGLEVEL_OFF:     newLevel = 0; break;
          case IDC_MW_LOGLEVEL_ERROR:   newLevel = 1; break;
          case IDC_MW_LOGLEVEL_WARN:    newLevel = 2; break;
          case IDC_MW_LOGLEVEL_INFO:    newLevel = 3; break;
          case IDC_MW_LOGLEVEL_VERBOSE: newLevel = 4; break;
        }
        // When enabling logging from Off, auto-enable both output destinations
        if (p->m_LogLevel == 0 && newLevel > 0) {
          p->m_LogOutput = LOG_OUTPUT_BOTH;
          DebugLogSetOutput(p->m_LogOutput);
          WritePrivateProfileIntW(p->m_LogOutput, L"LogOutput", p->GetConfigIniFile(), L"Milkwave");
          SetChecked(IDC_MW_LOGOUTPUT_FILE, true);
          SetChecked(IDC_MW_LOGOUTPUT_ODS, true);
        }
        p->m_LogLevel = newLevel;
        DebugLogSetLevel(newLevel);
        WritePrivateProfileIntW(newLevel, L"LogLevel", p->GetConfigIniFile(), L"Milkwave");
        return 0;
      }
      case IDC_MW_LOGOUTPUT_FILE:
      case IDC_MW_LOGOUTPUT_ODS:
      {
        int bit = (id == IDC_MW_LOGOUTPUT_FILE) ? LOG_OUTPUT_FILE : LOG_OUTPUT_ODS;
        if (bChecked)
          p->m_LogOutput |= bit;
        else
          p->m_LogOutput &= ~bit;
        DebugLogSetOutput(p->m_LogOutput);
        WritePrivateProfileIntW(p->m_LogOutput, L"LogOutput", p->GetConfigIniFile(), L"Milkwave");
        return 0;
      }
      case IDC_MW_HARD_CUTS:
        p->m_bHardCutsDisabled = bChecked;
        p->SaveSettingToINI(SET_HARD_CUTS);
        return 0;
      case IDC_MW_PRESET_LOCK:
        p->m_bPresetLockOnAtStartup = bChecked;
        p->SaveSettingToINI(SET_PRESET_LOCK);
        return 0;
      case IDC_MW_SEQ_ORDER:
        p->m_bSequentialPresetOrder = bChecked;
        p->SaveSettingToINI(SET_SEQ_ORDER);
        return 0;
      case IDC_MW_SHOW_FPS:
        p->m_bShowFPS = bChecked;
        p->SaveSettingToINI(SET_SHOW_FPS);
        return 0;
      case IDC_MW_ALWAYS_TOP:
        p->m_bAlwaysOnTop = bChecked;
        p->SaveSettingToINI(SET_ALWAYS_ON_TOP);
        if (hw) PostMessage(hw, WM_MW_SET_ALWAYS_ON_TOP, 0, 0);
        return 0;
      case IDC_MW_BORDERLESS:
        p->m_WindowBorderless = bChecked;
        p->SaveSettingToINI(SET_BORDERLESS);
        return 0;
      case IDC_MW_QUALITY_AUTO:
        p->bQualityAuto = bChecked;
        if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
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
      case IDC_MW_AUTO_HUE:
        p->m_AutoHue = bChecked;
        return 0;
      case IDC_MW_IDLE_ENABLE: {
        p->m_bIdleTimerEnabled = bChecked;
        if (!bChecked) p->m_bIdleActivated = false;  // Reset activation state
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_TIMEOUT), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_TIMEOUT_SPIN), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_ACTION), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_AUTO_RESTORE), bChecked);
        p->SaveIdleTimerSettings();
        return 0;
      }
      case IDC_MW_IDLE_AUTO_RESTORE:
        p->m_bIdleAutoRestore = bChecked;
        p->SaveIdleTimerSettings();
        return 0;
      case IDC_MW_CTRL_ENABLE: {
        p->m_bControllerEnabled = bChecked;
        p->m_dwLastControllerButtons = 0;
        EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_DEVICE), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_SCAN), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_DEFAULTS), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_SAVE), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_CTRL_LOAD), bChecked);
        if (bChecked) {
          // Parse JSON from the edit control when enabling
          HWND hEdit = GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT);
          if (hEdit) {
            int len = GetWindowTextLengthW(hEdit);
            std::wstring wText(len + 1, L'\0');
            GetWindowTextW(hEdit, &wText[0], len + 1);
            int cbNeeded = WideCharToMultiByte(CP_ACP, 0, wText.c_str(), len, NULL, 0, NULL, NULL);
            p->m_szControllerJSONText.resize(cbNeeded);
            WideCharToMultiByte(CP_ACP, 0, wText.c_str(), len, &p->m_szControllerJSONText[0], cbNeeded, NULL, NULL);
            p->ParseControllerJSON(p->m_szControllerJSONText);
          }
        }
        p->SaveControllerSettings();
        return 0;
      }
      // IDC_MW_DARK_THEME handled as CBN_SELCHANGE combo above
      }
    }

    // Remote tab: Apply & Restart IPC
    if (id == IDC_MW_IPC_APPLY && code == BN_CLICKED) {
      // Read current edit field values
      wchar_t buf[256];
      HWND hEdit = GetDlgItem(hWnd, IDC_MW_IPC_TITLE);
      if (hEdit) {
        GetWindowTextW(hEdit, buf, 256);
        lstrcpyW(p->m_szWindowTitle, buf);
      }
      // Save to INI
      const wchar_t* pIni = p->GetConfigIniFile();
      WritePrivateProfileStringW(L"Milkwave", L"WindowTitle", p->m_szWindowTitle, pIni);
      // Pipe server uses PID-based naming — no restart needed
      RefreshIPCList();
      return 0;
    }

    // Remote tab: Save Screenshot
    if (id == IDC_MW_IPC_CAPTURE && code == BN_CLICKED) {
      // Build a suggested filename from current preset + timestamp
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

      // Remember last directory (static persists across calls)
      static wchar_t sLastDir[MAX_PATH] = {};
      if (sLastDir[0] == L'\0') {
        // Default to capture\ subdirectory
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
        // Remember directory for next time
        wcsncpy_s(sLastDir, filePath, _TRUNCATE);
        wchar_t* lastSlash = wcsrchr(sLastDir, L'\\');
        if (lastSlash) lastSlash[1] = L'\0';

        // Set screenshot path and request capture on render thread
        wcsncpy_s(p->m_screenshotPath, filePath, _TRUNCATE);
        p->m_bScreenshotRequested = true;
      }
      return 0;
    }

    // About tab: Workspace Layout
    if (id == IDC_MW_OPEN_WORKSPACE_LAYOUT && code == BN_CLICKED) {
      m_pEngine->OpenWorkspaceLayoutWindow();
      return 0;
    }

    // About tab: Error Duration edit
    if (id == IDC_MW_ERROR_DISPLAY_SETTINGS && code == EN_KILLFOCUS) {
      wchar_t buf[32];
      GetDlgItemTextW(hWnd, IDC_MW_ERROR_DISPLAY_SETTINGS, buf, 32);
      float val = (float)_wtof(buf);
      if (val < 0.5f) val = 0.5f;
      if (val > 120.0f) val = 120.0f;
      m_pEngine->m_ErrorDuration = val;
      m_pEngine->MyWriteConfig();
      return 0;
    }

    // About tab: Register File Association
    if (id == IDC_MW_FILE_ASSOC && code == BN_CLICKED) {
      wchar_t exePath[MAX_PATH];
      GetModuleFileNameW(NULL, exePath, MAX_PATH);

      wchar_t cmdLine[MAX_PATH + 8];
      swprintf_s(cmdLine, L"\"%s\" \"%%1\"", exePath);

      const wchar_t* extensions[] = { L".milk", L".milk2", L".milk3" };
      bool ok = true;

      for (const wchar_t* ext : extensions) {
        HKEY hKey;
        std::wstring keyPath = std::wstring(L"Software\\Classes\\") + ext;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(),
            0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
          RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)L"MDropDX12.preset",
                         (DWORD)(wcslen(L"MDropDX12.preset") + 1) * sizeof(wchar_t));
          RegCloseKey(hKey);
        } else { ok = false; }
      }

      // ProgId description
      HKEY hKey;
      if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\MDropDX12.preset",
          0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)L"MilkDrop Preset",
                       (DWORD)(wcslen(L"MilkDrop Preset") + 1) * sizeof(wchar_t));
        RegCloseKey(hKey);
      }

      // DefaultIcon
      if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\MDropDX12.preset\\DefaultIcon",
          0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        wchar_t iconPath[MAX_PATH + 4];
        swprintf_s(iconPath, L"%s,0", exePath);
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)iconPath,
                       (DWORD)(wcslen(iconPath) + 1) * sizeof(wchar_t));
        RegCloseKey(hKey);
      }

      // shell\open\command
      if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\MDropDX12.preset\\shell\\open\\command",
          0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)cmdLine,
                       (DWORD)(wcslen(cmdLine) + 1) * sizeof(wchar_t));
        RegCloseKey(hKey);
      } else { ok = false; }

      // Notify Explorer of association change
      SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

      if (ok)
        p->AddNotification(L"File association registered for .milk, .milk2, and .milk3");
      else
        p->AddError((wchar_t*)L"Failed to register file association", p->m_ErrorDuration, ERR_NOTIFY, true);
      return 0;
    }

    // Script tab: Browse
    if (id == IDC_MW_SCRIPT_BROWSE && code == BN_CLICKED) {
      wchar_t filePath[MAX_PATH] = L"";
      OPENFILENAMEW ofn = {};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hWnd;
      ofn.lpstrFilter = L"Script Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
      ofn.lpstrFile = filePath;
      ofn.nMaxFile = MAX_PATH;
      ofn.lpstrInitialDir = p->m_szBaseDir;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
      ofn.lpstrTitle = L"Open Script File";
      if (GetOpenFileNameW(&ofn)) {
        p->LoadScript(filePath);
        SetWindowTextW(GetDlgItem(hWnd, IDC_MW_SCRIPT_FILE), filePath);
        // Populate listbox
        HWND hList = GetDlgItem(hWnd, IDC_MW_SCRIPT_LIST);
        if (hList) {
          SendMessage(hList, LB_RESETCONTENT, 0, 0);
          for (int i = 0; i < (int)p->m_script.lines.size(); i++)
            SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)p->m_script.lines[i].c_str());
        }
      }
      return 0;
    }

    // Script tab: Play
    if (id == IDC_MW_SCRIPT_PLAY && code == BN_CLICKED) {
      // Read BPM/Beats from UI before starting
      wchar_t buf[64];
      HWND hBpm = GetDlgItem(hWnd, IDC_MW_SCRIPT_BPM);
      if (hBpm) { GetWindowTextW(hBpm, buf, 64); double v = _wtof(buf); if (v > 0) p->m_script.bpm = v; }
      HWND hBeats = GetDlgItem(hWnd, IDC_MW_SCRIPT_BEATS);
      if (hBeats) { GetWindowTextW(hBeats, buf, 64); int v = _wtoi(buf); if (v > 0) p->m_script.beats = v; }
      p->StartScript();
      return 0;
    }

    // Script tab: Stop
    if (id == IDC_MW_SCRIPT_STOP && code == BN_CLICKED) {
      p->StopScript();
      return 0;
    }

    // Script tab: Loop checkbox
    if (id == IDC_MW_SCRIPT_LOOP && code == BN_CLICKED) {
      p->m_script.loop = IsChecked(id);
      return 0;
    }

    // Script tab: Listbox double-click to jump
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

    // Edit control changes (apply on focus lost)
    if (code == EN_KILLFOCUS) {
      wchar_t buf[64];
      GetWindowTextW((HWND)lParam, buf, 64);
      HWND hw = p->GetPluginWindow();
      switch (id) {
      case IDC_MW_AUDIO_SENS:
        p->m_fAudioSensitivity = (float)_wtof(buf);
        if (p->m_fAudioSensitivity < -1) p->m_fAudioSensitivity = -1;
        if (p->m_fAudioSensitivity > 256) p->m_fAudioSensitivity = 256;
        if (p->m_fAudioSensitivity == -1.0f) {
          mdropdx12_audio_adaptive = true;
        } else {
          mdropdx12_audio_adaptive = false;
          if (p->m_fAudioSensitivity < 0.5f) p->m_fAudioSensitivity = 0.5f;
          mdropdx12_audio_sensitivity = p->m_fAudioSensitivity;
        }
        p->SaveSettingToINI(SET_AUDIO_SENSITIVITY);
        return 0;
      case IDC_MW_BLEND_TIME:
        p->m_fBlendTimeAuto = (float)_wtof(buf);
        if (p->m_fBlendTimeAuto < 0.1f) p->m_fBlendTimeAuto = 0.1f;
        if (p->m_fBlendTimeAuto > 10) p->m_fBlendTimeAuto = 10;
        p->SaveSettingToINI(SET_BLEND_TIME);
        return 0;
      case IDC_MW_TIME_BETWEEN:
        p->m_fTimeBetweenPresets = (float)_wtof(buf);
        if (p->m_fTimeBetweenPresets < 1) p->m_fTimeBetweenPresets = 1;
        if (p->m_fTimeBetweenPresets > 300) p->m_fTimeBetweenPresets = 300;
        p->SaveSettingToINI(SET_TIME_BETWEEN);
        return 0;
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
      case IDC_MW_AUTO_HUE_SEC:
        p->m_AutoHueSeconds = (float)_wtof(buf);
        if (p->m_AutoHueSeconds < 0.001f) p->m_AutoHueSeconds = 0.001f;
        return 0;
      case IDC_MW_IPC_TITLE: {
        wchar_t tbuf[256];
        GetWindowTextW((HWND)lParam, tbuf, 256);
        lstrcpyW(p->m_szWindowTitle, tbuf);
        return 0;
      }
      // IDC_MW_IPC_REMOTE_TITLE removed — pipe server uses PID-based naming
      case IDC_MW_SCRIPT_BPM: {
        double v = _wtof(buf);
        if (v > 0) p->m_script.bpm = v;
        return 0;
      }
      case IDC_MW_SCRIPT_BEATS: {
        int v = _wtoi(buf);
        if (v > 0) p->m_script.beats = v;
        return 0;
      }
      }
    }

    // ===== Game Controller handlers =====
    if (code == BN_CLICKED && id == IDC_MW_CTRL_HELP) {
      p->ShowControllerHelpPopup(hWnd);
      return 0;
    }
    if (code == BN_CLICKED && id == IDC_MW_CTRL_SCAN) {
      HWND hCombo = GetDlgItem(hWnd, IDC_MW_CTRL_DEVICE);
      if (hCombo) p->EnumerateControllers(hCombo);
      return 0;
    }
    if (code == BN_CLICKED && id == IDC_MW_CTRL_DEFAULTS) {
      HWND hEdit = GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT);
      if (hEdit) {
        std::string def = p->GetDefaultControllerJSON();
        int wLen = MultiByteToWideChar(CP_ACP, 0, def.c_str(), (int)def.size(), NULL, 0);
        std::wstring wDef(wLen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, def.c_str(), (int)def.size(), &wDef[0], wLen);
        SetWindowTextW(hEdit, wDef.c_str());
      }
      return 0;
    }
    if (code == BN_CLICKED && id == IDC_MW_CTRL_SAVE) {
      HWND hEdit = GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT);
      if (hEdit) {
        int len = GetWindowTextLengthW(hEdit);
        std::wstring wText(len + 1, L'\0');
        GetWindowTextW(hEdit, &wText[0], len + 1);
        int cbNeeded = WideCharToMultiByte(CP_ACP, 0, wText.c_str(), len, NULL, 0, NULL, NULL);
        std::string text(cbNeeded, '\0');
        WideCharToMultiByte(CP_ACP, 0, wText.c_str(), len, &text[0], cbNeeded, NULL, NULL);
        p->SaveControllerJSON(text);
      }
      return 0;
    }
    if (code == BN_CLICKED && id == IDC_MW_CTRL_LOAD) {
      p->LoadControllerJSON();
      HWND hEdit = GetDlgItem(hWnd, IDC_MW_CTRL_JSON_EDIT);
      if (hEdit) {
        const std::string& s = p->m_szControllerJSONText;
        int wLen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), NULL, 0);
        std::wstring wJson(wLen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &wJson[0], wLen);
        SetWindowTextW(hEdit, wJson.c_str());
      }
      return 0;
    }
    if (code == CBN_SELCHANGE && id == IDC_MW_CTRL_DEVICE) {
      HWND hCombo = (HWND)lParam;
      int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
      if (sel >= 0) {
        int joyID = (int)SendMessage(hCombo, CB_GETITEMDATA, sel, 0);
        p->m_nControllerDeviceID = joyID;
        p->m_dwLastControllerButtons = 0;
        SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)p->m_szControllerName);
        p->SaveControllerSettings();
      }
      return 0;
    }

    return -1;  // not handled
  }

// Enumerate audio devices into a combo box. Returns the index of the current device, or -1.
static int EnumAudioDevicesIntoCombo(HWND hCombo, const wchar_t* szCurrentDevice) {
  int curIdx = -1;

  // Add "(Default)" as first entry
  SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(Default)");

  HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  IMMDeviceEnumerator* pEnum = NULL;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
  if (SUCCEEDED(hr) && pEnum) {
    // Enumerate render (output) devices
    IMMDeviceCollection* pCollection = NULL;
    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr) && pCollection) {
      UINT count = 0;
      pCollection->GetCount(&count);
      for (UINT i = 0; i < count; i++) {
        IMMDevice* pDev = NULL;
        if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
          IPropertyStore* pProps = NULL;
          if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
              int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)pv.pwszVal);
              if (_wcsicmp(pv.pwszVal, szCurrentDevice) == 0)
                curIdx = idx;
            }
            PropVariantClear(&pv);
            pProps->Release();
          }
          pDev->Release();
        }
      }
      pCollection->Release();
    }
    // Also enumerate capture (input) devices
    pCollection = NULL;
    hr = pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr) && pCollection) {
      UINT count = 0;
      pCollection->GetCount(&count);
      for (UINT i = 0; i < count; i++) {
        IMMDevice* pDev = NULL;
        if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
          IPropertyStore* pProps = NULL;
          if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
              // Mark input devices with [Input] suffix
              wchar_t label[MAX_PATH];
              swprintf(label, MAX_PATH, L"%s [Input]", pv.pwszVal);
              int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)label);
              if (_wcsicmp(pv.pwszVal, szCurrentDevice) == 0)
                curIdx = idx;
            }
            PropVariantClear(&pv);
            pProps->Release();
          }
          pDev->Release();
        }
      }
      pCollection->Release();
    }
    pEnum->Release();
  }
  if (SUCCEEDED(hrCom))
    CoUninitialize();

  // Select current device, or default
  SendMessageW(hCombo, CB_SETCURSEL, curIdx >= 0 ? curIdx : 0, 0);
  return curIdx;
}

// Thin wrappers so existing callers (engine.cpp, engine_input.cpp) still compile
void Engine::OpenSettingsWindow() {
  if (!m_settingsWindow) m_settingsWindow = std::make_unique<SettingsWindow>(this);
  m_settingsWindow->Open();
}

void Engine::CloseSettingsWindow() {
  if (m_settingsWindow) m_settingsWindow->Close();
}

// (Engine::ApplySettingsDarkTheme deleted — handled by ToolWindow::ApplyDarkTheme)

void SettingsWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  int clientW = rcWnd.right;
  int clientH = rcWnd.bottom;

  // Create fonts from shared font size (Settings manages its own — doesn't use BuildBaseControls)
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

  // Tab control (base handles creation, subclass, dark theme)
  const wchar_t* tabNames[] = { L"General", L"Tools", L"Visual", L"Colors", L"System", L"Files", L"Remote", L"Script", L"About" };
  RECT rcDisplay = BuildTabControl(IDC_MW_TAB, tabNames, SETTINGS_NUM_PAGES,
                                    0, 0, clientW, clientH);
  int tabTop = rcDisplay.top;

  // Pin button in the tab header area (right-aligned, sized to match tab header height)
  {
    if (m_hPinFont) DeleteObject(m_hPinFont);
    int pinSize = tabTop - 2;
    m_hPinFont = CreateFontW(-pinSize + 4, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    int pinX = clientW - pinSize - 2;
    HWND hPin = CreateWindowExW(0, L"BUTTON", L"\xE718",
      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
      pinX, 1, pinSize, pinSize, hw,
      (HMENU)(INT_PTR)IDC_MW_SETTINGS_PIN, GetModuleHandle(NULL), NULL);
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
    TrackControl(hPin);
  }

  int lineH = GetLineHeight();
  int gap = 6, x = 16;
  int lw = MulDiv(160, lineH, 26);
  int rw = clientW - 36;
  int sliderW = rw - lw - 60;
  wchar_t buf[64];
  int y;

  // Helper: track control for a page. All controls are children of hw (main window).
  // Pages 1-3 are created hidden; ShowPage(0) is called at the end.
  #define PAGE_CTRL(page, expr) TrackPageControl(page, (expr))

  // Convenience alias so existing code that accessed Engine members directly still compiles
  Engine* const _e = m_pEngine;
  // Macro aliases for engine member access (reduces diff noise)
  #define m_szCurrentPresetFile _e->m_szCurrentPresetFile
  #define m_szPresetDir _e->m_szPresetDir
  #define m_nPresets _e->m_nPresets
  #define m_presets _e->m_presets
  #define m_nCurrentPreset _e->m_nCurrentPreset
  #define m_fAudioSensitivity _e->m_fAudioSensitivity
  #define m_fBlendTimeAuto _e->m_fBlendTimeAuto
  #define m_fTimeBetweenPresets _e->m_fTimeBetweenPresets
  #define m_bHardCutsDisabled _e->m_bHardCutsDisabled
  #define m_bPresetLockOnAtStartup _e->m_bPresetLockOnAtStartup
  #define m_bSequentialPresetOrder _e->m_bSequentialPresetOrder
  #define m_nSpriteMessagesMode _e->m_nSpriteMessagesMode
  #define m_bShowFPS _e->m_bShowFPS
  #define m_bAlwaysOnTop _e->m_bAlwaysOnTop
  #define m_WindowBorderless _e->m_WindowBorderless
  #define IsDarkTheme _e->IsDarkTheme
  #define m_nThemeMode _e->m_nThemeMode
  #define m_nPresetFilter _e->m_nPresetFilter
  #define fOpacity _e->fOpacity
  #define m_fRenderQuality _e->m_fRenderQuality
  #define bQualityAuto _e->bQualityAuto
  #define m_timeFactor _e->m_timeFactor
  #define m_frameFactor _e->m_frameFactor
  #define m_fpsFactor _e->m_fpsFactor
  #define m_VisIntensity _e->m_VisIntensity
  #define m_VisShift _e->m_VisShift
  #define m_VisVersion _e->m_VisVersion
  #define m_nMaxShapeInstances _e->m_nMaxShapeInstances
  #define m_bScaleInstancesByResolution _e->m_bScaleInstancesByResolution
  #define m_nInstanceScaleBaseWidth _e->m_nInstanceScaleBaseWidth
  #define m_bSkipHeavyPresets _e->m_bSkipHeavyPresets
  #define m_nHeavyPresetMaxInstances _e->m_nHeavyPresetMaxInstances
  #define m_bEnableVSync _e->m_bEnableVSync
  #define m_max_fps_fs _e->m_max_fps_fs
  #define m_ColShiftHue _e->m_ColShiftHue
  #define m_ColShiftSaturation _e->m_ColShiftSaturation
  #define m_ColShiftBrightness _e->m_ColShiftBrightness
  #define m_pState _e->m_pState
  #define m_AutoHue _e->m_AutoHue
  #define m_AutoHueSeconds _e->m_AutoHueSeconds
  #define m_szAudioDevice _e->m_szAudioDevice
  #define m_bIdleTimerEnabled _e->m_bIdleTimerEnabled
  #define m_nIdleTimeoutMinutes _e->m_nIdleTimeoutMinutes
  #define m_nIdleAction _e->m_nIdleAction
  #define m_bIdleAutoRestore _e->m_bIdleAutoRestore
  #define m_bControllerEnabled _e->m_bControllerEnabled
  #define m_szControllerJSONText _e->m_szControllerJSONText
  #define m_szWindowTitle _e->m_szWindowTitle
  #define m_szRemoteWindowTitle _e->m_szRemoteWindowTitle
  #define m_szBaseDir _e->m_szBaseDir
  #define m_LogLevel _e->m_LogLevel
  #define m_LogOutput _e->m_LogOutput
  #define m_szContentBasePath _e->m_szContentBasePath
  #define m_fallbackPaths _e->m_fallbackPaths
  #define m_szRandomTexDir _e->m_szRandomTexDir
  #define m_nFallbackTexStyle _e->m_nFallbackTexStyle
  #define m_szFallbackTexFile _e->m_szFallbackTexFile
  #define m_colSettingsCtrlBg _e->m_colSettingsCtrlBg
  #define m_colSettingsText _e->m_colSettingsText
  #define m_script _e->m_script
  #define GetConfigIniFile _e->GetConfigIniFile
  #define EnumerateControllers _e->EnumerateControllers

  // ====== PAGE 0: General ======
  y = tabTop + 10;

  // Current/Startup Preset + Browse
  PAGE_CTRL(SP_GENERAL, CreateLabel(hw, L"Current Preset:", x, y, lw, lineH, hFont));
  PAGE_CTRL(SP_GENERAL, CreateEdit(hw, m_szCurrentPresetFile, IDC_MW_CURRENT_PRESET, x + lw + 4, y, rw - lw - 74, lineH, hFont, ES_READONLY));
  PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Browse", IDC_MW_BROWSE_PRESET, x + rw - 65, y, 65, lineH, hFont));
  y += lineH + gap;

  // ── Other ──
  PAGE_CTRL(SP_GENERAL, CreateLabel(hw, L"Messages/Sprites:", x, y, lw, lineH, hFont));
  {
    HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      x + lw + 4, y, rw - lw - 4, lineH + 4 * lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_SPRITES_MESSAGES,
      GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Off");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Messages");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Sprites");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Messages & Sprites");
    SendMessageW(hCombo, CB_SETCURSEL, m_nSpriteMessagesMode, 0);
    PAGE_CTRL(SP_GENERAL, hCombo);
  }
  y += lineH + 2;
  PAGE_CTRL(SP_GENERAL, CreateCheck(hw, L"Show FPS",                IDC_MW_SHOW_FPS,     x, y, rw, lineH, hFont, m_bShowFPS)); y += lineH + 2;
  PAGE_CTRL(SP_GENERAL, CreateCheck(hw, L"Always On Top",           IDC_MW_ALWAYS_TOP,   x, y, rw, lineH, hFont, m_bAlwaysOnTop)); y += lineH + 2;
  PAGE_CTRL(SP_GENERAL, CreateCheck(hw, L"Borderless Window",       IDC_MW_BORDERLESS,   x, y, rw, lineH, hFont, m_WindowBorderless)); y += lineH + 2;
  {
    int themeLblW = MulDiv(55, lineH, 26);
    PAGE_CTRL(SP_GENERAL, CreateLabel(hw, L"Theme:", x, y, themeLblW, lineH, hFont, false));
    HWND hThemeCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
      WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      x + themeLblW + 4, y, rw - themeLblW - 4, 200, hw,
      (HMENU)(INT_PTR)IDC_MW_DARK_THEME, GetModuleHandle(NULL), NULL);
    if (hThemeCombo && hFont) SendMessage(hThemeCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hThemeCombo, CB_ADDSTRING, 0, (LPARAM)L"Dark");
    SendMessageW(hThemeCombo, CB_ADDSTRING, 0, (LPARAM)L"Light");
    SendMessageW(hThemeCombo, CB_ADDSTRING, 0, (LPARAM)L"Follow System");
    SendMessage(hThemeCombo, CB_SETCURSEL, (WPARAM)m_nThemeMode, 0);
    PAGE_CTRL(SP_GENERAL, hThemeCombo);
  }
  y += lineH + gap + 4;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(95, lineH, 26), bw2 = MulDiv(65, lineH, 26), bw3 = MulDiv(80, lineH, 26);
    PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Resources...", IDC_MW_RESOURCES, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Reset", IDC_MW_RESET_ALL, bx, y, bw2, lineH, hFont)); bx += bw2 + bg;
    PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Save Safe", IDC_MW_SAVE_DEFAULTS, bx, y, bw3, lineH, hFont)); bx += bw3 + bg;
    PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Safe Reset", IDC_MW_USER_RESET, bx, y, bw3, lineH, hFont));
  }
  y += lineH + gap;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(100, lineH, 26), bw2 = MulDiv(55, lineH, 26);
    PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Reset Window", IDC_MW_RESET_WINDOW, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Font +", IDC_MW_FONT_PLUS, bx, y, bw2, lineH, hFont)); bx += bw2 + bg;
    PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Font \x2013", IDC_MW_FONT_MINUS, bx, y, bw2, lineH, hFont));
  }

  // ====== PAGE 1: Visual (created hidden) ======
  y = tabTop + 10;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Opacity:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(SP_VISUAL, CreateSlider(hw, IDC_MW_OPACITY, x + lw + 4, y, sliderW, lineH, 0, 100, (int)(fOpacity * 100), false));
  swprintf(buf, 64, L"%d%%", (int)(fOpacity * 100));
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_OPACITY_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_VISUAL, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Render Quality:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(SP_VISUAL, CreateSlider(hw, IDC_MW_RENDER_QUALITY, x + lw + 4, y, sliderW, lineH, 0, 100, (int)(m_fRenderQuality * 100), false));
  swprintf(buf, 64, L"%.2f", m_fRenderQuality);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_QUALITY_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_VISUAL, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateCheck(hw, L"Auto Quality", IDC_MW_QUALITY_AUTO, x, y, rw, lineH, hFont, bQualityAuto, false));
  y += lineH + gap + 4;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Time Factor:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_timeFactor);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_TIME_FACTOR, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Frame Factor:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_frameFactor);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_FRAME_FACTOR, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"FPS Factor:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_fpsFactor);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_FPS_FACTOR, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Vis Intensity:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_VisIntensity);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_VIS_INTENSITY, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Vis Shift:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.2f", m_VisShift);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_VIS_SHIFT, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Vis Version:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%.0f", m_VisVersion);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_VIS_VERSION, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap + 4;
  PAGE_CTRL(SP_VISUAL, CreateBtn(hw, L"Reset", IDC_MW_RESET_VISUAL, x, y, MulDiv(80, lineH, 26), lineH, hFont));
  y += lineH + gap + 8;

  // -- GPU Protection section --
  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"GPU Protection", x, y, rw, lineH, hFont, false));
  y += lineH + 2;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Max Instances:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%d", m_nMaxShapeInstances);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_GPU_MAX_INST, x + lw + 4, y, 60, lineH, hFont, 0, false));
  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"(0=unlimited)", x + lw + 70, y, 100, lineH, hFont, false));
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateCheck(hw, L"Scale Instances by Resolution", IDC_MW_GPU_SCALE_BY_RES, x, y, rw, lineH, hFont, m_bScaleInstancesByResolution, false));
  y += lineH + 2;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Scale Base Width:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%d", m_nInstanceScaleBaseWidth);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_GPU_SCALE_BASE, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateCheck(hw, L"Skip Heavy Presets", IDC_MW_GPU_SKIP_HEAVY, x, y, rw, lineH, hFont, m_bSkipHeavyPresets, false));
  y += lineH + 2;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"Heavy Threshold:", x, y, lw, lineH, hFont, false));
  swprintf(buf, 64, L"%d", m_nHeavyPresetMaxInstances);
  PAGE_CTRL(SP_VISUAL, CreateEdit(hw, buf, IDC_MW_GPU_HEAVY_THRESHOLD, x + lw + 4, y, 60, lineH, hFont, 0, false));
  y += lineH + gap + 4;

  PAGE_CTRL(SP_VISUAL, CreateCheck(hw, L"Enable VSync", IDC_MW_VSYNC_ENABLED, x, y, rw, lineH, hFont, m_bEnableVSync, false));
  y += lineH + gap;

  PAGE_CTRL(SP_VISUAL, CreateLabel(hw, L"FPS Cap:", x, y, lw, lineH, hFont, false));
  {
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
      WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
      x + lw + 4, y, 120, lineH * 8, hw,
      (HMENU)(INT_PTR)IDC_MW_FPS_CAP, GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    const wchar_t* fpsLabels[] = { L"30", L"60", L"90", L"120", L"144", L"240", L"360", L"720", L"Unlimited" };
    const int fpsValues[] = { 30, 60, 90, 120, 144, 240, 360, 720, 0 };
    int selIdx = 8; // default to Unlimited
    for (int i = 0; i < 9; i++) {
      SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)fpsLabels[i]);
      if (fpsValues[i] == m_max_fps_fs) selIdx = i;
    }
    SendMessage(hCombo, CB_SETCURSEL, selIdx, 0);
    PAGE_CTRL(SP_VISUAL, hCombo);
  }
  y += lineH + gap + 4;

  PAGE_CTRL(SP_VISUAL, CreateBtn(hw, L"Reload Preset", IDC_MW_GPU_RELOAD_PRESET, x, y, MulDiv(110, lineH, 26), lineH, hFont));
  PAGE_CTRL(SP_VISUAL, CreateBtn(hw, L"Restart Render", IDC_MW_RESTART_RENDER, x + MulDiv(120, lineH, 26), y, MulDiv(120, lineH, 26), lineH, hFont));

  // ====== PAGE 2: Colors (created hidden) ======
  y = tabTop + 10;

  PAGE_CTRL(SP_COLORS, CreateLabel(hw, L"Hue:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(SP_COLORS, CreateSlider(hw, IDC_MW_COL_HUE, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(m_ColShiftHue * 100) + 100, false));
  swprintf(buf, 64, L"%.2f", m_ColShiftHue);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_HUE_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_COLORS, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(SP_COLORS, CreateLabel(hw, L"Saturation:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(SP_COLORS, CreateSlider(hw, IDC_MW_COL_SAT, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(m_ColShiftSaturation * 100) + 100, false));
  swprintf(buf, 64, L"%.2f", m_ColShiftSaturation);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_SAT_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_COLORS, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(SP_COLORS, CreateLabel(hw, L"Brightness:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(SP_COLORS, CreateSlider(hw, IDC_MW_COL_BRIGHT, x + lw + 4, y, sliderW, lineH, 0, 200, (int)(m_ColShiftBrightness * 100) + 100, false));
  swprintf(buf, 64, L"%.2f", m_ColShiftBrightness);
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_BRIGHT_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_COLORS, hLbl);
  }
  y += lineH + gap;

  PAGE_CTRL(SP_COLORS, CreateLabel(hw, L"Gamma:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(SP_COLORS, CreateSlider(hw, IDC_MW_COL_GAMMA, x + lw + 4, y, sliderW, lineH, 0, 80, (int)(m_pState->m_fGammaAdj.eval(-1) * 10), false));
  swprintf(buf, 64, L"%.1f", m_pState->m_fGammaAdj.eval(-1));
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | SS_LEFT,
      x + lw + sliderW + 8, y, 50, lineH, hw, (HMENU)(INT_PTR)IDC_MW_COL_GAMMA_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_COLORS, hLbl);
  }
  y += lineH + gap + 4;

  PAGE_CTRL(SP_COLORS, CreateCheck(hw, L"Auto Hue", IDC_MW_AUTO_HUE, x, y, rw / 2, lineH, hFont, m_AutoHue, false));
  PAGE_CTRL(SP_COLORS, CreateLabel(hw, L"Seconds:", x + rw / 2, y, 60, lineH, hFont, false));
  swprintf(buf, 64, L"%.3f", m_AutoHueSeconds);
  PAGE_CTRL(SP_COLORS, CreateEdit(hw, buf, IDC_MW_AUTO_HUE_SEC, x + rw / 2 + 64, y, 70, lineH, hFont, 0, false));
  y += lineH + gap + 4;
  PAGE_CTRL(SP_COLORS, CreateBtn(hw, L"Reset", IDC_MW_RESET_COLORS, x, y, MulDiv(80, lineH, 26), lineH, hFont));

  // ====== PAGE 3: System (created hidden) ======
  y = tabTop + 10;

  // Audio Device
  PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Audio Device:", x, y, rw, lineH, hFont, false));
  y += lineH;
  {
    HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
      x, y, rw, 200, hw, (HMENU)(INT_PTR)IDC_MW_AUDIO_DEVICE, GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    EnumAudioDevicesIntoCombo(hCombo, m_szAudioDevice);
    PAGE_CTRL(SP_SYSTEM, hCombo);
  }
  y += lineH + gap + 8;

  // Keyboard Shortcuts / MIDI
  {
    int btnW = MulDiv(100, lineH, 26);
    int midiW = MulDiv(80, lineH, 26);
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Keyboard Shortcuts", x, y, rw - btnW - midiW - 16, lineH, hFontBold, false));
    PAGE_CTRL(SP_SYSTEM, CreateBtn(hw, L"MIDI...", IDC_MW_OPEN_MIDI, x + rw - btnW - midiW - 8, y, midiW, lineH, hFont));
    PAGE_CTRL(SP_SYSTEM, CreateBtn(hw, L"Hotkeys...", IDC_MW_OPEN_HOTKEYS, x + rw - btnW, y, btnW, lineH, hFont));
  }
  y += lineH + gap + 8;

  // Idle Timer (screensaver mode)
  PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Idle Timer", x, y, rw / 2 - 4, lineH, hFontBold, false));
  PAGE_CTRL(SP_SYSTEM, CreateCheck(hw, L"Enable", IDC_MW_IDLE_ENABLE, x + rw / 2, y, rw / 2, lineH, hFont, false, m_bIdleTimerEnabled));
  y += lineH + gap;

  {
    int lblW = MulDiv(60, lineH, 26);
    int editW = MulDiv(50, lineH, 26);
    int comboW = rw - lblW - editW - MulDiv(100, lineH, 26);

    // Timeout
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Timeout:", x, y, lblW, lineH, hFont, false));
    HWND hEdit = CreateEdit(hw, L"", IDC_MW_IDLE_TIMEOUT, x + lblW, y, editW, lineH, hFont);
    PAGE_CTRL(SP_SYSTEM, hEdit);
    {
      wchar_t buf[16];
      swprintf(buf, 16, L"%d", m_nIdleTimeoutMinutes);
      SetWindowTextW(hEdit, buf);
    }
    HWND hSpin = CreateWindowExW(0, UPDOWN_CLASSW, NULL,
      WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_ALIGNRIGHT,
      0, 0, 0, 0, hw, (HMENU)(INT_PTR)IDC_MW_IDLE_TIMEOUT_SPIN, GetModuleHandle(NULL), NULL);
    if (hSpin) {
      SendMessage(hSpin, UDM_SETRANGE32, 1, 60);
      SendMessage(hSpin, UDM_SETPOS32, 0, m_nIdleTimeoutMinutes);
    }
    PAGE_CTRL(SP_SYSTEM, hSpin);

    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"min", x + lblW + editW + 4, y, MulDiv(30, lineH, 26), lineH, hFont, false));

    // Action combo
    int actionX = x + lblW + editW + MulDiv(40, lineH, 26);
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Action:", actionX, y, lblW, lineH, hFont, false));
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
      WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      actionX + lblW, y, comboW, lineH * 6, hw,
      (HMENU)(INT_PTR)IDC_MW_IDLE_ACTION, GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Fullscreen");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Stretch/Mirror");
    SendMessageW(hCombo, CB_SETCURSEL, m_nIdleAction, 0);
    PAGE_CTRL(SP_SYSTEM, hCombo);

    y += lineH + gap;
    PAGE_CTRL(SP_SYSTEM, CreateCheck(hw, L"Auto-restore on input", IDC_MW_IDLE_AUTO_RESTORE,
      x, y, rw, lineH, hFont, false, m_bIdleAutoRestore));

    // Disable idle timer controls if not enabled
    if (!m_bIdleTimerEnabled) {
      EnableWindow(GetDlgItem(hw, IDC_MW_IDLE_TIMEOUT), FALSE);
      EnableWindow(hSpin, FALSE);
      EnableWindow(hCombo, FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_IDLE_AUTO_RESTORE), FALSE);
    }
  }
  y += lineH + gap + 8;

  // Game Controller
  {
    int titleW = MulDiv(130, lineH, 26);
    int helpW = lineH;  // square button
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Game Controller", x, y, titleW, lineH, hFontBold, false));
    PAGE_CTRL(SP_SYSTEM, CreateBtn(hw, L"\u2753", IDC_MW_CTRL_HELP, x + titleW + 4, y, helpW, lineH, hFont));
  }
  PAGE_CTRL(SP_SYSTEM, CreateCheck(hw, L"Enable", IDC_MW_CTRL_ENABLE, x + rw / 2, y, rw / 2, lineH, hFont, false, m_bControllerEnabled));
  y += lineH + gap;

  {
    int indent = MulDiv(16, lineH, 26);
    int lblW = MulDiv(55, lineH, 26);
    int scanW = MulDiv(60, lineH, 26);
    int comboW = rw - lblW - scanW - 8;

    // Device combo + Scan button
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Device:", x, y, lblW, lineH, hFont, false));
    HWND hCtrlCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
      x + lblW, y, comboW, lineH * 8, hw,
      (HMENU)(INT_PTR)IDC_MW_CTRL_DEVICE, GetModuleHandle(NULL), NULL);
    if (hCtrlCombo && hFont) SendMessage(hCtrlCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_SYSTEM, hCtrlCombo);
    EnumerateControllers(hCtrlCombo);

    PAGE_CTRL(SP_SYSTEM, CreateBtn(hw, L"Scan", IDC_MW_CTRL_SCAN, x + lblW + comboW + 4, y, scanW, lineH, hFont));
    y += lineH + gap;

    // Button Mapping label
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Button Mapping:", x, y, rw, lineH, hFont, false));
    y += lineH;

    // Multiline JSON edit control
    int editH = lineH * 8;
    HWND hJsonEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
      WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
      x, y, rw, editH, hw,
      (HMENU)(INT_PTR)IDC_MW_CTRL_JSON_EDIT, GetModuleHandle(NULL), NULL);
    if (hJsonEdit && hFont) SendMessage(hJsonEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_SYSTEM, hJsonEdit);

    // Populate with current JSON text
    {
      int wLen = MultiByteToWideChar(CP_ACP, 0, m_szControllerJSONText.c_str(), (int)m_szControllerJSONText.size(), NULL, 0);
      std::wstring wJson(wLen, L'\0');
      MultiByteToWideChar(CP_ACP, 0, m_szControllerJSONText.c_str(), (int)m_szControllerJSONText.size(), &wJson[0], wLen);
      SetWindowTextW(hJsonEdit, wJson.c_str());
    }
    y += editH + gap;

    // Defaults / Save / Load buttons
    int btnW = MulDiv(70, lineH, 26);
    int btnGap = 6;
    PAGE_CTRL(SP_SYSTEM, CreateBtn(hw, L"Defaults", IDC_MW_CTRL_DEFAULTS, x, y, btnW, lineH, hFont));
    PAGE_CTRL(SP_SYSTEM, CreateBtn(hw, L"Save", IDC_MW_CTRL_SAVE, x + btnW + btnGap, y, btnW, lineH, hFont));
    PAGE_CTRL(SP_SYSTEM, CreateBtn(hw, L"Load", IDC_MW_CTRL_LOAD, x + 2 * (btnW + btnGap), y, btnW, lineH, hFont));

    // Disable sub-controls if not enabled
    if (!m_bControllerEnabled) {
      EnableWindow(hCtrlCombo, FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_CTRL_SCAN), FALSE);
      EnableWindow(hJsonEdit, FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_CTRL_DEFAULTS), FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_CTRL_SAVE), FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_CTRL_LOAD), FALSE);
    }
  }

  // ====== PAGE 4: Files (created hidden) ======
  y = tabTop + 10;

  // Content Base Path (for textures, sprites, etc.)
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"Content Base Path (textures, sprites, etc.):",
      WS_CHILD | SS_LEFT, x, y, rw, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_CONTENT_BASE_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_FILES, hLbl);
  }
  y += lineH + 2;
  {
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_szContentBasePath,
      WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_CONTENT_BASE_EDIT,
      GetModuleHandle(NULL), NULL);
    if (hEdit && hFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_FILES, hEdit);
  }
  y += lineH + 6;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(80, lineH, 26), bw2 = MulDiv(60, lineH, 26);
    PAGE_CTRL(SP_FILES, CreateBtn(hw, L"Browse...", IDC_MW_CONTENT_BASE_BROWSE, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(SP_FILES, CreateBtn(hw, L"Clear", IDC_MW_CONTENT_BASE_CLEAR, bx, y, bw2, lineH, hFont));
  }
  y += lineH + gap + 4;

  PAGE_CTRL(SP_FILES, CreateLabel(hw, L"Fallback Search Paths:", x, y, rw, lineH, hFont, false));
  y += lineH + 2;
  {
    HWND hList = CreateWindowExW(0, L"LISTBOX", L"",
      WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
      x, y, rw, 80, hw, (HMENU)(INT_PTR)IDC_MW_FILE_LIST,
      GetModuleHandle(NULL), NULL);
    if (hList && hFont) SendMessage(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
    // Populate from m_fallbackPaths
    for (auto& p : m_fallbackPaths)
      SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)p.c_str());
    PAGE_CTRL(SP_FILES, hList);
  }
  y += 84;
  {
    int bx = x, bg = 4;
    int fbw = MulDiv(70, lineH, 26);
    PAGE_CTRL(SP_FILES, CreateBtn(hw, L"Add...", IDC_MW_FILE_ADD, bx, y, fbw, lineH, hFont)); bx += fbw + bg;
    PAGE_CTRL(SP_FILES, CreateBtn(hw, L"Remove", IDC_MW_FILE_REMOVE, bx, y, fbw, lineH, hFont));
  }
  y += lineH + gap;
  {
    HWND hDesc = CreateWindowExW(0, L"STATIC",
      L"These paths are searched for textures and presets\nin addition to the built-in directories.",
      WS_CHILD | SS_LEFT, x, y, rw, lineH * 2, hw,
      (HMENU)(INT_PTR)IDC_MW_FILE_DESC, GetModuleHandle(NULL), NULL);
    if (hDesc && hFont) SendMessage(hDesc, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_FILES, hDesc);
  }
  y += lineH * 2 + gap;

  // Random Textures Directory
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"Random Textures Directory:",
      WS_CHILD | SS_LEFT, x, y, rw, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_RANDTEX_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_FILES, hLbl);
  }
  y += lineH + 2;
  {
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_szRandomTexDir,
      WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_RANDTEX_EDIT,
      GetModuleHandle(NULL), NULL);
    if (hEdit && hFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_FILES, hEdit);
  }
  y += lineH + 6;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(80, lineH, 26), bw2 = MulDiv(60, lineH, 26);
    PAGE_CTRL(SP_FILES, CreateBtn(hw, L"Browse...", IDC_MW_RANDTEX_BROWSE, bx, y, bw1, lineH, hFont)); bx += bw1 + bg;
    PAGE_CTRL(SP_FILES, CreateBtn(hw, L"Clear", IDC_MW_RANDTEX_CLEAR, bx, y, bw2, lineH, hFont));
  }
  y += lineH + gap + 4;

  // Fallback Texture Style
  {
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"Fallback Texture (for missing textures):",
      WS_CHILD | SS_LEFT, x, y, rw, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_FALLBACK_TEX_LABEL, GetModuleHandle(NULL), NULL);
    if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_FILES, hLbl);
  }
  y += lineH + 2;
  {
    int comboW = MulDiv(260, lineH, 26);
    HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      x, y, comboW, lineH + 8 * lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_FALLBACK_TEX, GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Hue Gradient");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"White");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Black");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Random (Random Tex Dir)");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Random (Textures Dir)");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Custom File...");
    SendMessageW(hCombo, CB_SETCURSEL, m_nFallbackTexStyle, 0);
    PAGE_CTRL(SP_FILES, hCombo);
  }
  y += lineH + 6;
  // Custom fallback texture file controls (visible only when style == 5)
  {
    DWORD showStyle = (m_nFallbackTexStyle == 5) ? WS_CHILD : (WS_CHILD & ~WS_VISIBLE);
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_szFallbackTexFile,
      showStyle | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_FALLBACK_FILE_EDIT,
      GetModuleHandle(NULL), NULL);
    if (hEdit && hFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    if (m_nFallbackTexStyle == 5) ShowWindow(hEdit, SW_SHOW);
    PAGE_CTRL(SP_FILES, hEdit);
  }
  y += lineH + 6;
  {
    int bx = x, bg = 4;
    int bw1 = MulDiv(80, lineH, 26), bw2 = MulDiv(60, lineH, 26);
    HWND hBrowse = CreateBtn(hw, L"Browse...", IDC_MW_FALLBACK_FILE_BROWSE, bx, y, bw1, lineH, hFont);
    PAGE_CTRL(SP_FILES, hBrowse); bx += bw1 + bg;
    HWND hClear = CreateBtn(hw, L"Clear", IDC_MW_FALLBACK_FILE_CLEAR, bx, y, bw2, lineH, hFont);
    PAGE_CTRL(SP_FILES, hClear);
    if (m_nFallbackTexStyle != 5) {
      ShowWindow(hBrowse, SW_HIDE);
      ShowWindow(hClear, SW_HIDE);
    }
  }

  // ===== Script tab (page 6) =====
  y = tabTop + 10;

  // Script file path + Browse
  PAGE_CTRL(SP_SCRIPT, CreateLabel(hw, L"Script File:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(SP_SCRIPT, CreateEdit(hw, L"", IDC_MW_SCRIPT_FILE, x + lw + 4, y, rw - lw - 4 - 80 - 4, lineH, hFont, ES_READONLY, false));
  PAGE_CTRL(SP_SCRIPT, CreateBtn(hw, L"Browse...", IDC_MW_SCRIPT_BROWSE, x + rw - 80, y, 80, lineH, hFont, false));
  y += lineH + gap;

  // Play / Stop / Loop
  {
    int btnW = MulDiv(80, lineH, 26);
    PAGE_CTRL(SP_SCRIPT, CreateBtn(hw, L"Play", IDC_MW_SCRIPT_PLAY, x, y, btnW, lineH, hFont, false));
    PAGE_CTRL(SP_SCRIPT, CreateBtn(hw, L"Stop", IDC_MW_SCRIPT_STOP, x + btnW + 4, y, btnW, lineH, hFont, false));
    PAGE_CTRL(SP_SCRIPT, CreateCheck(hw, L"Loop", IDC_MW_SCRIPT_LOOP, x + btnW * 2 + 12, y, 80, lineH, hFont, false, false));
    y += lineH + gap;
  }

  // BPM + Beats
  PAGE_CTRL(SP_SCRIPT, CreateLabel(hw, L"BPM:", x, y, 40, lineH, hFont, false));
  PAGE_CTRL(SP_SCRIPT, CreateEdit(hw, L"120.0", IDC_MW_SCRIPT_BPM, x + 44, y, 70, lineH, hFont, 0, false));
  PAGE_CTRL(SP_SCRIPT, CreateLabel(hw, L"Beats:", x + 130, y, 50, lineH, hFont, false));
  PAGE_CTRL(SP_SCRIPT, CreateEdit(hw, L"4", IDC_MW_SCRIPT_BEATS, x + 184, y, 50, lineH, hFont, 0, false));
  y += lineH + gap;

  // Line status label (with ID for dynamic updates)
  {
    HWND hLineLabel = CreateWindowExW(0, L"STATIC", L"No script loaded",
      WS_CHILD | SS_LEFT, x, y, rw, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_SCRIPT_LINE, GetModuleHandle(NULL), NULL);
    if (hLineLabel && hFont) SendMessage(hLineLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_SCRIPT, hLineLabel);
  }
  y += lineH + gap;

  // Script lines listbox
  {
    int listH = lineH * 14;
    HWND hScriptList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
      WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
      x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_SCRIPT_LIST, GetModuleHandle(NULL), NULL);
    if (hScriptList && hFont) SendMessage(hScriptList, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_SCRIPT, hScriptList);
  }

  // ====== PAGE 1: Tools ======
  y = tabTop + 10;
  {
    RECT rc;
    GetClientRect(hw, &rc);
    int listH = rc.bottom - y - 16;
    if (listH < lineH * 5) listH = lineH * 5;

    HWND hList = CreateThemedListView(IDC_MW_TOOLS_LIST, x, y, rw, listH,
                                       /*visible=*/true, /*sortable=*/true);
    PAGE_CTRL(SP_TOOLS, hList);
    if (hList) {
      int scrollW = GetSystemMetrics(SM_CXVSCROLL) + 4;
      int colTool = MulDiv(rw, 60, 100);
      int colKey  = rw - colTool - scrollW;

      LVCOLUMNW col = {};
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = (LPWSTR)L"Tool";
      col.cx = colTool;
      SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
      col.pszText = (LPWSTR)L"Hotkey";
      col.cx = colKey;
      SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

      // Populate from m_hotkeys[] filtered to HKCAT_TOOLS
      // lParam = m_hotkeys array index (for dispatch via m_hotkeys[i].id)
      int row = 0;
      for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (_e->m_hotkeys[i].category != HKCAT_TOOLS) continue;

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = row;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)_e->m_hotkeys[i].szAction;
        lvi.lParam = (LPARAM)i;  // m_hotkeys array index
        int idx = (int)SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

        std::wstring hkStr = _e->FormatHotkeyDisplay(_e->m_hotkeys[i].modifiers, _e->m_hotkeys[i].vk);
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.iSubItem = 1;
        lvi.pszText = (LPWSTR)hkStr.c_str();
        SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi);
        row++;
      }


      ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
  }

  // ===== About tab (page 8) =====
  y = tabTop + 10;

  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"MDropDX12", x, y, rw, 24, hFontBold, false));
  y += 28;

  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Version " MDROP_VERSION_STRW, x, y, rw, lineH, hFont, false));
  y += lineH + 4;

  {
    wchar_t szBuild[128];
    wchar_t wDate[32], wTime[32];
    MultiByteToWideChar(CP_ACP, 0, __DATE__, -1, wDate, 32);
    MultiByteToWideChar(CP_ACP, 0, __TIME__, -1, wTime, 32);
    swprintf(szBuild, 128, L"Built: %s  %s", wDate, wTime);
    PAGE_CTRL(SP_ABOUT, CreateLabel(hw, szBuild, x, y, rw, lineH, hFont, false));
    y += lineH + 4;
  }

  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"MilkDrop2-based music visualizer", x, y, rw, lineH, hFont, false));
  y += lineH + 4;
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"DirectX 12 / Windows 11 64-bit", x, y, rw, lineH, hFont, false));
  y += lineH + 12;

  // Paths section
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Paths:", x, y, rw, lineH, hFontBold, false));
  y += lineH + 2;
  {
    wchar_t buf[MAX_PATH + 64];
    swprintf(buf, MAX_PATH + 64, L"Base Dir:  %s", m_szBaseDir);
    PAGE_CTRL(SP_ABOUT, CreateLabel(hw, buf, x, y, rw, lineH, hFont, false));
    y += lineH + 2;
    swprintf(buf, MAX_PATH + 64, L"Settings:  %s", GetConfigIniFile());
    PAGE_CTRL(SP_ABOUT, CreateLabel(hw, buf, x, y, rw, lineH, hFont, false));
    y += lineH + 2;
    swprintf(buf, MAX_PATH + 64, L"Presets:   %s", m_szPresetDir);
    PAGE_CTRL(SP_ABOUT, CreateLabel(hw, buf, x, y, rw, lineH, hFont, false));
    y += lineH + 2;
  }
  y += 8;

  // Debug Log Level radio buttons
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Debug Log Level:", x, y, lw, lineH, hFont, false));
  {
    int rx = x + lw + 4;
    int rbw = 80;
    PAGE_CTRL(SP_ABOUT, CreateRadio(hw, L"Off",     IDC_MW_LOGLEVEL_OFF,     rx,            y, rbw, lineH, hFont, m_LogLevel == 0, true,  false, IDC_MW_LOGLEVEL_OFF));
    rx += rbw;
    PAGE_CTRL(SP_ABOUT, CreateRadio(hw, L"Error",   IDC_MW_LOGLEVEL_ERROR,   rx,            y, rbw, lineH, hFont, m_LogLevel == 1, false, false, IDC_MW_LOGLEVEL_OFF));
    rx += rbw;
    PAGE_CTRL(SP_ABOUT, CreateRadio(hw, L"Warn",    IDC_MW_LOGLEVEL_WARN,    rx,            y, rbw, lineH, hFont, m_LogLevel == 2, false, false, IDC_MW_LOGLEVEL_OFF));
    rx += rbw;
    PAGE_CTRL(SP_ABOUT, CreateRadio(hw, L"Info",    IDC_MW_LOGLEVEL_INFO,    rx,            y, rbw, lineH, hFont, m_LogLevel == 3, false, false, IDC_MW_LOGLEVEL_OFF));
    rx += rbw;
    PAGE_CTRL(SP_ABOUT, CreateRadio(hw, L"Verbose", IDC_MW_LOGLEVEL_VERBOSE, rx,            y, rbw, lineH, hFont, m_LogLevel == 4, false, false, IDC_MW_LOGLEVEL_OFF));
  }
  y += lineH + 4;

  // Log Output destination checkboxes
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Log Output:", x, y, lw, lineH, hFont, false));
  {
    int cx = x + lw + 4;
    int cbw = 120;
    PAGE_CTRL(SP_ABOUT, CreateCheck(hw, L"File",            IDC_MW_LOGOUTPUT_FILE, cx, y, cbw, lineH, hFont, (m_LogOutput & LOG_OUTPUT_FILE) != 0));
    cx += cbw;
    PAGE_CTRL(SP_ABOUT, CreateCheck(hw, L"Debug Messages",  IDC_MW_LOGOUTPUT_ODS,  cx, y, cbw + 40, lineH, hFont, (m_LogOutput & LOG_OUTPUT_ODS) != 0));
  }
  y += lineH + 8;

  // File Association button
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"File Association:", x, y, lw, lineH, hFont, false));
  {
    int btnW = MulDiv(200, lineH, 26);
    PAGE_CTRL(SP_ABOUT, CreateBtn(hw, L"Register .milk / .milk2 / .milk3", IDC_MW_FILE_ASSOC, x + lw + 4, y, btnW, lineH, hFont, false));
  }
  y += lineH + 2;
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"(Associates preset files with this exe for double-click open)", x + lw + 4, y, rw - lw - 4, lineH, hFont, false));
  y += lineH + 8;

  // Workspace Layout button
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Workspace:", x, y, lw, lineH, hFont, false));
  {
    int btnW = MulDiv(200, lineH, 26);
    PAGE_CTRL(SP_ABOUT, CreateBtn(hw, L"Setup Workspace Layout...", IDC_MW_OPEN_WORKSPACE_LAYOUT, x + lw + 4, y, btnW, lineH, hFont, false));
  }
  y += lineH + 2;
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"(Tile tool windows across screen with render preview in corner)", x + lw + 4, y, rw - lw - 4, lineH, hFont, false));
  y += lineH + 8;

  // Error Duration
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Error Duration (s):", x, y, lw, lineH, hFont, false));
  {
    int ew = 60;
    PAGE_CTRL(SP_ABOUT, CreateEdit(hw, L"", IDC_MW_ERROR_DISPLAY_SETTINGS, x + lw + 4, y, ew, lineH, hFont, false));
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f", m_pEngine->m_ErrorDuration);
    SetDlgItemTextW(hw, IDC_MW_ERROR_DISPLAY_SETTINGS, buf);
  }

  // ===== Remote tab (page 5) =====
  y = tabTop + 10;

  // Section header
  PAGE_CTRL(SP_REMOTE, CreateLabel(hw, L"Milkwave Remote Compatibility", x, y, rw, lineH, hFontBold, false));
  y += lineH + gap;

  // Info line
  PAGE_CTRL(SP_REMOTE, CreateLabel(hw, L"Configure window titles so Milkwave Remote (or other controllers) can discover this instance.", x, y, rw, lineH * 2, hFont, false));
  y += lineH * 2 + gap;

  // Window Title
  PAGE_CTRL(SP_REMOTE, CreateLabel(hw, L"Window Title:", x, y, lw, lineH, hFont, false));
  PAGE_CTRL(SP_REMOTE, CreateEdit(hw, m_szWindowTitle, IDC_MW_IPC_TITLE, x + lw + 4, y, rw - lw - 4, lineH, hFont, 0, false));
  y += lineH + 2;

  // Hint
  PAGE_CTRL(SP_REMOTE, CreateLabel(hw, L"(empty = \"MDropDX12 Visualizer\"  |  e.g. \"Milkwave Visualizer\")", x + lw + 4, y, rw - lw - 4, lineH, hFont, false));
  y += lineH + gap;

  // Apply button + Capture Screenshot button
  {
    int applyW = MulDiv(100, lineH, 26);
    PAGE_CTRL(SP_REMOTE, CreateBtn(hw, L"Apply", IDC_MW_IPC_APPLY, x, y, applyW, lineH, hFont, false));
    int captureW = MulDiv(130, lineH, 26);
    PAGE_CTRL(SP_REMOTE, CreateBtn(hw, L"Save Screenshot...", IDC_MW_IPC_CAPTURE, x + applyW + 8, y, captureW, lineH, hFont, false));
    y += lineH + gap + 8;
  }

  // Named Pipe IPC status
  PAGE_CTRL(SP_REMOTE, CreateLabel(hw, L"Named Pipe IPC", x, y, rw, lineH, hFontBold, false));
  y += lineH + gap;

  // IPC list box
  {
    int listH = lineH * 6;
    HWND hIPCList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
      WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
      x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_LIST, GetModuleHandle(NULL), NULL);
    if (hIPCList && hFont) SendMessage(hIPCList, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_REMOTE, hIPCList);
    y += listH + gap;
  }

  // Last Message group box
  {
    int groupH = lineH * 5;
    HWND hGroup = CreateWindowExW(0, L"BUTTON", L"Last message:",
      WS_CHILD | BS_GROUPBOX,
      x, y, rw, groupH, hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_GROUP,
      GetModuleHandle(NULL), NULL);
    if (hGroup && hFont) SendMessage(hGroup, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_REMOTE, hGroup);

    int pad = 8;
    HWND hMsgText = CreateWindowExW(0, L"EDIT", L"",
      WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
      x + pad, y + lineH + 2, rw - pad * 2, groupH - lineH - pad - 2,
      hw, (HMENU)(INT_PTR)IDC_MW_IPC_MSG_TEXT,
      GetModuleHandle(NULL), NULL);
    if (hMsgText && hFont) SendMessage(hMsgText, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_REMOTE, hMsgText);
  }

  // Populate IPC list with current state
  RefreshIPCList();

  // Start IPC message monitor timer (500ms polling)
  SetTimer(hw, IDT_IPC_MONITOR, 500, NULL);

  #undef PAGE_CTRL

  // Undef all engine-member macros
  #undef m_szCurrentPresetFile
  #undef m_szPresetDir
  #undef m_nPresets
  #undef m_presets
  #undef m_nCurrentPreset
  #undef m_fAudioSensitivity
  #undef m_fBlendTimeAuto
  #undef m_fTimeBetweenPresets
  #undef m_bHardCutsDisabled
  #undef m_bPresetLockOnAtStartup
  #undef m_bSequentialPresetOrder
  #undef m_nSpriteMessagesMode
  #undef m_bShowFPS
  #undef m_bAlwaysOnTop
  #undef m_WindowBorderless
  #undef IsDarkTheme
  #undef m_nThemeMode
  #undef m_nPresetFilter
  #undef fOpacity
  #undef m_fRenderQuality
  #undef bQualityAuto
  #undef m_timeFactor
  #undef m_frameFactor
  #undef m_fpsFactor
  #undef m_VisIntensity
  #undef m_VisShift
  #undef m_VisVersion
  #undef m_nMaxShapeInstances
  #undef m_bScaleInstancesByResolution
  #undef m_nInstanceScaleBaseWidth
  #undef m_bSkipHeavyPresets
  #undef m_nHeavyPresetMaxInstances
  #undef m_bEnableVSync
  #undef m_max_fps_fs
  #undef m_ColShiftHue
  #undef m_ColShiftSaturation
  #undef m_ColShiftBrightness
  #undef m_pState
  #undef m_AutoHue
  #undef m_AutoHueSeconds
  #undef m_szAudioDevice
  #undef m_bIdleTimerEnabled
  #undef m_nIdleTimeoutMinutes
  #undef m_nIdleAction
  #undef m_bIdleAutoRestore
  #undef m_bControllerEnabled
  #undef m_szControllerJSONText
  #undef m_szWindowTitle
  #undef m_szRemoteWindowTitle
  #undef m_szBaseDir
  #undef m_LogLevel
  #undef m_LogOutput
  #undef m_szContentBasePath
  #undef m_fallbackPaths
  #undef m_szRandomTexDir
  #undef m_nFallbackTexStyle
  #undef m_szFallbackTexFile
  #undef m_colSettingsCtrlBg
  #undef m_colSettingsText
  #undef m_script
  #undef GetConfigIniFile
  #undef EnumerateControllers

  SelectInitialTab();
}

void SettingsWindow::RefreshIPCList() {
  HWND hList = GetDlgItem(m_hWnd, IDC_MW_IPC_LIST);
  if (!hList) return;

  SendMessage(hList, LB_RESETCONTENT, 0, 0);

  extern PipeServer g_pipeServer;
  if (g_pipeServer.IsRunning()) {
    wchar_t entry[512];
    if (g_pipeServer.IsConnected())
      swprintf_s(entry, L"%s  \u2014  Connected", g_pipeServer.GetPipeName());
    else
      swprintf_s(entry, L"%s  \u2014  Listening", g_pipeServer.GetPipeName());
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)entry);
  } else {
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"(pipe server not running)");
  }
}

void SettingsWindow::LayoutControls() {
  if (!m_hWnd || !m_hTab) return;

  RECT rc;
  GetClientRect(m_hWnd, &rc);
  MoveWindow(m_hTab, 0, 0, rc.right, rc.bottom, TRUE);

  // Get the content area inside the tab control
  RECT rcDisplay = { 0, 0, rc.right, rc.bottom };
  TabCtrl_AdjustRect(m_hTab, FALSE, &rcDisplay);

  // Reposition pin button to top-right of tab header area
  HWND hPin = GetDlgItem(m_hWnd, IDC_MW_SETTINGS_PIN);
  if (hPin) {
    int pinSize = rcDisplay.top - 2;
    MoveWindow(hPin, rc.right - pinSize - 2, 1, pinSize, pinSize, TRUE);
  }

  int lineH = GetLineHeight();
  int rw = rc.right - 36;  // 16px left + 20px right margin
  int lw = MulDiv(160, lineH, 26);
  int newSliderW = rw - lw - 60;
  if (newSliderW < 80) newSliderW = 80;

  // Stretch edit + reposition Browse buttons for Current Preset and Preset Dir rows.
  // Note: r.left is the edit's x, not the window margin. Button must be positioned
  // relative to the content area (xMargin + rw), not relative to the edit.
  int xMargin = 16;
  HWND hCur = GetDlgItem(m_hWnd, IDC_MW_CURRENT_PRESET);
  if (hCur) {
    RECT r; GetWindowRect(hCur, &r);
    MapWindowPoints(NULL, m_hWnd, (POINT*)&r, 2);
    MoveWindow(hCur, r.left, r.top, xMargin + rw - 70 - r.left, r.bottom - r.top, TRUE);
    HWND hBrw = GetDlgItem(m_hWnd, IDC_MW_BROWSE_PRESET);
    if (hBrw) MoveWindow(hBrw, xMargin + rw - 65, r.top, 65, r.bottom - r.top, TRUE);
  }
  // Stretch audio combo
  HWND hAudio = GetDlgItem(m_hWnd, IDC_MW_AUDIO_DEVICE);
  if (hAudio) {
    RECT r; GetWindowRect(hAudio, &r);
    MapWindowPoints(NULL, m_hWnd, (POINT*)&r, 2);
    MoveWindow(hAudio, r.left, r.top, rw, 200, TRUE);
  }

  // Stretch controller device combo + reposition Scan button
  {
    HWND hCtrlCombo = GetDlgItem(m_hWnd, IDC_MW_CTRL_DEVICE);
    HWND hScan = GetDlgItem(m_hWnd, IDC_MW_CTRL_SCAN);
    if (hCtrlCombo && hScan) {
      RECT rc; GetWindowRect(hCtrlCombo, &rc);
      MapWindowPoints(NULL, m_hWnd, (POINT*)&rc, 2);
      RECT rs; GetWindowRect(hScan, &rs);
      MapWindowPoints(NULL, m_hWnd, (POINT*)&rs, 2);
      int scanW = rs.right - rs.left;
      int rightEdge = rcDisplay.left + 16 + rw;
      int comboW = rightEdge - rc.left - scanW - 4;
      MoveWindow(hCtrlCombo, rc.left, rc.top, comboW, lineH * 8, TRUE);
      MoveWindow(hScan, rc.left + comboW + 4, rs.top, scanW, rs.bottom - rs.top, TRUE);
    }
  }

  // Stretch controller JSON edit
  HWND hJsonEdit = GetDlgItem(m_hWnd, IDC_MW_CTRL_JSON_EDIT);
  if (hJsonEdit) {
    RECT r; GetWindowRect(hJsonEdit, &r);
    MapWindowPoints(NULL, m_hWnd, (POINT*)&r, 2);
    MoveWindow(hJsonEdit, r.left, r.top, rw, r.bottom - r.top, TRUE);
  }

  // Stretch sliders + reposition value labels
  int sliderIDs[] = { IDC_MW_OPACITY, IDC_MW_RENDER_QUALITY, IDC_MW_COL_HUE, IDC_MW_COL_SAT, IDC_MW_COL_BRIGHT };
  int labelIDs[] = { IDC_MW_OPACITY_LABEL, IDC_MW_QUALITY_LABEL, IDC_MW_COL_HUE_LABEL, IDC_MW_COL_SAT_LABEL, IDC_MW_COL_BRIGHT_LABEL };
  for (int i = 0; i < 5; i++) {
    HWND hSlider = GetDlgItem(m_hWnd, sliderIDs[i]);
    HWND hLabel = GetDlgItem(m_hWnd, labelIDs[i]);
    if (hSlider) {
      RECT r; GetWindowRect(hSlider, &r);
      MapWindowPoints(NULL, m_hWnd, (POINT*)&r, 2);
      MoveWindow(hSlider, r.left, r.top, newSliderW, r.bottom - r.top, TRUE);
      if (hLabel) MoveWindow(hLabel, r.left + newSliderW + 4, r.top, 50, r.bottom - r.top, TRUE);
    }
  }

  // Stretch Files tab ListBox and reposition buttons + description below it
  HWND hFileList = GetDlgItem(m_hWnd, IDC_MW_FILE_LIST);
  if (hFileList) {
    RECT r; GetWindowRect(hFileList, &r);
    MapWindowPoints(NULL, m_hWnd, (POINT*)&r, 2);
    int gap = 6;
    // buttons + desc + randtex + fallback tex combo + custom file edit + custom file buttons
    int reserveBelow = 4 + lineH + gap + lineH * 2 + gap + lineH + 2 + (lineH + 4) + 6 + lineH + gap + 4 + lineH + 2 + lineH + 6 + (lineH + 4) + 6 + lineH;
    int listBottom = rcDisplay.bottom - reserveBelow;
    if (listBottom < r.top + 40) listBottom = r.top + 40;
    MoveWindow(hFileList, r.left, r.top, rw, listBottom - r.top, TRUE);

    int btnY = listBottom + 4;
    HWND hAdd = GetDlgItem(m_hWnd, IDC_MW_FILE_ADD);
    HWND hRem = GetDlgItem(m_hWnd, IDC_MW_FILE_REMOVE);
    int fbw = MulDiv(70, lineH, 26), fbg = 4;
    if (hAdd) MoveWindow(hAdd, r.left, btnY, fbw, lineH, TRUE);
    if (hRem) MoveWindow(hRem, r.left + fbw + fbg, btnY, fbw, lineH, TRUE);

    HWND hDesc = GetDlgItem(m_hWnd, IDC_MW_FILE_DESC);
    if (hDesc) MoveWindow(hDesc, r.left, btnY + lineH + gap, rw, lineH * 2, TRUE);

    // Random textures directory controls - keep grouped tightly
    int randY = btnY + lineH + gap + lineH * 2 + gap;
    HWND hRandLabel = GetDlgItem(m_hWnd, IDC_MW_RANDTEX_LABEL);
    HWND hRandEdit = GetDlgItem(m_hWnd, IDC_MW_RANDTEX_EDIT);
    HWND hRandBrowse = GetDlgItem(m_hWnd, IDC_MW_RANDTEX_BROWSE);
    HWND hRandClear = GetDlgItem(m_hWnd, IDC_MW_RANDTEX_CLEAR);
    if (hRandLabel) MoveWindow(hRandLabel, r.left, randY, rw, lineH, TRUE);
    if (hRandEdit) MoveWindow(hRandEdit, r.left, randY + lineH + 2, rw, lineH + 4, TRUE);
    int rbw1 = MulDiv(80, lineH, 26), rbw2 = MulDiv(60, lineH, 26);
    if (hRandBrowse) MoveWindow(hRandBrowse, r.left, randY + lineH + 2 + lineH + 6, rbw1, lineH, TRUE);
    if (hRandClear) MoveWindow(hRandClear, r.left + rbw1 + 4, randY + lineH + 2 + lineH + 6, rbw2, lineH, TRUE);

    // Fallback texture style controls
    int fbTexY = randY + lineH + 2 + lineH + 6 + lineH + gap + 4;
    HWND hFbLabel = GetDlgItem(m_hWnd, IDC_MW_FALLBACK_TEX_LABEL);
    HWND hFbCombo = GetDlgItem(m_hWnd, IDC_MW_FALLBACK_TEX);
    if (hFbLabel) MoveWindow(hFbLabel, r.left, fbTexY, rw, lineH, TRUE);
    int comboW = MulDiv(260, lineH, 26);
    if (hFbCombo) MoveWindow(hFbCombo, r.left, fbTexY + lineH + 2, comboW, lineH + 8 * lineH, TRUE);

    // Custom fallback file controls (below combo)
    int cfY = fbTexY + lineH + 2 + lineH + 6;
    HWND hCfEdit = GetDlgItem(m_hWnd, IDC_MW_FALLBACK_FILE_EDIT);
    if (hCfEdit) MoveWindow(hCfEdit, r.left, cfY, rw, lineH + 4, TRUE);
    int cfBtnY = cfY + lineH + 6;
    HWND hCfBrowse = GetDlgItem(m_hWnd, IDC_MW_FALLBACK_FILE_BROWSE);
    HWND hCfClear = GetDlgItem(m_hWnd, IDC_MW_FALLBACK_FILE_CLEAR);
    if (hCfBrowse) MoveWindow(hCfBrowse, r.left, cfBtnY, rbw1, lineH, TRUE);
    if (hCfClear) MoveWindow(hCfClear, r.left + rbw1 + 4, cfBtnY, rbw2, lineH, TRUE);
  }

  // Stretch Remote tab edit fields and list box to fill width
  {
    int lw = MulDiv(160, lineH, 26);
    auto moveCtrl = [&](int id, int cx, int cy, int cw, int ch) {
      HWND h = GetDlgItem(m_hWnd, id);
      if (!h) return;
      RECT r; GetWindowRect(h, &r);
      MapWindowPoints(NULL, m_hWnd, (POINT*)&r, 2);
      if (cx < 0) cx = r.left;
      if (cy < 0) cy = r.top;
      if (cw < 0) cw = r.right - r.left;
      if (ch < 0) ch = r.bottom - r.top;
      MoveWindow(h, cx, cy, cw, ch, TRUE);
    };
    int editX = rcDisplay.left + 16 + lw + 4;
    int editW = rw - lw - 4;
    moveCtrl(IDC_MW_IPC_TITLE, editX, -1, editW, -1);
    moveCtrl(IDC_MW_IPC_REMOTE_TITLE, editX, -1, editW, -1);
    moveCtrl(IDC_MW_IPC_LIST, rcDisplay.left + 16, -1, rw, -1);
    moveCtrl(IDC_MW_IPC_MSG_GROUP, rcDisplay.left + 16, -1, rw, -1);
    // Stretch message text inside group box
    HWND hGroup = GetDlgItem(m_hWnd, IDC_MW_IPC_MSG_GROUP);
    if (hGroup) {
      RECT rg; GetWindowRect(hGroup, &rg);
      MapWindowPoints(NULL, m_hWnd, (POINT*)&rg, 2);
      int pad = 8;
      moveCtrl(IDC_MW_IPC_MSG_TEXT, rg.left + pad, rg.top + lineH + 2,
               rg.right - rg.left - pad * 2, rg.bottom - rg.top - lineH - pad - 2);
    }
  }

  InvalidateRect(m_hWnd, NULL, TRUE);
}

// ====== User Defaults & Fallback Paths ======

void SettingsWindow::UpdateVisualUI() {
  Engine* p = m_pEngine;
  HWND hWnd = m_hWnd;
  wchar_t buf[32];
  SendMessage(GetDlgItem(hWnd, IDC_MW_OPACITY), TBM_SETPOS, TRUE, (int)(p->fOpacity * 100));
  swprintf(buf, 32, L"%d%%", (int)(p->fOpacity * 100));
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_OPACITY_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_RENDER_QUALITY), TBM_SETPOS, TRUE, (int)(p->m_fRenderQuality * 100));
  swprintf(buf, 32, L"%.2f", p->m_fRenderQuality);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_QUALITY_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_QUALITY_AUTO), BM_SETCHECK, p->bQualityAuto ? BST_CHECKED : BST_UNCHECKED, 0);
  swprintf(buf, 32, L"%.2f", p->m_timeFactor);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_TIME_FACTOR), buf);
  swprintf(buf, 32, L"%.2f", p->m_frameFactor);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_FRAME_FACTOR), buf);
  swprintf(buf, 32, L"%.2f", p->m_fpsFactor);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_FPS_FACTOR), buf);
  swprintf(buf, 32, L"%.2f", p->m_VisIntensity);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_INTENSITY), buf);
  swprintf(buf, 32, L"%.2f", p->m_VisShift);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_SHIFT), buf);
  swprintf(buf, 32, L"%.0f", p->m_VisVersion);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_VIS_VERSION), buf);
  // GPU Protection controls
  swprintf(buf, 32, L"%d", p->m_nMaxShapeInstances);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_MAX_INST), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BY_RES), BM_SETCHECK, p->m_bScaleInstancesByResolution ? BST_CHECKED : BST_UNCHECKED, 0);
  swprintf(buf, 32, L"%d", p->m_nInstanceScaleBaseWidth);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_SCALE_BASE), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_GPU_SKIP_HEAVY), BM_SETCHECK, p->m_bSkipHeavyPresets ? BST_CHECKED : BST_UNCHECKED, 0);
  swprintf(buf, 32, L"%d", p->m_nHeavyPresetMaxInstances);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_GPU_HEAVY_THRESHOLD), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_VSYNC_ENABLED), BM_SETCHECK, p->m_bEnableVSync ? BST_CHECKED : BST_UNCHECKED, 0);
  HWND hw = p->GetPluginWindow();
  if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
  if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
}

void SettingsWindow::UpdateColorsUI() {
  Engine* p = m_pEngine;
  HWND hWnd = m_hWnd;
  wchar_t buf[32];
  SendMessage(GetDlgItem(hWnd, IDC_MW_COL_HUE), TBM_SETPOS, TRUE, (int)(p->m_ColShiftHue * 100) + 100);
  swprintf(buf, 32, L"%.2f", p->m_ColShiftHue);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_HUE_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_COL_SAT), TBM_SETPOS, TRUE, (int)(p->m_ColShiftSaturation * 100) + 100);
  swprintf(buf, 32, L"%.2f", p->m_ColShiftSaturation);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_SAT_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT), TBM_SETPOS, TRUE, (int)(p->m_ColShiftBrightness * 100) + 100);
  swprintf(buf, 32, L"%.2f", p->m_ColShiftBrightness);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_BRIGHT_LABEL), buf);
  float gamma = p->m_pState ? p->m_pState->m_fGammaAdj.eval(-1) : 2.0f;
  SendMessage(GetDlgItem(hWnd, IDC_MW_COL_GAMMA), TBM_SETPOS, TRUE, (int)(gamma * 10));
  swprintf(buf, 32, L"%.1f", gamma);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_COL_GAMMA_LABEL), buf);
  SendMessage(GetDlgItem(hWnd, IDC_MW_AUTO_HUE), BM_SETCHECK, p->m_AutoHue ? BST_CHECKED : BST_UNCHECKED, 0);
  swprintf(buf, 32, L"%.3f", p->m_AutoHueSeconds);
  SetWindowTextW(GetDlgItem(hWnd, IDC_MW_AUTO_HUE_SEC), buf);
}

void SettingsWindow::ResetToFactory() {
  Engine* p = m_pEngine;
  // Visual defaults
  p->fOpacity = 1.0f;
  p->m_fRenderQuality = 1.0f;
  p->bQualityAuto = false;
  p->m_timeFactor = 1.0f;
  p->m_frameFactor = 1.0f;
  p->m_fpsFactor = 1.0f;
  p->m_VisIntensity = 1.0f;
  p->m_VisShift = 0.0f;
  p->m_VisVersion = 1.0f;
  // Color defaults
  p->m_ColShiftHue = 0.0f;
  p->m_ColShiftSaturation = 0.0f;
  p->m_ColShiftBrightness = 0.0f;
  if (p->m_pState) p->m_pState->m_fGammaAdj = 2.0f;
  p->m_AutoHue = false;
  p->m_AutoHueSeconds = 0.02f;
  // Update UI
  UpdateVisualUI();
  UpdateColorsUI();
}

void SettingsWindow::ResetToUserDefaults() {
  Engine* p = m_pEngine;
  if (!p->m_bUserDefaultsSaved) {
    ResetToFactory();
    return;
  }
  // Visual
  p->fOpacity = p->m_udOpacity;
  p->m_fRenderQuality = p->m_udRenderQuality;
  p->bQualityAuto = false;
  p->m_timeFactor = p->m_udTimeFactor;
  p->m_frameFactor = p->m_udFrameFactor;
  p->m_fpsFactor = p->m_udFpsFactor;
  p->m_VisIntensity = p->m_udVisIntensity;
  p->m_VisShift = p->m_udVisShift;
  p->m_VisVersion = p->m_udVisVersion;
  // Colors
  p->m_ColShiftHue = p->m_udHue;
  p->m_ColShiftSaturation = p->m_udSaturation;
  p->m_ColShiftBrightness = p->m_udBrightness;
  if (p->m_pState) p->m_pState->m_fGammaAdj = p->m_udGamma;
  p->m_AutoHue = false;
  p->m_AutoHueSeconds = 0.02f;
  // Update UI
  UpdateVisualUI();
  UpdateColorsUI();
}

// ====== Settings Fullscreen Awareness ======

struct EnumMonitorData {
  HMONITOR hExclude;   // monitor to skip (the one render is on)
  RECT     rcResult;   // work area of first alternate monitor found
  bool     bFound;
};

static BOOL CALLBACK FindAltMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
  EnumMonitorData* d = (EnumMonitorData*)lp;
  if (hMon == d->hExclude) return TRUE; // skip render monitor
  MONITORINFO mi = { sizeof(mi) };
  if (GetMonitorInfo(hMon, &mi)) {
    d->rcResult = mi.rcWork;
    d->bFound = true;
    return FALSE; // stop enumerating
  }
  return TRUE;
}

void SettingsWindow::EnsureVisible() {
  if (!m_hWnd || !IsWindow(m_hWnd) || !IsWindowVisible(m_hWnd))
    return;

  HWND hRender = m_pEngine->GetPluginWindow();
  if (!hRender) return;

  HMONITOR hRenderMon = MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST);
  HMONITOR hSettingsMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);

  // Only act if both windows are on the same monitor AND render is fullscreen
  if (hRenderMon != hSettingsMon || !m_pEngine->IsBorderlessFullscreen(hRender)) {
    SetForegroundWindow(m_hWnd);
    return;
  }

  // Try to find an alternate monitor
  EnumMonitorData emd = {};
  emd.hExclude = hRenderMon;
  emd.bFound = false;
  EnumDisplayMonitors(NULL, NULL, FindAltMonitorProc, (LPARAM)&emd);

  if (emd.bFound) {
    // Move settings window to center of the alternate monitor's work area
    int monW = emd.rcResult.right - emd.rcResult.left;
    int monH = emd.rcResult.bottom - emd.rcResult.top;
    int wx = emd.rcResult.left + (monW - m_nWndW) / 2;
    int wy = emd.rcResult.top + (monH - m_nWndH) / 2;
    SetWindowPos(m_hWnd, HWND_TOPMOST, wx, wy, m_nWndW, m_nWndH, SWP_SHOWWINDOW);
  } else {
    // Single monitor — just bring to foreground
    SetForegroundWindow(m_hWnd);
  }
}

// ====== Resource Viewer ======

static bool g_bResourceViewerClassRegistered = false;

void Engine::OpenResourceViewer() {
  if (m_hResourceWnd && IsWindow(m_hResourceWnd)) {
    ShowWindow(m_hResourceWnd, SW_SHOW);
    SetForegroundWindow(m_hResourceWnd);
    PopulateResourceViewer();
    return;
  }

  if (!g_bResourceViewerClassRegistered) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ResourceViewerWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = IsDarkTheme() ? CreateSolidBrush(m_colSettingsBg) : (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = L"MDropDX12ResourceViewer";
    RegisterClassExW(&wc);
    g_bResourceViewerClassRegistered = true;
  }

  m_hResourceWnd = CreateWindowExW(
    WS_EX_TOOLWINDOW,
    L"MDropDX12ResourceViewer",
    L"Resource Viewer",
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
    CW_USEDEFAULT, CW_USEDEFAULT, 750, 420,
    m_settingsWindow ? m_settingsWindow->GetHWND() : NULL,
    NULL,
    GetModuleHandle(NULL),
    NULL);

  SetWindowLongPtrW(m_hResourceWnd, GWLP_USERDATA, (LONG_PTR)this);

  m_hResourceList = CreateWindowExW(
    0,
    WC_LISTVIEWW,
    L"",
    WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
    0, 0, 100, 100,
    m_hResourceWnd,
    (HMENU)(INT_PTR)IDC_RV_LISTVIEW,
    GetModuleHandle(NULL),
    NULL);

  ListView_SetExtendedListViewStyle(m_hResourceList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

  // Add columns
  LVCOLUMNW col = {};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
  col.fmt = LVCFMT_CENTER;
  col.cx = 32;
  col.pszText = (LPWSTR)L"";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);

  col.fmt = LVCFMT_LEFT;
  col.cx = 90;
  col.pszText = (LPWSTR)L"Type";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

  col.cx = 150;
  col.pszText = (LPWSTR)L"Name";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

  col.cx = 320;
  col.pszText = (LPWSTR)L"Path";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

  col.cx = 90;
  col.pszText = (LPWSTR)L"Details";
  SendMessageW(m_hResourceList, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);

  // Create buttons (owner-draw for dark theme painting)
  CreateWindowExW(0, L"BUTTON", L"\u2702", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
    0, 0, 36, 28, m_hResourceWnd, (HMENU)(INT_PTR)IDC_RV_COPY_PATH, GetModuleHandle(NULL), NULL);
  CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
    0, 0, 70, 28, m_hResourceWnd, (HMENU)(INT_PTR)IDC_RV_REFRESH, GetModuleHandle(NULL), NULL);

  // Set font on ListView and buttons
  HFONT hSettingsFont = m_settingsWindow ? m_settingsWindow->GetFont() : NULL;
  if (hSettingsFont) {
    SendMessage(m_hResourceList, WM_SETFONT, (WPARAM)hSettingsFont, TRUE);
    SendMessage(GetDlgItem(m_hResourceWnd, IDC_RV_COPY_PATH), WM_SETFONT, (WPARAM)hSettingsFont, TRUE);
    SendMessage(GetDlgItem(m_hResourceWnd, IDC_RV_REFRESH), WM_SETFONT, (WPARAM)hSettingsFont, TRUE);
  }

  // Apply dark theme to resource viewer
  mdrop::ApplyDarkThemeToWindow(this, m_hResourceWnd);
  {
    std::vector<HWND> ctrls;
    ctrls.push_back(m_hResourceList);
    HWND hCopy = GetDlgItem(m_hResourceWnd, IDC_RV_COPY_PATH);
    HWND hRefresh = GetDlgItem(m_hResourceWnd, IDC_RV_REFRESH);
    if (hCopy) ctrls.push_back(hCopy);
    if (hRefresh) ctrls.push_back(hRefresh);
    mdrop::ApplyDarkThemeToChildren(this, ctrls);
  }
  if (IsDarkTheme()) {
    ListView_SetBkColor(m_hResourceList, m_colSettingsBg);
    ListView_SetTextBkColor(m_hResourceList, m_colSettingsBg);
    ListView_SetTextColor(m_hResourceList, m_colSettingsText);
  }

  LayoutResourceViewer();
  PopulateResourceViewer();
}

LRESULT CALLBACK Engine::ResourceViewerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  Engine* p = (Engine*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

  switch (uMsg) {
  case WM_CLOSE:
    ShowWindow(hWnd, SW_HIDE);
    return 0;

  case WM_SIZE:
    if (p) p->LayoutResourceViewer();
    return 0;

  case WM_GETMINMAXINFO: {
    MINMAXINFO* mmi = (MINMAXINFO*)lParam;
    mmi->ptMinTrackSize.x = 500;
    mmi->ptMinTrackSize.y = 250;
    return 0;
  }

  case WM_NOTIFY: {
    NMHDR* pnm = (NMHDR*)lParam;
    // Custom-draw the ListView header (column headers) for dark theme
    if (p && p->IsDarkTheme() && pnm->code == NM_CUSTOMDRAW) {
      bool handled = false;
      LRESULT result = PaintDarkListViewHeader(pnm, lParam, p->m_hResourceList,
        p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText, &handled);
      if (handled) return result;
    }
    break;
  }

  case WM_ERASEBKGND:
    if (p)
      return mdrop::HandleDarkEraseBkgnd(p, hWnd, (HDC)wParam);
    break;

  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORDLG:
    if (p) {
      LRESULT lr = mdrop::HandleDarkCtlColor(p, uMsg, wParam, lParam);
      if (lr) return lr;
    }
    break;

  case WM_DRAWITEM:
    if (p) {
      DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
      LRESULT lr = mdrop::HandleDarkDrawItem(p, pDIS);
      if (lr) return lr;
    }
    break;

  case WM_COMMAND: {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);
    if (id == IDC_RV_COPY_PATH && code == BN_CLICKED && p && p->m_hResourceList) {
      int sel = ListView_GetNextItem(p->m_hResourceList, -1, LVNI_SELECTED);
      if (sel >= 0) {
        wchar_t szPath[1024] = {};
        LVITEMW item = {};
        item.iItem = sel;
        item.iSubItem = 3;  // Path column
        item.mask = LVIF_TEXT;
        item.pszText = szPath;
        item.cchTextMax = 1024;
        SendMessageW(p->m_hResourceList, LVM_GETITEMTEXTW, sel, (LPARAM)&item);

        // For procedural resources, copy Name + Type + Details instead of path
        wchar_t szClip[2048] = {};
        if (!wcscmp(szPath, L"(procedural)") || !wcscmp(szPath, L"(render target)")) {
          wchar_t szType[128] = {}, szName[256] = {}, szDetails[128] = {};
          item.iSubItem = 1; item.pszText = szType; item.cchTextMax = 128;
          SendMessageW(p->m_hResourceList, LVM_GETITEMTEXTW, sel, (LPARAM)&item);
          item.iSubItem = 2; item.pszText = szName; item.cchTextMax = 256;
          SendMessageW(p->m_hResourceList, LVM_GETITEMTEXTW, sel, (LPARAM)&item);
          item.iSubItem = 4; item.pszText = szDetails; item.cchTextMax = 128;
          SendMessageW(p->m_hResourceList, LVM_GETITEMTEXTW, sel, (LPARAM)&item);
          swprintf(szClip, 2048, L"%s\t%s\t%s", szName, szType, szDetails);
        } else {
          lstrcpyW(szClip, szPath);
        }

        if (szClip[0] && OpenClipboard(hWnd)) {
          EmptyClipboard();
          size_t len = (wcslen(szClip) + 1) * sizeof(wchar_t);
          HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
          if (hMem) {
            memcpy(GlobalLock(hMem), szClip, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
          }
          CloseClipboard();
        }
      }
      return 0;
    }
    if (id == IDC_RV_REFRESH && code == BN_CLICKED && p) {
      p->PopulateResourceViewer();
      return 0;
    }
    break;
  }
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void Engine::LayoutResourceViewer() {
  if (!m_hResourceWnd || !m_hResourceList) return;
  RECT rc;
  GetClientRect(m_hResourceWnd, &rc);
  int btnH = 28;
  int margin = 6;
  int listBottom = rc.bottom - btnH - margin * 2;

  MoveWindow(m_hResourceList, 0, 0, rc.right, listBottom, TRUE);

  HWND hCopy = GetDlgItem(m_hResourceWnd, IDC_RV_COPY_PATH);
  HWND hRefresh = GetDlgItem(m_hResourceWnd, IDC_RV_REFRESH);
  if (hCopy) MoveWindow(hCopy, rc.right - 36 - margin - 70 - margin, listBottom + margin, 36, btnH, TRUE);
  if (hRefresh) MoveWindow(hRefresh, rc.right - 70 - margin, listBottom + margin, 70, btnH, TRUE);
}

// Sort callback for resource viewer: failed items first, then by type, name, path
static int CALLBACK RV_SortCompare(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
  HWND hList = (HWND)lParamSort;
  wchar_t buf1[512], buf2[512];
  LVITEMW item = {};
  item.mask = LVIF_TEXT;
  item.cchTextMax = 512;

  // Compare status column (col 0): ✗ before ✓
  item.iSubItem = 0;
  item.pszText = buf1; item.iItem = (int)lParam1;
  SendMessageW(hList, LVM_GETITEMTEXTW, lParam1, (LPARAM)&item);
  item.pszText = buf2; item.iItem = (int)lParam2;
  SendMessageW(hList, LVM_GETITEMTEXTW, lParam2, (LPARAM)&item);
  bool fail1 = (buf1[0] == L'\u2717'), fail2 = (buf2[0] == L'\u2717');
  if (fail1 != fail2) return fail1 ? -1 : 1;

  // Compare type (col 1), then name (col 2), then path (col 3)
  for (int col = 1; col <= 3; col++) {
    item.iSubItem = col;
    item.pszText = buf1; item.iItem = (int)lParam1;
    SendMessageW(hList, LVM_GETITEMTEXTW, lParam1, (LPARAM)&item);
    item.pszText = buf2; item.iItem = (int)lParam2;
    SendMessageW(hList, LVM_GETITEMTEXTW, lParam2, (LPARAM)&item);
    int cmp = _wcsicmp(buf1, buf2);
    if (cmp != 0) return cmp;
  }
  return 0;
}

static void RV_AddRow(HWND hList, int idx, const wchar_t* status, const wchar_t* type,
                      const wchar_t* name, const wchar_t* path, const wchar_t* details) {
  LVITEMW item = {};
  item.mask = LVIF_TEXT;
  item.iItem = idx;
  item.iSubItem = 0;
  item.pszText = (LPWSTR)status;
  SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&item);

  item.iSubItem = 1;
  item.pszText = (LPWSTR)type;
  SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&item);

  item.iSubItem = 2;
  item.pszText = (LPWSTR)name;
  SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&item);

  item.iSubItem = 3;
  item.pszText = (LPWSTR)path;
  SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&item);

  item.iSubItem = 4;
  item.pszText = (LPWSTR)details;
  SendMessageW(hList, LVM_SETITEMTEXTW, idx, (LPARAM)&item);
}

void Engine::PopulateResourceViewer() {
  if (!m_hResourceList) return;

  SendMessageW(m_hResourceList, LVM_DELETEALLITEMS, 0, 0);
  int row = 0;
  wchar_t szDetails[128];

  // 1. Render Targets
  for (int i = 0; i < 2; i++) {
    wchar_t szName[32];
    swprintf(szName, 32, L"VS[%d]", i);
    bool valid = m_dx12VS[i].IsValid();
    swprintf(szDetails, 128, L"%dx%d", m_dx12VS[i].width, m_dx12VS[i].height);
    RV_AddRow(m_hResourceList, row++, valid ? L"\u2713" : L"\u2717",
              L"Render Target", szName, L"(render target)", valid ? szDetails : L"");
  }

  // 2. Noise Textures (non-evictable)
  for (int i = 0; i < (int)m_textures.size(); i++) {
    if (m_textures[i].bEvictable) continue;
    bool valid = (m_textures[i].dx12Tex.srvIndex != UINT_MAX) || (m_textures[i].texptr != NULL);
    if (m_textures[i].d > 1)
      swprintf(szDetails, 128, L"%dx%dx%d", m_textures[i].w, m_textures[i].h, m_textures[i].d);
    else
      swprintf(szDetails, 128, L"%dx%d", m_textures[i].w, m_textures[i].h);
    RV_AddRow(m_hResourceList, row++, valid ? L"\u2713" : L"\u2717",
              L"Noise", m_textures[i].texname, L"(procedural)", valid ? szDetails : L"");
  }

  // 3. Blur Targets
  for (int i = 0; i < NUM_BLUR_TEX; i++) {
    wchar_t szName[32];
    swprintf(szName, 32, L"blur[%d]", i);
    bool valid = m_dx12Blur[i].IsValid();
    swprintf(szDetails, 128, L"%dx%d", m_nBlurTexW[i], m_nBlurTexH[i]);
    RV_AddRow(m_hResourceList, row++, valid ? L"\u2713" : L"\u2717",
              L"Blur Target", szName, L"(render target)", valid ? szDetails : L"");
  }

  // 4. Shaders
  {
    bool warpHasCode = m_pState && m_pState->m_nWarpPSVersion > 0 && m_pState->m_szWarpShadersText[0] != 0;
    bool warpOk = m_shaders.warp.bytecodeBlob != NULL;
    wchar_t szVer[32] = L"";
    if (m_pState) swprintf(szVer, 32, L"ps_%d_0", m_pState->m_nWarpPSVersion);
    RV_AddRow(m_hResourceList, row++, warpOk ? L"\u2713" : L"\u2717",
              L"Warp Shader", L"warp", warpHasCode ? L"(custom)" : L"(default)", szVer);

    bool compHasCode = m_pState && m_pState->m_nCompPSVersion > 0 && m_pState->m_szCompShadersText[0] != 0;
    bool compOk = m_shaders.comp.bytecodeBlob != NULL;
    szVer[0] = 0;
    if (m_pState) swprintf(szVer, 32, L"ps_%d_0", m_pState->m_nCompPSVersion);
    RV_AddRow(m_hResourceList, row++, compOk ? L"\u2713" : L"\u2717",
              L"Comp Shader", L"comp", compHasCode ? L"(custom)" : L"(default)", szVer);
  }

  // 5. Custom Textures — reflect sampler names from both warp and comp shader CTs
  {
    std::set<std::wstring> addedNames;  // deduplicate across warp/comp
    LPD3DXCONSTANTTABLE CTs[2] = { m_shaders.warp.CT, m_shaders.comp.CT };
    const wchar_t* shaderLabel[2] = { L"warp", L"comp" };

    for (int s = 0; s < 2; s++) {
      LPD3DXCONSTANTTABLE pCT = CTs[s];
      if (!pCT) continue;

      D3DXCONSTANTTABLE_DESC desc;
      pCT->GetDesc(&desc);

      for (UINT ci = 0; ci < desc.Constants; ci++) {
        D3DXHANDLE h = pCT->GetConstant(NULL, ci);
        D3DXCONSTANT_DESC cd;
        unsigned int count = 1;
        pCT->GetConstantDesc(h, &cd, &count);

        if (cd.RegisterSet != D3DXRS_SAMPLER) continue;

        // Get sampler name and strip "sampler_" prefix
        wchar_t szSamplerName[MAX_PATH];
        lstrcpyW(szSamplerName, AutoWide(cd.Name));

        wchar_t szRootName[MAX_PATH];
        if (!strncmp(cd.Name, "sampler_", 8))
          lstrcpyW(szRootName, AutoWide(&cd.Name[8]));
        else
          lstrcpyW(szRootName, AutoWide(cd.Name));

        // Strip XY_ filter/wrap prefix
        if (lstrlenW(szRootName) > 3 && szRootName[2] == L'_') {
          wchar_t c0 = szRootName[0], c1 = szRootName[1];
          if (c0 >= L'a' && c0 <= L'z') c0 -= L'a' - L'A';
          if (c1 >= L'a' && c1 <= L'z') c1 -= L'a' - L'A';
          bool isPrefix = (c0 == L'F' || c0 == L'P' || c0 == L'W' || c0 == L'C') &&
                          (c1 == L'F' || c1 == L'P' || c1 == L'W' || c1 == L'C');
          if (isPrefix) {
            int j = 0;
            while (szRootName[j + 3]) { szRootName[j] = szRootName[j + 3]; j++; }
            szRootName[j] = 0;
          }
        }

        // Skip built-in resources
        if (!wcscmp(szRootName, L"main")) continue;
        if (!wcscmp(szRootName, L"blur1") || !wcscmp(szRootName, L"blur2") || !wcscmp(szRootName, L"blur3")) continue;
        if (!wcscmp(szRootName, L"blur4") || !wcscmp(szRootName, L"blur5") || !wcscmp(szRootName, L"blur6")) continue;
        if (!wcsncmp(szRootName, L"noise_", 6) || !wcsncmp(szRootName, L"noisevol_", 9)) continue;

        // Deduplicate
        std::wstring key(szRootName);
        if (addedNames.count(key)) continue;
        addedNames.insert(key);

        // Look up in m_textures by name
        bool found = false;
        int texIdx = -1;
        for (int t = 0; t < (int)m_textures.size(); t++) {
          if (!wcscmp(m_textures[t].texname, szRootName)) {
            found = true;
            texIdx = t;
            break;
          }
        }
        // If not found by name (e.g. rand## textures get resolved to a different name),
        // check the actual shader binding to see if a texture was loaded for this slot.
        if (!found && cd.RegisterIndex < 32) {
          CShaderParams& sp = (s == 0) ? m_shaders.warp.params : m_shaders.comp.params;
          UINT srvIdx = sp.m_texture_bindings[cd.RegisterIndex].dx12SrvIndex;
          if (srvIdx != UINT_MAX) {
            for (int t = 0; t < (int)m_textures.size(); t++) {
              if (m_textures[t].dx12Tex.srvIndex == srvIdx) {
                found = true;
                texIdx = t;
                break;
              }
            }
          }
        }

        // For rand textures resolved via SRV index, use the actual loaded texture name
        const wchar_t* szLookupName = (texIdx >= 0) ? m_textures[texIdx].texname : szRootName;

        // Build full file path by searching the same way CacheParams does
        wchar_t szFullPath[MAX_PATH * 2] = {};
        {
          bool pathFound = false;
          for (int z = 0; z < 8; z++) {  // 8 extensions in texture_exts
            wchar_t szTry[MAX_PATH];
            swprintf(szTry, MAX_PATH, L"%stextures\\%s.%s", m_szMilkdrop2Path, szLookupName, texture_exts[z].c_str());
            if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
              lstrcpyW(szFullPath, szTry);
              pathFound = true;
              break;
            }
            swprintf(szTry, MAX_PATH, L"%s%s.%s", m_szPresetDir, szLookupName, texture_exts[z].c_str());
            if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
              lstrcpyW(szFullPath, szTry);
              pathFound = true;
              break;
            }
            // Search random textures directory
            if (m_szRandomTexDir[0]) {
              swprintf(szTry, MAX_PATH, L"%s%s.%s", m_szRandomTexDir, szLookupName, texture_exts[z].c_str());
              if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
                lstrcpyW(szFullPath, szTry);
                pathFound = true;
                break;
              }
            }
            // Search content base path
            if (m_szContentBasePath[0]) {
              swprintf(szTry, MAX_PATH, L"%s%s.%s", m_szContentBasePath, szLookupName, texture_exts[z].c_str());
              if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
                lstrcpyW(szFullPath, szTry);
                pathFound = true;
                break;
              }
            }
            // Search fallback paths
            for (auto& fbPath : m_fallbackPaths) {
              swprintf(szTry, MAX_PATH, L"%s%s.%s", fbPath.c_str(), szLookupName, texture_exts[z].c_str());
              if (GetFileAttributesW(szTry) != 0xFFFFFFFF) {
                lstrcpyW(szFullPath, szTry);
                pathFound = true;
                break;
              }
            }
            if (pathFound) break;
          }
          if (!pathFound) {
            // Show expected primary search path for missing textures
            swprintf(szFullPath, MAX_PATH * 2, L"%stextures\\%s", m_szMilkdrop2Path, szLookupName);
          }
        }

        if (found) {
          swprintf(szDetails, 128, L"%dx%d", m_textures[texIdx].w, m_textures[texIdx].h);
          RV_AddRow(m_hResourceList, row++, L"\u2713", L"Custom Tex", szSamplerName, szFullPath, szDetails);
        } else {
          RV_AddRow(m_hResourceList, row++, L"\u2717", L"Custom Tex", szSamplerName, szFullPath, L"(missing)");
        }
      }
    }
  }

  // Sort: failed items first, then by type, name, path
  ListView_SortItemsEx(m_hResourceList, RV_SortCompare, (LPARAM)m_hResourceList);
}

//----------------------------------------------------------------------
// Artist-Title Match Editor popup
//----------------------------------------------------------------------

static const wchar_t* WTP_WND_CLASS = L"MDropDX12WTParserWnd";
static bool g_bWTPWndClassRegistered = false;
static HWND g_hWTPWnd = NULL;

// Data context for the parser popup — holds a working copy of profiles
struct WTPContext {
  Engine* pEngine;
  HWND hParent;  // settings window
  std::vector<WindowTitleProfile> profiles;  // working copy
  int nActiveSel;
  HFONT hFont;
  HFONT hFontBold;
};

// Forward declarations
static LRESULT CALLBACK WTPWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void WTPPopulateProfileCombo(HWND hWnd, WTPContext* ctx);
static void WTPLoadProfileFields(HWND hWnd, WTPContext* ctx);
static void WTPUpdateMatchedWindow(HWND hWnd, WTPContext* ctx);
static void WTPUpdateParsedResult(HWND hWnd, WTPContext* ctx, const std::wstring& matchedTitle);

void Engine::OpenWindowTitleParserPopup(HWND hParent) {
  // If already open, bring to front
  if (g_hWTPWnd && IsWindow(g_hWTPWnd)) {
    SetForegroundWindow(g_hWTPWnd);
    return;
  }

  // Register window class
  if (!g_bWTPWndClassRegistered) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WTPWndProc;
    wc.cbWndExtra = sizeof(LONG_PTR);  // store context pointer
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = WTP_WND_CLASS;
    if (RegisterClassExW(&wc)) g_bWTPWndClassRegistered = true;
    else return;
  }

  // Create context
  auto* ctx = new WTPContext();
  ctx->pEngine = this;
  ctx->hParent = hParent;
  ctx->profiles = m_windowTitleProfiles; // copy
  ctx->nActiveSel = m_nActiveWindowTitleProfile;
  if (ctx->nActiveSel < 0 || ctx->nActiveSel >= (int)ctx->profiles.size())
    ctx->nActiveSel = 0;
  ctx->hFont = m_settingsWindow ? m_settingsWindow->GetFont() : NULL;
  ctx->hFontBold = m_settingsWindow ? m_settingsWindow->GetFontBold() : NULL;

  // Create window — resizable
  int dpi = 96;
  { HDC hdc = GetDC(NULL); if (hdc) { dpi = GetDeviceCaps(hdc, LOGPIXELSX); ReleaseDC(NULL, hdc); } }
  int w = MulDiv(520, dpi, 96);
  int h = MulDiv(440, dpi, 96);
  RECT rcParent;
  GetWindowRect(hParent, &rcParent);
  int cx = rcParent.left + (rcParent.right - rcParent.left - w) / 2;
  int cy = rcParent.top + (rcParent.bottom - rcParent.top - h) / 2;

  g_hWTPWnd = CreateWindowExW(
    WS_EX_DLGMODALFRAME,
    WTP_WND_CLASS, L"Artist-Title Match Editor",
    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
    cx, cy, w, h,
    hParent, NULL, GetModuleHandle(NULL), ctx);

  if (g_hWTPWnd) {
    ShowWindow(g_hWTPWnd, SW_SHOW);
    UpdateWindow(g_hWTPWnd);
  } else {
    delete ctx;
  }
}

static void WTPPopulateProfileCombo(HWND hWnd, WTPContext* ctx) {
  HWND hCombo = GetDlgItem(hWnd, IDC_MW_WTP_PROFILE);
  if (!hCombo) return;
  SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < (int)ctx->profiles.size(); i++) {
    const wchar_t* name = ctx->profiles[i].szName;
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)(name[0] ? name : L"(unnamed)"));
  }
  if (!ctx->profiles.empty())
    SendMessage(hCombo, CB_SETCURSEL, ctx->nActiveSel, 0);
}

static void WTPLoadProfileFields(HWND hWnd, WTPContext* ctx) {
  if (ctx->nActiveSel < 0 || ctx->nActiveSel >= (int)ctx->profiles.size()) return;
  const auto& p = ctx->profiles[ctx->nActiveSel];
  SetDlgItemTextW(hWnd, IDC_MW_WTP_NAME, p.szName);
  SetDlgItemTextW(hWnd, IDC_MW_WTP_WINDOW_REGEX, p.szWindowRegex);
  SetDlgItemTextW(hWnd, IDC_MW_WTP_PARSE_REGEX, p.szParseRegex);
  wchar_t intBuf[16];
  swprintf(intBuf, 16, L"%d", p.nPollIntervalSec);
  SetDlgItemTextW(hWnd, IDC_MW_WTP_INTERVAL, intBuf);
  WTPUpdateMatchedWindow(hWnd, ctx);
}

static void WTPUpdateMatchedWindow(HWND hWnd, WTPContext* ctx) {
  HWND hMatched = GetDlgItem(hWnd, IDC_MW_WTP_MATCHED);
  HWND hParsed = GetDlgItem(hWnd, IDC_MW_WTP_PARSED);
  if (!hMatched) return;

  if (ctx->nActiveSel < 0 || ctx->nActiveSel >= (int)ctx->profiles.size()) {
    SetWindowTextW(hMatched, L"(no profile)");
    if (hParsed) SetWindowTextW(hParsed, L"");
    return;
  }

  wchar_t regexBuf[256] = {};
  GetDlgItemTextW(hWnd, IDC_MW_WTP_WINDOW_REGEX, regexBuf, _countof(regexBuf));
  if (!regexBuf[0]) {
    SetWindowTextW(hMatched, L"(empty pattern)");
    if (hParsed) SetWindowTextW(hParsed, L"");
    return;
  }

  std::wregex compiledRegex;
  try {
    compiledRegex = std::wregex(regexBuf, std::regex_constants::ECMAScript | std::regex_constants::icase);
  } catch (const std::regex_error&) {
    SetWindowTextW(hMatched, L"(invalid regex)");
    if (hParsed) SetWindowTextW(hParsed, L"");
    return;
  }

  SettingsWTMatchContext matchCtx = { &compiledRegex, {} };
  EnumWindows(EnumWindowsSettingsMatch, reinterpret_cast<LPARAM>(&matchCtx));
  if (matchCtx.matchedTitle.empty()) {
    SetWindowTextW(hMatched, L"(no match)");
    if (hParsed) SetWindowTextW(hParsed, L"");
    return;
  }

  SetWindowTextW(hMatched, matchCtx.matchedTitle.c_str());
  WTPUpdateParsedResult(hWnd, ctx, matchCtx.matchedTitle);
}

static void WTPUpdateParsedResult(HWND hWnd, WTPContext* ctx, const std::wstring& matchedTitle) {
  HWND hParsed = GetDlgItem(hWnd, IDC_MW_WTP_PARSED);
  if (!hParsed) return;

  wchar_t parseBuf[512] = {};
  GetDlgItemTextW(hWnd, IDC_MW_WTP_PARSE_REGEX, parseBuf, _countof(parseBuf));
  if (!parseBuf[0]) {
    SetWindowTextW(hParsed, L"(no parse regex)");
    return;
  }

  std::wregex parseRegex;
  try {
    parseRegex = std::wregex(StripNamedGroups(parseBuf), std::regex_constants::ECMAScript);
  } catch (const std::regex_error&) {
    SetWindowTextW(hParsed, L"(invalid parse regex)");
    return;
  }

  std::wsmatch match;
  try {
    if (!std::regex_search(matchedTitle, match, parseRegex)) {
      SetWindowTextW(hParsed, L"(regex didn't match)");
      return;
    }
  } catch (...) {
    SetWindowTextW(hParsed, L"(regex error)");
    return;
  }

  // Build name→index map
  std::wstring parseStr(parseBuf);
  std::map<std::wstring, int> nameMap;
  int groupIdx = 0;
  for (size_t i = 0; i < parseStr.size(); i++) {
    if (parseStr[i] == L'\\') { i++; continue; }
    if (parseStr[i] == L'(') {
      if (i + 1 < parseStr.size() && parseStr[i + 1] == L'?') {
        if (i + 2 < parseStr.size() && (parseStr[i + 2] == L'<' || parseStr[i + 2] == L'\'')) {
          wchar_t delim = (parseStr[i + 2] == L'<') ? L'>' : L'\'';
          size_t nameStart = i + 3;
          size_t nameEnd = parseStr.find(delim, nameStart);
          if (nameEnd != std::wstring::npos) { groupIdx++; nameMap[parseStr.substr(nameStart, nameEnd - nameStart)] = groupIdx; }
        }
      } else { groupIdx++; }
    }
  }

  auto getGroup = [&](const wchar_t* name, int fallback) -> std::wstring {
    auto it = nameMap.find(name);
    int gi = (it != nameMap.end()) ? it->second : fallback;
    return (gi > 0 && gi < (int)match.size() && match[gi].matched) ? match[gi].str() : L"";
  };

  std::wstring artist = getGroup(L"artist", 1);
  std::wstring title = getGroup(L"title", 2);
  std::wstring album = getGroup(L"album", 3);
  std::wstring preview;
  if (!artist.empty()) preview += L"Artist: " + artist;
  if (!title.empty()) { if (!preview.empty()) preview += L"  |  "; preview += L"Title: " + title; }
  if (!album.empty()) { if (!preview.empty()) preview += L"  |  "; preview += L"Album: " + album; }
  if (preview.empty()) preview = L"(no named groups matched)";
  SetWindowTextW(hParsed, preview.c_str());
}

static LRESULT CALLBACK WTPWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  WTPContext* ctx = (WTPContext*)GetWindowLongPtr(hWnd, 0);

  switch (msg) {
  case WM_CREATE: {
    CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
    ctx = (WTPContext*)cs->lpCreateParams;
    SetWindowLongPtr(hWnd, 0, (LONG_PTR)ctx);

    HFONT hFont = ctx->hFont;
    HFONT hFontBold = ctx->hFontBold;

    // Measure line height from font
    HDC hdc = GetDC(hWnd);
    HFONT oldF = (HFONT)SelectObject(hdc, hFont);
    TEXTMETRIC tm; GetTextMetrics(hdc, &tm);
    int lineH = tm.tmHeight + tm.tmExternalLeading + 4;
    SelectObject(hdc, oldF);
    ReleaseDC(hWnd, hdc);

    RECT rc; GetClientRect(hWnd, &rc);
    int pad = MulDiv(10, lineH, 26);
    int x = pad, y = pad;
    int fullW = rc.right - 2 * pad;
    int lw = MulDiv(130, lineH, 26); // label width — wide enough for "Window Match:"
    int btnW = MulDiv(60, lineH, 26);
    int rw = fullW;

    // Profile row: combo + New + Delete
    {
      CreateLabel(hWnd, L"Profile:", x, y, lw, lineH, hFont);
      int comboW = fullW - lw - 4 - btnW * 2 - 8;
      HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
        x + lw + 4, y, comboW, lineH + 10 * lineH, hWnd,
        (HMENU)(INT_PTR)IDC_MW_WTP_PROFILE, GetModuleHandle(NULL), NULL);
      if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

      CreateBtn(hWnd, L"New", IDC_MW_WTP_NEW, x + lw + 4 + comboW + 4, y, btnW, lineH, hFont);
      CreateBtn(hWnd, L"Delete", IDC_MW_WTP_DELETE, x + lw + 4 + comboW + 4 + btnW + 4, y, btnW, lineH, hFont);
    }
    y += lineH + 6;

    // Profile Name
    CreateLabel(hWnd, L"Profile Name:", x, y, lw, lineH, hFont);
    CreateEdit(hWnd, L"", IDC_MW_WTP_NAME, x + lw + 4, y, rw - lw - 4, lineH, hFont);
    y += lineH + 4;

    // Enumerated windows dropdown (select a window to help build regex)
    CreateLabel(hWnd, L"Windows:", x, y, lw, lineH, hFont);
    {
      HWND hWinCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
        x + lw + 4, y, rw - lw - 4, lineH + 15 * lineH, hWnd,
        (HMENU)(INT_PTR)IDC_MW_WTP_WINDOWS, GetModuleHandle(NULL), NULL);
      if (hWinCombo && hFont) SendMessage(hWinCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
      // Populate with visible windows
      std::vector<std::wstring> titles;
      EnumWindows(EnumVisibleWindowTitlesProc, (LPARAM)&titles);
      std::sort(titles.begin(), titles.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
      });
      for (const auto& t : titles)
        SendMessageW(hWinCombo, CB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    y += lineH + 4;

    // Window Match regex
    CreateLabel(hWnd, L"Window Match:", x, y, lw, lineH, hFont);
    CreateEdit(hWnd, L"", IDC_MW_WTP_WINDOW_REGEX, x + lw + 4, y, rw - lw - 4, lineH, hFont);
    y += lineH + 4;

    // Matched Window (read-only)
    CreateLabel(hWnd, L"Matched:", x, y, lw, lineH, hFont);
    {
      HWND hStatic = CreateWindowExW(0, L"STATIC", L"(no match)",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        x + lw + 4, y, rw - lw - 4, lineH, hWnd,
        (HMENU)(INT_PTR)IDC_MW_WTP_MATCHED, GetModuleHandle(NULL), NULL);
      if (hStatic && hFont) SendMessage(hStatic, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    y += lineH + 6;

    // Parse Regex
    CreateLabel(hWnd, L"Parse Regex:", x, y, lw, lineH, hFont);
    CreateEdit(hWnd, L"", IDC_MW_WTP_PARSE_REGEX, x + lw + 4, y, rw - lw - 4, lineH, hFont);
    y += lineH + 4;

    // Parsed Result (read-only)
    CreateLabel(hWnd, L"Parsed:", x, y, lw, lineH, hFont);
    {
      HWND hStatic = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        x + lw + 4, y, rw - lw - 4, lineH, hWnd,
        (HMENU)(INT_PTR)IDC_MW_WTP_PARSED, GetModuleHandle(NULL), NULL);
      if (hStatic && hFont) SendMessage(hStatic, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    y += lineH + 6;

    // Poll Interval
    CreateLabel(hWnd, L"Poll Interval:", x, y, lw, lineH, hFont);
    {
      HWND hEdit = CreateEdit(hWnd, L"2", IDC_MW_WTP_INTERVAL, x + lw + 4, y, MulDiv(40, lineH, 26), lineH, hFont);
      (void)hEdit;
      CreateLabel(hWnd, L"seconds (1-10)", x + lw + 4 + MulDiv(44, lineH, 26), y, MulDiv(120, lineH, 26), lineH, hFont);
    }
    y += lineH + 10;

    // Bottom row: Regex Help + OK + Cancel
    {
      int helpW = MulDiv(85, lineH, 26);
      int okW = MulDiv(70, lineH, 26);
      int cancelW = MulDiv(70, lineH, 26);
      CreateBtn(hWnd, L"Regex Help", IDC_MW_WTP_HELP, x, y, helpW, lineH, hFont);
      CreateBtn(hWnd, L"OK", IDC_MW_WTP_OK, rw + pad - okW - cancelW - 4, y, okW, lineH, hFont);
      CreateBtn(hWnd, L"Cancel", IDC_MW_WTP_CANCEL, rw + pad - cancelW, y, cancelW, lineH, hFont);
    }

    // Apply dark theme
    mdrop::ApplyDarkThemeToWindow(ctx->pEngine, hWnd);
    {
      std::vector<HWND> ctrls;
      HWND hChild = GetWindow(hWnd, GW_CHILD);
      while (hChild) { ctrls.push_back(hChild); hChild = GetWindow(hChild, GW_HWNDNEXT); }
      mdrop::ApplyDarkThemeToChildren(ctx->pEngine, ctrls);
    }

    // Populate and load
    WTPPopulateProfileCombo(hWnd, ctx);
    WTPLoadProfileFields(hWnd, ctx);
    return 0;
  }

  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORDLG:
    if (ctx) {
      LRESULT lr = mdrop::HandleDarkCtlColor(ctx->pEngine, msg, wParam, lParam);
      if (lr) return lr;
    }
    break;

  case WM_ERASEBKGND:
    if (ctx)
      return mdrop::HandleDarkEraseBkgnd(ctx->pEngine, hWnd, (HDC)wParam);
    break;

  case WM_DRAWITEM: {
    DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
    if (ctx) {
      LRESULT lr = mdrop::HandleDarkDrawItem(ctx->pEngine, pDIS);
      if (lr) return lr;
    }
    break;
  }

  case WM_COMMAND: {
    if (!ctx) break;
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    // Profile combo: selection changed
    if (id == IDC_MW_WTP_PROFILE && code == CBN_SELCHANGE) {
      // Save current fields before switching
      if (ctx->nActiveSel >= 0 && ctx->nActiveSel < (int)ctx->profiles.size()) {
        auto& p = ctx->profiles[ctx->nActiveSel];
        GetDlgItemTextW(hWnd, IDC_MW_WTP_NAME, p.szName, _countof(p.szName));
        GetDlgItemTextW(hWnd, IDC_MW_WTP_WINDOW_REGEX, p.szWindowRegex, _countof(p.szWindowRegex));
        GetDlgItemTextW(hWnd, IDC_MW_WTP_PARSE_REGEX, p.szParseRegex, _countof(p.szParseRegex));
        wchar_t intBuf[16] = {};
        GetDlgItemTextW(hWnd, IDC_MW_WTP_INTERVAL, intBuf, _countof(intBuf));
        p.nPollIntervalSec = _wtoi(intBuf);
        if (p.nPollIntervalSec < 1) p.nPollIntervalSec = 1;
        if (p.nPollIntervalSec > 10) p.nPollIntervalSec = 10;
      }
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel < (int)ctx->profiles.size()) {
        ctx->nActiveSel = sel;
        WTPLoadProfileFields(hWnd, ctx);
      }
      return 0;
    }

    // New profile
    if (id == IDC_MW_WTP_NEW && code == BN_CLICKED) {
      // Save current before adding
      if (ctx->nActiveSel >= 0 && ctx->nActiveSel < (int)ctx->profiles.size()) {
        auto& p = ctx->profiles[ctx->nActiveSel];
        GetDlgItemTextW(hWnd, IDC_MW_WTP_NAME, p.szName, _countof(p.szName));
        GetDlgItemTextW(hWnd, IDC_MW_WTP_WINDOW_REGEX, p.szWindowRegex, _countof(p.szWindowRegex));
        GetDlgItemTextW(hWnd, IDC_MW_WTP_PARSE_REGEX, p.szParseRegex, _countof(p.szParseRegex));
        wchar_t intBuf[16] = {};
        GetDlgItemTextW(hWnd, IDC_MW_WTP_INTERVAL, intBuf, _countof(intBuf));
        p.nPollIntervalSec = _wtoi(intBuf);
        if (p.nPollIntervalSec < 1) p.nPollIntervalSec = 1;
        if (p.nPollIntervalSec > 10) p.nPollIntervalSec = 10;
      }
      WindowTitleProfile newP;
      wcscpy_s(newP.szName, L"New Profile");
      wcscpy_s(newP.szParseRegex, L"(?<artist>.+?) - (?<title>.+)");
      ctx->profiles.push_back(newP);
      ctx->nActiveSel = (int)ctx->profiles.size() - 1;
      WTPPopulateProfileCombo(hWnd, ctx);
      WTPLoadProfileFields(hWnd, ctx);
      return 0;
    }

    // Delete profile
    if (id == IDC_MW_WTP_DELETE && code == BN_CLICKED) {
      if (ctx->profiles.size() <= 1) {
        MessageBoxW(hWnd, L"Cannot delete the last profile.", L"Artist-Title Match Editor", MB_OK | MB_ICONWARNING);
        return 0;
      }
      if (ctx->nActiveSel >= 0 && ctx->nActiveSel < (int)ctx->profiles.size()) {
        ctx->profiles.erase(ctx->profiles.begin() + ctx->nActiveSel);
        if (ctx->nActiveSel >= (int)ctx->profiles.size())
          ctx->nActiveSel = (int)ctx->profiles.size() - 1;
        WTPPopulateProfileCombo(hWnd, ctx);
        WTPLoadProfileFields(hWnd, ctx);
      }
      return 0;
    }

    // Windows dropdown: user selected a window — set Window Match to .*escaped_title.*
    if (id == IDC_MW_WTP_WINDOWS && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0) {
        wchar_t selText[512] = {};
        SendMessageW((HWND)lParam, CB_GETLBTEXT, sel, (LPARAM)selText);
        // Build a regex that matches this window title: .*escaped_title.*
        std::wstring pattern = L".*";
        for (const wchar_t* c = selText; *c; ++c) {
          if (wcschr(L"\\^$.|?*+()[]{}", *c))
            pattern += L'\\';
          pattern += *c;
        }
        pattern += L".*";
        SetDlgItemTextW(hWnd, IDC_MW_WTP_WINDOW_REGEX, pattern.c_str());
        // EN_CHANGE will fire and update the matched window preview
      }
      return 0;
    }

    // Repopulate windows dropdown when it opens
    if (id == IDC_MW_WTP_WINDOWS && code == CBN_DROPDOWN) {
      HWND hCombo = (HWND)lParam;
      SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
      std::vector<std::wstring> titles;
      EnumWindows(EnumVisibleWindowTitlesProc, (LPARAM)&titles);
      std::sort(titles.begin(), titles.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
      });
      for (const auto& t : titles)
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)t.c_str());
      return 0;
    }

    // Window regex edit change — live update matched window
    if (id == IDC_MW_WTP_WINDOW_REGEX && code == EN_CHANGE) {
      WTPUpdateMatchedWindow(hWnd, ctx);
      return 0;
    }

    // Parse regex edit change — live update parsed result
    if (id == IDC_MW_WTP_PARSE_REGEX && code == EN_CHANGE) {
      // Get current matched title and re-parse
      wchar_t matchedBuf[512] = {};
      GetDlgItemTextW(hWnd, IDC_MW_WTP_MATCHED, matchedBuf, _countof(matchedBuf));
      std::wstring matched(matchedBuf);
      if (!matched.empty() && matched[0] != L'(')
        WTPUpdateParsedResult(hWnd, ctx, matched);
      return 0;
    }

    // Regex Help button
    if (id == IDC_MW_WTP_HELP && code == BN_CLICKED) {
      ShellExecuteW(NULL, L"open", L"https://developer.mozilla.org/en-US/docs/Web/JavaScript/Guide/Regular_expressions/Groups_and_backreferences#types", NULL, NULL, SW_SHOWNORMAL);
      return 0;
    }

    // OK button
    if (id == IDC_MW_WTP_OK && code == BN_CLICKED) {
      // Save current fields
      if (ctx->nActiveSel >= 0 && ctx->nActiveSel < (int)ctx->profiles.size()) {
        auto& p = ctx->profiles[ctx->nActiveSel];
        GetDlgItemTextW(hWnd, IDC_MW_WTP_NAME, p.szName, _countof(p.szName));
        GetDlgItemTextW(hWnd, IDC_MW_WTP_WINDOW_REGEX, p.szWindowRegex, _countof(p.szWindowRegex));
        GetDlgItemTextW(hWnd, IDC_MW_WTP_PARSE_REGEX, p.szParseRegex, _countof(p.szParseRegex));
        wchar_t intBuf[16] = {};
        GetDlgItemTextW(hWnd, IDC_MW_WTP_INTERVAL, intBuf, _countof(intBuf));
        p.nPollIntervalSec = _wtoi(intBuf);
        if (p.nPollIntervalSec < 1) p.nPollIntervalSec = 1;
        if (p.nPollIntervalSec > 10) p.nPollIntervalSec = 10;
      }
      // Apply to engine
      ctx->pEngine->m_windowTitleProfiles = ctx->profiles;
      ctx->pEngine->m_nActiveWindowTitleProfile = ctx->nActiveSel;
      // Save to INI
      ctx->pEngine->MyWriteConfig();
      // Update profile combo on General tab
      HWND hProfileCombo = GetDlgItem(ctx->hParent, IDC_MW_WT_PROFILE);
      if (hProfileCombo) {
        SendMessage(hProfileCombo, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < (int)ctx->profiles.size(); i++) {
          const wchar_t* name = ctx->profiles[i].szName;
          SendMessageW(hProfileCombo, CB_ADDSTRING, 0, (LPARAM)(name[0] ? name : L"(unnamed)"));
        }
        SendMessage(hProfileCombo, CB_SETCURSEL, ctx->nActiveSel, 0);
      }
      // Update preview on General tab
      if (!ctx->profiles.empty() && ctx->nActiveSel >= 0 && ctx->nActiveSel < (int)ctx->profiles.size()) {
        if (ctx->profiles[ctx->nActiveSel].szWindowRegex[0])
          UpdateWindowTitlePreview(ctx->hParent, ctx->profiles[ctx->nActiveSel].szWindowRegex);
      }
      DestroyWindow(hWnd);
      return 0;
    }

    // Cancel button
    if (id == IDC_MW_WTP_CANCEL && code == BN_CLICKED) {
      DestroyWindow(hWnd);
      return 0;
    }
    break;
  }

  case WM_DESTROY: {
    if (ctx) {
      delete ctx;
      SetWindowLongPtr(hWnd, 0, 0);
    }
    g_hWTPWnd = NULL;
    return 0;
  }

  case WM_CLOSE:
    DestroyWindow(hWnd);
    return 0;
  }

  return DefWindowProc(hWnd, msg, wParam, lParam);
}


} // namespace mdrop
