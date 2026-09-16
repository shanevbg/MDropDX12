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
#include "config_store.h"

using mdrop::Config;
using mdrop::ConfigFile;

using namespace mdrop;

// ── Layout window entry: extensible registry of tileable windows ──

// -- The window list, derived rather than enumerated ------------------------
//
// This used to be a hand-written table, and it had drifted: twelve tool
// windows that exist -- Video Effects, VFX Profiles, Custom Shaders, Text
// Animations, Remote, Visual, Colors, Controller, Annotations, Script, Preset
// Editor and Workspace Layout itself -- were missing from it, because adding a
// window means remembering three separate places and one is easy to skip.
//
// It is now built from the HKCAT_TOOLS hotkey entries, the same source the
// Settings window's Tools tab and the Hotkeys window already use. A tool action
// that maps to a window appears here automatically; one that does not --
// Toggle Shader Overrides, Apply Workspace Layout -- is filtered out.
struct LayoutEntry {
    std::wstring   name;        // display name, and the persistence key
    int            checkboxID;
    int            hotkeyAction;
    bool           defaultOn;
};

// Opens, or merely locates, the tool window an action id refers to. THE one
// place that knows which actions are windows. `open == false` is how the list
// is built without opening two dozen windows just to enumerate them.
static ToolWindow* ToolWindowForAction(Engine* e, int hkId, bool open) {
    switch (hkId) {
    case HK_OPEN_SETTINGS:      if (open) e->OpenSettingsWindow();     return e->m_settingsWindow.get();
    case HK_OPEN_HOTKEYS:       if (open) e->OpenHotkeysWindow();      return e->m_hotkeysWindow.get();
    case HK_OPEN_MIDI:          if (open) e->OpenMidiWindow();         return e->m_midiWindow.get();
    case HK_OPEN_BOARD:         if (open) e->OpenBoardWindow();        return e->m_boardWindow.get();
    case HK_OPEN_PRESETS:       if (open) e->OpenPresetsWindow();      return e->m_presetsWindow.get();
    case HK_OPEN_DISPLAYS:      if (open) e->OpenDisplaysWindow();     return e->m_displaysWindow.get();
    case HK_OPEN_SHADER_IMPORT: if (open) e->OpenShaderImportWindow(); return e->m_shaderImportWindow.get();
    case HK_OPEN_SONGINFO:      if (open) e->OpenSongInfoWindow();     return e->m_songInfoWindow.get();
    case HK_OPEN_SPRITES:       if (open) e->OpenSpritesWindow();      return e->m_spritesWindow.get();
    case HK_OPEN_MESSAGES:      if (open) e->OpenMessagesWindow();     return e->m_messagesWindow.get();
    case HK_OPEN_AUDIO_MIXER:   if (open) e->OpenAudioMixerWindow();   return e->m_mixerWindow.get();
    case HK_OPEN_ANNOTATIONS:   if (open) e->OpenAnnotationsWindow();  return e->m_annotationsWindow.get();
    case HK_OPEN_PRESET_EDITOR: if (open) e->OpenPresetEditorWindow(); return e->m_presetEditorWindow.get();
    case HK_OPEN_SCRIPT:        if (open) e->OpenScriptWindow();       return e->m_scriptWindow.get();
    case HK_OPEN_REMOTE:        if (open) e->OpenRemoteWindow();       return e->m_remoteWindow.get();
    case HK_OPEN_VISUAL:        if (open) e->OpenVisualWindow();       return e->m_visualWindow.get();
    case HK_OPEN_COLORS:        if (open) e->OpenColorsWindow();       return e->m_colorsWindow.get();
    case HK_OPEN_CONTROLLER:    if (open) e->OpenControllerWindow();   return e->m_controllerWindow.get();
    case HK_OPEN_TEXT_ANIM:     if (open) e->OpenTextAnimWindow();     return e->m_textAnimWindow.get();
    case HK_OPEN_WORKSPACE_LAYOUT:
        if (open) e->OpenWorkspaceLayoutWindow();
        return e->m_workspaceLayoutWindow.get();
    // These three hold their window in a raw pointer and have openers that do
    // not follow the OpenToolWindow pattern, which is part of why they were
    // missed by the hand-written list.
    case HK_OPEN_VIDEO_FX:
        if (open) e->OpenVideoEffectsWindow();
        return (ToolWindow*)e->m_pVideoEffectsWindow;
    case HK_OPEN_VFX_PROFILES:
        if (open) e->OpenVFXProfileWindow();
        return (ToolWindow*)e->m_pVFXProfileWindow;
    case HK_OPEN_CUSTOM_SHADERS:
        if (open) e->OpenCustomShadersWindow();
        return (ToolWindow*)e->m_pCustomShadersWindow;
    default: return nullptr;    // not a window, or not wired up yet
    }
}

