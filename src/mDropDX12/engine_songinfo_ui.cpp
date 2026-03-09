/*
  SongInfoWindow — Song Info window (ToolWindow subclass).
  Provides Song Info source selection, track info display options, and
  window title parser configuration.
  Launched from "Song Info..." button on the Settings General tab.
*/

#include "tool_window.h"
#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <regex>
#include <map>

extern MDropDX12 mdropdx12;

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------

SongInfoWindow::SongInfoWindow(Engine* pEngine)
  : ToolWindow(pEngine, 500, 600) {}

//----------------------------------------------------------------------
// Engine bridge: Open/Close via Engine members
//----------------------------------------------------------------------

void Engine::OpenSongInfoWindow() {
  if (!m_songInfoWindow)
    m_songInfoWindow = std::make_unique<SongInfoWindow>(this);
  m_songInfoWindow->Open();
}

void Engine::CloseSongInfoWindow() {
  if (m_songInfoWindow)
    m_songInfoWindow->Close();
}

//----------------------------------------------------------------------
// Window title preview helpers (same logic as in engine_settings_ui.cpp)
//----------------------------------------------------------------------

struct SongInfoWTMatchContext {
  const std::wregex* pRegex;
  std::wstring matchedTitle;
};

static BOOL CALLBACK EnumWindowsSongInfoMatch(HWND hWnd, LPARAM lParam) {
  auto* ctx = reinterpret_cast<SongInfoWTMatchContext*>(lParam);
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

static void UpdateWindowTitlePreview(HWND hWnd, const wchar_t* windowRegex) {
  HWND hPreview = GetDlgItem(hWnd, IDC_MW_SONG_WT_PREVIEW);
  if (!hPreview) return;
  if (!windowRegex || !windowRegex[0]) { SetWindowTextW(hPreview, L""); return; }

  std::wregex compiledRegex;
  try {
    compiledRegex = std::wregex(windowRegex, std::regex_constants::ECMAScript | std::regex_constants::icase);
  } catch (...) {
    SetWindowTextW(hPreview, L"(invalid regex)");
    return;
  }

  SongInfoWTMatchContext ctx = { &compiledRegex, {} };
  EnumWindows(EnumWindowsSongInfoMatch, reinterpret_cast<LPARAM>(&ctx));
  if (ctx.matchedTitle.empty()) { SetWindowTextW(hPreview, L"(no match)"); return; }

  std::wstring preview;
  int idx = g_engine.m_nActiveWindowTitleProfile;
  if (idx >= 0 && idx < (int)g_engine.m_windowTitleProfiles.size() && g_engine.m_windowTitleProfiles[idx].szParseRegex[0]) {
    try {
      std::wregex parseRegex(StripNamedGroups(g_engine.m_windowTitleProfiles[idx].szParseRegex), std::regex_constants::ECMAScript);
      std::wsmatch match;
      if (std::regex_search(ctx.matchedTitle, match, parseRegex)) {
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
    preview = L"Matched: " + ctx.matchedTitle;
  }
  SetWindowTextW(hPreview, preview.c_str());
}

//----------------------------------------------------------------------
// Build Controls
//----------------------------------------------------------------------

void SongInfoWindow::DoBuildControls() {
  HWND hw = m_hWnd;
  if (!hw) return;

  Engine* p = m_pEngine;

  // Common: fonts, font +/- buttons, pin button
  auto L = BuildBaseControls();
  int y = L.y, lineH = L.lineH, gap = L.gap, x = L.x, rw = L.rw;
  HFONT hFont = m_hFont;
  HFONT hFontBold = m_hFontBold;
  int lw = MulDiv(140, lineH, 26);
  wchar_t buf[64];

  // ── Header ──
  TrackControl(CreateLabel(hw, L"Song Info", x, y, rw, lineH, hFontBold));
  y += lineH + gap;

  // ── Source combo ──
  TrackControl(CreateLabel(hw, L"Source:", x, y, lw, lineH, hFont));
  {
    HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      x + lw + 4, y, rw - lw - 4, lineH + 4 * lineH, hw, (HMENU)(INT_PTR)IDC_MW_SONG_SOURCE,
      GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"SMTC (Windows)");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"IPC (Remote)");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Window Title");
    SendMessageW(hCombo, CB_SETCURSEL, p->m_nTrackInfoSource, 0);
    TrackControl(hCombo);
  }
  y += lineH + 2;

  // ── Window Title profile selector + Edit button (visible when source = Window Title) ──
  {
    bool showWT = (p->m_nTrackInfoSource == Engine::TRACK_SOURCE_WINDOW);
    DWORD vis = showWT ? WS_VISIBLE : 0;

    HWND hWTLabel = CreateWindowExW(0, L"STATIC", L"Profile:",
      WS_CHILD | vis, x, y, lw, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_SONG_WT_LABEL, GetModuleHandle(NULL), NULL);
    if (hWTLabel && hFont) SendMessage(hWTLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hWTLabel);

    int editBtnW = MulDiv(100, lineH, 26);
    int comboW = rw - lw - 4 - editBtnW - 4;
    HWND hProfileCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | vis | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      x + lw + 4, y, comboW, lineH + 10 * lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_WT_PROFILE, GetModuleHandle(NULL), NULL);
    if (hProfileCombo && hFont) SendMessage(hProfileCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    for (int i = 0; i < (int)p->m_windowTitleProfiles.size(); i++) {
      const wchar_t* name = p->m_windowTitleProfiles[i].szName;
      SendMessageW(hProfileCombo, CB_ADDSTRING, 0, (LPARAM)(name[0] ? name : L"(unnamed)"));
    }
    if (!p->m_windowTitleProfiles.empty())
      SendMessageW(hProfileCombo, CB_SETCURSEL, p->m_nActiveWindowTitleProfile, 0);
    TrackControl(hProfileCombo);

    HWND hEditBtn = CreateBtn(hw, L"Edit", IDC_MW_WT_EDIT_PARSER,
      x + lw + 4 + comboW + 4, y, editBtnW, lineH, hFont, showWT);
    TrackControl(hEditBtn);
    y += lineH + 2;

    HWND hPreview = CreateWindowExW(0, L"STATIC", L"",
      WS_CHILD | vis | SS_LEFT | SS_NOPREFIX,
      x + lw + 4, y, rw - lw - 4, lineH, hw,
      (HMENU)(INT_PTR)IDC_MW_SONG_WT_PREVIEW, GetModuleHandle(NULL), NULL);
    if (hPreview && hFont) SendMessage(hPreview, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hPreview);
    if (showWT && !p->m_windowTitleProfiles.empty()) {
      int idx = p->m_nActiveWindowTitleProfile;
      if (idx >= 0 && idx < (int)p->m_windowTitleProfiles.size() && p->m_windowTitleProfiles[idx].szWindowRegex[0])
        UpdateWindowTitlePreview(hw, p->m_windowTitleProfiles[idx].szWindowRegex);
    }
  }
  y += lineH + gap;

  // ── Checkboxes ──
  TrackControl(CreateCheck(hw, L"Song Title Animations",   IDC_MW_SONG_TITLE,        x, y, rw, lineH, hFont, p->m_bSongTitleAnims)); y += lineH + 2;
  TrackControl(CreateCheck(hw, L"Overlay Notifications",   IDC_MW_SONG_OVERLAY,      x, y, rw, lineH, hFont, p->m_bSongInfoOverlay)); y += lineH + 2;
  TrackControl(CreateCheck(hw, L"Change Preset w/ Song",   IDC_MW_CHANGE_SONG,       x, y, rw, lineH, hFont, p->m_ChangePresetWithSong)); y += lineH + 2;
  TrackControl(CreateCheck(hw, L"Show Cover Art",          IDC_MW_SONG_COVER,        x, y, rw, lineH, hFont, p->m_DisplayCover)); y += lineH + 2;
  TrackControl(CreateCheck(hw, L"Always Show Track Info",  IDC_MW_SONG_ALWAYS_SHOW,  x, y, rw, lineH, hFont, p->m_bSongInfoAlwaysShow)); y += lineH + 2;

  // ── Display Corner combo ──
  TrackControl(CreateLabel(hw, L"Display Corner:", x, y, lw, lineH, hFont));
  {
    HWND hCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
      x + lw + 4, y, rw - lw - 4, lineH + 5 * lineH, hw, (HMENU)(INT_PTR)IDC_MW_SONG_CORNER,
      GetModuleHandle(NULL), NULL);
    if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Top-Left");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Top-Right");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Bottom-Left");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Bottom-Right");
    SendMessageW(hCombo, CB_SETCURSEL, p->m_SongInfoDisplayCorner - 1, 0);
    TrackControl(hCombo);
  }
  y += lineH + 2;

  // ── Display Seconds + Show Now ──
  TrackControl(CreateLabel(hw, L"Display Seconds:", x, y, lw, lineH, hFont));
  swprintf(buf, 64, L"%.1f", p->m_SongInfoDisplaySeconds);
  TrackControl(CreateEdit(hw, buf, IDC_MW_SONG_DISPLAY_SEC, x + lw + 4, y, 100, lineH, hFont));
  {
    int showBtnX = x + lw + 4 + 100 + 8;
    int showBtnW = MulDiv(80, lineH, 26);
    TrackControl(CreateBtn(hw, L"Show Now", IDC_MW_SONG_SHOW_NOW, showBtnX, y, showBtnW, lineH, hFont));
  }
}

