// engine_presets_ui.cpp — Presets ToolWindow implementation.
//
// Standalone window for preset browsing, navigation, and settings.
// Extracted from Settings page 0 (General tab).

#include "engine.h"
#include "utility.h"
#include <CommCtrl.h>

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

// ─── Open / Close (Engine methods) ──────────────────────────────────────

void Engine::OpenPresetsWindow() {
    if (!m_presetsWindow)
        m_presetsWindow = std::make_unique<PresetsWindow>(this);
    m_presetsWindow->Open();
}

void Engine::ClosePresetsWindow() {
    if (m_presetsWindow)
        m_presetsWindow->Close();
}

// ─── Constructor ────────────────────────────────────────────────────────

PresetsWindow::PresetsWindow(Engine* pEngine)
    : ToolWindow(pEngine, 620, 680)
{
}

// ─── Helpers ────────────────────────────────────────────────────────────

void PresetsWindow::RefreshPresetList() {
    if (!m_hList) return;
    Engine* p = m_pEngine;
    SendMessage(m_hList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < p->m_nPresets; i++) {
        if (p->m_presets[i].szFilename.empty()) continue;
        SendMessageW(m_hList, LB_ADDSTRING, 0, (LPARAM)p->m_presets[i].szFilename.c_str());
    }
    if (p->m_nCurrentPreset >= 0 && p->m_nCurrentPreset < p->m_nPresets)
        SendMessage(m_hList, LB_SETCURSEL, p->m_nCurrentPreset, 0);
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

    // Preset Listbox (fills available space — calculated in LayoutControls)
    {
        int listH = 10 * lineH;
        m_hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_PRESET_LIST, GetModuleHandle(NULL), NULL);
        if (m_hList && hFont) SendMessage(m_hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        RefreshPresetList();
    }
    y += 10 * lineH + gap;

    // Nav buttons
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
    y += lineH + 4 + gap + 4;

    // Preset settings
    wchar_t buf[64];
    int slw = MulDiv(200, lineH, 26); // wider label for settings rows
    m_hLblSens = CreateLabel(hw, L"Audio Gain (-1=Auto):", x, y, slw, lineH, hFont);
    swprintf(buf, 64, L"%g", (double)p->m_fAudioSensitivity);
    m_hEditSens = CreateEdit(hw, buf, IDC_MW_AUDIO_SENS, x + slw + 4, y, 60, lineH, hFont);
    y += lineH + gap;

    m_hLblBlend = CreateLabel(hw, L"Blend Time (s):", x, y, slw, lineH, hFont);
    swprintf(buf, 64, L"%.1f", p->m_fBlendTimeAuto);
    m_hEditBlend = CreateEdit(hw, buf, IDC_MW_BLEND_TIME, x + slw + 4, y, 60, lineH, hFont);
    y += lineH + gap;

    m_hLblTime = CreateLabel(hw, L"Time Between (s):", x, y, slw, lineH, hFont);
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

    // Calculate listbox height: fill remaining space minus nav + settings below
    int belowList = (lineH + 4 + gap + 4) // nav row
        + 3 * (lineH + gap) + 4           // 3 edit rows + extra gap
        + 3 * (lineH + 2) + gap + 4       // 3 checkboxes + gap
        + (lineH + gap)                   // startup combo row
        + 8;                               // bottom margin
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
    y += lineH + 4 + gap + 4;

    // Settings rows (wider label column for "Audio Sensitivity (-1=Auto):")
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
        p->OpenFolderPickerForPresetDir();
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
        int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < p->m_nPresets) {
            if (p->m_presets[sel].szFilename.c_str()[0] == L'*')
                return 0; // directory entry — skip
            p->m_nCurrentPreset = sel;
            wchar_t szFile[MAX_PATH];
            swprintf(szFile, MAX_PATH, L"%s%s", p->m_szPresetDir, p->m_presets[sel].szFilename.c_str());
            p->LoadPreset(szFile, p->m_fBlendTimeUser);
            UpdateCurrentPresetDisplay();
        }
        return 0;
    }

    // ── Preset List Double-Click ──
    if (id == IDC_MW_PRESET_LIST && code == LBN_DBLCLK) {
        int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < p->m_nPresets) {
            if (p->m_presets[sel].szFilename.c_str()[0] == L'*') {
                if (wcscmp(p->m_presets[sel].szFilename.c_str(), L"*..") == 0)
                    NavigatePresetDirUp();
                else
                    NavigatePresetDirInto(sel);
            } else {
                p->m_nCurrentPreset = sel;
                wchar_t szFile[MAX_PATH];
                swprintf(szFile, MAX_PATH, L"%s%s", p->m_szPresetDir, p->m_presets[sel].szFilename.c_str());
                p->LoadPreset(szFile, p->m_fBlendTimeUser);
                UpdateCurrentPresetDisplay();
            }
        }
        return 0;
    }

    // ── Nav: Prev ──
    if (id == IDC_MW_PRESET_PREV && code == BN_CLICKED) {
        p->PrevPreset(p->m_fBlendTimeUser);
        if (m_hList && p->m_nCurrentPreset >= 0)
            SendMessage(m_hList, LB_SETCURSEL, p->m_nCurrentPreset, 0);
        UpdateCurrentPresetDisplay();
        return 0;
    }

    // ── Nav: Next ──
    if (id == IDC_MW_PRESET_NEXT && code == BN_CLICKED) {
        p->NextPreset(p->m_fBlendTimeUser);
        if (m_hList && p->m_nCurrentPreset >= 0)
            SendMessage(m_hList, LB_SETCURSEL, p->m_nCurrentPreset, 0);
        UpdateCurrentPresetDisplay();
        return 0;
    }

    // ── Nav: Copy Path ──
    if (id == IDC_MW_PRESET_COPY && code == BN_CLICKED) {
        int sel = m_hList ? (int)SendMessage(m_hList, LB_GETCURSEL, 0, 0) : -1;
        if (sel >= 0 && sel < p->m_nPresets) {
            wchar_t szFile[MAX_PATH];
            swprintf(szFile, MAX_PATH, L"%s%s", p->m_szPresetDir, p->m_presets[sel].szFilename.c_str());
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
        int sel = m_hList ? (int)SendMessage(m_hList, LB_GETCURSEL, 0, 0) : -1;
        NavigatePresetDirInto(sel);
        return 0;
    }

    // ── Preset Filter ──
    if (id == IDC_MW_PRESET_FILTER && code == BN_CLICKED) {
        p->m_nPresetFilter = (p->m_nPresetFilter + 1) % 4;
        const wchar_t* filterLabels[] = { L"All", L".milk", L".milk2", L".milk3" };
        SetWindowTextW((HWND)lParam, filterLabels[p->m_nPresetFilter]);
        p->UpdatePresetList(false, true);
        RefreshPresetList();
        return 0;
    }

    // ── Preset Settings (edit fields — apply on focus loss) ──
    if (code == EN_KILLFOCUS) {
        wchar_t buf[64] = {};
        GetWindowTextW((HWND)lParam, buf, 64);
        switch (id) {
        case IDC_MW_AUDIO_SENS: {
            p->m_fAudioSensitivity = (float)_wtof(buf);
            if (p->m_fAudioSensitivity < -1) p->m_fAudioSensitivity = -1;
            if (p->m_fAudioSensitivity > 256) p->m_fAudioSensitivity = 256;
            extern bool mdropdx12_audio_adaptive;
            extern float mdropdx12_audio_sensitivity;
            if (p->m_fAudioSensitivity == -1.0f) {
                mdropdx12_audio_adaptive = true;
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
            if (p->m_fTimeBetweenPresets < 1) p->m_fTimeBetweenPresets = 1;
            if (p->m_fTimeBetweenPresets > 300) p->m_fTimeBetweenPresets = 300;
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
        // Sync current preset display with engine state
        UpdateCurrentPresetDisplay();
        // Sync listbox selection
        if (m_hList && m_pEngine->m_nCurrentPreset >= 0) {
            int cur = (int)SendMessage(m_hList, LB_GETCURSEL, 0, 0);
            if (cur != m_pEngine->m_nCurrentPreset)
                SendMessage(m_hList, LB_SETCURSEL, m_pEngine->m_nCurrentPreset, 0);
        }
        return 0;
    }
    return -1;
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
    p->UpdatePresetList(false, true, false);
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
    p->UpdatePresetList(false, true, false);
    p->m_nCurrentPreset = -1;

    UpdatePresetDirDisplay();
    RefreshPresetList();
}

// ─── Destroy ────────────────────────────────────────────────────────────

void PresetsWindow::DoDestroy() {
    KillTimer(m_hWnd, 1);
}

} // namespace mdrop