// True when an action is a tool window, without opening anything.
static bool IsToolWindowAction(int hkId) {
    switch (hkId) {
    case HK_OPEN_SETTINGS: case HK_OPEN_HOTKEYS: case HK_OPEN_MIDI:
    case HK_OPEN_BOARD: case HK_OPEN_PRESETS: case HK_OPEN_DISPLAYS:
    case HK_OPEN_SHADER_IMPORT: case HK_OPEN_SONGINFO: case HK_OPEN_SPRITES:
    case HK_OPEN_MESSAGES: case HK_OPEN_ANNOTATIONS: case HK_OPEN_PRESET_EDITOR:
    case HK_OPEN_SCRIPT: case HK_OPEN_REMOTE: case HK_OPEN_VISUAL:
    case HK_OPEN_COLORS: case HK_OPEN_CONTROLLER: case HK_OPEN_TEXT_ANIM:
    case HK_OPEN_WORKSPACE_LAYOUT: case HK_OPEN_VIDEO_FX:
    case HK_OPEN_VFX_PROFILES: case HK_OPEN_CUSTOM_SHADERS:
    case HK_OPEN_AUDIO_MIXER:
        return true;
    default:
        return false;
    }
}

// The tool windows a caller may address, by name.
std::vector<std::wstring> Engine::ToolWindowNames() {
    std::vector<std::wstring> out;
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        const HotkeyBinding& hk = m_hotkeys[i];
        if (hk.category != HKCAT_TOOLS || !IsToolWindowAction(hk.id)) continue;
        ToolWindow* w = ToolWindowForAction(this, hk.id, false);
        out.push_back(std::wstring(hk.szIniKey) + L"|" +
                      (w && w->IsOpen() ? L"open" : L"closed"));
    }
    return out;
}

// Find a tool window by name WITHOUT opening it -- for closing, moving or
// pressing something in one, none of which should bring a window into
// existence as a side effect.
static ToolWindow* FindOpenToolWindow(Engine* e, const std::wstring& name) {
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        const HotkeyBinding& hk = e->m_hotkeys[i];
        if (_wcsicmp(hk.szIniKey, name.c_str()) != 0) continue;
        if (!IsToolWindowAction(hk.id)) return nullptr;
        return ToolWindowForAction(e, hk.id, false);
    }
    return nullptr;
}

bool Engine::CloseToolWindowUI(const std::wstring& name) {
    ToolWindow* w = FindOpenToolWindow(this, name);
    if (!w || !w->IsOpen()) return false;
    w->Close();
    return true;
}

bool Engine::ClickToolWindowUI(const std::wstring& name, int ctrlId) {
    ToolWindow* w = FindOpenToolWindow(this, name);
    if (!w || !w->IsOpen() || ctrlId <= 0) return false;
    w->ClickControl(ctrlId);
    return true;
}

bool Engine::MoveToolWindowUI(const std::wstring& name, int& x, int& y) {
    ToolWindow* w = FindOpenToolWindow(this, name);
    if (!w || !w->IsOpen()) return false;
    if (x == INT_MIN || y == INT_MIN) w->RenderCentredPos(x, y);
    w->MoveTo(x, y);
    return true;
}

// Open a tool window by name, and optionally go straight to something in it.
//
// The name is the hotkey entry's INI key, which is the vocabulary already used
// by the Hotkeys window, by settings.ini and by the layout list -- so there is
// no second list of window names to keep in step with this one.
bool Engine::ShowToolWindowUI(const std::wstring& name, int tab, int ctrlId) {
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        const HotkeyBinding& hk = m_hotkeys[i];
        if (_wcsicmp(hk.szIniKey, name.c_str()) != 0) continue;
        if (!IsToolWindowAction(hk.id)) return false;

        // The tab is written before the window is built, so a window that is
        // not open yet lands on the right page rather than flashing the wrong
        // one first.
        ToolWindow* w = ToolWindowForAction(this, hk.id, false);
        if (w && tab >= 0) w->ShowTab(tab);
        w = ToolWindowForAction(this, hk.id, true);
        if (!w) return false;

        // No waiting, and no guessing. A tool window is built on its own
        // thread, so whether a control exists is a question only that thread
        // can answer -- it reports UI_SHOWN|found=... once it has looked.
        if (tab >= 0) w->ShowTab(tab);
        if (ctrlId > 0) w->ShowControl(ctrlId);
        return true;
    }
    return false;
}

