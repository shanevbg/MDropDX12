/*
  engine_workspace_layout_ui.cpp — Workspace Layout window
  Tiles selected tool windows across the screen with the render window
  shrunk to a chosen corner or fullscreen on another display.
  Accessible from Welcome screen and Settings.
*/

#include "engine.h"
#include "tool_window.h"
#include "utility.h"
#include <cmath>
#include <algorithm>
#include <vector>

using namespace mdrop;

// ── Layout window entry: extensible registry of tileable windows ──

struct LayoutEntry {
    const wchar_t* name;
    int            checkboxID;
    bool           defaultOn;
};

static const LayoutEntry s_layoutWindows[] = {
    { L"Settings",      IDC_MW_WSLAYOUT_CHK_SETTINGS, true  },
    { L"Hotkeys",       IDC_MW_WSLAYOUT_CHK_HOTKEYS,  true  },
    { L"MIDI",          IDC_MW_WSLAYOUT_CHK_MIDI,      true  },
    { L"Button Board",  IDC_MW_WSLAYOUT_CHK_BOARD,     true  },
    { L"Presets",       IDC_MW_WSLAYOUT_CHK_PRESETS,   true  },
    { L"Displays",      IDC_MW_WSLAYOUT_CHK_DISPLAYS,  true  },
    { L"Shader Import", IDC_MW_WSLAYOUT_CHK_SHIMPORT,  true  },
    { L"Song Info",     IDC_MW_WSLAYOUT_CHK_SONGINFO,  true  },
    { L"Sprites",       IDC_MW_WSLAYOUT_CHK_SPRITES,   false },
    { L"Messages",      IDC_MW_WSLAYOUT_CHK_MESSAGES,  false },
};
static constexpr int NUM_LAYOUT_WINDOWS = _countof(s_layoutWindows);

// ── Monitor enumeration for display combo ──

struct MonitorEntry {
    HMONITOR hMon;
    RECT     rc;
    wchar_t  name[64];
};

static BOOL CALLBACK EnumMonCB(HMONITOR hMon, HDC, LPRECT lprc, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(lParam);
    MonitorEntry e{};
    e.hMon = hMon;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        e.rc = mi.rcMonitor;
        swprintf(e.name, 64, L"Display %d (%dx%d)",
            (int)list->size() + 1,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top);
    } else {
        e.rc = *lprc;
        swprintf(e.name, 64, L"Display %d", (int)list->size() + 1);
    }
    list->push_back(e);
    return TRUE;
}

// ── Open / Close ──

void Engine::OpenWorkspaceLayoutWindow() {
    if (!m_workspaceLayoutWindow)
        m_workspaceLayoutWindow = std::make_unique<WorkspaceLayoutWindow>(this);
    m_workspaceLayoutWindow->Open();
}

void Engine::CloseWorkspaceLayoutWindow() {
    if (m_workspaceLayoutWindow)
        m_workspaceLayoutWindow->Close();
}

// ── Constructor ──

WorkspaceLayoutWindow::WorkspaceLayoutWindow(Engine* pEngine)
    : ToolWindow(pEngine, 420, 800) {
}

// ── INI Persistence ──

void WorkspaceLayoutWindow::LoadLayoutPrefs() {
    const wchar_t* ini = m_pEngine->GetConfigIniFile();
    const wchar_t* sec = L"WorkspaceLayout";

    // Render mode: 0=corner on work display, 1=fullscreen on separate display
    int mode = GetPrivateProfileIntW(sec, L"RenderMode", 0, ini);
    SetChecked(IDC_MW_WSLAYOUT_MODE_CORNER, mode == 0);
    SetChecked(IDC_MW_WSLAYOUT_MODE_DISPLAY, mode == 1);

    // Corner: 0=TL, 1=TR, 2=BL, 3=BR (default TR)
    int corner = GetPrivateProfileIntW(sec, L"Corner", 1, ini);
    if (corner < 0 || corner > 3) corner = 1;
    SetChecked(IDC_MW_WSLAYOUT_CORNER_TL, corner == 0);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_TR, corner == 1);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_BL, corner == 2);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_BR, corner == 3);

    // Render size percent (default 20)
    int pct = GetPrivateProfileIntW(sec, L"RenderSizePct", 20, ini);
    if (pct < 10) pct = 10;
    if (pct > 40) pct = 40;
    HWND hSlider = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_SIZE_SLIDER);
    if (hSlider) SendMessageW(hSlider, TBM_SETPOS, TRUE, pct);
    UpdateSizeLabel();

    // Display index for fullscreen mode
    int dispIdx = GetPrivateProfileIntW(sec, L"DisplayIndex", 0, ini);
    HWND hCombo = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO);
    if (hCombo) {
        int count = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
        if (dispIdx >= 0 && dispIdx < count)
            SendMessageW(hCombo, CB_SETCURSEL, dispIdx, 0);
    }

    // Per-window checkboxes
    for (int i = 0; i < NUM_LAYOUT_WINDOWS; i++) {
        wchar_t key[64];
        swprintf(key, 64, L"Window_%ls", s_layoutWindows[i].name);
        int val = GetPrivateProfileIntW(sec, key, -1, ini);
        bool checked = (val == -1) ? s_layoutWindows[i].defaultOn : (val != 0);
        SetChecked(s_layoutWindows[i].checkboxID, checked);
    }

    UpdateModeState();
}

