#pragma once
#include <string>
#include <vector>
#include <Windows.h>

#include "format_to.h"

// Strip named capture groups (?<name>...) → (...) for std::wregex compatibility.
// MSVC ECMAScript mode doesn't support named groups, but we parse them separately
// via BuildNamedGroupMap() and access by index.
inline std::wstring StripNamedGroups(const std::wstring& pattern) {
  std::wstring result;
  result.reserve(pattern.size());
  for (size_t i = 0; i < pattern.size(); i++) {
    if (pattern[i] == L'\\') {
      result += pattern[i];
      if (i + 1 < pattern.size()) result += pattern[++i];
      continue;
    }
    if (pattern[i] == L'(' && i + 2 < pattern.size() && pattern[i + 1] == L'?') {
      if (pattern[i + 2] == L'<' && i + 3 < pattern.size() && pattern[i + 3] != L'=' && pattern[i + 3] != L'!') {
        // Named group (?<name>...) — emit just '(' and skip past '>'
        result += L'(';
        size_t close = pattern.find(L'>', i + 3);
        if (close != std::wstring::npos) { i = close; continue; }
      } else if (pattern[i + 2] == L'\'' && i + 3 < pattern.size()) {
        // Named group (?'name'...) — emit just '(' and skip past closing quote
        result += L'(';
        size_t close = pattern.find(L'\'', i + 3);
        if (close != std::wstring::npos) { i = close; continue; }
      }
    }
    result += pattern[i];
  }
  return result;
}

// Shared macros — used by shader and texture modules
#define IsAlphabetChar(x) ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z'))
#define IsAlphanumericChar(x) ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9') || x == '.')
#define IsNumericChar(x) (x >= '0' && x <= '9')

// Sprite section helpers — used by engine_sprites.cpp, engine_settings_ui.cpp, engine_sprites_ui.cpp.
inline void FormatSpriteSection(wchar_t* buf, int bufSize, int index) {
  if (index < 100)
    swprintf(buf, bufSize, L"img%02d", index);
  else
    swprintf(buf, bufSize, L"img%d", index);
}

// Template on the destination array so FormatToA can deduce its size; that is
// why the bufSize parameter is gone (issue 147). Both callers already passed a
// real char[64].
template <size_t N>
inline void FormatSpriteSectionA(char (&buf)[N], int index) {
  if (index < 100)
    FormatToA(buf, "img%02d", index);
  else
    FormatToA(buf, "img%d", index);
}

inline std::wstring MakeRelativeSpritePath(const wchar_t* szAbsPath,
                                           const wchar_t* szContentBasePath,
                                           const wchar_t* szMilkdrop2Path) {
  if (!szAbsPath || !szAbsPath[0]) return L"";
  if (szAbsPath[1] != L':') return szAbsPath;
  const wchar_t* bases[] = { szContentBasePath, szMilkdrop2Path };
  for (auto base : bases) {
    if (!base || !base[0]) continue;
    size_t baseLen = wcslen(base);
    if (_wcsnicmp(szAbsPath, base, baseLen) == 0)
      return szAbsPath + baseLen;
  }
  return szAbsPath;
}

// Whether this instance's render window belongs on the taskbar, as one rule in
// one place rather than four literals.
//
// A child instance is a renderer the parent drives over the pipe; it owns no
// interface of its own and is never a window the user switches to. Yet each one
// took a taskbar button AND an Alt+Tab entry, so four displays read as four
// copies of the app -- reported as confusing, and confusing for users too.
//
// Two separate reasons it happened, and both need answering:
//
//   * The window is created unowned, and an unowned top-level window gets a
//     taskbar button by DEFAULT. Being WS_POPUP does not exempt it.
//   * Every borderless/fullscreen transition then assigns WS_EX_APPWINDOW
//     wholesale, an ex-style whose entire purpose is forcing a taskbar button
//     onto a window that would not otherwise have one.
//
// WS_EX_TOOLWINDOW is the one style that answers both: it removes the window
// from the taskbar and from Alt+Tab, and it is what the parent's own tool
// windows already use. It does NOT stop the window being focused, raised,
// minimised or restored, so the parent's RAISE_WINDOW and the benchmark
// harness's minimise-then-restore focus trick both keep working.
//
// Set at creation, not afterwards: Windows decides taskbar presence when the
// window is first shown and does not re-read the style on a later change
// without a hide/show cycle. Assigning the same value again at the transitions
// below is therefore a no-op rather than a toggle, which is the point.
inline LONG_PTR RenderWindowTaskbarExStyle(bool bChildMode) {
  return bChildMode ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW;
}

namespace mdrop {

// Forward declarations for standalone helpers shared across engine_*.cpp modules.
// Definitions live in engine.cpp.
bool ReadFileToString(const wchar_t* szBaseFilename, char* szDestText, int nMaxBytes, bool bConvertToLFCA);
void StripComments(char* p);
void ConvertLLCto1310(char* d, const char* s);
void CancelThread(int max_wait_time_ms);

// Preset directory helpers — defined in engine_presets.cpp.
bool DirHasMilkFilesHelper(const wchar_t* szDir);
bool TryDescendIntoPresetSubdirHelper(wchar_t* szDir);

// Case-insensitive comparison table — defined in engine.cpp.
extern const unsigned char LC2UC[256];

// Case-insensitive wide-string compare — defined in engine_input.cpp.
int mystrcmpiW(const wchar_t* s1, const wchar_t* s2);

// Drag-and-drop preset loading — defined in engine_input.cpp.
void LoadPresetFilesViaDragAndDrop(WPARAM wParam);

// OnUserEdited callbacks — defined in engine.cpp, used as function pointers in menus.
void OnUserEditedPerFrame(LPARAM param1, LPARAM param2);
void OnUserEditedPerPixel(LPARAM param1, LPARAM param2);
void OnUserEditedPresetInit(LPARAM param1, LPARAM param2);
void OnUserEditedWavecode(LPARAM param1, LPARAM param2);
void OnUserEditedWavecodeInit(LPARAM param1, LPARAM param2);
void OnUserEditedShapecode(LPARAM param1, LPARAM param2);
void OnUserEditedShapecodeInit(LPARAM param1, LPARAM param2);
void OnUserEditedWarpShaders(LPARAM param1, LPARAM param2);
void OnUserEditedCompShaders(LPARAM param1, LPARAM param2);

// Texture file extensions — defined in engine_textures.cpp, used by settings UI.
extern std::wstring texture_exts[];
extern const int texture_exts_count;
extern const wchar_t szExtsWithSlashes[];

// Texture helpers — defined in engine_textures.cpp.
bool PickRandomTexture(const wchar_t* prefix, wchar_t* szRetTextureFilename);

// UI factory functions — defined in engine.cpp,
// used by settings window and message dialogs.
HWND CreateLabel(HWND hParent, const wchar_t* text, int x, int y, int w, int h, HFONT hFont, bool visible = true);
HWND CreateEdit(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, DWORD extraStyle = 0, bool visible = true);
// A read-only value shown WITHOUT the sunken border, so it does not read as a
// field you can type in. Still an EDIT, so the text stays selectable.
HWND CreateReadonlyField(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont);
// SetWindowTextW only when the text actually differs -- required for anything a
// timer refreshes, because SetWindowTextW resets the caret to 0 unconditionally.
void SetTextIfChanged(HWND hCtrl, const wchar_t* text);
// The file name within a path; the path itself if it has no separator.
const wchar_t* FileNameOnly(const wchar_t* path);
HWND CreateCheck(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool visible = true);
HWND CreateRadio(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool firstInGroup = false, bool visible = true, int radioGroup = 0);
HWND CreateBtn(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool visible = true);
// Drop-down and plain list box (issue 67). WS_TABSTOP and CBS_HASSTRINGS/
// LBS_NOTIFY are applied unconditionally -- see the definitions in engine.cpp
// for why the tab stop is not a parameter. `dropH` on a combo is the height
// INCLUDING the dropped list.
HWND CreateCombo(HWND hParent, int id, int x, int y, int w, int dropH, HFONT hFont,
                 DWORD style = CBS_DROPDOWNLIST | WS_VSCROLL, bool visible = true,
                 DWORD exStyle = 0);
HWND CreateListBox(HWND hParent, int id, int x, int y, int w, int h, HFONT hFont,
                   DWORD extraStyle = 0, bool visible = true,
                   DWORD exStyle = WS_EX_CLIENTEDGE);
HWND CreateSlider(HWND hParent, int id, int x, int y, int w, int h, int rangeMin, int rangeMax, int pos, bool visible = true);
// Show a value in an editable "mode" combo, selecting the matching list row
// when the value names one. Defined in engine_displays_ui.cpp next to the
// combo it fills; declared here because engine_displays.cpp repopulates that
// combo from UpdateDisplaysTabSelection.
void SetModeComboValue(HWND hCombo, const wchar_t* value);
void DrawOwnerCheckbox(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg, COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText);
void DrawOwnerRadio(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg, COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText);
void draw3DEdge(HDC hdc, const RECT& rc, COLORREF hi, COLORREF shadow, bool raised);
// bAccent draws the button in a warning red. It must be handled in both
// themes: the light path uses DrawFrameControl and ignores the colour
// arguments entirely, so an accent expressed only as a face colour would be
// invisible to anyone not running the dark theme.
void DrawOwnerButton(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBtnFace, COLORREF colBtnHi, COLORREF colBtnShadow, COLORREF colText, bool bAccent = false);

//----------------------------------------------------------------------
// Settings screen types (used by settings UI + input modules)
//----------------------------------------------------------------------
enum SettingType { ST_PATH, ST_BOOL, ST_INT, ST_FLOAT, ST_READONLY };

struct SettingDesc {
  const wchar_t* name;
  SettingType type;
  int id;
  float fMin, fMax, fStep;
  const wchar_t* iniSection;
  const wchar_t* iniKey;
};

enum {
  SET_PRESET_DIR = 0,
  SET_AUDIO_DEVICE,
  SET_AUDIO_SENSITIVITY,
  SET_BLEND_TIME,
  SET_TIME_BETWEEN,
  SET_HARD_CUTS,
  SET_PRESET_LOCK,
  SET_SEQ_ORDER,
  SET_SONG_TITLE_ANIMS,
  SET_CHANGE_WITH_SONG,
  SET_SHOW_FPS,
  SET_SHOW_BATTERY,
  SET_ALWAYS_ON_TOP,
  SET_BORDERLESS,
  SET_SPOUT,
  SET_SPRITES_MESSAGES,
  SET_COUNT
};

// g_settingsDesc[] — defined in engine.cpp, used by settings UI.
extern SettingDesc g_settingsDesc[];

// Settings window class — defined in engine_settings_ui.cpp.
extern const wchar_t* SETTINGS_WND_CLASS;
extern bool g_bSettingsWndClassRegistered;

} // namespace mdrop

//----------------------------------------------------------------------
// Settings window control IDs
//----------------------------------------------------------------------
#define IDC_MW_PRESET_DIR    2001
#define IDC_MW_BROWSE_DIR    2002
#define IDC_MW_AUDIO_DEVICE  2003
#define IDC_MW_AUDIO_SENS    2004
#define IDC_MW_BLEND_TIME    2005
#define IDC_MW_TIME_BETWEEN  2006
#define IDC_MW_HARD_CUTS     2007
#define IDC_MW_PRESET_LOCK   2008
#define IDC_MW_SEQ_ORDER     2009
#define IDC_MW_SONG_TITLE    2010
#define IDC_MW_CHANGE_SONG   2011
#define IDC_MW_SHOW_FPS      2012
#define IDC_MW_SHOW_BATTERY  2017
#define IDC_MW_ALWAYS_TOP    2013
#define IDC_MW_BORDERLESS    2014
#define IDC_MW_SPOUT         2015
#define IDC_MW_CLOSE         2016

// -- Visualization controls --
#define IDC_MW_OPACITY          2020
#define IDC_MW_OPACITY_LABEL    2021
#define IDC_MW_TIME_FACTOR      2022
#define IDC_MW_FRAME_FACTOR     2023
#define IDC_MW_FPS_FACTOR       2024
#define IDC_MW_VIS_INTENSITY    2025
#define IDC_MW_VIS_SHIFT        2026
#define IDC_MW_VIS_VERSION      2027
#define IDC_MW_RENDER_QUALITY   2028
#define IDC_MW_QUALITY_LABEL    2029
#define IDC_MW_QUALITY_AUTO     2030