// The eight that were on by default before this became derived. Anything
// discovered since defaults OFF: a layout that suddenly opened a dozen more
// windows than it used to would be an unpleasant surprise.
static bool DefaultOnFor(int hkId) {
    switch (hkId) {
    case HK_OPEN_SETTINGS: case HK_OPEN_HOTKEYS: case HK_OPEN_MIDI:
    case HK_OPEN_BOARD: case HK_OPEN_PRESETS: case HK_OPEN_DISPLAYS:
    case HK_OPEN_SHADER_IMPORT: case HK_OPEN_SONGINFO:
        return true;
    default:
        return false;
    }
}

// The label with its "Open " prefix removed: what the checkbox shows, and what
// the saved setting is keyed on.
//
// One override matters. The Displays action is labelled "Open Spout/Displays",
// so deriving blindly would produce "Spout/Displays" and orphan every existing
// Window_Displays setting.
static std::wstring LayoutNameFor(int hkId, const wchar_t* label) {
    if (hkId == HK_OPEN_DISPLAYS) return L"Displays";
    std::wstring name = label ? label : L"";
    const std::wstring prefix = L"Open ";
    if (name.compare(0, prefix.size(), prefix) == 0)
        name = name.substr(prefix.size());
    return name;
}

static std::vector<LayoutEntry> BuildLayoutWindows(Engine* e) {
    std::vector<LayoutEntry> out;
    if (!e) return out;
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (e->m_hotkeys[i].category != HKCAT_TOOLS) continue;
        const int hkId = e->m_hotkeys[i].id;
        if (!IsToolWindowAction(hkId)) continue;
        const int id = IDC_MW_WSLAYOUT_CHK_BASE + (int)out.size();
        if (id > IDC_MW_WSLAYOUT_CHK_MAX) {
            DebugLogAFmt("WorkspaceLayout: ran out of checkbox ids at %d", id);
            break;
        }
        LayoutEntry entry;
        entry.name = LayoutNameFor(hkId, e->m_hotkeys[i].szAction);
        entry.checkboxID = id;
        entry.hotkeyAction = hkId;
        entry.defaultOn = DefaultOnFor(hkId);
        out.push_back(entry);
    }
    return out;
}


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

// ── Constructor ──

WorkspaceLayoutWindow::WorkspaceLayoutWindow(Engine* pEngine)
    : ToolWindow(pEngine, 420, 800) {
}

// ── INI Persistence ──

void WorkspaceLayoutWindow::LoadLayoutPrefs() {
    const wchar_t* sec = L"WorkspaceLayout";

    // Render mode: 0=corner on work display, 1=fullscreen on separate display
    int mode = Config().GetInt(sec, L"RenderMode", 0);
    SetChecked(IDC_MW_WSLAYOUT_MODE_CORNER, mode == 0);
    SetChecked(IDC_MW_WSLAYOUT_MODE_DISPLAY, mode == 1);

    // Corner: 0=TL, 1=TR, 2=BL, 3=BR (default TR)
    int corner = Config().GetInt(sec, L"Corner", 1);
    if (corner < 0 || corner > 3) corner = 1;
    SetChecked(IDC_MW_WSLAYOUT_CORNER_TL, corner == 0);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_TR, corner == 1);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_BL, corner == 2);
    SetChecked(IDC_MW_WSLAYOUT_CORNER_BR, corner == 3);

    // Render size percent (default 20)
    int pct = Config().GetInt(sec, L"RenderSizePct", 20);
    if (pct < 10) pct = 10;
    if (pct > 40) pct = 40;
    HWND hSlider = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_SIZE_SLIDER);
    if (hSlider) SendMessageW(hSlider, TBM_SETPOS, TRUE, pct);
    UpdateSizeLabel();

    // Display index for fullscreen mode
    int dispIdx = Config().GetInt(sec, L"DisplayIndex", 0);
    HWND hDisplayCombo = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO);
    if (hDisplayCombo) {
        int count = (int)SendMessageW(hDisplayCombo, CB_GETCOUNT, 0, 0);
        if (dispIdx >= 0 && dispIdx < count)
            SendMessageW(hDisplayCombo, CB_SETCURSEL, dispIdx, 0);
    }

    // Per-window checkboxes
    for (const LayoutEntry& w : BuildLayoutWindows(m_pEngine)) {
        wchar_t key[64];
        swprintf(key, 64, L"Window_%ls", w.name.c_str());
        int val = Config().GetInt(sec, key, -1);
        bool checked = (val == -1) ? w.defaultOn : (val != 0);
        SetChecked(w.checkboxID, checked);
    }

    UpdateModeState();
}

