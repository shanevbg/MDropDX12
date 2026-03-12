// engine_presets_ui.cpp — Presets ToolWindow implementation.
//
// Standalone window for preset browsing, navigation, and settings.
// Extracted from Settings page 0 (General tab).

#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <CommCtrl.h>
#include <windowsx.h>

// Shorten a directory path to "...\parent\leaf\" for compact display.
static void ShortenDirectoryPath(const wchar_t* szFullPath, wchar_t* szOut, int nMaxChars) {
    if (!szFullPath || !szFullPath[0]) { szOut[0] = 0; return; }
    int len = (int)wcslen(szFullPath);
    int end = len;
    if (end > 0 && szFullPath[end - 1] == L'\\') end--;
    int slash1 = -1, slash2 = -1;
    for (int i = end - 1; i >= 0; i--) {
        if (szFullPath[i] == L'\\') {
            if (slash1 < 0) slash1 = i;
            else { slash2 = i; break; }
        }
    }
    if (slash2 < 0 || len < 35) { lstrcpynW(szOut, szFullPath, nMaxChars); return; }
    swprintf(szOut, nMaxChars, L"...%s", szFullPath + slash2);
}

namespace mdrop {

// ─── Constructor ────────────────────────────────────────────────────────

PresetsWindow::PresetsWindow(Engine* pEngine)
    : ToolWindow(pEngine, 620, 680)
{
}

// ─── Helpers ────────────────────────────────────────────────────────────

void PresetsWindow::RefreshPresetList() {
    if (!m_hList) return;
    Engine* p = m_pEngine;
    SendMessage(m_hList, WM_SETREDRAW, FALSE, 0);
    SendMessage(m_hList, LB_RESETCONTENT, 0, 0);
    bool bTagFilter = !p->m_szTagFilter.empty();
    for (int i = 0; i < p->m_nPresets; i++) {
        if (p->m_presets[i].szFilename.empty()) continue;

        // Get annotation (needed for both tag filter and display prefix)
        // For tag filter, extract the filename-only part for annotation lookup
        const wchar_t* fn = p->m_presets[i].szFilename.c_str();
        const wchar_t* fnOnly = wcsrchr(fn, L'\\');
        fnOnly = fnOnly ? (fnOnly + 1) : fn;
        PresetAnnotation* a = p->GetAnnotation(fnOnly);

        // Apply tag filter
        if (bTagFilter) {
            bool hasTag = false;
            if (a) {
                for (auto& t : a->tags) {
                    if (_wcsicmp(t.c_str(), p->m_szTagFilter.c_str()) == 0) {
                        hasTag = true;
                        break;
                    }
                }
            }
            if (!hasTag) continue;
        }

        // For list-loaded presets with absolute paths, show relative to preset dir for readability
        const wchar_t* displayFn = fn;
        if (fn[0] && fn[1] == L':') {
            // Absolute path — try to make relative to preset dir
            int dirLen = lstrlenW(p->m_szPresetDir);
            if (dirLen > 0 && _wcsnicmp(fn, p->m_szPresetDir, dirLen) == 0) {
                displayFn = fn + dirLen;
            } else {
                // Different base dir — show relative from "presets\" or just the filename portion
                const wchar_t* presetsDir = wcsstr(fn, L"\\presets\\");
                if (presetsDir) {
                    displayFn = presetsDir + 1;  // skip leading backslash, show "presets\..."
                } else {
                    displayFn = fnOnly;  // just the filename
                }
            }
        }

        // Add annotation prefix indicators
        int lbIdx;
        if (a && a->flags) {
            std::wstring display;
            if (a->flags & PFLAG_FAVORITE) display += L"\x2605";  // filled star
            if (a->flags & (PFLAG_ERROR | PFLAG_BROKEN)) display += L"\x26A0";  // warning
            if (a->flags & PFLAG_SKIP) display += L"\x2298";  // circled dash
            display += L" ";
            display += displayFn;
            lbIdx = (int)SendMessageW(m_hList, LB_ADDSTRING, 0, (LPARAM)display.c_str());
        } else {
            lbIdx = (int)SendMessageW(m_hList, LB_ADDSTRING, 0, (LPARAM)displayFn);
        }
        // Store actual preset index so selection works correctly when filtered
        if (lbIdx >= 0)
            SendMessageW(m_hList, LB_SETITEMDATA, lbIdx, (LPARAM)i);
    }
    // Select current preset in listbox (find by stored data)
    int nItems = (int)SendMessage(m_hList, LB_GETCOUNT, 0, 0);
    for (int j = 0; j < nItems; j++) {
        if ((int)SendMessage(m_hList, LB_GETITEMDATA, j, 0) == p->m_nCurrentPreset) {
            SendMessage(m_hList, LB_SETCURSEL, j, 0);
            break;
        }
    }
    SendMessage(m_hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m_hList, NULL, TRUE);
}

void PresetsWindow::SyncListBoxToCurrentPreset() {
    if (!m_hList || m_pEngine->m_nCurrentPreset < 0) return;
    int nItems = (int)SendMessage(m_hList, LB_GETCOUNT, 0, 0);
    for (int j = 0; j < nItems; j++) {
        if ((int)SendMessage(m_hList, LB_GETITEMDATA, j, 0) == m_pEngine->m_nCurrentPreset) {
            SendMessage(m_hList, LB_SETCURSEL, j, 0);
            return;
        }
    }
}

void PresetsWindow::UpdateCurrentPresetDisplay() {
    if (m_hCurrentPreset)
        SetWindowTextW(m_hCurrentPreset, m_pEngine->m_szCurrentPresetFile);
}

void PresetsWindow::UpdatePresetDirDisplay() {
    if (!m_hPresetDir) return;
    wchar_t szShort[MAX_PATH];
    ShortenDirectoryPath(m_pEngine->m_szPresetDir, szShort, MAX_PATH);
    SetWindowTextW(m_hPresetDir, szShort);
}

// ─── Build Controls ─────────────────────────────────────────────────────

void PresetsWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    auto L = BuildBaseControls();
    int x = L.x, y = L.y, rw = L.rw;
    HFONT hFont = GetFont();
    m_nTopY = y;

    int lineH = GetLineHeight();
    int gap = 4;
    int lw = MulDiv(130, lineH, 26); // label width

