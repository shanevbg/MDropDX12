// engine_presets_ui.cpp — Presets ToolWindow implementation.
//
// Standalone window for preset browsing, navigation, and settings.
// Extracted from Settings page 0 (General tab).

#include "engine.h"
#include "engine_helpers.h"
#include "format_to.h"
#include "utility.h"
#include <CommCtrl.h>
#include <windowsx.h>
#include "config_store.h"

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

// CopyTextToClipboard lives in tool_window.cpp — the shader error viewer needs
// it too, and two copies would drift.

// ─── Constructor ────────────────────────────────────────────────────────

PresetsWindow::PresetsWindow(Engine* pEngine)
    : ToolWindow(pEngine, 620, 680)
{
}

// ─── Helpers ────────────────────────────────────────────────────────────

// The browser only ever needs an annotation's flag bits, and it needs them
// without keeping the entry: every one of these sites is followed by a
// SendMessage or a menu build, and holding a PresetAnnotation* across either
// is the bug #11 exists to close. One place that copies the bits out.
static uint32_t AnnotFlagsOf(Engine* p, const wchar_t* fn) {
    uint32_t flags = 0;
    p->WithAnnotation(fn, /*create=*/false,
                      [&](PresetAnnotation& a) { flags = a.flags; });
    return flags;
}