void WorkspaceLayoutWindow::SaveLayoutPrefs() {
    const wchar_t* sec = L"WorkspaceLayout";
    wchar_t buf[16];

    // Render mode
    int mode = IsChecked(IDC_MW_WSLAYOUT_MODE_DISPLAY) ? 1 : 0;
    swprintf(buf, 16, L"%d", mode);
    Config().SetString(sec, L"RenderMode", buf);

    // Corner
    int corner = 1;
    if (IsChecked(IDC_MW_WSLAYOUT_CORNER_TL)) corner = 0;
    else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_TR)) corner = 1;
    else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_BL)) corner = 2;
    else if (IsChecked(IDC_MW_WSLAYOUT_CORNER_BR)) corner = 3;
    swprintf(buf, 16, L"%d", corner);
    Config().SetString(sec, L"Corner", buf);

    // Size
    HWND hSlider = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_SIZE_SLIDER);
    int pct = hSlider ? (int)SendMessageW(hSlider, TBM_GETPOS, 0, 0) : 20;
    swprintf(buf, 16, L"%d", pct);
    Config().SetString(sec, L"RenderSizePct", buf);

    // Display index
    HWND hDisplayCombo = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO);
    int dispIdx = hDisplayCombo ? (int)SendMessageW(hDisplayCombo, CB_GETCURSEL, 0, 0) : 0;
    if (dispIdx < 0) dispIdx = 0;
    swprintf(buf, 16, L"%d", dispIdx);
    Config().SetString(sec, L"DisplayIndex", buf);

    // Per-window checkboxes
    for (const LayoutEntry& w : BuildLayoutWindows(m_pEngine)) {
        wchar_t key[64];
        swprintf(key, 64, L"Window_%ls", w.name.c_str());
        bool checked = IsChecked(w.checkboxID);
        Config().SetString(sec, key, checked ? L"1" : L"0");
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
        IDC_MW_WSLAYOUT_MODE_CORNER, x + 8, y, w - 16, lineH, hFont, true, true, true, IDC_MW_WSLAYOUT_MODE_CORNER));
    y += lineH + 2;

    // Corner sub-options (indented)
    int rbw = (w - 40) / 2;
    TrackControl(CreateRadio(hw, L"Top-Left",     IDC_MW_WSLAYOUT_CORNER_TL, x + 24,          y, rbw, lineH, hFont, false, true,  true, IDC_MW_WSLAYOUT_CORNER_TL));
    TrackControl(CreateRadio(hw, L"Top-Right",    IDC_MW_WSLAYOUT_CORNER_TR, x + 24 + rbw + 8, y, rbw, lineH, hFont, true,  false, true, IDC_MW_WSLAYOUT_CORNER_TL));
    y += lineH + 2;
    TrackControl(CreateRadio(hw, L"Bottom-Left",  IDC_MW_WSLAYOUT_CORNER_BL, x + 24,          y, rbw, lineH, hFont, false, false, true, IDC_MW_WSLAYOUT_CORNER_TL));
    TrackControl(CreateRadio(hw, L"Bottom-Right", IDC_MW_WSLAYOUT_CORNER_BR, x + 24 + rbw + 8, y, rbw, lineH, hFont, false, false, true, IDC_MW_WSLAYOUT_CORNER_TL));
    y += lineH + 4;

    // Size slider (indented)
    TrackControl(CreateLabel(hw, L"Size:", x + MulDiv(24, lineH, 26), y,
                             MulDiv(40, lineH, 26), lineH, hFont));
    HWND hSizeLbl = CreateLabel(hw, L"20%", x + 68, y, 40, lineH, hFont);
    TrackControl(hSizeLbl);
    SetWindowLongPtrW(hSizeLbl, GWLP_ID, IDC_MW_WSLAYOUT_SIZE_LABEL);
    y += lineH + 2;
    TrackControl(CreateSlider(hw, IDC_MW_WSLAYOUT_SIZE_SLIDER, x + 24, y, w - 32, lineH, 10, 40, 20));
    y += lineH + gap + 2;

    // Fullscreen on separate display mode
    TrackControl(CreateRadio(hw, L"Fullscreen on separate display",
        IDC_MW_WSLAYOUT_MODE_DISPLAY, x + 8, y, w - 16, lineH, hFont, false, false, true, IDC_MW_WSLAYOUT_MODE_CORNER));
    y += lineH + 4;

    // Display combo (indented)
    HWND hDisplayCombo = CreateCombo(hw, IDC_MW_WSLAYOUT_DISPLAY_COMBO,
        x + 24, y, w - 32, lineH * 6, hFont);
    TrackControl(hDisplayCombo);

    // Populate display combo
    std::vector<MonitorEntry> monitors;
    EnumDisplayMonitors(NULL, NULL, EnumMonCB, reinterpret_cast<LPARAM>(&monitors));
    for (auto& m : monitors)
        SendMessageW(hDisplayCombo, CB_ADDSTRING, 0, (LPARAM)m.name);
    if (!monitors.empty())
        SendMessageW(hDisplayCombo, CB_SETCURSEL, 0, 0);

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

    {
        // The derived list is more than twice as long as the hand-written one
        // it replaced, so it wants two columns -- but only when there is room.
        // A saved window can be much narrower than the 420 default (this one
        // was found at 257), and forcing two columns there clips the names
        // rather than showing more of them. Adapt instead of resizing the
        // user's window out from under them.
        const std::vector<LayoutEntry> windows = BuildLayoutWindows(m_pEngine);
        const int minTwoColumnWidth = MulDiv(380, lineH, 26);
        const int cols = (w >= minTwoColumnWidth) ? 2 : 1;
        const int colW = (w - 16) / cols;
        const int rows = ((int)windows.size() + cols - 1) / cols;
        for (size_t i = 0; i < windows.size(); i++) {
            const int col = (int)i / (rows > 0 ? rows : 1);
            const int row = (int)i % (rows > 0 ? rows : 1);
            TrackControl(CreateCheck(hw, windows[i].name.c_str(),
                windows[i].checkboxID, x + 8 + col * colW,
                y + row * (lineH + 2), colW - 8, lineH, hFont,
                windows[i].defaultOn));
        }
        y += rows * (lineH + 2);
    }
    y += gap;

    // ── Buttons ──
    int btnW = (w - 8) / 2;
    int btnH = lineH + 10;
    TrackControl(CreateBtn(hw, L"Apply Layout", IDC_MW_WSLAYOUT_APPLY, x, y, btnW, btnH, hFont));
    TrackControl(CreateBtn(hw, L"Reset Defaults", IDC_MW_WSLAYOUT_RESET, x + btnW + 8, y, btnW, btnH, hFont));

    // Load saved preferences (overrides defaults)
    LoadLayoutPrefs();

    // Auto-apply if requested (e.g. from ACTION=ApplyWorkspaceLayout)
    if (m_bAutoApply) {
        m_bAutoApply = false;
        ApplyLayout();
    }
}

