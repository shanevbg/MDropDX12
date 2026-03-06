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
#include <set>
#include <cstdio>
#include <filesystem>

namespace mdrop {

// Forward declarations
static std::string WideHlslToNarrow(const std::wstring& hlslW, int len);

// Channel source lookup tables — MilkDrop noise (cubically interpolated)
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
    "sampler_bufferB",     // CHAN_BUFFER_B
};
// Shadertoy-compatible noise (uniform white noise, no interpolation, fixed seed)
static const char* kChannelSamplers_ST[] = {
    "sampler_noise_lq_st",    // CHAN_NOISE_LQ
    "sampler_noise_mq_st",    // CHAN_NOISE_MQ
    "sampler_noise_hq_st",    // CHAN_NOISE_HQ
    "sampler_feedback",       // CHAN_FEEDBACK (unchanged)
    "sampler_noisevol_lq_st", // CHAN_NOISEVOL_LQ
    "sampler_noisevol_hq_st", // CHAN_NOISEVOL_HQ
    "sampler_image",          // CHAN_IMAGE_PREV (unchanged)
    "sampler_audio",          // CHAN_AUDIO (unchanged)
    "sampler_rand00",         // CHAN_RANDOM_TEX (unchanged)
    "sampler_bufferB",        // CHAN_BUFFER_B (unchanged)
};
static const wchar_t* kChannelNames[] = {
    L"Noise LQ",      L"Noise MQ",       L"Noise HQ",
    L"Buffer A / Self", L"Noise Vol LQ",  L"Noise Vol HQ",
    L"Image (prev frame)", L"Audio (FFT + Wave)",
    L"Random Texture",
    L"Buffer B",
};
static const int kChannelTexDim[] = { 256, 256, 256, 0, 32, 32, 0, 0, 0, 0 }; // 0 = use texsize

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
                        // Notify import window for paste intelligence
                        if (m_pImportWindow) {
                            std::string narrow;
                            for (const wchar_t* p = pText; *p; p++) {
                                if (*p < 128) narrow += (char)*p;
                                else narrow += '?';
                            }
                            m_pImportWindow->OnPasteGLSL(narrow);
                        }
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
            // Add passes in priority order: Common → Buffer A → Buffer B
            bool hasCommon = false, hasBufferA = false, hasBufferB = false;
            for (auto& p : m_passes) {
                if (p.name == L"Common") hasCommon = true;
                if (p.name == L"Buffer A") hasBufferA = true;
                if (p.name == L"Buffer B") hasBufferB = true;
            }

            if (!hasCommon) {
                ShaderPass common;
                common.name = L"Common";
                m_passes.push_back(std::move(common));
            } else if (!hasBufferA) {
                ShaderPass bufA;
                bufA.name = L"Buffer A";
                bufA.channels[0] = CHAN_FEEDBACK;  // Buffer A ch0 = self-feedback
                m_passes.push_back(std::move(bufA));
                m_passes[0].channels[0] = CHAN_FEEDBACK;  // Image ch0 = Buffer A output
            } else if (!hasBufferB) {
                ShaderPass bufB;
                bufB.name = L"Buffer B";
                bufB.channels[0] = CHAN_BUFFER_B;  // Buffer B ch0 = self-feedback
                m_passes.push_back(std::move(bufB));
                // Image ch1 = Buffer B
                m_passes[0].channels[1] = CHAN_BUFFER_B;
                // Buffer A ch1 = Buffer B (if Buffer A exists)
                for (auto& p : m_passes) {
                    if (p.name == L"Buffer A") {
                        p.channels[1] = CHAN_BUFFER_B;
                        break;
                    }
                }
            } else {
                return 0;  // all passes added
            }
            SyncEditorToPass();
            m_nSelectedPass = (int)m_passes.size() - 1;
            RebuildPassList();
            SyncChannelCombos();
            OpenEditor();
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
        bool bufAOK = !p->m_bHasBufferA || (p->m_shaders.bufferA.bytecodeBlob != NULL);
        bool bufBOK = !p->m_bHasBufferB || (p->m_shaders.bufferB.bytecodeBlob != NULL);

        if (compOK && bufAOK && bufBOK) {
            errText = L"Shader compiled successfully.";
            if (p->m_bHasBufferA && p->m_bHasBufferB)
                errText += L" (Buffer A + Buffer B + Image)";
            else if (p->m_bHasBufferA)
                errText += L" (Buffer A + Image)";
        } else {
            errText = L"Compilation failed.\r\n";
            if (!compOK) errText += L"[Image/comp] ";
            if (!bufAOK) errText += L"[Buffer A] ";
            if (!bufBOK) errText += L"[Buffer B] ";
            errText += L"\r\n";
            // Read error files for each failed pass
            const wchar_t* diagNames[] = { nullptr, nullptr, nullptr };
            int diagCount = 0;
            if (!bufAOK) diagNames[diagCount++] = L"bufferA";
            if (!bufBOK) diagNames[diagCount++] = L"bufferB";
            if (!compOK) diagNames[diagCount++] = L"comp";
            for (int di = 0; di < diagCount; di++) {
                wchar_t errPath[MAX_PATH];
                swprintf(errPath, MAX_PATH, L"%lsdiag_%ls_shader_error.txt", p->m_szBaseDir, diagNames[di]);
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

    // Find passes by name
    std::string imageHlsl, bufAHlsl, bufBHlsl;
    for (auto& pass : m_passes) {
        if (pass.name == L"Image") imageHlsl = pass.hlslOutput;
        else if (pass.name == L"Buffer A") bufAHlsl = pass.hlslOutput;
        else if (pass.name == L"Buffer B") bufBHlsl = pass.hlslOutput;
    }

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

    // Apply Buffer B if present
    if (!bufBHlsl.empty()) {
        strncpy_s(p->m_pState->m_szBufferBShadersText, MAX_SHADER_TEXT_LEN, bufBHlsl.c_str(), _TRUNCATE);
        p->m_pState->m_nBufferBPSVersion = MD2_PS_5_0;
    }

    // Activate Shadertoy pipeline
    p->m_bShadertoyMode = true;
    p->m_bLoadingShadertoyMode = true;  // Must be set BEFORE recompile for correct shader wrapper
    p->m_nShadertoyStartFrame = p->GetFrame();

    p->m_nRecompileResult.store(1);
    p->EnqueueRenderCmd(RenderCmd::RecompileCompShader);

    // Status message
    {
        int storedLen = (int)strlen(p->m_pState->m_szCompShadersText);
        int bufALen = bufAHlsl.empty() ? 0 : (int)strlen(p->m_pState->m_szBufferAShadersText);
        int bufBLen = bufBHlsl.empty() ? 0 : (int)strlen(p->m_pState->m_szBufferBShadersText);
        wchar_t msg[256];
        bool truncated = ((int)imageHlsl.size() > storedLen + 1);
        if (truncated)
            swprintf(msg, 256, L"Compiling... (TRUNCATED: %d of %d chars stored)", storedLen, (int)imageHlsl.size());
        else if (bufALen > 0 || bufBLen > 0)
            swprintf(msg, 256, L"Compiling... (Image: %d, BufA: %d, BufB: %d chars)", storedLen, bufALen, bufBLen);
        else
            swprintf(msg, 256, L"Compiling... (%d chars)", storedLen);
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, msg);
    }
    SetTimer(hw, 1, 200, NULL);
}

// ─── Channel Auto-Detection ────────────────────────────────────────────

