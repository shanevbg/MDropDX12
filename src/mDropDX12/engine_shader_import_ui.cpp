// engine_shader_import_ui.cpp — Shader Import ToolWindow implementation.
//
// Provides paste GLSL → auto-convert to HLSL → live preview → save as .milk preset.
// GLSL→HLSL conversion ported from Milkwave Remote ShaderHelper.cs.

#include "tool_window.h"
#include "engine.h"
#include "state.h"
#include "json_utils.h"
#include "utility.h"
#include "md_defines.h"
#include <CommCtrl.h>
#include <commdlg.h>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace mdrop {

// Forward declarations
static std::string WideHlslToNarrow(const std::wstring& hlslW, int len);

// Channel source lookup tables
static const char* kChannelSamplers[] = {
    "sampler_noise_lq",    // CHAN_NOISE_LQ
    "sampler_noise_mq",    // CHAN_NOISE_MQ
    "sampler_noise_hq",    // CHAN_NOISE_HQ
    "sampler_feedback",    // CHAN_FEEDBACK
    "sampler_noisevol_lq", // CHAN_NOISEVOL_LQ
    "sampler_noisevol_hq", // CHAN_NOISEVOL_HQ
    "sampler_image",       // CHAN_IMAGE_PREV
    "sampler_audio",       // CHAN_AUDIO
    "sampler_rand00",      // CHAN_RANDOM_TEX
};
static const wchar_t* kChannelNames[] = {
    L"Noise LQ",      L"Noise MQ",       L"Noise HQ",
    L"Buffer A / Self", L"Noise Vol LQ",  L"Noise Vol HQ",
    L"Image (prev frame)", L"Audio (FFT + Wave)",
    L"Random Texture",
};
static const int kChannelTexDim[] = { 256, 256, 256, 0, 32, 32, 0, 0, 0 }; // 0 = use texsize

// ─── Open / Close (Engine methods) ──────────────────────────────────────

void Engine::OpenShaderImportWindow() {
    if (!m_shaderImportWindow)
        m_shaderImportWindow = std::make_unique<ShaderImportWindow>(this);
    m_shaderImportWindow->Open();
}

void Engine::CloseShaderImportWindow() {
    if (m_shaderImportWindow)
        m_shaderImportWindow->Close();
}

// ─── Welcome Window (no-presets prompt) ──────────────────────────────────

void Engine::OpenWelcomeWindow() {
    if (!m_welcomeWindow)
        m_welcomeWindow = std::make_unique<WelcomeWindow>(this);
    m_welcomeWindow->Open();
}

void Engine::CloseWelcomeWindow() {
    if (m_welcomeWindow)
        m_welcomeWindow->Close();
}

WelcomeWindow::WelcomeWindow(Engine* pEngine) : ToolWindow(pEngine, 340, 260) {
}

void WelcomeWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    auto L = BuildBaseControls();
    HFONT hFont = GetFont();

    RECT rc;
    GetClientRect(hw, &rc);
    int pad = 16;
    int x = pad;
    int w = rc.right - pad * 2;
    int lineH = L.lineH;
    int btnH = lineH + 10;
    int gap = 8;
    int y = L.y + gap;

    // Message label
    HWND hLabel = CreateWindowExW(0, L"STATIC",
        L"No presets found in the current directory.\r\n\r\n"
        L"Choose an option below to get started:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, lineH * 3, hw, NULL, NULL, NULL);
    TrackControl(hLabel);
    SendMessage(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += lineH * 3 + gap;

    // Open Settings button
    TrackControl(CreateBtn(hw, L"Open Settings...",
        IDC_MW_WELCOME_SETTINGS, x, y, w, btnH, hFont));
    y += btnH + gap;

    // Open Shader Import button
    TrackControl(CreateBtn(hw, L"Open Shader Import...",
        IDC_MW_WELCOME_SHADER_IMPORT, x, y, w, btnH, hFont));
    y += btnH + gap;

    // Open Preset Browser button
    TrackControl(CreateBtn(hw, L"Open Preset Browser...",
        IDC_MW_WELCOME_PRESETS, x, y, w, btnH, hFont));
}

LRESULT WelcomeWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    if (code != BN_CLICKED) return 0;
    Engine* p = m_pEngine;
    switch (id) {
    case IDC_MW_WELCOME_SETTINGS:
        p->OpenSettingsWindow();
        Close();
        return 0;
    case IDC_MW_WELCOME_SHADER_IMPORT:
        p->OpenShaderImportWindow();
        Close();
        return 0;
    case IDC_MW_WELCOME_PRESETS:
        p->OpenPresetsWindow();
        Close();
        return 0;
    }
    return 0;
}

// ─── ShaderEditorWindow ─────────────────────────────────────────────────

ShaderEditorWindow::ShaderEditorWindow(Engine* pEngine, ShaderImportWindow* pImport)
    : ToolWindow(pEngine, 700, 800), m_pImportWindow(pImport)
{
}

void ShaderEditorWindow::SetPassName(const std::wstring& name) {
    m_passName = name;
    m_title = L"Shader Editor - " + m_passName;
    if (m_hWnd)
        SetWindowTextW(m_hWnd, m_title.c_str());
}

// Convert narrow text to wide with proper \r\n line breaks for edit controls
static std::wstring NarrowToEditText(const std::string& s) {
    std::wstring w;
    w.reserve(s.size() + s.size() / 10);
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == LINEFEED_CONTROL_CHAR || c == '\n') {
            w += L"\r\n";
        } else if (c == '\r') {
            w += L"\r\n";
            if (i + 1 < s.size() && s[i + 1] == '\n') i++;  // skip \n after \r
        } else {
            w += (wchar_t)(unsigned char)c;
        }
    }
    return w;
}

void ShaderEditorWindow::SetGLSL(const std::string& glsl) {
    if (!m_hWnd) return;
    std::wstring w = NarrowToEditText(glsl);
    SetDlgItemTextW(m_hWnd, IDC_MW_SHEDITOR_GLSL_EDIT, w.c_str());
}

std::string ShaderEditorWindow::GetGLSL() {
    if (!m_hWnd) return "";
    HWND hEdit = GetDlgItem(m_hWnd, IDC_MW_SHEDITOR_GLSL_EDIT);
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return "";
    std::wstring w(len + 1, L'\0');
    GetWindowTextW(hEdit, w.data(), len + 1);
    std::string s;
    s.reserve(len);
    for (int i = 0; i < len; i++) {
        wchar_t ch = w[i];
        if (ch < 128) s += (char)ch; else s += '?';
    }
    return s;
}

void ShaderEditorWindow::SetHLSL(const std::string& hlsl) {
    if (!m_hWnd) return;
    std::wstring w = NarrowToEditText(hlsl);
    SetDlgItemTextW(m_hWnd, IDC_MW_SHEDITOR_HLSL_EDIT, w.c_str());
}

std::string ShaderEditorWindow::GetHLSL() {
    if (!m_hWnd) return "";
    HWND hEdit = GetDlgItem(m_hWnd, IDC_MW_SHEDITOR_HLSL_EDIT);
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return "";
    std::wstring w(len + 1, L'\0');
    GetWindowTextW(hEdit, w.data(), len + 1);
    // Convert to narrow with LINEFEED_CONTROL_CHAR
    return WideHlslToNarrow(w, len);
}

void ShaderEditorWindow::SetNotes(const std::string& notes) {
    if (!m_hWnd) return;
    std::wstring w = NarrowToEditText(notes);
    SetDlgItemTextW(m_hWnd, IDC_MW_SHEDITOR_NOTES_EDIT, w.c_str());
}

std::string ShaderEditorWindow::GetNotes() {
    if (!m_hWnd) return "";
    HWND hEdit = GetDlgItem(m_hWnd, IDC_MW_SHEDITOR_NOTES_EDIT);
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return "";
    std::wstring w(len + 1, L'\0');
    GetWindowTextW(hEdit, w.data(), len + 1);
    std::string s;
    s.reserve(len);
    for (int i = 0; i < len; i++) {
        wchar_t ch = w[i];
        if (ch == '\r') continue;  // strip \r, keep \n
        if (ch < 128) s += (char)ch; else s += '?';
    }
    return s;
}

void ShaderEditorWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    auto L = BuildBaseControls();
    HFONT hFont = GetFont();
    m_nTopY = L.y;

    RECT rc;
    GetClientRect(hw, &rc);
    int clientH = rc.bottom - rc.top;

    int x = L.x, rw = L.rw, lineH = L.lineH, gap = L.gap;
    int y = m_nTopY;
    int btnW = MulDiv(80, lineH, 26);
    int btnH = lineH + 4;

    // "Notes:" label
    TrackControl(CreateWindowExW(0, L"STATIC", L"Notes:", WS_CHILD | WS_VISIBLE,
        x, y, 60, lineH, hw, NULL, NULL, NULL));
    y += lineH + gap;

    // Notes multiline edit (small, ~3 lines)
    int notesH = lineH * 3 + 4;
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
        x, y, rw, notesH, hw, (HMENU)(INT_PTR)IDC_MW_SHEDITOR_NOTES_EDIT, NULL, NULL));
    SendDlgItemMessageW(hw, IDC_MW_SHEDITOR_NOTES_EDIT, EM_SETLIMITTEXT, 4096, 0);
    y += notesH + gap;

    // Available height for GLSL + HLSL edits
    int convertRowH = btnH + gap;  // Convert button row
    int fixedH = (lineH + gap) * 2 + convertRowH + gap;  // 2 label rows + convert row
    int editH = clientH - y - fixedH;
    if (editH < 120) editH = 120;
    int glslH = editH * 55 / 100;  // GLSL gets more space
    int hlslH = editH - glslH;

    // "GLSL:" label + Paste / Clear
    TrackControl(CreateWindowExW(0, L"STATIC", L"GLSL:", WS_CHILD | WS_VISIBLE,
        x, y, 60, lineH, hw, NULL, NULL, NULL));
    TrackControl(CreateBtn(hw, L"Paste", IDC_MW_SHEDITOR_PASTE,
        x + rw - btnW * 2 - 8, y - 2, btnW, btnH, hFont));
    TrackControl(CreateBtn(hw, L"Clear", IDC_MW_SHEDITOR_CLEAR,
        x + rw - btnW, y - 2, btnW, btnH, hFont));
    y += lineH + gap;

    // GLSL multiline edit
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
        WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
        x, y, rw, glslH, hw, (HMENU)(INT_PTR)IDC_MW_SHEDITOR_GLSL_EDIT, NULL, NULL));
    SendDlgItemMessageW(hw, IDC_MW_SHEDITOR_GLSL_EDIT, EM_SETLIMITTEXT, 0x100000, 0);
    y += glslH + gap;

    // [Convert & Apply] button (centered, prominent)
    {
        int cvtW = MulDiv(160, lineH, 26);
        int cvtX = x + (rw - cvtW) / 2;
        TrackControl(CreateBtn(hw, L"Convert && Apply", IDC_MW_SHEDITOR_CONVERT,
            cvtX, y, cvtW, btnH, hFont));
    }
    y += btnH + gap;

    // "HLSL:" label + Copy
    TrackControl(CreateWindowExW(0, L"STATIC", L"HLSL:", WS_CHILD | WS_VISIBLE,
        x, y, 60, lineH, hw, NULL, NULL, NULL));
    TrackControl(CreateBtn(hw, L"Copy", IDC_MW_SHEDITOR_COPY,
        x + rw - btnW, y - 2, btnW, btnH, hFont));
    y += lineH + gap;

    // HLSL multiline edit (editable — for direct HLSL authoring)
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
        WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
        x, y, rw, hlslH, hw, (HMENU)(INT_PTR)IDC_MW_SHEDITOR_HLSL_EDIT, NULL, NULL));
    SendDlgItemMessageW(hw, IDC_MW_SHEDITOR_HLSL_EDIT, EM_SETLIMITTEXT, 0x100000, 0);

    // Monospace font on code edits (not notes)
    HFONT hMono = CreateFontW(-MulDiv(10, GetDpiForWindow(hw), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (hMono) {
        SendDlgItemMessageW(hw, IDC_MW_SHEDITOR_GLSL_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);
        SendDlgItemMessageW(hw, IDC_MW_SHEDITOR_HLSL_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);
    }

    // Load pending data (set by OpenEditor before window creation)
    SetGLSL(m_pendingGlsl);
    SetHLSL(m_pendingHlsl);
    SetNotes(m_pendingNotes);
    SetPassName(m_pendingPassName);
}

void ShaderEditorWindow::SetPendingData(const ShaderPass& pass) {
    m_pendingGlsl     = pass.glslSource;
    m_pendingHlsl     = pass.hlslOutput;
    m_pendingNotes    = pass.notes;
    m_pendingPassName = pass.name;
}

void ShaderEditorWindow::OnResize() {
    RebuildFonts();
}

void ShaderEditorWindow::DoDestroy() {
    // Sync editor content back to pass data before controls are destroyed.
    // Read directly here since m_hWnd is still valid during WM_DESTROY
    // but IsOpen() may return false.
    if (m_pImportWindow && m_hWnd) {
        m_pImportWindow->OnEditorClosing(GetGLSL(), GetHLSL(), GetNotes());
    }
}

LRESULT ShaderEditorWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    if (code == BN_CLICKED) {
        switch (id) {
        case IDC_MW_SHEDITOR_PASTE:
            if (OpenClipboard(hWnd)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* pText = (wchar_t*)GlobalLock(hData);
                    if (pText) {
                        SetDlgItemTextW(hWnd, IDC_MW_SHEDITOR_GLSL_EDIT, pText);
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            return 0;
        case IDC_MW_SHEDITOR_CLEAR:
            SetDlgItemTextW(hWnd, IDC_MW_SHEDITOR_GLSL_EDIT, L"");
            SetDlgItemTextW(hWnd, IDC_MW_SHEDITOR_HLSL_EDIT, L"");
            return 0;
        case IDC_MW_SHEDITOR_CONVERT:
            // Convert all passes and apply via the import window
            if (m_pImportWindow)
                m_pImportWindow->ConvertAndApply();
            return 0;
        case IDC_MW_SHEDITOR_COPY: {
            HWND hEdit = GetDlgItem(hWnd, IDC_MW_SHEDITOR_HLSL_EDIT);
            int len = GetWindowTextLengthW(hEdit);
            if (len > 0) {
                std::wstring text(len + 1, L'\0');
                GetWindowTextW(hEdit, text.data(), len + 1);
                if (OpenClipboard(hWnd)) {
                    EmptyClipboard();
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
                    if (hMem) {
                        memcpy(GlobalLock(hMem), text.c_str(), (len + 1) * sizeof(wchar_t));
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                    CloseClipboard();
                }
            }
            return 0;
        }
        }
    }
    return -1;
}

// ─── ShaderImportWindow — Constructor / Destructor ──────────────────────

ShaderImportWindow::ShaderImportWindow(Engine* pEngine)
    : ToolWindow(pEngine, 400, 500)
{
    m_passes.push_back({L"Image", "", ""});
}

ShaderImportWindow::~ShaderImportWindow() {
    m_editorWindow.reset();
}

// ─── Build Controls ─────────────────────────────────────────────────────

void ShaderImportWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    auto L = BuildBaseControls();
    HFONT hFont = GetFont();
    m_nTopY = L.y;

    RECT rc;
    GetClientRect(hw, &rc);
    int clientH = rc.bottom - rc.top;

    int x = L.x, rw = L.rw, lineH = L.lineH, gap = L.gap;
    int y = m_nTopY;
    int btnW = MulDiv(80, lineH, 26);
    int btnH = lineH + 4;
    int smBtnW = lineH + 4;  // square buttons for +/-

    // "Passes:" label + [+] [-] buttons
    TrackControl(CreateWindowExW(0, L"STATIC", L"Passes:", WS_CHILD | WS_VISIBLE,
        x, y, 80, lineH, hw, NULL, NULL, NULL));
    TrackControl(CreateBtn(hw, L"+", IDC_MW_SHIMPORT_ADD_PASS,
        x + rw - smBtnW * 2 - 4, y - 2, smBtnW, btnH, hFont));
    TrackControl(CreateBtn(hw, L"-", IDC_MW_SHIMPORT_DEL_PASS,
        x + rw - smBtnW, y - 2, smBtnW, btnH, hFont));
    y += lineH + gap;

    // Listbox — fills middle area
    int chRowH = lineH + gap;  // one row of channel combos
    int fixedBelow = chRowH * 2          // 2 rows of channel combos (2x2)
                   + (btnH + gap)        // Convert&Apply/Apply/Save row
                   + (lineH + gap)       // Errors label
                   + (btnH + gap)        // Save/Load Import row
                   + gap;
    int listH = clientH - y - fixedBelow - 80;  // leave room for error edit
    if (listH < 60) listH = 60;
    int errH = clientH - y - listH - fixedBelow;
    if (errH < 40) errH = 40;

    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
        LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        x, y, rw, listH, hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_PASS_LIST,
        GetModuleHandle(NULL), NULL);
    if (hList && hFont) SendMessage(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
    TrackControl(hList);
    y += listH + gap;

    // Channel input combos (2x2 grid): ch0/ch1 on row 1, ch2/ch3 on row 2
    {
        int lblW = MulDiv(30, lineH, 26);
        int comboW = (rw - lblW * 2 - 8) / 2;
        for (int row = 0; row < 2; row++) {
            for (int col = 0; col < 2; col++) {
                int ch = row * 2 + col;
                int cx = x + col * (lblW + comboW + 8);
                wchar_t lbl[8];
                swprintf(lbl, 8, L"ch%d:", ch);
                TrackControl(CreateWindowExW(0, L"STATIC", lbl, WS_CHILD | WS_VISIBLE,
                    cx, y + 2, lblW, lineH, hw, NULL, NULL, NULL));
                HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                    cx + lblW, y, comboW, lineH * 8, hw,
                    (HMENU)(INT_PTR)(IDC_MW_SHIMPORT_CH0 + ch), NULL, NULL);
                if (hCombo && hFont) SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
                for (int s = 0; s < CHAN_COUNT; s++)
                    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)kChannelNames[s]);
                TrackControl(hCombo);
            }
            y += lineH + gap;
        }
    }

    // [Convert & Apply] [Apply] [Save .milk3...] buttons
    int caW = MulDiv(120, lineH, 26);
    int saveW = MulDiv(110, lineH, 26);
    TrackControl(CreateBtn(hw, L"Convert && Apply", IDC_MW_SHIMPORT_CONVERT,
        x, y, caW, btnH, hFont));
    TrackControl(CreateBtn(hw, L"Apply", IDC_MW_SHIMPORT_APPLY,
        x + caW + 4, y, btnW, btnH, hFont));
    TrackControl(CreateBtn(hw, L"Save .milk3...", IDC_MW_SHIMPORT_SAVE,
        x + rw - saveW, y, saveW, btnH, hFont));
    y += btnH + gap;

    // "Errors / Status:" label
    TrackControl(CreateWindowExW(0, L"STATIC", L"Errors / Status:", WS_CHILD | WS_VISIBLE,
        x, y, 150, lineH, hw, NULL, NULL, NULL));
    y += lineH + gap;

    // Error multiline edit (read-only)
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
        WS_VSCROLL,
        x, y, rw, errH, hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_ERROR_EDIT, NULL, NULL));
    y += errH + gap;

    // [New] [Save Import...] [Load Import...] buttons
    {
        int newW = MulDiv(40, lineH, 26);
        int impW = MulDiv(100, lineH, 26);
        TrackControl(CreateBtn(hw, L"New", IDC_MW_SHIMPORT_NEW,
            x, y, newW, btnH, hFont));
        TrackControl(CreateBtn(hw, L"Save Import...", IDC_MW_SHIMPORT_SAVE_IMPORT,
            x + newW + 4, y, impW, btnH, hFont));
        TrackControl(CreateBtn(hw, L"Load Import...", IDC_MW_SHIMPORT_LOAD_IMPORT,
            x + newW + 4 + impW + 4, y, impW, btnH, hFont));
    }

    // Monospace font on error edit
    HFONT hMono = CreateFontW(-MulDiv(10, GetDpiForWindow(hw), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (hMono)
        SendDlgItemMessageW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);

    // Populate listbox and channel combos
    RebuildPassList();
    SyncChannelCombos();
}

void ShaderImportWindow::DoDestroy() {
    // Close editor window when import window closes
    if (m_editorWindow)
        m_editorWindow->Close();
}

// ─── Layout ─────────────────────────────────────────────────────────────

void ShaderImportWindow::OnResize() {
    RebuildFonts();
}

void ShaderImportWindow::LayoutControls() {
    // All layout is handled by DoBuildControls; OnResize calls RebuildFonts.
}

// ─── Pass Management Helpers ────────────────────────────────────────────

void ShaderImportWindow::RebuildPassList() {
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_SHIMPORT_PASS_LIST);
    if (!hList) return;
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    for (auto& p : m_passes)
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)p.name.c_str());
    if (m_nSelectedPass >= (int)m_passes.size())
        m_nSelectedPass = 0;
    SendMessage(hList, LB_SETCURSEL, m_nSelectedPass, 0);
}

void ShaderImportWindow::SyncChannelCombos() {
    if (m_nSelectedPass < 0 || m_nSelectedPass >= (int)m_passes.size()) return;
    for (int ch = 0; ch < 4; ch++) {
        HWND hCombo = GetDlgItem(m_hWnd, IDC_MW_SHIMPORT_CH0 + ch);
        if (hCombo)
            SendMessage(hCombo, CB_SETCURSEL, m_passes[m_nSelectedPass].channels[ch], 0);
    }
}

void ShaderImportWindow::SyncEditorToPass() {
    if (!m_editorWindow || !m_editorWindow->IsOpen() ||
        m_nSelectedPass < 0 || m_nSelectedPass >= (int)m_passes.size())
        return;
    m_passes[m_nSelectedPass].glslSource = m_editorWindow->GetGLSL();
    m_passes[m_nSelectedPass].hlslOutput = m_editorWindow->GetHLSL();
    m_passes[m_nSelectedPass].notes = m_editorWindow->GetNotes();
}

void ShaderImportWindow::OnEditorClosing(const std::string& glsl, const std::string& hlsl, const std::string& notes) {
    if (m_nSelectedPass >= 0 && m_nSelectedPass < (int)m_passes.size()) {
        m_passes[m_nSelectedPass].glslSource = glsl;
        m_passes[m_nSelectedPass].hlslOutput = hlsl;
        m_passes[m_nSelectedPass].notes = notes;
    }
}

void ShaderImportWindow::SyncPassToEditor() {
    SyncChannelCombos();
    if (!m_editorWindow || m_nSelectedPass < 0 || m_nSelectedPass >= (int)m_passes.size())
        return;
    m_editorWindow->SetGLSL(m_passes[m_nSelectedPass].glslSource);
    m_editorWindow->SetHLSL(m_passes[m_nSelectedPass].hlslOutput);
    m_editorWindow->SetNotes(m_passes[m_nSelectedPass].notes);
    m_editorWindow->SetPassName(m_passes[m_nSelectedPass].name);
}

void ShaderImportWindow::OpenEditor() {
    if (!m_editorWindow)
        m_editorWindow = std::make_unique<ShaderEditorWindow>(m_pEngine, this);
    // Store data for the editor to load when its controls are ready
    if (m_nSelectedPass >= 0 && m_nSelectedPass < (int)m_passes.size())
        m_editorWindow->SetPendingData(m_passes[m_nSelectedPass]);
    bool wasOpen = m_editorWindow->IsOpen();
    m_editorWindow->Open();
    // If editor was already open, sync directly (controls exist)
    if (wasOpen)
        SyncPassToEditor();
}

// ─── Command Handler ────────────────────────────────────────────────────