    // Current Preset + Browse
    m_hLblPreset = CreateLabel(hw, L"Current Preset:", x, y, lw, lineH, hFont);
    m_hCurrentPreset = CreateEdit(hw, p->m_szCurrentPresetFile, IDC_MW_CURRENT_PRESET, x + lw + 4, y, rw - lw - 74, lineH, hFont, ES_READONLY);
    m_hBrowsePreset = CreateBtn(hw, L"Browse", IDC_MW_BROWSE_PRESET, x + rw - 65, y, 65, lineH, hFont);
    y += lineH + gap;

    // Preset Dir + Browse
    {
        wchar_t szShort[MAX_PATH];
        ShortenDirectoryPath(p->m_szPresetDir, szShort, MAX_PATH);
        m_hLblDir = CreateLabel(hw, L"Preset Dir:", x, y, lw, lineH, hFont);
        m_hPresetDir = CreateEdit(hw, szShort, IDC_MW_PRESET_DIR, x + lw + 4, y, rw - lw - 84, lineH, hFont, ES_READONLY);
        m_hBrowseDir = CreateBtn(hw, L"Browse...", IDC_MW_BROWSE_DIR, x + rw - 75, y, 75, lineH, hFont);
    }
    y += lineH + gap;

    // Tag filter row (above listbox — filters what's shown)
    {
        int tagLblW = MulDiv(30, lineH, 26);
        int importW = MulDiv(55, lineH, 26);
        int tagComboW = rw - tagLblW - 4 - importW - 6;
        m_hLblTag = CreateLabel(hw, L"Tag:", x, y + 2, tagLblW, lineH, hFont);
        m_hTagFilter = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            x + tagLblW + 4, y, tagComboW, lineH * 8, hw,
            (HMENU)(INT_PTR)IDC_MW_PRESETS_TAG_FILTER, GetModuleHandle(NULL), NULL);
        if (m_hTagFilter && hFont) SendMessageW(m_hTagFilter, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(m_hTagFilter, CB_ADDSTRING, 0, (LPARAM)L"All Tags");
        std::vector<std::wstring> allTags;
        p->CollectAllTags(allTags);
        int selIdx = 0;
        for (int t = 0; t < (int)allTags.size(); t++) {
            SendMessageW(m_hTagFilter, CB_ADDSTRING, 0, (LPARAM)allTags[t].c_str());
            if (!p->m_szTagFilter.empty() && p->m_szTagFilter == allTags[t])
                selIdx = t + 1;
        }
        SendMessageW(m_hTagFilter, CB_SETCURSEL, selIdx, 0);
        m_hBtnImportTags = CreateBtn(hw, L"Import", IDC_MW_PRESETS_IMPORT_TAGS, x + rw - importW, y, importW, lineH + 4, hFont);
    }
    y += lineH + 4 + gap;

    // Preset Listbox
    {
        int listH = 10 * lineH;
        m_hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_PRESET_LIST, GetModuleHandle(NULL), NULL);
        if (m_hList && hFont) SendMessage(m_hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        RefreshPresetList();
        m_nLastPresetCount = p->m_nPresets;  // prevent timer from redundantly refreshing
    }
    y += 10 * lineH + gap;

    // Nav row: < > scissors  Up  Into  Subdirs  Filter
    {
        int btnW = MulDiv(40, lineH, 26);
        int btnGap = 6;
        m_hBtnPrev = CreateBtn(hw, L"\x25C4", IDC_MW_PRESET_PREV, x, y, btnW, lineH + 4, hFont);
        m_hBtnNext = CreateBtn(hw, L"\x25BA", IDC_MW_PRESET_NEXT, x + btnW + btnGap, y, btnW, lineH + 4, hFont);
        m_hBtnCopy = CreateBtn(hw, L"\x2702", IDC_MW_PRESET_COPY, x + 2 * (btnW + btnGap), y, btnW, lineH + 4, hFont);
        int dirBtnX = x + 3 * (btnW + btnGap) + 10;
        int dirBtnW = MulDiv(55, lineH, 26);
        m_hBtnUp = CreateBtn(hw, L"\x25B2 Up", IDC_MW_PRESET_UP, dirBtnX, y, dirBtnW, lineH + 4, hFont);
        m_hBtnInto = CreateBtn(hw, L"\x25BC Into", IDC_MW_PRESET_INTO, dirBtnX + dirBtnW + btnGap, y, dirBtnW, lineH + 4, hFont);
        // Filter button (right-aligned)
        const wchar_t* filterLabels[] = { L"All", L".milk", L".milk2", L".milk3" };
        int filterW = MulDiv(50, lineH, 26);
        m_hBtnFilter = CreateBtn(hw, filterLabels[p->m_nPresetFilter], IDC_MW_PRESET_FILTER, x + rw - filterW, y, filterW, lineH + 4, hFont);
    }
    y += lineH + 4 + gap;

    // List row: Subdirs  List: [combo]  Save  Clear
    {
        int subdirW = MulDiv(90, lineH, 26);
        m_hBtnSubdir = CreateBtn(hw, p->m_nSubdirMode ? L"Subdirs: On" : L"Subdirs: Off", IDC_MW_PRESETS_SUBDIR, x, y, subdirW, lineH + 4, hFont);

        int listLblW = MulDiv(30, lineH, 26);
        int btnW = MulDiv(45, lineH, 26);
        int comboX = x + subdirW + 8 + listLblW + 4;
        int comboW = rw - subdirW - 8 - listLblW - 4 - 2 * (btnW + 6);
        m_hLblListName = CreateLabel(hw, L"List:", x + subdirW + 8, y + 2, listLblW, lineH, hFont);
        m_hPresetListCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            comboX, y, comboW, lineH * 8, hw,
            (HMENU)(INT_PTR)IDC_MW_PRESETS_LIST_COMBO, GetModuleHandle(NULL), NULL);
        if (m_hPresetListCombo && hFont) SendMessage(m_hPresetListCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(m_hPresetListCombo, CB_ADDSTRING, 0, (LPARAM)L"(none)");
        std::vector<std::wstring> listNames;
        p->EnumPresetLists(listNames);
        int selIdx = 0;
        for (int i = 0; i < (int)listNames.size(); i++) {
            SendMessageW(m_hPresetListCombo, CB_ADDSTRING, 0, (LPARAM)listNames[i].c_str());
            if (!p->m_szActivePresetList.empty() && p->m_szActivePresetList == listNames[i])
                selIdx = i + 1;
        }
        SendMessage(m_hPresetListCombo, CB_SETCURSEL, selIdx, 0);
        int btnX = x + rw - 2 * btnW - 6;
        m_hBtnListSave = CreateBtn(hw, L"Save", IDC_MW_PRESETS_LIST_SAVE, btnX, y, btnW, lineH + 4, hFont);
        m_hBtnListClear = CreateBtn(hw, L"Clear", IDC_MW_PRESETS_LIST_CLEAR, btnX + btnW + 6, y, btnW, lineH + 4, hFont);
    }
    y += lineH + 4 + gap + 4;

