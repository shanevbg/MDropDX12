// engine_custom_shaders_ui.cpp — Custom Shaders window
//
// Manages the shader override store (shader_overrides.h): named overrides, the
// ordered rules that pick one by preset tag, and applying an override to the
// running preset by hand.
//
// Shader TEXT is deliberately edited elsewhere. The .hlsl files live in
// resources/shaders and "Edit" hands them to the system's default editor, which
// is the whole reason they are files rather than strings inside a JSON blob:
// syntax highlighting, diffs, and version control.

#include "engine.h"
#include "shader_overrides.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "utility.h"
#include <shlwapi.h>
#include <commdlg.h>

namespace mdrop {

CustomShadersWindow::CustomShadersWindow(Engine* pEngine)
    : ToolWindow(pEngine, 470, 780) {}

// ---------------------------------------------------------------------------
// Open / Close wrappers on Engine
// ---------------------------------------------------------------------------
void Engine::OpenCustomShadersWindow()
{
    if (!m_pCustomShadersWindow)
        m_pCustomShadersWindow = new CustomShadersWindow(this);
    m_pCustomShadersWindow->Open();
}

void Engine::CloseCustomShadersWindow()
{
    if (m_pCustomShadersWindow) {
        m_pCustomShadersWindow->Close();
        delete m_pCustomShadersWindow;
        m_pCustomShadersWindow = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Rules are stored as tag lists; the window shows them as one editable string.
static std::wstring JoinTags(const std::vector<std::wstring>& tags)
{
    std::wstring out;
    for (size_t i = 0; i < tags.size(); i++) {
        if (i) out += L", ";
        out += tags[i];
    }
    return out;
}

static std::vector<std::wstring> SplitTags(const std::wstring& text)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L';') {
            while (!cur.empty() && cur.front() == L' ') cur.erase(0, 1);
            while (!cur.empty() && cur.back() == L' ') cur.pop_back();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    while (!cur.empty() && cur.front() == L' ') cur.erase(0, 1);
    while (!cur.empty() && cur.back() == L' ') cur.pop_back();
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool PickFile(HWND hParent, const wchar_t* filter, const wchar_t* title,
                     wchar_t* out, int outLen)
{
    out[0] = 0;
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hParent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = outLen;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) != 0;
}

// ---------------------------------------------------------------------------
// DoBuildControls
// ---------------------------------------------------------------------------
void CustomShadersWindow::DoBuildControls()
{
    auto base = BuildBaseControls();
    int y = base.y, lineH = base.lineH, gap = base.gap;
    int x = base.x, rw = base.rw;

    RECT rc;
    GetClientRect(m_hWnd, &rc);

    // Button rows divide the full width rather than using fixed widths: at a
    // larger HUD font the fixed widths ran past the right edge and clipped the
    // last button in a row.
    const int btnGap = 6;
    const int row    = lineH + gap;
    const int col2   = (rw - btnGap) / 2;
    const int col3   = (rw - btnGap * 2) / 3;
    const int col4   = (rw - btnGap * 3) / 4;

    // Master enable
    CreateCheck(m_hWnd, L"Enable shader overrides", IDC_MW_CSHADER_ENABLE,
                x, y, rw, lineH, m_hFont, ShaderOverrides().IsEnabled());
    y += row;

    // Status: what the running preset resolved to. Kept on screen so "why did
    // this preset get that shader" is never a guess.
    m_hStatus = CreateLabel(m_hWnd, L"", x, y, rw, lineH * 2, m_hFont);
    y += lineH * 2 + gap;

    // Everything below is laid out from the BOTTOM of the client area upwards,
    // and the shader list takes whatever is left in the middle. Laying out
    // downwards let the rules section run off the end of a window that does not
    // scroll, which is how it was first written and how it first shipped broken.
    const int bottom = rc.bottom - gap;

    // One row at the very bottom for the selected rule's VFX profile, then
    // the rule buttons above it. Still laid out upwards from the client
    // bottom, so nothing runs off the end of a window that does not scroll.
    const int vfxRowY    = bottom - lineH;
    const int ruleBtnY   = vfxRowY - gap - lineH;
    const int listY      = y + row;                    // after the "Shaders:" label

    // The shader list is the primary one, so the rules list takes the smaller
    // share of what is left.
    const int kMinListH = lineH * 4;
    const int kMinRuleH = lineH * 3;
    int ruleH = (bottom - y) / 4;
    if (ruleH < kMinRuleH) ruleH = kMinRuleH;

    // Clamping the shader list to a minimum is not enough on its own: when the
    // window is short the clamp pushes it back DOWN under the button row, and
    // the buttons then sit on top of the list. So if it does not fit, take the
    // space back from the rules list rather than overlapping.
    int ruleListY = 0, ruleLblY = 0, shaderBtnY = 0, listH = 0;
    for (int pass = 0; pass < 2; pass++) {
        ruleListY  = ruleBtnY - gap - ruleH;
        ruleLblY   = ruleListY - gap - lineH;
        shaderBtnY = ruleLblY - gap - row * 4;   // four rows of buttons
        listH      = shaderBtnY - gap - listY;
        if (listH >= kMinListH) break;
        const int need    = kMinListH - listH;
        const int canGive = ruleH - kMinRuleH;
        if (canGive <= 0) break;
        ruleH -= min(need, canGive);
    }
    // Still short only when the window is genuinely too small to hold either
    // list; keep it positive so the control is created rather than inverted.
    if (listH < lineH) listH = lineH;

    // ── Overrides ──
    CreateLabel(m_hWnd, L"Shaders:", x, y, rw, lineH, m_hFontBold);

    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        x, listY, rw, listH, m_hWnd,
        (HMENU)(INT_PTR)IDC_MW_CSHADER_LIST, GetModuleHandle(NULL), NULL);
    if (hList && m_hFont) SendMessage(hList, WM_SETFONT, (WPARAM)m_hFont, TRUE);

    int by = shaderBtnY;
    CreateBtn(m_hWnd, L"New...", IDC_MW_CSHADER_NEW, x, by, col3, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Rename...", IDC_MW_CSHADER_RENAME, x + col3 + btnGap, by, col3, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Delete", IDC_MW_CSHADER_DELETE, x + 2 * (col3 + btnGap), by, col3, lineH, m_hFont);
    by += row;

    CreateBtn(m_hWnd, L"Import .hlsl...", IDC_MW_CSHADER_IMPORTHLSL, x, by, col2, lineH, m_hFont);
    CreateBtn(m_hWnd, L"From preset...", IDC_MW_CSHADER_IMPORTMILK,
              x + col2 + btnGap, by, col2, lineH, m_hFont);
    by += row;

    CreateBtn(m_hWnd, L"Edit warp", IDC_MW_CSHADER_EDITWARP, x, by, col3, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Edit comp", IDC_MW_CSHADER_EDITCOMP, x + col3 + btnGap, by, col3, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Reload", IDC_MW_CSHADER_RELOAD, x + 2 * (col3 + btnGap), by, col3, lineH, m_hFont);
    by += row;

    CreateBtn(m_hWnd, L"Apply to preset", IDC_MW_CSHADER_APPLY, x, by, col2, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Revert", IDC_MW_CSHADER_REVERT, x + col2 + btnGap, by, col2, lineH, m_hFont);

    // ── Rules ──
    CreateLabel(m_hWnd, L"Rules (first match wins):", x, ruleLblY, rw, lineH, m_hFontBold);

    HWND hRules = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        x, ruleListY, rw, ruleH, m_hWnd,
        (HMENU)(INT_PTR)IDC_MW_CSHADER_RULES, GetModuleHandle(NULL), NULL);
    if (hRules && m_hFont) SendMessage(hRules, WM_SETFONT, (WPARAM)m_hFont, TRUE);

    int bx = x;
    CreateBtn(m_hWnd, L"Add rule...", IDC_MW_CSHADER_RULEADD, bx, ruleBtnY, col4, lineH, m_hFont);
    bx += col4 + btnGap;
    CreateBtn(m_hWnd, L"Remove", IDC_MW_CSHADER_RULEDEL, bx, ruleBtnY, col4, lineH, m_hFont);
    bx += col4 + btnGap;
    CreateBtn(m_hWnd, L"Up", IDC_MW_CSHADER_RULEUP, bx, ruleBtnY, col4, lineH, m_hFont);
    bx += col4 + btnGap;
    CreateBtn(m_hWnd, L"Down", IDC_MW_CSHADER_RULEDOWN, bx, ruleBtnY, col4, lineH, m_hFont);

    // ── The selected rule's VFX profile ──
    //
    // A rule may carry a profile, an override, or both; one with only a
    // profile changes video effects and leaves the preset's own shaders alone.
    // A third of the width, and a label short enough to fit in it at a larger
    // HUD font -- "VFX profile for rule:" clipped to "VFX profile for".
    const int lblW = rw / 3;
    CreateLabel(m_hWnd, L"VFX profile:", x, vfxRowY, lblW, lineH, m_hFont);
    HWND hVfx = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        x + lblW + btnGap, vfxRowY, rw - lblW - btnGap, lineH * 8, m_hWnd,
        (HMENU)(INT_PTR)IDC_MW_CSHADER_RULEVFX, GetModuleHandle(NULL), NULL);
    if (hVfx && m_hFont) SendMessage(hVfx, WM_SETFONT, (WPARAM)m_hFont, TRUE);

