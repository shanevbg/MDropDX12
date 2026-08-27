// engine_annotations_ui.cpp — Annotations ToolWindow implementation.
//
// Standalone window for viewing/editing preset annotations (ratings, flags, notes,
// shader error text). Provides a filtered ListView of all annotated presets,
// plus Import (from presets.json) and Scan (from .milk fRating values).

#include "engine.h"
#include "engine_helpers.h"
#include "shader_overrides.h"   // ShaderOverrides(), for the per-preset slots
#include "audio_profile_store.h"  // AudioProfiles(), for the audio slot
#include "utility.h"
#include <CommCtrl.h>
#include <Uxtheme.h>
#include <commdlg.h>
#include <shlwapi.h>   // PathFileExistsW, for marking dead copies
#include <shellapi.h>  // SHFileOperationW, for recycling duplicate files
#include <thread>
#include <atomic>
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace mdrop {

// ─── Constructor ────────────────────────────────────────────────────────

// 900 rather than 700: the list carries eight columns now, and at 700 the last
// two sat past the right edge on first open. Only the default is affected --
// a window whose size is already remembered keeps whatever the user set.
AnnotationsWindow::AnnotationsWindow(Engine* pEngine)
    : ToolWindow(pEngine, 900, 750)
{
}

// ─── Helpers ────────────────────────────────────────────────────────────

// Columns of the annotations list.
//
// Named rather than numbered because three places have to agree on them --
// insertion order in DoBuildControls, the writes in RefreshList, and the sort
// switch -- and a bare 3 in two of the three is how a column ends up sorting by
// a different column's data. Column 0 is load-bearing beyond display:
// GetSelectedFilename reads its TEXT back as the annotation key.
enum AnnotCol {
    ANNOT_COL_PRESET = 0,
    ANNOT_COL_COPIES,
    ANNOT_COL_RATING,
    ANNOT_COL_FLAGS,
    ANNOT_COL_LASTUSED,
    ANNOT_COL_PLAYS,
    ANNOT_COL_TIME,
    ANNOT_COL_NOTES,
    ANNOT_COL_COUNT
};

// Wide ListView helpers.
//
// Not because of the character set -- the project builds Unicode -- but because
// the house rule is that no text-bearing ListView_* macro appears anywhere in
// this source tree; every site sends LVM_*W with an LVITEMW/LVCOLUMNW by hand.
// These wrap that so this file does not repeat it forty times. (An earlier
// comment here blamed MBCS; engine.vcxproj sets CharacterSet=Unicode for all
// four configurations, so that reason was wrong even though the code is right.)
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

// Width a button needs to actually show its caption at the current HUD font.
//
// Every fixed pixel width in this window clipped something -- "Reset Use",
// "Remove", "Import" -- and scaling a guessed constant only moves where it
// breaks. Measuring the text is the only version that stays correct as the
// font changes.
static int BtnWidthFor(HWND hw, HFONT hFont, const wchar_t* text, int lineH) {
    int cx = 0;
    HDC hdc = GetDC(hw);
    if (hdc) {
        HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
        SIZE sz = {};
        if (GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz)) cx = sz.cx;
        if (hOld) SelectObject(hdc, hOld);
        ReleaseDC(hw, hdc);
    }
    const int pad = MulDiv(20, lineH, 26);
    const int minW = MulDiv(56, lineH, 26);
    return max(minW, cx + pad);
}

// "2026-08-24T19:07:31" -> "2026-08-24 19:07".  Seconds are dropped: the column
// is for telling last month from last night, and the extra two digits only cost
// width. Anything that is not the expected ISO shape is passed through rather
// than mangled, so an older or hand-edited value still shows something true.
static std::wstring FormatLastUsed(const std::wstring& iso) {
    if (iso.empty()) return L"";
    if (iso.size() < 16 || iso[10] != L'T') return iso;
    std::wstring out = iso.substr(0, 16);
    out[10] = L' ';
    return out;
}

// Seconds as a width-bounded, human duration.  Hours matter for "what have I
// actually watched"; seconds matter for "did this even play", so the unit pair
// shifts with the magnitude instead of always printing three fields.
static std::wstring FormatDuration(int seconds) {
    if (seconds <= 0) return L"";
    wchar_t buf[32];
    if (seconds < 60)            swprintf_s(buf, L"%ds", seconds);
    else if (seconds < 3600)     swprintf_s(buf, L"%dm %02ds", seconds / 60, seconds % 60);
    else                         swprintf_s(buf, L"%dh %02dm", seconds / 3600, (seconds % 3600) / 60);
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

    // Collect first, then order, then insert. Sorting needs the whole set in
    // hand, and doing it this way makes the search a one-line filter.
    std::vector<const PresetAnnotation*> rows;
    for (auto& [key, a] : p->m_presetAnnotations) {
        if (m_nFilterMode == 1 && !(a.flags & PFLAG_FAVORITE)) continue;
        if (m_nFilterMode == 2 && !(a.flags & PFLAG_ERROR))    continue;
        if (m_nFilterMode == 3 && !(a.flags & PFLAG_SKIP))     continue;
        if (m_nFilterMode == 4 && !(a.flags & PFLAG_BROKEN))   continue;
        // Duplicates: only meaningful after a scan. DuplicateCountFor returns 0
        // for "not looked at", so this shows nothing rather than everything
        // until Find Copies has run -- an empty list is the honest answer to a
        // question that has not been asked yet.
        if (m_nFilterMode == 5 && p->DuplicateCountFor(a) < 2) continue;
        if (!p->AnnotationMatches(a, m_searchQuery.c_str()))   continue;
        rows.push_back(&a);
    }

    // Sort on the UNDERLYING DATA, never the display text: Rating renders as
    // stars and Flags as glyphs, and ordering those strings would sort by
    // codepoint and look arbitrary. stable_sort plus a filename tiebreak stops
    // equal rows reshuffling when the list is re-sorted.
    const int  sortCol = m_nSortColumn;
    const bool asc     = m_bSortAscending;
    std::stable_sort(rows.begin(), rows.end(),
        [&](const PresetAnnotation* xa, const PresetAnnotation* ya) {
            int cmp = 0;
            switch (sortCol) {
            case ANNOT_COL_COPIES:
                cmp = p->DuplicateCountFor(*xa) - p->DuplicateCountFor(*ya); break;
            case ANNOT_COL_RATING:
                cmp = Engine::AverageRating(*xa) - Engine::AverageRating(*ya); break;
            case ANNOT_COL_FLAGS:
                cmp = (int)xa->flags - (int)ya->flags; break;
            // ISO 8601 is designed so that lexicographic order IS chronological
            // order, which is the whole reason lastUsed is stored in that shape.
            // Never-played is the empty string, so it sorts before every real
            // date -- ascending puts "not played yet" at the top, which is where
            // someone hunting for dead weight wants it.
            case ANNOT_COL_LASTUSED:
                cmp = xa->lastUsed.compare(ya->lastUsed); break;
            case ANNOT_COL_PLAYS:
                cmp = xa->useCount - ya->useCount; break;
            case ANNOT_COL_TIME:
                cmp = xa->secondsShown - ya->secondsShown; break;
            case ANNOT_COL_NOTES:
                cmp = _wcsicmp(xa->notes.c_str(), ya->notes.c_str()); break;
            default:
                cmp = _wcsicmp(xa->filename.c_str(), ya->filename.c_str()); break;
            }
            if (cmp == 0) cmp = _wcsicmp(xa->filename.c_str(), ya->filename.c_str());
            return asc ? (cmp < 0) : (cmp > 0);
        });

    int row = 0;
    for (const PresetAnnotation* pa : rows) {
        const PresetAnnotation& a = *pa;
        LV_InsertItemW(m_hListView, row, a.filename.c_str());
        LV_SetItemTextW(m_hListView, row, ANNOT_COL_RATING, (LPWSTR)RatingToStars(a.rating));
        LV_SetItemTextW(m_hListView, row, ANNOT_COL_FLAGS, (LPWSTR)FlagsToDisplay(a.flags));

        // Copies: blank until a scan has run, and blank for a preset the scan
        // never reached. Printing "1" in those cases would state a fact the app
        // has not established -- see Engine::DuplicateCountFor.
        const int nCopies = p->DuplicateCountFor(a);
        wchar_t copiesBuf[16] = {};
        if (nCopies > 0) swprintf_s(copiesBuf, L"%d", nCopies);
        LV_SetItemTextW(m_hListView, row, ANNOT_COL_COPIES, copiesBuf);

        // Usage columns. All three stay empty rather than showing 0 / an epoch
        // date, so "never played" reads as absence instead of as a measurement.
        const std::wstring lastUsed = FormatLastUsed(a.lastUsed);
        LV_SetItemTextW(m_hListView, row, ANNOT_COL_LASTUSED, (LPWSTR)lastUsed.c_str());

        wchar_t playsBuf[16] = {};
        if (a.useCount > 0) swprintf_s(playsBuf, L"%d", a.useCount);
        LV_SetItemTextW(m_hListView, row, ANNOT_COL_PLAYS, playsBuf);

        const std::wstring shown = FormatDuration(a.secondsShown);
        LV_SetItemTextW(m_hListView, row, ANNOT_COL_TIME, (LPWSTR)shown.c_str());

        // Notes column (truncated)
        wchar_t notesPreview[80] = {};
        if (!a.notes.empty()) {
            wcsncpy_s(notesPreview, a.notes.c_str(), 76);
            if (a.notes.size() > 76) wcscat_s(notesPreview, L"...");
        }
        // A missing file is worth seeing without clicking: this window is meant
        // to be the one place presets are found, so a dead entry should look
        // dead rather than only fail when loaded.
        //
        // Marked in Notes, NOT in column 0: GetSelectedFilename() reads column 0
        // as the annotation key, so decorating it would break every action on
        // exactly the rows that need attention.
        if (p->IsAnnotationKnownMissing(a)) {
            wchar_t gone[96];
            swprintf_s(gone, L"(file missing) %.60s", notesPreview);
            LV_SetItemTextW(m_hListView, row, ANNOT_COL_NOTES, gone);
        } else {
            LV_SetItemTextW(m_hListView, row, ANNOT_COL_NOTES, notesPreview);
        }

        row++;
    }
}