void WorkspaceLayoutWindow::SaveLayoutPrefs() {
    const wchar_t* ini = m_pEngine->GetConfigIniFile();
    const wchar_t* sec = L"WorkspaceLayout";
    wchar_t buf[16];

    // Render mode
    int mode = IsChecked(IDC_MW_WSLAYOUT_MODE_DISPLAY) ? 1 : 0;
    swprintf(buf, 16, L"%d", mode);
    WritePrivateProfileStringW(sec, L"RenderMode", buf, ini);

    // Corner
    int corner = 1;
    if (IsChecked(IDC_MW_WSLAYOUT_CORNER_TL)) corner = 0;
    else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_TR)) corner = 1;
    else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_BL)) corner = 2;
    else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_BR)) corner = 3;
    swprintf(buf, 16, L"%d", corner);
    WritePrivateProfileStringW(sec, L"Corner", buf, ini);

    // Size
    HWND hSlider = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_SIZE_SLIDER);
    int pct = hSlider ? (int)SendMessageW(hSlider, TBM_GETPOS, 0, 0) : 20;
    swprintf(buf, 16, L"%d", pct);
    WritePrivateProfileStringW(sec, L"RenderSizePct", buf, ini);

    // Display index
    HWND hCombo = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO);
    int dispIdx = hCombo ? (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0) : 0;
    if (dispIdx < 0) dispIdx = 0;
    swprintf(buf, 16, L"%d", dispIdx);
    WritePrivateProfileStringW(sec, L"DisplayIndex", buf, ini);

    // Per-window checkboxes
    for (int i = 0; i < NUM_LAYOUT_WINDOWS; i++) {
        wchar_t key[64];
        swprintf(key, 64, L"Window_%ls", s_layoutWindows[i].name);
        bool checked = IsChecked(s_layoutWindows[i].checkboxID);
        WritePrivateProfileStringW(sec, key, checked ? L"1" : L"0", ini);
    }
}

void WorkspaceLayoutWindow::UpdateSizeLabel() {
    HWND hSlider = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_SIZE_SLIDER);
    int pct = hSlider ? (int)SendMessageW(hSlider, TBM_GETPOS, 0, 0) : 20;
    wchar_t buf[16];
    swprintf(buf, 16, L"%d%%", pct);
    SetDlgItemTextW(m_hWnd, IDC_MW_WSLAYOUT_SIZE_LABEL, buf);
}

void WorkspaceLayoutWindow::UpdateModeState() {
    bool cornerMode = IsChecked(IDC_MW_WSLAYOUT_MODE_CORNER);

    // Enable/disable corner controls
    EnableWindow(GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_CORNER_TL), cornerMode);
    EnableWindow(GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_CORNER_TR), cornerMode);
    EnableWindow(GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_CORNER_BL), cornerMode);
    EnableWindow(GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_CORNER_BR), cornerMode);
    EnableWindow(GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_SIZE_SLIDER), cornerMode);

    // Enable/disable display combo
    EnableWindow(GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO), !cornerMode);
}

// ── Build Controls ──

void WorkspaceLayoutWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    auto L = BuildBaseControls();
    HFONT hFont = GetFont();
    HFONT hFontBold = GetFontBold();

    int pad = 16;
    int x = pad;
    int w = L.clientW - pad * 2;
    int lineH = L.lineH;
    int gap = 6;
    int y = L.y + gap;

    // ── Render Window Mode ──
    TrackControl(CreateLabel(hw, L"Render Window:", x, y, w, lineH, hFontBold));
    y += lineH + 4;

    // Mode radios (separate group from corner radios)
    TrackControl(CreateRadio(hw, L"Corner of work display",
        IDC_MW_WSLAYOUT_MODE_CORNER, x + 8, y, w - 16, lineH, hFont, true, true));
    y += lineH + 2;

    // Corner sub-options (indented)
    int rbw = (w - 40) / 2;
    TrackControl(CreateRadio(hw, L"Top-Left",     IDC_MW_WSLAYOUT_CORNER_TL, x + 24,          y, rbw, lineH, hFont, false, true));
    TrackControl(CreateRadio(hw, L"Top-Right",    IDC_MW_WSLAYOUT_CORNER_TR, x + 24 + rbw + 8, y, rbw, lineH, hFont, true, false));
    y += lineH + 2;
    TrackControl(CreateRadio(hw, L"Bottom-Left",  IDC_MW_WSLAYOUT_CORNER_BL, x + 24,          y, rbw, lineH, hFont, false, false));
    TrackControl(CreateRadio(hw, L"Bottom-Right", IDC_MW_WSLAYOUT_CORNER_BR, x + 24 + rbw + 8, y, rbw, lineH, hFont, false, false));
    y += lineH + 4;

    // Size slider (indented)
    TrackControl(CreateLabel(hw, L"Size:", x + 24, y, 40, lineH, hFont));
    HWND hSizeLbl = CreateLabel(hw, L"20%", x + 68, y, 40, lineH, hFont);
    TrackControl(hSizeLbl);
    SetWindowLongPtrW(hSizeLbl, GWLP_ID, IDC_MW_WSLAYOUT_SIZE_LABEL);
    y += lineH + 2;
    TrackControl(CreateSlider(hw, IDC_MW_WSLAYOUT_SIZE_SLIDER, x + 24, y, w - 32, lineH, 10, 40, 20));
    y += lineH + gap + 2;

    // Fullscreen on separate display mode
    TrackControl(CreateRadio(hw, L"Fullscreen on separate display",
        IDC_MW_WSLAYOUT_MODE_DISPLAY, x + 8, y, w - 16, lineH, hFont, false, false));
    y += lineH + 4;

    // Display combo (indented)
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x + 24, y, w - 32, lineH * 6, hw,
        (HMENU)(INT_PTR)IDC_MW_WSLAYOUT_DISPLAY_COMBO, NULL, NULL);
    TrackControl(hCombo);
    SendMessageW(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Populate display combo
    std::vector<MonitorEntry> monitors;
    EnumDisplayMonitors(NULL, NULL, EnumMonCB, reinterpret_cast<LPARAM>(&monitors));
    for (auto& m : monitors)
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)m.name);
    if (!monitors.empty())
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);

    y += lineH + gap + 4;

    // ── Separator ──
    HWND hSep = CreateWindowExW(0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        x, y, w, 2, hw, NULL, NULL, NULL);
    TrackControl(hSep);
    y += 2 + gap;

    // ── Windows to Open ──
    TrackControl(CreateLabel(hw, L"Windows to Open:", x, y, w, lineH, hFontBold));
    y += lineH + 4;

    for (int i = 0; i < NUM_LAYOUT_WINDOWS; i++) {
        TrackControl(CreateCheck(hw, s_layoutWindows[i].name,
            s_layoutWindows[i].checkboxID, x + 8, y, w - 16, lineH, hFont,
            s_layoutWindows[i].defaultOn));
        y += lineH + 2;
    }
    y += gap;

    // ── Buttons ──
    int btnW = (w - 8) / 2;
    int btnH = lineH + 10;
    TrackControl(CreateBtn(hw, L"Apply Layout", IDC_MW_WSLAYOUT_APPLY, x, y, btnW, btnH, hFont));
    TrackControl(CreateBtn(hw, L"Reset Defaults", IDC_MW_WSLAYOUT_RESET, x + btnW + 8, y, btnW, btnH, hFont));

    // Load saved preferences (overrides defaults)
    LoadLayoutPrefs();
}

// ── Open a tool window by checkbox ID and return its ToolWindow* ──