    // Preset settings
    wchar_t buf[64];
    int slw = MulDiv(200, lineH, 26); // wider label for settings rows
    m_hLblSens = CreateLabel(hw, L"Audio Gain (-1=Auto, -2=Legacy):", x, y, slw, lineH, hFont);
    swprintf(buf, 64, L"%g", (double)p->m_fAudioSensitivity);
    m_hEditSens = CreateEdit(hw, buf, IDC_MW_AUDIO_SENS, x + slw + 4, y, 60, lineH, hFont);
    y += lineH + gap;

    m_hLblBlend = CreateLabel(hw, L"Blend Time (s):", x, y, slw, lineH, hFont);
    swprintf(buf, 64, L"%.1f", p->m_fBlendTimeAuto);
    m_hEditBlend = CreateEdit(hw, buf, IDC_MW_BLEND_TIME, x + slw + 4, y, 60, lineH, hFont);
    y += lineH + gap;

    m_hLblTime = CreateLabel(hw, L"Time Between (s, 0=off):", x, y, slw, lineH, hFont);
    swprintf(buf, 64, L"%.0f", p->m_fTimeBetweenPresets);
    m_hEditTime = CreateEdit(hw, buf, IDC_MW_TIME_BETWEEN, x + slw + 4, y, 60, lineH, hFont);
    y += lineH + gap + 4;

    m_hChkHardCuts = CreateCheck(hw, L"Hard Cuts Disabled",      IDC_MW_HARD_CUTS,   x, y, rw, lineH, hFont, p->m_bHardCutsDisabled); y += lineH + 2;
    m_hChkLock     = CreateCheck(hw, L"Preset Lock on Startup",  IDC_MW_PRESET_LOCK, x, y, rw, lineH, hFont, p->m_bPresetLockOnAtStartup); y += lineH + 2;
    m_hChkSeq      = CreateCheck(hw, L"Sequential Preset Order", IDC_MW_SEQ_ORDER,   x, y, rw, lineH, hFont, p->m_bSequentialPresetOrder);
    y += lineH + gap + 4;

    // Startup preset mode combo
    {
        int startLblW = MulDiv(60, lineH, 26);
        m_hLblStartup = CreateLabel(hw, L"Startup:", x, y, startLblW, lineH, hFont);
        HWND hStartup = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            x + startLblW + 4, y, rw - startLblW - 4, lineH * 5, hw,
            (HMENU)(INT_PTR)IDC_MW_PRESETS_STARTUP, GetModuleHandle(NULL), NULL);
        m_hStartupCombo = hStartup;
        if (hStartup && hFont) SendMessage(hStartup, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hStartup, CB_ADDSTRING, 0, (LPARAM)L"Random");
        SendMessageW(hStartup, CB_ADDSTRING, 0, (LPARAM)L"Current Preset");
        SendMessageW(hStartup, CB_ADDSTRING, 0, (LPARAM)L"Last Used");
        // Determine current mode
        int mode = 0; // random
        if (p->m_bEnablePresetStartup) {
            mode = p->m_bEnablePresetStartupSavingOnClose ? 2 : 1; // last used or current
        }
        SendMessage(hStartup, CB_SETCURSEL, mode, 0);
    }
    y += lineH + gap + 4;

    // Start a timer to sync current preset display
    SetTimer(hw, 1, 500, NULL);
}

// ─── Layout on Resize ───────────────────────────────────────────────────

void PresetsWindow::OnResize() {
    LayoutControls();
}