    RefreshAll();
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------
void CustomShadersWindow::RefreshAll()
{
    if (!m_hWnd) return;
    RefreshOverrideList();
    RefreshRuleList();
    RefreshRuleVFXCombo();
    RefreshStatus();
}

// The profile list, plus a "(none)" entry at index 0 meaning the rule says
// nothing about video effects. Kept in step with the selected rule.
void CustomShadersWindow::RefreshRuleVFXCombo()
{
    HWND hVfx = GetDlgItem(m_hWnd, IDC_MW_CSHADER_RULEVFX);
    if (!hVfx) return;

    SendMessage(hVfx, CB_RESETCONTENT, 0, 0);
    m_vfxNames.clear();

    SendMessageW(hVfx, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    m_vfxNames.push_back(L"");
    std::vector<std::wstring> profiles;
    m_pEngine->m_vfxProfiles.Names(profiles);   // fills an out-param, returns void
    for (const auto& n : profiles) {
        SendMessageW(hVfx, CB_ADDSTRING, 0, (LPARAM)n.c_str());
        m_vfxNames.push_back(n);
    }

    const int sel = SelectedRule();
    const auto& rules = ShaderOverrides().Rules();
    int idx = 0;
    if (sel >= 0 && sel < (int)rules.size()) {
        for (size_t i = 0; i < m_vfxNames.size(); i++)
            if (_wcsicmp(m_vfxNames[i].c_str(), rules[sel].vfxProfile.c_str()) == 0) {
                idx = (int)i;
                break;
            }
    }
    SendMessage(hVfx, CB_SETCURSEL, idx, 0);
    // Nothing to point at with no rule selected.
    EnableWindow(hVfx, sel >= 0 && sel < (int)rules.size());
}

void CustomShadersWindow::RefreshOverrideList()
{
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_CSHADER_LIST);
    if (!hList) return;

    const std::wstring keep = SelectedOverride();
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    m_overrideNames.clear();

    int selIdx = 0;
    for (const auto& o : ShaderOverrides().Overrides()) {
        // Say what each override actually carries: an entry with neither slot
        // filled looks identical to a working one otherwise.
        std::wstring label = o.name;
        label += L"   [";
        label += o.warpText.empty() ? L"-" : L"warp";
        label += L"/";
        label += o.compText.empty() ? L"-" : L"comp";
        label += L"]";
        if (!o.lastError.empty()) label += L"  (!)";

        int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)label.c_str());
        m_overrideNames.push_back(o.name);
        if (_wcsicmp(o.name.c_str(), keep.c_str()) == 0) selIdx = idx;
    }
    if (!m_overrideNames.empty())
        SendMessage(hList, LB_SETCURSEL, selIdx, 0);
}