// -- Color Shift controls --
#define IDC_MW_COL_HUE          2031
#define IDC_MW_COL_HUE_LABEL    2032
#define IDC_MW_COL_SAT          2033
#define IDC_MW_COL_SAT_LABEL    2034
#define IDC_MW_COL_BRIGHT       2035
#define IDC_MW_COL_BRIGHT_LABEL 2036
#define IDC_MW_AUTO_HUE         2037
#define IDC_MW_AUTO_HUE_SEC     2038

// -- Spout Extended controls --
#define IDC_MW_SPOUT_FIXED      2040
#define IDC_MW_SPOUT_WIDTH      2041
#define IDC_MW_SPOUT_HEIGHT     2042
#define IDC_MW_HOTKEY_LIST      2043
#define IDC_MW_HOTKEY_EDIT      2044
#define IDC_MW_HOTKEY_SET       2045
#define IDC_MW_HOTKEY_CLEAR     2046
#define IDC_MW_HOTKEY_ENABLE    2047
#define IDC_MW_TAB              2050
#define IDC_MW_PRESET_LIST      2051
#define IDC_MW_PRESET_PREV      2052
#define IDC_MW_PRESET_NEXT      2053
#define IDC_MW_PRESET_COPY      2054
#define IDC_MW_COL_GAMMA        2055
#define IDC_MW_COL_GAMMA_LABEL  2056
#define IDC_MW_RESOURCES        2057   // "Resources..." button on General tab
#define IDC_MW_RESET_VISUAL     2058   // Reset button on Visual tab
#define IDC_MW_RESET_COLORS     2059   // Reset button on Colors tab
#define IDC_MW_RESET_ALL        2060   // Factory Reset (General tab)
#define IDC_MW_SAVE_DEFAULTS    2061   // Save Safe Defaults (General tab)
#define IDC_MW_USER_RESET       2062   // User Safe Reset (General tab)
#define IDC_MW_FILE_LIST        2063   // ListBox on Files tab
#define IDC_MW_FILE_ADD         2064   // Add button on Files tab
#define IDC_MW_FILE_REMOVE      2065   // Remove button on Files tab
#define IDC_MW_FILE_DESC        2066   // Description label on Files tab
#define IDC_MW_DARK_THEME       2067   // Dark Theme checkbox on General tab
#define IDC_MW_PRESET_UP        2068   // Navigate to parent directory
#define IDC_MW_PRESET_INTO      2069   // Enter selected subdirectory
#define IDC_MW_RANDTEX_LABEL    2070   // Random textures dir label
#define IDC_MW_RANDTEX_EDIT     2071   // Random textures dir edit control
#define IDC_MW_RANDTEX_BROWSE   2072   // Random textures dir Browse button
#define IDC_MW_RANDTEX_CLEAR    2073   // Random textures dir Clear button

// Messages tab (page 5)
#define IDC_MW_MSG_LIST         2080
#define IDC_MW_MSG_PUSH         2081
#define IDC_MW_MSG_UP           2082
#define IDC_MW_MSG_DOWN         2083
#define IDC_MW_MSG_ADD          2084
#define IDC_MW_MSG_EDIT         2085
#define IDC_MW_MSG_DELETE       2086
#define IDC_MW_MSG_RELOAD       2087
#define IDC_MW_MSG_AUTOPLAY     2088
#define IDC_MW_MSG_SEQUENTIAL   2089
#define IDC_MW_MSG_INTERVAL     2090
#define IDC_MW_MSG_JITTER       2092
#define IDC_MW_MSG_PREVIEW      2094
#define IDC_MW_MSG_PASTE        2095
#define IDC_MW_MSG_INTERVAL_LBL 2096
#define IDC_MW_MSG_JITTER_LBL   2097
#define IDC_MW_MSG_OPENINI      2098
#define IDC_MW_MSG_AUTOSIZE     2099

// -- Settings window controls --
#define IDC_MW_RESET_WINDOW         2110   // Reset Window Size button (General tab)
#define IDC_MW_FONT_PLUS            2111   // Font size + button (General tab)
#define IDC_MW_FONT_MINUS           2112   // Font size - button (General tab)
#define IDC_MW_MESH_SIZE            2113   // Mesh size slider (Visual tab)
#define IDC_MW_MESH_SIZE_LABEL      2114   // Mesh size value label
#define IDC_MW_TEX_BITS             2115   // Texture bits combo (Visual tab)

// -- GPU Protection controls (Visual tab) --
#define IDC_MW_GPU_MAX_INST         2100
#define IDC_MW_GPU_SCALE_BY_RES     2101
#define IDC_MW_GPU_SCALE_BASE       2102
#define IDC_MW_GPU_SKIP_HEAVY       2103
#define IDC_MW_GPU_HEAVY_THRESHOLD  2104
#define IDC_MW_GPU_RELOAD_PRESET    2105
#define IDC_MW_RESTART_RENDER       2106
#define IDC_MW_VSYNC_ENABLED        2107
#define IDC_MW_FPS_CAP              2108
#define IDC_MW_GPU_MIN_VRAM_PCT     2109  // Exit if available VRAM budget % below this (0=off)

// Remote tab control IDs
#define IDC_MW_IPC_TITLE            2120  // Edit: Window Title
#define IDC_MW_IPC_REMOTE_TITLE     2121  // Edit: Remote Window Title
#define IDC_MW_IPC_APPLY            2122  // Button: Apply & Restart IPC
#define IDC_MW_IPC_LIST             2123  // ListBox: active IPC windows
#define IDC_MW_IPC_TITLE_HINT       2125  // Label: hint for window title
#define IDC_MW_IPC_REMOTE_HINT      2126  // Label: hint for remote title
#define IDC_MW_IPC_MSG_GROUP        2127  // Label: "Last message: HH:MM:SS"
#define IDC_MW_IPC_MSG_TEXT         2128  // Read-only Edit: message content
#define IDC_MW_IPC_CAPTURE          2129  // Button: Save Screenshot
#define IDC_MW_SPRITES_MESSAGES     2130  // ComboBox: Messages/Sprites mode (General tab)
#define IDC_MW_LOGLEVEL_OFF         2131  // Radio: Log Level Off
#define IDC_MW_LOGLEVEL_ERROR       2132  // Radio: Log Level Error
#define IDC_MW_LOGLEVEL_WARN        2144  // Radio: Log Level Warn
#define IDC_MW_LOGLEVEL_INFO        2145  // Radio: Log Level Info
#define IDC_MW_LOGLEVEL_VERBOSE     2146  // Radio: Log Level Verbose
#define IDC_MW_MSG_SHOW_MESSAGES   2133  // Checkbox: Enable Messages (Messages tab)
#define IDC_MW_MSG_SHOW_SPRITES    2134  // Checkbox: Enable Sprites (Messages tab)
#define IDC_MW_CONTENT_BASE_LABEL  2135  // Label: Content Base Path (Files tab)
#define IDC_MW_CONTENT_BASE_EDIT   2136  // Edit: Content Base Path display (Files tab)
#define IDC_MW_CONTENT_BASE_BROWSE 2137  // Button: Browse for Content Base Path
#define IDC_MW_CONTENT_BASE_CLEAR  2138  // Button: Clear Content Base Path
#define IDC_MW_FALLBACK_TEX_LABEL  2139  // Label: Fallback Texture Style (Files tab)
#define IDC_MW_FALLBACK_TEX        2140  // ComboBox: Fallback Texture Style (Files tab)
#define IDC_MW_FALLBACK_FILE_EDIT  2141  // Edit: Custom fallback texture file path (Files tab)
#define IDC_MW_FALLBACK_FILE_BROWSE 2142 // Button: Browse for custom fallback texture
#define IDC_MW_FALLBACK_FILE_CLEAR 2143  // Button: Clear custom fallback texture

// Idle Timer controls (System tab)
#define IDC_MW_IDLE_ENABLE         2150  // Checkbox: enable idle timer
#define IDC_MW_IDLE_TIMEOUT        2151  // Edit: timeout in minutes
#define IDC_MW_IDLE_TIMEOUT_SPIN   2152  // Up-down spinner buddy
#define IDC_MW_IDLE_ACTION         2153  // ComboBox: Fullscreen / Stretch-Mirror / Mirror all / Load display profile
#define IDC_MW_IDLE_AUTO_RESTORE   2154  // Checkbox: auto-restore on input
#define IDC_MW_IDLE_PROFILE        2155  // ComboBox: display profile the Load-profile action loads
#define IDC_MW_IDLE_FULL_OPACITY   2156  // Checkbox: take every display to 100% opacity while idle

// Song Info controls (General tab)
#define IDC_MW_SONG_SOURCE         2160  // ComboBox: SMTC / IPC / Window Title
#define IDC_MW_SONG_WINDOW_TITLE   2161  // Edit: window title to scrape
#define IDC_MW_SONG_OVERLAY        2162  // Checkbox: overlay notifications
#define IDC_MW_SONG_COVER          2163  // Checkbox: show cover art sprite
#define IDC_MW_SONG_COVER_NOW      2174  // Button: show cover art now
#define IDC_MW_SONG_CORNER         2164  // ComboBox: display corner
#define IDC_MW_SONG_DISPLAY_SEC    2165  // Edit: display seconds
#define IDC_MW_CURRENT_PRESET      2166  // Edit: current/startup preset
#define IDC_MW_BROWSE_PRESET       2167  // Button: browse for preset file
#define IDC_MW_SONG_SHOW_NOW       2168  // Button: force display current track info
#define IDC_MW_SONG_ALWAYS_SHOW    2169  // Checkbox: always show track info
#define IDC_MW_SONG_WT_PREVIEW     2170  // Static label: window title parse preview
#define IDC_MW_SONG_WT_LABEL       2171  // Static label: "Window Title:" (for show/hide)
#define IDC_MW_WT_PROFILE          2172  // ComboBox: profile selector (General tab)
#define IDC_MW_WT_EDIT_PARSER      2173  // Button: "Edit Parser..." (General tab)
#define IDC_MW_SONG_ANIM_COMBO    2175  // ComboBox: song title animation profile
#define IDC_MW_SONG_PUSH_TITLE    2176  // Button: "Push Title"
#define IDC_MW_SONG_EDIT_PROFILES 2177  // Button: "Edit Profiles..."

// Window Title Parser popup controls
#define IDC_MW_WTP_PROFILE         2180  // ComboBox: profile selector
#define IDC_MW_WTP_NEW             2181  // Button: New profile
#define IDC_MW_WTP_DELETE          2182  // Button: Delete profile
#define IDC_MW_WTP_NAME            2183  // Edit: profile name
#define IDC_MW_WTP_WINDOW_REGEX    2184  // Edit: window match regex
#define IDC_MW_WTP_MATCHED         2185  // Static: matched window display
#define IDC_MW_WTP_PARSE_REGEX     2186  // Edit: parse regex
#define IDC_MW_WTP_PARSED          2187  // Static: parsed result display
#define IDC_MW_WTP_INTERVAL        2188  // Edit: poll interval
#define IDC_MW_WTP_HELP            2189  // Button: regex help link
#define IDC_MW_WTP_OK              2190  // Button: OK
#define IDC_MW_WTP_CANCEL          2191  // Button: Cancel
#define IDC_MW_WTP_WINDOWS         2192  // ComboBox: enumerated windows dropdown
#define IDC_MW_ANISO               2193  // Checkbox: Anisotropic Filtering (Visual tab)
#define IDC_MW_HUD_FONT_SCALE       2194  // Slider: HUD font user scale (System tab)
#define IDC_MW_HUD_FONT_SCALE_LABEL 2195  // Static: "100%"
#define IDC_MW_MIRROR_HUD_DISABLE   2196  // Checkbox: Disable HUD on mirrors

// 2197-2203 were the render tunable sliders (MD31 stand-ins); free to reuse.

