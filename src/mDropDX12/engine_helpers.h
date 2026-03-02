#pragma once
#include <string>
#include <vector>
#include <Windows.h>

// Shared macros — used by shader and texture modules
#define IsAlphabetChar(x) ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z'))
#define IsAlphanumericChar(x) ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9') || x == '.')
#define IsNumericChar(x) (x >= '0' && x <= '9')

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
HWND CreateRadio(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool firstInGroup = false, bool visible = true);
HWND CreateBtn(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool visible = true);
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
#define IDC_MW_LOGLEVEL_WARN        2140  // Radio: Log Level Warn
#define IDC_MW_LOGLEVEL_INFO        2141  // Radio: Log Level Info
#define IDC_MW_LOGLEVEL_VERBOSE     2142  // Radio: Log Level Verbose
#define IDC_MW_MSG_SHOW_MESSAGES   2133  // Checkbox: Enable Messages (Messages tab)
#define IDC_MW_MSG_SHOW_SPRITES    2134  // Checkbox: Enable Sprites (Messages tab)
#define IDC_MW_CONTENT_BASE_LABEL  2135  // Label: Content Base Path (Files tab)
#define IDC_MW_CONTENT_BASE_EDIT   2136  // Edit: Content Base Path display (Files tab)
#define IDC_MW_CONTENT_BASE_BROWSE 2137  // Button: Browse for Content Base Path
#define IDC_MW_CONTENT_BASE_CLEAR  2138  // Button: Clear Content Base Path

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

// Settings page count
#define SETTINGS_NUM_PAGES      11

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
#define WM_MW_RESTART_IPC       (WM_APP + 10) // settings thread requests IPC window restart
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
