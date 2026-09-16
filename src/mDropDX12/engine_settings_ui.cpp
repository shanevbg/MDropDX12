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
#include "format_to.h"
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
#include "config_store.h"

namespace mdrop {

extern Engine g_engine;
extern int ToggleFPSNumPressed;

// SettingsWindow constructor (ToolWindow subclass)
SettingsWindow::SettingsWindow(Engine* pEngine) : ToolWindow(pEngine, 930, 880) {}

void Engine::OpenSettingsWindowAt(int page) {
  // Constructed before ShowTab so the page reaches the INI before the window is
  // built and SelectInitialTab reads it. A window that is already open is told
  // by a posted message instead.
  if (!m_settingsWindow) m_settingsWindow = std::make_unique<SettingsWindow>(this);
  m_settingsWindow->ShowTab(page);
  OpenSettingsWindow();
}

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
static void PopulateWindowTitleCombo(HWND hTitleCombo, const wchar_t* currentTitle) {
  wchar_t editText[256] = {};
  if (currentTitle && currentTitle[0])
    lstrcpynW(editText, currentTitle, _countof(editText));
  else
    GetWindowTextW(hTitleCombo, editText, _countof(editText));
  SendMessage(hTitleCombo, CB_RESETCONTENT, 0, 0);
  std::vector<std::wstring> titles;
  EnumWindows(EnumVisibleWindowTitlesProc, (LPARAM)&titles);
  std::sort(titles.begin(), titles.end(), [](const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  });
  for (const auto& t : titles)
    SendMessageW(hTitleCombo, CB_ADDSTRING, 0, (LPARAM)t.c_str());
  SetWindowTextW(hTitleCombo, editText);
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
    HWND hBlend = CreateCombo(m_hWnd, IDC_SPRIMP_BLEND,
      col1 + lblW, cy, MulDiv(120, lineH, 20), 200, hFont);
    const wchar_t* blendNames[] = { L"0: Blend", L"1: Decal", L"2: Additive", L"3: SrcColor", L"4: ColorKey" };
    for (int i = 0; i < 5; i++) SendMessageW(hBlend, CB_ADDSTRING, 0, (LPARAM)blendNames[i]);
    SendMessageW(hBlend, CB_SETCURSEL, nBlendMode, 0);
    TrackControl(hBlend);
    cy += lineH + gap;

    // Layer
    TrackControl(CreateLabel(m_hWnd, L"Layer:", col1, cy, lblW, lineH, hFont));
    HWND hLayer = CreateCombo(m_hWnd, IDC_SPRIMP_LAYER,
      col1 + lblW, cy, MulDiv(140, lineH, 20), 200, hFont);
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

LRESULT SettingsWindow::DoMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
  case WM_MW_MSGMODE_CHANGED: {
    // A hotkey, IPC, or the Messages window changed the mode -- re-read it
    // rather than leaving the combo showing what it was built with.
    HWND hMsgModeCombo = GetDlgItem(hWnd, IDC_MW_SPRITES_MESSAGES);
    if (hMsgModeCombo)
      SendMessageW(hMsgModeCombo, CB_SETCURSEL, m_pEngine->m_nSpriteMessagesMode, 0);
    return 0;
  }
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
    break;
  case WM_DROPFILES:
    LoadPresetFilesViaDragAndDrop(wParam);
    // The preset-directory field this used to refresh lives in the Presets
    // window now, not here, so GetDlgItem returned NULL and the write went
    // nowhere. The list below is what this window still shows.
    {
      HWND hList = GetDlgItem(hWnd, IDC_MW_PRESET_LIST);
      if (hList) {
        SendMessage(hList, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < m_pEngine->m_nPresets; i++) {
          if (m_pEngine->PresetNameAt(i).empty()) continue;
          SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)m_pEngine->PresetNameAt(i).c_str());
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
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
      if (GetOpenFileNameW(&ofn)) {
        RequestLoadPreset(-1, szFile);
        // Update current preset display -- the name, matching how the field is
        // built, and only if it differs. See SetTextIfChanged.
        SetTextIfChanged(GetDlgItem(hWnd, IDC_MW_CURRENT_PRESET),
                         FileNameOnly(p->m_szCurrentPresetFile));
      }
      return 0;
    }

    if (id == IDC_MW_RESOURCES && code == BN_CLICKED) {
      p->OpenResourceViewer();
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
              p->AddFallbackPath(path);
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
      // The index came from the list box, which is a snapshot; let the engine
      // say whether it still points at anything.
      if (p->RemoveFallbackPathAt(sel)) {
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
      HWND hMsgModeCombo = (HWND)lParam;
      int sel = (int)SendMessage(hMsgModeCombo, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 3)
        p->SetSpriteMessagesMode(sel);
      return 0;
    }

    // Theme mode combo box (Dark / Light / Follow System)
    if (id == IDC_MW_DARK_THEME && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 2) {
        p->m_nThemeMode = (Engine::ThemeMode)sel;
        wchar_t buf[8]; swprintf(buf, 8, L"%d", sel);
        Config().SetString(L"SettingsTheme", L"ThemeMode", buf);
        p->LoadSettingsThemeFromINI();
        ApplyDarkTheme();
        p->BroadcastFontSync(m_hWnd);
      }
      return 0;
    }

    // Idle timer action combo box
    if (id == IDC_MW_IDLE_ACTION && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 3) {
        p->m_nIdleAction = sel;
        p->SaveIdleTimerSettings();
        // The profile only means anything to the action that loads one.
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_PROFILE), sel == 3);
      }
      return 0;
    }

    // Which display profile the Load-display-profile action loads
    if (id == IDC_MW_IDLE_PROFILE && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0) {
        wchar_t name[MAX_PATH] = {};
        SendMessage((HWND)lParam, CB_GETLBTEXT, sel, (LPARAM)name);
        wcsncpy_s(p->m_szIdleProfile, name, _TRUNCATE);
        p->SaveIdleTimerSettings();
      }
      return 0;
    }

    // Fallback texture style combo box
    if (id == IDC_MW_FALLBACK_TEX && code == CBN_SELCHANGE) {
      int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
      if (sel >= 0 && sel <= 5) {
        p->m_nFallbackTexStyle = sel;
        Config().SetInt(L"Milkwave", L"FallbackTexStyle", sel);
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
          CopyTo(szDir, p->m_szFallbackTexFile);
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
              CopyTo(p->m_szFallbackTexFile, pPath);
              HWND hFileEdit = GetDlgItem(hWnd, IDC_MW_FALLBACK_FILE_EDIT);
              if (hFileEdit) SetWindowTextW(hFileEdit, pPath);
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
      HWND hFileEdit = GetDlgItem(hWnd, IDC_MW_FALLBACK_FILE_EDIT);
      if (hFileEdit) SetWindowTextW(hFileEdit, L"");
      p->SaveFallbackPaths();
      return 0;
    }

    // Audio device combo box selection
    if (id == IDC_MW_AUDIO_DEVICE && code == CBN_SELCHANGE) {
      HWND hDeviceCombo = (HWND)lParam;
      int sel = (int)SendMessage(hDeviceCombo, CB_GETCURSEL, 0, 0);
      if (sel >= 0) {
        wchar_t deviceName[MAX_PATH] = {};
        SendMessageW(hDeviceCombo, CB_GETLBTEXT, sel, (LPARAM)deviceName);

        if (sel == 0) {
          CopyTo(p->m_szAudioDevicePrevious, p->m_szAudioDevice);
          p->m_nAudioDevicePreviousType = p->m_nAudioDeviceActiveType;
          p->m_szAudioDevice[0] = L'\0';
          p->m_nAudioDeviceRequestType = 0;
          p->SetAudioDeviceDisplayName(NULL, true);
        }
        else {
          bool isInput = false;
          wchar_t cleanName[MAX_PATH];
          CopyTo(cleanName, deviceName);
          int len = lstrlenW(cleanName);
          const wchar_t* inputSuffix = L" [Input]";
          int suffixLen = lstrlenW(inputSuffix);
          if (len > suffixLen && _wcsicmp(cleanName + len - suffixLen, inputSuffix) == 0) {
            cleanName[len - suffixLen] = L'\0';
            isInput = true;
          }
          CopyTo(p->m_szAudioDevicePrevious, p->m_szAudioDevice);
          p->m_nAudioDevicePreviousType = p->m_nAudioDeviceActiveType;
          CopyTo(p->m_szAudioDevice, cleanName);
          p->m_nAudioDeviceRequestType = isInput ? 1 : 2;
          p->SetAudioDeviceDisplayName(cleanName, !isInput);
        }
        Config().SetString(L"Milkwave", L"AudioDevice", p->m_szAudioDevice);
        wchar_t reqBuf[16];
        swprintf(reqBuf, 16, L"%d", p->m_nAudioDeviceRequestType);
        Config().SetString(L"Milkwave", L"AudioDeviceRequestType", reqBuf);
        p->m_nAudioLoopState = 1;
        p->AddNotificationAudioDevice();
      }
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
          Config().SetInt(L"Milkwave", L"LogOutput", p->m_LogOutput);
          SetChecked(IDC_MW_LOGOUTPUT_FILE, true);
          SetChecked(IDC_MW_LOGOUTPUT_ODS, true);
        }
        p->m_LogLevel = newLevel;
        DebugLogSetLevel(newLevel);
        if (p->mdropdx12) p->mdropdx12->logLevel = newLevel;
        Config().SetInt(L"Milkwave", L"LogLevel", newLevel);
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
        Config().SetInt(L"Milkwave", L"LogOutput", p->m_LogOutput);
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
        // Each child reports its OWN frame rate, which is the point of showing
        // it there: one number per process, on the screen that process drives.
        p->RelayHudStateToChildren();
        return 0;
      case IDC_MW_SHOW_BATTERY:
        // Through the setter, not straight onto the member: the checkbox is
        // one of three ways in and they must all wake the poll.
        p->SetShowBattery(bChecked);
        p->SaveSettingToINI(SET_SHOW_BATTERY);
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
      case IDC_MW_IDLE_ENABLE: {
        p->m_bIdleTimerEnabled = bChecked;
        // Restore FIRST, the way SET_IDLE_TIMER=0 already does. Clearing
        // m_bIdleActivated on its own only forgot that idle was on screen: the
        // fullscreen and the mirrors it had put up stayed up with nothing left
        // that knew how to take them down. With a profile-loading action that
        // would have left the user's whole display arrangement replaced,
        // permanently, for unticking a checkbox.
        if (!bChecked && p->m_bIdleActivated) {
          HWND hr = p->GetPluginWindow();
          if (hr) PostMessage(hr, WM_MW_IDLE_RESTORE, 0, 0);
        }
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_TIMEOUT), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_TIMEOUT_SPIN), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_ACTION), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_PROFILE),
                     bChecked && p->m_nIdleAction == 3);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_FULL_OPACITY), bChecked);
        EnableWindow(GetDlgItem(hWnd, IDC_MW_IDLE_AUTO_RESTORE), bChecked);
        p->SaveIdleTimerSettings();
        return 0;
      }
      case IDC_MW_MIRROR_HUD_DISABLE:
        p->m_bDisableMirrorHud = bChecked;
        Config().SetInt(L"Settings", L"bDisableMirrorHud", bChecked ? 1 : 0);
        // A child reads this from settings.ini once, at startup, so unticking
        // the box did nothing to one already running -- it went on hiding its
        // HUD for the rest of its life.
        p->RelayHudStateToChildren();
        break;
      case IDC_MW_IDLE_AUTO_RESTORE:
        p->m_bIdleAutoRestore = bChecked;
        p->SaveIdleTimerSettings();
        return 0;
      case IDC_MW_IDLE_FULL_OPACITY:
        p->m_bIdleFullOpacity = bChecked;
        p->SaveIdleTimerSettings();
        return 0;
      // IDC_MW_DARK_THEME handled as CBN_SELCHANGE combo above
      }
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
      FormatTo(cmdLine, L"\"%s\" \"%%1\"", exePath);

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
        FormatTo(iconPath, L"%s,0", exePath);
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

    // Edit control changes (apply on focus lost)
    if (code == EN_KILLFOCUS) {
      wchar_t buf[64];
      GetWindowTextW((HWND)lParam, buf, 64);
      HWND hw = p->GetPluginWindow();
      switch (id) {
      case IDC_MW_AUDIO_SENS:
        p->m_fAudioSensitivity = (float)_wtof(buf);
        if (p->m_fAudioSensitivity <= 0.0f) p->m_fAudioSensitivity = 1.0f;
        if (p->m_fAudioSensitivity > 256) p->m_fAudioSensitivity = 256;
        mdropdx12_audio_sensitivity = p->m_fAudioSensitivity;
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
      case IDC_MW_IDLE_TIMEOUT: {
        int val = _wtoi(buf);
        if (val < 1) val = 1;
        if (val > 60) val = 60;
        if (val != p->m_nIdleTimeoutMinutes) {
          p->m_nIdleTimeoutMinutes = val;
          p->SaveIdleTimerSettings();
        }
        HWND hSpin = GetDlgItem(hWnd, IDC_MW_IDLE_TIMEOUT_SPIN);
        if (hSpin) SendMessage(hSpin, UDM_SETPOS32, 0, val);
        wchar_t norm[16];
        swprintf(norm, 16, L"%d", val);
        if (wcscmp(buf, norm) != 0)
          SetWindowTextW((HWND)lParam, norm);
        return 0;
      }
      }
    }

    return -1;  // not handled
  }