// Video Input source selector (Displays tab)
#define IDC_MW_VIDINPUT_SOURCE      7050
// Webcam controls
#define IDC_MW_VIDINPUT_WEBCAM      7051
#define IDC_MW_VIDINPUT_WEBCAM_REF  7052
// Video File controls
#define IDC_MW_VIDINPUT_FILE_EDIT   7053
#define IDC_MW_VIDINPUT_FILE_BROWSE 7054
#define IDC_MW_VIDINPUT_FILE_LOOP   7055

// Spout Video Input controls (Displays tab)
#define IDC_MW_SPINPUT_ENABLE       7020
#define IDC_MW_SPINPUT_SENDER       7021
#define IDC_MW_SPINPUT_REFRESH      7022
#define IDC_MW_SPINPUT_LAYER_BG     7023
#define IDC_MW_SPINPUT_LAYER_OV     7024
#define IDC_MW_SPINPUT_OPACITY      7025
#define IDC_MW_SPINPUT_OPACITY_LBL  7026
#define IDC_MW_SPINPUT_LUMAKEY      7027
#define IDC_MW_SPINPUT_LUMA_THR     7028
#define IDC_MW_SPINPUT_LUMA_THR_LBL 7029
#define IDC_MW_SPINPUT_LUMA_SOFT    7030
#define IDC_MW_SPINPUT_LUMA_SOFT_LBL 7031

// Game Controller control IDs
#define IDC_MW_CTRL_ENABLE      7040
#define IDC_MW_CTRL_DEVICE      7041
#define IDC_MW_CTRL_SCAN        7042
#define IDC_MW_CTRL_JSON_EDIT   7043
#define IDC_MW_CTRL_DEFAULTS    7044
#define IDC_MW_CTRL_SAVE        7045
#define IDC_MW_CTRL_LOAD        7046
#define IDC_MW_CTRL_HELP        7047

// Settings window pin (always-on-top toggle)
#define IDC_MW_SETTINGS_PIN     7060

// Spout / Displays window controls
#define IDC_MW_DISPLAYS_PIN     7061
#define IDC_MW_DISP_FONT_PLUS   7062
#define IDC_MW_DISP_FONT_MINUS  7063
#define IDC_MW_OPEN_DISPLAYS    7064  // Button on General tab to open Displays window
#define IDC_MW_DISP_TAB         7065  // Tab control in Displays window

// Song Info window controls
#define IDC_MW_SONGINFO_PIN         7070
#define IDC_MW_SONGINFO_FONT_PLUS   7071
#define IDC_MW_SONGINFO_FONT_MINUS  7072
#define IDC_MW_OPEN_SONGINFO        7073  // Button on General tab to open Song Info window

// Hotkeys window controls
#define IDC_MW_HOTKEYS_PIN        7080
#define IDC_MW_HOTKEYS_FONT_PLUS  7081
#define IDC_MW_HOTKEYS_FONT_MINUS 7082
#define IDC_MW_OPEN_HOTKEYS       7083  // Button on Settings System tab
#define IDC_MW_HOTKEYS_LIST       7084  // ListView (report mode)
#define IDC_MW_HOTKEYS_ADD        7085  // "+" button (add user hotkey)
#define IDC_MW_HOTKEYS_EDITBTN    7086  // Edit button (open modal)
#define IDC_MW_HOTKEYS_DELETE     7087  // Delete button (user entries only)
#define IDC_MW_HOTKEYS_CLEARKEY   7088  // Clear Key button (unbind without deleting)
#define IDC_MW_HOTKEYS_RESET      7089  // Reset to Defaults button
#define IDC_MW_HOTKEYS_TAB        7095  // Tab control
#define IDC_MW_HOTKEYS_CATLIST    7096  // Help Display: category order listbox
#define IDC_MW_HOTKEYS_CAT_UP     7097  // Help Display: move up button
#define IDC_MW_HOTKEYS_CAT_DOWN   7098  // Help Display: move down button
#define IDC_MW_HOTKEYS_CAT_RESET  7099  // Help Display: reset order button
// Not in the 708x block with the other Hotkeys buttons: 7087 is Delete, and
// control ids only have to be unique per WINDOW, so this sits with the child
// dialog's own ids where there is room.
#define IDC_MW_HOTKEYS_AVAIL      8035  // "Available keys..." button (opens the child)
static_assert(IDC_MW_HOTKEYS_AVAIL != IDC_MW_HOTKEYS_DELETE &&
              IDC_MW_HOTKEYS_AVAIL != IDC_MW_HOTKEYS_EDITBTN &&
              IDC_MW_HOTKEYS_AVAIL != IDC_MW_HOTKEYS_CLEARKEY &&
              IDC_MW_HOTKEYS_AVAIL != IDC_MW_HOTKEYS_RESET,
              "Available keys button collides with another Hotkeys control");

// Available-keys child dialog (8030-8036)
#define IDC_AK_LIST           8030  // ListView: combination / status / action
#define IDC_AK_FREEONLY       8031  // Checkbox: hide combinations already taken
#define IDC_AK_RESCAN         8032  // Button: probe again
#define IDC_AK_USE            8033  // Button: bind the selection to the action
#define IDC_AK_NOTE           8034  // Static: the Win-key caveat
#define IDC_AK_CLEAR          8036  // Button: unbind the "yours" row's action
#define HOTKEYS_NUM_PAGES         2
#define HOTKEYS_PAGE_BINDINGS     0
#define HOTKEYS_PAGE_HELPORDER    1

// Edit Hotkey modal dialog controls
#define IDC_HK_EDIT_ACTION        8001  // Static: action name (read-only)
#define IDC_HK_EDIT_LABEL         8002  // Edit: user label
#define IDC_HK_EDIT_HOTKEY        8003  // HOTKEY_CLASS: key capture
#define IDC_HK_EDIT_CLEAR         8004  // Button: clear key
#define IDC_HK_EDIT_SCOPE         8005  // Checkbox: global
#define IDC_HK_EDIT_COMMAND       8006  // Edit: IPC command (Script)
#define IDC_HK_EDIT_PATH          8007  // Edit: app path (Launch)
#define IDC_HK_EDIT_BROWSE        8008  // Button: browse for exe (Launch)

// Shared action edit dialog (8010-8024)
#define IDC_AE_ACTION_TYPE    8010  // Combobox: action type dropdown
#define IDC_AE_LABEL          8011  // Edit: label text
#define IDC_AE_PAYLOAD        8012  // Edit: payload/command (multiline)
#define IDC_AE_BROWSE         8013  // Button: browse for file
#define IDC_AE_HOTKEY         8014  // HOTKEY_CLASS: local key capture
#define IDC_AE_CLEAR_KEY      8015  // Button: clear local key binding
#define IDC_AE_SCOPE          8016  // Checkbox: global scope (user hotkeys only)
#define IDC_AE_ACTION_LABEL   8017  // Edit (read-only): built-in action name
#define IDC_AE_MOUSE          8018  // Combobox: local mouse button selector
#define IDC_AE_GLOBAL_HOTKEY  8019  // HOTKEY_CLASS: global key capture
#define IDC_AE_GLOBAL_CLEAR   8020  // Button: clear global key binding
#define IDC_AE_GLOBAL_MOUSE   8021  // Combobox: global mouse button selector
#define IDC_AE_CONFLICT       8022  // Static: what this binding would unbind

// MIDI window controls (7090-7109)
#define IDC_MW_MIDI_PIN         7090
#define IDC_MW_MIDI_FONT_PLUS   7091
#define IDC_MW_MIDI_FONT_MINUS  7092
#define IDC_MW_MIDI_LIST        7093   // ListView (report mode)
#define IDC_MW_MIDI_DEVICE      7094   // ComboBox: MIDI device selector
#define IDC_MW_MIDI_SCAN        7095   // Button: Scan devices
#define IDC_MW_MIDI_ENABLE      7096   // Checkbox: Enable MIDI
#define IDC_MW_MIDI_LEARN       7097   // Button: Learn
#define IDC_MW_MIDI_CLEAR       7098   // Button: Clear selected row
#define IDC_MW_MIDI_DELETE      7099   // Button: Delete selected row
#define IDC_MW_MIDI_SAVE        7100   // Button: Save
#define IDC_MW_MIDI_LOAD        7101   // Button: Load
#define IDC_MW_MIDI_DEFAULTS    7102   // Button: Defaults
#define IDC_MW_MIDI_LABEL       7103   // Edit: row label
#define IDC_MW_MIDI_TYPE        7104   // ComboBox: Button/Knob
#define IDC_MW_MIDI_ACTION      7105   // ComboBox: action (dropdown for buttons, list for knobs)
#define IDC_MW_MIDI_INCREMENT   7106   // Edit: increment (knobs only)
#define IDC_MW_MIDI_BUFFER      7107   // Edit: buffer delay ms
#define IDC_MW_MIDI_BUFFER_SPIN 7108   // Spin for buffer
#define IDC_MW_OPEN_MIDI        7109   // Button on General tab

#define DISPLAYS_NUM_PAGES      2
#define DISPLAYS_PAGE_OUTPUTS   0
#define DISPLAYS_PAGE_VIDINPUT  1

#define IDT_IPC_MONITOR             10001 // Timer ID for IPC message polling
#define IDT_IDLE_CHECK              10002 // Timer ID for idle detection (1-second interval)
#define IDT_CONTROLLER_POLL         10003 // Timer ID for game controller polling (50ms)
#define IDT_SAVE_SETTINGS_DEBOUNCE  10004 // Timer ID for debounced settings save (2s after last change)
#define IDT_TCP_POLL                10005 // Timer ID for TCP server polling (50ms)
#define IDT_CHILD_FADE_IN           10006 // Timer ID for a child fading up over its mirror
#define IDT_DISPLAYS_CHILD_POLL     10007 // Timer ID: refresh the Displays list as children start

#define IDC_MW_MSG_PLAY             4010   // "Play/Stop" toggle button on Messages tab

// Resource viewer control IDs
#define IDC_RV_LISTVIEW         3001   // ListView in resource viewer
#define IDC_RV_COPY_PATH        3002   // "Copy Path" button
#define IDC_RV_REFRESH          3003   // "Refresh" button

// Message Edit Dialog control IDs (shared between settings UI and message modules)
#define IDC_MSGEDIT_TEXT         2100
#define IDC_MSGEDIT_FONT_COMBO  2101
#define IDC_MSGEDIT_CHOOSE_FONT 2102
#define IDC_MSGEDIT_CHOOSE_COLOR 2103
#define IDC_MSGEDIT_FONT_PREVIEW 2104
#define IDC_MSGEDIT_SIZE        2105
#define IDC_MSGEDIT_XPOS        2106
#define IDC_MSGEDIT_YPOS        2107
#define IDC_MSGEDIT_GROWTH      2108
#define IDC_MSGEDIT_TIME        2109
#define IDC_MSGEDIT_FADEIN      2110
#define IDC_MSGEDIT_FADEOUT     2111
#define IDC_MSGEDIT_OK          2112
#define IDC_MSGEDIT_CANCEL      2113
#define IDC_MSGEDIT_COLOR_SWATCH 2114
#define IDC_MSGEDIT_SEND_NOW      2115
#define IDC_MSGEDIT_RAND_ALL      2116
#define IDC_MSGEDIT_RAND_POS      2117
#define IDC_MSGEDIT_RAND_SIZE     2118
#define IDC_MSGEDIT_RAND_FONT     2119
#define IDC_MSGEDIT_RAND_COLOR    2120
#define IDC_MSGEDIT_RAND_EFFECTS  2121
#define IDC_MSGEDIT_RAND_GROWTH   2122
#define IDC_MSGEDIT_RAND_DURATION 2123
#define IDC_MSGEDIT_ANIM_PROFILE  2124  // Animation Profile combo in message edit dialog