void PresetsWindow::LayoutControls() {
    if (!m_hWnd || !m_hList) return;
    RECT rc;
    GetClientRect(m_hWnd, &rc);

    int x = 8;
    int y = m_nTopY;
    int rw = rc.right - 16;
    int lineH = GetLineHeight();
    int gap = 4;
    int lw = MulDiv(130, lineH, 26);
    int browseBtnW = 65;
    int browseDirW = 75;

    // Current Preset row
    MoveWindow(m_hLblPreset, x, y, lw, lineH, TRUE);
    MoveWindow(m_hCurrentPreset, x + lw + 4, y, rw - lw - browseBtnW - 9, lineH, TRUE);
    MoveWindow(m_hBrowsePreset, x + rw - browseBtnW, y, browseBtnW, lineH, TRUE);
    y += lineH + gap;

    // Preset Dir row
    MoveWindow(m_hLblDir, x, y, lw, lineH, TRUE);
    MoveWindow(m_hPresetDir, x + lw + 4, y, rw - lw - browseDirW - 9, lineH, TRUE);
    MoveWindow(m_hBrowseDir, x + rw - browseDirW, y, browseDirW, lineH, TRUE);
    y += lineH + gap;

    // Tag filter row
    {
        int tagLblW = MulDiv(30, lineH, 26);
        int importW = MulDiv(55, lineH, 26);
        int tagComboW = rw - tagLblW - 4 - importW - 6;
        if (m_hLblTag) MoveWindow(m_hLblTag, x, y + 2, tagLblW, lineH, TRUE);
        if (m_hTagFilter) MoveWindow(m_hTagFilter, x + tagLblW + 4, y, tagComboW, lineH * 8, TRUE);
        if (m_hBtnImportTags) MoveWindow(m_hBtnImportTags, x + rw - importW, y, importW, lineH + 4, TRUE);
    }
    y += lineH + 4 + gap;

    // Calculate listbox height: fill remaining space minus rows below
    int belowList = (lineH + 4 + gap)             // nav row
        + (lineH + 4 + gap + 4)                   // subdirs/list row
        + 3 * (lineH + gap) + 4                   // 3 edit rows + extra gap
        + 3 * (lineH + 2) + gap + 4               // 3 checkboxes + gap
        + (lineH + gap)                            // startup combo row
        + 8;                                       // bottom margin
    int listH = rc.bottom - y - belowList;
    if (listH < 3 * lineH) listH = 3 * lineH;

    MoveWindow(m_hList, x, y, rw, listH, TRUE);
    y += listH + gap;

    // Nav buttons
    {
        int btnW = MulDiv(40, lineH, 26);
        int btnGap = 6;
        MoveWindow(m_hBtnPrev, x, y, btnW, lineH + 4, TRUE);
        MoveWindow(m_hBtnNext, x + btnW + btnGap, y, btnW, lineH + 4, TRUE);
        MoveWindow(m_hBtnCopy, x + 2 * (btnW + btnGap), y, btnW, lineH + 4, TRUE);
        int dirBtnX = x + 3 * (btnW + btnGap) + 10;
        int dirBtnW = MulDiv(55, lineH, 26);
        MoveWindow(m_hBtnUp, dirBtnX, y, dirBtnW, lineH + 4, TRUE);
        MoveWindow(m_hBtnInto, dirBtnX + dirBtnW + btnGap, y, dirBtnW, lineH + 4, TRUE);
        int filterW = MulDiv(50, lineH, 26);
        MoveWindow(m_hBtnFilter, x + rw - filterW, y, filterW, lineH + 4, TRUE);
    }
    y += lineH + 4 + gap;

    // Subdirs + List row
    {
        int subdirW = MulDiv(90, lineH, 26);
        int listLblW = MulDiv(30, lineH, 26);
        int btnW = MulDiv(45, lineH, 26);
        int comboX = x + subdirW + 8 + listLblW + 4;
        int comboW = rw - subdirW - 8 - listLblW - 4 - 2 * (btnW + 6);
        if (m_hBtnSubdir) MoveWindow(m_hBtnSubdir, x, y, subdirW, lineH + 4, TRUE);
        if (m_hLblListName) MoveWindow(m_hLblListName, x + subdirW + 8, y + 2, listLblW, lineH, TRUE);
        if (m_hPresetListCombo) MoveWindow(m_hPresetListCombo, comboX, y, comboW, lineH * 8, TRUE);
        int btnX = x + rw - 2 * btnW - 6;
        if (m_hBtnListSave) MoveWindow(m_hBtnListSave, btnX, y, btnW, lineH + 4, TRUE);
        if (m_hBtnListClear) MoveWindow(m_hBtnListClear, btnX + btnW + 6, y, btnW, lineH + 4, TRUE);
    }
    y += lineH + 4 + gap + 4;

    // Settings rows
    int slw = MulDiv(200, lineH, 26);
    int editW = 60;
    MoveWindow(m_hLblSens, x, y, slw, lineH, TRUE);
    MoveWindow(m_hEditSens, x + slw + 4, y, editW, lineH, TRUE);
    y += lineH + gap;

    MoveWindow(m_hLblBlend, x, y, slw, lineH, TRUE);
    MoveWindow(m_hEditBlend, x + slw + 4, y, editW, lineH, TRUE);
    y += lineH + gap;

    MoveWindow(m_hLblTime, x, y, slw, lineH, TRUE);
    MoveWindow(m_hEditTime, x + slw + 4, y, editW, lineH, TRUE);
    y += lineH + gap + 4;

    // Checkboxes
    MoveWindow(m_hChkHardCuts, x, y, rw, lineH, TRUE);
    y += lineH + 2;
    MoveWindow(m_hChkLock, x, y, rw, lineH, TRUE);
    y += lineH + 2;
    MoveWindow(m_hChkSeq, x, y, rw, lineH, TRUE);
    y += lineH + gap + 4;

    // Startup combo
    {
        int startLblW = MulDiv(60, lineH, 26);
        if (m_hLblStartup) MoveWindow(m_hLblStartup, x, y, startLblW, lineH, TRUE);
        if (m_hStartupCombo) MoveWindow(m_hStartupCombo, x + startLblW + 4, y, rw - startLblW - 4, lineH * 5, TRUE);
    }
}

// ─── Commands ───────────────────────────────────────────────────────────

