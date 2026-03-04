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

// ─── Constructor ────────────────────────────────────────────────────────

ShaderImportWindow::ShaderImportWindow(Engine* pEngine)
    : ToolWindow(pEngine, 700, 800)
{
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
    int pad = 8;

    // Calculate proportional edit heights from available space
    int fixedH = (lineH + gap) * 3  // 3 label rows
               + (btnH + gap)       // Convert button row
               + (btnH + gap)       // bottom buttons row
               + pad;
    int editH = clientH - y - fixedH;
    if (editH < 120) editH = 120;
    int glslH = editH * 38 / 100;
    int hlslH = editH * 38 / 100;
    int errH  = editH - glslH - hlslH;
    if (errH < 40) errH = 40;

    // "GLSL Input:" label + Paste / Clear buttons
    TrackControl(CreateWindowExW(0, L"STATIC", L"GLSL Input:", WS_CHILD | WS_VISIBLE,
        x, y, 100, lineH, hw, NULL, NULL, NULL));
    TrackControl(CreateBtn(hw, L"Paste", IDC_MW_SHIMPORT_PASTE,
        x + rw - btnW * 2 - 8, y - 2, btnW, btnH, hFont));
    TrackControl(CreateBtn(hw, L"Clear", IDC_MW_SHIMPORT_CLEAR,
        x + rw - btnW, y - 2, btnW, btnH, hFont));
    y += lineH + gap;

    // GLSL multiline edit
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
        WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
        x, y, rw, glslH, hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_GLSL_EDIT, NULL, NULL));
    y += glslH + gap;

    // Convert button + pass selector
    TrackControl(CreateBtn(hw, L"Convert >>", IDC_MW_SHIMPORT_CONVERT,
        x, y, btnW + 20, btnH, hFont));
    {
        int comboW = MulDiv(120, lineH, 26);
        TrackControl(CreateWindowExW(0, L"STATIC", L"Target:", WS_CHILD | WS_VISIBLE,
            x + btnW + 30, y + 2, MulDiv(50, lineH, 26), lineH, hw, NULL, NULL, NULL));
        HWND hCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x + btnW + 30 + MulDiv(52, lineH, 26), y, comboW, lineH * 4,
            hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_PASS_COMBO, NULL, NULL);
        SendMessageW(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Image (comp)");
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Buffer A");
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
        TrackControl(hCombo);
    }
    y += btnH + gap;

    // "HLSL Output:" label + Copy button
    TrackControl(CreateWindowExW(0, L"STATIC", L"HLSL Output:", WS_CHILD | WS_VISIBLE,
        x, y, 100, lineH, hw, NULL, NULL, NULL));
    TrackControl(CreateBtn(hw, L"Copy", IDC_MW_SHIMPORT_COPY,
        x + rw - btnW, y - 2, btnW, btnH, hFont));
    y += lineH + gap;

    // HLSL multiline edit
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
        WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
        x, y, rw, hlslH, hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_HLSL_EDIT, NULL, NULL));
    y += hlslH + gap;

    // "Errors:" label
    TrackControl(CreateWindowExW(0, L"STATIC", L"Errors:", WS_CHILD | WS_VISIBLE,
        x, y, 100, lineH, hw, NULL, NULL, NULL));
    y += lineH + gap;

    // Error multiline edit (read-only)
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
        WS_VSCROLL,
        x, y, rw, errH, hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_ERROR_EDIT, NULL, NULL));
    y += errH + gap;

    // Bottom buttons: Apply, Save
    TrackControl(CreateBtn(hw, L"Apply", IDC_MW_SHIMPORT_APPLY,
        x, y, btnW, btnH, hFont));
    int saveW = MulDiv(140, lineH, 26);
    TrackControl(CreateBtn(hw, L"Save as .milk...", IDC_MW_SHIMPORT_SAVE,
        x + rw - saveW, y, saveW, btnH, hFont));

    // Set monospace font on code edits
    HFONT hMono = CreateFontW(-MulDiv(10, GetDpiForWindow(hw), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (hMono) {
        SendDlgItemMessageW(hw, IDC_MW_SHIMPORT_GLSL_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);
        SendDlgItemMessageW(hw, IDC_MW_SHIMPORT_HLSL_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);
        SendDlgItemMessageW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);
    }
}

