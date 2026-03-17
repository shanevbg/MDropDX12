// engine_annotations_ui.cpp — Annotations ToolWindow implementation.
//
// Standalone window for viewing/editing preset annotations (ratings, flags, notes,
// shader error text). Provides a filtered ListView of all annotated presets,
// plus Import (from presets.json) and Scan (from .milk fRating values).

#include "engine.h"
#include "engine_helpers.h"
#include "utility.h"
#include <CommCtrl.h>
#include <Uxtheme.h>
#include <commdlg.h>
#pragma comment(lib, "UxTheme.lib")

namespace mdrop {

// ─── Constructor ────────────────────────────────────────────────────────

AnnotationsWindow::AnnotationsWindow(Engine* pEngine)
    : ToolWindow(pEngine, 700, 750)
{
}

// ─── Helpers ────────────────────────────────────────────────────────────

// Wide ListView helpers (project uses MBCS, so ListView macros resolve to ANSI)
static void LV_SetItemTextW(HWND hLV, int row, int col, LPWSTR text) {
    LVITEMW lvi = {};
    lvi.iSubItem = col;
    lvi.pszText = text;
    SendMessageW(hLV, LVM_SETITEMTEXTW, row, (LPARAM)&lvi);
}

static void LV_GetItemTextW(HWND hLV, int row, int col, wchar_t* buf, int cchMax) {
    LVITEMW lvi = {};
    lvi.iSubItem = col;
    lvi.pszText = buf;
    lvi.cchTextMax = cchMax;
    SendMessageW(hLV, LVM_GETITEMTEXTW, row, (LPARAM)&lvi);
}

static void LV_InsertColumnW(HWND hLV, int col, const wchar_t* text, int cx) {
    LVCOLUMNW c = {};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = LVCFMT_LEFT;
    c.pszText = (LPWSTR)text;
    c.cx = cx;
    SendMessageW(hLV, LVM_INSERTCOLUMNW, col, (LPARAM)&c);
}

static int LV_InsertItemW(HWND hLV, int row, const wchar_t* text) {
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = row;
    lvi.pszText = (LPWSTR)text;
    return (int)SendMessageW(hLV, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
}

static const wchar_t* FlagsToDisplay(uint32_t flags) {
    static wchar_t buf[32];
    buf[0] = 0;
    if (flags & PFLAG_FAVORITE) wcscat_s(buf, L"\x2605 ");
    if (flags & PFLAG_ERROR)    wcscat_s(buf, L"\x26A0 ");
    if (flags & PFLAG_SKIP)     wcscat_s(buf, L"\x2298 ");
    if (flags & PFLAG_BROKEN)   wcscat_s(buf, L"\x2716 ");
    return buf;
}

static const wchar_t* RatingToStars(int rating) {
    static const wchar_t* stars[] = {
        L"", L"\x2605", L"\x2605\x2605", L"\x2605\x2605\x2605",
        L"\x2605\x2605\x2605\x2605", L"\x2605\x2605\x2605\x2605\x2605"
    };
    if (rating < 0 || rating > 5) rating = 0;
    return stars[rating];
}

void AnnotationsWindow::RefreshList() {
    if (!m_hListView) return;
    Engine* p = m_pEngine;
    ListView_DeleteAllItems(m_hListView);

    int row = 0;
    for (auto& [key, a] : p->m_presetAnnotations) {
        // Apply filter
        if (m_nFilterMode == 1 && !(a.flags & PFLAG_FAVORITE)) continue;
        if (m_nFilterMode == 2 && !(a.flags & PFLAG_ERROR))    continue;
        if (m_nFilterMode == 3 && !(a.flags & PFLAG_SKIP))     continue;
        if (m_nFilterMode == 4 && !(a.flags & PFLAG_BROKEN))   continue;

        LV_InsertItemW(m_hListView, row, a.filename.c_str());
        LV_SetItemTextW(m_hListView, row, 1, (LPWSTR)RatingToStars(a.rating));
        LV_SetItemTextW(m_hListView, row, 2, (LPWSTR)FlagsToDisplay(a.flags));

        // Notes column (truncated)
        wchar_t notesPreview[80] = {};
        if (!a.notes.empty()) {
            wcsncpy_s(notesPreview, a.notes.c_str(), 76);
            if (a.notes.size() > 76) wcscat_s(notesPreview, L"...");
        }
        LV_SetItemTextW(m_hListView, row, 3, notesPreview);

        row++;
    }
}

std::wstring AnnotationsWindow::GetSelectedFilename() {
    if (!m_hListView) return L"";
    int sel = ListView_GetNextItem(m_hListView, -1, LVNI_SELECTED);
    if (sel < 0) return L"";
    wchar_t buf[512] = {};
    LV_GetItemTextW(m_hListView, sel, 0, buf, _countof(buf));
    return buf;
}

// ─── Details Modal Dialog ────────────────────────────────────────────────

class AnnotationDetailsDialog : public ModalDialog {
    const PresetAnnotation& m_annot;

    const wchar_t* GetDialogTitle() const override { return m_annot.filename.c_str(); }
    const wchar_t* GetDialogClass() const override { return L"MDropAnnotDetailsDlg"; }

    void DoBuildControls(int clientW, int clientH) override {
        auto L = GetBaseLayout();
        int x = L.margin, y = L.margin;
        int rw = clientW - L.margin * 2;
        HFONT hFont = GetFont();

        // Flags line
        std::wstring flagStr;
        if (m_annot.flags & PFLAG_FAVORITE) flagStr += L"\x2605 Favorite  ";
        if (m_annot.flags & PFLAG_ERROR)    flagStr += L"\x26A0 Error  ";
        if (m_annot.flags & PFLAG_SKIP)     flagStr += L"\x2298 Skip  ";
        if (m_annot.flags & PFLAG_BROKEN)   flagStr += L"\x2716 Broken  ";
        if (flagStr.empty()) flagStr = L"(none)";

        HWND hLbl = CreateWindowExW(0, L"STATIC", L"Flags:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, L.labelW, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hLbl);

        HWND hFlags = CreateWindowExW(0, L"STATIC", flagStr.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
            x + L.labelW + 4, y, rw - L.labelW - 4, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hFlags && hFont) SendMessage(hFlags, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hFlags);
        y += L.lineH + L.gap;

        // Rating line
        wchar_t ratingStr[32];
        if (m_annot.rating > 0)
            swprintf(ratingStr, 32, L"%ls  (%d/5)", RatingToStars(m_annot.rating), m_annot.rating);
        else
            wcscpy_s(ratingStr, L"(unrated)");

        hLbl = CreateWindowExW(0, L"STATIC", L"Rating:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, L.labelW, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hLbl);

        HWND hRating = CreateWindowExW(0, L"STATIC", ratingStr, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x + L.labelW + 4, y, rw - L.labelW - 4, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hRating && hFont) SendMessage(hRating, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hRating);
        y += L.lineH + L.gap + 4;

        // Notes section
        hLbl = CreateWindowExW(0, L"STATIC", L"Notes:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, L.labelW, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hLbl);
        y += L.lineH;

        int editH = L.lineH * 4;
        HWND hNoteEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            m_annot.notes.empty() ? L"(no notes)" : m_annot.notes.c_str(),
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            x, y, rw, editH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hNoteEdit && hFont) SendMessage(hNoteEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hNoteEdit);
        y += editH + L.gap + 4;

        // Error section
        hLbl = CreateWindowExW(0, L"STATIC", L"Shader Error:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, 100, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hLbl);
        y += L.lineH;

        HWND hErrEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            m_annot.errorText.empty() ? L"(no error)" : m_annot.errorText.c_str(),
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            x, y, rw, editH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hErrEdit && hFont) SendMessage(hErrEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hErrEdit);
        y += editH + L.gap;

        // Close button
        int btnW = 80;
        HWND hClose = CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
            x + rw - btnW, y, btnW, L.lineH, m_hWnd, (HMENU)(INT_PTR)IDCANCEL, GetModuleHandle(NULL), NULL);
        if (hClose && hFont) SendMessage(hClose, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hClose);
        y += L.lineH + L.margin;

        FitToContent(clientW, y);
    }

    LRESULT DoCommand(int id, int code, LPARAM lParam) override {
        if (id == IDCANCEL) { EndDialog(false); return 0; }
        return -1;
    }

public:
    AnnotationDetailsDialog(Engine* pEngine, const PresetAnnotation& a)
        : ModalDialog(pEngine), m_annot(a) {}
};

void AnnotationsWindow::ShowDetailsDialog() {
    std::wstring fn = GetSelectedFilename();
    if (fn.empty()) return;
    PresetAnnotation* a = m_pEngine->GetAnnotation(fn.c_str());
    if (!a) return;
    AnnotationDetailsDialog dlg(m_pEngine, *a);
    dlg.Show(m_hWnd, 450, 200);
}

// ─── Import Modal Dialog ─────────────────────────────────────────────────

class ImportAnnotationsDialog : public ModalDialog {
    Engine* m_pEng;
    std::unordered_map<std::wstring, PresetAnnotation> m_imported;
    std::wstring m_title;
    HWND m_hList = NULL;
    int  m_nImported = 0;  // count of entries actually imported

    const wchar_t* GetDialogTitle() const override { return m_title.c_str(); }
    const wchar_t* GetDialogClass() const override { return L"MDropAnnotImportDlg"; }

    void DoBuildControls(int clientW, int clientH) override {
        auto L = GetBaseLayout();
        int x = L.margin, y = L.margin;
        int rw = clientW - L.margin * 2;
        HFONT hFont = GetFont();

        // Summary label
        wchar_t szSummary[128];
        swprintf(szSummary, 128, L"Found %d presets to review:", (int)m_imported.size());
        HWND hLbl = CreateWindowExW(0, L"STATIC", szSummary, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hLbl);
        y += L.lineH + L.gap;

        // ListView: Preset | Imported Rating | Current Rating | Imported Flags | Current Flags
        int listH = clientH - y - L.lineH - L.margin * 2 - L.gap;
        if (listH < 100) listH = 100;
        m_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
            x, y, rw, listH, m_hWnd, (HMENU)(INT_PTR)IDC_MW_IMPORT_LIST,
            GetModuleHandle(NULL), NULL);
        if (m_hList && hFont) SendMessage(m_hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        ListView_SetExtendedListViewStyle(m_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
        TrackControl(m_hList);

        // Dark theme for the ListView
        if (m_pEng->IsDarkTheme()) {
            SetWindowTheme(m_hList, L"DarkMode_Explorer", NULL);
            ListView_SetBkColor(m_hList, m_pEng->m_colSettingsCtrlBg);
            ListView_SetTextBkColor(m_hList, m_pEng->m_colSettingsCtrlBg);
            ListView_SetTextColor(m_hList, m_pEng->m_colSettingsText);
        }

        LV_InsertColumnW(m_hList, 0, L"Preset", 200);
        LV_InsertColumnW(m_hList, 1, L"Source Rating", 90);
        LV_InsertColumnW(m_hList, 2, L"Current Rating", 95);
        LV_InsertColumnW(m_hList, 3, L"Source Flags", 80);
        LV_InsertColumnW(m_hList, 4, L"Current Flags", 80);

        // Populate
        int row = 0;
        for (auto& [key, imp] : m_imported) {
            LV_InsertItemW(m_hList, row, imp.filename.c_str());
            LV_SetItemTextW(m_hList, row, 1, (LPWSTR)RatingToStars(imp.rating));

            // Current annotation (if any)
            PresetAnnotation* cur = m_pEng->GetAnnotation(imp.filename.c_str());
            LV_SetItemTextW(m_hList, row, 2, (LPWSTR)RatingToStars(cur ? cur->rating : 0));
            LV_SetItemTextW(m_hList, row, 3, (LPWSTR)FlagsToDisplay(imp.flags));
            LV_SetItemTextW(m_hList, row, 4, (LPWSTR)FlagsToDisplay(cur ? cur->flags : 0));

            // Check by default if imported has data and current doesn't
            bool shouldCheck = (!cur || cur->rating == 0) && (imp.rating > 0 || imp.flags != 0);
            ListView_SetCheckState(m_hList, row, shouldCheck);
            row++;
        }
        y += listH + L.gap;

        // Buttons: Import Selected | Merge All | Cancel
        int btnW = 110;
        int bx = x;
        HWND hBtn;

        hBtn = CreateWindowExW(0, L"BUTTON", L"Import Checked",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
            bx, y, btnW, L.lineH, m_hWnd, (HMENU)(INT_PTR)IDC_MW_IMPORT_SEL,
            GetModuleHandle(NULL), NULL);
        if (hBtn && hFont) SendMessage(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hBtn);
        bx += btnW + 8;

        hBtn = CreateWindowExW(0, L"BUTTON", L"Merge All",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
            bx, y, btnW - 20, L.lineH, m_hWnd, (HMENU)(INT_PTR)IDC_MW_IMPORT_MERGE,
            GetModuleHandle(NULL), NULL);
        if (hBtn && hFont) SendMessage(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hBtn);
        bx += (btnW - 20) + 8;

        hBtn = CreateWindowExW(0, L"BUTTON", L"Import All",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
            bx, y, btnW - 20, L.lineH, m_hWnd, (HMENU)(INT_PTR)IDC_MW_IMPORT_ALL,
            GetModuleHandle(NULL), NULL);
        if (hBtn && hFont) SendMessage(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hBtn);

        btnW = 70;
        hBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
            x + rw - btnW, y, btnW, L.lineH, m_hWnd, (HMENU)(INT_PTR)IDCANCEL,
            GetModuleHandle(NULL), NULL);
        if (hBtn && hFont) SendMessage(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hBtn);
    }

    // Import a single annotation: overwrite = replace entirely, merge = combine flags + take higher rating
    void ImportOne(const PresetAnnotation& imp, bool merge) {
        PresetAnnotation* cur = m_pEng->GetAnnotation(imp.filename.c_str(), true);
        if (!cur) return;
        if (merge) {
            // Take higher rating
            if (imp.rating > cur->rating) cur->rating = imp.rating;
            // Combine flags
            cur->flags |= imp.flags;
            // Append notes if imported has notes and current doesn't
            if (!imp.notes.empty() && cur->notes.empty()) cur->notes = imp.notes;
            // Keep error text if current is empty
            if (!imp.errorText.empty() && cur->errorText.empty()) cur->errorText = imp.errorText;
        } else {
            // Overwrite
            cur->rating = imp.rating;
            cur->flags = imp.flags;
            cur->notes = imp.notes;
            cur->errorText = imp.errorText;
        }
        m_nImported++;
    }

    LRESULT DoCommand(int id, int code, LPARAM lParam) override {
        if (id == IDCANCEL) { EndDialog(false); return 0; }

        if (id == IDC_MW_IMPORT_ALL) {
            for (auto& [key, imp] : m_imported)
                ImportOne(imp, false);
            m_pEng->m_bAnnotationsDirty = true;
            m_pEng->SavePresetAnnotations();
            EndDialog(true);
            return 0;
        }

        if (id == IDC_MW_IMPORT_MERGE) {
            for (auto& [key, imp] : m_imported)
                ImportOne(imp, true);
            m_pEng->m_bAnnotationsDirty = true;
            m_pEng->SavePresetAnnotations();
            EndDialog(true);
            return 0;
        }

        if (id == IDC_MW_IMPORT_SEL) {
            // Import only checked items
            int row = 0;
            for (auto& [key, imp] : m_imported) {
                if (ListView_GetCheckState(m_hList, row))
                    ImportOne(imp, false);
                row++;
            }
            if (m_nImported > 0) {
                m_pEng->m_bAnnotationsDirty = true;
                m_pEng->SavePresetAnnotations();
            }
            EndDialog(true);
            return 0;
        }

        return -1;
    }

public:
    ImportAnnotationsDialog(Engine* pEngine, std::unordered_map<std::wstring, PresetAnnotation>&& imported,
                            const wchar_t* title = L"Import Annotations")
        : ModalDialog(pEngine), m_pEng(pEngine), m_imported(std::move(imported)), m_title(title) {}

    int GetImportedCount() const { return m_nImported; }
};

void AnnotationsWindow::ShowImportDialog() {
    // File open dialog
    wchar_t szFile[MAX_PATH] = L"presets.json";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Import Annotations";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return;

    auto imported = Engine::ParseAnnotationsFile(szFile);
    if (imported.empty()) {
        MessageBoxW(m_hWnd, L"No annotations found in the selected file.", L"Import", MB_OK | MB_ICONINFORMATION);
        return;
    }

    ImportAnnotationsDialog dlg(m_pEngine, std::move(imported));
    dlg.Show(m_hWnd, 650, 500);

    if (dlg.GetImportedCount() > 0) {
        RefreshList();
        wchar_t msg[64];
        swprintf(msg, 64, L"Imported %d annotations.", dlg.GetImportedCount());
        MessageBoxW(m_hWnd, msg, L"Import", MB_OK | MB_ICONINFORMATION);
    }
}

// ─── Scan Presets ────────────────────────────────────────────────────────

void AnnotationsWindow::DoScanPresets() {
    Engine* p = m_pEngine;
    if (p->m_nPresets <= p->m_nDirs) {
        MessageBoxW(m_hWnd, L"No presets loaded yet.", L"Scan", MB_OK | MB_ICONINFORMATION);
        return;
    }

    auto scanned = p->ScanPresetsForRatings();
    if (scanned.empty()) {
        MessageBoxW(m_hWnd, L"No presets found.", L"Scan", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Show comparison dialog so user can review .milk ratings vs current annotations
    ImportAnnotationsDialog dlg(p, std::move(scanned), L"Scan Preset Ratings");
    dlg.Show(m_hWnd, 650, 500);

    if (dlg.GetImportedCount() > 0) {
        RefreshList();
        wchar_t msg[64];
        swprintf(msg, 64, L"Updated %d annotations.", dlg.GetImportedCount());
        MessageBoxW(m_hWnd, msg, L"Scan Presets", MB_OK | MB_ICONINFORMATION);
    }
}

// ─── Build Controls ─────────────────────────────────────────────────────

void AnnotationsWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    auto L = BuildBaseControls();
    int x = L.x, y = L.y, rw = L.rw;
    HFONT hFont = GetFont();
    m_nTopY = y;

    int lineH = GetLineHeight();
    int gap = 4;

    // Row 1: Filter combo + action buttons
    {
        CreateLabel(hw, L"Filter:", x, y, 50, lineH, hFont);
        m_hFilterCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            x + 54, y, 120, lineH * 7, hw,
            (HMENU)(INT_PTR)IDC_MW_ANNOTWIN_FILTER, GetModuleHandle(NULL), NULL);
        if (m_hFilterCombo && hFont) SendMessage(m_hFilterCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"All");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Favorites");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Errors");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Skipped");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Broken");
        SendMessage(m_hFilterCombo, CB_SETCURSEL, 0, 0);

        int btnW = 60;
        int bx = x + rw;
        bx -= btnW; m_hBtnRemove = CreateBtn(hw, L"Remove", IDC_MW_ANNOTWIN_REMOVE, bx, y, btnW, lineH, hFont);
        bx -= btnW + 4; m_hBtnLoad = CreateBtn(hw, L"Load", IDC_MW_ANNOTWIN_LOAD, bx, y, btnW, lineH, hFont);
        bx -= btnW + 4; m_hBtnDetails = CreateBtn(hw, L"Details", IDC_MW_ANNOTWIN_DETAILS, bx, y, btnW, lineH, hFont);
    }
    y += lineH + gap;

    // Row 2: Import / Scan buttons
    {
        int btnW = 60;
        m_hBtnImport = CreateBtn(hw, L"Import", IDC_MW_ANNOTWIN_IMPORT, x, y, btnW, lineH, hFont);
        m_hBtnScan = CreateBtn(hw, L"Scan", IDC_MW_ANNOTWIN_SCAN, x + btnW + 4, y, btnW, lineH, hFont);
    }
    y += lineH + gap;

    // ListView fills the rest of the window
    {
        RECT rc;
        GetClientRect(hw, &rc);
        int listH = rc.bottom - y - 8;
        if (listH < 100) listH = 100;

        m_hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_ANNOTWIN_LIST, GetModuleHandle(NULL), NULL);
        if (m_hListView && hFont) SendMessage(m_hListView, WM_SETFONT, (WPARAM)hFont, TRUE);
        ListView_SetExtendedListViewStyle(m_hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Apply dark theme if needed
        if (p->IsDarkTheme()) {
            SetWindowTheme(m_hListView, L"DarkMode_Explorer", NULL);
            ListView_SetBkColor(m_hListView, p->m_colSettingsCtrlBg);
            ListView_SetTextBkColor(m_hListView, p->m_colSettingsCtrlBg);
            ListView_SetTextColor(m_hListView, p->m_colSettingsText);
        }

        LV_InsertColumnW(m_hListView, 0, L"Preset", 300);
        LV_InsertColumnW(m_hListView, 1, L"Rating", 80);
        LV_InsertColumnW(m_hListView, 2, L"Flags", 70);
        LV_InsertColumnW(m_hListView, 3, L"Notes", 200);

        RefreshList();
    }
}

// ─── Layout on Resize ───────────────────────────────────────────────────

void AnnotationsWindow::OnResize() {
    LayoutControls();
}

void AnnotationsWindow::LayoutControls() {
    if (!m_hWnd || !m_hListView) return;
    RECT rc;
    GetClientRect(m_hWnd, &rc);

    int x = 8;
    int y = m_nTopY;
    int rw = rc.right - 16;
    int lineH = GetLineHeight();
    int gap = 4;

    // Row 1: filter + action buttons
    if (m_hFilterCombo) MoveWindow(m_hFilterCombo, x + 54, y, 120, lineH * 7, TRUE);
    int btnW = 60;
    int bx = x + rw;
    if (m_hBtnRemove) { bx -= btnW; MoveWindow(m_hBtnRemove, bx, y, btnW, lineH, TRUE); }
    if (m_hBtnLoad) { bx -= btnW + 4; MoveWindow(m_hBtnLoad, bx, y, btnW, lineH, TRUE); }
    if (m_hBtnDetails) { bx -= btnW + 4; MoveWindow(m_hBtnDetails, bx, y, btnW, lineH, TRUE); }
    y += lineH + gap;

    // Row 2: import / scan
    if (m_hBtnImport) MoveWindow(m_hBtnImport, x, y, btnW, lineH, TRUE);
    if (m_hBtnScan) MoveWindow(m_hBtnScan, x + btnW + 4, y, btnW, lineH, TRUE);
    y += lineH + gap;

    // ListView fills the rest
    int listH = rc.bottom - y - 8;
    if (listH < 3 * lineH) listH = 3 * lineH;
    MoveWindow(m_hListView, x, y, rw, listH, TRUE);
}

// ─── Commands ───────────────────────────────────────────────────────────

LRESULT AnnotationsWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    Engine* p = m_pEngine;