void ShaderImportWindow::AnalyzeChannels(ShaderPass& pass, bool jsonLoaded) {
    if (pass.glslSource.empty()) return;

    // Make local copies for pattern analysis — we normalize whitespace in these
    std::string src = pass.glslSource;

    // Combine pass source with Common source for analysis (functions may be in Common)
    std::string commonSrc;
    for (auto& p : m_passes) {
        if (p.name == L"Common") { commonSrc = p.glslSource; break; }
    }
    std::string combinedSrc = commonSrc + "\n" + src;

    // Normalize whitespace after function calls used in pattern matching
    // (GLSL allows "texelFetch( iChannel0" with space; our patterns expect no space)
    auto collapseSpaceAfterParen = [](std::string& s, const char* funcName) {
        size_t flen = strlen(funcName);
        std::string pat = std::string(funcName) + "(";
        size_t pos = 0;
        while ((pos = s.find(pat, pos)) != std::string::npos) {
            size_t start = pos + flen + 1; // position after '('
            size_t end = start;
            while (end < s.size() && s[end] == ' ') end++;
            if (end > start) s.erase(start, end - start);
            pos = start;
        }
    };
    collapseSpaceAfterParen(src, "texelFetch");
    collapseSpaceAfterParen(src, "texture");
    collapseSpaceAfterParen(src, "textureLod");
    collapseSpaceAfterParen(combinedSrc, "texelFetch");
    collapseSpaceAfterParen(combinedSrc, "texture");
    collapseSpaceAfterParen(combinedSrc, "textureLod");

    bool isBufferA = (pass.name == L"Buffer A");
    bool isBufferB = (pass.name == L"Buffer B");
    bool isImage   = (pass.name == L"Image");

    for (int ch = 0; ch < 4; ch++) {
        char chName[16];
        sprintf_s(chName, "iChannel%d", ch);

        if (src.find(chName) == std::string::npos)
            continue;  // This channel not used in source

        // High-confidence patterns (1, 2, 2b) can override any default including
        // CHAN_FEEDBACK — they detect specific texture types from source analysis.
        // Low-confidence patterns (3) only override noise defaults.

        // --- Pattern 1: Audio texture (texelFetch with ivec2(expr, 0) or ivec2(expr, 1)) ---
        // Audio textures are 512x2: row 0=FFT, row 1=waveform.
        // Distinguish from buffer reads which use ivec2(fragCoord) or ivec2(x, y).
        {
            char pat[64];
            sprintf_s(pat, "texelFetch(iChannel%d", ch);
            bool isAudio = false;
            size_t pos = 0;
            while ((pos = src.find(pat, pos)) != std::string::npos) {
                std::string region = src.substr(pos, 150);
                size_t iv = region.find("ivec2(");
                if (iv != std::string::npos) {
                    // Extract content inside ivec2(...) specifically
                    size_t ivStart = iv + 6; // past "ivec2("
                    size_t ivClose = region.find(')', ivStart);
                    if (ivClose != std::string::npos) {
                        std::string ivecArgs = region.substr(ivStart, ivClose - ivStart);
                        // Audio pattern: second arg is literal 0 or 1
                        // e.g. ivec2(x, 0) or ivec2(expr,1)
                        size_t lastComma = ivecArgs.rfind(',');
                        if (lastComma != std::string::npos) {
                            std::string secondArg = ivecArgs.substr(lastComma + 1);
                            // Trim whitespace
                            size_t a = secondArg.find_first_not_of(" \t");
                            if (a != std::string::npos) secondArg = secondArg.substr(a);
                            size_t b = secondArg.find_last_not_of(" \t");
                            if (b != std::string::npos) secondArg = secondArg.substr(0, b + 1);
                            if (secondArg == "0" || secondArg == "1") {
                                isAudio = true;
                                break;
                            }
                        }
                    }
                }
                pos += strlen(pat);
            }
            if (isAudio) {
                pass.channels[ch] = CHAN_AUDIO;
                continue;
            }
        }

        // --- Pattern 2: 3D texture (texture call with vec3 coordinate argument) ---
        // Must match vec3/float3/.xyz BEFORE the closing paren (as coords),
        // not after it (which would be a result swizzle like texture(...).xyz)
        {
            char pat[64];
            sprintf_s(pat, "texture(iChannel%d", ch);
            bool found3D = false;
            size_t pos = 0;
            while ((pos = src.find(pat, pos)) != std::string::npos) {
                // Find the closing paren of this texture() call
                size_t end = src.find(')', pos + strlen(pat));
                if (end == std::string::npos) { pos += strlen(pat); continue; }
                std::string args = src.substr(pos, end - pos);
                if (args.find("vec3") != std::string::npos ||
                    args.find("float3") != std::string::npos) {
                    found3D = true;
                    break;
                }
                pos += strlen(pat);
            }
            if (found3D) {
                pass.channels[ch] = CHAN_NOISEVOL_LQ;
                continue;
            }
        }

        // --- Pattern 2b: iChannelN passed to a function declared with sampler3D ---
        // Common code often has helper functions like fbm1(sampler3D tex, vec3 x)
        // that are called with iChannelN. Detect via combinedSrc.
        {
            char pat[64];
            sprintf_s(pat, "iChannel%d", ch);
            bool found3D = false;
            // Collect function names that take sampler3D as first param
            std::vector<std::string> sampler3DFuncs;
            {
                size_t pos = 0;
                while ((pos = combinedSrc.find("sampler3D", pos)) != std::string::npos) {
                    // Walk backwards to find function name: "type funcName( sampler3D ..."
                    // Find the opening paren before sampler3D
                    size_t pp = pos;
                    while (pp > 0 && combinedSrc[pp - 1] == ' ') pp--;
                    if (pp > 0 && combinedSrc[pp - 1] == '(') {
                        size_t parenPos = pp - 1;
                        size_t ne = parenPos;
                        while (ne > 0 && combinedSrc[ne - 1] == ' ') ne--;
                        size_t ns = ne;
                        while (ns > 0 && (isalnum((unsigned char)combinedSrc[ns - 1]) || combinedSrc[ns - 1] == '_')) ns--;
                        if (ne > ns) {
                            sampler3DFuncs.push_back(combinedSrc.substr(ns, ne - ns));
                        }
                    }
                    pos += 9;
                }
            }
            // Check if iChannelN is passed to any sampler3D function
            for (const auto& fn : sampler3DFuncs) {
                std::string callPat = fn + "(";
                size_t pos = 0;
                while ((pos = src.find(callPat, pos)) != std::string::npos) {
                    size_t argStart = pos + callPat.size();
                    size_t end = src.find(')', argStart);
                    if (end != std::string::npos) {
                        std::string args = src.substr(argStart, end - argStart);
                        if (args.find(pat) != std::string::npos) {
                            found3D = true;
                            break;
                        }
                    }
                    pos += callPat.size();
                }
                if (found3D) break;
            }
            if (found3D) {
                pass.channels[ch] = CHAN_NOISEVOL_LQ;
                continue;
            }
        }

        // --- Pattern 2c: Buffer B ch0 cross-buffer read (high confidence) ---
        // If Buffer B ch0 uses texelFetch and Buffer A exists, it reads Buffer A output.
        if (isBufferB && ch == 0) {
            bool hasBufA = false;
            for (auto& p : m_passes)
                if (p.name == L"Buffer A") { hasBufA = true; break; }
            char fetchPat[64];
            sprintf_s(fetchPat, "texelFetch(iChannel%d", ch);
            if (hasBufA && src.find(fetchPat) != std::string::npos) {
                pass.channels[ch] = CHAN_FEEDBACK;  // Read from Buffer A
                continue;
            }
        }

        // --- Pattern 2d: Buffer A self-feedback via textureLod with screen-space UVs ---
        // Some Shadertoy shaders use textureLod (not texelFetch) for Buffer A self-feedback:
        //   textureLod(iChannelN, vec2(x,y)/iResolution.xy, 0.0)  — reading stored state
        //   textureLod(iChannelN, spos, 0.0)  — reprojected screen-space reads
        // Detect the /iResolution pattern as strong evidence of screen-space self-reads.
        if (isBufferA) {
            char lodPat[64];
            sprintf_s(lodPat, "textureLod(iChannel%d,", ch);
            size_t lodPos = src.find(lodPat);
            if (lodPos != std::string::npos) {
                // Check if any textureLod call uses /iResolution (screen-space coords)
                bool hasScreenSpaceRead = false;
                size_t searchPos = lodPos;
                while (searchPos != std::string::npos) {
                    // Find the MATCHING closing paren (handle nested parens)
                    int depth = 1;
                    size_t scanPos = searchPos + strlen(lodPat);
                    // We're past the opening '(' of textureLod(
                    while (scanPos < src.size() && depth > 0) {
                        if (src[scanPos] == '(') depth++;
                        else if (src[scanPos] == ')') depth--;
                        scanPos++;
                    }
                    if (depth == 0) {
                        std::string callText = src.substr(searchPos, scanPos - searchPos);
                        if (callText.find("/iResolution") != std::string::npos ||
                            callText.find("/ iResolution") != std::string::npos) {
                            hasScreenSpaceRead = true;
                            break;
                        }
                    }
                    searchPos = src.find(lodPat, searchPos + 1);
                }
                if (hasScreenSpaceRead) {
                    pass.channels[ch] = CHAN_FEEDBACK;
                    continue;
                }
            }
        }

        // --- Pattern 2e: JSON CHAN_FEEDBACK self-feedback validation ---
        // Only applies to Buffer A reading itself (CHAN_FEEDBACK on isBufferA).
        // Self-feedback on Shadertoy always uses texelFetch or textureLod with
        // screen-space coords (caught by 2d above). If neither pattern matches,
        // the JSON value is wrong — it's noise.
        // Cross-buffer reads (CHAN_FEEDBACK on Image/BufferB, CHAN_BUFFER_B on any)
        // can use any sampling method, so we trust the JSON for those.
        if (jsonLoaded && pass.channels[ch] == CHAN_FEEDBACK && isBufferA) {
            char fetchPat[64];
            sprintf_s(fetchPat, "texelFetch(iChannel%d", ch);
            if (src.find(fetchPat) == std::string::npos) {
                // No texelFetch evidence — downgrade to noise default
                const int noiseDefaults[] = {CHAN_NOISE_LQ, CHAN_NOISE_LQ, CHAN_NOISE_MQ, CHAN_NOISE_HQ};
                pass.channels[ch] = noiseDefaults[ch];
            }
            continue;
        }

        // --- Pattern 3: Self-feedback / buffer reads ---
        // Low confidence — skip entirely for JSON-loaded passes (JSON is more reliable).
        // For non-JSON passes, only override noise defaults.
        if (jsonLoaded) continue;
        if (pass.channels[ch] != CHAN_NOISE_LQ &&
            pass.channels[ch] != CHAN_NOISE_MQ &&
            pass.channels[ch] != CHAN_NOISE_HQ)
            continue;

        // Only ch0 is typically self-feedback; other channels may be noise, audio, etc.
        if (isBufferA) {
            if (ch == 0)
                pass.channels[ch] = CHAN_FEEDBACK;
            continue;  // leave other channels at defaults
        }
        if (isBufferB) {
            if (ch == 0)
                pass.channels[ch] = CHAN_BUFFER_B;  // Self-feedback (no texelFetch evidence)
            continue;  // leave other channels at defaults
        }
        if (isImage) {
            // Only assign buffer channels when there's texelFetch evidence
            // (buffer reads use texelFetch for screen-sized pixel-exact reads).
            // Don't blindly assign buffers just because they exist.
            char fetchPat[64];
            sprintf_s(fetchPat, "texelFetch(iChannel%d", ch);
            if (src.find(fetchPat) != std::string::npos) {
                bool hasBufA = false, hasBufB = false;
                for (auto& p : m_passes) {
                    if (p.name == L"Buffer A") hasBufA = true;
                    if (p.name == L"Buffer B") hasBufB = true;
                }
                bool feedbackUsed = false, bufBUsed = false;
                for (int k = 0; k < ch; k++) {
                    if (pass.channels[k] == CHAN_FEEDBACK) feedbackUsed = true;
                    if (pass.channels[k] == CHAN_BUFFER_B) bufBUsed = true;
                }
                if (!feedbackUsed && hasBufA) {
                    pass.channels[ch] = CHAN_FEEDBACK;
                    continue;
                }
                if (!bufBUsed && hasBufB) {
                    pass.channels[ch] = CHAN_BUFFER_B;
                    continue;
                }
            }
        }
    }
}

