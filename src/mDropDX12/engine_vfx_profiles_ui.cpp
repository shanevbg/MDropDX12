// engine_vfx_profiles_ui.cpp — VFX Profile Picker window
//
// Listbox-based instant profile switching with save/delete/startup controls.

#include "engine.h"
#include "tool_window.h"
#include "engine_helpers.h"
#include "json_utils.h"
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace mdrop {

VFXProfileWindow::VFXProfileWindow(Engine* pEngine)
    : ToolWindow(pEngine, 340, 560) {}

// ---------------------------------------------------------------------------
// Open / Close wrappers on Engine
// ---------------------------------------------------------------------------
void Engine::OpenVFXProfileWindow()
{
    if (!m_pVFXProfileWindow)
        m_pVFXProfileWindow = new VFXProfileWindow(this);
    m_pVFXProfileWindow->Open();
}

void Engine::CloseVFXProfileWindow()
{
    if (m_pVFXProfileWindow) {
        m_pVFXProfileWindow->Close();
        delete m_pVFXProfileWindow;
        m_pVFXProfileWindow = nullptr;
    }
}

// ---------------------------------------------------------------------------
// DoBuildControls
// ---------------------------------------------------------------------------
void VFXProfileWindow::DoBuildControls()
{
    auto base = BuildBaseControls();
    int y = base.y, lineH = base.lineH, gap = base.gap;
    int x = base.x, rw = base.rw;

    RECT rc;
    GetClientRect(m_hWnd, &rc);

    // The rows below the list are placed from the bottom of the client area
    // up, and the list takes whatever is left. The old layout gave the list a
    // fixed six lines and let everything after it run past the end of a window
    // that does not scroll: at the 300x400 an older install has saved, Save As,
    // Delete and the checkbox all sat below the client area and could not be
    // reached at all.
    const int btnW = MulDiv(90, lineH, 26);
    const int btnGap = 8;
    // Save As / Delete, then Import, then the startup checkbox. Import gets its
    // own row rather than squeezing three buttons across: at the width this
    // window is often left at, thirds are narrower than "Save As..." renders.
    const int bottomH = (lineH + gap) * 3;

    int listH = (rc.bottom - gap - bottomH) - (y + lineH + gap);
    if (listH < lineH * 2) listH = lineH * 2;

    // Profile listbox
    CreateLabel(m_hWnd, L"Profiles:", x, y, rw, lineH, m_hFontBold);
    y += lineH + gap;

    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        x, y, rw, listH, m_hWnd,
        (HMENU)(INT_PTR)IDC_MW_VFXP_LIST, GetModuleHandle(NULL), NULL);
    if (hList && m_hFont) SendMessage(hList, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    y += listH + gap;

    // Save As / Delete buttons
    CreateBtn(m_hWnd, L"Save As...", IDC_MW_VFXP_SAVE, x, y, btnW, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Delete", IDC_MW_VFXP_DELETE, x + btnW + btnGap, y, btnW, lineH, m_hFont);
    y += lineH + gap;

    CreateBtn(m_hWnd, L"Import...", IDC_MW_VFXP_IMPORT, x, y, btnW, lineH, m_hFont);
    y += lineH + gap;

    // "Save on exit" used to live here as well, but a setting nobody could find
    // is the same as no setting -- it now sits beside the Profiles button on the
    // Video Effects window, where the Save Profile button already is.
    CreateCheck(m_hWnd, L"Load on startup", IDC_MW_VFXP_STARTUP, x, y, rw, lineH, m_hFont,
                m_pEngine->m_bEnableVFXStartup);

    RefreshProfileList();
}

// ---------------------------------------------------------------------------
// RefreshProfileList — read the names out of the profile store
// ---------------------------------------------------------------------------
void VFXProfileWindow::RefreshProfileList()
{
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_VFXP_LIST);
    if (!hList) return;
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    m_profileNames.clear();

    // (None) entry
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"(None)");
    m_profileNames.push_back(L"");

    std::vector<std::wstring> names;
    m_pEngine->m_vfxProfiles.Names(names);

    int selIdx = 0;
    for (const auto& name : names) {
        int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)name.c_str());
        m_profileNames.push_back(name);
        if (_wcsicmp(name.c_str(), m_pEngine->m_szCurrentVFXProfile) == 0)
            selIdx = idx;
    }

    SendMessage(hList, LB_SETCURSEL, selIdx, 0);
}