    // Filter changed
    if (id == IDC_MW_ANNOTWIN_FILTER && code == CBN_SELCHANGE) {
        m_nFilterMode = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
        RefreshList();
        return 0;
    }

    // Details button
    if (id == IDC_MW_ANNOTWIN_DETAILS && code == BN_CLICKED) {
        ShowDetailsDialog();
        return 0;
    }

    // Import button
    if (id == IDC_MW_ANNOTWIN_IMPORT && code == BN_CLICKED) {
        ShowImportDialog();
        return 0;
    }

    // Scan button
    if (id == IDC_MW_ANNOTWIN_SCAN && code == BN_CLICKED) {
        DoScanPresets();
        return 0;
    }

    // Load selected preset
    if (id == IDC_MW_ANNOTWIN_LOAD && code == BN_CLICKED) {
        std::wstring fn = GetSelectedFilename();
        if (!fn.empty()) {
            wchar_t szFile[MAX_PATH];
            swprintf(szFile, MAX_PATH, L"%s%s", p->m_szPresetDir, fn.c_str());
            p->LoadPreset(szFile, p->m_fBlendTimeUser);
        }
        return 0;
    }

    // Remove annotation
    if (id == IDC_MW_ANNOTWIN_REMOVE && code == BN_CLICKED) {
        std::wstring fn = GetSelectedFilename();
        if (!fn.empty()) {
            auto it = p->m_presetAnnotations.find(fn);
            if (it != p->m_presetAnnotations.end()) {
                p->m_presetAnnotations.erase(it);
                p->m_bAnnotationsDirty = true;
                p->SavePresetAnnotations();
                RefreshList();
                if (p->m_presetsWindow)
                    p->m_presetsWindow->Open();
            }
        }
        return 0;
    }

    return -1;
}

// ─── Notify (double-click opens details) ─────────────────────────────────

LRESULT AnnotationsWindow::DoNotify(HWND hWnd, NMHDR* pnm) {
    if (pnm->idFrom == IDC_MW_ANNOTWIN_LIST) {
        if (pnm->code == NM_DBLCLK) {
            ShowDetailsDialog();
            return 0;
        }
    }
    return -1;
}

} // namespace mdrop
