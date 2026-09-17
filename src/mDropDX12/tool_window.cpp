/*
  Plugin module: ToolWindow Base Class
  Base implementation for the 20+ standalone tool windows on own threads.
  Contains: Window lifecycle (Open/Close/CreateOnThread + message pump),
            position/open-state persistence (LoadWindowPosition,
            SaveWindowPosition, EnsureOnRenderMonitor), font sync +
            rebuild, dark theme painting (HandleDarkCtlColor,
            HandleDarkDrawItem, DarkTabSubclassProc), ModalDialog, base
            controls + tab pages + layout (BuildBaseControls, ShowPage,
            FitToContents), anchors + rebuild state capture/restore,
            BaseWndProc with SEH context, shared dialogs (PromptForName,
            ShowActionEditDialog, ShowShaderErrorDialog)
*/

#include "tool_window.h"
#include "seh_guard.h"
#include <map>
#include <mutex>
#include "engine.h"
#include <algorithm>
#include "engine_helpers.h"
#include "utility.h"
#include "json_utils.h"
#include "format_to.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include "config_store.h"
#pragma comment(lib, "dwmapi.lib")

//----------------------------------------------------------------------
// Undocumented uxtheme dark mode APIs (Windows 10 1903+ / build 18362+)
// Used by Notepad++, Windows Terminal, VS Code, etc. for dark popup menus.
//----------------------------------------------------------------------
enum PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
using fnFlushMenuThemes = void(WINAPI*)();
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);

static fnSetPreferredAppMode  pSetPreferredAppMode = nullptr;
static fnFlushMenuThemes      pFlushMenuThemes = nullptr;
static fnAllowDarkModeForWindow pAllowDarkModeForWindow = nullptr;
static bool s_bDarkAPIsResolved = false;

static void ResolveDarkModeAPIs() {
    if (s_bDarkAPIsResolved) return;
    s_bDarkAPIsResolved = true;
    HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");
    if (!hUx) return;
    pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
    pFlushMenuThemes = (fnFlushMenuThemes)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
    pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
}

namespace mdrop {

extern Engine g_engine;

//----------------------------------------------------------------------
// Constructor / Destructor
//----------------------------------------------------------------------

ToolWindow::ToolWindow(Engine* pEngine, int defaultW, int defaultH)
  : m_pEngine(pEngine), m_nDefaultW(defaultW), m_nDefaultH(defaultH),
    m_nWndW(defaultW), m_nWndH(defaultH)
{
  if (m_pEngine)
    m_pEngine->m_toolWindows.push_back(this);
}

ToolWindow::~ToolWindow() {
  Close();
  if (m_pEngine) {
    auto& v = m_pEngine->m_toolWindows;
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
  }
}

//----------------------------------------------------------------------
// Open / Close
//----------------------------------------------------------------------

void ToolWindow::OnAlreadyOpen() {
  // Post to the tool window's own thread so SetWindowPos runs on the owning thread.
  // Cross-thread SetWindowPos/SetForegroundWindow can silently fail.
  PostMessage(m_hWnd, WM_MW_BRING_TO_TOP, 0, 0);
}

DWORD ToolWindow::GetCommonControlFlags() const {
  return ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_TAB_CLASSES;
}

void ToolWindow::Open() {
  // A child instance is a render surface driven over IPC, not a UI. Every tool
  // window would open on whichever display the child owns, steal focus from the
  // parent (ToolWindow::Open calls SetForegroundWindow), and offer controls that
  // write settings the child's testing-mode shield then discards. Refused at the
  // one door they all come through, rather than at twenty call sites.
  if (m_pEngine && m_pEngine->m_bChildMode) return;

  if (m_hWnd && IsWindow(m_hWnd)) {
    OnAlreadyOpen();
    return;
  }
  if (m_bThreadRunning.load()) return;

  if (m_thread.joinable())
    m_thread.join();

  m_thread = std::thread(&ToolWindow::CreateOnThread, this);
}

void ToolWindow::Close() {
  SignalClose();
  WaitClose();
}

void ToolWindow::RequestLoadPreset(int nPresetIndex, const wchar_t* szPath,
                                   float fBlendTime) {
  if (!m_pEngine) return;
  RenderCommand cmd;
  cmd.cmd = RenderCmd::LoadPresetPath;
  cmd.iParam1 = nPresetIndex;
  cmd.fParam = (fBlendTime >= 0.0f) ? fBlendTime : m_pEngine->m_fBlendTimeUser;
  if (szPath && szPath[0])
    cmd.sParam = szPath;
  m_pEngine->EnqueueRenderCmd(std::move(cmd));
}

void ToolWindow::RequestNavPreset(int nDirection, float fBlendTime) {
  if (!m_pEngine) return;
  RenderCommand cmd;
  cmd.cmd = RenderCmd::NavPreset;
  cmd.iParam1 = nDirection;
  cmd.fParam = (fBlendTime >= 0.0f) ? fBlendTime : m_pEngine->m_fBlendTimeUser;
  m_pEngine->EnqueueRenderCmd(std::move(cmd));
}

void ToolWindow::SignalClose() {
  if (m_hWnd && IsWindow(m_hWnd))
    PostMessage(m_hWnd, WM_CLOSE, 0, 0);
}

void ToolWindow::WaitClose() {
  if (m_thread.joinable())
    m_thread.join();
}

bool ToolWindow::IsOpen() const {
  return m_hWnd && IsWindow(m_hWnd);
}

//----------------------------------------------------------------------
// Font sync broadcast — notifies all windows except the sender
//----------------------------------------------------------------------

void Engine::BroadcastFontSync(HWND hSender) {
  for (auto* tw : m_toolWindows) {
    if (tw->IsOpen() && tw->GetHWND() != hSender)
      PostMessage(tw->GetHWND(), WM_MW_REBUILD_FONTS, 0, 0);
  }
}

// ── Tool window geometry: resources/windows.json ────────────────────────
//
// Fifteen [*Wnd] sections and 84 keys came out of settings.ini for this --
// every tool window's size, position, always-on-top, active tab and
// was-open-last-session. None of it is a setting anybody edits; it is where the
// windows happen to be sitting, and it was a seventh of the file.
//
// Its own store rather than another ConfigStore file because the goal is fewer
// INI sections, not the same sections in a second INI. The shape is a map of
// window -> a handful of ints, which is a json object and nothing more.
//
// THREADING. Every tool window runs its own thread and they all write here, so
// unlike hotkeys.json this needs a lock. One mutex around the whole store is
// ample: these writes happen when a window moves, closes or changes tab, not
// per frame and not per pixel.
//
// The write shield is honoured the same way MixerSettingsStore honours it --
// testing mode must leave no trace, and a sweep that opens tool windows would
// otherwise rewrite where the user's windows live.
namespace {

// json_utils has no "get or create this member" helper -- vfx_profile_store
// keeps its own for the same reason. Small enough to have twice; a shared one
// would mean a header change for two callers.
JsonValue& Member(JsonValue& obj, const wchar_t* key) {
    obj.type = JsonValue::Object;
    for (auto& kv : obj.members)
        if (kv.first == key) return kv.second;
    obj.members.push_back({ key, JsonValue() });
    return obj.members.back().second;
}

class WindowStore {
public:
    static WindowStore& I() { static WindowStore s; return s; }

    void SetDir(const std::wstring& resourceDir) {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_dir == resourceDir) return;
        m_dir = resourceDir;
        m_loaded = false;
    }

    bool Has(const wchar_t* sec, const wchar_t* key) {
        std::lock_guard<std::mutex> lk(m_mx);
        Load();
        const JsonValue& w = m_root[L"windows"][sec];
        return w.isObject() && w.has(key);
    }

    int GetInt(const wchar_t* sec, const wchar_t* key, int def) {
        std::lock_guard<std::mutex> lk(m_mx);
        Load();
        const JsonValue& w = m_root[L"windows"][sec];
        if (!w.isObject() || !w.has(key)) return def;
        return w[key].asInt(def);
    }

    void SetInt(const wchar_t* sec, const wchar_t* key, int value) {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            Load();
            JsonValue& windows = Member(m_root, L"windows");
            windows.type = JsonValue::Object;
            JsonValue& one = Member(windows, sec);
            one.type = JsonValue::Object;
            Member(one, key) = JsonValue(value);
        }
        Save();
    }

    // The seven keys this store owns. Named explicitly because a tool window's
    // INI section is NOT always only geometry: [VideoFX] carries PosX and PosY
    // beside forty video-effect parameters, so moving a whole section would
    // take Scale, Rotation and every tint with it.
    static const wchar_t* const kGeomKeys[7];

    // Move one window's geometry out of settings.ini, the first time that
    // window asks for it.
    //
    // Replaces a startup sweep that matched sections by a "Wnd" suffix and so
    // missed seven windows whose sections are named otherwise -- Displays,
    // SongInfo, VideoFX, VFXProfiles, CustomShaders, ShaderEditor,
    // ShaderImport. Their geometry stayed in the INI while the loader had
    // already switched to this store, so they came up at their default size
    // with controls cut off the bottom. Asking per window needs no list of
    // window names and cannot miss one.
    void MigrateSection(const wchar_t* sec) {
        if (!sec || !*sec) return;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            Load();
            // Already here: this store is the newer of the two.
            const JsonValue& have = m_root[L"windows"][sec];
            if (have.isObject() && have.members.size()) return;
        }
        bool any = false;
        for (const wchar_t* key : kGeomKeys)
            if (Config().Has(sec, key)) { any = true; break; }
        if (!any) return;

        {
            std::lock_guard<std::mutex> lk(m_mx);
            Load();
            JsonValue& windows = Member(m_root, L"windows");
            windows.type = JsonValue::Object;
            JsonValue& one = Member(windows, sec);
            one.type = JsonValue::Object;
            for (const wchar_t* key : kGeomKeys) {
                if (!Config().Has(sec, key)) continue;
                // GetSignedInt: PosX and PosY go negative on a monitor left of
                // or above the primary -- Shane's Displays window sat at
                // -1770,-25 -- and GetInt reports zero for a negative.
                Member(one, key) = JsonValue(Config().GetSignedInt(sec, key, 0));
            }
        }
        if (!Save()) return;

        // Read back before removing anything, then remove only OUR keys. The
        // section itself may still be wanted: [VideoFX] keeps its parameters.
        for (const wchar_t* key : kGeomKeys) {
            if (!Config().Has(sec, key)) continue;
            if (GetInt(sec, key, INT_MIN) == Config().GetSignedInt(sec, key, 0)) continue;
            DLOG_ERROR("windows: [%ls]/%ls did not survive; keeping the INI keys",
                       sec, key);
            return;
        }
        for (const wchar_t* key : kGeomKeys) Config().Remove(sec, key);
        DLOG_INFO("windows: moved [%ls] geometry out of settings.ini", sec);
    }

    // Takes the [*Wnd] sections out of settings.ini, once. Called with the
    // section names because ConfigStore is the only thing that can enumerate
    // them, and this file has no business knowing which windows exist.
    void MigrateFrom(const std::vector<std::wstring>& sections) {
        if (sections.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            Load();
            JsonValue& windows = Member(m_root, L"windows");
            windows.type = JsonValue::Object;
            for (const std::wstring& sec : sections) {
                // A window already in the json wins: it is newer than anything
                // still sitting in the INI.
                if (windows.has(sec.c_str())) continue;
                JsonValue& one = Member(windows, sec.c_str());
                one.type = JsonValue::Object;
                for (const wchar_t* key : kGeomKeys) {
                    if (!Config().Has(sec.c_str(), key)) continue;
                    // GetSignedInt throughout: PosX/PosY go negative on a
                    // monitor left of or above the primary, and GetInt is
                    // bug-compatible with GetPrivateProfileIntW, which reports
                    // zero for a negative. Migrating with it would walk every
                    // left-hand-screen window onto the primary.
                    Member(one, key) =
                        JsonValue(Config().GetSignedInt(sec.c_str(), key, 0));
                }
            }
        }
        if (!Save()) {
            DLOG_ERROR("windows: could not write the store; keeping the INI sections");
            return;
        }
        // Read back before removing anything.
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_loaded = false;
            Load();
            for (const std::wstring& sec : sections) {
                if (m_root[L"windows"].has(sec.c_str())) continue;
                DLOG_ERROR("windows: [%ls] did not survive; keeping the INI sections",
                           sec.c_str());
                return;
            }
        }
        for (const std::wstring& sec : sections) Config().RemoveSection(sec.c_str());
        ConfigFlushAll();
        DLOG_INFO("windows: moved %zu section(s) out of settings.ini", sections.size());
    }

private:
    void Load() {                       // call with m_mx held
        if (m_loaded) return;
        m_loaded = true;
        m_root = JsonValue();
        m_root.type = JsonValue::Object;
        if (m_dir.empty()) return;
        try {
            JsonValue v = JsonLoadFile(Path().c_str());
            if (v.isObject()) m_root = v;
        } catch (...) {
            DLOG_ERROR("windows: %ls is malformed; starting empty", Path().c_str());
        }
    }

    bool Save() {
        // Testing mode leaves no trace, so a sweep that opens tool windows
        // cannot move where the user's windows live.
        if (IsConfigWriteShielded()) return true;
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_dir.empty()) return false;
        JsonWriter w;
        w.BeginObject();
        w.Int(L"version", 1);
        w.Value(L"windows", m_root[L"windows"]);
        w.EndObject();
        return w.SaveToFile(Path().c_str());
    }

    std::wstring Path() const {
        std::wstring p = m_dir;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        return p + L"windows.json";
    }

    std::mutex   m_mx;
    std::wstring m_dir;
    JsonValue    m_root;
    bool         m_loaded = false;
};

const wchar_t* const WindowStore::kGeomKeys[7] = {
    L"WndW", L"WndH", L"PosX", L"PosY", L"OnTop", L"ActiveTab", L"WasOpen"
};

}  // namespace

void ToolWindow::InitWindowStore(const wchar_t* resourceDir) {
    WindowStore::I().SetDir(resourceDir ? resourceDir : L"");

    // Every [*Wnd] section still in settings.ini. Found by suffix rather than
    // from a list of windows: a window that no longer exists still has a
    // section, and leaving it behind would defeat the point of the move.
    std::vector<std::wstring> secs;
    for (const std::wstring& s : Config().SectionNames()) {
        if (s.size() > 3 && _wcsicmp(s.c_str() + s.size() - 3, L"Wnd") == 0)
            secs.push_back(s);
    }
    WindowStore::I().MigrateFrom(secs);
}

//----------------------------------------------------------------------
// Thread + Window Creation
//----------------------------------------------------------------------