LRESULT PresetsWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    Engine* p = m_pEngine;

    // ── Browse Directory ──
    if (id == IDC_MW_BROWSE_DIR && code == BN_CLICKED) {
        p->OpenFolderPickerForPresetDir(hWnd);
        UpdatePresetDirDisplay();
        RefreshPresetList();
        return 0;
    }

    // ── Browse Preset File ──
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
            UpdateCurrentPresetDisplay();
        }
        return 0;
    }

    // ── Preset List Selection ──
    if (id == IDC_MW_PRESET_LIST && code == LBN_SELCHANGE) {
        int lbSel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
        int sel = (lbSel >= 0) ? (int)SendMessage((HWND)lParam, LB_GETITEMDATA, lbSel, 0) : -1;
        if (sel >= 0 && sel < p->m_nPresets) {
            if (p->m_presets[sel].szFilename.c_str()[0] == L'*') {
                if (wcscmp(p->m_presets[sel].szFilename.c_str(), L"*..") == 0)
                    NavigatePresetDirUp();
                else
                    NavigatePresetDirInto(sel);
                return 0;
            }
            p->m_nCurrentPreset = sel;
            wchar_t szFile[MAX_PATH];
            p->BuildPresetPath(sel, szFile, MAX_PATH);
            p->LoadPreset(szFile, p->m_fBlendTimeUser);
            UpdateCurrentPresetDisplay();
        }
        return 0;
    }

    // ── Preset List Double-Click ──
    if (id == IDC_MW_PRESET_LIST && code == LBN_DBLCLK) {
        int lbSel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
        int sel = (lbSel >= 0) ? (int)SendMessage((HWND)lParam, LB_GETITEMDATA, lbSel, 0) : -1;
        if (sel >= 0 && sel < p->m_nPresets) {
            if (p->m_presets[sel].szFilename.c_str()[0] == L'*') {
                if (wcscmp(p->m_presets[sel].szFilename.c_str(), L"*..") == 0)
                    NavigatePresetDirUp();
                else
                    NavigatePresetDirInto(sel);
            } else {
                p->m_nCurrentPreset = sel;
                wchar_t szFile[MAX_PATH];
                p->BuildPresetPath(sel, szFile, MAX_PATH);
                p->LoadPreset(szFile, p->m_fBlendTimeUser);
                UpdateCurrentPresetDisplay();
            }
        }
        return 0;
    }

    // ── Nav: Prev ──
    if (id == IDC_MW_PRESET_PREV && code == BN_CLICKED) {
        p->PrevPreset(p->m_fBlendTimeUser);
        SyncListBoxToCurrentPreset();
        UpdateCurrentPresetDisplay();
        return 0;
    }

    // ── Nav: Next ──
    if (id == IDC_MW_PRESET_NEXT && code == BN_CLICKED) {
        p->NextPreset(p->m_fBlendTimeUser);
        SyncListBoxToCurrentPreset();
        UpdateCurrentPresetDisplay();
        return 0;
    }

    // ── Nav: Copy Path ──
    if (id == IDC_MW_PRESET_COPY && code == BN_CLICKED) {
        int lbSel = m_hList ? (int)SendMessage(m_hList, LB_GETCURSEL, 0, 0) : -1;
        int sel = (lbSel >= 0) ? (int)SendMessage(m_hList, LB_GETITEMDATA, lbSel, 0) : -1;
        if (sel >= 0 && sel < p->m_nPresets) {
            wchar_t szFile[MAX_PATH];
            p->BuildPresetPath(sel, szFile, MAX_PATH);
            if (OpenClipboard(hWnd)) {
                EmptyClipboard();
                size_t len = (wcslen(szFile) + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                if (hMem) {
                    memcpy(GlobalLock(hMem), szFile, len);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                CloseClipboard();
            }
        }
        return 0;
    }

    // ── Nav: Up (parent directory) ──
    if (id == IDC_MW_PRESET_UP && code == BN_CLICKED) {
        NavigatePresetDirUp();
        return 0;
    }

    // ── Nav: Into (subdirectory) ──
    if (id == IDC_MW_PRESET_INTO && code == BN_CLICKED) {
        int lbSel = m_hList ? (int)SendMessage(m_hList, LB_GETCURSEL, 0, 0) : -1;
        int sel = (lbSel >= 0) ? (int)SendMessage(m_hList, LB_GETITEMDATA, lbSel, 0) : -1;
        NavigatePresetDirInto(sel);
        return 0;
    }

    // ── Preset Filter ──
    if (id == IDC_MW_PRESET_FILTER && code == BN_CLICKED) {
        p->m_nPresetFilter = (p->m_nPresetFilter + 1) % 4;
        const wchar_t* filterLabels[] = { L"All", L".milk", L".milk2", L".milk3" };
        SetWindowTextW((HWND)lParam, filterLabels[p->m_nPresetFilter]);
        p->UpdatePresetList(true, true);
        RefreshPresetList();
        return 0;
    }

    // ── Subdir Mode ──
    if (id == IDC_MW_PRESETS_SUBDIR && code == BN_CLICKED) {
        p->m_nSubdirMode = p->m_nSubdirMode ? 0 : 1;
        SetWindowTextW((HWND)lParam, p->m_nSubdirMode ? L"Subdirs: On" : L"Subdirs: Off");
        p->m_bRecursivePresets = (p->m_nSubdirMode == 1);
        p->UpdatePresetList(true, true);
        RefreshPresetList();
        return 0;
    }

    // ── Tag Filter ──
    if (id == IDC_MW_PRESETS_TAG_FILTER && code == CBN_SELCHANGE) {
        int sel = (int)SendMessageW(m_hTagFilter, CB_GETCURSEL, 0, 0);
        if (sel <= 0) {
            p->m_szTagFilter.clear();
        } else {
            wchar_t buf[256] = {};
            SendMessageW(m_hTagFilter, CB_GETLBTEXT, sel, (LPARAM)buf);
            p->m_szTagFilter = buf;
        }
        RefreshPresetList();
        return 0;
    }

    // ── Import MWR Tags ──
    if (id == IDC_MW_PRESETS_IMPORT_TAGS && code == BN_CLICKED) {
        // Try auto-detect first: look for tags-remote.json in preset dir and base dir
        wchar_t szPath[MAX_PATH] = {};
        bool found = false;

        // Check preset directory
        swprintf(szPath, MAX_PATH, L"%stags-remote.json", p->m_szPresetDir);
        if (GetFileAttributesW(szPath) != INVALID_FILE_ATTRIBUTES)
            found = true;

        // Check base directory
        if (!found) {
            swprintf(szPath, MAX_PATH, L"%stags-remote.json", p->m_szBaseDir);
            if (GetFileAttributesW(szPath) != INVALID_FILE_ATTRIBUTES)
                found = true;
        }

        // If not found, open file dialog
        if (!found) {
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"MWR Tags JSON (tags-remote.json)\0tags-remote.json\0JSON Files (*.json)\0*.json\0All Files\0*.*\0";
            ofn.lpstrFile = szPath;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Import Tags from Milkwave Remote";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn))
                return 0;  // cancelled
        }

        int nUpdated = p->ImportMWRTags(szPath);
        wchar_t msg[256];
        swprintf(msg, 256, L"Imported tags for %d presets from:\n%s", nUpdated, szPath);
        MessageBoxW(hWnd, msg, L"Import Tags", MB_OK | MB_ICONINFORMATION);

        // Refresh tag filter dropdown
        if (m_hTagFilter && nUpdated > 0) {
            int prevSel = (int)SendMessageW(m_hTagFilter, CB_GETCURSEL, 0, 0);
            wchar_t prevTag[256] = {};
            if (prevSel > 0) SendMessageW(m_hTagFilter, CB_GETLBTEXT, prevSel, (LPARAM)prevTag);

            SendMessageW(m_hTagFilter, CB_RESETCONTENT, 0, 0);
            SendMessageW(m_hTagFilter, CB_ADDSTRING, 0, (LPARAM)L"All Tags");
            std::vector<std::wstring> allTags;
            p->CollectAllTags(allTags);
            int newSel = 0;
            for (int t = 0; t < (int)allTags.size(); t++) {
                SendMessageW(m_hTagFilter, CB_ADDSTRING, 0, (LPARAM)allTags[t].c_str());
                if (prevTag[0] && allTags[t] == prevTag) newSel = t + 1;
            }
            SendMessageW(m_hTagFilter, CB_SETCURSEL, newSel, 0);
        }
        return 0;
    }

    // ── Preset List: Load ──
    if (id == IDC_MW_PRESETS_LIST_COMBO && code == CBN_SELCHANGE) {
        int sel = (int)SendMessage(m_hPresetListCombo, CB_GETCURSEL, 0, 0);
        if (sel <= 0) {
            // "(none)" selected — revert to directory scan
            p->m_szActivePresetList.clear();
            WritePrivateProfileStringW(L"Settings", L"szActivePresetList", L"", p->GetConfigIniFile());
            p->m_bRecursivePresets = (p->m_nSubdirMode == 1);
            p->UpdatePresetList(true, true);
            RefreshPresetList();
        } else {
            wchar_t listName[256] = {};
            SendMessageW(m_hPresetListCombo, CB_GETLBTEXT, sel, (LPARAM)listName);
            wchar_t szDir[MAX_PATH];
            p->GetPresetListDir(szDir, MAX_PATH);
            wchar_t szPath[MAX_PATH];
            swprintf(szPath, MAX_PATH, L"%s%s.txt", szDir, listName);
            if (p->LoadPresetList(szPath)) {
                WritePrivateProfileStringW(L"Settings", L"szActivePresetList", p->m_szActivePresetList.c_str(), p->GetConfigIniFile());
                RefreshPresetList();
            }
        }
        return 0;
    }

    // ── Preset List: Save ──
    if (id == IDC_MW_PRESETS_LIST_SAVE && code == BN_CLICKED) {
        // Prompt for list name
        wchar_t szName[128] = {};
        if (!p->m_szActivePresetList.empty())
            lstrcpynW(szName, p->m_szActivePresetList.c_str(), 128);

        // Simple input dialog via GetSaveFileName
        wchar_t szDir[MAX_PATH];
        p->GetPresetListDir(szDir, MAX_PATH);
        CreateDirectoryW(szDir, NULL);

        wchar_t szFile[MAX_PATH] = {};
        if (szName[0]) swprintf(szFile, MAX_PATH, L"%s", szName);

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hWnd;
        ofn.lpstrFilter = L"Preset Lists (*.txt)\0*.txt\0All Files\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = szDir;
        ofn.lpstrTitle = L"Save Preset List";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        ofn.lpstrDefExt = L"txt";

        if (GetSaveFileNameW(&ofn)) {
            // Extract name from path
            const wchar_t* pName = wcsrchr(szFile, L'\\');
            pName = pName ? (pName + 1) : szFile;
            std::wstring name = pName;
            size_t dot = name.rfind(L'.');
            if (dot != std::wstring::npos) name = name.substr(0, dot);

            p->SavePresetList(name.c_str());

            // Refresh combo
            SendMessage(m_hPresetListCombo, CB_RESETCONTENT, 0, 0);
            SendMessageW(m_hPresetListCombo, CB_ADDSTRING, 0, (LPARAM)L"(none)");
            std::vector<std::wstring> listNames;
            p->EnumPresetLists(listNames);
            int newSel = 0;
            for (int i = 0; i < (int)listNames.size(); i++) {
                SendMessageW(m_hPresetListCombo, CB_ADDSTRING, 0, (LPARAM)listNames[i].c_str());
                if (listNames[i] == name) newSel = i + 1;
            }
            SendMessage(m_hPresetListCombo, CB_SETCURSEL, newSel, 0);
        }
        return 0;
    }

    // ── Preset List: Clear ──
    if (id == IDC_MW_PRESETS_LIST_CLEAR && code == BN_CLICKED) {
        p->m_szActivePresetList.clear();
        WritePrivateProfileStringW(L"Settings", L"szActivePresetList", L"", p->GetConfigIniFile());
        p->m_bRecursivePresets = (p->m_nSubdirMode == 1);
        p->UpdatePresetList(true, true);
        RefreshPresetList();
        if (m_hPresetListCombo)
            SendMessage(m_hPresetListCombo, CB_SETCURSEL, 0, 0);
        return 0;
    }

    // ── Preset Settings (edit fields — apply on focus loss) ──
    if (code == EN_KILLFOCUS) {
        wchar_t buf[64] = {};
        GetWindowTextW((HWND)lParam, buf, 64);
        switch (id) {
        case IDC_MW_AUDIO_SENS: {
            p->m_fAudioSensitivity = (float)_wtof(buf);
            if (p->m_fAudioSensitivity < -2) p->m_fAudioSensitivity = -2;
            if (p->m_fAudioSensitivity > 256) p->m_fAudioSensitivity = 256;
            extern bool mdropdx12_audio_adaptive;
            extern float mdropdx12_audio_sensitivity;
            if (p->m_fAudioSensitivity <= -1.0f) {
                // -1 = improved adaptive, -2 = legacy adaptive
                mdropdx12_audio_adaptive = true;
                mdropdx12_audio_sensitivity = p->m_fAudioSensitivity;
            } else {
                mdropdx12_audio_adaptive = false;
                if (p->m_fAudioSensitivity < 0.5f) p->m_fAudioSensitivity = 0.5f;
                mdropdx12_audio_sensitivity = p->m_fAudioSensitivity;
            }
            p->SaveSettingToINI(SET_AUDIO_SENSITIVITY);
            return 0;
        }
        case IDC_MW_BLEND_TIME:
            p->m_fBlendTimeAuto = (float)_wtof(buf);
            if (p->m_fBlendTimeAuto < 0.1f) p->m_fBlendTimeAuto = 0.1f;
            if (p->m_fBlendTimeAuto > 10) p->m_fBlendTimeAuto = 10;
            p->SaveSettingToINI(SET_BLEND_TIME);
            return 0;
        case IDC_MW_TIME_BETWEEN:
            p->m_fTimeBetweenPresets = (float)_wtof(buf);
            if (p->m_fTimeBetweenPresets < 0) p->m_fTimeBetweenPresets = 0;
            if (p->m_fTimeBetweenPresets > 99999) p->m_fTimeBetweenPresets = 99999;
            p->SaveSettingToINI(SET_TIME_BETWEEN);
            return 0;
        }
    }

    // ── Checkboxes ──
    if (code == BN_CLICKED) {
        bool bChecked = IsChecked(id);
        switch (id) {
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
        }
    }

    // ── Annotation context menu commands ──
    if (code == 0 && m_nContextSel >= 0 && m_nContextSel < p->m_nPresets) {
        const wchar_t* fnFull = p->m_presets[m_nContextSel].szFilename.c_str();
        const wchar_t* fnSlash = wcsrchr(fnFull, L'\\');
        const wchar_t* fn = fnSlash ? (fnSlash + 1) : fnFull;
        if (id == IDC_MW_ANNOT_FAV) {
            PresetAnnotation* a = p->GetAnnotation(fn);
            p->SetPresetFlag(fn, PFLAG_FAVORITE, !(a && (a->flags & PFLAG_FAVORITE)));
            RefreshPresetList();
            return 0;
        }
        if (id == IDC_MW_ANNOT_SKIP) {
            PresetAnnotation* a = p->GetAnnotation(fn);
            p->SetPresetFlag(fn, PFLAG_SKIP, !(a && (a->flags & PFLAG_SKIP)));
            RefreshPresetList();
            return 0;
        }
        if (id == IDC_MW_ANNOT_BROKEN) {
            PresetAnnotation* a = p->GetAnnotation(fn);
            p->SetPresetFlag(fn, PFLAG_BROKEN, !(a && (a->flags & PFLAG_BROKEN)));
            RefreshPresetList();
            return 0;
        }
        if (id == IDC_MW_ANNOT_NOTE) {
            PresetAnnotation* a = p->GetAnnotation(fn);
            wchar_t szNote[1024] = {};
            if (a && !a->notes.empty())
                wcsncpy_s(szNote, a->notes.c_str(), _TRUNCATE);
            // Simple input dialog
            if (ShowNoteDialog(hWnd, fn, szNote, _countof(szNote))) {
                p->SetPresetNote(fn, szNote);
                p->AddNotification(L"Note saved");
            }
            return 0;
        }
        if (id == IDC_MW_ANNOT_VIEWERR) {
            PresetAnnotation* a = p->GetAnnotation(fn);
            if (a && !a->errorText.empty())
                MessageBoxW(hWnd, a->errorText.c_str(), L"Shader Error", MB_OK | MB_ICONWARNING);
            else
                MessageBoxW(hWnd, L"No error recorded.", L"Shader Error", MB_OK);
            return 0;
        }
        if (id == IDC_MW_ANNOT_CLEAR) {
            PresetAnnotation* a = p->GetAnnotation(fn);
            if (a) {
                a->flags = 0;
                a->errorText.clear();
                a->notes.clear();
                a->rating = 0;
                p->m_bAnnotationsDirty = true;
                p->SavePresetAnnotations();
                RefreshPresetList();
            }
            return 0;
        }
        if (id >= IDC_MW_ANNOT_RATE_BASE && id <= IDC_MW_ANNOT_RATE_BASE + 5) {
            int rating = id - IDC_MW_ANNOT_RATE_BASE;
            PresetAnnotation* a = p->GetAnnotation(fn, true);
            if (a) {
                a->rating = rating;
                p->m_bAnnotationsDirty = true;
                p->SavePresetAnnotations();
            }
            return 0;
        }
    }

    // Open Annotations window
    if (id == IDC_MW_OPEN_ANNOTATIONS && code == BN_CLICKED) {
        p->OpenAnnotationsWindow();
        return 0;
    }

    // ── Startup mode combo ──
    if (id == IDC_MW_PRESETS_STARTUP && code == CBN_SELCHANGE) {
        int mode = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
        wchar_t* pIni = p->GetConfigIniFile();
        switch (mode) {
        case 0: // Random
            p->m_bEnablePresetStartup = false;
            p->m_bEnablePresetStartupSavingOnClose = false;
            break;
        case 1: // Current Preset
            p->m_bEnablePresetStartup = true;
            p->m_bEnablePresetStartupSavingOnClose = false;
            // Set current preset as startup
            wcscpy_s(p->m_szPresetStartup, p->m_szCurrentPresetFile);
            WritePrivateProfileStringW(L"Settings", L"szPresetStartup", p->m_szPresetStartup, pIni);
            break;
        case 2: // Last Used
            p->m_bEnablePresetStartup = true;
            p->m_bEnablePresetStartupSavingOnClose = true;
            // Set current preset as startup (will be updated on close)
            wcscpy_s(p->m_szPresetStartup, p->m_szCurrentPresetFile);
            WritePrivateProfileStringW(L"Settings", L"szPresetStartup", p->m_szPresetStartup, pIni);
            break;
        }
        WritePrivateProfileIntW(p->m_bEnablePresetStartup, L"bEnablePresetStartup", pIni, L"Settings");
        WritePrivateProfileIntW(p->m_bEnablePresetStartupSavingOnClose, L"bEnablePresetStartupSavingOnClose", pIni, L"Settings");
        return 0;
    }

    return -1;
}

// ─── Messages (timer for syncing current preset) ────────────────────────

LRESULT PresetsWindow::DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == 1) {
        // Detect when background preset scan completes and refresh listbox
        if (m_pEngine->m_nPresets != m_nLastPresetCount && m_pEngine->m_bPresetListReady) {
            m_nLastPresetCount = m_pEngine->m_nPresets;
            RefreshPresetList();
        }
        // Sync current preset display with engine state
        UpdateCurrentPresetDisplay();
        // Sync listbox selection
        if (m_hList && m_pEngine->m_nCurrentPreset >= 0) {
            // Find listbox item with matching preset index (stored in item data)
            int lbCur = (int)SendMessage(m_hList, LB_GETCURSEL, 0, 0);
            int curData = (lbCur >= 0) ? (int)SendMessage(m_hList, LB_GETITEMDATA, lbCur, 0) : -1;
            if (curData != m_pEngine->m_nCurrentPreset) {
                int nItems = (int)SendMessage(m_hList, LB_GETCOUNT, 0, 0);
                for (int j = 0; j < nItems; j++) {
                    if ((int)SendMessage(m_hList, LB_GETITEMDATA, j, 0) == m_pEngine->m_nCurrentPreset) {
                        SendMessage(m_hList, LB_SETCURSEL, j, 0);
                        break;
                    }
                }
            }
        }
        return 0;
    }

    return -1;
}