// Enumerate audio devices into a combo box. Returns the index of the current device, or -1.
static int EnumAudioDevicesIntoCombo(HWND hDeviceCombo, const wchar_t* szCurrentDevice) {
  int curIdx = -1;

  // Add "(Default)" as first entry
  SendMessageW(hDeviceCombo, CB_ADDSTRING, 0, (LPARAM)L"(Default)");

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
              int idx = (int)SendMessageW(hDeviceCombo, CB_ADDSTRING, 0, (LPARAM)pv.pwszVal);
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
              int idx = (int)SendMessageW(hDeviceCombo, CB_ADDSTRING, 0, (LPARAM)label);
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
  SendMessageW(hDeviceCombo, CB_SETCURSEL, curIdx >= 0 ? curIdx : 0, 0);
  return curIdx;
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
  const wchar_t* tabNames[] = { L"General", L"Tools", L"System", L"Files", L"About" };
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
      TrackTooltip(hTip);
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
  #define m_nCurrentPreset _e->m_nCurrentPreset
  #define m_fAudioSensitivity _e->m_fAudioSensitivity
  #define m_fBlendTimeAuto _e->m_fBlendTimeAuto
  #define m_fTimeBetweenPresets _e->m_fTimeBetweenPresets
  #define m_bHardCutsDisabled _e->m_bHardCutsDisabled
  #define m_bPresetLockOnAtStartup _e->m_bPresetLockOnAtStartup
  #define m_bSequentialPresetOrder _e->m_bSequentialPresetOrder
  #define m_nSpriteMessagesMode _e->m_nSpriteMessagesMode
  #define m_bShowFPS _e->m_bShowFPS
  #define m_bShowBattery _e->m_bShowBattery
  #define m_bAlwaysOnTop _e->m_bAlwaysOnTop
  #define m_WindowBorderless _e->m_WindowBorderless
  #define IsDarkTheme _e->IsDarkTheme
  #define m_nThemeMode _e->m_nThemeMode
  #define m_nPresetFilter _e->m_nPresetFilter
  #define m_szAudioDevice _e->m_szAudioDevice
  #define m_bIdleTimerEnabled _e->m_bIdleTimerEnabled
  #define m_nIdleTimeoutMinutes _e->m_nIdleTimeoutMinutes
  #define m_nIdleAction _e->m_nIdleAction
  #define m_bIdleAutoRestore _e->m_bIdleAutoRestore
  #define m_szIdleProfile _e->m_szIdleProfile
  #define m_bIdleFullOpacity _e->m_bIdleFullOpacity
  #define m_szBaseDir _e->m_szBaseDir
  #define m_LogLevel _e->m_LogLevel
  #define m_LogOutput _e->m_LogOutput
  #define m_szContentBasePath _e->m_szContentBasePath
  #define m_szRandomTexDir _e->m_szRandomTexDir
  #define m_nFallbackTexStyle _e->m_nFallbackTexStyle
  #define m_szFallbackTexFile _e->m_szFallbackTexFile
  #define m_colSettingsCtrlBg _e->m_colSettingsCtrlBg
  #define m_colSettingsText _e->m_colSettingsText
  #define GetConfigIniFile _e->GetConfigIniFile

  // ====== PAGE 0: General ======
  y = tabTop + 10;

  // Current/Startup Preset + Browse
  PAGE_CTRL(SP_GENERAL, CreateLabel(hw, L"Current Preset:", x, y, lw, lineH, hFont));
  PAGE_CTRL(SP_GENERAL, CreateReadonlyField(hw, FileNameOnly(m_szCurrentPresetFile), IDC_MW_CURRENT_PRESET, x + lw + 4, y, rw - lw - 74, lineH, hFont));
  {
    // In 26ths: a flat 65 fitted "Browse" at a line height of 26 and clipped
    // it at anything larger. The field beside it gives up the difference,
    // so the row is no wider than it was.
    const int brwW = MulDiv(65, lineH, 26);
    PAGE_CTRL(SP_GENERAL, CreateBtn(hw, L"Browse", IDC_MW_BROWSE_PRESET, x + rw - brwW, y, brwW, lineH, hFont));
  }
  y += lineH + gap;

  // ── Other ──
  PAGE_CTRL(SP_GENERAL, CreateLabel(hw, L"Messages/Sprites:", x, y, lw, lineH, hFont));
  {
    HWND hMsgModeCombo = CreateCombo(hw, IDC_MW_SPRITES_MESSAGES, x + lw + 4, y, rw - lw - 4,
      lineH + 4 * lineH + 4, hFont, CBS_DROPDOWNLIST, true, WS_EX_CLIENTEDGE);
    SendMessageW(hMsgModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Off");
    SendMessageW(hMsgModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Messages");
    SendMessageW(hMsgModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Sprites");
    SendMessageW(hMsgModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Messages & Sprites");
    SendMessageW(hMsgModeCombo, CB_SETCURSEL, m_nSpriteMessagesMode, 0);
    PAGE_CTRL(SP_GENERAL, hMsgModeCombo);
  }
  y += lineH + 2;
  PAGE_CTRL(SP_GENERAL, CreateCheck(hw, L"Show FPS",                IDC_MW_SHOW_FPS,     x, y, rw, lineH, hFont, m_bShowFPS)); y += lineH + 2;
  PAGE_CTRL(SP_GENERAL, CreateCheck(hw, L"Show Battery",            IDC_MW_SHOW_BATTERY, x, y, rw, lineH, hFont, m_bShowBattery)); y += lineH + 2;
  PAGE_CTRL(SP_GENERAL, CreateCheck(hw, L"Always On Top",           IDC_MW_ALWAYS_TOP,   x, y, rw, lineH, hFont, m_bAlwaysOnTop)); y += lineH + 2;
  PAGE_CTRL(SP_GENERAL, CreateCheck(hw, L"Borderless Window",       IDC_MW_BORDERLESS,   x, y, rw, lineH, hFont, m_WindowBorderless)); y += lineH + 2;
  {
    int themeLblW = MulDiv(55, lineH, 26);
    PAGE_CTRL(SP_GENERAL, CreateLabel(hw, L"Theme:", x, y, themeLblW, lineH, hFont, false));
    HWND hThemeCombo = CreateCombo(hw, IDC_MW_DARK_THEME, x + themeLblW + 4, y,
      rw - themeLblW - 4, 200, hFont, CBS_DROPDOWNLIST, false);
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

  // ====== PAGE 2: System (created hidden) ======
  y = tabTop + 10;

  // Audio Device
  PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Audio Device:", x, y, rw, lineH, hFont, false));
  y += lineH;
  {
    HWND hAudioCombo = CreateCombo(hw, IDC_MW_AUDIO_DEVICE, x, y, rw, 200, hFont,
      CBS_DROPDOWNLIST | WS_VSCROLL, false, WS_EX_CLIENTEDGE);
    EnumAudioDevicesIntoCombo(hAudioCombo, m_szAudioDevice);
    PAGE_CTRL(SP_SYSTEM, hAudioCombo);
  }
  y += lineH + gap + 8;

  // Idle Timer (screensaver mode)
  PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Idle Timer", x, y, rw / 2 - 4, lineH, hFontBold, false));
  PAGE_CTRL(SP_SYSTEM, CreateCheck(hw, L"Enable", IDC_MW_IDLE_ENABLE, x + rw / 2, y, rw / 2, lineH, hFont, m_bIdleTimerEnabled, false));
  y += lineH + gap;

  {
    int lblW = MulDiv(60, lineH, 26);
    int editW = MulDiv(50, lineH, 26);
    int comboW = rw - lblW - editW - MulDiv(100, lineH, 26);

    // Timeout
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Timeout:", x, y, lblW, lineH, hFont, false));
    HWND hTimeoutEdit = CreateEdit(hw, L"", IDC_MW_IDLE_TIMEOUT, x + lblW, y, editW, lineH, hFont);
    PAGE_CTRL(SP_SYSTEM, hTimeoutEdit);
    {
      wchar_t buf[16];
      swprintf(buf, 16, L"%d", m_nIdleTimeoutMinutes);
      SetWindowTextW(hTimeoutEdit, buf);
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
    HWND hIdleActionCombo = CreateCombo(hw, IDC_MW_IDLE_ACTION, actionX + lblW, y, comboW,
      lineH * 6, hFont, CBS_DROPDOWNLIST, false);
    SendMessageW(hIdleActionCombo, CB_ADDSTRING, 0, (LPARAM)L"Fullscreen");
    SendMessageW(hIdleActionCombo, CB_ADDSTRING, 0, (LPARAM)L"Stretch/Mirror");
    SendMessageW(hIdleActionCombo, CB_ADDSTRING, 0, (LPARAM)L"Mirror all");
    SendMessageW(hIdleActionCombo, CB_ADDSTRING, 0, (LPARAM)L"Load display profile");
    SendMessageW(hIdleActionCombo, CB_SETCURSEL, m_nIdleAction, 0);
    PAGE_CTRL(SP_SYSTEM, hIdleActionCombo);

    // Which profile the Load-display-profile action loads. Listed rather than
    // typed: ListDisplayProfiles is the same set every other profile control
    // offers, and a name that is not in it would simply fail at idle time,
    // hours later, with nobody watching.
    y += lineH + gap;
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Profile:", x, y, lblW, lineH, hFont, false));
    HWND hIdleProfileCombo = CreateCombo(hw, IDC_MW_IDLE_PROFILE, x + lblW, y,
      rw - lblW, lineH * 8, hFont, CBS_DROPDOWNLIST | WS_VSCROLL, false);
    {
      int sel = -1, i = 0;
      for (const auto& leaf : _e->ListDisplayProfiles()) {
        // Shown without the .json, stored without it too -- a bare name is what
        // ResolveDisplayProfilePath expects and what every profile verb takes.
        std::wstring name = leaf;
        if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".json") == 0)
          name.resize(name.size() - 5);
        SendMessageW(hIdleProfileCombo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        if (_wcsicmp(name.c_str(), m_szIdleProfile) == 0) sel = i;
        i++;
      }
      if (sel >= 0) SendMessageW(hIdleProfileCombo, CB_SETCURSEL, sel, 0);
    }
    PAGE_CTRL(SP_SYSTEM, hIdleProfileCombo);

    // Composes with all four actions, which is why it is a checkbox beside the
    // dropdown rather than an action of its own: a set of displays left in
    // watermark mode is dim, and nobody is looking through them once the user
    // has walked away.
    y += lineH + gap;
    PAGE_CTRL(SP_SYSTEM, CreateCheck(hw, L"All displays to 100% opacity", IDC_MW_IDLE_FULL_OPACITY,
      x, y, rw, lineH, hFont, m_bIdleFullOpacity, false));

    y += lineH + gap;
    PAGE_CTRL(SP_SYSTEM, CreateCheck(hw, L"Auto-restore on input", IDC_MW_IDLE_AUTO_RESTORE,
      x, y, rw, lineH, hFont, m_bIdleAutoRestore, false));

    // Disable idle timer controls if not enabled
    if (!m_bIdleTimerEnabled) {
      EnableWindow(GetDlgItem(hw, IDC_MW_IDLE_TIMEOUT), FALSE);
      EnableWindow(hSpin, FALSE);
      EnableWindow(hIdleActionCombo, FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_IDLE_FULL_OPACITY), FALSE);
      EnableWindow(GetDlgItem(hw, IDC_MW_IDLE_AUTO_RESTORE), FALSE);
    }
    // The profile only means anything to the action that loads one.
    if (!m_bIdleTimerEnabled || m_nIdleAction != 3)
      EnableWindow(hIdleProfileCombo, FALSE);
  }
  y += lineH + gap + 8;

  PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"HUD", x, y, rw / 2, lineH, hFontBold, false));
  y += lineH + gap;
  {
    int slLbl = MulDiv(90, lineH, 26);
    int valW = MulDiv(48, lineH, 26);
    int sliderW = rw - slLbl - valW - 8;
    int slPos = (int)(_e->m_fHudFontUserScale * 100.0f + 0.5f);
    if (slPos < 10) slPos = 10;
    if (slPos > 200) slPos = 200;
    PAGE_CTRL(SP_SYSTEM, CreateLabel(hw, L"Font scale:", x, y, slLbl, lineH, hFont, false));
    PAGE_CTRL(SP_SYSTEM, CreateSlider(hw, IDC_MW_HUD_FONT_SCALE, x + slLbl + 4, y, sliderW, lineH, 10, 200, slPos, false));
    wchar_t pct[16];
    swprintf(pct, 16, L"%d%%", slPos);
    HWND hPct = CreateWindowExW(0, L"STATIC", pct, WS_CHILD | SS_LEFT,
      x + slLbl + 4 + sliderW + 4, y, valW, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_HUD_FONT_SCALE_LABEL, GetModuleHandle(NULL), NULL);
    if (hPct && hFont) SendMessage(hPct, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_SYSTEM, hPct);
  }
  y += lineH + gap;
  // "on mirrors" undersold it and mismatched the behaviour: a display running
  // its own process is a child, not a mirror, and kept its HUD. The flag now
  // covers every non-primary display, so the label says what it does. The ini
  // key stays bDisableMirrorHud -- renaming it would orphan the setting.
  PAGE_CTRL(SP_SYSTEM, CreateCheck(hw, L"Show HUD only on primary display",
    IDC_MW_MIRROR_HUD_DISABLE, x, y, rw, lineH, hFont, _e->m_bDisableMirrorHud, false));

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
    HWND hBaseEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_szContentBasePath,
      WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_CONTENT_BASE_EDIT,
      GetModuleHandle(NULL), NULL);
    if (hBaseEdit && hFont) SendMessage(hBaseEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_FILES, hBaseEdit);
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
    HWND hList = CreateListBox(hw, IDC_MW_FILE_LIST, x, y, rw, 80, hFont,
      WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, false, 0);
    // Populate from the fallback paths. A copy, because LB_ADDSTRING is a
    // SendMessage and therefore pumps.
    for (auto& p : _e->FallbackPathsCopy())
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
    HWND hRandTexEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_szRandomTexDir,
      WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_RANDTEX_EDIT,
      GetModuleHandle(NULL), NULL);
    if (hRandTexEdit && hFont) SendMessage(hRandTexEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    PAGE_CTRL(SP_FILES, hRandTexEdit);
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
    HWND hFallbackCombo = CreateCombo(hw, IDC_MW_FALLBACK_TEX, x, y, comboW, lineH + 8 * lineH,
      hFont, CBS_DROPDOWNLIST, false, WS_EX_CLIENTEDGE);
    SendMessageW(hFallbackCombo, CB_ADDSTRING, 0, (LPARAM)L"Hue Gradient");
    SendMessageW(hFallbackCombo, CB_ADDSTRING, 0, (LPARAM)L"White");
    SendMessageW(hFallbackCombo, CB_ADDSTRING, 0, (LPARAM)L"Black");
    SendMessageW(hFallbackCombo, CB_ADDSTRING, 0, (LPARAM)L"Random (Random Tex Dir)");
    SendMessageW(hFallbackCombo, CB_ADDSTRING, 0, (LPARAM)L"Random (Textures Dir)");
    SendMessageW(hFallbackCombo, CB_ADDSTRING, 0, (LPARAM)L"Custom File...");
    SendMessageW(hFallbackCombo, CB_SETCURSEL, m_nFallbackTexStyle, 0);
    PAGE_CTRL(SP_FILES, hFallbackCombo);
  }
  y += lineH + 6;
  // Custom fallback texture file controls (visible only when style == 5)
  {
    DWORD showStyle = (m_nFallbackTexStyle == 5) ? WS_CHILD : (WS_CHILD & ~WS_VISIBLE);
    HWND hFallbackFileEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_szFallbackTexFile,
      showStyle | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, lineH + 4, hw, (HMENU)(INT_PTR)IDC_MW_FALLBACK_FILE_EDIT,
      GetModuleHandle(NULL), NULL);
    if (hFallbackFileEdit && hFont) SendMessage(hFallbackFileEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    if (m_nFallbackTexStyle == 5) ShowWindow(hFallbackFileEdit, SW_SHOW);
    PAGE_CTRL(SP_FILES, hFallbackFileEdit);
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

  // ====== PAGE 1: Tools ======
  y = tabTop + 10;
  {
    RECT rc;
    GetClientRect(hw, &rc);
    int btnRowH = lineH + gap + lineH + 16; // two button rows below list
    int listH = rc.bottom - y - btnRowH - 16;
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

  // ===== About tab (page 6) =====
  y = tabTop + 10;

  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"MDropDX12", x, y, rw, 24, hFontBold, false));
  y += 28;

  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Version " MDROP_VERSION_STRW, x, y, rw, lineH, hFont, false));
  y += lineH + 4;

  {
    // The EXE's own timestamp, not __DATE__/__TIME__ -- those expand when THIS
    // FILE compiles, so an incremental build that did not touch it left the
    // About tab showing a build that was days old while the exe was minutes
    // old. Reported as "f8 shows the wrong build time".
    wchar_t szBuild[128];
    swprintf(szBuild, 128, L"Built: %s", BuildTimestampW());
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
    // A path is as long as it is, so no wording change can make one fit.
    // SS_PATHELLIPSIS takes the middle out instead of hard-cutting the end,
    // which keeps the drive and the leaf -- the two parts anyone reads --
    // and the whole path is on the tooltip (issue 159).
    auto pathRow = [&](const wchar_t* text) {
      HWND hLbl = CreateLabel(hw, text, x, y, rw, lineH, hFont, false);
      if (hLbl) {
        SetWindowLongPtrW(hLbl, GWL_STYLE,
            GetWindowLongPtrW(hLbl, GWL_STYLE) | SS_PATHELLIPSIS);
        AttachTip(hLbl, text);
      }
      PAGE_CTRL(SP_ABOUT, hLbl);
      y += lineH + 2;
    };
    swprintf(buf, MAX_PATH + 64, L"Base Dir:  %s", m_szBaseDir);
    pathRow(buf);
    swprintf(buf, MAX_PATH + 64, L"Settings:  %s", GetConfigIniFile());
    pathRow(buf);
    swprintf(buf, MAX_PATH + 64, L"Presets:   %s", m_szPresetDir);
    pathRow(buf);
  }
  y += 8;

  // Debug Log Level radio buttons
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"Debug Log Level:", x, y, lw, lineH, hFont, false));
  {
    int rx = x + lw + 4;
    // In 26ths like every other width here. A flat 80 does not grow with the
    // font, so at a large one the check glyph and "Verbose" no longer fit in
    // the same box and the word was cut (issue 159).
    int rbw = MulDiv(80, lineH, 26);
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
    int cbw = MulDiv(120, lineH, 26);
    PAGE_CTRL(SP_ABOUT, CreateCheck(hw, L"File",            IDC_MW_LOGOUTPUT_FILE, cx, y, cbw, lineH, hFont, (m_LogOutput & LOG_OUTPUT_FILE) != 0));
    cx += cbw;
    PAGE_CTRL(SP_ABOUT, CreateCheck(hw, L"Debug Messages",  IDC_MW_LOGOUTPUT_ODS,  cx, y, cbw + MulDiv(40, lineH, 26), lineH, hFont, (m_LogOutput & LOG_OUTPUT_ODS) != 0));
  }
  y += lineH + 8;

  // File Association button
  PAGE_CTRL(SP_ABOUT, CreateLabel(hw, L"File Association:", x, y, lw, lineH, hFont, false));
  {
    int btnW = MulDiv(200, lineH, 26);
    HWND hAssoc = CreateBtn(hw, L"Register File Types", IDC_MW_FILE_ASSOC,
                            x + lw + 4, y, btnW, lineH, hFont, false);
    PAGE_CTRL(SP_ABOUT, hAssoc);
    AttachTip(hAssoc, L"Register .milk, .milk2 and .milk3 with Explorer, so double-clicking a preset opens it here. Current user only; no administrator rights needed.");
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

  #undef PAGE_CTRL

  // Undef all engine-member macros
  #undef m_szCurrentPresetFile
  #undef m_szPresetDir
  #undef m_nPresets
  #undef m_nCurrentPreset
  #undef m_fAudioSensitivity
  #undef m_fBlendTimeAuto
  #undef m_fTimeBetweenPresets
  #undef m_bHardCutsDisabled
  #undef m_bPresetLockOnAtStartup
  #undef m_bSequentialPresetOrder
  #undef m_nSpriteMessagesMode
  #undef m_bShowFPS
  #undef m_bShowBattery
  #undef m_bAlwaysOnTop
  #undef m_WindowBorderless
  #undef IsDarkTheme
  #undef m_nThemeMode
  #undef m_nPresetFilter
  #undef m_szAudioDevice
  #undef m_bIdleTimerEnabled
  #undef m_nIdleTimeoutMinutes
  #undef m_nIdleAction
  #undef m_bIdleAutoRestore
  #undef m_szIdleProfile
  #undef m_bIdleFullOpacity
  #undef m_szBaseDir
  #undef m_LogLevel
  #undef m_LogOutput
  #undef m_szContentBasePath
  #undef m_szRandomTexDir
  #undef m_nFallbackTexStyle
  #undef m_szFallbackTexFile
  #undef m_colSettingsCtrlBg
  #undef m_colSettingsText
  #undef GetConfigIniFile

  SelectInitialTab();
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

LRESULT SettingsWindow::DoHScroll(HWND hWnd, int id, int pos) {
  Engine* p = m_pEngine;
  if (id == IDC_MW_HUD_FONT_SCALE) {
    if (pos < 10) pos = 10;
    if (pos > 200) pos = 200;
    p->m_fHudFontUserScale = pos / 100.0f;
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", pos);
    SetWindowTextW(GetDlgItem(hWnd, IDC_MW_HUD_FONT_SCALE_LABEL), buf);
    p->m_text.SetHudUserScale(p->m_fHudFontUserScale);
    return 0;
  }
  return -1;
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
  p->m_fHudFontUserScale = 1.0f;
  p->m_bDisableMirrorHud = false;
  p->m_text.SetHudUserScale(1.0f);
  // Apply side-effects
  HWND hw = p->GetPluginWindow();
  if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
  if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
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
  // Apply side-effects
  HWND hw = p->GetPluginWindow();
  if (hw) PostMessage(hw, WM_MW_SET_OPACITY, 0, 0);
  if (hw) PostMessage(hw, WM_MW_RESET_BUFFERS, 0, 0);
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
  // Testing mode never takes the focus.
  //
  // The Settings window is not a ToolWindow and so did not inherit that gate,
  // but a test opens it the same way and the interruption is identical. Shane
  // types at 80wpm and cannot use speech input, so a window that grabs the
  // keyboard mid-run costs him several characters into the wrong window --
  // and those keystrokes land in the window under test, which makes the
  // measurement wrong as well as the interruption rude.
  //
  // Only the ACTIVATION is suppressed. Being visible is fine and wanted:
  // "I don't mind seeing the window on the render screen it was the stealing
  // focus." Everything below still runs, so the window is still moved onto a
  // sensible monitor.
  const bool quiet = m_pEngine && m_pEngine->m_bTestingMode;

  if (!m_hWnd || !IsWindow(m_hWnd) || !IsWindowVisible(m_hWnd))
    return;

  HWND hRender = m_pEngine->GetPluginWindow();
  if (!hRender) return;

  HMONITOR hRenderMon = MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST);
  HMONITOR hSettingsMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);

  // Only act if both windows are on the same monitor AND render is fullscreen
  if (hRenderMon != hSettingsMon || !m_pEngine->IsBorderlessFullscreen(hRender)) {
    if (!quiet) SetForegroundWindow(m_hWnd);
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
    if (!quiet) SetForegroundWindow(m_hWnd);
  }
}