static ToolWindow* OpenAndGetWindow(Engine* e, int checkboxID) {
    switch (checkboxID) {
    case IDC_MW_WSLAYOUT_CHK_SETTINGS: e->OpenSettingsWindow();     return e->m_settingsWindow.get();
    case IDC_MW_WSLAYOUT_CHK_HOTKEYS:  e->OpenHotkeysWindow();      return e->m_hotkeysWindow.get();
    case IDC_MW_WSLAYOUT_CHK_MIDI:     e->OpenMidiWindow();         return e->m_midiWindow.get();
    case IDC_MW_WSLAYOUT_CHK_BOARD:    e->OpenBoardWindow();        return e->m_boardWindow.get();
    case IDC_MW_WSLAYOUT_CHK_PRESETS:  e->OpenPresetsWindow();      return e->m_presetsWindow.get();
    case IDC_MW_WSLAYOUT_CHK_DISPLAYS: e->OpenDisplaysWindow();     return e->m_displaysWindow.get();
    case IDC_MW_WSLAYOUT_CHK_SHIMPORT: e->OpenShaderImportWindow(); return e->m_shaderImportWindow.get();
    case IDC_MW_WSLAYOUT_CHK_SONGINFO: e->OpenSongInfoWindow();     return e->m_songInfoWindow.get();
    case IDC_MW_WSLAYOUT_CHK_SPRITES:  e->OpenSpritesWindow();      return e->m_spritesWindow.get();
    case IDC_MW_WSLAYOUT_CHK_MESSAGES: e->OpenMessagesWindow();     return e->m_messagesWindow.get();
    default: return nullptr;
    }
}

// ── Apply Layout ──