// Per-preset actions for the right-clicked row. Deliberately the same set the
// Presets browser offers, on the same ids, so the two windows manage the same
// data the same way.
void AnnotationsWindow::ShowRowContextMenu(int screenX, int screenY) {
    Engine* p = m_pEngine;
    const std::wstring fn = GetSelectedFilename();
    if (fn.empty()) return;

    PresetAnnotation* a = p->GetAnnotation(fn.c_str());
    const bool isFav    = a && (a->flags & PFLAG_FAVORITE);
    const bool isSkip   = a && (a->flags & PFLAG_SKIP);
    const bool isBroken = a && (a->flags & PFLAG_BROKEN);
    const bool hasError = a && (a->flags & PFLAG_ERROR);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDC_MW_ANNOTWIN_LOAD, L"Load Preset");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (isFav ? MF_CHECKED : 0), IDC_MW_ANNOT_FAV, L"Favorite");
    AppendMenuW(hMenu, MF_STRING | (isSkip ? MF_CHECKED : 0), IDC_MW_ANNOT_SKIP, L"Skip");
    AppendMenuW(hMenu, MF_STRING | (isBroken ? MF_CHECKED : 0), IDC_MW_ANNOT_BROKEN, L"Broken");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    HMENU hRate = CreatePopupMenu();
    const int curRating = a ? Engine::AverageRating(*a) : 0;
    for (int r = 5; r >= 0; r--) {
        wchar_t label[32];
        if (r == 0) wcscpy_s(label, L"Unrated");
        else { label[0] = 0; for (int t = 0; t < r; t++) wcscat_s(label, L"&05"); }
        AppendMenuW(hRate, MF_STRING | (curRating == r ? MF_CHECKED : 0),
                    IDC_MW_ANNOT_RATE_BASE + r, label);
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hRate, L"Rating");

    // High to low: the canvas starts at the display size and only steps down.
    static const int kCanvasChoices[] = { 0, 1920, 1440, 1080, 768, 512 };
    const int curCanvas = (a && a->hasCanvasMax) ? a->canvasMax : 0;
    HMENU hCanvas = CreatePopupMenu();
    for (int i = 0; i < (int)_countof(kCanvasChoices); i++) {
        wchar_t label[32];
        if (kCanvasChoices[i] == 0) wcscpy_s(label, L"Auto (no limit)");
        else swprintf_s(label, L"%d px", kCanvasChoices[i]);
        AppendMenuW(hCanvas, MF_STRING | (curCanvas == kCanvasChoices[i] ? MF_CHECKED : 0),
                    IDC_MW_ANNOT_CANVAS_BASE + i, label);
    }
    AppendMenuW(hCanvas, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hCanvas, MF_STRING | (curCanvas ? 0 : MF_GRAYED),
                IDC_MW_ANNOT_CANVAS_CLEAR, L"Clear canvas limit");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hCanvas, L"Canvas Limit");

    // The mitigation that keeps the canvas at full size. Independent of the
    // one above: either, both or neither.
    const float curDamp = (a && a->hasFeedbackDamp) ? a->feedbackDamp : 0.0f;
    HMENU hDamp = CreatePopupMenu();
    for (int i = 0; i < (int)_countof(Engine::kDampChoices); i++)
        AppendMenuW(hDamp, MF_STRING |
                    (Engine::DampChoiceIndex(curDamp) == i ? MF_CHECKED : 0),
                    IDC_MW_ANNOT_DAMP_BASE + i, Engine::DampChoiceLabel(i));
    AppendMenuW(hDamp, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hDamp, MF_STRING | (curDamp > 0.0f ? 0 : MF_GRAYED),
                IDC_MW_ANNOT_DAMP_CLEAR, L"Clear feedback damp");
    // Only meaningful for the preset actually on screen: the classifier reads
    // the RUNNING comp shader, so it says nothing about any other row.
    const bool dampInverts = p->CompShaderInvertsFeedbackNow() &&
                             _wcsicmp(fn.c_str(), p->CurrentPresetFilename()) == 0;
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hDamp,
                dampInverts ? L"Feedback Damp  (inverts - will brighten)"
                            : L"Feedback Damp");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    // No longer greyed without an error. The dialog now carries identity,
    // copies, aliases, usage and all four override slots, so it says something
    // useful about every row -- and greying it hid exactly the information
    // someone hunting for duplicates came here to read.
    AppendMenuW(hMenu, MF_STRING, IDC_MW_ANNOTWIN_DETAILS,
                hasError ? L"Details / Error..." : L"Details...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDC_MW_ANNOTWIN_REMOVEMISSING,
                L"Purge Missing Presets...");

    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN, screenX, screenY, 0, m_hWnd, NULL);
    DestroyMenu(hMenu);
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

// Best full path known for an annotation. The map key is the bare filename
// whenever the preset was loaded from the browser's current directory, and a
// copied error that names "Foo.milk" and nothing else is not much use to
// whoever has to find Foo.milk. `paths` records every location the preset was
// seen at; the last one is where it was seen most recently.
static std::wstring AnnotationDisplayPath(const PresetAnnotation& a) {
    if (a.filename.find(L'\\') != std::wstring::npos ||
        a.filename.find(L'/') != std::wstring::npos)
        return a.filename;
    if (!a.paths.empty()) return a.paths.back();
    return a.filename;
}

// Everywhere this preset's content is known to sit, newest knowledge last.
//
// Two sources, and they answer different questions.  `paths` is where the app
// has SEEN this preset -- it is recorded on load, so it is reliable but only
// covers places already visited.  The duplicate index is where a hash scan
// FOUND it, which covers files never opened but only exists after a scan.
// Neither alone answers "how many copies do I have", so both are merged.
static std::vector<std::wstring> KnownCopiesOf(const Engine* p,
                                               const PresetAnnotation& a) {
    std::vector<std::wstring> out;
    auto add = [&out](const std::wstring& s) {
        if (s.empty()) return;
        for (const std::wstring& e : out)
            if (_wcsicmp(e.c_str(), s.c_str()) == 0) return;
        out.push_back(s);
    };

    // A legacy entry whose key is itself a full path counts as a location.
    if (a.filename.find(L'\\') != std::wstring::npos ||
        a.filename.find(L'/') != std::wstring::npos)
        add(a.filename);
    for (const std::wstring& s : a.paths) add(s);

    if (!a.hash.empty()) {
        auto it = p->m_dupeIndex.find(a.hash);
        if (it != p->m_dupeIndex.end())
            for (const std::wstring& s : it->second) add(s);
    }
    return out;
}

static std::wstring BaseNameOf(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return (cut == std::wstring::npos) ? path : path.substr(cut + 1);
}

// The distinct filenames one preset is stored under.
//
// The annotation has ONE filename, but content identity means the same preset
// can sit on disk as three different names -- which is exactly the state that
// makes a library feel bigger than it is. Listing them is how you find out that
// "Foo.milk", "Foo (2).milk" and "aa_Foo.milk" were always one preset.
static std::vector<std::wstring> AliasesOf(const Engine* p,
                                           const PresetAnnotation& a) {
    std::vector<std::wstring> names;
    auto add = [&names](const std::wstring& n) {
        if (n.empty()) return;
        for (const std::wstring& e : names)
            if (_wcsicmp(e.c_str(), n.c_str()) == 0) return;
        names.push_back(n);
    };
    add(BaseNameOf(a.filename));
    for (const std::wstring& s : KnownCopiesOf(p, a)) add(BaseNameOf(s));
    return names;
}