// ─── Context Menu ────────────────────────────────────────────────────────

LRESULT PresetsWindow::DoContextMenu(HWND hWnd, int x, int y) {
    if (!m_hList) return -1;

    Engine* p = m_pEngine;
    int lbSel = (int)SendMessage(m_hList, LB_GETCURSEL, 0, 0);
    int sel = (lbSel >= 0) ? (int)SendMessage(m_hList, LB_GETITEMDATA, lbSel, 0) : -1;
    if (sel < 0 || sel >= p->m_nPresets) return -1;
    if (p->m_presets[sel].szFilename.c_str()[0] == L'*') return -1; // directory

    const wchar_t* fn = p->m_presets[sel].szFilename.c_str();
    const wchar_t* fnOnly = wcsrchr(fn, L'\\');
    fnOnly = fnOnly ? (fnOnly + 1) : fn;
    PresetAnnotation* a = p->GetAnnotation(fnOnly);
    bool isFav    = a && (a->flags & PFLAG_FAVORITE);
    bool isSkip   = a && (a->flags & PFLAG_SKIP);
    bool isBroken = a && (a->flags & PFLAG_BROKEN);
    bool hasError = a && (a->flags & PFLAG_ERROR);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | (isFav ? MF_CHECKED : 0), IDC_MW_ANNOT_FAV, L"Favorite");
    AppendMenuW(hMenu, MF_STRING | (isSkip ? MF_CHECKED : 0), IDC_MW_ANNOT_SKIP, L"Skip");
    AppendMenuW(hMenu, MF_STRING | (isBroken ? MF_CHECKED : 0), IDC_MW_ANNOT_BROKEN, L"Broken");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDC_MW_ANNOT_NOTE, L"Add Note...");
    AppendMenuW(hMenu, MF_STRING | (hasError ? 0 : MF_GRAYED), IDC_MW_ANNOT_VIEWERR, L"View Error...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDC_MW_ANNOT_CLEAR, L"Clear All Flags");

    // Rating submenu
    HMENU hRateMenu = CreatePopupMenu();
    int curRating = a ? a->rating : 0;
    for (int r = 5; r >= 0; r--) {
        wchar_t label[32];
        if (r == 0) wcscpy_s(label, L"Unrated");
        else {
            label[0] = 0;
            for (int s = 0; s < r; s++) wcscat_s(label, L"\x2605");
        }
        AppendMenuW(hRateMenu, MF_STRING | (curRating == r ? MF_CHECKED : 0), IDC_MW_ANNOT_RATE_BASE + r, label);
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hRateMenu, L"Rating");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDC_MW_OPEN_ANNOTATIONS, L"Open Annotations...");

    POINT pt = { x, y };
    if (pt.x == -1 && pt.y == -1) { // keyboard context menu
        RECT rc;
        SendMessage(m_hList, LB_GETITEMRECT, lbSel, (LPARAM)&rc);
        pt.x = rc.left + 20;
        pt.y = rc.bottom;
        ClientToScreen(m_hList, &pt);
    }

    // Store selected index for command handler
    m_nContextSel = sel;
    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
    return 0;
}