void WorkspaceLayoutWindow::ApplyLayout() {
    Engine* p = m_pEngine;
    bool useDisplay = IsChecked(IDC_MW_WSLAYOUT_MODE_DISPLAY);

    // Enumerate monitors for display mode
    std::vector<MonitorEntry> monitors;
    EnumDisplayMonitors(NULL, NULL, EnumMonCB, reinterpret_cast<LPARAM>(&monitors));

    // Get work area of the PRIMARY monitor (where tool windows will tile)
    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea.left = 0;
        workArea.top = 0;
        workArea.right = GetSystemMetrics(SM_CXSCREEN);
        workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int waW = workArea.right - workArea.left;
    int waH = workArea.bottom - workArea.top;

    HWND hRender = p->g_hwnd;
    RECT tileArea = workArea; // area for tiling tool windows

    if (useDisplay && monitors.size() > 1) {
        // ── Fullscreen on separate display ──
        HWND hCombo = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO);
        int sel = hCombo ? (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0) : 0;
        if (sel < 0) sel = 0;
        if (sel < (int)monitors.size()) {
            RECT& rc = monitors[sel].rc;
            int mw = rc.right - rc.left;
            int mh = rc.bottom - rc.top;

            if (hRender && IsWindow(hRender)) {
                // Save current style then go borderless fullscreen on that monitor
                SetWindowLongPtrW(hRender, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowLongPtrW(hRender, GWL_EXSTYLE, WS_EX_APPWINDOW);
                SetWindowPos(hRender, HWND_TOPMOST, rc.left, rc.top, mw, mh,
                             SWP_DRAWFRAME | SWP_FRAMECHANGED);
                p->SetVariableBackBuffer(mw, mh);
                p->m_WindowX = rc.left;
                p->m_WindowY = rc.top;
                p->m_WindowWidth = mw;
                p->m_WindowHeight = mh;
                p->m_bAlwaysOnTop = true;
            }

            // Tile on the primary monitor's work area (already set as tileArea)
            // If the selected display IS the primary, pick a different monitor for tiling
            HMONITOR hPrimary = MonitorFromPoint({workArea.left, workArea.top}, MONITOR_DEFAULTTOPRIMARY);
            if (monitors[sel].hMon == hPrimary && monitors.size() > 1) {
                // Find a non-selected monitor for tiling
                for (int i = 0; i < (int)monitors.size(); i++) {
                    if (i != sel) {
                        MONITORINFOEXW mi{};
                        mi.cbSize = sizeof(mi);
                        if (GetMonitorInfoW(monitors[i].hMon, &mi)) {
                            tileArea = mi.rcWork;
                            break;
                        }
                    }
                }
            }
        }
    } else {
        // ── Corner mode ──
        int corner = 1;
        if (IsChecked(IDC_MW_WSLAYOUT_CORNER_TL)) corner = 0;
        else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_TR)) corner = 1;
        else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_BL)) corner = 2;
        else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_BR)) corner = 3;

        HWND hSlider = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_SIZE_SLIDER);
        int sizePct = hSlider ? (int)SendMessageW(hSlider, TBM_GETPOS, 0, 0) : 20;
        if (sizePct < 10) sizePct = 10;
        if (sizePct > 40) sizePct = 40;

        int renderW = (int)(waW * sizePct / 100.0f);
        int renderH = (int)(renderW * 9.0f / 16.0f);
        if (renderH > waH / 2) renderH = waH / 2;

        int renderX, renderY;
        switch (corner) {
        case 0: renderX = workArea.left;              renderY = workArea.top;               break;
        case 1: renderX = workArea.right - renderW;   renderY = workArea.top;               break;
        case 2: renderX = workArea.left;              renderY = workArea.bottom - renderH;   break;
        case 3: renderX = workArea.right - renderW;   renderY = workArea.bottom - renderH;   break;
        default: renderX = workArea.right - renderW;  renderY = workArea.top;               break;
        }

        if (hRender && IsWindow(hRender)) {
            SetWindowPos(hRender, HWND_TOPMOST, renderX, renderY, renderW, renderH,
                         SWP_DRAWFRAME | SWP_FRAMECHANGED);
            p->m_WindowX = renderX;
            p->m_WindowY = renderY;
            p->m_WindowWidth = renderW;
            p->m_WindowHeight = renderH;
            p->m_bAlwaysOnTop = true;
        }

        // Compute tiling area: use the biggest rectangle not overlapped by render
        bool renderOnLeft = (corner == 0 || corner == 2);
        bool renderOnTop  = (corner == 0 || corner == 1);

        RECT stripH, stripV;
        if (renderOnTop)
            stripH = { workArea.left, workArea.top + renderH, workArea.right, workArea.bottom };
        else
            stripH = { workArea.left, workArea.top, workArea.right, workArea.bottom - renderH };

        if (renderOnLeft)
            stripV = { workArea.left + renderW, renderOnTop ? workArea.top : workArea.bottom - renderH,
                       workArea.right, renderOnTop ? workArea.top + renderH : workArea.bottom };
        else
            stripV = { workArea.left, renderOnTop ? workArea.top : workArea.bottom - renderH,
                       workArea.right - renderW, renderOnTop ? workArea.top + renderH : workArea.bottom };

        long long aH = (long long)(stripH.right - stripH.left) * (stripH.bottom - stripH.top);
        long long aV = (long long)(stripV.right - stripV.left) * (stripV.bottom - stripV.top);
        tileArea = (aH >= aV) ? stripH : stripV;

        int tw = tileArea.right - tileArea.left;
        int th = tileArea.bottom - tileArea.top;
        if (tw < 400 || th < 300)
            tileArea = workArea;
    }

    // ── Phase 1: Open all checked windows ──
    struct OpenedWindow {
        ToolWindow* tw;
    };
    std::vector<OpenedWindow> opened;

    for (int i = 0; i < NUM_LAYOUT_WINDOWS; i++) {
        bool chk = IsChecked(s_layoutWindows[i].checkboxID);
        DebugLogAFmt("WorkspaceLayout: checkbox %ls (id=%d) checked=%d",
            s_layoutWindows[i].name, s_layoutWindows[i].checkboxID, (int)chk);
        if (chk) {
            ToolWindow* tw = OpenAndGetWindow(p, s_layoutWindows[i].checkboxID);
            DebugLogAFmt("WorkspaceLayout: OpenAndGetWindow returned %p", tw);
            if (tw)
                opened.push_back({ tw });
        }
    }

    DebugLogAFmt("WorkspaceLayout: %d windows to open", (int)opened.size());

    if (opened.empty()) {
        SaveLayoutPrefs();
        PostMessage(m_hWnd, WM_CLOSE, 0, 0);
        return;
    }

    // ── Phase 2: Wait for all windows to have HWNDs ──
    for (int attempt = 0; attempt < 60; attempt++) {
        bool allReady = true;
        for (auto& ow : opened) {
            if (!ow.tw->GetHWND()) {
                allReady = false;
                break;
            }
        }
        if (allReady) break;
        Sleep(50);
    }

    // Log HWND status
    for (int i = 0; i < (int)opened.size(); i++)
        DebugLogAFmt("WorkspaceLayout: window %d HWND=%p", i, opened[i].tw->GetHWND());

    // ── Phase 3: Position all windows in grid ──
    int N = (int)opened.size();
    int cols = (int)std::ceil(std::sqrt((double)N));
    if (cols < 1) cols = 1;
    int rows = (int)std::ceil((double)N / cols);

    int tileW = tileArea.right - tileArea.left;
    int tileH = tileArea.bottom - tileArea.top;
    int cellW = tileW / cols;
    int cellH = tileH / rows;

    for (int i = 0; i < N; i++) {
        HWND hwnd = opened[i].tw->GetHWND();
        if (!hwnd) continue;

        int col = i % cols;
        int row = i / cols;
        int wx = tileArea.left + col * cellW;
        int wy = tileArea.top + row * cellH;

        SetWindowPos(hwnd, HWND_TOPMOST, wx, wy, cellW, cellH,
                     SWP_DRAWFRAME | SWP_FRAMECHANGED);
    }

    SaveLayoutPrefs();
    PostMessage(m_hWnd, WM_CLOSE, 0, 0);
}