void CustomShadersWindow::RefreshRuleList()
{
    HWND hRules = GetDlgItem(m_hWnd, IDC_MW_CSHADER_RULES);
    if (!hRules) return;

    const int keep = SelectedRule();
    SendMessage(hRules, LB_RESETCONTENT, 0, 0);

    const auto& rules = ShaderOverrides().Rules();
    for (size_t i = 0; i < rules.size(); i++) {
        std::wstring label = std::to_wstring(i + 1);
        label += L". ";
        label += JoinTags(rules[i].tags);
        label += L"  ->  ";
        label += rules[i].overrideName.empty() ? L"(no shader)"
                                              : rules[i].overrideName;
        if (!rules[i].vfxProfile.empty()) {
            label += L" + vfx:";
            label += rules[i].vfxProfile;
        }
        if (!rules[i].enabled) label += L"   (off)";
        SendMessageW(hRules, LB_ADDSTRING, 0, (LPARAM)label.c_str());
    }
    if (keep >= 0 && keep < (int)rules.size())
        SendMessage(hRules, LB_SETCURSEL, keep, 0);
}

void CustomShadersWindow::RefreshStatus()
{
    if (!m_hStatus) return;

    const auto& active = m_pEngine->m_activeOverride;
    std::wstring text = L"Current preset: ";

    if (m_pEngine->m_bShadertoyMode) {
        // .milk3 presets render through RenderFrameShadertoy and never run the
        // warp or comp pass, so no rule can apply to them.
        text += L"Shadertoy (.milk3) — overrides do not apply";
    } else if (!active.IsActive()) {
        text += ShaderOverrides().IsEnabled() ? L"no rule matched"
                                              : L"overrides disabled";
    } else {
        text += active.name;
        if (active.fromRule) {
            text += L"  (tag: ";
            text += active.matchedTag;
            text += L")";
        } else {
            text += L"  (applied by hand)";
        }
        if (active.warpFailed || active.compFailed) {
            text += L"\n";
            text += L"FAILED to compile — the preset's own shader is rendering";
        }
    }
    SetWindowTextW(m_hStatus, text.c_str());
}