// Message Overrides Dialog control IDs
#define IDC_MSGOVERRIDE_RAND_FONT     4000
#define IDC_MSGOVERRIDE_RAND_COLOR    4001
#define IDC_MSGOVERRIDE_RAND_SIZE     4002
#define IDC_MSGOVERRIDE_RAND_EFFECTS  4003
#define IDC_MSGOVERRIDE_SIZE_MIN      4004
#define IDC_MSGOVERRIDE_SIZE_MAX      4005
#define IDC_MSGOVERRIDE_MAX_ONSCREEN  4006
#define IDC_MSGOVERRIDE_OK            4007
#define IDC_MSGOVERRIDE_CANCEL        4008
#define IDC_MW_MSG_OVERRIDES          4009
#define IDC_MSGOVERRIDE_RAND_POS      4010
#define IDC_MSGOVERRIDE_RAND_GROWTH   4011
#define IDC_MSGOVERRIDE_SLIDE_IN      4012
#define IDC_MSGOVERRIDE_RAND_DURATION 4013
#define IDC_MSGOVERRIDE_SHADOW        4014
#define IDC_MSGOVERRIDE_BOX           4015
#define IDC_MSGOVERRIDE_APPLY_HUE     4016
#define IDC_MSGOVERRIDE_RAND_HUE      4017
#define IDC_MSGOVERRIDE_IGNORE_PERMSG 4018
#define IDC_MSGOVERRIDE_TAB          4019
#define IDC_MSGOVERRIDE_ANIM_LIST    4020
#define IDC_MSGOVERRIDE_ANIM_ALL     4021
#define IDC_MSGOVERRIDE_ANIM_NONE    4022

// Sprites tab control IDs (page 6)
#define IDC_MW_SPR_LIST         5000
#define IDC_MW_SPR_ADD          5001
#define IDC_MW_SPR_DELETE       5002
#define IDC_MW_SPR_IMPORT       5003
#define IDC_MW_SPR_PUSH         5004
#define IDC_MW_SPR_KILL         5005
#define IDC_MW_SPR_SAVE         5006
#define IDC_MW_SPR_OPENINI      5007
#define IDC_MW_SPR_RELOAD       5008
#define IDC_MW_SPR_IMG_PATH     5010
#define IDC_MW_SPR_IMG_BROWSE   5011
#define IDC_MW_SPR_BLENDMODE    5012
#define IDC_MW_SPR_X            5013
#define IDC_MW_SPR_Y            5014
#define IDC_MW_SPR_SX           5015
#define IDC_MW_SPR_SY           5016
#define IDC_MW_SPR_ROT          5017
#define IDC_MW_SPR_R            5018
#define IDC_MW_SPR_G            5019
#define IDC_MW_SPR_B            5020
#define IDC_MW_SPR_A            5021
#define IDC_MW_SPR_FLIPX        5022
#define IDC_MW_SPR_FLIPY        5023
#define IDC_MW_SPR_REPEATX      5024
#define IDC_MW_SPR_REPEATY      5025
#define IDC_MW_SPR_BURN         5026
#define IDC_MW_SPR_COLORKEY     5027
#define IDC_MW_SPR_LAYER        5028
#define IDC_MW_SPR_INIT_CODE    5030
#define IDC_MW_SPR_FRAME_CODE   5031
#define IDC_MW_SPR_KILLALL      5036
#define IDC_MW_SPR_DEFAULTS     5037

// Sprite Import dialog control IDs
#define IDC_SPRIMP_MODE_ADD       5100
#define IDC_SPRIMP_MODE_REPLACE   5101
#define IDC_SPRIMP_MAX_EDIT       5102
#define IDC_SPRIMP_BLEND          5103
#define IDC_SPRIMP_X              5104
#define IDC_SPRIMP_Y              5105
#define IDC_SPRIMP_SX             5106
#define IDC_SPRIMP_SY             5107
#define IDC_SPRIMP_ROT            5108
#define IDC_SPRIMP_R              5109
#define IDC_SPRIMP_G              5110
#define IDC_SPRIMP_B              5111
#define IDC_SPRIMP_A              5112
#define IDC_SPRIMP_LAYER          5113
#define IDC_SPRIMP_OK             5120
#define IDC_SPRIMP_CANCEL         5121

// Script tab control IDs (page 8 — before About)
#define IDC_MW_SCRIPT_FILE      6001
#define IDC_MW_SCRIPT_BROWSE    6002
#define IDC_MW_SCRIPT_PLAY      6003
#define IDC_MW_SCRIPT_STOP      6004
#define IDC_MW_SCRIPT_BPM       6005
#define IDC_MW_SCRIPT_BEATS     6006
#define IDC_MW_SCRIPT_LIST      6007
#define IDC_MW_SCRIPT_LOOP      6008
#define IDC_MW_SCRIPT_LINE      6009

// Displays tab control IDs (page 9)
#define IDC_MW_DISP_LIST        7000   // ListBox: display outputs
#define IDC_MW_DISP_ENABLE      7001   // Checkbox: enable selected output
#define IDC_MW_DISP_FULLSCREEN  7002   // Checkbox: fullscreen (monitors only)
#define IDC_MW_DISP_REFRESH     7003   // Button: refresh monitor list
#define IDC_MW_DISP_ADD_SPOUT   7004   // Button: add Spout output
#define IDC_MW_DISP_REMOVE      7005   // Button: remove selected Spout output
#define IDC_MW_DISP_SPOUT_NAME  7006   // Edit: Spout sender name
#define IDC_MW_DISP_SPOUT_W     7007   // Edit: Spout width
#define IDC_MW_DISP_SPOUT_H     7008   // Edit: Spout height
#define IDC_MW_DISP_SPOUT_FIXED 7009   // Checkbox: Spout fixed size
#define IDC_MW_DISP_ACTIVATE    7010   // Button: activate/deactivate mirrors
#define IDC_MW_DISP_CLICKTHRU   7011   // Checkbox: mirror click-through
#define IDC_MW_DISP_ONTOP       7135   // Checkbox: keep THIS display on top
#define IDC_MW_DISP_OPACITY     7012   // Edit: mirror opacity (1-100)
#define IDC_MW_DISP_OPACITY_SPIN 7013  // Up-down (spin) buddy for opacity
#define IDC_MW_DISP_MIRROR_ALTS 7014   // Combo: what ALT-S does (stretch/mirror/profile)
#define IDC_MW_DISP_PROFILE_DEFAULT 7138 // Button: make the shown profile the default
#define IDC_MW_DISP_PROFILE_DELETE 7140 // Button: delete a saved profile (to the bin)
#define IDC_MW_DISP_MIRROR_NOPROMPT 7015 // Checkbox: don't ask when no mirrors enabled
#define IDC_MW_DISP_SAVE_PROFILE   7016 // Button: save display profile
#define IDC_MW_DISP_LOAD_PROFILE   7017 // Button: load display profile
#define IDC_MW_DISP_MIRROR_WM     7018 // Button: toggle mirror watermark mode
#define IDC_MW_DISP_INDEPENDENT   7019 // Checkbox: independent render (correct aspect)
// 7125/7126, not 7020/7021: those are IDC_MW_SPINPUT_ENABLE and
// IDC_MW_SPINPUT_SENDER on the Video Input page of THIS SAME window, so the
// FPS combo and the Spout-input enable checkbox shared an id.
#define IDC_MW_DISP_MAXFPS        7125 // Combo: independent mirror max FPS (parity/120/60/30)
#define IDC_MW_DISP_HUD_DISABLE   7126 // Checkbox: hide HUD on mirrors (linked to System tab)
// Checkbox: watermark THIS display. The "Mirror Watermark" button beside it is
// the all-at-once gesture; this is the per-display state it sets.
#define IDC_MW_DISP_WATERMARK     7127
// Per-display child instance controls (forgejo#22).
// 7110+ because 7022-7031 are the IDC_MW_SPINPUT_* block -- the Video Input
// page of THIS SAME window, so a duplicate id makes GetDlgItem return the
// wrong control and command dispatch ambiguous. 7109 was the highest in use.
#define IDC_MW_DISP_MODE_MIRROR   7110 // Radio: this display mirrors the primary
#define IDC_MW_DISP_MODE_CHILD    7111 // Radio: its own preset, in its own process
#define IDC_MW_DISP_MODE_PRESET   7128 // Radio: its own preset, in THIS process
#define IDC_MW_DISP_PRESETDIR     7112 // Edit: this display's preset directory
#define IDC_MW_DISP_PRESETDIR_BR  7113 // Button: browse for the preset directory
#define IDC_MW_DISP_STARTUP       7114 // Edit: this display's startup preset
#define IDC_MW_DISP_STARTUP_BR    7115 // Button: browse for the startup preset
#define IDC_MW_DISP_CYCLE_ON      7116 // Checkbox: cycle presets on this display
#define IDC_MW_DISP_CYCLE_SECS    7117 // Edit: seconds between changes
#define IDC_MW_DISP_ORDER_RANDOM  7118 // Radio: random order
#define IDC_MW_DISP_ORDER_SEQ     7119 // Radio: sequential order
// Displays-window controls added later, at 7130+ because 7120-7128 were all
// TAKEN -- by IDC_MW_DISP_PROFILE_*, the three IDC_MW_DISP_GRP_*, MAXFPS,
// HUD_DISABLE and RESET, which are declared further up this file and are easy
// to miss. Every one of these nine collided with one of those, and because
// they all live in the SAME window that is fatal rather than untidy: clicking
// the Details radio raised the "Reset Displays to Defaults" prompt, because
// both were 7127 and the reset branch matched first.
//
// The other duplicate ids in this file (7095-7099, 9810/9815/9816) are between
// DIFFERENT windows -- Hotkeys vs MIDI, Annotations vs the Annotations tool
// window -- and control ids only need to be unique within one window, so those
// are fine. Take the next Displays id from 7139, and grep the NUMBER before
// picking it, not just the name.
#define IDC_MW_DISP_LOCK_INHERIT  7130 // Radio: follow the main window's lock
#define IDC_MW_DISP_LOCK_OFF      7131 // Radio: this display is never locked
#define IDC_MW_DISP_LOCK_ON       7132 // Radio: this display never advances
#define IDC_MW_DISP_PRESET_PREV   7133 // Button: previous preset, selected display only
#define IDC_MW_DISP_PRESET_NEXT   7134 // Button: next preset, selected display only
#define IDC_MW_DISP_VIEW_LAYOUT   7135 // Radio: show displays as an arrangement
#define IDC_MW_DISP_VIEW_DETAILS  7136 // Radio: show displays as a detail list
#define IDC_MW_DISP_PANEL         7137 // The arrangement panel itself
#define IDC_MW_DISP_BRING_FRONT   7139 // Button: raise every display's window
// Profile controls
#define IDC_MW_DISP_PROFILE_AUTOSTART 7129 // Checkbox: a profile starts every
                                          // display it names
#define IDC_MW_DISP_PROFILE_ONSTART 7120 // Checkbox: load a profile at startup
// Replaces the checkbox above with the same shape the PRESET startup uses --
// a mode combo plus a Save that writes through (#278). The checkbox could only
// say on/off, so "use whatever the displays are set to right now" had no way
// to be expressed and the stored name was the only possible answer.
#define IDC_MW_DISP_PROFILE_MODE    7136 // Combo: None / Current / <file>
#define IDC_MW_DISP_PROFILE_MODESAVE 7137 // Button: write the choice through
#define IDC_MW_DISP_PROFILE_PATH    7121 // Edit (read-only): the startup profile
// Group boxes — no messages, but they need tracking for the dark theme
#define IDC_MW_DISP_GRP_SELECTED  7122
#define IDC_MW_DISP_GRP_MIRRORS   7123
#define IDC_MW_DISP_GRP_PROFILES  7124
#define IDC_MW_DISP_RESET         7127 // Button: reset all display settings to defaults

// About tab
#define IDC_MW_FILE_ASSOC       2200   // Button: Register File Association (About tab)
#define IDC_MW_PRESET_FILTER    2201   // Button: Preset extension filter (General tab)
#define IDC_MW_LOGOUTPUT_FILE   2202   // Checkbox: Log Output to File (About tab)
#define IDC_MW_LOGOUTPUT_ODS    2203   // Checkbox: Log Output to Debug Messages (About tab)

// Button Board window controls (9000-9019)
#define IDC_MW_BOARD_PIN          9000
#define IDC_MW_BOARD_FONT_PLUS    9001
#define IDC_MW_BOARD_FONT_MINUS   9002
#define IDC_MW_BOARD_PANEL        9003
#define IDC_MW_BOARD_BANK_PREV    9004
#define IDC_MW_BOARD_BANK_NEXT    9005
#define IDC_MW_BOARD_BANK_LABEL   9006
#define IDC_MW_BOARD_CONFIG       9007