void ToolWindow::EnsureOnRenderMonitor() {
    if (!m_hWnd || !IsWindow(m_hWnd)) return;
    if (!m_pEngine || !m_pEngine->m_bTestingMode) return;
    if (!PlacesItselfInTestingMode()) return;

    HWND hRender = m_pEngine->GetPluginWindow();
    if (!hRender || !IsWindow(hRender)) return;

    HMONITOR hWant = MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST);
    HMONITOR hHave = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
    if (hWant == hHave) return;

    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfo(hWant, &mi)) return;

    RECT rc{};
    GetWindowRect(m_hWnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const int kMargin = 16;
    int x = mi.rcWork.right  - w - kMargin;
    int y = mi.rcWork.bottom - h - kMargin;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;

    // NOACTIVATE and HWND_BOTTOM: this runs while a measurement may be in
    // progress, and taking the focus would corrupt it as surely as covering
    // the frame would.
    SetWindowPos(m_hWnd, HWND_BOTTOM, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

void ToolWindow::LoadWindowPosition() {
  const wchar_t* sec = GetINISection();
  // Before the first read, and a no-op afterwards. See MigrateSection: the
  // startup sweep matches a "Wnd" suffix and seven windows are not named that
  // way, which is how the Displays window came back at its default size.
  WindowStore::I().MigrateSection(sec);
  // The default is scaled to the font, exactly as FitToContents scales it
  // (issue 157). It is a constant chosen against a line height of 26 -- the
  // baseline every MulDiv(n, lineH, 26) in the layouts is relative to -- so at
  // a larger font a window opening at its raw default opens smaller than its
  // own contents. That is how the Controller window came up with its only
  // three buttons, Defaults/Save/Load, sitting below the client bottom where
  // no click could reach them.
  //
  // Only the DEFAULT. A size in the store is the size the user left the window
  // at and is used exactly as written; the minimum below is what protects it.
  const int lineH = GetLineHeight();   // no window yet: measured from the font
  m_nWndW = WindowStore::I().GetInt(sec, L"WndW", MulDiv(m_nDefaultW, lineH, 26));
  m_nWndH = WindowStore::I().GetInt(sec, L"WndH", MulDiv(m_nDefaultH, lineH, 26));

  // A window that places ITSELF in testing mode also SIZES itself: it comes up
  // at its default, not at whatever size the user last left it.
  //
  // The saved size was being restored even while the saved POSITION was being
  // overridden, so a window the user had grown to fill a screen came back at
  // that size in the corner of the render display -- which is not a corner, it
  // is most of the monitor, and it covers the frame being measured. The point
  // of the corner placement is a window that can be watched during a run
  // without sitting on top of the thing being watched, and a restored size
  // defeats it entirely.
  //
  // Same asymmetry as the position, and for the same reason: testing mode
  // READS nothing it will not honour and WRITES nothing at all, so this cannot
  // turn a test's dimensions into the user's permanent ones. Outside testing
  // the remembered size is kept exactly as before.
  //
  // Only for windows that opt in via PlacesItselfInTestingMode -- today the
  // Remote window. A window that merely centres on the render monitor keeps
  // its size, because it was not asked to stay out of the way.
  if (m_pEngine && m_pEngine->m_bTestingMode && PlacesItselfInTestingMode()) {
    m_nWndW = MulDiv(m_nDefaultW, lineH, 26);
    m_nWndH = MulDiv(m_nDefaultH, lineH, 26);
  }
  // GetSignedInt, not GetInt: the plain one is bug-compatible with
  // GetPrivateProfileIntW, which reports zero for any negative value, and a
  // monitor placed left of or above the primary gives windows negative
  // coordinates. Saving x=-1800 and reading back 0 is how a window on the
  // left-hand screen jumps to the primary one on restart.
  //
  // INT_MIN is "never saved". Plain -1 cannot mean that here for the same
  // reason: it is a real coordinate.
  auto readPos = [&](const wchar_t* key) -> int {
    // json keeps the sign, so there is no GetSignedInt counterpart to need:
    // the INI reader had one only because GetPrivateProfileIntW reports zero
    // for a negative, which walked left-hand-screen windows onto the primary.
    return WindowStore::I().Has(sec, key)
               ? WindowStore::I().GetInt(sec, key, INT_MIN)
               : INT_MIN;
  };
  m_nPosX = readPos(L"PosX");
  m_nPosY = readPos(L"PosY");
  m_bOnTop = WindowStore::I().GetInt(sec, L"OnTop", 1) != 0; // default sticky
  m_bWasOpenLastSession = WindowStore::I().GetInt(sec, L"WasOpen", 0) != 0;
  // The DECLARED minimum, not the font-scaled one.
  //
  // Scaling here was tried and measured, and it is too much: it applies to a
  // size read from the store, which is the size the user left the window at. Of
  // the eleven windows that run resized, eight had no clipped control at all --
  // among them the Audio Mixer, taken from 640 wide to 908, and that window
  // draws as many faders as fit, so its width is a layout decision of the
  // user's. A minimum that moves a window nobody complained about is not a
  // repair.
  //
  // What issue 157 actually needs is above (the DEFAULT is scaled, because a
  // default is our number and not his) and in FitClippedContentsOnOpen (which
  // moves an edge only when something is genuinely outside it).
  if (m_nWndW < GetMinWidth()) m_nWndW = GetMinWidth();
  if (m_nWndH < GetMinHeight()) m_nWndH = GetMinHeight();

  // The saved position may name a monitor that is no longer attached, or one
  // whose coordinates moved when the desktop was rearranged. Either way the
  // window would open somewhere the user cannot reach it.
  if (m_nPosX != INT_MIN && m_nPosY != INT_MIN)
    ClampToVisibleMonitor(m_nPosX, m_nPosY, m_nWndW, m_nWndH);
}

// Pull a saved window rect back onto a monitor that is actually attached.
//
// Two things put a window somewhere unusable: a display being unplugged (its
// coordinates stop belonging to any monitor at all) and the desktop being
// rearranged so a window that fitted now hangs over the edge of every screen.
// MONITOR_DEFAULTTONEAREST answers with a real monitor in both cases, so the
// window lands on the closest surviving display rather than always the primary.
void ToolWindow::ClampToVisibleMonitor(int& posX, int& posY, int w, int h)
{
  RECT rcWnd = { posX, posY, posX + w, posY + h };

  HMONITOR hMon = MonitorFromRect(&rcWnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  if (!hMon || !GetMonitorInfo(hMon, &mi)) {
    posX = INT_MIN;   // no monitor info at all: fall back to the centring path
    posY = INT_MIN;
    return;
  }

  const int workW = mi.rcWork.right - mi.rcWork.left;
  const int workH = mi.rcWork.bottom - mi.rcWork.top;

  // A window wider or taller than the work area cannot be contained; centre
  // that axis instead so at least the middle of it is reachable.
  const int newX = (w >= workW) ? mi.rcWork.left + (workW - w) / 2
                                : min(max(posX, mi.rcWork.left), mi.rcWork.right - w);
  const int newY = (h >= workH) ? mi.rcWork.top + (workH - h) / 2
                                : min(max(posY, mi.rcWork.top), mi.rcWork.bottom - h);

  if (newX != posX || newY != posY) {
    DLOG_INFO("ToolWindow: saved position %d,%d was off the attached displays, moved to %d,%d",
              posX, posY, newX, newY);
    posX = newX;
    posY = newY;
  }
}

void ToolWindow::SaveOpenState(bool open) {
  // Whether the user has this window open, so a later run can tell a position
  // they are living with from one left behind by a window they closed.
  //
  // Behind the same shield as SaveWindowPosition, and for the same reason: a
  // test that opens a window must not leave the app believing the user did.
  // Testing mode therefore READS WasOpen and never writes it.
  if (m_pEngine->m_bTestingMode || m_bPlacedByTestingMode) return;
  WindowStore::I().SetInt(GetINISection(), L"WasOpen", open ? 1 : 0);
}

void ToolWindow::SaveWindowPosition() {
  if (!m_hWnd) return;

  // A test run must not repossess the user's window layout.
  //
  // This is called on drag-end and on close, so a harness that opens a tool
  // window, moves it somewhere convenient and closes it was overwriting the
  // remembered position permanently -- the user came back to windows parked
  // wherever the last test left them, on whichever display it used, and had to
  // put every one of them back by hand.
  //
  // Testing mode positions tool windows on the render window's monitor (see
  // Open) precisely because that placement is disposable. Persisting it would
  // make it permanent, which is the opposite of the point.
  // Checked BOTH at save time and at placement time.
  //
  // Testing mode alone was not enough. A window opened under testing mode is
  // moved to the render monitor, and if it is still open when testing mode
  // ends -- a suite that opens a window and never closes it -- then whatever
  // closes it later saves that disposable placement over the real one. Shane
  // lost the Settings window's position and its SIZE that way: the size came
  // back multiplied by 1.5, the DPI ratio between the monitor he keeps it on
  // and the one a test had centred it on.
  //
  // So a window placed by testing mode never saves its position again, for as
  // long as it lives.
  if (m_pEngine->m_bTestingMode || m_bPlacedByTestingMode) return;

  const wchar_t* sec = GetINISection();
  RECT rc;
  // A maximised window must not have its maximised geometry persisted as the
  // restored size -- reopening would then come up filling the monitor with no
  // way back. GetWindowPlacement reports the restored rect either way.
  WINDOWPLACEMENT wp = { sizeof(wp) };
  if (IsZoomed(m_hWnd) && GetWindowPlacement(m_hWnd, &wp))
    rc = wp.rcNormalPosition;
  else
    GetWindowRect(m_hWnd, &rc);
  WindowStore::I().SetInt(sec, L"WndW", (int)(rc.right - rc.left));
  WindowStore::I().SetInt(sec, L"WndH", (int)(rc.bottom - rc.top));
  WindowStore::I().SetInt(sec, L"PosX", (int)rc.left);
  WindowStore::I().SetInt(sec, L"PosY", (int)rc.top);
  WindowStore::I().SetInt(sec, L"OnTop", m_bOnTop ? 1 : 0);
}

void ToolWindow::CreateOnThread() {
  m_bThreadRunning.store(true);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  // Register window class (idempotent — RegisterClassEx fails silently if already registered)
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = BaseWndProc;
  wc.hInstance = GetModuleHandle(NULL);
  wc.lpszClassName = GetWindowClass();
  wc.hbrBackground = m_pEngine->IsDarkTheme()
    ? CreateSolidBrush(m_pEngine->m_colSettingsBg)
    : (HBRUSH)(COLOR_BTNFACE + 1);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  RegisterClassExW(&wc);

  // Init common controls
  INITCOMMONCONTROLSEX icex = { sizeof(icex), GetCommonControlFlags() };
  InitCommonControlsEx(&icex);

  // Ensure theme brushes are ready
  m_pEngine->LoadSettingsThemeFromINI();

  // Load persisted size/position
  LoadWindowPosition();

  // Centre on the monitor the RENDER window is on. SM_CXSCREEN is the PRIMARY
  // monitor's size, so it puts the window on the main display no matter where
  // the visualizer is.
  auto CenterOnRenderMonitor = [&](int& outX, int& outY) {
    HWND hRender = m_pEngine->GetPluginWindow();
    HMONITOR hMon = hRender ? MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST)
                            : MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
      outX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - m_nWndW) / 2;
      outY = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - m_nWndH) / 2;
    } else {
      outX = (GetSystemMetrics(SM_CXSCREEN) - m_nWndW) / 2;
      outY = (GetSystemMetrics(SM_CYSCREEN) - m_nWndH) / 2;
    }
  };

  // The LOWER RIGHT of the same monitor, inset by a margin.
  //
  // Right rather than left because MDropDX12 draws its HUD -- preset name,
  // FPS, notifications -- down the left, and lower rather than upper for the
  // same reason. The point is a window that can be watched during a run
  // without sitting on top of the thing being watched.
  auto CornerOfRenderMonitor = [&](int& outX, int& outY) {
    HWND hRender = m_pEngine->GetPluginWindow();
    HMONITOR hMon = hRender ? MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST)
                            : MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfo(hMon, &mi)) {
      CenterOnRenderMonitor(outX, outY);
      return;
    }
    const int kMargin = 16;
    outX = mi.rcWork.right  - m_nWndW - kMargin;
    outY = mi.rcWork.bottom - m_nWndH - kMargin;
    // A window taller or wider than the work area would be pushed off the top
    // or the left, which is worse than overlapping the corner.
    if (outX < mi.rcWork.left) outX = mi.rcWork.left;
    if (outY < mi.rcWork.top)  outY = mi.rcWork.top;
  };

  // Where a tool window opens.
  //
  // Normally the remembered position wins -- windows stay where they were left,
  // which is the whole point of sticky positions.
  //
  // In TESTING MODE they open on the render window's monitor instead, and
  // SaveWindowPosition refuses to write, so a test run keeps its windows
  // together with the thing being measured and gives the layout back untouched
  // when it ends. A harness that had to drag windows around itself was the
  // reason they kept ending up on the wrong display and staying there.
  //
  // A window that PlacesItselfInTestingMode (the Remote window) keeps its
  // position under testing if it was OPEN when the app was last used, and
  // otherwise opens in the lower corner. Open-at-last-exit rather than
  // has-a-saved-position, because those differ and the difference is the
  // point: a stale position from a window closed weeks ago is not consent to
  // put it back on screen, while a window left open is -- its owner does not
  // mind it appearing and disappearing there during a run. Note the asymmetry that makes this
  // safe: the saved position is READ and never written -- SaveWindowPosition
  // still refuses while testing mode is on -- so honouring it cannot turn a
  // test's placement into a permanent one. Suppressing the write is what keeps
  // a run from rearranging the desktop; suppressing the read as well would
  // throw away an arrangement the user made on purpose.
  const bool testing = m_pEngine->m_bTestingMode;
  const bool ownPlacement = PlacesItselfInTestingMode();
  const bool haveSaved = (m_nPosX != INT_MIN && m_nPosY != INT_MIN);

  // A saved position is only honoured if it is still on the RENDER window's
  // monitor.
  //
  // Sticky positions are the point, but they are sticky to a DISPLAY that may
  // not be there any more -- or may no longer be the one the visualizer is on.
  // Restoring one blindly strands the window on another screen, which is
  // exactly what was reported: the Remote window opening away from the render
  // output, where it is no use to anyone watching the frame.
  //
  // Same-monitor rather than merely on-screen: a position can be perfectly
  // valid, fully visible, and still on the wrong display. Within one monitor
  // the remembered spot is kept exactly, so nothing about ordinary use changes.
  auto SavedIsOnRenderMonitor = [&]() -> bool {
    HWND hRender = m_pEngine->GetPluginWindow();
    if (!hRender) return true;   // nothing to be away from
    POINT pt{ m_nPosX + m_nWndW / 2, m_nPosY + m_nWndH / 2 };
    HMONITOR hSaved = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (!hSaved) return false;   // off every display: treat as stale
    return hSaved == MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST);
  };

  int posX, posY;
  bool cornerPlaced = false;
  if (haveSaved && SavedIsOnRenderMonitor() &&
      (!testing || (ownPlacement && m_bWasOpenLastSession))) {
    posX = m_nPosX;
    posY = m_nPosY;
  } else if ((testing && ownPlacement) || (haveSaved && ownPlacement)) {
    // Its saved spot is on another display. The lower corner of the render
    // monitor is where this window is wanted -- beside the output rather than
    // over it, since the HUD draws down the left.
    CornerOfRenderMonitor(posX, posY);
    m_bPlacedByTestingMode = true;
    cornerPlaced = true;
  } else {
    CenterOnRenderMonitor(posX, posY);
    // Remembered for the window's lifetime: this placement is disposable and
    // must never be written back, even if testing mode ends first.
    m_bPlacedByTestingMode = testing;
  }

  // If the render window is TOPMOST (fullscreen/borderless/spanning), create the
  // tool window TOPMOST too so it appears above the render surface.
  bool renderIsTopmost = false;
  {
    HWND hRender = m_pEngine->GetPluginWindow();
    renderIsTopmost = hRender &&
        (GetWindowLongW(hRender, GWL_EXSTYLE) & WS_EX_TOPMOST);
  }

  DWORD exStyle = UsesToolWindowFrame() ? WS_EX_TOOLWINDOW : 0;
  if (m_bOnTop || renderIsTopmost) exStyle |= WS_EX_TOPMOST;

  m_hWnd = CreateWindowExW(
    exStyle,
    GetWindowClass(), GetWindowTitle(),
    // WS_CLIPCHILDREN: the frame stops painting the ground under its controls,
    // which is where resize flicker comes from -- the background is drawn, then
    // each control paints over it.
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN |
    GetExtraWindowStyle(),
    posX, posY, m_nWndW, m_nWndH,
    NULL, NULL, GetModuleHandle(NULL), (LPVOID)this);

  // Recorded on create rather than at shutdown: nothing closes tool windows
  // when the program exits, and a kill or a crash never gets the chance, so
  // there is no reliable moment later at which to write it.
  if (m_hWnd) SaveOpenState(true);

  if (!m_hWnd) {
    CoUninitialize();
    m_bThreadRunning.store(false);
    return;
  }

  if (AcceptsDragDrop())
    DragAcceptFiles(m_hWnd, TRUE);

  DoBuildControls();
  m_bFirstBuild = false;
  ApplyDarkTheme();

  // A control someone asked for while this window was still being built.
  if (m_pendingShowCtrl) {
    PostMessage(m_hWnd, WM_MW_SHOW_CTRL, (WPARAM)m_pendingShowCtrl, 0);
    m_pendingShowCtrl = 0;
  }

  // Testing mode opens a window WITHOUT taking the focus. It still appears,
  // and it still appears where it normally would.
  //
  // Those two halves are separate, and only the focus was the problem. Shane
  // keeps using the machine while tests run, types at 80wpm and cannot use
  // speech input, so the keyboard is his only channel: a window that grabs it
  // mid-run does not merely interrupt him, it corrupts the measurement,
  // because his keystrokes land in the window under test. Being VISIBLE costs
  // him nothing -- "I don't mind seeing the window on the render screen it was
  // the stealing focus" -- so it is not hidden, not pushed to the back, and
  // not treated as something to sneak past him.
  //
  // SW_SHOWNOACTIVATE shows without activating, and the SetWindowPos calls
  // already pass SWP_NOACTIVATE, so dropping SetForegroundWindow is the whole
  // fix. Testing mode already treats such a window's placement as disposable;
  // its focus is disposable in exactly the same sense.
  const bool quiet = m_pEngine && m_pEngine->m_bTestingMode;

  ShowWindow(m_hWnd, quiet ? SW_SHOWNOACTIVATE : SW_SHOW);

  // Ensure we come to front even over topmost render window
  SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  if (!m_bOnTop && !renderIsTopmost)
    SetWindowPos(m_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  if (!quiet) SetForegroundWindow(m_hWnd);
  UpdateWindow(m_hWnd);

  // A window whose own controls do not fit inside it grows enough that they do
  // (issue 157).
  //
  // Its size came from the store or from a default constant, and neither knows
  // what font is in use; the layouts are all MulDiv(n, lineH, 26) and do. At a
  // large font that gap opened four windows with controls past their own edge
  // -- Text Animations 18 of them, Workspace Layout 14, Remote 4, Controller 4
  // -- and Controller's were its only three buttons, sitting below the client
  // bottom where no click could reach them. The "resize to fit" glyph in the
  // title row does exactly this repair, but it cannot be the answer to a window
  // that comes up unusable, because pressing it is a thing done from inside.
  //
  // Before the corner placement below, which measures the window: it has to see
  // the size the window ended up at.
  FitClippedContentsOnOpen();

  // Corner placement is applied AGAIN here, from the size the window actually
  // ended up with, and it has to be AFTER ShowWindow.
  //
  // CreateWindowExW is given m_nWndW/m_nWndH, but a window opening on a
  // monitor whose DPI differs from the one those were saved on is rescaled by
  // Windows -- measured here at 1154 wide becoming 769, two thirds -- and that
  // rescale lands on the show, not on the create. Cornering before it misses
  // by the difference. Centring never noticed, because a rescale about the
  // centre is still centred; a corner is not.
  //
  // Last, after the z-order calls and UpdateWindow: the frame is still being
  // adjusted while those run, and cornering before they finish landed the
  // window five pixels off in each axis. Nothing after this moves it.
  if (cornerPlaced) {
    RECT rc = {};
    if (GetWindowRect(m_hWnd, &rc)) {
      const int w = rc.right - rc.left, h = rc.bottom - rc.top;
      HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi = { sizeof(mi) };
      if (GetMonitorInfo(hMon, &mi)) {
        const int kMargin = 16;
        int x = mi.rcWork.right - w - kMargin;
        int y = mi.rcWork.bottom - h - kMargin;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top)  y = mi.rcWork.top;
        SetWindowPos(m_hWnd, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
  }


  // Own message pump on this thread
  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    // --- Keyboard forwarding to render window ---
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
      UINT vk = (UINT)msg.wParam;
      bool bCtrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      bool bShift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
      bool bAlt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;

      // Escape always closes the tool window
      if (vk == VK_ESCAPE && !bCtrl && !bAlt) {
        PostMessage(m_hWnd, WM_CLOSE, 0, 0);
        continue;
      }

      // Ctrl+F2 resets tool window position/size to defaults
      if (vk == VK_F2 && bCtrl) {
        ResetPosition();
        continue;
      }

      {
        bool isFKey = (vk >= VK_F1 && vk <= VK_F24);
        // ForwardAllKeys(): forward everything (windows with no text edits)
        // Otherwise: only F-keys and Ctrl/Alt combos (bare alphanumerics go to edits)
        bool shouldForward = ForwardAllKeys() || isFKey || bCtrl || bAlt;

        // Don't forward standard edit shortcuts when an edit control has focus
        // (Ctrl+A=SelectAll, Ctrl+C=Copy, Ctrl+X=Cut, Ctrl+V=Paste, Ctrl+Z=Undo)
        if (shouldForward && bCtrl && !bAlt && !bShift) {
          if (vk == 'A' || vk == 'C' || vk == 'X' || vk == 'V' || vk == 'Z') {
            HWND hFocus = GetFocus();
            if (hFocus) {
              wchar_t cls[16];
              GetClassNameW(hFocus, cls, 16);
              // "Scintilla" is the Preset Editor's code control. Without it,
              // Ctrl+A/C/X/V/Z went to the render window and copy, paste and
              // undo simply did nothing in the editor.
              //
              // "ComboBox" is here because an EDITABLE combo (CBS_DROPDOWN) is
              // two windows: the combo, and an Edit child that normally holds
              // the focus. Normally -- but not when the control was reached by
              // TAB, or while its list is dropped, when GetFocus returns the
              // COMBOBOX itself. Ctrl+V then went to the render window and the
              // paste silently did nothing, which is the reason editable combos
              // got a reputation for not accepting typed or pasted text.
              if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"RichEdit20W") == 0 ||
                  _wcsicmp(cls, L"Scintilla") == 0 || _wcsicmp(cls, L"ComboBox") == 0)
                shouldForward = false;
            }
          }
        }

        if (shouldForward) {
          HWND hRender = m_pEngine->GetPluginWindow();
          if (hRender) {
            PostMessage(hRender, msg.message, msg.wParam, msg.lParam);
            continue;
          }
        }
      }
    }
    if (!IsDialogMessage(m_hWnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  m_hWnd = NULL;
  CoUninitialize();
  m_bThreadRunning.store(false);
}

//----------------------------------------------------------------------
// Dark Theme
//----------------------------------------------------------------------

void ToolWindow::ApplyDarkTheme() {
  if (!m_hWnd) return;
  m_pEngine->LoadSettingsThemeFromINI();
  ApplyDarkThemeToWindow(m_pEngine, m_hWnd);
  ApplyDarkThemeToChildren(m_pEngine, m_childCtrls);
  RedrawWindow(m_hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

//----------------------------------------------------------------------
// ModalDialog — lightweight modal popup base class
//----------------------------------------------------------------------

bool ModalDialog::Show(HWND hParent, int clientW, int clientH) {
  m_hParent = hParent;

  // Register window class once
  WNDCLASSEXW wc = { sizeof(wc) };
  if (!GetClassInfoExW(GetModuleHandle(NULL), GetDialogClass(), &wc)) {
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ModalWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = m_pEngine->IsDarkTheme()
      ? CreateSolidBrush(m_pEngine->m_colSettingsBg)
      : (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = GetDialogClass();
    RegisterClassExW(&wc);
  }

  // Create font
  m_hFont = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  // Compute window rect from desired client area (DPI-aware)
  DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  DWORD dwExStyle = WS_EX_DLGMODALFRAME;
  UINT dpi = GetDpiForWindow(hParent);
  if (dpi == 0) dpi = 96;
  RECT rc = { 0, 0, clientW, clientH };
  AdjustWindowRectExForDpi(&rc, dwStyle, FALSE, dwExStyle, dpi);
  int wndW = rc.right - rc.left;
  int wndH = rc.bottom - rc.top;

  // Center on parent's monitor
  HMONITOR hMon = MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  GetMonitorInfo(hMon, &mi);
  int cx = (mi.rcWork.left + mi.rcWork.right - wndW) / 2;
  int cy = (mi.rcWork.top + mi.rcWork.bottom - wndH) / 2;

  m_hWnd = CreateWindowExW(dwExStyle, GetDialogClass(), GetDialogTitle(),
    dwStyle, cx, cy, wndW, wndH, hParent, NULL, GetModuleHandle(NULL), this);
  if (!m_hWnd) {
    if (m_hFont) { DeleteObject(m_hFont); m_hFont = NULL; }
    return false;
  }

  // Build controls
  DoBuildControls(clientW, clientH);

  // Apply dark theme
  m_pEngine->LoadSettingsThemeFromINI();
  ApplyDarkThemeToWindow(m_pEngine, m_hWnd);
  ApplyDarkThemeToChildren(m_pEngine, m_childCtrls);

  // Show and make modal
  ShowWindow(m_hWnd, SW_SHOW);
  RedrawWindow(m_hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
  EnableWindow(hParent, FALSE);

  // Local message loop
  MSG msg;
  while (!m_bDone && GetMessage(&msg, NULL, 0, 0)) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
      HWND hNext = GetNextDlgTabItem(m_hWnd, GetFocus(), GetKeyState(VK_SHIFT) < 0);
      if (hNext) SetFocus(hNext);
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
      m_bResult = false;
      m_bDone = true;
      break;
    }
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  // Cleanup
  EnableWindow(hParent, TRUE);
  // Closing hands the focus back to the render window -- except under testing
  // mode, where the window never took the focus in the first place and giving
  // it to the visualiser would yank it out of whatever the user is typing in.
  // A test that opens and closes several windows would otherwise interrupt him
  // once per window on the way out as well as on the way in.
  if (!(m_pEngine && m_pEngine->m_bTestingMode))
    SetForegroundWindow(hParent);
  DestroyWindow(m_hWnd);
  m_hWnd = NULL;
  m_childCtrls.clear();
  if (m_hFont) { DeleteObject(m_hFont); m_hFont = NULL; }

  return m_bResult;
}

bool ModalDialog::IsChecked(int id) const {
  HWND h = GetDlgItem(m_hWnd, id);
  return h ? (bool)(intptr_t)GetPropW(h, L"Checked") : false;
}

void ModalDialog::SetChecked(int id, bool checked) {
  HWND h = GetDlgItem(m_hWnd, id);
  if (h) {
    SetPropW(h, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
    InvalidateRect(h, NULL, TRUE);
  }
}

int ModalDialog::GetLineHeight() {
  if (!m_hFont) return 18;
  HDC hdc = GetDC(m_hWnd);
  HFONT hOld = (HFONT)SelectObject(hdc, m_hFont);
  TEXTMETRIC tm;
  GetTextMetrics(hdc, &tm);
  SelectObject(hdc, hOld);
  ReleaseDC(m_hWnd, hdc);
  int h = tm.tmHeight + tm.tmExternalLeading + 6;
  return max(h, 20); // match ToolWindow::GetLineHeight()
}

ModalDialog::BaseLayout ModalDialog::GetBaseLayout() {
  return { GetLineHeight(), 6, 16, 85 };
}

void ModalDialog::FitToContent(int clientW, int contentH) {
  HWND hDlg = m_hWnd;
  DWORD dwStyle = (DWORD)GetWindowLongPtrW(hDlg, GWL_STYLE);
  DWORD dwExStyle = (DWORD)GetWindowLongPtrW(hDlg, GWL_EXSTYLE);
  UINT dpi = GetDpiForWindow(hDlg);
  if (dpi == 0) dpi = 96;
  RECT rc = { 0, 0, clientW, contentH };
  AdjustWindowRectExForDpi(&rc, dwStyle, FALSE, dwExStyle, dpi);
  SetWindowPos(hDlg, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
    SWP_NOMOVE | SWP_NOZORDER);
}

// Guarded like the others: a modal dialog's procedure is a window procedure.
LRESULT CALLBACK ModalDialog::ModalWndProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                           LPARAM lParam) {
  __try {
    return ModalWndProcImpl(hWnd, uMsg, wParam, lParam);
  } __except (SehCrashFilter(GetExceptionInformation(), L"ModalDialog")) {
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
  }
}

LRESULT CALLBACK ModalDialog::ModalWndProcImpl(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  if (uMsg == WM_NCCREATE) {
    CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
    if (pcs && pcs->lpCreateParams)
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pcs->lpCreateParams);
  }
  ModalDialog* dlg = (ModalDialog*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
  if (!dlg) return DefWindowProcW(hWnd, uMsg, wParam, lParam);

  Engine* p = dlg->m_pEngine;

  switch (uMsg) {
  case WM_CLOSE:
    dlg->EndDialog(false);
    return 0;

  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORDLG:
  {
    LRESULT lr = HandleDarkCtlColor(p, uMsg, wParam, lParam);
    if (lr) return lr;
    break;
  }

  case WM_DRAWITEM:
  {
    DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
    LRESULT lr = HandleDarkDrawItem(p, pDIS);
    if (lr) return lr;
    break;
  }

  case WM_ERASEBKGND:
    return HandleDarkEraseBkgnd(p, hWnd, (HDC)wParam);

  case WM_COMMAND:
  {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    // Auto-toggle owner-draw checkboxes/radios
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      if ((bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox")) {
        bool was = (bool)(intptr_t)GetPropW(hCtrl, L"Checked");
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(was ? 0 : 1));
        InvalidateRect(hCtrl, NULL, TRUE);
      }
      if ((bool)(intptr_t)GetPropW(hCtrl, L"IsRadio")) {
        int group = (int)(intptr_t)GetPropW(hCtrl, L"RadioGroup");
        if (group != 0) {
          for (HWND hChild : dlg->m_childCtrls) {
            if ((bool)(intptr_t)GetPropW(hChild, L"IsRadio") &&
                (int)(intptr_t)GetPropW(hChild, L"RadioGroup") == group) {
              SetPropW(hChild, L"Checked", (HANDLE)(intptr_t)(hChild == hCtrl ? 1 : 0));
              InvalidateRect(hChild, NULL, TRUE);
            }
          }
        }
      }
    }
    LRESULT r = dlg->DoCommand(id, code, lParam);
    if (r != -1) return r;
    break;
  }

  case WM_NOTIFY:
  {
    NMHDR* pnm = (NMHDR*)lParam;
    // ListView header dark theme custom draw
    if (p->IsDarkTheme() && pnm->code == NM_CUSTOMDRAW) {
      HWND hParent = GetParent(pnm->hwndFrom);
      if (hParent) {
        wchar_t szClass[32];
        GetClassNameW(hParent, szClass, 32);
        if (_wcsicmp(szClass, WC_LISTVIEWW) == 0) {
          bool handled = false;
          LRESULT result = PaintDarkListViewHeader(pnm, lParam, hParent,
            p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText, &handled);
          if (handled) return result;
        }
      }
    }
    LRESULT r = dlg->DoNotify(pnm);
    if (r != -1) return r;
    break;
  }

  case WM_SETTINGCHANGE:
    if (p->m_nThemeMode == Engine::THEME_SYSTEM && lParam &&
        _wcsicmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
      p->LoadSettingsThemeFromINI();
      ApplyDarkThemeToWindow(p, hWnd);
      ApplyDarkThemeToChildren(p, dlg->m_childCtrls);
      RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
    }
    break;

  default:
  {
    LRESULT r = dlg->DoMessage(uMsg, wParam, lParam);
    if (r != -1) return r;
    break;
  }
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

//----------------------------------------------------------------------
// Shared dark theme helpers (used by ToolWindow + ModalDialog + popups)
//----------------------------------------------------------------------

// A brush for a colour, valid even when the engine's shared brushes are not.
//
// m_hBrSettingsBg and friends are shared engine state, deleted and recreated by
// LoadSettingsThemeFromINI. Every tool window runs on ITS OWN THREAD and calls
// that when it opens and when the theme changes -- so one window can be
// painting at the instant another has deleted them.
//
// Every dark paint path guarded on the handle and, finding it null, fell
// through to the system COLOR_BTNFACE brush. On a dark theme that is WHITE:
// Shane's mixer window came up with white blocks where its controls and its
// background should have been, and looked fine again after the next repaint,
// because by then the brushes existed once more. A PrintWindow capture never
// showed it, since that re-renders into a clean DC.
//
// The COLOUR is a plain value and is always readable, so a brush can be made
// from it. Cached by colour and never freed -- there are a handful, and they
// are wanted for the life of the process.
static HBRUSH ThemeBrush(COLORREF col) {
  static std::mutex mtx;
  static std::map<COLORREF, HBRUSH> cache;
  std::lock_guard<std::mutex> lock(mtx);
  auto it = cache.find(col);
  if (it != cache.end()) return it->second;
  HBRUSH brush = CreateSolidBrush(col);
  cache[col] = brush;
  return brush;
}

LRESULT HandleDarkCtlColor(Engine* p, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (!p->IsDarkTheme()) return 0;

  HDC hdc = (HDC)wParam;
  switch (msg) {
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    SetTextColor(hdc, p->m_colSettingsText);
    SetBkColor(hdc, p->m_colSettingsCtrlBg);
    return (LRESULT)(p->m_hBrSettingsCtrlBg ? p->m_hBrSettingsCtrlBg
                                            : ThemeBrush(p->m_colSettingsCtrlBg));

  case WM_CTLCOLORSTATIC:
    {
      HWND hCtrl = (HWND)lParam;
      wchar_t szClass[32];
      GetClassNameW(hCtrl, szClass, 32);
      if (_wcsicmp(szClass, L"Edit") == 0) {
        SetTextColor(hdc, p->m_colSettingsText);
        SetBkColor(hdc, p->m_colSettingsCtrlBg);
        return (LRESULT)(p->m_hBrSettingsCtrlBg ? p->m_hBrSettingsCtrlBg
                                                : ThemeBrush(p->m_colSettingsCtrlBg));
      }
      // A control may ask for its own text colour. The high byte is a
      // present-flag, so a legitimately black colour is still expressible --
      // a bare 0 would be indistinguishable from "no property set".
      const UINT_PTR fg = (UINT_PTR)GetPropW(hCtrl, L"FgColor");
      COLORREF colText = fg ? (COLORREF)(fg & 0x00FFFFFF) : p->m_colSettingsText;
      // A disabled label is drawn by Windows in the colour we hand back, so
      // without this a whole page of dead controls still reads as live.
      if (!IsWindowEnabled(hCtrl))
        colText = RGB((GetRValue(colText) + GetRValue(p->m_colSettingsBg) * 2) / 3,
                      (GetGValue(colText) + GetGValue(p->m_colSettingsBg) * 2) / 3,
                      (GetBValue(colText) + GetBValue(p->m_colSettingsBg) * 2) / 3);
      SetTextColor(hdc, colText);
      SetBkColor(hdc, p->m_colSettingsBg);
      SetBkMode(hdc, TRANSPARENT);
      return (LRESULT)(p->m_hBrSettingsBg ? p->m_hBrSettingsBg
                                          : ThemeBrush(p->m_colSettingsBg));
    }

  case WM_CTLCOLORBTN:
    SetTextColor(hdc, p->m_colSettingsText);
    SetBkColor(hdc, p->m_colSettingsBg);
    return (LRESULT)(p->m_hBrSettingsBg ? p->m_hBrSettingsBg
                                        : ThemeBrush(p->m_colSettingsBg));

  case WM_CTLCOLORDLG:
    return (LRESULT)(p->m_hBrSettingsBg ? p->m_hBrSettingsBg
                                        : ThemeBrush(p->m_colSettingsBg));
  }
  return 0;
}

LRESULT HandleDarkDrawItem(Engine* p, DRAWITEMSTRUCT* pDIS) {
  if (!pDIS) return FALSE;

  if (pDIS->CtlType == ODT_TAB) {
    bool bSelected = (pDIS->itemState & ODS_SELECTED) != 0;
    HDC hdc = pDIS->hDC;
    RECT rc = pDIS->rcItem;
    if (p->IsDarkTheme()) {
      COLORREF bg = bSelected ? p->m_colSettingsCtrlBg : p->m_colSettingsBtnFace;
      HBRUSH hBr = CreateSolidBrush(bg);
      FillRect(hdc, &rc, hBr);
      DeleteObject(hBr);
      if (bSelected) {
        HPEN hiPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnHi);
        HPEN shPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnShadow);
        HPEN oldPen = (HPEN)SelectObject(hdc, hiPen);
        MoveToEx(hdc, rc.left, rc.top, NULL);
        LineTo(hdc, rc.right - 1, rc.top);
        MoveToEx(hdc, rc.left, rc.top, NULL);
        LineTo(hdc, rc.left, rc.bottom);
        SelectObject(hdc, shPen);
        MoveToEx(hdc, rc.right - 1, rc.top, NULL);
        LineTo(hdc, rc.right - 1, rc.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(hiPen);
        DeleteObject(shPen);
      } else {
        HPEN shPen = CreatePen(PS_SOLID, 1, p->m_colSettingsBtnShadow);
        HPEN oldPen = (HPEN)SelectObject(hdc, shPen);
        MoveToEx(hdc, rc.left, rc.bottom - 1, NULL);
        LineTo(hdc, rc.right, rc.bottom - 1);
        SelectObject(hdc, oldPen);
        DeleteObject(shPen);
      }
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, bSelected ? p->m_colSettingsHighlightText : p->m_colSettingsText);
    } else {
      FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    }
    wchar_t szText[64] = {};
    TCITEMW tci = {};
    tci.mask = TCIF_TEXT;
    tci.pszText = szText;
    tci.cchTextMax = 64;
    SendMessageW(pDIS->hwndItem, TCM_GETITEMW, pDIS->itemID, (LPARAM)&tci);
    DrawTextW(pDIS->hDC, szText, -1, &pDIS->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return TRUE;
  }

  if (pDIS->CtlType == ODT_BUTTON) {
    // Skip pin button — ToolWindow handles that itself
    if ((bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsPinBtn"))
      return FALSE;

    // Windows greys a standard control for us. An owner-drawn one is painted
    // entirely by this code, so without honouring ODS_DISABLED a control that
    // ignores every click still looks perfectly clickable -- which is worse
    // than leaving it enabled, because the user blames the feature rather than
    // the state it is in.
    const bool bDisabled = (pDIS->itemState & ODS_DISABLED) != 0;
    const COLORREF colText = bDisabled
        ? RGB((GetRValue(p->m_colSettingsText) + GetRValue(p->m_colSettingsBg) * 2) / 3,
              (GetGValue(p->m_colSettingsText) + GetGValue(p->m_colSettingsBg) * 2) / 3,
              (GetBValue(p->m_colSettingsText) + GetBValue(p->m_colSettingsBg) * 2) / 3)
        : p->m_colSettingsText;

    bool bIsCheckbox = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsCheckbox");
    bool bIsRadio = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsRadio");
    if (bIsCheckbox) {
      DrawOwnerCheckbox(pDIS, p->IsDarkTheme(),
        p->m_colSettingsBg, p->m_colSettingsCtrlBg, p->m_colSettingsBorder, colText);
    } else if (bIsRadio) {
      DrawOwnerRadio(pDIS, p->IsDarkTheme(),
        p->m_colSettingsBg, p->m_colSettingsCtrlBg, p->m_colSettingsBorder, colText);
    } else {
      // "AccentBtn" marks a button that needs to stand out -- currently the
      // Video Effects Save Profile button while there are unsaved changes.
      const bool bAccent = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"AccentBtn");
      DrawOwnerButton(pDIS, p->IsDarkTheme(),
        p->m_colSettingsBtnFace, p->m_colSettingsBtnHi, p->m_colSettingsBtnShadow,
        colText, bAccent);
    }
    return TRUE;
  }

  // Static swatch controls (SS_OWNERDRAW)
  if (pDIS->CtlType == ODT_STATIC) {
    COLORREF col = (COLORREF)(intptr_t)GetPropW(pDIS->hwndItem, L"SwatchColor");
    HDC hdc = pDIS->hDC;
    RECT rc = pDIS->rcItem;
    HBRUSH hBr = CreateSolidBrush(col);
    FillRect(hdc, &rc, hBr);
    DeleteObject(hBr);
    return TRUE;
  }

  return FALSE;
}

LRESULT HandleDarkEraseBkgnd(Engine* p, HWND hWnd, HDC hdc) {
  RECT rc;
  GetClientRect(hWnd, &rc);
  // A dark window never falls back to the system light brush. That fallback is
  // what painted Shane's mixer white while another thread was rebuilding the
  // shared brushes.
  if (p->IsDarkTheme())
    FillRect(hdc, &rc, p->m_hBrSettingsBg ? p->m_hBrSettingsBg
                                          : ThemeBrush(p->m_colSettingsBg));
  else
    FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
  return 1;
}

void ApplyDarkThemeToWindow(Engine* p, HWND hWnd) {
  if (!hWnd) return;
  bool bDark = p->IsDarkTheme();
  BOOL bDarkDWM = bDark ? TRUE : FALSE;
  DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &bDarkDWM, sizeof(bDarkDWM));
  if (bDark) {
    DwmSetWindowAttribute(hWnd, 35, &p->m_colSettingsBg, sizeof(COLORREF));
    DwmSetWindowAttribute(hWnd, 34, &p->m_colSettingsBorder, sizeof(COLORREF));
    DwmSetWindowAttribute(hWnd, 36, &p->m_colSettingsText, sizeof(COLORREF));
  } else {
    COLORREF reset = 0xFFFFFFFF;
    DwmSetWindowAttribute(hWnd, 35, &reset, sizeof(reset));
    DwmSetWindowAttribute(hWnd, 34, &reset, sizeof(reset));
    DwmSetWindowAttribute(hWnd, 36, &reset, sizeof(reset));
  }

  // Enable dark popup menus via undocumented uxtheme APIs (Windows 10 1903+)
  ResolveDarkModeAPIs();
  if (pSetPreferredAppMode)
    pSetPreferredAppMode(bDark ? ForceDark : ForceLight);
  if (pAllowDarkModeForWindow)
    pAllowDarkModeForWindow(hWnd, bDark);
  if (pFlushMenuThemes)
    pFlushMenuThemes();
}

void ApplyDarkThemeToChildren(Engine* p, const std::vector<HWND>& ctrls) {
  bool bDark = p->IsDarkTheme();
  for (HWND hChild : ctrls) {
    if (!hChild || !IsWindow(hChild)) continue;
    wchar_t szClass[32];
    GetClassNameW(hChild, szClass, 32);
    if (_wcsicmp(szClass, WC_TABCONTROLW) == 0)
      SetWindowTheme(hChild, bDark ? L"" : NULL, bDark ? L"" : NULL);
    else if (_wcsicmp(szClass, HOTKEY_CLASSW) == 0)
      SetWindowTheme(hChild, bDark ? L"DarkMode_CFD" : NULL, NULL);
    else if (_wcsicmp(szClass, WC_LISTVIEWW) == 0) {
      SetWindowTheme(hChild, bDark ? L"DarkMode_Explorer" : NULL, NULL);
      if (bDark) {
        ListView_SetBkColor(hChild, p->m_colSettingsCtrlBg);
        ListView_SetTextBkColor(hChild, p->m_colSettingsCtrlBg);
        ListView_SetTextColor(hChild, p->m_colSettingsText);
      } else {
        ListView_SetBkColor(hChild, CLR_DEFAULT);
        ListView_SetTextBkColor(hChild, CLR_DEFAULT);
        ListView_SetTextColor(hChild, CLR_DEFAULT);
      }
    }
    else if (_wcsicmp(szClass, L"ListBox") == 0) {
      // A list keeps its theme so the SCROLLBAR is dark. Stripping styles the
      // way everything else here does leaves the classic light-grey trough and
      // white thumb sitting on a dark list, which is the scrollbar Shane
      // reported. Item colours are unaffected: they come from
      // WM_CTLCOLORLISTBOX, which the shared WndProc already answers.
      SetWindowTheme(hChild, bDark ? L"DarkMode_Explorer" : NULL, NULL);
    }
    else {
      // Strip visual styles — dark painting handled by WM_CTLCOLOR*,
      // WM_DRAWITEM, and WM_ERASEBKGND in the parent WndProc.
      // DarkMode_Explorer on EDIT controls overrides WM_CTLCOLOREDIT brush.
      SetWindowTheme(hChild, bDark ? L"" : NULL, bDark ? L"" : NULL);
    }
  }
}

//----------------------------------------------------------------------
// Dark ListView header helper (shared by ToolWindows + Resource Viewer)
//----------------------------------------------------------------------

LRESULT PaintDarkListViewHeader(NMHDR* pnm, LPARAM lParam, HWND hListView,
                                COLORREF colBg, COLORREF colBorder, COLORREF colText,
                                bool* pHandled)
{
  *pHandled = false;
  HWND hHeader = ListView_GetHeader(hListView);
  if (!hHeader || pnm->hwndFrom != hHeader) return 0;

  NMCUSTOMDRAW* pcd = (NMCUSTOMDRAW*)lParam;
  switch (pcd->dwDrawStage) {
  case CDDS_PREPAINT:
    *pHandled = true;
    return CDRF_NOTIFYITEMDRAW;
  case CDDS_ITEMPREPAINT: {
    HDC hdc = pcd->hdc;
    RECT rc = pcd->rc;
    HBRUSH hBr = CreateSolidBrush(colBg);
    FillRect(hdc, &rc, hBr);
    DeleteObject(hBr);
    HPEN hPen = CreatePen(PS_SOLID, 1, colBorder);
    HPEN hOld = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, rc.right - 1, rc.top, NULL);
    LineTo(hdc, rc.right - 1, rc.bottom);
    SelectObject(hdc, hOld);
    DeleteObject(hPen);
    wchar_t szText[128] = {};
    HDITEMW hdi = {};
    hdi.mask = HDI_TEXT;
    hdi.pszText = szText;
    hdi.cchTextMax = 128;
    Header_GetItem(hHeader, (int)pcd->dwItemSpec, &hdi);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, colText);
    HFONT hFont = (HFONT)SendMessage(hHeader, WM_GETFONT, 0, 0);
    HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
    rc.left += 6;
    DrawTextW(hdc, szText, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (hOldFont) SelectObject(hdc, hOldFont);
    *pHandled = true;
    return CDRF_SKIPDEFAULT;
  }
  }
  return 0;
}

//----------------------------------------------------------------------
// Themed ListView factory
//----------------------------------------------------------------------

HWND ToolWindow::CreateThemedListView(int id, int x, int y, int w, int h,
                                      bool visible, bool sortable)
{
  DWORD style = WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS;
  if (!sortable) style |= LVS_NOSORTHEADER;
  if (visible) style |= WS_VISIBLE | WS_TABSTOP;

  HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
    style, x, y, w, h, m_hWnd,
    (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hList) {
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (m_hFont)
      SendMessage(hList, WM_SETFONT, (WPARAM)m_hFont, TRUE);
  }
  return hList;
}

//----------------------------------------------------------------------
// Font helpers
//----------------------------------------------------------------------

// static
int ToolWindow::LineHeightForFontSize(int nFontSize) {
  HDC hdc = GetDC(NULL);
  if (!hdc) return 26;
  // Same face and same weight as BuildBaseControls, because a different one
  // would answer a different question.
  HFONT hf = CreateFontW(nFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  int h = 26;
  if (hf) {
    HFONT hOld = (HFONT)SelectObject(hdc, hf);
    TEXTMETRIC tm = {};
    GetTextMetrics(hdc, &tm);
    SelectObject(hdc, hOld);
    DeleteObject(hf);
    h = tm.tmHeight + tm.tmExternalLeading + 6;   // matches GetLineHeight
  }
  ReleaseDC(NULL, hdc);
  return max(h, 20);
}

int ToolWindow::ScaledMinWidth() {
  return MulDiv(GetMinWidth(), GetLineHeight(), 26);
}

int ToolWindow::ScaledMinHeight() {
  return MulDiv(GetMinHeight(), GetLineHeight(), 26);
}

int ToolWindow::GetLineHeight() {
  // Before the window exists there is still an answer, and it is the one that
  // decides how big the window opens -- see LineHeightForFontSize. Returning a
  // bare 26 here is what made a large font open windows at a small font's size.
  if (!m_hFont || !m_hWnd)
    return m_pEngine ? LineHeightForFontSize(m_pEngine->m_nSettingsFontSize) : 26;
  HDC hdc = GetDC(m_hWnd);
  if (!hdc) return 26;
  HFONT hOld = (HFONT)SelectObject(hdc, m_hFont);
  TEXTMETRIC tm = {};
  GetTextMetrics(hdc, &tm);
  SelectObject(hdc, hOld);
  ReleaseDC(m_hWnd, hdc);
  int h = tm.tmHeight + tm.tmExternalLeading + 6;
  return max(h, 20);
}

ToolWindow::BaseLayout ToolWindow::BuildBaseControls() {
  HWND hw = m_hWnd;

  // Create fonts from shared font size
  if (m_hFont) DeleteObject(m_hFont);
  m_hFont = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  if (m_hFontBold) DeleteObject(m_hFontBold);
  m_hFontBold = CreateFontW(m_pEngine->m_nSettingsFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  RECT rcWnd;
  GetClientRect(hw, &rcWnd);
  int clientW = rcWnd.right;

  int lineH = GetLineHeight();
  int gap = 6, x = 16;
  int rw = clientW - x * 2;
  int y = 8;

  // Font +/- buttons (top-left)
  {
    int btnW = lineH;
    TrackControl(CreateBtn(hw, L"\u2212", GetFontMinusControlID(), x, y, btnW, lineH, m_hFont));
    TrackControl(CreateBtn(hw, L"+", GetFontPlusControlID(), x + btnW + 4, y, btnW, lineH, m_hFont));
  }

  // Pin button (top-right)
  {
    if (m_hPinFont) DeleteObject(m_hPinFont);
    if (m_hGlyphFont) DeleteObject(m_hGlyphFont);
    int pinSize = lineH;
    m_hPinFont = CreateFontW(-pinSize + 4, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    // 81% of the pin's, for the glyphs that are not the pin.
    //
    // Two passes of "another ten percent", both by eye and both Shane's. The
    // refresh wheel is a filled circle where the pin is a thin outline, so at a
    // matched font size it reads as the heavier and taller of the two even
    // though the em boxes are identical. There is no measurement that gives
    // this number; it is what looks level beside the pin.
    m_hGlyphFont = CreateFontW(MulDiv(-pinSize + 4, 81, 100), 0, 0, 0, FW_NORMAL,
      FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
      L"Segoe MDL2 Assets");

    // Resize-to-fit sits immediately left of the pin.
    //
    // Shane asked for it after a window came back too small to show its
    // controls: "maybe have a reset like button icon next to the pin for tool
    // windows to try to resize the window for the font size so that users can
    // have a good out of box experience". A tool window's layout is computed
    // from the font, so the size that fits depends on a font the user can
    // change with the two buttons opposite -- there is no single right default
    // to ship, only a way to ask for one.
    const int fitX = clientW - pinSize * 2 - x - 6;
    HWND hFit = CreateWindowExW(0, L"BUTTON", L"\xE72C",   // Segoe MDL2 refresh
      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
      fitX, y, pinSize, pinSize, hw,
      (HMENU)(INT_PTR)IDC_MW_TOOLWIN_FIT, GetModuleHandle(NULL), NULL);
    if (hFit) {
      if (m_hGlyphFont) SendMessage(hFit, WM_SETFONT, (WPARAM)m_hGlyphFont, TRUE);
      // Drawn by the same owner-draw path as the pin, which keys off this.
      SetPropW(hFit, L"IsPinBtn", (HANDLE)(intptr_t)1);
      HWND hFitTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hw, NULL, GetModuleHandle(NULL), NULL);
      TrackTooltip(hFitTip);
      if (hFitTip) {
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        ti.hwnd = hw;
        ti.uId = (UINT_PTR)hFit;
        ti.lpszText = (LPWSTR)L"Resize to fit the contents at this font size";
        SendMessageW(hFitTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
      }
      TrackControl(hFit);
    }

    int pinX = clientW - pinSize - x;
    HWND hPin = CreateWindowExW(0, L"BUTTON", L"\xE718",
      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
      pinX, y, pinSize, pinSize, hw,
      (HMENU)(INT_PTR)GetPinControlID(), GetModuleHandle(NULL), NULL);
    if (hPin) {
      if (m_hPinFont) SendMessage(hPin, WM_SETFONT, (WPARAM)m_hPinFont, TRUE);
      SetPropW(hPin, L"IsPinBtn", (HANDLE)(intptr_t)1);
      HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hw, NULL, GetModuleHandle(NULL), NULL);
      TrackTooltip(hTip);
      if (hTip) {
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        ti.hwnd = hw;
        ti.uId = (UINT_PTR)hPin;
        ti.lpszText = (LPWSTR)L"Always on top";
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
      }
    }
    TrackControl(hPin);
  }

  y += lineH + gap + 4;
  return { y, lineH, gap, x, rw, clientW };
}

// Re-anchor the base controls after a resize.
//
// The font +/- buttons are anchored to the left edge, so they are already right
// wherever the window goes. The PIN is anchored to the RIGHT edge, and nothing
// moved it: it was placed once at clientW - pinSize - x when the controls were
// built and then left there. Widen the window and it sat in the middle; narrow
// it and it went off the edge entirely and could not be clicked at all.
//
// This lives in the base class because the pin is a base control. Leaving it to
// each subclass's LayoutControls means twenty-odd windows each have to remember
// to move a control they never created -- and every one of them had forgotten.
void ToolWindow::LayoutBaseControls() {
  if (!m_hWnd) return;
  HWND hPin = GetDlgItem(m_hWnd, GetPinControlID());
  HWND hFit = GetDlgItem(m_hWnd, IDC_MW_TOOLWIN_FIT);
  if (!hPin && !hFit) return;

  RECT rc;
  GetClientRect(m_hWnd, &rc);

  // Must match BuildBaseControls: x = 16, y = 8, pinSize = lineH.
  const int lineH = GetLineHeight();
  const int x = 16, y = 8;
  if (hPin) MoveWindow(hPin, rc.right - lineH - x, y, lineH, lineH, TRUE);
  // Immediately left of the pin, and moved for the same reason it is: placed
  // once at build time it would sit in the middle of a widened window and fall
  // off the edge of a narrowed one.
  if (hFit) MoveWindow(hFit, rc.right - lineH * 2 - x - 6, y, lineH, lineH, TRUE);
}

// Resize the window so every control fits, at whatever font size is in use.
//
// A tool window's layout is computed from its font -- lineH drives nearly every
// position in every DoBuildControls -- so the size that fits is not a constant
// anybody could ship as a default. Change the font with the two buttons
// opposite and the right size changes with it. This asks for it instead.
//
// Measured from the CONTROLS rather than recomputed from the layout code: the
// windows lay themselves out twenty different ways and not one reports a
// required size, but every one has already placed its controls by the time this
// can be clicked. A control hanging off the bottom is exactly the symptom being
// fixed, and it is still a control with a rectangle.
void ToolWindow::FitToContents() {
  FitContents(/*bRestoreDesignedSize=*/true);
}

// Run once from CreateOnThread, after the window is shown (issue 157).
//
// After the show, and not before it, because the measurement below skips
// anything IsWindowVisible calls hidden -- and that answer is about the whole
// ancestor chain, so while the frame is still hidden EVERY child reports
// invisible and the window measures as empty. There is a second reason to
// prefer this moment: a window created with a size saved on a monitor of a
// different DPI is rescaled by Windows at the show, so a fit computed before it
// would be scaled away along with everything else.
void ToolWindow::FitClippedContentsOnOpen() {
  if (!m_hWnd) return;

  // Up to three passes, because a fit can MOVE what it just measured.
  //
  // Several of these layouts are elastic: the Text Animations list is
  // MulDiv(120, lineH, 26) tall but shrinks toward a floor of lineH * 3 when
  // the window is short, and the Remote list is sized from its tab's bottom
  // with a floor of lineH * 4. Growing the window lets such a list back out
  // toward its natural height, which pushes everything below it DOWN -- so one
  // pass fixed most of Text Animations and left its bottom row (Preview, the
  // preview text, Save) hanging over the edge by exactly the 58 pixels the list
  // had regained.
  //
  // It terminates because every one of those elastic controls is CAPPED at a
  // natural size: once the list stops growing, nothing below it moves and the
  // next pass changes nothing. The bound of three is a backstop for a layout
  // that is not capped rather than an expected count -- and stopping early
  // leaves the window no worse than the pass before, never oscillating,
  // because each pass only ever grows.
  for (int pass = 0; pass < 3; pass++) {
    RECT before = {}, after = {};
    if (!GetWindowRect(m_hWnd, &before)) return;
    FitContents(/*bRestoreDesignedSize=*/false);
    if (!GetWindowRect(m_hWnd, &after)) return;
    if (EqualRect(&before, &after)) return;   // converged
  }
}

void ToolWindow::FitContents(bool bRestoreDesignedSize) {
  if (!m_hWnd) return;

  struct Extent { HWND self; ToolWindow* tw; int right; int bottom; };
  Extent e{ m_hWnd, this, 0, 0 };
  EnumChildWindows(m_hWnd, [](HWND child, LPARAM lp) -> BOOL {
    Extent* ex = (Extent*)lp;
    if (!IsWindowVisible(child)) return TRUE;

    // A control anchored to an edge follows that edge, so its extent in that
    // direction says nothing about how big the window needs to be -- it is
    // whatever the window already is. The tab control below is the same
    // problem in its most obvious form; this is the general case.
    //
    // The Annotations window is the one that showed it: its list is anchored to
    // the bottom, so measuring it asked for a window taller than itself and the
    // button grew it by a line on every click, while three other windows had
    // already gone still.
    unsigned edges = 0;
    for (const AnchoredControl& a : ex->tw->m_anchors)
      if (a.hwnd == child) { edges = a.edges; break; }
    const bool followsRight  = (edges & kAnchorRight) && (edges & kAnchorLeft);
    const bool followsBottom = (edges & kAnchorBottom);

    // The tab control is not content, it is the container.
    //
    // It fills the window by construction, so its bottom edge IS the client
    // bottom whatever size the window is. Measuring it asks "how tall must the
    // window be to contain something exactly as tall as the window", which has
    // no answer -- and the button grew every tabbed window by one line per
    // click while Presets, which has no tabs, sat perfectly still. That
    // difference is what identified it.
    wchar_t cls[32] = {};
    GetClassNameW(child, cls, 32);
    if (_wcsicmp(cls, WC_TABCONTROLW) == 0) return TRUE;

    RECT rc;
    if (!GetWindowRect(child, &rc)) return TRUE;
    MapWindowPoints(NULL, ex->self, (LPPOINT)&rc, 2);
    if (!followsRight  && rc.right  > ex->right)  ex->right  = rc.right;
    if (!followsBottom && rc.bottom > ex->bottom) ex->bottom = rc.bottom;
    return TRUE;
  }, (LPARAM)&e);
  if (e.right <= 0 || e.bottom <= 0) return;

  // Grow to fix an OVERFLOW; never grow something that already fits.
  //
  // This added the margin unconditionally, which made the button grow the
  // window a little on every click -- Shane: "every time I click the reset size
  // button the width increases now". Most of these layouts anchor controls with
  // kAnchorStretchWide, so a stretched control's right edge IS the client edge
  // less whatever gap the layout leaves. Measure that, add a margin, and the
  // answer is always a bit wider than the window you measured; resize to it and
  // the next click measures the wider one. A feedback loop with a step of a few
  // pixels, which is exactly how it looked.
  //
  // So the client size is the floor, not the starting guess: a dimension only
  // moves when something is actually hanging outside it. That makes a second
  // click a no-op, which is what a button like this has to be.
  RECT client;
  GetClientRect(m_hWnd, &client);
  int wantW = client.right;
  int wantH = client.bottom;
  // The margin is added to the TARGET, never to the TEST.
  //
  // Getting that backwards is what made this grow on every click. Testing
  // "rightmost control + margin > client" always succeeds, because a stretched
  // control's right edge is the client edge less whatever small gap its layout
  // leaves -- so the test re-fires at any width and the window creeps outwards
  // forever. Testing the raw extent asks the only question that matters, is
  // anything actually hanging outside, and that answer becomes false as soon as
  // it has been fixed once.
  //
  // The margin still belongs in the result: growing to exactly the last
  // control's edge leaves it flush against the frame, which is what Shane saw
  // on the profile path field. Scaled from the font like every other gap in
  // these layouts, so it stays proportionate when the font is changed with the
  // two buttons opposite.
  // A couple of pixels of overflow is not an overflow.
  //
  // Resizing makes the window re-lay-out, and these layouts are built from
  // MulDiv(n, lineH, 26), so the rounding lands differently at a different
  // width: after a fit that removed a five-pixel overflow, a DIFFERENT control
  // came out one pixel past the new client edge. Chasing that pixel is how the
  // button starts oscillating -- grow one, reflow, grow one -- and one row of
  // pixels off the bottom of a control is not visible anyway.
  const int lineH = GetLineHeight();
  const int slack = 2;

  // WIDTH: only ever fixes an overflow, and adds the margin to the result.
  //
  // It cannot ask for a gap at the right-hand edge, because most controls here
  // are anchored kAnchorStretchWide and restretch to whatever width they are
  // given. "Leave 12px to the right of the widest control" is unsatisfiable by
  // construction: widen the window and the control widens with it. Asking for
  // it is what made this grow a few pixels on every click.
  const int marginX = MulDiv(12, lineH, 26);
  if (e.right > wantW + slack) wantW = e.right + marginX;

  // HEIGHT: only when something OVERFLOWS, and then with a full line of margin.
  //
  // Asking for a gap below the last control regardless was tried and cannot
  // work either. The Annotations list fills the window: measured at three
  // different heights it sat five pixels above the client edge every time, so
  // "leave nineteen pixels below it" grew the window, the list restretched to
  // five, and the button asked again -- a line per click, forever. Some windows
  // simply have no content height independent of their own size.
  //
  // Shane's rule, and it is the one that holds in both cases: "can't you check
  // and see if there is too much space before adding more". A window with room
  // below its last control is left exactly as it is, whether that room is five
  // pixels of a filler list's own margin or half a screen. Only content that is
  // actually cut off is worth moving the frame for.
  //
  // The margin is a full line height rather than the twelve the sides use --
  // the same row spacing the layouts leave between controls, so the foot of the
  // window matches the rhythm above it. That is the "not padding bottom enough"
  // Shane saw on the profile path field, which had overflowed by five pixels
  // and was being given back only twelve.
  const int marginY = lineH;
  if (e.bottom > wantH + slack) wantH = e.bottom + marginY;

  // Client size to frame size, using THIS window's real border rather than
  // AdjustWindowRectEx's idea of one.
  //
  // AdjustWindowRectEx computes a border from the style bits, and on a
  // per-monitor-DPI setup it disagrees with the frame the window actually has.
  // Feeding an unchanged client width back through it therefore produced a
  // slightly SMALLER window, so the button shrank the window a few pixels on
  // every click once the growing was fixed -- the same bug with its sign
  // flipped. Measuring the difference cannot disagree with reality: when
  // nothing needs to change, frameW comes out exactly equal to the current
  // frame and the resize is a no-op.
  RECT wr;
  GetWindowRect(m_hWnd, &wr);
  const int borderW = (wr.right - wr.left) - client.right;
  const int borderH = (wr.bottom - wr.top) - client.bottom;
  int frameW = wantW + borderW, frameH = wantH + borderH;

  // The minimum is a FRAME size, and belongs here rather than up beside wantW.
  //
  // It used to be applied to the client size and the border added afterwards,
  // which quietly asked for min + border -- roughly 22 by 56 more than the
  // minimum actually is. Nobody noticed while this only ran on a button press,
  // because the result still fitted; running it on every open made it visible
  // at once. Three windows sitting exactly at their minimum reported content
  // comfortably INSIDE their client rect and were resized anyway: Colors
  // 401 -> 456 tall, MIDI 550 -> 606, the Audio Mixer 640 -> 662 wide. The
  // mixer draws as many faders as fit, so that was a layout decision of the
  // user's being overwritten by a unit mismatch.
  //
  // WM_GETMINMAXINFO passes the same numbers to ptMinTrackSize, which is a
  // window size, so frame space is also the space they were written in.
  if (frameW < GetMinWidth())  frameW = GetMinWidth();
  if (frameH < GetMinHeight()) frameH = GetMinHeight();

  // ...and never smaller than this window's own default, scaled to the font.
  //
  // The content box alone is not enough, because these layouts REFLOW: shrink a
  // window and its controls re-lay-out to the smaller width, so measuring them
  // afterwards measures the shrunken arrangement and the fit converges on it.
  // Worse for a window whose content adapts on purpose -- the mixer draws as
  // many faders as fit, so a short window is honestly "full".
  //
  // The default size is the shape the window was designed to be. It is in
  // pixels chosen against a line height of 26, which is what GetLineHeight
  // falls back to and what every MulDiv(n, lineH, 26) in the layouts is
  // relative to, so scaling it by the current line height is what makes this
  // answer the actual request: a size that suits the font in use.
  //
  // ...and only when a human pressed the button. Opening a window applies the
  // overflow fix above and stops there (issue 157): a size the user chose is
  // not a fault to be corrected, so a window that already fits is not touched
  // at all, and one that does not is grown by the overflow and no further. The
  // button still means "put this window right", which is a larger claim and is
  // why it gets the larger answer.
  if (bRestoreDesignedSize) {
    const int defW = MulDiv(m_nDefaultW, lineH, 26);
    const int defH = MulDiv(m_nDefaultH, lineH, 26);
    if (frameW < defW) frameW = defW;
    if (frameH < defH) frameH = defH;
    // The declared minimum is in 26ths as well, and here -- where a person has
    // asked for the window to be put right -- it should mean what it says.
    if (frameW < ScaledMinWidth())  frameW = ScaledMinWidth();
    if (frameH < ScaledMinHeight()) frameH = ScaledMinHeight();
  }

  // Never bigger than the monitor it is on. A window taller than the screen is
  // no more usable than one too small, and this exists to rescue someone from
  // that problem rather than hand them its opposite.
  MONITORINFO mi = { sizeof(mi) };
  if (GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    const int availW = mi.rcWork.right - mi.rcWork.left;
    const int availH = mi.rcWork.bottom - mi.rcWork.top;
    if (frameW > availW) frameW = availW;
    if (frameH > availH) frameH = availH;
  }

  // SIZE ONLY. The window is not moved, and deliberately not clamped back onto
  // a monitor afterwards either -- Shane: "I didn't want the reset to reset the
  // position just resize the window to fit the font". Someone who has put a
  // window somewhere on purpose has not asked for it to be put somewhere else,
  // and SWP_NOMOVE keeps the top-left corner where it is, so the title bar
  // stays exactly where the hand expects it and growth goes right and down.
  // Nothing to do is DONE. Every tool window now runs this on the way up and
  // nineteen of the twenty-three already fit; a no-op must cost them no resize,
  // no relayout, no settings write and no log line. It also makes the button
  // idempotent in the strongest sense -- a second press does not even reach the
  // window manager.
  if (frameW == (wr.right - wr.left) && frameH == (wr.bottom - wr.top)) return;

  SetWindowPos(m_hWnd, NULL, 0, 0, frameW, frameH,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  SaveWindowPosition();
  // The MEASUREMENT is logged, not just the outcome. "It grew" is not a fact
  // anyone can check; "its content reached 412x364 inside a client of 434x349"
  // is, and it is the only way to tell a real overflow from a window that was
  // merely measured at an unlucky moment.
  DLOG_INFO("%ls: resized to fit its contents, %dx%d -- content %dx%d in a "
            "client of %dx%d%ls",
            GetINISection(), frameW, frameH, e.right, e.bottom,
            (int)client.right, (int)client.bottom,
            bRestoreDesignedSize ? L"" : L" (it opened clipped)");
}

//----------------------------------------------------------------------
// Tab control support
//----------------------------------------------------------------------

// Shared tab subclass for dark background — used by ToolWindow and ModalDialog
static LRESULT CALLBACK DarkTabSubclassProcImpl(
  HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
  UINT_PTR subclassId, DWORD_PTR refData);

// A subclass procedure is a window procedure, with the same fatal property: an
// exception escaping it takes the process down with no dump and no log line
// (seh_guard.h, forgejo#75). Guarded for that reason, and thin because __try
// cannot share a frame with C++ unwinding.
//
// The handler returns 0 and does NOT call DefSubclassProc.
//
// That is not tidiness. The fault here is usually raised INSIDE the default
// handler -- comctl32's own tab code, reached through DefSubclassProc -- so
// calling it again from the handler re-runs the very code that just faulted,
// and that second fault is outside the __try and therefore fatal. Returning a
// value the caller can live with is the only safe answer.
LRESULT CALLBACK DarkTabSubclassProc(
  HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
  UINT_PTR subclassId, DWORD_PTR refData)
{
  __try {
    return DarkTabSubclassProcImpl(hwnd, msg, wParam, lParam, subclassId, refData);
  } __except (SehCrashFilter(GetExceptionInformation(), L"tab control subclass")) {
    return 0;
  }
}

static LRESULT CALLBACK DarkTabSubclassProcImpl(
  HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
  UINT_PTR /*subclassId*/, DWORD_PTR refData)
{
  switch (msg) {
  case WM_ERASEBKGND: {
    Engine* p = (Engine*)refData;
    if (p && p->IsDarkTheme()) {
      HDC hdc = (HDC)wParam;
      RECT rc; GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, p->m_hBrSettingsBg ? p->m_hBrSettingsBg
                                            : ThemeBrush(p->m_colSettingsBg));
      return 1;
    }
    break;
  }
  case WM_PAINT: {
    // Default tab WM_PAINT repaints the display area with system theme colors,
    // overriding our dark WM_ERASEBKGND. Let default paint, then repaint display area dark.
    Engine* p = (Engine*)refData;
    // The dark repaint is NOT conditional on the shared brush existing.
    //
    // This is where Shane's white rectangles came from. The default paint above
    // fills the display area with system colours -- white on a dark theme --
    // and this used to skip the repaint whenever m_hBrSettingsBg happened to be
    // null, which it is for a moment whenever another tool window's thread
    // rebuilds the shared brushes. The white then simply stayed, in exactly the
    // rectangles the hidden page's controls had invalidated, until something
    // forced another repaint. A PrintWindow capture never showed it because
    // that re-renders from scratch.
    LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (p && p->IsDarkTheme()) {
      RECT rcDisplay;
      GetClientRect(hwnd, &rcDisplay);
      SendMessage(hwnd, TCM_ADJUSTRECT, FALSE, (LPARAM)&rcDisplay);
      HDC hdc = GetDC(hwnd);
      FillRect(hdc, &rcDisplay, p->m_hBrSettingsBg ? p->m_hBrSettingsBg
                                                   : ThemeBrush(p->m_colSettingsBg));
      ReleaseDC(hwnd, hdc);
    }
    return lr;
  }
  case WM_NCDESTROY:
    RemoveWindowSubclass(hwnd, DarkTabSubclassProc, 1);
    break;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ToolWindow::TabSubclassProc(
  HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
  UINT_PTR subclassId, DWORD_PTR refData)
{
  return DarkTabSubclassProc(hwnd, msg, wParam, lParam, subclassId, refData);
}

RECT ToolWindow::BuildTabControl(int tabCtrlID, const wchar_t* const* tabNames, int numPages,
                                  int x, int y, int w, int h)
{
  m_pageCtrls.clear();
  m_pageCtrls.resize(numPages);
  m_nActivePage = 0;

  m_hTab = CreateWindowExW(0, WC_TABCONTROLW, NULL,
    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED,
    x, y, w, h, m_hWnd, (HMENU)(INT_PTR)tabCtrlID,
    GetModuleHandle(NULL), NULL);
  SendMessage(m_hTab, WM_SETFONT, (WPARAM)m_hFont, TRUE);
  SetWindowSubclass(m_hTab, TabSubclassProc, 1, (DWORD_PTR)m_pEngine);
  TrackControl(m_hTab);

  for (int i = 0; i < numPages; i++) {
    TCITEMW ti = {};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)tabNames[i];
    SendMessageW(m_hTab, TCM_INSERTITEMW, i, (LPARAM)&ti);
  }

  RECT rcContent = { x, y, x + w, y + h };
  TabCtrl_AdjustRect(m_hTab, FALSE, &rcContent);

  // A margin inside the page, on every side.
  //
  // TabCtrl_AdjustRect returns the area right up to the tab control's inner
  // border, so a page laid out to fill it puts its controls flush against the
  // frame: a combo's drop arrow ends up half-cut at the right, and the last
  // line of text is clipped by the bottom. Shane asked for "a min padding to
  // the height and width so buttons and letters don't get cut off", and this
  // is the one place every tabbed tool window passes through.
  //
  // Scaled with the font rather than fixed, because the font is adjustable and
  // a margin that reads right at 26px is invisible at 48.
  // Not square. The sides are where controls actually collide with the frame
  // -- a combo's drop arrow sits hard against the right border -- so those get
  // the full margin. The TOP needs almost nothing, because the tab strip above
  // already separates the page from the frame, and a full margin there is just
  // a band of empty window before the first control. The bottom keeps enough
  // to stop the last line of text being clipped.
  const int side = (std::max)(8, GetLineHeight() / 3);
  rcContent.left += side;
  rcContent.right -= side;
  rcContent.top += 2;
  rcContent.bottom -= (std::max)(6, GetLineHeight() / 4);
  return rcContent;
}

void ToolWindow::ShowPage(int page) {
  int numPages = (int)m_pageCtrls.size();
  if (page < 0 || page >= numPages) return;
  for (int i = 0; i < numPages; i++) {
    if (i == page) {
      for (HWND h : m_pageCtrls[i])
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    } else {
      for (HWND h : m_pageCtrls[i])
        ShowWindow(h, SW_HIDE);
    }
  }
  m_nActivePage = page;
  wchar_t buf[8]; swprintf(buf, 8, L"%d", page);
  WindowStore::I().SetInt(GetINISection(), L"ActiveTab", _wtoi(buf));

  // The page is visible; now let the window correct it.
  //
  // The loop above SHOWS EVERY control registered for the page, which is right
  // for a page whose controls are all meant to be on screen at once and wrong
  // for one that keeps two controls in the same rectangle and picks between
  // them. The Displays window does exactly that -- the layout panel and the
  // detail list share one rect -- so switching to Video Input and back drew
  // both on top of each other. Nothing was wrong with either control; the
  // switch simply undid a decision the base class does not know about.
  //
  // Same reasoning as OnRebuilt: restoring the CONTROLS is not restoring the
  // WINDOW, when the window has state that decides which of them belongs.
  OnPageShown(page);
}

void ToolWindow::SelectInitialTab() {
  if (!m_hTab || m_pageCtrls.empty()) return;
  int numPages = (int)m_pageCtrls.size();
  int tab = WindowStore::I().GetInt(GetINISection(), L"ActiveTab", 0);
  if (tab < 0 || tab >= numPages) tab = 0;
  TabCtrl_SetCurSel(m_hTab, tab);
  ShowPage(tab);
}

void ToolWindow::TrackPageControl(int page, HWND h) {
  if (!h) return;
  if (page >= 0 && page < (int)m_pageCtrls.size())
    m_pageCtrls[page].push_back(h);
  TrackControl(h);
}

bool ToolWindow::IsChecked(int controlID) const {
  HWND hCtrl = GetDlgItem(m_hWnd, controlID);
  return hCtrl ? (bool)(intptr_t)GetPropW(hCtrl, L"Checked") : false;
}

void ToolWindow::SetChecked(int controlID, bool checked) {
  HWND hCtrl = GetDlgItem(m_hWnd, controlID);
  if (hCtrl) {
    SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
    InvalidateRect(hCtrl, NULL, TRUE);
  }
}

void ToolWindow::ResetPosition() {
  if (!m_hWnd || !IsWindow(m_hWnd)) return;
  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  int posX = (screenW - m_nDefaultW) / 2;
  int posY = (screenH - m_nDefaultH) / 2;
  m_nWndW = m_nDefaultW;
  m_nWndH = m_nDefaultH;
  m_bOnTop = false;
  SetWindowPos(m_hWnd, HWND_NOTOPMOST, posX, posY, m_nDefaultW, m_nDefaultH, SWP_SHOWWINDOW);
  RebuildFonts();
}

// ---------------------------------------------------------------------------
// Keeping the user's work across a rebuild
// ---------------------------------------------------------------------------

std::vector<ToolWindow::ControlState> ToolWindow::CaptureControlState() const {
  std::vector<ControlState> saved;
  if (!m_hWnd) return saved;

  const HWND hFocus = GetFocus();
  for (HWND h = GetWindow(m_hWnd, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT)) {
    const int id = GetDlgCtrlID(h);
    if (id <= 0) continue;

    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);

    ControlState s;
    s.id = id;
    s.cls = cls;
    s.hadFocus = (h == hFocus);

    if (_wcsicmp(cls, L"Edit") == 0) {
      const int len = GetWindowTextLengthW(h);
      if (len > 0) {
        s.text.resize((size_t)len + 1);
        GetWindowTextW(h, &s.text[0], len + 1);
        s.text.resize((size_t)len);
        s.hasText = true;
      }
      // EM_GETMODIFY is the whole reason this is safe to do generically: it is
      // true only if the USER changed the text since it was last set
      // programmatically. An edit that DoBuildControls fills from live state
      // comes back false and is left alone, so a deliberate refresh is never
      // overwritten by a stale value.
      s.userModified = SendMessageW(h, EM_GETMODIFY, 0, 0) != 0;
      const DWORD sel = (DWORD)SendMessageW(h, EM_GETSEL, 0, 0);
      s.selStart = (int)LOWORD(sel);
      s.selEnd = (int)HIWORD(sel);
      s.topIndex = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    } else if (_wcsicmp(cls, L"ListBox") == 0) {
      s.selection = (int)SendMessageW(h, LB_GETCURSEL, 0, 0);
      s.topIndex = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0);
    } else if (_wcsicmp(cls, L"ComboBox") == 0) {
      s.selection = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    } else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
      s.selection = ListView_GetNextItem(h, -1, LVNI_SELECTED);
      s.topIndex = ListView_GetTopIndex(h);
    } else {
      // Buttons and labels carry no state a rebuild can lose: owner-draw check
      // and radio state is re-derived from the engine by DoBuildControls, which
      // is where it belongs.
      if (!s.hadFocus) continue;
    }
    saved.push_back(std::move(s));
  }
  return saved;
}

// The control this state belongs to, or NULL.
//
// A matching ID is not enough: a window is free to use one ID for a different
// control on a different page, and putting an edit's text into a list would be
// worse than losing it.
HWND ToolWindow::MatchingControl(const ControlState& s) const {
  HWND h = GetDlgItem(m_hWnd, s.id);
  if (!h) return NULL;
  wchar_t cls[64] = {};
  GetClassNameW(h, cls, 64);
  return _wcsicmp(cls, s.cls.c_str()) == 0 ? h : NULL;
}

// Step 1: where each list was, before the window gets a say.
void ToolWindow::RestoreControlSelections(const std::vector<ControlState>& saved) {
  if (!m_hWnd) return;
  for (const ControlState& s : saved) {
    HWND h = MatchingControl(s);
    if (!h) continue;
    const wchar_t* cls = s.cls.c_str();

    if (_wcsicmp(cls, L"ListBox") == 0) {
      if (s.selection >= 0 && s.selection < (int)SendMessageW(h, LB_GETCOUNT, 0, 0))
        SendMessageW(h, LB_SETCURSEL, (WPARAM)s.selection, 0);
      if (s.topIndex > 0) SendMessageW(h, LB_SETTOPINDEX, (WPARAM)s.topIndex, 0);
    } else if (_wcsicmp(cls, L"ComboBox") == 0) {
      if (s.selection >= 0 && s.selection < (int)SendMessageW(h, CB_GETCOUNT, 0, 0))
        SendMessageW(h, CB_SETCURSEL, (WPARAM)s.selection, 0);
    } else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
      const int count = ListView_GetItemCount(h);
      if (s.selection >= 0 && s.selection < count)
        ListView_SetItemState(h, s.selection, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
      // ListView has no SETTOPINDEX, so scrolling back means making the row
      // that used to be at the top visible again, then the selected row.
      if (s.topIndex > 0 && s.topIndex < count)
        ListView_EnsureVisible(h, s.topIndex, FALSE);
      if (s.selection >= 0 && s.selection < count)
        ListView_EnsureVisible(h, s.selection, TRUE);
    }
  }
}

// Step 3: what the user typed, and the caret they left in it.
void ToolWindow::RestoreControlText(const std::vector<ControlState>& saved) {
  if (!m_hWnd) return;
  HWND hFocusTarget = NULL;
  for (const ControlState& s : saved) {
    HWND h = MatchingControl(s);
    if (!h) continue;
    if (s.hadFocus) hFocusTarget = h;
    if (_wcsicmp(s.cls.c_str(), L"Edit") != 0) continue;

    // Restore what the USER typed, and also restore a pane the rebuild left
    // blank -- the shader import window creates its error box empty and fills
    // it from twenty other places, so a resize used to wipe the compile error
    // you were reading. What is never restored is an edit the window itself
    // refreshed from live state, which is what EM_GETMODIFY tells us apart.
    const bool cameBackEmpty = GetWindowTextLengthW(h) == 0;
    if (s.hasText && (s.userModified || cameBackEmpty)) {
      SetWindowTextW(h, s.text.c_str());
      SendMessageW(h, EM_SETSEL, (WPARAM)s.selStart, (LPARAM)s.selEnd);
      if (s.topIndex > 0) SendMessageW(h, EM_LINESCROLL, 0, (LPARAM)s.topIndex);
      // SetWindowText clears the modify flag; put it back, or the next rebuild
      // would take this for a programmatic value and drop it.
      if (s.userModified) SendMessageW(h, EM_SETMODIFY, TRUE, 0);
    }
  }
  if (hFocusTarget) SetFocus(hFocusTarget);
}

// ---------------------------------------------------------------------------
// Anchored layout
// ---------------------------------------------------------------------------

void ToolWindow::AttachTip(HWND hCtrl, const wchar_t* tip) {
  if (!m_hWnd || !hCtrl || !tip || !*tip) return;
  HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
      m_hWnd, NULL, GetModuleHandle(NULL), NULL);
  TrackTooltip(hTip);
  if (!hTip) return;

  // Copied into a store that outlives this call. Nearly every caller passes a
  // literal, which would be safe, but the ones that matter most here do not:
  // a tooltip carrying a device name or a path is built at runtime.
  m_tipText.emplace_back(tip);

  TTTOOLINFOW ti = { sizeof(ti) };
  ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
  ti.hwnd = m_hWnd;
  ti.uId = (UINT_PTR)hCtrl;
  ti.lpszText = (LPWSTR)m_tipText.back().c_str();
  SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);

  // Long tips wrap rather than running off the screen. A tooltip exists here to
  // carry the sentence a label could not, so it is routinely long.
  SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)MulDiv(420, GetLineHeight(), 26));
}