// One override slot as text, preserving the three states the slot really has.
//
// "(inherit from tags)" and "(none)" are NOT the same answer and the whole
// point of the slot is the difference: absent falls through to whatever the
// preset's tags select, present-and-empty deliberately suppresses it.
static std::wstring OverrideSlotText(bool present, const std::wstring& name) {
    if (!present) return L"(inherit from tags)";
    return name.empty() ? L"(none)" : name;
}

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
        y += L.lineH + L.gap;

        // A labelled single-line value, which the block below needs six of.
        auto row = [&](const wchar_t* label, const std::wstring& value) {
            HWND hL = CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE | SS_LEFT,
                x, y, L.labelW, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
            if (hL && hFont) SendMessage(hL, WM_SETFONT, (WPARAM)hFont, TRUE);
            TrackControl(hL);
            HWND hV = CreateWindowExW(0, L"STATIC", value.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                x + L.labelW + 4, y, rw - L.labelW - 4, L.lineH, m_hWnd, NULL,
                GetModuleHandle(NULL), NULL);
            if (hV && hFont) SendMessage(hV, WM_SETFONT, (WPARAM)hFont, TRUE);
            TrackControl(hV);
            y += L.lineH;
        };

        // ── Usage ──
        {
            std::wstring used = FormatLastUsed(m_annot.lastUsed);
            if (used.empty()) used = L"never played";
            wchar_t plays[96];
            if (m_annot.useCount > 0) {
                const std::wstring dur = FormatDuration(m_annot.secondsShown);
                swprintf_s(plays, L"%d  (%.40s on screen)", m_annot.useCount,
                           dur.empty() ? L"no time recorded" : dur.c_str());
            } else {
                wcscpy_s(plays, L"0");
            }
            row(L"Last used:", used);
            row(L"Plays:", plays);
        }

        // ── Identity and copies ──
        //
        // The hash IS the key into presets.json, so showing it is what makes a
        // duplicate claim checkable rather than something to take on trust.
        const std::vector<std::wstring> copies =
            m_pEngine ? KnownCopiesOf(m_pEngine, m_annot) : std::vector<std::wstring>();
        {
            row(L"Identity:", m_annot.hash.empty()
                                  ? std::wstring(L"(no content hash - keyed by filename)")
                                  : m_annot.hash);

            const std::vector<std::wstring> aliases =
                m_pEngine ? AliasesOf(m_pEngine, m_annot) : std::vector<std::wstring>();
            if (aliases.size() > 1) {
                std::wstring joined;
                for (size_t i = 0; i < aliases.size(); i++) {
                    if (i) joined += L", ";
                    joined += aliases[i];
                }
                row(L"Aliases:", joined);
            }

            // Say which number this is. "3" alone invites the reading "there are
            // exactly three", which is only true when a scan has actually run.
            const int scanned = m_pEngine ? m_pEngine->DuplicateCountFor(m_annot) : 0;
            wchar_t copiesStr[160];
            if (scanned >= 2)
                swprintf_s(copiesStr, L"%d files share this content", scanned);
            else if (scanned == 1)
                wcscpy_s(copiesStr, L"1 - no other copy found by the last scan");
            else if (copies.size() > 1)
                swprintf_s(copiesStr, L"%d known location%s (run Find Copies for a full count)",
                           (int)copies.size(), copies.size() == 1 ? L"" : L"s");
            else
                wcscpy_s(copiesStr, L"not scanned - use Find Copies");
            row(L"Copies:", copiesStr);
        }

        // ── Per-preset overrides ──
        //
        // These are otherwise only visible as four combo boxes on the main
        // window, one selection at a time; listing them here is the only place
        // a preset's whole configuration can be read at once.
        {
            row(L"Shader:", OverrideSlotText(m_annot.hasShaderOverride, m_annot.shaderOverride));
            row(L"VFX:",    OverrideSlotText(m_annot.hasVfxProfile,     m_annot.vfxProfile));
            row(L"Audio:",  OverrideSlotText(m_annot.hasAudioProfile,   m_annot.audioProfile));

            std::wstring canvas = L"(inherit from tags)";
            if (m_annot.hasCanvasMax) {
                wchar_t b[32];
                swprintf_s(b, L"%d px", m_annot.canvasMax);
                canvas = b;
            }
            row(L"Canvas:", canvas);

            // Shown even when off, next to Canvas, because the two are
            // alternatives for the same problem and reading one without the
            // other tells you half the story.
            std::wstring damp = L"Off";
            if (m_annot.hasFeedbackDamp && m_annot.feedbackDamp > 0.0f) {
                wchar_t b2[48];
                swprintf_s(b2, L"%s (%.2f)",
                           Engine::DampChoiceLabel(
                               Engine::DampChoiceIndex(m_annot.feedbackDamp)),
                           m_annot.feedbackDamp);
                damp = b2;
            }
            row(L"Damp:", damp);

            if (!m_annot.tags.empty()) {
                std::wstring joined;
                for (size_t i = 0; i < m_annot.tags.size(); i++) {
                    if (i) joined += L", ";
                    joined += m_annot.tags[i];
                }
                row(L"Tags:", joined);
            }
        }
        y += L.gap;

        // ── Where the copies are ──
        //
        // A scrollable box rather than more label rows: this is the list the
        // user has to act on to clean up, and it can be dozens of lines.
        if (!copies.empty()) {
            HWND hLblCopies = CreateWindowExW(0, L"STATIC", L"Locations:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
            if (hLblCopies && hFont) SendMessage(hLblCopies, WM_SETFONT, (WPARAM)hFont, TRUE);
            TrackControl(hLblCopies);
            y += L.lineH;

            // CRLF, not LF: a multiline EDIT renders a bare \n as a box.
            std::wstring list;
            for (const std::wstring& c : copies) {
                if (!list.empty()) list += L"\r\n";
                list += c;
                if (!PathFileExistsW(c.c_str())) list += L"   (missing)";
            }

            const int copiesH = L.lineH * 3;
            HWND hCopies = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", list.c_str(),
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                x, y, rw, copiesH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
            if (hCopies && hFont) SendMessage(hCopies, WM_SETFONT, (WPARAM)hFont, TRUE);
            TrackControl(hCopies);
            y += copiesH + L.gap;
        }
        y += 4;

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
        // Full row width, not a guessed 100px: the label is clipped to
        // "Shader" at any HUD font size above the default.
        hLbl = CreateWindowExW(0, L"STATIC", L"Shader Error:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hLbl && hFont) SendMessage(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hLbl);
        y += L.lineH;

        // When it was captured.  A stored error outlives the build that wrote
        // it, so without a date it reads as a verdict on the running build.
        if (!m_annot.errorText.empty()) {
            std::wstring when = L"Captured: ";
            when += m_annot.errorTime.empty()
                        ? std::wstring(L"(before this was recorded)")
                        : m_annot.errorTime;
            HWND hWhen = CreateWindowExW(0, L"STATIC", when.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
            if (hWhen && hFont) SendMessage(hWhen, WM_SETFONT, (WPARAM)hFont, TRUE);
            TrackControl(hWhen);
            y += L.lineH;
        }

        HWND hErrEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            m_annot.errorText.empty() ? L"(no error)" : m_annot.errorText.c_str(),
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            x, y, rw, editH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hErrEdit && hFont) SendMessage(hErrEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hErrEdit);
        y += editH + L.gap;

        // Copy Error / Close buttons
        int btnW = 100;
        HWND hCopy = CreateWindowExW(0, L"BUTTON", L"Copy Error",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
            x + rw - btnW * 2 - L.gap, y, btnW, L.lineH, m_hWnd,
            (HMENU)(INT_PTR)IDC_MW_ANNOTWIN_COPYERR, GetModuleHandle(NULL), NULL);
        if (hCopy && hFont) SendMessage(hCopy, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (hCopy) EnableWindow(hCopy, !m_annot.errorText.empty());
        TrackControl(hCopy);

        HWND hClose = CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
            x + rw - btnW, y, btnW, L.lineH, m_hWnd, (HMENU)(INT_PTR)IDCANCEL, GetModuleHandle(NULL), NULL);
        if (hClose && hFont) SendMessage(hClose, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hClose);
        y += L.lineH + L.margin;

        FitToContent(clientW, y);
    }

    LRESULT DoCommand(int id, int code, LPARAM lParam) override {
        if (id == IDC_MW_ANNOTWIN_COPYERR) {
            if (m_annot.errorText.empty()) return 0;
            const std::wstring text =
                FormatShaderErrorForClipboard(AnnotationDisplayPath(m_annot),
                                              m_annot.errorText,
                                              m_annot.errorTime);
            if (CopyTextToClipboard(m_hWnd, text.c_str()) && m_pEngine)
                m_pEngine->AddNotification(L"Shader error copied to clipboard");
            return 0;
        }
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
    // Wide enough for the "Shader Error:" label and both buttons at the HUD
    // font: at 450 the label was clipped to "Shader" and the buttons crowded
    // the error box.  Widened again for the identity block -- a 16-hex-digit
    // hash and a full preset path both live on a value line now.
    dlg.Show(m_hWnd, 680, 200);
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
            // Text, date and kind describe one event and only move together.
            if (!imp.errorText.empty() && cur->errorText.empty()) {
                cur->errorText = imp.errorText;
                cur->errorTime = imp.errorTime;
                cur->errorKind = imp.errorKind;
            }
        } else {
            // Overwrite
            cur->rating = imp.rating;
            cur->flags = imp.flags;
            cur->notes = imp.notes;
            cur->errorText = imp.errorText;
            cur->errorTime = imp.errorTime;
            cur->errorKind = imp.errorKind;
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

// ─── Duplicate scan ──────────────────────────────────────────────────────
//
// Two dialogs: one that runs the walk and one that reports it. Split because
// they have opposite risk profiles -- the scan only reads, the report deletes.

// Insert carrying an lParam. The file's other LV_InsertItemW does not, and the
// report list must map a row back to a model entry by something sturdier than
// its position.
static int LV_InsertItemParamW(HWND hLV, int row, const wchar_t* text, LPARAM param) {
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = row;
    lvi.pszText = (LPWSTR)text;
    lvi.lParam = param;
    return (int)SendMessageW(hLV, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
}

static LPARAM LV_GetItemParam(HWND hLV, int row) {
    LVITEMW lvi = {};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = row;
    if (!SendMessageW(hLV, LVM_GETITEMW, 0, (LPARAM)&lvi)) return -1;
    return lvi.lParam;
}

static std::wstring FormatFileTime(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return L"";
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local)) return L"";
    if (!FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

static std::wstring FormatBytes(uint64_t n) {
    wchar_t buf[32];
    if (n < 1024)            swprintf_s(buf, L"%llu B", (unsigned long long)n);
    else if (n < 1024 * 1024) swprintf_s(buf, L"%.1f KB", n / 1024.0);
    else                      swprintf_s(buf, L"%.1f MB", n / (1024.0 * 1024.0));
    return buf;
}

static std::wstring FolderOf(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return (cut == std::wstring::npos) ? std::wstring() : path.substr(0, cut);
}

// ── The scan, with a way out of it ──
//
// The walk runs on a worker thread and this dialog polls it. That is one more
// moving part than doing it inline, and it buys the two things inline cannot:
// the window keeps painting, and Cancel works. A preset library on a network
// share can take a long time to read and there is no way to know in advance.
//
// Nothing here calls AddNotification or touches a control from the worker --
// Engine::AddError has no lock, and the worker's only channel to the UI is
// three atomics polled on a timer.
#define IDT_ANNOT_DUPESCAN 1

class DupeScanDialog : public ModalDialog {
    std::wstring m_root;
    std::wstring m_title;
    HWND m_hStatus = NULL;
    HWND m_hBtnCancel = NULL;

    std::thread       m_worker;
    std::atomic<bool> m_cancel{ false };
    std::atomic<bool> m_finished{ false };
    std::atomic<int>  m_seen{ 0 };
    std::atomic<int>  m_total{ 0 };   // 0 while the tree is still being walked

    // Written by the worker, read by the UI thread only after m_finished is
    // observed true. The atomic's release/acquire ordering is what makes that
    // handoff safe without a mutex.
    std::vector<Engine::DuplicateGroup> m_groups;
    std::set<std::wstring> m_allHashes;

    const wchar_t* GetDialogTitle() const override { return m_title.c_str(); }
    const wchar_t* GetDialogClass() const override { return L"MDropDupeScanDlg"; }

    void DoBuildControls(int clientW, int clientH) override {
        auto L = GetBaseLayout();
        int x = L.margin, y = L.margin;
        int rw = clientW - L.margin * 2;
        HFONT hFont = GetFont();

        std::wstring intro = L"Reading every preset under:\r\n" + m_root;
        HWND hIntro = CreateWindowExW(0, L"STATIC", intro.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
            x, y, rw, L.lineH * 2, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (hIntro && hFont) SendMessage(hIntro, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hIntro);
        y += L.lineH * 2 + L.gap;

        m_hStatus = CreateWindowExW(0, L"STATIC", L"Starting...",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
            x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (m_hStatus && hFont) SendMessage(m_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(m_hStatus);
        y += L.lineH + L.gap;

        const int btnW = 110;
        m_hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
            x + rw - btnW, y, btnW, L.lineH, m_hWnd,
            (HMENU)(INT_PTR)IDCANCEL, GetModuleHandle(NULL), NULL);
        if (m_hBtnCancel && hFont) SendMessage(m_hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(m_hBtnCancel);
        y += L.lineH + L.margin;

        FitToContent(clientW, y);

        // Started here rather than before Show: the window exists by now, so a
        // scan that finishes instantly still has somewhere to report to.
        const Engine* pEng = m_pEngine;
        const std::wstring root = m_root;
        m_worker = std::thread([this, pEng, root]() {
            auto progress = [this](int done, int total, const wchar_t*) -> bool {
                m_seen.store(done, std::memory_order_relaxed);
                m_total.store(total, std::memory_order_relaxed);
                return !m_cancel.load(std::memory_order_relaxed);
            };
            std::set<std::wstring> hashes;
            auto groups = pEng->ScanForDuplicatePresets(root.c_str(), progress, &hashes);
            m_groups = std::move(groups);
            m_allHashes = std::move(hashes);
            m_finished.store(true, std::memory_order_release);
        });

        SetTimer(m_hWnd, IDT_ANNOT_DUPESCAN, 120, NULL);
    }

    LRESULT DoMessage(UINT msg, WPARAM wParam, LPARAM lParam) override {
        if (msg == WM_TIMER && wParam == IDT_ANNOT_DUPESCAN) {
            if (m_finished.load(std::memory_order_acquire)) {
                KillTimer(m_hWnd, IDT_ANNOT_DUPESCAN);
                if (m_worker.joinable()) m_worker.join();
                EndDialog(!m_cancel.load());
                return 0;
            }
            if (m_hStatus) {
                const int done = m_seen.load(std::memory_order_relaxed);
                const int total = m_total.load(std::memory_order_relaxed);
                wchar_t buf[160];
                if (m_cancel.load())
                    swprintf_s(buf, L"Stopping...");
                else if (total <= 0)
                    swprintf_s(buf, L"Looking for presets... %d found", done);
                else
                    swprintf_s(buf, L"Reading presets... %d of %d", done, total);
                SetWindowTextW(m_hStatus, buf);
            }
            return 0;
        }
        return -1;
    }

    LRESULT DoCommand(int id, int code, LPARAM lParam) override {
        if (id == IDCANCEL) {
            // Ask the worker to stop and WAIT for it -- closing the dialog here
            // would run this object's destructor while the thread still holds a
            // pointer to it. The worker tests the flag once per file, so this
            // resolves in the time it takes to hash one preset.
            m_cancel.store(true);
            if (m_hStatus) SetWindowTextW(m_hStatus, L"Stopping...");
            if (m_hBtnCancel) EnableWindow(m_hBtnCancel, FALSE);
            return 0;
        }
        return -1;
    }

public:
    DupeScanDialog(Engine* pEngine, const std::wstring& root)
        : ModalDialog(pEngine), m_root(root), m_title(L"Find Copies") {}

    // Escape breaks ModalDialog's loop without passing through DoCommand, so
    // this is the only guaranteed join point. Cancelling first keeps it short.
    ~DupeScanDialog() override {
        m_cancel.store(true);
        if (m_worker.joinable()) m_worker.join();
    }

    bool Completed() const { return m_finished.load() && !m_cancel.load(); }
    int  FilesSeen() const { return m_seen.load(); }
    std::vector<Engine::DuplicateGroup> TakeGroups() { return std::move(m_groups); }
    std::set<std::wstring> TakeHashes() { return std::move(m_allHashes); }
};

// ── The report, and the only place this app deletes a preset ──

class DuplicatesReportDialog : public ModalDialog {
    Engine* m_pEng;
    std::vector<Engine::DuplicateGroup> m_groups;
    HWND m_hList = NULL;
    HWND m_hSummary = NULL;
    std::wstring m_title;
    bool m_bDeletedAny = false;

    // Flattened view of m_groups: one entry per FILE, carrying which group it
    // came from. Row lParam is an index into this, so a row always resolves to
    // the right file even though the list shows several groups at once.
    struct Row { size_t group; size_t file; };
    std::vector<Row> m_rows;

    const wchar_t* GetDialogTitle() const override { return m_title.c_str(); }
    const wchar_t* GetDialogClass() const override { return L"MDropDupeReportDlg"; }

    void DoBuildControls(int clientW, int clientH) override {
        auto L = GetBaseLayout();
        int x = L.margin, y = L.margin;
        int rw = clientW - L.margin * 2;
        HFONT hFont = GetFont();

        m_hSummary = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, rw, L.lineH * 2, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
        if (m_hSummary && hFont) SendMessage(m_hSummary, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(m_hSummary);
        y += L.lineH * 2 + L.gap;

        const int listH = clientH - y - L.lineH * 2 - L.gap * 3 - L.margin;
        m_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
            x, y, rw, listH > 100 ? listH : 100, m_hWnd,
            (HMENU)(INT_PTR)IDC_MW_ANNOTWIN_DUPES_LIST, GetModuleHandle(NULL), NULL);
        if (m_hList && hFont) SendMessage(m_hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        ListView_SetExtendedListViewStyle(m_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
        TrackControl(m_hList);

        if (m_pEng->IsDarkTheme()) {
            SetWindowTheme(m_hList, L"DarkMode_Explorer", NULL);
            ListView_SetBkColor(m_hList, m_pEng->m_colSettingsCtrlBg);
            ListView_SetTextBkColor(m_hList, m_pEng->m_colSettingsCtrlBg);
            ListView_SetTextColor(m_hList, m_pEng->m_colSettingsText);
        }

        // No LVM_INSERTGROUP anywhere in this codebase, so grouping is a column
        // plus a fixed order -- the same shape the Hotkeys window uses for its
        // Category column. The number is what makes "these two are the same
        // file" visible at a glance.
        // Placeholder widths; AutoSizeColumns measures the real client.
        LV_InsertColumnW(m_hList, 0, L"#", 40);
        LV_InsertColumnW(m_hList, 1, L"Preset", 210);
        LV_InsertColumnW(m_hList, 2, L"Folder", 330);
        LV_InsertColumnW(m_hList, 3, L"Modified", 120);
        LV_InsertColumnW(m_hList, 4, L"Size", 70);
        AutoSizeColumns();

        Populate();
        y += (listH > 100 ? listH : 100) + L.gap;

        // Every width is MEASURED. Fixed pixel widths clipped every caption
        // here at the default HUD font -- "Tick all but newest" rendered as
        // "ick all but newe", chopped at both ends because an owner-draw button
        // centres its text and simply overflows a box too small for it.
        const int bh = L.lineH;
        // Gap between buttons, scaled with the font. L.gap (6px) is the spacing
        // between stacked ROWS and is far too tight side by side -- adjacent
        // owner-draw buttons at 6px read as one run-together control. This is
        // clear separation, not merely non-overlap.
        const int btnGap = MulDiv(14, L.lineH, 26);
        auto mk = [&](const wchar_t* text, int id, int bx) {
            const int w = BtnWidthFor(m_hWnd, hFont, text, L.lineH);
            HWND h = CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                bx, y, w, bh, m_hWnd, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
            if (h && hFont) SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
            TrackControl(h);
            return bx + w + btnGap;
        };

        // Selection helpers, left. "Tick:" carries the verb so each button can
        // be a short noun phrase and the row fits without a second line.
        {
            HWND hL = CreateWindowExW(0, L"STATIC", L"Tick:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                x, y, BtnWidthFor(m_hWnd, hFont, L"Tick:", L.lineH), L.lineH,
                m_hWnd, NULL, GetModuleHandle(NULL), NULL);
            if (hL && hFont) SendMessage(hL, WM_SETFONT, (WPARAM)hFont, TRUE);
            TrackControl(hL);

            int bx = x + BtnWidthFor(m_hWnd, hFont, L"Tick:", L.lineH);
            bx = mk(L"All but newest", IDC_MW_ANNOTWIN_DUPES_NEWEST, bx);
            bx = mk(L"Copies in subfolders", IDC_MW_ANNOTWIN_DUPES_OUTSIDE, bx);
            bx = mk(L"None", IDC_MW_ANNOTWIN_DUPES_NONE, bx);
            bx = mk(L"Copy Report", IDC_MW_ANNOTWIN_DUPES_COPY, bx);
        }
        y += L.lineH + L.gap;

        // Delete and Close, right-aligned, measured back from the edge so the
        // pair stays flush whatever the captions measure.
        {
            const int wClose = BtnWidthFor(m_hWnd, hFont, L"Close", L.lineH);
            const int wDel   = BtnWidthFor(m_hWnd, hFont, L"Delete Ticked Files...", L.lineH);
            mk(L"Delete Ticked Files...", IDC_MW_ANNOTWIN_DUPES_DELETE,
               x + rw - wClose - L.gap - wDel);
            mk(L"Close", IDCANCEL, x + rw - wClose);
        }
    }

    // Same rule as the main list: fixed columns scale with the font, and the
    // two that carry paths absorb the rest. Folder is the column a delete
    // decision is actually made on, so it gets the larger share.
    void AutoSizeColumns() {
        if (!m_hList) return;
        RECT rc;
        GetClientRect(m_hList, &rc);
        int avail = (rc.right - rc.left) - GetSystemMetrics(SM_CXVSCROLL) - 4;
        if (avail < 200) return;

        const int lineH = GetLineHeight();
        auto S = [lineH](int px) { return MulDiv(px, lineH, 26); };

        const int wNum  = S(38);
        const int wWhen = S(148);   // sized for "2026-08-25 21:07", not the header
        const int wSize = S(76);
        int rest = avail - wNum - wWhen - wSize;
        const int minName = S(120), minFolder = S(160);
        if (rest < minName + minFolder) rest = minName + minFolder;

        int wName = rest * 2 / 5;
        if (wName < minName) wName = minName;
        int wFolder = rest - wName;
        if (wFolder < minFolder) wFolder = minFolder;

        ListView_SetColumnWidth(m_hList, 0, wNum);
        ListView_SetColumnWidth(m_hList, 1, wName);
        ListView_SetColumnWidth(m_hList, 2, wFolder);
        ListView_SetColumnWidth(m_hList, 3, wWhen);
        ListView_SetColumnWidth(m_hList, 4, wSize);
    }

    void Populate() {
        if (!m_hList) return;
        ListView_DeleteAllItems(m_hList);
        m_rows.clear();

        int wasted = 0;
        uint64_t wastedBytes = 0;
        for (size_t g = 0; g < m_groups.size(); g++) {
            for (size_t f = 0; f < m_groups[g].files.size(); f++) {
                m_rows.push_back({ g, f });
                if (f > 0) { wasted++; wastedBytes += m_groups[g].files[f].sizeBytes; }
            }
        }

        for (size_t i = 0; i < m_rows.size(); i++) {
            const Engine::DuplicateGroup& g = m_groups[m_rows[i].group];
            const Engine::DuplicateFile&  f = g.files[m_rows[i].file];

            wchar_t num[16];
            swprintf_s(num, L"%d", (int)m_rows[i].group + 1);
            LV_InsertItemParamW(m_hList, (int)i, num, (LPARAM)i);

            // The name is per-FILE, not per-group: two copies with different
            // names is the interesting case, and printing the group's name on
            // every row would hide exactly that.
            const std::wstring name = BaseNameOf(f.path);
            LV_SetItemTextW(m_hList, (int)i, 1, (LPWSTR)name.c_str());
            const std::wstring folder = FolderOf(f.path);
            LV_SetItemTextW(m_hList, (int)i, 2, (LPWSTR)folder.c_str());
            const std::wstring when = FormatFileTime(f.written);
            LV_SetItemTextW(m_hList, (int)i, 3, (LPWSTR)when.c_str());
            const std::wstring size = FormatBytes(f.sizeBytes);
            LV_SetItemTextW(m_hList, (int)i, 4, (LPWSTR)size.c_str());
        }

        if (m_hSummary) {
            wchar_t buf[512];
            if (m_groups.empty()) {
                wcscpy_s(buf, L"No duplicate presets found.");
            } else {
                const std::wstring sz = FormatBytes(wastedBytes);
                swprintf_s(buf,
                    L"%d preset%s exist%s as more than one file - %d redundant "
                    L"cop%s, %.40s.\r\n"
                    L"Tick the copies to remove. Deleting sends them to the Recycle Bin.",
                    (int)m_groups.size(), m_groups.size() == 1 ? L"" : L"s",
                    m_groups.size() == 1 ? L"s" : L"",
                    wasted, wasted == 1 ? L"y" : L"ies", sz.c_str());
            }
            SetWindowTextW(m_hSummary, buf);
        }
    }

    void SetAllChecks(bool on) {
        for (int i = 0; i < (int)m_rows.size(); i++)
            ListView_SetCheckState(m_hList, i, on ? TRUE : FALSE);
    }

    // Tick every copy in a group except the one written most recently.
    //
    // Newest as the keeper rather than oldest: the bodies are identical by
    // definition, so the only thing that separates them is which one the user
    // has been touching, and that is the one their tools point at.
    void TickAllButNewest() {
        SetAllChecks(false);
        for (size_t g = 0; g < m_groups.size(); g++) {
            const auto& files = m_groups[g].files;
            size_t keep = 0;
            for (size_t f = 1; f < files.size(); f++) {
                if (CompareFileTime(&files[f].written, &files[keep].written) > 0) keep = f;
            }
            for (size_t i = 0; i < m_rows.size(); i++)
                if (m_rows[i].group == g && m_rows[i].file != keep)
                    ListView_SetCheckState(m_hList, (int)i, TRUE);
        }
    }

    // Tick the copies that sit in SUBFOLDERS, keeping the one in the preset
    // folder itself.
    //
    // The button used to say "outside preset folder", which could never match
    // anything: the scan walks the preset folder, so every file in the report
    // is under it by construction. What the comparison actually distinguishes
    // is the top folder from its subfolders, which is a genuinely useful split
    // -- it collapses themed subfolder copies back to the main collection --
    // so the behaviour is kept and the label now says what it does.
    //
    // A group with no copy in the top folder is left alone rather than emptied.
    void TickOutsidePresetDir() {
        SetAllChecks(false);
        std::wstring dir = m_pEng->m_szPresetDir;
        if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        if (dir.empty()) return;

        for (size_t g = 0; g < m_groups.size(); g++) {
            const auto& files = m_groups[g].files;
            int inside = 0;
            for (const auto& f : files)
                if (_wcsicmp(FolderOf(f.path).c_str(), dir.c_str()) == 0) inside++;
            if (inside == 0) continue;   // nothing to keep here; leave it be

            for (size_t i = 0; i < m_rows.size(); i++) {
                if (m_rows[i].group != g) continue;
                const std::wstring folder = FolderOf(files[m_rows[i].file].path);
                if (_wcsicmp(folder.c_str(), dir.c_str()) != 0)
                    ListView_SetCheckState(m_hList, (int)i, TRUE);
            }
        }
    }

    std::wstring BuildReportText() const {
        std::wstring out;
        for (size_t g = 0; g < m_groups.size(); g++) {
            wchar_t head[128];
            swprintf_s(head, L"[%d] %d copies  (hash %.20s)\r\n",
                       (int)g + 1, (int)m_groups[g].files.size(), m_groups[g].hash.c_str());
            out += head;
            for (const auto& f : m_groups[g].files)
                out += L"    " + f.path + L"\r\n";
            out += L"\r\n";
        }
        if (out.empty()) out = L"No duplicate presets found.\r\n";
        return out;
    }

    void DoDelete() {
        // Read the ticks through lParam rather than trusting row order.
        std::vector<size_t> ticked;
        for (int i = 0; i < (int)m_rows.size(); i++) {
            if (!ListView_GetCheckState(m_hList, i)) continue;
            const LPARAM idx = LV_GetItemParam(m_hList, i);
            if (idx >= 0 && (size_t)idx < m_rows.size()) ticked.push_back((size_t)idx);
        }

        if (ticked.empty()) {
            MessageBoxW(m_hWnd, L"Nothing is ticked.", L"Delete Copies",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }

        // Refuse to empty a group.
        //
        // Checked here, at the commit, rather than by fighting the user's
        // clicks: this is the one invariant that matters -- every preset must
        // survive somewhere -- and enforcing it once, where the damage would
        // actually be done, is more reliable than trying to prevent the state.
        std::map<size_t, int> tickedPerGroup;
        for (size_t idx : ticked) tickedPerGroup[m_rows[idx].group]++;
        for (const auto& kv : tickedPerGroup) {
            if (kv.second >= (int)m_groups[kv.first].files.size()) {
                wchar_t msg[320];
                swprintf_s(msg,
                    L"Group %d has every copy ticked, which would delete the "
                    L"preset entirely.\n\nUntick at least one copy in that group "
                    L"and try again.",
                    (int)kv.first + 1);
                MessageBoxW(m_hWnd, msg, L"Delete Copies", MB_OK | MB_ICONWARNING);
                return;
            }
        }

        // Says "normally", because FOF_ALLOWUNDO is a request, not a guarantee:
        // Windows deletes outright when the file is on a network drive, is
        // bigger than the bin's quota, or the bin is disabled for that volume.
        // Promising the Recycle Bin unconditionally would be the app telling
        // the user something it cannot know is true, right before it is asked
        // to delete their files.
        wchar_t prompt[448];
        swprintf_s(prompt,
            L"Delete %d file%s?\n\n"
            L"They normally go to the Recycle Bin, but Windows removes them "
            L"permanently if they are on a network drive or the bin is off or "
            L"full for that drive.\n\n"
            L"Every preset keeps at least one copy. Ratings, tags and notes are "
            L"stored against the preset's content, not its path, so they follow "
            L"the surviving copy.",
            (int)ticked.size(), ticked.size() == 1 ? L"" : L"s");
        if (MessageBoxW(m_hWnd, prompt, L"Delete Copies",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;

        // SHFileOperationW wants a double-null-terminated, null-separated list.
        std::vector<wchar_t> buf;
        for (size_t idx : ticked) {
            const std::wstring& p = m_groups[m_rows[idx].group].files[m_rows[idx].file].path;
            buf.insert(buf.end(), p.begin(), p.end());
            buf.push_back(0);
        }
        buf.push_back(0);

        SHFILEOPSTRUCTW op = {};
        op.hwnd   = m_hWnd;
        op.wFunc  = FO_DELETE;
        op.pFrom  = buf.data();
        // ALLOWUNDO is the Recycle Bin, and it is the reason this is offered at
        // all. NOCONFIRMATION only suppresses the shell's second prompt -- the
        // question was already asked above.
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

        const int rc = SHFileOperationW(&op);

        // ALWAYS rebuild from disk, including on an error return.
        //
        // Returning early on failure was a route to losing a preset entirely.
        // SHFileOperation can delete some of the list and then fail, so on the
        // error path m_groups still listed files that were already gone -- and
        // the "never empty a group" guard above compares the ticked count
        // against m_groups[g].files.size(). With a stale, too-large count, a
        // second pass could tick every file that actually remained and the
        // guard would wave it through. What is on disk is the only thing worth
        // believing here.
        int removed = 0;
        std::vector<Engine::DuplicateGroup> kept;
        for (auto& g : m_groups) {
            Engine::DuplicateGroup ng;
            ng.hash = g.hash;
            ng.displayName = g.displayName;
            for (auto& f : g.files) {
                if (PathFileExistsW(f.path.c_str())) ng.files.push_back(f);
                else removed++;
            }
            if (ng.files.size() > 1) kept.push_back(std::move(ng));
        }
        m_groups = std::move(kept);
        if (removed > 0) m_bDeletedAny = true;
        Populate();

        if (rc != 0 && !op.fAnyOperationsAborted) {
            wchar_t msg[224];
            swprintf_s(msg,
                L"Windows refused part of the delete (error %d).\n\n"
                L"%d file%s actually removed. The list has been refreshed from "
                L"what is on disk.",
                rc, removed, removed == 1 ? L" was" : L"s were");
            MessageBoxW(m_hWnd, msg, L"Delete Copies", MB_OK | MB_ICONERROR);
            return;
        }

        wchar_t done[224];
        swprintf_s(done,
            L"%d file%s removed.\n\n"
            L"Check the Recycle Bin to undo. Windows deletes permanently rather "
            L"than recycling when a file is on a network drive, is larger than "
            L"the bin allows, or the bin is turned off for that drive.",
            removed, removed == 1 ? L" was" : L"s were");
        MessageBoxW(m_hWnd, done, L"Delete Copies", MB_OK | MB_ICONINFORMATION);
    }

    LRESULT DoCommand(int id, int code, LPARAM lParam) override {
        switch (id) {
        case IDC_MW_ANNOTWIN_DUPES_NEWEST:  TickAllButNewest();     return 0;
        case IDC_MW_ANNOTWIN_DUPES_OUTSIDE: TickOutsidePresetDir(); return 0;
        case IDC_MW_ANNOTWIN_DUPES_NONE:    SetAllChecks(false);    return 0;
        case IDC_MW_ANNOTWIN_DUPES_COPY:
            if (CopyTextToClipboard(m_hWnd, BuildReportText().c_str()) && m_pEng)
                m_pEng->AddNotification(L"Duplicate report copied to clipboard");
            return 0;
        case IDC_MW_ANNOTWIN_DUPES_DELETE:  DoDelete();             return 0;
        case IDCANCEL: EndDialog(m_bDeletedAny); return 0;
        }
        return -1;
    }

public:
    DuplicatesReportDialog(Engine* pEngine, std::vector<Engine::DuplicateGroup>&& groups)
        : ModalDialog(pEngine), m_pEng(pEngine), m_groups(std::move(groups)),
          m_title(L"Duplicate Presets") {}

    bool DeletedAny() const { return m_bDeletedAny; }
    const std::vector<Engine::DuplicateGroup>& Groups() const { return m_groups; }
};

void AnnotationsWindow::DoFindCopies() {
    Engine* p = m_pEngine;

    std::wstring root = p->m_szPresetDir;
    if (root.empty()) {
        MessageBoxW(m_hWnd, L"No preset directory is set.", L"Find Copies",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    // Say what is about to be read, and from where. This walks a whole tree,
    // and on a network share that is a real cost the user should agree to
    // rather than discover.
    {
        std::wstring ask =
            L"Read every preset file under this folder and its subfolders, and "
            L"group the ones with identical content?\n\n" + root +
            L"\n\nNothing is changed by the scan.";
        if (MessageBoxW(m_hWnd, ask.c_str(), L"Find Copies",
                        MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
            return;
    }

    DupeScanDialog scan(p, root);
    scan.Show(m_hWnd, 460, 200);
    if (!scan.Completed()) return;   // cancelled: a partial count would mislead

    auto groups = scan.TakeGroups();
    // Publish before showing the report, so the Copies column and the
    // Duplicates filter are already right behind it.
    p->AdoptDuplicateScan(groups, scan.TakeHashes());

    if (groups.empty()) {
        wchar_t msg[192];
        swprintf_s(msg,
            L"Read %d preset files. No two of them have identical content.",
            scan.FilesSeen());
        MessageBoxW(m_hWnd, msg, L"Find Copies", MB_OK | MB_ICONINFORMATION);
        RefreshList();
        return;
    }

    // Sized to the monitor, not to a constant. The column that a delete
    // decision is actually made on is Folder, and at a fixed 900 it truncated
    // every path to "C:\Users\shane\AppData\L..." -- which is exactly the part
    // that is the same for every copy and tells you nothing.
    int dlgW = 1180, dlgH = 620;
    {
        RECT wa = {};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            const int maxW = (wa.right - wa.left) - 120;
            const int maxH = (wa.bottom - wa.top) - 140;
            if (dlgW > maxW) dlgW = maxW;
            if (dlgH > maxH) dlgH = maxH;
        }
        if (dlgW < 820) dlgW = 820;
        if (dlgH < 460) dlgH = 460;
    }

    DuplicatesReportDialog report(p, std::move(groups));
    report.Show(m_hWnd, dlgW, dlgH);

    // Files may be gone now, so the index has to follow. Hashes are unchanged:
    // deleting a copy does not un-see the content.
    if (report.DeletedAny()) {
        std::set<std::wstring> hashes = p->m_dupeScannedHashes;
        p->AdoptDuplicateScan(report.Groups(), std::move(hashes));
    }
    RefreshList();
}

// ─── Build Controls ─────────────────────────────────────────────────────

// ── One source of truth for this window's geometry ──────────────────────
//
// Every position was previously written out TWICE -- once in DoBuildControls to
// create the control and once in LayoutControls to move it on resize -- with a
// comment warning that the two must be kept identical. They were not:
//
//   * DoBuildControls took x and rw from BuildBaseControls (x=16, rw=clientW-32)
//     while LayoutControls hardcoded x=8, rw=clientW-16, so the entire window
//     jumped 8px left and grew 16px wider the first time it was resized;
//   * Edit had been omitted from LayoutControls entirely, and before that so had
//     Audio and Canvas, so those controls stayed at their build positions while
//     everything around them moved.
//
// Both bugs are the same bug. Computing every rectangle once, here, and having
// creation and resize both consume it removes the whole category rather than
// fixing another instance of it.
//
// It also gives the layout somewhere to handle OVERFLOW. Nothing did before: at
// a narrow width the right-aligned action buttons slid underneath the filter
// combo, and the tool-button row simply ran off the right edge.
struct AnnotGeom {
    RECT lblFilter, cbFilter;
    RECT btnDetails, btnEdit, btnLoad, btnRemove;
    RECT lblSearch, edSearch, btnClear;
    RECT btnImport, btnScan, btnReset, btnFind, btnPurge;
    RECT lblShader, cbShader, lblVfx, cbVfx;
    RECT lblAudio, cbAudio, lblCanvas, cbCanvas, lblDamp, cbDamp;
    RECT list;
};

static RECT GeomRect(int x, int y, int w, int h) {
    RECT r; r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    return r;
}

static void PlaceControl(HWND h, const RECT& r) {
    if (h) MoveWindow(h, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}

// A combo's height argument is its DROPPED height, not its closed height, so
// these rects are deliberately taller than a row. Row advance uses lineH.
static AnnotGeom ComputeAnnotGeom(HWND hw, HFONT hFont, int lineH, int topY) {
    RECT rc;
    GetClientRect(hw, &rc);
    const int clientW = rc.right;
    const int clientH = rc.bottom;

    // MUST match ToolWindow::BuildBaseControls, which places the font and pin
    // buttons at these margins. This is the value LayoutControls used to get
    // wrong; there is now only one copy of it.
    const int x   = 16;
    const int rw  = clientW - x * 2;
    const int gap = 4;
    // Side-by-side spacing. 4px reads as one run-together control at any font
    // above the default -- adjacent owner-draw buttons have no border between
    // them, so the gap is the only thing separating two captions.
    const int btnGap = MulDiv(12, lineH, 26);
    const int lblW   = MulDiv(64, lineH, 26);

    AnnotGeom g = {};
    int y = topY;

    // ── Filter combo, and the per-row action buttons ──
    const int cbFilterW = MulDiv(140, lineH, 26);
    g.lblFilter = GeomRect(x, y, lblW, lineH);
    g.cbFilter  = GeomRect(x + lblW + 4, y, cbFilterW, lineH * 7);

    const int wDetails = BtnWidthFor(hw, hFont, L"Details", lineH);
    const int wEdit    = BtnWidthFor(hw, hFont, L"Edit", lineH);
    const int wLoad    = BtnWidthFor(hw, hFont, L"Load", lineH);
    const int wRemove  = BtnWidthFor(hw, hFont, L"Remove", lineH);
    const int actionsW = wDetails + wEdit + wLoad + wRemove + btnGap * 3;

    const int filterRight = x + lblW + 4 + cbFilterW;
    if (filterRight + btnGap + actionsW > x + rw)
        y += lineH + gap;          // no room beside the combo: own row

    int bx = x + rw - actionsW;
    if (bx < x) bx = x;            // narrower than the buttons: overflow right,
                                   // but never left over the window edge
    g.btnDetails = GeomRect(bx, y, wDetails, lineH); bx += wDetails + btnGap;
    g.btnEdit    = GeomRect(bx, y, wEdit,    lineH); bx += wEdit    + btnGap;
    g.btnLoad    = GeomRect(bx, y, wLoad,    lineH); bx += wLoad    + btnGap;
    g.btnRemove  = GeomRect(bx, y, wRemove,  lineH);
    y += lineH + gap;

    // ── Live search ──
    {
        const int clearW = lineH;
        int edW = rw - lblW - 4 - clearW - 4;
        if (edW < lineH * 3) edW = lineH * 3;
        g.lblSearch = GeomRect(x, y, lblW, lineH);
        g.edSearch  = GeomRect(x + lblW + 4, y, edW, lineH);
        g.btnClear  = GeomRect(x + lblW + 4 + edW + 4, y, clearW, lineH);
    }
    y += lineH + gap;

    // ── Tool buttons, wrapping ──
    {
        struct Tool { RECT* r; const wchar_t* text; };
        const Tool tools[] = {
            { &g.btnImport, L"Import" },
            { &g.btnScan,   L"Scan" },
            { &g.btnReset,  L"Reset Use" },
            { &g.btnFind,   L"Find Copies" },
            { &g.btnPurge,  L"Purge Missing" },
        };
        int tx = x;
        for (const Tool& t : tools) {
            const int w = BtnWidthFor(hw, hFont, t.text, lineH);
            if (tx > x && tx + w > x + rw) {   // would run off: next line
                tx = x;
                y += lineH + gap;
            }
            *t.r = GeomRect(tx, y, w, lineH);
            tx += w + btnGap;
        }
    }
    y += lineH + gap;

    // ── The four per-preset override slots ──
    //
    // Two columns while both still fit a usable combo, one column below that.
    // Forcing two columns at any width squeezed the combos to a few pixels and
    // made the slot names unreadable.
    {
        // Sized so the WIDEST value a slot can show -- "(inherit from tags)" --
        // still fits. A threshold that only guaranteed a visible combo left all
        // four reading "(inherit fron" at narrow widths, which is worse than
        // stacking them: the three states are told apart by exactly that text.
        const int minCombo = MulDiv(150, lineH, 26);
        const int halfW    = (rw - 8) / 2;
        const bool twoCol  = (halfW - lblW - 4) >= minCombo;

        struct Slot { RECT* lbl; RECT* combo; };
        // The count is read off the table rather than written out, because it
        // used to be the literal 4 in four places and adding a fifth slot meant
        // finding all of them.
        const Slot slots[] = {
            { &g.lblShader, &g.cbShader },
            { &g.lblVfx,    &g.cbVfx    },
            { &g.lblAudio,  &g.cbAudio  },
            { &g.lblCanvas, &g.cbCanvas },
            { &g.lblDamp,   &g.cbDamp   },
        };
        const int nSlots = (int)_countof(slots);
        if (twoCol) {
            const int comboW = halfW - lblW - 4;
            const int x2 = x + halfW + 8;
            for (int i = 0; i < nSlots; i++) {
                const int cx = (i % 2) ? x2 : x;
                const int cy = y + (i / 2) * (lineH + gap);
                *slots[i].lbl   = GeomRect(cx, cy, lblW, lineH);
                *slots[i].combo = GeomRect(cx + lblW + 4, cy, comboW, lineH * 8);
            }
            y += ((nSlots + 1) / 2) * (lineH + gap);
        } else {
            int comboW = rw - lblW - 4;
            if (comboW < lineH * 3) comboW = lineH * 3;
            for (int i = 0; i < nSlots; i++) {
                const int cy = y + i * (lineH + gap);
                *slots[i].lbl   = GeomRect(x, cy, lblW, lineH);
                *slots[i].combo = GeomRect(x + lblW + 4, cy, comboW, lineH * 8);
            }
            y += nSlots * (lineH + gap);
        }
    }

    // ── The list takes whatever is left ──
    int listH = clientH - y - 8;
    if (listH < 3 * lineH) listH = 3 * lineH;
    g.list = GeomRect(x, y, rw, listH);
    return g;
}

void AnnotationsWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    auto L = BuildBaseControls();
    int x = L.x, y = L.y, rw = L.rw;
    HFONT hFont = GetFont();
    m_nTopY = y;

    int lineH = GetLineHeight();
    int gap = 4;
    const AnnotGeom G = ComputeAnnotGeom(hw, hFont, lineH, y);

    // Row 1: Filter combo + action buttons.  Positions come from G; this
    // function's job is now only to create the controls and fill them in.
    {
        m_hLblFilter = CreateLabel(hw, L"Filter:", G.lblFilter.left, G.lblFilter.top,
                                   G.lblFilter.right - G.lblFilter.left, lineH, hFont);
        m_hFilterCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            G.cbFilter.left, G.cbFilter.top,
            G.cbFilter.right - G.cbFilter.left, G.cbFilter.bottom - G.cbFilter.top, hw,
            (HMENU)(INT_PTR)IDC_MW_ANNOTWIN_FILTER, GetModuleHandle(NULL), NULL);
        if (m_hFilterCombo && hFont) SendMessage(m_hFilterCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"All");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Favorites");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Errors");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Skipped");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Broken");
        SendMessageW(m_hFilterCombo, CB_ADDSTRING, 0, (LPARAM)L"Duplicates");
        // The SELECTION, not index 0. m_nFilterMode outlives the HWND (see
        // tool_window.h), so hardcoding 0 here made a reopened window show
        // "All" while still filtering to whatever was picked last time.
        SendMessage(m_hFilterCombo, CB_SETCURSEL, m_nFilterMode, 0);

        auto mkBtn = [&](const wchar_t* text, int id, const RECT& r) {
            return CreateBtn(hw, text, id, r.left, r.top, r.right - r.left, lineH, hFont);
        };
        m_hBtnDetails = mkBtn(L"Details", IDC_MW_ANNOTWIN_DETAILS, G.btnDetails);
        m_hBtnEdit    = mkBtn(L"Edit",    IDC_MW_ANNOTWIN_EDIT,    G.btnEdit);
        m_hBtnLoad    = mkBtn(L"Load",    IDC_MW_ANNOTWIN_LOAD,    G.btnLoad);
        m_hBtnRemove  = mkBtn(L"Remove",  IDC_MW_ANNOTWIN_REMOVE,  G.btnRemove);

        HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hw, NULL, GetModuleHandle(NULL), NULL);
        TrackTooltip(hTip);
        if (hTip && m_hBtnEdit) {
            TTTOOLINFOW ti = { sizeof(ti) };
            ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            ti.hwnd = hw;
            ti.uId = (UINT_PTR)m_hBtnEdit;
            ti.lpszText = (LPWSTR)L"Edit Preset";
            SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }

    // Live search on its own row: squeezing it beside the filter combo and the
    // right-aligned buttons overflowed the default 705px window.
    // EN_CHANGE fires per keystroke, which is the point -- the list narrows
    // from the first letter typed.
    {
        m_hLblSearch = CreateLabel(hw, L"Find:", G.lblSearch.left, G.lblSearch.top,
                                   G.lblSearch.right - G.lblSearch.left, lineH, hFont);
        m_hSearchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            G.edSearch.left, G.edSearch.top,
            G.edSearch.right - G.edSearch.left, lineH, hw,
            (HMENU)(INT_PTR)IDC_MW_ANNOTWIN_SEARCH, GetModuleHandle(NULL), NULL);
        if (m_hSearchEdit && hFont) SendMessage(m_hSearchEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Put the surviving query BACK in the box.
        //
        // This is the whole bug: m_searchQuery outlives the HWND, the edit is
        // rebuilt empty, and an empty edit sends no EN_CHANGE when you press
        // backspace -- so the window reopened filtered by a query that was
        // invisible and unreachable. Restoring the text makes the two agree
        // again; the clear button beside it means an empty box is never the
        // only way back to an unfiltered list.
        if (m_hSearchEdit && !m_searchQuery.empty())
            SetWindowTextW(m_hSearchEdit, m_searchQuery.c_str());

        m_hBtnSearchClear = CreateBtn(hw, L"\x2715", IDC_MW_ANNOTWIN_SEARCHCLEAR,
                                      G.btnClear.left, G.btnClear.top,
                                      G.btnClear.right - G.btnClear.left, lineH, hFont);

        HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hw, NULL, GetModuleHandle(NULL), NULL);
        TrackTooltip(hTip);
        if (hTip && m_hBtnSearchClear) {
            TTTOOLINFOW ti = { sizeof(ti) };
            ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            ti.hwnd = hw;
            ti.uId = (UINT_PTR)m_hBtnSearchClear;
            ti.lpszText = (LPWSTR)L"Clear the search";
            SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }

    // Row 3: tool buttons.  Find Copies reads files; Purge Missing edits the
    // database.  Kept apart from each other and from anything that deletes,
    // because "tidy the list" and "delete off my disk" must never be one slip
    // apart.  G already wrapped this row if it did not fit.
    {
        auto mkBtn = [&](const wchar_t* text, int id, const RECT& r) {
            return CreateBtn(hw, text, id, r.left, r.top, r.right - r.left, lineH, hFont);
        };
        m_hBtnImport       = mkBtn(L"Import",        IDC_MW_ANNOTWIN_IMPORT,      G.btnImport);
        m_hBtnScan         = mkBtn(L"Scan",          IDC_MW_ANNOTWIN_SCAN,        G.btnScan);
        m_hBtnResetUse     = mkBtn(L"Reset Use",     IDC_MW_ANNOTWIN_RESETUSE,    G.btnReset);
        m_hBtnFindCopies   = mkBtn(L"Find Copies",   IDC_MW_ANNOTWIN_FINDCOPIES,  G.btnFind);
        m_hBtnPurgeMissing = mkBtn(L"Purge Missing", IDC_MW_ANNOTWIN_REMOVEMISSING, G.btnPurge);
    }

    // Row 3: this preset's own override slots
    //
    // Tags stay generic; these are how one preset gets different treatment
    // without a tag being invented to name it. Three states each -- see
    // RefreshOverrideCombos.
    {
        // Canvas is a per-preset override like the other three, so it sits with
        // them. Two columns while both still fit a usable combo, one column
        // below that -- G decides which, so a narrow window stacks them rather
        // than squeezing four unreadable combos onto two rows.
        struct Col { HWND* lbl; HWND* combo; const wchar_t* text; int id;
                     const RECT* rLbl; const RECT* rCombo; };
        const Col cols[] = {
            { &m_hLblShader, &m_hShaderCombo, L"Shader:", IDC_MW_ANNOT_SHADEROV,  &G.lblShader, &G.cbShader },
            { &m_hLblVfx,    &m_hVfxCombo,    L"VFX:",    IDC_MW_ANNOT_VFXPROF,   &G.lblVfx,    &G.cbVfx    },
            { &m_hLblAudio,  &m_hAudioCombo,  L"Audio:",  IDC_MW_ANNOT_AUDIOPROF, &G.lblAudio,  &G.cbAudio  },
            { &m_hLblCanvas, &m_hCanvasCombo, L"Canvas:", IDC_MW_ANNOT_CANVASMAX, &G.lblCanvas, &G.cbCanvas },
            { &m_hLblDamp,   &m_hDampCombo,   L"Damp:",   IDC_MW_ANNOT_DAMPSTR,   &G.lblDamp,   &G.cbDamp   },
        };
        for (int c = 0; c < (int)_countof(cols); c++) {
            const RECT& rl = *cols[c].rLbl;
            const RECT& rc2 = *cols[c].rCombo;
            *cols[c].lbl = CreateLabel(hw, cols[c].text, rl.left, rl.top,
                                       rl.right - rl.left, lineH, hFont);
            *cols[c].combo = CreateWindowExW(0, L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                rc2.left, rc2.top, rc2.right - rc2.left, rc2.bottom - rc2.top, hw,
                (HMENU)(INT_PTR)cols[c].id, GetModuleHandle(NULL), NULL);
            if (*cols[c].combo && hFont)
                SendMessage(*cols[c].combo, WM_SETFONT, (WPARAM)hFont, TRUE);
        }
        // Fixed choices, high to low -- the canvas only ever steps down.
        if (m_hCanvasCombo) {
            for (const wchar_t* it : { L"Auto", L"1920", L"1440", L"1080", L"768", L"512" })
                SendMessageW(m_hCanvasCombo, CB_ADDSTRING, 0, (LPARAM)it);
            SendMessage(m_hCanvasCombo, CB_SETCURSEL, 0, 0);
        }
        // The OTHER runaway mitigation: leaves the canvas alone and takes the
        // energy out of the feedback loop instead. Independent of Canvas --
        // either, both or neither -- because which one a given preset survives
        // is not something this code can decide.
        if (m_hDampCombo) {
            for (int i = 0; i < (int)_countof(Engine::kDampChoices); i++)
                SendMessageW(m_hDampCombo, CB_ADDSTRING, 0,
                             (LPARAM)Engine::DampChoiceLabel(i));
            SendMessage(m_hDampCombo, CB_SETCURSEL, 0, 0);
        }
    }

    // ListView fills the rest of the window
    {
        m_hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            G.list.left, G.list.top, G.list.right - G.list.left,
            G.list.bottom - G.list.top,
            hw, (HMENU)(INT_PTR)IDC_MW_ANNOTWIN_LIST, GetModuleHandle(NULL), NULL);
        if (m_hListView && hFont) SendMessage(m_hListView, WM_SETFONT, (WPARAM)hFont, TRUE);
        ListView_SetExtendedListViewStyle(m_hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Apply dark theme if needed
        if (p->IsDarkTheme()) {
            SetWindowTheme(m_hListView, L"DarkMode_Explorer", NULL);
            ListView_SetBkColor(m_hListView, p->m_colSettingsCtrlBg);
            ListView_SetTextBkColor(m_hListView, p->m_colSettingsCtrlBg);
            ListView_SetTextColor(m_hListView, p->m_colSettingsText);
        }

        // Order matches AnnotCol. Notes is last because it is the only column
        // with unbounded content -- anything after it would be pushed off the
        // right edge on every row that has a long note.
        LV_InsertColumnW(m_hListView, ANNOT_COL_PRESET,   L"Preset", 210);
        LV_InsertColumnW(m_hListView, ANNOT_COL_COPIES,   L"Copies", 52);
        LV_InsertColumnW(m_hListView, ANNOT_COL_RATING,   L"Rating", 72);
        LV_InsertColumnW(m_hListView, ANNOT_COL_FLAGS,    L"Flags", 56);
        LV_InsertColumnW(m_hListView, ANNOT_COL_LASTUSED, L"Last Used", 112);
        LV_InsertColumnW(m_hListView, ANNOT_COL_PLAYS,    L"Plays", 46);
        LV_InsertColumnW(m_hListView, ANNOT_COL_TIME,     L"Time", 66);
        LV_InsertColumnW(m_hListView, ANNOT_COL_NOTES,    L"Notes", 160);
        AutoSizeListColumns();   // the widths above are placeholders

        RefreshList();
    }

    RefreshOverrideCombos();
}

// ─── Per-preset override slots ──────────────────────────────────────────
//
// Each combo carries THREE states, and the middle one is the one that gets
// forgotten when this kind of feature is built:
//
//   index 0   (inherit from tags)   the member is ABSENT
//   index 1   (none)                the member is PRESENT and empty
//   index 2+  a name                the member is PRESENT and named
//
// "Absent" and "present but empty" are not the same thing: the first falls
// through to whatever the preset's tags select, the second deliberately
// suppresses it. Collapsing them would make it impossible to exclude one
// preset from a tag it legitimately carries.

void AnnotationsWindow::RefreshOverrideCombos() {
    if (!m_hShaderCombo || !m_hVfxCombo || !m_hAudioCombo) return;

    const std::wstring fn = GetSelectedFilename();
    PresetAnnotation* a = fn.empty() ? nullptr : m_pEngine->GetAnnotation(fn.c_str());

    // The canvas combo follows the selection for the same reason the others do:
    // otherwise it shows one preset's limit while you are editing another's.
    if (m_hCanvasCombo) {
        static const int kChoices[] = { 0, 1920, 1440, 1080, 768, 512 };
        const int cur = (a && a->hasCanvasMax) ? a->canvasMax : 0;
        int idx = 0;
        for (int i = 0; i < (int)_countof(kChoices); i++)
            if (kChoices[i] == cur) { idx = i; break; }
        SendMessage(m_hCanvasCombo, CB_SETCURSEL, idx, 0);
    }
    if (m_hDampCombo) {
        const float cur = (a && a->hasFeedbackDamp) ? a->feedbackDamp : 0.0f;
        SendMessage(m_hDampCombo, CB_SETCURSEL, Engine::DampChoiceIndex(cur), 0);
    }

    struct Slot {
        HWND hCombo;
        std::vector<std::wstring>* names;
        bool present;
        std::wstring value;
    };
    std::vector<std::wstring> shaderList = ShaderOverrides().Names();
    std::vector<std::wstring> vfxList;
    m_pEngine->m_vfxProfiles.Names(vfxList);
    std::vector<std::wstring> audioList;
    AudioProfiles().Names(audioList);

    Slot slots[3] = {
        { m_hShaderCombo, &m_shaderNames,
          a && a->hasShaderOverride, a ? a->shaderOverride : std::wstring() },
        { m_hVfxCombo, &m_vfxProfNames,
          a && a->hasVfxProfile, a ? a->vfxProfile : std::wstring() },
        { m_hAudioCombo, &m_audioProfNames,
          a && a->hasAudioProfile, a ? a->audioProfile : std::wstring() },
    };
    const std::vector<std::wstring>* source[3] = { &shaderList, &vfxList, &audioList };

    for (int i = 0; i < 3; i++) {
        Slot& sl = slots[i];
        SendMessage(sl.hCombo, CB_RESETCONTENT, 0, 0);
        sl.names->clear();
        SendMessageW(sl.hCombo, CB_ADDSTRING, 0, (LPARAM)L"(inherit from tags)");
        SendMessageW(sl.hCombo, CB_ADDSTRING, 0, (LPARAM)L"(none)");
        for (const auto& n : *source[i]) {
            SendMessageW(sl.hCombo, CB_ADDSTRING, 0, (LPARAM)n.c_str());
            sl.names->push_back(n);
        }

        int idx = 0;                       // absent -> inherit
        if (sl.present) {
            idx = 1;                       // present and empty -> none
            for (size_t k = 0; k < sl.names->size(); k++)
                if (_wcsicmp((*sl.names)[k].c_str(), sl.value.c_str()) == 0) {
                    idx = (int)k + 2;
                    break;
                }
        }
        SendMessage(sl.hCombo, CB_SETCURSEL, idx, 0);
        EnableWindow(sl.hCombo, a != nullptr);
    }
}

void AnnotationsWindow::ApplyOverrideCombo(int slot) {
    const std::wstring fn = GetSelectedFilename();
    if (fn.empty()) return;

    HWND hCombo = (slot == 0) ? m_hShaderCombo
                : (slot == 1) ? m_hVfxCombo : m_hAudioCombo;
    const std::vector<std::wstring>& names =
        (slot == 0) ? m_shaderNames : (slot == 1) ? m_vfxProfNames : m_audioProfNames;
    const int idx = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (idx < 0) return;

    // idx 0 clears the member; 1 sets it present-and-empty; 2+ names it.
    const bool present = (idx >= 1);
    std::wstring value;
    if (idx >= 2 && (size_t)(idx - 2) < names.size()) value = names[idx - 2];

    if (slot == 0)      m_pEngine->SetPresetShaderOverride(fn.c_str(), value, present);
    else if (slot == 1) m_pEngine->SetPresetVFXProfile(fn.c_str(), value, present);
    else                m_pEngine->SetPresetAudioProfile(fn.c_str(), value, present);

    // The edited preset may be the one on screen, so re-resolve rather than
    // waiting for the next preset change to show it. Audio resolves separately
    // because it is not gated on the custom-shader master switch.
    if (slot == 2) {
        m_pEngine->ResolveAudioProfileForPreset(m_pEngine->m_pState);
    } else {
        m_pEngine->ResolveShaderOverrideForPreset(m_pEngine->m_pState);
        if (slot == 0) m_pEngine->RequestShaderRecompile();
    }
}

// ─── Layout on Resize ───────────────────────────────────────────────────

void AnnotationsWindow::OnResize() {
    LayoutControls();
}

// Fit the eight columns to the window that actually exists.
//
// Two failures this replaces. Fixed pixel widths truncate their own headers as
// soon as the HUD font grows -- "Rating" became "Rati..." at one notch up --
// and they never follow a resize, so widening the window just added empty space
// to the right of Notes. Everything here scales with the line height the way
// the rest of this file's geometry does, and the two open-ended columns absorb
// whatever is left.
void AnnotationsWindow::AutoSizeListColumns() {
    if (!m_hListView) return;

    RECT rc;
    GetClientRect(m_hListView, &rc);
    int avail = (rc.right - rc.left) - GetSystemMetrics(SM_CXVSCROLL) - 4;
    if (avail < 120) return;   // too small to be worth arranging

    const int lineH = GetLineHeight();
    auto S = [lineH](int px) { return MulDiv(px, lineH, 26); };

    // Bounded content: each is wide enough for its own header at this font.
    const int wCopies = S(58);
    const int wRating = S(78);
    const int wFlags  = S(60);
    // Sized for the VALUE, not the header: "2026-08-25 21:07" is wider than
    // the words "Last Used", and a date column that ends in "..." is useless
    // for the one thing it is for -- telling last month from last night.
    const int wLast    = S(148);
    const int wPlays   = S(52);
    const int wTime    = S(72);
    const int fixed = wCopies + wRating + wFlags + wLast + wPlays + wTime;

    // Preset and Notes share the remainder 3:2 -- a preset name is the thing
    // being identified, a note is a reminder and can be read in Details.
    int rest = avail - fixed;
    const int minPreset = S(110);
    const int minNotes  = S(60);
    if (rest < minPreset + minNotes) rest = minPreset + minNotes;
    int wPreset = rest * 3 / 5;
    int wNotes  = rest - wPreset;
    if (wPreset < minPreset) wPreset = minPreset;
    if (wNotes  < minNotes)  wNotes  = minNotes;

    ListView_SetColumnWidth(m_hListView, ANNOT_COL_PRESET,   wPreset);
    ListView_SetColumnWidth(m_hListView, ANNOT_COL_COPIES,   wCopies);
    ListView_SetColumnWidth(m_hListView, ANNOT_COL_RATING,   wRating);
    ListView_SetColumnWidth(m_hListView, ANNOT_COL_FLAGS,    wFlags);
    ListView_SetColumnWidth(m_hListView, ANNOT_COL_LASTUSED, wLast);
    ListView_SetColumnWidth(m_hListView, ANNOT_COL_PLAYS,    wPlays);
    ListView_SetColumnWidth(m_hListView, ANNOT_COL_TIME,     wTime);
    ListView_SetColumnWidth(m_hListView, ANNOT_COL_NOTES,    wNotes);
}

void AnnotationsWindow::LayoutControls() {
    if (!m_hWnd || !m_hListView) return;

    // Same geometry the controls were CREATED from -- see ComputeAnnotGeom.
    // This function used to recompute every rectangle by hand from a different
    // set of constants, which is why the window shifted on its first resize and
    // why controls that were forgotten here never moved at all.
    const AnnotGeom G = ComputeAnnotGeom(m_hWnd, m_hFont, GetLineHeight(), m_nTopY);

    PlaceControl(m_hLblFilter,   G.lblFilter);
    PlaceControl(m_hFilterCombo, G.cbFilter);
    PlaceControl(m_hBtnDetails,  G.btnDetails);
    PlaceControl(m_hBtnEdit,     G.btnEdit);
    PlaceControl(m_hBtnLoad,     G.btnLoad);
    PlaceControl(m_hBtnRemove,   G.btnRemove);

    PlaceControl(m_hLblSearch,      G.lblSearch);
    PlaceControl(m_hSearchEdit,     G.edSearch);
    PlaceControl(m_hBtnSearchClear, G.btnClear);

    PlaceControl(m_hBtnImport,       G.btnImport);
    PlaceControl(m_hBtnScan,         G.btnScan);
    PlaceControl(m_hBtnResetUse,     G.btnReset);
    PlaceControl(m_hBtnFindCopies,   G.btnFind);
    PlaceControl(m_hBtnPurgeMissing, G.btnPurge);

    PlaceControl(m_hLblShader, G.lblShader);  PlaceControl(m_hShaderCombo, G.cbShader);
    PlaceControl(m_hLblVfx,    G.lblVfx);     PlaceControl(m_hVfxCombo,    G.cbVfx);
    PlaceControl(m_hLblAudio,  G.lblAudio);   PlaceControl(m_hAudioCombo,  G.cbAudio);
    PlaceControl(m_hLblCanvas, G.lblCanvas);  PlaceControl(m_hCanvasCombo, G.cbCanvas);
    PlaceControl(m_hLblDamp,   G.lblDamp);    PlaceControl(m_hDampCombo,   G.cbDamp);

    PlaceControl(m_hListView, G.list);
    AutoSizeListColumns();   // after the move: it measures the new client width
}

// ─── Commands ───────────────────────────────────────────────────────────

LRESULT AnnotationsWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    Engine* p = m_pEngine;

    // Filter changed
    if (id == IDC_MW_ANNOTWIN_SEARCH && code == EN_CHANGE) {
        // Per keystroke, deliberately: the list narrows as you type.
        wchar_t buf[256] = {};
        if (m_hSearchEdit) GetWindowTextW(m_hSearchEdit, buf, _countof(buf));
        m_searchQuery = buf;
        RefreshList();
        return 0;
    }
    // Clear the search. Sets both the control and the query: setting only the
    // control would leave the filter live, and setting only the query would
    // leave stale text on screen -- the two halves of the same bug.
    if (id == IDC_MW_ANNOTWIN_SEARCHCLEAR && code == BN_CLICKED) {
        m_searchQuery.clear();
        if (m_hSearchEdit) {
            SetWindowTextW(m_hSearchEdit, L"");
            SetFocus(m_hSearchEdit);   // ready to type the next one
        }
        RefreshList();
        return 0;
    }
    if (id == IDC_MW_ANNOTWIN_FILTER && code == CBN_SELCHANGE) {
        m_nFilterMode = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);
        RefreshList();
        return 0;
    }
    if (id == IDC_MW_ANNOTWIN_FINDCOPIES && code == BN_CLICKED) {
        DoFindCopies();
        return 0;
    }

    if (id == IDC_MW_ANNOT_SHADEROV && code == CBN_SELCHANGE) {
        ApplyOverrideCombo(0);
        return 0;
    }
    if (id == IDC_MW_ANNOT_AUDIOPROF && code == CBN_SELCHANGE) {
        ApplyOverrideCombo(2);
        return 0;
    }
    if (id == IDC_MW_ANNOT_VFXPROF && code == CBN_SELCHANGE) {
        ApplyOverrideCombo(1);
        return 0;
    }
    if (id == IDC_MW_ANNOT_CANVASMAX && code == CBN_SELCHANGE) {
        static const int kChoices[] = { 0, 1920, 1440, 1080, 768, 512 };
        const int sel = (int)SendMessage(m_hCanvasCombo, CB_GETCURSEL, 0, 0);
        const std::wstring fn = GetSelectedFilename();
        if (sel >= 0 && sel < (int)_countof(kChoices) && !fn.empty()) {
            // Same setter the Presets context menu and IPC use, so the surfaces
            // cannot drift.
            m_pEngine->SetPresetCanvasMaxByFile(fn.c_str(), kChoices[sel]);
            RefreshList();
        }
        return 0;
    }
    if (id == IDC_MW_ANNOT_DAMPSTR && code == CBN_SELCHANGE) {
        const int sel = (int)SendMessage(m_hDampCombo, CB_GETCURSEL, 0, 0);
        const std::wstring fn = GetSelectedFilename();
        if (sel >= 0 && sel < (int)_countof(Engine::kDampChoices) && !fn.empty()) {
            m_pEngine->SetPresetFeedbackDampByFile(fn.c_str(), Engine::kDampChoices[sel]);
            RefreshList();
        }
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

    // Reset play count / time — for the selected preset, or for every preset.
    // Clearing everything is destructive and unrecoverable, so it asks first.
    if (id == IDC_MW_ANNOTWIN_RESETUSE && code == BN_CLICKED) {
        std::wstring fn = GetSelectedFilename();
        if (fn.empty()) {
            if (MessageBoxW(m_hWnd,
                    L"No preset selected.\n\n"
                    L"Reset the play count and time for EVERY preset?",
                    L"Reset Usage", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
            p->ResetUsageStats(nullptr);
        } else {
            p->ResetUsageStats(fn.c_str());
        }
        RefreshList();
        return 0;
    }

    // Load selected preset
    // Context-menu actions. code==0 for menu commands.
    if (code == 0) {
        const std::wstring cfn = GetSelectedFilename();
        if (!cfn.empty()) {
            PresetAnnotation* ca = p->GetAnnotation(cfn.c_str());
            if (id == IDC_MW_ANNOT_FAV) {
                p->SetPresetFlag(cfn.c_str(), PFLAG_FAVORITE, !(ca && (ca->flags & PFLAG_FAVORITE)));
                RefreshList(); return 0;
            }
            if (id == IDC_MW_ANNOT_SKIP) {
                p->SetPresetFlag(cfn.c_str(), PFLAG_SKIP, !(ca && (ca->flags & PFLAG_SKIP)));
                RefreshList(); return 0;
            }
            if (id == IDC_MW_ANNOT_BROKEN) {
                p->SetPresetFlag(cfn.c_str(), PFLAG_BROKEN, !(ca && (ca->flags & PFLAG_BROKEN)));
                RefreshList(); return 0;
            }
            if (id >= IDC_MW_ANNOT_RATE_BASE && id <= IDC_MW_ANNOT_RATE_BASE + 5) {
                p->SetPresetRatingForFile(cfn.c_str(), id - IDC_MW_ANNOT_RATE_BASE);
                RefreshList(); return 0;
            }
            if (id >= IDC_MW_ANNOT_CANVAS_BASE && id <= IDC_MW_ANNOT_CANVAS_BASE + 5) {
                static const int kChoices[] = { 0, 1920, 1440, 1080, 768, 512 };
                p->SetPresetCanvasMaxByFile(cfn.c_str(), kChoices[id - IDC_MW_ANNOT_CANVAS_BASE]);
                RefreshOverrideCombos(); RefreshList(); return 0;
            }
            if (id == IDC_MW_ANNOT_CANVAS_CLEAR) {
                p->SetPresetCanvasMaxByFile(cfn.c_str(), 0);
                RefreshOverrideCombos(); RefreshList(); return 0;
            }
            if (id >= IDC_MW_ANNOT_DAMP_BASE &&
                id < IDC_MW_ANNOT_DAMP_BASE + (int)_countof(Engine::kDampChoices)) {
                p->SetPresetFeedbackDampByFile(
                    cfn.c_str(), Engine::kDampChoices[id - IDC_MW_ANNOT_DAMP_BASE]);
                RefreshOverrideCombos(); RefreshList(); return 0;
            }
            if (id == IDC_MW_ANNOT_DAMP_CLEAR) {
                p->SetPresetFeedbackDampByFile(cfn.c_str(), 0.0f);
                RefreshOverrideCombos(); RefreshList(); return 0;
            }
        }
    }

    if (id == IDC_MW_ANNOTWIN_LOAD) {
        // Resolve where the preset actually lives rather than assuming the
        // directory currently browsed: annotations outlive directory changes,
        // and building m_szPresetDir + filename silently failed for every
        // preset stored elsewhere.
        std::wstring fn = GetSelectedFilename();
        if (!fn.empty()) {
            PresetAnnotation* a = p->GetAnnotation(fn.c_str());
            const std::wstring path = a ? p->ResolveAnnotationPath(*a) : std::wstring();
            if (path.empty()) {
                // Say so instead of appearing to do nothing.
                MessageBoxW(m_hWnd,
                    L"That preset's file cannot be found in any of its "
                    L"recorded locations.\n\n"
                    L"Use 'Remove Missing Presets' on the right-click menu to "
                    L"clear out entries whose files are gone.",
                    L"Preset not found", MB_OK | MB_ICONWARNING);
            } else {
                RequestLoadPreset(-1, path.c_str());
            }
        }
        return 0;
    }
    // Purge Missing — drops entries whose preset file is gone.
    //
    // Reached from the "Purge Missing" button and from the row context menu, on
    // the same id, so the two cannot drift apart. It edits presets.json only and
    // never touches a file on disk; the duplicates report is where files are
    // deleted, deliberately somewhere else entirely.
    if (id == IDC_MW_ANNOTWIN_REMOVEMISSING) {
        // Count first and name the number in the prompt. This throws away
        // ratings, notes and play history that cannot be recovered, so the
        // user gets to see the size of it before agreeing rather than after.
        int doomed = 0;
        for (const auto& kv : p->m_presetAnnotations)
            if (p->IsAnnotationKnownMissing(kv.second)) doomed++;

        if (doomed == 0) {
            MessageBoxW(m_hWnd,
                L"Every annotated preset was found. Nothing to purge.",
                L"Purge Missing", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        wchar_t prompt[512];
        swprintf_s(prompt,
            L"%d annotation%s name%s a location on disk, and the file is no "
            L"longer at any of them.\n\n"
            L"Purging drops %s entr%s from presets.json -- rating, tags, notes "
            L"and play history included. This cannot be undone.\n\n"
            L"Entries that were never seen at a recorded path are NOT touched.\n\n"
            L"Purge them?",
            doomed, doomed == 1 ? L"" : L"s", doomed == 1 ? L"s" : L"",
            doomed == 1 ? L"that" : L"those", doomed == 1 ? L"y" : L"ies");
        if (MessageBoxW(m_hWnd, prompt, L"Purge Missing",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return 0;

        const int n = p->RemoveMissingAnnotations();
        RefreshList();
        wchar_t msg[128];
        swprintf_s(msg, L"Purged %d annotation%s whose preset file is missing.",
                   n, n == 1 ? L"" : L"s");
        MessageBoxW(m_hWnd, msg, L"Purge Missing", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Edit: open the Preset Editor on the selected preset.  The editor always
    // edits the LIVE preset, so load the selection first -- through the render
    // thread, never inline (see the RequestLoadPreset note in tool_window.h).
    if (id == IDC_MW_ANNOTWIN_EDIT && code == BN_CLICKED) {
        std::wstring fn = GetSelectedFilename();
        if (!fn.empty()) {
            wchar_t szFile[MAX_PATH];
            swprintf(szFile, MAX_PATH, L"%s%s", p->m_szPresetDir, fn.c_str());
            RequestLoadPreset(-1, szFile, 0.0f);
        }
        p->OpenPresetEditorWindow();
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
        // The override combos describe the SELECTED preset, so they have to
        // follow the selection or they would show one preset's slots while
        // editing another's.
        // Right-click a row -> the same per-preset actions the Presets browser
        // offers, so the two windows behave the same way on the same data.
        if (pnm->code == NM_RCLICK) {
            NMITEMACTIVATE* nia = (NMITEMACTIVATE*)pnm;
            if (nia->iItem >= 0) {
                ListView_SetItemState(m_hListView, nia->iItem,
                                      LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                POINT pt = nia->ptAction;
                ClientToScreen(m_hListView, &pt);
                ShowRowContextMenu(pt.x, pt.y);
            }
            return 0;
        }
        if (pnm->code == LVN_COLUMNCLICK) {
            NMLISTVIEW* nlv = (NMLISTVIEW*)pnm;
            if (nlv->iSubItem == m_nSortColumn)
                m_bSortAscending = !m_bSortAscending;   // same column: flip
            else {
                m_nSortColumn = nlv->iSubItem;
                m_bSortAscending = true;                // new column: ascending
            }
            RefreshList();
            return 0;
        }
        if (pnm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* nlv = (NMLISTVIEW*)pnm;
            if (nlv->uChanged & LVIF_STATE) RefreshOverrideCombos();
            return -1;   // not consumed: default handling still applies
        }
    }
    return -1;
}

} // namespace mdrop