// Presets window controls (9020-9039)
#define IDC_MW_PRESETS_PIN          9020
#define IDC_MW_PRESETS_FONT_PLUS    9021
#define IDC_MW_PRESETS_FONT_MINUS   9022
#define IDC_MW_PRESETS_STARTUP      9023  // Combo: startup mode (Random/Current/Last Used/Specific)
// 9091/9092, not 9024/9025: those are IDC_MW_PRESETS_SUBDIR and
// IDC_MW_PRESETS_TAG_FILTER in this same window. 9090 was the highest in use.
#define IDC_MW_PRESETS_STARTUP_PATH   9091  // Edit (read-only): which preset that actually is
#define IDC_MW_PRESETS_STARTUP_BROWSE 9092  // Button: browse for a specific startup preset
#define IDC_MW_PRESETS_STARTUP_SAVE   9093  // Button: write the startup setting
                                          // out now, and say that it did
#define IDC_MW_PRESETS_SUBDIR       9024  // Button: Subdirectory mode (Never/Ask/Always)
#define IDC_MW_PRESETS_TAG_FILTER   9025  // Combo: Tag filter dropdown
#define IDC_MW_PRESETS_IMPORT_TAGS  9026  // Button: Import MWR tags
#define IDC_MW_PRESETS_LIST_COMBO  9027  // Combo: Preset list selector
#define IDC_MW_PRESETS_LIST_SAVE   9028  // Button: Save current list
#define IDC_MW_PRESETS_LIST_CLEAR  9029  // Button: Clear active list (back to directory scan)
// Checkbox: the LIVE preset lock (m_bPresetLockedByUser), as against
// IDC_MW_PRESET_LOCK which is the startup preference. Until this existed the
// live lock could only be toggled by hotkey and could not be observed at all,
// so the only way to learn its state was to press the key and watch whether
// presets started advancing.
#define IDC_MW_PRESETS_LOCK_NOW    9094

// Sprites window controls (9040-9059)
#define IDC_MW_SPRITES_WIN_PIN          9040
#define IDC_MW_SPRITES_WIN_FONT_PLUS    9041
#define IDC_MW_SPRITES_WIN_FONT_MINUS   9042

// Messages window controls (9060-9079)
#define IDC_MW_MESSAGES_WIN_PIN         9060
#define IDC_MW_MESSAGES_WIN_FONT_PLUS   9061
#define IDC_MW_MESSAGES_WIN_FONT_MINUS  9062

// Shader Import control panel (9100-9119)
#define IDC_MW_SHIMPORT_PIN          9100
#define IDC_MW_SHIMPORT_FONT_PLUS    9101
#define IDC_MW_SHIMPORT_FONT_MINUS   9102
#define IDC_MW_SHIMPORT_ERROR_EDIT   9105  // Multiline EDIT: errors (read-only)
#define IDC_MW_SHIMPORT_CONVERT      9106  // Button: Convert GLSL→HLSL
#define IDC_MW_SHIMPORT_APPLY        9107  // Button: Apply (live preview)
#define IDC_MW_SHIMPORT_SAVE         9108  // Button: Save as .milk3
#define IDC_MW_SHIMPORT_PASS_LIST    9113  // ListBox: pass selection
#define IDC_MW_SHIMPORT_ADD_PASS     9114  // Button: +
#define IDC_MW_SHIMPORT_DEL_PASS     9115  // Button: -
#define IDC_MW_SHIMPORT_SAVE_IMPORT  9116  // Button: Save Import...
#define IDC_MW_SHIMPORT_LOAD_IMPORT  9117  // Button: Load Import...
#define IDC_MW_SHIMPORT_NEW          9118  // Button: New
#define IDC_MW_SHIMPORT_COPY_STATUS  9119  // Button: Copy (errors/status to clipboard)
// Shader Editor window (9120-9129)
#define IDC_MW_SHEDITOR_PIN          9120
#define IDC_MW_SHEDITOR_FONT_PLUS    9121
#define IDC_MW_SHEDITOR_FONT_MINUS   9122
#define IDC_MW_SHEDITOR_GLSL_EDIT    9123  // Multiline EDIT: GLSL source
#define IDC_MW_SHEDITOR_PASTE        9124  // Button: Paste GLSL
#define IDC_MW_SHEDITOR_CLEAR        9125  // Button: Clear
#define IDC_MW_SHEDITOR_HLSL_EDIT    9126  // Multiline EDIT: HLSL (editable)
#define IDC_MW_SHEDITOR_COPY         9127  // Button: Copy HLSL
#define IDC_MW_SHEDITOR_CONVERT      9128  // Button: Convert & Apply
#define IDC_MW_SHEDITOR_NOTES_EDIT   9129  // Multiline EDIT: notes/comments

// Shader Import channel input combos (9140-9143)
#define IDC_MW_SHIMPORT_CH0          9140  // Combo: iChannel0 source
#define IDC_MW_SHIMPORT_CH1          9141  // Combo: iChannel1 source
#define IDC_MW_SHIMPORT_CH2          9142  // Combo: iChannel2 source
#define IDC_MW_SHIMPORT_CH3          9143  // Combo: iChannel3 source

// Welcome window (no-presets prompt)
#define IDC_MW_WELCOME_SETTINGS      9130  // Button: Open Settings
#define IDC_MW_WELCOME_SHADER_IMPORT 9131  // Button: Open Shader Import
#define IDC_MW_WELCOME_PRESETS       9132  // Button: Open Preset Browser
#define IDC_MW_WELCOME_BROWSE        9133  // Button: Browse for Resources
#define IDC_MW_WELCOME_PATH          9134  // Static: current path display

// Video Effects window (9200-9280)
#define IDC_MW_VFX_PIN            9200
#define IDC_MW_VFX_FONT_PLUS      9201
#define IDC_MW_VFX_FONT_MINUS     9202
#define IDC_MW_VFX_TAB            9203
// Transform tab
#define IDC_MW_VFX_POSX           9210
#define IDC_MW_VFX_POSX_LBL      9211
#define IDC_MW_VFX_POSY           9212
#define IDC_MW_VFX_POSY_LBL      9213
#define IDC_MW_VFX_SCALE          9214
#define IDC_MW_VFX_SCALE_LBL     9215
#define IDC_MW_VFX_ROTATION       9216
#define IDC_MW_VFX_ROTATION_LBL  9217
#define IDC_MW_VFX_MIRRORH        9218
#define IDC_MW_VFX_MIRRORV        9219
#define IDC_MW_VFX_BLENDMODE      9220
#define IDC_MW_VFX_RESET_XFORM    9221
// Effects tab
#define IDC_MW_VFX_TINTR          9230
#define IDC_MW_VFX_TINTR_LBL     9231
#define IDC_MW_VFX_TINTG          9232
#define IDC_MW_VFX_TINTG_LBL     9233
#define IDC_MW_VFX_TINTB          9234
#define IDC_MW_VFX_TINTB_LBL     9235
#define IDC_MW_VFX_BRIGHTNESS     9236
#define IDC_MW_VFX_BRIGHTNESS_LBL 9237
#define IDC_MW_VFX_CONTRAST       9238
#define IDC_MW_VFX_CONTRAST_LBL  9239
#define IDC_MW_VFX_SATURATION     9240
#define IDC_MW_VFX_SATURATION_LBL 9241
#define IDC_MW_VFX_HUESHIFT       9242
#define IDC_MW_VFX_HUESHIFT_LBL  9243
#define IDC_MW_VFX_INVERT         9244
#define IDC_MW_VFX_PIXELATION     9245
#define IDC_MW_VFX_PIXELATION_LBL 9246
#define IDC_MW_VFX_CHROMATIC      9247
#define IDC_MW_VFX_CHROMATIC_LBL 9248
#define IDC_MW_VFX_EDGEDETECT     9249
#define IDC_MW_VFX_RESET_EFFECTS  9250
// Audio tab
#define IDC_MW_VFX_AR_POSX_SRC    9260
#define IDC_MW_VFX_AR_POSX_INT    9261
#define IDC_MW_VFX_AR_POSY_SRC    9262
#define IDC_MW_VFX_AR_POSY_INT    9263
#define IDC_MW_VFX_AR_SCALE_SRC   9264
#define IDC_MW_VFX_AR_SCALE_INT   9265
#define IDC_MW_VFX_AR_ROT_SRC     9266
#define IDC_MW_VFX_AR_ROT_INT     9267
#define IDC_MW_VFX_AR_BRIGHT_SRC  9268
#define IDC_MW_VFX_AR_BRIGHT_INT  9269
#define IDC_MW_VFX_AR_SAT_SRC     9270
#define IDC_MW_VFX_AR_SAT_INT     9271
#define IDC_MW_VFX_AR_CHROM_SRC   9272
#define IDC_MW_VFX_AR_CHROM_INT   9273
#define IDC_MW_VFX_RESET_AUDIO    9274
// Open button on Displays window
#define IDC_MW_OPEN_VFX           9280
// Save/Load buttons on Video Effects window
#define IDC_MW_VFX_SAVE_PROFILE   9281
#define IDC_MW_VFX_LOAD_PROFILE   9282  // Opens profile picker
#define IDC_MW_VFX_SAVE_ON_EXIT   9283  // Checkbox: write the loaded profile at program exit

// VFX Profile Picker window controls (9290-9299)
#define IDC_MW_VFXP_PIN           9290
#define IDC_MW_VFXP_FONT_PLUS     9291
#define IDC_MW_VFXP_FONT_MINUS    9292
#define IDC_MW_VFXP_LIST          9293  // Listbox: profile list
#define IDC_MW_VFXP_SAVE          9294  // Button: Save As...
#define IDC_MW_VFXP_DELETE        9295  // Button: Delete
#define IDC_MW_VFXP_STARTUP       9296  // Checkbox: Load on startup
#define IDC_MW_VFXP_IMPORT        9297  // Button: Import... (older json / ini)

// Workspace Layout window (9300-9330)
#define IDC_MW_WSLAYOUT_PIN           9300
#define IDC_MW_WSLAYOUT_FONT_PLUS     9301
#define IDC_MW_WSLAYOUT_FONT_MINUS    9302
#define IDC_MW_WSLAYOUT_CORNER_TL     9303  // Radio: Top-Left
#define IDC_MW_WSLAYOUT_CORNER_TR     9304  // Radio: Top-Right
#define IDC_MW_WSLAYOUT_CORNER_BL     9305  // Radio: Bottom-Left
#define IDC_MW_WSLAYOUT_CORNER_BR     9306  // Radio: Bottom-Right
#define IDC_MW_WSLAYOUT_SIZE_SLIDER   9307  // Trackbar: render size %
#define IDC_MW_WSLAYOUT_SIZE_LABEL    9308  // Static: "20%"
#define IDC_MW_WSLAYOUT_APPLY         9309  // Button: Apply Layout
#define IDC_MW_WSLAYOUT_RESET         9310  // Button: Reset to Defaults
// Workspace Layout per-window checkboxes.
//
// Generated, not enumerated: the list is derived at runtime from the
// HKCAT_TOOLS hotkey entries that map to a tool window, so it cannot fall
// behind the way a hand-written table did -- it was missing twelve existing
// windows before this changed. 9350..9399 is reserved for them; 9311..9320
// used to hold one #define per window and no longer needs to.
#define IDC_MW_WSLAYOUT_CHK_BASE      9350
#define IDC_MW_WSLAYOUT_CHK_MAX       9399
#define IDC_MW_WSLAYOUT_MODE_CORNER   9323  // Radio: corner mode
#define IDC_MW_WSLAYOUT_MODE_DISPLAY  9324  // Radio: fullscreen on display
#define IDC_MW_WSLAYOUT_DISPLAY_COMBO 9325  // Combo: display picker