std::wstring CustomShadersWindow::SelectedOverride()
{
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_CSHADER_LIST);
    if (!hList) return std::wstring();
    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)m_overrideNames.size()) return std::wstring();
    return m_overrideNames[sel];
}

int CustomShadersWindow::SelectedRule()
{
    HWND hRules = GetDlgItem(m_hWnd, IDC_MW_CSHADER_RULES);
    if (!hRules) return -1;
    int sel = (int)SendMessage(hRules, LB_GETCURSEL, 0, 0);
    return sel;
}

void CustomShadersWindow::EditShaderFile(bool warp)
{
    const std::wstring name = SelectedOverride();
    if (name.empty()) return;

    std::wstring path = ShaderOverrides().FilePath(name, warp);
    if (path.empty()) {
        MessageBoxW(m_hWnd,
            warp ? L"This override has no warp shader.\n\n"
                   L"Use Import .hlsl... to give it one."
                 : L"This override has no comp shader.\n\n"
                   L"Use Import .hlsl... to give it one.",
            L"Custom Shaders", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // Hand it to whatever the user edits .hlsl with. Reload picks up the edit.
    ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// DoCommand
// ---------------------------------------------------------------------------
LRESULT CustomShadersWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam)
{
    Engine* p = m_pEngine;
    ShaderOverrideStore& store = ShaderOverrides();

    switch (id) {

    // Checkboxes are toggled by the base class before this runs, so read the
    // control rather than inverting the stored value (IsDlgButtonChecked does
    // not work here -- every control is BS_OWNERDRAW).
    case IDC_MW_CSHADER_ENABLE: {
        const bool on = IsChecked(IDC_MW_CSHADER_ENABLE);
        store.SetEnabled(on);
        store.Save();
        p->ResolveShaderOverrideForPreset(p->m_pState);
        p->RequestShaderRecompile();
        RefreshStatus();
        return 0;
    }

    case IDC_MW_CSHADER_LIST:
        if (code == LBN_SELCHANGE) RefreshStatus();
        return 0;

    case IDC_MW_CSHADER_RULEVFX: {
        if (code != CBN_SELCHANGE) return 0;
        int sel = SelectedRule();
        auto& rules = store.Rules();
        if (sel < 0 || sel >= (int)rules.size()) return 0;
        int idx = (int)SendMessage(GetDlgItem(m_hWnd, IDC_MW_CSHADER_RULEVFX),
                                   CB_GETCURSEL, 0, 0);
        if (idx < 0 || idx >= (int)m_vfxNames.size()) return 0;

        rules[sel].vfxProfile = m_vfxNames[idx];
        store.Save();
        // The running preset may resolve to this rule, so re-resolve rather
        // than waiting for the next preset change to show the effect.
        p->ResolveShaderOverrideForPreset(p->m_pState);
        RefreshRuleList();
        SendMessage(GetDlgItem(m_hWnd, IDC_MW_CSHADER_RULES), LB_SETCURSEL, sel, 0);
        RefreshStatus();
        return 0;
    }

    case IDC_MW_CSHADER_NEW: {
        std::wstring name;
        if (!PromptForName(p, m_hWnd, L"New Shader Override",
                           L"Name:", name, 64, store.Names()))
            return 0;
        if (!store.Add(name)) {
            MessageBoxW(m_hWnd, L"That name is already taken.",
                        L"Custom Shaders", MB_OK | MB_ICONWARNING);
            return 0;
        }
        RefreshAll();
        return 0;
    }

    case IDC_MW_CSHADER_IMPORTHLSL: {
        std::wstring name = SelectedOverride();
        if (name.empty()) {
            if (!PromptForName(p, m_hWnd, L"Import Shader",
                               L"Name for this override:", name, 64, store.Names()))
                return 0;
        }
        wchar_t path[MAX_PATH];
        if (!PickFile(m_hWnd, L"Shader files\0*.hlsl;*.fx;*.txt\0All files\0*.*\0\0",
                      L"Import shader", path, MAX_PATH))
            return 0;

        // Which slot it fills cannot be inferred from the file, so ask.
        const int which = MessageBoxW(m_hWnd,
            L"Use this as the WARP shader?\n\n"
            L"Yes = warp   No = comp",
            L"Import Shader", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (which == IDCANCEL) return 0;

        if (!store.AddFromFile(name, path, which == IDYES)) {
            MessageBoxW(m_hWnd, L"Could not read that file.",
                        L"Custom Shaders", MB_OK | MB_ICONWARNING);
            return 0;
        }
        RefreshAll();
        return 0;
    }

    case IDC_MW_CSHADER_IMPORTMILK: {
        wchar_t path[MAX_PATH];
        if (!PickFile(m_hWnd, L"Presets\0*.milk;*.milk2\0All files\0*.*\0\0",
                      L"Take the shaders from a preset", path, MAX_PATH))
            return 0;

        // Default the name to the preset's, which is nearly always what is wanted.
        std::wstring name = PathFindFileNameW(path);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name = name.substr(0, dot);
        if (!PromptForName(p, m_hWnd, L"Import From Preset",
                           L"Name for this override:", name, 64, store.Names()))
            return 0;

        if (!store.AddFromMilk(name, path)) {
            MessageBoxW(m_hWnd,
                L"That preset has no warp or comp shader to take.\n\n"
                L"Presets older than MilkDrop 2 have no shader text.",
                L"Custom Shaders", MB_OK | MB_ICONWARNING);
            return 0;
        }
        RefreshAll();
        return 0;
    }

    case IDC_MW_CSHADER_RENAME: {
        std::wstring name = SelectedOverride();
        if (name.empty()) return 0;
        std::wstring to = name;
        if (!PromptForName(p, m_hWnd, L"Rename Override", L"New name:", to, 64,
                           store.Names()))
            return 0;
        if (!store.Rename(name, to))
            MessageBoxW(m_hWnd, L"That name is already taken.",
                        L"Custom Shaders", MB_OK | MB_ICONWARNING);
        RefreshAll();
        return 0;
    }

    case IDC_MW_CSHADER_DELETE: {
        std::wstring name = SelectedOverride();
        if (name.empty()) return 0;

        wchar_t msg[512];
        swprintf_s(msg,
            L"Delete the override \"%s\"?\n\n"
            L"Its shader files are deleted too, and any rule pointing at it "
            L"is removed.", name.c_str());
        if (MessageBoxW(m_hWnd, msg, L"Custom Shaders",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return 0;

        store.Remove(name, true);
        if (p->m_activeOverride.name == name) p->RevertOverrideOnCurrentPreset();
        RefreshAll();
        return 0;
    }

    case IDC_MW_CSHADER_EDITWARP: EditShaderFile(true);  return 0;
    case IDC_MW_CSHADER_EDITCOMP: EditShaderFile(false); return 0;

    case IDC_MW_CSHADER_RELOAD: {
        // Re-read everything from disk: the point of editing shaders in an
        // external editor is not having to restart to see the change.
        store.Load(p->m_szMilkdrop2Path);
        p->ResolveShaderOverrideForPreset(p->m_pState);
        p->RequestShaderRecompile();
        RefreshAll();
        return 0;
    }

    case IDC_MW_CSHADER_APPLY: {
        std::wstring name = SelectedOverride();
        if (name.empty()) return 0;
        if (!p->ApplyOverrideToCurrentPreset(name)) {
            MessageBoxW(m_hWnd,
                L"That override cannot be applied to the running preset.\n\n"
                L"Shadertoy (.milk3) presets have no warp or comp pass.",
                L"Custom Shaders", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        RefreshStatus();
        return 0;
    }

    case IDC_MW_CSHADER_REVERT:
        p->RevertOverrideOnCurrentPreset();
        RefreshStatus();
        return 0;

    case IDC_MW_CSHADER_RULES:
        // Selecting a rule re-points the VFX profile combo below at it.
        if (code == LBN_SELCHANGE) RefreshRuleVFXCombo();
        return 0;

    case IDC_MW_CSHADER_RULEADD: {
        std::wstring name = SelectedOverride();
        if (name.empty()) {
            MessageBoxW(m_hWnd, L"Select a shader first — a rule points at one.",
                        L"Custom Shaders", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        // Offer the tags already in use, so a rule is normally a pick rather
        // than a spelling test.
        std::vector<std::wstring> allTags;
        p->CollectAllTags(allTags);
        std::wstring tags;
        if (!PromptForName(p, m_hWnd, L"Add Rule",
                           L"Tags (comma separated) — a preset matches on ANY of them:",
                           tags, 256, allTags))
            return 0;

        ShaderRule rule;
        rule.tags = SplitTags(tags);
        rule.overrideName = name;
        rule.enabled = true;
        if (rule.tags.empty()) return 0;

        store.Rules().push_back(rule);
        store.Save();
        p->ResolveShaderOverrideForPreset(p->m_pState);
        p->RequestShaderRecompile();
        RefreshAll();
        return 0;
    }

    case IDC_MW_CSHADER_RULEDEL: {
        int sel = SelectedRule();
        auto& rules = store.Rules();
        if (sel < 0 || sel >= (int)rules.size()) return 0;
        rules.erase(rules.begin() + sel);
        store.Save();
        p->ResolveShaderOverrideForPreset(p->m_pState);
        p->RequestShaderRecompile();
        RefreshAll();
        return 0;
    }

    case IDC_MW_CSHADER_RULEUP:
    case IDC_MW_CSHADER_RULEDOWN: {
        int sel = SelectedRule();
        auto& rules = store.Rules();
        const int to = (id == IDC_MW_CSHADER_RULEUP) ? sel - 1 : sel + 1;
        if (sel < 0 || sel >= (int)rules.size() || to < 0 || to >= (int)rules.size())
            return 0;

        std::swap(rules[sel], rules[to]);
        store.Save();
        // Order is priority, so moving a rule can change what the running
        // preset resolves to.
        p->ResolveShaderOverrideForPreset(p->m_pState);
        p->RequestShaderRecompile();
        RefreshRuleList();
        SendMessage(GetDlgItem(m_hWnd, IDC_MW_CSHADER_RULES), LB_SETCURSEL, to, 0);
        RefreshStatus();
        return 0;
    }
    }

    return ToolWindow::DoCommand(hWnd, id, code, lParam);
}

}  // namespace mdrop