// ---------------------------------------------------------------------------
// ApplySelectedProfile
// ---------------------------------------------------------------------------
void VFXProfileWindow::ApplySelectedProfile()
{
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_VFXP_LIST);
    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)m_profileNames.size()) return;

    if (m_profileNames[sel].empty()) {
        // (None) — reset to defaults, and detach from any profile so there is
        // nothing to be dirty against.
        m_pEngine->m_videoFX = VideoEffectParams{};
        m_pEngine->m_szCurrentVFXProfile[0] = 0;
    } else if (!m_pEngine->LoadVideoFXProfile(m_profileNames[sel].c_str())) {
        // Gone from the store since the list was built.
        RefreshProfileList();
        return;
    }
    // Freshly loaded means freshly clean: what is on screen is what the
    // profile holds.
    m_pEngine->MarkVideoFXSaved();

    // "Load on startup" tracks whatever is selected, so ticking it later does
    // not need the user to reselect.
    if (m_pEngine->m_bEnableVFXStartup)
        wcscpy_s(m_pEngine->m_szVFXStartup, m_pEngine->m_szCurrentVFXProfile);

    m_pEngine->SaveSpoutInputSettings();
    m_pEngine->OnVideoFXChanged();

    // Refresh effects window if open
    if (m_pEngine->m_pVideoEffectsWindow)
        m_pEngine->m_pVideoEffectsWindow->RebuildFonts();
}