HWND ToolWindow::CreateLabelTip(HWND hw, const wchar_t* text, const wchar_t* tip,
                                int x, int y, int w, int h, HFONT hFont) {
  HWND hLbl = CreateLabel(hw, text, x, y, w, h, hFont);
  AttachTip(hLbl, tip);
  return hLbl;
}

void ToolWindow::AnchorControl(HWND h, unsigned edges) {
  if (!h || !m_hWnd || !IsWindow(h)) return;

  RECT rc;
  GetWindowRect(h, &rc);
  MapWindowPoints(NULL, m_hWnd, (POINT*)&rc, 2);   // screen -> client

  RECT client;
  GetClientRect(m_hWnd, &client);
  m_anchorClient.cx = client.right;
  m_anchorClient.cy = client.bottom;

  AnchoredControl a;
  a.hwnd = h;
  a.edges = edges;
  a.rect = rc;
  m_anchors.push_back(a);
}

void ToolWindow::ShowTab(int page) {
  if (page < 0) return;
  wchar_t buf[8];
  swprintf(buf, 8, L"%d", page);
  WindowStore::I().SetInt(GetINISection(), L"ActiveTab", _wtoi(buf));
  if (m_hWnd && IsWindow(m_hWnd))
    PostMessage(m_hWnd, WM_MW_SHOW_TAB, (WPARAM)page, 0);
}