// ─── Layout ─────────────────────────────────────────────────────────────

void ShaderImportWindow::OnResize() {
    RebuildFonts();
}

void ShaderImportWindow::LayoutControls() {
    // All layout is handled by DoBuildControls; OnResize calls RebuildFonts.
}

// ─── Command Handler ────────────────────────────────────────────────────

LRESULT ShaderImportWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    Engine* p = m_pEngine;

    if (code == BN_CLICKED) {
        switch (id) {
        case IDC_MW_SHIMPORT_PASTE: {
            // Paste from clipboard into GLSL edit, then auto-convert
            if (OpenClipboard(hWnd)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* pText = (wchar_t*)GlobalLock(hData);
                    if (pText) {
                        SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_GLSL_EDIT, pText);
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            ConvertGLSLtoHLSL();
            return 0;
        }
        case IDC_MW_SHIMPORT_CLEAR:
            SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_GLSL_EDIT, L"");
            SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_HLSL_EDIT, L"");
            SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_ERROR_EDIT, L"");
            return 0;

        case IDC_MW_SHIMPORT_CONVERT:
            ConvertGLSLtoHLSL();
            return 0;

        case IDC_MW_SHIMPORT_COPY: {
            // Copy HLSL to clipboard
            int len = GetWindowTextLengthW(GetDlgItem(hWnd, IDC_MW_SHIMPORT_HLSL_EDIT));
            if (len > 0) {
                std::wstring text(len + 1, L'\0');
                GetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_HLSL_EDIT, text.data(), len + 1);
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
        case IDC_MW_SHIMPORT_APPLY:
            ApplyShader();
            return 0;

        case IDC_MW_SHIMPORT_SAVE:
            SaveAsPreset();
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
        bool bufAOK = !m_bufferAHlsl.empty() ? (p->m_shaders.bufferA.bytecodeBlob != NULL) : true;

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
    HWND hCombo = GetDlgItem(m_hWnd, IDC_MW_SHIMPORT_PASS_COMBO);
    return hCombo ? (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0) : 0;
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

    // Get HLSL text
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SHIMPORT_HLSL_EDIT));
    if (len <= 0) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"No HLSL text to apply.");
        return;
    }
    std::wstring hlslW(len + 1, L'\0');
    GetDlgItemTextW(hw, IDC_MW_SHIMPORT_HLSL_EDIT, hlslW.data(), len + 1);
    std::string hlsl = WideHlslToNarrow(hlslW, len);

    int pass = GetSelectedPass();
    if (pass == 1) {
        // Buffer A — store locally and write into state
        m_bufferAHlsl = hlsl;
        strncpy_s(p->m_pState->m_szBufferAShadersText, MAX_BIGSTRING_LEN, hlsl.c_str(), _TRUNCATE);
        p->m_pState->m_nBufferAPSVersion = MD2_PS_5_0;
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT,
            L"Buffer A stored. Switch to Image, convert & apply to see the full effect.");
        return;
    }

    // Image (comp) — also inject stored Buffer A if present
    strncpy_s(p->m_pState->m_szCompShadersText, MAX_BIGSTRING_LEN, hlsl.c_str(), _TRUNCATE);
    p->m_pState->m_nCompPSVersion = MD2_PS_5_0;
    p->m_pState->m_nMaxPSVersion = max(p->m_pState->m_nWarpPSVersion, (int)MD2_PS_5_0);

    if (!m_bufferAHlsl.empty()) {
        strncpy_s(p->m_pState->m_szBufferAShadersText, MAX_BIGSTRING_LEN, m_bufferAHlsl.c_str(), _TRUNCATE);
        p->m_pState->m_nBufferAPSVersion = MD2_PS_5_0;
    }

    // Activate Shadertoy pipeline (skip warp/blur/shapes)
    p->m_bShadertoyMode = true;
    p->m_nShadertoyStartFrame = p->GetFrame();

    // Signal pending and enqueue recompile
    p->m_nRecompileResult.store(1);  // 1=pending
    p->EnqueueRenderCmd(RenderCmd::RecompileCompShader);

    // Poll for result every 200ms (render thread signals completion)
    SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Compiling...");
    SetTimer(hw, 1, 200, NULL);
}