// ---------------------------------------------------------------------------
// DoCommand
// ---------------------------------------------------------------------------
LRESULT VFXProfileWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam)
{
    switch (id) {
    case IDC_MW_VFXP_LIST:
        if (code == LBN_SELCHANGE)
            ApplySelectedProfile();
        return 0;

    case IDC_MW_VFXP_SAVE: {
        // Name it, do not pick a file for it -- a profile is a key in
        // vfxprofiles.json, so there is no path for the user to choose.
        std::vector<std::wstring> existing;
        m_pEngine->m_vfxProfiles.Names(existing);
        std::wstring name = m_pEngine->m_szCurrentVFXProfile;
        if (!PromptForName(m_pEngine, m_hWnd, L"Save VFX Profile",
                           L"Profile name:", name, MAX_VFX_PROFILE_NAME, existing))
            return 0;

        if (m_pEngine->m_vfxProfiles.Exists(name.c_str())) {
            wchar_t msg[512];
            swprintf_s(msg, L"Profile \"%s\" already exists. Replace it?", name.c_str());
            if (MessageBoxW(m_hWnd, msg, L"Save VFX Profile", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
        }

        if (m_pEngine->SaveVideoFXProfile(name.c_str())) {
            wcscpy_s(m_pEngine->m_szCurrentVFXProfile, name.c_str());
            m_pEngine->MarkVideoFXSaved();      // just written -> clean
            if (m_pEngine->m_bEnableVFXStartup)
                wcscpy_s(m_pEngine->m_szVFXStartup, name.c_str());
            m_pEngine->SaveSpoutInputSettings();
            m_pEngine->OnVideoFXChanged();
            RefreshProfileList();
        }
        return 0;
    }

    case IDC_MW_VFXP_IMPORT: {
        // Read settings written by an older build, or carried over from another
        // machine. Accepts an old settings.ini, a single videofx/*.json, or a
        // whole vfxprofiles.json -- see VFXProfileStore::Import.
        wchar_t filePath[MAX_PATH] = {};
        wchar_t initialDir[MAX_PATH];
        m_pEngine->m_vfxProfiles.GetStorePath(initialDir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(initialDir, L'\\')) *slash = 0;

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWnd;
        ofn.lpstrFilter = L"VFX settings (*.json;*.ini)\0*.json;*.ini\0"
                          L"Profile JSON (*.json)\0*.json\0"
                          L"Settings INI (*.ini)\0*.ini\0"
                          L"All Files\0*.*\0";
        ofn.lpstrFile = filePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = initialDir;
        ofn.lpstrTitle = L"Import VFX Settings";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return 0;

        std::wstring suggested;
        const int n = m_pEngine->m_vfxProfiles.PeekImport(filePath, suggested);
        if (n <= 0) {
            MessageBoxW(m_hWnd,
                n < 0 ? L"That file could not be read."
                      : L"No video effect settings were found in that file.",
                L"Import VFX Settings", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        int done = 0;
        if (n == 1 && !suggested.empty()) {
            // One unnamed parameter set: the file has no name for it, so ask.
            std::vector<std::wstring> existing;
            m_pEngine->m_vfxProfiles.Names(existing);
            std::wstring name = suggested;
            if (!PromptForName(m_pEngine, m_hWnd, L"Import VFX Settings",
                               L"Import as profile:", name, MAX_VFX_PROFILE_NAME, existing))
                return 0;
            if (m_pEngine->m_vfxProfiles.Exists(name.c_str())) {
                wchar_t msg[512];
                swprintf_s(msg, L"Profile \"%s\" already exists. Replace it?", name.c_str());
                if (MessageBoxW(m_hWnd, msg, L"Import VFX Settings", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
            }
            done = m_pEngine->m_vfxProfiles.Import(filePath, name.c_str());
        } else {
            wchar_t msg[512];
            swprintf_s(msg, L"Import %d profile(s) from this file?\n\n"
                            L"Names already in use are kept -- imported copies are numbered.", n);
            if (MessageBoxW(m_hWnd, msg, L"Import VFX Settings", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            done = m_pEngine->m_vfxProfiles.Import(filePath, L"");
        }

        if (done > 0) {
            RefreshProfileList();
            wchar_t msg[128];
            swprintf_s(msg, L"Imported %d profile(s).", done);
            MessageBoxW(m_hWnd, msg, L"Import VFX Settings", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(m_hWnd, L"Nothing was imported.", L"Import VFX Settings",
                        MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    case IDC_MW_VFXP_DELETE: {
        HWND hList = GetDlgItem(m_hWnd, IDC_MW_VFXP_LIST);
        int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (sel <= 0 || sel >= (int)m_profileNames.size()) return 0;  // can't delete (None)

        const std::wstring name = m_profileNames[sel];
        wchar_t msg[512];
        swprintf_s(msg, L"Delete profile \"%s\"?", name.c_str());
        if (MessageBoxW(m_hWnd, msg, L"Delete VFX Profile", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;

        m_pEngine->m_vfxProfiles.Delete(name.c_str());

        if (_wcsicmp(name.c_str(), m_pEngine->m_szCurrentVFXProfile) == 0) {
            // Detaching from the deleted profile also clears the dirty state:
            // there is no longer anything to compare live parameters against.
            m_pEngine->m_szCurrentVFXProfile[0] = 0;
            m_pEngine->OnVideoFXChanged();
        }
        if (_wcsicmp(name.c_str(), m_pEngine->m_szVFXStartup) == 0)
            m_pEngine->m_szVFXStartup[0] = 0;

        m_pEngine->SaveSpoutInputSettings();
        RefreshProfileList();
        return 0;
    }

    case IDC_MW_VFXP_STARTUP:
        m_pEngine->m_bEnableVFXStartup = IsChecked(id);
        if (m_pEngine->m_bEnableVFXStartup)
            wcscpy_s(m_pEngine->m_szVFXStartup, m_pEngine->m_szCurrentVFXProfile);
        m_pEngine->SaveSpoutInputSettings();
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// DoDestroy
// ---------------------------------------------------------------------------
void VFXProfileWindow::DoDestroy()
{
    m_pEngine->SaveSpoutInputSettings();
}

} // namespace mdrop