void ToolWindow::RenderCentredPos(int& outX, int& outY) const {
  HWND hRender = m_pEngine ? m_pEngine->GetPluginWindow() : nullptr;
  HMONITOR hMon = hRender ? MonitorFromWindow(hRender, MONITOR_DEFAULTTONEAREST)
                          : MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = { sizeof(mi) };
  RECT rc = {};
  const int w = (m_hWnd && GetWindowRect(m_hWnd, &rc)) ? (rc.right - rc.left) : m_nWndW;
  const int h = (m_hWnd && GetWindowRect(m_hWnd, &rc)) ? (rc.bottom - rc.top) : m_nWndH;
  if (GetMonitorInfo(hMon, &mi)) {
    outX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    outY = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
  } else {
    outX = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    outY = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
  }
}

void ToolWindow::MoveTo(int x, int y) {
  if (m_hWnd && IsWindow(m_hWnd))
    PostMessage(m_hWnd, WM_MW_MOVE_TO, (WPARAM)x, (LPARAM)y);
}

void ToolWindow::CentreOnRender() {
  int x = 0, y = 0;
  RenderCentredPos(x, y);
  MoveTo(x, y);
}

void ToolWindow::ClickControl(int ctrlId) {
  if (m_hWnd && IsWindow(m_hWnd))
    PostMessage(m_hWnd, WM_MW_CLICK_CTRL, (WPARAM)ctrlId, 0);
}