// ─── Paste Intelligence ────────────────────────────────────────────────

void ShaderImportWindow::OnPasteGLSL(const std::string& glsl) {
    if (glsl.empty()) return;

    // Detect if this is Common code (no mainImage function)
    bool hasMainImage = (glsl.find("mainImage") != std::string::npos);

    if (!hasMainImage) {
        bool hasCommon = false;
        for (auto& p : m_passes) {
            if (p.name == L"Common") {
                hasCommon = true;
                break;
            }
        }
        if (!hasCommon) {
            ShaderPass common;
            common.name = L"Common";
            common.glslSource = glsl;
            m_passes.push_back(std::move(common));
            m_nSelectedPass = (int)m_passes.size() - 1;
            RebuildPassList();
            SyncPassToEditor();
            SyncChannelCombos();
            HWND hw = GetHWND();
            if (hw)
                SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT,
                    L"Auto-detected Common pass (no mainImage found). Added Common tab.");
            return;
        }
    }

    // For render passes: analyze channels on the current pass
    if (m_nSelectedPass >= 0 && m_nSelectedPass < (int)m_passes.size()) {
        ShaderPass& pass = m_passes[m_nSelectedPass];
        pass.glslSource = glsl;
        AnalyzeChannels(pass);
        SyncChannelCombos();
    }
}

// ─── Convert & Apply (all passes) ───────────────────────────────────────