LRESULT ShaderImportWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    // Listbox selection change
    if (id == IDC_MW_SHIMPORT_PASS_LIST && code == LBN_SELCHANGE) {
        HWND hList = GetDlgItem(hWnd, IDC_MW_SHIMPORT_PASS_LIST);
        int newSel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (newSel >= 0 && newSel < (int)m_passes.size() && newSel != m_nSelectedPass) {
            SyncEditorToPass();
            m_nSelectedPass = newSel;
            SyncPassToEditor();
        }
        return 0;
    }

    // Listbox double-click — open editor
    if (id == IDC_MW_SHIMPORT_PASS_LIST && code == LBN_DBLCLK) {
        OpenEditor();
        return 0;
    }

    // Channel combo selection change
    if (id >= IDC_MW_SHIMPORT_CH0 && id <= IDC_MW_SHIMPORT_CH3 && code == CBN_SELCHANGE) {
        int ch = id - IDC_MW_SHIMPORT_CH0;
        HWND hCombo = GetDlgItem(hWnd, id);
        int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < CHAN_COUNT &&
            m_nSelectedPass >= 0 && m_nSelectedPass < (int)m_passes.size())
            m_passes[m_nSelectedPass].channels[ch] = sel;
        return 0;
    }

    if (code == BN_CLICKED) {
        switch (id) {
        case IDC_MW_SHIMPORT_ADD_PASS: {
            // Add Buffer A if not present
            bool hasBufferA = false;
            for (auto& p : m_passes)
                if (p.name == L"Buffer A") { hasBufferA = true; break; }
            if (!hasBufferA) {
                ShaderPass bufA;
                bufA.name = L"Buffer A";
                // Buffer A ch0 defaults to CHAN_NOISE_LQ (256x256 noise texture).
                // Most Shadertoy shaders use noise for ch0 in Buffer A (e.g. terrain generation).
                // Users can change to CHAN_FEEDBACK if self-feedback is needed.
                m_passes.push_back(std::move(bufA));
                m_passes[0].channels[0] = CHAN_FEEDBACK;  // Image ch0 = Buffer A output
                SyncEditorToPass();  // save current pass before switching
                m_nSelectedPass = (int)m_passes.size() - 1;
                RebuildPassList();
                SyncChannelCombos();
                OpenEditor();
            }
            return 0;
        }
        case IDC_MW_SHIMPORT_DEL_PASS:
            if (m_nSelectedPass > 0 && m_nSelectedPass < (int)m_passes.size()) {
                m_passes.erase(m_passes.begin() + m_nSelectedPass);
                m_nSelectedPass = 0;
                // If only Image remains, reset ch0 to noise
                if (m_passes.size() == 1)
                    m_passes[0].channels[0] = CHAN_NOISE_LQ;
                RebuildPassList();
                SyncPassToEditor();
            }
            return 0;

        case IDC_MW_SHIMPORT_CONVERT:
            ConvertAndApply();
            return 0;

        case IDC_MW_SHIMPORT_APPLY:
            SyncEditorToPass();
            ApplyShader();
            return 0;

        case IDC_MW_SHIMPORT_SAVE:
            SyncEditorToPass();
            SaveAsPreset();
            return 0;

        case IDC_MW_SHIMPORT_SAVE_IMPORT:
            SyncEditorToPass();
            SaveImportProject();
            return 0;

        case IDC_MW_SHIMPORT_LOAD_IMPORT:
            LoadImportProject();
            return 0;

        case IDC_MW_SHIMPORT_NEW:
            // Reset to single empty Image pass
            SyncEditorToPass();
            m_passes.clear();
            m_passes.push_back({L"Image", "", ""});
            m_nSelectedPass = 0;
            RebuildPassList();
            SyncPassToEditor();
            SyncChannelCombos();
            SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_ERROR_EDIT, L"");
            return 0;
        }
    }
    return -1;
}

LRESULT ShaderImportWindow::DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == 1) {
        // Poll compilation result — render thread signals via m_nRecompileResult
        Engine* p = m_pEngine;
        int result = p->m_nRecompileResult.load();
        if (result < 2) {
            // Still compiling — keep polling (timer re-fires every 200ms)
            return 0;
        }
        KillTimer(hWnd, 1);
        p->m_nRecompileResult.store(0);  // reset to idle

        std::wstring errText;
        bool compOK = (p->m_shaders.comp.bytecodeBlob != NULL);
        bool hasBufA = (m_passes.size() > 1 && !m_passes[1].hlslOutput.empty());
        bool bufAOK = hasBufA ? (p->m_shaders.bufferA.bytecodeBlob != NULL) : true;

        if (compOK && bufAOK) {
            errText = L"Shader compiled successfully.";
            if (p->m_bHasBufferA)
                errText += L" (Buffer A + Image)";
        } else {
            errText = L"Compilation failed.\r\n";
            if (!compOK) errText += L"[Image/comp] ";
            if (!bufAOK) errText += L"[Buffer A] ";
            errText += L"\r\n";
            // Read error file (last compilation's errors)
            wchar_t errPath[MAX_PATH];
            swprintf(errPath, MAX_PATH, L"%lsdiag_comp_shader_error.txt", p->m_szBaseDir);
            FILE* f = _wfopen(errPath, L"r");
            if (f) {
                char buf[8192] = {};
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                buf[n] = '\0';
                if (n > 0) {
                    int wlen = MultiByteToWideChar(CP_ACP, 0, buf, -1, NULL, 0);
                    if (wlen > 0) {
                        std::wstring wbuf(wlen, L'\0');
                        MultiByteToWideChar(CP_ACP, 0, buf, -1, wbuf.data(), wlen);
                        errText += wbuf;
                    }
                }
            }
        }
        SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_ERROR_EDIT, errText.c_str());
        return 0;
    }
    return -1;
}

// ─── Helpers ────────────────────────────────────────────────────────────

int ShaderImportWindow::GetSelectedPass() {
    return m_nSelectedPass;  // 0=Image, 1=Buffer A
}

// Convert wide HLSL text to narrow with LINEFEED_CONTROL_CHAR line endings
static std::string WideHlslToNarrow(const std::wstring& hlslW, int len) {
    std::string hlsl;
    hlsl.reserve(len);
    for (int i = 0; i < len; i++) {
        wchar_t ch = hlslW[i];
        if (ch == L'\r') {
            if (i + 1 < len && hlslW[i + 1] == L'\n') i++;
            hlsl += (char)LINEFEED_CONTROL_CHAR;
        } else if (ch == L'\n') {
            hlsl += (char)LINEFEED_CONTROL_CHAR;
        } else if (ch < 128) {
            hlsl += (char)ch;
        } else {
            hlsl += '?';
        }
    }
    return hlsl;
}

// ─── Apply (live preview) ───────────────────────────────────────────────

void ShaderImportWindow::ApplyShader() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    // Get Image HLSL from m_passes[0]
    std::string imageHlsl = (m_passes.size() > 0) ? m_passes[0].hlslOutput : "";
    std::string bufAHlsl  = (m_passes.size() > 1) ? m_passes[1].hlslOutput : "";

    if (imageHlsl.empty()) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"No Image HLSL to apply. Convert first.");
        return;
    }

    // Apply Image (comp) shader
    strncpy_s(p->m_pState->m_szCompShadersText, MAX_SHADER_TEXT_LEN, imageHlsl.c_str(), _TRUNCATE);
    p->m_pState->m_nCompPSVersion = MD2_PS_5_0;
    p->m_pState->m_nMaxPSVersion = max(p->m_pState->m_nWarpPSVersion, (int)MD2_PS_5_0);

    // Apply Buffer A if present
    if (!bufAHlsl.empty()) {
        strncpy_s(p->m_pState->m_szBufferAShadersText, MAX_SHADER_TEXT_LEN, bufAHlsl.c_str(), _TRUNCATE);
        p->m_pState->m_nBufferAPSVersion = MD2_PS_5_0;
    }

    // Activate Shadertoy pipeline
    p->m_bShadertoyMode = true;
    p->m_nShadertoyStartFrame = p->GetFrame();

    p->m_nRecompileResult.store(1);
    p->EnqueueRenderCmd(RenderCmd::RecompileCompShader);

    // Status message
    {
        int storedLen = (int)strlen(p->m_pState->m_szCompShadersText);
        int bufALen = bufAHlsl.empty() ? 0 : (int)strlen(p->m_pState->m_szBufferAShadersText);
        wchar_t msg[256];
        bool truncated = ((int)imageHlsl.size() > storedLen + 1);
        if (truncated)
            swprintf(msg, 256, L"Compiling... (TRUNCATED: %d of %d chars stored)", storedLen, (int)imageHlsl.size());
        else if (bufALen > 0)
            swprintf(msg, 256, L"Compiling... (Image: %d chars, Buffer A: %d chars)", storedLen, bufALen);
        else
            swprintf(msg, 256, L"Compiling... (%d chars)", storedLen);
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, msg);
    }
    SetTimer(hw, 1, 200, NULL);
}

// ─── Convert & Apply (all passes) ───────────────────────────────────────

void ShaderImportWindow::ConvertAndApply() {
    SyncEditorToPass();

    // Convert each pass in order (Buffer A first if present, then Image)
    for (int i = (int)m_passes.size() - 1; i >= 0; i--) {
        if (m_passes[i].glslSource.empty()) continue;
        m_passes[i].hlslOutput.clear();
        ConvertGLSLtoHLSL(i);
        if (m_passes[i].hlslOutput.empty())
            return;  // Error already shown
    }

    // Update editor with current pass HLSL
    if (m_editorWindow && m_editorWindow->IsOpen())
        SyncPassToEditor();

    ApplyShader();
}

// ─── Save as .milk3 (JSON Shadertoy format) ─────────────────────────────

void ShaderImportWindow::SaveAsPreset() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    // Sync editor text into m_passes before saving
    SyncEditorToPass();

    // Must have Image HLSL
    if (m_passes.empty() || m_passes[0].hlslOutput.empty()) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"No Image HLSL to save. Convert first.");
        return;
    }

    // File save dialog — default to .milk3 for Shadertoy imports
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hw;
    ofn.lpstrFilter = L"Shadertoy Preset (*.milk3)\0*.milk3\0MilkDrop Preset (*.milk)\0*.milk\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = p->GetPresetDir();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"milk3";

    if (!GetSaveFileNameW(&ofn))
        return;

    // Determine which extension the user chose
    std::wstring ext = filePath;
    bool isMilk3 = (ext.size() >= 6 && _wcsicmp(ext.c_str() + ext.size() - 6, L".milk3") == 0);

    bool hasBufferA = (m_passes.size() > 1 && !m_passes[1].hlslOutput.empty());

    if (isMilk3) {
        // Save as .milk3 JSON
        JsonWriter w;
        w.BeginObject();
        w.Int(L"version", 1);
        w.Bool(L"shadertoy", true);

        // Extract a name from the filename
        std::wstring fname = std::filesystem::path(filePath).stem().wstring();
        w.String(L"name", fname.c_str());

        // Write notes (if any pass has notes)
        {
            bool hasNotes = false;
            for (auto& pass : m_passes)
                if (!pass.notes.empty()) { hasNotes = true; break; }
            if (hasNotes) {
                w.BeginObject(L"notes");
                if (!m_passes[0].notes.empty()) {
                    std::wstring wn(m_passes[0].notes.begin(), m_passes[0].notes.end());
                    w.String(L"image", wn.c_str());
                }
                if (hasBufferA && m_passes.size() > 1 && !m_passes[1].notes.empty()) {
                    std::wstring wn(m_passes[1].notes.begin(), m_passes[1].notes.end());
                    w.String(L"bufferA", wn.c_str());
                }
                w.EndObject();
            }
        }

        // Convert HLSL narrow string to wide, replacing LINEFEED_CONTROL_CHAR (0x01)
        // back to '\n' so that JSON stores standard newlines.
        auto hlslToWide = [](const std::string& s) -> std::wstring {
            std::wstring w;
            w.reserve(s.size());
            for (char c : s) {
                if (c == LINEFEED_CONTROL_CHAR)
                    w += L'\n';
                else if ((unsigned char)c < 128)
                    w += (wchar_t)c;
                else
                    w += L'?';
            }
            return w;
        };

        // Write shader text as wide strings (JsonWriter handles escaping)
        if (hasBufferA) {
            w.String(L"bufferA", hlslToWide(m_passes[1].hlslOutput).c_str());
        }
        {
            w.String(L"image", hlslToWide(m_passes[0].hlslOutput).c_str());
        }

        // Channel mappings
        w.BeginObject(L"channels");
        if (hasBufferA) {
            w.BeginObject(L"bufferA");
            w.String(L"iChannel0", L"self");
            w.EndObject();
        }
        w.BeginObject(L"image");
        w.String(L"iChannel0", hasBufferA ? L"bufferA" : L"self");
        w.EndObject();
        w.EndObject(); // channels
        w.EndObject(); // root

        if (w.SaveToFile(filePath)) {
            SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Preset saved as .milk3 successfully.");
        } else {
            SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Failed to save .milk3 preset.");
        }
    } else {
        // Legacy .milk save path
        auto tempState = std::make_unique<CState>();
        tempState->Default(0xFFFFFFFF);
        tempState->m_nCompPSVersion = MD2_PS_5_0;
        tempState->m_nWarpPSVersion = 0;
        tempState->m_nMaxPSVersion = MD2_PS_5_0;
        tempState->m_nMinPSVersion = 0;

        strncpy_s(tempState->m_szCompShadersText, MAX_SHADER_TEXT_LEN, m_passes[0].hlslOutput.c_str(), _TRUNCATE);

        if (hasBufferA) {
            strncpy_s(tempState->m_szBufferAShadersText, MAX_SHADER_TEXT_LEN, m_passes[1].hlslOutput.c_str(), _TRUNCATE);
            tempState->m_nBufferAPSVersion = MD2_PS_5_0;
        }

        if (tempState->Export(filePath)) {
            SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Preset saved successfully.");
        } else {
            SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Failed to save preset.");
        }
    }
}