void ToolWindow::ReportUiShown(int ctrlId, bool found) {
  if (!m_pEngine) return;
  wchar_t msg[256];
  FormatTo(msg, L"UI_SHOWN|window=%s|ctrl=%d|found=%d",
           GetINISection(), ctrlId, found ? 1 : 0);
  m_pEngine->SendMessageToMDropDX12Remote(msg, true);
}

void ToolWindow::ReportUiClicked(int ctrlId, bool pressed) {
  if (!m_pEngine) return;
  wchar_t msg[256];
  FormatTo(msg, L"UI_CLICKED|window=%s|ctrl=%d|pressed=%d",
           GetINISection(), ctrlId, pressed ? 1 : 0);
  m_pEngine->SendMessageToMDropDX12Remote(msg, true);
}

void ToolWindow::ShowControl(int ctrlId) {
  if (m_hWnd && IsWindow(m_hWnd)) {
    PostMessage(m_hWnd, WM_MW_SHOW_CTRL, (WPARAM)ctrlId, 0);
    return;
  }
  // Not built yet. Remembered, and applied by CreateOnThread once it is.
  m_pendingShowCtrl = ctrlId;
}

void ToolWindow::SetControlsEnabled(bool enable, std::initializer_list<int> keep) {
  for (HWND h : m_childCtrls) {
    if (!h || !IsWindow(h)) continue;
    if (h == m_hTab) continue;

    // Labels are left alone.
    //
    // Not laziness -- a disabled STATIC is drawn by Windows with DSS_DISABLED,
    // which embosses the text using the 3D HIGHLIGHT colour and ignores the
    // one WM_CTLCOLORSTATIC hands back. On a dark theme that comes out WHITE:
    // brighter and more prominent than the enabled text beside it, which is
    // the opposite of what disabling is for. A label has nothing to click
    // anyway; the greyed buttons and fields next to it say the page is off.
    //
    // Disabled EDIT controls are unaffected -- they route through
    // WM_CTLCOLORSTATIC and do dim correctly.
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (_wcsicmp(cls, L"Static") == 0) continue;

    const int id = GetDlgCtrlID(h);
    if (id == GetPinControlID() || id == GetFontPlusControlID() ||
        id == GetFontMinusControlID())
      continue;
    bool kept = false;
    for (int k : keep)
      if (k == id) kept = true;
    EnableWindow(h, enable || kept);
  }
}