void ShaderImportWindow::ConvertAndApply() {
    SyncEditorToPass();

    // Find Common pass GLSL (if any) — prepended to all other passes before conversion
    std::string commonGlsl;
    for (auto& p : m_passes) {
        if (p.name == L"Common") {
            commonGlsl = p.glslSource;
            p.hlslOutput.clear();  // Common has no mainImage, no HLSL output
            break;
        }
    }

    // Tier 2: Auto-detect channel types from GLSL source
    for (auto& p : m_passes) {
        if (p.name == L"Common") continue;
        AnalyzeChannels(p);
    }
    SyncChannelCombos();  // Update UI to reflect auto-detected channels

    // Diagnostic: log channel assignments after analysis
    {
        std::string diag = "Channel assignments:\n";
        for (auto& p : m_passes) {
            if (p.name == L"Common") continue;
            char line[256];
            char name8[32];
            WideCharToMultiByte(CP_ACP, 0, p.name.c_str(), -1, name8, 32, NULL, NULL);
            sprintf_s(line, "  %s: ch0=%s ch1=%s ch2=%s ch3=%s (glsl=%d chars)\n",
                name8,
                kChannelSamplers[p.channels[0]], kChannelSamplers[p.channels[1]],
                kChannelSamplers[p.channels[2]], kChannelSamplers[p.channels[3]],
                (int)p.glslSource.size());
            diag += line;
        }
        DebugLogA(diag.c_str());
    }

    // Convert each pass in order (Buffer B/A first if present, then Image)
    for (int i = (int)m_passes.size() - 1; i >= 0; i--) {
        if (m_passes[i].name == L"Common") continue;  // skip Common (no mainImage)
        if (m_passes[i].glslSource.empty()) continue;
        // Prepend Common GLSL to each pass before conversion
        if (!commonGlsl.empty()) {
            std::string saved = m_passes[i].glslSource;
            m_passes[i].glslSource = commonGlsl + "\n" + saved;
            m_passes[i].hlslOutput.clear();
            ConvertGLSLtoHLSL(i);
            m_passes[i].glslSource = saved;  // restore original (don't persist Common prefix)
        } else {
            m_passes[i].hlslOutput.clear();
            ConvertGLSLtoHLSL(i);
        }
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

    // Find passes by name
    bool hasBufferA = false, hasBufferB = false, hasCommon = false;
    std::string commonGlsl;
    for (auto& pass : m_passes) {
        if (pass.name == L"Buffer A" && !pass.hlslOutput.empty()) hasBufferA = true;
        if (pass.name == L"Buffer B" && !pass.hlslOutput.empty()) hasBufferB = true;
        if (pass.name == L"Common" && !pass.glslSource.empty()) {
            hasCommon = true;
            commonGlsl = pass.glslSource;
        }
    }

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
                for (auto& pass : m_passes) {
                    if (pass.notes.empty()) continue;
                    std::wstring wn(pass.notes.begin(), pass.notes.end());
                    if (pass.name == L"Image") w.String(L"image", wn.c_str());
                    else if (pass.name == L"Buffer A") w.String(L"bufferA", wn.c_str());
                    else if (pass.name == L"Buffer B") w.String(L"bufferB", wn.c_str());
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
        // Write Common GLSL (stored as raw GLSL, not HLSL — for round-tripping)
        if (hasCommon) {
            std::wstring wCommon(commonGlsl.begin(), commonGlsl.end());
            w.String(L"common", wCommon.c_str());
        }
        for (auto& pass : m_passes) {
            if (pass.name == L"Buffer A" && !pass.hlslOutput.empty())
                w.String(L"bufferA", hlslToWide(pass.hlslOutput).c_str());
            else if (pass.name == L"Buffer B" && !pass.hlslOutput.empty())
                w.String(L"bufferB", hlslToWide(pass.hlslOutput).c_str());
            else if (pass.name == L"Image" && !pass.hlslOutput.empty())
                w.String(L"image", hlslToWide(pass.hlslOutput).c_str());
        }

        // Channel mappings
        w.BeginObject(L"channels");
        if (hasBufferA) {
            w.BeginObject(L"bufferA");
            w.String(L"iChannel0", L"self");
            w.EndObject();
        }
        if (hasBufferB) {
            w.BeginObject(L"bufferB");
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
    // Word-boundary-aware replacement: only replace oldName when surrounded by non-identifier chars
    auto isIdent = [](char c) -> bool {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    std::string res = inp;
    size_t pos = 0;
    while ((pos = res.find(oldName, pos)) != std::string::npos) {
        bool boundBefore = (pos == 0) || !isIdent(res[pos - 1]);
        bool boundAfter = (pos + oldName.size() >= res.size()) || !isIdent(res[pos + oldName.size()]);
        if (boundBefore && boundAfter) {
            res.replace(pos, oldName.size(), newName);
            pos += newName.size();
        } else {
            pos++;
        }
    }
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
        // Also normalize already-replaced floatNxN forms
        replaceAll(result, "*= float", "*=float");
        replaceAll(result, "* float", "*float");

        // GLSL mat3(a,b,c) stores a,b,c as COLUMNS; HLSL float3x3(a,b,c) stores as ROWS.
        // Instead of transposing constructors, we swap mul() argument order:
        //   GLSL "v *= matN(...)" = "v = v * M" → HLSL "v = mul(floatNxN(...), v)"
        //   GLSL "x * matN(...)" = "x * M"     → HLSL "mul(floatNxN(...), x)"
        // Phase 1b handles named matrix variables with the same swap.
        // Must check both "matN(" and "floatNxN(" since Phase 1 may have already replaced.

        // Helper: find *=matN( or *=floatNxN( pattern and return {index, matSize, skip}
        auto findMulAssign = [&](size_t& outIdx, int& outSize, size_t& outSkip) -> bool {
            for (int ms = 2; ms <= 4; ms++) {
                char tokens[2][16];
                sprintf_s(tokens[0], "*=mat%d(", ms);
                sprintf_s(tokens[1], "*=float%dx%d(", ms, ms);
                for (int ti = 0; ti < 2; ti++) {
                    size_t idx = result.find(tokens[ti]);
                    if (idx != std::string::npos) {
                        outIdx = idx;
                        outSize = ms;
                        outSkip = strlen(tokens[ti]);
                        return true;
                    }
                }
            }
            return false;
        };
        auto findMulRight = [&](size_t& outIdx, int& outSize, size_t& outSkip) -> bool {
            for (int ms = 2; ms <= 4; ms++) {
                char tokens[2][16];
                sprintf_s(tokens[0], "*mat%d(", ms);
                sprintf_s(tokens[1], "*float%dx%d(", ms, ms);
                for (int ti = 0; ti < 2; ti++) {
                    size_t idx = result.find(tokens[ti]);
                    if (idx != std::string::npos) {
                        outIdx = idx;
                        outSize = ms;
                        outSkip = strlen(tokens[ti]);
                        return true;
                    }
                }
            }
            return false;
        };

        size_t index; int matSize; size_t skip;
        if (findMulAssign(index, matSize, skip)) {
            // e.g. "uv *= mat2(cos(a), -sin(a), sin(a), cos(a));"
            std::string fac1 = result.substr(0, index);
            // Trim right
            while (!fac1.empty() && fac1.back() == ' ') fac1.pop_back();
            std::string indent_str;
            size_t fac1Start = fac1.find_first_not_of(" \t");
            if (fac1Start != std::string::npos)
                indent_str = fac1.substr(0, fac1Start);
            fac1 = fac1.substr(fac1Start != std::string::npos ? fac1Start : 0);

            std::string rest = result.substr(index + skip); // after opening paren
            int closingIdx = FindClosingBracket(rest, '(', ')', 1);
            if (closingIdx > 0) {
                std::string args = rest.substr(0, closingIdx);
                // v *= matN(args) → v = mul(floatNxN(args), v)  [swapped for column-major]
                result = indent_str + fac1 + " = mul(float"
                       + std::to_string(matSize) + "x" + std::to_string(matSize)
                       + "(" + args + "), " + fac1 + ");";
            }
        } else if (findMulRight(index, matSize, skip)) {
            std::string prefix = result.substr(0, index);
            // Trim to get fac1 (last word before *mat)
            while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
            size_t lastSpace = prefix.rfind(' ');
            std::string fac1 = (lastSpace != std::string::npos) ? prefix.substr(lastSpace + 1) : prefix;
            std::string left = (lastSpace != std::string::npos) ? prefix.substr(0, lastSpace + 1) : "";

            std::string rest = result.substr(index + skip);
            int closingIdx = FindClosingBracket(rest, '(', ')', 1);
            if (closingIdx > 0) {
                std::string args = rest.substr(0, closingIdx);
                // x * matN(args) → mul(floatNxN(args), x)  [swapped for column-major]
                result = left + "mul(float"
                       + std::to_string(matSize) + "x" + std::to_string(matSize)
                       + "(" + args + "), " + fac1 + ");";
            }
        }

        // Handle matN(args)*expr or floatNxN(args)*expr — matrix constructor on LEFT of multiply
        // GLSL (column-major): M * v → HLSL (row-major): mul(v, M)
        // Must check both forms since Phase 1 may have already replaced mat2→float2x2
        for (int ms = 2; ms <= 4; ms++) {
            char matTokens[2][16];
            sprintf_s(matTokens[0], "mat%d(", ms);
            sprintf_s(matTokens[1], "float%dx%d(", ms, ms);
            for (int ti = 0; ti < 2; ti++) {
                const char* matToken = matTokens[ti];
                size_t mpos = result.find(matToken);
                if (mpos != std::string::npos) {
                    std::string afterMat = result.substr(mpos + strlen(matToken));
                    int closeIdx = FindClosingBracket(afterMat, '(', ')', 1);
                    if (closeIdx > 0) {
                        size_t afterClose = mpos + strlen(matToken) + closeIdx + 1;
                        // Skip whitespace after closing paren
                        size_t starPos = afterClose;
                        while (starPos < result.size() && result[starPos] == ' ') starPos++;
                        if (starPos < result.size() && result[starPos] == '*') {
                            // Make sure it's not *= (handled above)
                            if (starPos + 1 < result.size() && result[starPos + 1] == '=') continue;
                            // Extract the right-hand operand after *
                            size_t rhsStart = starPos + 1;
                            while (rhsStart < result.size() && result[rhsStart] == ' ') rhsStart++;
                            // Find end of RHS: stop at ; or ) or , respecting brackets
                            size_t rhsEnd = rhsStart;
                            int depth = 0;
                            while (rhsEnd < result.size()) {
                                char c = result[rhsEnd];
                                if (c == '(' || c == '[') depth++;
                                else if (c == ')' || c == ']') { if (depth == 0) break; depth--; }
                                else if ((c == ';' || c == ',') && depth == 0) break;
                                rhsEnd++;
                            }
                            if (rhsEnd > rhsStart) {
                                std::string matArgs = afterMat.substr(0, closeIdx);
                                std::string rhs = result.substr(rhsStart, rhsEnd - rhsStart);
                                // Trim trailing whitespace from rhs
                                while (!rhs.empty() && rhs.back() == ' ') rhs.pop_back();
                                std::string prefix = result.substr(0, mpos);
                                std::string suffix = result.substr(rhsEnd);
                                result = prefix + "mul(" + rhs + ", float"
                                       + std::to_string(ms) + "x" + std::to_string(ms)
                                       + "(" + matArgs + "))" + suffix;
                                break; // handled, don't check second token form
                            }
                        }
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

        // Integer/unsigned/boolean vector types (must come BEFORE vec→float to avoid ivec2→ifloat2)
        replaceAll(inp, "uvec2", "uint2");
        replaceAll(inp, "uvec3", "uint3");
        replaceAll(inp, "uvec4", "uint4");
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

        // Replace hardcoded resolution constants with texsize.xy.
        // Shadertoy authors often hardcode their preview resolution (e.g. vec2(1280,720))
        // instead of using iResolution. At different resolutions these cause wrong UV offsets,
        // blur radii, etc. vec2→float2 already happened above, so match HLSL form.
        {
            const char* hardcoded[] = {
                "float2(1280.0,720.0)",  "float2(1280.,720.)",
                "float2(1920.0,1080.0)", "float2(1920.,1080.)",
                "float2(800.0,450.0)",   "float2(800.,450.)",
                "float2(640.0,360.0)",   "float2(640.,360.)",
                "float2(1024.0,768.0)",  "float2(1024.,768.)",
                "float2(1280.0,800.0)",  "float2(1280.,800.)",
                "float2(1024.0,576.0)",  "float2(1024.,576.)",
            };
            for (auto& pat : hardcoded) {
                if (inp.find(pat) != std::string::npos) {
                    replaceAll(inp, pat, "texsize.xy");
                    errors += std::string("Replaced hardcoded resolution ") + pat + " with texsize.xy\r\n";
                }
            }
        }

        replaceAll(inp, "iFrame", "frame");
        replaceAll(inp, "iMouse", "_c14");  // Direct ref to avoid local 'mouse' shadowing #define
        // ZERO/ZEROU: Shadertoy anti-optimization trick (#define ZERO min(iFrame,0)).
        // Always 0 at runtime but prevents GLSL compiler constant-folding.
        // HLSL fxc needs literal 0 to unroll loops, so remove the #defines and replace usage.
        {
            auto removeDef = [&](const std::string& name) {
                std::string pattern = "#define " + name;
                size_t pos = inp.find(pattern);
                if (pos != std::string::npos) {
                    size_t eol = inp.find('\n', pos);
                    if (eol != std::string::npos) eol++; else eol = inp.size();
                    inp.erase(pos, eol - pos);
                }
            };
            removeDef("ZEROU");
            removeDef("ZERO");
            inp = WholeWordReplace(inp, "ZEROU", "0u");
            inp = WholeWordReplace(inp, "ZERO", "0");
        }
        // gl_FragCoord: GLSL built-in pixel coordinate (used in Common helper functions)
        // Replaced with a static global set at shader_body entry
        replaceAll(inp, "gl_FragCoord", "_gl_FragCoord");
        // iChannel samplers — use per-pass channel configuration
        {
            const int* ch = m_passes[passIdx].channels;
            for (int i = 0; i < 4; i++) {
                char from[16], to[32];
                sprintf_s(from, "iChannel%d", i);
                int src = (ch[i] >= 0 && ch[i] < CHAN_COUNT) ? ch[i] : 0;
                strcpy_s(to, kChannelSamplers_ST[src]);
                replaceAll(inp, from, to);
            }
        }
        replaceAll(inp, "texture(", "tex2D(");
        // Volume textures need tex3D, not tex2D (sampler3D + float3 coords)
        replaceAll(inp, "tex2D(sampler_noisevol_lq,", "tex3D(sampler_noisevol_lq,");
        replaceAll(inp, "tex2D(sampler_noisevol_hq,", "tex3D(sampler_noisevol_hq,");
        replaceAll(inp, "tex2D(sampler_noisevol_lq_st,", "tex3D(sampler_noisevol_lq_st,");
        replaceAll(inp, "tex2D(sampler_noisevol_hq_st,", "tex3D(sampler_noisevol_hq_st,");
        replaceAll(inp, "textureLod(", "tex2Dlod_conv(");
        replaceAll(inp, "texelFetch(", "texelFetch_conv(");

        // Fix tex2D/tex2Dlod_conv inside functions that take sampler3D parameters.
        // e.g. noise1(sampler3D tex, vec3 x) { return tex2Dlod_conv(tex,...) }
        //    → needs tex3Dlod(tex,...) instead.
        {
            size_t pos = 0;
            while ((pos = inp.find("sampler3D", pos)) != std::string::npos) {
                // Find parameter name after "sampler3D "
                size_t nameStart = pos + 9;
                while (nameStart < inp.size() && inp[nameStart] == ' ') nameStart++;
                size_t nameEnd = nameStart;
                while (nameEnd < inp.size() && (isalnum((unsigned char)inp[nameEnd]) || inp[nameEnd] == '_')) nameEnd++;
                if (nameEnd <= nameStart) { pos = nameEnd; continue; }
                std::string paramName = inp.substr(nameStart, nameEnd - nameStart);

                // Find the function body: scan forward to find { ... }
                size_t braceOpen = inp.find('{', nameEnd);
                if (braceOpen == std::string::npos) { pos = nameEnd; continue; }
                // Make sure we're in a function context (not a struct)
                // Find matching closing brace
                int depth = 1;
                size_t braceClose = braceOpen + 1;
                while (braceClose < inp.size() && depth > 0) {
                    if (inp[braceClose] == '{') depth++;
                    else if (inp[braceClose] == '}') depth--;
                    braceClose++;
                }
                if (depth != 0) { pos = nameEnd; continue; }

                // Within this function body, replace tex2D(paramName, → tex3D(paramName,
                // tex2Dlod_conv already has a sampler3D overload, so leave those alone.
                std::string body = inp.substr(braceOpen, braceClose - braceOpen);
                std::string from2D = "tex2D(" + paramName + ",";
                std::string to3D = "tex3D(" + paramName + ",";
                size_t rpos = 0;
                while ((rpos = body.find(from2D, rpos)) != std::string::npos) {
                    body.replace(rpos, from2D.size(), to3D);
                    rpos += to3D.size();
                }
                inp = inp.substr(0, braceOpen) + body + inp.substr(braceClose);
                pos = braceOpen + body.size();
            }
        }
        // Normalize whitespace after texelFetch_conv( so specialization matches
        while (inp.find("texelFetch_conv( ") != std::string::npos)
            replaceAll(inp, "texelFetch_conv( ", "texelFetch_conv(");
        // Specialize texelFetch for noise textures (256x256) vs screen-sized feedback
        replaceAll(inp, "texelFetch_conv(sampler_noise_lq,", "texelFetch_noise(sampler_noise_lq,");
        replaceAll(inp, "texelFetch_conv(sampler_noise_mq,", "texelFetch_noise(sampler_noise_mq,");
        replaceAll(inp, "texelFetch_conv(sampler_noise_hq,", "texelFetch_noise(sampler_noise_hq,");
        // Image pass: flip V on external texture reads to compensate for the
        // flipped quad UVs (Shadertoy fragCoord convention). Buffer/feedback reads
        // are self-consistent (written and read with same convention) so they stay.
        if (m_passes[passIdx].name == L"Image") {
            // External samplers that need V-flip (non-feedback, non-buffer)
            // Only 2D samplers — volume textures (sampler3D) can't use tex2D_flipV
            const char* extSamplers[] = {
                "sampler_noise_lq", "sampler_noise_mq", "sampler_noise_hq",
                "sampler_noise_lq_st", "sampler_noise_mq_st", "sampler_noise_hq_st",
                "sampler_image", "sampler_audio", "sampler_rand00"
            };
            for (auto* s : extSamplers) {
                char from[64], to[64];
                sprintf_s(from, "tex2D(%s,", s);
                sprintf_s(to, "tex2D_flipV(%s,", s);
                replaceAll(inp, from, to);
                sprintf_s(from, "tex2Dlod_conv(%s,", s);
                sprintf_s(to, "tex2Dlod_flipV(%s,", s);
                replaceAll(inp, from, to);
            }
        }

        // textureSize(sampler, mip) → int2(texsize.xy)
        // Shadertoy uses this for "is texture loaded" checks; in MDropDX12 textures are always bound.
        replaceAll(inp, "textureSize(", "_texSize(");

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
                // Pre-replacement GLSL forms (mat3 not yet replaced by Phase 3)
                {"mat2", true}, {"mat3", true}, {"mat4", true},
                // Post-replacement HLSL forms
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
                    // Check for "matFunc(args) * expr" pattern (matrix on LEFT of multiply)
                    // GLSL (column-major): M * v → HLSL (row-major): mul(v, M) for square
                    {
                        size_t afterCall = callEnd;
                        while (afterCall < inp.size() && inp[afterCall] == ' ') afterCall++;
                        if (afterCall < inp.size() && inp[afterCall] == '*' &&
                            (afterCall + 1 >= inp.size() || inp[afterCall + 1] != '=')) {
                            size_t rhsStart = afterCall + 1;
                            while (rhsStart < inp.size() && inp[rhsStart] == ' ') rhsStart++;
                            // Find end of RHS operand
                            size_t rhsEnd = rhsStart;
                            int depth = 0;
                            while (rhsEnd < inp.size()) {
                                char c = inp[rhsEnd];
                                if (c == '(' || c == '[') depth++;
                                else if (c == ')' || c == ']') { if (depth == 0) break; depth--; }
                                else if ((c == ';' || c == ',' || c == '+' || c == '-') && depth == 0) break;
                                rhsEnd++;
                            }
                            if (rhsEnd > rhsStart) {
                                std::string rhs = inp.substr(rhsStart, rhsEnd - rhsStart);
                                while (!rhs.empty() && rhs.back() == ' ') rhs.pop_back();
                                std::string prefix = inp.substr(0, pos);
                                std::string suffix = inp.substr(rhsEnd);
                                std::string repl;
                                if (mf.isSquare)
                                    repl = "mul(" + rhs + ", " + funcCall + ")";  // SWAPPED
                                else
                                    repl = "mul(" + funcCall + ", " + rhs + ")";  // STANDARD
                                inp = prefix + repl + suffix;
                                pos = prefix.size() + repl.size();
                                continue;
                            }
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

        // Add textureSize helper if needed (GLSL texture dimension query)
        if (inp.find("_texSize(") != std::string::npos) {
            std::string helper =
                "// CONV: textureSize → returns texsize.xy (textures always bound in MDropDX12)\n"
                "int2 _texSize(sampler2D s, int mip) { return int2(texsize.xy); }\n"
                "int3 _texSize(sampler3D s, int mip) { return int3(32, 32, 32); }\n\n";
            inpHeader = helper + inpHeader;
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

        // Add V-flip texture helpers for Image pass external texture reads
        if (inp.find("tex2D_flipV(") != std::string::npos || inp.find("tex2Dlod_flipV(") != std::string::npos) {
            std::string helper =
                "// CONV: V-flip wrappers for external textures in Image pass\n"
                "// Compensates for flipped quad UVs (Shadertoy fragCoord convention)\n"
                "float4 tex2D_flipV(sampler2D _s, float2 _tc) {\n"
                "  return tex2D(_s, float2(_tc.x, 1.0 - _tc.y));\n"
                "}\n"
                "float4 tex2Dlod_flipV(sampler2D _s, float2 _tc, float _lod) {\n"
                "  return tex2Dlod(_s, float4(_tc.x, 1.0 - _tc.y, 0, _lod));\n"
                "}\n\n";
            inpHeader = helper + inpHeader;
        }

        // Add textureLod helper if needed (GLSL explicit-LOD texture fetch)
        if (inp.find("tex2Dlod_conv(") != std::string::npos) {
            std::string helper =
                "// CONV: textureLod → tex2Dlod wrapper\n"
                "float4 tex2Dlod_conv(sampler2D s, float2 uv_tl, float l) {\n"
                "  return tex2Dlod(s, float4(uv_tl, 0, l));\n"
                "}\n"
                "float4 tex2Dlod_conv(sampler3D s, float3 uv_tl, float l) {\n"
                "  return tex3Dlod(s, float4(uv_tl, l));\n"
                "}\n\n";
            inpHeader = helper + inpHeader;
        }

        // Add lessThan helper if needed
        if (inp.find("lessThan") != std::string::npos || inp.find("lessthan") != std::string::npos) {
            std::string ltHelper =
                "float4 lessThan(float4 a, float4 b) { return float4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n\n";
            inpHeader = ltHelper + inpHeader;
        }

        // Add gl_FragCoord global if needed (GLSL built-in used in Common helpers)
        if (inp.find("_gl_FragCoord") != std::string::npos) {
            inpHeader = "static float4 _gl_FragCoord;\n" + inpHeader;
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
        // Initialize gl_FragCoord from pixel coordinates (set before any user code runs)
        if (inp.find("_gl_FragCoord") != std::string::npos)
            sbHeader << "  _gl_FragCoord = float4(uv * texsize.xy, 0, 1);\n";

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
        // Shadertoy shaders often declare local 'mouse' variables from iMouse.
        // #undef the MilkDrop mouse macros so they don't shadow _c14.
        sbHeader << "#undef mouse\n#undef mouse_x\n#undef mouse_y\n"
                    "#undef mouse_pos\n#undef mouse_clicked\n";

        // Find the opening brace of mainImage body and replace everything before it
        size_t braceIdx = inpMain.find('{');
        if (braceIdx != std::string::npos)
            inpMain = sbHeader.str() + inpMain.substr(braceIdx + 1);
        else
            inpMain = sbHeader.str() + inpMain;

        // Fix bare return; in mainImage body — must set _return_value before early exit.
        // _return_value is the out parameter added by engine_shaders.cpp's PS wrapper.
        // Without this, early return leaves _return_value uninitialized (undefined output).
        // Also set ret so that both the converter's ret=fragColor and engine's _return_value=ret
        // paths stay consistent.
        {
            size_t pos = 0;
            std::string retFix = "ret = " + retVarName + "; _return_value = ret; return;";
            while ((pos = inpMain.find("return;", pos)) != std::string::npos) {
                inpMain.replace(pos, 7, retFix);
                pos += retFix.size();
            }
        }

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

        // Fix GLSL vector comparisons in if/while conditions:
        // GLSL: if(v.xy == vec2(-0.5)) returns scalar bool
        // HLSL: v.xy == float2(-0.5) returns bool2, not scalar — must wrap with all()
        {
            size_t searchPos = 0;
            while (searchPos < result.size()) {
                // Find "if(" or "while("
                size_t ifPos = result.find("if(", searchPos);
                size_t if2Pos = result.find("if (", searchPos);
                size_t whilePos = result.find("while(", searchPos);
                size_t while2Pos = result.find("while (", searchPos);
                // Find earliest
                size_t pos = std::string::npos;
                for (size_t p : {ifPos, if2Pos, whilePos, while2Pos})
                    if (p != std::string::npos && (pos == std::string::npos || p < pos)) pos = p;
                if (pos == std::string::npos) break;
                // Find the opening paren
                size_t parenOpen = result.find('(', pos);
                if (parenOpen == std::string::npos) { searchPos = pos + 1; continue; }
                // Find matching closing paren
                std::string afterParen = result.substr(parenOpen + 1);
                int closeIdx = FindClosingBracket(afterParen, '(', ')', 1);
                if (closeIdx < 0) { searchPos = parenOpen + 1; continue; }
                std::string cond = afterParen.substr(0, closeIdx);
                size_t condStart = parenOpen + 1;
                size_t condEnd = condStart + closeIdx;
                // Check if condition contains == or != with vector types
                bool hasVecCompare = false;
                for (const char* op : {"==", "!="}) {
                    size_t opPos = cond.find(op);
                    if (opPos != std::string::npos) {
                        // Check if either side looks like a vector (swizzle or constructor)
                        std::string before = cond.substr(0, opPos);
                        std::string after = cond.substr(opPos + 2);
                        bool looksVector = false;
                        // Swizzle patterns
                        for (const char* sw : {".xy", ".xyz", ".xyzw", ".xz", ".yz", ".rg", ".rgb", ".rgba"})
                            if (before.find(sw) != std::string::npos || after.find(sw) != std::string::npos)
                                looksVector = true;
                        // Vector constructor patterns
                        for (const char* vc : {"float2(", "float3(", "float4(", "int2(", "int3(", "int4("})
                            if (before.find(vc) != std::string::npos || after.find(vc) != std::string::npos)
                                looksVector = true;
                        if (looksVector) { hasVecCompare = true; break; }
                    }
                }
                if (hasVecCompare && cond.find("all(") == std::string::npos) {
                    // Wrap condition with all()
                    result = result.substr(0, condStart) + "all(" + cond + ")" + result.substr(condEnd);
                    searchPos = condStart + cond.size() + 6; // past "all(...)"
                } else {
                    searchPos = condEnd + 1;
                }
            }
        }

        // Fix GLSL array parameter syntax: type[N] name → type name[N]
        // GLSL allows "in type[SIZE] paramName" but HLSL requires "in type paramName[SIZE]"
        {
            // Match patterns like "TypeName[EXPR] identifier" in function parameter contexts
            // We scan for IDENT[...] followed by space+IDENT, where the second IDENT is the param name
            size_t searchPos = 0;
            while (searchPos < result.size()) {
                size_t bracket = result.find('[', searchPos);
                if (bracket == std::string::npos) break;
                // Check what's before [ — should be an identifier (type name)
                size_t typeEnd = bracket;
                size_t typeStart = typeEnd;
                while (typeStart > 0 && (isalnum((unsigned char)result[typeStart-1]) || result[typeStart-1] == '_')) typeStart--;
                if (typeStart >= typeEnd) { searchPos = bracket + 1; continue; }
                std::string typeName = result.substr(typeStart, typeEnd - typeStart);
                // Find closing ]
                size_t closeBracket = result.find(']', bracket);
                if (closeBracket == std::string::npos) { searchPos = bracket + 1; continue; }
                std::string sizeExpr = result.substr(bracket, closeBracket - bracket + 1); // "[N]"
                // After ] should be whitespace then an identifier (parameter name)
                size_t afterBracket = closeBracket + 1;
                size_t nameStart = afterBracket;
                while (nameStart < result.size() && (result[nameStart] == ' ' || result[nameStart] == '\t')) nameStart++;
                size_t nameEnd = nameStart;
                while (nameEnd < result.size() && (isalnum((unsigned char)result[nameEnd]) || result[nameEnd] == '_')) nameEnd++;
                if (nameEnd <= nameStart) { searchPos = bracket + 1; continue; }
                // After paramName should be , or ) — confirms this is a function parameter
                size_t afterName = nameEnd;
                while (afterName < result.size() && result[afterName] == ' ') afterName++;
                if (afterName < result.size() && (result[afterName] == ',' || result[afterName] == ')')) {
                    // Check that typeName is not a vector type (those don't use array syntax in params)
                    // Rewrite: type[N] name → type name[N]
                    std::string paramName = result.substr(nameStart, nameEnd - nameStart);
                    std::string replacement = typeName + " " + paramName + sizeExpr;
                    result = result.substr(0, typeStart) + replacement + result.substr(nameEnd);
                    searchPos = typeStart + replacement.size();
                } else {
                    searchPos = bracket + 1;
                }
            }
        }

        // Fix vector l-value indexing: vec[dynamic_idx] = expr → _setComp(vec, idx, expr)
        // HLSL doesn't support writing to float2/3/4 components via dynamic index (X3500/X3550).
        // Replace with helper function calls that use static .x/.y/.z/.w member access.
        // Skip struct/array variables — only vector types need this workaround.
        {
            // Collect array variable names (declared as "type name[size]") to exclude from _setComp
            std::set<std::string> arrayVars;
            {
                size_t scanPos = 0;
                while (scanPos < result.size()) {
                    size_t br = result.find('[', scanPos);
                    if (br == std::string::npos) break;
                    // Walk back past spaces to find variable name
                    size_t ne = br;
                    while (ne > 0 && result[ne-1] == ' ') ne--;
                    size_t ns = ne;
                    while (ns > 0 && (isalnum((unsigned char)result[ns-1]) || result[ns-1] == '_')) ns--;
                    if (ne > ns) {
                        // Check if preceded by a type name (another identifier before this one)
                        size_t te = ns;
                        while (te > 0 && result[te-1] == ' ') te--;
                        size_t ts = te;
                        while (ts > 0 && (isalnum((unsigned char)result[ts-1]) || result[ts-1] == '_')) ts--;
                        if (te > ts) {
                            // Check that what's inside [] is a size (number or #define constant)
                            size_t cb = result.find(']', br);
                            if (cb != std::string::npos) {
                                std::string inside = result.substr(br + 1, cb - br - 1);
                                // Trim
                                size_t a = inside.find_first_not_of(" \t");
                                if (a != std::string::npos) inside = inside.substr(a);
                                size_t b = inside.find_last_not_of(" \t");
                                if (b != std::string::npos) inside = inside.substr(0, b + 1);
                                // If inside is a valid identifier or number, it's likely an array size
                                bool validSize = !inside.empty();
                                for (char c : inside) {
                                    if (!isalnum((unsigned char)c) && c != '_') { validSize = false; break; }
                                }
                                if (validSize) {
                                    arrayVars.insert(result.substr(ns, ne - ns));
                                }
                            }
                        }
                    }
                    scanPos = br + 1;
                }
            }

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

                        if (!isNumeric && arrayVars.find(varName) == arrayVars.end()) {
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

        // Fix scalar-to-vector broadcast: GLSL typeN(scalar) → HLSL ((typeN)(scalar))
        // GLSL allows vec3(1.0) to broadcast; HLSL requires explicit args or cast syntax.
        {
            const char* broadcastTypes[] = {
                "float2(", "float3(", "float4(",
                "int2(", "int3(", "int4(",
                "uint2(", "uint3(", "uint4(",
            };
            for (const char* prefix : broadcastTypes) {
                std::string typeName(prefix, strlen(prefix) - 1); // e.g. "float3"
                size_t searchFrom = 0;
                while (searchFrom < result.size()) {
                    size_t index = result.find(prefix, searchFrom);
                    if (index == std::string::npos) break;
                    // Don't match if preceded by identifier char (e.g. "myfloat3(")
                    if (index > 0) {
                        char prev = result[index - 1];
                        if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') || prev == '_' || (prev >= '0' && prev <= '9')) {
                            searchFrom = index + 1; continue;
                        }
                    }
                    size_t prefixLen = strlen(prefix);
                    std::string rest = result.substr(index + prefixLen);
                    int closingIdx = FindClosingBracket(rest, '(', ')', 1);
                    if (closingIdx <= 0) { searchFrom = index + prefixLen; continue; }
                    std::string argsLine = rest.substr(0, closingIdx);
                    // Count top-level commas — if 0, it's a single-arg broadcast
                    int topCommas = 0;
                    { int depth = 0;
                      for (char c : argsLine) {
                        if (c == '(') depth++;
                        else if (c == ')') depth--;
                        else if (c == ',' && depth == 0) topCommas++;
                      }
                    }
                    if (topCommas == 0 && !argsLine.empty()) {
                        // Single argument — convert to cast: ((typeN)(arg))
                        std::string trimmed = argsLine;
                        size_t s = trimmed.find_first_not_of(" \t");
                        size_t e = trimmed.find_last_not_of(" \t");
                        if (s != std::string::npos) trimmed = trimmed.substr(s, e - s + 1);
                        // Skip if arg is already a simple scalar literal (int/float) — those work as-is in some HLSL constructors
                        // Actually HLSL rejects single-arg vector constructors, so always convert
                        std::string replacement = "((" + typeName + ")(" + trimmed + "))";
                        result = result.substr(0, index) + replacement
                               + result.substr(index + prefixLen + closingIdx + 1);
                        searchFrom = index + replacement.size();
                        continue;
                    }
                    searchFrom = index + prefixLen + closingIdx;
                }
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

            // Fix GLSL struct constructors: StructName(a, b, c) → _init_StructName(a, b, c)
            // HLSL doesn't support struct constructors (X3037). Generate helper functions.
            {
                struct StructField { std::string type; std::string name; };
                struct StructDef { std::string name; std::vector<StructField> fields; };
                std::vector<StructDef> structDefs;

                // Parse struct definitions to get field types and names
                for (const auto& sname : structNames) {
                    std::string pat = "struct " + sname;
                    size_t spos = result.find(pat);
                    if (spos == std::string::npos) continue;
                    size_t braceOpen = result.find('{', spos);
                    if (braceOpen == std::string::npos) continue;
                    size_t braceClose = result.find('}', braceOpen);
                    if (braceClose == std::string::npos) continue;
                    std::string body = result.substr(braceOpen + 1, braceClose - braceOpen - 1);

                    StructDef sd;
                    sd.name = sname;
                    // Parse fields: "type name;" lines
                    std::istringstream fss(body);
                    std::string fline;
                    while (std::getline(fss, fline)) {
                        // Trim
                        size_t a = fline.find_first_not_of(" \t\r\n");
                        if (a == std::string::npos) continue;
                        fline = fline.substr(a);
                        size_t semi = fline.find(';');
                        if (semi == std::string::npos) continue;
                        fline = fline.substr(0, semi);
                        // Trim trailing
                        size_t b = fline.find_last_not_of(" \t");
                        if (b != std::string::npos) fline = fline.substr(0, b + 1);
                        // Split into type and name (last word is name)
                        size_t lastSpace = fline.rfind(' ');
                        if (lastSpace == std::string::npos) continue;
                        StructField sf;
                        sf.type = fline.substr(0, lastSpace);
                        // Trim type trailing space
                        size_t c = sf.type.find_last_not_of(" \t");
                        if (c != std::string::npos) sf.type = sf.type.substr(0, c + 1);
                        sf.name = fline.substr(lastSpace + 1);
                        if (!sf.type.empty() && !sf.name.empty())
                            sd.fields.push_back(sf);
                    }
                    if (!sd.fields.empty())
                        structDefs.push_back(sd);
                }

                // For each struct, check if constructor syntax is used and replace
                for (const auto& sd : structDefs) {
                    std::string ctorPat = sd.name + "(";
                    std::string initName = "_init_" + sd.name;
                    bool needsHelper = false;

                    // Replace StructName( with _init_StructName( but NOT "struct StructName" declarations
                    size_t pos = 0;
                    while ((pos = result.find(ctorPat, pos)) != std::string::npos) {
                        // Check it's not a declaration: "struct StructName" or type declaration
                        bool isDecl = false;
                        if (pos >= 7) {
                            std::string before = result.substr(pos - 7, 7);
                            if (before.find("struct") != std::string::npos) isDecl = true;
                        }
                        // Check it's not a function definition: "StructName funcname("
                        // A constructor call has StructName( immediately, possibly preceded by = or , or ( or space
                        if (!isDecl && pos > 0) {
                            char prev = result[pos - 1];
                            if (isalnum((unsigned char)prev) || prev == '_') {
                                // Part of another identifier
                                pos += ctorPat.size();
                                continue;
                            }
                        }
                        if (!isDecl) {
                            result.replace(pos, sd.name.size(), initName);
                            needsHelper = true;
                            pos += initName.size();
                        } else {
                            pos += ctorPat.size();
                        }
                    }

                    // Generate helper function
                    if (needsHelper) {
                        std::string helper = sd.name + " " + initName + "(";
                        for (size_t i = 0; i < sd.fields.size(); i++) {
                            if (i > 0) helper += ", ";
                            helper += sd.fields[i].type + " _f" + std::to_string(i);
                        }
                        helper += ") {\n  " + sd.name + " _s;\n";
                        for (size_t i = 0; i < sd.fields.size(); i++) {
                            helper += "  _s." + sd.fields[i].name + " = _f" + std::to_string(i) + ";\n";
                        }
                        helper += "  return _s;\n}\n\n";
                        // Insert before first use of struct (after struct definition)
                        size_t structEnd = result.find("};", result.find("struct " + sd.name));
                        if (structEnd != std::string::npos) {
                            structEnd += 2;
                            // Skip to next line
                            size_t nl = result.find('\n', structEnd);
                            if (nl != std::string::npos) structEnd = nl + 1;
                            result.insert(structEnd, helper);
                        }
                    }
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

        // Post-conversion validation: warn about incomplete blocks
        {
            int braceDepth = 0;
            int parenDepth = 0;
            int ifDepth = 0;
            bool inLineComment = false;
            bool inBlockComment = false;
            for (size_t ci = 0; ci < result.size(); ci++) {
                char c = result[ci];
                char cn = (ci + 1 < result.size()) ? result[ci + 1] : 0;
                if (inBlockComment) {
                    if (c == '*' && cn == '/') { inBlockComment = false; ci++; }
                    continue;
                }
                if (inLineComment) {
                    if (c == '\n') inLineComment = false;
                    continue;
                }
                if (c == '/' && cn == '/') { inLineComment = true; ci++; continue; }
                if (c == '/' && cn == '*') { inBlockComment = true; ci++; continue; }
                if (c == '{') braceDepth++;
                else if (c == '}') braceDepth--;
                else if (c == '(') parenDepth++;
                else if (c == ')') parenDepth--;
                else if (c == '#') {
                    // Check for #if / #endif
                    std::string rest = result.substr(ci, 20);
                    if (rest.substr(0, 3) == "#if") ifDepth++;
                    else if (rest.substr(0, 6) == "#endif") ifDepth--;
                }
            }
            if (braceDepth != 0)
                errors += "Warning: unmatched braces (depth " + std::to_string(braceDepth) + ")\r\n";
            if (parenDepth != 0)
                errors += "Warning: unmatched parentheses (depth " + std::to_string(parenDepth) + ")\r\n";
            if (ifDepth != 0)
                errors += "Warning: unmatched #if/#endif (depth " + std::to_string(ifDepth) + ")\r\n";
        }

    } catch (...) {
        errors += "Conversion error (exception)\r\n";
    }

    // Dump converter output for diagnostics (Verbose only)
    if (DLOG_DIAG_ENABLED()) {
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
        // Load channel inputs (if present) — supports both integer enum values
        // and string names like "self", "bufferA", "bufferB", "noiseLQ", etc.
        if (p.has(L"channels")) {
            const auto& ch = p[L"channels"];
            auto parseChan = [](const JsonValue& v, int def) -> int {
                if (v.isString()) {
                    std::wstring s = v.asString(L"");
                    if (s == L"self" || s == L"bufferA" || s == L"feedback") return CHAN_FEEDBACK;
                    if (s == L"bufferB") return CHAN_BUFFER_B;
                    if (s == L"noiseLQ") return CHAN_NOISE_LQ;
                    if (s == L"noiseMQ") return CHAN_NOISE_MQ;
                    if (s == L"noiseHQ") return CHAN_NOISE_HQ;
                    if (s == L"noiseVolLQ") return CHAN_NOISEVOL_LQ;
                    if (s == L"noiseVolHQ") return CHAN_NOISEVOL_HQ;
                    if (s == L"image") return CHAN_IMAGE_PREV;
                    if (s == L"audio") return CHAN_AUDIO;
                    if (s == L"random") return CHAN_RANDOM_TEX;
                }
                return v.asInt(def);
            };
            sp.channels[0] = parseChan(ch[L"ch0"], CHAN_NOISE_LQ);
            sp.channels[1] = parseChan(ch[L"ch1"], CHAN_NOISE_LQ);
            sp.channels[2] = parseChan(ch[L"ch2"], CHAN_NOISE_MQ);
            sp.channels[3] = parseChan(ch[L"ch3"], CHAN_NOISE_HQ);
            sp.channelsFromJSON = true;
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

    // Auto-detect channels for all non-Common passes.
    // For JSON-loaded passes, only high-confidence patterns run (3D texture, audio)
    // because JSON values can be wrong about texture types but right about buffer wiring.
    for (auto& p : m_passes) {
        if (p.name == L"Common") continue;
        AnalyzeChannels(p, p.channelsFromJSON);
    }

    m_nSelectedPass = 0;
    RebuildPassList();
    SyncPassToEditor();

    // Diagnostic: show detected channels after JSON load + AnalyzeChannels
    {
        std::wstring msg = L"Import project loaded. Channels:\r\n";
        const wchar_t* chanNames[] = {L"NoiseLQ",L"NoiseMQ",L"NoiseHQ",L"BufA/Self",L"NoiseVolLQ",L"NoiseVolHQ",L"ImgPrev",L"Audio",L"RandTex",L"BufB"};
        for (auto& p : m_passes) {
            if (p.name == L"Common") continue;
            msg += p.name + L": ";
            for (int i = 0; i < 4; i++) {
                int c = p.channels[i];
                msg += L"ch" + std::to_wstring(i) + L"=";
                msg += (c >= 0 && c <= 9) ? chanNames[c] : std::to_wstring(c);
                if (i < 3) msg += L" ";
            }
            msg += L"\r\n";
        }
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, msg.c_str());
    }
}

} // namespace mdrop