//----------------------------------------------------------------------
// DoCommand — handles WM_COMMAND from Song Info controls
//----------------------------------------------------------------------

LRESULT SongInfoWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
  Engine* p = m_pEngine;

  // Show Now button
  if (id == IDC_MW_SONG_SHOW_NOW && code == BN_CLICKED) {
    mdropdx12.doPollExplicit = true;
    return 0;
  }

  // Edit Parser button
  if (id == IDC_MW_WT_EDIT_PARSER && code == BN_CLICKED) {
    p->OpenWindowTitleParserPopup(hWnd);
    return 0;
  }

  // Source combo
  if (id == IDC_MW_SONG_SOURCE && code == CBN_SELCHANGE) {
    int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel <= 2) {
      p->m_nTrackInfoSource = sel;
      WritePrivateProfileIntW(sel, L"TrackInfoSource", p->GetConfigIniFile(), L"Milkwave");
      bool showWT = (sel == Engine::TRACK_SOURCE_WINDOW);
      int sw = showWT ? SW_SHOW : SW_HIDE;
      HWND h;
      if ((h = GetDlgItem(hWnd, IDC_MW_SONG_WT_LABEL)))     ShowWindow(h, sw);
      if ((h = GetDlgItem(hWnd, IDC_MW_WT_PROFILE)))         ShowWindow(h, sw);
      if ((h = GetDlgItem(hWnd, IDC_MW_WT_EDIT_PARSER)))     ShowWindow(h, sw);
      if ((h = GetDlgItem(hWnd, IDC_MW_SONG_WT_PREVIEW)))    ShowWindow(h, sw);
      if (showWT && !p->m_windowTitleProfiles.empty()) {
        int idx = p->m_nActiveWindowTitleProfile;
        if (idx >= 0 && idx < (int)p->m_windowTitleProfiles.size() && p->m_windowTitleProfiles[idx].szWindowRegex[0])
          UpdateWindowTitlePreview(hWnd, p->m_windowTitleProfiles[idx].szWindowRegex);
      }
    }
    return 0;
  }

  // Profile combo
  if (id == IDC_MW_WT_PROFILE && code == CBN_SELCHANGE) {
    int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)p->m_windowTitleProfiles.size()) {
      p->m_nActiveWindowTitleProfile = sel;
      WritePrivateProfileIntW(sel, L"WindowTitleProfile", p->GetConfigIniFile(), L"Milkwave");
      if (p->m_windowTitleProfiles[sel].szWindowRegex[0])
        UpdateWindowTitlePreview(hWnd, p->m_windowTitleProfiles[sel].szWindowRegex);
      else
        SetDlgItemTextW(hWnd, IDC_MW_SONG_WT_PREVIEW, L"(no window match regex)");
    }
    return 0;
  }

  // Display Corner combo
  if (id == IDC_MW_SONG_CORNER && code == CBN_SELCHANGE) {
    int sel = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel <= 3) {
      p->m_SongInfoDisplayCorner = sel + 1;
      WritePrivateProfileIntW(p->m_SongInfoDisplayCorner, L"SongInfoDisplayCorner", p->GetConfigIniFile(), L"Milkwave");
    }
    return 0;
  }

  // Display Seconds edit
  if (id == IDC_MW_SONG_DISPLAY_SEC && code == EN_CHANGE) {
    wchar_t buf[32];
    GetWindowTextW((HWND)lParam, buf, 32);
    p->m_SongInfoDisplaySeconds = (float)_wtof(buf);
    if (p->m_SongInfoDisplaySeconds < 0.5f) p->m_SongInfoDisplaySeconds = 0.5f;
    if (p->m_SongInfoDisplaySeconds > 60.0f) p->m_SongInfoDisplaySeconds = 60.0f;
    { wchar_t sb[32]; swprintf(sb, 32, L"%.1f", p->m_SongInfoDisplaySeconds);
      WritePrivateProfileStringW(L"Milkwave", L"SongInfoDisplaySeconds", sb, p->GetConfigIniFile()); }
    return 0;
  }

  // Checkbox handlers (base class auto-toggles checkbox state before DoCommand)
  if (code == BN_CLICKED) {
    HWND hCtrl = (HWND)lParam;
    bool bIsCheckbox = (bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox");
    if (bIsCheckbox) {
      BOOL bChecked = IsChecked(id);

      switch (id) {
      case IDC_MW_SONG_TITLE:
        p->m_bSongTitleAnims = bChecked;
        p->SaveSettingToINI(SET_SONG_TITLE_ANIMS);
        return 0;
      case IDC_MW_CHANGE_SONG:
        p->m_ChangePresetWithSong = bChecked;
        p->SaveSettingToINI(SET_CHANGE_WITH_SONG);
        return 0;
      case IDC_MW_SONG_OVERLAY:
        p->m_bSongInfoOverlay = bChecked;
        WritePrivateProfileIntW(bChecked, L"SongInfoOverlay", p->GetConfigIniFile(), L"Milkwave");
        return 0;
      case IDC_MW_SONG_COVER:
        p->m_DisplayCover = bChecked;
        WritePrivateProfileIntW(bChecked, L"DisplayCover", p->GetConfigIniFile(), L"Milkwave");
        return 0;
      case IDC_MW_SONG_ALWAYS_SHOW:
        p->m_bSongInfoAlwaysShow = bChecked;
        WritePrivateProfileIntW(bChecked, L"SongInfoAlwaysShow", p->GetConfigIniFile(), L"Milkwave");
        if (!bChecked) {
          p->ClearErrors(ERR_MSG_BOTTOM_EXTRA_1);
          p->ClearErrors(ERR_MSG_BOTTOM_EXTRA_2);
          p->ClearErrors(ERR_MSG_BOTTOM_EXTRA_3);
        } else {
          mdropdx12.doPollExplicit = true;
        }
        return 0;
      }
    }
  }

  return -1;  // not handled
}

} // namespace mdrop