// ====== Resource Viewer ======

static bool g_bResourceViewerClassRegistered = false;

void Engine::OpenResourceViewer() {
  // Asked for BEFORE the window is built, not after (#352). The snapshot now
  // comes from the render thread, so it arrives a frame late; requesting it
  // first means the frame is spent creating and theming the window rather
  // than staring at an empty list.
  if (!m_bResourceSnapshotPending.exchange(true))
    EnqueueRenderCmd(RenderCmd::SnapshotResources);

  if (m_hResourceWnd && IsWindow(m_hResourceWnd)) {
    if (m_bTestingMode) {
      ShowWindow(m_hResourceWnd, SW_SHOWNOACTIVATE);
    } else {
      ShowWindow(m_hResourceWnd, SW_SHOW);
      SetForegroundWindow(m_hResourceWnd);
    }
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

  // Created HIDDEN and shown at the bottom of this function, once it has been
  // put somewhere sensible.
  //
  // WS_VISIBLE with CW_USEDEFAULT let Windows cascade it, which is the primary
  // display as often as not -- and it came up activated. This is not a
  // ToolWindow, so none of ToolWindow::Open's placement or its testing-mode
  // quiet applies to it, and both had to be spelled out here.
  HWND hOwner = m_settingsWindow ? m_settingsWindow->GetHWND() : NULL;
  m_hResourceWnd = CreateWindowExW(
    WS_EX_TOOLWINDOW,
    L"MDropDX12ResourceViewer",
    L"Resource Viewer",
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
    CW_USEDEFAULT, CW_USEDEFAULT, 750, 420,
    hOwner,
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

  // On the OWNER's monitor, offset from its corner, clamped into the work
  // area -- so it lands beside the Settings window the user opened it from
  // rather than wherever Windows felt like cascading it, and in particular
  // not on the primary display when nothing else of ours is there.
  {
    HMONITOR hMon = MonitorFromWindow(hOwner ? hOwner : m_hResourceWnd,
                                      MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (hMon && GetMonitorInfoW(hMon, &mi)) {
      RECT rcOwner = mi.rcWork;
      if (hOwner) GetWindowRect(hOwner, &rcOwner);
      const int w = 750, h = 420;
      int x = rcOwner.left + 40;
      int y = rcOwner.top + 40;
      if (x + w > mi.rcWork.right)  x = mi.rcWork.right - w;
      if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
      if (x < mi.rcWork.left)       x = mi.rcWork.left;
      if (y < mi.rcWork.top)        y = mi.rcWork.top;
      SetWindowPos(m_hResourceWnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
    }
  }

  // Shown without activation under testing mode, for the reason every other
  // window here is: a run must not take the focus out from under someone who
  // is typing, because his keystrokes then land in the window under test and
  // the measurement is wrong as well as the interruption rude.
  ShowWindow(m_hResourceWnd, m_bTestingMode ? SW_SHOWNOACTIVATE : SW_SHOW);

  LayoutResourceViewer();
  PopulateResourceViewer();
}

LRESULT CALLBACK Engine::ResourceViewerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  Engine* p = (Engine*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

  switch (uMsg) {
  case WM_CLOSE:
    ShowWindow(hWnd, SW_HIDE);
    return 0;

  // The render thread has a fresh snapshot (#352). Draw it -- and DRAW only:
  // calling PopulateResourceViewer here would request another snapshot, whose
  // arrival would request another, forever.
  case WM_MW_RESOURCES_READY:
    if (p) p->DrawResourceViewerRows();
    return 0;

  // The render thread PostMessages to m_hResourceWnd, so it must not be left
  // holding an HWND that Windows has recycled. This window is normally only
  // hidden, but it is owned by the Settings ToolWindow and goes down with it.
  case WM_NCDESTROY:
    if (p && p->m_hResourceWnd == hWnd) {
      p->m_hResourceWnd = NULL;
      p->m_hResourceList = NULL;
    }
    break;

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
          CopyTo(szClip, szPath);
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

// Build the Resource Viewer's snapshot. RENDER THREAD ONLY (#352).
//
// Everything read here -- m_textures, m_shaders, m_pState, the search paths --
// belongs to this thread, so none of it is locked. The guard at the bottom
// covers the handoff and nothing else, and is held across a move, never across
// file I/O or a SendMessage.
//
// What this does NOT do is search the disk for a texture. That is up to 8
// extensions x 6 directories per custom texture and it stays on the UI side;
// see DrawResourceViewerRows.
void Engine::BuildResourceSnapshot() {
  m_bResourceSnapshotPending.store(false);

  ResourceSnapshot snap;
  snap.bEverBuilt = true;

  wchar_t szDetails[128];
  auto addRow = [&](const wchar_t* type, const wchar_t* name, const wchar_t* path,
                    const wchar_t* details, bool valid) {
    ResourceRow r;
    r.sType = type;
    r.sName = name;
    r.sPath = path;
    r.sDetails = details;
    r.bValid = valid;
    snap.rows.push_back(std::move(r));
  };

  // 1. Render Targets
  for (int i = 0; i < 2; i++) {
    wchar_t szName[32];
    swprintf(szName, 32, L"VS[%d]", i);
    bool valid = m_dx12VS[i].IsValid();
    swprintf(szDetails, 128, L"%dx%d", m_dx12VS[i].width, m_dx12VS[i].height);
    addRow(L"Render Target", szName, L"(render target)", valid ? szDetails : L"", valid);
  }

  // 2. Noise Textures (non-evictable)
  for (int i = 0; i < (int)m_textures.size(); i++) {
    if (m_textures[i].bEvictable) continue;
    bool valid = (m_textures[i].dx12Tex.srvIndex != UINT_MAX) || (m_textures[i].texptr != NULL);
    if (m_textures[i].d > 1)
      swprintf(szDetails, 128, L"%dx%dx%d", m_textures[i].w, m_textures[i].h, m_textures[i].d);
    else
      swprintf(szDetails, 128, L"%dx%d", m_textures[i].w, m_textures[i].h);
    addRow(L"Noise", m_textures[i].texname, L"(procedural)", valid ? szDetails : L"", valid);
  }

  // 3. Blur Targets
  for (int i = 0; i < NUM_BLUR_TEX; i++) {
    wchar_t szName[32];
    swprintf(szName, 32, L"blur[%d]", i);
    bool valid = m_dx12Blur[i].IsValid();
    swprintf(szDetails, 128, L"%dx%d", m_nBlurTexW[i], m_nBlurTexH[i]);
    addRow(L"Blur Target", szName, L"(render target)", valid ? szDetails : L"", valid);
  }

  // 4. Shaders
  {
    bool warpHasCode = m_pState && m_pState->m_nWarpPSVersion > 0 && m_pState->m_szWarpShadersText[0] != 0;
    bool warpOk = m_shaders.warp.bytecodeBlob != NULL;
    wchar_t szVer[32] = L"";
    if (m_pState) swprintf(szVer, 32, L"ps_%d_0", m_pState->m_nWarpPSVersion);
    addRow(L"Warp Shader", L"warp", warpHasCode ? L"(custom)" : L"(default)", szVer, warpOk);

    bool compHasCode = m_pState && m_pState->m_nCompPSVersion > 0 && m_pState->m_szCompShadersText[0] != 0;
    bool compOk = m_shaders.comp.bytecodeBlob != NULL;
    szVer[0] = 0;
    if (m_pState) swprintf(szVer, 32, L"ps_%d_0", m_pState->m_nCompPSVersion);
    addRow(L"Comp Shader", L"comp", compHasCode ? L"(custom)" : L"(default)", szVer, compOk);
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
        CopyTo(szSamplerName, AutoWide(cd.Name));

        wchar_t szRootName[MAX_PATH];
        if (!strncmp(cd.Name, "sampler_", 8))
          CopyTo(szRootName, AutoWide(&cd.Name[8]));
        else
          CopyTo(szRootName, AutoWide(cd.Name));

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

        // The disk search that used to stand here runs in
        // DrawResourceViewerRows now. It is file I/O -- up to eight extensions
        // across six directories per texture -- and the render thread has no
        // business paying that to fill a list. All this half owes the UI is
        // the name to look for.
        ResourceRow r;
        r.sType = L"Custom Tex";
        r.sName = szSamplerName;
        r.sLookup = szLookupName;
        r.bSearchPath = true;
        r.bValid = found;
        if (found) {
          swprintf(szDetails, 128, L"%dx%d", m_textures[texIdx].w, m_textures[texIdx].h);
          r.sDetails = szDetails;
        } else {
          r.sDetails = L"(missing)";
        }
        snap.rows.push_back(std::move(r));
      }
    }
  }

  // The search paths, copied. m_sz*Path are rewritten on preset load, and
  // SiblingResourceDirs() is a cache that clears and rebuilds a vector and
  // hands back a REFERENCE to it -- which the UI thread used to call, from the
  // other side of a directory listing this thread performs (#352).
  snap.sMilkdrop2Path   = m_szMilkdrop2Path;
  snap.sPresetDir       = m_szPresetDir;
  snap.sRandomTexDir    = m_szRandomTexDir;
  snap.sContentBasePath = m_szContentBasePath;
  snap.fallbackPaths    = FallbackPathsCopy();
  snap.siblingDirs      = SiblingResourceDirs();

  m_resourceSnapshot.with([&](ResourceSnapshot& dst) { dst = std::move(snap); });

  // Tell the viewer, if it is still there. PostMessage and not Send: this is
  // the render thread, and blocking it on a ToolWindow's message pump is the
  // deadlock RefreshDisplaysTab documents at length.
  if (m_hResourceWnd && IsWindow(m_hResourceWnd))
    PostMessageW(m_hResourceWnd, WM_MW_RESOURCES_READY, 0, 0);
}

//----------------------------------------------------------------------
// Resource Viewer: the UI half
//----------------------------------------------------------------------

// Ask for a fresh snapshot, then draw whichever one is on hand. UI thread.
//
// Asynchronous on purpose. Blocking here until the render thread answers would
// make a stalled renderer freeze the very window that exists to report what
// the renderer is holding.
void Engine::PopulateResourceViewer() {
  if (!m_hResourceList) return;
  // Collapsed, so leaning on Refresh queues one rebuild rather than thirty.
  if (!m_bResourceSnapshotPending.exchange(true))
    EnqueueRenderCmd(RenderCmd::SnapshotResources);
  DrawResourceViewerRows();
}

// Draw the current snapshot. UI thread, and it reads NO engine state: the
// snapshot is copied out under its guard and everything after that -- the
// per-texture disk search, the ListView -- runs against the copy.
void Engine::DrawResourceViewerRows() {
  if (!m_hResourceList) return;

  const ResourceSnapshot snap =
      m_resourceSnapshot.with([](const ResourceSnapshot& s) { return s; });

  // The first snapshot is still a frame away. Leave whatever is on screen
  // rather than blinking the list empty on the way past.
  if (!snap.bEverBuilt) return;

  // Where a custom texture's file actually is, searched in the order
  // CacheParams searches -- sibling directories included (#299). A viewer that
  // disagrees with the loader is worse than no viewer: it reports a texture
  // MISSING while the engine has it on screen, and sends whoever reads it
  // looking for a problem with the preset.
  const std::wstring texSub = snap.sMilkdrop2Path + L"textures\\";
  auto findTexturePath = [&](const std::wstring& lookup,
                             wchar_t (&szFullPath)[MAX_PATH * 2]) {
    bool pathFound = false;
    for (int z = 0; z < texture_exts_count; z++) {
      wchar_t szTry[MAX_PATH];
      auto probe = [&](const wchar_t* dir) {
        if (!dir || !dir[0]) return false;
        swprintf(szTry, MAX_PATH, L"%s%s.%s", dir, lookup.c_str(), texture_exts[z].c_str());
        if (GetFileAttributesW(szTry) == 0xFFFFFFFF) return false;
        CopyTo(szFullPath, szTry);
        return true;
      };
      if (probe(texSub.c_str()))                { pathFound = true; break; }
      if (probe(snap.sPresetDir.c_str()))       { pathFound = true; break; }
      if (probe(snap.sRandomTexDir.c_str()))    { pathFound = true; break; }
      if (probe(snap.sContentBasePath.c_str())) { pathFound = true; break; }
      for (const auto& fb : snap.fallbackPaths)
        if (probe(fb.c_str())) { pathFound = true; break; }
      if (pathFound) break;
      for (const auto& dir : snap.siblingDirs)
        if (probe(dir.c_str())) { pathFound = true; break; }
      if (pathFound) break;
    }
    if (!pathFound)  // show the expected primary search path for a miss
      swprintf(szFullPath, MAX_PATH * 2, L"%s%s", texSub.c_str(), lookup.c_str());
  };

  SendMessageW(m_hResourceList, LVM_DELETEALLITEMS, 0, 0);

  int row = 0;
  for (const auto& r : snap.rows) {
    const wchar_t* status = r.bValid ? L"\u2713" : L"\u2717";
    if (!r.bSearchPath) {
      RV_AddRow(m_hResourceList, row++, status, r.sType.c_str(), r.sName.c_str(),
                r.sPath.c_str(), r.sDetails.c_str());
      continue;
    }
    wchar_t szFullPath[MAX_PATH * 2] = {};
    findTexturePath(r.sLookup, szFullPath);
    RV_AddRow(m_hResourceList, row++, status, r.sType.c_str(), r.sName.c_str(),
              szFullPath, r.sDetails.c_str());
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
  HWND hProfileCombo = GetDlgItem(hWnd, IDC_MW_WTP_PROFILE);
  if (!hProfileCombo) return;
  SendMessage(hProfileCombo, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < (int)ctx->profiles.size(); i++) {
    const wchar_t* name = ctx->profiles[i].szName;
    SendMessageW(hProfileCombo, CB_ADDSTRING, 0, (LPARAM)(name[0] ? name : L"(unnamed)"));
  }
  if (!ctx->profiles.empty())
    SendMessage(hProfileCombo, CB_SETCURSEL, ctx->nActiveSel, 0);
}

static void WTPLoadProfileFields(HWND hWnd, WTPContext* ctx) {
  // No profile selected -- which is the state this window opens in when the
  // list is empty, before New has ever been pressed.
  //
  // This used to be a bare `return`. It cleared nothing and disabled nothing,
  // so all four edits kept whatever they held and stayed fully editable while
  // there was nowhere to put the text: every site that saves these fields is
  // guarded by the same range test, so anything typed was silently dropped on
  // OK, on New, and on switching profiles. Nothing corrupts, but the window
  // invites work it then discards, which is worse than saying so.
  const bool have = ctx->nActiveSel >= 0 &&
                    ctx->nActiveSel < (int)ctx->profiles.size();
  if (!have) {
    for (int id : { IDC_MW_WTP_NAME, IDC_MW_WTP_WINDOW_REGEX,
                    IDC_MW_WTP_PARSE_REGEX, IDC_MW_WTP_INTERVAL }) {
      HWND h = GetDlgItem(hWnd, id);
      if (!h) continue;
      SetWindowTextW(h, L"");
      EnableWindow(h, FALSE);
    }
    // The two read-outs describe a profile that is not there.
    SetDlgItemTextW(hWnd, IDC_MW_WTP_MATCHED, L"");
    SetDlgItemTextW(hWnd, IDC_MW_WTP_PARSED, L"");
    // Delete has nothing to delete either; New is what this state needs.
    if (HWND h = GetDlgItem(hWnd, IDC_MW_WTP_DELETE)) EnableWindow(h, FALSE);
    return;
  }
  for (int id : { IDC_MW_WTP_NAME, IDC_MW_WTP_WINDOW_REGEX,
                  IDC_MW_WTP_PARSE_REGEX, IDC_MW_WTP_INTERVAL }) {
    if (HWND h = GetDlgItem(hWnd, id)) EnableWindow(h, TRUE);
  }
  if (HWND h = GetDlgItem(hWnd, IDC_MW_WTP_DELETE)) EnableWindow(h, TRUE);

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
      HWND hProfileCombo = CreateCombo(hWnd, IDC_MW_WTP_PROFILE, x + lw + 4, y, comboW,
        lineH + 10 * lineH, hFont, CBS_DROPDOWNLIST, true, WS_EX_CLIENTEDGE);

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
      HWND hWinCombo = CreateCombo(hWnd, IDC_MW_WTP_WINDOWS, x + lw + 4, y, rw - lw - 4,
        lineH + 15 * lineH, hFont, CBS_DROPDOWNLIST | WS_VSCROLL, true, WS_EX_CLIENTEDGE);
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
      CreateEdit(hWnd, L"2", IDC_MW_WTP_INTERVAL, x + lw + 4, y, MulDiv(40, lineH, 26), lineH, hFont);
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
      CopyTo(newP.szName, L"New Profile");
      CopyTo(newP.szParseRegex, L"(?<artist>.+?) - (?<title>.+)");
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
      HWND hWinCombo = (HWND)lParam;
      SendMessage(hWinCombo, CB_RESETCONTENT, 0, 0);
      std::vector<std::wstring> titles;
      EnumWindows(EnumVisibleWindowTitlesProc, (LPARAM)&titles);
      std::sort(titles.begin(), titles.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
      });
      for (const auto& t : titles)
        SendMessageW(hWinCombo, CB_ADDSTRING, 0, (LPARAM)t.c_str());
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