// ─── GLSL→HLSL Conversion ──────────────────────────────────────────────
// Ported from Milkwave Remote ShaderHelper.cs

// Helper: whole-word replacement (identifier-boundary-aware)
static std::string WholeWordReplace(const std::string& input, const std::string& oldWord, const std::string& newWord) {
    std::string result;
    result.reserve(input.size() + 256);
    size_t pos = 0;
    size_t oldLen = oldWord.size();
    auto isIdent = [](char c) -> bool {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    while (pos < input.size()) {
        size_t found = input.find(oldWord, pos);
        if (found == std::string::npos) {
            result += input.substr(pos);
            break;
        }
        bool leftOk = (found == 0 || !isIdent(input[found - 1]));
        bool rightOk = (found + oldLen >= input.size() || !isIdent(input[found + oldLen]));
        result += input.substr(pos, found - pos);
        if (leftOk && rightOk)
            result += newWord;
        else
            result += oldWord;
        pos = found + oldLen;
    }
    return result;
}

// Helper: replace variable name patterns in various contexts
std::string ShaderImportWindow::ReplaceVarName(const std::string& oldName, const std::string& newName, const std::string& inp) {
    std::string res = inp;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(res, " " + oldName + " ", " " + newName + " ");
    replaceAll(res, oldName + ".", newName + ".");
    replaceAll(res, "(" + oldName + "-", "(" + newName + "-");
    replaceAll(res, "(" + oldName + ",", "(" + newName + ",");
    replaceAll(res, ", " + oldName + ",", ", " + newName + ",");
    replaceAll(res, " " + oldName + ",", " " + newName + ",");
    replaceAll(res, "," + oldName + ")", "," + newName + ")");
    replaceAll(res, ", " + oldName + ")", ", " + newName + ")");
    replaceAll(res, "(" + oldName + ")", "(" + newName + ")");
    replaceAll(res, oldName + "=", newName + "=");
    replaceAll(res, oldName + "*", newName + "*");
    replaceAll(res, "*" + oldName, "*" + newName);
    replaceAll(res, oldName + " =", newName + " =");
    replaceAll(res, oldName + "+", newName + "+");
    replaceAll(res, oldName + " +", newName + " +");
    replaceAll(res, oldName + ";", newName + ";");
    replaceAll(res, "float2 " + oldName + ",", "float2 " + newName + ", ");
    replaceAll(res, "float2 " + oldName + ";", "float2 " + newName + "; ");
    replaceAll(res, "float2 " + oldName + " ", "float2 " + newName + " ");
    replaceAll(res, "float2 " + oldName + ")", "float2 " + newName + ")");
    return res;
}

// Find closing bracket matching open/close at given nesting level
int ShaderImportWindow::FindClosingBracket(const std::string& input, char open, char close, int startLevel) {
    int level = startLevel;
    for (int i = 0; i < (int)input.size(); i++) {
        if (input[i] == open) level++;
        else if (input[i] == close) {
            level--;
            if (level == 0) return i;
        }
    }
    return -1;
}

// Fix matrix multiplication: *=mat2(...) → = mul(x, transpose(float2x2(...)))
std::string ShaderImportWindow::FixMatrixMultiplication(const std::string& inputLine) {
    std::string result = inputLine;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    try {
        replaceAll(result, "*= mat", "*=mat");
        replaceAll(result, "* mat", "*mat");
        replaceAll(result, " *mat", "*mat");

        // GLSL mat3(a,b,c) stores a,b,c as COLUMNS; HLSL float3x3(a,b,c) stores as ROWS.
        // Instead of transposing constructors, we swap mul() argument order:
        //   GLSL "v *= matN(...)" = "v = v * M" → HLSL "v = mul(floatNxN(...), v)"
        //   GLSL "x * matN(...)" = "x * M"     → HLSL "mul(floatNxN(...), x)"
        // Phase 1b handles named matrix variables with the same swap.

        std::string token = "*=mat";
        size_t index = result.find(token);
        if (index != std::string::npos) {
            // e.g. "uv *= mat2(cos(a), -sin(a), sin(a), cos(a));"
            char matSizeChar = result[index + token.size()];
            if (matSizeChar >= '2' && matSizeChar <= '4') {
                int matSize = matSizeChar - '0';
                std::string fac1 = result.substr(0, index);
                // Trim right
                while (!fac1.empty() && fac1.back() == ' ') fac1.pop_back();
                std::string indent_str;
                size_t fac1Start = fac1.find_first_not_of(" \t");
                if (fac1Start != std::string::npos)
                    indent_str = fac1.substr(0, fac1Start);
                fac1 = fac1.substr(fac1Start != std::string::npos ? fac1Start : 0);

                std::string rest = result.substr(index + token.size() + 2); // skip "matN("
                int closingIdx = FindClosingBracket(rest, '(', ')', 1);
                if (closingIdx > 0) {
                    std::string args = rest.substr(0, closingIdx);
                    // v *= matN(args) → v = mul(floatNxN(args), v)  [swapped for column-major]
                    result = indent_str + fac1 + " = mul(float"
                           + std::to_string(matSize) + "x" + std::to_string(matSize)
                           + "(" + args + "), " + fac1 + ");";
                }
            }
        } else {
            token = "*mat";
            index = result.find(token);
            if (index != std::string::npos) {
                char matSizeChar = result[index + token.size()];
                if (matSizeChar >= '2' && matSizeChar <= '4') {
                    int matSize = matSizeChar - '0';
                    std::string prefix = result.substr(0, index);
                    // Trim to get fac1 (last word before *mat)
                    while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
                    size_t lastSpace = prefix.rfind(' ');
                    std::string fac1 = (lastSpace != std::string::npos) ? prefix.substr(lastSpace + 1) : prefix;
                    std::string left = (lastSpace != std::string::npos) ? prefix.substr(0, lastSpace + 1) : "";

                    std::string rest = result.substr(index + token.size() + 2);
                    int closingIdx = FindClosingBracket(rest, '(', ')', 1);
                    if (closingIdx > 0) {
                        std::string args = rest.substr(0, closingIdx);
                        // x * matN(args) → mul(floatNxN(args), x)  [swapped for column-major]
                        result = left + "mul(float"
                               + std::to_string(matSize) + "x" + std::to_string(matSize)
                               + "(" + args + "), " + fac1 + ");";
                    }
                }
            }
        }

        // Simple type replacement for constructors and declarations
        replaceAll(result, "mat2(", "float2x2(");
        replaceAll(result, "mat3(", "float3x3(");
        replaceAll(result, "mat4(", "float4x4(");
        replaceAll(result, "mat2 ", "float2x2 ");
        replaceAll(result, "mat3 ", "float3x3 ");
        replaceAll(result, "mat4 ", "float4x4 ");
    } catch (...) {
        return inputLine;
    }
    return result;
}

// Fix float2/3/4 single-argument expansion: float3(1) → float3(1,1,1)
// Processes ALL occurrences per line, not just the first.
std::string ShaderImportWindow::FixFloatNumberOfArguments(const std::string& inputLine, const std::string& fullContext) {
    std::string result = inputLine;
    for (int numArgs = 2; numArgs <= 4; numArgs++) {
        std::string prefix = "float" + std::to_string(numArgs) + "(";
        size_t searchFrom = 0;
        while (searchFrom < result.size()) {
            size_t index = result.find(prefix, searchFrom);
            if (index == std::string::npos) break;

            std::string rest = result.substr(index + prefix.size());
            int closingIdx = FindClosingBracket(rest, '(', ')', 1);
            if (closingIdx <= 0) { searchFrom = index + prefix.size(); continue; }

            std::string argsLine = rest.substr(0, closingIdx);
            // Check if only one argument (no commas at top level)
            int topCommas = 0;
            { int depth = 0;
              for (char c : argsLine) {
                if (c == '(') depth++;
                else if (c == ')') depth--;
                else if (c == ',' && depth == 0) topCommas++;
              }
            }
            if (topCommas > 0) { searchFrom = index + prefix.size() + closingIdx; continue; }

            // Check if it's a number, function call, or known float variable
            bool shouldExpand = false;
            try {
                size_t pos;
                (void)std::stof(argsLine, &pos);
                if (pos == argsLine.size()) shouldExpand = true;
            } catch (...) {}
            if (!shouldExpand && argsLine.find('(') != std::string::npos && argsLine.find(')') != std::string::npos)
                shouldExpand = true;
            if (!shouldExpand && (fullContext.find("float " + argsLine + ",") != std::string::npos ||
                                  fullContext.find("float " + argsLine + ";") != std::string::npos))
                shouldExpand = true;

            if (shouldExpand) {
                std::string expanded = argsLine;
                for (int i = 1; i < numArgs; i++)
                    expanded += ", " + argsLine;
                result = result.substr(0, index + prefix.size())
                       + expanded
                       + result.substr(index + prefix.size() + closingIdx);
                searchFrom = index + prefix.size() + expanded.size();
            } else {
                searchFrom = index + prefix.size() + closingIdx;
            }
        }
    }
    return result;
}

// Fix two-argument atan(y,x) → atan2(y,x), checking each occurrence individually
std::string ShaderImportWindow::FixAtan(const std::string& line) {
    std::string result = line;
    size_t pos = 0;
    while ((pos = result.find("atan(", pos)) != std::string::npos) {
        std::string rest = result.substr(pos + 5);
        int closingIdx = FindClosingBracket(rest, '(', ')', 1);
        if (closingIdx > 0) {
            std::string args = rest.substr(0, closingIdx);
            if (args.find(',') != std::string::npos) {
                // Two arguments → atan2
                result.replace(pos, 5, "atan2(");
                pos += 6; // skip past "atan2("
                continue;
            }
        }
        pos += 5; // skip past "atan(" — single-arg, leave as is
    }
    return result;
}

// Basic code formatter: consistent indentation, blank line cleanup
std::string ShaderImportWindow::BasicFormatShaderCode(const std::string& code) {
    std::string src = code;
    // Normalize else placement
    {
        auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replaceAll(src, "}else", "}\nelse");
        replaceAll(src, "} else", "}\nelse");
    }

    int indentSize = 2;
    std::istringstream iss(src);
    std::string rawLine;
    std::ostringstream out;
    int indentLevel = 0;
    bool lastLineWasEmpty = false;

    std::vector<std::string> lines;
    while (std::getline(iss, rawLine)) {
        // Remove trailing \r
        if (!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();
        lines.push_back(rawLine);
    }

    for (size_t i = 0; i < lines.size(); i++) {
        std::string line = lines[i];
        // Trim
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            if (!lastLineWasEmpty) {
                out << "\n";
                lastLineWasEmpty = true;
            }
            continue;
        }
        line = line.substr(start);
        lastLineWasEmpty = false;

        // Decrease indent for closing brace
        if (line == "}")
            indentLevel = max(indentLevel - 1, 0);

        std::string indent_str(indentLevel * indentSize, ' ');

        // Handle inline comments
        size_t commentIdx = line.find("//");
        if (commentIdx != std::string::npos && commentIdx > 0) {
            std::string codePart = line.substr(0, commentIdx);
            while (!codePart.empty() && codePart.back() == ' ') codePart.pop_back();
            std::string commentPart = line.substr(commentIdx + 2);
            while (!commentPart.empty() && commentPart.front() == ' ') commentPart.erase(commentPart.begin());
            commentPart = "// " + commentPart;

            if (!codePart.empty())
                out << indent_str << codePart << "\n";
            out << indent_str << commentPart << "\n";
        } else {
            out << indent_str << line << "\n";
        }

        // Add blank line after lone '}'
        if (line == "}" && i + 1 < lines.size()) {
            std::string next = lines[i + 1];
            size_t ns = next.find_first_not_of(" \t");
            if (ns != std::string::npos)
                out << "\n";
        }

        // Increase indent after lines ending with '{'
        if (!line.empty() && line.back() == '{')
            indentLevel++;
    }

    // Trim leading/trailing blank lines
    std::string result = out.str();
    while (!result.empty() && result.front() == '\n') result.erase(result.begin());
    while (result.size() > 1 && result[result.size() - 1] == '\n' && result[result.size() - 2] == '\n')
        result.erase(result.size() - 1);

    return result;
}

// ─── Main Conversion ────────────────────────────────────────────────────

void ShaderImportWindow::ConvertGLSLtoHLSL(int passOverride) {
    HWND hw = m_hWnd;

    // Only sync editor when converting the currently selected pass
    if (passOverride < 0)
        SyncEditorToPass();

    // Get GLSL from specified pass (or selected pass)
    int passIdx = (passOverride >= 0) ? passOverride : m_nSelectedPass;
    if (passIdx < 0 || passIdx >= (int)m_passes.size()) return;
    const std::string& glslNarrow = m_passes[passIdx].glslSource;
    if (glslNarrow.empty()) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"No GLSL text to convert. Paste GLSL in the editor first.");
        return;
    }

    // Use narrow GLSL directly (already stored as narrow in m_passes)
    std::string inp = glslNarrow;

    // Strip // comment lines (browser copy-paste can line-wrap comments,
    // splitting words across lines and creating invalid identifiers like 'his' from 'this').
    // Also track commented-out preprocessor blocks: when we see "// #else" or "// #elif",
    // strip all subsequent lines (including non-comment code) until "// #endif".
    // This prevents dead code from leaking out when #else/#endif are commented out.
    {
        std::string cleaned;
        cleaned.reserve(inp.size());
        size_t i = 0;
        int deadBlockDepth = 0;
        while (i < inp.size()) {
            if (i + 1 < inp.size() && inp[i] == '/' && inp[i + 1] == '/') {
                // Extract comment text after //
                size_t cmtStart = i + 2;
                size_t lineEnd = i;
                while (lineEnd < inp.size() && inp[lineEnd] != '\n') lineEnd++;
                std::string cmtText;
                for (size_t j = cmtStart; j < lineEnd; j++) cmtText += inp[j];
                size_t fs = cmtText.find_first_not_of(" \t");
                std::string trimCmt = (fs != std::string::npos) ? cmtText.substr(fs) : "";
                // Check for commented-out preprocessor block boundaries
                if (trimCmt.substr(0, 5) == "#else" || trimCmt.substr(0, 5) == "#elif")
                    deadBlockDepth++;
                else if (trimCmt.substr(0, 6) == "#endif" && deadBlockDepth > 0)
                    deadBlockDepth--;
                // Skip the // comment line
                i = lineEnd;
            } else if (deadBlockDepth > 0) {
                // Inside a commented-out #else/#elif block — skip this line
                while (i < inp.size() && inp[i] != '\n') i++;
                if (i < inp.size()) i++; // skip \n
            } else {
                cleaned += inp[i];
                i++;
            }
        }
        inp = std::move(cleaned);
    }

    // Normalize line endings to \n (Edit Controls may use \r\n)
    {
        std::string norm;
        norm.reserve(inp.size());
        for (size_t i = 0; i < inp.size(); i++) {
            if (inp[i] == '\r' && i + 1 < inp.size() && inp[i + 1] == '\n') continue;
            if (inp[i] == '\r') { norm += '\n'; continue; }
            norm += inp[i];
        }
        inp = std::move(norm);
    }

    std::string errors;
    std::string result;

    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    try {
        // Phase 1: Global replacements

        // Integer/boolean vector types (must come BEFORE vec→float to avoid ivec2→ifloat2)
        replaceAll(inp, "ivec2", "int2");
        replaceAll(inp, "ivec3", "int3");
        replaceAll(inp, "ivec4", "int4");
        replaceAll(inp, "bvec2", "bool2");
        replaceAll(inp, "bvec3", "bool3");
        replaceAll(inp, "bvec4", "bool4");

        replaceAll(inp, "vec2", "float2");
        replaceAll(inp, "vec3", "float3");
        replaceAll(inp, "vec4", "float4");

        // Square matrix type aliases (matNxN → floatNxN, simple replacement)
        replaceAll(inp, "mat2x2", "float2x2");
        replaceAll(inp, "mat3x3", "float3x3");
        replaceAll(inp, "mat4x4", "float4x4");

        // Non-square matrix types: constructor vs declaration need different handling.
        // GLSL matAxB(args) fills COLUMNS (A cols × B rows); HLSL floatAxB(args) fills ROWS.
        // Constructor fix: matAxB(args) → transpose(floatAxB(args))
        //   floatAxB stores args as rows; transpose flips rows→columns, matching GLSL.
        // Declaration fix: matAxB varName → floatBxA varName (dimensions swapped)
        {
            auto isIdentNS = [](char c) -> bool {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_';
            };
            struct NonSqMat { const char* pat; const char* dims; const char* decl; };
            NonSqMat nsTypes[] = {
                {"mat2x3(", "2x3", "float3x2"},
                {"mat2x4(", "2x4", "float4x2"},
                {"mat3x2(", "3x2", "float2x3"},
                {"mat3x4(", "3x4", "float4x3"},
                {"mat4x2(", "4x2", "float2x4"},
                {"mat4x3(", "4x3", "float3x4"},
            };
            for (auto& ns : nsTypes) {
                std::string pat(ns.pat);
                // Wrap constructors: matAxB(args) → transpose(floatAxB(args))
                size_t pos = 0;
                while ((pos = inp.find(pat, pos)) != std::string::npos) {
                    if (pos > 0 && isIdentNS(inp[pos - 1])) { pos++; continue; }
                    size_t argsStart = pos + pat.size();
                    std::string rest = inp.substr(argsStart);
                    int closingIdx = FindClosingBracket(rest, '(', ')', 1);
                    if (closingIdx >= 0) {
                        size_t closePos = argsStart + closingIdx;
                        std::string args = inp.substr(argsStart, closingIdx);
                        std::string repl = "transpose(float" + std::string(ns.dims) + "(" + args + "))";
                        inp = inp.substr(0, pos) + repl + inp.substr(closePos + 1);
                        pos += repl.size();
                    } else {
                        pos += pat.size();
                    }
                }
                // Replace remaining type declarations: matAxB → floatBxA (dims swapped)
                std::string typeName = pat.substr(0, pat.size() - 1);
                replaceAll(inp, typeName, ns.decl);
            }
        }

        // Short-form square matrix types: mat2→float2x2, mat3→float3x3, mat4→float4x4
        // Done after matNxN and non-square forms so those patterns are already gone.
        // Word-boundary check prevents matching inside identifiers (e.g. "format4").
        {
            auto isIdentSM = [](char c) -> bool {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_';
            };
            struct ShortMat { const char* from; const char* to; int len; };
            ShortMat shortMats[] = {
                {"mat2", "float2x2", 4},
                {"mat3", "float3x3", 4},
                {"mat4", "float4x4", 4},
            };
            for (auto& sm : shortMats) {
                size_t pos = 0;
                while ((pos = inp.find(sm.from, pos)) != std::string::npos) {
                    if (pos > 0 && isIdentSM(inp[pos - 1])) { pos++; continue; }
                    size_t after = pos + sm.len;
                    if (after < inp.size() && isIdentSM(inp[after])) { pos++; continue; }
                    inp.replace(pos, sm.len, sm.to);
                    pos += strlen(sm.to);
                }
            }
        }

        replaceAll(inp, "fract (", "fract(");
        replaceAll(inp, "mod (", "mod(");
        replaceAll(inp, "mix (", "mix(");
        replaceAll(inp, "fract(", "frac(");
        replaceAll(inp, "mod(", "mod_conv(");
        replaceAll(inp, "mix(", "lerp(");
        inp = WholeWordReplace(inp, "time", "time_conv");
        replaceAll(inp, "refrac(", "refract("); // undo damage to refract

        // Rename variables that collide with MilkDrop macros.
        // #define mid _c3.y, bass _c3.x, treb _c3.z, vol _c4.x (audio)
        // #define rad _rad_ang.x, ang _rad_ang.y, uv _uv.xy (shader inputs)
        // These are common GLSL variable names that the preprocessor would expand
        // into constant buffer swizzles, breaking declarations/calls.
        inp = WholeWordReplace(inp, "mid", "_st_mid");
        inp = WholeWordReplace(inp, "bass", "_st_bass");
        inp = WholeWordReplace(inp, "treb", "_st_treb");
        inp = WholeWordReplace(inp, "vol", "_st_vol");
        inp = WholeWordReplace(inp, "rad", "_st_rad");
        inp = WholeWordReplace(inp, "ang", "_st_ang");
        replaceAll(inp, "iTimeDelta", "xTimeDelta"); // protect from iTime replace
        replaceAll(inp, "iTime", "time");
        // iResolution: Shadertoy vec3(width, height, pixelAspect=1.0)
        // texsize = float4(w, h, 1/w, 1/h), so texsize.z would be wrong.
        // Inline-expand so iResolution.z → float3(...).z → 1.0
        replaceAll(inp, "iResolution", "float3(texsize.x, texsize.y, 1.0)");
        replaceAll(inp, "iFrame", "frame");
        replaceAll(inp, "iMouse", "mouse");
        // iChannel samplers — use per-pass channel configuration
        {
            const int* ch = m_passes[passIdx].channels;
            for (int i = 0; i < 4; i++) {
                char from[16], to[32];
                sprintf_s(from, "iChannel%d", i);
                int src = (ch[i] >= 0 && ch[i] < CHAN_COUNT) ? ch[i] : 0;
                strcpy_s(to, kChannelSamplers[src]);
                replaceAll(inp, from, to);
            }
        }
        replaceAll(inp, "texture(", "tex2D(");
        replaceAll(inp, "textureLod(", "tex2Dlod_conv(");
        replaceAll(inp, "texelFetch(", "texelFetch_conv(");
        // Normalize whitespace after texelFetch_conv( so specialization matches
        while (inp.find("texelFetch_conv( ") != std::string::npos)
            replaceAll(inp, "texelFetch_conv( ", "texelFetch_conv(");
        // Specialize texelFetch for noise textures (256x256) vs screen-sized feedback
        replaceAll(inp, "texelFetch_conv(sampler_noise_lq,", "texelFetch_noise(sampler_noise_lq,");
        replaceAll(inp, "texelFetch_conv(sampler_noise_mq,", "texelFetch_noise(sampler_noise_mq,");
        replaceAll(inp, "texelFetch_conv(sampler_noise_hq,", "texelFetch_noise(sampler_noise_hq,");
        // GLSL bit-cast / math intrinsics → HLSL equivalents
        replaceAll(inp, "uintBitsToFloat(", "asfloat(");
        replaceAll(inp, "intBitsToFloat(", "asfloat(");
        replaceAll(inp, "floatBitsToUint(", "asuint(");
        replaceAll(inp, "floatBitsToInt(", "asint(");
        replaceAll(inp, "dFdx(", "ddx(");
        replaceAll(inp, "dFdy(", "ddy(");
        replaceAll(inp, "inversesqrt(", "rsqrt(");
        replaceAll(inp, "highp ", "");
        replaceAll(inp, "lowp ", "");
        replaceAll(inp, "mediump ", "");
        replaceAll(inp, "void mainImage(", "mainImage(");
        replaceAll(inp, "atan (", "atan(");

        // Phase 1b: Collect matrix variable names and fix matrix*vector multiplication
        // HLSL requires mul() for matrix-vector multiply; the * operator causes X3020 type mismatch.
        // Square matrices use mul-swap (no transpose on constructor, rows≡GLSL columns, M[i] works).
        // Non-square matrices use standard mul (constructor wrapped with transpose() in Phase 1).
        {
            auto isIdent = [](char c) -> bool {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_';
            };
            struct MatVar { std::string name; bool isSquare; };
            std::vector<MatVar> matVars;
            struct MatTypeInfo { const char* type; bool isSquare; };
            MatTypeInfo matTypes[] = {
                {"float2x2", true}, {"float3x3", true}, {"float4x4", true},
                {"float2x3", false}, {"float3x2", false}, {"float2x4", false},
                {"float4x2", false}, {"float3x4", false}, {"float4x3", false},
            };
            struct MatFunc { std::string name; bool isSquare; };
            std::vector<MatFunc> matFuncs;
            for (auto& mt : matTypes) {
                int mtLen = (int)strlen(mt.type);
                size_t pos = 0;
                while ((pos = inp.find(mt.type, pos)) != std::string::npos) {
                    if (pos > 0 && isIdent(inp[pos - 1])) { pos += mtLen; continue; }
                    size_t after = pos + mtLen;
                    if (after < inp.size() && isIdent(inp[after])) { pos += mtLen; continue; }
                    while (after < inp.size() && (inp[after] == ' ' || inp[after] == '\t')) after++;
                    size_t nameStart = after;
                    while (after < inp.size() && isIdent(inp[after])) after++;
                    if (after > nameStart) {
                        std::string name = inp.substr(nameStart, after - nameStart);
                        size_t check = after;
                        while (check < inp.size() && (inp[check] == ' ' || inp[check] == '\t')) check++;
                        if (check >= inp.size() || inp[check] != '(')
                            matVars.push_back({name, mt.isSquare});
                        else
                            matFuncs.push_back({name, mt.isSquare});
                    }
                    pos = after;
                }
            }
            // Convert matVar*expr and expr*matVar to mul() calls.
            // Square (mul-swap): matVar*expr → mul(expr, matVar), expr*matVar → mul(matVar, expr)
            //   Because float3x3(a,b,c) stores a,b,c as ROWS but GLSL treats them as COLUMNS.
            //   Swapping mul() args compensates. Also preserves M[i] indexing (row i = GLSL col i).
            // Non-square (standard): matVar*expr → mul(matVar, expr), expr*matVar → mul(expr, matVar)
            //   Because constructor was wrapped with transpose(), matrix is correct. Standard order.
            for (const auto& mv : matVars) {
                int mvLen = (int)mv.name.size();
                size_t pos = 0;
                while ((pos = inp.find(mv.name, pos)) != std::string::npos) {
                    if (pos > 0 && isIdent(inp[pos - 1])) { pos++; continue; }
                    size_t afterMv = pos + mvLen;
                    if (afterMv < inp.size() && isIdent(inp[afterMv])) { pos++; continue; }
                    size_t s = afterMv;
                    while (s < inp.size() && inp[s] == ' ') s++;
                    if (s < inp.size() && inp[s] == '*' && (s + 1 >= inp.size() || inp[s + 1] != '=')) {
                        size_t afterStar = s + 1;
                        while (afterStar < inp.size() && inp[afterStar] == ' ') afterStar++;
                        size_t opStart = afterStar;
                        while (afterStar < inp.size() && isIdent(inp[afterStar])) afterStar++;
                        if (afterStar > opStart) {
                            if (afterStar < inp.size() && inp[afterStar] == '(') {
                                int depth = 1;
                                size_t fp = afterStar + 1;
                                while (fp < inp.size() && depth > 0) {
                                    if (inp[fp] == '(') depth++;
                                    else if (inp[fp] == ')') depth--;
                                    fp++;
                                }
                                afterStar = fp;
                            }
                            // Capture trailing swizzle (.xyz, .yzw, etc.) as part of operand
                            if (afterStar < inp.size() && inp[afterStar] == '.') {
                                size_t dotPos = afterStar;
                                size_t swizEnd = dotPos + 1;
                                while (swizEnd < inp.size() && swizEnd - dotPos - 1 < 4 &&
                                       (inp[swizEnd] >= 'a' && inp[swizEnd] <= 'z'))
                                    swizEnd++;
                                if (swizEnd > dotPos + 1 && (swizEnd >= inp.size() || !isIdent(inp[swizEnd])))
                                    afterStar = swizEnd;
                            }
                            std::string operand = inp.substr(opStart, afterStar - opStart);
                            std::string repl;
                            if (mv.isSquare)
                                repl = "mul(" + operand + ", " + mv.name + ")";  // SWAPPED
                            else
                                repl = "mul(" + mv.name + ", " + operand + ")";  // STANDARD
                            inp = inp.substr(0, pos) + repl + inp.substr(afterStar);
                            pos += repl.size();
                            continue;
                        }
                    }
                    if (pos > 0) {
                        size_t bk = pos;
                        while (bk > 0 && inp[bk - 1] == ' ') bk--;
                        if (bk > 0 && inp[bk - 1] == '*') {
                            size_t starIdx = bk - 1;
                            if (starIdx == 0 || inp[starIdx - 1] != '=') {
                                size_t opEnd = starIdx;
                                while (opEnd > 0 && inp[opEnd - 1] == ' ') opEnd--;
                                size_t opStart = opEnd;
                                while (opStart > 0 && isIdent(inp[opStart - 1])) opStart--;
                                if (opEnd > opStart) {
                                    std::string operand = inp.substr(opStart, opEnd - opStart);
                                    std::string repl;
                                    if (mv.isSquare)
                                        repl = "mul(" + mv.name + ", " + operand + ")";  // SWAPPED
                                    else
                                        repl = "mul(" + operand + ", " + mv.name + ")";  // STANDARD
                                    inp = inp.substr(0, opStart) + repl + inp.substr(afterMv);
                                    pos = opStart + repl.size();
                                    continue;
                                }
                            }
                        }
                    }
                    pos++;
                }
            }

            // Convert expr *= matFunc(args) and expr * matFunc(args) to mul() calls.
            // HLSL * operator doesn't work between vectors and matrices — must use mul().
            // For matrix-returning functions like "rot()", the Phase 3 FixMatrixMultiplication
            // only handles inline "mat2()" constructors. This handles function calls.
            for (const auto& mf : matFuncs) {
                size_t pos = 0;
                while ((pos = inp.find(mf.name, pos)) != std::string::npos) {
                    if (pos > 0 && isIdent(inp[pos - 1])) { pos++; continue; }
                    size_t afterName = pos + mf.name.size();
                    if (afterName >= inp.size() || inp[afterName] != '(') { pos++; continue; }

                    // Find closing paren of function call
                    std::string rest = inp.substr(afterName + 1);
                    int closingIdx = FindClosingBracket(rest, '(', ')', 1);
                    if (closingIdx < 0) { pos++; continue; }
                    size_t callEnd = afterName + 1 + closingIdx + 1; // past closing ')'
                    std::string funcCall = inp.substr(pos, callEnd - pos);

                    // Check for "expr *= matFunc(args)" pattern
                    size_t bk = pos;
                    while (bk > 0 && inp[bk - 1] == ' ') bk--;
                    if (bk >= 2 && inp[bk - 1] == '=' && inp[bk - 2] == '*') {
                        size_t eqPos = bk - 2; // position of '*'
                        size_t lEnd = eqPos;
                        while (lEnd > 0 && inp[lEnd - 1] == ' ') lEnd--;
                        // Walk back to find lvalue (may include dots/swizzles like "p.xz")
                        size_t lStart = lEnd;
                        while (lStart > 0 && (isIdent(inp[lStart - 1]) || inp[lStart - 1] == '.')) lStart--;
                        if (lEnd > lStart) {
                            std::string lvalue = inp.substr(lStart, lEnd - lStart);
                            std::string repl;
                            if (mf.isSquare)
                                repl = lvalue + " = mul(" + funcCall + ", " + lvalue + ")";  // SWAPPED
                            else
                                repl = lvalue + " = mul(" + lvalue + ", " + funcCall + ")";  // STANDARD
                            inp = inp.substr(0, lStart) + repl + inp.substr(callEnd);
                            pos = lStart + repl.size();
                            continue;
                        }
                    }

                    // Check for "expr * matFunc(args)" pattern (not *=)
                    if (bk > 0 && inp[bk - 1] == '*' && (bk < 2 || inp[bk - 2] != '=')) {
                        size_t starPos = bk - 1;
                        size_t opEnd = starPos;
                        while (opEnd > 0 && inp[opEnd - 1] == ' ') opEnd--;
                        size_t opStart = opEnd;
                        while (opStart > 0 && (isIdent(inp[opStart - 1]) || inp[opStart - 1] == '.')) opStart--;
                        if (opEnd > opStart) {
                            std::string operand = inp.substr(opStart, opEnd - opStart);
                            std::string repl;
                            if (mf.isSquare)
                                repl = "mul(" + funcCall + ", " + operand + ")";  // SWAPPED
                            else
                                repl = "mul(" + operand + ", " + funcCall + ")";  // STANDARD
                            inp = inp.substr(0, opStart) + repl + inp.substr(callEnd);
                            pos = opStart + repl.size();
                            continue;
                        }
                    }
                    pos = callEnd;
                }
            }
        }

        // Phase 2: Extract mainImage
        size_t indexMainImage = inp.find("mainImage(");

        std::string inpHeader;
        std::string inpMain;
        std::string retVarName = "fragColor";

        if (indexMainImage == std::string::npos) {
            // No mainImage — wrap everything
            inpMain = inp + "\n}";
        } else {
            std::string afterMain = inp.substr(indexMainImage);
            int closingIdx = FindClosingBracket(afterMain, '{', '}', 0);

            inpHeader = inp.substr(0, indexMainImage);

            if (closingIdx > 0) {
                inpMain = inp.substr(indexMainImage, closingIdx + 1);
            } else {
                inpMain = inp.substr(indexMainImage);
            }

            // Footer (anything after mainImage closing brace)
            std::string inpFooter;
            size_t footerStart = indexMainImage + (closingIdx > 0 ? closingIdx + 1 : afterMain.size());
            if (footerStart < inp.size())
                inpFooter = inp.substr(footerStart);
            inpHeader += inpFooter;

            // Strip comments and blank lines from header
            {
                std::istringstream hss(inpHeader);
                std::string line;
                std::ostringstream clean;
                bool inComment = false;
                std::string prevLine;
                while (std::getline(hss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    std::string trimmed = line;
                    size_t s = trimmed.find_first_not_of(" \t");
                    if (s != std::string::npos) trimmed = trimmed.substr(s);
                    else trimmed = "";

                    if (trimmed.substr(0, 2) == "//") continue;
                    // Strip user-defined mul() macro — conflicts with HLSL's built-in mul()
                    if (trimmed.substr(0, 12) == "#define mul(" || trimmed.substr(0, 12) == "#define mul ") continue;
                    if (trimmed.substr(0, 2) == "/*") {
                        inComment = (trimmed.find("*/") == std::string::npos);
                        continue;
                    }
                    if (trimmed.find("*/") != std::string::npos) {
                        inComment = false;
                        continue;
                    }
                    if (inComment) continue;
                    if (!trimmed.empty() || !prevLine.empty()) {
                        // GLSL 'const' at line start → HLSL 'static const'
                        if (trimmed.substr(0, 6) == "const ")
                            trimmed = "static " + trimmed;
                        clean << trimmed << "\n";
                    }
                    prevLine = trimmed;
                }
                inpHeader = clean.str();
            }
        }

        // Rename 'uv' in header only — body uses MilkDrop's #define uv _uv.xy intentionally
        inpHeader = ReplaceVarName("uv", "uv_conv", inpHeader);

        // Add mod_conv helpers if needed
        if (inp.find("mod_conv(") != std::string::npos) {
            std::string modHelpers =
                "// CONV: adding helper functions\n"
                "float mod_conv(float x, float y) { return x - y * floor(x / y); }\n"
                "float2 mod_conv(float2 x, float2 y) { return x - y * floor(x / y); }\n"
                "float3 mod_conv(float3 x, float3 y) { return x - y * floor(x / y); }\n"
                "float4 mod_conv(float4 x, float4 y) { return x - y * floor(x / y); }\n\n";
            inpHeader = modHelpers + inpHeader;
        }

        // Add texelFetch helpers if needed (GLSL integer-coordinate texture fetch)
        if (inp.find("texelFetch_noise(") != std::string::npos) {
            std::string helper =
                "// CONV: texelFetch for noise textures (256x256)\n"
                "float4 texelFetch_noise(sampler2D s, int2 c, int l) {\n"
                "  return tex2Dlod(s, float4((float2(c) + 0.5) / 256.0, 0, l));\n"
                "}\n\n";
            inpHeader = helper + inpHeader;
        }
        if (inp.find("texelFetch_conv(") != std::string::npos) {
            std::string helper =
                "// CONV: texelFetch for screen-sized textures\n"
                "float4 texelFetch_conv(sampler2D s, int2 c, int l) {\n"
                "  return tex2Dlod(s, float4((float2(c) + 0.5) * texsize.zw, 0, l));\n"
                "}\n\n";
            inpHeader = helper + inpHeader;
        }

        // Add textureLod helper if needed (GLSL explicit-LOD texture fetch)
        if (inp.find("tex2Dlod_conv(") != std::string::npos) {
            std::string helper =
                "// CONV: textureLod → tex2Dlod wrapper\n"
                "float4 tex2Dlod_conv(sampler2D s, float2 uv_tl, float l) {\n"
                "  return tex2Dlod(s, float4(uv_tl, 0, l));\n"
                "}\n\n";
            inpHeader = helper + inpHeader;
        }

        // Add lessThan helper if needed
        if (inp.find("lessThan") != std::string::npos || inp.find("lessthan") != std::string::npos) {
            std::string ltHelper =
                "float4 lessThan(float4 a, float4 b) { return float4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n\n";
            inpHeader = ltHelper + inpHeader;
        }

        // Add remaining #defines (uniforms/channels already replaced inline above)
        {
            std::string defines;
            if (inp.find(" tx") == std::string::npos) {
                defines = "#define tx (sin(time)*0.5+1)\n\n";
            }
            if (!defines.empty())
                inpHeader = defines + inpHeader;
        }

        // Build shader_body wrapper
        std::ostringstream sbHeader;
        sbHeader << "\nshader_body {\n";

        if (indexMainImage != std::string::npos) {
            // Extract and convert mainImage arguments
            size_t argsOpen = inpMain.find('(');
            if (argsOpen != std::string::npos) {
                std::string argsRest = inpMain.substr(argsOpen + 1);
                int argsClose = FindClosingBracket(argsRest, '(', ')', 1);
                if (argsClose > 0) {
                    std::string argsStr = argsRest.substr(0, argsClose);
                    // Split by comma
                    std::vector<std::string> args;
                    {
                        std::istringstream ass(argsStr);
                        std::string tok;
                        while (std::getline(ass, tok, ',')) {
                            // Trim
                            size_t s = tok.find_first_not_of(" \t");
                            size_t e = tok.find_last_not_of(" \t");
                            if (s != std::string::npos) tok = tok.substr(s, e - s + 1);
                            args.push_back(tok);
                        }
                    }
                    for (auto& arg : args) {
                        if (arg.find("out ") != std::string::npos) {
                            // Output parameter — extract var name
                            size_t f4pos = arg.find("float4 ");
                            if (f4pos != std::string::npos)
                                retVarName = arg.substr(f4pos + 7);
                            replaceAll(arg, "out ", "");
                            sbHeader << arg << " = 0;\n";
                        } else {
                            // Input parameter (typically fragCoord) — use pixel coordinates
                            // Use DX convention (y=0 at top) so that fragCoord ∝ texture v.
                            // This keeps the feedback loop self-consistent: what's written at
                            // texture v=F/H is read back at uv=F/H. The Image pass flips the
                            // quad UVs to display right-side-up (see RenderFrameShadertoy).
                            replaceAll(arg, "in ", "");
                            sbHeader << arg << " = uv * texsize.xy;\n";
                        }
                    }
                }
            }
        }

        // Shadertoy shaders compute their own UVs from fragCoord / iResolution.
        // The MilkDrop 'uv' macro is only used to initialize fragCoord above.
        // #undef it so the shader can declare its own 'float2 uv = ...' variable.
        sbHeader << "#undef uv\n#undef uv_orig\n";

        // Find the opening brace of mainImage body and replace everything before it
        size_t braceIdx = inpMain.find('{');
        if (braceIdx != std::string::npos)
            inpMain = sbHeader.str() + inpMain.substr(braceIdx + 1);
        else
            inpMain = sbHeader.str() + inpMain;

        inp = inpHeader + inpMain;

        // Phase 3: Per-line processing
        std::ostringstream sb;
        {
            std::istringstream lss(inp);
            std::string line;
            while (std::getline(lss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string currentLine = line;

                if (line.find("iDate") != std::string::npos) {
                    errors += "iDate unsupported\r\n";
                    sb << "// CONV: iDate unsupported\n";
                    currentLine = "// " + line;
                } else if (line.find("xTimeDelta") != std::string::npos) {
                    errors += "iTimeDelta unsupported\r\n";
                    sb << "// CONV: iTimeDelta unsupported\n";
                    replaceAll(currentLine, "xTimeDelta", "iTimeDelta");
                    currentLine = "// " + currentLine;
                }

                currentLine = FixMatrixMultiplication(currentLine);
                currentLine = FixFloatNumberOfArguments(currentLine, inp);
                currentLine = FixAtan(currentLine);

                sb << currentLine << "\n";
            }
        }
        result = sb.str();

        // Remove trailing backslash line continuations
        {
            std::istringstream bss(result);
            std::string line;
            std::ostringstream clean;
            std::vector<std::string> allLines;
            while (std::getline(bss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                allLines.push_back(line);
            }
            for (size_t i = 0; i < allLines.size(); i++) {
                if (!allLines[i].empty() && allLines[i].back() == '\\') {
                    clean << allLines[i].substr(0, allLines[i].size() - 1);
                    if (i + 1 < allLines.size()) {
                        std::string next = allLines[i + 1];
                        size_t s = next.find_first_not_of(" \t");
                        if (s != std::string::npos) next = next.substr(s);
                        clean << next << "\n";
                        i++; // skip next line
                    }
                } else {
                    clean << allLines[i] << "\n";
                }
            }
            result = clean.str();
        }

        // Fix vector l-value indexing: vec[dynamic_idx] = expr → _setComp(vec, idx, expr)
        // HLSL doesn't support writing to float2/3/4 components via dynamic index (X3500/X3550).
        // Replace with helper function calls that use static .x/.y/.z/.w member access.
        {
            bool needsSetComp = false;
            std::istringstream lvss(result);
            std::string line;
            std::ostringstream lvout;
            while (std::getline(lvss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();

                // Find leading whitespace
                size_t ws = 0;
                while (ws < line.size() && (line[ws] == ' ' || line[ws] == '\t')) ws++;

                // Check for IDENTIFIER[
                size_t idStart = ws;
                size_t idEnd = idStart;
                while (idEnd < line.size() && (isalnum((unsigned char)line[idEnd]) || line[idEnd] == '_')) idEnd++;

                if (idEnd > idStart && idEnd < line.size() && line[idEnd] == '[') {
                    std::string varName = line.substr(idStart, idEnd - idStart);

                    // Check for IDENTIFIER index inside brackets
                    size_t idxStart = idEnd + 1;
                    size_t idxEnd = idxStart;
                    while (idxEnd < line.size() && (isalnum((unsigned char)line[idxEnd]) || line[idxEnd] == '_')) idxEnd++;

                    if (idxEnd > idxStart && idxEnd < line.size() && line[idxEnd] == ']') {
                        std::string idxName = line.substr(idxStart, idxEnd - idxStart);

                        // Skip if index is a numeric literal (HLSL handles constant indices fine)
                        bool isNumeric = !idxName.empty();
                        for (char c : idxName) {
                            if (!isdigit((unsigned char)c)) { isNumeric = false; break; }
                        }

                        if (!isNumeric) {
                            // Check for = (not ==, not compound +=/-=/*=//=)
                            // Scan past ] for first non-space: if it's '=', it's simple assignment
                            size_t eqPos = idxEnd + 1;
                            while (eqPos < line.size() && (line[eqPos] == ' ' || line[eqPos] == '\t')) eqPos++;

                            if (eqPos < line.size() && line[eqPos] == '=' &&
                                (eqPos + 1 >= line.size() || line[eqPos + 1] != '=')) {
                                // Find the expression until ;
                                size_t exprStart = eqPos + 1;
                                while (exprStart < line.size() && line[exprStart] == ' ') exprStart++;
                                size_t semi = line.find(';', exprStart);

                                if (semi != std::string::npos && semi > exprStart) {
                                    std::string expr = line.substr(exprStart, semi - exprStart);
                                    while (!expr.empty() && (expr.back() == ' ' || expr.back() == '\t')) expr.pop_back();

                                    std::string indent = line.substr(0, idStart);
                                    std::string rest = (semi + 1 < line.size()) ? line.substr(semi + 1) : "";
                                    line = indent + "_setComp(" + varName + ", " + idxName + ", " + expr + ");" + rest;
                                    needsSetComp = true;
                                }
                            }
                        }
                    }
                }

                lvout << line << "\n";
            }
            result = lvout.str();

            if (needsSetComp) {
                std::string helpers =
                    "// CONV: vector component write helpers (HLSL X3500 fix)\n"
                    "void _setComp(inout float2 v, int i, float val) { if (i == 0) v.x = val; else v.y = val; }\n"
                    "void _setComp(inout float3 v, int i, float val) { if (i == 0) v.x = val; else if (i == 1) v.y = val; else v.z = val; }\n"
                    "void _setComp(inout float4 v, int i, float val) { if (i == 0) v.x = val; else if (i == 1) v.y = val; else if (i == 2) v.z = val; else v.w = val; }\n\n";
                result = helpers + result;
                DebugLogA("CONV-FIX: Added _setComp helpers for vector l-value indexing\n");
            }
        }

        // Fix over-specified identical constructors: float3(expr, expr, expr) → ((float3)(expr))
        // This runs AFTER backslash continuation removal so multi-line constructs are joined.
        // GLSL allows vec3(vec3,vec3,vec3) (takes first N components); HLSL rejects excess.
        for (int numArgs = 2; numArgs <= 4; numArgs++) {
            std::string prefix = "float" + std::to_string(numArgs) + "(";
            std::string typeName = "float" + std::to_string(numArgs);
            size_t searchFrom = 0;
            while (searchFrom < result.size()) {
                size_t index = result.find(prefix, searchFrom);
                if (index == std::string::npos) break;
                std::string rest = result.substr(index + prefix.size());
                int closingIdx = FindClosingBracket(rest, '(', ')', 1);
                if (closingIdx <= 0) { searchFrom = index + prefix.size(); continue; }
                std::string argsLine = rest.substr(0, closingIdx);
                // Count top-level commas
                int topCommas = 0;
                { int depth = 0;
                  for (char c : argsLine) {
                    if (c == '(') depth++;
                    else if (c == ')') depth--;
                    else if (c == ',' && depth == 0) topCommas++;
                  }
                }
                if (topCommas == numArgs - 1) {
                    // Split by top-level commas
                    std::vector<std::string> args;
                    int depth2 = 0;
                    std::string current;
                    for (char c : argsLine) {
                        if (c == '(') depth2++;
                        else if (c == ')') depth2--;
                        if (c == ',' && depth2 == 0) { args.push_back(current); current.clear(); }
                        else current += c;
                    }
                    args.push_back(current);
                    for (auto& a : args) {
                        size_t s = a.find_first_not_of(" \t\n\r");
                        size_t e = a.find_last_not_of(" \t\n\r");
                        a = (s != std::string::npos) ? a.substr(s, e - s + 1) : "";
                    }
                    bool allIdentical = args.size() == (size_t)numArgs;
                    for (size_t ai = 1; ai < args.size() && allIdentical; ai++)
                        if (args[ai] != args[0]) allIdentical = false;
                    if (allIdentical && !args[0].empty() && args[0].find('(') != std::string::npos) {
                        std::string replacement = "((" + typeName + ")(" + args[0] + "))";
                        result = result.substr(0, index) + replacement
                               + result.substr(index + prefix.size() + closingIdx + 1);
                        searchFrom = index + replacement.size();
                        DebugLogA(("CONV-FIX: " + typeName + "(...) over-specified → cast\n").c_str());
                        continue;
                    }
                }
                searchFrom = index + prefix.size() + closingIdx;
            }
        }

        // Fix HLSL X3508: inout struct parameters must have all members written.
        // GLSL allows partial writes to inout struct params; HLSL rejects it.
        // fxc sees through self-assignments and struct copies — the only reliable
        // fix is to remove 'inout' entirely: transform
        //   void func(inout StructType param) { body }
        // into
        //   StructType func(StructType param) { body; return param; }
        // and call sites from func(arg) → arg = func(arg).
        {
            // Collect struct names so we only transform struct-typed inout params
            std::set<std::string> structNames;
            {
                size_t spos = 0;
                while (spos < result.size()) {
                    spos = result.find("struct ", spos);
                    if (spos == std::string::npos) break;
                    size_t lineStart = result.rfind('\n', spos);
                    if (lineStart == std::string::npos) lineStart = 0;
                    if (result.substr(lineStart, spos - lineStart).find("//") != std::string::npos) { spos += 7; continue; }
                    size_t ns = spos + 7;
                    while (ns < result.size() && result[ns] == ' ') ns++;
                    size_t ne = ns;
                    while (ne < result.size() && (isalnum((unsigned char)result[ne]) || result[ne] == '_')) ne++;
                    if (ne > ns) structNames.insert(result.substr(ns, ne - ns));
                    spos = ne;
                }
            }

            // Find "void funcName(inout StructType paramName)" and transform
            size_t searchFrom = 0;
            while (searchFrom < result.size()) {
                // Find "void " keyword
                size_t voidPos = result.find("void ", searchFrom);
                if (voidPos == std::string::npos) break;
                // Extract function name
                size_t fnStart = voidPos + 5;
                while (fnStart < result.size() && result[fnStart] == ' ') fnStart++;
                size_t fnEnd = fnStart;
                while (fnEnd < result.size() && (isalnum((unsigned char)result[fnEnd]) || result[fnEnd] == '_')) fnEnd++;
                if (fnEnd == fnStart) { searchFrom = fnEnd; continue; }
                std::string funcName = result.substr(fnStart, fnEnd - fnStart);
                // Find opening paren
                size_t parenOpen = fnEnd;
                while (parenOpen < result.size() && result[parenOpen] == ' ') parenOpen++;
                if (parenOpen >= result.size() || result[parenOpen] != '(') { searchFrom = fnEnd; continue; }
                // Check for "inout " after paren
                size_t afterParen = parenOpen + 1;
                while (afterParen < result.size() && (result[afterParen] == ' ' || result[afterParen] == '\n' || result[afterParen] == '\r')) afterParen++;
                if (result.substr(afterParen, 6) != "inout ") { searchFrom = fnEnd; continue; }
                // Extract type name
                size_t typeStart = afterParen + 6;
                while (typeStart < result.size() && result[typeStart] == ' ') typeStart++;
                size_t typeEnd = typeStart;
                while (typeEnd < result.size() && (isalnum((unsigned char)result[typeEnd]) || result[typeEnd] == '_')) typeEnd++;
                std::string typeName = result.substr(typeStart, typeEnd - typeStart);
                if (structNames.find(typeName) == structNames.end()) { searchFrom = fnEnd; continue; }
                // Extract param name
                size_t pnStart = typeEnd;
                while (pnStart < result.size() && result[pnStart] == ' ') pnStart++;
                size_t pnEnd = pnStart;
                while (pnEnd < result.size() && (isalnum((unsigned char)result[pnEnd]) || result[pnEnd] == '_')) pnEnd++;
                std::string paramName = result.substr(pnStart, pnEnd - pnStart);
                // Verify single parameter: closing paren after param name
                size_t afterParam = pnEnd;
                while (afterParam < result.size() && result[afterParam] == ' ') afterParam++;
                if (afterParam >= result.size() || result[afterParam] != ')') { searchFrom = fnEnd; continue; }
                // Find opening brace and verify it's a definition
                size_t braceOpen = result.find('{', afterParam);
                if (braceOpen == std::string::npos) { searchFrom = fnEnd; continue; }
                bool isFuncDef = true;
                for (size_t k = afterParam; k < braceOpen; k++) {
                    if (result[k] == ';') { isFuncDef = false; break; }
                }
                if (!isFuncDef) { searchFrom = fnEnd; continue; }
                // Find closing brace of function body
                std::string afterBrace = result.substr(braceOpen);
                int closeIdx = FindClosingBracket(afterBrace, '{', '}', 0);
                if (closeIdx <= 0) { searchFrom = fnEnd; continue; }
                size_t braceClose = braceOpen + closeIdx;

                // === Transform the function ===
                // 1. Replace "void" with typeName
                result.replace(voidPos, 4, typeName);
                int lenDiff = (int)typeName.size() - 4;
                fnStart += lenDiff; fnEnd += lenDiff; parenOpen += lenDiff;
                afterParen += lenDiff; braceOpen += lenDiff; braceClose += lenDiff;
                // 2. Remove "inout " from parameter
                size_t inoutPos = result.find("inout ", parenOpen);
                if (inoutPos != std::string::npos && inoutPos < braceOpen) {
                    result.erase(inoutPos, 6);
                    braceOpen -= 6; braceClose -= 6;
                }
                // 3. Insert "return paramName;" before closing brace
                std::string retStmt = "    return " + paramName + ";\n  ";
                result.insert(braceClose, retStmt);
                size_t funcDefEnd = braceClose + retStmt.size() + 1;

                // 4. Transform call sites: funcName(arg) → arg = funcName(arg)
                size_t callSearch = 0;
                while (callSearch < result.size()) {
                    size_t callPos = result.find(funcName + "(", callSearch);
                    if (callPos == std::string::npos) break;
                    // Skip if part of the function definition
                    if (callPos >= voidPos && callPos < funcDefEnd) {
                        callSearch = callPos + funcName.size() + 1;
                        continue;
                    }
                    // Skip if preceded by alphanumeric (part of longer identifier)
                    if (callPos > 0 && (isalnum((unsigned char)result[callPos - 1]) || result[callPos - 1] == '_')) {
                        callSearch = callPos + funcName.size();
                        continue;
                    }
                    // Extract argument between parens
                    size_t argStart = callPos + funcName.size() + 1;
                    std::string argRest = result.substr(argStart);
                    int argCloseIdx = FindClosingBracket("(" + argRest, '(', ')', 1);
                    if (argCloseIdx <= 0) { callSearch = argStart; continue; }
                    std::string arg = result.substr(argStart, argCloseIdx - 1);
                    // Trim whitespace from argument
                    size_t as = arg.find_first_not_of(" \t\n\r");
                    size_t ae = arg.find_last_not_of(" \t\n\r");
                    if (as != std::string::npos) arg = arg.substr(as, ae - as + 1);
                    // Replace: funcName(arg) → arg = funcName(arg)
                    size_t origCallLen = funcName.size() + 1 + argCloseIdx;
                    std::string replacement = arg + " = " + funcName + "(" + arg + ")";
                    result.replace(callPos, origCallLen, replacement);
                    int callDiff = (int)replacement.size() - (int)origCallLen;
                    funcDefEnd += callDiff;
                    callSearch = callPos + replacement.size();
                }

                searchFrom = funcDefEnd;
            }
        }

        // Add return value before closing brace of shader_body
        {
            size_t sbIdx = result.find("shader_body");
            if (sbIdx != std::string::npos) {
                std::string after = result.substr(sbIdx);
                int closeIdx = FindClosingBracket(after, '{', '}', 0);
                if (closeIdx > 0) {
                    size_t insertPos = sbIdx + closeIdx;
                    result = result.substr(0, insertPos)
                           + "ret = " + retVarName + ";\n"
                           + result.substr(insertPos);
                }
            }
        }

        // Fix double-renamed variables
        replaceAll(result, "_conv_conv", "_conv");

        // Format
        result = BasicFormatShaderCode(result);

    } catch (...) {
        errors += "Conversion error (exception)\r\n";
    }

    // Dump converter output for diagnostics (per-pass)
    {
        wchar_t diagPath[MAX_PATH];
        const wchar_t* passTag = (passIdx == 0) ? L"image" : L"bufferA";
        swprintf(diagPath, MAX_PATH, L"%lsdiag_converter_%ls.txt", m_pEngine->m_szBaseDir, passTag);
        FILE* f = _wfopen(diagPath, L"w");
        if (f) {
            fprintf(f, "// CONV OUTPUT [%s]: %d chars, %d lines\n",
                    (passIdx == 0) ? "Image" : "Buffer A",
                    (int)result.size(),
                    (int)std::count(result.begin(), result.end(), '\n'));
            fwrite(result.c_str(), 1, result.size(), f);
            fclose(f);
        }
    }

    // Store converted HLSL in the current pass
    m_passes[passIdx].hlslOutput = result;

    // Update editor window with the converted HLSL (only for selected pass)
    if (passIdx == m_nSelectedPass && m_editorWindow && m_editorWindow->IsOpen())
        m_editorWindow->SetHLSL(result);

    // Show errors/status
    std::wstring errW;
    if (errors.empty()) {
        errW = L"Conversion complete";
        errW += L" (" + m_passes[passIdx].name + L").";
    } else {
        for (char c : errors) errW += (wchar_t)(unsigned char)c;
    }
    SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, errW.c_str());
}

// ─── Save / Load Import Project (JSON) ──────────────────────────────────

void ShaderImportWindow::SaveImportProject() {
    HWND hw = m_hWnd;

    // Sync editor text into m_passes before saving
    SyncEditorToPass();

    // Must have at least one pass with GLSL
    bool hasAnyGlsl = false;
    for (auto& p : m_passes)
        if (!p.glslSource.empty()) { hasAnyGlsl = true; break; }
    if (m_passes.empty() || !hasAnyGlsl) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"No GLSL text to save.");
        return;
    }

    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hw;
    ofn.lpstrFilter = L"Shader Import (*.json)\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"json";

    if (!GetSaveFileNameW(&ofn))
        return;

    JsonWriter w;
    w.BeginObject();
    w.String(L"type", L"shader_import");
    w.Int(L"version", 1);

    w.BeginArray(L"passes");
    for (auto& pass : m_passes) {
        w.BeginObject();
        w.String(L"name", pass.name.c_str());
        // Store GLSL as wide string (JsonWriter handles escaping)
        std::wstring wGlsl(pass.glslSource.begin(), pass.glslSource.end());
        w.String(L"glsl", wGlsl.c_str());
        // Store notes (if any)
        if (!pass.notes.empty()) {
            std::wstring wNotes(pass.notes.begin(), pass.notes.end());
            w.String(L"notes", wNotes.c_str());
        }
        // Store channel inputs
        w.BeginObject(L"channels");
        w.Int(L"ch0", pass.channels[0]);
        w.Int(L"ch1", pass.channels[1]);
        w.Int(L"ch2", pass.channels[2]);
        w.Int(L"ch3", pass.channels[3]);
        w.EndObject();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();

    if (w.SaveToFile(filePath))
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Import project saved.");
    else
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Failed to save import project.");
}

void ShaderImportWindow::LoadImportProject() {
    HWND hw = m_hWnd;

    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hw;
    ofn.lpstrFilter = L"Shader Import (*.json)\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn))
        return;

    JsonValue root = JsonLoadFile(filePath);
    if (!root.isObject()) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Failed to load JSON file.");
        return;
    }

    std::wstring type = root[L"type"].asString(L"");
    if (type != L"shader_import") {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Not a shader import project file.");
        return;
    }

    int version = root[L"version"].asInt(0);
    if (version < 1) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Unsupported import project version.");
        return;
    }

    // Rebuild passes from JSON
    m_passes.clear();
    const auto& passes = root[L"passes"];
    for (size_t i = 0; i < passes.size(); i++) {
        const auto& p = passes.at(i);
        ShaderPass sp;
        sp.name = p[L"name"].asString(L"Image");
        // Convert wide GLSL back to narrow
        std::wstring wGlsl = p[L"glsl"].asString(L"");
        sp.glslSource.reserve(wGlsl.size());
        for (wchar_t ch : wGlsl) {
            if (ch < 128) sp.glslSource += (char)ch;
            else sp.glslSource += '?';
        }
        sp.hlslOutput.clear();
        // Load notes (if present)
        if (p.has(L"notes")) {
            std::wstring wNotes = p[L"notes"].asString(L"");
            sp.notes.reserve(wNotes.size());
            for (wchar_t ch : wNotes) {
                if (ch < 128) sp.notes += (char)ch;
                else sp.notes += '?';
            }
        }
        // Load channel inputs (if present)
        if (p.has(L"channels")) {
            const auto& ch = p[L"channels"];
            sp.channels[0] = ch[L"ch0"].asInt(CHAN_NOISE_LQ);
            sp.channels[1] = ch[L"ch1"].asInt(CHAN_NOISE_LQ);
            sp.channels[2] = ch[L"ch2"].asInt(CHAN_NOISE_MQ);
            sp.channels[3] = ch[L"ch3"].asInt(CHAN_NOISE_HQ);
        }
        m_passes.push_back(std::move(sp));
    }

    // Ensure Image is always at index 0
    if (m_passes.empty())
        m_passes.push_back({L"Image", "", ""});
    else if (m_passes[0].name != L"Image")
        m_passes.insert(m_passes.begin(), {L"Image", "", ""});

    // If Buffer A exists, default Image ch0 to feedback
    if (m_passes.size() > 1 && m_passes[0].channels[0] == CHAN_NOISE_LQ)
        m_passes[0].channels[0] = CHAN_FEEDBACK;

    m_nSelectedPass = 0;
    RebuildPassList();
    SyncPassToEditor();

    SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Import project loaded.");
}

} // namespace mdrop