void ToolWindow::ApplyAnchors() {
  if (m_anchors.empty() || !m_hWnd) return;

  RECT client;
  GetClientRect(m_hWnd, &client);
  const int dx = client.right - m_anchorClient.cx;
  const int dy = client.bottom - m_anchorClient.cy;
  if (dx == 0 && dy == 0) return;

  // One batch, so the window redraws once instead of once per control. The
  // rebuild path this replaces did a full teardown per mouse-move.
  HDWP dwp = BeginDeferWindowPos((int)m_anchors.size());

  for (const AnchoredControl& a : m_anchors) {
    if (!IsWindow(a.hwnd)) continue;

    RECT r = a.rect;
    const bool left   = (a.edges & kAnchorLeft) != 0;
    const bool right  = (a.edges & kAnchorRight) != 0;
    const bool top    = (a.edges & kAnchorTop) != 0;
    const bool bottom = (a.edges & kAnchorBottom) != 0;

    // Right edge follows the window; if the left edge does not, the whole
    // control slides instead of stretching. Same for the vertical axis.
    if (right) {
      r.right += dx;
      if (!left) r.left += dx;
    }
    if (bottom) {
      r.bottom += dy;
      if (!top) r.top += dy;
    }
    // Anchored to neither side of an axis: stay centred on that axis.
    if (!left && !right) { r.left += dx / 2; r.right += dx / 2; }
    if (!top && !bottom) { r.top += dy / 2; r.bottom += dy / 2; }

    const int w = max(0L, r.right - r.left);
    const int h = max(0L, r.bottom - r.top);

    if (dwp)
      dwp = DeferWindowPos(dwp, a.hwnd, NULL, r.left, r.top, w, h,
                           SWP_NOZORDER | SWP_NOACTIVATE);
    else
      MoveWindow(a.hwnd, r.left, r.top, w, h, TRUE);
  }

  if (dwp) EndDeferWindowPos(dwp);
  InvalidateRect(m_hWnd, NULL, TRUE);
}