// Text Animations window (9400-9490)
#define IDC_MW_TEXTANIM_PIN           9400
#define IDC_MW_TEXTANIM_FONT_PLUS     9401
#define IDC_MW_TEXTANIM_FONT_MINUS    9402
#define IDC_MW_TEXTANIM_LIST          9403  // ListView: animation profiles
#define IDC_MW_TEXTANIM_ADD           9404
#define IDC_MW_TEXTANIM_DUPLICATE     9405
#define IDC_MW_TEXTANIM_DELETE        9406
#define IDC_MW_TEXTANIM_MOVEUP        9407
#define IDC_MW_TEXTANIM_MOVEDOWN      9408
#define IDC_MW_TEXTANIM_PREVIEW       9409
#define IDC_MW_TEXTANIM_SAVE          9410
#define IDC_MW_TEXTANIM_TEMPLATES     9411  // Load Templates button
#define IDC_MW_TEXTANIM_SONG_COMBO    9412  // Song Title Profile dropdown
#define IDC_MW_TEXTANIM_PRESET_COMBO  9413  // Preset Name Profile dropdown
#define IDC_MW_TEXTANIM_PUSH_TITLE    9414  // Push Song Title button
#define IDC_MW_TEXTANIM_NAME          9420  // Edit: profile name
#define IDC_MW_TEXTANIM_ENABLED       9421  // Checkbox: enabled in random pool
#define IDC_MW_TEXTANIM_X             9422  // Edit: target X
#define IDC_MW_TEXTANIM_Y             9423  // Edit: target Y
#define IDC_MW_TEXTANIM_RANDX         9424  // Edit: random X offset
#define IDC_MW_TEXTANIM_RANDY         9425  // Edit: random Y offset
#define IDC_MW_TEXTANIM_STARTX        9426  // Edit: entry start X
#define IDC_MW_TEXTANIM_STARTY        9427  // Edit: entry start Y
#define IDC_MW_TEXTANIM_MOVETIME      9428  // Edit: move time
#define IDC_MW_TEXTANIM_EASE_LINEAR   9429  // Radio: linear
#define IDC_MW_TEXTANIM_EASE_IN       9430  // Radio: ease-in
#define IDC_MW_TEXTANIM_EASE_OUT      9431  // Radio: ease-out
#define IDC_MW_TEXTANIM_EASE_FACTOR   9432  // Edit: ease factor
#define IDC_MW_TEXTANIM_FONTFACE      9433  // Edit: font face
#define IDC_MW_TEXTANIM_FONTSIZE      9434  // Edit: font size
#define IDC_MW_TEXTANIM_BOLD          9435  // Checkbox: bold
#define IDC_MW_TEXTANIM_ITALIC        9436  // Checkbox: italic
#define IDC_MW_TEXTANIM_COLORR        9437  // Edit: color R
#define IDC_MW_TEXTANIM_COLORG        9438  // Edit: color G
#define IDC_MW_TEXTANIM_COLORB        9439  // Edit: color B
#define IDC_MW_TEXTANIM_RANDR         9440  // Edit: random R
#define IDC_MW_TEXTANIM_RANDG         9441  // Edit: random G
#define IDC_MW_TEXTANIM_RANDB         9442  // Edit: random B
#define IDC_MW_TEXTANIM_DURATION      9443  // Edit: duration
#define IDC_MW_TEXTANIM_FADEIN        9444  // Edit: fade in
#define IDC_MW_TEXTANIM_FADEOUT       9445  // Edit: fade out
#define IDC_MW_TEXTANIM_BURNTIME      9446  // Edit: burn time
#define IDC_MW_TEXTANIM_GROWTH        9447  // Edit: growth
#define IDC_MW_TEXTANIM_SHADOW        9448  // Edit: shadow offset
#define IDC_MW_TEXTANIM_BOXALPHA      9449  // Edit: box alpha
#define IDC_MW_TEXTANIM_BOXCOLR       9450  // Edit: box color R
#define IDC_MW_TEXTANIM_BOXCOLG       9451  // Edit: box color G
#define IDC_MW_TEXTANIM_BOXCOLB       9452  // Edit: box color B
#define IDC_MW_TEXTANIM_RAND_POS      9453  // Checkbox: randomize position
#define IDC_MW_TEXTANIM_RAND_SIZE     9454  // Checkbox: randomize size
#define IDC_MW_TEXTANIM_RAND_COLOR    9455  // Checkbox: randomize color
#define IDC_MW_TEXTANIM_RAND_GROWTH   9456  // Checkbox: randomize growth
#define IDC_MW_TEXTANIM_RAND_DURATION 9457  // Checkbox: randomize duration
#define IDC_MW_OPEN_TEXTANIM          9458  // Launch button on Settings Tools tab
#define IDC_MW_TEXTANIM_CHOOSE_FONT   9459  // "Font..." button → ChooseFont dialog
#define IDC_MW_TEXTANIM_CHOOSE_COLOR  9460  // "Color..." button → ChooseColor dialog
#define IDC_MW_TEXTANIM_CHOOSE_BOXCOL 9461  // "Box Color..." button → ChooseColor dialog
#define IDC_MW_TEXTANIM_COLOR_SWATCH  9462  // Static: text color preview swatch
#define IDC_MW_TEXTANIM_BOXCOL_SWATCH 9463  // Static: box color preview swatch
#define IDC_MW_TEXTANIM_FONT_PREVIEW  9464  // Static: font name/style preview
#define IDC_MW_TEXTANIM_EXPORT        9465  // Export profiles to .ini file
#define IDC_MW_TEXTANIM_IMPORT        9466  // Import profiles from .ini file
#define IDC_MW_TEXTANIM_PREVIEW_TEXT  9467  // Edit: preview message text

// Custom message for animation preview
#define WM_MW_PUSH_ANIM_PREVIEW       (WM_APP + 25)

// Welcome window — workspace layout button
#define IDC_MW_WELCOME_LAYOUT         9135

// Settings About tab — workspace layout button
#define IDC_MW_OPEN_WORKSPACE_LAYOUT  9136

// Settings About tab — error display settings button
#define IDC_MW_ERROR_DISPLAY_SETTINGS 9137  // Error Duration edit on About tab

// Script window controls (9500-9519)
#define IDC_MW_SCRIPTWIN_PIN          9500
#define IDC_MW_SCRIPTWIN_FONT_PLUS    9501
#define IDC_MW_SCRIPTWIN_FONT_MINUS   9502

// Remote window controls (9520-9539)
#define IDC_MW_REMOTEWIN_PIN          9520
#define IDC_MW_REMOTEWIN_FONT_PLUS    9521
#define IDC_MW_REMOTEWIN_FONT_MINUS   9522
#define IDC_MW_REMOTE_TAB            9523  // Tab control
#define IDC_MW_TCP_STATUS            9524  // Label: TCP status
#define IDC_MW_TCP_PIN_EDIT          9525  // Edit: PIN
#define IDC_MW_TCP_PIN_APPLY         9526  // Button: Apply PIN
#define IDC_MW_TCP_DEVICE_LIST       9527  // ListView: connected clients
#define IDC_MW_TCP_DEVICE_REMOVE     9528  // Button: Disconnect client
#define IDC_MW_TCP_ENABLED           9529  // Checkbox: Enable TCP server
#define IDC_MW_TCP_PORT_EDIT         9530  // Edit: TCP port
#define IDC_MW_TCP_STARTSTOP         9531  // Button: Start/Stop TCP server

// Visual window controls (9540-9559)
#define IDC_MW_VISUALWIN_PIN          9540
#define IDC_MW_VISUALWIN_FONT_PLUS    9541
#define IDC_MW_VISUALWIN_FONT_MINUS   9542

// Colors window controls (9560-9579)
#define IDC_MW_COLORSWIN_PIN          9560
#define IDC_MW_COLORSWIN_FONT_PLUS    9561
#define IDC_MW_COLORSWIN_FONT_MINUS   9562

// Controller window controls (9580-9599)
#define IDC_MW_CONTROLLERWIN_PIN       9580
#define IDC_MW_CONTROLLERWIN_FONT_PLUS 9581
#define IDC_MW_CONTROLLERWIN_FONT_MINUS 9582

// Launcher buttons on Settings Tools tab (for Script / Remote / Visual / Colors windows)
#define IDC_MW_OPEN_SCRIPT            9085
#define IDC_MW_OPEN_REMOTE            9086
#define IDC_MW_OPEN_VISUAL            9087
#define IDC_MW_OPEN_COLORS            9088

// Launcher buttons on Settings General tab
#define IDC_MW_OPEN_PRESETS       9080
#define IDC_MW_OPEN_SPRITES       9081
#define IDC_MW_OPEN_MESSAGES      9082
#define IDC_MW_OPEN_BOARD         9083
#define IDC_MW_OPEN_SHIMPORT      9084
#define IDC_MW_TOOLS_LIST         9090  // ListView on Tools tab

// Preset annotation context menu (9600-9619)
#define IDC_MW_ANNOT_FAV          9600  // Toggle Favorite
#define IDC_MW_ANNOT_SKIP         9601  // Toggle Skip
#define IDC_MW_ANNOT_BROKEN       9602  // Flag as Broken
#define IDC_MW_ANNOT_NOTE         9603  // Add Note...
#define IDC_MW_ANNOT_VIEWERR      9604  // View Error
#define IDC_MW_ANNOT_CLEAR        9605  // Clear All Flags
#define IDC_MW_ANNOT_RATE_BASE    9610  // 9610-9615 for ratings 0-5
#define IDC_MW_ANNOT_CANVAS_CLEAR 9616  // Clear canvas limit

// ── Canvas limit submenu (9800-9809) ──
// Not in the 9600 block: 9611-9615 are consumed implicitly by
// IDC_MW_ANNOT_RATE_BASE without ever being #defined, leaving too few free.
#define IDC_MW_ANNOT_CANVAS_BASE  9800  // 9800-9805 for the six choices

// ── Feedback damp submenu (9810-9815) ──
// The other runaway mitigation: keeps the canvas at full size and takes the
// energy out of the feedback loop instead. Deliberately a separate submenu
// rather than more entries under Canvas Limit -- they are independent, and
// which one looks better on a given preset is the user's call.
#define IDC_MW_ANNOT_DAMP_BASE    9810  // one per Engine::kDampChoices entry
#define IDC_MW_ANNOT_DAMP_CLEAR   9815  // Clear feedback damp
// The gap between BASE and CLEAR is the number of choices that fit. Adding a
// sixth to Engine::kDampChoices without moving CLEAR would make "Maximum" and
// "Clear feedback damp" the same command id -- picking Maximum would clear the
// damp. Asserted in engine.cpp, where Engine is a complete type; this header is
// included in places that do not see it.
#define IDC_MW_ANNOT_DAMPSTR      9816  // per-preset feedback damp combo
                                       // (not in the 9660s: 9670 starts the
                                       //  Preset Editor block)

// Annotations ToolWindow controls (9620-9639)
#define IDC_MW_ANNOTWIN_PIN          9620
#define IDC_MW_ANNOTWIN_FONT_PLUS    9621
#define IDC_MW_ANNOTWIN_FONT_MINUS   9622
#define IDC_MW_ANNOTWIN_LIST         9623  // ListView: annotations
#define IDC_MW_ANNOTWIN_FILTER       9624  // ComboBox: filter (All/Favorite/Error/Skip/Broken)
#define IDC_MW_ANNOTWIN_NOTE_EDIT    9625  // Multiline EDIT: notes (read-only)
#define IDC_MW_ANNOTWIN_ERROR_EDIT   9626  // Multiline EDIT: error text (read-only)
#define IDC_MW_ANNOTWIN_LOAD         9627  // Button: Load selected preset
#define IDC_MW_ANNOTWIN_REMOVE       9628  // Button: Remove annotation
#define IDC_MW_OPEN_ANNOTATIONS      9629  // Launcher button on Tools tab
#define IDC_MW_ANNOTWIN_DETAILS      9630  // Button: View details (notes/error)
#define IDC_MW_ANNOTWIN_IMPORT       9631  // Button: Import from file
#define IDC_MW_IMPORT_LIST           9632  // ListView in import dialog
#define IDC_MW_IMPORT_ALL            9633  // Button: Import All
#define IDC_MW_IMPORT_SEL            9634  // Button: Import Selected
#define IDC_MW_IMPORT_MERGE          9635  // Button: Merge All
#define IDC_MW_ANNOTWIN_SCAN         9636  // Button: Scan presets for ratings
#define IDC_MW_ANNOTWIN_SEARCH       9637  // Edit: live search box
#define IDC_MW_ANNOTWIN_REMOVEMISSING 9638 // Remove annotations whose file is gone
#define IDC_MW_ANNOTWIN_RESETUSE     9639  // Button: Reset play counts / times