void PresetsWindow::RefreshPresetList() {
    if (!m_hList) return;
    Engine* p = m_pEngine;
    SendMessage(m_hList, WM_SETREDRAW, FALSE, 0);
    SendMessage(m_hList, LB_RESETCONTENT, 0, 0);
    bool bTagFilter = !p->m_szTagFilter.empty();
    for (int i = 0; i < p->m_nPresets; i++) {
        if (p->PresetNameAt(i).empty()) continue;

        // Get annotation (needed for both tag filter and display prefix)
        // For tag filter, extract the filename-only part for annotation lookup
        const std::wstring name_i = p->PresetNameAt(i);
        const wchar_t* fn = name_i.c_str();
        const wchar_t* fnOnly = wcsrchr(fn, L'\\');
        fnOnly = fnOnly ? (fnOnly + 1) : fn;
        // Copy the two things this row needs out from under the guard and let
        // go of it (#11). Every LB_ADDSTRING below is a SendMessage, so it
        // pumps -- a PresetAnnotation* held across one is held across arbitrary
        // re-entrant work on this thread, and a reload on the IPC thread frees
        // it. (The m_presets pointer above has the same problem and a different
        // owner; that half of #11 is not yet scheduled.)
        uint32_t annotFlags = 0;
        bool hasTag = false;
        p->WithAnnotation(fnOnly, /*create=*/false, [&](PresetAnnotation& a) {
            annotFlags = a.flags;
            if (bTagFilter) {
                for (auto& t : a.tags) {
                    if (_wcsicmp(t.c_str(), p->m_szTagFilter.c_str()) == 0) {
                        hasTag = true;
                        break;
                    }
                }
            }
        });

        // Apply tag filter
        if (bTagFilter && !hasTag) continue;

        // Directory rows are stored as "*name" ("*.." for the parent). The '*'
        // is the storage marker the navigation handlers test for — show the
        // user "[name]" instead. m_presets already sorts directories first and
        // the listbox is unsorted, so this changes nothing but the text.
        if (fn[0] == L'*') {
            std::wstring dirDisplay = L"[";
            dirDisplay += (fn + 1);
            dirDisplay += L"]";
            int dirIdx = (int)SendMessageW(m_hList, LB_ADDSTRING, 0, (LPARAM)dirDisplay.c_str());
            if (dirIdx >= 0)
                SendMessageW(m_hList, LB_SETITEMDATA, dirIdx, (LPARAM)i);
            continue;
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
        if (annotFlags) {
            std::wstring display;
            if (annotFlags & PFLAG_FAVORITE) display += L"\x2605";  // filled star
            if (annotFlags & (PFLAG_ERROR | PFLAG_BROKEN)) display += L"\x26A0";  // warning
            if (annotFlags & PFLAG_CANVAS) display += L"\x25A3";  // canvas-sensitive
            if (annotFlags & PFLAG_SKIP) display += L"\x2298";  // circled dash
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
    // The NAME, not the path, and only when it has changed -- this runs from
    // the WM_TIMER in DoMessage about once a second. See SetTextIfChanged.
    SetTextIfChanged(m_hCurrentPreset, FileNameOnly(m_pEngine->m_szCurrentPresetFile));
}

void PresetsWindow::UpdatePresetDirDisplay() {
    if (!m_hPresetDir) return;
    wchar_t szShort[MAX_PATH];
    ShortenDirectoryPath(m_pEngine->m_szPresetDir, szShort, MAX_PATH);
    SetTextIfChanged(m_hPresetDir, szShort);
}

// ─── Icon buttons ───────────────────────────────────────────────────────
//
// Browse, Browse... and Import were 65 and 75 PIXELS wide while every other
// button on this window scales with the font (MulDiv(N, lineH, 26)). At a
// larger font their labels no longer fitted and were clipped mid-word inside
// the button -- Shane's screenshot showed "Browse" and "Browse.." sitting next
// to a full-size "Import". Widening them would have taken the space from the
// path fields beside them, which are already the cramped part of the row.
//
// A glyph is one character at any size, so it stays square, scales with the
// font, and hands the difference back to the field. The label moves to a
// tooltip rather than being lost: an icon nobody can name is worse than a
// clipped word.
//
// Width for one of them. Square-ish against the row, and the same number is
// used when the window is rebuilt and when it is resized -- the two paths held
// separate copies (65 here, browseBtnW there) which is how they could disagree.
static int IconBtnW(int lineH) { return MulDiv(34, lineH, 26); }

// A themed glyph button with a tooltip. DrawOwnerButton picks the font up from
// the control with WM_GETFONT and centres whatever text it holds, so a glyph
// button needs no drawing code of its own -- only the right font.
HWND PresetsWindow::IconBtn(HWND hw, const wchar_t* glyph, int id,
                            int x, int y, int w, int h, const wchar_t* tip) {
    if (!m_hIconBtnFont) {
        // 72% of the row, not the full (lineH - 4) the Copy button used to ask
        // for. An MDL2 glyph fills its em box far more than a letter does, so a
        // glyph matched to the text size reads as much taller than the words
        // beside it and makes the row look cramped -- Shane: "can you make the
        // font less vertically high". Same correction the title row already
        // carries, where the glyphs run at 81% of the pin beside them.
        const int glyphH = MulDiv(GetLineHeight() - 4, 72, 100);
        m_hIconBtnFont = CreateFontW(-glyphH, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe MDL2 Assets");
    }
    HWND hBtn = CreateBtn(hw, glyph, id, x, y, w, h,
                          m_hIconBtnFont ? m_hIconBtnFont : GetFont());
    AttachTip(hBtn, tip);
    return hBtn;
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
    const int iconW = IconBtnW(lineH);
    m_hCurrentPreset = CreateReadonlyField(hw, p->m_szCurrentPresetFile, IDC_MW_CURRENT_PRESET, x + lw + 4, y, rw - lw - iconW - 9, lineH, hFont);
    // \xE8E5 OpenFile. Written as an ASCII escape, not as the glyph itself:
    // this file has no BOM and the project sets no /utf-8, so a literal
    // glyph is read in the system codepage and arrives mangled.
    m_hBrowsePreset = IconBtn(hw, L"\xE8E5", IDC_MW_BROWSE_PRESET, x + rw - iconW, y, iconW, lineH,
        L"Browse for a preset file...");
    y += lineH + gap;

    // Preset Dir + Browse
    {
        wchar_t szShort[MAX_PATH];
        ShortenDirectoryPath(p->m_szPresetDir, szShort, MAX_PATH);
        m_hLblDir = CreateLabel(hw, L"Preset Dir:", x, y, lw, lineH, hFont);
        m_hPresetDir = CreateReadonlyField(hw, szShort, IDC_MW_PRESET_DIR, x + lw + 4, y, rw - lw - iconW - 9, lineH, hFont);
        m_hBrowseDir = IconBtn(hw, L"\xE8B7", IDC_MW_BROWSE_DIR, x + rw - iconW, y, iconW, lineH,
            L"Browse for a preset folder...");
    }
    y += lineH + gap;

    // Tag filter row (above listbox — filters what's shown)
    {
        int tagLblW = MulDiv(30, lineH, 26);
        int importW = IconBtnW(lineH);
        int tagComboW = rw - tagLblW - 4 - importW - 6;
        m_hLblTag = CreateLabel(hw, L"Tag:", x, y + 2, tagLblW, lineH, hFont);
        m_hTagFilter = CreateCombo(hw, IDC_MW_PRESETS_TAG_FILTER, x + tagLblW + 4, y,
          tagComboW, lineH * 8, hFont);
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
        m_hBtnImportTags = IconBtn(hw, L"\xE8B5", IDC_MW_PRESETS_IMPORT_TAGS, x + rw - importW, y, importW, lineH + 4,
            L"Import tags from a file...");
    }
    y += lineH + 4 + gap;

    // Preset Listbox
    {
        int listH = 10 * lineH;
        m_hList = CreateListBox(hw, IDC_MW_PRESET_LIST, x, y, rw, listH, hFont,
          WS_VSCROLL | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT);
        RefreshPresetList();
        m_nLastPresetCount = p->m_nPresets;  // prevent timer from redundantly refreshing
    }
    y += 10 * lineH + gap;

    // Nav row: < > clipboard  Up  Into  Subdirs  Filter
    {
        int btnW = MulDiv(40, lineH, 26);
        int btnGap = 6;
        m_hBtnPrev = CreateBtn(hw, L"\x25C4", IDC_MW_PRESET_PREV, x, y, btnW, lineH + 4, hFont);
        m_hBtnNext = CreateBtn(hw, L"\x25BA", IDC_MW_PRESET_NEXT, x + btnW + btnGap, y, btnW, lineH + 4, hFont);
        // Segoe MDL2 Copy (E8C8) — scissors (U+2702) looked like Cut
        m_hBtnCopy = IconBtn(hw, L"\xE8C8", IDC_MW_PRESET_COPY,
            x + 2 * (btnW + btnGap), y, btnW, lineH + 4, L"Copy current preset name");
        // Edit: open the Preset Editor on the selected preset.
        int editBtnW = MulDiv(50, lineH, 26);
        m_hBtnEdit = CreateBtn(hw, L"Edit", IDC_MW_PRESET_EDIT,
            x + 3 * (btnW + btnGap), y, editBtnW, lineH + 4, hFont);
        AttachTip(m_hBtnEdit, L"Edit Preset");
        int dirBtnX = x + 3 * (btnW + btnGap) + editBtnW + btnGap + 10;
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
        m_hPresetListCombo = CreateCombo(hw, IDC_MW_PRESETS_LIST_COMBO, comboX, y, comboW,
          lineH * 8, hFont);
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
    m_hLblSens = CreateLabelTip(hw, L"Audio Gain (1=default):",
        L"Multiplies the audio fed to the presets. 1 is the raw level, above 1 boosts it.",
        x, y, slw, lineH, hFont);
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

    // The LIVE lock, first, because it is the one that describes what the
    // visualizer is doing right now. It sits directly above the startup
    // preference so the pair reads as "locked now" / "locked at startup"
    // rather than as two settings that might be the same one.
    //
    // Before this there was no way to SEE the live lock at all: it moved only
    // by hotkey, game controller, IPC or the silence auto-lock, and the only
    // way to learn its state was to press the key and watch whether presets
    // began advancing.
    m_hChkLockNow  = CreateCheck(hw, L"Preset Locked (now)",     IDC_MW_PRESETS_LOCK_NOW, x, y, rw, lineH, hFont, p->m_bPresetLockedByUser); y += lineH + 2;
    m_hChkHardCuts = CreateCheck(hw, L"Hard Cuts Disabled",      IDC_MW_HARD_CUTS,   x, y, rw, lineH, hFont, p->m_bHardCutsDisabled); y += lineH + 2;
    m_hChkLock     = CreateCheck(hw, L"Preset Lock on Startup",  IDC_MW_PRESET_LOCK, x, y, rw, lineH, hFont, p->m_bPresetLockOnAtStartup); y += lineH + 2;
    m_hChkSeq      = CreateCheck(hw, L"Sequential Preset Order", IDC_MW_SEQ_ORDER,   x, y, rw, lineH, hFont, p->m_bSequentialPresetOrder);
    y += lineH + gap + 4;

    // Startup preset mode combo
    {
        int startLblW = MulDiv(60, lineH, 26);
        m_hLblStartup = CreateLabel(hw, L"Startup:", x, y, startLblW, lineH, hFont);
        // A Save beside the choice, because the choice alone gives no sign.
        //
        // Every branch below already writes the setting -- but ConfigStore
        // BUFFERS writes and flushes about once a second, so nothing on screen
        // changes and there is no way to tell it took. Shane asked for the
        // button so he "knows it saved the startup preset setting"; it writes
        // through and says so.
        const int saveW = MulDiv(70, lineH, 26);
        HWND hStartup = CreateCombo(hw, IDC_MW_PRESETS_STARTUP, x + startLblW + 4, y,
                                    rw - startLblW - 4 - saveW - 6, lineH * 5, hFont);
        TrackAnchored(CreateBtn(hw, L"Save", IDC_MW_PRESETS_STARTUP_SAVE,
                                x + rw - saveW, y, saveW, lineH, hFont),
                      kAnchorTopRight);
        m_hStartupCombo = hStartup;
        SendMessageW(hStartup, CB_ADDSTRING, 0, (LPARAM)L"Random");
        SendMessageW(hStartup, CB_ADDSTRING, 0, (LPARAM)L"Current Preset");
        SendMessageW(hStartup, CB_ADDSTRING, 0, (LPARAM)L"Last Used");
        SendMessageW(hStartup, CB_ADDSTRING, 0, (LPARAM)L"Specific Preset");
        // Determine current mode
        int mode = 0; // random
        if (p->m_bEnablePresetStartup) {
            mode = p->m_bEnablePresetStartupSavingOnClose ? 2 : 1; // last used or current
        }
        SendMessage(hStartup, CB_SETCURSEL, mode, 0);
    }
    y += lineH + gap;

    // Which preset that actually IS, and a way to pick a different one.
    //
    // The combo alone never showed the stored path: "Current Preset" silently
    // snapshots whatever happens to be playing, and there was no way to browse
    // for a different file. The value was always in szPresetStartup; nothing
    // displayed it.
    {
        int brW = MulDiv(80, lineH, 26);
        m_hStartupPath = CreateEdit(hw, L"", IDC_MW_PRESETS_STARTUP_PATH,
                                    x, y, rw - brW - 6, lineH, hFont, ES_READONLY);
        TrackAnchored(m_hStartupPath, kAnchorStretchWide);
        m_hStartupBrowse = CreateBtn(hw, L"Browse...", IDC_MW_PRESETS_STARTUP_BROWSE,
                                     x + rw - brW, y, brW, lineH, hFont);
        TrackAnchored(m_hStartupBrowse, kAnchorTopRight);
        UpdateStartupPresetField();
    }
    y += lineH + gap + 4;

    // Start a timer to sync current preset display
    SetTimer(hw, 1, 500, NULL);
}

// ─── Startup preset display ─────────────────────────────────────────────

void PresetsWindow::UpdateStartupPresetField() {
    if (!m_hStartupPath) return;
    Engine* p = m_pEngine;
    // "(random)" rather than an empty box: an empty read-only field reads as
    // "not loaded yet", which is a different thing from "no preset is pinned".
    if (!p->m_bEnablePresetStartup || !p->m_szPresetStartup[0]) {
        SetWindowTextW(m_hStartupPath, L"(random)");
    } else {
        SetWindowTextW(m_hStartupPath, p->m_szPresetStartup);
    }
    // Browsing only means anything when a specific file is being pinned; in
    // Last Used mode the app overwrites the choice on exit anyway.
    if (m_hStartupBrowse)
        EnableWindow(m_hStartupBrowse, !p->m_bEnablePresetStartupSavingOnClose);
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
    const int browseBtnW = IconBtnW(lineH);
    const int browseDirW = browseBtnW;

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
        int importW = IconBtnW(lineH);
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
        int editBtnW = MulDiv(50, lineH, 26);
        if (m_hBtnEdit)
            MoveWindow(m_hBtnEdit, x + 3 * (btnW + btnGap), y, editBtnW, lineH + 4, TRUE);
        int dirBtnX = x + 3 * (btnW + btnGap) + editBtnW + btnGap + 10;
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
    MoveWindow(m_hChkLockNow, x, y, rw, lineH, TRUE);
    y += lineH + 2;
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
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            RequestLoadPreset(-1, szFile);
            UpdateCurrentPresetDisplay();
        }
        return 0;
    }

    // ── Preset List Selection ──
    if (id == IDC_MW_PRESET_LIST && code == LBN_SELCHANGE) {
        int lbSel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
        int sel = (lbSel >= 0) ? (int)SendMessage((HWND)lParam, LB_GETITEMDATA, lbSel, 0) : -1;
        if (sel >= 0 && sel < p->m_nPresets) {
            if (p->PresetNameAt(sel).c_str()[0] == L'*') {
                if (wcscmp(p->PresetNameAt(sel).c_str(), L"*..") == 0)
                    NavigatePresetDirUp();
                else
                    NavigatePresetDirInto(sel);
                return 0;
            }
            RequestLoadPreset(sel);
            UpdateCurrentPresetDisplay();
        }
        return 0;
    }

    // ── Preset List Double-Click ──
    if (id == IDC_MW_PRESET_LIST && code == LBN_DBLCLK) {
        int lbSel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
        int sel = (lbSel >= 0) ? (int)SendMessage((HWND)lParam, LB_GETITEMDATA, lbSel, 0) : -1;
        if (sel >= 0 && sel < p->m_nPresets) {
            if (p->PresetNameAt(sel).c_str()[0] == L'*') {
                if (wcscmp(p->PresetNameAt(sel).c_str(), L"*..") == 0)
                    NavigatePresetDirUp();
                else
                    NavigatePresetDirInto(sel);
            } else {
                RequestLoadPreset(sel);
                UpdateCurrentPresetDisplay();
            }
        }
        return 0;
    }

    // ── Edit: open the Preset Editor on the selected preset ──
    // Both the button and the context-menu item land here.  The editor always
    // edits the LIVE preset, so if the user picked a different one it has to be
    // loaded first -- through the render thread, never inline (see the
    // RequestLoadPreset note in tool_window.h).
    if ((id == IDC_MW_PRESET_EDIT && code == BN_CLICKED) ||
        id == IDC_MW_ANNOT_EDITPRESET) {
        int sel = (id == IDC_MW_ANNOT_EDITPRESET) ? m_nContextSel : -1;
        if (sel < 0 && m_hList) {
            int lbSel = (int)SendMessage(m_hList, LB_GETCURSEL, 0, 0);
            if (lbSel >= 0) sel = (int)SendMessage(m_hList, LB_GETITEMDATA, lbSel, 0);
        }
        if (sel >= 0 && sel < p->m_nPresets &&
            p->PresetNameAt(sel).c_str()[0] != L'*') {
            // Index-only form: ExecuteRenderCommand builds the path on the
            // render thread, which owns m_presets.
            RequestLoadPreset(sel, nullptr, 0.0f);
        }
        p->OpenPresetEditorWindow();
        return 0;
    }

    // ── Nav: Prev ──
    if (id == IDC_MW_PRESET_PREV && code == BN_CLICKED) {
        RequestNavPreset(-1);
        SyncListBoxToCurrentPreset();
        UpdateCurrentPresetDisplay();
        return 0;
    }

    // ── Nav: Next ──
    if (id == IDC_MW_PRESET_NEXT && code == BN_CLICKED) {
        RequestNavPreset(+1);
        SyncListBoxToCurrentPreset();
        UpdateCurrentPresetDisplay();
        return 0;
    }

    // ── Nav: Copy current playing preset name ──
    if (id == IDC_MW_PRESET_COPY && code == BN_CLICKED) {
        const wchar_t* src = p->m_szCurrentPresetFile;
        if ((!src || !src[0]) && p->m_pState && p->m_pState->m_szDesc[0])
            src = p->m_pState->m_szDesc;
        const wchar_t* name = src ? wcsrchr(src, L'\\') : nullptr;
        if (!name && src) name = wcsrchr(src, L'/');
        name = name ? name + 1 : src;
        if (name && name[0])
            CopyTextToClipboard(hWnd, name);
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
            Config().SetString(L"Settings", L"szActivePresetList", L"");
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
                Config().SetString(L"Settings", L"szActivePresetList", p->m_szActivePresetList.c_str());
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
        Config().SetString(L"Settings", L"szActivePresetList", L"");
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
            if (p->m_fAudioSensitivity <= 0.0f) p->m_fAudioSensitivity = 1.0f;
            if (p->m_fAudioSensitivity > 256) p->m_fAudioSensitivity = 256;
            // mdropdx12.h (via engine.h) already declares this. The block-scope
            // "extern" that used to sit here names an entity in namespace mdrop
            // by the standard's rules, not the global -- it only worked because
            // MSVC binds it to the global anyway.
            mdropdx12_audio_sensitivity = p->m_fAudioSensitivity;
            p->SaveSettingToINI(SET_AUDIO_SENSITIVITY);
            return 0;
        }
        case IDC_MW_BLEND_TIME:
            p->m_fBlendTimeAuto = (float)_wtof(buf);
            if (p->m_fBlendTimeAuto < 0.1f) p->m_fBlendTimeAuto = 0.1f;
            if (p->m_fBlendTimeAuto > 10) p->m_fBlendTimeAuto = 10;
            p->SaveSettingToINI(SET_BLEND_TIME);
            return 0;
        case IDC_MW_TIME_BETWEEN: {
            // Through SetMainRenderInterval, so a number the user just typed
            // takes the value back off a display profile's session-only loan
            // (#356). Writing the member directly left the loan standing, and
            // both the profile save and the shutdown save then persisted the
            // profile's interval instead of this one -- which is how the
            // primary interval was measured wandering 41 -> 0 -> 60 across
            // thirteen real profiles with no deliberate change behind any of
            // it. The Displays window's copy of this field was fixed then;
            // this one was not.
            float secs = (float)_wtof(buf);
            if (secs < 0) secs = 0;
            if (secs > 99999) secs = 99999;
            p->SetMainRenderInterval(secs);
            return 0;
        }
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
        case IDC_MW_PRESETS_LOCK_NOW:
            // Through SetUserPresetLock, never by assigning the member: that
            // is what relays the change to child instances, and it early-outs
            // on no change. Nothing is written to the ini -- this is the live
            // state, and the startup preference beside it is a separate
            // setting that toggling the lock must not disturb (see
            // DIAG_STARTUP_PRESET_LOCK, which exists to assert exactly that).
            p->SetUserPresetLock(bChecked);
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
        const std::wstring nameCtx = p->PresetNameAt(m_nContextSel);
        const wchar_t* fnFull = nameCtx.c_str();
        const wchar_t* fnSlash = wcsrchr(fnFull, L'\\');
        const wchar_t* fn = fnSlash ? (fnSlash + 1) : fnFull;
        if (id == IDC_MW_ANNOT_FAV) {
            p->SetPresetFlag(fn, PFLAG_FAVORITE,
                             !(AnnotFlagsOf(p, fn) & PFLAG_FAVORITE));
            RefreshPresetList();
            return 0;
        }
        if (id == IDC_MW_ANNOT_SKIP) {
            p->SetPresetFlag(fn, PFLAG_SKIP,
                             !(AnnotFlagsOf(p, fn) & PFLAG_SKIP));
            RefreshPresetList();
            return 0;
        }
        if (id == IDC_MW_ANNOT_BROKEN) {
            p->SetPresetFlag(fn, PFLAG_BROKEN,
                             !(AnnotFlagsOf(p, fn) & PFLAG_BROKEN));
            RefreshPresetList();
            return 0;
        }
        if (id == IDC_MW_ANNOT_NOTE) {
            wchar_t szNote[1024] = {};
            // Copied out first: ShowNoteDialog is modal, and a modal dialog
            // inside the guard holds it for as long as the user takes to type.
            p->WithAnnotation(fn, /*create=*/false, [&](PresetAnnotation& a) {
                if (!a.notes.empty())
                    wcsncpy_s(szNote, a.notes.c_str(), _TRUNCATE);
            });
            // Simple input dialog
            if (ShowNoteDialog(hWnd, fn, szNote, _countof(szNote))) {
                p->SetPresetNote(fn, szNote);
                p->AddNotification(L"Note saved");
            }
            return 0;
        }
        if (id == IDC_MW_ANNOT_VIEWERR) {
            std::wstring errText, errTime;
            p->WithAnnotation(fn, /*create=*/false, [&](PresetAnnotation& a) {
                errText = a.errorText;
                errTime = a.errorTime;
            });
            // Copying an error is only useful if it says which preset it came
            // from, so the viewer is given the full path, not the list label.
            std::wstring full = fnFull;
            if (!(full.size() > 1 && full[1] == L':'))
                full = std::wstring(p->m_szPresetDir) + full;
            // Outside the guard: this opens a modal dialog.
            ShowShaderErrorDialog(p, hWnd, full, errText, errTime);
            return 0;
        }
        if (id == IDC_MW_ANNOT_CLEAR) {
            const bool cleared = p->WithAnnotation(fn, /*create=*/false,
                                                  [&](PresetAnnotation& a) {
                a.flags = 0;
                a.errorText.clear();
                a.errorTime.clear();
                a.errorKind = PresetErrorKind::Shader;
                a.notes.clear();
                a.rating = 0;
            });
            if (cleared) {
                p->m_annot.with([](Engine::AnnotStore& s) { s.dirty = true; });
                p->SavePresetAnnotations();   // outside the guard: writes a file
                RefreshPresetList();
            }
            return 0;
        }
        if (id >= IDC_MW_ANNOT_CANVAS_BASE && id <= IDC_MW_ANNOT_CANVAS_BASE + 5) {
            static const int kChoices[] = { 0, 1920, 1440, 1080, 768, 512 };
            p->SetPresetCanvasMaxByFile(fn, kChoices[id - IDC_MW_ANNOT_CANVAS_BASE]);
            return 0;
        }
        if (id == IDC_MW_ANNOT_CANVAS_CLEAR) {
            p->SetPresetCanvasMaxByFile(fn, 0);
            return 0;
        }
        if (id >= IDC_MW_ANNOT_DAMP_BASE &&
            id < IDC_MW_ANNOT_DAMP_BASE + (int)_countof(Engine::kDampChoices)) {
            p->SetPresetFeedbackDampByFile(
                fn, Engine::kDampChoices[id - IDC_MW_ANNOT_DAMP_BASE]);
            return 0;
        }
        if (id == IDC_MW_ANNOT_DAMP_CLEAR) {
            p->SetPresetFeedbackDampByFile(fn, 0.0f);
            return 0;
        }
        if (id >= IDC_MW_ANNOT_RATE_BASE && id <= IDC_MW_ANNOT_RATE_BASE + 5) {
            int rating = id - IDC_MW_ANNOT_RATE_BASE;
            // Recorded against the running preset's content hash when this IS
            // the running preset, so the rating survives the file being edited.
            // For any other preset the hash is unknown here, and the
            // observation binds to the entry's stored hash instead.
            p->SetPresetRatingForFile(fn, rating);
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
        switch (mode) {
        case 0: // Random
            p->m_bEnablePresetStartup = false;
            p->m_bEnablePresetStartupSavingOnClose = false;
            break;
        case 1: // Current Preset
            p->m_bEnablePresetStartup = true;
            p->m_bEnablePresetStartupSavingOnClose = false;
            // Set current preset as startup
            CopyTo(p->m_szPresetStartup, p->m_szCurrentPresetFile);
            Config().SetString(L"Settings", L"szPresetStartup", p->m_szPresetStartup);
            break;
        case 2: // Last Used
            p->m_bEnablePresetStartup = true;
            p->m_bEnablePresetStartupSavingOnClose = true;
            // Set current preset as startup (will be updated on close)
            CopyTo(p->m_szPresetStartup, p->m_szCurrentPresetFile);
            Config().SetString(L"Settings", L"szPresetStartup", p->m_szPresetStartup);
            break;
        case 3: // Specific Preset — keep whatever is stored until Browse picks one
            p->m_bEnablePresetStartup = true;
            p->m_bEnablePresetStartupSavingOnClose = false;
            break;
        }
        Config().SetInt(L"Settings", L"bEnablePresetStartup", p->m_bEnablePresetStartup);
        Config().SetInt(L"Settings", L"bEnablePresetStartupSavingOnClose", p->m_bEnablePresetStartupSavingOnClose);
        UpdateStartupPresetField();
        return 0;
    }

    // ── Write the startup setting out now, and say so ──
    if (id == IDC_MW_PRESETS_STARTUP_SAVE && code == BN_CLICKED) {
        // In "Current Preset" mode, Save means the preset that is current NOW.
        //
        // m_szPresetStartup was only ever filled in by CBN_SELCHANGE, so it
        // held whatever happened to be playing when the mode was chosen. Pick
        // the mode, browse around for a while, press Save, and it wrote the
        // preset from several minutes ago -- while the field below went on
        // showing that stale path, so nothing said which preset had been
        // pinned. Re-read it here, which is what the button already looks like
        // it does.
        //
        // From the combo, not from the flags: "Current Preset" and "Specific
        // Preset" are both bEnablePresetStartup && !SavingOnClose, so the flags
        // cannot tell them apart, and a Specific file the user browsed to must
        // not be overwritten by whatever is on screen.
        const int mode = m_hStartupCombo
            ? (int)SendMessage(m_hStartupCombo, CB_GETCURSEL, 0, 0) : -1;
        if (mode == 1 || mode == 2)
            CopyTo(p->m_szPresetStartup, p->m_szCurrentPresetFile);

        // ConfigWriteOverride, because this is the case it exists for: a write
        // the user explicitly asked for. Testing mode's shield would otherwise
        // swallow a deliberate press and report success.
        {
            ConfigWriteOverride allow;
            Config().SetInt(L"Settings", L"bEnablePresetStartup",
                            p->m_bEnablePresetStartup);
            Config().SetInt(L"Settings", L"bEnablePresetStartupSavingOnClose",
                            p->m_bEnablePresetStartupSavingOnClose);
            Config().SetString(L"Settings", L"szPresetStartup",
                               p->m_szPresetStartup);
            ConfigFlushAll();   // on disk before the message claims it is
        }
        // Show what was actually saved. The read-only field below the combo is
        // the only thing that names the pinned preset, and it was not refreshed
        // on Save -- so the one action whose whole purpose is confirmation left
        // the confirmation stale.
        UpdateStartupPresetField();
        p->AddNotification(p->m_szPresetStartup[0]
                               ? (wchar_t*)L"Startup preset saved"
                               : (wchar_t*)L"Startup setting saved");
        return 0;
    }

    // ── Browse for a specific startup preset ──
    if (id == IDC_MW_PRESETS_STARTUP_BROWSE && code == BN_CLICKED) {
        wchar_t file[MAX_PATH] = {};
        wcsncpy_s(file, p->m_szPresetStartup, _TRUNCATE);
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWnd;
        ofn.lpstrFilter = L"Presets\0*.milk;*.milk2;*.milk3\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = p->m_szPresetDir;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            CopyTo(p->m_szPresetStartup, file);
            p->m_bEnablePresetStartup = true;
            // Browsing pins THIS file. Last Used would overwrite it on exit,
            // which would quietly undo what the user just chose.
            p->m_bEnablePresetStartupSavingOnClose = false;
            Config().SetString(L"Settings", L"szPresetStartup", p->m_szPresetStartup);
            Config().SetInt(L"Settings", L"bEnablePresetStartup", 1);
            Config().SetInt(L"Settings", L"bEnablePresetStartupSavingOnClose", 0);
            if (m_hStartupCombo)
                SendMessage(m_hStartupCombo, CB_SETCURSEL, 3, 0);
            UpdateStartupPresetField();
        }
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
        // The live lock moves from four places that are not this window -- a
        // hotkey, the game controller, IPC, and the auto-lock-on-silence timer
        // -- so the box has to follow the engine rather than only lead it.
        // Written only when it actually differs, so clicking the box does not
        // fight the timer.
        //
        // These checkboxes are OWNER-DRAWN and paint from the "Checked" window
        // property, not from the button's own state -- see the SetCheckbox
        // note in engine_displays.cpp, which says outright that "BM_SETCHECK on
        // an owner-drawn checkbox ... never reads the real state when
        // painting". Setting BM_SETCHECK alone therefore changed a value
        // nothing draws from: the engine moved, the box did not, and a
        // full-window capture diff across a lock change came back completely
        // empty. Property first, then the button state so IsChecked() agrees,
        // then invalidate so it actually repaints.
        if (m_hChkLockNow) {
            const bool locked = m_pEngine->m_bPresetLockedByUser;
            const bool shown =
                (intptr_t)GetPropW(m_hChkLockNow, L"Checked") != 0;
            if (shown != locked) {
                SetPropW(m_hChkLockNow, L"Checked",
                         (HANDLE)(intptr_t)(locked ? 1 : 0));
                SendMessage(m_hChkLockNow, BM_SETCHECK,
                            locked ? BST_CHECKED : BST_UNCHECKED, 0);
                InvalidateRect(m_hChkLockNow, NULL, TRUE);
            }
        }

        // Sync current preset display with engine state
        UpdateCurrentPresetDisplay();
        // ...and the startup preset, which Last Used mode rewrites on exit and
        // a display profile can replace while this window is open.
        UpdateStartupPresetField();
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
    if (p->PresetNameAt(sel).c_str()[0] == L'*') return -1; // directory

    const std::wstring nameSel = p->PresetNameAt(sel);
    const wchar_t* fn = nameSel.c_str();
    const wchar_t* fnOnly = wcsrchr(fn, L'\\');
    fnOnly = fnOnly ? (fnOnly + 1) : fn;
    // Read the bits, then build the menu with the guard released -- menu
    // construction and TrackPopupMenu both pump.
    uint32_t annotFlags = 0;
    int curRating = 0;
    int curCanvas = 0;
    float curDamp = 0.0f;
    p->WithAnnotation(fnOnly, /*create=*/false, [&](PresetAnnotation& a) {
        annotFlags = a.flags;
        curRating  = mdrop::Engine::AverageRating(a);
        if (a.hasCanvasMax)    curCanvas = a.canvasMax;
        if (a.hasFeedbackDamp) curDamp   = a.feedbackDamp;
    });
    bool isFav    = (annotFlags & PFLAG_FAVORITE) != 0;
    bool isSkip   = (annotFlags & PFLAG_SKIP) != 0;
    bool isBroken = (annotFlags & PFLAG_BROKEN) != 0;
    bool hasError = (annotFlags & PFLAG_ERROR) != 0;

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDC_MW_ANNOT_EDITPRESET, L"Edit Preset...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
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
    for (int r = 5; r >= 0; r--) {
        wchar_t label[32];
        if (r == 0) CopyTo(label, L"Unrated");
        else {
            label[0] = 0;
            for (int s = 0; s < r; s++) AppendTo(label, L"\x2605");
        }
        AppendMenuW(hRateMenu, MF_STRING | (curRating == r ? MF_CHECKED : 0), IDC_MW_ANNOT_RATE_BASE + r, label);
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hRateMenu, L"Rating");

    // Canvas limit submenu. Listed high to low: the canvas starts at the
    // display size and is only ever stepped down. Caps the feedback canvas for
    // presets whose feedback runs away at large sizes.
    // Engine::kCanvasChoices -- one definition, see engine.h (issue 19).
    using mdrop::Engine;
    HMENU hCanvasMenu = CreatePopupMenu();
    for (int i = 0; i < (int)_countof(Engine::kCanvasChoices); i++) {
        wchar_t label[32];
        if (Engine::kCanvasChoices[i] == 0) CopyTo(label, L"Auto (no limit)");
        else FormatTo(label, L"%d px", Engine::kCanvasChoices[i]);
        AppendMenuW(hCanvasMenu, MF_STRING | (curCanvas == Engine::kCanvasChoices[i] ? MF_CHECKED : 0),
                    IDC_MW_ANNOT_CANVAS_BASE + i, label);
    }
    AppendMenuW(hCanvasMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hCanvasMenu, MF_STRING | (curCanvas ? 0 : MF_GRAYED),
                IDC_MW_ANNOT_CANVAS_CLEAR, L"Clear canvas limit");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hCanvasMenu, L"Canvas Limit");

    // Feedback damp submenu -- the other half of the pair. A preset whose
    // feedback runs away at large canvases can be fixed by capping the canvas
    // (above) or by bleeding energy out of the loop (here), and the two are
    // independent: either, both or neither. Which looks better on a given
    // preset is a judgement about that preset, so both are offered and neither
    // is applied automatically.
    HMENU hDampMenu = CreatePopupMenu();
    for (int i = 0; i < (int)_countof(Engine::kDampChoices); i++) {
        AppendMenuW(hDampMenu, MF_STRING |
                    (Engine::DampChoiceIndex(curDamp) == i ? MF_CHECKED : 0),
                    IDC_MW_ANNOT_DAMP_BASE + i, Engine::DampChoiceLabel(i));
    }
    AppendMenuW(hDampMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hDampMenu, MF_STRING | (curDamp > 0.0f ? 0 : MF_GRAYED),
                IDC_MW_ANNOT_DAMP_CLEAR, L"Clear feedback damp");
    // The warning rides on the submenu's own label. A preset whose composite
    // shader is a decreasing function of its feedback gets BRIGHTER as the damp
    // bites, so pointing that out here saves the user a minute of watching it
    // get worse. Only ever advisory -- the entries stay enabled, because the
    // check is a text scan over arbitrary HLSL and is allowed to be wrong.
    const bool inverts = p->CompShaderInvertsFeedbackNow() &&
                         _wcsicmp(fn, p->CurrentPresetFilename()) == 0;
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hDampMenu,
                inverts ? L"Feedback Damp  (inverts - will brighten)"
                        : L"Feedback Damp");

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
    HWND hNoteEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", szNote,
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
        10, 10, 370, 100, hDlg, (HMENU)101, GetModuleHandle(NULL), NULL);
    SendMessage(hNoteEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

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
                GetWindowTextW(hNoteEdit, szNote, nMaxNote);
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
    auto& pDir = p->m_szPresetDir;   // the array itself, so AppendTo deduces its size

    // Strip trailing backslash, find previous one, truncate after it
    int dirLen = lstrlenW(pDir);
    if (dirLen > 0 && pDir[dirLen - 1] == L'\\')
        pDir[dirLen - 1] = 0;
    wchar_t* p2 = wcsrchr(pDir, L'\\');
    if (p2 && p2 > pDir) {
        *(p2 + 1) = 0;
    } else {
        AppendTo(pDir, L"\\");
        return;
    }
    Config().SetString(L"Settings", L"szPresetDir", pDir);
    p->UpdatePresetList(true, true, false);
    p->m_nCurrentPreset = -1;

    UpdatePresetDirDisplay();
    RefreshPresetList();
}

void PresetsWindow::NavigatePresetDirInto(int sel) {
    Engine* p = m_pEngine;
    if (sel < 0 || sel >= p->m_nPresets) return;
    if (p->PresetNameAt(sel).c_str()[0] != L'*') return;

    auto& pDir = p->m_szPresetDir;   // the array itself, so AppendTo deduces its size
    AppendTo(pDir, &p->PresetNameAt(sel).c_str()[1]);
    AppendTo(pDir, L"\\");
    Config().SetString(L"Settings", L"szPresetDir", pDir);
    p->UpdatePresetList(true, true, false);
    p->m_nCurrentPreset = -1;

    UpdatePresetDirDisplay();
    RefreshPresetList();
}

// ─── Destroy ────────────────────────────────────────────────────────────

void PresetsWindow::DoDestroy() {
    KillTimer(m_hWnd, 1);
    if (m_hIconBtnFont) { DeleteObject(m_hIconBtnFont); m_hIconBtnFont = NULL; }
}

} // namespace mdrop