void ToolWindow::RebuildFonts() {
  if (!m_hWnd) return;

  // Every control is destroyed and re-created below, and each new one paints
  // itself as it appears -- unthemed first, then again once ApplyDarkTheme has
  // run. On a window that is already on screen (a resize, a font-size change)
  // that reads as the window redrawing itself piece by piece. Holding painting
  // off until the new controls are in place and themed makes it a single swap.
  const bool bVisible = IsWindowVisible(m_hWnd) != FALSE;
  if (bVisible) SendMessageW(m_hWnd, WM_SETREDRAW, FALSE, 0);

  // Save active tab before destroying controls
  int savedTab = m_hTab ? TabCtrl_GetCurSel(m_hTab) : 0;

  // ...and everything else the user was in the middle of.
  const std::vector<ControlState> savedState = CaptureControlState();

  // Destroy all child windows
  HWND hChild = GetWindow(m_hWnd, GW_CHILD);
  while (hChild) {
    HWND hNext = GetWindow(hChild, GW_HWNDNEXT);
    DestroyWindow(hChild);
    hChild = hNext;
  }
  // Tooltips are owned popups rather than children, so the loop above walks
  // straight past them. Left alone they accumulated one leaked window per
  // tooltip per rebuild, on every window, for the life of the process.
  for (HWND hTip : m_tooltips)
    if (IsWindow(hTip)) DestroyWindow(hTip);
  m_tooltips.clear();
  m_tipText.clear();      // the tooltips that pointed into it are gone

  m_childCtrls.clear();
  m_pageCtrls.clear();
  m_anchors.clear();      // the HWNDs they name are gone
  m_hTab = NULL;

  DoBuildControls();
  m_bFirstBuild = false;
  ApplyDarkTheme();

  // Restore tab selection (overrides the default from SelectInitialTab)
  if (m_hTab && savedTab > 0 && savedTab < (int)m_pageCtrls.size()) {
    TabCtrl_SetCurSel(m_hTab, savedTab);
    ShowPage(savedTab);
  }

  // Put the user's work back. Selections first, then the window's own
  // reconciliation, then the typing -- see RestoreControlSelections.
  RestoreControlSelections(savedState);
  OnRebuilt();
  RestoreControlText(savedState);

  if (bVisible) {
    SendMessageW(m_hWnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(m_hWnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
  }
}

//----------------------------------------------------------------------
// Base WndProc — handles common messages, delegates to subclass virtuals
//----------------------------------------------------------------------

// The guard, and nothing else in the frame.
//
// A fault in here cannot be caught anywhere else: a window procedure is a
// kernel-to-user callback, and Windows terminates the process outright with
// STATUS_FATAL_USER_CALLBACK_EXCEPTION rather than letting the exception
// unwind -- no WER dump, no event-log entry, no last line in debug.log. See
// seh_guard.h, and forgejo#75 for the measurement.
//
// It has to be a SEPARATE function from the body. __try/__except cannot share a
// frame with objects needing C++ unwinding, and the body below is full of them.
//
// It CONTINUES afterwards rather than exiting. A tool window is not the render
// path -- losing one message is survivable, and vanishing without a word is the
// failure being fixed. The report is what matters; DefWindowProcW is the least
// surprising thing to return when the real handler did not finish.
LRESULT CALLBACK ToolWindow::BaseWndProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                         LPARAM lParam) {
  __try {
    return BaseWndProcImpl(hWnd, uMsg, wParam, lParam);
  } __except (SehCrashFilter(GetExceptionInformation(),
                             SehContextFor(hWnd, uMsg))) {
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
  }
}

// WHICH window and WHICH message, for the report.
//
// "a tool window faulted" is most of a report short of useful when twenty-odd
// of them exist. The title is read with GetWindowTextW rather than from the
// ToolWindow object, because whatever just faulted may be exactly what is
// unreadable.
const wchar_t* ToolWindow::SehContextFor(HWND hWnd, UINT uMsg) {
  thread_local wchar_t buf[160];
  wchar_t title[96] = L"";
  GetWindowTextW(hWnd, title, 96);
  FormatTo(buf, L"ToolWindow \"%s\" msg=0x%04X", title, uMsg);
  return buf;
}

LRESULT CALLBACK ToolWindow::BaseWndProcImpl(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  // Store 'this' pointer on creation
  if (uMsg == WM_NCCREATE) {
    CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
    if (pcs && pcs->lpCreateParams)
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pcs->lpCreateParams);
  }
  ToolWindow* tw = (ToolWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
  if (!tw) return DefWindowProcW(hWnd, uMsg, wParam, lParam);

  Engine* p = tw->m_pEngine;

  switch (uMsg) {
  case WM_CLOSE:
    DestroyWindow(hWnd);
    return 0;

  case WM_DESTROY:
    {
      tw->SaveWindowPosition();
      // WM_DESTROY fires only when this window is actually closed -- program
      // exit does not close tool windows -- so this marks a deliberate close
      // by the user and nothing else.
      tw->SaveOpenState(false);

      // Let subclass clean up
      tw->DoDestroy();

      // Clean up base resources
      tw->m_hWnd = NULL;
      tw->m_hTab = NULL;
      tw->m_pageCtrls.clear();
      tw->m_childCtrls.clear();
      if (tw->m_hFont) { DeleteObject(tw->m_hFont); tw->m_hFont = NULL; }
      if (tw->m_hFontBold) { DeleteObject(tw->m_hFontBold); tw->m_hFontBold = NULL; }
      if (tw->m_hPinFont) { DeleteObject(tw->m_hPinFont); tw->m_hPinFont = NULL; }
      if (tw->m_hGlyphFont) { DeleteObject(tw->m_hGlyphFont); tw->m_hGlyphFont = NULL; }
    }
    PostQuitMessage(0);
    return 0;

  case WM_EXITSIZEMOVE:
    // Save where the user just put it.
    //
    // WM_DESTROY also saves, but that only fires when this window is closed,
    // and nothing closes tool windows when the program exits -- so a window
    // left open when MDropDX12 quit never remembered its position, and neither
    // did one open when the program was killed or crashed. Writing on drag-end
    // costs one INI write per drag and survives all three.
    tw->SaveWindowPosition();
    return 0;

  case WM_SIZE:
    if (wParam != SIZE_MINIMIZED) {
      RECT rc;
      GetWindowRect(hWnd, &rc);
      const int w = rc.right - rc.left;
      const int h = rc.bottom - rc.top;
      // ShowWindow(SW_SHOW) sends a WM_SIZE carrying the size the window was
      // created at. Acting on it tore down the controls DoBuildControls had
      // just made and built them all over again -- this time on a window that
      // was now on screen, so opening the window looked like it was drawing
      // itself in pieces, one tab page after another. Only a real size change
      // needs a relayout.
      // Maximise/restore must always relayout even when the restored size is
      // unchanged, so the early-out has to know which state we were in.
      const bool bMax = (wParam == SIZE_MAXIMIZED);
      if (!bMax && bMax == tw->m_bWasMaximized &&
          w == tw->m_nWndW && h == tw->m_nWndH) return 0;
      tw->m_bWasMaximized = bMax;
      // Only the restored size is worth remembering.
      if (!bMax) {
        tw->m_nWndW = w;
        tw->m_nWndH = h;
      }
      // Before the subclass: the pin is a base control, and every subclass
      // would otherwise have to remember to move something it never created.
      tw->LayoutBaseControls();
      tw->OnResize();
    }
    return 0;

  case WM_GETMINMAXINFO:
  {
    MINMAXINFO* mmi = (MINMAXINFO*)lParam;
    mmi->ptMinTrackSize.x = tw->GetMinWidth();
    mmi->ptMinTrackSize.y = tw->GetMinHeight();
    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
      mmi->ptMaxTrackSize.x = mi.rcWork.right - mi.rcWork.left;
      mmi->ptMaxTrackSize.y = mi.rcWork.bottom - mi.rcWork.top;
    }
    return 0;
  }

  case WM_MW_REBUILD_FONTS:
    tw->RebuildFonts();
    return 0;

  case WM_MW_BRING_TO_TOP:
  {
    if (IsIconic(hWnd))
      ShowWindow(hWnd, SW_RESTORE);
    // Check if the render window is TOPMOST (fullscreen/borderless mode).
    // If so, keep the tool window TOPMOST too, otherwise it stays behind.
    HWND hRender = tw->m_pEngine->GetPluginWindow();
    bool renderIsTopmost = hRender &&
        (GetWindowLongW(hRender, GWL_EXSTYLE) & WS_EX_TOPMOST);
    // SWP_NOACTIVATE on the raise as well. Without it the z-order change can
    // activate the window by itself, which is a focus steal that survives
    // skipping SetForegroundWindow below. The normal path activates explicitly
    // a few lines down, so nothing is lost by never doing it implicitly.
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!tw->m_bOnTop && !renderIsTopmost)
      SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // The last focus steal, and the one that survived the first fix.
    //
    // This runs when an ALREADY OPEN window is asked to come forward, which is
    // what UI_SHOW does -- so a run that opened a window once and showed it
    // again went through here rather than through Open. Shane saw it: "there
    // was a moment when it stole focus for 500 ms or so", and then "it gave it
    // back", which is the AttachThreadInput pair either side of it.
    //
    // Handing the focus back afterwards does not make it harmless. He types at
    // 80wpm, so half a second is several characters delivered to the wrong
    // window, and those keystrokes land in the window under test.
    if (!(tw->m_pEngine && tw->m_pEngine->m_bTestingMode)) {
      // Attach input to foreground thread so SetForegroundWindow succeeds
      // from this non-foreground ToolWindow thread
      DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
      DWORD myThread = GetCurrentThreadId();
      if (fgThread != myThread)
        AttachThreadInput(myThread, fgThread, TRUE);
      SetForegroundWindow(hWnd);
      if (fgThread != myThread)
        AttachThreadInput(myThread, fgThread, FALSE);
    }
    return 0;
  }

  case WM_MW_RESET_WINDOW:
    tw->ResetPosition();
    return 0;

  // ── Sliders ──
  case WM_HSCROLL:
  {
    HWND hTrack = (HWND)lParam;
    int id = GetDlgCtrlID(hTrack);
    int pos = (int)SendMessage(hTrack, TBM_GETPOS, 0, 0);
    LRESULT r = tw->DoHScroll(hWnd, id, pos);
    if (r != -1) return r;
    break;
  }

  // ── Notifications ──
  case WM_NOTIFY:
  {
    NMHDR* pnm = (NMHDR*)lParam;
    // Tab selection change (handled by base for all tabbed windows)
    if (tw->m_hTab && pnm->hwndFrom == tw->m_hTab && pnm->code == TCN_SELCHANGE) {
      tw->ShowPage(TabCtrl_GetCurSel(pnm->hwndFrom));
      return 0;
    }
    // ListView header dark theme custom draw (centralized for all ToolWindow ListViews)
    if (p->IsDarkTheme() && pnm->code == NM_CUSTOMDRAW) {
      HWND hParent = GetParent(pnm->hwndFrom);
      if (hParent) {
        wchar_t szClass[32];
        GetClassNameW(hParent, szClass, 32);
        if (_wcsicmp(szClass, WC_LISTVIEWW) == 0) {
          bool handled = false;
          LRESULT result = PaintDarkListViewHeader(pnm, lParam, hParent,
            p->m_colSettingsCtrlBg, p->m_colSettingsBorder, p->m_colSettingsText, &handled);
          if (handled) return result;
        }
      }
    }
    LRESULT r = tw->DoNotify(hWnd, pnm);
    if (r != -1) return r;
    break;
  }

  // ── Commands ──
  case WM_COMMAND:
  {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    // Pin button (common)
    // Resize-to-fit. Before the pin, and a plain id compare: this is the one
    // base control every tool window shares an id for.
    if (id == IDC_MW_TOOLWIN_FIT && code == BN_CLICKED) {
      tw->FitToContents();
      return 0;
    }
    if (id == tw->GetPinControlID() && code == BN_CLICKED) {
      tw->m_bOnTop = !tw->m_bOnTop;
      SetWindowPos(hWnd, tw->m_bOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      InvalidateRect((HWND)lParam, NULL, TRUE);
      return 0;
    }

    // Font + (common)
    if (id == tw->GetFontPlusControlID() && code == BN_CLICKED) {
      if (p->m_nSettingsFontSize > -32) {
        p->m_nSettingsFontSize -= 2;
        tw->RebuildFonts();
        p->BroadcastFontSync(hWnd);
      }
      return 0;
    }

    // Font - (common)
    if (id == tw->GetFontMinusControlID() && code == BN_CLICKED) {
      if (p->m_nSettingsFontSize < -12) {
        p->m_nSettingsFontSize += 2;
        tw->RebuildFonts();
        p->BroadcastFontSync(hWnd);
      }
      return 0;
    }

    // Owner-draw BN_CLICKED: auto-toggle checkbox and radio state.
    // Checkboxes and radio groups are toggled here so subclasses don't need to.
    if (code == BN_CLICKED) {
      HWND hCtrl = (HWND)lParam;
      bool bIsCheckbox = (bool)(intptr_t)GetPropW(hCtrl, L"IsCheckbox");
      if (bIsCheckbox) {
        bool wasChecked = (bool)(intptr_t)GetPropW(hCtrl, L"Checked");
        SetPropW(hCtrl, L"Checked", (HANDLE)(intptr_t)(wasChecked ? 0 : 1));
        InvalidateRect(hCtrl, NULL, TRUE);
      }

      bool bIsRadio = (bool)(intptr_t)GetPropW(hCtrl, L"IsRadio");
      if (bIsRadio) {
        int group = (int)(intptr_t)GetPropW(hCtrl, L"RadioGroup");
        if (group != 0) {
          for (HWND hChild : tw->m_childCtrls) {
            if ((bool)(intptr_t)GetPropW(hChild, L"IsRadio") &&
                (int)(intptr_t)GetPropW(hChild, L"RadioGroup") == group) {
              SetPropW(hChild, L"Checked", (HANDLE)(intptr_t)(hChild == hCtrl ? 1 : 0));
              InvalidateRect(hChild, NULL, TRUE);
            }
          }
        }
      }
    }

    // Delegate to subclass
    LRESULT r = tw->DoCommand(hWnd, id, code, lParam);
    if (r != -1) return r;
    break;
  }

  // ── Dark theme painting (delegated to shared helpers) ──
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORDLG:
  {
    LRESULT lr = HandleDarkCtlColor(p, uMsg, wParam, lParam);
    if (lr) return lr;
    break;
  }

  case WM_DRAWITEM:
  {
    DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
    // Pin button — ToolWindow-specific (accesses tw->m_bOnTop, tw->m_hPinFont)
    if (pDIS && pDIS->CtlType == ODT_BUTTON && (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"IsPinBtn")) {
      HDC hdc = pDIS->hDC;
      RECT rc = pDIS->rcItem;
      bool pressed = (pDIS->itemState & ODS_SELECTED) != 0;
      bool pinned = tw->m_bOnTop;
      COLORREF bg = p->IsDarkTheme() ? p->m_colSettingsBg : GetSysColor(COLOR_BTNFACE);
      HBRUSH hBr = CreateSolidBrush(bg);
      FillRect(hdc, &rc, hBr);
      DeleteObject(hBr);
      SetBkMode(hdc, TRANSPARENT);

      // This path draws every Segoe MDL2 glyph button in the title row, not
      // just the pin, so it takes the glyph from the CONTROL rather than
      // naming one. It used to draw a hardcoded \xE718, which is how the new
      // resize-to-fit button came out as a second pin.
      const bool isPin = (int)pDIS->CtlID == tw->GetPinControlID();
      COLORREF pinCol = isPin
        ? (pinned ? (p->IsDarkTheme() ? RGB(100, 180, 255) : RGB(0, 100, 200))
                  : (p->IsDarkTheme() ? RGB(120, 120, 120) : RGB(160, 160, 160)))
        // Anything else has no on/off state to report, so it takes the
        // ordinary foreground rather than the pin's blue.
        : (p->IsDarkTheme() ? RGB(170, 170, 170) : RGB(90, 90, 90));
      SetTextColor(hdc, pinCol);
      HFONT use = isPin ? tw->m_hPinFont : tw->m_hGlyphFont;
      HFONT hOld = use ? (HFONT)SelectObject(hdc, use) : NULL;
      RECT textRc = rc;
      if (pressed) OffsetRect(&textRc, 1, 1);
      wchar_t glyph[8] = {};
      GetWindowTextW(pDIS->hwndItem, glyph, 8);
      if (!glyph[0]) { glyph[0] = L'\xE718'; glyph[1] = 0; }
      DrawTextW(hdc, glyph, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (hOld) SelectObject(hdc, hOld);
      return TRUE;
    }
    // Everything else: tabs, checkboxes, radios, buttons, swatches
    LRESULT lr = HandleDarkDrawItem(p, pDIS);
    if (lr) return lr;
    break;
  }

  case WM_ERASEBKGND:
    return HandleDarkEraseBkgnd(p, hWnd, (HDC)wParam);

  case WM_SETTINGCHANGE:
    if (p->m_nThemeMode == Engine::THEME_SYSTEM && lParam &&
        _wcsicmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
      p->LoadSettingsThemeFromINI();
      tw->ApplyDarkTheme();
    }
    break;

  case WM_CONTEXTMENU:
  {
    LRESULT r = tw->DoContextMenu(hWnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    if (r != -1) return r;
    break;
  }

  case WM_MW_SHOW_TAB:
    // Posted by ShowTab so the switch happens on the window's own thread.
    if (tw->m_hTab) {
      TabCtrl_SetCurSel(tw->m_hTab, (int)wParam);
      tw->ShowPage((int)wParam);
    }
    return 0;

  case WM_MW_MOVE_TO:
    // Posted so the move happens on the window's own thread: cross-thread
    // SetWindowPos can silently fail, which is why OnAlreadyOpen posts too.
    SetWindowPos(hWnd, NULL, (int)wParam, (int)lParam, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;

  case WM_MW_CLICK_CTRL:
  {
    HWND hCtrl = GetDlgItem(hWnd, (int)wParam);
    if (!hCtrl || !IsWindowEnabled(hCtrl)) {
      // A disabled control is not a missing one, but pressing it would do
      // nothing either -- and silently doing nothing is what this whole
      // two-stage answer exists to avoid.
      tw->ReportUiClicked((int)wParam, false);
      return 0;
    }
    SendMessage(hWnd, WM_COMMAND,
                MAKEWPARAM((WORD)wParam, BN_CLICKED), (LPARAM)hCtrl);
    tw->ReportUiClicked((int)wParam, true);
    return 0;
  }

  case WM_MW_SHOW_CTRL:
  {
    HWND hCtrl = GetDlgItem(hWnd, (int)wParam);
    if (!hCtrl) {
      // Said out loud. The caller cannot see the window, so silence here reads
      // as success and leaves a wrong control id undiscovered.
      tw->ReportUiShown((int)wParam, false);
      return 0;
    }
    // Find the page it is on and switch to that first -- focusing a control on
    // a hidden page would put the caret somewhere nobody can see.
    for (size_t page = 0; page < tw->m_pageCtrls.size(); page++) {
      bool here = false;
      for (HWND h : tw->m_pageCtrls[page])
        if (h == hCtrl) here = true;
      if (!here) continue;
      if (tw->m_hTab) TabCtrl_SetCurSel(tw->m_hTab, (int)page);
      tw->ShowPage((int)page);
      break;
    }
    if (IsWindowEnabled(hCtrl)) SetFocus(hCtrl);
    tw->ReportUiShown((int)wParam, true);
    return 0;
  }

  default:
  {
    LRESULT r = tw->DoMessage(hWnd, uMsg, wParam, lParam);
    if (r != -1) return r;
    break;
  }
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

//----------------------------------------------------------------------
// Shared Action Edit Dialog — used by both Button Board and Hotkeys windows.
// Subclass of ModalDialog; dark theme handled automatically by the base class.
//----------------------------------------------------------------------

class ActionEditDialog : public ModalDialog {
  ActionEditData& m_data;

  const wchar_t* GetDialogTitle() const override {
    return m_data.isBuiltInHotkey ? L"Edit Hotkey" : L"Edit Action";
  }
  const wchar_t* GetDialogClass() const override {
    return L"MDropActionEditDlg";
  }

  static bool IsMouseVK(UINT v) {
    return v == VK_LBUTTON || v == VK_RBUTTON || v == VK_MBUTTON ||
           v == VK_XBUTTON1 || v == VK_XBUTTON2;
  }
  static int MouseVKToIdx(UINT v) {
    if (v == VK_LBUTTON) return 1; if (v == VK_RBUTTON) return 2;
    if (v == VK_MBUTTON) return 3; if (v == VK_XBUTTON1) return 4;
    if (v == VK_XBUTTON2) return 5; return 0;
  }
  static void PopulateMouseCombo(HWND hMouseCombo) {
    SendMessageW(hMouseCombo, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    SendMessageW(hMouseCombo, CB_ADDSTRING, 0, (LPARAM)L"Left Mouse");
    SendMessageW(hMouseCombo, CB_ADDSTRING, 0, (LPARAM)L"Right Mouse");
    SendMessageW(hMouseCombo, CB_ADDSTRING, 0, (LPARAM)L"Middle Mouse");
    SendMessageW(hMouseCombo, CB_ADDSTRING, 0, (LPARAM)L"X1 Mouse");
    SendMessageW(hMouseCombo, CB_ADDSTRING, 0, (LPARAM)L"X2 Mouse");
  }
  static void SetHotkeyCtrl(HWND hHK, UINT mod, UINT vk) {
    UINT hkMod = 0;
    if (mod & MOD_ALT)     hkMod |= HOTKEYF_ALT;
    if (mod & MOD_CONTROL) hkMod |= HOTKEYF_CONTROL;
    if (mod & MOD_SHIFT)   hkMod |= HOTKEYF_SHIFT;
    SendMessageW(hHK, HKM_SETHOTKEY, MAKEWORD(vk, hkMod), 0);
  }

  // What the two capture controls currently say. A method rather than the
  // lambda it used to be inside the OK handler, because the live conflict line
  // has to read exactly what OK will read -- a second copy of this would be a
  // warning about a binding the dialog is not actually about to apply.
  void ReadBinding(int hkID, int mouseID, UINT& outMod, UINT& outVK) const {
    static const UINT mouseVKs[] = { 0, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                                     VK_XBUTTON1, VK_XBUTTON2 };
    HWND hDlg = const_cast<ActionEditDialog*>(this)->GetHWND();
    HWND hMouse = GetDlgItem(hDlg, mouseID);
    int mouseIdx = hMouse ? (int)SendMessageW(hMouse, CB_GETCURSEL, 0, 0) : 0;
    if (mouseIdx > 0 && mouseIdx < (int)_countof(mouseVKs)) {
      outVK = mouseVKs[mouseIdx];
      outMod = 0;
      return;
    }
    HWND hHK = GetDlgItem(hDlg, hkID);
    DWORD hk = hHK ? (DWORD)SendMessageW(hHK, HKM_GETHOTKEY, 0, 0) : 0;
    outVK = LOBYTE(LOWORD(hk));
    UINT hkMod = HIBYTE(LOWORD(hk));
    outMod = 0;
    if (hkMod & HOTKEYF_ALT)     outMod |= MOD_ALT;
    if (hkMod & HOTKEYF_CONTROL) outMod |= MOD_CONTROL;
    if (hkMod & HOTKEYF_SHIFT)   outMod |= MOD_SHIFT;
  }

  // The live "this would unbind something" line, refreshed as keys are typed.
  //
  // Only the Hotkeys window sets selfHotkeyId / selfUserId, so the Button
  // Board -- which shares this dialog but edits buttons rather than rows in
  // the hotkey table -- shows nothing here and behaves exactly as before.
  void RefreshConflictLine() {
    HWND hCtl = GetDlgItem(GetHWND(), IDC_AE_CONFLICT);
    if (!hCtl || !m_data.pEngine) return;
    if (m_data.selfHotkeyId < 0 && m_data.selfUserId < 0) return;

    UINT mod = 0, vk = 0;
    std::wstring text;
    if (m_data.isBuiltInHotkey) {
      ReadBinding(IDC_AE_HOTKEY, IDC_AE_MOUSE, mod, vk);
      text = m_data.pEngine->DescribeHotkeyConflicts(
                 m_data.selfHotkeyId, m_data.selfUserId, mod, vk, ::HKSCOPE_LOCAL);
      UINT gmod = 0, gvk = 0;
      ReadBinding(IDC_AE_GLOBAL_HOTKEY, IDC_AE_GLOBAL_MOUSE, gmod, gvk);
      const std::wstring g = m_data.pEngine->DescribeHotkeyConflicts(
                 m_data.selfHotkeyId, m_data.selfUserId, gmod, gvk, ::HKSCOPE_GLOBAL);
      if (!g.empty()) text = text.empty() ? g : (text + L"  " + g);
    } else {
      ReadBinding(IDC_AE_HOTKEY, IDC_AE_MOUSE, mod, vk);
      const HotkeyScope sc = IsChecked(IDC_AE_SCOPE) ? ::HKSCOPE_GLOBAL
                                                     : ::HKSCOPE_LOCAL;
      text = m_data.pEngine->DescribeHotkeyConflicts(
                 m_data.selfHotkeyId, m_data.selfUserId, mod, vk, sc);
    }
    // \u26A0 as an escape, not the character itself. This file has no BOM,
    // so a literal UTF-8 warning sign in a wide string literal is read as
    // ANSI by the compiler and reaches the screen as mojibake -- it shipped
    // that way once and the capture is what caught it.
    SetWindowTextW(hCtl, text.empty() ? L"" : (L"\u26A0  " + text).c_str());
  }

  void DoBuildControls(int clientW, int clientH) override {
    HFONT hFont = GetFont();
    HWND hDlg = GetHWND();
    auto L = GetBaseLayout();

    int margin = L.margin;
    int cw = clientW - 2 * margin;
    int y = margin;
    int labelW = L.labelW;
    int lineH = L.lineH;
    int gap = L.gap;
    int browseW = 70;

    if (m_data.isBuiltInHotkey) {
      // Built-in hotkey: read-only action name
      TrackControl(CreateLabel(hDlg, L"Action:", margin, y + 2, labelW, lineH, hFont));
      HWND hAction = CreateEdit(hDlg, m_data.actionName.c_str(), IDC_AE_ACTION_LABEL,
        margin + labelW, y, cw - labelW, lineH, hFont, ES_READONLY);
      TrackControl(hAction);
      y += lineH + gap;
    } else {
      // Action type dropdown
      TrackControl(CreateLabel(hDlg, L"Action:", margin, y + 2, labelW, lineH, hFont));
      HWND hType = CreateCombo(hDlg, IDC_AE_ACTION_TYPE,
        margin + labelW, y, cw - labelW, 200, hFont,
        CBS_DROPDOWNLIST, true, WS_EX_CLIENTEDGE);
      TrackControl(hType);
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"None");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Load Preset");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Push Sprite");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Script Command");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Launch Message");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Run Script File");
      SendMessageW(hType, CB_ADDSTRING, 0, (LPARAM)L"Launch App");
      SendMessageW(hType, CB_SETCURSEL, (int)m_data.actionType, 0);
      y += lineH + gap;

      // Label
      TrackControl(CreateLabel(hDlg, L"Label:", margin, y + 2, labelW, lineH, hFont));
      TrackControl(CreateEdit(hDlg, m_data.label.c_str(), IDC_AE_LABEL,
        margin + labelW, y, cw - labelW, lineH, hFont));
      y += lineH + gap;

      // Payload (multiline) + Browse
      TrackControl(CreateLabel(hDlg, L"Command:", margin, y + 2, labelW, lineH, hFont));

      // Convert pipes to newlines for display (script commands)
      std::wstring displayPayload = m_data.payload;
      if (m_data.actionType == ButtonAction::ScriptCommand ||
          m_data.actionType == ButtonAction::LaunchMessage) {
        for (size_t pos = 0; (pos = displayPayload.find(L'|', pos)) != std::wstring::npos; pos += 2)
          displayPayload.replace(pos, 1, L"\r\n");
      }

      int cmdH = lineH * 4;
      TrackControl(CreateEdit(hDlg, displayPayload.c_str(), IDC_AE_PAYLOAD,
        margin + labelW, y, cw - labelW - browseW - 6, cmdH, hFont,
        ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL));
      TrackControl(CreateBtn(hDlg, L"Browse...", IDC_AE_BROWSE,
        margin + cw - browseW, y, browseW, lineH, hFont));
      y += cmdH + gap;
    }

    // Key binding section
    if (m_data.showKeyBinding) {
      // Ensure HOTKEY_CLASS is available
      INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_HOTKEY_CLASS };
      InitCommonControlsEx(&icc);

      int clearW = 50;
      HINSTANCE hInst = GetModuleHandle(NULL);

      if (m_data.isBuiltInHotkey) {
        // ── Built-in hotkey: dual binding (Local + Global) ──
        y += gap;

        // Local key row
        TrackControl(CreateLabel(hDlg, L"Local Key:", margin, y + 2, labelW, lineH, hFont));
        HWND hHK = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
          margin + labelW, y, cw - labelW - clearW - 6, lineH, hDlg,
          (HMENU)(INT_PTR)IDC_AE_HOTKEY, hInst, NULL);
        SendMessageW(hHK, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hHK);
        TrackControl(CreateBtn(hDlg, L"Clear", IDC_AE_CLEAR_KEY,
          margin + cw - clearW, y, clearW, lineH, hFont));
        y += lineH + gap;

        if (hHK && m_data.vk != 0 && !IsMouseVK(m_data.vk))
          SetHotkeyCtrl(hHK, m_data.modifiers, m_data.vk);

        // Local mouse dropdown
        TrackControl(CreateLabel(hDlg, L"Mouse:", margin, y + 2, labelW, lineH, hFont));
        HWND hMouse = CreateCombo(hDlg, IDC_AE_MOUSE,
          margin + labelW, y, cw - labelW - clearW - 6, 200, hFont,
          CBS_DROPDOWNLIST, true, WS_EX_CLIENTEDGE);
        TrackControl(hMouse);
        PopulateMouseCombo(hMouse);
        SendMessageW(hMouse, CB_SETCURSEL, MouseVKToIdx(m_data.vk), 0);
        y += lineH + gap + gap;

        // Global key row
        TrackControl(CreateLabel(hDlg, L"Global Key:", margin, y + 2, labelW, lineH, hFont));
        HWND hGHK = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
          margin + labelW, y, cw - labelW - clearW - 6, lineH, hDlg,
          (HMENU)(INT_PTR)IDC_AE_GLOBAL_HOTKEY, hInst, NULL);
        SendMessageW(hGHK, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hGHK);
        TrackControl(CreateBtn(hDlg, L"Clear", IDC_AE_GLOBAL_CLEAR,
          margin + cw - clearW, y, clearW, lineH, hFont));
        y += lineH + gap;

        if (hGHK && m_data.globalVK != 0 && !IsMouseVK(m_data.globalVK))
          SetHotkeyCtrl(hGHK, m_data.globalMod, m_data.globalVK);

        // Global mouse dropdown
        TrackControl(CreateLabel(hDlg, L"Mouse:", margin, y + 2, labelW, lineH, hFont));
        HWND hGMouse = CreateCombo(hDlg, IDC_AE_GLOBAL_MOUSE,
          margin + labelW, y, cw - labelW - clearW - 6, 200, hFont,
          CBS_DROPDOWNLIST, true, WS_EX_CLIENTEDGE);
        TrackControl(hGMouse);
        PopulateMouseCombo(hGMouse);
        SendMessageW(hGMouse, CB_SETCURSEL, MouseVKToIdx(m_data.globalVK), 0);
        y += lineH + gap;

      } else {
        // ── User hotkey: single binding with scope checkbox ──
        y += gap;
        TrackControl(CreateLabel(hDlg, L"Key:", margin, y + 2, labelW, lineH, hFont));
        HWND hHK = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, NULL,
          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
          margin + labelW, y, cw - labelW - clearW - 6, lineH, hDlg,
          (HMENU)(INT_PTR)IDC_AE_HOTKEY, hInst, NULL);
        SendMessageW(hHK, WM_SETFONT, (WPARAM)hFont, TRUE);
        TrackControl(hHK);
        TrackControl(CreateBtn(hDlg, L"Clear", IDC_AE_CLEAR_KEY,
          margin + cw - clearW, y, clearW, lineH, hFont));
        y += lineH + gap;

        if (hHK && m_data.vk != 0 && !IsMouseVK(m_data.vk))
          SetHotkeyCtrl(hHK, m_data.modifiers, m_data.vk);

        // Mouse button dropdown
        TrackControl(CreateLabel(hDlg, L"Mouse:", margin, y + 2, labelW, lineH, hFont));
        HWND hMouse = CreateCombo(hDlg, IDC_AE_MOUSE,
          margin + labelW, y, cw - labelW - clearW - 6, 200, hFont,
          CBS_DROPDOWNLIST, true, WS_EX_CLIENTEDGE);
        TrackControl(hMouse);
        PopulateMouseCombo(hMouse);
        SendMessageW(hMouse, CB_SETCURSEL, MouseVKToIdx(m_data.vk), 0);
        y += lineH + gap;

        // Scope checkbox (owner-draw via CreateCheck)
        TrackControl(CreateCheck(hDlg, L"Global (system-wide)", IDC_AE_SCOPE,
          margin + labelW, y, cw - labelW, lineH, hFont,
          m_data.scope == HKSCOPE_GLOBAL));
        y += lineH + gap;
      }
    }

    // Live conflict line. Two lines tall because an action name plus a
    // combination rarely fits one, and always present rather than created on
    // demand -- a control that appears would resize the dialog under the
    // pointer as the user types a key.
    if (m_data.showKeyBinding &&
        (m_data.selfHotkeyId >= 0 || m_data.selfUserId >= 0)) {
      // CreateLabel takes no control id, and this one has to be findable by
      // GetDlgItem to be rewritten as keys are typed.
      HWND hConflict = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, y, cw, lineH * 2, hDlg,
        (HMENU)(INT_PTR)IDC_AE_CONFLICT, GetModuleHandle(NULL), NULL);
      if (hConflict) SendMessageW(hConflict, WM_SETFONT, (WPARAM)hFont, TRUE);
      TrackControl(hConflict);
      y += lineH * 2 + gap;
    }

    y += gap;

    // OK / Cancel
    int btnW = 80;
    int btnH = lineH + 4;
    int totalBtnW = btnW * 2 + 12;
    int btnX = margin + (cw - totalBtnW) / 2;
    TrackControl(CreateBtn(hDlg, L"OK", IDOK, btnX, y, btnW, btnH, hFont));
    TrackControl(CreateBtn(hDlg, L"Cancel", IDCANCEL, btnX + btnW + 12, y, btnW, btnH, hFont));
    y += btnH + margin;

    FitToContent(clientW, y);

    // The binding being edited may ALREADY clash -- it can be one that was
    // assigned before this warning existed -- so the line is filled in on the
    // way up, not only when something changes.
    RefreshConflictLine();
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    HWND hDlg = GetHWND();

    // Scope decides WHICH namespace the key is checked against: local and
    // global are resolved by different mechanisms and never shadow each other.
    if (id == IDC_AE_SCOPE) RefreshConflictLine();

    // Clear local key
    if (id == IDC_AE_CLEAR_KEY && code == BN_CLICKED) {
      HWND hHK = GetDlgItem(hDlg, IDC_AE_HOTKEY);
      if (hHK) SendMessageW(hHK, HKM_SETHOTKEY, 0, 0);
      HWND hMouse = GetDlgItem(hDlg, IDC_AE_MOUSE);
      if (hMouse) SendMessageW(hMouse, CB_SETCURSEL, 0, 0);
      RefreshConflictLine();
      return 0;
    }

    // Clear global key
    if (id == IDC_AE_GLOBAL_CLEAR && code == BN_CLICKED) {
      HWND hGHK = GetDlgItem(hDlg, IDC_AE_GLOBAL_HOTKEY);
      if (hGHK) SendMessageW(hGHK, HKM_SETHOTKEY, 0, 0);
      HWND hGMouse = GetDlgItem(hDlg, IDC_AE_GLOBAL_MOUSE);
      if (hGMouse) SendMessageW(hGMouse, CB_SETCURSEL, 0, 0);
      RefreshConflictLine();
      return 0;
    }

    // Local keyboard key changed — clear local mouse dropdown
    if (id == IDC_AE_HOTKEY && code == EN_CHANGE) {
      HWND hHK = GetDlgItem(hDlg, IDC_AE_HOTKEY);
      DWORD hk = hHK ? (DWORD)SendMessageW(hHK, HKM_GETHOTKEY, 0, 0) : 0;
      if (LOBYTE(LOWORD(hk)) != 0) {
        HWND hMouse = GetDlgItem(hDlg, IDC_AE_MOUSE);
        if (hMouse) SendMessageW(hMouse, CB_SETCURSEL, 0, 0);
      }
      RefreshConflictLine();
      return 0;
    }

    // Global keyboard key changed — clear global mouse dropdown
    if (id == IDC_AE_GLOBAL_HOTKEY && code == EN_CHANGE) {
      HWND hGHK = GetDlgItem(hDlg, IDC_AE_GLOBAL_HOTKEY);
      DWORD hk = hGHK ? (DWORD)SendMessageW(hGHK, HKM_GETHOTKEY, 0, 0) : 0;
      if (LOBYTE(LOWORD(hk)) != 0) {
        HWND hGMouse = GetDlgItem(hDlg, IDC_AE_GLOBAL_MOUSE);
        if (hGMouse) SendMessageW(hGMouse, CB_SETCURSEL, 0, 0);
      }
      RefreshConflictLine();
      return 0;
    }

    // Local mouse dropdown changed — clear local keyboard key
    if (id == IDC_AE_MOUSE && code == CBN_SELCHANGE) {
      HWND hMouse = GetDlgItem(hDlg, IDC_AE_MOUSE);
      int sel = hMouse ? (int)SendMessageW(hMouse, CB_GETCURSEL, 0, 0) : 0;
      if (sel > 0) {
        HWND hHK = GetDlgItem(hDlg, IDC_AE_HOTKEY);
        if (hHK) SendMessageW(hHK, HKM_SETHOTKEY, 0, 0);
      }
      RefreshConflictLine();
      return 0;
    }

    // Global mouse dropdown changed — clear global keyboard key
    if (id == IDC_AE_GLOBAL_MOUSE && code == CBN_SELCHANGE) {
      HWND hGMouse = GetDlgItem(hDlg, IDC_AE_GLOBAL_MOUSE);
      int sel = hGMouse ? (int)SendMessageW(hGMouse, CB_GETCURSEL, 0, 0) : 0;
      if (sel > 0) {
        HWND hGHK = GetDlgItem(hDlg, IDC_AE_GLOBAL_HOTKEY);
        if (hGHK) SendMessageW(hGHK, HKM_SETHOTKEY, 0, 0);
      }
      RefreshConflictLine();
      return 0;
    }

    if (id == IDC_AE_BROWSE && code == BN_CLICKED) {
      HWND hType = GetDlgItem(hDlg, IDC_AE_ACTION_TYPE);
      int sel = hType ? (int)SendMessageW(hType, CB_GETCURSEL, 0, 0) : -1;
      ButtonAction act = (sel >= 0) ? (ButtonAction)sel : m_data.actionType;

      wchar_t szFile[MAX_PATH] = {};
      OPENFILENAMEW ofn = {};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hDlg;
      ofn.lpstrFile = szFile;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

      switch (act) {
      case ButtonAction::LoadPreset:
        ofn.lpstrFilter = L"Presets (*.milk;*.milk2;*.milk3)\0*.milk;*.milk2;*.milk3\0All Files\0*.*\0";
        ofn.lpstrTitle = L"Select Preset";
        break;
      case ButtonAction::RunScript:
        ofn.lpstrFilter = L"Script Files (*.txt;*.mws)\0*.txt;*.mws\0All Files\0*.*\0";
        ofn.lpstrTitle = L"Select Script File";
        break;
      case ButtonAction::LaunchApp:
        ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
        ofn.lpstrTitle = L"Select Application";
        break;
      default:
        ofn.lpstrFilter = L"Script Files (*.txt;*.mws)\0*.txt;*.mws\0All Files (*.*)\0*.*\0";
        ofn.lpstrTitle = L"Select File";
        break;
      }

      if (m_pEngine) {
        static std::wstring s_initDir;
        s_initDir = m_pEngine->m_szBaseDir;
        ofn.lpstrInitialDir = s_initDir.c_str();
      }

      if (GetOpenFileNameW(&ofn))
        SetWindowTextW(GetDlgItem(hDlg, IDC_AE_PAYLOAD), szFile);
      return 0;
    }

    if (id == IDOK && code == BN_CLICKED) {
      // Read action + payload (user/button mode)
      if (!m_data.isBuiltInHotkey) {
        HWND hType = GetDlgItem(hDlg, IDC_AE_ACTION_TYPE);
        if (hType) {
          int sel = (int)SendMessageW(hType, CB_GETCURSEL, 0, 0);
          m_data.actionType = (ButtonAction)sel;
        }

        wchar_t buf[512] = {};
        GetDlgItemTextW(hDlg, IDC_AE_LABEL, buf, 512);
        m_data.label = buf;

        // Read payload
        HWND hPayload = GetDlgItem(hDlg, IDC_AE_PAYLOAD);
        int payLen = GetWindowTextLengthW(hPayload);
        std::wstring payload(payLen + 1, L'\0');
        GetWindowTextW(hPayload, &payload[0], payLen + 1);
        payload.resize(payLen);

        // For script/message actions, convert newlines to pipes
        if (m_data.actionType == ButtonAction::ScriptCommand ||
            m_data.actionType == ButtonAction::LaunchMessage) {
          size_t pos = 0;
          while ((pos = payload.find(L"\r\n", pos)) != std::wstring::npos)
            payload.replace(pos, 2, L"|");
          pos = 0;
          while ((pos = payload.find(L'\n', pos)) != std::wstring::npos)
            payload.replace(pos, 1, L"|");
          while (!payload.empty() && payload.back() == L'|')
            payload.pop_back();
        }
        m_data.payload = payload;
      }

      // Read key binding
      if (m_data.showKeyBinding) {
        // Read local binding. ReadBinding is a member so the live conflict
        // line reads exactly what OK reads.
        ReadBinding(IDC_AE_HOTKEY, IDC_AE_MOUSE, m_data.modifiers, m_data.vk);

        if (m_data.isBuiltInHotkey) {
          // Read global binding
          ReadBinding(IDC_AE_GLOBAL_HOTKEY, IDC_AE_GLOBAL_MOUSE, m_data.globalMod, m_data.globalVK);
          // Mouse buttons can't be registered as global hotkeys
          HWND hGMouse = GetDlgItem(hDlg, IDC_AE_GLOBAL_MOUSE);
          int gMouseIdx = hGMouse ? (int)SendMessageW(hGMouse, CB_GETCURSEL, 0, 0) : 0;
          if (gMouseIdx > 0) { m_data.globalVK = 0; m_data.globalMod = 0; }
        } else {
          // User hotkeys: single binding with scope
          m_data.scope = IsChecked(IDC_AE_SCOPE) ? HKSCOPE_GLOBAL : HKSCOPE_LOCAL;
          HWND hMouse = GetDlgItem(hDlg, IDC_AE_MOUSE);
          int mouseIdx = hMouse ? (int)SendMessageW(hMouse, CB_GETCURSEL, 0, 0) : 0;
          if (mouseIdx > 0) m_data.scope = HKSCOPE_LOCAL;
        }
      }

      m_data.accepted = true;
      EndDialog(true);
      return 0;
    }

    if (id == IDCANCEL && code == BN_CLICKED) {
      EndDialog(false);
      return 0;
    }

    return -1;
  }