void WorkspaceLayoutWindow::ResetDefaults() {
    // Mode: corner
    SetChecked(IDC_MW_WSLAYOUT_MODE_CORNER, true);
    SetChecked(IDC_MW_WSLAYOUT_MODE_DISPLAY, false);

    // Corner: Top-Right
    SetChecked(IDC_MW_WSLAYOUT_CORNER_TL, false);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_TR, true);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_BL, false);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_BR, false);

    // Size: 20%
    HWND hSlider = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_SIZE_SLIDER);
    if (hSlider) SendMessageW(hSlider, TBM_SETPOS, TRUE, 20);
    UpdateSizeLabel();

    // Display combo: first item
    HWND hCombo = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO);
    if (hCombo) SendMessageW(hCombo, CB_SETCURSEL, 0, 0);

    // Checkboxes: reset to defaults
    for (int i = 0; i < NUM_LAYOUT_WINDOWS; i++)
        SetChecked(s_layoutWindows[i].checkboxID, s_layoutWindows[i].defaultOn);

    UpdateModeState();
}

// ── Command Handler ──

LRESULT WorkspaceLayoutWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    if (code != BN_CLICKED) return -1;

    // Checkbox state is auto-toggled by base class. Handle radio groups here.
    HWND hCtrl = (HWND)lParam;
    bool bIsRadio = (bool)(intptr_t)GetPropW(hCtrl, L"IsRadio");
    if (bIsRadio) {
        // Mode radio group
        static const int modeRadioIDs[] = { IDC_MW_WSLAYOUT_MODE_CORNER, IDC_MW_WSLAYOUT_MODE_DISPLAY };
        for (int rid : modeRadioIDs) {
            if (rid == id) {
                for (int gid : modeRadioIDs) {
                    HWND hR = GetDlgItem(hWnd, gid);
                    if (hR) { SetPropW(hR, L"Checked", (HANDLE)(intptr_t)(gid == id ? 1 : 0)); InvalidateRect(hR, NULL, TRUE); }
                }
                break;
            }
        }
        // Corner radio group
        static const int cornerRadioIDs[] = { IDC_MW_WSLAYOUT_CORNER_TL, IDC_MW_WSLAYOUT_CORNER_TR,
                                               IDC_MW_WSLAYOUT_CORNER_BL, IDC_MW_WSLAYOUT_CORNER_BR };
        for (int rid : cornerRadioIDs) {
            if (rid == id) {
                for (int gid : cornerRadioIDs) {
                    HWND hR = GetDlgItem(hWnd, gid);
                    if (hR) { SetPropW(hR, L"Checked", (HANDLE)(intptr_t)(gid == id ? 1 : 0)); InvalidateRect(hR, NULL, TRUE); }
                }
                break;
            }
        }
    }

    // ── Action buttons ──
    switch (id) {
    case IDC_MW_WSLAYOUT_APPLY:
        ApplyLayout();
        return 0;
    case IDC_MW_WSLAYOUT_RESET:
        ResetDefaults();
        return 0;
    case IDC_MW_WSLAYOUT_MODE_CORNER:
    case IDC_MW_WSLAYOUT_MODE_DISPLAY:
        UpdateModeState();
        return 0;
    }
    return -1;
}

// ── Slider Handler ──

LRESULT WorkspaceLayoutWindow::DoHScroll(HWND hWnd, int id, int pos) {
    if (id == IDC_MW_WSLAYOUT_SIZE_SLIDER) {
        UpdateSizeLabel();
        return 0;
    }
    return -1;
}