// ─── Note Dialog ────────────────────────────────────────────────────────

struct NoteDialogData {
    const wchar_t* presetName;
    wchar_t* szNote;
    int nMaxNote;
    bool accepted;
};

static INT_PTR CALLBACK NoteDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    NoteDialogData* d = (NoteDialogData*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG:
        d = (NoteDialogData*)lParam;
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)d);
        SetWindowTextW(hDlg, d->presetName);
        SetDlgItemTextW(hDlg, 101, d->szNote);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            GetDlgItemTextW(hDlg, 101, d->szNote, d->nMaxNote);
            d->accepted = true;
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            d->accepted = false;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        d->accepted = false;
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool PresetsWindow::ShowNoteDialog(HWND hParent, const wchar_t* presetName, wchar_t* szNote, int nMaxNote) {
    // Build a tiny dialog template in memory
    // Layout: multiline edit (id 101), OK button (IDOK), Cancel button (IDCANCEL)
    #pragma pack(push, 4)
    struct {
        DLGTEMPLATE dlg;
        WORD menu, cls, title;
        // Items follow
    } tmpl = {};
    #pragma pack(pop)

    // Instead of a template, just use CreateDialogIndirect or a simple MessageBox approach
    // Simplest: create a popup window manually
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"#32770", presetName,
        WS_VISIBLE | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 200,
        hParent, NULL, GetModuleHandle(NULL), NULL);
    if (!hDlg) return false;

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", szNote,
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
        10, 10, 370, 100, hDlg, (HMENU)101, GetModuleHandle(NULL), NULL);
    SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

    HWND hOK = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
        220, 120, 75, 28, hDlg, (HMENU)IDOK, GetModuleHandle(NULL), NULL);
    SendMessage(hOK, WM_SETFONT, (WPARAM)hFont, TRUE);

    HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        305, 120, 75, 28, hDlg, (HMENU)IDCANCEL, GetModuleHandle(NULL), NULL);
    SendMessage(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Center on parent
    RECT rcParent, rcDlg;
    GetWindowRect(hParent, &rcParent);
    GetWindowRect(hDlg, &rcDlg);
    int cx = (rcParent.left + rcParent.right) / 2 - (rcDlg.right - rcDlg.left) / 2;
    int cy = (rcParent.top + rcParent.bottom) / 2 - (rcDlg.bottom - rcDlg.top) / 2;
    SetWindowPos(hDlg, NULL, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    // Run modal message loop
    EnableWindow(hParent, FALSE);
    bool accepted = false;
    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            break;
        }
        if (m.message == WM_COMMAND && m.hwnd == hDlg) {
            if (LOWORD(m.wParam) == IDOK) {
                GetWindowTextW(hEdit, szNote, nMaxNote);
                accepted = true;
                break;
            }
            if (LOWORD(m.wParam) == IDCANCEL) break;
        }
        if (!IsDialogMessage(hDlg, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    EnableWindow(hParent, TRUE);
    DestroyWindow(hDlg);
    SetForegroundWindow(hParent);
    return accepted;
}

// ─── Directory Navigation ───────────────────────────────────────────────

void PresetsWindow::NavigatePresetDirUp() {
    Engine* p = m_pEngine;
    wchar_t* pDir = p->GetPresetDir();

    // Strip trailing backslash, find previous one, truncate after it
    int dirLen = lstrlenW(pDir);
    if (dirLen > 0 && pDir[dirLen - 1] == L'\\')
        pDir[dirLen - 1] = 0;
    wchar_t* p2 = wcsrchr(pDir, L'\\');
    if (p2 && p2 > pDir) {
        *(p2 + 1) = 0;
    } else {
        lstrcatW(pDir, L"\\");
        return;
    }
    WritePrivateProfileStringW(L"Settings", L"szPresetDir", pDir, p->GetConfigIniFile());
    p->UpdatePresetList(true, true, false);
    p->m_nCurrentPreset = -1;

    UpdatePresetDirDisplay();
    RefreshPresetList();
}

void PresetsWindow::NavigatePresetDirInto(int sel) {
    Engine* p = m_pEngine;
    if (sel < 0 || sel >= p->m_nPresets) return;
    if (p->m_presets[sel].szFilename.c_str()[0] != L'*') return;

    wchar_t* pDir = p->GetPresetDir();
    lstrcatW(pDir, &p->m_presets[sel].szFilename.c_str()[1]);
    lstrcatW(pDir, L"\\");
    WritePrivateProfileStringW(L"Settings", L"szPresetDir", pDir, p->GetConfigIniFile());
    p->UpdatePresetList(true, true, false);
    p->m_nCurrentPreset = -1;

    UpdatePresetDirDisplay();
    RefreshPresetList();
}

// ─── Destroy ────────────────────────────────────────────────────────────

void PresetsWindow::DoDestroy() {
    KillTimer(m_hWnd, 1);
}

} // namespace mdrop