// ── Annotations ToolWindow, second block (9810-9839) ──
// The 9620-9639 block above filled up, and the overflow was not noticed:
// RESETUSE was defined as 9637 (already SEARCH) and COPYERR as 9638 (already
// REMOVEMISSING), putting two children under one parent on one id. Nothing
// broke only because those pairs are told apart by notification code -- an edit
// EN_CHANGE against a button BN_CLICKED. That is luck, not design, so the
// duplicates are resolved here and everything new goes in a block with room.
#define IDC_MW_ANNOTWIN_COPYERR      9810  // Button: Copy error (details dialog)
#define IDC_MW_ANNOTWIN_SEARCHCLEAR  9811  // Button: clear the live search box
#define IDC_MW_ANNOTWIN_FINDCOPIES   9812  // Button: hash-scan the tree for duplicate files
#define IDC_MW_ANNOTWIN_DUPES_LIST   9813  // ListView in the duplicates report dialog
#define IDC_MW_ANNOTWIN_DUPES_DELETE 9814  // Button: recycle the ticked duplicate files
#define IDC_MW_ANNOTWIN_DUPES_NEWEST 9815  // Button: tick all but the newest in each group
#define IDC_MW_ANNOTWIN_DUPES_OUTSIDE 9816 // Button: tick every copy outside the preset dir
#define IDC_MW_ANNOTWIN_DUPES_NONE   9817  // Button: untick everything
#define IDC_MW_ANNOTWIN_DUPES_COPY   9818  // Button: copy the whole report to the clipboard

// Replacement preset (issue 120): the file that runs in place of the selected
// one. 9820-9839 was empty; the 9810s are already double-booked twice over
// (COPYERR==DAMP_BASE, DUPES_NEWEST==DAMP_CLEAR) and nothing new should join them.
#define IDC_MW_ANNOT_REPL_SET        9820  // Menu: designate a replacement file...
#define IDC_MW_ANNOT_REPL_COPY       9821  // Menu: copy this preset to become one...
#define IDC_MW_ANNOT_REPL_EDIT       9822  // Menu: open the replacement in the editor
#define IDC_MW_ANNOT_REPL_CLEAR      9823  // Menu: drop the designation

// ── Audio Mixer ToolWindow (9900-9969) ──
//
// The two BASE ids are ranges, not single controls: the fader list is built
// from whatever the providers publish, so the count is not known until the
// window is drawn. Slider id minus FADER_BASE indexes the window's own fader
// table, which is how a WM_HSCROLL finds the channel it belongs to.
#define IDC_MW_MIXERWIN_PIN           9900
#define IDC_MW_MIXERWIN_FONT_PLUS     9901
#define IDC_MW_MIXERWIN_FONT_MINUS    9902
#define IDC_MW_MIXER_TAB              9903
#define IDC_MW_MIXER_ENABLE           9904  // Check: run the mixer subsystem
#define IDC_MW_MIXER_RESTART          9905  // Button: restart SteelSeries
#define IDC_MW_MIXER_REFRESH          9906  // Button: re-read the providers now
// 9907 and 9908 were the two "hotkey slot" combos. The slots are gone -- the
// volume hotkeys move a ticked GROUP now -- and the ids are left retired
// rather than reused, so a stale WM_COMMAND cannot land on a new control.
#define IDC_MW_MIXER_STEP             9909  // Edit: hotkey step, percent
#define IDC_MW_MIXER_CONFIRM_HOTKEY   9910  // Edit: hotkey confirm window, seconds
#define IDC_MW_MIXER_CONFIRM_WINDOW   9911  // Check: confirm in this window
#define IDC_MW_MIXER_ROUTE            9912  // Combo: which route the tab edits
#define IDC_MW_MIXER_ARMED            9913  // Check: failover armed for that route
#define IDC_MW_MIXER_ALLOW_LIST       9914  // List: the ordered allowlist
#define IDC_MW_MIXER_DEVICE_COMBO     9915  // Combo: devices available to add
#define IDC_MW_MIXER_ALLOW_ADD        9916
#define IDC_MW_MIXER_ALLOW_REMOVE     9917
#define IDC_MW_MIXER_STABLE_SECS      9918  // Edit: seconds a replacement must be steady
#define IDC_MW_MIXER_DWELL_SECS       9919  // Edit: minimum seconds between switches
#define IDC_MW_MIXER_ORDER_LIST       9920  // List: fader display order
#define IDC_MW_MIXER_ORDER_UP         9921
#define IDC_MW_MIXER_ORDER_DOWN       9922
#define IDC_MW_MIXER_SORT_UNMUTED     9923  // Check: float unmuted channels up
#define IDC_MW_MIXER_SPIN             9924  // Shared by every up-down; the
                                          // buddy edit carries the identity
#define IDC_MW_MIXER_STYLE_SLIDER     9925  // Radio: drag a slider
#define IDC_MW_MIXER_STYLE_SPIN       9926  // Radio: type, or step by 1
#define IDC_MW_MIXER_VIS_INPUT        9927  // Button: jump to the audio
                                          // device the visualiser listens on
#define IDC_MW_MIXER_ALLOW_NAME       9928  // Edit: short name for the selected
                                          // preferred device
#define IDC_MW_MIXER_ALLOW_SETNAME    9929
#define IDC_MW_MIXER_RESET_SONAR      9996  // Button: kill the SteelSeries GUI
                                          // layers and let them come back
#define IDC_MW_MIXER_PIN_FAILOVER     9995  // Check: hold failover devices at
                                          // the top while they are connected

// The Profiles tab. A fresh block rather than 9996-9999, which is four ids and
// this needs six -- and a run that has to jump a gap is worse than one that
// starts somewhere clear. 10000-10007 are timers and 10100+ is spoken for.
#define IDC_MW_MIXER_PROF_LIST        10040  // List: saved mixer profiles
#define IDC_MW_MIXER_PROF_NAME        10041  // Edit: name for the next save
#define IDC_MW_MIXER_PROF_SAVE        10042  // Button: save levels + layout
#define IDC_MW_MIXER_PROF_SAVEALL     10043  // Button: save those AND the rest
#define IDC_MW_MIXER_PROF_LOAD        10044  // Button: apply the selection
#define IDC_MW_MIXER_PROF_DELETE      10045  // Button: delete the selection
#define IDC_MW_MIXER_FADER_BASE       9930  // Sliders, one per fader (9930-9949)
// Was a tick box per row. The speaker glyph is the mute control now -- it
// already said muted or not, in red or green, so the box repeated it. The
// range stays reserved rather than reused: nothing else should take ids a
// saved layout or an old build may still refer to.
// Per-fader preferences for the row selected in the order list (forgejo#50,
// forgejo#52). They sit in the same group as Move Up/Down deliberately: that
// list is already "the settings for the fader you picked", and a second group
// beside it would split one idea across two places.
//
// MOVED out of 9931-9934, which was NOT a gap.
//
// The note that put them there read "in the 9931-9949 gap, which is clear:
// 9921-9930 are the allowlist's and the retired mute run starts at 9950" -- and
// it treated 9930 as one id. It is not: IDC_MW_MIXER_FADER_BASE is 9930 and
// owns a run of IDC_MW_MIXER_MAX_FADERS (20), so it covers 9930-9949 exactly.
// These four landed on faders 1, 2, 3 and 4.
//
// It was not cosmetic. GetDlgItem became ambiguous, SetDlgItemTextW wrote
// "Show fader" into the second fader's spin box for anyone to read, and -- the
// real damage -- a WM_COMMAND from that spin box arrived as
// IDC_MW_MIXER_HIDE_TOGGLE, whose handler calls RebuildFonts() and destroys the
// control that is still sending the notification. That is a crash, and it is
// what took the mixer down.
#define IDC_MW_MIXER_HIDE_TOGGLE     10162  // Button: hide / show the selected
                                            // fader on both surfaces
#define IDC_MW_MIXER_FADER_NAME      10163  // Edit: its short name
#define IDC_MW_MIXER_FADER_SETNAME   10164  // Button: apply the short name.
                                            // Empty clears it.
#define IDC_MW_MIXER_SHOW_HIDDEN     10165  // Check: draw hidden faders anyway,
                                            // which is the way back from
                                            // hiding one

// #define IDC_MW_MIXER_MUTE_BASE     9950  // retired (9950-9969)
#define IDC_MW_MIXER_ICON_BASE        9970  // Muted-speaker indicators (9970-9989)
#define IDC_MW_MIXER_ALLOW_REFRESH    9990  // Button: re-read the devices now
#define IDC_MW_MIXER_ALLOW_SORT      9997  // Combo: how the allowlist is
                                           // ordered on screen (forgejo#69)
#define IDC_MW_MIXER_ORDER_GROUP     9994  // Check: Move Up/Down takes the
                                          // whole channel, not one fader
#define IDC_MW_MIXER_SHOW_VIRTUAL    9993  // Check: show the endpoints a
                                          // provider owns (forgejo#51)
#define IDC_MW_MIXER_ALLOW_UP         9991  // Buttons: reorder the allowlist.
#define IDC_MW_MIXER_ALLOW_DOWN       9992  // The order IS the preference --
                                          // first present entry wins.
// Coarse steps beside the spin box. Kept clear of the 10001+ IDT_* timer
// ids: a different namespace, but not one worth having to remember while
// reading a control id.
#define IDC_MW_MIXER_NUDGE_DN_BASE   10100 // "-10" per fader (10100-10119)
#define IDC_MW_MIXER_NUDGE_UP_BASE   10120 // "+10" per fader (10120-10139)
// Battery per fader on the Mixer tab (10140-10159). A fresh band rather than
// the retired 9950-9969: that range stays reserved, not reused.
#define IDC_MW_MIXER_BATT_BASE       10140
// The Mixer tab's row context menu. Menu ids, not controls -- they never reach
// GetDlgItem, but they share the WM_COMMAND space so they still need to be unique.
#define IDC_MW_MIXER_CTX_HIDE        10160 // "Hide this channel"
#define IDC_MW_MIXER_CTX_SHOWHIDDEN  10161 // "Show hidden channels" (toggle)

// Resize-to-fit, on EVERY tool window, beside the pin. One id rather than one
// per window like the pin has: a control id only has to be unique within its
// parent, and every tool window is its own top-level window. Clear of the
// mixer's 10100-10139 run.
#define IDC_MW_TOOLWIN_FIT            10200 // Button: resize a tool
                                          // window to fit its contents
#define IDC_MW_MIXER_MAX_FADERS       20


// ── Custom Shaders window ──
#define IDC_MW_CSHADER_PIN           9640
#define IDC_MW_CSHADER_FONT_PLUS     9641
#define IDC_MW_CSHADER_FONT_MINUS    9642
#define IDC_MW_CSHADER_LIST          9643  // Listbox: overrides
#define IDC_MW_CSHADER_RULES         9644  // Listbox: tag rules, ORDER IS PRIORITY
#define IDC_MW_CSHADER_ENABLE        9645  // Checkbox: master enable
#define IDC_MW_CSHADER_NEW           9646
#define IDC_MW_CSHADER_IMPORTHLSL    9647  // Import a .hlsl/.fx as warp or comp
#define IDC_MW_CSHADER_IMPORTMILK    9648  // Lift warp_/comp_ blocks out of a .milk
#define IDC_MW_CSHADER_RENAME        9649
#define IDC_MW_CSHADER_DELETE        9650
#define IDC_MW_CSHADER_EDITWARP      9651  // Open the .hlsl in the default editor
#define IDC_MW_CSHADER_EDITCOMP      9652
#define IDC_MW_CSHADER_RELOAD        9653  // Re-read files, recompile
#define IDC_MW_CSHADER_APPLY         9654  // Apply to the running preset, no rule
#define IDC_MW_CSHADER_REVERT        9655
#define IDC_MW_CSHADER_RULEADD       9656
#define IDC_MW_CSHADER_RULEDEL       9657
#define IDC_MW_CSHADER_RULEUP        9658
#define IDC_MW_CSHADER_RULEDOWN      9659
#define IDC_MW_CSHADER_RULEVFX       9665  // per-rule VFX profile combo

// Keep / Discard prompt for an edit made while a preset's VFX profile was on
#define IDC_MW_VFXPROMPT_PIN         9660
#define IDC_MW_VFXPROMPT_FONT_PLUS   9661
#define IDC_MW_VFXPROMPT_FONT_MINUS  9662
#define IDC_MW_VFXPROMPT_KEEP        9663
#define IDC_MW_VFXPROMPT_DISCARD     9664

