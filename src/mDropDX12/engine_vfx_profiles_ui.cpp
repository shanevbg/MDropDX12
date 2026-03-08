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
    : ToolWindow(pEngine, 300, 400) {}

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

    // Profile listbox
    CreateLabel(m_hWnd, L"Profiles:", x, y, rw, lineH, m_hFontBold);
    y += lineH + gap;

    int listH = lineH * 6;
    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        x, y, rw, listH, m_hWnd,
        (HMENU)(INT_PTR)IDC_MW_VFXP_LIST, GetModuleHandle(NULL), NULL);
    if (hList && m_hFont) SendMessage(hList, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    y += listH + gap;

    // Save As / Delete buttons
    int btnW = MulDiv(90, lineH, 26);
    int btnGap = 8;
    CreateBtn(m_hWnd, L"Save As...", IDC_MW_VFXP_SAVE, x, y, btnW, lineH, m_hFont);
    CreateBtn(m_hWnd, L"Delete", IDC_MW_VFXP_DELETE, x + btnW + btnGap, y, btnW, lineH, m_hFont);
    y += lineH + gap;

    // Startup checkboxes
    CreateCheck(m_hWnd, L"Load on startup", IDC_MW_VFXP_STARTUP, x, y, rw, lineH, m_hFont,
                m_pEngine->m_bEnableVFXStartup);
    y += lineH + gap;
    CreateCheck(m_hWnd, L"Save on close", IDC_MW_VFXP_SAVECLOSE, x, y, rw, lineH, m_hFont,
                m_pEngine->m_bEnableVFXStartupSavingOnClose);

    RefreshProfileList();
}

// ---------------------------------------------------------------------------
// RefreshProfileList — scan resources/videofx/*.json, populate listbox
// ---------------------------------------------------------------------------
void VFXProfileWindow::RefreshProfileList()
{
    HWND hList = GetDlgItem(m_hWnd, IDC_MW_VFXP_LIST);
    if (!hList) return;
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    m_profilePaths.clear();

    // (None) entry
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"(None)");
    m_profilePaths.push_back(L"");

    // Scan directory
    wchar_t dir[MAX_PATH];
    m_pEngine->GetVideoFXProfileDir(dir, MAX_PATH);

    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, MAX_PATH, L"%s*.json", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    int selIdx = 0;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            wchar_t fullPath[MAX_PATH];
            swprintf_s(fullPath, MAX_PATH, L"%s%s", dir, fd.cFileName);

            // Display name = filename without .json extension
            wchar_t displayName[MAX_PATH];
            wcscpy_s(displayName, fd.cFileName);
            wchar_t* dot = wcsrchr(displayName, L'.');
            if (dot) *dot = 0;

            int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)displayName);
            m_profilePaths.push_back(fullPath);

            // Select if this is the current profile
            if (_wcsicmp(fullPath, m_pEngine->m_szCurrentVFXProfile) == 0)
                selIdx = idx;
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
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
    if (sel < 0 || sel >= (int)m_profilePaths.size()) return;

    if (m_profilePaths[sel].empty()) {
        // (None) — reset to defaults
        m_pEngine->m_videoFX = Engine::VideoEffectParams{};
        m_pEngine->m_szCurrentVFXProfile[0] = 0;
    } else {
        m_pEngine->LoadVideoFXProfile(m_profilePaths[sel].c_str());
    }

    // Update startup path if startup is enabled
    if (m_pEngine->m_bEnableVFXStartup)
        wcscpy_s(m_pEngine->m_szVFXStartup, m_pEngine->m_szCurrentVFXProfile);

    m_pEngine->SaveSpoutInputSettings();

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
        // Save As — prompt with GetSaveFileNameW
        wchar_t dir[MAX_PATH];
        m_pEngine->GetVideoFXProfileDir(dir, MAX_PATH);
        CreateDirectoryW(dir, NULL);

        wchar_t filePath[MAX_PATH] = {};
        // Pre-fill with current profile filename
        if (wcslen(m_pEngine->m_szCurrentVFXProfile) > 0) {
            const wchar_t* fname = PathFindFileNameW(m_pEngine->m_szCurrentVFXProfile);
            wcscpy_s(filePath, fname);
        }

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWnd;
        ofn.lpstrFilter = L"VFX Profile (*.json)\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = filePath;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir;
        ofn.lpstrDefExt = L"json";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        if (GetSaveFileNameW(&ofn)) {
            m_pEngine->SaveVideoFXProfile(filePath);
            wcscpy_s(m_pEngine->m_szCurrentVFXProfile, filePath);
            if (m_pEngine->m_bEnableVFXStartup)
                wcscpy_s(m_pEngine->m_szVFXStartup, filePath);
            m_pEngine->SaveSpoutInputSettings();
            RefreshProfileList();
        }
        return 0;
    }

    case IDC_MW_VFXP_DELETE: {
        HWND hList = GetDlgItem(m_hWnd, IDC_MW_VFXP_LIST);
        int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
        if (sel <= 0 || sel >= (int)m_profilePaths.size()) return 0;  // can't delete (None)

        wchar_t name[MAX_PATH];
        SendMessageW(hList, LB_GETTEXT, sel, (LPARAM)name);
        wchar_t msg[512];
        swprintf_s(msg, L"Delete profile \"%s\"?", name);
        if (MessageBoxW(m_hWnd, msg, L"Delete VFX Profile", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;

        DeleteFileW(m_profilePaths[sel].c_str());

        if (_wcsicmp(m_profilePaths[sel].c_str(), m_pEngine->m_szCurrentVFXProfile) == 0)
            m_pEngine->m_szCurrentVFXProfile[0] = 0;
        if (_wcsicmp(m_profilePaths[sel].c_str(), m_pEngine->m_szVFXStartup) == 0)
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

    case IDC_MW_VFXP_SAVECLOSE:
        m_pEngine->m_bEnableVFXStartupSavingOnClose = IsChecked(id);
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