public:
  ActionEditDialog(Engine* pEngine, ActionEditData& data)
    : ModalDialog(pEngine), m_data(data) {}
};

bool ShowActionEditDialog(HWND hParent, ActionEditData& data)
{
  ActionEditDialog dlg(data.pEngine, data);
  // Initial client size — DoBuildControls resizes height to fit content
  bool ok = dlg.Show(hParent, 420, 500);
  return ok && data.accepted;
}

//----------------------------------------------------------------------
// PromptForName — one label, one editable dropdown, OK / Cancel
//
// Naming a VFX profile used to go through GetSaveFileNameW, because a profile
// was a file. It is a key in a store now, so a file picker would be offering
// to put it somewhere that no longer means anything -- but a bare edit box
// would be worse than the file picker in one way, because at least that
// listed what already existed. The dropdown carries the existing names, and
// stays typeable for a new one.
//----------------------------------------------------------------------

namespace {

#define IDC_PROMPT_NAME   1301
#define IDC_PROMPT_OK     1302
#define IDC_PROMPT_CANCEL 1303

class NamePromptDialog : public ModalDialog {
public:
  NamePromptDialog(Engine* pEngine, const wchar_t* title, const wchar_t* prompt,
                   std::wstring* text, size_t maxLen,
                   const std::vector<std::wstring>& choices)
    : ModalDialog(pEngine), m_title(title), m_prompt(prompt),
      m_pText(text), m_maxLen(maxLen), m_choices(choices) {}

protected:
  const wchar_t* GetDialogTitle() const override { return m_title.c_str(); }
  const wchar_t* GetDialogClass() const override { return L"MDropDX12NamePrompt"; }

  void DoBuildControls(int clientW, int clientH) override {
    auto L = GetBaseLayout();
    int x = L.margin, y = L.margin;
    int rw = clientW - L.margin * 2;

    TrackControl(CreateLabel(m_hWnd, m_prompt.c_str(), x, y, rw, L.lineH, m_hFont));
    y += L.lineH + L.gap;

    // CBS_DROPDOWN, not DROPDOWNLIST: the point is to allow a name that is not
    // in the list yet. The height passed is the dropped-down height.
    HWND hNameCombo = CreateCombo(m_hWnd, IDC_PROMPT_NAME, x, y, rw, L.lineH * 9, NULL,
      WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL);
    TrackControl(hNameCombo);
    if (hNameCombo) {
      if (m_hFont) SendMessage(hNameCombo, WM_SETFONT, (WPARAM)m_hFont, TRUE);
      for (const auto& c : m_choices)
        SendMessageW(hNameCombo, CB_ADDSTRING, 0, (LPARAM)c.c_str());
      SetWindowTextW(hNameCombo, m_pText->c_str());
      SendMessage(hNameCombo, CB_LIMITTEXT, (WPARAM)(m_maxLen - 1), 0);
    }
    y += L.lineH + L.gap * 2;

    int btnW = MulDiv(80, L.lineH, 26);
    TrackControl(CreateBtn(m_hWnd, L"OK", IDC_PROMPT_OK,
                           x + rw - btnW * 2 - L.gap, y, btnW, L.lineH, m_hFont));
    TrackControl(CreateBtn(m_hWnd, L"Cancel", IDC_PROMPT_CANCEL,
                           x + rw - btnW, y, btnW, L.lineH, m_hFont));
    y += L.lineH + L.margin;

    FitToContent(clientW, y);
    if (hNameCombo) SetFocus(hNameCombo);
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    if (id == IDC_PROMPT_OK) {
      wchar_t buf[512] = {};
      GetDlgItemTextW(m_hWnd, IDC_PROMPT_NAME, buf, 512);
      // Trim: a name that is only spaces reads as a blank row in the list.
      std::wstring v(buf);
      const size_t a = v.find_first_not_of(L" \t");
      const size_t b = v.find_last_not_of(L" \t");
      v = (a == std::wstring::npos) ? L"" : v.substr(a, b - a + 1);
      if (v.empty()) {
        MessageBoxW(m_hWnd, L"Please enter a name.", m_title.c_str(), MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      *m_pText = v;
      EndDialog(true);
      return 0;
    }
    if (id == IDC_PROMPT_CANCEL) {
      EndDialog(false);
      return 0;
    }
    return -1;
  }

private:
  std::wstring  m_title, m_prompt;
  std::wstring* m_pText;
  size_t        m_maxLen;
  std::vector<std::wstring> m_choices;
};

} // namespace

bool PromptForName(Engine* pEngine, HWND hParent, const wchar_t* title,
                   const wchar_t* prompt, std::wstring& text, size_t maxLen,
                   const std::vector<std::wstring>& choices)
{
  NamePromptDialog dlg(pEngine, title, prompt, &text, maxLen, choices);
  return dlg.Show(hParent, 380, 200);
}

//----------------------------------------------------------------------
// Clipboard
//----------------------------------------------------------------------
// Another window may own the clipboard for a few milliseconds at a time, so a
// single OpenClipboard is not enough -- it fails outright rather than waiting.

bool CopyTextToClipboard(HWND owner, const wchar_t* text) {
  if (!text || !text[0]) return false;
  for (int attempt = 0; attempt < 8; attempt++) {
    if (OpenClipboard(owner)) {
      EmptyClipboard();
      size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
      HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (!hMem) { CloseClipboard(); return false; }
      void* p = GlobalLock(hMem);
      if (!p) { GlobalFree(hMem); CloseClipboard(); return false; }
      memcpy(p, text, bytes);
      GlobalUnlock(hMem);
      if (!SetClipboardData(CF_UNICODETEXT, hMem))
        GlobalFree(hMem);   // ownership only transfers on success
      CloseClipboard();
      return true;
    }
    Sleep(10);
  }
  return false;
}

//----------------------------------------------------------------------
// Shader error viewer
//----------------------------------------------------------------------
// D3DCompile prefixes every diagnostic with the "source name" it was handed,
// which for us is `<some directory>\Shader@0x00000243F354A910` -- a directory
// that has nothing to do with the preset and a pointer that is different every
// run. Pasting that into a bug report or an editor is noise, and it pushes the
// part that matters (line, column, message) off the right of the box. Strip
// the prefix back to the parenthesised position.

std::wstring StripShaderErrorPrefix(const std::wstring& line) {
  const size_t at = line.find(L"Shader@0x");
  if (at == std::wstring::npos) return line;
  const size_t paren = line.find(L'(', at);
  if (paren == std::wstring::npos) return line;
  return line.substr(paren);
}

std::wstring FormatShaderErrorForClipboard(const std::wstring& presetPath,
                                           const std::wstring& errorText,
                                           const std::wstring& capturedAt) {
  std::wstring out = presetPath;
  out += L"\r\n";
  if (!capturedAt.empty()) {
    out += L"captured ";
    out += capturedAt;
    out += L"\r\n";
  }
  size_t pos = 0;
  while (pos <= errorText.size()) {
    size_t nl = errorText.find(L'\n', pos);
    std::wstring line = errorText.substr(pos, nl == std::wstring::npos
                                                ? std::wstring::npos : nl - pos);
    while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' ||
                             line.back() == L'\t'))
      line.pop_back();
    if (!line.empty()) {
      out += StripShaderErrorPrefix(line);
      out += L"\r\n";
    }
    if (nl == std::wstring::npos) break;
    pos = nl + 1;
  }
  return out;
}

namespace {

#define IDC_SHERR_TEXT   1311
#define IDC_SHERR_COPY   1312
#define IDC_SHERR_CLOSE  1313

class ShaderErrorDialog : public ModalDialog {
public:
  ShaderErrorDialog(Engine* pEngine, const std::wstring& presetPath,
                    const std::wstring& errorText, const std::wstring& capturedAt)
    : ModalDialog(pEngine), m_path(presetPath), m_error(errorText),
      m_captured(capturedAt) {}

protected:
  const wchar_t* GetDialogTitle() const override { return L"Shader Error"; }
  const wchar_t* GetDialogClass() const override { return L"MDropDX12ShaderError"; }

  void DoBuildControls(int clientW, int clientH) override {
    auto L = GetBaseLayout();
    int x = L.margin, y = L.margin;
    int rw = clientW - L.margin * 2;

    // The path is what identifies the preset, so it is shown as well as
    // copied -- an error alone does not say which preset produced it.
    HWND hPath = CreateWindowExW(0, L"EDIT", m_path.c_str(),
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
      x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
    if (hPath && m_hFont) SendMessage(hPath, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    TrackControl(hPath);
    y += L.lineH + L.gap;

    // When this was recorded, shown above the text rather than buried in it:
    // a stored error survives the build that produced it, so an undated one
    // reads as a verdict on the running build when it may be nothing of the
    // kind.  Entries written before the date existed say so.
    if (!m_error.empty()) {
      std::wstring when = L"Captured: ";
      when += m_captured.empty() ? std::wstring(L"(before this was recorded)")
                                 : m_captured;
      HWND hWhen = CreateWindowExW(0, L"STATIC", when.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, rw, L.lineH, m_hWnd, NULL, GetModuleHandle(NULL), NULL);
      if (hWhen && m_hFont) SendMessage(hWhen, WM_SETFONT, (WPARAM)m_hFont, TRUE);
      TrackControl(hWhen);
      y += L.lineH + L.gap;
    }

    const std::wstring body = m_error.empty()
      ? std::wstring(L"(no error recorded)")
      : FormatShaderErrorForClipboard(m_path, m_error).substr(m_path.size() + 2);

    int editH = L.lineH * 9;
    HWND hErrorEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", body.c_str(),
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
      ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
      x, y, rw, editH, m_hWnd, (HMENU)(INT_PTR)IDC_SHERR_TEXT,
      GetModuleHandle(NULL), NULL);
    if (hErrorEdit && m_hFont) SendMessage(hErrorEdit, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    TrackControl(hErrorEdit);
    y += editH + L.gap * 2;

    int btnW = MulDiv(80, L.lineH, 26);
    TrackControl(CreateBtn(m_hWnd, L"Copy", IDC_SHERR_COPY,
                           x + rw - btnW * 2 - L.gap, y, btnW, L.lineH, m_hFont));
    TrackControl(CreateBtn(m_hWnd, L"Close", IDC_SHERR_CLOSE,
                           x + rw - btnW, y, btnW, L.lineH, m_hFont));
    y += L.lineH + L.margin;

    FitToContent(clientW, y);
  }

  LRESULT DoCommand(int id, int code, LPARAM lParam) override {
    if (id == IDC_SHERR_COPY) {
      const std::wstring text = FormatShaderErrorForClipboard(m_path, m_error, m_captured);
      if (CopyTextToClipboard(m_hWnd, text.c_str()) && m_pEngine)
        m_pEngine->AddNotification(L"Shader error copied to clipboard");
      return 0;
    }
    if (id == IDC_SHERR_CLOSE || id == IDCANCEL) { EndDialog(false); return 0; }
    return -1;
  }

private:
  std::wstring m_path, m_error, m_captured;
};

} // namespace

void ShowShaderErrorDialog(Engine* pEngine, HWND hParent,
                           const std::wstring& presetPath,
                           const std::wstring& errorText,
                           const std::wstring& capturedAt) {
  ShaderErrorDialog dlg(pEngine, presetPath, errorText, capturedAt);
  dlg.Show(hParent, 620, 320);
}

} // namespace mdrop