// Does this IPC message start with this command literal?
//
// The length comes from the literal, so it cannot disagree with it. _countof
// refuses to compile on a pointer, so `MSG_IS(msg, someVariable)` is a build
// error rather than a silent prefix match on sizeof(wchar_t*)/2 characters.
//
// This exists because one of 138 hand-written wcsncmp lengths was wrong:
// wcsncmp(sMessage, L"SET_MIRROR_WIPE", 14) compared fourteen of fifteen
// characters, and the argument parsing beneath it indexed from 14 too, so
// SET_MIRROR_WIPE=reinit silently did nothing for as long as it has existed.
#define MSG_IS(msg, lit)  (wcsncmp((msg), (lit), _countof(lit) - 1) == 0)

// True when nothing but whitespace follows. A bare command should still match
// when a script or a remote's text box leaves a trailing space or newline on
// it, so this is not a bare wcscmp.
inline bool MsgTailBlank(const wchar_t* p) {
  while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') ++p;
  return *p == L'\0';
}

// Is this IPC message this command and NOTHING else?
//
// Use this for a command that takes no argument, whenever any LONGER command
// starts with the same text. MSG_IS is a prefix match, so a bare command
// dispatched first swallows every longer one beginning with it -- and because
// the body then returns, the longer one is not merely mis-parsed, it is
// unreachable.
//
// `DIAG_GPU` did exactly that to `DIAG_GPURISK=` and `DIAG_GPUTIME=`, which
// sit ~340 lines below it in the same function. Both answered with a
// GPU-usage dump and changed nothing, for as long as they had existed: a
// switch that reports success and does nothing. It cost a day of mirror
// measurements taken with the isolation gate believed to be off, and the
// belief was only wrong because the command reported that it had worked.
//
// Same failure class as the wrong wcsncmp length that MSG_IS was introduced to
// fix, which is why it gets a macro rather than a comment asking for the
// handlers to be kept in the right order.
#define MSG_EQ(msg, lit)  (wcsncmp((msg), (lit), _countof(lit) - 1) == 0 && \
                           MsgTailBlank((msg) + _countof(lit) - 1))

// Annotations window: the per-preset override slots
#define IDC_MW_ANNOT_SHADEROV        9666
#define IDC_MW_ANNOT_VFXPROF         9667
#define IDC_MW_ANNOT_AUDIOPROF       9668  // per-preset audio profile combo
#define IDC_MW_ANNOT_CANVASMAX       9669  // per-preset canvas limit combo

// ── Preset Editor window (9670-9689) ──
#define IDC_MW_PEDIT_PIN          9670
#define IDC_MW_PEDIT_FONT_PLUS    9671
#define IDC_MW_PEDIT_FONT_MINUS   9672
#define IDC_MW_PEDIT_SECTIONS     9673  // Listbox: code sections
#define IDC_MW_PEDIT_SCI          9674  // Scintilla editor
#define IDC_MW_PEDIT_APPLY        9675  // Apply the current section to the running preset
#define IDC_MW_PEDIT_APPLYALL     9676  // Apply every modified section
#define IDC_MW_PEDIT_REVERT       9677  // Reload the current section from CState
#define IDC_MW_PEDIT_SAVE         9678  // Write the preset back to its own file
#define IDC_MW_PEDIT_SAVEAS       9679  // Write to a new file
#define IDC_MW_PEDIT_STATUS       9680  // Read-only status / first compile error
#define IDC_MW_PEDIT_EXPAND       9685  // Break statements onto separate lines
#define IDC_MW_PEDIT_SIDETABS     9696  // Preset 1 / Preset 2 tabs (.milk2 only)
// Editor context menu (9686-9699)
#define IDC_MW_PEDIT_OPT_LINENUMS 9686  // Options > Line Numbers (toggle)
#define IDC_MW_PEDIT_OPT_FOLDING  9687  // Options > Code Folding (toggle)
#define IDC_MW_PEDIT_FOLDALL      9688  // Options > Fold All
#define IDC_MW_PEDIT_UNFOLDALL    9689  // Options > Unfold All
#define IDC_MW_PEDIT_UNDO         9690
#define IDC_MW_PEDIT_REDO         9691
#define IDC_MW_PEDIT_CUT          9692
#define IDC_MW_PEDIT_COPY         9693
#define IDC_MW_PEDIT_PASTE        9694
#define IDC_MW_PEDIT_SELECTALL    9695
// Templates submenu: a contiguous block, dispatched by range not by switch case
#define IDC_MW_PEDIT_TPL_BASE     9700  // 9700-9799
// 9681 free -- the Settings Tools tab needs no id of its own: it populates from
// m_hotkeys[] filtered to HKCAT_TOOLS, so HK_OPEN_PRESET_EDITOR lists itself.
#define IDC_MW_PRESET_EDIT        9682  // "Edit" button on the Presets window
#define IDC_MW_ANNOTWIN_EDIT      9683  // "Edit" button on the Annotations window
#define IDC_MW_ANNOT_EDITPRESET   9684  // "Edit Preset..." context-menu item

// Settings tab page indices
#define SP_GENERAL  0
#define SP_TOOLS    1
#define SP_SYSTEM   2
#define SP_FILES    3
#define SP_ABOUT    4
#define SETTINGS_NUM_PAGES  5

// Custom messages for thread-safe side effects (settings thread → render thread)
#define WM_MW_SET_OPACITY       (WM_APP + 1)
#define WM_MW_SET_ALWAYS_ON_TOP (WM_APP + 2)
#define WM_MW_TOGGLE_SPOUT      (WM_APP + 3)
#define WM_MW_RESET_BUFFERS     (WM_APP + 4)
#define WM_MW_SPOUT_FIXEDSIZE   (WM_APP + 5)
#define WM_MW_PUSH_MESSAGE      (WM_APP + 6)
#define WM_MW_PRESET_CHANGED    (WM_APP + 7)
#define WM_MW_RESTART_DEVICE    (WM_APP + 8)
#define WM_MW_IPC_MESSAGE       (WM_APP + 9)  // lParam = heap-allocated wchar_t* from IPC thread
// WM_APP + 10 was WM_MW_RESTART_IPC — removed (pipe server uses PID-based naming)
#define WM_MW_PUSH_SPRITE       (WM_APP + 11) // wParam = spriteNum, lParam = slot (-1=auto)
#define WM_MW_KILL_SPRITE       (WM_APP + 12) // wParam = slot index
#define WM_MW_TOGGLE_DISPLAY    (WM_APP + 13) // wParam = output index in m_displayOutputs
#define WM_MW_REFRESH_DISPLAYS  (WM_APP + 14) // re-enumerate monitors
#define WM_MW_REGISTER_HOTKEYS  (WM_APP + 15) // re-register global hotkeys on render thread
#define WM_MW_IDLE_ACTIVATE     (WM_APP + 16) // idle timer triggered — go fullscreen or activate mirrors
#define WM_MW_IDLE_RESTORE      (WM_APP + 17) // user became active — restore previous state
#define WM_MW_TOGGLE_STRETCH_MODE (WM_APP + 18) // toggle stretch across all monitors
#define WM_MW_TOGGLE_MIRROR_MODE  (WM_APP + 19) // toggle per-output mirror windows
#define WM_MW_RESET_WINDOW        (WM_APP + 20) // reset to safe windowed mode (Ctrl+F2)
#define WM_MW_REBUILD_FONTS       (WM_APP + 21) // cross-window font size sync
#define WM_MW_HOTKEY_ACTION       (WM_APP + 22) // wParam = HotkeyAction ID (dispatch to App.cpp)
#define WM_MW_MIDI_DATA           (WM_APP + 23) // lParam = packed MIDI bytes from MidiInput callback
#define WM_MW_NO_PRESETS_PROMPT   (WM_APP + 24) // show "no presets" dialog on UI thread
#define WM_MW_BRING_TO_TOP        (WM_APP + 26) // raise tool window above fullscreen render
#define WM_MW_SHOW_TAB            (WM_APP + 171) // select a tool window page, on its own thread
#define WM_MW_SHOW_CTRL           (WM_APP + 172) // reveal and focus one control
#define WM_MW_CLICK_CTRL          (WM_APP + 173) // press one control
#define WM_MW_MOVE_TO             (WM_APP + 174) // reposition, on the window's thread
#define WM_MW_CHILD_REVEAL        (WM_APP + 175) // a child has a frame; show it
#define WM_MW_CHILD_KEY           (WM_APP + 176) // key pressed on a child; wParam=vk lParam=mods
#define WM_MW_RESET_PIPELINE      (WM_APP + 27) // recreate root signature + all PSOs (aniso toggle)
#define WM_MW_GPU_FATAL           (WM_APP + 28) // TDR / GPU hung — UI thread warns and exits
#define WM_MW_VFX_DIRTY_CHANGED   (WM_APP + 29) // VideoFX params changed -- repaint the Save button
#define WM_MW_MSGMODE_CHANGED     (WM_APP + 30) // Messages/Sprites enable bits changed -- resync tool window controls
#define WM_MW_SET_FPS_CAP         (WM_APP + 31) // wParam = fps cap (0 = unlimited); SetFPSCap must run on the render thread
#define WM_MW_RESOURCES_READY     (WM_APP + 32) // render thread has rebuilt the Resource Viewer snapshot (#352)

// Milkwave Remote messages (sent via PostMessage from Milkwave Remote → IPC window → render window)
#define WM_MW_NEXT_PRESET       (WM_APP + 100)
#define WM_MW_PREV_PRESET       (WM_APP + 101)
#define WM_MW_COVER_CHANGED     (WM_APP + 102)
#define WM_MW_SPRITE_MODE       (WM_APP + 103)
#define WM_MW_MESSAGE_MODE      (WM_APP + 104)
#define WM_MW_CAPTURE           (WM_APP + 105)
#define WM_MW_SETVIDEODEVICE    (WM_APP + 106)
#define WM_MW_ENABLEVIDEOMIX    (WM_APP + 107)
#define WM_MW_SETSPOUTSENDER    (WM_APP + 108)
#define WM_MW_ENABLESPOUTMIX    (WM_APP + 109)
#define WM_MW_SHOW_COVER        (WM_APP + 110)
#define WM_MW_SET_INPUTMIX_OPACITY  (WM_APP + 150)
#define WM_MW_SET_INPUTMIX_LUMAKEY  (WM_APP + 151)
#define WM_MW_SET_INPUTMIX_ONTOP    (WM_APP + 152)
#define WM_MW_FULLSCREEN            (WM_APP + 160) // SIGNAL|FULLSCREEN
#define WM_MW_WATERMARK             (WM_APP + 161) // SIGNAL|WATERMARK
#define WM_MW_BORDERLESS_FS         (WM_APP + 162) // SIGNAL|BORDERLESS_FS
#define WM_MW_STRETCH               (WM_APP + 163) // SIGNAL|STRETCH
#define WM_MW_MIRROR                (WM_APP + 164) // SIGNAL|MIRROR
#define WM_MW_MIRROR_WM             (WM_APP + 165) // SIGNAL|MIRROR_WM
#define WM_MW_RAISE_PRIMARY         (WM_APP + 166) // render thread asks UI thread to raise primary
#define WM_MW_MIRROR_INDEPENDENT    (WM_APP + 167) // SIGNAL|MIRROR_INDEPENDENT — toggle independent mirror render
#define WM_MW_ALWAYS_ON_TOP_TOGGLE  (WM_APP + 168) // SIGNAL|ALWAYS_ON_TOP — toggle always-on-top
#define WM_MW_FOCUS_PRIMARY         (WM_APP + 169) // mirror click → UI thread SetForeground/SetFocus primary
#define WM_MW_RAISE_WINDOW          (WM_APP + 170) // RAISE_WINDOW — to the top of the z-order, without taking focus
#define WM_MW_LOWER_WINDOW          (WM_APP + 178) // LOWER_WINDOW — to the bottom, without taking focus (#237)
#define WM_MW_SET_MINIMIZED         (WM_APP + 177) // SET_MINIMIZED=<0|1> — wParam: 1 minimise, 0 restore
