#pragma once
#include <string>
#include <vector>
#include <Windows.h>

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

inline void FormatSpriteSectionA(char* buf, int bufSize, int index) {
  if (index < 100)
    sprintf_s(buf, bufSize, "img%02d", index);
  else
    sprintf_s(buf, bufSize, "img%d", index);
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
HWND CreateCheck(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool visible = true);
HWND CreateRadio(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool firstInGroup = false, bool visible = true, int radioGroup = 0);
HWND CreateBtn(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool visible = true);
HWND CreateSlider(HWND hParent, int id, int x, int y, int w, int h, int rangeMin, int rangeMax, int pos, bool visible = true);
void DrawOwnerCheckbox(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg, COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText);
void DrawOwnerRadio(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg, COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText);
void draw3DEdge(HDC hdc, const RECT& rc, COLORREF hi, COLORREF shadow, bool raised);
void DrawOwnerButton(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBtnFace, COLORREF colBtnHi, COLORREF colBtnShadow, COLORREF colText);

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

// Remote tab control IDs
#define IDC_MW_IPC_TITLE            2120  // Edit: Window Title
#define IDC_MW_IPC_REMOTE_TITLE     2121  // Edit: Remote Window Title
#define IDC_MW_IPC_APPLY            2122  // Button: Apply & Restart IPC
#define IDC_MW_IPC_LIST             2123  // ListBox: active IPC windows
#define IDC_MW_IPC_TITLE_HINT       2125  // Label: hint for window title
#define IDC_MW_IPC_REMOTE_HINT      2126  // Label: hint for remote title
#define IDC_MW_IPC_MSG_GROUP        2127  // GroupBox: "Last message: HH:MM:SS"
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
#define IDC_MW_IDLE_ACTION         2153  // ComboBox: Fullscreen / Stretch-Mirror
#define IDC_MW_IDLE_AUTO_RESTORE   2154  // Checkbox: auto-restore on input

// Song Info controls (General tab)
#define IDC_MW_SONG_SOURCE         2160  // ComboBox: SMTC / IPC / Window Title
#define IDC_MW_SONG_WINDOW_TITLE   2161  // Edit: window title to scrape
#define IDC_MW_SONG_OVERLAY        2162  // Checkbox: overlay notifications
#define IDC_MW_SONG_COVER          2163  // Checkbox: show cover art sprite
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
#define IDC_MW_DISP_OPACITY     7012   // Edit: mirror opacity (1-100)
#define IDC_MW_DISP_OPACITY_SPIN 7013  // Up-down (spin) buddy for opacity
#define IDC_MW_DISP_MIRROR_ALTS 7014   // Checkbox: use mirrors for ALT-S
#define IDC_MW_DISP_MIRROR_NOPROMPT 7015 // Checkbox: don't ask when no mirrors enabled
#define IDC_MW_DISP_SAVE_PROFILE   7016 // Button: save display profile
#define IDC_MW_DISP_LOAD_PROFILE   7017 // Button: load display profile

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
#define IDC_MW_PRESETS_STARTUP      9023  // Combo: startup mode (Random/Current/Last Used)

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

// VFX Profile Picker window controls (9290-9299)
#define IDC_MW_VFXP_PIN           9290
#define IDC_MW_VFXP_FONT_PLUS     9291
#define IDC_MW_VFXP_FONT_MINUS    9292
#define IDC_MW_VFXP_LIST          9293  // Listbox: profile list
#define IDC_MW_VFXP_SAVE          9294  // Button: Save As...
#define IDC_MW_VFXP_DELETE        9295  // Button: Delete
#define IDC_MW_VFXP_STARTUP       9296  // Checkbox: Load on startup
#define IDC_MW_VFXP_SAVECLOSE     9297  // Checkbox: Save on close

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
#define IDC_MW_WSLAYOUT_CHK_SETTINGS  9311
#define IDC_MW_WSLAYOUT_CHK_HOTKEYS   9312
#define IDC_MW_WSLAYOUT_CHK_MIDI      9313
#define IDC_MW_WSLAYOUT_CHK_BOARD     9314
#define IDC_MW_WSLAYOUT_CHK_PRESETS   9315
#define IDC_MW_WSLAYOUT_CHK_DISPLAYS  9316
#define IDC_MW_WSLAYOUT_CHK_SHIMPORT  9317
#define IDC_MW_WSLAYOUT_CHK_SONGINFO  9318
#define IDC_MW_WSLAYOUT_CHK_SPRITES   9319
#define IDC_MW_WSLAYOUT_CHK_MESSAGES  9320
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

// Launcher buttons on Settings General tab
#define IDC_MW_OPEN_PRESETS       9080
#define IDC_MW_OPEN_SPRITES       9081
#define IDC_MW_OPEN_MESSAGES      9082
#define IDC_MW_OPEN_BOARD         9083
#define IDC_MW_OPEN_SHIMPORT      9084
#define IDC_MW_TOOLS_LIST         9090  // ListView on Tools tab

// Settings tab page indices
#define SP_GENERAL  0
#define SP_TOOLS    1
#define SP_VISUAL   2
#define SP_COLORS   3
#define SP_SYSTEM   4
#define SP_FILES    5
#define SP_REMOTE   6
#define SP_SCRIPT   7
#define SP_ABOUT    8
#define SETTINGS_NUM_PAGES  9

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
#define WM_MW_SET_INPUTMIX_OPACITY  (WM_APP + 150)
#define WM_MW_SET_INPUTMIX_LUMAKEY  (WM_APP + 151)
#define WM_MW_SET_INPUTMIX_ONTOP    (WM_APP + 152)