// ── Open a tool window by checkbox ID and return its ToolWindow* ──


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
        HWND hDisplayCombo = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO);
        int sel = hDisplayCombo ? (int)SendMessageW(hDisplayCombo, CB_GETCURSEL, 0, 0) : 0;
        if (sel < 0) sel = 0;
        if (sel < (int)monitors.size()) {
            RECT& rc = monitors[sel].rc;
            int mw = rc.right - rc.left;
            int mh = rc.bottom - rc.top;

            if (hRender && IsWindow(hRender)) {
                // Save current style then go borderless fullscreen on that monitor
                SetWindowLongPtrW(hRender, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                // Unreachable in a child today -- tool windows are refused in
                // child mode -- but the rule lives in one place so it cannot
                // drift if that ever changes.
                SetWindowLongPtrW(hRender, GWL_EXSTYLE, RenderWindowTaskbarExStyle(p->m_bChildMode));
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

    for (const LayoutEntry& win : BuildLayoutWindows(p)) {
        bool chk = IsChecked(win.checkboxID);
        DebugLogAFmt("WorkspaceLayout: checkbox %ls (id=%d) checked=%d",
            win.name.c_str(), win.checkboxID, (int)chk);
        if (chk) {
            ToolWindow* tw = ToolWindowForAction(p, win.hotkeyAction, true);
            DebugLogAFmt("WorkspaceLayout: ToolWindowForAction returned %p", tw);
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
    HWND hDisplayCombo = GetDlgItem(m_hWnd, IDC_MW_WSLAYOUT_DISPLAY_COMBO);
    if (hDisplayCombo) SendMessageW(hDisplayCombo, CB_SETCURSEL, 0, 0);

    // Checkboxes: reset to defaults
    for (const LayoutEntry& w : BuildLayoutWindows(m_pEngine))
        SetChecked(w.checkboxID, w.defaultOn);

    UpdateModeState();
}

// ── Command Handler ──

LRESULT WorkspaceLayoutWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    if (code != BN_CLICKED) return -1;

    // Checkbox and radio state is auto-toggled by the base class before DoCommand.

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