// ─── Save as .milk3 (JSON Shadertoy format) ─────────────────────────────

void ShaderImportWindow::SaveAsPreset() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    // Get HLSL text
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SHIMPORT_HLSL_EDIT));
    if (len <= 0) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"No HLSL text to save.");
        return;
    }
    std::wstring hlslW(len + 1, L'\0');
    GetDlgItemTextW(hw, IDC_MW_SHIMPORT_HLSL_EDIT, hlslW.data(), len + 1);

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

    // Convert HLSL text to narrow
    std::string hlsl = WideHlslToNarrow(hlslW, len);

    // If currently on Buffer A, store it first before saving
    int pass = GetSelectedPass();
    if (pass == 1)
        m_bufferAHlsl = hlsl;

    // Determine which extension the user chose
    std::wstring ext = filePath;
    bool isMilk3 = (ext.size() >= 6 && _wcsicmp(ext.c_str() + ext.size() - 6, L".milk3") == 0);

    if (isMilk3) {
        // Save as .milk3 JSON
        std::string imageHlsl, bufferAHlsl;
        if (pass == 0) {
            imageHlsl = hlsl;
        } else {
            // Buffer A pass — use current state's comp text for Image
            if (p->m_pState->m_nCompPSVersion > 0)
                imageHlsl = p->m_pState->m_szCompShadersText;
        }
        if (!m_bufferAHlsl.empty())
            bufferAHlsl = m_bufferAHlsl;

        // Build JSON
        JsonWriter w;
        w.BeginObject();
        w.Int(L"version", 1);
        w.Bool(L"shadertoy", true);

        // Extract a name from the filename
        std::wstring fname = std::filesystem::path(filePath).stem().wstring();
        w.String(L"name", fname.c_str());

        // Write shader text as wide strings (JsonWriter handles escaping)
        if (!bufferAHlsl.empty()) {
            std::wstring wBufA(bufferAHlsl.begin(), bufferAHlsl.end());
            w.String(L"bufferA", wBufA.c_str());
        }
        {
            std::wstring wImage(imageHlsl.begin(), imageHlsl.end());
            w.String(L"image", wImage.c_str());
        }

        // Channel mappings
        w.BeginObject(L"channels");
        if (!bufferAHlsl.empty()) {
            w.BeginObject(L"bufferA");
            w.String(L"iChannel0", L"self");
            w.EndObject();
        }
        w.BeginObject(L"image");
        w.String(L"iChannel0", bufferAHlsl.empty() ? L"self" : L"bufferA");
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

        if (pass == 0) {
            strncpy_s(tempState->m_szCompShadersText, MAX_BIGSTRING_LEN, hlsl.c_str(), _TRUNCATE);
        } else {
            if (p->m_pState->m_nCompPSVersion > 0)
                strncpy_s(tempState->m_szCompShadersText, MAX_BIGSTRING_LEN, p->m_pState->m_szCompShadersText, _TRUNCATE);
        }

        if (!m_bufferAHlsl.empty()) {
            strncpy_s(tempState->m_szBufferAShadersText, MAX_BIGSTRING_LEN, m_bufferAHlsl.c_str(), _TRUNCATE);
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

void ShaderImportWindow::ConvertGLSLtoHLSL() {
    HWND hw = m_hWnd;

    // Get GLSL text
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SHIMPORT_GLSL_EDIT));
    if (len <= 0) return;

    std::wstring glslW(len + 1, L'\0');
    GetDlgItemTextW(hw, IDC_MW_SHIMPORT_GLSL_EDIT, glslW.data(), len + 1);

    // Convert wide → narrow for processing
    std::string inp;
    inp.reserve(len);
    for (int i = 0; i < len; i++) {
        wchar_t ch = glslW[i];
        if (ch < 128) inp += (char)ch;
        else inp += '?';
    }

    // Strip // comment lines (browser copy-paste can line-wrap comments,
    // splitting words across lines and creating invalid identifiers like 'his' from 'this')
    {
        std::string cleaned;
        cleaned.reserve(inp.size());
        size_t i = 0;
        while (i < inp.size()) {
            if (i + 1 < inp.size() && inp[i] == '/' && inp[i + 1] == '/') {
                // Skip to end of line
                while (i < inp.size() && inp[i] != '\n') i++;
            } else {
                cleaned += inp[i];
                i++;
            }
        }
        inp = std::move(cleaned);
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

        replaceAll(inp, "fract (", "fract(");
        replaceAll(inp, "mod (", "mod(");
        replaceAll(inp, "mix (", "mix(");
        replaceAll(inp, "fract(", "frac(");
        replaceAll(inp, "mod(", "mod_conv(");
        replaceAll(inp, "mix(", "lerp(");
        inp = ReplaceVarName("time", "time_conv", inp);
        replaceAll(inp, "refrac(", "refract("); // undo damage to refract

        // Rename variables that collide with MilkDrop audio macros (#define mid _c3.y etc.)
        // These are common GLSL variable names (e.g. "mid" = midpoint) that the preprocessor
        // would otherwise expand into constant buffer swizzles, breaking declarations/calls.
        inp = WholeWordReplace(inp, "mid", "_st_mid");
        inp = WholeWordReplace(inp, "bass", "_st_bass");
        inp = WholeWordReplace(inp, "treb", "_st_treb");
        inp = WholeWordReplace(inp, "vol", "_st_vol");
        replaceAll(inp, "iTimeDelta", "xTimeDelta"); // protect from iTime replace
        replaceAll(inp, "iTime", "time");
        // iResolution: Shadertoy vec3(width, height, pixelAspect=1.0)
        // texsize = float4(w, h, 1/w, 1/h), so texsize.z would be wrong.
        // Inline-expand so iResolution.z → float3(...).z → 1.0
        replaceAll(inp, "iResolution", "float3(texsize.x, texsize.y, 1.0)");
        replaceAll(inp, "iFrame", "frame");
        replaceAll(inp, "iMouse", "mouse");
        // iChannel samplers — inline replacement (not #define) to avoid preprocessor issues
        replaceAll(inp, "iChannel0", "sampler_feedback");
        replaceAll(inp, "iChannel1", "sampler_noise_lq");
        replaceAll(inp, "iChannel2", "sampler_noise_mq");
        replaceAll(inp, "iChannel3", "sampler_noise_hq");
        replaceAll(inp, "texture(", "tex2D(");
        replaceAll(inp, "textureLod(", "tex2Dlod_conv(");
        replaceAll(inp, "texelFetch(", "texelFetch_conv(");
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
                        clean << trimmed << "\n";
                    }
                    prevLine = trimmed;
                }
                inpHeader = clean.str();
            }
        }

        // Rename builtins in header to avoid conflicts
        inpHeader = ReplaceVarName("uv", "uv_conv", inpHeader);
        inpHeader = ReplaceVarName("ang", "ang_conv", inpHeader);

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

        // Add texelFetch helper if needed (GLSL integer-coordinate texture fetch)
        if (inp.find("texelFetch_conv(") != std::string::npos) {
            std::string helper =
                "// CONV: texelFetch → tex2Dlod with integer-to-UV coordinate conversion\n"
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

        // No UV manipulation — Shadertoy shaders compute their own UVs
        // from fragCoord / iResolution. The MilkDrop 'uv' parameter is only
        // used to initialize fragCoord above.

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

                if (line.find("float2 uv =") != std::string::npos) {
                    currentLine = "// " + line;
                } else if (line.find("iDate") != std::string::npos) {
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

    // Convert result to wide and set in HLSL edit
    std::wstring resultW;
    resultW.reserve(result.size());
    for (char c : result) resultW += (wchar_t)(unsigned char)c;

    SetDlgItemTextW(hw, IDC_MW_SHIMPORT_HLSL_EDIT, resultW.c_str());

    // Show errors
    std::wstring errW;
    if (errors.empty()) errW = L"Conversion complete.";
    else for (char c : errors) errW += (wchar_t)(unsigned char)c;
    SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, errW.c_str());
}

} // namespace mdrop
